#include <q/log.h>
#include <q/core/time.h>
#include <q/core/types.h>

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define Q_LOG_SLOT     1024        /* 每个槽位的数据区大小 */
#define Q_LOG_SLOTS    4096        /* 槽位数，必须是 2 的幂 */
#define Q_LOG_IDLE_NS  200000L     /* 消费者空闲等待 200 微秒 */
#define Q_LOG_FULL_WAIT_US 2000    /* 队列满时，调用线程最多等 2 毫秒再降级 */
#define Q_LOG_BATCH    256         /* 每批处理后检查一次轮转 */

typedef struct {
    _Atomic uint32_t seq;          /* 序号闸门：等于 t 表示可写，等于 t+1 表示可读 */
    uint32_t        len;
    uint32_t        cat;
    char            data[Q_LOG_SLOT];
} q_slot_t;

typedef struct {
    char   name[32];
    char   path[Q_LOG_PATH_LEN];
    int    fd;
    size_t max_bytes;
    size_t cur_bytes;
    int    keep_days;
} q_cat_t;

static q_slot_t  *g_ring = NULL;
static uint32_t   g_slots = Q_LOG_SLOTS;
static _Atomic uint32_t g_tail = 0;
static _Atomic uint32_t g_head = 0;

static q_cat_t    g_cats[Q_LOG_CAT_MAX];
static int        g_ncats = 0;
static int        g_default_cat = 0;

static _Atomic int  g_level   = Q_LOG_INFO;
static _Atomic int  g_running = 0;
static _Atomic int  g_stop    = 0;
static pthread_t    g_writer;

static char         g_dir[Q_LOG_PATH_LEN];
static char         g_app[64];

/* 秒级时间缓存：后台线程刷新，写线程只补毫秒 */
static char         g_tbuf[2][24];
static _Atomic int  g_tidx = 0;
static _Atomic long long g_tsec = 0;

static _Atomic unsigned long long g_written;
static _Atomic unsigned long long g_dropped;
static _Atomic unsigned long long g_sync_writes;
static _Atomic unsigned long long g_rotates;
static _Atomic unsigned long long g_open_failed;

static __thread char         t_trace[40];
static __thread char         t_req[40];
static __thread unsigned long long t_tid = 0;
static __thread int          t_tid_ok = 0;

/* ---------------- 工具 ---------------- */

static unsigned long long cur_tid(void)
{
    if (!t_tid_ok) {
#if defined(__APPLE__)
        unsigned long long tid = 0;
        pthread_threadid_np(NULL, &tid);
        t_tid = tid;
#elif defined(__linux__)
        t_tid = (unsigned long long)syscall(186);   /* SYS_gettid */
#else
        t_tid = (unsigned long long)(uintptr_t)pthread_self();
#endif
        t_tid_ok = 1;
    }
    return t_tid;
}

static int mkdir_p(const char *path)
{
    char   tmp[Q_LOG_PATH_LEN];
    size_t len;

    snprintf(tmp, sizeof(tmp), "%s", path);
    len = strlen(tmp);
    if (len > 0 && tmp[len - 1] == '/') tmp[len - 1] = '\0';

    for (char *p = tmp + 1; *p != '\0'; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
            *p = '/';
        }
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
    return 0;
}

static int cat_open(q_cat_t *c)
{
    int fd = open(c->path, O_WRONLY | O_APPEND | O_CREAT, 0644);
    if (fd < 0) {
        atomic_fetch_add_explicit(&g_open_failed, 1, memory_order_relaxed);
        c->fd = -1;
        return -1;
    }
    c->fd = fd;

    struct stat st;
    if (fstat(fd, &st) == 0) c->cur_bytes = (size_t)st.st_size;
    return 0;
}

static void cat_rotate(q_cat_t *c)
{
    char ts[32];
    char dst[Q_LOG_PATH_LEN];

    if (c->fd >= 0) {
        close(c->fd);
        c->fd = -1;
    }
    q_time_format(q_time_now_ms(), "%Y%m%d-%H%M%S", ts, sizeof(ts));
    snprintf(dst, sizeof(dst), "%s.%s", c->path, ts);
    rename(c->path, dst);

    cat_open(c);
    atomic_fetch_add_explicit(&g_rotates, 1, memory_order_relaxed);
}

static void rotate_check_all(void)
{
    for (int i = 0; i < g_ncats; i++) {
        q_cat_t *c = &g_cats[i];
        if (c->max_bytes > 0 && c->cur_bytes >= c->max_bytes) cat_rotate(c);
    }
}

static int cat_index(const char *name)
{
    if (name == NULL) return g_default_cat;
    for (int i = 0; i < g_ncats; i++) {
        if (strcmp(g_cats[i].name, name) == 0) return i;
    }
    return g_default_cat;
}

/* 完整的单次写入（循环 write 直到写完），返回写入字节数 */
static size_t write_once(int fd, const char *data, size_t len)
{
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(fd, data + off, len - off);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (n == 0) break;
        off += (size_t)n;
    }
    return off;
}

/* 每个分类一个写缓冲：把一批日志合并成一次 write(2)，系统调用数降一个量级 */
#define Q_LOG_WBUF  65536

static char   g_wbuf[Q_LOG_CAT_MAX][Q_LOG_WBUF];
static size_t g_wlen[Q_LOG_CAT_MAX];

static uint32_t cat_fix(uint32_t ci)
{
    return (ci < (uint32_t)g_ncats) ? ci : (uint32_t)g_default_cat;
}

static void buf_flush(uint32_t ci)
{
    ci = cat_fix(ci);
    size_t len = g_wlen[ci];
    if (len == 0) return;

    q_cat_t *c = &g_cats[ci];
    if (c->fd < 0) {
        (void)write_once(STDERR_FILENO, g_wbuf[ci], len);
    } else {
        c->cur_bytes += write_once(c->fd, g_wbuf[ci], len);
    }
    g_wlen[ci] = 0;
}

static void buf_flush_all(void)
{
    for (int i = 0; i < g_ncats; i++) buf_flush((uint32_t)i);
}

/* 不走缓冲，立即落盘 */
static void raw_write(uint32_t ci, const char *data, size_t len)
{
    q_cat_t *c = &g_cats[cat_fix(ci)];
    if (c->fd < 0) {
        (void)write_once(STDERR_FILENO, data, len);
    } else {
        c->cur_bytes += write_once(c->fd, data, len);
    }
    atomic_fetch_add_explicit(&g_written, 1, memory_order_relaxed);
}

static void buf_append(uint32_t ci, const char *data, size_t len)
{
    ci = cat_fix(ci);

    if (len > Q_LOG_WBUF) {                     /* 单行超过缓冲区：直接写 */
        buf_flush(ci);
        raw_write(ci, data, len);
        return;
    }
    if (g_wlen[ci] + len > Q_LOG_WBUF) buf_flush(ci);

    memcpy(g_wbuf[ci] + g_wlen[ci], data, len);
    g_wlen[ci] += len;
    atomic_fetch_add_explicit(&g_written, 1, memory_order_relaxed);
}

/*
 * 降级路径：日志线程自己写。
 * 先冲掉该分类的缓冲，保证同步写的内容排在同分类已缓冲内容之后，不产生乱序。
 */
static void write_sync(uint32_t ci, const char *data, size_t len)
{
    buf_flush(ci);
    raw_write(ci, data, len);
    atomic_fetch_add_explicit(&g_sync_writes, 1, memory_order_relaxed);
}

static void update_time_cache(void)
{
    int64_t ms  = q_time_now_ms();
    int64_t sec = ms / 1000;
    int     idx = 1 - atomic_load_explicit(&g_tidx, memory_order_relaxed);

    q_time_format(ms, "%Y-%m-%d %H:%M:%S", g_tbuf[idx], sizeof(g_tbuf[idx]));
    atomic_store_explicit(&g_tidx, idx, memory_order_release);
    atomic_store_explicit(&g_tsec, (long long)sec, memory_order_release);
}

/* ---------------- 后台刷盘线程 ---------------- */

/* 消费已发布的槽位，返回处理条数 */
static uint32_t drain_once(uint32_t max_items)
{
    uint32_t done = 0;

    for (;;) {
        uint32_t h = atomic_load_explicit(&g_head, memory_order_relaxed);
        uint32_t t = atomic_load_explicit(&g_tail, memory_order_acquire);
        if (h == t) break;
        if (max_items > 0 && done >= max_items) break;

        q_slot_t *s = &g_ring[h & (g_slots - 1)];
        if (atomic_load_explicit(&s->seq, memory_order_acquire) != h + 1) break;

        buf_append(s->cat, s->data, s->len);
        atomic_store_explicit(&s->seq, h + g_slots, memory_order_release);
        atomic_store_explicit(&g_head, h + 1, memory_order_release);
        done++;
    }
    buf_flush_all();          /* 一批写完立即落盘，最多丢一个批次的量 */
    return done;
}

static void *writer_thread(void *arg)
{
    (void)arg;

    for (;;) {
        uint32_t did = drain_once(Q_LOG_BATCH);
        rotate_check_all();
        update_time_cache();

        if (atomic_load_explicit(&g_stop, memory_order_acquire)) break;
        if (did == 0) {
            struct timespec ts;
            ts.tv_sec  = 0;
            ts.tv_nsec = Q_LOG_IDLE_NS;
            nanosleep(&ts, NULL);
        }
    }

    drain_once(0);          /* 退出前把剩余写完 */
    return NULL;
}

/* ---------------- 对外接口 ---------------- */

int q_log_init(const char *dir, const char *app, q_log_level_t level)
{
    if (g_ring != NULL) return Q_ERR_EXIST;
    if (dir == NULL || app == NULL) return Q_ERR_INVAL;

    if (mkdir_p(dir) != 0) return Q_ERR_IO;

    snprintf(g_dir, sizeof(g_dir), "%s", dir);
    snprintf(g_app, sizeof(g_app), "%s", app);
    atomic_store_explicit(&g_level, (int)level, memory_order_relaxed);

    g_ncats = 0;
    g_default_cat = 0;

    /* 默认分类 */
    q_cat_t *c = &g_cats[0];
    snprintf(c->name, sizeof(c->name), "app");
    snprintf(c->path, sizeof(c->path), "%s/%s.log", g_dir, g_app);
    c->fd = -1;
    c->max_bytes = 256u * 1024u * 1024u;
    c->cur_bytes = 0;
    c->keep_days = 7;
    g_ncats = 1;

    g_ring = calloc(g_slots, sizeof(q_slot_t));
    if (g_ring == NULL) return Q_ERR_NOMEM;
    for (uint32_t i = 0; i < g_slots; i++) {
        atomic_store_explicit(&g_ring[i].seq, i, memory_order_relaxed);
    }
    atomic_store_explicit(&g_head, 0, memory_order_relaxed);
    atomic_store_explicit(&g_tail, 0, memory_order_relaxed);

    for (int i = 0; i < g_ncats; i++) cat_open(&g_cats[i]);
    update_time_cache();

    atomic_store_explicit(&g_stop, 0, memory_order_relaxed);
    if (pthread_create(&g_writer, NULL, writer_thread, NULL) != 0) {
        free(g_ring);
        g_ring = NULL;
        return Q_ERR;
    }
    atomic_store_explicit(&g_running, 1, memory_order_release);
    return Q_OK;
}

int q_log_add_cat(const char *name, size_t max_bytes, int keep_days)
{
    if (name == NULL) return Q_ERR_INVAL;
    if (g_ncats >= Q_LOG_CAT_MAX) return Q_ERR_BUSY;

    int idx = cat_index(name);
    if (idx != g_default_cat || strcmp(g_cats[idx].name, name) == 0) {
        q_cat_t *c = &g_cats[idx];
        c->max_bytes = max_bytes;
        c->keep_days = keep_days;
        return Q_OK;
    }

    q_cat_t *c = &g_cats[g_ncats];
    snprintf(c->name, sizeof(c->name), "%s", name);
    snprintf(c->path, sizeof(c->path), "%s/%s.%s.log", g_dir, g_app, name);
    c->fd = -1;
    c->max_bytes = max_bytes;
    c->cur_bytes = 0;
    c->keep_days = keep_days;
    g_ncats++;

    if (g_ring != NULL) cat_open(c);
    return Q_OK;
}

void q_log_set_ctx(const char *trace_id, const char *req_id)
{
    if (trace_id != NULL) snprintf(t_trace, sizeof(t_trace), "%s", trace_id);
    if (req_id   != NULL) snprintf(t_req,   sizeof(t_req),   "%s", req_id);
}

void q_log_clear_ctx(void)
{
    t_trace[0] = '\0';
    t_req[0]   = '\0';
}

const char *q_log_level_str(q_log_level_t lvl)
{
    switch (lvl) {
    case Q_LOG_TRACE: return "TRACE";
    case Q_LOG_DEBUG: return "DEBUG";
    case Q_LOG_INFO:  return "INFO ";
    case Q_LOG_WARN:  return "WARN ";
    case Q_LOG_ERROR: return "ERROR";
    case Q_LOG_FATAL: return "FATAL";
    default:          return "-----";
    }
}

int q_log_level_from_str(const char *s)
{
    if (s == NULL) return Q_LOG_INFO;
    if (strcasecmp(s, "trace") == 0) return Q_LOG_TRACE;
    if (strcasecmp(s, "debug") == 0) return Q_LOG_DEBUG;
    if (strcasecmp(s, "info")  == 0) return Q_LOG_INFO;
    if (strcasecmp(s, "warn")  == 0) return Q_LOG_WARN;
    if (strcasecmp(s, "error") == 0) return Q_LOG_ERROR;
    if (strcasecmp(s, "fatal") == 0) return Q_LOG_FATAL;
    return Q_LOG_INFO;
}

static int format_line(char *buf, size_t cap, q_log_level_t lvl,
                       const char *cat, const char *fmt, va_list ap)
{
    int64_t ms  = q_time_now_ms();
    int     idx = atomic_load_explicit(&g_tidx, memory_order_acquire);
    long long csec = atomic_load_explicit(&g_tsec, memory_order_acquire);

    char tstr[32];
    int  tlen;
    if (ms / 1000 == csec) {
        memcpy(tstr, g_tbuf[idx], 20);          /* "2026-09-21 22:35:10" 19 字符 */
        tstr[19] = '\0';
        tlen = 19;
    } else {
        tlen = (int)q_time_format(ms, "%Y-%m-%d %H:%M:%S", tstr, sizeof(tstr));
        if (tlen < 0 || tlen > 19) tlen = 19;
    }
    if ((size_t)tlen < sizeof(tstr)) {
        int w = snprintf(tstr + tlen, sizeof(tstr) - (size_t)tlen,
                         ".%03d", (int)(ms % 1000));
        if (w > 0) tlen += w;
    }

    /* 每段写入前都先夹住偏移，避免 cap - n 下溢 */
    size_t n = 0;
    int    r;

    r = snprintf(buf + n, cap - n, "%s [%s] [%s] [tid:%llu]",
                 tstr, q_log_level_str(lvl), cat, cur_tid());
    if (r > 0) n += (size_t)r;
    if (n >= cap) n = cap - 1;

    if (t_trace[0] != '\0') {
        r = snprintf(buf + n, cap - n, " [trace:%s]", t_trace);
        if (r > 0) n += (size_t)r;
    }
    if (n >= cap) n = cap - 1;

    if (t_req[0] != '\0') {
        r = snprintf(buf + n, cap - n, " [req:%s]", t_req);
        if (r > 0) n += (size_t)r;
    }
    if (n >= cap) n = cap - 1;

    r = snprintf(buf + n, cap - n, " ");
    if (r > 0) n += (size_t)r;
    if (n >= cap) n = cap - 1;

    r = vsnprintf(buf + n, cap - n, fmt, ap);
    if (r > 0) n += (size_t)r;
    if (n >= cap) n = cap - 1;

    if (n > 0 && buf[n - 1] != '\n') {
        if (n + 1 < cap) buf[n++] = '\n';
    }
    buf[n] = '\0';
    return (int)n;
}

void q_log_write(q_log_level_t lvl, const char *cat, const char *fmt, ...)
{
    if ((int)lvl < atomic_load_explicit(&g_level, memory_order_relaxed)) return;
    if (fmt == NULL) return;

    char line[Q_LOG_LINE_MAX];
    va_list ap;
    va_start(ap, fmt);
    int n = format_line(line, sizeof(line), lvl,
                        cat ? cat : g_cats[g_default_cat].name, fmt, ap);
    va_end(ap);
    if (n <= 0) return;

    uint32_t ci = (uint32_t)cat_index(cat);

    /* 未初始化、或单条超过槽位容量：直接同步写 */
    if (!atomic_load_explicit(&g_running, memory_order_acquire) ||
        (size_t)n > Q_LOG_SLOT) {
        write_sync(ci, line, (size_t)n);
        return;
    }

    /*
     * CAS 抢占序号，保证 tail 不会虚高。
     * 队列满时不立刻丢：先给消费者一个等待窗口形成背压，
     * 窗口内腾不出位置才降级（ERROR 及以上同步写，其余丢弃并计数）。
     */
    uint32_t t;
    uint32_t h = atomic_load_explicit(&g_head, memory_order_relaxed);
    int      waiting = 0;
    int64_t  deadline = 0;

    for (;;) {
        t = atomic_load_explicit(&g_tail, memory_order_relaxed);

        if (t - h < g_slots) {
            if (atomic_compare_exchange_weak_explicit(&g_tail, &t, t + 1,
                    memory_order_acq_rel, memory_order_relaxed)) break;
            h = atomic_load_explicit(&g_head, memory_order_relaxed);
            continue;
        }

        if (!waiting) {
            deadline = q_time_now_us() + Q_LOG_FULL_WAIT_US;
            waiting  = 1;
        } else if (q_time_now_us() > deadline) {
            if ((int)lvl >= Q_LOG_ERROR) {
                write_sync(ci, line, (size_t)n);
            } else {
                atomic_fetch_add_explicit(&g_dropped, 1, memory_order_relaxed);
            }
            return;
        }

        sched_yield();
        h = atomic_load_explicit(&g_head, memory_order_relaxed);
    }

    q_slot_t *s = &g_ring[t & (g_slots - 1)];
    int spins = 0;
    while (atomic_load_explicit(&s->seq, memory_order_acquire) != t) {
        if (++spins > 256) {
            sched_yield();
            spins = 0;
        }
    }

    memcpy(s->data, line, (size_t)n);
    s->len = (uint32_t)n;
    s->cat = ci;
    atomic_store_explicit(&s->seq, t + 1, memory_order_release);
}

void q_log_flush(void)
{
    if (!atomic_load_explicit(&g_running, memory_order_acquire)) return;
    for (int i = 0; i < 500; i++) {                 /* 最多等 100ms */
        uint32_t h = atomic_load_explicit(&g_head, memory_order_acquire);
        uint32_t t = atomic_load_explicit(&g_tail, memory_order_acquire);
        if (h == t) break;
        q_time_sleep_ms(1);
    }
}

void q_log_sync_flush(void)
{
    if (g_ring == NULL) return;
    drain_once(0);
}

void q_log_close(void)
{
    if (g_ring == NULL) return;

    atomic_store_explicit(&g_stop, 1, memory_order_release);
    pthread_join(g_writer, NULL);
    atomic_store_explicit(&g_running, 0, memory_order_release);

    drain_once(0);
    for (int i = 0; i < g_ncats; i++) {
        if (g_cats[i].fd >= 0) {
            close(g_cats[i].fd);
            g_cats[i].fd = -1;
        }
    }
    free(g_ring);
    g_ring = NULL;
    g_ncats = 0;
}

void q_log_set_level(q_log_level_t level)
{
    atomic_store_explicit(&g_level, (int)level, memory_order_release);
}

q_log_level_t q_log_get_level(void)
{
    return (q_log_level_t)atomic_load_explicit(&g_level, memory_order_acquire);
}

void q_log_stat(q_log_stat_t *out)
{
    if (out == NULL) return;
    out->written     = atomic_load_explicit(&g_written, memory_order_relaxed);
    out->dropped     = atomic_load_explicit(&g_dropped, memory_order_relaxed);
    out->sync_writes = atomic_load_explicit(&g_sync_writes, memory_order_relaxed);
    out->rotates     = atomic_load_explicit(&g_rotates, memory_order_relaxed);
    out->open_failed = atomic_load_explicit(&g_open_failed, memory_order_relaxed);
}
