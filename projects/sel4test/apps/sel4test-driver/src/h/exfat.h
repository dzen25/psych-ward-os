#pragma once
#include <stdint.h>
#include <stdbool.h>

// Уход от FAT32/8.3 (см. ROADMAP.md/issuse.txt) — весь класс багов вокруг
// коротких DOS-имён и LFN-записей (регистронезависимость, отсутствие LFN у
// каталогов/файлов, созданных внешними инструментами для имён, "влезающих" в
// 8.3) структурно невозможен в exFAT: у файла ровно одно длинное имя, короткого
// алиаса не существует вообще. Этот драйвер работает ТОЛЬКО со второй
// партицией карты — первая (FAT32, читает прошивка RPi/U-Boot) им не
// монтируется и не трогается никогда, см. find_fat32_partition()/
// find_exfat_partition() в blk_driver.cpp.

typedef bool (*block_read_fn)(uint32_t sector, uint32_t count, void* buffer);
typedef bool (*block_write_fn)(uint32_t sector, uint32_t count, const void* buffer);

// Состояние смонтированного exFAT-тома (аналог FAT32_Instance).
struct EXFAT_Instance {
    // Сколько секторов драйвер готов перенести за ОДИН вызов read/write_blocks.
    // Зашитая восьмёрка (4 КБ) жила в пяти местах exfat.cpp и оставалась
    // единственным потолком даже после расширения bounce-буфера USB до 64 КБ:
    // запись 64 КБ распадалась на 16 SCSI-команд вместо одной. Значение
    // выставляет сам драйвер после exfat_init() — у каждого свой буфер.
    uint32_t max_sectors_per_io = 8;
    block_read_fn read_blocks;
    block_write_fn write_blocks;

    // Кэшированные метаданные из Main Boot Sector
    uint32_t fat_offset_sectors;     // FatOffset
    uint32_t fat_length_sectors;     // FatLength
    uint32_t cluster_heap_offset;    // ClusterHeapOffset
    uint32_t cluster_count;          // ClusterCount
    uint8_t  sectors_per_cluster_shift; // SectorsPerClusterShift
    uint32_t root_cluster;           // FirstClusterOfRootDirectory
    uint32_t current_dir_cluster;    // CWD (кластер)

    // exFAT НЕ хранит записи "."/".." — в отличие от FAT32, кластер родителя
    // невозможно узнать из самой дочерней папки. Поэтому CWD дополнительно
    // отслеживается как НОРМАЛИЗОВАННАЯ (без "."/"..") абсолютная строка пути
    // от корня — ".." резолвится текстовой нормализацией ДО обхода диска, сам
    // обход всегда идёт только вперёд (find_in_dir по очередному токену).
    char current_dir_path[128];
    bool current_dir_no_fat_chain;   // NoFatChain текущего CWD (см. GeneralSecondaryFlags)
    uint64_t current_dir_byte_length; // DataLength текущего CWD (значим только если current_dir_no_fat_chain)

    // Найдены при монтировании обходом корневого каталога
    uint32_t bitmap_cluster;         // FirstCluster записи 0x81
    uint32_t bitmap_size_bytes;      // DataLength записи 0x81 (== ceil(cluster_count/8))

    // Фаза 8 (`df`) — НАЙДЕНО НА ЖИВОМ ЖЕЛЕЗЕ: полный обход Allocation
    // Bitmap на КАЖДЫЙ вызов `df` (сотни SCSI-транзакций подряд на USB)
    // валил bulk-передачи после нескольких вызовов подряд, на РАЗНЫХ
    // устройствах — не баг конкретной флешки, а просто нагрузка. Так этого
    // не делает ни одна настоящая ОС: Linux `df`/statfs() читает кэш
    // свободных блоков, который ФС-драйвер поддерживает в памяти
    // инкрементально, диск трогается только при монтировании. Здесь то же
    // самое: одноразовый скан в exfat_init(), дальше bitmap_set_bit()
    // инкрементально поддерживает счётчик при каждой аллокации/освобождении
    // — exfat_free_space() больше не делает I/O вообще.
    uint32_t free_clusters_hint;
};

// --- API файловой системы (сигнатуры зеркалируют fat32.h нарочно —
// blk_driver.cpp переключается заменой имён вызовов, не переписыванием) ---

bool exfat_init(EXFAT_Instance* fs, block_read_fn read_func, block_write_fn write_func);

// Фаза 8 (мониторинг ресурсов, `df`) — read-only обход Allocation Bitmap,
// тот же приём, что bitmap_alloc_run() в exfat.cpp, но только считает
// occupied-биты, ничего не резервирует.
bool exfat_free_space(EXFAT_Instance* fs, uint64_t* out_total_bytes, uint64_t* out_free_bytes);

bool exfat_format_dir_listing(EXFAT_Instance* fs, uint32_t dir_cluster, char* out_buffer, uint32_t max_len);

// max_chunk — сколько байт максимум вернуть за один вызов. Раньше было
// зашито 4096 (размер страницы SHM); с расширением области полезной
// нагрузки (см. VFS_PAYLOAD_MAX в platform.h) вызывающий задаёт сам.
bool exfat_read_file(EXFAT_Instance* fs, const char* filename, char* out_buffer, uint32_t offset, uint32_t* bytes_read, uint32_t max_chunk = 4096);
// out_copied (может быть nullptr) — issuse.txt №56: сколько байт реально
// скопировано (до nul-терминатора в out_buffer это не учитывает — сам
// out_buffer '\0'-терминирован ВСЕГДА, даже если в файле встретился
// нулевой байт раньше конца — вызывающий может сравнить out_copied со
// strlen(out_buffer), чтобы отличить "весь файл текстовый" от "файл
// бинарный/оборван на первом нулевом байте").
bool exfat_read_text_file(EXFAT_Instance* fs, const char* path, char* out_buffer, uint32_t* out_copied = nullptr);

// out_existed (может быть nullptr) — при true уже существовал, ничего не
// создавалось (в отличие от возврата false = реальная ошибка).
bool exfat_create_file(EXFAT_Instance* fs, const char* path, bool* out_existed = nullptr);
bool exfat_write_file(EXFAT_Instance* fs, const char* path, const char* text, uint32_t len);
// Дописывание в конец (issuse: до 2026-09-06 append в системе не было вовсе).
bool exfat_append_file(EXFAT_Instance* fs, const char* path, const char* text, uint32_t len);

// Тег текущей операции exFAT. Драйвер считает блочные операции по тегам —
// без этого из общих цифр не видно, КТО именно генерирует трафик, и разбор
// вырождается в перебор версий.
enum ExfatIoTag {
    EXFAT_IO_OTHER = 0,
    EXFAT_IO_STREAM_WRITE,
    EXFAT_IO_BITMAP_SCAN,
    EXFAT_IO_BITMAP_SET,
    EXFAT_IO_DIR_CURSOR,
    EXFAT_IO_ENTRY_WRITE,
    EXFAT_IO_COUNT_FREE,
    EXFAT_IO_READ_EXTENT,
    EXFAT_IO_FAT,
    EXFAT_IO_TAG_MAX
};
extern uint32_t g_exfat_io_tag;
extern uint32_t g_exfat_copy_extent_calls;
extern uint32_t g_exfat_append_slow_calls;
extern uint32_t g_exfat_append_calls;
extern uint32_t g_exfat_stream_write_calls;

// Позиция в каталоге. Жила в exfat.cpp, но ExfatStream ниже держит её
// полем — потоку нужно помнить, КУДА писать длину при сбросе, не
// пересканируя каталог.
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
    // issuse.txt №38: счётчик шагов по FAT-цепочке (только для
    // no_fat_chain=false — root/чужеродные фрагментированные каталоги) —
    // см. dir_cursor_advance() ниже.
    uint32_t fat_chain_steps;
};

// --- ПОТОКОВАЯ ЗАПИСЬ (журнал/бортовой самописец) ---
//
// Зачем отдельно от exfat_append_file(): та на КАЖДЫЙ вызов заново
// проверяет размер экстента и ПЕРЕЗАПИСЫВАЕТ запись каталога, чтобы длина
// файла всегда была актуальной. Для журнала это чистая потеря — запись
// каталога лежит по далёкому адресу, и такая мелкая разбросанная запись
// для флеш-памяти самая дорогая из возможных.
//
// Здесь наоборот: всё, что можно, делается ОДИН раз при открытии —
// разбор пути, поиск записи, резервирование непрерывного места под весь
// объём. Дальше exfat_stream_write() только кладёт данные по текущему
// смещению, не трогая ни битмап, ни каталог.
//
// ЦЕНА, которую надо понимать: пока поток не закрыт, в каталоге стоит
// длина на момент последнего сброса. При внезапном пропадании питания
// данные на диске есть, но файл будет числиться короче. Для самописца это
// обычный компромисс, но он именно компромисс, а не бесплатный выигрыш —
// поэтому сброс вынесен отдельной операцией (exfat_stream_flush), чтобы
// вызывающий сам решал, как часто платить за него.
struct ExfatStream {
    bool     active;
    char     basename[256];
    // Позиция записи каталога НЕ кэшируется намеренно. Хранили здесь копию
    // DirCursor — вместе с ней хранился и СНИМОК сектора каталога, снятый
    // при открытии. Запись длины при закрытии шла через этот устаревший
    // снимок, и на железе 2026-09-06 файл после закрытия читался как пустой
    // (длина в каталог фактически не долетала). Вместо кэша храним данные
    // родительского каталога и находим запись заново — это один скан на
    // закрытие, то есть цена, которой не жалко.
    uint32_t parent_cluster;
    bool     parent_no_chain;
    uint64_t parent_len;
    uint32_t first_cluster;     // начало зарезервированного непрерывного экстента
    uint32_t reserved_clusters; // сколько кластеров зарезервировано
    uint64_t length;            // сколько реально записано
    uint64_t flushed_length;    // что сейчас стоит в каталоге
};

// Открыть поток. reserve_bytes — под сколько сразу зарезервировать место;
// превысить его записями нельзя (честный отказ вместо тихого расширения).
// Существующее содержимое файла отбрасывается.
bool exfat_stream_open(EXFAT_Instance* fs, const char* path, uint64_t reserve_bytes, ExfatStream* out);
// Записать по текущему смещению и сдвинуть его. Каталог не трогается.
bool exfat_stream_write(EXFAT_Instance* fs, ExfatStream* st, const char* data, uint32_t len);
// Записать актуальную длину в каталог (можно звать сколько угодно раз).
bool exfat_stream_flush(EXFAT_Instance* fs, ExfatStream* st);
// Сброс + закрытие.
bool exfat_stream_close(EXFAT_Instance* fs, ExfatStream* st);
bool exfat_mkdir(EXFAT_Instance* fs, const char* path, bool* out_existed = nullptr);
bool exfat_cd(EXFAT_Instance* fs, const char* path);
bool exfat_delete_file(EXFAT_Instance* fs, const char* path);
bool exfat_rename_file(EXFAT_Instance* fs, const char* old_path, const char* new_path);

// Главный VFS-резолвер путей — как и в FAT32, но без чтения ".."/".": у exFAT
// таких записей не существует, родитель отслеживается собственным стеком по
// мере спуска по пути (см. реализацию).
uint32_t exfat_resolve_parent(EXFAT_Instance* fs, const char* full_path, char* out_basename);

// Ищет запись с именем target_name внутри dir_cluster, возвращает её первый
// кластер (или 0xFFFFFFFF, если не найдена).
// out_is_dir (может быть nullptr) — issuse.txt №36: раньше не было
// способа отличить "нашли каталог" от "нашли обычный файл" по
// возвращаемому cluster'у — ls на файле обходил его содержимое как
// таблицу каталога.
uint32_t exfat_find_in_dir(EXFAT_Instance* fs, uint32_t dir_cluster, const char* target_name, bool* out_is_dir = nullptr);
