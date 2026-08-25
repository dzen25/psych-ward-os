#include "h/sys_client.h"

// issuse.txt №69 — детерминированный регрессионный тест на РОВНО ту гонку,
// которую ручной timedread+kill не смог надёжно поймать (окно "убить
// РОВНО посреди обработки VFS-команды" на реальном 4КБ chunk'е — единицы
// миллисекунд, слишком узко для набора kill'а руками; манёвр либо вообще
// не задевал driver, либо ловил другую, отдельную находку — утечку
// vfs_mutex_ep при убийстве держащего его клиента, см. issuse.txt).
//
// Посылает ОДИН SYS_READ_FILE с offset=KILL_WINDOW_TEST_OFFSET (common.h)
// НАПРЯМУЮ в blk_ep, БЕЗ vfs_lock() — сам тест не должен зависеть от
// глобального VFS-мьютекса (если тест случайно убьют вместо driver'а,
// мьютекс не течёт, в отличие от timedread). blk_driver.cpp узнаёт этот
// сентинел и делает искусственную 5-секундную паузу ПОСЛЕ SaveCaller(),
// ДО настоящей работы — гарантированное окно, чтобы во ВТОРОМ терминале
// (или сразу следом за этой командой) выполнить `kill <pid blk_driver>`.
//
// Три исхода по итоговому статусу/времени:
//  - status=0,  t≈5000мс  — driver НЕ убивали (или сообщение долетело до
//    оригинального процесса и он честно доответил) — контрольный прогон.
//  - status=-1, t<<5000мс — root поймал повисшую reply-капу через
//    generic_recover_process()/VFS_PENDING_REPLY_SLOT (main.cpp) и ответил
//    сам — ИМЕННО это доказывает фикс issuse.txt №69.
//  - status=0,  t≈5000..10000мс — сообщение НЕ было ещё доставлено в
//    момент убийства (driver был на seL4_Recv), просто застряло в очереди
//    endpoint'а и его подхватил уже НОВЫЙ (респавненный) blk_driver —
//    тоже нормальный исход, но НЕ подтверждает именно rescue-механизм.
static void putdec(int val) {
    char buf[12]; int j = 0;
    if (val == 0) buf[j++] = '0';
    while (val > 0) { buf[j++] = '0' + (val % 10); val /= 10; }
    while (j > 0) { char c[2] = {buf[--j], 0}; sys_puts(0, c); }
}

static seL4_Word get_time_ms(seL4_CPtr timer_ep) {
    seL4_SetMR(0, 3); // SYS_GET_TIME
    seL4_Call(timer_ep, seL4_MessageInfo_new(0, 0, 0, 1));
    return seL4_GetMR(0);
}

int main(int argc, char *argv[]) {
    SysClientEnv env;
    sys_client_init(env);

    sys_puts(0, "killwindow: отправляю SYS_READ_FILE(offset=KILL_WINDOW_TEST_OFFSET) в blk_driver.\n");
    sys_puts(0, "killwindow: у тебя ~5с — набери kill <pid blk_driver> ПРЯМО СЕЙЧАС.\n");

    seL4_Word t_start = get_time_ms(env.timer_ep);

    seL4_SetMR(0, 119); // SYS_READ_FILE
    seL4_SetMR(1, (seL4_Word)0xFFFFFFF0u); // KILL_WINDOW_TEST_OFFSET, см. common.h
    seL4_Call(env.blk_ep, seL4_MessageInfo_new(0, 0, 0, 2));
    int status = (int)seL4_GetMR(0);

    seL4_Word t_end = get_time_ms(env.timer_ep);
    seL4_Word elapsed = t_end - t_start;

    sys_puts(0, "killwindow: получен ответ. status="); putdec(status);
    sys_puts(0, " t="); putdec((int)elapsed); sys_puts(0, "мс\n");

    if (status == -1 && elapsed < 4000) {
        sys_puts(0, "killwindow: RESCUE СРАБОТАЛ — root поймал повисшую reply-капу и ответил сам (issuse.txt №69 подтверждён).\n");
    } else if (status == 0 && elapsed < 6000) {
        sys_puts(0, "killwindow: контрольный прогон — driver не убивали (или не успели), это норма.\n");
    } else if (status == 0) {
        sys_puts(0, "killwindow: сообщение подхватил уже НОВЫЙ (респавненный) driver из очереди endpoint'а — тоже не завис, но rescue не проверен этим прогоном.\n");
    } else {
        sys_puts(0, "killwindow: неожиданный результат — смотри лог root'а.\n");
    }

    sys_exit(env.root_ep);
    return 0;
}
