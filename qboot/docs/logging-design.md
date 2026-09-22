# libq_log 设计 —— 基于现有 q_log 改造

**结论**：接口风格（printf 风格、`q_` 前缀、`debugLog`/`errLog` 这种命名）完全保留，老代码一行不用改；但**实现要重写**。现有实现在"每条日志 fopen/fclose + 无锁 + 无级别"这三件事上，放到每秒几千请求的多线程服务里会同时踩性能、崩溃和日志撕裂三个坑。

---

## 1. 现有代码的问题清单

以下行号对应 `/Users/mac/gps_dev/lib_src/l_1/q_log/api/q_debug_log.c` 与 `q_error_log.c`。

| 级别 | 问题 | 位置 | 后果 |
|---|---|---|---|
| 致命 | `fopen` 失败后 `fp` 为 `NULL`，代码只 `printf` 提示，随后照样 `fprintf(fp, ...)` | `q_debug_log.c:45-49` → `:103` | 目录写满或权限变化时**直接段错误**，而这正是最需要日志的时刻 |
| 致命 | `localtime()` 返回静态缓冲区 | `q_debug_log.c:23` | 多线程并发下时间戳互相覆盖，需要 `localtime_r` |
| 致命 | 线程安全的锁被 `#if 0` 注释掉了 | `:52-58`、`:107-112` | 多线程并发写同一文件时 `fprintf` 分多次 `write`，**日志行交错撕裂** |
| 严重 | 每条日志 `fopen` + `fclose` | `:45`、`:119` | 每条 3 次系统调用 + 路径查找。按 2000 QPS × 3 条日志算，每秒 6000 次 open/close，CPU 大量耗在 sys 态 |
| 严重 | `pthread_mode` 下按 `gettid()` 分文件 | `:41-42` | 16 个线程 = 16 个文件/天；且 tid 会被复用，新线程接着写旧文件 |
| 中等 | `q_init_log` 只做了 `malloc` + `memset`，从未 `pthread_mutex_init` | `q_init_log.c:13-21` | 即使把锁打开，`log_lock` 也是未初始化的，行为未定义 |
| 中等 | `errLog` 用 `q_sys_log[1].log_dir` 但文件名前缀写死 `debugLog` | `q_error_log.c:39` | 与 `debugLog` 的命名逻辑不一致，疑似遗留 bug |
| 中等 | 只有按天切分，无按大小轮转、无过期清理 | — | 一条死循环日志一天就能打满磁盘 |
| 中等 | 无日志级别、无运行期开关 | — | 生产环境想关 debug 只能改代码重编译 |
| 轻微 | 每条日志都 `gettimeofday` + `localtime` + `strftime` | `:64`、`:69` | 高频调用下纯属浪费，可缓存到秒 |
| 轻微 | `q_sys_log` 数组只用到下标 0 和 1，`num` 参数形同虚设 | — | 分类能力实际不可用 |
| 轻微 | 没有 `format` 属性声明 | `q_log.h:30-32` | 编译期无法检查 `printf` 格式串与参数是否匹配 |

**一句话**：这套实现放在"低频、单线程、日志只是调试用"的老项目里没问题；放进"高频、多线程、日志是排障唯一依据"的微服务里，必须换掉。

---

## 2. 目标：保留接口形态，换掉内核

```c
/* 老代码，一行不改，继续能用 */
debugLog("user %s login, cost=%dms\n", name, cost);
errLog("connect mysql failed: %s\n", strerror(errno));

/* 新写法，可选的 */
q_info("user %s login", name);
q_error_cat("sql", "select failed: %s, sql=%s", err, sql);
```

对外提供：级别、分类、trace 上下文、异步刷盘、按大小+日期轮转、降级不崩。

---

## 3. 新接口（建议放 `qboot/log/include/q/log.h`）

```c
#ifndef Q_LOG_H
#define Q_LOG_H

#define Q_LOG_PATH_LEN  512
#define Q_LOG_LINE_MAX  2048

typedef enum {
    Q_LOG_TRACE = 0, Q_LOG_DEBUG, Q_LOG_INFO,
    Q_LOG_WARN,  Q_LOG_ERROR, Q_LOG_FATAL
} q_log_level_t;

/* 初始化：dir 目录、app 应用名（文件名前缀）、level 全局级别 */
int  q_log_init(const char *dir, const char *app, q_log_level_t level);

/* 注册分类：app / access / sql / registry，各自独立文件、独立轮转阈值 */
int  q_log_add_cat(const char *name, size_t max_bytes, int keep_days);

/* 线程上下文（ThreadLocal）：设置后该线程所有日志自动带上 */
void q_log_set_ctx(const char *trace_id, const char *req_id);
void q_log_clear_ctx(void);

/* 核心写接口 */
void q_log_write(q_log_level_t lvl, const char *cat, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

void q_log_flush(void);        /* 异步：通知后台线程尽快落盘 */
void q_log_sync_flush(void);   /* 同步：信号处理器 / 退出前用，只调 write(2) */
void q_log_close(void);

/* 便捷宏 */
#define q_trace(fmt, ...) q_log_write(Q_LOG_TRACE, "app", fmt, ##__VA_ARGS__)
#define q_debug(fmt, ...) q_log_write(Q_LOG_DEBUG, "app", fmt, ##__VA_ARGS__)
#define q_info( fmt, ...) q_log_write(Q_LOG_INFO,  "app", fmt, ##__VA_ARGS__)
#define q_warn( fmt, ...) q_log_write(Q_LOG_WARN,  "app", fmt, ##__VA_ARGS__)
#define q_error(fmt, ...) q_log_write(Q_LOG_ERROR, "app", fmt, ##__VA_ARGS__)
#define q_fatal(fmt, ...) q_log_write(Q_LOG_FATAL, "app", fmt, ##__VA_ARGS__)

/* 带分类 */
#define q_error_cat(cat, fmt, ...) q_log_write(Q_LOG_ERROR, (cat), fmt, ##__VA_ARGS__)

/* 兼容老代码：签名一致，老项目直接编过 */
#define debugLog(fmt, ...) q_log_write(Q_LOG_DEBUG, "app", fmt, ##__VA_ARGS__)
#define errLog(  fmt, ...) q_log_write(Q_LOG_ERROR, "app", fmt, ##__VA_ARGS__)

#endif
```

---

## 4. 实现要点

### 4.1 写路径（调用线程不碰磁盘）

```c
typedef struct { uint32_t len; char data[Q_LOG_LINE_MAX]; } q_log_item_t;

static q_log_item_t g_ring[Q_LOG_RING_SIZE];      /* 定长槽位数组，运行期零 malloc */
static atomic_uint  g_head, g_tail;               /* 单生产者多消费者按槽位发布 */

void q_log_write(q_log_level_t lvl, const char *cat, const char *fmt, ...) {
    if (lvl < g_level) return;                    /* 最快路径：先做级别过滤 */

    char line[Q_LOG_LINE_MAX];
    int  n = q_log_format(line, sizeof line, lvl, cat, &ap);   /* 栈上 vsnprintf */

    uint32_t t    = atomic_load_explicit(&g_tail, memory_order_relaxed);
    uint32_t slot = t & (Q_LOG_RING_SIZE - 1);

    if ((size_t)n < Q_LOG_LINE_MAX && (t - atomic_load(&g_head)) < Q_LOG_RING_SIZE) {
        memcpy(g_ring[slot].data, line, n);       /* 先填数据 */
        g_ring[slot].len = n;
        atomic_store_explicit(&g_tail, t + 1, memory_order_release);  /* 再发布 */
    } else {
        q_log_write_direct(line, n);              /* 超长行或队列满：同步写，绝不丢 */
        atomic_fetch_add(&g_dropped, 1);          /* 计数，暴露到 /metrics */
    }
}
```

- 队列是**定长环形数组**，槽位固定 `Q_LOG_LINE_MAX`（如 2KB），避免运行期 malloc，也避免变长块的碎片问题
- 单条日志超过 2KB 直接同步写（这种日志本来就罕见）
- 队列满同样降级为同步写，**优先保证不丢**，而不是静默丢弃

### 4.2 后台刷盘线程

```c
static void *q_log_writer(void *arg) {
    for (;;) {
        uint32_t h = atomic_load_explicit(&g_head, memory_order_relaxed);
        uint32_t t = atomic_load_explicit(&g_tail, memory_order_acquire);
        if (h == t) { q_log_wait(); continue; }        /* futex 或条件变量，空闲不占 CPU */

        for (uint32_t i = h; i != t; i++) {            /* 批量：按 fd 归并后一次 write */
            q_log_item_t *it = &g_ring[i & (Q_LOG_RING_SIZE - 1)];
            write(cat_fd(it), it->data, it->len);
        }
        atomic_store_explicit(&g_head, t, memory_order_release);
        q_log_rotate_if_needed();                      /* 轮转检查放在空闲时做 */
    }
}
```

- 文件用 `open(path, O_WRONLY|O_APPEND|O_CREAT)` 拿 fd，**长驻不关**；`O_APPEND` 下单次 `write` 对普通文件是原子的，行不会撕裂，因此写路径完全无锁
- 空闲时 `futex` 等待，忙时批量 `write`（一次系统调用写几十条）

### 4.3 时间戳零成本

```c
/* 后台线程每秒刷新一次，日志线程只拼毫秒 */
static _Atomic time_t g_now_sec;
static char g_now_str[20];      /* "2026-09-21 22:35:10" */

/* 写日志时 */
clock_gettime(CLOCK_REALTIME_COARSE, &ts);   /* 比 gettimeofday 便宜 */
```

用 `localtime_r` 而非 `localtime`。

### 4.4 轮转与清理

- 后台线程每轮写完后检查 `cur_bytes`，超过 `max_bytes` 就 `close` → `rename(app.log, app.log.20260921-223510)` → 重新 `open`
- 启动时扫一次目录，删除超过 `keep_days` 的旧文件
- 轮转只发生在后台线程，写路径无感知

### 4.5 降级与崩溃安全

| 场景 | 处理 |
|---|---|
| `open` 失败 | 降级写 `stderr`，`g_open_failed` 计数 +1，绝不 `fprintf(NULL)` |
| 队列满 | ERROR 及以上同步写；DEBUG/INFO 可丢弃并计数 |
| 进程 SIGSEGV | 信号处理器里调 `q_log_sync_flush()`，只使用 `write(2)` 这类 async-signal-safe 函数 |
| 优雅退出 | `q_app_run` 退出前 `q_log_sync_flush()` + `q_log_close()` |

### 4.6 trace 上下文

```c
static __thread char t_trace_id[32];
static __thread char t_req_id[32];

void q_log_set_ctx(const char *trace_id, const char *req_id);
```

HTTP 中间件在请求进入时调用，之后该请求产生的所有日志（含 SQL 日志）自动带 `trace_id`。业务代码不用手动拼。这是自研相比 zlog 最大的收益点。

---

## 5. 输出格式

```
2026-09-21 22:35:10.238 [INFO ] [app    ] [tid:28147] [trace:4bf92f3577b34da6] user 1001 login
2026-09-21 22:35:10.241 [WARN ] [sql    ] [tid:28149] [trace:4bf92f3577b34da6] slow sql 213ms: SELECT * FROM t_user WHERE ...
```

固定字段顺序，方便用 `awk`/Filebeat 直接切分；不做 JSON 输出（日志量大的时候，JSON 的转义开销和体积都不划算）。

---

## 6. 迁移步骤

1. 保留 `q_log.h` 的函数名，把 `debugLog` / `errLog` 改成宏转发到 `q_log_write`（老项目零改动）
2. 用本文第 3 节的头文件替换 `include/`，实现文件收拢到 `log/src/q_log.c`（约 300~500 行）
3. 目录结构从 `api/` + `headers.h` + `makefile` 换成 `include/` + `src/` + `CMakeLists.txt`，与 qboot 其他模块保持一致
4. 删掉 `pthread_mode` / `open_log_pid` 这些开关，改为配置文件驱动：

```ini
[log]
dir = /data/logs/user-svc
level = info
max_bytes = 268435456        ; 256MB 轮转
keep_days = 7
ring_size = 8192
async = 1
```

---

## 7. 验收标准

| 项 | 指标 |
|---|---|
| 性能 | 8 线程 × 12.5 万条（共 100 万条）日志，总耗时 < 3s，写路径单次 < 2μs |
| 正确性 | 并发写出的日志**无行交错**：每行都匹配 `^\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\.\d{3} ` 且以 `\n` 结尾 |
| 可靠性 | 队列满压测下 ERROR 级零丢失；`chmod 000` 日志目录后进程不崩、降级到 stderr |
| 轮转 | 写满 `max_bytes` 自动切新文件；过期文件被清理 |
| 内存 | ASan / TSan 下干净；运行期无 malloc（除启动期） |

工作量估计：**1~2 天**（含压力测试），建议放在 M0 和 core 一起做，因为后面所有模块的排障都依赖它。
