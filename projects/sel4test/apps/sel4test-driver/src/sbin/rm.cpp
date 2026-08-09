#include "h/sys_client.h"

// Фаза A (см. ROADMAP.md): вынесено из shell.cpp — SYS_DELETE=120 в blk_driver.cpp.
int main(int argc, char *argv[]) {
    SysClientEnv env;
    sys_client_init(env);

    char* p = env.arg;
    if (!p || *p == '\0') {
        sys_puts(0, "rm: missing operand\n");
        sys_exit(env.root_ep);
        return 1;
    }

    while (*p != '\0') {
        while (*p == ' ') p++;
        if (*p == '\0') break;

        char* start_of_arg = p;
        while (*p != ' ' && *p != '\0') p++;

        char temp_char = *p;
        *p = '\0';

        char *shm = env.shm;
        build_absolute_path(shm, start_of_arg, SHM_TOTAL_SIZE);
        seL4_CPtr target_ep = route_vfs_path(shm, env.blk_ep, env.usb_storage_ep); // Milestone 9
        if (vfs_syscall(120, target_ep) != 0) {
            sys_puts(0, "rm: cannot remove '");
            sys_puts(0, start_of_arg);
            sys_puts(0, "': No such file or directory\n");
        }
        *p = temp_char;
    }
    sys_exit(env.root_ep);
    return 0;
}
