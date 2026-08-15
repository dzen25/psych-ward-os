#include "h/sys_client.h"

// Фаза A (см. ROADMAP.md): вынесено из shell.cpp — SYS_RENAME=116 в blk_driver.cpp.
int main(int argc, char *argv[]) {
    SysClientEnv env;
    sys_client_init(env);

    if (!env.arg) {
        sys_puts(0, "mv: missing file operand\n");
        sys_exit(env.root_ep);
        return 1;
    }
    char* p = env.arg;

    // issuse.txt №50/№53: next_token() (h/sys_client.h) вместо ручного
    // посимвольного разбора — понимает "имя в кавычках" как один
    // аргумент. issuse.txt №53: локальные копии на 128 байт (весь env.arg
    // целиком ограничен ~63 байтами на уровне boot-IPC, см. common.h/
    // main.cpp spawn_process — с большим запасом, без практической
    // обрезки). issuse.txt №48: третий токен — явная ошибка "too many
    // arguments", а не молчаливое отбрасывание.
    char* old_tok = next_token(&p);
    if (!old_tok) {
        sys_puts(0, "mv: missing file operand\n");
        sys_exit(env.root_ep);
        return 1;
    }
    char old_name[128];
    my_strlcpy(old_name, old_tok, sizeof(old_name));

    char* new_tok = next_token(&p);
    if (!new_tok) {
        sys_puts(0, "mv: missing destination file operand after '");
        sys_puts(0, old_name);
        sys_puts(0, "'\n");
        sys_exit(env.root_ep);
        return 1;
    }
    char new_name[128];
    my_strlcpy(new_name, new_tok, sizeof(new_name));

    if (next_token(&p) != nullptr) {
        sys_puts(0, "mv: too many arguments\n");
        sys_exit(env.root_ep);
        return 1;
    }

    char *shm = env.shm;
    build_absolute_path(shm, old_name, 128);
    build_absolute_path(shm + 128, new_name, SHM_TOTAL_SIZE - 128);

    // Milestone 9 (Фаза 14, закрытие) — маршрутизация /mnt/usb0. route_vfs_path()
    // переписывает буфер НА МЕСТЕ, поэтому сначала проверяем оба префикса на
    // ОТДЕЛЬНЫХ копиях (иначе срез префикса у shm испортил бы сравнение,
    // если бы старый путь был на USB, а новый — нет), только потом маршрутизируем
    // старый путь по-настоящему (SYS_RENAME принимает оба имени одним вызовом,
    // значит оба обязаны попасть на ОДИН и тот же бэкенд — иначе честная
    // cross-device ошибка, как у обычных ОС, вместо переименования "в никуда").
    char old_probe[128], new_probe[128];
    my_strlcpy(old_probe, shm, sizeof(old_probe));
    my_strlcpy(new_probe, shm + 128, sizeof(new_probe));
    seL4_CPtr old_ep = route_vfs_path(old_probe, env.blk_ep, env.usb_storage_ep);
    seL4_CPtr new_ep = route_vfs_path(new_probe, env.blk_ep, env.usb_storage_ep);
    bool cross_device = (old_ep != new_ep);
    // Milestone A3 (Фаза 15) — несколько USB-устройств сразу, route_vfs_path()
    // больше не решает, КАКОЕ именно (это теперь делает сервер по имени
    // тома в пути, см. usb_driver.cpp::resolve_device_by_path()) — если
    // ОБА пути ушли на usb_storage_ep, дополнительно сравниваем ведущие
    // компоненты (имена томов) сами, чтобы дать честную cross-device
    // ошибку СРАЗУ; сервер всё равно перепроверит authoritative на
    // SYS_RENAME (cmd==116) — эта проверка только для быстрого/понятного
    // сообщения, а не единственная линия защиты.
    if (!cross_device && old_ep == env.usb_storage_ep) {
        int oe = 1; while (old_probe[oe] != '\0' && old_probe[oe] != '/') oe++;
        int ne = 1; while (new_probe[ne] != '\0' && new_probe[ne] != '/') ne++;
        cross_device = (oe != ne);
        if (!cross_device) {
            for (int k = 1; k < oe; k++) if (old_probe[k] != new_probe[k]) { cross_device = true; break; }
        }
    }
    if (cross_device) {
        sys_puts(0, "mv: '");
        sys_puts(0, old_name);
        sys_puts(0, "' -> '");
        sys_puts(0, new_name);
        sys_puts(0, "': Invalid cross-device link\n");
        sys_exit(env.root_ep);
        return 1;
    }
    seL4_CPtr target_ep = route_vfs_path(shm, env.blk_ep, env.usb_storage_ep);
    route_vfs_path(shm + 128, env.blk_ep, env.usb_storage_ep);

    vfs_lock();
    seL4_SetMR(0, 116); // SYS_RENAME
    seL4_Call(target_ep, seL4_MessageInfo_new(0, 0, 0, 1));
    int ret_val = seL4_GetMR(0);
    vfs_unlock();

    if (ret_val != 0) {
        sys_puts(0, "mv: cannot stat '");
        sys_puts(0, old_name);
        sys_puts(0, "': No such file or directory\n");
    }
    sys_exit(env.root_ep);
    return 0;
}
