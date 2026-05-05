#include "allocator.h"

PsychAllocator::PsychAllocator(seL4_BootInfo *_info) : info(_info) {
    next_slot = info->empty.start;
}

seL4_CPtr PsychAllocator::alloc_slot() {
    if (next_slot >= info->empty.end) {
        printf("Out of CSlots\n");
        while (1);
    }
    return next_slot++;
}

seL4_CPtr PsychAllocator::get_untyped_cap(size_t idx) {
    if (idx >= (info->untyped.end - info->untyped.start)) {
        printf("Untyped out of range\n");
        while (1);
    }
    return info->untyped.start + idx;
}

int find_untyped_by_paddr(seL4_BootInfo *info, uintptr_t phys_addr, int need_device, size_t min_size_bits, size_t *out_idx) {
    for (size_t i = 0; i < info->untyped.end - info->untyped.start; i++) {
        seL4_UntypedDesc &desc = info->untypedList[i];
        if (desc.isDevice == need_device && desc.sizeBits >= min_size_bits) {
            uintptr_t start = desc.paddr;
            uintptr_t end = start + (1ULL << desc.sizeBits);
            if (phys_addr >= start && phys_addr < end) {
                *out_idx = i;
                return 1;
            }
        }
    }
    return 0;
}