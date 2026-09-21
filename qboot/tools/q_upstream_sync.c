/*
 * q_upstream_sync —— nginx 上游同步器。
 *
 * 作用等价于 consul-template，但零额外依赖：直接复用 libq_reg 拉 Consul，
 * 把健康实例写成 nginx 的 upstream 配置，变化了才落盘并 reload。
 *
 *   q_upstream_sync <consul_addr> <out_dir> [--reload-cmd <cmd>] <service> [service...]
 *
 * 例：
 *   q_upstream_sync 127.0.0.1:8500 /usr/local/etc/nginx/upstream \
 *       --reload-cmd "nginx -s reload" user-svc order-svc
 *
 * 生成的文件形如 <out_dir>/user-svc.conf：
 *   upstream user-svc {
 *       server 127.0.0.1:8081 max_fails=3 fail_timeout=10s;
 *       server 127.0.0.1:8082 max_fails=3 fail_timeout=10s;
 *       keepalive 64;
 *   }
 *
 * nginx 主配置里 include 这个目录即可：
 *   http { include upstream/*.conf; server { location /api/user/ { proxy_pass http://user-svc/; } } }
 */

#include <q/log.h>
#include <q/reg.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAX_SERVICES 32
#define MAX_ENDPOINTS 64
#define POLL_SECS 3

static int read_file(const char *path, char *buf, size_t cap)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) return -1;
    size_t n = fread(buf, 1, cap - 1, f);
    buf[n] = '\0';
    fclose(f);
    return (int)n;
}

static int write_file(const char *path, const char *content)
{
    FILE *f = fopen(path, "wb");
    if (f == NULL) return -1;
    fputs(content, f);
    fclose(f);
    return 0;
}

int main(int argc, char **argv)
{
    const char *consul = NULL;
    const char *outdir = NULL;
    const char *reload = NULL;
    const char *svcs[MAX_SERVICES];
    int         nsvcs = 0;
    char        err[256];

    /* ---- 参数 ---- */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--reload-cmd") == 0 && i + 1 < argc) {
            reload = argv[++i];
        } else if (consul == NULL) {
            consul = argv[i];
        } else if (outdir == NULL) {
            outdir = argv[i];
        } else if (nsvcs < MAX_SERVICES) {
            svcs[nsvcs++] = argv[i];
        }
    }
    if (consul == NULL || outdir == NULL || nsvcs == 0) {
        fprintf(stderr,
                "usage: %s <consul_addr> <out_dir> [--reload-cmd <cmd>] "
                "<service> [service...]\n", argv[0]);
        return 2;
    }

    q_log_init("logs", "upstream_sync", Q_LOG_INFO);
    q_httpc_global_init();

    q_reg_cfg_t cfg = {
        .consul_addr  = consul,
        .service      = svcs[0],       /* 同步器自己不注册，这里只借 cfg 结构 */
        .ttl_secs     = 10,
        .refresh_secs = POLL_SECS
    };
    q_reg_t *r = q_reg_new(&cfg);
    if (r == NULL) {
        fprintf(stderr, "cannot init registry\n");
        return 1;
    }
    for (int i = 0; i < nsvcs; i++) q_reg_watch(r, svcs[i]);

    printf("q_upstream_sync: consul=%s out=%s services=%d\n",
           consul, outdir, nsvcs);
    for (int i = 0; i < nsvcs; i++) printf("  - %s\n", svcs[i]);

    int changed_total = 0;

    while (1) {
        for (int i = 0; i < nsvcs; i++) {
            q_endpoint_t eps[MAX_ENDPOINTS];
            char path[512], content[8192], old[8192];
            int  n;

            if (q_reg_refresh(r, svcs[i]) != Q_OK) continue;
            n = q_reg_endpoints(r, svcs[i], eps, MAX_ENDPOINTS);

            snprintf(path, sizeof(path), "%s/%s.conf", outdir, svcs[i]);

            int off = snprintf(content, sizeof(content), "upstream %s {\n", svcs[i]);
            for (int k = 0; k < n; k++) {
                off += snprintf(content + off, sizeof(content) - (size_t)off,
                                "    server %s:%d max_fails=3 fail_timeout=10s;\n",
                                eps[k].host, eps[k].port);
            }
            if (n == 0) {
                /*
                 * nginx 不允许 upstream 里一个 server 都没有，
                 * 塞一个 down 的占位，配置才合法。
                 */
                off += snprintf(content + off, sizeof(content) - (size_t)off,
                                "    server 127.0.0.1:1 down;\n");
            }
            snprintf(content + off, sizeof(content) - (size_t)off,
                     "    keepalive 64;\n}\n");

            if (read_file(path, old, sizeof(old)) >= 0 && strcmp(old, content) == 0) {
                continue;              /* 没变化就不写盘、不 reload */
            }
            if (write_file(path, content) != 0) {
                q_error("cannot write %s", path);
                continue;
            }
            changed_total++;
            q_info("upstream %s updated: %d instance(s)", svcs[i], n);

            if (reload != NULL) {
                int rc = system(reload);
                if (rc != 0) q_warn("reload command failed: %s (rc=%d)", reload, rc);
            }
        }
        sleep(POLL_SECS);
    }

    /* 实际靠 SIGINT/SIGTERM 退出，这里只是形式上收尾 */
    q_reg_free(r);
    q_httpc_global_cleanup();
    q_log_close();
    (void)err;
    (void)changed_total;
    return 0;
}
