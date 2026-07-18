#include <sel4/sel4.h>
#include "h/common.h"
#include "h/platform.h"

static inline seL4_IPCBuffer* get_local_ipc() {
    seL4_Word tls_addr;
    // Добавлена буква 'ro'. crt0 не мог его стереть!
    asm volatile("mrs %0, tpidrro_el0" : "=r"(tls_addr));
    return (seL4_IPCBuffer*)(tls_addr - 1024);
}

void __assert_fail(const char *assertion, const char *file, int line, const char *function) { while(1); }

// ARM generic timer (CNTVCT_EL0/CNTFRQ_EL0) — читается напрямую из EL0,
// без MMIO/device-frame (заменяет PL031, см. ROADMAP.md Фаза 3.1; подробности
// про EXPORT_*_USER этой сборки ядра — см. hw_timer.cpp). У процесса больше
// нет IRQ (BOOT_IRQ_EP не используется) — аппаратного будильника с EL0 не
// получить, поэтому "sleep" реализован клиентским поллингом SYS_GET_TIME
// (см. shell.cpp sys_sleep()), а не через прерывание, как было с PL031.
static inline uint64_t read_cntvct() {
    uint64_t val;
    asm volatile("mrs %0, cntvct_el0" : "=r"(val));
    return val;
}

static inline uint64_t read_cntfrq() {
    uint64_t val;
    asm volatile("mrs %0, cntfrq_el0" : "=r"(val));
    return val;
}

// Термодатчик AVS RO thermal (см. platform.h) — единственный MMIO-регистр,
// который этому процессу реально нужен, поэтому не заводим под него
// отдельный драйвер: та же экономия, что и с ARM generic timer выше —
// один регистр статуса, ни DMA, ни IRQ, ни инициализации.
static bool read_cpu_temp_mC(int32_t *out_mC) {
    volatile uint32_t *status = (volatile uint32_t*)(PLAT_AVS_VADDR + AVS_RO_TEMP_STATUS_OFFSET);
    uint32_t val = *status;
    if (!(val & AVS_RO_TEMP_STATUS_VALID_MSK)) return false;
    int32_t raw = (int32_t)(val & AVS_RO_TEMP_STATUS_DATA_MSK);
    *out_mC = AVS_TEMP_SLOPE_MC * raw + AVS_TEMP_OFFSET_MC;
    return true;
}

int main(int argc, char *argv[]) {
    // 2. Достаем настоящий адрес буфера
    seL4_IPCBuffer *ipc = get_local_ipc();

    // 3. Отдаем его libsel4 (теперь её TLS инициализирован, и она сохранит его куда надо)
    seL4_SetIPCBuffer(ipc);

    // 5. Теперь безопасно получаем Capability-индексы
    seL4_CPtr root_ep = ipc->msg[BOOT_ROOT_EP];
    seL4_CPtr my_ep   = ipc->msg[BOOT_TIMER_EP];

    if (my_ep == 0) {
        __assert_fail("FATAL: Null Capability #0 Detected!", __FILE__, __LINE__, __func__);
    }

    const uint64_t cntfrq = read_cntfrq();
    // Момент запуска драйвера — точка отсчета аптайма. Не корректируется
    // NTP-смещением: аптайм должен оставаться монотонным независимо от
    // коррекции показаний часов.
    const uint64_t boot_tick = read_cntvct();

    // Коррекция смещения (сек.) между аптаймом и NTP-сервером, применяется
    // только к SYS_GET_TIME. Выставляется командой шелла `ntp` через
    // net_driver (см. SYS_SET_TIME_OFFSET ниже).
    seL4_Int64 ntp_offset_seconds = 0;

    seL4_SetMR(0, SYS_DRIVER_READY);
    seL4_Call(root_ep, seL4_MessageInfo_new(0, 0, 0, 1));

    // Главный цикл обработки IPC-запросов
    while(1) {
        seL4_Word badge = 0;
        seL4_MessageInfo_t info = seL4_Recv(my_ep, &badge);

        uint64_t uptime_ms = ((read_cntvct() - boot_tick) * 1000) / cntfrq;

        // Обработка запросов от процессов (SYS_GET_TIME / SYS_GET_UPTIME)
        seL4_Word sys = seL4_GetMR(0);
        if (sys == 3) { // SYS_GET_TIME: мс "с эпохи" (аптайм + NTP-коррекция)
            seL4_Int64 corrected = (seL4_Int64)uptime_ms + ntp_offset_seconds * 1000;
            seL4_SetMR(0, (seL4_Word)corrected);
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
        } else if (sys == 4) { // SYS_GET_UPTIME: мс с момента запуска timer_driver
            seL4_SetMR(0, (seL4_Word)uptime_ms);
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
        } else if (sys == 5) { // SYS_SET_TIME_OFFSET: применить офсет от NTP-клиента (net_driver)
            ntp_offset_seconds = (seL4_Int64)seL4_GetMR(1);
            seL4_SetMR(0, 0);
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
        } else if (sys == 6) { // SYS_GET_TEMP: температура кристалла (см. AVS RO thermal выше)
            int32_t temp_mC = 0;
            bool valid = read_cpu_temp_mC(&temp_mC);
            seL4_SetMR(0, valid ? 0 : 1); // 0 = ok, 1 = датчик еще не выдал валидное показание
            seL4_SetMR(1, (seL4_Word)(int64_t)temp_mC);
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 2));
        } else {
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 0));
        }
    }

    return 0;
}
