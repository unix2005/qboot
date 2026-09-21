#include <q/core/time.h>

#include <stdio.h>
#include <time.h>

#ifdef __APPLE__
#include <mach/mach_time.h>
#endif

int64_t q_time_now_ms(void)
{
    struct timespec ts;
#if defined(CLOCK_REALTIME)
    clock_gettime(CLOCK_REALTIME, &ts);
#else
    /* 兜底：老平台 */
    struct timeval tv;
    gettimeofday(&tv, NULL);
    ts.tv_sec = tv.tv_sec;
    ts.tv_nsec = tv.tv_usec * 1000;
#endif
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int64_t q_time_now_us(void)
{
    struct timespec ts;
#if defined(CLOCK_REALTIME)
    clock_gettime(CLOCK_REALTIME, &ts);
#else
    struct timeval tv;
    gettimeofday(&tv, NULL);
    ts.tv_sec = tv.tv_sec;
    ts.tv_nsec = tv.tv_usec * 1000;
#endif
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

void q_time_sleep_ms(int ms)
{
    if (ms <= 0) return;
    struct timespec ts;
    ts.tv_sec  = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

size_t q_time_format(int64_t ms, const char *fmt, char *buf, size_t cap)
{
    if (buf == NULL || cap == 0) return 0;

    time_t sec = (time_t)(ms / 1000);
    struct tm tm;
#if defined(_WIN32)
    localtime_s(&tm, &sec);
#else
    localtime_r(&sec, &tm);
#endif
    size_t n = strftime(buf, cap, fmt ? fmt : "%Y-%m-%d %H:%M:%S", &tm);
    return n;
}

size_t q_time_format_ms(int64_t ms, char *buf, size_t cap)
{
    if (buf == NULL || cap == 0) return 0;

    size_t n = q_time_format(ms, "%Y-%m-%d %H:%M:%S", buf, cap);
    if (n == 0 || n + 5 > cap) return n;

    int written = snprintf(buf + n, cap - n, ".%03d", (int)(ms % 1000));
    if (written < 0) return n;
    return n + (size_t)written;
}
