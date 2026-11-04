/* 简单日志：带时间戳和级别，线程安全（单次 fprintf）。现场版本可替换为 zlog。 */
#ifndef EMS_LOG_H
#define EMS_LOG_H

enum { LOG_DEBUG, LOG_INFO, LOG_WARN, LOG_ERROR };

void log_set_level(int level);
void log_write(int level, const char *mod, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

#define LOGD(mod, ...) log_write(LOG_DEBUG, mod, __VA_ARGS__)
#define LOGI(mod, ...) log_write(LOG_INFO, mod, __VA_ARGS__)
#define LOGW(mod, ...) log_write(LOG_WARN, mod, __VA_ARGS__)
#define LOGE(mod, ...) log_write(LOG_ERROR, mod, __VA_ARGS__)

#endif
