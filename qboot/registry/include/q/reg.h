#ifndef Q_REG_H
#define Q_REG_H

#include <q/core/types.h>
#include <q/httpc.h>

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 服务注册与发现（Consul）。
 *
 * 约定：
 *   - 用 Consul 的 TTL check：注册时声明 TTL，由本进程后台线程定期 pass，
 *     进程挂了没人续期，Consul 在 TTL 到期后把它标记为 critical 并摘掉。
 *   - DeregisterCriticalServiceAfter 保证进程被 kill -9 后也能自动清除。
 *   - 发现结果本地缓存，后台线程定期刷新，调用方 pick 时不打 Consul。
 */

typedef struct {
    char host[128];
    int  port;
} q_endpoint_t;

typedef struct q_reg q_reg_t;

typedef struct {
    const char        *consul_addr;    /* "127.0.0.1:8500" */
    const char        *service;        /* 服务名，如 "user-svc" */
    const char        *id;             /* 实例 id；NULL 则自动 <service>-<host>:<port> */
    const char        *host;           /* 对外暴露的地址 */
    int                port;
    const char *const *tags;           /* 标签数组 */
    int                ntags;
    int                ttl_secs;       /* 心跳周期，同时作为 TTL，默认 10 */
    int                refresh_secs;   /* 服务列表刷新周期，默认 5 */
} q_reg_cfg_t;

q_reg_t *q_reg_new(const q_reg_cfg_t *cfg);
void     q_reg_free(q_reg_t *r);

/* 注册 + 启动心跳线程；失败返回 <0 */
int  q_reg_register(q_reg_t *r, char *err, size_t errlen);
int  q_reg_deregister(q_reg_t *r);

/*
 * 一键启动：注册 + 心跳 + 后台刷新（把 cfg.service 也放进刷新列表）。
 * 进程退出前调 q_reg_stop()：停线程并主动注销。
 */
int  q_reg_start(q_reg_t *r, char *err, size_t errlen);
void q_reg_stop(q_reg_t *r);

/* 把一个关心的服务加入后台刷新列表 */
int q_reg_watch(q_reg_t *r, const char *service);

/* 取健康实例列表；返回实际个数（可能超过 cap，超出的丢弃） */
int  q_reg_endpoints(q_reg_t *r, const char *service, q_endpoint_t *out, int cap);

/* 轮询选一个实例；无可用实例返回 Q_ERR_NOTFOUND */
int  q_reg_pick(q_reg_t *r, const char *service, q_endpoint_t *out);

/* 强制立刻拉一次（不走缓存） */
int  q_reg_refresh(q_reg_t *r, const char *service);

/* 诊断 */
typedef struct {
    int registered;
    int heartbeats;
    int refresh_ok;
    int refresh_fail;
    int watched;
} q_reg_stat_t;

void q_reg_stat(q_reg_t *r, q_reg_stat_t *out);

/*
 * 服务间调用：解析服务 -> 轮询选实例 -> 发请求。
 * path 以 '/' 开头，如 "/api/user/1"。失败重试下一个实例，最多试 3 个。
 */
int q_reg_call(q_reg_t *r, const char *service, const char *path,
               const char *method, const char *body,
               q_httpc_resp_t *out, char *err, size_t errlen);

#ifdef __cplusplus
}
#endif

#endif /* Q_REG_H */
