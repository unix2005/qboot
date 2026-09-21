#include <q/core/str.h>

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int q_str_init(q_str_t *s, size_t cap)
{
    if (cap == 0) cap = 64;
    s->data = malloc(cap);
    if (s->data == NULL) {
        s->len = s->cap = 0;
        return -1;
    }
    s->data[0] = '\0';
    s->len = 0;
    s->cap = cap;
    return 0;
}

void q_str_free(q_str_t *s)
{
    if (s == NULL) return;
    free(s->data);
    s->data = NULL;
    s->len = s->cap = 0;
}

void q_str_clear(q_str_t *s)
{
    if (s == NULL || s->data == NULL) return;
    s->len = 0;
    s->data[0] = '\0';
}

static int str_reserve(q_str_t *s, size_t need)
{
    if (s->cap - s->len > need) return 0;         /* 留 1 字节给 '\0' */
    size_t cap = s->cap ? s->cap : 64;
    while (cap - s->len <= need) {
        if (cap > (size_t)-1 / 2) return -1;
        cap *= 2;
    }
    char *p = realloc(s->data, cap);
    if (p == NULL) return -1;
    s->data = p;
    s->cap  = cap;
    return 0;
}

int q_str_append(q_str_t *s, const char *p, size_t n)
{
    if (s == NULL || p == NULL) return -1;
    if (str_reserve(s, n) != 0) return -1;
    memcpy(s->data + s->len, p, n);
    s->len += n;
    s->data[s->len] = '\0';
    return 0;
}

int q_str_append_cstr(q_str_t *s, const char *p)
{
    return (p == NULL) ? 0 : q_str_append(s, p, strlen(p));
}

int q_str_vappendf(q_str_t *s, const char *fmt, va_list ap)
{
    if (s == NULL || fmt == NULL) return -1;
    if (s->data == NULL && q_str_init(s, 128) != 0) return -1;

    va_list copy;
    va_copy(copy, ap);
    size_t avail = (s->cap > s->len) ? (s->cap - s->len) : 0;
    int n = vsnprintf(s->data + s->len, avail, fmt, copy);
    va_end(copy);

    if (n < 0) return -1;
    if ((size_t)n >= avail) {
        if (str_reserve(s, (size_t)n) != 0) return -1;
        avail = s->cap - s->len;
        int m = vsnprintf(s->data + s->len, avail, fmt, ap);
        if (m < 0 || (size_t)m >= avail) return -1;
        n = m;
    }
    s->len += (size_t)n;
    return 0;
}

int q_str_appendf(q_str_t *s, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int rc = q_str_vappendf(s, fmt, ap);
    va_end(ap);
    return rc;
}

char *q_str_trim(char *s)
{
    if (s == NULL) return NULL;

    while (*s != '\0' && isspace((unsigned char)*s)) s++;
    if (*s == '\0') return s;

    char *end = s + strlen(s) - 1;
    while (end > s && isspace((unsigned char)*end)) end--;
    end[1] = '\0';
    return s;
}

int q_str_starts_with(const char *s, const char *prefix)
{
    if (s == NULL || prefix == NULL) return 0;
    return strncmp(s, prefix, strlen(prefix)) == 0;
}

int q_str_ends_with(const char *s, const char *suffix)
{
    if (s == NULL || suffix == NULL) return 0;
    size_t ls = strlen(s), lp = strlen(suffix);
    if (lp > ls) return 0;
    return memcmp(s + ls - lp, suffix, lp) == 0;
}

int q_str_ieq(const char *a, const char *b)
{
    if (a == NULL || b == NULL) return a == b;
    return strcasecmp(a, b) == 0;
}
