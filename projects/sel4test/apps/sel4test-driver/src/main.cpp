#include "common.h"
#include "allocator.h"
#include "uart.h"
#include "patient.h"
#include "hw_timer.h" 

extern "C" {
#include <cpio/cpio.h>
#include <elf/elf.h>
}
#include <string.h>

extern char _cpio_archive[];
extern char _cpio_archive_end[];

enum SyscallID {
    SYS_PRINT = 1, SYS_YIELD = 2, SYS_GET_TIME = 3, SYS_SLEEP = 4, 
    SYS_PUTCHAR = 5, SYS_READ = 6, SYS_ALLOC = 7,
    SYS_READFILE = 98, SYS_DOCTOR = 99, SYS_EXEC = 100, SYS_LS = 101,
    SYS_EXIT = 102, SYS_KILL = 103, SYS_PS = 104,
    UART_IRQ_BADGE = (1 << 16), TIMER_IRQ_BADGE = (1 << 17)
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
};
static ProcessControlBlock pcbs[256];
static int next_pid = 1;

static int spawn_process(const char* elf_name, seL4_CPtr ep, seL4_CPtr med_ep,
                          PsychAllocator &alloc, seL4_CPtr root_cnode, seL4_CPtr root_vspace,
                          seL4_CPtr normal_untyped, seL4_CPtr shm_frame_root) {
    unsigned long elf_size = 0;
    unsigned long archive_len = _cpio_archive_end - _cpio_archive;
    char *elf_file = (char*)cpio_get_file(_cpio_archive, archive_len, elf_name, &elf_size);
    if (!elf_file) return -1;

    int pid = next_pid++;
    ProcessControlBlock& pcb = pcbs[pid];
    pcb.pid = pid;
    strncpy(pcb.name, elf_name, 31); pcb.name[31] = '\0';
    pcb.active = true;

    // Создаем "badged" копию Endpoint, чтобы ядро всегда знало, что это пишет именно этот процесс
    seL4_CPtr badged_ep = alloc.alloc_slot();
    seL4_CNode_Mint(root_cnode, badged_ep, seL4_WordBits, root_cnode, ep, seL4_WordBits, seL4_AllRights, pid);
    pcb.badged_ep = badged_ep;

    elf_t elf;
    elf_newFile(elf_file, elf_size, &elf);
    uint64_t entry_point = elf_getEntryPoint(&elf);

    // VSpace для песочницы
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

    // Копируем сегменты ELF
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

    seL4_ARM_Page_Map(ipc_frame, root_vspace, temp_window, seL4_AllRights, seL4_ARM_Default_VMAttributes);
    seL4_IPCBuffer *child_ipc_ptr = (seL4_IPCBuffer*)temp_window;
    memset(child_ipc_ptr, 0, 4096);
    child_ipc_ptr->userData = ep;
    child_ipc_ptr->caps_or_badges[0] = ep;
    seL4_ARM_Page_Unmap(ipc_frame);
    seL4_ARM_Page_Map(stack_frame, child_vspace, child_stack, seL4_AllRights, seL4_ARM_Default_VMAttributes);
    seL4_ARM_Page_Map(ipc_frame, child_vspace, child_ipc, seL4_AllRights, seL4_ARM_Default_VMAttributes);

    // Shared memory
    seL4_CPtr shm_frame_child = alloc.alloc_slot();
    seL4_CNode_Copy(root_cnode, shm_frame_child, seL4_WordBits, root_cnode, shm_frame_root, seL4_WordBits, seL4_AllRights);
    uintptr_t child_shm = 0x502000;
    seL4_ARM_Page_Map(shm_frame_child, child_vspace, child_shm, seL4_AllRights, seL4_ARM_Default_VMAttributes);

    // TCB пациента
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

    // --- Доктор сервис ---
    seL4_CPtr doc_tcb = alloc.alloc_slot();
    seL4_CPtr doc_ipc_frame = alloc.alloc_slot();
    seL4_Untyped_Retype(normal_untyped, seL4_TCBObject, 0, root_cnode, 0, 0, doc_tcb, 1);
    seL4_Untyped_Retype(normal_untyped, seL4_ARM_SmallPageObject, 0, root_cnode, 0, 0, doc_ipc_frame, 1);

    int doc_pid = next_pid++;
    ProcessControlBlock& doc_pcb = pcbs[doc_pid];
    doc_pcb.pid = doc_pid;
    strcpy(doc_pcb.name, "doctor_thread");
    doc_pcb.active = true;
    doc_pcb.tcb = doc_tcb;
    doc_pcb.vspace = root_vspace;
    
    seL4_CPtr doc_badged_ep = alloc.alloc_slot();
    seL4_CNode_Mint(root_cnode, doc_badged_ep, seL4_WordBits, root_cnode, ep, seL4_WordBits, seL4_AllRights, doc_pid);
    doc_pcb.badged_ep = doc_badged_ep;

    uintptr_t doc_ipc_vaddr = 0x200005000ULL;
    seL4_ARM_Page_Map(doc_ipc_frame, root_vspace, doc_ipc_vaddr, seL4_AllRights, seL4_ARM_Default_VMAttributes);
    seL4_TCB_Configure(doc_tcb, doc_badged_ep, root_cnode, seL4_NilData, root_vspace, seL4_NilData, doc_ipc_vaddr, doc_ipc_frame);
    seL4_TCB_SetPriority(doc_tcb, seL4_CapInitThreadTCB, 254);

    seL4_UserContext doc_regs = {0};
    doc_regs.pc = (seL4_Word)doctor_thread;
    doc_regs.sp = (seL4_Word)(doctor_stack + sizeof(doctor_stack));
    doc_regs.x0 = (seL4_Word)doc_badged_ep;           
    doc_regs.x1 = (seL4_Word)doc_ipc_vaddr;
    doc_regs.x2 = (seL4_Word)med_ep;       
    doc_regs.tpidr_el0 = (seL4_Word)doctor_tls;
    size_t reg_count = sizeof(seL4_UserContext) / sizeof(seL4_Word);
    seL4_TCB_WriteRegisters(doc_tcb, 0, 0, reg_count, &doc_regs);
    seL4_TCB_Resume(doc_tcb);

    // Shared memory (Оставляем ROOT себе, раздаем копии детям)
    seL4_CPtr shm_frame_root = alloc.alloc_slot();
    seL4_Untyped_Retype(normal_untyped, seL4_ARM_SmallPageObject, 0, root_cnode, 0, 0, shm_frame_root, 1);
    uintptr_t root_shm  = 0x200006000ULL;
    seL4_ARM_Page_Map(shm_frame_root, root_vspace, root_shm, seL4_AllRights, seL4_ARM_Default_VMAttributes);

    // --- Загрузка ELF ---
    if (spawn_process("patient_app", ep, med_ep, alloc, root_cnode, root_vspace, normal_untyped, shm_frame_root) < 0) {
        uart_puts("PANIC: patient_app not found!\n");
        while(1);
    }

    // Прерывания
    seL4_CPtr global_hub = alloc.alloc_slot();
    seL4_CPtr badged_irq_ntfn = alloc.alloc_slot();
    seL4_CPtr badged_timer_ntfn = alloc.alloc_slot();
    seL4_CPtr irq_handler = alloc.alloc_slot();
    seL4_CPtr timer_handler = alloc.alloc_slot();

    seL4_Untyped_Retype(normal_untyped, seL4_NotificationObject, 0, root_cnode, 0, 0, global_hub, 1);
    seL4_CNode_Mint(root_cnode, badged_irq_ntfn, seL4_WordBits, root_cnode, global_hub, seL4_WordBits, seL4_AllRights, UART_IRQ_BADGE);
    seL4_CNode_Mint(root_cnode, badged_timer_ntfn, seL4_WordBits, root_cnode, global_hub, seL4_WordBits, seL4_AllRights, TIMER_IRQ_BADGE);
    seL4_IRQControl_Get(seL4_CapIRQControl, 33, root_cnode, irq_handler, seL4_WordBits);
    seL4_IRQHandler_SetNotification(irq_handler, badged_irq_ntfn);
    uart_enable_interrupts();
    seL4_IRQHandler_Ack(irq_handler);
    seL4_IRQControl_Get(seL4_CapIRQControl, 34, root_cnode, timer_handler, seL4_WordBits);
    seL4_IRQHandler_SetNotification(timer_handler, badged_timer_ntfn);
    seL4_TCB_BindNotification(seL4_CapInitThreadTCB, global_hub);

    uart_puts("Patient Sandbox spawned. Kernel is now serving syscalls...\n");

    // --- ЕДИНЫЙ ЦИКЛ ЯДРА ---
    while (1) {
        seL4_Word sender_badge = 0;
        seL4_MessageInfo_t recv_info = seL4_Recv(ep, &sender_badge);
        seL4_Word sender_pid = 0;

        if (sender_badge & UART_IRQ_BADGE) {
            if (uart_havechar()) {
                    char c = uart_getchar();
                    if (reader_waiting) {
                        seL4_MessageInfo_t reply_msg = seL4_MessageInfo_new(0, 0, 0, 1);
                        seL4_SetMR(0, c);
                        seL4_Send(reader_reply_slot, reply_msg);
                        check_err(seL4_CNode_Delete(root_cnode, reader_reply_slot, seL4_WordBits), "Delete reader cap");
                        reader_waiting = false;
                    } else {
                        int next_head = (kbd_buffer_head + 1) % KBD_BUFFER_SIZE;
                        if (next_head != kbd_buffer_tail) {
                            kbd_buffer[kbd_buffer_head] = c;
                            kbd_buffer_head = next_head;
                        }
                    }
            }
            seL4_IRQHandler_Ack(irq_handler);
            continue;
        } else if (sender_badge & TIMER_IRQ_BADGE) {
                pl031_clear_interrupt();
                if (sleeper_waiting) {
                    seL4_SetMR(0, 0);
                    seL4_Send(sleeper_reply_slot, seL4_MessageInfo_new(0, 0, 0, 1));
                    check_err(seL4_CNode_Delete(root_cnode, sleeper_reply_slot, seL4_WordBits), "Delete sleeper cap");
                    sleeper_waiting = false;
                }
            seL4_IRQHandler_Ack(timer_handler);
            continue;
        } else if (sender_badge != 0 && sender_badge < 256 && pcbs[sender_badge].active) {
            sender_pid = sender_badge;
        }

        seL4_Word label = seL4_MessageInfo_get_label(recv_info);
        
        // ==========================================================
        // ИСПРАВЛЕНИЕ: ПЕРЕХВАТ ПАДЕНИЙ ПАМЯТИ (DEMAND PAGING!)
        // ==========================================================
        if (label == seL4_Fault_VMFault) {
            seL4_Word pc = seL4_GetMR(0);
            seL4_Word addr = seL4_GetMR(1);
            
            // Если Пациент обратился к памяти в диапазоне 0x510000 - 0x5FFFFF,
            // мы выделяем ему ОЗУ "на лету" и возобновляем выполнение!
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
            
            // НОВОЕ: Чтение файла из CPIO (VFS)
            case SYS_READFILE: { 
                char *shm = (char*)0x200006000ULL; 
                unsigned long file_size = 0;
                unsigned long archive_len = _cpio_archive_end - _cpio_archive;
                
                // Ищем файл в архиве по имени, которое Пациент оставил в SHM
                char *file_data = (char*)cpio_get_file(_cpio_archive, archive_len, shm, &file_size);
                
                if (file_data) {
                    uart_puts("\n[VFS] Patient requested file: "); uart_puts(shm); uart_puts("\n");
                    // Копируем файл в SHM (до 4КБ)
                    unsigned long copy_size = (file_size > 4095) ? 4095 : file_size;
                    memcpy(shm, file_data, copy_size);
                    shm[copy_size] = '\0';
                    seL4_SetMR(0, copy_size); // Возвращаем размер
                } else {
                    seL4_SetMR(0, (seL4_Word)-1); // Файл не найден
                }
                seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
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
                uart_puts("\n[KERNEL] Executing new program: "); uart_puts(shm); uart_puts("\n");
                int new_pid = spawn_process(shm, ep, med_ep, alloc, root_cnode, root_vspace, normal_untyped, shm_frame_root);
                if (new_pid > 0) {
                    seL4_SetMR(0, (seL4_Word)new_pid); // Возвращаем PID
                } else {
                    uart_puts("[KERNEL] Program not found in VFS.\n");
                    seL4_SetMR(0, (seL4_Word)-1);
                }
                seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                break;
            }
            case SYS_PS: {
                char *shm = (char*)0x200006000ULL;
                strcpy(shm, "  PID NAME\n");
                int len = strlen(shm);
                for (int i = 1; i < 256; i++) {
                    if (pcbs[i].active) {
                        char num_buf[16];
                        int val = pcbs[i].pid;
                        int n = 0;
                        if (val == 0) { num_buf[n++] = '0'; }
                        else {
                            while(val > 0) { num_buf[n++] = (val % 10) + '0'; val /= 10; }
                        }
                        num_buf[n] = '\0';
                        for(int j=0; j<n/2; j++) { char t=num_buf[j]; num_buf[j]=num_buf[n-1-j]; num_buf[n-1-j]=t; }
                        strcpy(shm + len, "    "); len += 4;
                        strcpy(shm + len, num_buf); len += n;
                        strcpy(shm + len, " "); len += 1;
                        strcpy(shm + len, pcbs[i].name); len += strlen(pcbs[i].name);
                        strcpy(shm + len, "\n"); len += 1;
                    }
                }
                seL4_SetMR(0, 0);
                seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                break;
            }
            case SYS_KILL: {
                seL4_Word target_pid = arg1;
                if (target_pid > 0 && target_pid < 256 && pcbs[target_pid].active) {
                    seL4_TCB_Suspend(pcbs[target_pid].tcb);
                    seL4_TCB_UnbindNotification(pcbs[target_pid].tcb);
                    pcbs[target_pid].active = false;
                    seL4_CNode_Delete(root_cnode, pcbs[target_pid].badged_ep, seL4_WordBits);
                    seL4_CNode_Delete(root_cnode, pcbs[target_pid].tcb, seL4_WordBits);
                    if (pcbs[target_pid].vspace != root_vspace) {
                        seL4_CNode_Delete(root_cnode, pcbs[target_pid].vspace, seL4_WordBits);
                    }
                    seL4_SetMR(0, 0);
                } else {
                    seL4_SetMR(0, (seL4_Word)-1);
                }
                seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                break;
            }
            case SYS_EXIT: {
                if (sender_pid > 0) {
                    seL4_TCB_Suspend(pcbs[sender_pid].tcb);
                    seL4_TCB_UnbindNotification(pcbs[sender_pid].tcb);
                    pcbs[sender_pid].active = false;
                    seL4_CNode_Delete(root_cnode, pcbs[sender_pid].badged_ep, seL4_WordBits);
                    seL4_CNode_Delete(root_cnode, pcbs[sender_pid].tcb, seL4_WordBits);
                    if (pcbs[sender_pid].vspace != root_vspace) {
                        seL4_CNode_Delete(root_cnode, pcbs[sender_pid].vspace, seL4_WordBits);
                    }
                }
                break; // Не отправляем Reply, процесс умер!
            }
            case SYS_LS: {
                char *shm = (char*)0x200006000ULL;
                char *ptr = _cpio_archive;
                char *end = _cpio_archive_end;
                int shm_offset = 0;
                
                // Парсим заголовки формата CPIO (newc)
                while (ptr + 110 <= end) {
                    if (strncmp(ptr, "070701", 6) != 0 && strncmp(ptr, "070702", 6) != 0) break;
                    
                    unsigned long namesize = 0, filesize = 0;
                    for (int i = 0; i < 8; i++) {
                        char c = ptr[94 + i];
                        int v = (c >= '0' && c <= '9') ? (c - '0') : ((c >= 'a' && c <= 'f') ? (c - 'a' + 10) : ((c >= 'A' && c <= 'F') ? (c - 'A' + 10) : 0));
                        namesize = (namesize << 4) | v;
                    }
                    for (int i = 0; i < 8; i++) {
                        char c = ptr[54 + i];
                        int v = (c >= '0' && c <= '9') ? (c - '0') : ((c >= 'a' && c <= 'f') ? (c - 'a' + 10) : ((c >= 'A' && c <= 'F') ? (c - 'A' + 10) : 0));
                        filesize = (filesize << 4) | v;
                    }
                    
                    char *name = ptr + 110;
                    if (strcmp(name, "TRAILER!!!") == 0) break; // Конец архива
                    
                    int len = strlen(name);
                    if (shm_offset + len + 2 < 4096) {
                        strcpy(shm + shm_offset, name);
                        shm_offset += len;
                        shm[shm_offset++] = '\n';
                    }
                    
                    ptr += (110 + namesize + 3) & ~3; // Выравниваем до 4 байт
                    ptr += (filesize + 3) & ~3;
                }
                shm[shm_offset] = '\0';
                seL4_SetMR(0, 0);
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