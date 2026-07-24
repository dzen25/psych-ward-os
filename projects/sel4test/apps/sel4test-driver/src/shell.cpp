#include <sel4/sel4.h>
#include "h/common.h"
#include "h/platform.h"
#include <stdint.h>

// Адреса для синхронизации доступа к VFS и TTY
#define BOOT_BLK_EP 7

// Смещение локального часового пояса от UTC (в часах) для команды `date`.
// Системные часы (SYS_GET_TIME) всегда хранятся в UTC — здесь только отображение.
static const int TZ_OFFSET_HOURS = 3;

// Выделяем по 16 КБ для каждого потока и СТРОГО выравниваем по 16 байт (требование ARM64)
static char ls_thread_stack[16384] __attribute__((aligned(16)));
static char grep_thread_stack[16384] __attribute__((aligned(16)));

static char* shm_base = nullptr;
static volatile int* vfs_spinlock_ptr = nullptr;

// Милстоун 4.4 (см. wifi_driver.cpp) — "знакомые сети" (PATH_WIFI_PQW,
// строки "имясети|пароль"). Буферы для чтения/перезаписи файла целиком
// (у blk_driver нет append, см. net_driver.cpp/platform.h) — статические
// (не в стеке main()), по тому же принципу, что ls_thread_stack/cmd_history
// выше: единственный поток шелла обрабатывает одну команду за раз, повторный
// вход невозможен, а 2x4000 байт на стеке main() было бы неоправданным риском.
static char g_pqw_old[4000];
static char g_pqw_new[4000];
// SSID сети, к которой шелл последний раз успешно подключился этой сессией
// (используется командой "wifi clean" без явного имени сети — см. ниже).
static char g_wifi_current_ssid[33] = {0};

// ВАЖНО: обычные volatile чтение/запись, БЕЗ __sync_lock_test_and_set/
// __sync_lock_release. Разделяемая SHM (см. vfs_spinlock_ptr ниже) мапится
// некэшируемой Device-памятью (map_frame_robust(), main.cpp — ради
// когерентности с GENET DMA), а exclusive-load/store инструкции (LDXR/STXR,
// именно в них компилируются __sync_lock_*) на Device-памяти по спеке ARM
// имеют непредсказуемое поведение — на живом железе это уронило ВЕСЬ kernel
// seL4 необрабатываемым исключением шины ("halting... Kernel entry via
// Unknown (0)"), а не просто эту функцию. Настоящий hardware-atomic тут и не
// нужен: система однопроцессорная (SMP OFF, easy-settings.cmake) — переключение
// контекста происходит только на syscall/yield, а не посреди пары "прочитать
// проверить -> записать" ниже, так что обычного volatile-флага достаточно.
void vfs_lock() {
    if (!vfs_spinlock_ptr) return; // Guard against early calls
    while (*vfs_spinlock_ptr) {
        seL4_Yield();
    }
    *vfs_spinlock_ptr = 1;
}

void vfs_unlock() {
    if (!vfs_spinlock_ptr) return;
    *vfs_spinlock_ptr = 0;
}

void __assert_fail(const char *assertion, const char *file, int line, const char *function) { while(1); }

static inline seL4_IPCBuffer* get_local_ipc() {
    seL4_Word tls_addr;
    asm volatile("mrs %0, tpidrro_el0" : "=r"(tls_addr)); 
    return (seL4_IPCBuffer*)(tls_addr - 1024);
}

static void my_strcpy(char *dest, const char *src);
static seL4_Word my_strlen(const char *s);

static void sys_write(int fd, const char* str);
static char sys_read_fd(int fd);

// Перенос длинных строк по словам теперь делает uart_driver.cpp (единственный
// процесс, через который реально проходит SYS_PUTS от ЛЮБОГО клиента —
// shell/wifi_driver/net_driver/blk_driver и т.д.) — раньше это делалось
// только здесь и только для того, что печатает сам шелл, из-за чего
// диагностические строки других драйверов вообще не переносились, а
// собственный перенос шелла к тому же считал длину строки в БАЙТАХ UTF-8
// вместо видимых символов (кириллица преждевременно вызывала перенос).
// Здесь остаётся простая пересылка — вся логика переноса теперь в одном
// месте, см. uart_putc_wrapped()/uart_flush_word() в uart_driver.cpp.
static void sys_puts(seL4_CPtr _ignored, const char *str) {
    sys_write(1, str);
}

// Helper to print a 64-bit value in hex via IPC.
static void sys_puthex(seL4_Word val) {
    char buf[17];
    const char hex_chars[] = "0123456789ABCDEF";
    buf[16] = '\0';
    for (int i = 15; i >= 0; i--) {
        buf[15 - i] = hex_chars[(val >> (i * 4)) & 0xF];
    }
    sys_puts(0, buf); // The first argument is ignored, writes to stdout
}

// Helper to print an unsigned value in decimal via IPC.
static void sys_putdec(seL4_Word val) {
    char buf[21]; int j = 0;
    if (val == 0) buf[j++] = '0';
    while (val > 0) { buf[j++] = (val % 10) + '0'; val /= 10; }
    while (j > 0) { char c[2] = {buf[--j], 0}; sys_puts(0, c); }
}

static void print_ip(seL4_CPtr console_ep, const uint8_t ip[4]) {
    sys_putdec(ip[0]); sys_puts(console_ep, ".");
    sys_putdec(ip[1]); sys_puts(console_ep, ".");
    sys_putdec(ip[2]); sys_puts(console_ep, ".");
    sys_putdec(ip[3]);
}

// Печатает микросекунды как "N.NNN" мс (для вывода в стиле обычного unix ping).
static void print_rtt_ms(seL4_CPtr console_ep, uint32_t us) {
    sys_putdec(us / 1000);
    sys_puts(console_ep, ".");
    uint32_t frac = us % 1000;
    if (frac < 100) sys_puts(console_ep, "0");
    if (frac < 10) sys_puts(console_ep, "0");
    sys_putdec(frac);
}

// Печатает миллиградусы Цельсия как "N.N C" (см. sys_get_temp_mC).
static void print_temp_c(seL4_CPtr console_ep, int32_t temp_mC) {
    if (temp_mC < 0) { sys_puts(console_ep, "-"); temp_mC = -temp_mC; }
    sys_putdec((seL4_Word)(temp_mC / 1000));
    sys_puts(console_ep, ".");
    sys_putdec((seL4_Word)((temp_mC / 100) % 10));
    sys_puts(console_ep, " C");
}

// Универсальная запись в файловый дескриптор
void sys_write(int fd, const char* str) {
    seL4_IPCBuffer *ipc = get_local_ipc();
    seL4_CPtr target_ep = ipc->caps_or_badges[fd]; 
    
    int total_len = 0;
    while(str[total_len]) total_len++;
    
    int offset = 0;
    while (offset < total_len) {
        int chunk = total_len - offset;
        if (chunk > 100) chunk = 100; 
        
        ipc->msg[0] = 8; // ВСЕГДА ИСПОЛЬЗУЕМ 8 (SYS_PUTS)
        for (int i = 0; i < chunk; i++) {
            ipc->msg[i + 1] = str[offset + i];
        }
        seL4_Call(target_ep, seL4_MessageInfo_new(0, 0, 0, chunk + 1));
        offset += chunk;
    }
}

void sys_write_eof(int fd) {
    seL4_IPCBuffer *ipc = get_local_ipc();
    seL4_CPtr target_ep = ipc->caps_or_badges[fd];
    ipc->msg[0] = 8; // SYS_PUTS
    ipc->msg[1] = '\0'; // Тот самый заветный EOF
    seL4_Call(target_ep, seL4_MessageInfo_new(0, 0, 0, 2));
}

// Универсальное чтение из файлового дескриптора
char sys_read_fd(int fd) {
    seL4_IPCBuffer *ipc = get_local_ipc();
    seL4_CPtr target_ep = ipc->caps_or_badges[fd];
    ipc->msg[0] = 6; // ВСЕГДА ИСПОЛЬЗУЕМ 6 (SYS_READ)
    seL4_Call(target_ep, seL4_MessageInfo_new(0, 0, 0, 1));
    return (char)ipc->msg[0];
}

// Принудительно выталкивает застрявший текст на экран (нужно для prompt и эхо символов)
static void sys_flush(seL4_CPtr console_ep) {
    seL4_IPCBuffer *ipc = get_local_ipc();
    ipc->msg[0] = 9; // SYS_FLUSH ID
    seL4_Call(console_ep, seL4_MessageInfo_new(0, 0, 0, 1));
}

static void my_strcpy(char *dest, const char *src) {
    while ((*dest++ = *src++));
}

static void my_strcat(char *dest, const char *src) {
    while (*dest) dest++;
    while ((*dest++ = *src++));
}

#define strcpy my_strcpy

static int my_strcmp(const char *s1, const char *s2) { 
    while (*s1 && (*s1 == *s2)) { s1++; s2++; } return *(const unsigned char*)s1 - *(const unsigned char*)s2; 
}

static int my_strncmp(const char *s1, const char *s2, int n) {
    while (n && *s1 && (*s1 == *s2)) { s1++; s2++; n--; }
    if (n == 0) return 0;
    return *(const unsigned char*)s1 - *(const unsigned char*)s2;
}

static const char* my_strstr(const char* haystack, const char* needle) {
    if (!*needle) return haystack;
    const char* p1 = haystack;
    while (*p1) {
        const char* p1_begin = p1, *p2 = needle;
        while (*p1 && *p2 && *p1 == *p2) { p1++; p2++; }
        if (!*p2) return p1_begin;
        p1 = p1_begin + 1;
    }
    return nullptr;
}

static void my_strncpy(char *dest, const char *src, int n) {
    int i;
    for (i = 0; i < n && src[i] != '\0'; i++) {
        dest[i] = src[i];
    }
    for ( ; i < n; i++) {
        dest[i] = '\0';
    }
}

// Копирует не более (cap-1) символов и всегда завершает '\0' в пределах [0, cap).
// Возвращает итоговую длину скопированной строки (без учета '\0').
static int my_strlcpy(char *dest, const char *src, int cap) {
    if (cap <= 0) return 0;
    int i = 0;
    for (; i < cap - 1 && src[i] != '\0'; i++) {
        dest[i] = src[i];
    }
    dest[i] = '\0';
    return i;
}

static seL4_Word my_strlen(const char *s) {
    seL4_Word len = 0; while (*s++) len++; return len;
}

static int simple_atoi(const char *str) {
    int res = 0;
    while (*str >= '0' && *str <= '9') { res = res * 10 + (*str - '0'); str++; }
    return res;
}

static bool is_piping = false;

enum NetCommand {
    NET_CMD_PING = 1,
    NET_CMD_SEND = 2,
    NET_CMD_STATUS = 3,
    NET_CMD_RESOLVE = 4,
    NET_CMD_RECV = 5,
    NET_CMD_NTP = 6,
};

// Фаза 4.5.6 (Wi-Fi data-plane, per-interface команды шелла) — значения
// ДОЛЖНЫ совпадать с NetIface enum в net_driver.cpp (IFACE_GENET/IFACE_WIFI);
// протокол NET_CMD_* между процессами и так уже задаётся простым совпадением
// чисел без общего заголовка (см. enum NetCommand выше), это тот же приём.
constexpr seL4_Word NET_IFACE_GENET = 0;
constexpr seL4_Word NET_IFACE_WIFI  = 1;

// Офсеты readiness-флага/DNS-IP/ping-статистики в общей SHM — по одному
// набору на интерфейс. GENET занимает исторический диапазон 4060-4095
// (не трогаем, старые скрипты/привычки не должны сломаться), Wi-Fi получает
// отдельный диапазон дальше (4100-8191 между net_vfs_lock'ом на 4096 и
// WIFI_SHM_* на 8192 — полностью свободно, с большим запасом). Числа ДОЛЖНЫ
// совпадать с одноимёнными функциями в net_driver.cpp.
// ВАЖНО: dns_ip читается/пишется как seL4_Word (8 байт на aarch64) — на
// Device-памяти (см. map_frame_robust()/main.cpp) невыровненный на 8 байт
// доступ ловит Alignment Fault (живой краш: было 4204, не кратно 8 — см.
// net_driver.cpp). Числа здесь ДОЛЖНЫ совпадать с net_driver.cpp.
static inline uint32_t net_mailbox_ready_offset(seL4_Word iface) { return (iface == NET_IFACE_WIFI) ? 4200 : 4060; }
static inline uint32_t net_mailbox_dns_ip_offset(seL4_Word iface) { return (iface == NET_IFACE_WIFI) ? 4208 : 4064; }
static inline uint32_t net_mailbox_ping_stats_offset(seL4_Word iface) { return (iface == NET_IFACE_WIFI) ? 4216 : 4068; }

// По просьбе пользователя — без явного `-W` шелл сам выбирает интерфейс:
// GENET, если у него реально есть IP (dhcp_bound), иначе Wi-Fi, если есть у
// него, иначе сразу ошибка "нет сети" — БЕЗ обращения к net_driver вообще
// (незачем ждать таймаут DHCP на мёртвом интерфейсе). net_driver публикует
// dhcp_bound обоих интерфейсов сюда каждый тик (см. net_publish_iface_ready()
// в net_driver.cpp) — простые uint32_t, 4-байтного выравнивания достаточно.
// Числа ДОЛЖНЫ совпадать с net_ready_flag_offset() там же.
static inline uint32_t net_ready_flag_offset(seL4_Word iface) { return (iface == NET_IFACE_WIFI) ? 4248 : 4244; }

static char *next_token(char **cursor) {
    if (!cursor || !*cursor) return nullptr;
    char *tok = *cursor;
    while (*tok == ' ') tok++;
    if (*tok == '\0') { *cursor = tok; return nullptr; }

    char *end = tok;
    while (*end && *end != ' ') end++;
    if (*end == ' ') {
        *end = '\0';
        end++;
        while (*end == ' ') end++;
    }
    *cursor = end;
    return tok;
}

static int parse_port(const char *str, uint16_t *out) {
    if (!str || !*str) return -1;
    int value = 0;
    while (*str >= '0' && *str <= '9') {
        value = value * 10 + (*str - '0');
        if (value > 65535) return -1;
        str++;
    }
    if (*str != '\0' || value <= 0) return -1;
    *out = (uint16_t)value;
    return 0;
}

static int parse_ipv4(const char *str, uint8_t out[4]) {
    for (int part = 0; part < 4; part++) {
        if (!str || *str < '0' || *str > '9') return -1;
        int value = 0;
        int digits = 0;
        while (*str >= '0' && *str <= '9') {
            value = value * 10 + (*str - '0');
            if (value > 255) return -1;
            str++;
            digits++;
        }
        if (digits == 0) return -1;
        out[part] = (uint8_t)value;
        if (part < 3) {
            if (*str != '.') return -1;
            str++;
        }
    }
    return *str == '\0' ? 0 : -1;
}

static seL4_Word pack_ipv4(const uint8_t ip[4]) {
    return ((seL4_Word)ip[0] << 24) | ((seL4_Word)ip[1] << 16) |
           ((seL4_Word)ip[2] << 8) | (seL4_Word)ip[3];
}

// Общий разбор "-W" (принудительно Wi-Fi, игнорируя авто-выбор ниже) для
// ping/send/sendto/recv/ntp. НЕ через next_token(): тот необратимо режет
// строку на месте (заменяет разделяющий пробел на '\0'), а send/sendto
// должны получить остаток строки как есть, ВКЛЮЧАЯ пробелы (это текст
// сообщения, не токен) — поэтому здесь ручной, недеструктивный разбор
// префикса: если флага нет, *cursor вообще не трогаем. Возвращает true,
// если флаг был и cursor сдвинут; false — флага нет, cursor как был.
static bool parse_wifi_flag(char **cursor) {
    if (!cursor || !*cursor) return false; // netstat/recv/ntp без аргументов вообще (arg == nullptr)
    char *p = *cursor;
    while (*p == ' ') p++;
    if (p[0] == '-' && p[1] == 'W' && (p[2] == ' ' || p[2] == '\0')) {
        p += 2;
        while (*p == ' ') p++;
        *cursor = p;
        return true;
    }
    return false;
}

// Без явного "-W" — авто-выбор интерфейса по умолчанию (по просьбе
// пользователя): GENET, если у него реально есть IP; иначе Wi-Fi, если есть
// у него; иначе сообщаем "нет сети" СРАЗУ, локально, вообще не трогая
// net_driver — незачем ждать 5-10с таймаута DHCP на заведомо мёртвом
// интерфейсе. Готовность обоих читается напрямую из SHM (net_driver
// публикует dhcp_bound туда каждый тик, см. net_publish_iface_ready() в
// net_driver.cpp) — дёшево, без единого IPC-вызова.
static bool pick_default_iface(seL4_CPtr console_ep, seL4_Word *out_iface) {
    volatile uint32_t *genet_ready = (volatile uint32_t*)(shm_base + net_ready_flag_offset(NET_IFACE_GENET));
    volatile uint32_t *wifi_ready  = (volatile uint32_t*)(shm_base + net_ready_flag_offset(NET_IFACE_WIFI));
    if (*genet_ready) { *out_iface = NET_IFACE_GENET; return true; }
    if (*wifi_ready)  { *out_iface = NET_IFACE_WIFI;  return true; }
    sys_puts(console_ep, "[SHELL] Error: no network connection available (neither GENET nor Wi-Fi has an IP). Use 'wifi connect' or plug in the cable.\n");
    return false;
}

// Общая обвязка для ping/send/sendto/recv/ntp: "-W" — принудительно Wi-Fi;
// иначе — авто-выбор (см. pick_default_iface() выше). Возвращает false,
// если сети нет вообще (ошибка уже напечатана, вызывающий код должен сразу
// `continue`).
static bool resolve_iface(seL4_CPtr console_ep, char **cursor, seL4_Word *out_iface) {
    if (parse_wifi_flag(cursor)) { *out_iface = NET_IFACE_WIFI; return true; }
    return pick_default_iface(console_ep, out_iface);
}

static void net_send_text_command(seL4_CPtr net_ep, seL4_Word cmd, seL4_Word ip, seL4_Word port, const char *text, seL4_Word iface) {
    const int word_bytes = sizeof(seL4_Word);
    const int max_text = 48;
    char clipped[max_text];
    int text_len = 0;

    if (text) {
        while (text[text_len] && text_len < max_text) {
            clipped[text_len] = text[text_len];
            text_len++;
        }
    }

    seL4_SetMR(0, cmd);
    seL4_SetMR(1, ip);
    seL4_SetMR(2, port);
    seL4_SetMR(3, (seL4_Word)text_len);
    seL4_SetMR(4, iface); // Фаза 4.5.6 — см. NetIface в net_driver.cpp; текст теперь начинается с MR5, не MR4

    int word_count = (text_len + word_bytes - 1) / word_bytes;
    for (int w = 0; w < word_count; w++) {
        seL4_Word packed = 0;
        for (int b = 0; b < word_bytes; b++) {
            int idx = w * word_bytes + b;
            if (idx < text_len) {
                packed |= ((seL4_Word)(uint8_t)clipped[idx]) << (b * 8);
            }
        }
        seL4_SetMR(5 + w, packed);
    }

    seL4_Send(net_ep, seL4_MessageInfo_new(0, 0, 0, 5 + word_count));
    seL4_Yield();
}

#define sys_puts_direct sys_puts

static void sys_thread_exit() {
    // 1. Получаем безопасный указатель на буфер текущего потока
    seL4_IPCBuffer *ipc = get_local_ipc(); 
    
    // 2. Везде используем локальный 'ipc' вместо глобального макроса
    seL4_CPtr root_ep = ipc->msg[BOOT_ROOT_EP];
    ipc->msg[0] = 105; // ID нашего нового сисколла
    seL4_Call(root_ep, seL4_MessageInfo_new(0, 0, 0, 1));
    while(1) seL4_Yield(); // Сюда выполнение никогда не дойдет, ядро уничтожит поток
}

static int spawn_thread(seL4_Word func_ptr, seL4_Word stack_top, seL4_Word arg0, seL4_Word arg1, seL4_Word arg2, int pipe_id, seL4_CPtr stdin_cap, seL4_CPtr stdout_cap, seL4_CPtr stderr_cap) {
    seL4_CPtr my_root_syscall_ep = get_local_ipc()->msg[BOOT_ROOT_EP];

    get_local_ipc()->msg[0] = 101; // SYS_CLONE
    get_local_ipc()->msg[1] = func_ptr;
    get_local_ipc()->msg[2] = arg0;
    get_local_ipc()->msg[3] = arg1;
    get_local_ipc()->msg[4] = arg2;
    get_local_ipc()->msg[5] = stdin_cap;
    get_local_ipc()->msg[6] = stdout_cap;
    get_local_ipc()->msg[7] = stderr_cap;
    get_local_ipc()->msg[8] = pipe_id;
    get_local_ipc()->msg[9] = stack_top;
    seL4_MessageInfo_t info = seL4_MessageInfo_new(0, 0, 0, 10);
    seL4_Call(my_root_syscall_ep, info);
    return seL4_GetMR(0);
}

static char sys_read(seL4_CPtr _ignored) {
    return sys_read_fd(0);
}

// Блокирующее чтение одного байта: пропускает "нет данных" (-1/255) от
// uart_driver, пока реальный байт не придёт. Нужно и для обычного ввода, и
// для дочтения байтов ANSI-последовательности стрелки (см. цикл ввода ниже) —
// после ESC второй и третий байт могут ещё не долететь до kbd_buffer.
static char sys_read_blocking(seL4_CPtr console_ep) {
    while (1) {
        char c = sys_read(console_ep);
        if (c == (char)-1 || c == (char)255) { seL4_Yield(); continue; }
        return c;
    }
}

static seL4_Word sys_get_time(seL4_CPtr timer_ep) {
    seL4_SetMR(0, 3); // 3 = SYS_GET_TIME
    seL4_Call(timer_ep, seL4_MessageInfo_new(0, 0, 0, 1));
    return seL4_GetMR(0);
}

// Мс с момента запуска timer_driver (не привязано к эпохе Unix).
static seL4_Word sys_get_uptime(seL4_CPtr timer_ep) {
    seL4_SetMR(0, 4); // 4 = SYS_GET_UPTIME
    seL4_Call(timer_ep, seL4_MessageInfo_new(0, 0, 0, 1));
    return seL4_GetMR(0);
}

// Температура кристалла (миллиградусы Цельсия), см. SYS_GET_TEMP в
// timer_driver.cpp. Возвращает false, пока AVS-датчик еще не выдал
// валидное показание (единичные мс сразу после старта).
static bool sys_get_temp_mC(seL4_CPtr timer_ep, int32_t *out_mC) {
    seL4_SetMR(0, 6); // 6 = SYS_GET_TEMP
    seL4_Call(timer_ep, seL4_MessageInfo_new(0, 0, 0, 1));
    seL4_Word status = seL4_GetMR(0);
    *out_mC = (int32_t)(int64_t)seL4_GetMR(1);
    return status == 0;
}

// Диагностика VideoCore mailbox (Фаза 4.6, расследование DVFS — см.
// ROADMAP.md/timer_driver.cpp SYS_MBOX_PROBE). Возвращает индекс
// сработавшего варианта трансляции bus-адреса или -1, если не ответил
// ни на один.
static int sys_mbox_probe(seL4_CPtr timer_ep) {
    seL4_SetMR(0, 7); // 7 = SYS_MBOX_PROBE
    seL4_Call(timer_ep, seL4_MessageInfo_new(0, 0, 0, 1));
    return (int)(seL4_Word)seL4_GetMR(0);
}

// Разбивает число дней с 1970-01-01 на (год, месяц, день) в пролептическом
// григорианском календаре. Алгоритм Хауарда Хиннанта (chrono-совместимый,
// не требует libc/времени с плавающей точкой).
static void civil_from_days(long z, int *y, int *m, int *d) {
    z += 719468;
    long era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    *d = (int)(doy - (153 * mp + 2) / 5 + 1);
    *m = (int)(mp + (mp < 10 ? 3 : -9));
    *y = (int)((long)yoe + era * 400 + (*m <= 2 ? 1 : 0));
}

static void put_2digit(seL4_CPtr console_ep, int val) {
    char buf[3];
    if (val < 0) val = 0;
    if (val > 99) val = 99;
    buf[0] = (char)('0' + (val / 10));
    buf[1] = (char)('0' + (val % 10));
    buf[2] = '\0';
    sys_puts(console_ep, buf);
}

// Печатает "YYYY-MM-DD HH:MM:SS UTC+N" по мс с эпохи Unix (UTC) и смещению в часах.
static void print_localtime(seL4_CPtr console_ep, seL4_Word epoch_ms, int tz_offset_hours) {
    long total_seconds = (long)(epoch_ms / 1000) + (long)tz_offset_hours * 3600;
    long days = total_seconds / 86400;
    long secs_of_day = total_seconds % 86400;
    if (secs_of_day < 0) { secs_of_day += 86400; days -= 1; }

    int year, month, day;
    civil_from_days(days, &year, &month, &day);

    int hour = (int)(secs_of_day / 3600);
    int minute = (int)((secs_of_day % 3600) / 60);
    int second = (int)(secs_of_day % 60);

    sys_putdec((seL4_Word)year);
    sys_puts(console_ep, "-"); put_2digit(console_ep, month);
    sys_puts(console_ep, "-"); put_2digit(console_ep, day);
    sys_puts(console_ep, " "); put_2digit(console_ep, hour);
    sys_puts(console_ep, ":"); put_2digit(console_ep, minute);
    sys_puts(console_ep, ":"); put_2digit(console_ep, second);
    sys_puts(console_ep, " UTC");
    sys_puts(console_ep, tz_offset_hours >= 0 ? "+" : "-");
    sys_putdec((seL4_Word)(tz_offset_hours >= 0 ? tz_offset_hours : -tz_offset_hours));
    sys_puts(console_ep, "\n");
}

// Фаза 4.5 (см. ROADMAP.md/timer_driver.cpp SYS_SLEEP_MS): раньше это был
// клиентский busy-poll (sys_get_time() в цикле с seL4_Yield()) — теперь
// обычный блокирующий IPC-вызов, timer_driver сам спит на реальном IRQ
// физического таймера и отвечает по дедлайну.
static void sys_sleep(seL4_CPtr timer_ep, seL4_Word ms) {
    seL4_SetMR(0, 8); // 8 = SYS_SLEEP_MS
    seL4_SetMR(1, ms);
    seL4_Call(timer_ep, seL4_MessageInfo_new(0, 0, 0, 2));
}

static void sys_recover(const char* driver_name) {
    seL4_CPtr root_ep = get_local_ipc()->msg[BOOT_ROOT_EP];

    char safe_name[32] = {0};
    my_strncpy(safe_name, driver_name, 31);

    seL4_SetMR(0, 117); // SYS_RECOVER
    uint64_t* name_ptr = (uint64_t*)safe_name;
    for (int i = 0; i < 4; i++) {
        seL4_SetMR(i + 1, name_ptr[i]);
    }

    // Передаем 5 регистров (1 для номера сисколла + 4 для имени)
    seL4_Call(root_ep, seL4_MessageInfo_new(0, 0, 0, 5));
}

// Ручной жизненный цикл Wi-Fi (см. common.h SYS_START_WIFI/SYS_STOP_WIFI/
// SYS_WIFI_STATUS — wifi_driver больше не спавнится при загрузке, подозрение
// на гонку мапинга/таймингов при одновременном спавне с остальными
// драйверами, см. ROADMAP.md/память проекта). В отличие от sys_recover()
// выше, имя процесса передавать не нужно — рутсервер сам ищет "wifi_driver".
static int sys_start_wifi() {
    seL4_CPtr root_ep = get_local_ipc()->msg[BOOT_ROOT_EP];
    seL4_SetMR(0, SYS_START_WIFI);
    seL4_Call(root_ep, seL4_MessageInfo_new(0, 0, 0, 1));
    return (int)seL4_GetMR(0);
}

static int sys_stop_wifi() {
    seL4_CPtr root_ep = get_local_ipc()->msg[BOOT_ROOT_EP];
    seL4_SetMR(0, SYS_STOP_WIFI);
    seL4_Call(root_ep, seL4_MessageInfo_new(0, 0, 0, 1));
    return (int)seL4_GetMR(0);
}

// Возвращает 0 (не запущен) / 1 (запущен, ещё не готов) / 2 (готов принимать
// WIFI_CMD_*). Любой seL4_Call(wifi_ep, ...) ДО статуса 2 рискует зависнуть
// навсегда — до Милстоуна с ручным стартом wifi_driver всегда был жив к
// моменту, когда шелл печатал приглашение, так что этой проверки нигде не
// требовалось; теперь любая команда, зовущая wifi_ep, обязана проверять это.
static int sys_wifi_status() {
    seL4_CPtr root_ep = get_local_ipc()->msg[BOOT_ROOT_EP];
    seL4_SetMR(0, SYS_WIFI_STATUS);
    seL4_Call(root_ep, seL4_MessageInfo_new(0, 0, 0, 1));
    return (int)seL4_GetMR(0);
}

static void wifi_print_not_ready(seL4_CPtr console_ep, int status) {
    if (status == 0) sys_puts(console_ep, "wifi_driver is not running — run 'wifi start' first.\n");
    else sys_puts(console_ep, "wifi_driver is still starting up (SDIO/firmware bring-up) — try again shortly.\n");
}

// Возвращает true, если mailbox разблокировался сам (операция реально
// завершилась), false — если пришлось снимать блокировку по таймауту (см.
// использование в "ping": печатать статистику серии имеет смысл только в
// первом случае — во втором никакой статистики в SHM ещё не записано).
static bool wait_for_net_mailbox(seL4_CPtr console_ep, seL4_CPtr timer_ep, int timeout_ms, seL4_Word iface) {
    volatile int* net_mailbox = (volatile int*)(shm_base + net_mailbox_ready_offset(iface));
    int elapsed = 0;
    while (net_mailbox[0] == 1 && elapsed < timeout_ms) {
        sys_sleep(timer_ep, 100);
        elapsed += 100;
    }
    if (net_mailbox[0] == 1) {
        sys_puts(console_ep, "\n[SHELL] Error: Network operation timed out (");
        char buf[10]; int s = timeout_ms / 1000, j = 0;
        if (s == 0) buf[j++] = '0';
        while(s > 0) { buf[j++] = (s % 10) + '0'; s /= 10; }
        while(j > 0) { char c[2] = {buf[--j], 0}; sys_puts(console_ep, c); }
        sys_puts(console_ep, "s). Unblocking shell.\n");
        net_mailbox[0] = 0; // Снимаем блокировку насильно

        // НОВОЕ: Перезапуск зависшего сетевого драйвера!
        sys_puts(console_ep, "[SHELL] Initiating emergency recovery for net_driver...\n");
        sys_recover("net_driver");
        return false;
    }
    return true;
}

static void sys_wait(seL4_CPtr root_ep, int pid) {
    seL4_IPCBuffer *ipc = get_local_ipc();
    ipc->msg[0] = 106; // SYS_WAIT
    ipc->msg[1] = pid;
    seL4_Call(root_ep, seL4_MessageInfo_new(0, 0, 0, 2));
}

static void sys_exit(seL4_CPtr root_ep) {
    seL4_IPCBuffer *ipc = get_local_ipc();
    ipc->msg[0] = 103; // SYS_EXIT
    seL4_Call(root_ep, seL4_MessageInfo_new(0, 0, 0, 1));
    while(1) seL4_Yield();
}

static int sys_shm_get(seL4_CPtr root_ep, int shm_id, seL4_Word vaddr) {
    seL4_IPCBuffer *ipc = get_local_ipc();
    ipc->msg[0] = 107; // SYS_SHM_GET
    ipc->msg[1] = shm_id;
    ipc->msg[2] = vaddr;
    seL4_Call(root_ep, seL4_MessageInfo_new(0, 0, 0, 3));
    return (int)seL4_GetMR(0);
}

static int sys_getpid(seL4_CPtr root_ep) {
    seL4_IPCBuffer *ipc = get_local_ipc();
    ipc->msg[0] = 108; // SYS_GETPID
    seL4_Call(root_ep, seL4_MessageInfo_new(0, 0, 0, 1));
    return (int)seL4_GetMR(0);
}

#define CWD_SIZE 64
#define SHM_TOTAL_SIZE 16384
// Стартовое значение отражает то, куда blk_driver сам переходит при
// монтировании (см. USER_ROOT_DIR/fat32_cd в blk_driver.cpp main()) — иначе
// pwd/приглашение показывали бы "/" пока реальный cwd на сервере уже "/root".
static char current_working_dir[CWD_SIZE] = "/root";
static char arg_buffer[512];

// История команд — только в оперативной памяти (кольцевой буфер), сбрасывается
// при перезапуске shell. Размер строки (64) совпадает с буфером cmd[] в main().
#define CMD_HISTORY_SIZE 16
static char cmd_history[CMD_HISTORY_SIZE][64];
static int cmd_history_count = 0;
static int cmd_history_head = 0; // индекс слота для следующей записи

static void history_push(const char *cmd) {
    if (!cmd || !cmd[0]) return;
    my_strlcpy(cmd_history[cmd_history_head], cmd, (int)sizeof(cmd_history[0]));
    cmd_history_head = (cmd_history_head + 1) % CMD_HISTORY_SIZE;
    if (cmd_history_count < CMD_HISTORY_SIZE) cmd_history_count++;
}

// distance_back: 1 = последняя выполненная команда, 2 = предпоследняя, и т.д.
static const char *history_get(int distance_back) {
    if (distance_back < 1 || distance_back > cmd_history_count) return nullptr;
    int idx = (cmd_history_head - distance_back + CMD_HISTORY_SIZE) % CMD_HISTORY_SIZE;
    return cmd_history[idx];
}

// max_len - полный размер буфера target (включая место под '\0').
// Результат всегда '\0'-терминирован в пределах [0, max_len); при
// переполнении путь молча обрезается, но выхода за границы буфера не происходит.
static void build_absolute_path(char* target, const char* arg, int max_len) {
    if (max_len <= 0) return;
    if (arg[0] == '/') {
        my_strlcpy(target, arg, max_len); // Уже абсолютный
        return;
    }
    int len = my_strlcpy(target, current_working_dir, max_len);
    if (len > 0 && target[len - 1] != '/' && len + 1 < max_len) {
        target[len] = '/';
        target[len + 1] = '\0';
        len++;
    }
    if (len < max_len - 1) {
        my_strlcpy(target + len, arg, max_len - len);
    }
}

static int vfs_syscall(int syscall_num, seL4_CPtr blk_ep) {
    vfs_lock();
    
    seL4_SetMR(0, syscall_num);
    seL4_MessageInfo_t info = seL4_MessageInfo_new(0, 0, 0, 1);
    seL4_Call(blk_ep, info);
    
    int ret_val = seL4_GetMR(0);
    vfs_unlock();
    return ret_val;
}

// --- "Знакомые сети" Wi-Fi (Милстоун 4.4, см. wifi_driver.cpp) ---
// Формат PATH_WIFI_PQW: одна сеть на строку, "имясети|пароль". Всё через
// тот же синхронный blk_ep-протокол (SYS_TOUCH=112/SYS_READ_TEXT_FILE=114/
// SYS_WRITE_FILE=113), что уже используют "touch"/"cat"/"echo >" — блокирует
// только сам обработчик команды "wifi ..." в шелле (однопоточном, обрабатывает
// одну команду за раз в любом случае), а не фоновый цикл какого-то ДРУГОГО
// процесса — это другой класс проблемы, чем тот, что решался mailbox-паттерном
// в net_driver.cpp (там блокировался чужой поллинг-цикл). Поэтому здесь
// достаточно обычного vfs_syscall()/vfs_lock(), без seL4_Send+mailbox.

// Создаёт PATH_WIFI_PQW, если файла ещё нет. Возвращает false только при
// ошибке самого создания (SYS_TOUCH), не при "файл уже существует".
static bool wifi_pqw_ensure_file(seL4_CPtr console_ep, seL4_CPtr blk_ep, bool verbose) {
    build_absolute_path(shm_base, PATH_WIFI_PQW, SHM_TOTAL_SIZE);
    if (vfs_syscall(114, blk_ep) == 0) { // SYS_READ_TEXT_FILE — уже существует
        if (verbose) sys_puts(console_ep, "[WIFI][PQW] файл знакомых сетей уже существует.\n");
        return true;
    }
    build_absolute_path(shm_base, PATH_WIFI_PQW, SHM_TOTAL_SIZE);
    bool ok = vfs_syscall(112, blk_ep) == 0; // SYS_TOUCH
    if (verbose) {
        sys_puts(console_ep, ok ? "[WIFI][PQW] файл отсутствовал, создан пустым.\n"
                                : "[WIFI][PQW] файл отсутствовал, ОШИБКА создания.\n");
    }
    return ok;
}

// Читает PATH_WIFI_PQW целиком в out_buf. false, если файла нет/пуст на чтение.
static bool wifi_pqw_read_all(seL4_CPtr blk_ep, char* out_buf, int out_cap) {
    build_absolute_path(shm_base, PATH_WIFI_PQW, SHM_TOTAL_SIZE);
    if (vfs_syscall(114, blk_ep) != 0) { out_buf[0] = '\0'; return false; }
    my_strlcpy(out_buf, shm_base, out_cap);
    return true;
}

// Ищет строку вида "ssid|пароль" в filedata; при находке копирует пароль в
// pass_out. Строки разделены '\n' (как их пишет wifi_pqw_rewrite() ниже).
static bool wifi_pqw_find(const char* filedata, const char* ssid, char* pass_out, int pass_cap) {
    int ssid_len = (int)my_strlen(ssid);
    const char* line = filedata;
    while (*line) {
        const char* line_end = line;
        while (*line_end && *line_end != '\n') line_end++;

        bool match = (int)(line_end - line) > ssid_len && line[ssid_len] == '|';
        for (int i = 0; match && i < ssid_len; i++) if (line[i] != ssid[i]) match = false;

        if (match) {
            const char* pass_start = line + ssid_len + 1;
            int i = 0;
            while (pass_start + i < line_end && i < pass_cap - 1) { pass_out[i] = pass_start[i]; i++; }
            pass_out[i] = '\0';
            return true;
        }
        line = (*line_end == '\n') ? line_end + 1 : line_end;
    }
    return false;
}

// Перезаписывает PATH_WIFI_PQW целиком (у blk_driver нет append — см.
// net_driver.cpp/platform.h). mode: 0=upsert(ssid,pass) — обновляет пароль,
// если сеть уже известна, иначе добавляет новую строку; 1=remove(ssid) —
// убирает строку сети; 2=очистить всё (ssid/pass игнорируются).
static bool wifi_pqw_rewrite(seL4_CPtr console_ep, seL4_CPtr blk_ep, const char* old_data,
                              int mode, const char* ssid, const char* pass, bool verbose) {
    int out_len = 0;
    bool found = false;
    int ssid_len = ssid ? (int)my_strlen(ssid) : 0;
    int pass_len = pass ? (int)my_strlen(pass) : 0;

    if (mode != 2) {
        const char* line = old_data;
        while (*line) {
            const char* line_end = line;
            while (*line_end && *line_end != '\n') line_end++;
            int line_len = (int)(line_end - line);

            bool match = line_len > ssid_len && line[ssid_len] == '|';
            for (int i = 0; match && i < ssid_len; i++) if (line[i] != ssid[i]) match = false;

            if (match) {
                found = true;
                if (mode == 0 && out_len + ssid_len + 1 + pass_len + 1 < (int)sizeof(g_pqw_new)) {
                    for (int i = 0; i < ssid_len; i++) g_pqw_new[out_len++] = ssid[i];
                    g_pqw_new[out_len++] = '|';
                    for (int i = 0; i < pass_len; i++) g_pqw_new[out_len++] = pass[i];
                    g_pqw_new[out_len++] = '\n';
                } // mode==1 (remove) — просто не копируем эту строку дальше
            } else if (line_len > 0 && out_len + line_len + 1 < (int)sizeof(g_pqw_new)) {
                for (int i = 0; i < line_len; i++) g_pqw_new[out_len++] = line[i];
                g_pqw_new[out_len++] = '\n';
            }

            line = (*line_end == '\n') ? line_end + 1 : line_end;
        }
        if (mode == 0 && !found && out_len + ssid_len + 1 + pass_len + 1 < (int)sizeof(g_pqw_new)) {
            for (int i = 0; i < ssid_len; i++) g_pqw_new[out_len++] = ssid[i];
            g_pqw_new[out_len++] = '|';
            for (int i = 0; i < pass_len; i++) g_pqw_new[out_len++] = pass[i];
            g_pqw_new[out_len++] = '\n';
        }
    }
    g_pqw_new[out_len] = '\0';

    build_absolute_path(shm_base, PATH_WIFI_PQW, 128);
    my_strlcpy(shm_base + 128, g_pqw_new, SHM_TOTAL_SIZE - 128);

    vfs_lock();
    seL4_SetMR(0, 113); // SYS_WRITE_FILE
    seL4_SetMR(1, (seL4_Word)out_len);
    seL4_Call(blk_ep, seL4_MessageInfo_new(0, 0, 0, 2));
    int ret_val = (int)seL4_GetMR(0);
    vfs_unlock();

    if (verbose) {
        sys_puts(console_ep, ret_val == 0 ? "[WIFI][PQW] файл знакомых сетей обновлён.\n"
                                           : "[WIFI][PQW] ОШИБКА записи файла знакомых сетей.\n");
    }
    return ret_val == 0;
}

static void sys_pipe_wr_close(int fd) {
    seL4_SetMR(0, 24); // SYS_PIPE_WR_CLOSE
    seL4_Call(get_local_ipc()->caps_or_badges[fd], seL4_MessageInfo_new(0,0,0,1));
}

static void sys_pipe_close(int fd) {
    seL4_SetMR(0, 25); // SYS_PIPE_CLOSE
    seL4_Call(get_local_ipc()->caps_or_badges[fd], seL4_MessageInfo_new(0,0,0,1));
    get_local_ipc()->caps_or_badges[fd] = 0; // Invalidate local FD
}

void ls_thread_func(seL4_Word _timer_ep, seL4_Word _console_ep, seL4_Word blk_ep) {
    // CRITICAL: Initialize libsel4's IPC buffer for this thread.
    seL4_Word tls_addr;
    asm volatile("mrs %0, tpidr_el0" : "=r" (tls_addr));
    seL4_SetIPCBuffer((seL4_IPCBuffer*)(tls_addr - 1024));

    // The shell has already placed the target path into shm_base.
    // We just need to call the VFS syscall.
    vfs_syscall(110, blk_ep); // SYS_LS

    // The result is now in shm_base. Write it to our stdout (the pipe).
    sys_write(1, shm_base);

    // Signal end of data to the reader.
    sys_pipe_wr_close(1);

    // Terminate the thread.
    sys_thread_exit();
}

void grep_thread_func(const char* pattern) {
    // CRITICAL: Initialize libsel4's IPC buffer for this thread.
    seL4_Word tls_addr;
    asm volatile("mrs %0, tpidr_el0" : "=r" (tls_addr));
    seL4_SetIPCBuffer((seL4_IPCBuffer*)(tls_addr - 1024));

    if (!pattern) {
        sys_write(2, "grep: missing pattern\n"); // Write to stderr
        sys_thread_exit();
        return;
    }

    char line_buf[256];
    int line_pos = 0;

    while (1) {
        char c = sys_read_fd(0); // Read from pipe (stdin)

        if (c == '\n' || c == '\0') {
            if (line_pos > 0) {
                line_buf[line_pos] = '\0';
                if (my_strstr(line_buf, pattern)) {
                    sys_write(1, line_buf);
                    sys_write(1, "\n");
                }
                line_pos = 0;
            }
            if (c == '\0') {
                break; // EOF
            }
        } else if (line_pos < sizeof(line_buf) - 1) {
            line_buf[line_pos++] = c;
        }
    }
    
    // Корректно завершаем поток
    sys_thread_exit();
}

// --- Точка входа ---
int main(int argc, char *argv[]) {
    seL4_IPCBuffer *ipc = get_local_ipc();
    
    seL4_SetIPCBuffer(ipc);

    // 2. Теперь безопасно получаем root_ep
    seL4_CPtr root_ep    = ipc->msg[BOOT_ROOT_EP];  
    seL4_CPtr console_ep = ipc->msg[BOOT_CONSOLE_EP]; 
    seL4_CPtr timer_ep   = ipc->msg[BOOT_TIMER_EP];  
    seL4_CPtr my_ep      = ipc->msg[BOOT_TIMER_EP];        
    seL4_CPtr net_ep     = ipc->msg[BOOT_NET_EP];
    seL4_CPtr blk_ep     = ipc->msg[BOOT_BLK_EP];
    seL4_CPtr wifi_ep    = ipc->msg[BOOT_WIFI_EP]; // Фаза 4, Милстоун 4.1 (см. wifi_driver.cpp)

    if (my_ep == 0) {
        __assert_fail("FATAL: Null Capability #0 Detected!", __FILE__, __LINE__, __func__);
    }

    // Ждем, пока все остальные модули (uart/timer/blk/net) не отрапортуют о
    // готовности рутсерверу (SYS_DRIVER_READY) — иначе собственный баннер и
    // первое приглашение оболочки попадают в лог раньше их логов инициализации.
    seL4_SetMR(0, SYS_WAIT_ALL_DRIVERS_READY);
    seL4_Call(root_ep, seL4_MessageInfo_new(0, 0, 0, 1));

    // --- ДИНАМИЧЕСКИЙ ЗАПРОС SHM ---
    seL4_SetMR(0, 107); // SYS_SHM_GET
    seL4_MessageInfo_t msg = seL4_MessageInfo_new(0, 0, 0, 1);
    seL4_Call(root_ep, msg);

    shm_base = (char*)seL4_GetMR(0);
    // Physical address is not needed by the shell, only by DMA-capable drivers.

    // Общий межпроцессный спинлок на офсет 0/128 разделяемой SHM (16КБ, те же
    // физические страницы у shell/rootserver/blk_driver/net_driver — см.
    // SYS_SHM_GET). Раньше vfs_spinlock_ptr никогда не присваивался — все
    // вызовы vfs_lock()/vfs_unlock() ниже были тихими no-op'ами. Пока это не
    // было заметно: shell был единственным, кто писал в офсет 0/128. Как
    // только net_driver.cpp сам стал писать туда же (net_log_flush(), журнал
    // /root/net_udp.log — фоновая, независимая от команд шелла активность),
    // гонка стала реальной: `ps` (пишет в тот же офсет 0 через rootserver)
    // мог получить обратно "/root/net_udp.log" вместо таблицы процессов,
    // если net_driver успевал перезаписать буфер между ответом рутсервера и
    // чтением его шеллом. Офсет 4096 — сразу после последнего занятого поля
    // net_driver'а (ping-статистика, 4068..4092, см. net_driver.cpp).
    vfs_spinlock_ptr = (volatile int*)(shm_base + 4096);

    if (shm_base == nullptr) {
        sys_puts(console_ep, "[SHELL] FATAL: Failed to get dynamic SHM!\n");
        volatile int* boom = (volatile int*)0x0; *boom = 0; 
    }

    my_strlcpy(arg_buffer, (char*)&ipc->msg[0], (int)sizeof(arg_buffer));

    int cmd_argc = 1;
    char *cmd_argv[16];
    cmd_argv[0] = (char*)"shell";
    
    char *cmd_args = arg_buffer; 
    
    if (cmd_args[0] != '\0') {
        cmd_argv[cmd_argc++] = cmd_args;
        
        while (*cmd_args) {
            if (*cmd_args == ' ') {
                *cmd_args = '\0';
                
                while (*(cmd_args + 1) == ' ') cmd_args++;
                
                if (*(cmd_args + 1) != '\0') {
                    cmd_argv[cmd_argc++] = cmd_args + 1;
                }
            }
            cmd_args++;
            if (cmd_argc >= 15) break;
        }
    }
    cmd_argv[cmd_argc] = nullptr;
    
    sys_puts(console_ep, "\n=================================================\n"
                          "  All modules online.\n"
                          "=================================================\n\n");

    // 4. Демонстрация (заменено на cmd_argc/cmd_argv)
    if (cmd_argc > 1) {
        sys_puts(console_ep, "[SHELL] Started with arguments:\n");
        for (int j = 0; j < cmd_argc; j++) {
            sys_puts(console_ep, "  argv[");
            char buf[2] = {(char)(j + '0'), 0}; sys_puts(console_ep, buf);
            sys_puts(console_ep, "] = ");
            sys_puts(console_ep, cmd_argv[j]);
            sys_puts(console_ep, "\n");
        }
    }

    // 5. Парсинг флагов (заменено на cmd_argc/cmd_argv)
    bool is_daemon = false;
    for (int j = 1; j < cmd_argc; j++) {
        if (my_strcmp(cmd_argv[j], "--daemon") == 0) {
            is_daemon = true;
            break;
        }
    }

    if (is_daemon) {
        sys_puts(console_ep, "[Daemon] Mode engaged. TTY input disabled.\n");
        int ticks = 0;

        while (1) {
            sys_sleep(timer_ep, 10000);
            sys_puts(console_ep, "\n[Daemon] Heartbeat tick: ");
            char buf[16]; int temp = ++ticks, k = 0;
            if (temp == 0) buf[k++] = '0';
            while(temp > 0) { buf[k++] = (temp % 10) + '0'; temp /= 10; }
            while(k > 0) { char c[2] = {buf[--k], 0}; sys_puts(console_ep, c); }
            sys_puts(console_ep, "\n");
        }
    }
    // ==========================================================
    // Запрашиваем свой PID у Rootserver'а
    int my_pid = sys_getpid(root_ep);
    
    while (1) {
        char prompt[128];
        my_strcpy(prompt, "sandbox[");
        
        int temp_pid = my_pid, p_idx = 0;
        char pid_buf[8];
        if (temp_pid == 0) pid_buf[p_idx++] = '0';
        while (temp_pid > 0) { pid_buf[p_idx++] = (temp_pid % 10) + '0'; temp_pid /= 10; }
        
        int len = my_strlen(prompt);
        while (p_idx > 0) { prompt[len++] = pid_buf[--p_idx]; }
        
        prompt[len++] = ']'; prompt[len++] = ' ';
        // Добавляем текущую директорию! (с запасом под "> \0" ниже)
        len += my_strlcpy(prompt + len, current_working_dir, (int)sizeof(prompt) - len - 3);
        prompt[len++] = '>'; prompt[len++] = ' '; prompt[len] = '\0';
        
        sys_puts(console_ep, prompt);
        sys_flush(console_ep); // <--- СБРОС: чтобы prompt появился мгновенно!
        
        char cmd[64]; int i = 0;
        int cur = 0;             // позиция курсора внутри cmd, 0..i (стрелки влево/вправо двигают её)
        int hist_nav = 0;        // 0 = не листаем историю; иначе "N команд назад от последней"
        char saved_line[64];     // то, что было набрано до первого Up — восстанавливается по Down
        saved_line[0] = '\0';

        // 2. Читаем ввод: печатные символы (вставка по курсору), Backspace,
        // и ANSI-стрелки ESC '[' 'A'/'B'/'C'/'D' (up/down/right/left).
        while (i < 63) {
            char c = sys_read_blocking(console_ep);

            if (c == '\r' || c == '\n') { sys_puts(console_ep, "\n"); break; }

            else if (c == 27) { // ESC — читаем ещё 2 байта, чтобы распознать стрелку
                char c1 = sys_read_blocking(console_ep);
                if (c1 != '[') continue;
                char c2 = sys_read_blocking(console_ep);

                if (c2 == 'D') { // Влево
                    if (cur > 0) { cur--; sys_puts(console_ep, "\x1b[D"); sys_flush(console_ep); }
                }
                else if (c2 == 'C') { // Вправо
                    if (cur < i) { cur++; sys_puts(console_ep, "\x1b[C"); sys_flush(console_ep); }
                }
                else if (c2 == 'A' || c2 == 'B') { // Вверх/вниз — навигация по истории команд
                    if (c2 == 'A') {
                        if (hist_nav == 0) my_strlcpy(saved_line, cmd, (int)sizeof(saved_line));
                        if (hist_nav < cmd_history_count) hist_nav++;
                    } else {
                        if (hist_nav > 0) hist_nav--;
                    }
                    const char *new_line = (hist_nav == 0) ? saved_line : history_get(hist_nav);

                    // Стираем текущую строку на терминале (курсор к началу, затем до конца строки)
                    // и печатаем выбранную из истории команду взамен.
                    if (cur > 0) { sys_puts(console_ep, "\x1b["); sys_putdec((seL4_Word)cur); sys_puts(console_ep, "D"); }
                    sys_puts(console_ep, "\x1b[K");

                    i = my_strlcpy(cmd, new_line, (int)sizeof(cmd));
                    cur = i;
                    sys_puts(console_ep, cmd);
                    sys_flush(console_ep);
                }
            }

            else if (c == 127 || c == '\b') {
                if (cur > 0) {
                    hist_nav = 0;
                    for (int k = cur - 1; k < i - 1; k++) cmd[k] = cmd[k + 1];
                    i--; cur--;
                    cmd[i] = '\0';

                    sys_puts(console_ep, "\b");      // курсор на позицию удаляемого символа
                    sys_puts(console_ep, cmd + cur);  // перепечатать хвост строки после удаления
                    sys_puts(console_ep, " ");        // затереть "хвост" от старой, более длинной строки
                    int back = (i - cur) + 1;
                    sys_puts(console_ep, "\x1b["); sys_putdec((seL4_Word)back); sys_puts(console_ep, "D");
                    sys_flush(console_ep);
                }
            }

            else if (c >= 32 && c <= 126) {
                hist_nav = 0;
                for (int k = i; k > cur; k--) cmd[k] = cmd[k - 1];
                cmd[cur] = c;
                i++; cur++;
                cmd[i] = '\0';

                sys_puts(console_ep, cmd + cur - 1); // новый символ + всё, что после него по курсору
                int back = i - cur;
                if (back > 0) { sys_puts(console_ep, "\x1b["); sys_putdec((seL4_Word)back); sys_puts(console_ep, "D"); }
                sys_flush(console_ep);
            }
        }
        cmd[i] = '\0';

        if (i > 0) {
            history_push(cmd);

            // НОВОЕ: Пропускаем пробелы в начале команды
            char *cmd_ptr = cmd;
            while (*cmd_ptr == ' ') cmd_ptr++;

            // --- НОВЫЙ ПАРСЕР КОНВЕЙЕРОВ ---
            int left_pid = -1;
            int right_pid = -1;

            int pipe_fd = -1;
            seL4_CPtr pipe_cap = 0;

            char *pipe_sym = cmd_ptr;
            while (*pipe_sym && *pipe_sym != '|') pipe_sym++;
            
            char cmd2[64];
            cmd2[0] = '\0';
            
            if (*pipe_sym == '|') {
                *pipe_sym = '\0'; // Отрезаем левую команду
                char *right_cmd = pipe_sym + 1;
                while (*right_cmd == ' ') right_cmd++; // Убираем пробелы
                my_strcpy(cmd2, right_cmd);
                
                // Убираем пробел в конце левой команды
                char *left_end = pipe_sym - 1;
                while (left_end >= cmd_ptr && *left_end == ' ') {
                    *left_end = '\0';
                    left_end--;
                }

                seL4_SetMR(0, 20); // SYS_PIPE
                seL4_SetMR(1, PIPE_FD_SLOT); // Просим ядро заминтить capability в наш слот пайпа
                seL4_Call(root_ep, seL4_MessageInfo_new(0, 0, 0, 2));
                pipe_fd = seL4_GetMR(0);

                // Защита от кривых ответов ядра и исчерпания IPC-буфера
                if (pipe_fd < 0 || pipe_fd >= 32) {
                    sys_puts(console_ep, "shell: failed to create pipe or invalid FD returned\n");
                    is_piping = false;
                } else {
                    is_piping = true;
                    // 🔥 ТОТ САМЫЙ ФИКС: ЗАПИСЫВАЕМ НОМЕР СЛОТА В НАШУ FD-ТАБЛИЦУ! 🔥
                    // В seL4 Capability Pointer (CPtr) — это и есть номер слота (индекс).
                    // Раз ядро положило cap в слот pipe_fd, значит CPtr равен pipe_fd!
                    ipc->caps_or_badges[pipe_fd] = pipe_fd;

                    // Теперь мы можем безопасно читать его для передачи потомкам
                    pipe_cap = ipc->caps_or_badges[pipe_fd];
                }
            } else {
                is_piping = false;
            }
            char *arg = cmd_ptr; while (*arg && *arg != ' ') arg++;
            if (*arg == ' ') { *arg = '\0'; arg++; while (*arg == ' ') arg++; } else { arg = nullptr; }

            char *shm = shm_base; // Адрес разделяемой памяти (Shared Memory)

            if (my_strcmp(cmd_ptr, "time") == 0) {
                seL4_Word current = sys_get_time(timer_ep);
                sys_puts(console_ep, "Time: "); sys_putdec(current); sys_puts(console_ep, " ms since epoch\n");
            }

            else if (my_strcmp(cmd_ptr, "uptime") == 0) {
                seL4_Word ms = sys_get_uptime(timer_ep);
                seL4_Word total_s = ms / 1000;
                seL4_Word days = total_s / 86400;
                seL4_Word hours = (total_s % 86400) / 3600;
                seL4_Word mins = (total_s % 3600) / 60;
                seL4_Word secs = total_s % 60;
                sys_puts(console_ep, "up ");
                if (days > 0) { sys_putdec(days); sys_puts(console_ep, "d "); }
                sys_putdec(hours); sys_puts(console_ep, "h ");
                sys_putdec(mins); sys_puts(console_ep, "m ");
                sys_putdec(secs); sys_puts(console_ep, "s\n");
            }

            else if (my_strcmp(cmd_ptr, "date") == 0) {
                seL4_Word epoch_ms = sys_get_time(timer_ep);
                print_localtime(console_ep, epoch_ms, TZ_OFFSET_HOURS);
            }

            else if (my_strcmp(cmd_ptr, "temp") == 0) {
                int32_t temp_mC;
                if (sys_get_temp_mC(timer_ep, &temp_mC)) {
                    sys_puts(console_ep, "CPU: "); print_temp_c(console_ep, temp_mC); sys_puts(console_ep, "\n");
                } else {
                    sys_puts(console_ep, "temp: sensor not ready yet\n");
                }
            }

            else if (my_strcmp(cmd_ptr, "mboxprobe") == 0) {
                // Таймбоксед-расследование из ROADMAP.md 4.6: пробует несколько
                // вариантов bus-адреса на безобидном теге, печатает подробности
                // в консоль по ходу (см. timer_driver.cpp mbox_probe()).
                int variant = sys_mbox_probe(timer_ep);
                if (variant < 0) {
                    sys_puts(console_ep, "mboxprobe: mailbox не ответил ни на один вариант — см. ROADMAP.md 4.6\n");
                } else {
                    sys_puts(console_ep, "mboxprobe: mailbox ЖИВ (сработавший вариант напечатан выше)\n");
                }
            }

            else if (my_strcmp(cmd_ptr, "sleep") == 0) {
                seL4_Word ms = arg ? (seL4_Word)simple_atoi(arg) : 3000; // без аргумента — 3с по умолчанию
                sys_puts(console_ep, "Sleeping "); sys_putdec(ms); sys_puts(console_ep, " ms...\n");
                sys_sleep(timer_ep, ms);
                sys_puts(console_ep, "Woke up!\n");
            }

            else if (my_strcmp(cmd_ptr, "ping") == 0) {
                if (!arg) { sys_puts(console_ep, "Usage: ping [-W] <domain_or_ip> [count]\n"); continue; }

                char *cursor = arg;
                seL4_Word iface;
                if (!resolve_iface(console_ep, &cursor, &iface)) continue;
                char *target_str = next_token(&cursor);
                char *count_str = next_token(&cursor);
                uint8_t ip[4];
                uint16_t count = 1;

                if (count_str && parse_port(count_str, &count) != 0) count = 1;

                volatile int* net_mailbox = (volatile int*)(shm_base + net_mailbox_ready_offset(iface));

                // Пытаемся распарсить как IP. Если не вышло — это домен!
                if (parse_ipv4(target_str, ip) != 0) {
                    sys_puts(console_ep, "[SHELL] Target looks like a domain. Starting DNS resolution...\n");
                    net_mailbox[0] = 1;
                    net_send_text_command(net_ep, NET_CMD_RESOLVE, 0, 0, target_str, iface);

                    // ВАЖНО: используем именно возвращаемое значение, а не
                    // "net_mailbox[0] == 0" — wait_for_net_mailbox() сама
                    // насильно обнуляет mailbox и при таймауте тоже, так что
                    // эта проверка всегда была бы true и читала бы протухший
                    // IP от предыдущего успешного resolve (если был).
                    if (!wait_for_net_mailbox(console_ep, timer_ep, 10000, iface)) {
                        sys_puts(console_ep, "[SHELL] DNS Resolution failed.\n");
                        continue;
                    }

                    seL4_Word packed_ip = *((seL4_Word*)(shm_base + net_mailbox_dns_ip_offset(iface)));
                    if (packed_ip == 0) {
                        sys_puts(console_ep, "[SHELL] DNS Error: Domain not found.\n");
                        continue;
                    }
                    ip[0] = (packed_ip >> 24) & 0xFF; ip[1] = (packed_ip >> 16) & 0xFF;
                    ip[2] = (packed_ip >> 8) & 0xFF;  ip[3] = packed_ip & 0xFF;
                }

                // Теперь у нас точно есть IP (распарсенный или полученный от DNS)
                sys_puts(console_ep, "PING "); sys_puts(console_ep, target_str);
                sys_puts(console_ep, " ("); print_ip(console_ep, ip); sys_puts(console_ep, ") 56(84) bytes of data.\n");

                net_mailbox[0] = 1;
                net_send_text_command(net_ep, NET_CMD_PING, pack_ipv4(ip), count, nullptr, iface);

                int timeout = 5000;
                if (count * 2000 + 2000 > timeout) timeout = count * 2000 + 2000;

                // Статистику серии (см. net_driver.cpp: net_schedule_next_ping)
                // печатаем только если mailbox разблокировался сам — при
                // вынужденном таймауте net_driver ничего в SHM ещё не записал.
                if (wait_for_net_mailbox(console_ep, timer_ep, timeout, iface)) {
                    uint32_t stats_off = net_mailbox_ping_stats_offset(iface);
                    uint32_t sent    = *((uint32_t*)(shm_base + stats_off + 0));
                    uint32_t reply   = *((uint32_t*)(shm_base + stats_off + 4));
                    uint32_t min_us  = *((uint32_t*)(shm_base + stats_off + 8));
                    uint32_t max_us  = *((uint32_t*)(shm_base + stats_off + 12));
                    uint32_t avg_us  = *((uint32_t*)(shm_base + stats_off + 16));
                    uint32_t mdev_us = *((uint32_t*)(shm_base + stats_off + 20));
                    uint32_t elapsed = *((uint32_t*)(shm_base + stats_off + 24));
                    uint32_t loss_pct = sent > 0 ? ((sent - reply) * 100) / sent : 0;

                    sys_puts(console_ep, "\n--- "); sys_puts(console_ep, target_str);
                    sys_puts(console_ep, " ping statistics ---\n");
                    sys_putdec(sent); sys_puts(console_ep, " packets transmitted, ");
                    sys_putdec(reply); sys_puts(console_ep, " received, ");
                    sys_putdec(loss_pct); sys_puts(console_ep, "% packet loss, time ");
                    sys_putdec(elapsed); sys_puts(console_ep, "ms\n");

                    if (reply > 0) {
                        sys_puts(console_ep, "rtt min/avg/max/mdev = ");
                        print_rtt_ms(console_ep, min_us); sys_puts(console_ep, "/");
                        print_rtt_ms(console_ep, avg_us); sys_puts(console_ep, "/");
                        print_rtt_ms(console_ep, max_us); sys_puts(console_ep, "/");
                        print_rtt_ms(console_ep, mdev_us); sys_puts(console_ep, " ms\n");
                    }
                }
            }

            else if (my_strcmp(cmd_ptr, "send") == 0) {
                if (!arg) { sys_puts(console_ep, "Usage: send [-W] <text>\n"); continue; }
                if (net_ep == 0) { sys_puts(console_ep, "Net driver endpoint is unavailable.\n"); continue; }

                char *cursor = arg;
                seL4_Word iface;
                if (!resolve_iface(console_ep, &cursor, &iface)) continue; // остаток cursor'а — сообщение как есть, с пробелами
                uint8_t ip[4] = {10, 0, 2, 2};

                volatile int* net_mailbox = (volatile int*)(shm_base + net_mailbox_ready_offset(iface));
                net_mailbox[0] = 1; // Запираем Mailbox!

                sys_puts(console_ep, "UDP datagram queued for 10.0.2.2:8080.\n");
                net_send_text_command(net_ep, NET_CMD_SEND, pack_ipv4(ip), 8080, cursor, iface);

                wait_for_net_mailbox(console_ep, timer_ep, 5000, iface);
            }

            else if (my_strcmp(cmd_ptr, "sendto") == 0) {
                if (!arg) { sys_puts(console_ep, "Usage: sendto [-W] <ip_address> <port> <text>\n"); continue; }
                if (net_ep == 0) { sys_puts(console_ep, "Net driver endpoint is unavailable.\n"); continue; }

                char *cursor = arg;
                seL4_Word iface;
                if (!resolve_iface(console_ep, &cursor, &iface)) continue;
                char *ip_str = next_token(&cursor);
                char *port_str = next_token(&cursor);
                char *text = cursor;
                while (text && *text == ' ') text++;

                uint8_t ip[4];
                uint16_t port = 0;
                if (!ip_str || parse_ipv4(ip_str, ip) != 0) {
                    sys_puts(console_ep, "Invalid IPv4 address.\n");
                    continue;
                }
                if (!port_str || parse_port(port_str, &port) != 0) {
                    sys_puts(console_ep, "Invalid UDP port.\n");
                    continue;
                }
                if (!text || text[0] == '\0') {
                    sys_puts(console_ep, "Usage: sendto [-W] <ip_address> <port> <text>\n");
                    continue;
                }

                volatile int* net_mailbox = (volatile int*)(shm_base + net_mailbox_ready_offset(iface));
                net_mailbox[0] = 1; // Запираем Mailbox!

                sys_puts(console_ep, "UDP datagram queued.\n");
                net_send_text_command(net_ep, NET_CMD_SEND, pack_ipv4(ip), port, text, iface);

                wait_for_net_mailbox(console_ep, timer_ep, 5000, iface);
            }

            else if (my_strcmp(cmd_ptr, "netstat") == 0) {
                if (net_ep == 0) { sys_puts(console_ep, "Net driver endpoint is unavailable.\n"); continue; }

                // По просьбе пользователя — единый отчёт по обоим интерфейсам
                // сразу (2 строки), без -i: GENET и Wi-Fi полностью
                // равноправны в остальных командах, но netstat — это просто
                // статус, а не действие над ОДНИМ конкретным интерфейсом,
                // поэтому смысла выбирать только один тут нет.
                sys_puts(console_ep, "Net status requested.\n");
                seL4_Word ifaces[2] = { NET_IFACE_GENET, NET_IFACE_WIFI };
                for (int i = 0; i < 2; i++) {
                    volatile int* net_mailbox = (volatile int*)(shm_base + net_mailbox_ready_offset(ifaces[i]));
                    net_mailbox[0] = 1; // Запираем Mailbox!
                    net_send_text_command(net_ep, NET_CMD_STATUS, 0, 0, nullptr, ifaces[i]);
                    wait_for_net_mailbox(console_ep, timer_ep, 2000, ifaces[i]); // Для статуса достаточно 2-х секунд
                }
            }

            else if (my_strcmp(cmd_ptr, "recv") == 0) {
                if (net_ep == 0) { sys_puts(console_ep, "Net driver endpoint is unavailable.\n"); continue; }

                char *cursor = arg;
                seL4_Word iface;
                if (!resolve_iface(console_ep, &cursor, &iface)) continue;
                volatile int* net_mailbox = (volatile int*)(shm_base + net_mailbox_ready_offset(iface));
                net_mailbox[0] = 1; // Запираем Mailbox!

                net_send_text_command(net_ep, NET_CMD_RECV, 0, 0, nullptr, iface);

                wait_for_net_mailbox(console_ep, timer_ep, 2000, iface);
            }

            else if (my_strcmp(cmd_ptr, "ntp") == 0) {
                if (net_ep == 0) { sys_puts(console_ep, "Net driver endpoint is unavailable.\n"); continue; }

                char *cursor = arg;
                seL4_Word iface;
                if (!resolve_iface(console_ep, &cursor, &iface)) continue;
                volatile int* net_mailbox = (volatile int*)(shm_base + net_mailbox_ready_offset(iface));
                net_mailbox[0] = 1; // Запираем Mailbox!

                sys_puts(console_ep, "Requesting NTP time sync...\n");
                net_send_text_command(net_ep, NET_CMD_NTP, 0, 0, nullptr, iface);

                // ARP (если еще не разрешен) + запрос-ответ через интернет — щедрый запас.
                wait_for_net_mailbox(console_ep, timer_ep, 5000, iface);
            }

            else if (my_strcmp(cmd_ptr, "wifiprobe") == 0) {
                // Фаза 4, Милстоун 4.1 (см. wifi_driver.cpp) — диагностика
                // SDIO-хост-бринг-апа. В отличие от net-команд выше, это
                // мгновенный синхронный запрос (просто отдаёт последние
                // сохранённые значения CMD5/CMD52), поэтому seL4_Call вместо
                // seL4_Send+mailbox-poll. wifi_driver теперь стартует вручную
                // ("wifi start") — без проверки статуса этот seL4_Call
                // повис бы навсегда, если никто не слушает wifi_ep.
                if (wifi_ep == 0) { sys_puts(console_ep, "Wi-Fi driver endpoint is unavailable (RPI4_ENABLE_WIFI=false?).\n"); continue; }
                int probe_status = sys_wifi_status();
                if (probe_status != 2) { wifi_print_not_ready(console_ep, probe_status); continue; }

                seL4_SetMR(0, 1); // WIFI_CMD_PROBE_STATUS
                seL4_SetMR(1, 0); // "-l" не поддерживается этой командой (нет протокольной активности, нечего скрывать/показывать) — но MR1 обязателен: wifi_driver безусловно читает его как бит подробности для ЛЮБОЙ команды.
                seL4_Call(wifi_ep, seL4_MessageInfo_new(0, 0, 0, 2));
                seL4_Word status   = seL4_GetMR(0);
                seL4_Word ocr      = seL4_GetMR(1);
                seL4_Word cccr     = seL4_GetMR(2);
                seL4_Word fw_alive = seL4_GetMR(3);
                seL4_Word shaddr   = seL4_GetMR(4);
                seL4_Word sdpcm_ok = seL4_GetMR(5);

                sys_puts(console_ep, status == 0 ? "SDIO probe: OK\n" : "SDIO probe: FAILED (see boot log)\n");
                sys_puts(console_ep, "  CMD5 OCR:  0x"); sys_puthex(ocr);  sys_puts(console_ep, "\n");
                sys_puts(console_ep, "  F0 CCCR:   0x"); sys_puthex(cccr); sys_puts(console_ep, "\n");
                // Милстоун 4.2 (backplane/прошивка) — см. wifi_driver.cpp.
                sys_puts(console_ep, fw_alive ? "  Firmware:  ALIVE\n" : "  Firmware:  not alive (see boot log)\n");
                sys_puts(console_ep, "  shaddr:    0x"); sys_puthex(shaddr); sys_puts(console_ep, "\n");
                // Милстоун 4.3 (sdpcm/IOCTL) — см. wifi_driver.cpp.
                sys_puts(console_ep, sdpcm_ok ? "  sdpcm:     OK (см. лог загрузки — версия прошивки)\n" : "  sdpcm:     FAILED (see boot log)\n");
            }

            else if (my_strcmp(cmd_ptr, "wifi") == 0) {
                static const char *WIFI_USAGE =
                    "Usage: wifi start|stop|restart [-l]\n"
                    "       wifi scan [-l]\n"
                    "       wifi connect <ssid> [password] [-l] [&&save]\n"
                    "       wifi clean [all] [-l]\n";
                if (!arg) { sys_puts(console_ep, WIFI_USAGE); continue; }
                char *cursor = arg;
                char *subcmd = next_token(&cursor);
                if (!subcmd) { sys_puts(console_ep, WIFI_USAGE); continue; }

                // --- start/stop/restart: жизненный цикл процесса wifi_driver
                // (см. common.h SYS_START_WIFI/SYS_STOP_WIFI). Не трогают
                // wifi_ep вообще — идут на root_ep, поэтому безопасны, даже
                // если wifi_driver сейчас не запущен. "-l" — подождать и
                // напечатать финальный статус (до ~20с); без него команда
                // возвращается сразу же, не блокируя шелл (собственные логи
                // бринг-апа wifi_driver и так печатаются в ту же консоль по
                // мере готовности, независимо от -l).
                if (my_strcmp(subcmd, "start") == 0 || my_strcmp(subcmd, "restart") == 0) {
                    bool verbose = false;
                    char *tok;
                    while ((tok = next_token(&cursor)) != nullptr) if (my_strcmp(tok, "-l") == 0) verbose = true;

                    if (wifi_ep == 0) { sys_puts(console_ep, "Wi-Fi driver endpoint is unavailable (RPI4_ENABLE_WIFI=false?).\n"); continue; }

                    if (my_strcmp(subcmd, "restart") == 0) {
                        sys_stop_wifi(); // без эффекта, если уже не запущен
                    }
                    // Бит подробности для ФОНОВОГО bring-up — должен лежать в SHM
                    // ДО спавна процесса (см. WIFI_SHM_VERBOSE_OFFSET, h/platform.h:
                    // main() читает его один раз в самом начале,
                    // до отправки WIFI_CMD_* тут вообще возможно).
                    shm[WIFI_SHM_VERBOSE_OFFSET] = verbose ? 1 : 0;
                    int r = sys_start_wifi();
                    if (r == 0) sys_puts(console_ep, "wifi_driver starting (SDIO/firmware bring-up in background)...\n");
                    else if (r == 1) sys_puts(console_ep, "wifi_driver is already running.\n");
                    else sys_puts(console_ep, "Failed to start wifi_driver.\n");

                    // ВАЖНО: НЕ трогаем /wifi/pqw.txt сразу после свежего "start"
                    // (r==0) — wifi_driver в этот момент ещё вовсю печатает свой
                    // собственный лог бринг-апа в тот же console_ep, и blk_ep-вызов
                    // отсюда (тоже пишущий в console_ep через wifi_pqw_ensure_file)
                    // гонится с ним и рвёт строки друг друга (подтверждено на
                    // живом железе — см. память проекта). Если driver уже был
                    // запущен (r==1), бринг-ап давно закончился — трогать файл
                    // сразу безопасно. Если r==0 и указан -l, ждём готовности
                    // (бринг-ап гарантированно завершён к этому моменту) и только
                    // тогда трогаем файл; без -l полагаемся на то, что "wifi connect"
                    // сделает ту же проверку позже (она уже ждёт status==2 первой).
                    if (r == 1) wifi_pqw_ensure_file(console_ep, blk_ep, verbose);

                    if (verbose && r == 0) {
                        int st = sys_wifi_status();
                        for (int i = 0; i < 100 && st == 1; i++) { sys_sleep(timer_ep, 200); st = sys_wifi_status(); }
                        sys_puts(console_ep, st == 2 ? "wifi_driver ready.\n" : "wifi_driver still not ready (see boot log above).\n");
                        if (st == 2) wifi_pqw_ensure_file(console_ep, blk_ep, verbose);
                    }
                }

                else if (my_strcmp(subcmd, "stop") == 0) {
                    bool verbose = false;
                    char *tok;
                    while ((tok = next_token(&cursor)) != nullptr) if (my_strcmp(tok, "-l") == 0) verbose = true;
                    (void)verbose;

                    int r = sys_stop_wifi();
                    sys_puts(console_ep, r == 0 ? "wifi_driver stopped.\n" : "wifi_driver was not running.\n");
                    g_wifi_current_ssid[0] = '\0';
                }

                // --- scan: переиспользует диагностический слепой escan из
                // wifi_driver.cpp (Милстоун 4.4) — печатает сырые
                // event_type/status, БЕЗ декодирования списка SSID/BSS (это
                // отдельная, более крупная задача).
                else if (my_strcmp(subcmd, "scan") == 0) {
                    bool verbose = false;
                    uint32_t timeout_s = 30; // по умолчанию — прежнее поведение (полный проход 2.4+5ГГц)
                    uint32_t band = 0;       // 0 = оба диапазона (слепой скан, как раньше)
                    char *tok;
                    while ((tok = next_token(&cursor)) != nullptr) {
                        if (my_strcmp(tok, "-l") == 0) {
                            verbose = true;
                        } else if (my_strcmp(tok, "-t") == 0) {
                            char *val = next_token(&cursor);
                            if (val) timeout_s = (uint32_t)simple_atoi(val);
                        } else if (my_strcmp(tok, "-f") == 0) {
                            char *val = next_token(&cursor);
                            if (val) band = (uint32_t)simple_atoi(val);
                        }
                    }
                    if (timeout_s == 0) timeout_s = 30;
                    if (band != 2 && band != 5) band = 0;

                    if (wifi_ep == 0) { sys_puts(console_ep, "Wi-Fi driver endpoint is unavailable (RPI4_ENABLE_WIFI=false?).\n"); continue; }
                    int st = sys_wifi_status();
                    if (st != 2) { wifi_print_not_ready(console_ep, st); continue; }

                    sys_puts(console_ep, "Scanning (");
                    sys_puts(console_ep, band == 0 ? "2.4+5GHz" : (band == 2 ? "2.4GHz only" : "5GHz only"));
                    sys_puts(console_ep, ", up to ~"); sys_putdec(timeout_s); sys_puts(console_ep, "s)...\n");
                    seL4_SetMR(0, 3); // WIFI_CMD_SCAN (см. wifi_driver.cpp)
                    seL4_SetMR(1, verbose ? 1 : 0); // "-l" — подробный лог самого драйвера на время этой команды
                    seL4_SetMR(2, timeout_s);       // "-t" — сколько секунд слушать результаты
                    seL4_SetMR(3, band);             // "-f" — 0=оба диапазона, 2=только 2.4ГГц, 5=только 5ГГц
                    seL4_Call(wifi_ep, seL4_MessageInfo_new(0, 0, 0, 4));
                    seL4_Word scan_status = seL4_GetMR(0);
                    seL4_Word events_seen = seL4_GetMR(1);
                    if (scan_status == 0) {
                        sys_puts(console_ep, "Scan done. Events seen: "); sys_putdec(events_seen); sys_puts(console_ep, "\n");
                    } else {
                        sys_puts(console_ep, "Scan failed (see driver log above).\n");
                    }
                }

                // --- connect <ssid> [password] [-l] [&&save]: как раньше,
                // плюс необязательный пароль (если сеть уже "знакома" — см.
                // /wifi/pqw.txt) и необязательное сохранение в знакомые.
                else if (my_strcmp(subcmd, "connect") == 0) {
                    char *ssid = next_token(&cursor);
                    if (!ssid) { sys_puts(console_ep, WIFI_USAGE); continue; }
                    char *pass = nullptr;
                    bool verbose = false;
                    bool do_save = false;
                    char *tok;
                    while ((tok = next_token(&cursor)) != nullptr) {
                        if (my_strcmp(tok, "-l") == 0) verbose = true;
                        else if (my_strcmp(tok, "&&save") == 0) do_save = true;
                        else if (!pass) pass = tok;
                    }

                    if (wifi_ep == 0) { sys_puts(console_ep, "Wi-Fi driver endpoint is unavailable (RPI4_ENABLE_WIFI=false?).\n"); continue; }
                    int st = sys_wifi_status();
                    if (st != 2) { wifi_print_not_ready(console_ep, st); continue; }

                    wifi_pqw_ensure_file(console_ep, blk_ep, verbose);

                    char known_pass[64];
                    if (!pass) {
                        if (!wifi_pqw_read_all(blk_ep, g_pqw_old, sizeof(g_pqw_old)) ||
                            !wifi_pqw_find(g_pqw_old, ssid, known_pass, sizeof(known_pass))) {
                            sys_puts(console_ep, "Unknown network — provide a password: wifi connect <ssid> <password>\n");
                            continue;
                        }
                        pass = known_pass;
                        if (verbose) sys_puts(console_ep, "[WIFI][PQW] using saved password for known network.\n");
                    }

                    // Сохраняем В ФАЙЛ СРАЗУ по факту запроса "&&save", не
                    // дожидаясь результата подключения — пароль, который ввёл
                    // пользователь, стоит запомнить независимо от того, окажется
                    // ли ТЕКУЩАЯ попытка join неудачной по другой причине (AP
                    // временно недоступна и т.п.), иначе "&&save" пришлось бы
                    // повторять при каждой попытке до первого успеха.
                    if (do_save) {
                        if (wifi_pqw_read_all(blk_ep, g_pqw_old, sizeof(g_pqw_old))) {
                            wifi_pqw_rewrite(console_ep, blk_ep, g_pqw_old, /*mode=*/0, ssid, pass, verbose);
                        }
                    }

                    uint32_t ssid_len = 0; while (ssid[ssid_len] && ssid_len < 32) ssid_len++;
                    uint32_t pass_len = 0; while (pass[pass_len] && pass_len < 63) pass_len++;

                    *(uint32_t*)(shm + WIFI_SHM_SSID_LEN_OFFSET) = ssid_len;
                    for (uint32_t i = 0; i < 32; i++) (shm + WIFI_SHM_SSID_OFFSET)[i] = (i < ssid_len) ? ssid[i] : '\0';
                    *(uint32_t*)(shm + WIFI_SHM_PASS_LEN_OFFSET) = pass_len;
                    for (uint32_t i = 0; i < 64; i++) (shm + WIFI_SHM_PASS_OFFSET)[i] = (i < pass_len) ? pass[i] : '\0';

                    sys_puts(console_ep, "Connecting to \""); sys_puts(console_ep, ssid);
                    sys_puts(console_ep, "\" (WPA2-PSK) — this can take up to ~15s while firmware completes the 4-way handshake...\n");

                    seL4_SetMR(0, 2); // WIFI_CMD_CONNECT
                    seL4_SetMR(1, verbose ? 1 : 0); // "-l" — подробный лог самого драйвера на время этой команды
                    seL4_Call(wifi_ep, seL4_MessageInfo_new(0, 0, 0, 2));
                    seL4_Word result = seL4_GetMR(0);
                    seL4_Word reason = seL4_GetMR(1);

                    if (result == 0) {
                        sys_puts(console_ep, "Connected!\n");
                        my_strlcpy(g_wifi_current_ssid, ssid, sizeof(g_wifi_current_ssid));
                    } else {
                        sys_puts(console_ep, "Connect FAILED (code "); sys_putdec(result);
                        sys_puts(console_ep, ", reason "); sys_putdec(reason);
                        sys_puts(console_ep, ") — see driver log above for details.\n");
                    }
                }

                // --- clean [all] [-l]: убрать сохранённый пароль сети, к
                // которой сейчас подключены ("текущее подключение"), либо
                // все знакомые сети разом ("all").
                else if (my_strcmp(subcmd, "clean") == 0) {
                    bool verbose = false;
                    bool clean_all = false;
                    char *tok;
                    while ((tok = next_token(&cursor)) != nullptr) {
                        if (my_strcmp(tok, "-l") == 0) verbose = true;
                        else if (my_strcmp(tok, "all") == 0) clean_all = true;
                    }

                    if (clean_all) {
                        if (wifi_pqw_rewrite(console_ep, blk_ep, "", /*mode=*/2, nullptr, nullptr, verbose)) {
                            sys_puts(console_ep, "Removed all known networks.\n");
                            g_wifi_current_ssid[0] = '\0';
                        } else {
                            sys_puts(console_ep, "Failed to clear known networks file.\n");
                        }
                    } else if (g_wifi_current_ssid[0] == '\0') {
                        sys_puts(console_ep, "No active connection to clean.\n");
                    } else if (wifi_pqw_read_all(blk_ep, g_pqw_old, sizeof(g_pqw_old)) &&
                               wifi_pqw_rewrite(console_ep, blk_ep, g_pqw_old, /*mode=*/1, g_wifi_current_ssid, nullptr, verbose)) {
                        sys_puts(console_ep, "Removed \""); sys_puts(console_ep, g_wifi_current_ssid);
                        sys_puts(console_ep, "\" from known networks.\n");
                        g_wifi_current_ssid[0] = '\0';
                    } else {
                        sys_puts(console_ep, "Failed to update known networks file.\n");
                    }
                }

                else {
                    sys_puts(console_ep, WIFI_USAGE);
                }
            }

            else if (my_strcmp(cmd_ptr, "ls") == 0) {
                char *shm = shm_base; 
                if (arg) { build_absolute_path(shm, arg, SHM_TOTAL_SIZE); }
                else { build_absolute_path(shm, "", SHM_TOTAL_SIZE); }

                if (is_piping) { 
                    // Запускаем ls в потоке, перенаправив его stdout в пайп
                    left_pid = spawn_thread((seL4_Word)ls_thread_func, (seL4_Word)ls_thread_stack + sizeof(ls_thread_stack) - 16,
                                 timer_ep, console_ep, blk_ep, pipe_fd, ipc->caps_or_badges[0], pipe_cap, ipc->caps_or_badges[2]);
                } else {
                    vfs_syscall(110, blk_ep);
                    sys_puts(console_ep, shm);
                }
            }

            else if (my_strcmp(cmd_ptr, "pwd") == 0) {
                sys_puts(console_ep, current_working_dir);
                sys_puts(console_ep, "\n");
            }

            else if (my_strcmp(cmd_ptr, "mkdir") == 0) {
                if (!arg) {
                    sys_puts(console_ep, "mkdir: missing operand\n");
                    continue;
                }

                char* p = arg;
                while (*p != '\0') {
                    while (*p == ' ') p++;
                    if (*p == '\0') break;

                    char* start_of_arg = p;
                    while (*p != ' ' && *p != '\0') p++;
                    
                    char temp_char = *p;
                    *p = '\0';

                    my_strcpy(shm_base, start_of_arg);
                    vfs_lock();
                    seL4_SetMR(0, 117); // SYS_MKDIR
                    seL4_Call(blk_ep, seL4_MessageInfo_new(0, 0, 0, 1));
                    int ret = seL4_GetMR(0);
                    vfs_unlock();
                    
                    if (ret != 0) {
                        sys_puts(console_ep, "mkdir: cannot create directory '");
                        sys_puts(console_ep, start_of_arg);
                        sys_puts(console_ep, "'\n");
                    }
                    *p = temp_char;
                }
            }

            else if (my_strcmp(cmd_ptr, "cd") == 0) {
                char* path = arg;
                if (!path || path[0] == '\0') {
                    path = (char*)"/";
                }

                my_strcpy(shm_base, path);
                vfs_lock();
                seL4_SetMR(0, 118); // SYS_CD
                seL4_Call(blk_ep, seL4_MessageInfo_new(0, 0, 0, 1));
                int ret = seL4_GetMR(0);
                vfs_unlock();

                if (ret == 0) {
                    if (my_strcmp(path, "/") == 0) {
                        my_strcpy(current_working_dir, "/");
                    } else if (my_strcmp(path, "..") == 0) {
                        int len = my_strlen(current_working_dir);
                        if (len > 1) {
                            len--;
                            if (current_working_dir[len] == '/') len--;
                            while (len > 0 && current_working_dir[len] != '/') len--;
                            if (len == 0) my_strcpy(current_working_dir, "/");
                            else current_working_dir[len] = '\0';
                        }
                    } else {
                        build_absolute_path(current_working_dir, path, CWD_SIZE);
                    }
                } else {
                    sys_puts(console_ep, "cd: ");
                    sys_puts(console_ep, path);
                    sys_puts(console_ep, ": No such file or directory\n");
                }
            }

            else if (my_strcmp(cmd_ptr, "ps") == 0) {
                seL4_IPCBuffer *ipc = get_local_ipc();

                // SYS_PS пишет таблицу процессов в тот же офсет 0 разделяемой
                // SHM, что и net_driver.cpp (net_log_flush, журнал UDP) —
                // держим лок на весь путь "запрос -> ответ рутсервера ->
                // чтение shm", иначе фоновая запись net_driver может
                // перезаписать буфер до того, как мы его прочитаем (см.
                // vfs_spinlock_ptr выше).
                vfs_lock();
                ipc->msg[0] = 104;
                seL4_Call(root_ep, seL4_MessageInfo_new(0, 0, 0, 1));
                if (is_piping) {
                    sys_write(pipe_fd, shm);
                    sys_pipe_wr_close(pipe_fd);
                } else {
                    sys_puts(console_ep, shm);
                }
                vfs_unlock();
            }

            else if (my_strcmp(cmd_ptr, "kill") == 0) {
                seL4_IPCBuffer *ipc = get_local_ipc();
                if (!arg) { sys_puts(console_ep, "Usage: kill <pid>\n"); continue; }
                ipc->msg[0] = 102; // SYS_KILL
                ipc->msg[1] = simple_atoi(arg);
                seL4_Call(root_ep, seL4_MessageInfo_new(0, 0, 0, 2));
                sys_puts(console_ep, "Signal sent.\n");
            }

            else if (my_strcmp(cmd_ptr, "exec") == 0) {
                if (!arg) { sys_puts(console_ep, "Usage: exec <filename> [args] [&]\n"); continue; }
                
                bool run_in_background = false;
                
                // Проверяем, есть ли '&' в конце строки
                int arg_len = my_strlen(arg);
                if (arg_len > 0 && arg[arg_len - 1] == '&') {
                    run_in_background = true;
                    arg[arg_len - 1] = '\0'; // Отрезаем '&'
                    
                    // Убираем возможные пробелы перед '&'
                    arg_len--;
                    while (arg_len > 0 && arg[arg_len - 1] == ' ') {
                        arg[arg_len - 1] = '\0';
                        arg_len--;
                    }
                }

                char safe_name[64] = {0};
                my_strncpy(safe_name, arg, 63);

                seL4_SetMR(0, 100); // SYS_EXEC
                uint64_t* name_ptr = (uint64_t*)safe_name;
                for (int i = 0; i < 8; i++) {
                    seL4_SetMR(i + 1, name_ptr[i]);
                }
                
                seL4_MessageInfo_t msg = seL4_MessageInfo_new(0, 0, 0, 9);
                seL4_Call(root_ep, msg);
                
                int pid = (int)seL4_GetMR(0);
                if (pid > 0) {
                    sys_puts(console_ep, "Spawned process with PID: ");
                    char buf[16]; int temp = pid, j = 0;
                    if (temp == 0) buf[j++] = '0';
                    while(temp > 0) { buf[j++] = (temp % 10) + '0'; temp /= 10; }
                    while(j > 0) { char c[2] = {buf[--j], 0}; sys_puts(console_ep, c); }
                    sys_puts(console_ep, "\n");

                    // === МАГИЯ ФОНОВОГО ВЫПОЛНЕНИЯ ===
                    if (run_in_background) {
                        sys_puts(console_ep, "[Running in background] TTY retained by parent.\n");
                        // Мы ПРОПУСКАЕМ sys_wait! Цикл просто пойдет на следующий круг и выдаст "sandbox>"
                    } else {
                        sys_puts(console_ep, "Parent sleeping, handing over TTY...\n");
                        sys_wait(root_ep, pid);
                        sys_puts(console_ep, "\nChild exited. Parent taking back TTY.\n");
                    }
                }
            }

            else if (my_strcmp(cmd_ptr, "shm") == 0) {
                if (!arg) { sys_puts(console_ep, "Usage: shm <id> <read|write> [text]\n"); continue; }
                
                char *id_str = arg;
                
                // 1. Извлекаем операцию (read или write)
                char *op = id_str; while (*op && *op != ' ') op++;
                if (*op == ' ') { *op = '\0'; op++; } else { sys_puts(console_ep, "Invalid syntax.\n"); continue; }
                
                // 2. Извлекаем текст для записи (отделяем его от op нулевым байтом)
                char *text = op; while (*text && *text != ' ') text++;
                if (*text == ' ') { *text = '\0'; text++; }
                
                int shm_id = simple_atoi(id_str);
                seL4_Word vaddr = 0x580000 + (shm_id * 4096); 
                
                int res = sys_shm_get(root_ep, shm_id, vaddr);
                if (res != 0) {
                    sys_puts(console_ep, "Failed to map Shared Memory.\n");
                    continue;
                }
                
                char *shm_ptr = (char*)vaddr;

                if (my_strcmp(op, "read") == 0) {
                    sys_puts(console_ep, "SHM Content:\n");
                    sys_puts(console_ep, shm_ptr);
                    sys_puts(console_ep, "\n");
                } 

                else if (my_strcmp(op, "write") == 0) {
                    my_strcpy(shm_ptr, text);
                    sys_puts(console_ep, "Written to SHM.\n");
                } else {
                    sys_puts(console_ep, "Operation must be 'read' or 'write'.\n");
                }
            }

            else if (my_strcmp(cmd_ptr, "pid") == 0) {
                sys_puts(console_ep, "Current Shell PID: ");
                char buf[16]; int temp = my_pid, j = 0;
                if (temp == 0) buf[j++] = '0';
                while(temp > 0) { buf[j++] = (temp % 10) + '0'; temp /= 10; }
                while(j > 0) { char c[2] = {buf[--j], 0}; sys_puts(console_ep, c); }
                sys_puts(console_ep, "\n");
            }

            // === КОМАНДА TOUCH (Поддержка бесконечного числа аргументов) ===
            else if (my_strcmp(cmd_ptr, "touch") == 0) {
                char* p = arg;

                // 1. Ошибка: если после пробелов сразу конец строки (нет аргументов)
                if (!p || *p == '\0') {
                    sys_puts(console_ep, "touch: missing file operand\n");
                    continue;
                }

                // 2. Парсим бесконечное количество аргументов
                while (*p != '\0') {
                    // Пропускаем лишние пробелы перед очередным файлом (на случай "touch  a     b")
                    while (*p == ' ') p++;
                    if (*p == '\0') break;

                    char* start_of_arg = p;
                    // Ищем конец имени файла
                    while (*p != ' ' && *p != '\0') p++;
                    
                    char temp_char = *p;
                    *p = '\0'; // Временно обрезаем строку, чтобы получить один аргумент

                    // 3. Отправляем IPC-вызов драйверу диска для ЭТОГО конкретного файла
                    char *shm = shm_base;
                    build_absolute_path(shm, start_of_arg, SHM_TOTAL_SIZE);
                    if (vfs_syscall(112, blk_ep) != 0) {
                        sys_puts(console_ep, "touch: failed to create '");
                        sys_puts(console_ep, start_of_arg);
                        sys_puts(console_ep, "'\n");
                    }
                    *p = temp_char; // Восстанавливаем строку для следующей итерации
                }
            }

            else if (my_strcmp(cmd_ptr, "cat") == 0) {
                if (!arg) { sys_puts(console_ep, "Usage: cat <file>\n"); continue; }
                char *shm = shm_base;
                build_absolute_path(shm, arg, SHM_TOTAL_SIZE);
                
                if (vfs_syscall(114, blk_ep) == 0) { // Файл прочитан в shm
                    if (is_piping) {
                        sys_write(pipe_fd, shm);
                        sys_write(pipe_fd, "\n");
                        sys_pipe_wr_close(pipe_fd);
                    } else {
                        sys_puts(console_ep, shm);
                        sys_puts(console_ep, "\n");
                    }
                } else {
                    sys_puts(console_ep, "File not found or is a directory.\n");
                }
            }

            else if (my_strcmp(cmd_ptr, "echo") == 0) {
                if (!arg) { if (!is_piping) sys_puts(console_ep, "\n"); continue; }
                
                // Парсер перенаправления потока (ищем символ '>')
                char *redir = arg;
                while (*redir && *redir != '>') redir++;
                
                if (*redir == '>') {
                    *redir = '\0'; // Отрезаем строку текста
                    redir++;       // Сдвигаемся на начало пути к файлу
                    
                    // Пропускаем пробелы после '>'
                    while (*redir == ' ') redir++; 
                    
                    // Убираем пробелы в конце самого текста (перед '>')
                    char *text_end = arg;
                    while (*text_end != '\0') text_end++;
                    text_end--;
                    while (text_end >= arg && *text_end == ' ') {
                        *text_end = '\0';
                        text_end--;
                    }
                    
                    if (*redir == '\0') {
                        sys_puts(console_ep, "Parse error: expected file path after '>'\n");
                        continue;
                    }
                    
                    char *shm = shm_base;
                    char *path_ptr = shm;
                    char *text_ptr = shm + 128; // Текст кладем со смещением!

                    build_absolute_path(path_ptr, redir, 128);
                    my_strlcpy(text_ptr, arg, SHM_TOTAL_SIZE - 128);
                    
                    vfs_lock();
                    seL4_SetMR(0, 113); 
                    seL4_SetMR(1, my_strlen(arg)); 
                    
                    seL4_MessageInfo_t msg = seL4_MessageInfo_new(0, 0, 0, 2); // 2 регистра передано
                    seL4_Call(blk_ep, msg);
                    int ret_val = seL4_GetMR(0);
                    vfs_unlock();
                    if (ret_val != 0) {
                        sys_puts(console_ep, "Failed to write to file.\n");
                    }
                } else if (is_piping) {
                    sys_write(pipe_fd, arg);
                    sys_write(pipe_fd, "\n");
                    sys_pipe_wr_close(pipe_fd);
                } else {
                    // Обычный echo без перенаправления
                    sys_puts(console_ep, arg);
                    sys_puts(console_ep, "\n");
                }
            }

            else if (my_strcmp(cmd_ptr, "help") == 0) {
                const char* help_text = "Available: help, time, uptime, date, temp, mboxprobe, sleep, ls, ps, cat, echo, exec, kill, exit, shm, pid, mkdir, cd, pwd, ping, send, sendto, recv, netstat, ntp, wifiprobe, wifi (start/stop/restart/scan/connect/clean), touch, rm, mv\n";
                if (is_piping) {
                    sys_write(pipe_fd, help_text);
                    sys_pipe_wr_close(pipe_fd);
                } else {
                    sys_puts(console_ep, help_text);
                }
            }

            else if (my_strcmp(cmd_ptr, "exit") == 0) {
                sys_puts(console_ep, "Exiting sandbox...\n");
                sys_exit(root_ep);
            }
            
            // ==========================================
            // НОВОЕ: ТРИГГЕРЫ АППАРАТНЫХ КРАШЕЙ // Краш-тест - удалить
            // ==========================================
            else if (my_strcmp(cmd_ptr, "crash_shell") == 0) {
                sys_puts(console_ep, "[SHELL] Initiating intentional Segfault (Null Pointer Dereference)...\n");
                volatile int* boom = (volatile int*)0x0;
                *boom = 0xDEAD; // Оболочка умрет на этой строке
            }

            else if (my_strcmp(cmd_ptr, "crash_disk") == 0) {
                sys_puts(console_ep, "[SHELL] Sending poison pill to blk_driver...\n");
                vfs_syscall(121, blk_ep); // Оправляем команду умереть
            }
            // ==========================================

            else if (my_strcmp(cmd_ptr, "rm") == 0) {
                char* p = arg;

                if (!p || *p == '\0') {
                    sys_puts(console_ep, "rm: missing operand\n");
                    continue;
                }

                while (*p != '\0') {
                    while (*p == ' ') p++;
                    if (*p == '\0') break;

                    char* start_of_arg = p;
                    while (*p != ' ' && *p != '\0') p++;
                    
                    char temp_char = *p;
                    *p = '\0';

                    char *shm = shm_base;
                    build_absolute_path(shm, start_of_arg, SHM_TOTAL_SIZE);
                    if (vfs_syscall(120, blk_ep) != 0) {
                        sys_puts(console_ep, "rm: cannot remove '");
                        sys_puts(console_ep, start_of_arg);
                        sys_puts(console_ep, "': No such file or directory\n");
                    }
                    *p = temp_char;
                }
            } 
            // === КОМАНДА MV (Переименование) ===
            else if (my_strcmp(cmd_ptr, "mv") == 0) {
                if (!arg) {
                    sys_puts(console_ep, "mv: missing file operand\n");
                    continue;
                }
                char* p = arg;

                // 1. Вытаскиваем ИМЯ СТАРОГО ФАЙЛА (old_name)
                char old_name[32];
                int i = 0;
                while (*p != ' ' && *p != '\0' && i < 31) {
                    old_name[i++] = *p++;
                }
                old_name[i] = '\0';

                // 2. Пропускаем пробелы между аргументами
                while (*p == ' ') p++; 

                if (*p == '\0') {
                    sys_puts(console_ep, "mv: missing destination file operand after '");
                    sys_puts(console_ep, old_name);
                    sys_puts(console_ep, "'\n");
                    continue;
                }

                // 3. Вытаскиваем ИМЯ НОВОГО ФАЙЛА (new_name)
                char new_name[32];
                i = 0;
                while (*p != ' ' && *p != '\0' && i < 31) {
                    new_name[i++] = *p++;
                }
                new_name[i] = '\0';

                // 4. Готовим IPC-сообщение
                char *shm = shm_base;
                build_absolute_path(shm, old_name, 128);
                build_absolute_path(shm + 128, new_name, SHM_TOTAL_SIZE - 128);
                
                vfs_lock();
                seL4_SetMR(0, 116); // SYS_RENAME
                seL4_MessageInfo_t info = seL4_MessageInfo_new(0, 0, 0, 1);
                seL4_Call(blk_ep, info);
                int ret_val = seL4_GetMR(0);
                vfs_unlock();
                
                if (ret_val != 0) {
                    sys_puts(console_ep, "mv: cannot stat '");
                    sys_puts(console_ep, old_name);
                    sys_puts(console_ep, "': No such file or directory\n");
                }
            }
            else if (my_strncmp(cmd_ptr, "./", 2) == 0) {
                // Пользователь ввел команду типа ./test.elf
                char* filename = cmd_ptr + 2; // Пропускаем "./"
                
                // --- НОВЫЙ БЛОК ПАРСИНГА ПУТЕЙ ---
                // Если в имени файла есть слеш (например, mnt/test.elf),
                // нам нужно извлечь только само имя (test.elf)
                char* pure_filename = filename;
                int len = my_strlen(filename);
                for (int i = len - 1; i >= 0; i--) {
                    if (filename[i] == '/') {
                        pure_filename = &filename[i + 1];
                        break;
                    }
                }
                
                char safe_name[64] = {0};
                my_strncpy(safe_name, pure_filename, 63);
                
                // Упаковываем строку прямо в регистры процессора!
                seL4_SetMR(0, 100); // 100 = SYS_EXEC
                uint64_t* name_ptr = (uint64_t*)safe_name;
                for (int i = 0; i < 8; i++) {
                    seL4_SetMR(i + 1, name_ptr[i]);
                }
                
                seL4_MessageInfo_t msg = seL4_MessageInfo_new(0, 0, 0, 9);
                seL4_Call(root_ep, msg);

                int pid = (int)seL4_GetMR(0);
                if (pid > 0) {
                    // С новым мультиплексором в uart_driver, гонки больше нет.
                    // Можно выводить частями.
                    sys_puts(console_ep, "Spawned process with PID: ");
                    char buf[16]; int temp = pid, j = 0;
                    if (temp == 0) buf[j++] = '0';
                    while(temp > 0) { buf[j++] = (temp % 10) + '0'; temp /= 10; }
                    while(j > 0) { char c[2] = {buf[--j], 0}; sys_puts(console_ep, c); }
                    sys_puts(console_ep, "\nParent sleeping, handing over TTY...\n");

                    sys_wait(root_ep, pid);
                    sys_puts(console_ep, "\nChild exited. Parent taking back TTY.\n");
                } else if (pid == -1) {
                    sys_puts(console_ep, "[SHELL] Error: File not found on disk.\n");
                } else if (pid == -2) {
                    sys_puts(console_ep, "[SHELL] Error: Invalid ELF format.\n");
                } else {
                    sys_puts(console_ep, "[SHELL] Error: Spawn failed.\n");
                }
            } 
            else { sys_puts(console_ep, "Unknown command. Type 'help'.\n"); }

            // --- ПРАВАЯ ЧАСТЬ КОНВЕЙЕРА ---
            if (is_piping) {
                char *arg2 = cmd2;
                while (*arg2 && *arg2 != ' ') arg2++;
                if (*arg2 == ' ') { *arg2 = '\0'; arg2++; } else { arg2 = nullptr; }

                if (my_strcmp(cmd2, "grep") == 0) {
                    if (arg2) {
                        right_pid = spawn_thread((seL4_Word)grep_thread_func, (seL4_Word)grep_thread_stack + sizeof(grep_thread_stack) - 16, 
                                                    (seL4_Word)arg2, 0, 0, -1,
                                                    pipe_cap, ipc->caps_or_badges[1], ipc->caps_or_badges[2]);
                    } else {
                        sys_puts(console_ep, "grep: usage: grep <pattern>\n");
                    }
                } else {
                    sys_puts(console_ep, "Microkernel Pipe currently supports 'grep' on the right side.\n");
                }

                // ИСПРАВЛЕНО: Ждем завершения дочерних процессов с помощью sys_wait
                // Это надежнее, чем глобальный флаг.
                // Порядок не важен, т.к. sys_wait немедленно вернется, если процесс уже завершился.
                if (left_pid != -1) sys_wait(root_ep, left_pid);
                if (right_pid != -1) sys_wait(root_ep, right_pid);

                sys_pipe_close(pipe_fd);
            }
        }
    }

    return 0;
}