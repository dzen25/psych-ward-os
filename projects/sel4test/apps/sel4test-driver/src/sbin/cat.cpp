#include "h/sys_client.h"

// issuse.txt №63(b) — раньше читал файл ОДНИМ заходом через
// SYS_READ_TEXT_FILE=114, жёстко обрезанным до 4000 байт в
// exfat_read_text_file() (exfat.cpp) — реальный потолок страницы SHM всё
// равно 4096 (SHM_PAGE_VFS в main.cpp — один физический фрейм, не 16КБ,
// как можно подумать по SHM_TOTAL_SIZE). Теперь читаем чанками через
// SYS_READ_FILE=119 (offset+chunk, тот же протокол, которым root грузит
// КАЖДЫЙ ELF на диске — read_file_raw_from_disk() в main.cpp, уже
// проверенный путь, не новый) в цикле до реального EOF — файл любого
// размера читается и печатается полностью, без молчаливой обрезки.
//
// issuse.txt №63(c) — вместо strlen()-завязанного sys_puts() печатаем
// каждый чанк побайтово известной длиной (sys_write_n(), см.
// h/sys_client.h) и сканируем на небезопасные для терминала байты — не
// printable ASCII и не \n/\t/\r. Это включает нулевые байты (issuse.txt
// №56 — раньше отдельная проверка specifically под них) и ESC/ANSI-
// последовательности (живая находка в issuse.txt №62 — порча памяти
// однажды вывела "мусор", похожий на ANSI, и стёрла экран пользователю)
// одним и тем же механизмом. Первый же небезопасный байт — печатаем всё
// ДО него и честно останавливаемся с предупреждением, вместо того чтобы
// тащить сырые control-байты в терминал.
static bool is_safe_terminal_byte(unsigned char c) {
    if (c >= 0x20 && c <= 0x7E) return true; // печатаемый ASCII
    return c == '\n' || c == '\t' || c == '\r';
}

int main(int argc, char *argv[]) {
    SysClientEnv env;
    sys_client_init(env);

    if (!env.arg) { sys_puts(0, "Usage: cat <file>\n"); sys_exit(env.root_ep); return 1; }

    char *shm = env.shm;
    build_absolute_path(shm, env.arg, SHM_TOTAL_SIZE);
    seL4_CPtr target_ep = route_vfs_path(shm, env.blk_ep, env.usb_storage_ep); // Milestone 9, мутирует shm на месте для /mnt

    char path[256];
    my_strlcpy(path, shm, sizeof(path)); // shm перезапишется содержимым файла на первом же чанке

    vfs_lock(); // держим на весь цикл — иначе чужой ls/touch между чанками испортит SHM
    uint32_t offset = 0;
    bool file_found = false;
    bool stopped_unsafe = false;

    while (1) {
        my_strlcpy(shm, path, SHM_TOTAL_SIZE); // драйвер перезаписывает shm содержимым — путь восстанавливаем перед каждым запросом
        seL4_SetMR(0, 119); // SYS_READ_FILE
        seL4_SetMR(1, offset);
        seL4_Call(target_ep, seL4_MessageInfo_new(0, 0, 0, 2));
        int status = (int)seL4_GetMR(0);
        if (status != 0) break; // file_found остаётся как было — если это первый заход, файла нет

        int bytes_read = (int)seL4_GetMR(1);
        file_found = true;
        if (bytes_read == 0) break; // EOF

        int safe_len = 0;
        while (safe_len < bytes_read && is_safe_terminal_byte((unsigned char)shm[safe_len])) safe_len++;

        if (safe_len > 0) sys_write_n(1, shm, safe_len);

        if (safe_len < bytes_read) {
            stopped_unsafe = true;
            break;
        }
        offset += (uint32_t)bytes_read;
    }
    vfs_unlock();

    if (!file_found) {
        sys_puts(0, "File not found or is a directory.\n");
    } else {
        sys_puts(0, "\n");
        if (stopped_unsafe) {
            sys_puts(0, "cat: файл содержит небезопасные для терминала байты (нулевые/control/ANSI) — вывод оборван на первом из них.\n");
        }
    }

    sys_exit(env.root_ep);
    return 0;
}
