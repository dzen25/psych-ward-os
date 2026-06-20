#include <sel4/sel4.h>
#include "common.h"

static inline seL4_IPCBuffer* get_local_ipc() {
    seL4_Word tls_addr;
    // Добавлена буква 'ro'. crt0 не мог его стереть!
    asm volatile("mrs %0, tpidrro_el0" : "=r"(tls_addr)); 
    return (seL4_IPCBuffer*)(tls_addr - 1024);
}

void __assert_fail(const char *assertion, const char *file, int line, const char *function) { while(1); }

int main(int argc, char *argv[]) {
    // 2. Достаем настоящий адрес буфера
    seL4_IPCBuffer *ipc = get_local_ipc();
    
    // 3. Отдаем его libsel4 (теперь её TLS инициализирован, и она сохранит его куда надо)
    seL4_SetIPCBuffer(ipc);

    // 2. Теперь безопасно получаем root_ep
    seL4_CPtr root_ep = ipc->msg[BOOT_ROOT_EP];
    seL4_CPtr my_ep   = ipc->msg[BOOT_CONSOLE_EP];
    seL4_CPtr irq_ep  = ipc->msg[BOOT_IRQ_EP];

    if (my_ep == 0 || irq_ep == 0) {
        __assert_fail("Null Capability Detected in Driver Init!", __FILE__, __LINE__, __func__);
    }

    volatile seL4_Uint32 *uart_dr = (volatile seL4_Uint32*)(0x200000000ULL);
    volatile seL4_Uint32 *uart_fr = (volatile seL4_Uint32*)(0x200000000ULL + 0x18);
    char kbd_buffer[128]; int head = 0, tail = 0;

    while(1) {
        seL4_Word badge = 0;
        seL4_MessageInfo_t info = seL4_Recv(my_ep, &badge);

        if (badge == 1) {
            while (((*uart_fr) & (1 << 4)) == 0) {
                kbd_buffer[head] = *uart_dr; head = (head + 1) % 128;
            }
            seL4_IRQHandler_Ack(irq_ep); continue;
        }

        seL4_Word sys = seL4_GetMR(0);
        if (sys == 8) {
            int len = seL4_MessageInfo_get_length(info);
            for(int i = 1; i < len; i++) {
                char c = (char)seL4_GetMR(i);

                int timeout = 100000;
                while (((*uart_fr) & (1 << 5)) && timeout > 0) {
                    timeout--;
                }
                if (timeout > 0) {
                    *uart_dr = c;
                }
            }
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 0));
        } else if (sys == 6) {
            if (head != tail) { seL4_SetMR(0, kbd_buffer[tail]); tail = (tail + 1) % 128; }
            else { seL4_SetMR(0, (seL4_Word)-1); }
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
        } else {
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
        }
    }

    return 0;
}