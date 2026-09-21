/*
 * <if test="..."> 里的表达式求值。
 * 支持：路径(a.b.c)、数字、字符串、null/true/false、== != > >= < <=、and/or/not、括号。
 * 不做完整 SQL 表达式语义，只覆盖 MyBatis 常用判断。
 */

#include "q_mapper_int.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const char *s;
    size_t      i;
    size_t      n;
} scanner_t;

typedef struct {
    int         is_null;
    int         is_num;
    double      num;
    const char *str;
    int         own_str;    /* str 是本函数 malloc 的，用完要 free */
} ev_t;

static void skip_ws(scanner_t *sc)
{
    while (sc->i < sc->n && isspace((unsigned char)sc->s[sc->i])) sc->i++;
}

static int match_kw(scanner_t *sc, const char *kw)
{
    size_t l = strlen(kw);
    skip_ws(sc);
    if (sc->i + l <= sc->n && strncmp(sc->s + sc->i, kw, l) == 0) {
        /* 词边界：and 不能吃掉 android，or 不能吃掉 order */
        unsigned char nx = (sc->i + l < sc->n) ? (unsigned char)sc->s[sc->i + l] : 0;
        if (isalnum(nx) || nx == '_') return 0;
        sc->i += l;
        return 1;
    }
    return 0;
}

static void ev_release(ev_t *v)
{
    if (v != NULL && v->own_str && v->str != NULL) free((void *)v->str);
}

static int peek_ch(scanner_t *sc, char c)
{
    skip_ws(sc);
    return (sc->i < sc->n && sc->s[sc->i] == c);
}

static int eat_ch(scanner_t *sc, char c)
{
    if (peek_ch(sc, c)) {
        sc->i++;
        return 1;
    }
    return 0;
}

json_t *q_expr_path(json_t *root, const char *path)
{
    char buf[256];
    char *save = NULL;
    json_t *cur = root;

    if (root == NULL || path == NULL) return NULL;

    snprintf(buf, sizeof(buf), "%s", path);
    for (char *tok = strtok_r(buf, ".", &save);
         tok != NULL && cur != NULL;
         tok = strtok_r(NULL, ".", &save)) {
        if (json_is_object(cur)) {
            cur = json_object_get(cur, tok);
        } else if (json_is_array(cur)) {
            int idx = atoi(tok);
            cur = json_array_get(cur, (size_t)idx);
        } else {
            return NULL;
        }
    }
    return cur;
}

static ev_t from_json(json_t *j)
{
    ev_t v;
    memset(&v, 0, sizeof(v));

    if (j == NULL || json_is_null(j)) {
        v.is_null = 1;
        return v;
    }
    if (json_is_integer(j))      { v.is_num = 1; v.num = (double)json_integer_value(j); return v; }
    if (json_is_real(j))         { v.is_num = 1; v.num = json_real_value(j); return v; }
    if (json_is_string(j))       { v.str = json_string_value(j); return v; }
    if (json_is_true(j))         { v.is_num = 1; v.num = 1; return v; }
    if (json_is_false(j))        { v.is_num = 1; v.num = 0; return v; }
    /* 数组/对象：非空即为真。<if test="list != null"> 依赖这一点 */
    if (json_is_array(j))        { v.is_num = 1; v.num = (double)json_array_size(j); return v; }
    if (json_is_object(j))       { v.is_num = 1; v.num = (double)json_object_size(j); return v; }
    v.is_null = 1;
    return v;
}

static int to_num(const ev_t *v, double *out)
{
    if (v->is_null) {
        *out = 0;
        return 1;                 /* null 参与数值比较时按 0 处理 */
    }
    if (v->is_num) {
        *out = v->num;
        return 1;
    }
    if (v->str != NULL && v->str[0] != '\0') {
        char *end = NULL;
        double d = strtod(v->str, &end);
        if (end != NULL && *end == '\0') {
            *out = d;
            return 1;
        }
    }
    return 0;
}

static const char *to_str(const ev_t *v)
{
    return (v->str != NULL) ? v->str : "";
}

static ev_t eval_or(scanner_t *sc, json_t *params, int *ok);

static ev_t eval_primary(scanner_t *sc, json_t *params, int *ok)
{
    ev_t v;
    memset(&v, 0, sizeof(v));

    skip_ws(sc);

    if (match_kw(sc, "not")) {
        ev_t x = eval_primary(sc, params, ok);
        int truth = !x.is_null && (x.is_num ? x.num != 0 : (x.str && x.str[0] != '\0'));
        ev_release(&x);
        v.is_num = 1;
        v.num = truth ? 1 : 0;
        return v;
    }
    if (eat_ch(sc, '(')) {
        v = eval_or(sc, params, ok);        /* 可能带 own_str，由上层释放 */
        eat_ch(sc, ')');
        return v;
    }
    if (peek_ch(sc, '\'') || peek_ch(sc, '"')) {
        char quote = sc->s[sc->i];
        sc->i++;
        size_t start = sc->i;
        while (sc->i < sc->n && sc->s[sc->i] != quote) sc->i++;
        size_t len = sc->i - start;
        char *buf = malloc(len + 1);
        if (buf == NULL) {
            *ok = 0;
            return v;
        }
        memcpy(buf, sc->s + start, len);
        buf[len] = '\0';
        if (sc->i < sc->n) sc->i++;
        v.str = buf;
        v.own_str = 1;            /* 字面量是 malloc 出来的，用完要释放 */
        v.is_null = 0;
        v.is_num  = 0;
        return v;
    }

    if (match_kw(sc, "null")) {
        v.is_null = 1;
        return v;
    }
    if (match_kw(sc, "true"))  { v.is_num = 1; v.num = 1; return v; }
    if (match_kw(sc, "false")) { v.is_num = 1; v.num = 0; return v; }

    /* 数字 */
    skip_ws(sc);
    if (sc->i < sc->n && (isdigit((unsigned char)sc->s[sc->i]) || sc->s[sc->i] == '-')) {
        char *end = NULL;
        double d = strtod(sc->s + sc->i, &end);
        if (end != NULL && end > sc->s + sc->i) {
            sc->i += (size_t)(end - (sc->s + sc->i));
            v.is_num = 1;
            v.num = d;
            return v;
        }
    }

    /* 路径 */
    skip_ws(sc);
    size_t start = sc->i;
    while (sc->i < sc->n &&
           (isalnum((unsigned char)sc->s[sc->i]) || sc->s[sc->i] == '_' || sc->s[sc->i] == '.')) {
        sc->i++;
    }
    if (sc->i == start) {
        *ok = 0;
        return v;
    }
    char name[256];
    size_t len = sc->i - start;
    if (len >= sizeof(name)) len = sizeof(name) - 1;
    memcpy(name, sc->s + start, len);
    name[len] = '\0';

    json_t *j = q_expr_path(params, name);
    if (j == NULL) {
        v.is_null = 1;
        return v;
    }
    return from_json(j);
}

static int cmp_op(scanner_t *sc, char *op, size_t opcap)
{
    skip_ws(sc);
    static const char *ops[] = { "==", "!=", ">=", "<=", ">", "<" };
    for (size_t k = 0; k < sizeof(ops) / sizeof(ops[0]); k++) {
        size_t l = strlen(ops[k]);
        if (sc->i + l <= sc->n && strncmp(sc->s + sc->i, ops[k], l) == 0) {
            sc->i += l;
            snprintf(op, opcap, "%s", ops[k]);
            return 1;
        }
    }
    return 0;
}

static ev_t eval_cmp(scanner_t *sc, json_t *params, int *ok)
{
    ev_t left = eval_primary(sc, params, ok);
    if (!*ok) return left;

    char op[8];
    if (!cmp_op(sc, op, sizeof(op))) return left;

    ev_t right = eval_primary(sc, params, ok);
    if (!*ok) {
        ev_release(&left);
        return right;
    }

    int res = 0;
    double dl = 0, dr = 0;

    if (strcmp(op, "==") == 0 || strcmp(op, "!=") == 0) {
        if (left.is_null || right.is_null) {
            res = (left.is_null && right.is_null) ? 1 : 0;
        } else if (left.is_num && right.is_num) {
            res = (left.num == right.num) ? 1 : 0;
        } else if (!left.is_num && !right.is_num) {
            res = (strcmp(to_str(&left), to_str(&right)) == 0) ? 1 : 0;
        } else if (to_num(&left, &dl) && to_num(&right, &dr)) {
            res = (dl == dr) ? 1 : 0;
        } else {
            res = 0;
        }
        if (strcmp(op, "!=") == 0) res = !res;
    } else {
        if (!to_num(&left, &dl)) dl = 0;
        if (!to_num(&right, &dr)) dr = 0;
        if (strcmp(op, ">") == 0)       res = (dl > dr);
        else if (strcmp(op, ">=") == 0) res = (dl >= dr);
        else if (strcmp(op, "<") == 0)  res = (dl < dr);
        else if (strcmp(op, "<=") == 0) res = (dl <= dr);
    }

    ev_release(&left);
    ev_release(&right);

    ev_t v;
    memset(&v, 0, sizeof(v));
    v.is_num = 1;
    v.num = res ? 1 : 0;
    return v;
}

static ev_t eval_and(scanner_t *sc, json_t *params, int *ok)
{
    ev_t v = eval_cmp(sc, params, ok);
    while (*ok && match_kw(sc, "and")) {
        ev_t r = eval_cmp(sc, params, ok);
        int lt = !v.is_null && (v.is_num ? v.num != 0 : (v.str && v.str[0] != '\0'));
        int rt = !r.is_null && (r.is_num ? r.num != 0 : (r.str && r.str[0] != '\0'));
        ev_release(&v);
        ev_release(&r);
        memset(&v, 0, sizeof(v));
        v.is_num = 1;
        v.num = (lt && rt) ? 1 : 0;
    }
    return v;
}

static ev_t eval_or(scanner_t *sc, json_t *params, int *ok)
{
    ev_t v = eval_and(sc, params, ok);
    while (*ok && match_kw(sc, "or")) {
        ev_t r = eval_and(sc, params, ok);
        int lt = !v.is_null && (v.is_num ? v.num != 0 : (v.str && v.str[0] != '\0'));
        int rt = !r.is_null && (r.is_num ? r.num != 0 : (r.str && r.str[0] != '\0'));
        ev_release(&v);
        ev_release(&r);
        memset(&v, 0, sizeof(v));
        v.is_num = 1;
        v.num = (lt || rt) ? 1 : 0;
    }
    return v;
}

int q_expr_eval(const char *expr, json_t *params, int *ok)
{
    scanner_t sc;
    ev_t      v;

    if (ok != NULL) *ok = 1;
    if (expr == NULL || *expr == '\0') return 0;

    sc.s = expr;
    sc.i = 0;
    sc.n = strlen(expr);

    v = eval_or(&sc, params, ok);
    if (ok != NULL && !*ok) {
        ev_release(&v);
        return 0;
    }

    int res;
    if (v.is_null)      res = 0;
    else if (v.is_num)  res = (v.num != 0);
    else                res = (v.str != NULL && v.str[0] != '\0');
    ev_release(&v);
    return res;
}
