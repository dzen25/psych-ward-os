#include <sel4/sel4.h>

__attribute__((weak)) LIBSEL4_THREAD_LOCAL seL4_IPCBuffer *__sel4_ipc_buffer = nullptr;
void __assert_fail(const char *assertion, const char *file, int line, const char *function) { while(1); }

extern "C" void __sel4_start_c(void) {
    seL4_Word fake_tls_base = 0x501800; 
    asm volatile("msr tpidr_el0, %0" :: "r"(fake_tls_base));
    seL4_IPCBuffer *ipc = (seL4_IPCBuffer*)0x501000;
    __sel4_ipc_buffer = ipc;

    seL4_CPtr timer_ep = ipc->caps_or_badges[0];
    seL4_CPtr irq_handler = ipc->caps_or_badges[1];

    volatile seL4_Uint32 *rtc_dr  = (volatile seL4_Uint32*)(0x200002000ULL + 0x00);
    volatile seL4_Uint32 *rtc_icr = (volatile seL4_Uint32*)(0x200002000ULL + 0x10);

    while(1) {
        seL4_Word badge = 0;
        seL4_Recv(timer_ep, &badge); 

        if (badge == 2) { 
            *rtc_icr = 1; 
            seL4_IRQHandler_Ack(irq_handler);
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
}