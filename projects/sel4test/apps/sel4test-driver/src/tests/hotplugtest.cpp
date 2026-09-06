#include "h/sys_client.h"

// hotplugtest — автотест устойчивости монтирования/размонтирования
// USB-накопителя к многократному горячему переподключению.
//
// ЗАЧЕМ. Отказ, ради которого тест написан, воспроизводился так:
// пользователь физически вынимал и вставлял флешку, и на 4-10 цикле
// монтирование заклинивало, а вместе с ним умирала вся плата. Проверка
// стоила нескольких минут ручной работы и давала невоспроизводимый
// результат — "иногда на четвёртом, иногда на десятом". Отлаживать так
// нельзя: каждая гипотеза требует нового ритуала, и ни один прогон не
// сравним с другим.
//
// КАК. Хаб умеет снимать и подавать питание на свой downstream-порт
// (PORT_POWER, USB 2.0 spec 11.24.2). Для устройства это ЭЛЕКТРИЧЕСКИ
// то же самое, что вынуть и вставить: пропадает VBUS, хаб рапортует
// disconnect, потом connect, дальше работает ровно тот же путь
// перечисления и монтирования, что и при физическом переподключении.
// Драйвер делает это по команде USB_CMD_PORT_POWER_CYCLE (см.
// usb_driver.cpp), тест её дёргает N раз и после каждого цикла проверяет,
// вернулся ли том.
//
// ЧТО СЧИТАЕТСЯ УСПЕХОМ ЦИКЛА. Мало увидеть выставленный бит
// "смонтировано" — флаг мог остаться от прошлой инкарнации слота.
// Поэтому дополнительно запрашивается размер/свободное место
// (USB_CMD_GET_ALL_SPACE): это заставляет драйвер реально прочитать
// файловую систему. Цикл засчитан, только если ФС отвечает.
//
// Запуск:
//   exec /sbin/tests/hotplugtest.elf            — 20 циклов, пауза 800мс
//   exec /sbin/tests/hotplugtest.elf 50         — 50 циклов
//   exec /sbin/tests/hotplugtest.elf 50 1500    — 50 циклов, питание снято 1.5с

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
static void sleep_ms(int ms) {
    seL4_SetMR(0, 8); // SYS_SLEEP_MS
    seL4_SetMR(1, (seL4_Word)ms);
    seL4_Call(g_timer_ep, seL4_MessageInfo_new(0, 0, 0, 2));
}

// Сколько слотов сейчас реально смонтировано И отвечает на запрос ФС.
// Именно "и": голый бит mounted мог пережить прошлое подключение.
static int mounted_and_alive(seL4_CPtr usb_ep, char *name_out) {
    if (usb_ep == 0) return 0;
    seL4_SetMR(0, 4); // USB_CMD_GET_ALL_SPACE — заставляет драйвер прочитать ФС
    seL4_MessageInfo_t r = seL4_Call(usb_ep, seL4_MessageInfo_new(0, 0, 0, 1));
    if (seL4_MessageInfo_get_length(r) < 1) return 0;
    seL4_Word mask = seL4_GetMR(0);
    int alive = 0;
    for (int i = 0; i < USB_MAX_DEVICES; i++) {
        if (!(mask & (1u << i))) continue;
        int base = 1 + i * 6;
        uint64_t total = seL4_GetMR(base + 4);
        if (total == 0) continue; // смонтирован, но ФС не отвечает — не считаем
        alive++;
        if (name_out && name_out[0] == '\0') {
            seL4_Word *words = (seL4_Word*)name_out;
            for (int w = 0; w < 4; w++) words[w] = seL4_GetMR(base + w);
            name_out[31] = '\0';
        }
    }
    return alive;
}

int main(int argc, char *argv[]) {
    SysClientEnv env;
    sys_client_init(env);
    g_timer_ep = env.timer_ep;

    int cycles = 20;
    int off_ms = 800;
    if (env.arg && *env.arg) {
        char buf[128]; my_strlcpy(buf, env.arg, sizeof(buf));
        char *cur = buf;
        char *t1 = next_token(&cur);
        char *t2 = next_token(&cur);
        if (t1 && *t1 && is_all_digits(t1)) { int v = simple_atoi(t1); if (v > 0) cycles = v; }
        if (t2 && *t2 && is_all_digits(t2)) { int v = simple_atoi(t2); if (v > 0) off_ms = v; }
    }

    // Сколько ждать возвращения тома после подачи питания. С запасом:
    // перечисление за хабом (сброс порта, Address Device, дескрипторы,
    // SCSI INQUIRY/READ CAPACITY, поиск раздела, exFAT) на живом железе
    // укладывается в секунды, но выбирать порог "впритык" значит ловить
    // ложные провалы вместо настоящих.
    constexpr int REMOUNT_TIMEOUT_MS = 20000;
    constexpr int POLL_STEP_MS = 200;

    sys_puts(0, "==========================================================\n");
    sys_puts(0, "hotplugtest — устойчивость монтирования к переподключениям\n");
    sys_puts(0, "циклов: "); putdec(cycles);
    sys_puts(0, ", питание снято на "); putdec(off_ms); sys_puts(0, "мс\n");
    sys_puts(0, "==========================================================\n");

    if (env.usb_storage_ep == 0) {
        sys_puts(0, "ОСТАНОВЛЕН — ОШИБКА: нет доступа к usb_driver (запускайте полным путём /sbin/tests/hotplugtest.elf).\n");
        sys_exit(env.root_ep); return 1;
    }

    char name[32] = {0};
    if (mounted_and_alive(env.usb_storage_ep, name) == 0) {
        sys_puts(0, "ОСТАНОВЛЕН — ОШИБКА: сейчас не смонтировано ни одного тома. Воткните флешку в хаб и повторите.\n");
        sys_exit(env.root_ep); return 1;
    }
    sys_puts(0, "исходный том: /mnt/"); sys_puts(0, name); sys_puts(0, "\n");

    int ok_cycles = 0;
    for (int c = 1; c <= cycles; c++) {
        sys_puts(0, "\nЦИКЛ "); putdec(c); sys_puts(0, "/"); putdec(cycles); sys_puts(0, "\n");

        seL4_SetMR(0, 5); // USB_CMD_PORT_POWER_CYCLE
        seL4_SetMR(1, (seL4_Word)off_ms);
        seL4_MessageInfo_t r = seL4_Call(env.usb_storage_ep, seL4_MessageInfo_new(0, 0, 0, 2));
        int st = (seL4_MessageInfo_get_length(r) >= 1) ? (int)seL4_GetMR(0) : -99;
        if (st != 0) {
            sys_puts(0, "ЦИКЛ "); putdec(c); sys_puts(0, " ОСТАНОВЛЕН — ОШИБКА: снять/подать питание не удалось, код ");
            putdec(st);
            if (st == -1) sys_puts(0, " (за хабом нет накопителя — предыдущий цикл его потерял)");
            else if (st == -2) sys_puts(0, " (хаб не принял CLEAR_FEATURE PORT_POWER)");
            else if (st == -3) sys_puts(0, " (хаб не принял SET_FEATURE PORT_POWER — порт остался обесточен!)");
            sys_puts(0, "\n");
            break;
        }

        // Цикл ограничен И временем, И числом итераций. Только временем
        // — недостаточно: в прогоне 2026-09-06 он однажды крутился 71
        // секунду при лимите в 20 (часы вернули значение, из-за которого
        // разность ушла в отрицательные числа, и условие снова стало
        // истинным). Счётчик итераций такой сценарий закрывает наглухо.
        seL4_Word t0 = now_ms();
        int alive = 0;
        const int MAX_POLLS = REMOUNT_TIMEOUT_MS / POLL_STEP_MS + 2;
        for (int pollc = 0; pollc < MAX_POLLS; pollc++) {
            if ((int)(now_ms() - t0) >= REMOUNT_TIMEOUT_MS) break;
            sleep_ms(POLL_STEP_MS);
            alive = mounted_and_alive(env.usb_storage_ep, nullptr);
            if (alive > 0) break;
        }
        int took = (int)(now_ms() - t0);
        if (took < 0 || took > REMOUNT_TIMEOUT_MS * 4) took = REMOUNT_TIMEOUT_MS; // часы соврали — не печатать бессмыслицу

        if (alive > 0) {
            ok_cycles++;
            sys_puts(0, "ЦИКЛ "); putdec(c); sys_puts(0, " ЗАВЕРШЁН — том вернулся за "); putdec(took); sys_puts(0, "мс\n");
        } else {
            sys_puts(0, "ЦИКЛ "); putdec(c);
            sys_puts(0, " ОСТАНОВЛЕН — ОШИБКА: том НЕ вернулся за "); putdec(took);
            sys_puts(0, "мс. Смотрите лог драйвера выше — на каком шаге перечисления встали.\n");
            break;
        }
    }

    sys_puts(0, "\n==========================================================\n");
    sys_puts(0, "ИТОГ: успешных циклов "); putdec(ok_cycles); sys_puts(0, " из "); putdec(cycles);
    if (ok_cycles == cycles) sys_puts(0, " — монтирование выдержало весь прогон.\n");
    else sys_puts(0, " — прогон прерван на первом же отказе (см. выше).\n");
    sys_puts(0, "==========================================================\n");

    sys_exit(env.root_ep);
    return 0;
}
