#pragma once
#include <stdio.h>
#include <stdint.h>

extern "C" {
#include <sel4/sel4.h>
#include <sel4platsupport/bootinfo.h>
}

extern "C" void __assert_fail(const char *assertion, const char *file, int line, const char *function);

const char* sel4_err_str(seL4_Error err);
void check_err(seL4_Error err, const char *msg);