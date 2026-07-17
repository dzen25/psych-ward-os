#include <sel4/sel4.h>
#include "h/common.h"
#include "h/fat32.h"
#include "h/platform.h"
#include <stdint.h>

uint32_t fat32_find_in_dir(FAT32_Instance* fs, uint32_t dir_cluster, const char* target_name);

// --- Глобальные переменные ---
static char* g_shm_vaddr = nullptr;
static FAT32_Instance g_file_system;

// Глобальные переменные EMMC2 (см. h/platform.h — регистровая карта SDHCI)
static volatile uint32_t* g_emmc_base = nullptr;
static uint32_t g_emmc_rca = 0; // Relative Card Address, получаем в emmc_init()

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
static bool emmc_wait_irpt_bit(uint32_t bit) {
    uint32_t timeout = 1000000;
    while (true) {
        uint32_t irpt = *emmc_reg(EMMC_INTERRUPT_OFFSET);
        if (irpt & EMMC_INT_ERROR_MASK) { *emmc_reg(EMMC_INTERRUPT_OFFSET) = irpt; return false; }
        if (irpt & bit) { *emmc_reg(EMMC_INTERRUPT_OFFSET) = bit; return true; }
        if (--timeout == 0) return false;
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
    *emmc_reg(EMMC_IRPT_EN_OFFSET)   = 0; // сигнальные IRQ не используем, только статус-биты (polling)
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

    return true;
}

// Читает/пишет по одному сектору за раз (CMD17/CMD24) — без multi-block
// (CMD18/CMD25), чтобы не связываться с auto-CMD12/CMD23 на первом проходе.
// FAT32-слой запрашивает не больше 8 секторов (1 страница SHM) за вызов, так
// что цикл по count здесь совсем короткий.
bool hardware_emmc_read(uint32_t sector, uint32_t count, void* buffer) {
    if (count == 0 || count > 8) return false;
    uint32_t* out = (uint32_t*)buffer;

    for (uint32_t i = 0; i < count; i++) {
        if (!emmc_wait_dat_ready()) return false;
        *emmc_reg(EMMC_BLKSIZECNT_OFFSET) = (1u << 16) | 512;

        uint32_t cmd_flags = EMMC_CMD_RSPNS_48 | EMMC_CMD_CRCCHK_EN | EMMC_CMD_IXCHK_EN
                            | EMMC_CMD_ISDATA | EMMC_TM_DAT_DIR_READ;
        if (!emmc_send_cmd(cmd_flags, EMMC_CMD_READ_SINGLE, g_partition_start_sector + sector + i)) return false;

        if (!emmc_wait_irpt_bit(EMMC_INT_READ_RDY)) return false;
        for (int w = 0; w < 128; w++) out[i * 128 + w] = *emmc_reg(EMMC_DATA_OFFSET);
        if (!emmc_wait_irpt_bit(EMMC_INT_DATA_DONE)) return false;
    }
    return true;
}

bool hardware_emmc_write(uint32_t sector, uint32_t count, const void* buffer) {
    if (!RPI4_EMMC_ALLOW_WRITE) return false;
    if (count == 0 || count > 8) return false;
    const uint32_t* in = (const uint32_t*)buffer;

    for (uint32_t i = 0; i < count; i++) {
        if (!emmc_wait_dat_ready()) return false;
        *emmc_reg(EMMC_BLKSIZECNT_OFFSET) = (1u << 16) | 512;

        uint32_t cmd_flags = EMMC_CMD_RSPNS_48 | EMMC_CMD_CRCCHK_EN | EMMC_CMD_IXCHK_EN | EMMC_CMD_ISDATA;
        if (!emmc_send_cmd(cmd_flags, EMMC_CMD_WRITE_SINGLE, g_partition_start_sector + sector + i)) return false;

        if (!emmc_wait_irpt_bit(EMMC_INT_WRITE_RDY)) return false;
        for (int w = 0; w < 128; w++) *emmc_reg(EMMC_DATA_OFFSET) = in[i * 128 + w];
        if (!emmc_wait_irpt_bit(EMMC_INT_DATA_DONE)) return false;
    }
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

            // Защита памяти: копируем текст в безопасную 3-ю страницу SHM,
            // чтобы DMA-контроллер VirtIO случайно не затер текст при чтении FAT
            char* safe_text_buf = g_shm_vaddr + 0x2000;
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