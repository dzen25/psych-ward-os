#include "common.h"
#include "allocator.h"
#include "uart.h"
#include "hw_timer.h" 

#include <sel4/sel4.h>

extern "C" {
#include <cpio/cpio.h>
#include <elf/elf.h>
}
#include <string.h>

extern char _cpio_archive[];
extern char _cpio_archive_end[];

enum SyscallID {
    // --- БАЗОВЫЕ КЕРНЕЛ-ВЫЗОВЫ ---
    SYS_PRINT = 1, 
    SYS_YIELD = 2, 
    SYS_GET_TIME = 3, 
    SYS_SLEEP = 4, 
    SYS_PUTCHAR = 5, 
    SYS_READ = 6, 
    SYS_ALLOC = 7,
    SYS_PUTS = 8,
    
    // --- УПРАВЛЕНИЕ ПРОЦЕССАМИ И ПАМЯТЬЮ ---
    SYS_DOCTOR = 99, 
    SYS_EXEC = 100, 
    SYS_KILL = 102, 
    SYS_EXIT = 103, 
    SYS_PS = 104,
    SYS_WAIT = 106, 
    SYS_SHM_GET = 107,
    SYS_GETPID = 108,

    // --- ПРЕРЫВАНИЯ ЖЕЛЕЗА  ---
    UART_IRQ_BADGE = (1 << 0), 
    TIMER_IRQ_BADGE = (1 << 1)

};

static void uart_putdec(uint64_t val) {
    char buf[24];
    if (val == 0) { uart_puts("0"); return; }
    int i = 23; buf[i] = '\0';
    while (val > 0 && i > 0) { buf[--i] = (val % 10) + '0'; val /= 10; }
    uart_puts(&buf[i]);
}

static void print_human_time(uint64_t total_ms) {
    uint32_t ms = total_ms % 1000; uint32_t s = (total_ms / 1000) % 60;
    uint32_t m = (total_ms / 60000) % 60; uint32_t h = (total_ms / 3600000);
    uart_putdec(h); uart_puts("h "); uart_putdec(m); uart_puts("m ");
    uart_putdec(s); uart_puts("s "); uart_putdec(ms); uart_puts("ms");
}

static void pl011_putchar(char c) {
    volatile uint32_t *uart_dr = (volatile uint32_t*)(0x200000000ULL);
    volatile uint32_t *uart_fr = (volatile uint32_t*)(0x200000000ULL + 0x18);
    while ((*uart_fr) & (1 << 5)); *uart_dr = c;
}

#define KBD_BUFFER_SIZE 128
static char kbd_buffer[KBD_BUFFER_SIZE];
static int kbd_buffer_head = 0;
static int kbd_buffer_tail = 0;
static seL4_CPtr reader_reply_slot = 0; static bool reader_waiting = false; 
static seL4_CPtr sleeper_reply_slot = 0; static bool sleeper_waiting = false;

static uintptr_t untyped_watermarks[256] = {0};

// Исправленная версия: ищет подходящий untyped (включая device) и выделяет страницу
static seL4_CPtr alloc_device_frame(seL4_BootInfo *info, PsychAllocator &alloc, uintptr_t target_paddr, seL4_CPtr root_cnode) {
    size_t idx = (size_t)-1;
    size_t num_untyped = info->untyped.end - info->untyped.start;
    for (size_t i = 0; i < num_untyped; i++) {
        uintptr_t start = info->untypedList[i].paddr;
        size_t size = 1ULL << info->untypedList[i].sizeBits;
        if (target_paddr >= start && target_paddr < start + size) {
            idx = i;
            break;
        }
    }
    if (idx == (size_t)-1) while(1); // не нашли — фатально

    if (untyped_watermarks[idx] == 0)
        untyped_watermarks[idx] = info->untypedList[idx].paddr;

    seL4_CPtr untyped_cap = alloc.get_untyped_cap(idx);

    // Выравниваем waterMark до целевого адреса
    while (untyped_watermarks[idx] < target_paddr) {
        seL4_CPtr dummy = alloc.alloc_slot();
        seL4_Untyped_Retype(untyped_cap, seL4_ARM_SmallPageObject, 0, root_cnode, 0, 0, dummy, 1);
        untyped_watermarks[idx] += 4096;
    }

    seL4_CPtr frame = alloc.alloc_slot();
    seL4_Untyped_Retype(untyped_cap, seL4_ARM_SmallPageObject, 0, root_cnode, 0, 0, frame, 1);
    untyped_watermarks[idx] += 4096;
    return frame;
}

struct ProcessControlBlock {
    seL4_Word pid;
    char name[32];
    seL4_CPtr tcb;
    seL4_CPtr vspace;
    seL4_CPtr badged_ep;
    bool active;
    int waiting_for;      // <--- Какого PID мы ждем?
    seL4_CPtr reply_cap;  // <--- Куда отправить ответ, чтобы разбудить
};
static ProcessControlBlock pcbs[256];
static int next_pid = 1;

struct SharedMemoryRegion {
    bool active;
    seL4_CPtr frame_cap; // Физический фрейм памяти
};
static SharedMemoryRegion shm_regions[16];

static int spawn_process(const char* elf_name, seL4_CPtr ep, seL4_CPtr med_ep,
                         PsychAllocator &alloc, seL4_CPtr root_cnode, seL4_CPtr root_vspace,
                         seL4_CPtr normal_untyped, seL4_CPtr shm_frame_root,
                         int is_driver, seL4_CPtr console_ep, seL4_CPtr timer_ep, 
                         seL4_CPtr irq_ntfn, seL4_CPtr irq_handler, seL4_CPtr hw_frame,
                         const char *args_payload = nullptr,
                         seL4_CPtr net_cmd_recv_ep = 0,
                         seL4_CPtr net_cmd_send_ep = 0) {
    
    unsigned long elf_size = 0;
    unsigned long archive_len = _cpio_archive_end - _cpio_archive;
    char *elf_file = (char*)cpio_get_file(_cpio_archive, archive_len, elf_name, &elf_size);
    if (!elf_file) return -1;

    int pid = next_pid++;
    ProcessControlBlock& pcb = pcbs[pid];
    pcb.pid = pid;
    strncpy(pcb.name, elf_name, 31); pcb.name[31] = '\0';
    pcb.active = true;
    pcb.waiting_for = 0;  // <--- Инициализация
    pcb.reply_cap = 0;    // <--- Инициализация

    // Badged Endpoint для идентификации процесса Ядром
    seL4_CPtr badged_ep = alloc.alloc_slot();
    seL4_CNode_Mint(root_cnode, badged_ep, seL4_WordBits, root_cnode, ep, seL4_WordBits, seL4_AllRights, pid);
    pcb.badged_ep = badged_ep;

    elf_t elf;
    elf_newFile(elf_file, elf_size, &elf);
    uint64_t entry_point = elf_getEntryPoint(&elf);

    // VSpace (Виртуальная память песочницы)
    seL4_CPtr child_vspace = alloc.alloc_slot();
    seL4_CPtr child_pud = alloc.alloc_slot();
    seL4_CPtr child_pd  = alloc.alloc_slot();
    seL4_CPtr child_pt  = alloc.alloc_slot();

    seL4_Untyped_Retype(normal_untyped, seL4_ARM_PageGlobalDirectoryObject, 0, root_cnode, 0, 0, child_vspace, 1);
    seL4_Untyped_Retype(normal_untyped, seL4_ARM_PageUpperDirectoryObject, 0, root_cnode, 0, 0, child_pud, 1);
    seL4_Untyped_Retype(normal_untyped, seL4_ARM_PageDirectoryObject, 0, root_cnode, 0, 0, child_pd, 1);
    seL4_Untyped_Retype(normal_untyped, seL4_ARM_PageTableObject, 0, root_cnode, 0, 0, child_pt, 1);
    seL4_ARM_ASIDPool_Assign(seL4_CapInitThreadASIDPool, child_vspace);
    seL4_ARM_PageUpperDirectory_Map(child_pud, child_vspace, 0x400000, seL4_ARM_Default_VMAttributes);
    seL4_ARM_PageDirectory_Map(child_pd, child_vspace, 0x400000, seL4_ARM_Default_VMAttributes);
    seL4_ARM_PageTable_Map(child_pt, child_vspace, 0x400000, seL4_ARM_Default_VMAttributes);

    // Копируем ELF в память процесса
    uintptr_t temp_window = 0x200100000ULL;
    for (int i = 0; i < elf_getNumProgramHeaders(&elf); i++) {
        if (elf_getProgramHeaderType(&elf, i) == PT_LOAD) {
            uint64_t vaddr = elf_getProgramHeaderVaddr(&elf, i);
            uint64_t filesz = elf_getProgramHeaderFileSize(&elf, i);
            uint64_t memsz = elf_getProgramHeaderMemorySize(&elf, i);
            uint64_t offset = elf_getProgramHeaderOffset(&elf, i);
            uint64_t page_start = vaddr & ~0xFFFULL;
            uint64_t page_end = (vaddr + memsz + 0xFFF) & ~0xFFFULL;
            for (uint64_t page = page_start; page < page_end; page += 4096) {
                seL4_CPtr frame = alloc.alloc_slot();
                seL4_Untyped_Retype(normal_untyped, seL4_ARM_SmallPageObject, 0, root_cnode, 0, 0, frame, 1);
                seL4_ARM_Page_Map(frame, root_vspace, temp_window, seL4_AllRights, seL4_ARM_Default_VMAttributes);
                memset((void*)temp_window, 0, 4096);
                uint64_t copy_start = (page > vaddr) ? page : vaddr;
                uint64_t copy_end = (page + 4096 < vaddr + filesz) ? page + 4096 : vaddr + filesz;
                if (copy_start < copy_end) {
                    memcpy((void*)(temp_window + (copy_start - page)), elf_file + offset + (copy_start - vaddr), copy_end - copy_start);
                }
                seL4_ARM_Page_Unmap(frame);
                seL4_ARM_Page_Map(frame, child_vspace, page, seL4_AllRights, seL4_ARM_Default_VMAttributes);
            }
        }
    }

    uintptr_t child_stack = 0x500000;
    uintptr_t child_ipc   = 0x501000;
    seL4_CPtr stack_frame = alloc.alloc_slot();
    seL4_CPtr ipc_frame = alloc.alloc_slot();
    seL4_Untyped_Retype(normal_untyped, seL4_ARM_SmallPageObject, 0, root_cnode, 0, 0, stack_frame, 1);
    seL4_Untyped_Retype(normal_untyped, seL4_ARM_SmallPageObject, 0, root_cnode, 0, 0, ipc_frame, 1);

    check_err(seL4_ARM_Page_Map(ipc_frame, root_vspace, temp_window, seL4_AllRights, seL4_ARM_Default_VMAttributes), "Map IPC to Root");
    seL4_IPCBuffer *child_ipc_ptr = (seL4_IPCBuffer*)temp_window;
    memset(child_ipc_ptr, 0, 4096);
    // === STARTUP PAYLOAD ===
    // Копируем строку аргументов в массив msg IPC-буфера ребенка
    if (args_payload && args_payload[0] != '\0') {
        strcpy((char*)&child_ipc_ptr->msg[0], args_payload);
    }

    // ===================================================================
    // НАСТРОЙКА РОЛЕЙ ПРОЦЕССА В ЗАВИСИМОСТИ ОТ is_driver
    // ===================================================================
    if (is_driver == 1 || is_driver == 2 || is_driver == 3 || is_driver == 4) {
        // Процесс - Драйвер (UART, Timer или Block)
        seL4_CPtr drv_pud = alloc.alloc_slot();
        seL4_CPtr drv_pd  = alloc.alloc_slot();
        seL4_CPtr drv_pt  = alloc.alloc_slot();
        check_err(seL4_Untyped_Retype(normal_untyped, seL4_ARM_PageUpperDirectoryObject, 0, root_cnode, 0, 0, drv_pud, 1), "Retype Drv PUD");
        check_err(seL4_Untyped_Retype(normal_untyped, seL4_ARM_PageDirectoryObject, 0, root_cnode, 0, 0, drv_pd, 1), "Retype Drv PD");
        check_err(seL4_Untyped_Retype(normal_untyped, seL4_ARM_PageTableObject, 0, root_cnode, 0, 0, drv_pt, 1), "Retype Drv PT");
        
        uintptr_t hw_vaddr = (is_driver == 1) ? 0x200000000ULL : 
                             ((is_driver == 2) ? 0x200002000ULL : 0x200004000ULL);
        
        // Мапим структуры таблиц страниц (они покроют 2МБ диапазон, чего нам за глаза)
        seL4_ARM_PageUpperDirectory_Map(drv_pud, child_vspace, hw_vaddr, (seL4_ARM_VMAttributes)0);
        seL4_ARM_PageDirectory_Map(drv_pd, child_vspace, hw_vaddr, (seL4_ARM_VMAttributes)0);
        seL4_ARM_PageTable_Map(drv_pt, child_vspace, hw_vaddr, (seL4_ARM_VMAttributes)0);

        // --- НОВАЯ ЛОГИКА МАППИНГА ---
        // Если это драйвер диска (3), мапим 4 страницы (16КБ). Иначе — 1 страницу.
        int num_pages = (is_driver == 3 || is_driver == 4) ? 4 : 1;
        
        for (int i = 0; i < num_pages; i++) {
            seL4_CPtr frame_child = alloc.alloc_slot();
            // Копируем i-ю страницу из набора (они должны лежать в CNode подряд)
            check_err(seL4_CNode_Copy(root_cnode, frame_child, seL4_WordBits, 
                                      root_cnode, hw_frame + i, seL4_WordBits, seL4_AllRights), "Copy HW Frame Cap");
            
            // Мапим физическую страницу на виртуальный адрес со смещением
            check_err(seL4_ARM_Page_Map(frame_child, child_vspace, hw_vaddr + (i * 4096), 
                                        seL4_AllRights, (seL4_ARM_VMAttributes)0), "Map HW to Driver");
        }
        // -----------------------------
        
        child_ipc_ptr->caps_or_badges[0] = (is_driver == 1) ? console_ep : timer_ep;
        child_ipc_ptr->caps_or_badges[1] = irq_handler; 
        if (is_driver == 4) {
            child_ipc_ptr->caps_or_badges[2] = net_cmd_recv_ep;
        }
        child_ipc_ptr->userData = badged_ep;

    } else {
        
        // Процесс - Оболочка (Shell)
        child_ipc_ptr->userData = badged_ep;           // Команды Ядру (ls, ps)
        child_ipc_ptr->caps_or_badges[0] = console_ep; // Связь с UART Драйвером
        child_ipc_ptr->caps_or_badges[1] = timer_ep;   // Связь с Timer Драйвером
        child_ipc_ptr->caps_or_badges[2] = net_cmd_send_ep; // Команды Net Driver
    }
    // ===================================================================

    check_err(seL4_ARM_Page_Unmap(ipc_frame), "Unmap IPC from Root");
    check_err(seL4_ARM_Page_Map(stack_frame, child_vspace, child_stack, seL4_AllRights, seL4_ARM_Default_VMAttributes), "Map Stack to Child");
    check_err(seL4_ARM_Page_Map(ipc_frame, child_vspace, child_ipc, seL4_AllRights, seL4_ARM_Default_VMAttributes), "Map IPC to Child");

    // Выдаем Shared Memory VFS (Сразу 4 страницы = 16 КБ!)
    uintptr_t child_shm = 0x502000;
    for (int i = 0; i < 4; i++) {
        seL4_CPtr shm_frame_child = alloc.alloc_slot();
        seL4_CNode_Copy(root_cnode, shm_frame_child, seL4_WordBits, root_cnode, shm_frame_root + i, seL4_WordBits, seL4_AllRights);
        seL4_ARM_Page_Map(shm_frame_child, child_vspace, child_shm + (i * 4096), seL4_AllRights, seL4_ARM_Default_VMAttributes);
    }

    seL4_CPtr tcb = alloc.alloc_slot();
    pcb.tcb = tcb;
    pcb.vspace = child_vspace;

    seL4_Untyped_Retype(normal_untyped, seL4_TCBObject, 0, root_cnode, 0, 0, tcb, 1);
    seL4_TCB_Configure(tcb, badged_ep, root_cnode, seL4_NilData, child_vspace, seL4_NilData, child_ipc, ipc_frame);
    seL4_UserContext regs = {0};
    regs.pc = entry_point;
    regs.sp = child_stack + 4096;
    regs.x0 = (seL4_Word)badged_ep;
    regs.x1 = (seL4_Word)child_ipc;
    regs.x2 = (seL4_Word)med_ep;
    regs.tpidr_el0 = (seL4_Word)child_ipc;
    regs.tpidrro_el0 = (seL4_Word)child_ipc;
    size_t reg_count = sizeof(seL4_UserContext) / sizeof(seL4_Word);
    seL4_TCB_WriteRegisters(tcb, 0, 0, reg_count, &regs);
    seL4_TCB_SetPriority(tcb, seL4_CapInitThreadTCB, 254);

    // Привязываем прерывание ТОЛЬКО драйверам (Оболочка работает без IRQ)
    if (is_driver == 1 || is_driver == 2) {
        check_err(seL4_TCB_BindNotification(tcb, irq_ntfn), "Bind IRQ to Driver");
    }

    seL4_TCB_Resume(tcb);
    return pid;
}

int main(int argc, char *argv[]) {
    seL4_BootInfo *info = platsupport_get_bootinfo();
    if (!info) while (1);

    // Ищем самый большой RAM‑untyped
    size_t normal_idx = 0;
    uint8_t max_size_bits = 0;
    size_t num_untyped = info->untyped.end - info->untyped.start;
    for (size_t i = 0; i < num_untyped; i++) {
        if (!info->untypedList[i].isDevice && info->untypedList[i].sizeBits > max_size_bits) {
            max_size_bits = info->untypedList[i].sizeBits;
            normal_idx = i;
        }
    }
    if (max_size_bits == 0) while (1); // нет RAM – фатально

    PsychAllocator alloc(info);
    seL4_CPtr root_cnode = seL4_CapInitThreadCNode;
    seL4_CPtr root_vspace = seL4_CapInitThreadVSpace;
    seL4_CPtr normal_untyped = alloc.get_untyped_cap(normal_idx);

    memset(pcbs, 0, sizeof(pcbs));
    next_pid = 1;
    memset(shm_regions, 0, sizeof(shm_regions));

    

    // Резервируем слоты для всех объектов
    reader_reply_slot = alloc.alloc_slot();
    sleeper_reply_slot = alloc.alloc_slot();

    seL4_CPtr pmd = alloc.alloc_slot();
    seL4_CPtr pt = alloc.alloc_slot();
    seL4_Untyped_Retype(normal_untyped, seL4_ARM_PageDirectoryObject, 0, root_cnode, 0, 0, pmd, 1);
    seL4_Untyped_Retype(normal_untyped, seL4_ARM_PageTableObject, 0, root_cnode, 0, 0, pt, 1);

    // Фреймы устройств
    seL4_CPtr uart_frame = alloc_device_frame(info, alloc, 0x09000000, root_cnode);
    seL4_CPtr rtc_frame  = alloc_device_frame(info, alloc, 0x09010000, root_cnode);
    seL4_CPtr virtio_frames[4];
    for (int i = 0; i < 4; i++) {
        virtio_frames[i] = alloc_device_frame(info, alloc, 0x0a000000 + (i * 4096), root_cnode);
    }

    uintptr_t uart_vaddr = 0x200000000ULL;
    seL4_ARM_PageDirectory_Map(pmd, root_vspace, uart_vaddr, seL4_ARM_Default_VMAttributes);
    seL4_ARM_PageTable_Map(pt, root_vspace, uart_vaddr, seL4_ARM_Default_VMAttributes);
    seL4_ARM_Page_Map(uart_frame, root_vspace, uart_vaddr, seL4_AllRights, seL4_ARM_Default_VMAttributes);

    uintptr_t rtc_vaddr = 0x200002000ULL;
    seL4_ARM_Page_Map(rtc_frame, root_vspace, rtc_vaddr, seL4_AllRights, seL4_ARM_Default_VMAttributes);

    // Инициализация UART и таймера – после этого можно пользоваться uart_puts
    uart_init((void*)uart_vaddr);
    timer_init((void*)rtc_vaddr);

    // Теперь можно выводить сообщения
    uart_puts("\n=== Psych Ward OS: TRUE MICROKERNEL EDITION ===\n");

    // Endpoint'ы
    seL4_CPtr ep = alloc.alloc_slot();
    seL4_CPtr med_ep = alloc.alloc_slot();
    seL4_Untyped_Retype(normal_untyped, seL4_EndpointObject, 0, root_cnode, 0, 0, ep, 1);
    seL4_Untyped_Retype(normal_untyped, seL4_EndpointObject, 0, root_cnode, 0, 0, med_ep, 1);

    

    // 1. Shared memory (Оставляем ROOT себе, раздаем копии детям)
    // Аллоцируем 4 страницы подряд, чтобы они лежали в CNode одна за другой
    seL4_CPtr shm_frame_root = alloc_device_frame(info, alloc, 0x60000000, root_cnode);
    alloc_device_frame(info, alloc, 0x60001000, root_cnode);
    alloc_device_frame(info, alloc, 0x60002000, root_cnode);
    alloc_device_frame(info, alloc, 0x60003000, root_cnode);
    
    uintptr_t root_shm  = 0x200006000ULL;
    for (int i = 0; i < 4; i++) {
        seL4_ARM_Page_Map(shm_frame_root + i, root_vspace, root_shm + (i * 4096), seL4_AllRights, seL4_ARM_Default_VMAttributes);
    }
    
    // 2. Каналы связи (Endpoints) для драйверов
    seL4_CPtr console_ep = alloc.alloc_slot();
    seL4_CPtr timer_ep = alloc.alloc_slot();
    seL4_CPtr net_cmd_ep = alloc.alloc_slot();
    seL4_CPtr net_cmd_recv_ep = alloc.alloc_slot();
    seL4_CPtr net_cmd_send_ep = alloc.alloc_slot();
    seL4_Untyped_Retype(normal_untyped, seL4_EndpointObject, 0, root_cnode, 0, 0, console_ep, 1);
    seL4_Untyped_Retype(normal_untyped, seL4_EndpointObject, 0, root_cnode, 0, 0, timer_ep, 1);
    seL4_Untyped_Retype(normal_untyped, seL4_EndpointObject, 0, root_cnode, 0, 0, net_cmd_ep, 1);
    seL4_CNode_Copy(root_cnode, net_cmd_recv_ep, seL4_WordBits,
                    root_cnode, net_cmd_ep, seL4_WordBits, seL4_CanRead);
    seL4_CNode_Mint(root_cnode, net_cmd_send_ep, seL4_WordBits,
                    root_cnode, net_cmd_ep, seL4_WordBits, seL4_CanWrite, 5);

    // 3. Прерывания ДЛЯ ДРАЙВЕРА ТАЙМЕРА (IRQ 34)
    seL4_CPtr timer_ntfn = alloc.alloc_slot();
    seL4_CPtr badged_timer_ntfn = alloc.alloc_slot();
    seL4_CPtr timer_irq_handler = alloc.alloc_slot();
    seL4_Untyped_Retype(normal_untyped, seL4_NotificationObject, 0, root_cnode, 0, 0, timer_ntfn, 1);
    seL4_CNode_Mint(root_cnode, badged_timer_ntfn, seL4_WordBits, root_cnode, timer_ntfn, seL4_WordBits, seL4_AllRights, 2); 
    seL4_IRQControl_Get(seL4_CapIRQControl, 34, root_cnode, timer_irq_handler, seL4_WordBits);
    seL4_IRQHandler_SetNotification(timer_irq_handler, badged_timer_ntfn);
    seL4_IRQHandler_Ack(timer_irq_handler);

    // 4. Прерывания ДЛЯ ДРАЙВЕРА UART (IRQ 33)
    seL4_CPtr uart_ntfn = alloc.alloc_slot();
    seL4_CPtr badged_uart_ntfn = alloc.alloc_slot(); 
    seL4_CPtr uart_irq_handler = alloc.alloc_slot();
    seL4_Untyped_Retype(normal_untyped, seL4_NotificationObject, 0, root_cnode, 0, 0, uart_ntfn, 1);
    seL4_CNode_Mint(root_cnode, badged_uart_ntfn, seL4_WordBits, root_cnode, uart_ntfn, seL4_WordBits, seL4_AllRights, 1); 
    seL4_IRQControl_Get(seL4_CapIRQControl, 33, root_cnode, uart_irq_handler, seL4_WordBits);
    seL4_IRQHandler_SetNotification(uart_irq_handler, badged_uart_ntfn); 
    uart_enable_interrupts();
    seL4_IRQHandler_Ack(uart_irq_handler);

    // Запускаем Драйвер UART (is_driver = 1)
    if (spawn_process("uart_driver", ep, med_ep, alloc, root_cnode, root_vspace, normal_untyped, shm_frame_root, 
                      1, console_ep, timer_ep, uart_ntfn, uart_irq_handler, uart_frame) < 0) {
        uart_puts("PANIC: UART Driver failed to load!\n"); while(1);
    }

    // Запускаем Драйвер Таймера (is_driver = 2)
    if (spawn_process("timer_driver", ep, med_ep, alloc, root_cnode, root_vspace, normal_untyped, shm_frame_root, 
                      2, console_ep, timer_ep, timer_ntfn, timer_irq_handler, rtc_frame) < 0) {
        uart_puts("PANIC: Timer Driver failed to load!\n"); while(1);
    }


    // Запускаем Драйвер Диска и ФС (is_driver = 3)
    if (spawn_process("blk_driver", ep, med_ep, alloc, root_cnode, root_vspace, normal_untyped, shm_frame_root, 
                      3, console_ep, timer_ep, 0, 0, virtio_frames[0]) < 0) {
        uart_puts("PANIC: Block Driver failed to load!\n"); while(1);
    }

    // Запускаем Драйвер Сети (is_driver = 4)
    // Передаем ему virtio_frames[0], так как сеть и диск сидят на одной физической шине
    if (spawn_process("net_driver", ep, med_ep, alloc, root_cnode, root_vspace, normal_untyped, shm_frame_root, 
                      4, console_ep, timer_ep, 0, 0, virtio_frames[0], nullptr,
                      net_cmd_recv_ep, 0) < 0) {
        uart_puts("PANIC: Net Driver failed to load!\n"); while(1);
    }

    // Запускаем Оболочку (is_driver = 0)
    if (spawn_process("shell", ep, med_ep, alloc, root_cnode, root_vspace, normal_untyped, shm_frame_root, 
                      0, console_ep, timer_ep, 0, 0, 0, nullptr,
                      0, net_cmd_send_ep) < 0) {
        uart_puts("PANIC: Shell failed to load!\n"); while(1);
    }

    uart_puts("Quadruple Sandboxes Spawned! Kernel is purified and serving IPC...\n");

    // --- ЕДИНЫЙ ЦИКЛ ЯДРА ---
    while (1) {
        seL4_Word sender_badge = 0;
        seL4_MessageInfo_t recv_info = seL4_Recv(ep, &sender_badge);
        seL4_Word sender_pid = 0;

        if (sender_badge != 0 && sender_badge < 256 && pcbs[sender_badge].active) {
            sender_pid = sender_badge;
        }

        seL4_Word label = seL4_MessageInfo_get_label(recv_info);
        
        if (label == seL4_Fault_VMFault) {
            seL4_Word pc = seL4_GetMR(0);
            seL4_Word addr = seL4_GetMR(1);
            
            if (addr >= 0x510000 && addr < 0x600000 && sender_pid != 0) {
                uart_puts("\n[KERNEL PAGER] Page Fault at 0x"); uart_puthex(addr);
                uart_puts(" for PID "); uart_putdec(sender_pid);
                uart_puts(" -> Allocating Frame On-The-Fly!\n");
                
                seL4_CPtr frame = alloc.alloc_slot();
                check_err(seL4_Untyped_Retype(normal_untyped, seL4_ARM_SmallPageObject, 0, root_cnode, 0, 0, frame, 1), "Pager Frame");
                
                uintptr_t page_aligned = addr & ~0xFFFULL;
                check_err(seL4_ARM_Page_Map(frame, pcbs[sender_pid].vspace, page_aligned, seL4_AllRights, seL4_ARM_Default_VMAttributes), "Pager Map");
                
                // Перезапускаем упавшую инструкцию (Пациент даже не узнает, что падал)
                seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 0));
                continue;
            } else {
                uart_puts("\nFATAL FAULT! PID: "); uart_putdec(sender_pid);
                uart_puts("\nPC: "); uart_puthex(pc);
                uart_puts("\nMem Addr: "); uart_puthex(addr);
                uart_puts("\n");
                if (sender_pid != 0) {
                    seL4_TCB_Suspend(pcbs[sender_pid].tcb);
                    pcbs[sender_pid].active = false;
                    uart_puts("Process terminated.\n");
                }
                continue; 
            }
        }

        seL4_Word syscall_num = seL4_GetMR(0); 
        seL4_Word arg1 = seL4_GetMR(1);        

        switch (syscall_num) {
            case SYS_PRINT:
                uart_puts("Sandbox Time: [ "); print_human_time(arg1); uart_puts(" ]\n");
                seL4_SetMR(0, 0); seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                break;

            case SYS_GET_TIME: {
                uint64_t ms = pl031_get_time() * 1000; 
                seL4_SetMR(0, (seL4_Word)ms); seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                break;
            }

            case SYS_SLEEP:
                if (sleeper_waiting) {
                    seL4_SetMR(0, (seL4_Word)-1); seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                } else {
                    seL4_Error err = seL4_CNode_SaveCaller(root_cnode, sleeper_reply_slot, seL4_WordBits);
                    if (err == seL4_NoError) {
                        sleeper_waiting = true;
                        uint32_t seconds = arg1 / 1000;
                        if (seconds == 0) seconds = 1; 
                        pl031_set_match(pl031_get_time() + seconds); 
                    } else {
                        seL4_SetMR(0, (seL4_Word)-1); seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                    }
                }
                break;
                
            case SYS_PUTCHAR:
                pl011_putchar((char)arg1);
                seL4_SetMR(0, 0); seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                break;

            case SYS_PUTS: {
                int msg_len = seL4_MessageInfo_get_length(recv_info);
                for (int i = 1; i < msg_len; i++) {
                    pl011_putchar((char)seL4_GetMR(i));
                }
                seL4_SetMR(0, 0); 
                seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                break;
            }

            case SYS_READ:
                if (kbd_buffer_head != kbd_buffer_tail) {
                    char c = kbd_buffer[kbd_buffer_tail];
                    kbd_buffer_tail = (kbd_buffer_tail + 1) % KBD_BUFFER_SIZE;
                    seL4_SetMR(0, c); seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                } else {
                    if (reader_waiting) {
                        seL4_SetMR(0, (seL4_Word)-1); seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                    } else {
                        seL4_Error err = seL4_CNode_SaveCaller(root_cnode, reader_reply_slot, seL4_WordBits);
                        if (err == seL4_NoError) { reader_waiting = true; } 
                        else { seL4_SetMR(0, (seL4_Word)-1); seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1)); }
                    }
                }
                break;

            case SYS_ALLOC:
                seL4_SetMR(0, 0); seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                break;
            
            case SYS_WAIT: {
                int target_pid = arg1;
                // Проверяем, жив ли еще процесс, которого мы хотим ждать
                if (target_pid > 0 && target_pid < 256 && pcbs[target_pid].active) {
                    pcbs[sender_pid].waiting_for = target_pid;
                    
                    // Сохраняем "канал возврата" к заснувшему процессу
                    seL4_CPtr wait_reply = alloc.alloc_slot();
                    seL4_CNode_SaveCaller(root_cnode, wait_reply, seL4_WordBits);
                    pcbs[sender_pid].reply_cap = wait_reply;
                    
                    // ВАЖНО: Мы делаем continue, а не break. 
                    // Ядро НЕ отправляет seL4_Reply сейчас, процесс "повисает" в спячке!
                    continue; 
                } else {
                    seL4_SetMR(0, 0); // Процесс уже умер, сразу возвращаем успех
                    seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                }
                break;
            }

            case SYS_DOCTOR: {
                char *shm = (char*)0x200006000ULL; 
                uart_puts("\n[DOCTOR] Patient wrote in SHM: \"");
                uart_puts(shm);
                uart_puts("\"\n");
                
                const char* reply = "Take 2 bytes of C++ and call me in the morning.";
                int i = 0;
                while(reply[i]) { shm[i] = reply[i]; i++; }
                shm[i] = '\0';

                seL4_SetMR(0, 0); 
                seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                break;
            }

            case SYS_EXEC: {
                char *shm = (char*)0x200006000ULL;
                
                // 1. Извлекаем имя ELF-файла (до первого пробела)
                char *elf_name = shm;
                char *args = shm; 
                while (*args && *args != ' ') args++;
                
                if (*args == ' ') { 
                    *args = '\0'; // Отрезаем имя программы
                    args++;       // Указатель на начало аргументов
                } else {
                    args = nullptr; // Аргументов нет
                }
                
                // 2. Передаем args в spawn_process как последний параметр
                int new_pid = spawn_process(elf_name, ep, med_ep, alloc, root_cnode, root_vspace, normal_untyped, shm_frame_root,
                                            0, console_ep, timer_ep, 0, 0, 0, args);
                
                if (new_pid > 0) {
                    seL4_SetMR(0, (seL4_Word)new_pid);
                } else {
                    uart_puts("[KERNEL] Program not found in VFS.\n");
                    seL4_SetMR(0, (seL4_Word)-1);
                }
                seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                break;
            }

            case SYS_PS: {
                char *shm = (char*)0x200006000ULL;
                int offset = 0;
                
                strcpy(shm, "  PID STATUS    NAME\n");
                offset = strlen(shm);
                
                // Дадим Rootserver'у системный PID 0
                strcpy(shm + offset, "    0 [RUNNING] rootserver\n");
                offset = strlen(shm);
                
                // Начинаем цикл честно с 1 (где живет uart_driver)
                for (int i = 1; i < 256; i++) {
                    if (pcbs[i].active) {
                        char pid_str[8];
                        int temp = i, j = 0;
                        while(temp > 0) { pid_str[j++] = (temp % 10) + '0'; temp /= 10; }
                        
                        strcpy(shm + offset, "    "); offset += 4;
                        while(j > 0) { shm[offset++] = pid_str[--j]; }
                        strcpy(shm + offset, " [RUNNING] "); offset += 11;
                        
                        strcpy(shm + offset, pcbs[i].name); offset = strlen(shm);
                        strcpy(shm + offset, "\n"); offset++;
                    }
                }
                seL4_SetMR(0, 0);
                seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                break;
            }

            case SYS_KILL: {
                int target_pid = arg1;
                
                if (target_pid == 1) {
                    uart_puts("\n[KERNEL PANIC] Attempted to kill Rootserver (PID 1)!\n");
                    seL4_SetMR(0, (seL4_Word)-1);
                } 
                else if (target_pid > 1 && target_pid < 256 && pcbs[target_pid].active) {
                    uart_puts("\n[KERNEL] Terminating PID: "); uart_putdec(target_pid); uart_puts("\n");
                    
                    // ==========================================================
                    // МАГИЯ ПРОБУЖДЕНИЯ (Точно так же, как в SYS_EXIT)
                    // ==========================================================
                    // Ищем всех, кто ждал смерти target_pid (например, родительскую оболочку)
                    for (int i = 1; i < 256; i++) {
                        if (pcbs[i].active && pcbs[i].waiting_for == target_pid) { 
                            pcbs[i].waiting_for = 0; // Сбрасываем ожидание
                            seL4_SetMR(0, 0);        // Готовим код успеха для заснувшего процесса
                            // Отправляем IPC спящему процессу, чтобы он проснулся!
                            seL4_Send(pcbs[i].reply_cap, seL4_MessageInfo_new(0, 0, 0, 1));
                            // Удаляем временный канал связи
                            seL4_CNode_Delete(root_cnode, pcbs[i].reply_cap, seL4_WordBits);
                        }
                    }
                    // ==========================================================
                    
                    // 1. Физически замораживаем поток
                    seL4_TCB_Suspend(pcbs[target_pid].tcb);
                    
                    // 2. Отвязываем прерывания (если это драйвер)
                    seL4_TCB_UnbindNotification(pcbs[target_pid].tcb);
                    
                    // 3. Помечаем слот как свободный
                    pcbs[target_pid].active = false;

                    // 4. Очищаем память, чтобы убитые процессы не текли (как в SYS_EXIT)
                    seL4_CNode_Delete(root_cnode, pcbs[target_pid].badged_ep, seL4_WordBits);
                    seL4_CNode_Delete(root_cnode, pcbs[target_pid].tcb, seL4_WordBits);
                    if (pcbs[target_pid].vspace != root_vspace) {
                        seL4_CNode_Delete(root_cnode, pcbs[target_pid].vspace, seL4_WordBits);
                    }
                    
                    seL4_SetMR(0, 0); // Успех для того, кто вызвал команду kill
                } else {
                    seL4_SetMR(0, (seL4_Word)-1); // Процесс не найден
                }
                
                // Отвечаем тому процессу, который вызвал команду kill (если это не сам убитый)
                seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                break;
            }

            case SYS_EXIT: {
                if (sender_pid > 0) {
                    // Разбудить всех, кто ждал смерти этого процесса!
                    for (int i = 1; i < 256; i++) {
                        if (pcbs[i].active && pcbs[i].waiting_for == sender_pid) {
                            pcbs[i].waiting_for = 0;
                            seL4_SetMR(0, 0);
                            seL4_Send(pcbs[i].reply_cap, seL4_MessageInfo_new(0, 0, 0, 1));
                            seL4_CNode_Delete(root_cnode, pcbs[i].reply_cap, seL4_WordBits);
                        }
                    }
                    seL4_TCB_Suspend(pcbs[sender_pid].tcb);
                    seL4_TCB_UnbindNotification(pcbs[sender_pid].tcb);
                    pcbs[sender_pid].active = false;
                    seL4_CNode_Delete(root_cnode, pcbs[sender_pid].badged_ep, seL4_WordBits);
                    seL4_CNode_Delete(root_cnode, pcbs[sender_pid].tcb, seL4_WordBits);
                    if (pcbs[sender_pid].vspace != root_vspace) {
                        seL4_CNode_Delete(root_cnode, pcbs[sender_pid].vspace, seL4_WordBits);
                    }
                }
                
                // ИСПРАВЛЕНИЕ: Пропускаем seL4_Reply внизу цикла!
                // Процесс мертв, отвечать ему нельзя.
                continue; 
            }

            case SYS_GETPID: {
                seL4_SetMR(0, sender_pid); // Возвращаем PID вызывающего процесса
                seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                break;
            }

            case SYS_SHM_GET: {
                int shm_id = arg1;                  // ID региона (например, от 0 до 15)
                seL4_Word vaddr = seL4_GetMR(2);    // Виртуальный адрес в пространстве процесса

                if (shm_id < 0 || shm_id >= 16) {
                    seL4_SetMR(0, (seL4_Word)-1); // Ошибка: неверный ID
                    seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                    break;
                }

                // 1. Если этот фрейм еще не был создан — аллоцируем физическую память
                if (!shm_regions[shm_id].active) {
                    seL4_CPtr frame = alloc.alloc_slot();
                    check_err(seL4_Untyped_Retype(normal_untyped, seL4_ARM_SmallPageObject, 0, 
                                                  root_cnode, 0, 0, frame, 1), "Alloc SHM frame");
                    shm_regions[shm_id].frame_cap = frame;
                    shm_regions[shm_id].active = true;
                    uart_puts("[KERNEL] Created new SHM region ID: "); uart_putdec(shm_id); uart_puts("\n");
                }

                // 2. Копируем капу фрейма для конкретного ребенка
                seL4_CPtr child_frame_cap = alloc.alloc_slot();
                check_err(seL4_CNode_Copy(root_cnode, child_frame_cap, seL4_WordBits, 
                                          root_cnode, shm_regions[shm_id].frame_cap, seL4_WordBits, seL4_AllRights), "Copy SHM cap");

                // 3. Маппим скопированный фрейм в виртуальное пространство процесса-отправителя
                seL4_Error map_err = seL4_ARM_Page_Map(child_frame_cap, pcbs[sender_pid].vspace, vaddr, 
                                                       seL4_AllRights, seL4_ARM_Default_VMAttributes);
                
                if (map_err == seL4_NoError) {
                    seL4_SetMR(0, 0); // Успех
                } else {
                    seL4_SetMR(0, (seL4_Word)-1); // Ошибка маппинга (например, адрес уже занят)
                }
                seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                break;
            }

            default:
                seL4_SetMR(0, (seL4_Word)-1); seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                break;
        }
    }
    return 0;
}