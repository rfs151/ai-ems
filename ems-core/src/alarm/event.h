/* 异步事件队列：产生方（采集、控制、规则线程）只做入队，不碰数据库；
 * 存储线程统一出队写库并放入上云发件箱。队列满时丢弃最旧的事件并计数。 */
#ifndef EMS_EVENT_H
#define EMS_EVENT_H

#include <stdint.h>

enum { EV_INFO = 0, EV_WARN = 1, EV_ALARM = 2 };

typedef struct {
    int64_t ts;          /* 墙钟毫秒 */
    int level;
    char source[24];     /* 设备 id 或模块名 */
    char code[32];       /* 机器可读代码，如 comm_lost */
    char text[128];      /* 中文描述 */
} event_t;

void event_push(int level, const char *source, const char *code, const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));
/* 取出最多 max 条，返回条数 */
int event_pop(event_t *out, int max);
uint64_t event_dropped(void);

#endif
