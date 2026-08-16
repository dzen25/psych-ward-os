#include "h/sys_client.h"

// issuse.txt №63(a) — читает файл целиком чанками SYS_READ_FILE=119 (тот же
// проверенный протокол, что теперь использует cat.cpp), НЕ печатая
// содержимое — только замеряет время. Нужен для проверки: укладывается ли
// реальное чтение большого файла с SD (blk_driver) в 3-секундный бюджет
// heartbeat-watchdog'а (Фаза 3b плана "Сигналы драйверам", см. issuse.txt
// и ROADMAP.md) — если нет, watchdog ложно убьёт blk_driver прямо
// посреди чтения. Раньше единственный доступный большой файл лежал на
// USB (не через blk_driver вообще), тест не проводился.
//
// НЕ пытается копировать файл на SD сам — exfat_write_file() (exfat.cpp)
// пишет ТОЛЬКО целиком за один вызов (нет append/write-at-offset пути в
// коде вообще, только полная перезапись), так что "стриминговое копирование
// на SD" потребовало бы новой фичи в самом exFAT-writer'е — риск того же
// класса, что уже не раз ловили на этом фронте (issuse.txt история). Файл
// на SD нужно положить заранее (с Mac при подготовке SD-карты) — читать
// это устройство умеет отлично, писать большими кусками — нет.
static void putdec(int val) {
    char buf[12]; int j = 0;
    if (val == 0) buf[j++] = '0';
    while (val > 0) { buf[j++] = '0' + (val % 10); val /= 10; }
    while (j > 0) { char c[2] = {buf[--j], 0}; sys_puts(0, c); }
}

static seL4_Word get_time_ms(seL4_CPtr timer_ep) {
    seL4_SetMR(0, 3); // SYS_GET_TIME
    seL4_Call(timer_ep, seL4_MessageInfo_new(0, 0, 0, 1));
    return seL4_GetMR(0);
}

int main(int argc, char *argv[]) {
    SysClientEnv env;
    sys_client_init(env);

    if (!env.arg) { sys_puts(0, "Usage: timedread <file>\n"); sys_exit(env.root_ep); return 1; }

    char *shm = env.shm;
    build_absolute_path(shm, env.arg, SHM_TOTAL_SIZE);
    seL4_CPtr target_ep = route_vfs_path(shm, env.blk_ep, env.usb_storage_ep);

    char path[256];
    my_strlcpy(path, shm, sizeof(path));

    seL4_Word t_start = get_time_ms(env.timer_ep);
    sys_puts(0, "timedread: старт, t="); putdec((int)t_start); sys_puts(0, "мс\n");

    vfs_lock();
    uint32_t offset = 0;
    bool file_found = false;
    bool read_error_mid = false;
    int chunk_num = 0;

    while (1) {
        my_strlcpy(shm, path, SHM_TOTAL_SIZE);
        seL4_SetMR(0, 119); // SYS_READ_FILE
        seL4_SetMR(1, offset);
        seL4_Call(target_ep, seL4_MessageInfo_new(0, 0, 0, 2));
        int status = (int)seL4_GetMR(0);
        if (status != 0) {
            if (file_found) read_error_mid = true; // была часть файла, а потом вдруг ошибка — интересно само по себе
            break;
        }
        int bytes_read = (int)seL4_GetMR(1);
        file_found = true;
        if (bytes_read == 0) break; // EOF
        offset += (uint32_t)bytes_read;
        chunk_num++;

        // ГОРАЗДО более частый прогресс, чем раньше (было раз в 4МиБ, ~1000
        // чанков — недостаточно разрешения, чтобы отличить "медленно ползёт"
        // от "встало намертво"). Первый чанк — всегда, дальше — каждые 32
        // (128КБ) с текущим временем на каждой строке, чтобы сразу была
        // видна скорость (или её отсутствие).
        if (chunk_num == 1 || (chunk_num % 32) == 0) {
            seL4_Word now = get_time_ms(env.timer_ep);
            sys_puts(0, "timedread: чанк "); putdec(chunk_num);
            sys_puts(0, " offset="); putdec((int)offset);
            sys_puts(0, " t="); putdec((int)(now - t_start)); sys_puts(0, "мс\n");
        }
    }
    vfs_unlock();

    seL4_Word t_end = get_time_ms(env.timer_ep);

    if (!file_found) {
        sys_puts(0, "timedread: файл не найден или это каталог\n");
    } else {
        sys_puts(0, "timedread: готово. байт="); putdec((int)offset);
        sys_puts(0, " время="); putdec((int)(t_end - t_start)); sys_puts(0, "мс");
        if (read_error_mid) sys_puts(0, " (ОШИБКА на середине чтения!)");
        sys_puts(0, "\n");
    }

    sys_exit(env.root_ep);
    return 0;
}
