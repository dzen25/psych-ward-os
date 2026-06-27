#include "fat32.h"

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
    
    // Сохраняем метаданные
    fs->bytes_per_sector = bpb->bytes_per_sector == 0 ? 512 : bpb->bytes_per_sector;
    fs->sectors_per_cluster = bpb->sectors_per_cluster == 0 ? 8 : bpb->sectors_per_cluster;
    fs->reserved_sectors = bpb->reserved_sectors;
    fs->fat_count = bpb->fat_count;
    fs->sectors_per_fat = bpb->sectors_per_fat_32;
    fs->root_cluster = bpb->root_cluster;

    // Вычисляем, где начинаются данные
    uint32_t fat_size = fs->fat_count * fs->sectors_per_fat;
    fs->data_start_sector = fs->reserved_sectors + fat_size;

    fs->current_dir_cluster = fs->root_cluster;

    return true;
}

// Возвращает сектор на диске для текущей открытой директории
static uint32_t get_cwd_sector(FAT32_Instance* fs) {
    uint32_t clus = fs->current_dir_cluster;
    if (clus == 0) clus = fs->root_cluster; // В FAT32 "0" часто означает Root
    return fs->data_start_sector + (clus - 2) * fs->sectors_per_cluster;
}

bool fat32_list_directory(FAT32_Instance* fs, const char* path, char* out_buffer) {
    char sector_buf[512];
    
    // Пока что игнорируем path и читаем только Root Directory
    // Вычисляем физический сектор корневой папки
    uint32_t root_sector = get_cwd_sector(fs); // Теперь они работают в текущей папке!
    
    if (!fs->read_blocks(root_sector, 1, sector_buf)) {
        my_strcpy(out_buffer, "Error: Failed to read disk\n");
        return false;
    }

    my_strcpy(out_buffer, "Directory listing:\n");
    
    // Парсинг 32-байтных записей (Directory Entries)
    for (int i = 0; i < 512; i += 32) {
        uint8_t* entry = (uint8_t*)&sector_buf[i];
        if (entry[0] == 0x00) break;       // Конец списка файлов
        if (entry[0] == 0xE5) continue;    // Удаленный файл
        if (entry[11] & 0x0F) continue;    // LFN (длинное имя) или системный атрибут
        
        // Извлекаем имя формата 8.3
        char name[13];
        int pos = 0;
        for (int j = 0; j < 8; j++) { if (entry[j] != ' ') name[pos++] = entry[j]; }
        if (entry[8] != ' ') {
            name[pos++] = '.';
            for (int j = 8; j < 11; j++) { if (entry[j] != ' ') name[pos++] = entry[j]; }
        }
        name[pos] = '\0';

        if (pos > 0) {
            if (entry[11] & 0x10) { // Атрибут: Директория
                my_strcat(out_buffer, " [DIR] ");
            } else {                // Атрибут: Файл
                my_strcat(out_buffer, " [FAT] ");
            }
            my_strcat(out_buffer, name);
            my_strcat(out_buffer, "\n");
        }
    }
    return true;
}

bool fat32_read_file(FAT32_Instance* fs, const char* filename, char* out_buffer, uint32_t offset, uint32_t* bytes_read) {
    *bytes_read = 0;
    char basename[64];
    uint32_t parent_clus = fat32_resolve_parent(fs, filename, basename);
    if (parent_clus == 0xFFFFFFFF || basename[0] == '\0') return false;

    if (parent_clus == 0) parent_clus = fs->root_cluster;
    uint32_t parent_sector = fs->data_start_sector + (parent_clus - 2) * fs->sectors_per_cluster;
    
    char sector_buf[512];
    if (!fs->read_blocks(parent_sector, 1, sector_buf)) return false;

    char formatted_name[12];
    format_fat32_name(basename, formatted_name);
    char lfn_buf[256] = {0};
    uint32_t start_cluster = 0;
    uint32_t file_size = 0;
    bool found = false;

    for (int i = 0; i < 512; i += 32) {
        uint8_t* entry = (uint8_t*)&sector_buf[i];
        if (entry[0] == 0x00) break;
        if (entry[0] == 0xE5) { for(int k=0; k<256; k++) lfn_buf[k] = 0; continue; }

        if (entry[11] == 0x0F) {
            int seq = (entry[0] & 0x1F) - 1;
            if (seq >= 0 && seq < 20) {
                char* p = lfn_buf + (seq * 13);
                for(int k=1;  k<11; k+=2) { if(entry[k] != 0xFF && entry[k] != 0) *p++ = entry[k]; }
                for(int k=14; k<26; k+=2) { if(entry[k] != 0xFF && entry[k] != 0) *p++ = entry[k]; }
                for(int k=28; k<32; k+=2) { if(entry[k] != 0xFF && entry[k] != 0) *p++ = entry[k]; }
            }
            continue;
        }

        if (entry[11] & 0x08) { for(int k=0; k<256; k++) lfn_buf[k] = 0; continue; }

        bool match = false;
        if (lfn_buf[0] != '\0') {
            if (my_strcasecmp(lfn_buf, basename) == 0) match = true;
            for(int k=0; k<256; k++) lfn_buf[k] = 0;
        } else {
            char entry_name[12];
            my_memcpy(entry_name, entry, 11); entry_name[11] = '\0';
            if (my_strcmp(entry_name, formatted_name) == 0) match = true;
        }

        if (match) {
            uint16_t clus_hi = *(uint16_t*)&entry[20];
            uint16_t clus_lo = *(uint16_t*)&entry[26];
            start_cluster = ((uint32_t)clus_hi << 16) | clus_lo;
            file_size = *(uint32_t*)&entry[28];
            found = true;
            break;
        }
    }

    if (!found) return false;

    if (offset >= file_size) return true; // Достигнут конец файла (EOF)

    uint32_t remaining = file_size - offset;
    // Ограничиваем чтение размером страницы разделяемой памяти (SHM = 4096)
    uint32_t chunk_size = (remaining > 4096) ? 4096 : remaining; 
    
    // Внимание: для простоты предполагаем, что кластеры идут подряд (Contiguous).
    // Для запуска небольших test.elf этого хватит с головой.
    uint32_t base_sector = fs->data_start_sector + (start_cluster - 2) * fs->sectors_per_cluster;
    uint32_t target_sector = base_sector + (offset / fs->bytes_per_sector);
    
    // Вычисляем, сколько 512-байтных секторов надо вычитать
    uint32_t sectors_to_read = (chunk_size + fs->bytes_per_sector - 1) / fs->bytes_per_sector;
    if (sectors_to_read * 512 > 4096) sectors_to_read = 8; // Жесткий лимит DMA
    
    // Читаем сырые данные с QEMU-диска прямиком в Shared Memory буфер (out_buffer)
    if (!fs->read_blocks(target_sector, sectors_to_read, out_buffer)) {
        return false;
    }

    *bytes_read = chunk_size;
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
static uint32_t fat32_find_in_dir(FAT32_Instance* fs, uint32_t dir_cluster, const char* target_name) {
    // === ФИКС ДЛЯ "cd .." ИЗ КОРНЯ ===
    if (my_strcmp(target_name, "..") == 0 && (dir_cluster == fs->root_cluster || dir_cluster == 0)) {
        return fs->root_cluster; // Безопасно возвращаем корень, предотвращая ошибку
    }

    char sector_buf[512];
    uint32_t clus = (dir_cluster == 0) ? fs->root_cluster : dir_cluster;
    uint32_t sector = fs->data_start_sector + (clus - 2) * fs->sectors_per_cluster;
    
    if (!fs->read_blocks(sector, 1, sector_buf)) return 0xFFFFFFFF;

    char formatted_name[12];
    format_fat32_name(target_name, formatted_name); // Готовим 8.3 на случай фоллбека

    char lfn_buf[256];
    for(int i = 0; i < 256; i++) lfn_buf[i] = 0;

    for (int i = 0; i < 512; i += 32) {
        uint8_t* entry = (uint8_t*)&sector_buf[i];
        if (entry[0] == 0x00) break;
        if (entry[0] == 0xE5) {
            // LFN-записи, относящиеся к удаленному файлу, тоже недействительны. Сбрасываем буфер.
            for(int k=0; k<256; k++) lfn_buf[k] = 0;
            continue;
        }
 
        // 1. ПАРСИНГ LFN
        if (entry[11] == 0x0F) { 
            int seq = (entry[0] & 0x1F) - 1;
            if (seq >= 0 && seq < 20) {
                char* p = lfn_buf + (seq * 13);
                for(int k=1;  k<11; k+=2) { if(entry[k] != 0xFF && entry[k] != 0) *p++ = entry[k]; }
                for(int k=14; k<26; k+=2) { if(entry[k] != 0xFF && entry[k] != 0) *p++ = entry[k]; }
                for(int k=28; k<32; k+=2) { if(entry[k] != 0xFF && entry[k] != 0) *p++ = entry[k]; }
            }
            continue; // 8.3 запись с кластером всегда идет ПОСЛЕ LFN кусков
        }

        if (entry[11] & 0x08) { // Пропускаем метку тома
            // Метка тома не должна иметь LFN, но на всякий случай сбрасываем буфер.
            for(int k=0; k<256; k++) lfn_buf[k] = 0;
            continue;
        }

        // 2. ПОПЫТКА СОВПАДЕНИЯ ПО LFN
        if (lfn_buf[0] != '\0') {
            if (my_strcasecmp(lfn_buf, target_name) == 0) {
                // Если имя совпало, берем номер кластера из 8.3 записи!
                uint16_t clus_hi = *(uint16_t*)&entry[20];
                uint16_t clus_lo = *(uint16_t*)&entry[26];
                return ((uint32_t)clus_hi << 16) | clus_lo;
            }
            for(int k=0; k<256; k++) lfn_buf[k] = 0; // Не совпало - очищаем буфер
        } else {
            // 3. ПОПЫТКА СОВПАДЕНИЯ ПО 8.3 (Если LFN не было)
            char entry_name[12];
            my_memcpy(entry_name, entry, 11);
            entry_name[11] = '\0';
            if (my_strcmp(entry_name, formatted_name) == 0) {
                uint16_t clus_hi = *(uint16_t*)&entry[20];
                uint16_t clus_lo = *(uint16_t*)&entry[26];
                return ((uint32_t)clus_hi << 16) | clus_lo;
            }
        }
    }
    return 0xFFFFFFFF; // Файл не найден
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
    uint32_t parent_sector = fs->data_start_sector + (parent_clus - 2) * fs->sectors_per_cluster;

    char sector_buf[512];
    if (!fs->read_blocks(parent_sector, 1, sector_buf)) return false;

    char formatted_name[12];
    format_fat32_name(basename, formatted_name);
    char lfn_buf[256] = {0};

    for (int i = 0; i < 512; i += 32) {
        uint8_t* entry = (uint8_t*)&sector_buf[i];
        if (entry[0] == 0x00) break;
        if (entry[0] == 0xE5) { for(int k=0; k<256; k++) lfn_buf[k] = 0; continue; }

        if (entry[11] == 0x0F) {
            int seq = (entry[0] & 0x1F) - 1;
            if (seq >= 0 && seq < 20) {
                char* p = lfn_buf + (seq * 13);
                for(int k=1;  k<11; k+=2) { if(entry[k] != 0xFF && entry[k] != 0) *p++ = entry[k]; }
                for(int k=14; k<26; k+=2) { if(entry[k] != 0xFF && entry[k] != 0) *p++ = entry[k]; }
                for(int k=28; k<32; k+=2) { if(entry[k] != 0xFF && entry[k] != 0) *p++ = entry[k]; }
            }
            continue;
        }

        if (entry[11] & 0x08) { for(int k=0; k<256; k++) lfn_buf[k] = 0; continue; }

        bool match = false;
        if (lfn_buf[0] != '\0') {
            if (my_strcasecmp(lfn_buf, basename) == 0) match = true;
            for(int k=0; k<256; k++) lfn_buf[k] = 0;
        } else {
            char entry_name[12];
            my_memcpy(entry_name, entry, 11); entry_name[11] = '\0';
            if (my_strcmp(entry_name, formatted_name) == 0) match = true;
        }

        if (match) {
            uint16_t clus_hi = *(uint16_t*)&entry[20];
            uint16_t clus_lo = *(uint16_t*)&entry[26];
            uint32_t clus = (clus_hi << 16) | clus_lo;
            uint32_t size = *(uint32_t*)&entry[28];

            if (size == 0 || clus == 0) { out_buffer[0] = '\0'; return true; }
            if (size > 4000) size = 4000; // Безопасный лимит для текста

            uint32_t file_sector = fs->data_start_sector + (clus - 2) * fs->sectors_per_cluster;
            uint32_t sectors = (size + 511) / 512;
            if (!fs->read_blocks(file_sector, sectors, out_buffer)) return false;
            
            out_buffer[size] = '\0'; // Закрываем строку нулем для безопасного вывода
            return true;
        }
    }
    return false;
}

// === СОЗДАНИЕ ПУСТОГО ФАЙЛА (touch) ===
bool fat32_create_file(FAT32_Instance* fs, const char* path) {
    char basename[64];
    uint32_t parent_clus = fat32_resolve_parent(fs, path, basename);
    if (parent_clus == 0xFFFFFFFF || basename[0] == '\0') return false; // Неверный путь

    if (parent_clus == 0) parent_clus = fs->root_cluster;
    uint32_t target_sector = fs->data_start_sector + (parent_clus - 2) * fs->sectors_per_cluster;
    
    char sector_buf[512];
    if (!fs->read_blocks(target_sector, 1, sector_buf)) return false;

    char formatted_name[12];
    format_fat32_name(basename, formatted_name);
    char lfn_buf[256] = {0};
    int free_idx = -1;

    for (int i = 0; i < 512; i += 32) {
        uint8_t* entry = (uint8_t*)&sector_buf[i];

        if (entry[0] == 0x00 || entry[0] == 0xE5) {
            if (free_idx == -1) free_idx = i;
            if (entry[0] == 0x00) break; 
            if (entry[0] == 0xE5) for(int k=0; k<256; k++) lfn_buf[k] = 0;
            continue;
        }

        if (entry[11] == 0x0F) {
            int seq = (entry[0] & 0x1F) - 1;
            if (seq >= 0 && seq < 20) {
                char* p = lfn_buf + (seq * 13);
                for(int k=1;  k<11; k+=2) { if(entry[k] != 0xFF && entry[k] != 0) *p++ = entry[k]; }
                for(int k=14; k<26; k+=2) { if(entry[k] != 0xFF && entry[k] != 0) *p++ = entry[k]; }
                for(int k=28; k<32; k+=2) { if(entry[k] != 0xFF && entry[k] != 0) *p++ = entry[k]; }
            }
            continue;
        }

        if (entry[11] & 0x08) { for(int k=0; k<256; k++) lfn_buf[k] = 0; continue; }

        if (lfn_buf[0] != '\0') {
            if (my_strcasecmp(lfn_buf, basename) == 0) return true; // Уже существует
            for(int k=0; k<256; k++) lfn_buf[k] = 0;
        } else {
            char entry_name[12];
            my_memcpy(entry_name, entry, 11); entry_name[11] = '\0';
            if (my_strcmp(entry_name, formatted_name) == 0) return true; // Уже существует
        }
    }

    if (free_idx == -1) return false;

    uint8_t* new_entry = (uint8_t*)&sector_buf[free_idx];
    for(int i = 0; i < 32; i++) new_entry[i] = 0;
    char target_name[12];
    format_fat32_name(basename, target_name);
    my_memcpy(new_entry, target_name, 11);
    new_entry[11] = 0x20; // Атрибут Archive
    return fs->write_blocks(target_sector, 1, sector_buf);
}

// === ЗАПИСЬ В ФАЙЛ (echo > file) ===
bool fat32_write_file(FAT32_Instance* fs, const char* path, const char* text, uint32_t len) {
    if (!fat32_create_file(fs, path)) return false; 

    char basename[64];
    uint32_t parent_clus = fat32_resolve_parent(fs, path, basename);
    if (parent_clus == 0xFFFFFFFF || basename[0] == '\0') return false;

    if (parent_clus == 0) parent_clus = fs->root_cluster;
    uint32_t parent_sector = fs->data_start_sector + (parent_clus - 2) * fs->sectors_per_cluster;

    char sector_buf[512];
    if (!fs->read_blocks(parent_sector, 1, sector_buf)) return false;

    char formatted_name[12];
    format_fat32_name(basename, formatted_name);
    char lfn_buf[256] = {0};

    for (int i = 0; i < 512; i += 32) {
        uint8_t* entry = (uint8_t*)&sector_buf[i];
        if (entry[0] == 0x00) break;
        if (entry[0] == 0xE5) { for(int k=0; k<256; k++) lfn_buf[k] = 0; continue; }

        if (entry[11] == 0x0F) {
            int seq = (entry[0] & 0x1F) - 1;
            if (seq >= 0 && seq < 20) {
                char* p = lfn_buf + (seq * 13);
                for(int k=1;  k<11; k+=2) { if(entry[k] != 0xFF && entry[k] != 0) *p++ = entry[k]; }
                for(int k=14; k<26; k+=2) { if(entry[k] != 0xFF && entry[k] != 0) *p++ = entry[k]; }
                for(int k=28; k<32; k+=2) { if(entry[k] != 0xFF && entry[k] != 0) *p++ = entry[k]; }
            }
            continue;
        }

        if (entry[11] & 0x08) { for(int k=0; k<256; k++) lfn_buf[k] = 0; continue; }

        bool match = false;
        if (lfn_buf[0] != '\0') {
            if (my_strcasecmp(lfn_buf, basename) == 0) match = true;
            for(int k=0; k<256; k++) lfn_buf[k] = 0;
        } else {
            char entry_name[12];
            my_memcpy(entry_name, entry, 11); entry_name[11] = '\0';
            if (my_strcmp(entry_name, formatted_name) == 0) match = true;
        }

        if (match) {
            uint16_t clus_hi = *(uint16_t*)&entry[20];
            uint16_t clus_lo = *(uint16_t*)&entry[26];
            uint32_t clus = (clus_hi << 16) | clus_lo;

            // Если файл новый, выделяем ему кластер из таблицы FAT
            if (clus == 0) {
                char fat_buf[512];
                if (!fs->read_blocks(fs->reserved_sectors, 1, fat_buf)) return false;
                uint32_t* fat_table = (uint32_t*)fat_buf;

                for (uint32_t c = 3; c < 128; c++) {
                    if ((fat_table[c] & 0x0FFFFFFF) == 0) { // Нашли свободный кластер
                        clus = c;
                        fat_table[c] = 0x0FFFFFFF; // Маркер конца (EOF)
                        fs->write_blocks(fs->reserved_sectors, 1, fat_buf);
                        break;
                    }
                }
                if (clus == 0) return false; // Диск заполнен
                
                *(uint16_t*)&entry[20] = (clus >> 16);
                *(uint16_t*)&entry[26] = (clus & 0xFFFF);
            }

            *(uint32_t*)&entry[28] = len; // Обновляем размер файла
            fs->write_blocks(parent_sector, 1, sector_buf); // Сохраняем Директорию

            // Пишем сами данные
            uint32_t file_sector = fs->data_start_sector + (clus - 2) * fs->sectors_per_cluster;
            uint32_t sectors = (len + 511) / 512;
            if (sectors == 0) sectors = 1;
            return fs->write_blocks(file_sector, sectors, text);
        }
    }
    return false;
}

// === УДАЛЕНИЕ ФАЙЛА (rm) ===
bool fat32_delete_file(FAT32_Instance* fs, const char* path) {
    char basename[64];
    uint32_t parent_clus = fat32_resolve_parent(fs, path, basename);
    if (parent_clus == 0xFFFFFFFF || basename[0] == '\0') return false;

    if (parent_clus == 0) parent_clus = fs->root_cluster;
    uint32_t parent_sector = fs->data_start_sector + (parent_clus - 2) * fs->sectors_per_cluster;

    char sector_buf[512];
    if (!fs->read_blocks(parent_sector, 1, sector_buf)) return false;

    char formatted_name[12];
    format_fat32_name(basename, formatted_name);
    char lfn_buf[256] = {0};
    int lfn_start_idx = -1;

    for (int i = 0; i < 512; i += 32) {
        uint8_t* entry = (uint8_t*)&sector_buf[i];
        if (entry[0] == 0x00) break;
        if (entry[0] == 0xE5) { lfn_start_idx = -1; for(int k=0; k<256; k++) lfn_buf[k] = 0; continue; }

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

        if (entry[11] & 0x08) { lfn_start_idx = -1; for(int k=0; k<256; k++) lfn_buf[k] = 0; continue; }

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
            return fs->write_blocks(parent_sector, 1, sector_buf);
        }
    }
    return false;
}

// === ПЕРЕИМЕНОВАНИЕ (mv) ===
bool fat32_rename_file(FAT32_Instance* fs, const char* old_path, const char* new_path) {
    char old_basename[64];
    uint32_t old_parent_clus = fat32_resolve_parent(fs, old_path, old_basename);
    if (old_parent_clus == 0xFFFFFFFF || old_basename[0] == '\0') return false;

    char new_basename[64];
    uint32_t new_parent_clus = fat32_resolve_parent(fs, new_path, new_basename);
    if (new_parent_clus == 0xFFFFFFFF || new_basename[0] == '\0') return false;

    if (old_parent_clus != new_parent_clus) return false; // Поддерживаем переименование только в той же папке

    if (old_parent_clus == 0) old_parent_clus = fs->root_cluster;
    uint32_t parent_sector = fs->data_start_sector + (old_parent_clus - 2) * fs->sectors_per_cluster;

    char sector_buf[512];
    if (!fs->read_blocks(parent_sector, 1, sector_buf)) return false;

    char formatted_name[12];
    format_fat32_name(old_basename, formatted_name);
    char lfn_buf[256] = {0};

    for (int i = 0; i < 512; i += 32) {
        uint8_t* entry = (uint8_t*)&sector_buf[i];
        if (entry[0] == 0x00) break;
        if (entry[0] == 0xE5) { for(int k=0; k<256; k++) lfn_buf[k] = 0; continue; }

        if (entry[11] == 0x0F) {
            int seq = (entry[0] & 0x1F) - 1;
            if (seq >= 0 && seq < 20) {
                char* p = lfn_buf + (seq * 13);
                for(int k=1;  k<11; k+=2) { if(entry[k] != 0xFF && entry[k] != 0) *p++ = entry[k]; }
                for(int k=14; k<26; k+=2) { if(entry[k] != 0xFF && entry[k] != 0) *p++ = entry[k]; }
                for(int k=28; k<32; k+=2) { if(entry[k] != 0xFF && entry[k] != 0) *p++ = entry[k]; }
            }
            continue;
        }

        if (entry[11] & 0x08) { for(int k=0; k<256; k++) lfn_buf[k] = 0; continue; }

        bool match = false;
        if (lfn_buf[0] != '\0') {
            if (my_strcasecmp(lfn_buf, old_basename) == 0) match = true;
            for(int k=0; k<256; k++) lfn_buf[k] = 0;
        } else {
            char entry_name[12];
            my_memcpy(entry_name, entry, 11); entry_name[11] = '\0';
            if (my_strcmp(entry_name, formatted_name) == 0) match = true;
        }

        if (match) {
            char new_name_fmt[12];
            format_fat32_name(new_basename, new_name_fmt);
            my_memcpy(entry, new_name_fmt, 11);
            return fs->write_blocks(parent_sector, 1, sector_buf);
        }
    }
    return false;
}

// === СОЗДАНИЕ ДИРЕКТОРИИ (mkdir) ===
bool fat32_mkdir(FAT32_Instance* fs, const char* path) {
    char basename[64];
    uint32_t parent_clus = fat32_resolve_parent(fs, path, basename);
    if (parent_clus == 0xFFFFFFFF || basename[0] == '\0') return false;

    uint32_t original_parent_clus = parent_clus;
    if (parent_clus == 0) parent_clus = fs->root_cluster;
    uint32_t parent_sector = fs->data_start_sector + (parent_clus - 2) * fs->sectors_per_cluster;

    char sector_buf[512];
    if (!fs->read_blocks(parent_sector, 1, sector_buf)) return false;

    char formatted_name[12];
    format_fat32_name(basename, formatted_name);
    char lfn_buf[256] = {0};
    int free_idx = -1;

    // 1. Ищем свободное место в текущей папке
    for (int i = 0; i < 512; i += 32) {
        uint8_t* entry = (uint8_t*)&sector_buf[i];

        if (entry[0] == 0x00 || entry[0] == 0xE5) {
            if (free_idx == -1) free_idx = i;
            if (entry[0] == 0x00) break; 
            if (entry[0] == 0xE5) for(int k=0; k<256; k++) lfn_buf[k] = 0;
            continue;
        }

        if (entry[11] == 0x0F) {
            int seq = (entry[0] & 0x1F) - 1;
            if (seq >= 0 && seq < 20) {
                char* p = lfn_buf + (seq * 13);
                for(int k=1;  k<11; k+=2) { if(entry[k] != 0xFF && entry[k] != 0) *p++ = entry[k]; }
                for(int k=14; k<26; k+=2) { if(entry[k] != 0xFF && entry[k] != 0) *p++ = entry[k]; }
                for(int k=28; k<32; k+=2) { if(entry[k] != 0xFF && entry[k] != 0) *p++ = entry[k]; }
            }
            continue;
        }

        if (entry[11] & 0x08) { for(int k=0; k<256; k++) lfn_buf[k] = 0; continue; }

        if (lfn_buf[0] != '\0') {
            if (my_strcasecmp(lfn_buf, basename) == 0) return false; // Уже существует
            for(int k=0; k<256; k++) lfn_buf[k] = 0;
        } else {
            char entry_name[12];
            my_memcpy(entry_name, entry, 11); entry_name[11] = '\0';
            if (my_strcmp(entry_name, formatted_name) == 0) return false; // Уже существует
        }
    }

    if (free_idx == -1) return false; // Папка переполнена

    // 2. Аллоцируем новый кластер в таблице FAT
    char fat_buf[512];
    if (!fs->read_blocks(fs->reserved_sectors, 1, fat_buf)) return false;
    uint32_t* fat_table = (uint32_t*)fat_buf;
    uint32_t new_clus = 0;
    for (uint32_t c = 3; c < 128; c++) {
        if ((fat_table[c] & 0x0FFFFFFF) == 0) {
            new_clus = c;
            fat_table[c] = 0x0FFFFFFF; // Маркер EOF
            fs->write_blocks(fs->reserved_sectors, 1, fat_buf);
            break;
        }
    }
    if (new_clus == 0) return false;

    // 3. Создаем запись о папке в CWD (Текущей директории)
    uint8_t* new_entry = (uint8_t*)&sector_buf[free_idx];
    for(int i = 0; i < 32; i++) new_entry[i] = 0;
    char target_name[12];
    format_fat32_name(basename, target_name);
    my_memcpy(new_entry, target_name, 11);
    new_entry[11] = 0x10; // ATTR_DIRECTORY
    *(uint16_t*)&new_entry[20] = (new_clus >> 16);
    *(uint16_t*)&new_entry[26] = (new_clus & 0xFFFF);
    fs->write_blocks(parent_sector, 1, sector_buf);

    // 4. Инициализируем новую папку (записи "." и "..")
    char new_dir_buf[512];
    for(int i=0; i<512; i++) new_dir_buf[i] = 0;

    // Запись "."
    my_memcpy(&new_dir_buf[0], ".          ", 11);
    new_dir_buf[11] = 0x10;
    *(uint16_t*)&new_dir_buf[20] = (new_clus >> 16);
    *(uint16_t*)&new_dir_buf[26] = (new_clus & 0xFFFF);

    // Запись ".."
    my_memcpy(&new_dir_buf[32], "..         ", 11);
    new_dir_buf[43] = 0x10;
    *(uint16_t*)&new_dir_buf[52] = (original_parent_clus >> 16);
    *(uint16_t*)&new_dir_buf[58] = (original_parent_clus & 0xFFFF);

    uint32_t new_dir_sector = fs->data_start_sector + (new_clus - 2) * fs->sectors_per_cluster;
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