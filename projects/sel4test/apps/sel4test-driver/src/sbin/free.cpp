#include "h/sys_client.h"

// Фаза 8 (мониторинг ресурсов, см. ROADMAP.md) — вынесено в отдельный
// sbin по образцу top.cpp, см. SYS_FREE_STATS.
int main(int argc, char *argv[]) {
    SysClientEnv env;
    sys_client_init(env);

    vfs_lock();
    seL4_SetMR(0, SYS_FREE_STATS);
    seL4_Call(env.root_ep, seL4_MessageInfo_new(0, 0, 0, 1));
    sys_puts(0, env.shm);
    vfs_unlock();
    sys_exit(env.root_ep);
    return 0;
}
