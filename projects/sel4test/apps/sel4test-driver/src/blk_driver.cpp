#include <sel4/sel4.h>
#include "h/common.h"
#include "h/driver_state.h"
#include "h/exfat.h"
#include "h/platform.h"
#include "h/gpio.h"
#include <stdint.h>

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
static EXFAT_Instance g_file_system;
// План "Сигналы драйверам" — SYS_DRIVER_SIGNAL(STOP) гейтит всю
// остальную бизнес-логику диспетчера (см. main(), самое начало цикла);
// START/RESTART сбрасывают обратно в false.
static bool g_blk_stopped = false;
// Фаза 3b плана "Сигналы драйверам" — капа, которой САМ blk_driver сигналит
// root'у "я жив" (badge DRIVER_LIVENESS_BLK_BADGE), на каждом полученном
// BLK_LIVENESS_TICK_BADGE (см. main() ниже). 0, если root её не выдал
// (не должно случаться, но main() уже гейтит на 0 своей стороной).
//
// issuse.txt №66 — этого одного источника (тик снаружи, только пока драйвер
// простаивает на seL4_Recv()) НЕДОСТАТОЧНО: длинная легитимная VFS-операция
// (chunked-чтение большого файла, рост фрагментированной директории при
// mkdir/write) держит driver вне seL4_Recv() дольше 3с — тик просто некому
// принять, watchdog видит "не отвечает" и убивает ЖИВОЙ, просто занятый
// процесс, что навсегда вешает клиента, синхронно ждавшего ответа (root не
// умеет перехватить чужой reply у обычного seL4_Call, см. issuse.txt). Фикс
// (см. hardware_emmc_read()/hardware_emmc_write() ниже) — сигналить эту же
// капу ДОПОЛНИТЕЛЬНО после КАЖДОГО реального сектор-I/O, не только по тику:
// самый нижний общий слой, через который проходит любая VFS-операция (чтение/
// запись/рост директории/бит carte), так что "занят, но жив" покрывает вообще
// любую будущую медленную операцию, а не только конкретный repro с большим
// файлом. Лишние сигналы безвредны — root просто обновляет last_seen_ms на
// текущее время (main.cpp, DRIVER_LIVENESS_BLK_BADGE-блок), не считает их.
static seL4_CPtr g_blk_liveness_ntfn = 0;

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

// Смещение (в секторах) начала ВТОРОЙ (exFAT) партиции карты — см.
// find_exfat_partition() ниже. Уход от FAT32/8.3 (см. ROADMAP.md/issuse.txt):
// карта теперь размечена ДВУМЯ партициями — первая (FAT32, config.txt/
// u-boot.bin/образ ОС/прошивка) читается ТОЛЬКО ROM-загрузчиком RPi и
// U-Boot, этот драйвер её не монтирует и не трогает вообще никогда; вторая
// (exFAT) — единственная, которую видит blk_driver, здесь живут /bin/sbin/
// etc/conf/service/root. Все hardware_emmc_read/write ниже адресуют сектора
// ОТНОСИТЕЛЬНО этого смещения, то есть всегда только вторую партицию.
static uint32_t g_partition_start_sector = 0;

// Чтение (ls/cat) подтверждено стабильным на 3 холодных перезагрузках —
// включаем запись. Начиная с ухода на exFAT (см. выше) вторая партиция —
// ЧИСТО пользовательские данные, загрузочные файлы физически на другой
// партиции и этим кодом недостижимы в принципе — специальная осторожность
// при тестах записи (не трогать конкретные файлы) больше не требуется.
constexpr bool RPI4_EMMC_ALLOW_WRITE = true;

// Пользовательская рабочая директория (создаётся при первом запуске, если
// нет — см. main()). Загрузочные файлы (config.txt/u-boot.bin/образ ОС)
// остаются в корне раздела, шелл при старте всегда оказывается здесь.
constexpr const char* USER_ROOT_DIR = "/root";

// --- Вспомогательные функции ---
static ExfatStream g_blk_stream = {}; // потоковая запись, см. h/exfat.h

static void my_memcpy(void *dest, const void *src, int n) {
    // По 8 байт там, где ОБА адреса выровнены — см. подробный разбор в
    // usb_driver.cpp: SHM и DMA-буферы отображены НЕкэшируемо, каждый
    // доступ это отдельная транзакция на шине, и побайтовый цикл был
    // настоящим потолком скорости. Невыровненный 8-байтовый доступ к
    // Device-памяти даёт Alignment Fault, поэтому хвост — побайтово.
    unsigned char *d = (unsigned char *)dest;
    const unsigned char *s = (const unsigned char *)src;
    if (n >= 8 && (((uintptr_t)d | (uintptr_t)s) & 7) == 0) {
        uint64_t *d8 = (uint64_t *)d; const uint64_t *s8 = (const uint64_t *)s;
        int words = n / 8;
        for (int i = 0; i < words; i++) d8[i] = s8[i];
        int done = words * 8;
        d += done; s += done; n -= done;
    }
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
    // IRPT_EN остаётся 0 навсегда (см. ROADMAP.md/issuse.txt — "Spurious
    // interrupt!" фикс в main()/после монтирования FAT32): статус-биты
    // всегда доступны только опросом, реальный GIC-сигнал для них больше
    // нигде не включается.
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
    // ЛЮБОГО реального чтения/записи сектора (find_exfat_partition() дёргает
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
    // issuse.txt №66 — "занят, но жив" для watchdog'а, см. комментарий у
    // g_blk_liveness_ntfn выше.
    if (g_blk_liveness_ntfn != 0) seL4_Signal(g_blk_liveness_ntfn);
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
    // issuse.txt №66 — "занят, но жив" для watchdog'а, см. комментарий у
    // g_blk_liveness_ntfn выше.
    if (g_blk_liveness_ntfn != 0) seL4_Signal(g_blk_liveness_ntfn);
    return true;
}

// Уход от FAT32/8.3 (см. ROADMAP.md/issuse.txt, план): карта теперь
// размечена ДВУМЯ MBR-партициями — первая FAT32 (для ROM-загрузчика RPi/
// U-Boot, этот код её не трогает вообще), вторая exFAT (единственная,
// которую монтирует blk_driver). Ищем ВТОРУЮ запись в таблице разделов с
// типом 0x07 (стандартный MBR-тип exFAT — тот же байт, что у NTFS, поэтому
// сам по себе не доказателен) и сдвигаем g_partition_start_sector на её LBA.
// Реальную проверку "это точно exFAT, не чужой NTFS-раздел" делает
// exfat_init() (сигнатура FileSystemName=="EXFAT   "+BootSignature) — здесь
// достаточно найти КАНДИДАТА по типу байта, монтирование само откажет на
// невалидном содержимом.
static void find_exfat_partition(seL4_CPtr console_ep) {
    // План "Сигналы драйверам" (найдено на живом железе, SYS_DRIVER_SIGNAL
    // RESTART) — hardware_emmc_read() сама прибавляет g_partition_start_sector
    // к номеру сектора (см. её реализацию). При ПОВТОРНОМ вызове этой
    // функции (после первого успешного монтирования) g_partition_start_sector
    // уже ненулевой — "чтение сектора 0" реально читало начало УЖЕ
    // найденного exFAT-раздела (его VBR, не MBR физического диска), отсюда
    // ложное "MBR found but no partition with type 0x07". Явный сброс
    // перед чтением — единственный надёжный способ снова прочитать ФИЗИЧЕСКИЙ
    // сектор 0, вне зависимости от того, первый это вызов или нет.
    g_partition_start_sector = 0;
    uint8_t sector0[512];
    if (!hardware_emmc_read(0, 1, sector0)) {
        sys_puts(console_ep, "[BLK] WARNING: couldn't read sector 0 to detect partition table.\n");
        return;
    }

    uint16_t sig = (uint16_t)sector0[510] | ((uint16_t)sector0[511] << 8);
    if (sig != 0xAA55) {
        sys_puts(console_ep, "[BLK] WARNING: no MBR signature on sector 0 — карта не размечена под двухпартиционную схему exFAT, монтирование, вероятно, провалится.\n");
        return;
    }

    for (int i = 0; i < 4; i++) {
        const uint8_t* entry = &sector0[0x1BE + i * 16];
        uint8_t type = entry[4];
        if (type == 0x07) { // exFAT/NTFS — уточняется в exfat_init() по сигнатуре
            g_partition_start_sector = (uint32_t)entry[8] | ((uint32_t)entry[9] << 8)
                                      | ((uint32_t)entry[10] << 16) | ((uint32_t)entry[11] << 24);
            if (LOG_BLK) sys_puthex32(console_ep, "[BLK] exFAT partition candidate found, start LBA = ", g_partition_start_sector);
            return;
        }
    }
    sys_puts(console_ep, "[BLK] WARNING: MBR found but no partition with type 0x07 (exFAT) — карта размечена по-старому (одна FAT32-партиция)?\n");
}

// План "Сигналы драйверам" — монтирование exFAT вынесено из main() в
// отдельную вызываемую функцию (была вморожена в тело main() до цикла),
// чтобы SYS_DRIVER_SIGNAL(RESTART) могла позвать её повторно вместе с
// emmc_init() без убийства процесса. Логика 1:1 с прежним инлайном.
static bool blk_mount_exfat(seL4_CPtr console_ep) {
    // Ищем реальное начало exFAT-раздела (см. комментарий у
    // find_exfat_partition()) до монтирования, иначе exfat_init() прочитает
    // не тот сектор.
    find_exfat_partition(console_ep);

    if (exfat_init(&g_file_system, hardware_emmc_read, hardware_emmc_write)) {
        if (LOG_BLK) {
            sys_puts(console_ep, "[BLK] exFAT mounted.\n");
            sys_puthex32(console_ep, "[BLK][exFAT] fat_offset_sectors  = ", g_file_system.fat_offset_sectors);
            sys_puthex32(console_ep, "[BLK][exFAT] fat_length_sectors  = ", g_file_system.fat_length_sectors);
            sys_puthex32(console_ep, "[BLK][exFAT] cluster_heap_offset = ", g_file_system.cluster_heap_offset);
            sys_puthex32(console_ep, "[BLK][exFAT] root_cluster        = ", g_file_system.root_cluster);
            sys_puthex32(console_ep, "[BLK][exFAT] bitmap_cluster      = ", g_file_system.bitmap_cluster);
        }

        // Этап B (см. план — запись ещё не реализована, exfat_mkdir() пока
        // всегда возвращает false): каталоги /root/bin/sbin/etc/service/conf/*
        // на этом этапе НЕ создаются автоматически — карту нужно подготовить
        // с ними вручную (Finder/diskutil) до первой загрузки, пока Этап B
        // не реализован. Вызовы оставлены — они безопасно no-op'ают (false
        // молча игнорируется, как и раньше игнорировалось "уже существует"),
        // заработают сами по себе, как только exfat_mkdir получит реализацию.
        exfat_mkdir(&g_file_system, USER_ROOT_DIR);
        exfat_mkdir(&g_file_system, "/bin");
        exfat_mkdir(&g_file_system, "/sbin");
        exfat_mkdir(&g_file_system, "/etc");
        exfat_mkdir(&g_file_system, "/service");
        exfat_mkdir(&g_file_system, "/conf");
        exfat_mkdir(&g_file_system, "/conf/wifi_conf");
        exfat_mkdir(&g_file_system, "/conf/balancer_conf");
        exfat_mkdir(&g_file_system, "/conf/logger_conf");

        if (exfat_cd(&g_file_system, USER_ROOT_DIR)) {
            if (LOG_BLK) {
                sys_puts(console_ep, "[BLK] cwd set to ");
                sys_puts(console_ep, USER_ROOT_DIR);
                sys_puts(console_ep, "\n");
            }
        } else {
            sys_puts(console_ep, "[BLK] WARNING: couldn't cd into ");
            sys_puts(console_ep, USER_ROOT_DIR);
            sys_puts(console_ep, ", staying at exFAT root.\n");
        }
        return true;
    } else {
        sys_puts(console_ep, "[BLK] exFAT mount failed.\n");
        return false;
    }
}

// issuse.txt №69 — заменяет seL4_Reply() для ВСЕХ настоящих VFS-команд
// (см. вызов seL4_CNode_SaveCaller(SELF_CNODE_SLOT, VFS_PENDING_REPLY_SLOT,
// 8) в главном цикле, прямо перед разбором cmd). Reply-капа текущего
// клиента к этому моменту уже лежит в VFS_PENDING_REPLY_SLOT (см.
// common.h) — обычный seL4_Reply() использовал бы её неявно и тоже сработал
// бы, но тогда root не смог бы её оттуда достать, если blk_driver умрёт
// ПОСЛЕ SaveCaller, но ДО этого вызова (см. generic_recover_process()).
// seL4_Send на явный слот + Delete — функционально то же самое, что
// seL4_Reply, только с явным слотом, который остаётся адресуемым снаружи
// (из root'а) всё это время.
static inline void blk_vfs_reply(seL4_MessageInfo_t info) {
    seL4_Send(VFS_PENDING_REPLY_SLOT, info);
    seL4_CNode_Delete(SELF_CNODE_SLOT, VFS_PENDING_REPLY_SLOT, 8);
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
    // issuse.txt №74, часть "б" (адаптивный перенос драйверов между
    // ядрами) — общая страница состояния PARKED/BUSY, см.
    // h/driver_state.h. Инициализируем как можно раньше: до первого
    // seL4_Recv драйвер занят своим bring-up'ом, и root обязан это
    // видеть, иначе может счесть безопасным суспендить его посреди
    // инициализации железа.
    driver_state_init(PLAT_DRIVER_STATE_VADDR, 3, ipc->msg[BOOT_DRIVER_STATE_PRESENT]);
    seL4_CPtr console_ep = ipc->msg[BOOT_CONSOLE_EP];
    seL4_CPtr my_ep   = ipc->msg[7]; // BOOT_BLK_EP
    seL4_CPtr timer_ep = ipc->msg[BOOT_TIMER_EP]; // Фикс зависания (см. situation.txt): нужен для SYS_TIMER_HEARTBEAT_SUBSCRIBE ниже
    seL4_CPtr self_tcb = ipc->msg[BOOT_SELF_TCB_CAP]; // Фаза 6.1 (продолжение, см. ROADMAP.md)
    g_emmc_irq_ntfn = ipc->msg[BOOT_IRQ_EP]; // Фаза 4.5: капа на нотификацию общего IRQ EMMC2/Wi-Fi SDIO (см. main.cpp)
    g_root_ep = root_ep; // используется только на error-путях инициализации
    g_mmc_irq_handler = ipc->msg[BOOT_MMC_IRQ_HANDLER_CAP]; // фикс дедлока — см. notify_root_irq_handled()
    g_blk_dma_paddr = ipc->msg[BOOT_BLK_DMA_PADDR]; // Фаза 4.5/ADMA2, см. blk_dma_buf()/blk_dma_desc() выше
    g_blk_dma2_paddr = ipc->msg[BOOT_BLK_DMA2_PADDR]; // фикс задержки — вторая страница, см. blk_dma_desc()
    g_blk_liveness_ntfn = ipc->msg[BOOT_BLK_LIVENESS_NTFN_CAP]; // Фаза 3b, см. main.cpp
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

    // Начало GPIO-драйвера (см. h/gpio.h) — зелёный ACT LED, независимая от
    // EMMC периферия, PLAT_GPIO_VADDR уже замаплен root'ом при спавне (см.
    // spawn_process()/main.cpp, gpio_frame_param). Физический адрес не нужен —
    // достаточно скомпилированного vaddr.
    gpio_init((void*)PLAT_GPIO_VADDR);

    // 2.5/3. Монтируем файловую систему (см. blk_mount_exfat() выше —
    // теперь отдельная функция, чтобы SYS_DRIVER_SIGNAL(RESTART) могла
    // позвать её повторно).
    blk_mount_exfat(console_ep);

    // НАЙДЕНО НА ЖИВОМ ЖЕЛЕЗЕ (см. ROADMAP.md/issuse.txt — "Spurious
    // interrupt!" после КАЖДОЙ команды, задевающей диск, без всякого
    // balance/taskset): эта строка раньше включала реальный GIC-сигнал
    // (IRPT_EN = EMMC_INT_ALL_EN) для CMD_DONE/DATA_DONE/READ_RDY/
    // WRITE_RDY — предполагая, что ПОСЛЕ инициализации blk_driver ждёт эти
    // события через g_emmc_irq_ntfn/seL4_Wait (см. комментарий у неё выше).
    // Но emmc_wait_irpt_bit() (единственное место, которое реально ждёт эти
    // биты в горячем пути каждой команды) давно переведена на честный
    // wall-clock busy-yield (CNTVCT_EL0 + опрос регистра) — seL4_Wait на
    // g_emmc_irq_ntfn нигде в файле больше не вызывается, g_emmc_irq_ready
    // не читается нигде. Значит эти биты физически включали GIC-прерывание
    // ради события, которое ВСЕГДА обнаруживается и снимается софтом через
    // полинг РАНЬШЕ, чем аппаратное прерывание успевает дойти до ядра:
    // emmc_wait_irpt_bit сама снимает бит и сама зовёт
    // seL4_IRQHandler_Ack() (см. notify_root_irq_handled) в тот момент,
    // когда реальное аппаратное прерывание для ТОГО ЖЕ события зачастую
    // ещё только "в полёте" — когда оно всё же доходит до CPU (общая
    // level-triggered линия с Wi-Fi SDIO, см. IRQ_MMC_SHARED_BADGE), GIC
    // уже деактивирован нашим же опережающим Ack'ом, checkInterrupt() не
    // находит активного IRQ и печатает "Spurious interrupt!". Раз ждать
    // эти биты через прерывание всё равно никто не пытается — отключаем их
    // физическую генерацию совсем (тем же значением 0, что уже стоит в
    // emmc_init() до этого места, см. выше) — статус по-прежнему читается
    // полингом через EMMC_INTERRUPT, просто больше не дублируется настоящим
    // GIC-прерыванием, которое некому вовремя обработать.
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
        driver_state_parked(); // см. h/driver_state.h — с этой точки root вправе нас суспендить/переносить
        seL4_MessageInfo_t info = seL4_Recv(my_ep, &sender_badge);
        driver_state_busy();   // ДО любых ветвлений/continue ниже

        // Начало GPIO-драйвера (см. h/gpio.h) — гасим ACT LED здесь,
        // БЕЗУСЛОВНО, СРАЗУ после seL4_Recv(), ДО любых веток/`continue`
        // ниже (включая heartbeat-тик — иначе на holостом ходу, когда
        // единственные пробуждения это тики, этот код никогда бы не
        // исполнялся, и LED, once зажжённый первой же командой, оставался
        // бы гореть навсегда — именно так и было в первой версии, hw-
        // подтверждено 2026-08-16: "горит постоянно"). LED снова включится
        // ниже, непосредственно перед разбором настоящей VFS-команды.
        // Задержка выключения ограничена периодом heartbeat-тика (20мс,
        // см. main() выше) — незаметна глазу.
        gpio_act_led_off();

        // Фаза 3b плана "Сигналы драйверам" — ЧИСТО notification-пробуждение
        // (badge, MR0 не несёт настоящего сообщения — см. тот же паттерн у
        // net_driver.cpp/badge&NET_EVENT_*), проверяется ДО чтения cmd,
        // БЕЗУСЛОВНО (даже если g_blk_stopped — сигнал живости обязан идти
        // независимо от STOP, драйвер жив в любом случае). Единственный
        // источник этого бейджа — timer_driver, единственный бейдж на этом
        // ОТДЕЛЬНОМ (не разделяемом ни с чем, см. common.h) объекте.
        if (sender_badge == BLK_LIVENESS_TICK_BADGE) {
            if (g_blk_liveness_ntfn != 0) seL4_Signal(g_blk_liveness_ntfn);
            continue;
        }

        seL4_Word cmd = seL4_GetMR(0);

        // План "Сигналы драйверам" — проверяется БЕЗУСЛОВНО, ДО
        // stopped-гейта ниже: сигнал обязан доходить, даже если драйвер
        // уже остановлен (иначе STOP необратим без full respawn).
        if (cmd == SYS_DRIVER_SIGNAL) {
            seL4_Word sig = seL4_GetMR(1);
            if (sig == DRIVER_SIGNAL_STOP) {
                g_blk_stopped = true;
            } else if (sig == DRIVER_SIGNAL_START) {
                g_blk_stopped = false;
            } else if (sig == DRIVER_SIGNAL_RESTART) {
                // Повторный вызов уже факторизованных init-функций (см.
                // main() ниже) — без убийства процесса, без потери
                // капабилити. Непроверено на железе: emmc_init()/
                // blk_mount_exfat() штатно вызываются РОВНО один раз при
                // старте, безопасность ПОВТОРНОГО вызова на уже живом
                // контроллере — гипотеза, не факт (см. план), первая
                // hw-проверка обязана это подтвердить.
                emmc_init((void*)PLAT_EMMC_VADDR, console_ep);
                blk_mount_exfat(console_ep);
                g_blk_stopped = false;
            }
            seL4_SetMR(0, 0);
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
            continue;
        }
        if (g_blk_stopped) {
            seL4_SetMR(0, (seL4_Word)-1); // остановлен сигналом STOP — см. SYS_DRIVER_SIGNAL выше
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
            continue;
        }

        // Настоящая VFS-команда — мигаем ACT LED (см. gpio_act_led_off() выше).
        gpio_act_led_on();

        // issuse.txt №69 — reply-капа ТЕКУЩЕГО клиента откладывается в
        // фиксированный слот СВОЕГО CNode на всё время обработки этой
        // команды (см. blk_vfs_reply()/VFS_PENDING_REPLY_SLOT выше) — если
        // blk_driver умрёт/зависнет посреди обработки, root сможет вытащить
        // эту капу из нашего CNode и ответить клиенту сам (см.
        // generic_recover_process() в main.cpp). Каждая ветка ниже отвечает
        // через blk_vfs_reply(), а не напрямую seL4_Reply() — иначе капа
        // осталась бы висеть в слоте, никогда не удаляемая.
        seL4_CNode_SaveCaller(SELF_CNODE_SLOT, VFS_PENDING_REPLY_SLOT, 8);

        if (cmd == 110) { // SYS_LS
            char path[256]; // issuse.txt №42: exFAT-имя до 255 символов, был 64
            my_strlcpy(path, g_shm_vaddr, sizeof(path));

            uint32_t dir_cluster;
            if (path[0] == '\0') {
                dir_cluster = g_file_system.current_dir_cluster;
                if (dir_cluster == 0) dir_cluster = g_file_system.root_cluster;
            } else {
                char basename[256]; // issuse.txt №42
                uint32_t parent_clus = exfat_resolve_parent(&g_file_system, path, basename);
                if (parent_clus == 0xFFFFFFFF) {
                    my_strcpy(g_shm_vaddr, "ls: path not found\n");
                    seL4_SetMR(0, 0);
                    blk_vfs_reply(seL4_MessageInfo_new(0, 0, 0, 1));
                    continue;
                }
                if (basename[0] == '\0') {
                    dir_cluster = parent_clus;
                } else {
                    bool is_dir = false;
                    dir_cluster = exfat_find_in_dir(&g_file_system, parent_clus, basename, &is_dir);
                    // issuse.txt №36: раньше ls на обычном файле обходил его
                    // содержимое как таблицу каталога вместо явной ошибки.
                    if (dir_cluster != 0xFFFFFFFF && !is_dir) {
                        my_strcpy(g_shm_vaddr, "ls: not a directory\n");
                        seL4_SetMR(0, 0);
                        blk_vfs_reply(seL4_MessageInfo_new(0, 0, 0, 1));
                        continue;
                    }
                }
            }

            if (dir_cluster == 0xFFFFFFFF) {
                my_strcpy(g_shm_vaddr, "ls: directory not found\n");
                seL4_SetMR(0, 0);
                blk_vfs_reply(seL4_MessageInfo_new(0, 0, 0, 1));
                continue;
            }
            if (dir_cluster == 0) dir_cluster = g_file_system.root_cluster;

            // exfat_format_dir_listing обходит ВСЮ цепочку/пробег кластеров
            // каталога, поэтому каталоги, не помещающиеся в один кластер,
            // перечисляются полностью. Лимит — размер первой страницы SHM.
            exfat_format_dir_listing(&g_file_system, dir_cluster, g_shm_vaddr, 0x1000 - 8);
            seL4_SetMR(0, 0);
            blk_vfs_reply(seL4_MessageInfo_new(0, 0, 0, 1));
        }
        else if (cmd == 119) { // SYS_READ_FILE
            uint32_t offset = seL4_GetMR(1);
            uint32_t bytes_read = 0;

            // issuse.txt №69 (регрессионный тест) — см. KILL_WINDOW_TEST_OFFSET
            // в common.h: искусственная пауза ВМЕСТО настоящего чтения, чтобы
            // дать гарантированное окно на ручной `kill <pid>` РОВНО посреди
            // обработки (SaveCaller уже отработал выше, до этой ветки).
            if (offset == KILL_WINDOW_TEST_OFFSET) {
                uint64_t deadline = read_cntvct() + (uint64_t)KILL_WINDOW_TEST_DELAY_MS * g_cntfrq / 1000;
                while (read_cntvct() < deadline) {}
                seL4_SetMR(0, 0);
                seL4_SetMR(1, 0);
                blk_vfs_reply(seL4_MessageInfo_new(0, 0, 0, 2));
                continue;
            }

            // ОЧЕНЬ ВАЖНО: Сейчас в SHM (g_shm_vaddr) лежит строковое имя файла,
            // которое передал Rootserver. Мы обязаны скопировать его себе на стек,
            // потому что функция exfat_read_file перезапишет SHM бинарными данными ELF-файла!
            char filename[256]; // issuse.txt №42
            my_strlcpy(filename, g_shm_vaddr, sizeof(filename));

            bool success = exfat_read_file(&g_file_system, filename, g_shm_vaddr + VFS_PAYLOAD_OFFSET, offset, &bytes_read, VFS_PAYLOAD_MAX);

            if (success) {
                seL4_SetMR(0, 0); // Статус: OK
                seL4_SetMR(1, bytes_read);
            } else {
                seL4_SetMR(0, -1); // Ошибка: Файл не найден
                seL4_SetMR(1, 0);
            }
            blk_vfs_reply(seL4_MessageInfo_new(0, 0, 0, 2));
        }

        else if (cmd == 112) { // SYS_TOUCH
            char path[256]; // issuse.txt №42: exFAT-имя до 255 символов, был 64
            my_strlcpy(path, g_shm_vaddr, sizeof(path)); // Спасаем имя файла со стека
            bool existed = false;
            if (exfat_create_file(&g_file_system, path, &existed)) seL4_SetMR(0, existed ? 1 : 0); // 1 = уже существовал, ничего не создали
            else seL4_SetMR(0, -1);
            blk_vfs_reply(seL4_MessageInfo_new(0, 0, 0, 1));
        }

        else if (cmd == 113) { // SYS_WRITE_FILE (echo > file)
            char path[256]; // issuse.txt №42: exFAT-имя до 255 символов, был 64
            my_strlcpy(path, g_shm_vaddr, sizeof(path)); // Спасаем путь
            uint32_t len = seL4_GetMR(1);
            if (len > VFS_PAYLOAD_MAX) len = VFS_PAYLOAD_MAX;

            // ЛИШНЕЙ КОПИИ БОЛЬШЕ НЕТ. Раньше текст перекладывался в
            // приватный staging-буфер драйвера (BLK_SHM_STAGING_OFFSET),
            // потому что лежал в странице 0 общей памяти — а она же служит
            // TX-скретчем GENET, и DMA сети могла затереть его прямо во время
            // записи на диск. С переездом полезной нагрузки в собственную
            // область (VFS_PAYLOAD_OFFSET, см. platform.h) её не касается
            // никакой DMA: GENET работает со страницами 0/2/3, блочный DMA и
            // bounce USB лежат вообще вне SHM.
            //
            // Зачем убрали: копия стоила ещё один полный проход по
            // НЕкэшируемой памяти — столько же, сколько сама запись на диск
            // (см. разбор у my_memcpy выше). Клиент в это время заблокирован
            // в seL4_Call и изменить данные не может, другие клиенты
            // сериализованы vfs_lock.
            if (exfat_write_file(&g_file_system, path, g_shm_vaddr + VFS_PAYLOAD_OFFSET, len)) seL4_SetMR(0, 0);
            else seL4_SetMR(0, -1);
            blk_vfs_reply(seL4_MessageInfo_new(0, 0, 0, 1));
        }

        else if (cmd == 121) { // SYS_APPEND_FILE — дописывание в конец
            // Раскладка та же, что у cmd 113 выше; отличие ровно одно —
            // exfat_append_file() вместо exfat_write_file().
            char path[256]; // issuse.txt №42
            my_strlcpy(path, g_shm_vaddr, sizeof(path));
            uint32_t len = seL4_GetMR(1);
            if (len > VFS_PAYLOAD_MAX) len = VFS_PAYLOAD_MAX;

            if (exfat_append_file(&g_file_system, path, g_shm_vaddr + VFS_PAYLOAD_OFFSET, len)) seL4_SetMR(0, 0);
            else seL4_SetMR(0, -1);
            blk_vfs_reply(seL4_MessageInfo_new(0, 0, 0, 1));
        }


        else if (cmd == 122 || cmd == 123 || cmd == 124) { // ПОТОКОВАЯ ЗАПИСЬ, см. h/exfat.h
            // Один активный поток на драйвер: характерная нагрузка — один
            // журнал. Второй open поверх открытого честно отказывает, а не
            // молча подменяет цель под первым писателем.
            // Коды различимы, чтобы отказ не приходилось разгадывать по
            // косвенным признакам: -1 открытие, -2 запись, -3 закрытие,
            // -4 поток не открыт / уже открыт.
            int rc = -4;
            if (cmd == 122) {
                char path[256]; my_strlcpy(path, g_shm_vaddr, sizeof(path));
                if (!g_blk_stream.active) rc = exfat_stream_open(&g_file_system, path, (uint64_t)seL4_GetMR(1), &g_blk_stream) ? 0 : -1;
            } else if (cmd == 123) {
                uint32_t len = seL4_GetMR(1);
                if (len > VFS_PAYLOAD_MAX) len = VFS_PAYLOAD_MAX;
                if (g_blk_stream.active) rc = exfat_stream_write(&g_file_system, &g_blk_stream, g_shm_vaddr + VFS_PAYLOAD_OFFSET, len) ? 0 : -2;
            } else {
                if (g_blk_stream.active) rc = exfat_stream_close(&g_file_system, &g_blk_stream) ? 0 : -3;
            }
            seL4_SetMR(0, rc);
            blk_vfs_reply(seL4_MessageInfo_new(0, 0, 0, 1));
        }

        else if (cmd == 114) { // SYS_READ_TEXT_FILE (cat)
            char path[256]; // issuse.txt №42: exFAT-имя до 255 символов, был 64
            my_strlcpy(path, g_shm_vaddr, sizeof(path));

            // Читаем напрямую в SHM, чтобы shell мог сразу это распечатать
            uint32_t copied = 0;
            if (exfat_read_text_file(&g_file_system, path, g_shm_vaddr + VFS_PAYLOAD_OFFSET, &copied)) {
                seL4_SetMR(0, 0);
                seL4_SetMR(1, copied); // issuse.txt №56: реальный размер, cat сверяет со strlen()
                blk_vfs_reply(seL4_MessageInfo_new(0, 0, 0, 2));
            } else {
                seL4_SetMR(0, -1);
                blk_vfs_reply(seL4_MessageInfo_new(0, 0, 0, 1));
            }
        }

        else if (cmd == 120) { // SYS_RM
            char path[256]; // issuse.txt №42: exFAT-имя до 255 символов, был 64
            my_strlcpy(path, g_shm_vaddr, sizeof(path));
            if (exfat_delete_file(&g_file_system, path)) seL4_SetMR(0, 0);
            else seL4_SetMR(0, -1);
            blk_vfs_reply(seL4_MessageInfo_new(0, 0, 0, 1));
        }

        else if (cmd == 116) { // SYS_RENAME (mv)
            char old_p[256], new_p[256]; // issuse.txt №35/№42: было 32, обрезало длинные имена
            my_strlcpy(old_p, g_shm_vaddr, sizeof(old_p));
            my_strlcpy(new_p, g_shm_vaddr + 128, sizeof(new_p)); // Ожидаем новое имя по смещению 128
            if (exfat_rename_file(&g_file_system, old_p, new_p)) seL4_SetMR(0, 0);
            else seL4_SetMR(0, -1);
            blk_vfs_reply(seL4_MessageInfo_new(0, 0, 0, 1));
        }

        else if (cmd == 117) { // SYS_MKDIR
            char path[256]; // issuse.txt №42: exFAT-имя до 255 символов, был 64
            my_strlcpy(path, g_shm_vaddr, sizeof(path));
            bool existed = false;
            if (exfat_mkdir(&g_file_system, path, &existed)) seL4_SetMR(0, 0);
            else seL4_SetMR(0, existed ? 1 : -1); // 1 = уже существовал
            blk_vfs_reply(seL4_MessageInfo_new(0, 0, 0, 1));
        }
        
        else if (cmd == 118) { // SYS_CD
            char path[256]; // issuse.txt №42: exFAT-имя до 255 символов, был 64
            my_strlcpy(path, g_shm_vaddr, sizeof(path));
            if (exfat_cd(&g_file_system, path)) seL4_SetMR(0, 0);
            else seL4_SetMR(0, -1);
            blk_vfs_reply(seL4_MessageInfo_new(0, 0, 0, 1));
        }


        else if (cmd == SYS_GET_FS_SPACE) { // Фаза 8 (мониторинг ресурсов, `df`), см. h/common.h
            uint64_t total = 0, free_bytes = 0;
            exfat_free_space(&g_file_system, &total, &free_bytes);
            seL4_SetMR(0, (seL4_Word)total);
            seL4_SetMR(1, (seL4_Word)free_bytes);
            blk_vfs_reply(seL4_MessageInfo_new(0, 0, 0, 2));
        }

        else if (cmd == SYS_BENCHMARK_RESET_LOCAL) { // Фаза 6.1 (продолжение, см. ROADMAP.md)
            seL4_BenchmarkResetLog();
            seL4_SetMR(0, 0);
            blk_vfs_reply(seL4_MessageInfo_new(0, 0, 0, 1));
        }

        else if (cmd == SYS_BENCHMARK_FINALIZE_LOCAL) { // пара к RESET выше, см. h/common.h
            seL4_BenchmarkFinalizeLog();
            seL4_BenchmarkGetThreadUtilisation(self_tcb);
            seL4_Word idle_local = seL4_GetMR(4);  // BENCHMARK_IDLE_LOCALCPU_UTILISATION
            seL4_Word total_local = seL4_GetMR(9); // BENCHMARK_TOTAL_UTILISATION
            seL4_SetMR(0, idle_local);
            seL4_SetMR(1, total_local);
            blk_vfs_reply(seL4_MessageInfo_new(0, 0, 0, 2));
        }

        else {
            blk_vfs_reply(seL4_MessageInfo_new(0, 0, 0, 0));
        }
    }

    return 0;
}