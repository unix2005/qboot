/*
 * MySQL 驱动：基于 MariaDB Connector/C（与 libmysqlclient API 兼容，LGPL）。
 * 用 ? 占位符，分页用 LIMIT offset,size。
 */

#include <q/db.h>

#include <q/core/types.h>
#include <q/log.h>

#include <mysql.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MY_ERRBUF 256

typedef struct {
    int         kind;          /* 0 = 文本结果集(MYSQL_RES)，1 = 预处理结果集 */
    MYSQL_RES  *res;
    MYSQL_STMT *stmt;
    MYSQL_FIELD *fields;
    char      **names;
    int         ncols;
    MYSQL_BIND *out;
    unsigned long *out_len;
    my_bool    *out_null;
    char       *out_buf;
    size_t     *out_off;
} my_result_t;

typedef struct {
    MYSQL_STMT *stmt;
    MYSQL_BIND *in;
    unsigned long *in_len;
    my_bool    *in_null;
    int         nparams;
    int         ncols;
} my_stmt_t;

static void copy_err(char *err, size_t errlen, const char *src)
{
    if (err != NULL && errlen > 0) snprintf(err, errlen, "%s", src ? src : "unknown");
}

/* ---------------- 连接 ---------------- */

static int my_connect(void **out, const q_dsn_t *dsn, char *err, size_t errlen)
{
    MYSQL *m = mysql_init(NULL);
    if (m == NULL) {
        copy_err(err, errlen, "mysql_init failed (out of memory?)");
        return Q_ERR;
    }

    my_bool reconnect = 0;
    mysql_options(m, MYSQL_SET_CHARSET_NAME, dsn->charset);
    mysql_options(m, MYSQL_OPT_RECONNECT, &reconnect);   /* 重连交给池的 ping 逻辑 */

    if (mysql_real_connect(m, dsn->host[0] ? dsn->host : "127.0.0.1",
                           dsn->user, dsn->pass,
                           dsn->name[0] ? dsn->name : NULL,
                           (unsigned int)dsn->port, NULL, 0) == NULL) {
        copy_err(err, errlen, mysql_error(m));
        mysql_close(m);
        return Q_ERR;
    }

    my_bool on = 1;
    mysql_autocommit(m, on);
    *out = m;
    return Q_OK;
}

static void my_close(void *h)
{
    if (h != NULL) mysql_close((MYSQL *)h);
}

static int my_ping(void *h, char *err, size_t errlen)
{
    MYSQL *m = (MYSQL *)h;
    if (mysql_ping(m) != 0) {
        copy_err(err, errlen, mysql_error(m));
        return Q_ERR;
    }
    return Q_OK;
}

/* ---------------- 结果集 ---------------- */

static void my_res_free(void *res)
{
    my_result_t *mr = res;
    if (mr == NULL) return;

    if (mr->kind == 0) {
        if (mr->res != NULL) mysql_free_result(mr->res);
    }
    free(mr->out);
    free(mr->out_len);
    free(mr->out_null);
    free(mr->out_off);
    free(mr->out_buf);
    free(mr->names);
    free(mr);
}

static int my_res_cols(void *res, const char ***names, int *ncols)
{
    my_result_t *mr = res;
    if (mr == NULL || mr->ncols <= 0) return Q_ERR;
    *names = (const char **)mr->names;
    *ncols = mr->ncols;
    return Q_OK;
}

static int my_res_row(void *res, q_value_t *vals, int ncols, char *err, size_t errlen)
{
    my_result_t *mr = res;
    if (mr == NULL || vals == NULL) return Q_ERR_INVAL;

    if (mr->kind == 0) {
        MYSQL_ROW row = mysql_fetch_row(mr->res);
        if (row == NULL) return 0;

        unsigned long *len = mysql_fetch_lengths(mr->res);
        for (int i = 0; i < ncols && i < mr->ncols; i++) {
            memset(&vals[i], 0, sizeof(vals[i]));
            if (row[i] == NULL) {
                vals[i].type = Q_VAL_NULL;
                continue;
            }
            switch (mr->fields[i].type) {
            case MYSQL_TYPE_TINY:
            case MYSQL_TYPE_SHORT:
            case MYSQL_TYPE_LONG:
            case MYSQL_TYPE_INT24:
            case MYSQL_TYPE_LONGLONG:
                vals[i].type = Q_VAL_INT;
                vals[i].i64  = strtoll(row[i], NULL, 10);
                break;
            case MYSQL_TYPE_FLOAT:
            case MYSQL_TYPE_DOUBLE:
                vals[i].type = Q_VAL_DOUBLE;
                vals[i].dbl  = atof(row[i]);
                break;
            default:
                vals[i].type = Q_VAL_STRING;
                vals[i].str  = row[i];
                vals[i].len  = len ? len[i] : strlen(row[i]);
                break;
            }
        }
        return 1;
    }

    /* 预处理结果集 */
    int rc = mysql_stmt_fetch(mr->stmt);
    if (rc == MYSQL_NO_DATA) return 0;
    if (rc != 0 && rc != MYSQL_DATA_TRUNCATED) {
        copy_err(err, errlen, mysql_stmt_error(mr->stmt));
        return Q_ERR;
    }

    for (int i = 0; i < ncols && i < mr->ncols; i++) {
        memset(&vals[i], 0, sizeof(vals[i]));
        if (mr->out_null[i]) {
            vals[i].type = Q_VAL_NULL;
            continue;
        }
        char *p = mr->out_buf + mr->out_off[i];
        switch (mr->out[i].buffer_type) {
        case MYSQL_TYPE_LONGLONG:
            vals[i].type = Q_VAL_INT;
            memcpy(&vals[i].i64, p, sizeof(int64_t));
            break;
        case MYSQL_TYPE_DOUBLE:
            vals[i].type = Q_VAL_DOUBLE;
            memcpy(&vals[i].dbl, p, sizeof(double));
            break;
        default:
            vals[i].type = Q_VAL_STRING;
            vals[i].str  = p;
            vals[i].len  = mr->out_len[i];
            if (vals[i].len > mr->out[i].buffer_length) {
                vals[i].len = mr->out[i].buffer_length;   /* 截断：长文本场景需显式处理 */
            }
            break;
        }
    }
    return 1;
}

/* ---------------- 文本协议执行 ---------------- */

static int my_exec(void *h, const char *sql, size_t len, void **res,
                   uint64_t *affected, uint64_t *insert_id, char *err, size_t errlen)
{
    MYSQL *m = (MYSQL *)h;

    if (mysql_real_query(m, sql, (unsigned long)len) != 0) {
        copy_err(err, errlen, mysql_error(m));
        return Q_ERR;
    }
    if (affected != NULL)  *affected  = (uint64_t)mysql_affected_rows(m);
    if (insert_id != NULL) *insert_id = (uint64_t)mysql_insert_id(m);

    if (res == NULL) return Q_OK;

    MYSQL_RES *r = mysql_store_result(m);
    if (r == NULL) {
        *res = NULL;                     /* 非查询语句，没有结果集 */
        return Q_OK;
    }

    my_result_t *mr = calloc(1, sizeof(my_result_t));
    if (mr == NULL) {
        mysql_free_result(r);
        copy_err(err, errlen, "out of memory");
        return Q_ERR_NOMEM;
    }
    mr->kind   = 0;
    mr->res    = r;
    mr->ncols  = (int)mysql_num_fields(r);
    mr->fields = mysql_fetch_fields(r);
    mr->names  = calloc((size_t)(mr->ncols > 0 ? mr->ncols : 1), sizeof(char *));
    for (int i = 0; i < mr->ncols; i++) mr->names[i] = mr->fields[i].name;

    *res = mr;
    return Q_OK;
}

/* ---------------- 预处理 ---------------- */

static int my_prepare(void *h, void **stmt, const char *sql, size_t len,
                      int *nparams, char *err, size_t errlen)
{
    MYSQL *m = (MYSQL *)h;

    MYSQL_STMT *st = mysql_stmt_init(m);
    if (st == NULL) {
        copy_err(err, errlen, mysql_error(m));
        return Q_ERR;
    }
    if (mysql_stmt_prepare(st, sql, (unsigned long)len) != 0) {
        copy_err(err, errlen, mysql_stmt_error(st));
        mysql_stmt_close(st);
        return Q_ERR;
    }

    my_stmt_t *ms = calloc(1, sizeof(my_stmt_t));
    if (ms == NULL) {
        mysql_stmt_close(st);
        return Q_ERR_NOMEM;
    }
    ms->stmt    = st;
    ms->nparams = (int)mysql_stmt_param_count(st);
    ms->ncols   = (int)mysql_stmt_field_count(st);

    *stmt = ms;
    if (nparams != NULL) *nparams = ms->nparams;
    return Q_OK;
}

static void my_stmt_close(void *stmt)
{
    my_stmt_t *ms = stmt;
    if (ms == NULL) return;
    if (ms->stmt != NULL) mysql_stmt_close(ms->stmt);
    free(ms->in);
    free(ms->in_len);
    free(ms->in_null);
    free(ms);
}

static int my_stmt_bind(void *stmt, const q_value_t *params, int n, char *err, size_t errlen)
{
    my_stmt_t *ms = stmt;
    if (ms == NULL || params == NULL) return Q_ERR_INVAL;
    if (n <= 0) return Q_OK;

    if (ms->in == NULL || ms->nparams < n) {
        free(ms->in);
        free(ms->in_len);
        free(ms->in_null);
        ms->in      = calloc((size_t)n, sizeof(MYSQL_BIND));
        ms->in_len  = calloc((size_t)n, sizeof(unsigned long));
        ms->in_null = calloc((size_t)n, sizeof(my_bool));
        if (ms->in == NULL || ms->in_len == NULL || ms->in_null == NULL) return Q_ERR_NOMEM;
    }
    memset(ms->in, 0, (size_t)n * sizeof(MYSQL_BIND));

    for (int i = 0; i < n; i++) {
        const q_value_t *v = &params[i];
        MYSQL_BIND *b = &ms->in[i];

        switch (v->type) {
        case Q_VAL_NULL:
            b->buffer_type = MYSQL_TYPE_NULL;
            ms->in_null[i] = 1;
            b->is_null = &ms->in_null[i];
            break;
        case Q_VAL_INT:
            b->buffer_type   = MYSQL_TYPE_LONGLONG;
            b->buffer        = (void *)&v->i64;
            b->buffer_length = sizeof(int64_t);
            b->is_unsigned   = 0;
            break;
        case Q_VAL_DOUBLE:
            b->buffer_type   = MYSQL_TYPE_DOUBLE;
            b->buffer        = (void *)&v->dbl;
            b->buffer_length = sizeof(double);
            break;
        case Q_VAL_STRING:
        case Q_VAL_BLOB:
            b->buffer_type   = (v->type == Q_VAL_BLOB) ? MYSQL_TYPE_BLOB : MYSQL_TYPE_STRING;
            b->buffer        = (void *)(v->str ? v->str : "");
            ms->in_len[i]    = v->len;
            b->buffer_length = v->len;
            b->length        = &ms->in_len[i];
            break;
        default:
            b->buffer_type = MYSQL_TYPE_NULL;
            ms->in_null[i] = 1;
            b->is_null     = &ms->in_null[i];
            break;
        }
    }

    if (mysql_stmt_bind_param(ms->stmt, ms->in) != 0) {
        copy_err(err, errlen, mysql_stmt_error(ms->stmt));
        return Q_ERR;
    }
    return Q_OK;
}

/* 为预处理结果集准备输出缓冲 */
static int bind_out_buffers(my_result_t *mr)
{
    size_t total = 0;

    mr->out      = calloc((size_t)mr->ncols, sizeof(MYSQL_BIND));
    mr->out_len  = calloc((size_t)mr->ncols, sizeof(unsigned long));
    mr->out_null = calloc((size_t)mr->ncols, sizeof(my_bool));
    mr->out_off  = calloc((size_t)mr->ncols, sizeof(size_t));
    if (mr->out == NULL || mr->out_len == NULL || mr->out_null == NULL || mr->out_off == NULL) {
        return Q_ERR_NOMEM;
    }

    for (int i = 0; i < mr->ncols; i++) {
        size_t need = 8;
        switch (mr->fields[i].type) {
        case MYSQL_TYPE_TINY:
        case MYSQL_TYPE_SHORT:
        case MYSQL_TYPE_LONG:
        case MYSQL_TYPE_INT24:
        case MYSQL_TYPE_LONGLONG:
            mr->out[i].buffer_type = MYSQL_TYPE_LONGLONG;
            need = 8;
            break;
        case MYSQL_TYPE_FLOAT:
        case MYSQL_TYPE_DOUBLE:
            mr->out[i].buffer_type = MYSQL_TYPE_DOUBLE;
            need = 8;
            break;
        default: {
            /* DECIMAL/DATE 等一律按字符串取，避免精度与时区问题 */
            unsigned long maxlen = mr->fields[i].max_length;
            unsigned long declen = mr->fields[i].length;
            unsigned long want = maxlen > 0 ? maxlen : declen;
            if (want == 0 || want > 8192) want = 8192;
            mr->out[i].buffer_type = MYSQL_TYPE_STRING;
            need = (size_t)want + 1;
            break;
        }
        }
        mr->out_off[i] = total;
        total += need;
    }

    mr->out_buf = calloc(1, total > 0 ? total : 1);
    if (mr->out_buf == NULL) return Q_ERR_NOMEM;

    for (int i = 0; i < mr->ncols; i++) {
        char *p = mr->out_buf + mr->out_off[i];
        size_t cap = (i + 1 < mr->ncols) ? (mr->out_off[i + 1] - mr->out_off[i])
                                         : (total - mr->out_off[i]);
        mr->out[i].buffer        = p;
        mr->out[i].buffer_length = (unsigned long)cap;
        mr->out[i].length        = &mr->out_len[i];
        mr->out[i].is_null       = &mr->out_null[i];
    }

    if (mysql_stmt_bind_result(mr->stmt, mr->out) != 0) {
        q_error("bind result failed: %s", mysql_stmt_error(mr->stmt));
        return Q_ERR;
    }
    return Q_OK;
}

static int my_stmt_exec(void *stmt, void **res, uint64_t *affected, uint64_t *insert_id,
                        char *err, size_t errlen)
{
    my_stmt_t *ms = stmt;
    if (ms == NULL) return Q_ERR_INVAL;

    if (mysql_stmt_execute(ms->stmt) != 0) {
        copy_err(err, errlen, mysql_stmt_error(ms->stmt));
        return Q_ERR;
    }
    if (affected != NULL)  *affected  = (uint64_t)mysql_stmt_affected_rows(ms->stmt);
    if (insert_id != NULL) *insert_id = (uint64_t)mysql_stmt_insert_id(ms->stmt);

    if (res == NULL) return Q_OK;
    *res = NULL;
    if (ms->ncols <= 0) return Q_OK;

    if (mysql_stmt_store_result(ms->stmt) != 0) {
        copy_err(err, errlen, mysql_stmt_error(ms->stmt));
        return Q_ERR;
    }

    my_result_t *mr = calloc(1, sizeof(my_result_t));
    if (mr == NULL) return Q_ERR_NOMEM;

    mr->kind   = 1;
    mr->stmt   = ms->stmt;
    mr->ncols  = ms->ncols;
    mr->names  = calloc((size_t)mr->ncols, sizeof(char *));

    MYSQL_RES *meta = mysql_stmt_result_metadata(ms->stmt);
    if (meta == NULL || mr->names == NULL) {
        if (meta != NULL) mysql_free_result(meta);
        my_res_free(mr);
        return Q_ERR;
    }
    mr->res    = meta;
    mr->fields = mysql_fetch_fields(meta);
    for (int i = 0; i < mr->ncols; i++) mr->names[i] = mr->fields[i].name;

    if (bind_out_buffers(mr) != Q_OK) {
        my_res_free(mr);
        copy_err(err, errlen, "bind result buffers failed");
        return Q_ERR;
    }
    *res = mr;
    return Q_OK;
}

/* ---------------- 方言与注册 ---------------- */

static void mysql_paginate(char *buf, size_t cap, long long offset, long long limit)
{
    if (limit <= 0) return;
    if (offset <= 0) {
        snprintf(buf, cap, " LIMIT %lld", limit);
    } else {
        snprintf(buf, cap, " LIMIT %lld,%lld", offset, limit);
    }
}

static const q_db_ops_t mysql_ops = {
    my_connect,
    my_close,
    my_ping,
    my_exec,
    my_prepare,
    my_stmt_close,
    my_stmt_bind,
    my_stmt_exec,
    my_res_cols,
    my_res_row,
    my_res_free
};

static const q_dialect_t mysql_dialect = {
    "mysql",
    '?',
    0,                  /* 不支持 RETURNING */
    mysql_paginate
};

static const q_db_driver_t mysql_driver = {
    "mysql",
    &mysql_ops,
    &mysql_dialect
};

int q_db_register_mysql(void)
{
    return q_db_register(&mysql_driver);
}
