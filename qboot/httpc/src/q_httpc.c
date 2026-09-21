/*
 * libcurl 薄封装。要点：
 *   1) curl_global_init 进程级只调一次
 *   2) 每个 q_httpc_t 独占一个 easy handle，跨请求复用连接
 *   3) 响应体用可增长缓冲收，避免预估长度
 */

#include <q/httpc.h>

#include <curl/curl.h>

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#define Q_HTTPC_MAX_BODY (16u * 1024u * 1024u)   /* 响应体上限 16MB */

struct q_httpc {
    CURL       *h;
    long        timeout_ms;
    long        connect_timeout_ms;
    char        errbuf[CURL_ERROR_SIZE];
};

static pthread_mutex_t g_init_mu  = PTHREAD_MUTEX_INITIALIZER;
static int             g_inited   = 0;

int q_httpc_global_init(void)
{
    pthread_mutex_lock(&g_init_mu);
    if (!g_inited) {
        if (curl_global_init(CURL_GLOBAL_DEFAULT) != 0) {
            pthread_mutex_unlock(&g_init_mu);
            return Q_ERR;
        }
        g_inited = 1;
    }
    pthread_mutex_unlock(&g_init_mu);
    return Q_OK;
}

void q_httpc_global_cleanup(void)
{
    pthread_mutex_lock(&g_init_mu);
    if (g_inited) {
        curl_global_cleanup();
        g_inited = 0;
    }
    pthread_mutex_unlock(&g_init_mu);
}

q_httpc_t *q_httpc_new(long timeout_ms, long connect_timeout_ms)
{
    q_httpc_t *c;

    if (!g_inited) q_httpc_global_init();

    c = calloc(1, sizeof(q_httpc_t));
    if (c == NULL) return NULL;

    c->h = curl_easy_init();
    if (c->h == NULL) {
        free(c);
        return NULL;
    }
    c->timeout_ms         = timeout_ms > 0 ? timeout_ms : 3000;
    c->connect_timeout_ms = connect_timeout_ms > 0 ? connect_timeout_ms : 500;

    /* 不收信号：多线程程序里 curl 触发 SIGPIPE 会要命 */
    curl_easy_setopt(c->h, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c->h, CURLOPT_ERRORBUFFER, c->errbuf);
    curl_easy_setopt(c->h, CURLOPT_TIMEOUT_MS, c->timeout_ms);
    curl_easy_setopt(c->h, CURLOPT_CONNECTTIMEOUT_MS, c->connect_timeout_ms);
    curl_easy_setopt(c->h, CURLOPT_TCP_NODELAY, 1L);
    /* 重定向交给调用方决定，默认不跟 */
    curl_easy_setopt(c->h, CURLOPT_FOLLOWLOCATION, 0L);

    /*
     * 关键：显式关掉代理。
     * libcurl 默认会读 http_proxy / HTTPS_PROXY 环境变量，
     * 服务注册、服务间调用这类内网流量一旦被环境里的代理截走就会各种诡异失败
     * （请求行变成绝对 URI，对端直接 404）。直连是这里唯一正确的语义。
     */
    curl_easy_setopt(c->h, CURLOPT_PROXY, "");
    curl_easy_setopt(c->h, CURLOPT_NOPROXY, "*");
    return c;
}

void q_httpc_free(q_httpc_t *c)
{
    if (c == NULL) return;
    if (c->h != NULL) curl_easy_cleanup(c->h);
    free(c);
}

/* ---------------- 响应体收集 ---------------- */

typedef struct {
    char  *data;
    size_t len;
    size_t cap;
    int    oom;
} sink_t;

static size_t sink_write(char *ptr, size_t size, size_t nmemb, void *ud)
{
    sink_t   *s = ud;
    size_t    n = size * nmemb;

    if (n == 0) return 0;
    if (s->oom) return 0;

    if (s->len + n + 1 > s->cap) {
        size_t want = (s->len + n + 1) * 2;
        if (want > Q_HTTPC_MAX_BODY) want = Q_HTTPC_MAX_BODY;
        if (s->len + n + 1 > want) {
            s->oom = 1;
            return 0;
        }
        char *p = realloc(s->data, want);
        if (p == NULL) {
            s->oom = 1;
            return 0;
        }
        s->data = p;
        s->cap  = want;
    }
    memcpy(s->data + s->len, ptr, n);
    s->len += n;
    s->data[s->len] = '\0';
    return n;
}

static void sink_init(sink_t *s)
{
    s->cap = 4096;
    s->len = 0;
    s->oom = 0;
    s->data = malloc(s->cap);
    if (s->data == NULL) {
        s->cap = 0;
        s->oom = 1;
    }
}

static void sink_free(sink_t *s) { free(s->data); }

/* ---------------- 请求 ---------------- */

int q_httpc_do(q_httpc_t *c, const char *method, const char *url,
               const char *body, size_t body_len,
               const char *const *headers, int nheaders,
               q_httpc_resp_t *out)
{
    struct curl_slist *hl = NULL;
    sink_t             sink;
    CURLcode           rc;
    long               status = 0;

    if (c == NULL || method == NULL || url == NULL || out == NULL) return Q_ERR_INVAL;

    memset(out, 0, sizeof(*out));
    c->errbuf[0] = '\0';
    sink_init(&sink);

    /* 每次请求都重置，避免上一次的设置残留 */
    curl_easy_reset(c->h);
    curl_easy_setopt(c->h, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c->h, CURLOPT_ERRORBUFFER, c->errbuf);
    curl_easy_setopt(c->h, CURLOPT_TIMEOUT_MS, c->timeout_ms);
    curl_easy_setopt(c->h, CURLOPT_CONNECTTIMEOUT_MS, c->connect_timeout_ms);
    curl_easy_setopt(c->h, CURLOPT_TCP_NODELAY, 1L);
    /* reset 会把代理恢复成「读环境变量」，这里必须再关一次，否则第二次请求就走代理 */
    curl_easy_setopt(c->h, CURLOPT_PROXY, "");
    curl_easy_setopt(c->h, CURLOPT_NOPROXY, "*");
    curl_easy_setopt(c->h, CURLOPT_WRITEFUNCTION, sink_write);
    curl_easy_setopt(c->h, CURLOPT_WRITEDATA, &sink);

    curl_easy_setopt(c->h, CURLOPT_URL, url);
    curl_easy_setopt(c->h, CURLOPT_CUSTOMREQUEST, method);

    if (body != NULL && body_len > 0) {
        curl_easy_setopt(c->h, CURLOPT_POSTFIELDS, body);
        curl_easy_setopt(c->h, CURLOPT_POSTFIELDSIZE, (long)body_len);
    } else {
        /* 不带 body 的请求把 Content-Length 置 0，否则某些服务端一直等 */
        curl_easy_setopt(c->h, CURLOPT_POSTFIELDS, "");
        curl_easy_setopt(c->h, CURLOPT_POSTFIELDSIZE, 0L);
    }

    for (int i = 0; i < nheaders; i++) {
        if (headers != NULL && headers[i] != NULL) {
            hl = curl_slist_append(hl, headers[i]);
        }
    }
    if (hl != NULL) curl_easy_setopt(c->h, CURLOPT_HTTPHEADER, hl);

    rc = curl_easy_perform(c->h);
    if (hl != NULL) curl_slist_free_all(hl);

    if (rc != CURLE_OK) {
        snprintf(out->err, sizeof(out->err), "%s",
                 c->errbuf[0] ? c->errbuf : curl_easy_strerror(rc));
        sink_free(&sink);
        return Q_ERR_IO;
    }

    curl_easy_getinfo(c->h, CURLINFO_RESPONSE_CODE, &status);
    out->status   = status;
    out->body     = sink.data;
    out->body_len = sink.len;

    if (sink.oom) {
        snprintf(out->err, sizeof(out->err), "response too large (>%u bytes)",
                 Q_HTTPC_MAX_BODY);
        return Q_ERR_NOMEM;
    }
    /* body 所有权移交给 out，别在这里 free */
    return (status >= 200 && status < 300) ? Q_OK : Q_ERR;
}

void q_httpc_resp_free(q_httpc_resp_t *r)
{
    if (r == NULL) return;
    free(r->body);
    r->body = NULL;
    r->body_len = 0;
}

static const char *k_json[] = {
    "Content-Type: application/json",
    "Accept: application/json"
};

int q_httpc_get(q_httpc_t *c, const char *url, q_httpc_resp_t *out)
{
    return q_httpc_do(c, "GET", url, NULL, 0, k_json, 2, out);
}

int q_httpc_put(q_httpc_t *c, const char *url, const char *json, q_httpc_resp_t *out)
{
    return q_httpc_do(c, "PUT", url, json, json ? strlen(json) : 0, k_json, 2, out);
}

int q_httpc_post(q_httpc_t *c, const char *url, const char *json, q_httpc_resp_t *out)
{
    return q_httpc_do(c, "POST", url, json, json ? strlen(json) : 0, k_json, 2, out);
}

int q_httpc_delete(q_httpc_t *c, const char *url, q_httpc_resp_t *out)
{
    return q_httpc_do(c, "DELETE", url, NULL, 0, NULL, 0, out);
}
