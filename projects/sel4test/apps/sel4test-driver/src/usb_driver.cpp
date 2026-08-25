// usb_driver.cpp — Фаза 14 (см. ROADMAP.md): xHCI (VL805) bring-up +
// перечисление ОДНОГО подключённого USB-устройства. Без класс-драйверов
// (ни Mass Storage, ни HID) — это отдельная будущая фаза. Каждый шаг
// bring-up (см. ROADMAP.md "Порядок bring-up") — с ограниченным по времени
// опросом (wall-clock через CNTVCT_EL0, тот же приём, что
// emmc_wait_irpt_bit() в blk_driver.cpp) и понятным логом — ни один шаг
// не виснет навсегда, если что-то на этой конкретной плате пойдёт не так.
#include <sel4/sel4.h>
#include "h/common.h"
#include "h/platform.h"
#include "h/exfat.h"
#include <stdint.h>

// Milestone 7 — hardware_usb_read/write берут его отсюда (сигнатура
// block_read_fn/block_write_fn фиксирована в h/exfat.h, без параметра
// контекста). Milestone B2 (Фаза 15) — понадобился РАНЬШЕ по файлу
// (cond_hub_port_c_reset(), см. ниже) — перенесено сюда из точки
// объявления рядом с hardware_usb_read/write, единственная реальная
// зависимость которых — читать этот же глобал, не порядок объявления.
static seL4_CPtr g_console_ep = 0;

// issuse.txt №66 (тот же фикс, что blk_driver.cpp/g_blk_liveness_ntfn) — капа,
// которой САМ usb_driver сигналит root'у "я жив" (badge
// DRIVER_LIVENESS_USB_BADGE). Раньше heartbeat уходил ТОЛЬКО пока driver
// простаивал на seL4_Recv() — длинная легитимная VFS-операция над USB-
// накопителем (chunked-чтение большого файла и т.п.) держала бы его занятым
// дольше WATCHDOG_TIMEOUT_MS[6]=5000мс без единого тика, watchdog принял бы
// "занят" за "завис" и убил бы живой процесс, навсегда повесив клиента,
// синхронно ждавшего ответа (root не умеет перехватить чужой reply у
// обычного seL4_Call). Та же фиксированная сигнатура block_read_fn/
// block_write_fn (см. g_console_ep выше) не оставляет места для параметра
// контекста — глобал по тому же паттерну. Сигналится дополнительно после
// КАЖДОГО реального сектор-I/O в hardware_usb_rw_generic_read/write() ниже,
// не только по тику — лишние сигналы безвредны, root просто обновляет
// last_seen_ms (main.cpp, DRIVER_LIVENESS_*_BADGE-блок).
static seL4_CPtr g_usb_liveness_ntfn = 0;

// --- Обвязка (тот же паттерн, что blk_driver.cpp/net_driver.cpp) ---

static void my_memcpy(void *dest, const void *src, int n) {
    uint8_t *d = (uint8_t*)dest; const uint8_t *s = (const uint8_t*)src;
    for (int i = 0; i < n; i++) d[i] = s[i];
}
// Milestone 8 — тот же приём, что my_strcpy/my_strlcpy в blk_driver.cpp
// (собственные копии, не общий заголовок — у каждого драйвера свои).
static void my_strcpy(char *dest, const char *src) { while ((*dest++ = *src++)); }
static void my_strlcpy(char *dest, const char *src, int cap) {
    if (cap <= 0) return;
    int i = 0;
    for (; i < cap - 1 && src[i] != '\0'; i++) dest[i] = src[i];
    dest[i] = '\0';
}

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
static uint64_t g_cntfrq = 0;

static inline seL4_IPCBuffer* get_local_ipc() {
    seL4_Word tls_addr;
    asm volatile("mrs %0, tpidrro_el0" : "=r"(tls_addr));
    return (seL4_IPCBuffer*)(tls_addr - 1024);
}

void __assert_fail(const char *expr, const char *file, int line, const char *func) { while (1) seL4_Yield(); }

static int my_strlen(const char* s) { int len = 0; while (s[len]) len++; return len; }

static void sys_puts(seL4_CPtr console_ep, const char *str) {
    seL4_IPCBuffer *ipc = get_local_ipc();
    int total_len = my_strlen(str);
    int offset = 0;
    while (offset < total_len) {
        int chunk = total_len - offset;
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

static void sys_puthex16(seL4_CPtr console_ep, const char* label, uint16_t val) {
    sys_puthex32(console_ep, label, (uint32_t)val);
}

static void sys_puthex64(seL4_CPtr console_ep, const char* label, uint64_t val) {
    sys_puts(console_ep, label);
    char buf[19];
    buf[0] = '0'; buf[1] = 'x';
    for (int i = 0; i < 16; i++) buf[2 + i] = "0123456789abcdef"[(val >> ((15 - i) * 4)) & 0xF];
    buf[18] = 0;
    sys_puts(console_ep, buf);
    sys_puts(console_ep, "\n");
}

// --- Регистровая карта xHCI (см. xHCI спецификацию 1.2; PLAT_XHCI_VADDR —
// база MMIO-окна, платформенные PADDR/IRQ/SIZE — см. platform.h). ---

// Capability Registers (от базы xHCI)
constexpr uintptr_t XHCI_CAPLENGTH   = 0x00; // u8
constexpr uintptr_t XHCI_HCIVERSION  = 0x02; // u16
constexpr uintptr_t XHCI_HCSPARAMS1  = 0x04;
constexpr uintptr_t XHCI_HCSPARAMS2  = 0x08;
constexpr uintptr_t XHCI_HCCPARAMS1  = 0x10;
constexpr uintptr_t XHCI_DBOFF       = 0x14;
constexpr uintptr_t XHCI_RTSOFF      = 0x18;

// Operational Registers (offset относительно op_base = xhci_base + CAPLENGTH)
constexpr uintptr_t XHCI_OP_USBCMD   = 0x00;
constexpr uintptr_t XHCI_OP_USBSTS   = 0x04;
constexpr uintptr_t XHCI_OP_PAGESIZE = 0x08;
constexpr uintptr_t XHCI_OP_CRCR     = 0x18; // u64
constexpr uintptr_t XHCI_OP_DCBAAP   = 0x30; // u64
constexpr uintptr_t XHCI_OP_CONFIG   = 0x38;
constexpr uintptr_t XHCI_OP_PORTSC_BASE = 0x400; // + (port-1)*0x10

constexpr uint32_t USBCMD_RS    = (1u << 0);
constexpr uint32_t USBCMD_HCRST = (1u << 1);
constexpr uint32_t USBCMD_INTE  = (1u << 2);
constexpr uint32_t USBSTS_HCH   = (1u << 0);
constexpr uint32_t USBSTS_HSE   = (1u << 2);
constexpr uint32_t USBSTS_EINT  = (1u << 3);
constexpr uint32_t USBSTS_CNR   = (1u << 11);

constexpr uint32_t PORTSC_CCS = (1u << 0);
constexpr uint32_t PORTSC_PED = (1u << 1);
constexpr uint32_t PORTSC_PR  = (1u << 4);
constexpr uint32_t PORTSC_PP  = (1u << 9);
constexpr uint32_t PORTSC_CSC = (1u << 17);
constexpr uint32_t PORTSC_PRC = (1u << 21);
// RW1C-биты PORTSC, которые НЕЛЬЗЯ случайно затереть при read-modify-write
// (запись 0 в PED, например, отключает порт) — маска для "не трогать при
// простом опросе".
constexpr uint32_t PORTSC_RW1C_MASK = PORTSC_CSC | (1u<<18) | (1u<<19) | (1u<<20) | PORTSC_PRC | (1u<<22) | (1u<<23);

// Runtime Registers (offset относительно rt_base = xhci_base + RTSOFF)
constexpr uintptr_t XHCI_RT_IR0 = 0x20; // Interrupter Register Set 0
constexpr uintptr_t XHCI_IR_IMAN   = 0x00;
constexpr uintptr_t XHCI_IR_IMOD   = 0x04;
constexpr uintptr_t XHCI_IR_ERSTSZ = 0x08;
constexpr uintptr_t XHCI_IR_ERSTBA = 0x10; // u64
constexpr uintptr_t XHCI_IR_ERDP   = 0x18; // u64

constexpr uint32_t IMAN_IP = (1u << 0);
constexpr uint32_t IMAN_IE = (1u << 1);
constexpr uint64_t ERDP_EHB = (1ull << 3);

// TRB (16 байт) — Parameter(u64) + Status(u32) + Control(u32). Control:
// Cycle[0], TRB Type[15:10].
struct Trb {
    uint64_t parameter;
    uint32_t status;
    uint32_t control;
};
constexpr int TRB_TYPE_SHIFT = 10;
constexpr uint32_t trb_type(uint32_t t) { return t << TRB_TYPE_SHIFT; }
constexpr uint32_t TRB_TYPE_MASK = 0x3F << TRB_TYPE_SHIFT;

constexpr uint32_t TRB_TYPE_LINK               = 6;
constexpr uint32_t TRB_TYPE_ENABLE_SLOT_CMD     = 9;
constexpr uint32_t TRB_TYPE_DISABLE_SLOT_CMD    = 10;
constexpr uint32_t TRB_TYPE_ADDRESS_DEVICE_CMD  = 11;
constexpr uint32_t TRB_TYPE_CONFIGURE_ENDPOINT_CMD = 12; // Milestone 4
constexpr uint32_t TRB_TYPE_RESET_ENDPOINT_CMD  = 14; // Фаза 8 (df) — восстановление bulk-эндпоинта после ошибки, xHCI 6.4.3.9
constexpr uint32_t TRB_TYPE_SET_TR_DEQUEUE_CMD  = 16; // xHCI 6.4.3.10, идёт СРАЗУ после Reset Endpoint
constexpr uint32_t TRB_TYPE_NO_OP_CMD           = 23;
constexpr uint32_t TRB_TYPE_TRANSFER_EVENT      = 32;
constexpr uint32_t TRB_TYPE_COMMAND_COMPLETION_EVENT = 33;
constexpr uint32_t TRB_TYPE_PORT_STATUS_CHANGE_EVENT = 34;
constexpr uint32_t TRB_TYPE_SETUP_STAGE = 2;
constexpr uint32_t TRB_TYPE_DATA_STAGE  = 3;
constexpr uint32_t TRB_TYPE_STATUS_STAGE = 4;
constexpr uint32_t TRB_TYPE_NORMAL      = 1; // Milestone 5+ (bulk-передачи), объявлена заранее

constexpr uint32_t TRB_CYCLE = (1u << 0);
constexpr uint32_t TRB_TC    = (1u << 1); // Toggle Cycle (только в Link TRB)
constexpr uint32_t TRB_IOC   = (1u << 5); // Interrupt On Completion

// --- Глобальное состояние ---
static volatile uint8_t *g_xhci_base = nullptr;
static volatile uint8_t *g_op_base = nullptr;
static volatile uint8_t *g_rt_base = nullptr;
static volatile uint8_t *g_db_base = nullptr;
static int g_ctx_size = 32; // 32 или 64 байта на контекст — см. HCCPARAMS1.CSZ

static inline volatile uint32_t* reg32(volatile uint8_t *base, uintptr_t off) {
    return (volatile uint32_t*)(base + off);
}

// Двадцать шестая попытка (см. ROADMAP.md) — 64-битные Operational/Runtime
// регистры xHCI (DCBAAP/CRCR/ERSTBA/ERDP) НЕЛЬЗЯ читать/писать одной
// 64-битной инструкцией на этом железе: живой readback DCBAAP сразу после
// записи показал НИЖНИЕ 32 бита, продублированные в ОБЕИХ половинах
// (`0x4000500040005000` вместо реально записанного `0x440005000`) — явный
// признак того, что регистр физически 32-битный по шине (сверено с самой
// xHCI-спецификацией — она прямо рекомендует раздельные 32-битные LO/HI
// доступы для платформ без нативной атомарной 64-битной шины, а не
// одну 64-битную STR/LDR). Пишем/читаем ВСЕГДА раздельно: LO по off,
// HI по off+4 — единственный универсально-корректный способ.
static inline void reg64_write_split(volatile uint8_t *base, uintptr_t off, uint64_t val) {
    *reg32(base, off)     = (uint32_t)(val & 0xFFFFFFFFu);
    *reg32(base, off + 4) = (uint32_t)(val >> 32);
}
static inline uint64_t reg64_read_split(volatile uint8_t *base, uintptr_t off) {
    uint32_t lo = *reg32(base, off);
    uint32_t hi = *reg32(base, off + 4);
    return ((uint64_t)hi << 32) | lo;
}

// Опрос с ограничением по wall-clock времени (тот же приём, что
// emmc_wait_irpt_bit() в blk_driver.cpp) — НИКОГДА не виснет навсегда.
static bool wait_ms(uint32_t timeout_ms, bool (*cond)()) {
    uint64_t deadline = read_cntvct() + (uint64_t)timeout_ms * g_cntfrq / 1000;
    while (!cond()) {
        if (read_cntvct() >= deadline) return false;
        seL4_Yield();
    }
    return true;
}

// Milestone 1 (закрытие Фазы 14, см. ROADMAP.md/план) — обобщённое TRB-
// кольцо "производителя" (Command Ring, EP0/Bulk-OUT/Bulk-IN Transfer
// Ring'и) — структура нужна как тип ПОЛЯ в UsbDeviceSlot ниже, поэтому
// определена ДО него (сами хелперы init_trb_ring()/ring_enqueue_trb() —
// дальше в файле, там же, где раньше).
struct TrbRing {
    volatile Trb *base = nullptr; // vaddr сегмента
    uint64_t dev_base = 0;        // device-видимый (см. to_dev_addr) физический адрес начала сегмента
    int enqueue_idx = 0;
    uint32_t pcs = 1;              // Producer Cycle State, см. xHCI 4.9.2
};

// --- Найденное устройство ---
struct UsbFoundDevice {
    bool found = false;
    uint16_t vendor_id = 0;
    uint16_t product_id = 0;
    uint8_t device_class = 0;
    uint8_t device_subclass = 0;
    uint8_t device_protocol = 0;
};

// Milestone 3 (закрытие Фазы 14, см. ROADMAP.md/план) — bulk-эндпоинты
// Mass Storage / SCSI Transparent / Bulk-Only Transport интерфейса,
// найденные разбором Configuration Descriptor (см. step9_get_configuration_descriptor).
struct UsbBulkEndpoints {
    bool found = false;
    uint8_t bulk_out_addr = 0;      // bEndpointAddress целиком (bit7=0)
    uint8_t bulk_in_addr = 0;       // bEndpointAddress целиком (bit7=1)
    uint16_t bulk_out_mps = 0;
    uint16_t bulk_in_mps = 0;
    uint8_t bulk_out_max_burst = 0; // из SS Endpoint Companion Descriptor, 0 если не SuperSpeed
    uint8_t bulk_in_max_burst = 0;
    uint8_t config_value = 0;       // bConfigurationValue — нужен для SET_CONFIGURATION (Milestone 4)
    uint8_t interface_num = 0;      // bInterfaceNumber Mass Storage интерфейса — нужен для BOT Reset (issuse.txt №14)
};

// Milestone 6 — найденная ёмкость устройства (см. step14_read_capacity).
struct UsbCapacity {
    bool found = false;
    uint32_t last_lba = 0;
    uint32_t block_size = 0;
};

// Фаза 15 (несколько накопителей одновременно, см. ROADMAP.md/план) —
// раньше ВСЁ состояние одного накопителя (found/bulk_eps/slot_id/
// partition_start_sector/EXFAT_Instance/mounted-флаг/volume_name/port/
// DCI/Transfer Ring'и) жило в отдельных одиночных глобалах — архитектурно
// поддерживалось РОВНО одно устройство. Теперь один слот = один
// накопитель, `g_usb_devices[USB_MAX_DEVICES]` — по числу физических
// разъёмов платы (см. план). Низкоуровневая xHCI-механика (TrbRing,
// bulk_transfer(), scsi_command(), wait_transfer_completion()) уже была
// параметризована явными аргументами — переделывать её не потребовалось,
// только код, который её вызывает (везде ниже добавлен параметр idx —
// индекс в этом массиве).
struct UsbDeviceSlot {
    bool in_use = false;   // слот занят (в процессе перечисления ИЛИ уже смонтирован) — см. find_free_device_slot()
    int port = 0;          // корневой root-порт (0 = не привязан; Фаза B добавит хаб-топологию)
    uint8_t slot_id = 0;   // xHCI Slot ID
    UsbFoundDevice found;
    UsbBulkEndpoints bulk_eps;
    UsbCapacity capacity;
    uint32_t partition_start_sector = 0;
    EXFAT_Instance fs;
    bool storage_mounted = false;
    // "usb0" — стабильный фоллбэк ПЕРВОГО слота (см. route_vfs_path() в
    // shell.cpp/h/sys_client.h) на случай скриптов/привычки; остальные
    // слоты фоллбэчат на "usbN".
    char volume_name[32] = "usb0";
    uint8_t bulk_out_dci = 0, bulk_in_dci = 0;
    TrbRing ep0_ring, bulkout_ring, bulkin_ring;
    // per-device paddr'ы (считаются в main() один раз из BOOT_USB_*_PADDR
    // — базового адреса — плюс idx*4096, см. h/platform.h).
    seL4_Word ep0_trring_paddr = 0, ctrl_buf_paddr = 0;
    seL4_Word bulkout_trring_paddr = 0, bulkin_trring_paddr = 0;
    seL4_Word cbw_csw_paddr = 0, bounce_paddr = 0;
    // Milestone B1 (Фаза 15) — слот занят ХАБОМ (found.device_class ==
    // USB_CLASS_HUB), не накопителем: bulk_eps/fs/storage_mounted
    // остаются в нулевом состоянии, эти два поля — единственное, что
    // реально используется. bPwrOn2PwrGood — в единицах по 2мс (см. USB
    // 2.0 spec 11.23.2.1), понадобится в B2 (сколько ждать после
    // включения питания порта хаба перед опросом статуса).
    uint8_t hub_num_ports = 0;
    uint8_t hub_pwr_on_to_pwr_good = 0;
    // Milestone B2 (доп.) — wHubCharacteristics биты 5-6 (см. USB 2.0
    // spec Table 11-13), нужно детям этого хаба для их Slot Context'а
    // (TT Think Time) в B3.
    uint8_t hub_tt_think_time = 0;
    // Milestone B3 (доп.) — Interrupt-эндпоинт хаба (статус портов).
    // Найдено на живом железе: без РЕАЛЬНОГО EP Context (не только
    // Slot-полей) Configure Endpoint не переводит Slot State хаба в
    // Configured (остаётся Addressed) — Address Device ребёнка падает с
    // Parameter Error, даже если Parent Hub Slot ID/Number of Ports
    // формально верны. Сама передача данных с этого эндпоинта (опрос
    // изменений портов) — Milestone B4, здесь только конфигурируется.
    uint8_t hub_int_ep_addr = 0;
    uint16_t hub_int_ep_mps = 0;
    uint8_t hub_int_ep_interval = 0;
    uint8_t hub_int_dci = 0; // вычислен в step_hub_configure_slot(), нужен B4 для доорбелла опроса
    uint64_t hub_int_pending_trb = 0; // Milestone B4 — device-адрес "слушающего" TRB, ждём его Transfer Event не блокируясь (try_check_transfer_complete)

    // Milestone B3 (Фаза 15) — если это устройство ЗА хабом (не на
    // корневом порту): parent_hub_idx — индекс ХАБА в этом же массиве,
    // остальные поля — то, что Slot Context (dword2, см. step7/step10)
    // должен нести ДАЛЬШЕ каждой командой, которая его переписывает
    // (Address Device, Configure Endpoint), иначе xHCI потеряет
    // хаб-топологию устройства. false/0 для корневых устройств —
    // поведение НЕ меняется относительно Фазы A/B1/B2.
    bool behind_hub = false;
    int parent_hub_idx = -1;
    uint8_t parent_hub_slot_id = 0;
    uint8_t parent_port_number = 0;
    bool parent_multi_tt = false;
    uint8_t parent_tt_think_time = 0;
    // issuse.txt №15 — НАЙДЕНО НА ЖИВОМ ЖЕЛЕЗЕ: parent_port_number один
    // сам по себе достаточен ТОЛЬКО для устройства на 1 уровне вложенности
    // (хаб напрямую на корневом порту). Для 2+ уровней (флешка -> хаб
    // пользователя -> встроенный root-hub VL805 -> корневой порт) нужен
    // ПОЛНЫЙ Route String (по нибблу на каждый ярус, см. xHCI 8.9/USB3
    // spec) и НАСТОЯЩИЙ корневой порт (не порт непосредственного
    // родителя, если сам родитель — тоже не на корневом порту). hub_tier
    // — номер яруса (0 для корневого устройства, 1 для хаба/устройства
    // сразу на корневом хабе, 2 для следующего уровня и т.д.) — считается
    // при перечислении (enumerate_device_behind_hub), наследуется от
    // родителя. route_string_full/root_port_full — уже готовые, полные
    // значения для Slot Context (dword0/dword1), не пересчитываются
    // заново в каждой команде из одного parent_port_number, как раньше.
    uint8_t hub_tier = 0;
    uint32_t route_string_full = 0;
    int root_port_full = 0;
};
static UsbDeviceSlot g_usb_devices[USB_MAX_DEVICES];

// Строит g_usb_devices[idx].volume_name из ASCII vendor[9]/product[17]
// (сырые, ещё пробельно-дополненные строки INQUIRY, как в
// step12_inquiry — SCSI space-pads короткие имена до фиксированной
// ширины поля). Обрезаем хвостовые пробелы у КАЖДОГО поля отдельно
// (иначе "Netac   " дало бы "Netac___-..."), оставшиеся пробелы ВНУТРИ
// имени меняем на '_' — это имя идёт частью пути (/mnt/<имя>), а шелл
// делит аргументы по пробелу.
static void update_usb_volume_name(int idx, const char* vendor, const char* product) {
    char *name = g_usb_devices[idx].volume_name;
    int vlen = 0; while (vendor[vlen] != '\0') vlen++;
    while (vlen > 0 && vendor[vlen - 1] == ' ') vlen--;
    int plen = 0; while (product[plen] != '\0') plen++;
    while (plen > 0 && product[plen - 1] == ' ') plen--;

    int out = 0;
    for (int i = 0; i < vlen && out < 31; i++) {
        name[out++] = (vendor[i] == ' ') ? '_' : vendor[i];
    }
    if (vlen > 0 && plen > 0 && out < 31) name[out++] = '-';
    for (int i = 0; i < plen && out < 31; i++) {
        name[out++] = (product[i] == ' ') ? '_' : product[i];
    }
    if (out == 0) { name[0]='u'; name[1]='s'; name[2]='b'; name[3]='0'+idx; out=4; }
    name[out] = '\0';
}

// --- DMA-регионы (фиксированные виртуальные адреса, см. platform.h;
// физические адреса приходят через BOOT_USB_*_PADDR при спавне). ---
static seL4_Word g_dcbaa_paddr, g_cmdring_paddr, g_erst_paddr, g_evtring_paddr;
// Фаза 15 — devctx_paddr_base/ep0_trring_paddr_base/... это БАЗА подряд
// идущих страниц (см. h/platform.h) — по одной на xHCI Slot ID (devctx)
// или на "наш" индекс устройства (остальные пять). Конкретный адрес —
// devctx_paddr_for(slot_id)/per-device поля в g_usb_devices[idx],
// заполняемые один раз в main() сразу после чтения этих баз.
static seL4_Word g_devctx_paddr_base, g_inputctx_paddr, g_scratchpad_arr_paddr;
static seL4_Word g_scratchpad_buf_paddr[USB_MAX_SCRATCHPAD_PAGES];
static int g_scratchpad_supplied = 0;
static seL4_Word g_ep0_trring_paddr_base;     // Milestone 1 — настоящий EP0 Transfer Ring
static seL4_Word g_ctrl_buf_paddr_base;       // Milestone 2 — буфер данных control-transfer'ов на EP0
static seL4_Word g_bulkout_trring_paddr_base, g_bulkin_trring_paddr_base; // Milestone 4
static seL4_Word g_cbw_csw_paddr_base, g_bounce_paddr_base; // Milestone 5

// Двадцать третья попытка (см. ROADMAP.md) — HSE на Шаге 6 объяснился:
// PCIE_MISC_RC_BAR2_CONFIG_LO/HI (входящее xHCI->RAM окно, читаны на Шаге 0)
// показали offset = 0x400000000 (16GiB), не 0, как предполагалось при
// проектировании DMA (см. platform.h комментарий "dma-ranges НЕ трогали").
// Устройство переводит СВОЙ ("bus"/"dev") адрес в CPU-адрес вычитанием
// этого offset'а (bar_offset = bus - cpu, см. brcm_pcie_set_inbound_windows()
// в ~/u-boot/drivers/pci/pcie_brcmstb.c) — значит, чтобы устройство попало
// РОВНО в наш g_*_paddr (реальный CPU-физический адрес страницы), ему нужно
// давать НЕ сырой g_*_paddr, а g_*_paddr + offset. Применяется КО ВСЕМ
// адресам, которые видит/пишет САМО устройство (DCBAAP, CRCR/Link TRB,
// ERSTBA/ERDP, Device/Input Context указатели, scratchpad) — но НЕ к MMIO
// (PLAT_XHCI_PADDR/PCIE_RC — та ОБРАТНАЯ, уже независимо работающая
// трансляция через outbound-окно 0) и не к vaddr-указателям, которыми
// читает/пишет сам процесс (dcbaa()/cmdring()/... остаются как есть).
constexpr uint64_t PCIE_INBOUND_DMA_OFFSET = 0x400000000ULL; // 16GiB, см. RC_BAR2_CONFIG_HI/LO на Шаге 0
static inline uint64_t to_dev_addr(uint64_t cpu_paddr) { return cpu_paddr + PCIE_INBOUND_DMA_OFFSET; }

static inline volatile uint64_t* dcbaa()      { return (volatile uint64_t*)PLAT_XHCI_DCBAA_VADDR; }
static inline volatile Trb*      cmdring()    { return (volatile Trb*)PLAT_XHCI_CMDRING_VADDR; }
static inline volatile uint8_t*  erst()       { return (volatile uint8_t*)PLAT_XHCI_ERST_VADDR; }
static inline volatile Trb*      evtring()    { return (volatile Trb*)PLAT_XHCI_EVTRING_VADDR; }
// Device Context software НИКОГДА не читает/пишет напрямую (контроллер
// сам заполняет её как результат Address Device/Configure Endpoint) —
// нужен только физический адрес (см. devctx_paddr_for() ниже), vaddr-
// аксессор не требуется.
static inline volatile uint32_t* inputctx()   { return (volatile uint32_t*)PLAT_XHCI_INPUTCTX_VADDR; } // общий, транзитный
static inline volatile uint64_t* scratchpad_arr() { return (volatile uint64_t*)PLAT_XHCI_SCRATCHPAD_ARR_VADDR; }

// Фаза 15 — per-device vaddr/paddr: idx — "наш" индекс устройства
// (0..USB_MAX_DEVICES-1), НЕ xHCI Slot ID. Все шесть ресурсов ниже —
// подряд идущие страницы (см. h/platform.h), поэтому просто base+idx*4096.
static inline volatile Trb*      ep0ring_vaddr(int idx)     { return (volatile Trb*)(PLAT_XHCI_EP0_TRRING_VADDR + (uintptr_t)idx * 4096); }
static inline volatile uint8_t*  ctrlbuf_vaddr(int idx)     { return (volatile uint8_t*)(PLAT_XHCI_CTRL_BUF_VADDR + (uintptr_t)idx * 4096); }
static inline volatile Trb*      bulkoutring_vaddr(int idx) { return (volatile Trb*)(PLAT_XHCI_BULKOUT_TRRING_VADDR + (uintptr_t)idx * 4096); }
static inline volatile Trb*      bulkinring_vaddr(int idx)  { return (volatile Trb*)(PLAT_XHCI_BULKIN_TRRING_VADDR + (uintptr_t)idx * 4096); }
static inline volatile uint8_t*  cbw_vaddr(int idx)         { return (volatile uint8_t*)(PLAT_XHCI_CBW_CSW_VADDR + (uintptr_t)idx * 4096); }
static inline volatile uint8_t*  csw_vaddr(int idx)         { return (volatile uint8_t*)(PLAT_XHCI_CBW_CSW_VADDR + (uintptr_t)idx * 4096 + 64); }
static inline volatile uint8_t*  bounce_vaddr(int idx)      { return (volatile uint8_t*)(PLAT_XHCI_BOUNCE_VADDR + (uintptr_t)idx * 4096); }

// Device Context — по одной странице на xHCI Slot ID (не на "наш" idx —
// хабы в Фазе B тоже получают Slot ID из того же пространства), см.
// USB_MAX_SLOTS_ENABLED/platform.h.
static inline seL4_Word devctx_paddr_for(uint8_t slot_id) { return g_devctx_paddr_base + (seL4_Word)slot_id * 4096; }
// Milestone B3 (Фаза 15, диагностика) — раньше vaddr-аксессор для Device
// Context был признан ненужным (software её не пишет, только paddr для
// dcbaa()) — понадобился ЖИВОЙ READBACK: после Configure Endpoint для
// хаба нужно убедиться, что контроллер РЕАЛЬНО записал туда Hub-бит/
// Number of Ports, а не гадать по одному коду завершения команды.
static inline volatile uint32_t* devctx_vaddr_for(uint8_t slot_id) { return (volatile uint32_t*)(PLAT_XHCI_DEVCTX_VADDR + (uintptr_t)slot_id * 4096); }

constexpr int CMDRING_TRB_COUNT = 4096 / 16; // 256
constexpr int EVTRING_TRB_COUNT = 4096 / 16; // 256
constexpr int TRB_RING_COUNT    = 4096 / 16; // 256 — общий размер для любого производящего (не Event) кольца, см. TrbRing ниже
// issuse.txt №15 — НАЙДЕНО НА ЖИВОМ ЖЕЛЕЗЕ: живой тест поймал полный
// зависон системы БЕЗ единого дальнейшего сообщения в логе и БЕЗ
// срабатывания watchdog'а usb_driver'а — печать останавливалась ровно
// после чтения битмапа interrupt-эндпоинта хаба, ДО следующего печатного
// вызова (hub_get_port_status()). Единственный цикл между ними без
// собственного дедлайна — внутренний `while (dequeue_event_trb(ev))`
// внутри wait_transfer_completion()/wait_command_completion()/
// try_check_transfer_complete() (и одноразового дренажа в
// poll_hub_interrupts()) — если event ring когда-либо попадёт в
// состояние, где dequeue_event_trb() бесконечно возвращает true (не
// понят точный триггер, но это ЕДИНСТВЕННОЕ место без верхней границы
// на число итераций), внешний дедлайн по read_cntvct() никогда не
// перепроверяется — настоящий зависон, не просто долгое ожидание.
// Защитный потолок ниже НЕ объясняет первопричину, но переводит отказ
// из "весь стенд намертво, watchdog не помогает" в чистый, ограниченный
// по времени возврат false с диагностикой — и даёт данные для следующего
// живого теста (какая функция и с каким dev.slot_id/hp упёрлась).
constexpr int EVT_RING_DRAIN_SANITY_CAP = 20000;

static int g_evt_dequeue_idx = 0;
static uint32_t g_evt_ccs = 1;

// Milestone 1 (закрытие Фазы 14, см. ROADMAP.md/план) — обобщённое TRB-кольцо
// "производителя" (Command Ring, и с этого момента — EP0 Transfer Ring;
// в будущих milestone'ах — Bulk-OUT/Bulk-IN Transfer Rings). Раньше Command
// Ring был единственным таким кольцом и вся бухгалтерия (enqueue-индекс,
// PCS) жила в паре голых глобальных переменных `g_cmd_enqueue_idx`/
// `g_cmd_pcs` — при появлении второго (EP0) и будущих третьего-четвёртого
// колец копипаст этой логики стал бы источником рассинхронизации. Doorbell
// НЕ звонится отсюда — у разных колец разные doorbell-таргеты (Command
// Ring — DB[0], Transfer Ring эндпоинта X — DB[slot] с Target=DCI), поэтому
// это ответственность вызывающего кода. (Само определение struct TrbRing
// переехало ВЫШЕ, перед UsbDeviceSlot — она его использует как тип поля.)

static void init_trb_ring(TrbRing &ring, volatile Trb *vaddr_base, uint64_t paddr) {
    for (int i = 0; i < TRB_RING_COUNT; i++) {
        vaddr_base[i].parameter = 0; vaddr_base[i].status = 0; vaddr_base[i].control = 0;
    }
    uint64_t dev_base = to_dev_addr(paddr);
    // Последний слот сегмента — Link TRB обратно на начало (обязательно
    // даже для одного сегмента, см. xHCI 4.9.2), Toggle Cycle бит установлен.
    volatile Trb *link = &vaddr_base[TRB_RING_COUNT - 1];
    link->parameter = dev_base;
    link->status = 0;
    link->control = trb_type(TRB_TYPE_LINK) | TRB_TC | TRB_CYCLE; // cycle=1 (PCS стартует с 1)
    ring.base = vaddr_base;
    ring.dev_base = dev_base;
    ring.enqueue_idx = 0;
    ring.pcs = 1;
}

// Кладёт TRB в кольцо, возвращает device-видимый физический адрес
// enqueued-слота (нужен вызывающему, чтобы сверять с полем parameter
// пришедшего потом Event TRB). Doorbell — ответственность вызывающего.
static uint64_t ring_enqueue_trb(TrbRing &ring, uint64_t parameter, uint32_t status, uint32_t control_no_cycle) {
    volatile Trb *slot = &ring.base[ring.enqueue_idx];
    uint64_t slot_dev_addr = ring.dev_base + (uint64_t)ring.enqueue_idx * 16;
    slot->parameter = parameter;
    slot->status = status;
    slot->control = control_no_cycle | (ring.pcs & TRB_CYCLE);
    ring.enqueue_idx++;
    if (ring.enqueue_idx == TRB_RING_COUNT - 1) {
        // Последний слот зарезервирован под Link TRB (см. init_trb_ring).
        // Аппаратура реально ЧИТАЕТ этот слот (не просто "перепрыгивает" его,
        // как наш программный enqueue-указатель) — её cycle-бит должен
        // совпадать с ТЕКУЩИМ PCS в момент, когда она до него дойдёт, иначе
        // на втором круге контроллер решит, что Link TRB ещё не наш, и
        // остановится. Обновляем его перед каждым переключением PCS — не
        // страшно, что в первый круг мы уже написали то же самое значение
        // при init_trb_ring().
        volatile Trb *link = &ring.base[TRB_RING_COUNT - 1];
        ring.pcs ^= 1;
        link->control = (link->control & ~TRB_CYCLE) | (ring.pcs & TRB_CYCLE);
        ring.enqueue_idx = 0;
    }
    return slot_dev_addr;
}

static TrbRing g_cmd_ring; // общий, контроллерный уровень — не per-device
// Фаза 15 — ep0/bulkout/bulkin Transfer Ring'и переехали в
// g_usb_devices[idx].ep0_ring/bulkout_ring/bulkin_ring (по одному набору
// на устройство, было — единственный общий).

// Кладёт команду в Command Ring и звонит в дверной звонок (DB[0]).
static void ring_command_doorbell() {
    *reg32(g_db_base, 0) = 0; // Target=0, StreamID=0 — команда
}
static uint64_t enqueue_command_trb(uint64_t parameter, uint32_t status, uint32_t control_no_cycle) {
    uint64_t addr = ring_enqueue_trb(g_cmd_ring, parameter, status, control_no_cycle);
    ring_command_doorbell();
    return addr;
}

// Milestone 2 — доорбелл ЭНДПОИНТА (не команды): Doorbell Array индексирован
// Slot ID (смещение slot_id*4 от g_db_base, см. xHCI 5.6), значение в
// младшем байте — DCI целевого эндпоинта (DCI=1 для EP0, независимо от
// направления — управляющий эндпоинт двунаправленный; для bulk-эндпоинтов
// в будущих milestone'ах DCI = 2*номер + (1 если IN иначе 0), см. xHCI 4.5.1).
static void ring_endpoint_doorbell(uint8_t slot_id, uint8_t dci) {
    *reg32(g_db_base, (uintptr_t)slot_id * 4) = dci;
}

// Читает следующее событие из Event Ring, если оно готово (cycle-бит
// совпадает с текущим CCS). Возвращает false, если событий нет.
static bool dequeue_event_trb(Trb &out) {
    volatile Trb *slot = &evtring()[g_evt_dequeue_idx];
    uint32_t control = slot->control;
    if ((control & TRB_CYCLE) != (g_evt_ccs & TRB_CYCLE)) return false;
    out.parameter = slot->parameter;
    out.status = slot->status;
    out.control = control;
    g_evt_dequeue_idx++;
    if (g_evt_dequeue_idx == EVTRING_TRB_COUNT) {
        // Один сегмент — аппаратура сама заворачивает ERDP на начало
        // сегмента, но CCS переключаем мы (см. xHCI 4.9.4).
        g_evt_dequeue_idx = 0;
        g_evt_ccs ^= 1;
    }
    return true;
}

// Сообщает контроллеру, докуда мы дочитали Event Ring (пишет ERDP,
// сбрасывая Event Handler Busy — см. xHCI 5.5.2.3.3).
static void update_erdp() {
    uint64_t addr = to_dev_addr(g_evtring_paddr + (uint64_t)g_evt_dequeue_idx * 16);
    reg64_write_split(g_rt_base, XHCI_RT_IR0 + XHCI_IR_ERDP, addr | ERDP_EHB);
}

// Опрашивает Event Ring, пока не найдёт Command Completion Event (по
// адресу конкретного TRB команды) либо не истечёт таймаут. Используется
// ТОЛЬКО во время bring-up (шаги 6-7) — синхронно, с понятным таймаутом,
// как emmc_send_cmd()/emmc_wait_irpt_bit() в blk_driver.cpp; штатный,
// событийный путь (через USB_EVENT_XHCI_IRQ) — уже после bring-up, в
// главном цикле (см. main() ниже).
static bool wait_command_completion(seL4_CPtr console_ep, uint64_t cmd_trb_paddr, uint32_t timeout_ms, uint8_t &completion_code, uint8_t &slot_id) {
    uint64_t deadline = read_cntvct() + (uint64_t)timeout_ms * g_cntfrq / 1000;
    int other_events = 0;
    uint32_t last_other_type = 0;
    uint64_t last_other_parameter = 0;
    uint32_t last_other_status = 0, last_other_control = 0;
    while (read_cntvct() < deadline) {
        Trb ev;
        while (dequeue_event_trb(ev)) {
            update_erdp();
            uint32_t type = (ev.control & TRB_TYPE_MASK) >> TRB_TYPE_SHIFT;
            if (type == TRB_TYPE_COMMAND_COMPLETION_EVENT && ev.parameter == cmd_trb_paddr) {
                completion_code = (uint8_t)(ev.status >> 24);
                slot_id = (uint8_t)(ev.control >> 24);
                return true;
            }
            // Port Status Change Event и прочее — на этом этапе игнорируем,
            // просто продвигаем ERDP (уже сделано выше), чтобы не заблокировать
            // очередь событий. Считаем и запоминаем ВСЕ поля — пригодится в
            // диагностике на таймауте (Двадцать девятая попытка).
            other_events++;
            last_other_type = type;
            last_other_parameter = ev.parameter;
            last_other_status = ev.status;
            last_other_control = ev.control;
            if (other_events >= EVT_RING_DRAIN_SANITY_CAP) {
                sys_puts(console_ep, "[USB]   ОШИБКА: дренаж event ring превысил защитный потолок (см. EVT_RING_DRAIN_SANITY_CAP) — обрываю.\n");
                break;
            }
        }
        seL4_Yield();
    }
    // Диагностика (Двадцать вторая попытка, см. ROADMAP.md) — на таймауте
    // печатаем USBSTS (HSE=bit2 — Host System Error, контроллер мог
    // молча остановиться после ошибки шины) и CRCR read-back (CRR=bit3 —
    // Command Ring Running, покажет, реально ли контроллер считает кольцо
    // запущенным), плюс сколько НЕ-completion событий видели (0 — кольцо
    // событий вообще молчит, >0 — какие-то события есть, но не наши).
    uint32_t usbsts = *reg32(g_op_base, XHCI_OP_USBSTS);
    uint64_t crcr = reg64_read_split(g_op_base, XHCI_OP_CRCR);
    if (LOG_USB) sys_puthex32(console_ep, "[USB]   DIAG timeout: USBSTS = ", usbsts);
    if (LOG_USB) sys_puts(console_ep, (usbsts & USBSTS_HSE) ? "[USB]     HSE = 1 (Host System Error!)\n" : "[USB]     HSE = 0\n");
    if (LOG_USB) sys_puthex64(console_ep, "[USB]   DIAG timeout: CRCR читается обратно как ", crcr);
    if (LOG_USB) sys_puts(console_ep, (crcr & (1ull << 3)) ? "[USB]     CRR = 1 (кольцо считается запущенным)\n" : "[USB]     CRR = 0 (кольцо НЕ запущено!)\n");
    if (LOG_USB) sys_puthex32(console_ep, "[USB]   DIAG прочих событий в кольце за время ожидания: ", (uint32_t)other_events);
    if (other_events > 0) {
        if (LOG_USB) sys_puthex32(console_ep, "[USB]   DIAG   тип последнего прочего события: ", last_other_type);
        if (LOG_USB) sys_puthex64(console_ep, "[USB]   DIAG   parameter = ", last_other_parameter);
        if (LOG_USB) sys_puthex32(console_ep, "[USB]   DIAG   status    = ", last_other_status);
        if (LOG_USB) sys_puthex32(console_ep, "[USB]   DIAG   control   = ", last_other_control);
    }
    return false;
}

// Milestone 2 — та же схема, что wait_command_completion(), но матчит
// Transfer Event TRB (не Command Completion): parameter события — адрес
// Setup/Data/Status/Normal TRB, который его вызвал. residual_len — сколько
// байт ИЗ ЗАПРОШЕННОЙ длины НЕ было передано (см. xHCI 6.4.2.1, поле TRB
// Transfer Length события) — вызывающий сам вычитает из запрошенной длины,
// чтобы получить реально переданное количество байт. Код завершения 13
// (Short Packet) — НЕ ошибка (устройство прислало меньше данных, чем мы
// предложили буфером — законно для GET_DESCRIPTOR/INQUIRY/READ CAPACITY),
// вызывающий сам решает, считать ли это успехом.
static bool wait_transfer_completion(seL4_CPtr console_ep, uint64_t trb_dev_addr, uint32_t timeout_ms,
                                      uint8_t &completion_code, uint32_t &residual_len) {
    uint64_t deadline = read_cntvct() + (uint64_t)timeout_ms * g_cntfrq / 1000;
    int other_events = 0;
    uint32_t last_other_type = 0;
    uint64_t last_other_parameter = 0;
    uint32_t last_other_status = 0, last_other_control = 0;
    while (read_cntvct() < deadline) {
        Trb ev;
        while (dequeue_event_trb(ev)) {
            update_erdp();
            uint32_t type = (ev.control & TRB_TYPE_MASK) >> TRB_TYPE_SHIFT;
            if (type == TRB_TYPE_TRANSFER_EVENT && ev.parameter == trb_dev_addr) {
                completion_code = (uint8_t)(ev.status >> 24);
                residual_len = ev.status & 0xFFFFFFu;
                return true;
            }
            // Прочие события (Command Completion от предыдущих шагов, Port
            // Status Change, Transfer Event ДРУГОГО TRB — например,
            // "потерянное" событие от предыдущего устройства/передачи) —
            // пропускаем, курсор уже продвинут выше, но ЗАПОМИНАЕМ для
            // диагностики на таймауте (см. тот же приём в
            // wait_command_completion() — раньше здесь эта диагностика
            // отсутствовала, что мешало отличить "Event Ring реально
            // молчит" от "события есть, просто не те, что ждём").
            other_events++;
            last_other_type = type;
            last_other_parameter = ev.parameter;
            last_other_status = ev.status;
            last_other_control = ev.control;
            if (other_events >= EVT_RING_DRAIN_SANITY_CAP) {
                sys_puts(console_ep, "[USB]   ОШИБКА: дренаж event ring превысил защитный потолок (см. EVT_RING_DRAIN_SANITY_CAP) — обрываю.\n");
                break;
            }
        }
        seL4_Yield();
    }
    uint32_t usbsts = *reg32(g_op_base, XHCI_OP_USBSTS);
    sys_puthex64(console_ep, "[USB]   DIAG transfer timeout: ожидали TRB по адресу ", trb_dev_addr);
    sys_puthex32(console_ep, "[USB]   DIAG transfer timeout: USBSTS = ", usbsts);
    sys_puts(console_ep, (usbsts & USBSTS_HSE) ? "[USB]     HSE = 1 (Host System Error!)\n" : "[USB]     HSE = 0\n");
    sys_puthex32(console_ep, "[USB]   DIAG прочих событий в кольце за время ожидания: ", (uint32_t)other_events);
    if (other_events > 0) {
        sys_puthex32(console_ep, "[USB]   DIAG   тип последнего прочего события: ", last_other_type);
        sys_puthex64(console_ep, "[USB]   DIAG   parameter = ", last_other_parameter);
        sys_puthex32(console_ep, "[USB]   DIAG   status    = ", last_other_status);
        sys_puthex32(console_ep, "[USB]   DIAG   control   = ", last_other_control);
    }
    return false;
}

// Milestone B4 (Фаза 15) — НЕ-блокирующий вариант wait_transfer_completion():
// одна проверка Event Ring (весь готовый "урожай" сразу, не только
// первое совпадение — Port Status Change и прочие события тоже нужно
// продвинуть, курсор общий), БЕЗ ожидания/таймаута. Для periodic
// interrupt-эндпоинта хаба (статус downstream-портов) — опрашивается раз
// в heartbeat-тик (см. poll_hub_interrupts()), а не блокирующим
// wait_transfer_completion(), который держал бы весь usb_driver
// (единственный процесс, обслуживающий И VFS-команды, И hot-plug)
// колом до 1с на КАЖДЫЙ тик, даже когда хаб молчит (типичный случай).
static bool try_check_transfer_complete(uint64_t trb_dev_addr, uint8_t &completion_code, uint32_t &residual_len) {
    bool found = false;
    Trb ev;
    int drained = 0;
    while (dequeue_event_trb(ev)) {
        update_erdp();
        uint32_t type = (ev.control & TRB_TYPE_MASK) >> TRB_TYPE_SHIFT;
        if (type == TRB_TYPE_TRANSFER_EVENT && ev.parameter == trb_dev_addr) {
            completion_code = (uint8_t)(ev.status >> 24);
            residual_len = ev.status & 0xFFFFFFu;
            found = true;
        }
        // Прочие события (Command Completion, Port Status Change,
        // Transfer Event другого TRB) — молча пропускаем, курсор уже
        // продвинут выше update_erdp(). Port Status Change событиям
        // от КОРНЕВЫХ портов это не мешает — их основной путь всё
        // равно polling (poll_ports_for_hotplug), не событийный (см.
        // Milestone 11, живая находка про ненадёжность событий).
        if (++drained >= EVT_RING_DRAIN_SANITY_CAP) {
            sys_puts(g_console_ep, "[USB]   ОШИБКА: дренаж event ring (try_check_transfer_complete) превысил защитный потолок — обрываю.\n");
            break;
        }
    }
    return found;
}

// issuse.txt №15 — асинхронный, тик-возобновляемый вариант control-
// transfer'а (ep0_control_in()/ep0_control_no_data() ЦЕЛИКОМ остаются
// нетронутыми, синхронными — этот код НИЧЕГО в них не меняет, чистое
// добавление рядом). Setup+Data+Status TRB енкьюжатся СРАЗУ (та же схема,
// что уже в ep0_control_in — контроллер сам проигрывает их по очереди,
// одного doorbell'а достаточно), поэтому "асинхронность" — это просто
// замена двух блокирующих wait_transfer_completion() на два тик-опроса
// try_check_transfer_complete() (уже существующий неблокирующий примитив,
// см. выше) с собственным дедлайном на каждой стадии. Мотивация — см.
// HubConnAsync ниже: настоящая причина issuse.txt №15 в том, что
// blocking control-transfer'ы происходят ВНУТРИ функции, которую
// poll_hub_interrupts() зовёт синхронно каждый heartbeat-тик, так что
// сделать "внутренний" wait неблокирующим бессмысленно, если вызывающая
// функция всё равно не возвращается в диспетчер между тиками — отсюда и
// необходимость поднять асинхронность на уровень ВСЕЙ последовательности
// (см. HubConnAsync), а не только одного control-transfer'а.
struct AsyncCtrl {
    enum St : uint8_t { IDLE, WAITING, DONE, FAILED } state = IDLE;
    uint64_t data_trb_addr = 0;   // 0, если у запроса нет Data Stage
    uint64_t status_trb_addr = 0;
    uint64_t deadline = 0;        // cntvct-тики
    uint32_t wlength = 0;
    uint32_t actual_length = 0;   // валидно только при state==DONE
    bool data_done = false;       // true сразу при старте, если Data Stage нет вообще
    uint8_t data_cc = 0;
    uint32_t data_residual = 0;
};

// issuse.txt №15 — НАЙДЕНО НА ЖИВОМ ЖЕЛЕЗЕ: Data и Status TRB енкьюжатся
// ВМЕСТЕ (см. *_start() ниже) и контроллер обычно завершает их почти
// одновременно — первая версия ждала ИХ ПООЧЕРЁДНО (WAIT_DATA, потом
// WAIT_STATUS), через try_check_transfer_complete(), который дренирует
// ВЕСЬ event ring за один вызов и ищет только ОДИН целевой адрес. Если
// оба события уже лежали в кольце к моменту первого опроса — событие
// Status Stage тихо ВЫБРАСЫВАЛОСЬ (не совпало с искомым Data-адресом),
// и второй, отдельный опрос на WAIT_STATUS никогда его не находил —
// висело до дедлайна на КАЖДОМ control-transfer'е с Data Stage (GET_
// PORT_STATUS). Фикс — один общий WAITING-статус, каждый тик дренирует
// кольцо ОДИН раз и проверяет ОБА адреса сразу (см. цикл ниже) — то же
// самое кольцо, тот же courtsor, но ни одно событие больше не теряется,
// в каком бы порядке/пачками они ни пришли.
static void async_ctrl_in_start(AsyncCtrl &x, TrbRing &ep0_ring, uint8_t slot_id, uint8_t bmRequestType,
                                 uint8_t bRequest, uint16_t wValue, uint16_t wIndex, uint16_t wLength,
                                 uint64_t buffer_paddr) {
    uint64_t setup = (uint64_t)bmRequestType | ((uint64_t)bRequest << 8) | ((uint64_t)wValue << 16)
                    | ((uint64_t)wIndex << 32) | ((uint64_t)wLength << 48);
    ring_enqueue_trb(ep0_ring, setup, 8u,
                      trb_type(TRB_TYPE_SETUP_STAGE) | (1u << 6) /*IDT*/ | (3u << 16) /*TRT=IN Data Stage*/);
    x.data_trb_addr = ring_enqueue_trb(ep0_ring, to_dev_addr(buffer_paddr), (uint32_t)wLength,
                      trb_type(TRB_TYPE_DATA_STAGE) | (1u << 16) /*DIR=IN*/ | (1u << 2) /*ISP*/ | TRB_IOC);
    x.status_trb_addr = ring_enqueue_trb(ep0_ring, 0, 0,
                      trb_type(TRB_TYPE_STATUS_STAGE) | TRB_IOC);
    ring_endpoint_doorbell(slot_id, 1);
    x.wlength = wLength;
    x.data_done = false;
    x.deadline = read_cntvct() + 500ull * g_cntfrq / 1000;
    x.state = AsyncCtrl::WAITING;
}

static void async_ctrl_no_data_start(AsyncCtrl &x, TrbRing &ep0_ring, uint8_t slot_id, uint8_t bmRequestType,
                                      uint8_t bRequest, uint16_t wValue, uint16_t wIndex) {
    uint64_t setup = (uint64_t)bmRequestType | ((uint64_t)bRequest << 8) | ((uint64_t)wValue << 16)
                    | ((uint64_t)wIndex << 32);
    ring_enqueue_trb(ep0_ring, setup, 8u,
                      trb_type(TRB_TYPE_SETUP_STAGE) | (1u << 6) /*IDT*/ | (0u << 16) /*TRT=No Data Stage*/);
    x.data_trb_addr = 0;
    x.status_trb_addr = ring_enqueue_trb(ep0_ring, 0, 0,
                      trb_type(TRB_TYPE_STATUS_STAGE) | (1u << 16) /*DIR=IN — обязателен без Data Stage*/ | TRB_IOC);
    ring_endpoint_doorbell(slot_id, 1);
    x.data_done = true; // нет Data Stage — нечего ждать
    x.deadline = read_cntvct() + 500ull * g_cntfrq / 1000;
    x.state = AsyncCtrl::WAITING;
}

// Один шаг вперёд. Возвращает true, когда state стал терминальным
// (DONE или FAILED) — вызывающий смотрит x.state, чтобы различить их.
// НЕ блокируется НИКОГДА — при "ещё не готово" просто возвращает false.
// Один проход по event ring'у за тик, проверяет ОБА адреса разом (см.
// комментарий у struct AsyncCtrl выше) — не теряет событие, даже если
// Data и Status Stage завершились в один и тот же дрейн.
static bool async_ctrl_tick(AsyncCtrl &x) {
    if (x.state != AsyncCtrl::WAITING) return true; // IDLE/DONE/FAILED — уже терминально

    bool status_done = false;
    uint8_t status_cc = 0; uint32_t status_residual = 0;
    Trb ev;
    int drained = 0;
    while (dequeue_event_trb(ev)) {
        update_erdp();
        uint32_t type = (ev.control & TRB_TYPE_MASK) >> TRB_TYPE_SHIFT;
        if (type != TRB_TYPE_TRANSFER_EVENT) continue;
        if (!x.data_done && x.data_trb_addr != 0 && ev.parameter == x.data_trb_addr) {
            x.data_done = true;
            x.data_cc = (uint8_t)(ev.status >> 24);
            x.data_residual = ev.status & 0xFFFFFFu;
        } else if (ev.parameter == x.status_trb_addr) {
            status_done = true;
            status_cc = (uint8_t)(ev.status >> 24);
            status_residual = ev.status & 0xFFFFFFu;
        }
        if (++drained >= EVT_RING_DRAIN_SANITY_CAP) {
            sys_puts(g_console_ep, "[USB]   ОШИБКА: дренаж event ring (async_ctrl_tick) превысил защитный потолок — обрываю.\n");
            break;
        }
    }

    if (!x.data_done) {
        if (read_cntvct() >= x.deadline) { x.state = AsyncCtrl::FAILED; return true; }
        return false; // Data Stage ещё не завершился — Status физически не мог завершиться раньше
    }
    if (x.data_trb_addr != 0 && x.data_cc != 1 && x.data_cc != 13) { // код см. ep0_control_in()
        x.state = AsyncCtrl::FAILED; return true;
    }
    if (x.data_trb_addr != 0) x.actual_length = x.wlength - x.data_residual;

    if (!status_done) {
        if (read_cntvct() >= x.deadline) { x.state = AsyncCtrl::FAILED; return true; }
        return false;
    }
    x.state = (status_cc == 1) ? AsyncCtrl::DONE : AsyncCtrl::FAILED;
    return true;
}

// === Шаги bring-up (см. ROADMAP.md "Порядок bring-up") ===

// Шаг 0: раньше (Пятнадцатая-Семнадцатая попытки, см. ROADMAP.md)
// программировали ВТОРОЕ outbound-окно моста (индекс 1), потому что
// единственное окно U-Boot'а (индекс 0, bus 0xc0000000 -> CPU
// 0x600000000) считалось "недоступным seL4". Восемнадцатая попытка
// показала, что это было неверно (0x600000000 — обычный device-Untyped,
// KernelPaddrUserTop покрывает его) — используем окно 0 напрямую
// (PLAT_XHCI_PADDR = 0x600000000, см. platform.h), окно 1 и вся его
// регистровая арифметика (offsets/маски WIN1_*) больше не нужны.

static volatile uint8_t *g_pcie_rc_base = nullptr;

// Двадцать четвёртая попытка (см. ROADMAP.md) — ПРОБОВАЛИ читать
// PCIE_OUTB_ERR_VALID/ACC_INFO/MEM_CAUSE/MEM_ADDR_LO/HI (offset 0x6000+ от
// RPI4_PCIE_PADDR), взятые из АПСТРИМ Linux (drivers/pci/controller/
// pcie-brcmstb.c). Живое железо: САМО ЧТЕНИЕ PCIE_OUTB_ERR_VALID вызвало
// настоящий SError/halt ядра (тот же почерк, что при обращении по неверному
// PCIe-адресу в Пятнадцатой попытке) — этого регистрового блока физически
// НЕТ на BCM2711 (апстримный драйвер, судя по всему, покрывает более новые
// чипы, BCM2712+, а не только наш). НЕ ВОССТАНАВЛИВАТЬ без предварительной
// сверки конкретно под BCM2711 (см. память проекта "Check feasibility
// first") — код читающий этот диапазон полностью убран, PLAT_PCIE_ERR_*
// (platform.h) и связанный маппинг в main.cpp оставлены только как
// задокументированный, но НЕ вызываемый путь.

// Шаг 0 (Девятнадцатая попытка, см. ROADMAP.md) — БЫЛО: программирование
// второго outbound-окна (индекс 1) + подавление UBUS-ошибок
// (DECERR_DIS/ERR_DIS). С переходом на окно 0 (0x600000000, см.
// PLAT_XHCI_PADDR/platform.h) обе части оказались НЕ просто лишними, а
// вредными: (1) окно 1 программировалось на ТОТ ЖЕ bus-диапазон
// (0xC0000000), которым уже владеет окно 0 — два окна, одна и та же
// трансляция, прямо перед чтением через окно 0; (2) UBUS_CTRL.DECERR_DIS
// (бит 19) — по даташиту это буквально "подавлять decode error, возвращать
// 0 вместо него" — значит "мёртвые нули", которые читались в Восемнадцатой
// попытке (CAPLENGTH=0x00000000 и т.д.), могли быть НЕ живыми данными
// устройства, а ТИХО ПОДАВЛЕННОЙ ошибкой декодирования. Обе причины убраны
// целиком — оставлен только read-only опрос PCIE_MISC_PCIE_STATUS (линк),
// чтобы получить честный, неподавленный результат следующего чтения через
// окно 0 (реальные данные, классический PCI "0xFFFFFFFF", или настоящий
// SError — любой из трёх сигналов информативнее тихого нуля).
static void step0_check_link(seL4_CPtr console_ep) {
    if (LOG_USB) sys_puts(console_ep, "[USB] Шаг 0: проверяю статус линка PCIe (без модификации регистров моста) ...\n");
    uint32_t pcie_status = *reg32(g_pcie_rc_base, 0x4068 - 0x4000); // PCIE_MISC_PCIE_STATUS
    if (LOG_USB) sys_puthex32(console_ep, "[USB]   PCIE_MISC_PCIE_STATUS = ", pcie_status);
    if (LOG_USB) sys_puts(console_ep, (pcie_status & (1u << 4)) ? "[USB]     PHYLINKUP = 1 (физический линк поднят)\n"
                                                     : "[USB]     PHYLINKUP = 0 (физического линка НЕТ)\n");
    if (LOG_USB) sys_puts(console_ep, (pcie_status & (1u << 5)) ? "[USB]     DL_ACTIVE = 1 (data link layer активен)\n"
                                                     : "[USB]     DL_ACTIVE = 0 (data link layer НЕ активен)\n");

    // Двадцать вторая попытка (см. ROADMAP.md) — HSE (Host System Error) на
    // Шаге 6 указывает на проблему ВХОДЯЩЕГО (xHCI -> RAM DMA) пути, не
    // исходящего (CPU -> xHCI регистры, уже подтверждённо рабочего).
    // Входящее окно PCIe RC — RC_BAR2_CONFIG_LO/HI (сверено с исходником
    // U-Boot, ~/u-boot/drivers/pci/pcie_brcmstb.c:
    // brcm_pcie_set_inbound_windows(), НЕ по памяти) — read-only, безопасно
    // всегда. Для BCM2711 CONFIG_LO хранит смещение (bus-cpu) в старших
    // битах + закодированный размер в младших 5 битах (см.
    // brcm_pcie_encode_ibar_size()); из dma-ranges DTB (bus=0x0, cpu=0x0,
    // size=0xC0000000) ожидаем смещение=0, размер округлён вверх до 4GB
    // (log2=32 -> код 0x11).
    uint32_t bar2_lo = *reg32(g_pcie_rc_base, 0x4034 - 0x4000);
    uint32_t bar2_hi = *reg32(g_pcie_rc_base, 0x4038 - 0x4000);
    if (LOG_USB) sys_puthex32(console_ep, "[USB]   DIAG RC_BAR2_CONFIG_LO = ", bar2_lo);
    if (LOG_USB) sys_puthex32(console_ep, "[USB]   DIAG RC_BAR2_CONFIG_HI = ", bar2_hi);
    if (LOG_USB) sys_puthex32(console_ep, "[USB]   DIAG RC_BAR2 закодированный размер (низкие 5 бит) = ", bar2_lo & 0x1Fu);
    // MISC_CTRL (0x4008) — бит SCB_ACCESS_EN должен быть 1, иначе входящие
    // DMA-транзакции в системную память вообще запрещены на уровне моста.
    uint32_t misc_ctrl = *reg32(g_pcie_rc_base, 0x4008 - 0x4000);
    if (LOG_USB) sys_puthex32(console_ep, "[USB]   DIAG PCIE_MISC_MISC_CTRL = ", misc_ctrl);
    if (LOG_USB) sys_puts(console_ep, (misc_ctrl & (1u << 12)) ? "[USB]     SCB_ACCESS_EN = 1\n" : "[USB]     SCB_ACCESS_EN = 0 (входящий DMA запрещён!)\n");
}

// Шаг 1: регистры читаются осмысленно.
static bool step1_read_capabilities(seL4_CPtr console_ep, uint8_t &caplen, uint32_t &hcsparams1,
                                     uint32_t &hcsparams2, uint32_t &hccparams1) {
    // Печатаем ПЕРЕД каждым отдельным чтением (не одним блоком после всех
    // четырёх, как раньше) — первая живая попытка (см. ROADMAP.md) зависла
    // без единой строки "Шаг 1", а параллельно оборвалось на середине
    // сообщение ДРУГОГО процесса (timer_driver) — похоже на зависание всей
    // системы на шинном уровне при первом же обращении к xHCI, а не на
    // page fault (иначе root's watchdog успел бы что-то напечатать). Раз
    // так — нужна гранулярность "на каком именно регистре встали", а не
    // "дошли до Шага 1 или нет" целиком.
    if (LOG_USB) sys_puts(console_ep, "[USB] Шаг 1: читаю CAPLENGTH...\n");
    caplen = *(volatile uint8_t*)(g_xhci_base + XHCI_CAPLENGTH);
    if (LOG_USB) sys_puthex32(console_ep, "[USB]   CAPLENGTH  = ", caplen);
    if (LOG_USB) sys_puts(console_ep, "[USB]   читаю HCSPARAMS1...\n");
    hcsparams1 = *reg32(g_xhci_base, XHCI_HCSPARAMS1);
    if (LOG_USB) sys_puthex32(console_ep, "[USB]   HCSPARAMS1 = ", hcsparams1);
    if (LOG_USB) sys_puts(console_ep, "[USB]   читаю HCSPARAMS2...\n");
    hcsparams2 = *reg32(g_xhci_base, XHCI_HCSPARAMS2);
    if (LOG_USB) sys_puthex32(console_ep, "[USB]   HCSPARAMS2 = ", hcsparams2);
    if (LOG_USB) sys_puts(console_ep, "[USB]   читаю HCCPARAMS1...\n");
    hccparams1 = *reg32(g_xhci_base, XHCI_HCCPARAMS1);
    if (LOG_USB) sys_puthex32(console_ep, "[USB]   HCCPARAMS1 = ", hccparams1);
    // 0x00000000 и 0xFFFFFFFF — типичные признаки "MMIO-алиас не живой"
    // (не подключено/не запитано железо) — см. ROADMAP.md, проверка шага 1.
    if (caplen == 0 || hcsparams1 == 0 || hcsparams1 == 0xFFFFFFFFu) {
        sys_puts(console_ep, "[USB] ОШИБКА: capability-регистры выглядят мёртвыми (0 или 0xFFFFFFFF) — MMIO-алиас не отвечает.\n");
        return false;
    }
    return true;
}

// Шаг 2: HCRST, дождаться CNR==0.
static bool g_wait_cnr_flag;
static bool cond_cnr_clear() { return (*reg32(g_op_base, XHCI_OP_USBSTS) & USBSTS_CNR) == 0; }
static bool cond_hcrst_clear() { return (*reg32(g_op_base, XHCI_OP_USBCMD) & USBCMD_HCRST) == 0; }
static bool step2_reset(seL4_CPtr console_ep) {
    if (LOG_USB) sys_puts(console_ep, "[USB] Шаг 2: HCRST.\n");
    *reg32(g_op_base, XHCI_OP_USBCMD) |= USBCMD_HCRST;
    if (!wait_ms(1000, cond_hcrst_clear)) {
        sys_puts(console_ep, "[USB] ОШИБКА: USBCMD.HCRST не сбросился за 1с.\n");
        return false;
    }
    if (!wait_ms(1000, cond_cnr_clear)) {
        sys_puts(console_ep, "[USB] ОШИБКА: USBSTS.CNR не очистился за 1с (контроллер не готов).\n");
        return false;
    }
    if (LOG_USB) sys_puts(console_ep, "[USB]   HCRST завершён, CNR очищен.\n");
    return true;
}

// Инициализация Command Ring — теперь тонкая обёртка над общим init_trb_ring().
static void init_command_ring() {
    init_trb_ring(g_cmd_ring, cmdring(), g_cmdring_paddr);
}

// Инициализация Event Ring — 1 сегмент (256 TRB), ERST из одной записи.
static void init_event_ring() {
    for (int i = 0; i < EVTRING_TRB_COUNT; i++) {
        evtring()[i].parameter = 0; evtring()[i].status = 0; evtring()[i].control = 0;
    }
    // ERST-запись: Ring Segment Base Address(u64) + Ring Segment Size(u32, только младшие 16 бит) + резерв(u32)
    volatile uint64_t *erst_base = (volatile uint64_t*)erst();
    volatile uint32_t *erst_size = (volatile uint32_t*)(erst() + 8);
    erst_base[0] = to_dev_addr(g_evtring_paddr);
    erst_size[0] = EVTRING_TRB_COUNT;
    erst_size[1] = 0;
    g_evt_dequeue_idx = 0;
    g_evt_ccs = 1;

    *reg32(g_rt_base, XHCI_RT_IR0 + XHCI_IR_ERSTSZ) = 1; // один сегмент
    reg64_write_split(g_rt_base, XHCI_RT_IR0 + XHCI_IR_ERSTBA, to_dev_addr(g_erst_paddr));
    reg64_write_split(g_rt_base, XHCI_RT_IR0 + XHCI_IR_ERDP, to_dev_addr(g_evtring_paddr));
    *reg32(g_rt_base, XHCI_RT_IR0 + XHCI_IR_IMAN) |= IMAN_IE; // включить прерывания интерраптера 0
}

// Шаг 3: DCBAAP/Command Ring/ERST/ERDP.
static bool step3_setup_rings(seL4_CPtr console_ep, uint32_t hcsparams2) {
    if (LOG_USB) sys_puts(console_ep, "[USB] Шаг 3: DCBAAP/Command Ring/Event Ring.\n");

    for (int i = 0; i < 512; i++) dcbaa()[i] = 0;

    // Scratchpad Buffer Array — нужен, только если контроллер просит (см.
    // HCSPARAMS2, Max Scratchpad Buffers = Hi[25:21]<<5 | Lo[31:27]>>27).
    uint32_t max_scratch = ((hcsparams2 >> 27) & 0x1F) | (((hcsparams2 >> 21) & 0x1F) << 5);
    if (max_scratch > 0) {
        if ((int)max_scratch > g_scratchpad_supplied) {
            sys_puthex32(console_ep, "[USB] ОШИБКА: контроллеру нужно scratchpad-страниц больше бюджета фазы: ", max_scratch);
            if (LOG_USB) sys_puts(console_ep, "[USB]   см. USB_MAX_SCRATCHPAD_PAGES в platform.h — отказ, а не динамический аллокатор.\n");
            return false;
        }
        for (uint32_t i = 0; i < max_scratch; i++) {
            scratchpad_arr()[i] = to_dev_addr(g_scratchpad_buf_paddr[i]);
        }
        dcbaa()[0] = to_dev_addr(g_scratchpad_arr_paddr); // запись 0 DCBAA — указатель на scratchpad-массив (см. xHCI 6.1)
    }

    init_command_ring();
    init_event_ring();

    reg64_write_split(g_op_base, XHCI_OP_DCBAAP, to_dev_addr(g_dcbaa_paddr));
    reg64_write_split(g_op_base, XHCI_OP_CRCR, to_dev_addr(g_cmdring_paddr) | 1); // RCS=1 (Ring Cycle State стартует с 1, см. xHCI 5.4.5)
    // Фаза 15 (несколько накопителей, см. ROADMAP.md/план) — было
    // MaxSlotsEn=1 (ровно один слот единовременно); теперь
    // USB_MAX_SLOTS_ENABLED (см. platform.h) — с запасом сверх
    // USB_MAX_DEVICES под сами хабы (Фаза B, у хаба тоже есть свой Slot ID).
    *reg32(g_op_base, XHCI_OP_CONFIG) = USB_MAX_SLOTS_ENABLED;

    // Двадцать пятая попытка (см. ROADMAP.md) — читаем DCBAAP/CRCR ОБРАТНО
    // СРАЗУ после записи, ДО HCRST-смежных операций/доорбелла — обе
    // предыдущие гипотезы про адрес (сырой и +16GiB) дали ПОБАЙТОВО
    // идентичный HSE, что подозрительно похоже на "проблема не в значении
    // адреса вообще". Это ещё исходящее направление (те же регистры,
    // которые уже подтверждённо читаются/пишутся нормально в Шагах 1-2) —
    // безопасно, риска повторить крах Двадцать четвёртой попытки нет.
    uint64_t dcbaap_rb = reg64_read_split(g_op_base, XHCI_OP_DCBAAP);
    uint64_t crcr_rb = reg64_read_split(g_op_base, XHCI_OP_CRCR);
    if (LOG_USB) sys_puthex64(console_ep, "[USB]   DIAG DCBAAP сразу после записи = ", dcbaap_rb);
    if (LOG_USB) sys_puthex64(console_ep, "[USB]   DIAG CRCR сразу после записи   = ", crcr_rb);

    if (LOG_USB) sys_puts(console_ep, "[USB]   Кольца настроены.\n");
    return true;
}

// Шаг 4: USBCMD.RS=1, дождаться HCH==0.
static bool cond_hch_clear() { return (*reg32(g_op_base, XHCI_OP_USBSTS) & USBSTS_HCH) == 0; }
static bool step4_run(seL4_CPtr console_ep) {
    if (LOG_USB) sys_puts(console_ep, "[USB] Шаг 4: USBCMD.RS=1 (run).\n");
    // USBCMD.INTE (бит 2) — общее разрешение прерываний контроллера,
    // отдельно от IMAN.IE конкретного интерраптера (уже включён в
    // init_event_ring()) — оба нужны, чтобы реальный IRQ дошёл до GIC.
    *reg32(g_op_base, XHCI_OP_USBCMD) |= USBCMD_RS | USBCMD_INTE;
    if (!wait_ms(1000, cond_hch_clear)) {
        sys_puts(console_ep, "[USB] ОШИБКА: USBSTS.HCH не погас за 1с — контроллер не запустился.\n");
        return false;
    }
    if (LOG_USB) sys_puts(console_ep, "[USB]   Контроллер запущен (HCH=0).\n");
    return true;
}

// Шаг 5: PORTSC КОНКРЕТНОГО порта — Port Reset, дождаться PED.
//
// Milestone 1 (закрытие Фазы 14, см. ROADMAP.md/план) — раньше эта функция
// САМА сканировала все порты и брала первый с CCS=1 (сканирование теперь
// отдельно, см. run_bring_up()/try_enumerate_port() ниже). Параметризация
// конкретным портом
// нужна для hot-plug (будущий Milestone 11): там порт уже известен из
// Port Status Change Event, повторное сканирование всех портов было бы не
// просто лишним, а НЕВЕРНЫМ на живой системе с несколькими портами (другие
// порты с CCS=1 — это уже смонтированные устройства, не кандидаты на
// "попробовать вместо этого"). Раньше при неудаче PED-ожидания код пробовал
// СЛЕДУЮЩИЙ порт со сканирования — сейчас просто возвращает false; в
// реальности (одна флешка, один порт) это не меняет наблюдаемое поведение.
static int g_wait_port;
static bool cond_port_ped() { return (*reg32(g_op_base, XHCI_OP_PORTSC_BASE + (uintptr_t)(g_wait_port - 1) * 0x10) & PORTSC_PED) != 0; }
static bool step5_port_reset(seL4_CPtr console_ep, int port) {
    uintptr_t off = XHCI_OP_PORTSC_BASE + (uintptr_t)(port - 1) * 0x10;
    uint32_t portsc = *reg32(g_op_base, off);
    g_wait_port = port;

    // Milestone 2 (находка на живом железе, см. ROADMAP.md) — порт 2
    // (SuperSpeed, PLS сразу после подключения показывал 5=RxDetect —
    // переходное состояние link-training) НЕ перешёл в PED=1 за 500мс
    // после явного Port Reset. SS-порты xHCI включаются АВТОМАТИЧЕСКИ по
    // завершении link-training (переход в U0), БЕЗ явного Port Reset —
    // это принципиально не то же самое, что USB2, где reset обязателен
    // для перехода в Default-состояние. Даём порту время дойти до
    // PED=1 САМО ПО СЕБЕ первым; явный Port Reset — только если этого не
    // случилось (покрывает USB2-путь, ровно как раньше).
    if (portsc & PORTSC_PED) {
        if (LOG_USB) sys_puts(console_ep, "[USB]   Порт уже включён (PED=1), явный сброс не нужен.\n");
    } else if (wait_ms(200, cond_port_ped)) {
        if (LOG_USB) sys_puts(console_ep, "[USB]   Порт включился сам (PED=1) — SuperSpeed auto-enable, без явного Port Reset.\n");
    } else {
        if (LOG_USB) sys_puts(console_ep, "[USB]   Порт не включился сам за 200мс — пробую явный Port Reset (USB2-путь).\n");
        portsc = *reg32(g_op_base, off);
        // Port Reset — RW1C-биты сохраняем нетронутыми (не пишем 0 в PP/PED).
        *reg32(g_op_base, off) = (portsc & ~PORTSC_RW1C_MASK) | PORTSC_PR;
        if (!wait_ms(500, cond_port_ped)) {
            sys_puts(console_ep, "[USB]   ОШИБКА: порт не перешёл в Enabled (PED) за 500мс после сброса.\n");
            return false;
        }
    }
    // Снимаем CSC/PRC (RW1C), если они успели выставиться — иначе первое
    // же событие смены статуса порта будет "залипшим" со старым значением.
    //
    // Тридцатая попытка (см. ROADMAP.md) — БАГ: PED (бит 1) НЕ входит в
    // PORTSC_RW1C_MASK (это не RW1C-бит), но у него СВОЯ опасная
    // семантика — "запись 1 отключает порт" (не обычный сохраняемый
    // статус-бит). `cur` в этой точке уже содержит PED=1 (порт только
    // что включился, см. cond_port_ped() выше) — старая запись
    // `cur & ~PORTSC_RW1C_MASK` копировала этот РЕАЛЬНЫЙ PED=1 обратно
    // в регистр, а хардвер интерпретирует ЛЮБУЮ запись 1 в PED как
    // "отключить порт" — мы сами гасили порт, который только что
    // включили, ещё ДО Шага 6/7. Найдено по живому PORTSC (0x40000ee1,
    // PED=0) сразу после таймаута Address Device. Явно зануляем PED в
    // сохраняемой части — единственный безопасный вариант (запись 0 в
    // PED — гарантированный no-op по спеке).
    uint32_t cur = *reg32(g_op_base, off);
    *reg32(g_op_base, off) = (cur & ~PORTSC_RW1C_MASK & ~PORTSC_PED) | (cur & (PORTSC_CSC | PORTSC_PRC));
    if (LOG_USB) sys_puts(console_ep, "[USB]   Порт включён (PED=1).\n");
    return true;
}

// Печатает CCS всех портов подряд — чистая диагностика, поведение не меняет.
static void print_all_ports_ccs(seL4_CPtr console_ep, uint32_t max_ports) {
    for (uint32_t p = 1; p <= max_ports; p++) {
        uint32_t portsc = *reg32(g_op_base, XHCI_OP_PORTSC_BASE + (uintptr_t)(p - 1) * 0x10);
        if (LOG_USB) sys_puthex32(console_ep, "[USB]   DIAG PORTSC порта ", p);
        if (LOG_USB) sys_puthex32(console_ep, "[USB]     = ", portsc);
        if (LOG_USB) sys_puts(console_ep, (portsc & PORTSC_CCS) ? "[USB]     CCS = 1 (подключено)\n" : "[USB]     CCS = 0\n");
    }
}

// Шаг 6: Enable Slot Command.
static bool step6_enable_slot(seL4_CPtr console_ep, uint8_t &slot_id) {
    if (LOG_USB) sys_puts(console_ep, "[USB] Шаг 6: команда Enable Slot.\n");
    uint64_t cmd_paddr = enqueue_command_trb(0, 0, trb_type(TRB_TYPE_ENABLE_SLOT_CMD));
    uint8_t completion_code = 0;
    if (!wait_command_completion(console_ep, cmd_paddr, 500, completion_code, slot_id)) {
        sys_puts(console_ep, "[USB] ОШИБКА: Enable Slot не завершился за 500мс (нет Command Completion Event).\n");
        return false;
    }
    if (completion_code != 1) { // 1 = Success (см. xHCI Таблица 6.90)
        sys_puthex32(console_ep, "[USB] ОШИБКА: Enable Slot завершился с кодом ", completion_code);
        return false;
    }
    if (LOG_USB) sys_puthex32(console_ep, "[USB]   Slot ID получен: ", slot_id);
    return true;
}

// Milestone 2 (находка на живом железе, см. ROADMAP.md) — освобождает
// Slot ID. Обязателен перед тем, как пробовать другой порт в рамках одной
// попытки bring-up: MaxSlotsEn=1 (см. step3_setup_rings — этой фазе нужен
// РОВНО один одновременно активный слот) — контроллер выдаёт ОДИН слот,
// повторный Enable Slot без освобождения предыдущего завершается кодом 9
// (No Slots Available). Device/Input Context — тоже ОДНА физическая
// страница на оба места (см. platform.h): активным может быть только один
// слот единовременно, иначе следующий Address Device перезаписал бы
// память, которой контроллер всё ещё формально "владеет" для предыдущего
// (не освобождённого) слота.
static bool step_disable_slot(seL4_CPtr console_ep, uint8_t slot_id) {
    if (LOG_USB) sys_puthex32(console_ep, "[USB]   Освобождаю Slot ID: ", slot_id);
    uint64_t cmd_paddr = enqueue_command_trb(0, 0, trb_type(TRB_TYPE_DISABLE_SLOT_CMD) | ((uint32_t)slot_id << 24));
    uint8_t completion_code = 0, ret_slot = 0;
    if (!wait_command_completion(console_ep, cmd_paddr, 500, completion_code, ret_slot)) {
        sys_puts(console_ep, "[USB]   ПРЕДУПРЕЖДЕНИЕ: Disable Slot не завершился за 500мс.\n");
        return false;
    }
    if (completion_code != 1) {
        sys_puthex32(console_ep, "[USB]   ПРЕДУПРЕЖДЕНИЕ: Disable Slot завершился с кодом ", completion_code);
        return false;
    }
    return true;
}

static void write_ctx_dword(volatile uint32_t *ctx_base, int ctx_index, int dword_index, uint32_t value) {
    ((volatile uint32_t*)((uintptr_t)ctx_base + (uintptr_t)ctx_index * g_ctx_size))[dword_index] = value;
}
static uint32_t read_ctx_dword(volatile uint32_t *ctx_base, int ctx_index, int dword_index) {
    return ((volatile uint32_t*)((uintptr_t)ctx_base + (uintptr_t)ctx_index * g_ctx_size))[dword_index];
}

// Milestone B3 (Фаза 15, диагностика) — печатает 4 dword'а Slot Context
// начиная с ctx_base. ctx_index различается по ИСТОЧНИКУ (НАЙДЕНО НА
// ЖИВОМ ЖЕЛЕЗЕ — не сразу учёл эту разницу, первый дамп Device Context
// читал не то поле): у Input Context ПЕРВАЯ запись — Input Control
// Context, поэтому Slot Context на ctx_index=1 (inputctx(), то, что мы
// СОБИРАЕМСЯ отправить); у Device Context ТАКОГО префикса нет вообще —
// Slot Context сразу на ctx_index=0 (devctx_vaddr_for(slot_id), то, что
// контроллер РЕАЛЬНО записал после предыдущей команды).
static void dump_slot_context(seL4_CPtr console_ep, const char *label, volatile uint32_t *ctx_base, int ctx_index) {
    sys_puts(console_ep, label);
    sys_puts(console_ep, "\n");
    sys_puthex32(console_ep, "[USB]     dword0 (RouteString/Speed/MTT/Hub/CtxEntries) = ", read_ctx_dword(ctx_base, ctx_index, 0));
    sys_puthex32(console_ep, "[USB]     dword1 (MaxExitLat/RootHubPort/NumPorts)      = ", read_ctx_dword(ctx_base, ctx_index, 1));
    sys_puthex32(console_ep, "[USB]     dword2 (ParentHubSlot/ParentPort/TTT)         = ", read_ctx_dword(ctx_base, ctx_index, 2));
    sys_puthex32(console_ep, "[USB]     dword3 (DeviceAddress/SlotState)              = ", read_ctx_dword(ctx_base, ctx_index, 3));
}

// Двадцать седьмая попытка (см. ROADMAP.md) — Max Packet Size EP0 был
// захардкожен в 64 независимо от реальной скорости порта. По USB/xHCI
// спеке control-эндпоинт 0 ДО чтения дескриптора устройства должен
// использовать: Low Speed — строго 8 (единственное валидное значение),
// Full Speed — 8 (безопасный начальный дефолт, реальное значение 8/16/32/64
// уточняется позже через Evaluate Context), High Speed — строго 64,
// SuperSpeed (Gen1 x1 и быстрее) — строго 512. Несовпадение может сорвать
// неявный SET_ADDRESS, который контроллер сам выполняет на шине при
// обработке Address Device — именно то место, где сейчас таймаут.
static uint32_t ep0_max_packet_size_for_speed(uint32_t speed) {
    switch (speed) {
        case 1: return 8;   // Full Speed
        case 2: return 8;   // Low Speed
        case 3: return 64;  // High Speed
        default: return 512; // SuperSpeed Gen1 x1 и быстрее (значения 4+)
    }
}

// Шаг 7: Address Device — минимальный Input Context (Slot + EP0), команда
// Address Device, control-эндпоинт 0 становится рабочим.
static bool step7_address_device(seL4_CPtr console_ep, int idx, uint8_t slot_id, int port, uint32_t port_speed) {
    if (LOG_USB) sys_puts(console_ep, "[USB] Шаг 7: Address Device.\n");
    if (LOG_USB) sys_puthex32(console_ep, "[USB]   port_speed (PSIV) = ", port_speed);

    // Input Context: [0]=Input Control Context, [1]=Slot Context, [2]=EP0 Context
    for (int i = 0; i < 3; i++) for (int d = 0; d < g_ctx_size / 4; d++) write_ctx_dword(inputctx(), i, d, 0);
    // Input Control Context: A0 (Slot) + A1 (EP0) — добавляем оба.
    write_ctx_dword(inputctx(), 0, 1, (1u << 0) | (1u << 1));
    // Slot Context (dword0: Route String в [19:0] — НАЙДЕНО НА ЖИВОМ
    // ЖЕЛЕЗЕ (Milestone B3): раньше считал, что Route String нужен
    // ТОЛЬКО для SuperSpeed hub-топологии, а для устройства за обычным
    // (не-SS) хабом остаётся 0 — Address Device падал с кодом 0x11
    // (Parameter Error). На самом деле xHCI spec 4.3.3 требует Route
    // String для ЛЮБОГО хаб-подключённого устройства, независимо от
    // скорости промежуточного хаба — по нибблу (4 бита) на каждый ярус
    // топологии, начиная с младшего; для устройства на ПЕРВОМ ярусе под
    // корнем-хабом (наш случай, глубже не поддерживаем) — просто номер
    // downstream-порта хаба. 0 для корневых устройств (уже 0 по
    // умолчанию — поведение A1-B2 не меняется). Context Entries=1 в
    // [31:27]; Speed в [23:20]; MTT в [25] — Milestone B3, только для
    // устройств ЗА Multi-TT хабом; dword1: Root Hub Port Number в
    // [23:16] — для устройства ЗА хабом это КОРНЕВОЙ порт ХАБА, не его
    // downstream-порт, вызывающий обязан передать именно его в `port`).
    UsbDeviceSlot &self = g_usb_devices[idx];
    // issuse.txt №15 — route_string_full уже полный (по нибблу на КАЖДЫЙ
    // ярус вложенности, см. UsbDeviceSlot/enumerate_device_behind_hub) —
    // раньше здесь пересчитывался ЗАНОВО из одного parent_port_number,
    // что верно только для 1 яруса.
    uint32_t route_string = self.behind_hub ? self.route_string_full : 0;
    write_ctx_dword(inputctx(), 1, 0, route_string | (1u << 27) | ((port_speed & 0xF) << 20) | (self.behind_hub && self.parent_multi_tt ? (1u << 25) : 0));
    write_ctx_dword(inputctx(), 1, 1, ((uint32_t)port << 16));
    // Milestone B3 (Фаза 15) — dword2: Parent Hub Slot ID [7:0], Parent
    // Port Number [15:8], TT Think Time [17:16] — НАЙДЕНО НА ЖИВОМ
    // ЖЕЛЕЗЕ (Milestone B3, прямое сравнение Input Context РАБОЧЕГО
    // (корневой порт) и ПАДАЮЩЕГО (за хабом) случаев байт-в-байт: только
    // Route String и dword2 отличались, EP0/Speed/Add-flags идентичны).
    // Эти поля — TT-ассоциация (нужна ТОЛЬКО когда САМО устройство
    // FS/LS, требует Transaction Translator хаба для split-транзакций),
    // а не общая топология — для High-Speed устройства за хабом (наш
    // единственный поддерживаемый случай, TT не реализован — см. план)
    // dword2 должен остаться 0, как и у корневых устройств. Route String
    // (dword0) — другое дело, нужен всегда для маршрутизации через
    // хаб-иерархию независимо от скорости, не трогаем.
    // EP0 Context (dword1: CErr=3 в [2:1] — Двадцать восьмая попытка, см.
    //              ROADMAP.md: было 0, стандартная xHCI-конвенция для
    //              control-эндпоинтов — 3 (2 повтора при ошибке), иначе
    //              поведение контроллера при первой же ошибке/ретрае на
    //              шине не определено нашим кодом; EP Type=4 (Control) в
    //              [5:3]; Max Packet Size — по реальной скорости порта, см.
    //              ep0_max_packet_size_for_speed;
    //              dword2: TR Dequeue Pointer | DCS=1)
    uint32_t ep0_mps = ep0_max_packet_size_for_speed(port_speed);
    if (LOG_USB) sys_puthex32(console_ep, "[USB]   EP0 MaxPacketSize = ", ep0_mps);
    write_ctx_dword(inputctx(), 2, 1, (3u << 1) | (4u << 3) | (ep0_mps << 16));
    // Milestone 1 (закрытие Фазы 14, см. ROADMAP.md/план) — раньше TR
    // Dequeue Pointer EP0 указывал на страницу Device Context как временную
    // заглушку ("EP0 Transfer Ring" на самом деле не было вообще) — опасно
    // для настоящих Setup/Data/Status TRB (Milestone 2+), эта память
    // принадлежит железу. Теперь — честное отдельное кольцо (см.
    // g_usb_devices[idx].ep0_ring/ep0ring_vaddr(idx)), инициализируется
    // заново на каждую Address Device (в т.ч. hot-plug reconnect —
    // Milestone 11).
    init_trb_ring(g_usb_devices[idx].ep0_ring, ep0ring_vaddr(idx), g_usb_devices[idx].ep0_trring_paddr);
    uint64_t ep0_paddr = g_usb_devices[idx].ep0_ring.dev_base; // уже в device-видимом виде, см. init_trb_ring
    write_ctx_dword(inputctx(), 2, 2, (uint32_t)(ep0_paddr & 0xFFFFFFFFu) | 1u);
    write_ctx_dword(inputctx(), 2, 3, (uint32_t)(ep0_paddr >> 32));

    // Фаза 15 — Device Context по одной странице на Slot ID (не на idx —
    // см. devctx_paddr_for()/platform.h).
    dcbaa()[slot_id] = to_dev_addr(devctx_paddr_for(slot_id));

    // Milestone B3 (Фаза 15, диагностика) — печатает РЕАЛЬНЫЕ байты
    // перед КАЖДЫМ Address Device — использовалось для прямого сравнения
    // рабочего (корневой порт) и падающего (за хабом) случаев байт-в-байт.
    // Расследование закрыто (см. ROADMAP.md/память) — под LOG_USB, как и
    // остальная пошаговая диагностика.
    if (LOG_USB) {
        dump_slot_context(console_ep, self.behind_hub
                           ? "[USB]   DIAG Input Context (Slot) перед Address Device РЕБЁНКА:"
                           : "[USB]   DIAG Input Context (Slot) перед Address Device (корневой порт):",
                           inputctx(), 1);
        sys_puthex32(console_ep, "[USB]     Input Control Context (Add flags) = ", read_ctx_dword(inputctx(), 0, 1));
        sys_puthex32(console_ep, "[USB]     EP0 Context dword1 (CErr/Type/MPS) = ", read_ctx_dword(inputctx(), 2, 1));
    }

    uint64_t cmd_paddr = enqueue_command_trb(to_dev_addr(g_inputctx_paddr), 0, trb_type(TRB_TYPE_ADDRESS_DEVICE_CMD) | ((uint32_t)slot_id << 24));
    uint8_t completion_code = 0, ret_slot = 0;
    if (!wait_command_completion(console_ep, cmd_paddr, 500, completion_code, ret_slot)) {
        sys_puts(console_ep, "[USB] ОШИБКА: Address Device не завершился за 500мс.\n");
        // Двадцать девятая попытка (см. ROADMAP.md) — живой PORTSC порта
        // ПРЯМО СЕЙЧАС: если устройство отвалилось/перезапустило линию
        // во время неявного SET_ADDRESS, CCS погаснет или PRC/CSC
        // выставятся заново — прямое подтверждение или опровержение
        // гипотезы "устройство отключилось посреди команды".
        uint32_t portsc_now = *reg32(g_op_base, XHCI_OP_PORTSC_BASE + (uintptr_t)(port - 1) * 0x10);
        if (LOG_USB) sys_puthex32(console_ep, "[USB]   DIAG живой PORTSC порта прямо сейчас = ", portsc_now);
        if (LOG_USB) sys_puts(console_ep, (portsc_now & PORTSC_CCS) ? "[USB]     CCS = 1 (устройство всё ещё подключено)\n" : "[USB]     CCS = 0 (устройство ОТКЛЮЧИЛОСЬ!)\n");
        return false;
    }
    if (completion_code != 1) {
        sys_puthex32(console_ep, "[USB] ОШИБКА: Address Device завершился с кодом ", completion_code);
        return false;
    }
    if (LOG_USB) sys_puts(console_ep, "[USB]   Устройство адресовано, control-эндпоинт готов.\n");
    return true;
}

// Milestone 2 (закрытие Фазы 14, см. ROADMAP.md/план) — IN control-transfer
// на EP0: Setup+Data+Status TRB, ОДИН звонок в doorbell (xHC сам проходит
// все три стадии подряд), два wait_transfer_completion() — на Data TRB
// (узнать реальную длину и подтвердить успех/short packet) и на Status TRB
// (подтвердить рукопожатие). buffer_paddr — физический адрес приёмного
// буфера (CPU-адрес, to_dev_addr применяется здесь же, вызывающему думать
// об этом не надо).
static bool ep0_control_in(seL4_CPtr console_ep, TrbRing &ep0_ring, uint8_t slot_id, uint8_t bmRequestType, uint8_t bRequest,
                            uint16_t wValue, uint16_t wIndex, uint16_t wLength,
                            uint64_t buffer_paddr, uint32_t &actual_length) {
    // USB Setup-пакет (8 байт, LE) упакован напрямую в Parameter TRB — IDT
    // (Immediate Data, см. control ниже) говорит контроллеру, что Parameter
    // это САМИ данные, а не указатель на них.
    uint64_t setup = (uint64_t)bmRequestType | ((uint64_t)bRequest << 8) | ((uint64_t)wValue << 16)
                    | ((uint64_t)wIndex << 32) | ((uint64_t)wLength << 48);
    ring_enqueue_trb(ep0_ring, setup, 8u /* TRB Transfer Length для Setup Stage — всегда 8, см. xHCI 6.4.1.2.1 */,
                      trb_type(TRB_TYPE_SETUP_STAGE) | (1u << 6) /*IDT*/ | (3u << 16) /*TRT=IN Data Stage*/);
    uint64_t data_trb_addr = ring_enqueue_trb(ep0_ring, to_dev_addr(buffer_paddr), (uint32_t)wLength,
                      trb_type(TRB_TYPE_DATA_STAGE) | (1u << 16) /*DIR=IN*/ | (1u << 2) /*ISP*/ | TRB_IOC);
    uint64_t status_trb_addr = ring_enqueue_trb(ep0_ring, 0, 0,
                      trb_type(TRB_TYPE_STATUS_STAGE) /*DIR=OUT(0) — противоположно Data Stage*/ | TRB_IOC);
    ring_endpoint_doorbell(slot_id, 1 /* DCI=1 — EP0, единственный управляющий эндпоинт */);

    uint8_t cc = 0;
    uint32_t residual = 0;
    if (!wait_transfer_completion(console_ep, data_trb_addr, 500, cc, residual)) {
        sys_puts(console_ep, "[USB] ОШИБКА: Data Stage control-transfer'а не завершился за 500мс.\n");
        return false;
    }
    // Completion code 1 (Success) ИЛИ 13 (Short Packet) — оба означают
    // "данные реально пришли"; Short Packet — устройство прислало МЕНЬШЕ,
    // чем мы предложили буфером (законно, не ошибка, см. xHCI спецификацию).
    if (cc != 1 && cc != 13) {
        sys_puthex32(console_ep, "[USB] ОШИБКА: Data Stage control-transfer'а завершился с кодом ", cc);
        return false;
    }
    actual_length = (uint32_t)wLength - residual;

    uint32_t status_residual = 0;
    if (!wait_transfer_completion(console_ep, status_trb_addr, 500, cc, status_residual)) {
        sys_puts(console_ep, "[USB] ОШИБКА: Status Stage control-transfer'а не завершился за 500мс.\n");
        return false;
    }
    if (cc != 1) {
        sys_puthex32(console_ep, "[USB] ОШИБКА: Status Stage control-transfer'а завершился с кодом ", cc);
        return false;
    }
    return true;
}

// Milestone 4 — control-transfer БЕЗ Data Stage (TRT=0), нужен для
// SET_CONFIGURATION/SET_INTERFACE и т.п. (запрос без полезной нагрузки).
// Per xHCI/USB спек: если у запроса нет Data Stage, Status Stage ВСЕГДА
// идёт с DIR=IN (в отличие от ep0_control_in(), где Status — OUT,
// противоположно IN Data Stage).
static bool ep0_control_no_data(seL4_CPtr console_ep, TrbRing &ep0_ring, uint8_t slot_id, uint8_t bmRequestType, uint8_t bRequest,
                                 uint16_t wValue, uint16_t wIndex) {
    uint64_t setup = (uint64_t)bmRequestType | ((uint64_t)bRequest << 8) | ((uint64_t)wValue << 16)
                    | ((uint64_t)wIndex << 32); // wLength=0
    ring_enqueue_trb(ep0_ring, setup, 8u,
                      trb_type(TRB_TYPE_SETUP_STAGE) | (1u << 6) /*IDT*/ | (0u << 16) /*TRT=No Data Stage*/);
    uint64_t status_trb_addr = ring_enqueue_trb(ep0_ring, 0, 0,
                      trb_type(TRB_TYPE_STATUS_STAGE) | (1u << 16) /*DIR=IN — обязателен без Data Stage*/ | TRB_IOC);
    ring_endpoint_doorbell(slot_id, 1 /* DCI=1 — EP0 */);

    uint8_t cc = 0;
    uint32_t residual = 0;
    if (!wait_transfer_completion(console_ep, status_trb_addr, 500, cc, residual)) {
        sys_puts(console_ep, "[USB] ОШИБКА: Status Stage control-transfer'а (без данных) не завершился за 500мс.\n");
        return false;
    }
    if (cc != 1) {
        sys_puthex32(console_ep, "[USB] ОШИБКА: Status Stage control-transfer'а (без данных) завершился с кодом ", cc);
        return false;
    }
    return true;
}

// Шаг 8 — настоящий GET_DESCRIPTOR(Device) вместо заглушки с нулями.
// Device Descriptor (18 байт, см. USB 2.0 спецификацию 9.6.1):
//   offset 4=bDeviceClass, 5=bDeviceSubClass, 6=bDeviceProtocol,
//   8-9=idVendor(LE), 10-11=idProduct(LE).
static void step8_get_device_descriptor(seL4_CPtr console_ep, int idx, uint8_t slot_id) {
    if (LOG_USB) sys_puts(console_ep, "[USB] Шаг 8: GET_DESCRIPTOR(Device).\n");
    volatile uint8_t *buf = ctrlbuf_vaddr(idx);
    for (int i = 0; i < 18; i++) buf[i] = 0;

    uint32_t actual = 0;
    UsbFoundDevice &found = g_usb_devices[idx].found;
    // bmRequestType=0x80 (Device-to-Host|Standard|Device), bRequest=0x06
    // (GET_DESCRIPTOR), wValue=(тип<<8)|индекс = (0x01<<8)|0 (Device, #0).
    if (!ep0_control_in(console_ep, g_usb_devices[idx].ep0_ring, slot_id, 0x80, 0x06, (0x01u << 8), 0, 18, g_usb_devices[idx].ctrl_buf_paddr, actual)) {
        sys_puts(console_ep, "[USB] ОШИБКА: GET_DESCRIPTOR(Device) не удался (см. лог выше).\n");
        return;
    }
    if (LOG_USB) sys_puthex32(console_ep, "[USB]   получено байт: ", actual);
    if (actual < 18) {
        sys_puts(console_ep, "[USB]   ПРЕДУПРЕЖДЕНИЕ: получено меньше 18 байт — дескриптор может быть неполным.\n");
    }

    found.found = true;
    found.device_class    = buf[4];
    found.device_subclass = buf[5];
    found.device_protocol = buf[6];
    found.vendor_id  = (uint16_t)buf[8]  | ((uint16_t)buf[9]  << 8);
    found.product_id = (uint16_t)buf[10] | ((uint16_t)buf[11] << 8);
    if (LOG_USB) sys_puthex16(console_ep, "[USB]   idVendor  = ", found.vendor_id);
    if (LOG_USB) sys_puthex16(console_ep, "[USB]   idProduct = ", found.product_id);
    if (LOG_USB) sys_puthex32(console_ep, "[USB]   bDeviceClass/SubClass/Protocol = ",
                 ((uint32_t)found.device_class << 16) | ((uint32_t)found.device_subclass << 8) | found.device_protocol);
}

// USB-класс, которого ищем в этой фазе: Mass Storage / SCSI Transparent /
// Bulk-Only Transport (см. USB Mass Storage Class спецификацию).
constexpr uint8_t USB_CLASS_MASS_STORAGE = 0x08;
constexpr uint8_t USB_SUBCLASS_SCSI      = 0x06;
constexpr uint8_t USB_PROTOCOL_BOT       = 0x50;
// Milestone B1 (Фаза 15) — перенесено выше (было объявлено ниже,
// step9_get_configuration_descriptor() теперь тоже на него ссылается).
constexpr uint8_t USB_CLASS_HUB = 0x09;

constexpr uint8_t USB_DESC_TYPE_CONFIGURATION   = 0x02;
constexpr uint8_t USB_DESC_TYPE_INTERFACE       = 0x04;
constexpr uint8_t USB_DESC_TYPE_ENDPOINT        = 0x05;
constexpr uint8_t USB_DESC_TYPE_SS_EP_COMPANION = 0x30; // только для SuperSpeed (port_speed>=4)
constexpr uint8_t USB_DESC_TYPE_HUB             = 0x29; // class-специфичный HS/FS хаб, см. step_hub_enumerate()
constexpr uint8_t USB_DESC_TYPE_SS_HUB          = 0x2A; // Milestone B5 (доп.) — SuperSpeed хаб (USB 3.x spec Table 10-13) требует ДРУГОЙ тип дескриптора — 0x29 STALL'ит на SS-хабе (найдено на живом железе, внешний 4-портовый донгл)

// Milestone 3 (закрытие Фазы 14, см. ROADMAP.md/план) — GET_DESCRIPTOR
// (Configuration): bDeviceClass у Шага 8 часто равен 0 (класс определён на
// уровне Interface, не Device — типичный паттерн для Mass Storage, ровно
// то, что нашли на живом железе) — окончательно класс устройства
// определяется только здесь. Запрашиваем буфер целиком одним запросом (с
// запасом — 512 байт; реальная цепочка Configuration+Interface+
// Endpoint(+SS Companion) для простого MSC-устройства обычно куда
// компактнее), затем обходим TLV-цепочку по bLength/bDescriptorType.
static bool step9_get_configuration_descriptor(seL4_CPtr console_ep, int idx, uint8_t slot_id, uint32_t port_speed, bool is_hub = false) {
    if (LOG_USB) sys_puts(console_ep, "[USB] Шаг 9: GET_DESCRIPTOR(Configuration).\n");
    volatile uint8_t *buf = ctrlbuf_vaddr(idx);
    // issuse.txt №16: ctrlbuf_vaddr() выделяет ЦЕЛУЮ страницу (4096 байт,
    // см. PLAT_XHCI_CTRL_BUF_VADDR/idx*4096) на устройство, но раньше
    // читалось (и парсилось) только первые 512 — у составного/
    // многоинтерфейсного устройства с полной цепочкой дескрипторов длиннее
    // 512 байт интерфейсы/эндпоинты дальше этой границы никогда не
    // разбирались. Память уже выделена и замаплена — используем всю
    // страницу, без второго запроса.
    constexpr uint32_t BUF_LEN = 4096;
    for (uint32_t i = 0; i < BUF_LEN; i++) buf[i] = 0;

    UsbBulkEndpoints &bulk_eps = g_usb_devices[idx].bulk_eps;
    uint32_t actual = 0;
    // bmRequestType=0x80, bRequest=0x06, wValue=(тип<<8)|индекс = (0x02<<8)|0
    // (Configuration, #0).
    if (!ep0_control_in(console_ep, g_usb_devices[idx].ep0_ring, slot_id, 0x80, 0x06, ((uint16_t)USB_DESC_TYPE_CONFIGURATION << 8), 0,
                         (uint16_t)BUF_LEN, g_usb_devices[idx].ctrl_buf_paddr, actual)) {
        sys_puts(console_ep, "[USB] ОШИБКА: GET_DESCRIPTOR(Configuration) не удался (см. лог выше).\n");
        return false;
    }
    if (LOG_USB) sys_puthex32(console_ep, "[USB]   получено байт: ", actual);
    if (actual < 9) {
        sys_puts(console_ep, "[USB] ОШИБКА: меньше 9 байт — даже заголовок Configuration Descriptor не поместился.\n");
        return false;
    }
    uint16_t total_length = (uint16_t)buf[2] | ((uint16_t)buf[3] << 8);
    if (LOG_USB) sys_puthex32(console_ep, "[USB]   wTotalLength = ", total_length);
    if (LOG_USB) sys_puthex32(console_ep, "[USB]   bNumInterfaces = ", buf[4]);
    bulk_eps.config_value = buf[5]; // bConfigurationValue — нужен для SET_CONFIGURATION (Milestone 4)

    uint32_t parse_limit = (total_length < actual) ? total_length : actual;
    if (parse_limit > BUF_LEN) parse_limit = BUF_LEN;

    bool in_target_interface = false;
    uint32_t off = 9; // сразу после 9-байтного Configuration Descriptor
    while (off + 2 <= parse_limit) {
        uint8_t desc_len = buf[off];
        uint8_t desc_type = buf[off + 1];
        if (desc_len == 0 || off + desc_len > parse_limit) break; // защита от битой/усечённой цепочки

        if (desc_type == USB_DESC_TYPE_INTERFACE && desc_len >= 9) {
            uint8_t if_num = buf[off + 2], if_alt = buf[off + 3], if_numeps = buf[off + 4];
            uint8_t if_class = buf[off + 5], if_subclass = buf[off + 6], if_protocol = buf[off + 7];
            // Под LOG_USB — раньше было безусловно (зацепка при отладке
            // UAS-only адаптера без Bulk-Only alt-setting), но после
            // закрытия того расследования решили не захламлять обычный
            // (тихий) лог — см. пользовательский запрос.
            if (LOG_USB) sys_puthex32(console_ep, "[USB]   Interface # / AltSetting / NumEndpoints = ",
                         ((uint32_t)if_num << 16) | ((uint32_t)if_alt << 8) | if_numeps);
            if (LOG_USB) sys_puthex32(console_ep, "[USB]   Interface класс/подкласс/протокол = ",
                         ((uint32_t)if_class << 16) | ((uint32_t)if_subclass << 8) | if_protocol);
            // Milestone B3 (доп.) — у хаба класс интерфейса заведомо не
            // Mass Storage (проверка ниже осмысленна только для
            // накопителей) — нужен его Interrupt-эндпоинт (см. ниже), не
            // bulk, поэтому для is_hub считаем интерфейс "целевым" всегда.
            in_target_interface = is_hub || (if_class == USB_CLASS_MASS_STORAGE && if_subclass == USB_SUBCLASS_SCSI
                                    && if_protocol == USB_PROTOCOL_BOT);
            if (in_target_interface && !is_hub) bulk_eps.interface_num = if_num; // нужен для BOT Reset (issuse.txt №14)
            if (in_target_interface && !is_hub) {
                if (LOG_USB) sys_puts(console_ep, "[USB]     Mass Storage / SCSI Transparent / Bulk-Only Transport — то, что нужно.\n");
            }
        } else if (desc_type == USB_DESC_TYPE_ENDPOINT && desc_len >= 7 && in_target_interface) {
            uint8_t ep_addr = buf[off + 2];
            uint8_t ep_attr = buf[off + 3];
            uint16_t ep_mps = (uint16_t)buf[off + 4] | ((uint16_t)buf[off + 5] << 8);
            if (is_hub && (ep_attr & 0x03u) == 0x03u) { // Interrupt — эндпоинт статуса портов хаба (используется в B4, EP Context настраивается уже здесь в B3, см. step_hub_configure_slot())
                uint8_t ep_interval = buf[off + 6];
                if (LOG_USB) sys_puthex32(console_ep, "[USB]   Хаб: Interrupt-эндпоинт, адрес = ", ep_addr);
                if (LOG_USB) sys_puthex32(console_ep, "[USB]     MaxPacketSize/bInterval = ", ((uint32_t)ep_mps << 8) | ep_interval);
                g_usb_devices[idx].hub_int_ep_addr = ep_addr;
                g_usb_devices[idx].hub_int_ep_mps = ep_mps;
                g_usb_devices[idx].hub_int_ep_interval = ep_interval;
            } else if ((ep_attr & 0x03u) == 0x02u) { // Bulk (00=Control,01=Isoch,10=Bulk,11=Interrupt)
                bool is_in = (ep_addr & 0x80u) != 0;
                if (LOG_USB) sys_puthex32(console_ep, is_in ? "[USB]   Bulk IN эндпоинт, адрес = " : "[USB]   Bulk OUT эндпоинт, адрес = ", ep_addr);
                if (LOG_USB) sys_puthex32(console_ep, "[USB]     MaxPacketSize = ", ep_mps);
                if (is_in) { bulk_eps.bulk_in_addr = ep_addr; bulk_eps.bulk_in_mps = ep_mps; }
                else       { bulk_eps.bulk_out_addr = ep_addr; bulk_eps.bulk_out_mps = ep_mps; }

                // SuperSpeed Endpoint Companion Descriptor — следующая
                // запись СРАЗУ за этим Endpoint Descriptor в ТОЙ ЖЕ
                // цепочке (не отдельный проход), только если порт SS.
                uint32_t next_off = off + desc_len;
                if (port_speed >= 4 && next_off + 2 <= parse_limit
                    && buf[next_off + 1] == USB_DESC_TYPE_SS_EP_COMPANION && buf[next_off] >= 6) {
                    uint8_t max_burst = buf[next_off + 2];
                    if (LOG_USB) sys_puthex32(console_ep, "[USB]     MaxBurst (SS Companion) = ", max_burst);
                    if (is_in) bulk_eps.bulk_in_max_burst = max_burst;
                    else       bulk_eps.bulk_out_max_burst = max_burst;
                }
            }
        }
        off += desc_len;
    }

    bulk_eps.found = (bulk_eps.bulk_in_addr != 0 && bulk_eps.bulk_out_addr != 0);
    if (!bulk_eps.found) {
        // Milestone B1 (Фаза 15) — у хаба ЗАКОНОМЕРНО нет Mass Storage
        // bulk-эндпоинтов (не ошибка) — вызывающий уже знает это по
        // device_class и пойдёт своей веткой (step_hub_enumerate()), не
        // печатаем вводящую в заблуждение "ОШИБКУ". bConfigurationValue
        // (buf[5], записан выше) остаётся валидным в любом случае.
        if (!is_hub) sys_puts(console_ep, "[USB] ОШИБКА: не нашёл оба bulk-эндпоинта (IN и OUT) у Mass Storage интерфейса.\n");
        return false;
    }
    return true;
}

// Milestone B1 (Фаза 15) — раньше ЛЮБОЙ хаб (bDeviceClass=0x09, включая
// встроенный VL805 на порту 1 этой платы) отбрасывался сразу после Шага 8
// как "не то, что ищем". Теперь: SET_CONFIGURATION (bConfigurationValue
// уже захвачен в Шаге 9 — buf[5] читается ДО разбора интерфейсов,
// одинаково для любого класса устройства) + class-специфичный
// GET_DESCRIPTOR(Hub) — bmRequestType=0xA0 (Device-to-Host|Class|Device,
// см. USB 2.0 spec 11.24.2), bRequest=0x06, wValue=(тип<<8)|0 (без
// индекса — у устройства ровно один Hub Descriptor). Milestone B5 (доп.,
// найдено на живом железе — внешний 4-портовый донгл в USB3.0-порт) —
// тип зависит от bDeviceProtocol (Шаг 8): 3 = SuperSpeed-хаб (spec USB
// 3.x), иначе — HS/FS-хаб (spec USB 2.0). Байты 0-6 (bNbrPorts@2,
// wHubCharacteristics@3-4, bPwrOn2PwrGood@5) ИДЕНТИЧНЫ в обоих форматах
// (SS-версия добавляет bHubHdrDecLat/wHubDelay ПОСЛЕ них) — разбор ниже
// общий, менять не пришлось. Разбираем только bNbrPorts и
// bPwrOn2PwrGood — DeviceRemovable/PortPwrCtrlMask (переменной длины,
// зависят от числа портов) не нужны для этого шага, понадобились в B2
// (перечисление downstream-портов).
static void step_hub_enumerate(seL4_CPtr console_ep, int idx, uint8_t slot_id) {
    UsbDeviceSlot &dev = g_usb_devices[idx];
    if (!ep0_control_no_data(console_ep, dev.ep0_ring, slot_id, 0x00, 0x09, dev.bulk_eps.config_value, 0)) {
        sys_puts(console_ep, "[USB] ОШИБКА (хаб): SET_CONFIGURATION не удался.\n");
        return;
    }
    volatile uint8_t *buf = ctrlbuf_vaddr(idx);
    for (int i = 0; i < 16; i++) buf[i] = 0;
    uint32_t actual = 0;
    bool is_ss_hub = (dev.found.device_protocol == 3);
    uint8_t desc_type = is_ss_hub ? USB_DESC_TYPE_SS_HUB : USB_DESC_TYPE_HUB;
    if (!ep0_control_in(console_ep, dev.ep0_ring, slot_id, 0xA0, 0x06, ((uint16_t)desc_type << 8), 0,
                         16, dev.ctrl_buf_paddr, actual)) {
        sys_puts(console_ep, "[USB] ОШИБКА (хаб): GET_DESCRIPTOR(Hub) не удался.\n");
        return;
    }
    if (actual < 7) {
        sys_puts(console_ep, "[USB] ОШИБКА (хаб): Hub Descriptor короче ожидаемых 7 байт.\n");
        return;
    }
    dev.hub_num_ports = buf[2];
    dev.hub_pwr_on_to_pwr_good = buf[5];
    // wHubCharacteristics (offset 3-4, LE) — биты 5-6 = TT Think Time
    // (00=8 FS bit times .. 11=32), нужно детям этого хаба (Milestone B3).
    uint16_t hub_chars = (uint16_t)buf[3] | ((uint16_t)buf[4] << 8);
    dev.hub_tt_think_time = (uint8_t)((hub_chars >> 5) & 0x3u);
    if (LOG_USB) sys_puthex32(console_ep, "[USB]   Хаб: портов = ", dev.hub_num_ports);
    if (LOG_USB) sys_puthex32(console_ep, "[USB]   Хаб: PwrOn2PwrGood (x2мс) = ", dev.hub_pwr_on_to_pwr_good);
}

// Milestone B3 (Фаза 15) — НАЙДЕНО НА ЖИВОМ ЖЕЛЕЗЕ, несколько попыток
// подряд: (1) Evaluate Context (TRB Type 13) для хаба формально
// завершался успешно, но xHCI spec (4.6.7) явно ЗАПРЕЩАЕТ менять
// Hub/Number of Ports/Route String/Speed/TTT через эту команду — молча
// проигнорировано; (2) переход на Configure Endpoint (TRB Type 12) с
// ТОЛЬКО A0 (Slot), без единого добавленного DCI — Hub-бит/Number of
// Ports ПОДТВЕРЖДЁННО применились (живой Device Context readback), но
// Slot State остался 2 (Addressed), не 3 (Configured) — Address Device
// ребёнка всё равно падал с кодом 0x11. Настоящая причина — Configure
// Endpoint БЕЗ единого добавленного эндпоинта не переводит Slot State в
// Configured (несмотря на успешный код завершения команды). Реальный
// Interrupt-эндпоинт хаба (статус портов) теперь конфигурируется ЗДЕСЬ,
// той же командой — Transfer Ring переиспользует bulkin_ring/
// bulkin_trring_paddr (у хаба нет bulk-эндпоинтов, страница иначе
// простаивала бы); фактический опрос данных с него — Milestone B4.
constexpr uint8_t USB_HUB_REQ_SET_HUB_DEPTH = 0x0C; // USB3 spec 10.14.2.6 — ТОЛЬКО для SS-хабов, recipient=Device (0x20), не Other

static bool step_hub_configure_slot(seL4_CPtr console_ep, int idx, uint8_t slot_id, int root_port, uint32_t port_speed) {
    UsbDeviceSlot &dev = g_usb_devices[idx];

    // issuse.txt №18: hub_int_ep_addr==0 (дескриптор хаба не дал interrupt
    // IN endpoint — нестандартный/необычный хаб) раньше молча проваливался
    // в ТОТ ЖЕ путь (Configure Endpoint только с A0, без единого DCI),
    // который сам комментарий выше документирует как ПОДТВЕРЖДЁННО битый
    // (Slot State остаётся Addressed несмотря на успешный код завершения
    // команды) — отказываем сразу, с понятной причиной, вместо повторения
    // уже известного бага молча.
    if (dev.hub_int_ep_addr == 0) {
        sys_puts(console_ep, "[USB]   ОШИБКА (хаб): не найден interrupt IN endpoint статуса портов в дескрипторе — Configure Endpoint только со Slot'ом не переводит Slot State в Configured (см. issuse.txt №18), хаб не поддержан.\n");
        return false;
    }

    // issuse.txt №15 — НАЙДЕНО НА ЖИВОМ ЖЕЛЕЗЕ (первый раз, когда хаб-за-
    // хабом реально дошёл до этого шага): раньше port_speed бралась ЖИВЫМ
    // чтением PORTSC(dev.port-1) — корректно ТОЛЬКО для хаба на корневом
    // порту. Для хаба ЗА хабом dev.port умышленно 0 (см. behind_hub в
    // UsbDeviceSlot) — (dev.port-1) уходил в wraparound и читал МУСОР из
    // MMIO по случайному смещению вместо PORTSC. Теперь port_speed —
    // параметр, тот же device_speed, что уже корректно определила
    // предшествующая асинхронная (или синхронная, root-port) стадия
    // сброса/скорости и передала через step7_address_device() — та же
    // логика, что уже применяется в step10_configure_endpoints() для
    // Mass Storage за хабом (см. её же route_string ниже — тот же
    // источник живой находки Milestone B3).
    uint32_t route_string = dev.behind_hub ? dev.route_string_full : 0;

    bool have_int_ep = (dev.hub_int_ep_addr != 0);
    uint8_t int_epnum = dev.hub_int_ep_addr & 0x0Fu;
    uint8_t int_dci = have_int_ep ? (uint8_t)(2u * int_epnum + 1u) : 0; // хаб-эндпоинт статуса всегда IN (адрес & 0x80)
    dev.hub_int_dci = int_dci;
    uint32_t ctx_entries = have_int_ep ? int_dci : 1;

    for (int i = 0; i < 3; i++) for (int d = 0; d < g_ctx_size / 4; d++) write_ctx_dword(inputctx(), i, d, 0);
    write_ctx_dword(inputctx(), 0, 1, (1u << 0) | (have_int_ep ? (1u << int_dci) : 0)); // A0 (Slot) [+ A(int_dci)]
    write_ctx_dword(inputctx(), 1, 0, route_string | (ctx_entries << 27) | ((port_speed & 0xF) << 20) | (1u << 26) /*Hub*/
                     | (dev.behind_hub && dev.parent_multi_tt ? (1u << 25) : 0));
    write_ctx_dword(inputctx(), 1, 1, ((uint32_t)root_port << 16) | ((uint32_t)dev.hub_num_ports << 24));

    if (have_int_ep) {
        init_trb_ring(dev.bulkin_ring, bulkinring_vaddr(idx), dev.bulkin_trring_paddr);
        uint32_t interval_field = (dev.hub_int_ep_interval >= 1) ? ((uint32_t)dev.hub_int_ep_interval - 1) : 0;
        if (interval_field > 15) interval_field = 15;
        int ctx_index = (int)int_dci + 1;
        for (int d = 0; d < g_ctx_size / 4; d++) write_ctx_dword(inputctx(), ctx_index, d, 0);
        write_ctx_dword(inputctx(), ctx_index, 0, interval_field << 16); // Interval, см. xHCI EP Context dword0
        write_ctx_dword(inputctx(), ctx_index, 1, (3u << 1) /*CErr*/ | (7u << 3) /*EP Type=Interrupt In*/ | ((uint32_t)dev.hub_int_ep_mps << 16));
        write_ctx_dword(inputctx(), ctx_index, 2, (uint32_t)(dev.bulkin_ring.dev_base & 0xFFFFFFFFu) | 1u /*DCS*/);
        write_ctx_dword(inputctx(), ctx_index, 3, (uint32_t)(dev.bulkin_ring.dev_base >> 32));
        write_ctx_dword(inputctx(), ctx_index, 4, dev.hub_int_ep_mps); // Average TRB Length
    }

    uint64_t cmd_paddr = enqueue_command_trb(to_dev_addr(g_inputctx_paddr), 0,
                                              trb_type(TRB_TYPE_CONFIGURE_ENDPOINT_CMD) | ((uint32_t)slot_id << 24));
    uint8_t completion_code = 0, ret_slot = 0;
    if (!wait_command_completion(console_ep, cmd_paddr, 500, completion_code, ret_slot)) {
        sys_puts(console_ep, "[USB]   ОШИБКА (хаб): Configure Endpoint (слот) не завершился за 500мс.\n");
        return false;
    }
    if (completion_code != 1) {
        sys_puthex32(console_ep, "[USB]   ОШИБКА (хаб): Configure Endpoint (слот) завершился с кодом ", completion_code);
        return false;
    }
    // Milestone B3 (Фаза 15, диагностика) — команда завершилась успешно,
    // но три попытки подряд показали, что "успешный код завершения" НЕ
    // значит "поля реально применились" — читаем ЖИВОЙ Device Context
    // хаба (не Input Context, который мы только что ОТПРАВИЛИ) и
    // печатаем то, что контроллер РЕАЛЬНО туда записал.
    if (LOG_USB) dump_slot_context(console_ep, "[USB]   DIAG живой Device Context хаба ПОСЛЕ Configure Endpoint:", devctx_vaddr_for(slot_id), 0);
    // issuse.txt №15 — НАЙДЕНО ЧТЕНИЕМ СПЕКИ (USB 3.x spec 10.14.2.6):
    // SS-хаб ОБЯЗАН получить SET_HUB_DEPTH (bRequest=12, recipient=Device,
    // не Other — не путать с SET/CLEAR_FEATURE на порт) ДО того, как его
    // downstream-порты можно адресовать — без этого хаб не знает свою
    // позицию в топологии и не может корректно маршрутизировать/линковать
    // трафик к устройствам за собой, что на практике даёт Transaction
    // Error на Address Device ребёнка (см. issuse.txt, Milestone B5).
    // Раньше этот запрос не отправлялся вообще, ни для одного SS-хаба.
    // Значение — hub_tier ЭТОГО хаба (0, если хаб сам на корневом порту —
    // ровно то же число, что USB3 spec называет Hub Depth: "0 хабов
    // ВЫШЕ меня", см. hub_tier в UsbDeviceSlot).
    if (dev.found.device_protocol == 3) {
        if (!ep0_control_no_data(console_ep, dev.ep0_ring, slot_id, 0x20, USB_HUB_REQ_SET_HUB_DEPTH, dev.hub_tier, 0)) {
            sys_puts(console_ep, "[USB]   ПРЕДУПРЕЖДЕНИЕ (хаб): SET_HUB_DEPTH не удался — устройства за этим SS-хабом могут не адресоваться.\n");
        }
    }
    return true;
}

// Milestone B4 (Фаза 15) — ставит ОДНУ "слушающую" Normal TRB на
// Interrupt-эндпоинт хаба (статус downstream-портов) и звонит в
// доорбелл — сама передача завершится АСИНХРОННО, когда хаб реально
// заметит изменение (Transfer Event появится в Event Ring когда-то
// потом). Landing-буфер — bounce_paddr(idx) (хаб не делает SCSI,
// страница иначе простаивала бы), размер — реальный MaxPacketSize
// эндпоинта (обычно 1 байт на ≤7-портовый хаб). dev.hub_int_pending_trb
// запоминает device-адрес ЭТОЙ TRB — poll_hub_interrupts() не блокируясь
// проверяет его try_check_transfer_complete()'ом на каждом
// heartbeat-тике. Перезаряжается заново после каждого обработанного
// события (см. poll_hub_interrupts()).
static void hub_enqueue_interrupt_listen(int idx) {
    UsbDeviceSlot &dev = g_usb_devices[idx];
    if (dev.hub_int_dci == 0) return; // не было Interrupt-эндпоинта — нечего слушать
    uint32_t len = dev.hub_int_ep_mps ? dev.hub_int_ep_mps : 1;
    uint64_t trb_addr = ring_enqueue_trb(dev.bulkin_ring, to_dev_addr(dev.bounce_paddr), len,
                                          trb_type(TRB_TYPE_NORMAL) | (1u << 2) /*ISP*/ | TRB_IOC);
    dev.hub_int_pending_trb = trb_addr;
    ring_endpoint_doorbell(dev.slot_id, dev.hub_int_dci);
}

// Milestone B2 (Фаза 15) — class-специфичные запросы К ПОРТУ хаба (не к
// самому хабу) — bmRequestType для recipient="Other" (см. USB 2.0 spec
// 9.4/11.24.2): 0xA3 (Device-to-Host|Class|Other) для GET_STATUS, 0x23
// (Host-to-Device|Class|Other) для SET/CLEAR_FEATURE. wIndex = номер
// порта хаба (1..bNbrPorts, НЕ 0-based).
constexpr uint8_t  USB_HUB_REQ_GET_STATUS    = 0x00;
constexpr uint8_t  USB_HUB_REQ_CLEAR_FEATURE = 0x01;
constexpr uint8_t  USB_HUB_REQ_SET_FEATURE   = 0x03;
constexpr uint16_t USB_PORT_FEAT_POWER             = 8;  // см. USB 2.0 spec Table 11-17
constexpr uint16_t USB_PORT_FEAT_RESET             = 4;
constexpr uint16_t USB_PORT_FEAT_C_PORT_CONNECTION = 16;
constexpr uint16_t USB_PORT_FEAT_C_PORT_RESET      = 20;
// issuse.txt №15 — НАЙДЕНО НА ЖИВОМ ЖЕЛЕЗЕ: у SS-порта wPortChange несёт
// ещё и C_PORT_LINK_STATE (бит6, USB 3.x spec Table 10-12/10-7) —
// меняется, например, при отключении SS-устройства (линк уходит из U0).
// Раньше код чистил только C_PORT_CONNECTION — если реальное событие
// несло ТОЛЬКО этот бит (что и происходит после отключения), хаб
// продолжал репортить его на КАЖДОМ interrupt-опросе бесконечно (см.
// живой лог — один и тот же bitmap/DIAG на каждом тике без остановки).
constexpr uint16_t USB_PORT_FEAT_C_PORT_LINK_STATE = 25;
constexpr uint16_t USB_PORT_STAT_CONNECTION = 0x0001; // wPortStatus, бит0
constexpr uint16_t USB_PORT_STAT_LOW_SPEED  = 0x0200;  // wPortStatus, бит9
constexpr uint16_t USB_PORT_STAT_HIGH_SPEED = 0x0400;  // wPortStatus, бит10 (ни один из двух = Full-Speed)
constexpr uint16_t USB_PORT_STAT_C_RESET    = 0x0010;  // wPortChange, бит4
constexpr uint16_t USB_PORT_STAT_C_LINK_STATE = 0x0040; // wPortChange, бит6 (SS)

// GET_PORT_STATUS возвращает 4 байта: wPortStatus (0-1), wPortChange
// (2-3) — тот же ctrlbuf(idx), что и у GET_DESCRIPTOR (переиспользуем,
// хаб не делает ничего параллельно с этим control-transfer'ом).
static bool hub_get_port_status(seL4_CPtr console_ep, int idx, uint8_t slot_id, uint8_t hub_port,
                                 uint16_t &status, uint16_t &change) {
    volatile uint8_t *buf = ctrlbuf_vaddr(idx);
    buf[0] = buf[1] = buf[2] = buf[3] = 0;
    uint32_t actual = 0;
    if (!ep0_control_in(console_ep, g_usb_devices[idx].ep0_ring, slot_id, 0xA3, USB_HUB_REQ_GET_STATUS,
                         0, hub_port, 4, g_usb_devices[idx].ctrl_buf_paddr, actual)) return false;
    if (actual < 4) return false;
    status = (uint16_t)buf[0] | ((uint16_t)buf[1] << 8);
    change = (uint16_t)buf[2] | ((uint16_t)buf[3] << 8);
    return true;
}
static bool hub_set_port_feature(seL4_CPtr console_ep, int idx, uint8_t slot_id, uint8_t hub_port, uint16_t feature) {
    return ep0_control_no_data(console_ep, g_usb_devices[idx].ep0_ring, slot_id, 0x23, USB_HUB_REQ_SET_FEATURE, feature, hub_port);
}
static bool hub_clear_port_feature(seL4_CPtr console_ep, int idx, uint8_t slot_id, uint8_t hub_port, uint16_t feature) {
    return ep0_control_no_data(console_ep, g_usb_devices[idx].ep0_ring, slot_id, 0x23, USB_HUB_REQ_CLEAR_FEATURE, feature, hub_port);
}

// wait_ms() (см. выше) зовёт предикат БЕЗ параметров — состояние
// передаём через глобалы, тот же приём, что g_wait_port/cond_port_ped()
// у step5_port_reset() (корневые порты).
static int g_hub_wait_idx = 0;
static uint8_t g_hub_wait_slot_id = 0, g_hub_wait_port = 0;
static uint16_t g_hub_wait_status = 0, g_hub_wait_change = 0;
static bool cond_hub_port_c_reset() {
    if (!hub_get_port_status(g_console_ep, g_hub_wait_idx, g_hub_wait_slot_id, g_hub_wait_port,
                              g_hub_wait_status, g_hub_wait_change)) return false;
    return (g_hub_wait_change & USB_PORT_STAT_C_RESET) != 0;
}

// Форвард-декларации — определены значительно ниже по файлу (рядом с
// try_enumerate_port()/enumerate_and_mount_device(), см. Milestone A1/B3/B4),
// но нужны уже здесь, в hub_handle_port_connect()/poll_hub_interrupts().
static int find_free_device_slot();
static void enumerate_device_behind_hub(seL4_CPtr console_ep, int hub_idx, uint8_t hub_port, int idx, uint8_t &out_slot_id, uint32_t device_speed);
static void unmount_usb_storage(seL4_CPtr console_ep, int idx);

// Milestone B3/B4/B5 (Фаза 15) — общая обработка "на этом порту хаба
// подтверждён PORT_CONNECTION" (вызывающий уже проверил бит) — сброс
// порта (SET_PORT_FEATURE(RESET), тот же смысл, что step5_port_reset()
// для корневых портов: переводит устройство из Powered в Default
// state — без этого никакого ответа на Address Device не будет), ждём
// C_PORT_RESET (ограниченное число попыток — wait_ms). Дальше — развилка
// по типу САМОГО хаба (не ребёнка — тип ребёнка ещё не известен):
// SuperSpeed-хаб (Milestone B5, доп.) — читаем Port Link State (биты
// [8:5] wPortStatus, USB 3.x spec Table 10-9) — 0=U0 значит линк реально
// поднялся; HS/FS-хаб — как раньше, LOW_SPEED/HIGH_SPEED биты (ни один
// не установлен = Full-Speed), не-HS честно отклоняем (Transaction
// Translator не реализован). Дальше — общий код-путь: адресуем и
// монтируем (enumerate_device_behind_hub()) с уже определённой
// скоростью. Используется и статическим сканом при перечислении хаба
// (step_hub_scan_downstream_ports, B2/B3), и динамическим опросом
// interrupt-эндпоинта (poll_hub_interrupts, B4) — не дублируем.
static void hub_handle_port_connect(seL4_CPtr console_ep, int idx, uint8_t hp) {
    UsbDeviceSlot &dev = g_usb_devices[idx];
    bool hub_is_ss = (dev.found.device_protocol == 3);
    sys_puthex32(console_ep, "[USB]   Хаб-порт с устройством: ", hp);

    uint32_t device_speed;
    if (hub_is_ss) {
        // Milestone B5 (доп.) — НАЙДЕНО НА ЖИВОМ ЖЕЛЕЗЕ: settle-задержка
        // после явного Hot Reset не помогла (Address Device всё равно
        // падал с кодом 0x04, USB Transaction Error). Гипотеза №2 — та
        // же логика, что УЖЕ подтверждена для КОРНЕВЫХ SS-портов (см.
        // step5_port_reset(): "SS-порты xHCI включаются АВТОМАТИЧЕСКИ по
        // завершении link-training, БЕЗ явного Port Reset"): SS-линк за
        // хабом ТОЖЕ тренируется сам при физическом подключении, явный
        // Hot Reset на УЖЕ поднятом до U0 линке — лишний и, похоже,
        // вредный шаг. Сначала смотрим ТЕКУЩИЙ Port Link State БЕЗ
        // сброса; сбрасываем ТОЛЬКО если реально не поднялся сам.
        uint16_t status = 0, change = 0;
        if (!hub_get_port_status(console_ep, idx, dev.slot_id, hp, status, change)) {
            sys_puts(console_ep, "[USB]     ОШИБКА: GET_PORT_STATUS не удался.\n");
            return;
        }
        uint16_t pls = (status >> 5) & 0xFu; // Port Link State, биты [8:5]
        sys_puthex32(console_ep, "[USB]     Port Link State ДО сброса = ", pls);
        if (pls != 0) { // не поднялся сам — пробуем явный Hot Reset как раньше (запасной путь)
            if (!hub_set_port_feature(console_ep, idx, dev.slot_id, hp, USB_PORT_FEAT_RESET)) {
                sys_puts(console_ep, "[USB]     ОШИБКА: SET_PORT_FEATURE(RESET) не удался.\n");
                return;
            }
            g_hub_wait_idx = idx; g_hub_wait_slot_id = dev.slot_id; g_hub_wait_port = hp;
            bool reset_done = wait_ms(500, cond_hub_port_c_reset);
            status = g_hub_wait_status;
            hub_clear_port_feature(console_ep, idx, dev.slot_id, hp, USB_PORT_FEAT_C_PORT_RESET);
            if (!reset_done) {
                sys_puts(console_ep, "[USB]     ОШИБКА: порт не сообщил о завершении сброса за 500мс.\n");
                hub_clear_port_feature(console_ep, idx, dev.slot_id, hp, USB_PORT_FEAT_C_PORT_CONNECTION);
                return;
            }
            pls = (status >> 5) & 0xFu;
            sys_puthex32(console_ep, "[USB]     Port Link State ПОСЛЕ сброса = ", pls);
        }
        hub_clear_port_feature(console_ep, idx, dev.slot_id, hp, USB_PORT_FEAT_C_PORT_CONNECTION);
        if (pls != 0) { // 0 = U0 (линк активен) — см. USB 3.x spec Table 10-11
            sys_puts(console_ep, "[USB]     ПРЕДУПРЕЖДЕНИЕ: SuperSpeed-порт не поднялся до U0 — устройство честно пропущено.\n");
            return;
        }
        sys_puts(console_ep, "[USB]     Скорость: SuperSpeed\n");
        device_speed = 4;
    } else {
        if (!hub_set_port_feature(console_ep, idx, dev.slot_id, hp, USB_PORT_FEAT_RESET)) {
            sys_puts(console_ep, "[USB]     ОШИБКА: SET_PORT_FEATURE(RESET) не удался.\n");
            return;
        }
        g_hub_wait_idx = idx; g_hub_wait_slot_id = dev.slot_id; g_hub_wait_port = hp;
        bool reset_done = wait_ms(500, cond_hub_port_c_reset);
        uint16_t status = g_hub_wait_status;
        // Очищаем change-биты в ЛЮБОМ случае — иначе следующий скан/опрос
        // увидит "залипший" C_PORT_RESET/C_PORT_CONNECTION с этого раза.
        hub_clear_port_feature(console_ep, idx, dev.slot_id, hp, USB_PORT_FEAT_C_PORT_RESET);
        hub_clear_port_feature(console_ep, idx, dev.slot_id, hp, USB_PORT_FEAT_C_PORT_CONNECTION);
        if (!reset_done) {
            sys_puts(console_ep, "[USB]     ОШИБКА: порт не сообщил о завершении сброса за 500мс.\n");
            return;
        }
        bool low_speed = (status & USB_PORT_STAT_LOW_SPEED) != 0;
        bool high_speed = (status & USB_PORT_STAT_HIGH_SPEED) != 0;
        sys_puts(console_ep, low_speed ? "[USB]     Скорость: Low-Speed\n"
                                        : (high_speed ? "[USB]     Скорость: High-Speed\n" : "[USB]     Скорость: Full-Speed\n"));
        if (!high_speed) {
            sys_puts(console_ep, "[USB]     ПРЕДУПРЕЖДЕНИЕ: не High-Speed — Transaction Translator не реализован в этой фазе, монтирование этого устройства невозможно (честное ограничение, см. план).\n");
            return;
        }
        device_speed = 3;
    }
    // Milestone B3 (Фаза 15) — устройство за хабом: тот же приём
    // выделения слота/освобождения при неудаче, что try_enumerate_port()
    // использует для корневых портов.
    int child_idx = find_free_device_slot();
    if (child_idx < 0) {
        sys_puts(console_ep, "[USB]     ПРЕДУПРЕЖДЕНИЕ: нет свободных слотов под устройство за хабом (USB_MAX_DEVICES исчерпан).\n");
        return;
    }
    g_usb_devices[child_idx].found = UsbFoundDevice{};
    g_usb_devices[child_idx].bulk_eps = UsbBulkEndpoints{};
    // Milestone B4 — резервируем idx СРАЗУ (тот же фикс, что и у самого
    // хаба в try_enumerate_port() — иначе повторная гонка индексов,
    // если два порта хаба меняются между соседними heartbeat-тиками).
    g_usb_devices[child_idx].in_use = true;
    g_usb_devices[child_idx].port = 0; // не корневой порт — см. комментарий у behind_hub в UsbDeviceSlot
    uint8_t child_slot_id = 0;
    enumerate_device_behind_hub(console_ep, idx, hp, child_idx, child_slot_id, device_speed);
    // Milestone B5 (доп.) — НАЙДЕНО НА ЖИВОМ ЖЕЛЕЗЕ: комбо-донгл (SS-хаб
    // на отдельном корневом порту + отдельная HS-ипостась ТОГО ЖЕ
    // донгла, видна ЗА уже известным хабом) — успех означает ЛИБО хаб
    // (continue_enumeration_after_address() уже честно и рекурсивно
    // прошла Hub Descriptor/Configure Endpoint/downstream-скан для НЕГО
    // ЖЕ, тот же общий код-путь), ЛИБО накопитель (bulk_eps.found) — та
    // же логика, что try_enumerate_port() уже использует для корневых
    // портов (is_hub || is_storage), раньше здесь проверялся только
    // накопитель, и хаб-за-хабом ошибочно считался неудачей.
    bool child_is_hub = (g_usb_devices[child_idx].found.device_class == USB_CLASS_HUB);
    bool child_ok = g_usb_devices[child_idx].found.found && (child_is_hub || g_usb_devices[child_idx].bulk_eps.found);
    if (!child_ok) {
        if (child_slot_id != 0) step_disable_slot(console_ep, child_slot_id);
        g_usb_devices[child_idx].found.found = false;
        g_usb_devices[child_idx].bulk_eps.found = false;
        g_usb_devices[child_idx].behind_hub = false;
        g_usb_devices[child_idx].in_use = false;
        return;
    }
    if (child_is_hub) {
        // Хаб-за-хабом — не "смонтирован" в exFAT-смысле, отдельное
        // honest-сообщение (та же логика, что уже есть в
        // poll_ports_for_hotplug() для хаба на корневом порту).
        sys_puthex32(console_ep, "[USB]     Хаб за хабом подключён и опрошен, портов = ", g_usb_devices[child_idx].hub_num_ports);
    } else if (!g_usb_devices[child_idx].storage_mounted) {
        sys_puts(console_ep, "[USB]     Устройство за хабом перечислено, но exFAT не смонтировался (не exFAT / повреждён / не тот раздел).\n");
    } else {
        sys_puts(console_ep, "[USB]     Флешка за хабом автоматически смонтирована: /mnt/");
        sys_puts(console_ep, g_usb_devices[child_idx].volume_name);
        sys_puts(console_ep, "\n");
    }
}

// issuse.txt №15 — асинхронный, тик-возобновляемый аналог
// hub_handle_port_connect() ВЫШЕ (та функция оставлена нетронутой,
// используется только статическим сканом step_hub_scan_downstream_ports()
// при ПЕРВОМ подключении хаба — одноразовое событие при bring-up, не тот
// повторяющийся источник задержки, о котором issuse.txt №15). Этот путь
// используется ТОЛЬКО из poll_hub_interrupts() (динамический опрос
// interrupt-эндпоинта хаба, каждый heartbeat-тик) — именно там
// нестабильный/медленный нижестоящий порт даёт повторные ретраи,
// синхронно державшие ВЕСЬ usb_driver (и VFS-команды ко ВСЕМ другим
// смонтированным томам) колом на секунды.
//
// Фаза 1 этого фикса: сама последовательность сброса порта + определения
// скорости (то, что issuse.txt №15 явно называет — hub_get_port_status/
// hub_handle_port_connect) — асинхронная, тик-возобновляемая. Финальное
// перечисление устройства (enumerate_device_behind_hub() — Enable Slot/
// Address Device/дескрипторы/SCSI/монтирование exFAT, общий "стержень" с
// root-port путём, см. continue_enumeration_after_address()) НАМЕРЕННО
// остаётся ОДНИМ синхронным вызовом здесь — это одноразовый блокирующий
// вызов НА УСПЕШНОЕ подключение (не повторяющиеся ретраи нестабильного
// порта — та часть, что реально копится в секунды), и полная асинхронная
// переделка ~15 функций общего движка (используемого И root-port путём)
// — отдельная, более крупная следующая фаза, не в этом заходе.
//
// Однопоточность: как и оригинал (см. g_hub_wait_* выше), это ОДИН
// активный процесс подключения за раз — hub_conn_async_start() молча
// игнорирует новый вызов, если уже что-то в процессе; бит изменения
// порта на хабе НЕ чистится, пока мы не дойдём до него, поэтому хаб
// продолжит репортить его на каждом interrupt-опросе — событие не
// теряется, просто откладывается до освобождения (см. poll_hub_interrupts()).
enum class HubConnSt : uint8_t {
    IDLE,
    SS_PRECHECK, SS_RESET_SET, SS_RESET_POLL, SS_CLEAR_C_RESET, SS_CLEAR_C_CONN,
    NONSS_RESET_SET, NONSS_RESET_POLL, NONSS_CLEAR_C_RESET, NONSS_CLEAR_C_CONN,
    START_ENUM,
};
static HubConnSt g_hub_conn_state = HubConnSt::IDLE;
static AsyncCtrl  g_hub_conn_actrl;
static int        g_hub_conn_idx = 0;
static uint8_t    g_hub_conn_port = 0;
static bool       g_hub_conn_is_ss = false;
static bool       g_hub_conn_reset_needed = true; // (SS) false, если Port Link State уже 0 до всякого сброса
static bool       g_hub_conn_reset_ok = false;
static uint16_t   g_hub_conn_status = 0;          // последний прочитанный wPortStatus (для финального speed-check)
static uint16_t   g_hub_conn_pls = 0;              // (SS) Port Link State, биты[8:5]
static uint64_t   g_hub_conn_poll_deadline = 0;    // общий бюджет цикла RESET_POLL — как у исходного wait_ms(500,...)

static void hub_conn_async_start(seL4_CPtr console_ep, int idx, uint8_t hp) {
    if (g_hub_conn_state != HubConnSt::IDLE) return; // уже что-то в процессе, см. комментарий выше
    UsbDeviceSlot &dev = g_usb_devices[idx];
    g_hub_conn_idx = idx;
    g_hub_conn_port = hp;
    g_hub_conn_is_ss = (dev.found.device_protocol == 3);
    g_hub_conn_reset_needed = true;
    sys_puthex32(console_ep, "[USB]   Хаб-порт с устройством: ", hp);
    volatile uint8_t *buf = ctrlbuf_vaddr(idx);
    buf[0] = buf[1] = buf[2] = buf[3] = 0;
    if (g_hub_conn_is_ss) {
        async_ctrl_in_start(g_hub_conn_actrl, dev.ep0_ring, dev.slot_id, 0xA3, USB_HUB_REQ_GET_STATUS, 0, hp, 4, dev.ctrl_buf_paddr);
        g_hub_conn_state = HubConnSt::SS_PRECHECK;
    } else {
        async_ctrl_no_data_start(g_hub_conn_actrl, dev.ep0_ring, dev.slot_id, 0x23, USB_HUB_REQ_SET_FEATURE, USB_PORT_FEAT_RESET, hp);
        g_hub_conn_state = HubConnSt::NONSS_RESET_SET;
    }
}

// Вызывается КАЖДЫЙ heartbeat-тик из main() — НЕ блокируется никогда,
// продвигает текущий шаг ровно на один tick вперёд (или ничего не делает,
// если IDLE — большинство тиков).
static void hub_conn_async_tick(seL4_CPtr console_ep) {
    if (g_hub_conn_state == HubConnSt::IDLE) return;
    int idx = g_hub_conn_idx;
    UsbDeviceSlot &dev = g_usb_devices[idx];
    uint8_t hp = g_hub_conn_port;

    switch (g_hub_conn_state) {

    case HubConnSt::SS_PRECHECK: {
        if (!async_ctrl_tick(g_hub_conn_actrl)) return;
        if (g_hub_conn_actrl.state != AsyncCtrl::DONE || g_hub_conn_actrl.actual_length < 4) {
            sys_puts(console_ep, "[USB]     ОШИБКА: GET_PORT_STATUS не удался.\n");
            g_hub_conn_state = HubConnSt::IDLE; return;
        }
        volatile uint8_t *buf = ctrlbuf_vaddr(idx);
        g_hub_conn_status = (uint16_t)buf[0] | ((uint16_t)buf[1] << 8);
        g_hub_conn_pls = (g_hub_conn_status >> 5) & 0xFu;
        sys_puthex32(console_ep, "[USB]     Port Link State ДО сброса = ", g_hub_conn_pls);
        if (g_hub_conn_pls != 0) {
            async_ctrl_no_data_start(g_hub_conn_actrl, dev.ep0_ring, dev.slot_id, 0x23, USB_HUB_REQ_SET_FEATURE, USB_PORT_FEAT_RESET, hp);
            g_hub_conn_state = HubConnSt::SS_RESET_SET;
        } else {
            g_hub_conn_reset_needed = false;
            async_ctrl_no_data_start(g_hub_conn_actrl, dev.ep0_ring, dev.slot_id, 0x23, USB_HUB_REQ_CLEAR_FEATURE, USB_PORT_FEAT_C_PORT_CONNECTION, hp);
            g_hub_conn_state = HubConnSt::SS_CLEAR_C_CONN;
        }
        return;
    }

    case HubConnSt::SS_RESET_SET: {
        if (!async_ctrl_tick(g_hub_conn_actrl)) return;
        if (g_hub_conn_actrl.state != AsyncCtrl::DONE) {
            sys_puts(console_ep, "[USB]     ОШИБКА: SET_PORT_FEATURE(RESET) не удался.\n");
            g_hub_conn_state = HubConnSt::IDLE; return;
        }
        g_hub_conn_poll_deadline = read_cntvct() + 500ull * g_cntfrq / 1000;
        volatile uint8_t *buf = ctrlbuf_vaddr(idx);
        buf[0] = buf[1] = buf[2] = buf[3] = 0;
        async_ctrl_in_start(g_hub_conn_actrl, dev.ep0_ring, dev.slot_id, 0xA3, USB_HUB_REQ_GET_STATUS, 0, hp, 4, dev.ctrl_buf_paddr);
        g_hub_conn_state = HubConnSt::SS_RESET_POLL;
        return;
    }
    case HubConnSt::SS_RESET_POLL: {
        if (!async_ctrl_tick(g_hub_conn_actrl)) {
            if (read_cntvct() >= g_hub_conn_poll_deadline) {
                g_hub_conn_reset_ok = false;
                async_ctrl_no_data_start(g_hub_conn_actrl, dev.ep0_ring, dev.slot_id, 0x23, USB_HUB_REQ_CLEAR_FEATURE, USB_PORT_FEAT_C_PORT_RESET, hp);
                g_hub_conn_state = HubConnSt::SS_CLEAR_C_RESET;
            }
            return;
        }
        volatile uint8_t *buf = ctrlbuf_vaddr(idx);
        bool ok = (g_hub_conn_actrl.state == AsyncCtrl::DONE && g_hub_conn_actrl.actual_length >= 4);
        uint16_t change = 0;
        if (ok) {
            g_hub_conn_status = (uint16_t)buf[0] | ((uint16_t)buf[1] << 8);
            change = (uint16_t)buf[2] | ((uint16_t)buf[3] << 8);
        }
        if ((ok && (change & USB_PORT_STAT_C_RESET)) || read_cntvct() >= g_hub_conn_poll_deadline) {
            g_hub_conn_reset_ok = ok && (change & USB_PORT_STAT_C_RESET) != 0;
            async_ctrl_no_data_start(g_hub_conn_actrl, dev.ep0_ring, dev.slot_id, 0x23, USB_HUB_REQ_CLEAR_FEATURE, USB_PORT_FEAT_C_PORT_RESET, hp);
            g_hub_conn_state = HubConnSt::SS_CLEAR_C_RESET;
            return;
        }
        // Ни C_PORT_RESET, ни дедлайн — повторяем опрос (см. cond_hub_port_c_reset()).
        buf[0] = buf[1] = buf[2] = buf[3] = 0;
        async_ctrl_in_start(g_hub_conn_actrl, dev.ep0_ring, dev.slot_id, 0xA3, USB_HUB_REQ_GET_STATUS, 0, hp, 4, dev.ctrl_buf_paddr);
        return;
    }
    case HubConnSt::SS_CLEAR_C_RESET: {
        if (!async_ctrl_tick(g_hub_conn_actrl)) return; // код завершения самого CLEAR не критичен, оригинал его тоже не проверял
        async_ctrl_no_data_start(g_hub_conn_actrl, dev.ep0_ring, dev.slot_id, 0x23, USB_HUB_REQ_CLEAR_FEATURE, USB_PORT_FEAT_C_PORT_CONNECTION, hp);
        g_hub_conn_state = HubConnSt::SS_CLEAR_C_CONN;
        return;
    }
    case HubConnSt::SS_CLEAR_C_CONN: {
        if (!async_ctrl_tick(g_hub_conn_actrl)) return;
        if (g_hub_conn_reset_needed && !g_hub_conn_reset_ok) {
            sys_puts(console_ep, "[USB]     ОШИБКА: порт не сообщил о завершении сброса за 500мс.\n");
            g_hub_conn_state = HubConnSt::IDLE; return;
        }
        if (g_hub_conn_reset_needed) g_hub_conn_pls = (g_hub_conn_status >> 5) & 0xFu;
        sys_puthex32(console_ep, "[USB]     Port Link State ПОСЛЕ сброса = ", g_hub_conn_pls);
        if (g_hub_conn_pls != 0) {
            sys_puts(console_ep, "[USB]     ПРЕДУПРЕЖДЕНИЕ: SuperSpeed-порт не поднялся до U0 — устройство честно пропущено.\n");
            g_hub_conn_state = HubConnSt::IDLE; return;
        }
        sys_puts(console_ep, "[USB]     Скорость: SuperSpeed\n");
        g_hub_conn_state = HubConnSt::START_ENUM;
        return;
    }

    case HubConnSt::NONSS_RESET_SET: {
        if (!async_ctrl_tick(g_hub_conn_actrl)) return;
        if (g_hub_conn_actrl.state != AsyncCtrl::DONE) {
            sys_puts(console_ep, "[USB]     ОШИБКА: SET_PORT_FEATURE(RESET) не удался.\n");
            g_hub_conn_state = HubConnSt::IDLE; return;
        }
        g_hub_conn_poll_deadline = read_cntvct() + 500ull * g_cntfrq / 1000;
        volatile uint8_t *buf = ctrlbuf_vaddr(idx);
        buf[0] = buf[1] = buf[2] = buf[3] = 0;
        async_ctrl_in_start(g_hub_conn_actrl, dev.ep0_ring, dev.slot_id, 0xA3, USB_HUB_REQ_GET_STATUS, 0, hp, 4, dev.ctrl_buf_paddr);
        g_hub_conn_state = HubConnSt::NONSS_RESET_POLL;
        return;
    }
    case HubConnSt::NONSS_RESET_POLL: {
        if (!async_ctrl_tick(g_hub_conn_actrl)) {
            if (read_cntvct() >= g_hub_conn_poll_deadline) {
                g_hub_conn_reset_ok = false;
                async_ctrl_no_data_start(g_hub_conn_actrl, dev.ep0_ring, dev.slot_id, 0x23, USB_HUB_REQ_CLEAR_FEATURE, USB_PORT_FEAT_C_PORT_RESET, hp);
                g_hub_conn_state = HubConnSt::NONSS_CLEAR_C_RESET;
            }
            return;
        }
        volatile uint8_t *buf = ctrlbuf_vaddr(idx);
        bool ok = (g_hub_conn_actrl.state == AsyncCtrl::DONE && g_hub_conn_actrl.actual_length >= 4);
        uint16_t change = 0;
        if (ok) {
            g_hub_conn_status = (uint16_t)buf[0] | ((uint16_t)buf[1] << 8);
            change = (uint16_t)buf[2] | ((uint16_t)buf[3] << 8);
        }
        if ((ok && (change & USB_PORT_STAT_C_RESET)) || read_cntvct() >= g_hub_conn_poll_deadline) {
            g_hub_conn_reset_ok = ok && (change & USB_PORT_STAT_C_RESET) != 0;
            async_ctrl_no_data_start(g_hub_conn_actrl, dev.ep0_ring, dev.slot_id, 0x23, USB_HUB_REQ_CLEAR_FEATURE, USB_PORT_FEAT_C_PORT_RESET, hp);
            g_hub_conn_state = HubConnSt::NONSS_CLEAR_C_RESET;
            return;
        }
        buf[0] = buf[1] = buf[2] = buf[3] = 0;
        async_ctrl_in_start(g_hub_conn_actrl, dev.ep0_ring, dev.slot_id, 0xA3, USB_HUB_REQ_GET_STATUS, 0, hp, 4, dev.ctrl_buf_paddr);
        return;
    }
    case HubConnSt::NONSS_CLEAR_C_RESET: {
        if (!async_ctrl_tick(g_hub_conn_actrl)) return;
        async_ctrl_no_data_start(g_hub_conn_actrl, dev.ep0_ring, dev.slot_id, 0x23, USB_HUB_REQ_CLEAR_FEATURE, USB_PORT_FEAT_C_PORT_CONNECTION, hp);
        g_hub_conn_state = HubConnSt::NONSS_CLEAR_C_CONN;
        return;
    }
    case HubConnSt::NONSS_CLEAR_C_CONN: {
        if (!async_ctrl_tick(g_hub_conn_actrl)) return;
        if (!g_hub_conn_reset_ok) {
            sys_puts(console_ep, "[USB]     ОШИБКА: порт не сообщил о завершении сброса за 500мс.\n");
            g_hub_conn_state = HubConnSt::IDLE; return;
        }
        bool low_speed = (g_hub_conn_status & USB_PORT_STAT_LOW_SPEED) != 0;
        bool high_speed = (g_hub_conn_status & USB_PORT_STAT_HIGH_SPEED) != 0;
        sys_puts(console_ep, low_speed ? "[USB]     Скорость: Low-Speed\n"
                                        : (high_speed ? "[USB]     Скорость: High-Speed\n" : "[USB]     Скорость: Full-Speed\n"));
        if (!high_speed) {
            sys_puts(console_ep, "[USB]     ПРЕДУПРЕЖДЕНИЕ: не High-Speed — Transaction Translator не реализован в этой фазе, монтирование этого устройства невозможно (честное ограничение, см. план).\n");
            g_hub_conn_state = HubConnSt::IDLE; return;
        }
        g_hub_conn_state = HubConnSt::START_ENUM;
        return;
    }

    case HubConnSt::START_ENUM: {
        // Фаза 1 (см. комментарий у enum выше) — отсюда и глубже всё ещё
        // ОДИН синхронный блокирующий вызов, как в оригинальном
        // hub_handle_port_connect() (см. её же копию этого хвоста).
        uint32_t device_speed = g_hub_conn_is_ss ? 4u : 3u;
        int child_idx = find_free_device_slot();
        if (child_idx < 0) {
            sys_puts(console_ep, "[USB]     ПРЕДУПРЕЖДЕНИЕ: нет свободных слотов под устройство за хабом (USB_MAX_DEVICES исчерпан).\n");
            g_hub_conn_state = HubConnSt::IDLE; return;
        }
        g_usb_devices[child_idx].found = UsbFoundDevice{};
        g_usb_devices[child_idx].bulk_eps = UsbBulkEndpoints{};
        g_usb_devices[child_idx].in_use = true;
        g_usb_devices[child_idx].port = 0;
        uint8_t child_slot_id = 0;
        enumerate_device_behind_hub(console_ep, idx, hp, child_idx, child_slot_id, device_speed);
        // issuse.txt №15 — диагностика (временная, до выяснения находки
        // "флешка за хабом определилась как хаб за хабом"): что реально
        // прочитал Device Descriptor для child_idx, и на каком slot_id —
        // плюс VID/PID/slot_id САМОГО РОДИТЕЛЬСКОГО хаба (idx) рядом, для
        // прямого сравнения на предмет "child читает данные parent'а".
        sys_puthex32(console_ep, "[USB]     DIAG parent_idx=", (uint32_t)idx);
        sys_puthex32(console_ep, "[USB]     DIAG parent_slot_id=", (uint32_t)dev.slot_id);
        sys_puthex32(console_ep, "[USB]     DIAG parent_vendor=", dev.found.vendor_id);
        sys_puthex32(console_ep, "[USB]     DIAG parent_product=", dev.found.product_id);
        sys_puthex32(console_ep, "[USB]     DIAG child_idx=", (uint32_t)child_idx);
        sys_puthex32(console_ep, "[USB]     DIAG child_slot_id=", (uint32_t)child_slot_id);
        sys_puthex32(console_ep, "[USB]     DIAG vendor=", g_usb_devices[child_idx].found.vendor_id);
        sys_puthex32(console_ep, "[USB]     DIAG product=", g_usb_devices[child_idx].found.product_id);
        sys_puthex32(console_ep, "[USB]     DIAG device_class=", g_usb_devices[child_idx].found.device_class);
        sys_puthex32(console_ep, "[USB]     DIAG device_subclass=", g_usb_devices[child_idx].found.device_subclass);
        sys_puthex32(console_ep, "[USB]     DIAG device_protocol=", g_usb_devices[child_idx].found.device_protocol);
        sys_puthex32(console_ep, "[USB]     DIAG found.found=", (uint32_t)g_usb_devices[child_idx].found.found);
        bool child_is_hub = (g_usb_devices[child_idx].found.device_class == USB_CLASS_HUB);
        bool child_ok = g_usb_devices[child_idx].found.found && (child_is_hub || g_usb_devices[child_idx].bulk_eps.found);
        if (!child_ok) {
            if (child_slot_id != 0) step_disable_slot(console_ep, child_slot_id);
            g_usb_devices[child_idx].found.found = false;
            g_usb_devices[child_idx].bulk_eps.found = false;
            g_usb_devices[child_idx].behind_hub = false;
            g_usb_devices[child_idx].in_use = false;
        } else if (child_is_hub) {
            sys_puthex32(console_ep, "[USB]     Хаб за хабом подключён и опрошен, портов = ", g_usb_devices[child_idx].hub_num_ports);
        } else if (!g_usb_devices[child_idx].storage_mounted) {
            sys_puts(console_ep, "[USB]     Устройство за хабом перечислено, но exFAT не смонтировался (не exFAT / повреждён / не тот раздел).\n");
        } else {
            sys_puts(console_ep, "[USB]     Флешка за хабом автоматически смонтирована: /mnt/");
            sys_puts(console_ep, g_usb_devices[child_idx].volume_name);
            sys_puts(console_ep, "\n");
        }
        g_hub_conn_state = HubConnSt::IDLE;
        return;
    }

    default:
        g_hub_conn_state = HubConnSt::IDLE;
        return;
    }
}

// Milestone B2/B3 (Фаза 15) — статический скан downstream-портов хаба
// (один раз, при перечислении САМОГО хаба): GET_PORT_STATUS на КАЖДОМ
// порту (1..hub_num_ports — хаб-порты нумеруются с 1, не с 0), если
// PORT_CONNECTION установлен — hub_handle_port_connect() (общая с B4
// логика сброса/скорости/адресации).
static void step_hub_scan_downstream_ports(seL4_CPtr console_ep, int idx, uint8_t slot_id) {
    UsbDeviceSlot &dev = g_usb_devices[idx];
    // НАЙДЕНО НА ЖИВОМ ЖЕЛЕЗЕ — первая версия этого скана молча не видела
    // НИЧЕГО ни на одном порту (даже физически подключённую флешку) — по
    // USB 2.0 spec (11.11, Power Switching) порт с выключенным питанием
    // физически не может почувствовать подключение (пул-апы на D+/D- не
    // запитаны), GET_PORT_STATUS честно вернёт PORT_CONNECTION=0. Порты
    // НЕ включаются автоматически сами (ни в ganged, ни в individual
    // режиме питания — хост обязан явно послать SET_PORT_FEATURE(POWER),
    // см. ту же логику в hub-драйвере Linux) — раньше это было пропущено.
    // Шлём POWER на КАЖДЫЙ порт (безопасно для обоих режимов ganged/
    // individual — избыточно для ganged, но не вредно), затем ОДНА пауза
    // bPwrOn2PwrGood*2мс (не на каждый порт — она покрывает все разом).
    for (uint8_t hp = 1; hp <= dev.hub_num_ports; hp++) {
        if (!hub_set_port_feature(console_ep, idx, slot_id, hp, USB_PORT_FEAT_POWER)) {
            sys_puthex32(console_ep, "[USB]   ОШИБКА (хаб): SET_PORT_FEATURE(POWER) порта ", hp);
        }
    }
    uint32_t pwr_wait_ms = (uint32_t)dev.hub_pwr_on_to_pwr_good * 2;
    if (pwr_wait_ms < 20) pwr_wait_ms = 20; // подстраховка на случай нулевого/бракованного значения в дескрипторе
    {
        uint64_t deadline = read_cntvct() + (uint64_t)pwr_wait_ms * g_cntfrq / 1000;
        while (read_cntvct() < deadline) seL4_Yield();
    }
    for (uint8_t hp = 1; hp <= dev.hub_num_ports; hp++) {
        uint16_t status = 0, change = 0;
        if (!hub_get_port_status(console_ep, idx, slot_id, hp, status, change)) {
            sys_puthex32(console_ep, "[USB]   ОШИБКА (хаб): GET_PORT_STATUS порта ", hp);
            continue;
        }
        if (!(status & USB_PORT_STAT_CONNECTION)) {
            // issuse.txt №15 — НАЙДЕНО ЧТЕНИЕМ КОДА: SET_FEATURE(POWER) на
            // всех портах разом — известный триггер паразитного
            // C_PORT_CONNECTION даже без физического устройства (settle
            // переходного процесса на D+/D-, см. USB 2.0 spec 11.11).
            // Раньше для "ничего не подключено" change-биты НЕ чистились —
            // паразитный C_PORT_CONNECTION оставался висеть и хаб report'ил
            // бы его ПОЗЖЕ через interrupt-эндпоинт, как будто это новое
            // событие (см. poll_hub_interrupts() — не различает "новое
            // событие" от "старый непрочищенный change-бит").
            if (change & USB_PORT_STAT_CONNECTION) {
                hub_clear_port_feature(console_ep, idx, slot_id, hp, USB_PORT_FEAT_C_PORT_CONNECTION);
            }
            continue; // ничего не подключено
        }
        hub_handle_port_connect(console_ep, idx, hp);
    }
}

// Milestone B4 (Фаза 15) — находит устройство ЗА хабом (idx в
// g_usb_devices[]) по (hub_idx, hub_port) — нужно и на отключение
// конкретного downstream-порта, и на каскадное отключение ВСЕХ детей,
// когда хаб уходит целиком (см. poll_ports_for_hotplug()).
static int find_device_behind_hub(int hub_idx, uint8_t hub_port) {
    for (int i = 0; i < USB_MAX_DEVICES; i++) {
        if (g_usb_devices[i].in_use && g_usb_devices[i].behind_hub &&
            g_usb_devices[i].parent_hub_idx == hub_idx && g_usb_devices[i].parent_port_number == hub_port) {
            return i;
        }
    }
    return -1;
}

// Milestone B4 (Фаза 15) — отключение НА КОНКРЕТНОМ downstream-порту
// хаба (не сам хаб). Если на этом порту ничего не было смонтировано
// (например, был FS/LS и честно пропущен — см. hub_handle_port_connect())
// — молча ничего не делаем, отключать нечего.
static void hub_handle_port_disconnect(seL4_CPtr console_ep, int hub_idx, uint8_t hub_port) {
    int idx = find_device_behind_hub(hub_idx, hub_port);
    if (idx < 0) return;
    char old_name[32];
    my_strcpy(old_name, g_usb_devices[idx].volume_name);
    sys_puts(console_ep, "[USB]   Обнаружено отключение устройства за хабом — размонтирую /mnt/");
    sys_puts(console_ep, old_name);
    sys_puts(console_ep, "\n");
    unmount_usb_storage(console_ep, idx);
    sys_puts(console_ep, "[USB]   Точка монтирования удалена.\n");
}

// Milestone B4 (Фаза 15) — динамический опрос interrupt-эндпоинтов ВСЕХ
// смонтированных хабов, раз в heartbeat-тик (тот же каданс, что
// poll_ports_for_hotplug(), см. main()). НЕ блокируясь (try_check_transfer_complete)
// проверяет "слушающую" TRB (см. hub_enqueue_interrupt_listen()); если
// пришла — читает битовую карту статуса из bounce-буфера хаба (бит0 =
// сам хаб, биты 1..N = downstream-порты, см. USB 2.0 spec 11.12.4),
// для каждого установленного бита порта — GET_PORT_STATUS (текущее
// состояние решает: подключение или отключение), обрабатывает, чистит
// C_PORT_CONNECTION, и в САМОМ КОНЦЕ перевооружает эндпоинт заново
// (одна новая слушающая TRB) — без этого второе изменение порта
// никогда не заметили бы.
static void poll_hub_interrupts(seL4_CPtr console_ep) {
    // issuse.txt №15 — НАЙДЕНО НА ЖИВОМ ЖЕЛЕЗЕ: тот же класс бага, что уже
    // чинили в AsyncCtrl (Data+Status stage теряли друг друга). Раньше
    // try_check_transfer_complete() звался ОТДЕЛЬНО на каждый хаб в
    // цикле — а эта функция дренирует ВЕСЬ event ring за один вызов и
    // матчит только ОДИН целевой адрес, молча выбрасывая всё остальное.
    // Если события ДВУХ хабов (например, встроенного root-hub'а VL805 и
    // хаба пользователя, висящего у него на порту) оказывались в кольце
    // ОДНОВРЕМЕННО — первый по индексу хаб дренировал кольцо и МОЛЧА
    // терял событие второго, пока искал только своё. На живом тесте это
    // проявилось как "нулевая реакция" на устройство, воткнутое в хаб
    // пользователя, сразу после того, как встроенный VL805-хаб получил
    // СВОЁ собственное событие (апстрим-линк хаба пользователя мигнул в
    // момент вставки) — событие хаба пользователя терялось безвозвратно.
    // Фикс — ОДИН проход по кольцу за тик, матчим СРАЗУ все армированные
    // hub_int_pending_trb, обрабатываем результаты уже ПОСЛЕ дренажа.
    uint8_t hub_cc[USB_MAX_DEVICES];
    uint32_t hub_residual[USB_MAX_DEVICES];
    bool hub_completed[USB_MAX_DEVICES];
    bool any_target = false;
    for (int i = 0; i < USB_MAX_DEVICES; i++) {
        hub_completed[i] = false;
        UsbDeviceSlot &d = g_usb_devices[i];
        if (d.in_use && d.found.device_class == USB_CLASS_HUB && d.hub_int_dci != 0 && d.hub_int_pending_trb != 0) any_target = true;
    }
    if (any_target) {
        Trb ev;
        int drained = 0;
        while (dequeue_event_trb(ev)) {
            update_erdp();
            uint32_t type = (ev.control & TRB_TYPE_MASK) >> TRB_TYPE_SHIFT;
            if (type != TRB_TYPE_TRANSFER_EVENT) continue;
            for (int i = 0; i < USB_MAX_DEVICES; i++) {
                UsbDeviceSlot &d = g_usb_devices[i];
                if (!d.in_use || d.found.device_class != USB_CLASS_HUB || d.hub_int_dci == 0) continue;
                if (ev.parameter == d.hub_int_pending_trb) {
                    hub_cc[i] = (uint8_t)(ev.status >> 24);
                    hub_residual[i] = ev.status & 0xFFFFFFu;
                    hub_completed[i] = true;
                    break;
                }
            }
            if (++drained >= EVT_RING_DRAIN_SANITY_CAP) {
                sys_puts(console_ep, "[USB]   ОШИБКА: дренаж event ring (poll_hub_interrupts) превысил защитный потолок — обрываю.\n");
                break;
            }
        }
    }
    for (int idx = 0; idx < USB_MAX_DEVICES; idx++) {
        UsbDeviceSlot &dev = g_usb_devices[idx];
        if (!dev.in_use || dev.found.device_class != USB_CLASS_HUB || dev.hub_int_dci == 0) continue;
        if (!hub_completed[idx]) continue;
        uint8_t cc = hub_cc[idx]; uint32_t residual = hub_residual[idx];
        if (cc != 1 && cc != 13) { // 13 = Short Packet, не ошибка (см. bulk_transfer)
            sys_puthex32(console_ep, "[USB]   ПРЕДУПРЕЖДЕНИЕ: interrupt-передача хаба завершилась с кодом ", cc);
            hub_enqueue_interrupt_listen(idx); // перевооружаем в любом случае — иначе эндпоинт замолчит навсегда
            continue;
        }
        volatile uint8_t *bitmap = bounce_vaddr(idx);
        uint32_t mps = dev.hub_int_ep_mps ? dev.hub_int_ep_mps : 1;
        // issuse.txt №15 — НАЙДЕНО ЧТЕНИЕМ КОДА (ждёт hw-подтверждения):
        // cc==13 (Short Packet) означает "пришло МЕНЬШЕ, чем mps байт", а не
        // "пришло 0 байт" — но раньше это тоже принималось как "пришло 0
        // байт". Настоящий actual = mps - residual; если он 0, xHC не
        // записал в bounce-буфер ВООБЩЕ НИЧЕГО — читать его как битмап
        // означает читать неинициализированную/старую DMA-страницу.
        // ПОДОЗРЕНИЕ: именно так объясняется "флешка за хабом определилась
        // как хаб за хабом" при пустом хабе — первое вооружение interrupt-
        // эндпоинта после Configure Endpoint у некоторых хабов завершается
        // 0-байтным Short Packet'ом ещё до реального события на порту.
        uint32_t actual_bytes = (residual <= mps) ? (mps - residual) : 0;
        sys_puthex32(console_ep, "[USB]   DIAG hub interrupt: mps=", mps);
        sys_puthex32(console_ep, "[USB]   DIAG hub interrupt: residual=", residual);
        sys_puthex32(console_ep, "[USB]   DIAG hub interrupt: actual_bytes=", actual_bytes);
        sys_puthex32(console_ep, "[USB]   DIAG hub interrupt: bitmap[0]=", bitmap[0]);
        if (actual_bytes == 0) {
            sys_puts(console_ep, "[USB]   DIAG hub interrupt: 0 реальных байт — битмап НЕ доверяем, пропускаем.\n");
            hub_enqueue_interrupt_listen(idx);
            continue;
        }
        for (uint8_t hp = 1; hp <= dev.hub_num_ports; hp++) {
            uint32_t byte_idx = (uint32_t)hp / 8, bit_idx = (uint32_t)hp % 8;
            if (byte_idx >= mps) break; // защита от битой/усечённой карты
            if (byte_idx >= actual_bytes) break; // за пределами реально пришедших байт
            if (!((bitmap[byte_idx] >> bit_idx) & 1u)) continue; // этот порт не менялся
            uint16_t status = 0, change = 0;
            if (!hub_get_port_status(console_ep, idx, dev.slot_id, hp, status, change)) {
                sys_puthex32(console_ep, "[USB]   ОШИБКА (хаб): GET_PORT_STATUS порта ", hp);
                continue;
            }
            sys_puthex32(console_ep, "[USB]   DIAG живой wPortStatus порта = ", status);
            sys_puthex32(console_ep, "[USB]   DIAG живой wPortChange порта = ", change);
            hub_clear_port_feature(console_ep, idx, dev.slot_id, hp, USB_PORT_FEAT_C_PORT_CONNECTION);
            // issuse.txt №15 — см. комментарий у USB_PORT_FEAT_C_PORT_LINK_STATE:
            // без этого хаб бесконечно репортит один и тот же C_PORT_LINK_STATE
            // (например, после отключения SS-устройства) на каждом опросе.
            if (change & USB_PORT_STAT_C_LINK_STATE) {
                hub_clear_port_feature(console_ep, idx, dev.slot_id, hp, USB_PORT_FEAT_C_PORT_LINK_STATE);
            }
            // issuse.txt №15 — НАЙДЕНО НА ЖИВОМ ЖЕЛЕЗЕ: раньше решение
            // "подключение или отключение" принималось ТОЛЬКО по текущему
            // status.CONNECTION — но событие могло быть чисто Link-State
            // (мигнул линк уже смонтированного устройства, change.CONNECTION
            // не установлен, status.CONNECTION всё ещё 1, потому что кабель
            // никуда не делся). Раньше это ошибочно трактовалось как "новое
            // устройство" — пере-перечисление уже смонтированного слота,
            // тратящее ещё один g_usb_devices[]-слот впустую и в итоге
            // приводящее к "нет свободных слотов". Реагируем ТОЛЬКО если
            // change.CONNECTION реально установлен — сам факт смены статуса
            // подключения, а не любое шевеление на порту.
            if (change & USB_PORT_STAT_CONNECTION) {
                if (status & USB_PORT_STAT_CONNECTION) {
                    sys_puthex32(console_ep, "[USB] Хаб: обнаружено подключение (опрос interrupt-эндпоинта), порт = ", hp);
                    // issuse.txt №15 — асинхронный, тик-возобновляемый путь
                    // (см. hub_conn_async_start()/hub_conn_async_tick() выше)
                    // вместо синхронного hub_handle_port_connect(), который
                    // держал бы весь usb_driver колом на секунды при
                    // нестабильном/медленном нижестоящем порту.
                    hub_conn_async_start(console_ep, idx, hp);
                } else {
                    hub_handle_port_disconnect(console_ep, idx, hp);
                }
            }
        }
        hub_enqueue_interrupt_listen(idx); // перевооружаем — ждём следующее изменение
    }
}

// Milestone 4 (закрытие Фазы 14, см. ROADMAP.md/план) — Configure Endpoint:
// активирует bulk OUT/IN эндпоинты (найдены в Шаге 9) — команда xHCI,
// ничего не уходит на шину, контроллер просто начинает принимать доорбеллы
// для этих DCI. DCI = 2*номер_эндпоинта + (1 если IN иначе 0), см. xHCI
// 4.5.1 — вычисляется из bEndpointAddress, НЕ жёстко фиксирован (варьируется
// по устройству/вендору). bulk_out_dci/bulk_in_dci (переиспользуются
// каждой bulk-передачей, см. Milestone 5) теперь поля g_usb_devices[idx].
static bool step10_configure_endpoints(seL4_CPtr console_ep, int idx, uint8_t slot_id, int port, uint32_t port_speed) {
    if (LOG_USB) sys_puts(console_ep, "[USB] Шаг 10: Configure Endpoint.\n");

    UsbDeviceSlot &dev = g_usb_devices[idx];
    uint8_t out_epnum = dev.bulk_eps.bulk_out_addr & 0x0Fu;
    uint8_t in_epnum  = dev.bulk_eps.bulk_in_addr & 0x0Fu;
    uint8_t out_dci = (uint8_t)(2u * out_epnum + 0u); // OUT
    uint8_t in_dci  = (uint8_t)(2u * in_epnum + 1u);  // IN
    if (LOG_USB) sys_puthex32(console_ep, "[USB]   DCI bulk OUT = ", out_dci);
    if (LOG_USB) sys_puthex32(console_ep, "[USB]   DCI bulk IN  = ", in_dci);
    uint8_t max_dci = out_dci > in_dci ? out_dci : in_dci;
    dev.bulk_out_dci = out_dci; // для доорбеллов будущих bulk-передач (Milestone 5)
    dev.bulk_in_dci = in_dci;

    // Input Control Context (ctx_index=0): Drop=0, Add = A0 (Slot,
    // обязателен при изменении Context Entries) | A(out_dci) | A(in_dci).
    // EP0 (A1) НЕ трогаем — не во флагах, контроллер его текущее
    // состояние не изменит.
    for (int d = 0; d < g_ctx_size / 4; d++) write_ctx_dword(inputctx(), 0, d, 0);
    write_ctx_dword(inputctx(), 0, 1, (1u << 0) | (1u << out_dci) | (1u << in_dci));

    // Slot Context (ctx_index=1) — ПОЛНОСТЬЮ переписывается заново (раз A0
    // установлен), поэтому нужны ВСЕ поля, не только Context Entries —
    // иначе затрём Speed/Root Hub Port Number нулями (те же поля, что и
    // Шаг 7 писал для Address Device).
    for (int d = 0; d < g_ctx_size / 4; d++) write_ctx_dword(inputctx(), 1, d, 0);
    // Route String — см. подробный комментарий в step7_address_device()
    // (Milestone B3, живая находка) — та же логика, обязана совпадать с
    // тем, что было записано на Address Device, иначе Parameter Error.
    uint32_t route_string = dev.behind_hub ? dev.route_string_full : 0;
    write_ctx_dword(inputctx(), 1, 0, route_string | ((uint32_t)max_dci << 27) | ((port_speed & 0xF) << 20) | (dev.behind_hub && dev.parent_multi_tt ? (1u << 25) : 0));
    write_ctx_dword(inputctx(), 1, 1, ((uint32_t)port << 16));
    // dword2 (Parent Hub Slot ID/Port/TT Think Time) — НАЙДЕНО НА ЖИВОМ
    // ЖЕЛЕЗЕ (Milestone B3, см. тот же комментарий в step7_address_device()):
    // эти поля — TT-ассоциация, нужна только для FS/LS устройств за
    // хабом (не наш случай, только High-Speed) — остаётся 0, как и у
    // корневых устройств, дальше не пишем.

    // EP Context'ы bulk-эндпоинтов (ctx_index = DCI+1, тот же приём, что
    // EP0 в Шаге 7). EP Type: 2=Bulk OUT, 6=Bulk IN (см. xHCI Таблица 6.9).
    // CErr=3 — та же конвенция, что EP0 (Двадцать восьмая попытка).
    // Average TRB Length (dword4) — не нулевой, разумное значение —
    // MaxPacketSize (некоторые реализации ведут себя непредсказуемо при
    // первой передаче, если это поле осталось нулём).
    init_trb_ring(dev.bulkout_ring, bulkoutring_vaddr(idx), dev.bulkout_trring_paddr);
    init_trb_ring(dev.bulkin_ring, bulkinring_vaddr(idx), dev.bulkin_trring_paddr);
    auto write_bulk_ep = [&](uint8_t dci, uint32_t ep_type, uint16_t mps, uint8_t max_burst, uint64_t trring_dev_base) {
        int ctx_index = (int)dci + 1;
        for (int d = 0; d < g_ctx_size / 4; d++) write_ctx_dword(inputctx(), ctx_index, d, 0);
        write_ctx_dword(inputctx(), ctx_index, 1,
                         (3u << 1) /*CErr*/ | (ep_type << 3) | ((uint32_t)max_burst << 8) | ((uint32_t)mps << 16));
        write_ctx_dword(inputctx(), ctx_index, 2, (uint32_t)(trring_dev_base & 0xFFFFFFFFu) | 1u /*DCS*/);
        write_ctx_dword(inputctx(), ctx_index, 3, (uint32_t)(trring_dev_base >> 32));
        write_ctx_dword(inputctx(), ctx_index, 4, mps); // Average TRB Length
    };
    write_bulk_ep(out_dci, 2 /*Bulk Out*/, dev.bulk_eps.bulk_out_mps, dev.bulk_eps.bulk_out_max_burst, dev.bulkout_ring.dev_base);
    write_bulk_ep(in_dci, 6 /*Bulk In*/, dev.bulk_eps.bulk_in_mps, dev.bulk_eps.bulk_in_max_burst, dev.bulkin_ring.dev_base);

    uint64_t cmd_paddr = enqueue_command_trb(to_dev_addr(g_inputctx_paddr), 0,
                                              trb_type(TRB_TYPE_CONFIGURE_ENDPOINT_CMD) | ((uint32_t)slot_id << 24));
    uint8_t completion_code = 0, ret_slot = 0;
    if (!wait_command_completion(console_ep, cmd_paddr, 500, completion_code, ret_slot)) {
        sys_puts(console_ep, "[USB] ОШИБКА: Configure Endpoint не завершился за 500мс.\n");
        return false;
    }
    if (completion_code != 1) {
        sys_puthex32(console_ep, "[USB] ОШИБКА: Configure Endpoint завершился с кодом ", completion_code);
        return false;
    }
    if (LOG_USB) sys_puts(console_ep, "[USB]   Bulk-эндпоинты активированы.\n");
    return true;
}

// Шаг 11 — SET_CONFIGURATION: control-transfer без Data Stage,
// wValue=bConfigurationValue (захвачен в Шаге 9).
static bool step11_set_configuration(seL4_CPtr console_ep, int idx, uint8_t slot_id) {
    if (LOG_USB) sys_puts(console_ep, "[USB] Шаг 11: SET_CONFIGURATION.\n");
    if (LOG_USB) sys_puthex32(console_ep, "[USB]   bConfigurationValue = ", g_usb_devices[idx].bulk_eps.config_value);
    // bmRequestType=0x00 (Host-to-Device|Standard|Device), bRequest=0x09.
    if (!ep0_control_no_data(console_ep, g_usb_devices[idx].ep0_ring, slot_id, 0x00, 0x09, g_usb_devices[idx].bulk_eps.config_value, 0)) {
        sys_puts(console_ep, "[USB] ОШИБКА: SET_CONFIGURATION не удался.\n");
        return false;
    }
    if (LOG_USB) sys_puts(console_ep, "[USB]   Устройство сконфигурировано, bulk-эндпоинты готовы к передачам.\n");
    return true;
}

// Фаза 8 (df) — восстановление bulk-эндпоинта после ошибки/таймаута
// передачи. НАЙДЕНО НА ЖИВОМ ЖЕЛЕЗЕ: без этого после ПЕРВОГО же таймаута/
// ошибки ВСЕ последующие bulk-передачи на этот эндпоинт (даже отправка
// самого CBW) немедленно проваливались — устройство оставалось
// зависшим до перезагрузки, ни один следующий df не восстанавливался
// сам. Reset Endpoint (xHCI 6.4.3.9) снимает Halted-состояние
// эндпоинта; Set TR Dequeue Pointer (6.4.3.10), СРАЗУ следом — переводит
// аппаратный dequeue на ТЕКУЩУЮ программную позицию нашего кольца
// (ring.enqueue_idx/pcs), иначе контроллер продолжит ждать TRB там, где
// застрял. Best-effort: даже если сама команда вернёт не-успешный код
// (например, эндпоинт и не был реально Halted) — не фатально, исходная
// передача уже провалена в любом случае, хуже не станет.
static void recover_bulk_endpoint(seL4_CPtr console_ep, uint8_t slot_id, uint8_t dci, TrbRing &ring) {
    // Живой Device Context ДО восстановления — dword0 бит[2:0] = Endpoint
    // State (0=Disabled,1=Running,2=Halted,3=Stopped,4=Error). В Device
    // Context (в отличие от Input Context) префикса Input Control Context
    // нет — индекс контекста эндпоинта РАВЕН его DCI напрямую (Slot=0).
    // НЕ гейтим LOG_USB — первая попытка восстановления не сработала на
    // живом железе (см. ROADMAP.md/issuse.txt), это диагностический проход.
    sys_puthex32(console_ep, "[USB]   DIAG восстановление: EP dword0 ДО reset = ", read_ctx_dword(devctx_vaddr_for(slot_id), dci, 0));
    // USBSTS на таймауте показывал PCD=1 (Port Change Detect) — читаем
    // живой PORTSC КОРНЕВОГО порта этого устройства, чтобы понять,
    // реальный ли это дисконнект/link-событие, а не просто залипший,
    // никогда не сбрасываемый RW1C-бит с более раннего события. Slot
    // Context dword1 биты[23:16] (см. step7_address_device()/write_ctx_dword
    // (inputctx(), 1, 1, (port << 16)) — НЕ биты[15:8], это исправление
    // предыдущей попытки: там был байт из Max Exit Latency, не порт).
    {
        uint32_t root_port = (read_ctx_dword(devctx_vaddr_for(slot_id), 0, 1) >> 16) & 0xFFu;
        if (root_port >= 1) {
            uint32_t portsc = *reg32(g_op_base, XHCI_OP_PORTSC_BASE + (uintptr_t)(root_port - 1) * 0x10);
            sys_puthex32(console_ep, "[USB]   DIAG восстановление: корневой порт = ", root_port);
            sys_puthex32(console_ep, "[USB]   DIAG восстановление: PORTSC этого порта = ", portsc);
        }
    }

    uint64_t cmd_paddr = enqueue_command_trb(0, 0,
        trb_type(TRB_TYPE_RESET_ENDPOINT_CMD) | ((uint32_t)dci << 16) | ((uint32_t)slot_id << 24));
    uint8_t cc = 0, ret_slot = 0;
    if (!wait_command_completion(console_ep, cmd_paddr, 500, cc, ret_slot)) {
        sys_puts(console_ep, "[USB]   DIAG восстановление: Reset Endpoint не завершился за 500мс.\n");
        return;
    }
    sys_puthex32(console_ep, "[USB]   DIAG восстановление: Reset Endpoint код завершения = ", cc);
    sys_puthex32(console_ep, "[USB]   DIAG восстановление: EP dword0 после Reset Endpoint = ", read_ctx_dword(devctx_vaddr_for(slot_id), dci, 0));

    uint64_t deq_addr = ring.dev_base + (uint64_t)ring.enqueue_idx * 16;
    uint64_t param = (deq_addr & ~0xFull) | (ring.pcs & 1u); // DCS в бите 0, SCT=0 (без streams)
    uint64_t cmd2_paddr = enqueue_command_trb(param, 0,
        trb_type(TRB_TYPE_SET_TR_DEQUEUE_CMD) | ((uint32_t)dci << 16) | ((uint32_t)slot_id << 24));
    if (!wait_command_completion(console_ep, cmd2_paddr, 500, cc, ret_slot)) {
        sys_puts(console_ep, "[USB]   DIAG восстановление: Set TR Dequeue Pointer не завершился за 500мс.\n");
        return;
    }
    sys_puthex32(console_ep, "[USB]   DIAG восстановление: Set TR Dequeue Pointer код завершения = ", cc);
    sys_puthex32(console_ep, "[USB]   DIAG восстановление: EP dword0 после Set TR Dequeue = ", read_ctx_dword(devctx_vaddr_for(slot_id), dci, 0));
}

// Milestone 5 (закрытие Фазы 14, см. ROADMAP.md/план) — одна bulk-передача
// (Normal TRB) + ожидание её Transfer Event. ring — bulkout_ring или
// bulkin_ring вызывающего устройства (g_usb_devices[idx].*), dci —
// соответствующий доорбелл-таргет (bulk_out_dci/bulk_in_dci, вычислены в
// Шаге 10). ISP — тот же приём, что Data Stage у control-transfer'ов:
// получаем событие даже на short packet.
static bool bulk_transfer(seL4_CPtr console_ep, uint8_t slot_id, TrbRing &ring, uint8_t dci,
                           uint64_t buffer_paddr, uint32_t length, uint32_t &actual_length) {
    uint64_t trb_addr = ring_enqueue_trb(ring, to_dev_addr(buffer_paddr), length,
                                          trb_type(TRB_TYPE_NORMAL) | (1u << 2) /*ISP*/ | TRB_IOC);
    ring_endpoint_doorbell(slot_id, dci);

    uint8_t cc = 0;
    uint32_t residual = 0;
    if (!wait_transfer_completion(console_ep, trb_addr, 1000, cc, residual)) {
        sys_puts(console_ep, "[USB] ОШИБКА: bulk-передача не завершилась за 1с.\n");
        recover_bulk_endpoint(console_ep, slot_id, dci, ring);
        return false;
    }
    if (cc != 1 && cc != 13) { // 13 = Short Packet, не ошибка
        sys_puthex32(console_ep, "[USB] ОШИБКА: bulk-передача завершилась с кодом ", cc);
        recover_bulk_endpoint(console_ep, slot_id, dci, ring);
        return false;
    }
    actual_length = length - residual;
    return true;
}

constexpr uint32_t BOT_CBW_SIGNATURE = 0x43425355u; // "USBC"
constexpr uint32_t BOT_CSW_SIGNATURE = 0x53425355u; // "USBS"
static uint32_t g_cbw_tag = 1; // инкрементируется на каждую команду, сверяется с dCSWTag

// issuse.txt №14 — Bulk-Only Mass Storage Reset (class-специфичный запрос,
// bmRequestType=0x21 Host-to-Device|Class|Interface, bRequest=0xFF, без
// данных, см. "USB Mass Storage Class — Bulk-Only Transport" spec, §3.1)
// + Clear Feature(ENDPOINT_HALT) на обоих bulk-эндпоинтах (bmRequestType=
// 0x02 Host-to-Device|Standard|Endpoint, bRequest=0x01, wValue=0
// ENDPOINT_HALT, wIndex=адрес эндпоинта, см. USB 2.0 spec 9.4.1/Table 9-6).
// По спеке BOT это единственный штатный способ ресинхронизировать
// состояние устройства после CSW status=2 (Phase Error) — xHCI-уровневый
// recover_bulk_endpoint() (см. выше) чинит только зависшее TRB-кольцо
// контроллера, но не знает о протоколе BOT самого устройства. Best-effort:
// вызывается уже на заведомо ошибочном пути (Phase Error), поэтому если
// сам reset тоже не удастся — хуже не станет, просто честно логируем.
static void bot_reset_recovery(seL4_CPtr console_ep, int idx, uint8_t slot_id) {
    UsbDeviceSlot &dev = g_usb_devices[idx];
    sys_puts(console_ep, "[USB]   CSW Phase Error — выполняю Bulk-Only Mass Storage Reset.\n");
    if (!ep0_control_no_data(console_ep, dev.ep0_ring, slot_id, 0x21, 0xFF, 0, dev.bulk_eps.interface_num)) {
        sys_puts(console_ep, "[USB]   ОШИБКА: Bulk-Only Mass Storage Reset не удался.\n");
    }
    if (!ep0_control_no_data(console_ep, dev.ep0_ring, slot_id, 0x02, 0x01, 0, dev.bulk_eps.bulk_out_addr)) {
        sys_puts(console_ep, "[USB]   ОШИБКА: Clear Feature(HALT) на bulk OUT не удался.\n");
    }
    if (!ep0_control_no_data(console_ep, dev.ep0_ring, slot_id, 0x02, 0x01, 0, dev.bulk_eps.bulk_in_addr)) {
        sys_puts(console_ep, "[USB]   ОШИБКА: Clear Feature(HALT) на bulk IN не удался.\n");
    }
}

// Milestone 5 — один SCSI-обмен через Bulk-Only Transport: CBW (31 байт,
// bulk OUT) -> опциональная Data-стадия (bulk IN/OUT, направление данных,
// НЕ CBW/CSW — у тех направление фиксировано спекой BOT) -> CSW (13 байт,
// bulk IN). CBW/CSW в ОДНОЙ странице (см. PLAT_XHCI_CBW_CSW_VADDR) —
// CBW на offset 0, CSW на offset 64.
static bool scsi_command(seL4_CPtr console_ep, int idx, uint8_t slot_id, const uint8_t *cdb, uint8_t cdb_len,
                          bool data_dir_in, uint64_t data_paddr, uint32_t data_len,
                          uint32_t &actual_data_len, uint8_t &csw_status) {
    UsbDeviceSlot &dev = g_usb_devices[idx];
    volatile uint8_t *cbw = cbw_vaddr(idx);
    volatile uint8_t *csw = csw_vaddr(idx);
    uint32_t tag = g_cbw_tag++;

    for (int i = 0; i < 31; i++) cbw[i] = 0;
    cbw[0] = (uint8_t)BOT_CBW_SIGNATURE; cbw[1] = (uint8_t)(BOT_CBW_SIGNATURE >> 8);
    cbw[2] = (uint8_t)(BOT_CBW_SIGNATURE >> 16); cbw[3] = (uint8_t)(BOT_CBW_SIGNATURE >> 24);
    cbw[4] = (uint8_t)tag; cbw[5] = (uint8_t)(tag >> 8); cbw[6] = (uint8_t)(tag >> 16); cbw[7] = (uint8_t)(tag >> 24);
    cbw[8] = (uint8_t)data_len; cbw[9] = (uint8_t)(data_len >> 8);
    cbw[10] = (uint8_t)(data_len >> 16); cbw[11] = (uint8_t)(data_len >> 24);
    cbw[12] = data_dir_in ? 0x80u : 0x00u; // bmCBWFlags
    cbw[13] = 0;                            // bCBWLUN — единственный LUN этой фазы
    cbw[14] = cdb_len & 0x1Fu;              // bCBWCBLength
    for (int i = 0; i < cdb_len && i < 16; i++) cbw[15 + i] = cdb[i];

    uint32_t actual = 0;
    if (!bulk_transfer(console_ep, slot_id, dev.bulkout_ring, dev.bulk_out_dci, dev.cbw_csw_paddr, 31, actual)) {
        sys_puts(console_ep, "[USB] ОШИБКА: отправка CBW не удалась.\n");
        return false;
    }

    actual_data_len = 0;
    if (data_len > 0) {
        bool ok = data_dir_in
            ? bulk_transfer(console_ep, slot_id, dev.bulkin_ring, dev.bulk_in_dci, data_paddr, data_len, actual_data_len)
            : bulk_transfer(console_ep, slot_id, dev.bulkout_ring, dev.bulk_out_dci, data_paddr, data_len, actual_data_len);
        if (!ok) {
            sys_puts(console_ep, "[USB] ОШИБКА: Data-стадия SCSI-команды не удалась.\n");
            return false;
        }
    }

    uint32_t csw_actual = 0;
    if (!bulk_transfer(console_ep, slot_id, dev.bulkin_ring, dev.bulk_in_dci, dev.cbw_csw_paddr + 64, 13, csw_actual)) {
        sys_puts(console_ep, "[USB] ОШИБКА: приём CSW не удался.\n");
        return false;
    }
    uint32_t csw_sig = (uint32_t)csw[0] | ((uint32_t)csw[1] << 8) | ((uint32_t)csw[2] << 16) | ((uint32_t)csw[3] << 24);
    uint32_t csw_tag = (uint32_t)csw[4] | ((uint32_t)csw[5] << 8) | ((uint32_t)csw[6] << 16) | ((uint32_t)csw[7] << 24);
    csw_status = csw[12];
    if (csw_sig != BOT_CSW_SIGNATURE) {
        sys_puthex32(console_ep, "[USB] ОШИБКА: CSW сигнатура неверна: ", csw_sig);
        return false;
    }
    if (csw_tag != tag) {
        // issuse.txt №13: раньше это было только предупреждением — csw_status
        // из ЧУЖОГО (несовпадающего tag) CSW всё равно применялся к текущей
        // команде. Несовпадение tag означает, что этому CSW вообще нельзя
        // доверять (например хвостовой CSW предыдущей команды после
        // таймаута/восстановления bulk-эндпоинта) — честная ошибка
        // транспорта, а не просто диагностика.
        sys_puts(console_ep, "[USB]   ОШИБКА: CSW tag не совпадает с CBW (устройство перепутало ответы?).\n");
        return false;
    }
    // issuse.txt №14 — csw_status=2 (Phase Error) требует по спеке BOT
    // полного reset-восстановления, иначе состояние устройства остаётся
    // рассинхронизированным для ВСЕХ последующих команд, не только этой.
    // Транспорт этого вызова формально успешен (CSW честно получен) —
    // csw_status=2 возвращается вызывающему как есть (уже трактуется им
    // как ошибка), reset — побочный эффект для БУДУЩИХ команд.
    if (csw_status == 2) bot_reset_recovery(console_ep, idx, slot_id);
    return true;
}

// Шаг 12 — первая настоящая BOT/SCSI-транзакция: INQUIRY (6-байтный CDB,
// opcode 0x12). Первое доказательство, что bulk-данные реально ходят через
// устройство (не просто настройка регистров, как Шаги 1-11) — печатает
// vendor/product строки из ответа.
static bool step12_inquiry(seL4_CPtr console_ep, int idx, uint8_t slot_id) {
    if (LOG_USB) sys_puts(console_ep, "[USB] Шаг 12: SCSI INQUIRY.\n");
    uint8_t cdb[16] = {0};
    cdb[0] = 0x12; // INQUIRY
    cdb[4] = 36;   // Allocation Length

    volatile uint8_t *data = bounce_vaddr(idx);
    for (int i = 0; i < 36; i++) data[i] = 0;

    uint32_t actual_data_len = 0;
    uint8_t csw_status = 0xFFu;
    if (!scsi_command(console_ep, idx, slot_id, cdb, 6, /*data_dir_in=*/true, g_usb_devices[idx].bounce_paddr, 36, actual_data_len, csw_status)) {
        sys_puts(console_ep, "[USB] ОШИБКА: INQUIRY не удался (см. лог выше).\n");
        return false;
    }
    if (LOG_USB) sys_puthex32(console_ep, "[USB]   CSW status = ", csw_status);
    if (LOG_USB) sys_puthex32(console_ep, "[USB]   получено байт данных: ", actual_data_len);
    if (csw_status != 0) {
        sys_puts(console_ep, "[USB] ОШИБКА: INQUIRY завершился с ненулевым CSW status.\n");
        return false;
    }
    // INQUIRY-ответ (см. SCSI спецификацию): offset 8-15 = Vendor
    // Identification (8 ASCII байт), 16-31 = Product Identification (16
    // ASCII байт). Пробельно-дополненные, не гарантированно с нулевым
    // байтом — копируем в локальный буфер и сами ставим терминатор.
    char vendor[9]; for (int i = 0; i < 8; i++) vendor[i] = (char)data[8 + i]; vendor[8] = 0;
    char product[17]; for (int i = 0; i < 16; i++) product[i] = (char)data[16 + i]; product[16] = 0;
    if (LOG_USB) sys_puts(console_ep, "[USB]   Vendor:  "); if (LOG_USB) sys_puts(console_ep, vendor); if (LOG_USB) sys_puts(console_ep, "\n");
    if (LOG_USB) sys_puts(console_ep, "[USB]   Product: "); if (LOG_USB) sys_puts(console_ep, product); if (LOG_USB) sys_puts(console_ep, "\n");
    update_usb_volume_name(idx, vendor, product); // Milestone 10 (доп.) — имя точки монтирования = Vendor-Product
    return true;
}

// Шаг 13 (Milestone 6, закрытие Фазы 14, см. ROADMAP.md/план) — SCSI TEST
// UNIT READY (6-байтный CDB, opcode 0x00, без данных). Не все устройства
// готовы сразу после SET_CONFIGURATION — опрашиваем с ограниченным (не
// бесконечным) числом попыток, тот же принцип, что везде в этой фазе.
static bool step13_test_unit_ready(seL4_CPtr console_ep, int idx, uint8_t slot_id) {
    if (LOG_USB) sys_puts(console_ep, "[USB] Шаг 13: SCSI TEST UNIT READY.\n");
    uint8_t cdb[16] = {0};
    cdb[0] = 0x00; // TEST UNIT READY

    for (int attempt = 1; attempt <= 5; attempt++) {
        uint32_t actual = 0;
        uint8_t csw_status = 0xFFu;
        if (!scsi_command(console_ep, idx, slot_id, cdb, 6, /*data_dir_in=*/true, 0, 0, actual, csw_status)) {
            sys_puts(console_ep, "[USB] ОШИБКА: TEST UNIT READY — сбой транспорта (см. лог выше).\n");
            return false;
        }
        if (LOG_USB) sys_puthex32(console_ep, "[USB]   попытка, CSW status = ", csw_status);
        if (csw_status == 0) {
            if (LOG_USB) sys_puts(console_ep, "[USB]   Устройство готово.\n");
            return true;
        }
        uint64_t deadline = read_cntvct() + (uint64_t)200 * g_cntfrq / 1000;
        while (read_cntvct() < deadline) seL4_Yield();
    }
    sys_puts(console_ep, "[USB] ОШИБКА: устройство не стало готовым за 5 попыток.\n");
    return false;
}

// Milestone 7 монтировала exFAT только для чтения; Milestone 10 включает
// запись через SCSI WRITE(10) (см. hardware_usb_write() ниже) — тот же
// флаг, что RPI4_EMMC_ALLOW_WRITE в blk_driver.cpp (там всегда true,
// запись на SD-карту никогда не выключалась). Диспетчер (Milestone 8)
// перехватывает мутирующие VFS-команды ДО exfat.cpp, пока флаг false —
// см. комментарий в главном цикле ниже про находку "touch вешал шелл".
constexpr bool RPI4_USB_ALLOW_WRITE = true;
constexpr uint32_t USB_MAX_SECTORS_PER_IO = 8; // = размер bounce-буфера (PLAT_XHCI_BOUNCE_VADDR)/512, тот же бюджет, что hardware_emmc_read()

// hardware_usb_read/write вызываются exfat.cpp БЕЗ возможности передать ни
// console_ep, ни "какое устройство" (сигнатура block_read_fn/
// block_write_fn фиксирована в h/exfat.h, без параметра контекста) —
// console_ep (g_console_ep, объявлен в самом начале файла — см. Milestone
// B2 там же) остаётся глобалом, а "какое устройство" решается N тонкими
// функциями-обёртками ниже (см. hardware_usb_read_N/hardware_usb_write_N)
// — каждая жёстко зашивает свой индекс и делегирует общей
// hardware_usb_rw_generic(idx, ...). Остальное per-device состояние
// (slot_id/partition_start_sector/fs/mounted/port) — теперь поля
// g_usb_devices[idx] (см. struct UsbDeviceSlot выше).

// Milestone 8 — собственный VFS-диспетчер usb_driver'а, зеркалит
// g_shm_vaddr/BLK_SHM_STAGING_OFFSET у blk_driver.cpp. USB — опциональный
// модуль (см. SYS_DRIVER_READY в main.cpp) — если SYS_SHM_GET не выдаст
// страницы (case 6 в shm_pages_mask_for_role() почему-то не сработал),
// g_shm_vaddr останется nullptr, и диспетчер ниже честно отвечает ошибкой
// вместо разыменования null.
static char *g_shm_vaddr = nullptr;

// Шаг 14 — SCSI READ CAPACITY(10) (10-байтный CDB, opcode 0x25). Ответ
// (8 байт, см. SCSI спецификацию) — Last LBA + Block Size, ОБА
// BIG-ENDIAN — в отличие от USB-дескрипторов (little-endian), это другой
// протокол со своей разрядностью байт.
static bool step14_read_capacity(seL4_CPtr console_ep, int idx, uint8_t slot_id) {
    if (LOG_USB) sys_puts(console_ep, "[USB] Шаг 14: SCSI READ CAPACITY(10).\n");
    uint8_t cdb[16] = {0};
    cdb[0] = 0x25; // READ CAPACITY(10)

    volatile uint8_t *data = bounce_vaddr(idx);
    for (int i = 0; i < 8; i++) data[i] = 0;

    uint32_t actual = 0;
    uint8_t csw_status = 0xFFu;
    if (!scsi_command(console_ep, idx, slot_id, cdb, 10, /*data_dir_in=*/true, g_usb_devices[idx].bounce_paddr, 8, actual, csw_status)) {
        sys_puts(console_ep, "[USB] ОШИБКА: READ CAPACITY(10) — сбой транспорта (см. лог выше).\n");
        return false;
    }
    if (csw_status != 0) {
        sys_puthex32(console_ep, "[USB] ОШИБКА: READ CAPACITY(10) завершился с CSW status = ", csw_status);
        return false;
    }
    uint32_t last_lba = ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16) | ((uint32_t)data[2] << 8) | data[3];
    uint32_t block_size = ((uint32_t)data[4] << 24) | ((uint32_t)data[5] << 16) | ((uint32_t)data[6] << 8) | data[7];
    if (LOG_USB) sys_puthex32(console_ep, "[USB]   Last LBA = ", last_lba);
    if (LOG_USB) sys_puthex32(console_ep, "[USB]   Block Size = ", block_size);
    if (block_size != 512) {
        sys_puts(console_ep, "[USB] ОШИБКА: размер блока не 512 байт — не поддерживается в этой фазе (честное ограничение).\n");
        return false;
    }
    UsbCapacity &cap = g_usb_devices[idx].capacity;
    cap.found = true;
    cap.last_lba = last_lba;
    cap.block_size = block_size;
    uint32_t capacity_mb = (uint32_t)(((uint64_t)(last_lba + 1) * block_size) / (1024ull * 1024ull));
    if (LOG_USB) sys_puthex32(console_ep, "[USB]   Приблизительная ёмкость (МБ) = ", capacity_mb);
    return true;
}

// Milestone 7 — block_read_fn/block_write_fn для exFAT (см. h/exfat.h)
// поверх SCSI READ(10)/WRITE(10). Максимум USB_MAX_SECTORS_PER_IO секторов
// за вызов (размер bounce-буфера) — тот же бюджет/приём, что
// hardware_emmc_read/write() в blk_driver.cpp: один bounce-буфер, один
// my_memcpy в/из буфера вызывающего (может быть на стеке exfat.cpp),
// избегает риска кэш/DMA-алиасинга на некэшируемой Device-памяти.
static bool hardware_usb_rw_generic_read(int idx, uint32_t sector, uint32_t count, void* buffer) {
    if (count == 0 || count > USB_MAX_SECTORS_PER_IO) return false;
    UsbDeviceSlot &dev = g_usb_devices[idx];
    uint8_t cdb[16] = {0};
    cdb[0] = 0x28; // READ(10)
    uint32_t lba = dev.partition_start_sector + sector;
    cdb[2] = (uint8_t)(lba >> 24); cdb[3] = (uint8_t)(lba >> 16);
    cdb[4] = (uint8_t)(lba >> 8);  cdb[5] = (uint8_t)lba;
    cdb[7] = (uint8_t)(count >> 8); cdb[8] = (uint8_t)count; // Transfer Length, big-endian

    uint32_t actual = 0;
    uint8_t csw_status = 0xFFu;
    if (!scsi_command(g_console_ep, idx, dev.slot_id, cdb, 10, /*data_dir_in=*/true, dev.bounce_paddr,
                       count * 512u, actual, csw_status)) {
        return false;
    }
    if (csw_status != 0) return false;
    // issuse.txt №11: short packet (actual < запрошенного) означало бы, что
    // хвост bounce-буфера — не свежие данные с устройства, а мусор
    // предыдущего вызова; раньше это молча копировалось наружу.
    if (actual != count * 512u) return false;
    my_memcpy(buffer, (const void*)bounce_vaddr(idx), (int)(count * 512u));
    // issuse.txt №66 — "занят, но жив" для watchdog'а, см. комментарий у
    // g_usb_liveness_ntfn выше.
    if (g_usb_liveness_ntfn != 0) seL4_Signal(g_usb_liveness_ntfn);
    return true;
}

// Milestone 10 — зеркалит hardware_usb_rw_generic_read() (тот же bounce-
// буфер, тот же лимит USB_MAX_SECTORS_PER_IO за вызов), но SCSI WRITE(10)
// (opcode 0x2A) и data_dir_in=false — scsi_command() уже умеет OUT-
// направление (см. bulk_transfer на bulkout_ring, используется и для
// CBW/CDB), реализовывать его отдельно не нужно.
static bool hardware_usb_rw_generic_write(int idx, uint32_t sector, uint32_t count, const void* buffer) {
    if (!RPI4_USB_ALLOW_WRITE) return false;
    if (count == 0 || count > USB_MAX_SECTORS_PER_IO) return false;
    UsbDeviceSlot &dev = g_usb_devices[idx];
    my_memcpy((void*)bounce_vaddr(idx), buffer, (int)(count * 512u));

    uint8_t cdb[16] = {0};
    cdb[0] = 0x2A; // WRITE(10)
    uint32_t lba = dev.partition_start_sector + sector;
    cdb[2] = (uint8_t)(lba >> 24); cdb[3] = (uint8_t)(lba >> 16);
    cdb[4] = (uint8_t)(lba >> 8);  cdb[5] = (uint8_t)lba;
    cdb[7] = (uint8_t)(count >> 8); cdb[8] = (uint8_t)count; // Transfer Length, big-endian

    uint32_t actual = 0;
    uint8_t csw_status = 0xFFu;
    if (!scsi_command(g_console_ep, idx, dev.slot_id, cdb, 10, /*data_dir_in=*/false, dev.bounce_paddr,
                       count * 512u, actual, csw_status)) {
        return false;
    }
    // issuse.txt №11: симметрично чтению — если устройство приняло меньше
    // байт, чем запрошено, это не полноценная успешная запись, даже если
    // CSW status формально 0.
    bool ok = csw_status == 0 && actual == count * 512u;
    // issuse.txt №66 — "занят, но жив" для watchdog'а, см. комментарий у
    // g_usb_liveness_ntfn выше.
    if (ok && g_usb_liveness_ntfn != 0) seL4_Signal(g_usb_liveness_ntfn);
    return ok;
}

// exfat.h фиксирует сигнатуру block_read_fn/block_write_fn без параметра
// контекста — N тонких обёрток, каждая жёстко зашивает СВОЙ индекс и
// делегирует общей hardware_usb_rw_generic_*(idx, ...) выше. Написаны
// явно (не через макрос) — USB_MAX_DEVICES мало (4), явный код читается
// проще, чем макрос+его раскрытие в диагностике компилятора.
static bool hardware_usb_read_0(uint32_t s, uint32_t c, void* b) { return hardware_usb_rw_generic_read(0, s, c, b); }
static bool hardware_usb_read_1(uint32_t s, uint32_t c, void* b) { return hardware_usb_rw_generic_read(1, s, c, b); }
static bool hardware_usb_read_2(uint32_t s, uint32_t c, void* b) { return hardware_usb_rw_generic_read(2, s, c, b); }
static bool hardware_usb_read_3(uint32_t s, uint32_t c, void* b) { return hardware_usb_rw_generic_read(3, s, c, b); }
static bool hardware_usb_read_4(uint32_t s, uint32_t c, void* b) { return hardware_usb_rw_generic_read(4, s, c, b); }
static bool hardware_usb_read_5(uint32_t s, uint32_t c, void* b) { return hardware_usb_rw_generic_read(5, s, c, b); }
static bool hardware_usb_read_6(uint32_t s, uint32_t c, void* b) { return hardware_usb_rw_generic_read(6, s, c, b); }
static bool hardware_usb_read_7(uint32_t s, uint32_t c, void* b) { return hardware_usb_rw_generic_read(7, s, c, b); }
static bool hardware_usb_write_0(uint32_t s, uint32_t c, const void* b) { return hardware_usb_rw_generic_write(0, s, c, b); }
static bool hardware_usb_write_1(uint32_t s, uint32_t c, const void* b) { return hardware_usb_rw_generic_write(1, s, c, b); }
static bool hardware_usb_write_2(uint32_t s, uint32_t c, const void* b) { return hardware_usb_rw_generic_write(2, s, c, b); }
static bool hardware_usb_write_3(uint32_t s, uint32_t c, const void* b) { return hardware_usb_rw_generic_write(3, s, c, b); }
static bool hardware_usb_write_4(uint32_t s, uint32_t c, const void* b) { return hardware_usb_rw_generic_write(4, s, c, b); }
static bool hardware_usb_write_5(uint32_t s, uint32_t c, const void* b) { return hardware_usb_rw_generic_write(5, s, c, b); }
static bool hardware_usb_write_6(uint32_t s, uint32_t c, const void* b) { return hardware_usb_rw_generic_write(6, s, c, b); }
static bool hardware_usb_write_7(uint32_t s, uint32_t c, const void* b) { return hardware_usb_rw_generic_write(7, s, c, b); }
static_assert(USB_MAX_DEVICES == 8, "добавьте ещё пару hardware_usb_read_N/write_N и строку в step15_mount_filesystem, если увеличили USB_MAX_DEVICES");

// GPT (GUID Partition Table) — найдено на живом железе: NVMe SSD через
// переходник, ранее стоявший системным диском в другом компьютере,
// размечен GPT, а не legacy MBR. Protective MBR (LBA 0) по спецификации
// UEFI ВСЕГДА содержит РОВНО ОДНУ запись — #0, типа 0xEE, на весь диск —
// это не "MBR без разделов", а гарантированный признак "настоящая таблица
// разделов — в GPT-заголовке на LBA1", отличимый от отсутствия разметки.
static bool gpt_type_is_basic_data(const uint8_t *entry) {
    // "Microsoft Basic Data" GUID EBD0A0A2-B9E5-4433-87C0-68B6B72699C7,
    // как записано на диске (mixed-endian, см. UEFI spec §5.3.3) — общий
    // тип для NTFS/exFAT/FAT32 в GPT (GPT не различает их отдельными GUID);
    // окончательную проверку "это точно exFAT" всё равно делает exfat_init()
    // по сигнатуре, как и для MBR-пути ниже.
    static const uint8_t kBasicData[16] = {
        0xA2, 0xA0, 0xD0, 0xEB, 0xE5, 0xB9, 0x33, 0x44,
        0x87, 0xC0, 0x68, 0xB6, 0xB7, 0x26, 0x99, 0xC7
    };
    for (int i = 0; i < 16; i++) if (entry[i] != kBasicData[i]) return false;
    return true;
}

static bool find_gpt_exfat_partition(seL4_CPtr console_ep, int idx) {
    uint8_t hdr[512];
    if (!hardware_usb_rw_generic_read(idx, 1, 1, hdr)) return false;
    static const uint8_t kSig[8] = {'E','F','I',' ','P','A','R','T'};
    for (int i = 0; i < 8; i++) {
        if (hdr[i] != kSig[i]) {
            sys_puts(console_ep, "[USB]   ПРЕДУПРЕЖДЕНИЕ: protective MBR (0xEE) есть, но подписи GPT-заголовка (EFI PART) на LBA1 нет — пропускаю.\n");
            return false;
        }
    }
    uint64_t part_entry_lba = 0;
    for (int i = 0; i < 8; i++) part_entry_lba |= ((uint64_t)hdr[72 + i]) << (8 * i);
    uint32_t num_entries = (uint32_t)hdr[80] | ((uint32_t)hdr[81] << 8) | ((uint32_t)hdr[82] << 16) | ((uint32_t)hdr[83] << 24);
    uint32_t entry_size  = (uint32_t)hdr[84] | ((uint32_t)hdr[85] << 8) | ((uint32_t)hdr[86] << 16) | ((uint32_t)hdr[87] << 24);
    if (entry_size == 0 || entry_size > 512) {
        sys_puts(console_ep, "[USB]   ПРЕДУПРЕЖДЕНИЕ: GPT SizeOfPartitionEntry подозрительный — пропускаю.\n");
        return false;
    }
    // Ограничиваемся первыми 4 секторами таблицы разделов (при типичном
    // entry_size=128 — до 16 записей) — с большим запасом хватает на
    // ESP+MSR+данные+recovery, которые реально встречаются на живых дисках;
    // не читаем всю (обычно 32-секторную) таблицу ради экономии bulk-транзакций.
    constexpr uint32_t kScanSectors = 4;
    static uint8_t entry_buf[512 * kScanSectors];
    if (!hardware_usb_rw_generic_read(idx, (uint32_t)part_entry_lba, kScanSectors, entry_buf)) return false;
    uint32_t entries_per_sector = 512 / entry_size;
    uint32_t max_entries = entries_per_sector * kScanSectors;
    if (max_entries > num_entries) max_entries = num_entries;
    for (uint32_t i = 0; i < max_entries; i++) {
        const uint8_t *entry = entry_buf + i * entry_size;
        bool all_zero = true;
        for (int b = 0; b < 16; b++) if (entry[b] != 0) { all_zero = false; break; }
        if (all_zero) continue;
        if (gpt_type_is_basic_data(entry)) {
            uint64_t start_lba = 0;
            for (int b = 0; b < 8; b++) start_lba |= ((uint64_t)entry[32 + b]) << (8 * b);
            g_usb_devices[idx].partition_start_sector = (uint32_t)start_lba;
            sys_puthex32(console_ep, "[USB]   GPT: найден раздел Microsoft Basic Data, start LBA = ", (uint32_t)start_lba);
            return true;
        }
    }
    sys_puts(console_ep, "[USB]   GPT: раздела типа Microsoft Basic Data (NTFS/exFAT/FAT32) не нашлось.\n");
    return false;
}

// Milestone 7 — поиск начала exFAT-раздела на USB-накопителе, тот же приём,
// что find_exfat_partition() в blk_driver.cpp: ищем MBR-запись типа 0x07.
// В отличие от SD-карты (где ВСЕГДА двухпартиционная схема — первая FAT32
// для прошивки, вторая exFAT), флешка может быть отформатирована ОДНИМ
// разделом "суперфлоппи" (exFAT прямо с LBA 0, без MBR вообще) — если MBR
// не найден или нет записи типа 0x07, просто оставляем start_sector=0 и
// пробуем монтировать оттуда; настоящую проверку "это точно exFAT" всё
// равно делает exfat_init() по сигнатуре, а не эта функция. Если MBR есть,
// но все 4 записи пусты, кроме записи 0 типа 0xEE — это protective MBR,
// разбираем GPT (find_gpt_exfat_partition) вместо "разделов нет".
static void find_usb_exfat_partition(seL4_CPtr console_ep, int idx) {
    uint8_t sector0[512];
    if (!hardware_usb_rw_generic_read(idx, 0, 1, sector0)) {
        sys_puts(console_ep, "[USB] ПРЕДУПРЕЖДЕНИЕ: не удалось прочитать сектор 0 для поиска раздела.\n");
        return;
    }
    uint16_t sig = (uint16_t)sector0[510] | ((uint16_t)sector0[511] << 8);
    if (sig != 0xAA55) {
        if (LOG_USB) sys_puts(console_ep, "[USB]   MBR-подписи нет — пробую монтировать как superfloppy (exFAT прямо с LBA 0).\n");
        return;
    }
    for (int i = 0; i < 4; i++) {
        const uint8_t *entry = &sector0[0x1BE + i * 16];
        if (entry[4] == 0x07) { // exFAT/NTFS — уточняется в exfat_init() по сигнатуре
            g_usb_devices[idx].partition_start_sector = (uint32_t)entry[8] | ((uint32_t)entry[9] << 8)
                                          | ((uint32_t)entry[10] << 16) | ((uint32_t)entry[11] << 24);
            if (LOG_USB) sys_puthex32(console_ep, "[USB]   Найден раздел-кандидат (тип 0x07), start LBA = ", g_usb_devices[idx].partition_start_sector);
            return;
        }
    }
    if (sector0[0x1BE + 4] == 0xEE) {
        if (find_gpt_exfat_partition(console_ep, idx)) return;
    }
    if (LOG_USB) sys_puts(console_ep, "[USB]   MBR есть, но раздела типа 0x07 (exFAT) не нашлось — пробую монтировать с LBA 0 как есть.\n");
}

// Шаг 15 — монтирование exFAT (только чтение в этой фазе, см.
// RPI4_USB_ALLOW_WRITE). slot_id сохраняется в g_usb_devices[idx].slot_id
// — hardware_usb_read_N/write_N вызываются exfat.cpp БЕЗ параметра idx
// (фиксированная сигнатура), берут его из того, КАКАЯ ИЗ N ОБЁРТОК была
// передана exfat_init() ниже.
static void step15_mount_filesystem(seL4_CPtr console_ep, int idx, uint8_t slot_id) {
    if (LOG_USB) sys_puts(console_ep, "[USB] Шаг 15: монтирование exFAT (только чтение).\n");
    UsbDeviceSlot &dev = g_usb_devices[idx];
    dev.slot_id = slot_id;
    dev.partition_start_sector = 0;
    find_usb_exfat_partition(console_ep, idx);
    bool mounted;
    switch (idx) {
        case 0:  mounted = exfat_init(&dev.fs, hardware_usb_read_0, hardware_usb_write_0); break;
        case 1:  mounted = exfat_init(&dev.fs, hardware_usb_read_1, hardware_usb_write_1); break;
        case 2:  mounted = exfat_init(&dev.fs, hardware_usb_read_2, hardware_usb_write_2); break;
        case 3:  mounted = exfat_init(&dev.fs, hardware_usb_read_3, hardware_usb_write_3); break;
        case 4:  mounted = exfat_init(&dev.fs, hardware_usb_read_4, hardware_usb_write_4); break;
        case 5:  mounted = exfat_init(&dev.fs, hardware_usb_read_5, hardware_usb_write_5); break;
        case 6:  mounted = exfat_init(&dev.fs, hardware_usb_read_6, hardware_usb_write_6); break;
        default: mounted = exfat_init(&dev.fs, hardware_usb_read_7, hardware_usb_write_7); break;
    }
    if (mounted) {
        dev.storage_mounted = true;
        if (LOG_USB) sys_puts(console_ep, "[USB]   exFAT смонтирован (только чтение).\n");
    } else {
        dev.storage_mounted = false;
        sys_puts(console_ep, "[USB] ОШИБКА: exFAT не смонтировался (не exFAT / повреждён / не тот раздел?).\n");
    }
}

// Milestone 1 (закрытие Фазы 14, см. ROADMAP.md/план) — раньше единая
// run_bring_up() делала И контроллерную инициализацию (Шаги 0-4, разово
// нужную ОДИН РАЗ за всё время жизни процесса — повторный HCRST/пересоздание
// колец убил бы состояние уже смонтированного устройства), И перечисление
// конкретного устройства (Шаги 5-8, нужное на КАЖДОЕ подключение). Для
// hot-plug (Milestone 11) нужно уметь звать второе без первого — отсюда
// разделение на xhci_controller_init() (один раз) и
// enumerate_and_mount_device(port) (на каждое устройство/переподключение).
static bool xhci_controller_init(seL4_CPtr console_ep, uint32_t &max_ports) {
    // g_pcie_rc_base — фиксированный vaddr (PLAT_PCIE_RC_VADDR), реально
    // замаплен main.cpp безусловно вместе со всем остальным MMIO
    // is_driver==6 (см. spawn_process()) — отдельной проверки на nullptr
    // тут не нужно, тот же приём, что и у g_xhci_base ниже.
    step0_check_link(console_ep);

    uint8_t caplen; uint32_t hcsparams1, hcsparams2, hccparams1;
    if (!step1_read_capabilities(console_ep, caplen, hcsparams1, hcsparams2, hccparams1)) return false;

    g_op_base = g_xhci_base + caplen;
    if ((caplen % 8) != 0) {
        sys_puthex32(console_ep, "[USB] ПРЕДУПРЕЖДЕНИЕ: CAPLENGTH не кратен 8 — 64-битные регистры операционного блока могут быть невыровнены: ", caplen);
    }
    uint32_t dboff = *reg32(g_xhci_base, XHCI_DBOFF) & ~0x3u;
    uint32_t rtsoff = *reg32(g_xhci_base, XHCI_RTSOFF) & ~0x1Fu;
    g_db_base = g_xhci_base + dboff;
    g_rt_base = g_xhci_base + rtsoff;
    g_ctx_size = (hccparams1 & (1u << 2)) ? 64 : 32; // CSZ

    max_ports = (hcsparams1 >> 24) & 0xFF;
    uint32_t max_slots = hcsparams1 & 0xFF;
    if (LOG_USB) sys_puthex32(console_ep, "[USB]   MaxPorts = ", max_ports);
    if (LOG_USB) sys_puthex32(console_ep, "[USB]   MaxSlots = ", max_slots);
    if (LOG_USB) sys_puthex32(console_ep, "[USB]   Context size = ", (uint32_t)g_ctx_size);

    if (!step2_reset(console_ep)) return false;
    if (!step3_setup_rings(console_ep, hcsparams2)) return false;
    if (!step4_run(console_ep)) return false;
    return true;
}

// Фаза 15 (несколько накопителей, см. ROADMAP.md/план) — находит первый
// НЕ занятый слот в g_usb_devices[]/слот ТЕКУЩЕГО смонтированного
// устройства на данном root-порту. -1 = не найдено.
static int find_free_device_slot() {
    for (int i = 0; i < USB_MAX_DEVICES; i++) if (!g_usb_devices[i].in_use) return i;
    return -1;
}
static int find_device_by_port(int port) {
    for (int i = 0; i < USB_MAX_DEVICES; i++) if (g_usb_devices[i].in_use && g_usb_devices[i].port == port) return i;
    return -1;
}
// Milestone A1 (Фаза 15, см. ROADMAP.md/план) — ВРЕМЕННО: VFS-диспетчер и
// USB_CMD_LIST/USB_CMD_GET_VOLUME_NAME (см. main() ниже) пока обслуживают
// только ПЕРВОЕ найденное/смонтированное устройство — честный,
// промежуточный шаг перед Milestone A3 (реальная многоустройственная
// маршрутизация по имени тома в самом пути, требует изменений в
// shell.cpp/h/sys_client.h/sbin/ls.cpp/sbin/mv.cpp — отдельный milestone).
// -1, если ни одного нет — вызывающий код обязан проверять.
static int first_found_device_idx() {
    for (int i = 0; i < USB_MAX_DEVICES; i++) if (g_usb_devices[i].found.found) return i;
    return -1;
}
static int first_mounted_device_idx() {
    for (int i = 0; i < USB_MAX_DEVICES; i++) if (g_usb_devices[i].storage_mounted) return i;
    return -1;
}

// Milestone A3 (Фаза 15) — путь приходит от клиента уже БЕЗ "/mnt" (см.
// route_vfs_path() в shell.cpp/h/sys_client.h — клиент срезает только сам
// "/mnt" целиком, без лишнего IPC на каждый путь), ведущий компонент
// остатка — имя тома (volume_name СМОНТИРОВАННОГО устройства), не
// хардкоженное "usb0". Срезает совпавшее "/<имя>" ПРЯМО В БУФЕРЕ (тот же
// приём, что раньше делал клиент) — пустой остаток -> "/", иначе
// возвращает индекс устройства; -1, если ведущий компонент не совпал ни
// с одним смонтированным устройством (честная ошибка выше по стеку).
static int resolve_device_by_path(char *path) {
    if (path[0] != '/') return -1;
    int end = 1;
    // issuse.txt №17: раньше без верхней границы — если SHM-страница
    // заполнена край в край без нулевого байта (баг/неисправный клиент),
    // сканирование уходило за пределы замапленной страницы в
    // немаппированную память. 512 — с большим запасом относительно
    // реальных путей (том + подпуть), хорошо укладывается в любую
    // реальную SHM-страницу этого проекта.
    while (path[end] != '\0' && path[end] != '/' && end < 511) end++;
    int name_len = end - 1;
    for (int i = 0; i < USB_MAX_DEVICES; i++) {
        if (!g_usb_devices[i].storage_mounted) continue;
        int vlen = (int)my_strlen(g_usb_devices[i].volume_name);
        if (vlen != name_len) continue;
        bool match = true;
        for (int k = 0; k < vlen; k++) {
            if (g_usb_devices[i].volume_name[k] != path[1 + k]) { match = false; break; }
        }
        if (!match) continue;
        const char *remainder = path + end;
        if (remainder[0] == '\0') { path[0] = '/'; path[1] = '\0'; }
        else { int j = 0; while (remainder[j] != '\0') { path[j] = remainder[j]; j++; } path[j] = '\0'; }
        return i;
    }
    return -1;
}

// Шаги 5-8 для КОНКРЕТНОГО порта — вызывается один раз на boot-пути (после
// сканирования, см. run_bring_up()) и один раз на каждое hot-plug
// подключение (Milestone 11), без пересоздания колец/повторного HCRST.
// idx — индекс в g_usb_devices[], выделенный вызывающим (см.
// try_enumerate_port ниже) ДО начала перечисления. out_slot_id — выводит
// выделенный Slot ID (0, если Enable Slot не дошёл до успеха) вызывающему,
// ЧТОБЫ ОН МОГ его освободить (см. step_disable_slot), если решит, что
// это не то устройство.
// Milestone B3 (Фаза 15) — общий хвост перечисления (Шаги 8-15), ПОСЛЕ
// Address Device (у каждого пути — корневой порт или за хабом — СВОЙ
// вызов Шага 7, разные поля Slot Context, см. step7_address_device()) —
// дальше всё идентично: GET_DESCRIPTOR/Configure Endpoint/
// SET_CONFIGURATION/SCSI/exFAT не зависят от того, корневой это порт или
// хаб. Вынесено из enumerate_and_mount_device(), чтобы
// enumerate_device_behind_hub() могла переиспользовать его же, а не
// дублировать 15 строк подряд идущих вызовов.
static void continue_enumeration_after_address(seL4_CPtr console_ep, int idx, uint8_t slot_id, int port, uint32_t port_speed) {
    step8_get_device_descriptor(console_ep, idx, slot_id);
    if (!g_usb_devices[idx].found.found) return;
    // Milestone 3 — bDeviceClass часто 0 (класс на уровне Interface, не
    // Device) — окончательно решаем по Configuration/Interface Descriptor,
    // не только по Device Descriptor (Шаг 8 сам по себе не различает
    // Mass Storage от чего угодно ещё с bDeviceClass=0). bDeviceClass=0x09
    // (хаб) — известен уже здесь, из Шага 8, и НАДЁЖЕН (в отличие от
    // Mass Storage хаб не прячет класс на уровне Interface).
    bool is_hub = (g_usb_devices[idx].found.device_class == USB_CLASS_HUB);
    step9_get_configuration_descriptor(console_ep, idx, slot_id, port_speed, is_hub);
    // Milestone B1 (Фаза 15) — хаб идёт ОТДЕЛЬНОЙ веткой: у него
    // ЗАКОНОМЕРНО нет Mass Storage bulk-эндпоинтов (см. is_hub выше),
    // вместо Шагов 10-15 — class-специфичный Hub Descriptor.
    // try_enumerate_port() решит, оставлять ли слот занятым хабом.
    if (is_hub) {
        step_hub_enumerate(console_ep, idx, slot_id);
        // Milestone B3 — Configure Endpoint (слот, см. step_hub_configure_slot())
        // ОБЯЗАТЕЛЕН до попытки адресовать любого ребёнка этого хаба —
        // если он не удался, честно НЕ сканируем downstream-порты (нет
        // смысла находить детей, которых всё равно не сможем адресовать).
        if (!step_hub_configure_slot(console_ep, idx, slot_id, port, port_speed)) return;
        // Milestone B4 — сразу "вооружаем" Interrupt-эндпоинт статуса
        // портов (одна слушающая TRB) — дальше poll_hub_interrupts()
        // подхватит её на heartbeat-тиках, обеспечивая динамический
        // hot-plug ЗА хабом без перезагрузки.
        hub_enqueue_interrupt_listen(idx);
        // Milestone B2 (Фаза 15) — статический скан downstream-портов
        // сразу после того, как узнали bNbrPorts; если Hub Descriptor не
        // прочитался, hub_num_ports==0 и цикл внутри просто не выполнится.
        step_hub_scan_downstream_ports(console_ep, idx, slot_id);
        return;
    }
    if (!g_usb_devices[idx].bulk_eps.found) return; // не Mass Storage/SCSI/BOT — try_enumerate_port сам решит, пробовать ли дальше
    // Milestone 4 — активируем bulk-эндпоинты и переводим устройство в
    // Configured-состояние. Без этого никакие данные передавать нельзя —
    // устройство остаётся в Addressed-состоянии (см. USB спецификацию,
    // диаграмма состояний устройства).
    if (!step10_configure_endpoints(console_ep, idx, slot_id, port, port_speed)) return;
    if (!step11_set_configuration(console_ep, idx, slot_id)) return;
    // Milestone 5 — первая настоящая проверка данных через bulk-эндпоинты.
    step12_inquiry(console_ep, idx, slot_id);
    // Milestone 6 — готовность устройства + ёмкость (нужна для Milestone 7,
    // монтирования exFAT).
    if (!step13_test_unit_ready(console_ep, idx, slot_id)) return;
    if (!step14_read_capacity(console_ep, idx, slot_id)) return;
    // Milestone 7 — монтируем exFAT (только чтение).
    step15_mount_filesystem(console_ep, idx, slot_id);
}

// Шаги 5-8 для КОНКРЕТНОГО порта — вызывается один раз на boot-пути (после
// сканирования, см. run_bring_up()) и один раз на каждое hot-plug
// подключение (Milestone 11), без пересоздания колец/повторного HCRST.
// idx — индекс в g_usb_devices[], выделенный вызывающим (см.
// try_enumerate_port ниже) ДО начала перечисления. out_slot_id — выводит
// выделенный Slot ID (0, если Enable Slot не дошёл до успеха) вызывающему,
// ЧТОБЫ ОН МОГ его освободить (см. step_disable_slot), если решит, что
// это не то устройство.
static void enumerate_and_mount_device(seL4_CPtr console_ep, int idx, int port, uint8_t &out_slot_id) {
    out_slot_id = 0;
    if (!step5_port_reset(console_ep, port)) return;

    uint32_t portsc = *reg32(g_op_base, XHCI_OP_PORTSC_BASE + (uintptr_t)(port - 1) * 0x10);
    uint32_t port_speed = (portsc >> 10) & 0xF;

    uint8_t slot_id = 0;
    if (!step6_enable_slot(console_ep, slot_id)) return;
    out_slot_id = slot_id;
    // Milestone B3 — НАЙДЕНО НА ЖИВОМ ЖЕЛЕЗЕ: раньше g_usb_devices[idx].slot_id/.port
    // выставлялись только СНАРУЖИ, в try_enumerate_port(), ПОСЛЕ того, как
    // весь этот вызов (включая вложенный, для хаба — step_hub_scan_downstream_ports()
    // -> enumerate_device_behind_hub()) уже вернул управление. Если ЭТО
    // устройство — хаб, его дети читают g_usb_devices[hub_idx].slot_id/.port
    // ИЗНУТРИ этого же вызова (см. enumerate_device_behind_hub()) — там
    // ещё лежали 0 (невалидные Slot ID/Root Hub Port Number, подтверждено
    // диагностическим дампом Input Context) — Parameter Error на Address
    // Device ребёнка. Выставляем здесь же, сразу после Enable Slot —
    // try_enumerate_port() и step15_mount_filesystem() пишут то же самое
    // значение позже повторно, безопасно.
    g_usb_devices[idx].slot_id = slot_id;
    g_usb_devices[idx].port = port;
    if (!step7_address_device(console_ep, idx, slot_id, port, port_speed)) return;
    continue_enumeration_after_address(console_ep, idx, slot_id, port, port_speed);
}

// Milestone B3 (Фаза 15) — устройство ЗА хабом (не на корневом порту).
// Отличия от enumerate_and_mount_device(): нет своего Port Reset (сброс
// СВОЕГО, downstream-порта хаба уже сделан вызывающим —
// step_hub_scan_downstream_ports()); Enable Slot — своя, отдельная
// команда (у каждого устройства в топологии свой Slot ID, даже за одним
// хабом), Address Device — с полями хаб-топологии (parent_* поля
// UsbDeviceSlot, выставляются здесь ДО step7_address_device(), которая
// их читает). Дальше — тот же общий хвост (Шаги 8-15), что и у
// корневых устройств.
static void enumerate_device_behind_hub(seL4_CPtr console_ep, int hub_idx, uint8_t hub_port, int idx, uint8_t &out_slot_id, uint32_t device_speed) {
    out_slot_id = 0;
    UsbDeviceSlot &hub = g_usb_devices[hub_idx];
    UsbDeviceSlot &self = g_usb_devices[idx];
    self.behind_hub = true;
    self.parent_hub_idx = hub_idx;
    self.parent_hub_slot_id = hub.slot_id;
    self.parent_port_number = hub_port;
    self.parent_multi_tt = (hub.found.device_protocol == 2); // см. USB 2.0 spec, Hub Class Device Descriptor bDeviceProtocol — для SS-хаба (protocol==3) остаётся false, MTT/TT не для SS
    self.parent_tt_think_time = hub.hub_tt_think_time;
    // issuse.txt №15 — НАЙДЕНО НА ЖИВОМ ЖЕЛЕЗЕ (первый раз, когда реально
    // дошли до 2 яруса вложенности — устройство за хабом пользователя,
    // который сам висит на встроенном root-hub'е VL805): hub.port у
    // РОДИТЕЛЯ равен 0, если родитель САМ не на корневом порту (см.
    // behind_hub) — наследуем настоящий корневой порт и полный Route
    // String от родителя, а не берём его "port"/parent_port_number
    // напрямую (это годится ТОЛЬКО для 1 яруса, ровно то, что было
    // проверено раньше). hub.hub_tier/route_string_full/root_port_full
    // для хаба НА корневом порту остаются 0 по умолчанию — формулы ниже
    // корректно сводятся к прежнему поведению для 1 яруса.
    self.hub_tier = (uint8_t)(hub.hub_tier + 1);
    self.route_string_full = hub.route_string_full | ((uint32_t)(hub_port & 0xFu) << (4 * (self.hub_tier - 1)));
    self.root_port_full = hub.behind_hub ? hub.root_port_full : hub.port;

    uint8_t slot_id = 0;
    if (!step6_enable_slot(console_ep, slot_id)) return;
    out_slot_id = slot_id;
    self.slot_id = slot_id; // см. тот же комментарий в enumerate_and_mount_device() — на будущее (вложенные хабы), сейчас не критично
    // Root Hub Port Number = НАСТОЯЩИЙ корневой порт xHC, на котором
    // висит вся цепочка (см. root_port_full выше), НЕ порт непосредственного
    // родителя — downstream-порт хаба идёт отдельным полем (Parent Port
    // Number, dword2, см. step7).
    // device_speed — Milestone B5 (доп.): 3 (High-Speed, за HS-хабом)
    // или 4 (SuperSpeed, за SS-хабом) — вызывающий (hub_handle_port_connect())
    // уже определил реальную скорость и отфильтровал остальные случаи.
    if (!step7_address_device(console_ep, idx, slot_id, self.root_port_full, device_speed)) return;
    continue_enumeration_after_address(console_ep, idx, slot_id, self.root_port_full, device_speed);
}

// Milestone 2 (находка на живом железе, см. ROADMAP.md) — на этой плате
// порт 1 ПОСТОЯННО занят внутренним root-hub'ом VL805 (0x2109:0x3431,
// bDeviceClass=0x09), независимо от того, что физически воткнуто в разъёмы
// — не связано с внешним хабом пользователя. "Первый порт с CCS=1" —
// НЕ обязательно искомое устройство. Полная попытка (Enable Slot/Address
// Device/GET_DESCRIPTOR) на этом порту + проверка класса результата;
// возвращает true, только если найдено НЕ-hub устройство (годится и для
// hot-plug — Milestone 11 сможет звать это же на новый порт из Port
// Status Change Event, не только на boot-скане).
//
// Фаза 15 — САМА находит свободный слот в g_usb_devices[] (было: работала
// с единственным устройством) — out_idx выводит его вызывающему на
// успехе (для сообщений в консоль), -1 при любой неудаче, включая "нет
// свободных слотов" (честная ошибка, не тихий отказ).
static bool try_enumerate_port(seL4_CPtr console_ep, int port, int &out_idx) {
    out_idx = -1;
    int idx = find_free_device_slot();
    if (idx < 0) {
        sys_puts(console_ep, "[USB]   ПРЕДУПРЕЖДЕНИЕ: нет свободных слотов под накопитель (USB_MAX_DEVICES исчерпан) — устройство проигнорировано.\n");
        return false;
    }
    g_usb_devices[idx].found = UsbFoundDevice{};
    g_usb_devices[idx].bulk_eps = UsbBulkEndpoints{};
    // Milestone B3 — НАЙДЕНО НА ЖИВОМ ЖЕЛЕЗЕ, серьёзный баг: раньше
    // in_use/port выставлялись ТОЛЬКО в конце, ПОСЛЕ успешного
    // enumerate_and_mount_device() — но если ЭТО устройство хаб, внутри
    // ТОГО ЖЕ вызова (continue_enumeration_after_address ->
    // step_hub_scan_downstream_ports -> enumerate_device_behind_hub)
    // find_free_device_slot() ищет слот для РЕБЁНКА, пока in_use ещё
    // false — может вернуть ТОТ ЖЕ idx, что уже используется для хаба!
    // Данные ребёнка затирают данные хаба в одной структуре, внешний код
    // потом частично восстанавливает хабовы slot_id/port поверх уже
    // смонтированного ребёнка — Frankenstein-состояние (found/bulk_eps/
    // fs/volume_name от ребёнка, slot_id/port от хаба), объясняющее
    // живую находку: бул-передача ls звонила в доорбелл с slot_id хаба
    // и DCI ребёнка ("Endpoint Not Enabled"). Резервируем idx СРАЗУ —
    // до enumerate_and_mount_device(), не после.
    g_usb_devices[idx].in_use = true;
    g_usb_devices[idx].port = port;
    // issuse.txt №15 — НАЙДЕНО НА ЖИВОМ ЖЕЛЕЗЕ (плавающий зависон Address
    // Device после многих циклов подключения/отключения в разные порты):
    // этот слот мог РАНЬШЕ принадлежать устройству ЗА хабом (behind_hub=
    // true, ненулевые hub_tier/route_string_full/root_port_full) —
    // find_free_device_slot() не различает роль ПРЕДЫДУЩЕГО обитателя.
    // Если тот же idx теперь достаётся устройству НА КОРНЕВОМ порту,
    // step7_address_device() читает self.behind_hub/route_string_full
    // как есть — сброс только found/bulk_eps их не трогал, в Address
    // Device уходил чужой, стухший Route String/MTT от предыдущего
    // обитателя. Сбрасываем явно, а не полагаемся на enumerate_device_
    // behind_hub() (для КОРНЕВОГО устройства она вообще не вызывается).
    g_usb_devices[idx].behind_hub = false;
    g_usb_devices[idx].parent_hub_idx = -1;
    g_usb_devices[idx].parent_hub_slot_id = 0;
    g_usb_devices[idx].parent_port_number = 0;
    g_usb_devices[idx].parent_multi_tt = false;
    g_usb_devices[idx].parent_tt_think_time = 0;
    g_usb_devices[idx].hub_tier = 0;
    g_usb_devices[idx].route_string_full = 0;
    g_usb_devices[idx].root_port_full = 0;
    uint8_t slot_id = 0;
    enumerate_and_mount_device(console_ep, idx, port, slot_id);
    // Milestone 3/B1 — "то самое" устройство теперь означает: ЛИБО хаб
    // (Шаг 8, class-специфичный Hub Descriptor прочитан — Milestone B1,
    // раньше отбрасывался ЗДЕСЬ), ЛИБО нашлась Mass Storage/SCSI/BOT-
    // интерфейс с обоими bulk-эндпоинтами (Шаг 9) — bDeviceClass=0 сам по
    // себе ничего не решает. НЕ проверяет факт монтирования exFAT — см.
    // поле storage_mounted отдельно (найдено на живом железе:
    // NVMe-переходник enumerated чисто, но exFAT не смонтировался — это
    // два разных исхода).
    bool is_hub = g_usb_devices[idx].found.found && g_usb_devices[idx].found.device_class == USB_CLASS_HUB;
    bool is_storage = g_usb_devices[idx].found.found && !is_hub && g_usb_devices[idx].bulk_eps.found;
    bool keep = is_hub || is_storage;
    if (!keep) {
        // Освобождаем слот в ЛЮБОМ случае отказа (не-MSC интерфейс,
        // ИЛИ любая другая неудача после Enable Slot) — иначе следующая
        // попытка на другом порту упрётся в "No Slots Available" (см.
        // ROADMAP.md, живая находка).
        if (slot_id != 0) step_disable_slot(console_ep, slot_id);
        g_usb_devices[idx].found.found = false;
        g_usb_devices[idx].bulk_eps.found = false;
        g_usb_devices[idx].in_use = false; // резервировали заранее (см. выше) — освобождаем обратно на отказе
        g_usb_devices[idx].port = 0;
        return false;
    }
    // Milestone B1 — раньше slot_id выставлялся ТОЛЬКО в
    // step15_mount_filesystem() (только для накопителей); хаб до неё не
    // доходит (enumerate_and_mount_device возвращается сразу после
    // step_hub_enumerate()) — без этого unmount_usb_storage() не смог бы
    // на отключении освободить настоящий xHCI Slot ID хаба (её же
    // собственная guard-проверка `slot_id==0` посчитала бы, что нечего
    // освобождать). Присваивание здесь ИЗБЫТОЧНО, но безопасно для
    // накопителей (то же значение, что позже повторно запишет Шаг 15).
    g_usb_devices[idx].slot_id = slot_id;
    out_idx = idx;
    return true;
}

// Milestone 11 (закрытие Фазы 14, hot-plug) — размонтирует УКАЗАННОЕ
// устройство и освобождает его Slot ID (иначе следующий Enable Slot после
// переподключения упрётся в "No Slots Available", та же находка, что уже
// была на Milestone 2). Вызывается и при явном disconnect (Port Status
// Change Event / опрос, см. ниже), и — на всякий случай — перед попыткой
// смонтировать НОВОЕ устройство В ТОТ ЖЕ СЛОТ, если предыдущее почему-то
// осталось помеченным смонтированным.
static void unmount_usb_storage(seL4_CPtr console_ep, int idx) {
    UsbDeviceSlot &dev = g_usb_devices[idx];
    if (!dev.in_use) return; // Milestone B1 — было storage_mounted||slot_id!=0, но хаб не смонтирован в exFAT-смысле; in_use покрывает и его, и накопитель единообразно
    if (LOG_USB && dev.found.device_class != USB_CLASS_HUB) { sys_puts(console_ep, "[USB] Hot-plug: размонтирую /mnt/"); sys_puts(console_ep, dev.volume_name); sys_puts(console_ep, "\n"); }
    if (dev.slot_id != 0) step_disable_slot(console_ep, dev.slot_id);
    dev.storage_mounted = false;
    dev.found.found = false;
    dev.bulk_eps.found = false;
    dev.slot_id = 0;
    dev.partition_start_sector = 0;
    dev.port = 0;
    dev.in_use = false;
    dev.hub_num_ports = 0;
    dev.hub_pwr_on_to_pwr_good = 0;
    // issuse.txt №12: раньше топология хаба НЕ сбрасывалась — если этот
    // слот раньше был устройством ЗА хабом (behind_hub=true), а потом
    // переиспользовался под НОВОЕ устройство в корневом порту,
    // step7_address_device()/step10_configure_endpoints() строили Route
    // String и multi-TT флаги из этих устаревших полей, ломая Address
    // Device для устройства, которое вообще не за хабом.
    dev.behind_hub = false;
    dev.parent_hub_idx = -1;
    dev.parent_hub_slot_id = 0;
    dev.parent_port_number = 0;
    dev.parent_multi_tt = false;
    dev.parent_tt_think_time = 0;
    dev.volume_name[0]='u'; dev.volume_name[1]='s'; dev.volume_name[2]='b'; dev.volume_name[3]=(char)('0'+idx); dev.volume_name[4]='\0';
}

// Milestone 11 — реакция на Port Status Change Event (см. главный цикл):
// CCS всегда нужно перепроверить ЖИВЫМ чтением PORTSC — само событие
// сообщает только номер порта, не новое состояние (см. xHCI 6.4.2.3).
// Очистка CSC — тот же безопасный RW1C-паттерн, что step5_port_reset()
// (см. ROADMAP.md, "тридцатая попытка": запись PED=1 обратно ВЫКЛЮЧИЛА
// БЫ порт — PED не входит в PORTSC_RW1C_MASK, нулим его явно).
static void handle_port_status_change(seL4_CPtr console_ep, uint8_t port) {
    if (port < 1) return;
    uintptr_t off = XHCI_OP_PORTSC_BASE + (uintptr_t)(port - 1) * 0x10;
    uint32_t portsc = *reg32(g_op_base, off);
    bool connected = (portsc & PORTSC_CCS) != 0;
    *reg32(g_op_base, off) = (portsc & ~PORTSC_RW1C_MASK & ~PORTSC_PED) | (portsc & PORTSC_CSC);

    if (connected) {
        if (LOG_USB) sys_puthex32(console_ep, "[USB] Hot-plug: обнаружено подключение, порт = ", port);
        int idx = -1;
        if (!try_enumerate_port(console_ep, (int)port, idx)) {
            if (LOG_USB) sys_puts(console_ep, "[USB] Hot-plug: устройство на этом порту не подошло (hub / не Mass Storage) или свободных слотов не осталось.\n");
        } else {
            if (LOG_USB) sys_puts(console_ep, "[USB] Hot-plug: устройство смонтировано.\n");
        }
    } else {
        int idx = find_device_by_port((int)port);
        if (idx >= 0) {
            if (LOG_USB) sys_puts(console_ep, "[USB] Hot-plug: обнаружено отключение смонтированного устройства.\n");
            unmount_usb_storage(console_ep, idx);
        }
    }
}

// Milestone 11 (доп., по запросу пользователя) — max_ports нужен и вне
// run_bring_up() (см. poll_ports_for_hotplug() ниже, вызывается из
// главного цикла на каждый heartbeat-тик) — кэшируем один раз здесь.
static uint32_t g_max_ports = 0;

// План "Сигналы драйверам" — SYS_DRIVER_SIGNAL(STOP) гейтит VFS-командную
// цепочку (cmd 110-120) в главном цикле ниже; хот-плаг/heartbeat-обработка
// продолжает идти как обычно (симметрично net_driver.cpp).
static bool g_usb_stopped = false;

// Фаза 15 (Milestone A2) — раньше останавливался на ПЕРВОМ успехе (одно
// устройство архитектурно); теперь честно обходит ВСЕ CCS=1 порты, чтобы
// смонтировать НЕСКОЛЬКО накопителей одновременно при загрузке —
// try_enumerate_port() сам откажет, если свободных слотов не осталось.
// Возвращает число реально смонтированных-как-Mass-Storage устройств (не
// факт монтирования exFAT — см. комментарий у try_enumerate_port).
static int run_bring_up(seL4_CPtr console_ep) {
    uint32_t max_ports = 0;
    if (!xhci_controller_init(console_ep, max_ports)) return 0;
    g_max_ports = max_ports;

    if (LOG_USB) sys_puts(console_ep, "[USB] Шаг 5: опрос портов на подключённое устройство.\n");
    print_all_ports_ccs(console_ep, max_ports);

    int found_count = 0;
    for (uint32_t p = 1; p <= max_ports; p++) {
        uint32_t portsc = *reg32(g_op_base, XHCI_OP_PORTSC_BASE + (uintptr_t)(p - 1) * 0x10);
        if (!(portsc & PORTSC_CCS)) continue;
        if (LOG_USB) sys_puthex32(console_ep, "[USB]   Пробую порт: ", p);
        int idx = -1;
        if (try_enumerate_port(console_ep, (int)p, idx)) found_count++;
    }
    if (found_count == 0) {
        // Milestone B1 (Фаза 15) — хабы больше не отбрасываются (см.
        // try_enumerate_port()), это сообщение честно означает "вообще
        // НИЧЕГО" — маловероятно на этой плате (порт 1 всегда занят
        // встроенным VL805), но формально возможно (например, при
        // полностью пустой шине без встроенного хаба).
        sys_puts(console_ep, "[USB]   Ни на одном порту не найдено ни накопителя, ни хаба.\n");
    }
    return found_count;
}

// Milestone 11 (доп., по запросу пользователя) — периодический опрос
// PORTSC.CCS на КАЖДОМ порту (вместо/вместе с Port Status Change Event,
// который на живом железе себя не показал надёжно — см. ROADMAP.md:
// после переподключения флешки bulk-передача провалилась на "старом"
// слоте, т.е. либо событие не пришло, либо обработка не успела). Сравнение
// со СНЯТЫМ НА ПРЕДЫДУЩЕМ тике состоянием — g_port_ccs_mask_initialized
// гасит ложное "подключение" на самом первом тике (когда устройство уже
// смонтировано с боевого bring-up).
static uint32_t g_last_port_ccs_mask = 0;
static bool g_port_ccs_mask_initialized = false;

static void poll_ports_for_hotplug(seL4_CPtr console_ep) {
    for (uint32_t p = 1; p <= g_max_ports && p <= 32; p++) {
        uint32_t portsc = *reg32(g_op_base, XHCI_OP_PORTSC_BASE + (uintptr_t)(p - 1) * 0x10);
        bool ccs = (portsc & PORTSC_CCS) != 0;
        uint32_t bit = 1u << (p - 1);
        bool was_ccs = (g_last_port_ccs_mask & bit) != 0;

        if (g_port_ccs_mask_initialized && ccs != was_ccs) {
            if (ccs) {
                sys_puthex32(console_ep, "[USB] Обнаружено подключение (опрос), порт = ", p);
                int idx = -1;
                if (!try_enumerate_port(console_ep, (int)p, idx)) {
                    sys_puts(console_ep, "[USB]   Устройство на этом порту не подошло (не Mass Storage/не хаб) или свободных слотов не осталось.\n");
                } else if (g_usb_devices[idx].found.device_class == USB_CLASS_HUB) {
                    // Milestone B1 (Фаза 15) — хаб теперь НЕ отбрасывается
                    // (см. try_enumerate_port()), но сам по себе не
                    // "смонтирован" в exFAT-смысле — отдельное honest-сообщение,
                    // не путать с "Флешка автоматически смонтирована".
                    sys_puthex32(console_ep, "[USB]   Хаб подключён и опрошен, портов = ", g_usb_devices[idx].hub_num_ports);
                    if (LOG_USB) sys_puts(console_ep, "[USB]   (перечисление downstream-устройств — Фаза B, следующие milestone'ы).\n");
                } else if (!g_usb_devices[idx].storage_mounted) {
                    // НАЙДЕНО НА ЖИВОМ ЖЕЛЕЗЕ — try_enumerate_port() проверяет
                    // только успех USB-перечисления (класс/bulk-эндпоинты), а
                    // НЕ факт монтирования exFAT (step15 внутри
                    // enumerate_and_mount_device вызывается без проверки
                    // возврата) — реальный NVMe-переходник enumerated чисто,
                    // но exFAT не смонтировался, а лог всё равно писал
                    // "смонтирована". Разделяем эти два случая явно.
                    sys_puts(console_ep, "[USB]   Устройство перечислено, но exFAT не смонтировался (не exFAT / повреждён / не тот раздел).\n");
                } else {
                    sys_puts(console_ep, "[USB]   Флешка автоматически смонтирована: /mnt/");
                    sys_puts(console_ep, g_usb_devices[idx].volume_name);
                    sys_puts(console_ep, "\n");
                }
            } else {
                int idx = find_device_by_port((int)p);
                if (idx >= 0) {
                    bool was_hub = (g_usb_devices[idx].found.device_class == USB_CLASS_HUB);
                    char old_name[32];
                    my_strcpy(old_name, g_usb_devices[idx].volume_name);
                    if (was_hub) {
                        sys_puts(console_ep, "[USB] Обнаружено отключение хаба (опрос) — размонтирую его детей...\n");
                        // Milestone B4 — каскадное отключение: хаб уходит
                        // ЦЕЛИКОМ (root-порт CCS погас), все его дети
                        // (behind_hub && parent_hub_idx==idx) физически
                        // отключились ВМЕСТЕ с ним — их downstream-порты
                        // индивидуально ничего не заметят (сам хаб уже не
                        // отвечает), unmount_usb_storage() ДО хаба, иначе
                        // find_device_behind_hub() их не найдёт (in_use
                        // уже сброшен у самого хаба).
                        for (int c = 0; c < USB_MAX_DEVICES; c++) {
                            if (g_usb_devices[c].in_use && g_usb_devices[c].behind_hub && g_usb_devices[c].parent_hub_idx == idx) {
                                char cname[32];
                                my_strcpy(cname, g_usb_devices[c].volume_name);
                                sys_puts(console_ep, "[USB]   Каскадное отключение: /mnt/");
                                sys_puts(console_ep, cname);
                                sys_puts(console_ep, "\n");
                                unmount_usb_storage(console_ep, c);
                            }
                        }
                    } else {
                        sys_puts(console_ep, "[USB] Обнаружено отключение (опрос) — размонтирую /mnt/");
                        sys_puts(console_ep, old_name);
                        sys_puts(console_ep, "\n");
                    }
                    unmount_usb_storage(console_ep, idx);
                    sys_puts(console_ep, was_hub ? "[USB]   Слот хаба освобождён.\n" : "[USB]   Точка монтирования удалена.\n");
                }
            }
        }
        if (ccs) g_last_port_ccs_mask |= bit; else g_last_port_ccs_mask &= ~bit;
    }
    g_port_ccs_mask_initialized = true;
}

// issuse.txt №69 — тот же приём и то же обоснование, что blk_vfs_reply() в
// blk_driver.cpp: заменяет seL4_Reply() для настоящих VFS-команд (110-120,
// после резолва устройства/write-гейта), чей reply-капа уже отложена в
// VFS_PENDING_REPLY_SLOT (см. seL4_CNode_SaveCaller перед `if (cmd == 110)`
// ниже) — так root может забрать эту капу из нашего CNode и ответить
// клиенту сам, если usb_driver умрёт/зависнет посреди обработки (см.
// generic_recover_process() в main.cpp).
static inline void usb_vfs_reply(seL4_MessageInfo_t info) {
    seL4_Send(VFS_PENDING_REPLY_SLOT, info);
    seL4_CNode_Delete(SELF_CNODE_SLOT, VFS_PENDING_REPLY_SLOT, 8);
}

// --- Главный цикл ---

int main(int argc, char *argv[]) {
    seL4_IPCBuffer *ipc = get_local_ipc();
    seL4_SetIPCBuffer(ipc);

    seL4_CPtr root_ep = ipc->msg[BOOT_ROOT_EP];
    seL4_CPtr console_ep = ipc->msg[BOOT_CONSOLE_EP];
    g_console_ep = console_ep; // Milestone 7 — hardware_usb_read/write берут его отсюда (сигнатура block_read_fn фиксирована)
    seL4_CPtr usb_cmd_ep = ipc->msg[BOOT_USB_EP];
    seL4_CPtr liveness_ntfn = ipc->msg[BOOT_USB_LIVENESS_NTFN_CAP]; // Фаза 3b плана "Сигналы драйверам", см. main.cpp
    g_usb_liveness_ntfn = liveness_ntfn; // issuse.txt №66 — hardware_usb_rw_generic_read/write() берут его отсюда, см. комментарий у объявления
    g_cntfrq = read_cntfrq();

    g_dcbaa_paddr          = ipc->msg[BOOT_USB_DCBAA_PADDR];
    g_cmdring_paddr        = ipc->msg[BOOT_USB_CMDRING_PADDR];
    g_erst_paddr           = ipc->msg[BOOT_USB_ERST_PADDR];
    g_evtring_paddr        = ipc->msg[BOOT_USB_EVTRING_PADDR];
    g_devctx_paddr_base    = ipc->msg[BOOT_USB_DEVCTX_PADDR];
    g_inputctx_paddr       = ipc->msg[BOOT_USB_INPUTCTX_PADDR];
    g_scratchpad_arr_paddr = ipc->msg[BOOT_USB_SCRATCHPAD_ARR_PADDR];
    g_scratchpad_supplied  = (int)ipc->msg[BOOT_USB_SCRATCHPAD_COUNT];
    for (int i = 0; i < g_scratchpad_supplied && i < USB_MAX_SCRATCHPAD_PAGES; i++) {
        g_scratchpad_buf_paddr[i] = ipc->msg[BOOT_USB_SCRATCHPAD_BUF0_PADDR + i];
    }
    g_ep0_trring_paddr_base     = ipc->msg[BOOT_USB_EP0_TRRING_PADDR];
    g_ctrl_buf_paddr_base       = ipc->msg[BOOT_USB_CTRL_BUF_PADDR];
    g_bulkout_trring_paddr_base = ipc->msg[BOOT_USB_BULKOUT_TRRING_PADDR];
    g_bulkin_trring_paddr_base  = ipc->msg[BOOT_USB_BULKIN_TRRING_PADDR];
    g_cbw_csw_paddr_base        = ipc->msg[BOOT_USB_CBW_CSW_PADDR];
    g_bounce_paddr_base         = ipc->msg[BOOT_USB_BOUNCE_PADDR];
    // Фаза 15 — раскладываем базы на per-device paddr'ы, один раз, до
    // bring-up (см. struct UsbDeviceSlot/h/platform.h — страницы подряд,
    // idx-е по 4096 байт).
    for (int i = 0; i < USB_MAX_DEVICES; i++) {
        g_usb_devices[i].ep0_trring_paddr     = g_ep0_trring_paddr_base     + (seL4_Word)i * 4096;
        g_usb_devices[i].ctrl_buf_paddr       = g_ctrl_buf_paddr_base       + (seL4_Word)i * 4096;
        g_usb_devices[i].bulkout_trring_paddr = g_bulkout_trring_paddr_base + (seL4_Word)i * 4096;
        g_usb_devices[i].bulkin_trring_paddr  = g_bulkin_trring_paddr_base  + (seL4_Word)i * 4096;
        g_usb_devices[i].cbw_csw_paddr        = g_cbw_csw_paddr_base        + (seL4_Word)i * 4096;
        g_usb_devices[i].bounce_paddr         = g_bounce_paddr_base         + (seL4_Word)i * 4096;
    }

    g_xhci_base = (volatile uint8_t*)PLAT_XHCI_VADDR;
    g_pcie_rc_base = (volatile uint8_t*)PLAT_PCIE_RC_VADDR;

    if (LOG_USB) sys_puts(console_ep, "[USB] usb_driver: начинаю bring-up xHCI (Фаза 14).\n");

    // Milestone 8 (см. ROADMAP.md/план) — просим динамический SHM тем же
    // протоколом, что blk_driver.cpp (107=SYS_SHM_GET). case 6 в
    // shm_pages_mask_for_role() (main.cpp) выдаёт VFS(0)+staging(8). USB —
    // опциональный модуль (см. комментарий у SYS_DRIVER_READY в main.cpp)
    // — отказ здесь НЕ паркует процесс (в отличие от blk_driver): bring-up
    // ниже всё равно отработает, просто VFS-команды честно ответят ошибкой
    // (см. проверку g_shm_vaddr==nullptr в диспетчере ниже).
    seL4_SetMR(0, 107); // SYS_SHM_GET
    seL4_Call(root_ep, seL4_MessageInfo_new(0, 0, 0, 1));
    g_shm_vaddr = (char*)seL4_GetMR(0);
    if (!g_shm_vaddr) {
        sys_puts(console_ep, "[USB]   WARNING: SHM недоступен — VFS-команды (ls/cat/... на /mnt/usb0) работать не будут.\n");
    }

    // Двадцать первая попытка (см. ROADMAP.md) — Шаг 6 не получал Command
    // Completion Event: /soc/pcie dma-ranges в bcm2711-rpi-4-b.dts
    // ограничивает ВХОДЯЩИЙ DMA VL805 диапазоном CPU-адресов [0x0,
    // 0xC0000000) (3GiB) — normal_untyped (main.cpp) начинается с
    // 0x40000000 и растёт монотонно, к моменту выделения DMA-страниц
    // usb_driver'а (после uart/timer/blk/net/wifi) МОГ уйти выше 3GiB.
    // Печатаем реальные физические адреса колец ДО bring-up — если
    // окажутся >= 0xC0000000, это и есть причина: устройство не может
    // физически записать событие в нашу память.
    if (LOG_USB) sys_puthex64(console_ep, "[USB]   DIAG dcbaa_paddr   = ", g_dcbaa_paddr);
    if (LOG_USB) sys_puthex64(console_ep, "[USB]   DIAG cmdring_paddr = ", g_cmdring_paddr);
    if (LOG_USB) sys_puthex64(console_ep, "[USB]   DIAG evtring_paddr = ", g_evtring_paddr);
    // Фаза 15 (Milestone A1) — та же проверка раньше покрывала ТОЛЬКО
    // контроллерные Command/Event Ring — per-device DMA-регионы
    // (USB_MAX_DEVICES страниц каждый — ep0/ctrl/bulkout/bulkin/cbw-csw/
    // bounce) и Device Context (USB_MAX_SLOTS_ENABLED страниц) появились
    // позже (A1) и никогда не проверялись на этот же класс проблемы.
    // Найдено на живом железе — воспроизводимое зависание ИМЕННО на
    // первой bulk-передаче ВТОРОГО устройства (idx=1): base+idx*4096
    // монотонно растёт, поэтому ПОСЛЕДНИЙ idx/slot — заведомо наихудший
    // (не ниже остальных) случай, проверять остальные избыточно.
    UsbDeviceSlot &worst = g_usb_devices[USB_MAX_DEVICES - 1];
    seL4_Word worst_devctx = devctx_paddr_for((uint8_t)(USB_MAX_SLOTS_ENABLED - 1));
    if (LOG_USB) sys_puthex64(console_ep, "[USB]   DIAG bounce_paddr(idx=последний)         = ", worst.bounce_paddr);
    if (LOG_USB) sys_puthex64(console_ep, "[USB]   DIAG cbw_csw_paddr(idx=последний)        = ", worst.cbw_csw_paddr);
    if (LOG_USB) sys_puthex64(console_ep, "[USB]   DIAG bulkin_trring_paddr(idx=последний)  = ", worst.bulkin_trring_paddr);
    if (LOG_USB) sys_puthex64(console_ep, "[USB]   DIAG devctx_paddr(slot=последний)        = ", worst_devctx);
    bool dma_range_bad = (g_cmdring_paddr >= 0xC0000000ULL || g_evtring_paddr >= 0xC0000000ULL ||
                           worst.ep0_trring_paddr >= 0xC0000000ULL || worst.ctrl_buf_paddr >= 0xC0000000ULL ||
                           worst.bulkout_trring_paddr >= 0xC0000000ULL || worst.bulkin_trring_paddr >= 0xC0000000ULL ||
                           worst.cbw_csw_paddr >= 0xC0000000ULL || worst.bounce_paddr >= 0xC0000000ULL ||
                           worst_devctx >= 0xC0000000ULL);
    if (dma_range_bad) {
        sys_puts(console_ep, "[USB]   DIAG ПРЕДУПРЕЖДЕНИЕ: кольцо(а)/per-device DMA-регион(ы) >= 0xC0000000 — ВНЕ dma-ranges PCIe-моста (3GiB), устройство не сможет туда писать/читать.\n");
    }

    int found_count = run_bring_up(console_ep);
    if (found_count > 0) {
        if (LOG_USB) sys_puthex32(console_ep, "[USB] Bring-up завершён: перечислено устройств = ", (uint32_t)found_count);
    } else {
        if (LOG_USB) sys_puts(console_ep, "[USB] Bring-up завершён без перечисленного устройства (см. лог выше).\n");
    }

    // Сигналим готовность ВСЕГДА (успех или нет) — иначе rootserver навечно
    // ждал бы (если бы USB вообще входил в driver_ready[], чего нет — но
    // сама SYS_DRIVER_READY нужна, чтобы usb_driver появился в логе и
    // g_usb_driver_ready выставился, см. main.cpp).
    seL4_SetMR(0, SYS_DRIVER_READY);
    seL4_Call(root_ep, seL4_MessageInfo_new(0, 0, 0, 1));

    // Milestone 11 (доп., по запросу пользователя) — считаем тики
    // heartbeat'а, реально опрашиваем PORTSC раз в ~10 тиков. Общий
    // heartbeat в этом проекте тикает раз в 20мс (см. blk_driver.cpp,
    // SYS_TIMER_HEARTBEAT_SUBSCRIBE) — период ОБЩИЙ на весь процесс
    // timer_driver (см. common.h/timer_driver.cpp), usb_driver его не
    // трогает (не зовёт SYS_TIMER_HEARTBEAT_SUBSCRIBE сам, просто
    // получает уже идущие тики), поэтому 10×20мс≈200мс — как просил
    // пользователь, без изменения каданса для net/wifi/blk.
    constexpr int USB_HOTPLUG_POLL_EVERY_N_TICKS = 10;
    int heartbeat_tick_count = 0;

    while (1) {
        seL4_Word badge = 0;
        seL4_MessageInfo_t info = seL4_Recv(usb_cmd_ep, &badge);

        if (badge & USB_EVENT_HEARTBEAT) {
            // Фаза 3b плана "Сигналы драйверам" — "я жив", БЕЗУСЛОВНО (даже
            // если g_usb_stopped — см. тот же принцип в blk_driver.cpp).
            if (liveness_ntfn != 0) seL4_Signal(liveness_ntfn);
            // issuse.txt №15 — асинхронная последовательность подключения
            // за хабом (см. hub_conn_async_tick() выше) продвигается на
            // КАЖДОМ heartbeat-тике (~20мс), а не только раз в
            // USB_HOTPLUG_POLL_EVERY_N_TICKS (~200мс, см. ниже) — иначе
            // сама "асинхронность" искусственно тормозила бы уже начатое
            // подключение лишней гранулярностью поверх и так небыстрого
            // xHCI-обмена.
            hub_conn_async_tick(console_ep);
            if (++heartbeat_tick_count >= USB_HOTPLUG_POLL_EVERY_N_TICKS) {
                heartbeat_tick_count = 0;
                poll_ports_for_hotplug(console_ep);
                // Milestone B4 — тот же каданс (~200мс), что и опрос
                // корневых портов: динамический hot-plug ЗА хабом
                // (Interrupt-эндпоинт статуса downstream-портов).
                poll_hub_interrupts(console_ep);
            }
            if (!(badge & (USB_EVENT_XHCI_IRQ))) continue; // чистый heartbeat-тик, не реальный IRQ
        }

        if (badge & USB_EVENT_XHCI_IRQ) {
            // issuse.txt №15 — НАЙДЕНО НА ЖИВОМ ЖЕЛЕЗЕ: реальное xHCI-
            // прерывание от завершения control-transfer'а асинхронной
            // hub-connect-последовательности (см. hub_conn_async_tick()
            // выше) чаще всего приходит НА СВОЁМ бейдже, БЕЗ heartbeat-
            // бита (аппаратура отвечает за микросекунды-миллисекунды,
            // редко совпадая с 20мс-тиком) — а общий дренаж событий чуть
            // ниже слепо вычитывает и отбрасывает ЛЮБОЕ событие, кроме
            // Port Status Change (см. его же комментарий: "Transfer Event
            // уже обрабатываются синхронно... сюда попасть не должны" —
            // верно для СТАРОГО синхронного кода, но НЕ для нового
            // асинхронного, у которого именно такие события в очереди —
            // штатный случай). Без этого вызова здесь стейт-машина
            // практически никогда не видела свой Transfer Event сама
            // (общий дренаж успевал украсть его первым) и таймаутила по
            // 500мс на КАЖДОМ шаге — hw-подтверждённая находка.
            hub_conn_async_tick(console_ep);
            // Штатный событийный путь (после bring-up) — снять EINT,
            // дочитать Event Ring, отдать назад ERDP, Ack'нуть IRQ (см.
            // GENET-паттерн в net_driver.cpp — собственная линия, Ack сам).
            uint32_t sts = *reg32(g_op_base, XHCI_OP_USBSTS);
            if (sts & USBSTS_EINT) *reg32(g_op_base, XHCI_OP_USBSTS) = USBSTS_EINT; // RW1C
            Trb ev;
            // Milestone 11 (закрытие Фазы 14, hot-plug) — единственный
            // тип события, который нас интересует здесь: Port Status
            // Change (подключение/отключение). Command Completion и
            // Transfer Event уже обрабатываются синхронно своими
            // wait_command_completion()/wait_transfer_completion() в
            // момент самой транзакции (bring-up/SCSI) — сюда, в
            // асинхронный путь, они попасть не должны в норме, но если
            // всё же попадут (устройство прислало событие ПОСЛЕ того, как
            // синхронный ожидающий уже сдался по таймауту) — просто
            // молча дренируем, как и раньше.
            {
                int drained = 0;
                while (dequeue_event_trb(ev)) {
                    uint32_t ev_type = (ev.control & TRB_TYPE_MASK) >> TRB_TYPE_SHIFT;
                    if (ev_type == TRB_TYPE_PORT_STATUS_CHANGE_EVENT) {
                        uint8_t changed_port = (uint8_t)(ev.parameter >> 24);
                        handle_port_status_change(console_ep, changed_port);
                    }
                    if (++drained >= EVT_RING_DRAIN_SANITY_CAP) {
                        sys_puts(console_ep, "[USB]   ОШИБКА: дренаж event ring (XHCI_IRQ) превысил защитный потолок — обрываю.\n");
                        break;
                    }
                }
            }
            update_erdp();
            seL4_IRQHandler_Ack(ipc->msg[BOOT_IRQ_EP]);
            continue;
        }

        // План "Сигналы драйверам" — проверяется БЕЗУСЛОВНО, до какого-либо
        // g_usb_stopped гейта ниже: сигнал обязан доходить, даже если
        // драйвер уже остановлен (иначе STOP необратим без full respawn).
        if (seL4_MessageInfo_get_length(info) >= 1 && seL4_GetMR(0) == SYS_DRIVER_SIGNAL) {
            seL4_Word sig = seL4_GetMR(1);
            if (sig == DRIVER_SIGNAL_STOP) {
                g_usb_stopped = true;
            } else if (sig == DRIVER_SIGNAL_START) {
                g_usb_stopped = false;
            } else if (sig == DRIVER_SIGNAL_RESTART) {
                // Найдено на живом железе (первая hw-проверка Фазы 2):
                // гипотеза "xhci_controller_init()+enumerate идемпотентны"
                // была НЕВЕРНА — g_usb_devices[]-слоты, оставшиеся
                // in_use/storage_mounted от ДО-restart сессии, не
                // освобождались. HCRST внутри run_bring_up() убивает ВСЕ
                // xHCI Slot ID на контроллере, но find_free_device_slot()
                // всё равно считал старый слот занятым и монтировал то же
                // физическое устройство в ДРУГОЙ слот с НОВЫМ slot_id —
                // resolve_device_by_path() по имени тома находил ПЕРВЫЙ
                // (старый, с невалидным после HCRST slot_id) слот, и
                // bulk_transfer() дальше слал доорбелл несуществующему
                // слоту — timeout, а последующее Reset Endpoint/Set TR
                // Dequeue Pointer падали с Context State Error (0x13),
                // потому что сам Slot Context был Disabled. Фикс: тем же
                // путём, что и hot-unplug (unmount_usb_storage(), уже
                // hw-проверенная функция) освобождаем ВСЕ занятые слоты
                // ДО run_bring_up() — накопители И хабы (in_use покрывает
                // оба, см. её же комментарий), пока контроллер ещё в
                // ДО-restart состоянии и Disable Slot имеет шанс дойти
                // штатно (после HCRST это уже не нужно — слоты и так мертвы).
                for (int i = 0; i < USB_MAX_DEVICES; i++) {
                    if (g_usb_devices[i].in_use) unmount_usb_storage(console_ep, i);
                }
                int found_count = run_bring_up(console_ep);
                if (LOG_USB) sys_puthex32(console_ep, "[USB] Bring-up (signal RESTART) завершён: перечислено устройств = ", (uint32_t)found_count);
                g_usb_stopped = false;
            }
            seL4_SetMR(0, 0);
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
            continue;
        }
        if (g_usb_stopped && seL4_MessageInfo_get_length(info) >= 1 &&
            (seL4_GetMR(0) == 110 || seL4_GetMR(0) == 112 || seL4_GetMR(0) == 113 ||
             seL4_GetMR(0) == 114 || seL4_GetMR(0) == 116 || seL4_GetMR(0) == 117 ||
             seL4_GetMR(0) == 118 || seL4_GetMR(0) == 119 || seL4_GetMR(0) == 120)) {
            // Остановлен сигналом STOP — гейтим ТОЛЬКО VFS-командную
            // цепочку (110-120), как в плане; USB_CMD_LIST/LIST_VOLUMES/
            // GET_ALL_SPACE (1/3/4) — безобидный readonly-опрос статуса
            // g_usb_devices[], не трогает железо, отвечаем как обычно.
            // cmd==119 (SYS_READ_FILE) отдельно — у него wire-формат
            // ответа 2 слова (status, bytes_read), как в ветке
            // dispatch_idx<0 ниже.
            if (seL4_GetMR(0) == 119) {
                seL4_SetMR(0, (seL4_Word)-1); seL4_SetMR(1, 0);
                seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 2));
            } else {
                seL4_SetMR(0, (seL4_Word)-1);
                seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
            }
            continue;
        }

        if (seL4_MessageInfo_get_length(info) >= 1 && seL4_GetMR(0) == 1 /* USB_CMD_LIST */) {
            int list_idx = first_found_device_idx(); // Milestone A1 — временно первое найденное, см. комментарий выше
            if (list_idx < 0) {
                seL4_SetMR(0, 0);
                seL4_SetMR(1, 0); seL4_SetMR(2, 0); seL4_SetMR(3, 0); seL4_SetMR(4, 0); seL4_SetMR(5, 0);
            } else {
                UsbFoundDevice &f = g_usb_devices[list_idx].found;
                seL4_SetMR(0, 1);
                seL4_SetMR(1, f.vendor_id);
                seL4_SetMR(2, f.product_id);
                seL4_SetMR(3, f.device_class);
                seL4_SetMR(4, f.device_subclass);
                seL4_SetMR(5, f.device_protocol);
            }
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 6));
            continue;
        }

        // Milestone A3 (Фаза 15) — заменяет USB_CMD_GET_VOLUME_NAME
        // (Milestone 10, возвращал имя только ПЕРВОГО смонтированного
        // устройства) — теперь битовая маска смонтированных устройств
        // (бит i = g_usb_devices[i].storage_mounted) + имя КАЖДОГО слота
        // (4 регистра на слот, тот же приём упаковки строки в регистры,
        // что уже использует SYS_EXEC для имени/cwd), даже для
        // несмонтированных — клиент смотрит только на выставленные в
        // маске биты, не обязан отдельно фильтровать пустые имена.
        // ls.cpp вызывает это ОДИН раз при листинге "/mnt", печатает
        // "[DIR] <имя>" на каждый установленный бит.
        if (seL4_MessageInfo_get_length(info) >= 1 && seL4_GetMR(0) == 3 /* USB_CMD_LIST_VOLUMES */) {
            seL4_Word mask = 0;
            for (int i = 0; i < USB_MAX_DEVICES; i++) if (g_usb_devices[i].storage_mounted) mask |= (1u << i);
            seL4_SetMR(0, mask);
            for (int i = 0; i < USB_MAX_DEVICES; i++) {
                seL4_Word *words = (seL4_Word*)g_usb_devices[i].volume_name;
                for (int w = 0; w < 4; w++) seL4_SetMR(1 + i * 4 + w, words[w]);
            }
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1 + 4 * USB_MAX_DEVICES));
            continue;
        }

        // Фаза 8 (мониторинг ресурсов, `df`) — та же маска смонтированных
        // слотов, что USB_CMD_LIST_VOLUMES, но на слот вместо одной строки
        // имени пакуем имя(4 слова)+total_bytes(1)+free_bytes(1) — ОДИН
        // атомарный вызов вместо двух round-trip'ов (имя отдельно,
        // занятость отдельно), чтобы root не пришлось сопоставлять два
        // снимка состояния, которое может измениться между вызовами
        // (устройство отмонтировалось между первым и вторым запросом).
        if (seL4_MessageInfo_get_length(info) >= 1 && seL4_GetMR(0) == 4 /* USB_CMD_GET_ALL_SPACE */) {
            seL4_Word mask = 0;
            for (int i = 0; i < USB_MAX_DEVICES; i++) if (g_usb_devices[i].storage_mounted) mask |= (1u << i);
            seL4_SetMR(0, mask);
            for (int i = 0; i < USB_MAX_DEVICES; i++) {
                seL4_Word *words = (seL4_Word*)g_usb_devices[i].volume_name;
                int base = 1 + i * 6;
                for (int w = 0; w < 4; w++) seL4_SetMR(base + w, words[w]);
                uint64_t total = 0, free_bytes = 0;
                if (g_usb_devices[i].storage_mounted) exfat_free_space(&g_usb_devices[i].fs, &total, &free_bytes);
                seL4_SetMR(base + 4, (seL4_Word)total);
                seL4_SetMR(base + 5, (seL4_Word)free_bytes);
            }
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1 + 6 * USB_MAX_DEVICES));
            continue;
        }

        // Milestone 8 (Фаза 14, закрытие) — собственный VFS-диспетчер,
        // зеркалит числовые команды и wire-формат blk_driver.cpp один-в-
        // один (110/112/113/114/116/117/118/119/120), чтобы Milestone 9
        // (маршрутизация /mnt/usb0 в shell/sys_client.h) могла просто
        // переслать вызов на usb_storage_ep без переписывания ответа —
        // ls.cpp/cat.cpp/touch.cpp/... не будут знать, с каким драйвером
        // говорят. g_shm_vaddr==nullptr (SHM недоступен) или
        // !g_usb_storage_mounted (не смонтировано/флешки нет) — честная
        // ошибка вместо зависания/null-деref.
        seL4_Word cmd = (seL4_MessageInfo_get_length(info) >= 1) ? seL4_GetMR(0) : (seL4_Word)-1;
        // Milestone A3 (Фаза 15) — путь(и) приходят с ведущим компонентом =
        // имя тома (см. route_vfs_path() в shell.cpp/h/sys_client.h —
        // клиент срезает только "/mnt"), resolve_device_by_path() сам
        // находит нужное устройство и срезает "/<имя>" прямо в буфере.
        int dispatch_idx = -1;

        if (cmd == 110 || cmd == 112 || cmd == 113 || cmd == 114 ||
            cmd == 116 || cmd == 117 || cmd == 118 || cmd == 119 || cmd == 120) {
            if (!g_shm_vaddr) {
                seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 0)); // нет SHM вообще — как неизвестная команда
                continue;
            }
            if (cmd == 116) {
                // SYS_RENAME — ДВА пути (offset 0 и 128, см. ниже), оба
                // ОБЯЗАНЫ резолвиться в ОДНО и то же устройство —
                // authoritative-проверка (sbin/mv.cpp уже подсказывает то
                // же самое сравнением имён томов ДО этого вызова, но
                // финальное слово за сервером, не за клиентом).
                int idx_old = resolve_device_by_path(g_shm_vaddr);
                int idx_new = resolve_device_by_path(g_shm_vaddr + 128);
                dispatch_idx = (idx_old >= 0 && idx_old == idx_new) ? idx_old : -1;
            } else {
                dispatch_idx = resolve_device_by_path(g_shm_vaddr);
            }
            if (dispatch_idx < 0) {
                if (cmd == 119) { seL4_SetMR(0, (seL4_Word)-1); seL4_SetMR(1, 0); seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 2)); }
                else if (cmd == 110) { my_strcpy(g_shm_vaddr, "ls: no such USB volume (not mounted?)\n"); seL4_SetMR(0, 0); seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1)); }
                else { seL4_SetMR(0, (seL4_Word)-1); seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1)); }
                continue;
            }
            // Милстоун 7 монтирует /mnt/usb0 ЯВНО только для чтения (см. лог
            // "exFAT смонтирован (только чтение)"), но диспетчер (Milestone 8)
            // был скопирован с blk_driver.cpp буквально, включая мутирующие
            // команды — RPI4_USB_ALLOW_WRITE проверялась только ВНУТРИ
            // hardware_usb_write(), уже ПОСЛЕ того, как exfat.cpp успевал
            // пройти bitmap/directory-курсорную часть записи. У blk_driver'а
            // этот код-путь (запись, гарантированно проваливающаяся с самого
            // начала) НИКОГДА не исполнялся на живом железе — там запись
            // всегда разрешена (RPI4_EMMC_ALLOW_WRITE=true). На реальном
            // железе `touch` на /mnt/usb0 зависал (см. ROADMAP.md/лог) —
            // подозрение на непротестированный код-путь внутри exfat.cpp.
            // Перехватываем мутирующие команды ЗДЕСЬ, ДО exfat.cpp, пока флаг
            // выключен — честная быстрая ошибка вместо входа в код, который
            // никогда не проверялся на этом железе. Реальная запись —
            // Milestone 10, тестируется отдельно вместе с этим кодом-путём.
            if (!RPI4_USB_ALLOW_WRITE && (cmd == 112 || cmd == 113 || cmd == 116 || cmd == 117 || cmd == 120)) {
                seL4_SetMR(0, (seL4_Word)-1);
                seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                continue;
            }
        }
        // dispatch_idx гарантированно >= 0 здесь для ЛЮБОГО cmd из набора
        // VFS-команд (иначе уже сделали continue выше) — индекс 0 в
        // остальных случаях никогда не читается (cmd не совпадёт ни с
        // одной веткой ниже, см. финальный else).
        EXFAT_Instance &fs = g_usb_devices[dispatch_idx >= 0 ? dispatch_idx : 0].fs;

        // issuse.txt №69 — reply-капа текущего клиента откладывается в
        // фиксированный слот СВОЕГО CNode на всё время обработки этой
        // команды (см. usb_vfs_reply()/VFS_PENDING_REPLY_SLOT выше) — та же
        // защита, что у blk_driver.cpp. Каждая ветка ниже отвечает через
        // usb_vfs_reply(), а не напрямую seL4_Reply().
        seL4_CNode_SaveCaller(SELF_CNODE_SLOT, VFS_PENDING_REPLY_SLOT, 8);

        if (cmd == 110) { // SYS_LS
            char path[256]; // issuse.txt №42: exFAT-имя до 255 символов, был 64
            my_strlcpy(path, g_shm_vaddr, sizeof(path));

            uint32_t dir_cluster;
            if (path[0] == '\0') {
                dir_cluster = fs.current_dir_cluster;
                if (dir_cluster == 0) dir_cluster = fs.root_cluster;
            } else {
                char basename[256]; // issuse.txt №42
                uint32_t parent_clus = exfat_resolve_parent(&fs, path, basename);
                if (parent_clus == 0xFFFFFFFF) {
                    my_strcpy(g_shm_vaddr, "ls: path not found\n");
                    seL4_SetMR(0, 0);
                    usb_vfs_reply(seL4_MessageInfo_new(0, 0, 0, 1));
                    continue;
                }
                if (basename[0] == '\0') {
                    dir_cluster = parent_clus;
                } else {
                    bool is_dir = false;
                    dir_cluster = exfat_find_in_dir(&fs, parent_clus, basename, &is_dir);
                    // issuse.txt №36: раньше ls на обычном файле обходил его
                    // содержимое как таблицу каталога вместо явной ошибки.
                    if (dir_cluster != 0xFFFFFFFF && !is_dir) {
                        my_strcpy(g_shm_vaddr, "ls: not a directory\n");
                        seL4_SetMR(0, 0);
                        usb_vfs_reply(seL4_MessageInfo_new(0, 0, 0, 1));
                        continue;
                    }
                }
            }

            if (dir_cluster == 0xFFFFFFFF) {
                my_strcpy(g_shm_vaddr, "ls: directory not found\n");
                seL4_SetMR(0, 0);
                usb_vfs_reply(seL4_MessageInfo_new(0, 0, 0, 1));
                continue;
            }
            if (dir_cluster == 0) dir_cluster = fs.root_cluster;

            exfat_format_dir_listing(&fs, dir_cluster, g_shm_vaddr, 0x1000 - 8);
            seL4_SetMR(0, 0);
            usb_vfs_reply(seL4_MessageInfo_new(0, 0, 0, 1));
        }
        else if (cmd == 119) { // SYS_READ_FILE
            uint32_t offset = seL4_GetMR(1);
            uint32_t bytes_read = 0;

            char filename[256]; // issuse.txt №42
            my_strlcpy(filename, g_shm_vaddr, sizeof(filename));

            bool success = exfat_read_file(&fs, filename, g_shm_vaddr, offset, &bytes_read);

            if (success) {
                seL4_SetMR(0, 0);
                seL4_SetMR(1, bytes_read);
            } else {
                seL4_SetMR(0, (seL4_Word)-1);
                seL4_SetMR(1, 0);
            }
            usb_vfs_reply(seL4_MessageInfo_new(0, 0, 0, 2));
        }
        else if (cmd == 112) { // SYS_TOUCH
            char path[256]; // issuse.txt №42: exFAT-имя до 255 символов, был 64
            my_strlcpy(path, g_shm_vaddr, sizeof(path));
            bool existed = false;
            if (exfat_create_file(&fs, path, &existed)) seL4_SetMR(0, existed ? 1 : 0);
            else seL4_SetMR(0, (seL4_Word)-1);
            usb_vfs_reply(seL4_MessageInfo_new(0, 0, 0, 1));
        }
        else if (cmd == 113) { // SYS_WRITE_FILE (echo > file) — RPI4_USB_ALLOW_WRITE=false, см. hardware_usb_write()
            char path[256]; // issuse.txt №42: exFAT-имя до 255 символов, был 64
            my_strlcpy(path, g_shm_vaddr, sizeof(path));
            uint32_t len = seL4_GetMR(1);
            if (len > 4096) len = 4096;

            char* safe_text_buf = g_shm_vaddr + USB_SHM_STAGING_OFFSET;
            for (int i = 0; i < 4096; i++) safe_text_buf[i] = 0;
            my_memcpy(safe_text_buf, g_shm_vaddr + 128, (int)len);

            if (exfat_write_file(&fs, path, safe_text_buf, len)) seL4_SetMR(0, 0);
            else seL4_SetMR(0, (seL4_Word)-1);
            usb_vfs_reply(seL4_MessageInfo_new(0, 0, 0, 1));
        }
        else if (cmd == 114) { // SYS_READ_TEXT_FILE (cat)
            char path[256]; // issuse.txt №42: exFAT-имя до 255 символов, был 64
            my_strlcpy(path, g_shm_vaddr, sizeof(path));

            uint32_t copied = 0;
            if (exfat_read_text_file(&fs, path, g_shm_vaddr, &copied)) {
                seL4_SetMR(0, 0);
                seL4_SetMR(1, copied); // issuse.txt №56: реальный размер, cat сверяет со strlen()
                usb_vfs_reply(seL4_MessageInfo_new(0, 0, 0, 2));
            } else {
                seL4_SetMR(0, (seL4_Word)-1);
                usb_vfs_reply(seL4_MessageInfo_new(0, 0, 0, 1));
            }
        }
        else if (cmd == 120) { // SYS_RM
            char path[256]; // issuse.txt №42: exFAT-имя до 255 символов, был 64
            my_strlcpy(path, g_shm_vaddr, sizeof(path));
            if (exfat_delete_file(&fs, path)) seL4_SetMR(0, 0);
            else seL4_SetMR(0, (seL4_Word)-1);
            usb_vfs_reply(seL4_MessageInfo_new(0, 0, 0, 1));
        }
        else if (cmd == 116) { // SYS_RENAME (mv)
            char old_p[256], new_p[256]; // issuse.txt №35/№42: было 32, обрезало длинные имена
            my_strlcpy(old_p, g_shm_vaddr, sizeof(old_p));
            my_strlcpy(new_p, g_shm_vaddr + 128, sizeof(new_p));
            if (exfat_rename_file(&fs, old_p, new_p)) seL4_SetMR(0, 0);
            else seL4_SetMR(0, (seL4_Word)-1);
            usb_vfs_reply(seL4_MessageInfo_new(0, 0, 0, 1));
        }
        else if (cmd == 117) { // SYS_MKDIR
            char path[256]; // issuse.txt №42: exFAT-имя до 255 символов, был 64
            my_strlcpy(path, g_shm_vaddr, sizeof(path));
            bool existed = false;
            if (exfat_mkdir(&fs, path, &existed)) seL4_SetMR(0, 0);
            else seL4_SetMR(0, existed ? 1 : (seL4_Word)-1);
            usb_vfs_reply(seL4_MessageInfo_new(0, 0, 0, 1));
        }
        else if (cmd == 118) { // SYS_CD
            char path[256]; // issuse.txt №42: exFAT-имя до 255 символов, был 64
            my_strlcpy(path, g_shm_vaddr, sizeof(path));
            if (exfat_cd(&fs, path)) seL4_SetMR(0, 0);
            else seL4_SetMR(0, (seL4_Word)-1);
            usb_vfs_reply(seL4_MessageInfo_new(0, 0, 0, 1));
        }
        else {
            usb_vfs_reply(seL4_MessageInfo_new(0, 0, 0, 0)); // неизвестная команда — не подвешиваем вызывающего
        }
    }

    return 0;
}
