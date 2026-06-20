#include <sel4/sel4.h>
#include "common.h"

static inline seL4_IPCBuffer* get_local_ipc() {
    seL4_Word tls_addr;
    asm volatile("mrs %0, tpidr_el0" : "=r"(tls_addr));
    return (seL4_IPCBuffer*)(tls_addr - 1024);
}

void __assert_fail(const char *assertion, const char *file, int line, const char *function) { while(1); }

int main(void) {
    seL4_Word tls_addr;
    // 1. Безопасно читаем аппаратный регистр TLS (он указывает на +3072)
    asm volatile("mrs %0, tpidr_el0" : "=r"(tls_addr));
    
    // 2. Вычитаем 1024 байта, чтобы попасть на реальный seL4_IPCBuffer (+2048)
    seL4_IPCBuffer *ipc = (seL4_IPCBuffer*)(tls_addr - 1024);
    seL4_SetIPCBuffer(ipc);

    // 2. Теперь безопасно получаем root_ep
    seL4_CPtr root_ep = ipc->msg[BOOT_ROOT_EP];
    seL4_CPtr my_ep   = ipc->msg[BOOT_TIMER_EP];
    seL4_CPtr irq_ep  = ipc->msg[BOOT_IRQ_EP];

    volatile seL4_Uint32 *rtc_dr  = (volatile seL4_Uint32*)(0x200002000ULL + 0x00);
    volatile seL4_Uint32 *rtc_icr = (volatile seL4_Uint32*)(0x200002000ULL + 0x10);

    while(1) {
        seL4_Word badge = 0;
        seL4_MessageInfo_t info = seL4_Recv(my_ep, &badge);

        if (badge == 2) { 
            *rtc_icr = 1; 
            seL4_IRQHandler_Ack(irq_ep);
            continue;
        }

        seL4_Word sys = seL4_GetMR(0);
        if (sys == 3) { 
            seL4_SetMR(0, (*rtc_dr) * 1000);
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
        } else {
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 0));
        }
    }

    return 0;
}