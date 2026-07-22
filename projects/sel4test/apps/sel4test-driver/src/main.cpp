#include "h/common.h"
#include "h/allocator.h"
#include "h/uart.h"
#include "h/hw_timer.h"
#include "h/platform.h"

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
    volatile uint32_t *uart_io  = (volatile uint32_t*)(PLAT_UART_VADDR + AUX_MU_IO_OFFSET);
    volatile uint32_t *uart_lsr = (volatile uint32_t*)(PLAT_UART_VADDR + AUX_MU_LSR_OFFSET);
    while (!((*uart_lsr) & AUX_MU_LSR_TX_EMPTY)); *uart_io = c;
}

// ВРЕМЕННАЯ диагностика для hw bring-up (см. RPI4_ENABLE_* в main()).
// Печатает через отладочную консоль ядра (mini-UART, работает независимо от
// состояния нашего PL011), так что видна даже если PL011 завис/не отвечает.
static void debug_puthex32(const char *label, uint32_t val) {
    seL4_DebugPutString((char*)label);
    seL4_DebugPutString((char*)"0x");
    for (int nib = 7; nib >= 0; nib--) {
        seL4_DebugPutChar("0123456789abcdef"[(val >> (nib * 4)) & 0xF]);
    }
    seL4_DebugPutString((char*)"\n");
}



// --- В начале файла или внутри spawn_process ---
// Базовые адреса для временного маппинга (сдвигаются атомарно)
static uintptr_t global_elf_temp_vaddr = 0x200100000ULL;
static uintptr_t global_ipc_temp_vaddr = 0x200800000ULL;

// ИСПРАВЛЕНО (см. память проекта — краш на "wifi restart", 2026-07-20):
// адрес 0x502000 попадал ВНУТРЬ статического массива pcbs[256] (реальный
// адрес которого определяет линкер, не мы) — sizeof(ProcessControlBlock)
// выросло настолько (крипто/join-код Милстоуна 4.4), что диапазон SHM
// (0x502000..0x506000, те же 4 страницы, что мапятся ниже) целиком
// перекрывал pcbs[3..9], и запись SSID/пароля в SHM (см. WIFI_SHM_*_OFFSET
// в wifi_driver.cpp) физически совпадала с pcbs[6].cap_tracker.caps[54..66]
// — отсюда "капабилити" со значением байт пароля/SSID и краш рутсервера при
// попытке их Revoke/Delete. Перенесено в тот же "высокий" диапазон адресов
// (0x2000000000+), что уже используют global_elf_temp_vaddr/
// global_ipc_temp_vaddr ниже — эти адреса заведомо вне статического образа
// rootserver'а (vaddr=[400000..7b8fff] по логу загрузки), так что коллизия
// с pcbs[]/любыми другими статическими данными невозможна в принципе, а не
// просто "пока не встретилась".
static char* rootserver_shm_base = (char*)0x200A00000ULL;
static seL4_CPtr shm_frames[4]; // Массив Capability для 4-х страниц SHM

// rootserver_shm_base мапится КЭШИРУЕМО (см. цикл маппинга в main()), а все
// остальные процессы (shell/blk_driver/net_driver/...) видят ЭТУ ЖЕ
// физическую память некэшируемо (map_frame_robust()). Без явного cache
// maintenance рутсервер рискует: (а) записать данные, которые останутся в
// dirty-кэше и не долетят до RAM к моменту, когда некэшируемый читатель их
// ждёт (например, SYS_PS — таблица процессов для `ps`), или (б) прочитать
// устаревшую закэшированную копию вместо того, что кто-то другой только что
// записал некэшируемо (например, load_elf_from_disk — ответ blk_driver).
// Вызывать после записи (перед тем, как другой процесс должен её увидеть)
// и/или перед чтением (после того, как другой процесс мог что-то записать).
static void flush_rootserver_shm() {
    for (int i = 0; i < 4; i++) {
        seL4_ARM_Page_CleanInvalidate_Data(shm_frames[i], 0, 4096);
    }
}

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
    if (idx == (size_t)-1) {
        // ВРЕМЕННАЯ диагностика для hw bring-up на RPi4 — см. RPI4_ENABLE_* в main().
        // seL4_DebugPutString идет через отладочную консоль ядра напрямую,
        // так что работает даже если наш собственный UART еще не замаплен.
        seL4_DebugPutString((char*)"[BRINGUP] alloc_device_frame: NOT FOUND in any untyped region, target_paddr=0x");
        for (int nib = 15; nib >= 0; nib--) {
            char c = "0123456789abcdef"[(target_paddr >> (nib * 4)) & 0xF];
            seL4_DebugPutChar(c);
        }
        seL4_DebugPutString((char*)"\n");
        while(1); // не нашли — фатально
    }

    // Диагностика для hw bring-up на RPi4 (см. LOG_BRINGUP в platform.h) —
    // печатает, В КАКОЙ untyped-регион реально попал target_paddr: если
    // isDevice=0, значит это обычная RAM, а не настоящий MMIO device-фрейм —
    // тогда чтение/запись регистров будут самосогласованы (что писали, то и
    // читаем), но к реальному железу отношения иметь не будут.
    if (LOG_BRINGUP) {
        debug_puthex32("[BRINGUP]   matched region.paddr = ", (uint32_t)info->untypedList[idx].paddr);
        debug_puthex32("[BRINGUP]   matched region.sizeBits = ", (uint32_t)info->untypedList[idx].sizeBits);
        debug_puthex32("[BRINGUP]   matched region.isDevice = ", (uint32_t)info->untypedList[idx].isDevice);
    }

    if (untyped_watermarks[idx] == 0)
        untyped_watermarks[idx] = info->untypedList[idx].paddr;

    // Этот аллокатор — чистый bump-allocator по возрастанию: retype всегда
    // отдаёт СЛЕДУЮЩИЙ физический фрейм от текущего watermark'а, независимо
    // от того, какой target_paddr запрошен. Если target_paddr уже ПОЗАДИ
    // watermark'а (т.е. какой-то более ранний вызов alloc_device_frame() для
    // ЭТОГО ЖЕ untyped-региона уже увёл watermark дальше вперёд), функция
    // молча вернула бы фрейм по совершенно другому физическому адресу — не
    // тому устройству, которое просили, а какому-то соседнему/уже занятому.
    // Ровно так и была найдена эта проверка: PLAT_WIFI_SDIO_PADDR (0xfe300000)
    // allocировался ПОСЛЕ PLAT_EMMC_PADDR (0xfe340000) в том же untyped —
    // wifi_driver получил чужой фрейм (фактически чуть выше EMMC2) и его
    // SDIO-транзакции разбудили/сломали реальный EMMC2, а не WiFi-чип (см.
    // ROADMAP.md, Милстоун 4.1 — упавший CMD0 в blk_driver ровно в тот момент,
    // когда включили RPI4_ENABLE_WIFI). Падаем громко здесь же, а не отдаём
    // тихо неверный фрейм — единственный порядок вызовов, который надёжен,
    // это строго по возрастанию target_paddr в пределах одного untyped-региона.
    if (target_paddr < untyped_watermarks[idx]) {
        seL4_DebugPutString((char*)"[BRINGUP] alloc_device_frame: target_paddr BEHIND watermark (out-of-order call for this untyped region) target_paddr=0x");
        for (int nib = 15; nib >= 0; nib--) seL4_DebugPutChar("0123456789abcdef"[(target_paddr >> (nib * 4)) & 0xF]);
        seL4_DebugPutString((char*)" watermark=0x");
        for (int nib = 15; nib >= 0; nib--) seL4_DebugPutChar("0123456789abcdef"[(untyped_watermarks[idx] >> (nib * 4)) & 0xF]);
        seL4_DebugPutString((char*)"\n");
        while(1); // отдать неверный фрейм намного хуже, чем зависнуть здесь
    }

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

// 256 хватает с запасом: wifi_driver со своими статическими буферами прошивки/NVRAM
// (~708KB) сам по себе требует ~180-200 отслеживаемых ELF-страничных фреймов.
#define MAX_TRACKED_CAPS 256

struct CapTracker {
    seL4_CPtr caps[MAX_TRACKED_CAPS];
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
    seL4_CPtr wifi_cmd_recv_ep;
    seL4_CPtr wifi_cmd_send_ep;

    CapTracker cap_tracker;

    // --- НОВОЕ: Трекинг копий SHM для защиты от утечек ---
    bool has_shm;
    seL4_CPtr shm_copies[4];
};

// --- UNIX PIPES SUBSYSTEM ---
#define MAX_PIPES 16
#define PIPE_BASE_BADGE 1000 // Бейджи от 1000 до 1015 будут пайпами

// Общий IRQ 158 (EMMC2 + Wi-Fi SDIO — одна физическая GIC-линия на обоих
// контроллерах, см. platform.h/ROADMAP.md 4.5) слушает САМ root, а не
// какой-то конкретный драйвер — только один процесс вообще может держать
// IRQHandler-капу на этот номер. Badge заведомо вне диапазонов PID (1-255)
// и пайпов (1000-1015) выше, чтобы не путаться с обычными сообщениями в
// главном цикле ядра.
#define IRQ_MMC_SHARED_BADGE 2000

struct pipe_t {
    bool active;
    char buffer[4096];
    int count; // Сколько байт сейчас в буфере
    
    // Блокировка! Если читатель пришел, а пайп пуст, мы сохраняем его Reply Cap,
    // чтобы ответить (разбудить) его позже, когда писатель положит данные.
    seL4_CPtr reader_reply_cap; 
    int writer_pid;
    int owner_pid; // PID процесса, который создал пайп
    
    bool eof; // Флаг конца файла (писатель умер или закрыл трубу)
};

static ProcessControlBlock pcbs[256];
static int next_pid = 1;

static seL4_CPtr alloc_and_track_cap(PsychAllocator &alloc, ProcessControlBlock &pcb) {
    seL4_CPtr cap = alloc.alloc_slot();
    
    if (cap == 0) {
        uart_puts("KERNEL PANIC: Out of CSlots during process allocation!\n");
        while(1);
    }

    if (pcb.cap_tracker.count < MAX_TRACKED_CAPS) {
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

static pipe_t g_pipes[MAX_PIPES] = {0};
static SharedMemoryRegion shm_regions[16];

// Готовность драйверов при загрузке (см. SYS_DRIVER_READY/SYS_WAIT_ALL_DRIVERS_READY
// в common.h). Индекс — это is_driver (1=uart, 2=timer, 3=blk, 4=net); 0 (shell/apps)
// не используется. driver_ready_wait_reply — сохраненный reply-cap шелла, если он
// вызвал SYS_WAIT_ALL_DRIVERS_READY раньше, чем готовы все 4 модуля.
static bool driver_ready[5] = {false, false, false, false, false};
static seL4_CPtr driver_ready_wait_reply = 0;

static bool all_drivers_ready() {
    return driver_ready[1] && driver_ready[2] && driver_ready[3] && driver_ready[4];
}

// Wi-Fi (index 5) сознательно НЕ входит в driver_ready[]/all_drivers_ready()
// выше (см. их комментарий) — он больше не автоспавнится при загрузке и
// живёт своим отдельным жизненным циклом (SYS_START_WIFI/SYS_STOP_WIFI/
// SYS_WIFI_STATUS). true означает "wifi_driver дошёл до своего SYS_DRIVER_READY",
// т.е. гарантированно уже висит в блокирующем seL4_Recv и готов принимать
// любые WIFI_CMD_* — даже если сама проба SDIO/прошивки внутри провалилась
// (в этом случае команды просто вернут код ошибки, а не зависнут).
static bool g_wifi_driver_ready = false;

static int load_elf_from_disk(seL4_CPtr blk_ep, const char* filename, char* load_buffer) {    char* shm = rootserver_shm_base;
    uint32_t total_read = 0;

    while (1) {
        // Драйвер перезаписывает SHM, поэтому имя файла восстанавливаем перед каждым запросом
        strncpy(shm, filename, 63);
        shm[63] = '\0';
        flush_rootserver_shm(); // иначе имя файла может не долететь до RAM к моменту, когда blk_driver его некэшируемо прочитает

        seL4_SetMR(0, 119); // SYS_READ_FILE
        seL4_SetMR(1, total_read); // Передаем СМЕЩЕНИЕ (offset)

        seL4_MessageInfo_t msg = seL4_MessageInfo_new(0, 0, 0, 2);
        seL4_Call(blk_ep, msg);

        int status = seL4_GetMR(0);
        int bytes_read = seL4_GetMR(1);

        if (status != 0) return -1; // Ошибка чтения или файл не найден
        if (bytes_read == 0) break;    // Конец файла (EOF)

        flush_rootserver_shm(); // иначе можем прочитать устаревшую закэшированную копию вместо свежего ответа blk_driver
        // Копируем полученный безопасный кусок в большой буфер Rootserver'а
        memcpy(load_buffer + total_read, shm, bytes_read);
        total_read += bytes_read;
    }
    
    return total_read;
}

// Умная функция маппинга (Самовосстанавливающееся дерево VSpace)
// Эта функция мапит ТОЛЬКО динамическое SHM (см. единственный вызов в
// SYS_SHM_GET ниже) — а эта память используется как буфер и для GENET DMA
// (net_driver.cpp: net_hw_send/net_hw_poll_rx), и GENET DMA-движок читает и
// пишет физическую RAM напрямую, в обход кэша CPU. seL4_ARM_Default_VMAttributes
// маппит страницу как обычную кэшируемую (WriteBack) память — тогда запись
// пакета в SHM перед отправкой могла годами оставаться только в dirty-кэше
// CPU и никогда не долетать до RAM до того, как DMA её оттуда читал (отсюда
// "TX вроде завершается, а на проводе мусор" — GENET считает дескриптор
// обработанным независимо от того, актуальны ли байты в физической памяти).
// Явное cache maintenance (clean/invalidate) потребовало бы прокидывать
// capability страницы через IPC в net_driver — вместо этого, как и для MMIO
// device-страниц в этом же файле (см. комментарий про attridx=NORMAL/DEVICE
// ниже), проще и надёжнее сразу мапить эту память некэшируемой: (seL4_ARM_VMAttributes)0.
static bool map_frame_robust(PsychAllocator &alloc, ProcessControlBlock &pcb, seL4_CPtr frame, seL4_CPtr vspace, uintptr_t vaddr, seL4_CPtr normal_untyped, seL4_CPtr root_cnode) {
    // Сначала пробуем замапить фрейм напрямую
    seL4_Error err = seL4_ARM_Page_Map(frame, vspace, vaddr, seL4_AllRights, (seL4_ARM_VMAttributes)0);

    if (err == seL4_FailedLookup) {
        // Не хватает промежуточных каталогов. Создаем их вслепую.
        // Если каталог уже существует (например, PGD[0]), seL4 вернет DeleteFirst (8). Мы ИГНОРИРУЕМ эту ошибку.

        seL4_CPtr pud = alloc_and_track_cap(alloc, pcb);
        if (seL4_Untyped_Retype(normal_untyped, seL4_ARM_PageUpperDirectoryObject, 0, root_cnode, 0, 0, pud, 1) == seL4_NoError) {
            seL4_ARM_PageUpperDirectory_Map(pud, vspace, vaddr, (seL4_ARM_VMAttributes)0);
        }

        seL4_CPtr pd = alloc_and_track_cap(alloc, pcb);
        if (seL4_Untyped_Retype(normal_untyped, seL4_ARM_PageDirectoryObject, 0, root_cnode, 0, 0, pd, 1) == seL4_NoError) {
            seL4_ARM_PageDirectory_Map(pd, vspace, vaddr, (seL4_ARM_VMAttributes)0);
        }

        seL4_CPtr pt = alloc_and_track_cap(alloc, pcb);
        if (seL4_Untyped_Retype(normal_untyped, seL4_ARM_PageTableObject, 0, root_cnode, 0, 0, pt, 1) == seL4_NoError) {
            seL4_ARM_PageTable_Map(pt, vspace, vaddr, (seL4_ARM_VMAttributes)0);
        }

        // Дерево проложено. Мапим фрейм повторно.
        err = seL4_ARM_Page_Map(frame, vspace, vaddr, seL4_AllRights, (seL4_ARM_VMAttributes)0);
    }

    if (err != seL4_NoError) {
        uart_puts("[ROOT] FATAL: Robust map failed!\n");
        return false;
    }
    return true;
}

static int spawn_process(const char* name, char* elf_data, unsigned long elf_size, seL4_CPtr ep, seL4_CPtr med_ep,
                         PsychAllocator &alloc, seL4_CPtr root_cnode, seL4_CPtr root_vspace, seL4_CPtr normal_untyped,
                         seL4_CPtr shm_frame_root, int is_driver, seL4_CPtr console_ep, seL4_CPtr timer_ep,
                         seL4_CPtr blk_ep, seL4_CPtr stdin_ep, seL4_CPtr stdout_ep, seL4_CPtr stderr_ep,
                         seL4_CPtr irq_ntfn, seL4_CPtr irq_handler, seL4_CPtr hw_frame,
                         const char *args_payload = nullptr,
                         seL4_CPtr net_cmd_recv_ep = 0,
                         seL4_CPtr net_cmd_send_ep = 0,
                         seL4_CPtr wifi_cmd_recv_ep = 0,
                         seL4_CPtr wifi_cmd_send_ep = 0,
                         // Второй/третий MMIO-регион для is_driver == 2 (timer_driver) —
                         // регистры VideoCore mailbox + приватный буфер под property-tag
                         // запрос (Фаза 4.6, см. ROADMAP.md). Не обобщаем на другие
                         // драйверы — это разовая надобность именно timer_driver'а.
                         seL4_CPtr mbox_regs_frame = 0,
                         seL4_CPtr mbox_buf_frame_param = 0,
                         seL4_Word mbox_buf_paddr_param = 0,
                         // Для is_driver == 2 (timer_driver): капа на нотификацию
                         // net_driver'а (badged NET_EVENT_HEARTBEAT), которой
                         // timer_driver периодически будит net_driver (Фаза 4.5,
                         // см. common.h/BOOT_HEARTBEAT_NTFN_CAP). Обычное копирование
                         // capability, как и с blk_irq_ntfn — не TCB-bind.
                         seL4_CPtr extra_ntfn_param = 0,
                         // Для is_driver == 3 (blk_driver): приватный
                         // некэшируемый DMA bounce-буфер под ADMA2-дескрипторы
                         // (Фаза 4.5, см. PLAT_BLK_DMA_VADDR/platform.h) —
                         // та же схема, что mbox_buf_frame_param выше.
                         seL4_CPtr blk_dma_frame_param = 0,
                         seL4_Word blk_dma_paddr_param = 0) {
    
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
    seL4_Word local_wifi_send_ep = 6; // Wi-Fi (Фаза 4): шелл шлёт диагностические команды wifi_driver
    seL4_Word local_blk_ep      = 7; // VFS/Block Driver Endpoint
    seL4_Word local_wifi_recv_ep = 8; // Wi-Fi (Фаза 4): wifi_driver слушает на этом слоте (см. BOOT_WIFI_EP)
    seL4_Word local_syscall_ep  = 10; // <-- Локальный индекс для Faults и Syscalls
    seL4_Word local_extra_ntfn  = 13; // Фаза 4.5: капа на heartbeat-нотификацию net_driver'а (только для timer_driver, см. extra_ntfn_param)

    check_err(seL4_CNode_Copy(child_cnode, local_syscall_ep, 8, root_cnode, badged_ep, seL4_WordBits, seL4_AllRights), "Copy syscall ep");

    if (is_driver == 1 || is_driver == 2) {
        // UART/Timer driver: капа на СОБСТВЕННЫЙ CNode (см. SELF_CNODE_SLOT в
        // common.h) — нужна для seL4_CNode_SaveCaller() внутри самого
        // процесса, чтобы откладывать reply вместо немедленного ответа
        // (UART: SYS_READ, Фаза 4.5; Timer: SYS_SLEEP_MS, тоже Фаза 4.5, см.
        // timer_driver.cpp). child_cnode здесь — это capability НА ТОТ ЖЕ
        // CNode-объект в адресном пространстве root_cnode; копия внутрь
        // себя же — обычный seL4-приём для self-reference.
        check_err(seL4_CNode_Copy(child_cnode, SELF_CNODE_SLOT, 8, root_cnode, child_cnode, seL4_WordBits, seL4_AllRights), "Copy self-CNode cap");
    }

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
    if (wifi_cmd_send_ep != 0) check_err(seL4_CNode_Mint(child_cnode, local_wifi_send_ep, 8, root_cnode, wifi_cmd_send_ep, seL4_WordBits, seL4_AllRights, pid), "Mint wifi send ep");
    if (wifi_cmd_recv_ep != 0) check_err(seL4_CNode_Copy(child_cnode, local_wifi_recv_ep, 8, root_cnode, wifi_cmd_recv_ep, seL4_WordBits, seL4_AllRights), "Copy wifi recv ep");
    if (extra_ntfn_param != 0) check_err(seL4_CNode_Copy(child_cnode, local_extra_ntfn, 8, root_cnode, extra_ntfn_param, seL4_WordBits, seL4_AllRights), "Copy extra ntfn (heartbeat)");

    pcb.cspace = child_cnode;
    pcb.badged_ep = badged_ep; // Оставляем глобальный в pcb для нужд ядра

    // СОХРАНЯЕМ АППАРАТНЫЙ ПРОФИЛЬ В PCB:
    pcb.is_driver = is_driver; 
    pcb.irq_ntfn = irq_ntfn;
    pcb.irq_handler = irq_handler;
    pcb.hw_frame = hw_frame;
    pcb.net_cmd_recv_ep = net_cmd_recv_ep;
    pcb.net_cmd_send_ep = net_cmd_send_ep;
    pcb.wifi_cmd_recv_ep = wifi_cmd_recv_ep;
    pcb.wifi_cmd_send_ep = wifi_cmd_send_ep;

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

    for (int i = 0; i < elf_getNumProgramHeaders(&elf); i++) {
        if (elf_getProgramHeaderType(&elf, i) == PT_LOAD) {
            uint64_t vaddr = elf_getProgramHeaderVaddr(&elf, i);
            uint64_t filesz = elf_getProgramHeaderFileSize(&elf, i);
            uint64_t memsz = elf_getProgramHeaderMemorySize(&elf, i);
            uint64_t offset = elf_getProgramHeaderOffset(&elf, i);
            uint64_t page_start = vaddr & ~0xFFFULL;
            uint64_t page_end = (vaddr + memsz + 0xFFF) & ~0xFFFULL;
            
            for (uint64_t page = page_start; page < page_end; page += 4096) {
                seL4_CPtr frame = alloc_and_track_cap(alloc, pcb);
                seL4_Untyped_Retype(normal_untyped, seL4_ARM_SmallPageObject, 0, root_cnode, 0, 0, frame, 1);
                
                // Используем скользящее окно!
                seL4_ARM_Page_Map(frame, root_vspace, elf_temp_vaddr, seL4_AllRights, seL4_ARM_Default_VMAttributes);
                memset((void*)elf_temp_vaddr, 0, 4096);
                
                uint64_t copy_start = (page > vaddr) ? page : vaddr;
                uint64_t copy_end = (page + 4096 < vaddr + filesz) ? page + 4096 : vaddr + filesz;
                if (copy_start < copy_end) {
                    memcpy((void*)(elf_temp_vaddr + (copy_start - page)), elf_file + offset + (copy_start - vaddr), copy_end - copy_start);
                }
                seL4_ARM_Page_Clean_Data(frame, 0, 4096);
                seL4_ARM_Page_Unmap(frame);

                seL4_ARM_Page_Map(frame, child_vspace, page, seL4_AllRights, seL4_ARM_Default_VMAttributes);
            }
        }
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

    // ИСПРАВЛЕНИЕ: Мы не можем передавать Capability из CSpace ядра напрямую.
    // Вместо этого мы используем локальные слоты, в которые мы уже сминтовали
    // нужные capabilities (в данном случае, console_ep).
    child_ipc_ptr->caps_or_badges[0] = local_console_ep; // FD 0 = STDIN
    child_ipc_ptr->caps_or_badges[1] = local_console_ep; // FD 1 = STDOUT
    child_ipc_ptr->caps_or_badges[2] = local_console_ep; // FD 2 = STDERR

    child_ipc_ptr->msg[BOOT_ROOT_EP] = local_syscall_ep;

    // is_driver == 2 (timer): ARM generic timer сам по себе читается прямой
    // mrs-инструкцией из EL0 и не мапится как MMIO (см. hw_timer.cpp, main()
    // выше) — но тот же процесс теперь дополнительно читает термодатчик AVS
    // RO thermal, который MMIO-регистр как обычно, поэтому hw_frame для
    // timer_driver больше не всегда 0 (см. avs_frame выше).
    if (hw_frame != 0 && (is_driver == 1 || is_driver == 2 || is_driver == 3 || is_driver == 4 || is_driver == 5)) { // Any driver with real MMIO
        seL4_CPtr drv_pud = alloc_and_track_cap(alloc, pcb);
        seL4_CPtr drv_pd  = alloc_and_track_cap(alloc, pcb);
        seL4_CPtr drv_pt  = alloc_and_track_cap(alloc, pcb);
        check_err(seL4_Untyped_Retype(normal_untyped, seL4_ARM_PageUpperDirectoryObject, 0, root_cnode, 0, 0, drv_pud, 1), "Retype Drv PUD");
        check_err(seL4_Untyped_Retype(normal_untyped, seL4_ARM_PageDirectoryObject, 0, root_cnode, 0, 0, drv_pd, 1), "Retype Drv PD");
        check_err(seL4_Untyped_Retype(normal_untyped, seL4_ARM_PageTableObject, 0, root_cnode, 0, 0, drv_pt, 1), "Retype Drv PT");

        uintptr_t hw_vaddr = (is_driver == 1) ? PLAT_UART_VADDR :
                             (is_driver == 2) ? PLAT_AVS_VADDR :
                             (is_driver == 5) ? PLAT_WIFI_SDIO_VADDR :
                             ((is_driver == 3) ? PLAT_EMMC_VADDR : PLAT_GENET_VADDR);

        seL4_ARM_PageUpperDirectory_Map(drv_pud, child_vspace, hw_vaddr, (seL4_ARM_VMAttributes)0);
        seL4_ARM_PageDirectory_Map(drv_pd, child_vspace, hw_vaddr, (seL4_ARM_VMAttributes)0);
        seL4_ARM_PageTable_Map(drv_pt, child_vspace, hw_vaddr, (seL4_ARM_VMAttributes)0);

        // EMMC2 умещается в одну страницу (0x100 байт регистров). GENET
        // занимает целых 64KB (0x10000) — 16 страниц.
        int num_pages = (is_driver == 4) ? 16 : 1;
        for (int i = 0; i < num_pages; i++) {
            seL4_CPtr frame_child = alloc_and_track_cap(alloc, pcb);
            check_err(seL4_CNode_Copy(root_cnode, frame_child, seL4_WordBits, 
                                      root_cnode, hw_frame + i, seL4_WordBits, seL4_AllRights), "Copy HW Frame Cap");
            check_err(seL4_ARM_Page_Map(frame_child, child_vspace, hw_vaddr + (i * 4096),
                                        seL4_AllRights, (seL4_ARM_VMAttributes)0), "Map HW to Driver");
        }

        // PLAT_MBOX_VADDR/PLAT_MBOX_BUF_VADDR лежат в том же 2MB-регионе,
        // что и PLAT_AVS_VADDR (см. platform.h) — drv_pud/drv_pd/drv_pt выше
        // уже покрывают весь этот диапазон, отдельная PUD/PD/PT-иерархия для
        // них не нужна, только дополнительный Page_Map.
        if (is_driver == 2 && mbox_regs_frame != 0) {
            seL4_CPtr mbox_regs_child = alloc_and_track_cap(alloc, pcb);
            check_err(seL4_CNode_Copy(root_cnode, mbox_regs_child, seL4_WordBits,
                                      root_cnode, mbox_regs_frame, seL4_WordBits, seL4_AllRights), "Copy Mbox Regs Frame Cap");
            check_err(seL4_ARM_Page_Map(mbox_regs_child, child_vspace, PLAT_MBOX_VADDR,
                                        seL4_AllRights, (seL4_ARM_VMAttributes)0), "Map Mbox Regs to Driver");
        }
        if (is_driver == 2 && mbox_buf_frame_param != 0) {
            seL4_CPtr mbox_buf_child = alloc_and_track_cap(alloc, pcb);
            check_err(seL4_CNode_Copy(root_cnode, mbox_buf_child, seL4_WordBits,
                                      root_cnode, mbox_buf_frame_param, seL4_WordBits, seL4_AllRights), "Copy Mbox Buf Frame Cap");
            check_err(seL4_ARM_Page_Map(mbox_buf_child, child_vspace, PLAT_MBOX_BUF_VADDR,
                                        seL4_AllRights, (seL4_ARM_VMAttributes)0), "Map Mbox Buf to Driver");
        }
        // Фаза 4.5/ADMA2 (см. ROADMAP.md) — приватный некэшируемый DMA
        // bounce-буфер blk_driver, тот же приём, что mbox_buf выше.
        // PLAT_BLK_DMA_VADDR лежит в том же 2MB-регионе, что и PLAT_EMMC_VADDR
        // (drv_pud/pd/pt для is_driver==3 уже созданы в этом же блоке выше).
        if (is_driver == 3 && blk_dma_frame_param != 0) {
            seL4_CPtr blk_dma_child = alloc_and_track_cap(alloc, pcb);
            check_err(seL4_CNode_Copy(root_cnode, blk_dma_child, seL4_WordBits,
                                      root_cnode, blk_dma_frame_param, seL4_WordBits, seL4_AllRights), "Copy Blk DMA Frame Cap");
            check_err(seL4_ARM_Page_Map(blk_dma_child, child_vspace, PLAT_BLK_DMA_VADDR,
                                        seL4_AllRights, (seL4_ARM_VMAttributes)0), "Map Blk DMA Buf to Driver");
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
            child_ipc_ptr->msg[BOOT_MBOX_BUF_PADDR] = mbox_buf_paddr_param; // Фаза 4.6, см. platform.h
            child_ipc_ptr->msg[BOOT_HEARTBEAT_NTFN_CAP] = (extra_ntfn_param != 0) ? local_extra_ntfn : 0; // Фаза 4.5, см. common.h
        } else if (is_driver == 3) { // Block driver - клиент консоли
            child_ipc_ptr->msg[7] = local_blk_ep; // BOOT_BLK_EP
            child_ipc_ptr->msg[BOOT_CONSOLE_EP] = local_console_ep;
            // Фаза 4.5: капа на нотификацию общего IRQ EMMC2/Wi-Fi SDIO —
            // см. g_emmc_irq_ntfn в blk_driver.cpp, local_irq_handler
            // используется здесь просто как "ещё один слот с готовой
            // capability", не как настоящий IRQHandler.
            child_ipc_ptr->msg[BOOT_IRQ_EP] = local_irq_handler;
            // Фаза 4.5/ADMA2: физический адрес DMA bounce-буфера — см.
            // blk_dma_paddr_param выше и PLAT_BLK_DMA_VADDR/platform.h.
            child_ipc_ptr->msg[BOOT_BLK_DMA_PADDR] = blk_dma_paddr_param;
        } else if (is_driver == 4) { // Net driver - клиент консоли, таймера и blk (журнал net_udp.log)
            child_ipc_ptr->msg[BOOT_CONSOLE_EP] = local_console_ep;
            child_ipc_ptr->msg[BOOT_TIMER_EP] = local_timer_ep;
            child_ipc_ptr->msg[BOOT_NET_EP] = local_net_recv_ep;
            child_ipc_ptr->msg[7] = local_blk_ep; // BOOT_BLK_EP
            // Фаза 4.5: настоящая IRQHandler-капа GENET RX (RPI4_GENET_IRQ_A) —
            // собственная линия, ни с кем не разделяемая (в отличие от IRQ 158
            // EMMC2/Wi-Fi), поэтому net_driver держит её сам и Ack'ает сам,
            // без root-релея (тот же приём, что у timer_driver).
            child_ipc_ptr->msg[BOOT_IRQ_EP] = local_irq_handler;
        } else if (is_driver == 5) { // Wi-Fi driver (Фаза 4) - сервер для шелла, клиент консоли и blk (Милстоун 4.2: чтение прошивки/NVRAM с SD)
            child_ipc_ptr->msg[BOOT_CONSOLE_EP] = local_console_ep;
            child_ipc_ptr->msg[BOOT_WIFI_EP] = local_wifi_recv_ep;
            child_ipc_ptr->msg[7] = local_blk_ep; // BOOT_BLK_EP
            // Фаза 4.5 (продолжение): капа на нотификацию общего IRQ EMMC2/
            // Wi-Fi SDIO — тот же приём, что у blk_driver (local_irq_handler
            // здесь тоже просто "слот с готовой capability на нотификацию",
            // не настоящий IRQHandler, см. wifi_irq_ntfn в main()).
            child_ipc_ptr->msg[BOOT_IRQ_EP] = local_irq_handler;
        }

    } else {
        // Shell or other user app
        child_ipc_ptr->msg[BOOT_CONSOLE_EP] = local_console_ep;
        child_ipc_ptr->msg[BOOT_TIMER_EP] = local_timer_ep;
        child_ipc_ptr->msg[BOOT_NET_EP] = local_net_send_ep;
        child_ipc_ptr->msg[BOOT_WIFI_EP] = local_wifi_send_ep;
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
    regs.pc = entry_point;
    regs.sp = child_stack + 4096;
    regs.x0 = (seL4_Word)badged_ep;
    regs.x1 = (seL4_Word)child_ipc;
    regs.x2 = (seL4_Word)med_ep; 
    regs.tpidr_el0 = (seL4_Word)child_ipc + 3072;
    regs.tpidrro_el0 = (seL4_Word)child_ipc + 3072;
    size_t reg_count = sizeof(seL4_UserContext) / sizeof(seL4_Word);
    seL4_TCB_WriteRegisters(tcb, 0, 0, reg_count, &regs);

    
    seL4_TCB_SetTLSBase(tcb, child_ipc + 3072);
    seL4_TCB_SetPriority(tcb, seL4_CapInitThreadTCB, 254);

    // Привязываем прерывание только тем драйверам, у кого реально есть IRQ
    // (раньше это было "is_driver == 1 || is_driver == 2", но таймер (2)
    // больше не MMIO/IRQ-устройство — ARM generic timer читается из EL0
    // напрямую, см. platform.h/hw_timer.cpp — irq_ntfn для него теперь 0,
    // а bind нулевой notification-капы — IllegalOperation в ядре).
    if (irq_ntfn != 0) {
        check_err(seL4_TCB_BindNotification(tcb, irq_ntfn), "Bind IRQ to Driver");
    }

    seL4_TCB_Resume(tcb);
    return pid;
}

static void generic_recover_process(int pid, seL4_CPtr ep, seL4_CPtr med_ep, PsychAllocator &alloc,
                                    seL4_CPtr root_cnode, seL4_CPtr root_vspace, seL4_CPtr normal_untyped,
                                    seL4_CPtr shm_frame_root, seL4_CPtr console_ep, seL4_CPtr timer_ep, seL4_CPtr blk_ep,
                                    bool respawn = true) {
    if (pid <= 0 || pid >= 256 || !pcbs[pid].active)
        return;

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

    if (respawn && (meta.is_driver > 0 || strcmp(meta.name, "shell") == 0)) {
        uart_puts("[WATCHDOG] Respawning critical system component...\n");

        int new_pid = spawn_process(meta.name, nullptr, 0, ep, med_ep, alloc, root_cnode, root_vspace, normal_untyped, shm_frames[0],
                                    meta.is_driver, console_ep, timer_ep, blk_ep, console_ep, console_ep, console_ep,
                                    meta.irq_ntfn, meta.irq_handler, meta.hw_frame,
                                    nullptr, meta.net_cmd_recv_ep, meta.net_cmd_send_ep,
                                    meta.wifi_cmd_recv_ep, meta.wifi_cmd_send_ep);

        if (new_pid > 0) {
            uart_puts("[WATCHDOG] Service restored successfully. New PID: "); uart_putdec(new_pid); uart_puts("\n");
        } else {
            uart_puts("[WATCHDOG] CRITICAL ERROR: Failed to respawn component!\n");
        }
    } else if (!respawn) {
        // Явная ручная остановка (см. SYS_STOP_WIFI) — в отличие от аварийного
        // восстановления (SYS_KILL/SYS_RECOVER/watchdog), респавн НЕ нужен.
        uart_puts("[ROOT] Process stopped (manual, no respawn): "); uart_puts(meta.name); uart_puts("\n");
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

    seL4_CPtr pmd = alloc.alloc_slot();
    seL4_CPtr pt = alloc.alloc_slot();
    seL4_Untyped_Retype(normal_untyped, seL4_ARM_PageDirectoryObject, 0, root_cnode, 0, 0, pmd, 1);
    seL4_Untyped_Retype(normal_untyped, seL4_ARM_PageTableObject, 0, root_cnode, 0, 0, pt, 1);

    // ИСПРАВЛЕНИЕ: Создаем и мапим дополнительную таблицу страниц (Page Table)
    // для временного окна IPC, которое было перемещено на новый адрес.
    seL4_CPtr pt_ipc_temp = alloc.alloc_slot();
    seL4_Untyped_Retype(normal_untyped, seL4_ARM_PageTableObject, 0, root_cnode, 0, 0, pt_ipc_temp, 1);

    // --- ВРЕМЕННО (hw bring-up на живой плате): гранулярные флаги на каждый
    // драйвер (см. platform.h, ROADMAP.md Фаза 3). Все три теперь портированы
    // на реальные адреса/механизмы RPi4 — UART/EMMC2/GENET через
    // alloc_device_frame(), таймер вообще без MMIO (ARM generic timer,
    // CNTVCT_EL0/CNTFRQ_EL0 из EL0, см. hw_timer.cpp).
    constexpr bool RPI4_ENABLE_TIMER = true;
    constexpr bool RPI4_ENABLE_BLK   = true;
    constexpr bool RPI4_ENABLE_NET   = true;
    // Wi-Fi (Фаза 4, Милстоун 4.1 — см. ROADMAP.md): выключено по умолчанию.
    // ВАЖНО: последний живой тест с циклом повтора CMD5 положил ВЕСЬ ядро
    // (seL4 "halting... Kernel entry via Unknown (0)" — это фатальный
    // необрабатываемый kernel-level halt, скорее всего SError/внешний abort
    // от повторной отправки CMD5 без паузы этому квирковому legacy-SDHCI
    // блоку, а не обычный recoverable page fault пользовательского процесса).
    // Такое требует физического перезапуска платы, а не просто перезаливки —
    // держим выключенным, пока не добавлена пошаговая диагностика и не
    // сделан повтор менее агрессивным (см. wifi_driver.cpp).
    constexpr bool RPI4_ENABLE_WIFI  = true;

    // PLAT_MBOX_PADDR (0xfe00b000) физически МЕНЬШЕ mini-UART AUX (0xfe215000)
    // и лежит в том же untyped-регионе — должен аллоцироваться СТРОГО ДО
    // uart_frame (тот же приём, что и для PLAT_WIFI_SDIO_PADDR/PLAT_EMMC_PADDR
    // ниже — см. комментарий там же и проверку "target_paddr BEHIND watermark"
    // в alloc_device_frame()).
    seL4_CPtr mbox_regs_frame = alloc_device_frame(info, alloc, PLAT_MBOX_PADDR, root_cnode);
    seL4_CPtr uart_frame = alloc_device_frame(info, alloc, PLAT_UART_PADDR, root_cnode);
    seL4_CPtr emmc_frame = 0;
    seL4_CPtr avs_frame = 0;
    seL4_CPtr mbox_buf_frame = 0;
    seL4_Word mbox_buf_paddr = 0;
    seL4_CPtr wifi_sdio_frame = 0;
    // GENET занимает 64KB (0x10000) — 16 страниц, а не один слот, как EMMC2.
    seL4_CPtr genet_frames[16] = {0};
    // ВАЖНО: PLAT_WIFI_SDIO_PADDR (0xfe300000) лежит в ТОМ ЖЕ untyped-регионе,
    // что и PLAT_EMMC_PADDR (0xfe340000), и физически МЕНЬШЕ него — поэтому
    // должен аллоцироваться СТРОГО ДО EMMC2 (см. проверку "target_paddr BEHIND
    // watermark" в alloc_device_frame() выше). Раньше это было наоборот и
    // wifi_driver получал чужой (EMMC2-соседний) физический фрейм, что портило
    // реальный SD-контроллер при живом тесте — см. ROADMAP.md, Милстоун 4.1.
    if (RPI4_ENABLE_WIFI) {
        wifi_sdio_frame = alloc_device_frame(info, alloc, PLAT_WIFI_SDIO_PADDR, root_cnode);
    }
    seL4_CPtr blk_dma_frame = 0;
    seL4_Word blk_dma_paddr = 0;
    if (RPI4_ENABLE_BLK) {
        emmc_frame = alloc_device_frame(info, alloc, PLAT_EMMC_PADDR, root_cnode);

        // Фаза 4.5/ADMA2 (см. ROADMAP.md) — приватный некэшируемый DMA
        // bounce-буфер blk_driver, тот же приём, что mbox_buf_frame ниже:
        // обычная RAM-страница (не device-фрейм с фиксированным адресом),
        // физический адрес нужен только для программирования
        // EMMC_ADMA_SYSADDR_OFFSET (см. platform.h/blk_driver.cpp).
        blk_dma_frame = alloc.alloc_slot();
        seL4_Untyped_Retype(normal_untyped, seL4_ARM_SmallPageObject, 0, root_cnode, 0, 0, blk_dma_frame, 1);
        seL4_ARM_Page_GetAddress_t blk_dma_addr_res = seL4_ARM_Page_GetAddress(blk_dma_frame);
        blk_dma_paddr = (seL4_Word)blk_dma_addr_res.paddr;
    }
    if (RPI4_ENABLE_NET) {
        for (int i = 0; i < 16; i++) {
            genet_frames[i] = alloc_device_frame(info, alloc, PLAT_GENET_PADDR + (i * 4096), root_cnode);
        }
    }
    if (RPI4_ENABLE_TIMER) {
        // AVS RO thermal (0xf00 байт регистров) умещается в одну страницу,
        // как EMMC2 — читается тем же процессом, что и таймер (timer_driver).
        avs_frame = alloc_device_frame(info, alloc, PLAT_AVS_PADDR, root_cnode);

        // Приватный буфер под property-tag запрос VideoCore mailbox (Фаза
        // 4.6, расследование DVFS, см. ROADMAP.md) — обычная RAM-страница
        // (не MMIO-device-фрейм с фиксированным физическим адресом, как
        // остальные выше), физический адрес нужен только затем, чтобы
        // сообщить его GPU через MAILBOX_WRITE. Маппится некэшируемым (см.
        // is_driver == 2 ниже) — разовый диагностический запрос не стоит
        // усложнять cache maintenance (см. flush_rootserver_shm() — тот же
        // класс проблемы, но там от него отказаться было нельзя).
        mbox_buf_frame = alloc.alloc_slot();
        seL4_Untyped_Retype(normal_untyped, seL4_ARM_SmallPageObject, 0, root_cnode, 0, 0, mbox_buf_frame, 1);
        seL4_ARM_Page_GetAddress_t mbox_buf_addr_res = seL4_ARM_Page_GetAddress(mbox_buf_frame);
        mbox_buf_paddr = (seL4_Word)mbox_buf_addr_res.paddr;
    }

    // ВАЖНО: MMIO должен маппиться некэшируемым (Device memory), иначе CPU
    // читает FR/LSR из кэша и никогда не видит обновления статусных битов
    // железа — драйвер зависает в busy-wait навечно. seL4_ARM_Default_VMAttributes
    // включает PageCacheable (см. kernel/src/arch/arm/64/kernel/vspace.c:
    // makeUserPagePTE — cacheable=1 выбирает attridx=NORMAL вместо DEVICE_nGnRnE).
    // Тот же (seL4_ARM_VMAttributes)0 уже правильно используется ниже для
    // маппинга UART в дочерний uart_driver (см. hw_vaddr).
    uintptr_t uart_vaddr = PLAT_UART_VADDR;
    seL4_ARM_PageDirectory_Map(pmd, root_vspace, uart_vaddr, (seL4_ARM_VMAttributes)0);
    seL4_ARM_PageTable_Map(pt, root_vspace, uart_vaddr, (seL4_ARM_VMAttributes)0);
    seL4_ARM_Page_Map(uart_frame, root_vspace, uart_vaddr, seL4_AllRights, (seL4_ARM_VMAttributes)0);

    // Мапим таблицу для временного окна IPC. Адрес должен совпадать с global_ipc_temp_vaddr.
    // Одна таблица покрывает 2MB, чего достаточно для 512 процессов.
    seL4_ARM_PageTable_Map(pt_ipc_temp, root_vspace, 0x200800000ULL, seL4_ARM_Default_VMAttributes);

    uart_init((void*)uart_vaddr);
    seL4_DebugPutString((char*)"[BRINGUP] uart_init (mini-UART): done\n");

    if (RPI4_ENABLE_TIMER) {
        timer_init(); // ARM generic timer — без device-frame, см. выше
    }

    // Драйверы, которые в этой сборке не спавнятся (см. RPI4_ENABLE_* выше),
    // никогда не пришлют SYS_DRIVER_READY — отмечаем их готовыми заранее,
    // иначе shell навечно зависнет в SYS_WAIT_ALL_DRIVERS_READY (main.cpp,
    // all_drivers_ready() ждёт все 4 индекса безусловно).
    if (!RPI4_ENABLE_TIMER) driver_ready[2] = true;
    if (!RPI4_ENABLE_BLK)   driver_ready[3] = true;
    if (!RPI4_ENABLE_NET)   driver_ready[4] = true;

    uart_puts("\n=================================================\n"
              "  Psych Ward OS -- microkernel edition (seL4)\n"
              "=================================================\n");
    if (!RPI4_ENABLE_TIMER || !RPI4_ENABLE_BLK || !RPI4_ENABLE_NET) {
        uart_puts("[ROOT] HW BRING-UP BUILD:");
        if (!RPI4_ENABLE_TIMER) uart_puts(" timer DISABLED (time/date/sleep/ntp will hang);");
        if (!RPI4_ENABLE_BLK)   uart_puts(" blk DISABLED (ls/cat/exec/touch/... will hang);");
        if (!RPI4_ENABLE_NET)   uart_puts(" net DISABLED (ping/send/... will hang);");
        uart_puts("\n");
    } else {
        // Wi-Fi больше НЕ в этом списке — wifi_driver больше не спавнится при
        // загрузке (см. SYS_START_WIFI ниже/ROADMAP.md): подозрение, что
        // одновременный спавн вместе с остальными драйверами вызывал гонку
        // мапинга/таймингов, изредка ломавшую готовность sdpcm-канала (см.
        // память проекта — интермиттентный краш на настройке CR4). Теперь
        // это отдельный, изолированный от общего бута шаг — команда шелла
        // "wifi start".
        uart_puts("[ROOT] Booting UART / Timer / Block / Net / Shell...\n");
        if (RPI4_ENABLE_WIFI) {
            uart_puts("[ROOT] Wi-Fi driver compiled in — run 'wifi start' to enable.\n");
        }
    }

    seL4_CPtr ep = alloc.alloc_slot();
    seL4_CPtr med_ep = alloc.alloc_slot();
    seL4_Untyped_Retype(normal_untyped, seL4_EndpointObject, 0, root_cnode, 0, 0, ep, 1);
    seL4_Untyped_Retype(normal_untyped, seL4_EndpointObject, 0, root_cnode, 0, 0, med_ep, 1);

    // --- ПРАВИЛЬНОЕ ВЫДЕЛЕНИЕ SHM (Из обычной ОЗУ, а не из Device Memory) ---
    // ВАЖНО: retype ВСЕХ 4 страниц идёт ОТДЕЛЬНЫМ, ничем не прерываемым
    // циклом, ДО какого-либо маппинга — net_driver.cpp (GENET DMA) и
    // остальные потребители этой SHM считают физический адрес любого
    // смещения как paddr(shm_frames[0]) + смещение, что верно, ТОЛЬКО если
    // все 4 страницы физически идут подряд. Раньше retype и map были в
    // ОДНОМ цикле — маппинг страницы[0] в НИКОГДА не мапленный диапазон
    // (см. комментарий у rootserver_shm_base ниже) triggers on-demand
    // создание PUD/PD/PT (ещё 3 объекта из ТОГО ЖЕ normal_untyped) МЕЖДУ
    // retype'ом страницы[0] и страницы[1] — на живом железе это давало
    // paddr(frame[0])=0x40005000, paddr(frame[1])=0x40009000 (дыра 0x4000
    // вместо 0x1000!), из-за чего GENET писал принятые кадры в
    // "дырочный" (чужой/неинициализированный) физический адрес вместо
    // настоящей 2-й/3-й страницы SHM — net_driver читал свою страницу и
    // видел одни нули (см. ROADMAP.md 4.5, живой баг ethertype=0).
    for (int i = 0; i < 4; i++) {
        shm_frames[i] = alloc.alloc_slot();
        seL4_Error err = seL4_Untyped_Retype(normal_untyped, seL4_ARM_SmallPageObject, 0,
                                             root_cnode, 0, 0, shm_frames[i], 1);
        if (err != seL4_NoError) {
            uart_puts("[ROOT] FATAL: Failed to allocate normal RAM for SHM!\n");
            while(1);
        }
    }
    for (int i = 0; i < 4; i++) {
        // Мапим эти физические фреймы в виртуальную память Rootserver'а —
        // ОБЯЗАТЕЛЬНО кэшируемой (seL4_ARM_Default_VMAttributes), в отличие
        // от map_frame_robust() ниже (та мапит некэшируемо ради когерентности
        // с GENET DMA). Пробовали сделать и здесь некэшируемо (для той же
        // когерентности с другими процессами) — но Device-память на ARM
        // требует строго выровненных обращений, а `strcpy()`/аналоги из
        // muslc (используются, например, в SYS_PS ниже) этого не гарантируют
        // — сразу же Alignment Fault прямо в потоке rootserver. Поэтому
        // остаёмся на кэшируемом маппинге, а когерентность с некэшируемыми
        // читателями/писателями (shell/blk_driver/net_driver и т.д., все
        // видят эту же физическую память через map_frame_robust()) обеспечиваем
        // явным cache maintenance — см. flush_rootserver_shm() ниже,
        // вызывается в каждом syscall-хендлере, который пишет сюда данные
        // для чужого некэшируемого чтения (SYS_PS и т.п.), а также перед
        // рутсерверным чтением того, что кто-то другой записал некэшируемо
        // (load_elf_from_disk — ответ blk_driver).
        // Новый адрес (см. комментарий у rootserver_shm_base) лежит в ранее
        // никогда не мапленном для root_vspace диапазоне — в отличие от
        // старого 0x502000, который "бесплатно" попадал в уже замапленный
        // элфлоадером образ rootserver'а, здесь нужно создать промежуточные
        // PUD/PD/PT сами (тот же приём отказоустойчивого создания, что и в
        // map_frame_robust() ниже, но с КЭШИРУЕМЫМИ атрибутами — см. комментарий
        // выше про то, почему rootserver_shm_base обязан остаться кэшируемым).
        uintptr_t shm_vaddr = (uintptr_t)rootserver_shm_base + (i * 4096);
        seL4_Error shm_map_err = seL4_ARM_Page_Map(shm_frames[i], root_vspace, shm_vaddr,
                                                    seL4_AllRights, seL4_ARM_Default_VMAttributes);
        if (shm_map_err == seL4_FailedLookup) {
            seL4_CPtr shm_pud = alloc.alloc_slot();
            if (seL4_Untyped_Retype(normal_untyped, seL4_ARM_PageUpperDirectoryObject, 0, root_cnode, 0, 0, shm_pud, 1) == seL4_NoError) {
                seL4_ARM_PageUpperDirectory_Map(shm_pud, root_vspace, shm_vaddr, seL4_ARM_Default_VMAttributes);
            }
            seL4_CPtr shm_pd = alloc.alloc_slot();
            if (seL4_Untyped_Retype(normal_untyped, seL4_ARM_PageDirectoryObject, 0, root_cnode, 0, 0, shm_pd, 1) == seL4_NoError) {
                seL4_ARM_PageDirectory_Map(shm_pd, root_vspace, shm_vaddr, seL4_ARM_Default_VMAttributes);
            }
            seL4_CPtr shm_pt = alloc.alloc_slot();
            if (seL4_Untyped_Retype(normal_untyped, seL4_ARM_PageTableObject, 0, root_cnode, 0, 0, shm_pt, 1) == seL4_NoError) {
                seL4_ARM_PageTable_Map(shm_pt, root_vspace, shm_vaddr, seL4_ARM_Default_VMAttributes);
            }
            shm_map_err = seL4_ARM_Page_Map(shm_frames[i], root_vspace, shm_vaddr, seL4_AllRights, seL4_ARM_Default_VMAttributes);
        }
        if (shm_map_err != seL4_NoError) {
            uart_puts("[ROOT] FATAL: Failed to map rootserver SHM window!\n");
            while(1);
        }
    }

    seL4_CPtr console_ep = alloc.alloc_slot();
    seL4_CPtr timer_ep = alloc.alloc_slot();
    seL4_CPtr blk_ep = alloc.alloc_slot();
    seL4_CPtr net_cmd_ep = alloc.alloc_slot();
    seL4_CPtr net_cmd_recv_ep = alloc.alloc_slot();
    seL4_CPtr net_cmd_send_ep = alloc.alloc_slot();
    seL4_CPtr wifi_cmd_ep = 0;
    seL4_CPtr wifi_cmd_recv_ep = 0;
    seL4_CPtr wifi_cmd_send_ep = 0;
    seL4_Untyped_Retype(normal_untyped, seL4_EndpointObject, 0, root_cnode, 0, 0, console_ep, 1);
    seL4_Untyped_Retype(normal_untyped, seL4_EndpointObject, 0, root_cnode, 0, 0, timer_ep, 1);
    seL4_Untyped_Retype(normal_untyped, seL4_EndpointObject, 0, root_cnode, 0, 0, blk_ep, 1);
    seL4_Untyped_Retype(normal_untyped, seL4_EndpointObject, 0, root_cnode, 0, 0, net_cmd_ep, 1);
    seL4_CNode_Copy(root_cnode, net_cmd_recv_ep, seL4_WordBits,
                    root_cnode, net_cmd_ep, seL4_WordBits, seL4_CanRead);
    seL4_CNode_Copy(root_cnode, net_cmd_send_ep, seL4_WordBits,
                    root_cnode, net_cmd_ep, seL4_WordBits, seL4_CapRights_new(0, 1, 0, 1)); // Write + Grant
    // Wi-Fi (Фаза 4, Милстоун 4.1) — тот же паттерн клиент/сервер, что и net_cmd_*
    // выше: wifi_driver слушает на recv-копии, шелл шлёт диагностику через send-копию.
    // ВАЖНО: строго под RPI4_ENABLE_WIFI, как и любой другой ресурс, завязанный
    // на конкретный RPI4_ENABLE_* флаг (emmc_frame/genet_frames/avs_frame выше
    // тоже аллоцируются только под своим флагом) — при выключенном wifi этот
    // код не должен потреблять вообще ничего лишнего (раньше по ошибке создавался
    // безусловно, см. ROADMAP.md Милстоун 4.1 — расследование halt'а на живом железе).
    if (RPI4_ENABLE_WIFI) {
        wifi_cmd_ep = alloc.alloc_slot();
        wifi_cmd_recv_ep = alloc.alloc_slot();
        wifi_cmd_send_ep = alloc.alloc_slot();
        seL4_Untyped_Retype(normal_untyped, seL4_EndpointObject, 0, root_cnode, 0, 0, wifi_cmd_ep, 1);
        seL4_CNode_Copy(root_cnode, wifi_cmd_recv_ep, seL4_WordBits,
                        root_cnode, wifi_cmd_ep, seL4_WordBits, seL4_CanRead);
        seL4_CNode_Copy(root_cnode, wifi_cmd_send_ep, seL4_WordBits,
                        root_cnode, wifi_cmd_ep, seL4_WordBits, seL4_CapRights_new(0, 1, 0, 1)); // Write + Grant
    }

    // Таймер (ARM generic timer) не MMIO-устройство и не генерирует IRQ,
    // доступный из EL0 на этой сборке ядра (см. hw_timer.cpp) — никакой
    // IRQ-обвязки timer_driver'у больше не нужно, в отличие от PL031.

    seL4_CPtr uart_ntfn = alloc.alloc_slot();
    seL4_CPtr badged_uart_ntfn = alloc.alloc_slot(); 
    seL4_CPtr uart_irq_handler = alloc.alloc_slot();
    seL4_Untyped_Retype(normal_untyped, seL4_NotificationObject, 0, root_cnode, 0, 0, uart_ntfn, 1);
    seL4_CNode_Mint(root_cnode, badged_uart_ntfn, seL4_WordBits, root_cnode, uart_ntfn, seL4_WordBits, seL4_AllRights, 1); 
    seL4_IRQControl_Get(seL4_CapIRQControl, PLAT_UART_IRQ, root_cnode, uart_irq_handler, seL4_WordBits);
    seL4_IRQHandler_SetNotification(uart_irq_handler, badged_uart_ntfn); 
    uart_enable_interrupts();
    seL4_IRQHandler_Ack(uart_irq_handler);

    // Запускаем Драйвер UART (is_driver = 1)
    if (spawn_process("uart_driver", nullptr, 0, ep, med_ep, alloc, root_cnode, root_vspace, normal_untyped, shm_frames[0],
                      1, console_ep, timer_ep, 0, console_ep, console_ep, console_ep, uart_ntfn, uart_irq_handler, uart_frame) < 0) {
        uart_puts("PANIC: UART Driver failed to load!\n"); while(1);
    }

    // GENET RX IRQ + heartbeat-нотификация для net_driver (Фаза 4.5, см.
    // ROADMAP.md/common.h) — создаём ЗДЕСЬ, ДО спавна timer_driver, потому
    // что timer_driver'у ниже нужна badged-копия badged_net_heartbeat_ntfn.
    // Оба badge (NET_EVENT_GENET_RX/NET_EVENT_HEARTBEAT) минтятся из ОДНОГО
    // net_event_ntfn — seL4 OR'ит непотреблённые бейджи одного объекта,
    // поэтому net_driver одним Recv видит и кадр, и будильник (см.
    // net_driver.cpp). GENET_IRQ_A — собственная линия (не общая, в отличие
    // от IRQ 158 EMMC2/Wi-Fi), поэтому net_driver держит настоящую
    // IRQHandler-капу сам и Ack'ает сам — root-релей не нужен.
    seL4_CPtr net_event_ntfn = alloc.alloc_slot();
    seL4_CPtr badged_genet_rx_ntfn = alloc.alloc_slot();
    seL4_CPtr badged_net_heartbeat_ntfn = alloc.alloc_slot();
    seL4_CPtr genet_irq_handler = alloc.alloc_slot();
    if (RPI4_ENABLE_NET) {
        seL4_Untyped_Retype(normal_untyped, seL4_NotificationObject, 0, root_cnode, 0, 0, net_event_ntfn, 1);
        seL4_CNode_Mint(root_cnode, badged_genet_rx_ntfn, seL4_WordBits, root_cnode, net_event_ntfn, seL4_WordBits, seL4_AllRights, NET_EVENT_GENET_RX);
        seL4_CNode_Mint(root_cnode, badged_net_heartbeat_ntfn, seL4_WordBits, root_cnode, net_event_ntfn, seL4_WordBits, seL4_AllRights, NET_EVENT_HEARTBEAT);
        check_err(seL4_IRQControl_Get(seL4_CapIRQControl, RPI4_GENET_IRQ_A, root_cnode, genet_irq_handler, seL4_WordBits), "IRQControl_Get(GENET_IRQ_A)");
        check_err(seL4_IRQHandler_SetNotification(genet_irq_handler, badged_genet_rx_ntfn), "IRQHandler_SetNotification(genet)");
        check_err(seL4_IRQHandler_Ack(genet_irq_handler), "IRQHandler_Ack(genet, initial)");
    }

    // Физический таймер (PPI 30, non-secure) — Фаза 4.5, событийный sys_sleep
    // (см. platform.h/PLAT_TIMER_IRQ, easy-settings.cmake/KernelArmExportPTMRUser).
    // Не общий ни с чем (в отличие от IRQ 158 EMMC2/Wi-Fi) — timer_driver
    // держит настоящую IRQHandler-капу сам и сам себя Ack'ает, никакого
    // root-релея не нужно (см. blk_driver.cpp/SYS_MMC_IRQ_ACK для контраста,
    // где релей был обязателен из-за общей линии и разных priority).
    seL4_CPtr timer_ntfn = alloc.alloc_slot();
    seL4_CPtr badged_timer_ntfn = alloc.alloc_slot();
    seL4_CPtr timer_irq_handler = alloc.alloc_slot();
    if (RPI4_ENABLE_TIMER) {
        seL4_Untyped_Retype(normal_untyped, seL4_NotificationObject, 0, root_cnode, 0, 0, timer_ntfn, 1);
        seL4_CNode_Mint(root_cnode, badged_timer_ntfn, seL4_WordBits, root_cnode, timer_ntfn, seL4_WordBits, seL4_AllRights, 1);
        // ВРЕМЕННО (отладка живого зависания sleep, см. ROADMAP.md 4.5): эти
        // три вызова раньше не проверялись (как и у UART) — заворачиваем в
        // check_err(), чтобы явно увидеть в логе загрузки, если PPI 30
        // (физический таймер) почему-то не claim'ится как обычный IRQ.
        check_err(seL4_IRQControl_Get(seL4_CapIRQControl, PLAT_TIMER_IRQ, root_cnode, timer_irq_handler, seL4_WordBits), "IRQControl_Get(PLAT_TIMER_IRQ)");
        check_err(seL4_IRQHandler_SetNotification(timer_irq_handler, badged_timer_ntfn), "IRQHandler_SetNotification(timer)");
        check_err(seL4_IRQHandler_Ack(timer_irq_handler), "IRQHandler_Ack(timer, initial)");
    }

    if (RPI4_ENABLE_TIMER) {
    // Запускаем Драйвер Таймера (is_driver = 2)
    if (spawn_process("timer_driver", nullptr, 0, ep, med_ep, alloc, root_cnode, root_vspace, normal_untyped, shm_frames[0],
                      2, console_ep, timer_ep, 0, console_ep, console_ep, console_ep, timer_ntfn, timer_irq_handler, avs_frame,
                      nullptr, 0, 0, 0, 0, mbox_regs_frame, mbox_buf_frame, mbox_buf_paddr, badged_net_heartbeat_ntfn) < 0) {
        uart_puts("PANIC: Timer Driver failed to load!\n"); while(1);
    }
    }

    // Общий IRQ 158 (Фаза 4.5, см. define IRQ_MMC_SHARED_BADGE выше) — root
    // держит единственную IRQHandler-капу на эту линию (её физически
    // делят EMMC2 и Wi-Fi SDIO, см. platform.h), сам ничего не знает про
    // регистры конкретного контроллера и просто будит ОБА процесса (blk_
    // driver и wifi_driver) своими нотификациями при срабатывании — каждый
    // сам проверяет СВОЙ статусный регистр (разные физические контроллеры,
    // разные MMIO-адреса) и решает, его ли это событие (см.
    // sdpcm_wait_and_read_ctrl в wifi_driver.cpp / emmc_wait_irpt_bit в
    // blk_driver.cpp — оба уже толерантны к чужим/спекулятивным пробуждениям
    // на своей нотификации).
    seL4_CPtr blk_irq_ntfn = 0;
    seL4_CPtr wifi_irq_ntfn = 0;
    seL4_CPtr mmc_shared_irq_handler = 0;
    if (RPI4_ENABLE_BLK) {
        seL4_CPtr mmc_shared_irq_ntfn = alloc.alloc_slot();
        seL4_CPtr mmc_shared_irq_badged = alloc.alloc_slot();
        mmc_shared_irq_handler = alloc.alloc_slot();
        seL4_Untyped_Retype(normal_untyped, seL4_NotificationObject, 0, root_cnode, 0, 0, mmc_shared_irq_ntfn, 1);
        seL4_CNode_Mint(root_cnode, mmc_shared_irq_badged, seL4_WordBits, root_cnode, mmc_shared_irq_ntfn, seL4_WordBits, seL4_AllRights, IRQ_MMC_SHARED_BADGE);
        seL4_IRQControl_Get(seL4_CapIRQControl, RPI4_WIFI_SDIO_IRQ, root_cnode, mmc_shared_irq_handler, seL4_WordBits);
        seL4_IRQHandler_SetNotification(mmc_shared_irq_handler, mmc_shared_irq_badged);
        check_err(seL4_TCB_BindNotification(seL4_CapInitThreadTCB, mmc_shared_irq_ntfn), "Bind shared MMC IRQ to root");
        seL4_IRQHandler_Ack(mmc_shared_irq_handler);

        // Отдельная нотификация, которой root будит именно blk_driver (см.
        // seL4_TCB_BindNotification для него внутри spawn_process — тот же
        // общий механизм, что уже используется для UART, просто источник
        // сигнала теперь root, а не сам GIC напрямую).
        blk_irq_ntfn = alloc.alloc_slot();
        seL4_Untyped_Retype(normal_untyped, seL4_NotificationObject, 0, root_cnode, 0, 0, blk_irq_ntfn, 1);

        // Та же идея, но для wifi_driver (Фаза 4.5, продолжение) — отдельный
        // объект, а не badged-копия blk_irq_ntfn, потому что wifi_driver
        // спавнится/убивается динамически (SYS_START_WIFI/SYS_STOP_WIFI,
        // см. ROADMAP.md 4.4.1) — capability передаётся заново при каждом
        // "wifi start", сам notification-объект переживает рестарты
        // процесса без пересоздания.
        wifi_irq_ntfn = alloc.alloc_slot();
        seL4_Untyped_Retype(normal_untyped, seL4_NotificationObject, 0, root_cnode, 0, 0, wifi_irq_ntfn, 1);
    }

    if (RPI4_ENABLE_BLK) {
    // Запускаем Драйвер Диска и ФС (is_driver = 3). blk_irq_ntfn передаётся
    // ТРЕТЬИМ параметром из пары irq_ntfn/irq_handler (не первым!) — это
    // НЕ TCB-bind (blk_driver не читает свой my_ep для IRQ, см. комментарий
    // у g_emmc_irq_ntfn в blk_driver.cpp), а обычное копирование capability
    // на нотификацию в cspace процесса (тот же spawn_process-механизм, что
    // копирует IRQHandler для UART — семантика объекта ему не важна).
    if (spawn_process("blk_driver", nullptr, 0, ep, med_ep, alloc, root_cnode, root_vspace, normal_untyped, shm_frames[0],
                      3, console_ep, timer_ep, blk_ep, console_ep, console_ep, console_ep, 0, blk_irq_ntfn, emmc_frame,
                      nullptr, 0, 0, 0, 0, 0, 0, 0, 0, blk_dma_frame, blk_dma_paddr) < 0) {
        uart_puts("PANIC: Block Driver failed to load!\n"); while(1);
    }
    }

    if (RPI4_ENABLE_NET) {
    if (spawn_process("net_driver", nullptr, 0, ep, med_ep, alloc, root_cnode, root_vspace, normal_untyped, shm_frames[0],
                      4, console_ep, timer_ep, blk_ep, console_ep, console_ep, console_ep, net_event_ntfn, genet_irq_handler, genet_frames[0], nullptr,
                      net_cmd_recv_ep, 0) < 0) {
        uart_puts("PANIC: Net Driver failed to load!\n"); while(1);
    }
    }

    // Wi-Fi (is_driver = 5, Фаза 4) БОЛЬШЕ НЕ спавнится здесь при загрузке —
    // только по требованию, через "wifi start" в шелле (см. case
    // SYS_START_WIFI ниже). Ресурсы (wifi_cmd_ep/wifi_sdio_frame) уже
    // выделены выше под RPI4_ENABLE_WIFI — spawn_process() с ТЕМИ ЖЕ
    // аргументами, что были здесь раньше, просто перенесён в обработчик
    // SYS_START_WIFI.

    // Запускаем Оболочку (is_driver = 0). Сама оболочка при старте блокируется
    // на SYS_WAIT_ALL_DRIVERS_READY (см. главный цикл ниже и shell.cpp) —
    // поэтому ее собственный баннер/приглашение печатаются только после того,
    // как остальные 4 модуля отрапортуют о готовности через SYS_DRIVER_READY.
    // Порядок этого ожидания просто следует порядку spawn_process() выше —
    // никакого отдельного списка "кого ждать" не требуется.
    if (spawn_process("shell", nullptr, 0, ep, med_ep, alloc, root_cnode, root_vspace, normal_untyped, shm_frames[0],
                      0, console_ep, timer_ep, blk_ep, console_ep, console_ep, console_ep, 0, 0, 0, nullptr,
                      0, net_cmd_send_ep, 0, wifi_cmd_send_ep) < 0) {
        uart_puts("PANIC: Shell failed to load!\n"); while(1);
    }

    uart_puts("[ROOT] All sandboxes spawned. Serving IPC.\n");

    // --- ЕДИНЫЙ ЦИКЛ ЯДРА ---
    while (1) {
        seL4_Word sender_badge = 0;
        // Ожидаем прерывание, сообщение IPC или fault
        seL4_MessageInfo_t recv_info = seL4_Recv(ep, &sender_badge);

        if (sender_badge == IRQ_MMC_SHARED_BADGE) {
            // Общий IRQ EMMC2/Wi-Fi SDIO (см. define выше) — root не читает
            // регистры контроллеров сам, просто будит blk_driver; тот
            // проверяет свой статусный бит и, если это не он, просто снова
            // засыпает на своей нотификации (см. blk_driver.cpp).
            //
            // ВАЖНО: seL4_IRQHandler_Ack() здесь НЕ вызывается (см.
            // SYS_MMC_IRQ_ACK в common.h) — линия level-triggered, и Ack без
            // предварительного сброса девайсного статус-бита мгновенно
            // перезаводит тот же IRQ; root (priority 255) в таком цикле
            // никогда не отдал бы CPU blk_driver'у (priority 254), который
            // единственный может реально снять бит. Ack откладывается до
            // явного запроса от blk_driver, когда бит уже гарантированно снят.
            if (blk_irq_ntfn != 0) seL4_Signal(blk_irq_ntfn);
            if (wifi_irq_ntfn != 0) seL4_Signal(wifi_irq_ntfn);
            continue;
        }

        seL4_Word sender_pid = 0;

        // --- НОВАЯ, УМНАЯ ЛОГИКА ОБРАБОТКИ БЕЙДЖЕЙ ---
        bool is_pipe_call = (sender_badge >= PIPE_BASE_BADGE && sender_badge < PIPE_BASE_BADGE + MAX_PIPES);

        if (sender_badge != 0 && !is_pipe_call) {
            // Это обычный системный вызов или fault от процесса/потока.
            // Извлекаем PID из младших 16 бит.
            seL4_Word actual_pid = sender_badge & 0xFFFF;
            
            // Проверяем, что PID валиден и процесс активен
            if (actual_pid > 0 && actual_pid < 256 && pcbs[actual_pid].active) {
                sender_pid = actual_pid;
            } else {
                // Неопознанный бейдж, который не является пайпом. Игнорируем.
                continue; 
            }
        }
        // Если это вызов к пайпу (is_pipe_call == true), sender_pid остается 0.
        // Логика обработки пайпов в case 6 и 8 использует sender_badge, а не sender_pid.

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
                uint64_t ms = hw_timer_get_uptime_ms();
                seL4_SetMR(0, (seL4_Word)ms); seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                break;
            }

            case SYS_SLEEP:
                // ARM generic timer не даёт с EL0 аппаратного будильника (см.
                // hw_timer.cpp — EXPORT_PTMR_USER/VTMR_USER=false в этой
                // сборке ядра), а PL031-альтернативы на реальном железе нет.
                // Этот путь и раньше никем не вызывался (шелл спит через
                // клиентский поллинг SYS_GET_TIME, см. shell.cpp sys_sleep())
                // — отвечаем честной ошибкой вместо того, чтобы бесконечно
                // повесить вызывающего в SaveCaller без шанса на пробуждение.
                seL4_SetMR(0, (seL4_Word)-1); seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                break;
                
            case SYS_PUTCHAR:
                pl011_putchar((char)arg1);
                seL4_SetMR(0, 0); seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                break;

            case 20: { // SYS_PIPE (Создать конвейер)
                // 1. СРАЗУ СПАСАЕМ ВХОДНЫЕ ДАННЫЕ!
                seL4_Word requested_fd = seL4_GetMR(1);
                seL4_CPtr child_cspace = pcbs[sender_pid].cspace;

                int pipe_id = -1;
                for (int i = 0; i < MAX_PIPES; i++) {
                    if (!g_pipes[i].active) {
                        g_pipes[i].active = true;
                        g_pipes[i].count = 0;
                        g_pipes[i].reader_reply_cap = 0;
                        g_pipes[i].eof = false;
                        g_pipes[i].writer_pid = sender_pid;
                        g_pipes[i].owner_pid = sender_pid; // Запоминаем владельца
                        pipe_id = i;
                        break;
                    }
                }

                if (pipe_id != -1) {
                    seL4_Word pipe_badge = PIPE_BASE_BADGE + pipe_id;

                    // 2. Минтим Capability в правильный слот (requested_fd)
                    seL4_CNode_Delete(child_cspace, requested_fd, 8); // Pre-emptively clear slot
                    seL4_Error err = seL4_CNode_Mint(
                        child_cspace,       // CNode оболочки
                        requested_fd,       // Слот (см. PIPE_FD_SLOT в h/common.h)
                        8,                  // Глубина слота
                        root_cnode,         // Откуда берем
                        ep,                 // Базовый Endpoint (на который ядро слушает)
                        seL4_WordBits,
                        seL4_AllRights,
                        pipe_badge          // Устанавливаем бейдж 1000+
                    );

                    if (err == seL4_NoError) {
                        // 3. Формируем ответ (только теперь трогаем MR)
                        seL4_SetMR(0, requested_fd); // Возвращаем реальный FD
                        seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                    } else {
                        g_pipes[pipe_id].active = false; // Rollback
                        seL4_SetMR(0, -1);
                        seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                    }
                } else {
                    seL4_SetMR(0, -1);
                    seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                }
                break;
            }

            case 8: { // Универсальный WRITE (SYS_PUTS)
                if (sender_badge >= PIPE_BASE_BADGE && sender_badge < PIPE_BASE_BADGE + MAX_PIPES) { // Это пайп
                    int pipe_id = sender_badge - PIPE_BASE_BADGE;
                    pipe_t* p = &g_pipes[pipe_id];
                    
                    int chunk = seL4_MessageInfo_get_length(recv_info) - 1;
                    // Пишем данные в кольцевой буфер пайпа
                    for (int i = 0; i < chunk; i++) {
                        if (p->count < 4096) {
                            p->buffer[p->count++] = (char)seL4_GetMR(i + 1);
                        }
                    }

                    // Если кто-то спал и ждал данных (grep) - БУДИМ ЕГО!
                    if (p->reader_reply_cap != 0 && p->count > 0) {
                        seL4_SetMR(0, p->buffer[0]); // Отдаем 1 байт
                        for(int i = 1; i < p->count; i++) p->buffer[i-1] = p->buffer[i]; // Сдвигаем
                        p->count--;

                        seL4_Send(p->reader_reply_cap, seL4_MessageInfo_new(0, 0, 0, 1));
                        seL4_CNode_Delete(root_cnode, p->reader_reply_cap, seL4_WordBits);
                        alloc.free(p->reader_reply_cap);
                        p->reader_reply_cap = 0;
                    }
                    seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 0));
                } else {
                    // Заглушка, если кто-то случайно прислал консольный вывод в ядро
                    seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 0));
                }
                break;
            }

            case 6: { // Универсальный READ (SYS_READ)
                if (sender_badge >= PIPE_BASE_BADGE && sender_badge < PIPE_BASE_BADGE + MAX_PIPES) {
                    int pipe_id = sender_badge - PIPE_BASE_BADGE;
                    pipe_t* p = &g_pipes[pipe_id];

                    if (p->count > 0) {
                        // Данные есть, отдаем байт читателю!
                        seL4_SetMR(0, p->buffer[0]);
                        for(int i = 1; i < p->count; i++) p->buffer[i-1] = p->buffer[i];
                        p->count--;
                        seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                    } else if (p->eof) {
                        // Писатель (ls) завершился, закрываем трубу
                        seL4_SetMR(0, 0); // 0 байт = конец файла
                        seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                    } else {
                        // ДАННЫХ НЕТ! Писатель еще ничего не написал. Замораживаем читателя!
                        p->reader_reply_cap = alloc.alloc_slot();
                        seL4_CNode_SaveCaller(root_cnode, p->reader_reply_cap, seL4_WordBits);
                        // БЕЗ seL4_Reply! Процесс засыпает до прихода данных в case 8.
                    }
                } else {
                    seL4_SetMR(0, 0);
                    seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                }
                break;
            }

            case 24: { // SYS_PIPE_WR_CLOSE
                if (sender_badge >= PIPE_BASE_BADGE && sender_badge < PIPE_BASE_BADGE + MAX_PIPES) {
                    int pipe_id = sender_badge - PIPE_BASE_BADGE;
                    pipe_t* p = &g_pipes[pipe_id];
                    p->eof = true;
                    if (p->reader_reply_cap != 0) {
                        seL4_SetMR(0, 0); // EOF
                        seL4_Send(p->reader_reply_cap, seL4_MessageInfo_new(0, 0, 0, 1));
                        seL4_CNode_Delete(root_cnode, p->reader_reply_cap, seL4_WordBits);
                        alloc.free(p->reader_reply_cap);
                        p->reader_reply_cap = 0;
                    }
                }
                seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 0));
                break;
            }

            case 25: { // SYS_PIPE_CLOSE
                if (sender_badge >= PIPE_BASE_BADGE && sender_badge < PIPE_BASE_BADGE + MAX_PIPES) {
                    int pipe_id = sender_badge - PIPE_BASE_BADGE;
                    g_pipes[pipe_id].active = false;
                    
                    // ИСПРАВЛЕНО: Используем PID владельца, а не sender_pid, который равен 0
                    int owner_pid = g_pipes[pipe_id].owner_pid;
                    if (owner_pid > 0 && owner_pid < 256 && pcbs[owner_pid].active) {
                        // Удаляем capability из CSpace процесса-владельца.
                        // ВАЖНО: раньше здесь был захардкожен слот 3, который совпадает
                        // с local_net_send_ep — закрытие любого пайпа стирало сетевой
                        // capability оболочки. Слот пайпа — общий PIPE_FD_SLOT.
                        seL4_CNode_Delete(pcbs[owner_pid].cspace, PIPE_FD_SLOT, 8);
                    }
                }
                seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 0));
                break;
            }

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
                flush_rootserver_shm(); // "пациент" мог записать некэшируемо — иначе рутсервер прочитает устаревшую копию
                uart_puts("\n[DOCTOR] Patient wrote in SHM: \"");
                uart_puts(shm);
                uart_puts("\"\n");

                const char* reply = "Take 2 bytes of C++ and call me in the morning.";
                int i = 0;
                while(reply[i]) { shm[i] = reply[i]; i++; }
                shm[i] = '\0';

                flush_rootserver_shm(); // чтобы "пациент" некэшируемо увидел свежий ответ
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
                seL4_CPtr req_stdin_cap = seL4_GetMR(5);
                seL4_CPtr req_stdout_cap = seL4_GetMR(6);
                seL4_CPtr req_stderr_cap = seL4_GetMR(7);
                int pipe_id = (int)seL4_GetMR(8);
                seL4_Word stack_top = seL4_GetMR(9);

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

                ProcessControlBlock& pcb = pcbs[new_pid];
                memset(&pcb, 0, sizeof(ProcessControlBlock));
                pcb.pid = new_pid;
                strncpy(pcb.name, "shell_thread", 31);
                pcb.active = true;
                pcb.vspace = pcbs[sender_pid].vspace; // Потоки разделяют VSpace и CSpace родителя
                pcb.cspace = pcbs[sender_pid].cspace;

                seL4_CPtr new_tcb = alloc_and_track_cap(alloc, pcb);
                seL4_Untyped_Retype(normal_untyped, seL4_TCBObject, seL4_TCBBits, root_cnode, 0, 0, new_tcb, 1);

                // --- ГЕНЕРАЦИЯ УНИКАЛЬНОГО БЕЙДЖА ПОТОКА ---
                seL4_Word thread_badge = (sender_pid << 16) | new_pid;
                seL4_CPtr thread_badged_ep = alloc_and_track_cap(alloc, pcb);
                seL4_CNode_Mint(root_cnode, thread_badged_ep, seL4_WordBits,
                                root_cnode, ep, seL4_WordBits, seL4_AllRights, thread_badge);

                seL4_Word local_thread_fault_ep = 100 + new_pid; 
                // ИСПРАВЛЕНО: Превентивно удаляем старый Capability из слота, чтобы избежать ошибки
                // "Destination not empty" при повторном использовании PID потока.
                seL4_CNode_Delete(pcbs[sender_pid].cspace, local_thread_fault_ep, 8);
                seL4_CNode_Copy(pcbs[sender_pid].cspace, local_thread_fault_ep, 8,
                                root_cnode, thread_badged_ep, seL4_WordBits, seL4_AllRights);

                seL4_CPtr ipc_frame = alloc_and_track_cap(alloc, pcb);
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
                
                // НОВОЕ: Устанавливаем файловые дескрипторы для потока
                child_ipc_ptr->caps_or_badges[0] = req_stdin_cap;
                child_ipc_ptr->caps_or_badges[1] = req_stdout_cap;
                child_ipc_ptr->caps_or_badges[2] = req_stderr_cap;

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

                // --- Запуск контекста ---
                seL4_UserContext context = {0};
                context.pc = entry_point;   
                context.sp = stack_top; // Используем переданный указатель на стек
                // Передаем аргументы в новый поток через регистры x0, x1, x2
                context.x0 = arg0;
                context.x1 = arg1;
                context.x2 = arg2;

                context.tpidr_el0 = thread_ipc_vaddr + 3072;
                context.tpidrro_el0 = thread_ipc_vaddr + 3072;
                
                seL4_TCB_WriteRegisters(new_tcb, false, 0, sizeof(context) / sizeof(seL4_Word), &context);
                seL4_TCB_SetTLSBase(new_tcb, thread_ipc_vaddr + 3072);

                pcb.tcb = new_tcb;
                pcb.badged_ep = thread_badged_ep;
                pcb.thread_ipc_frame = ipc_frame;
                
                // Если мы создаем поток для пайпа, регистрируем его как писателя.
                // pipe_id приходит от вызывающего процесса через MR8 — обязательно
                // проверяем обе границы, иначе отрицательный индекс (кроме -1) даёт
                // запись за пределы статического массива g_pipes[MAX_PIPES].
                if (pipe_id >= 0 && pipe_id < MAX_PIPES) {
                    g_pipes[pipe_id].writer_pid = new_pid;
                }

                seL4_TCB_Resume(new_tcb);

                seL4_SetMR(0, new_pid);
                seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                break;
            }

            case 105: { // SYS_THREAD_EXIT
                int thread_pid = sender_badge & 0xFFFF;
                int parent_pid = (sender_badge >> 16) & 0xFFFF;

                if (thread_pid <= 0 || thread_pid >= 256 || !pcbs[thread_pid].active) {
                    break; // Invalid thread PID, ignore.
                }

                // Если поток был писателем в пайп, сообщаем читателю, что данные закончились (EOF)
                for (int i = 0; i < MAX_PIPES; i++) {
                    if (g_pipes[i].active && g_pipes[i].writer_pid == thread_pid) {
                        g_pipes[i].eof = true;
                        if (g_pipes[i].reader_reply_cap != 0) {
                            seL4_SetMR(0, 0); // EOF
                            seL4_Send(g_pipes[i].reader_reply_cap, seL4_MessageInfo_new(0, 0, 0, 1));
                            seL4_CNode_Delete(root_cnode, g_pipes[i].reader_reply_cap, seL4_WordBits);
                            alloc.free(g_pipes[i].reader_reply_cap);
                            g_pipes[i].reader_reply_cap = 0;
                        }
                    }
                }

                // Будим родительский процесс, если он ждал этот поток
                if (parent_pid > 0 && parent_pid < 256 && pcbs[parent_pid].active && pcbs[parent_pid].waiting_for == thread_pid) {
                    pcbs[parent_pid].waiting_for = 0;
                    seL4_SetMR(0, 0); // Success
                    seL4_Send(pcbs[parent_pid].reply_cap, seL4_MessageInfo_new(0, 0, 0, 1));
                    seL4_CNode_Delete(root_cnode, pcbs[parent_pid].reply_cap, seL4_WordBits);
                    alloc.free(pcbs[parent_pid].reply_cap);
                    pcbs[parent_pid].reply_cap = 0;
                }

                // Финальная очистка ресурсов потока
                ProcessControlBlock& pcb = pcbs[thread_pid];
                if (strncmp(pcb.name, "shell_thread", 12) == 0) {
                    seL4_TCB_Suspend(pcb.tcb);

                    // Отмапим IPC буфер потока из VSpace родителя
                    if (pcb.thread_ipc_frame) {
                        seL4_ARM_Page_Unmap(pcb.thread_ipc_frame);
                    }

                    // Уничтожаем и освобождаем все capabilities, принадлежащие потоку
                    for (int i = 0; i < pcb.cap_tracker.count; i++) {
                        seL4_CPtr cap_to_free = pcb.cap_tracker.caps[i];
                        seL4_CNode_Revoke(root_cnode, cap_to_free, seL4_WordBits);
                        seL4_CNode_Delete(root_cnode, cap_to_free, seL4_WordBits);
                        alloc.free(cap_to_free);
                    }
                    pcb.cap_tracker.count = 0;
                    
                    pcb.active = false;
                }
                // Не отвечаем на этот вызов, т.к. поток уничтожается
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
                        new_pid = spawn_process(app_name_and_args, elf_staging_buffer, elf_size, ep, med_ep, alloc, root_cnode, root_vspace, normal_untyped,
                                                shm_frames[0], 254, console_ep, timer_ep, blk_ep, console_ep, console_ep, console_ep,
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
                        // rootserver_shm_base — это ровно 4 страницы (16KB, shm_frames[4]).
                        // Резервируем запас на самую длинную возможную строку записи
                        // ("    " + PID + " [RUNNING] " + name[32] + "\n"), чтобы не выйти
                        // за пределы физических страниц при большом числе процессов.
                        if (offset > 16384 - 64) {
                            strcpy(shm + offset, "...\n");
                            offset += 4;
                            break;
                        }

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
                flush_rootserver_shm(); // иначе шелл может некэшируемо прочитать не эту таблицу, а что-то устаревшее (см. flush_rootserver_shm())
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
                    // Проверяем, не был ли этот процесс писателем в пайп
                    for (int i = 0; i < MAX_PIPES; i++) {
                        if (g_pipes[i].active && g_pipes[i].writer_pid == sender_pid) {
                            g_pipes[i].eof = true;
                            if (g_pipes[i].reader_reply_cap != 0) {
                                seL4_SetMR(0, 0); // EOF
                                seL4_Send(g_pipes[i].reader_reply_cap, seL4_MessageInfo_new(0, 0, 0, 1));
                                seL4_CNode_Delete(root_cnode, g_pipes[i].reader_reply_cap, seL4_WordBits);
                                alloc.free(g_pipes[i].reader_reply_cap);
                                g_pipes[i].reader_reply_cap = 0;
                            }
                        }
                    }

                    // Будим все процессы, которые ждали этот
                    for (int i = 1; i < 256; i++) {
                        if (pcbs[i].active && pcbs[i].waiting_for == sender_pid) {
                            pcbs[i].waiting_for = 0;
                            seL4_SetMR(0, 0);
                            seL4_Send(pcbs[i].reply_cap, seL4_MessageInfo_new(0, 0, 0, 1));
                            seL4_CNode_Delete(root_cnode, pcbs[i].reply_cap, seL4_WordBits);
                            alloc.free(pcbs[i].reply_cap);
                            pcbs[i].reply_cap = 0;
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

            case SYS_DRIVER_READY: {
                int drv = (sender_pid > 0) ? pcbs[sender_pid].is_driver : 0;
                // Печатаем "ready" для ЛЮБОГО настоящего драйвера (1..5,
                // включая wifi_driver — Фаза 4, Милстоун 4.1), чтобы он был
                // виден в логе наравне с остальными. Но в driver_ready[]/
                // all_drivers_ready() по-прежнему учитываются только 1..4 —
                // wifi всё ещё экспериментальный (см. RPI4_ENABLE_WIFI), и
                // зависание/провал его пробы не должно блокировать загрузку
                // остальных модулей и шелла.
                if (drv >= 1 && drv <= 5 && drv != 3) {
                    uart_puts("[ROOT] "); uart_puts(pcbs[sender_pid].name); uart_puts(" ready.\n");
                }
                if (drv == 5) {
                    g_wifi_driver_ready = true;
                }
                if (drv >= 1 && drv <= 4) {
                    driver_ready[drv] = true;

                    // Отпускаем shell, ждавший на SYS_WAIT_ALL_DRIVERS_READY — ОТДЕЛЬНЫМ
                    // Send на сохраненный reply-cap, а не seL4_Reply() (тот отвечал бы
                    // текущему вызывающему, то есть этому самому драйверу, а не шеллу).
                    if (all_drivers_ready() && driver_ready_wait_reply != 0) {
                        seL4_Send(driver_ready_wait_reply, seL4_MessageInfo_new(0, 0, 0, 0));
                        seL4_CNode_Delete(root_cnode, driver_ready_wait_reply, seL4_WordBits);
                        alloc.free(driver_ready_wait_reply);
                        driver_ready_wait_reply = 0;
                    }
                }
                seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 0)); // Отпускаем сам драйвер, приславший READY
                break;
            }

            case SYS_WAIT_ALL_DRIVERS_READY: {
                if (all_drivers_ready()) {
                    seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 0));
                } else {
                    // Не отвечаем сразу — сохраняем reply-cap шелла и продолжаем цикл.
                    // Ответим из case SYS_DRIVER_READY выше, когда готовы будут все 4.
                    driver_ready_wait_reply = alloc.alloc_slot();
                    seL4_CNode_SaveCaller(root_cnode, driver_ready_wait_reply, seL4_WordBits);
                }
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

            // Ручной жизненный цикл Wi-Fi (см. common.h — почему wifi_driver
            // больше не спавнится при загрузке). Все три сисколла ниже не
            // принимают имя процесса — рутсервер и так знает, что речь про
            // "wifi_driver" (единственный процесс с этим именем).
            case SYS_START_WIFI: {
                int existing_pid = -1;
                for (int i = 1; i < 256; i++) {
                    if (pcbs[i].active && strcmp(pcbs[i].name, "wifi_driver") == 0) { existing_pid = i; break; }
                }
                if (existing_pid != -1) {
                    seL4_SetMR(0, 1); // уже запущен
                } else if (wifi_cmd_recv_ep == 0) {
                    seL4_SetMR(0, (seL4_Word)-1); // не скомпилирован (RPI4_ENABLE_WIFI=false)
                } else {
                    g_wifi_driver_ready = false;
                    // wifi_irq_ntfn передаётся ДЕВЯТЫМ параметром (irq_handler
                    // слот) — тот же приём, что у blk_irq_ntfn: обычная
                    // capability на нотификацию, не настоящий IRQHandler (см.
                    // is_driver==5 блок выше и common.h/SYS_WIFI_IRQ_ACK).
                    int new_pid = spawn_process("wifi_driver", nullptr, 0, ep, med_ep, alloc, root_cnode, root_vspace, normal_untyped, shm_frames[0],
                                                5, console_ep, timer_ep, blk_ep, console_ep, console_ep, console_ep, 0, wifi_irq_ntfn, wifi_sdio_frame, nullptr,
                                                0, 0, wifi_cmd_recv_ep, 0);
                    seL4_SetMR(0, new_pid > 0 ? 0 : (seL4_Word)-1);
                }
                seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                break;
            }

            case SYS_STOP_WIFI: {
                int target_pid = -1;
                for (int i = 1; i < 256; i++) {
                    if (pcbs[i].active && strcmp(pcbs[i].name, "wifi_driver") == 0) { target_pid = i; break; }
                }
                if (target_pid == -1) {
                    seL4_SetMR(0, (seL4_Word)-1); // не запущен
                } else {
                    generic_recover_process(target_pid, ep, med_ep, alloc, root_cnode, root_vspace, normal_untyped,
                                            shm_frames[0], console_ep, timer_ep, blk_ep, /*respawn=*/false);
                    g_wifi_driver_ready = false;
                    seL4_SetMR(0, 0);
                }
                seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                break;
            }

            case SYS_WIFI_STATUS: {
                int target_pid = -1;
                for (int i = 1; i < 256; i++) {
                    if (pcbs[i].active && strcmp(pcbs[i].name, "wifi_driver") == 0) { target_pid = i; break; }
                }
                seL4_Word status;
                if (target_pid == -1) status = 0;       // не запущен
                else if (!g_wifi_driver_ready) status = 1; // запущен, ещё не дошёл до SYS_DRIVER_READY
                else status = 2;                          // готов принимать WIFI_CMD_*
                seL4_SetMR(0, status);
                seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                break;
            }

            case SYS_MMC_IRQ_ACK: { // Фаза 4.5, см. common.h — blk_driver уже снял девайсный статус-бит
                if (mmc_shared_irq_handler != 0) seL4_IRQHandler_Ack(mmc_shared_irq_handler);
                seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 0));
                break;
            }

            case SYS_WIFI_IRQ_ACK: { // Фаза 4.5, см. common.h — wifi_driver уже снял I_HMB_*/INTSTATUS на своей стороне
                if (mmc_shared_irq_handler != 0) seL4_IRQHandler_Ack(mmc_shared_irq_handler);
                seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 0));
                break;
            }

            default:
                seL4_SetMR(0, (seL4_Word)-1); seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                break;
        }
    }
    return 0;
}