#pragma once
#include "common.h"

// ARM generic timer (CNTVCT_EL0/CNTFRQ_EL0) — читается напрямую из EL0,
// без MMIO/device-frame (см. hw_timer.cpp). Заменяет PL031 (Фаза 3.1,
// ROADMAP.md) — на реальной RPi4 battery-backed RTC нет вообще.
void timer_init();
uint64_t hw_timer_get_uptime_ms();
