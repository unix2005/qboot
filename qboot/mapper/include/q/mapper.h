#ifndef Q_MAPPER_H
#define Q_MAPPER_H

#include <q/db.h>

#include <jansson.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * XML SQL Mapper：SQL 全部外置在 XML，改 SQL 不改 C 代码。
 * 启动期把 XML 解析成内存 AST，运行期只做动态标签求值和参数绑定。
 */

typedef struct q_mapper q_mapper_t;
typedef struct q_ctx    q_ctx_t;

/* 启动期：加载目录下所有 *.xml（非递归）或单个 .xml 文件 */
q_mapper_t *q_mapper_load(const char *path);
void        q_mapper_free(q_mapper_t *m);
int         q_mapper_size(const q_mapper_t *m);
int         q_mapper_has(const q_mapper_t *m, const char *id);

/* 运行期：一个 ctx 绑定 mapper + 连接池，ctx 不跨线程共享 */
q_ctx_t *q_ctx_new(q_mapper_t *m, q_dbp_t *pool);
void     q_ctx_free(q_ctx_t *c);
void     q_ctx_debug(q_ctx_t *c, int on);        /* 打印生成的 SQL 与耗时 */

/*
 * params 是 json 对象，键名对应 XML 里 #{name} / ${name}。
 * query 返回结果集（json 数组，每行一个对象）；exec 用于 insert/update/delete。
 */
int q_ctx_query(q_ctx_t *c, const char *id, json_t *params, json_t **out,
                char *err, size_t errlen);
int q_ctx_exec(q_ctx_t *c, const char *id, json_t *params,
               uint64_t *affected, uint64_t *insert_id, char *err, size_t errlen);
int q_ctx_insert(q_ctx_t *c, const char *id, json_t *params, uint64_t *insert_id,
                 char *err, size_t errlen);

/* 事务：内部走 ThreadLocal 连接，commit/rollback 后自动归还 */
q_conn_t *q_ctx_tx(q_ctx_t *c, char *err, size_t errlen);

/* ---------------- 结果集 → struct ---------------- */

typedef enum {
    Q_F_INT = 0,
    Q_F_INT64,
    Q_F_DOUBLE,
    Q_F_STR
} q_field_type_t;

typedef struct {
    const char    *name;      /* 列名 */
    q_field_type_t type;
    size_t         offset;    /* struct 内偏移 */
    size_t         size;      /* 目标字段字节数（字符串用） */
} q_field_t;

#define Q_FIELD(type, member, ftype) \
    { #member, (ftype), offsetof(type, member), sizeof(((type *)0)->member) }
#define Q_FIELD_END { NULL, 0, 0, 0 }

/* out 为数组首地址，stride = sizeof(结构体)，cap 为数组容量；返回行数 */
int q_ctx_query_struct(q_ctx_t *c, const char *id, json_t *params,
                       const q_field_t *fields, void *out,
                       size_t stride, size_t cap, char *err, size_t errlen);

#ifdef __cplusplus
}
#endif

#endif /* Q_MAPPER_H */
