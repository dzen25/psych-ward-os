#include "h/sys_client.h"

// Фаза A (см. ROADMAP.md): вынесено из shell.cpp — SYS_TOUCH=112 в blk_driver.cpp.
int main(int argc, char *argv[]) {
    SysClientEnv env;

    sys_client_init(env);

    char* p = env.arg;
    if (!p || *p == '\0') {
        sys_puts(0, "touch: missing file operand\n");
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
        int status = vfs_syscall(112, target_ep);
        if (status == 1) {
            sys_puts(0, "touch: '");
            sys_puts(0, start_of_arg);
            sys_puts(0, "' уже существует — ничего не делаю\n");
        } else if (status != 0) {
            sys_puts(0, "touch: failed to create '");
            sys_puts(0, start_of_arg);
            sys_puts(0, "'\n");
        }
    }
    sys_exit(env.root_ep);
    return 0;
}
