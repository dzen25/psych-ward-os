#include <sel4/sel4.h>
#include "common.h"
#include "fat32.h"
#include <stdint.h>

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

// --- Глобальные переменные ---
static char* g_shm_vaddr = nullptr;
static uint32_t g_shm_paddr = 0;
static FAT32_Instance g_file_system;

// Глобальные переменные VirtIO
static volatile VirtioMmioRegs* g_disk_regs = nullptr;
static uint16_t g_vq_avail_idx = 0;
static char* virtio_q_shm_base = nullptr;

// --- Вспомогательные функции ---
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

static inline seL4_IPCBuffer* get_local_ipc() {
    seL4_Word tls_addr;
    // Добавлена буква 'ro'. crt0 не мог его стереть!
    asm volatile("mrs %0, tpidrro_el0" : "=r"(tls_addr)); 
    return (seL4_IPCBuffer*)(tls_addr - 1024);
}

void __assert_fail(const char *expr, const char *file, int line, const char *func) {
    while (1) {}
}

// --- СТРОГИЕ СМЕЩЕНИЯ VIRTIO (Legacy Spec) ---
#define VQ_DESC_OFFSET      0x000
#define VQ_AVAIL_OFFSET     0x100
#define VQ_USED_OFFSET      0x140
#define BLK_REQ_OFFSET      0x200
#define BLK_STATUS_OFFSET   0x220

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
struct virtq_desc { uint64_t addr; uint32_t len; uint16_t flags; uint16_t next; };
struct virtq_avail { uint16_t flags; uint16_t idx; uint16_t ring[16]; };
struct virtq_used_elem { uint32_t id; uint32_t len; };
struct virtq_used { uint16_t flags; uint16_t idx; virtq_used_elem ring[16]; };
struct virtio_blk_req { uint32_t type; uint32_t reserved; uint64_t sector; };

// ========================================================
// АППАРАТНЫЙ УРОВЕНЬ: Функция, которую будет дергать FAT32
// ========================================================
bool hardware_virtio_read(uint32_t sector, uint32_t count, void* buffer) {
    // Эта функция читает в временный DMA-буфер в разделяемой памяти,
    // а затем копирует в конечный буфер 'buffer'.
    uint32_t len = count * 512;

    // Проверка, не слишком ли велик запрос для нашего одностраничного DMA-буфера
    if (len > 4096) {
        // Этот простой драйвер пока поддерживает чтение только в пределах одной страницы.
        return false;
    }

    volatile virtq_desc* vq_desc = (volatile virtq_desc*)((uintptr_t)virtio_q_shm_base + VQ_DESC_OFFSET);
    volatile virtq_avail* vq_avail = (volatile virtq_avail*)((uintptr_t)virtio_q_shm_base + VQ_AVAIL_OFFSET);
    volatile virtq_used* vq_used = (volatile virtq_used*)((uintptr_t)virtio_q_shm_base + VQ_USED_OFFSET);
    volatile virtio_blk_req* blk_req = (volatile virtio_blk_req*)((uintptr_t)virtio_q_shm_base + BLK_REQ_OFFSET);
    volatile uint8_t* blk_status = (volatile uint8_t*)((uintptr_t)virtio_q_shm_base + BLK_STATUS_OFFSET);

    // Подготовка запроса
    blk_req->type = 0; // VIRTIO_BLK_T_IN (чтение)
    blk_req->sector = sector;
    *blk_status = 0xFF; // Невалидный статус
    
    // Дескриптор данных должен быть обновлен для этого чтения.
    // DMA будет производиться в начало разделяемой области памяти.
    vq_desc[1].addr = g_shm_paddr;
    vq_desc[1].len = len;

    // Добавляем цепочку дескрипторов в кольцо доступных
    vq_avail->ring[g_vq_avail_idx % 16] = 0;
    g_vq_avail_idx++;

    // Барьер памяти, чтобы гарантировать, что записи дескриптора видны до обновления индекса
    __atomic_store_n(&vq_avail->idx, g_vq_avail_idx, __ATOMIC_RELEASE);

    // Уведомляем устройство
    g_disk_regs->queue_notify = 0;

    // Ждем, пока устройство обработает запрос
    uint32_t timeout_counter = 5000000;
    while (__atomic_load_n(&vq_used->idx, __ATOMIC_ACQUIRE) < g_vq_avail_idx) {
        if (--timeout_counter == 0) {
            return false; // Таймаут
        }
        seL4_Yield(); 
    } 

    // Проверяем статус
    if (*blk_status != 0) {
        return false; // VIRTIO_BLK_S_IOERR или VIRTIO_BLK_S_UNSUPP
    }

    // Копируем данные из DMA-буфера в конечное место назначения
    my_memcpy(buffer, (void*)g_shm_vaddr, len);

    return true;
}

bool hardware_virtio_write(uint32_t sector, uint32_t count, const void* buffer) {
    uint32_t len = count * 512;
    if (len > 4096) return false;

    // Копируем данные из безопасного буфера в DMA-буфер разделяемой памяти (Page 0)
    my_memcpy((void*)g_shm_vaddr, buffer, len);

    volatile virtq_desc* vq_desc = (volatile virtq_desc*)((uintptr_t)virtio_q_shm_base + VQ_DESC_OFFSET);
    volatile virtq_avail* vq_avail = (volatile virtq_avail*)((uintptr_t)virtio_q_shm_base + VQ_AVAIL_OFFSET);
    volatile virtq_used* vq_used = (volatile virtq_used*)((uintptr_t)virtio_q_shm_base + VQ_USED_OFFSET);
    volatile virtio_blk_req* blk_req = (volatile virtio_blk_req*)((uintptr_t)virtio_q_shm_base + BLK_REQ_OFFSET);
    volatile uint8_t* blk_status = (volatile uint8_t*)((uintptr_t)virtio_q_shm_base + BLK_STATUS_OFFSET);

    blk_req->type = 1; // VIRTIO_BLK_T_OUT (ЗАПИСЬ)
    blk_req->sector = sector;
    *blk_status = 0xFF; // Сброс статуса
    
    vq_desc[1].addr = g_shm_paddr;
    vq_desc[1].len = len;
    // ВАЖНО: Убираем флаг WRITE, потому что для записи на диск, диск должен ЧИТАТЬ из RAM
    vq_desc[1].flags = 1; // Только VIRTQ_DESC_F_NEXT
    vq_desc[1].next = 2;

    vq_avail->ring[g_vq_avail_idx % 16] = 0;
    g_vq_avail_idx++;
    __atomic_store_n(&vq_avail->idx, g_vq_avail_idx, __ATOMIC_RELEASE);

    g_disk_regs->queue_notify = 0;

    uint32_t timeout_counter = 5000000;
    while (__atomic_load_n(&vq_used->idx, __ATOMIC_ACQUIRE) < g_vq_avail_idx) {
        if (--timeout_counter == 0) return false;
        seL4_Yield(); 
    } 

    // ВОЗВРАЩАЕМ дескриптор в режим ЧТЕНИЯ для будущих операций
    vq_desc[1].flags = 1 | 2; // NEXT | WRITE

    return (*blk_status == 0);
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

    sys_puts(console_ep, "\n[BLK DRIVER] VFS & FAT32 Server Online (Refactored)!\n");

    // --- ДИНАМИЧЕСКИЙ ЗАПРОС SHM ---
    sys_puts(console_ep, "[BLK DRIVER] Requesting dynamic SHM from kernel...\n");
    seL4_SetMR(0, 107); // SYS_SHM_GET
    seL4_MessageInfo_t msg = seL4_MessageInfo_new(0, 0, 0, 1);
    seL4_Call(root_ep, msg);

    g_shm_vaddr = (char*)seL4_GetMR(0);
    g_shm_paddr = (uint32_t)seL4_GetMR(1);

    if (!g_shm_vaddr) {
        sys_puts(console_ep, "[BLK DRIVER] FATAL: Failed to get dynamic SHM!\n");
        while(1) seL4_Yield();
    }

    // Структуры virtio-очереди будут находиться на второй странице нашей SHM-области.
    virtio_q_shm_base = g_shm_vaddr + 0x1000;

    // 2. Инициализация железа (MMIO VirtIO)
    // Поиск устройства
    for (int i = 0; i < 32; i++) {
        uintptr_t slot_addr = 0x200004000ULL + (i * 0x200);
        volatile VirtioMmioRegs* regs = (volatile VirtioMmioRegs*)slot_addr;
        if (regs->magic_value == 0x74726976 && regs->device_id == 2) {
            g_disk_regs = regs;
            break;
        }
    }

    if (!g_disk_regs) {
        sys_puts(console_ep, "[VIRTIO] ERROR: Block device not found.\n");
        return -1;
    }

    // Handshake
    g_disk_regs->status = 0; 
    g_disk_regs->status |= 1; // ACKNOWLEDGE
    g_disk_regs->status |= 2; // DRIVER
    g_disk_regs->guest_features = 0;
    g_disk_regs->status |= 8; // FEATURES_OK

    // Настройка virtqueue
    g_disk_regs->guest_page_size = 4096;
    g_disk_regs->queue_sel = 0;
    g_disk_regs->queue_num = 16;
    g_disk_regs->queue_align = 64;
    g_disk_regs->queue_pfn = (g_shm_paddr + 0x1000) / 4096;
    g_disk_regs->status |= 4; // DRIVER_OK

    // Статически настраиваем дескрипторы для 3-х компонентного запроса (заголовок, данные, статус)
    volatile virtq_desc* vq_desc = (volatile virtq_desc*)((uintptr_t)virtio_q_shm_base + VQ_DESC_OFFSET);
    
    // Заголовок (читаемый устройством)
    vq_desc[0].addr = (g_shm_paddr + 0x1000) + BLK_REQ_OFFSET;
    vq_desc[0].len = sizeof(virtio_blk_req);
    vq_desc[0].flags = 1; // VIRTQ_DESC_F_NEXT
    vq_desc[0].next = 1;
    
    // Данные (записываемые устройством для чтения, читаемые для записи)
    // Для чтения это место, куда устройство записывает данные.
    vq_desc[1].flags = 1 | 2; // VIRTQ_DESC_F_NEXT | VIRTQ_DESC_F_WRITE
    vq_desc[1].next = 2; 
    
    // Статус (записываемый устройством)
    vq_desc[2].addr = (g_shm_paddr + 0x1000) + BLK_STATUS_OFFSET;
    vq_desc[2].len = 1;
    vq_desc[2].flags = 2; // VIRTQ_DESC_F_WRITE
    vq_desc[2].next = 0;

    // 3. Монтируем Файловую Систему!
    // Передаем в нее указатель на функцию hardware_virtio_read
    if (fat32_init(&g_file_system, hardware_virtio_read, hardware_virtio_write)) {
        sys_puts(console_ep, "[BLK] FAT32 Mounted Successfully!\n");
    } else {
        sys_puts(console_ep, "[BLK] Failed to mount FAT32\n");
    }

    // 4. Главный цикл диспетчеризации (Control Plane)
    while (1) {
        seL4_Word sender_badge = 0;
        seL4_MessageInfo_t info = seL4_Recv(my_ep, &sender_badge);
        
        seL4_Word cmd = seL4_GetMR(0);
        
        if (cmd == 110) { // SYS_LS
            uint32_t target_sector = get_cwd_sector(&g_file_system);
            char sector_buf[512];
            
            // ВНИМАНИЕ: Это чтение аппаратно затирает g_shm_vaddr сырым FAT-сектором!
            hardware_virtio_read(target_sector, 1, sector_buf);

            char lfn_buf[256];
            for(int i = 0; i < 256; i++) lfn_buf[i] = 0;

            sys_puts(console_ep, "Directory listing:\n");

            for (int i = 0; i < 512; i += 32) {
                uint8_t* entry = (uint8_t*)&sector_buf[i];
                if (entry[0] == 0x00) break; // Конец списка
                
                if (entry[0] == 0xE5) {
                    for(int k=0; k<256; k++) lfn_buf[k] = 0; 
                    continue; 
                }

                if (entry[11] == 0x0F) { // LFN
                    int seq = (entry[0] & 0x1F) - 1; 
                    if (seq >= 0 && seq < 20) {
                        char* p = lfn_buf + (seq * 13); 
                        for(int k=1;  k<11; k+=2) { if(entry[k] != 0xFF && entry[k] != 0) *p++ = entry[k]; }
                        for(int k=14; k<26; k+=2) { if(entry[k] != 0xFF && entry[k] != 0) *p++ = entry[k]; }
                        for(int k=28; k<32; k+=2) { if(entry[k] != 0xFF && entry[k] != 0) *p++ = entry[k]; }
                    }
                    continue; 
                }

                if (entry[11] & 0x08) {
                    for(int k=0; k<256; k++) lfn_buf[k] = 0; 
                    continue; 
                }

                bool is_dir = (entry[11] & 0x10);
                
                // === УМНАЯ СБОРКА ИМЕНИ ===
                char final_name[256];
                final_name[0] = '\0';

                if (lfn_buf[0] != '\0') {
                    my_strcpy(final_name, lfn_buf);           
                    for(int k=0; k<256; k++) lfn_buf[k] = 0; 
                } else {
                    int pos = 0;
                    for (int j = 0; j < 8 && entry[j] != ' '; j++) final_name[pos++] = entry[j];
                    if (entry[8] != ' ') {
                        final_name[pos++] = '.';
                        for (int j = 8; j < 11 && entry[j] != ' '; j++) final_name[pos++] = entry[j];
                    }
                    final_name[pos] = '\0';
                }

                // КРИТИЧЕСКАЯ ЗАЩИТА: Если имя оказалось пустым (артефакт/мусор), просто игнорируем эту запись!
                if (final_name[0] == '\0') {
                    continue;
                }

                // Печатаем тип и финальное имя
                sys_puts(console_ep, is_dir ? " [DIR] " : " [FAT] ");
                sys_puts(console_ep, final_name);

                // === ВЫВОД РАЗМЕРА ФАЙЛА ===
                if (!is_dir) { 
                    uint32_t file_size = *(uint32_t*)&entry[28]; 
                    char size_str[16];
                    
                    int idx = 0;
                    if (file_size == 0) {
                        size_str[idx++] = '0';
                    } else {
                        char rev[16];
                        int r_idx = 0;
                        uint32_t temp = file_size;
                        while (temp > 0) {
                            rev[r_idx++] = '0' + (temp % 10);
                            temp /= 10;
                        }
                        while (r_idx > 0) size_str[idx++] = rev[--r_idx];
                    }
                    size_str[idx] = '\0';

                    sys_puts(console_ep, " \t(");
                    sys_puts(console_ep, size_str);
                    sys_puts(console_ep, " bytes)");
                }

                sys_puts(console_ep, "\n");
            }
            
            ((char*)g_shm_vaddr)[0] = '\0'; // Блокируем утечку SHM
            
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
            my_strcpy(filename, g_shm_vaddr);
            
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
            my_strcpy(path, g_shm_vaddr); // Спасаем имя файла со стека
            if (fat32_create_file(&g_file_system, path)) seL4_SetMR(0, 0);
            else seL4_SetMR(0, -1);
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
        }

        else if (cmd == 113) { // SYS_WRITE_FILE (echo > file)
            char path[64];
            my_strcpy(path, g_shm_vaddr); // Спасаем путь
            uint32_t len = seL4_GetMR(1);
            
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
            my_strcpy(path, g_shm_vaddr);
            
            // Читаем напрямую в SHM, чтобы shell мог сразу это распечатать
            if (fat32_read_text_file(&g_file_system, path, g_shm_vaddr)) seL4_SetMR(0, 0);
            else seL4_SetMR(0, -1);
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
        }

        else if (cmd == 120) { // SYS_RM
            char path[64];
            my_strcpy(path, g_shm_vaddr);
            if (fat32_delete_file(&g_file_system, path)) seL4_SetMR(0, 0);
            else seL4_SetMR(0, -1);
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
        }

        else if (cmd == 116) { // SYS_RENAME (mv)
            char old_p[32], new_p[32];
            my_strcpy(old_p, g_shm_vaddr);
            my_strcpy(new_p, g_shm_vaddr + 128); // Ожидаем новое имя по смещению 128
            if (fat32_rename_file(&g_file_system, old_p, new_p)) seL4_SetMR(0, 0);
            else seL4_SetMR(0, -1);
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
        }

        else if (cmd == 117) { // SYS_MKDIR
            char path[64];
            my_strcpy(path, g_shm_vaddr);
            if (fat32_mkdir(&g_file_system, path)) seL4_SetMR(0, 0);
            else seL4_SetMR(0, -1);
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
        }
        
        else if (cmd == 118) { // SYS_CD
            char path[64];
            my_strcpy(path, g_shm_vaddr);
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