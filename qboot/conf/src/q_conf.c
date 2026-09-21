#include <q/conf.h>

#include <q/core/ds.h>
#include <q/core/str.h>

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* key 形式："sec.key"，section 为空时只存 "key" */
struct q_conf {
    q_hash_t *h;
};

static void make_key(char *out, size_t cap, const char *sec, const char *key)
{
    if (sec != NULL && *sec != '\0') {
        snprintf(out, cap, "%s.%s", sec, key);
    } else {
        snprintf(out, cap, "%s", key);
    }
}

/* Q_<SEC>_<KEY>，非法字符统一换成 '_'，全大写 */
static void make_env(char *out, size_t cap, const char *sec, const char *key)
{
    size_t n = 0;
    out[n++] = 'Q';
    out[n++] = '_';

    const char *src[2] = { sec, key };
    for (int i = 0; i < 2; i++) {
        if (src[i] == NULL) continue;
        if (i == 1 && n < cap) out[n++] = '_';
        for (const char *p = src[i]; *p != '\0' && n + 1 < cap; p++) {
            out[n++] = (char)toupper((unsigned char)(*p == '.' || *p == '-' ? '_' : *p));
        }
    }
    out[n] = '\0';
}

q_conf_t *q_conf_new(void)
{
    q_conf_t *c = malloc(sizeof(q_conf_t));
    if (c == NULL) return NULL;
    c->h = q_hash_new(16);
    if (c->h == NULL) {
        free(c);
        return NULL;
    }
    return c;
}

int q_conf_set(q_conf_t *c, const char *sec, const char *key, const char *val)
{
    if (c == NULL || key == NULL || val == NULL) return -1;

    char k[256];
    make_key(k, sizeof(k), sec, key);
    return q_hash_set(c->h, k, strdup(val));
}

static void free_val(const char *key, void *val, void *ud)
{
    (void)key;
    (void)ud;
    free(val);
}

void q_conf_free(q_conf_t *c)
{
    if (c == NULL) return;
    q_hash_foreach(c->h, free_val, NULL);
    q_hash_free(c->h);
    free(c);
}

q_conf_t *q_conf_load(const char *path)
{
    if (path == NULL) return NULL;

    FILE *fp = fopen(path, "r");
    if (fp == NULL) return NULL;

    q_conf_t *c = q_conf_new();
    if (c == NULL) {
        fclose(fp);
        return NULL;
    }

    char line[1024];
    char sec[128] = "";

    while (fgets(line, sizeof(line), fp) != NULL) {
        char *s = q_str_trim(line);
        if (*s == '\0' || *s == ';' || *s == '#') continue;

        if (*s == '[') {
            char *end = strchr(s, ']');
            if (end != NULL) {
                *end = '\0';
                snprintf(sec, sizeof(sec), "%s", q_str_trim(s + 1));
            }
            continue;
        }

        char *eq = strchr(s, '=');
        if (eq == NULL) continue;

        *eq = '\0';
        char *key = q_str_trim(s);
        char *val = q_str_trim(eq + 1);

        /* 去掉成对引号 */
        size_t vl = strlen(val);
        if (vl >= 2 && ((val[0] == '"' && val[vl - 1] == '"') ||
                        (val[0] == '\'' && val[vl - 1] == '\''))) {
            val[vl - 1] = '\0';
            val++;
        }
        q_conf_set(c, sec, key, val);
    }

    fclose(fp);
    return c;
}

const char *q_conf_get(const q_conf_t *c, const char *sec, const char *key, const char *def)
{
    if (key == NULL) return def;

    char env[192];
    make_env(env, sizeof(env), sec, key);
    const char *e = getenv(env);
    if (e != NULL && *e != '\0') return e;

    if (c != NULL) {
        char k[256];
        make_key(k, sizeof(k), sec, key);
        const char *v = q_hash_get(c->h, k);
        if (v != NULL) return v;
    }
    return def;
}

int q_conf_has(const q_conf_t *c, const char *sec, const char *key)
{
    return q_conf_get(c, sec, key, NULL) != NULL;
}

int q_conf_get_int(const q_conf_t *c, const char *sec, const char *key, int def)
{
    const char *v = q_conf_get(c, sec, key, NULL);
    return v ? atoi(v) : def;
}

long long q_conf_get_ll(const q_conf_t *c, const char *sec, const char *key, long long def)
{
    const char *v = q_conf_get(c, sec, key, NULL);
    return v ? atoll(v) : def;
}

int q_conf_get_bool(const q_conf_t *c, const char *sec, const char *key, int def)
{
    const char *v = q_conf_get(c, sec, key, NULL);
    if (v == NULL) return def;
    if (strcasecmp(v, "1") == 0 || strcasecmp(v, "true") == 0 ||
        strcasecmp(v, "yes") == 0 || strcasecmp(v, "on") == 0) return 1;
    if (strcasecmp(v, "0") == 0 || strcasecmp(v, "false") == 0 ||
        strcasecmp(v, "no") == 0 || strcasecmp(v, "off") == 0) return 0;
    return def;
}

double q_conf_get_double(const q_conf_t *c, const char *sec, const char *key, double def)
{
    const char *v = q_conf_get(c, sec, key, NULL);
    return v ? atof(v) : def;
}
