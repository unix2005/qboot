#ifndef Q_CORE_STR_H
#define Q_CORE_STR_H

#include <stdarg.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 可增长字节缓冲，始终以 '\0' 结尾，方便直接当 C 字符串用 */
typedef struct {
    char  *data;
    size_t len;
    size_t cap;
} q_str_t;

int  q_str_init(q_str_t *s, size_t cap);
void q_str_free(q_str_t *s);
void q_str_clear(q_str_t *s);

int  q_str_append(q_str_t *s, const char *p, size_t n);
int  q_str_append_cstr(q_str_t *s, const char *p);
int  q_str_appendf(q_str_t *s, const char *fmt, ...);
int  q_str_vappendf(q_str_t *s, const char *fmt, va_list ap);

/* 原地去除首尾空白 */
char *q_str_trim(char *s);
int   q_str_starts_with(const char *s, const char *prefix);
int   q_str_ends_with(const char *s, const char *suffix);
int   q_str_ieq(const char *a, const char *b);

#ifdef __cplusplus
}
#endif

#endif /* Q_CORE_STR_H */
