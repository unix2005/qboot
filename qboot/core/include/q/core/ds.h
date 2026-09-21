#ifndef Q_CORE_DS_H
#define Q_CORE_DS_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 动态指针数组 ---- */

typedef struct {
    void **items;
    size_t len;
    size_t cap;
} q_array_t;

int   q_array_init(q_array_t *a, size_t cap);
void  q_array_free(q_array_t *a);
int   q_array_push(q_array_t *a, void *p);
void *q_array_pop(q_array_t *a);
void *q_array_get(const q_array_t *a, size_t i);
void  q_array_clear(q_array_t *a);

/* ---- 字符串键哈希表（拥有 key 副本，不拥有 value） ---- */

typedef struct q_hash q_hash_t;
typedef void (*q_hash_iter_fn)(const char *key, void *val, void *ud);

q_hash_t *q_hash_new(size_t cap);
void      q_hash_free(q_hash_t *h);
int       q_hash_set(q_hash_t *h, const char *key, void *val);
void     *q_hash_get(const q_hash_t *h, const char *key);
int       q_hash_del(q_hash_t *h, const char *key);
size_t    q_hash_size(const q_hash_t *h);
void      q_hash_foreach(q_hash_t *h, q_hash_iter_fn fn, void *ud);

#ifdef __cplusplus
}
#endif

#endif /* Q_CORE_DS_H */
