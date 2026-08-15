#include "h/sys_client.h"

// issuse.txt №3 (тестовый хук) — проверяет ветку ОТКАЗА is_admin_caller()
// в обработчике SYS_RECOVER (main.cpp, case SYS_RECOVER): ни один
// существующий инструмент раньше не бил по этому сисколлу как untrusted
// exec (is_driver==254) — sys_recover() в shell.cpp вызывается только из
// самого шелла (is_driver==0, admin), это проверяет лишь разрешённый путь.
//
// Получить is_driver==254 для ЭТОГО ЖЕ бинарника можно, запустив его НЕ
// через буквальный префикс "/sbin/" — main.cpp сравнивает введённую
// строку команды, а не резолвленный путь на диске (см. main.cpp,
// is_trusted_sbin). Например: `cd /sbin`, затем `exec recovertest.elf
// <имя>` — файл физически лежит в /sbin, но раз строка не начинается с
// "/sbin/", вызывающий получает is_driver==254.
//
// Аргумент — имя целевого процесса (например "holdshm", запущенный
// заранее в фоне: `exec /sbin/holdshm.elf 20 &`, чтобы respawn был
// безобиден). Печатает статус, вернувшийся от SYS_RECOVER:
//   0  — respawn выполнен (БАГ №3 ЖИВ, если вызывающий не admin)
//  -1  — цель не найдена
//  -2  — отказано в доступе (ожидаемый результат для untrusted exec)
int main(int argc, char *argv[]) {
    SysClientEnv env;
    sys_client_init(env);

    if (!env.arg || !*env.arg) {
        sys_puts(0, "recovertest: usage: recovertest <target_process_name>\n");
        sys_exit(env.root_ep);
        return 1;
    }

    char *target = next_token(&env.arg);

    char safe_name[32] = {0};
    my_strlcpy(safe_name, target, sizeof(safe_name));

    // 1:1 упаковка с sys_recover() в shell.cpp.
    seL4_SetMR(0, 117); // SYS_RECOVER
    uint64_t *name_ptr = (uint64_t*)safe_name;
    for (int i = 0; i < 4; i++) seL4_SetMR(i + 1, name_ptr[i]);
    seL4_Call(env.root_ep, seL4_MessageInfo_new(0, 0, 0, 5));
    int status = (int)seL4_GetMR(0);

    sys_puts(0, "recovertest: SYS_RECOVER(\"");
    sys_puts(0, safe_name);
    sys_puts(0, "\") status=");
    if (status == 0) sys_puts(0, "0 (respawn выполнен — БАГ №3 ЖИВ, если вызывающий не admin)\n");
    else if (status == -1) sys_puts(0, "-1 (цель не найдена)\n");
    else if (status == -2) sys_puts(0, "-2 (отказано в доступе — ожидаемо для untrusted exec)\n");
    else sys_puts(0, "? (неожиданный код)\n");

    sys_exit(env.root_ep);
    return 0;
}
