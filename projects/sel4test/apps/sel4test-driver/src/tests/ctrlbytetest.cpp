#include "h/sys_client.h"

// issuse.txt №63(c) — создаёт файл с ЗАВЕДОМО небезопасными для терминала
// байтами: нулевым байтом (issuse.txt №56) и сырой ANSI-последовательностью
// очистки экрана (ESC '[' '2' 'J') внутри иначе обычного текста. Обычный
// `echo > файл` не годится — его протокол (shell.cpp) считает длину через
// my_strlen(), а NUL-байт внутри строки её тут же обрывает. Здесь длина
// передаётся явно (MR1), в обход strlen() — тот же сырой протокол
// SYS_WRITE_FILE=113, что использует echo, просто с явным count.
//
// Проверка: после запуска — `cat /root/ctrl_test.txt`. Ожидается: печатает
// "before-null:" и останавливается с предупреждением ДО нулевого байта —
// экран НЕ должен очиститься (значит ESC-последовательность дальше по
// файлу даже не достигнута), в консоли — явное сообщение про небезопасные
// байты вместо тишины/мусора.
int main(int argc, char *argv[]) {
    SysClientEnv env;
    sys_client_init(env);

    const char *path = "/root/ctrl_test.txt";
    char *shm = env.shm;

    char *path_ptr = shm;
    char *data_ptr = shm + 128;

    my_strlcpy(path_ptr, path, 128);

    // Payload: "before-null:" + 0x00 + "AFTER-NULL-SHOULD-NOT-PRINT" +
    // ESC '[' '2' 'J' (очистка экрана) + "should-not-appear-either".
    int len = 0;
    const char *part1 = "before-null:";
    while (part1[len]) { data_ptr[len] = part1[len]; len++; }
    data_ptr[len++] = '\0'; // сам небезопасный байт №1
    const char *part2 = "AFTER-NULL-SHOULD-NOT-PRINT";
    for (int i = 0; part2[i]; i++) data_ptr[len++] = part2[i];
    data_ptr[len++] = 0x1B; // ESC — небезопасный байт №2 (если бы дошли до него)
    data_ptr[len++] = '[';
    data_ptr[len++] = '2';
    data_ptr[len++] = 'J';
    const char *part3 = "should-not-appear-either";
    for (int i = 0; part3[i]; i++) data_ptr[len++] = part3[i];

    seL4_CPtr target_ep = route_vfs_path(path_ptr, env.blk_ep, env.usb_storage_ep);

    vfs_lock();
    seL4_SetMR(0, 113); // SYS_WRITE_FILE
    seL4_SetMR(1, (seL4_Word)len);
    seL4_Call(target_ep, seL4_MessageInfo_new(0, 0, 0, 2));
    int status = (int)seL4_GetMR(0);
    vfs_unlock();

    if (status == 0) {
        sys_puts(0, "ctrlbytetest: записано /root/ctrl_test.txt (");
        char buf[12]; int j = 0; int v = len;
        if (v == 0) buf[j++] = '0';
        while (v > 0) { buf[j++] = '0' + (v % 10); v /= 10; }
        while (j > 0) { char c[2] = {buf[--j], 0}; sys_puts(0, c); }
        sys_puts(0, " байт) — теперь: cat /root/ctrl_test.txt\n");
    } else {
        sys_puts(0, "ctrlbytetest: запись не удалась\n");
    }

    sys_exit(env.root_ep);
    return 0;
}
