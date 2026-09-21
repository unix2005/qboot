/*
 * 动态 SQL 构建 + 执行 + 结果映射。
 * 运行期不做 XML 解析：启动期已经把 mapper 解析成 AST，这里只做求值、绑定、执行。
 */

#include "q_mapper_int.h"

#include <q/core/mem.h>
#include <q/core/str.h>
#include <q/core/time.h>
#include <q/core/types.h>
#include <q/log.h>

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#define Q_MAX_PARAMS     64
#define Q_STMT_CACHE_MAX 512

struct q_mapper {
    q_hash_t *stmts;        /* id -> q_stmt_def_t* */
};

struct q_ctx {
    q_mapper_t *mapper;
    q_dbp_t    *pool;
    int         debug;
};

/* ---------------- SQL 构建 ---------------- */

typedef struct {
    q_str_t   sql;
    q_value_t params[Q_MAX_PARAMS];
    int       nparams;
    char     *strbufs[Q_MAX_PARAMS];      /* 字符串参数的持久副本 */
    int       nstrbufs;
} build_t;

static void build_init(build_t *b)
{
    memset(b, 0, sizeof(*b));
    q_str_init(&b->sql, 256);
}

static void build_free(build_t *b)
{
    for (int i = 0; i < b->nstrbufs; i++) free(b->strbufs[i]);
    q_str_free(&b->sql);
}

static void add_param(build_t *b, json_t *j)
{
    char buf[64];

    if (b->nparams >= Q_MAX_PARAMS) {
        q_warn("mapper: too many params (>%d), ignored", Q_MAX_PARAMS);
        return;
    }

    q_value_t *slot = &b->params[b->nparams++];

    if (j == NULL || json_is_null(j)) {
        *slot = q_val_null();
        return;
    }
    if (json_is_integer(j)) {
        *slot = q_val_int(json_integer_value(j));
        return;
    }
    if (json_is_real(j)) {
        *slot = q_val_double(json_real_value(j));
        return;
    }
    if (json_is_string(j)) {
        char *s = strdup(json_string_value(j));
        if (s == NULL) {
            *slot = q_val_null();
            return;
        }
        if (b->nstrbufs < Q_MAX_PARAMS) b->strbufs[b->nstrbufs++] = s;
        *slot = q_val_str(s);
        return;
    }
    if (json_is_true(j))  { *slot = q_val_int(1); return; }
    if (json_is_false(j)) { *slot = q_val_int(0); return; }

    /* 对象/数组：转成 JSON 文本塞进去，避免静默丢参数 */
    const char *txt = json_dumps(j, JSON_COMPACT);
    if (txt != NULL) {
        snprintf(buf, sizeof(buf), "%s", txt);
        char *s = strdup(buf);
        if (s != NULL && b->nstrbufs < Q_MAX_PARAMS) {
            b->strbufs[b->nstrbufs++] = s;
            *slot = q_val_str(s);
        } else {
            *slot = q_val_null();
        }
    } else {
        *slot = q_val_null();
    }
}

static void sql_tidy(q_str_t *s);        /* 定义在下面，foreach 展开时要用 */

/* 扫描 #{name}（参数绑定）与 ${name}（直接拼接，有注入风险，打 warning） */
static void emit_text(build_t *b, const char *text, json_t *scope)
{
    const char *p = text;

    while (*p != '\0') {
        const char *hash   = strchr(p, '#');
        const char *dollar = strchr(p, '$');
        const char *ph     = NULL;
        char        kind   = 0;

        if (hash != NULL && (dollar == NULL || hash < dollar)) {
            ph = hash;
            kind = '#';
        } else if (dollar != NULL) {
            ph = dollar;
            kind = '$';
        }
        if (ph == NULL || ph[1] != '{') {
            q_str_append_cstr(&b->sql, p);
            return;
        }

        const char *end = strchr(ph + 2, '}');
        if (end == NULL) {
            q_str_append_cstr(&b->sql, p);
            return;
        }

        q_str_append(&b->sql, p, (size_t)(ph - p));

        size_t nlen = (size_t)(end - ph - 2);
        char   name[128];
        if (nlen >= sizeof(name)) nlen = sizeof(name) - 1;
        memcpy(name, ph + 2, nlen);
        name[nlen] = '\0';
        /* 允许 #{ name } 这种带空格的写法 */
        {
            char *b0 = name;
            while (*b0 == ' ' || *b0 == '\t') b0++;
            size_t l = strlen(b0);
            while (l > 0 && (b0[l - 1] == ' ' || b0[l - 1] == '\t')) l--;
            memmove(name, b0, l);
            name[l] = '\0';
        }

        json_t *j = q_expr_path(scope, name);

        if (kind == '#') {
            q_str_append_cstr(&b->sql, "?");
            add_param(b, j);
        } else {
            const char *s = (j != NULL && json_is_string(j)) ? json_string_value(j) : "";
            q_warn("mapper: ${%s} 直接拼接有注入风险，仅用于白名单标识符", name);
            q_str_append_cstr(&b->sql, s ? s : "");
        }

        p = end + 1;
    }
}

static void build_node(build_t *b, q_node_t *n, json_t *scope, int depth)
{
    if (b == NULL || n == NULL || depth > 16) return;

    switch (n->kind) {
    case QN_TEXT:
        emit_text(b, n->text ? n->text : "", scope);
        break;

    case QN_GROUP:
    case QN_INCLUDE:
        for (int i = 0; i < n->nkids; i++) build_node(b, n->kids[i], scope, depth + 1);
        break;

    case QN_IF: {
        int ok = 0;
        if (q_expr_eval(n->test ? n->test : "", scope, &ok) && ok) {
            for (int i = 0; i < n->nkids; i++) build_node(b, n->kids[i], scope, depth + 1);
        }
        break;
    }

    case QN_CHOOSE: {
        int taken = 0;
        for (int i = 0; i < n->nkids && !taken; i++) {
            q_node_t *k = n->kids[i];
            if (k->kind == QN_WHEN) {
                int ok = 0;
                if (q_expr_eval(k->test ? k->test : "", scope, &ok) && ok) {
                    for (int j = 0; j < k->nkids; j++) build_node(b, k->kids[j], scope, depth + 1);
                    taken = 1;
                }
            }
        }
        if (!taken) {
            for (int i = 0; i < n->nkids; i++) {
                if (n->kids[i]->kind == QN_OTHERWISE) {
                    q_node_t *k = n->kids[i];
                    for (int j = 0; j < k->nkids; j++) build_node(b, k->kids[j], scope, depth + 1);
                    break;
                }
            }
        }
        break;
    }

    case QN_WHERE: {
        q_str_t saved = b->sql;
        q_str_t tmp;
        if (q_str_init(&tmp, 128) != 0) break;
        b->sql = tmp;
        for (int i = 0; i < n->nkids; i++) build_node(b, n->kids[i], scope, depth + 1);
        tmp = b->sql;
        b->sql = saved;

        char  *s   = tmp.data ? tmp.data : "";
        size_t len = tmp.len;
        while (len > 0 && isspace((unsigned char)s[len - 1])) len--;
        size_t start = 0;
        while (start < len && isspace((unsigned char)s[start])) start++;

        if (len > start) {
            if (strncasecmp(s + start, "AND ", 4) == 0)      start += 4;
            else if (strncasecmp(s + start, "OR ", 3) == 0)  start += 3;
            while (start < len && isspace((unsigned char)s[start])) start++;

            if (len > start) {
                q_str_append_cstr(&b->sql, " WHERE ");
                q_str_append(&b->sql, s + start, len - start);
            }
        }
        q_str_free(&tmp);
        break;
    }

    case QN_SET: {
        q_str_t saved = b->sql;
        q_str_t tmp;
        if (q_str_init(&tmp, 128) != 0) break;
        b->sql = tmp;
        for (int i = 0; i < n->nkids; i++) build_node(b, n->kids[i], scope, depth + 1);
        tmp = b->sql;
        b->sql = saved;

        char  *s   = tmp.data ? tmp.data : "";
        size_t len = tmp.len;
        while (len > 0 && isspace((unsigned char)s[len - 1])) len--;
        if (len > 0 && s[len - 1] == ',') len--;
        while (len > 0 && isspace((unsigned char)s[len - 1])) len--;
        size_t start = 0;
        while (start < len && isspace((unsigned char)s[start])) start++;

        if (len > start) {
            q_str_append_cstr(&b->sql, " SET ");
            q_str_append(&b->sql, s + start, len - start);
        }
        q_str_free(&tmp);
        break;
    }

    case QN_FOREACH: {
        json_t *arr = q_expr_path(scope, n->collection ? n->collection : "");
        if (!json_is_array(arr)) break;

        if (n->open != NULL) q_str_append_cstr(&b->sql, n->open);

        size_t  idx;
        json_t *val;
        json_array_foreach(arr, idx, val) {
            if (idx > 0 && n->separator != NULL) q_str_append_cstr(&b->sql, n->separator);

            json_t *ns = json_object();
            if (ns == NULL) break;
            json_object_set(ns, n->item ? n->item : "item", val);

            const char *k;
            json_t     *v;
            if (json_is_object(scope)) {
                json_object_foreach(scope, k, v) json_object_set(ns, k, v);
            }

            /*
             * 每一项单独渲染再 trim，否则 XML 里 #{id} 前后的缩进会带进来，
             * 拼成 "( ? , ? , ? )" 这种多余空格。
             */
            q_str_t saved = b->sql;
            q_str_t item;
            if (q_str_init(&item, 64) != 0) { json_decref(ns); break; }
            b->sql = item;
            for (int i = 0; i < n->nkids; i++) build_node(b, n->kids[i], ns, depth + 1);
            item = b->sql;
            b->sql = saved;
            sql_tidy(&item);
            q_str_append_cstr(&b->sql, item.data ? item.data : "");
            q_str_free(&item);

            json_decref(ns);
        }
        if (n->close != NULL) q_str_append_cstr(&b->sql, n->close);
        break;
    }

    default:
        for (int i = 0; i < n->nkids; i++) build_node(b, n->kids[i], scope, depth + 1);
        break;
    }
}

/*
 * 收尾整理：把连续空白压成一个空格、去掉首尾空白。
 * 引号内的空白原样保留，否则 'a  b' 这种字面量会被改坏。
 */
static void sql_tidy(q_str_t *s)
{
    if (s == NULL || s->data == NULL) return;

    char  *src = s->data;
    size_t n   = s->len;
    size_t j   = 0;
    int    quote = 0;          /* 0=不在引号内，否则为引号字符 */
    int    pending = 0;

    for (size_t i = 0; i < n; i++) {
        char c = src[i];

        if (quote != 0) {
            src[j++] = c;
            if (c == quote) quote = 0;
            continue;
        }
        if (c == '\'' || c == '"') {
            quote = c;
            if (pending) { src[j++] = ' '; pending = 0; }
            src[j++] = c;
            continue;
        }
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            if (j > 0) pending = 1;
            continue;
        }
        if (pending) { src[j++] = ' '; pending = 0; }
        src[j++] = c;
    }
    src[j] = '\0';
    s->len = j;
}

/* ---------------- 语句缓存（挂在连接上） ---------------- */

static void close_cached(const char *key, void *val, void *ud)
{
    (void)key;
    (void)ud;
    q_stmt_close((q_stmt_t *)val);
}

static void cache_free(void *ud)
{
    q_hash_t *h = ud;
    if (h == NULL) return;
    q_hash_foreach(h, close_cached, NULL);
    q_hash_free(h);
}

static q_stmt_t *stmt_cached(q_conn_t *conn, const char *sql)
{
    if (conn->ud == NULL) {
        conn->ud      = q_hash_new(16);
        conn->ud_free = cache_free;
        if (conn->ud == NULL) return NULL;
    }
    return q_hash_get(conn->ud, sql);
}

static void stmt_cache_put(q_conn_t *conn, const char *sql, q_stmt_t *st)
{
    if (conn->ud == NULL) return;
    if (q_hash_size(conn->ud) >= Q_STMT_CACHE_MAX) {
        q_warn("mapper: statement cache full (%d), not caching", Q_STMT_CACHE_MAX);
        return;
    }
    q_hash_set(conn->ud, sql, st);
}

/* ---------------- mapper 生命周期 ---------------- */

static void free_stmt_def(const char *key, void *val, void *ud)
{
    (void)key;
    (void)ud;
    q_stmt_def_t *sd = val;
    q_node_free(sd->root);
    free(sd);
}

q_mapper_t *q_mapper_load(const char *path)
{
    struct stat st;
    q_array_t   files;
    q_array_t   defs;
    q_mapper_t *m;
    char        err[256];

    if (path == NULL || stat(path, &st) != 0) return NULL;

    m = calloc(1, sizeof(q_mapper_t));
    if (m == NULL) return NULL;
    m->stmts = q_hash_new(64);
    if (m->stmts == NULL) {
        free(m);
        return NULL;
    }

    q_array_init(&files, 8);
    q_array_init(&defs, 16);

    if (S_ISDIR(st.st_mode)) {
        DIR *d = opendir(path);
        if (d != NULL) {
            struct dirent *e;
            while ((e = readdir(d)) != NULL) {
                const char *nm = e->d_name;
                size_t len = strlen(nm);
                if (len < 5 || strcmp(nm + len - 4, ".xml") != 0) continue;
                char full[1024];
                snprintf(full, sizeof(full), "%s/%s", path, nm);
                q_array_push(&files, strdup(full));
            }
            closedir(d);
        }
    } else {
        q_array_push(&files, strdup(path));
    }

    for (size_t i = 0; i < files.len; i++) {
        char *f = files.items[i];
        if (q_mapper_parse_file(f, &defs, err, sizeof(err)) != Q_OK) {
            q_error("mapper load failed: %s (%s)", f, err);
        } else {
            q_info("mapper loaded: %s", f);
        }
        free(f);
    }
    q_array_free(&files);

    for (size_t i = 0; i < defs.len; i++) {
        q_stmt_def_t *sd = defs.items[i];
        q_hash_set(m->stmts, sd->id, sd);
    }
    q_array_free(&defs);

    if (q_hash_size(m->stmts) == 0) {
        q_error("mapper: no statement loaded from %s", path);
    }
    return m;
}

void q_mapper_free(q_mapper_t *m)
{
    if (m == NULL) return;
    q_hash_foreach(m->stmts, free_stmt_def, NULL);
    q_hash_free(m->stmts);
    free(m);
}

int q_mapper_size(const q_mapper_t *m) { return (int)q_hash_size(m->stmts); }

int q_mapper_has(const q_mapper_t *m, const char *id)
{
    return (m != NULL && id != NULL && q_hash_get(m->stmts, id) != NULL) ? 1 : 0;
}

q_ctx_t *q_ctx_new(q_mapper_t *m, q_dbp_t *pool)
{
    q_ctx_t *c;
    if (m == NULL) return NULL;

    c = calloc(1, sizeof(q_ctx_t));
    if (c == NULL) return NULL;
    c->mapper = m;
    c->pool   = pool;
    return c;
}

void q_ctx_free(q_ctx_t *c) { free(c); }
void q_ctx_debug(q_ctx_t *c, int on) { if (c != NULL) c->debug = on ? 1 : 0; }

q_conn_t *q_ctx_tx(q_ctx_t *c, char *err, size_t errlen)
{
    if (c == NULL || c->pool == NULL) return NULL;
    return q_dbp_tx(c->pool, err, errlen);
}

/* ---------------- 执行 ---------------- */

static int exec_prepared(q_ctx_t *c, q_stmt_def_t *sd, json_t *params,
                         q_result_t **out, uint64_t *affected, uint64_t *insert_id,
                         char *err, size_t errlen)
{
    build_t    b;
    q_conn_t  *conn = NULL;
    q_stmt_t  *st   = NULL;
    q_result_t *res = NULL;
    char       perr[256];
    int        rc;
    int64_t    t0;

    build_init(&b);
    build_node(&b, sd->root, params, 0);
    sql_tidy(&b.sql);

    t0 = q_time_now_ms();
    if (c->debug) {
        q_info_cat("sql", "%s | params=%d", b.sql.data ? b.sql.data : "", b.nparams);
    }

    conn = q_dbp_get(c->pool, perr, sizeof(perr));
    if (conn == NULL) {
        snprintf(err, errlen, "get connection failed: %s", perr);
        build_free(&b);
        return Q_ERR;
    }

    st = stmt_cached(conn, b.sql.data);
    if (st == NULL) {
        int np = 0;
        rc = q_conn_prepare(conn, &st, b.sql.data, &np, perr, sizeof(perr));
        if (rc != Q_OK) {
            snprintf(err, errlen, "prepare failed: %s | sql=%s", perr, b.sql.data);
            q_dbp_put(conn);
            build_free(&b);
            return rc;
        }
        stmt_cache_put(conn, b.sql.data, st);
    }

    rc = q_stmt_bind(st, b.params, b.nparams, perr, sizeof(perr));
    if (rc != Q_OK) {
        snprintf(err, errlen, "bind failed: %s", perr);
        q_dbp_put(conn);
        build_free(&b);
        return rc;
    }

    rc = q_stmt_query(st, &res, perr, sizeof(perr));
    int64_t cost = q_time_now_ms() - t0;
    if (cost > 200) {
        q_warn_cat("sql", "slow mapper %lldms: %s", (long long)cost,
                   b.sql.data ? b.sql.data : "");
    }
    if (rc != Q_OK) {
        snprintf(err, errlen, "exec failed: %s | sql=%s", perr, b.sql.data);
        q_result_free(res);
        q_dbp_put(conn);
        build_free(&b);
        return rc;
    }

    /* affected / insert_id 由通用层从 result 里带出来 */
    if (affected != NULL && res != NULL)  *affected  = res->affected;
    if (insert_id != NULL && res != NULL) *insert_id = res->insert_id;

    q_dbp_put(conn);

    if (out != NULL) {
        *out = res;
    } else {
        q_result_free(res);
    }
    build_free(&b);
    return Q_OK;
}

int q_ctx_query(q_ctx_t *c, const char *id, json_t *params, json_t **out,
                char *err, size_t errlen)
{
    q_stmt_def_t *sd;
    q_result_t   *res = NULL;
    json_t       *rows;
    int           rc;

    if (c == NULL || id == NULL || out == NULL) return Q_ERR_INVAL;
    *out = NULL;

    sd = q_hash_get(c->mapper->stmts, id);
    if (sd == NULL) {
        snprintf(err, errlen, "statement not found: %s", id);
        return Q_ERR_NOTFOUND;
    }

    rc = exec_prepared(c, sd, params, &res, NULL, NULL, err, errlen);
    if (rc != Q_OK) return rc;

    rows = json_array();
    if (rows == NULL) {
        q_result_free(res);
        snprintf(err, errlen, "out of memory");
        return Q_ERR_NOMEM;
    }

    while (q_result_next(res) == 1) {
        json_t *row = json_object();
        if (row == NULL) break;
        for (int i = 0; i < res->ncols; i++) {
            q_value_t *v = &res->row[i];
            const char *name = res->names[i] ? res->names[i] : "";
            json_t *jv;
            switch (v->type) {
            case Q_VAL_INT:    jv = json_integer(v->i64); break;
            case Q_VAL_DOUBLE: jv = json_real(v->dbl); break;
            case Q_VAL_STRING:
            case Q_VAL_BLOB:   jv = json_stringn(v->str ? v->str : "", v->len); break;
            default:           jv = json_null(); break;
            }
            json_object_set_new(row, name, jv);
        }
        json_array_append_new(rows, row);
    }
    q_result_free(res);

    *out = rows;
    return Q_OK;
}

int q_ctx_exec(q_ctx_t *c, const char *id, json_t *params,
               uint64_t *affected, uint64_t *insert_id, char *err, size_t errlen)
{
    q_stmt_def_t *sd;

    if (c == NULL || id == NULL) return Q_ERR_INVAL;

    sd = q_hash_get(c->mapper->stmts, id);
    if (sd == NULL) {
        snprintf(err, errlen, "statement not found: %s", id);
        return Q_ERR_NOTFOUND;
    }
    return exec_prepared(c, sd, params, NULL, affected, insert_id, err, errlen);
}

int q_ctx_insert(q_ctx_t *c, const char *id, json_t *params, uint64_t *insert_id,
                 char *err, size_t errlen)
{
    uint64_t affected = 0;
    return q_ctx_exec(c, id, params, &affected, insert_id, err, errlen);
}

int q_ctx_query_struct(q_ctx_t *c, const char *id, json_t *params,
                       const q_field_t *fields, void *out, size_t stride,
                       size_t cap, char *err, size_t errlen)
{
    q_stmt_def_t *sd;
    q_result_t   *res = NULL;
    int           rc;
    size_t        n = 0;

    if (c == NULL || id == NULL || fields == NULL || out == NULL) return Q_ERR_INVAL;

    sd = q_hash_get(c->mapper->stmts, id);
    if (sd == NULL) {
        snprintf(err, errlen, "statement not found: %s", id);
        return Q_ERR_NOTFOUND;
    }

    rc = exec_prepared(c, sd, params, &res, NULL, NULL, err, errlen);
    if (rc != Q_OK) return rc;

    while (n < cap && q_result_next(res) == 1) {
        char *base = (char *)out + n * stride;
        for (int i = 0; i < res->ncols; i++) {
            const char *name = res->names[i] ? res->names[i] : "";
            const q_field_t *f = NULL;
            for (const q_field_t *p = fields; p->name != NULL; p++) {
                if (strcasecmp(p->name, name) == 0) {
                    f = p;
                    break;
                }
            }
            if (f == NULL) continue;

            q_value_t *v = &res->row[i];
            char      *dst = base + f->offset;
            switch (f->type) {
            case Q_F_INT:    *(int *)dst     = (int)q_val_as_int(v); break;
            case Q_F_INT64:  *(int64_t *)dst = q_val_as_int(v); break;
            case Q_F_DOUBLE: *(double *)dst  = q_val_as_double(v); break;
            case Q_F_STR:
                if (f->size > 0) snprintf(dst, f->size, "%s", q_val_as_str(v));
                break;
            default: break;
            }
        }
        n++;
    }
    q_result_free(res);
    return (int)n;
}
