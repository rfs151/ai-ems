/* 功率控制
 *
 * 每 100ms 一个控制周期：
 *   P = P_base（计划曲线模式才有）+ P_mFRR + P_FCR + P_LFSM-O
 *   → 安全联锁（急停、消防、并网开关分闸时置 0）
 *   → SOC 与 BMS 允许功率限幅
 *   → 爬坡率限制
 *   → 按各 PCS 可用能力分配，变化超过死区或到刷新周期才下发
 * 算法本身在 algo.c（纯函数，有单元测试）。
 */
#ifndef EMS_CONTROL_H
#define EMS_CONTROL_H

#include "../config/config.h"

struct cJSON;

int control_start(const config_t *cfg);
void control_stop(void);

/* 往遥测报文追加控制状态（storage 每秒调用） */
void control_status(struct cJSON *obj);

/* 处理一条下行指令（cmd 模块调用，线程安全）。成功返回 0；msg 写入结果说明 */
int control_command(const char *type, const struct cJSON *cmd, char *msg, int msg_size);

#endif
