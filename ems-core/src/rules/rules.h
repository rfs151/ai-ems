/* 规则引擎：TinyExpr 条件表达式 + 点位变化触发 + 告警抑制 + 联动动作
 *
 * 规则存放在 SQLite（rules 表），可由云端在线增删改。每 200ms 取一次实时库快照：
 *   1. 逐点做变化检测（首次采到的值只记录，不算变化）
 *   2. 变化的点在"按触发点位排序的索引"里二分查找命中的规则，只对这些规则求值
 *   3. 条件由假变真 → 告警（受 1 分钟 / 24 小时次数上限约束）并执行动作；由真变假 → 恢复并撤销动作
 *   4. 条件为空的规则是"变位规则"：触发点位每变化一次记一条事件
 * 动作：alarm（只告警）、do:<DO 控制点>=<0|1>（如 do:fan_cmd=1，恢复时写回）、stop（停止功率输出，恢复时解除）。
 */
#ifndef EMS_RULES_H
#define EMS_RULES_H

#include "../config/config.h"

struct cJSON;

int rules_start(const config_t *cfg);
void rules_stop(void);

/* 往遥测报文追加规则表与统计 */
void rules_status(struct cJSON *obj);

/* rule_set / rule_del / rule_reset；成功返回 0 */
int rules_command(const char *type, const struct cJSON *cmd, char *msg, int msg_size);

#endif
