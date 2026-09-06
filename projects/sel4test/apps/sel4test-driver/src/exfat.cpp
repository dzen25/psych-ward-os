#include <sel4/sel4.h>
#include "h/exfat.h"

uint32_t g_exfat_io_tag = EXFAT_IO_OTHER;
// Прямые счётчики входа — тег по функции может быть неточен (разметчик
// держит имя до следующей распознанной функции), а это считает факт.
uint32_t g_exfat_copy_extent_calls = 0;
uint32_t g_exfat_append_slow_calls = 0;
uint32_t g_exfat_append_calls = 0;
uint32_t g_exfat_stream_write_calls = 0;
// Тег ставится ПРЯМО ПЕРЕД блочной операцией, а не на входе в функцию:
// RAII с деструктором тянет за собой C++-раскрутку стека, а драйверы
// собираются без неё (__gxx_personality_v0 при линковке).

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

// Сколько секторов влезает в ЛОКАЛЬНЫЕ (стековые/статические) буферы этого
// файла. Это НЕ то же самое, что fs->max_sectors_per_io: тот говорит,
// сколько драйвер способен перенести за один вызов, а этот — сколько мы
// physически можем принять. Смешивать нельзя.
//
// hw 2026-09-06: после подъёма max_sectors_per_io до 128 (bounce-буфер USB
// вырос до 64 КБ) count_free_clusters() попросила 128 секторов в
// sector_buf[512*8] НА СТЕКЕ — 64 КБ в 4 КБ. usb_driver упал с обращением
// по 0x502000, за верхушкой собственного стека (0x501000). Буферы, которые
// раньше молча совпадали с прежним лимитом 8, теперь ограничены явно.
constexpr uint32_t EXFAT_LOCAL_BUF_SECTORS = 8;

// min(что может драйвер, что влезает к нам) — для мест с локальным буфером.
static inline uint32_t exfat_local_chunk(EXFAT_Instance* fs, uint32_t want) {
    uint32_t cap = fs->max_sectors_per_io < EXFAT_LOCAL_BUF_SECTORS
                 ? fs->max_sectors_per_io : EXFAT_LOCAL_BUF_SECTORS;
    return want > cap ? cap : want;
}
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
    // issuse.txt №39: clus за пределами реальной кучи (испорченное
    // FirstCluster/запись FAT с диска) раньше уходило прямо в
    // (clus-2)*sectors_per_cluster без проверки — на переполнении 32-бит
    // это заворачивается в произвольный маленький сектор, который потом
    // используется как есть в hardware_emmc_read/write. Тот же fallback,
    // что уже применяется ниже для clus<2 — на root_cluster.
    if (clus < 2 || clus - 2 >= fs->cluster_count) clus = (fs->root_cluster >= 2) ? fs->root_cluster : 2;
    return fs->cluster_heap_offset + (clus - 2) * (1u << fs->sectors_per_cluster_shift);
}

// В отличие от FAT32 (значения-ПОРОГИ), у exFAT метки цепочки — ТОЧНЫЕ
// значения: 0xFFFFFFFF = конец цепочки, 0xFFFFFFF7 = битый кластер. Записи
// FAT НЕ маскируются (32 полных бита, не 28, как в FAT32).
static bool exfat_cluster_has_next(uint32_t v) { return v >= 2 && v != 0xFFFFFFFF && v != 0xFFFFFFF7; }

static uint32_t fat_get_entry(EXFAT_Instance* fs, uint32_t cluster) {
    // issuse.txt №39: cluster с диска (не своё вычисленное значение) может
    // быть испорчено — без проверки byte_offset = cluster*4 может уйти
    // сколь угодно далеко за реальную FAT-область. 0xFFFFFFFF — тот же
    // сентинел, что уже возвращается на сбое чтения ниже, безопасно
    // трактуется exfat_cluster_has_next() как "нет продолжения".
    if (cluster < 2 || cluster - 2 >= fs->cluster_count) return 0xFFFFFFFF;
    uint32_t byte_offset = cluster * 4;
    uint32_t fat_sector = fs->fat_offset_sectors + (byte_offset / EXFAT_SECTOR_SIZE);
    uint32_t sector_offset = byte_offset % EXFAT_SECTOR_SIZE;
    char buf[512];
    g_exfat_io_tag = EXFAT_IO_FAT;
    if (!fs->read_blocks(fat_sector, 1, buf)) return 0xFFFFFFFF;
    uint32_t* entries = (uint32_t*)buf;
    return entries[sector_offset / 4];
}

// Этап B: нужна только для освобождения ЧУЖЕРОДНЫХ фрагментированных файлов/
// папок (NoFatChain=0, например созданных Finder'ом ДО первой загрузки) —
// свои собственные экстенты (NoFatChain=1) в FAT не участвуют вообще, см.
// bitmap_alloc_run ниже.
static bool fat_set_entry(EXFAT_Instance* fs, uint32_t cluster, uint32_t value) {
    if (cluster < 2 || cluster - 2 >= fs->cluster_count) return false; // issuse.txt №39
    uint32_t byte_offset = cluster * 4;
    uint32_t fat_sector = fs->fat_offset_sectors + (byte_offset / EXFAT_SECTOR_SIZE);
    uint32_t sector_offset = byte_offset % EXFAT_SECTOR_SIZE;
    char buf[512];
    g_exfat_io_tag = EXFAT_IO_FAT;
    if (!fs->read_blocks(fat_sector, 1, buf)) return false;
    uint32_t* entries = (uint32_t*)buf;
    entries[sector_offset / 4] = value;
    g_exfat_io_tag = EXFAT_IO_FAT;
    return fs->write_blocks(fat_sector, 1, buf);
}

// Алгоритм Флойда ("черепаха и заяц") — тот же приём, что в fat32.cpp,
// применяется только к РЕАЛЬНЫМ цепочкам (root, либо чужеродные
// фрагментированные файлы/папки с NoFatChain=0) — собственные экстенты
// (NoFatChain=1) в FAT не ходят вообще, там циклов быть не может.
static bool fat_chain_has_cycle(EXFAT_Instance* fs, uint32_t start_cluster) {
    if (start_cluster < 2) return false;
    uint32_t slow = start_cluster, fast = start_cluster;
    // issuse.txt №15 — НАЙДЕНО НА ЖИВОМ ЖЕЛЕЗЕ (полное зависание системы,
    // без срабатывания watchdog'а, детерминированно на 5-й подряд
    // перемонтировании флешки за хабом): алгоритм Флойда математически
    // гарантированно завершается для ЛЮБОЙ конечной цепочки (либо цикл
    // найден, либо кто-то из курсоров упирается в конец), НО не даёт
    // НИКАКОЙ практической верхней границы по РЕАЛЬНОМУ времени/числу
    // аппаратных чтений — каждое успешное чтение сигналит usb-watchdog'у
    // "жив" (см. g_usb_liveness_ntfn в hardware_usb_rw_generic_read()),
    // поэтому по-настоящему длинный или испорченный проход не ловится
    // watchdog'ом вообще, просто крутится. Соседний цикл ниже (обход
    // корня после этой проверки) уже защищён guard++ < 65536 — здесь
    // такого потолка не было ни разу, хотя комментарий на месте вызова
    // прямо предполагал двойную защиту. Добавлен тот же приём.
    //
    // Второй, более точный фактор той же живой находки (второе мнение,
    // независимо проверено против исходников seL4 — handleYield/schedule):
    // на non-MCS seL4 seL4_Yield() честно ставит вызывающего в конец
    // очереди готовности своего приоритета — НО примитивы ожидания завершения
    // передачи (wait_transfer_completion() и т.п., см. usb_driver.cpp)
    // зовут seL4_Yield() ТОЛЬКО если совпадение не нашлось с первой
    // попытки — быстрый (обычный!) путь при живом устройстве возвращается
    // сразу, ни разу не отдав квант. Длинная цепочка из МНОГИХ подряд
    // быстрых чтений (типичный случай, не редкий) может не отдавать CPU
    // вообще ни разу за весь проход — другой процесс той же приоритетности
    // на том же ядре (blk_driver, ждущий свой 20мс liveness-тик) реально
    // может не получить квант секундами, это НЕ придумка, а прямое
    // следствие round-robin без принудительного yield в хот-пути. Explicit
    // yield здесь — тот же дешёвый приём, что уже применяется в
    // wait_transfer_completion() на медленном пути, просто безусловно.
    int guard = 0;
    while (guard++ < 65536) {
        uint32_t f1 = fat_get_entry(fs, fast);
        if (!exfat_cluster_has_next(f1)) return false;
        uint32_t f2 = fat_get_entry(fs, f1);
        if (!exfat_cluster_has_next(f2)) return false;
        slow = fat_get_entry(fs, slow);
        fast = f2;
        if (slow == fast) return true;
        seL4_Yield();
    }
    return true; // потолок исчерпан — не доверяем цепочке, ведём себя как при обнаруженном цикле
}

// ============================================================================
// === КУРСОР ПО 32-БАЙТНЫМ СЛОТАМ КАТАЛОГА (пересекает границы секторов и
// кластеров, работает одинаково для NoFatChain=1 (свои каталоги, подряд
// идущие кластеры) и NoFatChain=0 (root; чужеродные фрагментированные) ===
// ============================================================================

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
    c->fat_chain_steps = 0;
}

// Указатель на текущий 32-байтный слот (внутри c->sector_buf) или nullptr,
// если каталог исчерпан. Курсор не продвигает.
static uint8_t* dir_cursor_current(DirCursor* c) {
    if (c->cur_cluster < 2) return nullptr;
    if (c->no_fat_chain && c->clusters_visited >= c->max_clusters) return nullptr;
    if (!c->sector_loaded) {
        uint32_t sector = cluster_to_sector(c->fs, c->cur_cluster) + c->sector_in_cluster;
        g_exfat_io_tag = EXFAT_IO_DIR_CURSOR;
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
    // issuse.txt №38: цепочка корня проверяется на цикл ОДИН раз при
    // монтировании (fat_chain_has_cycle), но любой другой NoFatChain=0
    // каталог (фрагментированный, например созданный чужой ОС) никогда не
    // проверялся — а полноценный fat_chain_has_cycle() на КАЖДЫЙ advance()
    // был бы O(n²) на весь скан каталога. Дешёвая альтернатива — просто
    // ограничить общее число шагов по цепочке (тот же приём, что уже есть
    // в free_fat_chain/fat_chain_has_cycle): зацикленная/чрезмерно длинная
    // цепочка обрывает скан как честный конец каталога вместо зависания
    // blk_driver навсегда.
    if (++c->fat_chain_steps > 65536) { c->cur_cluster = 0; return false; }
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
    g_exfat_io_tag = EXFAT_IO_DIR_CURSOR;
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
    // issuse.txt №41: true, если хотя бы один код UTF-16 имени был вне
    // Latin-1 (>0xFF) и заменён на '?' при декодировании — см.
    // exfat_next_dir_entry(). Такое имя НЕЛЬЗЯ использовать для поиска
    // совпадения (exfat_dir_scan) — разные исходные Unicode-имена могут
    // схлопнуться в одинаковую '?'-строку, что раньше приводило к тому,
    // что cat/rm/mv по такому "коллизионному" имени могли попасть не в
    // тот файл. ls всё равно показывает такие имена (as-is, с '?').
    bool has_lossy_chars;
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
    out->has_lossy_chars = false;
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
        // issuse.txt №40: раньше один испорченный набор записей (0x85 без
        // ожидаемого 0xC0 следом) трактовался как конец ВСЕГО каталога —
        // все валидные файлы дальше в сыром потоке байт становились
        // невидимыми. Теперь просто пропускаем этот единственный слот и
        // продолжаем поиск следующей 0x85-записи (внешний while(true) сам
        // досканирует вперёд байт за байтом — тот же путь, что уже
        // обрабатывает entry_type != 0x85 ниже).
        if (!se) { out->end_of_dir = true; return false; } // курсор реально кончился — это честный конец
        if (se[0] != 0xC0) continue; // повреждённый набор записей — пропускаем, не конец каталога

        uint8_t name_length = se[3];
        uint32_t first_cluster = (uint32_t)se[20] | ((uint32_t)se[21] << 8) | ((uint32_t)se[22] << 16) | ((uint32_t)se[23] << 24);
        uint64_t data_length = 0;
        for (int b = 0; b < 8; b++) data_length |= ((uint64_t)se[24 + b]) << (8 * b);
        bool no_chain = (se[1] & 0x02) != 0;

        int name_len_out = 0;
        int remaining_name_entries = secondary_count - 1;
        // issuse.txt №40: различаем "курсор реально кончился" (exhausted —
        // честный конец каталога) от "запись continuation битая/не 0xC1"
        // (corrupted — пропускаем только этот набор, каталог не кончился).
        bool exhausted = false;
        bool corrupted = false;
        for (int k = 0; k < remaining_name_entries; k++) {
            if (!dir_cursor_advance(cur)) { exhausted = true; break; }
            uint8_t* ne = dir_cursor_current(cur);
            if (!ne) { exhausted = true; break; }
            if (ne[0] != 0xC1) { corrupted = true; break; }
            for (int ci = 0; ci < 15 && name_len_out < (int)name_length && name_len_out < 255; ci++) {
                uint16_t code = (uint16_t)(ne[2 + ci * 2] | (ne[2 + ci * 2 + 1] << 8));
                // ASCII-only проект (см. issuse.txt/ROADMAP.md) — код за пределами
                // Latin-1 заменяем на '?' вместо падения; на практике не встречается.
                if (code != 0 && code <= 0xFF) {
                    out->name[name_len_out++] = (char)code;
                } else {
                    out->name[name_len_out++] = '?';
                    out->has_lossy_chars = true; // issuse.txt №41 — это имя нельзя использовать для сопоставления
                }
            }
        }
        out->name[name_len_out] = '\0';
        out->name_len = name_len_out;

        if (exhausted) { out->end_of_dir = true; return false; }
        if (corrupted) continue; // issuse.txt №40 — пропускаем битый набор записей, не конец каталога

        bool advanced = dir_cursor_advance(cur); // не критично, если false — следующий вызов увидит end_of_dir

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

// --- Кэш расположения последнего файла ---
//
// Зачем: exfat_append_file() на КАЖДЫЙ вызов заново разбирала путь
// (exfat_resolve_parent — скан каталога на каждый компонент) и сканировала
// родительский каталог в поисках самой записи. Для журнала, куда пишут
// тысячи раз подряд по одному и тому же пути, это чистая потеря: файл
// никуда не переезжает между двумя дописываниями. На железе замер показал
// ~7 мс на дописывание при ~7 блочных операциях, из которых полезную
// нагрузку несла ОДНА — остальные были поиском.
//
// Кэш на одну запись: больше не нужно, характерная нагрузка — поток в один
// файл. Инвалидация — из всех функций, меняющих структуру ФС (создание,
// удаление, переименование, mkdir, полная перезапись), плюс при монтировании.
// Правило простое: если операция может сдвинуть или уничтожить запись
// каталога, кэш сбрасывается. Дописывание запись НЕ двигает (пишет на
// месте, см. exfat_write_entry_set_at), поэтому само себя не инвалидирует —
// только обновляет длину и первый кластер.
struct FileLocCache {
    EXFAT_Instance* fs;
    char path[256];
    uint32_t parent_clus;
    bool parent_no_chain;
    uint64_t parent_len;
    char basename[256];
    ExfatSlot slot;
    bool valid;
};
static FileLocCache g_file_loc = {nullptr, {0}, 0, false, 0, {0}, {}, false};

// Ограниченное копирование: в exfat.cpp есть только неограниченный
// my_strcpy, а сюда приходят пути из IPC — обрезать безопаснее, чем
// надеяться на длину у вызывающего.
static void loc_copy(char* dst, const char* src, int cap) {
    int i = 0;
    while (src[i] && i < cap - 1) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

static void file_loc_invalidate(void) { g_file_loc.valid = false; }

static bool file_loc_same_path(EXFAT_Instance* fs, const char* path) {
    if (!g_file_loc.valid || g_file_loc.fs != fs) return false;
    const char* a = g_file_loc.path;
    while (*a && *path && *a == *path) { a++; path++; }
    return *a == '\0' && *path == '\0';
}

static void file_loc_store(EXFAT_Instance* fs, const char* path, uint32_t parent_clus,
                           bool parent_no_chain, uint64_t parent_len,
                           const char* basename, const ExfatSlot& slot) {
    g_file_loc.fs = fs;
    loc_copy(g_file_loc.path, path, (int)sizeof(g_file_loc.path));
    g_file_loc.parent_clus = parent_clus;
    g_file_loc.parent_no_chain = parent_no_chain;
    g_file_loc.parent_len = parent_len;
    loc_copy(g_file_loc.basename, basename, (int)sizeof(g_file_loc.basename));
    g_file_loc.slot = slot;
    g_file_loc.valid = true;
}


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
        // issuse.txt №41: имя с потерянными (не-Latin-1) символами не
        // может достоверно сопоставляться ни с каким запросом — разные
        // исходные Unicode-имена могут схлопнуться в одинаковую '?'-
        // строку, совпадение по ней рискует попасть не в тот файл.
        if (e.has_lossy_chars) continue;
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
// issuse.txt №43: возвращает false, если путь глубже MAX_COMPONENTS
// уровней вложенности — раньше лишние компоненты молча отбрасывались,
// и вызывающий получал ДРУГОЙ (более короткий) нормализованный путь без
// единой ошибки, рискуя выполнить операцию не над тем файлом/каталогом.
static bool exfat_normalize_path(EXFAT_Instance* fs, const char* input_path, char* out_normalized, int out_size) {
    char raw[256];
    int rp = 0;
    if (input_path[0] != '/') {
        int cl = my_strlen(fs->current_dir_path);
        for (int i = 0; i < cl && rp < 254; i++) raw[rp++] = fs->current_dir_path[i];
        if (rp == 0 || raw[rp - 1] != '/') { if (rp < 254) raw[rp++] = '/'; }
    }
    for (int i = 0; input_path[i] && rp < 254; i++) raw[rp++] = input_path[i];
    raw[rp] = '\0';

    constexpr int MAX_COMPONENTS = 16;
    // issuse.txt №66 (расследование) — было [64]: comp[] ниже принимает до
    // 255 символов (issuse.txt №42), my_strcpy(components[depth], comp) без
    // проверки границы — перезапись стека для ЛЮБОГО компонента пути
    // длиннее 63 символов. Не подтверждено как причина №66 (тестовое имя —
    // 52 символа, короче 64), но это реальное переполнение независимо от
    // этого — исправляем на тот же лимит, что и остальные exFAT-имена.
    char components[MAX_COMPONENTS][256];
    int depth = 0;
    const char* p = raw;
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        char comp[256]; // issuse.txt №42: exFAT-имя до 255 символов, был 64
        int clen = 0;
        while (*p && *p != '/' && clen < 255) comp[clen++] = *p++;
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
        if (depth >= MAX_COMPONENTS) return false; // issuse.txt №43: путь слишком глубокий — ошибка, не тихая обрезка
        my_strcpy(components[depth], comp); depth++;
    }

    int pos = 0;
    out_normalized[pos++] = '/';
    for (int i = 0; i < depth; i++) {
        if (i > 0 && pos < out_size - 1) out_normalized[pos++] = '/';
        for (int j = 0; components[i][j] && pos < out_size - 1; j++) out_normalized[pos++] = components[i][j];
    }
    out_normalized[pos] = '\0';
    return true;
}

uint32_t exfat_resolve_parent(EXFAT_Instance* fs, const char* full_path, char* out_basename) {
    char normalized[256];
    if (!exfat_normalize_path(fs, full_path, normalized, sizeof(normalized))) return 0xFFFFFFFF; // issuse.txt №43

    uint32_t current_clus = fs->root_cluster;
    bool current_no_chain = false;
    uint64_t current_len = 0;

    const char* p = normalized;
    while (*p == '/') p++;

    out_basename[0] = '\0';
    char token[256]; // issuse.txt №42
    while (*p) {
        int i = 0;
        while (*p && *p != '/' && i < 255) token[i++] = *p++;
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

uint32_t exfat_find_in_dir(EXFAT_Instance* fs, uint32_t dir_cluster, const char* target_name, bool* out_is_dir) {
    bool no_chain; uint64_t len;
    resolve_dir_extent(fs, dir_cluster, &no_chain, &len);

    ExfatSlot slot;
    if (!exfat_dir_scan(fs, dir_cluster, no_chain, len, target_name, &slot) || !slot.found) return 0xFFFFFFFF;

    if (out_is_dir) *out_is_dir = slot.is_dir;
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
// issuse.txt №66/№63(a) — кэш последней позиции обхода FAT-цепочки (см.
// read_extent() ниже). Живой тест: chunked-чтение (SYS_READ_FILE, offset
// растёт монотонно на каждый вызов — так читают cat.cpp и timedread.elf)
// 120МиБ фрагментированного файла ПОВИСЛО на 10+ минут без единого чанка.
// Причина: для файла БЕЗ NoFatChain-бита (реальная цепочка, не один
// сплошной run) read_extent() КАЖДЫЙ раз шёл по цепочке кластеров с самого
// начала (first_cluster), чтобы найти нужный cluster_index — O(индекс) на
// вызов, а раз offset растёт линейно с каждым чанком — O(N^2) на весь файл
// целиком. Единственный клиент этой функции — здесь, в этом же файле, и
// вызывается всегда последовательно (chunked-протокол только вперёд) —
// кэш валиден, только если это ТОТ ЖЕ файл (совпадает first_cluster) И
// запрошенный cluster_index >= кэшированного (иначе — честно с начала, как
// раньше, ничего не ломаем для произвольного/обратного доступа).
static uint32_t g_extent_cache_first_cluster = 0; // 0 = кэш пуст/невалиден (first_cluster всегда >= 2)
static uint32_t g_extent_cache_cluster_index = 0;
static uint32_t g_extent_cache_cluster = 0;

static uint32_t read_extent(EXFAT_Instance* fs, uint32_t first_cluster, bool no_fat_chain, uint32_t offset, char* out_buffer, uint32_t max_len) {
    uint32_t bytes_per_cluster = EXFAT_SECTOR_SIZE << fs->sectors_per_cluster_shift;
    if (bytes_per_cluster == 0 || first_cluster < 2) return 0;
    // Внутреннего потолка больше нет: размер куска задаёт вызывающий
    // (exfat_read_file/max_chunk), а данные теперь читаются ПРЯМО в его
    // буфер, а не через промежуточный staging фиксированного размера.

    uint32_t cluster_index = offset / bytes_per_cluster;
    uint32_t offset_in_cluster = offset % bytes_per_cluster;

    uint32_t cluster;
    uint32_t running_cluster_index = cluster_index; // индекс ТЕКУЩЕГО cluster — для кэша на выходе
    if (no_fat_chain) {
        cluster = first_cluster + cluster_index;
    } else {
        // issuse.txt №68 (продолжение) — сам кэш позиции (start_index) не
        // помог по-настоящему: fat_chain_has_cycle() двумя строками выше
        // ходит по ВСЕЙ цепочке целиком (алгоритм Флойда, O(длина цепочки))
        // БЕЗУСЛОВНО на каждый вызов, независимо от того, что мы уже кэшируем
        // позицию — оказалась доминирующей стоимостью (константные ~3с на
        // чанк на живом железе, 154МиБ файл, не зависит от offset — именно
        // так и выглядит "не растущая, но постоянная" стоимость O(N) на
        // каждый вызов). Раз цепочка для ЭТОГО first_cluster уже была
        // проверена раньше (кэш непуст и совпадает) — цикла в ней физически
        // не могло появиться с прошлого раза без изменения ФС (а любое
        // изменение ФС уже инвалидирует кэш в bitmap_free_run()) —
        // повторная проверка того же самого не нужна.
        bool already_verified = (g_extent_cache_first_cluster == first_cluster);
        if (!already_verified && fat_chain_has_cycle(fs, first_cluster)) return 0;

        uint32_t start_index = 0;
        if (already_verified && g_extent_cache_cluster_index <= cluster_index) {
            cluster = g_extent_cache_cluster;
            start_index = g_extent_cache_cluster_index;
        } else {
            cluster = first_cluster;
            start_index = 0;
        }
        for (uint32_t i = start_index; i < cluster_index; i++) {
            uint32_t next = fat_get_entry(fs, cluster);
            if (!exfat_cluster_has_next(next)) return 0;
            cluster = next;
        }
    }

    uint32_t copied = 0;
    char sbuf[512];
    uint32_t sectors_per_cluster = 1u << fs->sectors_per_cluster_shift;

    while (copied < max_len && cluster >= 2) {
        uint32_t sector_in_cluster = offset_in_cluster / EXFAT_SECTOR_SIZE;
        uint32_t byte_in_sector = offset_in_cluster % EXFAT_SECTOR_SIZE;
        uint32_t sector = cluster_to_sector(fs, cluster) + sector_in_cluster;
        uint32_t need = max_len - copied;
        uint32_t take;

        // Быстрый путь: читаем сразу НЕСКОЛЬКО секторов ПРЯМО в буфер
        // вызывающего. Раньше здесь всегда читался ровно один сектор в
        // sbuf, оттуда копировался в staging, а staging в конце копировался
        // в out_buffer — три прохода и по 512 байт на SCSI-команду. На
        // железе это давало "макс секторов в чтении: 1", то есть худший
        // возможный режим: команда на каждые полкилобайта.
        if (byte_in_sector == 0 && need >= EXFAT_SECTOR_SIZE) {
            uint32_t whole = need / EXFAT_SECTOR_SIZE;
            // Границу кластера соблюдаем ТОЛЬКО для файлов с FAT-цепочкой:
            // там следующий кластер может лежать где угодно. У непрерывного
            // экстента (no_fat_chain) кластеры идут подряд физически, и
            // упираться в границу значило бы резать чтение на куски размером
            // с кластер — при кластере 4 КБ это вернуло бы команды по 4 КБ,
            // ровно то, от чего уходим.
            if (!no_fat_chain) {
                uint32_t left_in_cluster = sectors_per_cluster - sector_in_cluster;
                if (whole > left_in_cluster) whole = left_in_cluster;
            }
            if (whole > fs->max_sectors_per_io) whole = fs->max_sectors_per_io;
            g_exfat_io_tag = EXFAT_IO_READ_EXTENT;
            if (!fs->read_blocks(sector, whole, out_buffer + copied)) break;
            take = whole * EXFAT_SECTOR_SIZE;
            copied += take;
            offset_in_cluster += take;
            // Могли перешагнуть сразу НЕСКОЛЬКО кластеров — отсюда цикл,
            // а не одна проверка.
            while (offset_in_cluster >= bytes_per_cluster) {
                offset_in_cluster -= bytes_per_cluster;
                running_cluster_index++;
                if (no_fat_chain) {
                    cluster = cluster + 1;
                } else {
                    uint32_t next = fat_get_entry(fs, cluster);
                    cluster = exfat_cluster_has_next(next) ? next : 0;
                    if (cluster < 2) break;
                }
            }
            continue;
        }

        // Медленный путь — только для неполного начала/хвоста сектора.
        g_exfat_io_tag = EXFAT_IO_READ_EXTENT;
        if (!fs->read_blocks(sector, 1, sbuf)) break;
        uint32_t avail = EXFAT_SECTOR_SIZE - byte_in_sector;
        take = need < avail ? need : avail;
        my_memcpy(out_buffer + copied, sbuf + byte_in_sector, take);
        copied += take;

        offset_in_cluster += take;
        if (offset_in_cluster >= bytes_per_cluster) {
            offset_in_cluster = 0;
            running_cluster_index++;
            if (no_fat_chain) {
                cluster = cluster + 1; // предел общей длины проверяет вызывающий (remaining/chunk)
            } else {
                uint32_t next = fat_get_entry(fs, cluster);
                cluster = exfat_cluster_has_next(next) ? next : 0;
            }
        }
    }

    if (!no_fat_chain && cluster >= 2) {
        g_extent_cache_first_cluster = first_cluster;
        g_extent_cache_cluster_index = running_cluster_index;
        g_extent_cache_cluster = cluster;
    }

    return copied; // данные уже лежат в буфере вызывающего
}

bool exfat_read_file(EXFAT_Instance* fs, const char* filename, char* out_buffer, uint32_t offset, uint32_t* bytes_read, uint32_t max_chunk) {
    *bytes_read = 0;
    char basename[256]; // issuse.txt №42
    uint32_t parent_clus = exfat_resolve_parent(fs, filename, basename);
    if (parent_clus == 0xFFFFFFFF || basename[0] == '\0') return false;

    bool parent_no_chain; uint64_t parent_len;
    resolve_dir_extent(fs, parent_clus, &parent_no_chain, &parent_len);

    ExfatSlot slot;
    if (!exfat_dir_scan(fs, parent_clus, parent_no_chain, parent_len, basename, &slot) || !slot.found || slot.is_dir) return false;

    if (offset >= slot.data_length) return true; // EOF
    uint32_t remaining = (uint32_t)(slot.data_length - offset);
    if (max_chunk == 0) max_chunk = 4096;
    uint32_t chunk = remaining > max_chunk ? max_chunk : remaining;

    uint32_t copied = read_extent(fs, slot.first_cluster, slot.no_fat_chain, offset, out_buffer, chunk);
    if (copied == 0) return false;
    *bytes_read = copied;
    return true;
}

bool exfat_read_text_file(EXFAT_Instance* fs, const char* path, char* out_buffer, uint32_t* out_copied) {
    char basename[256]; // issuse.txt №42
    uint32_t parent_clus = exfat_resolve_parent(fs, path, basename);
    if (parent_clus == 0xFFFFFFFF || basename[0] == '\0') return false;

    bool parent_no_chain; uint64_t parent_len;
    resolve_dir_extent(fs, parent_clus, &parent_no_chain, &parent_len);

    ExfatSlot slot;
    if (!exfat_dir_scan(fs, parent_clus, parent_no_chain, parent_len, basename, &slot) || !slot.found || slot.is_dir) return false;

    if (slot.data_length == 0 || slot.first_cluster < 2) {
        out_buffer[0] = '\0';
        if (out_copied) *out_copied = 0;
        return true;
    }
    uint32_t size = (uint32_t)slot.data_length;
    if (size > 4000) size = 4000;

    uint32_t copied = read_extent(fs, slot.first_cluster, slot.no_fat_chain, 0, out_buffer, size);
    out_buffer[copied] = '\0';
    if (out_copied) *out_copied = copied;
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
    if (!exfat_normalize_path(fs, path, normalized, sizeof(normalized))) return false; // issuse.txt №43

    uint32_t current_clus = fs->root_cluster;
    bool current_no_chain = false;
    uint64_t current_len = 0;

    const char* p = normalized;
    while (*p == '/') p++;
    char token[256]; // issuse.txt №42
    while (*p) {
        int i = 0;
        while (*p && *p != '/' && i < 255) token[i++] = *p++;
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
static void bitmap_sector_table_build(EXFAT_Instance* fs); // см. таблицу секторов битмапа ниже

// ============================================================================
// === МОНТИРОВАНИЕ ===
// ============================================================================
bool exfat_init(EXFAT_Instance* fs, block_read_fn read_func, block_write_fn write_func) {
    file_loc_invalidate(); // монтирование — кэш от прошлого тома недействителен целиком (см. FileLocCache)
    fs->read_blocks = read_func;
    fs->write_blocks = write_func;

    char sector_buf[512];
    g_exfat_io_tag = EXFAT_IO_READ_EXTENT;
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
        seL4_Yield(); // issuse.txt №15 — см. комментарий у fat_chain_has_cycle() выше
    }

    // Фаза 8 (`df`) — единственный ПОЛНЫЙ проход по битмапу за всю жизнь
    // монтирования, см. free_clusters_hint в h/exfat.h. Только если битмап
    // вообще найден (bitmap_cluster!=0) — иначе (не должно случаться на
    // валидном exFAT) hint остаётся 0, как и cluster_count-независимые поля.
    // Таблица секторов битмапа строится ДО первого прохода по нему —
    // иначе count_free_clusters() ниже сама пойдёт по медленному пути.
    bitmap_sector_table_build(fs);
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
// --- Таблица секторов битмапа (строится один раз при монтировании) ---
//
// ПРИЧИНА (найдена по замеру на железе 2026-09-06 и подтверждена чтением
// референса — fs/exfat/balloc.c в ядре Linux): bitmap_sector_for_byte()
// ниже шла по FAT-цепочке от НАЧАЛА битмапа на КАЖДЫЙ вызов, и каждый шаг
// цепочки — отдельное чтение сектора. Стоимость одного вызова O(k), где
// k — насколько далеко в битмап мы залезли, а суммарная стоимость прохода
// по битмапу получалась КВАДРАТИЧНОЙ. Для битмапа в 1 МБ это около 260
// тысяч чтений. Отсюда и наблюдение "команд становится всё больше": чем
// дальше уходит поиск свободного места, тем дороже каждый следующий шаг.
//
// Linux решает это тем, что при монтировании читает все секторы битмапа в
// массив sbi->vol_amap[] и дальше обращается по индексу, без единого
// прохода по цепочке. Здесь то же самое, но экономнее: кэшируются не
// данные, а НОМЕРА секторов — для битмапа в 1 МБ это 2048 записей по 4
// байта, 8 КБ, вместо мегабайта.
//
// Если битмап окажется больше таблицы (очень большой том), таблица просто
// не строится и работает прежний медленный путь — корректность не зависит
// от кэша.
constexpr uint32_t EXFAT_BITMAP_SECTORS_MAX = 8192; // до 4 МБ битмапа = 32M кластеров
static uint32_t g_bitmap_sectors[EXFAT_BITMAP_SECTORS_MAX];
static EXFAT_Instance* g_bitmap_owner = nullptr;
static uint32_t g_bitmap_sectors_count = 0;

static void bitmap_sector_table_invalidate(void) {
    g_bitmap_owner = nullptr;
    g_bitmap_sectors_count = 0;
}

// Строит таблицу ОДНИМ проходом по цепочке. Вызывается из exfat_init().
static void bitmap_sector_table_build(EXFAT_Instance* fs) {
    bitmap_sector_table_invalidate();
    if (fs->bitmap_cluster < 2 || fs->bitmap_size_bytes == 0) return;

    uint32_t sectors_per_cluster = 1u << fs->sectors_per_cluster_shift;
    uint32_t need = (fs->bitmap_size_bytes + EXFAT_SECTOR_SIZE - 1) / EXFAT_SECTOR_SIZE;
    if (need > EXFAT_BITMAP_SECTORS_MAX) return; // слишком большой том — остаёмся на прежнем пути

    uint32_t cluster = fs->bitmap_cluster;
    uint32_t filled = 0;
    while (filled < need) {
        uint32_t base = cluster_to_sector(fs, cluster);
        for (uint32_t i = 0; i < sectors_per_cluster && filled < need; i++) {
            g_bitmap_sectors[filled++] = base + i;
        }
        if (filled >= need) break;
        uint32_t next = fat_get_entry(fs, cluster);
        if (!exfat_cluster_has_next(next)) return; // цепочка короче ожидаемого — таблицу не публикуем
        cluster = next;
    }
    g_bitmap_sectors_count = need;
    g_bitmap_owner = fs;
}

static bool bitmap_sector_for_byte(EXFAT_Instance* fs, uint32_t byte_offset, uint32_t* out_sector, uint32_t* out_byte_in_sector) {
    // Быстрый путь: таблица секторов построена при монтировании (см. выше).
    if (g_bitmap_owner == fs && g_bitmap_sectors_count > 0) {
        uint32_t sec_index = byte_offset / EXFAT_SECTOR_SIZE;
        if (sec_index < g_bitmap_sectors_count) {
            *out_sector = g_bitmap_sectors[sec_index];
            *out_byte_in_sector = byte_offset % EXFAT_SECTOR_SIZE;
            return true;
        }
        return false; // за пределами битмапа — честный отказ
    }

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

// Пометить ПРОГОН кластеров одним проходом.
//
// Зачем отдельно от bitmap_set_bit(): та читает и пишет сектор битмапа на
// КАЖДЫЙ кластер. Один сектор битмапа описывает 4096 кластеров, поэтому
// разметка большого прогона превращалась в тысячи round-trip'ов по 512
// байт. Измерено на железе 2026-09-06: резервирование 12.5 МБ (3200
// кластеров по 4 КБ) стоило 3203 односекторных SCSI-команды — больше, чем
// вся полезная передача данных, и съело всю экономию потоковой записи.
//
// Здесь сектор читается один раз, в нём выставляются ВСЕ попавшие в него
// биты, и он пишется один раз. Для типичного прогона это 1 чтение + 1
// запись вместо тысяч.
static bool bitmap_set_run(EXFAT_Instance* fs, uint32_t first_cluster, uint32_t count, bool value) {
    if (fs->bitmap_cluster < 2 || first_cluster < 2 || count == 0) return false;
    char buf[512];
    uint32_t i = 0;
    while (i < count) {
        uint32_t bit_index  = (first_cluster + i) - 2;
        uint32_t byte_index = bit_index / 8;
        if (byte_index >= fs->bitmap_size_bytes) return false;

        // bitmap_sector_for_byte() НЕЛЬЗЯ звать на каждый кластер: она на
        // каждый вызов заново идёт по FAT-цепочке от fs->bitmap_cluster,
        // а это дисковые чтения. Первая версия батчинга звала её во
        // внутреннем цикле и на 3200 кластерах дала 40 МБ лишнего трафика
        // при файле в 12.5 МБ (hw 2026-09-06) — то есть «батчинг», который
        // сделал хуже. Зовём РОВНО ОДИН раз на сектор, а границу сектора
        // считаем арифметически: сектор битмапа описывает 512*8 кластеров.
        uint32_t sector, byte_in_sector;
        if (!bitmap_sector_for_byte(fs, byte_index, &sector, &byte_in_sector)) return false;
        g_exfat_io_tag = EXFAT_IO_BITMAP_SET;
        if (!fs->read_blocks(sector, 1, buf)) return false;

        // Сколько кластеров прогона попадает в ЭТОТ сектор: до его конца
        // остаётся (512 - byte_in_sector) байт, то есть столько же*8 бит,
        // минус смещение внутри первого байта.
        uint32_t bits_left_in_sector = (512u - byte_in_sector) * 8u - (bit_index % 8u);
        uint32_t take = count - i;
        if (take > bits_left_in_sector) take = bits_left_in_sector;

        for (uint32_t k = 0; k < take; k++) {
            uint32_t bi  = bit_index + k;
            uint32_t off = byte_in_sector + (bi / 8) - (bit_index / 8);
            if (off >= 512u) return false; // защита от арифметической ошибки выше
            bool was_set = (buf[off] & (1 << (bi % 8))) != 0;
            if (value) buf[off] = (char)(buf[off] | (1 << (bi % 8)));
            else       buf[off] = (char)(buf[off] & ~(1 << (bi % 8)));
            // free_clusters_hint — по РЕАЛЬНОМУ прошлому значению бита, как
            // и в bitmap_set_bit(), иначе счётчик df уедет.
            if (value && !was_set) { if (fs->free_clusters_hint > 0) fs->free_clusters_hint--; }
            else if (!value && was_set) { fs->free_clusters_hint++; }
        }
        g_exfat_io_tag = EXFAT_IO_BITMAP_SET;
        if (!fs->write_blocks(sector, 1, buf)) return false;
        i += take;
    }
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
    g_exfat_io_tag = EXFAT_IO_BITMAP_SET;
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
    g_exfat_io_tag = EXFAT_IO_BITMAP_SET;
    if (!fs->write_blocks(sector, 1, buf)) return false;
    if (value && !was_set) { if (fs->free_clusters_hint > 0) fs->free_clusters_hint--; }
    else if (!value && was_set) { fs->free_clusters_hint++; }
    return true;
}

// issuse.txt №45: возвращает false, если хотя бы один bitmap_set_bit()
// провалился (сбой чтения/записи сектора битмапа) — раньше возврат
// bitmap_set_bit() полностью игнорировался, вызывающий не мог узнать,
// что часть кластеров осталась помечена занятой навсегда.
static bool bitmap_free_run(EXFAT_Instance* fs, uint32_t first_cluster, uint32_t num_clusters) {
    bool ok = bitmap_set_run(fs, first_cluster, num_clusters, false); // см. bitmap_set_run()
    // issuse.txt №66 — освобождённые кластеры могут быть переиспользованы
    // ДРУГИМ файлом (bitmap_alloc_run), потенциально с тем же самым
    // first_cluster, что и у только что удалённого — кэш read_extent()
    // хранит позицию по одному лишь first_cluster, так что должен стать
    // невалидным здесь, иначе отдаст позицию из чужой, уже не существующей
    // цепочки.
    g_extent_cache_first_cluster = 0;
    return ok;
}

// Занять КОНКРЕТНЫЙ пробег кластеров, если он целиком свободен. В отличие
// от bitmap_alloc_run() ниже (та ищет место где угодно), здесь адрес задан:
// нужно продлить уже существующий экстент ВПРАВО, не сдвигая файл.
//
// Ради чего: без этого exfat_append_file() на каждой границе кластера
// выделяла новый пробег и КОПИРОВАЛА файл целиком — рост журнала стоил
// O(размер^2 / размер_кластера). На железе это выглядело парадоксом:
// запись по 2048 байт оказалась МЕДЛЕННЕЕ записи по 256 (27 КБ/с против
// 37), потому что дело было не в размере записи, а в размере файла —
// 3 МБ журнала means около сотни перевыделений со средней копией в 1.5 МБ,
// то есть больше сотни мегабайт лишнего I/O. С этой функцией типичный
// случай (за файлом свободно) обходится вообще без копирования.
//
// Возвращает true, ТОЛЬКО если заняты все num_clusters — при частичной
// свободе не занимает ничего, чтобы вызывающему не пришлось разбирать
// половинчатое состояние.
static bool bitmap_try_alloc_at(EXFAT_Instance* fs, uint32_t first_cluster, uint32_t num_clusters) {
    if (num_clusters == 0 || fs->bitmap_cluster < 2 || first_cluster < 2) return false;

    uint32_t cached_sector = 0xFFFFFFFF;
    char sector_buf[512];

    // Проход 1 — убедиться, что весь пробег свободен и лежит в пределах кучи.
    for (uint32_t i = 0; i < num_clusters; i++) {
        uint32_t bit_index = (first_cluster + i) - 2;
        if (bit_index >= fs->cluster_count) return false;
        uint32_t byte_index = bit_index / 8;
        if (byte_index >= fs->bitmap_size_bytes) return false;

        uint32_t sector, byte_in_sector;
        if (!bitmap_sector_for_byte(fs, byte_index, &sector, &byte_in_sector)) return false;
        if (sector != cached_sector) {
            g_exfat_io_tag = EXFAT_IO_BITMAP_SET;
            if (!fs->read_blocks(sector, 1, sector_buf)) return false;
            cached_sector = sector;
        }
        if ((sector_buf[byte_in_sector] & (1 << (bit_index % 8))) != 0) return false; // занято
    }

    // Проход 2 — занять. Раздельные проходы: занимать по ходу проверки
    // значило бы при отказе на середине оставить часть битов занятыми
    // навсегда (утечка места, которую никто потом не найдёт).
    if (!bitmap_set_run(fs, first_cluster, num_clusters, true)) {
        bitmap_free_run(fs, first_cluster, num_clusters); // откат
        return false;
    }
    return true;
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

    // bitmap_sector_for_byte() НЕЛЬЗЯ звать на каждый кластер: она на каждый
    // вызов заново идёт по FAT-цепочке от fs->bitmap_cluster, то есть делает
    // дисковые чтения. Раньше это сходило с рук, потому что поиск почти
    // всегда останавливался в первых байтах битмапа — так и написано в
    // старом комментарии здесь. Как только понадобился БОЛЬШОЙ непрерывный
    // прогон (резерв под журнал), поиск пошёл далеко, и на 3200 кластерах
    // это дало 27 МБ лишнего трафика при файле в 12.5 МБ (hw 2026-09-06).
    // Теперь адрес сектора берётся РОВНО ОДИН РАЗ на сектор: один сектор
    // битмапа описывает 512*8 = 4096 кластеров.
    constexpr uint32_t CLUSTERS_PER_BITMAP_SECTOR = 512u * 8u;

    uint32_t run_start = 0;
    uint32_t run_len = 0;
    char sector_buf[512];

    uint32_t c = 2;
    while (true) {
        uint32_t bit_index = c - 2;
        if (bit_index >= fs->cluster_count) break;
        uint32_t byte_index = bit_index / 8;
        // issuse.txt №37: паддинг-биты последнего байта не соответствуют
        // реальным кластерам — граница обязательна, иначе аллокатор мог бы
        // выдать кластер за пределами кучи.
        if (byte_index >= fs->bitmap_size_bytes) break;

        uint32_t sector, byte_in_sector;
        if (!bitmap_sector_for_byte(fs, byte_index, &sector, &byte_in_sector)) break;
        g_exfat_io_tag = EXFAT_IO_BITMAP_SCAN;
        if (!fs->read_blocks(sector, 1, sector_buf)) break;

        // Сколько кластеров описывает ОСТАТОК этого сектора.
        uint32_t in_sector = bit_index % CLUSTERS_PER_BITMAP_SECTOR;
        uint32_t left_here = CLUSTERS_PER_BITMAP_SECTOR - in_sector;

        for (uint32_t k = 0; k < left_here; k++) {
            uint32_t bi = bit_index + k;
            if (bi >= fs->cluster_count) { left_here = k; break; }
            uint32_t byi = bi / 8;
            if (byi >= fs->bitmap_size_bytes) { left_here = k; break; }
            uint32_t off = byte_in_sector + (byi - byte_index);
            if (off >= 512u) { left_here = k; break; }

            bool occupied = (sector_buf[off] & (1 << (bi % 8))) != 0;
            if (!occupied) {
                if (run_len == 0) run_start = bi + 2;
                run_len++;
                if (run_len >= num_clusters) {
                    if (!bitmap_set_run(fs, run_start, num_clusters, true)) return 0;
                    return run_start;
                }
            } else {
                run_len = 0;
            }
        }
        if (left_here == 0) break;
        c += left_here;
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
            chunk = exfat_local_chunk(fs, chunk); // sector_buf локальный, см. EXFAT_LOCAL_BUF_SECTORS
            if (chunk > sectors_per_cluster - sector_in_cluster) chunk = sectors_per_cluster - sector_in_cluster;
            g_exfat_io_tag = EXFAT_IO_COUNT_FREE;
            if (chunk == 0 || !fs->read_blocks(cluster_to_sector(fs, cur_cluster) + sector_in_cluster, chunk, sector_buf)) {
                sectors_loaded = 0;
                goto done;
            }
            sectors_loaded = chunk;
            seL4_Yield(); // issuse.txt №15 — см. комментарий у fat_chain_has_cycle() выше, тот же приём
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
// issuse.txt №45: возвращает false, если хотя бы один bitmap_set_bit()
// в цепочке провалился — раньше результат bitmap_set_bit() тут вообще не
// проверялся.
static bool free_fat_chain(EXFAT_Instance* fs, uint32_t start_cluster) {
    if (fat_chain_has_cycle(fs, start_cluster)) return false;
    uint32_t cluster = start_cluster;
    int guard = 0;
    bool ok = true;
    while (cluster >= 2 && guard++ < 65536) {
        uint32_t next = fat_get_entry(fs, cluster);
        fat_set_entry(fs, cluster, 0);
        if (!bitmap_set_bit(fs, cluster, false)) ok = false;
        if (!exfat_cluster_has_next(next)) break;
        cluster = next;
    }
    return ok;
}

// Освобождает данные существующей записи (файла или каталога) — общий шаг
// для перезаписи/удаления/переименования поверх существующего имени.
// issuse.txt №45: теперь возвращает bool (было void) — false означает,
// что часть кластеров НЕ была реально освобождена на диске (сбой I/O),
// то есть навсегда останется занятой/недоступной. Сама запись каталога
// при этом уже удалена/переписана вызывающим ДО или ПОСЛЕ этого вызова
// независимо от результата — здесь только учёт кластеров, откатывать
// операцию уже поздно и незачем (см. вызывающих ниже).
static bool free_slot_data(EXFAT_Instance* fs, const ExfatSlot& slot) {
    if (slot.first_cluster < 2) return true;
    if (slot.no_fat_chain) {
        uint32_t bpc = EXFAT_SECTOR_SIZE << fs->sectors_per_cluster_shift;
        uint32_t n = (uint32_t)((slot.data_length + bpc - 1) / bpc);
        if (n == 0) n = 1; // директории хранят размер СВОЕГО аллоцированного экстента, см. exfat_mkdir
        return bitmap_free_run(fs, slot.first_cluster, n);
    } else {
        return free_fat_chain(fs, slot.first_cluster);
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
            uint32_t chunk_sectors = full_sectors > fs->max_sectors_per_io ? fs->max_sectors_per_io : full_sectors;
            g_exfat_io_tag = EXFAT_IO_STREAM_WRITE;
            if (!fs->write_blocks(sector, chunk_sectors, p)) return false;
            uint32_t chunk_bytes = chunk_sectors * EXFAT_SECTOR_SIZE;
            p += chunk_bytes;
            remaining -= chunk_bytes;
            sector += chunk_sectors;
            continue;
        }
        for (uint32_t i = 0; i < remaining; i++) pad_buf[i] = p[i];
        for (uint32_t i = remaining; i < EXFAT_SECTOR_SIZE; i++) pad_buf[i] = 0;
        g_exfat_io_tag = EXFAT_IO_STREAM_WRITE;
        if (!fs->write_blocks(sector, 1, pad_buf)) return false;
        remaining = 0;
    }
    return true;
}

// Запись в НЕПРЕРЫВНЫЙ экстент по произвольному байтовому смещению
// (write_extent_data умеет только с начала). Нужна для exfat_append_file():
// дописывание почти всегда начинается посреди сектора, и затирать его
// начало нулями нельзя — поэтому первый и последний неполные секторы
// читаются, правятся и пишутся обратно.
static bool write_extent_at(EXFAT_Instance* fs, uint32_t first_cluster, uint64_t offset,
                            const char* data, uint32_t len) {
    static char sec_buf[EXFAT_SECTOR_SIZE];
    uint32_t sector = cluster_to_sector(fs, first_cluster) + (uint32_t)(offset / EXFAT_SECTOR_SIZE);
    uint32_t in_sec = (uint32_t)(offset % EXFAT_SECTOR_SIZE);
    const char* p = data;
    uint32_t remaining = len;

    if (in_sec != 0) {
        g_exfat_io_tag = EXFAT_IO_STREAM_WRITE;
        if (!fs->read_blocks(sector, 1, sec_buf)) return false;
        uint32_t n = EXFAT_SECTOR_SIZE - in_sec;
        if (n > remaining) n = remaining;
        for (uint32_t i = 0; i < n; i++) sec_buf[in_sec + i] = p[i];
        g_exfat_io_tag = EXFAT_IO_STREAM_WRITE;
        if (!fs->write_blocks(sector, 1, sec_buf)) return false;
        p += n; remaining -= n; sector++;
    }
    while (remaining >= EXFAT_SECTOR_SIZE) {
        uint32_t chunk = remaining / EXFAT_SECTOR_SIZE;
        if (chunk > fs->max_sectors_per_io) chunk = fs->max_sectors_per_io;
        g_exfat_io_tag = EXFAT_IO_STREAM_WRITE;
        if (!fs->write_blocks(sector, chunk, p)) return false;
        uint32_t b = chunk * EXFAT_SECTOR_SIZE;
        p += b; remaining -= b; sector += chunk;
    }
    if (remaining > 0) {
        g_exfat_io_tag = EXFAT_IO_STREAM_WRITE;
        if (!fs->read_blocks(sector, 1, sec_buf)) return false;
        for (uint32_t i = 0; i < remaining; i++) sec_buf[i] = p[i];
        g_exfat_io_tag = EXFAT_IO_STREAM_WRITE;
        if (!fs->write_blocks(sector, 1, sec_buf)) return false;
    }
    return true;
}

// Посекторное копирование непрерывного экстента. Буфер статический и
// маленький — файл может быть сильно больше любой разумной локальной
// переменной, а копировать надо целиком.
static bool copy_extent(EXFAT_Instance* fs, uint32_t src_first, uint32_t dst_first, uint64_t bytes) {
    g_exfat_copy_extent_calls++;
    static char cp_buf[EXFAT_SECTOR_SIZE * 8];
    uint32_t src = cluster_to_sector(fs, src_first);
    uint32_t dst = cluster_to_sector(fs, dst_first);
    uint64_t sectors = (bytes + EXFAT_SECTOR_SIZE - 1) / EXFAT_SECTOR_SIZE;
    while (sectors > 0) {
        uint32_t chunk = exfat_local_chunk(fs, sectors > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)sectors); // cp_buf локальный
        g_exfat_io_tag = EXFAT_IO_READ_EXTENT;
        if (!fs->read_blocks(src, chunk, cp_buf)) return false;
        g_exfat_io_tag = EXFAT_IO_READ_EXTENT;
        if (!fs->write_blocks(dst, chunk, cp_buf)) return false;
        src += chunk; dst += chunk; sectors -= chunk;
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
    file_loc_invalidate(); // создание файла может добавить запись в каталог и сдвинуть соседние (см. FileLocCache)
    if (out_existed) *out_existed = false;
    char basename[256]; // issuse.txt №42
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
    file_loc_invalidate(); // полная перезапись меняет first_cluster и длину (см. FileLocCache)
    char basename[256]; // issuse.txt №42
    uint32_t parent_clus;
    bool parent_no_chain; uint64_t parent_len;
    ExfatSlot slot;
    bool exists;

    if (file_loc_same_path(fs, path)) {
        // Тот же файл, что и в прошлый раз — поиск не нужен (см. FileLocCache).
        parent_clus     = g_file_loc.parent_clus;
        parent_no_chain = g_file_loc.parent_no_chain;
        parent_len      = g_file_loc.parent_len;
        loc_copy(basename, g_file_loc.basename, (int)sizeof(basename));
        slot            = g_file_loc.slot;
        exists          = true;
    } else {
        parent_clus = exfat_resolve_parent(fs, path, basename);
        if (parent_clus == 0xFFFFFFFF || basename[0] == '\0') return false;
        resolve_dir_extent(fs, parent_clus, &parent_no_chain, &parent_len);
        exists = exfat_dir_scan(fs, parent_clus, parent_no_chain, parent_len, basename, &slot) && slot.found;
        if (exists && !slot.is_dir) {
            file_loc_store(fs, path, parent_clus, parent_no_chain, parent_len, basename, slot);
        }
    }
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

// Дописывание в конец файла. Раньше в системе его НЕ БЫЛО вовсе: единственной
// записью была exfat_write_file(), перезаписывающая файл целиком, поэтому
// журнал/бортовой самописец приходилось эмулировать циклом
// "прочитать всё -> дописать -> записать всё", с квадратичной стоимостью и
// потолком 4096 байт на файл (лимит транспорта cmd 113).
//
// Два пути:
//  - БЫСТРЫЙ: новые данные помещаются в уже выделенные кластеры. Пишем
//    только в хвост и обновляем длину в записи каталога. Для журнала это
//    подавляющее большинство вызовов (кластер 32КБ, запись 256Б — 127 из 128).
//  - МЕДЛЕННЫЙ: нужен экстент больше. Выделяем новый непрерывный прогон,
//    копируем старое, дописываем новое, переписываем запись каталога,
//    освобождаем старый прогон. Срабатывает только на границе кластера.
//
// ЧЕСТНО ПРО СТОИМОСТЬ: медленный путь копирует файл целиком, поэтому рост
// журнала стоит O(размер / размер_кластера) копирований. Для настоящего
// долгоживущего самописца правильнее наращивать цепочку кластеров на месте
// (FAT-chain или запас в ValidDataLength) — здесь этого сознательно нет,
// чтобы не трогать формат размещения, на который завязан весь остальной код.
bool exfat_append_file(EXFAT_Instance* fs, const char* path, const char* text, uint32_t len) {
    g_exfat_append_calls++;
    if (len == 0) return true;

    char basename[256]; // issuse.txt №42
    uint32_t parent_clus = exfat_resolve_parent(fs, path, basename);
    if (parent_clus == 0xFFFFFFFF || basename[0] == '\0') return false;

    bool parent_no_chain; uint64_t parent_len;
    resolve_dir_extent(fs, parent_clus, &parent_no_chain, &parent_len);

    ExfatSlot slot;
    bool exists = exfat_dir_scan(fs, parent_clus, parent_no_chain, parent_len, basename, &slot) && slot.found;
    // Файла нет — дописывание в несуществующий файл это просто создание.
    if (!exists) return exfat_write_file(fs, path, text, len);
    if (slot.is_dir) return false;
    // Чужой формат размещения (цепочка FAT) — отказываем, а не портим файл.
    if (slot.first_cluster != 0 && !slot.no_fat_chain) return false;

    uint32_t bpc = EXFAT_SECTOR_SIZE << fs->sectors_per_cluster_shift;
    uint64_t old_len = slot.data_length;
    uint64_t new_len = old_len + len;
    uint32_t old_clusters = (uint32_t)((old_len + bpc - 1) / bpc);
    uint32_t need_clusters = (uint32_t)((new_len + bpc - 1) / bpc);

    if (slot.first_cluster != 0 && need_clusters <= old_clusters) {
        if (!write_extent_at(fs, slot.first_cluster, old_len, text, len)) return false;
        if (!exfat_write_entry_set_at(slot.entry_start, basename, 0x20, slot.first_cluster, new_len, true)) return false;
        // Запись каталога осталась на месте — двигать кэш не нужно, только
        // догнать длину, иначе следующий вызов возьмёт устаревший old_len.
        if (file_loc_same_path(fs, path)) g_file_loc.slot.data_length = new_len;
        return true;
    }

    // СНАЧАЛА пробуем продлить экстент НА МЕСТЕ — за концом файла обычно
    // свободно, и тогда дописывание не стоит вообще ничего сверх записи
    // самих данных. Именно отсутствие этой ветки делало рост журнала
    // квадратичным (см. bitmap_try_alloc_at выше).
    if (slot.first_cluster != 0 && old_clusters > 0 &&
        bitmap_try_alloc_at(fs, slot.first_cluster + old_clusters, need_clusters - old_clusters)) {
        if (!write_extent_at(fs, slot.first_cluster, old_len, text, len)) {
            bitmap_free_run(fs, slot.first_cluster + old_clusters, need_clusters - old_clusters);
            return false;
        }
        if (!exfat_write_entry_set_at(slot.entry_start, basename, 0x20, slot.first_cluster, new_len, true)) return false;
        if (file_loc_same_path(fs, path)) g_file_loc.slot.data_length = new_len;
        return true;
    }

    g_exfat_append_slow_calls++;
    // Не вышло (за файлом занято) — прежний путь: новый пробег + копирование.
    uint32_t new_clus = bitmap_alloc_run(fs, need_clusters);
    if (new_clus == 0) return false;
    if (old_len > 0 && slot.first_cluster != 0) {
        if (!copy_extent(fs, slot.first_cluster, new_clus, old_len)) {
            bitmap_free_run(fs, new_clus, need_clusters);
            return false;
        }
    }
    if (!write_extent_at(fs, new_clus, old_len, text, len)) {
        bitmap_free_run(fs, new_clus, need_clusters);
        return false;
    }
    // Запись каталога переписывается ПОСЛЕ того, как новые данные легли —
    // тот же порядок, что в exfat_write_file(): при обрыве питания посреди
    // операции файл остаётся прежним, а не половинчатым.
    if (!exfat_write_entry_set_at(slot.entry_start, basename, 0x20, new_clus, new_len, true)) {
        bitmap_free_run(fs, new_clus, need_clusters);
        return false;
    }
    if (old_len > 0 && slot.first_cluster != 0) free_slot_data(fs, slot);
    // Файл переехал на другой экстент — кэш обязан узнать новое место,
    // иначе следующее дописывание пойдёт по освобождённым кластерам.
    if (file_loc_same_path(fs, path)) {
        g_file_loc.slot.first_cluster = new_clus;
        g_file_loc.slot.data_length = new_len;
    }
    return true;
}

// ============================================================================
// === ПОТОКОВАЯ ЗАПИСЬ (см. подробное "зачем" в h/exfat.h) ===
// ============================================================================

bool exfat_stream_open(EXFAT_Instance* fs, const char* path, uint64_t reserve_bytes, ExfatStream* out) {
    if (!out) return false;
    out->active = false;
    file_loc_invalidate(); // поток меняет размещение файла — кэш обычного пути недействителен

    char basename[256]; // issuse.txt №42
    uint32_t parent_clus = exfat_resolve_parent(fs, path, basename);
    if (parent_clus == 0xFFFFFFFF || basename[0] == '\0') return false;

    bool parent_no_chain; uint64_t parent_len;
    resolve_dir_extent(fs, parent_clus, &parent_no_chain, &parent_len);

    ExfatSlot slot;
    bool exists = exfat_dir_scan(fs, parent_clus, parent_no_chain, parent_len, basename, &slot) && slot.found;
    if (exists && slot.is_dir) return false;

    uint32_t bpc = EXFAT_SECTOR_SIZE << fs->sectors_per_cluster_shift;
    if (bpc == 0) return false;
    if (reserve_bytes == 0) reserve_bytes = bpc;
    uint32_t need = (uint32_t)((reserve_bytes + bpc - 1) / bpc);

    uint32_t clus = bitmap_alloc_run(fs, need);
    if (clus == 0) return false; // нет непрерывного места нужного размера — честный отказ

    // Запись каталога СРАЗУ учитывает ВЕСЬ резерв, а не нулевую длину.
    //
    // Так задумано не ради удобства: если процесс умрёт между открытием и
    // закрытием (а это уже случалось на железе — тест падал на закрытии),
    // при нулевой длине зарезервированные кластеры остаются занятыми в
    // битмапе, но не принадлежат никакому файлу. Освободить их нечем: rm
    // удаляет то, что числится за записью каталога, а там ноль. Диск
    // необратимо теряет место — на 12.5 МБ прогоне это заметили сразу,
    // второй запуск уже не нашёл непрерывного участка.
    // С полным резервом в длине крах оставляет файл с мусорным хвостом —
    // некрасиво, но rm вернёт всё место до байта.
    uint64_t reserved_bytes = (uint64_t)need * bpc;
    bool ok = exists
        ? exfat_write_entry_set_at(slot.entry_start, basename, 0x20, clus, reserved_bytes, true)
        : exfat_write_entry_set(fs, parent_clus, parent_no_chain, parent_len, basename, 0x20, clus, reserved_bytes, true);
    if (!ok) { bitmap_free_run(fs, clus, need); return false; }

    // Старые данные освобождаем только ПОСЛЕ успешной перезаписи записи —
    // тот же порядок, что в exfat_write_file().
    if (exists && slot.first_cluster >= 2 && slot.data_length > 0) free_slot_data(fs, slot);

    // Проверяем, что запись каталога действительно приняла новый экстент.
    // На железе дважды было так, что открытие/закрытие рапортовали успех, а
    // файл потом читался пустым — значит писали не туда, куда думали.
    {
        ExfatSlot chk;
        if (!exfat_dir_scan(fs, parent_clus, parent_no_chain, parent_len, basename, &chk)
            || !chk.found || chk.first_cluster != clus) {
            bitmap_free_run(fs, clus, need);
            return false;
        }
    }

    out->parent_cluster  = parent_clus;
    out->parent_no_chain = parent_no_chain;
    out->parent_len      = parent_len;

    for (int i = 0; i < 256; i++) out->basename[i] = basename[i];
    out->first_cluster     = clus;
    out->reserved_clusters = need;
    out->length            = 0;
    out->flushed_length    = 0;
    out->active            = true;
    return true;
}

bool exfat_stream_write(EXFAT_Instance* fs, ExfatStream* st, const char* data, uint32_t len) {
    g_exfat_stream_write_calls++;
    if (!st || !st->active || len == 0) return st && st->active;
    uint32_t bpc = EXFAT_SECTOR_SIZE << fs->sectors_per_cluster_shift;
    uint64_t capacity = (uint64_t)st->reserved_clusters * bpc;
    // Молча оборвать запись хуже, чем отказать: вызывающий должен узнать,
    // что зарезервированное место кончилось, а не досчитывать до конца с
    // потерянным хвостом.
    if (st->length + len > capacity) return false;
    if (!write_extent_at(fs, st->first_cluster, st->length, data, len)) return false;
    st->length += len;
    return true;
}

bool exfat_stream_flush(EXFAT_Instance* fs, ExfatStream* st) {
    if (!st || !st->active) return false;
    // Сравнение с flushed_length больше НЕ используется как признак "нечего
    // делать": после открытия в каталоге стоит длина РЕЗЕРВА, а не 0, и
    // совпадение счётчиков ничего не гарантирует. Пишем всегда — операция
    // редкая (раз на закрытие или на явный сброс).
    // Запись каталога ищется ЗАНОВО: кэшировать её позицию нельзя, вместе с
    // ней кэшируется снимок сектора (см. комментарий у ExfatStream).
    ExfatSlot slot;
    if (!exfat_dir_scan(fs, st->parent_cluster, st->parent_no_chain, st->parent_len, st->basename, &slot) || !slot.found) return false;
    if (!exfat_write_entry_set_at(slot.entry_start, st->basename, 0x20, st->first_cluster, st->length, true)) return false;
    st->flushed_length = st->length;
    return true;
}

bool exfat_stream_close(EXFAT_Instance* fs, ExfatStream* st) {
    if (!st || !st->active) return false;
    bool ok = exfat_stream_flush(fs, st);
    // Проверяем, что длина ДЕЙСТВИТЕЛЬНО встала в каталог, а не просто
    // "запись прошла без ошибки". На железе уже дважды было так, что
    // закрытие рапортовало успех, а файл потом читался пустым — молчаливое
    // расхождение хуже честного отказа.
    if (ok) {
        ExfatSlot chk;
        if (!exfat_dir_scan(fs, st->parent_cluster, st->parent_no_chain, st->parent_len, st->basename, &chk)
            || !chk.found || chk.data_length != st->length) {
            ok = false;
        }
    }
    // Лишние зарезервированные кластеры возвращаем в битмап: иначе журнал,
    // открытый "с запасом на гигабайт", навсегда съедал бы этот гигабайт.
    uint32_t bpc = EXFAT_SECTOR_SIZE << fs->sectors_per_cluster_shift;
    uint32_t used = (uint32_t)((st->length + bpc - 1) / bpc);
    if (used < st->reserved_clusters) {
        bitmap_free_run(fs, st->first_cluster + used, st->reserved_clusters - used);
    }
    st->active = false;
    file_loc_invalidate();
    return ok;
}

bool exfat_mkdir(EXFAT_Instance* fs, const char* path, bool* out_existed) {
    file_loc_invalidate(); // новый каталог — новая запись в родителе (см. FileLocCache)
    if (out_existed) *out_existed = false;
    char basename[256]; // issuse.txt №42
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
        uint32_t chunk = exfat_local_chunk(fs, remaining); // zero_buf локальный
        g_exfat_io_tag = EXFAT_IO_ENTRY_WRITE;
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
    file_loc_invalidate(); // удаление освобождает кластеры, кэш стал бы указывать в никуда (см. FileLocCache)
    char basename[256]; // issuse.txt №42
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
    file_loc_invalidate(); // переименование меняет и имя, и позицию записи (см. FileLocCache)
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

    // issuse.txt №44: раньше сначала писали НОВУЮ запись, потом удаляли
    // старую — если удаление старой проваливалось (например временный
    // сбой записи), обе записи оставались живыми и указывали на ОДНУ и
    // ту же цепочку кластеров: последующий rm любой из них освободил бы
    // кластеры, на которые всё ещё ссылается уцелевшая запись (активное
    // повреждение при следующем чтении/записи через неё). Меняем порядок:
    // сначала удаляем старую запись, потом пишем новую — при сбое второго
    // шага файл временно "теряется" (запись удалена, новая не создана),
    // но это безопаснее дублирования/алиасинга — кластеры остаются
    // консистентно принадлежащими ровно одной (уже несуществующей) записи,
    // а не двум одновременно.
    if (!exfat_mark_entry_deleted(slot.entry_start, slot.secondary_count)) {
        return false;
    }
    uint16_t attrs = slot.is_dir ? 0x10 : 0x20;
    return exfat_write_entry_set(fs, new_parent, new_parent_no_chain, new_parent_len, new_basename, attrs, slot.first_cluster, slot.data_length, slot.no_fat_chain);
}
