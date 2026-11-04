/* ems-core：储能 EMS 边缘主进程
 *
 *   ems-core [-c 配置目录] [-v]
 *
 * 启动顺序：配置 → 实时库 → 存储 → 采集 → 控制 → 规则 → 指令通道；退出时反向停止，
 * 并先把 PCS 设定值清零再断开设备连接。
 */
#include <errno.h>
#include <libgen.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "acq/acq.h"
#include "cmd/cmd.h"
#include "config/config.h"
#include "control/control.h"
#include "rtdb/rtdb.h"
#include "rules/rules.h"
#include "storage/storage.h"
#include "util/log.h"

static volatile sig_atomic_t g_stop;

static void on_signal(int sig)
{
    (void)sig;
    g_stop = 1;
}

/* 每秒遥测报文里追加各模块状态 */
static void status_all(struct cJSON *obj)
{
    control_status(obj);
    rules_status(obj);
}

static void ensure_dir_of(const char *path)
{
    char buf[256];
    snprintf(buf, sizeof buf, "%s", path);
    char *dir = dirname(buf);
    if (mkdir(dir, 0755) && errno != EEXIST)
        LOGW("main", "创建目录 %s 失败：%s", dir, strerror(errno));
}

int main(int argc, char **argv)
{
    const char *config_dir = "config";
    int opt;
    while ((opt = getopt(argc, argv, "c:vh")) != -1) {
        switch (opt) {
        case 'c': config_dir = optarg; break;
        case 'v': log_set_level(LOG_DEBUG); break;
        default:
            fprintf(stderr, "用法：%s [-c 配置目录] [-v]\n", argv[0]);
            return opt == 'h' ? 0 : 2;
        }
    }

    struct sigaction sa = {.sa_handler = on_signal};
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    config_t cfg;
    if (config_load(&cfg, config_dir))
        return 1;
    LOGI("main", "站点 %s（%s），%d 台设备", cfg.site_id, cfg.site_name, cfg.ndevices);

    ensure_dir_of(cfg.db_path);
    /* 存储线程最先启动：之后各模块的事件都有人落库（事件队列先于产生方就绪） */
    if (rtdb_init(&cfg) || storage_start(&cfg, status_all) || acq_start(&cfg) || control_start(&cfg) ||
        rules_start(&cfg) || cmd_start(&cfg)) {
        LOGE("main", "启动失败");
        return 1;
    }
    LOGI("main", "运行中，Ctrl+C 退出");

    while (!g_stop)
        pause();

    LOGI("main", "正在退出");
    cmd_stop();
    rules_stop();
    control_stop();
    usleep(300 * 1000);      /* 给采集线程时间下发清零设定值 */
    acq_stop();
    storage_stop();
    rtdb_free();
    config_free(&cfg);
    LOGI("main", "已退出");
    return 0;
}
