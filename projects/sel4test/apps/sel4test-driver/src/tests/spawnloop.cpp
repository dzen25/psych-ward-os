#include "h/sys_client.h"

// spawnloop — воспроизводитель плавающего падения тестового процесса
// (issuse.txt №3: FATAL FAULT с PC=0 при запуске двух утилит подряд).
//
// ПОЧЕМУ ОН НУЖЕН. Отказ плавающий: две попытки из трёх. Ловить его,
// набирая две команды руками и надеясь, — то же самое, чем мы уже
// намучились с флешкой: минуты на попытку и несравнимые между собой
// прогоны. Здесь та же связка гоняется автоматически столько раз,
// сколько нужно, а поймать её должна уже стоящая в rootserver'е ловушка:
// обработчик FATAL FAULT печатает имя процесса, SP, LR (откуда прыгнули),
// FP и x0..x3.
//
// ЧТО ИМЕННО ГОНЯЕТСЯ. По умолчанию — короткий цикл переподключения
// (одна итерация, питание снято 300мс): процесс спавнится, разговаривает
// с драйвером, завершается. Ровно та последовательность "второй
// /sbin/tests-процесс подряд", в которой отказ и наблюдался, только без
// пятнадцатисекундного зависания и пятидесяти циклов.
//
// КАК ЧИТАТЬ РЕЗУЛЬТАТ. Сам spawnloop про падение ребёнка не узнаёт —
// SYS_WAIT возвращается одинаково и после нормального выхода, и после
// восстановления упавшего процесса (см. комментарий в stresstest.cpp,
// там та же особенность). Признак — строки FATAL FAULT в логе root'а.
// Поэтому перед КАЖДЫМ спавном печатается номер итерации: в логе будет
// точно видно, на какой именно из них случилось.
//
// Запуск:
//   exec /sbin/tests/spawnloop.elf                — 30 итераций
//   exec /sbin/tests/spawnloop.elf 100            — 100 итераций
//   exec /sbin/tests/spawnloop.elf 50 /bin/test_app.elf
//        — то же число итераций, но гонять другой бинарник (контрольный
//          опыт: если с ним НЕ падает, а с hotplugtest падает, дело в
//          том, что делает именно он, а не в спавне как таковом)

static void putdec(int val) {
    char buf[12]; int j = 0;
    if (val < 0) { sys_puts(0, "-"); val = -val; }
    if (val == 0) buf[j++] = '0';
    while (val > 0) { buf[j++] = '0' + (val % 10); val /= 10; }
    while (j > 0) { char c[2] = {buf[--j], 0}; sys_puts(0, c); }
}

// Сырые SYS_EXEC/SYS_WAIT — та же раскладка регистров, что у shell.cpp и
// stresstest.cpp (MR0=100, MR1-24 имя+аргументы, MR25-32 cwd).
static int raw_exec(seL4_CPtr root_ep, const char *cmdline, const char *cwd) {
    char payload[192] = {0};
    my_strlcpy(payload, cmdline, sizeof(payload));
    char cwd_payload[64] = {0};
    my_strlcpy(cwd_payload, cwd, sizeof(cwd_payload));

    seL4_SetMR(0, 100); // SYS_EXEC
    uint64_t *name_ptr = (uint64_t*)payload;
    for (int i = 0; i < 24; i++) seL4_SetMR(i + 1, name_ptr[i]);
    uint64_t *cwd_ptr = (uint64_t*)cwd_payload;
    for (int i = 0; i < 8; i++) seL4_SetMR(i + 25, cwd_ptr[i]);
    seL4_Call(root_ep, seL4_MessageInfo_new(0, 0, 0, 33));
    return (int)seL4_GetMR(0);
}

static void raw_wait(seL4_CPtr root_ep, int pid) {
    seL4_SetMR(0, 106); // SYS_WAIT
    seL4_SetMR(1, (seL4_Word)pid);
    seL4_Call(root_ep, seL4_MessageInfo_new(0, 0, 0, 2));
}

int main(int argc, char *argv[]) {
    SysClientEnv env;
    sys_client_init(env);

    int iterations = 30;
    char cmd[160];
    my_strlcpy(cmd, "/sbin/tests/hotplugtest.elf 1 300", sizeof(cmd));

    if (env.arg && *env.arg) {
        char buf[192]; my_strlcpy(buf, env.arg, sizeof(buf));
        char *cur = buf;
        char *t1 = next_token(&cur);
        if (t1 && *t1 && is_all_digits(t1)) {
            int v = simple_atoi(t1);
            if (v > 0) iterations = v;
            // всё, что осталось после числа, — команда целиком
            if (cur && *cur) my_strlcpy(cmd, cur, sizeof(cmd));
        } else if (t1 && *t1) {
            my_strlcpy(cmd, env.arg, sizeof(cmd));
        }
    }

    sys_puts(0, "==========================================================\n");
    sys_puts(0, "spawnloop — ловля плавающего падения (issuse.txt №3)\n");
    sys_puts(0, "итераций: "); putdec(iterations); sys_puts(0, "\n");
    sys_puts(0, "команда:  "); sys_puts(0, cmd); sys_puts(0, "\n");
    sys_puts(0, "Признак отказа — строки FATAL FAULT в логе rootserver'а.\n");
    sys_puts(0, "==========================================================\n");

    int spawned = 0, failed_spawn = 0;
    for (int i = 1; i <= iterations; i++) {
        sys_puts(0, "\n--- ИТЕРАЦИЯ "); putdec(i); sys_puts(0, "/"); putdec(iterations); sys_puts(0, " ---\n");
        int pid = raw_exec(env.root_ep, cmd, "/root");
        if (pid <= 0) {
            failed_spawn++;
            sys_puts(0, "ИТЕРАЦИЯ "); putdec(i);
            sys_puts(0, " ОСТАНОВЛЕНА — ОШИБКА: спавн не удался, код "); putdec(pid); sys_puts(0, "\n");
            break;
        }
        spawned++;
        raw_wait(env.root_ep, pid);
    }

    sys_puts(0, "\n==========================================================\n");
    sys_puts(0, "ИТОГ: успешно заспавнено "); putdec(spawned);
    sys_puts(0, " из "); putdec(iterations);
    if (failed_spawn) sys_puts(0, " (прогон прерван неудачным спавном)");
    sys_puts(0, "\nЕсли в логе выше нет FATAL FAULT — отказ не воспроизвёлся.\n");
    sys_puts(0, "==========================================================\n");

    sys_exit(env.root_ep);
    return 0;
}
