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
};

// --- API файловой системы (сигнатуры зеркалируют fat32.h нарочно —
// blk_driver.cpp переключается заменой имён вызовов, не переписыванием) ---

bool exfat_init(EXFAT_Instance* fs, block_read_fn read_func, block_write_fn write_func);

bool exfat_format_dir_listing(EXFAT_Instance* fs, uint32_t dir_cluster, char* out_buffer, uint32_t max_len);

bool exfat_read_file(EXFAT_Instance* fs, const char* filename, char* out_buffer, uint32_t offset, uint32_t* bytes_read);
bool exfat_read_text_file(EXFAT_Instance* fs, const char* path, char* out_buffer);

// out_existed (может быть nullptr) — при true уже существовал, ничего не
// создавалось (в отличие от возврата false = реальная ошибка).
bool exfat_create_file(EXFAT_Instance* fs, const char* path, bool* out_existed = nullptr);
bool exfat_write_file(EXFAT_Instance* fs, const char* path, const char* text, uint32_t len);
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
uint32_t exfat_find_in_dir(EXFAT_Instance* fs, uint32_t dir_cluster, const char* target_name);
