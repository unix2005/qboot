/*
 * 内存假驱动。不解析 SQL，只做三件事：
 *   1) 记录实际下发的 SQL 与绑定参数，供断言使用
 *   2) 按 qmock_expect() 设定的内容返回结果集
 *   3) 让 insert/update/delete 返回可预期的 affected / insert_id
 * 目的：没有真实数据库时，mapper / 池 / 事务 / 映射照样能端到端自测。
 */

#include <q/db.h>
#include <q/db_mock.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* ---------------- 全局录制状态 ---------------- */

typedef struct {
    int          has_res;
    const char  *cols[QMOCK_MAX_COLS];
    char         colbuf[QMOCK_MAX_COLS][64];
    int          ncols;
    qmock_cell_t cells[QMOCK_MAX_ROWS * QMOCK_MAX_COLS];
    int          nrows;
} qmock_res_t;

static qmock_res_t g_res;
static char        g_sql[4096];
static q_value_t   g_params[64];
static int         g_nparams;
static uint64_t    g_affected = 1;
static uint64_t    g_affected_last;
static uint64_t    g_insert_id;
static int         g_exec_count;
static int         g_prepare_count;
static int         g_fail_next;
static int         g_tx_begin, g_tx_commit, g_tx_rollback;
static int         g_conn_seq;

/*
 * 参数字符串的私有副本。上层（mapper）绑完可能在 exec 之后立刻释放自己的缓冲，
 * 这里必须自己留一份，否则 qmock_params() 拿到的是悬空指针。
 */
#define QMOCK_PSTR_SLOTS 64
#define QMOCK_PSTR_LEN   256
static char g_pstr[QMOCK_PSTR_SLOTS][QMOCK_PSTR_LEN];
static int  g_npstr;

void qmock_reset(void)
{
    memset(&g_res, 0, sizeof(g_res));
    g_sql[0]        = '\0';
    g_nparams       = 0;
    g_npstr         = 0;
    g_affected      = 1;
    g_affected_last = 0;
    g_insert_id     = 0;
    g_exec_count    = 0;
    g_prepare_count = 0;
    g_fail_next     = 0;
    g_tx_begin = g_tx_commit = g_tx_rollback = 0;
}

void qmock_expect(const char *cols[], int ncols, const qmock_cell_t *cells, int nrows)
{
    memset(&g_res, 0, sizeof(g_res));
    if (ncols > QMOCK_MAX_COLS) ncols = QMOCK_MAX_COLS;
    if (nrows > QMOCK_MAX_ROWS) nrows = QMOCK_MAX_ROWS;

    g_res.has_res = 1;
    g_res.ncols   = ncols;
    g_res.nrows   = nrows;
    for (int i = 0; i < ncols; i++) {
        snprintf(g_res.colbuf[i], sizeof(g_res.colbuf[i]), "%s", cols[i] ? cols[i] : "");
        g_res.cols[i] = g_res.colbuf[i];
    }
    if (cells != NULL) {
        memcpy(g_res.cells, cells, sizeof(qmock_cell_t) * (size_t)ncols * (size_t)nrows);
    }
}

void qmock_fail_next(int on) { g_fail_next = on ? 1 : 0; }
void qmock_affected(uint64_t n) { g_affected = n; }

const char      *qmock_sql(void)         { return g_sql; }
int              qmock_nparams(void)     { return g_nparams; }
uint64_t         qmock_affected_last(void) { return g_affected_last; }
uint64_t         qmock_insert_id(void)   { return g_insert_id; }
int              qmock_exec_count(void)  { return g_exec_count; }
int              qmock_prepare_count(void) { return g_prepare_count; }

const q_value_t *qmock_params(int *n)
{
    if (n != NULL) *n = g_nparams;
    return g_params;
}

int qmock_tx(const char *what)
{
    if (what == NULL) return 0;
    if (strcmp(what, "BEGIN") == 0)    return g_tx_begin;
    if (strcmp(what, "COMMIT") == 0)   return g_tx_commit;
    if (strcmp(what, "ROLLBACK") == 0) return g_tx_rollback;
    return 0;
}

/* ---------------- 驱动实现 ---------------- */

typedef struct {
    int id;
} mock_conn_t;

typedef struct {
    char *sql;
    int   nparams;
} mock_stmt_t;

typedef struct {
    int row;
} mock_res_t;

static int mc_connect(void **out, const q_dsn_t *dsn, char *err, size_t errlen)
{
    (void)dsn; (void)err; (void)errlen;
    mock_conn_t *c = calloc(1, sizeof(mock_conn_t));
    if (c == NULL) return Q_ERR_NOMEM;
    c->id = ++g_conn_seq;
    *out = c;
    return Q_OK;
}

static void mc_close(void *h) { free(h); }

static int mc_ping(void *h, char *err, size_t errlen)
{
    (void)h; (void)err; (void)errlen;
    return Q_OK;
}

/* 把一行结果填进通用层的 q_value_t 数组 */
static int fill_row(mock_res_t *r, q_value_t *vals, int ncols)
{
    if (r->row >= g_res.nrows) return 0;                /* 结束 */

    const qmock_cell_t *src = &g_res.cells[(size_t)r->row * (size_t)g_res.ncols];
    int n = ncols < g_res.ncols ? ncols : g_res.ncols;

    for (int i = 0; i < n; i++) {
        memset(&vals[i], 0, sizeof(vals[i]));
        vals[i].type = src[i].type;
        vals[i].i64  = src[i].i64;
        vals[i].dbl  = src[i].dbl;
        vals[i].str  = src[i].str;
        vals[i].len  = src[i].len ? src[i].len
                                  : (src[i].str ? strlen(src[i].str) : 0);
    }
    r->row++;
    return 1;
}

static void *make_res(void)
{
    if (!g_res.has_res || g_res.ncols <= 0 || g_res.nrows < 0) return NULL;
    mock_res_t *r = calloc(1, sizeof(mock_res_t));
    return r;
}

static int mc_exec(void *h, const char *sql, size_t len, void **res,
                   uint64_t *affected, uint64_t *insert_id, char *err, size_t errlen)
{
    (void)h;
    if (res != NULL) *res = NULL;

    size_t n = len < sizeof(g_sql) - 1 ? len : sizeof(g_sql) - 1;
    memcpy(g_sql, sql, n);
    g_sql[n] = '\0';
    g_nparams = 0;
    g_exec_count++;

    if (g_fail_next) {
        g_fail_next = 0;
        snprintf(err, errlen, "mock: forced failure");
        return Q_ERR;
    }

    /* 事务语句单独计数，不下发结果集 */
    if (strncasecmp(g_sql, "START TRANSACTION", 17) == 0) { g_tx_begin++;    return Q_OK; }
    if (strncasecmp(g_sql, "COMMIT", 6) == 0)             { g_tx_commit++;   return Q_OK; }
    if (strncasecmp(g_sql, "ROLLBACK", 8) == 0)           { g_tx_rollback++; return Q_OK; }

    g_affected_last = g_affected;
    if (affected != NULL)  *affected  = g_affected;
    if (insert_id != NULL) *insert_id = g_insert_id;

    if (res != NULL) {
        void *r = make_res();
        if (r != NULL) *res = r;
    }
    return Q_OK;
}

static int mc_prepare(void *h, void **stmt, const char *sql, size_t len,
                      int *nparams, char *err, size_t errlen)
{
    (void)h; (void)err; (void)errlen;
    g_prepare_count++;

    mock_stmt_t *s = calloc(1, sizeof(mock_stmt_t));
    if (s == NULL) return Q_ERR_NOMEM;

    s->sql = malloc(len + 1);
    if (s->sql == NULL) { free(s); return Q_ERR_NOMEM; }
    memcpy(s->sql, sql, len);
    s->sql[len] = '\0';

    int np = 0;
    for (size_t i = 0; i < len; i++) {
        if (sql[i] == '?') np++;
    }
    s->nparams = np;

    *stmt = s;
    if (nparams != NULL) *nparams = np;
    return Q_OK;
}

static void mc_stmt_close(void *stmt)
{
    mock_stmt_t *s = stmt;
    if (s == NULL) return;
    free(s->sql);
    free(s);
}

static int mc_stmt_bind(void *stmt, const q_value_t *params, int n,
                        char *err, size_t errlen)
{
    mock_stmt_t *s = stmt;
    if (s == NULL) return Q_ERR_INVAL;

    if (n > (int)(sizeof(g_params) / sizeof(g_params[0]))) {
        snprintf(err, errlen, "mock: too many params (%d)", n);
        return Q_ERR_INVAL;
    }
    g_nparams = n;
    g_npstr   = 0;
    for (int i = 0; i < n; i++) {
        g_params[i] = params[i];
        if ((params[i].type == Q_VAL_STRING || params[i].type == Q_VAL_BLOB)
            && params[i].str != NULL) {
            int slot = g_npstr % QMOCK_PSTR_SLOTS;
            size_t l = params[i].len;
            if (l == 0) l = strlen(params[i].str);
            if (l >= QMOCK_PSTR_LEN) l = QMOCK_PSTR_LEN - 1;
            memcpy(g_pstr[slot], params[i].str, l);
            g_pstr[slot][l] = '\0';
            g_params[i].str = g_pstr[slot];
            g_params[i].len = l;
            g_npstr++;
        }
    }
    return Q_OK;
}

static int mc_stmt_exec(void *stmt, void **res,
                        uint64_t *affected, uint64_t *insert_id,
                        char *err, size_t errlen)
{
    mock_stmt_t *s = stmt;
    if (s == NULL) return Q_ERR_INVAL;

    snprintf(g_sql, sizeof(g_sql), "%s", s->sql);
    g_exec_count++;

    if (g_fail_next) {
        g_fail_next = 0;
        snprintf(err, errlen, "mock: forced failure");
        return Q_ERR;
    }

    /* insert 时先自增主键，再回填，否则调用方拿到的永远是上一次的值 */
    if (strncasecmp(g_sql, "insert", 6) == 0) g_insert_id++;

    g_affected_last = g_affected;
    if (affected != NULL)  *affected  = g_affected;
    if (insert_id != NULL) *insert_id = g_insert_id;

    if (res != NULL) {
        void *r = make_res();
        if (r != NULL) *res = r;
    }
    return Q_OK;
}

static int mc_res_cols(void *res, const char ***names, int *ncols)
{
    (void)res;
    if (!g_res.has_res || g_res.ncols <= 0) return Q_ERR;
    *names = g_res.cols;
    *ncols = g_res.ncols;
    return Q_OK;
}

static int mc_res_row(void *res, q_value_t *vals, int ncols, char *err, size_t errlen)
{
    (void)err; (void)errlen;
    return fill_row(res, vals, ncols);
}

static void mc_res_free(void *res) { free(res); }

static const q_db_ops_t mock_ops = {
    .connect     = mc_connect,
    .close       = mc_close,
    .ping        = mc_ping,
    .exec        = mc_exec,
    .prepare     = mc_prepare,
    .stmt_close  = mc_stmt_close,
    .stmt_bind   = mc_stmt_bind,
    .stmt_exec   = mc_stmt_exec,
    .res_cols    = mc_res_cols,
    .res_row     = mc_res_row,
    .res_free    = mc_res_free
};

static void mock_paginate(char *buf, size_t cap, long long offset, long long limit)
{
    snprintf(buf, cap, " LIMIT %lld,%lld", offset, limit);
}

static const q_dialect_t mock_dialect = {
    .name              = "mock",
    .placeholder       = '?',
    .support_returning = 1,
    .paginate          = mock_paginate
};

static const q_db_driver_t mock_driver = {
    .name    = "mock",
    .ops     = &mock_ops,
    .dialect = &mock_dialect
};

int q_db_register_mock(void)
{
    return q_db_register(&mock_driver);
}
