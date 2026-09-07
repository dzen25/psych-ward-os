#include "h/sys_client.h"

// hashtest — контрольная сумма файла.
//
// ЗАЧЕМ. Проверить, что копия файла совпала с оригиналом, глядя на два
// числа, а не перечитывая сотни мегабайт глазами. Порядок работы:
//   1) посчитать сумму на исходном файле;
//   2) скопировать (cp);
//   3) посчитать сумму на копии и сравнить с первой.
// Совпали размер И сумма — копия верна.
//
// ПОЧЕМУ СУММА СЧИТАЕТСЯ НЕ ПРЯМО В SHM. Полезная нагрузка VFS лежит в
// НЕкэшируемой (Device) памяти: побайтовое чтение оттуда идёт единицы
// МБ/с и превратило бы проверку 162 МБ в минуты. Поэтому кусок сначала
// переносится в обычный (кэшируемый) буфер восьмибайтовыми словами —
// такое чтение Device-памяти на порядок быстрее побайтового, — и уже там
// хэшируется. Восьмибайтовые обращения обязаны быть выровнены, иначе
// Alignment Fault; область нагрузки выровнена (VFS_PAYLOAD_OFFSET кратен
// странице), поэтому копируются только ЦЕЛЫЕ слова, а хвост — побайтно.
//
// Сумма — FNV-1a по 64-битным словам, с добавлением длины в конце.
// Криптостойкость тут не нужна: задача — заметить расхождение копии, а не
// сопротивляться подбору.
//
// Запуск:
//   exec /sbin/tests/hashtest.elf /mnt/Realtek-RTL9210C_NVME/bigfile.txt

static SysClientEnv env;

constexpr int PATH_OFFSET = 0;
constexpr int DATA_OFFSET = (int)VFS_PAYLOAD_OFFSET;

static char g_path[256];
static seL4_CPtr g_ep = 0;
static int g_passed = 0;

// Кэшируемый буфер под один кусок — см. шапку про Device-память.
static char g_buf[VFS_PAYLOAD_MAX];

static uint64_t g_cntfrq = 0;
static inline uint64_t now_us() {
    uint64_t v;
    if (g_cntfrq == 0) { asm volatile("mrs %0, cntfrq_el0" : "=r"(g_cntfrq)); }
    asm volatile("isb; mrs %0, cntvct_el0" : "=r"(v));
    return g_cntfrq ? (v * 1000000ull / g_cntfrq) : 0;
}

static void putdec(long long val) {
    char buf[24]; int j = 0;
    if (val < 0) { sys_puts(0, "-"); val = -val; }
    if (val == 0) buf[j++] = '0';
    while (val > 0) { buf[j++] = (char)('0' + (val % 10)); val /= 10; }
    while (j > 0) { char c[2] = {buf[--j], 0}; sys_puts(0, c); }
}

static void puthex64(uint64_t v) {
    const char* hex = "0123456789ABCDEF";
    char out[19]; out[0] = '0'; out[1] = 'x';
    for (int i = 0; i < 16; i++) out[2 + i] = hex[(v >> ((15 - i) * 4)) & 0xF];
    out[18] = '\0';
    sys_puts(0, out);
}

static void step_begin(int n, const char* what) {
    sys_puts(0, "\nТЕСТ "); putdec(n); sys_puts(0, ": "); sys_puts(0, what); sys_puts(0, "\n");
}
static void step_ok(int n)  { sys_puts(0, "ТЕСТ "); putdec(n); sys_puts(0, " ЗАВЕРШЁН\n"); g_passed++; }
static void step_fail(int n, const char* why) {
    sys_puts(0, "ТЕСТ "); putdec(n); sys_puts(0, " ОСТАНОВЛЕН — ОШИБКА: "); sys_puts(0, why); sys_puts(0, "\n");
}

// --- контрольная сумма ---
static uint64_t g_hash = 0xcbf29ce484222325ull;
static uint64_t g_total = 0;
static inline void hash_bytes(const char* p, uint32_t len) {
    for (uint32_t i = 0; i < len; i++) {
        g_hash ^= (uint64_t)(uint8_t)p[i];
        g_hash *= 0x100000001b3ull;
    }
}

// Перенос куска из НЕкэшируемой нагрузки в обычную память целыми словами.
static void copy_from_shm(char* dst, const volatile char* src, uint32_t len) {
    uint32_t i = 0;
    uint32_t words = len / 8;
    const volatile uint64_t* sw = (const volatile uint64_t*)src;
    uint64_t* dw = (uint64_t*)dst;
    for (uint32_t k = 0; k < words; k++) dw[k] = sw[k];
    i = words * 8;
    for (; i < len; i++) dst[i] = src[i];
}

int main(int argc, char *argv[]) {
    (void)argc; (void)argv;
    sys_client_init(env);
    if (!env.shm) { sys_puts(0, "hashtest: нет доступа к SHM — запускать через exec.\n"); sys_exit(env.root_ep); return 1; }

    if (!env.arg || !*env.arg) {
        sys_puts(0, "hashtest: не задан путь к файлу.\n");
        sys_puts(0, "  exec /sbin/tests/hashtest.elf <путь к файлу>\n");
        sys_exit(env.root_ep); return 1;
    }
    char raw[256]; my_strlcpy(raw, env.arg, sizeof(raw));
    // Один аргумент — путь; лишние отсекаем, чтобы не молчать о опечатке.
    for (char* q = raw; *q; q++) if (*q == ' ') { *q = '\0'; break; }

    build_absolute_path(env.shm + PATH_OFFSET, raw, 128);
    // ВАЖЕН ПОРЯДОК: route_vfs_path() правит буфер НА МЕСТЕ — срезает
    // ведущий "/mnt", потому что драйвер тома ожидает путь уже без него.
    // Сначала маршрутизация, и только ПОТОМ забираем результат: иначе
    // драйверу уедет несрезанный путь и любая операция честно ответит
    // "не найдено". Ровно на этом споткнулся первый прогон.
    g_ep = route_vfs_path(env.shm + PATH_OFFSET, env.blk_ep, env.usb_storage_ep);
    my_strlcpy(g_path, env.shm + PATH_OFFSET, sizeof(g_path));

    sys_puts(0, "==========================================================\n");
    sys_puts(0, "hashtest — контрольная сумма файла\n\n");
    sys_puts(0, "файл: "); sys_puts(0, g_path); sys_puts(0, "\n");
    sys_puts(0, "==========================================================\n");

    // --- ТЕСТ 1: файл существует, размер известен ---
    step_begin(1, "файл найден, размер получен");
    uint64_t declared = 0;
    bool is_dir = false;
    {
        my_strlcpy(env.shm + PATH_OFFSET, g_path, 128);
        vfs_lock();
        seL4_SetMR(0, 128); // SYS_STAT
        seL4_Call(g_ep, seL4_MessageInfo_new(0, 0, 0, 1));
        int rc = (int)seL4_GetMR(0);
        declared = (uint64_t)seL4_GetMR(1);
        is_dir = (seL4_GetMR(2) != 0);
        vfs_unlock();
        if (rc != 0) { step_fail(1, "файл не найден"); sys_exit(env.root_ep); return 1; }
        if (is_dir)  { step_fail(1, "это каталог, а не файл"); sys_exit(env.root_ep); return 1; }
    }
    sys_puts(0, "  объявленный размер: "); putdec((long long)declared); sys_puts(0, " Б\n");
    step_ok(1);

    // --- ТЕСТ 2: файл прочитан целиком, сумма посчитана ---
    step_begin(2, "файл прочитан целиком, посчитана контрольная сумма");
    uint64_t t0 = now_us();
    uint64_t offset = 0;
    bool read_fail = false;
    while (1) {
        my_strlcpy(env.shm + PATH_OFFSET, g_path, 128);
        vfs_lock();
        seL4_SetMR(0, 119); // SYS_READ_FILE
        seL4_SetMR(1, (seL4_Word)offset);
        seL4_Call(g_ep, seL4_MessageInfo_new(0, 0, 0, 2));
        int rc = (int)seL4_GetMR(0);
        int got = (int)seL4_GetMR(1);
        if (rc == 0 && got > 0) copy_from_shm(g_buf, env.shm + DATA_OFFSET, (uint32_t)got);
        vfs_unlock();

        if (rc != 0) { read_fail = true; break; }
        if (got <= 0) break; // EOF
        hash_bytes(g_buf, (uint32_t)got);
        g_total += (uint64_t)got;
        offset += (uint64_t)got;
    }
    uint64_t dt = now_us() - t0;
    if (read_fail) { step_fail(2, "чтение оборвалось до конца файла"); sys_exit(env.root_ep); return 1; }
    if (g_total != declared) {
        sys_puts(0, "  прочитано "); putdec((long long)g_total);
        sys_puts(0, " Б, а объявлено "); putdec((long long)declared); sys_puts(0, " Б\n");
        step_fail(2, "прочитанный объём не совпал с объявленным размером");
        sys_exit(env.root_ep); return 1;
    }
    // Длина подмешивается в сумму: иначе усечённый файл, совпавший по
    // содержимому начала, дал бы ту же сумму.
    g_hash ^= g_total; g_hash *= 0x100000001b3ull;
    sys_puts(0, "  прочитано "); putdec((long long)g_total);
    sys_puts(0, " Б за "); putdec((long long)(dt / 1000)); sys_puts(0, " мс\n");
    step_ok(2);

    sys_puts(0, "\n==========================================================\n");
    sys_puts(0, "ИТОГ: пройдено шагов "); putdec(g_passed); sys_puts(0, " из 2.\n");
    sys_puts(0, "  размер:          "); putdec((long long)g_total); sys_puts(0, " Б\n");
    sys_puts(0, "  КОНТРОЛЬНАЯ СУММА: "); puthex64(g_hash); sys_puts(0, "\n");
    if (dt > 0) {
        sys_puts(0, "  скорость чтения: "); putdec((long long)(g_total * 1000000ull / dt / 1024));
        sys_puts(0, " КБ/с (с учётом переноса из SHM и самого хэширования)\n");
    }
    sys_puts(0, "==========================================================\n");
    sys_puts(0, "Сравнивайте РАЗМЕР и СУММУ до и после копирования.\n");
    // Выход ТОЛЬКО через sys_exit: возврат из main в этих утилитах уходит
    // в никуда и падает FATAL FAULT с PC=0 (поймано на первом прогоне).
    sys_exit(env.root_ep);
    return 0;
}
