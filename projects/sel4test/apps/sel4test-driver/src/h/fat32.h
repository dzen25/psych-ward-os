#pragma once
#include <stdint.h>
#include <stdbool.h>

// Сигнатура функции чтения сырых секторов.
// Возвращает true при успехе.
typedef bool (*block_read_fn)(uint32_t sector, uint32_t count, void* buffer);
typedef bool (*block_write_fn)(uint32_t sector, uint32_t count, const void* buffer); // НОВОЕ

// Состояние нашей файловой системы
struct FAT32_Instance {
    block_read_fn read_blocks; // Указатель на аппаратную функцию VirtIO
    block_write_fn write_blocks; // НОВОЕ
    
    // Кэшированные метаданные FAT32
    uint16_t bytes_per_sector;
    uint8_t  sectors_per_cluster;
    uint16_t reserved_sectors;
    uint8_t  fat_count;
    uint32_t sectors_per_fat;
    uint32_t root_cluster;
    uint32_t current_dir_cluster; // <--- НОВОЕ: Текущая папка (CWD)
    uint32_t data_start_sector;
};

// --- API Файловой Системы ---

// Инициализация (читает Boot Sector и настраивает параметры)
bool fat32_init(FAT32_Instance* fs, block_read_fn read_func, block_write_fn write_func);

// Вывод содержимого директории в строковый буфер (out_buffer)
bool fat32_list_directory(FAT32_Instance* fs, const char* path, char* out_buffer);

// Форматирует список произвольного каталога (по номеру кластера) в out_buffer,
// обходя ВСЮ цепочку его кластеров. max_len — полный размер out_buffer.
bool fat32_format_dir_listing(FAT32_Instance* fs, uint32_t dir_cluster, char* out_buffer, uint32_t max_len);

// Чтение файла с диска с заданным смещением
bool fat32_read_file(FAT32_Instance* fs, const char* filename, char* out_buffer, uint32_t offset, uint32_t* bytes_read);

bool fat32_read_text_file(FAT32_Instance* fs, const char* path, char* out_buffer);
bool fat32_create_file(FAT32_Instance* fs, const char* path);
bool fat32_write_file(FAT32_Instance* fs, const char* path, const char* text, uint32_t len);
bool fat32_mkdir(FAT32_Instance* fs, const char* path);
bool fat32_cd(FAT32_Instance* fs, const char* path);
bool fat32_delete_file(FAT32_Instance* fs, const char* path);
bool fat32_rename_file(FAT32_Instance* fs, const char* old_path, const char* new_path);

// Главный VFS Резолвер Путей
uint32_t fat32_resolve_parent(FAT32_Instance* fs, const char* full_path, char* out_basename);

// (Задел на будущее) Поиск начального кластера файла
uint32_t fat32_find_file(FAT32_Instance* fs, const char* filename);