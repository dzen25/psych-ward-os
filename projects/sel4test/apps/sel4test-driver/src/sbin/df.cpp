#include "h/sys_client.h"

// Фаза 8 (мониторинг ресурсов, см. ROADMAP.md) — вынесено в отдельный
// sbin по образцу top.cpp, см. SYS_DF_STATS. `-h` — тот же приём, что
// `top -l` (флаг передаётся серверу через MR1).
int main(int argc, char *argv[]) {
    SysClientEnv env;
    sys_client_init(env);

    bool humanReadable = env.arg && my_strcmp(env.arg, "-h") == 0;
    vfs_lock();
    seL4_SetMR(0, SYS_DF_STATS);
    seL4_SetMR(1, humanReadable ? 1 : 0);
    seL4_Call(env.root_ep, seL4_MessageInfo_new(0, 0, 0, 2));
    sys_puts(0, env.shm);
    vfs_unlock();
    sys_exit(env.root_ep);
    return 0;
}
