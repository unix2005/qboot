#include <q/conf.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond, msg)                                              \
    do {                                                              \
        if (cond) { g_pass++; printf("  ok   %s\n", msg); }           \
        else      { g_fail++; printf("  FAIL %s (line %d)\n", msg, __LINE__); } \
    } while (0)

#define CONF_PATH "/tmp/qboot-conf-test.ini"

static const char *g_ini =
    "; qboot 示例配置\n"
    "[server]\n"
    "name = demo-svc\n"
    "port = 8081\n"
    "io_threads = 4\n"
    "\n"
    "[log]\n"
    "dir   = /tmp/qboot-logs\n"
    "level = debug\n"
    "async = true\n"
    "ratio = 0.75\n"
    "\n"
    "[db]\n"
    "url = mysql://root:pwd@127.0.0.1:3306/demo\n"
    "pool = 8\n";

int main(void)
{
    FILE *fp = fopen(CONF_PATH, "w");
    if (fp == NULL) {
        printf("cannot write %s\n", CONF_PATH);
        return 1;
    }
    fwrite(g_ini, 1, strlen(g_ini), fp);
    fclose(fp);

    printf("== qboot conf tests ==\n\n");

    q_conf_t *c = q_conf_load(CONF_PATH);
    CHECK(c != NULL, "conf load");

    CHECK(strcmp(q_conf_get(c, "server", "name", "x"), "demo-svc") == 0, "get string");
    CHECK(q_conf_get_int(c, "server", "port", 0) == 8081, "get int");
    CHECK(q_conf_get_int(c, "server", "io_threads", 0) == 4, "get int 2");
    CHECK(q_conf_get_bool(c, "log", "async", 0) == 1, "get bool true");
    CHECK(q_conf_get_double(c, "log", "ratio", 0.0) > 0.74, "get double");
    CHECK(q_conf_has(c, "db", "url") == 1, "has key");
    CHECK(q_conf_has(c, "db", "nope") == 0, "has missing key");
    CHECK(strcmp(q_conf_get(c, "db", "missing", "fallback"), "fallback") == 0, "default value");
    CHECK(q_conf_get_int(c, "db", "missing", 42) == 42, "default int");

    /* 环境变量覆盖：Q_SERVER_PORT */
    setenv("Q_SERVER_PORT", "9999", 1);
    CHECK(q_conf_get_int(c, "server", "port", 0) == 9999, "env override Q_SERVER_PORT");
    unsetenv("Q_SERVER_PORT");
    CHECK(q_conf_get_int(c, "server", "port", 0) == 8081, "env unset falls back");

    q_conf_free(c);
    remove(CONF_PATH);

    printf("\npassed=%d failed=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
