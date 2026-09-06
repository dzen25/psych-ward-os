#include "h/sys_client.h"

// orphantest — детерминированная проверка уборки SYS_CLONE-потоков за
// завершившимся процессом (issuse.txt №3).
//
// ЧТО ПРОВЕРЯЕТ. До фикса ни один из трёх путей завершения процесса
// (SYS_EXIT, SYS_KILL, generic_recover_process) не трогал потоки,
// созданные этим процессом через SYS_CLONE: поле pcb.parent_pid читалось
// РОВНО в одном месте — case SYS_THREAD_EXIT, то есть только когда поток
// завершался сам. Процесс, вышедший с живым потоком, оставлял за собой:
//   - PCB потока навсегда в active=true (утечка PID/TCB/IPC-фрейма/слотов);
//   - исполняющийся поток без адресного пространства (revoke+delete капов
//     родителя сносит общие корни VSpace/CSpace прямо под ним);
//   - захваченный НАВСЕГДА глобальный vfs_mutex_ep, если поток успел его
//     взять — держателем числится PID вечно-живого потока, и освободить
//     его после этого не мог уже никто (та же механика, что в issuse.txt
//     №70, только там жертву добивал watchdog, а тут добивать некому).
//
// ПОЧЕМУ ЭТО НЕ УМОЗРИТЕЛЬНО. coretest/ТЕСТ 7 запускает поток-читатель,
// который держит VFS-мьютекс на всё чтение, и имеет четыре пути выхода по
// ошибке, каждый зовёт sys_exit() — в том числе выход по таймауту, где
// поток ГАРАНТИРОВАННО ещё жив (иначе таймаут бы не наступил). Отсюда и
// наблюдение "отказ требует предшествующего сброса шины": сброс шины
// заставляет чтение упереться в таймаут, то есть создаёт осиротевший поток.
//
// КАК УСТРОЕН ДЕТЕКТОР. Новый syscall не понадобился: все клон-потоки
// называются "shell_thread", а SYS_PROC_INFO ищет по имени и возвращает
// первый АКТИВНЫЙ PCB с таким именем. Значит proc_info("shell_thread")
// == -1 — потоков не осталось, >= 0 — утечка, и сразу видно чей PID.
//
// Запуск:
//   exec /sbin/tests/orphantest.elf         — полный прогон (10 циклов)
//   exec /sbin/tests/orphantest.elf 3       — 3 цикла
//   exec /sbin/tests/orphantest.elf leak    — служебный режим, см. ниже;
//                                             руками запускать незачем

static SysClientEnv env;

static void putdec(int val) {
    char buf[12]; int j = 0;
    if (val < 0) { sys_puts(0, "-"); val = -val; }
    if (val == 0) buf[j++] = '0';
    while (val > 0) { buf[j++] = '0' + (val % 10); val /= 10; }
    while (j > 0) { char c[2] = {buf[--j], 0}; sys_puts(0, c); }
}

// --- обёртки над сисколлами root'а (раскладка 1:1 с coretest/spawnloop) ---

static int proc_info_pid(const char *name) {
    char safe[32] = {0};
    my_strlcpy(safe, name, sizeof(safe));
    seL4_SetMR(0, SYS_PROC_INFO);
    uint64_t *p = (uint64_t*)safe;
    for (int i = 0; i < 4; i++) seL4_SetMR(i + 1, p[i]);
    seL4_Call(env.root_ep, seL4_MessageInfo_new(0, 0, 0, 5));
    return (int)seL4_GetMR(0);
}

static int raw_exec(const char *cmdline, const char *cwd) {
    char payload[192] = {0};
    my_strlcpy(payload, cmdline, sizeof(payload));
    char cwd_payload[64] = {0};
    my_strlcpy(cwd_payload, cwd, sizeof(cwd_payload));

    seL4_SetMR(0, 100); // SYS_EXEC
    uint64_t *name_ptr = (uint64_t*)payload;
    for (int i = 0; i < 24; i++) seL4_SetMR(i + 1, name_ptr[i]);
    uint64_t *cwd_ptr = (uint64_t*)cwd_payload;
    for (int i = 0; i < 8; i++) seL4_SetMR(i + 25, cwd_ptr[i]);
    seL4_Call(env.root_ep, seL4_MessageInfo_new(0, 0, 0, 33));
    return (int)seL4_GetMR(0);
}

static void raw_wait(int pid) {
    seL4_SetMR(0, 106); // SYS_WAIT
    seL4_SetMR(1, (seL4_Word)pid);
    seL4_Call(env.root_ep, seL4_MessageInfo_new(0, 0, 0, 2));
}

// --- служебный режим "leak": намеренно уйти с живым потоком ---

static char g_orphan_stack[8192] __attribute__((aligned(16)));

static void orphan_thread_entry(seL4_Word, seL4_Word, seL4_Word) {
    // Обязательный пролог клонированного потока (см. tests/clonetest.cpp):
    // сам регистр TLS настраивает root, но кеш указателя на IPC-буфер
    // внутри libsel4 поток обязан проинициализировать себе сам.
    seL4_Word tls_addr;
    asm volatile("mrs %0, tpidrro_el0" : "=r"(tls_addr));
    seL4_SetIPCBuffer((seL4_IPCBuffer*)(tls_addr - 1024));

    // Намеренно НЕ зовём SYS_THREAD_EXIT — в этом весь смысл режима.
    // Родитель выйдет прямо из-под нас; после фикса root обязан суспендить
    // этот поток ДО того, как снесёт общий VSpace.
    while (1) seL4_Yield();
}

static int clone_orphan(void) {
    seL4_SetMR(0, 101); // SYS_CLONE
    seL4_SetMR(1, (seL4_Word)(uintptr_t)&orphan_thread_entry);
    seL4_SetMR(2, 0);
    seL4_SetMR(3, 0);
    seL4_SetMR(4, 0);
    seL4_SetMR(5, 0); seL4_SetMR(6, 0); seL4_SetMR(7, 0); // stdin/stdout/stderr не нужны
    seL4_SetMR(8, (seL4_Word)-1); // не пайп
    seL4_SetMR(9, (seL4_Word)(uintptr_t)(g_orphan_stack + sizeof(g_orphan_stack)));
    seL4_Call(env.root_ep, seL4_MessageInfo_new(0, 0, 0, 10));
    return (int)seL4_GetMR(0);
}

// --- каркас шагов (формат тот же, что в coretest) ---

static int g_step = 0, g_passed = 0;

static void step_begin(int n, const char *title) {
    g_step = n;
    sys_puts(0, "\nТЕСТ "); putdec(g_step); sys_puts(0, ": "); sys_puts(0, title); sys_puts(0, "\n");
}
static void step_ok() {
    g_passed++;
    sys_puts(0, "ТЕСТ "); putdec(g_step); sys_puts(0, " ЗАВЕРШЁН\n");
}
static void step_fail(const char *why) {
    sys_puts(0, "ТЕСТ "); putdec(g_step); sys_puts(0, " ОСТАНОВЛЕН — ОШИБКА: ");
    sys_puts(0, why); sys_puts(0, "\n");
}

// Лёгкое обращение к VFS: берёт глобальный мьютекс, делает настоящую
// работу, отдаёт. Если мьютекс заперт осиротевшим потоком — здесь и
// повиснет, что само по себе диагноз. Путь заведомо существующий.
static bool vfs_alive(void) {
    if (!env.shm) return false;
    my_strlcpy(env.shm, "/etc/init.conf", SHM_TOTAL_SIZE);
    return vfs_syscall(114, env.blk_ep) == 0; // SYS_READ_TEXT_FILE
}

int main(int argc, char *argv[]) {
    sys_client_init(env);

    // --- служебный режим ---
    if (env.arg && env.arg[0] == 'l' && env.arg[1] == 'e' && env.arg[2] == 'a' && env.arg[3] == 'k') {
        int tid = clone_orphan();
        sys_puts(0, "  [leak] поток создан, pid="); putdec(tid);
        sys_puts(0, " — выхожу, НЕ дожидаясь его.\n");
        sys_exit(env.root_ep);
        return 0;
    }

    int cycles = 10;
    if (env.arg && *env.arg && is_all_digits(env.arg)) {
        int v = simple_atoi(env.arg);
        if (v > 0) cycles = v;
    }

    sys_puts(0, "==========================================================\n");
    sys_puts(0, "orphantest — уборка SYS_CLONE-потоков за умершим процессом\n");
    sys_puts(0, "issuse.txt №3, циклов: "); putdec(cycles); sys_puts(0, "\n");
    sys_puts(0, "==========================================================\n");

    // ТЕСТ 1 — чистый старт
    step_begin(1, "перед началом в системе нет висящих клон-потоков");
    {
        int stale = proc_info_pid("shell_thread");
        if (stale >= 0) {
            sys_puts(0, "  найден активный shell_thread, pid="); putdec(stale); sys_puts(0, "\n");
            step_fail("система уже содержит осиротевший поток от прошлого прогона — прогон не с чистого листа, нужна перезагрузка");
            sys_exit(env.root_ep);
            return 1;
        }
        sys_puts(0, "  висящих клон-потоков нет\n");
        step_ok();
    }

    // ТЕСТ 2 — VFS исправна ДО начала (иначе шаг 5 нечего будет сравнивать)
    step_begin(2, "VFS отвечает до начала прогона");
    if (!vfs_alive()) { step_fail("не удалось прочитать /etc/init.conf ещё до создания утечек"), sys_exit(env.root_ep); return 1; }
    sys_puts(0, "  /etc/init.conf прочитан\n");
    step_ok();

    // ТЕСТ 3 — порождение утечки
    step_begin(3, "процесс уходит, оставляя живой SYS_CLONE-поток");
    {
        int pid = raw_exec("/sbin/tests/orphantest.elf leak", "/root");
        if (pid <= 0) { step_fail("не удалось заспавнить себя в режиме leak"), sys_exit(env.root_ep); return 1; }
        raw_wait(pid);
        sys_puts(0, "  дочерний процесс pid="); putdec(pid); sys_puts(0, " завершился\n");
        step_ok();
    }

    // ТЕСТ 4 — ГЛАВНЫЙ КРИТЕРИЙ
    step_begin(4, "root убрал поток за ушедшим родителем");
    {
        int leaked = proc_info_pid("shell_thread");
        if (leaked >= 0) {
            sys_puts(0, "  остался активный shell_thread, pid="); putdec(leaked); sys_puts(0, "\n");
            step_fail("поток пережил родителя — уборка не сработала (это и есть issuse.txt №3)");
            sys_exit(env.root_ep);
            return 1;
        }
        sys_puts(0, "  висящих клон-потоков не осталось\n");
        step_ok();
    }

    // ТЕСТ 5 — VFS не заперта осиротевшим держателем
    step_begin(5, "VFS отвечает после уборки");
    if (!vfs_alive()) { step_fail("после утечки VFS перестала отвечать — похоже, мьютекс остался захвачен"), sys_exit(env.root_ep); return 1; }
    sys_puts(0, "  /etc/init.conf прочитан\n");
    step_ok();

    // ТЕСТ 6 — накопление: одна успешная уборка ничего не доказывает
    step_begin(6, "уборка работает многократно, ресурсы не накапливаются");
    {
        for (int i = 1; i <= cycles; i++) {
            int pid = raw_exec("/sbin/tests/orphantest.elf leak", "/root");
            if (pid <= 0) {
                sys_puts(0, "  цикл "); putdec(i); sys_puts(0, ": спавн не удался, код "); putdec(pid); sys_puts(0, "\n");
                step_fail("спавн перестал удаваться — похоже на исчерпание PID или слотов CNode");
                sys_exit(env.root_ep);
                return 1;
            }
            raw_wait(pid);
            int leaked = proc_info_pid("shell_thread");
            if (leaked >= 0) {
                sys_puts(0, "  цикл "); putdec(i); sys_puts(0, ": остался shell_thread pid="); putdec(leaked); sys_puts(0, "\n");
                step_fail("уборка сработала не на каждом цикле");
                sys_exit(env.root_ep);
                return 1;
            }
            if (!vfs_alive()) {
                sys_puts(0, "  цикл "); putdec(i); sys_puts(0, ": VFS не ответила\n");
                step_fail("VFS отвалилась в середине прогона");
                sys_exit(env.root_ep);
                return 1;
            }
            sys_puts(0, "  цикл "); putdec(i); sys_puts(0, "/"); putdec(cycles); sys_puts(0, " — чисто\n");
        }
        step_ok();
    }

    sys_puts(0, "\n==========================================================\n");
    sys_puts(0, "ИТОГ: пройдено шагов "); putdec(g_passed); sys_puts(0, " из 6.\n");
    sys_puts(0, "==========================================================\n");

    sys_exit(env.root_ep);
    return 0;
}
