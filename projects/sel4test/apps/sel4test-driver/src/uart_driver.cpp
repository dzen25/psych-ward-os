#include <sel4/sel4.h>

__attribute__((weak)) LIBSEL4_THREAD_LOCAL seL4_IPCBuffer *__sel4_ipc_buffer = nullptr;
void __assert_fail(const char *assertion, const char *file, int line, const char *function) { while(1); }

extern "C" void __sel4_start_c(void) {
    seL4_Word fake_tls_base = 0x501800; 
    asm volatile("msr tpidr_el0, %0" :: "r"(fake_tls_base));
    seL4_IPCBuffer *ipc = (seL4_IPCBuffer*)0x501000;
    __sel4_ipc_buffer = ipc;

    seL4_CPtr console_ep = ipc->caps_or_badges[0];
    seL4_CPtr irq_handler = ipc->caps_or_badges[1];

    volatile seL4_Uint32 *uart_dr = (volatile seL4_Uint32*)(0x200000000ULL);
    volatile seL4_Uint32 *uart_fr = (volatile seL4_Uint32*)(0x200000000ULL + 0x18);
    char kbd_buffer[128]; int head = 0, tail = 0;

    while(1) {
        seL4_Word badge = 0;
        seL4_MessageInfo_t info = seL4_Recv(console_ep, &badge);

        if (badge == 1) { 
            while (((*uart_fr) & (1 << 4)) == 0) { 
                kbd_buffer[head] = *uart_dr; head = (head + 1) % 128;
            }
            seL4_IRQHandler_Ack(irq_handler); continue; 
        }

        seL4_Word sys = seL4_GetMR(0);
        if (sys == 8) { 
            int len = seL4_MessageInfo_get_length(info);
            for(int i = 1; i < len; i++) {
                char c = (char)seL4_GetMR(i);
                while ((*uart_fr) & (1 << 5)); *uart_dr = c;
            }
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 0));
        } else if (sys == 6) { 
            if (head != tail) { seL4_SetMR(0, kbd_buffer[tail]); tail = (tail + 1) % 128; } 
            else { seL4_SetMR(0, (seL4_Word)-1); }
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
        } else { seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1)); }
    }
}