/* 本地指令通道：ems-sync 把云端下行指令通过 Unix 数据报套接字转给 ems-core，
 * ems-core 处理后原路回复结果。不走数据库，往返在毫秒级。
 * 套接字路径：环境变量 EMS_CMD_SOCK，默认与数据库同目录的 ems-core.sock。 */
#ifndef EMS_CMD_H
#define EMS_CMD_H

#include "../config/config.h"

/* 计算套接字路径（ems-core 与 ems-sync 共用） */
void cmd_socket_path(const config_t *cfg, char *out, int size);

int cmd_start(const config_t *cfg);
void cmd_stop(void);

#endif
