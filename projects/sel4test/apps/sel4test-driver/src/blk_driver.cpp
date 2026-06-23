#include <sel4/sel4.h>
#include "common.h"
#include <stdint.h>

#define VIRTIO_MMIO_BASE 0x200004000ULL


// В будущем эти адреса должны получаться через BootInfo
#define VFS_SHM_VIRT_BASE 0x502000ULL
#define VIRTIO_Q_SHM_VIRT_BASE (VFS_SHM_VIRT_BASE + 0x1000)

// ИСПРАВЛЕНО: Физический адрес для DMA теперь получается динамически от ядра,
// а не хардкодится.
static uint64_t g_vfs_shm_phys_base = 0;

void __assert_fail(const char *expr, const char *file, int line, const char *func) {
    // Если внутри макросов seL4 произойдет ошибка, просто тихо висим
    while (1) {
    }
}

static void my_memcpy(void *dest, const void *src, int n) {
    unsigned char *d = (unsigned char *)dest;
    const unsigned char *s = (const unsigned char *)src;
    while (n--) *d++ = *s++;
}
static int my_strlen(const char* s) { int len = 0; while (s[len]) len++; return len; }
static void my_strcpy(char *dest, const char *src) { while ((*dest++ = *src++)); }
static int my_strcmp(const char *s1, const char *s2) {
    while (*s1 && (*s1 == *s2)) { s1++; s2++; }
    return *(const unsigned char*)s1 - *(const unsigned char*)s2;
}
static int my_strncmp(const char *s1, const char *s2, int n) {
    while (n && *s1 && (*s1 == *s2)) { s1++; s2++; n--; }
    if (n == 0) return 0;
    return *(const unsigned char*)s1 - *(const unsigned char*)s2;
}

// Умный конвертер строк в FAT32 формат (8.3) с поддержкой дубликатов (1), (2)...
static void to_fat32_name(const char* input, int counter, char* output) {
    for (int i = 0; i < 11; i++) output[i] = ' '; // Заполняем пробелами
    output[11] = '\0';
    
    int in_idx = 0;
    int out_idx = 0;
    
    // Копируем базовое имя
    while (input[in_idx] && input[in_idx] != '.' && out_idx < 8) {
        char c = input[in_idx++];
        if (c >= 'a' && c <= 'z') c -= 32; // Перевод в UPPERCASE
        output[out_idx++] = c;
    }
    
    // Приклеиваем счетчик (1), (2), (3)...
    if (counter > 0) {
        char num[4];
        int n = counter, len = 0;
        while (n > 0) { num[len++] = (n % 10) + '0'; n /= 10; }
        
        int needed = len + 2; // Место под скобки и цифру
        if (out_idx + needed > 8) {
            out_idx = 8 - needed; // Обрезаем имя, чтобы влез суффикс!
        }
        
        output[out_idx++] = '(';
        for (int i = len - 1; i >= 0; i--) output[out_idx++] = num[i];
        output[out_idx++] = ')';
    }
    
    // Ищем расширение после точки
    while (input[in_idx] && input[in_idx] != '.') in_idx++;
    if (input[in_idx] == '.') {
        in_idx++; // Пропускаем саму точку
        out_idx = 8;
        while (input[in_idx] && out_idx < 11) {
            char c = input[in_idx++];
            if (c >= 'a' && c <= 'z') c -= 32;
            output[out_idx++] = c;
        }
    }
}

static char* my_strchr(const char *s, int c) {
    while (*s) { if (*s == (char)c) return (char*)s; s++; }
    return nullptr;
}
static void my_memset(void *s, int c, int n) {
    unsigned char* p = (unsigned char*)s;
    while (n--) *p++ = (unsigned char)c;
}

static inline seL4_IPCBuffer* get_local_ipc() {
    seL4_Word tls_addr;
    // Добавлена буква 'ro'. crt0 не мог его стереть!
    asm volatile("mrs %0, tpidrro_el0" : "=r"(tls_addr)); 
    return (seL4_IPCBuffer*)(tls_addr - 1024);
}

// --- СТРОГИЕ СМЕЩЕНИЯ VIRTIO (Legacy Spec) ---
#define VQ_DESC_OFFSET      0x000   // 16 * 16 = 256 байт
#define VQ_AVAIL_OFFSET     0x100   // 38 байт
#define VQ_USED_OFFSET      0x140   // Выровнено по 64 байтам (queue_align)
#define BLK_REQ_OFFSET      0x200   // Произвольное безопасное место
#define BLK_STATUS_OFFSET   0x220   // Произвольное безопасное место

// ИСПРАВЛЕНО: Безопасная отправка строк любой длины (Chunking)
static void sys_puts(seL4_CPtr console_ep, const char *str) {
    seL4_IPCBuffer *ipc = get_local_ipc();
    int total_len = my_strlen(str);
    int offset = 0;
    
    while (offset < total_len) {
        int chunk = total_len - offset;
        if (chunk > 100) chunk = 100; // Безопасный лимит Message Registers
        
        ipc->msg[0] = 8; // SYS_PUTS ID
        for (int i = 0; i < chunk; i++) {
            ipc->msg[i + 1] = str[offset + i];
        }
        seL4_Call(console_ep, seL4_MessageInfo_new(0, 0, 0, chunk + 1));
        offset += chunk;
    }
}

static void my_strncpy(char *dest, const char *src, int n) {
    int i;
    for (i = 0; i < n && src[i] != '\0'; i++) {
        dest[i] = src[i];
    }
    for ( ; i < n; i++) {
        dest[i] = '\0';
    }
}

// Макросы-перехватчики
#define strlen my_strlen
#define strcpy my_strcpy
#define strncpy my_strncpy
#define strcmp my_strcmp
#define strncmp my_strncmp
#define strchr my_strchr
#define memset my_memset
#define memcpy my_memcpy


// ==========================================
// VFS 2.0: Virtual File System Registry
// ==========================================
struct VfsNode {
    bool active;
    char path[64];  // Полный абсолютный путь (например, "/home" или "/uart_driver")
    bool is_dir;    // Это папка или файл?
    bool is_rom;    // Защита от записи (встроенные драйверы)
    char data[256]; // Буфер для хранения текста файла
    int size;       // Размер файла
};

static VfsNode vfs[128];
static int vfs_count = 0;

struct __attribute__((packed)) FAT32_BPB {
    uint8_t  jmp[3];            // Jump code
    char     oem_name[8];       // OEM Name
    uint16_t bytes_per_sector;  // Обычно 512
    uint8_t  sectors_per_cluster;// Секторов в кластере
    uint16_t reserved_sectors;  // Зарезервированные сектора (здесь лежит сам Boot Sector)
    uint8_t  fat_count;         // Количество таблиц FAT (обычно 2)
    uint16_t dir_entries;       // Для FAT32 всегда 0
    uint16_t total_sectors_16;  // Для FAT32 всегда 0
    uint8_t  media_descriptor;  // Тип носителя
    uint16_t fat_size_16;       // Для FAT32 всегда 0
    uint16_t sectors_per_track; 
    uint16_t heads;
    uint32_t hidden_sectors;
    uint32_t total_sectors_32;  // Всего секторов на диске
    
    // Специфика FAT32 начинается отсюда:
    uint32_t fat_size_32;       // Размер одной таблицы FAT в секторах
    uint16_t ext_flags;
    uint16_t fs_version;
    uint32_t root_cluster;      // Номер первого кластера корневой директории! (Обычно 2)
    uint16_t fs_info;
    uint16_t backup_boot_sector;
    uint8_t  reserved[12];
    uint8_t  drive_number;
    uint8_t  reserved1;
    uint8_t  boot_signature;    // Должно быть 0x28 или 0x29
    uint32_t volume_id;
    char     volume_label[11];  // Метка тома (например "NO NAME    ")
    char     fs_type[8];        // Тип ФС (строка "FAT32   ")
};

struct __attribute__((packed)) FAT32_DirEntry {
    uint8_t  name[11];      // Имя (8 байт) + Расширение (3 байта) без точки!
    uint8_t  attr;          // Атрибуты (Скрытый, Системный, Папка и т.д.)
    uint8_t  nt_res;
    uint8_t  crt_time_tenth;
    uint16_t crt_time;
    uint16_t crt_date;
    uint16_t lst_acc_date;
    uint16_t fst_clus_hi;   // Старшие 16 бит номера кластера файла
    uint16_t wrt_time;
    uint16_t wrt_date;
    uint16_t fst_clus_lo;   // Младшие 16 бит номера кластера файла
    uint32_t file_size;     // Размер файла в байтах
};

struct VirtioMmioRegs {
    uint32_t magic_value;        // 0x000 (Должно быть 0x74726976)
    uint32_t version;            // 0x004
    uint32_t device_id;          // 0x008 (Должно быть 2)
    uint32_t vendor_id;          // 0x00c
    uint32_t host_features;      // 0x010
    uint32_t host_features_sel;  // 0x014
    uint32_t reserved_1[2];      // 0x018 - 0x01c (пропуск)
    uint32_t guest_features;     // 0x020
    uint32_t guest_features_sel; // 0x024
    uint32_t guest_page_size;    // 0x028
    uint32_t reserved_2;         // 0x02c
    uint32_t queue_sel;          // 0x030
    uint32_t queue_num_max;      // 0x034
    uint32_t queue_num;          // 0x038
    uint32_t queue_align;        // 0x03c
    uint32_t queue_pfn;          // 0x040
    uint32_t reserved_3[3];      // 0x044
    uint32_t queue_notify;       // 0x050
    uint32_t reserved_4[3];      // 0x054
    uint32_t interrupt_status;   // 0x060
    uint32_t interrupt_ack;      // 0x064
    uint32_t reserved_5[2];      // 0x068
    uint32_t status;             // 0x070 (Тот самый регистр!)
};

// ==========================================
// Глобальное состояние FAT32
// ==========================================
static char* fat32_image_ptr = nullptr;
static volatile VirtioMmioRegs* g_disk_regs = nullptr;
static uint16_t g_vq_avail_idx = 0;

// Структуры очереди команд Virtio
struct virtq_desc { uint64_t addr; uint32_t len; uint16_t flags; uint16_t next; };
struct virtq_avail { uint16_t flags; uint16_t idx; uint16_t ring[16]; };
struct virtq_used_elem { uint32_t id; uint32_t len; };
struct virtq_used { uint16_t flags; uint16_t idx; virtq_used_elem ring[16]; };
struct virtio_blk_req { uint32_t type; uint32_t reserved; uint64_t sector; };

// Функция-помощник: скачивает любой сектор с диска в нашу память
static void virtio_read_sector(uint64_t sector, uint32_t dest_offset, uint32_t len) {
    volatile virtq_desc* vq_desc = (volatile virtq_desc*)(VIRTIO_Q_SHM_VIRT_BASE + VQ_DESC_OFFSET);
    volatile virtq_avail* vq_avail = (volatile virtq_avail*)(VIRTIO_Q_SHM_VIRT_BASE + VQ_AVAIL_OFFSET);
    volatile virtq_used* vq_used = (volatile virtq_used*)(VIRTIO_Q_SHM_VIRT_BASE + VQ_USED_OFFSET);
    volatile virtio_blk_req* blk_req = (volatile virtio_blk_req*)(VIRTIO_Q_SHM_VIRT_BASE + BLK_REQ_OFFSET);
    volatile uint8_t* blk_status = (volatile uint8_t*)(VIRTIO_Q_SHM_VIRT_BASE + BLK_STATUS_OFFSET);

    // Подготовка запроса
    blk_req->sector = sector;
    *blk_status = 0xFF; 
    
    vq_desc[1].addr = g_vfs_shm_phys_base + dest_offset; // Данные по-прежнему пишутся в общую память
    vq_desc[1].len = len;

    vq_avail->ring[g_vq_avail_idx % 16] = 0; 
    g_vq_avail_idx++;

    // 1. Публикуем индекс в кольце с барьером RELEASE
    // Это гарантирует, что QEMU не увидит новый индекс, пока не запишутся дескрипторы
    __atomic_store_n(&vq_avail->idx, g_vq_avail_idx, __ATOMIC_RELEASE);

    // ИСПРАВЛЕНО: Используем динамический указатель на регистры найденного устройства, а не хардкод.
    g_disk_regs->queue_notify = 0;

    // 3. Ожидание ответа от хоста с барьером ACQUIRE
    // Это гарантирует, что мы не прочитаем данные в буфере до того, как обновится индекс used
    while (__atomic_load_n(&vq_used->idx, __ATOMIC_ACQUIRE) < g_vq_avail_idx) { 
        seL4_Yield(); 
    } 
}

// Функция-помощник: записывает данные из нашей памяти на физический диск!
static void virtio_write_sector(uint64_t sector, uint32_t src_offset, uint32_t len) {
    volatile virtq_desc* vq_desc = (volatile virtq_desc*)(VIRTIO_Q_SHM_VIRT_BASE + VQ_DESC_OFFSET);
    volatile virtq_avail* vq_avail = (volatile virtq_avail*)(VIRTIO_Q_SHM_VIRT_BASE + VQ_AVAIL_OFFSET);
    volatile virtq_used* vq_used = (volatile virtq_used*)(VIRTIO_Q_SHM_VIRT_BASE + VQ_USED_OFFSET);
    volatile virtio_blk_req* blk_req = (volatile virtio_blk_req*)(VIRTIO_Q_SHM_VIRT_BASE + BLK_REQ_OFFSET);
    volatile uint8_t* blk_status = (volatile uint8_t*)(VIRTIO_Q_SHM_VIRT_BASE + BLK_STATUS_OFFSET);

    // ВАЖНО: 1 = VIRTIO_BLK_T_OUT (Запись на диск)
    blk_req->type = 1; 
    blk_req->reserved = 0;
    blk_req->sector = sector;
    *blk_status = 0xFF; 
    
    // Дескриптор 0 (Заголовок: Железо ЧИТАЕТ тип запроса)
    vq_desc[0].addr = (g_vfs_shm_phys_base + 0x1000) + BLK_REQ_OFFSET;
    vq_desc[0].len = sizeof(virtio_blk_req);
    vq_desc[0].flags = 1; // NEXT
    vq_desc[0].next = 1;

    // Дескриптор 1 (Данные: Железо ЧИТАЕТ из нашей RAM, чтобы записать на диск)
    vq_desc[1].addr = g_vfs_shm_phys_base + src_offset; // Данные берутся из общей памяти
    vq_desc[1].len = len;
    vq_desc[1].flags = 1; // ТОЛЬКО NEXT! (Флага WRITE больше нет)
    vq_desc[1].next = 2;

    // Дескриптор 2 (Статус: Железо ПИШЕТ статус операции 0 в нашу RAM)
    vq_desc[2].addr = (g_vfs_shm_phys_base + 0x1000) + BLK_STATUS_OFFSET;
    vq_desc[2].len = 1;
    vq_desc[2].flags = 2; // WRITE
    vq_desc[2].next = 0;

    // Сообщаем диску о новой задаче
    vq_avail->ring[g_vq_avail_idx % 16] = 0; 
    g_vq_avail_idx++;
    __atomic_store_n(&vq_avail->idx, g_vq_avail_idx, __ATOMIC_RELEASE);

    g_disk_regs->queue_notify = 0; // ПИНГУЕМ ДИСК НА ЗАПИСЬ!

    while (vq_used->idx < g_vq_avail_idx) {
        seL4_Yield(); // Ждем окончания физической записи
    }

    // ВОЗВРАЩАЕМ настройки дескрипторов по умолчанию для будущих операций ЧТЕНИЯ (cat, ls)
    blk_req->type = 0; 
    vq_desc[1].flags = 1 | 2; // NEXT | WRITE
}

static FAT32_BPB* fat32_bpb = nullptr;
static uint32_t fat32_first_data_sector = 0;
static uint32_t fat32_root_offset = 0;

static void put_hex(seL4_CPtr console_ep, uint32_t val) {
    char hex_chars[] = "0123456789ABCDEF";
    char buffer[11];
    buffer[0] = '0'; buffer[1] = 'x';
    for (int i = 0; i < 8; i++) {
        buffer[9 - i] = hex_chars[(val >> (i * 4)) & 0xF];
    }
    buffer[10] = '\0';
    sys_puts(console_ep, buffer);
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
    seL4_CPtr root_ep       = ipc->msg[BOOT_ROOT_EP];
    seL4_CPtr console_ep    = ipc->msg[BOOT_CONSOLE_EP];
    seL4_CPtr my_ep         = ipc->msg[7]; // BOOT_BLK_EP

    if (my_ep == 0) {
        __assert_fail("FATAL: Null Capability #0 Detected!", __FILE__, __LINE__, __func__);
    }

    sys_puts(console_ep, "\n[BLK DRIVER] VFS & FAT32 Server Online!\n");

    // ИСПРАВЛЕНО: Запрашиваем у ядра истинный физический адрес SHM для DMA
    sys_puts(console_ep, "[BLK DRIVER] Requesting physical SHM address from kernel...\n");
    seL4_MessageInfo_t shm_info = seL4_MessageInfo_new(0, 0, 0, 1);
    seL4_SetMR(0, 107); // SYS_SHM_GET
    seL4_Call(root_ep, shm_info);
    g_vfs_shm_phys_base = seL4_GetMR(1);
    if (g_vfs_shm_phys_base == 0) {
        sys_puts(console_ep, "[BLK DRIVER] FATAL: Failed to get physical SHM address!\n");
        while(1);
    }

    // ИСПРАВЛЕНИЕ: Жестко обнуляем 1 страницу памяти Virtio Queues (Control Plane),
    // чтобы стереть остатки от предыдущего упавшего драйвера и избежать
    // рассинхронизации с QEMU при перезапуске.
    my_memset((void*)VIRTIO_Q_SHM_VIRT_BASE, 0, 4096);

    // Сбрасываем программный счетчик индекса
    g_vq_avail_idx = 0;

    // ==========================================
    // ГЛОБАЛЬНЫЙ ПОИСК ДИСКА (32 СЛОТА)
    // ==========================================
    sys_puts(console_ep, "[VIRTIO] Scanning all 32 slots...\n");
    
    // Используем нашу глобальную переменную
    g_disk_regs = nullptr;

    for (int i = 0; i < 32; i++) {
        uintptr_t slot_addr = 0x200004000ULL + (i * 0x200);
        volatile VirtioMmioRegs* regs = (volatile VirtioMmioRegs*)slot_addr;
        
        if (regs->magic_value == 0x74726976) {
            uint32_t id = regs->device_id;
            if (id == 2) {
                // ИСПРАВЛЕНО: Объединяем вывод в один буфер, чтобы избежать "склеивания" логов
                char msg_buf[128];
                strcpy(msg_buf, "[VIRTIO] Found Block Device at Slot ");
                char* cursor = msg_buf + strlen(msg_buf);
                char num_buf[4] = {(char)((i/10)+'0'), (char)((i%10)+'0'), '\0', '\0'};
                strcpy(cursor, num_buf);
                cursor += strlen(num_buf);
                strcpy(cursor, " SUCCESS!\n");
                sys_puts(console_ep, msg_buf);
                
                // ЗАПИСЫВАЕМ В ГЛОБАЛЬНУЮ ПЕРЕМЕННУЮ!
                g_disk_regs = regs;
                break;
            }
        }
    }

    if (!g_disk_regs) {
        sys_puts(console_ep, "[VIRTIO] ERROR: Block device not found on the entire bus.\n");
        return -1;
    }

    // ==========================================
    // VIRTIO HANDSHAKE & VIRTQUEUE SETUP
    // ==========================================
    g_disk_regs->status = 0; 
    g_disk_regs->status |= 1;
    g_disk_regs->status |= 2;
    g_disk_regs->guest_features = 0;
    g_disk_regs->status |= 8;

    // Настраиваем очередь команд (Virtqueue 0)
    g_disk_regs->guest_page_size = 4096;
    g_disk_regs->queue_sel = 0;
    g_disk_regs->queue_num = 16;
    g_disk_regs->queue_align = 64;
    // ИСПРАВЛЕНО: Убран ошибочный сдвиг +0x400. Теперь PFN строго указывает на начало страницы!
    g_disk_regs->queue_pfn = (g_vfs_shm_phys_base + 0x1000) / 4096;
    g_disk_regs->status |= 4; // DRIVER_OK

    // ==========================================
    // СТАТИЧЕСКАЯ НАСТРОЙКА ДЕСКРИПТОРОВ
    // ==========================================
    volatile virtq_desc* vq_desc = (volatile virtq_desc*)(VIRTIO_Q_SHM_VIRT_BASE + VQ_DESC_OFFSET);
    volatile virtio_blk_req* blk_req = (volatile virtio_blk_req*)(VIRTIO_Q_SHM_VIRT_BASE + BLK_REQ_OFFSET);
    
    blk_req->type = 0; 
    blk_req->reserved = 0;
    vq_desc[0].addr = (g_vfs_shm_phys_base + 0x1000) + BLK_REQ_OFFSET;
    vq_desc[0].len = sizeof(virtio_blk_req);
    vq_desc[0].flags = 1; vq_desc[0].next = 1;
    
    vq_desc[1].flags = 1 | 2; vq_desc[1].next = 2; 
    
    vq_desc[2].addr = (g_vfs_shm_phys_base + 0x1000) + BLK_STATUS_OFFSET;
    vq_desc[2].len = 1;
    vq_desc[2].flags = 2; vq_desc[2].next = 0;

    // ==========================================
    // ЧТЕНИЕ FAT32 (БЕЗОПАСНЫЕ БУФЕРЫ)
    // ==========================================
    // Boot Sector читаем в 0x600
    virtio_read_sector(0, 0x600, 512);

    volatile uint8_t* blk_status = (volatile uint8_t*)(VIRTIO_Q_SHM_VIRT_BASE + BLK_STATUS_OFFSET);
    if (*blk_status == 0) {
        fat32_image_ptr = (char*)(VFS_SHM_VIRT_BASE + 0x600);
        fat32_bpb = (FAT32_BPB*)fat32_image_ptr;
        
        sys_puts(console_ep, "[FAT32] Boot Sector Loaded! Volume: ");
        char vol[12]; strncpy(vol, fat32_bpb->volume_label, 11); vol[11] = '\0';
        sys_puts(console_ep, vol); sys_puts(console_ep, "\n");

        fat32_first_data_sector = fat32_bpb->reserved_sectors + (fat32_bpb->fat_count * fat32_bpb->fat_size_32);
        uint32_t root_sector = fat32_first_data_sector + ((fat32_bpb->root_cluster - 2) * fat32_bpb->sectors_per_cluster);

        // Root Directory читаем в 0x800 (1024 байта достаточно для 32 файлов)
        virtio_read_sector(root_sector, 0x800, 1024);
        if (*blk_status == 0) {
            sys_puts(console_ep, "[FAT32] Root Directory Loaded!\n");
            // Смещение от BootSector (0x600) до RootDir (0x800) = 0x200 (512 байт)
            fat32_root_offset = 0x200; 
        }
    }

    // Инициализируем ROM-драйверы ВРУЧНУЮ (без фигурных скобок, чтобы не зависеть от CRT)
    vfs[0].active = true; strcpy(vfs[0].path, "/uart_driver"); vfs[0].is_dir = false; vfs[0].is_rom = true;
    vfs[1].active = true; strcpy(vfs[1].path, "/timer_driver"); vfs[1].is_dir = false; vfs[1].is_rom = true;
    vfs[2].active = true; strcpy(vfs[2].path, "/shell"); vfs[2].is_dir = false; vfs[2].is_rom = true;
    vfs[3].active = true; strcpy(vfs[3].path, "/blk_driver"); vfs[3].is_dir = false; vfs[3].is_rom = true;
    vfs[4].active = true; strcpy(vfs[4].path, "/mnt"); vfs[4].is_dir = true; vfs[4].is_rom = true; // ТОЧКА МОНТИРОВАНИЯ
    vfs_count = 5;

    sys_puts(console_ep, "[BLK DRIVER] Listening for IPC requests...\n");

    volatile int* mailbox = (volatile int*)(VFS_SHM_VIRT_BASE + 4084 - 12);
    mailbox[0] = 0; mailbox[1] = 0; mailbox[2] = 0;

    seL4_Word sender_badge;

    while (1) {
        // Block and wait for an IPC message from the Shell/Client. 
        // This eliminates CPU busy-waiting and standardizes seL4 synchronization.
        seL4_MessageInfo_t info = seL4_Recv(my_ep, &sender_badge);
        
        // Extract System Call Number from Message Register (Control Plane)
        // Fallback to legacy mailbox if the shell hasn't been updated to pass MRs yet.
        int syscall_num = seL4_GetMR(0); 
        if (syscall_num == 0 && mailbox[0] != 0) {
            syscall_num = mailbox[0]; 
        }

        int ret_val = -1;
            switch (syscall_num) {
                case 109: { // SYS_MKDIR
                    char *shm = (char*)VFS_SHM_VIRT_BASE;
                    int found = -1;
                    
                    for (int k = 0; k < 128; k++) {
                        if (vfs[k].active && strcmp(vfs[k].path, shm) == 0) { found = k; break; }
                    }
                    
                    if (found >= 0) {
                        ret_val = vfs[found].is_dir ? 0 : -1;
                    } else if (vfs_count < 128) {
                        vfs[vfs_count].active = true;
                        strcpy(vfs[vfs_count].path, shm);
                        vfs[vfs_count].is_dir = true;
                        vfs[vfs_count].is_rom = false;
                        vfs_count++;
                        ret_val = 0;
                    } else {
                        ret_val = -1;
                    }
                    break;
                }

                case 110: { // SYS_LS
                    sys_puts(console_ep, "[BLK] LS command received\n");
                    char *shm = (char*)VFS_SHM_VIRT_BASE;
                    char target_dir[64];
                    
                    // FIX 2: Stack overflow protection
                    strncpy(target_dir, shm, sizeof(target_dir) - 1);
                    target_dir[sizeof(target_dir) - 1] = '\0';

                    // FIX 1: Removed sprintf, using custom sys_puts concatenation
                    sys_puts(console_ep, "[BLK] path='");
                    sys_puts(console_ep, target_dir);
                    sys_puts(console_ep, "'\n");
                    
                    // Append trailing slash (if not root) to filter subdirectories correctly
                    int t_len = strlen(target_dir);
                    if (t_len > 1 && target_dir[t_len - 1] != '/') {
                        target_dir[t_len] = '/';
                        target_dir[t_len + 1] = '\0';
                        t_len++;
                    }
                    
                    strcpy(shm, "Directory listing:\n");
                    char *curr = shm + strlen(shm);
                    
                    // Limit: 4096 (Page) - 16 (Mailbox footprint) - 128 (Buffer for one line)
                    char *shm_limit = shm + 3950; 
                    
                    // RAM/ROM VFS Search
                    for (int k = 0; k < 128; k++) {
                        if (vfs[k].active && strncmp(vfs[k].path, target_dir, t_len) == 0) {
                            char *rest = vfs[k].path + t_len;
                            if (strlen(rest) > 0 && strchr(rest, '/') == nullptr) {
                                // FIX 3: Shared Memory and Mailbox overflow protection
                                if (curr >= shm_limit) break; 

                                strcpy(curr, vfs[k].is_dir ? " [DIR] " : (vfs[k].is_rom ? " [ROM] " : " [RAM] "));
                                curr += 7;
                                strcpy(curr, rest); curr += strlen(rest);
                                strcpy(curr, "\n"); curr += 1;
                            }
                        }
                    }

                    // FAT32 Read: Check if path targets /mnt/
                    if (strncmp(target_dir, "/mnt/", 5) == 0 && fat32_image_ptr) {
                        FAT32_DirEntry* entry = (FAT32_DirEntry*)(fat32_image_ptr + fat32_root_offset);
                        for (int i = 0; i < 16; i++) {
                            // FIX 3 (Applied): Overflow protection for FAT32 loop
                            if (curr >= shm_limit) break; 

                            if (entry[i].name[0] == 0x00) break;
                            if (entry[i].name[0] == 0xE5 || entry[i].attr == 0x0F) continue;
                            
                            strcpy(curr, " [FAT] "); curr += 7;
                            for(int j=0; j < 11; j++) {
                                if (entry[i].name[j] != ' ') *curr++ = entry[i].name[j];
                                if (j == 7 && entry[i].name[8] != ' ') *curr++ = '.';
                            }
                            *curr++ = '\n';
                        }
                    }
                    *curr = '\0';
                    ret_val = 0;
                    break;
                }

                case 111: { // SYS_STAT (CD)
                    char *shm = (char*)VFS_SHM_VIRT_BASE;
                    int found = -1;
                    if (strcmp(shm, "/") == 0) { found = 0; } // Разрешаем прыгать в корень всегда
                    else {
                        for (int k = 0; k < 128; k++) {
                            if (vfs[k].active && strcmp(vfs[k].path, shm) == 0 && vfs[k].is_dir) {
                                found = 0; break;
                            }
                        }
                    }
                    ret_val = found;
                    break;
                }

                case 112: { // SYS_TOUCH
                    char *shm = (char*)VFS_SHM_VIRT_BASE;
                    int found = -1;
                    for(int k=0; k<128; k++) {
                        if(vfs[k].active && strcmp(vfs[k].path, shm) == 0) { found = k; break; }
                    }
                    if (found >= 0) {
                        ret_val = 0;
                    } else if (vfs_count < 128) {
                        vfs[vfs_count].active = true;
                        strcpy(vfs[vfs_count].path, shm);
                        vfs[vfs_count].is_dir = false;
                        vfs[vfs_count].is_rom = false;
                        vfs[vfs_count].size = 0;
                        memset(vfs[vfs_count].data, 0, sizeof(vfs[vfs_count].data));
                        vfs_count++;
                        ret_val = 0;
                    } else {
                        ret_val = -1;
                    }
                    break;
                }

                case 113: { // SYS_WRITE_FILE (Гибридный: VFS RAM + FAT32 DMA)
                    char *shm = (char*)VFS_SHM_VIRT_BASE;
                    char *path = shm;          
                    char *text = shm + 128;
                    
                    // ИСПРАВЛЕНО: Unbounded Buffer Copy (Защита от переполнения SHM)
                    // Ограничиваем окно пользовательских данных безопасными пределами
                    text[1023] = '\0';

                    // ==========================================
                    // МАГИЯ FAT32: ЕСЛИ ПУТЬ НАЧИНАЕТСЯ С /mnt/
                    // ==========================================
                    if (strncmp(path, "/mnt/", 5) == 0 && fat32_image_ptr) {
                        char* target_file = path + 5;
                        char fat_name[11];
                        
                        // Конвертируем "my_file.txt" в формат "MY_FILE TXT"
                        for(int i=0; i<11; i++) fat_name[i] = ' ';
                        int idx = 0;
                        for(int i=0; target_file[i] && target_file[i] != '.'; i++) {
                            char c = target_file[i];
                            if (c >= 'a' && c <= 'z') c -= 32;
                            if (idx < 8) fat_name[idx++] = c;
                        }
                        char* ext = strchr(target_file, '.');
                        if (ext) {
                            ext++; idx = 8;
                            for(int i=0; ext[i]; i++) {
                                char c = ext[i];
                                if (c >= 'a' && c <= 'z') c -= 32;
                                if (idx < 11) fat_name[idx++] = c;
                            }
                        }

                        FAT32_DirEntry* entry = (FAT32_DirEntry*)(fat32_image_ptr + fat32_root_offset);
                        int existing_idx = -1;
                        int free_idx = -1;
                        
                        // Ищем файл или свободное место в Корневой Директории
                        for (int i = 0; i < 16; i++) {
                            if (entry[i].name[0] == 0x00 || entry[i].name[0] == 0xE5) {
                                if (free_idx == -1) free_idx = i;
                                if (entry[i].name[0] == 0x00) break; // Конец списка
                            } else if (strncmp((const char*)entry[i].name, fat_name, 11) == 0) {
                                existing_idx = i;
                                break;
                            }
                        }

                        uint32_t file_cluster = 0;
                        int target_idx = -1;

                        if (existing_idx >= 0) {
                            // 1. ФАЙЛ СУЩЕСТВУЕТ
                            file_cluster = (entry[existing_idx].fst_clus_hi << 16) | entry[existing_idx].fst_clus_lo;
                            entry[existing_idx].file_size = strlen(text);
                            target_idx = existing_idx;
                        } else if (free_idx >= 0) {
                            // 2. НОВЫЙ ФАЙЛ (Будет записан в свободный слот)
                            target_idx = free_idx;
                        }

                        // МАГИЯ ЗДЕСЬ: Если кластер еще не выделен (это совершенно новый файл, 
                        // ИЛИ существующий пустой файл от create_file)
                        if (file_cluster == 0 && target_idx >= 0) {
                            virtio_read_sector(fat32_bpb->reserved_sectors, 0xE00, 512);
                            uint32_t* fat_table = (uint32_t*)(VFS_SHM_VIRT_BASE + 0xE00);
                            
                            // Ищем свободный кластер в таблице FAT
                            for (int c = 3; c < 128; c++) {
                                if ((fat_table[c] & 0x0FFFFFFF) == 0x00000000) {
                                    file_cluster = c;
                                    fat_table[c] = 0x0FFFFFFF; // Помечаем как занятый (End of File)
                                    break;
                                }
                            }
                            
                            if (file_cluster > 0) {
                                // Сохраняем обновленную таблицу FAT обратно на диск
                                virtio_write_sector(fat32_bpb->reserved_sectors, 0xE00, 512);
                                
                                // Если это совершенно новый файл (free_idx) - прописываем ему имя и атрибуты
                                if (target_idx == free_idx) {
                                    memcpy(entry[target_idx].name, fat_name, 11);
                                    entry[target_idx].attr = 0x20; 
                                }
                                
                                // Привязываем выделенный кластер к файлу (и для новых, и для существующих пустых)
                                entry[target_idx].fst_clus_hi = (file_cluster >> 16) & 0xFFFF;
                                entry[target_idx].fst_clus_lo = file_cluster & 0xFFFF;
                            }
                        }

                        // Теперь, когда у нас 100% есть валидный кластер, пишем данные!
                        if (file_cluster > 0) {
                            // Сбрасываем Текст на диск (Сектор Данных)
                            uint32_t data_sector = fat32_first_data_sector + ((file_cluster - 2) * fat32_bpb->sectors_per_cluster);
                            char* file_data = (char*)(VFS_SHM_VIRT_BASE + 0xC00);
                            for(int k=0; k<512; k++) file_data[k] = 0; 
                            strcpy(file_data, text);
                            virtio_write_sector(data_sector, 0xC00, 512);

                            // Сбрасываем Метаданные на диск (Root Directory)
                            uint32_t root_sector = fat32_first_data_sector + ((fat32_bpb->root_cluster - 2) * fat32_bpb->sectors_per_cluster);
                            virtio_write_sector(root_sector, 0x800, 1024); 
                            ret_val = 0;
                        } else {
                            ret_val = -1; // Диск заполнен! Нет свободных кластеров
                        }
                        break;
                    }

                    // ==========================================
                    // ИНАЧЕ: ЗАПИСЬ НА RAM-ДИСК (VFS)
                    // ==========================================
                    int target = -1;
                    for(int k=0; k<128; k++) {
                        if(vfs[k].active && strcmp(vfs[k].path, path) == 0 && !vfs[k].is_dir) { target = k; break; }
                    }

                    if (target == -1 && vfs_count < 128) {
                        target = vfs_count;
                        vfs[target].active = true;
                        strcpy(vfs[target].path, path);
                        vfs[target].is_dir = false;
                        vfs[target].is_rom = false;
                        vfs_count++;
                    }

                    if (target >= 0) {
                        if (vfs[target].is_rom) {
                            ret_val = -1;
                        } else {
                            // ИСПРАВЛЕНО: Безопасное копирование с проверкой границ
                            strncpy(vfs[target].data, text, 255);
                            vfs[target].data[255] = '\0'; // Принудительный нуль-терминатор
                            vfs[target].size = strlen(vfs[target].data);
                            ret_val = 0;
                        }
                    } else {
                        ret_val = -1;
                    }
                    break;
                }

                case 114: { // SYS_READ_FILE
                    char *shm = (char*)VFS_SHM_VIRT_BASE;
                    
                    if (strncmp(shm, "/mnt/", 5) == 0 && fat32_image_ptr) {
                        char* target_file = shm + 5;
                        char fat_name[11];
                        for(int i=0; i<11; i++) fat_name[i] = ' ';
                        int idx = 0;
                        for(int i=0; target_file[i] && target_file[i] != '.'; i++) {
                            char c = target_file[i];
                            if (c >= 'a' && c <= 'z') c -= 32;
                            if (idx < 8) fat_name[idx++] = c;
                        }
                        char* ext = strchr(target_file, '.');
                        if (ext) {
                            ext++; idx = 8;
                            for(int i=0; ext[i]; i++) {
                                char c = ext[i];
                                if (c >= 'a' && c <= 'z') c -= 32;
                                if (idx < 11) fat_name[idx++] = c;
                            }
                        }

                        FAT32_DirEntry* entry = (FAT32_DirEntry*)(fat32_image_ptr + fat32_root_offset);
                        bool found = false;
                        for (int i = 0; i < 16; i++) {
                            if (entry[i].name[0] == 0x00) break;
                            if (entry[i].name[0] == 0xE5 || entry[i].attr == 0x0F) continue;
                            
                            if (strncmp((const char*)entry[i].name, fat_name, 11) == 0) {
                                uint32_t file_cluster = (entry[i].fst_clus_hi << 16) | entry[i].fst_clus_lo;
                                uint32_t file_sector = fat32_first_data_sector + ((file_cluster - 2) * fat32_bpb->sectors_per_cluster);
                                uint32_t copy_size = entry[i].file_size < 1024 ? entry[i].file_size : 1024;
                                
                                uint32_t read_len = ((copy_size + 511) / 512) * 512;
                                if (read_len == 0) read_len = 512;
                                if (read_len > 1024) read_len = 1024; // Ограничиваем, чтобы не вылезти за пределы страницы
                                
                                // Читаем данные в безопасный буфер (Смещение 0xC00)
                                virtio_read_sector(file_sector, 0xC00, read_len);
                                
                                volatile uint8_t* blk_status = (volatile uint8_t*)(VIRTIO_Q_SHM_VIRT_BASE + BLK_STATUS_OFFSET);
                                if (*blk_status == 0) {
                                    // Теперь безопасно копируем текст файла в начало SHM (0x502000), 
                                    // откуда Оболочка заберет его для вывода на экран!
                                    char* file_data = (char*)(VFS_SHM_VIRT_BASE + 0xC00);
                                    for(uint32_t k = 0; k < copy_size; k++) {
                                        shm[k] = file_data[k];
                                    }
                                    shm[copy_size] = '\0';
                                    found = true;
                                }
                                break;
                            }
                            
                        }
                        ret_val = found ? 0 : -1;
                    } else {
                        int found = -1;
                        for(int k=0; k<128; k++) {
                            if(vfs[k].active && strcmp(vfs[k].path, shm) == 0) { found = k; break; }
                        }
                        
                        if (found >= 0 && !vfs[found].is_dir) {
                            if (vfs[found].is_rom) strcpy(shm, "<ELF Binary Data...>");
                            else strcpy(shm, vfs[found].data);
                            ret_val = 0;
                        } else {
                            ret_val = -1;
                        }
                    }
                    break;
                }

                case 115: { // CREATE_FILE (Умное создание)
                    char* shm = (char*)VFS_SHM_VIRT_BASE;
                    char req_name[32];
                    my_strcpy(req_name, shm);
                    
                    // Если вызвали create_file без аргументов
                    if (req_name[0] == '\0') {
                        my_strcpy(req_name, "FILE.TXT");
                    }
                    
                    uint32_t root_sector = fat32_first_data_sector + ((fat32_bpb->root_cluster - 2) * fat32_bpb->sectors_per_cluster);
                    virtio_read_sector(root_sector, 0x800, 1024);
                    FAT32_DirEntry* entry = (FAT32_DirEntry*)(VFS_SHM_VIRT_BASE + 0x800);
                    
                    char fat_name[12];
                    int counter = 0;
                    bool unique_found = false;
                    
                    // ИЩЕМ УНИКАЛЬНОЕ ИМЯ
                    while (!unique_found) {
                        to_fat32_name(req_name, counter, fat_name);
                        bool conflict = false;
                        for (int i = 0; i < 32; i++) {
                            // 0x00 = пусто, 0xE5 = удаленный файл (можно переиспользовать)
                            if (entry[i].name[0] == 0x00 || entry[i].name[0] == 0xE5) continue;
                            if (my_strncmp((char*)entry[i].name, fat_name, 11) == 0) {
                                conflict = true;
                                break;
                            }
                        }
                        if (!conflict) unique_found = true;
                        else counter++; // Имя занято, увеличиваем цифру в скобках!
                    }
                    
                    // ЗАПИСЫВАЕМ ФАЙЛ В ПЕРВЫЙ СВОБОДНЫЙ СЛОТ
                    bool created = false;
                    for (int i = 0; i < 32; i++) {
                        if (entry[i].name[0] == 0x00 || entry[i].name[0] == 0xE5) {
                            my_memcpy(entry[i].name, fat_name, 11);
                            entry[i].attr = 0x20; // Archive (Обычный файл)
                            entry[i].file_size = 0;     // Пустой файл
                            entry[i].fst_clus_hi = 0;
                            entry[i].fst_clus_lo = 0;
                            created = true;
                            break;
                        }
                    }
                    
                    if (created) {
                        virtio_write_sector(root_sector, 0x800, 1024);
                        
                        // Красиво форматируем вывод для консоли
                        my_strcpy(shm, "SUCCESS: Created ");
                        char* p = shm + 17;
                        for(int i=0; i<8 && fat_name[i]!=' '; i++) *p++ = fat_name[i];
                        if (fat_name[8] != ' ') {
                            *p++ = '.';
                            for(int i=8; i<11 && fat_name[i]!=' '; i++) *p++ = fat_name[i];
                        }
                        *p++ = '\n';
                        *p = '\0';
                    } else {
                        my_strcpy(shm, "ERROR: Root Directory is full!\n");
                    }
                    ret_val = 0;
                    break;
                }

                case 119: { // RM (Удаление файла)
                    char* shm = (char*)VFS_SHM_VIRT_BASE;
                    char fat_name[12];
                    to_fat32_name(shm, 0, fat_name); // Конвертируем аргумент в формат FAT32
                    
                    uint32_t root_sector = fat32_first_data_sector + ((fat32_bpb->root_cluster - 2) * fat32_bpb->sectors_per_cluster);
                    virtio_read_sector(root_sector, 0x800, 1024);
                    FAT32_DirEntry* entry = (FAT32_DirEntry*)(VFS_SHM_VIRT_BASE + 0x800);
                    
                    bool found = false;
                    for (int i = 0; i < 32; i++) {
                        if (entry[i].name[0] == 0x00) break; // Конец списка
                        if (entry[i].name[0] == 0xE5) continue; // Уже удален
                        
                        if (my_strncmp((char*)entry[i].name, fat_name, 11) == 0) {
                            // Магия FAT32: чтобы удалить файл, достаточно заменить первый 
                            // символ его имени на байт 0xE5. Система посчитает слот свободным!
                            entry[i].name[0] = 0xE5; 
                            found = true;
                            break;
                        }
                    }
                    
                    if (found) {
                        virtio_write_sector(root_sector, 0x800, 1024);
                        my_strcpy(shm, "SUCCESS: File deleted.\n");
                    } else {
                        my_strcpy(shm, "ERROR: File not found.\n");
                    }
                    ret_val = 0;
                    break;
                }
                case 116: { // SYS_CREATE_FAT_FILE
                    char *shm = (char*)VFS_SHM_VIRT_BASE;
                    
                    // Парсим входные данные: "NEWFILE TXT|Привет, мир!"
                    char filename[12];
                    strncpy(filename, shm, 11);
                    filename[11] = '\0';
                    char* file_content = shm + 12; // Текст начинается после 11 символов имени и 1 разделителя
                    
                    if (fat32_image_ptr) {
                        FAT32_DirEntry* entry = (FAT32_DirEntry*)(fat32_image_ptr + fat32_root_offset);

                        // ИСПРАВЛЕНИЕ: Сначала проверяем, существует ли уже файл с таким именем,
                        // чтобы избежать создания дубликатов.
                        bool file_exists = false;
                        for (int i = 0; i < 16; i++) {
                            if (entry[i].name[0] == 0x00) break; // Конец директории
                            if (entry[i].name[0] == 0xE5) continue; // Удаленный файл
                            if (strncmp((const char*)entry[i].name, filename, 11) == 0) {
                                file_exists = true;
                                break;
                            }
                        }

                        if (file_exists) {
                            strcpy(shm, "ERROR: File already exists!\n");
                            break; // Выходим, ret_val останется -1
                        }

                        // ==========================================
                        // ШАГ 1: АЛЛОКАЦИЯ КЛАСТЕРА В ТАБЛИЦЕ FAT
                        // ==========================================
                        // Читаем первый сектор таблицы FAT (она идет сразу после Boot Sector)
                        virtio_read_sector(fat32_bpb->reserved_sectors, 0xE00, 512);
                        uint32_t* fat_table = (uint32_t*)(VFS_SHM_VIRT_BASE + 0xE00);
                        
                        uint32_t free_cluster = 0;
                        for (int c = 3; c < 128; c++) { // Ищем с 3-го кластера (0 и 1 зарезервированы, 2 - Root)
                            if ((fat_table[c] & 0x0FFFFFFF) == 0x00000000) {
                                free_cluster = c;
                                fat_table[c] = 0x0FFFFFFF; // Помечаем кластер как занятый (End of File)
                                break;
                            }
                        }
                        
                        if (free_cluster == 0) {
                            strcpy(shm, "ERROR: No free clusters on disk!\n");
                            break;
                        }

                        // Сохраняем обновленную таблицу FAT обратно на диск
                        virtio_write_sector(fat32_bpb->reserved_sectors, 0xE00, 512);

                        // ==========================================
                        // ШАГ 2: ЗАПИСЬ ДАННЫХ ФАЙЛА
                        // ==========================================
                        uint32_t data_sector = fat32_first_data_sector + ((free_cluster - 2) * fat32_bpb->sectors_per_cluster);
                        char* file_data = (char*)(VFS_SHM_VIRT_BASE + 0xC00);
                        for(int k=0; k<512; k++) file_data[k] = 0; // Очищаем сектор
                        
                        strcpy(file_data, file_content); // Копируем наш текст
                        virtio_write_sector(data_sector, 0xC00, 512); // Сбрасываем на диск!

                        // ==========================================
                        // ШАГ 3: ОБНОВЛЕНИЕ ROOT DIRECTORY
                        // ==========================================
                        // FAT32_DirEntry* entry уже объявлен выше
                        bool created = false;
                        for (int i = 0; i < 16; i++) {
                            // 0x00 = никогда не использовался, 0xE5 = файл был удален (свободное место)
                            if (entry[i].name[0] == 0x00 || entry[i].name[0] == 0xE5) { 
                                memcpy(entry[i].name, filename, 11);
                                entry[i].attr = 0x20; // 0x20 = Обычный архивный файл
                                entry[i].fst_clus_hi = (free_cluster >> 16) & 0xFFFF;
                                entry[i].fst_clus_lo = free_cluster & 0xFFFF;
                                entry[i].file_size = strlen(file_content);
                                created = true;
                                break;
                            }
                        }

                        if (created) {
                            uint32_t root_sector = fat32_first_data_sector + ((fat32_bpb->root_cluster - 2) * fat32_bpb->sectors_per_cluster);
                            virtio_write_sector(root_sector, 0x800, 1024); // Перезаписываем директорию
                            strcpy(shm, "SUCCESS: File successfully created on FAT32!\n");
                        } else {
                            strcpy(shm, "ERROR: Root Directory is full!\n");
                        }
                    }
                    ret_val = 0;
                    break;
                }


                case 118: { // Краш-тест - удалить 
                    volatile int* boom = (volatile int*)0x0;
                    *boom = 0xBAD; // Попытка записи в нулевой указатель. ПРОЦЕССОР УБЬЕТ НАС ЗДЕСЬ!
                    break;
                }
            }
            
            // Legacy Data Plane flag reset
            mailbox[1] = ret_val;
            mailbox[0] = 0; 
            mailbox[2] = 1;

            // Unblock the client and pass the return code safely via Control Plane
            seL4_SetMR(0, ret_val);
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
    }

    return 0;
}