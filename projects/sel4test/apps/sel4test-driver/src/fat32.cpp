#include "h/fat32.h"

// Forward declarations for static helpers
static void my_memcpy(void *dest, const void *src, int n);
static void format_fat32_name(const char* input, char* output);

// Вспомогательные функции (замена libc)
// Вспомогательная функция для сравнения строк без учета регистра (case-insensitive)
static int my_strcasecmp(const char *s1, const char *s2) {
    while (*s1 && *s2) {
        char c1 = (*s1 >= 'a' && *s1 <= 'z') ? *s1 - 32 : *s1;
        char c2 = (*s2 >= 'a' && *s2 <= 'z') ? *s2 - 32 : *s2;
        if (c1 != c2) return c1 - c2;
        s1++; s2++;
    }
    return *s1 - *s2;
}

static int my_strlen(const char* s) { int l=0; while(s[l]) l++; return l; }
static void my_strcpy(char* dest, const char* src) { while(*src) { *dest++ = *src++; } *dest = '\0'; }
static void my_strcat(char* dest, const char* src) { while(*dest) dest++; my_strcpy(dest, src); }
static int my_strcmp(const char *s1, const char *s2) {
    while (*s1 && (*s1 == *s2)) { s1++; s2++; }
    return *(const unsigned char*)s1 - *(const unsigned char*)s2;
}

// Структура загрузочного сектора (Boot Sector / BPB)
#pragma pack(push, 1)
struct FAT32_BPB {
    uint8_t  jmp[3];
    char     oem[8];
    uint16_t bytes_per_sector;
    uint8_t  sectors_per_cluster;
    uint16_t reserved_sectors;
    uint8_t  fat_count;
    uint16_t dir_entries;
    uint16_t total_sectors_16;
    uint8_t  media_descriptor;
    uint16_t sectors_per_fat_16;
    uint16_t sectors_per_track;
    uint16_t heads;
    uint32_t hidden_sectors;
    uint32_t total_sectors_32;
    uint32_t sectors_per_fat_32;
    uint16_t flags;
    uint16_t version;
    uint32_t root_cluster;
};
#pragma pack(pop)

bool fat32_init(FAT32_Instance* fs, block_read_fn read_func, block_write_fn write_func) {
    fs->read_blocks = read_func;
    fs->write_blocks = write_func; // Инициализируем запись!
    
    char sector_buf[512];
    // Просим аппаратный драйвер прочитать 0-й сектор
    if (!fs->read_blocks(0, 1, sector_buf)) return false;

    FAT32_BPB* bpb = (FAT32_BPB*)sector_buf;
    
    // Сохраняем метаданные (с защитой от заведомо невалидных значений на диске,
    // которые иначе привели бы к порче загрузочного сектора/FAT при записи, см. ниже)
    fs->bytes_per_sector = bpb->bytes_per_sector == 0 ? 512 : bpb->bytes_per_sector;
    fs->sectors_per_cluster = bpb->sectors_per_cluster == 0 ? 8 : bpb->sectors_per_cluster;
    fs->reserved_sectors = bpb->reserved_sectors == 0 ? 32 : bpb->reserved_sectors;
    fs->fat_count = bpb->fat_count == 0 ? 2 : bpb->fat_count;
    fs->sectors_per_fat = bpb->sectors_per_fat_32 == 0 ? 1 : bpb->sectors_per_fat_32;
    fs->root_cluster = bpb->root_cluster < 2 ? 2 : bpb->root_cluster;

    // Вычисляем, где начинаются данные
    uint32_t fat_size = (uint32_t)fs->fat_count * fs->sectors_per_fat;
    fs->data_start_sector = fs->reserved_sectors + fat_size;

    fs->current_dir_cluster = fs->root_cluster;

    return true;
}

// Переводит номер кластера в физический сектор на диске.
// Кластеры 0 и 1 зарезервированы спецификацией FAT32 и никогда не адресуют
// реальные данные; без этой проверки (clus - 2) в uint32_t арифметике
// underflow'ит в огромное число и даёт произвольный (контролируемый данными
// на диске) номер сектора для чтения/записи.
static uint32_t cluster_to_sector(FAT32_Instance* fs, uint32_t clus) {
    if (clus == 0 || clus == 1) clus = fs->root_cluster; // "0"/"1" часто означает Root
    if (clus < 2) clus = 2; // fallback, если и root_cluster невалиден
    return fs->data_start_sector + (clus - 2) * fs->sectors_per_cluster;
}

// ============================================================================
// === ОБХОД ЦЕПОЧКИ КЛАСТЕРОВ FAT (реальная FAT32-семантика, а не "contiguous") ===
// ============================================================================

static const uint32_t FAT32_EOC = 0x0FFFFFF8; // Значения >= этого — конец цепочки

// true, если значение из таблицы FAT указывает на следующий реальный кластер данных
static bool cluster_has_next(uint32_t fat_value) {
    return fat_value >= 2 && fat_value < FAT32_EOC;
}

// Читает 32-битную запись FAT для заданного кластера (маскированную, без верхних 4 служебных бит).
static uint32_t fat_get_entry(FAT32_Instance* fs, uint32_t cluster) {
    uint32_t fat_offset = cluster * 4;
    uint32_t fat_sector = fs->reserved_sectors + (fat_offset / fs->bytes_per_sector);
    uint32_t sector_offset = fat_offset % fs->bytes_per_sector;

    char buf[512];
    if (!fs->read_blocks(fat_sector, 1, buf)) return FAT32_EOC;
    uint32_t* entries = (uint32_t*)buf;
    return entries[sector_offset / 4] & 0x0FFFFFFF;
}

// Кэш последовательного чтения файла (см. read_chain_data ниже) — все реальные
// вызывающие (wifi firmware/NVRAM/CLM, cat больших файлов) читают файл строго по
// возрастающему offset кусками по FAT32_MAX_IO_CHUNK. Без кэша каждый такой вызов
// заново проходил ВСЮ цепочку FAT от начала файла (и fat_chain_has_cycle, и
// skip_clusters — оба O(длины цепочки)), т.е. O(N^2) обращений к FAT-таблице на
// файл длиной N кластеров, каждое отдельным чтением сектора с диска — на живом
// железе именно это, а не PIO/ширина SDIO-шины, давало ~5.8с на заливку 645КБ
// прошивки (см. ROADMAP.md/память, U-Boot читает тот же тип карты на 17МиБ/с).
// Инвалидируется в fat_set_entry() — на любое изменение FAT-таблицы (запись,
// truncate, rename, free) кэш сбрасывается, следующее чтение просто пойдёт
// холодным (медленным, но всегда корректным) путём с нуля.
struct FatSeqCursor {
    FAT32_Instance* fs;
    uint32_t start_cluster;
    uint32_t offset;   // offset, с которого продолжится следующее чтение
    uint32_t cluster;  // кластер, соответствующий этому offset (0 = конец цепочки)
};
static FatSeqCursor g_fat_seq_cursor = {nullptr, 0, 0, 0};

// Записывает запись FAT во ВСЕ копии таблицы (fs->fat_count), как того требует спецификация.
static bool fat_set_entry(FAT32_Instance* fs, uint32_t cluster, uint32_t value) {
    g_fat_seq_cursor.fs = nullptr; // см. комментарий у FatSeqCursor выше
    uint32_t fat_offset = cluster * 4;
    uint32_t sector_offset = fat_offset % fs->bytes_per_sector;
    char buf[512];
    for (uint8_t copy = 0; copy < fs->fat_count; copy++) {
        uint32_t fat_sector = fs->reserved_sectors + (uint32_t)copy * fs->sectors_per_fat + (fat_offset / fs->bytes_per_sector);
        if (!fs->read_blocks(fat_sector, 1, buf)) return false;
        uint32_t* entries = (uint32_t*)buf;
        entries[sector_offset / 4] = (entries[sector_offset / 4] & 0xF0000000) | (value & 0x0FFFFFFF);
        if (!fs->write_blocks(fat_sector, 1, buf)) return false;
    }
    return true;
}

// Определяет, зациклена ли цепочка кластеров, начинающаяся с start_cluster, алгоритмом
// Флойда ("черепаха и заяц", O(1) памяти) — точно обнаруживает цикл, в отличие от
// эвристического обрыва по счётчику итераций (guard), который лишь предотвращает
// зависание, но не отличает реальный цикл от очень длинной легитимной цепочки.
static bool fat_chain_has_cycle(FAT32_Instance* fs, uint32_t start_cluster) {
    if (start_cluster < 2) return false;
    uint32_t slow = start_cluster;
    uint32_t fast = start_cluster;
    while (true) {
        uint32_t f1 = fat_get_entry(fs, fast);
        if (!cluster_has_next(f1)) return false;
        uint32_t f2 = fat_get_entry(fs, f1);
        if (!cluster_has_next(f2)) return false;
        slow = fat_get_entry(fs, slow);
        fast = f2;
        if (slow == fast) return true;
    }
}

// Ищет свободный (== 0) кластер, просматривая ВСЮ таблицу FAT, а не только первый сектор.
static uint32_t fat_find_free_cluster(FAT32_Instance* fs) {
    uint32_t entries_per_sector = fs->bytes_per_sector / 4;
    char buf[512];
    for (uint32_t sec = 0; sec < fs->sectors_per_fat; sec++) {
        if (!fs->read_blocks(fs->reserved_sectors + sec, 1, buf)) break;
        uint32_t* entries = (uint32_t*)buf;
        for (uint32_t e = 0; e < entries_per_sector; e++) {
            uint32_t cluster = sec * entries_per_sector + e;
            if (cluster < 2) continue; // 0 и 1 зарезервированы спецификацией
            if ((entries[e] & 0x0FFFFFFF) == 0) return cluster;
        }
    }
    return 0; // Диск заполнен
}

// Освобождает всю цепочку кластеров, начиная с start_cluster (используется при удалении
// файла и при перезаписи существующего файла новыми данными).
static bool fat_free_chain(FAT32_Instance* fs, uint32_t start_cluster) {
    if (fat_chain_has_cycle(fs, start_cluster)) return false; // Повреждённая цепочка — не трогаем диск

    uint32_t cluster = start_cluster;
    int guard = 0; // Доп. защита от иных форм повреждения цепочки
    while (cluster >= 2 && guard++ < 65536) {
        uint32_t next = fat_get_entry(fs, cluster);
        fat_set_entry(fs, cluster, 0);
        if (!cluster_has_next(next)) break;
        cluster = next;
    }
    return true;
}

// Выделяет и связывает цепочку из num_clusters новых кластеров, обнуляя содержимое
// каждого (важно: иначе в новых файлах/директориях будет виден мусор со старых данных
// на диске). Возвращает номер первого кластера цепочки или 0 при нехватке места.
static uint32_t fat_alloc_chain(FAT32_Instance* fs, uint32_t num_clusters) {
    if (num_clusters == 0) return 0;

    uint32_t first = 0, prev = 0;
    char zero_buf[512] = {0};

    for (uint32_t i = 0; i < num_clusters; i++) {
        uint32_t c = fat_find_free_cluster(fs);
        if (c == 0 || !fat_set_entry(fs, c, FAT32_EOC)) {
            if (first != 0) fat_free_chain(fs, first);
            return 0;
        }
        if (prev != 0 && !fat_set_entry(fs, prev, c)) {
            fat_set_entry(fs, c, 0);
            if (first != 0) fat_free_chain(fs, first);
            return 0;
        }
        if (first == 0) first = c;
        prev = c;

        uint32_t sec = cluster_to_sector(fs, c);
        for (uint32_t s = 0; s < fs->sectors_per_cluster; s++) {
            fs->write_blocks(sec + s, 1, zero_buf);
        }
    }
    return first;
}

// Верхняя граница одного вызова read_chain_data/fat32_format_dir_listing — совпадает
// с размером страницы SHM, которой оперируют вызывающие функции в blk_driver.cpp.
static const uint32_t FAT32_MAX_IO_CHUNK = 4096;

// Читает до max_len байт данных файла, начиная с байта offset, следуя реальной цепочке
// кластеров FAT (в отличие от старого кода, предполагавшего непрерывное размещение).
// Возвращает количество фактически скопированных байт.
//
// ВАЖНО: результат сначала накапливается в приватном статическом буфере, а не пишется
// напрямую в out_buffer. Причина: out_buffer у вызывающей стороны (blk_driver.cpp) часто
// указывает на g_shm_vaddr, а hardware_virtio_read() использует ТУ ЖЕ САМУЮ страницу как
// DMA-скретч для КАЖДОГО fs->read_blocks() — при накоплении нескольких секторов подряд
// прямо в out_buffer каждое следующее чтение затирало бы уже скопированные ранее байты.
static uint32_t read_chain_data(FAT32_Instance* fs, uint32_t start_cluster, uint32_t offset, char* out_buffer, uint32_t max_len) {
    uint32_t bytes_per_cluster = (uint32_t)fs->bytes_per_sector * fs->sectors_per_cluster;
    if (bytes_per_cluster == 0 || start_cluster < 2) return 0;
    if (max_len > FAT32_MAX_IO_CHUNK) max_len = FAT32_MAX_IO_CHUNK;

    uint32_t cluster;
    // Тёплое продолжение предыдущего последовательного чтения ЭТОГО ЖЕ файла с
    // ТОЧНО того offset, на котором оно остановилось — цепочка от начала файла
    // до этой точки уже пройдена и проверена раньше (см. FatSeqCursor выше),
    // не гоняем skip_clusters/fat_chain_has_cycle заново. Любое расхождение
    // (другой файл, "прыжок"/seek, offset назад) — обычный холодный путь.
    if (g_fat_seq_cursor.fs == fs && g_fat_seq_cursor.start_cluster == start_cluster &&
        g_fat_seq_cursor.offset == offset) {
        cluster = g_fat_seq_cursor.cluster;
    } else {
        if (fat_chain_has_cycle(fs, start_cluster)) return 0; // Повреждённая (зацикленная) цепочка файла
        cluster = start_cluster;
        uint32_t skip_clusters = offset / bytes_per_cluster;
        for (uint32_t i = 0; i < skip_clusters; i++) {
            if (cluster < 2) { g_fat_seq_cursor.fs = nullptr; return 0; }
            uint32_t next = fat_get_entry(fs, cluster);
            if (!cluster_has_next(next)) { g_fat_seq_cursor.fs = nullptr; return 0; } // offset выходит за пределы реальной цепочки
            cluster = next;
        }
    }

    static char staging[FAT32_MAX_IO_CHUNK];
    uint32_t offset_in_cluster = offset % bytes_per_cluster;
    uint32_t copied = 0;
    char sbuf[512];

    while (copied < max_len && cluster >= 2) {
        uint32_t sector_in_cluster = offset_in_cluster / fs->bytes_per_sector;
        uint32_t byte_in_sector = offset_in_cluster % fs->bytes_per_sector;
        uint32_t sector = cluster_to_sector(fs, cluster) + sector_in_cluster;

        if (!fs->read_blocks(sector, 1, sbuf)) break;

        uint32_t avail_in_sector = fs->bytes_per_sector - byte_in_sector;
        uint32_t need = max_len - copied;
        uint32_t take = need < avail_in_sector ? need : avail_in_sector;
        my_memcpy(staging + copied, sbuf + byte_in_sector, take);
        copied += take;

        offset_in_cluster += take;
        if (offset_in_cluster >= bytes_per_cluster) {
            offset_in_cluster = 0;
            uint32_t next = fat_get_entry(fs, cluster);
            cluster = cluster_has_next(next) ? next : 0;
        }
    }

    my_memcpy(out_buffer, staging, copied);

    // Запоминаем позицию НА КОНЕЦ этого чтения — если следующий вызов попросит
    // ровно offset+copied того же файла (обычное последовательное чтение по
    // кругу chunk'ов), read_chain_data продолжит с этого cluster без повторного
    // прохода цепочки с нуля.
    g_fat_seq_cursor.fs = fs;
    g_fat_seq_cursor.start_cluster = start_cluster;
    g_fat_seq_cursor.offset = offset + copied;
    g_fat_seq_cursor.cluster = (cluster >= 2) ? cluster : 0;

    return copied;
}

// Записывает len байт данных, начиная с first_cluster, следуя цепочке FAT.
// Цепочка должна быть заранее выделена достаточного размера (см. fat_alloc_chain).
static bool write_chain_data(FAT32_Instance* fs, uint32_t first_cluster, const char* data, uint32_t len) {
    uint32_t cluster = first_cluster;
    uint32_t sector_in_cluster = 0;
    uint32_t remaining = len;
    const char* p = data;
    char sector_buf[512];

    while (remaining > 0) {
        if (cluster < 2) return false; // цепочка кончилась раньше данных — не должно происходить

        uint32_t sector = cluster_to_sector(fs, cluster) + sector_in_cluster;
        uint32_t chunk = remaining > fs->bytes_per_sector ? fs->bytes_per_sector : remaining;

        const char* src = p;
        if (chunk < fs->bytes_per_sector) {
            // Последний неполный сектор дозаполняем нулями, чтобы не читать за пределы data
            for (uint32_t i = 0; i < chunk; i++) sector_buf[i] = p[i];
            for (uint32_t i = chunk; i < fs->bytes_per_sector; i++) sector_buf[i] = 0;
            src = sector_buf;
        }
        if (!fs->write_blocks(sector, 1, src)) return false;

        p += chunk;
        remaining -= chunk;
        sector_in_cluster++;
        if (sector_in_cluster >= fs->sectors_per_cluster) {
            sector_in_cluster = 0;
            if (remaining > 0) {
                uint32_t next = fat_get_entry(fs, cluster);
                if (!cluster_has_next(next)) return false; // выделенной цепочки не хватило
                cluster = next;
            }
        }
    }
    return true;
}

// Результат поиска записи (или места под неё) в каталоге, с поддержкой обхода всей
// цепочки кластеров каталога (а не только первого сектора).
struct DirSlot {
    bool found;            // Нашли запись с этим именем
    uint32_t sector;       // Физический сектор с найденной 32-байтной записью
    int offset;            // Смещение записи внутри sector (0..480, кратно 32)
    uint32_t clus;         // Стартовый кластер файла (валиден, если found)
    uint32_t size;         // Размер файла (валиден, если found)

    bool has_free;         // Нашли свободный (0x00/0xE5) слот для вставки новой записи
    uint32_t free_sector;
    int free_offset;

    uint32_t last_clus;    // Последний кластер цепочки каталога (для роста, если !has_free)
};

// Кэш резолва пути (см. FatSeqCursor выше — тот же принцип, только уровнем выше):
// fat32_read_file() вызывается на КАЖДЫЙ чанк одного и того же последовательного
// чтения с ТЕМ ЖЕ filename, и без этого кэша каждый раз заново гонял
// fat32_resolve_parent+dir_scan — обход каталога с нуля (свой FAT-обход внутри
// dir_scan тоже не бесплатен). Именно это, а не read_chain_data, оставалось
// узким местом после первого фикса (~1.1с вместо ~5.9с на живом железе — FatSeq-
// Cursor убрал квадратичность по цепочке ФАЙЛА, но не трогал повторный резолв
// каталога). Ключ — путь (строка) + fs + CWD на момент резолва (чтобы
// относительный путь корректно переразрешался при смене CWD). Инвалидируется
// в любой мутирующей операции (create/write/delete/rename/mkdir) — на промахе
// обычный путь с нуля, как раньше.
struct FatPathCache {
    FAT32_Instance* fs;
    char path[64];
    uint32_t cwd_at_lookup;
    DirSlot slot;
};
static FatPathCache g_fat_path_cache{};

static inline void fat_invalidate_path_cache() {
    g_fat_path_cache.fs = nullptr;
}

// Ищет 8.3/LFN-запись с именем target_name в каталоге dir_cluster, обходя ВСЮ цепочку
// его кластеров и все сектора внутри каждого кластера (в отличие от старого кода,
// читавшего только первый сектор). Попутно запоминает первый встреченный свободный слот.
static bool dir_scan(FAT32_Instance* fs, uint32_t dir_cluster, const char* target_name, DirSlot* out) {
    out->found = false;
    out->has_free = false;

    uint32_t clus = (dir_cluster < 2) ? fs->root_cluster : dir_cluster;
    if (clus < 2) return false;
    if (fat_chain_has_cycle(fs, clus)) return false; // Повреждённая (зацикленная) цепочка каталога

    char formatted_name[12];
    format_fat32_name(target_name, formatted_name);
    char lfn_buf[261] = {0};
    char sector_buf[512];

    bool done = false;
    int guard = 0;
    while (!done && clus >= 2 && guard++ < 4096) {
        out->last_clus = clus;
        for (int s = 0; !done && s < fs->sectors_per_cluster; s++) {
            uint32_t sector = cluster_to_sector(fs, clus) + s;
            if (!fs->read_blocks(sector, 1, sector_buf)) return false;

            for (int i = 0; i < 512; i += 32) {
                uint8_t* entry = (uint8_t*)&sector_buf[i];

                if (entry[0] == 0x00 || entry[0] == 0xE5) {
                    if (!out->has_free) {
                        out->has_free = true;
                        out->free_sector = sector;
                        out->free_offset = i;
                    }
                    if (entry[0] == 0x00) { done = true; break; } // Истинный конец каталога
                    for (int k = 0; k < 261; k++) lfn_buf[k] = 0;
                    continue;
                }

                if (entry[11] == 0x0F) { // LFN
                    int seq = (entry[0] & 0x1F) - 1;
                    if (seq >= 0 && seq < 20) {
                        char* p = lfn_buf + (seq * 13);
                        for (int k = 1;  k < 11; k += 2) { if (entry[k] != 0xFF && entry[k] != 0) *p++ = entry[k]; }
                        for (int k = 14; k < 26; k += 2) { if (entry[k] != 0xFF && entry[k] != 0) *p++ = entry[k]; }
                        for (int k = 28; k < 32; k += 2) { if (entry[k] != 0xFF && entry[k] != 0) *p++ = entry[k]; }
                    }
                    continue;
                }

                if (entry[11] & 0x08) { for (int k = 0; k < 261; k++) lfn_buf[k] = 0; continue; }

                bool match = false;
                if (lfn_buf[0] != '\0') {
                    if (my_strcasecmp(lfn_buf, target_name) == 0) match = true;
                    for (int k = 0; k < 261; k++) lfn_buf[k] = 0;
                } else {
                    char entry_name[12];
                    my_memcpy(entry_name, entry, 11); entry_name[11] = '\0';
                    if (my_strcmp(entry_name, formatted_name) == 0) match = true;
                }

                if (match) {
                    out->found = true;
                    out->sector = sector;
                    out->offset = i;
                    uint16_t clus_hi = *(uint16_t*)&entry[20];
                    uint16_t clus_lo = *(uint16_t*)&entry[26];
                    out->clus = ((uint32_t)clus_hi << 16) | clus_lo;
                    out->size = *(uint32_t*)&entry[28];
                    done = true;
                    break;
                }
            }
        }
        if (done) break;
        uint32_t next = fat_get_entry(fs, clus);
        if (!cluster_has_next(next)) break;
        clus = next;
    }
    return true; // I/O прошло успешно; результат (found/has_free) — в out
}

// Расширяет цепочку каталога на один новый (уже обнулённый) кластер и возвращает его
// первый сектор — используется, когда directory-скан не нашёл свободного слота ни в
// одном из существующих кластеров каталога.
static bool dir_grow(FAT32_Instance* fs, uint32_t last_clus, uint32_t* out_sector, uint32_t* out_cluster) {
    uint32_t new_clus = fat_alloc_chain(fs, 1);
    if (new_clus == 0) return false;
    if (!fat_set_entry(fs, last_clus, new_clus)) {
        fat_free_chain(fs, new_clus);
        return false;
    }
    *out_sector = cluster_to_sector(fs, new_clus);
    if (out_cluster) *out_cluster = new_clus;
    return true;
}

// ============================================================================
// === ВСТАВКА МНОГОСЛОТОВЫХ ЗАПИСЕЙ (LFN + 8.3) — для имён длиннее 8.3 ===
// ============================================================================

// Курсор на конкретный 32-байтный слот внутри цепочки кластеров каталога.
struct DirCursor {
    uint32_t cluster;
    uint32_t sector_in_cluster;
    int offset_in_sector;
};

static uint32_t dir_cursor_sector(FAT32_Instance* fs, const DirCursor* c) {
    return cluster_to_sector(fs, c->cluster) + c->sector_in_cluster;
}

// Переходит к следующему 32-байтному слоту, при необходимости пересекая границы
// секторов и кластеров (через цепочку FAT).
static bool dir_cursor_advance(FAT32_Instance* fs, DirCursor* c) {
    c->offset_in_sector += 32;
    if (c->offset_in_sector < 512) return true;
    c->offset_in_sector = 0;
    c->sector_in_cluster++;
    if (c->sector_in_cluster < fs->sectors_per_cluster) return true;
    c->sector_in_cluster = 0;
    uint32_t next = fat_get_entry(fs, c->cluster);
    if (!cluster_has_next(next)) return false;
    c->cluster = next;
    return true;
}

static bool dir_write_entry_at(FAT32_Instance* fs, const DirCursor* c, const uint8_t entry[32]) {
    char sector_buf[512];
    uint32_t sector = dir_cursor_sector(fs, c);
    if (!fs->read_blocks(sector, 1, sector_buf)) return false;
    my_memcpy(sector_buf + c->offset_in_sector, entry, 32);
    return fs->write_blocks(sector, 1, sector_buf);
}

// Ищет непрерывный пробег из slots_needed свободных 32-байтных слотов подряд (разрешая
// переход через границы секторов/кластеров — так же, как это допускает сама
// спецификация FAT32 для последовательностей LFN+8.3). Если такого пробега нет ни в
// одном месте существующей цепочки, расширяет каталог одним новым кластером.
static bool dir_find_or_grow_run(FAT32_Instance* fs, uint32_t dir_cluster, int slots_needed, DirCursor* out) {
    uint32_t clus = (dir_cluster < 2) ? fs->root_cluster : dir_cluster;
    if (clus < 2) return false;
    if (fat_chain_has_cycle(fs, clus)) return false;

    char sector_buf[512];
    int run_len = 0;
    DirCursor run_start = {0, 0, 0};
    bool past_end = false; // true, как только встретили настоящий 0x00 (дальше всё гарантированно свободно)
    uint32_t last_clus = clus;

    int guard = 0;
    while (clus >= 2 && guard++ < 4096) {
        last_clus = clus;
        for (uint32_t s = 0; s < fs->sectors_per_cluster; s++) {
            uint32_t sector = cluster_to_sector(fs, clus) + s;
            if (!fs->read_blocks(sector, 1, sector_buf)) return false;

            for (int i = 0; i < 512; i += 32) {
                uint8_t* entry = (uint8_t*)&sector_buf[i];
                if (entry[0] == 0x00) past_end = true;
                bool free_slot = past_end || entry[0] == 0xE5;

                if (free_slot) {
                    if (run_len == 0) { run_start.cluster = clus; run_start.sector_in_cluster = s; run_start.offset_in_sector = i; }
                    run_len++;
                    if (run_len >= slots_needed) { *out = run_start; return true; }
                } else {
                    run_len = 0;
                }
            }
        }
        uint32_t next = fat_get_entry(fs, clus);
        if (!cluster_has_next(next)) break;
        clus = next;
    }

    // Пробега нужной длины нет — расширяем каталог одним новым кластером. Если run_len
    // > 0, он уже начат в хвосте last_clus и корректно продолжится в новый кластер
    // (тот линкуется сразу за last_clus в цепочке FAT).
    uint32_t new_sector, new_clus;
    if (!dir_grow(fs, last_clus, &new_sector, &new_clus)) return false;
    if (run_len == 0) { run_start.cluster = new_clus; run_start.sector_in_cluster = 0; run_start.offset_in_sector = 0; }
    *out = run_start;
    return true;
}

// Контрольная сумма короткого 8.3-имени для связи LFN-записей со своей 8.3-записью
// (см. спецификацию FAT32).
static uint8_t lfn_checksum(const uint8_t short_name[11]) {
    uint8_t sum = 0;
    for (int i = 0; i < 11; i++) {
        sum = (uint8_t)(((sum & 1) ? 0x80 : 0) + (sum >> 1) + short_name[i]);
    }
    return sum;
}

// true, если имя НЕ может быть без потерь представлено как 8.3 (длиннее 8+3 символов,
// больше одной точки, содержит недопустимые символы, или отличается регистром от
// своей 8.3-нормализации) — в этом случае нужна LFN-последовательность.
static bool name_needs_lfn(const char* name) {
    if (my_strlen(name) > 12) return true;

    int dots = 0;
    for (int i = 0; name[i]; i++) if (name[i] == '.') dots++;
    if (dots > 1) return true;

    char formatted[12];
    format_fat32_name(name, formatted);

    char reconstructed[13];
    int pos = 0;
    for (int i = 0; i < 8 && formatted[i] != ' '; i++) reconstructed[pos++] = formatted[i];
    if (formatted[8] != ' ') {
        reconstructed[pos++] = '.';
        for (int i = 8; i < 11 && formatted[i] != ' '; i++) reconstructed[pos++] = formatted[i];
    }
    reconstructed[pos] = '\0';

    return my_strcmp(reconstructed, name) != 0; // Регистрозависимо — сохраняем точный регистр через LFN
}

// true, если в каталоге dir_cluster уже существует запись с точно такими же 11 сырыми
// байтами короткого имени (raw11) — используется при подборе уникального алиаса.
static bool dir_has_raw_83(FAT32_Instance* fs, uint32_t dir_cluster, const char raw11[11]) {
    uint32_t clus = (dir_cluster < 2) ? fs->root_cluster : dir_cluster;
    if (clus < 2 || fat_chain_has_cycle(fs, clus)) return true; // Считаем "занято" при проблеме, чтобы не зациклиться дальше

    char sector_buf[512];
    bool done = false;
    int guard = 0;
    while (!done && clus >= 2 && guard++ < 4096) {
        for (uint32_t s = 0; !done && s < fs->sectors_per_cluster; s++) {
            uint32_t sector = cluster_to_sector(fs, clus) + s;
            if (!fs->read_blocks(sector, 1, sector_buf)) { done = true; break; }
            for (int i = 0; i < 512; i += 32) {
                uint8_t* entry = (uint8_t*)&sector_buf[i];
                if (entry[0] == 0x00) { done = true; break; }
                if (entry[0] == 0xE5 || entry[11] == 0x0F) continue;
                bool match = true;
                for (int k = 0; k < 11; k++) { if ((uint8_t)entry[k] != (uint8_t)raw11[k]) { match = false; break; } }
                if (match) return true;
            }
        }
        if (done) break;
        uint32_t next = fat_get_entry(fs, clus);
        if (!cluster_has_next(next)) break;
        clus = next;
    }
    return false;
}

// Генерирует уникальный короткий алиас 8.3 в стиле Windows (BASENAME~N.EXT) для
// длинного имени name внутри каталога dir_cluster.
static bool generate_short_alias(FAT32_Instance* fs, uint32_t dir_cluster, const char* name, uint8_t out_11[11]) {
    char primary[9]; int p_len = 0;
    char ext[4]; int e_len = 0;
    bool in_ext = false;

    for (int i = 0; name[i]; i++) {
        char c = name[i];
        if (c == '.') { in_ext = true; e_len = 0; continue; }
        char up = (c >= 'a' && c <= 'z') ? (char)(c - 32) : c;
        bool valid = (up >= 'A' && up <= 'Z') || (up >= '0' && up <= '9');
        if (!valid) continue; // Пропускаем недопустимые для 8.3 символы
        if (in_ext) { if (e_len < 3) ext[e_len++] = up; }
        else { if (p_len < 8) primary[p_len++] = up; }
    }
    if (p_len == 0) { primary[0] = '_'; p_len = 1; }

    for (int suffix = 1; suffix <= 999999; suffix++) {
        char num[8];
        int n_len = 0;
        int tmp = suffix;
        do { num[n_len++] = (char)('0' + (tmp % 10)); tmp /= 10; } while (tmp > 0);

        int tail_len = 1 + n_len; // '~' + цифры
        int base_len = p_len;
        if (base_len + tail_len > 8) base_len = 8 - tail_len;
        if (base_len < 1) return false; // Практически недостижимо при разумном числе файлов

        char candidate[11];
        int pos = 0;
        for (int i = 0; i < base_len; i++) candidate[pos++] = primary[i];
        candidate[pos++] = '~';
        for (int i = n_len - 1; i >= 0; i--) candidate[pos++] = num[i];
        for (int i = pos; i < 8; i++) candidate[i] = ' ';
        for (int i = 0; i < 3; i++) candidate[8 + i] = (i < e_len) ? ext[i] : ' ';

        if (!dir_has_raw_83(fs, dir_cluster, candidate)) {
            my_memcpy(out_11, candidate, 11);
            return true;
        }
    }
    return false;
}

// Записывает последовательность LFN-записей (если имя того требует) + 8.3-запись для
// basename в каталог dir_cluster, указывающую на кластер file_clus/размер file_size.
static bool dir_write_named_entry(FAT32_Instance* fs, uint32_t dir_cluster, const char* basename,
                                   uint32_t file_clus, uint32_t file_size, uint8_t attr) {
    bool need_lfn = name_needs_lfn(basename);
    uint8_t short_name[11];

    if (need_lfn) {
        if (!generate_short_alias(fs, dir_cluster, basename, short_name)) return false;
    } else {
        char formatted[12];
        format_fat32_name(basename, formatted);
        my_memcpy(short_name, formatted, 11);
    }

    int name_len = my_strlen(basename);
    int lfn_count = need_lfn ? (name_len + 12) / 13 : 0;
    int slots_needed = lfn_count + 1;
    if (slots_needed > 21) return false; // Не должно случиться — basename ограничено 63 байтами

    DirCursor cur;
    if (!dir_find_or_grow_run(fs, dir_cluster, slots_needed, &cur)) return false;

    if (need_lfn) {
        uint8_t checksum = lfn_checksum(short_name);
        static const int positions[13] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};

        // LFN-записи пишутся в ОБРАТНОМ порядке: последний фрагмент имени — первым,
        // с установленным битом 0x40 ("последняя логическая запись" в цепочке LFN).
        for (int seq = lfn_count; seq >= 1; seq--) {
            uint8_t lfn_entry[32];
            for (int i = 0; i < 32; i++) lfn_entry[i] = 0;
            lfn_entry[0] = (uint8_t)(seq | (seq == lfn_count ? 0x40 : 0));
            lfn_entry[11] = 0x0F; // ATTR_LFN
            lfn_entry[13] = checksum;

            int base = (seq - 1) * 13;
            for (int k = 0; k < 13; k++) {
                int ch_idx = base + k;
                uint16_t ch;
                if (ch_idx < name_len) ch = (uint8_t)basename[ch_idx];
                else if (ch_idx == name_len) ch = 0x0000; // Терминатор имени
                else ch = 0xFFFF; // Заполнитель после терминатора
                lfn_entry[positions[k]] = (uint8_t)(ch & 0xFF);
                lfn_entry[positions[k] + 1] = (uint8_t)(ch >> 8);
            }

            if (!dir_write_entry_at(fs, &cur, lfn_entry)) return false;
            if (!dir_cursor_advance(fs, &cur)) return false;
        }
    }

    uint8_t entry83[32];
    for (int i = 0; i < 32; i++) entry83[i] = 0;
    my_memcpy(entry83, short_name, 11);
    entry83[11] = attr;
    *(uint16_t*)&entry83[20] = (uint16_t)(file_clus >> 16);
    *(uint16_t*)&entry83[26] = (uint16_t)(file_clus & 0xFFFF);
    *(uint32_t*)&entry83[28] = file_size;

    return dir_write_entry_at(fs, &cur, entry83);
}

// Форматирует список каталога dir_cluster (тип/имя/размер) в out_buffer, обходя ВСЮ
// цепочку кластеров каталога и все сектора внутри каждого кластера — в отличие от
// старого кода, который читал ровно один сектор и потому "терял" файлы в каталогах,
// не помещающихся в первые 512 байт. max_len — полный размер out_buffer; при нехватке
// места вывод обрезается с маркером "..." вместо выхода за границы буфера.
// ВАЖНО (см. read_chain_data выше): пишем накопленный текст в приватный статический
// буфер, а НЕ напрямую в out_buffer. out_buffer у вызывающей стороны (blk_driver.cpp)
// часто указывает на g_shm_vaddr, а каждый fs->read_blocks() внутри обхода каталога
// использует ТУ ЖЕ страницу как DMA-скретч — запись результата напрямую в out_buffer
// в процессе обхода приводила бы к тому, что каждое следующее чтение сектора затирало
// уже сформированный текст в начале буфера.
bool fat32_format_dir_listing(FAT32_Instance* fs, uint32_t dir_cluster, char* out_buffer, uint32_t max_len) {
    uint32_t clus = (dir_cluster < 2) ? fs->root_cluster : dir_cluster;
    out_buffer[0] = '\0';
    if (clus < 2 || max_len < 300) return false;
    if (fat_chain_has_cycle(fs, clus)) return false; // Повреждённая (зацикленная) цепочка каталога
    if (max_len > FAT32_MAX_IO_CHUNK) max_len = FAT32_MAX_IO_CHUNK;

    static char staging[FAT32_MAX_IO_CHUNK];
    uint32_t offset = 0;
    char lfn_buf[261] = {0};
    char sector_buf[512];
    bool done = false;
    bool truncated = false;

    int guard = 0;
    while (!done && clus >= 2 && guard++ < 4096) {
        for (int s = 0; !done && s < fs->sectors_per_cluster; s++) {
            uint32_t sector = cluster_to_sector(fs, clus) + s;
            if (!fs->read_blocks(sector, 1, sector_buf)) { done = true; break; }

            for (int i = 0; i < 512; i += 32) {
                uint8_t* entry = (uint8_t*)&sector_buf[i];
                if (entry[0] == 0x00) { done = true; break; } // Истинный конец каталога

                if (entry[0] == 0xE5) { for (int k = 0; k < 261; k++) lfn_buf[k] = 0; continue; }

                if (entry[11] == 0x0F) { // LFN
                    int seq = (entry[0] & 0x1F) - 1;
                    if (seq >= 0 && seq < 20) {
                        char* p = lfn_buf + (seq * 13);
                        for (int k = 1;  k < 11; k += 2) { if (entry[k] != 0xFF && entry[k] != 0) *p++ = entry[k]; }
                        for (int k = 14; k < 26; k += 2) { if (entry[k] != 0xFF && entry[k] != 0) *p++ = entry[k]; }
                        for (int k = 28; k < 32; k += 2) { if (entry[k] != 0xFF && entry[k] != 0) *p++ = entry[k]; }
                    }
                    continue;
                }

                if (entry[11] & 0x08) { for (int k = 0; k < 261; k++) lfn_buf[k] = 0; continue; }

                bool is_dir = (entry[11] & 0x10);
                char final_name[261];
                final_name[0] = '\0';
                if (lfn_buf[0] != '\0') {
                    my_strcpy(final_name, lfn_buf);
                    for (int k = 0; k < 261; k++) lfn_buf[k] = 0;
                } else {
                    int pos = 0;
                    for (int j = 0; j < 8 && entry[j] != ' '; j++) final_name[pos++] = entry[j];
                    if (entry[8] != ' ') {
                        final_name[pos++] = '.';
                        for (int j = 8; j < 11 && entry[j] != ' '; j++) final_name[pos++] = entry[j];
                    }
                    final_name[pos] = '\0';
                }
                if (final_name[0] == '\0') continue; // Артефакт/мусор — игнорируем

                // Резервируем запас на самую длинную возможную запись, чтобы не выйти
                // за пределы staging при большом количестве/длинных именах файлов.
                if (offset > max_len - 300) { done = true; truncated = true; break; }

                const char* type_str = is_dir ? " [DIR] " : " [FAT] ";
                my_strcpy(staging + offset, type_str); offset += my_strlen(type_str);
                my_strcpy(staging + offset, final_name); offset += my_strlen(final_name);

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
                        while (temp > 0) { rev[r_idx++] = '0' + (temp % 10); temp /= 10; }
                        while (r_idx > 0) size_str[idx++] = rev[--r_idx];
                    }
                    size_str[idx] = '\0';

                    my_strcpy(staging + offset, " \t("); offset += 3;
                    my_strcpy(staging + offset, size_str); offset += my_strlen(size_str);
                    my_strcpy(staging + offset, " bytes)"); offset += 7;
                }
                my_strcpy(staging + offset, "\n"); offset += 1;
            }
        }
        if (done) break;
        uint32_t next = fat_get_entry(fs, clus);
        if (!cluster_has_next(next)) break;
        clus = next;
    }

    if (truncated) { my_strcpy(staging + offset, "...\n"); offset += 4; }

    my_memcpy(out_buffer, staging, offset + 1); // +1, чтобы захватить завершающий '\0'
    return true;
}

bool fat32_list_directory(FAT32_Instance* fs, const char* path, char* out_buffer) {
    (void)path; // Как и раньше: всегда работает с текущей директорией (CWD)
    my_strcpy(out_buffer, "Directory listing:\n");
    uint32_t used = my_strlen(out_buffer);
    return fat32_format_dir_listing(fs, fs->current_dir_cluster, out_buffer + used, 4096 - used);
}

bool fat32_read_file(FAT32_Instance* fs, const char* filename, char* out_buffer, uint32_t offset, uint32_t* bytes_read) {
    *bytes_read = 0;

    DirSlot slot;
    bool have_slot = false;
    if (g_fat_path_cache.fs == fs && g_fat_path_cache.cwd_at_lookup == fs->current_dir_cluster &&
        my_strcmp(g_fat_path_cache.path, filename) == 0) {
        slot = g_fat_path_cache.slot;
        have_slot = true;
    }

    if (!have_slot) {
        char basename[64];
        uint32_t parent_clus = fat32_resolve_parent(fs, filename, basename);
        if (parent_clus == 0xFFFFFFFF || basename[0] == '\0') return false;
        if (parent_clus == 0) parent_clus = fs->root_cluster;

        if (!dir_scan(fs, parent_clus, basename, &slot) || !slot.found) return false;

        g_fat_path_cache.fs = fs;
        int fnlen = 0;
        while (filename[fnlen] && fnlen < 63) { g_fat_path_cache.path[fnlen] = filename[fnlen]; fnlen++; }
        g_fat_path_cache.path[fnlen] = '\0';
        g_fat_path_cache.cwd_at_lookup = fs->current_dir_cluster;
        g_fat_path_cache.slot = slot;
    }

    if (offset >= slot.size) return true; // Достигнут конец файла (EOF)

    uint32_t remaining = slot.size - offset;
    // Ограничиваем чтение размером страницы разделяемой памяти (SHM = 4096)
    uint32_t chunk_size = (remaining > 4096) ? 4096 : remaining;

    // Читаем данные, следуя реальной цепочке кластеров FAT (а не предполагая, что
    // файл занимает подряд идущие кластеры) — корректно работает и для
    // фрагментированных файлов.
    uint32_t copied = read_chain_data(fs, slot.clus, offset, out_buffer, chunk_size);
    if (copied == 0) return false;

    *bytes_read = copied;
    return true;
}

// === ВСПОМОГАТЕЛЬНЫЕ ФУНКЦИИ ===
static void my_memcpy(void *dest, const void *src, int n) {
    unsigned char *d = (unsigned char *)dest;
    const unsigned char *s = (const unsigned char *)src;
    if (d == s) return;
    while (n--) *d++ = *s++;
}

// Конвертер: "hello.txt" -> "HELLO   TXT"
static void format_fat32_name(const char* input, char* output) {
    for (int i = 0; i < 11; i++) output[i] = ' ';
    output[11] = '\0';
    
    if (my_strcmp(input, ".") == 0) { output[0] = '.'; return; }
    if (my_strcmp(input, "..") == 0) { output[0] = '.'; output[1] = '.'; return; }

    int i = 0, j = 0;
    while (input[i] && input[i] != '.' && j < 8) {
        output[j++] = (input[i] >= 'a' && input[i] <= 'z') ? input[i] - 32 : input[i];
        i++;
    }
    while (input[i] && input[i] != '.') i++; 
    if (input[i] == '.') {
        i++; j = 8;
        while (input[i] && j < 11) {
            output[j++] = (input[i] >= 'a' && input[i] <= 'z') ? input[i] - 32 : input[i];
            i++;
        }
    }
}

// Ищет запись внутри конкретной папки (dir_cluster) и возвращает её кластер (или 0xFFFFFFFF, если не найдена)
uint32_t fat32_find_in_dir(FAT32_Instance* fs, uint32_t dir_cluster, const char* target_name) {
    // === ФИКС ДЛЯ "cd .." ИЗ КОРНЯ ===
    if (my_strcmp(target_name, "..") == 0 && (dir_cluster == fs->root_cluster || dir_cluster == 0)) {
        return fs->root_cluster; // Безопасно возвращаем корень, предотвращая ошибку
    }

    // dir_scan обходит ВСЮ цепочку кластеров каталога (а не только первый сектор),
    // что необходимо для директорий, не помещающихся в 512 байт.
    DirSlot slot;
    if (!dir_scan(fs, dir_cluster, target_name, &slot) || !slot.found) return 0xFFFFFFFF;
    return slot.clus;
}

// Главный VFS Резолвер Путей
// Возвращает кластер родительской папки (или 0xFFFFFFFF при ошибке)
// Записывает конечное имя файла/папки в out_basename
uint32_t fat32_resolve_parent(FAT32_Instance* fs, const char* full_path, char* out_basename) {
    uint32_t current_clus = fs->current_dir_cluster;
    if (current_clus == 0) current_clus = fs->root_cluster;

    const char* p = full_path;
    
    // Если путь абсолютный (с корня)
    if (*p == '/') {
        current_clus = fs->root_cluster;
        while (*p == '/') p++; 
    }

    char token[64];
    while (*p) {
        int i = 0;
        // Извлекаем токен до следующего слеша
        while (*p && *p != '/' && i < 63) token[i++] = *p++;
        token[i] = '\0';

        while (*p == '/') p++; // Пропускаем дублирующиеся слеши (напр. ///)

        // Если строка кончилась, токен — это наш целевой файл/папка (basename)
        if (*p == '\0') {
            my_strcpy(out_basename, token);
            return current_clus; 
        }

        // Иначе это промежуточная директория, пытаемся в неё "провалиться"
        uint32_t next_clus = fat32_find_in_dir(fs, current_clus, token);
        if (next_clus == 0xFFFFFFFF) return 0xFFFFFFFF; // Промежуточная папка не существует!
        
        // Если `find_in_dir` вернул 0, это значит, что мы перешли в корневой каталог (например, `cd ..` из `/mydir`)
        if (next_clus == 0) next_clus = fs->root_cluster;
        current_clus = next_clus;
    }

    // Если ввели просто "/", "a/" или что-то подобное
    out_basename[0] = '\0';
    return current_clus;
}

// === ЧТЕНИЕ ТЕКСТА (cat) ===
bool fat32_read_text_file(FAT32_Instance* fs, const char* path, char* out_buffer) {
    char basename[64];
    uint32_t parent_clus = fat32_resolve_parent(fs, path, basename);
    if (parent_clus == 0xFFFFFFFF || basename[0] == '\0') return false;
    if (parent_clus == 0) parent_clus = fs->root_cluster;

    DirSlot slot;
    if (!dir_scan(fs, parent_clus, basename, &slot) || !slot.found) return false;

    if (slot.size == 0 || slot.clus < 2) { out_buffer[0] = '\0'; return true; }

    uint32_t size = slot.size;
    if (size > 4000) size = 4000; // Безопасный лимит для текста

    // Следуем реальной цепочке кластеров FAT — корректно работает и для
    // фрагментированных файлов, в отличие от старого "contiguous"-предположения.
    uint32_t copied = read_chain_data(fs, slot.clus, 0, out_buffer, size);
    out_buffer[copied] = '\0'; // Закрываем строку нулем для безопасного вывода
    return true;
}

// === СОЗДАНИЕ ПУСТОГО ФАЙЛА (touch) ===
bool fat32_create_file(FAT32_Instance* fs, const char* path) {
    fat_invalidate_path_cache();
    char basename[64];
    uint32_t parent_clus = fat32_resolve_parent(fs, path, basename);
    if (parent_clus == 0xFFFFFFFF || basename[0] == '\0') return false; // Неверный путь
    if (parent_clus == 0) parent_clus = fs->root_cluster;

    DirSlot slot;
    if (!dir_scan(fs, parent_clus, basename, &slot)) return false;
    if (slot.found) return true; // Уже существует

    uint32_t free_sector;
    int free_offset;
    if (slot.has_free) {
        free_sector = slot.free_sector;
        free_offset = slot.free_offset;
    } else {
        // Ни в одном из существующих кластеров каталога нет места — расширяем каталог.
        if (!dir_grow(fs, slot.last_clus, &free_sector, nullptr)) return false;
        free_offset = 0;
    }

    char sector_buf[512];
    if (!fs->read_blocks(free_sector, 1, sector_buf)) return false;

    uint8_t* new_entry = (uint8_t*)&sector_buf[free_offset];
    for (int i = 0; i < 32; i++) new_entry[i] = 0;
    char target_name[12];
    format_fat32_name(basename, target_name);
    my_memcpy(new_entry, target_name, 11);
    new_entry[11] = 0x20; // Атрибут Archive
    return fs->write_blocks(free_sector, 1, sector_buf);
}

// === ЗАПИСЬ В ФАЙЛ (echo > file) ===
// Полностью перезаписывает содержимое файла: старая цепочка кластеров (если была)
// освобождается, под новые данные выделяется цепочка ровно нужного размера —
// в отличие от старого кода, который писал только в один кластер, тихо затирая
// соседние (чужие) кластеры на диске при len больше одного кластера.
bool fat32_write_file(FAT32_Instance* fs, const char* path, const char* text, uint32_t len) {
    fat_invalidate_path_cache();
    char basename[64];
    uint32_t parent_clus = fat32_resolve_parent(fs, path, basename);
    if (parent_clus == 0xFFFFFFFF || basename[0] == '\0') return false;
    if (parent_clus == 0) parent_clus = fs->root_cluster;

    DirSlot slot;
    if (!dir_scan(fs, parent_clus, basename, &slot)) return false;

    uint32_t entry_sector;
    int entry_offset;
    uint32_t old_clus = 0;

    if (slot.found) {
        entry_sector = slot.sector;
        entry_offset = slot.offset;
        old_clus = slot.clus;
    } else if (slot.has_free) {
        entry_sector = slot.free_sector;
        entry_offset = slot.free_offset;
    } else {
        if (!dir_grow(fs, slot.last_clus, &entry_sector, nullptr)) return false;
        entry_offset = 0;
    }

    char sector_buf[512];
    if (!fs->read_blocks(entry_sector, 1, sector_buf)) return false;
    uint8_t* target_entry = (uint8_t*)&sector_buf[entry_offset];

    if (!slot.found) {
        for (int i = 0; i < 32; i++) target_entry[i] = 0;
        char formatted_name[12];
        format_fat32_name(basename, formatted_name);
        my_memcpy(target_entry, formatted_name, 11);
        target_entry[11] = 0x20; // ATTR_ARCHIVE
    }

    // echo > всегда перезаписывает файл целиком — освобождаем старую цепочку,
    // чтобы не оставлять "хвост" из старых кластеров длиннее новых данных.
    if (old_clus >= 2) fat_free_chain(fs, old_clus);

    uint32_t new_clus = 0;
    if (len > 0) {
        uint32_t bytes_per_cluster = (uint32_t)fs->bytes_per_sector * fs->sectors_per_cluster;
        uint32_t need_clusters = (len + bytes_per_cluster - 1) / bytes_per_cluster;
        new_clus = fat_alloc_chain(fs, need_clusters);
        if (new_clus == 0) return false; // Диск заполнен
    }

    *(uint16_t*)&target_entry[20] = (uint16_t)(new_clus >> 16);
    *(uint16_t*)&target_entry[26] = (uint16_t)(new_clus & 0xFFFF);
    *(uint32_t*)&target_entry[28] = len;

    if (!fs->write_blocks(entry_sector, 1, sector_buf)) return false;

    if (len > 0) {
        return write_chain_data(fs, new_clus, text, len);
    }
    return true; // Успех (например, для файла нулевой длины)
}

// === УДАЛЕНИЕ ФАЙЛА (rm) ===
// В отличие от старого кода, после удаления записи каталога освобождает всю цепочку
// кластеров файла (fat_free_chain) — иначе место на диске тихо утекало бы навсегда.
bool fat32_delete_file(FAT32_Instance* fs, const char* path) {
    fat_invalidate_path_cache();
    char basename[64];
    uint32_t parent_clus = fat32_resolve_parent(fs, path, basename);
    if (parent_clus == 0xFFFFFFFF || basename[0] == '\0') return false;
    if (parent_clus == 0) parent_clus = fs->root_cluster;

    uint32_t clus = (parent_clus < 2) ? fs->root_cluster : parent_clus;
    if (clus < 2) return false;
    if (fat_chain_has_cycle(fs, clus)) return false; // Повреждённая (зацикленная) цепочка каталога

    char formatted_name[12];
    format_fat32_name(basename, formatted_name);
    char lfn_buf[261] = {0};
    char sector_buf[512];
    // Смещение начала LFN-цепочки внутри ТЕКУЩЕГО sector_buf (-1, если её не было
    // или она начиналась в предыдущем секторе — этот редкий случай не подчищается,
    // см. ограничение ниже).
    int lfn_start_idx = -1;

    int guard = 0;
    while (clus >= 2 && guard++ < 4096) {
        for (int s = 0; s < fs->sectors_per_cluster; s++) {
            uint32_t sector = cluster_to_sector(fs, clus) + s;
            if (!fs->read_blocks(sector, 1, sector_buf)) return false;
            lfn_start_idx = -1; // LFN-цепочка, начавшаяся в предыдущем секторе, здесь не отслеживается

            for (int i = 0; i < 512; i += 32) {
                uint8_t* entry = (uint8_t*)&sector_buf[i];
                if (entry[0] == 0x00) return false; // Истинный конец каталога — не найдено
                if (entry[0] == 0xE5) { lfn_start_idx = -1; for(int k=0; k<261; k++) lfn_buf[k] = 0; continue; }

                if (entry[11] == 0x0F) {
                    if (lfn_start_idx == -1) lfn_start_idx = i;
                    int seq = (entry[0] & 0x1F) - 1;
                    if (seq >= 0 && seq < 20) {
                        char* p = lfn_buf + (seq * 13);
                        for(int k=1;  k<11; k+=2) { if(entry[k] != 0xFF && entry[k] != 0) *p++ = entry[k]; }
                        for(int k=14; k<26; k+=2) { if(entry[k] != 0xFF && entry[k] != 0) *p++ = entry[k]; }
                        for(int k=28; k<32; k+=2) { if(entry[k] != 0xFF && entry[k] != 0) *p++ = entry[k]; }
                    }
                    continue;
                }

                if (entry[11] & 0x08) { lfn_start_idx = -1; for(int k=0; k<261; k++) lfn_buf[k] = 0; continue; }

                bool match = false;
                if (lfn_buf[0] != '\0') {
                    if (my_strcasecmp(lfn_buf, basename) == 0) match = true;
                } else {
                    char entry_name[12];
                    my_memcpy(entry_name, entry, 11); entry_name[11] = '\0';
                    if (my_strcmp(entry_name, formatted_name) == 0) match = true;
                }

                if (match) {
                    entry[0] = 0xE5; // 0xE5 - Стандартная метка "Удален" в FAT32
                    if (lfn_start_idx != -1) {
                        for (int j = lfn_start_idx; j < i; j += 32) sector_buf[j] = 0xE5;
                    }
                    uint16_t clus_hi = *(uint16_t*)&entry[20];
                    uint16_t clus_lo = *(uint16_t*)&entry[26];
                    uint32_t file_clus = ((uint32_t)clus_hi << 16) | clus_lo;

                    if (!fs->write_blocks(sector, 1, sector_buf)) return false;
                    if (file_clus >= 2) fat_free_chain(fs, file_clus);
                    return true;
                }
                lfn_start_idx = -1;
                for(int k=0; k<261; k++) lfn_buf[k] = 0;
            }
        }
        uint32_t next = fat_get_entry(fs, clus);
        if (!cluster_has_next(next)) break;
        clus = next;
    }
    return false;
}

// === ПЕРЕИМЕНОВАНИЕ (mv) ===
// Поддерживает переименование только в пределах той же папки (как и раньше).
// Переименование в имя, требующее LFN длиннее 8.3 старой записи, не создаёт новых
// LFN-записей (не входит в объявленный объём переработки) — сохраняется старое
// ограничение исходного кода.
bool fat32_rename_file(FAT32_Instance* fs, const char* old_path, const char* new_path) {
    fat_invalidate_path_cache();
    char old_basename[64];
    uint32_t old_parent_clus = fat32_resolve_parent(fs, old_path, old_basename);
    if (old_parent_clus == 0xFFFFFFFF || old_basename[0] == '\0') return false;

    char new_basename[64];
    uint32_t new_parent_clus = fat32_resolve_parent(fs, new_path, new_basename);
    if (new_parent_clus == 0xFFFFFFFF || new_basename[0] == '\0') return false;

    if (old_parent_clus != new_parent_clus) return false; // Поддерживаем переименование только в той же папке
    if (old_parent_clus == 0) old_parent_clus = fs->root_cluster;

    DirSlot slot;
    if (!dir_scan(fs, old_parent_clus, old_basename, &slot) || !slot.found) return false;

    char sector_buf[512];
    if (!fs->read_blocks(slot.sector, 1, sector_buf)) return false;
    uint8_t* entry = (uint8_t*)&sector_buf[slot.offset];
    uint8_t old_attr = entry[11];

    // Если новое имя НЕ требует LFN, просто переписываем 8.3-имя на месте — это
    // сохраняет позицию записи и не требует поиска места под LFN-цепочку.
    if (!name_needs_lfn(new_basename)) {
        char new_name_fmt[12];
        format_fat32_name(new_basename, new_name_fmt);
        my_memcpy(entry, new_name_fmt, 11);
        return fs->write_blocks(slot.sector, 1, sector_buf);
    }

    // Новое имя требует LFN: удаляем старую запись (и её собственную LFN-цепочку в
    // пределах того же сектора — как и в fat32_delete_file) и вставляем новую
    // последовательность LFN+8.3, сохраняя кластер/размер/атрибуты файла.
    entry[0] = 0xE5;
    for (int j = slot.offset - 32; j >= 0; j -= 32) {
        uint8_t* prev = (uint8_t*)&sector_buf[j];
        if (prev[11] == 0x0F && prev[0] != 0xE5) prev[0] = 0xE5;
        else break;
    }
    if (!fs->write_blocks(slot.sector, 1, sector_buf)) return false;

    return dir_write_named_entry(fs, old_parent_clus, new_basename, slot.clus, slot.size, old_attr);
}

// === СОЗДАНИЕ ДИРЕКТОРИИ (mkdir) ===
bool fat32_mkdir(FAT32_Instance* fs, const char* path) {
    fat_invalidate_path_cache();
    char basename[64];
    uint32_t parent_clus = fat32_resolve_parent(fs, path, basename);
    if (parent_clus == 0xFFFFFFFF || basename[0] == '\0') return false;

    uint32_t original_parent_clus = parent_clus;
    if (parent_clus == 0) parent_clus = fs->root_cluster;

    // 1. Ищем существующую запись / свободное место, обходя всю цепочку каталога
    DirSlot slot;
    if (!dir_scan(fs, parent_clus, basename, &slot)) return false;
    if (slot.found) return false; // Уже существует

    uint32_t free_sector;
    int free_offset;
    if (slot.has_free) {
        free_sector = slot.free_sector;
        free_offset = slot.free_offset;
    } else {
        if (!dir_grow(fs, slot.last_clus, &free_sector, nullptr)) return false; // Папка переполнена и не может расти
        free_offset = 0;
    }

    // 2. Аллоцируем новый кластер под содержимое директории (уже обнулён fat_alloc_chain)
    uint32_t new_clus = fat_alloc_chain(fs, 1);
    if (new_clus == 0) return false;

    // 3. Создаем запись о папке в родительском каталоге
    char sector_buf[512];
    if (!fs->read_blocks(free_sector, 1, sector_buf)) { fat_free_chain(fs, new_clus); return false; }
    uint8_t* new_entry = (uint8_t*)&sector_buf[free_offset];
    for (int i = 0; i < 32; i++) new_entry[i] = 0;
    char target_name[12];
    format_fat32_name(basename, target_name);
    my_memcpy(new_entry, target_name, 11);
    new_entry[11] = 0x10; // ATTR_DIRECTORY
    *(uint16_t*)&new_entry[20] = (uint16_t)(new_clus >> 16);
    *(uint16_t*)&new_entry[26] = (uint16_t)(new_clus & 0xFFFF);
    if (!fs->write_blocks(free_sector, 1, sector_buf)) { fat_free_chain(fs, new_clus); return false; }

    // 4. Инициализируем новую папку (записи "." и "..")
    char new_dir_buf[512];
    for (int i = 0; i < 512; i++) new_dir_buf[i] = 0;

    // Запись "."
    my_memcpy(&new_dir_buf[0], ".          ", 11);
    new_dir_buf[11] = 0x10;
    *(uint16_t*)&new_dir_buf[20] = (uint16_t)(new_clus >> 16);
    *(uint16_t*)&new_dir_buf[26] = (uint16_t)(new_clus & 0xFFFF);

    // Запись ".."
    my_memcpy(&new_dir_buf[32], "..         ", 11);
    new_dir_buf[43] = 0x10;
    *(uint16_t*)&new_dir_buf[52] = (uint16_t)(original_parent_clus >> 16);
    *(uint16_t*)&new_dir_buf[58] = (uint16_t)(original_parent_clus & 0xFFFF);

    uint32_t new_dir_sector = cluster_to_sector(fs, new_clus);
    return fs->write_blocks(new_dir_sector, 1, new_dir_buf);
}

// === СМЕНА ДИРЕКТОРИИ (cd) ===
bool fat32_cd(FAT32_Instance* fs, const char* path) {
    if (my_strcmp(path, "/") == 0) { 
        fs->current_dir_cluster = fs->root_cluster;
        return true;
    }

    char basename[64];
    uint32_t parent_clus = fat32_resolve_parent(fs, path, basename);
    if (parent_clus == 0xFFFFFFFF) return false; 

    // Ищем целевую папку внутри найденного родителя
    uint32_t target_clus = fat32_find_in_dir(fs, parent_clus, basename);
    if (target_clus == 0xFFFFFFFF) return false;

    // В FAT32 ".." из папки первого уровня ведет в 0. Конвертируем обратно в root
    if (target_clus == 0) target_clus = fs->root_cluster; 
    
    fs->current_dir_cluster = target_clus;
    return true;
}