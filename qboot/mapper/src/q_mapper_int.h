#ifndef Q_MAPPER_INT_H
#define Q_MAPPER_INT_H

#include <q/core/ds.h>
#include <q/mapper.h>

/* ---------------- AST ---------------- */

typedef enum {
    QN_TEXT = 0,
    QN_GROUP,       /* 纯容器：<select> 根节点 / <sql> 片段 */
    QN_IF,
    QN_CHOOSE,
    QN_WHEN,
    QN_OTHERWISE,
    QN_WHERE,
    QN_SET,
    QN_FOREACH,
    QN_INCLUDE
} q_node_kind_t;

typedef struct q_node {
    q_node_kind_t   kind;
    char           *text;        /* QN_TEXT：SQL 片段 */
    char           *test;        /* QN_IF / QN_WHEN：表达式 */
    char           *collection;  /* QN_FOREACH */
    char           *item;
    char           *open;
    char           *close;
    char           *separator;
    char           *refid;       /* QN_INCLUDE */
    struct q_node **kids;
    int             nkids;
} q_node_t;

typedef struct {
    char      id[160];
    int       is_select;
    int       use_keys;
    char      key_property[64];
    q_node_t *root;
} q_stmt_def_t;

q_node_t *q_node_new(q_node_kind_t kind);
q_node_t *q_node_clone(const q_node_t *n);       /* <include> 展开时用，避免共享节点 */
void      q_node_free(q_node_t *n);

/* ---------------- loader.c ---------------- */

/* 解析单个 mapper 文件，语句 push 到 out（q_array_t<q_stmt_def_t*>） */
int q_mapper_parse_file(const char *path, q_array_t *out, char *err, size_t errlen);

/* ---------------- expr.c ---------------- */

/* 求值 test 表达式，返回真/假；ok=0 表示表达式无法解析 */
int q_expr_eval(const char *expr, json_t *params, int *ok);

/* 按 "a.b.c" 路径从 json 里取值 */
json_t *q_expr_path(json_t *root, const char *path);

#endif /* Q_MAPPER_INT_H */
