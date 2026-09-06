#include "h/sys_client.h"

// stalltest — стенд для проверки САМОГО watchdog'а.
//
// ЗАЧЕМ. Пошаговый watchdog (root видит, что счётчик прогресса драйвера
// замер на конкретном шаге, и запускает полный сброс шины) до сих пор ни
// разу не проверялся по-настоящему: чтобы он сработал, нужно настоящее
// зависание USB, а оно случается редко и непредсказуемо. Это была часть
// "а" плана issuse.txt №74 — "стенд, на котором отказ вызывается по
// требованию" — и до неё так и не дошли руки.
//
// КАК. Драйвер по команде USB_CMD_SIMULATE_STALL объявляет шаг и дальше
// крутится заданное время, НЕ трогая счётчик прогресса и не возвращаясь в
// свой seL4_Recv. Для root'а это неотличимо от ядра, замершего на
// аппаратной транзакции: проверяется ровно тот детектор, который должен
// ловить реальный отказ.
//
// ЧТО ДОЛЖНО ПРОИЗОЙТИ (смотреть в лог root'а во время прогона):
//   [WATCHDOG] is_driver=6: прогресс замер на шаге "монтирование exFAT"
//              уже ~10000 мс — это не долгая операция, это зависание.
//   [WATCHDOG] Зависание на шаге (is_driver=6 ...) — auto-recovering.
//   [ROOT] USB RESET: ... (полный сброс шины)
// И, главное, ПЛАТА ДОЛЖНА ОСТАТЬСЯ ЖИВОЙ: тест обязан досчитать до конца
// и напечатать итог.
//
// Требования: usb_driver должен быть в /etc/auto_restart.conf (иначе
// автотриггер выключен и watchdog промолчит — это не баг, а конфиг).
//
// Запуск:
//   exec /sbin/tests/stalltest.elf         — зависание на 15с (порог 10с)
//   exec /sbin/tests/stalltest.elf 20000   — зависание на 20с

static void putdec(int val) {
    char buf[12]; int j = 0;
    if (val < 0) { sys_puts(0, "-"); val = -val; }
    if (val == 0) buf[j++] = '0';
    while (val > 0) { buf[j++] = '0' + (val % 10); val /= 10; }
    while (j > 0) { char c[2] = {buf[--j], 0}; sys_puts(0, c); }
}

static seL4_CPtr g_timer_ep = 0;
static seL4_Word now_ms() {
    seL4_SetMR(0, 3); // SYS_GET_TIME
    seL4_Call(g_timer_ep, seL4_MessageInfo_new(0, 0, 0, 1));
    return seL4_GetMR(0);
}
// Root печатает в UART НАПРЯМУЮ (busy-wait по регистрам), драйвер и тест
// — через uart_driver. Три независимых писателя в одну линию теряют
// символы: в прогоне 2026-09-06 вердикт ТЕСТ 2 вышел нечитаемым месивом.
// Пауза перед печатью своего результата даёт линии успокоиться.
static void settle_uart();

static void sleep_ms(int ms) {
    seL4_SetMR(0, 8); // SYS_SLEEP_MS
    seL4_SetMR(1, (seL4_Word)ms);
    seL4_Call(g_timer_ep, seL4_MessageInfo_new(0, 0, 0, 2));
}

static void settle_uart() { sleep_ms(500); }

int main(int argc, char *argv[]) {
    SysClientEnv env;
    sys_client_init(env);
    g_timer_ep = env.timer_ep;

    int stall_ms = 15000;
    if (env.arg && *env.arg) {
        char buf[64]; my_strlcpy(buf, env.arg, sizeof(buf));
        char *cur = buf; char *t = next_token(&cur);
        if (t && *t && is_all_digits(t)) { int v = simple_atoi(t); if (v > 0) stall_ms = v; }
    }

    sys_puts(0, "==========================================================\n");
    sys_puts(0, "stalltest — проверка срабатывания watchdog'а на зависании\n");
    sys_puts(0, "==========================================================\n");

    if (env.usb_storage_ep == 0) {
        sys_puts(0, "ОСТАНОВЛЕН — ОШИБКА: нет доступа к usb_driver (запускайте полным путём /sbin/tests/stalltest.elf).\n");
        sys_exit(env.root_ep); return 1;
    }

    sys_puts(0, "\nТЕСТ 1: usb_driver жив до начала\n");
    seL4_SetMR(0, 3); // USB_CMD_LIST_VOLUMES
    seL4_Call(env.usb_storage_ep, seL4_MessageInfo_new(0, 0, 0, 1));
    seL4_Word mask_before = seL4_GetMR(0);
    sys_puts(0, "  маска смонтированных томов до зависания = "); putdec((int)mask_before); sys_puts(0, "\n");
    sys_puts(0, "ТЕСТ 1 ЗАВЕРШЁН\n");

    sys_puts(0, "\nТЕСТ 2: имитирую зависание драйвера на "); putdec(stall_ms);
    sys_puts(0, "мс (порог watchdog'а — 10000мс)\n");
    sys_puts(0, "  этот вызов вернётся только после конца зависания — так же,\n");
    sys_puts(0, "  как ждал бы любой реальный клиент зависшего драйвера.\n");

    seL4_Word t0 = now_ms();
    seL4_SetMR(0, 6); // USB_CMD_SIMULATE_STALL
    seL4_SetMR(1, (seL4_Word)stall_ms);
    seL4_Call(env.usb_storage_ep, seL4_MessageInfo_new(0, 0, 0, 2));
    int st = (int)seL4_GetMR(0);
    int took = (int)(now_ms() - t0);
    settle_uart(); // драйвер и root в этот момент договаривают своё в лог

    sys_puts(0, "  вызов вернулся: код "); putdec(st);
    sys_puts(0, ", реально ждали "); putdec(took); sys_puts(0, "мс\n");
    if (st != 0) {
        sys_puts(0, "ТЕСТ 2 ОСТАНОВЛЕН — ОШИБКА: драйвер не принял команду имитации.\n");
        sys_exit(env.root_ep); return 1;
    }
    if (took < stall_ms / 2) {
        sys_puts(0, "ТЕСТ 2 ОСТАНОВЛЕН — ОШИБКА: зависание длилось заметно меньше заказанного, имитация не отработала.\n");
        sys_exit(env.root_ep); return 1;
    }
    sys_puts(0, "ТЕСТ 2 ЗАВЕРШЁН — и, главное, система жива: этот текст печатается.\n");

    settle_uart(); // переэнумерация после сброса шины шумит в лог — дать ей договорить
    sys_puts(0, "\nТЕСТ 3: USB работает после вмешательства watchdog'а\n");
    sys_puts(0, "  (watchdog должен был сбросить шину — ищите в логе выше\n");
    sys_puts(0, "   строку [WATCHDOG] ... прогресс замер на шаге ...)\n");
    seL4_Word mask_after = 0;
    for (int i = 0; i < 60; i++) { // до 12с на переэнумерацию после сброса шины
        sleep_ms(200);
        seL4_SetMR(0, 3); // USB_CMD_LIST_VOLUMES
        seL4_Call(env.usb_storage_ep, seL4_MessageInfo_new(0, 0, 0, 1));
        mask_after = seL4_GetMR(0);
        if (mask_after != 0) break;
    }
    sys_puts(0, "  маска смонтированных томов после = "); putdec((int)mask_after); sys_puts(0, "\n");
    if (mask_before != 0 && mask_after == 0) {
        sys_puts(0, "ТЕСТ 3 ОСТАНОВЛЕН — ОШИБКА: до зависания том был смонтирован, после — нет.\n");
        sys_puts(0, "  Драйвер отвечает (значит плата жива), но шина не восстановилась.\n");
        sys_exit(env.root_ep); return 1;
    }
    sys_puts(0, "ТЕСТ 3 ЗАВЕРШЁН\n");

    sys_puts(0, "\n==========================================================\n");
    sys_puts(0, "ИТОГ: зависание вызвано, система его пережила.\n");
    sys_puts(0, "Сработал ли watchdog — смотрите строки [WATCHDOG] в логе выше.\n");
    sys_puts(0, "==========================================================\n");

    sys_exit(env.root_ep);
    return 0;
}
