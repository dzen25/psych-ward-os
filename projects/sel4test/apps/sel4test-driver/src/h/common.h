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
};

const char* sel4_err_str(seL4_Error err);
void check_err(seL4_Error err, const char *msg);