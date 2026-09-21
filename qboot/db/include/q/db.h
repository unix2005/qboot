#ifndef Q_DB_H
#define Q_DB_H

/* 错误码 Q_OK / Q_ERR_* 是本头文件契约的一部分，直接带上，调用方不必再猜 */
#include <q/core/types.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 数据库访问统一抽象。
 * 一期实现 MySQL（MariaDB Connector/C）；Oracle / 达梦后续按同一套 ops 接入，
 * 上层 mapper 与业务代码不需要改动。
 */

/* ---------------- 值 ---------------- */

typedef enum {
    Q_VAL_NULL = 0,
    Q_VAL_INT,
    Q_VAL_DOUBLE,
    Q_VAL_STRING,
    Q_VAL_BLOB
} q_val_type_t;

typedef struct {
    q_val_type_t type;
    int64_t      i64;
    double       dbl;
    const char  *str;        /* 指向结果集内部缓冲，换到下一行或释放后失效 */
    size_t       len;
} q_value_t;

q_value_t   q_val_int(int64_t v);
q_value_t   q_val_double(double v);
q_value_t   q_val_str(const char *s);
q_value_t   q_val_blob(const void *p, size_t n);
q_value_t   q_val_null(void);

int         q_val_is_null(const q_value_t *v);
int64_t     q_val_as_int(const q_value_t *v);
double      q_val_as_double(const q_value_t *v);
const char *q_val_as_str(const q_value_t *v);      /* 非字符串返回栈缓冲 */

/* ---------------- DSN ---------------- */

typedef struct {
    char driver[16];
    char host[128];
    int  port;
    char user[64];
    char pass[128];
    char name[64];
    char charset[32];
} q_dsn_t;

/* mysql://user:pass@host:3306/dbname */
int q_dsn_parse(const char *url, q_dsn_t *out, char *err, size_t errlen);

/* ---------------- 驱动接口 ---------------- */

typedef struct q_result q_result_t;
typedef struct q_dialect q_dialect_t;

typedef struct q_db_ops {
    int  (*connect)(void **out, const q_dsn_t *dsn, char *err, size_t errlen);
    void (*close)(void *h);
    int  (*ping)(void *h, char *err, size_t errlen);

    /* 无参数直接执行。res 传 NULL 表示不需要结果集 */
    int  (*exec)(void *h, const char *sql, size_t len, void **res,
                 uint64_t *affected, uint64_t *insert_id, char *err, size_t errlen);

    /* 预处理语句 */
    int  (*prepare)(void *h, void **stmt, const char *sql, size_t len,
                    int *nparams, char *err, size_t errlen);
    void (*stmt_close)(void *stmt);
    int  (*stmt_bind)(void *stmt, const q_value_t *params, int n, char *err, size_t errlen);
    int  (*stmt_exec)(void *stmt, void **res,
                      uint64_t *affected, uint64_t *insert_id, char *err, size_t errlen);

    /* 结果集：流式逐行 */
    int  (*res_cols)(void *res, const char ***names, int *ncols);
    int  (*res_row)(void *res, q_value_t *vals, int ncols, char *err, size_t errlen);
    void (*res_free)(void *res);
} q_db_ops_t;

typedef struct q_dialect {
    const char *name;
    char        placeholder;         /* '?' 或 ':' */
    int         support_returning;
    void      (*paginate)(char *buf, size_t cap, long long offset, long long limit);
} q_dialect_t;

typedef struct q_db_driver {
    const char        *name;
    const q_db_ops_t  *ops;
    const q_dialect_t *dialect;
} q_db_driver_t;

int                q_db_register(const q_db_driver_t *drv);
const q_db_driver_t *q_db_find(const char *name);

int q_db_register_mysql(void);        /* 各驱动自带的注册入口 */

/* ---------------- 连接 / 结果 / 语句 ---------------- */

typedef struct q_conn {
    struct q_dbp   *pool;
    const q_db_ops_t *ops;
    void           *handle;
    int             in_tx;
    int             shard;
    int             tx_owned;         /* 事务专用连接，需要显式归还 */
    int64_t         last_used_ms;
    void           *ud;               /* 上层私有数据：mapper 用它挂本连接的语句缓存 */
    void          (*ud_free)(void *ud);
} q_conn_t;

struct q_result {
    void             *drv;
    const q_db_ops_t *ops;
    int               ncols;
    const char      **names;
    q_value_t        *row;            /* ncols 个，通用层分配 */
    uint64_t          affected;
    uint64_t          insert_id;
};

typedef struct q_stmt {
    q_conn_t         *conn;
    void             *drv_stmt;
    const q_db_ops_t *ops;
    int               nparams;
} q_stmt_t;

/* ---------------- 连接池 ---------------- */

typedef struct q_dbp q_dbp_t;

/*
 * url        形如 mysql://user:pass@host:3306/db
 * max_open   每个分片的最大连接数
 * idle_secs  空闲连接保留秒数，超过则关闭
 * nshards    分段数（建议 = 业务线程数），降低锁竞争
 */
q_dbp_t  *q_dbp_new(const char *url, int max_open, int idle_secs, int nshards);
void      q_dbp_free(q_dbp_t *p);

q_conn_t *q_dbp_get(q_dbp_t *p, char *err, size_t errlen);    /* 借，用完必须 q_dbp_put */
void      q_dbp_put(q_conn_t *c);
q_conn_t *q_dbp_tx(q_dbp_t *p, char *err, size_t errlen);     /* 线程事务连接，首次自动 BEGIN */

int  q_conn_exec(q_conn_t *c, const char *sql, uint64_t *affected, uint64_t *insert_id,
                 char *err, size_t errlen);
int  q_conn_query(q_conn_t *c, const char *sql, q_result_t **out, char *err, size_t errlen);
int  q_conn_prepare(q_conn_t *c, q_stmt_t **out, const char *sql, int *nparams,
                    char *err, size_t errlen);

int  q_stmt_bind(q_stmt_t *s, const q_value_t *params, int n, char *err, size_t errlen);
int  q_stmt_query(q_stmt_t *s, q_result_t **out, char *err, size_t errlen);
int  q_stmt_exec(q_stmt_t *s, uint64_t *affected, uint64_t *insert_id,
                 char *err, size_t errlen);
void q_stmt_close(q_stmt_t *s);

int  q_result_next(q_result_t *r);        /* 1=取到行，0=结束，<0 出错 */
void q_result_free(q_result_t *r);

/* 事务：作用于 ThreadLocal 连接，提交/回滚后自动归还 */
int  q_tx_begin(q_conn_t *c);
int  q_tx_commit(q_conn_t *c);
int  q_tx_rollback(q_conn_t *c);

typedef struct {
    int                  in_use;
    int                  idle;
    unsigned long long   queries;
    unsigned long long   errors;
    unsigned long long   conn_created;
} q_dbp_stat_t;

void q_dbp_stat(q_dbp_t *p, q_dbp_stat_t *out);

#ifdef __cplusplus
}
#endif

#endif /* Q_DB_H */
