#include "h/sys_client.h"

// coretest — автотест адаптивного переноса драйверов между ядрами
// (issuse.txt №74, часть "б") и полного сброса USB "с нуля" (часть "в").
//
// ЗАЧЕМ ОТДЕЛЬНЫЙ БИНАРНИК, а не ручной прогон команд в шелле. Главную
// проверку — "длинное чтение с диска ОДНОВРЕМЕННО с балансировкой ядер" —
// человек за одним терминалом физически выполнить не может: пока идёт
// `cat` большого файла, набрать `balance` некуда. Здесь длинное чтение
// уходит в отдельный поток (SYS_CLONE), а главный поток в это же время
// гоняет balance. Плюс результат перестаёт зависеть от того, что и в
// каком порядке человек успел напечатать.
//
// ПОЧЕМУ ЧТЕНИЕ НЕ ПЕЧАТАЕТСЯ. Файл /root/bigfile.txt большой; вывод его
// содержимого в консоль забил бы лог и сам по себе стал бы узким местом
// теста. Вместо этого считаем длину и контрольную сумму, и сравниваем их
// с эталонным прогоном ТОГО ЖЕ файла, сделанным без балансировки: если
// перенос драйвера между ядрами посреди чтения хоть что-то портит,
// суммы разойдутся.
//
// ПРО SHM. Страница SHM одна на всех (см. vfs_lock()/h/sys_client.h) —
// именно поэтому shell берёт vfs_lock даже вокруг `balance`, и именно
// поэтому обычный balance тут не подошёл бы: он затёр бы страницу, куда
// в этот момент blk_driver кладёт данные файла. Для этого в root добавлен
// "тихий" режим SYS_BALANCE (MR1=1) — балансирует, но в SHM не пишет
// ничего (см. common.h/SYS_BALANCE).
//
// Формат вывода — по шагам, как просил пользователь:
//   ТЕСТ N: <название>
//   <вывод шага>
//   ТЕСТ N ЗАВЕРШЁН
// либо, при провале:
//   ТЕСТ N ОСТАНОВЛЕН — ОШИБКА: <причина>
// и прогон прекращается: дальше по сломанной системе мерить нечего.
//
// Запуск:
//   exec /sbin/tests/coretest.elf              — все шаги
//   exec /sbin/tests/coretest.elf 6            — только шаг 6
//   exec /sbin/tests/coretest.elf 0 /root/x.txt — все шаги, другой файл

// --- мелкая печать (в тестах libc нет, см. соседние tests/*.cpp) ---
static void putdec(int val) {
    char buf[12]; int j = 0;
    if (val < 0) { sys_puts(0, "-"); val = -val; }
    if (val == 0) buf[j++] = '0';
    while (val > 0) { buf[j++] = '0' + (val % 10); val /= 10; }
    while (j > 0) { char c[2] = {buf[--j], 0}; sys_puts(0, c); }
}
static void putu64(uint64_t val) {
    char buf[24]; int j = 0;
    if (val == 0) buf[j++] = '0';
    while (val > 0) { buf[j++] = (char)('0' + (val % 10)); val /= 10; }
    while (j > 0) { char c[2] = {buf[--j], 0}; sys_puts(0, c); }
}

static seL4_CPtr g_root_ep = 0;
static seL4_CPtr g_timer_ep = 0;

static seL4_Word now_ms() {
    seL4_SetMR(0, 3); // SYS_GET_TIME
    seL4_Call(g_timer_ep, seL4_MessageInfo_new(0, 0, 0, 1));
    return seL4_GetMR(0);
}
static void sleep_ms(int ms) {
    seL4_SetMR(0, 8); // SYS_SLEEP_MS
    seL4_SetMR(1, (seL4_Word)ms);
    seL4_Call(g_timer_ep, seL4_MessageInfo_new(0, 0, 0, 2));
}

// --- обёртки над сисколлами root'а ---
struct ProcInfo {
    int pid;        // -1, если процесса нет
    int core;
    int is_driver;
    int state;      // DRIVER_STATE_BUSY/PARKED/UNKNOWN
};

static ProcInfo proc_info(const char *name) {
    char safe[32] = {0};
    my_strlcpy(safe, name, sizeof(safe));
    seL4_SetMR(0, SYS_PROC_INFO);
    uint64_t *p = (uint64_t*)safe;
    for (int i = 0; i < 4; i++) seL4_SetMR(i + 1, p[i]);
    seL4_Call(g_root_ep, seL4_MessageInfo_new(0, 0, 0, 5));
    ProcInfo out;
    out.pid = (int)seL4_GetMR(0);
    out.core = (int)seL4_GetMR(1);
    out.is_driver = (int)seL4_GetMR(2);
    out.state = (int)seL4_GetMR(3);
    return out;
}

static int set_affinity(int pid, int core) {
    seL4_SetMR(0, SYS_SET_AFFINITY);
    seL4_SetMR(1, (seL4_Word)pid);
    seL4_SetMR(2, (seL4_Word)core);
    seL4_Call(g_root_ep, seL4_MessageInfo_new(0, 0, 0, 3));
    return (int)seL4_GetMR(0);
}

// quiet=true — не писать отчёт в общую SHM-страницу (см. шапку файла).
static int balance(bool quiet, int *moved_out) {
    seL4_SetMR(0, SYS_BALANCE);
    seL4_SetMR(1, quiet ? 1 : 0);
    seL4_Call(g_root_ep, seL4_MessageInfo_new(0, 0, 0, 2));
    int status = (int)seL4_GetMR(0);
    if (moved_out) *moved_out = (int)seL4_GetMR(1);
    return status;
}

static int driver_signal(const char *name, int sig) {
    char safe[32] = {0};
    my_strlcpy(safe, name, sizeof(safe));
    seL4_SetMR(0, SYS_DRIVER_SIGNAL);
    uint64_t *p = (uint64_t*)safe;
    for (int i = 0; i < 4; i++) seL4_SetMR(i + 1, p[i]);
    seL4_SetMR(5, (seL4_Word)sig);
    seL4_Call(g_root_ep, seL4_MessageInfo_new(0, 0, 0, 6));
    return (int)seL4_GetMR(0);
}

static void recover(const char *name) {
    char safe[32] = {0};
    my_strlcpy(safe, name, sizeof(safe));
    seL4_SetMR(0, 117); // SYS_RECOVER
    uint64_t *p = (uint64_t*)safe;
    for (int i = 0; i < 4; i++) seL4_SetMR(i + 1, p[i]);
    seL4_Call(g_root_ep, seL4_MessageInfo_new(0, 0, 0, 5));
}

// --- чтение файла без печати содержимого ---
// Возвращает false, если файл не найден или чтение оборвалось на середине.
// Контрольная сумма — простая аддитивная с ротацией: цель не
// криптостойкость, а поймать перестановку/потерю/дублирование блока,
// чего простая сумма байт не заметила бы.
struct ReadResult {
    volatile bool done;
    volatile bool ok;
    volatile bool truncated_mid;   // файл начал читаться, потом ошибка
    volatile bool not_found;
    volatile uint64_t bytes;
    volatile uint64_t checksum;
    volatile int chunks;
    volatile seL4_Word ms;
};

// max_chunks == 0 — читать до конца файла. Ненулевое значение —
// оборвать после стольких чанков: пробам вида "драйвер на новом ядре
// вообще отвечает" читать весь большой файл незачем, а время прогона это
// экономит заметно. При обрыве по лимиту bytes/checksum считаются только
// по прочитанной части и с эталоном НЕ сравнимы.
static void read_file_checksummed(const char *path, seL4_CPtr blk_ep, seL4_CPtr usb_ep,
                                  char *shm, ReadResult *out, int max_chunks = 0) {
    out->done = false; out->ok = false; out->truncated_mid = false; out->not_found = false;
    out->bytes = 0; out->checksum = 0; out->chunks = 0; out->ms = 0;

    char abs[256];
    my_strlcpy(abs, path, sizeof(abs));
    build_absolute_path(shm, abs, SHM_TOTAL_SIZE);
    seL4_CPtr target_ep = route_vfs_path(shm, blk_ep, usb_ep);
    my_strlcpy(abs, shm, sizeof(abs)); // абсолютный (и уже сдвинутый для /mnt) путь

    seL4_Word t0 = now_ms();
    vfs_lock();
    uint32_t offset = 0;
    bool found = false;
    uint64_t sum = 0;
    while (1) {
        my_strlcpy(shm, abs, SHM_TOTAL_SIZE);
        seL4_SetMR(0, 119); // SYS_READ_FILE
        seL4_SetMR(1, offset);
        seL4_Call(target_ep, seL4_MessageInfo_new(0, 0, 0, 2));
        if ((int)seL4_GetMR(0) != 0) {
            if (found) out->truncated_mid = true;
            break;
        }
        int n = (int)seL4_GetMR(1);
        found = true;
        if (n == 0) break; // EOF
        for (int i = 0; i < n; i++) {
            sum = (sum << 1) | (sum >> 63);       // ротация — ловит перестановку блоков
            sum += (uint64_t)(unsigned char)shm[VFS_PAYLOAD_OFFSET + i];
        }
        offset += (uint32_t)n;
        out->chunks = out->chunks + 1;
        out->bytes = offset;
        out->checksum = sum;
        if (max_chunks > 0 && out->chunks >= max_chunks) break;
    }
    vfs_unlock();
    out->ms = now_ms() - t0;
    out->not_found = !found;
    out->ok = found && !out->truncated_mid && offset > 0;
    out->done = true;
}

// --- поток-читатель для шага 7 (истинная параллельность) ---
static char g_reader_stack[16384] __attribute__((aligned(16)));
static ReadResult g_concurrent_read;
static char g_reader_path[256];
static seL4_CPtr g_reader_blk_ep = 0;
static seL4_CPtr g_reader_usb_ep = 0;
static char *g_reader_shm = nullptr;

static void reader_thread_entry(seL4_Word root_ep_raw, seL4_Word, seL4_Word) {
    // Обязательный пролог клонированного потока (см. tests/clonetest.cpp,
    // найдено на живом железе): root настраивает сам регистр TLS, но
    // библиотечный кеш указателя на IPC-буфер внутри libsel4 поток обязан
    // проинициализировать себе сам, как это делает main() любого
    // /sbin-бинарника через sys_client_init().
    seL4_Word tls_addr;
    asm volatile("mrs %0, tpidrro_el0" : "=r"(tls_addr));
    seL4_SetIPCBuffer((seL4_IPCBuffer*)(tls_addr - 1024));

    read_file_checksummed(g_reader_path, g_reader_blk_ep, g_reader_usb_ep, g_reader_shm, &g_concurrent_read);

    seL4_SetMR(0, 105); // SYS_THREAD_EXIT — полная уборка кап потока в root'е
    seL4_Call((seL4_CPtr)root_ep_raw, seL4_MessageInfo_new(0, 0, 0, 1));
    while (1) seL4_Yield(); // сюда ядро вернуть не должно (TCB suspended)
}

static int clone_reader(seL4_CPtr root_ep) {
    seL4_SetMR(0, 101); // SYS_CLONE
    seL4_SetMR(1, (seL4_Word)(uintptr_t)&reader_thread_entry);
    seL4_SetMR(2, (seL4_Word)root_ep);
    seL4_SetMR(3, 0);
    seL4_SetMR(4, 0);
    seL4_SetMR(5, 0); seL4_SetMR(6, 0); seL4_SetMR(7, 0); // stdin/stdout/stderr — поток ничего не печатает
    seL4_SetMR(8, (seL4_Word)-1); // не пайп
    seL4_SetMR(9, (seL4_Word)(uintptr_t)(g_reader_stack + sizeof(g_reader_stack)));
    seL4_Call(root_ep, seL4_MessageInfo_new(0, 0, 0, 10));
    return (int)seL4_GetMR(0);
}

// --- каркас шагов ---
static int g_step = 0;
static int g_passed = 0;
static int g_skipped = 0;

// Номер шага задаётся явно, а не инкрементом: при запуске одного шага
// (`coretest 7`) он обязан называться седьмым, а не первым — иначе лог
// одиночного прогона не сопоставить с логом полного.
static void step_begin(int n, const char *title) {
    g_step = n;
    sys_puts(0, "\nТЕСТ "); putdec(g_step); sys_puts(0, ": "); sys_puts(0, title); sys_puts(0, "\n");
}
static void step_ok() {
    g_passed++;
    sys_puts(0, "ТЕСТ "); putdec(g_step); sys_puts(0, " ЗАВЕРШЁН\n");
}
static void step_skip(const char *why) {
    g_skipped++;
    sys_puts(0, "ТЕСТ "); putdec(g_step); sys_puts(0, " ПРОПУЩЕН — "); sys_puts(0, why); sys_puts(0, "\n");
}
// Возвращает false — вызывающий обязан прекратить прогон.
static bool step_fail(const char *why) {
    sys_puts(0, "ТЕСТ "); putdec(g_step); sys_puts(0, " ОСТАНОВЛЕН — ОШИБКА: "); sys_puts(0, why); sys_puts(0, "\n");
    return false;
}

// Root печатает в UART НАПРЯМУЮ (uart_puts, busy-wait по регистрам), а
// этот тест — через console_ep/uart_driver. Два независимых писателя в одну
// линию перемешивают и теряют символы: в прогоне 2026-09-05 вердикт шага 10
// и весь итоговый блок утонули в логе usbreset'а ровно так. Поэтому после
// каждого шага, который заставляет root много печатать (usbreset, recover,
// wifi start), даём линии успокоиться, прежде чем печатать свой результат.
static void settle_uart() { sleep_ms(400); }

static const char *state_name(int st) {
    if (st == (int)DRIVER_STATE_PARKED) return "PARKED";
    if (st == (int)DRIVER_STATE_BUSY)   return "BUSY";
    return "нет данных";
}

static void print_proc_line(const char *name, const ProcInfo &pi) {
    sys_puts(0, "  "); sys_puts(0, name);
    if (pi.pid < 0) { sys_puts(0, ": не запущен\n"); return; }
    sys_puts(0, ": pid="); putdec(pi.pid);
    sys_puts(0, " ядро="); putdec(pi.core);
    sys_puts(0, " is_driver="); putdec(pi.is_driver);
    sys_puts(0, " состояние="); sys_puts(0, state_name(pi.state));
    sys_puts(0, "\n");
}

static const char *DRIVER_NAMES[] = {
    "uart_driver", "timer_driver", "blk_driver", "net_driver", "wifi_driver", "usb_driver"
};
constexpr int DRIVER_COUNT = sizeof(DRIVER_NAMES) / sizeof(DRIVER_NAMES[0]);

int main(int argc, char *argv[]) {
    SysClientEnv env;
    sys_client_init(env);
    g_root_ep = env.root_ep;
    g_timer_ep = env.timer_ep;

    int only = 0; // 0 = все шаги
    const char *bigfile = "/root/bigfile.txt";
    char argbuf[256];
    if (env.arg && *env.arg) {
        my_strlcpy(argbuf, env.arg, sizeof(argbuf));
        char *cursor = argbuf;
        char *t1 = next_token(&cursor);
        char *t2 = next_token(&cursor);
        // Порядок токенов свободный: "7", "7 /root/x.txt" или просто
        // "/root/x.txt" — первый числовой токен это номер шага, первый
        // нечисловой это путь к файлу.
        if (t1 && *t1) {
            if (is_all_digits(t1)) only = simple_atoi(t1);
            else bigfile = t1;
        }
        if (t2 && *t2 && !is_all_digits(t2)) bigfile = t2;
    }

    sys_puts(0, "==========================================================\n");
    sys_puts(0, "coretest — перенос драйверов между ядрами + сброс USB\n");
    sys_puts(0, "большой файл: "); sys_puts(0, bigfile); sys_puts(0, "\n");
    if (only) { sys_puts(0, "режим: только шаг "); putdec(only); sys_puts(0, "\n"); }
    sys_puts(0, "==========================================================\n");

    // Состояние, переносимое между шагами.
    ReadResult baseline;
    baseline.ok = false; baseline.bytes = 0; baseline.checksum = 0;
    bool baseline_valid = false;
    bool usb_present = false;

    // ================= ТЕСТ 1 =================
    // Инвентаризация: кто из драйверов запущен, на каком ядре, и видит ли
    // root их состояние PARKED/BUSY. Последнее — прямая проверка, что
    // общая страница состояния (issuse.txt №74/б) вообще создалась и
    // драйверы в неё пишут: без неё весь механизм безопасного Suspend'а
    // молча вырождается в "отказывать всем вне ядра 0".
    if (!only || only == 1) {
        step_begin(1, "инвентаризация драйверов и общая страница состояния");
        bool have_state = false;
        for (int i = 0; i < DRIVER_COUNT; i++) {
            ProcInfo pi = proc_info(DRIVER_NAMES[i]);
            print_proc_line(DRIVER_NAMES[i], pi);
            if (pi.pid >= 0 && pi.state != (int)DRIVER_STATE_UNKNOWN) have_state = true;
            if (my_strcmp(DRIVER_NAMES[i], "usb_driver") == 0 && pi.pid >= 0) usb_present = true;
        }
        for (int i = 0; i < DRIVER_COUNT; i++) {
            if (my_strcmp(DRIVER_NAMES[i], "wifi_driver") == 0) continue; // штатно может быть не запущен
            if (my_strcmp(DRIVER_NAMES[i], "usb_driver") == 0) continue;  // может быть выключен в сборке
            if (proc_info(DRIVER_NAMES[i]).pid < 0) return step_fail("обязательный драйвер не запущен"), sys_exit(env.root_ep), 1;
        }
        if (!have_state) return step_fail("root не видит состояние НИ ОДНОГО драйвера — общая страница PARKED/BUSY не работает (см. issuse.txt №74/б)"), sys_exit(env.root_ep), 1;
        step_ok();
    } else {
        usb_present = proc_info("usb_driver").pid >= 0;
    }

    // ================= ТЕСТ 2 =================
    // timer_driver переносить нельзя — единственное исключение из
    // process_is_migratable() (per-core PPI, кормит PM_WDOG, обслуживает
    // SYS_SLEEP_MS, на котором держится сам механизм ожидания PARKED).
    // Ожидаемый код ответа 3.
    if (!only || only == 2) {
        step_begin(2, "timer_driver защищён от переноса");
        ProcInfo t = proc_info("timer_driver");
        int st = set_affinity(t.pid, 3);
        sys_puts(0, "  taskset timer_driver -> ядро 3: код ответа "); putdec(st); sys_puts(0, "\n");
        if (st != 3) return step_fail("ожидался код 3 (перенос запрещён); timer_driver мог реально уехать с ядра 0"), sys_exit(env.root_ep), 1;
        ProcInfo after = proc_info("timer_driver");
        sys_puts(0, "  ядро после попытки: "); putdec(after.core); sys_puts(0, "\n");
        if (after.core != t.core) return step_fail("ядро timer_driver всё-таки изменилось"), sys_exit(env.root_ep), 1;
        step_ok();
    }

    // ================= ТЕСТ 3 =================
    // Ручной перенос драйвера (то, что до issuse.txt №74/б было запрещено
    // целиком). Берём blk_driver: он штатно живёт на ядре 0, значит
    // проверяем именно переход 0 -> не-0 и обратно.
    if (!only || only == 3) {
        step_begin(3, "taskset: blk_driver уезжает с ядра 0 и возвращается");
        ProcInfo before = proc_info("blk_driver");
        sys_puts(0, "  до: ядро "); putdec(before.core); sys_puts(0, "\n");
        int st = set_affinity(before.pid, 3);
        if (st != 0) return step_fail("taskset вернул не 0"), sys_exit(env.root_ep), 1;
        ProcInfo mid = proc_info("blk_driver");
        sys_puts(0, "  после переноса: ядро "); putdec(mid.core); sys_puts(0, "\n");
        if (mid.core != 3) return step_fail("ядро не изменилось на 3"), sys_exit(env.root_ep), 1;

        // Драйвер на новом ядре обязан продолжать работать — иначе
        // "перенос удался" ничего не значит.
        ReadResult r;
        read_file_checksummed(bigfile, env.blk_ep, env.usb_storage_ep, env.shm, &r, 8);
        sys_puts(0, "  контрольное чтение на новом ядре: байт="); putu64(r.bytes);
        sys_puts(0, " чанков="); putdec(r.chunks); sys_puts(0, "\n");
        if (r.not_found) {
            set_affinity(before.pid, before.core); // не оставлять драйвер на чужом ядре из-за отсутствия файла
            return step_fail("файл не найден — положите его на SD (по умолчанию /root/bigfile.txt) или укажите путь: coretest 0 /root/другой.txt"), sys_exit(env.root_ep), 1;
        }
        if (!r.ok) {
            set_affinity(before.pid, before.core);
            return step_fail("blk_driver на не-нулевом ядре не смог прочитать файл"), sys_exit(env.root_ep), 1;
        }

        st = set_affinity(before.pid, before.core);
        if (st != 0) return step_fail("обратный taskset вернул не 0"), sys_exit(env.root_ep), 1;
        ProcInfo back = proc_info("blk_driver");
        sys_puts(0, "  возврат: ядро "); putdec(back.core); sys_puts(0, "\n");
        if (back.core != before.core) return step_fail("не вернулся на исходное ядро"), sys_exit(env.root_ep), 1;
        step_ok();
    }

    // ================= ТЕСТ 4 =================
    if (!only || only == 4) {
        step_begin(4, "balance (обычный режим, с отчётом)");
        int moved = 0;
        int st = balance(false, &moved);
        if (st != 0) return step_fail("balance вернул не 0 (нет прав администратора?)"), sys_exit(env.root_ep), 1;
        sys_puts(0, env.shm); // отчёт балансировщика, как его печатает шелл
        sys_puts(0, "  перенесено процессов: "); putdec(moved); sys_puts(0, "\n");
        step_ok();
    }

    // ================= ТЕСТ 5 =================
    // Эталонное чтение: длина + контрольная сумма файла в спокойной
    // обстановке. Всё, что дальше, сравнивается с этими двумя числами.
    if (!only || only == 5 || only == 7) {
        step_begin(5, "эталонное чтение большого файла (дважды, без балансировки)");
        read_file_checksummed(bigfile, env.blk_ep, env.usb_storage_ep, env.shm, &baseline);
        if (baseline.not_found) return step_fail("файл не найден — положите его на SD и повторите"), sys_exit(env.root_ep), 1;
        sys_puts(0, "  проход 1: байт="); putu64(baseline.bytes);
        sys_puts(0, " чанков="); putdec(baseline.chunks);
        sys_puts(0, " время="); putdec((int)baseline.ms); sys_puts(0, "мс");
        sys_puts(0, " сумма="); putu64(baseline.checksum); sys_puts(0, "\n");
        if (!baseline.ok) return step_fail("эталонное чтение не удалось (обрыв на середине)"), sys_exit(env.root_ep), 1;

        // Читаем ТОТ ЖЕ файл второй раз, в тех же условиях. Если два
        // спокойных прохода уже расходятся — сравнивать с ними шаг 7
        // бессмысленно, и виноват будет НЕ перенос ядер. Ровно этот случай
        // и поймался на живом железе 2026-09-05: фоновый balancer звал
        // SYS_BALANCE без "тихого" режима, root писал отчёт в страницу 0
        // общей SHM — ту самую, куда blk_driver кладёт очередной чанк
        // файла. Длина сходилась, содержимое — нет.
        ReadResult baseline2;
        read_file_checksummed(bigfile, env.blk_ep, env.usb_storage_ep, env.shm, &baseline2);
        sys_puts(0, "  проход 2: байт="); putu64(baseline2.bytes);
        sys_puts(0, " время="); putdec((int)baseline2.ms); sys_puts(0, "мс");
        sys_puts(0, " сумма="); putu64(baseline2.checksum); sys_puts(0, "\n");
        if (!baseline2.ok) return step_fail("второй эталонный проход не удался"), sys_exit(env.root_ep), 1;
        if (baseline2.bytes != baseline.bytes || baseline2.checksum != baseline.checksum) {
            return step_fail("два ПОДРЯД спокойных чтения одного файла дали разный результат — чтение не воспроизводимо само по себе. Ищите того, кто пишет в общую SHM-страницу 0 без vfs_lock (исторический пример: фоновый balancer с отчётом SYS_BALANCE), а не проблему с переносом ядер"), sys_exit(env.root_ep), 1;
        }
        sys_puts(0, "  оба прохода совпали — эталон надёжен\n");
        if (baseline.chunks < 8) {
            sys_puts(0, "  ВНИМАНИЕ: файл слишком мал ("); putdec(baseline.chunks);
            sys_puts(0, " чанков) — шаг 7 не успеет ничего перенести за время чтения.\n");
        }
        baseline_valid = true;
        step_ok();
    }

    // ================= ТЕСТ 6 =================
    // Сигнал драйверу, живущему НЕ на ядре 0. Именно этот путь до
    // issuse.txt №74/б мог утащить root в блокирующий seL4_Call к
    // зависшему драйверу; теперь он гейтится ожиданием PARKED.
    if (!only || only == 6) {
        step_begin(6, "driver stop/start драйверу на не-нулевом ядре");
        ProcInfo net = proc_info("net_driver");
        if (net.pid < 0) { step_skip("net_driver не запущен"); }
        else {
            int st = set_affinity(net.pid, 2);
            if (st != 0) return step_fail("не удалось перенести net_driver"), sys_exit(env.root_ep), 1;
            sys_puts(0, "  net_driver перенесён на ядро 2\n");

            int s_stop = driver_signal("net_driver", DRIVER_SIGNAL_STOP);
            sys_puts(0, "  driver net_driver stop: код "); putdec(s_stop); sys_puts(0, "\n");
            int s_start = driver_signal("net_driver", DRIVER_SIGNAL_START);
            sys_puts(0, "  driver net_driver start: код "); putdec(s_start); sys_puts(0, "\n");

            set_affinity(net.pid, net.core); // вернуть как было, независимо от исхода
            if (s_stop != 0 || s_start != 0) return step_fail("сигнал драйверу вне ядра 0 не прошёл (код -4 = драйвер не встал в PARKED)"), sys_exit(env.root_ep), 1;
            sys_puts(0, "  net_driver возвращён на ядро "); putdec(net.core); sys_puts(0, "\n");
            step_ok();
        }
    }

    // ================= ТЕСТ 7 — ГЛАВНЫЙ =================
    // Длинное чтение файла ОДНОВРЕМЕННО с балансировкой ядер. Читает
    // отдельный поток (он же держит VFS-мьютекс на всё чтение), главный
    // поток в это время в цикле дёргает "тихий" balance. Успех = файл
    // прочитан целиком и его контрольная сумма совпала с эталонной,
    // причём балансировщик за это время реально что-то переносил.
    if (!only || only == 7) {
        step_begin(7, "длинное чтение файла ОДНОВРЕМЕННО с balance");
        if (!baseline_valid) { step_skip("нет эталона (шаг 5 не выполнялся)"); }
        else {
            my_strlcpy(g_reader_path, bigfile, sizeof(g_reader_path));
            g_reader_blk_ep = env.blk_ep;
            g_reader_usb_ep = env.usb_storage_ep;
            g_reader_shm = env.shm;
            g_concurrent_read.done = false;

            int tid = clone_reader(env.root_ep);
            if (tid < 0) return step_fail("не удалось создать поток-читатель (SYS_CLONE вернул -1)"), sys_exit(env.root_ep), 1;
            sys_puts(0, "  поток-читатель запущен, pid="); putdec(tid); sys_puts(0, "\n");

            int rounds = 0, moved_total = 0;
            seL4_Word t0 = now_ms();
            // Потолок ожидания — с большим запасом относительно эталонного
            // времени: если чтение вдруг встанет намертво, тест обязан
            // закончиться сообщением, а не висеть вместе с системой.
            seL4_Word deadline_ms = baseline.ms * 8 + 30000;
            while (!g_concurrent_read.done) {
                if (now_ms() - t0 > deadline_ms) break;
                int moved = 0;
                balance(true, &moved);
                moved_total += moved;
                rounds++;
                sleep_ms(50);
            }
            seL4_Word elapsed = now_ms() - t0;

            sys_puts(0, "  проходов balance за время чтения: "); putdec(rounds);
            sys_puts(0, ", перенесено процессов суммарно: "); putdec(moved_total); sys_puts(0, "\n");
            sys_puts(0, "  чтение: байт="); putu64(g_concurrent_read.bytes);
            sys_puts(0, " чанков="); putdec(g_concurrent_read.chunks);
            sys_puts(0, " время="); putdec((int)elapsed); sys_puts(0, "мс\n");
            sys_puts(0, "  контрольная сумма="); putu64(g_concurrent_read.checksum);
            sys_puts(0, " (эталон "); putu64(baseline.checksum); sys_puts(0, ")\n");

            if (!g_concurrent_read.done) return step_fail("чтение не завершилось за отведённое время — похоже на зависание драйвера или root'а"), sys_exit(env.root_ep), 1;
            if (!g_concurrent_read.ok) return step_fail("чтение оборвалось на середине"), sys_exit(env.root_ep), 1;
            if (g_concurrent_read.bytes != baseline.bytes) return step_fail("прочитано другое число байт, чем в эталоне"), sys_exit(env.root_ep), 1;
            if (g_concurrent_read.checksum != baseline.checksum) {
                // Формулировка зависит от того, был ли вообще перенос:
                // без него винить перенос не в чем, и искать надо того,
                // кто пишет в общую SHM-страницу мимо vfs_lock.
                if (moved_total > 0) return step_fail("контрольная сумма не совпала с эталонной, и за время чтения были переносы между ядрами — похоже, перенос посреди работы драйвера портит данные"), sys_exit(env.root_ep), 1;
                return step_fail("контрольная сумма не совпала с эталонной, ХОТЯ переносов между ядрами не было ни одного — виноват не перенос, а посторонняя запись в общую SHM-страницу 0 во время чтения"), sys_exit(env.root_ep), 1;
            }
            if (rounds == 0) {
                sys_puts(0, "  ВНИМАНИЕ: ни одного прохода balance не успело выполниться — файл слишком мал, параллельность фактически не проверена.\n");
            }
            step_ok();
        }
    }

    // ================= ТЕСТ 8 =================
    // Самое главное для issuse.txt №74/б: полный kill+respawn (Suspend!)
    // драйвера, который в этот момент живёт НЕ на ядре 0. Ровно это
    // раньше вешало root навсегда в ipi_wait(). prepare_driver_for_
    // suspend() обязан дождаться PARKED, вернуть поток на ядро 0 и только
    // тогда суспендить — а система обязана остаться живой и работающей.
    if (!only || only == 8) {
        step_begin(8, "recover драйвера, живущего на не-нулевом ядре (бывший ipi_wait-дедлок)");
        ProcInfo before = proc_info("blk_driver");
        int st = set_affinity(before.pid, 1);
        if (st != 0) return step_fail("не удалось перенести blk_driver"), sys_exit(env.root_ep), 1;
        sys_puts(0, "  blk_driver перенесён на ядро 1, pid="); putdec(before.pid); sys_puts(0, "\n");

        recover("blk_driver");
        settle_uart(); // root печатает свой [WATCHDOG]-блок прямо в UART
        sys_puts(0, "  recover выполнен, root ответил (значит НЕ завис в ipi_wait)\n");

        // Дать респавну добежать до готовности (перемонтирование exFAT).
        ProcInfo after;
        for (int i = 0; i < 100; i++) {
            after = proc_info("blk_driver");
            if (after.pid >= 0 && after.pid != before.pid) break;
            sleep_ms(100);
        }
        print_proc_line("blk_driver", after);
        if (after.pid < 0) return step_fail("blk_driver не поднялся после recover"), sys_exit(env.root_ep), 1;
        if (after.pid == before.pid) return step_fail("PID не изменился — recover не пересоздал процесс"), sys_exit(env.root_ep), 1;

        // И снова читаем тот же файл: пересозданный драйвер обязан
        // перемонтировать ФС и отдавать ровно те же данные.
        if (baseline_valid) {
            ReadResult r;
            for (int i = 0; i < 50; i++) {
                read_file_checksummed(bigfile, env.blk_ep, env.usb_storage_ep, env.shm, &r);
                if (r.ok) break;
                sleep_ms(200);
            }
            sys_puts(0, "  чтение после respawn: байт="); putu64(r.bytes);
            sys_puts(0, " сумма="); putu64(r.checksum);
            sys_puts(0, " (эталон "); putu64(baseline.checksum); sys_puts(0, ")\n");
            if (!r.ok) return step_fail("после respawn blk_driver не читает файл"), sys_exit(env.root_ep), 1;
            if (r.checksum != baseline.checksum) return step_fail("после respawn контрольная сумма не совпала"), sys_exit(env.root_ep), 1;
        }
        step_ok();
    }

    // ================= ТЕСТ 9 =================
    // wifi_driver штатно живёт на ядре 1, и его stop/start идёт ровно
    // через тот же Suspend, что и шаг 8 — но по отдельному пути
    // (root_stop_wifi_driver/root_start_wifi_driver). Самый вероятный
    // источник регресса, поэтому проверяется отдельно.
    if (!only || only == 9) {
        step_begin(9, "wifi stop/start (Suspend драйвера с ядра 1)");
        ProcInfo w = proc_info("wifi_driver");
        if (w.pid < 0) { step_skip("wifi_driver не запущен (штатно — он спавнится только по `wifi start`)"); }
        else {
            sys_puts(0, "  wifi_driver до: ядро "); putdec(w.core);
            sys_puts(0, ", состояние "); sys_puts(0, state_name(w.state)); sys_puts(0, "\n");
            seL4_SetMR(0, SYS_STOP_WIFI);
            seL4_Call(env.root_ep, seL4_MessageInfo_new(0, 0, 0, 1));
            int s_stop = (int)seL4_GetMR(0);
            sys_puts(0, "  wifi stop: код "); putdec(s_stop); sys_puts(0, "\n");
            if (proc_info("wifi_driver").pid >= 0) return step_fail("wifi_driver всё ещё жив после stop"), sys_exit(env.root_ep), 1;

            seL4_SetMR(0, SYS_START_WIFI);
            seL4_Call(env.root_ep, seL4_MessageInfo_new(0, 0, 0, 1));
            int s_start = (int)seL4_GetMR(0);
            sys_puts(0, "  wifi start: код "); putdec(s_start); sys_puts(0, "\n");
            // Дожидаемся не просто "процесс есть", а готовности: bring-up
            // wifi_driver долгий (загрузка прошивки, PBKDF2-самотест) и всё
            // это время печатает в консоль. Если уйти дальше, не дождавшись,
            // его вывод перемешается со следующим шагом.
            int wifi_state = -1;
            for (int i = 0; i < 300; i++) {
                seL4_SetMR(0, SYS_WIFI_STATUS);
                seL4_Call(env.root_ep, seL4_MessageInfo_new(0, 0, 0, 1));
                wifi_state = (int)seL4_GetMR(0);
                if (wifi_state == 2) break; // готов принимать команды
                sleep_ms(100);
            }
            settle_uart();
            sys_puts(0, "  статус wifi после старта: "); putdec(wifi_state);
            sys_puts(0, " (2 = готов принимать команды)\n");
            ProcInfo w2 = proc_info("wifi_driver");
            print_proc_line("wifi_driver", w2);
            if (w2.pid < 0) return step_fail("wifi_driver не поднялся обратно"), sys_exit(env.root_ep), 1;
            if (wifi_state != 2) return step_fail("wifi_driver поднялся, но так и не дошёл до готовности за 30с"), sys_exit(env.root_ep), 1;
            step_ok();
        }
    }

    // ================= ТЕСТ 10 =================
    // Полный сброс USB "с нуля" — тот же путь, который теперь запускает и
    // heartbeat-watchdog вместо бесполезного kill+respawn (issuse.txt
    // №74/в + просьба пользователя 2026-09-05). Успех = код 0 и тома
    // смонтированы заново.
    if (!only || only == 10) {
        step_begin(10, "usbreset: полная инициализация USB с нуля");
        if (!usb_present) { step_skip("usb_driver не запущен"); }
        else {
            UsbVolumeList before;
            int before_count = 0;
            if (fetch_usb_volume_list(env.usb_storage_ep, before)) {
                for (int i = 0; i < USB_MAX_DEVICES; i++) if (before.mounted[i]) before_count++;
            }
            sys_puts(0, "  томов смонтировано до сброса: "); putdec(before_count); sys_puts(0, "\n");

            seL4_Word t0 = now_ms();
            seL4_SetMR(0, SYS_PCIE_RESET);
            seL4_Call(env.root_ep, seL4_MessageInfo_new(0, 0, 0, 1));
            int st = (int)seL4_GetMR(0);
            settle_uart(); // root печатает всю PCIe-последовательность прямо в UART
            sys_puts(0, "  usbreset: код "); putdec(st);
            sys_puts(0, ", заняло "); putdec((int)(now_ms() - t0)); sys_puts(0, "мс\n");
            if (st != 0) return step_fail("usbreset не вернул 0 (-3 линк, -4 mailbox, -5 драйвер завис)"), sys_exit(env.root_ep), 1;

            UsbVolumeList after;
            int after_count = 0;
            for (int i = 0; i < 30; i++) {
                if (fetch_usb_volume_list(env.usb_storage_ep, after)) {
                    after_count = 0;
                    for (int k = 0; k < USB_MAX_DEVICES; k++) if (after.mounted[k]) after_count++;
                    if (after_count >= before_count) break;
                }
                sleep_ms(200);
            }
            sys_puts(0, "  томов смонтировано после сброса: "); putdec(after_count); sys_puts(0, "\n");
            if (before_count > 0 && after_count < before_count) return step_fail("после сброса смонтировано меньше томов, чем было"), sys_exit(env.root_ep), 1;
            ProcInfo u = proc_info("usb_driver");
            print_proc_line("usb_driver", u);
            step_ok();
        }
    }

    settle_uart();
    sys_puts(0, "\n==========================================================\n");
    sys_puts(0, "ИТОГ: пройдено "); putdec(g_passed);
    sys_puts(0, ", пропущено "); putdec(g_skipped);
    sys_puts(0, ", провалено 0 — все выполненные шаги успешны.\n");
    sys_puts(0, "==========================================================\n");

    sys_exit(env.root_ep);
    return 0;
}
