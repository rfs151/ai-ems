/* 本地存储（SQLite，WAL 模式）
 *
 * 存储线程每秒执行一次，全部写入放在一个事务里：
 *   - telemetry：快组点每秒、慢组点每 10 秒入库，(point, ts) 复合索引
 *   - events：事件队列出队入库
 *   - outbox：上云发件箱，ems-sync 进程读取发送后删除；断网时数据留在这里
 * WAL + busy_timeout 让 ems-core 写、ems-sync 读删可以并发，不会互相报 SQLITE_BUSY。
 */
#ifndef EMS_STORAGE_H
#define EMS_STORAGE_H

#include "../config/config.h"

struct cJSON;
/* 其他模块往每秒的遥测报文里追加状态字段（如控制状态） */
typedef void (*storage_status_fn)(struct cJSON *obj);

int storage_start(const config_t *cfg, storage_status_fn status_fn);
void storage_stop(void);

/* 公共：打开数据库并设置 WAL、busy_timeout，ems-sync 也用 */
struct sqlite3;
struct sqlite3 *storage_open(const char *path);

/* 表结构升级：列不存在时才 ALTER TABLE ADD COLUMN（老版本数据库原地升级，重复执行不报错）。
 * 返回 1 = 新增了列，0 = 已存在，-1 = 失败 */
int storage_ensure_column(struct sqlite3 *db, const char *table, const char *column, const char *decl);

#endif
