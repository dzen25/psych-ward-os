// =====================================================================
// wifi_driver.cpp — Фаза 4, Милстоун 4.1: SDIO host bring-up (см. ROADMAP.md).
//
// Реализовано ТОЛЬКО: инициализация SDIO-хост-контроллера
// (brcm,bcm2835-sdhci, PLAT_WIFI_SDIO_PADDR — НЕ тот же физический блок, что
// EMMC2/blk_driver.cpp) + сырые команды CMD5 (SDIO_SEND_OP_COND) и CMD52
// (SDIO_RW_DIRECT), достаточные чтобы прочитать F0 CCCR-регистр чипа
// BCM43455 и доказать, что host+транзакция работают на живом железе.
//
// НЕ реализовано (сознательно, отдельные будущие милстоуны — см.
// ROADMAP.md Фаза 4.2-4.4): backplane/core enumeration, загрузка
// прошивки/NVRAM, sdpcm-протокол, CDC/BDC IOCTL, подключение к точке
// доступа, data-path в net_driver.cpp. Эталон для всего перечисленного —
// /home/nikita/workspace_nofing/common/drivers/net/wireless/broadcom/brcm80211/brcmfmac/
// (sdio.c/bcmsdh.c/chip.c/bcdc.c/fwil.c/fweh.c).
//
// Регистровая карта SDHCI (CMDTM/ARG1/RESP0-3/DATA/STATUS/CONTROL0/
// CONTROL1/INTERRUPT/CAP0) — те же смещения EMMC_*, что и у EMMC2
// (blk_driver.cpp): "brcm,bcm2835-sdhci" — тот же стандартный SDHCI
// Simplified Spec layout, просто другой физический адрес и другой набор
// команд (SDIO, не SD-memory). Низкоуровневые хелперы ниже — прямой перенос
// emmc_send_cmd/emmc_wait_*/emmc_set_clock_divider из blk_driver.cpp.
// =====================================================================

#include <sel4/sel4.h>
#include "h/common.h"
#include "h/platform.h"

static inline seL4_IPCBuffer* get_local_ipc() {
    seL4_Word tls_addr;
    asm volatile("mrs %0, tpidrro_el0" : "=r"(tls_addr));
    return (seL4_IPCBuffer*)(tls_addr - 1024);
}

void __assert_fail(const char *assertion, const char *file, int line, const char *function) { while(1); }

// ARM generic timer, читается напрямую EL0-инструкцией mrs (см. hw_timer.cpp/
// timer_driver.cpp — тот же приём). Нужен для НАСТОЯЩЕГО, привязанного к
// реальному времени таймаута ожидания ALP/HT clock у чипа (см.
// wifi_request_alp_clock/wifi_request_ht_clock ниже) — "подожди N итераций
// seL4_Yield()" не даёт никакой гарантии реального времени (зависит от
// планировщика/числа других runnable-процессов), а эталонный
// brcmf_sdio_htclk() ждёт HT честный 1 секунду (PMU_MAX_TRANSITION_DLY =
// 1000000 микросекунд) — именно этого не хватило в первой версии (HT так и
// не появился за фиксированное число yield-итераций, которое, видимо,
// соответствовало заметно меньше секунды реального времени).
static inline uint64_t wifi_read_cntvct() {
    uint64_t val;
    asm volatile("mrs %0, cntvct_el0" : "=r"(val));
    return val;
}

static inline uint64_t wifi_read_cntfrq() {
    uint64_t val;
    asm volatile("mrs %0, cntfrq_el0" : "=r"(val));
    return val;
}

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

// Диагностика: печатает сырые байты буфера (hex, через пробел) — нужно один
// раз честно проверить, что реально уезжает на шину, а не рассуждать по
// коду (BCDC-заголовок/sdpcm-заголовок уже дважды перепроверены построчно
// против эталона и оба раза оказались верны — следующий шаг это увидеть
// байты своими глазами).
static void sys_puthexbuf(seL4_CPtr console_ep, const char* label, const uint8_t* buf, uint32_t len) {
    sys_puts(console_ep, label);
    for (uint32_t i = 0; i < len; i++) {
        char hex[3];
        hex[0] = "0123456789abcdef"[(buf[i] >> 4) & 0xF];
        hex[1] = "0123456789abcdef"[buf[i] & 0xF];
        hex[2] = 0;
        sys_puts(console_ep, hex);
        sys_puts(console_ep, " ");
    }
    sys_puts(console_ep, "\n");
}

// ========================================================
// АППАРАТНЫЙ УРОВЕНЬ: SDIO-хост (brcm,bcm2835-sdhci), PIO/polling.
// Регистровая карта — h/platform.h (EMMC_* переиспользуются, см. шапку файла).
// ========================================================
static volatile uint32_t* g_wifi_base = nullptr;

static inline volatile uint32_t* wifi_reg(uintptr_t offset) {
    return (volatile uint32_t*)((uintptr_t)g_wifi_base + offset);
}

// Честный wall-clock таймаут (ARM generic timer) вместо счётчика yield-
// итераций — тот же урок, что и с ожиданием ALP/HT-клока (см.
// wifi_wait_chipclkcsr): "подожди N итераций seL4_Yield()" не даёт никакой
// гарантии реального времени. Пойман на живом железе: CMD_INHIBIT/DAT_INHIBIT
// не снимался за миллион yield-итераций (даже с тремя повторами) ровно после
// переключения клока чипа на HT — похоже, переходу клока просто нужно
// немного реального времени на settle, а не "заедание" контроллера.
// После настоящей аппаратной ошибки ИЛИ застрявшего CMD_INHIBIT/DAT_INHIBIT
// SDHCI-контроллер, как правило, требует явного software-сброса командной/
// DAT-линии (SRST_CMD/SRST_DATA, CONTROL1) — иначе его внутренний автомат
// остаётся заклиненным и СЛЕДУЮЩАЯ команда виснет, даже будучи сама по себе
// полностью корректной. Пойман на живом железе дважды: (1) DAT_READY не
// снимался после переключения клока чипа на HT, (2) после best-effort-ошибки
// на watermark-регистре первый же CMD53 (F2 GET_VERSION) вис на CMD_INHIBIT.
static void wifi_reset_cmd_data_lines() {
    uint32_t c1 = *wifi_reg(EMMC_CONTROL1_OFFSET) | EMMC_C1_SRST_CMD | EMMC_C1_SRST_DATA;
    *wifi_reg(EMMC_CONTROL1_OFFSET) = c1;
    uint64_t freq = wifi_read_cntfrq();
    uint64_t start = wifi_read_cntvct();
    uint64_t timeout_ticks = (freq * 100000) / 1000000ull; // 100мс — сбросы самоочищаются быстро
    while (*wifi_reg(EMMC_CONTROL1_OFFSET) & (EMMC_C1_SRST_CMD | EMMC_C1_SRST_DATA)) {
        if (wifi_read_cntvct() - start >= timeout_ticks) break;
        seL4_Yield();
    }
}

// Честный wall-clock таймаут (ARM generic timer) вместо счётчика yield-
// итераций — тот же урок, что и с ожиданием ALP/HT-клока (см.
// wifi_wait_chipclkcsr): "подожди N итераций seL4_Yield()" не даёт никакой
// гарантии реального времени. Если статус не снялся за отведённое время —
// одна попытка software-сброса линии (см. wifi_reset_cmd_data_lines) и ещё
// один полный таймаут, ПРЕЖДЕ чем сдаться — застрявший бит сам по себе
// обычно не самоочищается без явного сброса (см. её комментарий).
static bool wifi_wait_status_clear(uint32_t mask, uint32_t timeout_us) {
    for (int attempt = 0; attempt < 2; attempt++) {
        uint64_t freq = wifi_read_cntfrq();
        uint64_t start = wifi_read_cntvct();
        uint64_t timeout_ticks = (freq * timeout_us) / 1000000ull;
        bool ok = true;
        while (*wifi_reg(EMMC_STATUS_OFFSET) & mask) {
            if (wifi_read_cntvct() - start >= timeout_ticks) { ok = false; break; }
            seL4_Yield();
        }
        if (ok) return true;
        if (attempt == 0) wifi_reset_cmd_data_lines();
    }
    return false;
}

static bool wifi_wait_cmd_ready() {
    return wifi_wait_status_clear(EMMC_STATUS_CMD_INHIBIT, 100000); // 100мс
}

// Нужен для CMD53 (есть фаза данных) — как emmc_wait_dat_ready() в blk_driver.cpp.
static bool wifi_wait_dat_ready() {
    return wifi_wait_status_clear(EMMC_STATUS_DAT_INHIBIT, 100000); // 100мс
}

// console_ep (необязательный, по умолчанию 0 = молчать) — раньше false из
// этой функции означало И "настоящую аппаратную ошибку" (биты EMMC_INT_
// ERROR_MASK), И "обычный таймаут" одним и тем же кодом — из лога было
// невозможно понять, что из двух произошло. Передавайте console_ep там, где
// нужна точная диагностика (см. backplane_write_chunk) — распечатает сырые
// INTERRUPT/STATUS в момент отказа.
static bool wifi_wait_irpt_bit(uint32_t bit, seL4_CPtr console_ep = 0) {
    uint32_t timeout = 1000000;
    while (true) {
        uint32_t irpt = *wifi_reg(EMMC_INTERRUPT_OFFSET);
        if (irpt & EMMC_INT_ERROR_MASK) {
            *wifi_reg(EMMC_INTERRUPT_OFFSET) = irpt;
            if (console_ep) {
                sys_puthex32(console_ep, "[WIFI][FW] ОШИБКА — IRPT raw = ", irpt);
                sys_puthex32(console_ep, "[WIFI][FW] STATUS raw        = ", *wifi_reg(EMMC_STATUS_OFFSET));
            }
            // Настоящая ошибка (не обычный таймаут) оставляет командную/DAT-
            // линию заклиненной для СЛЕДУЮЩЕЙ команды — сбрасываем сразу,
            // чтобы вызывающий код (даже если он терпит эту ошибку и
            // продолжает, как best-effort watermark в Милстоуне 4.3) не
            // унаследовал испорченное состояние контроллера.
            wifi_reset_cmd_data_lines();
            return false;
        }
        if (irpt & bit) { *wifi_reg(EMMC_INTERRUPT_OFFSET) = bit; return true; }
        if (--timeout == 0) {
            if (console_ep) {
                sys_puthex32(console_ep, "[WIFI][FW] ТАЙМАУТ — последний IRPT raw = ", irpt);
                sys_puthex32(console_ep, "[WIFI][FW] STATUS raw                    = ", *wifi_reg(EMMC_STATUS_OFFSET));
            }
            return false;
        }
        seL4_Yield();
    }
}

// console_ep (необязательный, по умолчанию 0 = молчать) — та же диагностика,
// что у wifi_wait_irpt_bit: печатает сырые IRPT/STATUS при отказе, чтобы
// отличить настоящую ошибку шины от банального таймаута CMD_INHIBIT.
static bool sdio_send_cmd(uint32_t cmd_flags, uint32_t index, uint32_t arg, seL4_CPtr console_ep = 0) {
    if (!wifi_wait_cmd_ready()) {
        if (console_ep) sys_puts(console_ep, "[WIFI][SDIO] ТАЙМАУТ CMD_INHIBIT перед командой\n");
        return false;
    }
    *wifi_reg(EMMC_INTERRUPT_OFFSET) = 0xFFFFFFFF;
    *wifi_reg(EMMC_ARG1_OFFSET) = arg;
    *wifi_reg(EMMC_CMDTM_OFFSET) = (index << EMMC_CMD_INDEX_SHIFT) | cmd_flags;
    return wifi_wait_irpt_bit(EMMC_INT_CMD_DONE, console_ep);
}

static void sdio_set_clock_divider(uint32_t divisor) {
    uint32_t c1 = *wifi_reg(EMMC_CONTROL1_OFFSET) & ~EMMC_C1_CLK_EN;
    *wifi_reg(EMMC_CONTROL1_OFFSET) = c1;

    c1 &= ~(0xFFu << EMMC_C1_CLK_FREQ_SHIFT);
    c1 |= (divisor & 0xFFu) << EMMC_C1_CLK_FREQ_SHIFT;
    *wifi_reg(EMMC_CONTROL1_OFFSET) = c1;

    uint32_t timeout = 1000000;
    while (!(*wifi_reg(EMMC_CONTROL1_OFFSET) & EMMC_C1_CLK_STABLE)) {
        if (--timeout == 0) break;
        seL4_Yield();
    }
    *wifi_reg(EMMC_CONTROL1_OFFSET) = *wifi_reg(EMMC_CONTROL1_OFFSET) | EMMC_C1_CLK_EN;
}

// Последние значения, прочитанные при пробе — отдаются шеллу по команде
// WIFI_CMD_PROBE_STATUS (см. main() ниже).
static bool g_probe_ok = false;
static uint32_t g_sdio_ocr = 0;      // ответ CMD5 (R4): OCR + число I/O-функций
static uint32_t g_sdio_rca = 0;      // ответ CMD3 (R6, старшие 16 бит) — адрес карты для CMD7
static uint32_t g_cccr_rev = 0;      // F0 CCCR offset 0x00, прочитан через CMD52

// Инициализация хоста + минимальная SDIO-транзакция (см. шапку файла —
// Милстоун 4.1). console_ep — только диагностика на живом железе, как и в
// blk_driver.cpp/net_driver.cpp на их первых этапах.
// Пошаговая диагностика (ВРЕМЕННО, см. LOG_BLK/LOG_NET в platform.h —
// тот же принцип): предыдущий живой прогон с циклом повтора CMD5 положил
// ВЕСЬ кернел (seL4 "halting... Kernel entry via Unknown (0)" — фатальный
// необрабатываемый halt, не обычный recoverable page fault пользовательского
// процесса — судя по всему, SError/внешний abort шины). Печатаем явный маркер
// перед КАЖДЫМ регистровым действием, чтобы при повторном падении в логе было
// видно, какой именно шаг стал последним. Уменьшили и агрессивность самого
// цикла CMD5 (было 1000 попыток почти без паузы; стало 20 попыток с
// yield-spin паузой между ними, как и в задержке стабилизации клока ниже).
static bool wifi_sdio_probe(void *vaddr, seL4_CPtr console_ep) {
    g_wifi_base = (volatile uint32_t*)vaddr;

    sys_puts(console_ep, "[WIFI][SDIO] step: SRST_HC...\n");
    *wifi_reg(EMMC_CONTROL1_OFFSET) = EMMC_C1_SRST_HC;
    uint32_t timeout = 1000000;
    while (*wifi_reg(EMMC_CONTROL1_OFFSET) & EMMC_C1_SRST_HC) {
        if (--timeout == 0) {
            sys_puts(console_ep, "[WIFI][SDIO] FAIL: software reset (SRST_HC) never cleared\n");
            return false;
        }
        seL4_Yield();
    }
    sys_puts(console_ep, "[WIFI][SDIO] step: SRST_HC cleared, CONTROL0 restore...\n");

    // Как и у EMMC2 (blk_driver.cpp), SRST_HC может гасить биты питания шины
    // в CONTROL0 — восстанавливаем защитно, даже если на этом контроллере
    // окажется не нужно (дёшево подстраховаться, см. план Милстоуна 4.1).
    *wifi_reg(EMMC_CONTROL0_OFFSET) = *wifi_reg(EMMC_CONTROL0_OFFSET) | EMMC_C0_PWR_ON | EMMC_C0_PWR_3V3;

    *wifi_reg(EMMC_IRPT_MASK_OFFSET) = EMMC_INT_ALL_EN;
    *wifi_reg(EMMC_IRPT_EN_OFFSET)   = 0; // polling, без IRQ — как и все остальные драйверы этой ОС
    *wifi_reg(EMMC_INTERRUPT_OFFSET) = 0xFFFFFFFF;

    sys_puts(console_ep, "[WIFI][SDIO] step: clock init...\n");
    // CAP0 на этом контроллере помечен квirком SDHCI_QUIRK_MISSING_CAPS в
    // эталонном Linux-драйвере (sdhci-iproc.c) — не заслуживает доверия,
    // поэтому делитель для идентификационной стадии берём тем же, что и у
    // EMMC2 (0x80, ~390kHz при базовых 100MHz) как отправную точку; если на
    // живом железе окажется, что база клока другая — эмпирически подобрать
    // заново, см. ROADMAP.md Милстоун 4.1, шаг 4 проверки.
    *wifi_reg(EMMC_CONTROL1_OFFSET) = EMMC_C1_CLK_INTLEN | EMMC_C1_TOUNIT_MAX;
    sdio_set_clock_divider(0x80);
    for (int i = 0; i < 100000; i++) seL4_Yield(); // см. blk_driver.cpp — CLK_STABLE иногда врёт раньше времени
    sys_puts(console_ep, "[WIFI][SDIO] step: clock done, starting CMD5...\n");

    // CMD5 (SDIO_SEND_OP_COND), arg=0: узнать OCR/число функций. R4 —
    // 48-битный ответ без CRC-проверки (см. SDIO_CMD_SEND_OP_COND в
    // platform.h) — поэтому EMMC_CMD_RSPNS_48 без EMMC_CMD_CRCCHK_EN,
    // ровно как ACMD41 в blk_driver.cpp. Первый ответ почти всегда придёт с
    // неустановленным битом "ready" (карта ещё договаривается по питанию) —
    // как и ACMD41, шлём CMD5 в цикле, пока SDIO_R4_READY не появится; без
    // этого следующий CMD52 может не пройти, потому что чип ещё занят своей
    // инициализацией. МЕНЬШЕ попыток и РЕАЛЬНАЯ пауза между ними (в отличие
    // от ACMD41 в blk_driver.cpp, который спокойно переносит частый повтор) —
    // этот SDIO-блок в эталонном Linux-драйвере помечен сразу тремя квirками
    // (BROKEN_CARD_DETECTION/MISSING_CAPS/NO_HISPD_BIT), так что не рискуем
    // повторять предыдущий, судя по всему, фатальный для шины сценарий.
    bool sdio_ready = false;
    for (int i = 0; i < 20 && !sdio_ready; i++) {
        sys_puts(console_ep, "[WIFI][SDIO] step: CMD5 attempt "); sys_puthex32(console_ep, "#", (uint32_t)i);
        if (!sdio_send_cmd(EMMC_CMD_RSPNS_48, SDIO_CMD_SEND_OP_COND, 0)) {
            sys_puts(console_ep, "[WIFI][SDIO] FAIL: CMD5 (SDIO_SEND_OP_COND) — карта не отвечает\n");
            return false;
        }
        g_sdio_ocr = *wifi_reg(EMMC_RESP0_OFFSET);
        if (g_sdio_ocr & SDIO_R4_READY) sdio_ready = true;
        else for (int y = 0; y < 50000; y++) seL4_Yield(); // реальная пауза, не мгновенный повтор
    }
    sys_puts(console_ep, "[WIFI][SDIO] step: CMD5 loop done\n");
    sys_puthex32(console_ep, "[WIFI][SDIO] CMD5 OCR = ", g_sdio_ocr);
    if (!sdio_ready) {
        sys_puts(console_ep, "[WIFI][SDIO] FAIL: CMD5 never set ready bit (карта не готова за 20 попыток)\n");
        return false;
    }

    // CMD52 работает только с картой в состоянии "transfer" — сверено с
    // эталонным Linux MMC core (drivers/mmc/core/sdio.c::mmc_sdio_init_card:
    // mmc_send_io_op_cond -> mmc_send_relative_addr -> mmc_select_card ->
    // только потом sdio_read_cccr/CMD52). Пропуск CMD3/CMD7 — то, чего не
    // хватало в первой версии этого файла и что, скорее всего, было
    // причиной падения CMD52 (карта ещё в состоянии ready/stand-by, не
    // transfer). CMD3/CMD7 — те же номера команд, что и у SD-memory карт в
    // blk_driver.cpp (EMMC_CMD_SEND_REL_ADDR/EMMC_CMD_SELECT_CARD), формат
    // ответа тоже совпадает (R6: RCA в старших 16 битах RESP0; R1b/48bit+busy).
    sys_puts(console_ep, "[WIFI][SDIO] step: CMD3 (get RCA)...\n");
    if (!sdio_send_cmd(EMMC_CMD_RSPNS_48, EMMC_CMD_SEND_REL_ADDR, 0)) {
        sys_puts(console_ep, "[WIFI][SDIO] FAIL: CMD3 (SEND_RELATIVE_ADDR)\n");
        return false;
    }
    g_sdio_rca = *wifi_reg(EMMC_RESP0_OFFSET) & 0xFFFF0000u;
    sys_puthex32(console_ep, "[WIFI][SDIO] CMD3 RCA = ", g_sdio_rca);

    sys_puts(console_ep, "[WIFI][SDIO] step: CMD7 (select card)...\n");
    if (!sdio_send_cmd(EMMC_CMD_RSPNS_48B, EMMC_CMD_SELECT_CARD, g_sdio_rca)) {
        sys_puts(console_ep, "[WIFI][SDIO] FAIL: CMD7 (SELECT_CARD)\n");
        return false;
    }

    sys_puts(console_ep, "[WIFI][SDIO] step: CMD52...\n");
    // CMD52 (SDIO_RW_DIRECT) чтение F0 CCCR offset 0x00 (версия CCCR/FBR).
    // R/W=0 (чтение), функция 0, RAW=0, адрес=SDIO_CCCR_CCCR_OFFSET, данные=0.
    uint32_t cmd52_arg = (SDIO_FUNC_0 << SDIO_ARG_FUNC_SHIFT) |
                         (SDIO_CCCR_CCCR_OFFSET << SDIO_ARG_REG_ADDR_SHIFT);
    if (!sdio_send_cmd(EMMC_CMD_RSPNS_48, SDIO_CMD_RW_DIRECT, cmd52_arg)) {
        sys_puts(console_ep, "[WIFI][SDIO] FAIL: CMD52 (read F0 CCCR) — команда не прошла\n");
        return false;
    }
    // R5: биты [15:8] — Response Flags, [7:0] — прочитанные данные (см.
    // platform.h — комментарий у SDIO_ARG_* про формат R5).
    uint32_t cmd52_resp = *wifi_reg(EMMC_RESP0_OFFSET);
    g_cccr_rev = cmd52_resp & 0xFFu;
    sys_puthex32(console_ep, "[WIFI][SDIO] CMD52 F0 CCCR raw resp = ", cmd52_resp);
    sys_puthex32(console_ep, "[WIFI][SDIO] CMD52 F0 CCCR value    = ", g_cccr_rev);

    return true;
}

// =====================================================================
// Милстоун 4.2: backplane (внутренняя шина чипа) — перечисление ядер, сброс
// ARM-ядра, заливка прошивки/NVRAM. Все адреса/константы сверены построчно
// с эталонным chip.c/sdio.c/bcmsdh.c (см. platform.h, шапка блока
// констант) — не догадки, а вычитанные значения для чипов семейства 0x4345.
// =====================================================================

// --- Включить SDIO-функцию 1 (backplane) — обязательный шаг перед ЛЮБЫМ
// CMD52/CMD53 к F1 (см. platform.h SDIO_CCCR_IOEx/IORx): пишем бит функции 1
// в IOEx (F0, offset 0x02), затем ждём тот же бит в IORx (F0, offset 0x03) —
// карта включает функцию не мгновенно. Без этого шага CMD53 к F1 либо не
// проходит, либо возвращает мусор — этого шага не было в первой версии
// Милстоуна 4.2, что и было причиной падения чтения chipid. ---
// Обобщено для любой SDIO-функции (изначально только F1 — Милстоун 4.2;
// Милстоун 4.3 включает F2 тем же алгоритмом, см. wifi_sdpcm_bringup()).
static bool sdio_enable_func(uint32_t func_num, seL4_CPtr console_ep) {
    uint32_t func_bit = (1u << func_num);

    // ИСПРАВЛЕНО: CMD52-запись всегда ПЕРЕЗАПИСЫВАЕТ байт целиком — нет
    // аппаратного OR. Раньше сюда писался просто func_bit, что при включении
    // F2 (func_bit=0x04) СБРАСЫВАЛО уже включённый бит функции 1 (0x02) в
    // IOEx! Это объясняет и точный момент появления бага (ровно enable F2),
    // и асимметрию (F1 CMD53-WRITE ещё как-то проходили, а READ — нет).
    // Читаем текущее значение и ORим новый бит перед записью.
    uint8_t current_ioex = 0;
    {
        uint32_t rarg = (SDIO_FUNC_0 << SDIO_ARG_FUNC_SHIFT) | (SDIO_CCCR_IOEx_OFFSET << SDIO_ARG_REG_ADDR_SHIFT);
        if (sdio_send_cmd(EMMC_CMD_RSPNS_48, SDIO_CMD_RW_DIRECT, rarg, console_ep)) {
            current_ioex = (uint8_t)(*wifi_reg(EMMC_RESP0_OFFSET) & 0xFFu);
        }
    }
    uint32_t new_ioex = ((uint32_t)current_ioex) | func_bit;

    uint32_t arg = SDIO_ARG_RW_FLAG | (SDIO_FUNC_0 << SDIO_ARG_FUNC_SHIFT) |
                   (SDIO_CCCR_IOEx_OFFSET << SDIO_ARG_REG_ADDR_SHIFT) | new_ioex;
    if (!sdio_send_cmd(EMMC_CMD_RSPNS_48, SDIO_CMD_RW_DIRECT, arg)) {
        sys_puts(console_ep, "[WIFI][BP] FAIL: CMD52 IOEx (enable func) — команда не прошла\n");
        return false;
    }

    // ИСПРАВЛЕНО: раньше ждали "100 попыток по 10000 yield-итераций" — тот же
    // анти-паттерн (число итераций вместо честного времени), который уже не
    // раз ловился в этом проекте (ALP/HT-клок и т.д.) — никакой гарантии
    // реального времени. На живом железе поймано: F2 иногда не успевает стать
    // ready за этот бюджет (не факт, что аппаратная проблема — просто
    // недостаточно реального времени), хотя F1 (та же функция) всегда
    // проходила с запасом. Честный wall-clock таймаут ~2с.
    uint64_t freq = wifi_read_cntfrq();
    uint64_t start = wifi_read_cntvct();
    uint64_t timeout_ticks = (freq * 2000000ull) / 1000000ull; // 2с
    while (true) {
        uint32_t rarg = (SDIO_FUNC_0 << SDIO_ARG_FUNC_SHIFT) | (SDIO_CCCR_IORx_OFFSET << SDIO_ARG_REG_ADDR_SHIFT);
        if (!sdio_send_cmd(EMMC_CMD_RSPNS_48, SDIO_CMD_RW_DIRECT, rarg)) {
            sys_puts(console_ep, "[WIFI][BP] FAIL: CMD52 IORx (poll func ready) — команда не прошла\n");
            return false;
        }
        uint32_t resp = *wifi_reg(EMMC_RESP0_OFFSET) & 0xFFu;
        if (resp & func_bit) return true;
        if (wifi_read_cntvct() - start >= timeout_ticks) break;
        seL4_Yield();
    }
    sys_puts(console_ep, "[WIFI][BP] FAIL: функция не стала ready за ~2с реального времени\n");
    return false;
}

static bool sdio_enable_func1(seL4_CPtr console_ep) {
    return sdio_enable_func(SBSDIO_FUNC_1, console_ep);
}

// --- SDIO-функция 1: байтовый read/write через CMD52 (для регистров окна
// SBADDRLOW/MID/HIGH — они всегда однобайтовые, не backplane-данные). ---
static bool sdio_f1_write_byte(uint32_t reg_addr, uint8_t data, seL4_CPtr console_ep = 0) {
    uint32_t arg = SDIO_ARG_RW_FLAG | (SBSDIO_FUNC_1 << SDIO_ARG_FUNC_SHIFT) |
                   (reg_addr << SDIO_ARG_REG_ADDR_SHIFT) | data;
    return sdio_send_cmd(EMMC_CMD_RSPNS_48, SDIO_CMD_RW_DIRECT, arg, console_ep);
}

static bool sdio_f1_read_byte(uint32_t reg_addr, uint8_t *out, seL4_CPtr console_ep = 0) {
    uint32_t arg = (SBSDIO_FUNC_1 << SDIO_ARG_FUNC_SHIFT) | (reg_addr << SDIO_ARG_REG_ADDR_SHIFT);
    if (!sdio_send_cmd(EMMC_CMD_RSPNS_48, SDIO_CMD_RW_DIRECT, arg, console_ep)) return false;
    *out = (uint8_t)(*wifi_reg(EMMC_RESP0_OFFSET) & 0xFFu);
    return true;
}

// --- Функция 0: CCCR/FBR-регистры (общие для карты, включая FBR других
// функций — см. SDIO_FBR_BASE_FUNC1 в platform.h) всегда адресуются с
// номером функции 0 в аргументе CMD52, ДАЖЕ когда речь о настройках функции
// 1 (её FBR — это не то же самое, что собственное адресное пространство F1,
// используемое sdio_f1_write_byte выше для CHIPCLKCSR/SBADDR*/SDIOPULLUP). ---
static bool sdio_f0_write_byte(uint32_t reg_addr, uint8_t data) {
    uint32_t arg = SDIO_ARG_RW_FLAG | (SDIO_FUNC_0 << SDIO_ARG_FUNC_SHIFT) |
                   (reg_addr << SDIO_ARG_REG_ADDR_SHIFT) | data;
    return sdio_send_cmd(EMMC_CMD_RSPNS_48, SDIO_CMD_RW_DIRECT, arg);
}

// Согласовать размер блока функции (FBR-регистр её базы + 0x10, см.
// platform.h) — реальный драйвер делает это один раз при подключении
// функции, до любых bulk-передач. Обобщено — F1 (64 байта, Милстоун 4.2) и
// F2 (512 байт, Милстоун 4.3) используют один и тот же приём.
static bool wifi_set_func_blocksize(uint32_t fbr_base, uint32_t blocksize, seL4_CPtr console_ep) {
    uint32_t reg = fbr_base + SDIO_FBR_BLKSIZE_OFFSET;
    if (!sdio_f0_write_byte(reg, (uint8_t)(blocksize & 0xFF))) {
        sys_puts(console_ep, "[WIFI][BP] FAIL: запись FBR blocksize (low)\n");
        return false;
    }
    if (!sdio_f0_write_byte(reg + 1, (uint8_t)((blocksize >> 8) & 0xFF))) {
        sys_puts(console_ep, "[WIFI][BP] FAIL: запись FBR blocksize (high)\n");
        return false;
    }
    return true;
}

static bool wifi_set_func1_blocksize(seL4_CPtr console_ep) {
    return wifi_set_func_blocksize(SDIO_FBR_BASE_FUNC1, SDIO_FUNC1_BLOCKSIZE, console_ep);
}

static bool wifi_set_func2_blocksize(seL4_CPtr console_ep) {
    return wifi_set_func_blocksize(SDIO_FBR_BASE_FUNC2, SDIO_FUNC2_BLOCKSIZE, console_ep);
}

// --- Запрос тактовой частоты САМОГО ЧИПА (не хостовой SDIO-шины!) через
// SBSDIO_FUNC1_CHIPCLKCSR (F1, offset 0x1000E) — см. эталонный
// brcmf_sdio_buscoreprep()/brcmf_sdio_clkctl() в sdio.c. Простые F0/F1
// однословные CMD52/CMD53-обращения (chipid/eromptr/регистры ядер — весь
// Милстоун 4.2 до этого места) проходят и без этого шага, но backplane
// фабрика чипа для РЕАЛЬНОЙ передачи данных (заливка прошивки в TCM RAM)
// должна быть явно разбужена и подтверждена доступной — иначе именно на
// первой настоящей bulk-записи транзакция зависает на стороне чипа и
// DATA_DONE никогда не приходит (см. FAIL при заливке прошивки на живом
// железе — это была ПЕРВАЯ операция крупнее одного 32-битного слова).
// ALP (Active Low Power clock) достаточно для enumeration/halt/reset (что
// мы уже проделали до этого без явного запроса — судя по всему, при
// разомкнутом биты SBSDIO_FORCE_HW_CLKREQ_OFF чип сам держит минимальный
// клок для одиночных регистровых обращений), но HT (полная скорость)
// нужен именно для устойчивой заливки прошивки, см. brcmf_sdio_download_
// firmware(): "brcmf_sdio_clkctl(bus, CLK_AVAIL, false)" прямо перед
// download_code_file(). ---
// Ждёт (bit(s) чипа), пока `mask` не появится целиком в CHIPCLKCSR, ИЛИ пока
// не истечёт `timeout_us` РЕАЛЬНОГО времени (ARM generic timer, не число
// итераций yield). Эталон ждёт HT честную секунду (PMU_MAX_TRANSITION_DLY) —
// без привязки к настоящему таймеру такой бюджет невозможно ни воспроизвести,
// ни доверять ему на разном железе/загрузке планировщика.
static bool wifi_wait_chipclkcsr(uint8_t mask, uint32_t timeout_us, uint8_t *out_val) {
    uint64_t freq = wifi_read_cntfrq();
    uint64_t start = wifi_read_cntvct();
    uint64_t timeout_ticks = (freq * timeout_us) / 1000000ull;
    uint8_t clkval = 0;
    while (true) {
        if (!sdio_f1_read_byte(SBSDIO_FUNC1_CHIPCLKCSR, &clkval)) return false;
        if ((clkval & mask) == mask) { *out_val = clkval; return true; }
        if (wifi_read_cntvct() - start >= timeout_ticks) { *out_val = clkval; return false; }
        seL4_Yield();
    }
}

static bool wifi_request_alp_clock(seL4_CPtr console_ep) {
    uint8_t clkset = SBSDIO_FORCE_HW_CLKREQ_OFF | SBSDIO_ALP_AVAIL_REQ;
    if (!sdio_f1_write_byte(SBSDIO_FUNC1_CHIPCLKCSR, clkset)) {
        sys_puts(console_ep, "[WIFI][BP] FAIL: запись CHIPCLKCSR (ALP req)\n");
        return false;
    }
    uint8_t clkval = 0;
    // 1 секунда — тот же бюджет, что PMU_MAX_TRANSITION_DLY в эталоне.
    wifi_wait_chipclkcsr(SBSDIO_ALP_AVAIL, 1000000, &clkval);
    if (!(clkval & SBSDIO_ALP_AVAIL)) {
        sys_puthex32(console_ep, "[WIFI][BP] FAIL: ALP_AVAIL не появился, CHIPCLKCSR = ", clkval);
        return false;
    }
    // Форсируем именно ALP (не полагаемся на HW-автопереключение) — как в buscoreprep().
    clkset = SBSDIO_FORCE_HW_CLKREQ_OFF | SBSDIO_FORCE_ALP;
    if (!sdio_f1_write_byte(SBSDIO_FUNC1_CHIPCLKCSR, clkset)) {
        sys_puts(console_ep, "[WIFI][BP] FAIL: запись CHIPCLKCSR (force ALP)\n");
        return false;
    }
    for (int y = 0; y < 500; y++) seL4_Yield(); // ~65мкс в эталоне (udelay(65))

    // Отключаем лишние встроенные подтяжки (pull-ups) на линиях SDIO — как и
    // buscoreprep() в эталоне. На плате уже есть штатная терминация (RPi4
    // WiFi — фиксированная разводка), а лишние software pull-ups ухудшают
    // целостность сигнала именно на длинных data-burst'ах (см. Data CRC
    // Error, пойманный на живом железе ровно на первой заливке прошивки
    // крупным CMD53-блоком — короткие 4-байтные backplane-обращения до этого
    // проходили, потому что там на порядки меньше шансов словить помеху).
    if (!sdio_f1_write_byte(SBSDIO_FUNC1_SDIOPULLUP, 0)) {
        sys_puts(console_ep, "[WIFI][BP] FAIL: запись SDIOPULLUP\n");
        return false;
    }
    return true;
}

// Точное соответствие эталонному brcmf_sdio_htclk() (не buscoreprep — это
// ДРУГАЯ функция эталона с другим набором бит!): запрос ТОЛЬКО
// SBSDIO_HT_AVAIL_REQ, БЕЗ SBSDIO_FORCE_HW_CLKREQ_OFF (тот бит — специфика
// buscoreprep()/wifi_request_alp_clock() выше, для htclk() эталон его не
// пишет вообще — их спутал в первой версии этого кода). Возвращает
// наблюдаемое значение через out_saveclk — эталон (brcmf_sdio_firmware_
// callback) сохраняет его, форсирует HT ПОВЕРХ него на время настройки F2, а
// затем ВОССТАНАВЛИВАЕТ это сохранённое значение — см. wifi_sdpcm_bringup().
static bool wifi_request_ht_clock(seL4_CPtr console_ep, uint8_t *out_saveclk) {
    if (!sdio_f1_write_byte(SBSDIO_FUNC1_CHIPCLKCSR, SBSDIO_HT_AVAIL_REQ, console_ep)) {
        sys_puts(console_ep, "[WIFI][SDPCM] FAIL: запись CHIPCLKCSR (HT req)\n");
        return false;
    }
    uint8_t clkval = 0;
    // 1 секунда — тот же бюджет, что PMU_MAX_TRANSITION_DLY в эталоне (HT
    // требует прогрева кристалла/PLL, заметно дольше ALP).
    wifi_wait_chipclkcsr(SBSDIO_AVBITS, 1000000, &clkval);
    if ((clkval & SBSDIO_AVBITS) != SBSDIO_AVBITS) {
        sys_puthex32(console_ep, "[WIFI][SDPCM] FAIL: HT_AVAIL не появился, CHIPCLKCSR = ", clkval);
        return false;
    }
    if (out_saveclk) *out_saveclk = clkval;
    return true;
}

// --- Backplane-окно (32KB) — см. platform.h SBSDIO_FUNC1_SBADDR*. Кэшируем
// последнее выставленное окно, чтобы не переключать его на каждый доступ,
// если адрес остался в том же окне (тот же приём, что и в эталоне). ---
static uint32_t g_wifi_sb_window = 0xFFFFFFFFu; // заведомо невалидное значение -> первый вызов точно переключит окно

static bool backplane_set_window(uint32_t addr) {
    uint32_t win = addr & SBSDIO_SBWINDOW_MASK;
    if (win == g_wifi_sb_window) return true;
    if (!sdio_f1_write_byte(SBSDIO_FUNC1_SBADDRLOW,  (uint8_t)(win >> 8)))  return false;
    if (!sdio_f1_write_byte(SBSDIO_FUNC1_SBADDRMID,  (uint8_t)(win >> 16))) return false;
    if (!sdio_f1_write_byte(SBSDIO_FUNC1_SBADDRHIGH, (uint8_t)(win >> 24))) return false;
    g_wifi_sb_window = win;
    return true;
}

// --- Backplane read/write32 через CMD53 (SDIO_RW_EXTENDED), byte-mode,
// count=4 — PIO-цикл 1:1 по образцу hardware_emmc_read/write в
// blk_driver.cpp, только "сектор" здесь — 4 байта одного слова, а не 512
// байт. Без CRCCHK/IXCHK — тот же принцип, что и у CMD5/CMD52 выше (SDIO,
// не SD-memory команды, см. их комментарии). ---
static bool backplane_read32(uint32_t addr, uint32_t *out, seL4_CPtr console_ep = 0) {
    if (!backplane_set_window(addr)) {
        if (console_ep) sys_puts(console_ep, "[WIFI][BP] FAIL: set_window (read32)\n");
        return false;
    }
    uint32_t reg_addr = (addr & SBSDIO_SB_OFT_ADDR_MASK) | SBSDIO_SB_ACCESS_2_4B_FLAG;
    uint32_t arg = (SBSDIO_FUNC_1 << SDIO_ARG_FUNC_SHIFT) | SDIO_ARG_INCR_ADDR_FLAG |
                   (reg_addr << SDIO_ARG_REG_ADDR_SHIFT) | (4u & SDIO_ARG_COUNT_MASK);
    if (!wifi_wait_dat_ready()) {
        if (console_ep) sys_puts(console_ep, "[WIFI][BP] FAIL: DAT_READY timeout (read32)\n");
        return false;
    }
    *wifi_reg(EMMC_BLKSIZECNT_OFFSET) = (1u << 16) | 4u;
    if (!sdio_send_cmd(EMMC_CMD_RSPNS_48 | EMMC_CMD_ISDATA | EMMC_TM_DAT_DIR_READ, SDIO_CMD_RW_EXTENDED, arg, console_ep)) return false;
    if (!wifi_wait_irpt_bit(EMMC_INT_READ_RDY, console_ep)) return false;
    *out = *wifi_reg(EMMC_DATA_OFFSET);
    if (!wifi_wait_irpt_bit(EMMC_INT_DATA_DONE, console_ep)) return false;
    return true;
}

static bool backplane_write32(uint32_t addr, uint32_t val, seL4_CPtr console_ep = 0) {
    if (!backplane_set_window(addr)) {
        if (console_ep) sys_puts(console_ep, "[WIFI][BP] FAIL: set_window (write32)\n");
        return false;
    }
    uint32_t reg_addr = (addr & SBSDIO_SB_OFT_ADDR_MASK) | SBSDIO_SB_ACCESS_2_4B_FLAG;
    uint32_t arg = SDIO_ARG_RW_FLAG | (SBSDIO_FUNC_1 << SDIO_ARG_FUNC_SHIFT) | SDIO_ARG_INCR_ADDR_FLAG |
                   (reg_addr << SDIO_ARG_REG_ADDR_SHIFT) | (4u & SDIO_ARG_COUNT_MASK);
    if (!wifi_wait_dat_ready()) {
        if (console_ep) sys_puts(console_ep, "[WIFI][BP] FAIL: DAT_READY timeout (write32)\n");
        return false;
    }
    *wifi_reg(EMMC_BLKSIZECNT_OFFSET) = (1u << 16) | 4u;
    if (!sdio_send_cmd(EMMC_CMD_RSPNS_48 | EMMC_CMD_ISDATA, SDIO_CMD_RW_EXTENDED, arg, console_ep)) return false;
    if (!wifi_wait_irpt_bit(EMMC_INT_WRITE_RDY, console_ep)) return false;
    *wifi_reg(EMMC_DATA_OFFSET) = val;
    if (!wifi_wait_irpt_bit(EMMC_INT_DATA_DONE, console_ep)) return false;
    return true;
}

// Один CMD53-вызов побольше 4 байт (заливка прошивки/NVRAM) — БЛОЧНЫЙ режим
// (BLOCK_MODE_FLAG, blksize=SDIO_FUNC1_BLOCKSIZE), когда len — кратно 64 и
// >= одного блока (см. wifi_set_func1_blocksize() — размер блока согласован
// заранее, реальный драйвер всегда льёт крупные куски именно так, а не
// байтовым режимом, см. bcmsdh.c). Байтовый режим остаётся только для
// "хвоста" короче одного блока (len < 64) — backplane_write_bulk ниже сам
// решает, когда какой нужен. len должен быть кратен 4 и не длиннее
// WIFI_WRITE_CHUNK, не пересекать границу 32KB-окна — гарантирует вызывающий.
constexpr uint32_t WIFI_WRITE_BLOCK_SIZE = SDIO_FUNC1_BLOCKSIZE; // 64
constexpr uint32_t WIFI_WRITE_MAX_BLOCKS = 64; // 64*64=4096 байт за один CMD53 — с большим запасом от предела в 511 блоков
constexpr uint32_t WIFI_WRITE_CHUNK = WIFI_WRITE_BLOCK_SIZE * WIFI_WRITE_MAX_BLOCKS; // 4096, кратно блоку

// console_ep только для диагностики (см. wifi_backplane_bringup) — на каком
// именно под-шаге отвалилась заливка, а не просто общий "не получилось".
static bool backplane_write_chunk(seL4_CPtr console_ep, uint32_t addr, const uint8_t *data, uint32_t len) {
    if (!backplane_set_window(addr)) {
        sys_puts(console_ep, "[WIFI][FW] FAIL: set_window\n");
        return false;
    }
    uint32_t reg_addr = (addr & SBSDIO_SB_OFT_ADDR_MASK) | SBSDIO_SB_ACCESS_2_4B_FLAG;

    bool block_mode = (len >= WIFI_WRITE_BLOCK_SIZE) && (len % WIFI_WRITE_BLOCK_SIZE == 0);
    uint32_t blksize = block_mode ? WIFI_WRITE_BLOCK_SIZE : len;
    uint32_t blkcnt  = block_mode ? (len / WIFI_WRITE_BLOCK_SIZE) : 1;
    uint32_t count_field = block_mode ? blkcnt : len; // CMD53 arg: в block mode это КОЛИЧЕСТВО блоков, в byte mode — байты

    uint32_t arg = SDIO_ARG_RW_FLAG | (SBSDIO_FUNC_1 << SDIO_ARG_FUNC_SHIFT) | SDIO_ARG_INCR_ADDR_FLAG |
                   (block_mode ? SDIO_ARG_BLOCK_MODE_FLAG : 0u) |
                   (reg_addr << SDIO_ARG_REG_ADDR_SHIFT) | (count_field & SDIO_ARG_COUNT_MASK);
    if (!wifi_wait_dat_ready()) {
        sys_puts(console_ep, "[WIFI][FW] FAIL: DAT_READY timeout\n");
        return false;
    }
    *wifi_reg(EMMC_BLKSIZECNT_OFFSET) = (blkcnt << 16) | blksize;
    // EMMC_TM_MULTI_BLOCK|BLKCNT_EN — это флаги ХОСТ-КОНТРОЛЛЕРА (регистр
    // CMDTM), отдельные от SDIO_ARG_BLOCK_MODE_FLAG в самом аргументе CMD53
    // (который видит только карта). Без них контроллер игнорирует blkcnt из
    // BLKSIZECNT и останавливается после ПЕРВОГО блока — весь остальной
    // проект (blk_driver.cpp) никогда не делал настоящий multi-block, всегда
    // гонял CMD17/24 по одному сектору, поэтому этот баг тут и всплыл
    // впервые, ровно с тем симптомом, что был на живом железе: DATA_DONE
    // приходил после одного 64-байтного блока вместо всех запрошенных.
    uint32_t cmd_flags = EMMC_CMD_RSPNS_48 | EMMC_CMD_ISDATA;
    if (block_mode) cmd_flags |= EMMC_TM_MULTI_BLOCK | EMMC_TM_BLKCNT_EN;
    if (!sdio_send_cmd(cmd_flags, SDIO_CMD_RW_EXTENDED, arg)) {
        sys_puts(console_ep, "[WIFI][FW] FAIL: CMD53 команда не прошла\n");
        return false;
    }

    // Buffer Write Ready — это событие ПЕР БЛОК (не на весь multi-block
    // перенос разом): для blkcnt>1 нужно ждать его перед КАЖДЫМ блоком, а не
    // один раз перед всем PIO-циклом — иначе часть блоков пишется в буфер,
    // который контроллер ещё не готов принять, что и давало Data CRC Error
    // на живом железе (см. историю в ROADMAP.md/памяти) на первом же куске
    // крупнее одного слова.
    const uint32_t *words = (const uint32_t*)data;
    uint32_t words_per_block = blksize / 4;
    for (uint32_t b = 0; b < blkcnt; b++) {
        if (!wifi_wait_irpt_bit(EMMC_INT_WRITE_RDY, console_ep)) {
            sys_puts(console_ep, "[WIFI][FW] FAIL: WRITE_RDY timeout\n");
            return false;
        }
        for (uint32_t w = 0; w < words_per_block; w++) {
            *wifi_reg(EMMC_DATA_OFFSET) = words[b * words_per_block + w];
        }
    }
    if (!wifi_wait_irpt_bit(EMMC_INT_DATA_DONE, console_ep)) {
        sys_puts(console_ep, "[WIFI][FW] FAIL: DATA_DONE timeout\n");
        return false;
    }
    return true;
}

// Заливает произвольной длины буфер в backplane, разбивая и на
// WIFI_WRITE_CHUNK-куски, и по границам 32KB-окна — вызывающему не нужно
// знать ни про то, ни про другое. Каждый кусок по возможности выравнивается
// ВНИЗ до кратности блока (WIFI_WRITE_BLOCK_SIZE), чтобы backplane_write_
// chunk() мог использовать честный блочный режим — короткий "хвост" в конце
// (< одного блока) уходит отдельным байтовым CMD53, как и раньше.
static bool backplane_write_bulk(seL4_CPtr console_ep, uint32_t addr, const uint8_t *data, uint32_t total_len) {
    uint32_t written = 0;
    while (written < total_len) {
        uint32_t cur_addr = addr + written;
        uint32_t win_off = cur_addr & ~SBSDIO_SBWINDOW_MASK;
        uint32_t win_remaining = SBSDIO_WINDOW_SIZE - win_off;
        uint32_t chunk = total_len - written;
        if (chunk > WIFI_WRITE_CHUNK) chunk = WIFI_WRITE_CHUNK;
        if (chunk > win_remaining) chunk = win_remaining;
        if (chunk >= WIFI_WRITE_BLOCK_SIZE) {
            chunk -= (chunk % WIFI_WRITE_BLOCK_SIZE); // выравниваем вниз, если есть хотя бы один полный блок
        }
        if (chunk == 0) {
            sys_puts(console_ep, "[WIFI][FW] FAIL: chunk == 0 (не должно случиться)\n");
            return false;
        }
        if (!backplane_write_chunk(console_ep, cur_addr, data + written, chunk)) {
            sys_puthex32(console_ep, "[WIFI][FW] FAIL: не удалось записать чанк по адресу ", cur_addr);
            sys_puthex32(console_ep, "[WIFI][FW]   written so far = ", written);
            return false;
        }
        written += chunk;
    }
    return true;
}

// --- EROM (Enumeration ROM) — находит base/wrap для CR4 и D11 ядер.
// Алгоритм 1:1 из brcmf_chip_dmp_erom_scan/brcmf_chip_dmp_get_regaddr
// (chip.c) — см. platform.h DMP_* константы и план Милстоуна 4.2. ---
struct WifiCoreInfo { uint32_t base; uint32_t wrap; };
static WifiCoreInfo g_cr4_core = {0, 0};
static WifiCoreInfo g_d11_core = {0, 0};
// Милстоун 4.3: "SDIO/PCMCIA core" — шлюз к sdpcm-регистрам (intstatus/
// mailbox), найденный тем же EROM-сканом, что CR4/D11.
static WifiCoreInfo g_sdio_core = {0, 0};

static uint32_t erom_get_desc(uint32_t *eromaddr, uint8_t *type_out) {
    uint32_t val = 0;
    backplane_read32(*eromaddr, &val);
    *eromaddr += 4;
    if (type_out) {
        uint8_t type = (uint8_t)(val & DMP_DESC_TYPE_MSK);
        if ((type & ~DMP_DESC_ADDRSIZE_GT32) == DMP_DESC_ADDRESS) type = (uint8_t)DMP_DESC_ADDRESS;
        *type_out = type;
    }
    return val;
}

// Возвращает false только на настоящую ошибку разбора (EOT посреди пути,
// или дескриптор не master-port/address там, где мы его ждали) — если
// просто дошли до следующего компонента раньше, чем нашли и base, и wrap,
// это НЕ ошибка (у ядра могло не быть, скажем, wrapper'а — оставляем 0,
// как и в эталоне: см. brcmf_chip_dmp_get_regaddr).
static bool erom_get_regaddr(uint32_t *eromaddr, uint32_t *regbase, uint32_t *wrapbase) {
    uint8_t desc;
    uint32_t val;
    uint32_t wraptype;
    *regbase = 0;
    *wrapbase = 0;

    val = erom_get_desc(eromaddr, &desc);
    if (desc == DMP_DESC_MASTER_PORT) {
        wraptype = DMP_SLAVE_TYPE_MWRAP;
    } else if (desc == DMP_DESC_ADDRESS) {
        *eromaddr -= 4;
        wraptype = DMP_SLAVE_TYPE_SWRAP;
    } else {
        *eromaddr -= 4;
        return false;
    }

    do {
        do {
            val = erom_get_desc(eromaddr, &desc);
            if (desc == DMP_DESC_EOT) { *eromaddr -= 4; return false; }
        } while (desc != DMP_DESC_ADDRESS && desc != DMP_DESC_COMPONENT);

        if (desc == DMP_DESC_COMPONENT) { *eromaddr -= 4; return true; }

        if (val & DMP_DESC_ADDRSIZE_GT32) erom_get_desc(eromaddr, nullptr); // пропустить старшие 32 бита адреса

        uint32_t sztype = (val & DMP_SLAVE_SIZE_TYPE) >> DMP_SLAVE_SIZE_TYPE_S;
        if (sztype == DMP_SLAVE_SIZE_DESC) {
            uint32_t szdesc = erom_get_desc(eromaddr, nullptr);
            if (szdesc & DMP_DESC_ADDRSIZE_GT32) erom_get_desc(eromaddr, nullptr);
        }
        if (sztype != DMP_SLAVE_SIZE_4K && sztype != DMP_SLAVE_SIZE_8K) continue;

        uint32_t stype = (val & DMP_SLAVE_TYPE) >> DMP_SLAVE_TYPE_S;
        if (*regbase == 0 && stype == DMP_SLAVE_TYPE_SLAVE) *regbase = val & DMP_SLAVE_ADDR_BASE;
        if (*wrapbase == 0 && stype == wraptype) *wrapbase = val & DMP_SLAVE_ADDR_BASE;
    } while (*regbase == 0 || *wrapbase == 0);

    return true;
}

// Chipcommon (ядро #0) сидит на фиксированном адресе SI_ENUM_BASE — читаем
// eromptr оттуда и идём по таблице, пока не найдём CR4 и D11 (остальные
// ядра нам сейчас не нужны, пропускаем).
static bool wifi_erom_scan(seL4_CPtr console_ep) {
    uint32_t chipid = 0;
    if (!backplane_read32(SI_ENUM_BASE + CHIPCOMMON_CHIPID_OFFSET, &chipid)) {
        sys_puts(console_ep, "[WIFI][BP] FAIL: не удалось прочитать chipid с backplane\n");
        return false;
    }
    sys_puthex32(console_ep, "[WIFI][BP] chipid raw = ", chipid);

    uint32_t eromaddr = 0;
    if (!backplane_read32(SI_ENUM_BASE + CHIPCOMMON_EROMPTR_OFFSET, &eromaddr)) {
        sys_puts(console_ep, "[WIFI][BP] FAIL: не удалось прочитать eromptr\n");
        return false;
    }
    sys_puthex32(console_ep, "[WIFI][BP] eromptr = ", eromaddr);

    uint8_t desc_type = 0;
    int guard = 0;
    while (desc_type != DMP_DESC_EOT) {
        if (++guard > 4000) {
            sys_puts(console_ep, "[WIFI][BP] FAIL: EROM scan — слишком много дескрипторов, похоже на мусор\n");
            return false;
        }
        uint32_t val = erom_get_desc(&eromaddr, &desc_type);
        if (!(val & DMP_DESC_VALID)) continue;
        if (desc_type == DMP_DESC_EMPTY) continue;
        if (desc_type != DMP_DESC_COMPONENT) continue;

        uint32_t id = (val & DMP_COMP_PARTNUM) >> DMP_COMP_PARTNUM_S;

        uint8_t desc2_type;
        val = erom_get_desc(&eromaddr, &desc2_type);
        if (desc2_type != DMP_DESC_COMPONENT) {
            sys_puts(console_ep, "[WIFI][BP] FAIL: EROM — некорректная пара component-дескрипторов\n");
            return false;
        }

        uint32_t nmw = (val & DMP_COMP_NUM_MWRAP) >> DMP_COMP_NUM_MWRAP_S;
        uint32_t nsw = (val & DMP_COMP_NUM_SWRAP) >> DMP_COMP_NUM_SWRAP_S;
        if (nmw + nsw == 0) continue; // ядро без портов нам не интересно (PMU/GCI и т.п.)

        uint32_t base = 0, wrap = 0;
        if (!erom_get_regaddr(&eromaddr, &base, &wrap)) continue;

        if (id == BCMA_CORE_ARM_CR4)   { g_cr4_core.base = base; g_cr4_core.wrap = wrap; }
        if (id == BCMA_CORE_80211)     { g_d11_core.base = base; g_d11_core.wrap = wrap; }
        if (id == BCMA_CORE_SDIO_DEV)  { g_sdio_core.base = base; g_sdio_core.wrap = wrap; }
    }

    sys_puthex32(console_ep, "[WIFI][BP] CR4 core.base = ", g_cr4_core.base);
    sys_puthex32(console_ep, "[WIFI][BP] CR4 core.wrap = ", g_cr4_core.wrap);
    sys_puthex32(console_ep, "[WIFI][BP] D11 core.base = ", g_d11_core.base);
    sys_puthex32(console_ep, "[WIFI][BP] D11 core.wrap = ", g_d11_core.wrap);
    sys_puthex32(console_ep, "[WIFI][BP] SDIO core.base = ", g_sdio_core.base);
    sys_puthex32(console_ep, "[WIFI][BP] SDIO core.wrap = ", g_sdio_core.wrap);

    return (g_cr4_core.wrap != 0 && g_d11_core.wrap != 0 && g_sdio_core.base != 0);
}

// --- Сброс/halt/старт ядра (AI/AXI-вариант, brcmf_chip_ai_coredisable +
// brcmf_chip_ai_resetcore слиты в одну функцию — тот же порядок действий,
// см. platform.h BCMA_*/ARMCR4_* и план Милстоуна 4.2, шаг B/D). ---
static bool wifi_core_reset(WifiCoreInfo core, uint32_t prereset, uint32_t reset, uint32_t postreset) {
    uint32_t regdata = 0;
    if (!backplane_read32(core.wrap + BCMA_RESET_CTL, &regdata)) return false;

    if ((regdata & BCMA_RESET_CTL_RESET) == 0) {
        if (!backplane_write32(core.wrap + BCMA_IOCTL, prereset | BCMA_IOCTL_FGC | BCMA_IOCTL_CLK)) return false;
        backplane_read32(core.wrap + BCMA_IOCTL, &regdata); // барьер чтения, как в эталоне
        if (!backplane_write32(core.wrap + BCMA_RESET_CTL, BCMA_RESET_CTL_RESET)) return false;
        for (int y = 0; y < 2000; y++) seL4_Yield(); // ~10-20мкс в эталоне
    }

    if (!backplane_write32(core.wrap + BCMA_IOCTL, reset | BCMA_IOCTL_FGC | BCMA_IOCTL_CLK)) return false;
    backplane_read32(core.wrap + BCMA_IOCTL, &regdata);

    int count = 0;
    while (count < 50) {
        if (!backplane_read32(core.wrap + BCMA_RESET_CTL, &regdata)) return false;
        if (!(regdata & BCMA_RESET_CTL_RESET)) break;
        backplane_write32(core.wrap + BCMA_RESET_CTL, 0);
        count++;
        for (int y = 0; y < 3000; y++) seL4_Yield(); // ~40-60мкс в эталоне
    }

    if (!backplane_write32(core.wrap + BCMA_IOCTL, postreset | BCMA_IOCTL_CLK)) return false;
    backplane_read32(core.wrap + BCMA_IOCTL, &regdata);
    return true;
}

static bool wifi_halt_cr4() {
    uint32_t val = 0;
    if (!backplane_read32(g_cr4_core.wrap + BCMA_IOCTL, &val)) return false;
    val &= ARMCR4_BCMA_IOCTL_CPUHALT; // сохраняем текущий halt-бит (обычно 0 при первом заходе)
    return wifi_core_reset(g_cr4_core, val, ARMCR4_BCMA_IOCTL_CPUHALT, ARMCR4_BCMA_IOCTL_CPUHALT);
}

static bool wifi_reset_d11() {
    return wifi_core_reset(g_d11_core,
                            D11_BCMA_IOCTL_PHYRESET | D11_BCMA_IOCTL_PHYCLOCKEN,
                            D11_BCMA_IOCTL_PHYCLOCKEN,
                            D11_BCMA_IOCTL_PHYCLOCKEN);
}

static bool wifi_start_cr4(uint32_t rstvec) {
    // brcmf_sdio_buscore_activate: reset vector пишется по backplane-адресу 0.
    if (!backplane_write32(0x00000000u, rstvec)) return false;
    return wifi_core_reset(g_cr4_core, ARMCR4_BCMA_IOCTL_CPUHALT, 0, 0);
}

// Реальный размер TCM RAM — читается из ARMCR4_CAP/BANKIDX/BANKINFO (офсеты
// от core.BASE, не wrap — см. platform.h). Нужен только для адреса NVRAM и
// sdpcm_shared (оба — в самом верху RAM), сам rambase — хардкод для чипа 0x4345.
static uint32_t wifi_calc_ramsize(seL4_CPtr console_ep) {
    uint32_t cap = 0;
    if (!backplane_read32(g_cr4_core.base + ARMCR4_CAP, &cap)) return 0;
    uint32_t nab = (cap & ARMCR4_TCBANB_MASK) >> ARMCR4_TCBANB_SHIFT;
    uint32_t nbb = (cap & ARMCR4_TCBBNB_MASK) >> ARMCR4_TCBBNB_SHIFT;
    uint32_t totb = nab + nbb;
    uint32_t memsize = 0;
    for (uint32_t idx = 0; idx < totb; idx++) {
        if (!backplane_write32(g_cr4_core.base + ARMCR4_BANKIDX, idx)) return 0;
        uint32_t bxinfo = 0;
        if (!backplane_read32(g_cr4_core.base + ARMCR4_BANKINFO, &bxinfo)) return 0;
        memsize += ((bxinfo & ARMCR4_BSZ_MASK) + 1) * ARMCR4_BSZ_MULT;
    }
    sys_puthex32(console_ep, "[WIFI][BP] ramsize = ", memsize);
    return memsize;
}

// --- Чтение файлов с SD через blk_driver (SYS_READ_FILE=119) — тот же
// протокол, что main.cpp::load_elf_from_disk (путь в SHM offset 0,
// перезатирается ответом при каждом вызове), просто читаем в свой большой
// статический буфер вместо буфера рутсервера. ---
static char *g_wifi_shm = nullptr;
static seL4_CPtr g_wifi_blk_ep = 0;

// Общий межпроцессный спинлок на офсете 4096 разделяемой SHM — тот же самый,
// что vfs_spinlock_ptr в shell.cpp и net_vfs_lock/unlock в net_driver.cpp (см.
// их комментарии). wifi_read_file() ниже пишет имя файла в тот же офсет 0-63
// общей SHM, что и ЛЮБОЙ файловый syscall шелла и фоновый net_log_flush()
// net_driver'а — без этого лока их запись может гонкой перезатереть имя файла
// прямо между тем, как мы его положили в SHM, и тем, как blk_driver его
// прочитает (та же причина, что ранее ломала `ps`). Обычные volatile
// чтение/запись, БЕЗ __sync_lock_test_and_set/release — на некэшируемой
// Device-памяти они компилируются в LDXR/STXR с непредсказуемым поведением
// (см. подробный комментарий в net_driver.cpp); настоящий atomic не нужен,
// ядро однопроцессорное (SMP OFF).
static inline void wifi_vfs_lock() {
    volatile int* lock = (volatile int*)(g_wifi_shm + 4096);
    while (*lock) seL4_Yield();
    *lock = 1;
}
static inline void wifi_vfs_unlock() {
    volatile int* lock = (volatile int*)(g_wifi_shm + 4096);
    *lock = 0;
}

static int wifi_read_file(const char *filename, uint8_t *out_buf, uint32_t max_len) {
    if (g_wifi_blk_ep == 0 || g_wifi_shm == nullptr) return -1;
    uint32_t total = 0;
    while (true) {
        wifi_vfs_lock();
        int i = 0;
        while (filename[i] && i < 63) { g_wifi_shm[i] = filename[i]; i++; }
        g_wifi_shm[i] = '\0';

        seL4_SetMR(0, 119); // SYS_READ_FILE
        seL4_SetMR(1, total); // смещение
        seL4_Call(g_wifi_blk_ep, seL4_MessageInfo_new(0, 0, 0, 2));

        int status = seL4_GetMR(0);
        int bytes_read = seL4_GetMR(1);

        if (status != 0) { wifi_vfs_unlock(); return -1; } // файл не найден/ошибка чтения
        if (bytes_read == 0) { wifi_vfs_unlock(); break; } // EOF

        if (total + (uint32_t)bytes_read > max_len) { wifi_vfs_unlock(); return -2; } // буфер мал
        for (int k = 0; k < bytes_read; k++) out_buf[total + k] = (uint8_t)g_wifi_shm[k];
        total += (uint32_t)bytes_read;
        wifi_vfs_unlock();
    }
    return (int)total;
}

// --- NVRAM: текстовый key=value\n... -> формат, который ждёт прошивка
// (key=value\0key=value\0...\0, дополнено нулями до кратности 4, плюс
// 4-байтовый трейлер). Минимальная версия brcmf_fw_nvram_strip() —
// достаточно для одноплатного, уже готового файла (без multi-device
// секций/macaddr-плейсхолдера, см. план Милстоуна 4.2): пропускаем
// пустые строки и строки-комментарии (начинаются с '#'), '\n'/"\r\n"
// заменяем на один '\0'. ---
static uint32_t wifi_process_nvram(const uint8_t *raw, uint32_t raw_len, uint8_t *out, uint32_t out_cap) {
    uint32_t out_len = 0;
    uint32_t i = 0;
    while (i < raw_len) {
        uint32_t line_start = i;
        while (i < raw_len && raw[i] != '\n') i++;
        uint32_t line_len = i - line_start;
        if (i < raw_len) i++; // пропустить сам '\n'
        if (line_len > 0 && raw[line_start + line_len - 1] == '\r') line_len--; // CRLF

        if (line_len == 0) continue;
        if (raw[line_start] == '#') continue;

        if (out_len + line_len + 1 > out_cap) return 0;
        for (uint32_t k = 0; k < line_len; k++) out[out_len++] = raw[line_start + k];
        out[out_len++] = 0;
    }

    uint32_t new_length = (out_len + 1 + 3) & ~3u; // roundup(out_len+1, 4)
    if (new_length + 4 > out_cap) return 0;
    while (out_len < new_length) out[out_len++] = 0;

    uint32_t token = new_length / 4;
    uint32_t trailer = (~token << 16) | (token & 0xFFFFu);
    out[out_len++] = (uint8_t)(trailer & 0xFF);
    out[out_len++] = (uint8_t)((trailer >> 8) & 0xFF);
    out[out_len++] = (uint8_t)((trailer >> 16) & 0xFF);
    out[out_len++] = (uint8_t)((trailer >> 24) & 0xFF);
    return out_len;
}

// Статические буферы под прошивку/NVRAM — большие, живут в .bss процесса
// (получают память постранично через обычный ELF-загрузчик рутсервера, см.
// план Милстоуна 4.2 — предупреждение про расход CSlot-бюджета при первом тесте).
constexpr uint32_t WIFI_FW_BUF_CAP     = 700 * 1024;
constexpr uint32_t WIFI_NVRAM_RAW_CAP  = 4 * 1024;
constexpr uint32_t WIFI_NVRAM_OUT_CAP  = 4 * 1024;
// alignas(4): backplane_write_chunk() реинтерпретирует кусок этого буфера
// как uint32_t* для PIO-цикла — без явного выравнивания это было бы UB
// (uint8_t[] в общем случае не гарантирует выравнивание), а не абстрактная
// придирка: ровно такой же класс сюрприза (Alignment Fault) уже один раз
// уронил рутсервер в этой же сессии (см. flush_rootserver_shm() в main.cpp).
alignas(4) static uint8_t g_wifi_fw_buf[WIFI_FW_BUF_CAP];
alignas(4) static uint8_t g_wifi_nvram_raw[WIFI_NVRAM_RAW_CAP];
alignas(4) static uint8_t g_wifi_nvram_out[WIFI_NVRAM_OUT_CAP];

static uint32_t g_wifi_ramsize = 0;
static uint32_t g_wifi_shaddr = 0;       // адрес sdpcm_shared, если прошивка ожила
static bool     g_wifi_fw_alive = false;

// --- sdpcm_shared/rte_console — минимальный readback "жива ли прошивка"
// (см. sdio.c: struct sdpcm_shared/rte_console, brcmf_sdio_readshared/
// readconsole — это ровно то же самое, без sdpcm-кольца). ---
constexpr uint32_t SDPCM_SHARED_CONSOLE_ADDR_OFFSET = 20; // 6-е поле (offset), см. struct sdpcm_shared в sdio.c
constexpr uint32_t RTE_CONSOLE_LOG_BUF_OFFSET      = 8;  // rte_console.log_le.buf
constexpr uint32_t RTE_CONSOLE_LOG_BUF_SIZE_OFFSET = 12; // rte_console.log_le.buf_size
constexpr uint32_t RTE_CONSOLE_LOG_IDX_OFFSET      = 16; // rte_console.log_le.idx

static void wifi_try_read_console(seL4_CPtr console_ep, uint32_t console_addr) {
    uint32_t log_buf = 0, log_size = 0, log_idx = 0;
    if (!backplane_read32(console_addr + RTE_CONSOLE_LOG_BUF_OFFSET, &log_buf)) return;
    if (!backplane_read32(console_addr + RTE_CONSOLE_LOG_BUF_SIZE_OFFSET, &log_size)) return;
    if (!backplane_read32(console_addr + RTE_CONSOLE_LOG_IDX_OFFSET, &log_idx)) return;
    sys_puthex32(console_ep, "[WIFI][FW] console log_buf  = ", log_buf);
    sys_puthex32(console_ep, "[WIFI][FW] console log_size = ", log_size);
    sys_puthex32(console_ep, "[WIFI][FW] console log_idx  = ", log_idx);
    if (log_buf == 0 || log_size == 0) return;

    // Дамп первых ~128 байт лога прошивки как есть (диагностика, не парсим
    // кольцевой буфер по-настоящему — если видно что-то похожее на текст,
    // этого достаточно, чтобы доказать, что прошивка реально исполняется).
    uint32_t dump_len = log_size < 128 ? log_size : 128;
    sys_puts(console_ep, "[WIFI][FW] console dump: \"");
    for (uint32_t off = 0; off < dump_len; off += 4) {
        uint32_t word = 0;
        if (!backplane_read32(log_buf + off, &word)) break;
        for (int b = 0; b < 4; b++) {
            char c = (char)((word >> (b * 8)) & 0xFF);
            if (c == 0) continue;
            if (c < 0x20 || c > 0x7E) c = '.'; // непечатное -> точка, не мусорить консоль
            char s[2] = {c, 0};
            sys_puts(console_ep, s);
        }
    }
    sys_puts(console_ep, "\"\n");
}

static void wifi_check_alive(seL4_CPtr console_ep) {
    uint32_t shaddr = 0;
    uint32_t top = BRCM_4345_RAMBASE + g_wifi_ramsize;
    if (!backplane_read32(top - 4, &shaddr)) {
        sys_puts(console_ep, "[WIFI][FW] FAIL: не удалось прочитать shaddr\n");
        return;
    }
    sys_puthex32(console_ep, "[WIFI][FW] shaddr = ", shaddr);
    // Простая проверка на валидность (см. brcmf_sdio_valid_shared_address):
    // не 0, не 0xFFFFFFFF, в разумных пределах RAM самого чипа.
    if (shaddr == 0 || shaddr == 0xFFFFFFFFu || shaddr < BRCM_4345_RAMBASE || shaddr >= top) {
        sys_puts(console_ep, "[WIFI][FW] shaddr невалиден — прошивка, похоже, ещё не стартовала (или стартовала не так, как ожидалось)\n");
        return;
    }
    g_wifi_shaddr = shaddr;
    g_wifi_fw_alive = true;
    sys_puts(console_ep, "[WIFI][FW] shaddr валиден — прошивка похожа на живую!\n");

    uint32_t console_addr = 0;
    if (!backplane_read32(shaddr + SDPCM_SHARED_CONSOLE_ADDR_OFFSET, &console_addr)) return;
    sys_puthex32(console_ep, "[WIFI][FW] console_addr = ", console_addr);
    if (console_addr != 0 && console_addr != 0xFFFFFFFFu) {
        wifi_try_read_console(console_ep, console_addr);
    }
}

// Полная оркестровка Милстоуна 4.2 — вызывается из main() ПОСЛЕ успешного
// Милстоуна 4.1 (SDIO host/CMD52 уже подтверждены). Каждый шаг логируется
// отдельно — см. wifi_sdio_probe() выше про причину такой подробности
// (предыдущий halt всего kernel на живом железе).
static bool wifi_backplane_bringup(seL4_CPtr console_ep) {
    // Милстоун 4.1 намеренно держал делитель идентификационной стадии (0x80,
    // ~390кГц) на всё время — там речь шла об одной-двух командах, скорость
    // была не важна. Все backplane-обращения Милстоуна 4.2 (включая bulk-
    // заливку прошивки) шли на том же делителе — работало, но крайне
    // медленно. Карта уже выбрана (CMD7 в wifi_sdio_probe завершился успешно
    // до входа сюда), поэтому, как и blk_driver.cpp после его CMD7 ("рабочая
    // стадия, standard speed"), поднимаем клок до 0x02 (100МГц/(2*2)=25МГц).
    sys_puts(console_ep, "[WIFI][BP] step: переключение SDIO-клока на рабочую скорость (25МГц)...\n");
    sdio_set_clock_divider(0x02);

    sys_puts(console_ep, "[WIFI][BP] step: enable SDIO func1 (IOEx/IORx)...\n");
    if (!sdio_enable_func1(console_ep)) return false;

    sys_puts(console_ep, "[WIFI][BP] step: согласование блок-размера func1 (64 байта)...\n");
    if (!wifi_set_func1_blocksize(console_ep)) return false;

    sys_puts(console_ep, "[WIFI][BP] step: запрос тактовой частоты чипа (ALP)...\n");
    if (!wifi_request_alp_clock(console_ep)) return false;

    sys_puts(console_ep, "[WIFI][BP] step: EROM scan (искать CR4/D11 ядра)...\n");
    if (!wifi_erom_scan(console_ep)) {
        sys_puts(console_ep, "[WIFI][BP] FAIL: EROM scan не нашёл оба нужных ядра\n");
        return false;
    }

    sys_puts(console_ep, "[WIFI][BP] step: halt CR4...\n");
    if (!wifi_halt_cr4()) { sys_puts(console_ep, "[WIFI][BP] FAIL: halt CR4\n"); return false; }

    sys_puts(console_ep, "[WIFI][BP] step: reset D11...\n");
    if (!wifi_reset_d11()) { sys_puts(console_ep, "[WIFI][BP] FAIL: reset D11\n"); return false; }

    sys_puts(console_ep, "[WIFI][BP] step: ramsize...\n");
    g_wifi_ramsize = wifi_calc_ramsize(console_ep);
    if (g_wifi_ramsize == 0) { sys_puts(console_ep, "[WIFI][BP] FAIL: ramsize == 0\n"); return false; }

    sys_puts(console_ep, "[WIFI][BP] step: чтение прошивки с SD...\n");
    int fw_len = wifi_read_file(PATH_WIFI_FW, g_wifi_fw_buf, WIFI_FW_BUF_CAP);
    if (fw_len <= 0) { sys_puts(console_ep, "[WIFI][BP] FAIL: не удалось прочитать wifi_fw.bin\n"); return false; }
    sys_puthex32(console_ep, "[WIFI][BP] firmware length = ", (uint32_t)fw_len);
    // PIO-цикл backplane_write_chunk() пишет данные 32-битными словами — длина
    // файла произвольная (не обязана быть кратна 4), поэтому "хвост" в 1-3
    // байта дополняем нулями до целого слова (лишние нулевые байты в TCM RAM
    // безвредны — это либо неиспользуемое место перед NVRAM в самом верху
    // RAM, либо просто конец образа). Без этого последний CMD53-чанк с
    // len%4!=0 вычислял words_per_block=0 и зависал: PIO-цикл не писал в FIFO
    // ни единого слова, а контроллер ждал данные бесконечно (DATA_DONE timeout,
    // словлено на живом железе ровно на последних 3 байтах прошивки).
    uint32_t fw_len_padded = ((uint32_t)fw_len + 3u) & ~3u;
    for (uint32_t i = (uint32_t)fw_len; i < fw_len_padded; i++) g_wifi_fw_buf[i] = 0;
    fw_len = (int)fw_len_padded;

    sys_puts(console_ep, "[WIFI][BP] step: чтение NVRAM с SD...\n");
    int nvram_raw_len = wifi_read_file(PATH_WIFI_NVRAM, g_wifi_nvram_raw, WIFI_NVRAM_RAW_CAP);
    if (nvram_raw_len <= 0) { sys_puts(console_ep, "[WIFI][BP] FAIL: не удалось прочитать wifi_nvram.txt\n"); return false; }
    uint32_t nvram_len = wifi_process_nvram(g_wifi_nvram_raw, (uint32_t)nvram_raw_len, g_wifi_nvram_out, WIFI_NVRAM_OUT_CAP);
    if (nvram_len == 0) { sys_puts(console_ep, "[WIFI][BP] FAIL: обработка NVRAM не удалась (буфер мал?)\n"); return false; }
    sys_puthex32(console_ep, "[WIFI][BP] nvram processed length = ", nvram_len);

    // ВАЖНО: во время заливки прошивки эталонный brcmf_sdio_firmware_callback()
    // явно выставляет bus->alp_only=true перед download_firmware() — то есть
    // сам эталон запрашивает здесь ТОЛЬКО ALP (уже получен выше), а не HT.
    // HT (wifi_request_ht_clock — см. функцию выше, оставлена для будущего
    // милстоуна) запрашивается эталоном только ПОСЛЕ успешной загрузки,
    // перед включением F2/data-path — там он и нужен по-настоящему.
    sys_puts(console_ep, "[WIFI][BP] step: заливка прошивки в TCM RAM...\n");
    if (!backplane_write_bulk(console_ep, BRCM_4345_RAMBASE, g_wifi_fw_buf, (uint32_t)fw_len)) {
        sys_puts(console_ep, "[WIFI][BP] FAIL: заливка прошивки\n");
        return false;
    }

    sys_puts(console_ep, "[WIFI][BP] step: заливка NVRAM в верх RAM...\n");
    uint32_t nvram_addr = BRCM_4345_RAMBASE + g_wifi_ramsize - nvram_len;
    if (!backplane_write_bulk(console_ep, nvram_addr, g_wifi_nvram_out, nvram_len)) {
        sys_puts(console_ep, "[WIFI][BP] FAIL: заливка NVRAM\n");
        return false;
    }

    uint32_t rstvec = ((uint32_t)g_wifi_fw_buf[0]) | ((uint32_t)g_wifi_fw_buf[1] << 8) |
                      ((uint32_t)g_wifi_fw_buf[2] << 16) | ((uint32_t)g_wifi_fw_buf[3] << 24);
    sys_puthex32(console_ep, "[WIFI][BP] rstvec = ", rstvec);

    sys_puts(console_ep, "[WIFI][BP] step: запуск CR4 (un-halt)...\n");
    if (!wifi_start_cr4(rstvec)) { sys_puts(console_ep, "[WIFI][BP] FAIL: старт CR4\n"); return false; }

    // Дать прошивке время на собственную инициализацию перед первым чтением.
    // ИСПРАВЛЕНО: раньше здесь был счётчик yield-итераций (500000), тот же
    // анти-паттерн, что уже не раз ловился в этом проекте — никакой гарантии
    // реального времени, зависит от размера бинарника/скорости выполнения
    // хоста. На живом железе поймано: после того как wifi_driver вырос
    // (Милстоун 4.4, крипто+join), прошивка стала детерминированно падать
    // (регистровый trace-дамп в консоли, `log_idx` замирает — прошивка
    // реально останавливается) ещё ДО того, как мы вообще доходим до нового
    // кода — похоже на гонку: хост начинал дёргать backplane/читать
    // sdpcm_shared раньше, чем прошивка успевала закончить свою внутреннюю
    // инициализацию. ОБНОВЛЕНИЕ: 500мс однажды помогли (чистый успешный
    // прогон), но на СЛЕДУЮЩЕЙ попытке — БЕЗ каких-либо изменений в этом
    // месте — краш вернулся с ТЕМ ЖЕ log_idx/регистрами, что и раньше. Значит
    // это не детерминированный порог "хватает/не хватает", а настоящая
    // граничная нестабильность реального времени (тепловой режим/SD-карта/
    // что-то ещё) — раз margin в 500мс недостаточно надёжен, увеличиваем
    // паузу с запасом до ~1с.
    {
        uint64_t freq = wifi_read_cntfrq();
        uint64_t start = wifi_read_cntvct();
        uint64_t settle_ticks = (freq * 1000000ull) / 1000000ull; // 1с
        while (wifi_read_cntvct() - start < settle_ticks) seL4_Yield();
    }

    sys_puts(console_ep, "[WIFI][BP] step: проверка, жива ли прошивка (sdpcm_shared)...\n");
    wifi_check_alive(console_ep);

    return g_wifi_fw_alive;
}

// =====================================================================
// Милстоун 4.3: sdpcm-канал (SDIO-функция 2) + CDC/BDC IOCTL. Все раскладки/
// константы сверены построчно с эталонным sdio.c/bcdc.c/fwil.c/bcmsdh.c
// агентом-исследователем (цитаты file:line) — см. platform.h, блок констант
// "Милстоун 4.3". Первый сквозной тест — запросить у прошивки версию (сырой
// dcmd GET_VERSION, потом iovar "ver") и получить осмысленный ответ по
// НАСТОЯЩЕМУ протокольному каналу (не через отладочный rte_console-хак
// Милстоуна 4.2).
//
// ВАЖНО (исправлено после отказов на живом железе): F2-передачи данных — это
// НЕ независимый FIFO-порт с адресом 0, как предполагалось изначально. Они
// идут через ТОТ ЖЕ backplane-window механизм F1 (см. bcmsdh.c
// brcmf_sdiod_send_buf/recv_chain — addr=chipcommon.base=SI_ENUM_BASE, та же
// формула (addr & SBSDIO_SB_OFT_ADDR_MASK)|SBSDIO_SB_ACCESS_2_4B_FLAG, что и
// у backplane_read32/write32), просто с номером функции 2 вместо 1 в
// аргументе CMD53 — см. sdio_f2_write/read ниже.
//
// Проект нигде не использует настоящие SDIO-прерывания (ни OOB GPIO, ни
// in-band DAT1 через GIC) — везде чистый polling статусных регистров, тот же
// принцип и здесь: пропускаем регистрацию CCCR IENx/OOB/in-band IRQ целиком
// и просто поллим intstatus напрямую через уже проверенный backplane_read32.
// =====================================================================

// --- SDIO-функция 2: read/write через CMD53 — тот же приём (block/byte
// mode, per-block WRITE_RDY/READ_RDY на каждый блок отдельно), что
// backplane_write_chunk() выше для F1. ИСПРАВЛЕНО (после отказов на живом
// железе с Command Timeout+CRC Error): вопреки первоначальному
// предположению, F2 — НЕ независимый FIFO-порт с адресом 0. Эталонный
// bcmsdh.c (brcmf_sdiod_send_buf/recv_chain) показывает, что F2-передачи
// ТОЖЕ идут через backplane-окно F1 (brcmf_sdiod_set_backplane_window с
// addr=cc_core->base, т.е. chipcommon — тот же SI_ENUM_BASE, что уже
// используем в Милстоуне 4.2), и адрес в аргументе CMD53 считается ТЕМ ЖЕ
// способом: (addr & SBSDIO_SB_OFT_ADDR_MASK) | SBSDIO_SB_ACCESS_2_4B_FLAG —
// просто номер функции в аргументе другой (2, не 1). ---
static bool sdio_f2_write(seL4_CPtr console_ep, const uint8_t *data, uint32_t len) {
    if (!backplane_set_window(SI_ENUM_BASE)) {
        sys_puts(console_ep, "[WIFI][SDPCM] FAIL: set_window (F2 write)\n");
        return false;
    }
    uint32_t reg_addr = (SI_ENUM_BASE & SBSDIO_SB_OFT_ADDR_MASK) | SBSDIO_SB_ACCESS_2_4B_FLAG;

    bool block_mode = (len >= SDIO_FUNC2_BLOCKSIZE) && (len % SDIO_FUNC2_BLOCKSIZE == 0);
    uint32_t blksize = block_mode ? SDIO_FUNC2_BLOCKSIZE : len;
    uint32_t blkcnt  = block_mode ? (len / SDIO_FUNC2_BLOCKSIZE) : 1;
    uint32_t count_field = block_mode ? blkcnt : len;

    uint32_t arg = SDIO_ARG_RW_FLAG | (SBSDIO_FUNC_2 << SDIO_ARG_FUNC_SHIFT) | SDIO_ARG_INCR_ADDR_FLAG |
                   (block_mode ? SDIO_ARG_BLOCK_MODE_FLAG : 0u) |
                   (reg_addr << SDIO_ARG_REG_ADDR_SHIFT) | (count_field & SDIO_ARG_COUNT_MASK);
    if (!wifi_wait_dat_ready()) {
        sys_puts(console_ep, "[WIFI][SDPCM] FAIL: DAT_READY timeout (F2 write)\n");
        return false;
    }
    *wifi_reg(EMMC_BLKSIZECNT_OFFSET) = (blkcnt << 16) | blksize;
    uint32_t cmd_flags = EMMC_CMD_RSPNS_48 | EMMC_CMD_ISDATA;
    if (block_mode) cmd_flags |= EMMC_TM_MULTI_BLOCK | EMMC_TM_BLKCNT_EN;
    if (!sdio_send_cmd(cmd_flags, SDIO_CMD_RW_EXTENDED, arg, console_ep)) {
        sys_puts(console_ep, "[WIFI][SDPCM] FAIL: CMD53 (F2 write) команда не прошла\n");
        return false;
    }
    const uint32_t *words = (const uint32_t*)data;
    uint32_t words_per_block = blksize / 4;
    for (uint32_t b = 0; b < blkcnt; b++) {
        if (!wifi_wait_irpt_bit(EMMC_INT_WRITE_RDY, console_ep)) {
            sys_puts(console_ep, "[WIFI][SDPCM] FAIL: WRITE_RDY timeout (F2)\n");
            return false;
        }
        for (uint32_t w = 0; w < words_per_block; w++) *wifi_reg(EMMC_DATA_OFFSET) = words[b * words_per_block + w];
    }
    if (!wifi_wait_irpt_bit(EMMC_INT_DATA_DONE, console_ep)) {
        sys_puts(console_ep, "[WIFI][SDPCM] FAIL: DATA_DONE timeout (F2 write)\n");
        return false;
    }
    return true;
}

static bool sdio_f2_read(seL4_CPtr console_ep, uint8_t *out, uint32_t len) {
    if (!backplane_set_window(SI_ENUM_BASE)) {
        sys_puts(console_ep, "[WIFI][SDPCM] FAIL: set_window (F2 read)\n");
        return false;
    }
    uint32_t reg_addr = (SI_ENUM_BASE & SBSDIO_SB_OFT_ADDR_MASK) | SBSDIO_SB_ACCESS_2_4B_FLAG;

    bool block_mode = (len >= SDIO_FUNC2_BLOCKSIZE) && (len % SDIO_FUNC2_BLOCKSIZE == 0);
    uint32_t blksize = block_mode ? SDIO_FUNC2_BLOCKSIZE : len;
    uint32_t blkcnt  = block_mode ? (len / SDIO_FUNC2_BLOCKSIZE) : 1;
    uint32_t count_field = block_mode ? blkcnt : len;

    uint32_t arg = (SBSDIO_FUNC_2 << SDIO_ARG_FUNC_SHIFT) | SDIO_ARG_INCR_ADDR_FLAG |
                   (block_mode ? SDIO_ARG_BLOCK_MODE_FLAG : 0u) |
                   (reg_addr << SDIO_ARG_REG_ADDR_SHIFT) | (count_field & SDIO_ARG_COUNT_MASK);
    if (!wifi_wait_dat_ready()) {
        sys_puts(console_ep, "[WIFI][SDPCM] FAIL: DAT_READY timeout (F2 read)\n");
        return false;
    }
    *wifi_reg(EMMC_BLKSIZECNT_OFFSET) = (blkcnt << 16) | blksize;
    uint32_t cmd_flags = EMMC_CMD_RSPNS_48 | EMMC_CMD_ISDATA | EMMC_TM_DAT_DIR_READ;
    if (block_mode) cmd_flags |= EMMC_TM_MULTI_BLOCK | EMMC_TM_BLKCNT_EN;
    if (!sdio_send_cmd(cmd_flags, SDIO_CMD_RW_EXTENDED, arg, console_ep)) {
        sys_puts(console_ep, "[WIFI][SDPCM] FAIL: CMD53 (F2 read) команда не прошла\n");
        return false;
    }
    uint32_t *words = (uint32_t*)out;
    uint32_t words_per_block = blksize / 4;
    for (uint32_t b = 0; b < blkcnt; b++) {
        if (!wifi_wait_irpt_bit(EMMC_INT_READ_RDY, console_ep)) {
            sys_puts(console_ep, "[WIFI][SDPCM] FAIL: READ_RDY timeout (F2)\n");
            return false;
        }
        for (uint32_t w = 0; w < words_per_block; w++) words[b * words_per_block + w] = *wifi_reg(EMMC_DATA_OFFSET);
    }
    if (!wifi_wait_irpt_bit(EMMC_INT_DATA_DONE, console_ep)) {
        sys_puts(console_ep, "[WIFI][SDPCM] FAIL: DATA_DONE timeout (F2 read)\n");
        return false;
    }
    return true;
}

// --- sdpcm software header (12 байт: 4 hwhdr + 8 swhdr), TX-сборка. Байтовый
// layout (см. план Милстоуна 4.3):
//   0-1: len (LE)         2-3: ~len (LE, чексумма)
//   4: seq_num            5[3:0]: channel (control=0)
//   6: len_nxtfrm (TX=0)  7: dat_offset (=SDPCM_HDRLEN=12, без глома)
//   8: fcmask (TX=0)      9: tx_seq_max/window (TX=0)   10-11: 0
// Статические буферы (не стек — тот же принцип, что и большие буферы
// прошивки/NVRAM в Милстоуне 4.2). ---
constexpr uint32_t WIFI_SDPCM_TX_BUF_CAP = 512;
alignas(4) static uint8_t g_sdpcm_tx_buf[WIFI_SDPCM_TX_BUF_CAP];
constexpr uint32_t WIFI_SDPCM_RX_BUF_CAP = 512;
alignas(4) static uint8_t g_sdpcm_rx_buf[WIFI_SDPCM_RX_BUF_CAP];
static uint8_t g_sdpcm_tx_seq = 0;

static bool sdpcm_send_ctrl(seL4_CPtr console_ep, const uint8_t *payload, uint32_t payload_len) {
    uint32_t total_len = SDPCM_HDRLEN + payload_len;
    // Хвост не кратный 4 не пишется word-PIO циклом (та же причина, что
    // сломала последние 3 байта прошивки в Милстоуне 4.2) — дополняем нулями.
    uint32_t padded_len = (total_len + 3u) & ~3u;
    if (padded_len > WIFI_SDPCM_TX_BUF_CAP) {
        sys_puts(console_ep, "[WIFI][SDPCM] FAIL: TX-кадр не влезает в буфер\n");
        return false;
    }
    for (uint32_t i = 0; i < padded_len; i++) g_sdpcm_tx_buf[i] = 0;
    for (uint32_t i = 0; i < payload_len; i++) g_sdpcm_tx_buf[SDPCM_HDRLEN + i] = payload[i];

    uint16_t len16 = (uint16_t)total_len;
    uint16_t chk16 = (uint16_t)~len16;
    g_sdpcm_tx_buf[0] = (uint8_t)(len16 & 0xFF);
    g_sdpcm_tx_buf[1] = (uint8_t)(len16 >> 8);
    g_sdpcm_tx_buf[2] = (uint8_t)(chk16 & 0xFF);
    g_sdpcm_tx_buf[3] = (uint8_t)(chk16 >> 8);
    g_sdpcm_tx_buf[4] = g_sdpcm_tx_seq++;
    g_sdpcm_tx_buf[5] = (uint8_t)(SDPCM_CONTROL_CHANNEL & 0x0F);
    g_sdpcm_tx_buf[6] = 0;
    g_sdpcm_tx_buf[7] = (uint8_t)SDPCM_HDRLEN;
    g_sdpcm_tx_buf[8] = 0; g_sdpcm_tx_buf[9] = 0; g_sdpcm_tx_buf[10] = 0; g_sdpcm_tx_buf[11] = 0;

    sys_puthexbuf(console_ep, "[WIFI][SDPCM] диагностика: полный TX-кадр (hdr+dcmd) = ", g_sdpcm_tx_buf, padded_len);

    return sdio_f2_write(console_ep, g_sdpcm_tx_buf, padded_len);
}

// Честный wall-clock таймаут (ARM generic timer — см. wifi_wait_chipclkcsr()
// выше, тот же приём). Изначально 2.5с, как CTL_DONE_TIMEOUT/DCMD_RESP_
// TIMEOUT в эталоне — но на живом железе КАЖДЫЙ ответ прошивки (не только
// первый) стабильно приходил чуть ПОЗЖЕ 8с — каждый раз подбирался только во
// время ожидания СЛЕДУЮЩЕГО запроса (reqid не совпадал ровно на 1), что
// указывает на постоянную задержку ~8+с у самой прошивки на КАЖДЫЙ dcmd, а
// не на разовые накладные расходы инициализации. Увеличено с ещё большим
// запасом, чтобы каждый запрос дождался СВОЕГО ответа сам, не полагаясь на
// то, что его подберёт ожидание следующего.
// Не разводим строго "ack mailbox" / "прочитать кадр" по фазам, как DPC в
// эталоне — пробуем прочитать кадр сразу после любого из двух битов, этого
// достаточно для одного синхронного IOCTL-запроса за раз.
constexpr uint32_t WIFI_SDPCM_TIMEOUT_US = 15000000;

// Первое обращение к ядру SDIO_DEV после переключения клока на HT поймало на
// живом железе Data Timeout Error (не Command Timeout — команда прошла,
// зависла именно фаза данных) — похоже на переходный глюк ровно в момент
// смены тактового домена ALP->HT, а не системную ошибку (адрес/ядро найдены
// корректно тем же EROM-сканом, что CR4/D11). Несколько попыток с паузой —
// дешёвая проверка этой гипотезы, не трогая уже проверенный backplane_
// write32/read32 в остальном коде (Милстоун 4.2 их использует десятки раз
// без единого сбоя). Диагностика печатается только на последней попытке,
// чтобы не шуметь, если разойдётся со второй-третьей попытки.
static bool backplane_write32_retry(uint32_t addr, uint32_t val, seL4_CPtr console_ep, int attempts = 3) {
    for (int i = 0; i < attempts; i++) {
        if (backplane_write32(addr, val, (i == attempts - 1) ? console_ep : 0)) return true;
        for (int y = 0; y < 50000; y++) seL4_Yield();
    }
    return false;
}

static bool backplane_read32_retry(uint32_t addr, uint32_t *out, seL4_CPtr console_ep, int attempts = 3) {
    for (int i = 0; i < attempts; i++) {
        if (backplane_read32(addr, out, (i == attempts - 1) ? console_ep : 0)) return true;
        for (int y = 0; y < 50000; y++) seL4_Yield();
    }
    return false;
}

static bool sdpcm_wait_and_read_ctrl(seL4_CPtr console_ep, uint8_t **out_payload, uint32_t *out_len) {
    uint64_t freq = wifi_read_cntfrq();
    uint64_t start = wifi_read_cntvct();
    uint64_t timeout_ticks = (freq * WIFI_SDPCM_TIMEOUT_US) / 1000000ull;
    uint64_t diag_interval_ticks = (freq * 250000ull) / 1000000ull; // 250мс
    uint64_t next_diag = start + diag_interval_ticks;
    uint32_t last_intstatus_seen = 0xFFFFFFFFu; // заведомо невозможное значение — гарантированно напечатать первый раз
    // ДИАГНОСТИКА показала: intstatus стабильно НЕ показывает HOST_INT/
    // FRAME_IND в течение ВСЕГО ожидания (даже 15с), но ответ на ЭТОТ же
    // запрос находился в F2 почти сразу после отправки СЛЕДУЮЩЕГО запроса —
    // т.е. не зависит от прошедшего времени вообще. Похоже, intstatus просто
    // не отражает готовность кадра надёжно на этом железе (ещё одна
    // аппаратная особенность, как и вся история с F1-доступом после enable
    // F2). Раз чистый polling всё равно не полагается на настоящие
    // прерывания, подстраховываемся: помимо intstatus-триггера, СПЕКУЛЯТИВНО
    // пробуем прямое чтение F2 раз в ~300мс НЕЗАВИСИМО от intstatus — если
    // кадра там ещё нет, чтение просто вернёт мусор/пустоту (не аппаратная
    // ошибка), проверка чек-суммы отбросит это молча, и мы продолжим ждать.
    uint64_t poll_interval_ticks = (freq * 300000ull) / 1000000ull; // 300мс
    uint64_t next_poll = start;

    while (true) {
        uint32_t intstatus = 0;
        bool have_intstatus = backplane_read32_retry(g_sdio_core.base + SDPCMD_INTSTATUS, &intstatus, 0);
        uint64_t now = wifi_read_cntvct();
        if (have_intstatus && (intstatus != last_intstatus_seen || now >= next_diag)) {
            sys_puthex32(console_ep, "[WIFI][SDPCM] диагностика: intstatus = ", intstatus);
            last_intstatus_seen = intstatus;
            next_diag = now + diag_interval_ticks;
        }

        bool signaled = have_intstatus && (intstatus & (I_HMB_HOST_INT | I_HMB_FRAME_IND));
        bool speculative_turn = (now >= next_poll);

        if (signaled || speculative_turn) {
            if (signaled) {
                backplane_write32_retry(g_sdio_core.base + SDPCMD_INTSTATUS, intstatus & (I_HMB_HOST_INT | I_HMB_FRAME_IND), 0);
                if (intstatus & I_HMB_HOST_INT) {
                    uint32_t mbdata = 0;
                    backplane_read32_retry(g_sdio_core.base + SDPCMD_TOHOSTMAILBOXDATA, &mbdata, 0);
                    backplane_write32_retry(g_sdio_core.base + SDPCMD_TOSBMAILBOX, SMB_INT_ACK, 0);
                    if (mbdata & HMB_DATA_FWHALT) {
                        sys_puts(console_ep, "[WIFI][SDPCM] FAIL: прошивка сообщила FWHALT\n");
                        return false;
                    }
                }
            }
            if (speculative_turn) next_poll = now + poll_interval_ticks;

            if (sdio_f2_read(0, g_sdpcm_rx_buf, BRCMF_FIRSTREAD)) {
                uint16_t len16 = (uint16_t)(g_sdpcm_rx_buf[0] | (g_sdpcm_rx_buf[1] << 8));
                uint16_t chk16 = (uint16_t)(g_sdpcm_rx_buf[2] | (g_sdpcm_rx_buf[3] << 8));
                bool checksum_ok = (uint16_t)~(len16 ^ chk16) == 0;
                if (checksum_ok && len16 >= SDPCM_HDRLEN) {
                    uint8_t channel = g_sdpcm_rx_buf[5] & 0x0F;
                    uint8_t dat_offset = g_sdpcm_rx_buf[7];
                    if (channel == SDPCM_CONTROL_CHANNEL && dat_offset >= SDPCM_HDRLEN && dat_offset <= len16) {
                        if (len16 > BRCMF_FIRSTREAD) {
                            uint32_t remaining = len16 - BRCMF_FIRSTREAD;
                            uint32_t remaining_padded = (remaining + 3u) & ~3u;
                            if (BRCMF_FIRSTREAD + remaining_padded > WIFI_SDPCM_RX_BUF_CAP) {
                                sys_puts(console_ep, "[WIFI][SDPCM] FAIL: ответ не влезает в RX-буфер\n");
                                return false;
                            }
                            if (!sdio_f2_read(console_ep, g_sdpcm_rx_buf + BRCMF_FIRSTREAD, remaining_padded)) {
                                sys_puts(console_ep, "[WIFI][SDPCM] FAIL: чтение остатка кадра\n");
                                return false;
                            }
                        }
                        *out_payload = g_sdpcm_rx_buf + dat_offset;
                        *out_len = len16 - dat_offset;
                        return true;
                    }
                    // channel != CONTROL или некорректный dat_offset — событие/данные, не наш ответ, ждём дальше молча
                }
                // чек-сумма не сошлась/len16==0/короче заголовка — считаем это "кадра ещё нет" (особенно для
                // спекулятивных попыток без сигнала от intstatus), не фатально, просто ждём дальше молча
            }
        }

        if (wifi_read_cntvct() - start >= timeout_ticks) {
            sys_puts(console_ep, "[WIFI][SDPCM] FAIL: таймаут ожидания ответа\n");
            return false;
        }
        seL4_Yield();
    }
}

// --- CDC/BDC dcmd (16 байт: cmd/len/flags/status, все little-endian) поверх
// sdpcm control-канала. GET_VAR-запросы (iovar) — это обычный dcmd
// (cmd=BRCMF_C_GET_VAR) с payload = "имя\0" + запас под ответ; вызывающий
// заранее кладёт имя в начало buf и обнуляет остаток до buf_len==req_len
// (именно столько байт реально едет по проводу и указывается в dcmd.len —
// прошивка использует dcmd.len, чтобы знать, сколько места у неё есть под
// ответ). ---
constexpr uint32_t WIFI_IOCTL_FRAME_CAP = 400; // 16(dcmd) + payload/ответ (BRCMF_DCMD_SMLEN=256 с запасом)
alignas(4) static uint8_t g_ioctl_frame[WIFI_IOCTL_FRAME_CAP];
static uint16_t g_bcdc_reqid = 0;

static bool wifi_ioctl(seL4_CPtr console_ep, uint32_t cmd, bool is_set,
                        uint8_t *inout_buf, uint32_t inout_cap, uint32_t req_len, uint32_t *out_len) {
    uint32_t frame_len = 16 + req_len;
    if (frame_len > WIFI_IOCTL_FRAME_CAP || req_len > inout_cap) {
        sys_puts(console_ep, "[WIFI][IOCTL] FAIL: запрос не влезает в буфер\n");
        return false;
    }
    uint32_t reqid = (uint32_t)(++g_bcdc_reqid);
    uint32_t flags = (reqid << BCDC_DCMD_ID_SHIFT) | (is_set ? BCDC_DCMD_SET : 0u);

    g_ioctl_frame[0] = (uint8_t)(cmd & 0xFF);
    g_ioctl_frame[1] = (uint8_t)((cmd >> 8) & 0xFF);
    g_ioctl_frame[2] = (uint8_t)((cmd >> 16) & 0xFF);
    g_ioctl_frame[3] = (uint8_t)((cmd >> 24) & 0xFF);
    g_ioctl_frame[4] = (uint8_t)(req_len & 0xFF);
    g_ioctl_frame[5] = (uint8_t)((req_len >> 8) & 0xFF);
    g_ioctl_frame[6] = 0; g_ioctl_frame[7] = 0;
    g_ioctl_frame[8]  = (uint8_t)(flags & 0xFF);
    g_ioctl_frame[9]  = (uint8_t)((flags >> 8) & 0xFF);
    g_ioctl_frame[10] = (uint8_t)((flags >> 16) & 0xFF);
    g_ioctl_frame[11] = (uint8_t)((flags >> 24) & 0xFF);
    g_ioctl_frame[12] = 0; g_ioctl_frame[13] = 0; g_ioctl_frame[14] = 0; g_ioctl_frame[15] = 0; // status

    for (uint32_t i = 0; i < req_len; i++) g_ioctl_frame[16 + i] = inout_buf[i];

    sys_puthexbuf(console_ep, "[WIFI][IOCTL] диагностика: dcmd+payload = ", g_ioctl_frame, frame_len);

    if (!sdpcm_send_ctrl(console_ep, g_ioctl_frame, frame_len)) return false;

    // Ответ на ПРЕДЫДУЩИЙ запрос (протухший по таймауту раньше) может всё
    // ещё сидеть в очереди F2 и прийти раньше ответа на текущий — подтверждено
    // на живом железе (GET_VERSION истёк по таймауту, а его ответ подобрался
    // уже во время ожидания "ver"). Реальный драйвер (brcmf_proto_bcdc_
    // query_dcmd) в этой ситуации просто продолжает ждать следующий кадр, а
    // не считает рассинхрон reqid фатальным — делаем так же, до 3 попыток.
    uint8_t *resp = nullptr;
    uint32_t resp_len = 0;
    uint32_t resp_flags = 0;
    uint32_t resp_status = 0;
    uint32_t resp_id = 0;
    for (int attempt = 0; attempt < 3; attempt++) {
        if (!sdpcm_wait_and_read_ctrl(console_ep, &resp, &resp_len)) return false;
        if (resp_len < 16) {
            sys_puts(console_ep, "[WIFI][IOCTL] FAIL: ответ короче dcmd-заголовка\n");
            return false;
        }
        resp_flags  = (uint32_t)resp[8] | ((uint32_t)resp[9] << 8) | ((uint32_t)resp[10] << 16) | ((uint32_t)resp[11] << 24);
        resp_status = (uint32_t)resp[12] | ((uint32_t)resp[13] << 8) | ((uint32_t)resp[14] << 16) | ((uint32_t)resp[15] << 24);
        resp_id = (resp_flags >> BCDC_DCMD_ID_SHIFT) & 0xFFFFu;
        if (resp_id == reqid) break;
        sys_puthex32(console_ep, "[WIFI][IOCTL] WARN: reqid не совпал (устаревший ответ), ожидали ", reqid);
        sys_puthex32(console_ep, "[WIFI][IOCTL]   получили ", resp_id);
        if (attempt == 2) {
            sys_puts(console_ep, "[WIFI][IOCTL] FAIL: не дождались ответа с нужным reqid\n");
            return false;
        }
    }
    if (resp_flags & BCDC_DCMD_ERROR) {
        sys_puthex32(console_ep, "[WIFI][IOCTL] FAIL: прошивка вернула ошибку, status=", resp_status);
        return false;
    }

    uint32_t payload_len = resp_len - 16;
    if (payload_len > inout_cap) payload_len = inout_cap;
    for (uint32_t i = 0; i < payload_len; i++) inout_buf[i] = resp[16 + i];
    if (out_len) *out_len = payload_len;
    return true;
}

// Ответ на IOCTL перезаписывает тот же буфер, что и запрос — общий, статика
// (не стек), с запасом под "ver"-строку (BRCMF_DCMD_SMLEN=256 в эталоне).
alignas(4) static uint8_t g_ioctl_iobuf[256];
static bool g_wifi_sdpcm_ok = false;

// Полная оркестровка Милстоуна 4.3 — вызывается из main() ПОСЛЕ успешного
// Милстоуна 4.2 (прошивка уже запущена и подтверждена живой).
static void wifi_sdpcm_bringup(seL4_CPtr console_ep) {
    // ИСПРАВЛЕНО (снова): изначально убрал запрос HT, посчитав, что он нужен
    // только для распространения настоящего IRQ (у нас его нет — чистый
    // polling). Но исследование эталона (brcmf_sdio_firmware_callback,
    // sdio.c) показало: комментарий "needed to generate F2 interrupt"/"be
    // sure F2 interrupt propagates" — про то, что БЭКПЛЕЙН-КЛОК, питающий
    // саму FIFO функции 2, должен реально работать на HT, а не про физическую
    // линию IRQ как таковую. Именно этого клока не хватало — F2 CMD53 стабильно
    // проваливался с Command Timeout+CRC Error (карта вообще не отвечает на
    // уровне шины) ровно потому, что backplane, питающий F2, не был поднят
    // до HT. Раньше запрос HT вешал DAT0 — но тогда ещё не было общего
    // восстановления линии (wifi_reset_cmd_data_lines() при любой реальной
    // ошибке/застревании, см. wifi_wait_status_clear/wifi_wait_irpt_bit) —
    // пробуем снова, уже с этой защитой.
    sys_puts(console_ep, "[WIFI][SDPCM] step: запрос тактовой частоты чипа (HT, нужен backplane'у F2)...\n");
    uint8_t saveclk = 0;
    bool have_saveclk = wifi_request_ht_clock(console_ep, &saveclk);
    if (!have_saveclk) {
        sys_puts(console_ep, "[WIFI][SDPCM] WARN: HT не подтвердился, пробуем дальше на ALP\n");
    } else {
        // Форсируем HT ПОВЕРХ сохранённого значения на время настройки F2 —
        // "Force clocks on backplane to be sure F2 interrupt propagates"
        // (сам эталон восстанавливает saveclk обратно уже ПОСЛЕ watermark/MES,
        // см. ниже) — если просто оставить force навсегда, PMU может никогда
        // не вернуться в энергосберегающий режим, но это не должно ломать сам
        // протокол; порядок как в эталоне на всякий случай.
        if (!sdio_f1_write_byte(SBSDIO_FUNC1_CHIPCLKCSR, saveclk | SBSDIO_FORCE_HT, console_ep)) {
            sys_puts(console_ep, "[WIFI][SDPCM] WARN: force HT (CHIPCLKCSR) не прошёл\n");
        }
    }

    // ПЕРЕСТАВЛЕНО РАНЬШЕ enable F2 (эталон делает это ПОСЛЕ, но на этом
    // железе прямые F1 CMD52-ЗАПИСИ стабильно ловят Command Timeout, если
    // выполняются ПОСЛЕ enable F2 — независимо от конкретного регистра
    // (проверено на CHIPCLKCSR-restore, watermark, MESBUSYCTRL и даже на
    // SBSDIO_FUNC1_SLEEPCSR/KSO, который по спецификации должен быть в
    // "always-on" домене). До enable F2 такие записи (см. HT-force чуть
    // выше) проходят надёжно. Watermark/MES управляют порогом FIFO функции 2
    // — судя по всему, не менее важны для доставки данных до прошивки, чем
    // сама передача по шине: F2-запись сама по себе может успешно завершиться
    // на уровне SDIO, но прошивка не увидит кадр, если FIFO не настроен
    // правильно. Пробуем настроить его здесь, пока прямые F1-записи ещё
    // работают, вместо того чтобы пропускать целиком.
    sys_puts(console_ep, "[WIFI][SDPCM] step: watermark/MES (до enable F2)...\n");
    if (!sdio_f1_write_byte(SBSDIO_WATERMARK, CY_43455_F2_WATERMARK, console_ep)) {
        sys_puts(console_ep, "[WIFI][SDPCM] WARN: watermark не прошёл, продолжаем best-effort\n");
    } else {
        uint8_t devctl = 0;
        if (sdio_f1_read_byte(SBSDIO_DEVICE_CTL, &devctl, console_ep)) {
            sdio_f1_write_byte(SBSDIO_DEVICE_CTL, devctl | SBSDIO_DEVCTL_F2WM_ENAB, console_ep);
        }
        sdio_f1_write_byte(SBSDIO_FUNC1_MESBUSYCTRL, CY_43455_MESBUSYCTRL, console_ep);
    }

    sys_puts(console_ep, "[WIFI][SDPCM] step: tosbmailboxdata (версия sdpcm, до enable F2)...\n");
    if (!backplane_write32_retry(g_sdio_core.base + SDPCMD_TOSBMAILBOXDATA, SDPCM_PROT_VERSION << SMB_DATA_VERSION_SHIFT, console_ep)) {
        sys_puts(console_ep, "[WIFI][SDPCM] FAIL: запись tosbmailboxdata\n");
        return;
    }

    sys_puts(console_ep, "[WIFI][SDPCM] step: enable SDIO func2...\n");
    if (!sdio_enable_func(SBSDIO_FUNC_2, console_ep)) return;

    sys_puts(console_ep, "[WIFI][SDPCM] step: согласование блок-размера func2 (512 байт)...\n");
    if (!wifi_set_func2_blocksize(console_ep)) return;

    // ДИАГНОСТИКА: первая же команда СРАЗУ после enable F2 стабильно ловила
    // Command Timeout Error (карта вообще не отвечает) — не CRC, что похоже
    // на "чип занят" на короткое время сразу после включения функции, а не
    // структурную ошибку протокола. Даём честную реальную паузу перед тем,
    // как трогать что-либо ещё.
    sys_puts(console_ep, "[WIFI][SDPCM] step: пауза после enable F2 (settle)...\n");
    {
        uint64_t freq = wifi_read_cntfrq();
        uint64_t start = wifi_read_cntvct();
        uint64_t settle_ticks = (freq * 50000ull) / 1000000ull; // 50мс
        while (wifi_read_cntvct() - start < settle_ticks) seL4_Yield();
    }

    // ДИАГНОСТИКА: та же самая команда, что уже проверенно работала в
    // Милстоуне 4.1 (CMD52, чтение F0 CCCR offset 0x00) — если ОНА тоже
    // начинает падать после enable F2, значит после включения F2 ломается
    // вообще любая команда на шине, а не что-то конкретное про F2/backplane.
    {
        uint32_t cccr_arg = (SDIO_FUNC_0 << SDIO_ARG_FUNC_SHIFT) | (SDIO_CCCR_CCCR_OFFSET << SDIO_ARG_REG_ADDR_SHIFT);
        if (!sdio_send_cmd(EMMC_CMD_RSPNS_48, SDIO_CMD_RW_DIRECT, cccr_arg, console_ep)) {
            sys_puts(console_ep, "[WIFI][SDPCM] FAIL: контрольное чтение F0 CCCR после enable F2 НЕ ПРОШЛО — шина сломана целиком\n");
        } else {
            uint32_t cccr_resp = *wifi_reg(EMMC_RESP0_OFFSET) & 0xFFu;
            sys_puthex32(console_ep, "[WIFI][SDPCM] контрольное чтение F0 CCCR после enable F2 = ", cccr_resp);
        }
    }

    // hostintmask (CMD53 через backplane-окно) — стабильно проходит, оставляем.
    sys_puts(console_ep, "[WIFI][SDPCM] step: hostintmask...\n");
    if (!backplane_write32_retry(g_sdio_core.base + SDPCMD_HOSTINTMASK, HOSTINTMASK, console_ep)) {
        sys_puts(console_ep, "[WIFI][SDPCM] WARN: hostintmask не прошёл, продолжаем best-effort\n");
    }

    // ИСПЫТАНО И ОТКЛОНЕНО (для справки): гипотеза про KSO/автосон
    // (SBSDIO_FUNC1_SLEEPCSR, "always-on"-регистр) не подтвердилась на живом
    // железе — сама запись KSO ловила Command Timeout ТАК ЖЕ стабильно, как
    // watermark/CHIPCLKCSR-restore/MESBUSYCTRL, когда всё это пробовалось
    // ПОСЛЕ enable F2. watermark/MES теперь настраиваются раньше (см. выше,
    // перед tosbmailboxdata) — именно там прямые F1-записи ещё надёжны.

    // ВОЗВРАЩЕНО: раньше пропускали восстановление CHIPCLKCSR=saveclk, потому
    // что ЛЮБАЯ прямая F1 CMD52-запись после enable F2 стабильно ловила
    // Command Timeout. Настоящая причина найдена и исправлена: sdio_enable_
    // func() перезаписывал CCCR IOEx одним битом БЕЗ read-modify-write,
    // сбрасывая уже включённый бит функции 1 — после фикса backplane-чтения
    // снова надёжны (подтверждено: tohostmailboxdata читается, FWREADY
    // виден). Пробуем восстановление снова — permanently forced HT (без
    // восстановления в энергосберегающий режим) мог быть как раз причиной
    // непредсказуемо долгих ответов прошивки на первый control-запрос
    // (PMU/clock state machine прошивки могла быть в замешательстве).
    // Best-effort, как и остальные шаги здесь.
    if (have_saveclk) {
        sys_puts(console_ep, "[WIFI][SDPCM] step: восстановление CHIPCLKCSR=saveclk...\n");
        if (!sdio_f1_write_byte(SBSDIO_FUNC1_CHIPCLKCSR, saveclk, console_ep)) {
            sys_puts(console_ep, "[WIFI][SDPCM] WARN: восстановление CHIPCLKCSR не прошло, продолжаем best-effort\n");
        }
    }

    // Реальный драйвер узнаёт о готовности прошивки к F2-трафику по биту
    // HMB_DATA_DEVREADY/FWREADY в tohostmailboxdata (обычно приходит вместе с
    // прерыванием сразу после того, как прошивка закончит собственную
    // инициализацию bus-уровня) — мы до сих пор нигде это не проверяли,
    // просто сразу пробовали слать IOCTL. Даём честный шанс (до ~2с реального
    // времени) увидеть этот бит, прежде чем пробовать GET_VERSION — если
    // прошивке действительно нужно было чуть больше времени на собственную
    // sdpcm-инициализацию, чем на то, чтобы просто напечатать что-то в
    // консоль (см. Милстоун 4.2), это должно проявиться здесь.
    sys_puts(console_ep, "[WIFI][SDPCM] step: ожидание готовности прошивки (tohostmailboxdata)...\n");
    {
        // ДИАГНОСТИКА: первое чтение — С console_ep, чтобы отличить "реально
        // 0" от "тихо падает" (весь остальной цикл ниже читает молча,
        // console_ep=0, чтобы не спамить — сюда же нужна хоть одна честная
        // проверка, работает ли backplane-чтение ПОСЛЕ enable F2, но ДО
        // какого-либо трафика по F2, раз именно после F2-трафика backplane-
        // чтения стабильно ловили Data Timeout Error).
        uint32_t probe_mbdata = 0;
        if (!backplane_read32_retry(g_sdio_core.base + SDPCMD_TOHOSTMAILBOXDATA, &probe_mbdata, console_ep)) {
            sys_puts(console_ep, "[WIFI][SDPCM] диагностика: первое чтение tohostmailboxdata НЕ ПРОШЛО\n");
        } else {
            sys_puthex32(console_ep, "[WIFI][SDPCM] диагностика: первое чтение tohostmailboxdata = ", probe_mbdata);
        }

        uint64_t freq = wifi_read_cntfrq();
        uint64_t start = wifi_read_cntvct();
        uint64_t timeout_ticks = (freq * 2000000ull) / 1000000ull; // 2с
        bool seen_ready = false;
        while (wifi_read_cntvct() - start < timeout_ticks) {
            uint32_t mbdata = 0;
            if (backplane_read32_retry(g_sdio_core.base + SDPCMD_TOHOSTMAILBOXDATA, &mbdata, 0) && mbdata != 0) {
                sys_puthex32(console_ep, "[WIFI][SDPCM] tohostmailboxdata = ", mbdata);
                if (mbdata & (HMB_DATA_DEVREADY | HMB_DATA_FWREADY)) { seen_ready = true; break; }
            }
            seL4_Yield();
        }
        if (!seen_ready) sys_puts(console_ep, "[WIFI][SDPCM] WARN: не увидели DEVREADY/FWREADY за 2с, пробуем IOCTL всё равно\n");
    }

    // Эталон (brcmf_sdio_bus_preinit(), sdio.c) отправляет "bus:txglom"=0
    // (SET iovar) буквально ПЕРВЫМ control-запросом за всю сессию, ДО
    // cur_etheraddr/GET_REVINFO и уж тем более до наших смок-тестов —
    // отключает scatter-gather TX glomming, которого у нас всё равно нет.
    // Не является предпосылкой готовности прошивки к остальным dcmd'ам (per
    // research), но раз это буквально первый запрос эталона — отправляем и
    // мы первым, для полного соответствия порядку.
    sys_puts(console_ep, "[WIFI][SDPCM] step: iovar \"bus:txglom\"=0 (первый control-запрос, как в эталоне)...\n");
    {
        const char *name = "bus:txglom";
        uint32_t ni = 0;
        while (name[ni]) { g_ioctl_iobuf[ni] = (uint8_t)name[ni]; ni++; }
        g_ioctl_iobuf[ni] = 0; ni++; // NUL-терминатор
        g_ioctl_iobuf[ni] = 0; g_ioctl_iobuf[ni+1] = 0; g_ioctl_iobuf[ni+2] = 0; g_ioctl_iobuf[ni+3] = 0; // value=0, LE
        uint32_t txglom_len = ni + 4;
        uint32_t txglom_out_len = 0;
        if (wifi_ioctl(console_ep, BRCMF_C_SET_VAR, true, g_ioctl_iobuf, sizeof(g_ioctl_iobuf), txglom_len, &txglom_out_len)) {
            sys_puts(console_ep, "[WIFI][SDPCM] \"bus:txglom\" SET OK\n");
        } else {
            sys_puts(console_ep, "[WIFI][SDPCM] WARN: \"bus:txglom\" SET не прошёл, продолжаем best-effort\n");
        }
    }

    // ИСПРАВЛЕНО: req_len=0 означало dcmd.len=0 — прошивка получала "у тебя
    // 0 байт буфера под ответ" и, судя по всему, поэтому вообще не отвечала
    // (не проверяли раньше: F2 bit никогда не загорался). dcmd.len для GET-
    // команд — это заявленная ёмкость буфера ПОД ОТВЕТ, а не длина реального
    // payload'а запроса (у GET_VERSION запрос вообще без параметров) — как и
    // "ver" ниже, должны честно объявить место под ответ (4 байта — размер
    // uint32-версии).
    sys_puts(console_ep, "[WIFI][SDPCM] step: IOCTL GET_VERSION (смок-тест #1)...\n");
    for (uint32_t i = 0; i < sizeof(g_ioctl_iobuf); i++) g_ioctl_iobuf[i] = 0;
    uint32_t out_len = 0;
    if (wifi_ioctl(console_ep, BRCMF_C_GET_VERSION, false, g_ioctl_iobuf, sizeof(g_ioctl_iobuf), 4, &out_len)) {
        sys_puts(console_ep, "[WIFI][SDPCM] GET_VERSION OK\n");
        if (out_len >= 4) {
            uint32_t ver = (uint32_t)g_ioctl_iobuf[0] | ((uint32_t)g_ioctl_iobuf[1] << 8) |
                           ((uint32_t)g_ioctl_iobuf[2] << 16) | ((uint32_t)g_ioctl_iobuf[3] << 24);
            sys_puthex32(console_ep, "[WIFI][SDPCM] GET_VERSION value = ", ver);
        }
        g_wifi_sdpcm_ok = true;
    } else {
        sys_puts(console_ep, "[WIFI][SDPCM] GET_VERSION FAILED\n");
    }

    sys_puts(console_ep, "[WIFI][SDPCM] step: IOCTL GET_VAR \"ver\" (смок-тест #2)...\n");
    for (uint32_t i = 0; i < sizeof(g_ioctl_iobuf); i++) g_ioctl_iobuf[i] = 0;
    const char *name = "ver";
    uint32_t ni = 0;
    while (name[ni]) { g_ioctl_iobuf[ni] = (uint8_t)name[ni]; ni++; }
    g_ioctl_iobuf[ni] = 0; // NUL-терминатор имени iovar'а
    out_len = 0;
    // req_len = ВЕСЬ буфер (не только "ver\0") — прошивка использует dcmd.len,
    // чтобы знать, сколько места у неё есть под ответ (см. план, п.4/6).
    if (wifi_ioctl(console_ep, BRCMF_C_GET_VAR, false, g_ioctl_iobuf, sizeof(g_ioctl_iobuf), (uint32_t)sizeof(g_ioctl_iobuf), &out_len)) {
        g_ioctl_iobuf[sizeof(g_ioctl_iobuf) - 1] = 0; // защититься от неNUL-терминированного ответа
        sys_puts(console_ep, "[WIFI][SDPCM] \"ver\" = \"");
        sys_puts(console_ep, (const char*)g_ioctl_iobuf);
        sys_puts(console_ep, "\"\n");
        g_wifi_sdpcm_ok = true;
    } else {
        sys_puts(console_ep, "[WIFI][SDPCM] \"ver\" iovar FAILED\n");
    }
}

// =====================================================================
// Милстоун 4.4 — подключение к точке доступа (SSID/WPA2-PSK). Сам 4-way
// handshake и 802.11 MLME выполняет прошивка чипа, хосту не нужно ничего
// этого реализовывать — НО, вопреки первоначальному предположению из
// ROADMAP.md, прошивка (в этой версии эталонного драйвера) НЕ умеет сама
// превращать ASCII-пароль в PMK: `brcmf_set_pmk()` в эталоне
// (cfg80211.c) всегда шлёт уже готовый 32-байтный PMK с flags=0, а
// PBKDF2-HMAC-SHA1(passphrase, SSID, 4096) в реальном Linux-стеке делает
// userspace (wpa_supplicant) ДО драйвера. Значит PBKDF2 нужно сделать
// здесь, на хосте, самим — см. SHA1/HMAC/PBKDF2 ниже.
// =====================================================================

// --- SHA1 (FIPS 180-1) — простой "всё за один вызов" вариант (без
// потокового API): наши сообщения всегда маленькие (HMAC/PBKDF2 внутри
// используют блоки по ~20-100 байт), стриминг не нужен. ---
static void sha1_transform(uint32_t state[5], const uint8_t block[64]) {
    uint32_t w[80];
    for (int i = 0; i < 16; i++) {
        w[i] = ((uint32_t)block[i * 4] << 24) | ((uint32_t)block[i * 4 + 1] << 16) |
               ((uint32_t)block[i * 4 + 2] << 8) | ((uint32_t)block[i * 4 + 3]);
    }
    for (int i = 16; i < 80; i++) {
        uint32_t v = w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16];
        w[i] = (v << 1) | (v >> 31);
    }
    uint32_t a = state[0], b = state[1], c = state[2], d = state[3], e = state[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20)      { f = (b & c) | ((~b) & d);      k = 0x5A827999u; }
        else if (i < 40) { f = b ^ c ^ d;                 k = 0x6ED9EBA1u; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDCu; }
        else             { f = b ^ c ^ d;                 k = 0xCA62C1D6u; }
        uint32_t temp = ((a << 5) | (a >> 27)) + f + e + k + w[i];
        e = d; d = c; c = (b << 30) | (b >> 2); b = a; a = temp;
    }
    state[0] += a; state[1] += b; state[2] += c; state[3] += d; state[4] += e;
}

// len ограничена практическим использованием ниже (HMAC-ключи/сообщения,
// максимум ~128 байт) — буфер хвоста/паддинга размером 128 байт с запасом.
static void sha1(const uint8_t *data, uint32_t len, uint8_t out[20]) {
    uint32_t state[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};
    uint64_t bitlen = (uint64_t)len * 8u;
    uint32_t full_blocks = len / 64u;
    for (uint32_t b = 0; b < full_blocks; b++) sha1_transform(state, data + b * 64u);

    uint8_t buf[128];
    uint32_t rem = len - full_blocks * 64u;
    for (uint32_t i = 0; i < rem; i++) buf[i] = data[full_blocks * 64u + i];
    buf[rem] = 0x80u;
    uint32_t padlen = rem + 1u;
    uint32_t total_blocks = (padlen + 8u <= 64u) ? 1u : 2u;
    uint32_t total_len = total_blocks * 64u;
    for (uint32_t i = padlen; i < total_len - 8u; i++) buf[i] = 0;
    for (int i = 0; i < 8; i++) buf[total_len - 1 - i] = (uint8_t)(bitlen >> (i * 8));
    for (uint32_t b = 0; b < total_blocks; b++) sha1_transform(state, buf + b * 64u);

    for (int i = 0; i < 5; i++) {
        out[i * 4]     = (uint8_t)(state[i] >> 24);
        out[i * 4 + 1] = (uint8_t)(state[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(state[i] >> 8);
        out[i * 4 + 3] = (uint8_t)(state[i]);
    }
}

// --- HMAC-SHA1 (RFC 2104). key_len до 64 байт покрывает пароль (максимум
// 63 по спецификации WPA2) без необходимости хешировать сам ключ. ---
static void hmac_sha1(const uint8_t *key, uint32_t key_len,
                       const uint8_t *msg, uint32_t msg_len, uint8_t out[20]) {
    uint8_t k[64];
    if (key_len > 64u) {
        uint8_t kh[20];
        sha1(key, key_len, kh);
        for (uint32_t i = 0; i < 20u; i++) k[i] = kh[i];
        for (uint32_t i = 20u; i < 64u; i++) k[i] = 0;
    } else {
        uint32_t i = 0;
        for (; i < key_len; i++) k[i] = key[i];
        for (; i < 64u; i++) k[i] = 0;
    }
    uint8_t ipad[64], opad[64];
    for (int i = 0; i < 64; i++) { ipad[i] = (uint8_t)(k[i] ^ 0x36u); opad[i] = (uint8_t)(k[i] ^ 0x5Cu); }

    // msg_len здесь никогда не превышает ~36 байт (SSID(32)+4-байтный
    // счётчик PBKDF2, либо 20-байтный промежуточный хеш) — 128 байт с запасом.
    uint8_t inner_buf[64 + 128];
    for (int i = 0; i < 64; i++) inner_buf[i] = ipad[i];
    for (uint32_t i = 0; i < msg_len; i++) inner_buf[64 + i] = msg[i];
    uint8_t inner_hash[20];
    sha1(inner_buf, 64u + msg_len, inner_hash);

    uint8_t outer_buf[64 + 20];
    for (int i = 0; i < 64; i++) outer_buf[i] = opad[i];
    for (int i = 0; i < 20; i++) outer_buf[64 + i] = inner_hash[i];
    sha1(outer_buf, 64u + 20u, out);
}

// --- PBKDF2-HMAC-SHA1 (RFC 2898) — используется WPA2-PSK для превращения
// passphrase+SSID в 256-битный PMK, 4096 итераций (802.11i). ---
static void pbkdf2_hmac_sha1(const uint8_t *password, uint32_t password_len,
                              const uint8_t *salt, uint32_t salt_len,
                              uint32_t iterations, uint8_t *out, uint32_t dklen) {
    constexpr uint32_t HLEN = 20;
    uint32_t blocks = (dklen + HLEN - 1u) / HLEN;
    uint8_t salt_buf[36]; // SSID (<=32) + 4-байтный BE-счётчик блока
    for (uint32_t block_idx = 1; block_idx <= blocks; block_idx++) {
        for (uint32_t i = 0; i < salt_len; i++) salt_buf[i] = salt[i];
        salt_buf[salt_len + 0] = (uint8_t)(block_idx >> 24);
        salt_buf[salt_len + 1] = (uint8_t)(block_idx >> 16);
        salt_buf[salt_len + 2] = (uint8_t)(block_idx >> 8);
        salt_buf[salt_len + 3] = (uint8_t)(block_idx);

        uint8_t u[20];
        hmac_sha1(password, password_len, salt_buf, salt_len + 4u, u); // U1
        uint8_t t[20];
        for (int i = 0; i < 20; i++) t[i] = u[i];
        for (uint32_t iter = 1; iter < iterations; iter++) {
            uint8_t u_next[20];
            hmac_sha1(password, password_len, u, 20u, u_next);
            for (int i = 0; i < 20; i++) { u[i] = u_next[i]; t[i] = (uint8_t)(t[i] ^ u_next[i]); }
        }
        uint32_t offset = (block_idx - 1u) * HLEN;
        uint32_t copy_len = (offset + HLEN <= dklen) ? HLEN : (dklen - offset);
        for (uint32_t i = 0; i < copy_len; i++) out[offset + i] = t[i];
    }
}

static void sys_puthexbyte(seL4_CPtr console_ep, uint8_t v) {
    char hex[3];
    hex[0] = "0123456789abcdef"[(v >> 4) & 0xF];
    hex[1] = "0123456789abcdef"[v & 0xF];
    hex[2] = 0;
    sys_puts(console_ep, hex);
}

// Самотест PBKDF2 по общеизвестному тестовому вектору 802.11i/WPA2
// (SSID="IEEE", passphrase="password" -> PMK=
// f42c6fc52df0ebef9ebb4b90b38a5f902e83fe1b135a70e23aed762e9710a12e),
// сверено отдельно через `hashlib.pbkdf2_hmac` перед написанием этого
// кода. Печатает вычисленный PMK и PASS/FAIL — чтобы отличить баг в
// крипто-примитивах от бага в самом join, если что-то пойдёт не так на
// живом железе.
static bool wifi_pbkdf2_selftest(seL4_CPtr console_ep) {
    static const uint8_t expected[32] = {
        0xf4, 0x2c, 0x6f, 0xc5, 0x2d, 0xf0, 0xeb, 0xef, 0x9e, 0xbb, 0x4b, 0x90, 0xb3, 0x8a, 0x5f, 0x90,
        0x2e, 0x83, 0xfe, 0x1b, 0x13, 0x5a, 0x70, 0xe2, 0x3a, 0xed, 0x76, 0x2e, 0x97, 0x10, 0xa1, 0x2e,
    };
    const uint8_t pass[] = "password";
    const uint8_t ssid[] = "IEEE";
    uint8_t pmk[32];
    pbkdf2_hmac_sha1(pass, 8, ssid, 4, 4096, pmk, 32);

    sys_puts(console_ep, "[WIFI][CRYPTO] PBKDF2 самотест: вычислено = ");
    for (int i = 0; i < 32; i++) sys_puthexbyte(console_ep, pmk[i]);
    sys_puts(console_ep, "\n");

    bool ok = true;
    for (int i = 0; i < 32; i++) if (pmk[i] != expected[i]) ok = false;
    sys_puts(console_ep, ok ? "[WIFI][CRYPTO] PBKDF2 самотест: PASS\n" : "[WIFI][CRYPTO] PBKDF2 самотест: FAIL\n");
    return ok;
}

// Прямой (не-iovar) dcmd с 4-байтным целым значением — BRCMF_C_UP/
// BRCMF_C_SET_PM ниже. НЕ ходит через "name\0данные" — просто payload
// целиком является значением.
static bool wifi_dcmd_set_int(seL4_CPtr console_ep, uint32_t cmd, uint32_t value) {
    for (int i = 0; i < 4; i++) g_ioctl_iobuf[i] = (uint8_t)((value >> (i * 8)) & 0xFF);
    uint32_t out_len = 0;
    return wifi_ioctl(console_ep, cmd, true, g_ioctl_iobuf, sizeof(g_ioctl_iobuf), 4u, &out_len);
}

// Буфер под приём кадров EVENT-канала — общий для escan-диагностики и
// ожидания результата join (ниже).
constexpr uint32_t WIFI_EVENT_RX_BUF_CAP = 512;
alignas(4) static uint8_t g_event_rx_buf[WIFI_EVENT_RX_BUF_CAP];

// ДИАГНОСТИКА: слепой escan (все каналы, все SSID) — не зависит от
// правильности SSID/PSK/join вообще, только от того, включено ли радио и
// делает ли прошивка ХОТЬ ЧТО-ТО в эфире. Если это тоже не даёт ни одного
// события — проблема выше join, в самом базовом bring-up радио/интерфейса.
static bool wifi_escan_start(seL4_CPtr console_ep) {
    const char *name = "escan";
    uint32_t ni = 0;
    while (name[ni]) { g_ioctl_iobuf[ni] = (uint8_t)name[ni]; ni++; }
    g_ioctl_iobuf[ni] = 0; ni++;
    uint32_t base = ni;
    for (uint32_t i = 0; i < BRCMF_ESCAN_BLIND_LEN; i++) g_ioctl_iobuf[base + i] = 0;

    // brcmf_escan_params_le: version(le32) + action(le16) + sync_id(le16)
    g_ioctl_iobuf[base + 0] = (uint8_t)(BRCMF_ESCAN_REQ_VERSION & 0xFF);
    g_ioctl_iobuf[base + 4] = (uint8_t)(WL_ESCAN_ACTION_START & 0xFF);
    g_ioctl_iobuf[base + 6] = 0x34; g_ioctl_iobuf[base + 7] = 0x12; // sync_id = 0x1234, произвольно

    // brcmf_scan_params_le (offset base+8): ssid_le=0 (все SSID, т.к. n_ssids=0
    // в этом упрощённом слепом варианте), bssid=broadcast, bss_type=ANY,
    // scan_type=ACTIVE, nprobes/active/passive/home=-1, channel_num=0 (все
    // каналы, без отдельного channel_list — n_channels=0 в этом варианте).
    uint32_t p = base + BRCMF_ESCAN_PARAMS_HDR_LEN;
    // ssid_le (36 байт) — уже занулено (означает "любой SSID")
    for (int i = 0; i < 6; i++) g_ioctl_iobuf[p + 36 + i] = 0xFF; // bssid broadcast
    g_ioctl_iobuf[p + 42] = (uint8_t)DOT11_BSSTYPE_ANY;   // bss_type (s8)
    g_ioctl_iobuf[p + 43] = (uint8_t)BRCMF_SCANTYPE_ACTIVE; // scan_type
    for (int f = 0; f < 4; f++) { // nprobes/active_time/passive_time/home_time = -1
        uint32_t off = p + 44 + (uint32_t)f * 4u;
        g_ioctl_iobuf[off+0]=0xFF; g_ioctl_iobuf[off+1]=0xFF; g_ioctl_iobuf[off+2]=0xFF; g_ioctl_iobuf[off+3]=0xFF;
    }
    // channel_num (offset p+60) = 0 — уже занулено (все каналы)

    uint32_t req_len = ni + BRCMF_ESCAN_BLIND_LEN;
    uint32_t out_len = 0;
    return wifi_ioctl(console_ep, BRCMF_C_SET_VAR, true, g_ioctl_iobuf, sizeof(g_ioctl_iobuf), req_len, &out_len);
}

// ДИАГНОСТИКА: печатает ЛЮБОЕ событие (не фильтруя по типу), которое видит
// за timeout_us — чтобы понять, делает ли прошивка вообще хоть что-то в
// эфире (при escan) независимо от корректности join-последовательности.
// Не завершает работу при первом событии — копит все события до таймаута,
// чтобы увидеть полную картину (escan обычно шлёт много PARTIAL + один
// финальный).
static void wifi_wait_and_dump_any_events(seL4_CPtr console_ep, uint32_t timeout_us) {
    uint64_t freq = wifi_read_cntfrq();
    uint64_t start = wifi_read_cntvct();
    uint64_t timeout_ticks = (freq * (uint64_t)timeout_us) / 1000000ull;
    uint64_t poll_interval_ticks = (freq * 200000ull) / 1000000ull; // 200мс
    uint64_t next_poll = start;
    uint32_t events_seen = 0;

    while (wifi_read_cntvct() - start < timeout_ticks) {
        uint64_t now = wifi_read_cntvct();
        if (now < next_poll) { seL4_Yield(); continue; }
        next_poll = now + poll_interval_ticks;

        if (!sdio_f2_read(0, g_event_rx_buf, BRCMF_FIRSTREAD)) continue;
        uint16_t len16 = (uint16_t)(g_event_rx_buf[0] | (g_event_rx_buf[1] << 8));
        uint16_t chk16 = (uint16_t)(g_event_rx_buf[2] | (g_event_rx_buf[3] << 8));
        if ((uint16_t)~(len16 ^ chk16) != 0 || len16 < SDPCM_HDRLEN) continue;
        uint8_t channel = g_event_rx_buf[5] & 0x0F;
        uint8_t dat_offset = g_event_rx_buf[7];
        if (channel != SDPCM_EVENT_CHANNEL || dat_offset < SDPCM_HDRLEN || dat_offset > len16) continue;
        if (len16 > BRCMF_FIRSTREAD) {
            uint32_t remaining = len16 - BRCMF_FIRSTREAD;
            uint32_t remaining_padded = (remaining + 3u) & ~3u;
            if (BRCMF_FIRSTREAD + remaining_padded > WIFI_EVENT_RX_BUF_CAP) continue;
            if (!sdio_f2_read(0, g_event_rx_buf + BRCMF_FIRSTREAD, remaining_padded)) continue;
        }
        uint32_t payload_off = dat_offset;
        uint32_t payload_len = len16 - dat_offset;
        if (payload_len < BCDC_HEADER_LEN) continue;
        uint8_t bcdc_data_offset = g_event_rx_buf[payload_off + 3];
        uint32_t hdr_skip = BCDC_HEADER_LEN + (uint32_t)bcdc_data_offset * 4u;
        if (payload_len < hdr_skip + BRCMF_EVENT_HDR_LEN) continue;
        uint32_t ev = payload_off + hdr_skip;
        uint32_t brcm_off = ev + ETHHDR_LEN;
        bool oui_ok = (g_event_rx_buf[brcm_off + 5] == 0x00 && g_event_rx_buf[brcm_off + 6] == 0x10 && g_event_rx_buf[brcm_off + 7] == 0x18);
        if (!oui_ok) continue;
        uint32_t msg_off = brcm_off + BRCM_ETHHDR_LEN;
        uint32_t event_type = ((uint32_t)g_event_rx_buf[msg_off+4]<<24)|((uint32_t)g_event_rx_buf[msg_off+5]<<16)|((uint32_t)g_event_rx_buf[msg_off+6]<<8)|(uint32_t)g_event_rx_buf[msg_off+7];
        uint32_t status = ((uint32_t)g_event_rx_buf[msg_off+8]<<24)|((uint32_t)g_event_rx_buf[msg_off+9]<<16)|((uint32_t)g_event_rx_buf[msg_off+10]<<8)|(uint32_t)g_event_rx_buf[msg_off+11];
        events_seen++;
        sys_puthex32(console_ep, "[WIFI][SCAN] событие: event_type = ", event_type);
        sys_puthex32(console_ep, "[WIFI][SCAN]           status     = ", status);
    }
    sys_puthex32(console_ep, "[WIFI][SCAN] всего событий увидено: ", events_seen);
}

// --- iovar-хелперы для join: обычный SET-iovar с 4-байтным целым значением
// (wsec/auth/wpa_auth/sup_wpa — всё это либо plain iovar, либо bsscfg-iovar,
// но при bsscfgidx==0 — единственный интерфейс в этом порте — оба сводятся
// к одному и тому же wire-формату name+NUL+данные через BRCMF_C_SET_VAR,
// см. fwil.c brcmf_create_iovar()/память проекта). Переиспользуют
// g_ioctl_iobuf (тот же буфер, что и GET_VERSION/"ver" в Милстоуне 4.3). ---
static bool wifi_iovar_set_int(seL4_CPtr console_ep, const char *name, uint32_t value) {
    uint32_t ni = 0;
    while (name[ni]) { g_ioctl_iobuf[ni] = (uint8_t)name[ni]; ni++; }
    g_ioctl_iobuf[ni] = 0; ni++;
    g_ioctl_iobuf[ni + 0] = (uint8_t)(value & 0xFF);
    g_ioctl_iobuf[ni + 1] = (uint8_t)((value >> 8) & 0xFF);
    g_ioctl_iobuf[ni + 2] = (uint8_t)((value >> 16) & 0xFF);
    g_ioctl_iobuf[ni + 3] = (uint8_t)((value >> 24) & 0xFF);
    uint32_t req_len = ni + 4u;
    uint32_t out_len = 0;
    return wifi_ioctl(console_ep, BRCMF_C_SET_VAR, true, g_ioctl_iobuf, sizeof(g_ioctl_iobuf), req_len, &out_len);
}

// SET_WSEC_PMK (raw dcmd 268) — struct brcmf_wsec_pmk_le: key_len(le16) +
// flags(le16, ВСЕГДА 0 — готовый 32-байтный бинарный PMK, не ASCII-пароль,
// см. заголовок секции выше) + key[65].
static bool wifi_set_wsec_pmk(seL4_CPtr console_ep, const uint8_t pmk[32]) {
    for (uint32_t i = 0; i < sizeof(g_ioctl_iobuf); i++) g_ioctl_iobuf[i] = 0;
    g_ioctl_iobuf[0] = (uint8_t)(BRCMF_WSEC_MAX_PSK_LEN & 0xFF);
    g_ioctl_iobuf[1] = (uint8_t)((BRCMF_WSEC_MAX_PSK_LEN >> 8) & 0xFF);
    g_ioctl_iobuf[2] = 0; g_ioctl_iobuf[3] = 0; // flags = 0
    for (uint32_t i = 0; i < 32u; i++) g_ioctl_iobuf[4 + i] = pmk[i];
    uint32_t out_len = 0;
    return wifi_ioctl(console_ep, BRCMF_C_SET_WSEC_PMK, true, g_ioctl_iobuf, sizeof(g_ioctl_iobuf),
                       BRCMF_WSEC_PMK_LE_LEN, &out_len);
}

// iovar "join" (bsscfg-iovar, при bsscfgidx==0 — тот же plain wire-формат)
// со struct brcmf_ext_join_params_le, ОБРЕЗАННой ровно так же, как в
// эталоне (cfg80211.c: offsetof(...,assoc_le)+offsetof(assoc_params_le,
// chanspec_list), т.е. без chanspec_list[] — канал не указываем, пусть
// прошивка сама сканирует все каналы). BSSID = широковещательный
// (FF:FF:FF:FF:FF:FF — eth_broadcast_addr() в эталоне, НЕ 00:00:00:00:00:00,
// как можно ошибочно подумать по устаревшему комментарию в самой структуре
// эталона), канал/сканирование — все параметры "-1" (использовать
// значения прошивки по умолчанию).
static bool wifi_join(seL4_CPtr console_ep, const char *ssid, uint32_t ssid_len) {
    const char *name = "join";
    uint32_t ni = 0;
    while (name[ni]) { g_ioctl_iobuf[ni] = (uint8_t)name[ni]; ni++; }
    g_ioctl_iobuf[ni] = 0; ni++;
    uint32_t base = ni;
    for (uint32_t i = 0; i < BRCMF_EXT_JOIN_PARAMS_LE_LEN; i++) g_ioctl_iobuf[base + i] = 0;

    // ssid_le: SSID_len(le32) + SSID[32]
    g_ioctl_iobuf[base + 0] = (uint8_t)(ssid_len & 0xFF);
    g_ioctl_iobuf[base + 1] = (uint8_t)((ssid_len >> 8) & 0xFF);
    g_ioctl_iobuf[base + 2] = (uint8_t)((ssid_len >> 16) & 0xFF);
    g_ioctl_iobuf[base + 3] = (uint8_t)((ssid_len >> 24) & 0xFF);
    for (uint32_t i = 0; i < ssid_len && i < IEEE80211_MAX_SSID_LEN; i++) {
        g_ioctl_iobuf[base + 4 + i] = (uint8_t)ssid[i];
    }

    // scan_le: scan_type=-1(0xFF), 3 байта паддинга (уже 0), nprobes/
    // active_time/passive_time/home_time = -1 (0xFFFFFFFF) — канал не
    // известен, разрешаем прошивке использовать её собственные значения по
    // умолчанию для скана всех каналов при join (см. эталон, cfg80211.c).
    uint32_t scan_off = base + BRCMF_SSID_LE_LEN;
    g_ioctl_iobuf[scan_off] = 0xFF;
    for (int f = 0; f < 4; f++) {
        uint32_t off = scan_off + 4u + (uint32_t)f * 4u;
        g_ioctl_iobuf[off + 0] = 0xFF; g_ioctl_iobuf[off + 1] = 0xFF;
        g_ioctl_iobuf[off + 2] = 0xFF; g_ioctl_iobuf[off + 3] = 0xFF;
    }

    // assoc_le (обрезано до chanspec_num включительно): bssid=broadcast,
    // 2 байта паддинга (уже 0), chanspec_num=0 (уже 0 — "все каналы").
    uint32_t assoc_off = scan_off + BRCMF_JOIN_SCAN_PARAMS_LE_LEN;
    for (int i = 0; i < 6; i++) g_ioctl_iobuf[assoc_off + i] = 0xFF;

    uint32_t req_len = ni + BRCMF_EXT_JOIN_PARAMS_LE_LEN;
    uint32_t out_len = 0;
    return wifi_ioctl(console_ep, BRCMF_C_SET_VAR, true, g_ioctl_iobuf, sizeof(g_ioctl_iobuf), req_len, &out_len);
}

// --- Ожидание результата join через sdpcm EVENT-канал (channel=1) —
// прежде этот проект вообще не читал ничего, кроме CONTROL-канала(0).
// Событие успеха — БОБА условия увидены (см. эталон, brcmf_is_linkup()):
// SET_SSID/SUCCESS (ассоциация прошла) И PSK_SUP/FWSUP_COMPLETED (4-way
// handshake завершён прошивкой). Полный layout заголовка события (все
// поля BIG-ENDIAN, в отличие от остального sdpcm/BCDC!) — см. platform.h. ---

enum WifiJoinResult { WIFI_JOIN_SUCCESS, WIFI_JOIN_FAILED, WIFI_JOIN_TIMEOUT };

static WifiJoinResult wifi_wait_for_join_result(seL4_CPtr console_ep, uint32_t timeout_us, uint32_t *out_reason) {
    uint64_t freq = wifi_read_cntfrq();
    uint64_t start = wifi_read_cntvct();
    uint64_t timeout_ticks = (freq * (uint64_t)timeout_us) / 1000000ull;
    uint64_t poll_interval_ticks = (freq * 200000ull) / 1000000ull; // 200мс
    uint64_t next_poll = start;

    bool seen_set_ssid_ok = false;
    bool seen_psk_sup_ok = false;

    while (true) {
        uint64_t now = wifi_read_cntvct();
        if (now >= next_poll) {
            next_poll = now + poll_interval_ticks;
            if (sdio_f2_read(0, g_event_rx_buf, BRCMF_FIRSTREAD)) {
                uint16_t len16 = (uint16_t)(g_event_rx_buf[0] | (g_event_rx_buf[1] << 8));
                uint16_t chk16 = (uint16_t)(g_event_rx_buf[2] | (g_event_rx_buf[3] << 8));
                bool checksum_ok = (uint16_t)~(len16 ^ chk16) == 0;
                if (checksum_ok && len16 >= SDPCM_HDRLEN) {
                    uint8_t channel = g_event_rx_buf[5] & 0x0F;
                    uint8_t dat_offset = g_event_rx_buf[7];
                    if (channel == SDPCM_EVENT_CHANNEL && dat_offset >= SDPCM_HDRLEN && dat_offset <= len16) {
                        if (len16 > BRCMF_FIRSTREAD) {
                            uint32_t remaining = len16 - BRCMF_FIRSTREAD;
                            uint32_t remaining_padded = (remaining + 3u) & ~3u;
                            if (BRCMF_FIRSTREAD + remaining_padded <= WIFI_EVENT_RX_BUF_CAP) {
                                sdio_f2_read(0, g_event_rx_buf + BRCMF_FIRSTREAD, remaining_padded);
                            }
                        }
                        uint32_t payload_off = dat_offset;
                        uint32_t payload_len = len16 - dat_offset;
                        if (payload_len >= BCDC_HEADER_LEN) {
                            uint8_t bcdc_data_offset = g_event_rx_buf[payload_off + 3];
                            uint32_t hdr_skip = BCDC_HEADER_LEN + (uint32_t)bcdc_data_offset * 4u;
                            if (payload_len >= hdr_skip + BRCMF_EVENT_HDR_LEN) {
                                uint32_t ev = payload_off + hdr_skip;
                                uint16_t eth_proto = (uint16_t)(((uint16_t)g_event_rx_buf[ev + 12] << 8) | g_event_rx_buf[ev + 13]);
                                uint32_t brcm_off = ev + ETHHDR_LEN;
                                bool oui_ok = (g_event_rx_buf[brcm_off + 5] == 0x00 &&
                                               g_event_rx_buf[brcm_off + 6] == 0x10 &&
                                               g_event_rx_buf[brcm_off + 7] == 0x18);
                                uint16_t usr_subtype = (uint16_t)(((uint16_t)g_event_rx_buf[brcm_off + 8] << 8) | g_event_rx_buf[brcm_off + 9]);
                                if (eth_proto == ETH_P_LINK_CTL && oui_ok && usr_subtype == BCMILCP_BCM_SUBTYPE_EVENT) {
                                    uint32_t msg_off = brcm_off + BRCM_ETHHDR_LEN;
                                    uint32_t event_type =
                                        ((uint32_t)g_event_rx_buf[msg_off + 4] << 24) | ((uint32_t)g_event_rx_buf[msg_off + 5] << 16) |
                                        ((uint32_t)g_event_rx_buf[msg_off + 6] << 8)  |  (uint32_t)g_event_rx_buf[msg_off + 7];
                                    uint32_t status =
                                        ((uint32_t)g_event_rx_buf[msg_off + 8] << 24) | ((uint32_t)g_event_rx_buf[msg_off + 9] << 16) |
                                        ((uint32_t)g_event_rx_buf[msg_off + 10] << 8) |  (uint32_t)g_event_rx_buf[msg_off + 11];
                                    uint32_t reason =
                                        ((uint32_t)g_event_rx_buf[msg_off + 12] << 24) | ((uint32_t)g_event_rx_buf[msg_off + 13] << 16) |
                                        ((uint32_t)g_event_rx_buf[msg_off + 14] << 8)  |  (uint32_t)g_event_rx_buf[msg_off + 15];

                                    sys_puthex32(console_ep, "[WIFI][JOIN] событие: event_type = ", event_type);
                                    sys_puthex32(console_ep, "[WIFI][JOIN]           status     = ", status);

                                    if (event_type == BRCMF_E_SET_SSID) {
                                        if (status == BRCMF_E_STATUS_SUCCESS) seen_set_ssid_ok = true;
                                        else { if (out_reason) *out_reason = reason; return WIFI_JOIN_FAILED; }
                                    } else if (event_type == BRCMF_E_PSK_SUP) {
                                        if (status == BRCMF_E_STATUS_FWSUP_COMPLETED) seen_psk_sup_ok = true;
                                        else { if (out_reason) *out_reason = status; return WIFI_JOIN_FAILED; }
                                    } else if (event_type == BRCMF_E_LINK && status == BRCMF_E_STATUS_NO_NETWORKS) {
                                        if (out_reason) *out_reason = status;
                                        return WIFI_JOIN_FAILED;
                                    } else if (event_type == BRCMF_E_DEAUTH || event_type == BRCMF_E_DEAUTH_IND ||
                                               event_type == BRCMF_E_DISASSOC_IND) {
                                        if (out_reason) *out_reason = reason;
                                        return WIFI_JOIN_FAILED;
                                    }

                                    if (seen_set_ssid_ok && seen_psk_sup_ok) return WIFI_JOIN_SUCCESS;
                                }
                            }
                        }
                    }
                }
            }
        }
        if (wifi_read_cntvct() - start >= timeout_ticks) {
            if (out_reason) *out_reason = 0;
            return WIFI_JOIN_TIMEOUT;
        }
        seL4_Yield();
    }
}

enum WifiConnectResult {
    WIFI_CONNECT_OK = 0,
    WIFI_CONNECT_ERR_IOVAR = 1,
    WIFI_CONNECT_ERR_PMK = 2,
    WIFI_CONNECT_ERR_JOIN = 3,
    WIFI_CONNECT_ERR_EVENT_FAILED = 4,
    WIFI_CONNECT_ERR_EVENT_TIMEOUT = 5,
};

// Полная оркестровка Милстоуна 4.4 — вызывается из main() по IPC-команде
// от шелла (см. WIFI_CMD_CONNECT ниже). Крипто (PBKDF2) — на хосте (см.
// заголовок секции выше про то, почему прошивка не делает это сама),
// 802.11 MLME/4-way handshake — прошивкой.
static WifiConnectResult wifi_connect(seL4_CPtr console_ep, const char *ssid, uint32_t ssid_len,
                                       const char *pass, uint32_t pass_len, uint32_t *out_reason) {
    // ИСПРАВЛЕНО (после первого неудачного теста на живом железе — join/
    // iovar-последовательность вся прошла с ack, но за 15с не пришло НИ
    // ОДНОГО события): эталон (brcmf_config_dongle(), cfg80211.c) шлёт
    // BRCMF_C_UP=0 ("make sure RF is ready for work") ПЕРЕД любым join —
    // мы этот шаг вообще пропускали, из-за чего прошивка, похоже, никогда
    // не запускала сам скан/ассоциацию. BRCMF_C_SET_PM=PM_OFF(0) — отключить
    // power-save, чтобы прошивка не проспала окно скана/ответа AP.
    sys_puts(console_ep, "[WIFI][JOIN] step: BRCMF_C_UP (включить радио)...\n");
    if (!wifi_dcmd_set_int(console_ep, BRCMF_C_UP, 0)) return WIFI_CONNECT_ERR_IOVAR;

    // ИСПРАВЛЕНО (после второго неудачного теста — весь iovar/join-обмен
    // снова с ack, снова 0 событий за 15с): brcmf_config_dongle() ТАКЖЕ
    // вызывает brcmf_cfg80211_change_iface(..., NL80211_IFTYPE_STATION,...),
    // которая шлёт BRCMF_C_SET_INFRA=1 ("infrastructure", т.е. "я станция,
    // подключаюсь к AP", в отличие от 0 = IBSS/ad-hoc) — мы этот шаг тоже
    // пропускали. Без него прошивка, видимо, не понимает свою роль и join
    // молча не запускает настоящую 802.11-ассоциацию.
    sys_puts(console_ep, "[WIFI][JOIN] step: BRCMF_C_SET_INFRA = 1 (режим станции)...\n");
    if (!wifi_dcmd_set_int(console_ep, BRCMF_C_SET_INFRA, 1)) return WIFI_CONNECT_ERR_IOVAR;

    sys_puts(console_ep, "[WIFI][JOIN] step: BRCMF_C_SET_PM = PM_OFF (отключить power-save)...\n");
    if (!wifi_dcmd_set_int(console_ep, BRCMF_C_SET_PM, 0)) return WIFI_CONNECT_ERR_IOVAR;

    // ДИАГНОСТИКА (временно, перед основной WPA2/join-последовательностью):
    // слепой escan — не зависит от корректности SSID/PSK вообще, только от
    // того, включено ли радио и делает ли прошивка ХОТЬ ЧТО-ТО в эфире.
    // Прошлые попытки join (всё ack'ается, 0 событий за 15с) не дают понять,
    // ломается ли сам join, или прошивка вообще ничего не делает после
    // UP/INFRA/PM — это разделит две гипотезы.
    sys_puts(console_ep, "[WIFI][JOIN] step: ДИАГНОСТИКА — слепой escan (все каналы/SSID)...\n");
    if (!wifi_escan_start(console_ep)) {
        sys_puts(console_ep, "[WIFI][JOIN] WARN: escan iovar не прошёл, продолжаем join всё равно\n");
    } else {
        wifi_wait_and_dump_any_events(console_ep, 5000000); // 5с
    }

    sys_puts(console_ep, "[WIFI][JOIN] step: PBKDF2(passphrase, SSID, 4096 итераций) -> PMK...\n");
    uint8_t pmk[32];
    pbkdf2_hmac_sha1((const uint8_t*)pass, pass_len, (const uint8_t*)ssid, ssid_len, 4096, pmk, 32);

    sys_puts(console_ep, "[WIFI][JOIN] step: iovar \"wsec\" = AES...\n");
    if (!wifi_iovar_set_int(console_ep, "wsec", WSEC_AES_ENABLED)) return WIFI_CONNECT_ERR_IOVAR;

    sys_puts(console_ep, "[WIFI][JOIN] step: iovar \"auth\" = 0 (open system)...\n");
    if (!wifi_iovar_set_int(console_ep, "auth", 0)) return WIFI_CONNECT_ERR_IOVAR;

    sys_puts(console_ep, "[WIFI][JOIN] step: iovar \"wpa_auth\" = WPA2_AUTH_PSK...\n");
    if (!wifi_iovar_set_int(console_ep, "wpa_auth", WPA2_AUTH_PSK)) return WIFI_CONNECT_ERR_IOVAR;

    sys_puts(console_ep, "[WIFI][JOIN] step: iovar \"sup_wpa\" = 1 (внутренний supplicant прошивки)...\n");
    if (!wifi_iovar_set_int(console_ep, "sup_wpa", 1)) return WIFI_CONNECT_ERR_IOVAR;

    sys_puts(console_ep, "[WIFI][JOIN] step: SET_WSEC_PMK...\n");
    if (!wifi_set_wsec_pmk(console_ep, pmk)) return WIFI_CONNECT_ERR_PMK;

    sys_puts(console_ep, "[WIFI][JOIN] step: iovar \"join\"...\n");
    if (!wifi_join(console_ep, ssid, ssid_len)) return WIFI_CONNECT_ERR_JOIN;

    sys_puts(console_ep, "[WIFI][JOIN] step: ожидание событий (SET_SSID + PSK_SUP)...\n");
    uint32_t reason = 0;
    WifiJoinResult r = wifi_wait_for_join_result(console_ep, 15000000, &reason); // 15с
    if (out_reason) *out_reason = reason;
    if (r == WIFI_JOIN_SUCCESS) {
        sys_puts(console_ep, "[WIFI][JOIN] УСПЕХ — подключено, handshake завершён.\n");
        return WIFI_CONNECT_OK;
    }
    if (r == WIFI_JOIN_TIMEOUT) {
        sys_puts(console_ep, "[WIFI][JOIN] FAIL: таймаут ожидания событий join\n");
        return WIFI_CONNECT_ERR_EVENT_TIMEOUT;
    }
    sys_puts(console_ep, "[WIFI][JOIN] FAIL: событие сообщило об ошибке (см. status/reason выше)\n");
    return WIFI_CONNECT_ERR_EVENT_FAILED;
}

// Команда IPC от шелла (см. shell.cpp `wifiprobe`): отдать последние
// сохранённые значения CMD5/CMD52.
constexpr seL4_Word WIFI_CMD_PROBE_STATUS = 1;
// Команда IPC от шелла (см. shell.cpp `wifi connect <ssid> <pass>`,
// Милстоун 4.4) — SSID/пароль передаются через разделяемую SHM (та же
// физическая память, что и SYS_READ_FILE в Милстоуне 4.2), т.к. не влезают
// в message registers. Смещения — область SHM, ничем больше не занятая
// (см. shell.cpp: 128 и 4060-4096 заняты под vfs/net, дальше свободно).
constexpr seL4_Word WIFI_CMD_CONNECT = 2;
constexpr uint32_t WIFI_SHM_SSID_LEN_OFFSET = 8192;
constexpr uint32_t WIFI_SHM_SSID_OFFSET     = 8196; // 32 байта
constexpr uint32_t WIFI_SHM_PASS_LEN_OFFSET = 8228;
constexpr uint32_t WIFI_SHM_PASS_OFFSET     = 8232; // 64 байта

int main(int argc, char *argv[]) {
    seL4_IPCBuffer *ipc = get_local_ipc();
    seL4_SetIPCBuffer(ipc);

    seL4_CPtr root_ep    = ipc->msg[BOOT_ROOT_EP];
    seL4_CPtr console_ep = ipc->msg[BOOT_CONSOLE_EP];
    seL4_CPtr my_ep      = ipc->msg[BOOT_WIFI_EP];
    g_wifi_blk_ep         = ipc->msg[7]; // BOOT_BLK_EP (Милстоун 4.2 — чтение прошивки/NVRAM)

    if (my_ep == 0) {
        __assert_fail("FATAL: Null Capability #0 Detected!", __FILE__, __LINE__, __func__);
    }

    // Своя копия разделяемой SHM (Милстоун 4.2 — путь+данные для SYS_READ_FILE,
    // тот же протокол, что shell.cpp/net_driver.cpp).
    seL4_SetMR(0, 107); // SYS_SHM_GET
    seL4_Call(root_ep, seL4_MessageInfo_new(0, 0, 0, 1));
    g_wifi_shm = (char*)seL4_GetMR(0);

    // Крипто-самотест (Милстоун 4.4) — не зависит от состояния железа,
    // прогоняем всегда, чтобы сразу отличить баг в PBKDF2/SHA1/HMAC от бага
    // в самом join, если что-то пойдёт не так на живом железе.
    wifi_pbkdf2_selftest(console_ep);

    sys_puts(console_ep, "[WIFI] SDIO host bring-up (Milestone 4.1)...\n");
    g_probe_ok = wifi_sdio_probe((void*)PLAT_WIFI_SDIO_VADDR, console_ep);
    if (g_probe_ok) sys_puts(console_ep, "[WIFI] SDIO probe OK.\n");
    else            sys_puts(console_ep, "[WIFI] SDIO probe FAILED (see log above) — wifiprobe will report last state.\n");

    if (g_probe_ok && g_wifi_blk_ep != 0 && g_wifi_shm != nullptr) {
        sys_puts(console_ep, "[WIFI] Backplane bring-up + прошивка/NVRAM (Milestone 4.2)...\n");
        if (wifi_backplane_bringup(console_ep)) {
            sys_puts(console_ep, "[WIFI] Milestone 4.2: прошивка похожа на живую!\n");
            sys_puts(console_ep, "[WIFI] sdpcm + IOCTL (Milestone 4.3)...\n");
            wifi_sdpcm_bringup(console_ep);
        } else {
            sys_puts(console_ep, "[WIFI] Milestone 4.2 FAILED (see log above) — wifiprobe will report last state.\n");
        }
    } else if (g_probe_ok) {
        sys_puts(console_ep, "[WIFI] Milestone 4.2 пропущен: нет blk_ep или SHM.\n");
    }

    // Не завязываем загрузку шелла на успех пробы: SYS_DRIVER_READY для
    // is_driver вне 1..4 сегодня игнорируется рутсервером (main.cpp), так что
    // этот вызов не блокирует SYS_WAIT_ALL_DRIVERS_READY, даже если проба выше
    // упала или зависла бы (см. ROADMAP.md Милстоун 4.1, план проверки п.2).
    seL4_SetMR(0, SYS_DRIVER_READY);
    seL4_Call(root_ep, seL4_MessageInfo_new(0, 0, 0, 1));

    while (1) {
        seL4_Word badge = 0;
        seL4_Recv(my_ep, &badge);

        seL4_Word cmd = seL4_GetMR(0);
        if (cmd == WIFI_CMD_PROBE_STATUS) {
            seL4_SetMR(0, g_probe_ok ? 0 : 1);
            seL4_SetMR(1, g_sdio_ocr);
            seL4_SetMR(2, g_cccr_rev);
            seL4_SetMR(3, g_wifi_fw_alive ? 1u : 0u);
            seL4_SetMR(4, g_wifi_shaddr);
            seL4_SetMR(5, g_wifi_sdpcm_ok ? 1u : 0u);
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 6));
        } else if (cmd == WIFI_CMD_CONNECT) {
            // Синхронно (как wifiprobe) — но может занять до ~15с (см.
            // wifi_wait_for_join_result), пока не увидим события успеха/
            // ошибки join, поэтому шелл будет заблокирован на время звонка.
            uint32_t result = (uint32_t)WIFI_CONNECT_ERR_JOIN;
            uint32_t reason = 0;
            if (!g_wifi_sdpcm_ok || g_wifi_shm == nullptr) {
                sys_puts(console_ep, "[WIFI][JOIN] FAIL: sdpcm/IOCTL (Милстоун 4.3) не готов или нет SHM\n");
            } else {
                uint32_t ssid_len = *(uint32_t*)(g_wifi_shm + WIFI_SHM_SSID_LEN_OFFSET);
                uint32_t pass_len = *(uint32_t*)(g_wifi_shm + WIFI_SHM_PASS_LEN_OFFSET);
                if (ssid_len > 32) ssid_len = 32;
                if (pass_len > 63) pass_len = 63;
                const char *ssid = g_wifi_shm + WIFI_SHM_SSID_OFFSET;
                const char *pass = g_wifi_shm + WIFI_SHM_PASS_OFFSET;
                result = (uint32_t)wifi_connect(console_ep, ssid, ssid_len, pass, pass_len, &reason);
            }
            seL4_SetMR(0, result);
            seL4_SetMR(1, reason);
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 2));
        } else {
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 0));
        }
    }

    return 0;
}
