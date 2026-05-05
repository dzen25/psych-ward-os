#include "patient.h"
#include "uart.h"

// Выделяем память под стек пациента
alignas(16) uint8_t patient_stack[4096];

// Выделяем отдельную изолированную память под локальное хранилище потока (TLS)
alignas(16) uint8_t patient_tls[1024]; 

// Память для второго изолированного потока (Доктора)
alignas(16) uint8_t doctor_stack[4096];
alignas(16) uint8_t doctor_tls[1024]; 

// --- ОБЕРТКИ ДЛЯ СИСТЕМНЫХ ВЫЗОВОВ ---
static void sys_print(seL4_CPtr ep, seL4_Word val) {
    seL4_SetMR(0, 1); // SYS_PRINT
    seL4_SetMR(1, val);
    seL4_Call(ep, seL4_MessageInfo_new(0, 0, 0, 2));
}

static seL4_Word sys_get_time(seL4_CPtr ep) {
    seL4_SetMR(0, 3); // SYS_GET_TIME
    seL4_Call(ep, seL4_MessageInfo_new(0, 0, 0, 1));
    return seL4_GetMR(0);
}

static void sys_sleep(seL4_CPtr ep, seL4_Word ms) {
    seL4_SetMR(0, 4); // SYS_SLEEP
    seL4_SetMR(1, ms);
    seL4_Call(ep, seL4_MessageInfo_new(0, 0, 0, 2));
}

static void sys_putchar(seL4_CPtr ep, char c) {
    seL4_SetMR(0, 5); // SYS_PUTCHAR
    seL4_SetMR(1, c);
    seL4_Call(ep, seL4_MessageInfo_new(0, 0, 0, 2));
}

static char sys_read(seL4_CPtr ep) {
    seL4_SetMR(0, 6); // SYS_READ
    seL4_Call(ep, seL4_MessageInfo_new(0, 0, 0, 1));
    return (char)seL4_GetMR(0);
}

static void sys_puts(seL4_CPtr ep, const char* str) {
    while (*str) {
        sys_putchar(ep, *str++);
    }
}

static void sys_puthex(seL4_CPtr ep, uint64_t val) {
    const char hex[] = "0123456789abcdef";
    char buf[17];
    buf[16] = 0;
    for (int i = 15; i >= 0; i--) {
        buf[i] = hex[val & 0xF];
        val >>= 4;
    }
    sys_puts(ep, buf);
}

static int simple_atoi(const char *str) {
    int res = 0;
    while (*str >= '0' && *str <= '9') {
        res = res * 10 + (*str - '0');
        str++;
    }
    return res;
}

// --- ПОТОК ДОКТОРА ---
void doctor_thread(seL4_CPtr ep, seL4_Word ipc_buf, seL4_CPtr med_ep) {
    seL4_SetIPCBuffer((seL4_IPCBuffer*)ipc_buf);
    
    while (1) {
        seL4_Word badge;
        seL4_Recv(med_ep, &badge); // Ждем пациента
        
        // Эмулируем сложную работу (без сисколлов, чтобы не затереть reply capability!)
        for (volatile int i = 0; i < 40000000; i++) {} 
        
        seL4_SetMR(0, 0xC0FFEE); // Выписываем рецепт и возвращаем результат
        seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
    }
}

// --- ПОТОК ПАЦИЕНТА ---
void patient_thread(seL4_CPtr ep, seL4_Word ipc_buf, seL4_CPtr med_ep) {
    // 1. Инициализируем указатель TLS
    seL4_SetIPCBuffer((seL4_IPCBuffer*)ipc_buf);
    
    sys_puts(ep, "\n=== Welcome to Patient Shell ===\n");
    sys_puts(ep, "Type 'time' to get current system time.\n");
    sys_puts(ep, "Type 'sleep <ms>' to pause execution.\n");
    sys_puts(ep, "Type 'doctor' to call doctor_thread via IPC.\n");
    sys_puts(ep, "Type 'alloc' to request dynamic memory mapping.\n");
    
    while (1) {
        sys_puts(ep, "patient> ");
        
        char cmd[64];
        int i = 0;
        bool in_escape = false;

        while (i < 63) {
            char c = sys_read(ep);
            
            // Игнорируем ANSI escape-последовательности (например, стрелки)
            if (c == 27) { // ESC
                in_escape = true;
                continue;
            }
            if (in_escape) {
                if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '~') {
                    in_escape = false; // Конец последовательности
                }
                continue;
            }

            if (c == '\r' || c == '\n') {
                sys_putchar(ep, '\n');
                break;
            } else if (c == 127 || c == 8) { // Корректная обработка Backspace
                if (i > 0) {
                    i--;
                    sys_puts(ep, "\b \b");
                }
            } else {
                sys_putchar(ep, c); // Эхо-вывод на экран того, что ввел пользователь
                cmd[i++] = c;
            }
        }
        cmd[i] = '\0';
        
        if (i > 0) {
            // --- Простой парсер команд ---
            char *arg = cmd;
            while (*arg && *arg != ' ') {
                arg++;
            }
            // Если нашли пробел, завершаем команду нулем и сдвигаем указатель на аргумент
            if (*arg == ' ') {
                *arg = '\0';
                arg++;
                // Пропускаем лишние пробелы перед аргументом
                while (*arg == ' ') {
                    arg++;
                }
                if (*arg == '\0') { // Если после пробелов ничего нет
                    arg = NULL;
                }
            } else {
                arg = NULL; // Аргументов нет
            }

            if (my_strcmp(cmd, "time") == 0) {
                seL4_Word current_time = sys_get_time(ep);
                sys_print(ep, current_time);
            } else if (my_strcmp(cmd, "sleep") == 0) {
                if (arg) {
                    int ms = simple_atoi(arg);
                    sys_puts(ep, "Sleeping...\n");
                    sys_sleep(ep, ms);
                    sys_puts(ep, "Awake!\n");
                } else {
                    sys_puts(ep, "Usage: sleep <milliseconds>\n");
                }
            } else if (my_strcmp(cmd, "hello") == 0) {
                sys_puts(ep, "Hello from Psych Ward OS user-space!\n");
            } else if (my_strcmp(cmd, "clear") == 0) {
                sys_puts(ep, "\x1b[2J\x1b[H"); // Команда терминалу на очистку экрана
            } else if (my_strcmp(cmd, "echo") == 0) {
                if (arg) { sys_puts(ep, arg); }
                sys_puts(ep, "\n");
            } else if (my_strcmp(cmd, "doctor") == 0) {
                sys_puts(ep, "Calling doctor thread via IPC...\n");
                // Синхронный вызов (блокирует пациента, пока доктор не ответит)
                seL4_Call(med_ep, seL4_MessageInfo_new(0, 0, 0, 0));
                
                // ИСПРАВЛЕНИЕ: Сохраняем рецепт ДО вывода текста
                seL4_Word prescription = seL4_GetMR(0); 
                
                sys_puts(ep, "Doctor returned prescription: 0x");
                sys_puthex(ep, prescription); 
                sys_puts(ep, "\n");
            } else if (my_strcmp(cmd, "alloc") == 0) {
                seL4_SetMR(0, 7); // SYS_ALLOC
                seL4_Call(ep, seL4_MessageInfo_new(0, 0, 0, 1));
                
                // Читаем ответ ДО вывода текста
                seL4_Word mapped_vaddr = seL4_GetMR(0);
                
                sys_puts(ep, "Kernel mapped a new 4KB page at VAddr: 0x");
                sys_puthex(ep, mapped_vaddr); 
                sys_puts(ep, "\n");
            } else {
                sys_puts(ep, "Unknown command: ");
                sys_puts(ep, cmd);
                sys_puts(ep, "\n");
            }
        }
    }
}