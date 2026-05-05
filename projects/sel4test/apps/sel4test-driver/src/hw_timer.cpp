
#include "hw_timer.h"

volatile uint32_t *rtc_dr;
volatile uint32_t *rtc_mr;
volatile uint32_t *rtc_imsc;
volatile uint32_t *rtc_icr;

void timer_init(void *vaddr) {
    rtc_dr = (volatile uint32_t*)((char*)vaddr + 0x00);
    rtc_mr = (volatile uint32_t*)((char*)vaddr + 0x04);
    rtc_imsc = (volatile uint32_t*)((char*)vaddr + 0x10);
    
    // Правильный регистр очистки прерывания
    rtc_icr = (volatile uint32_t*)((char*)vaddr + 0x1C);

    // Мы больше НЕ трогаем 0x0C (rtc_cr), чтобы QEMU не падал!
    
    *rtc_imsc = 1; // Включаем генерацию аппаратных прерываний
}

uint64_t pl031_get_time() {
    return *rtc_dr; // Таймер PL031 тикает 1 раз в секунду
}

void pl031_set_match(uint32_t match_val) {
    *rtc_mr = match_val; // Устанавливаем будильник
}

void pl031_clear_interrupt() {
    *rtc_icr = 1; // Подтверждаем прерывание железу
}