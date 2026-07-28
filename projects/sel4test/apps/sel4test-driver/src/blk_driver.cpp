#include <sel4/sel4.h>
#include "h/common.h"
#include "h/fat32.h"
#include "h/platform.h"
#include <stdint.h>

uint32_t fat32_find_in_dir(FAT32_Instance* fs, uint32_t dir_cluster, const char* target_name);

// ARM generic timer (CNTVCT_EL0/CNTFRQ_EL0) — те же самые EL0-регистры, что
// уже использует timer_driver.cpp/wifi_driver.cpp для честного wall-clock
// таймаута (см. sdpcm_wait_and_read_ctrl в wifi_driver.cpp). Не требует
// вообще никакой capability — читается напрямую из EL0 любым процессом.
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
static uint64_t g_cntfrq = 0; // читается один раз в main(), см. ниже

// --- Глобальные переменные ---
static char* g_shm_vaddr = nullptr;
static FAT32_Instance g_file_system;

// Глобальные переменные EMMC2 (см. h/platform.h — регистровая карта SDHCI)
static volatile uint32_t* g_emmc_base = nullptr;
static uint32_t g_emmc_rca = 0; // Relative Card Address, получаем в emmc_init()

// Фаза 4.5/ADMA2 (см. ROADMAP.md) — приватный НЕКЭШИРУЕМЫЙ DMA bounce-буфер
// (физические адреса приходят через BOOT_BLK_DMA_PADDR/BOOT_BLK_DMA2_PADDR
// при спавне — см. main.cpp). Некэшируемый, а не стек/куча процесса —
// стандартный паттерн non-coherent DMA (см. подробный разбор в ROADMAP.md
// 4.5: DMA напрямую в кэшируемый стек потребовал бы явного cache maintenance
// + alignas(64) на каждом буфере в fat32.cpp, риск aliasing'а кэш-линий с
// соседними живыми локальными переменными — решили не рисковать, тот же
// приём, что уже проверен для GENET/net_driver/SHM).
// Фикс задержки (см. situation.txt, multi-block CMD18/25): данные теперь
// занимают ВСЮ первую страницу (до 8 секторов = 4096 байт за одну команду,
// вместо цикла из 8 отдельных CMD17/24) — дескриптору (8 байт) больше не
// хватает места рядом, поэтому под него отдельная вторая страница
// (PLAT_BLK_DMA_VADDR + 0x1000, физический адрес — g_blk_dma2_paddr).
static uint32_t g_blk_dma_paddr = 0;
static uint32_t g_blk_dma2_paddr = 0;
constexpr uintptr_t BLK_DMA_BUF_OFFSET = 0;
static inline volatile uint8_t* blk_dma_buf() {
    return (volatile uint8_t*)(PLAT_BLK_DMA_VADDR + BLK_DMA_BUF_OFFSET);
}
static inline volatile Adma2Descriptor32* blk_dma_desc() {
    return (volatile Adma2Descriptor32*)(PLAT_BLK_DMA_VADDR + 0x1000);
}

// Смещение (в секторах) начала FAT32-раздела на физической карте — см.
// find_fat32_partition() ниже. На стандартно размеченной SD-карте (с MBR)
// сектор 0 диска — это НЕ BPB, а таблица разделов; реальный BPB лежит по
// LBA первого FAT-раздела. 0 — если раздела нет и сектор 0 сам является BPB.
static uint32_t g_partition_start_sector = 0;

// Чтение (ls/cat) подтверждено стабильным на 3 холодных перезагрузках —
// включаем запись. ВНИМАНИЕ: FAT32-раздел на реальной SD-карте — тот же
// раздел с config.txt/образом ОС, так что первые тесты (touch/echo/mkdir/rm/mv)
// нужно делать на некритичных новых файлах, не трогая config.txt/u-boot.bin/
// sel4test-driver-image-arm-bcm2711/bcm2711-rpi-4-b.dtb/START4.ELF/BOOT.SCR.
constexpr bool RPI4_EMMC_ALLOW_WRITE = true;

// Пользовательская рабочая директория (создаётся при первом запуске, если
// нет — см. main()). Загрузочные файлы (config.txt/u-boot.bin/образ ОС)
// остаются в корне раздела, шелл при старте всегда оказывается здесь.
constexpr const char* USER_ROOT_DIR = "/root";

// --- Вспомогательные функции ---
static void my_memcpy(void *dest, const void *src, int n) {
    unsigned char *d = (unsigned char *)dest;
    const unsigned char *s = (const unsigned char *)src;
    while (n--) *d++ = *s++;
}
static int my_strlen(const char* s) { int len = 0; while (s[len]) len++; return len; }
static void my_strcpy(char *dest, const char *src) { while ((*dest++ = *src++)); }
// Копирует не более (cap-1) байт и всегда завершает '\0' в пределах [0, cap).
static void my_strlcpy(char *dest, const char *src, int cap) {
    if (cap <= 0) return;
    int i = 0;
    for (; i < cap - 1 && src[i] != '\0'; i++) dest[i] = src[i];
    dest[i] = '\0';
}
static int my_strcmp(const char *s1, const char *s2) {
    while (*s1 && (*s1 == *s2)) { s1++; s2++; }
    return *(const unsigned char*)s1 - *(const unsigned char*)s2;
}

static inline seL4_IPCBuffer* get_local_ipc() {
    seL4_Word tls_addr;
    // Добавлена буква 'ro'. crt0 не мог его стереть!
    asm volatile("mrs %0, tpidrro_el0" : "=r"(tls_addr)); 
    return (seL4_IPCBuffer*)(tls_addr - 1024);
}

void __assert_fail(const char *expr, const char *file, int line, const char *func) {
    while (1) {}
}

static void sys_puts(seL4_CPtr console_ep, const char *str);

static void sys_puthex32(seL4_CPtr console_ep, const char* label, uint32_t val) {
    sys_puts(console_ep, label);
    char buf[11];
    buf[0] = '0'; buf[1] = 'x';
    for (int i = 0; i < 8; i++) buf[2 + i] = "0123456789abcdef"[(val >> ((7 - i) * 4)) & 0xF];
    buf[10] = 0;
    sys_puts(console_ep, buf);
    sys_puts(console_ep, "\n");
}

static void sys_puts(seL4_CPtr console_ep, const char *str) {
    seL4_IPCBuffer *ipc = get_local_ipc();
    int total_len = my_strlen(str);
    int offset = 0;
    
    while (offset < total_len) {
        int chunk = total_len - offset;
        if (chunk > 100) chunk = 100;
        
        ipc->msg[0] = 8; // SYS_PUTS ID
        for (int i = 0; i < chunk; i++) {
            ipc->msg[i + 1] = str[offset + i];
        }
        seL4_Call(console_ep, seL4_MessageInfo_new(0, 0, 0, chunk + 1));
        offset += chunk;
    }
}



// ==========================================
// Глобальное состояние FAT32
// ==========================================

// ========================================================
// АППАРАТНЫЙ УРОВЕНЬ: EMMC2 (Arasan SDHCI), PIO/polling.
// Регистровая карта и биты — см. h/platform.h (EMMC_*).
// ========================================================
static inline volatile uint32_t* emmc_reg(uintptr_t offset) {
    return (volatile uint32_t*)((uintptr_t)g_emmc_base + offset);
}

// Капа на нотификацию общего IRQ EMMC2/Wi-Fi SDIO (см. IRQ_MMC_SHARED_BADGE
// и настройку в main.cpp) — сюда root будит нас, когда линия сработала.
// НЕ TCB-bind (как у UART) — та схема годится, только когда IRQ и клиентские
// сообщения делят один and тот же верхнеуровневый seL4_Recv(my_ep, ...); здесь
// ожидание IRQ происходит ВЛОЖЕННО, посреди обработки одной команды клиента
// (см. emmc_wait_irpt_bit ниже) — если бы это шло через my_ep, случайно
// пришедшее сообщение от ДРУГОГО клиента было бы ошибочно съедено вместо
// настоящего события железа. Поэтому это отдельная капа, отдельный
// seL4_Wait(), никак не пересекающийся с главным циклом диспетчеризации.
static seL4_CPtr g_emmc_irq_ntfn = 0;

// true после успешной инициализации (см. main(), ниже emmc_init()) — только
// тогда включаем событийное ожидание вместо busy-yield. См. ROADMAP.md 4.5:
// на этапе инициализации карта может вообще не отвечать (нет карты и т.п.),
// и там честный отказ по счётчику итераций важнее — иначе весь boot
// (SYS_WAIT_ALL_DRIVERS_READY) зависнет навсегда вместо чистой ошибки "EMMC2
// init failed" (см. память проекта: driver обязан просигналить готовность,
// иначе шелл висит вечно).
static bool g_emmc_irq_ready = false;

// Капа на root_ep (см. main(), ниже) — используется только на путях ошибок
// инициализации (сигнал SYS_DRIVER_READY при неудаче), НЕ в notify_root_irq_handled().
static seL4_CPtr g_root_ep = 0;

// ИСПРАВЛЕНО (живой дедлок, см. situation.txt): раньше здесь был СИНХРОННЫЙ
// seL4_Call(g_root_ep, SYS_MMC_IRQ_ACK), чтобы root сам вызвал
// seL4_IRQHandler_Ack(). Но когда ИМЕННО root является текущим синхронным
// вызывающим blk_driver (SYS_EXEC -> load_elf_from_disk(), см. main.cpp), root
// уже заблокирован в ожидании ОТВЕТА НА ЭТОТ САМЫЙ вызов — обратный IPC сюда
// мертво блокируется (root ждёт blk_driver, blk_driver ждёт root). Теперь
// blk_driver держит СОБСТВЕННУЮ копию IRQHandler-capability (g_mmc_irq_handler,
// см. BOOT_MMC_IRQ_HANDLER_CAP/common.h) и Ack'ает сам, без какого-либо IPC —
// безопасно, т.к. к этому моменту девайсный статус-бит уже снят (см. вызовы
// ниже в emmc_wait_irpt_bit): GIC не увидит линию всё ещё asserted.
static seL4_CPtr g_mmc_irq_handler = 0;

static void notify_root_irq_handled() {
    if (g_mmc_irq_handler == 0) return;
    seL4_IRQHandler_Ack(g_mmc_irq_handler);
}

// СОЗНАТЕЛЬНО НЕ переведены на событийное ожидание (Фаза 4.5, см.
// ROADMAP.md) — в отличие от emmc_wait_irpt_bit() ниже, это принципиально
// другой случай:
// 1. CMD_INHIBIT/DAT_INHIBIT — биты регистра STATUS (busy-флаги
//    контроллера), а не EMMC_INTERRUPT — по спеке SDHCI у них НЕТ
//    собственного прерывания, в отличие от CMD_DONE/DATA_DONE/READ_RDY/
//    WRITE_RDY.
// 2. Если бы всё равно завести для них ожидание на той же g_emmc_irq_ntfn,
//    возник бы риск дедлока по ответственности за SYS_MMC_IRQ_ACK: root не
//    Ack'ает GIC, пока blk_driver явно не подтвердит, что снял бит в
//    EMMC_INTERRUPT (см. emmc_wait_irpt_bit) — а эти две функции тот
//    регистр не трогают вообще, им нечего "снимать" и не о чем уведомлять
//    root. Проснуться по чужому сигналу и уйти в повторный Wait(), так и не
//    вызвав notify_root_irq_handled(), значит НАВСЕГДА заблокировать
//    переармирование GIC — заденет и уже рабочий emmc_wait_irpt_bit().
// 3. Реальный поток команд — cmd_ready() -> issue cmd -> irpt_bit(CMD_DONE)
//    -> (следующая команда) cmd_ready() — к моменту повторного вызова
//    cmd_ready() предыдущая команда уже полностью дождана через IRQ, т.е.
//    CMD_INHIBIT/DAT_INHIBIT на практике почти всегда УЖЕ сняты — цикл
//    ниже в норме проходит на первой же проверке, без единой реальной
//    итерации seL4_Yield(). Выигрыш от событийной версии здесь околонулевой
//    при не-нулевом риске — решили не трогать.
static bool emmc_wait_cmd_ready() {
    uint32_t timeout = 1000000;
    while (*emmc_reg(EMMC_STATUS_OFFSET) & EMMC_STATUS_CMD_INHIBIT) {
        if (--timeout == 0) return false;
        seL4_Yield();
    }
    return true;
}

static bool emmc_wait_dat_ready() {
    uint32_t timeout = 1000000;
    while (*emmc_reg(EMMC_STATUS_OFFSET) & EMMC_STATUS_DAT_INHIBIT) {
        if (--timeout == 0) return false;
        seL4_Yield();
    }
    return true;
}

// Ждет конкретный бит в INTERRUPT (READ_RDY/WRITE_RDY/DATA_DONE/CMD_DONE),
// сбрасывает его по получении. Любая ошибка (верхние 16 бит) — немедленный отказ.
//
// ИСПРАВЛЕНО (задержка на живом железе, см. situation.txt): раньше здесь был
// heartbeat-based seL4_Wait (timer_driver сигналил badged-копию notification
// каждые ~20мс, чтобы ожидание не висело вечно, если карта пропустит IRQ) —
// но карта РЕГУЛЯРНО (не изредка) отвечает с задержкой в десятки-сотни мс на
// multi-block DMA-передачах, и 20мс-гранулярность heartbeat добавляла
// заметную задержку ПОВЕРХ этого реального времени, которая накапливалась
// по 14 чанкам ELF-файла в разы дольше, чем нужно. Прямая привязка реального
// IRQ158 к blk_driver вместо релея через root тоже пробовалась и вызвала
// катастрофический регресс (см. common.h/IRQ_MMC_SHARED_BADGE) — откачена.
// Решение: честный wall-clock busy-yield (тот же приём, что уже проверен в
// wifi_driver.cpp для SDIO-таймингов — CNTVCT_EL0 напрямую, без capability,
// без IPC, без зависимости от того, чем занят root или кто держит IRQ-капу).
// Опрашивает регистр в цикле с seL4_Yield() между итерациями — гранулярность
// ограничена только скоростью самого цикла (десятки-сотни МКС), а не тиком
// heartbeat, и таймаут — настоящее время, а не число итераций.
static bool emmc_wait_irpt_bit(uint32_t bit) {
    uint64_t timeout_ticks = (600ull * g_cntfrq) / 1000; // ~600мс — тот же потолок, что был у heartbeat-версии
    uint64_t deadline = read_cntvct() + timeout_ticks;
    while (true) {
        uint32_t irpt = *emmc_reg(EMMC_INTERRUPT_OFFSET);
        if (irpt & EMMC_INT_ERROR_MASK) {
            *emmc_reg(EMMC_INTERRUPT_OFFSET) = irpt;
            notify_root_irq_handled(); // бит реально снят — теперь можно Ack'нуть GIC
            return false;
        }
        if (irpt & bit) {
            *emmc_reg(EMMC_INTERRUPT_OFFSET) = bit;
            notify_root_irq_handled();
            return true;
        }
        if (read_cntvct() >= deadline) return false;
        seL4_Yield();
    }
}

static bool emmc_send_cmd(uint32_t cmd_flags, uint32_t index, uint32_t arg) {
    if (!emmc_wait_cmd_ready()) return false;
    *emmc_reg(EMMC_INTERRUPT_OFFSET) = 0xFFFFFFFF; // сброс старых статусов
    *emmc_reg(EMMC_ARG1_OFFSET) = arg;
    *emmc_reg(EMMC_CMDTM_OFFSET) = (index << EMMC_CMD_INDEX_SHIFT) | cmd_flags;
    return emmc_wait_irpt_bit(EMMC_INT_CMD_DONE);
}

// Меняет делитель тактовой частоты (Divided Clock Mode). Клок обязательно
// выключается перед сменой делителя и включается заново после стабилизации —
// так требует SDHCI-спека.
static void emmc_set_clock_divider(uint32_t divisor) {
    uint32_t c1 = *emmc_reg(EMMC_CONTROL1_OFFSET) & ~EMMC_C1_CLK_EN;
    *emmc_reg(EMMC_CONTROL1_OFFSET) = c1;

    c1 &= ~(0xFFu << EMMC_C1_CLK_FREQ_SHIFT);
    c1 |= (divisor & 0xFFu) << EMMC_C1_CLK_FREQ_SHIFT;
    *emmc_reg(EMMC_CONTROL1_OFFSET) = c1;

    uint32_t timeout = 1000000;
    while (!(*emmc_reg(EMMC_CONTROL1_OFFSET) & EMMC_C1_CLK_STABLE)) {
        if (--timeout == 0) break; // не фатально само по себе — увидим по дальнейшим таймаутам команд
        seL4_Yield();
    }

    *emmc_reg(EMMC_CONTROL1_OFFSET) = *emmc_reg(EMMC_CONTROL1_OFFSET) | EMMC_C1_CLK_EN;
}

// Стандартная последовательность инициализации SD-карты (см. план Фазы 3.3):
// software reset -> идентификационный клок (~400kHz) -> CMD0 -> CMD8 ->
// ACMD41 (ждём готовности OCR) -> CMD2 -> CMD3 (получаем RCA) -> CMD7
// (выбираем карту) -> переключение на рабочий клок (~25MHz). 1-бит шина,
// без high-speed — минимум подвижных частей для первого теста на живом железе.
// console_ep — только для диагностики на живом железе (ВРЕМЕННО, см. ROADMAP.md
// Фаза 3.3): печатаем, на каком именно шаге инициализация упала, плюс дамп
// ключевых регистров в этот момент — без этого "EMMC2 init failed" ничего не
// говорит о причине.
bool emmc_init(void *vaddr, seL4_CPtr console_ep) {
    g_emmc_base = (volatile uint32_t*)vaddr;

    // Диагностика состояния, оставленного U-Boot'ом (он же реально грузит наш
    // образ с этой же карты через этот же контроллер, так что "до сброса" —
    // заведомо рабочая конфигурация; сравнение с "после" покажет, что именно
    // теряется после SRST_HC).
    if (LOG_BLK) {
        sys_puthex32(console_ep, "[BLK][EMMC] SLOTISR_VER (host version) = ", *emmc_reg(EMMC_SLOTISR_VER_OFFSET));
        sys_puthex32(console_ep, "[BLK][EMMC] CAP0 (capabilities)        = ", *emmc_reg(EMMC_CAP0_OFFSET));
        sys_puthex32(console_ep, "[BLK][EMMC] CONTROL0 before reset = ", *emmc_reg(EMMC_CONTROL0_OFFSET));
        sys_puthex32(console_ep, "[BLK][EMMC] CONTROL1 before reset = ", *emmc_reg(EMMC_CONTROL1_OFFSET));
        sys_puthex32(console_ep, "[BLK][EMMC] STATUS before reset    = ", *emmc_reg(EMMC_STATUS_OFFSET));
    }

    *emmc_reg(EMMC_CONTROL1_OFFSET) = EMMC_C1_SRST_HC;
    uint32_t timeout = 1000000;
    while (*emmc_reg(EMMC_CONTROL1_OFFSET) & EMMC_C1_SRST_HC) {
        if (--timeout == 0) {
            sys_puts(console_ep, "[BLK][EMMC] FAIL: software reset (SRST_HC) never cleared\n");
            sys_puthex32(console_ep, "[BLK][EMMC]   CONTROL1 = ", *emmc_reg(EMMC_CONTROL1_OFFSET));
            return false;
        }
        seL4_Yield();
    }
    if (LOG_BLK) {
        sys_puts(console_ep, "[BLK][EMMC] software reset OK\n");
        sys_puthex32(console_ep, "[BLK][EMMC] CONTROL0 after reset = ", *emmc_reg(EMMC_CONTROL0_OFFSET));
        sys_puthex32(console_ep, "[BLK][EMMC] CONTROL1 after reset = ", *emmc_reg(EMMC_CONTROL1_OFFSET));
    }

    // SRST_HC гасит питание шины (см. EMMC_C0_PWR_ON выше) — без этого ни одна
    // команда никогда не завершится (CMD_INHIBIT висит вечно, INTERRUPT=0).
    *emmc_reg(EMMC_CONTROL0_OFFSET) = *emmc_reg(EMMC_CONTROL0_OFFSET) | EMMC_C0_PWR_ON | EMMC_C0_PWR_3V3;
    if (LOG_BLK) sys_puthex32(console_ep, "[BLK][EMMC] CONTROL0 after power-on = ", *emmc_reg(EMMC_CONTROL0_OFFSET));

    *emmc_reg(EMMC_IRPT_MASK_OFFSET) = EMMC_INT_ALL_EN;
    // ВАЖНО (см. живой баг, найденный на этом самом шаге, ROADMAP.md 4.5):
    // сигнальные IRQ (IRPT_EN) НЕЛЬЗЯ включать здесь. GIC-линия общая с
    // Wi-Fi SDIO (см. IRQ_MMC_SHARED_BADGE в main.cpp), root её только
    // Ack'ает и сигналит нотификацию — САМ статусный бит в EMMC_INTERRUPT
    // при этом НЕ сбрасывается (это делает только код ниже, в
    // emmc_wait_irpt_bit). Поскольку линия level-triggered, а во время
    // ЭТОЙ функции blk_driver ещё не читает нотификацию (сидит в старом
    // busy-yield на статус-регистре), GIC переретриггерит тот же IRQ на
    // КАЖДОЙ итерации root'а мгновенно после Ack — реальный IRQ-шторм в
    // процессе root, который вообще не даёт scheduler'у дойти до
    // blk_driver, чтобы тот наконец сбросил бит. Ровно так и подвис boot
    // сразу после первой же команды EMMC во время написания этого кода.
    // IRPT_EN включается только ПОСЛЕ успешной инициализации (см. main(),
    // g_emmc_irq_ready) — до этого статус-биты доступны только опросом.
    *emmc_reg(EMMC_IRPT_EN_OFFSET)   = 0;
    *emmc_reg(EMMC_INTERRUPT_OFFSET) = 0xFFFFFFFF;

    // Базовая частота EMMC2 на BCM2711 — фиксированные 100MHz (DT clocks =
    // <0x06 0x33>). 0x80 -> 100MHz/(2*0x80) ≈ 390kHz (идентификационная стадия).
    *emmc_reg(EMMC_CONTROL1_OFFSET) = EMMC_C1_CLK_INTLEN | EMMC_C1_TOUNIT_MAX;
    emmc_set_clock_divider(0x80);
    if (LOG_BLK) sys_puthex32(console_ep, "[BLK][EMMC] CONTROL1 after clock setup = ", *emmc_reg(EMMC_CONTROL1_OFFSET));
    if (!(*emmc_reg(EMMC_CONTROL1_OFFSET) & EMMC_C1_CLK_STABLE)) {
        sys_puts(console_ep, "[BLK][EMMC] WARNING: clock not stable, continuing anyway\n");
    }
    // CLK_STABLE иногда выставляется раньше, чем клок реально устаканился на
    // выходе (встречающийся на практике quirk некоторых SDHCI-реализаций) —
    // добавляем небольшую "слепую" задержку сверх опроса бита, дешево и не
    // мешает, если это не требуется.
    for (int i = 0; i < 100000; i++) seL4_Yield();

    if (!emmc_send_cmd(EMMC_CMD_RSPNS_NONE, EMMC_CMD_GO_IDLE, 0)) {
        sys_puts(console_ep, "[BLK][EMMC] FAIL: CMD0 (GO_IDLE_STATE)\n");
        sys_puthex32(console_ep, "[BLK][EMMC]   STATUS    = ", *emmc_reg(EMMC_STATUS_OFFSET));
        sys_puthex32(console_ep, "[BLK][EMMC]   INTERRUPT = ", *emmc_reg(EMMC_INTERRUPT_OFFSET));
        return false;
    }
    if (LOG_BLK) sys_puts(console_ep, "[BLK][EMMC] CMD0 OK\n");

    if (!emmc_send_cmd(EMMC_CMD_RSPNS_48 | EMMC_CMD_CRCCHK_EN | EMMC_CMD_IXCHK_EN,
                        EMMC_CMD_SEND_IF_COND, 0x1AA)) {
        sys_puts(console_ep, "[BLK][EMMC] FAIL: CMD8 (SEND_IF_COND) — command itself failed/timed out\n");
        sys_puthex32(console_ep, "[BLK][EMMC]   STATUS    = ", *emmc_reg(EMMC_STATUS_OFFSET));
        sys_puthex32(console_ep, "[BLK][EMMC]   INTERRUPT = ", *emmc_reg(EMMC_INTERRUPT_OFFSET));
        return false;
    }
    uint32_t cmd8_resp = *emmc_reg(EMMC_RESP0_OFFSET);
    if ((cmd8_resp & 0xFF) != 0xAA) {
        sys_puts(console_ep, "[BLK][EMMC] FAIL: CMD8 echo mismatch (ответила, но не тем)\n");
        sys_puthex32(console_ep, "[BLK][EMMC]   RESP0 = ", cmd8_resp);
        return false;
    }
    if (LOG_BLK) sys_puts(console_ep, "[BLK][EMMC] CMD8 OK\n");

    bool ready = false;
    uint32_t last_ocr = 0;
    int acmd41_iters = 0;
    for (int i = 0; i < 1000 && !ready; i++) {
        acmd41_iters = i + 1;
        if (!emmc_send_cmd(EMMC_CMD_RSPNS_48, EMMC_CMD_APP_CMD, 0)) {
            sys_puts(console_ep, "[BLK][EMMC] FAIL: CMD55 (APP_CMD) во время ACMD41-цикла\n");
            sys_puthex32(console_ep, "[BLK][EMMC]   iteration = ", (uint32_t)i);
            return false;
        }
        if (!emmc_send_cmd(EMMC_CMD_RSPNS_48, EMMC_ACMD_SD_SEND_OP_COND,
                            EMMC_ACMD41_HCS | EMMC_ACMD41_VOLTAGE)) {
            sys_puts(console_ep, "[BLK][EMMC] FAIL: ACMD41 — команда сама не прошла\n");
            sys_puthex32(console_ep, "[BLK][EMMC]   iteration = ", (uint32_t)i);
            return false;
        }
        last_ocr = *emmc_reg(EMMC_RESP0_OFFSET);
        if (last_ocr & EMMC_OCR_READY) ready = true;
        else seL4_Yield();
    }
    if (LOG_BLK) {
        sys_puthex32(console_ep, "[BLK][EMMC] ACMD41 last OCR = ", last_ocr);
        sys_puthex32(console_ep, "[BLK][EMMC] ACMD41 iterations = ", (uint32_t)acmd41_iters);
    }
    if (!ready) {
        sys_puts(console_ep, "[BLK][EMMC] FAIL: ACMD41 never set OCR ready bit (карта не готова за 1000 попыток)\n");
        return false;
    }
    if (LOG_BLK) sys_puts(console_ep, "[BLK][EMMC] ACMD41 OK, card ready\n");

    if (!emmc_send_cmd(EMMC_CMD_RSPNS_136, EMMC_CMD_ALL_SEND_CID, 0)) {
        sys_puts(console_ep, "[BLK][EMMC] FAIL: CMD2 (ALL_SEND_CID)\n");
        sys_puthex32(console_ep, "[BLK][EMMC]   INTERRUPT = ", *emmc_reg(EMMC_INTERRUPT_OFFSET));
        return false;
    }
    if (LOG_BLK) sys_puts(console_ep, "[BLK][EMMC] CMD2 OK\n");

    if (!emmc_send_cmd(EMMC_CMD_RSPNS_48, EMMC_CMD_SEND_REL_ADDR, 0)) {
        sys_puts(console_ep, "[BLK][EMMC] FAIL: CMD3 (SEND_RELATIVE_ADDR)\n");
        sys_puthex32(console_ep, "[BLK][EMMC]   INTERRUPT = ", *emmc_reg(EMMC_INTERRUPT_OFFSET));
        return false;
    }
    g_emmc_rca = *emmc_reg(EMMC_RESP0_OFFSET) & 0xFFFF0000u;
    if (LOG_BLK) sys_puthex32(console_ep, "[BLK][EMMC] CMD3 OK, RCA = ", g_emmc_rca);

    if (!emmc_send_cmd(EMMC_CMD_RSPNS_48B, EMMC_CMD_SELECT_CARD, g_emmc_rca)) {
        sys_puts(console_ep, "[BLK][EMMC] FAIL: CMD7 (SELECT_CARD)\n");
        sys_puthex32(console_ep, "[BLK][EMMC]   STATUS    = ", *emmc_reg(EMMC_STATUS_OFFSET));
        sys_puthex32(console_ep, "[BLK][EMMC]   INTERRUPT = ", *emmc_reg(EMMC_INTERRUPT_OFFSET));
        return false;
    }
    if (LOG_BLK) sys_puts(console_ep, "[BLK][EMMC] CMD7 OK, card selected\n");

    // 0x02 -> 100MHz/(2*2) = 25MHz (рабочая стадия, standard speed).
    emmc_set_clock_divider(0x02);

    // Фаза 4.5/ADMA2 (см. ROADMAP.md) — режим DMA-select персистентный (не
    // per-команда, как EMMC_TM_DMA_EN), поэтому включаем один раз здесь, до
    // ЛЮБОГО реального чтения/записи сектора (find_fat32_partition() дёргает
    // hardware_emmc_read() сразу после emmc_init(), см. main()). На обычные
    // безданные команды (CMD0/CMD2/CMD3/CMD7 выше) DMA Select не влияет —
    // учитывается контроллером только вместе с EMMC_CMD_ISDATA+DMA_EN.
    *emmc_reg(EMMC_CONTROL0_OFFSET) = (*emmc_reg(EMMC_CONTROL0_OFFSET) & ~EMMC_C0_DMA_SEL_MASK) | EMMC_C0_DMA_SEL_ADMA2_32;

    return true;
}

// ИСПРАВЛЕНО (задержка на живом железе, см. situation.txt): раньше здесь был
// цикл из отдельных CMD17/24 (single-block) на КАЖДЫЙ сектор — 8 секторов
// (максимум за вызов, 1 страница SHM) значило 8 полных команда+ответ+данные
// циклов, и на 55КБ-файле (~108 секторов) это набегало на несколько реальных
// секунд задержки, полностью объяснимой без какой-либо аппаратной аномалии —
// просто накладные расходы SD-протокола, умноженные на число команд. Теперь
// count>1 сектор(ов) читаются/пишутся ОДНОЙ multi-block командой (CMD18/25) с
// ОДНИМ ADMA2-дескриптором на весь диапазон — 8 команд превращаются в одну.
// EMMC_TM_AUTO_CMD12 обязателен для CMD18/25 (см. platform.h) — без него
// контроллер не остановит передачу сам после последнего блока. count==1
// оставлен на старом, отдельно проверенном single-block пути (CMD17/24, без
// AUTO_CMD12) — минимальный риск для самого частого случая (FAT-таблица/
// директории по одному сектору).
//
// Фаза 4.5/ADMA2 (см. ROADMAP.md): ADMA2-дескриптор в некэшируемом bounce-
// буфере (см. blk_dma_buf()/blk_dma_desc() выше) вместо цикла на MMIO-слова
// через EMMC_DATA. Один memcpy на весь диапазон между bounce-буфером и
// buffer вызывающего (может быть на стеке fat32.cpp) — дёшево по сравнению с
// самим SD-обменом.
bool hardware_emmc_read(uint32_t sector, uint32_t count, void* buffer) {
    if (count == 0 || count > 8) return false;
    if (g_blk_dma_paddr == 0 || g_blk_dma2_paddr == 0) return false;

    volatile Adma2Descriptor32* desc = blk_dma_desc();
    desc->attr = (uint16_t)(ADMA2_ATTR_VALID | ADMA2_ATTR_END | ADMA2_ATTR_ACT_TRAN);
    desc->length = (uint16_t)(count * 512);
    desc->addr = g_blk_dma_paddr + BLK_DMA_BUF_OFFSET;

    if (!emmc_wait_dat_ready()) return false;
    *emmc_reg(EMMC_ADMA_SYSADDR_OFFSET) = g_blk_dma2_paddr;
    *emmc_reg(EMMC_BLKSIZECNT_OFFSET) = (count << 16) | 512;

    uint32_t cmd_flags = EMMC_CMD_RSPNS_48 | EMMC_CMD_CRCCHK_EN | EMMC_CMD_IXCHK_EN
                        | EMMC_CMD_ISDATA | EMMC_TM_DAT_DIR_READ | EMMC_TM_DMA_EN;
    uint32_t cmd_index = EMMC_CMD_READ_SINGLE;
    if (count > 1) {
        cmd_flags |= EMMC_TM_MULTI_BLOCK | EMMC_TM_BLKCNT_EN | EMMC_TM_AUTO_CMD12;
        cmd_index = EMMC_CMD_READ_MULTI;
    }
    if (!emmc_send_cmd(cmd_flags, cmd_index, g_partition_start_sector + sector)) return false;

    // ADMA2 сам гоняет данные между картой и памятью — READ_RDY (чисто
    // PIO-семантика "слово готово в FIFO") здесь не ждём, только конец
    // всего переноса.
    if (!emmc_wait_irpt_bit(EMMC_INT_DATA_DONE)) return false;

    my_memcpy(buffer, (const void*)blk_dma_buf(), count * 512);
    return true;
}

bool hardware_emmc_write(uint32_t sector, uint32_t count, const void* buffer) {
    if (!RPI4_EMMC_ALLOW_WRITE) return false;
    if (count == 0 || count > 8) return false;
    if (g_blk_dma_paddr == 0 || g_blk_dma2_paddr == 0) return false;

    my_memcpy((void*)blk_dma_buf(), buffer, count * 512);

    volatile Adma2Descriptor32* desc = blk_dma_desc();
    desc->attr = (uint16_t)(ADMA2_ATTR_VALID | ADMA2_ATTR_END | ADMA2_ATTR_ACT_TRAN);
    desc->length = (uint16_t)(count * 512);
    desc->addr = g_blk_dma_paddr + BLK_DMA_BUF_OFFSET;

    if (!emmc_wait_dat_ready()) return false;
    *emmc_reg(EMMC_ADMA_SYSADDR_OFFSET) = g_blk_dma2_paddr;
    *emmc_reg(EMMC_BLKSIZECNT_OFFSET) = (count << 16) | 512;

    uint32_t cmd_flags = EMMC_CMD_RSPNS_48 | EMMC_CMD_CRCCHK_EN | EMMC_CMD_IXCHK_EN
                        | EMMC_CMD_ISDATA | EMMC_TM_DMA_EN;
    uint32_t cmd_index = EMMC_CMD_WRITE_SINGLE;
    if (count > 1) {
        cmd_flags |= EMMC_TM_MULTI_BLOCK | EMMC_TM_BLKCNT_EN | EMMC_TM_AUTO_CMD12;
        cmd_index = EMMC_CMD_WRITE_MULTI;
    }
    if (!emmc_send_cmd(cmd_flags, cmd_index, g_partition_start_sector + sector)) return false;

    if (!emmc_wait_irpt_bit(EMMC_INT_DATA_DONE)) return false;
    return true;
}

// Стандартно размеченные SD-карты (в т.ч. подготовленные обычными
// инструментами вроде Raspberry Pi Imager) несут MBR в секторе 0 — это НЕ
// BPB, а таблица разделов, и реальный FAT32 начинается по LBA первого
// FAT-раздела. fat32_init() слепо трактует сектор 0 как BPB и не проверяет
// сигнатуры, поэтому на такой карте "монтирование" формально проходит (все
// поля защищены дефолтами на случай нулевых значений), но реального
// содержимого не видно — корневая директория читается из данных MBR/боот-кода,
// что выглядит как пустой каталог. Проверяем сигнатуру 0x55AA и jump instruction
// (0xEB/0xE9 — то, чем всегда начинается настоящий BPB), и если сектор 0 не
// похож на BPB — ищем первый FAT32/FAT16-раздел (тип 0x0B/0x0C/0x0E) в
// таблице MBR и сдвигаем все дальнейшие чтения/записи на его LBA.
static void find_fat32_partition(seL4_CPtr console_ep) {
    uint8_t sector0[512];
    if (!hardware_emmc_read(0, 1, sector0)) {
        sys_puts(console_ep, "[BLK] WARNING: couldn't read sector 0 to detect partition table.\n");
        return;
    }

    uint16_t sig = (uint16_t)sector0[510] | ((uint16_t)sector0[511] << 8);
    bool looks_like_bpb = (sector0[0] == 0xEB || sector0[0] == 0xE9);

    if (looks_like_bpb) {
        if (LOG_BLK) sys_puts(console_ep, "[BLK] sector 0 looks like a raw FAT32 BPB (no MBR).\n");
        return;
    }
    if (sig != 0xAA55) {
        sys_puts(console_ep, "[BLK] WARNING: sector 0 is neither a BPB nor has an MBR signature — mounting as-is.\n");
        return;
    }

    for (int i = 0; i < 4; i++) {
        const uint8_t* entry = &sector0[0x1BE + i * 16];
        uint8_t type = entry[4];
        if (type == 0x0B || type == 0x0C || type == 0x0E) { // FAT32 (CHS/LBA) / FAT16 LBA
            g_partition_start_sector = (uint32_t)entry[8] | ((uint32_t)entry[9] << 8)
                                      | ((uint32_t)entry[10] << 16) | ((uint32_t)entry[11] << 24);
            if (LOG_BLK) sys_puthex32(console_ep, "[BLK] MBR partition found, start LBA = ", g_partition_start_sector);
            return;
        }
    }
    sys_puts(console_ep, "[BLK] WARNING: MBR signature found but no FAT partition entry — mounting sector 0 as-is.\n");
}

// Helper to get the sector of the current working directory
static uint32_t get_cwd_sector(FAT32_Instance* fs) {
    uint32_t clus = fs->current_dir_cluster;
    if (clus == 0) clus = fs->root_cluster;
    return fs->data_start_sector + (clus - 2) * fs->sectors_per_cluster;
}


// ==========================================
// ГЛАВНАЯ ФУНКЦИЯ БЛОЧНОГО ДРАЙВЕРА
// ==========================================
int main(int argc, char *argv[]) {
    // 2. Достаем настоящий адрес буфера
    seL4_IPCBuffer *ipc = get_local_ipc();
    
    // 3. Отдаем его libsel4 (теперь её TLS инициализирован, и она сохранит его куда надо)
    seL4_SetIPCBuffer(ipc);

    // 2. Теперь безопасно получаем root_ep
    seL4_CPtr root_ep = ipc->msg[BOOT_ROOT_EP];
    seL4_CPtr console_ep = ipc->msg[BOOT_CONSOLE_EP];
    seL4_CPtr my_ep   = ipc->msg[7]; // BOOT_BLK_EP
    seL4_CPtr timer_ep = ipc->msg[BOOT_TIMER_EP]; // Фикс зависания (см. situation.txt): нужен для SYS_TIMER_HEARTBEAT_SUBSCRIBE ниже
    g_emmc_irq_ntfn = ipc->msg[BOOT_IRQ_EP]; // Фаза 4.5: капа на нотификацию общего IRQ EMMC2/Wi-Fi SDIO (см. main.cpp)
    g_root_ep = root_ep; // используется только на error-путях инициализации
    g_mmc_irq_handler = ipc->msg[BOOT_MMC_IRQ_HANDLER_CAP]; // фикс дедлока — см. notify_root_irq_handled()
    g_blk_dma_paddr = ipc->msg[BOOT_BLK_DMA_PADDR]; // Фаза 4.5/ADMA2, см. blk_dma_buf()/blk_dma_desc() выше
    g_blk_dma2_paddr = ipc->msg[BOOT_BLK_DMA2_PADDR]; // фикс задержки — вторая страница, см. blk_dma_desc()
    g_cntfrq = read_cntfrq(); // ДО emmc_init() — emmc_wait_irpt_bit() уже использует g_cntfrq для таймаута

    if (my_ep == 0) {
        __assert_fail("FATAL: Null Capability #0 Detected!", __FILE__, __LINE__, __func__);
    }

    if (LOG_BLK) sys_puts(console_ep, "\n[BLK] Server online.\n");

    // --- ДИНАМИЧЕСКИЙ ЗАПРОС SHM ---
    if (LOG_BLK) sys_puts(console_ep, "[BLK] Requesting SHM from kernel...\n");
    seL4_SetMR(0, 107); // SYS_SHM_GET
    seL4_MessageInfo_t msg = seL4_MessageInfo_new(0, 0, 0, 1);
    seL4_Call(root_ep, msg);

    g_shm_vaddr = (char*)seL4_GetMR(0);

    if (!g_shm_vaddr) {
        sys_puts(console_ep, "[BLK] FATAL: Failed to get dynamic SHM!\n");
        // Все равно сигналим готовность — иначе rootserver навечно зависнет
        // на wait_for_driver_ready() и не запустит остальные модули/shell.
        seL4_SetMR(0, SYS_DRIVER_READY);
        seL4_Call(root_ep, seL4_MessageInfo_new(0, 0, 0, 1));
        while(1) seL4_Yield();
    }

    // 2. Инициализация железа (EMMC2, PIO/polling — см. h/platform.h, emmc_init() выше)
    if (!emmc_init((void*)PLAT_EMMC_VADDR, console_ep)) {
        sys_puts(console_ep, "[BLK] ERROR: EMMC2 init failed.\n");
        // Как и выше — сигналим готовность перед выходом, иначе rootserver
        // навечно зависнет и не запустит остальные модули/shell.
        // ВАЖНО: 'return' из main() здесь недопустим — без обвязки libc/_exit
        // это уводит PC в мусор (см. PID:2 PC=0 фолт в логе живого железа) и
        // watchdog уходит в бесконечный респавн. while(1) паркует процесс
        // безопасно, ровно как уже сделано в других error-путях этого файла.
        seL4_SetMR(0, SYS_DRIVER_READY);
        seL4_Call(root_ep, seL4_MessageInfo_new(0, 0, 0, 1));
        while(1) seL4_Yield();
    }
    if (LOG_BLK) sys_puts(console_ep, "[BLK] EMMC2 initialized.\n");

    // 2.5. Ищем реальное начало FAT32-раздела (MBR или нет — см. комментарий
    // у find_fat32_partition()) до монтирования, иначе fat32_init() прочитает
    // не тот сектор.
    find_fat32_partition(console_ep);

    // 3. Монтируем Файловую Систему!
    if (fat32_init(&g_file_system, hardware_emmc_read, hardware_emmc_write)) {
        if (LOG_BLK) {
            sys_puts(console_ep, "[BLK] FAT32 mounted.\n");
            sys_puthex32(console_ep, "[BLK][FAT32] reserved_sectors  = ", g_file_system.reserved_sectors);
            sys_puthex32(console_ep, "[BLK][FAT32] sectors_per_fat   = ", g_file_system.sectors_per_fat);
            sys_puthex32(console_ep, "[BLK][FAT32] sectors_per_clus  = ", g_file_system.sectors_per_cluster);
            sys_puthex32(console_ep, "[BLK][FAT32] root_cluster      = ", g_file_system.root_cluster);
            sys_puthex32(console_ep, "[BLK][FAT32] data_start_sector = ", g_file_system.data_start_sector);
        }

        // Пользовательская рабочая директория — отделяем от загрузочных
        // файлов (config.txt/u-boot.bin/образ ОС и т.д.), которые обязаны
        // оставаться в корне FAT-раздела для U-Boot/прошивки. Создаём при
        // первом запуске, если её ещё нет (fat32_mkdir() безопасно вернёт
        // false, если уже существует, см. slot.found в fat32.cpp — ничего
        // не портит), заходим в неё и остаёмся там при старте системы.
        fat32_mkdir(&g_file_system, USER_ROOT_DIR); // false здесь = "уже существует", это ок
        if (fat32_cd(&g_file_system, USER_ROOT_DIR)) {
            if (LOG_BLK) {
                sys_puts(console_ep, "[BLK] cwd set to ");
                sys_puts(console_ep, USER_ROOT_DIR);
                sys_puts(console_ep, "\n");
            }
        } else {
            sys_puts(console_ep, "[BLK] WARNING: couldn't cd into ");
            sys_puts(console_ep, USER_ROOT_DIR);
            sys_puts(console_ep, ", staying at FAT root.\n");
        }
    } else {
        sys_puts(console_ep, "[BLK] FAT32 mount failed.\n");
    }

    // Только теперь безопасно включать реальный сигнальный IRQ (см. живой
    // баг и подробный комментарий в emmc_init() у EMMC_IRPT_EN_OFFSET) — ВСЯ
    // наша собственная инициализация (emmc_init + поиск раздела + монтирование
    // FAT32 + cd в рабочую директорию, все они тоже гоняют EMMC-команды через
    // emmc_send_cmd/emmc_wait_irpt_bit) уже прошла на старом проверенном
    // busy-yield. Раньше эта строка стояла СРАЗУ после emmc_init() — из-за
    // этого поиск раздела/монтирование FAT32 (которые тоже дергают железо)
    // первыми же попадали под совершенно новый, ещё не обкатанный
    // событийный путь ещё до готовности драйвера, и именно там подвисало.
    if (g_emmc_irq_ntfn != 0) {
        *emmc_reg(EMMC_IRPT_EN_OFFSET) = EMMC_INT_ALL_EN;
        g_emmc_irq_ready = true;
    }

    // Фикс живого зависания (см. situation.txt): подписываемся на heartbeat
    // от timer_driver'а — тот же каданс (20мс), что и net_driver (период общий
    // на весь процесс timer_driver, см. комментарий там же — обе подписки
    // ДОЛЖНЫ совпадать). Сигналится badged-копия ТОГО ЖЕ notification-
    // объекта, на котором emmc_wait_irpt_bit() блокируется через
    // g_emmc_irq_ntfn (см. main.cpp/BOOT_BLK_HEARTBEAT_NTFN_CAP) — без этого
    // seL4_Wait там мог зависнуть навсегда, если карта пропустит IRQ. 20мс
    // (не 100мс) — чтобы 30-итерационный таймаут ниже занимал ~0.6с, а не ~3с.
    if (timer_ep != 0) {
        seL4_SetMR(0, 9); // SYS_TIMER_HEARTBEAT_SUBSCRIBE
        seL4_SetMR(1, 20); // период, мс
        seL4_Call(timer_ep, seL4_MessageInfo_new(0, 0, 0, 2));
    }

    seL4_SetMR(0, SYS_DRIVER_READY);
    seL4_Call(root_ep, seL4_MessageInfo_new(0, 0, 0, 1));

    // 4. Главный цикл диспетчеризации (Control Plane)
    while (1) {
        seL4_Word sender_badge = 0;
        seL4_MessageInfo_t info = seL4_Recv(my_ep, &sender_badge);

        seL4_Word cmd = seL4_GetMR(0);

        if (cmd == 110) { // SYS_LS
            char path[64];
            my_strlcpy(path, g_shm_vaddr, sizeof(path));

            uint32_t dir_cluster;
            if (path[0] == '\0') {
                dir_cluster = g_file_system.current_dir_cluster;
                if (dir_cluster == 0) dir_cluster = g_file_system.root_cluster;
            } else {
                char basename[64];
                uint32_t parent_clus = fat32_resolve_parent(&g_file_system, path, basename);
                if (parent_clus == 0xFFFFFFFF) {
                    my_strcpy(g_shm_vaddr, "ls: path not found\n");
                    seL4_SetMR(0, 0);
                    seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                    continue;
                }
                if (basename[0] == '\0') {
                    dir_cluster = parent_clus;
                } else {
                    dir_cluster = fat32_find_in_dir(&g_file_system, parent_clus, basename);
                }
            }

            if (dir_cluster == 0xFFFFFFFF) {
                my_strcpy(g_shm_vaddr, "ls: directory not found\n");
                seL4_SetMR(0, 0);
                seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                continue;
            }
            if (dir_cluster == 0) dir_cluster = g_file_system.root_cluster;

            // fat32_format_dir_listing обходит ВСЮ цепочку кластеров каталога (а не
            // только первый сектор), поэтому директории, не помещающиеся в 512 байт,
            // теперь перечисляются полностью. Лимит — размер первой страницы SHM.
            fat32_format_dir_listing(&g_file_system, dir_cluster, g_shm_vaddr, 0x1000 - 8);
            seL4_SetMR(0, 0);
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
        }
        else if (cmd == 119) { // SYS_READ_FILE
            uint32_t offset = seL4_GetMR(1);
            uint32_t bytes_read = 0;

            // ОЧЕНЬ ВАЖНО: Сейчас в SHM (g_shm_vaddr) лежит строковое имя файла,
            // которое передал Rootserver. Мы обязаны скопировать его себе на стек,
            // потому что функция fat32_read_file перезапишет SHM бинарными данными ELF-файла!
            char filename[64];
            my_strlcpy(filename, g_shm_vaddr, sizeof(filename));

            bool success = fat32_read_file(&g_file_system, filename, g_shm_vaddr, offset, &bytes_read);

            if (success) {
                seL4_SetMR(0, 0); // Статус: OK
                seL4_SetMR(1, bytes_read);
            } else {
                seL4_SetMR(0, -1); // Ошибка: Файл не найден
                seL4_SetMR(1, 0);
            }
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 2));
        }

        else if (cmd == 112) { // SYS_TOUCH
            char path[64];
            my_strlcpy(path, g_shm_vaddr, sizeof(path)); // Спасаем имя файла со стека
            if (fat32_create_file(&g_file_system, path)) seL4_SetMR(0, 0);
            else seL4_SetMR(0, -1);
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
        }

        else if (cmd == 113) { // SYS_WRITE_FILE (echo > file)
            char path[64];
            my_strlcpy(path, g_shm_vaddr, sizeof(path)); // Спасаем путь
            uint32_t len = seL4_GetMR(1);
            // len приходит от клиента IPC и не должна превышать размер safe_text_buf
            if (len > 4096) len = 4096;

            // Защита памяти: копируем текст в собственную 6-ю страницу SHM
            // (BLK_SHM_STAGING_OFFSET, platform.h), чтобы DMA-контроллер не
            // затёр текст при чтении FAT. Раньше здесь была 3-я страница
            // (0x2000/8192) — арифметически пересекалась с GENET
            // rx_buffer_offsets[] (10240-16383), см. platform.h возле
            // BLK_SHM_STAGING_OFFSET за полным разбором бага.
            char* safe_text_buf = g_shm_vaddr + BLK_SHM_STAGING_OFFSET;
            for (int i = 0; i < 4096; i++) safe_text_buf[i] = 0; // Очищаем мусор
            my_memcpy(safe_text_buf, g_shm_vaddr + 128, len);
            
            if (fat32_write_file(&g_file_system, path, safe_text_buf, len)) seL4_SetMR(0, 0);
            else seL4_SetMR(0, -1);
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
        }

        else if (cmd == 114) { // SYS_READ_TEXT_FILE (cat)
            char path[64];
            my_strlcpy(path, g_shm_vaddr, sizeof(path));
            
            // Читаем напрямую в SHM, чтобы shell мог сразу это распечатать
            if (fat32_read_text_file(&g_file_system, path, g_shm_vaddr)) seL4_SetMR(0, 0);
            else seL4_SetMR(0, -1);
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
        }

        else if (cmd == 120) { // SYS_RM
            char path[64];
            my_strlcpy(path, g_shm_vaddr, sizeof(path));
            if (fat32_delete_file(&g_file_system, path)) seL4_SetMR(0, 0);
            else seL4_SetMR(0, -1);
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
        }

        else if (cmd == 116) { // SYS_RENAME (mv)
            char old_p[32], new_p[32];
            my_strlcpy(old_p, g_shm_vaddr, sizeof(old_p));
            my_strlcpy(new_p, g_shm_vaddr + 128, sizeof(new_p)); // Ожидаем новое имя по смещению 128
            if (fat32_rename_file(&g_file_system, old_p, new_p)) seL4_SetMR(0, 0);
            else seL4_SetMR(0, -1);
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
        }

        else if (cmd == 117) { // SYS_MKDIR
            char path[64];
            my_strlcpy(path, g_shm_vaddr, sizeof(path));
            if (fat32_mkdir(&g_file_system, path)) seL4_SetMR(0, 0);
            else seL4_SetMR(0, -1);
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
        }
        
        else if (cmd == 118) { // SYS_CD
            char path[64];
            my_strlcpy(path, g_shm_vaddr, sizeof(path));
            if (fat32_cd(&g_file_system, path)) seL4_SetMR(0, 0);
            else seL4_SetMR(0, -1);
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
        }


        else {
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 0));
        }
    }

    return 0;
}