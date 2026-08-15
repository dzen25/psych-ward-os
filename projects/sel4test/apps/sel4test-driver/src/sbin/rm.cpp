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

    // issuse.txt №50: next_token() (h/sys_client.h) вместо ручного разбора
    // по пробелам — понимает "имя в кавычках" как один аргумент.
    char* start_of_arg;
    while ((start_of_arg = next_token(&p)) != nullptr) {
        char *shm = env.shm;
        build_absolute_path(shm, start_of_arg, SHM_TOTAL_SIZE);
        seL4_CPtr target_ep = route_vfs_path(shm, env.blk_ep, env.usb_storage_ep); // Milestone 9
        if (vfs_syscall(120, target_ep) != 0) {
            sys_puts(0, "rm: cannot remove '");
            sys_puts(0, start_of_arg);
            sys_puts(0, "': No such file or directory\n");
        }
    }
    sys_exit(env.root_ep);
    return 0;
}
