#pragma once
#include "common.h"

class PsychAllocator {
public:
    seL4_CPtr next_slot;
    seL4_BootInfo *info;

    PsychAllocator(seL4_BootInfo *_info);
    seL4_CPtr alloc_slot();
    seL4_CPtr get_untyped_cap(size_t idx);
};

int find_untyped_by_paddr(seL4_BootInfo *info, uintptr_t phys_addr, int need_device, size_t min_size_bits, size_t *out_idx);