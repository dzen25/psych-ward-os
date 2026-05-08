#include <sel4/sel4.h>

__attribute__((weak)) LIBSEL4_THREAD_LOCAL seL4_IPCBuffer *__sel4_ipc_buffer = nullptr;
void __assert_fail(const char *assertion, const char *file, int line, const char *function) { while(1); }

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
static char pipe_buffer[4096];

static void sys_puts_direct(seL4_CPtr console_ep, const char *str) {
    while (*str) {
        seL4_SetMR(0, 8); // 8 = SYS_PUTS
        seL4_SetMR(1, (seL4_Word)*str++);
        seL4_Call(console_ep, seL4_MessageInfo_new(0, 0, 0, 2));
    }
}

static void sys_puts(seL4_CPtr console_ep, const char *str) {
    if (is_piping) {
        // Если труба открыта — складываем текст в буфер Оболочки
        int curr_len = my_strlen(pipe_buffer);
        int str_len = my_strlen(str);
        if (curr_len + str_len < 4095) {
            my_strcpy(pipe_buffer + curr_len, str);
        }
    } else {
        // Если трубы нет — печатаем на экран по-настоящему
        sys_puts_direct(console_ep, str);
    }
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

static char current_working_dir[64] = "/";

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

static int vfs_syscall(int syscall_num) {
    volatile int* mailbox = (volatile int*)(0x502000 + 4084 - 12);
    mailbox[2] = 0; // Сбрасываем флаг готовности
    mailbox[0] = syscall_num; // Кладем команду в ящик
    
    while (mailbox[2] == 0) {
        seL4_Yield(); // Ждем, пока blk_driver не сделает работу
    }
    return mailbox[1]; // Возвращаем статус (0 или -1)
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
        // 1. Формируем динамический промпт локально в буфере
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
            // ==========================================
            // ПАРСЕР КОНВЕЙЕРА (PIPES)
            // ==========================================
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
                pipe_buffer[0] = '\0'; // Очищаем буфер для новых данных
            } else {
                is_piping = false;
            }
            // ==========================================

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
                char *shm = (char*)0x502000; 
                // Если есть аргумент - берем его, иначе берем текущую папку (пустую строку)
                if (arg) {
                    build_absolute_path(shm, arg);
                } else {
                    build_absolute_path(shm, "");
                }
                vfs_syscall(110);
                sys_puts(console_ep, shm);
            }

            else if (my_strcmp(cmd, "pwd") == 0) {
                sys_puts(console_ep, current_working_dir);
                sys_puts(console_ep, "\n");
            }

            else if (my_strcmp(cmd, "mkdir") == 0) {
                if (!arg) { sys_puts(console_ep, "Usage: mkdir <path>\n"); continue; }
                char *shm = (char*)0x502000;
                build_absolute_path(shm, arg);
                
                // МАГИЯ "mkdir -p": идем по строке и создаем папки шаг за шагом
                int fail = 0;
                for (int i = 1; shm[i] != '\0'; i++) {
                    if (shm[i] == '/') {
                        shm[i] = '\0'; // Временно отрезаем хвост пути
                        if (vfs_syscall(109) != 0) fail = 1;
                        shm[i] = '/';  // Приклеиваем слэш обратно
                    }
                }
                
                // Создаем финальную папку (весь путь целиком)
                if (vfs_syscall(109) != 0) fail = 1;
                
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
                
                if (vfs_syscall(111) == 0) {
                    my_strcpy(current_working_dir, shm);
                } else {
                    sys_puts(console_ep, "No such directory.\n");
                }
            }

            else if (my_strcmp(cmd, "ps") == 0) {
                __sel4_ipc_buffer->msg[0] = 104; seL4_Call(root_ep, seL4_MessageInfo_new(0, 0, 0, 1));
                sys_puts(console_ep, shm);
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

            else if (my_strcmp(cmd, "touch") == 0) {
                if (!arg) { sys_puts(console_ep, "Usage: touch <file>\n"); continue; }
                char *shm = (char*)0x502000;
                build_absolute_path(shm, arg);
                if (vfs_syscall(112) == 0) sys_puts(console_ep, "File created.\n");
                else sys_puts(console_ep, "Failed to create file.\n");
            }

            else if (my_strcmp(cmd, "cat") == 0) {
                if (!arg) { sys_puts(console_ep, "Usage: cat <file>\n"); continue; }
                char *shm = (char*)0x502000;
                build_absolute_path(shm, arg);
                
                if (vfs_syscall(114) == 0) {
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
                    
                    if (vfs_syscall(113) != 0) {
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
                sys_puts(console_ep, "Available: help, time, sleep, ls, ps, cat, echo, exec, kill, exit, shm, pid, mkdir, cd, pwd\n");
            }

            else if (my_strcmp(cmd, "exit") == 0) {
                sys_puts(console_ep, "Exiting sandbox...\n");
                sys_exit(root_ep);
            }

            else if (my_strcmp(cmd, "hack_disk") == 0) {
                // Вызываем наш тестовый сисколл на перезапись!
                vfs_syscall(115);
                char *shm = (char*)0x502000;
                sys_puts(console_ep, shm);
            }

            else if (my_strcmp(cmd, "create_file") == 0) {
                char *shm = (char*)0x502000;
                // Формат: ИМЯ(11 символов) + '|' + Текст
                strcpy(shm, "NEWFILE TXT|This file was built from SCRATCH by Psych Ward OS using raw DMA cluster allocation!");
                
                vfs_syscall(116); // Вызываем наш новый код создания!
                sys_puts(console_ep, shm);
            }

            else { sys_puts(console_ep, "Unknown command. Type 'help'.\n"); }

            if (is_piping) {
                is_piping = false; // Закрываем трубу (клапан переключается на консоль)
                
                char *arg2 = cmd2;
                while (*arg2 && *arg2 != ' ') arg2++;
                if (*arg2 == ' ') { *arg2 = '\0'; arg2++; } else { arg2 = nullptr; }
                
                if (my_strcmp(cmd2, "grep") == 0) {
                    if (!arg2) { sys_puts_direct(console_ep, "Usage: <cmd> | grep <text>\n"); continue; }
                    
                    // Идем по нашему сохраненному буферу построчно
                    char *line = pipe_buffer;
                    while (*line) {
                        char *next_line = line;
                        while (*next_line && *next_line != '\n') next_line++;
                        char saved = *next_line;
                        if (*next_line == '\n') *next_line = '\0'; // Временно отрезаем строку
                        
                        // Поиск подстроки (arg2) внутри строки (line)
                        char *search = line;
                        bool found = false;
                        while (*search) {
                            char *p1 = search;
                            char *p2 = arg2;
                            while (*p1 && *p2 && *p1 == *p2) { p1++; p2++; }
                            if (!*p2) { found = true; break; } // Совпадение найдено!
                            search++;
                        }
                        
                        if (found) {
                            sys_puts_direct(console_ep, line);
                            sys_puts_direct(console_ep, "\n");
                        }
                        
                        // Восстанавливаем строку и идем дальше
                        if (saved == '\n') {
                            *next_line = '\n';
                            line = next_line + 1;
                        } else {
                            break;
                        }
                    }
                } else {
                    sys_puts_direct(console_ep, "Microkernel Pipe currently supports 'grep' on the right side.\n");
                }
            }
        }
    }
}