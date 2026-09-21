#include <q/core/threadpool.h>
#include <q/core/types.h>

#include <pthread.h>
#include <stdlib.h>

typedef struct {
    q_task_fn fn;
    void     *arg;
} q_task_t;

struct q_tp {
    pthread_t      *threads;
    int             nthreads;

    q_task_t       *ring;
    size_t          cap;          /* 2 的幂 */
    size_t          head;
    size_t          tail;

    pthread_mutex_t mu;
    pthread_cond_t  not_empty;
    pthread_cond_t  not_full;

    int             stop;
    size_t          done;
};

static void *tp_worker(void *arg)
{
    q_tp_t *tp = arg;

    for (;;) {
        pthread_mutex_lock(&tp->mu);

        while (tp->head == tp->tail && !tp->stop) {
            pthread_cond_wait(&tp->not_empty, &tp->mu);
        }
        if (tp->head == tp->tail && tp->stop) {
            pthread_mutex_unlock(&tp->mu);
            break;
        }

        q_task_t task = tp->ring[tp->head & (tp->cap - 1)];
        tp->head++;
        pthread_cond_signal(&tp->not_full);
        pthread_mutex_unlock(&tp->mu);

        if (task.fn != NULL) task.fn(task.arg);

        pthread_mutex_lock(&tp->mu);
        tp->done++;
        pthread_mutex_unlock(&tp->mu);
    }
    return NULL;
}

q_tp_t *q_tp_new(int nthreads, size_t queue_cap)
{
    if (nthreads <= 0) nthreads = 1;

    size_t cap = 64;
    while (cap < queue_cap) cap *= 2;

    q_tp_t *tp = calloc(1, sizeof(q_tp_t));
    if (tp == NULL) return NULL;

    tp->threads = calloc((size_t)nthreads, sizeof(pthread_t));
    tp->ring    = calloc(cap, sizeof(q_task_t));
    if (tp->threads == NULL || tp->ring == NULL) {
        free(tp->threads);
        free(tp->ring);
        free(tp);
        return NULL;
    }

    tp->nthreads = 0;
    tp->cap      = cap;
    tp->head = tp->tail = 0;
    tp->stop = 0;
    tp->done = 0;

    pthread_mutex_init(&tp->mu, NULL);
    pthread_cond_init(&tp->not_empty, NULL);
    pthread_cond_init(&tp->not_full, NULL);

    for (int i = 0; i < nthreads; i++) {
        if (pthread_create(&tp->threads[i], NULL, tp_worker, tp) != 0) break;
        tp->nthreads++;
    }
    if (tp->nthreads == 0) {
        q_tp_destroy(tp);
        return NULL;
    }
    return tp;
}

int q_tp_submit(q_tp_t *tp, q_task_fn fn, void *arg)
{
    if (tp == NULL || fn == NULL) return Q_ERR_INVAL;

    pthread_mutex_lock(&tp->mu);
    if (tp->stop) {
        pthread_mutex_unlock(&tp->mu);
        return Q_ERR_INVAL;
    }
    if (tp->tail - tp->head >= tp->cap) {
        pthread_mutex_unlock(&tp->mu);
        return Q_ERR_BUSY;
    }

    tp->ring[tp->tail & (tp->cap - 1)].fn  = fn;
    tp->ring[tp->tail & (tp->cap - 1)].arg = arg;
    tp->tail++;
    pthread_cond_signal(&tp->not_empty);
    pthread_mutex_unlock(&tp->mu);
    return Q_OK;
}

void q_tp_destroy(q_tp_t *tp)
{
    if (tp == NULL) return;

    pthread_mutex_lock(&tp->mu);
    tp->stop = 1;
    pthread_cond_broadcast(&tp->not_empty);
    pthread_cond_broadcast(&tp->not_full);
    pthread_mutex_unlock(&tp->mu);

    for (int i = 0; i < tp->nthreads; i++) pthread_join(tp->threads[i], NULL);

    pthread_mutex_destroy(&tp->mu);
    pthread_cond_destroy(&tp->not_empty);
    pthread_cond_destroy(&tp->not_full);

    free(tp->threads);
    free(tp->ring);
    free(tp);
}

size_t q_tp_pending(q_tp_t *tp)
{
    size_t n = 0;
    if (tp == NULL) return 0;
    pthread_mutex_lock(&tp->mu);
    n = tp->tail - tp->head;
    pthread_mutex_unlock(&tp->mu);
    return n;
}

size_t q_tp_done(q_tp_t *tp)
{
    size_t n = 0;
    if (tp == NULL) return 0;
    pthread_mutex_lock(&tp->mu);
    n = tp->done;
    pthread_mutex_unlock(&tp->mu);
    return n;
}

int q_tp_threads(q_tp_t *tp) { return tp ? tp->nthreads : 0; }
