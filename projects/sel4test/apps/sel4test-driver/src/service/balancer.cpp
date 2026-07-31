#include "h/sys_client.h"

// Фаза 9.B (см. ROADMAP.md): фоновый сервис, автозапускается из
// /etc/init.conf. Периодически сам зовёт SYS_BALANCE вместо ручной команды
// `balance` в шелле — закрывает пункт "периодический авто-запуск balance"
// из планов Фазы 6.1.

constexpr seL4_Word BALANCER_DEFAULT_INTERVAL_MS = 5000;
constexpr const char* BALANCER_CONF_PATH = "/conf/balancer_conf/balancer.conf";

static void sys_sleep(seL4_CPtr timer_ep, seL4_Word ms) {
    seL4_SetMR(0, 8); // SYS_SLEEP_MS
    seL4_SetMR(1, ms);
    seL4_Call(timer_ep, seL4_MessageInfo_new(0, 0, 0, 2));
}

// Читает /conf/balancer_conf/balancer.conf — одно число (интервал в мс),
// остальное содержимое игнорируется. Файла нет — тихо используем
// BALANCER_DEFAULT_INTERVAL_MS, это не ошибка (конфиг опционален). Но если
// файл ЕСТЬ и число в нём не нашлось (issuse.txt: раньше комментарий ДО
// числа означал тихий fallback без единого предупреждения) — теперь
// пропускаем пустые строки и строки-комментарии (# или ; первым
// непробельным символом) перед числом, а если после этого числа всё равно
// нет — печатаем предупреждение вместо молчания.
static seL4_Word read_configured_interval(SysClientEnv &env) {
    if (!env.shm) return BALANCER_DEFAULT_INTERVAL_MS;

    my_strlcpy(env.shm, BALANCER_CONF_PATH, SHM_TOTAL_SIZE); // путь абсолютный, build_absolute_path не нужен
    if (vfs_syscall(114, env.blk_ep) != 0) return BALANCER_DEFAULT_INTERVAL_MS; // файла нет — конфиг опционален

    char *p = env.shm;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
        if (*p == '#' || *p == ';') { while (*p && *p != '\n') p++; continue; }
        break;
    }

    if (*p < '0' || *p > '9') {
        sys_write(1, "[balancer] предупреждение: "); sys_write(1, BALANCER_CONF_PATH);
        sys_write(1, " не начинается с числа (после пропуска пустых строк/комментариев) — использую интервал по умолчанию.\n");
        return BALANCER_DEFAULT_INTERVAL_MS;
    }

    seL4_Word val = 0;
    while (*p >= '0' && *p <= '9') { val = val * 10 + (seL4_Word)(*p - '0'); p++; }
    if (val == 0) {
        sys_write(1, "[balancer] предупреждение: интервал в конфиге равен 0 — использую интервал по умолчанию.\n");
        return BALANCER_DEFAULT_INTERVAL_MS;
    }
    return val;
}

int main(int argc, char *argv[]) {
    SysClientEnv env;
    sys_client_init(env);

    seL4_IPCBuffer *ipc = get_local_ipc();
    seL4_CPtr timer_ep = ipc->msg[BOOT_TIMER_EP];

    seL4_Word interval_ms = read_configured_interval(env);

    while (1) {
        sys_sleep(timer_ep, interval_ms);
        seL4_SetMR(0, SYS_BALANCE);
        seL4_Call(env.root_ep, seL4_MessageInfo_new(0, 0, 0, 1));
    }

    return 0; // недостижимо
}
