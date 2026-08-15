#include "h/sys_client.h"

static void sys_putdec_local(uint32_t val) {
    char buf[12]; int i = 11; buf[i--] = '\0';
    if (val == 0) buf[i--] = '0';
    while (val > 0) { buf[i--] = '0' + (val % 10); val /= 10; }
    sys_puts(0, &buf[i + 1]);
}

// issuse.txt №4 (тестовый хук) — напрямую бьёт по SYS_PROXY_WRITE_FILE
// (136) с той же комбинацией path_len/data_len, что в репро самого пункта
// №4 ("60 и 60"): claims data_len=60, но реально в IPC-сообщение
// умещается (при MsgLength=120) только 57 байт данных ПОСЛЕ 60-байтного
// пути (3+60+57=120). До фикса сервер слепо читал seL4_GetMR() ещё для
// 3 "лишних" байт данных (индексы 120-122, msg[] кончается на 119) —
// это уже поля userData/caps_or_badges IPC-буфера, не данные файла.
// После фикса data_len молча урезается до 57 — в файл попадает ТОЛЬКО
// чистый префикс из настоящих '#', без единого чужого байта.
int main(int argc, char *argv[]) {
    SysClientEnv env;
    sys_client_init(env);

    const char *path = "/root/proxytest_out_XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX.txt";
    int path_len = (int)my_strlen(path);
    int claimed_data_len = 60;
    int real_data_len = seL4_MsgMaxLength - 3 - path_len; // = 57 при MsgMaxLength=120, path_len=60

    if (real_data_len <= 0 || real_data_len >= claimed_data_len) {
        sys_puts(0, "proxytest: internal error — параметры теста не создают перекос claimed/real.\n");
        sys_exit(env.root_ep);
        return 1;
    }

    seL4_SetMR(0, 136); // SYS_PROXY_WRITE_FILE
    seL4_SetMR(1, (seL4_Word)path_len);
    seL4_SetMR(2, (seL4_Word)claimed_data_len); // ЛОЖЬ — реально дальше передаём меньше
    for (int i = 0; i < path_len; i++) seL4_SetMR(3 + i, (seL4_Word)(uint8_t)path[i]);
    for (int i = 0; i < real_data_len; i++) seL4_SetMR(3 + path_len + i, (seL4_Word)'#');

    seL4_Call(env.root_ep, seL4_MessageInfo_new(0, 0, 0, (seL4_Word)(3 + path_len + real_data_len)));
    seL4_Word status = seL4_GetMR(0);

    sys_puts(0, "proxytest: SYS_PROXY_WRITE_FILE ");
    sys_puts(0, (status == 0) ? "status=OK\n" : "status=ERROR\n");

    // issuse.txt №66 (расследование) — читаем ОБРАТНО ТЕМ ЖЕ зашитым в
    // бинарник `path` сырым сисколлом SYS_PROXY_READ_FILE (135), НИ ОДНОГО
    // символа с клавиатуры/UART не участвует. Если это найдёт файл — баг
    // был в ручном вводе длинных строк (см. независимо всплывшее touch/ls
    // расхождение 51->41 символ), не в exfat.cpp; если НЕ найдёт — баг
    // реально в резолве пути/поиске по имени на сервере.
    seL4_SetMR(0, 135); // SYS_PROXY_READ_FILE
    seL4_SetMR(1, (seL4_Word)path_len);
    seL4_SetMR(2, 0); // offset
    for (int i = 0; i < path_len; i++) seL4_SetMR(3 + i, (seL4_Word)(uint8_t)path[i]);
    seL4_Call(env.root_ep, seL4_MessageInfo_new(0, 0, 0, (seL4_Word)(3 + path_len)));
    seL4_Word read_status = seL4_GetMR(0);
    seL4_Word bytes_read = (read_status == 0) ? seL4_GetMR(1) : 0;

    sys_puts(0, "proxytest: self-read (без клавиатуры, тот же зашитый путь) ");
    if (read_status == 0) {
        sys_puts(0, "НАШЁЛ файл, байт = ");
        sys_putdec_local((uint32_t)bytes_read);
        sys_puts(0, "\n");
    } else {
        sys_puts(0, "НЕ НАШЁЛ файл (status != 0) — баг реально в резолве/поиске имени, не во вводе.\n");
    }

    sys_puts(0, "proxytest: для ручной проверки тем же путём:\n  cat ");
    sys_puts(0, path);
    sys_puts(0, "\n");

    sys_exit(env.root_ep);
    return 0;
}
