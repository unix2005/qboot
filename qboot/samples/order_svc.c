/*
 * order-svc：演示"服务间调用"。
 *
 * 与 user_svc 的差别在于它不只是查自己的库，还要去调 user-svc 拿用户信息：
 *
 *   有 Consul（[consul] enable=1）：
 *       q_reg_call(g_reg, "user-svc", "/user/1", ...)   // 服务名 -> 实例 -> 请求
 *   没 Consul（enable=0）：
 *       退化成 [upstream] user_svc 里写的直连地址，代码路径完全一致。
 *
 * 也就是说：业务代码只认服务名，地址从哪来是配置的事。
 *
 * 启动：
 *   ./order_svc samples/conf/order_svc.ini
 *
 * 试（另开一个终端先起 user_svc，否则跨服务调用会报 502）：
 *   curl http://127.0.0.1:8082/order/1
 *   curl http://127.0.0.1:8082/order/list?userId=1
 *   curl -XPOST http://127.0.0.1:8082/order -d '{"userId":1,"item":"键盘","amount":299.5}'
 *   curl http://127.0.0.1:8082/demo/call-user/1
 *
 * 注意：db.url 默认是 mock://，mock 驱动返回的是固定结果集（见 main 里的
 * qmock_expect），接上真库以后这里就是真实数据，业务代码一行都不用改。
 */

#include <q/conf.h>
#include <q/db.h>
#include <q/db_mock.h>
#include <q/http.h>
#include <q/httpc.h>
#include <q/log.h>
#include <q/mapper.h>
#include <q/reg.h>

#include <jansson.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static q_http_t *g_srv = NULL;
static q_reg_t  *g_reg = NULL;

/* Consul 没开时的兜底地址 */
static char g_user_addr[128];

/* ---------------- 每线程资源 ---------------- */

typedef struct {
    q_mapper_t *mapper;
    q_dbp_t    *pool;
} app_t;

static app_t g_app;

static q_ctx_t *ctx_for(void)
{
    static __thread q_ctx_t *t_ctx = NULL;
    if (t_ctx == NULL) t_ctx = q_ctx_new(g_app.mapper, g_app.pool);
    return t_ctx;
}

/* q_httpc_t 内部持有 curl handle，不能跨线程共享，一人一个 */
static q_httpc_t *httpc_for(void)
{
    static __thread q_httpc_t *t_c = NULL;
    if (t_c == NULL) t_c = q_httpc_new(3000, 1000);
    return t_c;
}

/* ---------------- 响应辅助 ---------------- */

/*
 * 注意所有权：json_object_set_new 会"接管"这个引用，
 * 调用 json_ok 之后不能再 json_decref(data)，否则就是 use-after-free。
 * 想继续用就先 json_incref（参考 h_get_order）。
 */
static int json_ok(q_resp_t *resp, json_t *data)
{
    json_t *wrap = json_object();
    json_object_set_new(wrap, "code", json_integer(0));
    json_object_set_new(wrap, "data", data ? data : json_null());

    char *s = json_dumps(wrap, JSON_COMPACT);
    int rc = q_resp_json(resp, 200, s ? s : "{\"code\":0}");
    free(s);
    json_decref(wrap);
    return rc;
}

static int json_err(q_resp_t *resp, int code, int http_code, const char *msg)
{
    char buf[512];
    snprintf(buf, sizeof(buf), "{\"code\":%d,\"msg\":\"%s\"}", code, msg);
    return q_resp_json(resp, http_code, buf);
}

/* ---------------- 调用 user-svc ---------------- */

/*
 * 业务代码只写服务名 "user-svc"。
 * g_reg != NULL 时走 Consul 发现 + 轮询 + 失败换实例；
 * 否则退化成配置里的直连地址。两条路的返回结构一样，上层无感。
 */
static int call_user_svc(const char *path, q_httpc_resp_t *out,
                         char *err, size_t errlen)
{
    if (g_reg != NULL) {
        return q_reg_call(g_reg, "user-svc", path, "GET", NULL, out, err, errlen);
    }

    if (g_user_addr[0] == '\0') {
        snprintf(err, errlen, "user-svc 地址未配置（[upstream] user_svc 或开 Consul）");
        return -1;
    }

    char url[640];
    snprintf(url, sizeof(url), "http://%s%s", g_user_addr, path);
    if (q_httpc_get(httpc_for(), url, out) != Q_OK) {
        snprintf(err, errlen, "直连 %s 失败: %s", url, out->err);
        return -1;
    }
    return Q_OK;
}

/* ---------------- 业务处理 ---------------- */

static int h_get_order(q_req_t *req, q_resp_t *resp, void *ud)
{
    (void)ud;
    const char *id = q_req_param(req, "id");
    char err[256];

    json_t *p = json_pack("{s:I}", "id", (json_int_t)atoll(id ? id : "0"));
    json_t *rows = NULL;
    if (q_ctx_query(ctx_for(), "order.getById", p, &rows, err, sizeof(err)) != Q_OK) {
        json_decref(p);
        return json_err(resp, 500, 500, err);
    }
    json_decref(p);

    json_t *one = json_array_size(rows) > 0 ? json_incref(json_array_get(rows, 0))
                                            : json_null();
    int rc = json_ok(resp, one);
    json_decref(rows);
    return rc;
}

static int h_list(q_req_t *req, q_resp_t *resp, void *ud)
{
    (void)ud;
    const char *userId = q_req_query(req, "userId");
    char err[256];

    json_t *p = json_object();
    if (userId != NULL) json_object_set_new(p, "userId", json_integer(atoll(userId)));
    json_object_set_new(p, "offset", json_integer(0));
    json_object_set_new(p, "size",   json_integer(20));

    json_t *rows = NULL;
    if (q_ctx_query(ctx_for(), "order.listByUser", p, &rows, err, sizeof(err)) != Q_OK) {
        json_decref(p);
        return json_err(resp, 500, 500, err);
    }
    json_decref(p);

    /* rows 的所有权已经交给 json_ok 了，这里不能再 decref */
    return json_ok(resp, rows);
}

/*
 * 建单：先问 user-svc 这个用户存不存在，不存在就 400，存在才落库。
 * 这是微服务里最常见的一段"编排"逻辑，正好演示跨服务调用。
 */
static int h_add_order(q_req_t *req, q_resp_t *resp, void *ud)
{
    (void)ud;
    const char *body = q_req_body(req);
    json_error_t e;
    json_t *p = json_loads(body ? body : "{}", 0, &e);
    char err[256];

    if (p == NULL) return json_err(resp, 400, 400, "body 不是合法 JSON");

    json_t *juid = json_object_get(p, "userId");
    if (juid == NULL) {
        json_decref(p);
        return json_err(resp, 400, 400, "缺少 userId");
    }

    /* --- 1) 调 user-svc 校验用户 --- */
    char path[128];
    snprintf(path, sizeof(path), "/user/%lld", (long long)json_integer_value(juid));

    q_httpc_resp_t uout;
    if (call_user_svc(path, &uout, err, sizeof(err)) != Q_OK) {
        json_decref(p);
        return json_err(resp, 502, 502, err);
    }

    int user_ok = 0;
    if (uout.status == 200) {
        json_t *uj = json_loads(uout.body ? uout.body : "{}", 0, NULL);
        if (uj != NULL) {
            json_t *d = json_object_get(uj, "data");
            user_ok = json_is_object(d);
            json_decref(uj);
        }
    }
    q_httpc_resp_free(&uout);

    if (!user_ok) {
        json_decref(p);
        return json_err(resp, 400, 400, "用户不存在或 user-svc 不可用");
    }

    /* --- 2) 落库 --- */
    if (!json_object_get(p, "status")) json_object_set_new(p, "status", json_integer(0));

    uint64_t id = 0;
    if (q_ctx_insert(ctx_for(), "order.add", p, &id, err, sizeof(err)) != Q_OK) {
        json_decref(p);
        return json_err(resp, 500, 500, err);
    }
    json_decref(p);

    return json_ok(resp, json_pack("{s:I}", "id", (json_int_t)id));
}

/* 纯粹演示服务间调用：把 user-svc 的响应原样透出 */
static int h_call_user(q_req_t *req, q_resp_t *resp, void *ud)
{
    (void)ud;
    const char *id = q_req_param(req, "id");
    char err[256];
    char path[128];

    snprintf(path, sizeof(path), "/user/%s", id ? id : "1");

    q_httpc_resp_t out;
    if (call_user_svc(path, &out, err, sizeof(err)) != Q_OK) {
        return json_err(resp, 502, 502, err);
    }
    long st = out.status;
    int rc = q_resp_json(resp, st == 200 ? 200 : 502, out.body ? out.body : "{}");
    q_httpc_resp_free(&out);
    return rc;
}

/* ---------------- 启动 ---------------- */

static void on_signal(int sig)
{
    if (sig == SIGINT || sig == SIGTERM) {
        if (g_srv != NULL) q_http_stop(g_srv);
    }
}

int main(int argc, char **argv)
{
    const char *cfgpath = (argc > 1) ? argv[1] : "samples/conf/order_svc.ini";
    char err[256];

    q_conf_t *cf = q_conf_load(cfgpath);
    if (cf == NULL) {
        fprintf(stderr, "cannot load config: %s\n", cfgpath);
        return 1;
    }

    const char *name = q_conf_get(cf, "server", "name", "order-svc");
    int  port        = q_conf_get_int(cf, "server", "port", 8082);
    int  io_threads  = q_conf_get_int(cf, "server", "io_threads", 4);

    const char *logdir = q_conf_get(cf, "log", "dir", "logs");
    const char *loglvl = q_conf_get(cf, "log", "level", "info");
    q_log_init(logdir, name, q_log_level_from_str(loglvl));
    q_log_add_cat("sql",      64 * 1024 * 1024, 7);
    q_log_add_cat("registry", 16 * 1024 * 1024, 3);

    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);

    /* ---- 数据库 ---- */
    q_db_register_mock();
#ifdef Q_HAVE_MYSQL
    q_db_register_mysql();
#endif

    const char *dburl = q_conf_get(cf, "db", "url", "mock://u:p@127.0.0.1:0/demo");
    g_app.pool = q_dbp_new(dburl,
                           q_conf_get_int(cf, "db", "pool", 8),
                           q_conf_get_int(cf, "db", "idle_secs", 60),
                           io_threads);
    if (g_app.pool == NULL) {
        fprintf(stderr, "cannot create db pool: %s\n", dburl);
        return 1;
    }

    /*
     * mock 驱动不解析 SQL，返回的就是这里塞进去的固定结果集。
     * 换真库删掉这段即可。
     */
    if (strncmp(dburl, "mock://", 7) == 0) {
        static const char *cols[] = { "id", "user_id", "item", "amount",
                                      "status", "created_at" };
        static qmock_cell_t cells[] = {
            QMOCK_INT(1), QMOCK_INT(1), QMOCK_STR("键盘"), QMOCK_DOUBLE(299.5),
            QMOCK_INT(0), QMOCK_STR("2026-09-22 10:00:00"),

            QMOCK_INT(2), QMOCK_INT(1), QMOCK_STR("显示器"), QMOCK_DOUBLE(1299.0),
            QMOCK_INT(1), QMOCK_STR("2026-09-22 11:30:00"),
        };
        qmock_expect(cols, 6, cells, 2);
    }

    /* ---- mapper ---- */
    const char *mpdir = q_conf_get(cf, "mapper", "dir", "samples/mapper");
    g_app.mapper = q_mapper_load(mpdir);
    if (g_app.mapper == NULL || q_mapper_size(g_app.mapper) == 0) {
        fprintf(stderr, "cannot load mapper from %s\n", mpdir);
        return 1;
    }
    q_info("mapper ready: %d statement(s) from %s", q_mapper_size(g_app.mapper), mpdir);

    /* ---- 注册 / 发现 ---- */
    q_httpc_global_init();
    snprintf(g_user_addr, sizeof(g_user_addr), "%s",
             q_conf_get(cf, "upstream", "user_svc", "127.0.0.1:8081"));

    if (q_conf_get_bool(cf, "consul", "enable", 0)) {
        const char *tags[] = { "v1" };
        q_reg_cfg_t rcfg = {
            .consul_addr = q_conf_get(cf, "consul", "addr", "127.0.0.1:8500"),
            .service     = name,
            .host        = q_conf_get(cf, "consul", "host", "127.0.0.1"),
            .port        = port,
            .tags        = tags,
            .ntags       = 1,
            .ttl_secs    = q_conf_get_int(cf, "consul", "ttl", 10)
        };
        g_reg = q_reg_new(&rcfg);
        if (g_reg == NULL || q_reg_start(g_reg, err, sizeof(err)) != Q_OK) {
            q_error("consul register failed: %s（退化为直连 %s）", err, g_user_addr);
            if (g_reg != NULL) { q_reg_free(g_reg); g_reg = NULL; }
        } else {
            q_reg_watch(g_reg, "user-svc");
            q_info("registered: %s -> %s:%d", name, rcfg.host, port);
        }
    }

    /* ---- HTTP ---- */
    g_srv = q_http_new(port, io_threads);
    if (g_srv == NULL) {
        fprintf(stderr, "cannot create http server\n");
        return 1;
    }

    q_http_route_health(g_srv);
    q_http_route(g_srv, "GET",  "/order/list",        h_list,       NULL);
    q_http_route(g_srv, "GET",  "/order/:id",         h_get_order,  NULL);
    q_http_route(g_srv, "POST", "/order",             h_add_order,  NULL);
    q_http_route(g_srv, "GET",  "/demo/call-user/:id", h_call_user, NULL);

    q_info("%s listening on %d", name, port);
    printf("%s listening on %d (user-svc via %s)\n", name, port,
           g_reg != NULL ? "consul" : g_user_addr);

    q_http_run(g_srv);

    /* ---- 优雅退出 ---- */
    q_info("shutting down");
    if (g_reg != NULL) { q_reg_stop(g_reg); q_reg_free(g_reg); }
    q_httpc_global_cleanup();
    q_http_free(g_srv);
    q_mapper_free(g_app.mapper);
    q_dbp_free(g_app.pool);
    q_conf_free(cf);
    q_log_close();
    return 0;
}
