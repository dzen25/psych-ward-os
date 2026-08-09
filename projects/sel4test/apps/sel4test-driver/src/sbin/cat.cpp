#include "h/sys_client.h"

// Фаза A (см. ROADMAP.md): вынесено из shell.cpp — SYS_READ_TEXT_FILE=114 в blk_driver.cpp.
int main(int argc, char *argv[]) {
    SysClientEnv env;
    sys_client_init(env);

    if (!env.arg) { sys_puts(0, "Usage: cat <file>\n"); sys_exit(env.root_ep); return 1; }

    char *shm = env.shm;
    build_absolute_path(shm, env.arg, SHM_TOTAL_SIZE);
    seL4_CPtr target_ep = route_vfs_path(shm, env.blk_ep, env.usb_storage_ep); // Milestone 9

    if (vfs_syscall(114, target_ep) == 0) { // Файл прочитан в shm
        sys_puts(0, shm);
        sys_puts(0, "\n");
    } else {
        sys_puts(0, "File not found or is a directory.\n");
    }
    sys_exit(env.root_ep);
    return 0;
}
