#include "h/sys_client.h"

// Фаза A (см. ROADMAP.md): вынесено из shell.cpp — SYS_READ_TEXT_FILE=114 в blk_driver.cpp.
int main(int argc, char *argv[]) {
    SysClientEnv env;
    sys_client_init(env);

    if (!env.arg) { sys_puts(0, "Usage: cat <file>\n"); sys_exit(env.root_ep); return 1; }

    char *shm = env.shm;
    build_absolute_path(shm, env.arg, SHM_TOTAL_SIZE);
    seL4_CPtr target_ep = route_vfs_path(shm, env.blk_ep, env.usb_storage_ep); // Milestone 9

    if (vfs_syscall(114, target_ep) == 0) { // Файл прочитан в shm
        // issuse.txt №56: shm — это '\0'-терминированная C-строка (весь
        // стек вывода — sys_puts/sys_write/grep — завязан на strlen()), но
        // реальный файл мог содержать нулевые байты РАНЬШЕ своего конца
        // (бинарник). seL4_GetMR(1) — сколько байт реально скопировал
        // сервер (см. blk_driver.cpp/usb_driver.cpp, cmd 114); если это
        // больше видимой строки — где-то внутри был '\0', вывод обрезан.
        seL4_Word copied = seL4_GetMR(1);
        seL4_Word shown = (seL4_Word)my_strlen(shm);
        sys_puts(0, shm);
        sys_puts(0, "\n");
        if (copied > shown) {
            sys_puts(0, "cat: файл содержит нулевые байты (похоже на бинарный) — вывод оборван на первом из них.\n");
        }
    } else {
        sys_puts(0, "File not found or is a directory.\n");
    }
    sys_exit(env.root_ep);
    return 0;
}
