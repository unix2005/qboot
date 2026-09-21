#ifndef Q_CORE_THREADPOOL_H
#define Q_CORE_THREADPOOL_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 固定线程数 + 定长环形任务队列。
 * submit 时队列满返回 Q_ERR_BUSY，调用方决定是阻塞重试还是降级，
 * 不在这里偷偷阻塞，避免把背压藏起来。
 */

typedef struct q_tp q_tp_t;
typedef void (*q_task_fn)(void *arg);

q_tp_t *q_tp_new(int nthreads, size_t queue_cap);
int     q_tp_submit(q_tp_t *tp, q_task_fn fn, void *arg);
void    q_tp_destroy(q_tp_t *tp);

size_t  q_tp_pending(q_tp_t *tp);
size_t  q_tp_done(q_tp_t *tp);
int     q_tp_threads(q_tp_t *tp);

#ifdef __cplusplus
}
#endif

#endif /* Q_CORE_THREADPOOL_H */
