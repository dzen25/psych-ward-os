#include "h/sys_client.h"

// Фаза A (см. ROADMAP.md): вынесено из shell.cpp — SYS_RENAME=116 в blk_driver.cpp.
int main(int argc, char *argv[]) {
    SysClientEnv env;
    sys_client_init(env);

    if (!env.arg) {
        sys_puts(0, "mv: missing file operand\n");
        sys_exit(env.root_ep);
        return 1;
    }
    char* p = env.arg;

    char old_name[32];
    int i = 0;
    while (*p != ' ' && *p != '\0' && i < 31) {
        old_name[i++] = *p++;
    }
    old_name[i] = '\0';

    while (*p == ' ') p++;

    if (*p == '\0') {
        sys_puts(0, "mv: missing destination file operand after '");
        sys_puts(0, old_name);
        sys_puts(0, "'\n");
        sys_exit(env.root_ep);
        return 1;
    }

    char new_name[32];
    i = 0;
    while (*p != ' ' && *p != '\0' && i < 31) {
        new_name[i++] = *p++;
    }
    new_name[i] = '\0';

    char *shm = env.shm;
    build_absolute_path(shm, old_name, 128);
    build_absolute_path(shm + 128, new_name, SHM_TOTAL_SIZE - 128);

    vfs_lock();
    seL4_SetMR(0, 116); // SYS_RENAME
    seL4_Call(env.blk_ep, seL4_MessageInfo_new(0, 0, 0, 1));
    int ret_val = seL4_GetMR(0);
    vfs_unlock();

    if (ret_val != 0) {
        sys_puts(0, "mv: cannot stat '");
        sys_puts(0, old_name);
        sys_puts(0, "': No such file or directory\n");
    }
    sys_exit(env.root_ep);
    return 0;
}
