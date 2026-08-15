#include "h/sys_client.h"

// Фаза A (см. ROADMAP.md): вынесено из shell.cpp — см. SYS_SET_AFFINITY в
// main.cpp (Фаза 6.1). Защита root/timer_driver от переноса — на стороне
// root, здесь только разбор ответа-статуса.
int main(int argc, char *argv[]) {
    SysClientEnv env;
    sys_client_init(env);

    char *cursor = env.arg;
    char *pid_str = env.arg ? next_token(&cursor) : nullptr;
    char *core_str = env.arg ? next_token(&cursor) : nullptr;
    if (!pid_str || !core_str) {
        sys_puts(0, "Usage: taskset <pid> <ядро 0-3>\n");
        sys_exit(env.root_ep);
        return 1;
    }
    if (!is_all_digits(pid_str) || !is_all_digits(core_str)) { // issuse.txt №54
        sys_puts(0, "taskset: pid и номер ядра должны быть числами\n");
        sys_exit(env.root_ep);
        return 1;
    }

    seL4_SetMR(0, SYS_SET_AFFINITY);
    seL4_SetMR(1, simple_atoi(pid_str));
    seL4_SetMR(2, simple_atoi(core_str));
    seL4_Call(env.root_ep, seL4_MessageInfo_new(0, 0, 0, 3));
    seL4_Word status = seL4_GetMR(0);
    switch (status) {
        case 0: sys_puts(0, "OK.\n"); break;
        case 1: sys_puts(0, "Процесс не найден.\n"); break;
        case 2: sys_puts(0, "root зафиксирован на ядре 0, перенос невозможен.\n"); break;
        case 3: sys_puts(0, "timer_driver нельзя переносить: держит физический таймер (PPI) через капу, привязанную к ядру 0 — перенос вызовет тихое зависание при следующем перевзведении.\n"); break;
        case 4: sys_puts(0, "Некорректный номер ядра (0-3).\n"); break;
        case 5: sys_puts(0, "Доступ запрещён: taskset может выполнять только shell/доверенные /sbin-сервисы.\n"); break;
        default: sys_puts(0, "Неизвестная ошибка.\n"); break;
    }
    sys_exit(env.root_ep);
    return 0;
}
