/* 实时库：所有设备的采集点放在一个数组里，读写锁保护。
 *
 * 采集线程按点写入；控制、存储、上云按需读单点或一次性拷贝快照，
 * 快照保证同一时刻的数据一致（控制计算不会读到"一半新一半旧"的数据）。
 */
#ifndef EMS_RTDB_H
#define EMS_RTDB_H

#include <stdint.h>

#include "../config/config.h"

/* 数据质量 */
enum { Q_INVALID = 0, Q_GOOD = 1, Q_OFFLINE = 2 };

typedef struct {
    char id[48];        /* 设备 id + 模型键，如 pcs1.p */
    const point_def_t *def;
    int dev;            /* 设备下标 */
    double value;
    uint8_t quality;
    int64_t ts;         /* 墙钟毫秒 */
} rt_point_t;

typedef struct {
    int known;          /* 是否已判定过在线状态（启动后从未连上也要报通信中断） */
    int online;
    int failures;       /* 连续失败次数 */
    int64_t last_ok;    /* 墙钟毫秒 */
    uint64_t reads;
    uint64_t errors;
    uint64_t writes;
    double poll_ms;     /* 最近一轮快组采集耗时 */
    int bad_blocks;     /* 读取异常的寄存器块数（部分失败，设备仍在线） */
} rt_device_t;

int rtdb_init(const config_t *cfg);
void rtdb_free(void);

int rtdb_count(void);
int rtdb_find(const char *id);                 /* 找不到返回 -1 */
const rt_point_t *rtdb_meta(int index);         /* 只读元数据（id、def、dev），不加锁 */

void rtdb_set(int index, double value, uint8_t quality, int64_t ts);
void rtdb_set_device_quality(int dev, uint8_t quality);  /* 设备离线时整体置质量 */
int rtdb_get(int index, double *value, uint8_t *quality);
double rtdb_value(const char *id, double def);  /* 质量不好时返回 def */

/* 拷贝全部点到 out（长度 rtdb_count()），返回点数 */
int rtdb_snapshot(rt_point_t *out);

void rtdb_device_update(int dev, int ok, double poll_ms, int offline_after, int *changed);
void rtdb_device_count_write(int dev);
void rtdb_device_set_bad_blocks(int dev, int n);
void rtdb_device_get(int dev, rt_device_t *out);

#endif
