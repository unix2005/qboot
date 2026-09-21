#ifndef Q_CORE_TYPES_H
#define Q_CORE_TYPES_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    Q_OK           =   0,
    Q_ERR          =  -1,   /* 通用失败 */
    Q_ERR_NOMEM    =  -2,
    Q_ERR_INVAL    =  -3,
    Q_ERR_NOTFOUND =  -4,
    Q_ERR_BUSY     =  -5,   /* 队列满 / 资源暂时不可用 */
    Q_ERR_TIMEOUT  =  -6,
    Q_ERR_IO       =  -7,
    Q_ERR_EXIST    =  -8
} q_err_t;

#define Q_UNUSED(x)       ((void)(x))
#define Q_LIKELY(x)       __builtin_expect(!!(x), 1)
#define Q_UNLIKELY(x)     __builtin_expect(!!(x), 0)
#define Q_ARRAY_SIZE(a)   (sizeof(a) / sizeof((a)[0]))
#define Q_ALIGN_UP(n, a)  (((n) + ((a) - 1)) & ~((a) - 1))
#define Q_MIN(a, b)       ((a) < (b) ? (a) : (b))
#define Q_MAX(a, b)       ((a) > (b) ? (a) : (b))

/* 容器成员指针 -> 容器指针，链表实现里省掉额外的 next 字段语义 */
#define Q_CONTAINER_OF(ptr, type, member) \
    ((type *)((char *)(ptr) - offsetof(type, member)))

const char *q_err_str(int rc);

#ifdef __cplusplus
}
#endif

#endif /* Q_CORE_TYPES_H */
