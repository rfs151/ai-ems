/* 采集调度：每台设备一个线程，独占一个 Modbus 连接。
 *
 * - 同一区、同一采集组里地址相近的点合并成一次批量读（减少报文数）
 * - 快组/慢组按各自周期调度；连续失败 N 次判离线，整台设备数据质量置为离线
 * - 写操作由其他线程入队，采集线程在两次读之间执行；同一控制点只保留最新值
 */
#ifndef EMS_ACQ_H
#define EMS_ACQ_H

#include "../config/config.h"

int acq_start(const config_t *cfg);
void acq_stop(void);

/* 异步写控制点（如 pcs2 的 p_set）；未知设备/控制点返回 -1 */
int acq_write(const char *dev_id, const char *control_key, double value);

#endif
