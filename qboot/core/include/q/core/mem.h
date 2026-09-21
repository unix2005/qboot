#ifndef Q_CORE_MEM_H
#define Q_CORE_MEM_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 区域式内存池：bump 分配，整体 reset。
 * 典型用法是「每请求一个池」，请求结束 q_pool_reset() 后复用，
 * 运行期不再有逐块 free，天然避免泄漏与碎片。
 *
 * 非线程安全：一个池只属于一个线程（与请求绑定）。
 */

typedef struct q_pool q_pool_t;

q_pool_t *q_pool_new(size_t block_size);
void      q_pool_destroy(q_pool_t *p);

/* 分配 n 字节（8 字节对齐）。失败返回 NULL */
void     *q_pool_alloc(q_pool_t *p, size_t n);
void     *q_pool_calloc(q_pool_t *p, size_t n);
char     *q_pool_strdup(q_pool_t *p, const char *s);
char     *q_pool_strndup(q_pool_t *p, const char *s, size_t n);

/* 保留已申请的内存块，把所有分配标记清零后复用 */
void      q_pool_reset(q_pool_t *p);

size_t    q_pool_used(q_pool_t *p);
size_t    q_pool_total(q_pool_t *p);

/* 进程级分配统计，用于泄漏自检（只在 Debug 下有意义） */
size_t    q_mem_allocated(void);
size_t    q_mem_alloc_count(void);

#ifdef __cplusplus
}
#endif

#endif /* Q_CORE_MEM_H */
