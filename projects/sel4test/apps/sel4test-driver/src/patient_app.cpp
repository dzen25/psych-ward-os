#include <sel4/sel4.h>

__attribute__((weak)) LIBSEL4_THREAD_LOCAL seL4_IPCBuffer *__sel4_ipc_buffer = nullptr;

void __assert_fail(const char *assertion, const char *file, int line, const char *function) {
    while (1); 
}

// === ОБЕРТКИ ДЛЯ СИСТЕМНЫХ ВЫЗОВОВ ===
static void sys_putchar(seL4_CPtr ep, seL4_Word c) {
    seL4_SetMR(0, 5); seL4_SetMR(1, c);
    seL4_Call(ep, seL4_MessageInfo_new(0, 0, 0, 2));
}

static void sys_puts(seL4_CPtr ep, const char* str) {
    while (*str) sys_putchar(ep, (seL4_Word)*str++);
}

static char sys_read(seL4_CPtr ep) {
    seL4_SetMR(0, 6); seL4_Call(ep, seL4_MessageInfo_new(0, 0, 0, 1));
    return (char)seL4_GetMR(0);
}

static void sys_print(seL4_CPtr ep, seL4_Word val) {
    seL4_SetMR(0, 1); seL4_SetMR(1, val);
    seL4_Call(ep, seL4_MessageInfo_new(0, 0, 0, 2));
}

static seL4_Word sys_get_time(seL4_CPtr ep) {
    seL4_SetMR(0, 3); seL4_Call(ep, seL4_MessageInfo_new(0, 0, 0, 1));
    return seL4_GetMR(0);
}

static void sys_sleep(seL4_CPtr ep, seL4_Word ms) {
    seL4_SetMR(0, 4); seL4_SetMR(1, ms);
    seL4_Call(ep, seL4_MessageInfo_new(0, 0, 0, 2));
}

static void sys_puthex(seL4_CPtr ep, seL4_Word val) {
    const char hex[] = "0123456789abcdef";
    char buf[17]; buf[16] = 0;
    for (int i = 15; i >= 0; i--) { buf[i] = hex[val & 0xF]; val >>= 4; }
    sys_puts(ep, buf);
}

static void sys_exit(seL4_CPtr ep) {
    seL4_SetMR(0, 102); // SYS_EXIT
    seL4_Call(ep, seL4_MessageInfo_new(0, 0, 0, 1));
    while(1);
}

static void sys_kill(seL4_CPtr ep, int pid) {
    seL4_SetMR(0, 103); // SYS_KILL
    seL4_SetMR(1, pid);
    seL4_Call(ep, seL4_MessageInfo_new(0, 0, 0, 2));
}

static int simple_atoi(const char *str) {
    int res = 0;
    while (*str >= '0' && *str <= '9') { res = res * 10 + (*str - '0'); str++; }
    return res;
}

static int my_strcmp(const char *s1, const char *s2) {
    while (*s1 && (*s1 == *s2)) { s1++; s2++; }
    return *(const unsigned char*)s1 - *(const unsigned char*)s2;
}

static void my_strcpy(char *dest, const char *src) {
    while (*src) { *dest++ = *src++; }
    *dest = '\0';
}

// === ТОЧКА ВХОДА ===
extern "C" void __sel4_start_c(void) {
    seL4_Word fake_tls_base = 0x501800; 
    asm volatile("msr tpidr_el0, %0" :: "r"(fake_tls_base));

    seL4_IPCBuffer *ipc = (seL4_IPCBuffer*)0x501000;
    __sel4_ipc_buffer = ipc;

    seL4_CPtr ep = ipc->userData;
    seL4_CPtr med_ep = ipc->caps_or_badges[0];
    
    sys_puts(ep, "\n======================================================\n");
    sys_puts(ep, "  GOD MODE UNLOCKED: DEMAND PAGING & VFS ACTIVE!      \n");
    sys_puts(ep, "======================================================\n\n");
    
    while (1) {
        sys_puts(ep, "sandbox> ");
        char cmd[64]; int i = 0;
        
        while (i < 63) {
            char c = sys_read(ep);
            if (c == '\r' || c == '\n') { sys_putchar(ep, '\n'); break; }
            else if (c == 127 || c == '\b') { 
                if (i > 0) { i--; sys_puts(ep, "\b \b"); }
            } else { 
                sys_putchar(ep, c); cmd[i++] = c; 
            }
        }
        cmd[i] = '\0';
        
        if (i > 0) {
            char *arg = cmd;
            while (*arg && *arg != ' ') arg++;
            if (*arg == ' ') { 
                *arg = '\0'; arg++; 
                while (*arg == ' ') arg++; 
            } else { arg = 0; }

            if (my_strcmp(cmd, "time") == 0) {
                sys_print(ep, sys_get_time(ep));
            } else if (my_strcmp(cmd, "sleep") == 0) {
                if (arg) { sys_sleep(ep, simple_atoi(arg)); sys_puts(ep, "Awake!\n"); }
            } else if (my_strcmp(cmd, "doctor") == 0) {
                char *shm = (char*)0x502000;
                my_strcpy(shm, "Doctor! The voices in the RAM are getting louder!");
                ipc->msg[0] = 99; seL4_Call(med_ep, seL4_MessageInfo_new(0, 0, 0, 1));
                sys_puts(ep, "Doctor's prescription: "); sys_puts(ep, shm); sys_puts(ep, "\n");
            
            // НОВОЕ: ДИНАМИЧЕСКИЙ ЗАПУСК
            } else if (my_strcmp(cmd, "exec") == 0) {
                if (!arg) { sys_puts(ep, "Usage: exec <filename>\n"); continue; }
                char *shm = (char*)0x502000;
                my_strcpy(shm, arg);
                ipc->msg[0] = 100; // SYS_EXEC
                seL4_Call(ep, seL4_MessageInfo_new(0, 0, 0, 1));
                seL4_Word res = seL4_GetMR(0);
                if (res == (seL4_Word)-1) {
                    sys_puts(ep, "Execution failed: File not found.\n");
                } else {
                sys_puts(ep, "Process launched successfully! PID: ");
                sys_print(ep, res);
                sys_puts(ep, "\n");
                }

        } else if (my_strcmp(cmd, "exit") == 0) {
            sys_puts(ep, "Goodbye!\n");
            sys_exit(ep);
        } else if (my_strcmp(cmd, "kill") == 0) {
            if (!arg) { sys_puts(ep, "Usage: kill <pid>\n"); continue; }
            int target = simple_atoi(arg);
            sys_kill(ep, target);
            sys_puts(ep, "Process terminated.\n");
            
            // НОВОЕ: ТЕСТ ВИРТУАЛЬНОЙ ФАЙЛОВОЙ СИСТЕМЫ
            } else if (my_strcmp(cmd, "cat") == 0) {
                if (!arg) { sys_puts(ep, "Usage: cat <filename>\n"); continue; }
                char *shm = (char*)0x502000;
                my_strcpy(shm, arg);
                ipc->msg[0] = 98; // SYS_READFILE
                seL4_Call(med_ep, seL4_MessageInfo_new(0, 0, 0, 1));
                
                seL4_Word size = seL4_GetMR(0);
                if (size == (seL4_Word)-1) {
                    sys_puts(ep, "File not found in CPIO archive.\n");
                } else {
                    sys_puts(ep, "--- FILE START ---\n");
                    // Печатаем первые 128 байт (чтобы не забить консоль бинарником)
                    for(seL4_Word j = 0; j < size && j < 128; j++) sys_putchar(ep, shm[j]);
                    sys_puts(ep, "\n--- EOF ---\n");
                }
            
            // НОВОЕ: ТЕСТ МАГИИ ПАМЯТИ (Demand Paging)
            } else if (my_strcmp(cmd, "malloc") == 0) {
                sys_puts(ep, "Accessing unmapped memory at 0x510000...\n");
                
                // Это должно вызвать VMFault! Но ядро нас спасет.
                volatile char *heap = (volatile char*)0x510000;
                heap[0] = 'H'; heap[1] = 'E'; heap[2] = 'L'; heap[3] = 'L'; heap[4] = 'O'; heap[5] = '\0';
                
                sys_puts(ep, "Demand Paging Success! Read back: ");
                sys_puts(ep, (const char*)heap);
                sys_puts(ep, "\n");

            // НОВОЕ: СТАНДАРТНЫЕ КОМАНДЫ LINUX
            } else if (my_strcmp(cmd, "ls") == 0) {
                char *shm = (char*)0x502000;
                ipc->msg[0] = 101; // SYS_LS
                seL4_Call(ep, seL4_MessageInfo_new(0, 0, 0, 1));
                sys_puts(ep, shm);
            } else if (my_strcmp(cmd, "uname") == 0) {
                if (arg && my_strcmp(arg, "-a") == 0) {
                    sys_puts(ep, "PsychWardOS 1.0.0 (seL4 true-microkernel) aarch64\n");
                } else {
                    sys_puts(ep, "PsychWardOS\n");
                }
            } else if (my_strcmp(cmd, "pwd") == 0) {
                sys_puts(ep, "/sandbox\n");
            } else if (my_strcmp(cmd, "whoami") == 0) {
                sys_puts(ep, "patient\n");
            } else if (my_strcmp(cmd, "ps") == 0) {
            char *shm = (char*)0x502000;
            ipc->msg[0] = 104; // SYS_PS
            seL4_Call(ep, seL4_MessageInfo_new(0, 0, 0, 1));
            sys_puts(ep, shm);
            } else if (my_strcmp(cmd, "help") == 0) {
            sys_puts(ep, "Available commands: time, sleep, doctor, exec, cat, ls, ps, kill, exit, uname, whoami, pwd, alloc, malloc, clear, echo, help\n");
            } else {
                sys_puts(ep, "Unknown command\n");
            }
        }
    }
}