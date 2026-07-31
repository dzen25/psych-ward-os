#include "h/sys_client.h"

// Фаза A (см. ROADMAP.md): вынесено из shell.cpp — SYS_LS=110 в blk_driver.cpp.
int main(int argc, char *argv[]) {
    SysClientEnv env;
    sys_client_init(env);

    char *shm = env.shm;
    if (env.arg) build_absolute_path(shm, env.arg, SHM_TOTAL_SIZE);
    else build_absolute_path(shm, "", SHM_TOTAL_SIZE);

    vfs_syscall(110, env.blk_ep); // SYS_LS — пишет листинг прямо в shm
    sys_puts(0, shm);
    sys_exit(env.root_ep);
    return 0;
}
