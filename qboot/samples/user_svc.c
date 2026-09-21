/*
 * user-svc：把 http + db + mapper + registry 串起来的完整示例。
 *
 *   ./user_svc samples/conf/user_svc.ini
 *
 * 因为默认连的是 mock 驱动，没有 MySQL 也能起来跑。
 * 换成真库：把 [db] url 改成 mysql://user:pass@host:3306/db 即可，代码不用动。
 *
 * 试：
 *   curl 'http://127.0.0.1:8081/user/1'
 *   curl 'http://127.0.0.1:8081/user/search?name=a%25&minAge=18'
 *   curl -XPOST http://127.0.0.1:8081/user -d '{"name":"lisi","age":20,"balance":1.5,"status":1}'
 */

#include <q/conf.h>
#include <q/db.h>
#include <q/db_mock.h>
#include <q/http.h>
#include <q/log.h>
#include <q/mapper.h>
#include <q/reg.h>

#include <jansson.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static volatile sig_atomic_t g_stop = 0;
static q_http_t             *g_srv  = NULL;
static q_reg_t              *g_reg  = NULL;

static void on_signal(int sig)
{
    if (sig == SIGINT || sig == SIGTERM) {
        g_stop = 1;
        /* 让阻塞在 q_http_run 的主线程退出，走优雅收尾 */
        if (g_srv != NULL) q_http_stop(g_srv);
    }
}

/* ---------------- 全局资源 ---------------- */

typedef struct {
    q_mapper_t *mapper;
    q_dbp_t    *pool;
} app_t;

static app_t g_app;

/* ---------------- 响应辅助 ---------------- */

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

/* 每个请求一个 ctx：ctx 不跨线程共享 */
static q_ctx_t *ctx_for(void)
{
    static __thread q_ctx_t *t_ctx = NULL;
    if (t_ctx == NULL) t_ctx = q_ctx_new(g_app.mapper, g_app.pool);
    return t_ctx;
}

/* ---------------- 业务处理 ---------------- */

static int h_get_user(q_req_t *req, q_resp_t *resp, void *ud)
{
    (void)ud;
    const char *id = q_req_param(req, "id");
    char  err[256];
    json_t *p = json_pack("{s:s}", "id", id ? id : "");
    json_t *rows = NULL;

    if (q_ctx_query(ctx_for(), "user.getById", p, &rows, err, sizeof(err)) != Q_OK) {
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

static int h_search(q_req_t *req, q_resp_t *resp, void *ud)
{
    (void)ud;
    const char *name   = q_req_query(req, "name");
    const char *minAge = q_req_query(req, "minAge");
    char err[256];

    json_t *p = json_object();
    if (name != NULL)   json_object_set_new(p, "name", json_string(name));
    if (minAge != NULL) json_object_set_new(p, "minAge", json_integer(atoll(minAge)));
    json_object_set_new(p, "offset", json_integer(0));
    json_object_set_new(p, "size",   json_integer(20));

    json_t *rows = NULL;
    if (q_ctx_query(ctx_for(), "user.search", p, &rows, err, sizeof(err)) != Q_OK) {
        json_decref(p);
        return json_err(resp, 500, 500, err);
    }
    json_decref(p);

    int rc = json_ok(resp, rows);
    json_decref(rows);
    return rc;
}

static int h_add(q_req_t *req, q_resp_t *resp, void *ud)
{
    (void)ud;
    const char *body = q_req_body(req);
    json_error_t e;
    json_t *p = json_loads(body ? body : "{}", 0, &e);
    char err[256];
    uint64_t id = 0;

    if (p == NULL) return json_err(resp, 400, 400, "body 不是合法 JSON");

    if (q_ctx_insert(ctx_for(), "user.add", p, &id, err, sizeof(err)) != Q_OK) {
        json_decref(p);
        return json_err(resp, 500, 500, err);
    }
    json_decref(p);

    int rc = json_ok(resp, json_pack("{s:I}", "id", (json_int_t)id));
    return rc;
}

static int h_update(q_req_t *req, q_resp_t *resp, void *ud)
{
    (void)ud;
    const char *id = q_req_param(req, "id");
    const char *body = q_req_body(req);
    json_error_t e;
    json_t *p = json_loads(body ? body : "{}", 0, &e);
    char err[256];
    uint64_t aff = 0;

    if (p == NULL) return json_err(resp, 400, 400, "body 不是合法 JSON");
    json_object_set_new(p, "id", json_integer(atoll(id ? id : "0")));

    if (q_ctx_exec(ctx_for(), "user.updateSelective", p, &aff, NULL, err, sizeof(err)) != Q_OK) {
        json_decref(p);
        return json_err(resp, 500, 500, err);
    }
    json_decref(p);
    return json_ok(resp, json_pack("{s:I}", "affected", (json_int_t)aff));
}

static int h_remove(q_req_t *req, q_resp_t *resp, void *ud)
{
    (void)ud;
    const char *id = q_req_param(req, "id");
    char err[256];
    uint64_t aff = 0;

    json_t *p = json_pack("{s:I}", "id", (json_int_t)atoll(id ? id : "0"));
    if (q_ctx_exec(ctx_for(), "user.remove", p, &aff, NULL, err, sizeof(err)) != Q_OK) {
        json_decref(p);
        return json_err(resp, 500, 500, err);
    }
    json_decref(p);
    return json_ok(resp, json_pack("{s:I}", "affected", (json_int_t)aff));
}

/* 演示服务间调用：本服务 -> order-svc */
static int h_call_order(q_req_t *req, q_resp_t *resp, void *ud)
{
    (void)ud; (void)req;
    char err[256];

    if (g_reg == NULL) return json_err(resp, 501, 501, "consul 未启用");

    q_httpc_resp_t out;
    if (q_reg_call(g_reg, "order-svc", "/order/list", "GET", NULL, &out,
                   err, sizeof(err)) != Q_OK) {
        return json_err(resp, 502, 502, err);
    }
    int rc = q_resp_json(resp, 200, out.body ? out.body : "{}");
    q_httpc_resp_free(&out);
    return rc;
}

/* ---------------- 启动 ---------------- */

int main(int argc, char **argv)
{
    const char *cfgpath = (argc > 1) ? argv[1] : "samples/conf/user_svc.ini";
    char err[256];

    q_conf_t *cf = q_conf_load(cfgpath);
    if (cf == NULL) {
        fprintf(stderr, "cannot load config: %s\n", cfgpath);
        return 1;
    }

    const char *name = q_conf_get(cf, "server", "name", "user-svc");
    int  port        = q_conf_get_int(cf, "server", "port", 8081);
    int  io_threads  = q_conf_get_int(cf, "server", "io_threads", 4);

    const char *logdir  = q_conf_get(cf, "log", "dir", "logs");
    const char *loglvl  = q_conf_get(cf, "log", "level", "info");
    q_log_init(logdir, name, q_log_level_from_str(loglvl));
    q_log_add_cat("sql",      64 * 1024 * 1024, 7);
    q_log_add_cat("registry", 16 * 1024 * 1024, 3);

    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);

    /* ---- 数据库 ---- */
    const char *dburl = q_conf_get(cf, "db", "url", "mock://u:p@127.0.0.1:0/demo");
    int         npool = q_conf_get_int(cf, "db", "pool", 8);
    int         idle  = q_conf_get_int(cf, "db", "idle_secs", 60);

    q_db_register_mock();
#ifdef Q_HAVE_MYSQL
    q_db_register_mysql();
#endif

    g_app.pool = q_dbp_new(dburl, npool, idle, io_threads);
    if (g_app.pool == NULL) {
        fprintf(stderr, "cannot create db pool: %s\n", dburl);
        return 1;
    }
    q_info("db pool ready: %s", dburl);

    /* ---- mapper ---- */
    const char *mpdir = q_conf_get(cf, "mapper", "dir", "samples/mapper");
    g_app.mapper = q_mapper_load(mpdir);
    if (g_app.mapper == NULL || q_mapper_size(g_app.mapper) == 0) {
        fprintf(stderr, "cannot load mapper from %s\n", mpdir);
        return 1;
    }
    q_info("mapper ready: %d statement(s) from %s",
           q_mapper_size(g_app.mapper), mpdir);

    /* ---- 服务注册 ---- */
    if (q_conf_get_bool(cf, "consul", "enable", 0)) {
        const char *tags[] = { "v1" };
        q_reg_cfg_t rcfg = {
            .consul_addr  = q_conf_get(cf, "consul", "addr", "127.0.0.1:8500"),
            .service      = name,
            .host         = q_conf_get(cf, "consul", "host", "127.0.0.1"),
            .port         = port,
            .tags         = tags,
            .ntags        = 1,
            .ttl_secs     = q_conf_get_int(cf, "consul", "ttl", 10)
        };
        q_httpc_global_init();
        g_reg = q_reg_new(&rcfg);
        if (g_reg == NULL || q_reg_start(g_reg, err, sizeof(err)) != Q_OK) {
            q_error("consul register failed: %s (服务继续启动，但不对外可见)", err);
        } else {
            q_info("registered: %s -> %s:%d", name, rcfg.host, port);
        }
    }

    /* ---- HTTP ---- */
    g_srv = q_http_new(port, io_threads);
    if (g_srv == NULL) {
        fprintf(stderr, "cannot create http server\n");
        return 1;
    }
    /*
     * 注册顺序有讲究：/user/search 必须排在 /user/:id 前面，
     * 否则 "search" 会被 :id 吃掉（路由是按下标顺序匹配的）。
     */
    q_http_route_health(g_srv);
    q_http_route(g_srv, "GET",    "/user/search",     h_search,   NULL);
    q_http_route(g_srv, "GET",    "/user/:id",        h_get_user, NULL);
    q_http_route(g_srv, "POST",   "/user",            h_add,      NULL);
    q_http_route(g_srv, "PUT",    "/user/:id",        h_update,   NULL);
    q_http_route(g_srv, "DELETE", "/user/:id",        h_remove,   NULL);
    q_http_route(g_srv, "GET",    "/demo/call-order", h_call_order, NULL);

    q_info("user-svc listening on %d", port);
    printf("user-svc listening on %d (io_threads=%d)\n", port, io_threads);

    q_http_run(g_srv);

    /* ---- 优雅退出 ---- */
    q_info("shutting down");
    if (g_reg != NULL) {
        q_reg_stop(g_reg);
        q_reg_free(g_reg);
        q_httpc_global_cleanup();
    }
    q_http_free(g_srv);
    q_mapper_free(g_app.mapper);
    q_dbp_free(g_app.pool);
    q_conf_free(cf);
    q_log_close();
    (void)g_stop;
    return 0;
}
