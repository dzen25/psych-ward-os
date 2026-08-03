// sign_elf — офлайн-утилита для Фазы 12 (см. ROADMAP.md): подписывает
// /sbin/*.elf, /service/*.elf, /etc/init.conf перед копированием в
// load_chain/, чтобы rootserver (load_elf_from_disk() в main.cpp) мог
// проверить подпись перед исполнением/использованием файла.
//
// СОБИРАЕТСЯ ХОСТОВЫМ gcc (НЕ ARM cross-toolchain) — этот бинарник никогда
// не попадает на плату, работает только здесь, на этой Linux-машине, где
// хранится приватный ключ. См. Фазу 12 в ROADMAP.md за полным объяснением
// модели доверия (коротко: приватный ключ живёт ТОЛЬКО тут, вне git и вне
// SD-карты; "разрешение обновлять бинарники" — это просто владение файлом
// ключа на этой машине, никакого пароля/механизма внутри самой ОС нет).
//
// Сборка:
//   gcc -O2 -o sign_elf sign_elf.c \
//       ../monocypher/src/monocypher.c \
//       ../monocypher/src/optional/monocypher-ed25519.c \
//       -I../monocypher/src -I../monocypher/src/optional
//
// Использование:
//   sign_elf genkey <новый_файл_приватного_ключа>
//       Печатает публичный ключ как C-массив (для вставки в main.cpp) в stdout.
//   sign_elf sign <файл_приватного_ключа> <входной_файл> <выходной_файл>
//       Дописывает 68-байтовый трейлер (4 байта magic "PWSG" + 64-байтовая
//       Ed25519-подпись) в конец файла. Формат совпадает с тем, что ожидает
//       load_elf_from_disk() в main.cpp — см. Фазу 12 в ROADMAP.md.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#include "monocypher.h"
#include "monocypher-ed25519.h"

static const unsigned char SIG_MAGIC[4] = { 'P', 'W', 'S', 'G' };
#define SIG_TRAILER_SIZE 68 /* 4 (magic) + 64 (Ed25519 signature) */

static void die(const char *msg) {
    fprintf(stderr, "sign_elf: %s\n", msg);
    exit(1);
}

static unsigned char *read_whole_file(const char *path, size_t *out_size) {
    FILE *f = fopen(path, "rb");
    if (!f) die("не удалось открыть входной файл");
    if (fseek(f, 0, SEEK_END) != 0) die("fseek не удался");
    long size = ftell(f);
    if (size < 0) die("ftell не удался");
    rewind(f);
    unsigned char *buf = malloc((size_t)size);
    if (!buf) die("malloc не удался");
    if (size > 0 && fread(buf, 1, (size_t)size, f) != (size_t)size) die("fread не удался");
    fclose(f);
    *out_size = (size_t)size;
    return buf;
}

static void write_whole_file(const char *path, const unsigned char *data, size_t size, int mode0600) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, mode0600 ? 0600 : 0644);
    if (fd < 0) die("не удалось создать выходной файл");
    size_t off = 0;
    while (off < size) {
        ssize_t n = write(fd, data + off, size - off);
        if (n <= 0) die("write не удался");
        off += (size_t)n;
    }
    close(fd);
}

static int cmd_genkey(const char *out_priv_path) {
    // Отказываемся тихо перезаписывать существующий ключ — потеря приватного
    // ключа означает необходимость перевыпускать и перепрошивать ВСЕ
    // подписанные бинарники с новым публичным ключом в main.cpp.
    struct stat st;
    if (stat(out_priv_path, &st) == 0) die("файл ключа уже существует — не перезаписываю");

    unsigned char seed[32];
    int rnd = open("/dev/urandom", O_RDONLY);
    if (rnd < 0) die("не удалось открыть /dev/urandom");
    if (read(rnd, seed, sizeof(seed)) != (ssize_t)sizeof(seed)) die("не удалось прочитать случайные байты");
    close(rnd);

    // ВАЖНО: писать seed на диск ДО вызова crypto_ed25519_key_pair() —
    // сам Monocypher защитно затирает свой параметр seed[] изнутри сразу
    // после копирования (см. monocypher-ed25519.c: `crypto_wipe(seed, 32)`
    // на строке 421) — вызванная ПОСЛЕ этого запись писала бы на диск уже
    // обнулённый буфер, хотя публичный ключ на экране при этом печатался бы
    // правильный (он считается раньше). Была ровно эта ошибка порядка.
    write_whole_file(out_priv_path, seed, sizeof(seed), 1 /* 0600 */);

    unsigned char secret_key[64];
    unsigned char public_key[32];
    crypto_ed25519_key_pair(secret_key, public_key, seed); // затирает seed сам
    crypto_wipe(secret_key, sizeof(secret_key));

    printf("Приватный ключ сохранён: %s (0600, храните вне git/SD-карты)\n", out_priv_path);
    printf("Публичный ключ — вставить в main.cpp как OS_PUBLIC_KEY:\n\n");
    printf("constexpr unsigned char OS_PUBLIC_KEY[32] = {\n    ");
    for (int i = 0; i < 32; i++) {
        printf("0x%02x%s", public_key[i], (i == 31) ? "\n" : ((i % 8 == 7) ? ",\n    " : ", "));
    }
    printf("};\n");
    return 0;
}

static int cmd_sign(const char *priv_path, const char *in_path, const char *out_path) {
    size_t seed_size = 0;
    unsigned char *seed_buf = read_whole_file(priv_path, &seed_size);
    if (seed_size != 32) die("файл ключа должен быть ровно 32 байта (seed)");

    unsigned char secret_key[64];
    unsigned char public_key[32];
    crypto_ed25519_key_pair(secret_key, public_key, seed_buf);
    crypto_wipe(seed_buf, seed_size);
    free(seed_buf);

    size_t in_size = 0;
    unsigned char *in_buf = read_whole_file(in_path, &in_size);

    unsigned char signature[64];
    crypto_ed25519_sign(signature, secret_key, in_buf, in_size);
    crypto_wipe(secret_key, sizeof(secret_key));

    // Сверяем сразу же своим же публичным ключом — если это по какой-то
    // причине не проходит, лучше упасть тут, чем выкатить бинарник, который
    // rootserver потом молча откажется исполнять.
    if (crypto_ed25519_check(signature, public_key, in_buf, in_size) != 0) {
        die("самопроверка подписи не прошла (внутренняя ошибка) — файл НЕ записан");
    }

    unsigned char *out_buf = malloc(in_size + SIG_TRAILER_SIZE);
    if (!out_buf) die("malloc не удался");
    memcpy(out_buf, in_buf, in_size);
    memcpy(out_buf + in_size, SIG_MAGIC, 4);
    memcpy(out_buf + in_size + 4, signature, 64);
    free(in_buf);

    write_whole_file(out_path, out_buf, in_size + SIG_TRAILER_SIZE, 0);
    free(out_buf);

    printf("Подписано: %s -> %s (%zu + %d байт)\n", in_path, out_path, in_size, SIG_TRAILER_SIZE);
    return 0;
}

int main(int argc, char **argv) {
    if (argc == 3 && strcmp(argv[1], "genkey") == 0) {
        return cmd_genkey(argv[2]);
    }
    if (argc == 5 && strcmp(argv[1], "sign") == 0) {
        return cmd_sign(argv[2], argv[3], argv[4]);
    }
    fprintf(stderr,
        "Usage:\n"
        "  sign_elf genkey <new_priv_key_file>\n"
        "  sign_elf sign <priv_key_file> <in_file> <out_file>\n");
    return 1;
}
