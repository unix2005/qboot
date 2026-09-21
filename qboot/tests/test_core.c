#include <q/core/ds.h>
#include <q/core/mem.h>
#include <q/core/str.h>
#include <q/core/threadpool.h>
#include <q/core/time.h>
#include <q/core/types.h>

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond, msg)                                              \
    do {                                                              \
        if (cond) { g_pass++; printf("  ok   %s\n", msg); }           \
        else      { g_fail++; printf("  FAIL %s (line %d)\n", msg, __LINE__); } \
    } while (0)

static void test_pool(void)
{
    printf("[pool]\n");
    q_pool_t *p = q_pool_new(256);
    CHECK(p != NULL, "pool new");

    char *a = q_pool_strdup(p, "hello qboot");
    CHECK(a != NULL && strcmp(a, "hello qboot") == 0, "pool strdup");

    void *b = q_pool_alloc(p, 100);
    CHECK(b != NULL, "pool alloc 100");
    CHECK(q_pool_used(p) > 0, "pool used > 0");

    /* 超过 block_size 的大块走单独分配 */
    void *big = q_pool_alloc(p, 4096);
    CHECK(big != NULL, "pool alloc big");

    /* 连续小分配 */
    for (int i = 0; i < 100; i++) {
        if (q_pool_alloc(p, 32) == NULL) { CHECK(0, "pool repeat alloc"); break; }
    }
    CHECK(1, "pool repeat alloc");

    size_t used1 = q_pool_used(p);
    q_pool_reset(p);
    CHECK(q_pool_used(p) == 0, "pool reset clears used");
    CHECK(used1 > 0, "pool had usage before reset");

    char *c = q_pool_strdup(p, "after reset");
    CHECK(c != NULL && strcmp(c, "after reset") == 0, "pool reusable after reset");

    q_pool_destroy(p);
    printf("\n");
}

static void test_str(void)
{
    printf("[str]\n");
    q_str_t s;
    CHECK(q_str_init(&s, 16) == 0, "str init");
    CHECK(q_str_append_cstr(&s, "abc") == 0, "str append cstr");
    CHECK(s.len == 3, "str len");
    CHECK(q_str_appendf(&s, "-%d-%s", 42, "xyz") == 0, "str appendf");
    CHECK(strcmp(s.data, "abc-42-xyz") == 0, "str content");

    /* 触发扩容 */
    for (int i = 0; i < 200; i++) q_str_appendf(&s, "%d", i);
    CHECK(s.len > 100, "str grow");

    q_str_clear(&s);
    CHECK(s.len == 0 && s.data[0] == '\0', "str clear");
    q_str_free(&s);

    char buf[32];
    snprintf(buf, sizeof(buf), "  trim me  ");
    CHECK(strcmp(q_str_trim(buf), "trim me") == 0, "str trim");
    CHECK(q_str_starts_with("qboot", "qb") == 1, "str starts_with");
    CHECK(q_str_ends_with("app.log", ".log") == 1, "str ends_with");
    CHECK(q_str_ieq("INFO", "info") == 1, "str ieq");
    printf("\n");
}

static void test_array(void)
{
    printf("[array]\n");
    q_array_t a;
    CHECK(q_array_init(&a, 2) == 0, "array init");
    for (int i = 0; i < 100; i++) {
        CHECK(q_array_push(&a, (void *)(intptr_t)(i + 1)) == 0, "array push");
        if (i > 0) break;                       /* 只测两次，避免刷屏 */
    }
    CHECK(a.len == 2, "array len after 2 pushes");
    CHECK((int)(intptr_t)q_array_get(&a, 1) == 2, "array get");
    CHECK((int)(intptr_t)q_array_pop(&a) == 2, "array pop");
    q_array_clear(&a);
    CHECK(a.len == 0, "array clear");
    q_array_free(&a);
    printf("\n");
}

static void test_hash(void)
{
    printf("[hash]\n");
    q_hash_t *h = q_hash_new(8);
    CHECK(h != NULL, "hash new");

    for (int i = 0; i < 1000; i++) {
        char k[32];
        snprintf(k, sizeof(k), "key-%d", i);
        CHECK(q_hash_set(h, k, (void *)(intptr_t)(i + 1)) == 0, "hash set");
        if (i > 0) break;
    }
    for (int i = 2; i < 1000; i++) {
        char k[32];
        snprintf(k, sizeof(k), "key-%d", i);
        q_hash_set(h, k, (void *)(intptr_t)(i + 1));
    }
    CHECK(q_hash_size(h) == 1000, "hash size 1000 (triggers grow)");

    void *v = q_hash_get(h, "key-500");
    CHECK(v != NULL && (int)(intptr_t)v == 501, "hash get");
    CHECK(q_hash_get(h, "nope") == NULL, "hash get missing");
    CHECK(q_hash_del(h, "key-500") == 0, "hash del");
    CHECK(q_hash_get(h, "key-500") == NULL, "hash get after del");
    CHECK(q_hash_size(h) == 999, "hash size after del");

    q_hash_free(h);
    printf("\n");
}

static void test_time(void)
{
    printf("[time]\n");
    int64_t ms = q_time_now_ms();
    CHECK(ms > 1700000000000LL, "time now ms sane");

    char buf[64];
    size_t n = q_time_format_ms(ms, buf, sizeof(buf));
    CHECK(n == 23, "time format_ms length");
    CHECK(buf[4] == '-' && buf[13] == ':', "time format shape");

    int64_t t0 = q_time_now_us();
    q_time_sleep_ms(5);
    int64_t t1 = q_time_now_us();
    CHECK(t1 - t0 >= 4000, "sleep 5ms elapsed");
    printf("       now=%s\n", buf);
    printf("\n");
}

static _Atomic int g_tp_hits = 0;
static void tp_task(void *arg)
{
    (void)arg;
    atomic_fetch_add(&g_tp_hits, 1);
}

static void test_threadpool(void)
{
    printf("[threadpool]\n");
    q_tp_t *tp = q_tp_new(4, 128);
    CHECK(tp != NULL, "tp new");
    CHECK(q_tp_threads(tp) == 4, "tp thread count");

    int submitted = 0;
    for (int i = 0; i < 1000; i++) {
        int rc = q_tp_submit(tp, tp_task, NULL);
        if (rc == Q_OK) submitted++;
        else if (rc == Q_ERR_BUSY) q_time_sleep_ms(1), i--;   /* 队列满就重试 */
    }
    q_tp_destroy(tp);                                          /* 内部会等队列清空 */
    CHECK(submitted == 1000, "tp submitted 1000");
    CHECK(atomic_load(&g_tp_hits) == 1000, "tp executed 1000");
    printf("\n");
}

int main(void)
{
    printf("== qboot core tests ==\n\n");
    test_pool();
    test_str();
    test_array();
    test_hash();
    test_time();
    test_threadpool();

    printf("passed=%d failed=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
