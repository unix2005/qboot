/*
 * qboot 示例服务（M0 阶段）：配置 + 日志 + 线程池 + 优雅退出。
 * 现在还没有 HTTP 层，用线程池模拟请求处理，验证框架底座。
 *
 *   ./demo_svc samples/conf/app.ini
 *   Q_LOG_LEVEL=error ./demo_svc samples/conf/app.ini     # 环境变量覆盖
 */

#include <q/conf.h>
#include <q/log.h>

#include <q/core/mem.h>
#include <q/core/threadpool.h>
#include <q/core/time.h>
#include <q/core/types.h>

#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static volatile sig_atomic_t g_stop = 0;
static _Atomic int           g_handled = 0;

static void on_signal(int sig)
{
    if (sig == SIGINT || sig == SIGTERM) g_stop = 1;
}

/* 每线程一个请求池：请求内所有临时分配都从这里出，请求结束 reset 复用 */
static __thread q_pool_t *t_pool = NULL;

static void handle_request(void *arg)
{
    int id = (int)(intptr_t)arg;

    if (t_pool == NULL) t_pool = q_pool_new(4096);

    char rid[32];
    snprintf(rid, sizeof(rid), "req-%06d", id);
    q_log_set_ctx(NULL, rid);

    char *path = q_pool_strdup(t_pool, "/api/users/1001");
    char *ua   = q_pool_strdup(t_pool, "qboot-demo/0.1");
    char *sql  = q_pool_strdup(t_pool, "SELECT * FROM t_user WHERE id=?");

    q_info_cat("access", "GET %s ua=%s", path, ua);

    if (id % 50 == 0)  q_warn_cat("sql", "slow query 120ms: %s", sql);
    if (id % 500 == 0) q_error_cat("sql", "connect failed, retrying: %s", path);

    atomic_fetch_add(&g_handled, 1);
    q_log_clear_ctx();

    q_pool_reset(t_pool);          /* 整个请求的临时内存一次回收 */
}

int main(int argc, char **argv)
{
    const char *conf_path = (argc > 1) ? argv[1] : "conf/app.ini";

    q_conf_t *conf = q_conf_load(conf_path);
    if (conf == NULL) {
        fprintf(stderr, "load conf failed: %s\n", conf_path);
        return 1;
    }

    const char *name      = q_conf_get(conf, "server", "name", "demo-svc");
    const char *dir       = q_conf_get(conf, "log", "dir", "/tmp/qboot-demo");
    const char *level     = q_conf_get(conf, "log", "level", "info");
    int         workers   = q_conf_get_int(conf, "server", "worker_threads", 4);
    int         port      = q_conf_get_int(conf, "server", "port", 8081);
    size_t      max_bytes = (size_t)q_conf_get_ll(conf, "log", "max_bytes", 67108864LL);
    int         keep_days = q_conf_get_int(conf, "log", "keep_days", 7);
    const char *db_url    = q_conf_get(conf, "db", "url", "");

    if (q_log_init(dir, name, q_log_level_from_str(level)) != Q_OK) {
        fprintf(stderr, "log init failed, dir=%s\n", dir);
        return 1;
    }
    q_log_add_cat("access", max_bytes, keep_days);
    q_log_add_cat("sql",    max_bytes, keep_days);

    q_info("service %s starting: port=%d workers=%d level=%s", name, port, workers, level);
    q_info("db url=%s", db_url);

    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);

    q_tp_t *tp = q_tp_new(workers, 4096);
    if (tp == NULL) {
        q_error("threadpool create failed");
        q_log_close();
        return 1;
    }

    int id = 0;
    while (!g_stop && id < 20000) {
        int rc = q_tp_submit(tp, handle_request, (void *)(intptr_t)(++id));
        if (rc == Q_ERR_BUSY) {
            q_time_sleep_ms(1);      /* 队列满：退避重试，背压由框架显式暴露给调用方 */
            continue;
        }
        if (id % 2000 == 0) q_time_sleep_ms(1);
    }

    q_tp_destroy(tp);                /* 内部等队列清空 */

    q_log_stat_t st;
    q_log_stat(&st);
    q_info("service stopping: handled=%d written=%llu dropped=%llu sync=%llu",
           atomic_load(&g_handled), st.written, st.dropped, st.sync_writes);

    q_log_close();
    q_conf_free(conf);

    printf("handled=%d  log dir=%s\n", atomic_load(&g_handled), dir);
    return 0;
}
