#include <sel4/sel4.h>
#include "h/common.h"

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

    // 5. Теперь безопасно получаем Capability-индексы
    seL4_CPtr root_ep = ipc->msg[BOOT_ROOT_EP];
    seL4_CPtr my_ep   = ipc->msg[BOOT_TIMER_EP];
    seL4_CPtr irq_ep  = ipc->msg[BOOT_IRQ_EP];

    if (my_ep == 0) {
        __assert_fail("FATAL: Null Capability #0 Detected!", __FILE__, __LINE__, __func__);
    }

    volatile seL4_Uint32 *rtc_dr  = (volatile seL4_Uint32*)(0x200002000ULL + 0x00);
    volatile seL4_Uint32 *rtc_icr = (volatile seL4_Uint32*)(0x200002000ULL + 0x10);

    // Момент запуска драйвера (секунды эпохи PL031) — точка отсчета аптайма.
    // В будущем при добавлении NTP-синхронизации сюда же ляжет коррекция смещения (offset)
    // между локальным PL031 и временем, полученным от NTP-сервера.
    const seL4_Uint32 boot_epoch_seconds = *rtc_dr;

    // Главный цикл обработки прерываний и IPC
    while(1) {
        seL4_Word badge = 0;
        seL4_MessageInfo_t info = seL4_Recv(my_ep, &badge);

        // Обработка прерывания таймера
        if (badge == 2) {
            *rtc_icr = 1;
            seL4_IRQHandler_Ack(irq_ep);
            continue;
        }

        // Обработка запросов от процессов (SYS_GET_TIME / SYS_GET_UPTIME)
        seL4_Word sys = seL4_GetMR(0);
        if (sys == 3) { // SYS_GET_TIME: мс с эпохи Unix (по локальным часам PL031)
            seL4_SetMR(0, (*rtc_dr) * 1000);
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
        } else if (sys == 4) { // SYS_GET_UPTIME: мс с момента запуска timer_driver
            seL4_SetMR(0, (*rtc_dr - boot_epoch_seconds) * 1000);
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
        } else {
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 0));
        }
    }

    return 0;
}