#include "h/sys_client.h"

// Фаза A (см. ROADMAP.md): вынесено из shell.cpp — SYS_MKDIR=117 в
// blk_driver.cpp. В отличие от ls/cat/touch/rm/mv здесь НЕТ
// build_absolute_path — оригинальный shell.cpp тоже передавал сюда сырой
// аргумент как есть, полагаясь на собственное серверное разрешение
// относительного пути внутри blk_driver.cpp (синхронизируется командой `cd`
// через SYS_CD=118) — сохраняем это поведение 1:1, не унифицируем.
int main(int argc, char *argv[]) {
    SysClientEnv env;
    sys_client_init(env);

    char* p = env.arg;
    if (!p) {
        sys_puts(0, "mkdir: missing operand\n");
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

        my_strcpy(env.shm, start_of_arg);
        vfs_lock();
        seL4_SetMR(0, 117); // SYS_MKDIR
        seL4_Call(env.blk_ep, seL4_MessageInfo_new(0, 0, 0, 1));
        int ret = seL4_GetMR(0);
        vfs_unlock();

        if (ret == 1) {
            sys_puts(0, "mkdir: '");
            sys_puts(0, start_of_arg);
            sys_puts(0, "' уже существует — ничего не делаю\n");
        } else if (ret != 0) {
            sys_puts(0, "mkdir: cannot create directory '");
            sys_puts(0, start_of_arg);
            sys_puts(0, "'\n");
        }
        *p = temp_char;
    }
    sys_exit(env.root_ep);
    return 0;
}
