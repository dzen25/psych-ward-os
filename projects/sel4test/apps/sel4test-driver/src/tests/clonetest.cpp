#include "h/sys_client.h"

// issuse.txt №64 — SYS_CLONE(=101 в main.cpp)/SYS_THREAD_EXIT(=105) не
// вызываются НИКЕМ в кодовой базе с тех пор, как grep перевели на прямой
// вызов функции (run_grep_filter() в shell.cpp, issuse.txt) — единственный
// прежний пользователь. Раскладка MR — 1:1 со старым spawn_thread() из
// shell.cpp (см. git history коммита 535bed6^, до удаления):
//   MR1=entry_point, MR2-4=arg0-2, MR5-7=stdin/stdout/stderr caps,
//   MR8=pipe_id, MR9=stack_top. Ответ: MR0=new_pid (или -1, если PID'ов нет).
//
// Проверено по исходникам самого ядра (kernel/src/kernel/faulthandler.c,
// kernel/src/object/tcb.c:decodeTCBConfigure) — старая гипотеза в
// issuse.txt ("fault_ep резолвится в CSpace root'а") НЕ подтвердилась:
// в non-MCS сборке fault_ep хранится как СЫРОЙ cptr (tcb.c:1799,
// `target->tcbFaultHandler = faultep`) и резолвится ТОЛЬКО в момент
// реального fault'а через lookupCap(tptr, ...), где tptr — САМ упавший
// поток (faulthandler.c:75), не root. Раз клонированный поток делит
// CSpace с родителем (main.cpp: pcb.cspace = pcbs[sender_pid].cspace) —
// и туда же был вминчен badge-капа (case SYS_CLONE, local_thread_fault_ep)
// — резолюция должна отработать корректно. Причина зависания где-то ещё.
//
// Этот тест не гадает, а НАБЛЮДАЕТ прогресс потока напрямую по памяти
// (общая VSpace с родителем — обычные static-переменные, без IPC) —
// даёт log-видимый сигнал БЕЗ JTAG в первую очередь: печатаем состояние
// флагов каждую секунду из родителя. Если этого не хватит (например,
// поток вообще не планируется и флаг reached_entry никогда не станет 1,
// а с ним и родитель почему-то перестанет отвечать) — точки для JTAG:
// clone_thread_entry (первая инструкция потока) и метка ПОСЛЕ seL4_Call
// внутри неё (см. комментарии на месте).

static char g_clone_stack[16384] __attribute__((aligned(16)));
static volatile int g_thread_reached_entry = 0;
static volatile int g_thread_call_returned = 0;
static volatile seL4_Word g_thread_result = 0xDEADBEEF;
// cap_null_cap=0, cap_endpoint_cap=4 (build-rpi4/kernel/generated/arch/
// object/structures_gen.h) — seL4_DebugCapIdentify(root_ep) ДО seL4_Call
// напрямую проверяет, резолвится ли cptr, которым пользуется поток, в
// endpoint-capability В ЕГО СОБСТВЕННОМ CSpace (kernel/src/api/syscall.c,
// SysDebugCapIdentify -> lookupCapAndSlot(ksCurThread, cptr)) — без
// гадания по семантике TCB_Configure/fault_ep (та гипотеза из issuse.txt
// уже проверена по исходникам ядра и НЕ подтвердилась, см. комментарий
// в начале файла).
static volatile seL4_Word g_thread_cap_type = 0xDEADBEEF;

// JTAG-точка №1: первая исполняемая инструкция потока — если сюда никогда
// не доходит (даже после долгого ожидания), поток не был реально
// запланирован (проблема в TCB_Configure/TCB_Resume/регистрах контекста),
// а не в самом seL4_Call.
static void clone_thread_entry(seL4_Word root_ep_raw, seL4_Word unused1, seL4_Word unused2) {
    // КРИТИЧНО (найдено на живом железе 2026-08-23, отсутствие этого дало
    // FATAL FAULT/PID убит вотчдогом СРАЗУ ПОСЛЕ того, как фикс №64 заставил
    // root реально ответить) — тот же шаг, что был в историческом
    // grep_thread_func (git-история shell.cpp до удаления): libsel4
    // кеширует указатель на IPC-буфер в TLS (tpidrro_el0), root настраивает
    // САМ РЕГИСТР через TCB_WriteRegisters/SetTLSBase (см. case SYS_CLONE,
    // main.cpp), но НЕ инициализирует библиотечный кеш seL4_GetIPCBuffer()
    // за нас — это должен сделать сам поток при старте, как обычный main()
    // любого /sbin-бинарника (см. sys_client_init()). Без этого GetMR/SetMR
    // для более длинных сообщений и любые обращения к IPC-буферу через
    // libsel4 бьют в непроинициализированный указатель.
    seL4_Word tls_addr;
    asm volatile("mrs %0, tpidrro_el0" : "=r"(tls_addr));
    seL4_SetIPCBuffer((seL4_IPCBuffer*)(tls_addr - 1024));

    g_thread_reached_entry = 1;
    seL4_CPtr root_ep = (seL4_CPtr)root_ep_raw;

    g_thread_cap_type = seL4_DebugCapIdentify(root_ep);

    // SYS_PS=104 — безобидный синхронный вызов к root, тот же путь, что
    // используют run_ps()/остальные builtin-команды shell.cpp. Ничего не
    // печатает сам (SHM не наш) — важен только факт возврата.
    seL4_SetMR(0, 104);
    seL4_Call(root_ep, seL4_MessageInfo_new(0, 0, 0, 1));
    g_thread_result = seL4_GetMR(0);

    // JTAG-точка №2: сюда доходим, ТОЛЬКО ЕСЛИ seL4_Call реально вернулся.
    // Если issuse.txt №64 воспроизводится — исполнение никогда сюда не
    // попадёт, g_thread_call_returned навсегда останется 0.
    g_thread_call_returned = 1;

    // hw-найдено 2026-08-24: раньше здесь был while(1) seL4_Yield() — держать
    // поток живым для JTAG-инспекции, если #64 воспроизведётся. Теперь, когда
    // #64 исправлен (см. main.cpp/case SYS_CLONE), поток нормально доходит
    // сюда и должен КОРРЕКТНО завершиться — родитель делает sys_exit() сразу
    // после того, как увидит call_returned=1, и если поток продолжает жить в
    // Yield-цикле, родитель уничтожает ОБЩИЙ (см. SYS_CLONE: pcb.vspace/
    // cspace = родительские) VSpace/CSpace прямо под ним — кернел печатает
    // "Caught cap fault in send phase ... Invalid vspace" (подтверждено на
    // железе). SYS_THREAD_EXIT=105 (main.cpp, case 105) уже сам делает полную
    // уборку (revoke/delete всех кап потока, суспенд TCB, pcb.active=false) —
    // используем её вместо самодельного зависания. Заодно это регрессионный
    // тест и на SYS_THREAD_EXIT (тот же класс бага с бейджем, что и у
    // SYS_CLONE, был исправлен тем же самым фиксом).
    seL4_SetMR(0, 105); // SYS_THREAD_EXIT
    seL4_Call(root_ep, seL4_MessageInfo_new(0, 0, 0, 1));
    while (1) seL4_Yield(); // сюда кернел не должен дать вернуться (TCB suspended) — на всякий случай
}

static int raw_clone(seL4_CPtr root_ep, seL4_Word entry, seL4_Word stack_top,
                      seL4_Word arg0, seL4_Word arg1, seL4_Word arg2) {
    seL4_SetMR(0, 101); // SYS_CLONE
    seL4_SetMR(1, entry);
    seL4_SetMR(2, arg0);
    seL4_SetMR(3, arg1);
    seL4_SetMR(4, arg2);
    seL4_SetMR(5, 0); // stdin_cap — не нужен
    seL4_SetMR(6, 0); // stdout_cap — не нужен, поток ничего не пишет
    seL4_SetMR(7, 0); // stderr_cap
    seL4_SetMR(8, -1); // pipe_id — не пайп
    seL4_SetMR(9, stack_top);
    seL4_Call(root_ep, seL4_MessageInfo_new(0, 0, 0, 10));
    return (int)seL4_GetMR(0);
}

static void putdec(int val) {
    char buf[12]; int j = 0;
    if (val == 0) buf[j++] = '0';
    while (val > 0) { buf[j++] = '0' + (val % 10); val /= 10; }
    while (j > 0) { char c[2] = {buf[--j], 0}; sys_puts(0, c); }
}

static void puthex(seL4_Word val) {
    char buf[17]; buf[16] = '\0';
    const char hex[] = "0123456789abcdef";
    for (int i = 15; i >= 0; i--) { buf[15 - i] = hex[(val >> (i * 4)) & 0xF]; }
    sys_puts(0, "0x"); sys_puts(0, buf);
}

static seL4_Word get_time_ms(seL4_CPtr timer_ep) {
    seL4_SetMR(0, 3); // SYS_GET_TIME
    seL4_Call(timer_ep, seL4_MessageInfo_new(0, 0, 0, 1));
    return seL4_GetMR(0);
}

int main(int argc, char *argv[]) {
    SysClientEnv env;
    sys_client_init(env);

    sys_puts(0, "clonetest: старт. перед SYS_CLONE.\n");

    seL4_Word entry = (seL4_Word)clone_thread_entry;
    seL4_Word stack_top = (seL4_Word)(g_clone_stack + sizeof(g_clone_stack) - 16);

    int new_pid = raw_clone(env.root_ep, entry, stack_top, (seL4_Word)env.root_ep, 0, 0);

    sys_puts(0, "clonetest: SYS_CLONE вернул pid=");
    putdec(new_pid);
    sys_puts(0, "\n");

    if (new_pid <= 0) {
        sys_puts(0, "clonetest: спавн потока не удался, дальше нечего ждать.\n");
        sys_exit(env.root_ep);
        return 1;
    }

    // НЕ SYS_WAIT — поток нарочно не завершается сам (вечный цикл в конце
    // clone_thread_entry), ждать через SYS_WAIT означало бы зависнуть
    // самим по конструкции теста, а не из-за бага. Вместо этого — простой
    // опрос раз в секунду, 20 секунд с запасом (issuse.txt №64: зависание
    // 100% детерминированное и мгновенное, если оно есть — 20с более чем
    // достаточно, чтобы отличить "зависло" от "просто медленно").
    for (int i = 0; i < 20; i++) {
        seL4_Word t0 = get_time_ms(env.timer_ep);
        while (get_time_ms(env.timer_ep) - t0 < 1000) seL4_Yield();

        sys_puts(0, "clonetest: t="); putdec(i + 1);
        sys_puts(0, "с reached_entry="); putdec(g_thread_reached_entry);
        sys_puts(0, " call_returned="); putdec(g_thread_call_returned);
        sys_puts(0, " cap_type="); puthex(g_thread_cap_type);
        sys_puts(0, " (0=null,4=endpoint,0xdeadbeef=ещё не дошли до проверки)\n");

        if (g_thread_call_returned) {
            sys_puts(0, "clonetest: ГОТОВО — seL4_Call из клонированного потока ВЕРНУЛСЯ. result=");
            putdec((int)g_thread_result);
            sys_puts(0, "\n issuse.txt №64 НЕ воспроизвелось (либо уже исправлено, либо условия отличаются).\n");
            sys_exit(env.root_ep);
            return 0;
        }
    }

    sys_puts(0, "clonetest: 20с прошло, call_returned так и не стал 1 — ");
    if (g_thread_reached_entry) {
        sys_puts(0, "поток стартовал, но seL4_Call из него не вернулся (issuse.txt №64 воспроизведено).\n");
        sys_puts(0, "clonetest: cap_type потока для root_ep = "); puthex(g_thread_cap_type);
        sys_puts(0, " (4=endpoint — cap в порядке, значит зависание НЕ в резолюции CSpace).\n");
    } else {
        sys_puts(0, "поток вообще НЕ стартовал (reached_entry так и не стал 1) — проблема раньше, в TCB_Configure/TCB_Resume, не в seL4_Call.\n");
    }

    // kernel/include/api/debug.h: debug_dumpScheduler() печатает ИМЯ/
    // СОСТОЯНИЕ/PC/приоритет/ядро КАЖДОГО TCB — НО через NODE_STATE(),
    // т.е. только TCB-список ТОГО ЯДРА, на котором сейчас исполняется САМ
    // вызывающий (проверено на железе 2026-08-23: первый прогон показал
    // всего 3 потока, все на cpu3 — ядро, где, видимо, планируется сам
    // clonetest, а не обязательно наш клонированный поток). Клонированный
    // поток НИКАК явно не пинится на ядро (SYS_CLONE в main.cpp не зовёт
    // SYS_SET_AFFINITY) — где он реально окажется, заранее не известно.
    // Обходим ВСЕ 4 ядра сами: переставляем СЕБЯ (родителя) по очереди на
    // каждое через SYS_SET_AFFINITY (тот же путь, что `taskset`) и с
    // каждого зовём dump — так увидим полный список процессов на каждом
    // ядре за один прогон, без JTAG.
    seL4_SetMR(0, 108); // SYS_GETPID
    seL4_Call(env.root_ep, seL4_MessageInfo_new(0, 0, 0, 1));
    int my_pid = (int)seL4_GetMR(0);
    sys_puts(0, "clonetest: свой pid="); putdec(my_pid); sys_puts(0, "\n");

    for (int core = 0; core < 4; core++) {
        seL4_SetMR(0, 137); // SYS_SET_AFFINITY
        seL4_SetMR(1, (seL4_Word)my_pid);
        seL4_SetMR(2, (seL4_Word)core);
        seL4_Call(env.root_ep, seL4_MessageInfo_new(0, 0, 0, 3));
        seL4_Word aff_status = seL4_GetMR(0);

        sys_puts(0, "\nclonetest: ===== ядро "); putdec(core);
        sys_puts(0, " (SYS_SET_AFFINITY status="); putdec((int)aff_status);
        sys_puts(0, ") =====\n");
        if (aff_status != 0) {
            sys_puts(0, "clonetest: не удалось переехать на это ядро, пропускаю дамп.\n");
            continue;
        }
        seL4_DebugDumpScheduler();
    }
    sys_puts(0, "\nclonetest: обход всех ядер завершён (см. вывод выше в этом же логе).\n");

    // Зависаем нарочно (не sys_exit) — держим и родителя, и дочерний поток
    // в стабильном, легко находимом по PID/имени состоянии, если дамп
    // выше всё же не дал полной картины и понадобится JTAG.
    while (1) seL4_Yield();
    return 0;
}
