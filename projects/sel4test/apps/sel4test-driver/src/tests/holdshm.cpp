#include "h/sys_client.h"

// issuse.txt №5 (тестовый хук) — обычный /sbin-tool (is_driver==253,
// получает SHM как любая другая /sbin-команда через sys_client_init())
// специально блокируется на N секунд, чтобы дать окно для `kill <pid>`
// ПОКА процесс жив и держит SHM — ни одна из штатных /sbin-команд
// (cat/ls/...) не блокируется достаточно долго для этого вручную.
// НАЗВАНИЕ: не "sleep" — в шелле уже есть встроенная команда "sleep <ms>"
// (shell.cpp), которая перехватывает это имя ДО поиска /sbin-бинарника —
// /sbin/sleep.elf был бы физически недостижим (найдено на живом железе,
// первая попытка называлась именно sleep.cpp).
static bool sys_sleep_ms(seL4_CPtr timer_ep, seL4_Word ms) {
    seL4_SetMR(0, 8); // SYS_SLEEP_MS
    seL4_SetMR(1, ms);
    seL4_Call(timer_ep, seL4_MessageInfo_new(0, 0, 0, 2));
    return (int)seL4_GetMR(0) != -1;
}

int main(int argc, char *argv[]) {
    SysClientEnv env;
    sys_client_init(env);

    int total_sec = 20;
    if (env.arg && *env.arg) {
        int v = simple_atoi(env.arg);
        if (v > 0) total_sec = v;
    }

    sys_puts(0, "holdshm: спим (SHM получен, можно 'kill <pid>' из другого окна)...\n");
    for (int i = 0; i < total_sec; i++) {
        if (!sys_sleep_ms(env.timer_ep, 1000)) {
            sys_puts(0, "holdshm: timer_driver отказал (все слоты заняты) — выхожу раньше.\n");
            break;
        }
    }
    sys_puts(0, "holdshm: проснулся, выхожу штатно.\n");

    sys_exit(env.root_ep);
    return 0;
}
