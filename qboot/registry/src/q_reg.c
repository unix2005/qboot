/*
 * Consul 注册 / 心跳 / 发现 / 轮询。
 *
 * Consul API（全部走 agent 的 HTTP 接口，不引 SDK）：
 *   注册      PUT /v1/agent/service/register
 *   心跳      PUT /v1/agent/check/pass/service:<ID>
 *   注销      PUT /v1/agent/service/deregister/<ID>
 *   健康实例  GET /v1/health/service/<name>?passing=1
 */

#include <q/reg.h>

#include <q/core/ds.h>
#include <q/core/time.h>
#include <q/log.h>

#include <jansson.h>

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define Q_REG_DEFAULT_TTL      10
#define Q_REG_DEFAULT_REFRESH   5
#define Q_REG_MAX_WATCH        32
#define Q_REG_MAX_ENDPOINTS    64
#define Q_REG_CALL_TRY_MAX      3

/* ---------------- 缓存条目 ---------------- */

typedef struct {
    char            name[128];
    q_endpoint_t    eps[Q_REG_MAX_ENDPOINTS];
    int             nep;
    int             last_ok;         /* 最近一次刷新是否成功 */
    int64_t         refreshed_ms;
    unsigned long   rr;              /* 轮询游标 */
} q_svc_t;

struct q_reg {
    char             consul[256];
    char             service[128];
    char             id[192];
    char             host[128];
    int              port;
    char           **tags;
    int              ntags;
    int              ttl_secs;
    int              refresh_secs;

    pthread_mutex_t  mu;
    q_hash_t        *svcs;           /* name -> q_svc_t* */

    pthread_t        hb_thr;         /* 心跳 */
    pthread_t        rf_thr;         /* 刷新 */
    int              running;
    int              registered;
    int              heartbeats;
    int              refresh_ok;
    int              refresh_fail;
};

/* ---------------- 工具 ---------------- */

static void local_ip(char *out, size_t cap)
{
    /*
     * 不做网卡枚举（跨平台太脏），约定：调用方显式传 host。
     * 这里兜底用主机名解析，失败就 127.0.0.1。
     */
    snprintf(out, cap, "127.0.0.1");
}

static void svc_free(const char *key, void *val, void *ud)
{
    (void)key; (void)ud;
    free(val);
}

/* ---------------- 生命周期 ---------------- */

q_reg_t *q_reg_new(const q_reg_cfg_t *cfg)
{
    q_reg_t *r;

    if (cfg == NULL || cfg->consul_addr == NULL) return NULL;

    r = calloc(1, sizeof(q_reg_t));
    if (r == NULL) return NULL;

    snprintf(r->consul,  sizeof(r->consul),  "%s", cfg->consul_addr);
    snprintf(r->service, sizeof(r->service), "%s", cfg->service ? cfg->service : "unknown");
    snprintf(r->host,    sizeof(r->host),    "%s",
             (cfg->host != NULL && cfg->host[0] != '\0') ? cfg->host : "");
    if (r->host[0] == '\0') local_ip(r->host, sizeof(r->host));

    r->port = cfg->port;

    if (cfg->id != NULL && cfg->id[0] != '\0') {
        snprintf(r->id, sizeof(r->id), "%s", cfg->id);
    } else {
        snprintf(r->id, sizeof(r->id), "%s-%s-%d", r->service, r->host, r->port);
    }

    r->ttl_secs     = cfg->ttl_secs     > 0 ? cfg->ttl_secs     : Q_REG_DEFAULT_TTL;
    r->refresh_secs = cfg->refresh_secs > 0 ? cfg->refresh_secs : Q_REG_DEFAULT_REFRESH;

    if (cfg->tags != NULL && cfg->ntags > 0) {
        r->tags = calloc((size_t)cfg->ntags, sizeof(char *));
        if (r->tags != NULL) {
            for (int i = 0; i < cfg->ntags; i++) {
                if (cfg->tags[i] != NULL) r->tags[i] = strdup(cfg->tags[i]);
            }
            r->ntags = cfg->ntags;
        }
    }

    pthread_mutex_init(&r->mu, NULL);
    r->svcs = q_hash_new(16);
    if (r->svcs == NULL) {
        q_reg_free(r);
        return NULL;
    }
    return r;
}

void q_reg_free(q_reg_t *r)
{
    if (r == NULL) return;

    if (r->running) q_reg_stop(r);

    for (int i = 0; i < r->ntags; i++) free(r->tags[i]);
    free(r->tags);

    if (r->svcs != NULL) {
        q_hash_foreach(r->svcs, svc_free, NULL);
        q_hash_free(r->svcs);
    }
    pthread_mutex_destroy(&r->mu);
    free(r);
}

/* ---------------- 注册 / 心跳 / 注销 ---------------- */

static char *build_register_json(const q_reg_t *r)
{
    json_t *j = json_object();
    if (j == NULL) return NULL;

    json_object_set_new(j, "ID",      json_string(r->id));
    json_object_set_new(j, "Name",    json_string(r->service));
    json_object_set_new(j, "Address", json_string(r->host));
    json_object_set_new(j, "Port",    json_integer(r->port));

    if (r->ntags > 0) {
        json_t *t = json_array();
        for (int i = 0; i < r->ntags; i++) {
            if (r->tags[i] != NULL) json_array_append_new(t, json_string(r->tags[i]));
        }
        json_object_set_new(j, "Tags", t);
    }

    char ttl[32];
    snprintf(ttl, sizeof(ttl), "%ds", r->ttl_secs);

    json_t *ck = json_object();
    json_object_set_new(ck, "TTL",    json_string(ttl));
    json_object_set_new(ck, "Status", json_string("passing"));
    /* 进程被 kill -9 时没人续期，让 Consul 自己清掉 */
    json_object_set_new(ck, "DeregisterCriticalServiceAfter", json_string("2m"));
    json_object_set_new(j, "Check", ck);

    char *s = json_dumps(j, JSON_COMPACT);
    json_decref(j);
    return s;
}

int q_reg_register(q_reg_t *r, char *err, size_t errlen)
{
    q_httpc_t     *c;
    q_httpc_resp_t resp;
    char          *body;
    char           url[512];
    int            rc;

    if (r == NULL) return Q_ERR_INVAL;

    body = build_register_json(r);
    if (body == NULL) {
        snprintf(err, errlen, "build register payload failed");
        return Q_ERR_NOMEM;
    }

    c = q_httpc_new(3000, 500);
    if (c == NULL) {
        free(body);
        snprintf(err, errlen, "http client init failed");
        return Q_ERR;
    }

    snprintf(url, sizeof(url), "http://%s/v1/agent/service/register", r->consul);
    rc = q_httpc_put(c, url, body, &resp);
    free(body);

    if (rc != Q_OK) {
        snprintf(err, errlen, "register failed: %s (http=%ld)",
                 resp.err, resp.status);
        q_httpc_resp_free(&resp);
        q_httpc_free(c);
        q_error("consul register failed: %s", err);
        return rc;
    }
    q_httpc_resp_free(&resp);
    q_httpc_free(c);

    r->registered = 1;
    q_info("registered to consul: %s -> %s (%s:%d)",
           r->consul, r->service, r->host, r->port);
    return Q_OK;
}

int q_reg_deregister(q_reg_t *r)
{
    q_httpc_t     *c;
    q_httpc_resp_t resp;
    char           url[512];
    int            rc;

    if (r == NULL || !r->registered) return Q_OK;

    c = q_httpc_new(3000, 500);
    if (c == NULL) return Q_ERR;

    snprintf(url, sizeof(url), "http://%s/v1/agent/service/deregister/%s",
             r->consul, r->id);
    rc = q_httpc_put(c, url, NULL, &resp);
    if (rc != Q_OK) {
        q_warn("consul deregister failed: %s", resp.err);
    } else {
        r->registered = 0;
        q_info("deregistered from consul: %s", r->id);
    }
    q_httpc_resp_free(&resp);
    q_httpc_free(c);
    return rc;
}

static void *heartbeat_thread(void *ud)
{
    q_reg_t *r = ud;
    q_httpc_t *c = q_httpc_new(3000, 500);
    char url[512];
    int  half = r->ttl_secs / 2;

    if (half < 1) half = 1;
    snprintf(url, sizeof(url), "http://%s/v1/agent/check/pass/service:%s",
             r->consul, r->id);

    while (r->running) {
        /* 分段睡，退出信号来了能及时响应 */
        for (int i = 0; i < half * 2 && r->running; i++) usleep(500 * 1000);

        if (!r->running) break;
        if (c == NULL) continue;

        q_httpc_resp_t resp;
        if (q_httpc_put(c, url, NULL, &resp) == Q_OK) {
            r->heartbeats++;
        } else {
            q_warn("consul heartbeat failed: %s (http=%ld)", resp.err, resp.status);
            /*
             * 连续失败可能是 agent 重启导致注册丢失，重新注册一次。
             * 这里不做复杂退避，下一次心跳会继续尝试。
             */
            if (resp.status == 404) {
                char err[256];
                q_reg_register(r, err, sizeof(err));
            }
        }
        q_httpc_resp_free(&resp);
    }
    if (c != NULL) q_httpc_free(c);
    return NULL;
}

/* ---------------- 发现 ---------------- */

static int parse_health(const char *body, q_svc_t *svc)
{
    json_error_t e;
    json_t *root = json_loads(body, 0, &e);
    if (root == NULL || !json_is_array(root)) {
        if (root != NULL) json_decref(root);
        return Q_ERR;
    }

    svc->nep = 0;
    size_t i;
    json_t *item;
    json_array_foreach(root, i, item) {
        if (svc->nep >= Q_REG_MAX_ENDPOINTS) break;

        json_t *s = json_object_get(item, "Service");
        if (s == NULL) continue;

        const char *addr = json_string_value(json_object_get(s, "Address"));
        json_t     *port = json_object_get(s, "Port");

        if ((addr == NULL || addr[0] == '\0')) {
            json_t *node = json_object_get(item, "Node");
            if (node != NULL) addr = json_string_value(json_object_get(node, "Address"));
        }
        if (addr == NULL || addr[0] == '\0' || port == NULL) continue;

        snprintf(svc->eps[svc->nep].host, sizeof(svc->eps[svc->nep].host), "%s", addr);
        svc->eps[svc->nep].port = (int)json_integer_value(port);
        svc->nep++;
    }
    json_decref(root);
    return Q_OK;
}

int q_reg_refresh(q_reg_t *r, const char *service)
{
    q_httpc_t     *c;
    q_httpc_resp_t resp;
    char           url[512];
    q_svc_t       *svc;
    int            rc;

    if (r == NULL || service == NULL) return Q_ERR_INVAL;

    c = q_httpc_new(3000, 500);
    if (c == NULL) return Q_ERR;

    snprintf(url, sizeof(url), "http://%s/v1/health/service/%s?passing=1",
             r->consul, service);
    rc = q_httpc_get(c, url, &resp);
    q_httpc_free(c);

    pthread_mutex_lock(&r->mu);
    svc = q_hash_get(r->svcs, service);
    if (svc == NULL) {
        svc = calloc(1, sizeof(q_svc_t));
        if (svc == NULL) {
            pthread_mutex_unlock(&r->mu);
            q_httpc_resp_free(&resp);
            return Q_ERR_NOMEM;
        }
        snprintf(svc->name, sizeof(svc->name), "%s", service);
        q_hash_set(r->svcs, service, svc);
    }

    if (rc != Q_OK || resp.body == NULL) {
        svc->last_ok = 0;
        r->refresh_fail++;
        pthread_mutex_unlock(&r->mu);
        q_warn("consul discover '%s' failed: %s", service,
               rc != Q_OK ? resp.err : "empty body");
        q_httpc_resp_free(&resp);
        return rc == Q_OK ? Q_ERR : rc;
    }

    if (parse_health(resp.body, svc) != Q_OK) {
        svc->last_ok = 0;
        r->refresh_fail++;
        pthread_mutex_unlock(&r->mu);
        q_httpc_resp_free(&resp);
        return Q_ERR;
    }

    svc->last_ok      = 1;
    svc->refreshed_ms = q_time_now_ms();
    r->refresh_ok++;
    pthread_mutex_unlock(&r->mu);

    q_httpc_resp_free(&resp);
    return Q_OK;
}

typedef struct {
    char names[Q_REG_MAX_WATCH][128];
    int  n;
} name_list_t;

static void collect_name(const char *key, void *val, void *ud)
{
    name_list_t *l = ud;
    (void)val;
    if (l->n >= Q_REG_MAX_WATCH) return;
    snprintf(l->names[l->n], sizeof(l->names[0]), "%s", key);
    l->n++;
}

static void *refresh_thread(void *ud)
{
    q_reg_t *r = ud;

    while (r->running) {
        /* 先把要刷的服务名拷出来再逐个刷新：不要持锁做网络 IO */
        name_list_t l;
        memset(&l, 0, sizeof(l));

        pthread_mutex_lock(&r->mu);
        q_hash_foreach(r->svcs, collect_name, &l);
        pthread_mutex_unlock(&r->mu);

        for (int i = 0; i < l.n && r->running; i++) q_reg_refresh(r, l.names[i]);

        for (int i = 0; i < r->refresh_secs * 2 && r->running; i++) usleep(500 * 1000);
    }
    return NULL;
}

/* 记录额外关注的服务：放进哈希表，刷新线程就能拿到 */
int q_reg_watch(q_reg_t *r, const char *service)
{
    q_svc_t *svc;
    if (r == NULL || service == NULL) return Q_ERR_INVAL;

    pthread_mutex_lock(&r->mu);
    svc = q_hash_get(r->svcs, service);
    if (svc == NULL) {
        svc = calloc(1, sizeof(q_svc_t));
        if (svc == NULL) {
            pthread_mutex_unlock(&r->mu);
            return Q_ERR_NOMEM;
        }
        snprintf(svc->name, sizeof(svc->name), "%s", service);
        q_hash_set(r->svcs, service, svc);
    }
    pthread_mutex_unlock(&r->mu);
    return Q_OK;
}

int q_reg_endpoints(q_reg_t *r, const char *service, q_endpoint_t *out, int cap)
{
    q_svc_t *svc;
    int      n = 0;

    if (r == NULL || service == NULL || out == NULL || cap <= 0) return 0;

    pthread_mutex_lock(&r->mu);
    svc = q_hash_get(r->svcs, service);
    if (svc != NULL) {
        n = svc->nep < cap ? svc->nep : cap;
        memcpy(out, svc->eps, sizeof(q_endpoint_t) * (size_t)n);
    }
    pthread_mutex_unlock(&r->mu);
    return n;
}

int q_reg_pick(q_reg_t *r, const char *service, q_endpoint_t *out)
{
    q_svc_t *svc;

    if (r == NULL || service == NULL || out == NULL) return Q_ERR_INVAL;

    pthread_mutex_lock(&r->mu);
    svc = q_hash_get(r->svcs, service);
    if (svc == NULL || svc->nep <= 0) {
        pthread_mutex_unlock(&r->mu);
        return Q_ERR_NOTFOUND;
    }
    unsigned long k = svc->rr++ % (unsigned long)svc->nep;
    *out = svc->eps[k];
    pthread_mutex_unlock(&r->mu);
    return Q_OK;
}

/* ---------------- 启动 / 停止 ---------------- */

int q_reg_start(q_reg_t *r, char *err, size_t errlen)
{
    if (r == NULL) return Q_ERR_INVAL;
    if (r->running) return Q_OK;

    if (q_reg_register(r, err, errlen) != Q_OK) return Q_ERR;

    q_reg_watch(r, r->service);

    r->running = 1;
    if (pthread_create(&r->hb_thr, NULL, heartbeat_thread, r) != 0) {
        r->running = 0;
        snprintf(err, errlen, "cannot start heartbeat thread");
        return Q_ERR;
    }
    if (pthread_create(&r->rf_thr, NULL, refresh_thread, r) != 0) {
        r->running = 0;
        pthread_join(r->hb_thr, NULL);
        snprintf(err, errlen, "cannot start refresh thread");
        return Q_ERR;
    }
    return Q_OK;
}

void q_reg_stop(q_reg_t *r)
{
    if (r == NULL || !r->running) return;

    r->running = 0;
    pthread_join(r->hb_thr, NULL);
    pthread_join(r->rf_thr, NULL);
    q_reg_deregister(r);
}

void q_reg_stat(q_reg_t *r, q_reg_stat_t *out)
{
    if (r == NULL || out == NULL) return;
    memset(out, 0, sizeof(*out));
    out->registered   = r->registered;
    out->heartbeats   = r->heartbeats;
    out->refresh_ok   = r->refresh_ok;
    out->refresh_fail = r->refresh_fail;
    out->watched      = (int)q_hash_size(r->svcs);
}

/* ---------------- 服务间调用 ---------------- */

static __thread q_httpc_t *t_httpc = NULL;

static q_httpc_t *call_client(void)
{
    if (t_httpc == NULL) t_httpc = q_httpc_new(3000, 500);
    return t_httpc;
}

int q_reg_call(q_reg_t *r, const char *service, const char *path,
               const char *method, const char *body,
               q_httpc_resp_t *out, char *err, size_t errlen)
{
    q_endpoint_t  eps[Q_REG_MAX_ENDPOINTS];
    int           n;
    char          url[1024];
    q_httpc_t    *c;

    if (r == NULL || service == NULL || path == NULL || out == NULL) return Q_ERR_INVAL;

    n = q_reg_endpoints(r, service, eps, Q_REG_MAX_ENDPOINTS);
    if (n <= 0) {
        snprintf(err, errlen, "no healthy instance for '%s'", service);
        return Q_ERR_NOTFOUND;
    }

    c = call_client();
    if (c == NULL) {
        snprintf(err, errlen, "http client init failed");
        return Q_ERR;
    }

    /* 从轮询游标开始，最多试 Q_REG_CALL_TRY_MAX 个实例 */
    q_endpoint_t start;
    int begin = 0;
    if (q_reg_pick(r, service, &start) == Q_OK) {
        for (int i = 0; i < n; i++) {
            if (eps[i].port == start.port &&
                strcmp(eps[i].host, start.host) == 0) { begin = i; break; }
        }
    }

    char last[256];
    last[0] = '\0';

    for (int k = 0; k < n && k < Q_REG_CALL_TRY_MAX; k++) {
        q_endpoint_t *e = &eps[(begin + k) % n];
        snprintf(url, sizeof(url), "http://%s:%d%s", e->host, e->port, path);

        int rc = q_httpc_do(c, method ? method : "GET", url,
                            body, body ? strlen(body) : 0,
                            NULL, 0, out);
        if (rc == Q_OK) return Q_OK;

        snprintf(last, sizeof(last), "%s -> %s", url, out->err);
        q_httpc_resp_free(out);
        q_warn("call %s failed: %s", url, last);
    }
    snprintf(err, errlen, "all %d instance(s) failed, last: %s", n, last);
    return Q_ERR;
}
