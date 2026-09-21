/*
 * libq_http：libevent（IO） + llhttp（解析）的 HTTP/1.1 server。
 *
 * 线程模型：每个 IO 线程独立 event_base，各自 bind 同一端口（SO_REUSEPORT），
 * 由内核分发连接；连接对象绑定到接收它的线程，生命周期内不跨线程。
 */

#include <q/http.h>

#include <q/core/ds.h>
#include <q/core/str.h>
#include <q/core/types.h>
#include <q/log.h>

#include "llhttp.h"

#include <event2/buffer.h>
#include <event2/bufferevent.h>
#include <event2/event.h>
#include <event2/listener.h>
#include <event2/thread.h>
#include <event2/util.h>

#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define Q_HTTP_ROUTES   256
#define Q_HTTP_PARAMS   8
#define Q_HTTP_HEADERS  32
#define Q_HTTP_RESP_HDR 8
#define Q_HTTP_CHUNK    65536

typedef struct {
    char key[64];
    char val[256];
} kv_t;

typedef struct {
    char         method[8];
    char         path[512];
    size_t       path_len;
    char         query[512];
    size_t       query_len;
    kv_t         headers[Q_HTTP_HEADERS];
    int          nheaders;
    kv_t         params[Q_HTTP_PARAMS];
    int          nparams;
    q_str_t      body;
    void        *ud;
    int          keep_alive;
} q_req_impl_t;

struct q_req {
    q_req_impl_t impl;
};

struct q_resp {
    int     code;
    char    content_type[64];
    kv_t    headers[Q_HTTP_RESP_HDR];
    int     nheaders;
    q_str_t body;
};

typedef struct {
    char        method[8];
    char        pattern[256];
    q_handler_t fn;
    void       *ud;
} q_route_t;

typedef struct q_http {
    int                   port;
    int                   io_threads;
    q_route_t             routes[Q_HTTP_ROUTES];
    int                   nroutes;

    pthread_t            *threads;
    struct event_base   **bases;
    _Atomic int           stop;
    int                   running;
} q_http_t;

typedef struct {
    q_http_t            *srv;
    struct event_base   *base;
    struct evconnlistener *listener;
    int                  idx;
} io_ctx_t;

typedef struct {
    q_http_t            *srv;
    struct bufferevent  *bev;
    llhttp_t             parser;
    llhttp_settings_t    settings;
    struct q_req        *req;
    struct q_resp        resp;
    int                  closing;
    int                  need_reset;    /* 本轮消息已处理完，待 execute 返回后 llhttp_reset */
    char                 hfield[128];
    size_t               hfield_len;
} conn_t;

/* ---------------- 小工具 ---------------- */

static void sbuf_append(char *buf, size_t cap, size_t *len, const char *s, size_t n)
{
    if (*len + n >= cap) n = cap - *len - 1;
    if (n <= 0) return;
    memcpy(buf + *len, s, n);
    *len += n;
    buf[*len] = '\0';
}

static const char *seg_end(const char *s)
{
    while (*s != '\0' && *s != '/') s++;
    return s;
}

static const char *reason_for(int code)
{
    switch (code) {
    case 200: return "OK";
    case 201: return "Created";
    case 204: return "No Content";
    case 400: return "Bad Request";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 500: return "Internal Server Error";
    case 503: return "Service Unavailable";
    default:  return "Unknown";
    }
}

/* ---------------- llhttp 回调 ---------------- */

static void dispatch(conn_t *c);     /* 实现在路由匹配之后 */

static int cb_url(llhttp_t *p, const char *at, size_t len)
{
    conn_t *c = p->data;
    const char *q = memchr(at, '?', len);

    if (q != NULL) {
        sbuf_append(c->req->impl.path, sizeof(c->req->impl.path), &c->req->impl.path_len,
                    at, (size_t)(q - at));
        sbuf_append(c->req->impl.query, sizeof(c->req->impl.query), &c->req->impl.query_len,
                    q + 1, len - (size_t)(q - at) - 1);
    } else {
        sbuf_append(c->req->impl.path, sizeof(c->req->impl.path), &c->req->impl.path_len, at, len);
    }
    return 0;
}

static int cb_header_field(llhttp_t *p, const char *at, size_t len)
{
    conn_t *c = p->data;
    c->hfield_len = 0;
    sbuf_append(c->hfield, sizeof(c->hfield), &c->hfield_len, at, len);
    return 0;
}

static int cb_header_value(llhttp_t *p, const char *at, size_t len)
{
    conn_t *c = p->data;
    if (c->req->impl.nheaders >= Q_HTTP_HEADERS) return 0;

    kv_t *kv = &c->req->impl.headers[c->req->impl.nheaders++];
    snprintf(kv->key, sizeof(kv->key), "%s", c->hfield);

    size_t n = len < sizeof(kv->val) - 1 ? len : sizeof(kv->val) - 1;
    memcpy(kv->val, at, n);
    kv->val[n] = '\0';
    return 0;
}

static int cb_body(llhttp_t *p, const char *at, size_t len)
{
    conn_t *c = p->data;
    q_str_append(&c->req->impl.body, at, len);
    return 0;
}

/* 一个请求解析完成：路由分发 + 写响应 */
static int cb_message_complete(llhttp_t *p)
{
    conn_t *c = p->data;
    dispatch(c);
    return 0;
}

/* ---------------- 路由匹配 ---------------- */

static const char *param_store(q_req_impl_t *r, const char *name, size_t nlen,
                               const char *val, size_t vlen)
{
    kv_t *kv = &r->params[r->nparams++];
    size_t n = nlen < sizeof(kv->key) - 1 ? nlen : sizeof(kv->key) - 1;
    memcpy(kv->key, name, n);
    kv->key[n] = '\0';

    n = vlen < sizeof(kv->val) - 1 ? vlen : sizeof(kv->val) - 1;
    memcpy(kv->val, val, n);
    kv->val[n] = '\0';
    return kv->val;
}

static int match_path(const char *pattern, const char *path, q_req_impl_t *req)
{
    const char *p = pattern;
    const char *s = path;

    while (*p != '\0' && *s != '\0') {
        const char *pe = seg_end(p);
        const char *se = seg_end(s);

        if (*p == ':') {
            param_store(req, p + 1, (size_t)(pe - p - 1), s, (size_t)(se - s));
        } else {
            if ((pe - p) != (se - s) || strncmp(p, s, (size_t)(pe - p)) != 0) return 0;
        }

        p = pe;
        s = se;
        if (*p == '/' && *s == '/') { p++; s++; }
    }
    return (*p == '\0' && *s == '\0');
}

/* ---------------- 响应 ---------------- */

static void resp_reset(struct q_resp *resp)
{
    resp->code = 200;
    resp->nheaders = 0;
    resp->content_type[0] = '\0';
    q_str_clear(&resp->body);
}

static void send_response(conn_t *c)
{
    struct evbuffer *out = bufferevent_get_output(c->bev);

    evbuffer_add_printf(out, "HTTP/1.1 %d %s\r\n", c->resp.code, reason_for(c->resp.code));
    evbuffer_add_printf(out, "Content-Type: %s\r\n",
                        c->resp.content_type[0] ? c->resp.content_type : "text/plain; charset=utf-8");
    evbuffer_add_printf(out, "Content-Length: %zu\r\n", c->resp.body.len);
    for (int i = 0; i < c->resp.nheaders; i++) {
        evbuffer_add_printf(out, "%s: %s\r\n", c->resp.headers[i].key, c->resp.headers[i].val);
    }
    evbuffer_add_printf(out, "Connection: %s\r\n", c->req->impl.keep_alive ? "keep-alive" : "close");
    evbuffer_add(out, "\r\n", 2);
    if (c->resp.body.len > 0) {
        evbuffer_add(out, c->resp.body.data, c->resp.body.len);
    }

    if (c->req->impl.keep_alive) {
        /* 复用连接：清掉本轮状态，等待下一个请求 */
        c->req->impl.path_len = 0;
        c->req->impl.path[0] = '\0';
        c->req->impl.query_len = 0;
        c->req->impl.query[0] = '\0';
        c->req->impl.nheaders = 0;
        c->req->impl.nparams = 0;
        q_str_clear(&c->req->impl.body);
        resp_reset(&c->resp);
        /*
         * 不能在 llhttp 回调栈内 reset（会破坏解析状态机），
         * 标记后由 on_read 在 llhttp_execute 返回后再执行。
         */
        c->need_reset = 1;
    } else {
        c->closing = 1;
    }
}

static void dispatch(conn_t *c)
{
    q_http_t *srv = c->srv;
    q_req_impl_t *req = &c->req->impl;

    snprintf(req->method, sizeof(req->method), "%s",
             llhttp_method_name((llhttp_method_t)c->parser.method));
    req->keep_alive = llhttp_should_keep_alive(&c->parser);
    resp_reset(&c->resp);

    int matched = 0;
    for (int i = 0; i < srv->nroutes; i++) {
        q_route_t *rt = &srv->routes[i];
        if (strcmp(rt->method, req->method) != 0) continue;
        req->nparams = 0;
        if (match_path(rt->pattern, req->path, req)) {
            req->ud = rt->ud;
            rt->fn((q_req_t *)c->req, &c->resp, rt->ud);
            matched = 1;
            break;
        }
    }

    if (!matched) {
        q_resp_text(&c->resp, 404, "{\"error\":\"not found\"}");
    }
    send_response(c);
}

/* ---------------- bufferevent 回调 ---------------- */

static void conn_free(conn_t *c)
{
    if (c == NULL) return;
    if (c->req != NULL) {
        q_str_free(&c->req->impl.body);
        free(c->req);
    }
    q_str_free(&c->resp.body);
    free(c);
}

static void on_read(struct bufferevent *bev, void *arg)
{
    conn_t *c = arg;
    char   buf[Q_HTTP_CHUNK];
    size_t n;

    while ((n = bufferevent_read(bev, buf, sizeof(buf))) > 0) {
        enum llhttp_errno err = llhttp_execute(&c->parser, buf, n);
        if (err != HPE_OK) {
            q_warn("http parse error: %s", llhttp_errno_name(err));
            c->req->impl.keep_alive = 0;
            resp_reset(&c->resp);
            q_resp_json(&c->resp, 400, "{\"error\":\"bad request\"}");
            send_response(c);
            return;
        }
        if (c->need_reset) {          /* keep-alive：为下一个请求复位解析器 */
            llhttp_reset(&c->parser);
            c->need_reset = 0;
        }
    }
}

static void on_write(struct bufferevent *bev, void *arg)
{
    conn_t *c = arg;
    if (c->closing && evbuffer_get_length(bufferevent_get_output(bev)) == 0) {
        bufferevent_free(bev);
        conn_free(c);
    }
}

static void on_event(struct bufferevent *bev, short events, void *arg)
{
    conn_t *c = arg;
    if (events & (BEV_EVENT_EOF | BEV_EVENT_ERROR)) {
        bufferevent_free(bev);
        conn_free(c);
    }
}

static void on_accept(struct evconnlistener *l, evutil_socket_t fd,
                      struct sockaddr *addr, int socklen, void *arg)
{
    io_ctx_t *io = arg;
    (void)addr;
    (void)socklen;

    struct bufferevent *bev =
        bufferevent_socket_new(evconnlistener_get_base(l), fd, BEV_OPT_CLOSE_ON_FREE);
    if (bev == NULL) {
        evutil_closesocket(fd);
        return;
    }

    conn_t *c = calloc(1, sizeof(conn_t));
    if (c == NULL) {
        bufferevent_free(bev);
        return;
    }
    c->srv = io->srv;
    c->bev = bev;
    c->req = calloc(1, sizeof(struct q_req));
    if (c->req == NULL || q_str_init(&c->req->impl.body, 256) != 0 ||
        q_str_init(&c->resp.body, 256) != 0) {
        bufferevent_free(bev);
        conn_free(c);
        return;
    }

    llhttp_settings_init(&c->settings);
    c->settings.on_url          = cb_url;
    c->settings.on_header_field = cb_header_field;
    c->settings.on_header_value = cb_header_value;
    c->settings.on_body         = cb_body;
    c->settings.on_message_complete = cb_message_complete;

    llhttp_init(&c->parser, HTTP_REQUEST, &c->settings);
    c->parser.data = c;

    bufferevent_setcb(bev, on_read, on_write, on_event, c);
    bufferevent_enable(bev, EV_READ | EV_WRITE);
    (void)l;
}

/* ---------------- IO 线程 ---------------- */

static int make_listener(int port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    int on = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
#ifdef SO_REUSEPORT
    setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &on, sizeof(on));
#endif
    int nodelay = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

    struct sockaddr_in sin;
    memset(&sin, 0, sizeof(sin));
    sin.sin_family      = AF_INET;
    sin.sin_port        = htons((uint16_t)port);
    sin.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(fd, (struct sockaddr *)&sin, sizeof(sin)) != 0) {
        evutil_closesocket(fd);
        return -1;
    }
    if (listen(fd, 1024) != 0) {
        evutil_closesocket(fd);
        return -1;
    }
    evutil_make_socket_nonblocking(fd);
    return fd;
}

static void *io_thread_main(void *arg)
{
    io_ctx_t *io = arg;

    io->base = event_base_new();
    if (io->base == NULL) return NULL;
    io->srv->bases[io->idx] = io->base;      /* 供 q_http_stop 跨线程打断 loop */

    int fd = make_listener(io->srv->port);
    if (fd < 0) {
        q_error("bind port %d failed: %s", io->srv->port, strerror(errno));
        return NULL;
    }

    io->listener = evconnlistener_new(io->base, on_accept, io,
                                      LEV_OPT_CLOSE_ON_FREE, 1024, fd);
    if (io->listener == NULL) {
        evutil_closesocket(fd);
        return NULL;
    }

    event_base_loop(io->base, 0);

    evconnlistener_free(io->listener);
    event_base_free(io->base);
    return NULL;
}

/* ---------------- 对外接口 ---------------- */

q_http_t *q_http_new(int port, int io_threads)
{
    if (io_threads <= 0) io_threads = 1;
    if (io_threads > 32) io_threads = 32;

    q_http_t *h = calloc(1, sizeof(q_http_t));
    if (h == NULL) return NULL;

    h->port       = port;
    h->io_threads = io_threads;
    h->nroutes    = 0;
    h->running    = 0;
    atomic_store(&h->stop, 0);

    h->threads = calloc((size_t)io_threads, sizeof(pthread_t));
    h->bases   = calloc((size_t)io_threads, sizeof(struct event_base *));
    if (h->threads == NULL || h->bases == NULL) {
        free(h->threads);
        free(h->bases);
        free(h);
        return NULL;
    }
    return h;
}

void q_http_free(q_http_t *h)
{
    if (h == NULL) return;
    free(h->threads);
    free(h->bases);
    free(h);
}

int q_http_route(q_http_t *h, const char *method, const char *path,
                 q_handler_t fn, void *ud)
{
    if (h == NULL || method == NULL || path == NULL || fn == NULL) return Q_ERR_INVAL;
    if (h->nroutes >= Q_HTTP_ROUTES) return Q_ERR_BUSY;

    q_route_t *rt = &h->routes[h->nroutes++];
    snprintf(rt->method, sizeof(rt->method), "%s", method);
    snprintf(rt->pattern, sizeof(rt->pattern), "%s", path);
    rt->fn = fn;
    rt->ud = ud;
    return Q_OK;
}

static int health_handler(q_req_t *req, q_resp_t *resp, void *ud)
{
    (void)req;
    (void)ud;
    return q_resp_json(resp, 200, "{\"status\":\"up\"}");
}

int q_http_route_health(q_http_t *h)
{
    return q_http_route(h, "GET", "/health", health_handler, NULL);
}

int q_http_run(q_http_t *h)
{
    if (h == NULL) return Q_ERR_INVAL;

    evthread_use_pthreads();        /* 必须在任何 event_base 创建之前 */

    io_ctx_t *ctxs = calloc((size_t)h->io_threads, sizeof(io_ctx_t));
    if (ctxs == NULL) return Q_ERR_NOMEM;

    h->running = 1;
    for (int i = 0; i < h->io_threads; i++) {
        ctxs[i].srv = h;
        ctxs[i].idx = i;
        if (pthread_create(&h->threads[i], NULL, io_thread_main, &ctxs[i]) != 0) {
            h->threads[i] = 0;
        }
    }
    for (int i = 0; i < h->io_threads; i++) {
        if (h->threads[i] != 0) pthread_join(h->threads[i], NULL);
    }

    free(ctxs);
    h->running = 0;
    return Q_OK;
}

void q_http_stop(q_http_t *h)
{
    if (h == NULL) return;
    atomic_store(&h->stop, 1);
    for (int i = 0; i < h->io_threads; i++) {
        if (h->bases[i] != NULL) event_base_loopbreak(h->bases[i]);
    }
}

/* ---- 请求访问器 ---- */

const char *q_req_method(const q_req_t *r) { return r ? r->impl.method : NULL; }
const char *q_req_path(const q_req_t *r)   { return r ? r->impl.path   : NULL; }

const char *q_req_header(const q_req_t *r, const char *name)
{
    if (r == NULL || name == NULL) return NULL;
    for (int i = 0; i < r->impl.nheaders; i++) {
        if (strcasecmp(r->impl.headers[i].key, name) == 0) return r->impl.headers[i].val;
    }
    return NULL;
}

const char *q_req_body(const q_req_t *r)
{
    return (r && r->impl.body.data) ? r->impl.body.data : "";
}

size_t q_req_body_len(const q_req_t *r)
{
    return r ? r->impl.body.len : 0;
}

const char *q_req_param(const q_req_t *r, const char *key)
{
    if (r == NULL || key == NULL) return NULL;
    for (int i = 0; i < r->impl.nparams; i++) {
        if (strcmp(r->impl.params[i].key, key) == 0) return r->impl.params[i].val;
    }
    return NULL;
}

const char *q_req_query(const q_req_t *r, const char *key)
{
    static __thread char buf[256];
    if (r == NULL || key == NULL) return NULL;

    size_t klen = strlen(key);
    const char *p = r->impl.query;

    while (*p != '\0') {
        const char *amp = strchr(p, '&');
        size_t seg = amp ? (size_t)(amp - p) : strlen(p);
        if (seg > klen && p[klen] == '=' && strncmp(p, key, klen) == 0) {
            size_t vlen = seg - klen - 1;
            if (vlen >= sizeof(buf)) vlen = sizeof(buf) - 1;
            memcpy(buf, p + klen + 1, vlen);
            buf[vlen] = '\0';
            return buf;
        }
        if (amp == NULL) break;
        p = amp + 1;
    }
    return NULL;
}

void *q_req_ud(const q_req_t *r) { return r ? r->impl.ud : NULL; }

/* ---- 响应构造器 ---- */

int q_resp_status(q_resp_t *resp, int code)
{
    if (resp == NULL) return Q_ERR_INVAL;
    resp->code = code;
    return Q_OK;
}

int q_resp_header(q_resp_t *resp, const char *key, const char *val)
{
    if (resp == NULL || key == NULL || val == NULL) return Q_ERR_INVAL;
    if (resp->nheaders >= Q_HTTP_RESP_HDR) return Q_ERR_BUSY;

    kv_t *kv = &resp->headers[resp->nheaders++];
    snprintf(kv->key, sizeof(kv->key), "%s", key);
    snprintf(kv->val, sizeof(kv->val), "%s", val);
    return Q_OK;
}

int q_resp_write(q_resp_t *resp, const char *data, size_t len)
{
    if (resp == NULL || data == NULL) return Q_ERR_INVAL;
    return q_str_append(&resp->body, data, len);
}

int q_resp_text(q_resp_t *resp, int code, const char *text)
{
    if (resp == NULL) return Q_ERR_INVAL;
    resp->code = code;
    snprintf(resp->content_type, sizeof(resp->content_type), "text/plain; charset=utf-8");
    q_str_clear(&resp->body);
    return (text == NULL) ? Q_OK : q_str_append_cstr(&resp->body, text);
}

int q_resp_json(q_resp_t *resp, int code, const char *json)
{
    if (resp == NULL) return Q_ERR_INVAL;
    resp->code = code;
    snprintf(resp->content_type, sizeof(resp->content_type), "application/json");
    q_str_clear(&resp->body);
    return (json == NULL) ? Q_OK : q_str_append_cstr(&resp->body, json);
}
