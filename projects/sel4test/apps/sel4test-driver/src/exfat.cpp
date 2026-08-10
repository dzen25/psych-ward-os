#include "h/exfat.h"

// Этап A (см. ROADMAP.md/issuse.txt, план ухода от FAT32/8.3): монтирование +
// чтение (list/cat/exec-загрузка/cd). Запись (touch/echo>/mkdir/rm/mv) — Этап
// B, ниже пока только заглушки. Работает ИСКЛЮЧИТЕЛЬНО со второй партицией
// карты (exFAT) — первая (FAT32, для прошивки RPi/U-Boot) этим кодом не
// монтируется и не трогается никогда, см. find_exfat_partition() в
// blk_driver.cpp.

// Вспомогательные функции (замена libc, тот же набор, что в fat32.cpp)
static int my_strlen(const char* s) { int l = 0; while (s[l]) l++; return l; }
static void my_strcpy(char* dest, const char* src) { while (*src) { *dest++ = *src++; } *dest = '\0'; }
static int my_strcmp(const char* s1, const char* s2) {
    while (*s1 && (*s1 == *s2)) { s1++; s2++; }
    return *(const unsigned char*)s1 - *(const unsigned char*)s2;
}
static void my_memcpy(void* dest, const void* src, int n) {
    char* d = (char*)dest; const char* s = (const char*)src;
    for (int i = 0; i < n; i++) d[i] = s[i];
}
static bool mem_eq(const void* a, const void* b, int n) {
    const uint8_t* pa = (const uint8_t*)a; const uint8_t* pb = (const uint8_t*)b;
    for (int i = 0; i < n; i++) if (pa[i] != pb[i]) return false;
    return true;
}
static inline char ascii_upcase(char c) { return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c; }

// Драйвер жёстко требует 512-байтные логические секторы (см. exfat_init) —
// все остальные вычисления в файле опираются на эту константу напрямую,
// вместо отдельного поля в EXFAT_Instance.
static const uint32_t EXFAT_SECTOR_SIZE = 512;
static const uint32_t EXFAT_MAX_IO_CHUNK = 4096;

#pragma pack(push, 1)
struct ExfatBootSector {
    uint8_t  jump_boot[3];
    char     fs_name[8];
    uint8_t  must_be_zero[53];
    uint64_t partition_offset;
    uint64_t volume_length;
    uint32_t fat_offset;
    uint32_t fat_length;
    uint32_t cluster_heap_offset;
    uint32_t cluster_count;
    uint32_t first_cluster_of_root_directory;
    uint32_t volume_serial_number;
    uint16_t filesystem_revision;
    uint16_t volume_flags;
    uint8_t  bytes_per_sector_shift;
    uint8_t  sectors_per_cluster_shift;
    uint8_t  number_of_fats;
    uint8_t  drive_select;
    uint8_t  percent_in_use;
    uint8_t  reserved[7];
    uint8_t  boot_code[390];
    uint16_t boot_signature;
};
#pragma pack(pop)

static uint32_t cluster_to_sector(EXFAT_Instance* fs, uint32_t clus) {
    if (clus < 2) clus = (fs->root_cluster >= 2) ? fs->root_cluster : 2;
    return fs->cluster_heap_offset + (clus - 2) * (1u << fs->sectors_per_cluster_shift);
}

// В отличие от FAT32 (значения-ПОРОГИ), у exFAT метки цепочки — ТОЧНЫЕ
// значения: 0xFFFFFFFF = конец цепочки, 0xFFFFFFF7 = битый кластер. Записи
// FAT НЕ маскируются (32 полных бита, не 28, как в FAT32).
static bool exfat_cluster_has_next(uint32_t v) { return v >= 2 && v != 0xFFFFFFFF && v != 0xFFFFFFF7; }

static uint32_t fat_get_entry(EXFAT_Instance* fs, uint32_t cluster) {
    uint32_t byte_offset = cluster * 4;
    uint32_t fat_sector = fs->fat_offset_sectors + (byte_offset / EXFAT_SECTOR_SIZE);
    uint32_t sector_offset = byte_offset % EXFAT_SECTOR_SIZE;
    char buf[512];
    if (!fs->read_blocks(fat_sector, 1, buf)) return 0xFFFFFFFF;
    uint32_t* entries = (uint32_t*)buf;
    return entries[sector_offset / 4];
}

// Этап B: нужна только для освобождения ЧУЖЕРОДНЫХ фрагментированных файлов/
// папок (NoFatChain=0, например созданных Finder'ом ДО первой загрузки) —
// свои собственные экстенты (NoFatChain=1) в FAT не участвуют вообще, см.
// bitmap_alloc_run ниже.
static bool fat_set_entry(EXFAT_Instance* fs, uint32_t cluster, uint32_t value) {
    uint32_t byte_offset = cluster * 4;
    uint32_t fat_sector = fs->fat_offset_sectors + (byte_offset / EXFAT_SECTOR_SIZE);
    uint32_t sector_offset = byte_offset % EXFAT_SECTOR_SIZE;
    char buf[512];
    if (!fs->read_blocks(fat_sector, 1, buf)) return false;
    uint32_t* entries = (uint32_t*)buf;
    entries[sector_offset / 4] = value;
    return fs->write_blocks(fat_sector, 1, buf);
}

// Алгоритм Флойда ("черепаха и заяц") — тот же приём, что в fat32.cpp,
// применяется только к РЕАЛЬНЫМ цепочкам (root, либо чужеродные
// фрагментированные файлы/папки с NoFatChain=0) — собственные экстенты
// (NoFatChain=1) в FAT не ходят вообще, там циклов быть не может.
static bool fat_chain_has_cycle(EXFAT_Instance* fs, uint32_t start_cluster) {
    if (start_cluster < 2) return false;
    uint32_t slow = start_cluster, fast = start_cluster;
    while (true) {
        uint32_t f1 = fat_get_entry(fs, fast);
        if (!exfat_cluster_has_next(f1)) return false;
        uint32_t f2 = fat_get_entry(fs, f1);
        if (!exfat_cluster_has_next(f2)) return false;
        slow = fat_get_entry(fs, slow);
        fast = f2;
        if (slow == fast) return true;
    }
}

// ============================================================================
// === КУРСОР ПО 32-БАЙТНЫМ СЛОТАМ КАТАЛОГА (пересекает границы секторов и
// кластеров, работает одинаково для NoFatChain=1 (свои каталоги, подряд
// идущие кластеры) и NoFatChain=0 (root; чужеродные фрагментированные) ===
// ============================================================================
struct DirCursor {
    EXFAT_Instance* fs;
    uint32_t first_cluster;
    bool no_fat_chain;
    uint32_t max_clusters;      // значимо только для no_fat_chain (из DataLength)
    uint32_t cur_cluster;
    uint32_t clusters_visited;
    uint32_t sector_in_cluster;
    int slot_in_sector;         // 0..15
    char sector_buf[512];
    bool sector_loaded;
};

static void dir_cursor_init(DirCursor* c, EXFAT_Instance* fs, uint32_t first_cluster, bool no_fat_chain, uint64_t byte_length) {
    c->fs = fs;
    c->first_cluster = first_cluster;
    c->no_fat_chain = no_fat_chain;
    uint32_t bytes_per_cluster = EXFAT_SECTOR_SIZE << fs->sectors_per_cluster_shift;
    c->max_clusters = no_fat_chain ? (uint32_t)((byte_length + bytes_per_cluster - 1) / bytes_per_cluster) : 0xFFFFFFFF;
    c->cur_cluster = first_cluster;
    c->clusters_visited = 0;
    c->sector_in_cluster = 0;
    c->slot_in_sector = 0;
    c->sector_loaded = false;
}

// Указатель на текущий 32-байтный слот (внутри c->sector_buf) или nullptr,
// если каталог исчерпан. Курсор не продвигает.
static uint8_t* dir_cursor_current(DirCursor* c) {
    if (c->cur_cluster < 2) return nullptr;
    if (c->no_fat_chain && c->clusters_visited >= c->max_clusters) return nullptr;
    if (!c->sector_loaded) {
        uint32_t sector = cluster_to_sector(c->fs, c->cur_cluster) + c->sector_in_cluster;
        if (!c->fs->read_blocks(sector, 1, c->sector_buf)) return nullptr;
        c->sector_loaded = true;
    }
    return (uint8_t*)c->sector_buf + c->slot_in_sector * 32;
}

static bool dir_cursor_advance(DirCursor* c) {
    c->slot_in_sector++;
    if (c->slot_in_sector < 16) return true;
    c->slot_in_sector = 0;
    c->sector_in_cluster++;
    c->sector_loaded = false;

    uint32_t sectors_per_cluster = 1u << c->fs->sectors_per_cluster_shift;
    if (c->sector_in_cluster < sectors_per_cluster) return true;
    c->sector_in_cluster = 0;
    c->clusters_visited++;

    if (c->no_fat_chain) {
        c->cur_cluster = c->first_cluster + c->clusters_visited;
        return c->clusters_visited < c->max_clusters;
    }
    uint32_t next = fat_get_entry(c->fs, c->cur_cluster);
    if (!exfat_cluster_has_next(next)) { c->cur_cluster = 0; return false; }
    c->cur_cluster = next;
    return true;
}

// Этап B: правит ТЕКУЩИЙ слот на месте (dir_cursor_current уже гарантирует
// c->sector_buf загружен и валиден) и сразу сбрасывает изменённый сектор на
// диск — по одной записи за раз, без батчинга (метаданные каталогов пишутся
// нечасто, простота важнее).
static bool dir_cursor_write_current(DirCursor* c, const uint8_t entry[32]) {
    uint8_t* slot = dir_cursor_current(c);
    if (!slot) return false;
    my_memcpy(slot, entry, 32);
    uint32_t sector = cluster_to_sector(c->fs, c->cur_cluster) + c->sector_in_cluster;
    return c->fs->write_blocks(sector, 1, c->sector_buf);
}

// ============================================================================
// === КЭШ "ПОСЛЕДНЕГО РАЗРЕШЁННОГО КАТАЛОГА" ===
// exFAT не хранит записи "."/".." — узнать NoFatChain/DataLength каталога по
// одному только номеру кластера НЕЛЬЗЯ (в отличие от FAT32, где для обхода
// каталога достаточно кластера, всё остальное — реальная FAT-цепочка).
// blk_driver.cpp всегда вызывает exfat_resolve_parent(), а ЗАТЕМ
// exfat_find_in_dir()/exfat_format_dir_listing() на его результате — этот
// кэш переносит NoFatChain/DataLength между такими двумя вызовами. Корень и
// текущий CWD (EXFAT_Instance::current_dir_*) разрешаются без кэша, отдельным
// путём. Резервный случай (кэш-промах на чужом кластере) — см.
// resolve_dir_extent(), считается цепочкой (безопасный дефолт).
// ============================================================================
struct DirExtentCache { EXFAT_Instance* fs; uint32_t cluster; bool no_fat_chain; uint64_t byte_length; };
static DirExtentCache g_dir_extent_cache = {nullptr, 0, false, 0};

static void cache_dir_extent(EXFAT_Instance* fs, uint32_t cluster, bool no_fat_chain, uint64_t byte_length) {
    g_dir_extent_cache.fs = fs;
    g_dir_extent_cache.cluster = cluster;
    g_dir_extent_cache.no_fat_chain = no_fat_chain;
    g_dir_extent_cache.byte_length = byte_length;
}

static void resolve_dir_extent(EXFAT_Instance* fs, uint32_t cluster, bool* out_no_chain, uint64_t* out_len) {
    if (cluster == fs->root_cluster) { *out_no_chain = false; *out_len = 0; return; }
    if (cluster == fs->current_dir_cluster) { *out_no_chain = fs->current_dir_no_fat_chain; *out_len = fs->current_dir_byte_length; return; }
    if (g_dir_extent_cache.fs == fs && g_dir_extent_cache.cluster == cluster) {
        *out_no_chain = g_dir_extent_cache.no_fat_chain; *out_len = g_dir_extent_cache.byte_length; return;
    }
    *out_no_chain = false; *out_len = 0; // см. комментарий у DirExtentCache
}

// ============================================================================
// === ОБХОД ЗАПИСЕЙ КАТАЛОГА (0x85 File + 0xC0 Stream Extension + 0xC1... Filename) ===
// ============================================================================
struct ExfatDirEntry {
    bool got_entry;
    bool end_of_dir;
    char name[256];
    int name_len;
    bool is_dir;
    uint32_t first_cluster;
    uint64_t data_length;
    bool no_fat_chain;
    int secondary_count;
    // Курсор, указывающий на ПЕРВУЮ (0x85) запись набора — снимок ДО каких-
    // либо продвижений для этой записи. Нужен Этапу B (delete/rename/
    // перезапись на месте), чтобы физически найти и отредактировать/
    // пометить удалённым весь набор записей файла на диске.
    DirCursor entry_start;
};

// Продвигает cur к следующей записи файла/папки, пропуская служебные
// 0x81 (битмап)/0x82 (upcase table)/0x83 (метка тома) и любой прочий мусор.
// false означает "каталог исчерпан" (реальный конец 0x00 ИЛИ пробег/цепочка
// кластеров кончились) — end_of_dir всегда true в обоих случаях, разница
// (испорченная структура vs честный конец) для Этапа A не принципиальна.
static bool exfat_next_dir_entry(DirCursor* cur, ExfatDirEntry* out) {
    out->got_entry = false;
    out->end_of_dir = false;
    while (true) {
        uint8_t* slot = dir_cursor_current(cur);
        if (!slot) { out->end_of_dir = true; return false; }
        uint8_t entry_type = slot[0];
        if (entry_type == 0x00) { out->end_of_dir = true; return false; }

        if (entry_type != 0x85) {
            if (!dir_cursor_advance(cur)) { out->end_of_dir = true; return false; }
            continue;
        }

        DirCursor entry_start_snapshot = *cur; // cur ещё указывает НА эту 0x85-запись
        uint8_t secondary_count = slot[1];
        uint16_t attrs = (uint16_t)(slot[4] | (slot[5] << 8));

        if (!dir_cursor_advance(cur)) { out->end_of_dir = true; return false; }
        uint8_t* se = dir_cursor_current(cur);
        if (!se || se[0] != 0xC0) { out->end_of_dir = true; return false; } // повреждённый набор записей

        uint8_t name_length = se[3];
        uint32_t first_cluster = (uint32_t)se[20] | ((uint32_t)se[21] << 8) | ((uint32_t)se[22] << 16) | ((uint32_t)se[23] << 24);
        uint64_t data_length = 0;
        for (int b = 0; b < 8; b++) data_length |= ((uint64_t)se[24 + b]) << (8 * b);
        bool no_chain = (se[1] & 0x02) != 0;

        int name_len_out = 0;
        int remaining_name_entries = secondary_count - 1;
        bool ok = true;
        for (int k = 0; k < remaining_name_entries; k++) {
            if (!dir_cursor_advance(cur)) { ok = false; break; }
            uint8_t* ne = dir_cursor_current(cur);
            if (!ne || ne[0] != 0xC1) { ok = false; break; }
            for (int ci = 0; ci < 15 && name_len_out < (int)name_length && name_len_out < 255; ci++) {
                uint16_t code = (uint16_t)(ne[2 + ci * 2] | (ne[2 + ci * 2 + 1] << 8));
                // ASCII-only проект (см. issuse.txt/ROADMAP.md) — код за пределами
                // Latin-1 заменяем на '?' вместо падения; на практике не встречается.
                out->name[name_len_out++] = (code != 0 && code <= 0xFF) ? (char)code : '?';
            }
        }
        out->name[name_len_out] = '\0';
        out->name_len = name_len_out;

        bool advanced = dir_cursor_advance(cur); // не критично, если false — следующий вызов увидит end_of_dir

        if (!ok) { out->end_of_dir = true; return false; }

        (void)advanced;
        out->got_entry = true;
        out->is_dir = (attrs & 0x10) != 0;
        out->first_cluster = first_cluster;
        out->data_length = data_length;
        out->no_fat_chain = no_chain;
        out->secondary_count = secondary_count;
        out->entry_start = entry_start_snapshot;
        return true;
    }
}

struct ExfatSlot {
    bool found;
    bool is_dir;
    uint32_t first_cluster;
    uint64_t data_length;
    bool no_fat_chain;
    int secondary_count;
    DirCursor entry_start; // см. комментарий у ExfatDirEntry::entry_start
};

// Ищет запись с именем target_name (регистронезависимо, ASCII-фолд —
// достаточно, пока все имена в проекте латиница/цифры/точка/подчёркивание,
// см. issuse.txt про upcase table) в каталоге dir_cluster.
static bool exfat_dir_scan(EXFAT_Instance* fs, uint32_t dir_cluster, bool dir_no_chain, uint64_t dir_len,
                            const char* target_name, ExfatSlot* out) {
    out->found = false;
    int target_len = my_strlen(target_name);

    DirCursor cur;
    dir_cursor_init(&cur, fs, dir_cluster, dir_no_chain, dir_len);

    ExfatDirEntry e;
    while (exfat_next_dir_entry(&cur, &e)) {
        if (e.name_len != target_len) continue;
        bool match = true;
        for (int i = 0; i < target_len; i++) {
            if (ascii_upcase(e.name[i]) != ascii_upcase(target_name[i])) { match = false; break; }
        }
        if (match) {
            out->found = true;
            out->is_dir = e.is_dir;
            out->first_cluster = e.first_cluster;
            out->data_length = e.data_length;
            out->no_fat_chain = e.no_fat_chain;
            out->secondary_count = e.secondary_count;
            out->entry_start = e.entry_start;
            return true;
        }
    }
    return false;
}

// ============================================================================
// === НОРМАЛИЗАЦИЯ ПУТИ ===
// exFAT не хранит "."/".." — резолвим их ТЕКСТОМ (стек компонентов) ДО
// обхода диска; сам обход диска после этого идёт только вперёд, как в FAT32.
// ============================================================================
static void exfat_normalize_path(EXFAT_Instance* fs, const char* input_path, char* out_normalized, int out_size) {
    char raw[256];
    int rp = 0;
    if (input_path[0] != '/') {
        int cl = my_strlen(fs->current_dir_path);
        for (int i = 0; i < cl && rp < 254; i++) raw[rp++] = fs->current_dir_path[i];
        if (rp == 0 || raw[rp - 1] != '/') { if (rp < 254) raw[rp++] = '/'; }
    }
    for (int i = 0; input_path[i] && rp < 254; i++) raw[rp++] = input_path[i];
    raw[rp] = '\0';

    char components[16][64];
    int depth = 0;
    const char* p = raw;
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        char comp[64];
        int clen = 0;
        while (*p && *p != '/' && clen < 63) comp[clen++] = *p++;
        comp[clen] = '\0';
        // Обрезаем пробелы В КОНЦЕ компонента (issuse.txt п.1): под FAT32/8.3
        // конечный пробел в имени незаметно проглатывался space-padding'ом
        // короткого имени, под exFAT "foo.txt"/"foo.txt " — разные имена.
        // Уже исправлено в shell.cpp (там обрезается общий arg), но здесь —
        // тот же фикс на уровне ФС, единая точка для ЛЮБОГО клиента
        // blk_driver (не только шелла) и всех операций (create/write/mkdir/
        // delete/rename/cd) разом, раз все они идут через resolve_parent.
        while (clen > 0 && comp[clen - 1] == ' ') comp[--clen] = '\0';
        if (clen == 0) continue;
        if (my_strcmp(comp, ".") == 0) continue;
        if (my_strcmp(comp, "..") == 0) { if (depth > 0) depth--; continue; }
        if (depth < 16) { my_strcpy(components[depth], comp); depth++; }
    }

    int pos = 0;
    out_normalized[pos++] = '/';
    for (int i = 0; i < depth; i++) {
        if (i > 0 && pos < out_size - 1) out_normalized[pos++] = '/';
        for (int j = 0; components[i][j] && pos < out_size - 1; j++) out_normalized[pos++] = components[i][j];
    }
    out_normalized[pos] = '\0';
}

uint32_t exfat_resolve_parent(EXFAT_Instance* fs, const char* full_path, char* out_basename) {
    char normalized[256];
    exfat_normalize_path(fs, full_path, normalized, sizeof(normalized));

    uint32_t current_clus = fs->root_cluster;
    bool current_no_chain = false;
    uint64_t current_len = 0;

    const char* p = normalized;
    while (*p == '/') p++;

    out_basename[0] = '\0';
    char token[64];
    while (*p) {
        int i = 0;
        while (*p && *p != '/' && i < 63) token[i++] = *p++;
        token[i] = '\0';
        while (*p == '/') p++;

        if (*p == '\0') {
            my_strcpy(out_basename, token);
            cache_dir_extent(fs, current_clus, current_no_chain, current_len);
            return current_clus;
        }

        ExfatSlot slot;
        if (!exfat_dir_scan(fs, current_clus, current_no_chain, current_len, token, &slot) || !slot.found || !slot.is_dir) {
            return 0xFFFFFFFF;
        }
        current_clus = slot.first_cluster;
        current_no_chain = slot.no_fat_chain;
        current_len = slot.data_length;
    }

    // normalized был просто "/" — путь есть сам корень
    cache_dir_extent(fs, current_clus, current_no_chain, current_len);
    return current_clus;
}

uint32_t exfat_find_in_dir(EXFAT_Instance* fs, uint32_t dir_cluster, const char* target_name) {
    bool no_chain; uint64_t len;
    resolve_dir_extent(fs, dir_cluster, &no_chain, &len);

    ExfatSlot slot;
    if (!exfat_dir_scan(fs, dir_cluster, no_chain, len, target_name, &slot) || !slot.found) return 0xFFFFFFFF;

    if (slot.is_dir) cache_dir_extent(fs, slot.first_cluster, slot.no_fat_chain, slot.data_length);
    return slot.first_cluster;
}

bool exfat_format_dir_listing(EXFAT_Instance* fs, uint32_t dir_cluster, char* out_buffer, uint32_t max_len) {
    out_buffer[0] = '\0';
    if (max_len < 300) return false;
    if (max_len > EXFAT_MAX_IO_CHUNK) max_len = EXFAT_MAX_IO_CHUNK;

    bool no_chain; uint64_t len;
    resolve_dir_extent(fs, dir_cluster, &no_chain, &len);

    DirCursor cur;
    dir_cursor_init(&cur, fs, dir_cluster, no_chain, len);

    // ВАЖНО (та же причина, что в fat32.cpp): накапливаем в приватном
    // статическом буфере, не в out_buffer напрямую — тот часто указывает на
    // SHM, а read_blocks() использует ТУ ЖЕ страницу как DMA-скретч.
    static char staging[EXFAT_MAX_IO_CHUNK];
    uint32_t offset = 0;
    bool truncated = false;

    ExfatDirEntry e;
    while (exfat_next_dir_entry(&cur, &e)) {
        if (offset > max_len - 300) { truncated = true; break; }

        const char* type_str = e.is_dir ? " [DIR] " : " [FAT] ";
        my_strcpy(staging + offset, type_str); offset += my_strlen(type_str);
        my_strcpy(staging + offset, e.name); offset += my_strlen(e.name);

        if (!e.is_dir) {
            char size_str[20]; int idx = 0;
            uint64_t sz = e.data_length;
            if (sz == 0) { size_str[idx++] = '0'; }
            else {
                char rev[20]; int r = 0;
                while (sz > 0) { rev[r++] = (char)('0' + (sz % 10)); sz /= 10; }
                while (r > 0) size_str[idx++] = rev[--r];
            }
            size_str[idx] = '\0';
            my_strcpy(staging + offset, " \t("); offset += 3;
            my_strcpy(staging + offset, size_str); offset += my_strlen(size_str);
            my_strcpy(staging + offset, " bytes)"); offset += 7;
        }
        my_strcpy(staging + offset, "\n"); offset += 1;
    }

    if (truncated) { my_strcpy(staging + offset, "...\n"); offset += 4; }
    my_memcpy(out_buffer, staging, offset + 1);
    return true;
}

// ============================================================================
// === ЧТЕНИЕ ДАННЫХ ФАЙЛА ===
// ============================================================================
static uint32_t read_extent(EXFAT_Instance* fs, uint32_t first_cluster, bool no_fat_chain, uint32_t offset, char* out_buffer, uint32_t max_len) {
    uint32_t bytes_per_cluster = EXFAT_SECTOR_SIZE << fs->sectors_per_cluster_shift;
    if (bytes_per_cluster == 0 || first_cluster < 2) return 0;
    if (max_len > EXFAT_MAX_IO_CHUNK) max_len = EXFAT_MAX_IO_CHUNK;

    uint32_t cluster_index = offset / bytes_per_cluster;
    uint32_t offset_in_cluster = offset % bytes_per_cluster;

    uint32_t cluster;
    if (no_fat_chain) {
        cluster = first_cluster + cluster_index;
    } else {
        if (fat_chain_has_cycle(fs, first_cluster)) return 0;
        cluster = first_cluster;
        for (uint32_t i = 0; i < cluster_index; i++) {
            uint32_t next = fat_get_entry(fs, cluster);
            if (!exfat_cluster_has_next(next)) return 0;
            cluster = next;
        }
    }

    static char staging[EXFAT_MAX_IO_CHUNK];
    uint32_t copied = 0;
    char sbuf[512];

    while (copied < max_len && cluster >= 2) {
        uint32_t sector_in_cluster = offset_in_cluster / EXFAT_SECTOR_SIZE;
        uint32_t byte_in_sector = offset_in_cluster % EXFAT_SECTOR_SIZE;
        uint32_t sector = cluster_to_sector(fs, cluster) + sector_in_cluster;
        if (!fs->read_blocks(sector, 1, sbuf)) break;

        uint32_t avail = EXFAT_SECTOR_SIZE - byte_in_sector;
        uint32_t need = max_len - copied;
        uint32_t take = need < avail ? need : avail;
        my_memcpy(staging + copied, sbuf + byte_in_sector, take);
        copied += take;

        offset_in_cluster += take;
        if (offset_in_cluster >= bytes_per_cluster) {
            offset_in_cluster = 0;
            if (no_fat_chain) {
                cluster = cluster + 1; // предел общей длины проверяет вызывающий (remaining/chunk)
            } else {
                uint32_t next = fat_get_entry(fs, cluster);
                cluster = exfat_cluster_has_next(next) ? next : 0;
            }
        }
    }

    my_memcpy(out_buffer, staging, copied);
    return copied;
}

bool exfat_read_file(EXFAT_Instance* fs, const char* filename, char* out_buffer, uint32_t offset, uint32_t* bytes_read) {
    *bytes_read = 0;
    char basename[64];
    uint32_t parent_clus = exfat_resolve_parent(fs, filename, basename);
    if (parent_clus == 0xFFFFFFFF || basename[0] == '\0') return false;

    bool parent_no_chain; uint64_t parent_len;
    resolve_dir_extent(fs, parent_clus, &parent_no_chain, &parent_len);

    ExfatSlot slot;
    if (!exfat_dir_scan(fs, parent_clus, parent_no_chain, parent_len, basename, &slot) || !slot.found || slot.is_dir) return false;

    if (offset >= slot.data_length) return true; // EOF
    uint32_t remaining = (uint32_t)(slot.data_length - offset);
    uint32_t chunk = remaining > 4096 ? 4096 : remaining;

    uint32_t copied = read_extent(fs, slot.first_cluster, slot.no_fat_chain, offset, out_buffer, chunk);
    if (copied == 0) return false;
    *bytes_read = copied;
    return true;
}

bool exfat_read_text_file(EXFAT_Instance* fs, const char* path, char* out_buffer) {
    char basename[64];
    uint32_t parent_clus = exfat_resolve_parent(fs, path, basename);
    if (parent_clus == 0xFFFFFFFF || basename[0] == '\0') return false;

    bool parent_no_chain; uint64_t parent_len;
    resolve_dir_extent(fs, parent_clus, &parent_no_chain, &parent_len);

    ExfatSlot slot;
    if (!exfat_dir_scan(fs, parent_clus, parent_no_chain, parent_len, basename, &slot) || !slot.found || slot.is_dir) return false;

    if (slot.data_length == 0 || slot.first_cluster < 2) { out_buffer[0] = '\0'; return true; }
    uint32_t size = (uint32_t)slot.data_length;
    if (size > 4000) size = 4000;

    uint32_t copied = read_extent(fs, slot.first_cluster, slot.no_fat_chain, 0, out_buffer, size);
    out_buffer[copied] = '\0';
    return true;
}

bool exfat_cd(EXFAT_Instance* fs, const char* path) {
    if (my_strcmp(path, "/") == 0) {
        fs->current_dir_cluster = fs->root_cluster;
        fs->current_dir_no_fat_chain = false;
        fs->current_dir_byte_length = 0;
        my_strcpy(fs->current_dir_path, "/");
        return true;
    }

    char normalized[256];
    exfat_normalize_path(fs, path, normalized, sizeof(normalized));

    uint32_t current_clus = fs->root_cluster;
    bool current_no_chain = false;
    uint64_t current_len = 0;

    const char* p = normalized;
    while (*p == '/') p++;
    char token[64];
    while (*p) {
        int i = 0;
        while (*p && *p != '/' && i < 63) token[i++] = *p++;
        token[i] = '\0';
        while (*p == '/') p++;

        ExfatSlot slot;
        if (!exfat_dir_scan(fs, current_clus, current_no_chain, current_len, token, &slot) || !slot.found || !slot.is_dir) return false;
        current_clus = slot.first_cluster;
        current_no_chain = slot.no_fat_chain;
        current_len = slot.data_length;
    }

    fs->current_dir_cluster = current_clus;
    fs->current_dir_no_fat_chain = current_no_chain;
    fs->current_dir_byte_length = current_len;
    my_strcpy(fs->current_dir_path, normalized);
    return true;
}

// Фаза 8 (`df`) — определена ниже (Этап B, рядом с остальным битовым
// аллокатором), нужна уже здесь в exfat_init() для одноразового скана
// при монтировании.
static uint32_t count_free_clusters(EXFAT_Instance* fs);

// ============================================================================
// === МОНТИРОВАНИЕ ===
// ============================================================================
bool exfat_init(EXFAT_Instance* fs, block_read_fn read_func, block_write_fn write_func) {
    fs->read_blocks = read_func;
    fs->write_blocks = write_func;

    char sector_buf[512];
    if (!fs->read_blocks(0, 1, sector_buf)) return false;
    ExfatBootSector* bs = (ExfatBootSector*)sector_buf;

    // Валидация (см. issuse.txt/ROADMAP.md): партиция уже отобрана по типу
    // MBR 0x07 в blk_driver.cpp, но этот байт делится с NTFS — обязательно
    // проверяем реальную сигнатуру exFAT перед тем, как доверять содержимому
    // и тем более писать на этот раздел.
    if (bs->jump_boot[0] != 0xEB) return false;
    if (!mem_eq(bs->fs_name, "EXFAT   ", 8)) return false;
    if (bs->boot_signature != 0xAA55) return false;
    // Жёсткий отказ (не тихая подстановка дефолта, как раньше в FAT32-коде
    // для bytes_per_sector) — весь остальной код файла считает размер
    // сектора буквально 512 байт, а не читает его из тома.
    if (bs->bytes_per_sector_shift != 9) return false;

    fs->fat_offset_sectors = bs->fat_offset;
    fs->fat_length_sectors = bs->fat_length;
    fs->cluster_heap_offset = bs->cluster_heap_offset;
    fs->cluster_count = bs->cluster_count;
    fs->sectors_per_cluster_shift = bs->sectors_per_cluster_shift;
    fs->root_cluster = bs->first_cluster_of_root_directory;

    fs->current_dir_cluster = fs->root_cluster;
    fs->current_dir_no_fat_chain = false;
    fs->current_dir_byte_length = 0;
    my_strcpy(fs->current_dir_path, "/");

    fs->bitmap_cluster = 0;
    fs->bitmap_size_bytes = 0;

    // Находим Allocation Bitmap (0x81) в корне — нужен для Этапа B
    // (аллокатор). Корень всегда честная FAT-цепочка (см. DirCursor).
    //
    // НАЙДЕНО НА ЖИВОМ ЖЕЛЕЗЕ (Milestone 11, повторное монтирование при
    // hot-plug) — этот `while (true)` (и вообще `dir_cursor_advance()` в
    // ветке настоящей FAT-цепочки) НЕ был защищён от цикла, в отличие от
    // free_fat_chain()/read_extent() в этом же файле (см.
    // fat_chain_has_cycle()) — при повторном монтировании (в отличие от
    // самого первого, boot-time) зависание случилось именно здесь.
    // Подозрение: сразу после переподключения устройство/шина ещё не
    // полностью "устаканились", и хотя бы одно чтение FAT вернуло не то,
    // что должно — двойная защита: сначала честная проверка на цикл (как
    // у остальных обходов FAT в этом файле), и жёсткий потолок итераций
    // (тот же приём, что guard++ < 65536 у free_fat_chain) на случай ЛЮБОЙ
    // другой причины, не только цикла — цикл здесь физически невозможен
    // (return false вместо зависания), даже если гипотеза неверна.
    if (fat_chain_has_cycle(fs, fs->root_cluster)) return false;

    DirCursor cur;
    dir_cursor_init(&cur, fs, fs->root_cluster, false, 0);
    int guard = 0;
    while (guard++ < 65536) {
        uint8_t* slot = dir_cursor_current(&cur);
        if (!slot) break;
        if (slot[0] == 0x00) break;
        if (slot[0] == 0x81) {
            fs->bitmap_cluster = (uint32_t)slot[20] | ((uint32_t)slot[21] << 8) | ((uint32_t)slot[22] << 16) | ((uint32_t)slot[23] << 24);
            uint64_t dl = 0;
            for (int b = 0; b < 8; b++) dl |= ((uint64_t)slot[24 + b]) << (8 * b);
            fs->bitmap_size_bytes = (uint32_t)dl;
            break;
        }
        if (!dir_cursor_advance(&cur)) break;
    }

    // Фаза 8 (`df`) — единственный ПОЛНЫЙ проход по битмапу за всю жизнь
    // монтирования, см. free_clusters_hint в h/exfat.h. Только если битмап
    // вообще найден (bitmap_cluster!=0) — иначе (не должно случаться на
    // валидном exFAT) hint остаётся 0, как и cluster_count-независимые поля.
    fs->free_clusters_hint = (fs->bitmap_cluster >= 2) ? count_free_clusters(fs) : 0;

    return true;
}

// ============================================================================
// === ЭТАП B: БИТОВЫЙ АЛЛОКАТОР ===
// Allocation Bitmap (запись 0x81 в корне) — 1 бит на кластер начиная с
// кластера 2, найдена при монтировании (EXFAT_Instance::bitmap_cluster/
// bitmap_size_bytes). У самой этой записи, в отличие от файлов, НЕТ поля
// NoFatChain вообще (см. план/агент-сверку) — как и корневой каталог, битмап
// читается ТОЛЬКО обходом настоящей FAT-цепочки от bitmap_cluster.
// ============================================================================
static bool bitmap_sector_for_byte(EXFAT_Instance* fs, uint32_t byte_offset, uint32_t* out_sector, uint32_t* out_byte_in_sector) {
    uint32_t bytes_per_cluster = EXFAT_SECTOR_SIZE << fs->sectors_per_cluster_shift;
    uint32_t cluster_index = byte_offset / bytes_per_cluster;
    uint32_t offset_in_cluster = byte_offset % bytes_per_cluster;

    uint32_t cluster = fs->bitmap_cluster;
    for (uint32_t i = 0; i < cluster_index; i++) {
        uint32_t next = fat_get_entry(fs, cluster);
        if (!exfat_cluster_has_next(next)) return false;
        cluster = next;
    }
    *out_sector = cluster_to_sector(fs, cluster) + offset_in_cluster / EXFAT_SECTOR_SIZE;
    *out_byte_in_sector = offset_in_cluster % EXFAT_SECTOR_SIZE;
    return true;
}

static bool bitmap_set_bit(EXFAT_Instance* fs, uint32_t cluster, bool value) {
    if (fs->bitmap_cluster < 2 || cluster < 2) return false;
    uint32_t bit_index = cluster - 2;
    uint32_t byte_index = bit_index / 8;
    if (byte_index >= fs->bitmap_size_bytes) return false;

    uint32_t sector, byte_in_sector;
    if (!bitmap_sector_for_byte(fs, byte_index, &sector, &byte_in_sector)) return false;
    char buf[512];
    if (!fs->read_blocks(sector, 1, buf)) return false;
    // Фаза 8 (`df`) — free_clusters_hint поддерживается инкрементально
    // ИМЕННО здесь: это единственная точка, через которую проходят ВСЕ
    // аллокации/освобождения кластеров (bitmap_alloc_run/bitmap_free_run/
    // free_fat_chain). Сверяем с РЕАЛЬНЫМ предыдущим значением бита (не
    // слепо -1/+1 на каждый вызов) — устойчиво к теоретическому повторному
    // set/clear уже установленного бита.
    bool was_set = (buf[byte_in_sector] & (1 << (bit_index % 8))) != 0;
    if (value) buf[byte_in_sector] = (char)(buf[byte_in_sector] | (1 << (bit_index % 8)));
    else buf[byte_in_sector] = (char)(buf[byte_in_sector] & ~(1 << (bit_index % 8)));
    if (!fs->write_blocks(sector, 1, buf)) return false;
    if (value && !was_set) { if (fs->free_clusters_hint > 0) fs->free_clusters_hint--; }
    else if (!value && was_set) { fs->free_clusters_hint++; }
    return true;
}

static void bitmap_free_run(EXFAT_Instance* fs, uint32_t first_cluster, uint32_t num_clusters) {
    for (uint32_t i = 0; i < num_clusters; i++) bitmap_set_bit(fs, first_cluster + i, false);
}

// Ищет и резервирует пробег из num_clusters подряд идущих свободных
// кластеров. Кэширует последний прочитанный сектор битмапа (иначе на каждый
// ИЗ ТЫСЯЧ битов пришлось бы заново читать один и тот же сектор — see план,
// "не пере-оптимизировать раньше времени": сам обход FAT-цепочки битмапа
// внутри bitmap_sector_for_byte остаётся простым, потому что сам битмап на
// практике укладывается в 1-2 кластера — кэш сектора убирает единственную
// реально дорогую часть). Возвращает первый кластер пробега или 0 (диск
// заполнен / пробега такой длины нет).
static uint32_t bitmap_alloc_run(EXFAT_Instance* fs, uint32_t num_clusters) {
    if (num_clusters == 0 || fs->bitmap_cluster < 2) return 0;

    uint32_t run_start = 0;
    uint32_t run_len = 0;
    uint32_t cached_sector = 0xFFFFFFFF;
    char sector_buf[512];

    for (uint32_t c = 2; ; c++) {
        uint32_t bit_index = c - 2;
        uint32_t byte_index = bit_index / 8;
        if (byte_index >= fs->bitmap_size_bytes) break;

        uint32_t sector, byte_in_sector;
        if (!bitmap_sector_for_byte(fs, byte_index, &sector, &byte_in_sector)) break;
        if (sector != cached_sector) {
            if (!fs->read_blocks(sector, 1, sector_buf)) break;
            cached_sector = sector;
        }
        bool occupied = (sector_buf[byte_in_sector] & (1 << (bit_index % 8))) != 0;

        if (!occupied) {
            if (run_len == 0) run_start = c;
            run_len++;
            if (run_len >= num_clusters) {
                for (uint32_t i = 0; i < num_clusters; i++) bitmap_set_bit(fs, run_start + i, true);
                return run_start;
            }
        } else {
            run_len = 0;
        }
    }
    return 0;
}

// Фаза 8 (`df`) — read-only обход, считает occupied-биты по всему
// битмапу. НЕ переиспользует bitmap_sector_for_byte() (та на КАЖДЫЙ вызов
// заново идёт по цепочке FAT от fs->bitmap_cluster — O(k) на вызов, где k —
// номер кластера битмапа; сходит с рук bitmap_alloc_run() выше только
// потому, что тот почти всегда останавливается в первых же байтах —
// свободный кластер на не заполненном диске находится сразу). Здесь нужен
// ПОЛНЫЙ проход до конца битмапа — тот же O(k) на КАЖДЫЙ байт даёт
// O(k^2) суммарно, на живом железе это оказалось настоящим зависанием
// (весь root заблокирован в seL4_Call к blk_driver, пока тот крутится в
// этом цикле). Курсор ниже продвигается по цепочке ИНКРЕМЕНТАЛЬНО, тот же
// приём, что уже использует DirCursor выше — O(1) амортизированно на байт.
//
// Читает батчами по 8 секторов (4КБ), а не по одному — на живом железе
// одиночные USB bulk round-trip'ы подряд на КАЖДЫЙ сектор битмапа
// (для крупной флешки — сотни подряд) валили "USB Transaction Error"/
// таймауты; тот же класс проблемы уже чинили для write_blocks() при записи
// каталогов (см. комментарий у USB_MAX_SECTORS_PER_IO ниже по файлу,
// Milestone 10) — read_blocks() у обоих бэкендов (EMMC/USB) точно так же
// поддерживает count>1 за вызов, просто не был использован здесь.
// Вызывается РОВНО ОДИН РАЗ за жизнь монтирования — из exfat_init(), см.
// forward-декларацию выше и free_clusters_hint в h/exfat.h. Дальше счётчик
// поддерживается инкрементально в bitmap_set_bit(), сюда обход битмапа
// больше не возвращается.
static uint32_t count_free_clusters(EXFAT_Instance* fs) {
    uint32_t sectors_per_cluster = 1u << fs->sectors_per_cluster_shift;
    uint32_t occupied = 0;

    uint32_t cur_cluster = fs->bitmap_cluster;
    uint32_t sector_in_cluster = 0;
    char sector_buf[512 * 8]; // тот же батч, что USB_MAX_SECTORS_PER_IO
    uint32_t sectors_loaded = 0; // сколько СЕКТОРОВ валидно лежит в sector_buf
    uint32_t sector_base_byte = 0; // byte_index, соответствующий началу sector_buf

    for (uint32_t byte_index = 0; byte_index < fs->bitmap_size_bytes; byte_index++) {
        if (byte_index >= sector_base_byte + sectors_loaded * EXFAT_SECTOR_SIZE) {
            sector_in_cluster += sectors_loaded;
            sector_base_byte += sectors_loaded * EXFAT_SECTOR_SIZE;
            while (sector_in_cluster >= sectors_per_cluster) {
                sector_in_cluster -= sectors_per_cluster;
                uint32_t next = fat_get_entry(fs, cur_cluster);
                if (!exfat_cluster_has_next(next)) { sectors_loaded = 0; goto done; } // цепочка кончилась раньше bitmap_size_bytes — не должно случаться, честно останавливаемся
                cur_cluster = next;
            }

            uint32_t remaining_bytes = fs->bitmap_size_bytes - sector_base_byte;
            uint32_t chunk = (remaining_bytes + EXFAT_SECTOR_SIZE - 1) / EXFAT_SECTOR_SIZE;
            if (chunk > 8) chunk = 8;
            if (chunk > sectors_per_cluster - sector_in_cluster) chunk = sectors_per_cluster - sector_in_cluster;
            if (chunk == 0 || !fs->read_blocks(cluster_to_sector(fs, cur_cluster) + sector_in_cluster, chunk, sector_buf)) {
                sectors_loaded = 0;
                goto done;
            }
            sectors_loaded = chunk;
        }
        uint8_t byte_val = (uint8_t)sector_buf[byte_index - sector_base_byte];
        for (int b = 0; b < 8; b++) {
            if (byte_index * 8u + (uint32_t)b >= fs->cluster_count) break; // паддинг-биты за реальным числом кластеров не считаем
            if (byte_val & (1 << b)) occupied++;
        }
    }
done:
    return (fs->cluster_count > occupied) ? (fs->cluster_count - occupied) : 0;
}

// Фаза 8 (`df`) — БЕЗ дискового I/O: total/free берутся из cluster_count
// (известен с монтирования) и free_clusters_hint (см. h/exfat.h — считан
// один раз в exfat_init(), дальше инкрементально поддерживается в
// bitmap_set_bit()). Раньше здесь был полный проход по битмапу НА КАЖДЫЙ
// вызов — см. count_free_clusters() выше и историю в issuse.txt/ROADMAP.md
// про USB-таймауты после нескольких `df` подряд.
bool exfat_free_space(EXFAT_Instance* fs, uint64_t* out_total_bytes, uint64_t* out_free_bytes) {
    if (!fs || fs->bitmap_cluster < 2) return false;
    uint64_t bytes_per_cluster = (uint64_t)EXFAT_SECTOR_SIZE << fs->sectors_per_cluster_shift;
    *out_total_bytes = (uint64_t)fs->cluster_count * bytes_per_cluster;
    *out_free_bytes = (uint64_t)fs->free_clusters_hint * bytes_per_cluster;
    return true;
}

// Освобождает ЧУЖЕРОДНУЮ (NoFatChain=0) цепочку по-старому — обходом FAT,
// снимая и запись FAT, и бит в битмапе для каждого кластера. Собственные
// (NoFatChain=1) экстенты освобождаются напрямую через bitmap_free_run —
// в FAT они не участвуют вообще, трогать там нечего.
static bool free_fat_chain(EXFAT_Instance* fs, uint32_t start_cluster) {
    if (fat_chain_has_cycle(fs, start_cluster)) return false;
    uint32_t cluster = start_cluster;
    int guard = 0;
    while (cluster >= 2 && guard++ < 65536) {
        uint32_t next = fat_get_entry(fs, cluster);
        fat_set_entry(fs, cluster, 0);
        bitmap_set_bit(fs, cluster, false);
        if (!exfat_cluster_has_next(next)) break;
        cluster = next;
    }
    return true;
}

// Освобождает данные существующей записи (файла или каталога) — общий шаг
// для перезаписи/удаления/переименования поверх существующего имени.
static void free_slot_data(EXFAT_Instance* fs, const ExfatSlot& slot) {
    if (slot.first_cluster < 2) return;
    if (slot.no_fat_chain) {
        uint32_t bpc = EXFAT_SECTOR_SIZE << fs->sectors_per_cluster_shift;
        uint32_t n = (uint32_t)((slot.data_length + bpc - 1) / bpc);
        if (n == 0) n = 1; // директории хранят размер СВОЕГО аллоцированного экстента, см. exfat_mkdir
        bitmap_free_run(fs, slot.first_cluster, n);
    } else {
        free_fat_chain(fs, slot.first_cluster);
    }
}

// ============================================================================
// === ЭТАП B: ЗАПИСЬ ДАННЫХ ФАЙЛА ===
// Собственные экстенты всегда смежные (NoFatChain=1, см. bitmap_alloc_run) —
// в отличие от read_extent/FAT32 здесь не нужно ходить по цепочке вообще,
// сектора просто идут подряд.
// ============================================================================
// Батчинг по 8 секторов (4КБ) за вызов write_blocks() — тот же фикс и по
// той же причине, что в exfat_mkdir() (см. комментарий там, Milestone 10,
// найдено на живом железе с USB) — раньше по сектору за раз, что на
// файле в несколько КБ означало соответствующее число отдельных
// SCSI-транзакций подряд. Полные сектора пишутся ПРЯМО из буфера
// вызывающего (без копирования — `data`/`len` у обоих бэкендов всегда
// это whole-buffer staging-страница, см. exfat_write_file), паддинг
// нулями нужен только для ПОСЛЕДНЕГО неполного сектора.
static bool write_extent_data(EXFAT_Instance* fs, uint32_t first_cluster, const char* data, uint32_t len) {
    uint32_t sector = cluster_to_sector(fs, first_cluster);
    uint32_t remaining = len;
    const char* p = data;
    static char pad_buf[512 * 8];
    while (remaining > 0) {
        uint32_t full_sectors = remaining / EXFAT_SECTOR_SIZE;
        if (full_sectors > 0) {
            uint32_t chunk_sectors = full_sectors > 8 ? 8 : full_sectors;
            if (!fs->write_blocks(sector, chunk_sectors, p)) return false;
            uint32_t chunk_bytes = chunk_sectors * EXFAT_SECTOR_SIZE;
            p += chunk_bytes;
            remaining -= chunk_bytes;
            sector += chunk_sectors;
            continue;
        }
        for (uint32_t i = 0; i < remaining; i++) pad_buf[i] = p[i];
        for (uint32_t i = remaining; i < EXFAT_SECTOR_SIZE; i++) pad_buf[i] = 0;
        if (!fs->write_blocks(sector, 1, pad_buf)) return false;
        remaining = 0;
    }
    return true;
}

// ============================================================================
// === ЭТАП B: NameHash/SetChecksum + ЗАПИСЬ НАБОРА DIRECTORY ENTRIES ===
// ============================================================================
static uint16_t exfat_name_hash(const char* ascii_name, int len) {
    uint16_t hash = 0;
    for (int i = 0; i < len; i++) {
        uint8_t ch = (uint8_t)ascii_upcase(ascii_name[i]);
        // UTF-16LE code unit = {ch, 0} — младший байт, потом старший (всегда 0 для ASCII).
        hash = (uint16_t)(((hash << 15) | (hash >> 1)) + ch);
        hash = (uint16_t)(((hash << 15) | (hash >> 1)) + 0);
    }
    return hash;
}

// Собирает набор записей (0x85 + 0xC0 + 0xC1...) в памяти, считает
// SetChecksum и пишет их подряд начиная С УЖЕ НАЙДЕННОЙ позиции
// start_cursor (копия — сама функция её продвигает, оригинал вызывающего не
// трогает). Не ищет место сама — см. exfat_write_entry_set (поиск) и вызовы
// "на месте" (перезапись существующей записи без изменения имени).
static bool exfat_write_entry_set_at(DirCursor start_cursor, const char* name, uint16_t attrs,
                                      uint32_t first_cluster, uint64_t data_length, bool no_fat_chain) {
    int name_len = my_strlen(name);
    if (name_len == 0 || name_len > 255) return false;
    int name_entries = (name_len + 14) / 15;
    int secondary_count = 1 + name_entries;
    int total_entries = 1 + secondary_count;
    if (total_entries > 19) return false; // 255 символов имени — практический потолок этого проекта (buffers[64] у вызывающих)

    uint8_t entries[19 * 32];
    for (int i = 0; i < total_entries * 32; i++) entries[i] = 0;

    // 0x85 File Directory Entry
    entries[0] = 0x85;
    entries[1] = (uint8_t)secondary_count;
    entries[4] = (uint8_t)(attrs & 0xFF);
    entries[5] = (uint8_t)(attrs >> 8);
    // Реальное время не отслеживается (нет RTC на плате, см. ROADMAP.md про
    // NTP) — таймстампы оставляем нулевыми, UtcOffset-байты (смещения 22/23/
    // 24) = 0x80 ("нет информации о часовом поясе", бит7 — спека это прямо
    // разрешает вместо выдумывания несуществующего offset'а).
    entries[22] = 0x80; entries[23] = 0x80; entries[24] = 0x80;

    // 0xC0 Stream Extension Entry
    uint8_t* se = entries + 32;
    se[0] = 0xC0;
    se[1] = (uint8_t)(0x01 | (no_fat_chain ? 0x02 : 0x00)); // AllocationPossible=1 всегда + NoFatChain
    se[3] = (uint8_t)name_len;
    uint16_t hash = exfat_name_hash(name, name_len);
    se[4] = (uint8_t)(hash & 0xFF);
    se[5] = (uint8_t)(hash >> 8);
    for (int b = 0; b < 8; b++) se[8 + b] = (uint8_t)((data_length >> (8 * b)) & 0xFF);  // ValidDataLength
    se[20] = (uint8_t)(first_cluster & 0xFF);
    se[21] = (uint8_t)((first_cluster >> 8) & 0xFF);
    se[22] = (uint8_t)((first_cluster >> 16) & 0xFF);
    se[23] = (uint8_t)((first_cluster >> 24) & 0xFF);
    for (int b = 0; b < 8; b++) se[24 + b] = (uint8_t)((data_length >> (8 * b)) & 0xFF); // DataLength

    // 0xC1 File Name Entries
    for (int e = 0; e < name_entries; e++) {
        uint8_t* ne = entries + 32 * (2 + e);
        ne[0] = 0xC1;
        for (int ci = 0; ci < 15; ci++) {
            int name_idx = e * 15 + ci;
            uint16_t code = (name_idx < name_len) ? (uint8_t)name[name_idx] : 0x0000;
            ne[2 + ci * 2] = (uint8_t)(code & 0xFF);
            ne[2 + ci * 2 + 1] = (uint8_t)(code >> 8);
        }
    }

    // SetChecksum — по всему набору, пропуская байты 2-3 ПЕРВОЙ записи (это и есть само поле).
    uint16_t checksum = 0;
    int total_bytes = total_entries * 32;
    for (int i = 0; i < total_bytes; i++) {
        if (i == 2 || i == 3) continue;
        checksum = (uint16_t)(((checksum << 15) | (checksum >> 1)) + entries[i]);
    }
    entries[2] = (uint8_t)(checksum & 0xFF);
    entries[3] = (uint8_t)(checksum >> 8);

    DirCursor cur = start_cursor;
    for (int e = 0; e < total_entries; e++) {
        if (!dir_cursor_write_current(&cur, entries + 32 * e)) return false;
        if (e < total_entries - 1) { if (!dir_cursor_advance(&cur)) return false; }
    }
    return true;
}

// Ищет slots_needed подряд идущих свободных 32-байтных слотов, начиная с
// ПЕРВОГО настоящего конца каталога (0x00) — спецификация гарантирует, что
// всё после него свободно. Дырки от УДАЛЁННЫХ (не терминальных) записей до
// конца НЕ переиспользуются в этой реализации (см. план/ограничения Этапа
// B) — единственное исключение сделано отдельно в exfat_write_file для
// перезаписи БЕЗ смены имени (see exfat_write_entry_set_at, вызывается
// напрямую на месте старой записи). Каталог должен быть уже создан НАШИМ
// кодом как один смежный (NoFatChain=1) кластер — рост каталога за пределы
// одного кластера не реализован.
static bool find_free_slot_run(EXFAT_Instance* fs, uint32_t dir_cluster, bool dir_no_chain, uint64_t dir_len,
                                int slots_needed, DirCursor* out_cur) {
    dir_cursor_init(out_cur, fs, dir_cluster, dir_no_chain, dir_len);
    while (true) {
        uint8_t* slot = dir_cursor_current(out_cur);
        if (!slot) return false;
        if (slot[0] == 0x00) {
            DirCursor probe = *out_cur;
            bool fits = true;
            for (int i = 1; i < slots_needed; i++) {
                if (!dir_cursor_advance(&probe) || !dir_cursor_current(&probe)) { fits = false; break; }
            }
            return fits;
        }
        if (!dir_cursor_advance(out_cur)) return false;
    }
}

static bool exfat_write_entry_set(EXFAT_Instance* fs, uint32_t dir_cluster, bool dir_no_chain, uint64_t dir_len,
                                   const char* name, uint16_t attrs, uint32_t first_cluster, uint64_t data_length, bool no_fat_chain) {
    int name_len = my_strlen(name);
    if (name_len == 0) return false;
    int name_entries = (name_len + 14) / 15;
    int total_entries = 2 + name_entries;

    DirCursor cur;
    if (!find_free_slot_run(fs, dir_cluster, dir_no_chain, dir_len, total_entries, &cur)) return false;
    return exfat_write_entry_set_at(cur, name, attrs, first_cluster, data_length, no_fat_chain);
}

// Сбрасывает бит7 (InUse) у КАЖДОЙ записи набора — спецификация сanкционирует
// именно так помечать записи удалёнными (0x85->0x05, 0xC0->0x40, 0xC1->0x41).
static bool exfat_mark_entry_deleted(DirCursor entry_cursor, int secondary_count) {
    DirCursor cur = entry_cursor;
    int total = 1 + secondary_count;
    for (int i = 0; i < total; i++) {
        uint8_t* slot = dir_cursor_current(&cur);
        if (!slot) return false;
        uint8_t entry[32];
        my_memcpy(entry, slot, 32);
        entry[0] = (uint8_t)(entry[0] & 0x7F);
        if (!dir_cursor_write_current(&cur, entry)) return false;
        if (i < total - 1) { if (!dir_cursor_advance(&cur)) return false; }
    }
    return true;
}

// ============================================================================
// === ЭТАП B: ПУБЛИЧНЫЙ API ЗАПИСИ ===
// ============================================================================
bool exfat_create_file(EXFAT_Instance* fs, const char* path, bool* out_existed) {
    if (out_existed) *out_existed = false;
    char basename[64];
    uint32_t parent_clus = exfat_resolve_parent(fs, path, basename);
    if (parent_clus == 0xFFFFFFFF || basename[0] == '\0') return false;

    bool parent_no_chain; uint64_t parent_len;
    resolve_dir_extent(fs, parent_clus, &parent_no_chain, &parent_len);

    ExfatSlot existing;
    if (exfat_dir_scan(fs, parent_clus, parent_no_chain, parent_len, basename, &existing) && existing.found) {
        if (out_existed) *out_existed = true;
        return true; // уже существует
    }

    return exfat_write_entry_set(fs, parent_clus, parent_no_chain, parent_len, basename, 0x20 /* ARCHIVE */, 0, 0, true);
}

bool exfat_write_file(EXFAT_Instance* fs, const char* path, const char* text, uint32_t len) {
    char basename[64];
    uint32_t parent_clus = exfat_resolve_parent(fs, path, basename);
    if (parent_clus == 0xFFFFFFFF || basename[0] == '\0') return false;

    bool parent_no_chain; uint64_t parent_len;
    resolve_dir_extent(fs, parent_clus, &parent_no_chain, &parent_len);

    ExfatSlot slot;
    bool exists = exfat_dir_scan(fs, parent_clus, parent_no_chain, parent_len, basename, &slot) && slot.found;
    if (exists && slot.is_dir) return false; // нельзя echo> в каталог

    uint32_t bpc = EXFAT_SECTOR_SIZE << fs->sectors_per_cluster_shift;
    uint32_t new_clus = 0;
    if (len > 0) {
        uint32_t need_clusters = (len + bpc - 1) / bpc;
        new_clus = bitmap_alloc_run(fs, need_clusters);
        if (new_clus == 0) return false;
        if (!write_extent_data(fs, new_clus, text, len)) {
            bitmap_free_run(fs, new_clus, need_clusters);
            return false;
        }
    }

    bool written;
    if (exists) {
        // Имя не меняется — перезаписываем ТОТ ЖЕ физический набор записей на
        // месте (не ищем новое, иначе каждый повторный echo> в один и тот же
        // файл плодил бы дыры в каталоге — see find_free_slot_run).
        written = exfat_write_entry_set_at(slot.entry_start, basename, 0x20, new_clus, len, true);
    } else {
        written = exfat_write_entry_set(fs, parent_clus, parent_no_chain, parent_len, basename, 0x20, new_clus, len, true);
    }

    if (!written) {
        if (new_clus != 0) bitmap_free_run(fs, new_clus, (len + bpc - 1) / bpc);
        return false;
    }

    if (exists) free_slot_data(fs, slot); // старые данные — только ПОСЛЕ успешной перезаписи записи
    return true;
}

bool exfat_mkdir(EXFAT_Instance* fs, const char* path, bool* out_existed) {
    if (out_existed) *out_existed = false;
    char basename[64];
    uint32_t parent_clus = exfat_resolve_parent(fs, path, basename);
    if (parent_clus == 0xFFFFFFFF || basename[0] == '\0') return false;

    bool parent_no_chain; uint64_t parent_len;
    resolve_dir_extent(fs, parent_clus, &parent_no_chain, &parent_len);

    ExfatSlot existing;
    if (exfat_dir_scan(fs, parent_clus, parent_no_chain, parent_len, basename, &existing) && existing.found) {
        if (out_existed) *out_existed = true;
        return false; // уже существует
    }

    uint32_t new_clus = bitmap_alloc_run(fs, 1);
    if (new_clus == 0) return false;

    // Новый кластер обнуляем целиком — валидный пустой каталог (0x00 =
    // конец) без единой реальной записи. В отличие от FAT32, "."/".." не нужны.
    //
    // НАЙДЕНО НА ЖИВОМ ЖЕЛЕЗЕ (Milestone 10, USB) — тот же класс задержки,
    // что уже чинили для hardware_emmc_read/write (см. blk_driver.cpp,
    // "раньше здесь был цикл из отдельных CMD17/24 на КАЖДЫЙ сектор"):
    // по СЕКТОРУ за раз на кластер в 128КБ (типичный для macOS-
    // отформатированных exFAT-накопителей от 32ГБ, см. ROADMAP.md) — это
    // 256 отдельных SCSI WRITE(10) round-trip'ов через USB bulk-эндпоинты
    // подряд, и `mkdir` выглядел зависшим (не был — просто очень медленный).
    // write_blocks() у ОБОИХ бэкендов (hardware_emmc_write/hardware_usb_write)
    // и так уже поддерживает count>1 за вызов (see USB_MAX_SECTORS_PER_IO=8
    // в usb_driver.cpp — тот же лимит, что здесь) — просто не
    // использовался экфат-уровнем. batch по 8 секторов (4КБ) — на порядок
    // меньше round-trip'ов.
    uint32_t sectors_per_cluster = 1u << fs->sectors_per_cluster_shift;
    static char zero_buf[512 * 8] = {0};
    uint32_t sec = cluster_to_sector(fs, new_clus);
    uint32_t remaining = sectors_per_cluster;
    while (remaining > 0) {
        uint32_t chunk = remaining > 8 ? 8 : remaining;
        if (!fs->write_blocks(sec, chunk, zero_buf)) { bitmap_free_run(fs, new_clus, 1); return false; }
        sec += chunk;
        remaining -= chunk;
    }

    // DataLength = размер РЕАЛЬНО аллоцированного экстента (1 кластер), а не
    // 0 — иначе DirCursor решит, что у каталога 0 валидных кластеров
    // (ceil(0/bpc)=0), хотя кластер реально выделен и обнулён.
    uint32_t bpc = EXFAT_SECTOR_SIZE << fs->sectors_per_cluster_shift;
    if (!exfat_write_entry_set(fs, parent_clus, parent_no_chain, parent_len, basename, 0x10 /* DIRECTORY */, new_clus, bpc, true)) {
        bitmap_free_run(fs, new_clus, 1);
        return false;
    }
    return true;
}

bool exfat_delete_file(EXFAT_Instance* fs, const char* path) {
    char basename[64];
    uint32_t parent_clus = exfat_resolve_parent(fs, path, basename);
    if (parent_clus == 0xFFFFFFFF || basename[0] == '\0') return false;

    bool parent_no_chain; uint64_t parent_len;
    resolve_dir_extent(fs, parent_clus, &parent_no_chain, &parent_len);

    ExfatSlot slot;
    if (!exfat_dir_scan(fs, parent_clus, parent_no_chain, parent_len, basename, &slot) || !slot.found) return false;

    free_slot_data(fs, slot);
    return exfat_mark_entry_deleted(slot.entry_start, slot.secondary_count);
}

bool exfat_rename_file(EXFAT_Instance* fs, const char* old_path, const char* new_path) {
    char old_basename[64];
    uint32_t old_parent = exfat_resolve_parent(fs, old_path, old_basename);
    if (old_parent == 0xFFFFFFFF || old_basename[0] == '\0') return false;

    bool old_parent_no_chain; uint64_t old_parent_len;
    resolve_dir_extent(fs, old_parent, &old_parent_no_chain, &old_parent_len);

    ExfatSlot slot;
    if (!exfat_dir_scan(fs, old_parent, old_parent_no_chain, old_parent_len, old_basename, &slot) || !slot.found) return false;

    char new_basename[64];
    uint32_t new_parent = exfat_resolve_parent(fs, new_path, new_basename);
    if (new_parent == 0xFFFFFFFF || new_basename[0] == '\0') return false;

    bool new_parent_no_chain; uint64_t new_parent_len;
    resolve_dir_extent(fs, new_parent, &new_parent_no_chain, &new_parent_len);

    // exFAT — одно имя на файл, короткого алиаса нет вообще: переименование
    // всегда единообразно "удалить старый набор записей, записать новый с
    // тем же FirstCluster/DataLength/атрибутами" — проще, чем было в FAT32
    // (там отдельно разбиралось, влезает ли новое имя в 8.3 без LFN).
    ExfatSlot existing;
    if (exfat_dir_scan(fs, new_parent, new_parent_no_chain, new_parent_len, new_basename, &existing) && existing.found) return false;

    uint16_t attrs = slot.is_dir ? 0x10 : 0x20;
    if (!exfat_write_entry_set(fs, new_parent, new_parent_no_chain, new_parent_len, new_basename, attrs, slot.first_cluster, slot.data_length, slot.no_fat_chain)) {
        return false;
    }
    return exfat_mark_entry_deleted(slot.entry_start, slot.secondary_count);
}
