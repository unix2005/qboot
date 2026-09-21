#include <q/db.h>

#include <q/core/time.h>
#include <q/core/types.h>
#include <q/log.h>

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define Q_DB_DRIVER_MAX 8
#define Q_DB_POOL_MAX   8

/* ---------------- 驱动注册表 ---------------- */

static const q_db_driver_t *g_drivers[Q_DB_DRIVER_MAX];
static int                  g_ndrivers = 0;
static pthread_mutex_t      g_reg_mu = PTHREAD_MUTEX_INITIALIZER;

int q_db_register(const q_db_driver_t *drv)
{
    if (drv == NULL || drv->name == NULL || drv->ops == NULL) return Q_ERR_INVAL;

    pthread_mutex_lock(&g_reg_mu);
    for (int i = 0; i < g_ndrivers; i++) {
        if (strcmp(g_drivers[i]->name, drv->name) == 0) {
            pthread_mutex_unlock(&g_reg_mu);
            return Q_ERR_EXIST;
        }
    }
    if (g_ndrivers >= Q_DB_DRIVER_MAX) {
        pthread_mutex_unlock(&g_reg_mu);
        return Q_ERR_BUSY;
    }
    g_drivers[g_ndrivers++] = drv;
    pthread_mutex_unlock(&g_reg_mu);
    return Q_OK;
}

const q_db_driver_t *q_db_find(const char *name)
{
    const q_db_driver_t *found = NULL;
    if (name == NULL) return NULL;

    pthread_mutex_lock(&g_reg_mu);
    for (int i = 0; i < g_ndrivers; i++) {
        if (strcmp(g_drivers[i]->name, name) == 0) {
            found = g_drivers[i];
            break;
        }
    }
    pthread_mutex_unlock(&g_reg_mu);
    return found;
}

/* ---------------- 值 ---------------- */

q_value_t q_val_int(int64_t v)
{
    q_value_t x;
    memset(&x, 0, sizeof(x));
    x.type = Q_VAL_INT;
    x.i64  = v;
    return x;
}

q_value_t q_val_double(double v)
{
    q_value_t x;
    memset(&x, 0, sizeof(x));
    x.type = Q_VAL_DOUBLE;
    x.dbl  = v;
    return x;
}

q_value_t q_val_str(const char *s)
{
    q_value_t x;
    memset(&x, 0, sizeof(x));
    x.type = s ? Q_VAL_STRING : Q_VAL_NULL;
    x.str  = s;
    x.len  = s ? strlen(s) : 0;
    return x;
}

q_value_t q_val_blob(const void *p, size_t n)
{
    q_value_t x;
    memset(&x, 0, sizeof(x));
    x.type = Q_VAL_BLOB;
    x.str  = (const char *)p;
    x.len  = n;
    return x;
}

q_value_t q_val_null(void)
{
    q_value_t x;
    memset(&x, 0, sizeof(x));
    x.type = Q_VAL_NULL;
    return x;
}

int q_val_is_null(const q_value_t *v)
{
    return (v == NULL) || (v->type == Q_VAL_NULL) || (v->str == NULL && v->i64 == 0 && v->type != Q_VAL_DOUBLE);
}

int64_t q_val_as_int(const q_value_t *v)
{
    if (v == NULL) return 0;
    switch (v->type) {
    case Q_VAL_INT:    return v->i64;
    case Q_VAL_DOUBLE: return (int64_t)v->dbl;
    case Q_VAL_STRING:
    case Q_VAL_BLOB:   return v->str ? atoll(v->str) : 0;
    default:           return 0;
    }
}

double q_val_as_double(const q_value_t *v)
{
    if (v == NULL) return 0.0;
    switch (v->type) {
    case Q_VAL_DOUBLE: return v->dbl;
    case Q_VAL_INT:    return (double)v->i64;
    case Q_VAL_STRING:
    case Q_VAL_BLOB:   return v->str ? atof(v->str) : 0.0;
    default:           return 0.0;
    }
}

const char *q_val_as_str(const q_value_t *v)
{
    static __thread char buf[64];
    if (v == NULL) return "";
    switch (v->type) {
    case Q_VAL_STRING:
    case Q_VAL_BLOB:   return v->str ? v->str : "";
    case Q_VAL_INT:    snprintf(buf, sizeof(buf), "%lld", (long long)v->i64); return buf;
    case Q_VAL_DOUBLE: snprintf(buf, sizeof(buf), "%g", v->dbl); return buf;
    default:           return "";
    }
}

/* ---------------- DSN ---------------- */

static int default_port(const char *driver)
{
    if (driver == NULL) return 3306;
    if (strcmp(driver, "mysql") == 0)  return 3306;
    if (strcmp(driver, "oracle") == 0) return 1521;
    if (strcmp(driver, "dameng") == 0) return 5236;
    return 3306;
}

static void copy_field(char *dst, size_t cap, const char *src, size_t n)
{
    if (n >= cap) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

int q_dsn_parse(const char *url, q_dsn_t *out, char *err, size_t errlen)
{
    if (url == NULL || out == NULL) return Q_ERR_INVAL;

    memset(out, 0, sizeof(*out));

    const char *scheme_end = strstr(url, "://");
    if (scheme_end == NULL) {
        snprintf(err, errlen, "bad dsn, expect scheme://: %s", url);
        return Q_ERR_INVAL;
    }
    copy_field(out->driver, sizeof(out->driver), url, (size_t)(scheme_end - url));

    const char *p = scheme_end + 3;
    const char *at = strchr(p, '@');
    const char *host = p;

    if (at != NULL) {
        const char *colon = memchr(p, ':', (size_t)(at - p));
        if (colon != NULL) {
            copy_field(out->user, sizeof(out->user), p, (size_t)(colon - p));
            copy_field(out->pass, sizeof(out->pass), colon + 1,
                       (size_t)(at - colon - 1));
        } else {
            copy_field(out->user, sizeof(out->user), p, (size_t)(at - p));
        }
        host = at + 1;
    }

    const char *slash = strchr(host, '/');
    const char *hpend = slash ? slash : host + strlen(host);
    const char *pcolon = memchr(host, ':', (size_t)(hpend - host));

    if (pcolon != NULL) {
        copy_field(out->host, sizeof(out->host), host, (size_t)(pcolon - host));
        out->port = atoi(pcolon + 1);
    } else {
        copy_field(out->host, sizeof(out->host), host, (size_t)(hpend - host));
        out->port = default_port(out->driver);
    }
    if (out->port <= 0) out->port = default_port(out->driver);

    if (slash != NULL) {
        const char *q = strchr(slash + 1, '?');
        copy_field(out->name, sizeof(out->name), slash + 1,
                   q ? (size_t)(q - slash - 1) : strlen(slash + 1));
    }
    snprintf(out->charset, sizeof(out->charset), "utf8mb4");
    return Q_OK;
}

/* ---------------- 连接池 ---------------- */

typedef struct q_node {
    q_conn_t        conn;
    struct q_node  *next;
    int64_t         idle_since_ms;
    int             temporary;      /* 池满时临时创建的连接，归还即关闭 */
} q_node_t;

typedef struct {
    pthread_mutex_t mu;
    q_node_t       *idle;
    int             n_total;
    int             n_idle;
} q_shard_t;

struct q_dbp {
    q_dsn_t              dsn;
    const q_db_driver_t *drv;
    int                  max_open;
    int                  idle_secs;
    int                  nshards;
    int                  slot;
    _Atomic unsigned long long queries;
    _Atomic unsigned long long errors;
    _Atomic unsigned long long created;
    q_shard_t            shards[];
};

static _Atomic int  g_pool_seq = 0;
static __thread q_conn_t *t_tx[Q_DB_POOL_MAX];

#define Q_ERRBUF 256

q_dbp_t *q_dbp_new(const char *url, int max_open, int idle_secs, int nshards)
{
    char err[Q_ERRBUF];

    if (url == NULL) return NULL;
    if (max_open <= 0) max_open = 4;
    if (nshards <= 0) nshards = 4;

    q_dbp_t *p = calloc(1, sizeof(q_dbp_t) + (size_t)nshards * sizeof(q_shard_t));
    if (p == NULL) return NULL;

    if (q_dsn_parse(url, &p->dsn, err, sizeof(err)) != Q_OK) {
        q_error("dsn parse failed: %s", err);
        free(p);
        return NULL;
    }
    p->drv = q_db_find(p->dsn.driver);
    if (p->drv == NULL) {
        q_error("driver '%s' not registered", p->dsn.driver);
        free(p);
        return NULL;
    }
    p->max_open  = max_open;
    p->idle_secs = idle_secs;
    p->nshards   = nshards;
    p->slot      = atomic_fetch_add(&g_pool_seq, 1);
    if (p->slot >= Q_DB_POOL_MAX) {
        q_error("too many db pools (max %d)", Q_DB_POOL_MAX);
        free(p);
        return NULL;
    }
    for (int i = 0; i < nshards; i++) pthread_mutex_init(&p->shards[i].mu, NULL);

    q_info("db pool ready: %s@%s:%d/%s shards=%d max=%d",
           p->dsn.user, p->dsn.host, p->dsn.port, p->dsn.name, nshards, max_open);
    return p;
}

/* 前向声明：q_dbp_free 与 conn_destroy 相互引用 */
static void conn_destroy(q_dbp_t *p, q_node_t *n);

void q_dbp_free(q_dbp_t *p)
{
    if (p == NULL) return;

    for (int i = 0; i < p->nshards; i++) {
        q_shard_t *sh = &p->shards[i];
        pthread_mutex_lock(&sh->mu);
        q_node_t *n = sh->idle;
        while (n != NULL) {
            q_node_t *next = n->next;
            conn_destroy(p, n);
            n = next;
        }
        sh->idle = NULL;
        pthread_mutex_unlock(&sh->mu);
        pthread_mutex_destroy(&sh->mu);
    }
    if (p->slot >= 0 && p->slot < Q_DB_POOL_MAX) t_tx[p->slot] = NULL;
    free(p);
}

/* 建一条新连接，失败返回 NULL */
static q_node_t *conn_create(q_dbp_t *p, int temporary, char *err, size_t errlen)
{
    void *handle = NULL;
    int64_t t0 = q_time_now_ms();

    if (p->drv->ops->connect(&handle, &p->dsn, err, errlen) != Q_OK || handle == NULL) {
        return NULL;
    }

    q_node_t *n = calloc(1, sizeof(q_node_t));
    if (n == NULL) {
        p->drv->ops->close(handle);
        return NULL;
    }
    n->conn.pool      = p;
    n->conn.ops       = p->drv->ops;
    n->conn.handle    = handle;
    n->conn.shard     = -1;
    n->conn.last_used_ms = t0;
    n->temporary      = temporary;
    atomic_fetch_add(&p->created, 1);
    return n;
}

static void conn_destroy(q_dbp_t *p, q_node_t *n)
{
    if (p == NULL || n == NULL) return;
    if (n->conn.ud_free != NULL && n->conn.ud != NULL) n->conn.ud_free(n->conn.ud);
    n->conn.ops->close(n->conn.handle);
    free(n);
}

q_conn_t *q_dbp_get(q_dbp_t *p, char *err, size_t errlen)
{
    if (p == NULL) return NULL;

    /* 每个线程固定落在同一分片，减少跨线程竞争 */
    static __thread unsigned long t_seed = 0;
    if (t_seed == 0) t_seed = (unsigned long)(intptr_t)&t_seed;
    int shard = (int)((t_seed >> 4) % (unsigned long)p->nshards);

    q_shard_t *sh = &p->shards[shard];
    pthread_mutex_lock(&sh->mu);

    while (sh->idle != NULL) {
        q_node_t *n = sh->idle;
        sh->idle = n->next;
        sh->n_idle--;

        /* 空闲过久 -> 关掉，避免服务端连接堆积 */
        if (p->idle_secs > 0 &&
            q_time_now_ms() - n->idle_since_ms > (int64_t)p->idle_secs * 1000) {
            sh->n_total--;
            pthread_mutex_unlock(&sh->mu);
            conn_destroy(p, n);
            pthread_mutex_lock(&sh->mu);
            continue;
        }
        pthread_mutex_unlock(&sh->mu);

        /* 拿出去之前探活，断开则重建 */
        char perr[Q_ERRBUF];
        if (n->conn.ops->ping(n->conn.handle, perr, sizeof(perr)) != Q_OK) {
            q_warn("stale db connection dropped: %s", perr);
            conn_destroy(p, n);
            pthread_mutex_lock(&sh->mu);
            sh->n_total--;
            pthread_mutex_unlock(&sh->mu);
            pthread_mutex_lock(&sh->mu);
            continue;
        }
        n->conn.shard = shard;
        n->conn.last_used_ms = q_time_now_ms();
        return &n->conn;
    }

    int can_create = (sh->n_total < p->max_open);
    if (can_create) sh->n_total++;
    pthread_mutex_unlock(&sh->mu);

    q_node_t *n = conn_create(p, !can_create, err, errlen);
    if (n == NULL) {
        if (can_create) {
            pthread_mutex_lock(&sh->mu);
            sh->n_total--;
            pthread_mutex_unlock(&sh->mu);
        }
        return NULL;
    }
    n->conn.shard = can_create ? shard : -1;
    return &n->conn;
}

void q_dbp_put(q_conn_t *c)
{
    if (c == NULL) return;

    q_dbp_t   *p  = c->pool;
    q_node_t  *n  = Q_CONTAINER_OF(c, q_node_t, conn);

    if (n->temporary || c->shard < 0) {
        conn_destroy(p, n);
        return;
    }

    q_shard_t *sh = &p->shards[c->shard];
    pthread_mutex_lock(&sh->mu);
    n->next = sh->idle;
    sh->idle = n;
    sh->n_idle++;
    n->idle_since_ms = q_time_now_ms();
    pthread_mutex_unlock(&sh->mu);
}

q_conn_t *q_dbp_tx(q_dbp_t *p, char *err, size_t errlen)
{
    if (p == NULL) return NULL;
    if (t_tx[p->slot] != NULL) return t_tx[p->slot];

    q_conn_t *c = q_dbp_get(p, err, errlen);
    if (c == NULL) return NULL;

    if (q_tx_begin(c) != Q_OK) {
        q_dbp_put(c);
        return NULL;
    }
    c->tx_owned = 1;
    t_tx[p->slot] = c;
    return c;
}

void q_dbp_stat(q_dbp_t *p, q_dbp_stat_t *out)
{
    if (out == NULL) return;
    memset(out, 0, sizeof(*out));
    if (p == NULL) return;

    int total = 0, idle = 0;
    for (int i = 0; i < p->nshards; i++) {
        pthread_mutex_lock(&p->shards[i].mu);
        total += p->shards[i].n_total;
        idle  += p->shards[i].n_idle;
        pthread_mutex_unlock(&p->shards[i].mu);
    }
    out->in_use = total - idle;
    out->idle   = idle;
    out->queries      = atomic_load(&p->queries);
    out->errors       = atomic_load(&p->errors);
    out->conn_created = atomic_load(&p->created);
}

/* ---------------- 结果集封装 ---------------- */

static q_result_t *result_wrap(const q_db_ops_t *ops, void *drv,
                               uint64_t affected, uint64_t insert_id)
{
    q_result_t *r = calloc(1, sizeof(q_result_t));
    if (r == NULL) {
        ops->res_free(drv);
        return NULL;
    }
    r->drv        = drv;
    r->ops        = ops;
    r->affected   = affected;
    r->insert_id  = insert_id;

    if (ops->res_cols(drv, &r->names, &r->ncols) != Q_OK || r->ncols <= 0) {
        ops->res_free(drv);
        free(r);
        return NULL;
    }
    r->row = calloc((size_t)r->ncols, sizeof(q_value_t));
    if (r->row == NULL) {
        ops->res_free(drv);
        free(r);
        return NULL;
    }
    return r;
}

int q_result_next(q_result_t *r)
{
    char err[Q_ERRBUF];
    if (r == NULL || r->drv == NULL) return Q_ERR_INVAL;

    int rc = r->ops->res_row(r->drv, r->row, r->ncols, err, sizeof(err));
    if (rc < 0) q_error("fetch row failed: %s", err);
    return rc;
}

void q_result_free(q_result_t *r)
{
    if (r == NULL) return;
    if (r->drv != NULL && r->ops != NULL) r->ops->res_free(r->drv);
    free(r->row);
    free(r);
}

/* ---------------- 连接操作 ---------------- */

int q_conn_exec(q_conn_t *c, const char *sql, uint64_t *affected, uint64_t *insert_id,
                char *err, size_t errlen)
{
    uint64_t a = 0, i = 0;
    int rc;

    if (c == NULL || sql == NULL) return Q_ERR_INVAL;

    int64_t t0 = q_time_now_ms();
    rc = c->ops->exec(c->handle, sql, strlen(sql), NULL, &a, &i, err, errlen);
    if (affected != NULL)  *affected  = a;
    if (insert_id != NULL) *insert_id = i;

    if (c->pool != NULL) {
        atomic_fetch_add(&c->pool->queries, 1);
        if (rc != Q_OK) atomic_fetch_add(&c->pool->errors, 1);
    }
    int64_t cost = q_time_now_ms() - t0;
    if (cost > 200) q_warn_cat("sql", "slow exec %lldms: %s", (long long)cost, sql);
    return rc;
}

int q_conn_query(q_conn_t *c, const char *sql, q_result_t **out,
                 char *err, size_t errlen)
{
    void     *drv = NULL;
    uint64_t  a = 0, i = 0;
    int       rc;

    if (c == NULL || sql == NULL || out == NULL) return Q_ERR_INVAL;
    *out = NULL;

    int64_t t0 = q_time_now_ms();
    rc = c->ops->exec(c->handle, sql, strlen(sql), &drv, &a, &i, err, errlen);
    if (c->pool != NULL) {
        atomic_fetch_add(&c->pool->queries, 1);
        if (rc != Q_OK) atomic_fetch_add(&c->pool->errors, 1);
    }
    if (rc != Q_OK) return rc;

    int64_t cost = q_time_now_ms() - t0;
    if (cost > 200) q_warn_cat("sql", "slow query %lldms: %s", (long long)cost, sql);

    if (drv == NULL) return Q_OK;          /* 无结果集 */

    *out = result_wrap(c->ops, drv, a, i);
    if (*out == NULL) {
        snprintf(err, errlen, "wrap result failed");
        return Q_ERR;
    }
    return Q_OK;
}

int q_conn_prepare(q_conn_t *c, q_stmt_t **out, const char *sql, int *nparams,
                   char *err, size_t errlen)
{
    void *drv_stmt = NULL;
    int   np = 0;

    if (c == NULL || sql == NULL || out == NULL) return Q_ERR_INVAL;
    *out = NULL;

    int rc = c->ops->prepare(c->handle, &drv_stmt, sql, strlen(sql), &np, err, errlen);
    if (rc != Q_OK) {
        if (c->pool != NULL) atomic_fetch_add(&c->pool->errors, 1);
        return rc;
    }

    q_stmt_t *s = calloc(1, sizeof(q_stmt_t));
    if (s == NULL) {
        c->ops->stmt_close(drv_stmt);
        return Q_ERR_NOMEM;
    }
    s->conn     = c;
    s->drv_stmt = drv_stmt;
    s->ops      = c->ops;
    s->nparams  = np;

    *out = s;
    if (nparams != NULL) *nparams = np;
    return Q_OK;
}

int q_stmt_bind(q_stmt_t *s, const q_value_t *params, int n, char *err, size_t errlen)
{
    if (s == NULL || params == NULL) return Q_ERR_INVAL;
    if (s->nparams > 0 && n != s->nparams) {
        snprintf(err, errlen, "param count mismatch: need %d got %d", s->nparams, n);
        return Q_ERR_INVAL;
    }
    return s->ops->stmt_bind(s->drv_stmt, params, n, err, errlen);
}

int q_stmt_query(q_stmt_t *s, q_result_t **out, char *err, size_t errlen)
{
    void    *drv = NULL;
    uint64_t a = 0, i = 0;
    int      rc;

    if (s == NULL || out == NULL) return Q_ERR_INVAL;
    *out = NULL;

    int64_t t0 = q_time_now_ms();
    rc = s->ops->stmt_exec(s->drv_stmt, &drv, &a, &i, err, errlen);
    int64_t cost = q_time_now_ms() - t0;
    if (cost > 200) q_warn_cat("sql", "slow stmt %lldms", (long long)cost);

    if (rc != Q_OK) return rc;
    if (drv == NULL) {
        q_result_t *r = calloc(1, sizeof(q_result_t));
        if (r == NULL) return Q_ERR_NOMEM;
        r->ops = s->ops;
        r->affected = a;
        r->insert_id = i;
        *out = r;
        return Q_OK;
    }
    *out = result_wrap(s->ops, drv, a, i);
    return (*out == NULL) ? Q_ERR : Q_OK;
}

int q_stmt_exec(q_stmt_t *s, uint64_t *affected, uint64_t *insert_id,
                char *err, size_t errlen)
{
    uint64_t a = 0, i = 0;
    int      rc;

    if (s == NULL) return Q_ERR_INVAL;

    int64_t t0 = q_time_now_ms();
    rc = s->ops->stmt_exec(s->drv_stmt, NULL, &a, &i, err, errlen);
    int64_t cost = q_time_now_ms() - t0;
    if (cost > 200) q_warn_cat("sql", "slow stmt exec %lldms", (long long)cost);

    if (affected != NULL)  *affected  = a;
    if (insert_id != NULL) *insert_id = i;
    return rc;
}

void q_stmt_close(q_stmt_t *s)
{
    if (s == NULL) return;
    s->ops->stmt_close(s->drv_stmt);
    free(s);
}

/* ---------------- 事务 ---------------- */

int q_tx_begin(q_conn_t *c)
{
    uint64_t a = 0, i = 0;
    char err[Q_ERRBUF];

    if (c == NULL) return Q_ERR_INVAL;
    if (c->ops->exec(c->handle, "START TRANSACTION", 17, NULL, &a, &i, err, sizeof(err)) != Q_OK) {
        q_error("tx begin failed: %s", err);
        return Q_ERR;
    }
    c->in_tx = 1;
    return Q_OK;
}

static int tx_finish(q_conn_t *c, const char *sql)
{
    uint64_t a = 0, i = 0;
    char err[Q_ERRBUF];
    int  rc;

    if (c == NULL) return Q_ERR_INVAL;

    rc = c->ops->exec(c->handle, sql, strlen(sql), NULL, &a, &i, err, sizeof(err));
    if (rc != Q_OK) q_error("tx %s failed: %s", sql, err);

    c->in_tx = 0;
    if (c->pool != NULL && c->pool->slot >= 0 && c->pool->slot < Q_DB_POOL_MAX &&
        t_tx[c->pool->slot] == c) {
        t_tx[c->pool->slot] = NULL;
    }
    q_dbp_put(c);
    return rc;
}

int q_tx_commit(q_conn_t *c)    { return tx_finish(c, "COMMIT"); }
int q_tx_rollback(q_conn_t *c)  { return tx_finish(c, "ROLLBACK"); }
