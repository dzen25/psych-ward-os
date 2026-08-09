#include "h/sys_client.h"

// Фаза A (см. ROADMAP.md): вынесено из shell.cpp — SYS_LS=110 в blk_driver.cpp.
int main(int argc, char *argv[]) {
    SysClientEnv env;
    sys_client_init(env);

    char *shm = env.shm;
    if (env.arg) build_absolute_path(shm, env.arg, SHM_TOTAL_SIZE);
    else build_absolute_path(shm, "", SHM_TOTAL_SIZE);

    // build_absolute_path("") на непустом cwd оставляет хвостовой "/"
    // (копирует cwd, дописывает "/", затем дописывает пустой arg) — снимаем
    // его, иначе сравнение с "/mnt" ниже никогда не совпадёт для "ls" без
    // аргумента (нашли на живом железе).
    {
        int shm_len = my_strlen(shm);
        if (shm_len > 1 && shm[shm_len - 1] == '/') shm[shm_len - 1] = '\0';
    }

    // Milestone 9/10/A3 (доп. фикс + запрос пользователя) — точки
    // монтирования USB (перехват по префиксу, см. route_vfs_path())
    // физически НЕ существуют как записи каталога на SD-карте, поэтому
    // обычный листинг "/mnt" их не увидит. Пользователь явно ожидает
    // видеть их в ls (как в обычных ОС), причём под ИМЕНЕМ КАЖДОГО
    // НАКОПИТЕЛЯ (Vendor-Product из SCSI INQUIRY) — для "/mnt" отдельно
    // запрашиваем у usb_driver'а СПИСОК всех смонтированных (Milestone A3
    // — раньше было только одно устройство, fetch_usb_volume_name()).
    bool is_mnt_root = (my_strcmp(shm, "/mnt") == 0);
    UsbVolumeList vols;
    bool have_usb_entries = false;
    if (is_mnt_root && env.usb_storage_ep != 0) {
        have_usb_entries = fetch_usb_volume_list(env.usb_storage_ep, vols);
    }

    // Milestone 9 (Фаза 14, закрытие) — маршрутизация /mnt/<имя тома>.
    seL4_CPtr target_ep = route_vfs_path(shm, env.blk_ep, env.usb_storage_ep);
    vfs_syscall(110, target_ep); // SYS_LS — пишет листинг прямо в shm
    sys_puts(0, shm);
    if (have_usb_entries) {
        for (int i = 0; i < USB_MAX_DEVICES; i++) {
            if (!vols.mounted[i]) continue;
            sys_puts(0, " [DIR] ");
            sys_puts(0, vols.name[i]);
            sys_puts(0, "\n");
        }
    }
    sys_exit(env.root_ep);
    return 0;
}
