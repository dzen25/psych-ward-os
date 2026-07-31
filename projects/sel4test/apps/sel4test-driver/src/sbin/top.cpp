#include "h/sys_client.h"

// Фаза A (см. ROADMAP.md): вынесено из shell.cpp — `top` (компактная
// таблица по ядрам) и `top -l` (подробная построчная), см. SYS_TOP_STATS.
int main(int argc, char *argv[]) {
    SysClientEnv env;
    sys_client_init(env);

    bool longFormat = env.arg && my_strcmp(env.arg, "-l") == 0;
    vfs_lock();
    seL4_SetMR(0, SYS_TOP_STATS);
    seL4_SetMR(1, longFormat ? 1 : 0);
    seL4_Call(env.root_ep, seL4_MessageInfo_new(0, 0, 0, 2));
    sys_puts(0, env.shm);
    vfs_unlock();
    sys_exit(env.root_ep);
    return 0;
}
