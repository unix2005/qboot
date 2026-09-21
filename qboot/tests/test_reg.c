/*
 * libq_reg 端到端自测。
 * 不依赖真实 Consul：用 libq_http 起一个假 Consul agent（实现它用到的 4 个接口）。
 */

#include <q/http.h>
#include <q/httpc.h>
#include <q/log.h>
#include <q/reg.h>

#include <jansson.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

#define FAKE_PORT 18500
#define FAKE_ADDR "127.0.0.1:18500"

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond, fmt, ...)                                              \
    do {                                                                   \
        if (cond) {                                                        \
            g_pass++;                                                      \
            printf("  [ok]   " fmt "\n", ##__VA_ARGS__);                   \
        } else {                                                           \
            g_fail++;                                                      \
            printf("  [FAIL] " fmt "\n", ##__VA_ARGS__);                   \
        }                                                                  \
    } while (0)

/* ---------------- 假 Consul ---------------- */

typedef struct {
    char id[192];
    char name[128];
    char addr[128];
    int  port;
} fake_svc_t;

#define FAKE_MAX 16
static fake_svc_t   g_svcs[FAKE_MAX];
static int          g_nsvcs;
static int          g_registers;
static int          g_deregisters;
static int          g_heartbeats;
static char         g_last_dereg[192];
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;

static int h_register(q_req_t *req, q_resp_t *resp, void *ud)
{
    (void)ud;
    const char *body = q_req_body(req);
    json_error_t e;
    json_t *j = json_loads(body ? body : "", 0, &e);

    if (j == NULL) return q_resp_text(resp, 400, "{\"error\":\"bad json\"}");

    pthread_mutex_lock(&g_mu);
    int slot = -1;
    for (int i = 0; i < g_nsvcs; i++) {
        const char *id = json_string_value(json_object_get(j, "ID"));
        if (id != NULL && strcmp(g_svcs[i].id, id) == 0) { slot = i; break; }
    }
    if (slot < 0 && g_nsvcs < FAKE_MAX) slot = g_nsvcs++;

    if (slot >= 0) {
        const char *s;
        s = json_string_value(json_object_get(j, "ID"));
        snprintf(g_svcs[slot].id, sizeof(g_svcs[slot].id), "%s", s ? s : "");
        s = json_string_value(json_object_get(j, "Name"));
        snprintf(g_svcs[slot].name, sizeof(g_svcs[slot].name), "%s", s ? s : "");
        s = json_string_value(json_object_get(j, "Address"));
        snprintf(g_svcs[slot].addr, sizeof(g_svcs[slot].addr), "%s", s ? s : "");
        json_t *p = json_object_get(j, "Port");
        g_svcs[slot].port = json_is_integer(p) ? (int)json_integer_value(p) : 0;
        g_registers++;
    }
    pthread_mutex_unlock(&g_mu);

    json_decref(j);
    return q_resp_text(resp, 200, "");
}

static int h_pass(q_req_t *req, q_resp_t *resp, void *ud)
{
    (void)ud;
    (void)req;
    pthread_mutex_lock(&g_mu);
    g_heartbeats++;
    pthread_mutex_unlock(&g_mu);
    return q_resp_text(resp, 200, "ok");
}

static int h_dereg(q_req_t *req, q_resp_t *resp, void *ud)
{
    (void)ud;
    const char *id = q_req_param(req, "id");

    pthread_mutex_lock(&g_mu);
    g_deregisters++;
    snprintf(g_last_dereg, sizeof(g_last_dereg), "%s", id ? id : "");
    for (int i = 0; i < g_nsvcs; i++) {
        if (strcmp(g_svcs[i].id, id ? id : "") == 0) {
            for (int k = i; k < g_nsvcs - 1; k++) g_svcs[k] = g_svcs[k + 1];
            g_nsvcs--;
            break;
        }
    }
    pthread_mutex_unlock(&g_mu);
    return q_resp_text(resp, 200, "");
}

static int h_health(q_req_t *req, q_resp_t *resp, void *ud)
{
    (void)ud;
    const char *name = q_req_param(req, "name");

    pthread_mutex_lock(&g_mu);
    json_t *arr = json_array();
    for (int i = 0; i < g_nsvcs; i++) {
        if (name == NULL || strcmp(g_svcs[i].name, name) != 0) continue;

        json_t *item = json_object();
        json_t *svc  = json_object();
        json_object_set_new(svc, "Address", json_string(g_svcs[i].addr));
        json_object_set_new(svc, "Port",    json_integer(g_svcs[i].port));
        json_object_set_new(item, "Service", svc);

        json_t *node = json_object();
        json_object_set_new(node, "Address", json_string("10.0.0.1"));
        json_object_set_new(item, "Node", node);

        json_array_append_new(arr, item);
    }
    pthread_mutex_unlock(&g_mu);

    char *s = json_dumps(arr, JSON_COMPACT);
    json_decref(arr);
    int rc = q_resp_json(resp, 200, s ? s : "[]");
    free(s);
    return rc;
}

/* q_reg_call 打过来落在这里 */
static int h_echo(q_req_t *req, q_resp_t *resp, void *ud)
{
    (void)ud; (void)req;
    return q_resp_json(resp, 200, "{\"from\":\"fake-backend\"}");
}

static q_http_t *g_srv;

static void *srv_thread(void *ud)
{
    (void)ud;
    q_http_run(g_srv);
    return NULL;
}

static int wait_port_up(int port, int timeout_ms)
{
    for (int i = 0; i < timeout_ms / 20; i++) {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd >= 0) {
            struct sockaddr_in a;
            memset(&a, 0, sizeof(a));
            a.sin_family = AF_INET;
            a.sin_port   = htons((uint16_t)port);
            a.sin_addr.s_addr = inet_addr("127.0.0.1");
            int ok = connect(fd, (struct sockaddr *)&a, sizeof(a));
            close(fd);
            if (ok == 0) return 1;
        }
        usleep(20 * 1000);
    }
    return 0;
}

static void seed(const char *id, const char *name, const char *addr, int port)
{
    pthread_mutex_lock(&g_mu);
    if (g_nsvcs < FAKE_MAX) {
        int i = g_nsvcs++;
        snprintf(g_svcs[i].id,   sizeof(g_svcs[i].id),   "%s", id);
        snprintf(g_svcs[i].name, sizeof(g_svcs[i].name), "%s", name);
        snprintf(g_svcs[i].addr, sizeof(g_svcs[i].addr), "%s", addr);
        g_svcs[i].port = port;
    }
    pthread_mutex_unlock(&g_mu);
}

int main(void)
{
    char err[256];

    q_log_init("logs", "test_reg", Q_LOG_WARN);
    q_httpc_global_init();

    /* ---- 起假 Consul ---- */
    g_srv = q_http_new(FAKE_PORT, 1);
    if (g_srv == NULL) { printf("cannot start fake consul\n"); return 1; }
    q_http_route(g_srv, "PUT",  "/v1/agent/service/register",      h_register, NULL);
    q_http_route(g_srv, "PUT",  "/v1/agent/check/pass/:id",        h_pass,     NULL);
    q_http_route(g_srv, "PUT",  "/v1/agent/service/deregister/:id", h_dereg,   NULL);
    q_http_route(g_srv, "GET",  "/v1/health/service/:name",        h_health,   NULL);
    q_http_route(g_srv, "GET",  "/echo",                           h_echo,     NULL);

    pthread_t th;
    pthread_create(&th, NULL, srv_thread, NULL);
    if (!wait_port_up(FAKE_PORT, 3000)) {
        printf("fake consul did not come up\n");
        return 1;
    }
    printf("fake consul listening on %d\n\n", FAKE_PORT);

    /* ---- 1. 注册 ---- */
    printf("== 1. 注册到 Consul ==\n");
    const char *tags[] = { "v1", "zone-a" };
    q_reg_cfg_t cfg = {
        .consul_addr  = FAKE_ADDR,
        .service      = "user-svc",
        .host         = "127.0.0.1",
        .port         = 8081,
        .tags         = tags,
        .ntags        = 2,
        .ttl_secs     = 2,
        .refresh_secs = 1
    };
    q_reg_t *r = q_reg_new(&cfg);
    CHECK(r != NULL, "q_reg_new");

    int rc = q_reg_register(r, err, sizeof(err));
    CHECK(rc == Q_OK, "register rc=%d %s%s", rc, rc ? "err=" : "", rc ? err : "");
    CHECK(g_registers == 1, "consul 收到注册请求 %d 次", g_registers);
    CHECK(g_nsvcs == 1 && strcmp(g_svcs[0].name, "user-svc") == 0 &&
          g_svcs[0].port == 8081,
          "注册内容: name=%s port=%d", g_svcs[0].name, g_svcs[0].port);
    CHECK(g_nsvcs == 1 && strcmp(g_svcs[0].id, "user-svc-127.0.0.1-8081") == 0,
          "实例 id 自动生成: %s", g_svcs[0].id);

    /* ---- 2. 发现 ---- */
    printf("\n== 2. 服务发现 ==\n");
    seed("user-svc-2", "user-svc", "127.0.0.1", 8082);
    seed("order-1",    "order-svc", "127.0.0.1", 9091);

    rc = q_reg_refresh(r, "user-svc");
    CHECK(rc == Q_OK, "refresh rc=%d", rc);

    q_endpoint_t eps[16];
    int n = q_reg_endpoints(r, "user-svc", eps, 16);
    CHECK(n == 2, "user-svc 健康实例 = %d (expect 2)", n);
    if (n == 2) {
        CHECK((eps[0].port == 8081 && eps[1].port == 8082) ||
              (eps[0].port == 8082 && eps[1].port == 8081),
              "端口分别是 8081/8082: %d,%d", eps[0].port, eps[1].port);
    }

    /* 未刷新的服务拿不到实例 */
    CHECK(q_reg_endpoints(r, "order-svc", eps, 16) == 0,
          "未 watch 的服务列表为空");

    /* ---- 3. 轮询负载均衡 ---- */
    printf("\n== 3. 轮询 ==\n");
    q_endpoint_t a, b, c;
    CHECK(q_reg_pick(r, "user-svc", &a) == Q_OK, "pick 1");
    CHECK(q_reg_pick(r, "user-svc", &b) == Q_OK, "pick 2");
    CHECK(q_reg_pick(r, "user-svc", &c) == Q_OK, "pick 3");
    CHECK(a.port != b.port, "连续两次 pick 落到不同实例: %d -> %d", a.port, b.port);
    CHECK(a.port == c.port, "第三次回到第一个实例（轮询）: %d", c.port);

    q_endpoint_t bad;
    CHECK(q_reg_pick(r, "no-such-svc", &bad) == Q_ERR_NOTFOUND,
          "未知服务返回 Q_ERR_NOTFOUND");

    /* ---- 4. 服务间调用 ---- */
    printf("\n== 4. 服务间调用 ==\n");
    seed("echo-1", "echo-svc", "127.0.0.1", FAKE_PORT);
    seed("echo-2", "echo-svc", "127.0.0.1", FAKE_PORT);
    q_reg_watch(r, "echo-svc");
    CHECK(q_reg_refresh(r, "echo-svc") == Q_OK, "refresh echo-svc");

    q_httpc_resp_t resp;
    rc = q_reg_call(r, "echo-svc", "/echo", "GET", NULL, &resp, err, sizeof(err));
    CHECK(rc == Q_OK, "call rc=%d %s%s", rc, rc ? "err=" : "", rc ? err : "");
    if (rc == Q_OK) {
        CHECK(resp.status == 200, "HTTP %ld", resp.status);
        CHECK(resp.body != NULL && strstr(resp.body, "fake-backend") != NULL,
              "响应体: %s", resp.body ? resp.body : "(null)");
        q_httpc_resp_free(&resp);
    }

    rc = q_reg_call(r, "ghost-svc", "/x", "GET", NULL, &resp, err, sizeof(err));
    CHECK(rc == Q_ERR_NOTFOUND, "无实例时返回 Q_ERR_NOTFOUND (rc=%d)", rc);

    /* ---- 5. 心跳 ---- */
    printf("\n== 5. TTL 心跳 ==\n");
    int hb_before = g_heartbeats;
    rc = q_reg_start(r, err, sizeof(err));
    CHECK(rc == Q_OK, "q_reg_start rc=%d %s%s", rc, rc ? "err=" : "", rc ? err : "");
    sleep(5);
    CHECK(g_heartbeats > hb_before, "心跳次数 %d -> %d", hb_before, g_heartbeats);
    CHECK(g_heartbeats - hb_before >= 2, "5 秒内至少心跳 2 次（ttl=2s）: %d",
          g_heartbeats - hb_before);

    q_reg_stat_t st;
    q_reg_stat(r, &st);
    CHECK(st.registered == 1, "stat.registered=%d", st.registered);
    CHECK(st.watched >= 2, "stat.watched=%d (user-svc + echo-svc)", st.watched);
    printf("     stat: hb=%d refresh_ok=%d refresh_fail=%d\n",
           st.heartbeats, st.refresh_ok, st.refresh_fail);

    /* ---- 6. 停止并注销 ---- */
    printf("\n== 6. 停止并注销 ==\n");
    int dereg_before = g_deregisters;
    q_reg_stop(r);
    CHECK(g_deregisters == dereg_before + 1, "注销请求 %d 次", g_deregisters);
    CHECK(strcmp(g_last_dereg, "user-svc-127.0.0.1-8081") == 0,
          "注销的是自己: %s", g_last_dereg);
    CHECK(g_nsvcs == 4, "Consul 上只剩 4 个种子实例 (expect 4, got %d)", g_nsvcs);

    q_reg_free(r);

    q_http_stop(g_srv);
    pthread_join(th, NULL);
    q_http_free(g_srv);
    q_httpc_global_cleanup();
    q_log_close();

    printf("\n----------------------------------------\n");
    printf("reg test: pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
