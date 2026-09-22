/*
 * 最小 qboot 服务模板：配置 + 日志 + HTTP + 优雅退出。
 *
 * 这是"链接 qboot 库写微服务"的最小骨架，不含数据库。
 * 要加数据访问，参考 samples/user_svc.c。
 *
 * 编译（用安装后的库）：
 *   cc -o minimal_svc minimal_svc.c $(pkg-config --cflags --libs qboot)
 *   ./minimal_svc app.ini
 *
 * 试：
 *   curl http://127.0.0.1:9090/hello
 *   curl http://127.0.0.1:9090/health
 */

#include <q/conf.h>
#include <q/http.h>
#include <q/log.h>

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>

static q_http_t *g_srv = NULL;

static void on_signal(int sig)
{
    if (sig == SIGINT || sig == SIGTERM) {
        /* 让阻塞在 q_http_run 的主线程退出，走正常收尾 */
        if (g_srv != NULL) q_http_stop(g_srv);
    }
}

static int h_hello(q_req_t *req, q_resp_t *resp, void *ud)
{
    (void)req;
    (void)ud;
    return q_resp_json(resp, 200, "{\"msg\":\"hello from minimal-svc\"}");
}

static int h_echo(q_req_t *req, q_resp_t *resp, void *ud)
{
    (void)ud;
    const char *body = q_req_body(req);
    if (body == NULL || q_req_body_len(req) == 0) body = "{}";
    return q_resp_json(resp, 200, body);
}

int main(int argc, char **argv)
{
    const char *cfgpath = (argc > 1) ? argv[1] : "app.ini";

    q_conf_t *cf = q_conf_load(cfgpath);
    if (cf == NULL) {
        /* 配置文件缺失也要能起来，全部走默认值 */
        fprintf(stderr, "warn: cannot load %s, use defaults\n", cfgpath);
        cf = q_conf_new();
    }

    const char *name = q_conf_get(cf, "server", "name", "minimal-svc");
    int         port = q_conf_get_int(cf, "server", "port", 9090);
    int         io   = q_conf_get_int(cf, "server", "io_threads", 2);
    const char *dir  = q_conf_get(cf, "log", "dir", "logs");

    q_log_init(dir, name, Q_LOG_INFO);

    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);

    g_srv = q_http_new(port, io);
    if (g_srv == NULL) {
        fprintf(stderr, "cannot listen on %d\n", port);
        return 1;
    }
    /* 内置 /health，无需自己写 */
    q_http_route_health(g_srv);
    q_http_route(g_srv, "GET",  "/hello", h_hello, NULL);
    q_http_route(g_srv, "POST", "/echo",  h_echo,  NULL);

    printf("%s listening on %d\n", name, port);
    q_info("%s started on port %d", name, port);

    q_http_run(g_srv);            /* 阻塞，直到 q_http_stop */

    q_info("shutting down");
    q_http_free(g_srv);
    q_conf_free(cf);
    q_log_close();
    return 0;
}
