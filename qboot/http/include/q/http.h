#ifndef Q_HTTP_H
#define Q_HTTP_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct q_http q_http_t;
typedef struct q_req  q_req_t;
typedef struct q_resp q_resp_t;

typedef int (*q_handler_t)(q_req_t *req, q_resp_t *resp, void *ud);

/*
 * 多线程 HTTP server：每个 IO 线程一个 event_base，
 * 各自 bind 同一端口（SO_REUSEPORT），由内核分发连接。
 * 连接对象绑定到接收它的那个 IO 线程，生命周期内不跨线程，因此不需要加锁。
 */

q_http_t *q_http_new(int port, int io_threads);
int       q_http_run(q_http_t *h);        /* 阻塞直到 q_http_stop */
void      q_http_stop(q_http_t *h);
void      q_http_free(q_http_t *h);

/* 路由注册。path 支持 :name 形式的路径参数 */
int q_http_route(q_http_t *h, const char *method, const char *path,
                 q_handler_t fn, void *ud);

#define Q_ROUTE_GET(h, path, fn)  q_http_route((h), "GET",  (path), (fn), NULL)
#define Q_ROUTE_POST(h, path, fn) q_http_route((h), "POST", (path), (fn), NULL)

/* 内置：/health 返回 {"status":"up"}，无需业务注册 */
int  q_http_route_health(q_http_t *h);

/* ---- 请求 ---- */

const char *q_req_method(const q_req_t *r);
const char *q_req_path(const q_req_t *r);
const char *q_req_header(const q_req_t *r, const char *name);
const char *q_req_body(const q_req_t *r);
size_t      q_req_body_len(const q_req_t *r);
const char *q_req_query(const q_req_t *r, const char *key);
const char *q_req_param(const q_req_t *r, const char *key);   /* /users/:id */
void       *q_req_ud(const q_req_t *r);

/* ---- 响应 ---- */

int  q_resp_status(q_resp_t *resp, int code);
int  q_resp_header(q_resp_t *resp, const char *key, const char *val);
int  q_resp_write(q_resp_t *resp, const char *data, size_t len);
int  q_resp_text(q_resp_t *resp, int code, const char *text);
int  q_resp_json(q_resp_t *resp, int code, const char *json);

#ifdef __cplusplus
}
#endif

#endif /* Q_HTTP_H */
