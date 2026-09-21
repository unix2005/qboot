#ifndef Q_HTTPC_H
#define Q_HTTPC_H

#include <q/core/types.h>

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 出站 HTTP 客户端（libcurl 薄封装）。
 * 服务注册、服务间调用都走它。
 *
 * 线程模型：一个 q_httpc_t 只能被一个线程用，内部持有 curl easy handle，
 * 复用 TCP 连接（keep-alive）。不要用锁去共享它，每个线程自己 new 一个。
 */

typedef struct q_httpc q_httpc_t;

typedef struct {
    long   status;       /* HTTP 状态码，0 表示根本没连上 */
    char  *body;         /* 响应体，malloc 出来的，用完 q_httpc_resp_free */
    size_t body_len;
    char   err[256];     /* 失败原因 */
} q_httpc_resp_t;

/* 全局初始化，进程启动时调一次；非线程安全 */
int  q_httpc_global_init(void);
void q_httpc_global_cleanup(void);

q_httpc_t *q_httpc_new(long timeout_ms, long connect_timeout_ms);
void       q_httpc_free(q_httpc_t *c);

/*
 * headers 形如 {"Content-Type: application/json", "X-Token: abc", NULL} 亦可，
 * 传 nheaders 显式指定个数更稳妥。
 */
int q_httpc_do(q_httpc_t *c, const char *method, const char *url,
               const char *body, size_t body_len,
               const char *const *headers, int nheaders,
               q_httpc_resp_t *out);

void q_httpc_resp_free(q_httpc_resp_t *r);

int q_httpc_get(q_httpc_t *c, const char *url, q_httpc_resp_t *out);
int q_httpc_put(q_httpc_t *c, const char *url, const char *json, q_httpc_resp_t *out);
int q_httpc_post(q_httpc_t *c, const char *url, const char *json, q_httpc_resp_t *out);
int q_httpc_delete(q_httpc_t *c, const char *url, q_httpc_resp_t *out);   /* 无 body */

#ifdef __cplusplus
}
#endif

#endif /* Q_HTTPC_H */
