#include <q/core/mem.h>
#include <q/core/types.h>

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

/* ---- 进程级分配统计 ---- */

static _Atomic size_t g_bytes = 0;
static _Atomic size_t g_count = 0;

size_t q_mem_allocated(void) { return atomic_load_explicit(&g_bytes, memory_order_relaxed); }
size_t q_mem_alloc_count(void) { return atomic_load_explicit(&g_count, memory_order_relaxed); }

static void mem_account(size_t n)
{
    atomic_fetch_add_explicit(&g_bytes, n, memory_order_relaxed);
    atomic_fetch_add_explicit(&g_count, 1, memory_order_relaxed);
}

static void mem_unaccount(size_t n)
{
    atomic_fetch_sub_explicit(&g_bytes, n, memory_order_relaxed);
    atomic_fetch_sub_explicit(&g_count, 1, memory_order_relaxed);
}

/* ---- 内存池 ---- */

typedef struct q_block {
    struct q_block *next;
    size_t cap;
    size_t used;
} q_block_t;

typedef struct q_big {
    struct q_big *next;
    size_t size;
} q_big_t;

struct q_pool {
    size_t     block_size;
    q_block_t *blocks;
    q_big_t   *bigs;
    size_t     used;
    size_t     total;
};

#define Q_BLOCK_HDR Q_ALIGN_UP(sizeof(q_block_t), 8)
#define Q_BIG_HDR   Q_ALIGN_UP(sizeof(q_big_t), 8)

q_pool_t *q_pool_new(size_t block_size)
{
    if (block_size == 0) block_size = 4096;
    if (block_size < 256) block_size = 256;

    q_pool_t *p = malloc(sizeof(q_pool_t));
    if (p == NULL) return NULL;
    mem_account(sizeof(q_pool_t));

    p->block_size = block_size;
    p->blocks     = NULL;
    p->bigs       = NULL;
    p->used       = 0;
    p->total      = sizeof(q_pool_t);
    return p;
}

void q_pool_destroy(q_pool_t *p)
{
    if (p == NULL) return;

    q_block_t *b = p->blocks;
    while (b != NULL) {
        q_block_t *next = b->next;
        mem_unaccount(Q_BLOCK_HDR + b->cap);
        free(b);
        b = next;
    }
    q_big_t *g = p->bigs;
    while (g != NULL) {
        q_big_t *next = g->next;
        mem_unaccount(Q_BIG_HDR + g->size);
        free(g);
        g = next;
    }
    mem_unaccount(sizeof(q_pool_t));
    free(p);
}

void *q_pool_alloc(q_pool_t *p, size_t n)
{
    if (p == NULL || n == 0) return NULL;

    n = Q_ALIGN_UP(n, 8);

    if (n > p->block_size) {
        q_big_t *g = malloc(Q_BIG_HDR + n);
        if (g == NULL) return NULL;
        mem_account(Q_BIG_HDR + n);
        g->size = n;
        g->next = p->bigs;
        p->bigs = g;
        p->used  += n;
        p->total += Q_BIG_HDR + n;
        return (char *)g + Q_BIG_HDR;
    }

    if (p->blocks == NULL || (p->blocks->cap - p->blocks->used) < n) {
        size_t cap = p->block_size;
        q_block_t *b = malloc(Q_BLOCK_HDR + cap);
        if (b == NULL) return NULL;
        mem_account(Q_BLOCK_HDR + cap);
        b->cap  = cap;
        b->used = 0;
        b->next = p->blocks;
        p->blocks = b;
        p->total += Q_BLOCK_HDR + cap;
    }

    char *ptr = (char *)p->blocks + Q_BLOCK_HDR + p->blocks->used;
    p->blocks->used += n;
    p->used += n;
    return ptr;
}

void *q_pool_calloc(q_pool_t *p, size_t n)
{
    void *ptr = q_pool_alloc(p, n);
    if (ptr != NULL) memset(ptr, 0, n);
    return ptr;
}

char *q_pool_strdup(q_pool_t *p, const char *s)
{
    if (s == NULL) return NULL;
    return q_pool_strndup(p, s, strlen(s));
}

char *q_pool_strndup(q_pool_t *p, const char *s, size_t n)
{
    if (s == NULL) return NULL;
    char *dst = q_pool_alloc(p, n + 1);
    if (dst == NULL) return NULL;
    memcpy(dst, s, n);
    dst[n] = '\0';
    return dst;
}

void q_pool_reset(q_pool_t *p)
{
    if (p == NULL) return;

    for (q_block_t *b = p->blocks; b != NULL; b = b->next) b->used = 0;

    q_big_t *g = p->bigs;
    while (g != NULL) {
        q_big_t *next = g->next;
        mem_unaccount(Q_BIG_HDR + g->size);
        free(g);
        g = next;
    }
    p->bigs = NULL;
    p->used = 0;
}

size_t q_pool_used(q_pool_t *p)  { return p ? p->used  : 0; }
size_t q_pool_total(q_pool_t *p) { return p ? p->total : 0; }
