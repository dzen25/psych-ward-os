#include <sel4/sel4.h>
#include "common.h"
#include <stdint.h>
#include "pipe.h"

static volatile int* vfs_spinlock_ptr = (volatile int*)(0x502000 + 4084 - 16);

void vfs_lock() {
    while (__sync_lock_test_and_set(vfs_spinlock_ptr, 1)) {
        seL4_Yield(); 
    }
}

void vfs_unlock() {
    __sync_lock_release(vfs_spinlock_ptr);
}

void __assert_fail(const char *assertion, const char *file, int line, const char *function) { while(1); }

static inline seL4_IPCBuffer* get_local_ipc() {
    seL4_Word tls_addr;
    asm volatile("mrs %0, tpidrro_el0" : "=r"(tls_addr)); 
    return (seL4_IPCBuffer*)(tls_addr - 1024);
}

// Пример правильного sys_puts для драйвера:
static void sys_puts(seL4_CPtr console_ep, const char *str) {
    seL4_IPCBuffer *ipc = get_local_ipc();
    int len = 0;
    while(str[len]) len++;
    
    ipc->msg[0] = 8; // SYS_PUTS
    for (int i = 0; i < len; i++) {
        ipc->msg[i + 1] = str[i];
    }
    seL4_Call(console_ep, seL4_MessageInfo_new(0, 0, 0, len + 1));
}

static void my_strcpy(char *dest, const char *src) {
    while ((*dest++ = *src++));
}

#define strcpy my_strcpy

static int my_strcmp(const char *s1, const char *s2) { 
    while (*s1 && (*s1 == *s2)) { s1++; s2++; } return *(const unsigned char*)s1 - *(const unsigned char*)s2; 
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
static __attribute__((aligned(8))) uint8_t global_pipe_memory[4096]; 
#define global_pipe (*((volatile ipc_pipe*)global_pipe_memory))

enum NetCommand {
    NET_CMD_PING = 1,
    NET_CMD_SEND = 2,
    NET_CMD_STATUS = 3,
    NET_CMD_RESOLVE = 4,
};

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

static void net_send_text_command(seL4_CPtr net_ep, seL4_Word cmd, seL4_Word ip, seL4_Word port, const char *text) {
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

    int word_count = (text_len + word_bytes - 1) / word_bytes;
    for (int w = 0; w < word_count; w++) {
        seL4_Word packed = 0;
        for (int b = 0; b < word_bytes; b++) {
            int idx = w * word_bytes + b;
            if (idx < text_len) {
                packed |= ((seL4_Word)(uint8_t)clipped[idx]) << (b * 8);
            }
        }
        seL4_SetMR(4 + w, packed);
    }

    seL4_Send(net_ep, seL4_MessageInfo_new(0, 0, 0, 4 + word_count));
    seL4_Yield();
}

static void sys_puts_direct(seL4_CPtr console_ep, const char *str) {
    seL4_IPCBuffer *ipc = get_local_ipc();
    // Пишем строку в MR регистры IPC буфера
    seL4_Word len = my_strlen(str);
    if (len > 40) len = 40; // Ограничение на размер одного сообщения в регистрах

    // По спецификации твоей ОС, SYS_PUTS подготавливает данные в буфере
    ipc->msg[0] = 8; // SYS_PUTS ID
    for (seL4_Word i = 0; i < len; i++) {
        ipc->msg[i + 1] = str[i];
    }
    seL4_Call(console_ep, seL4_MessageInfo_new(0, 0, 0, len + 1));
}

struct ThreadArgs {
    void (*func)(void*);
    seL4_CPtr timer_ep;
    seL4_CPtr console_ep;
};

static void sys_thread_exit() {
    // 1. Получаем безопасный указатель на буфер текущего потока
    seL4_IPCBuffer *ipc = get_local_ipc(); 
    
    // 2. Везде используем локальный 'ipc' вместо глобального макроса
    seL4_CPtr root_ep = ipc->msg[BOOT_ROOT_EP];
    ipc->msg[0] = 105; // ID нашего нового сисколла
    
    seL4_Call(root_ep, seL4_MessageInfo_new(0, 0, 0, 1));
    while(1) seL4_Yield(); // Сюда выполнение никогда не дойдет
}

static void internal_thread_wrapper(void* raw_args) {
    seL4_Word tls_addr;
    asm volatile("mrs %0, tpidr_el0" : "=r"(tls_addr));
    seL4_SetIPCBuffer((seL4_IPCBuffer*)(tls_addr - 1024));

    ThreadArgs* args = (ThreadArgs*)raw_args;
    args->func(raw_args); 
    sys_thread_exit();
}

static int spawn_thread(void (*func)(void*), seL4_CPtr timer, seL4_CPtr console) {
    // 2. Сдвигаем ThreadArgs на 2 КБ вперед, чтобы не затереть путь VFS в начале SHM
    ThreadArgs* args = (ThreadArgs*)(0x502000 + 2048); 
    args->func = func;
    args->timer_ep = timer;
    args->console_ep = console;
    
    // Динамически берем Root Endpoint для сисколлов из нашего легитимного TLS
    seL4_CPtr my_root_syscall_ep = get_local_ipc()->msg[BOOT_ROOT_EP];

    get_local_ipc()->msg[0] = 101; // SYS_CLONE
    get_local_ipc()->msg[1] = (seL4_Word)internal_thread_wrapper;
    get_local_ipc()->msg[2] = (seL4_Word)args;
    
    seL4_MessageInfo_t info = seL4_MessageInfo_new(0, 0, 0, 3);
    // Делаем сисколл к ядру через наш локальный root эндпоинт
    seL4_Call(my_root_syscall_ep, info);
    
    return seL4_GetMR(0);
}

static char sys_read(seL4_CPtr console_ep) {
    seL4_SetMR(0, 6); // 6 = SYS_READ
    seL4_Call(console_ep, seL4_MessageInfo_new(0, 0, 0, 1));
    return (char)seL4_GetMR(0);
}

static seL4_Word sys_get_time(seL4_CPtr timer_ep) {
    seL4_SetMR(0, 3); // 3 = SYS_GET_TIME
    seL4_Call(timer_ep, seL4_MessageInfo_new(0, 0, 0, 1));
    return seL4_GetMR(0);
}

static void sys_sleep(seL4_CPtr timer_ep, seL4_Word ms) {
    seL4_Word start = sys_get_time(timer_ep);
    while (sys_get_time(timer_ep) - start < ms) { seL4_Yield(); }
}

static void sys_recover(const char* driver_name) {
    seL4_IPCBuffer *ipc = get_local_ipc();
    seL4_CPtr root_ep = ipc->msg[BOOT_ROOT_EP];
    char *shm = (char*)0x502000;
    my_strcpy(shm, driver_name);      // Передаем имя упавшего драйвера
    ipc->msg[0] = 117;  // SYS_RECOVER
    seL4_Call(root_ep, seL4_MessageInfo_new(0, 0, 0, 1));
}

static void wait_for_net_mailbox(seL4_CPtr console_ep, seL4_CPtr timer_ep, int timeout_ms) {
    volatile int* net_mailbox = (volatile int*)(0x502000 + 4060);
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
    }
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

static char current_working_dir[64] = "/";
static char arg_buffer[512];

static void build_absolute_path(char* target, const char* arg) {
    if (arg[0] == '/') {
        my_strcpy(target, arg); // Уже абсолютный
        return;
    }
    my_strcpy(target, current_working_dir);
    int len = my_strlen(target);
    if (target[len-1] != '/') { target[len] = '/'; target[len+1] = '\0'; }
    my_strcpy(target + my_strlen(target), arg);
}

static int vfs_syscall(int syscall_num, seL4_CPtr console_ep, seL4_CPtr timer_ep, int timeout_ms) {
    vfs_lock();
    volatile int* mailbox = (volatile int*)(0x502000 + 4084 - 12); // blk_driver mailbox
    mailbox[2] = 0; // Сбрасываем флаг готовности
    mailbox[0] = syscall_num; // Кладем команду в ящик
    
    int elapsed = 0;
    // Ждем пока драйвер диска ответит, но не дольше timeout_ms
    while (mailbox[2] == 0 && elapsed < timeout_ms) {
        // Временно освобождаем лок, чтобы другие потоки могли работать, пока мы ждем
        vfs_unlock();
        sys_sleep(timer_ep, 100);
        elapsed += 100;
        vfs_lock(); // Снова захватываем лок перед проверкой условия
    }
    
    if (mailbox[2] == 0) {
        sys_puts_direct(console_ep, "\n[SHELL] Error: Disk/VFS operation timed out (");
        char buf[10]; int s = timeout_ms / 1000, j = 0;
        if (s == 0) buf[j++] = '0';
        while(s > 0) { buf[j++] = (s % 10) + '0'; s /= 10; }
        while(j > 0) { char c[2] = {buf[--j], 0}; sys_puts_direct(console_ep, c); }
        sys_puts_direct(console_ep, "s). Unblocking shell.\n");
        mailbox[2] = 1; // Насильно снимаем блокировку
        
        // НОВОЕ: Перезапуск зависшего дискового драйвера!
        sys_puts_direct(console_ep, "[SHELL] Initiating emergency recovery for blk_driver...\n");
        sys_recover("blk_driver");
        
        return -1; 
    }
    
    int ret_val = mailbox[1]; // Нормальный возврат статуса
    vfs_unlock();
    return ret_val;
}

static void ls_thread_func(void* arg) {
    ThreadArgs* args = (ThreadArgs*)arg;
    seL4_CPtr console_ep = args->console_ep;
    seL4_CPtr timer_ep = args->timer_ep;
    
    char *shm = (char*)0x502000;
    vfs_syscall(110, console_ep, timer_ep, 2000); 
    sys_puts(console_ep, shm);
    global_pipe.writer_closed = true;
    return;
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

    if (my_ep == 0) {
        __assert_fail("FATAL: Null Capability #0 Detected!", __FILE__, __LINE__, __func__);
    }

    my_strcpy(arg_buffer, (char*)&ipc->msg[0]);

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
    
    sys_puts(console_ep, "\n======================================================\n");
    sys_puts(console_ep, "  TRUE MICROKERNEL: ALL MODULES ONLINE & FUNCTIONAL!  \n");
    sys_puts(console_ep, "======================================================\n\n");

    // 4. Демонстрация (заменено на cmd_argc/cmd_argv)
    if (cmd_argc > 1) {
        sys_puts(console_ep, "[Shell Init] Started with arguments:\n");
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
        // Добавляем текущую директорию!
        my_strcpy(prompt + len, current_working_dir);
        len = my_strlen(prompt);
        prompt[len++] = '>'; prompt[len++] = ' '; prompt[len] = '\0';
        
        sys_puts(console_ep, prompt);
        
        char cmd[64]; int i = 0;
        
        // 2. Читаем ввод с защитой от непечатных символов и ANSI-мусора
        while (i < 63) {
            char c = sys_read(console_ep); 
            
            if (c == (char)-1 || c == (char)255) { seL4_Yield(); continue; }
            if (c == '\r' || c == '\n') { sys_puts(console_ep, "\n"); break; }
            else if (c == 127 || c == '\b') { 
                if (i > 0) { i--; sys_puts(console_ep, "\b \b"); } 
            } 
            // ИСПРАВЛЕНИЕ: Берем только печатные символы (игнорируем стрелочки и спецкоды)
            else if (c >= 32 && c <= 126) { 
                char tmp[2] = {c, 0}; 
                sys_puts(console_ep, tmp); 
                cmd[i++] = c; 
            }
        }
        cmd[i] = '\0';
        
        if (i > 0) {

            char *pipe_sym = cmd;
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
                while (left_end >= cmd && *left_end == ' ') {
                    *left_end = '\0';
                    left_end--;
                }
                
                is_piping = true; // Открываем трубу!
                pipe_init(&global_pipe);
            } else {
                is_piping = false;
            }

            char *arg = cmd; while (*arg && *arg != ' ') arg++;
            if (*arg == ' ') { *arg = '\0'; arg++; while (*arg == ' ') arg++; } else { arg = nullptr; }

            char *shm = (char*)0x502000; // Адрес разделяемой памяти (Shared Memory)

            if (my_strcmp(cmd, "time") == 0) {
                seL4_Word current = sys_get_time(timer_ep);
                sys_puts(console_ep, "Uptime: ");
                char buf[16]; int temp = current, j = 0;
                if (temp == 0) buf[j++] = '0';
                while(temp > 0) { buf[j++] = (temp % 10) + '0'; temp /= 10; }
                while(j > 0) { char c[2] = {buf[--j], 0}; sys_puts(console_ep, c); }
                sys_puts(console_ep, " ms\n");
            } 
             
            else if (my_strcmp(cmd, "sleep") == 0) {
                sys_puts(console_ep, "Sleeping 3 seconds...\n");
                sys_sleep(timer_ep, 3000);
                sys_puts(console_ep, "Woke up!\n");
            }

            else if (my_strcmp(cmd, "ping") == 0) {
                if (!arg) { sys_puts(console_ep, "Usage: ping <domain_or_ip> [count]\n"); continue; }
                
                char *cursor = arg;
                char *target_str = next_token(&cursor);
                char *count_str = next_token(&cursor);
                uint8_t ip[4];
                uint16_t count = 1;

                if (count_str && parse_port(count_str, &count) != 0) count = 1;

                volatile int* net_mailbox = (volatile int*)(0x502000 + 4060);
                
                // Пытаемся распарсить как IP. Если не вышло — это домен!
                if (parse_ipv4(target_str, ip) != 0) {
                    sys_puts(console_ep, "[SHELL] Target looks like a domain. Starting DNS resolution...\n");
                    net_mailbox[0] = 1;
                    net_send_text_command(net_ep, NET_CMD_RESOLVE, 0, 0, target_str);
                    
                    wait_for_net_mailbox(console_ep, timer_ep, 10000);
                    
                    if (net_mailbox[0] == 0) {
                        seL4_Word packed_ip = *((seL4_Word*)(0x502000 + 4064));
                        if (packed_ip == 0) {
                            sys_puts(console_ep, "[SHELL] DNS Error: Domain not found.\n");
                            continue;
                        }
                        ip[0] = (packed_ip >> 24) & 0xFF; ip[1] = (packed_ip >> 16) & 0xFF;
                        ip[2] = (packed_ip >> 8) & 0xFF;  ip[3] = packed_ip & 0xFF;
                    } else {
                        sys_puts(console_ep, "[SHELL] DNS Resolution failed.\n");
                        continue;
                    }
                }

                // Теперь у нас точно есть IP (распарсенный или полученный от DNS)
                net_mailbox[0] = 1;
                net_send_text_command(net_ep, NET_CMD_PING, pack_ipv4(ip), count, nullptr);
                
                int timeout = 5000;
                if (count * 2000 + 2000 > timeout) timeout = count * 2000 + 2000;
                wait_for_net_mailbox(console_ep, timer_ep, timeout);
            }

            else if (my_strcmp(cmd, "send") == 0) {
                if (!arg) { sys_puts(console_ep, "Usage: send <text>\n"); continue; }
                if (net_ep == 0) { sys_puts(console_ep, "Net driver endpoint is unavailable.\n"); continue; }

                uint8_t ip[4] = {10, 0, 2, 2};
                
                volatile int* net_mailbox = (volatile int*)(0x502000 + 4060);
                net_mailbox[0] = 1; // Запираем Mailbox!

                sys_puts(console_ep, "UDP datagram queued for 10.0.2.2:8080.\n");
                net_send_text_command(net_ep, NET_CMD_SEND, pack_ipv4(ip), 8080, arg);

                wait_for_net_mailbox(console_ep, timer_ep, 5000);
            }

            else if (my_strcmp(cmd, "sendto") == 0) {
                if (!arg) { sys_puts(console_ep, "Usage: sendto <ip_address> <port> <text>\n"); continue; }
                if (net_ep == 0) { sys_puts(console_ep, "Net driver endpoint is unavailable.\n"); continue; }

                char *cursor = arg;
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
                    sys_puts(console_ep, "Usage: sendto <ip_address> <port> <text>\n");
                    continue;
                }

                volatile int* net_mailbox = (volatile int*)(0x502000 + 4060);
                net_mailbox[0] = 1; // Запираем Mailbox!

                sys_puts(console_ep, "UDP datagram queued.\n");
                net_send_text_command(net_ep, NET_CMD_SEND, pack_ipv4(ip), port, text);

                wait_for_net_mailbox(console_ep, timer_ep, 5000);
            }

            else if (my_strcmp(cmd, "netstat") == 0) {
                if (net_ep == 0) { sys_puts(console_ep, "Net driver endpoint is unavailable.\n"); continue; }
                
                volatile int* net_mailbox = (volatile int*)(0x502000 + 4060);
                net_mailbox[0] = 1; // Запираем Mailbox!
                
                sys_puts(console_ep, "Net status requested.\n");
                net_send_text_command(net_ep, NET_CMD_STATUS, 0, 0, nullptr);
                
                wait_for_net_mailbox(console_ep, timer_ep, 2000); // Для статуса достаточно 2-х секунд
            }

            else if (my_strcmp(cmd, "ls") == 0) {
                char *shm = (char*)0x502000; 
                if (arg) { build_absolute_path(shm, arg); } 
                else { build_absolute_path(shm, ""); }
                
                if (is_piping) { 
                    // Если есть пайп (|), кидаем задачу в фоновый поток
                    spawn_thread(ls_thread_func, timer_ep, console_ep);
                } else {
                    // Если пайпа нет, работаем как обычно
                    vfs_syscall(110, console_ep, timer_ep, 2000); // Таймаут 2 сек
                    sys_puts(console_ep, shm);
                }
            }

            else if (my_strcmp(cmd, "pwd") == 0) {
                sys_puts(console_ep, current_working_dir);
                sys_puts(console_ep, "\n");
            }

            else if (my_strcmp(cmd, "mkdir") == 0) {
                if (!arg) { sys_puts(console_ep, "Usage: mkdir <path>\n"); continue; }
                char *shm = (char*)0x502000;
                build_absolute_path(shm, arg);
                
                int fail = 0;
                for (int i = 1; shm[i] != '\0'; i++) {
                    if (shm[i] == '/') {
                        shm[i] = '\0'; 
                        if (vfs_syscall(109, console_ep, timer_ep, 5000) != 0) fail = 1; // Таймаут 5 сек
                        shm[i] = '/';  
                    }
                }
                
                if (vfs_syscall(109, console_ep, timer_ep, 5000) != 0) fail = 1; // Таймаут 5 сек
                
                if (!fail) sys_puts(console_ep, "Directory tree created.\n");
                else sys_puts(console_ep, "Failed to create directory tree.\n");
            }

            else if (my_strcmp(cmd, "cd") == 0) {
                // 1. Если просто "cd" без аргументов — прыгаем в корень
                if (!arg || arg[0] == '\0') { 
                    my_strcpy(current_working_dir, "/"); 
                    continue; 
                }
                
                // 2. Если "cd /" — тоже прыгаем в корень
                if (my_strcmp(arg, "/") == 0) {
                    my_strcpy(current_working_dir, "/");
                    continue;
                }
                
                // 3. Обработка перехода на уровень вверх "cd .."
                if (my_strcmp(arg, "..") == 0) {
                    int len = my_strlen(current_working_dir);
                    if (len > 1) { // Если мы не в корне
                        len--; // Сдвигаемся с нулевого байта
                        if (current_working_dir[len] == '/') len--; // Пропускаем возможный слеш на конце
                        
                        // Идем назад, пока не встретим предыдущий слеш
                        while (len > 0 && current_working_dir[len] != '/') {
                            len--;
                        }
                        
                        // Если дошли до начала пути, значит мы вернулись в корень
                        if (len == 0) {
                            my_strcpy(current_working_dir, "/");
                        } else {
                            current_working_dir[len] = '\0'; // Отрезаем последнюю папку
                        }
                    }
                    continue;
                }
                
                // 4. Обычный переход в папку
                char *shm = (char*)0x502000;
                build_absolute_path(shm, arg);
                
                if (vfs_syscall(111, console_ep, timer_ep, 5000) == 0) {
                    my_strcpy(current_working_dir, shm);
                } else {
                    sys_puts(console_ep, "No such directory.\n");
                }
            }

            else if (my_strcmp(cmd, "ps") == 0) {
                seL4_IPCBuffer *ipc = get_local_ipc();

                ipc->msg[0] = 104; seL4_Call(root_ep, seL4_MessageInfo_new(0, 0, 0, 1));
                sys_puts(console_ep, shm);
            }

            else if (my_strcmp(cmd, "kill") == 0) {
                seL4_IPCBuffer *ipc = get_local_ipc();
                if (!arg) { sys_puts(console_ep, "Usage: kill <pid>\n"); continue; }
                ipc->msg[0] = 102; // SYS_KILL
                ipc->msg[1] = simple_atoi(arg);
                seL4_Call(root_ep, seL4_MessageInfo_new(0, 0, 0, 2));
                sys_puts(console_ep, "Signal sent.\n");
            }

            else if (my_strcmp(cmd, "exec") == 0) {
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

                my_strcpy(shm, arg);
                ipc->msg[0] = 100; // SYS_EXEC
                seL4_Call(root_ep, seL4_MessageInfo_new(0, 0, 0, 1));
                
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

            else if (my_strcmp(cmd, "shm") == 0) {
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

            else if (my_strcmp(cmd, "pid") == 0) {
                sys_puts(console_ep, "Current Shell PID: ");
                char buf[16]; int temp = my_pid, j = 0;
                if (temp == 0) buf[j++] = '0';
                while(temp > 0) { buf[j++] = (temp % 10) + '0'; temp /= 10; }
                while(j > 0) { char c[2] = {buf[--j], 0}; sys_puts(console_ep, c); }
                sys_puts(console_ep, "\n");
            }

            else if (my_strcmp(cmd, "touch") == 0) {
                if (!arg) { sys_puts(console_ep, "Usage: touch <file>\n"); continue; }
                char *shm = (char*)0x502000;
                build_absolute_path(shm, arg);
                
                if (vfs_syscall(112, console_ep, timer_ep, 5000) == 0) sys_puts(console_ep, "File created.\n"); // 5 сек
                else sys_puts(console_ep, "Failed to create file.\n");
            }

            else if (my_strcmp(cmd, "cat") == 0) {
                if (!arg) { sys_puts(console_ep, "Usage: cat <file>\n"); continue; }
                char *shm = (char*)0x502000;
                build_absolute_path(shm, arg);
                
                if (vfs_syscall(114, console_ep, timer_ep, 5000) == 0) { // Чтение: 5 сек
                    sys_puts(console_ep, shm);
                    sys_puts(console_ep, "\n");
                } else {
                    sys_puts(console_ep, "File not found or is a directory.\n");
                }
            }

            else if (my_strcmp(cmd, "echo") == 0) {
                if (!arg) { sys_puts(console_ep, "\n"); continue; }
                
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
                    
                    char *shm = (char*)0x502000;
                    char *path_ptr = shm;
                    char *text_ptr = shm + 128; // Текст кладем со смещением!
                    
                    build_absolute_path(path_ptr, redir);
                    my_strcpy(text_ptr, arg);
                    
                    if (vfs_syscall(113, console_ep, timer_ep, 5000) != 0) { // Запись текста: 5 сек
                        sys_puts(console_ep, "Failed to write to file.\n");
                    }
                    // Если все ок - молчим, как настоящий bash!
                } else {
                    // Обычный echo без перенаправления
                    sys_puts(console_ep, arg);
                    sys_puts(console_ep, "\n");
                }
            }

            else if (my_strcmp(cmd, "help") == 0) {
                sys_puts(console_ep, "Available: help, time, sleep, ls, ps, cat, echo, exec, kill, exit, shm, pid, mkdir, cd, pwd, ping, send, sendto, netstat\n");
            }

            else if (my_strcmp(cmd, "exit") == 0) {
                sys_puts(console_ep, "Exiting sandbox...\n");
                sys_exit(root_ep);
            }
            
            // ==========================================
            // НОВОЕ: ТРИГГЕРЫ АППАРАТНЫХ КРАШЕЙ // Краш-тест - удалить
            // ==========================================
            else if (my_strcmp(cmd, "crash_shell") == 0) {
                sys_puts(console_ep, "[SHELL] Initiating intentional Segfault (Null Pointer Dereference)...\n");
                volatile int* boom = (volatile int*)0x0;
                *boom = 0xDEAD; // Оболочка умрет на этой строке
            }

            else if (my_strcmp(cmd, "crash_disk") == 0) {
                sys_puts(console_ep, "[SHELL] Sending poison pill to blk_driver...\n");
                vfs_syscall(118, console_ep, timer_ep, 5000); // Оправляем команду умереть
            }
            // ==========================================

            else if (my_strcmp(cmd, "hack_disk") == 0) {
                vfs_syscall(115, console_ep, timer_ep, 7000); // Жесткая операция: 7 сек
                char *shm = (char*)0x502000;
                sys_puts(console_ep, shm);
            }

            else if (my_strcmp(cmd, "create_file") == 0) {
                char *shm = (char*)0x502000;
                strcpy(shm, "NEWFILE TXT|This file was built from SCRATCH by Psych Ward OS using raw DMA cluster allocation!");
                
                vfs_syscall(116, console_ep, timer_ep, 7000); // Поиск кластеров FAT32: 7 сек
                sys_puts(console_ep, shm);
            }

            else { sys_puts(console_ep, "Unknown command. Type 'help'.\n"); }

            if (is_piping) {
                    // Если левая команда была НЕ 'ls', значит она отработала мгновенно (например, help)
                    // и нам нужно вручную закрыть задвижку. (А вот ls_thread закрывает её сам изнутри потока)
                    if (my_strcmp(cmd, "ls") != 0) {
                        global_pipe.writer_closed = true;
                    }

                    char *arg2 = cmd2;
                    while (*arg2 && *arg2 != ' ') arg2++;
                    if (*arg2 == ' ') { *arg2 = '\0'; arg2++; } else { arg2 = nullptr; }

                    if (my_strcmp(cmd2, "grep") == 0) {
                        if (!arg2) { sys_puts_direct(console_ep, "Usage: <cmd> | grep <text>\n"); continue; }

                        char line[256];
                        int line_idx = 0;
                        uint8_t c;

                        // Главный поток (grep) будет вычитывать байты по мере того,
                        // как фоновый поток (ls_thread) будет их туда писать параллельно!
                        while (pipe_read(&global_pipe, 0, 0, &c, 1) > 0) {
                            if (c == '\n' || line_idx >= 255) {
                                line[line_idx] = '\0'; // Конец строки найден

                                char *search = line;
                                bool found = false;
                                while (*search) {
                                    char *p1 = search;
                                    char *p2 = arg2;
                                    while (*p1 && *p2 && *p1 == *p2) { p1++; p2++; }
                                    if (!*p2) { found = true; break; }
                                    search++;
                                }

                                if (found) {
                                    sys_puts_direct(console_ep, line);
                                    sys_puts_direct(console_ep, "\n");
                                }
                                line_idx = 0; // Сбрасываем буфер
                            } else {
                                line[line_idx++] = c;
                            }
                        }
                    } else {
                        sys_puts_direct(console_ep, "Microkernel Pipe currently supports 'grep' on the right side.\n");
                    }

                    // Сбрасываем флаг только после того, как grep закончил работу
                    is_piping = false; 
                }
        }
    }

    return 0;
}