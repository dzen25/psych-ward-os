#include "h/allocator.h"
#include <stdint.h> // Добавлено для uintptr_t
#include <stddef.h> // Добавлено для size_t

PsychAllocator::PsychAllocator(seL4_BootInfo* info) {
    boot_info = info;
    current_slot = info->empty.start;
    max_slot = info->empty.end;
    free_count = 0;
}

seL4_CPtr PsychAllocator::alloc_slot() {
    // 1. Сначала отдаем освобожденные слоты
    if (free_count > 0) {
        return free_slots[--free_count];
    }
    // 2. Иначе берем новые из бамп-аллокатора
    if (current_slot < max_slot) {
        return current_slot++;
    }
    return 0; // OOM
}

void PsychAllocator::free(seL4_Word slot) {
    // Возвращаем слот в пул
    if (free_count < FREE_POOL_SIZE && slot != 0) {
        free_slots[free_count++] = slot;
    }
}

seL4_CPtr PsychAllocator::get_untyped_cap(int idx) {
    if (boot_info->untyped.start + idx < boot_info->untyped.end) {
        return boot_info->untyped.start + idx;
    }
    return 0;
}

// Глобальная функция поиска Untyped-памяти по физическому адресу
int find_untyped_by_paddr(seL4_BootInfo *info, uintptr_t phys_addr, int need_device, size_t min_size_bits, size_t *out_idx) {
    // Защита от нулевых указателей
    if (!info || !out_idx) return 0;

    for (size_t i = 0; i < (size_t)(info->untyped.end - info->untyped.start); i++) {
        seL4_UntypedDesc &desc = info->untypedList[i];
        
        if (desc.isDevice == need_device && desc.sizeBits >= min_size_bits) {
            uintptr_t start = desc.paddr;
            uintptr_t end = start + (1ULL << desc.sizeBits);
            
            // Вычисляем, где должен закончиться наш запрашиваемый блок памяти
            uintptr_t req_end = phys_addr + (1ULL << min_size_bits);
            
            // ИСПРАВЛЕНИЕ: Проверяем не только начало, но и то, что нужный 
            // нам объем памяти полностью помещается до конца региона (req_end <= end)
            if (phys_addr >= start && req_end <= end) {
                *out_idx = i;
                return 1;
            }
        }
    }
    return 0;
}