#include "h/common.h"
#include "h/allocator.h"
#include "h/uart.h"
#include "h/hw_timer.h" 

#include <sel4/sel4.h>

extern "C" {
#include <cpio/cpio.h>
#include <elf/elf.h>
}
#include <string.h>

extern char _cpio_archive[];
extern char _cpio_archive_end[];

// Выделяем гарантированно безопасный 1 МБ в секции BSS Rootserver'а
static char global_lib_staging_buffer[1024 * 1024]; 

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
    SYS_CLONE = 101,
    SYS_KILL = 102, 
    SYS_EXIT = 103, 
    SYS_PS = 104,
    SYS_THREAD_EXIT = 105,
    SYS_WAIT = 106, 
    SYS_SHM_GET = 107,
    SYS_GETPID = 108,
    SYS_RECOVER = 117,

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

// --- В начале файла или внутри spawn_process ---
// Базовые адреса для временного маппинга (сдвигаются атомарно)
static uintptr_t global_elf_temp_vaddr = 0x200100000ULL;
static uintptr_t global_ipc_temp_vaddr = 0x200800000ULL;

static char* rootserver_shm_base = (char*)0x502000ULL; // Адрес SHM внутри Rootserver-а
static seL4_CPtr shm_frames[4]; // Массив Capability для 4-х страниц SHM

static uintptr_t untyped_watermarks[256] = {0};

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

struct CapTracker {
    seL4_CPtr caps[64];
    int count;
};

struct ProcessControlBlock {
    seL4_Word pid;
    char name[32];
    seL4_CPtr tcb;
    seL4_CPtr vspace;
    seL4_CPtr cspace;
    seL4_CPtr badged_ep;
    uintptr_t vmap_bump_pointer;   // Курсор для динамического выделения памяти
    bool active;
    int waiting_for;
    seL4_CPtr reply_cap;
    seL4_CPtr thread_ipc_frame;
    seL4_CPtr thread_stack_frame1;
    seL4_CPtr thread_stack_frame2;

    // --- ГЕНЕРИЧЕСКИЕ МЕТАДАННЫЕ ДЛЯ АВТОПЕРЕЗАПУСКА ---
    int is_driver;
    seL4_CPtr irq_ntfn;
    seL4_CPtr irq_handler;
    seL4_CPtr hw_frame;
    seL4_CPtr net_cmd_recv_ep;
    seL4_CPtr net_cmd_send_ep;

    CapTracker cap_tracker;

    // --- НОВОЕ: Трекинг копий SHM для защиты от утечек ---
    bool has_shm;
    seL4_CPtr shm_copies[4];
};
static ProcessControlBlock pcbs[256];
static int next_pid = 1;

static seL4_CPtr alloc_and_track_cap(PsychAllocator &alloc, ProcessControlBlock &pcb) {
    seL4_CPtr cap = alloc.alloc_slot();
    
    if (cap == 0) {
        uart_puts("KERNEL PANIC: Out of CSlots during process allocation!\n");
        while(1);
    }

    if (pcb.cap_tracker.count < 64) {
        pcb.cap_tracker.caps[pcb.cap_tracker.count++] = cap;
    } else {
        uart_puts("PANIC: Process exceeded capability tracking limit!\n");
        while(1);
    }
    return cap;
}
struct SharedMemoryRegion {
    bool active;
    seL4_CPtr frame_cap; // Физический фрейм памяти
};
static SharedMemoryRegion shm_regions[16];

static int load_elf_from_disk(seL4_CPtr blk_ep, const char* filename, char* load_buffer) {
    char* shm = rootserver_shm_base;
    uint32_t total_read = 0;

    // ВРЕМЕННЫЙ ПРИНТ ДЛЯ ОТЛАДКИ
    uart_puts("[ROOT] Calling load_elf_from_disk for: '");
    uart_puts(filename);
    uart_puts("'\n");

    while (1) {
        // Драйвер перезаписывает SHM, поэтому имя файла восстанавливаем перед каждым запросом
        strncpy(shm, filename, 63);
        shm[63] = '\0';
        
        seL4_SetMR(0, 119); // SYS_READ_FILE
        seL4_SetMR(1, total_read); // Передаем СМЕЩЕНИЕ (offset)
        
        seL4_MessageInfo_t msg = seL4_MessageInfo_new(0, 0, 0, 2);
        seL4_Call(blk_ep, msg);
        
        int32_t status = seL4_GetMR(0);
        uint32_t chunk_size = seL4_GetMR(1);
        
        if (status == -1 || chunk_size == 0) {
            break;
        }
        
        // Копируем полученный безопасный кусок в большой буфер Rootserver'а
        memcpy(load_buffer + total_read, shm, chunk_size);
        total_read += chunk_size;
    }
    
    return total_read;
}

// Умная функция маппинга (Самовосстанавливающееся дерево VSpace)
static bool map_frame_robust(PsychAllocator &alloc, ProcessControlBlock &pcb, seL4_CPtr frame, seL4_CPtr vspace, uintptr_t vaddr, seL4_CPtr normal_untyped, seL4_CPtr root_cnode) {
    // Сначала пробуем замапить фрейм напрямую
    seL4_Error err = seL4_ARM_Page_Map(frame, vspace, vaddr, seL4_AllRights, seL4_ARM_Default_VMAttributes);
    
    if (err == seL4_FailedLookup) {
        // Не хватает промежуточных каталогов. Создаем их вслепую.
        // Если каталог уже существует (например, PGD[0]), seL4 вернет DeleteFirst (8). Мы ИГНОРИРУЕМ эту ошибку.
        
        seL4_CPtr pud = alloc_and_track_cap(alloc, pcb);
        if (seL4_Untyped_Retype(normal_untyped, seL4_ARM_PageUpperDirectoryObject, 0, root_cnode, 0, 0, pud, 1) == seL4_NoError) {
            seL4_ARM_PageUpperDirectory_Map(pud, vspace, vaddr, seL4_ARM_Default_VMAttributes);
        }

        seL4_CPtr pd = alloc_and_track_cap(alloc, pcb);
        if (seL4_Untyped_Retype(normal_untyped, seL4_ARM_PageDirectoryObject, 0, root_cnode, 0, 0, pd, 1) == seL4_NoError) {
            seL4_ARM_PageDirectory_Map(pd, vspace, vaddr, seL4_ARM_Default_VMAttributes);
        }

        seL4_CPtr pt = alloc_and_track_cap(alloc, pcb);
        if (seL4_Untyped_Retype(normal_untyped, seL4_ARM_PageTableObject, 0, root_cnode, 0, 0, pt, 1) == seL4_NoError) {
            seL4_ARM_PageTable_Map(pt, vspace, vaddr, seL4_ARM_Default_VMAttributes);
        }

        // Дерево проложено. Мапим фрейм повторно.
        err = seL4_ARM_Page_Map(frame, vspace, vaddr, seL4_AllRights, seL4_ARM_Default_VMAttributes);
    }
    
    if (err != seL4_NoError) {
        uart_puts("[ROOT] FATAL: Robust map failed!\n");
        return false;
    }
    return true;
}

static int spawn_process(const char* name, char* elf_data, unsigned long elf_size, seL4_CPtr ep, seL4_CPtr med_ep,
                         PsychAllocator &alloc, seL4_CPtr root_cnode, seL4_CPtr root_vspace,
                         seL4_CPtr normal_untyped, seL4_CPtr shm_frame,
                         int is_driver, seL4_CPtr console_ep, seL4_CPtr timer_ep, seL4_CPtr blk_ep,
                         seL4_CPtr irq_ntfn, seL4_CPtr irq_handler, seL4_CPtr hw_frame,
                         const char *args_payload = nullptr,
                         seL4_CPtr net_cmd_recv_ep = 0,
                         seL4_CPtr net_cmd_send_ep = 0) {
    
    char *elf_file = elf_data;
    if (!elf_file) {
        unsigned long archive_len = _cpio_archive_end - _cpio_archive;
        elf_file = (char*)cpio_get_file(_cpio_archive, archive_len, name, &elf_size);
    }
    if (!elf_file) return -1;

    int pid = -1;
    for (int i = 1; i < 256; i++) {
        if (!pcbs[i].active) {
            pid = i;
            break;
        }
    }

    if (pid == -1) {
        seL4_DebugPutString((char*)"KERNEL PANIC: Out of PIDs!\n");
        return -1;
    }

    ProcessControlBlock& pcb = pcbs[pid];
    memset(&pcb, 0, sizeof(ProcessControlBlock));
    pcb.pid = pid;
    pcb.has_shm = false;
    pcb.active = true;

    strncpy(pcb.name, name, 31); 
    pcb.name[31] = '\0';

    // 1. Создаем локальный CSpace (8 бит = 256 слотов)
    seL4_CPtr child_cnode = alloc_and_track_cap(alloc, pcb);
    seL4_Untyped_Retype(normal_untyped, seL4_CapTableObject, 8, root_cnode, 0, 0, child_cnode, 1);

    seL4_CPtr badged_ep = alloc_and_track_cap(alloc, pcb);
    seL4_CNode_Mint(root_cnode, badged_ep, seL4_WordBits, 
                    root_cnode, ep, seL4_WordBits, seL4_AllRights, pid);
    
    // 3. Наши локальные "файловые дескрипторы"
    seL4_Word local_console_ep  = 1;
    seL4_Word local_timer_ep    = 2;
    seL4_Word local_net_send_ep = 3;
    seL4_Word local_irq_handler = 4;
    seL4_Word local_net_recv_ep = 5;
    seL4_Word local_blk_ep      = 7; // VFS/Block Driver Endpoint
    seL4_Word local_syscall_ep  = 10; // <-- Локальный индекс для Faults и Syscalls

    check_err(seL4_CNode_Copy(child_cnode, local_syscall_ep, 8, root_cnode, badged_ep, seL4_WordBits, seL4_AllRights), "Copy syscall ep");

    // ИСПРАВЛЕНО: Упрощена и исправлена логика копирования. Теперь она зависит от аргументов, а не от PCB.
    if (console_ep != 0) {
        if (is_driver == 1) check_err(seL4_CNode_Copy(child_cnode, local_console_ep, 8, root_cnode, console_ep, seL4_WordBits, seL4_AllRights), "Copy console ep");
        else check_err(seL4_CNode_Mint(child_cnode, local_console_ep, 8, root_cnode, console_ep, seL4_WordBits, seL4_AllRights, pid), "Mint console ep");
    }
    if (timer_ep != 0) {
        if (is_driver == 2) check_err(seL4_CNode_Copy(child_cnode, local_timer_ep, 8, root_cnode, timer_ep, seL4_WordBits, seL4_AllRights), "Copy timer ep");
        else check_err(seL4_CNode_Mint(child_cnode, local_timer_ep, 8, root_cnode, timer_ep, seL4_WordBits, seL4_AllRights, pid), "Mint timer ep");
    }
    if (blk_ep != 0) {
        if (is_driver == 3) check_err(seL4_CNode_Copy(child_cnode, local_blk_ep, 8, root_cnode, blk_ep, seL4_WordBits, seL4_AllRights), "Copy blk ep");
        else check_err(seL4_CNode_Mint(child_cnode, local_blk_ep, 8, root_cnode, blk_ep, seL4_WordBits, seL4_AllRights, pid), "Mint blk ep");
    }
    if (net_cmd_send_ep != 0) check_err(seL4_CNode_Mint(child_cnode, local_net_send_ep, 8, root_cnode, net_cmd_send_ep, seL4_WordBits, seL4_AllRights, pid), "Mint net send ep");
    if (irq_handler != 0) check_err(seL4_CNode_Copy(child_cnode, local_irq_handler, 8, root_cnode, irq_handler, seL4_WordBits, seL4_AllRights), "Copy IRQ handler");
    if (net_cmd_recv_ep != 0) check_err(seL4_CNode_Copy(child_cnode, local_net_recv_ep, 8, root_cnode, net_cmd_recv_ep, seL4_WordBits, seL4_AllRights), "Copy net recv ep");

    pcb.cspace = child_cnode;
    pcb.badged_ep = badged_ep; // Оставляем глобальный в pcb для нужд ядра

    // СОХРАНЯЕМ АППАРАТНЫЙ ПРОФИЛЬ В PCB:
    pcb.is_driver = is_driver; 
    pcb.irq_ntfn = irq_ntfn;
    pcb.irq_handler = irq_handler;
    pcb.hw_frame = hw_frame;
    pcb.net_cmd_recv_ep = net_cmd_recv_ep;
    pcb.net_cmd_send_ep = net_cmd_send_ep;

    elf_t elf;
    elf_newFile(elf_file, elf_size, &elf);
    uint64_t entry_point = elf_getEntryPoint(&elf);

    // VSpace (Виртуальная память песочницы)
    seL4_CPtr child_vspace = alloc_and_track_cap(alloc, pcb);
    seL4_CPtr child_pud = alloc_and_track_cap(alloc, pcb);
    seL4_CPtr child_pd  = alloc_and_track_cap(alloc, pcb);
    seL4_CPtr child_pt  = alloc_and_track_cap(alloc, pcb);
    seL4_CPtr child_pt2 = alloc_and_track_cap(alloc, pcb); // Вторая таблица для потоков

    seL4_Untyped_Retype(normal_untyped, seL4_ARM_PageGlobalDirectoryObject, 0, root_cnode, 0, 0, child_vspace, 1);
    seL4_Untyped_Retype(normal_untyped, seL4_ARM_PageUpperDirectoryObject, 0, root_cnode, 0, 0, child_pud, 1);
    seL4_Untyped_Retype(normal_untyped, seL4_ARM_PageDirectoryObject, 0, root_cnode, 0, 0, child_pd, 1);
    seL4_Untyped_Retype(normal_untyped, seL4_ARM_PageTableObject, 0, root_cnode, 0, 0, child_pt, 1);
    seL4_Untyped_Retype(normal_untyped, seL4_ARM_PageTableObject, 0, root_cnode, 0, 0, child_pt2, 1); // Выделяем объект

    seL4_ARM_ASIDPool_Assign(seL4_CapInitThreadASIDPool, child_vspace);
    seL4_ARM_PageUpperDirectory_Map(child_pud, child_vspace, 0x400000, seL4_ARM_Default_VMAttributes);
    seL4_ARM_PageDirectory_Map(child_pd, child_vspace, 0x400000, seL4_ARM_Default_VMAttributes);
    seL4_ARM_PageTable_Map(child_pt, child_vspace, 0x400000, seL4_ARM_Default_VMAttributes);     // Покрывает 0x400000 - 0x5FFFFF
    seL4_ARM_PageTable_Map(child_pt2, child_vspace, 0x600000, seL4_ARM_Default_VMAttributes);    // Покрывает 0x600000 - 0x7FFFFF (Тут живут наши потоки)

    pcbs[pid].vmap_bump_pointer = 0x60000000; // Курсор для динамического маппинга


    // Внутри функции spawn_process:
    // Атомарно выделяем окно 16MB под ELF и 4KB под IPC для каждого нового процесса
    // ИСПРАВЛЕНИЕ: Инкремент в 16MB (0x1000000) на каждый процесс не масштабируется, так как требует
    // маппинга огромного количества таблиц страниц. Для временного окна достаточно 4KB,
    // поэтому используем инкремент 0x1000, чтобы каждый параллельный вызов получил
    // уникальную 64KB-страницу в пределах заранее смапленного 1MB-региона.
    uintptr_t elf_temp_vaddr = global_elf_temp_vaddr;
    global_elf_temp_vaddr += 0x10000;
    if (global_elf_temp_vaddr >= 0x200100000ULL + 0x100000) {
        global_elf_temp_vaddr = 0x200100000ULL;
    }

    // Внутри цикла перебора Program Headers в spawn_process:
    
    bool is_dynamic = false;
    uint64_t dyn_vaddr = 0;
    uint64_t dyn_size = 0;
    uint64_t base_address = 0x400000; // Базовый адрес для PIE (обычно ELF загружаются сюда)

    // 1. Сначала определяем, является ли бинарник динамическим (PIE)
    // ВАЖНО: Код маппинга основного ELF файла перенесен ПОСЛЕ кода динамического линкера,
    // чтобы линкер мог сначала пропатчить GOT/PLT в staging-буфере, и только потом
    // эти уже измененные страницы будут скопированы в память процесса.

    for (int i = 0; i < elf_getNumProgramHeaders(&elf); i++) {
        if (elf_getProgramHeaderType(&elf, i) == 2) { // PT_DYNAMIC
            is_dynamic = true;
            // В PIE-файлах vaddr - это смещение. Прибавляем базовый адрес.
            dyn_vaddr = base_address + elf_getProgramHeaderVaddr(&elf, i);
            dyn_size = elf_getProgramHeaderMemorySize(&elf, i);
            break;
        }
    }

    // 3. Анализ динамической секции после загрузки в память
    if (is_dynamic) {
        uart_puts("[LINKER] Auto-resolving dependencies...\n");

        // Виртуальный адрес для первой библиотеки. Последующие будут размещаться выше.
        uint64_t current_lib_vaddr = 0x800000;

        uint64_t dyn_offset = 0;
        // Ищем смещение секции .dynamic в самом файле (elf_file)
        for (int i = 0; i < elf_getNumProgramHeaders(&elf); i++) {
            if (elf_getProgramHeaderType(&elf, i) == PT_DYNAMIC) { // PT_DYNAMIC = 2
                dyn_offset = elf_getProgramHeaderOffset(&elf, i);
                break;
            }
        }

        if (dyn_offset != 0) {
            Elf64_Dyn* dyn_table = (Elf64_Dyn*)((uintptr_t)elf_file + dyn_offset);
            uint64_t strtab_offset = 0;

            // 1. Ищем таблицу строк (DT_STRTAB)
            for (int i = 0; dyn_table[i].d_tag != 0; i++) {
                if (dyn_table[i].d_tag == 5 /* DT_STRTAB */) {
                    uint64_t strtab_vaddr = dyn_table[i].d_un.d_ptr;
                    for (int j = 0; j < elf_getNumProgramHeaders(&elf); j++) {
                        if (elf_getProgramHeaderType(&elf, j) == PT_LOAD /* PT_LOAD = 1 */) {
                            uint64_t p_vaddr = elf_getProgramHeaderVaddr(&elf, j);
                            uint64_t p_memsz = elf_getProgramHeaderMemorySize(&elf, j);
                            if (strtab_vaddr >= p_vaddr && strtab_vaddr < p_vaddr + p_memsz) {
                                strtab_offset = elf_getProgramHeaderOffset(&elf, j) + (strtab_vaddr - p_vaddr);
                                break;
                            }
                        }
                    }
                    break;
                }
            }

            // 2. Ищем все теги DT_NEEDED и автоматически грузим их!
            if (strtab_offset != 0) {
                for (int i = 0; dyn_table[i].d_tag != 0; i++) {
                    if (dyn_table[i].d_tag == 1 /* DT_NEEDED */) {
                        uint64_t name_idx = dyn_table[i].d_un.d_val;
                        char* lib_name = (char*)((uintptr_t)elf_file + strtab_offset + name_idx);
                        
                        uart_puts("[LINKER] Need library: ");
                        uart_puts(lib_name);
                        uart_puts("\n");

                        // Используем глобальный буфер для загрузки библиотеки
                        char* lib_buf = global_lib_staging_buffer;
                        memset(lib_buf, 0, 1024 * 1024); // Очищаем 1 МБ под либу
                        
                        // АВТОМАТИЧЕСКАЯ ЗАГРУЗКА БЕЗ ХАРДКОДА!
                        int lib_size = load_elf_from_disk(blk_ep, lib_name, lib_buf);

                        if (lib_size > 0) {
                            elf_t lib_elf;
                            if (elf_newFile(lib_buf, lib_size, &lib_elf) == 0) {
                                
                                for (int j = 0; j < elf_getNumProgramHeaders(&lib_elf); j++) {
                                    if (elf_getProgramHeaderType(&lib_elf, j) == PT_LOAD /* 1 */) {
                                        uint64_t l_offset = elf_getProgramHeaderOffset(&lib_elf, j);
                                        uint64_t l_vaddr  = elf_getProgramHeaderVaddr(&lib_elf, j) + current_lib_vaddr;
                                        uint64_t l_filesz = elf_getProgramHeaderFileSize(&lib_elf, j);
                                        uint64_t l_memsz  = elf_getProgramHeaderMemorySize(&lib_elf, j);

                                        uint64_t p_start = l_vaddr & ~0xFFFULL;
                                        uint64_t p_end = (l_vaddr + l_memsz + 0xFFF) & ~0xFFFULL;

                                        for (uint64_t page = p_start; page < p_end; page += 4096) {
                                            seL4_CPtr frame = alloc_and_track_cap(alloc, pcb);
                                            seL4_Untyped_Retype(normal_untyped, seL4_ARM_SmallPageObject, 0, root_cnode, 0, 0, frame, 1);
                                            
                                            seL4_ARM_Page_Map(frame, root_vspace, elf_temp_vaddr, seL4_AllRights, seL4_ARM_Default_VMAttributes);
                                            memset((void*)elf_temp_vaddr, 0, 4096);

                                            uint64_t copy_start = (page > l_vaddr) ? page : l_vaddr;
                                            uint64_t copy_end = (page + 4096 < l_vaddr + l_filesz) ? page + 4096 : l_vaddr + l_filesz;
                                            if (copy_start < copy_end) {
                                                memcpy((void*)(elf_temp_vaddr + (copy_start - page)), 
                                                       lib_buf + l_offset + (copy_start - l_vaddr), 
                                                       copy_end - copy_start);
                                            }
                                            seL4_ARM_Page_Clean_Data(frame, 0, 4096);
                                            seL4_ARM_Page_Unmap(frame);

                                            if (!map_frame_robust(alloc, pcb, frame, child_vspace, page, normal_untyped, root_cnode)) {
                                                uart_puts("[LINKER] FATAL: Failed to map library page!\n");
                                            }
                                        }
                                    }
                                }
                                uart_puts("[LINKER] Mapped successfully!\n");
                                
                                // ==========================================
                                // 4. THE RELOCATION ENGINE (Патчим GOT/PLT)
                                // ==========================================
                                uart_puts("[LINKER] Starting Relocation Phase...\n");
                                
                                uint64_t lib_base_address = 0x800000; 

                                uint64_t rela_offset = 0;
                                uint64_t relasz = 0;
                                uint64_t symtab_offset = 0;
                                // strtab_offset is already found above

                                // 1. Ищем нужные таблицы в основном файле (dyn_test.elf)
                                // dyn_offset is already found above

                                if (dyn_offset != 0) {
                                    Elf64_Dyn* dyn_table = (Elf64_Dyn*)((uintptr_t)elf_file + dyn_offset);
                                    
                                    // Функция-хелпер для конвертации VAddr -> Offset
                                    auto vaddr_to_offset = [&](uint64_t vaddr) -> uint64_t {
                                        for (int j = 0; j < elf_getNumProgramHeaders(&elf); j++) {
                                            if (elf_getProgramHeaderType(&elf, j) == 1 /* PT_LOAD */) {
                                                uint64_t p_vaddr = elf_getProgramHeaderVaddr(&elf, j);
                                                uint64_t p_memsz = elf_getProgramHeaderMemorySize(&elf, j);
                                                if (vaddr >= p_vaddr && vaddr < p_vaddr + p_memsz) {
                                                    return elf_getProgramHeaderOffset(&elf, j) + (vaddr - p_vaddr);
                                                }
                                            }
                                        }
                                        return 0;
                                    };

                                    for (int i = 0; dyn_table[i].d_tag != 0; i++) {
                                        if (dyn_table[i].d_tag == 7 /* DT_RELA */ || dyn_table[i].d_tag == 23 /* DT_JMPREL */) {
                                            rela_offset = vaddr_to_offset(dyn_table[i].d_un.d_ptr);
                                        } else if (dyn_table[i].d_tag == 8 /* DT_RELASZ */ || dyn_table[i].d_tag == 2 /* DT_PLTRELSZ */) {
                                            relasz = dyn_table[i].d_un.d_val;
                                        } else if (dyn_table[i].d_tag == 6 /* DT_SYMTAB */) {
                                            symtab_offset = vaddr_to_offset(dyn_table[i].d_un.d_ptr);
                                        }
                                        // strtab_offset is handled outside this loop
                                    }

                                    // 2. Ищем таблицу символов и строк в Библиотеке (libpsych.so)
                                    uint64_t lib_symtab_offset = 0;
                                    uint64_t lib_strtab_offset = 0;
                                    
                                    uint64_t lib_dyn_offset = 0;
                                    for (int i = 0; i < elf_getNumProgramHeaders(&lib_elf); i++) {
                                        if (elf_getProgramHeaderType(&lib_elf, i) == 2 /* PT_DYNAMIC */) {
                                            lib_dyn_offset = elf_getProgramHeaderOffset(&lib_elf, i);
                                            break;
                                        }
                                    }
                                    
                                    if (lib_dyn_offset != 0) {
                                        Elf64_Dyn* lib_dyn_table = (Elf64_Dyn*)((uintptr_t)lib_buf + lib_dyn_offset);
                                        for (int i = 0; lib_dyn_table[i].d_tag != 0; i++) {
                                            // Аналогичный хелпер для библиотеки
                                            auto lib_vaddr_to_offset = [&](uint64_t vaddr) -> uint64_t {
                                                for (int j = 0; j < elf_getNumProgramHeaders(&lib_elf); j++) {
                                                    if (elf_getProgramHeaderType(&lib_elf, j) == 1) {
                                                        uint64_t p_vaddr = elf_getProgramHeaderVaddr(&lib_elf, j);
                                                        uint64_t p_memsz = elf_getProgramHeaderMemorySize(&lib_elf, j);
                                                        if (vaddr >= p_vaddr && vaddr < p_vaddr + p_memsz) {
                                                            return elf_getProgramHeaderOffset(&lib_elf, j) + (vaddr - p_vaddr);
                                                        }
                                                    }
                                                }
                                                return 0;
                                            };

                                            if (lib_dyn_table[i].d_tag == 6 /* DT_SYMTAB */) {
                                                lib_symtab_offset = lib_vaddr_to_offset(lib_dyn_table[i].d_un.d_ptr);
                                            } else if (lib_dyn_table[i].d_tag == 5 /* DT_STRTAB */) {
                                                lib_strtab_offset = lib_vaddr_to_offset(lib_dyn_table[i].d_un.d_ptr);
                                            }
                                        }
                                    }

                                    // 3. Выполняем ПАТЧИНГ!
                                    if (rela_offset != 0 && symtab_offset != 0 && strtab_offset != 0 && 
                                        lib_symtab_offset != 0 && lib_strtab_offset != 0) {
                                        
                                        Elf64_Rela* relocs = (Elf64_Rela*)((uintptr_t)elf_file + rela_offset);
                                        Elf64_Sym* syms = (Elf64_Sym*)((uintptr_t)elf_file + symtab_offset);
                                        Elf64_Sym* lib_syms = (Elf64_Sym*)((uintptr_t)lib_buf + lib_symtab_offset);
                                        
                                        int num_relocs = relasz / sizeof(Elf64_Rela);
                                        
                                        uart_puts("[LINKER] Patching ");
                                        uart_putdec(num_relocs);
                                        uart_puts(" relocations...\n");

                                        for (int r = 0; r < num_relocs; r++) {
                                            uint32_t type = ELF64_R_TYPE(relocs[r].r_info);
                                            uint32_t sym_idx = ELF64_R_SYM(relocs[r].r_info);
                                            
                                            if (type == 1026 /* R_AARCH64_JUMP_SLOT */ || type == 1025 /* R_AARCH64_GLOB_DAT */) {
                                                uint32_t name_offset = syms[sym_idx].st_name;
                                                char* target_name = (char*)((uintptr_t)elf_file + strtab_offset + name_offset);
                                                
                                                uint64_t target_vaddr = 0;
                                                for (int k = 1; k < 1000; k++) {
                                                    if (lib_syms[k].st_name == 0) continue;
                                                    if (lib_strtab_offset + lib_syms[k].st_name > (uint64_t)lib_size) break; 
                                                    
                                                    char* lib_sym_name = (char*)((uintptr_t)lib_buf + lib_strtab_offset + lib_syms[k].st_name);
                                                    
                                                    if (my_strcmp(target_name, lib_sym_name) == 0) {
                                                        target_vaddr = lib_syms[k].st_value + lib_base_address;
                                                        break;
                                                    }
                                                }

                                                if (target_vaddr != 0) {
                                                    uint64_t patch_offset = vaddr_to_offset(relocs[r].r_offset);
                                                    if (patch_offset != 0) {
                                                        uint64_t* got_entry = (uint64_t*)((uintptr_t)elf_file + patch_offset);
                                                        *got_entry = target_vaddr;
                                                        
                                                        uart_puts("[LINKER] Patched ");
                                                        uart_puts(target_name);
                                                        uart_puts(" at GOT\n");
                                                    }
                                                } else {
                                                    uart_puts("[LINKER] WARNING: Symbol not found: ");
                                                    uart_puts(target_name);
                                                    uart_puts("\n");
                                                }
                                            }
                                        }
                                    }
                                }

                                // Сдвигаем адреса для следующей библиотеки (шаг 2 МБ)
                                current_lib_vaddr += 0x200000;
                            } else {
                                uart_puts("[LINKER] ERROR: Could not parse library ELF!\n");
                            }
                        } else {
                            uart_puts("[LINKER] ERROR: Could not load library from disk!\n");
                        }
                    }
                }
            }
        }

    }

    // 2. Модифицированная загрузка PT_LOAD (учитываем базовый адрес)
    // Этот код выполняется ПОСЛЕ динамического линкера, чтобы он мог пропатчить ELF в буфере.
    for (int i = 0; i < elf_getNumProgramHeaders(&elf); i++) {
        if (elf_getProgramHeaderType(&elf, i) == PT_LOAD) {
            uint64_t offset = elf_getProgramHeaderOffset(&elf, i);
            uint64_t vaddr = elf_getProgramHeaderVaddr(&elf, i);
            uint64_t file_size = elf_getProgramHeaderFileSize(&elf, i);
            uint64_t mem_size = elf_getProgramHeaderMemorySize(&elf, i);

            if (is_dynamic) {
                vaddr += base_address;
            }

            uint64_t page_start = vaddr & ~0xFFFULL;
            uint64_t page_end = (vaddr + mem_size + 0xFFF) & ~0xFFFULL;

            for (uint64_t page = page_start; page < page_end; page += 4096) {
                seL4_CPtr frame = alloc_and_track_cap(alloc, pcb);
                seL4_Untyped_Retype(normal_untyped, seL4_ARM_SmallPageObject, 0, root_cnode, 0, 0, frame, 1);

                seL4_ARM_Page_Map(frame, root_vspace, elf_temp_vaddr, seL4_AllRights, seL4_ARM_Default_VMAttributes);
                memset((void*)elf_temp_vaddr, 0, 4096);

                uint64_t copy_start = (page > vaddr) ? page : vaddr;
                uint64_t copy_end = (page + 4096 < vaddr + file_size) ? page + 4096 : vaddr + file_size;
                if (copy_start < copy_end) {
                    memcpy((void*)(elf_temp_vaddr + (copy_start - page)), elf_file + offset + (copy_start - vaddr), copy_end - copy_start);
                }
                seL4_ARM_Page_Clean_Data(frame, 0, 4096);
                seL4_ARM_Page_Unmap(frame);

                seL4_ARM_Page_Map(frame, child_vspace, page, seL4_AllRights, seL4_ARM_Default_VMAttributes);
            }
        }
    }

    if (is_dynamic) {
        entry_point += base_address; 
    }

    uintptr_t child_stack = 0x500000;
    uintptr_t child_ipc   = 0x501000;
    seL4_CPtr stack_frame = alloc_and_track_cap(alloc, pcb);
    seL4_CPtr ipc_frame = alloc_and_track_cap(alloc, pcb);
    seL4_Untyped_Retype(normal_untyped, seL4_ARM_SmallPageObject, 0, root_cnode, 0, 0, stack_frame, 1);
    seL4_Untyped_Retype(normal_untyped, seL4_ARM_SmallPageObject, 0, root_cnode, 0, 0, ipc_frame, 1);

    uintptr_t ipc_temp_vaddr = global_ipc_temp_vaddr;
    global_ipc_temp_vaddr += 0x1000;
    // Кольцевой буфер для IPC окон
    if (global_ipc_temp_vaddr >= 0x200800000ULL + 0x100000) {
        global_ipc_temp_vaddr = 0x200800000ULL;
    }

    check_err(seL4_ARM_Page_Map(ipc_frame, root_vspace, ipc_temp_vaddr, seL4_AllRights, seL4_ARM_Default_VMAttributes), "Map IPC to Root");
    memset((void*)ipc_temp_vaddr, 0, 4096);

    // Сдвиг +2048 байт от нашего нового безопасного адреса
    seL4_IPCBuffer *child_ipc_ptr = (seL4_IPCBuffer *)(ipc_temp_vaddr + 2048);
    
    // === STARTUP PAYLOAD ===
    if (args_payload && args_payload[0] != '\0') {
        strcpy((char*)&child_ipc_ptr->msg[0], args_payload);
    }

    child_ipc_ptr->msg[BOOT_ROOT_EP] = local_syscall_ep;

    if (is_driver > 0 && is_driver <= 4) { // Any driver
        seL4_CPtr drv_pud = alloc_and_track_cap(alloc, pcb);
        seL4_CPtr drv_pd  = alloc_and_track_cap(alloc, pcb);
        seL4_CPtr drv_pt  = alloc_and_track_cap(alloc, pcb);
        check_err(seL4_Untyped_Retype(normal_untyped, seL4_ARM_PageUpperDirectoryObject, 0, root_cnode, 0, 0, drv_pud, 1), "Retype Drv PUD");
        check_err(seL4_Untyped_Retype(normal_untyped, seL4_ARM_PageDirectoryObject, 0, root_cnode, 0, 0, drv_pd, 1), "Retype Drv PD");
        check_err(seL4_Untyped_Retype(normal_untyped, seL4_ARM_PageTableObject, 0, root_cnode, 0, 0, drv_pt, 1), "Retype Drv PT");
        
        uintptr_t hw_vaddr = (is_driver == 1) ? 0x200000000ULL : 
                             ((is_driver == 2) ? 0x200002000ULL : 0x200004000ULL);
        
        seL4_ARM_PageUpperDirectory_Map(drv_pud, child_vspace, hw_vaddr, (seL4_ARM_VMAttributes)0);
        seL4_ARM_PageDirectory_Map(drv_pd, child_vspace, hw_vaddr, (seL4_ARM_VMAttributes)0);
        seL4_ARM_PageTable_Map(drv_pt, child_vspace, hw_vaddr, (seL4_ARM_VMAttributes)0);

        int num_pages = (is_driver == 3 || is_driver == 4) ? 4 : 1;
        for (int i = 0; i < num_pages; i++) {
            seL4_CPtr frame_child = alloc_and_track_cap(alloc, pcb);
            check_err(seL4_CNode_Copy(root_cnode, frame_child, seL4_WordBits, 
                                      root_cnode, hw_frame + i, seL4_WordBits, seL4_AllRights), "Copy HW Frame Cap");
            check_err(seL4_ARM_Page_Map(frame_child, child_vspace, hw_vaddr + (i * 4096), 
                                        seL4_AllRights, (seL4_ARM_VMAttributes)0), "Map HW to Driver");
        }

        if (is_driver == 1) { // UART
            // UART driver является сервером для console_ep, он на нем слушает.
            child_ipc_ptr->msg[BOOT_CONSOLE_EP] = local_console_ep; 
            child_ipc_ptr->msg[BOOT_IRQ_EP] = local_irq_handler;
        } else if (is_driver == 2) { // Timer
            // Timer driver является сервером для timer_ep, он на нем слушает.
            child_ipc_ptr->msg[BOOT_TIMER_EP] = local_timer_ep; 
            child_ipc_ptr->msg[BOOT_IRQ_EP] = local_irq_handler; 
            child_ipc_ptr->msg[BOOT_CONSOLE_EP] = local_console_ep; // Таймер может логировать в консоль, но не обязан
        } else if (is_driver == 3) { // Block driver - клиент консоли
            child_ipc_ptr->msg[7] = local_blk_ep; // BOOT_BLK_EP
            child_ipc_ptr->msg[BOOT_CONSOLE_EP] = local_console_ep;
        } else if (is_driver == 4) { // Net driver - клиент консоли и таймера
            child_ipc_ptr->msg[BOOT_CONSOLE_EP] = local_console_ep;
            child_ipc_ptr->msg[BOOT_TIMER_EP] = local_timer_ep;
            child_ipc_ptr->msg[BOOT_NET_EP] = local_net_recv_ep;
        }

    } else {
        // Shell or other user app
        child_ipc_ptr->msg[BOOT_CONSOLE_EP] = local_console_ep;
        child_ipc_ptr->msg[BOOT_TIMER_EP] = local_timer_ep;
        child_ipc_ptr->msg[BOOT_NET_EP] = local_net_send_ep;
        child_ipc_ptr->msg[7] = local_blk_ep; // BOOT_BLK_EP
    }

    seL4_ARM_Page_Clean_Data(ipc_frame, 0, 4096);
    
    check_err(seL4_ARM_Page_Unmap(ipc_frame), "Unmap IPC from Root");

    check_err(seL4_ARM_Page_Map(stack_frame, child_vspace, child_stack, seL4_AllRights, seL4_ARM_Default_VMAttributes), "Map Stack to Child");
    check_err(seL4_ARM_Page_Map(ipc_frame, child_vspace, child_ipc, seL4_AllRights, seL4_ARM_Default_VMAttributes), "Map IPC to Child");

    seL4_CPtr tcb = alloc_and_track_cap(alloc, pcb);
    pcb.tcb = tcb;
    pcb.vspace = child_vspace;
    seL4_Untyped_Retype(normal_untyped, seL4_TCBObject, 0, root_cnode, 0, 0, tcb, 1);

    seL4_Word cspace_guard = seL4_CNode_CapData_new(0, seL4_WordBits - 8).words[0];
    
    seL4_TCB_Configure(
        tcb, 
        local_syscall_ep, // <-- Вернули как было
        child_cnode, 
        cspace_guard, 
        child_vspace, 
        seL4_NilData, 
        child_ipc + 2048, 
        ipc_frame
    );
    
    // ИСПРАВЛЕНО: Удален дублирующийся вызов seL4_TCB_Configure, который перезаписывал Fault Endpoint и скрывал падения.
    seL4_UserContext regs = {0};
    regs.sp = child_stack + 4096;
    regs.x0 = (seL4_Word)badged_ep;
    regs.x1 = (seL4_Word)child_ipc;
    regs.x2 = (seL4_Word)med_ep; 
    regs.tpidr_el0 = (seL4_Word)child_ipc + 3072;
    regs.tpidrro_el0 = (seL4_Word)child_ipc + 3072;

    // ИСПРАВЛЕНО: Устанавливаем PC после того, как он был скорректирован для PIE
    regs.pc = entry_point;

    size_t reg_count = sizeof(seL4_UserContext) / sizeof(seL4_Word);
    seL4_TCB_WriteRegisters(tcb, 0, 0, reg_count, &regs);

    
    seL4_TCB_SetTLSBase(tcb, child_ipc + 3072);
    seL4_TCB_SetPriority(tcb, seL4_CapInitThreadTCB, 254);

    // Привязываем прерывание ТОЛЬКО драйверам (Оболочка работает без IRQ)
    if (is_driver == 1 || is_driver == 2) {
        check_err(seL4_TCB_BindNotification(tcb, irq_ntfn), "Bind IRQ to Driver");
    }

    seL4_TCB_Resume(tcb);
    return pid;
}

static void generic_recover_process(int pid, seL4_CPtr ep, seL4_CPtr med_ep, PsychAllocator &alloc, 
                                    seL4_CPtr root_cnode, seL4_CPtr root_vspace, seL4_CPtr normal_untyped,
                                    seL4_CPtr shm_frame, seL4_CPtr console_ep, seL4_CPtr timer_ep, seL4_CPtr blk_ep) {
    if (pid <= 0 || pid >= 256 || !pcbs[pid].active) return;

    // 1. Копируем метаданные упавшего процесса во временный буфер
    ProcessControlBlock meta = pcbs[pid];

    uart_puts("\n[WATCHDOG] Emergency recovery initiated for PID: "); uart_putdec(pid);
    uart_puts(" ("); uart_puts(meta.name); uart_puts(")\n");

    for (int i = 1; i < 256; i++) {
        if (pcbs[i].active && pcbs[i].waiting_for == pid) { 
            pcbs[i].waiting_for = 0;
            seL4_SetMR(0, 0);
            seL4_Send(pcbs[i].reply_cap, seL4_MessageInfo_new(0, 0, 0, 1));
            seL4_CNode_Delete(root_cnode, pcbs[i].reply_cap, seL4_WordBits);
            
            pcbs[i].reply_cap = 0;
        }
    }
    
    if (pcbs[pid].tcb) {
        seL4_TCB_Suspend(pcbs[pid].tcb);
        if (pcbs[pid].irq_ntfn != 0) {
            seL4_TCB_UnbindNotification(pcbs[pid].tcb);
        }
    }

    for (int i = 0; i < pcbs[pid].cap_tracker.count; i++) {
        bool is_thread = (strncmp(pcbs[pid].name, "shell_thread", 12) == 0);
        if (is_thread && pcbs[pid].cap_tracker.caps[i] == pcbs[pid].vspace) continue;
        
        seL4_CPtr cap_to_free = pcbs[pid].cap_tracker.caps[i];

        // 1. Сначала ОТЗЫВАЕМ (Revoke) все дочерние объекты в ядре, чтобы освободить RAM
        seL4_CNode_Revoke(root_cnode, cap_to_free, seL4_WordBits);
        
        // 2. Затем УДАЛЯЕМ (Delete) сам слот в CNode
        seL4_CNode_Delete(root_cnode, cap_to_free, seL4_WordBits);
        
        // ДОБАВЛЕНО: Возвращаем слот обратно в пул свободных слотов!
        alloc.free(cap_to_free);
    }
    
    pcbs[pid].cap_tracker.count = 0;

    // Если процесс использовал SHM, уничтожаем копии Capabilities, 
    // чтобы вернуть слоты в аллокатор и отмапить память!
    if (meta.has_shm) {
        for (int i = 0; i < 4; i++) {
            if (meta.shm_copies[i] != 0) {
                seL4_CNode_Delete(root_cnode, meta.shm_copies[i], seL4_WordBits);
                alloc.free(meta.shm_copies[i]);
            }
        }
    }

    if (meta.is_driver > 0 || strcmp(meta.name, "shell") == 0) {
        uart_puts("[WATCHDOG] Respawning critical system component...\n");
        
        int new_pid = spawn_process(meta.name, nullptr, 0, ep, med_ep, alloc, root_cnode, root_vspace, normal_untyped, shm_frame,
                                    meta.is_driver, console_ep, timer_ep, blk_ep, meta.irq_ntfn, meta.irq_handler, meta.hw_frame,
                                    nullptr, meta.net_cmd_recv_ep, meta.net_cmd_send_ep);
        
        if (new_pid > 0) {
            uart_puts("[WATCHDOG] Service restored successfully. New PID: "); uart_putdec(new_pid); uart_puts("\n");
        } else {
            uart_puts("[WATCHDOG] CRITICAL ERROR: Failed to respawn component!\n");
        }
    } else {
        uart_puts("[WATCHDOG] Non-critical user process terminated permanently.\n");
    }

    pcbs[pid].active = false;
}

int main(int argc, char *argv[]) {
    seL4_BootInfo *info = platsupport_get_bootinfo();
    if (!info) while (1);

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

    reader_reply_slot = alloc.alloc_slot();
    sleeper_reply_slot = alloc.alloc_slot();

    seL4_CPtr pmd = alloc.alloc_slot();
    seL4_CPtr pt = alloc.alloc_slot();
    seL4_Untyped_Retype(normal_untyped, seL4_ARM_PageDirectoryObject, 0, root_cnode, 0, 0, pmd, 1);
    seL4_Untyped_Retype(normal_untyped, seL4_ARM_PageTableObject, 0, root_cnode, 0, 0, pt, 1);

    // ИСПРАВЛЕНИЕ: Создаем и мапим дополнительную таблицу страниц (Page Table)
    // для временного окна IPC, которое было перемещено на новый адрес.
    seL4_CPtr pt_ipc_temp = alloc.alloc_slot();
    seL4_Untyped_Retype(normal_untyped, seL4_ARM_PageTableObject, 0, root_cnode, 0, 0, pt_ipc_temp, 1);

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

    // Мапим таблицу для временного окна IPC. Адрес должен совпадать с global_ipc_temp_vaddr.
    // Одна таблица покрывает 2MB, чего достаточно для 512 процессов.
    seL4_ARM_PageTable_Map(pt_ipc_temp, root_vspace, 0x200800000ULL, seL4_ARM_Default_VMAttributes);

    uintptr_t rtc_vaddr = 0x200002000ULL;
    seL4_ARM_Page_Map(rtc_frame, root_vspace, rtc_vaddr, seL4_AllRights, seL4_ARM_Default_VMAttributes);

    uart_init((void*)uart_vaddr);
    timer_init((void*)rtc_vaddr);

    uart_puts("\n=== Psych Ward OS: TRUE MICROKERNEL EDITION ===\n");

    seL4_CPtr ep = alloc.alloc_slot();
    seL4_CPtr med_ep = alloc.alloc_slot();
    seL4_Untyped_Retype(normal_untyped, seL4_EndpointObject, 0, root_cnode, 0, 0, ep, 1);
    seL4_Untyped_Retype(normal_untyped, seL4_EndpointObject, 0, root_cnode, 0, 0, med_ep, 1);

    // --- ПРАВИЛЬНОЕ ВЫДЕЛЕНИЕ SHM (Из обычной ОЗУ, а не из Device Memory) ---
    for (int i = 0; i < 4; i++) {
        shm_frames[i] = alloc.alloc_slot();        
        // ИСПРАВЛЕНО: Передаем 0, 0 вместо root_cnode для индексов!
        seL4_Error err = seL4_Untyped_Retype(normal_untyped, seL4_ARM_SmallPageObject, 0,
                                             root_cnode, 0, 0, shm_frames[i], 1);
        if (err != seL4_NoError) {
            uart_puts("[ROOT] FATAL: Failed to allocate normal RAM for SHM!\n");
            while(1);
        }
        // Мапим эти физические фреймы в виртуальную память Rootserver'а
        seL4_ARM_Page_Map(shm_frames[i], root_vspace, (uintptr_t)rootserver_shm_base + (i * 4096),
                          seL4_AllRights, seL4_ARM_Default_VMAttributes);
    }

    seL4_CPtr console_ep = alloc.alloc_slot();
    seL4_CPtr timer_ep = alloc.alloc_slot();
    seL4_CPtr blk_ep = alloc.alloc_slot();
    seL4_CPtr net_cmd_ep = alloc.alloc_slot();
    seL4_CPtr net_cmd_recv_ep = alloc.alloc_slot();
    seL4_CPtr net_cmd_send_ep = alloc.alloc_slot();
    seL4_Untyped_Retype(normal_untyped, seL4_EndpointObject, 0, root_cnode, 0, 0, console_ep, 1);
    seL4_Untyped_Retype(normal_untyped, seL4_EndpointObject, 0, root_cnode, 0, 0, timer_ep, 1);
    seL4_Untyped_Retype(normal_untyped, seL4_EndpointObject, 0, root_cnode, 0, 0, blk_ep, 1);
    seL4_Untyped_Retype(normal_untyped, seL4_EndpointObject, 0, root_cnode, 0, 0, net_cmd_ep, 1);
    seL4_CNode_Copy(root_cnode, net_cmd_recv_ep, seL4_WordBits,
                    root_cnode, net_cmd_ep, seL4_WordBits, seL4_CanRead);
    seL4_CNode_Copy(root_cnode, net_cmd_send_ep, seL4_WordBits,
                    root_cnode, net_cmd_ep, seL4_WordBits, seL4_CapRights_new(0, 1, 0, 1)); // Write + Grant

    seL4_CPtr timer_ntfn = alloc.alloc_slot();
    seL4_CPtr badged_timer_ntfn = alloc.alloc_slot();
    seL4_CPtr timer_irq_handler = alloc.alloc_slot();
    seL4_Untyped_Retype(normal_untyped, seL4_NotificationObject, 0, root_cnode, 0, 0, timer_ntfn, 1);
    seL4_CNode_Mint(root_cnode, badged_timer_ntfn, seL4_WordBits, root_cnode, timer_ntfn, seL4_WordBits, seL4_AllRights, 2); 
    seL4_IRQControl_Get(seL4_CapIRQControl, 34, root_cnode, timer_irq_handler, seL4_WordBits);
    seL4_IRQHandler_SetNotification(timer_irq_handler, badged_timer_ntfn);
    seL4_IRQHandler_Ack(timer_irq_handler);

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
    if (spawn_process("uart_driver", nullptr, 0, ep, med_ep, alloc, root_cnode, root_vspace, normal_untyped, shm_frames[0], 
                      1, console_ep, timer_ep, 0, uart_ntfn, uart_irq_handler, uart_frame) < 0) {
        uart_puts("PANIC: UART Driver failed to load!\n"); while(1);
    }

    // Запускаем Драйвер Таймера (is_driver = 2)
    if (spawn_process("timer_driver", nullptr, 0, ep, med_ep, alloc, root_cnode, root_vspace, normal_untyped, shm_frames[0], 
                      2, console_ep, timer_ep, 0, timer_ntfn, timer_irq_handler, rtc_frame) < 0) {
        uart_puts("PANIC: Timer Driver failed to load!\n"); while(1);
    }

    // Запускаем Драйвер Диска и ФС (is_driver = 3)
    if (spawn_process("blk_driver", nullptr, 0, ep, med_ep, alloc, root_cnode, root_vspace, normal_untyped, shm_frames[0], 
                      3, console_ep, timer_ep, blk_ep, 0, 0, virtio_frames[0]) < 0) {
        uart_puts("PANIC: Block Driver failed to load!\n"); while(1);
    }

    if (spawn_process("net_driver", nullptr, 0, ep, med_ep, alloc, root_cnode, root_vspace, normal_untyped, shm_frames[0], 
                      4, console_ep, timer_ep, 0, 0, 0, virtio_frames[0], nullptr,
                      net_cmd_recv_ep, 0) < 0) {
        uart_puts("PANIC: Net Driver failed to load!\n"); while(1);
    }

    // Запускаем Оболочку (is_driver = 0)
    if (spawn_process("shell", nullptr, 0, ep, med_ep, alloc, root_cnode, root_vspace, normal_untyped, shm_frames[0], 
                      0, console_ep, timer_ep, blk_ep, 0, 0, 0, nullptr,
                      0, net_cmd_send_ep) < 0) {
        uart_puts("PANIC: Shell failed to load!\n"); while(1);
    }

    uart_puts("Quadruple Sandboxes Spawned! Kernel is purified and serving IPC...\n");

    // --- ЕДИНЫЙ ЦИКЛ ЯДРА ---
    while (1) {
        seL4_Word sender_badge = 0;
    // Ожидаем прерывание, сообщение IPC или fault
    seL4_MessageInfo_t recv_info = seL4_Recv(ep, &sender_badge);

    seL4_Word sender_pid = 0;

    if (sender_badge != 0) {
        // Извлекаем чистый PID процесса/потока из младших 16 бит
        seL4_Word actual_pid = sender_badge & 0xFFFF;
        
        // Проверяем, что это не ядро (0) и процесс зарегистрирован
        if (actual_pid > 0 && actual_pid < 256 && pcbs[actual_pid].active) {
            sender_pid = actual_pid;
        } else {
            // Защита от взлома/коррупции: игнорируем неопознанный бейдж
            continue; 
        }
    }

        seL4_Word label = seL4_MessageInfo_get_label(recv_info);
        
        if (label == seL4_Fault_VMFault) {
            seL4_Word pc = seL4_GetMR(0);
            seL4_Word addr = seL4_GetMR(1);
            
            if (addr >= 0x510000 && addr < 0x600000 && sender_pid != 0) {
                uart_puts("\n[KERNEL PAGER] Page Fault at 0x"); uart_puthex(addr);
                uart_puts(" for PID "); uart_putdec(sender_pid);
                uart_puts(" -> Allocating Frame On-The-Fly!\n");
                
                // ИСПРАВЛЕНО: Используем alloc_and_track_cap, чтобы избежать утечки памяти при смерти процесса
                seL4_CPtr frame = alloc_and_track_cap(alloc, pcbs[sender_pid]);
                check_err(seL4_Untyped_Retype(normal_untyped, seL4_ARM_SmallPageObject, 0, root_cnode, 0, 0, frame, 1), "Pager Frame");
                
                uintptr_t page_aligned = addr & ~0xFFFULL;
                check_err(seL4_ARM_Page_Map(frame, pcbs[sender_pid].vspace, page_aligned, seL4_AllRights, seL4_ARM_Default_VMAttributes), "Pager Map");
                
                seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 0));
                continue;
            } else {
                uart_puts("\nFATAL FAULT! PID: "); uart_putdec(sender_pid);
                uart_puts("\nPC: "); uart_puthex(pc);
                uart_puts("\nMem Addr: "); uart_puthex(addr);
                uart_puts("\n");

                // ДОБАВЛЕНО: Предохранитель от бага неинициализированного TLS/IPC Buffer
                if (addr == 0x12) {
                    uart_puts("FATAL: TLS/IPC Buffer not initialized (Data Abort at 0x12).\n");
                    uart_puts("Halting Watchdog respawn to prevent memory/slot leaks.\n");
                    while(1); 
                }

                if (sender_pid != 0) {
                    generic_recover_process(sender_pid, ep, med_ep, alloc, root_cnode, root_vspace, normal_untyped, shm_frames[0], console_ep, timer_ep, blk_ep);
                }
                continue;
            }
        }

        else if (label == seL4_Fault_CapFault) {
            seL4_Word pc = seL4_GetMR(0);
            seL4_Word cap = seL4_GetMR(1);
            
            uart_puts("\n[KERNEL PANIC] CapFault! PID "); uart_putdec(sender_pid);
            uart_puts(" attempted to invoke invalid cap #"); uart_putdec(cap);
            uart_puts(" at PC: "); uart_puthex(pc); uart_puts("\n");
            
            if (sender_pid != 0) {
                generic_recover_process(sender_pid, ep, med_ep, alloc, root_cnode, root_vspace, normal_untyped, shm_frames[0], console_ep, timer_ep, blk_ep);
            }
            continue; // КРИТИЧНО: Возвращаемся в начало цикла, НЕ делая Reply!
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
                    
                    continue; 
                } else {
                    seL4_SetMR(0, 0); // Процесс уже умер, сразу возвращаем успех
                    seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                }
                break;
            }

            case SYS_DOCTOR: {
                char *shm = rootserver_shm_base; 
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

            case SYS_CLONE: {
                // ИСПРАВЛЕНО: Аргументы потока (func, arg0, arg1, arg2) передаются через регистры, а не SHM
                seL4_Word entry_point = seL4_GetMR(1);
                seL4_Word arg0 = seL4_GetMR(2);
                seL4_Word arg1 = seL4_GetMR(3);
                seL4_Word arg2 = seL4_GetMR(4);

                int new_pid = -1;
                for (int i = 1; i < 256; i++) {
                    if (!pcbs[i].active) {
                        new_pid = i;
                        break;
                    }
                }

                if (new_pid == -1) {
                    seL4_SetMR(0, (seL4_Word)-1); 
                    seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                    break;
                }

                seL4_CPtr new_tcb = alloc.alloc_slot(); 
                seL4_Untyped_Retype(normal_untyped, seL4_TCBObject, seL4_TCBBits, root_cnode, 0, 0, new_tcb, 1);

                // --- ГЕНЕРАЦИЯ УНИКАЛЬНОГО БЕЙДЖА ПОТОКА ---
                seL4_Word thread_badge = (sender_pid << 16) | new_pid; 
                seL4_CPtr thread_badged_ep = alloc.alloc_slot();
                seL4_CNode_Mint(root_cnode, thread_badged_ep, seL4_WordBits,
                                root_cnode, ep, seL4_WordBits, seL4_AllRights, thread_badge);

                seL4_Word local_thread_fault_ep = 100 + new_pid; 
                // ИСПРАВЛЕНО: Превентивно удаляем старый Capability из слота, чтобы избежать ошибки
                // "Destination not empty" при повторном использовании PID потока.
                seL4_CNode_Delete(pcbs[sender_pid].cspace, local_thread_fault_ep, 8);
                seL4_CNode_Copy(pcbs[sender_pid].cspace, local_thread_fault_ep, 8,
                                root_cnode, thread_badged_ep, seL4_WordBits, seL4_AllRights);

                seL4_CPtr ipc_frame = alloc.alloc_slot();
                seL4_Untyped_Retype(normal_untyped, seL4_ARM_SmallPageObject, 0, root_cnode, 0, 0, ipc_frame, 1);
                
                static uintptr_t clone_temp_vaddr = 0x2001C0000ULL; 
                uintptr_t temp_window = clone_temp_vaddr;
                clone_temp_vaddr += 0x1000; // Сдвигаем на 4КБ для следующего потока!
                if (clone_temp_vaddr >= 0x2001F0000ULL) clone_temp_vaddr = 0x2001C0000ULL; // Сброс
                
                seL4_ARM_Page_Map(ipc_frame, root_vspace, temp_window, seL4_AllRights, seL4_ARM_Default_VMAttributes);
                memset((void*)temp_window, 0, 4096);
                seL4_IPCBuffer *child_ipc_ptr = (seL4_IPCBuffer *)(temp_window + 2048);
                
                child_ipc_ptr->msg[BOOT_ROOT_EP] = local_thread_fault_ep;
                child_ipc_ptr->msg[BOOT_CONSOLE_EP] = 1; // local_console_ep
                child_ipc_ptr->msg[BOOT_TIMER_EP] = 2; // local_timer_ep
                child_ipc_ptr->msg[BOOT_NET_EP] = 3; // local_net_send_ep
                
                seL4_ARM_Page_Unmap(ipc_frame);

                // ИСПРАВЛЕНО: База IPC-буферов потоков смещена на 0x700000 во избежание коллизии со стеками
                uintptr_t thread_ipc_vaddr = 0x700000 + (new_pid * 4096); 
                check_err(seL4_ARM_Page_Map(ipc_frame, pcbs[sender_pid].vspace, thread_ipc_vaddr, seL4_AllRights, seL4_ARM_Default_VMAttributes), "Thread IPC Page");

                // --- Конфигурация TCB ---
                seL4_Word cspace_guard = seL4_CNode_CapData_new(0, seL4_WordBits - 8).words[0];

                seL4_TCB_Configure(new_tcb, 
                                   local_thread_fault_ep, 
                                   pcbs[sender_pid].cspace, cspace_guard, 
                                   pcbs[sender_pid].vspace, seL4_NilData, 
                                   thread_ipc_vaddr + 2048, ipc_frame);       
                
                seL4_TCB_SetPriority(new_tcb, seL4_CapInitThreadTCB, 254);

                // --- Выделение стека ---
                seL4_Word stack_vaddr = 0x540000 + (new_pid * 0x2000); 
                
                seL4_CPtr stack_frame1 = alloc.alloc_slot(); 
                seL4_Untyped_Retype(normal_untyped, seL4_ARM_SmallPageObject, 0, root_cnode, 0, 0, stack_frame1, 1);
                check_err(seL4_ARM_Page_Map(stack_frame1, pcbs[sender_pid].vspace, stack_vaddr, seL4_AllRights, seL4_ARM_Default_VMAttributes), "Thread Stack Page 1");
                
                seL4_CPtr stack_frame2 = alloc.alloc_slot(); 
                seL4_Untyped_Retype(normal_untyped, seL4_ARM_SmallPageObject, 0, root_cnode, 0, 0, stack_frame2, 1);
                check_err(seL4_ARM_Page_Map(stack_frame2, pcbs[sender_pid].vspace, stack_vaddr + 0x1000, seL4_AllRights, seL4_ARM_Default_VMAttributes), "Thread Stack Page 2");

                seL4_Word stack_top = (stack_vaddr + 0x2000) & ~0xF; 

                // --- Запуск контекста ---
                seL4_UserContext context = {0};
                context.pc = entry_point;   
                context.sp = stack_top;     
                // Передаем аргументы в новый поток через регистры x0, x1, x2
                context.x0 = arg0;
                context.x1 = arg1;
                context.x2 = arg2;

                context.tpidr_el0 = thread_ipc_vaddr + 3072;
                context.tpidrro_el0 = thread_ipc_vaddr + 3072;
                
                seL4_TCB_WriteRegisters(new_tcb, false, 0, sizeof(context) / sizeof(seL4_Word), &context);
                seL4_TCB_SetTLSBase(new_tcb, thread_ipc_vaddr + 3072);

                pcbs[new_pid].active = true;
                pcbs[new_pid].tcb = new_tcb;
                pcbs[new_pid].vspace = pcbs[sender_pid].vspace;
                pcbs[new_pid].cspace = pcbs[sender_pid].cspace;
                pcbs[new_pid].badged_ep = thread_badged_ep;
                strncpy(pcbs[new_pid].name, "shell_thread", 31);
                
                pcbs[new_pid].thread_ipc_frame = ipc_frame;
                pcbs[new_pid].thread_stack_frame1 = stack_frame1;
                pcbs[new_pid].thread_stack_frame2 = stack_frame2;

                seL4_TCB_Resume(new_tcb);

                seL4_SetMR(0, new_pid);
                seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                break;
            }

            case 105: { // SYS_THREAD_EXIT
                int thread_pid = sender_badge & 0xFFFF; // Младшие 16 бит - это new_pid потока
                
                if (pcbs[thread_pid].active && strncmp(pcbs[thread_pid].name, "shell_thread", 12) == 0) {
                    
                    seL4_TCB_Suspend(pcbs[thread_pid].tcb);
                    
                    if (pcbs[thread_pid].thread_stack_frame1) {
                        seL4_ARM_Page_Unmap(pcbs[thread_pid].thread_stack_frame1);
                        seL4_CNode_Revoke(root_cnode, pcbs[thread_pid].thread_stack_frame1, seL4_WordBits);
                        seL4_CNode_Delete(root_cnode, pcbs[thread_pid].thread_stack_frame1, seL4_WordBits);
                    }
                    if (pcbs[thread_pid].thread_stack_frame2) {
                        seL4_ARM_Page_Unmap(pcbs[thread_pid].thread_stack_frame2);
                        seL4_CNode_Revoke(root_cnode, pcbs[thread_pid].thread_stack_frame2, seL4_WordBits);
                        seL4_CNode_Delete(root_cnode, pcbs[thread_pid].thread_stack_frame2, seL4_WordBits);
                    }
                    if (pcbs[thread_pid].thread_ipc_frame) {
                        seL4_ARM_Page_Unmap(pcbs[thread_pid].thread_ipc_frame);
                        seL4_CNode_Revoke(root_cnode, pcbs[thread_pid].thread_ipc_frame, seL4_WordBits);
                        seL4_CNode_Delete(root_cnode, pcbs[thread_pid].thread_ipc_frame, seL4_WordBits);
                    }
                    
                    seL4_CNode_Revoke(root_cnode, pcbs[thread_pid].tcb, seL4_WordBits);
                    seL4_CNode_Delete(root_cnode, pcbs[thread_pid].tcb, seL4_WordBits);
                    
                    seL4_CNode_Delete(root_cnode, pcbs[thread_pid].badged_ep, seL4_WordBits); 

                    pcbs[thread_pid].active = false;
                }
                break;
            }

            case SYS_EXEC: {
                char app_name_and_args[64] = {0};
                
                // Распаковываем 64 байта (8 регистров) из MR1 - MR8
                uint64_t* name_ptr = (uint64_t*)app_name_and_args;
                for (int i = 0; i < 8; i++) {
                    name_ptr[i] = seL4_GetMR(i + 1);
                }
                app_name_and_args[63] = '\0'; // Защита

                // Отделяем имя приложения от аргументов
                char* args = app_name_and_args;
                while (*args && *args != ' ') args++;
                if (*args == ' ') {
                    *args = '\0';
                    args++;
                } else {
                    args = (char*)""; // No args
                }
                
                static char elf_staging_buffer[1024 * 1024];

                uart_puts("[ROOT] Fetching ELF from disk: ");
                uart_puts(app_name_and_args);
                uart_puts("...\n");
                
                int elf_size = load_elf_from_disk(blk_ep, app_name_and_args, elf_staging_buffer);
                int new_pid = -1;

                if (elf_size > 0) {
                    uart_puts("[ROOT] ELF loaded successfully! Spawning...\n");
                    elf_t elf;
                    if (elf_newFile(elf_staging_buffer, elf_size, &elf) == 0) {
                        new_pid = spawn_process(app_name_and_args, elf_staging_buffer, elf_size, ep, med_ep, alloc, root_cnode, root_vspace, 
                                                normal_untyped, shm_frames[0], 254, console_ep, timer_ep, blk_ep,
                                                0, 0, 0, args, 0, net_cmd_send_ep);
                    } else {
                        new_pid = -2; // Invalid ELF
                    }
                }
                
                seL4_SetMR(0, new_pid);
                seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                break;
            }

            case SYS_PS: {
                char *shm = rootserver_shm_base;
                int offset = 0;
                
                strcpy(shm, "  PID STATUS    NAME\n");
                offset = strlen(shm);
                
                strcpy(shm + offset, "    0 [RUNNING] rootserver\n");
                offset = strlen(shm);
                
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
                
                if (target_pid == 0) {
                    uart_puts("\n[KERNEL PANIC] Attempted to kill Rootserver!\n");
                    seL4_SetMR(0, (seL4_Word)-1);
                } 
                else if (target_pid > 0 && target_pid < 256 && pcbs[target_pid].active) {
                    if (pcbs[target_pid].is_driver > 0 || strcmp(pcbs[target_pid].name, "shell") == 0) {
                        uart_puts("\n[KERNEL] Critical process killed manually. Triggering recovery...\n");                        
                        generic_recover_process(target_pid, ep, med_ep, alloc, root_cnode, root_vspace, normal_untyped, shm_frames[0], console_ep, timer_ep, blk_ep);
                    } else {
                    seL4_TCB_Suspend(pcbs[target_pid].tcb);
                    // Отвязываем прерывание, только если оно было привязано (у драйверов)
                    if (pcbs[target_pid].irq_ntfn != 0) {
                        seL4_TCB_UnbindNotification(pcbs[target_pid].tcb);
                    }
                        pcbs[target_pid].active = false;
                        seL4_CNode_Delete(root_cnode, pcbs[target_pid].badged_ep, seL4_WordBits);
                        seL4_CNode_Delete(root_cnode, pcbs[target_pid].tcb, seL4_WordBits);
                        if (pcbs[target_pid].vspace != root_vspace) {
                            seL4_CNode_Delete(root_cnode, pcbs[target_pid].vspace, seL4_WordBits);
                        }
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
                    for (int i = 1; i < 256; i++) {
                        if (pcbs[i].active && pcbs[i].waiting_for == sender_pid) {
                            pcbs[i].waiting_for = 0;
                            seL4_SetMR(0, 0);
                            seL4_Send(pcbs[i].reply_cap, seL4_MessageInfo_new(0, 0, 0, 1));
                            seL4_CNode_Delete(root_cnode, pcbs[i].reply_cap, seL4_WordBits);
                        }
                    }
                    seL4_TCB_Suspend(pcbs[sender_pid].tcb);
                    if (pcbs[sender_pid].irq_ntfn != 0) {
                        seL4_TCB_UnbindNotification(pcbs[sender_pid].tcb);
                    }
                    pcbs[sender_pid].active = false;
                    seL4_CNode_Delete(root_cnode, pcbs[sender_pid].badged_ep, seL4_WordBits);
                    seL4_CNode_Delete(root_cnode, pcbs[sender_pid].tcb, seL4_WordBits);
                    if (pcbs[sender_pid].vspace != root_vspace) {
                        seL4_CNode_Delete(root_cnode, pcbs[sender_pid].vspace, seL4_WordBits);
                    }
                }
                continue; 
            }

            case SYS_GETPID: {
                seL4_SetMR(0, sender_pid); 
                seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                break;
            }

            case SYS_SHM_GET: {
                int pid = sender_badge;
                if (pid <= 0 || pid >= 256 || !pcbs[pid].active) {
                    seL4_SetMR(0, 0);
                    seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                    break;
                }

                uintptr_t vaddr = pcbs[pid].vmap_bump_pointer;
                pcbs[pid].vmap_bump_pointer += 0x4000; // Сдвигаем курсор на 16 КБ

                // Отмечаем, что этот процесс взял SHM
                pcbs[pid].has_shm = true;
                bool success = true;

                for (int i = 0; i < 4; i++) {
                    seL4_CPtr frame_copy = alloc.alloc_slot();
                    if (frame_copy == 0) { success = false; pcbs[pid].shm_copies[i] = 0; }
                    else { pcbs[pid].shm_copies[i] = frame_copy; }

                    if (!success) break;

                    // 1. КОПИРУЕМ Capability (Обязательно для seL4)
                    seL4_Error err = seL4_CNode_Copy(
                        root_cnode, frame_copy, seL4_WordBits,
                        root_cnode, shm_frames[i], seL4_WordBits,
                        seL4_AllRights
                    );

                    if (err != seL4_NoError) { success = false; break; }

                    // ВОТ ОНО! Делегируем маппинг нашему On-Demand алгоритму
                    if (!map_frame_robust(alloc, pcbs[pid], frame_copy, pcbs[pid].vspace, vaddr + (i * 0x1000), normal_untyped, root_cnode)) {
                        success = false; break;
                    }
                }

                if (!success) {
                    uart_puts("[ROOT] FATAL: Failed to dynamically map 16KB SHM!\n");
                    for (int i = 0; i < 4; i++) {
                        if (pcbs[pid].shm_copies[i] != 0) {
                            seL4_CNode_Delete(root_cnode, pcbs[pid].shm_copies[i], seL4_WordBits);
                            alloc.free(pcbs[pid].shm_copies[i]);
                        }
                    }
                    pcbs[pid].has_shm = false;
                    pcbs[pid].vmap_bump_pointer -= 0x4000;
                    seL4_SetMR(0, 0);
                    seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                    break;
                }

                // Успех! Возвращаем адреса драйверу
                seL4_ARM_Page_GetAddress_t res = seL4_ARM_Page_GetAddress(shm_frames[0]);
                seL4_SetMR(0, vaddr);
                seL4_SetMR(1, res.paddr);
                seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 2));
                break;
            }

            case SYS_RECOVER: {
                char driver_name[32] = {0}; // Заполняем нулями
                
                // Читаем 32 байта (4 регистра по 8 байт) напрямую из сообщения ядра!
                // MR0 занят номером системного вызова (117)
                uint64_t* name_ptr = (uint64_t*)driver_name;
                name_ptr[0] = seL4_GetMR(1);
                name_ptr[1] = seL4_GetMR(2);
                name_ptr[2] = seL4_GetMR(3);
                name_ptr[3] = seL4_GetMR(4);
                
                driver_name[31] = '\0'; // Гарантируем нуль-терминатор для безопасности

                int target_pid = -1;
                for (int i = 1; i < 256; i++) {
                    if (pcbs[i].active && strcmp(pcbs[i].name, driver_name) == 0) {
                        target_pid = i;
                        break;
                    }
                }

                if (target_pid != -1) {
                    generic_recover_process(target_pid, ep, med_ep, alloc, root_cnode, root_vspace, normal_untyped, shm_frames[0], console_ep, timer_ep, blk_ep);
                    seL4_SetMR(0, 0);
                } else {
                    seL4_SetMR(0, (seL4_Word)-1);
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