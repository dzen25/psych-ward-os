#include "h/sys_client.h"

// issuse.txt №62 (расследование) — самостоятельно (без участия человека,
// печатающего команды в реальном терминале) прогоняет N раз подряд
// ротацию /sbin-команд, тем же самым сырым протоколом SYS_EXEC/SYS_WAIT,
// каким пользуется shell.cpp (see shell.cpp: seL4_SetMR(0,100)/MR1-24
// имя+аргументы/MR25-32 cwd, и seL4_SetMR(0,106)/MR1=pid для ожидания).
// Перед КАЖДЫМ спавном печатает номер итерации и саму команду — если
// плата зависнет/рутсервер напечатает FATAL FAULT, в логе будет видно
// ТОЧНО после какой команды это случилось, без гадания.
//
// Специально НЕ пытаемся программно детектировать краш/зависание
// дочернего процесса — generic_recover_process() будит ожидающего
// (SYS_WAIT) с тем же "успехом", что и штатное завершение (см. main.cpp),
// так что со стороны этого раннера крash неотличим от обычного выхода.
// Сигнал даёт сам факт FATAL FAULT в живом логе root'а — этот бинарник
// лишь убирает человеческий фактор набора команд и делает прогон
// воспроизводимым 1-в-1 между попытками.

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

static void putdec(int val) {
    char buf[12]; int j = 0;
    if (val == 0) buf[j++] = '0';
    while (val > 0) { buf[j++] = '0' + (val % 10); val /= 10; }
    while (j > 0) { char c[2] = {buf[--j], 0}; sys_puts(0, c); }
}

// По просьбе пользователя (2026-08-23) ls/ps/free/touch/cat/mkdir/rm слиты
// в shell.cpp как builtin-функции (см. run_*() там) — больше не отдельные
// ELF, спавнить их по пути через SYS_EXEC (как раньше) уже нельзя. Ротация
// команд, которая изначально и нашла баг №62, потеряла разнообразие —
// сейчас гоняет единственный оставшийся безопасный к повтору отдельный
// бинарник, /bin/test_app.elf (минимальный, только печатает и выходит, см.
// src/test.cpp). Цель теста (стресс самого spawn_process()/SYS_EXEC/
// SYS_WAIT — CNode/VSpace/TCB-аллокация по кругу) по-прежнему полностью
// покрывается; VFS-специфичные пути (exFAT-запись/чтение/mkdir через
// разные процессы подряд) этим прогоном больше не проверяются.
static const char *ROTATION[] = {
    "/bin/test_app.elf",
};
constexpr int ROTATION_LEN = sizeof(ROTATION) / sizeof(ROTATION[0]);

int main(int argc, char *argv[]) {
    SysClientEnv env;
    sys_client_init(env);

    int iterations = 30;
    if (env.arg && *env.arg) {
        int v = simple_atoi(env.arg);
        if (v > 0) iterations = v;
    }

    sys_puts(0, "stresstest: старт, итераций="); putdec(iterations); sys_puts(0, "\n");

    for (int i = 0; i < iterations; i++) {
        const char *cmd = ROTATION[i % ROTATION_LEN];
        sys_puts(0, "[STRESS] iter "); putdec(i); sys_puts(0, "/"); putdec(iterations);
        sys_puts(0, ": "); sys_puts(0, cmd); sys_puts(0, "\n");

        int pid = raw_exec(env.root_ep, cmd, "/root");
        if (pid > 0) {
            raw_wait(env.root_ep, pid);
        } else {
            sys_puts(0, "[STRESS] spawn failed, status="); putdec(pid); sys_puts(0, "\n");
        }
    }

    sys_puts(0, "stresstest: все итерации пройдены без зависания.\n");
    sys_exit(env.root_ep);
    return 0;
}
