#pragma once
#include <stdint.h>

// Начало драйвера GPIO общего назначения (BCM2711 ARM-side GPIO-контроллер,
// см. platform.h RPI4_GPIO_PADDR/RPI4_GPIO_SIZE + PLAT_GPIO_VADDR). Задача
// пользователя 2026-08-16: мигать зелёным ACT LED при обращении к SD-карте
// (чтение/запись/просмотр каталогов) — заведено пока только в blk_driver.cpp,
// но сам модуль не привязан к конкретному драйверу (любой процесс, которому
// root замаппит GPIO-фрейм, может его подключить).
//
// GPIO42 подтверждён по официальному DT (bcm2711-rpi-4-b.dts):
//   leds { led-act { gpios = <&gpio 42 GPIO_ACTIVE_HIGH>; ... }; };
// Именно &gpio (прямой ARM-side контроллер, 0x7e200000 -> 0xfe200000), а НЕ
// &expgpio (firmware-gpio через VideoCore mailbox, как PWR LED/BT_ON/WL_ON) —
// значит пином можно управлять напрямую через MMIO, без mailbox-тегов.
// Регистровая раскладка (GPFSELn/GPSET/GPCLR) не менялась с BCM2835 (Pi1),
// сверено с DT/datasheet напрямую, не переиспользует чужой код.

static volatile uint32_t* g_gpio_base = nullptr;

constexpr uint32_t GPIO_REG_GPFSEL4 = 0x10; // function select, пины 40-49, 3 бита/пин
constexpr uint32_t GPIO_REG_GPSET1  = 0x20; // set,   пины 32-57 (запись 1 включает, 0 — без эффекта)
constexpr uint32_t GPIO_REG_GPCLR1  = 0x2C; // clear, пины 32-57 (запись 1 выключает, 0 — без эффекта)

constexpr int GPIO_ACT_LED_PIN = 42; // зелёный ACT LED, GPIO_ACTIVE_HIGH (см. DT выше)

static inline volatile uint32_t* gpio_reg(uintptr_t offset) {
    return (volatile uint32_t*)((uintptr_t)g_gpio_base + offset);
}

// vaddr — уже смапленная страница RPI4_GPIO_PADDR (см. PLAT_GPIO_VADDR,
// platform.h + spawn_process(), main.cpp). Настраивает ACT LED как выход,
// остальные пины не трогает.
static inline void gpio_init(void* vaddr) {
    g_gpio_base = (volatile uint32_t*)vaddr;
    int field = GPIO_ACT_LED_PIN - 40; // 0..9 внутри GPFSEL4 (пины 40-49)
    int shift = field * 3;
    uint32_t val = *gpio_reg(GPIO_REG_GPFSEL4);
    val &= ~((uint32_t)0x7 << shift);
    val |= ((uint32_t)0x1 << shift); // 001 = output
    *gpio_reg(GPIO_REG_GPFSEL4) = val;
}

static inline void gpio_act_led_on() {
    if (!g_gpio_base) return; // gpio_init() не звался — фрейм не замаплен, писать некуда
    *gpio_reg(GPIO_REG_GPSET1) = (uint32_t)1 << (GPIO_ACT_LED_PIN - 32);
}

static inline void gpio_act_led_off() {
    if (!g_gpio_base) return;
    *gpio_reg(GPIO_REG_GPCLR1) = (uint32_t)1 << (GPIO_ACT_LED_PIN - 32);
}
