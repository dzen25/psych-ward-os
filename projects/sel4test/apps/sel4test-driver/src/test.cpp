#include <sel4/sel4.h>
#include "h/common.h"

// Эта функция вызывается макросом assert() при ошибке.
// Так как мы не линкуем стандартную библиотеку, нам нужно определить её самим.
void __assert_fail(const char *assertion, const char *file, int line, const char *function) {
    while(1);
}

static void sys_puts(seL4_CPtr console_ep, const char *str) {
    seL4_IPCBuffer *ipc = (seL4_IPCBuffer *)seL4_GetIPCBuffer();
    int total_len = 0;
    while(str[total_len]) total_len++;
    
    int offset = 0;
    while (offset < total_len) {
        int chunk = total_len - offset;
        // Строгий лимит IPC Message Registers для ARM64 (оставляем запас безопасности)
        if (chunk > 100) chunk = 100; 
        
        ipc->msg[0] = 8; // SYS_PUTS ID
        for (int i = 0; i < chunk; i++) {
            ipc->msg[i + 1] = str[offset + i];
        }
        seL4_Call(console_ep, seL4_MessageInfo_new(0, 0, 0, chunk + 1));
        offset += chunk;
    }
}

static inline seL4_IPCBuffer* get_local_ipc() {
    seL4_Word tls_addr;
    asm volatile("mrs %0, tpidrro_el0" : "=r"(tls_addr)); 
    return (seL4_IPCBuffer*)(tls_addr - 1024);
}

static void sys_exit(seL4_CPtr root_ep) {
    seL4_IPCBuffer *ipc = get_local_ipc();
    ipc->msg[0] = 103; // SYS_EXIT
    seL4_Call(root_ep, seL4_MessageInfo_new(0, 0, 0, 1));
    while(1) seL4_Yield(); // Сюда выполнение никогда не дойдет
}

// Теперь мы используем стандартный main!
int main(int argc, char *argv[]) {
    // 1. Получаем IPC-буфер и инициализируем libsel4, как и другие процессы
    seL4_IPCBuffer *ipc = get_local_ipc();
    seL4_SetIPCBuffer(ipc);

    // 2. Достаем Endpoint консоли из стартового сообщения от rootserver
    seL4_CPtr console_ep = ipc->msg[BOOT_CONSOLE_EP];
    seL4_CPtr root_ep = ipc->msg[BOOT_ROOT_EP];

    // 3. Печатаем баннер. С новым мультиплексором в uart_driver, гонки больше нет.
    const char* banner = 
        "\n======================================\n"
        "  SUCCESS: HELLO FROM FAT32 64 DISK!!!\n"
        "  (Standard main() execution)\n"
        "======================================\n\n";
    sys_puts(console_ep, banner);

    // 4. Завершаем процесс, чтобы вернуть управление оболочке
    sys_exit(root_ep);

    return 0;
}