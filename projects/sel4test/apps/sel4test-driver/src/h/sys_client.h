#pragma once
// Фаза A (см. ROADMAP.md): общий клиентский IPC-бойлерплейт для команд,
// вынесенных из shell.cpp в отдельные `/sbin/*.elf` (ps/kill/taskset/top/
// balance/ls/cat/touch/rm/mv/mkdir). Логика 1:1 списана с shell.cpp/
// test.cpp (get_local_ipc/sys_write/vfs_lock/строковые хелперы) — дублировать
// её ещё 11 раз копипастой не имеет смысла, отсюда общий заголовок.
//
// ВАЖНО: каждый из 11 бинарников — ОТДЕЛЬНЫЙ `add_executable`, этот файл
// подключается ровно в одну единицу трансляции на программу, так что
// статические функции ниже не конфликтуют между исполняемыми файлами.

#include <sel4/sel4.h>
#include "h/common.h"
#include "h/platform.h" // USB_MAX_DEVICES (Milestone A3, Фаза 15 — fetch_usb_volume_list())

#define BOOT_BLK_EP 7 // см. тот же #define в shell.cpp

static inline seL4_IPCBuffer* get_local_ipc() {
    seL4_Word tls_addr;
    asm volatile("mrs %0, tpidrro_el0" : "=r"(tls_addr));
    return (seL4_IPCBuffer*)(tls_addr - 1024);
}

// Требуется muslc (см. тот же стаб в shell.cpp/test.cpp) — просто зависаем,
// эти утилиты не рассчитаны на восстановление после assert.
void __assert_fail(const char *assertion, const char *file, int line, const char *function) { while(1); }

// НАЙДЕНО НА ЖИВОМ ЖЕЛЕЗЕ: обычный `return 0;` из main() в этом рантайме не
// работает (некому его обработать — нет полноценного crt-эпилога) и валит
// процесс с page fault по PC=0/Mem=0 (прыжок через нулевой указатель) СРАЗУ
// ПОСЛЕ того, как вся полезная работа уже сделана и вывод напечатан. Каждый
// /sbin-бинарник ОБЯЗАН завершаться явным sys_exit() вместо return — тот же
// приём, что уже используют shell.cpp/test.cpp (см. sys_exit() там же).
static void sys_exit(seL4_CPtr root_ep) {
    seL4_IPCBuffer *ipc = get_local_ipc();
    ipc->msg[0] = 103; // SYS_EXIT
    seL4_Call(root_ep, seL4_MessageInfo_new(0, 0, 0, 1));
    while (1) seL4_Yield(); // Сюда выполнение никогда не дойдёт
}

// Фаза 6 (SMP): общий межпроцессный мьютекс на нотификации, см. shell.cpp —
// капу получают только is_driver==0 (shell) и is_driver==253 (доверенные
// /sbin-утилиты, см. ROADMAP Фаза A). Ноль для обычного exec — vfs_lock/
// unlock() в этом случае просто no-op (см. main.cpp shm_pages_mask_for_role).
static seL4_CPtr g_vfs_mutex_ep = 0;

static inline void vfs_lock() {
    if (!g_vfs_mutex_ep) return;
    seL4_Word badge;
    seL4_Wait(g_vfs_mutex_ep, &badge);
}

static inline void vfs_unlock() {
    if (!g_vfs_mutex_ep) return;
    seL4_Signal(g_vfs_mutex_ep);
}

// Универсальная запись в файловый дескриптор (см. shell.cpp/sys_write) —
// fd=1 (stdout) у любого спавненного процесса заведён на console_ep самим
// root'ом при спавне (caps_or_badges[1]), пайпинг для /sbin-утилит пока не
// поддержан (см. ROADMAP Фаза A — известное, принятое ограничение).
static void sys_write(int fd, const char* str) {
    seL4_IPCBuffer *ipc = get_local_ipc();
    seL4_CPtr target_ep = ipc->caps_or_badges[fd];

    int total_len = 0;
    while (str[total_len]) total_len++;

    int offset = 0;
    while (offset < total_len) {
        int chunk = total_len - offset;
        if (chunk > 100) chunk = 100;

        ipc->msg[0] = 8; // SYS_PUTS
        for (int i = 0; i < chunk; i++) {
            ipc->msg[i + 1] = str[offset + i];
        }
        seL4_MessageInfo_t reply = seL4_Call(target_ep, seL4_MessageInfo_new(0, 0, 0, chunk + 1));
        // issuse.txt №7 — при записи в пайп root теперь возвращает реальное
        // число принятых байт в MR0 (см. main.cpp, case 8) вместо молчаливого
        // "всё ок", если буфер пайпа полон. Консоль (uart_driver) отвечает
        // пустым сообщением (length 0) — там accepted остаётся chunk, как и
        // раньше, поведение для обычного вывода не меняется.
        int accepted = chunk;
        if (seL4_MessageInfo_get_length(reply) >= 1) {
            accepted = (int)seL4_GetMR(0);
            if (accepted < chunk) break; // пайп полон — читателя, способного его освободить, ждать нет смысла
        }
        offset += accepted;
    }
}

// Сигнатура совместима с shell.cpp::sys_puts (первый параметр исторически
// игнорируется) — чтобы логику команд можно было переносить как есть.
static inline void sys_puts(seL4_CPtr /*unused_ep*/, const char *str) {
    sys_write(1, str);
}

// issuse.txt №63(b) — вариант sys_write() с явной длиной вместо strlen():
// нужен для сырых чанков файла (SYS_READ_FILE=119), которые НЕ гарантированно
// '\0'-терминированы в нужном месте (могут содержать нулевые байты как
// законный байт содержимого посередине чанка — strlen() в этом случае
// молча обрежет вывод раньше времени).
static void sys_write_n(int fd, const char *buf, int len) {
    seL4_IPCBuffer *ipc = get_local_ipc();
    seL4_CPtr target_ep = ipc->caps_or_badges[fd];

    int offset = 0;
    while (offset < len) {
        int chunk = len - offset;
        if (chunk > 100) chunk = 100;

        ipc->msg[0] = 8; // SYS_PUTS
        for (int i = 0; i < chunk; i++) ipc->msg[i + 1] = buf[offset + i];
        seL4_MessageInfo_t reply = seL4_Call(target_ep, seL4_MessageInfo_new(0, 0, 0, chunk + 1));
        int accepted = chunk;
        if (seL4_MessageInfo_get_length(reply) >= 1) {
            accepted = (int)seL4_GetMR(0);
            if (accepted < chunk) break; // см. sys_write() — пайп полон, дальше ждать некого
        }
        offset += accepted;
    }
}

static int my_strcmp(const char *s1, const char *s2) {
    while (*s1 && (*s1 == *s2)) { s1++; s2++; }
    return *(const unsigned char*)s1 - *(const unsigned char*)s2;
}

static int my_strncmp(const char *s1, const char *s2, int n) {
    while (n && *s1 && (*s1 == *s2)) { s1++; s2++; n--; }
    if (n == 0) return 0;
    return *(const unsigned char*)s1 - *(const unsigned char*)s2;
}

static void my_strcpy(char *dest, const char *src) {
    while ((*dest++ = *src++));
}

static int my_strlcpy(char *dest, const char *src, int cap) {
    if (cap <= 0) return 0;
    int i = 0;
    for (; i < cap - 1 && src[i] != '\0'; i++) dest[i] = src[i];
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

// issuse.txt №54: simple_atoi() останавливается на первом не-цифровом
// символе и возвращает 0 для строк вроде "abc"/"5x"/"-1" без единой
// ошибки — kill/taskset передавали этот 0 (или обрезанное число) дальше
// как реальный PID/номер ядра. Вызывающий должен проверить это ПЕРЕД
// simple_atoi(), если разница между "ввели 0" и "ввели мусор" важна.
static bool is_all_digits(const char *str) {
    if (!str || *str == '\0') return false;
    for (const char *p = str; *p; p++) {
        if (*p < '0' || *p > '9') return false;
    }
    return true;
}

// issuse.txt №50: поддержка одного уровня двойных кавычек — токен вида
// "foo bar" читается как ОДИН аргумент "foo bar" (пробел внутри не
// разделяет, сами кавычки в результат не входят), вместо того чтобы
// молча развалиться на два токена `"foo` и `bar"`. Без экранирования
// (нет \" внутри кавычек) — минимально достаточно для путей/имён файлов
// с пробелами, которые и были реальной жалобой.
static char *next_token(char **cursor) {
    if (!cursor || !*cursor) return nullptr;
    char *tok = *cursor;
    while (*tok == ' ') tok++;
    if (*tok == '\0') { *cursor = tok; return nullptr; }

    char *end;
    if (*tok == '"') {
        tok++; // токен начинается ПОСЛЕ открывающей кавычки
        end = tok;
        while (*end && *end != '"') end++;
        if (*end == '"') { *end = '\0'; end++; } // закрывающую тоже съедаем
        // (не закрыта — берём до конца строки как есть, без ошибки)
    } else {
        end = tok;
        while (*end && *end != ' ') end++;
    }
    if (*end == ' ') {
        *end = '\0';
        end++;
        while (*end == ' ') end++;
    }
    *cursor = end;
    return tok;
}

// Инициализация окружения — читает root_ep/blk_ep/vfs_mutex_ep из boot-IPC
// (см. main.cpp::spawn_process, ветка "Shell or other user app") и запрашивает
// динамический SHM (107=SYS_SHM_GET, см. shell.cpp). Для is_driver==253 это
// единственная страница (SHM_PAGE_VFS), которая вообще выдаётся — см.
// shm_pages_mask_for_role() в main.cpp.
#define CWD_SIZE 64
#define SHM_TOTAL_SIZE 16384

static char g_cwd[CWD_SIZE] = "/root";

// issuse.txt №57: см. 1:1-копию normalize_absolute_path() в shell.cpp —
// резолвит "." и ".." в ЛЮБОМ месте уже собранного абсолютного пути
// (раньше это умела только ручная спецветка "cd .." в shell.cpp, а
// touch/rm/mkdir/cat/mv/ls через этот build_absolute_path — нет).
static void normalize_absolute_path(char* target, int max_len) {
    constexpr int SCRATCH = 512;
    if ((int)my_strlen(target) >= SCRATCH) return;

    char src[SCRATCH];
    my_strlcpy(src, target, SCRATCH);

    constexpr int MAX_COMPONENTS = 64;
    int starts[MAX_COMPONENTS];
    int lens[MAX_COMPONENTS];
    int n = 0;

    int i = 1; // после ведущего '/'
    while (src[i] != '\0') {
        while (src[i] == '/') i++;
        if (src[i] == '\0') break;
        int start = i;
        while (src[i] != '\0' && src[i] != '/') i++;
        int len = i - start;
        if (len == 1 && src[start] == '.') {
            // "." — сам текущий каталог, пропускаем компонент
        } else if (len == 2 && src[start] == '.' && src[start + 1] == '.') {
            if (n > 0) n--; // ".." — подняться на уровень выше
        } else if (n < MAX_COMPONENTS) {
            starts[n] = start; lens[n] = len; n++;
        }
    }

    char out[SCRATCH];
    int w = 0;
    out[w++] = '/';
    for (int c = 0; c < n; c++) {
        if (c > 0) out[w++] = '/';
        for (int k = 0; k < lens[c] && w < SCRATCH - 1; k++) out[w++] = src[starts[c] + k];
    }
    out[w] = '\0';

    my_strlcpy(target, out, max_len);
}

// 1:1 с build_absolute_path() в shell.cpp, только читает g_cwd вместо
// current_working_dir.
static void build_absolute_path(char* target, const char* arg, int max_len) {
    if (max_len <= 0) return;
    if (arg[0] == '/') {
        my_strlcpy(target, arg, max_len);
    } else {
        int len = my_strlcpy(target, g_cwd, max_len);
        if (len > 0 && target[len - 1] != '/' && len + 1 < max_len) {
            target[len] = '/';
            target[len + 1] = '\0';
            len++;
        }
        if (len < max_len - 1) {
            my_strlcpy(target + len, arg, max_len - len);
        }
    }
    normalize_absolute_path(target, max_len);
}

// Milestone A3 (Фаза 15, см. ROADMAP.md/план) — при нескольких
// одновременно смонтированных USB-устройствах клиент больше не может
// заранее знать имя (или имена) точек монтирования, поэтому маршрутизация
// упростилась: клиент срезает только сам "/mnt" целиком (дешёвая
// строковая проверка, без IPC), а какому конкретно устройству
// принадлежит ведущий компонент остатка (имя тома) — решает СЕРВЕР
// (usb_driver.cpp::resolve_device_by_path()). Путь ДОЛЖЕН быть уже
// абсолютным (build_absolute_path()). Нулевой usb_storage_ep (untrusted
// exec, is_driver==254 — см. main.cpp) — fail-closed на blk_ep, тот же
// принцип, что и остальной least-privilege в этом проекте (см.
// shm_pages_mask_for_role).
static seL4_CPtr route_vfs_path(char *path, seL4_CPtr blk_ep, seL4_CPtr usb_storage_ep) {
    if (usb_storage_ep == 0) return blk_ep;
    if (path[0] == '/' && path[1] == 'm' && path[2] == 'n' && path[3] == 't' && path[4] == '/') {
        const char *remainder = path + 4; // остаётся "/<имя_тома>/..." — ведущий '/' уже здесь
        int j = 0; while (remainder[j] != '\0') { path[j] = remainder[j]; j++; } path[j] = '\0';
        return usb_storage_ep;
    }
    return blk_ep;
}

// Milestone A3 (Фаза 15) — заменяет fetch_usb_volume_name() (Milestone
// 10, возвращал имя только ПЕРВОГО смонтированного устройства) —
// USB_CMD_LIST_VOLUMES возвращает битовую маску смонтированных слотов +
// имя КАЖДОГО (4 регистра на слот, 32 байта, тот же приём упаковки
// строки в регистры, что уже использует sys_client_init() для cwd/имени
// exec'а). ls.cpp зовёт это ОДИН раз при листинге "/mnt", печатает
// "[DIR] <имя>" на каждый установленный бит.
struct UsbVolumeList {
    bool mounted[USB_MAX_DEVICES];
    char name[USB_MAX_DEVICES][32];
};

static bool fetch_usb_volume_list(seL4_CPtr usb_storage_ep, UsbVolumeList &out) {
    for (int i = 0; i < USB_MAX_DEVICES; i++) { out.mounted[i] = false; out.name[i][0] = '\0'; }
    if (usb_storage_ep == 0) return false;
    seL4_SetMR(0, 3); // USB_CMD_LIST_VOLUMES
    seL4_MessageInfo_t reply = seL4_Call(usb_storage_ep, seL4_MessageInfo_new(0, 0, 0, 1));
    if (seL4_MessageInfo_get_length(reply) < (seL4_Word)(1 + 4 * USB_MAX_DEVICES)) return false;
    seL4_Word mask = seL4_GetMR(0);
    bool any = false;
    for (int i = 0; i < USB_MAX_DEVICES; i++) {
        out.mounted[i] = (mask & (1u << i)) != 0;
        seL4_Word *words = (seL4_Word*)out.name[i];
        for (int w = 0; w < 4; w++) words[w] = seL4_GetMR(1 + i * 4 + w);
        out.name[i][31] = '\0';
        if (out.mounted[i]) any = true;
    }
    return any;
}

struct SysClientEnv {
    seL4_CPtr root_ep;
    seL4_CPtr blk_ep;
    seL4_CPtr usb_storage_ep; // Milestone 9 — 0, если не выдан (см. main.cpp shm/cap-выдачу для is_driver!=0/253)
    // issuse.txt №5 (тестовый хук `sleep`, см. sbin/sleep.cpp) — уже
    // безусловно минтится каждому спавненному процессу (main.cpp, ветка
    // "Shell or other user app") и проставляется в BOOT_TIMER_EP, просто
    // раньше ничем в общем клиентском окружении не читался.
    seL4_CPtr timer_ep;
    char *shm;      // nullptr, если SHM не выдан (не доверенный exec)
    char arg_buffer[512];
    char *arg;      // nullptr, если аргументов не передали
};

static void sys_client_init(SysClientEnv &env) {
    seL4_IPCBuffer *ipc = get_local_ipc();
    seL4_SetIPCBuffer(ipc);

    env.root_ep = ipc->msg[BOOT_ROOT_EP];
    env.blk_ep  = ipc->msg[BOOT_BLK_EP];
    env.usb_storage_ep = ipc->msg[BOOT_USB_STORAGE_EP]; // Milestone 9
    env.timer_ep = ipc->msg[BOOT_TIMER_EP];
    g_vfs_mutex_ep = ipc->msg[BOOT_VFS_MUTEX_NTFN_CAP];

    my_strlcpy(env.arg_buffer, (char*)&ipc->msg[0], (int)sizeof(env.arg_buffer));
    env.arg = env.arg_buffer[0] ? env.arg_buffer : nullptr;

    // Фаза 9.A (продолжение, см. ROADMAP.md/EXEC_CWD_MSG_SLOT в common.h):
    // cwd вызывающего шелла — отдельный слот boot-IPC (не общая VFS SHM,
    // которую до, во время и после этого места по-прежнему трогает
    // load_elf_from_disk() в main.cpp — читаем ДО любых seL4_Call, чтобы не
    // зависеть от того, что там окажется к этому моменту).
    my_strlcpy(g_cwd, (char*)&ipc->msg[EXEC_CWD_MSG_SLOT], CWD_SIZE);
    if (g_cwd[0] == '\0') my_strlcpy(g_cwd, "/root", CWD_SIZE);

    seL4_SetMR(0, 107); // SYS_SHM_GET
    seL4_Call(env.root_ep, seL4_MessageInfo_new(0, 0, 0, 1));
    env.shm = (char*)seL4_GetMR(0);
}

static int vfs_syscall(int syscall_num, seL4_CPtr blk_ep) {
    vfs_lock();
    seL4_SetMR(0, syscall_num);
    seL4_Call(blk_ep, seL4_MessageInfo_new(0, 0, 0, 1));
    int ret_val = seL4_GetMR(0);
    vfs_unlock();
    return ret_val;
}
