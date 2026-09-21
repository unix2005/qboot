#ifndef Q_DB_MOCK_H
#define Q_DB_MOCK_H

#include <q/db.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 内存假驱动：不连任何真实数据库，用来在没有 MySQL/Oracle 的环境里
 * 跑通 mapper / 连接池 / 事务 / 结果映射的全链路自测和演示。
 *
 * 用法：
 *   q_db_register_mock();
 *   q_dbp_t *p = q_dbp_new("mock://u:p@127.0.0.1:0/testdb", 4, 60, 1);
 *   qmock_expect(cols, ncols, cells, nrows);   // 设置下一次查询返回什么
 *   ... 正常走 q_ctx_query ...
 *   printf("%s\n", qmock_sql());               // 看实际生成的 SQL
 */

#define QMOCK_MAX_COLS 16
#define QMOCK_MAX_ROWS 128

typedef struct {
    q_val_type_t type;
    int64_t      i64;
    double       dbl;
    const char  *str;        /* 静态字符串，调用方保证生命周期 */
    size_t       len;
} qmock_cell_t;

/* 便捷构造宏 */
#define QMOCK_INT(v)    { Q_VAL_INT,    (int64_t)(v), 0, NULL, 0 }
#define QMOCK_DOUBLE(v) { Q_VAL_DOUBLE, 0, (double)(v), NULL, 0 }
#define QMOCK_STR(s)    { Q_VAL_STRING, 0, 0, (s), (s) ? strlen(s) : 0 }
#define QMOCK_NULL()    { Q_VAL_NULL,   0, 0, NULL, 0 }

int  q_db_register_mock(void);

void qmock_reset(void);

/* 设置下一次查询返回的结果集（cols 为列名数组，cells 按行优先排列） */
void qmock_expect(const char *cols[], int ncols, const qmock_cell_t *cells, int nrows);

/* 让下一次执行返回错误，用于测失败分支 */
void qmock_fail_next(int on);

/* 设置下一次 update/delete 影响的行数 */
void qmock_affected(uint64_t n);

/* 取回上一次实际执行的内容 */
const char      *qmock_sql(void);
int              qmock_nparams(void);
const q_value_t *qmock_params(int *n);
uint64_t         qmock_affected_last(void);
uint64_t         qmock_insert_id(void);
int              qmock_exec_count(void);
int              qmock_prepare_count(void);
int              qmock_tx(const char *what);   /* 返回 BEGIN/COMMIT/ROLLBACK 的次数 */

#ifdef __cplusplus
}
#endif

#endif /* Q_DB_MOCK_H */
