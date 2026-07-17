#include "h/hw_timer.h"
#include "h/platform.h"

// ARM generic timer. CNTVCT_EL0/CNTFRQ_EL0 читаются напрямую EL0-инструкцией
// mrs — на этой сборке ядра (gen_config: EXPORT_VCNT_USER=true) доступ
// разрешён без какого-либо трапа в ядро. Регистры сравнения/управления
// таймера (CNTP_*/CNTV_*) с EL0 НЕ читаются (EXPORT_PTMR_USER/VTMR_USER=false
// в этой сборке) — реального аппаратного будильника с userspace получить
// нельзя, поэтому "sleep" реализован поллингом (см. shell.cpp sys_sleep()),
// а не через IRQ, как было с PL031.
static uint64_t g_boot_tick = 0;
static uint64_t g_cntfrq = 0;

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

void timer_init() {
    g_cntfrq = read_cntfrq();
    g_boot_tick = read_cntvct();
}

uint64_t hw_timer_get_uptime_ms() {
    if (g_cntfrq == 0) return 0; // timer_init() ещё не вызывался
    uint64_t elapsed_ticks = read_cntvct() - g_boot_tick;
    return (elapsed_ticks * 1000) / g_cntfrq;
}
