#ifndef Q_CORE_TIME_H
#define Q_CORE_TIME_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int64_t q_time_now_ms(void);
int64_t q_time_now_us(void);
void    q_time_sleep_ms(int ms);

/* 格式化为 "2026-09-21 22:35:10.238"，返回写入长度（不含 '\0'） */
size_t  q_time_format_ms(int64_t ms, char *buf, size_t cap);

/* 按 strftime 格式串输出本地时间（不含毫秒），返回写入长度 */
size_t  q_time_format(int64_t ms, const char *fmt, char *buf, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* Q_CORE_TIME_H */
