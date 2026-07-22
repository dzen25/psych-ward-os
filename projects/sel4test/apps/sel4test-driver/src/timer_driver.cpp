#include <sel4/sel4.h>
#include "h/common.h"
#include "h/platform.h"

static inline seL4_IPCBuffer* get_local_ipc() {
    seL4_Word tls_addr;
    // Добавлена буква 'ro'. crt0 не мог его стереть!
    asm volatile("mrs %0, tpidrro_el0" : "=r"(tls_addr));
    return (seL4_IPCBuffer*)(tls_addr - 1024);
}

void __assert_fail(const char *assertion, const char *file, int line, const char *function) { while(1); }

// ARM generic timer (CNTVCT_EL0/CNTFRQ_EL0) — читается напрямую из EL0,
// без MMIO/device-frame (заменяет PL031, см. ROADMAP.md Фаза 3.1; подробности
// про EXPORT_*_USER этой сборки ядра — см. hw_timer.cpp).
static inline uint64_t read_cntvct() {
    uint64_t val;
    asm volatile("mrs %0, cntvct_el0" : "=r"(val));
    return val;
}

static inline uint64_t read_cntfrq() {
    uint64_t val;
    asm volatile("mrs %0, cntfrq_el0" : "=r"(val));
    return val;
}

// Физический таймер (CNTP_CTL_EL0/CNTP_CVAL_EL0, PPI 30 non-secure) — Фаза
// 4.5, см. platform.h/PLAT_TIMER_IRQ и easy-settings.cmake/
// KernelArmExportPTMRUser. Даёт настоящий IRQ по дедлайну вместо busy-poll
// SYS_GET_TIME (см. sys_sleep() в shell.cpp — теперь блокирующий IPC-вызов,
// а не клиентский цикл). Условие СNTP (CNTPCT_EL0 >= CNTP_CVAL_EL0) —
// сравнение с ФИЗИЧЕСКИМ счётчиком, а не CNTVCT; но ARM_HYP выключен (нет
// гипервизора, который выставлял бы CNTVOFF != 0), поэтому CNTVCT_EL0 ==
// CNTPCT_EL0 здесь всегда — используем уже читаемый read_cntvct() вместо
// того, чтобы просить отдельный EXPORT_PCNT_USER только ради дедлайна.
static inline void write_cntp_cval(uint64_t val) {
    asm volatile("msr cntp_cval_el0, %0" :: "r"(val));
}
static inline void write_cntp_ctl(uint64_t val) {
    asm volatile("msr cntp_ctl_el0, %0" :: "r"(val));
}
constexpr uint64_t CNTP_CTL_ENABLE = 1u; // бит 0: включить; бит 1 (IMASK) оставляем 0 — не маскируем

// Фаза 4.5 (см. ROADMAP.md/net_driver.cpp): физический таймер — ЕДИНСТВЕННЫЙ
// аппаратный ресурс (один компаратор), а нужд у нас теперь две — разовый
// sleep(ms) по запросу шелла И периодический heartbeat для net_driver
// (DHCP/ARP/ping/link-таймауты, которые сами по себе ни с каким IRQ не
// связаны). Мультиплексируем обе поверх одного CNTP_CVAL по принципу
// "взводим на БЛИЖАЙШИЙ из известных дедлайнов" — простейшая версия
// software timer wheel (тот же принцип, что hrtimer в Linux, просто на
// два таймера, а не на N).
static void rearm_timer(bool pending_sleep, uint64_t sleep_deadline,
                        bool heartbeat_enabled, uint64_t next_heartbeat_deadline) {
    bool have_deadline = false;
    uint64_t next = 0;
    if (pending_sleep) { next = sleep_deadline; have_deadline = true; }
    if (heartbeat_enabled && (!have_deadline || next_heartbeat_deadline < next)) {
        next = next_heartbeat_deadline;
        have_deadline = true;
    }
    if (have_deadline) {
        write_cntp_cval(next);
        write_cntp_ctl(CNTP_CTL_ENABLE);
    } else {
        write_cntp_ctl(0); // нечего ждать — не держим таймер взведённым попусту
    }
}

// Термодатчик AVS RO thermal (см. platform.h) — единственный MMIO-регистр,
// который этому процессу реально нужен, поэтому не заводим под него
// отдельный драйвер: та же экономия, что и с ARM generic timer выше —
// один регистр статуса, ни DMA, ни IRQ, ни инициализации.
static bool read_cpu_temp_mC(int32_t *out_mC) {
    volatile uint32_t *status = (volatile uint32_t*)(PLAT_AVS_VADDR + AVS_RO_TEMP_STATUS_OFFSET);
    uint32_t val = *status;
    if (!(val & AVS_RO_TEMP_STATUS_VALID_MSK)) return false;
    int32_t raw = (int32_t)(val & AVS_RO_TEMP_STATUS_DATA_MSK);
    *out_mC = AVS_TEMP_SLOPE_MC * raw + AVS_TEMP_OFFSET_MC;
    return true;
}

// ========================================================
// VideoCore mailbox (Фаза 4.6, расследование DVFS — см. ROADMAP.md).
// Диагностика по команде шелла "mboxprobe" (SYS_MBOX_PROBE), не часть
// обычного цикла таймера. Перенос sys_puts/sys_puthex32 — по образцу
// wifi_driver.cpp (там же дублируются те же две функции, не общий
// заголовок — драйверы этого проекта намеренно не шарят рантайм-хелперы
// друг с другом, только регистровые константы в platform.h).
// ========================================================
static void sys_puts(seL4_CPtr console_ep, const char *str) {
    seL4_IPCBuffer *ipc = get_local_ipc();
    int len = 0; while (str[len]) len++;
    int offset = 0;
    while (offset < len) {
        int chunk = len - offset;
        if (chunk > 100) chunk = 100;
        ipc->msg[0] = 8; // SYS_PUTS
        for (int i = 0; i < chunk; i++) ipc->msg[i + 1] = str[offset + i];
        seL4_Call(console_ep, seL4_MessageInfo_new(0, 0, 0, chunk + 1));
        offset += chunk;
    }
}

static void sys_puthex32(seL4_CPtr console_ep, const char* label, uint32_t val) {
    sys_puts(console_ep, label);
    char buf[11];
    buf[0] = '0'; buf[1] = 'x';
    for (int i = 0; i < 8; i++) buf[2 + i] = "0123456789abcdef"[(val >> ((7 - i) * 4)) & 0xF];
    buf[10] = 0;
    sys_puts(console_ep, buf);
    sys_puts(console_ep, "\n");
}

static volatile uint32_t *g_mbox_buf = nullptr;   // PLAT_MBOX_BUF_VADDR, некэшируемый, 4KB
static uint32_t g_mbox_buf_paddr = 0;

static inline uint32_t mbox_reg_read(uintptr_t off) { return *(volatile uint32_t*)(PLAT_MBOX_VADDR + off); }
static inline void mbox_reg_write(uintptr_t off, uint32_t val) { *(volatile uint32_t*)(PLAT_MBOX_VADDR + off) = val; }

// Один запрос-ответ по property-channel, честный wall-clock таймаут (тот же
// приём, что и EMMC/Wi-Fi busy-wait в blk_driver.cpp/wifi_driver.cpp — см.
// ROADMAP.md 4.5 про честные таймауты вместо счётчика итераций; здесь
// оставлен счётчик итераций, т.к. это разовый диагностический пробник, а
// не рабочий путь с требованиями к времени).
static bool mbox_call(uint32_t bus_addr, int timeout_iters) {
    int i = 0;
    while (mbox_reg_read(MBOX_STATUS_OFFSET) & MBOX_STATUS_FULL) {
        if (++i > timeout_iters) return false;
    }
    mbox_reg_write(MBOX_WRITE_OFFSET, bus_addr);

    i = 0;
    while (true) {
        while (mbox_reg_read(MBOX_STATUS_OFFSET) & MBOX_STATUS_EMPTY) {
            if (++i > timeout_iters) return false;
        }
        uint32_t resp = mbox_reg_read(MBOX_READ_OFFSET);
        if ((resp & 0xF) == MBOX_CHANNEL_PROP) {
            return (resp & ~0xFu) == (bus_addr & ~0xFu);
        }
        // Ответ на чужой канал — не наш, продолжаем ждать (тот же класс
        // ситуации, что и рассинхронизация reqid в wifi_driver.cpp
        // wifi_ioctl() — не считаем это фатальным сразу).
        if (++i > timeout_iters) return false;
    }
}

// Не известно заранее (см. ROADMAP.md 4.6), какая трансляция ARM
// physical -> VC bus address нужна на этой связке прошивки/loader'а —
// предыдущая попытка (смена тактовой PL011) не отвечала вообще, и не ясно,
// был ли неправильно сформирован именно адрес, или mailbox мёртв целиком.
// Пробуем по очереди самые распространённые варианты (см. классические
// bare-metal Raspberry Pi мануалы) одним и тем же безобидным тегом
// (GET_FIRMWARE_REVISION — не зависит от clock-специфики предыдущей
// попытки) — это и есть "timeboxed" эксперимент из плана сессии.
struct MboxAddrVariant { uint32_t or_mask; const char *name; };
static const MboxAddrVariant kMboxAddrVariants[] = {
    { 0x00000000u, "raw ARM physical (без bus-смещения)" },
    { 0xC0000000u, "L2-cached alias (+0xC0000000)" },
    { 0x40000000u, "L1-only alias (+0x40000000)" },
    { 0x80000000u, "L2-coherent alias (+0x80000000)" },
};
constexpr int kMboxAddrVariantCount = sizeof(kMboxAddrVariants) / sizeof(kMboxAddrVariants[0]);

// Возвращает индекс сработавшего варианта (см. kMboxAddrVariants) или -1,
// если mailbox не ответил ни на один из них — тогда, по плану сессии,
// 4.6 фиксируется как принятая деградация, а не откладывается на "попробовать
// ещё вариант адреса" до бесконечности.
static int mbox_probe(seL4_CPtr console_ep) {
    if (g_mbox_buf == nullptr || g_mbox_buf_paddr == 0) {
        sys_puts(console_ep, "[MBOX] буфер не замаплен (BOOT_MBOX_BUF_PADDR == 0) — пропускаем\n");
        return -1;
    }

    for (int v = 0; v < kMboxAddrVariantCount; v++) {
        // Формируем property-tag запрос заново перед КАЖДОЙ попыткой — GPU
        // мог что-то дописать в буфер при предыдущей неудачной попытке.
        g_mbox_buf[0] = 7 * 4;                          // общий размер буфера в байтах (7 слов)
        g_mbox_buf[1] = MBOX_CODE_REQUEST;
        g_mbox_buf[2] = MBOX_TAG_GET_FIRMWARE_REVISION; // tag id
        g_mbox_buf[3] = 4;                               // размер буфера значения (байт)
        g_mbox_buf[4] = 0;                                // request/response indicator (0 = запрос)
        g_mbox_buf[5] = 0;                                // значение (заполнит VC)
        g_mbox_buf[6] = MBOX_TAG_LAST;

        uint32_t bus_addr = (g_mbox_buf_paddr & ~0xFu) | kMboxAddrVariants[v].or_mask | MBOX_CHANNEL_PROP;
        // Низкие 4 бита канала не должны портить старшие адресные биты —
        // маска варианта задаёт только ВЕРХНИЕ биты, поэтому OR, а не сложение.

        sys_puts(console_ep, "[MBOX] пробуем: ");
        sys_puts(console_ep, kMboxAddrVariants[v].name);
        sys_puts(console_ep, " ...\n");

        if (mbox_call(bus_addr, 2000000) && g_mbox_buf[1] == MBOX_CODE_RESPONSE_SUCCESS) {
            sys_puts(console_ep, "[MBOX] ОТВЕТИЛ: ");
            sys_puts(console_ep, kMboxAddrVariants[v].name);
            sys_puts(console_ep, "\n");
            sys_puthex32(console_ep, "[MBOX] firmware revision = ", g_mbox_buf[5]);
            return v;
        }
    }
    sys_puts(console_ep, "[MBOX] не ответил ни на один вариант адреса — см. ROADMAP.md 4.6 (возможная деградация)\n");
    return -1;
}

int main(int argc, char *argv[]) {
    // 2. Достаем настоящий адрес буфера
    seL4_IPCBuffer *ipc = get_local_ipc();

    // 3. Отдаем его libsel4 (теперь её TLS инициализирован, и она сохранит его куда надо)
    seL4_SetIPCBuffer(ipc);

    // 5. Теперь безопасно получаем Capability-индексы
    seL4_CPtr root_ep    = ipc->msg[BOOT_ROOT_EP];
    seL4_CPtr my_ep      = ipc->msg[BOOT_TIMER_EP];
    seL4_CPtr console_ep = ipc->msg[BOOT_CONSOLE_EP];
    seL4_CPtr irq_ep     = ipc->msg[BOOT_IRQ_EP]; // Фаза 4.5: настоящий IRQHandler физического таймера, не общий ни с кем
    seL4_CPtr heartbeat_ntfn = ipc->msg[BOOT_HEARTBEAT_NTFN_CAP]; // Фаза 4.5: badged-капа на net_driver, см. common.h

    if (my_ep == 0) {
        __assert_fail("FATAL: Null Capability #0 Detected!", __FILE__, __LINE__, __func__);
    }

    // VideoCore mailbox (Фаза 4.6) — буфер уже замаплен main.cpp'ом при
    // спавне (см. PLAT_MBOX_BUF_VADDR/mbox_buf_frame_param), физический
    // адрес пришёл через boot IPC, т.к. дочерний процесс не может узнать
    // его сам (нет капабилити на собственный frame, см. main.cpp).
    g_mbox_buf = (volatile uint32_t*)PLAT_MBOX_BUF_VADDR;
    g_mbox_buf_paddr = (uint32_t)ipc->msg[BOOT_MBOX_BUF_PADDR];

    const uint64_t cntfrq = read_cntfrq();
    // Момент запуска драйвера — точка отсчета аптайма. Не корректируется
    // NTP-смещением: аптайм должен оставаться монотонным независимо от
    // коррекции показаний часов.
    const uint64_t boot_tick = read_cntvct();

    // Коррекция смещения (сек.) между аптаймом и NTP-сервером, применяется
    // только к SYS_GET_TIME. Выставляется командой шелла `ntp` через
    // net_driver (см. SYS_SET_TIME_OFFSET ниже).
    seL4_Int64 ntp_offset_seconds = 0;

    seL4_SetMR(0, SYS_DRIVER_READY);
    seL4_Call(root_ep, seL4_MessageInfo_new(0, 0, 0, 1));

    // Отложенный reply на SYS_SLEEP_MS (Фаза 4.5, см. ROADMAP.md) — тот же
    // приём, что uart_driver.cpp уже использует для SYS_READ: сохраняем
    // reply-cap вызывающего (SELF_CNODE_SLOT, common.h) и отвечаем из ветки
    // IRQ ниже, когда физический таймер реально сработает. Только ОДИН
    // отложенный sleep одновременно — аппаратный дедлайн-регистр один на
    // процесс, второй одновременный запрос получает отказ сразу (см. ниже).
    constexpr seL4_Word SLEEP_REPLY_SLOT = 12;
    bool pending_sleep = false;
    uint64_t sleep_deadline = 0;

    // Периодический heartbeat для net_driver (Фаза 4.5, см. rearm_timer()
    // выше) — подписка одна на весь процесс (net_driver — единственный
    // подписчик, регистрируется один раз при своём старте, см.
    // SYS_TIMER_HEARTBEAT_SUBSCRIBE ниже).
    bool heartbeat_enabled = false;
    uint64_t heartbeat_period_ticks = 0;
    uint64_t next_heartbeat_deadline = 0;

    // Главный цикл обработки IPC-запросов
    while(1) {
        seL4_Word badge = 0;
        seL4_MessageInfo_t info = seL4_Recv(my_ep, &badge);

        if (badge == 1) {
            // IRQ физического таймера — снимаем условие СРАЗУ (ENABLE=0
            // гасит выход таймера независимо от ISTATUS), потом Ack: этот
            // IRQ не общий ни с чем (в отличие от IRQ 158 EMMC2/Wi-Fi), и
            // Ack'аем сами, в том же треде — никакого relay/старвации по
            // priority тут в принципе быть не может (см. blk_driver.cpp
            // для контраста, где общая линия потребовала SYS_MMC_IRQ_ACK).
            write_cntp_ctl(0);
            seL4_IRQHandler_Ack(irq_ep);
            if (LOG_TIMER) sys_puts(console_ep, "[TIMER] IRQ физического таймера пришёл\n");

            // Один компаратор, два независимых дедлайна (см. rearm_timer()
            // выше) — проверяем КАЖДЫЙ отдельно по факту истечения, а не
            // просто "раз IRQ пришёл, значит это sleep": если heartbeat был
            // взведён раньше sleep'а, этот конкретный firing вполне может
            // быть только за heartbeat, а sleep ещё не наступил.
            uint64_t now = read_cntvct();
            if (pending_sleep && now >= sleep_deadline) {
                seL4_Send(SLEEP_REPLY_SLOT, seL4_MessageInfo_new(0, 0, 0, 0));
                seL4_CNode_Delete(SELF_CNODE_SLOT, SLEEP_REPLY_SLOT, 8); // depth=8, см. урок из uart_driver.cpp
                pending_sleep = false;
            }
            if (heartbeat_enabled && now >= next_heartbeat_deadline) {
                seL4_Signal(heartbeat_ntfn);
                next_heartbeat_deadline = now + heartbeat_period_ticks;
            }
            rearm_timer(pending_sleep, sleep_deadline, heartbeat_enabled, next_heartbeat_deadline);
            continue;
        }

        uint64_t uptime_ms = ((read_cntvct() - boot_tick) * 1000) / cntfrq;

        // Обработка запросов от процессов (SYS_GET_TIME / SYS_GET_UPTIME)
        seL4_Word sys = seL4_GetMR(0);
        if (sys == 3) { // SYS_GET_TIME: мс "с эпохи" (аптайм + NTP-коррекция)
            seL4_Int64 corrected = (seL4_Int64)uptime_ms + ntp_offset_seconds * 1000;
            seL4_SetMR(0, (seL4_Word)corrected);
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
        } else if (sys == 4) { // SYS_GET_UPTIME: мс с момента запуска timer_driver
            seL4_SetMR(0, (seL4_Word)uptime_ms);
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
        } else if (sys == 5) { // SYS_SET_TIME_OFFSET: применить офсет от NTP-клиента (net_driver)
            ntp_offset_seconds = (seL4_Int64)seL4_GetMR(1);
            seL4_SetMR(0, 0);
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
        } else if (sys == 6) { // SYS_GET_TEMP: температура кристалла (см. AVS RO thermal выше)
            int32_t temp_mC = 0;
            bool valid = read_cpu_temp_mC(&temp_mC);
            seL4_SetMR(0, valid ? 0 : 1); // 0 = ok, 1 = датчик еще не выдал валидное показание
            seL4_SetMR(1, (seL4_Word)(int64_t)temp_mC);
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 2));
        } else if (sys == 7) { // SYS_MBOX_PROBE: диагностика VideoCore mailbox (Фаза 4.6, см. platform.h)
            int variant = mbox_probe(console_ep);
            seL4_SetMR(0, variant); // -1 = не ответил ни на один вариант адреса
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
        } else if (sys == 8) { // SYS_SLEEP_MS: событийный sleep (Фаза 4.5, см. shell.cpp sys_sleep())
            if (pending_sleep) {
                // Уже кто-то спит — аппаратный дедлайн один на процесс (см.
                // комментарий у pending_sleep выше). Отказ сразу, не зависаем.
                seL4_SetMR(0, (seL4_Word)-1);
                seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
            } else {
                seL4_Word ms = seL4_GetMR(1);
                sleep_deadline = read_cntvct() + ((uint64_t)ms * cntfrq) / 1000;
                pending_sleep = true;
                // rearm_timer() сам разберётся, что взводить — этот дедлайн
                // или уже тикающий heartbeat, смотря что ближе (см. функцию
                // выше). ВАЖНО: SaveCaller — ПЕРВЫМ делом, до любых чужих
                // IPC-вызовов (даже sys_puts()!). Неявное право на reply
                // исходному вызывающему держится только до СЛЕДУЮЩЕГО Recv
                // этого треда — а seL4_Call внутри sys_puts() как раз делает
                // Recv (ждёт ответ uart_driver'а). Если бы печать была раньше
                // SaveCaller, она сама стёрла бы ещё не сохранённое право на
                // reply, и шелл завис бы навсегда — ровно того же рода баг,
                // что уже ловили в Блоке A/B (см. depth=8 у SaveCaller ниже).
                seL4_CNode_SaveCaller(SELF_CNODE_SLOT, SLEEP_REPLY_SLOT, 8); // depth=8, см. урок из uart_driver.cpp
                rearm_timer(pending_sleep, sleep_deadline, heartbeat_enabled, next_heartbeat_deadline);
                if (LOG_TIMER) sys_puts(console_ep, "[TIMER] sleep: взвели физический таймер, ждём IRQ...\n");
                // Reply НЕ отправляем — см. ветку badge==1 выше.
            }
        } else if (sys == 9) { // SYS_TIMER_HEARTBEAT_SUBSCRIBE: разовая регистрация net_driver'а (Фаза 4.5)
            // MR1 = период в мс. Отвечаем сразу — это просто регистрация,
            // не ожидание (в отличие от SYS_SLEEP_MS выше).
            seL4_Word period_ms = seL4_GetMR(1);
            heartbeat_period_ticks = ((uint64_t)period_ms * cntfrq) / 1000;
            next_heartbeat_deadline = read_cntvct() + heartbeat_period_ticks;
            heartbeat_enabled = (heartbeat_ntfn != 0);
            rearm_timer(pending_sleep, sleep_deadline, heartbeat_enabled, next_heartbeat_deadline);
            seL4_SetMR(0, heartbeat_enabled ? 0 : (seL4_Word)-1);
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
        } else {
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 0));
        }
    }

    return 0;
}
