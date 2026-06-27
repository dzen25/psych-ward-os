#ifndef ALLOCATOR_H
#define ALLOCATOR_H

extern "C" {
#include <sel4/sel4.h>
#include <sel4platsupport/bootinfo.h>
#include <stdint.h>
#include <stddef.h>
}

class PsychAllocator {
private:
    seL4_Word current_slot;
    seL4_Word max_slot;
    seL4_BootInfo* boot_info; // Для get_untyped_cap

    // Пул свободных слотов (защита от Out of CSlots)
    static const int FREE_POOL_SIZE = 256; 
    seL4_Word free_slots[FREE_POOL_SIZE];
    int free_count;

public:
    // Оригинальный конструктор, который ожидает main.cpp
    PsychAllocator(seL4_BootInfo* info);
    
    // Оригинальные методы
    seL4_CPtr alloc_slot(); 
    seL4_CPtr get_untyped_cap(int idx);

    // Наш новый метод для возврата слотов
    void free(seL4_Word slot); 
};
int find_untyped_by_paddr(seL4_BootInfo *info, uintptr_t phys_addr, int need_device, size_t min_size_bits, size_t *out_idx);
#endif