#include "h/sys_client.h"

// Фаза A (см. ROADMAP.md): вынесено из shell.cpp — SYS_MKDIR=117 в
// blk_driver.cpp. Раньше здесь НЕ было build_absolute_path — сырой
// аргумент уходил как есть, полагаясь на серверное относительное
// разрешение внутри blk_driver.cpp. Milestone 9 (Фаза 14, закрытие) это
// меняет: маршрутизация /mnt/usb0 требует знать ПОЛНЫЙ абсолютный путь
// НА КЛИЕНТЕ (см. route_vfs_path()/h/sys_client.h), иначе нельзя понять,
// какой бэкенд обслуживает относительный путь. exfat_normalize_path()
// одинаково принимает и абсолютные, и относительные пути, так что для
// SD-карты (blk_driver) поведение не меняется.
int main(int argc, char *argv[]) {
    SysClientEnv env;
    sys_client_init(env);

    char* p = env.arg;
    if (!p) {
        sys_puts(0, "mkdir: missing operand\n");
        sys_exit(env.root_ep);
        return 1;
    }

    // issuse.txt №50: next_token() (h/sys_client.h) вместо ручного разбора
    // по пробелам — понимает "имя в кавычках" как один аргумент.
    char* start_of_arg;
    while ((start_of_arg = next_token(&p)) != nullptr) {
        build_absolute_path(env.shm, start_of_arg, SHM_TOTAL_SIZE);
        seL4_CPtr target_ep = route_vfs_path(env.shm, env.blk_ep, env.usb_storage_ep);
        vfs_lock();
        seL4_SetMR(0, 117); // SYS_MKDIR
        seL4_Call(target_ep, seL4_MessageInfo_new(0, 0, 0, 1));
        int ret = seL4_GetMR(0);
        vfs_unlock();

        if (ret == 1) {
            sys_puts(0, "mkdir: '");
            sys_puts(0, start_of_arg);
            sys_puts(0, "' уже существует — ничего не делаю\n");
        } else if (ret != 0) {
            sys_puts(0, "mkdir: cannot create directory '");
            sys_puts(0, start_of_arg);
            sys_puts(0, "'\n");
        }
    }
    sys_exit(env.root_ep);
    return 0;
}
