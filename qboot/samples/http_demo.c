/*
 * qboot HTTP 示例服务（M0）：
 *   GET  /health          -> {"status":"up"}          框架内置
 *   GET  /                -> {"msg":"hello qboot"}
 *   GET  /api/users/:id   -> 路径参数示例
 *   GET  /api/search?q=   -> query 参数示例
 *   POST /api/echo        -> 回显请求体
 *
 *   ./http_demo samples/conf/app.ini
 *   curl -i http://127.0.0.1:8081/api/users/42
 */

#include <q/http.h>

#include <q/conf.h>
#include <q/core/types.h>
#include <q/log.h>

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static q_http_t *g_srv = NULL;

static void on_signal(int sig)
{
    if (sig == SIGINT || sig == SIGTERM) {
        q_info("signal %d received, stopping", sig);
        q_http_stop(g_srv);
    }
}

static int hello(q_req_t *req, q_resp_t *resp, void *ud)
{
    (void)req;
    (void)ud;
    return q_resp_json(resp, 200, "{\"msg\":\"hello qboot\"}");
}

static int user_get(q_req_t *req, q_resp_t *resp, void *ud)
{
    (void)ud;
    const char *id = q_req_param(req, "id");
    char json[256];

    snprintf(json, sizeof(json),
             "{\"id\":\"%s\",\"name\":\"user-%s\",\"ua\":\"%s\"}",
             id ? id : "", id ? id : "",
             q_req_header(req, "User-Agent") ? q_req_header(req, "User-Agent") : "");
    return q_resp_json(resp, 200, json);
}

static int search(q_req_t *req, q_resp_t *resp, void *ud)
{
    (void)ud;
    const char *q = q_req_query(req, "q");
    char json[256];

    snprintf(json, sizeof(json), "{\"q\":\"%s\",\"count\":0}", q ? q : "");
    return q_resp_json(resp, 200, json);
}

static int echo(q_req_t *req, q_resp_t *resp, void *ud)
{
    (void)ud;
    q_resp_header(resp, "X-Echo-Len", "1");
    q_resp_write(resp, "{\"echo\":", 8);
    q_resp_write(resp, "\"", 1);
    q_resp_write(resp, q_req_body(req), q_req_body_len(req));
    q_resp_write(resp, "\"}", 2);
    return q_resp_status(resp, 200);
}

int main(int argc, char **argv)
{
    const char *conf_path = (argc > 1) ? argv[1] : "conf/app.ini";

    q_conf_t *conf = q_conf_load(conf_path);
    if (conf == NULL) {
        fprintf(stderr, "load conf failed: %s\n", conf_path);
        return 1;
    }

    const char *name    = q_conf_get(conf, "server", "name", "demo-svc");
    const char *dir     = q_conf_get(conf, "log", "dir", "/tmp/qboot-demo");
    const char *level   = q_conf_get(conf, "log", "level", "info");
    int         port    = q_conf_get_int(conf, "server", "port", 8081);
    int         io_thr  = q_conf_get_int(conf, "server", "io_threads", 4);

    if (q_log_init(dir, name, q_log_level_from_str(level)) != Q_OK) {
        fprintf(stderr, "log init failed\n");
        return 1;
    }
    q_log_add_cat("access", 67108864, 7);

    g_srv = q_http_new(port, io_thr);
    if (g_srv == NULL) {
        q_error("http server create failed");
        q_log_close();
        return 1;
    }

    q_http_route_health(g_srv);
    Q_ROUTE_GET (g_srv, "/",              hello);
    Q_ROUTE_GET (g_srv, "/api/users/:id", user_get);
    Q_ROUTE_GET (g_srv, "/api/search",    search);
    Q_ROUTE_POST(g_srv, "/api/echo",      echo);

    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);

    q_info("%s listening on :%d (io_threads=%d)", name, port, io_thr);
    printf("%s listening on http://127.0.0.1:%d\n", name, port);

    q_http_run(g_srv);          /* 阻塞，直到收到信号 */

    q_info("service stopped");
    q_http_free(g_srv);
    q_log_close();
    q_conf_free(conf);
    return 0;
}
