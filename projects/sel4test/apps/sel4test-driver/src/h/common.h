#pragma once
#include <stdio.h>
#include <stdint.h>

extern "C" {
#include <sel4/sel4.h>
}

void __assert_fail(const char *assertion, const char *file, int line, const char *function);

enum BootIPCSlot {
    BOOT_CONSOLE_EP = 100,
    BOOT_TIMER_EP   = 101,
    BOOT_NET_EP     = 102,
    BOOT_ROOT_EP    = 103,
    BOOT_IRQ_EP     = 104,
    BOOT_WIFI_EP    = 105, // wifi_driver слушает команды шелла (см. ROADMAP.md Фаза 4)
};

// Слот CSpace процесса, в который ядро минтит capability активного пайпа
// (см. SYS_PIPE/SYS_PIPE_CLOSE в main.cpp и запрос пайпа в shell.cpp).
// Должен отличаться от зарезервированных local_* слотов в main.cpp::spawn_process
// (console=1, timer=2, net_send=3, irq=4, net_recv=5, blk=7, syscall=10) —
// раньше здесь был захардкожен слот 3, что уничтожало net_send_ep при закрытии пайпа.
constexpr seL4_Word PIPE_FD_SLOT = 20;

// Сисколл rootserver'у: "моя синхронная инициализация завершена, я готов
// обслуживать запросы". Каждый драйвер шлет его один раз перед входом в
// свой главный while(1); rootserver ждет этот сигнал между spawn_process()
// соседних драйверов (см. main.cpp), поэтому порядок готовности определяется
// самим порядком вызовов spawn_process(), а не отдельным списком где-то еще.
// Специально НЕ используется для "лучше-стараться" фоновых задач (например,
// первого NTP-синка в net_driver) — иначе загрузка могла бы зависнуть
// навсегда при недоступной сети.
constexpr seL4_Word SYS_DRIVER_READY = 109;

// Сисколл шелла: "заблокируй меня, пока не готовы ВСЕ драйверы (см.
// SYS_DRIVER_READY выше)". Шелл шлет его один раз при старте, до печати
// собственного баннера/приглашения — так его "sandbox[N] />" оказывается
// в логе строго после логов инициализации остальных модулей, без ручной
// синхронизации порядка где-либо еще.
constexpr seL4_Word SYS_WAIT_ALL_DRIVERS_READY = 110;

const char* sel4_err_str(seL4_Error err);
void check_err(seL4_Error err, const char *msg);