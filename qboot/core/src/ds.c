#include <q/core/ds.h>

#include <stdlib.h>
#include <string.h>

/* ---------------- 动态数组 ---------------- */

int q_array_init(q_array_t *a, size_t cap)
{
    if (cap == 0) cap = 8;
    a->items = malloc(cap * sizeof(void *));
    if (a->items == NULL) {
        a->len = a->cap = 0;
        return -1;
    }
    a->len = 0;
    a->cap = cap;
    return 0;
}

void q_array_free(q_array_t *a)
{
    if (a == NULL) return;
    free(a->items);
    a->items = NULL;
    a->len = a->cap = 0;
}

int q_array_push(q_array_t *a, void *p)
{
    if (a->len == a->cap) {
        size_t cap = a->cap ? a->cap * 2 : 8;
        void **items = realloc(a->items, cap * sizeof(void *));
        if (items == NULL) return -1;
        a->items = items;
        a->cap   = cap;
    }
    a->items[a->len++] = p;
    return 0;
}

void *q_array_pop(q_array_t *a)
{
    if (a->len == 0) return NULL;
    return a->items[--a->len];
}

void *q_array_get(const q_array_t *a, size_t i)
{
    return (i < a->len) ? a->items[i] : NULL;
}

void q_array_clear(q_array_t *a)
{
    a->len = 0;
}

/* ---------------- 哈希表 ---------------- */

typedef struct q_hash_node {
    struct q_hash_node *next;
    char               *key;
    void               *val;
    size_t              hash;
} q_hash_node_t;

struct q_hash {
    q_hash_node_t **buckets;
    size_t          nbuckets;
    size_t          size;
};

static size_t hash_str(const char *s)
{
    size_t h = 1469598103934665603ULL;          /* FNV-1a */
    while (*s != '\0') {
        h ^= (unsigned char)*s++;
        h *= 1099511628211ULL;
    }
    return h;
}

q_hash_t *q_hash_new(size_t cap)
{
    if (cap < 8) cap = 8;
    q_hash_t *h = malloc(sizeof(q_hash_t));
    if (h == NULL) return NULL;
    h->buckets = calloc(cap, sizeof(q_hash_node_t *));
    if (h->buckets == NULL) {
        free(h);
        return NULL;
    }
    h->nbuckets = cap;
    h->size = 0;
    return h;
}

void q_hash_free(q_hash_t *h)
{
    if (h == NULL) return;
    for (size_t i = 0; i < h->nbuckets; i++) {
        q_hash_node_t *n = h->buckets[i];
        while (n != NULL) {
            q_hash_node_t *next = n->next;
            free(n->key);
            free(n);
            n = next;
        }
    }
    free(h->buckets);
    free(h);
}

static int hash_grow(q_hash_t *h)
{
    size_t nb = h->nbuckets * 2;
    q_hash_node_t **buckets = calloc(nb, sizeof(q_hash_node_t *));
    if (buckets == NULL) return -1;

    for (size_t i = 0; i < h->nbuckets; i++) {
        q_hash_node_t *n = h->buckets[i];
        while (n != NULL) {
            q_hash_node_t *next = n->next;
            size_t idx = n->hash & (nb - 1);
            n->next = buckets[idx];
            buckets[idx] = n;
            n = next;
        }
    }
    free(h->buckets);
    h->buckets  = buckets;
    h->nbuckets = nb;
    return 0;
}

int q_hash_set(q_hash_t *h, const char *key, void *val)
{
    if (h == NULL || key == NULL) return -1;

    size_t hash = hash_str(key);
    size_t idx  = hash & (h->nbuckets - 1);

    for (q_hash_node_t *n = h->buckets[idx]; n != NULL; n = n->next) {
        if (n->hash == hash && strcmp(n->key, key) == 0) {
            n->val = val;
            return 0;
        }
    }

    if (h->size + 1 > h->nbuckets * 3 / 4 && hash_grow(h) != 0) return -1;

    idx = hash & (h->nbuckets - 1);
    q_hash_node_t *n = malloc(sizeof(q_hash_node_t));
    if (n == NULL) return -1;
    n->key = strdup(key);
    if (n->key == NULL) {
        free(n);
        return -1;
    }
    n->val  = val;
    n->hash = hash;
    n->next = h->buckets[idx];
    h->buckets[idx] = n;
    h->size++;
    return 0;
}

void *q_hash_get(const q_hash_t *h, const char *key)
{
    if (h == NULL || key == NULL) return NULL;

    size_t hash = hash_str(key);
    size_t idx  = hash & (h->nbuckets - 1);

    for (q_hash_node_t *n = h->buckets[idx]; n != NULL; n = n->next) {
        if (n->hash == hash && strcmp(n->key, key) == 0) return n->val;
    }
    return NULL;
}

int q_hash_del(q_hash_t *h, const char *key)
{
    if (h == NULL || key == NULL) return -1;

    size_t hash = hash_str(key);
    size_t idx  = hash & (h->nbuckets - 1);
    q_hash_node_t **pp = &h->buckets[idx];

    while (*pp != NULL) {
        q_hash_node_t *n = *pp;
        if (n->hash == hash && strcmp(n->key, key) == 0) {
            *pp = n->next;
            free(n->key);
            free(n);
            h->size--;
            return 0;
        }
        pp = &n->next;
    }
    return -1;
}

size_t q_hash_size(const q_hash_t *h) { return h ? h->size : 0; }

void q_hash_foreach(q_hash_t *h, q_hash_iter_fn fn, void *ud)
{
    if (h == NULL || fn == NULL) return;
    for (size_t i = 0; i < h->nbuckets; i++) {
        for (q_hash_node_t *n = h->buckets[i]; n != NULL; n = n->next) {
            fn(n->key, n->val, ud);
        }
    }
}
