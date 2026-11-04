#include "cmd.h"

#include <libgen.h>
#include <stdio.h>
#include <stdlib.h>

void cmd_socket_path(const config_t *cfg, char *out, int size)
{
    const char *env = getenv("EMS_CMD_SOCK");
    if (env) {
        snprintf(out, (size_t)size, "%s", env);
        return;
    }
    char buf[256];
    snprintf(buf, sizeof buf, "%s", cfg->db_path);
    snprintf(out, (size_t)size, "%s/ems-core.sock", dirname(buf));
}
