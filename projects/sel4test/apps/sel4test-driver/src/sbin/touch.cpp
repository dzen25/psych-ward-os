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

    while (*p != '\0') {
        while (*p == ' ') p++;
        if (*p == '\0') break;

        char* start_of_arg = p;
        while (*p != ' ' && *p != '\0') p++;

        char temp_char = *p;
        *p = '\0';

        char *shm = env.shm;
        build_absolute_path(shm, start_of_arg, SHM_TOTAL_SIZE);
        int status = vfs_syscall(112, env.blk_ep);
        if (status == 1) {
            sys_puts(0, "touch: '");
            sys_puts(0, start_of_arg);
            sys_puts(0, "' уже существует — ничего не делаю\n");
        } else if (status != 0) {
            sys_puts(0, "touch: failed to create '");
            sys_puts(0, start_of_arg);
            sys_puts(0, "'\n");
        }
        *p = temp_char;
    }
    sys_exit(env.root_ep);
    return 0;
}
