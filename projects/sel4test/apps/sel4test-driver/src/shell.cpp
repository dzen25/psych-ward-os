#include <sel4/sel4.h>

__attribute__((weak)) LIBSEL4_THREAD_LOCAL seL4_IPCBuffer *__sel4_ipc_buffer = nullptr;
void __assert_fail(const char *assertion, const char *file, int line, const char *function) { while(1); }

static int my_strcmp(const char *s1, const char *s2) { 
    while (*s1 && (*s1 == *s2)) { s1++; s2++; } return *(const unsigned char*)s1 - *(const unsigned char*)s2; 
}

static seL4_Word my_strlen(const char *s) {
    seL4_Word len = 0; while (*s++) len++; return len;
}

static void my_strcpy(char *dest, const char *src) {
    while ((*dest++ = *src++));
}

static int simple_atoi(const char *str) {
    int res = 0;
    while (*str >= '0' && *str <= '9') { res = res * 10 + (*str - '0'); str++; }
    return res;
}

static void sys_puts(seL4_CPtr console_ep, const char* str) {
    int i = 0; seL4_SetMR(0, 8); // 8 = SYS_PUTS
    while (*str && i < 110) { seL4_SetMR(i + 1, (seL4_Word)*str++); i++; }
    seL4_Call(console_ep, seL4_MessageInfo_new(0, 0, 0, i + 1));
    if (*str) sys_puts(console_ep, str);
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

static void sys_wait(seL4_CPtr root_ep, int pid) {
    __sel4_ipc_buffer->msg[0] = 106; // SYS_WAIT
    __sel4_ipc_buffer->msg[1] = pid;
    seL4_Call(root_ep, seL4_MessageInfo_new(0, 0, 0, 2));
}

static void sys_exit(seL4_CPtr root_ep) {
    __sel4_ipc_buffer->msg[0] = 103; // SYS_EXIT
    seL4_Call(root_ep, seL4_MessageInfo_new(0, 0, 0, 1));
    while(1) seL4_Yield(); // Сюда мы никогда не дойдем, Ядро нас убьет
}

static int sys_shm_get(seL4_CPtr root_ep, int shm_id, seL4_Word vaddr) {
    __sel4_ipc_buffer->msg[0] = 107; // SYS_SHM_GET
    __sel4_ipc_buffer->msg[1] = shm_id;
    __sel4_ipc_buffer->msg[2] = vaddr;
    seL4_Call(root_ep, seL4_MessageInfo_new(0, 0, 0, 3));
    return (int)seL4_GetMR(0);
}

static int sys_getpid(seL4_CPtr root_ep) {
    __sel4_ipc_buffer->msg[0] = 108; // SYS_GETPID
    seL4_Call(root_ep, seL4_MessageInfo_new(0, 0, 0, 1));
    return (int)seL4_GetMR(0);
}

// --- Точка входа ---
extern "C" void __sel4_start_c(void) {
    seL4_Word fake_tls_base = 0x501800; 
    asm volatile("msr tpidr_el0, %0" :: "r"(fake_tls_base));
    __sel4_ipc_buffer = (seL4_IPCBuffer*)0x501000;

    seL4_CPtr root_ep = __sel4_ipc_buffer->userData;
    seL4_CPtr console_ep = __sel4_ipc_buffer->caps_or_badges[0];
    seL4_CPtr timer_ep = __sel4_ipc_buffer->caps_or_badges[1];

    // ==========================================================
    // СБОРКА ARGC И ARGV ИЗ PAYLOAD
    // ==========================================================
    // ==========================================================
    // ИСПРАВЛЕНИЕ: СПАСАЕМ АРГУМЕНТЫ ИЗ IPC-БУФЕРА
    // ==========================================================
    // Локальное хранилище для строки аргументов, чтобы сисколлы её не затерли
    static char arg_buffer[512]; 
    my_strcpy(arg_buffer, (char*)&__sel4_ipc_buffer->msg[0]);

    int argc = 1;
    char *argv[16];
    argv[0] = (char*)"shell";
    
    // Теперь парсим нашу СПАСЕННУЮ копию, а не сам IPC-буфер!
    char *cmd_args = arg_buffer; 
    
    if (cmd_args[0] != '\0') {
        argv[argc++] = cmd_args;
        
        while (*cmd_args) {
            if (*cmd_args == ' ') {
                *cmd_args = '\0';
                
                // Пропускаем лишние пробелы (если пользователь ввел два пробела подряд)
                while (*(cmd_args + 1) == ' ') cmd_args++;
                
                if (*(cmd_args + 1) != '\0') {
                    argv[argc++] = cmd_args + 1;
                }
            }
            cmd_args++;
            if (argc >= 15) break;
        }
    }
    argv[argc] = nullptr;
    // ==========================================================
    
    sys_puts(console_ep, "\n======================================================\n");
    sys_puts(console_ep, "  TRUE MICROKERNEL: ALL MODULES ONLINE & FUNCTIONAL!  \n");
    sys_puts(console_ep, "======================================================\n\n");

    // Демонстрация: выводим полученные аргументы
    if (argc > 1) {
        sys_puts(console_ep, "[Shell Init] Started with arguments:\n");
        for (int j = 0; j < argc; j++) {
            sys_puts(console_ep, "  argv[");
            char buf[2] = {(char)(j + '0'), 0}; sys_puts(console_ep, buf);
            sys_puts(console_ep, "] = ");
            sys_puts(console_ep, argv[j]);
            sys_puts(console_ep, "\n");
        }
    }

    // ==========================================================
    // ПРАГМАТИЧНЫЙ ФОНОВЫЙ РЕЖИМ (DAEMON MODE)
    // ==========================================================
    bool is_daemon = false;
    for (int j = 1; j < argc; j++) {
        if (my_strcmp(argv[j], "--daemon") == 0) {
            is_daemon = true;
            break;
        }
    }

    if (is_daemon) {
        sys_puts(console_ep, "[Daemon] Mode engaged. TTY input disabled.\n");
        int ticks = 0;
        // Фоновый цикл: делаем "полезную работу" (например, стучим в лог каждые 10 сек)
        // ВАЖНО: Мы НЕ вызываем sys_read(), поэтому TTY остается свободен!
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
    
    // --- Оригинальный интерактивный цикл (для Foreground) ---
    while (1) {
        // 1. Формируем динамический промпт локально в буфере (чтобы не спамить IPC-вызовами)
        char prompt[32];
        my_strcpy(prompt, "sandbox[");
        
        int temp_pid = my_pid, p_idx = 0;
        char pid_buf[8];
        if (temp_pid == 0) pid_buf[p_idx++] = '0';
        while (temp_pid > 0) { pid_buf[p_idx++] = (temp_pid % 10) + '0'; temp_pid /= 10; }
        
        int len = my_strlen(prompt);
        while (p_idx > 0) { prompt[len++] = pid_buf[--p_idx]; }
        
        prompt[len++] = ']';
        prompt[len++] = '>';
        prompt[len++] = ' ';
        prompt[len] = '\0';
        
        // Отправляем готовую строку одним системным вызовом!
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
            // Разделяем команду и аргументы
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

            else if (my_strcmp(cmd, "ls") == 0) {
                __sel4_ipc_buffer->msg[0] = 101; seL4_Call(root_ep, seL4_MessageInfo_new(0, 0, 0, 1));
                sys_puts(console_ep, shm);
            }

            else if (my_strcmp(cmd, "ps") == 0) {
                __sel4_ipc_buffer->msg[0] = 104; seL4_Call(root_ep, seL4_MessageInfo_new(0, 0, 0, 1));
                sys_puts(console_ep, shm);
            }

            else if (my_strcmp(cmd, "echo") == 0) {
                if (!arg) { sys_puts(console_ep, "Usage: echo <text> <filename>\n"); continue; }
                char *text = arg; char *fname = arg;
                while (*fname && *fname != ' ') fname++;
                if (*fname == ' ') { *fname = '\0'; fname++; }

                my_strcpy(shm, fname);
                char *shm_content = shm + my_strlen(fname) + 1;
                my_strcpy(shm_content, text);

                __sel4_ipc_buffer->msg[0] = 105; // SYS_WRITEFILE
                __sel4_ipc_buffer->msg[1] = my_strlen(text);
                seL4_Call(root_ep, seL4_MessageInfo_new(0, 0, 0, 2));
                sys_puts(console_ep, "File saved to RAM disk.\n");
            }

            else if (my_strcmp(cmd, "cat") == 0) {
                if (!arg) { sys_puts(console_ep, "Usage: cat <filename>\n"); continue; }
                my_strcpy(shm, arg);
                __sel4_ipc_buffer->msg[0] = 98; // SYS_READFILE
                seL4_Call(root_ep, seL4_MessageInfo_new(0, 0, 0, 1));
                if ((int)seL4_GetMR(0) != -1) {
                    sys_puts(console_ep, "---\n"); sys_puts(console_ep, shm); sys_puts(console_ep, "\n---\n");
                } else { sys_puts(console_ep, "File not found.\n"); }
            }

            else if (my_strcmp(cmd, "kill") == 0) {
                if (!arg) { sys_puts(console_ep, "Usage: kill <pid>\n"); continue; }
                __sel4_ipc_buffer->msg[0] = 102; // SYS_KILL
                __sel4_ipc_buffer->msg[1] = simple_atoi(arg);
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
                __sel4_ipc_buffer->msg[0] = 100; // SYS_EXEC
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

            else if (my_strcmp(cmd, "help") == 0) {
                sys_puts(console_ep, "Available: help, time, sleep, ls, ps, cat, echo, exec, kill, exit, shm, pid\n");
            }

            else if (my_strcmp(cmd, "exit") == 0) {
                sys_puts(console_ep, "Exiting sandbox...\n");
                sys_exit(root_ep);
            }

            else { sys_puts(console_ep, "Unknown command. Type 'help'.\n"); }
        }
    }
}