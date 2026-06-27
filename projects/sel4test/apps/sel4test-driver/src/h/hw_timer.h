#pragma once
#include "common.h"

void timer_init(void *vaddr);
uint64_t pl031_get_time();
void pl031_set_match(uint32_t match_val);
void pl031_clear_interrupt();