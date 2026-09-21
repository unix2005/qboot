/*
 * XML -> AST。启动期一次性解析，运行期不再碰 XML。
 * 支持的标签：select/insert/update/delete、if、choose/when/otherwise、
 *             where、set、foreach、sql/include。
 */

#include "q_mapper_int.h"

#include <libxml/parser.h>
#include <libxml/tree.h>

#include <q/core/types.h>
#include <q/log.h>

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------- 节点生命周期 ---------------- */

q_node_t *q_node_new(q_node_kind_t kind)
{
    q_node_t *n = calloc(1, sizeof(q_node_t));
    if (n != NULL) n->kind = kind;
    return n;
}

static char *dup_cstr(const char *s)
{
    return (s == NULL) ? NULL : strdup(s);
}

q_node_t *q_node_clone(const q_node_t *n)
{
    q_node_t *c;
    if (n == NULL) return NULL;

    c = calloc(1, sizeof(q_node_t));
    if (c == NULL) return NULL;

    c->kind       = n->kind;
    c->text       = dup_cstr(n->text);
    c->test       = dup_cstr(n->test);
    c->collection = dup_cstr(n->collection);
    c->item       = dup_cstr(n->item);
    c->open       = dup_cstr(n->open);
    c->close      = dup_cstr(n->close);
    c->separator  = dup_cstr(n->separator);
    c->refid      = dup_cstr(n->refid);

    if (n->nkids > 0) {
        c->kids = calloc((size_t)n->nkids, sizeof(q_node_t *));
        if (c->kids == NULL) {
            q_node_free(c);
            return NULL;
        }
        for (int i = 0; i < n->nkids; i++) {
            c->kids[i] = q_node_clone(n->kids[i]);
        }
        c->nkids = n->nkids;
    }
    return c;
}

void q_node_free(q_node_t *n)
{
    if (n == NULL) return;

    free(n->text);
    free(n->test);
    free(n->collection);
    free(n->item);
    free(n->open);
    free(n->close);
    free(n->separator);
    free(n->refid);

    for (int i = 0; i < n->nkids; i++) q_node_free(n->kids[i]);
    free(n->kids);
    free(n);
}

/* ---------------- XML 辅助 ---------------- */

static char *attr(xmlNodePtr n, const char *name)
{
    xmlChar *v = xmlGetProp(n, (const xmlChar *)name);
    if (v == NULL) return NULL;
    char *s = strdup((const char *)v);
    xmlFree(v);
    return s;
}

/*
 * 换行/制表/连续空白 -> 单个空格。
 * 注意：两端各保留一个空格（MyBatis 也是这么干的）。
 * XML 里 <include> 前后的缩进正是靠它变成 SQL 的分隔空格，
 * 如果在这里 trim 掉，会拼出 "SELECTid, name FROM" 这种连体 SQL。
 * 最终的多余空白由 mapper.c 的 sql_tidy() 统一收尾。
 */
static char *norm_text(const char *s)
{
    size_t n   = strlen(s);
    char  *out = malloc(n + 3);
    if (out == NULL) return NULL;

    size_t j = 0;
    int    pending = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (isspace(c)) {
            pending = 1;
            continue;
        }
        if (pending) {
            out[j++] = ' ';
            pending = 0;
        }
        out[j++] = (char)c;
    }
    if (pending) out[j++] = ' ';
    out[j] = '\0';
    return out;
}

static void add_kid(q_node_t *parent, q_node_t *kid)
{
    if (parent == NULL || kid == NULL) return;

    q_node_t **kids = realloc(parent->kids,
                              (size_t)(parent->nkids + 1) * sizeof(q_node_t *));
    if (kids == NULL) {
        q_node_free(kid);
        return;
    }
    parent->kids = kids;
    parent->kids[parent->nkids++] = kid;
}

static void parse_children(xmlNodePtr first, q_node_t *parent, q_hash_t *frags)
{
    for (xmlNodePtr n = first; n != NULL; n = n->next) {
        if (n->type == XML_TEXT_NODE) {
            const char *txt = (const char *)n->content;
            if (txt == NULL) continue;

            char *s = norm_text(txt);
            if (s == NULL) continue;
            if (*s == '\0') {
                free(s);
                continue;
            }
            q_node_t *t = q_node_new(QN_TEXT);
            t->text = s;
            add_kid(parent, t);
            continue;
        }

        if (n->type != XML_ELEMENT_NODE) continue;

        const char *name = (const char *)n->name;

        if (strcmp(name, "if") == 0) {
            q_node_t *k = q_node_new(QN_IF);
            k->test = attr(n, "test");
            parse_children(n->children, k, frags);
            add_kid(parent, k);

        } else if (strcmp(name, "where") == 0) {
            q_node_t *k = q_node_new(QN_WHERE);
            parse_children(n->children, k, frags);
            add_kid(parent, k);

        } else if (strcmp(name, "set") == 0) {
            q_node_t *k = q_node_new(QN_SET);
            parse_children(n->children, k, frags);
            add_kid(parent, k);

        } else if (strcmp(name, "choose") == 0) {
            q_node_t *k = q_node_new(QN_CHOOSE);
            parse_children(n->children, k, frags);
            add_kid(parent, k);

        } else if (strcmp(name, "when") == 0) {
            q_node_t *k = q_node_new(QN_WHEN);
            k->test = attr(n, "test");
            parse_children(n->children, k, frags);
            add_kid(parent, k);

        } else if (strcmp(name, "otherwise") == 0) {
            q_node_t *k = q_node_new(QN_OTHERWISE);
            parse_children(n->children, k, frags);
            add_kid(parent, k);

        } else if (strcmp(name, "foreach") == 0) {
            q_node_t *k = q_node_new(QN_FOREACH);
            k->collection = attr(n, "collection");
            k->item       = attr(n, "item");
            k->open       = attr(n, "open");
            k->close      = attr(n, "close");
            k->separator  = attr(n, "separator");
            parse_children(n->children, k, frags);
            add_kid(parent, k);

        } else if (strcmp(name, "include") == 0) {
            char *ref = attr(n, "refid");
            if (ref != NULL && frags != NULL) {
                q_node_t *frag = q_hash_get(frags, ref);
                if (frag != NULL) {
                    add_kid(parent, q_node_clone(frag));
                } else {
                    q_warn("mapper: include refid '%s' not found", ref);
                }
            }
            free(ref);

        } else {
            /* 未知标签：不报错，把里面的内容接上来，避免整个 mapper 加载失败 */
            parse_children(n->children, parent, frags);
        }
    }
}

static void free_frag(const char *key, void *val, void *ud)
{
    (void)key;
    (void)ud;
    q_node_free((q_node_t *)val);
}

/* ---------------- 文件解析 ---------------- */

int q_mapper_parse_file(const char *path, q_array_t *out, char *err, size_t errlen)
{
    xmlDocPtr  doc;
    xmlNodePtr root;
    char      *ns = NULL;
    char      *id;
    q_hash_t  *frags;

    /*
     * 不能用 XML_PARSE_NOBLANKS：那会把纯空白文本节点整个删掉，
     * 导致 <include> 前后的缩进消失，拼出 "SELECTid, name FROM"。
     * 这里保留空白节点，交给 norm_text 折叠成单个空格。
     */
    doc = xmlReadFile(path, "UTF-8", XML_PARSE_NOERROR | XML_PARSE_NOWARNING);
    if (doc == NULL) {
        snprintf(err, errlen, "cannot parse xml: %s", path);
        return Q_ERR;
    }

    root = xmlDocGetRootElement(doc);
    if (root == NULL || strcmp((const char *)root->name, "mapper") != 0) {
        snprintf(err, errlen, "root element must be <mapper>: %s", path);
        xmlFreeDoc(doc);
        return Q_ERR;
    }

    ns    = attr(root, "namespace");
    frags = q_hash_new(8);
    if (frags == NULL) {
        snprintf(err, errlen, "out of memory");
        free(ns);
        xmlFreeDoc(doc);
        return Q_ERR_NOMEM;
    }

    /* 第一遍：收集 <sql id="..."> 片段 */
    for (xmlNodePtr n = root->children; n != NULL; n = n->next) {
        if (n->type != XML_ELEMENT_NODE) continue;
        if (strcmp((const char *)n->name, "sql") != 0) continue;

        id = attr(n, "id");
        if (id == NULL) continue;

        q_node_t *frag = q_node_new(QN_GROUP);
        parse_children(n->children, frag, frags);
        q_hash_set(frags, id, frag);
        free(id);
    }

    /* 第二遍：解析语句 */
    for (xmlNodePtr n = root->children; n != NULL; n = n->next) {
        if (n->type != XML_ELEMENT_NODE) continue;

        const char *tag = (const char *)n->name;
        int is_select   = (strcmp(tag, "select") == 0);
        int is_dml      = (strcmp(tag, "insert") == 0 ||
                           strcmp(tag, "update") == 0 ||
                           strcmp(tag, "delete") == 0);
        if (!is_select && !is_dml) continue;

        id = attr(n, "id");
        if (id == NULL) continue;

        q_stmt_def_t *sd = calloc(1, sizeof(q_stmt_def_t));
        if (sd == NULL) {
            free(id);
            continue;
        }
        if (ns != NULL && *ns != '\0') {
            snprintf(sd->id, sizeof(sd->id), "%s.%s", ns, id);
        } else {
            snprintf(sd->id, sizeof(sd->id), "%s", id);
        }
        sd->is_select = is_select;

        char *gk = attr(n, "useGeneratedKeys");
        if (gk != NULL) {
            sd->use_keys = (strcasecmp(gk, "true") == 0 || strcmp(gk, "1") == 0);
            free(gk);
        }
        char *kp = attr(n, "keyProperty");
        if (kp != NULL) {
            snprintf(sd->key_property, sizeof(sd->key_property), "%s", kp);
            free(kp);
        }

        sd->root = q_node_new(QN_GROUP);
        parse_children(n->children, sd->root, frags);

        q_array_push(out, sd);
        free(id);
    }

    q_hash_foreach(frags, free_frag, NULL);
    q_hash_free(frags);
    free(ns);
    xmlFreeDoc(doc);
    return Q_OK;
}
