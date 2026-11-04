/* 时间工具：调度一律用单调时钟（不受系统改时影响），上报数据用墙钟时间戳。 */
#ifndef EMS_CLOCK_H
#define EMS_CLOCK_H

#include <stdint.h>
#include <time.h>

static inline int64_t mono_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static inline int64_t wall_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* 给 pthread_cond_timedwait 用的绝对时间（条件变量需设置为 CLOCK_MONOTONIC） */
static inline struct timespec mono_deadline(int64_t ms_from_now)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    ts.tv_sec += ms_from_now / 1000;
    ts.tv_nsec += (ms_from_now % 1000) * 1000000;
    if (ts.tv_nsec >= 1000000000) {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000;
    }
    return ts;
}

#endif
