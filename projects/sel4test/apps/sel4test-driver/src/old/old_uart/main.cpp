#include <stdio.h>
#include <stdint.h>
#include <string.h>

extern "C" {
#include <sel4/sel4.h>
#include <sel4platsupport/bootinfo.h>
}

extern "C" void __assert_fail(const char *assertion, const char *file, int line, const char *function) {
    printf("PANIC: %s at %s:%d\n", assertion, file, line);
    while (1);
}

const char* sel4_err_str(seL4_Error err) {
    switch (err) {
        case seL4_NoError:          return "NoError";
        case seL4_InvalidArgument:  return "InvalidArgument";
        case seL4_InvalidCapability:return "InvalidCapability";
        case seL4_IllegalOperation: return "IllegalOperation";
        case seL4_RangeError:       return "RangeError";
        case seL4_AlignmentError:   return "AlignmentError";
        case seL4_FailedLookup:     return "FailedLookup";
        case seL4_TruncatedMessage: return "TruncatedMessage";
        case seL4_DeleteFirst:      return "DeleteFirst";
        case seL4_RevokeFirst:      return "RevokeFirst";
        case seL4_NotEnoughMemory:  return "NotEnoughMemory";
        default: return "Unknown";
    }
}

void check_err(seL4_Error err, const char *msg) {
    if (err != seL4_NoError) {
        printf("FATAL: %s -> %s (code %d)\n", msg, sel4_err_str(err), err);
        while (1);
    }
}

class PsychAllocator {
public:
    seL4_CPtr next_slot;
    seL4_BootInfo *info;

    PsychAllocator(seL4_BootInfo *_info) : info(_info) {
        next_slot = info->empty.start;
    }

    seL4_CPtr alloc_slot() {
        if (next_slot >= info->empty.end) {
            printf("Out of CSlots\n");
            while (1);
        }
        return next_slot++;
    }

    seL4_CPtr get_untyped_cap(size_t idx) {
        return info->untyped.start + idx;
    }
};

// Поиск untyped (device или normal) по физическому адресу и типу
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

int main(int argc, char *argv[]) {
    seL4_BootInfo *info = platsupport_get_bootinfo();
    if (!info) {
        printf("No bootinfo\n");
        while (1);
    }

    printf("\n=== Psych Ward OS: UART Therapy (Fixed Mapping) ===\n");

    // --- 1. Найти device untyped для UART ---
    const uintptr_t uart_phys = 0x09000000;  // стандартный PL011 в QEMU virt
    size_t device_idx = 0;
    if (!find_untyped_by_paddr(info, uart_phys, 1, 12, &device_idx)) {
        printf("FATAL: No device untyped covering UART phys %p\n", (void*)uart_phys);
        while (1);
    }
    seL4_UntypedDesc &dev_desc = info->untypedList[device_idx];
    printf("Device untyped[%zu]: sizeBits=%u, paddr=%p\n",
           device_idx, dev_desc.sizeBits, (void*)dev_desc.paddr);

    // --- 2. Найти обычный (non-device) untyped для PMD и PT ---
    size_t normal_idx = 34; // из предыдущего вывода (paddr=0x40380000, sizeBits=19)
    if (normal_idx >= (info->untyped.end - info->untyped.start) || info->untypedList[normal_idx].isDevice != 0) {
        // запасной поиск с индекса 30
        for (size_t i = 30; i < info->untyped.end - info->untyped.start; i++) {
            if (!info->untypedList[i].isDevice && info->untypedList[i].sizeBits >= 12) {
                normal_idx = i;
                break;
            }
        }
    }
    if (normal_idx >= (info->untyped.end - info->untyped.start)) {
        printf("FATAL: No normal untyped found for page tables\n");
        while(1);
    }
    seL4_UntypedDesc &norm_desc = info->untypedList[normal_idx];
    printf("Normal untyped[%zu]: sizeBits=%u, paddr=%p\n",
           normal_idx, norm_desc.sizeBits, (void*)norm_desc.paddr);

    PsychAllocator alloc(info);
    seL4_CPtr root_cnode = seL4_CapInitThreadCNode;
    seL4_CPtr root_vspace = seL4_CapInitThreadVSpace;

    seL4_CPtr pmd   = alloc.alloc_slot();
    seL4_CPtr pt    = alloc.alloc_slot();
    seL4_CPtr uart_frame = alloc.alloc_slot();

    seL4_CPtr normal_untyped = alloc.get_untyped_cap(normal_idx);
    seL4_CPtr device_untyped = alloc.get_untyped_cap(device_idx);

    // Retype PMD и PT из обычной памяти
    check_err(seL4_Untyped_Retype(normal_untyped, seL4_ARM_PageDirectoryObject, 0,
                                  root_cnode, 0, 0, pmd, 1), "Retype PMD");
    check_err(seL4_Untyped_Retype(normal_untyped, seL4_ARM_PageTableObject, 0,
                                  root_cnode, 0, 0, pt, 1), "Retype PT");
    // Retype кадр UART из device-памяти
    check_err(seL4_Untyped_Retype(device_untyped, seL4_ARM_SmallPageObject, 0,
                                  root_cnode, 0, 0, uart_frame, 1), "Retype UART frame");

    // Виртуальный адрес – выравнен на 2 МБ, вне конфликтов с ядром
    uintptr_t uart_vaddr = 0x200000000ULL; // 8 ГБ

    // Отображаем PMD в корневую таблицу
    check_err(seL4_ARM_PageDirectory_Map(pmd, root_vspace, uart_vaddr,
                                         seL4_ARM_Default_VMAttributes), "Map PMD");
    // Отображаем PT в PMD
    check_err(seL4_ARM_PageTable_Map(pt, root_vspace, uart_vaddr,
                                     seL4_ARM_Default_VMAttributes), "Map PT");
    // Отображаем кадр UART в PT
    check_err(seL4_ARM_Page_Map(uart_frame, root_vspace, uart_vaddr, seL4_AllRights,
                                seL4_ARM_Default_VMAttributes), "Map UART frame");

    printf("UART mapped at virtual address %p\n", (void*)uart_vaddr);

    // --- Простой вывод на PL011 ---
    volatile uint32_t *uart_dr = (volatile uint32_t*)uart_vaddr;       // регистр данных
    volatile uint32_t *uart_fr = (volatile uint32_t*)(uart_vaddr + 0x18); // регистр флагов
    const char *msg = "Hello from Psych Ward OS via direct UART!\n";
    for (const char *p = msg; *p; p++) {
        // Ожидаем, пока TX FIFO не заполнен (бит 5 = TXFF)
        while ((*uart_fr) & (1 << 5));
        *uart_dr = *p;
    }

    printf("Message sent. UART therapy complete.\n");

    while (1) seL4_Yield();
    return 0;
}