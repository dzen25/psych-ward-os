#include "h/sys_client.h"

// Фаза A (см. ROADMAP.md): вынесено из shell.cpp — см. SYS_KILL=102 в main.cpp.
int main(int argc, char *argv[]) {
    SysClientEnv env;
    sys_client_init(env);

    if (!env.arg) { sys_puts(0, "Usage: kill <pid>\n"); sys_exit(env.root_ep); return 1; }
    if (!is_all_digits(env.arg)) { // issuse.txt №54
        sys_puts(0, "kill: pid must be a non-negative number\n");
        sys_exit(env.root_ep);
        return 1;
    }

    seL4_SetMR(0, 102); // SYS_KILL
    seL4_SetMR(1, simple_atoi(env.arg));
    seL4_Call(env.root_ep, seL4_MessageInfo_new(0, 0, 0, 2));
    seL4_Word status = seL4_GetMR(0);
    if ((int)status == -2) {
        sys_puts(0, "Доступ запрещён: kill может выполнять только shell/доверенные /sbin-сервисы.\n");
    } else if ((int)status == -1) {
        sys_puts(0, "Нельзя убить rootserver.\n");
    } else {
        sys_puts(0, "Signal sent.\n");
    }
    sys_exit(env.root_ep);
    return 0;
}
