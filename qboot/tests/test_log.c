#include <q/log.h>

#include <q/core/time.h>
#include <q/core/types.h>

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LOGDIR       "/tmp/qboot-log-test"
#define NTHREADS     8
#define PER_THREAD   2000
#define BENCH_TOTAL  1000000

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond, msg)                                              \
    do {                                                              \
        if (cond) { g_pass++; printf("  ok   %s\n", msg); }           \
        else      { g_fail++; printf("  FAIL %s (line %d)\n", msg, __LINE__); } \
    } while (0)

static int g_per_thread = PER_THREAD;

static void *writer(void *arg)
{
    int id = (int)(intptr_t)arg;
    char trace[32];
    snprintf(trace, sizeof(trace), "trace-%08d", id);
    q_log_set_ctx(trace, NULL);

    for (int i = 0; i < g_per_thread; i++) {
        q_info("thread=%d seq=%d hello qboot async log", id, i);
        if (i % 500 == 0) q_error_cat("sql", "slow sql %dms: SELECT * FROM t_user", i);
    }
    return NULL;
}

/* 校验：每行必须以时间戳开头、以换行结尾，且带 tid —— 任一不满足就是行撕裂 */
static int verify(const char *path, int *lines, int *bad, char *sample, size_t sample_cap)
{
    FILE *fp = fopen(path, "r");
    if (fp == NULL) return -1;

    char buf[4096];
    *lines = 0;
    *bad   = 0;

    while (fgets(buf, sizeof(buf), fp) != NULL) {
        size_t n = strlen(buf);
        if (n == 0) continue;
        (*lines)++;

        int ok = 1;
        if (n < 40) ok = 0;                                   /* 前缀+正文下限 */
        if (!(buf[0] >= '0' && buf[0] <= '9' && buf[4] == '-')) ok = 0;
        if (buf[n - 1] != '\n') ok = 0;
        if (strstr(buf, "[tid:") == NULL) ok = 0;

        if (!ok) (*bad)++;
        else if (*lines == 1 && sample != NULL) snprintf(sample, sample_cap, "%s", buf);
    }
    fclose(fp);
    return 0;
}

static int run_concurrent(void)
{
    pthread_t th[NTHREADS];

    for (int i = 0; i < NTHREADS; i++) {
        pthread_create(&th[i], NULL, writer, (void *)(intptr_t)i);
    }
    for (int i = 0; i < NTHREADS; i++) pthread_join(th[i], NULL);

    q_log_flush();

    char path[512];
    snprintf(path, sizeof(path), "%s/test.log", LOGDIR);

    int lines = 0, bad = 0;
    char sample[512] = "";
    if (verify(path, &lines, &bad, sample, sizeof(sample)) != 0) {
        CHECK(0, "open log file");
        return 1;
    }

    printf("  lines=%d bad=%d\n", lines, bad);
    printf("  sample: %s", sample[0] ? sample : "(none)\n");
    CHECK(bad == 0, "no interleaved lines");
    CHECK(lines >= NTHREADS * PER_THREAD, "all lines written");

    q_log_stat_t st;
    q_log_stat(&st);
    printf("  written=%llu dropped=%llu sync=%llu\n",
           st.written, st.dropped, st.sync_writes);
    CHECK(st.dropped == 0, "no dropped under normal load");
    return 0;
}

static void run_bench(void)
{
    printf("== log bench: %d messages ==\n", BENCH_TOTAL);

    int nthreads = 4;
    g_per_thread = BENCH_TOTAL / nthreads;

    pthread_t th[8];
    int64_t   t0 = q_time_now_ms();

    for (int i = 0; i < nthreads; i++) {
        pthread_create(&th[i], NULL, writer, (void *)(intptr_t)i);
    }
    for (int i = 0; i < nthreads; i++) pthread_join(th[i], NULL);

    int64_t submit_ms = q_time_now_ms() - t0;

    q_log_flush();
    int64_t total_ms = q_time_now_ms() - t0;

    q_log_stat_t st;
    q_log_stat(&st);

    double secs = (double)(total_ms > 0 ? total_ms : 1) / 1000.0;
    printf("threads=%d per=%d\n", nthreads, g_per_thread);
    printf("submit only : %lld ms\n", (long long)submit_ms);
    printf("with flush  : %lld ms  (%.2f M msg/s)\n",
           (long long)total_ms, (double)st.written / 1000000.0 / secs);
    printf("written=%llu dropped=%llu sync=%llu\n", st.written, st.dropped, st.sync_writes);
}

int main(int argc, char **argv)
{
    (void)system("rm -rf " LOGDIR);

    if (q_log_init(LOGDIR, "test", Q_LOG_INFO) != Q_OK) {
        printf("log init failed\n");
        return 1;
    }
    q_log_add_cat("sql", 1024 * 1024, 3);
    q_log_add_cat("access", 0, 0);

    if (argc > 1 && strcmp(argv[1], "bench") == 0) {
        run_bench();
        q_log_close();
        return 0;
    }

    printf("== qboot log tests ==\n\n");

    /* 级别过滤 */
    q_log_stat_t before, after;
    q_log_stat(&before);
    q_debug("this debug line must be filtered out");
    q_time_sleep_ms(20);
    q_log_stat(&after);
    CHECK(after.written == before.written, "debug filtered by level=info");

    q_log_set_level(Q_LOG_DEBUG);
    q_debug("debug line after lowering level");
    q_log_set_level(Q_LOG_INFO);

    printf("\n[concurrent]\n");
    run_concurrent();

    printf("\n[ctx]\n");
    q_log_set_ctx("4bf92f3577b34da6", "req-0001");
    q_info("request scoped log line");
    q_log_clear_ctx();

    q_log_close();

    printf("\nlog dir: %s\n", LOGDIR);
    printf("passed=%d failed=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
