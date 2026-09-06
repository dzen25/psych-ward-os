#include "h/common.h"
#include "h/allocator.h"
#include "h/uart.h"
#include "h/hw_timer.h"
#include "h/platform.h"

#include <sel4/sel4.h>
#include <sel4/benchmark_utilisation_types.h> // Фаза 6.1 (SMP): seL4_BenchmarkGetThreadUtilisation, см. SYS_TOP_STATS

extern "C" {
#include <cpio/cpio.h>
#include <elf/elf.h>
}
#include <string.h>

// Фаза 12 (см. ROADMAP.md) — проверка подписи файлов с диска перед
// исполнением/использованием, см. load_elf_from_disk() ниже. Заголовок сам
// оборачивает объявления в extern "C" при сборке как C++ — свою обёртку не
// добавляем.
#include "monocypher-ed25519.h"

extern char _cpio_archive[];
extern char _cpio_archive_end[];

// Фаза 12 (см. ROADMAP.md) — публичный ключ Ed25519, зашитый в rootserver.
// Это НЕ секрет (публичный ключ по определению) — приватный ключ живёт
// отдельно (`.signing-key` в корне репозитория, в .gitignore, никогда не
// попадает ни в git, ни на SD-карту). Сгенерирован tools/sign_elf/sign_elf
// genkey. Перевыпущен 2026-08-03 (см. issuse.txt/ROADMAP.md Фаза 12) —
// исходный ключ был безвозвратно утрачен из-за бага в sign_elf.c (писал на
// диск seed ПОСЛЕ вызова crypto_ed25519_key_pair(), которая сама затирает
// свой параметр seed[] изнутри — на диск улетали нули).
constexpr unsigned char OS_PUBLIC_KEY[32] = {
    0x34, 0x1d, 0xa4, 0x22, 0x22, 0x1a, 0xdc, 0x88,
    0x11, 0x2b, 0xd7, 0xfe, 0x23, 0x8a, 0xb5, 0x25,
    0xdf, 0x3c, 0x14, 0x24, 0x5a, 0xbe, 0x18, 0xe9,
    0x8e, 0x82, 0xb7, 0xc4, 0x93, 0xda, 0xaa, 0x76
};

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
// 5-я страница добавлена ради изолированного Wi-Fi TX/RX data-мейлбокса
// (см. h/platform.h WIFI_SHM_TX_DATA_OFFSET/RX_DATA_OFFSET) — старые 4
// страницы (0-16383) пересекались с GENET rx_buffer_offsets[] (живой баг,
// см. situation.txt); SHM_TOTAL_SIZE в shell.cpp (VFS/`ps`) НАРОЧНО остаётся
// 16384, не 20480 — 5-я страница не должна быть доступна файловому протоколу.
// 6-я страница (Фаза 5, аудит перед capability-изоляцией) — под собственный
// staging-буфер blk_driver'а (BLK_SHM_STAGING_OFFSET, platform.h), который
// раньше жил на 3-й странице и арифметически пересекался с GENET
// rx_buffer_offsets[] — тот же класс бага, что уже чинили для Wi-Fi.
// 7-я и 8-я страницы (Фаза 5.3, read-only права) — старая единая Wi-Fi
// страница (4) разбита на 3: control-plane остаётся на 4, link-state
// переехал на новую 5-ю (по счёту здесь — 6-й индекс массива), TX/RX-
// мейлбокс+канарейка на новую 6-ю (7-й индекс) — иначе процессу, которому
// нужен только TX/RX-мейлбокс (net_driver), пришлось бы давать доступ и к
// странице с паролем просто потому что они были на одной физической
// странице. blk_driver'ов staging съехал с индекса 5 на индекс 7 (см.
// platform.h). Итого 8 страниц вместо 6.
// 9-я страница (Фаза 14, Milestone 8) — собственный staging-буфер
// usb_driver'а (USB_SHM_STAGING_OFFSET, platform.h), тот же приём, что и
// BLK_SHM_STAGING_OFFSET у blk_driver — отдельная страница вместо шаринга
// с чужим staging, чтобы не повторить класс бага с GENET/BLK-пересечением.
static seL4_CPtr shm_frames[SHM_TOTAL_PAGES]; // Капы всех страниц SHM (размер — из platform.h, см. раскладку там)

// НАЙДЕНО НА ЖИВОМ ЖЕЛЕЗЕ (Фаза 9.B): каждый из четырёх мест, где root
// читает .elf/текстовый файл целиком в память (SYS_EXEC, SYS_START_WIFI,
// generic_recover_process при респавне wifi_driver, start_init_services
// для сервисов из init.conf) заводил СВОЙ собственный статический буфер на
// 1МБ — итого 4МБ лишнего BSS в образе root'а. Это раздуло физический
// футпринт rootserver-образа настолько, что перестало хватать untyped-
// памяти на CNode-слоты при спавне: "Untyped Retype: Slot #0 in
// destination window non-empty" на ровном месте при загрузке, до первого
// же спавна процесса, и итоговый "Failed to allocate normal RAM for SHM!".
// root однопоточный и обрабатывает syscall'ы строго последовательно —
// одного общего буфера достаточно, ни один из этих путей не выполняется
// параллельно с другим.
static char g_elf_load_buffer[1024 * 1024];

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
    for (int i = 0; i < SHM_TOTAL_PAGES; i++) {
        seL4_ARM_Page_CleanInvalidate_Data(shm_frames[i], 0, 4096);
    }
}

// Фаза 6.1 (SMP): дописывает десятичное представление val в buf (без
// нуль-терминатора), возвращает число записанных символов — тот же приём
// реверса цифр, что уже инлайнился в SYS_PS для pid, вынесен в helper, т.к.
// в SYS_TOP_STATS нужен трижды на строку (PID/CORE/%CPU) для каждого процесса.
static int append_udec(char *buf, uint64_t val) {
    if (val == 0) { buf[0] = '0'; return 1; }
    char tmp[20];
    int j = 0;
    while (val > 0) { tmp[j++] = (char)('0' + (val % 10)); val /= 10; }
    int n = j;
    while (j > 0) *buf++ = tmp[--j];
    return n;
}

// Фаза 6.1 (продолжение, см. ROADMAP.md): та же цифра, но прижатая вправо в
// поле фиксированной ширины (пробелами слева) — для выровненных таблиц
// `top`/`top -l`. Если число шире width — не обрезаем, просто печатаем как
// есть (ширина колонки в таком случае "поедет", но данные не потеряются).
static int append_udec_width(char *buf, uint64_t val, int width) {
    char tmp[20];
    int n = append_udec(tmp, val);
    int pad = (width > n) ? (width - n) : 0;
    for (int i = 0; i < pad; i++) buf[i] = ' ';
    for (int i = 0; i < n; i++) buf[pad + i] = tmp[i];
    return pad + n;
}

// Фаза 8 (`df`) — ширина колонки должна считаться в ВИДИМЫХ символах, не
// байтах: кириллица в UTF-8 — 2 байта на символ, а строки заголовков df
// ("ИСПОЛЬЗ", "СВОБОДНО" и т.д.) первыми в этом файле смешали кириллицу с
// паддингом по ширине — на живом железе это давало сдвиг колонок (паддинг
// считался на N байт короче видимой строки, N = число кириллических
// символов). Байты-продолжения UTF-8 (0x80-0xBF) не считаются отдельным
// символом.
static int utf8_visual_len(const char *s) {
    int chars = 0;
    for (int i = 0; s[i]; i++) if (((unsigned char)s[i] & 0xC0) != 0x80) chars++;
    return chars;
}

// Та же логика, но для строк (заголовки столбцов и "?" на месте
// отсутствующих данных) — чтобы заголовок и значения выравнивались по
// одной и той же ширине колонки. Паддинг считается по utf8_visual_len(),
// копируются же ВСЕ байты строки как есть.
static int append_str_width(char *buf, const char *s, int width) {
    int nbytes = 0;
    while (s[nbytes]) nbytes++;
    int pad = (width > utf8_visual_len(s)) ? (width - utf8_visual_len(s)) : 0;
    for (int i = 0; i < pad; i++) buf[i] = ' ';
    for (int i = 0; i < nbytes; i++) buf[pad + i] = s[i];
    return pad + nbytes;
}

// Фаза 8 (`df`) — тот же приём, что append_str_width(), но выравнивание
// СЛЕВА (паддинг пробелами СПРАВА) — колонки "имя тома"/"точка
// монтирования" в df прижаты влево, а не вправо, как числовые столбцы.
static int append_str_left_width(char *buf, const char *s, int width) {
    int n = 0;
    while (s[n]) { buf[n] = s[n]; n++; }
    int pad = (width > utf8_visual_len(s)) ? (width - utf8_visual_len(s)) : 0;
    for (int i = 0; i < pad; i++) buf[n + i] = ' ';
    return n + pad;
}

// Фаза 8 (`df -h`) — человекочитаемый размер (степени 1024), без float —
// целочисленное масштабирование. НАЙДЕНО НА ЖИВОМ ЖЕЛЕЗЕ: раньше дробная
// часть показывалась ВСЕГДА ("28.8G") — 5 символов, вылезает за отведённые
// в таблице 4 и ломает выравнивание всех строк. Реальный coreutils `df -h`
// показывает дробную часть, ТОЛЬКО когда целая часть однозначная (6.3G, но
// 80G/233G без точки) — так значение всегда укладывается в ≤4 символа
// (3 цифры/2 цифры+точка+цифра + буква суффикса). buf НЕ нуль-
// терминируется (тот же контракт, что у append_udec).
static int append_human_size(char *buf, uint64_t bytes) {
    static const char suffixes[] = {'B', 'K', 'M', 'G', 'T'};
    int idx = 0;
    uint64_t divisor = 1;
    while (bytes / divisor >= 1024 && idx < 4) { divisor *= 1024; idx++; }
    uint64_t value = bytes / divisor;
    if (value >= 1000 && idx < 4) { divisor *= 1024; idx++; value = bytes / divisor; } // не более 3 цифр в целой части

    int n;
    if (idx > 0 && value < 10) {
        uint64_t frac = ((bytes - value * divisor) * 10) / divisor;
        n = append_udec(buf, value);
        buf[n++] = '.';
        buf[n++] = (char)('0' + frac);
    } else {
        n = append_udec(buf, value);
    }
    buf[n++] = suffixes[idx];
    return n;
}

// Фаза 8 (`df`) — одна строка таблицы, общая для SD и каждого USB-тома
// (см. case SYS_DF_STATS ниже). human=true — единицы как у `df -h` в
// Linux (append_human_size), иначе — целые МиБ, как раньше.
static int df_format_row(char *buf, const char *name, uint64_t total_bytes,
                          uint64_t free_bytes, const char *mount, bool human) {
    uint64_t used_bytes = (total_bytes > free_bytes) ? (total_bytes - free_bytes) : 0;
    int use_pct = (total_bytes > 0) ? (int)((used_bytes * 100) / total_bytes) : 0;

    // Позиции колонок скопированы с реального `df -h` (coreutils) —
    // пользователь прислал точный образец, см. case SYS_DF_STATS ниже.
    int offset = 0;
    offset += append_str_left_width(buf + offset, name, 29);

    char tmp[24];
    int n;

    // issuse.txt (найдено 2026-08-24) — без -h цифры печатались голыми
    // МиБ без единицы измерения (напр. "29586") — неотличимо на глаз от
    // байт/КБ, выглядело как явно неверный, слишком маленький размер.
    // Суффикс "M" убирает двусмысленность, не трогая -h путь (тот уже сам
    // выбирает K/M/G/T через append_human_size). Ширины колонок раздвинуты
    // (4→6/4→6/5→7), чтобы вместить суффикс без потери выравнивания на
    // реалистичных объёмах (карта до сотен ГиБ = до 6 цифр + буква).
    n = human ? append_human_size(tmp, total_bytes) : append_udec(tmp, total_bytes / (1024 * 1024));
    if (!human) tmp[n++] = 'M';
    tmp[n] = '\0'; offset += append_str_width(buf + offset, tmp, 6);

    buf[offset++] = ' '; buf[offset++] = ' ';
    n = human ? append_human_size(tmp, used_bytes) : append_udec(tmp, used_bytes / (1024 * 1024));
    if (!human) tmp[n++] = 'M';
    tmp[n] = '\0'; offset += append_str_width(buf + offset, tmp, 6);

    buf[offset++] = ' ';
    n = human ? append_human_size(tmp, free_bytes) : append_udec(tmp, free_bytes / (1024 * 1024));
    if (!human) tmp[n++] = 'M';
    tmp[n] = '\0'; offset += append_str_width(buf + offset, tmp, 7);

    buf[offset++] = ' ';
    offset += append_udec_width(buf + offset, (uint64_t)use_pct, 3);
    buf[offset++] = '%';

    buf[offset++] = ' ';
    offset += append_str_left_width(buf + offset, mount, 0); // 0 = без паддинга, последняя колонка
    buf[offset++] = '\n';
    buf[offset] = '\0';
    return offset;
}

// Фаза 8 (мониторинг ресурсов, `free`) — учёт RAM, выданной из
// normal_untyped. seL4-контекст: seL4_CNode_Revoke/Delete при завершении
// процесса освобождает КЭПЫ, но НЕ возвращает память самому untyped'у
// (простого bump-allocator'а, здесь нет per-процессного under-untyped) —
// used-память в этой ОС монотонно растёт за сессию, каждый спавн процесса
// навсегда откусывает кусок RAM. g_ram_bytes_total считается один раз при
// выборе normal_untyped/low_untyped (см. main() ниже), g_ram_bytes_used
// растёт по мере ram_retype()-вызовов.
static uint64_t g_ram_bytes_used = 0;
static uint64_t g_ram_bytes_total = 0;

// issuse.txt (найдено при реализации free) — раньше ВСЯ RAM-аллокация шла
// из ОДНОГО (самого большого) untyped-региона; seL4 отдаёт RAM не единым
// блоком, а набором выровненных по степени двойки блоков (buddy-разбиение
// физических диапазонов из boot info) — на этой плате самый большой блок
// оказался ровно 1024 МиБ, хотя физической RAM 3.9 GiB. Остальные блоки
// поменьше реально существуют в info->untypedList[], просто раньше никем
// не трогались. g_ram_pool[0] — тот же normal_untyped, что и раньше (см.
// main(), заполнение пула) — типовой путь не меняется, остальные записи —
// запасные блоки, в которые ram_retype() переходит, когда текущий кап
// исчерпан (seL4_NotEnoughMemory). low_untyped (VideoCore mbox, <1ГиБ) в
// пул НЕ входит — отдельный, как и раньше.
struct RamPoolEntry { seL4_CPtr cap; uint64_t size; };
constexpr int RAM_POOL_MAX = 32;
static RamPoolEntry g_ram_pool[RAM_POOL_MAX];
static int g_ram_pool_count = 0;
static int g_ram_pool_idx = 0; // текущая позиция марша — только вперёд, назад не возвращается (untyped bump-allocator ничего не отдаёт обратно)

// Реальный размер объекта в байтах по типу — те же *Bits-константы,
// которыми оперирует сам kernel при retype (см. sel4/sel4_arch/constants.h),
// не захардкоженные числа. BIT()-макрос из libutils сюда не тянем (main.cpp
// его нигде больше не использует) — просто явный сдвиг 1ULL. Покрывает
// ровно те типы, что встречаются в вызовах ram_retype() ниже.
// if/else, а не switch — на этой конфигурации seL4 несколько ARM
// page-table-типов (PageTable/PageDirectory/PageUpperDirectory) физически
// совпадают по значению (все объекты уровней трансляции — одна и та же
// 4КиБ-страница), что ломает switch (duplicate case value); if-цепочка с
// такими же дублирующимися значениями компилируется нормально.
static uint64_t object_size_bytes(seL4_Word type, seL4_Word size_bits) {
    if (type == seL4_TCBObject)          return 1ULL << seL4_TCBBits;
    if (type == seL4_EndpointObject)     return 1ULL << seL4_EndpointBits;
    if (type == seL4_NotificationObject) return 1ULL << seL4_NotificationBits;
    if (type == seL4_CapTableObject)     return 1ULL << (size_bits + seL4_SlotBits);
    if (type == seL4_ARM_SmallPageObject ||
        type == seL4_ARM_PageTableObject ||
        type == seL4_ARM_PageDirectoryObject ||
        type == seL4_ARM_PageUpperDirectoryObject ||
        type == seL4_ARM_PageGlobalDirectoryObject)
        return 1ULL << seL4_PageBits;
    return 0; // неизвестный тип — не считаем
}

// issuse.txt п.3 — переиспользуемые RAM-арены для /sbin-команд (обычный
// exec, is_driver 253/254 — см. SYS_EXEC-диспетчер ниже). Устраняет
// монотонный рост Used в `free`: вместо retype напрямую из общего пула
// (который никогда не отдаёт память назад), команда получает выделенный
// Untyped-регион; на выходе (SYS_EXIT/SYS_KILL/generic_recover_process)
// не удаляем объекты по одному, а делаем seL4_CNode_Revoke НАД САМОЙ
// АРЕНОЙ — это и уничтожает все её объекты, и (в отличие от revoke
// дочернего объекта) сбрасывает free index арены на 0, так что капа
// арены реально переиспользуется для следующей команды. Драйверы/
// сервисы/shell — НЕ участвуют (g_current_arena выставляется только
// вокруг конкретного вызова spawn_process() для команд), их retype идут
// через тот же ram_retype(), просто с g_current_arena==0 — поведение не
// меняется ни на бит.
//
// issuse.txt №62 (ЗАКРЫТО 2026-08-16) — подтверждённая причина: именно
// переиспользование этой арены (весь address space команды — CNode/
// VSpace/TCB/страницы ELF — из одного и того же физического 1МиБ между
// командами) давала спорадическую порчу TLS-образа при старте. hw-тест
// (см. sbin/tests/stresstest.cpp): 350 спавнов /sbin-команд подряд (50+300
// итераций, 2 отдельных прогона) с ареной выключенной — 0 крашей/зависаний,
// раньше проблема ловилась уже в пределах 5-10 команд. Арену тогда выключили
// целиком, обратная сторона — issuse.txt №65: `free`/Used снова растёт
// монотонно на КАЖДУЮ команду.
//
// issuse.txt №65 (2026-08-24) — реализован путь, намеченный в самой
// находке: арена переиспользуется ТОЛЬКО под data/ELF/stack/IPC-фреймы
// команды (seL4_ARM_SmallPageObject-ретайпы в spawn_process() — сама
// TLS-страница, содержимое ELF, стек), а служебные kernel-объекты (CNode,
// VSpace/PUD/PD/PT ×2, TCB — см. force_pool=true у соответствующих
// ram_retype() внутри spawn_process()) всегда из общего монотонного
// g_ram_pool, как и раньше. Логика: seL4_Untyped_Retype ВСЕГДА зануляет
// память нового объекта (kernel-инвариант, иначе была бы утечка данных
// между capability-доменами) — простое переиспользование физических байт
// само по себе не может оставить "мусор" от предыдущей команды. Значит
// порча №62 была не из зануляемого содержимого, а из побочного эффекта
// самого факта переиспользования РОВНО ТЕХ ЖЕ физических страниц под
// CNode/VSpace/TCB следующей команды (детальный механизм не диагностирован
// — решение сознательно обходит его, а не объясняет). Гипотеза проверяется
// стресс-тестом (см. stresstest.elf) с ареной снова включённой именно в
// этом урезанном виде.
constexpr bool CMD_ARENA_ENABLED = true;
static seL4_CPtr g_current_arena = 0; // 0 = обычный путь через g_ram_pool, как раньше
static uint64_t g_current_arena_bytes = 0; // сколько байт ТЕКУЩАЯ арена уже отдала — для точного отката g_ram_bytes_used при освобождении

constexpr uint8_t CMD_ARENA_SIZE_BITS = 20; // 1 МиБ — самый большой сегодняшний /sbin-бинарник ~120КБ + ~48КБ фикс. оверхед (CNode/VSpace/стек/IPC), запас ×5
constexpr int CMD_ARENA_POOL_MAX = 16; // одновременно работающих команд реально 1-2, с большим запасом (конвейеры)
static seL4_CPtr g_cmd_arena_free[CMD_ARENA_POOL_MAX]; // стек свободных (уже созданных, сейчас не используемых) арен
static int g_cmd_arena_free_count = 0;
static int g_cmd_arena_created_count = 0; // сколько арен вообще когда-либо ретайпнуто из глобального пула — растёт лениво, никогда не уменьшается

// Обёртка над seL4_Untyped_Retype — на seL4_NoError прибавляет байты в
// g_ram_bytes_used. ВСЕ вызовы retype от normal_untyped в этом файле идут
// через неё (вызовы через low_untyped — device-память для драйверов — НЕ
// учитываются, это не RAM "используемая процессами"). Параметр `untyped`
// намеренно ИГНОРИРУЕТСЯ (оставлен в сигнатуре, чтобы ни один из 46
// сайтов вызова не менять) — реальный кап берётся из g_current_arena (если
// выставлена — см. выше) либо из g_ram_pool по g_ram_pool_idx: на
// g_ram_pool[0] (тот же normal_untyped, что раньше был единственным
// вариантом) типовой путь побайтово не меняется; если он исчерпан
// (seL4_NotEnoughMemory), переходим на следующий блок пула и повторяем
// retype ТОЙ ЖЕ команды — вызывающий код ничего не замечает, кроме того
// что теперь может получить успех там, где раньше был бы отказ.
// issuse.txt №65 — force_pool=true заставляет retype идти из общего
// g_ram_pool, ДАЖЕ если сейчас активна командная арена (g_current_arena) —
// используется в spawn_process() для служебных kernel-объектов (CNode/
// VSpace-уровни/TCB), см. комментарий у CMD_ARENA_ENABLED выше. По
// умолчанию false — все существующие сайты вызова (ELF/стек/IPC-страницы и
// весь остальной код файла, не связанный с командной ареной) не меняют
// поведение ни на бит.
static inline seL4_Error ram_retype(seL4_CPtr /*untyped, игнорируется*/, seL4_Word type, seL4_Word size_bits,
                                     seL4_CPtr root, seL4_Word node_index, seL4_Word node_depth,
                                     seL4_Word slot, seL4_Word num_objects, bool force_pool = false) {
    if (g_current_arena != 0 && !force_pool) {
        seL4_Error err = seL4_Untyped_Retype(g_current_arena, type, size_bits, root, node_index, node_depth, slot, num_objects);
        if (err == seL4_NoError) {
            uint64_t bytes = object_size_bytes(type, size_bits) * num_objects;
            g_ram_bytes_used += bytes;
            g_current_arena_bytes += bytes;
            return err;
        }
        if (err != seL4_NotEnoughMemory) return err; // настоящая ошибка — возвращаем как есть
        // Арена неожиданно переполнена (не должно случаться при 1МиБ и
        // сегодняшних размерах бинарников) — честно откатываемся на общий
        // пул этим же вызовом, не роняем спавн молча.
    }
    while (g_ram_pool_idx < g_ram_pool_count) {
        seL4_Error err = seL4_Untyped_Retype(g_ram_pool[g_ram_pool_idx].cap, type, size_bits,
                                              root, node_index, node_depth, slot, num_objects);
        if (err == seL4_NoError) {
            g_ram_bytes_used += object_size_bytes(type, size_bits) * num_objects;
            return err;
        }
        if (err != seL4_NotEnoughMemory) return err; // настоящая ошибка — не "кончилось место", не лечим переходом на другой блок
        g_ram_pool_idx++; // текущий блок исчерпан НАВСЕГДА — пробуем следующий
    }
    return seL4_NotEnoughMemory;
}

// Приобретение/освобождение переиспользуемой арены — см. блок выше.
// alloc/root_cnode передаются явно (те же параметры, что уже есть в
// main()/spawn_process(), никакого нового глобального состояния для них).
static seL4_CPtr acquire_cmd_arena(PsychAllocator &alloc, seL4_CPtr root_cnode) {
    if (g_cmd_arena_free_count > 0) return g_cmd_arena_free[--g_cmd_arena_free_count];
    if (g_cmd_arena_created_count >= CMD_ARENA_POOL_MAX) return 0; // пул арен исчерпан — вызывающий откатится на общий путь
    seL4_CPtr arena_slot = alloc.alloc_slot();
    if (arena_slot == 0) return 0;
    if (ram_retype(0, seL4_UntypedObject, CMD_ARENA_SIZE_BITS, root_cnode, 0, 0, arena_slot, 1) != seL4_NoError) return 0;
    g_cmd_arena_created_count++;
    return arena_slot;
}
static void release_cmd_arena(seL4_CPtr root_cnode, seL4_CPtr arena_cap) {
    seL4_CNode_Revoke(root_cnode, arena_cap, seL4_WordBits); // уничтожает все объекты арены И сбрасывает её free index на 0
    if (g_cmd_arena_free_count < CMD_ARENA_POOL_MAX) g_cmd_arena_free[g_cmd_arena_free_count++] = arena_cap;
}

// Индексы SHM-страниц (Фаза 5.3) — используются в shm_pages_mask_for_role()/
// shm_page_readonly_for_role() ниже и должны совпадать с раскладкой в
// h/platform.h (WIFI_SHM_*_OFFSET/BLK_SHM_STAGING_OFFSET).
constexpr int SHM_PAGE_VFS            = 0; // VFS path/data + GENET TX-staging (0x280)
constexpr int SHM_PAGE_NET_MAILBOX    = 1; // net_vfs_lock(4096) + per-iface net-мейлбоксы
constexpr int SHM_PAGE_GENET_RX0      = 2; // GENET RX-кольцо, буферы 0-1
constexpr int SHM_PAGE_GENET_RX1      = 3; // GENET RX-кольцо, буферы 2-3 (физически смежна с GENET_RX0)
constexpr int SHM_PAGE_WIFI_CONTROL   = 4; // Wi-Fi control-plane: SSID/PASS/VERBOSE
constexpr int SHM_PAGE_WIFI_LINKSTATE = 5; // Wi-Fi link-state: LINK_STATE/MAC/REASON
constexpr int SHM_PAGE_WIFI_MAILBOX   = 6; // Wi-Fi TX/RX data-мейлбокс + канарейка
constexpr int SHM_PAGE_BLK_STAGING    = 7; // приватный staging-буфер blk_driver'а
constexpr int SHM_PAGE_USB_STAGING    = 8; // приватный staging-буфер usb_driver'а (Фаза 14, Milestone 8)

// Фаза 5.2 (least-privilege, см. situation.txt): раньше SYS_SHM_GET отдавал
// ВСЕ страницы ЛЮБОМУ активному процессу (проверялся только pcbs[pid].active,
// не то, какой это процесс) — включая запущенные через `exec <file>`
// (is_driver==254), которые ничем не отличались от системных драйверов с
// точки зрения прав и могли прочитать Wi-Fi-пароль/сетевое состояние/GENET
// DMA-буферы. Ниже — явный per-роль список разрешённых страниц (бит i =
// shm_frames[i]), выданный по аудиту фактического использования каждого
// канала (см. план в situation.txt/ROADMAP.md Фаза 5). Права внутри
// разрешённых страниц — см. shm_page_readonly_for_role() ниже (Фаза 5.3):
// по умолчанию RW, кроме перечисленных там комбинаций.
// Права на ФИКСИРОВАННЫЕ страницы 0..6 (см. раскладку в platform.h). Только
// они и остались битовой маской: растущие области с масками несовместимы —
// в uint32_t помещается 32 страницы, а их теперь сотня.
static uint32_t shm_fixed_mask_for_role(int is_driver) {
    switch (is_driver) {
        case 0: return 0b0110011; // shell: VFS-путь(0) + net-мейлбоксы(1) + Wi-Fi control(4, пишет SSID/пароль) + link-state(5, читает для "wifi status")
        case 1: return 0b0000001; // uart_driver: только страница пути(0) — легаси fallback для SYS_PUTS
        case 2: return 0b0000000; // timer_driver: SHM вообще не использует
        case 3: return 0b0000001; // blk_driver: путь(0); свой staging — отдельной областью ниже
        case 4: return 0b1101111; // net_driver: VFS/lock(0,1) + GENET(2,3) + Wi-Fi link-state(5) + TX/RX-мейлбокс(6) — control-plane(4) ему не нужен, там только пароль
        case 5: return 0b1110011; // wifi_driver: путь(0) + net_vfs_lock(1) + Wi-Fi control(4) + link-state(5) + TX/RX-мейлбокс(6)
        case 6: return 0b0000001; // usb_driver: путь(0); свой staging — отдельной областью ниже
        case 253: return 0b0000001; // доверенные /sbin-утилиты: путь(0)
        default: return 0;        // exec-процессы (254) и всё прочее — fail closed, как и было
    }
}

// Нужна ли роли ОБЛАСТЬ ПОЛЕЗНОЙ НАГРУЗКИ VFS. Отдельно от маски выше:
// это уже не одна страница, а прогон из SHM_VFS_PAYLOAD_PAGES.
// Выдаётся только тем, кто реально передаёт содержимое файлов:
// шелл, оба файловых драйвера, wifi_driver (читает прошивку/NVRAM/CLM),
// net_driver (VFS-путь) и доверенные /sbin-утилиты. Недоверенному
// exec'у (254) — по-прежнему ничего.
static bool shm_role_needs_vfs_payload(int is_driver) {
    return is_driver == 0 || is_driver == 3 || is_driver == 4 ||
           is_driver == 5 || is_driver == 6 || is_driver == 253;
}

// Единая точка ответа "положена ли роли эта страница". Заменила битовую
// маску: страниц теперь больше, чем битов в маске, и растущие области
// описываются диапазоном, а не перечислением.
static bool shm_page_allowed_for_role(int is_driver, int page) {
    if (page < 0 || page >= SHM_TOTAL_PAGES) return false;
    if (page < SHM_FIXED_PAGES) return (shm_fixed_mask_for_role(is_driver) >> page) & 1u;
    // Страницы запаса (SHM_FIXED_RESERVE_PAGES) пока не выдаются никому —
    // они существуют только чтобы будущая фиксированная страница не
    // сдвигала адреса растущих областей.
    if (page < SHM_DYNAMIC_FIRST_PAGE) return false;
    if (page < SHM_PAGE_BLK_STAGING_FIRST) return shm_role_needs_vfs_payload(is_driver);
    if (page < SHM_PAGE_USB_STAGING_FIRST) return is_driver == 3; // staging blk_driver — только ему
    return is_driver == 6;                                        // staging usb_driver — только ему
}

// Есть ли у роли хоть одна страница — для fail-closed проверки в SYS_SHM_GET.
static bool shm_role_has_any_pages(int is_driver) {
    for (int i = 0; i < SHM_TOTAL_PAGES; i++) {
        if (shm_page_allowed_for_role(is_driver, i)) return true;
    }
    return false;
}


// Фаза 5.3: на ARM НЕТ write-only страниц (seL4_CapRights_new с read=0,write=1
// даёт VMKernelOnly — вообще никакого доступа, проверено по
// kernel/src/arch/arm/64/kernel/vspace.c:maskVMRights) — сокращать можно
// только RW→RO, и только там, где роль ДЕЙСТВИТЕЛЬНО никогда не пишет на
// страницу. Значения ниже — по аудиту:
// - uart_driver на VFS: только легаси-чтение offset 0 для SYS_PUTS.
// - net_driver на GENET RX: пишет только DMA-железо по физическому адресу
//   (в обход capability), net_driver только читает принятые кадры.
// - net_driver и shell на Wi-Fi link-state: оба только читают (net_driver —
//   состояние линка, shell — для "wifi status"); пишет туда только
//   wifi_driver (и root через SYS_STOP_WIFI, но это его собственная кэшируемая
//   карта, не через эту capability-систему вообще).
// - wifi_driver на Wi-Fi control-plane: раньше сам занулял пароль после
//   использования — теперь это делает shell (у него и так RW на этой
//   странице, он же пароль изначально туда и пишет), см. shell.cpp
//   обработчик WIFI_CMD_CONNECT. wifi_driver стал чистым читателем.
static bool shm_page_readonly_for_role(int is_driver, int page_idx) {
    switch (is_driver) {
        case 1: return page_idx == SHM_PAGE_VFS;
        case 4: return page_idx == SHM_PAGE_GENET_RX0 || page_idx == SHM_PAGE_GENET_RX1
                    || page_idx == SHM_PAGE_WIFI_LINKSTATE;
        case 0: return page_idx == SHM_PAGE_WIFI_LINKSTATE;
        case 5: return page_idx == SHM_PAGE_WIFI_CONTROL;
        default: return false;
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

    // Выравниваем watermark до целевого адреса. Заполняем самыми
    // крупными страницами, какие позволяет текущее выравнивание watermark'а
    // (Восемнадцатая попытка, см. ROADMAP.md) — для 0x600000000 (высокое
    // PCIe-окно U-Boot'а) разрыв от начала его Untyped-региона составляет
    // 8GB; по 4KB это 2 миллиона retype-вызовов (гарантированный
    // CSlot-exhaustion/зависание при загрузке — тот же класс бага, что
    // уже ловили в Первой попытке, см. KernelRootCNodeSizeBits, только на
    // 3 порядка хуже). seL4_ARM_HugePageObject (1GB) для device-памяти
    // не имеет особых ограничений (Arch_isFrameType/Arch_createObject
    // трактуют его наравне с SmallPage/LargePage, см. kernel/src/arch/
    // arm/64/object/objecttype.c) — ядро само выравнивает свой
    // внутренний free-pointer под размер объекта (kernel/src/object/
    // untyped.c:alignedFreeRef), так что условие "watermark кратен
    // размеру страницы" здесь просто выбирает самый эффективный шаг,
    // не обходит никакую реальную проверку. Для уже существующих
    // маленьких разрывов (UART/EMMC/WIFI_SDIO и т.п., все на
    // невыровненных под 1GB/2MB адресах) условия ниже не сработают и
    // поведение остаётся прежним (4KB-шаг, как раньше).
    while (untyped_watermarks[idx] < target_paddr) {
        uint64_t remaining = target_paddr - untyped_watermarks[idx];
        seL4_CPtr dummy = alloc.alloc_slot();
        if ((untyped_watermarks[idx] % (1ULL << 30)) == 0 && remaining >= (1ULL << 30)) {
            seL4_Untyped_Retype(untyped_cap, seL4_ARM_HugePageObject, 0, root_cnode, 0, 0, dummy, 1);
            untyped_watermarks[idx] += (1ULL << 30);
        } else if ((untyped_watermarks[idx] % (1ULL << 21)) == 0 && remaining >= (1ULL << 21)) {
            seL4_Untyped_Retype(untyped_cap, seL4_ARM_LargePageObject, 0, root_cnode, 0, 0, dummy, 1);
            untyped_watermarks[idx] += (1ULL << 21);
        } else {
            seL4_Untyped_Retype(untyped_cap, seL4_ARM_SmallPageObject, 0, root_cnode, 0, 0, dummy, 1);
            untyped_watermarks[idx] += 4096;
        }
    }

    seL4_CPtr frame = alloc.alloc_slot();
    seL4_Untyped_Retype(untyped_cap, seL4_ARM_SmallPageObject, 0, root_cnode, 0, 0, frame, 1);
    untyped_watermarks[idx] += 4096;
    return frame;
}

// Было 256 (хватало с запасом: wifi_driver со своими статическими буферами
// прошивки/NVRAM, ~708KB, сам по себе требует ~180-200 отслеживаемых ELF-
// страничных фреймов). Фаза 14 (USB): изначально планировался xHCI BAR на
// 1MB (256 фрейм-кап) — живое железо показало, что реальный BAR всего 4KB
// (см. RPI4_XHCI_SIZE, platform.h), так что фактическая потребность куда
// скромнее (1 страница MMIO + ~23 DMA-страницы). Бюджет оставлен подня-
// тым — запас на будущие фазы (класс-драйверам понадобится больше).
#define MAX_TRACKED_CAPS 640

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
    // issuse.txt №64 (root-caused и исправлено 2026-08-23) — PID родителя
    // для потока, созданного через SYS_CLONE (см. case SYS_CLONE ниже).
    // Раньше кодировался ПРЯМО В БЕЙДЖЕ ((parent_pid<<16)|new_pid) — это и
    // была настоящая причина зависания: биты 16-19 составного бейджа
    // случайно (для БОЛЬШИНСТВА PID) совпадали с диапазоном
    // DRIVER_LIVENESS_*_BADGE (0x10000-0x80000, common.h), который
    // диспетчер root'а (см. "sender_badge >= DRIVER_LIVENESS_BLK_BADGE")
    // безусловно перехватывает как heartbeat-тик и делает `continue` — ДО
    // switch(cmd), seL4_Reply() никогда не вызывается. Само IPC ядром
    // доставлялось корректно (проверено JTAG: cap_type=endpoint,
    // BlockedOnReply, не BlockedOnSend) — root просто ошибочно
    // игнорировал сообщение. Обычное поле PCB вместо кодирования в
    // бейдж — тот же класс данных, что is_driver/name/etc, никакого
    // риска коллизии с ЛЮБЫМ будущим диапазоном бейджей. 0 — не поток
    // (обычный процесс) или родитель неизвестен.
    int parent_pid = 0;

    // --- ГЕНЕРИЧЕСКИЕ МЕТАДАННЫЕ ДЛЯ АВТОПЕРЕЗАПУСКА ---
    int is_driver;
    seL4_CPtr irq_ntfn;
    seL4_CPtr irq_handler;
    seL4_CPtr hw_frame;
    seL4_CPtr net_cmd_recv_ep;
    seL4_CPtr net_cmd_send_ep;
    seL4_CPtr wifi_cmd_recv_ep;
    seL4_CPtr wifi_cmd_send_ep;
    // Фаза 4.5 (Wi-Fi data-plane) — для авто-респавна (watchdog/SYS_START_WIFI)
    // должны пережить рестарт процесса так же, как wifi_cmd_recv_ep выше:
    // net_wifi_rx_badged — капа wifi_driver'а на сигнал net_driver'у (RX);
    // wifi_tx_wake_badged — капа net_driver'а на сигнал wifi_driver'у (TX).
    seL4_CPtr net_wifi_rx_badged;
    seL4_CPtr wifi_tx_wake_badged;
    // Фикс зависания blk_driver (см. situation.txt) — та же логика
    // сохранения через респавн, что у двух полей выше: капа timer_driver'а
    // на heartbeat-badge blk_driver'а (BLK_HEARTBEAT_BADGE).
    seL4_CPtr blk_heartbeat_badged;
    // Фаза 3b — та же логика: капа timer_driver'а на BLK_LIVENESS_TICK_BADGE
    // (см. common.h) — переживает респавн timer_driver'а так же, как поле выше.
    seL4_CPtr blk_liveness_tick_badged;
    // Фикс дедлока root<->blk_driver (см. common.h/BOOT_MMC_IRQ_HANDLER_CAP)
    // — собственная копия IRQHandler'а, тоже переживает респавн.
    seL4_CPtr mmc_irq_handler;
    // issuse.txt №8 — VideoCore mailbox (только is_driver==2, timer_driver)
    // должен пережить респавн так же, как остальные HW-профили выше.
    seL4_CPtr mbox_regs_frame;
    seL4_CPtr mbox_buf_frame;
    seL4_Word mbox_buf_paddr;
    // issuse.txt №65 — приватные некэшируемые DMA-страницы blk_driver'а
    // (только is_driver==3) для ADMA2-дескрипторов (hardware_emmc_read/
    // write отказывают немедленно, если эти адреса нулевые, см.
    // blk_driver.cpp) — тот же класс потери, что у mbox выше, просто для
    // ДРУГОГО драйвера; найдено на живом железе — respawn/`recover
    // blk_driver` рвал EMMC-чтение полностью (даже сектор 0), exFAT-mount
    // проваливался, ВСЕ /sbin-команды переставали находиться.
    seL4_CPtr blk_dma_frame;
    seL4_Word blk_dma_paddr;
    seL4_CPtr blk_dma_frame2;
    seL4_Word blk_dma_paddr2;
    // Фаза 3b плана "Сигналы драйверам" (heartbeat-watchdog) — та же логика
    // сохранения через респавн, что у полей выше: badged-копия
    // mmc_shared_irq_ntfn (DRIVER_LIVENESS_*_BADGE), которой САМ этот процесс
    // сигналит root'у "я жив" (только is_driver 3/4/5/6). Без сохранения
    // здесь респавненный драйвер молча терял бы автомониторинг навсегда —
    // тот же класс потери, что уже был у mbox/blk_dma выше (issuse.txt №8/№65).
    seL4_CPtr liveness_ntfn_badged;
    // Начало GPIO-драйвера (см. h/gpio.h) — фрейм регистров GPIO-контроллера,
    // только is_driver==3 (blk_driver). Тот же класс потери через респавн,
    // что у mbox_regs_frame/blk_dma_frame выше, если не сохранить здесь.
    seL4_CPtr gpio_frame;
    // issuse.txt №73/№75 — аппаратный watchdog (PM_WDOG/PM_RSTC), только
    // is_driver==2 (timer_driver). Тот же класс потери через респавн, что
    // у остальных HW-профилей выше.
    seL4_CPtr pm_wdog_frame;
    // issuse.txt №6 — vaddr, выданный ПЕРВЫМ SYS_SHM_GET этого процесса.
    // Повторный вызов (например shell-команда `shm`, вызываемая уже ПОСЛЕ
    // автоматического SHM_GET при старте в sys_client_init()) отдаёт его же
    // повторно вместо повторного минта/маппинга поверх старых капов.
    uintptr_t shm_vaddr;

    CapTracker cap_tracker;

    // --- НОВОЕ: Трекинг копий SHM для защиты от утечек ---
    bool has_shm;
    seL4_CPtr shm_copies[SHM_TOTAL_PAGES];

    // Фаза 6.1 (SMP, см. ROADMAP.md): на каком ядре сейчас реально исполняется
    // этот процесс — проставляется при спавне (0 по умолчанию, 1 для
    // wifi_driver) и обновляется SYS_SET_AFFINITY. НЕ переживает респавн
    // намеренно (при respawn — назад к зашитому дефолту, см. ROADMAP) — это
    // просто отображение текущего факта, не персистентная настройка.
    int core;

    // issuse.txt: watchdog не мог респавнить не-драйверные процессы
    // (/sbin-утилиты, обычный exec, init.conf-сервисы вроде balancer) — их
    // ELF не встроен в CPIO-архив образа, грузится с диска, а generic_
    // recover_process() умел перечитывать с диска только один хардкод —
    // "/service/wifi.elf" для wifi_driver. path — реальный путь на диске,
    // ИМЕННО ТОТ, которым процесс был загружен (не путать с name — это
    // короткое отображаемое имя для ps/поиска по имени, например
    // "wifi_driver"/"balancer", у /sbin-утилит name уже совпадает с path,
    // но для wifi_driver/init.conf-сервисов они РАЗНЫЕ, см. spawn-сайты).
    // Пусто ("") — процесс встроен в CPIO (drivers/shell), respawn как раньше.
    char path[64];

    // issuse.txt п.3 — переиспользуемая RAM-арена (см. acquire_cmd_arena()/
    // release_cmd_arena() выше), только для обычных /sbin-команд
    // (is_driver 253/254, см. SYS_EXEC-диспетчер). 0 — арена не
    // использовалась (драйверы/сервисы/shell, или пул арен был исчерпан на
    // момент спавна) — освобождать нечего. arena_bytes_used — сколько
    // байт РЕАЛЬНО насчитано в g_ram_bytes_used через эту арену, чтобы
    // откатить счётчик точно при освобождении (не всю ёмкость арены).
    seL4_CPtr cmd_arena = 0;
    uint64_t arena_bytes_used = 0;
};

// --- UNIX PIPES SUBSYSTEM ---
#define MAX_PIPES 16
#define PIPE_BASE_BADGE 1000 // Бейджи от 1000 до 1015 будут пайпами

// IRQ_MMC_SHARED_BADGE теперь в common.h (см. комментарий там же — фикс
// задержки, нотификация TCB-bound к blk_driver, не к root).

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

// Фаза 7 (DVFS), см. SYS_MARK_SHELL_ACTIVITY в common.h — момент (аптайм в мс)
// последнего введённого в шелле символа команды. 0 = ещё не было ни одной
// команды (считаем это "недавняя активность", не идём в low сразу на старте).
static seL4_Word g_last_shell_activity_ms = 0;

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

// Фаза 9.C (см. ROADMAP.md): узкая проверка права на административные
// syscall'ы (SYS_KILL/SYS_SET_AFFINITY/SYS_BALANCE и присвоение is_driver=253
// при SYS_EXEC "/sbin/..."). До этой фазы ЛЮБОЙ процесс, у которого есть
// root_ep (а он есть у всех — даже у is_driver==254, untrusted exec, он нужен
// им для базовых sys_write/sys_read), мог убить/переместить/сбалансировать
// что угодно — достаточно было знать номер syscall'а, никакой проверки
// вызывающего не было вообще. sender_pid здесь — это PID, извлечённый из
// бейджа отправителя (см. цикл main(), decode бейджа перед switch), поэтому
// подделать чужой PID нельзя (бейдж выдаётся root'ом при спавне и меняться
// процессом не может). Разрешено: shell (is_driver==0) и доверенные /sbin- и
// /service-процессы (is_driver==253, категория из Фазы 9.A). Не разрешено:
// произвольный exec (is_driver==254) и сами драйверы (1/2/3/4/5) — им эти
// операции не нужны и раньше не были доступны через штатные команды.
static bool is_admin_caller(seL4_Word pid) {
    if (pid == 0 || pid >= 256 || !pcbs[pid].active) return false;
    return pcbs[pid].is_driver == 0 || pcbs[pid].is_driver == 253;
}

// Wi-Fi (index 5) сознательно НЕ входит в driver_ready[]/all_drivers_ready()
// выше (см. их комментарий) — он больше не автоспавнится при загрузке и
// живёт своим отдельным жизненным циклом (SYS_START_WIFI/SYS_STOP_WIFI/
// SYS_WIFI_STATUS). true означает "wifi_driver дошёл до своего SYS_DRIVER_READY",
// т.е. гарантированно уже висит в блокирующем seL4_Recv и готов принимать
// любые WIFI_CMD_* — даже если сама проба SDIO/прошивки внутри провалилась
// (в этом случае команды просто вернут код ошибки, а не зависнут).
static bool g_wifi_driver_ready = false;

// Фаза 14 (USB, xHCI) — тот же принцип, что g_wifi_driver_ready выше: usb_driver
// компилируется в CPIO и всегда запущен (в отличие от wifi, у него нет
// собственного start/stop), но НЕ входит в driver_ready[]/all_drivers_ready()
// (см. ниже) — опциональная периферия (ROADMAP.md Фаза 8), загрузка не
// должна ждать/виснуть, если ничего не воткнуто в USB. true — usb_driver
// дошёл до своего SYS_DRIVER_READY (готов принимать SYS_USB_LIST), даже
// если bring-up контроллера не нашёл ни одного устройства.
static bool g_usb_driver_ready = false;

// Фаза 3b плана "Сигналы драйверам" (heartbeat-watchdog) — индекс всюду
// ниже = is_driver (1=uart,2=timer,3=blk,4=net,5=wifi,6=usb; 0 неисп.).
// Мониторятся ТОЛЬКО 3/4/5/6 — uart без heartbeat-тика, timer не может
// обнаружить собственное зависание через тик, который сам же производит
// (см. план). last_seen — метка SYS_GET_UPTIME (мс) последнего полученного
// liveness-сигнала от процесса; 0 = ещё ни разу не приходил (свежеспавненный
// — см. spawn_process(), где last_seen[is_driver] сбрасывается в 0 при
// каждом (ре)спавне, без какого-либо IPC — см. живой урок про дедлок
// загрузки там же). auto_restart — включён ли автотриггер для этого
// driver'а; дефолт false для всех — единственный источник true теперь
// load_auto_restart_config() (Фаза 4, читает /etc/auto_restart, fail-closed
// если файла нет). timeout — стартовые цифры калибровки, НЕ измеренный факт
// (см. план) — особенно wifi: 15-30с легально занимает один connect/scan
// (hw-подтверждено: реальный `wifi scan` ~30с не вызвал ложного
// срабатывания), драйвер физически не может ответить heartbeat'ом мимо
// этого; таймаут должен быть заведомо больше худшего легального случая.
static seL4_Word g_driver_last_seen_ms[7] = {0, 0, 0, 0, 0, 0, 0};
static bool g_auto_restart_enabled[7] = {false, false, false, false, false, false, false};
static constexpr seL4_Word WATCHDOG_TIMEOUT_MS[7] = {0, 0, 0, 3000, 5000, 45000, 5000};

// issuse.txt №74, часть "б" (адаптивный перенос драйверов между ядрами) —
// общая страница состояния драйверов. Фрейм создаётся один раз в main(),
// мапится НЕКЭШИРУЕМО и в root_vspace (PLAT_DRIVER_STATE_ROOT_VADDR), и —
// копией капы — в каждый драйвер (PLAT_DRIVER_STATE_VADDR /
// PLAT_DRIVER_STATE_USB_VADDR, см. spawn_process()). Глобал, а не параметр
// spawn_process(), сознательно: у той уже ~54 позиционных параметра, и
// добавление ещё одного — известная ловушка этого проекта (см.
// project_psych_ward_os_gpio_driver в памяти: молча сдвигает чужие
// аргументы). Читает root ОБЫЧНОЙ инструкцией загрузки — ни syscall'а, ни
// шанса заблокироваться, что и есть весь смысл механизма.
static seL4_CPtr g_driver_state_frame = 0;
static volatile seL4_Word *g_driver_state_page = nullptr;

// Badged-копия mmc_shared_irq_ntfn с ROOT_WATCHDOG_TICK_BADGE — её
// spawn_process() выдаёт timer_driver'у (is_driver==2), и тот сигналит ею
// root'у на каждом своём тике. Глобал по той же причине, что и
// g_driver_state_frame выше: ещё один позиционный параметр у функции с
// полусотней аргументов — известная ловушка этого проекта.
static seL4_CPtr g_root_watchdog_tick_badged = 0;

// Копия usb_cmd_ep для generic_recover_process(): та обязана уметь сбросить
// PCIe-шину ДО того, как начнёт разрушать чей-либо VSpace (см. развёрнутое
// объяснение у usb_bus_stalled_suspect() ниже). Параметром не тащим — у
// функции их и так избыток.
static seL4_CPtr g_usb_cmd_ep = 0;

// Пошаговый watchdog (см. common.h/UsbStep, h/driver_state.h). Индекс =
// is_driver. last_progress — последнее увиденное значение счётчика
// прогресса драйвера; progress_seen_ms — когда оно в последний раз
// менялось. Растущий счётчик означает "занят, но жив и дойдёт до своего
// таймаута"; замерший при непарковке — "ядро физически не исполняет код
// драйвера", то есть настоящий аппаратный стопор.
static seL4_Word g_driver_last_progress[7] = {0, 0, 0, 0, 0, 0, 0};
static seL4_Word g_driver_progress_seen_ms[7] = {0, 0, 0, 0, 0, 0, 0};

static const char *usb_step_name(seL4_Word step) {
    switch (step) {
        case USB_STEP_IDLE:            return "простой (ничего не делает)";
        case USB_STEP_CONTROLLER_INIT: return "инициализация контроллера xHCI";
        case USB_STEP_PORT_POLL:       return "опрос корневых портов";
        case USB_STEP_PORT_RESET:      return "сброс корневого порта";
        case USB_STEP_ENUMERATE:       return "перечисление устройства";
        case USB_STEP_ADDRESS_DEVICE:  return "Address Device";
        case USB_STEP_GET_DESCRIPTOR:  return "чтение дескрипторов";
        case USB_STEP_SET_CONFIG:      return "SET_CONFIGURATION";
        case USB_STEP_HUB_POLL:        return "опрос interrupt-эндпоинта хаба";
        case USB_STEP_HUB_PORT_RESET:  return "сброс порта хаба";
        case USB_STEP_HUB_ENUMERATE:   return "перечисление устройства за хабом";
        case USB_STEP_SCSI_COMMAND:    return "SCSI-команда";
        case USB_STEP_READ_SECTORS:    return "чтение секторов";
        case USB_STEP_PARTITION_SCAN:  return "поиск раздела (MBR/GPT)";
        case USB_STEP_EXFAT_MOUNT:     return "монтирование exFAT";
        case USB_STEP_UNMOUNT:         return "размонтирование";
        case USB_STEP_BUS_RESET:       return "сброс шины";
        default:                       return "неизвестный шаг";
    }
}

// Сколько раз подряд heartbeat-watchdog делал полный сброс USB-шины, не
// дождавшись между попытками ни одного признака жизни от usb_driver.
// Обнуляется первым же пришедшим liveness-сигналом. Нужен, чтобы
// по-настоящему мёртвая шина не превращалась в бесконечный цикл
// PERST-дёрганья (см. главный цикл).
static int g_usb_reset_attempts = 0;

// Сброс шины прошёл, а сказать драйверу "перечисляй заново" было некому:
// он в этот момент не отвечал. Контроллер после PERST+перезаливки
// прошивки VL805 стоит с нуля, кольца и слоты драйвера недействительны —
// без переэнумерации USB останется мёртвым, даже когда драйвер очнётся.
// Поэтому запоминаем долг и отдаём его на первом же тике, когда драйвер
// снова окажется припаркован.
static bool g_usb_restart_pending = false;
constexpr int USB_RESET_MAX_CONSECUTIVE = 3;

// issuse.txt №70 — PID текущего держателя глобального vfs_mutex_ep (0 =
// никто), обновляется ТОЛЬКО через case SYS_VFS_LOCK_NOTIFY ниже (клиенты
// сами сообщают о взятии/отдаче, см. vfs_lock()/vfs_unlock() в shell.cpp/
// h/sys_client.h) — единственный держатель одновременно, т.к. сам мьютекс
// бинарный. g_vfs_mutex_ntfn_cap — копия локальной vfs_mutex_ntfn из
// main() (см. её создание ниже), нужна здесь, чтобы generic_recover_process()
// могла форсировать seL4_Signal, если убитый/упавший процесс оказался
// текущим держателем (root создал объект и всегда сохраняет на него
// сигнал-капу, см. "main.cpp сигналит vfs_mutex_ntfn один раз сразу после
// создания" в shell.cpp).
static seL4_Word g_vfs_lock_holder_pid = 0;
static seL4_CPtr g_vfs_mutex_ntfn_cap = 0;

// Фаза 12 (см. ROADMAP.md) — формат подписи: 4-байтовый magic + 64-байтовая
// Ed25519-подпись, приклеенные в конец файла (см. tools/sign_elf/). Работает
// одинаково для ELF-бинарников и текстовых конфигов (сама схема не смотрит
// на содержимое) — единая точка проверки прямо здесь, в load_elf_from_disk(),
// автоматически покрывает ВСЕ 5 мест, которые её вызывают (SYS_EXEC,
// init.conf, watchdog-респавн, автозапуск сервисов, /service/wifi.elf), без
// изменений в вызывающем коде: возврат -1 при провале уже трактуется всеми
// вызывающими как обычная ошибка чтения.
constexpr unsigned char SIG_MAGIC[4] = { 'P', 'W', 'S', 'G' };
constexpr uint32_t SIG_TRAILER_SIZE = 68; // 4 (magic) + 64 (подпись)

// Общий цикл чтения файла с диска через blk_ep (SYS_READ_FILE, чанками
// через SHM) — вынесен из load_elf_from_disk() ниже, чтобы
// load_text_config_from_disk() (см. там же) не дублировал эту логику
// (offset-цикл + два flush_rootserver_shm(), см. их комментарии) один в
// один. -1 — ошибка чтения/файл не найден; иначе — сколько байт реально
// прочитано (может быть 0 для пустого файла).
static int read_file_raw_from_disk(seL4_CPtr blk_ep, const char* filename, char* load_buffer) {
    char* shm = rootserver_shm_base;
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
        // Содержимое приходит в область полезной нагрузки, а не в начало
        // SHM (см. VFS_PAYLOAD_OFFSET в platform.h) — начало занято путём.
        memcpy(load_buffer + total_read, shm + VFS_PAYLOAD_OFFSET, bytes_read);
        total_read += bytes_read;
    }
    return (int)total_read;
}

static int load_elf_from_disk(seL4_CPtr blk_ep, const char* filename, char* load_buffer) {
    int total_read = read_file_raw_from_disk(blk_ep, filename, load_buffer);
    if (total_read < 0) return -1;

    // Проверка подписи (Фаза 12) — fail-closed: нет валидного трейлера —
    // файл не используется, точка. Никаких исключений "для старых
    // неподписанных файлов" — иначе вся схема тривиально обходится. Это
    // касается ТОЛЬКО настоящего исполняемого кода (/sbin, /service, любой
    // SYS_EXEC target) — для простых текстовых конфигов из /etc см.
    // load_text_config_from_disk() ниже, у неё намеренно другая модель.
    if ((uint32_t)total_read <= SIG_TRAILER_SIZE ||
        memcmp(load_buffer + total_read - SIG_TRAILER_SIZE, SIG_MAGIC, 4) != 0) {
        uart_puts("[SIG] отсутствует подпись, отказ: "); uart_puts(filename); uart_puts("\n");
        return -1;
    }
    uint32_t data_len = (uint32_t)total_read - SIG_TRAILER_SIZE;
    const unsigned char *signature = (const unsigned char*)(load_buffer + data_len + 4);
    if (crypto_ed25519_check(signature, OS_PUBLIC_KEY, (const unsigned char*)load_buffer, data_len) != 0) {
        uart_puts("[SIG] ПОДПИСЬ НЕВЕРНА, исполнение отклонено: "); uart_puts(filename); uart_puts("\n");
        return -1;
    }
    return data_len;
}

// По просьбе пользователя (2026-08-16) — /etc-конфиги (init.conf,
// auto_restart и любые будущие) читаются КАК ЕСТЬ, без Ed25519-подписи:
// приватный ключ подписи существует только на машине разработчика —
// требовать подпись для файлов, которые должны быть редактируемы прямо на
// устройстве (touch/echo>файл, в будущем — полноценный редактор), было бы
// противоречием самой их цели. НЕ путать с load_elf_from_disk() выше — та
// ВСЕГДА fail-closed на отсутствие подписи, намеренно, для настоящего
// исполняемого кода. Осознанное ослабление защиты Фазы 12 ИМЕННО для этих
// мест вызова — оба потребителя (start_init_services()/
// load_auto_restart_config()) разбирают содержимое только как текст
// (имя/путь/приоритет/ядро либо булев переключатель по точному совпадению
// имени драйвера), ничего из прочитанного не исполняется и не попадает
// на файловую систему как путь без отдельной, всё ещё подписанной
// проверки — риск подмены ограничен "заставить систему запустить/не
// перезапустить то, что и так уже есть на диске и само по себе всё ещё
// проверяется своей подписью", не произвольным исполнением кода.
static int load_text_config_from_disk(seL4_CPtr blk_ep, const char* filename, char* load_buffer) {
    // Не null-терминируем здесь — тот же контракт, что у load_elf_from_disk()
    // выше: оба вызывающих (start_init_services()/load_auto_restart_config())
    // уже сами проверяют границу буфера и терминируют ПОСЛЕ этой проверки
    // (см. их же код) — терминировать здесь, до этой проверки, рисковало бы
    // записью на 1 байт за пределами буфера вызывающего, если total_read
    // ровно равен его размеру.
    return read_file_raw_from_disk(blk_ep, filename, load_buffer);
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
        if (ram_retype(normal_untyped, seL4_ARM_PageUpperDirectoryObject, 0, root_cnode, 0, 0, pud, 1) == seL4_NoError) {
            seL4_ARM_PageUpperDirectory_Map(pud, vspace, vaddr, (seL4_ARM_VMAttributes)0);
        }

        seL4_CPtr pd = alloc_and_track_cap(alloc, pcb);
        if (ram_retype(normal_untyped, seL4_ARM_PageDirectoryObject, 0, root_cnode, 0, 0, pd, 1) == seL4_NoError) {
            seL4_ARM_PageDirectory_Map(pd, vspace, vaddr, (seL4_ARM_VMAttributes)0);
        }

        seL4_CPtr pt = alloc_and_track_cap(alloc, pcb);
        if (ram_retype(normal_untyped, seL4_ARM_PageTableObject, 0, root_cnode, 0, 0, pt, 1) == seL4_NoError) {
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

// Фаза 6.1 (продолжение, см. ROADMAP.md): снимок нагрузки за короткое окно
// (~100мс), общий для `top` (SYS_TOP_STATS) и `balance` (SYS_BALANCE) —
// вынесено в отдельную функцию, т.к. протокол измерения хрупкий (трижды
// доводился через баги на живом железе, см. ROADMAP) и дублировать его
// нельзя. См. подробности каждого шага в исходном месте использования
// (main.cpp, до рефакторинга) и в SYS_BENCHMARK_RESET_LOCAL/
// SYS_BENCHMARK_FINALIZE_LOCAL (h/common.h).
struct LoadSnapshot {
    bool core_enabled[4];
    uint64_t total[4];
    uint64_t core_idle[4];
    uint64_t proc_util[256]; // валиден только если core_enabled[pcbs[i].core]
};

static void collect_load_snapshot(LoadSnapshot &snap, seL4_CPtr console_ep, seL4_CPtr blk_ep,
                                   seL4_CPtr net_cmd_send_ep, seL4_CPtr wifi_cmd_send_ep,
                                   seL4_CPtr timer_ep) {
    seL4_BenchmarkResetThreadUtilisation(seL4_CapInitThreadTCB);
    for (int i = 1; i < 256; i++) {
        if (pcbs[i].active) seL4_BenchmarkResetThreadUtilisation(pcbs[i].tcb);
    }

    // Включить учёт utilisation на каждом занятом ненулевом ядре — root не
    // может сделать это за другое ядро сам (per-core состояние в ядре),
    // просит ЛЮБОЙ активный uart/blk/net/wifi процесс на этом ядре сделать
    // это самому. Одного представителя достаточно — включает учёт СРАЗУ
    // для ВСЕХ потоков на этом ядре (в т.ч. шелл/exec).
    snap.core_enabled[0] = true;
    snap.core_enabled[1] = snap.core_enabled[2] = snap.core_enabled[3] = false;
    int representative[4] = {-1, -1, -1, -1}; // pid представителя каждого ядра (для парного Finalize после сна)
    for (int c = 1; c < 4; c++) {
        for (int i = 1; i < 256; i++) {
            if (!pcbs[i].active || pcbs[i].core != c) continue;
            int d = pcbs[i].is_driver;
            if (d == 1) {
                seL4_SetMR(0, SYS_BENCHMARK_RESET_LOCAL);
                seL4_Call(console_ep, seL4_MessageInfo_new(0, 0, 0, 1));
            } else if (d == 3) {
                seL4_SetMR(0, SYS_BENCHMARK_RESET_LOCAL);
                seL4_Call(blk_ep, seL4_MessageInfo_new(0, 0, 0, 1));
            } else if (d == 4) {
                seL4_SetMR(0, 8); // NET_CMD_BENCHMARK_RESET, см. net_driver.cpp
                seL4_Call(net_cmd_send_ep, seL4_MessageInfo_new(0, 0, 0, 1));
            } else if (d == 5) {
                seL4_SetMR(0, 4); // WIFI_CMD_BENCHMARK_RESET, см. wifi_driver.cpp
                seL4_SetMR(1, 0); // wifi_driver читает MR1 (verbose) безусловно
                seL4_Call(wifi_cmd_send_ep, seL4_MessageInfo_new(0, 0, 0, 2));
            } else {
                continue; // шелл/exec на этом ядре — попросить не может, пробуем следующего
            }
            snap.core_enabled[c] = true;
            representative[c] = i;
            break; // один представитель на ядро достаточно
        }
    }

    seL4_BenchmarkResetLog();

    seL4_SetMR(0, 8); // SYS_SLEEP_MS (см. shell.cpp/sys_sleep) — root как обычный клиент timer_ep
    seL4_SetMR(1, 100);
    seL4_Call(timer_ep, seL4_MessageInfo_new(0, 0, 0, 2));

    seL4_BenchmarkFinalizeLog();

    for (int c = 0; c < 4; c++) { snap.total[c] = 1; snap.core_idle[c] = 0; }

    // pid 0 = rootserver, TCB = seL4_CapInitThreadTCB, всегда ядро 0
    seL4_BenchmarkGetThreadUtilisation(seL4_CapInitThreadTCB);
    uint64_t root_util = seL4_GetMR(BENCHMARK_TCB_UTILISATION);
    snap.total[0] = seL4_GetMR(BENCHMARK_TOTAL_UTILISATION);
    if (snap.total[0] == 0) snap.total[0] = 1;
    snap.core_idle[0] = seL4_GetMR(BENCHMARK_IDLE_TCBCPU_UTILISATION);

    // total у ядра N нельзя считать от значения ядра 0
    // (BENCHMARK_TOTAL_UTILISATION всегда от ВЫЗЫВАЮЩЕГО, не от того, чей
    // TCB спрашивают) — просим ТОГО ЖЕ представителя финализировать и
    // отдать СВОЙ честный idle/total (см. SYS_BENCHMARK_FINALIZE_LOCAL).
    for (int c = 1; c < 4; c++) {
        if (!snap.core_enabled[c] || representative[c] < 0) continue;
        int i = representative[c];
        int d = pcbs[i].is_driver;
        seL4_Word idle_local = 0, total_local = 0;
        if (d == 1) {
            seL4_SetMR(0, SYS_BENCHMARK_FINALIZE_LOCAL);
            seL4_Call(console_ep, seL4_MessageInfo_new(0, 0, 0, 1));
            idle_local = seL4_GetMR(0); total_local = seL4_GetMR(1);
        } else if (d == 3) {
            seL4_SetMR(0, SYS_BENCHMARK_FINALIZE_LOCAL);
            seL4_Call(blk_ep, seL4_MessageInfo_new(0, 0, 0, 1));
            idle_local = seL4_GetMR(0); total_local = seL4_GetMR(1);
        } else if (d == 4) {
            seL4_SetMR(0, 9); // NET_CMD_BENCHMARK_FINALIZE, см. net_driver.cpp
            seL4_Call(net_cmd_send_ep, seL4_MessageInfo_new(0, 0, 0, 1));
            idle_local = seL4_GetMR(0); total_local = seL4_GetMR(1);
        } else if (d == 5) {
            seL4_SetMR(0, 5); // WIFI_CMD_BENCHMARK_FINALIZE, см. wifi_driver.cpp
            seL4_SetMR(1, 0);
            seL4_Call(wifi_cmd_send_ep, seL4_MessageInfo_new(0, 0, 0, 2));
            idle_local = seL4_GetMR(0); total_local = seL4_GetMR(1);
        }
        snap.core_idle[c] = idle_local;
        snap.total[c] = (total_local == 0) ? 1 : total_local;
    }

    for (int i = 0; i < 256; i++) snap.proc_util[i] = 0;
    snap.proc_util[0] = root_util;
    for (int i = 1; i < 256; i++) {
        if (!pcbs[i].active) continue;
        int c = pcbs[i].core;
        if (c < 0 || c >= 4 || !snap.core_enabled[c]) continue;
        seL4_BenchmarkGetThreadUtilisation(pcbs[i].tcb);
        snap.proc_util[i] = seL4_GetMR(BENCHMARK_TCB_UTILISATION);
    }
}

// Фаза 14 (USB, xHCI) — приватные DMA-страницы usb_driver'а (см.
// PLAT_XHCI_*_VADDR в platform.h). Один параметр вместо ~10 отдельных
// scalar-параметров spawn_process() (как для blk_dma_frame_param/
// blk_dma_paddr_param) — у blk_driver таких страниц было только 2, у USB —
// до 7 фиксированных структур плюс до USB_MAX_SCRATCHPAD_PAGES scratchpad-
// страниц, отдельный параметр на каждую раздул бы и так огромный список
// параметров spawn_process() ещё сильнее без всякой пользы.
struct UsbDmaSetup {
    seL4_CPtr dcbaa_frame = 0;          seL4_Word dcbaa_paddr = 0;
    seL4_CPtr cmdring_frame = 0;        seL4_Word cmdring_paddr = 0;
    seL4_CPtr erst_frame = 0;           seL4_Word erst_paddr = 0;
    seL4_CPtr evtring_frame = 0;        seL4_Word evtring_paddr = 0;
    // Фаза 15 (несколько накопителей, см. ROADMAP.md/план) — Device
    // Context теперь по одной странице на КАЖДЫЙ xHCI Slot ID
    // (USB_MAX_SLOTS_ENABLED), не единственная общая страница.
    seL4_CPtr devctx_frame[USB_MAX_SLOTS_ENABLED] = {0};
    seL4_Word devctx_paddr[USB_MAX_SLOTS_ENABLED] = {0};
    seL4_CPtr inputctx_frame = 0;       seL4_Word inputctx_paddr = 0; // общий, транзитный — см. platform.h
    seL4_CPtr scratchpad_arr_frame = 0; seL4_Word scratchpad_arr_paddr = 0;
    int scratchpad_count = 0;
    seL4_CPtr scratchpad_buf_frame[USB_MAX_SCRATCHPAD_PAGES] = {0};
    seL4_Word scratchpad_buf_paddr[USB_MAX_SCRATCHPAD_PAGES] = {0};
    // Фаза 15 — шесть per-device ресурсов (было: по одной странице каждый,
    // единственное устройство; см. h/platform.h) — теперь по
    // USB_MAX_DEVICES страниц каждый, индексируются "нашим" индексом
    // устройства (0..USB_MAX_DEVICES-1), не Slot ID.
    seL4_CPtr ep0_trring_frame[USB_MAX_DEVICES] = {0};     seL4_Word ep0_trring_paddr[USB_MAX_DEVICES] = {0};
    seL4_CPtr ctrl_buf_frame[USB_MAX_DEVICES] = {0};       seL4_Word ctrl_buf_paddr[USB_MAX_DEVICES] = {0};
    seL4_CPtr bulkout_trring_frame[USB_MAX_DEVICES] = {0}; seL4_Word bulkout_trring_paddr[USB_MAX_DEVICES] = {0};
    seL4_CPtr bulkin_trring_frame[USB_MAX_DEVICES] = {0};  seL4_Word bulkin_trring_paddr[USB_MAX_DEVICES] = {0};
    seL4_CPtr cbw_csw_frame[USB_MAX_DEVICES] = {0};        seL4_Word cbw_csw_paddr[USB_MAX_DEVICES] = {0};
    // Bounce — единственный ресурс из нескольких СМЕЖНЫХ страниц на устройство
    // (см. USB_BOUNCE_PAGES в platform.h): [устройство][страница].
    seL4_CPtr bounce_frame[USB_MAX_DEVICES][USB_BOUNCE_PAGES] = {{0}}; seL4_Word bounce_paddr[USB_MAX_DEVICES] = {0};
};

// issuse.txt (найдено на живом железе 2026-08-10, при hw-проверке фикса
// №8): generic_recover_process() респавнил usb_driver (kill/crash), но НЕ
// передавал ни usb_cmd_recv_ep, ни usb_dma (весь набор xHCI DMA-страниц:
// DCBAA/Command Ring/ERST/Event Ring/Device Context/Input Context/
// scratchpad/per-device EP0-TRB-кольца/CBW-CSW/bounce), ни PCIe RC MISC/
// ERR-фреймы — все они были ЛОКАЛЬНЫМИ переменными main(), невидимыми из
// generic_recover_process() (тот же класс бага, что и №8 с VideoCore
// mailbox, просто для usb_driver — на порядок больше состояния). На
// живом железе это вызывало ДЕТЕРМИНИРОВАННЫЙ крах уже на первой
// аппаратной операции нового процесса (step0_check_link(),
// usb_driver.cpp:730, читает PLAT_PCIE_RC_VADDR — страница просто не
// замаплена) — и, что хуже, крах-респавн зацикливался (новый процесс
// падает на той же строке, watchdog его снова респавнит, снова падает...)
// вплоть до полного зависания платы, потребовавшего физической
// перезагрузки. Все эти объекты аллоцируются РОВНО ОДИН РАЗ при загрузке
// и живут вечно (main() никогда не возвращается, root_cnode-капы,
// которые они держат, никогда не revoke'ятся при крахе ЧУЖОГО процесса)
// — поэтому вместо дублирования полей в PCB (как для mbox) просто
// поднимаем те же самые переменные до file-scope: main() продолжает
// инициализировать их как раньше (тела циклов/lambda не менялись), но
// теперь generic_recover_process() тоже может их прочитать напрямую.
static UsbDmaSetup usb_dma;
static seL4_CPtr pcie_rc_frame = 0;
static seL4_CPtr pcie_err_frame = 0;
static seL4_CPtr usb_cmd_recv_ep = 0;

// issuse.txt №74/в (см. situation.txt) — точечный, локальный сброс
// именно PCIe Root Complex (не всей платы, см. SYS_REBOOT), выполняется
// ЦЕЛИКОМ root'ом, через СОБСТВЕННЫЙ прямой маппинг регистров
// (PLAT_PCIE_RC_MISC_ROOT_VADDR/PLAT_PCIE_RGR1_ROOT_VADDR, см. main()) —
// НЕ через usb_driver: сама идея в том, что если usb_driver завис
// (в т.ч. предельно жёстко, на самой шине — issuse.txt, "АППАРАТНОЕ
// ОГРАНИЧЕНИЕ"), root не должен зависеть от её ответа, чтобы иметь шанс
// восстановить шину. Регистровая последовательность 1:1 сверена с
// реальным исходником Linux (drivers/pci/controller/pcie-brcmstb.c,
// raspberrypi/linux — brcm_pcie_perst_set_generic()/
// brcm_pcie_bridge_sw_init_set_generic()/brcm_pcie_setup(),
// поле brcm_pcie_link_up()) — та же последовательность, что Linux
// выполняет при пробе контроллера.
static inline uint32_t pcie_rgr1_read() {
    return *(volatile uint32_t*)(PLAT_PCIE_RGR1_ROOT_VADDR + PCIE_RGR1_SW_INIT_1_OFFSET);
}
static inline void pcie_rgr1_write(uint32_t val) {
    *(volatile uint32_t*)(PLAT_PCIE_RGR1_ROOT_VADDR + PCIE_RGR1_SW_INIT_1_OFFSET) = val;
}
static inline uint32_t pcie_hard_debug_read() {
    return *(volatile uint32_t*)(PLAT_PCIE_RC_MISC_ROOT_VADDR + PCIE_MISC_HARD_PCIE_HARD_DEBUG_OFFSET);
}
static inline void pcie_hard_debug_write(uint32_t val) {
    *(volatile uint32_t*)(PLAT_PCIE_RC_MISC_ROOT_VADDR + PCIE_MISC_HARD_PCIE_HARD_DEBUG_OFFSET) = val;
}
static inline uint32_t pcie_status_read() {
    return *(volatile uint32_t*)(PLAT_PCIE_RC_MISC_ROOT_VADDR + PCIE_MISC_PCIE_STATUS_OFFSET);
}
// issuse.txt №74/в — общий доступ к остальным регистрам ТОЙ ЖЕ страницы
// PLAT_PCIE_RC_MISC_PADDR (RC_BAR2_CONFIG_LO/HI, MISC_CTRL) — см.
// root_pcie_fundamental_reset().
static inline void pcie_rc_misc_write(uintptr_t offset, uint32_t val) {
    *(volatile uint32_t*)(PLAT_PCIE_RC_MISC_ROOT_VADDR + offset) = val;
}
static inline uint32_t pcie_rc_misc_read(uintptr_t offset) {
    return *(volatile uint32_t*)(PLAT_PCIE_RC_MISC_ROOT_VADDR + offset);
}

// issuse.txt №74/в — 29-я hw-попытка: точечная диагностика Device Status
// VL805 (см. PCI_EXPRESS_CAP_VL805_DEVICE_STATUS_OFFSET/platform.h) в
// НЕСКОЛЬКИХ точках root_pcie_fundamental_reset(), чтобы найти, ПОСЛЕ
// какого именно нашего обращения к VL805 (BAR0? Command? или он уже
// взведён СРАЗУ после PERST, ещё до наших обращений) появляются биты
// Correctable Error/Unsupported Request Detected — 28-я попытка нашла
// их взведёнными ПОСТФАКТУМ (после краха), 29-я — что они взведены УЖЕ
// сразу после mailbox-тега и НЕ снимаются ожиданием (RW1C). Требует,
// чтобы PCIE_EXT_CFG_INDEX уже указывал на VL805 (MBOX_XHCI_RESET_DEV_ADDR)
// — вызывающая сторона отвечает за это (в root_pcie_fundamental_reset()
// это уже так с момента первой установки индекса).
static void pcie_debug_vl805_devstatus(const char *label) {
    if (!LOG_PCIE_DIAG) return; // см. platform.h — обвязка оставлена, но по умолчанию молчит
    uint32_t raw = *(volatile uint32_t*)(PLAT_PCIE_CFG_DATA_ROOT_VADDR + PCI_EXPRESS_CAP_VL805_DEVICE_STATUS_OFFSET);
    uint32_t dev_status = (raw >> 16) & 0xFFFFu;
    uart_puts(label); uart_puthex(dev_status);
    if (dev_status & (PCIE_DEV_STATUS_CORRECTABLE_ERROR_MASK | PCIE_DEV_STATUS_UNSUPPORTED_REQUEST_MASK)) {
        uart_puts(" (есть ошибки)\n");
    } else {
        uart_puts(" (чисто)\n");
    }
}

// issuse.txt №74/в — 31-я hw-попытка: чисто диагностическое чтение
// RBUS-таймаута (см. PCIE_RGR1_RBUS_TIMEOUT_OFFSET/platform.h) — эта
// страница (PLAT_PCIE_RGR1_PADDR) доступна ВСЕГДА, включая ДО release
// PERST/подтверждения линка (в отличие от VL805 config-space).
static void pcie_debug_rbus_timeout(const char *label) {
    if (!LOG_PCIE_DIAG) return; // см. platform.h — обвязка оставлена, но по умолчанию молчит
    uint32_t val = *(volatile uint32_t*)(PLAT_PCIE_RGR1_ROOT_VADDR + PCIE_RGR1_RBUS_TIMEOUT_OFFSET);
    uart_puts(label); uart_puthex(val); uart_puts("\n");
}

// Возвращает true, если PHYLINKUP+DL_ACTIVE поднялись до истечения
// таймаута. НЕ включает сам mailbox-вызов (см. SYS_MBOX_XHCI_RESET/
// SYS_PCIE_RESET, вызывающая сторона — по предупреждению у
// MBOX_TAG_NOTIFY_XHCI_RESET/platform.h тег нельзя звать, если линк не
// поднялся) и НЕ включает повторное перечисление устройств
// (`driver usb_driver restart`, уже существует — намеренно отдельный
// шаг, см. situation.txt: сначала проверить на живом железе, что сам
// сброс шины вообще работает, прежде чем что-либо сцеплять/
// автоматизировать).
static bool root_pcie_fundamental_reset(seL4_CPtr timer_ep) {
    uint32_t tmp;

    // контрольная точка G — RBUS-таймаут ДО чего-либо вообще (истинный
    // baseline).
    pcie_debug_rbus_timeout("[ROOT]   DIAG RBUS timeout (G: начало функции) = ");

    // 1. Assert bridge software init.
    tmp = pcie_rgr1_read();
    pcie_rgr1_write(tmp | PCIE_RGR1_SW_INIT_1_INIT_GENERIC_MASK);

    // 2. Assert PERST.
    tmp = pcie_rgr1_read();
    pcie_rgr1_write(tmp | PCIE_RGR1_SW_INIT_1_PERST_MASK);

    // 3. usleep_range(100, 200) в Linux — root не может звать
    // SYS_SLEEP_MS с суб-миллисекундной точностью, а держать PERST
    // ДОЛЬШЕ минимума безопасно (это просто assert-импульс) — 1мс с
    // честным запросом к timer_ep (root как обычный клиент, тот же
    // приём, что уже есть в этом файле для SYS_BENCHMARK), а не
    // некалиброванный busy-spin по счётчику итераций.
    seL4_SetMR(0, 8); // SYS_SLEEP_MS
    seL4_SetMR(1, 1);
    seL4_Call(timer_ep, seL4_MessageInfo_new(0, 0, 0, 2));

    // 4. Release bridge software init.
    tmp = pcie_rgr1_read();
    pcie_rgr1_write(tmp & ~PCIE_RGR1_SW_INIT_1_INIT_GENERIC_MASK);

    // ШЕСТАЯ hw-попытка (issuse.txt №74/в) — эти самые записи, вставленные
    // ПОКА bridge init был ещё удержан (между assert и release), повесили
    // систему ЕЩЁ РАНЬШЕ, чем в пятой попытке (до печати ЛЮБОГО следующего
    // сообщения) — весь PCIE_MISC-блок, судя по всему, физически
    // недоступен, пока bridge software init удержан, ВКЛЮЧАЯ регистры
    // (PCIE_MISC_PCIE_STATUS/HARD_PCIE_HARD_DEBUG), которые в ЭТОЙ ЖЕ
    // функции уже безопасно читаются/пишутся, но ТОЛЬКО ПОСЛЕ release
    // (см. ниже) — перенесено сюда, ТУДА ЖЕ, где уже проверенно
    // безопасно (тот же принцип, что SERDES_IDDQ чуть ниже).
    pcie_rc_misc_write(PCIE_MISC_RC_BAR2_CONFIG_LO_OFFSET, PCIE_MISC_RC_BAR2_CONFIG_LO_KNOWN_GOOD);
    pcie_rc_misc_write(PCIE_MISC_RC_BAR2_CONFIG_HI_OFFSET, PCIE_MISC_RC_BAR2_CONFIG_HI_KNOWN_GOOD);
    pcie_rc_misc_write(PCIE_MISC_MISC_CTRL_OFFSET, PCIE_MISC_MISC_CTRL_KNOWN_GOOD);

    // НАЙДЕНО НА ЖИВОМ ЖЕЛЕЗЕ (диагностика step0_check_link(), сравнение
    // readback'а на штатной загрузке и после usbreset): bridge software
    // init стирает и outbound-окно (CPU 0x600000000 -> шина 0xC0000000)
    // тоже — WIN0_LO 0xc0000000->0, WIN0_BASE_LIMIT 0x3ff00000->0x10,
    // WIN0_BASE_HI/LIMIT_HI 0x6->0. Без него первое же MMIO-чтение xHCI
    // ловит асинхронный SError (ESR=0xbf000002) — восстанавливаем той же
    // техникой и в том же безопасном месте, что и RC_BAR2/MISC_CTRL выше.
    pcie_rc_misc_write(PCIE_MISC_CPU_2_PCIE_MEM_WIN0_LO_OFFSET, PCIE_MISC_CPU_2_PCIE_MEM_WIN0_LO_KNOWN_GOOD);
    pcie_rc_misc_write(PCIE_MISC_CPU_2_PCIE_MEM_WIN0_HI_OFFSET, PCIE_MISC_CPU_2_PCIE_MEM_WIN0_HI_KNOWN_GOOD);
    pcie_rc_misc_write(PCIE_MISC_CPU_2_PCIE_MEM_WIN0_BASE_LIMIT_OFFSET, PCIE_MISC_CPU_2_PCIE_MEM_WIN0_BASE_LIMIT_KNOWN_GOOD);
    pcie_rc_misc_write(PCIE_MISC_CPU_2_PCIE_MEM_WIN0_BASE_HI_OFFSET, PCIE_MISC_CPU_2_PCIE_MEM_WIN0_BASE_HI_KNOWN_GOOD);
    pcie_rc_misc_write(PCIE_MISC_CPU_2_PCIE_MEM_WIN0_LIMIT_HI_OFFSET, PCIE_MISC_CPU_2_PCIE_MEM_WIN0_LIMIT_HI_KNOWN_GOOD);

    // НАЙДЕНО НЕЗАВИСИМЫМ РАЗБОРОМ КОДА (issuse.txt №74/в, после 10-й
    // hw-попытки) — профилактическое восстановление ещё 2 вещей, которые
    // U-Boot делает БЕЗУСЛОВНО при штатном bring-up (platform.h содержит
    // разбор смещений/источника). Не hw-подтверждено как ПРИЧИНА SError,
    // но та же страница уже 3 раза подтверждённо стиралась целиком —
    // безопасно восстановить заодно, той же техникой.
    pcie_rc_misc_write(PCIE_MSI_INTR2_MASK_SET_OFFSET, 0xffffffffu); // маскируем все MSI — мы их не обрабатываем
    pcie_rc_misc_write(PCIE_MSI_INTR2_CLR_OFFSET, 0xffffffffu);      // и чистим всё, что успело накопиться
    pcie_rc_misc_write(PCIE_MISC_RC_BAR1_CONFIG_LO_OFFSET,
        pcie_rc_misc_read(PCIE_MISC_RC_BAR1_CONFIG_LO_OFFSET) & ~PCIE_MISC_RC_BAR_CONFIG_LO_SIZE_MASK);
    pcie_rc_misc_write(PCIE_MISC_RC_BAR3_CONFIG_LO_OFFSET,
        pcie_rc_misc_read(PCIE_MISC_RC_BAR3_CONFIG_LO_OFFSET) & ~PCIE_MISC_RC_BAR_CONFIG_LO_SIZE_MASK);

    // НАЙДЕНО ЧЕРЕЗ ВНЕШНЕЕ ИССЛЕДОВАНИЕ (issuse.txt №74/в, после 13-й
    // hw-попытки) — см. подробный разбор в platform.h у самих констант:
    // если bridge software init стирает эти два таймаута до короткого
    // POR-дефолта, RC мог молча ретраить/обрывать наши же BAR0/Command
    // config-чтения на CRS ("устройство ещё не готово"), из-за чего
    // readback выглядел успешным, хотя реального ответа от VL805 не
    // было. Восстанавливаем ЗАРАНЕЕ, до BAR0/Command ниже — та же
    // логика, что и остальной PCIE_MISC-блок выше.
    pcie_rc_misc_write(PCIE_MISC_RC_CONFIG_RETRY_TIMEOUT_OFFSET, PCIE_MISC_RC_CONFIG_RETRY_TIMEOUT_GENEROUS);
    pcie_rc_misc_write(PCIE_MISC_UBUS_TIMEOUT_OFFSET, PCIE_MISC_UBUS_TIMEOUT_GENEROUS);

    // issuse.txt №74/в — Command самого Root Port'а (bus=0) проверялась
    // отдельно (внешняя консультация + JTAG) как возможная причина
    // SError — гипотеза НЕ подтвердилась (запись оказалась полным no-op,
    // бит, вероятно, read-only на этой реализации Broadcom RC). Убрано
    // (см. память проекта/situation.txt для полного разбора).

    // 5. Снять SERDES_IDDQ.
    tmp = pcie_hard_debug_read();
    pcie_hard_debug_write(tmp & ~PCIE_MISC_HARD_PCIE_HARD_DEBUG_SERDES_IDDQ_MASK);
    seL4_SetMR(0, 8);
    seL4_SetMR(1, 1);
    seL4_Call(timer_ep, seL4_MessageInfo_new(0, 0, 0, 2));

    // 6. Release PERST.
    tmp = pcie_rgr1_read();
    pcie_rgr1_write(tmp & ~PCIE_RGR1_SW_INIT_1_PERST_MASK);

    // 7. НАЙДЕНО ЧЕРЕЗ JTAG+код (девятая hw-попытка, issuse.txt №74/в):
    // этот шаг отсутствовал вообще (нумерация прыгала с 6 на 8). Реальный
    // U-Boot (drivers/pci/pcie_brcmstb.c/brcm_pcie_probe()) ПОСЛЕ release
    // PERST делает БЕЗУСЛОВНУЮ mdelay(100) — "Wait for 100ms after PERST#
    // deassertion; see PCIe CEM specification sections 2.2, PCIe r5.0,
    // 6.6.1" (комментарий в самом U-Boot) — И ТОЛЬКО ПОТОМ начинает опрос
    // линка (см. шаг 8 ниже). У нас этой безусловной паузы не было —
    // опрос линка (и, если он мгновенно вернёт true, вообще всё
    // дальнейшее) мог начаться сразу после release PERST. Это ДРУГАЯ,
    // более ранняя точка в последовательности, чем уже проверенная (и не
    // сработавшая) 500мс-пауза после mailbox-тега (SYS_PCIE_RESET) — та
    // была про готовность прошивки VL805, эта — про readiness самого
    // PERST-деассерта по спецификации, до какой-либо проверки линка
    // вообще. WIN0/RC_BAR2/MISC_CTRL/PCIE_INTR2_CPU_STATUS/ASPM все
    // подтверждены корректными (readback+JTAG) — SError всё равно
    // повторялся, что и привело сюда.
    seL4_SetMR(0, 8); // SYS_SLEEP_MS
    seL4_SetMR(1, 100);
    seL4_Call(timer_ep, seL4_MessageInfo_new(0, 0, 0, 2));

    // 8. Опрос PHYLINKUP+DL_ACTIVE — та же формула, что проба Linux
    // (100мс, шаг 5мс: `for (i = 0; i < 100 && !link_up; i += 5)
    // msleep(5);`).
    constexpr uint32_t kLinkMask = PCIE_MISC_PCIE_STATUS_PHYLINKUP_MASK | PCIE_MISC_PCIE_STATUS_DL_ACTIVE_MASK;
    bool link_up = false;
    for (int waited_ms = 0; waited_ms < 100 && !link_up; waited_ms += 5) {
        link_up = (pcie_status_read() & kLinkMask) == kLinkMask;
        if (!link_up) {
            seL4_SetMR(0, 8);
            seL4_SetMR(1, 5);
            seL4_Call(timer_ep, seL4_MessageInfo_new(0, 0, 0, 2));
        }
    }
    if (!link_up) link_up = (pcie_status_read() & kLinkMask) == kLinkMask;
    if (!link_up) return false;

    // ============================================================
    // issuse.txt №74/в — ПРИЧИНА SError (38-я hw-попытка, доказана живой
    // JTAG-записью на упавшей плате; полный разбор — platform.h у
    // PLAT_PCIE_RC_TYPE1_PADDR и situation.txt).
    //
    // bridge software init (PCIE_RGR1_SW_INIT_1, шаги 1-4 выше) сбрасывает
    // Command САМОГО МОСТА (Root Port, шина 0) в 0x0000 — снимая Memory
    // Space Enable. Без него мост не претендует на адреса собственного
    // memory-окна, и первое же обращение к PLAT_XHCI_PADDR (0x600000000)
    // уходит в никуда -> асинхронный SError. Восстанавливаем ЗДЕСЬ —
    // ПОСЛЕ release bridge sw_init (иначе сброс снял бы биты обратно).
    //
    // Доступ ТОЛЬКО напрямую по базе (PLAT_PCIE_RC_TYPE1_ROOT_VADDR), НЕ
    // через PCIE_EXT_CFG_INDEX/DATA: для шины 0 ECAM-окно не работает
    // вообще (сверено дословно с U-Boot brcm_pcie_config_address()) —
    // именно поэтому 16-я hw-попытка сочла эту запись "no-op" и была
    // ошибочно откачена.
    //
    // Запись RW1C-safe: верхние 16 бит (Status) обнуляем, чтобы случайно
    // не сбросить залипшие статус-биты (тот же приём, что для Command
    // VL805 ниже).
    {
        volatile uint32_t *rc_cmd = (volatile uint32_t*)(PLAT_PCIE_RC_TYPE1_ROOT_VADDR + PCIE_RC_TYPE1_COMMAND_OFFSET);
        uint32_t rc_cmd_before = *rc_cmd;
        uart_puts("[ROOT] PCIE RC(мост) Command (до записи) = "); uart_puthex(rc_cmd_before); uart_puts("\n");
        *rc_cmd = (rc_cmd_before & 0xFFFFu) | PCI_CMD_MEMORY_SPACE_ENABLE | PCI_CMD_BUS_MASTER_ENABLE;
        uint32_t rc_cmd_after = *rc_cmd;
        uart_puts("[ROOT] PCIE RC(мост) Command (после записи) = "); uart_puthex(rc_cmd_after); uart_puts("\n");
        if ((rc_cmd_after & PCI_CMD_MEMORY_SPACE_ENABLE) == 0) {
            uart_puts("[ROOT] ВНИМАНИЕ: Memory Space Enable у моста НЕ встал — обращение к xHCI MMIO упадёт в SError (см. issuse.txt №74/в).\n");
        }
    }
    // ============================================================

    // НАЙДЕНО НА ЖИВОМ ЖЕЛЕЗЕ (вторая hw-попытка, issuse.txt №74/в) —
    // PERST сбрасывает конфигурационное пространство САМОГО VL805
    // (Command register/BAR0) до состояния "только что включили
    // питание" — без этого шага PHYLINKUP+DL_ACTIVE поднимаются
    // (физический линк жив), но её BAR0-MMIO (PLAT_XHCI_PADDR, через
    // УЖЕ настроенное — PERST его не трогает, это регистр RC, не
    // устройства — outbound-окно) не отвечает вообще: первое же
    // обращение к нему вешало систему целиком. Сверено с реальным
    // U-Boot (drivers/pci/pcie_brcmstb.c/brcm_pcie_config_address() +
    // arch/arm/mach-bcm283x/include/mach/acpi/bcm2711.h) — та же
    // индиректная ECAM-подобная схема, что и штатная PCI-энумерация:
    // PCIE_EXT_CFG_INDEX выбирает bus/dev/func VL805 (bus=1,slot=0,
    // func=0 — MBOX_XHCI_RESET_DEV_ADDR), затем BAR0/Command читаются/
    // пишутся через PCIE_EXT_CFG_DATA + смещение. BAR0 сначала (пока
    // Memory Space Enable ещё выключен — safe per PCI spec, не даём
    // декодировать мусорный адрес во время самой записи), Command —
    // последним.
    *(volatile uint32_t*)(PLAT_PCIE_RGR1_ROOT_VADDR + PCIE_EXT_CFG_INDEX_PAGE_OFFSET) = MBOX_XHCI_RESET_DEV_ADDR;

    // 29-я hw-попытка (issuse.txt №74/в) — контрольная точка A: индекс
    // только что выставлен, к DATA-окну ЕЩЁ НИ РАЗУ не обращались.
    // Установка индекса — регистр самого RC, TLP на VL805 не уходит,
    // так что это чистый "до каких-либо обращений к VL805" снимок.
    pcie_debug_vl805_devstatus("[ROOT]   DIAG Device Status VL805 (A: сразу после установки индекса) = ");

    // ТРЕТЬЯ hw-попытка (issuse.txt №74/в) — BAR0/Command запись САМА
    // прошла без немедленного краха (usbreset печатает "готово"), но
    // СЛЕДУЮЩЕЕ реальное обращение к xHCI MMIO из usb_driver'а (в
    // ДРУГОМ процессе, через её ЕЁ ОДИН собственный vaddr-маппинг —
    // приход к тем же физическим регистрам, что и всегда) всё равно
    // виснет — та же самая строка ("Освобождаю Slot ID"), что и до
    // этого фикса. Гипотеза: BAR0 у VL805 — НИЖНЯЯ половина 64-битного
    // BAR (тип-биты read-only, наша запись 0xC0000000 их не портит, но
    // и не задаёт verhний 32-битный BAR1/offset+4 — если он после PERST
    // остался НЕ нулевым мусором, эффективный 64-битный адрес НЕ равен
    // RPI4_XHCI_PADDR, устройство просто не декодирует ожидаемый
    // bus-адрес). НЕ проверено — печатаем ВСЁ (до/после), чтобы
    // подтвердить или опровергнуть по логу, вместо очередной слепой
    // догадки (см. память проекта — послойное живое чтение состояния).
    uint32_t bar0_before = *(volatile uint32_t*)(PLAT_PCIE_CFG_DATA_ROOT_VADDR + PCI_CFG_BAR0_OFFSET);
    uint32_t bar1_before = *(volatile uint32_t*)(PLAT_PCIE_CFG_DATA_ROOT_VADDR + PCI_CFG_BAR0_OFFSET + 4);
    uart_puts("[ROOT] PCIE CFG BAR0 (до записи) = "); uart_puthex(bar0_before); uart_puts("\n");
    uart_puts("[ROOT] PCIE CFG BAR1 (до записи) = "); uart_puthex(bar1_before); uart_puts("\n");
    // контрольная точка B: после ЧТЕНИЯ BAR0/BAR1 (Configuration Read),
    // ДО какой-либо ЗАПИСИ в конфиг-пространство VL805.
    pcie_debug_vl805_devstatus("[ROOT]   DIAG Device Status VL805 (B: после чтения BAR0/BAR1) = ");

    *(volatile uint32_t*)(PLAT_PCIE_CFG_DATA_ROOT_VADDR + PCI_CFG_BAR0_OFFSET) = (uint32_t)RPI4_XHCI_PADDR;
    // Явно обнуляем верхнюю половину (на случай 64-битного BAR0/BAR1) —
    // безопасно и для 32-битного случая (BAR1 тогда просто отдельный,
    // нами не используемый слот).
    *(volatile uint32_t*)(PLAT_PCIE_CFG_DATA_ROOT_VADDR + PCI_CFG_BAR0_OFFSET + 4) = 0;

    uint32_t bar0_after = *(volatile uint32_t*)(PLAT_PCIE_CFG_DATA_ROOT_VADDR + PCI_CFG_BAR0_OFFSET);
    uint32_t bar1_after = *(volatile uint32_t*)(PLAT_PCIE_CFG_DATA_ROOT_VADDR + PCI_CFG_BAR0_OFFSET + 4);
    uart_puts("[ROOT] PCIE CFG BAR0 (после записи) = "); uart_puthex(bar0_after); uart_puts("\n");
    uart_puts("[ROOT] PCIE CFG BAR1 (после записи) = "); uart_puthex(bar1_after); uart_puts("\n");
    // контрольная точка C: сразу после ЗАПИСИ BAR0/BAR1 (двух
    // Configuration Write) — первая наша ЗАПИСЬ в конфиг-пространство
    // VL805 за весь сброс.
    pcie_debug_vl805_devstatus("[ROOT]   DIAG Device Status VL805 (C: после записи BAR0/BAR1) = ");

    uint32_t cmd_before = *(volatile uint32_t*)(PLAT_PCIE_CFG_DATA_ROOT_VADDR + PCI_CFG_COMMAND_OFFSET);
    uart_puts("[ROOT] PCIE CFG Command (до записи) = "); uart_puthex(cmd_before); uart_puts("\n");
    // контрольная точка D: после чтения Command (следующая Configuration
    // Read после точки C).
    pcie_debug_vl805_devstatus("[ROOT]   DIAG Device Status VL805 (D: после чтения Command) = ");
    // НАЙДЕНО ПРИ РАЗБОРЕ ЭТОГО ЖЕ КУСКА ЛОГА (issuse.txt №74/в, 12-я
    // hw-попытка): верхние 16 бит этого же 32-битного слова — Status,
    // БОЛЬШИНСТВО бит которого RW1C (запись 1 = очистка). Слепой
    // `cmd_before | ENABLE-биты` переписывал бы Status ТЕМ ЖЕ значением,
    // что мы только что прочитали — а если бы там оказался реальный бит
    // ошибки (Received/Signaled Master Abort и т.п.), эта же запись
    // стёрла бы его до того, как мы успели его увидеть. Обнуляем верхние
    // 16 бит явно — пишем ТОЛЬКО в Command, Status не трогаем вообще.
    *(volatile uint32_t*)(PLAT_PCIE_CFG_DATA_ROOT_VADDR + PCI_CFG_COMMAND_OFFSET) =
        (cmd_before & 0x0000ffffu) | PCI_CMD_MEMORY_SPACE_ENABLE | PCI_CMD_BUS_MASTER_ENABLE;
    uint32_t cmd_after = *(volatile uint32_t*)(PLAT_PCIE_CFG_DATA_ROOT_VADDR + PCI_CFG_COMMAND_OFFSET);
    uart_puts("[ROOT] PCIE CFG Command (после записи) = "); uart_puthex(cmd_after); uart_puts("\n");
    // контрольная точка E: последнее наше обращение к конфиг-
    // пространству VL805 в этой функции — сразу после записи Command.
    pcie_debug_vl805_devstatus("[ROOT]   DIAG Device Status VL805 (E: после записи Command, конец функции) = ");
    // контрольная точка H — RBUS-таймаут в конце функции: если
    // отличается от G, bridge software init (или что-то ещё в этой
    // последовательности) реально его меняет.
    pcie_debug_rbus_timeout("[ROOT]   DIAG RBUS timeout (H: конец функции) = ");

    return true;
}

// issuse.txt №9 — базовая раздача ресурсов процесса в spawn_process()
// (retype CNode/VSpace/PUD/PD/PT, ELF-страницы, стек, IPC-фрейм, TCB) не
// проверяла ошибки ram_retype()/seL4_ARM_Page_Map(), непоследовательно с
// HW-фреймами драйверов чуть ниже в этой же функции (те через check_err()).
// НЕ переиспользуем сам check_err() — там halt-forever (while(1)), что
// оправдано ТОЛЬКО для одноразового boot-time кода (без MMIO-фрейма
// драйвер вообще не может жить). spawn_process() же вызывается на КАЖДЫЙ
// runtime `exec` — halt-forever здесь превратил бы истощение RAM/CSlot-
// пула (например, от одной неудачной фоновой команды) в смерть всей
// системы вместо честного отказа этого одного spawn'а. Вместо этого —
// откат уже захваченных капов через тот же cap_tracker-цикл, что
// использует generic_recover_process() при респавне, и return -1
// вызывающему (тот уже умеет обрабатывать -1 — см. другие ранние return -1
// в этой же функции).
static int abort_spawn(ProcessControlBlock &pcb, PsychAllocator &alloc, seL4_CPtr root_cnode, const char *stage) {
    uart_puts("[SPAWN] FATAL: '"); uart_puts(pcb.name); uart_puts("' - ");
    uart_puts(stage); uart_puts(" failed (RAM/CSlots exhausted?) - aborting this spawn.\n");
    for (int i = 0; i < pcb.cap_tracker.count; i++) {
        seL4_CPtr cap_to_free = pcb.cap_tracker.caps[i];
        seL4_CNode_Revoke(root_cnode, cap_to_free, seL4_WordBits);
        seL4_CNode_Delete(root_cnode, cap_to_free, seL4_WordBits);
        alloc.free(cap_to_free);
    }
    pcb.cap_tracker.count = 0;
    pcb.active = false;
    return -1;
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
                         seL4_Word blk_dma_paddr_param = 0,
                         // Фаза 4.5 (Wi-Fi data-plane) — три новых параметра,
                         // тот же принцип, что extra_ntfn_param выше, каждый
                         // читается РОВНО одним конкретным драйвером:
                         // - extra_ntfn2_param: heartbeat-капа wifi_driver'а
                         //   (badge WIFI_EVENT_HEARTBEAT из wifi_wake_ntfn),
                         //   читает ТОЛЬКО timer_driver (is_driver==2), чтобы
                         //   периодически её сигналить рядом с net-heartbeat.
                         seL4_CPtr extra_ntfn2_param = 0,
                         // - net_wifi_rx_badged_param: капа (badge
                         //   NET_EVENT_WIFI_RX из net_event_ntfn), которой
                         //   wifi_driver сигналит net_driver'у о новом RX-
                         //   кадре — читает ТОЛЬКО wifi_driver (is_driver==5),
                         //   передаётся заново при каждом (ре)спавне, как и
                         //   wifi_cmd_recv_ep выше.
                         seL4_CPtr net_wifi_rx_badged_param = 0,
                         // - net_wifi_tx_wake_param: капа (badge
                         //   WIFI_EVENT_TX_READY из wifi_wake_ntfn), которой
                         //   net_driver сигналит wifi_driver'у о новом TX-
                         //   кадре — читает ТОЛЬКО net_driver (is_driver==4).
                         seL4_CPtr net_wifi_tx_wake_param = 0,
                         // extra_ntfn3_param: капа на badged-копию blk_irq_ntfn
                         // (badge BLK_HEARTBEAT_BADGE) — читает ТОЛЬКО
                         // timer_driver (is_driver==2), чтобы периодически её
                         // сигналить рядом с net/wifi-heartbeat. Исправляет
                         // зависание blk_driver на seL4_Wait без таймаута —
                         // см. situation.txt.
                         seL4_CPtr extra_ntfn3_param = 0,
                         // mmc_irq_handler_param: собственная копия IRQHandler
                         // общей линии EMMC2/Wi-Fi SDIO — читает ТОЛЬКО
                         // blk_driver (is_driver==3), чтобы звать
                         // seL4_IRQHandler_Ack() САМ, без обратного IPC к
                         // root (фикс дедлока root<->blk_driver, см.
                         // BOOT_MMC_IRQ_HANDLER_CAP в common.h).
                         seL4_CPtr mmc_irq_handler_param = 0,
                         // blk_dma_frame2_param/blk_dma_paddr2_param: вторая
                         // приватная некэшируемая страница blk_driver'а — под
                         // ADMA2-дескриптор multi-block чтения/записи (фикс
                         // задержки, см. situation.txt) — первая страница
                         // теперь целиком занята данными (до 4096 байт).
                         seL4_CPtr blk_dma_frame2_param = 0,
                         seL4_Word blk_dma_paddr2_param = 0,
                         // vfs_mutex_ntfn_param: капа общего мьютекса на
                         // нотификации для VFS-прокси staging области в SHM
                         // (Фаза 6, SMP, см. common.h/BOOT_VFS_MUTEX_NTFN_CAP) —
                         // читают ТОЛЬКО shell/net_driver/wifi_driver, один и
                         // тот же объект без бейджа у всех троих.
                         seL4_CPtr vfs_mutex_ntfn_param = 0,
                         // Фаза 9.A (см. ROADMAP.md): cwd вызывающего шелла —
                         // ТОЛЬКО для доверенных /sbin-утилит (is_driver==253),
                         // см. EXEC_CWD_MSG_SLOT/common.h. Через boot-IPC
                         // msg[], а не через общую VFS SHM — там его затирает
                         // load_elf_from_disk() (см. комментарий у слота).
                         const char *cwd_payload = nullptr,
                         // issuse.txt (пайпинг для /sbin-команд): -1 = обычный
                         // stdout на console_ep, как всегда. >=0 — номер
                         // пайпа (g_pipes[]), созданного шеллом ДО спавна
                         // (см. SYS_PIPE/case 20) — вместо console_ep в FD 1
                         // минтится badged-копия ep с PIPE_BASE_BADGE+id, тем
                         // же способом, каким сам пайп создаётся для любого
                         // другого писателя. /sbin-утилита пишет через
                         // обычный sys_write(1, ...) — caps_or_badges[1] уже
                         // generic, никаких изменений в самих утилитах не
                         // нужно (см. h/sys_client.h).
                         int stdout_pipe_id = -1,
                         // Фаза 14 (USB, xHCI) — только для is_driver==6.
                         // usb_cmd_recv_ep_param: raw endpoint, на котором
                         // usb_driver слушает (root держит СВОЮ же копию
                         // как send-сторону, см. usb_cmd_ep в main() —
                         // root-опосредованный доступ, тот же приём, что
                         // SYS_WIFI_STATUS/SYS_TOP_STATS, поэтому НИКАКОЙ
                         // другой процесс не нуждается в отдельной send-
                         // капе, в отличие от net/wifi). usb_dma_param —
                         // см. UsbDmaSetup выше.
                         seL4_CPtr usb_cmd_recv_ep_param = 0,
                         const UsbDmaSetup *usb_dma_param = nullptr,
                         // Пятнадцатая попытка (см. ROADMAP.md/platform.h
                         // PLAT_PCIE_RC_MISC_PADDR) — фрейм под PCIe RC
                         // MISC-регистры (outbound-window), usb_driver сам
                         // прописывает окно 1 для трансляции BAR0 xHCI.
                         seL4_CPtr pcie_rc_frame_param = 0,
                         // Двадцать четвёртая попытка (см. ROADMAP.md/
                         // platform.h PLAT_PCIE_ERR_PADDR) — фрейм под
                         // PCIE_OUTB_ERR_* (диагностика AXI-ошибок исходящих
                         // от моста транзакций, включая DMA устройства->RAM).
                         seL4_CPtr pcie_err_frame_param = 0,
                         // Milestone 9 (Фаза 14, закрытие) — клиентский
                         // Call-капа на usb_driver'ов командный endpoint (та
                         // же send-капа, что root держит как usb_cmd_ep) —
                         // тот же приём, что blk_ep, но выдаётся ТОЛЬКО
                         // shell(0) и доверенным /sbin(253) на call-сайтах
                         // (см. план/ROADMAP.md), остальные ~5 спавнов не
                         // трогаются (дефолт 0).
                         seL4_CPtr usb_storage_ep_param = 0,
                         // Milestone 11 (доп., по запросу пользователя) —
                         // badged-копия usb_irq_ntfn (badge
                         // USB_EVENT_HEARTBEAT, см. common.h) — читает
                         // ТОЛЬКО timer_driver (is_driver==2), тот же
                         // принцип, что extra_ntfn3_param (blk heartbeat)
                         // выше, отдельный параметр — тот уже занят.
                         seL4_CPtr usb_heartbeat_ntfn_param = 0,
                         // Фаза 3b плана "Сигналы драйверам" — badged-копия
                         // root'ового mmc_shared_irq_ntfn (DRIVER_LIVENESS_*_
                         // BADGE), которой ЭТОТ процесс сам сигналит root'у
                         // "я жив" на heartbeat-тике — читают ТОЛЬКО is_driver
                         // 3/4/5/6 (blk/net/wifi/usb), обычное копирование
                         // capability, не TCB-bind (тот же принцип, что и
                         // остальные *_ntfn_param выше).
                         seL4_CPtr liveness_ntfn_param = 0,
                         // Фаза 3b — badged-копия blk_liveness_tick_ntfn
                         // (badge BLK_LIVENESS_TICK_BADGE) — читает ТОЛЬКО
                         // timer_driver (is_driver==2), чтобы периодически
                         // сигналить её рядом с net/wifi/blk/usb-heartbeat
                         // (см. BOOT_BLK_LIVENESS_TICK_NTFN_CAP/common.h).
                         seL4_CPtr blk_liveness_tick_param = 0,
                         // Начало GPIO-драйвера (см. h/gpio.h) — фрейм
                         // регистров GPIO-контроллера (RPI4_GPIO_PADDR,
                         // platform.h), читает ТОЛЬКО blk_driver
                         // (is_driver==3) — мигание ACT LED при обращении
                         // к SD. Обычное копирование capability, не TCB-bind,
                         // тот же приём, что mbox_regs_frame выше.
                         seL4_CPtr gpio_frame_param = 0,
                         // issuse.txt №73/№75 — фрейм регистров PM_WDOG/
                         // PM_RSTC (platform.h/PLAT_PM_PADDR), читает
                         // ТОЛЬКО timer_driver (is_driver==2) — "кормит"
                         // его на каждом своём heartbeat-тике, последний
                         // рубеж на случай, если сам CPU застрянет
                         // настолько жёстко (PCIe-транзакция без ответа),
                         // что даже JTAG-halt не проходит.
                         seL4_CPtr pm_wdog_frame_param = 0,
                         // issuse.txt №74/в — 37-я hw-попытка: раньше
                         // usb_driver просила root прочитать Device Status
                         // VL805 через IPC (SYS_PCIE_VL805_DEVSTATUS_PROBE) —
                         // НА ЖИВОМ ЖЕЛЕЗЕ выяснилось, что этот вызов
                         // выполняется, ПОКА root САМ висит в своём
                         // seL4_Call на usb_cmd_ep (форвард DRIVER_SIGNAL_
                         // RESTART) — то есть root не может нормально его
                         // обслужить, и что-то (не главный switch(syscall_num)
                         // — тот дал бы -1) эхом отражает MR0 обратно.
                         // Вместо попытки чинить реентерабельность — тот же
                         // приём, что pcie_rc_frame_param/pcie_err_frame_param
                         // выше: даём usb_driver ПРЯМОЙ read-only доступ к
                         // ОДНОЙ странице PLAT_PCIE_CFG_DATA_PADDR (НЕ ко
                         // всей PLAT_PCIE_RGR1_PADDR — там PERST/bridge-init
                         // биты, которые usb_driver трогать не должна;
                         // PCIE_EXT_CFG_INDEX она НЕ переустанавливает сама,
                         // полагаясь на то, что root уже выставил его перед
                         // forward'ом RESTART и больше никто его не меняет).
                         seL4_CPtr pcie_cfg_data_frame_param = 0) {

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
    if (ram_retype(normal_untyped, seL4_CapTableObject, 8, root_cnode, 0, 0, child_cnode, 1, true) != seL4_NoError)
        return abort_spawn(pcb, alloc, root_cnode, "retype CNode");

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
    seL4_Word local_extra_ntfn2 = 14; // Фаза 4.5 (Wi-Fi data-plane): heartbeat-капа wifi_driver'а (только для timer_driver, см. extra_ntfn2_param)
    seL4_Word local_net_wifi_rx = 15; // Фаза 4.5 (Wi-Fi data-plane): капа wifi_driver->net_driver сигнала RX (только для wifi_driver, см. net_wifi_rx_badged_param)
    seL4_Word local_wifi_tx_wake = 16; // Фаза 4.5 (Wi-Fi data-plane): капа net_driver->wifi_driver сигнала TX (только для net_driver, см. net_wifi_tx_wake_param)
    seL4_Word local_extra_ntfn3 = 17; // Фикс зависания blk_driver: heartbeat-капа blk_driver'а (badge BLK_HEARTBEAT_BADGE, только для timer_driver, см. extra_ntfn3_param)
    seL4_Word local_mmc_irq_handler = 18; // Фикс дедлока root<->blk_driver: собственная копия IRQHandler общей линии (только для blk_driver, см. mmc_irq_handler_param)
    seL4_Word local_vfs_mutex_ntfn = 19; // Фаза 6 (SMP): капа общего VFS-мьютекса (только для shell/net_driver/wifi_driver, см. vfs_mutex_ntfn_param)
    seL4_Word local_self_tcb = 20; // Фаза 6.1 (продолжение): собственная TCB-капа (только для uart/blk/net/wifi, is_driver 1/3/4/5)
    seL4_Word local_usb_recv_ep = 21; // Фаза 14 (USB): usb_driver слушает на этом слоте (только для is_driver==6, см. usb_cmd_recv_ep_param)
    // ВНИМАНИЕ: 22 занят local_stdout_pipe_ep (см. ниже, пайпинг /sbin-
    // команд) — найдено при добавлении heartbeat-слота ниже: local_usb_
    // storage_ep изначально тоже стоял на 22, что тихо ломало бы `cmd |
    // other` для доверенных /sbin-команд (обе Mint на один слот, вторая
    // упала бы, т.к. slot уже занят). 23/24 — свободны.
    seL4_Word local_usb_storage_ep = 23; // Milestone 9: клиентская капа на usb_driver'ов VFS-диспетчер (только shell/253, см. usb_storage_ep_param)
    seL4_Word local_usb_heartbeat_ntfn = 24; // Milestone 11: heartbeat-капа usb_driver'а (только для timer_driver, см. usb_heartbeat_ntfn_param)
    seL4_Word local_liveness_ntfn = 25; // Фаза 3b: капа-"я жив" (только для is_driver 3/4/5/6, см. liveness_ntfn_param)
    seL4_Word local_blk_liveness_tick = 26; // Фаза 3b: тик для blk_driver (только для timer_driver, см. blk_liveness_tick_param)
    seL4_Word local_root_wd_tick = 27; // тик heartbeat-watchdog'а root'а (только для timer_driver, см. g_root_watchdog_tick_badged)

    check_err(seL4_CNode_Copy(child_cnode, local_syscall_ep, 8, root_cnode, badged_ep, seL4_WordBits, seL4_AllRights), "Copy syscall ep");

    if (is_driver == 1 || is_driver == 2 || is_driver == 3 || is_driver == 4 || is_driver == 6) {
        // UART/Timer/Block/Net/USB driver: капа на СОБСТВЕННЫЙ CNode (см.
        // SELF_CNODE_SLOT в common.h) — нужна для seL4_CNode_SaveCaller()
        // внутри самого процесса, чтобы откладывать reply вместо немедленного
        // ответа (UART: SYS_READ, Timer: SYS_SLEEP_MS, Фаза 4.5; Net:
        // NET_CMD_PING, фикс низкочастотных мейлбоксов — см. situation.txt,
        // net_driver.cpp; Block/USB: issuse.txt №69 — SaveCaller на КАЖДУЮ
        // VFS-команду, чтобы root мог вытащить reply-капу зависшего клиента,
        // если сам driver умрёт посреди обработки, см. VFS_PENDING_REPLY_SLOT).
        // child_cnode здесь — это capability НА ТОТ ЖЕ CNode-объект в
        // адресном пространстве root_cnode; копия внутрь себя же — обычный
        // seL4-приём для self-reference.
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
    if (extra_ntfn2_param != 0) check_err(seL4_CNode_Copy(child_cnode, local_extra_ntfn2, 8, root_cnode, extra_ntfn2_param, seL4_WordBits, seL4_AllRights), "Copy extra ntfn2 (wifi heartbeat)");
    if (net_wifi_rx_badged_param != 0) check_err(seL4_CNode_Copy(child_cnode, local_net_wifi_rx, 8, root_cnode, net_wifi_rx_badged_param, seL4_WordBits, seL4_AllRights), "Copy net_wifi_rx signal cap");
    if (net_wifi_tx_wake_param != 0) check_err(seL4_CNode_Copy(child_cnode, local_wifi_tx_wake, 8, root_cnode, net_wifi_tx_wake_param, seL4_WordBits, seL4_AllRights), "Copy wifi_tx_wake signal cap");
    if (extra_ntfn3_param != 0) check_err(seL4_CNode_Copy(child_cnode, local_extra_ntfn3, 8, root_cnode, extra_ntfn3_param, seL4_WordBits, seL4_AllRights), "Copy extra ntfn3 (blk heartbeat)");
    if (mmc_irq_handler_param != 0) check_err(seL4_CNode_Copy(child_cnode, local_mmc_irq_handler, 8, root_cnode, mmc_irq_handler_param, seL4_WordBits, seL4_AllRights), "Copy mmc irq handler (blk self-ack)");
    if (vfs_mutex_ntfn_param != 0) check_err(seL4_CNode_Copy(child_cnode, local_vfs_mutex_ntfn, 8, root_cnode, vfs_mutex_ntfn_param, seL4_WordBits, seL4_AllRights), "Copy vfs_mutex_ntfn");
    if (usb_cmd_recv_ep_param != 0) check_err(seL4_CNode_Copy(child_cnode, local_usb_recv_ep, 8, root_cnode, usb_cmd_recv_ep_param, seL4_WordBits, seL4_AllRights), "Copy usb recv ep");
    if (liveness_ntfn_param != 0) check_err(seL4_CNode_Copy(child_cnode, local_liveness_ntfn, 8, root_cnode, liveness_ntfn_param, seL4_WordBits, seL4_AllRights), "Copy liveness ntfn");
    if (blk_liveness_tick_param != 0) check_err(seL4_CNode_Copy(child_cnode, local_blk_liveness_tick, 8, root_cnode, blk_liveness_tick_param, seL4_WordBits, seL4_AllRights), "Copy blk liveness tick ntfn");
    // Тик heartbeat-watchdog'а root'а — только timer_driver (см.
    // common.h/ROOT_WATCHDOG_TICK_BADGE). Глобал, а не параметр, — см.
    // комментарий у самого g_root_watchdog_tick_badged.
    if (is_driver == 2 && g_root_watchdog_tick_badged != 0) {
        check_err(seL4_CNode_Copy(child_cnode, local_root_wd_tick, 8, root_cnode, g_root_watchdog_tick_badged, seL4_WordBits, seL4_AllRights), "Copy root watchdog tick ntfn");
    }
    if (usb_storage_ep_param != 0) check_err(seL4_CNode_Mint(child_cnode, local_usb_storage_ep, 8, root_cnode, usb_storage_ep_param, seL4_WordBits, seL4_AllRights, pid), "Mint usb storage ep");
    if (usb_heartbeat_ntfn_param != 0) check_err(seL4_CNode_Copy(child_cnode, local_usb_heartbeat_ntfn, 8, root_cnode, usb_heartbeat_ntfn_param, seL4_WordBits, seL4_AllRights), "Copy usb heartbeat ntfn");

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
    pcb.net_wifi_rx_badged = net_wifi_rx_badged_param;
    pcb.wifi_tx_wake_badged = net_wifi_tx_wake_param;
    pcb.blk_heartbeat_badged = extra_ntfn3_param;
    pcb.blk_liveness_tick_badged = blk_liveness_tick_param; // Фаза 3b
    pcb.mmc_irq_handler = mmc_irq_handler_param;
    // issuse.txt №8 — раньше НЕ сохранялись, respawn timer_driver передавал
    // сюда захардкоженные 0,0,0 (см. generic_recover_process()), молча теряя
    // VideoCore mailbox навсегда (DVFS/cpufreq переставал работать без единой
    // ошибки в логе).
    pcb.mbox_regs_frame = mbox_regs_frame;
    pcb.mbox_buf_frame = mbox_buf_frame_param;
    pcb.mbox_buf_paddr = mbox_buf_paddr_param;
    // issuse.txt №65 — тот же приём, для DMA-страниц blk_driver'а.
    pcb.blk_dma_frame = blk_dma_frame_param;
    pcb.blk_dma_paddr = blk_dma_paddr_param;
    pcb.blk_dma_frame2 = blk_dma_frame2_param;
    pcb.blk_dma_paddr2 = blk_dma_paddr2_param;
    // Начало GPIO-драйвера (см. h/gpio.h) — тот же приём, для respawn.
    pcb.gpio_frame = gpio_frame_param;
    // issuse.txt №73/№75 — тот же приём, для respawn.
    pcb.pm_wdog_frame = pm_wdog_frame_param;
    // Фаза 3b — см. поле в struct ProcessControlBlock выше.
    pcb.liveness_ntfn_badged = liveness_ntfn_param;
    // Фаза 3b — сбрасываем last_seen на "ещё не проверялся" (0) при каждом
    // (ре)спавне. НАЙДЕНО НА ЖИВОМ ЖЕЛЕЗЕ: первая версия синхронно звала
    // timer_ep здесь (SYS_GET_UPTIME), чтобы получить настоящее "сейчас" —
    // это ДЕДЛОК при первом же спавне blk_driver во время линейной загрузки:
    // root (внутри spawn_process()) ждёт ответа от timer_driver, а
    // timer_driver в этот самый момент сам блокирован на seL4_Call(root_ep,
    // SYS_DRIVER_READY) внутри СВОЕГО старта (см. timer_driver.cpp) — root
    // ещё не дошёл до главного цикла, чтобы этот вызов обработать. Простой
    // сброс в 0 не нуждается ни в каком IPC и достаточен: скан таймаутов
    // (см. главный цикл ниже) уже пропускает last_seen==0 как "ещё не
    // проверялся" — драйвер просто не проверяется, пока не придёт его
    // первый настоящий heartbeat-тик (обычно в пределах ~20мс), который и
    // выставит last_seen в реальное "сейчас". Важно для РЕСПАВНА конкретно:
    // без сброса last_seen[is_driver] осталось бы старым (просроченным)
    // значением от УБИТОГО экземпляра, и скан немедленно попытался бы
    // "восстановить" ещё не успевший ответить свежий процесс повторно.
    if (is_driver == 3 || is_driver == 4 || is_driver == 5 || is_driver == 6) {
        g_driver_last_seen_ms[is_driver] = 0;
    }

    elf_t elf;
    elf_newFile(elf_file, elf_size, &elf);
    uint64_t entry_point = elf_getEntryPoint(&elf);

    // VSpace (Виртуальная память песочницы)
    seL4_CPtr child_vspace = alloc_and_track_cap(alloc, pcb);
    seL4_CPtr child_pud = alloc_and_track_cap(alloc, pcb);
    seL4_CPtr child_pd  = alloc_and_track_cap(alloc, pcb);
    seL4_CPtr child_pt  = alloc_and_track_cap(alloc, pcb);
    seL4_CPtr child_pt2 = alloc_and_track_cap(alloc, pcb); // Вторая таблица для потоков

    if (ram_retype(normal_untyped, seL4_ARM_PageGlobalDirectoryObject, 0, root_cnode, 0, 0, child_vspace, 1, true) != seL4_NoError)
        return abort_spawn(pcb, alloc, root_cnode, "retype VSpace");
    if (ram_retype(normal_untyped, seL4_ARM_PageUpperDirectoryObject, 0, root_cnode, 0, 0, child_pud, 1, true) != seL4_NoError)
        return abort_spawn(pcb, alloc, root_cnode, "retype PUD");
    if (ram_retype(normal_untyped, seL4_ARM_PageDirectoryObject, 0, root_cnode, 0, 0, child_pd, 1, true) != seL4_NoError)
        return abort_spawn(pcb, alloc, root_cnode, "retype PD");
    if (ram_retype(normal_untyped, seL4_ARM_PageTableObject, 0, root_cnode, 0, 0, child_pt, 1, true) != seL4_NoError)
        return abort_spawn(pcb, alloc, root_cnode, "retype PT");
    if (ram_retype(normal_untyped, seL4_ARM_PageTableObject, 0, root_cnode, 0, 0, child_pt2, 1, true) != seL4_NoError) // Вторая таблица для потоков
        return abort_spawn(pcb, alloc, root_cnode, "retype PT2");

    if (seL4_ARM_ASIDPool_Assign(seL4_CapInitThreadASIDPool, child_vspace) != seL4_NoError)
        return abort_spawn(pcb, alloc, root_cnode, "ASIDPool_Assign");
    if (seL4_ARM_PageUpperDirectory_Map(child_pud, child_vspace, 0x400000, seL4_ARM_Default_VMAttributes) != seL4_NoError)
        return abort_spawn(pcb, alloc, root_cnode, "map PUD");
    if (seL4_ARM_PageDirectory_Map(child_pd, child_vspace, 0x400000, seL4_ARM_Default_VMAttributes) != seL4_NoError)
        return abort_spawn(pcb, alloc, root_cnode, "map PD");
    if (seL4_ARM_PageTable_Map(child_pt, child_vspace, 0x400000, seL4_ARM_Default_VMAttributes) != seL4_NoError) // Покрывает 0x400000 - 0x5FFFFF
        return abort_spawn(pcb, alloc, root_cnode, "map PT");
    if (seL4_ARM_PageTable_Map(child_pt2, child_vspace, 0x600000, seL4_ARM_Default_VMAttributes) != seL4_NoError) // Покрывает 0x600000 - 0x7FFFFF (Тут живут наши потоки)
        return abort_spawn(pcb, alloc, root_cnode, "map PT2");

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
                if (ram_retype(normal_untyped, seL4_ARM_SmallPageObject, 0, root_cnode, 0, 0, frame, 1) != seL4_NoError)
                    return abort_spawn(pcb, alloc, root_cnode, "retype ELF page");

                // Используем скользящее окно!
                if (seL4_ARM_Page_Map(frame, root_vspace, elf_temp_vaddr, seL4_AllRights, seL4_ARM_Default_VMAttributes) != seL4_NoError)
                    return abort_spawn(pcb, alloc, root_cnode, "map ELF page to root");
                memset((void*)elf_temp_vaddr, 0, 4096);

                uint64_t copy_start = (page > vaddr) ? page : vaddr;
                uint64_t copy_end = (page + 4096 < vaddr + filesz) ? page + 4096 : vaddr + filesz;
                if (copy_start < copy_end) {
                    memcpy((void*)(elf_temp_vaddr + (copy_start - page)), elf_file + offset + (copy_start - vaddr), copy_end - copy_start);
                }
                // issuse.txt №3 — ПРИЧИНА ПЛАВАЮЩЕГО ПАДЕНИЯ С PC=0.
                // Здесь в страницу только что записан КОД, а не данные, и
                // раньше стоял seL4_ARM_Page_Clean_Data — он сбрасывает
                // только кэш ДАННЫХ. Кэш ИНСТРУКЦИЙ при этом сохранял
                // строки предыдущего процесса: все /sbin-бинарники
                // слинкованы на один и тот же адрес 0x400000, поэтому
                // свежий процесс исполнял чужой код по своим же адресам.
                // Unify_Instruction делает то, что нужно для кода: чистит
                // D-кэш до точки когерентности И инвалидирует I-кэш.
                //
                // Почему отказ выглядел плавающим и почему контрольный
                // опыт его не ловил: если подряд запускать ОДИН И ТОТ ЖЕ
                // бинарник (100 спавнов /bin/test_app.elf — 100/100 чисто),
                // устаревшие строки I-кэша совпадают с тем, что там и
                // должно быть, и вреда нет. Ломается только связка ДВУХ
                // РАЗНЫХ утилит подряд — ровно та, на которой отказ и
                // наблюдался.
                //
                // Подтверждение из дампа fault'а (2026-09-06, два прогона с
                // побайтово одинаковыми регистрами): LR=0x403454 — адрес
                // возврата после `blr x0` в __sel4runtime_run_destructors
                // ИЗ hotplugtest.elf, хотя падал spawnloop.elf. Код был
                // чужой (устаревший I-кэш), а данные свои (D-кэш чистился
                // корректно) — поэтому .fini_array не совпал с кодом и в
                // x0 пришёл ноль.
                seL4_ARM_Page_Unify_Instruction(frame, 0, 4096);
                seL4_ARM_Page_Unmap(frame);

                if (seL4_ARM_Page_Map(frame, child_vspace, page, seL4_AllRights, seL4_ARM_Default_VMAttributes) != seL4_NoError)
                    return abort_spawn(pcb, alloc, root_cnode, "map ELF page to child");
            }
        }
    }

    // НАЙДЕНО НА ЖИВОМ ЖЕЛЕЗЕ (см. ROADMAP.md/issuse.txt — FATAL FAULT у
    // blk_driver на Mem Addr чуть НИЖЕ 0x500000 сразу после первых же
    // реальных exfat_mkdir() при монтировании exFAT, Этап B): раньше на
    // каждый процесс выделялась РОВНО одна страница (4КБ) стека. Глубокий
    // вызывной путь Этапа B (exfat_mkdir -> exfat_resolve_parent
    // [exfat_normalize_path: char[256]+char[16][64] на кадр] ->
    // exfat_dir_scan [ExfatDirEntry: char[256]+DirCursor(~544Б с учётом
    // sector_buf[512])] -> bitmap_alloc_run -> exfat_write_entry_set ->
    // find_free_slot_run [два DirCursor подряд] -> exfat_write_entry_set_at
    // [entries[608]+DirCursor]) суммарно перевалил за 4096 байт — SP ушёл
    // ниже нижней границы единственной замапленной страницы, в
    // несуществующую память. Стек теперь 4 страницы (16КБ) — с большим
    // запасом, RAM на плате 3.9ГиБ, лишние ~12КБ на процесс ни на что не
    // влияют. Верх стека (0x501000, вплотную под IPC-страницей child_ipc)
    // НЕ меняется — расширяем только вниз, ничего больше в кодовой базе не
    // завязано на конкретный адрес низа (проверено grep'ом).
    constexpr int STACK_PAGES = 4;
    uintptr_t child_stack = 0x500000 - (uintptr_t)(STACK_PAGES - 1) * 0x1000;
    uintptr_t child_ipc   = 0x501000;
    seL4_CPtr stack_frames[STACK_PAGES];
    for (int i = 0; i < STACK_PAGES; i++) {
        stack_frames[i] = alloc_and_track_cap(alloc, pcb);
        if (ram_retype(normal_untyped, seL4_ARM_SmallPageObject, 0, root_cnode, 0, 0, stack_frames[i], 1) != seL4_NoError)
            return abort_spawn(pcb, alloc, root_cnode, "retype stack page");
    }
    seL4_CPtr ipc_frame = alloc_and_track_cap(alloc, pcb);
    if (ram_retype(normal_untyped, seL4_ARM_SmallPageObject, 0, root_cnode, 0, 0, ipc_frame, 1) != seL4_NoError)
        return abort_spawn(pcb, alloc, root_cnode, "retype IPC frame");

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
    // issuse.txt №61: раньше strcpy без проверки длины — args_payload/
    // cwd_payload длиннее своей зоны затирали соседнюю зону, а при
    // достаточной длине — и boot-капы на msg[100+]. Граница проверяется
    // здесь же, а не полагается на инвариант вызывающего.
    // issuse.txt №30 (расследование, найдено на живом железе): было
    // 63 символа — `touch /root/<51 символ>` создавал файл КОРОЧЕ на 10
    // символов, потому что клиентские буферы упаковки (shell.cpp
    // exec_payload/app_name_and_args ниже) были ЕЩЁ теснее (64 байта на
    // "/sbin/<cmd>.elf " + сам аргумент). Расширено до 127 символов.
    //
    // 2026-09-06, найдено на живом железе: зона аргументов ПЕРЕЕХАЛА с
    // msg[0] на EXEC_ARGS_MSG_SLOT. Обещанные 127 символов на msg[0]
    // никогда не работали — реальный предел был 55, потому что msg[7]
    // (BOOT_BLK_EP, он же байты 56..63 строки) записывается СВОИМ
    // значением ПОЗЖЕ этого блока и рвал строку посередине. Прежний
    // комментарий здесь это упоминал, но рассуждал только о сохранности
    // значения msg[7], а не строки. Полный разбор — у EXEC_ARGS_MSG_SLOT
    // в common.h.
    if (args_payload && args_payload[0] != '\0') {
        size_t args_len = strlen(args_payload);
        if (args_len > EXEC_ARGS_MAX_LEN) args_len = EXEC_ARGS_MAX_LEN;
        memcpy((char*)&child_ipc_ptr->msg[EXEC_ARGS_MSG_SLOT], args_payload, args_len);
        ((char*)&child_ipc_ptr->msg[EXEC_ARGS_MSG_SLOT])[args_len] = '\0';
    }
    // Фаза 9.A (см. ROADMAP.md/EXEC_CWD_MSG_SLOT): cwd вызывающего шелла для
    // доверенных /sbin-утилит — отдельный слот, не пересекается ни с
    // args_payload (msg[0..7]) выше, ни с boot-капами (100+) ниже.
    if (cwd_payload && cwd_payload[0] != '\0') {
        size_t cwd_len = strlen(cwd_payload);
        if (cwd_len > 63) cwd_len = 63;
        memcpy((char*)&child_ipc_ptr->msg[EXEC_CWD_MSG_SLOT], cwd_payload, cwd_len);
        ((char*)&child_ipc_ptr->msg[EXEC_CWD_MSG_SLOT])[cwd_len] = '\0';
    }

    // ИСПРАВЛЕНИЕ: Мы не можем передавать Capability из CSpace ядра напрямую.
    // Вместо этого мы используем локальные слоты, в которые мы уже сминтовали
    // нужные capabilities (в данном случае, console_ep).
    child_ipc_ptr->caps_or_badges[0] = local_console_ep; // FD 0 = STDIN
    child_ipc_ptr->caps_or_badges[2] = local_console_ep; // FD 2 = STDERR

    // issuse.txt (пайпинг для /sbin-команд, левая сторона): если вызывающий
    // (шелл) уже создал пайп через SYS_PIPE и передал его номер, минтим
    // FD 1 (STDOUT) на badged-копию ep с тем же PIPE_BASE_BADGE+id, каким
    // сам пайп и адресуется (см. case 20/case 8 ниже) — точно так же, как
    // получает свою капу на пайп ЛЮБОЙ другой писатель. sys_write(1, ...) в
    // /sbin-утилите (h/sys_client.h) уже общий по FD, менять её код не
    // нужно. -1 (по умолчанию) — обычный console_ep, как было всегда.
    if (stdout_pipe_id >= 0 && stdout_pipe_id < MAX_PIPES) {
        constexpr seL4_Word local_stdout_pipe_ep = 22; // свободный слот, см. local_* выше (0-21, 23-24 заняты)
        seL4_Word pipe_badge = PIPE_BASE_BADGE + stdout_pipe_id;
        seL4_CNode_Mint(child_cnode, local_stdout_pipe_ep, 8, root_cnode, ep, seL4_WordBits, seL4_AllRights, pipe_badge);
        child_ipc_ptr->caps_or_badges[1] = local_stdout_pipe_ep;
        // issuse.txt №7 (найдено при расследовании зависания) — writer_pid
        // раньше оставался PID'ом шелла (кто вызвал SYS_PIPE), никогда не
        // переставлялся на реального EXEC'нутого писателя. Из-за этого
        // авто-EOF в SYS_EXIT/SYS_THREAD_EXIT (сравнение writer_pid ==
        // sender_pid) был мёртвым кодом для ЛЮБОГО /sbin-писателя вроде
        // ls — работало только благодаря отдельному, безусловному
        // sys_pipe_wr_close() в shell.cpp. Если писатель падает/убит
        // (не штатный exit) вместо этого явного close — читатель раньше
        // завис бы навсегда без EOF.
        g_pipes[stdout_pipe_id].writer_pid = pid;
    } else {
        child_ipc_ptr->caps_or_badges[1] = local_console_ep; // FD 1 = STDOUT
    }

    child_ipc_ptr->msg[BOOT_ROOT_EP] = local_syscall_ep;

    // is_driver == 2 (timer): ARM generic timer сам по себе читается прямой
    // mrs-инструкцией из EL0 и не мапится как MMIO (см. hw_timer.cpp, main()
    // выше) — но тот же процесс теперь дополнительно читает термодатчик AVS
    // RO thermal, который MMIO-регистр как обычно, поэтому hw_frame для
    // timer_driver больше не всегда 0 (см. avs_frame выше).
    if (hw_frame != 0 && (is_driver == 1 || is_driver == 2 || is_driver == 3 || is_driver == 4 || is_driver == 5 || is_driver == 6)) { // Any driver with real MMIO
        seL4_CPtr drv_pud = alloc_and_track_cap(alloc, pcb);
        seL4_CPtr drv_pd  = alloc_and_track_cap(alloc, pcb);
        seL4_CPtr drv_pt  = alloc_and_track_cap(alloc, pcb);
        check_err(ram_retype(normal_untyped, seL4_ARM_PageUpperDirectoryObject, 0, root_cnode, 0, 0, drv_pud, 1), "Retype Drv PUD");
        check_err(ram_retype(normal_untyped, seL4_ARM_PageDirectoryObject, 0, root_cnode, 0, 0, drv_pd, 1), "Retype Drv PD");
        check_err(ram_retype(normal_untyped, seL4_ARM_PageTableObject, 0, root_cnode, 0, 0, drv_pt, 1), "Retype Drv PT");

        uintptr_t hw_vaddr = (is_driver == 1) ? PLAT_UART_VADDR :
                             (is_driver == 2) ? PLAT_AVS_VADDR :
                             (is_driver == 5) ? PLAT_WIFI_SDIO_VADDR :
                             (is_driver == 6) ? PLAT_XHCI_VADDR : // Фаза 14 (USB) — собственное 2MB-окно, см. platform.h
                             ((is_driver == 3) ? PLAT_EMMC_VADDR : PLAT_GENET_VADDR);

        seL4_ARM_PageUpperDirectory_Map(drv_pud, child_vspace, hw_vaddr, (seL4_ARM_VMAttributes)0);
        seL4_ARM_PageDirectory_Map(drv_pd, child_vspace, hw_vaddr, (seL4_ARM_VMAttributes)0);
        seL4_ARM_PageTable_Map(drv_pt, child_vspace, hw_vaddr, (seL4_ARM_VMAttributes)0);

        // EMMC2 умещается в одну страницу (0x100 байт регистров). GENET
        // занимает целых 64KB (0x10000) — 16 страниц. xHCI (Фаза 14) —
        // считается из PLAT_XHCI_SIZE (живое железо: реальный BAR 4KB, 1
        // страница — см. platform.h, изначально ошибочно предполагали 1MB).
        int num_pages = (is_driver == 6) ? (int)(PLAT_XHCI_SIZE / 4096) : (is_driver == 4) ? 16 : 1;
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
        // Фикс задержки (см. situation.txt): вторая страница, только под
        // ADMA2-дескриптор multi-block чтения/записи — сразу за первой.
        if (is_driver == 3 && blk_dma_frame2_param != 0) {
            seL4_CPtr blk_dma_child2 = alloc_and_track_cap(alloc, pcb);
            check_err(seL4_CNode_Copy(root_cnode, blk_dma_child2, seL4_WordBits,
                                      root_cnode, blk_dma_frame2_param, seL4_WordBits, seL4_AllRights), "Copy Blk DMA Frame2 Cap");
            check_err(seL4_ARM_Page_Map(blk_dma_child2, child_vspace, PLAT_BLK_DMA_VADDR + 0x1000,
                                        seL4_AllRights, (seL4_ARM_VMAttributes)0), "Map Blk DMA Buf2 to Driver");
        }
        // Начало GPIO-драйвера (см. h/gpio.h) — регистры GPIO-контроллера
        // (RPI4_GPIO_PADDR, одна страница), тот же приём, что DMA-страницы
        // выше — Page_Map поверх уже созданной для is_driver==3 иерархии
        // drv_pud/drv_pd/drv_pt, отдельная PUD/PD/PT не нужна.
        if (is_driver == 3 && gpio_frame_param != 0) {
            seL4_CPtr gpio_child = alloc_and_track_cap(alloc, pcb);
            check_err(seL4_CNode_Copy(root_cnode, gpio_child, seL4_WordBits,
                                      root_cnode, gpio_frame_param, seL4_WordBits, seL4_AllRights), "Copy GPIO Frame Cap");
            check_err(seL4_ARM_Page_Map(gpio_child, child_vspace, PLAT_GPIO_VADDR,
                                        seL4_AllRights, (seL4_ARM_VMAttributes)0), "Map GPIO to Driver");
        }
        // issuse.txt №73/№75 — аппаратный watchdog (PM_WDOG/PM_RSTC),
        // тот же приём, что mbox_regs_frame выше — тот же 2MB-регион,
        // отдельная PUD/PD/PT не нужна. Только timer_driver.
        if (is_driver == 2 && pm_wdog_frame_param != 0) {
            seL4_CPtr pm_wdog_child = alloc_and_track_cap(alloc, pcb);
            check_err(seL4_CNode_Copy(root_cnode, pm_wdog_child, seL4_WordBits,
                                      root_cnode, pm_wdog_frame_param, seL4_WordBits, seL4_AllRights), "Copy PM Wdog Frame Cap");
            check_err(seL4_ARM_Page_Map(pm_wdog_child, child_vspace, PLAT_PM_VADDR,
                                        seL4_AllRights, (seL4_ARM_VMAttributes)0), "Map PM Wdog to Driver");
        }

        // Фаза 14 (USB, xHCI) — приватные DMA-страницы usb_driver'а, все в
        // ТОМ ЖЕ 2MB-окне, что PLAT_XHCI_VADDR выше (drv_pud/pd/pt для
        // is_driver==6 уже создаются в этом же блоке) — тот же приём, что
        // mbox_buf/blk_dma выше, просто на 7 фиксированных структур + до
        // USB_MAX_SCRATCHPAD_PAGES scratchpad-страниц вместо одной-двух.
        if (is_driver == 6 && usb_dma_param != nullptr) {
            auto map_usb_dma = [&](seL4_CPtr frame, uintptr_t vaddr) {
                if (frame == 0) return;
                seL4_CPtr child_frame = alloc_and_track_cap(alloc, pcb);
                check_err(seL4_CNode_Copy(root_cnode, child_frame, seL4_WordBits,
                                          root_cnode, frame, seL4_WordBits, seL4_AllRights), "Copy USB DMA Frame Cap");
                // Через map_frame_robust, а не голым Page_Map: тот падает с
                // FailedLookup, если для адреса ещё нет промежуточной таблицы
                // страниц. Пока bounce-буферы занимали 512 КБ, они целиком
                // укладывались в уже проложенную таблицу; после расширения до
                // 1 МБ диапазон перешёл границу 2 МБ, и загрузка встала на
                // "FATAL: Map USB DMA Buf to Driver -> 6 (FailedLookup)".
                if (!map_frame_robust(alloc, pcb, child_frame, child_vspace, vaddr, normal_untyped, root_cnode)) {
                    uart_puts("[ROOT] FATAL: не удалось замапить USB DMA буфер драйверу.\n");
                    while (1) {}
                }
            };
            map_usb_dma(usb_dma_param->dcbaa_frame, PLAT_XHCI_DCBAA_VADDR);
            map_usb_dma(usb_dma_param->cmdring_frame, PLAT_XHCI_CMDRING_VADDR);
            map_usb_dma(usb_dma_param->erst_frame, PLAT_XHCI_ERST_VADDR);
            map_usb_dma(usb_dma_param->evtring_frame, PLAT_XHCI_EVTRING_VADDR);
            for (int i = 0; i < USB_MAX_SLOTS_ENABLED; i++) {
                map_usb_dma(usb_dma_param->devctx_frame[i], PLAT_XHCI_DEVCTX_VADDR + (uintptr_t)i * 4096);
            }
            map_usb_dma(usb_dma_param->inputctx_frame, PLAT_XHCI_INPUTCTX_VADDR);
            map_usb_dma(usb_dma_param->scratchpad_arr_frame, PLAT_XHCI_SCRATCHPAD_ARR_VADDR);
            for (int i = 0; i < usb_dma_param->scratchpad_count && i < USB_MAX_SCRATCHPAD_PAGES; i++) {
                map_usb_dma(usb_dma_param->scratchpad_buf_frame[i], PLAT_XHCI_SCRATCHPAD_BUF_VADDR + (uintptr_t)i * 4096);
            }
            // Фаза 15 — шесть per-device ресурсов, USB_MAX_DEVICES страниц каждый.
            for (int i = 0; i < USB_MAX_DEVICES; i++) map_usb_dma(usb_dma_param->ep0_trring_frame[i], PLAT_XHCI_EP0_TRRING_VADDR + (uintptr_t)i * 4096);
            for (int i = 0; i < USB_MAX_DEVICES; i++) map_usb_dma(usb_dma_param->ctrl_buf_frame[i], PLAT_XHCI_CTRL_BUF_VADDR + (uintptr_t)i * 4096);
            for (int i = 0; i < USB_MAX_DEVICES; i++) map_usb_dma(usb_dma_param->bulkout_trring_frame[i], PLAT_XHCI_BULKOUT_TRRING_VADDR + (uintptr_t)i * 4096);
            for (int i = 0; i < USB_MAX_DEVICES; i++) map_usb_dma(usb_dma_param->bulkin_trring_frame[i], PLAT_XHCI_BULKIN_TRRING_VADDR + (uintptr_t)i * 4096);
            for (int i = 0; i < USB_MAX_DEVICES; i++) map_usb_dma(usb_dma_param->cbw_csw_frame[i], PLAT_XHCI_CBW_CSW_VADDR + (uintptr_t)i * 4096);
            for (int i = 0; i < USB_MAX_DEVICES; i++) {
                for (int pg = 0; pg < USB_BOUNCE_PAGES; pg++) {
                    map_usb_dma(usb_dma_param->bounce_frame[i][pg],
                                PLAT_XHCI_BOUNCE_VADDR + ((uintptr_t)i * USB_BOUNCE_PAGES + (uintptr_t)pg) * 4096);
                }
            }
        }
        // Пятнадцатая попытка — PCIe RC MISC-регистры, тот же приём, что
        // mbox_regs_frame у timer_driver (одна страница, отдельный vaddr
        // в ТОМ ЖЕ 2MB-окне usb_driver'а, drv_pud/pd/pt уже созданы выше).
        if (is_driver == 6 && pcie_rc_frame_param != 0) {
            seL4_CPtr pcie_rc_child = alloc_and_track_cap(alloc, pcb);
            check_err(seL4_CNode_Copy(root_cnode, pcie_rc_child, seL4_WordBits,
                                      root_cnode, pcie_rc_frame_param, seL4_WordBits, seL4_AllRights), "Copy PCIe RC Frame Cap");
            check_err(seL4_ARM_Page_Map(pcie_rc_child, child_vspace, PLAT_PCIE_RC_VADDR,
                                        seL4_AllRights, (seL4_ARM_VMAttributes)0), "Map PCIe RC Regs to Driver");
        }
        // Двадцать четвёртая попытка — PCIE_OUTB_ERR_* регистры, тот же
        // приём, что PCIe RC MISC-регистры выше, отдельная страница/vaddr.
        if (is_driver == 6 && pcie_err_frame_param != 0) {
            seL4_CPtr pcie_err_child = alloc_and_track_cap(alloc, pcb);
            check_err(seL4_CNode_Copy(root_cnode, pcie_err_child, seL4_WordBits,
                                      root_cnode, pcie_err_frame_param, seL4_WordBits, seL4_AllRights), "Copy PCIe ERR Frame Cap");
            check_err(seL4_ARM_Page_Map(pcie_err_child, child_vspace, PLAT_PCIE_ERR_VADDR,
                                        seL4_AllRights, (seL4_ARM_VMAttributes)0), "Map PCIe ERR Regs to Driver");
        }
        // 37-я hw-попытка (issuse.txt №74/в) — та же схема, что PCIe RC
        // MISC/ERR выше, но READ-ONLY (seL4_CanRead, не AllRights) — эта
        // страница используется только для диагностического чтения Device
        // Status VL805, писать сюда usb_driver не должна.
        if (is_driver == 6 && pcie_cfg_data_frame_param != 0) {
            seL4_CPtr pcie_cfg_data_child = alloc_and_track_cap(alloc, pcb);
            check_err(seL4_CNode_Copy(root_cnode, pcie_cfg_data_child, seL4_WordBits,
                                      root_cnode, pcie_cfg_data_frame_param, seL4_WordBits, seL4_AllRights), "Copy PCIe CFG DATA Frame Cap");
            check_err(seL4_ARM_Page_Map(pcie_cfg_data_child, child_vspace, PLAT_PCIE_CFG_DATA_USB_VADDR,
                                        seL4_CanRead, (seL4_ARM_VMAttributes)0), "Map PCIe CFG DATA Regs to Driver");
        }

        // issuse.txt №74, часть "б" — общая страница состояния драйверов
        // (см. g_driver_state_frame выше, h/driver_state.h). Одна и та же
        // физическая страница у всех шести драйверов, у каждого свой слот
        // по индексу is_driver; usb_driver'у — по своему адресу, т.к. у неё
        // отдельное 2MB-окно (drv_pud/pd/pt уже созданы выше в обоих
        // случаях, дополнительная иерархия не нужна). Некэшируемо
        // ((seL4_ARM_VMAttributes)0) — тот же приём, что у всех страниц
        // выше в этом блоке: root читает эти слова с другого ядра, и
        // возиться с кэш-обслуживанием на каждый опрос незачем.
        if (g_driver_state_frame != 0) {
            uintptr_t drv_state_vaddr = (is_driver == 6) ? PLAT_DRIVER_STATE_USB_VADDR : PLAT_DRIVER_STATE_VADDR;
            seL4_CPtr drv_state_child = alloc_and_track_cap(alloc, pcb);
            check_err(seL4_CNode_Copy(root_cnode, drv_state_child, seL4_WordBits,
                                      root_cnode, g_driver_state_frame, seL4_WordBits, seL4_AllRights), "Copy Driver State Frame Cap");
            check_err(seL4_ARM_Page_Map(drv_state_child, child_vspace, drv_state_vaddr,
                                        seL4_AllRights, (seL4_ARM_VMAttributes)0), "Map Driver State Page to Driver");
            child_ipc_ptr->msg[BOOT_DRIVER_STATE_PRESENT] = 1;
            // Слот драйвера обнуляем в BUSY ПРЯМО СЕЙЧАС, из root'а: между
            // этим моментом и первым driver_state_init() внутри самого
            // драйвера проходит весь его bring-up, и всё это время слот
            // обязан честно говорить "занят" (иначе после респавна там
            // остался бы PARKED от прошлой инкарнации, и root счёл бы
            // безопасным суспендить процесс посреди инициализации железа).
            if (g_driver_state_page != nullptr && is_driver >= 1 && is_driver <= 6) {
                g_driver_state_page[(seL4_Word)is_driver * (DRIVER_STATE_SLOT_STRIDE / sizeof(seL4_Word))] = DRIVER_STATE_BUSY;
            }
        }

        if (is_driver == 1) { // UART
            // UART driver является сервером для console_ep, он на нем слушает.
            child_ipc_ptr->msg[BOOT_CONSOLE_EP] = local_console_ep;
            child_ipc_ptr->msg[BOOT_IRQ_EP] = local_irq_handler;
            // Фаза 6.1 (продолжение): собственная TCB-капа — см. common.h/BOOT_SELF_TCB_CAP.
            child_ipc_ptr->msg[BOOT_SELF_TCB_CAP] = local_self_tcb;
        } else if (is_driver == 2) { // Timer
            // Timer driver является сервером для timer_ep, он на нем слушает.
            child_ipc_ptr->msg[BOOT_TIMER_EP] = local_timer_ep;
            child_ipc_ptr->msg[BOOT_IRQ_EP] = local_irq_handler;
            child_ipc_ptr->msg[BOOT_CONSOLE_EP] = local_console_ep; // Таймер может логировать в консоль, но не обязан
            child_ipc_ptr->msg[BOOT_MBOX_BUF_PADDR] = mbox_buf_paddr_param; // Фаза 4.6, см. platform.h
            child_ipc_ptr->msg[BOOT_HEARTBEAT_NTFN_CAP] = (extra_ntfn_param != 0) ? local_extra_ntfn : 0; // Фаза 4.5, см. common.h
            child_ipc_ptr->msg[BOOT_WIFI_HEARTBEAT_NTFN_CAP] = (extra_ntfn2_param != 0) ? local_extra_ntfn2 : 0; // Фаза 4.5 (Wi-Fi data-plane), см. common.h
            child_ipc_ptr->msg[BOOT_BLK_HEARTBEAT_NTFN_CAP] = (extra_ntfn3_param != 0) ? local_extra_ntfn3 : 0; // Фикс зависания blk_driver, см. common.h
            child_ipc_ptr->msg[BOOT_USB_HEARTBEAT_NTFN_CAP] = (usb_heartbeat_ntfn_param != 0) ? local_usb_heartbeat_ntfn : 0; // Milestone 11 (доп.), см. common.h
            child_ipc_ptr->msg[BOOT_BLK_LIVENESS_TICK_NTFN_CAP] = (blk_liveness_tick_param != 0) ? local_blk_liveness_tick : 0; // Фаза 3b, см. common.h
            child_ipc_ptr->msg[BOOT_ROOT_WATCHDOG_TICK_NTFN_CAP] = (g_root_watchdog_tick_badged != 0) ? local_root_wd_tick : 0; // см. common.h/ROOT_WATCHDOG_TICK_BADGE
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
            // Фикс задержки (см. situation.txt): физический адрес второй
            // страницы (ADMA2-дескриптор multi-block).
            child_ipc_ptr->msg[BOOT_BLK_DMA2_PADDR] = blk_dma_paddr2_param;
            // Фикс зависания (см. situation.txt): local_timer_ep уже минтится
            // в child_cnode безусловно (см. цикл выше), но раньше не
            // проставлялся в boot-IPC — blk_driver физически не мог узнать
            // эту capability, чтобы позвать SYS_TIMER_HEARTBEAT_SUBSCRIBE.
            child_ipc_ptr->msg[BOOT_TIMER_EP] = local_timer_ep;
            // Фикс дедлока root<->blk_driver (см. common.h/BOOT_MMC_IRQ_HANDLER_CAP):
            // собственная копия IRQHandler'а общей линии — blk_driver Ack'ает
            // сам, без обратного IPC к root.
            child_ipc_ptr->msg[BOOT_MMC_IRQ_HANDLER_CAP] = (mmc_irq_handler_param != 0) ? local_mmc_irq_handler : 0;
            // Фаза 6.1 (продолжение): собственная TCB-капа — см. common.h/BOOT_SELF_TCB_CAP.
            child_ipc_ptr->msg[BOOT_SELF_TCB_CAP] = local_self_tcb;
            // Фаза 3b: капа-"я жив" — см. common.h/BOOT_BLK_LIVENESS_NTFN_CAP.
            child_ipc_ptr->msg[BOOT_BLK_LIVENESS_NTFN_CAP] = (liveness_ntfn_param != 0) ? local_liveness_ntfn : 0;
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
            // Фаза 4.5 (Wi-Fi data-plane): капа net_driver->wifi_driver
            // сигнала TX (badge WIFI_EVENT_TX_READY) — см. common.h.
            child_ipc_ptr->msg[BOOT_WIFI_TX_WAKE_CAP] = (net_wifi_tx_wake_param != 0) ? local_wifi_tx_wake : 0;
            // Фаза 6 (SMP): капа общего VFS-мьютекса — см. common.h.
            child_ipc_ptr->msg[BOOT_VFS_MUTEX_NTFN_CAP] = (vfs_mutex_ntfn_param != 0) ? local_vfs_mutex_ntfn : 0;
            // Фаза 6.1 (продолжение): собственная TCB-капа — см. common.h/BOOT_SELF_TCB_CAP.
            child_ipc_ptr->msg[BOOT_SELF_TCB_CAP] = local_self_tcb;
            // Фаза 3b: капа-"я жив" — см. common.h/BOOT_NET_LIVENESS_NTFN_CAP.
            child_ipc_ptr->msg[BOOT_NET_LIVENESS_NTFN_CAP] = (liveness_ntfn_param != 0) ? local_liveness_ntfn : 0;
        } else if (is_driver == 5) { // Wi-Fi driver (Фаза 4) - сервер для шелла, клиент консоли и blk (Милстоун 4.2: чтение прошивки/NVRAM с SD)
            child_ipc_ptr->msg[BOOT_CONSOLE_EP] = local_console_ep;
            child_ipc_ptr->msg[BOOT_WIFI_EP] = local_wifi_recv_ep;
            child_ipc_ptr->msg[7] = local_blk_ep; // BOOT_BLK_EP
            // Фаза 4.5 (продолжение): капа на нотификацию общего IRQ EMMC2/
            // Wi-Fi SDIO — тот же приём, что у blk_driver (local_irq_handler
            // здесь тоже просто "слот с готовой capability на нотификацию",
            // не настоящий IRQHandler, см. wifi_irq_ntfn в main()).
            child_ipc_ptr->msg[BOOT_IRQ_EP] = local_irq_handler;
            // Фаза 4.5 (Wi-Fi data-plane): капа wifi_driver->net_driver
            // сигнала RX (badge NET_EVENT_WIFI_RX) — см. common.h.
            child_ipc_ptr->msg[BOOT_WIFI_NET_RX_SIGNAL_CAP] = (net_wifi_rx_badged_param != 0) ? local_net_wifi_rx : 0;
            // Фаза 6 (SMP): капа общего VFS-мьютекса — см. common.h.
            child_ipc_ptr->msg[BOOT_VFS_MUTEX_NTFN_CAP] = (vfs_mutex_ntfn_param != 0) ? local_vfs_mutex_ntfn : 0;
            // Фаза 6.1 (продолжение): собственная TCB-капа — см. common.h/BOOT_SELF_TCB_CAP.
            child_ipc_ptr->msg[BOOT_SELF_TCB_CAP] = local_self_tcb;
            // Фаза 3b: капа-"я жив" — см. common.h/BOOT_WIFI_LIVENESS_NTFN_CAP.
            child_ipc_ptr->msg[BOOT_WIFI_LIVENESS_NTFN_CAP] = (liveness_ntfn_param != 0) ? local_liveness_ntfn : 0;
        } else if (is_driver == 6) { // USB driver (Фаза 14, xHCI) — сервер для root (root-опосредованный SYS_USB_LIST), клиент консоли
            child_ipc_ptr->msg[BOOT_CONSOLE_EP] = local_console_ep;
            child_ipc_ptr->msg[BOOT_USB_EP] = local_usb_recv_ep;
            // RPI4_XHCI_IRQ — собственная (не шаренная) SPI-линия, тот же
            // приём, что у GENET (is_driver==4): настоящая IRQHandler-капа,
            // usb_driver Ack'ает сам, без root-релея.
            child_ipc_ptr->msg[BOOT_IRQ_EP] = local_irq_handler;
            if (usb_dma_param != nullptr) {
                child_ipc_ptr->msg[BOOT_USB_DCBAA_PADDR]          = usb_dma_param->dcbaa_paddr;
                child_ipc_ptr->msg[BOOT_USB_CMDRING_PADDR]        = usb_dma_param->cmdring_paddr;
                child_ipc_ptr->msg[BOOT_USB_ERST_PADDR]           = usb_dma_param->erst_paddr;
                child_ipc_ptr->msg[BOOT_USB_EVTRING_PADDR]        = usb_dma_param->evtring_paddr;
                // Фаза 15 — только БАЗА (paddr[0]) каждого multi-page
                // ресурса; страницы гарантированно подряд (см. неразрывный
                // retype-цикл выше) — usb_driver.cpp сам считает
                // paddr(idx) = base + idx*4096, новых boot-IPC слотов не
                // требуется.
                child_ipc_ptr->msg[BOOT_USB_DEVCTX_PADDR]         = usb_dma_param->devctx_paddr[0];
                child_ipc_ptr->msg[BOOT_USB_INPUTCTX_PADDR]       = usb_dma_param->inputctx_paddr;
                child_ipc_ptr->msg[BOOT_USB_SCRATCHPAD_ARR_PADDR] = usb_dma_param->scratchpad_arr_paddr;
                child_ipc_ptr->msg[BOOT_USB_SCRATCHPAD_COUNT]     = (seL4_Word)usb_dma_param->scratchpad_count;
                for (int i = 0; i < usb_dma_param->scratchpad_count && i < USB_MAX_SCRATCHPAD_PAGES; i++) {
                    child_ipc_ptr->msg[BOOT_USB_SCRATCHPAD_BUF0_PADDR + i] = usb_dma_param->scratchpad_buf_paddr[i];
                }
                child_ipc_ptr->msg[BOOT_USB_EP0_TRRING_PADDR]     = usb_dma_param->ep0_trring_paddr[0];
                child_ipc_ptr->msg[BOOT_USB_CTRL_BUF_PADDR]       = usb_dma_param->ctrl_buf_paddr[0];
                child_ipc_ptr->msg[BOOT_USB_BULKOUT_TRRING_PADDR] = usb_dma_param->bulkout_trring_paddr[0];
                child_ipc_ptr->msg[BOOT_USB_BULKIN_TRRING_PADDR]  = usb_dma_param->bulkin_trring_paddr[0];
                child_ipc_ptr->msg[BOOT_USB_CBW_CSW_PADDR]        = usb_dma_param->cbw_csw_paddr[0];
                child_ipc_ptr->msg[BOOT_USB_BOUNCE_PADDR]         = usb_dma_param->bounce_paddr[0];
            }
            // Фаза 3b: капа-"я жив" — см. common.h/BOOT_USB_LIVENESS_NTFN_CAP.
            child_ipc_ptr->msg[BOOT_USB_LIVENESS_NTFN_CAP] = (liveness_ntfn_param != 0) ? local_liveness_ntfn : 0;
        }

    } else {
        // Shell or other user app
        child_ipc_ptr->msg[BOOT_CONSOLE_EP] = local_console_ep;
        child_ipc_ptr->msg[BOOT_TIMER_EP] = local_timer_ep;
        child_ipc_ptr->msg[BOOT_NET_EP] = local_net_send_ep;
        child_ipc_ptr->msg[BOOT_WIFI_EP] = local_wifi_send_ep;
        child_ipc_ptr->msg[7] = local_blk_ep; // BOOT_BLK_EP
        // Shell (0) и доверенные /sbin-утилиты (253, см. Фазу A) — участники
        // общего VFS-мьютекса (Фаза 6); обычный пользовательский exec
        // (is_driver==254) — нет.
        if (is_driver == 0 || is_driver == 253) {
            child_ipc_ptr->msg[BOOT_VFS_MUTEX_NTFN_CAP] = (vfs_mutex_ntfn_param != 0) ? local_vfs_mutex_ntfn : 0;
        }
        // Milestone 9 (Фаза 14, закрытие) — та же капа, что BOOT_BLK_EP
        // выше, но на usb_driver'ов VFS-диспетчер (см.
        // usb_storage_ep_param). 0, если параметр не передан (остальные
        // ~5 мест спавна) — client-код (sys_client.h/shell.cpp) обязан
        // проверять на 0 перед использованием, тот же принцип, что и с
        // BOOT_IRQ_EP/остальными опциональными капами.
        child_ipc_ptr->msg[BOOT_USB_STORAGE_EP] = (usb_storage_ep_param != 0) ? local_usb_storage_ep : 0;
    }

    seL4_ARM_Page_Clean_Data(ipc_frame, 0, 4096);
    
    check_err(seL4_ARM_Page_Unmap(ipc_frame), "Unmap IPC from Root");

    for (int i = 0; i < STACK_PAGES; i++) {
        check_err(seL4_ARM_Page_Map(stack_frames[i], child_vspace, child_stack + (uintptr_t)i * 0x1000, seL4_AllRights, seL4_ARM_Default_VMAttributes), "Map Stack to Child");
    }
    check_err(seL4_ARM_Page_Map(ipc_frame, child_vspace, child_ipc, seL4_AllRights, seL4_ARM_Default_VMAttributes), "Map IPC to Child");

    seL4_CPtr tcb = alloc_and_track_cap(alloc, pcb);
    pcb.tcb = tcb;
    pcb.vspace = child_vspace;
    if (ram_retype(normal_untyped, seL4_TCBObject, 0, root_cnode, 0, 0, tcb, 1, true) != seL4_NoError)
        return abort_spawn(pcb, alloc, root_cnode, "retype TCB");

    // Фаза 6.1 (продолжение, см. ROADMAP.md): собственная TCB-капа — только
    // uart/blk/net/wifi (is_driver 1/3/4/5), см. BOOT_SELF_TCB_CAP выше.
    // ПОСЛЕ Retype (tcb должен уже существовать) — copy в cnode не трогает
    // child_ipc_ptr (уже отмаплен из root'а строкой выше), поэтому порядок
    // относительно unmap'а не важен, важен только относительно Retype.
    if (is_driver == 1 || is_driver == 3 || is_driver == 4 || is_driver == 5) {
        check_err(seL4_CNode_Copy(child_cnode, local_self_tcb, 8, root_cnode, tcb, seL4_WordBits, seL4_AllRights), "Copy self TCB cap");
    }

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

    // issuse.txt №74/в (JTAG-диагностика 2026-08-27) — тот же приём, что
    // issuse.txt №64 уже использовал для CLONE_THREAD ниже (generic_recover_process()):
    // без этого КАЖДЫЙ дочерний процесс root'а неотличим в живом JTAG-дампе
    // ksDebugTCBs от остальных ("child of: 'rootserver'" у всех одинаково) —
    // при разборе реального зависания (usbreset/restart) не удалось
    // сопоставить конкретные зависшие TCB с именами процессов (uart_driver/
    // blk_driver/timer_driver/net_driver/shell), несмотря на полный обход
    // списка. seL4_DebugNameThread не влияет на поведение, только на имя в
    // отладочном выводе (CONFIG_DEBUG_BUILD) — здесь именуем КАЖДЫЙ спавн
    // (не только CLONE_THREAD) РЕАЛЬНЫМ именем процесса, чтобы следующий
    // JTAG-разбор сразу видел, кто есть кто.
    seL4_DebugNameThread(tcb, name);

    seL4_UserContext regs = {0};
    regs.pc = entry_point;
    regs.sp = child_stack + (uintptr_t)STACK_PAGES * 4096; // = 0x501000 всегда, см. комментарий у STACK_PAGES выше
    regs.x0 = (seL4_Word)badged_ep;
    regs.x1 = (seL4_Word)child_ipc;
    regs.x2 = (seL4_Word)med_ep; 
    regs.tpidr_el0 = (seL4_Word)child_ipc + 3072;
    regs.tpidrro_el0 = (seL4_Word)child_ipc + 3072;
    size_t reg_count = sizeof(seL4_UserContext) / sizeof(seL4_Word);
    seL4_TCB_WriteRegisters(tcb, 0, 0, reg_count, &regs);

    
    seL4_TCB_SetTLSBase(tcb, child_ipc + 3072);
    seL4_TCB_SetPriority(tcb, seL4_CapInitThreadTCB, 254);

    // Фаза 6 (SMP): wifi_driver и usb_driver стартуют не на ядре 0.
    // wifi_driver (ядро 1) — потому что его SDIO data-plane целиком
    // busy-poll/PIO (нет in-band IRQ, Фаза 4), а PBKDF2-хендшейк — чистое
    // вычисление: корректность не зависит ни от чего, доступного только на
    // ядре 0. usb_driver (USB_DRIVER_DEFAULT_CORE, см. platform.h) —
    // потому что он единственный, кто умеет зависнуть НАСМЕРТЬ на
    // PCIe-транзакции (аппаратное ограничение, см. issuse.txt), и на ядре 0
    // утаскивал бы за собой root'а. Условие на is_driver, а не отдельный
    // параметр — переживает respawn ("wifi restart") без дополнительного
    // состояния. Вызывается ДО seL4_TCB_Resume — поток ещё не runnable,
    // просто выставляет поле affinity.
    //
    // ИСТОРИЯ (issuse.txt №15/№73): ровно этот перенос usb_driver уже
    // пробовали раньше и ОТКАТЫВАЛИ — после него аварийное восстановление
    // (generic_recover_process() -> seL4_TCB_Suspend) вешало ВСЮ систему,
    // включая root. Причину нашли JTAG'ом: Suspend над потоком, реально
    // исполняющимся на другом ядре, уходит в remoteTCBStall() -> ipi_wait()
    // без таймаута. Тогда это "починили" запретом переносить драйверы
    // вообще; теперь (issuse.txt №74, часть "б") сделано по существу —
    // prepare_driver_for_suspend() ниже гарантирует, что Suspend вызывается
    // ТОЛЬКО когда поток стоит в своём seL4_Recv и уже возвращён на ядро 0.
    bool pin_to_own_core = (is_driver == 5) || (is_driver == 6);
    int own_core = (is_driver == 5) ? 1 : USB_DRIVER_DEFAULT_CORE;
    if (pin_to_own_core) {
        seL4_TCB_SetAffinity(tcb, own_core);
    }
    pcb.core = pin_to_own_core ? own_core : 0; // Фаза 6.1: отображение текущего ядра для taskset/top

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

// =====================================================================
// issuse.txt №74, часть "б" — АДАПТИВНЫЙ ПЕРЕНОС ДРАЙВЕРОВ МЕЖДУ ЯДРАМИ.
// Полный проект механизма — в situation.txt; здесь только реализация.
//
// Суть в одном абзаце. Сам перенос (seL4_TCB_SetAffinity) БЕЗОПАСЕН
// всегда: в ядре это голая запись поля tcbAffinity, без единого IPI (см.
// kernel/src/model/smp.c/migrateTCB()) — зависнуть на нём невозможно в
// принципе. Опасен ПОСЛЕДУЮЩИЙ seL4_TCB_Suspend() над потоком, который в
// этот момент реально исполняется на ДРУГОМ ядре: тогда ядро зовёт
// remoteTCBStall() -> ipi_wait(), синхронный барьер без таймаута, и root
// висит навсегда (hw-подтверждено JTAG'ом, issuse.txt №73 — именно из-за
// этого драйверы и были заперты на ядре 0).
//
// Поэтому root перед КАЖДЫМ Suspend'ом процесса с ядром != 0:
//   1) ждёт, пока драйвер сам объявит PARKED (стоит в своём seL4_Recv) —
//      честный таймаут на СВОЕЙ стороне, через обычный SYS_SLEEP_MS к
//      timer_driver, а не опрос зависшего syscall'а;
//   2) возвращает поток на ядро 0 — после этого условие remoteTCBStall()
//      (tcbAffinity != текущее ядро) ложно по построению, IPI не будет;
//   3) даёт старому ядру домолотить до своей ближайшей точки планирования;
//   4) и только теперь суспендит.
// Не дождался PARKED — значит драйвер завис ПО-НАСТОЯЩЕМУ (тот самый
// класс аппаратного стопора PCIe, см. issuse.txt): Suspend НЕ вызывается
// вообще, восстановление честно отменяется. Софтом такое зависание не
// лечится, а повесить заодно и root — единственное, чего мы этим
// добились бы.
// =====================================================================

// Прочитать слот состояния драйвера. Возвращает DRIVER_STATE_BUSY, если
// страницы нет, драйвер не из наблюдаемых, или слот ещё не заполнен —
// консервативно, "не уверен, значит занят".
static seL4_Word driver_state_read(int is_driver) {
    if (g_driver_state_page == nullptr) return DRIVER_STATE_BUSY;
    if (is_driver < 1 || is_driver > 6) return DRIVER_STATE_BUSY;
    return g_driver_state_page[(seL4_Word)is_driver * (DRIVER_STATE_SLOT_STRIDE / sizeof(seL4_Word))];
}

// Единственное слово общей страницы, которое пишет root (см.
// common.h/DRIVER_STATE_WORD_FREEZE).
static void usb_set_freeze(bool on) {
    if (g_driver_state_page == nullptr) return;
    g_driver_state_page[(seL4_Word)6 * (DRIVER_STATE_SLOT_STRIDE / sizeof(seL4_Word)) + DRIVER_STATE_WORD_FREEZE] = on ? 1 : 0;
}

static bool driver_is_parked(int is_driver) {
    return driver_state_read(is_driver) == DRIVER_STATE_PARKED;
}

// Слово слота по индексу (см. common.h/DRIVER_STATE_WORD_*).
static seL4_Word driver_state_word(int is_driver, int word) {
    if (g_driver_state_page == nullptr) return 0;
    if (is_driver < 1 || is_driver > 6) return 0;
    return g_driver_state_page[(seL4_Word)is_driver * (DRIVER_STATE_SLOT_STRIDE / sizeof(seL4_Word)) + word];
}

// Ждать PARKED не дольше budget_ms. Спит через timer_driver (SYS_SLEEP_MS)
// — единственный процесс, который сам ни от кого не зависит; если
// восстанавливаем ЕГО ЖЕ (is_driver==2) или timer_ep недоступен, ждать
// нечем, честно возвращаем текущее состояние без сна.
static bool wait_for_driver_parked(int is_driver, seL4_CPtr timer_ep, seL4_Word budget_ms) {
    if (driver_is_parked(is_driver)) return true;
    if (timer_ep == 0 || is_driver == 2) return false;
    seL4_Word waited = 0;
    while (waited < budget_ms) {
        seL4_SetMR(0, 8); // SYS_SLEEP_MS (см. timer_driver.cpp, root как обычный клиент timer_ep)
        seL4_SetMR(1, DRIVER_PARK_POLL_STEP_MS);
        seL4_Call(timer_ep, seL4_MessageInfo_new(0, 0, 0, 2));
        waited += DRIVER_PARK_POLL_STEP_MS;
        if (driver_is_parked(is_driver)) return true;
    }
    return false;
}

// Привести процесс в состояние, в котором seL4_TCB_Suspend() над ним
// доказуемо не заблокирует root. true — можно суспендить.
static bool prepare_driver_for_suspend(int pid, seL4_CPtr timer_ep) {
    if (pid <= 0 || pid >= 256 || !pcbs[pid].active) return false;

    // Ядро 0 — root (приоритет 255) вытесняет там кого угодно, целевой
    // поток гарантированно не ksCurThread ни на одном ядре, Suspend
    // безопасен без всяких приготовлений. Это ровно тот случай, который
    // работал всегда, и трогать его не нужно.
    if (pcbs[pid].core == 0) return true;

    int is_driver = pcbs[pid].is_driver;

    // Не-драйвер (обычный процесс, разогнанный `balance` по ядрам) не
    // публикует своё состояние — про него мы ничего не знаем. Для него
    // остаётся шаг "вернуть домой и дать осесть": он снимает IPI-барьер
    // (условие remoteTCBStall() перестаёт выполняться), а окно, в котором
    // поток ещё физически докручивает на старом ядре, закрывается паузой
    // заметно длиннее кванта планировщика.
    bool parked = false;
    if (is_driver >= 1 && is_driver <= 6) {
        parked = wait_for_driver_parked(is_driver, timer_ep, DRIVER_PARK_WAIT_BUDGET_MS);
        if (!parked) {
            uart_puts("[SMP] "); uart_puts(pcbs[pid].name);
            uart_puts(" (ядро "); uart_putdec(pcbs[pid].core);
            uart_puts(") не встал в PARKED за "); uart_putdec(DRIVER_PARK_WAIT_BUDGET_MS);
            uart_puts(" мс — Suspend ОТМЕНЁН (иначе root повис бы в ipi_wait(), issuse.txt №74/б).\n");
            return false;
        }
    }

    // Подтверждённый PARKED сам по себе достаточен: поток, честно
    // заблокированный в seL4_Recv, не является ksCurThread НИ НА ОДНОМ
    // ядре, а remoteTCBStall() гейтится именно этим условием — IPI не
    // будет. Значит возвращать драйвер на ядро 0 не нужно, и не нужно
    // ставить его туда даже на мгновение: ядро 0 — ядро root'а, и
    // держать там драйвер, способный намертво застопорить своё ядро на
    // аппаратной транзакции, противоречит всему смыслу этой работы (см.
    // core_allowed_for_process()).
    if (parked) return true;

    // Про не-драйвер мы не знаем ничего — для него остаётся "вернуть
    // домой и дать осесть": это снимает условие remoteTCBStall(), а окно,
    // в котором поток ещё докручивает на старом ядре, закрывается паузой
    // заметно длиннее кванта планировщика.
    seL4_TCB_SetAffinity(pcbs[pid].tcb, 0);
    pcbs[pid].core = 0;

    if (timer_ep != 0 && is_driver != 2) {
        seL4_SetMR(0, 8); // SYS_SLEEP_MS
        seL4_SetMR(1, DRIVER_SETTLE_AFTER_HOMING_MS);
        seL4_Call(timer_ep, seL4_MessageInfo_new(0, 0, 0, 2));
    }
    return true;
}

// Ядро 0 — ядро root'а, и оно ЗАПОВЕДНОЕ для драйверов.
//
// НАЙДЕНО НА ЖИВОМ ЖЕЛЕЗЕ 2026-09-05, регрессия от снятия запрета на
// перенос драйверов: `balance` выбирает получателем НАИМЕНЕЕ загруженное
// ядро, и ничто не мешало ему поставить usb_driver на ядро 0. А usb_driver
// — единственный, кто умеет зависнуть НАСМЕРТЬ на PCIe-транзакции
// (аппаратное ограничение, см. issuse.txt): ядро, исполняющее такой load,
// перестаёт исполнять код вообще, его не берёт даже JTAG-halt. Пока это
// ядро 2 — зависает только USB, root жив и лечит. Стоит балансировщику
// увести драйвер на ядро 0 — тем же зависанием выносится root, а вместе с
// ним ВСЯ система: ни watchdog, ни shell, ни единой строчки в лог. Ровно
// это и наблюдалось: полная тишина после "GPT: найден раздел ...".
//
// Смысл всей части "б" (issuse.txt №74) в том, чтобы root пережил
// зависание драйвера. Разрешать переносить драйвер на ядро root'а —
// значит своими руками ломать то, ради чего всё делалось.
static bool core_allowed_for_process(int pid, int core) {
    if (core < 0 || core > 3) return false;
    if (pcbs[pid].is_driver == 0) return true; // обычные процессы — куда угодно
    return core != 0;
}

// Перенос процесса на ядро core. Возвращает false, если перенос НЕ сделан.
//
// Драйвер переносится ТОЛЬКО припаркованным. Сам вызов SetAffinity
// безопасен всегда (см. шапку блока), но переносить драйвер в середине
// реальной работы с железом — отдельный риск: у xHCI/EMMC/SDIO в полёте
// остаются транзакции, чьё завершение драйвер ждёт опросом по wall-clock,
// а смена ядра меняет тайминги этого опроса. Ничего не теряем, ожидая
// безопасного момента: `balance` приходит каждые 5с и просто перенесёт в
// следующий раз.
static bool migrate_process_to_core(int pid, int core, seL4_CPtr timer_ep) {
    if (pcbs[pid].core == core) return true; // уже там, делать нечего
    if (!core_allowed_for_process(pid, core)) return false;

    int is_driver = pcbs[pid].is_driver;
    if (is_driver >= 1 && is_driver <= 6) {
        if (!wait_for_driver_parked(is_driver, timer_ep, DRIVER_MIGRATE_WAIT_MS)) {
            uart_puts("[SMP] "); uart_puts(pcbs[pid].name);
            uart_puts(" занят работой с железом — перенос отложен до следующего прохода.\n");
            return false;
        }
    }
    seL4_TCB_SetAffinity(pcbs[pid].tcb, core);
    pcbs[pid].core = core;
    return true;
}

// Можно ли этот процесс переносить между ядрами вообще.
// timer_driver — нельзя: он единственный владелец ARM generic timer'а,
// чьи прерывания PPI приходят per-core (см. ROADMAP.md Фаза 6.1), плюс он
// же кормит PM_WDOG и обслуживает SYS_SLEEP_MS всей системы, включая
// механизм ожидания PARKED выше — переносить того, на ком держится сама
// защита, было бы кольцевой зависимостью.
// Кто из драйверов реально размечает свои шаги (driver_state_step()/
// driver_state_progress(), см. h/driver_state.h). У остальных слова
// "шаг" и "прогресс" в слоте просто нули, и трактовать их как "прогресс
// замер" — ровно та ошибка, что на живом железе положила blk_driver и
// net_driver в цикл ложных восстановлений (см. issuse.txt №11).
// =====================================================================
// НАЙДЕНО JTAG'ом НА МЁРТВОЙ ПЛАТЕ 2026-09-06 — почему зависание USB
// убивает ВСЮ систему, а не только USB.
//
// Снято с зависшей платы, без единой перепрошивки:
//   - cpu1 и cpu2 остановлены отладчиком штатно, обе в clh_lock_acquire
//     (большой замок ядра seL4, kernel/include/smp/lock.h);
//   - cpu0 и cpu3 на запрос halt не отвечают ВООБЩЕ;
//   - разбор структуры big_kernel_lock показал очередь cpu0 -> cpu2 ->
//     cpu1, причём myreq у cpu0 всё ещё Pending, то есть ЗАМОК ДЕРЖИТ
//     cpu0 и не отпускал его;
//   - PC ядра cpu0, прочитанный через сэмплирующий регистр отладки
//     EDPCSR (БЕЗ halt, который не проходит), = 0xffffff800001830c.
//
// Этот адрес — инструкция `dsb sy` СРАЗУ ПОСЛЕ `tlbi aside1` внутри
// deleteASID() (kernel/src/arch/arm/64/kernel/vspace.c:1079 ->
// invalidateTLBByASID -> ... -> invalidateLocalTLB_ASID).
//
// Механизм гибели целиком:
//   1. usb_driver выдаёт транзакцию на PCIe, которую VL805/мост никогда
//      не завершает (то самое аппаратное ограничение, см. issuse.txt).
//   2. Root в это время восстанавливает КАКОЙ УГОДНО процесс — в живом
//      случае это был net_driver, к USB отношения не имеющий. Ядро ОС
//      сносит его VSpace, а это deleteASID -> tlbi + dsb sy.
//   3. `dsb sy` ждёт завершения ВСЕХ незавершённых транзакций системы,
//      включая ту самую застрявшую PCIe-шную. Он не завершается никогда.
//   4. cpu0 замирает ВНУТРИ ядра ОС, держа большой замок.
//   5. Любое другое ядро на первом же системном вызове встаёт в очередь
//      за этим замком. Плата мертва: ни лога, ни watchdog'а, ни шелла.
//   6. JTAG не может остановить cpu0, потому что halt принимается только
//      на границе инструкции, а эта инструкция не завершается. Отсюда и
//      давняя запись в issuse.txt "не берёт даже JTAG-halt" — теперь
//      известно, ЧТО именно не берётся и почему.
//
// ЧТО ЭТО ОПРОВЕРГАЕТ: предпосылку, на которой строилась вся часть "б"
// (issuse.txt №74) — "драйвер завис на своём ядре, root на ядре 0 жив и
// лечит". В сборке с большим замком ядра это неверно: замирает ТО ядро,
// которое первым выполнит dsb sy, чьё бы оно ни было, — и делает это,
// уже держа замок. Разделение по ядрам от этого класса отказа не спасает
// вообще.
//
// ЧТО ИЗ ЭТОГО СЛЕДУЕТ ПРАКТИЧЕСКИ: разрушать VSpace, пока на шине висит
// незавершённая транзакция, НЕЛЬЗЯ. Сначала снять причину (fundamental
// reset PCIe обрывает зависшую транзакцию), и только потом трогать
// капабилити/VSpace кого бы то ни было.
// =====================================================================
static bool usb_bus_stalled_suspect(seL4_CPtr timer_ep) {
    if (g_driver_state_page == nullptr || g_usb_cmd_ep == 0 || timer_ep == 0) return false;
    if (g_driver_progress_seen_ms[6] == 0) return false;
    // Припаркован — значит дошёл до своего Recv, ничего в полёте нет.
    if (driver_state_read(6) == DRIVER_STATE_PARKED) return false;
    // Не в шаге — тоже нечему висеть.
    if (driver_state_word(6, DRIVER_STATE_WORD_STEP) == USB_STEP_IDLE) return false;
    seL4_SetMR(0, 4); // SYS_GET_UPTIME
    seL4_Call(timer_ep, seL4_MessageInfo_new(0, 0, 0, 1));
    seL4_Word now = seL4_GetMR(0);
    return (now - g_driver_progress_seen_ms[6]) >= USB_STEP_STUCK_TIMEOUT_MS;
}

static bool driver_reports_steps(int is_driver) {
    return is_driver == 6; // пока только usb_driver
}

static bool process_is_migratable(int pid) {
    if (pid <= 0 || pid >= 256 || !pcbs[pid].active) return false;
    if (pcbs[pid].is_driver == 2) return false;
    return true;
}

// =====================================================================
// ПОЛНЫЙ СБРОС USB "С НУЛЯ" — тот самый путь, который закрыл issuse.txt
// №74/в (команда `usbreset`, 38 hw-попыток): fundamental reset PCIe Root
// Complex'а + восстановление всего, что стирает bridge software init
// (включая Command самого моста — ту единственную причину, что пряталась
// 37 попыток), + перезаливка прошивки VL805 через VideoCore-mailbox, +
// переэнумерация устройств.
//
// Вынесено из case SYS_PCIE_RESET в отдельную функцию, потому что теперь у
// этого пути ДВА вызывающих: ручной `usbreset` из шелла и АВТОМАТИЧЕСКИЙ
// heartbeat-watchdog (см. главный цикл). Для usb_driver обычный
// kill+respawn бесполезен: процесс пересоздаётся, а шина остаётся лежать —
// лечить нужно железо, а не процесс.
//
// Ключевая тонкость, из-за которой это не просто "вызвать то же самое":
// оба IPC к usb_driver ниже (заморозка и RESTART) — обычные seL4_Call, и
// если драйвер уже завис на PCIe-транзакции, они повесят root ровно так
// же. Поэтому каждый из них делается ТОЛЬКО когда драйвер объявил себя
// PARKED (issuse.txt №74/б, см. driver_is_parked()). Не объявил — сброс
// шины всё равно выполняется (он root'у ничего не стоит и не требует
// участия драйвера), а вызывающему возвращается USB_RESET_DRIVER_STUCK:
// шина поднята, но процесс надо пересоздавать целиком.
//
// Коды возврата совпадают с ответом SYS_PCIE_RESET шеллу (см. common.h).
constexpr seL4_Word USB_RESET_OK           = 0;
constexpr seL4_Word USB_RESET_NO_LINK      = (seL4_Word)-3;
constexpr seL4_Word USB_RESET_NO_MAILBOX   = (seL4_Word)-4;
constexpr seL4_Word USB_RESET_DRIVER_STUCK = (seL4_Word)-5;

static seL4_Word root_usb_full_reset(seL4_CPtr timer_ep, seL4_CPtr usb_cmd_ep) {
    // Шаг 1. Заморозить usb_driver, если он в состоянии нас услышать.
    // Зачем вообще: без этого PM_WDOG срабатывал на КАЖДОМ вызове —
    // usb_driver продолжал свой heartbeat-опрос xHCI MMIO
    // (hub_conn_async_tick/poll_ports_for_hotplug/poll_hub_interrupts, см.
    // g_usb_hw_frozen в usb_driver.cpp), ПОКА root дёргал PERST на том же
    // живом линке — гарантированное "load физически зависает" ровно там,
    // где драйвер застаёт линк электрически лежащим. Синхронный round-trip
    // ДОКАЗЫВАЕТ, что драйвер дошёл до Recv и больше не тронет железо,
    // пока не придёт RESTART.
    // Ждём PARKED, а не смотрим мгновенное состояние: usb_driver каждые
    // ~20мс просыпается на heartbeat-тик, и разовая проверка с заметной
    // вероятностью застала бы его "занятым" на ровном месте — а платой за
    // ошибку было бы напрасное пересоздание процесса. Просто занятый
    // драйвер припаркуется за миллисекунды; по-настоящему зависший — не
    // припаркуется никогда, и бюджет истечёт честно.
    // Если общей страницы состояния нет вообще (не смогли её создать —
    // см. main()), возвращаемся к прежнему поведению: звоним вслепую.
    // Именно так `usbreset` работал до этой правки, и работал успешно.
    bool driver_reachable = (g_driver_state_page == nullptr) ||
                            wait_for_driver_parked(6, timer_ep, DRIVER_PARK_WAIT_BUDGET_MS);
    if (driver_reachable) {
        uart_puts("[ROOT] USB RESET: замораживаю usb_driver (heartbeat-опрос xHCI MMIO должен остановиться на время сброса моста).\n");
        seL4_SetMR(0, SYS_PCIE_RESET);
        seL4_SetMR(1, 1); // freeze
        seL4_Call(usb_cmd_ep, seL4_MessageInfo_new(0, 0, 0, 2));
    } else {
        uart_puts("[ROOT] USB RESET: usb_driver НЕ припаркован (завис или занят) — заморозку пропускаю, IPC к нему сейчас повесил бы root. Сброс шины это не отменяет.\n");
    }

    // Заморозка через общую страницу — БЕЗУСЛОВНО, независимо от того,
    // удался ли IPC выше. Если драйвер сейчас не отвечает, он прочитает
    // это предупреждение, как только вернётся в свой цикл, и не полезет к
    // контроллеру, который мы прямо сейчас будем ресетить.
    usb_set_freeze(true);
    uart_puts("[ROOT] USB RESET: локальный fundamental reset PCIe RC (issuse.txt №74/в).\n");
    if (!root_pcie_fundamental_reset(timer_ep)) {
        uart_puts("[ROOT] USB RESET: линк НЕ поднялся за таймаут — mailbox-тег НЕ вызываю (см. предупреждение у MBOX_TAG_NOTIFY_XHCI_RESET/platform.h).\n");
        return USB_RESET_NO_LINK;
    }

    uart_puts("[ROOT] USB RESET: линк поднялся (PHYLINKUP+DL_ACTIVE), форвардю mailbox-тег на timer_driver (перезаливка прошивки VL805).\n");
    seL4_SetMR(0, SYS_MBOX_XHCI_RESET);
    seL4_Call(timer_ep, seL4_MessageInfo_new(0, 0, 0, 1));
    if (seL4_GetMR(0) != 0) {
        uart_puts("[ROOT] USB RESET: mailbox-тег НЕ подтверждён — прошивка VL805 могла не перезалиться.\n");
        return USB_RESET_NO_MAILBOX;
    }

    // Диагностическая обвязка расследования №74/в (контрольные точки F/F2):
    // переустановить ECAM-индекс на VL805 и снять залипшие RW1C-биты
    // Device Status. Сама переустановка индекса — часть обычной
    // последовательности (usb_driver на неё полагается и сама его не
    // трогает), а печать/чтение статуса гейтится LOG_PCIE_DIAG.
    *(volatile uint32_t*)(PLAT_PCIE_RGR1_ROOT_VADDR + PCIE_EXT_CFG_INDEX_PAGE_OFFSET) = MBOX_XHCI_RESET_DEV_ADDR;
    if (LOG_PCIE_DIAG) pcie_debug_vl805_devstatus("[ROOT]   DIAG Device Status VL805 (F: после mailbox-тега) = ");
    {
        uint32_t raw = *(volatile uint32_t*)(PLAT_PCIE_CFG_DATA_ROOT_VADDR + PCI_EXPRESS_CAP_VL805_DEVICE_STATUS_OFFSET);
        uint32_t dev_status = (raw >> 16) & 0xFFFFu;
        constexpr uint32_t kErrBits = PCIE_DEV_STATUS_CORRECTABLE_ERROR_MASK | PCIE_DEV_STATUS_UNSUPPORTED_REQUEST_MASK;
        if (dev_status & kErrBits) {
            // RW1C — очищаем найденные биты явно, Device Control (нижние
            // 16 бит того же dword'а) сохраняем как есть.
            *(volatile uint32_t*)(PLAT_PCIE_CFG_DATA_ROOT_VADDR + PCI_EXPRESS_CAP_VL805_DEVICE_STATUS_OFFSET) =
                (raw & 0xFFFFu) | (kErrBits << 16);
        }
    }
    if (LOG_PCIE_DIAG) {
        uint32_t raw_f2 = *(volatile uint32_t*)(PLAT_PCIE_CFG_DATA_ROOT_VADDR + 0);
        uart_puts("[ROOT]   DIAG VendorID/DeviceID VL805 (F2: сразу после F+очистки, без IPC) = "); uart_puthex(raw_f2); uart_puts("\n");
    }

    // Шаг 2. Перечислить устройства заново. RESTART снимает g_usb_hw_frozen
    // и целиком повторяет run_bring_up() — то есть xHCI поднимается с нуля,
    // ровно как при холодной загрузке. Перепроверяем PARKED ещё раз: между
    // заморозкой и этой точкой прошёл целый сброс шины.
    if (!driver_reachable ||
        (g_driver_state_page != nullptr && !wait_for_driver_parked(6, timer_ep, DRIVER_PARK_WAIT_BUDGET_MS))) {
        uart_puts("[ROOT] USB RESET: шина поднята, но usb_driver недоступен по IPC — переэнумерация отложена до момента, когда он вернётся в свой Recv.\n");
        g_usb_restart_pending = true;
        return USB_RESET_DRIVER_STUCK;
    }
    usb_set_freeze(false); // сейчас будет переэнумерация — железо снова можно трогать
    uart_puts("[ROOT] USB RESET: форвардю DRIVER_SIGNAL_RESTART на usb_driver (переэнумерация устройств).\n");
    seL4_SetMR(0, SYS_DRIVER_SIGNAL);
    seL4_SetMR(1, DRIVER_SIGNAL_RESTART);
    seL4_Call(usb_cmd_ep, seL4_MessageInfo_new(0, 0, 0, 2));
    uart_puts("[ROOT] USB RESET: DRIVER_SIGNAL_RESTART вернул статус = "); uart_puthex(seL4_GetMR(0)); uart_puts("\n");
    uart_puts("[ROOT] USB RESET: готово.\n");
    return USB_RESET_OK;
}

// --- issuse.txt №3 (плавающее падение тестового процесса с PC=0) ---
//
// Три пути завершения процесса делали РАЗНУЮ уборку, и обычный выход был
// самым бедным из них. generic_recover_process() (kill драйвера/краш) уже
// освобождала глобальный vfs_mutex_ep, если умирающий был его держателем
// (issuse.txt №70), а SYS_EXIT/SYS_KILL — нет. И НИ ОДИН из трёх не трогал
// потоки, созданные умирающим процессом через SYS_CLONE: поле
// pcb.parent_pid писалось в case SYS_CLONE и читалось РОВНО в одном месте —
// case SYS_THREAD_EXIT, то есть только когда поток завершался сам.
//
// Чем это плохо на практике (последовательность проверена по коду, не
// предположение): coretest/ТЕСТ 7 запускает поток-читатель, который держит
// VFS-мьютекс на всё чтение, и имеет четыре пути выхода по ошибке, каждый
// из которых зовёт sys_exit() — в том числе выход по таймауту, где поток
// ГАРАНТИРОВАННО ещё жив (именно потому таймаут и наступил). Дальше:
//   1. root суспендит ТОЛЬКО TCB родителя;
//   2. revoke+delete+alloc.free() проходит по всем капам родителя, включая
//      корни его VSpace и CSpace — а поток исполняется именно на них
//      (SYS_CLONE: pcb.vspace/cspace = родительские) и НЕ суспенднут;
//   3. PCB потока навсегда остаётся active=true — утечка PID, TCB,
//      IPC-фрейма и трёх слотов CNode на каждый такой выход;
//   4. держателем vfs_mutex_ep числится PID этого вечно-живого потока,
//      так что мьютекс не освободит уже никто и никогда.
// Пункт 4 сам по себе вешает КАЖДУЮ последующую VFS-команду, пункт 2 —
// оставляет исполняться поток без адресного пространства.
//
// Согласуется с уже собранными наблюдениями: контрольный опыт со 100
// спавнами /bin/test_app.elf (клонов не делает) прошёл 100/100, а отказ
// требовал «предшествующего сброса шины» — сброс шины как раз и заставляет
// чтение в ТЕСТЕ 7 упереться в таймаут, то есть создаёт осиротевший поток.
//
// ВАЖНО про порядок вызова: обе функции обязаны отработать ДО того, как
// вызывающий начнёт удалять свои капы, иначе поток успеет исполниться на
// уже уничтоженном VSpace.
static void release_vfs_lock_if_held_by(int pid)
{
    if (g_vfs_lock_holder_pid != (seL4_Word)pid) return;
    if (g_vfs_mutex_ntfn_cap != 0) {
        seL4_Signal(g_vfs_mutex_ntfn_cap);
        uart_puts("[KERNEL] Освобождён глобальный vfs_mutex_ep умершего держателя (issuse.txt №3/№70).\n");
    }
    g_vfs_lock_holder_pid = 0;
}

// Убрать все SYS_CLONE-потоки, принадлежащие owner_pid. Тело повторяет
// уборку из case SYS_THREAD_EXIT — разница лишь в том, что там поток
// сообщает о себе сам, а здесь его добирает уходящий родитель.
static void reap_clone_threads_of(int owner_pid, PsychAllocator &alloc, seL4_CPtr root_cnode)
{
    if (owner_pid <= 0 || owner_pid >= 256) return;

    for (int t = 1; t < 256; t++) {
        if (t == owner_pid) continue;
        if (!pcbs[t].active) continue;
        if (pcbs[t].parent_pid != owner_pid) continue;
        // Двойная проверка по имени: parent_pid отличен от нуля только у
        // клон-потоков (memset в case SYS_CLONE), но полагаться на одно
        // поле для операции, которая суспендит чужой TCB, не стоит.
        if (strncmp(pcbs[t].name, "shell_thread", 12) != 0) continue;

        uart_puts("[KERNEL] Уборка осиротевшего SYS_CLONE-потока pid=");
        uart_putdec(t);
        uart_puts(" (родитель ");
        uart_putdec(owner_pid);
        uart_puts(" завершился, issuse.txt №3).\n");

        // Суспенд ПЕРВЫМ делом — дальше у потока пропадёт адресное
        // пространство, и до этого момента он исполняться не должен.
        if (pcbs[t].tcb) seL4_TCB_Suspend(pcbs[t].tcb);

        release_vfs_lock_if_held_by(t);

        for (int i = 0; i < MAX_PIPES; i++) {
            if (g_pipes[i].active && g_pipes[i].writer_pid == t) {
                g_pipes[i].eof = true;
            }
        }

        if (pcbs[t].thread_ipc_frame) {
            seL4_ARM_Page_Unmap(pcbs[t].thread_ipc_frame);
        }

        for (int i = 0; i < pcbs[t].cap_tracker.count; i++) {
            seL4_CPtr cap_to_free = pcbs[t].cap_tracker.caps[i];
            seL4_CNode_Revoke(root_cnode, cap_to_free, seL4_WordBits);
            seL4_CNode_Delete(root_cnode, cap_to_free, seL4_WordBits);
            alloc.free(cap_to_free);
        }
        pcbs[t].cap_tracker.count = 0;
        pcbs[t].active = false;
    }
}

static void generic_recover_process(int pid, seL4_CPtr ep, seL4_CPtr med_ep, PsychAllocator &alloc,
                                    seL4_CPtr root_cnode, seL4_CPtr root_vspace, seL4_CPtr normal_untyped,
                                    seL4_CPtr shm_frame_root, seL4_CPtr console_ep, seL4_CPtr timer_ep, seL4_CPtr blk_ep,
                                    bool respawn = true,
                                    // issuse.txt №74/б — true, если процесс попал сюда из
                                    // обработчика fault'а (VMFault/CapFault). Такой поток
                                    // ЗАБЛОКИРОВАН ядром на fault-endpoint'е и не исполняется
                                    // ни на одном ядре — Suspend над ним безопасен без всяких
                                    // приготовлений, а его слот состояния навсегда застрял в
                                    // BUSY (до seL4_Recv он уже не дойдёт), так что ожидание
                                    // PARKED просто сожгло бы бюджет и отменило законное
                                    // восстановление.
                                    bool target_faulted = false) {
    if (pid <= 0 || pid >= 256 || !pcbs[pid].active)
        return;

    // 1. Копируем метаданные упавшего процесса во временный буфер
    ProcessControlBlock meta = pcbs[pid];

    uart_puts("\n[WATCHDOG] Emergency recovery initiated for PID: "); uart_putdec(pid);
    uart_puts(" ("); uart_puts(meta.name); uart_puts(")\n");

    // issuse.txt №74, часть "б" — ПЕРВЫМ ДЕЛОМ, до единого разрушающего
    // действия ниже: убедиться, что seL4_TCB_Suspend() над этим процессом
    // вообще можно звать (см. развёрнутое объяснение у
    // prepare_driver_for_suspend()). Порядок принципиален: всё, что идёт
    // дальше — освобождение vfs_mutex, отмена отложенных ответов, спасение
    // зависшего VFS-клиента — необратимо, и делать это ради восстановления,
    // которое мы всё равно не имеем права завершить, значит оставить
    // систему в состоянии хуже исходного.
    // ПЕРЕД ЛЮБЫМ разрушением: если PCIe-шина похоже застопорена, сперва
    // снять причину. Иначе deleteASID() ниже по цепочке (снос VSpace
    // жертвы) выполнит `dsb sy`, тот будет ждать незавершённую
    // PCIe-транзакцию и заморозит ЭТО ядро внутри ядра ОС, с большим
    // замком в руках — вся плата умрёт молча. Полный разбор с JTAG'а — у
    // usb_bus_stalled_suspect() выше. Порядок здесь принципиален: сначала
    // шина, потом капабилити.
    // usb_driver НИКОГДА не пересоздаётся (решение пользователя
    // 2026-09-06). Причина не в удобстве, а в том, что пересоздание —
    // это снос VSpace, то есть deleteASID -> `dsb sy`, то есть ровно та
    // инструкция, на которой JTAG поймал замёрзшее ядро с большим замком
    // ядра ОС в руках (см. usb_bus_stalled_suspect() выше). Для USB
    // "восстановление" = сброс шины, и только он: root_usb_full_reset()
    // не трогает ничьи капабилити и потому этой ловушки не содержит.
    if (meta.is_driver == 6) {
        if (target_faulted) {
            // Драйвер УПАЛ (VMFault/CapFault). Сброс шины тут не поможет
            // — сломан сам процесс. Но и сносить его нельзя по причине
            // выше. Подвешиваем поток (это безопасно, Suspend не делает
            // dsb sy) и честно говорим, что USB до перезагрузки не будет:
            // живая система без USB заведомо лучше мёртвой платы.
            if (pcbs[pid].tcb) seL4_TCB_Suspend(pcbs[pid].tcb);
            uart_puts("[WATCHDOG] usb_driver УПАЛ. Пересоздание для него запрещено (снос VSpace вызывает dsb sy, который вешает всю плату при застопоренной шине) — поток остановлен, USB не работает до перезагрузки.\n");
            return;
        }
        uart_puts("[WATCHDOG] usb_driver: вместо пересоздания процесса выполняю полный сброс шины (единственный разрешённый для USB путь восстановления).\n");
        if (g_usb_cmd_ep != 0) root_usb_full_reset(timer_ep, g_usb_cmd_ep);
        g_driver_progress_seen_ms[6] = 0;
        return;
    }

    if (usb_bus_stalled_suspect(timer_ep)) {
        uart_puts("[WATCHDOG] PCIe-шина похоже застопорена (usb_driver замер на шаге \"");
        uart_puts(usb_step_name(driver_state_word(6, DRIVER_STATE_WORD_STEP)));
        uart_puts("\"). Разрушать VSpace сейчас НЕЛЬЗЯ — dsb sy в deleteASID заморозил бы ядро ОС целиком. Сбрасываю шину первым делом.\n");
        root_usb_full_reset(timer_ep, g_usb_cmd_ep);
        // Отсчёт прогресса начинаем заново: сброс либо оживил драйвер,
        // либо нет — решать это будет следующий тик watchdog'а, а не
        // этот, уже устаревший, замер.
        g_driver_progress_seen_ms[6] = 0;
    }

    if (!target_faulted && !prepare_driver_for_suspend(pid, timer_ep)) {
        uart_puts("[WATCHDOG] Восстановление "); uart_puts(meta.name);
        uart_puts(" ОТМЕНЕНО: процесс не на ядре 0 и не отвечает признаком PARKED — это настоящее зависание (аппаратный стопор шины, см. issuse.txt/usb_driver). Программно оно не лечится; последний рубеж — PM_WDOG.\n");
        return;
    }

    // issuse.txt №3 — у жертвы могли остаться SYS_CLONE-потоки. Убрать их
    // ОБЯЗАТЕЛЬНО до цикла по cap_tracker ниже: он уничтожит корни VSpace/
    // CSpace, на которых эти потоки исполняются (см. reap_clone_threads_of).
    reap_clone_threads_of(pid, alloc, root_cnode);

    // issuse.txt №70 — если жертва оказалась ТЕКУЩИМ держателем глобального
    // vfs_mutex_ep (см. g_vfs_lock_holder_pid/SYS_VFS_LOCK_NOTIFY выше),
    // форсируем seL4_Signal сам ЕЁ ИМЕНИ — обычный vfs_unlock() жертва уже
    // никогда не вызовет (убита/упала ДО него), а голый notification-
    // семафор без владельца/robust-семантики иначе остаётся захваченным
    // НАВСЕГДА, вешая абсолютно любую будущую VFS-команду (даже `ps`) от
    // ЛЮБОГО процесса в системе — hw-подтверждённый баг, не гипотеза
    // (см. issuse.txt: `kill` фонового `timedread.elf` посреди чтения
    // навсегда повесил `ps` без единого пути восстановления кроме ребута).
    // Тело вынесено в release_vfs_lock_if_held_by() (см. выше) — та же
    // уборка понадобилась SYS_EXIT/SYS_KILL по issuse.txt №3, и держать
    // две расходящиеся копии одной логики не стоит.
    release_vfs_lock_if_held_by(pid);

    // issuse.txt: если у жертвы был отложенный (deferred-reply) запрос в
    // timer_driver (SYS_SLEEP_MS) или uart_driver (SYS_READ), просим их
    // отбросить свой слот ДО того, как реально уберём жертву — иначе когда
    // дедлайн/ввод наступит, они попробуют ответить по капе на уже не
    // существующий TCB и уронят "Attempted to invoke a null cap" (не
    // фатально, но грязно и маскирует реальные проблемы в логе). Не шлём
    // САМОМУ восстанавливаемому драйверу, если это как раз он и есть — его
    // TCB уже подвешен/умирает, ответа не будет никогда.
    if (timer_ep != 0 && meta.is_driver != 2) {
        seL4_SetMR(0, SYS_CANCEL_PENDING_FOR_PID);
        seL4_SetMR(1, (seL4_Word)pid);
        seL4_Call(timer_ep, seL4_MessageInfo_new(0, 0, 0, 2));
    }
    if (console_ep != 0 && meta.is_driver != 1) {
        seL4_SetMR(0, SYS_CANCEL_PENDING_FOR_PID);
        seL4_SetMR(1, (seL4_Word)pid);
        seL4_Call(console_ep, seL4_MessageInfo_new(0, 0, 0, 2));
    }

    // issuse.txt №69 — blk_driver/usb_driver теперь SaveCaller'ят reply-капу
    // ТЕКУЩЕГО VFS-клиента в VFS_PENDING_REPLY_SLOT своего CNode на всё время
    // обработки команды (см. common.h/VFS_PENDING_REPLY_SLOT). Если жертва —
    // именно blk/usb driver, вытаскиваем эту капу из meta.cspace ПРЯМО
    // СЕЙЧАС, пока CNode жертвы ещё жив (ниже, в цикле по cap_tracker, он
    // будет revoke+delete'нут вместе со всем остальным) — и отвечаем
    // клиенту сами, с ошибкой, вместо того чтобы он завис навсегда (обычный
    // seL4_Call не даёт root'у другого способа вмешаться, см. issuse.txt).
    // seL4_CNode_Move, НЕ Copy — hw-подтверждено 2026-08-24: reply-капы в
    // seL4 принципиально не копируются (deriveCap() для cap_reply_cap
    // ВСЕГДА возвращает null-cap, kernel/src/object/objecttype.c) — Copy
    // ловил "Mutated cap would be invalid" (seL4_IllegalOperation) на
    // каждый вызов, спасение ни разу не срабатывало, а зависший клиент
    // навсегда держал глобальный vfs_mutex_ep, вешая заодно КАЖДУЮ
    // следующую VFS-команду (даже ps/ls от других процессов). Move не
    // проходит через deriveCap() (kernel/src/object/cnode.c,
    // case CNodeMove — прямой cteMove) — тот же приём, что уже использует
    // сам seL4_CNode_SaveCaller() внутри ядра для translateCaller. Честно
    // проваливается (seL4_FailedLookup), если слот пуст — это НОРМАЛЬНЫЙ,
    // самый частый случай ("жертва была на seL4_Recv, не в процессе
    // VFS-команды"), не ошибка, просто нечего спасать.
    // issuse.txt №74/б (доп., 2026-09-05): спасать нечего, если драйвер
    // стоит в своём seL4_Recv — PARKED по определению означает, что он уже
    // ответил предыдущему клиенту и никакой reply-капы в
    // VFS_PENDING_REPLY_SLOT не держит. Раньше Move звался безусловно и в
    // этом (самом частом) случае честно проваливался, но КАЖДЫЙ раз печатал
    // в лог предупреждение самого ядра "CNode Copy/Mint/Move/Mutate: Source
    // slot invalid or empty" — шум, который вдобавок маскировал бы
    // настоящую проблему с капами, случись она.
    bool victim_may_hold_reply = (driver_state_read(meta.is_driver) != DRIVER_STATE_PARKED);
    if ((meta.is_driver == 3 || meta.is_driver == 6) && victim_may_hold_reply) {
        seL4_CPtr rescue_slot = alloc.alloc_slot();
        if (rescue_slot != 0) {
            seL4_Error rescue_err = seL4_CNode_Move(root_cnode, rescue_slot, seL4_WordBits,
                                                     meta.cspace, VFS_PENDING_REPLY_SLOT, 8);
            if (rescue_err == seL4_NoError) {
                uart_puts("[WATCHDOG] Найден зависший VFS-клиент — будим с ошибкой вместо вечного зависания.\n");
                seL4_SetMR(0, (seL4_Word)-1); // общий "ошибка" статус, тот же, что и у обычных VFS-неудач
                seL4_SetMR(1, 0);
                seL4_Send(rescue_slot, seL4_MessageInfo_new(0, 0, 0, 2)); // 2 слова — покрывает и 1-словные, и 2-словные VFS-ответы
                seL4_CNode_Delete(root_cnode, rescue_slot, seL4_WordBits);
            }
            alloc.free(rescue_slot);
        }
    }

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

    // issuse.txt п.3 — освобождение RAM-арены (см. ram_retype()/
    // acquire_cmd_arena() выше), тот же приём, что в SYS_EXIT/SYS_KILL —
    // watchdog-респавн /sbin-команды (редкий случай, но встречается,
    // см. комментарий у ProcessControlBlock::path).
    if (pcbs[pid].cmd_arena != 0) {
        release_cmd_arena(root_cnode, pcbs[pid].cmd_arena);
        g_ram_bytes_used -= pcbs[pid].arena_bytes_used;
        pcbs[pid].cmd_arena = 0;
        pcbs[pid].arena_bytes_used = 0;
    }

    // Если процесс использовал SHM, уничтожаем копии Capabilities,
    // чтобы вернуть слоты в аллокатор и отмапить память!
    if (meta.has_shm) {
        for (int i = 0; i < SHM_TOTAL_PAGES; i++) {
            if (meta.shm_copies[i] != 0) {
                seL4_CNode_Delete(root_cnode, meta.shm_copies[i], seL4_WordBits);
                alloc.free(meta.shm_copies[i]);
            }
        }
    }

    // issuse.txt №71 — is_driver==253/254 покрывает ДВА разных класса:
    // настоящие персистентные сервисы (init.conf, напр. balancer — ХОТЯТ
    // авто-восстановление после краха) И ручные одноразовые тестовые
    // инструменты (/sbin/tests/*, exec'нутые вручную — НЕ предназначены
    // жить долго/переживать kill, respawn для них бессмысленный шум:
    // "не найден на диске" — не настоящая ошибка, просто driver ещё
    // доигрывал ответ на ИХ ЖЕ прерванный запрос, см. issuse.txt). meta.path
    // для них всегда начинается с "/sbin/tests/" (см. shell.cpp: exec
    // хранит туда буквально то, что набрал пользователь) — единственный
    // надёжный признак, is_driver сам по себе не различает эти два случая.
    bool is_manual_test_tool = (strncmp(meta.path, "/sbin/tests/", 12) == 0);

    if (respawn && is_manual_test_tool) {
        uart_puts("[WATCHDOG] "); uart_puts(meta.name);
        uart_puts(" — ручной тестовый инструмент (/sbin/tests/), респавн не требуется.\n");
    } else if (respawn && (meta.is_driver > 0 || strcmp(meta.name, "shell") == 0)) {
        uart_puts("[WATCHDOG] Respawning critical system component...\n");

        // issuse.txt (было: watchdog не мог респавнить не-драйверные
        // процессы — /sbin-утилиты, обычный exec, init.conf-сервисы вроде
        // balancer): их ELF не встроен в CPIO-архив образа, грузится с
        // диска — respawn обязан читать его ОТТУДА ЖЕ, иначе watchdog не
        // найдёт файл во встроенном архиве (там его никогда и не было) и
        // респавн провалится. meta.path — реальный путь, которым процесс
        // был загружен изначально (см. ProcessControlBlock::path и все
        // спавн-сайты, где он проставляется); пусто — значит процесс
        // встроен в CPIO (драйверы uart/timer/blk/net, shell), респавн как
        // раньше, из архива по имени.
        char *recover_elf_data = nullptr;
        int recover_elf_size = 0;
        if (meta.path[0] != '\0') {
            recover_elf_size = load_elf_from_disk(blk_ep, meta.path, g_elf_load_buffer);
            if (recover_elf_size > 0) {
                recover_elf_data = g_elf_load_buffer;
            } else {
                uart_puts("[WATCHDOG] "); uart_puts(meta.path); uart_puts(" не найден на диске — respawn невозможен.\n");
                recover_elf_size = 0;
            }
        }

        int new_pid = spawn_process(meta.name, recover_elf_data, recover_elf_size, ep, med_ep, alloc, root_cnode, root_vspace, normal_untyped, shm_frames[0],
                                    meta.is_driver, console_ep, timer_ep, blk_ep, console_ep, console_ep, console_ep,
                                    meta.irq_ntfn, meta.irq_handler, meta.hw_frame,
                                    nullptr, meta.net_cmd_recv_ep, meta.net_cmd_send_ep,
                                    meta.wifi_cmd_recv_ep, meta.wifi_cmd_send_ep,
                                    // issuse.txt №8 — раньше здесь были захардкожены 0,0,0
                                    // (mbox_regs_frame/mbox_buf_frame_param/mbox_buf_paddr_param),
                                    // теряя VideoCore mailbox timer_driver'а при каждом respawn.
                                    meta.mbox_regs_frame, meta.mbox_buf_frame, meta.mbox_buf_paddr,
                                    0,       // extra_ntfn_param (heartbeat->net_driver, только собственный респавн timer_driver — не восстанавливается здесь)
                                    // issuse.txt №65 — раньше здесь тоже были захардкожены 0,0
                                    // (blk_dma_frame_param/blk_dma_paddr_param) — respawn
                                    // blk_driver терял приватные DMA-страницы, hardware_emmc_
                                    // read/write отказывали немедленно (g_blk_dma_paddr==0),
                                    // exFAT не мог перемонтироваться, все /sbin-команды
                                    // переставали находиться.
                                    meta.blk_dma_frame, meta.blk_dma_paddr,
                                    0,       // extra_ntfn2_param (heartbeat->wifi_driver, аналогично extra_ntfn_param)
                                    meta.net_wifi_rx_badged, meta.wifi_tx_wake_badged, meta.blk_heartbeat_badged,
                                    meta.mmc_irq_handler,
                                    meta.blk_dma_frame2, meta.blk_dma_paddr2, // issuse.txt №65 — вторая DMA-страница (multi-block ADMA2-дескриптор), тот же баг
                                    0,       // vfs_mutex_ntfn_param
                                    nullptr, // cwd_payload
                                    -1,      // stdout_pipe_id
                                    // issuse.txt (найдено на живом железе 2026-08-10) — usb_driver
                                    // (is_driver==6) respawn: раньше сюда не передавалось НИЧЕГО
                                    // из xHCI DMA-состояния (usb_cmd_recv_ep/usb_dma/PCIe RC-
                                    // фреймы) — детерминированный крах-цикл на первой же
                                    // аппаратной операции нового процесса, вплоть до зависания
                                    // платы. Для остальных типов процессов эти 4 параметра
                                    // безвредны — spawn_process читает их только при is_driver==6.
                                    usb_cmd_recv_ep, &usb_dma, pcie_rc_frame, pcie_err_frame,
                                    0,       // usb_storage_ep_param
                                    0,       // usb_heartbeat_ntfn_param
                                    // Фаза 3b — обе капы watchdog'а переживают
                                    // респавн так же, как остальные HW-профили
                                    // выше (тот же класс потери, что issuse.txt
                                    // №8/№65, если бы не сохранялись).
                                    meta.liveness_ntfn_badged,
                                    meta.blk_liveness_tick_badged,
                                    // Начало GPIO-драйвера (см. h/gpio.h) — тот
                                    // же класс потери через респавн, что у
                                    // blk_dma_frame выше, если бы не сохранялся.
                                    meta.gpio_frame,
                                    // issuse.txt №73/№75 — аппаратный watchdog,
                                    // тот же класс потери через респавн.
                                    meta.pm_wdog_frame);

        if (new_pid > 0) {
            // path не приходит через spawn_process (и так огромный список
            // параметров) — переносим вручную, чтобы ВТОРОЙ подряд краш
            // тоже смог респавниться (см. path выше).
            strncpy(pcbs[new_pid].path, meta.path, 63);
            pcbs[new_pid].path[63] = '\0';
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

// Фаза 9.B (см. ROADMAP.md): простой int со знаком — simple_atoi (см.
// shell.cpp/sys_client.h) не понимает минус, а ядро "-1" в init.conf
// (= не переставлять) как раз отрицательное.
static int simple_atoi_signed(const char *str) {
    bool neg = (*str == '-');
    if (neg) str++;
    int res = 0;
    while (*str >= '0' && *str <= '9') { res = res * 10 + (*str - '0'); str++; }
    return neg ? -res : res;
}

// Фаза 9.B (см. ROADMAP.md): читает /etc/init.conf и автозапускает
// перечисленные там сервисы (например /service/balancer.elf) — вызывается
// ОДИН раз, как только все базовые драйверы готовы (см. case
// SYS_DRIVER_READY). Формат строки: "<имя> <путь> <приоритет> <ядро>",
// разделители — пробелы/табы; '#'-комментарии и пустые строки пропускаются;
// ядро -1 = не переставлять (остаётся там, где заспавнился). Ошибка на
// отдельной строке (не хватает полей, файл не читается, битый ELF) — лог и
// переход к следующей строке; отсутствие самого /etc/init.conf — не
// ошибка, просто нет сконфигурированных сервисов.
static void start_init_services(seL4_CPtr ep, seL4_CPtr med_ep, PsychAllocator &alloc,
                                 seL4_CPtr root_cnode, seL4_CPtr root_vspace, seL4_CPtr normal_untyped,
                                 seL4_CPtr blk_ep, seL4_CPtr console_ep, seL4_CPtr timer_ep,
                                 seL4_CPtr net_cmd_send_ep, seL4_CPtr vfs_mutex_ntfn) {
    static char conf_buf[16384];
    int conf_len = load_text_config_from_disk(blk_ep, "/etc/init.conf", conf_buf);
    if (conf_len <= 0) {
        uart_puts("[ROOT] init.conf не найден — автозапуск сервисов пропущен.\n");
        return;
    }
    if (conf_len >= (int)sizeof(conf_buf)) conf_len = sizeof(conf_buf) - 1;
    conf_buf[conf_len] = '\0';

    char *line = conf_buf;
    while (line) {
        char *newline = line;
        while (*newline && *newline != '\n') newline++;
        bool has_more = (*newline == '\n');
        *newline = '\0';

        char *p = line;
        while (*p == ' ' || *p == '\t' || *p == '\r') p++;

        if (*p == '\0' || *p == '#') {
            line = has_more ? newline + 1 : nullptr;
            continue;
        }

        char *name = p;
        while (*p && *p != ' ' && *p != '\t') p++;
        if (*p) { *p++ = '\0'; }
        while (*p == ' ' || *p == '\t') p++;

        char *path = p;
        while (*p && *p != ' ' && *p != '\t') p++;
        if (*p) { *p++ = '\0'; }
        while (*p == ' ' || *p == '\t') p++;

        char *prio_str = p;
        while (*p && *p != ' ' && *p != '\t') p++;
        if (*p) { *p++ = '\0'; }
        while (*p == ' ' || *p == '\t') p++;

        char *core_str = p;
        while (*p && *p != ' ' && *p != '\t' && *p != '\r') p++;
        *p = '\0';

        if (name[0] == '\0' || path[0] == '\0' || prio_str[0] == '\0' || core_str[0] == '\0') {
            uart_puts("[ROOT] init.conf: строка с неверным числом полей, пропускаю: "); uart_puts(line); uart_puts("\n");
            line = has_more ? newline + 1 : nullptr;
            continue;
        }

        int priority = simple_atoi_signed(prio_str);
        int core = simple_atoi_signed(core_str);

        int elf_size = load_elf_from_disk(blk_ep, path, g_elf_load_buffer);
        if (elf_size <= 0) {
            uart_puts("[ROOT] init.conf: не удалось прочитать "); uart_puts(path);
            uart_puts(", пропускаю сервис "); uart_puts(name); uart_puts("\n");
            line = has_more ? newline + 1 : nullptr;
            continue;
        }

        elf_t elf;
        if (elf_newFile(g_elf_load_buffer, elf_size, &elf) != 0) {
            uart_puts("[ROOT] init.conf: битый ELF "); uart_puts(path);
            uart_puts(", пропускаю сервис "); uart_puts(name); uart_puts("\n");
            line = has_more ? newline + 1 : nullptr;
            continue;
        }

        int pid = spawn_process(name, g_elf_load_buffer, elf_size, ep, med_ep, alloc, root_cnode, root_vspace, normal_untyped,
                                shm_frames[0], 253, console_ep, timer_ep, blk_ep, console_ep, console_ep, console_ep,
                                0, // irq_ntfn
                                0, // irq_handler
                                0, // hw_frame
                                nullptr, // args_payload — сервисам аргументы не нужны
                                0, // net_cmd_recv_ep
                                net_cmd_send_ep,
                                0, // wifi_cmd_recv_ep
                                0, // wifi_cmd_send_ep
                                0, // mbox_regs_frame
                                0, // mbox_buf_frame_param
                                0, // mbox_buf_paddr_param
                                0, // extra_ntfn_param
                                0, // blk_dma_frame_param
                                0, // blk_dma_paddr_param
                                0, // extra_ntfn2_param
                                0, // net_wifi_rx_badged_param
                                0, // net_wifi_tx_wake_param
                                0, // extra_ntfn3_param
                                0, // mmc_irq_handler_param
                                0, // blk_dma_frame2_param
                                0, // blk_dma_paddr2_param
                                vfs_mutex_ntfn,
                                nullptr); // cwd_payload — сервисам не нужен

        if (pid > 0) {
            if (priority >= 0 && priority <= 255) {
                seL4_TCB_SetPriority(pcbs[pid].tcb, seL4_CapInitThreadTCB, priority);
            }
            if (core >= 0 && core <= 3) {
                seL4_TCB_SetAffinity(pcbs[pid].tcb, core);
                pcbs[pid].core = core;
            }
            // issuse.txt: запоминаем реальный ПУТЬ (не name — тот короткий,
            // "balancer", по нему ищут процесс, path — "/service/balancer.elf",
            // именно ЕГО читает load_elf_from_disk() чуть выше), чтобы
            // watchdog мог перечитать этот же файл при аварийном респавне.
            strncpy(pcbs[pid].path, path, 63);
            pcbs[pid].path[63] = '\0';
            uart_puts("[ROOT] Сервис запущен: "); uart_puts(name); uart_puts(" (pid "); uart_putdec(pid); uart_puts(")\n");
        } else {
            uart_puts("[ROOT] init.conf: не удалось запустить сервис "); uart_puts(name); uart_puts("\n");
        }

        line = has_more ? newline + 1 : nullptr;
    }
}

// Фаза 4 плана "Сигналы драйверам" — /etc/auto_restart.conf: одно имя драйвера
// на строку (должно точно совпадать с pcbs[].name — "blk_driver"/
// "net_driver"/"wifi_driver"/"usb_driver"), #-комментарии/пустые строки
// пропускаются, тот же токенайзер-стиль, что start_init_services() выше, но
// проще (нет path/priority/core — только имя). Гейтит ТОЛЬКО автоматический
// heartbeat-триггер (g_auto_restart_enabled[], см. главный цикл выше) — НЕ
// затрагивает SYS_KILL/SYS_RECOVER/SYS_DRIVER_SIGNAL, те остаются доступны
// admin-вызывающему всегда, независимо от этого файла. Fail-closed: файл не
// найден/не читается -> ВСЕ 4 флага остаются false (обнуляются здесь же,
// перед попыткой чтения) — без файла НИКАКОГО автовосстановления по
// таймауту, только свежая сборка со свежим load_chain включает эту защиту
// (см. план). Читается один раз, из той же точки, что start_init_services()
// (case SYS_DRIVER_READY, all_drivers_ready() && !init_services_started).
static void load_auto_restart_config(seL4_CPtr blk_ep) {
    g_auto_restart_enabled[3] = false;
    g_auto_restart_enabled[4] = false;
    g_auto_restart_enabled[5] = false;
    g_auto_restart_enabled[6] = false;

    static char conf_buf[4096];
    int conf_len = load_text_config_from_disk(blk_ep, "/etc/auto_restart.conf", conf_buf);
    if (conf_len <= 0) {
        uart_puts("[ROOT] auto_restart не найден — автовосстановление драйверов по heartbeat выключено.\n");
        return;
    }
    if (conf_len >= (int)sizeof(conf_buf)) conf_len = sizeof(conf_buf) - 1;
    conf_buf[conf_len] = '\0';

    char *line = conf_buf;
    while (line) {
        char *newline = line;
        while (*newline && *newline != '\n') newline++;
        bool has_more = (*newline == '\n');
        *newline = '\0';

        char *p = line;
        while (*p == ' ' || *p == '\t' || *p == '\r') p++;

        if (*p == '\0' || *p == '#') {
            line = has_more ? newline + 1 : nullptr;
            continue;
        }

        char *name = p;
        while (*p && *p != ' ' && *p != '\t' && *p != '\r') p++;
        *p = '\0';

        int matched_driver = -1;
        if (strcmp(name, "blk_driver") == 0) matched_driver = 3;
        else if (strcmp(name, "net_driver") == 0) matched_driver = 4;
        else if (strcmp(name, "wifi_driver") == 0) matched_driver = 5;
        else if (strcmp(name, "usb_driver") == 0) matched_driver = 6;

        if (matched_driver != -1) {
            g_auto_restart_enabled[matched_driver] = true;
            uart_puts("[ROOT] auto_restart: включено для "); uart_puts(name); uart_puts("\n");
        } else {
            uart_puts("[ROOT] auto_restart: неизвестное имя драйвера, пропускаю: "); uart_puts(name); uart_puts("\n");
        }

        line = has_more ? newline + 1 : nullptr;
    }
}

// План "Сигналы драйверам" — тела SYS_STOP_WIFI/SYS_START_WIFI вынесены
// в отдельные функции (были инлайн в своих case-блоках), чтобы новый
// SYS_DRIVER_SIGNAL мог переиспользовать уже hw-проверенный
// kill+respawn-путь wifi_driver'а вместо параллельного механизма — у
// wifi_driver's bring-up нет факторизованной init-функции (вся
// orchestration зависимостей probe→bus-width→backplane→sdpcm вморожена
// в main()), поэтому "restart" для него — это по-прежнему stop+start,
// не лёгкий сигнал. Логика 1:1 со старыми case-блоками, только реплай
// теперь один раз у вызывающего (было несколько ранних seL4_Reply()
// внутри — семантика та же, просто общий путь ответа).
static void root_stop_wifi_driver(seL4_CPtr ep, seL4_CPtr med_ep, PsychAllocator &alloc,
                                   seL4_CPtr root_cnode, seL4_CPtr root_vspace, seL4_CPtr normal_untyped,
                                   seL4_CPtr shm_frame_root, seL4_CPtr console_ep, seL4_CPtr timer_ep, seL4_CPtr blk_ep,
                                   seL4_Word *out_status) {
    int target_pid = -1;
    for (int i = 1; i < 256; i++) {
        if (pcbs[i].active && strcmp(pcbs[i].name, "wifi_driver") == 0) { target_pid = i; break; }
    }
    if (target_pid == -1) {
        *out_status = (seL4_Word)-1; // не запущен
        return;
    }
    generic_recover_process(target_pid, ep, med_ep, alloc, root_cnode, root_vspace, normal_untyped,
                            shm_frame_root, console_ep, timer_ep, blk_ep, /*respawn=*/false);
    g_wifi_driver_ready = false;
    // Фаза 4.5.3 (Wi-Fi data-plane): гасим link-state в SHM-мейлбоксе,
    // чтобы net_driver увидел wifi-интерфейс down в течение одного
    // тика, а не ждал, пока сам wifi_driver (уже убитый) это сделает.
    *(uint32_t*)(rootserver_shm_base + WIFI_SHM_LINK_STATE_OFFSET) = 0;
    *(uint32_t*)(rootserver_shm_base + WIFI_SHM_LINK_STATE_REASON_OFFSET) = WIFI_LINK_REASON_SYS_STOP_WIFI; // диагностика живого бага, см. platform.h
    flush_rootserver_shm(); // иначе запись может годами остаться в dirty-кэше root'а и не долететь до RAM, а net_driver читает эту страницу некэшируемо (см. flush_rootserver_shm())
    *out_status = 0;
}

static void root_start_wifi_driver(seL4_CPtr ep, seL4_CPtr med_ep, PsychAllocator &alloc,
                                    seL4_CPtr root_cnode, seL4_CPtr root_vspace, seL4_CPtr normal_untyped,
                                    seL4_CPtr shm_frame_root, seL4_CPtr console_ep, seL4_CPtr timer_ep, seL4_CPtr blk_ep,
                                    seL4_CPtr wifi_wake_ntfn, seL4_CPtr wifi_irq_ntfn, seL4_CPtr wifi_sdio_frame,
                                    seL4_CPtr wifi_cmd_recv_ep, seL4_CPtr net_wifi_rx_badged, seL4_CPtr vfs_mutex_ntfn,
                                    seL4_CPtr wifi_liveness_ntfn, // Фаза 3b: капа-"я жив" (см. common.h/DRIVER_LIVENESS_WIFI_BADGE)
                                    seL4_Word *out_status) {
    int existing_pid = -1;
    for (int i = 1; i < 256; i++) {
        if (pcbs[i].active && strcmp(pcbs[i].name, "wifi_driver") == 0) { existing_pid = i; break; }
    }
    if (existing_pid != -1) {
        *out_status = 1; // уже запущен
        return;
    }
    if (wifi_cmd_recv_ep == 0) {
        *out_status = (seL4_Word)-1; // не скомпилирован (RPI4_ENABLE_WIFI=false)
        return;
    }
    g_wifi_driver_ready = false;
    // Фаза 9.B (см. ROADMAP.md): wifi_driver теперь грузится С ДИСКА
    // (/service/wifi.elf), а не из встроенного в образ CPIO-архива —
    // тот же принцип, что /sbin/*.elf и /service/balancer.elf, просто
    // без общего init.conf-парсера: is_driver==5 несёт СВОЙ, гораздо
    // более богатый набор capability (SDIO MMIO, heartbeat/RX-badge и
    // т.д.), который generic-парсер init.conf не понимает — это
    // специфика именно этого драйвера, отдельный путь.
    int wifi_elf_size = load_elf_from_disk(blk_ep, "/service/wifi.elf", g_elf_load_buffer);
    if (wifi_elf_size <= 0) {
        uart_puts("[ROOT] /service/wifi.elf не найден на диске.\n");
        *out_status = (seL4_Word)-1;
        return;
    }
    elf_t wifi_elf;
    if (elf_newFile(g_elf_load_buffer, wifi_elf_size, &wifi_elf) != 0) {
        uart_puts("[ROOT] /service/wifi.elf: битый ELF.\n");
        *out_status = (seL4_Word)-1;
        return;
    }
    // wifi_irq_ntfn передаётся ДЕВЯТЫМ параметром (irq_handler слот) —
    // тот же приём, что у blk_irq_ntfn: обычная capability на
    // нотификацию, не настоящий IRQHandler (см. is_driver==5 блок выше
    // и common.h/SYS_WIFI_IRQ_ACK). wifi_wake_ntfn идёт irq_ntfn-
    // параметром (TCB-bind) — то же самое место, где uart/timer/net
    // получают свою нотификацию (Фаза 4.5, Wi-Fi data-plane, см.
    // common.h/WIFI_EVENT_HEARTBEAT|WIFI_EVENT_TX_READY).
    int new_pid = spawn_process("wifi_driver", g_elf_load_buffer, wifi_elf_size, ep, med_ep, alloc, root_cnode, root_vspace, normal_untyped, shm_frame_root,
                                5, console_ep, timer_ep, blk_ep, console_ep, console_ep, console_ep, wifi_wake_ntfn, wifi_irq_ntfn, wifi_sdio_frame, nullptr,
                                0, 0, wifi_cmd_recv_ep, 0, 0, 0, 0, 0, 0, 0, 0, net_wifi_rx_badged,
                                0, 0, 0, 0, 0, vfs_mutex_ntfn,
                                nullptr, -1, 0, nullptr, 0, 0, 0, 0, // cwd_payload .. usb_heartbeat_ntfn_param (не для wifi_driver)
                                wifi_liveness_ntfn); // Фаза 3b: капа-"я жив"
    // issuse.txt: путь на диске для аварийного респавна (name здесь и
    // так "wifi_driver" — короткое имя для поиска процесса, path —
    // отдельно).
    if (new_pid > 0) {
        strncpy(pcbs[new_pid].path, "/service/wifi.elf", 63);
        pcbs[new_pid].path[63] = '\0';
    }
    *out_status = new_pid > 0 ? 0 : (seL4_Word)-1;
}

int main(int argc, char *argv[]) {
    // SMP: root уже структурно гарантированно стартует на ядре 0 (только
    // физическое ядро с индексом 0 доходит до create_initial_thread() в
    // ядре, которая явно зануляет tcbAffinity) — этот вызов технически
    // избыточен, но явно фиксирует намерение и защищает от будущих
    // недоразумений, если этот факт когда-нибудь перестанет быть очевиден.
    seL4_TCB_SetAffinity(seL4_CapInitThreadTCB, 0);

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

    // VideoCore mailbox property-буфер (см. mbox_buf_frame ниже) обязан физически
    // лежать НИЖЕ 1ГиБ (0x40000000) — классическая ARM->VC bus-alias схема
    // (OR верхних битов адреса, см. platform.h/timer_driver.cpp mbox_call())
    // адресует только нижний 1ГиБ ARM-физической памяти. normal_untyped выше
    // выбирается как САМЫЙ БОЛЬШОЙ untyped, а на плате с несколькими ГиБ ОЗУ
    // (см. лог: регионы [1000..3b400000) и [40000000..fc000000)) самый большой
    // почти наверняка окажется ВЫШЕ 1ГиБ — тогда любой вариант bus-адреса для
    // mailbox указывает VideoCore в никуда. Остальным RAM-аллокациям системы
    // (DMA-буферы GENET/EMMC2, стеки, CNode) это ограничение не касается — те
    // DMA-движки собственные ARM-периферийные AXI-мастера, а не VideoCore, и
    // видят всю физическую память как есть.
    size_t low_idx = 0;
    uint8_t low_max_size_bits = 0;
    bool have_low_untyped = false;
    for (size_t i = 0; i < num_untyped; i++) {
        if (!info->untypedList[i].isDevice &&
            info->untypedList[i].paddr < 0x40000000ULL &&
            info->untypedList[i].sizeBits > low_max_size_bits) {
            low_max_size_bits = info->untypedList[i].sizeBits;
            low_idx = i;
            have_low_untyped = true;
        }
    }

    PsychAllocator alloc(info);
    seL4_CPtr root_cnode = seL4_CapInitThreadCNode;
    seL4_CPtr root_vspace = seL4_CapInitThreadVSpace;
    seL4_CPtr normal_untyped = alloc.get_untyped_cap(normal_idx);
    seL4_CPtr low_untyped = have_low_untyped ? alloc.get_untyped_cap(low_idx) : normal_untyped;

    // issuse.txt (найдено при реализации `free`) — заполняем пул ВСЕХ
    // non-device untyped-регионов, не только самого большого (см.
    // RamPoolEntry/g_ram_pool/ram_retype() выше). Слот 0 — ТОТ ЖЕ
    // normal_untyped, что и раньше был единственным вариантом (типовой
    // путь побайтово не меняется). low_idx (mbox, <1ГиБ) сознательно
    // исключён — остаётся отдельным нишевым случаем, как и раньше.
    g_ram_pool[0].cap = normal_untyped;
    g_ram_pool[0].size = 1ULL << max_size_bits;
    g_ram_pool_count = 1;
    for (size_t i = 0; i < num_untyped && g_ram_pool_count < RAM_POOL_MAX; i++) {
        if (info->untypedList[i].isDevice) continue;
        if (i == normal_idx) continue;
        if (have_low_untyped && i == low_idx) continue;
        g_ram_pool[g_ram_pool_count].cap = alloc.get_untyped_cap(i);
        g_ram_pool[g_ram_pool_count].size = 1ULL << info->untypedList[i].sizeBits;
        g_ram_pool_count++;
    }
    // Сортировка по убыванию размера — крупные блоки расходуются раньше
    // мелких, меньше фрагментации для больших объектов (например CNode).
    // Пул маленький, разовая цена при загрузке — простой insertion sort.
    // g_ram_pool[0] уже гарантированно максимален по построению — сортировка
    // его с места не сдвинет.
    for (int i = 1; i < g_ram_pool_count; i++) {
        RamPoolEntry key = g_ram_pool[i];
        int j = i - 1;
        while (j >= 0 && g_ram_pool[j].size < key.size) {
            g_ram_pool[j + 1] = g_ram_pool[j];
            j--;
        }
        g_ram_pool[j + 1] = key;
    }
    g_ram_bytes_total = 0;
    for (int i = 0; i < g_ram_pool_count; i++) g_ram_bytes_total += g_ram_pool[i].size;

    memset(pcbs, 0, sizeof(pcbs));
    next_pid = 1;
    memset(shm_regions, 0, sizeof(shm_regions));

    seL4_CPtr pmd = alloc.alloc_slot();
    seL4_CPtr pt = alloc.alloc_slot();
    ram_retype(normal_untyped, seL4_ARM_PageDirectoryObject, 0, root_cnode, 0, 0, pmd, 1);
    ram_retype(normal_untyped, seL4_ARM_PageTableObject, 0, root_cnode, 0, 0, pt, 1);

    // ИСПРАВЛЕНИЕ: Создаем и мапим дополнительную таблицу страниц (Page Table)
    // для временного окна IPC, которое было перемещено на новый адрес.
    seL4_CPtr pt_ipc_temp = alloc.alloc_slot();
    ram_retype(normal_untyped, seL4_ARM_PageTableObject, 0, root_cnode, 0, 0, pt_ipc_temp, 1);

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
    // USB (Фаза 14, xHCI bring-up — см. ROADMAP.md): выключено по умолчанию.
    // Совсем новый, ни разу не проверенный на живом железе код (первое
    // включение — это и есть первая хардварная попытка, см. ROADMAP.md
    // "Проверка", шаги 1-8) — держим выключенным, пока bring-up не пройден
    // хотя бы до шага 4 (USBSTS.HCH гаснет). RPI4_ENABLE_WIFI выше сейчас
    // true только ПОТОМУ, что уже прошёл этот путь раньше.
    constexpr bool RPI4_ENABLE_USB   = true;

    // PLAT_MBOX_PADDR (0xfe00b000) физически МЕНЬШЕ mini-UART AUX (0xfe215000)
    // и лежит в том же untyped-регионе — должен аллоцироваться СТРОГО ДО
    // uart_frame (тот же приём, что и для PLAT_WIFI_SDIO_PADDR/PLAT_EMMC_PADDR
    // ниже — см. комментарий там же и проверку "target_paddr BEHIND watermark"
    // в alloc_device_frame()).
    seL4_CPtr mbox_regs_frame = alloc_device_frame(info, alloc, PLAT_MBOX_PADDR, root_cnode);
    // issuse.txt №73/№75 — аппаратный watchdog (PM_WDOG/PM_RSTC, см.
    // platform.h/PLAT_PM_PADDR). PLAT_PM_PADDR (0xfe100000) физически
    // МЕЖДУ MBOX (0xfe00b000) и RPI4_GPIO_PADDR (0xfe200000) в ТОМ ЖЕ
    // untyped-регионе — аллоцируется строго здесь, между ними (тот же
    // класс требования, что и у gpio_frame ниже).
    seL4_CPtr pm_wdog_frame = alloc_device_frame(info, alloc, PLAT_PM_PADDR, root_cnode);
    // Начало GPIO-драйвера (зелёный ACT LED, GPIO42 — см. h/gpio.h) — DT
    // bcm2711-rpi-4-b.dts: led-act, gpios = <&gpio 42 GPIO_ACTIVE_HIGH>,
    // напрямую через ARM-side GPIO-контроллер (не firmware-gpio/expgpio,
    // как PWR LED). RPI4_GPIO_PADDR (0xfe200000) физически МЕЖДУ MBOX
    // (0xfe00b000) и mini-UART AUX (0xfe215000) в ТОМ ЖЕ untyped-регионе —
    // ДОЛЖЕН аллоцироваться строго здесь, между ними (см. проверку
    // "target_paddr BEHIND watermark" в alloc_device_frame() выше — тот же
    // класс бага, что уже один раз сломал wifi_sdio_frame/emmc_frame).
    seL4_CPtr gpio_frame = alloc_device_frame(info, alloc, RPI4_GPIO_PADDR, root_cnode);
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
    seL4_CPtr blk_dma_frame2 = 0;
    seL4_Word blk_dma_paddr2 = 0;
    if (RPI4_ENABLE_BLK) {
        emmc_frame = alloc_device_frame(info, alloc, PLAT_EMMC_PADDR, root_cnode);

        // Фаза 4.5/ADMA2 (см. ROADMAP.md) — приватный некэшируемый DMA
        // bounce-буфер blk_driver, тот же приём, что mbox_buf_frame ниже:
        // обычная RAM-страница (не device-фрейм с фиксированным адресом),
        // физический адрес нужен только для программирования
        // EMMC_ADMA_SYSADDR_OFFSET (см. platform.h/blk_driver.cpp).
        blk_dma_frame = alloc.alloc_slot();
        ram_retype(normal_untyped, seL4_ARM_SmallPageObject, 0, root_cnode, 0, 0, blk_dma_frame, 1);
        seL4_ARM_Page_GetAddress_t blk_dma_addr_res = seL4_ARM_Page_GetAddress(blk_dma_frame);
        blk_dma_paddr = (seL4_Word)blk_dma_addr_res.paddr;

        // Фикс задержки (см. situation.txt): переход read/write на multi-block
        // (CMD18/25) — данные теперь занимают всю первую страницу (до 8
        // секторов = 4096 байт), ADMA2-дескриптору (8 байт) больше не хватает
        // места В ТОЙ ЖЕ странице. Вторая приватная некэшируемая страница —
        // только под дескриптор, мапится в blk_driver сразу за первой
        // (PLAT_BLK_DMA_VADDR + 0x1000, тот же 2MB-регион, PUD/PD/PT уже есть).
        blk_dma_frame2 = alloc.alloc_slot();
        ram_retype(normal_untyped, seL4_ARM_SmallPageObject, 0, root_cnode, 0, 0, blk_dma_frame2, 1);
        seL4_ARM_Page_GetAddress_t blk_dma_addr_res2 = seL4_ARM_Page_GetAddress(blk_dma_frame2);
        blk_dma_paddr2 = (seL4_Word)blk_dma_addr_res2.paddr;
    }
    // Пятнадцатая попытка (USB, продолжение Фазы 14, см. ROADMAP.md) — PCIe
    // RC MISC-регистры (PLAT_PCIE_RC_MISC_PADDR = 0xfd504000) лежат в том
    // же /scb untyped-регионе, что GENET (0xfd580000) и AVS (0xfd5d2000)
    // ниже (см. platform.h: "шинные 0x7c000000..0x7fffffff — блок /scb:
    // genet, pcie, xhci") — ФИЗИЧЕСКИ МЕНЬШЕ обоих, значит должен
    // аллоцироваться СТРОГО ДО них (тот же watermark-приём, что и MBOX/
    // UART/WIFI_SDIO/EMMC выше — см. проверку "target_paddr BEHIND
    // watermark" в alloc_device_frame()).
    // pcie_rc_frame/pcie_err_frame теперь file-scope globals (issuse.txt,
    // см. комментарий у их объявления, найдено на живом железе) — не
    // передекларируем здесь, просто присваиваем.
    // pcie_rgr1_frame — ОБЫЧНАЯ локальная переменная main() (issuse.txt
    // №74/в), НЕ file-scope: в отличие от pcie_rc_frame/pcie_err_frame,
    // эта страница никогда не передаётся дочернему процессу (только root
    // мапит её в root_vspace, см. ниже) — generic_recover_process() её
    // никогда не спрашивает, переживать креш usb_driver'а незачем.
    seL4_CPtr pcie_rgr1_frame = 0;
    // issuse.txt №74/в — pcie_cfg_data_frame, как и pcie_rgr1_frame,
    // ОБЫЧНАЯ локальная переменная (см. её же комментарий выше) — только
    // root мапит её в root_vspace, usb_driver её не получает.
    seL4_CPtr pcie_cfg_data_frame = 0;
    // issuse.txt №74/в (38-я hw-попытка, ПРИЧИНА SError) — Type-1 заголовок
    // САМОГО моста, см. подробный разбор у PLAT_PCIE_RC_TYPE1_PADDR/
    // platform.h. Тоже только root'овая страница (usb_driver её не
    // получает — там Command всего моста, ей это трогать незачем).
    seL4_CPtr pcie_rc_type1_frame = 0;
    if (RPI4_ENABLE_USB) {
        // ВАЖНО (watermark): PLAT_PCIE_RC_TYPE1_PADDR (0xfd500000) МЕНЬШЕ
        // всех остальных страниц этого же /scb untyped-региона, поэтому
        // аллоцируется СТРОГО ПЕРВОЙ (см. alloc_device_frame()).
        pcie_rc_type1_frame = alloc_device_frame(info, alloc, PLAT_PCIE_RC_TYPE1_PADDR, root_cnode);
        pcie_rc_frame = alloc_device_frame(info, alloc, PLAT_PCIE_RC_MISC_PADDR, root_cnode);
        // PLAT_PCIE_ERR_PADDR (0xfd506000) > PLAT_PCIE_RC_MISC_PADDR
        // (0xfd504000) — сохраняет монотонный порядок watermark'а того же
        // /scb untyped-региона (см. alloc_device_frame()).
        pcie_err_frame = alloc_device_frame(info, alloc, PLAT_PCIE_ERR_PADDR, root_cnode);
        // issuse.txt №74/в (см. situation.txt) — PLAT_PCIE_CFG_DATA_PADDR
        // (0xfd508000) > PLAT_PCIE_ERR_PADDR (0xfd506000) И < PLAT_PCIE_RGR1_PADDR
        // (0xfd509000) — ДОЛЖНА аллоцироваться СТРОГО между ними (watermark).
        pcie_cfg_data_frame = alloc_device_frame(info, alloc, PLAT_PCIE_CFG_DATA_PADDR, root_cnode);
        // issuse.txt №74/в (см. situation.txt) — PLAT_PCIE_RGR1_PADDR
        // (0xfd509000) > PLAT_PCIE_CFG_DATA_PADDR (0xfd508000) — тот же
        // watermark-приём, следующая по возрастанию страница в том же
        // /scb untyped-регионе. Только root мапит эту страницу (см. ниже,
        // root_vspace) — usb_driver её не получает, ей никогда не
        // передаётся как child-параметр spawn_process().
        pcie_rgr1_frame = alloc_device_frame(info, alloc, PLAT_PCIE_RGR1_PADDR, root_cnode);
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
        seL4_Untyped_Retype(low_untyped, seL4_ARM_SmallPageObject, 0, root_cnode, 0, 0, mbox_buf_frame, 1);
        seL4_ARM_Page_GetAddress_t mbox_buf_addr_res = seL4_ARM_Page_GetAddress(mbox_buf_frame);
        mbox_buf_paddr = (seL4_Word)mbox_buf_addr_res.paddr;
    }

    // Фаза 14 (USB, xHCI) — MMIO-регион (256 4KB-страниц) + приватные DMA-
    // страницы usb_driver'а. scratchpad_count здесь — это СКОЛЬКО страниц
    // root РЕАЛЬНО выделил (всегда весь бюджет USB_MAX_SCRATCHPAD_PAGES,
    // безусловно — root не может прочитать HCSPARAMS2 ДО спавна, чтобы
    // выделить ровно нужное число; см. ROADMAP.md). usb_driver сам при
    // старте читает РЕАЛЬНОЕ требование из HCSPARAMS2 и либо использует
    // подмножество (real_N <= supplied), либо явно отказывается работать
    // со scratchpad (real_N > supplied) — root просто даёт максимум, что
    // может.
    seL4_CPtr usb_xhci_frames[256] = {0};
    // usb_dma теперь file-scope global (см. комментарий у её объявления) —
    // не передекларируем здесь, просто заполняем через тот же alloc_usb_dma_page.
    if (RPI4_ENABLE_USB) {
        for (int i = 0; i < (int)(PLAT_XHCI_SIZE / 4096); i++) {
            usb_xhci_frames[i] = alloc_device_frame(info, alloc, PLAT_XHCI_PADDR + (uintptr_t)i * 4096, root_cnode);
        }

        auto alloc_usb_dma_page = [&](seL4_CPtr &frame_out, seL4_Word &paddr_out) {
            frame_out = alloc.alloc_slot();
            ram_retype(normal_untyped, seL4_ARM_SmallPageObject, 0, root_cnode, 0, 0, frame_out, 1);
            seL4_ARM_Page_GetAddress_t res = seL4_ARM_Page_GetAddress(frame_out);
            paddr_out = (seL4_Word)res.paddr;
        };
        alloc_usb_dma_page(usb_dma.dcbaa_frame, usb_dma.dcbaa_paddr);
        alloc_usb_dma_page(usb_dma.cmdring_frame, usb_dma.cmdring_paddr);
        alloc_usb_dma_page(usb_dma.erst_frame, usb_dma.erst_paddr);
        alloc_usb_dma_page(usb_dma.evtring_frame, usb_dma.evtring_paddr);
        // Фаза 15 — по странице на КАЖДЫЙ xHCI Slot ID (не единственная
        // общая страница, см. h/platform.h PLAT_XHCI_DEVCTX_VADDR).
        for (int i = 0; i < USB_MAX_SLOTS_ENABLED; i++) {
            alloc_usb_dma_page(usb_dma.devctx_frame[i], usb_dma.devctx_paddr[i]);
        }
        alloc_usb_dma_page(usb_dma.inputctx_frame, usb_dma.inputctx_paddr);
        alloc_usb_dma_page(usb_dma.scratchpad_arr_frame, usb_dma.scratchpad_arr_paddr);
        usb_dma.scratchpad_count = USB_MAX_SCRATCHPAD_PAGES;
        for (int i = 0; i < USB_MAX_SCRATCHPAD_PAGES; i++) {
            alloc_usb_dma_page(usb_dma.scratchpad_buf_frame[i], usb_dma.scratchpad_buf_paddr[i]);
        }
        // Фаза 15 — шесть per-device ресурсов, по USB_MAX_DEVICES страниц
        // каждый (было: по одной, единственное устройство). Retype ВСЕХ
        // страниц ОДНОГО ресурса идёт неразрывным циклом (тот же приём,
        // что уже используется для shm_frames[]/scratchpad выше) — иначе
        // между двумя retype'ами может вклиниться on-demand создание
        // PUD/PD/PT (другой аллокатор), и физические страницы окажутся не
        // подряд, а strided-адресация в usb_driver.cpp (base+idx*4096) на
        // это рассчитывает.
        for (int i = 0; i < USB_MAX_DEVICES; i++) alloc_usb_dma_page(usb_dma.ep0_trring_frame[i], usb_dma.ep0_trring_paddr[i]);
        for (int i = 0; i < USB_MAX_DEVICES; i++) alloc_usb_dma_page(usb_dma.ctrl_buf_frame[i], usb_dma.ctrl_buf_paddr[i]);
        for (int i = 0; i < USB_MAX_DEVICES; i++) alloc_usb_dma_page(usb_dma.bulkout_trring_frame[i], usb_dma.bulkout_trring_paddr[i]);
        for (int i = 0; i < USB_MAX_DEVICES; i++) alloc_usb_dma_page(usb_dma.bulkin_trring_frame[i], usb_dma.bulkin_trring_paddr[i]);
        for (int i = 0; i < USB_MAX_DEVICES; i++) alloc_usb_dma_page(usb_dma.cbw_csw_frame[i], usb_dma.cbw_csw_paddr[i]);
        // Bounce-буферы ВСЕХ устройств — ОДИН непрерывный прогон, ВЫРОВНЕННЫЙ
        // ПО 64 КБ.
        //
        // Выравнивание здесь не косметика, а требование xHCI: буфер одного
        // TRB не может ПЕРЕСЕКАТЬ границу 64 КБ. Прогон, выровненный лишь по
        // странице (4 КБ), при передаче в 64 КБ такую границу почти всегда
        // пересекает — контроллер на этом встаёт молча, без кода ошибки
        // (hw 2026-09-06: usb_driver завис на шаге "SCSI-команда", watchdog
        // трижды сбросил шину и сдался). При базе, кратной 64 КБ, передача
        // ровно в 64 КБ заканчивается НА границе, а не пересекает её.
        //
        // Один прогон на все устройства, а не по прогону на каждое: драйвер
        // получает единственный базовый физический адрес и выводит адрес
        // устройства как база + idx*шаг (см. BOOT_USB_BOUNCE_PADDR). Раздельные
        // прогоны этот контракт ломают — уже проверено на железе, READ CAPACITY
        // вернул мусор. Размер слайса устройства (USB_BOUNCE_PAGES*4КБ = 64 КБ)
        // сам кратен 64 КБ, поэтому выравнивание базы выравнивает и всех.
        {
            const int total_pages = USB_MAX_DEVICES * USB_BOUNCE_PAGES;
            const int align_pages = 16; // 64 КБ / 4 КБ — запас на подгонку базы
            const int alloc_pages = total_pages + align_pages;
            static seL4_CPtr bounce_pool[USB_MAX_DEVICES * USB_BOUNCE_PAGES + 16];

            for (int n = 0; n < alloc_pages; n++) bounce_pool[n] = alloc.alloc_slot();
            // Ретайп одним неразрывным циклом — между двумя ретайпами не должно
            // вклиниться on-demand создание таблиц страниц, иначе в физической
            // последовательности появится дыра (тот же приём, что у shm_frames).
            for (int n = 0; n < alloc_pages; n++) {
                ram_retype(normal_untyped, seL4_ARM_SmallPageObject, 0, root_cnode, 0, 0, bounce_pool[n], 1);
            }
            seL4_Word pool_base = (seL4_Word)seL4_ARM_Page_GetAddress(bounce_pool[0]).paddr;
            for (int n = 1; n < alloc_pages; n++) {
                if ((seL4_Word)seL4_ARM_Page_GetAddress(bounce_pool[n]).paddr != pool_base + (seL4_Word)n * 4096) {
                    uart_puts("[ROOT] KERNEL PANIC: bounce-буферы USB получились физически разрывными "
                              "(см. USB_BOUNCE_PAGES в platform.h).\n");
                    while (1) {}
                }
            }
            // Сдвиг до ближайшей границы 64 КБ.
            seL4_Word aligned = (pool_base + 0xFFFFu) & ~(seL4_Word)0xFFFFu;
            int skip = (int)((aligned - pool_base) / 4096);
            if (skip + total_pages > alloc_pages) {
                uart_puts("[ROOT] KERNEL PANIC: не удалось выровнять bounce-буферы USB по 64 КБ.\n");
                while (1) {}
            }
            for (int i = 0; i < USB_MAX_DEVICES; i++) {
                for (int pg = 0; pg < USB_BOUNCE_PAGES; pg++) {
                    usb_dma.bounce_frame[i][pg] = bounce_pool[skip + i * USB_BOUNCE_PAGES + pg];
                }
                usb_dma.bounce_paddr[i] = aligned + (seL4_Word)i * USB_BOUNCE_PAGES * 4096;
            }
        }
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

    // issuse.txt №74/в (см. situation.txt) — root мапит PCIe RC-регистры
    // НАПРЯМУЮ в СВОЙ СОБСТВЕННЫЙ vspace, тем же приёмом, что uart_frame
    // выше (тот же уже покрытый этой PD/PT парой 2MB-регион — отдельная
    // seL4_ARM_PageDirectory_Map/PageTable_Map не нужна, только Page_Map
    // самой страницы). pcie_rc_frame — это ТА ЖЕ физическая страница
    // (0xfd504000), что уже мапится в usb_driver (см. spawn_process(),
    // is_driver==6) — один и тот же frame cap МОЖНО мапить в НЕСКОЛЬКО
    // разных vspace одновременно (стандартный seL4-приём, тот же, что уже
    // используется для SHM-страниц в этом проекте) — root получает
    // независимый от usb_driver доступ к тем же регистрам (PCIE_MISC_
    // PCIE_STATUS/HARD_PCIE_HARD_DEBUG), не идёт через её процесс.
    // pcie_rgr1_frame — новая страница (см. alloc_device_frame() выше),
    // мапится ТОЛЬКО сюда, usb_driver её не получает вообще.
    if (RPI4_ENABLE_USB) {
        seL4_ARM_Page_Map(pcie_rc_frame, root_vspace, PLAT_PCIE_RC_MISC_ROOT_VADDR, seL4_AllRights, (seL4_ARM_VMAttributes)0);
        seL4_ARM_Page_Map(pcie_rgr1_frame, root_vspace, PLAT_PCIE_RGR1_ROOT_VADDR, seL4_AllRights, (seL4_ARM_VMAttributes)0);
        // issuse.txt №74/в — PCIE_EXT_CFG_DATA (см. platform.h), нужна
        // для переэнумерации PCI config space VL805 (Command/BAR0) после
        // PERST, ДО первого обращения к её BAR-mapped xHCI MMIO.
        seL4_ARM_Page_Map(pcie_cfg_data_frame, root_vspace, PLAT_PCIE_CFG_DATA_ROOT_VADDR, seL4_AllRights, (seL4_ARM_VMAttributes)0);
        // issuse.txt №74/в (38-я hw-попытка) — Type-1 заголовок САМОГО
        // моста: единственное место, откуда можно вернуть ему Memory Space
        // Enable после bridge software init (см. platform.h/
        // PLAT_PCIE_RC_TYPE1_PADDR — полный разбор причины SError).
        seL4_ARM_Page_Map(pcie_rc_type1_frame, root_vspace, PLAT_PCIE_RC_TYPE1_ROOT_VADDR, seL4_AllRights, (seL4_ARM_VMAttributes)0);
    }

    // Мапим таблицу для временного окна IPC. Адрес должен совпадать с global_ipc_temp_vaddr.
    // Одна таблица покрывает 2MB, чего достаточно для 512 процессов.
    seL4_ARM_PageTable_Map(pt_ipc_temp, root_vspace, 0x200800000ULL, seL4_ARM_Default_VMAttributes);

    uart_init((void*)uart_vaddr);
    seL4_DebugPutString((char*)"[BRINGUP] uart_init (mini-UART): done\n");

    // ТАЙМАУТЫ ЗАВЕРШЕНИЯ ТРАНЗАКЦИЙ Root Complex — программируем на
    // холодной загрузке, а не только внутри usbreset.
    //
    // ПОДТВЕРЖДЕНО ЖИВЫМ ЛОГОМ 2026-09-06: сразу после загрузки
    // RC_CONFIG_RETRY_TIMEOUT = 0x00000000, UBUS_TIMEOUT = 0x00080000.
    // Он же после нашего собственного сброса шины: 0x0aba0000 и
    // 0x0b2d0000 — то есть до первого usbreset система жила с НУЛЕВЫМ
    // таймаутом CRS-ретраев и на два порядка более коротким UBUS. U-Boot
    // программирует эти регистры только для BCM2712, наш BCM2711 не
    // трогает вообще, а POR-дефолт оказался именно таким.
    //
    // Почему это ровно наш отказ: незавершающаяся транзакция к
    // устройству, переставшему отвечать, — тот самый механизм, что
    // замораживает ядро на `dsb sy` и убивает всю плату (см.
    // usb_bus_stalled_suspect()). Конечный щедрый таймаут превращает
    // "висит вечно" в "вернулась ошибка", а ошибку драйвер переживает.
    // Значения — те же, что U-Boot ставит для BCM2712.
    //
    // МЕСТО ВЫБРАНО НЕ СЛУЧАЙНО: строго ПОСЛЕ uart_init() и строго ДО
    // спавна usb_driver. Первая версия этой правки стояла выше по main(),
    // рядом с маппингом страницы RC, — и первая же строка лога тут же
    // упала VM fault'ом по адресу 0, потому что UART там ещё не
    // инициализирован (hw 2026-09-06). Сами регистровые записи там
    // работали бы (страница уже отображена), убивала именно печать.
    if (RPI4_ENABLE_USB) {
        pcie_rc_misc_write(PCIE_MISC_RC_CONFIG_RETRY_TIMEOUT_OFFSET, PCIE_MISC_RC_CONFIG_RETRY_TIMEOUT_GENEROUS);
        pcie_rc_misc_write(PCIE_MISC_UBUS_TIMEOUT_OFFSET, PCIE_MISC_UBUS_TIMEOUT_GENEROUS);
        uart_puts("[ROOT] PCIe: таймауты завершения транзакций (CRS/UBUS) запрограммированы на холодной загрузке — POR-дефолт BCM2711 оставлял CRS-таймаут нулевым.\n");
    }

    // issuse.txt №74/в — root сам себя не проходит через spawn_process()
    // (см. её же новый seL4_DebugNameThread(tcb, name) вызов) — именуем
    // явно, тем же приёмом, чтобы в живом JTAG-дампе ksDebugTCBs root
    // тоже был узнаваем по имени, а не только "методом исключения".
    seL4_DebugNameThread(seL4_CapInitThreadTCB, "ROOT");

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
    ram_retype(normal_untyped, seL4_EndpointObject, 0, root_cnode, 0, 0, ep, 1);
    ram_retype(normal_untyped, seL4_EndpointObject, 0, root_cnode, 0, 0, med_ep, 1);

    // --- ПРАВИЛЬНОЕ ВЫДЕЛЕНИЕ SHM (Из обычной ОЗУ, а не из Device Memory) ---
    // ВАЖНО: retype ВСЕХ 6 страниц идёт ОТДЕЛЬНЫМ, ничем не прерываемым
    // циклом, ДО какого-либо маппинга — net_driver.cpp (GENET DMA) и
    // остальные потребители этой SHM считают физический адрес любого
    // смещения как paddr(shm_frames[0]) + смещение, что верно, ТОЛЬКО если
    // все 8 страниц физически идут подряд. Раньше retype и map были в
    // ОДНОМ цикле — маппинг страницы[0] в НИКОГДА не мапленный диапазон
    // (см. комментарий у rootserver_shm_base ниже) triggers on-demand
    // создание PUD/PD/PT (ещё 3 объекта из ТОГО ЖЕ normal_untyped) МЕЖДУ
    // retype'ом страницы[0] и страницы[1] — на живом железе это давало
    // paddr(frame[0])=0x40005000, paddr(frame[1])=0x40009000 (дыра 0x4000
    // вместо 0x1000!), из-за чего GENET писал принятые кадры в
    // "дырочный" (чужой/неинициализированный) физический адрес вместо
    // настоящей 2-й/3-й страницы SHM — net_driver читал свою страницу и
    // видел одни нули (см. ROADMAP.md 4.5, живой баг ethertype=0).
    for (int i = 0; i < SHM_TOTAL_PAGES; i++) {
        shm_frames[i] = alloc.alloc_slot();
        seL4_Error err = ram_retype(normal_untyped, seL4_ARM_SmallPageObject, 0,
                                             root_cnode, 0, 0, shm_frames[i], 1);
        if (err != seL4_NoError) {
            uart_puts("[ROOT] FATAL: Failed to allocate normal RAM for SHM!\n");
            while(1);
        }
    }
    for (int i = 0; i < SHM_TOTAL_PAGES; i++) {
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
            if (ram_retype(normal_untyped, seL4_ARM_PageUpperDirectoryObject, 0, root_cnode, 0, 0, shm_pud, 1) == seL4_NoError) {
                seL4_ARM_PageUpperDirectory_Map(shm_pud, root_vspace, shm_vaddr, seL4_ARM_Default_VMAttributes);
            }
            seL4_CPtr shm_pd = alloc.alloc_slot();
            if (ram_retype(normal_untyped, seL4_ARM_PageDirectoryObject, 0, root_cnode, 0, 0, shm_pd, 1) == seL4_NoError) {
                seL4_ARM_PageDirectory_Map(shm_pd, root_vspace, shm_vaddr, seL4_ARM_Default_VMAttributes);
            }
            seL4_CPtr shm_pt = alloc.alloc_slot();
            if (ram_retype(normal_untyped, seL4_ARM_PageTableObject, 0, root_cnode, 0, 0, shm_pt, 1) == seL4_NoError) {
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
    ram_retype(normal_untyped, seL4_EndpointObject, 0, root_cnode, 0, 0, console_ep, 1);
    ram_retype(normal_untyped, seL4_EndpointObject, 0, root_cnode, 0, 0, timer_ep, 1);
    ram_retype(normal_untyped, seL4_EndpointObject, 0, root_cnode, 0, 0, blk_ep, 1);
    ram_retype(normal_untyped, seL4_EndpointObject, 0, root_cnode, 0, 0, net_cmd_ep, 1);
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
    seL4_CPtr wifi_wake_ntfn = 0;
    seL4_CPtr wifi_wake_heartbeat_badged = 0;
    seL4_CPtr wifi_wake_tx_ready_badged = 0;
    if (RPI4_ENABLE_WIFI) {
        wifi_cmd_ep = alloc.alloc_slot();
        wifi_cmd_recv_ep = alloc.alloc_slot();
        wifi_cmd_send_ep = alloc.alloc_slot();
        ram_retype(normal_untyped, seL4_EndpointObject, 0, root_cnode, 0, 0, wifi_cmd_ep, 1);
        seL4_CNode_Copy(root_cnode, wifi_cmd_recv_ep, seL4_WordBits,
                        root_cnode, wifi_cmd_ep, seL4_WordBits, seL4_CanRead);
        seL4_CNode_Copy(root_cnode, wifi_cmd_send_ep, seL4_WordBits,
                        root_cnode, wifi_cmd_ep, seL4_WordBits, seL4_CapRights_new(0, 1, 0, 1)); // Write + Grant

        // Фаза 4.5 (Wi-Fi data-plane) — heartbeat/wake-нотификация самого
        // wifi_driver'а: TCB-bind через обычный irq_ntfn-параметр spawn_process()
        // (тот же механизм, что уже используется для net_event_ntfn у
        // net_driver — см. ниже). Два бейджа с ОДНОГО объекта, seL4 сам OR'ит
        // непотреблённые сигналы, поэтому wifi_driver одним Recv видит и
        // heartbeat (от timer_driver), и "есть TX-кадр" (от net_driver).
        // Создаётся один раз при загрузке и переживает рестарты wifi_driver
        // (тот же принцип постоянства, что у wifi_cmd_recv_ep выше) — сам
        // TCB-bind просто накатывается заново при каждом (ре)спавне.
        wifi_wake_ntfn = alloc.alloc_slot();
        wifi_wake_heartbeat_badged = alloc.alloc_slot();
        wifi_wake_tx_ready_badged = alloc.alloc_slot();
        ram_retype(normal_untyped, seL4_NotificationObject, 0, root_cnode, 0, 0, wifi_wake_ntfn, 1);
        seL4_CNode_Mint(root_cnode, wifi_wake_heartbeat_badged, seL4_WordBits, root_cnode, wifi_wake_ntfn, seL4_WordBits, seL4_AllRights, WIFI_EVENT_HEARTBEAT);
        seL4_CNode_Mint(root_cnode, wifi_wake_tx_ready_badged, seL4_WordBits, root_cnode, wifi_wake_ntfn, seL4_WordBits, seL4_AllRights, WIFI_EVENT_TX_READY);
    }

    // Фаза 14 (USB, xHCI) — командный endpoint (root-опосредованный доступ,
    // см. SYS_USB_LIST: root сам держит usb_cmd_ep с полными правами и
    // вызывает его напрямую, ни одному другому процессу send-копия не
    // нужна — иначе, в отличие от net/wifi, никто, кроме root, с
    // usb_driver не разговаривает) + IRQ-нотификация (RPI4_XHCI_IRQ —
    // собственная линия, один источник событий, тот же паттерн, что у
    // uart_ntfn/uart_irq_handler чуть ниже, но не разделяемая ни с кем).
    seL4_CPtr usb_cmd_ep = 0;
    // usb_cmd_recv_ep теперь file-scope global (см. комментарий у её
    // объявления) — не передекларируем здесь, просто присваиваем ниже.
    seL4_CPtr usb_irq_ntfn = 0;
    seL4_CPtr usb_irq_handler = 0;
    // Milestone 11 (доп., по запросу пользователя) — badged-копия
    // usb_irq_ntfn (badge USB_EVENT_HEARTBEAT), отдаётся timer_driver'у
    // (см. spawn_process(usb_heartbeat_ntfn_param) ниже) — тот же приём,
    // что wifi_wake_heartbeat_badged/blk_heartbeat_badged: несколько
    // бейджей с ОДНОГО объекта, seL4 сам OR'ит непотреблённые сигналы, так
    // что usb_driver одним Recv видит и реальный XHCI IRQ, и heartbeat.
    seL4_CPtr badged_usb_heartbeat_ntfn = 0;
    if (RPI4_ENABLE_USB) {
        usb_cmd_ep = alloc.alloc_slot();
        usb_cmd_recv_ep = alloc.alloc_slot();
        ram_retype(normal_untyped, seL4_EndpointObject, 0, root_cnode, 0, 0, usb_cmd_ep, 1);
        seL4_CNode_Copy(root_cnode, usb_cmd_recv_ep, seL4_WordBits,
                        root_cnode, usb_cmd_ep, seL4_WordBits, seL4_CanRead);
        g_usb_cmd_ep = usb_cmd_ep; // см. usb_bus_stalled_suspect()/generic_recover_process()

        seL4_CPtr badged_usb_irq_ntfn = alloc.alloc_slot();
        usb_irq_ntfn = alloc.alloc_slot();
        usb_irq_handler = alloc.alloc_slot();
        ram_retype(normal_untyped, seL4_NotificationObject, 0, root_cnode, 0, 0, usb_irq_ntfn, 1);
        seL4_CNode_Mint(root_cnode, badged_usb_irq_ntfn, seL4_WordBits, root_cnode, usb_irq_ntfn, seL4_WordBits, seL4_AllRights, USB_EVENT_XHCI_IRQ);
        // issuse.txt №74/в — ВРЕМЕННО, ТОЛЬКО ДЛЯ JTAG-ДИАГНОСТИКИ: до
        // этой правки IRQ208 (xHCI) ВСЕГДА таргетился GIC'ом на CPU0
        // (см. platform.h/USB_DRIVER_DEFAULT_CORE) — обычный
        // seL4_IRQControl_Get() core-обливиозен (жёстко кодирует ядро 0
        // в кодировку irq_t, kernel/src/object/interrupt.c) и не имеет
        // способа задать другое ядро. seL4_IRQControl_GetTriggerCore()
        // (ARM SMP-специфичный вариант, kernel/src/arch/arm/object/
        // interrupt.c, ARMIRQIssueIRQHandlerTriggerCore) добавляет
        // ПОСЛЕДНИЙ параметр target — доставляет прерывание физически
        // на нужное ядро (setIRQTarget() -> GICD_ITARGETSR). Параметр
        // trigger (здесь 0) НА ЭТОЙ ПЛАТФОРМЕ безопасно игнорируется
        // ядром (HAVE_SET_TRIGGER не сконфигурирован для bcm2711,
        // Arch_invokeIRQControl() пропускает setIRQTrigger() целиком) —
        // меняется ТОЛЬКО таргетинг, не тип триггера.
        check_err(seL4_IRQControl_GetTriggerCore(seL4_CapIRQControl, PLAT_XHCI_IRQ, 0, root_cnode, usb_irq_handler, seL4_WordBits, USB_DRIVER_DEFAULT_CORE), "IRQControl_GetTriggerCore(XHCI)");
        check_err(seL4_IRQHandler_SetNotification(usb_irq_handler, badged_usb_irq_ntfn), "IRQHandler_SetNotification(xhci)");
        check_err(seL4_IRQHandler_Ack(usb_irq_handler), "IRQHandler_Ack(xhci, initial)");

        badged_usb_heartbeat_ntfn = alloc.alloc_slot();
        seL4_CNode_Mint(root_cnode, badged_usb_heartbeat_ntfn, seL4_WordBits, root_cnode, usb_irq_ntfn, seL4_WordBits, seL4_AllRights, USB_EVENT_HEARTBEAT);
    }

    // Таймер (ARM generic timer) не MMIO-устройство и не генерирует IRQ,
    // доступный из EL0 на этой сборке ядра (см. hw_timer.cpp) — никакой
    // IRQ-обвязки timer_driver'у больше не нужно, в отличие от PL031.

    seL4_CPtr uart_ntfn = alloc.alloc_slot();
    seL4_CPtr badged_uart_ntfn = alloc.alloc_slot(); 
    seL4_CPtr uart_irq_handler = alloc.alloc_slot();
    ram_retype(normal_untyped, seL4_NotificationObject, 0, root_cnode, 0, 0, uart_ntfn, 1);
    seL4_CNode_Mint(root_cnode, badged_uart_ntfn, seL4_WordBits, root_cnode, uart_ntfn, seL4_WordBits, seL4_AllRights, UART_KBD_IRQ_BADGE);
    seL4_IRQControl_Get(seL4_CapIRQControl, PLAT_UART_IRQ, root_cnode, uart_irq_handler, seL4_WordBits);
    seL4_IRQHandler_SetNotification(uart_irq_handler, badged_uart_ntfn); 
    uart_enable_interrupts();
    seL4_IRQHandler_Ack(uart_irq_handler);

    // issuse.txt №74, часть "б" — общая страница состояния драйверов
    // (PARKED/BUSY). Создаётся ДО первого spawn_process(), потому что та
    // мапит её в каждый драйвер (см. g_driver_state_frame). Обычная
    // RAM-страница из normal_untyped, но мапится НЕКЭШИРУЕМО с обеих
    // сторон: её читает root с ядра 0, а пишут драйверы с других ядер, и
    // разбираться с кэш-обслуживанием на каждый опрос незачем — данных
    // одно слово на драйвер.
    {
        g_driver_state_frame = alloc.alloc_slot();
        if (g_driver_state_frame != 0 &&
            ram_retype(normal_untyped, seL4_ARM_SmallPageObject, 0, root_cnode, 0, 0, g_driver_state_frame, 1) == seL4_NoError &&
            seL4_ARM_Page_Map(g_driver_state_frame, root_vspace, PLAT_DRIVER_STATE_ROOT_VADDR,
                              seL4_AllRights, (seL4_ARM_VMAttributes)0) == seL4_NoError) {
            g_driver_state_page = (volatile seL4_Word *)PLAT_DRIVER_STATE_ROOT_VADDR;
            for (int i = 0; i < 4096 / (int)sizeof(seL4_Word); i++) g_driver_state_page[i] = DRIVER_STATE_BUSY;
        } else {
            // Не смертельно: без страницы механизм просто вырождается в
            // прежнее поведение — driver_state_read() всегда возвращает
            // BUSY, значит prepare_driver_for_suspend() откажет любому
            // драйверу на не-нулевом ядре, а восстановление на ядре 0
            // (как было до этой работы) продолжит работать как раньше.
            uart_puts("[ROOT] ВНИМАНИЕ: не удалось создать общую страницу состояния драйверов — перенос между ядрами останется без защиты, Suspend вне ядра 0 будет отклоняться (issuse.txt №74/б).\n");
            g_driver_state_frame = 0;
            g_driver_state_page = nullptr;
        }
    }

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
    seL4_CPtr net_wifi_rx_badged = 0;
    if (RPI4_ENABLE_NET) {
        ram_retype(normal_untyped, seL4_NotificationObject, 0, root_cnode, 0, 0, net_event_ntfn, 1);
        seL4_CNode_Mint(root_cnode, badged_genet_rx_ntfn, seL4_WordBits, root_cnode, net_event_ntfn, seL4_WordBits, seL4_AllRights, NET_EVENT_GENET_RX);
        seL4_CNode_Mint(root_cnode, badged_net_heartbeat_ntfn, seL4_WordBits, root_cnode, net_event_ntfn, seL4_WordBits, seL4_AllRights, NET_EVENT_HEARTBEAT);
        check_err(seL4_IRQControl_Get(seL4_CapIRQControl, RPI4_GENET_IRQ_A, root_cnode, genet_irq_handler, seL4_WordBits), "IRQControl_Get(GENET_IRQ_A)");
        check_err(seL4_IRQHandler_SetNotification(genet_irq_handler, badged_genet_rx_ntfn), "IRQHandler_SetNotification(genet)");
        check_err(seL4_IRQHandler_Ack(genet_irq_handler), "IRQHandler_Ack(genet, initial)");

        // Фаза 4.5 (Wi-Fi data-plane) — третий бейдж ТОГО ЖЕ net_event_ntfn:
        // wifi_driver сигналит им net_driver, когда кладёт кадр в RX-mailbox.
        // Сам объект/bind у net_driver не меняются — просто ещё один badge,
        // который может прийти на тот же net_driver'а Recv (см. common.h).
        if (RPI4_ENABLE_WIFI) {
            net_wifi_rx_badged = alloc.alloc_slot();
            seL4_CNode_Mint(root_cnode, net_wifi_rx_badged, seL4_WordBits, root_cnode, net_event_ntfn, seL4_WordBits, seL4_AllRights, NET_EVENT_WIFI_RX);
        }
    }

    // Исправление живого зависания (см. situation.txt): blk_driver блокируется
    // на g_emmc_irq_ntfn (= это самое blk_irq_ntfn) без таймаута — если карта
    // пропустит ожидаемое IRQ, зависает навсегда, а вместе с ним и ROOT (см.
    // load_elf_from_disk() — синхронный вызов blk_ep прямо из обработчика
    // root'а). Фикс — тот же heartbeat-паттерн, что у net_driver/wifi_driver:
    // timer_driver периодически шлёт доп. badged-копию ЭТОГО ЖЕ объекта, так
    // что seL4_Wait в blk_driver гарантированно просыпается каждые ~100мс
    // независимо от реального железного IRQ. Создаём blk_irq_ntfn ЗДЕСЬ (а не
    // в его обычном месте ниже, вместе с mmc_shared_irq_ntfn/wifi_irq_ntfn) —
    // ПО ТОЙ ЖЕ причине, что и net_event_ntfn выше: объект должен существовать
    // ДО спавна timer_driver, чтобы тот получил badged-копию при своём спавне.
    seL4_CPtr blk_irq_ntfn = 0;
    seL4_CPtr blk_heartbeat_badged = 0;
    if (RPI4_ENABLE_BLK) {
        blk_irq_ntfn = alloc.alloc_slot();
        blk_heartbeat_badged = alloc.alloc_slot();
        check_err(ram_retype(normal_untyped, seL4_NotificationObject, 0, root_cnode, 0, 0, blk_irq_ntfn, 1), "Retype blk_irq_ntfn");
        check_err(seL4_CNode_Mint(root_cnode, blk_heartbeat_badged, seL4_WordBits, root_cnode, blk_irq_ntfn, seL4_WordBits, seL4_AllRights, BLK_HEARTBEAT_BADGE), "Mint blk_heartbeat_badged");
    }

    // Фаза 3b плана "Сигналы драйверам" — см. common.h/BLK_LIVENESS_TICK_BADGE:
    // ПОЛНОСТЬЮ ОТДЕЛЬНЫЙ от blk_irq_ntfn объект (сознательно НЕ переиспользуем
    // его — тот разделяет общую линию EMMC2/Wi-Fi SDIO, см. комментарий у
    // константы). TCB-bind на blk_driver (через пустовавший irq_ntfn-параметр
    // его spawn_process(), см. ниже) + одна badged-копия для timer_driver, той
    // же схемой, что и остальные heartbeat-капы выше. Создаётся здесь (до
    // спавна timer_driver) по той же причине, что net_event_ntfn/blk_irq_ntfn.
    seL4_CPtr blk_liveness_tick_ntfn = 0;
    seL4_CPtr blk_liveness_tick_badged = 0;
    if (RPI4_ENABLE_BLK) {
        blk_liveness_tick_ntfn = alloc.alloc_slot();
        blk_liveness_tick_badged = alloc.alloc_slot();
        check_err(ram_retype(normal_untyped, seL4_NotificationObject, 0, root_cnode, 0, 0, blk_liveness_tick_ntfn, 1), "Retype blk_liveness_tick_ntfn");
        check_err(seL4_CNode_Mint(root_cnode, blk_liveness_tick_badged, seL4_WordBits, root_cnode, blk_liveness_tick_ntfn, seL4_WordBits, seL4_AllRights, BLK_LIVENESS_TICK_BADGE), "Mint blk_liveness_tick_badged");
    }

    // Собственный тик heartbeat-watchdog'а (см. common.h/
    // ROOT_WATCHDOG_TICK_BADGE — там же разбор, почему прежняя схема
    // "скан запускается от сигналов самих наблюдаемых драйверов" не
    // работает ровно в тот момент, когда нужна). Сам объект нотификации
    // создаётся ЗДЕСЬ, до спавна timer_driver, — именно порядок
    // инициализации и был причиной, по которой этот тик когда-то не
    // завели; остальное (IRQ-обвязка SDIO, TCB-bind к root'у, badged-копии
    // liveness) по-прежнему настраивается ниже, над этим же объектом.
    seL4_CPtr mmc_shared_irq_ntfn = 0;
    if (RPI4_ENABLE_BLK) {
        mmc_shared_irq_ntfn = alloc.alloc_slot();
        check_err(ram_retype(normal_untyped, seL4_NotificationObject, 0, root_cnode, 0, 0, mmc_shared_irq_ntfn, 1), "Retype mmc_shared_irq_ntfn");
        g_root_watchdog_tick_badged = alloc.alloc_slot();
        check_err(seL4_CNode_Mint(root_cnode, g_root_watchdog_tick_badged, seL4_WordBits, root_cnode, mmc_shared_irq_ntfn, seL4_WordBits, seL4_AllRights, ROOT_WATCHDOG_TICK_BADGE), "Mint root_watchdog_tick_badged");
    }

    // Фаза 6 (SMP, см. ROADMAP.md/common.h): общий мьютекс на нотификации
    // для VFS-прокси staging области в SHM (офсет 4096, см. shell.cpp/
    // vfs_lock, net_driver.cpp/net_vfs_lock, wifi_driver.cpp/wifi_vfs_lock).
    // Заменяет старые non-atomic busy-spin локи — те были безопасны только
    // пока не было настоящего межъядерного параллелизма (переключение
    // контекста — исключительно на syscall/yield границе). Один и тот же
    // объект без бейджей у всех троих (различать источник не нужно, это
    // чистый binary semaphore): seL4_Signal сразу после создания сеет
    // "разблокировано", дальше lock()=seL4_Wait, unlock()=seL4_Signal —
    // состояние живёт в ядре, а не в Device-памяти SHM, поэтому вопрос
    // exclusive-monitor инструкций на Device-памяти (см. net_driver.cpp,
    // уже роняли этим весь kernel на живом железе) вообще не встаёт.
    seL4_CPtr vfs_mutex_ntfn = 0;
    if (RPI4_ENABLE_BLK) {
        vfs_mutex_ntfn = alloc.alloc_slot();
        check_err(ram_retype(normal_untyped, seL4_NotificationObject, 0, root_cnode, 0, 0, vfs_mutex_ntfn, 1), "Retype vfs_mutex_ntfn");
        seL4_Signal(vfs_mutex_ntfn);
        g_vfs_mutex_ntfn_cap = vfs_mutex_ntfn; // issuse.txt №70 — см. generic_recover_process()
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
        ram_retype(normal_untyped, seL4_NotificationObject, 0, root_cnode, 0, 0, timer_ntfn, 1);
        seL4_CNode_Mint(root_cnode, badged_timer_ntfn, seL4_WordBits, root_cnode, timer_ntfn, seL4_WordBits, seL4_AllRights, TIMER_IRQ_BADGE);
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
                      nullptr, 0, 0, 0, 0, mbox_regs_frame, mbox_buf_frame, mbox_buf_paddr, badged_net_heartbeat_ntfn,
                      0, 0, wifi_wake_heartbeat_badged, 0, 0, blk_heartbeat_badged,
                      0, // mmc_irq_handler_param
                      0, 0, // blk_dma_frame2_param, blk_dma_paddr2_param
                      0, // vfs_mutex_ntfn_param (timer_driver не VFS-клиент)
                      nullptr, -1, // cwd_payload, stdout_pipe_id
                      0, nullptr, 0, 0, // usb_cmd_recv_ep_param .. pcie_err_frame_param (timer_driver не usb_driver)
                      0, // usb_storage_ep_param (timer_driver не VFS-клиент)
                      // Milestone 11 (доп.): heartbeat-капа usb_driver'а —
                      // timer_driver сигналит её на том же тике, что и
                      // net/wifi/blk (см. timer_driver.cpp).
                      badged_usb_heartbeat_ntfn,
                      0, // liveness_ntfn_param (Фаза 3b, timer_driver сам не мониторится)
                      // Фаза 3b: тик для blk_driver — timer_driver сигналит
                      // её на том же общем heartbeat-тике (см. timer_driver.cpp).
                      blk_liveness_tick_badged,
                      0, // gpio_frame_param (timer_driver не GPIO)
                      // issuse.txt №73/№75 — аппаратный watchdog, кормит
                      // его на каждом heartbeat-тике (см. timer_driver.cpp).
                      pm_wdog_frame) < 0) {
        uart_puts("PANIC: Timer Driver failed to load!\n"); while(1);
    }
    }

    // Общий IRQ 158 (Фаза 4.5, см. IRQ_MMC_SHARED_BADGE/common.h) слушает
    // САМ root, а не какой-то конкретный драйвер — только один процесс
    // вообще может держать IRQHandler-капу на этот номер. ОТКАЧЕНО (см.
    // situation.txt): пробовали TCB-bind напрямую к blk_driver, чтобы
    // обойти задержку root-инициированных чтений — вызвало КАТАСТРОФИЧЕСКИЙ
    // регресс (чтение WiFi-прошивки: 800мс -> 27с), потому что blk_driver
    // копит пендинг-сигналы от КАЖДОГО реального завершения EMMC-команды,
    // пока сам занят обработкой (не в Recv/Wait) — а следующий
    // seL4_Recv(my_ep,...) в его главном цикле перехватывается этим
    // накопленным сигналом ВМЕСТО реального клиентского запроса
    // (wifi_driver/shell), снова и снова. Возврат к исходной схеме: root
    // релеит обоим процессам, каждый сам проверяет свой статусный регистр.
    seL4_CPtr wifi_irq_ntfn = 0;
    seL4_CPtr mmc_shared_irq_handler = 0;
    // Фаза 3b плана "Сигналы драйверам" — badged-копии ЭТОГО ЖЕ
    // mmc_shared_irq_ntfn (см. common.h/DRIVER_LIVENESS_*_BADGE), которыми
    // blk/net/wifi/usb сами сигналят root'у "я жив". Объявлены здесь (не
    // внутри if-блока ниже), потому что нужны позже, в отдельных spawn_
    // process()-вызовах blk/net/usb и в root_start_wifi_driver().
    seL4_CPtr blk_liveness_badged = 0;
    seL4_CPtr net_liveness_badged = 0;
    seL4_CPtr wifi_liveness_badged = 0;
    seL4_CPtr usb_liveness_badged = 0;
    if (RPI4_ENABLE_BLK) {
        // Сам объект уже создан ВЫШЕ, до спавна timer_driver (нужен был
        // раньше, чтобы выдать ему тик watchdog'а) — здесь только обвязка.
        seL4_CPtr mmc_shared_irq_badged = alloc.alloc_slot();
        mmc_shared_irq_handler = alloc.alloc_slot();
        seL4_CNode_Mint(root_cnode, mmc_shared_irq_badged, seL4_WordBits, root_cnode, mmc_shared_irq_ntfn, seL4_WordBits, seL4_AllRights, IRQ_MMC_SHARED_BADGE);
        seL4_IRQControl_Get(seL4_CapIRQControl, RPI4_WIFI_SDIO_IRQ, root_cnode, mmc_shared_irq_handler, seL4_WordBits);
        seL4_IRQHandler_SetNotification(mmc_shared_irq_handler, mmc_shared_irq_badged);
        check_err(seL4_TCB_BindNotification(seL4_CapInitThreadTCB, mmc_shared_irq_ntfn), "Bind shared MMC IRQ to root");
        seL4_IRQHandler_Ack(mmc_shared_irq_handler);

        // Фаза 3b — см. объявления blk_liveness_badged и др. выше. Root
        // держит ЕДИНСТВЕННЫЙ TCB-bind на mmc_shared_irq_ntfn (см. факт в
        // плане) — новые бейджи ТОЛЬКО как дополнительные badged-копии ТОГО
        // ЖЕ объекта, не новый bind. Минтим все 4 сразу здесь, пока объект
        // ещё в области видимости — используются позже при спавне
        // соответствующих драйверов и в root_start_wifi_driver().
        blk_liveness_badged = alloc.alloc_slot();
        net_liveness_badged = alloc.alloc_slot();
        wifi_liveness_badged = alloc.alloc_slot();
        usb_liveness_badged = alloc.alloc_slot();
        seL4_CNode_Mint(root_cnode, blk_liveness_badged, seL4_WordBits, root_cnode, mmc_shared_irq_ntfn, seL4_WordBits, seL4_AllRights, DRIVER_LIVENESS_BLK_BADGE);
        seL4_CNode_Mint(root_cnode, net_liveness_badged, seL4_WordBits, root_cnode, mmc_shared_irq_ntfn, seL4_WordBits, seL4_AllRights, DRIVER_LIVENESS_NET_BADGE);
        seL4_CNode_Mint(root_cnode, wifi_liveness_badged, seL4_WordBits, root_cnode, mmc_shared_irq_ntfn, seL4_WordBits, seL4_AllRights, DRIVER_LIVENESS_WIFI_BADGE);
        seL4_CNode_Mint(root_cnode, usb_liveness_badged, seL4_WordBits, root_cnode, mmc_shared_irq_ntfn, seL4_WordBits, seL4_AllRights, DRIVER_LIVENESS_USB_BADGE);

        // blk_irq_ntfn (нотификация, которой root будит именно blk_driver на
        // событие с общей линии) теперь создаётся ВЫШЕ, до спавна timer_driver
        // — см. комментарий у BLK_HEARTBEAT_BADGE.

        // Та же идея, но для wifi_driver (Фаза 4.5, продолжение) — отдельный
        // объект, а не badged-копия blk_irq_ntfn, потому что wifi_driver
        // спавнится/убивается динамически (SYS_START_WIFI/SYS_STOP_WIFI,
        // см. ROADMAP.md 4.4.1) — capability передаётся заново при каждом
        // "wifi start", сам notification-объект переживает рестарты
        // процесса без пересоздания.
        wifi_irq_ntfn = alloc.alloc_slot();
        ram_retype(normal_untyped, seL4_NotificationObject, 0, root_cnode, 0, 0, wifi_irq_ntfn, 1);
    }

    if (RPI4_ENABLE_BLK) {
    // Запускаем Драйвер Диска и ФС (is_driver = 3). blk_irq_ntfn передаётся
    // ТРЕТЬИМ параметром из пары irq_ntfn/irq_handler (не первым!) — это
    // НЕ TCB-bind (blk_driver не читает свой my_ep для IRQ, см. комментарий
    // у g_emmc_irq_ntfn в blk_driver.cpp), а обычное копирование capability
    // на нотификацию в cspace процесса (тот же spawn_process-механизм, что
    // копирует IRQHandler для UART — семантика объекта ему не важна).
    if (spawn_process("blk_driver", nullptr, 0, ep, med_ep, alloc, root_cnode, root_vspace, normal_untyped, shm_frames[0],
                      3, console_ep, timer_ep, blk_ep, console_ep, console_ep, console_ep,
                      // Фаза 3b: irq_ntfn-параметр у blk_driver раньше был 0
                      // (без TCB-bind) — теперь TCB-bind на выделенный тик
                      // liveness-watchdog'а (см. common.h/BLK_LIVENESS_TICK_BADGE).
                      blk_liveness_tick_ntfn, blk_irq_ntfn, emmc_frame,
                      nullptr, 0, 0, 0, 0, 0, 0, 0, 0, blk_dma_frame, blk_dma_paddr,
                      0, 0, 0, 0, mmc_shared_irq_handler, blk_dma_frame2, blk_dma_paddr2,
                      0, nullptr, -1, 0, nullptr, 0, 0, 0, 0, // vfs_mutex_ntfn_param .. usb_heartbeat_ntfn_param (не для blk_driver)
                      blk_liveness_badged, // Фаза 3b: капа-"я жив" (param 48: liveness_ntfn_param)
                      0, // blk_liveness_tick_param (param 49) — НЕ для самого blk_driver, читает только timer_driver
                      gpio_frame) < 0) { // Начало GPIO-драйвера (см. h/gpio.h) — ACT LED при обращении к SD (param 50: gpio_frame_param)
        uart_puts("PANIC: Block Driver failed to load!\n"); while(1);
    }
    }

    if (RPI4_ENABLE_NET) {
    if (spawn_process("net_driver", nullptr, 0, ep, med_ep, alloc, root_cnode, root_vspace, normal_untyped, shm_frames[0],
                      4, console_ep, timer_ep, blk_ep, console_ep, console_ep, console_ep, net_event_ntfn, genet_irq_handler, genet_frames[0], nullptr,
                      net_cmd_recv_ep, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, wifi_wake_tx_ready_badged,
                      0, 0, 0, 0, vfs_mutex_ntfn,
                      nullptr, -1, 0, nullptr, 0, 0, 0, 0, // cwd_payload .. usb_heartbeat_ntfn_param (не для net_driver)
                      net_liveness_badged) < 0) { // Фаза 3b: капа-"я жив"
        uart_puts("PANIC: Net Driver failed to load!\n"); while(1);
    }
    }

    if (RPI4_ENABLE_USB) {
    // usb_driver (is_driver = 6, Фаза 14) — компилируется в CPIO и всегда
    // запущен (нет собственного lifecycle start/stop, в отличие от wifi),
    // но НЕ входит в driver_ready[]/all_drivers_ready() (см. выше) —
    // опционален по определению (ROADMAP.md Фаза 8). blk_ep=0 — usb_driver
    // не работает с диском (Phase 14 — только bring-up + перечисление, без
    // класс-драйверов, см. ROADMAP.md).
    if (spawn_process("usb_driver", nullptr, 0, ep, med_ep, alloc, root_cnode, root_vspace, normal_untyped, shm_frames[0],
                      6, console_ep, timer_ep, 0, console_ep, console_ep, console_ep, usb_irq_ntfn, usb_irq_handler, usb_xhci_frames[0],
                      nullptr, // args_payload
                      0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, // net_cmd_recv_ep .. vfs_mutex_ntfn_param (18 params)
                      nullptr, // cwd_payload
                      -1, // stdout_pipe_id
                      usb_cmd_recv_ep, &usb_dma, pcie_rc_frame, pcie_err_frame,
                      0, // usb_storage_ep_param (не для usb_driver самого)
                      0, // usb_heartbeat_ntfn_param (не для usb_driver самого)
                      usb_liveness_badged, // Фаза 3b: капа-"я жив"
                      0, // blk_liveness_tick_param (не для usb_driver)
                      0, // gpio_frame_param (не для usb_driver)
                      0, // pm_wdog_frame_param (не для usb_driver)
                      pcie_cfg_data_frame) < 0) { // issuse.txt №74/в, 37-я hw-попытка (param 54: pcie_cfg_data_frame_param)
        uart_puts("PANIC: USB Driver failed to load!\n"); while(1);
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
                      0, net_cmd_send_ep, 0, wifi_cmd_send_ep, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                      0, 0, 0, 0, vfs_mutex_ntfn,
                      nullptr, // cwd_payload
                      -1, // stdout_pipe_id
                      0, nullptr, 0, 0, // usb_cmd_recv_ep_param .. pcie_err_frame_param (shell не usb_driver)
                      usb_cmd_ep) < 0) { // Milestone 9: shell получает VFS-доступ к /mnt/usb0
        uart_puts("PANIC: Shell failed to load!\n"); while(1);
    }

    uart_puts("[ROOT] All sandboxes spawned. Serving IPC.\n");

    // --- ЕДИНЫЙ ЦИКЛ ЯДРА ---
    while (1) {
        seL4_Word sender_badge = 0;
        // Ожидаем прерывание, сообщение IPC или fault
        seL4_MessageInfo_t recv_info = seL4_Recv(ep, &sender_badge);

        // Фаза 3a плана "Сигналы драйверам" — было "== IRQ_MMC_SHARED_BADGE"
        // (готовим почву под Фазу 3b: будущие watchdog-бейджи на ЭТОМ ЖЕ
        // mmc_shared_irq_ntfn, ≥0x10000, будут ОРиться с этим при
        // одновременной сигнализации — голое "==" перестало бы видеть MMC-
        // часть комбинированного бейджа). ВАЖНО, найдено ДО прошивки при
        // проверке плана: IRQ_MMC_SHARED_BADGE=2000 (0x7D0) — НЕ одиночный
        // бит, а {4,6,7,8,9,10}, и sender_badge здесь делят PID клиентов
        // (1-255) и бейджи пайпов (PIPE_BASE_BADGE..+MAX_PIPES=1000-1015) —
        // оба диапазона побитово ПЕРЕСЕКАЮТСЯ с 2000 (напр. PID=16: "16 & 2000"
        // = 16 != 0). Голое "&" без нижней границы перехватывало бы такие
        // вызовы клиентов как MMC/Wi-Fi SDIO IRQ — зависший навсегда без
        // ответа звонок, тот же класс бага, что уже вызывал катастрофический
        // регресс (см. комментарий ниже). PID (макс 255) и pipe-диапазон
        // (1000-1015) гарантированно ниже 2000 — этой нижней границы
        // достаточно, чтобы отличить их ото всех текущих и будущих (Фаза 3b)
        // нотификационных бейджей mmc_shared_irq_ntfn. Для ЛЮБОГО сегодня
        // возможного sender_badge поведение идентично старому "==".
        if (sender_badge >= IRQ_MMC_SHARED_BADGE && (sender_badge & IRQ_MMC_SHARED_BADGE)) {
            // Общий IRQ EMMC2/Wi-Fi SDIO (см. common.h) — root не читает
            // регистры контроллеров сам, просто будит blk_driver; тот
            // проверяет свой статусный бит и, если это не он, просто снова
            // засыпает на своей нотификации (см. blk_driver.cpp). ОТКАЧЕНА
            // попытка TCB-bind напрямую к blk_driver (см. situation.txt) —
            // вызвала катастрофический регресс (blk_driver копит пендинг-
            // сигналы от каждого реального завершения EMMC-команды, пока
            // сам занят, и следующий Recv в его главном цикле перехватывается
            // этим накопленным сигналом вместо реального клиентского
            // запроса). Возврат к исходной, проверенной схеме.
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

        // Фаза 3b плана "Сигналы драйверам" — Направление Б: blk/net/wifi/
        // usb сами сигналят root'у "я жив" (см. common.h/DRIVER_LIVENESS_*_
        // BADGE, все ≥0x10000 — тот же guard "sender_badge >= порог", что
        // уже применён у IRQ_MMC_SHARED_BADGE выше в Фазе 3a, отличает их от
        // PID (1-255) и pipe-бейджей (1000-1015)). Отдельный
        // ROOT_WATCHDOG_TICK_BADGE от timer_driver сознательно НЕ заведён —
        // timer_driver спавнится ДО mmc_shared_irq_ntfn (см. факты плана),
        // вместо этого сам поток liveness-сигналов от МОНИТОРИМЫХ драйверов
        // служит достаточно частым regular tick для скана таймаутов
        // (~20мс, пока жив хотя бы один из четырёх) — единственный пробел:
        // если ВСЕ 4 зависнут в один и тот же момент, скан не запустится, но
        // это уже за пределами того, что heartbeat-автовосстановление
        // способно исправить в принципе (ручной driver/recover не зависят
        // от этого скана и остаются доступны всегда).
        if (sender_badge >= DRIVER_LIVENESS_BLK_BADGE) {
            seL4_SetMR(0, 4); // SYS_GET_UPTIME
            seL4_Call(timer_ep, seL4_MessageInfo_new(0, 0, 0, 1));
            seL4_Word watchdog_now_ms = seL4_GetMR(0);
            if (sender_badge & DRIVER_LIVENESS_BLK_BADGE)  g_driver_last_seen_ms[3] = watchdog_now_ms;
            if (sender_badge & DRIVER_LIVENESS_NET_BADGE)  g_driver_last_seen_ms[4] = watchdog_now_ms;
            if (sender_badge & DRIVER_LIVENESS_WIFI_BADGE) g_driver_last_seen_ms[5] = watchdog_now_ms;
            if (sender_badge & DRIVER_LIVENESS_USB_BADGE) {
                g_driver_last_seen_ms[6] = watchdog_now_ms;
                g_usb_reset_attempts = 0; // драйвер жив — счётчик подряд идущих сбросов шины обнуляется
            }

            // Пошаговый мониторинг (по прямой просьбе пользователя
            // 2026-09-05). Отдельно от liveness-сигналов: те приходят,
            // только когда драйвер возвращается в свой Recv или успешно
            // завершает I/O, — а нас интересует ровно промежуток МЕЖДУ
            // ними. Счётчик прогресса драйвер крутит внутри всех длинных
            // циклов ожидания, поэтому:
            //   счётчик растёт      -> занят делом, не трогать;
            //   счётчик замер       -> ядро не исполняет его код вообще.
            for (int d = 3; d <= 6; d++) {
                if (g_driver_state_page == nullptr) break;
                if (!driver_reports_steps(d)) continue;
                seL4_Word prog = driver_state_word(d, DRIVER_STATE_WORD_PROGRESS);
                // Счётчик стоит на месте ЗАКОННО, пока драйвер не объявил,
                // что вошёл в шаг: простаивающему двигать нечего. Отсчёт
                // "как давно не двигался" начинаем заново и на каждом
                // возврате в простой — иначе к моменту следующей реальной
                // операции он уже был бы просрочен.
                seL4_Word step = driver_state_word(d, DRIVER_STATE_WORD_STEP);
                if (prog != g_driver_last_progress[d] || g_driver_progress_seen_ms[d] == 0 ||
                    step == USB_STEP_IDLE) {
                    g_driver_last_progress[d] = prog;
                    g_driver_progress_seen_ms[d] = watchdog_now_ms;
                }
            }

            // Отложенная переэнумерация: сброс шины уже был, а драйвер
            // тогда не отвечал. Как только он снова припаркован — отдаём
            // долг, иначе USB так и останется мёртвым при живой плате.
            if (g_usb_restart_pending && usb_cmd_ep != 0 && driver_is_parked(6)) {
                g_usb_restart_pending = false;
                usb_set_freeze(false); // снимаем заморозку ровно перед переэнумерацией
                uart_puts("[WATCHDOG] usb_driver снова отвечает — отдаю отложенную переэнумерацию после сброса шины.\n");
                seL4_SetMR(0, SYS_DRIVER_SIGNAL);
                seL4_SetMR(1, DRIVER_SIGNAL_RESTART);
                seL4_Call(usb_cmd_ep, seL4_MessageInfo_new(0, 0, 0, 2));
                uart_puts("[WATCHDOG] Отложенная переэнумерация вернула статус = "); uart_puthex(seL4_GetMR(0)); uart_puts("\n");
                g_driver_progress_seen_ms[6] = 0;
                g_usb_reset_attempts = 0;
            }

            for (int wd_driver = 3; wd_driver <= 6; wd_driver++) {
                if (!g_auto_restart_enabled[wd_driver]) continue;
                if (g_driver_last_seen_ms[wd_driver] == 0) continue; // ещё ни разу не спавнился/не инициализирован

                // Замер ли прогресс? Это ГЛАВНЫЙ признак настоящего
                // зависания, и он не зависит от того, доходит ли драйвер до
                // своего Recv.
                // Все три условия обязательны, и каждое отсекает свой
                // ложный случай: драйвер должен САМ РАЗМЕЧАТЬ шаги (иначе
                // нули в слоте читались бы как "замерший прогресс"), он
                // должен НАХОДИТЬСЯ В ШАГЕ (простой — законная причина не
                // двигать счётчик) и НЕ быть припаркованным.
                seL4_Word wd_step = driver_state_word(wd_driver, DRIVER_STATE_WORD_STEP);
                bool progress_stuck = false;
                if (g_driver_state_page != nullptr && driver_reports_steps(wd_driver) &&
                    wd_step != USB_STEP_IDLE &&
                    g_driver_progress_seen_ms[wd_driver] != 0 &&
                    driver_state_read(wd_driver) != DRIVER_STATE_PARKED &&
                    watchdog_now_ms - g_driver_progress_seen_ms[wd_driver] >= USB_STEP_STUCK_TIMEOUT_MS) {
                    progress_stuck = true;
                    uart_puts("[WATCHDOG] is_driver="); uart_putdec(wd_driver);
                    uart_puts(": прогресс замер на шаге \""); uart_puts(usb_step_name(wd_step));
                    uart_puts("\" уже "); uart_putdec(watchdog_now_ms - g_driver_progress_seen_ms[wd_driver]);
                    uart_puts(" мс — это не долгая операция, это зависание.\n");
                }

                if (!progress_stuck) {
                    if (watchdog_now_ms - g_driver_last_seen_ms[wd_driver] < WATCHDOG_TIMEOUT_MS[wd_driver]) continue;
                    // Сигнала живости нет, НО счётчик прогресса шевелится —
                    // драйвер занят реальной длинной работой (перечисление
                    // устройства, длинная SCSI-операция) и до своего Recv
                    // просто ещё не дошёл. Раньше такой случай был
                    // неотличим от зависания и стоил ложных убийств живого
                    // драйвера (issuse.txt №66); теперь отличим.
                    if (g_driver_state_page != nullptr && g_driver_progress_seen_ms[wd_driver] != 0 &&
                        watchdog_now_ms - g_driver_progress_seen_ms[wd_driver] < WATCHDOG_TIMEOUT_MS[wd_driver]) {
                        continue;
                    }
                }

                int wd_target_pid = -1;
                for (int i = 1; i < 256; i++) {
                    if (pcbs[i].active && pcbs[i].is_driver == wd_driver) { wd_target_pid = i; break; }
                }
                // Нет активного процесса с таким is_driver — либо ещё не
                // заспавнен (wifi_driver до "wifi start"), либо уже в
                // процессе респавна кем-то другим — пропускаем тихо, не
                // ошибка. last_seen НЕ сбрасываем — если/когда драйвер
                // появится, spawn_process() сам переинициализирует его.
                if (wd_target_pid == -1) continue;

                // Ограничитель для usb_driver: если шина мертва
                // по-настоящему, сброс не поможет ни с первого раза, ни с
                // десятого, а повторять его каждые WATCHDOG_TIMEOUT_MS[6]
                // значит бесконечно дёргать PERST и забивать лог. Счётчик
                // обнуляется, как только приходит хоть один настоящий
                // liveness-сигнал от usb_driver (см. выше по циклу).
                // Проверяем ДО печати "auto-recovering" — иначе она сама
                // стала бы тем самым бесконечным потоком в логе.
                if (wd_driver == 6 && RPI4_ENABLE_USB && g_usb_reset_attempts >= USB_RESET_MAX_CONSECUTIVE) {
                    if (g_usb_reset_attempts == USB_RESET_MAX_CONSECUTIVE) {
                        uart_puts("[WATCHDOG] usb_driver не ожил после "); uart_putdec(USB_RESET_MAX_CONSECUTIVE);
                        uart_puts(" подряд полных сбросов шины — прекращаю автоматические попытки. Ручные `usbreset`/`recover usb_driver` по-прежнему доступны; если и они не помогают, шина требует физической перезагрузки платы.\n");
                        g_usb_reset_attempts++; // сообщение печатается ровно один раз
                    }
                    continue;
                }

                uart_puts(progress_stuck ? "[WATCHDOG] Зависание на шаге (is_driver="
                                         : "[WATCHDOG] Heartbeat timeout for is_driver=");
                uart_putdec(wd_driver);
                uart_puts(" (PID "); uart_putdec(wd_target_pid); uart_puts(")");
                // Порог heartbeat'а (5с) короче порога "замер прогресс"
                // (10с), поэтому для usb_driver почти всегда первым
                // срабатывает именно heartbeat — и информация о том, НА
                // КАКОМ ШАГЕ он встал, потерялась бы. Печатаем её здесь.
                if (driver_reports_steps(wd_driver) && g_driver_state_page != nullptr) {
                    uart_puts(", шаг: \""); uart_puts(usb_step_name(wd_step)); uart_puts("\"");
                }
                uart_puts(" — auto-recovering.\n");

                // usb_driver — особый случай (по прямому решению
                // пользователя 2026-09-05). Обычный kill+respawn ему не
                // помогает: он зависает не сам по себе, а вместе с PCIe-
                // шиной под собой (аппаратное ограничение, см. issuse.txt/
                // usb_driver.cpp), и пересозданный процесс упирается ровно
                // в ту же мёртвую шину. Поэтому здесь запускается ТОТ ЖЕ
                // путь, что и ручной `usbreset`: полная инициализация с
                // нуля — fundamental reset Root Complex'а, восстановление
                // всего, что стирает bridge software init, перезаливка
                // прошивки VL805 через VideoCore-mailbox и переэнумерация.
                // Пересоздание процесса остаётся запасным вариантом — на
                // случай, если шину подняли, а сам драйвер так и не ожил.
                if (wd_driver == 6 && RPI4_ENABLE_USB) {
                    g_usb_reset_attempts++;
                    seL4_Word wd_reset_status = root_usb_full_reset(timer_ep, usb_cmd_ep);
                    g_driver_progress_seen_ms[6] = watchdog_now_ms; // отсчёт заново, иначе следующий тик сработает мгновенно
                    if (wd_reset_status == USB_RESET_OK) {
                        // Драйвер жив и переэнумерировал устройства сам —
                        // отметим его "видели только что", иначе следующий
                        // же тик снова сочтёт таймаут истёкшим и мы уйдём в
                        // цикл сбросов, не дав драйверу выйти на связь.
                        g_driver_last_seen_ms[6] = watchdog_now_ms;
                        continue;
                    }
                    // Пересоздания процесса для USB больше нет вообще
                    // (см. generic_recover_process()): снос VSpace — это
                    // dsb sy, который при застопоренной шине вешает плату.
                    uart_puts("[WATCHDOG] Сброс шины не закрыл вопрос (статус = "); uart_puthex(wd_reset_status);
                    uart_puts("). Пересоздание процесса для USB запрещено — жду следующей попытки.\n");
                    continue;
                }

                generic_recover_process(wd_target_pid, ep, med_ep, alloc, root_cnode, root_vspace, normal_untyped,
                                        shm_frames[0], console_ep, timer_ep, blk_ep);
                // last_seen для этого is_driver будет переинициализирован
                // самим spawn_process() внутри generic_recover_process()
                // выше (см. блок "инициализируем last_seen" там) — здесь
                // ничего вручную поправлять не нужно.
            }
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
                check_err(ram_retype(normal_untyped, seL4_ARM_SmallPageObject, 0, root_cnode, 0, 0, frame, 1), "Pager Frame");
                
                uintptr_t page_aligned = addr & ~0xFFFULL;
                check_err(seL4_ARM_Page_Map(frame, pcbs[sender_pid].vspace, page_aligned, seL4_AllRights, seL4_ARM_Default_VMAttributes), "Pager Map");
                
                seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 0));
                continue;
            } else {
                uart_puts("\nFATAL FAULT! PID: "); uart_putdec(sender_pid);
                uart_puts(" ("); uart_puts(pcbs[sender_pid].name); uart_puts(")");
                uart_puts("\nPC: "); uart_puthex(pc);
                uart_puts("\nMem Addr: "); uart_puthex(addr);
                uart_puts("\n");

                // Полный контекст упавшего потока. Двух чисел (PC и адрес
                // данных) катастрофически не хватает в самом частом
                // интересном случае — PC=0: это значит, что поток КУДА-ТО
                // прыгнул по нулю, и весь вопрос в том, ОТКУДА. Ответ
                // лежит в x30 (LR, адрес возврата вызвавшего) и в стеке.
                // Раньше такой отказ приходилось разбирать гаданием
                // (hotplugtest, 2026-09-06: два прогона подряд умерли с
                // PC=0, и по двум числам сказать было нечего).
                // seL4_TCB_ReadRegisters над упавшим потоком безопасен: он
                // заблокирован ядром на fault-endpoint'е и никуда не
                // денется. MR'ы при этом затираются — поэтому pc/addr
                // прочитаны ВЫШЕ, до вызова.
                if (sender_pid != 0 && pcbs[sender_pid].tcb != 0) {
                    seL4_UserContext fregs = {0};
                    if (seL4_TCB_ReadRegisters(pcbs[sender_pid].tcb, false, 0,
                                               sizeof(seL4_UserContext) / sizeof(seL4_Word), &fregs) == seL4_NoError) {
                        uart_puts("  SP  = "); uart_puthex(fregs.sp);
                        uart_puts("\n  LR  (x30, откуда пришли) = "); uart_puthex(fregs.x30);
                        uart_puts("\n  FP  (x29) = "); uart_puthex(fregs.x29);
                        uart_puts("\n  x0..x3 = "); uart_puthex(fregs.x0); uart_puts(" ");
                        uart_puthex(fregs.x1); uart_puts(" ");
                        uart_puthex(fregs.x2); uart_puts(" ");
                        uart_puthex(fregs.x3); uart_puts("\n");
                    }
                }

                // ДОБАВЛЕНО: Предохранитель от бага неинициализированного TLS/IPC Buffer
                if (addr == 0x12) {
                    uart_puts("FATAL: TLS/IPC Buffer not initialized (Data Abort at 0x12).\n");
                    uart_puts("Halting Watchdog respawn to prevent memory/slot leaks.\n");
                    while(1); 
                }

                if (sender_pid != 0) {
                    generic_recover_process(sender_pid, ep, med_ep, alloc, root_cnode, root_vspace, normal_untyped, shm_frames[0], console_ep, timer_ep, blk_ep, /*respawn=*/true, /*target_faulted=*/true);
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
                generic_recover_process(sender_pid, ep, med_ep, alloc, root_cnode, root_vspace, normal_untyped, shm_frames[0], console_ep, timer_ep, blk_ep, /*respawn=*/true, /*target_faulted=*/true);
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

                // issuse.txt №10 — requested_fd раньше использовался как сырой
                // индекс CSlot'а без проверки против зарезервированных boot-
                // слотов (0-24, см. local_console_ep/local_timer_ep/.../
                // local_usb_heartbeat_ntfn выше в spawn_process()). Единственный
                // легитимный вызывающий (shell.cpp:1332) всегда шлёт ровно
                // PIPE_FD_SLOT — любое другое значение либо ошибка клиента,
                // либо попытка перезаписать чужой boot-слот (диск/консоль/
                // таймер и т.п.) чужой pipe-капой.
                if (requested_fd != PIPE_FD_SLOT) {
                    seL4_SetMR(0, -1);
                    seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                    break;
                }

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
                        // issuse.txt (пайпинг для /sbin-команд): MR1=pipe_id
                        // — нужен вызывающему, чтобы передать его дальше в
                        // SYS_EXEC (см. spawn_process/stdout_pipe_id), когда
                        // левая часть пайпа — не встроенная команда, а
                        // отдельный /sbin-процесс.
                        seL4_SetMR(1, (seL4_Word)pipe_id);
                        seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 2));
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
                    // Пишем данные в буфер пайпа — сколько влезает.
                    int written = 0;
                    while (written < chunk && p->count < 4096) {
                        p->buffer[p->count++] = (char)seL4_GetMR(written + 1);
                        written++;
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

                    // issuse.txt №7 — буфер (4096) кончился раньше, чем влез
                    // весь чанк. ПЕРВАЯ попытка фикса (2026-08-16) блокировала
                    // писателя через SaveCaller до освобождения места —
                    // ОТКАЧЕНА после hw-теста: shell.cpp запускает правую
                    // часть конвейера ТОЛЬКО ПОСЛЕ полного выхода левой
                    // (sys_wait перед spawn_thread(grep)), так что писатель
                    // мог застрять в ожидании читателя, которого структурно
                    // ещё не существует — гарантированный дедлок при любом
                    // выводе больше 4096 байт. Вместо блокировки — честный
                    // короткий write: пишем сколько влезло, ВСЕГДА отвечаем
                    // немедленно, но возвращаем реальное число принятых байт
                    // вместо молчаливого "успех" (см. sys_write() в
                    // h/sys_client.h — теперь останавливается на коротком
                    // write вместо тихой потери хвоста).
                    if (written < chunk) {
                        uart_puts("[PIPE] WARNING: buffer full, write truncated\n");
                    }
                    seL4_SetMR(0, written);
                    seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
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
                // issuse.txt №64 — root-caused и исправлено 2026-08-23 (было
                // "не root-caused, обойдено" с 2026-08-16). Настоящая причина:
                // бейдж потока кодировал parent_pid В СТАРШИХ БИТАХ
                // ((parent_pid<<16)|new_pid) — для БОЛЬШИНСТВА parent_pid это
                // задевало биты 16-19, ровно диапазон DRIVER_LIVENESS_*_BADGE
                // (0x10000-0x80000, common.h, Фаза 15 — появился ПОЗЖЕ этого
                // кода). Диспетчер root'а безусловно перехватывает ЛЮБОЙ
                // sender_badge >= DRIVER_LIVENESS_BLK_BADGE как heartbeat-тик
                // и делает `continue` до switch(cmd) — seL4_Reply() для
                // настоящего сообщения потока никогда не вызывался, поток
                // навсегда зависал в seL4_Call (подтверждено JTAG:
                // seL4_DebugCapIdentify показывал корректный endpoint-cap,
                // seL4_DebugDumpScheduler — BlockedOnReply, не BlockedOnSend,
                // т.е. IPC ядром доставлялось верно, root его просто
                // игнорировал). Фикс — бейдж потока теперь ПРОСТО new_pid
                // (1:1 с обычным badged_ep процессов выше, безопасный
                // диапазон 1-255), parent_pid хранится отдельным полем PCB
                // (см. struct ProcessControlBlock) вместо кодирования в
                // бейдж — тот же принцип, что и весь остальной PCB, без
                // риска коллизии с любым будущим диапазоном бейджей.
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
                pcb.parent_pid = (int)sender_pid; // issuse.txt №64 — вместо кодирования в бейдж

                seL4_CPtr new_tcb = alloc_and_track_cap(alloc, pcb);
                ram_retype(normal_untyped, seL4_TCBObject, seL4_TCBBits, root_cnode, 0, 0, new_tcb, 1);

                // --- БЕЙДЖ ПОТОКА (issuse.txt №64) — просто new_pid, 1:1 с
                // обычным badged_ep процессов (см. выше в этой функции),
                // безопасный диапазон 1-255, никогда не пересечётся ни с
                // pipe-бейджами (1000+), ни с IRQ_MMC (2000), ни с
                // DRIVER_LIVENESS_*_BADGE (0x10000+). parent_pid — в pcb
                // (см. выше), не в бейдже.
                seL4_Word thread_badge = (seL4_Word)new_pid;
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
                ram_retype(normal_untyped, seL4_ARM_SmallPageObject, 0, root_cnode, 0, 0, ipc_frame, 1);
                
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

                // issuse.txt №64 (расследование, временно) — именуем поток для
                // seL4_DebugDumpScheduler() (clonetest.elf), иначе в дампе он
                // неотличим от остальных "child of: 'rootserver'" по одному
                // только IP. Чисто диагностическое, безопасно оставить —
                // seL4_DebugNameThread не влияет на поведение, только на имя
                // в отладочном выводе (CONFIG_DEBUG_BUILD).
                seL4_DebugNameThread(new_tcb, "CLONE_THREAD");

                seL4_TCB_Resume(new_tcb);

                seL4_SetMR(0, new_pid);
                seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                break;
            }

            case 105: { // SYS_THREAD_EXIT
                // issuse.txt №64 — root-caused и исправлено 2026-08-23 (см.
                // подробный разбор у case SYS_CLONE выше). Бейдж потока
                // теперь просто new_pid — sender_pid (общая переменная
                // диспетчера, извлечённая ДО switch(cmd)) уже корректно
                // равен thread_pid, доп. извлечение из бейджа не нужно.
                // parent_pid — из pcb (записан в case SYS_CLONE), не из
                // бейджа.
                int thread_pid = (int)sender_pid;
                int parent_pid = pcbs[thread_pid].parent_pid;

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
                // issuse.txt №30 (расследование) — было 64 байта (8
                // регистров MR1-8), см. подробный разбор у STARTUP
                // PAYLOAD/spawn_process() выше. Теперь 24 регистра
                // (MR1-24, 192 байта) — с запасом покрывает "/sbin/<cmd>.elf "
                // + аргумент до 127 символов (реальный предел ниже по
                // цепочке, см. args_len в spawn_process()).
                char app_name_and_args[256] = {0};

                uint64_t* name_ptr = (uint64_t*)app_name_and_args;
                for (int i = 0; i < 24; i++) {
                    name_ptr[i] = seL4_GetMR(i + 1);
                }
                app_name_and_args[255] = '\0'; // Защита

                // Отделяем имя приложения от аргументов
                char* args = app_name_and_args;
                while (*args && *args != ' ') args++;
                if (*args == ' ') {
                    *args = '\0';
                    args++;
                } else {
                    args = (char*)""; // No args
                }

                // Фаза A (см. ROADMAP.md): путь вида "/sbin/..." — доверенная
                // системная утилита (is_driver=253, см.
                // shm_pages_mask_for_role()), всё остальное — обычный
                // пользовательский exec (254, fail-closed по SHM, как раньше).
                // Сравнение регистрозависимое (Фаза 9.D: ФС теперь тоже
                // регистрозависима, см. fat32.cpp dir_scan) — "/SBIN/x.elf" не
                // совпадёт с этим префиксом И не найдётся на диске (каталог
                // называется "sbin", а не "SBIN"), так что ложного доверия не
                // возникает в любом случае.
                const char *sbin_prefix = "/sbin/";
                bool is_trusted_sbin = true;
                for (int i = 0; sbin_prefix[i] != '\0'; i++) {
                    if (app_name_and_args[i] != sbin_prefix[i]) { is_trusted_sbin = false; break; }
                }
                // Фаза 9.C: is_driver=253 выдаём, только если сам вызывающий
                // уже доверенный (shell или другой 253) — иначе untrusted
                // exec (254) мог бы самоповысить себе привилегии просто
                // запустив "/sbin/whatever.elf" и получить доступ к VFS SHM +
                // мьютексу, которого у него не было. Легитимные пути (shell,
                // balancer.elf) как вызывали /sbin с полным доверием, так и
                // продолжают.
                int exec_is_driver = (is_trusted_sbin && is_admin_caller(sender_pid)) ? 253 : 254;

                // Фаза 9.A (продолжение, найдено на живом железе): cwd
                // вызывающего шелла для доверенной /sbin-утилиты — MR25-32
                // (сдвинуто с MR9-16 после расширения app_name_and_args до
                // 24 регистров, см. issuse.txt №30 выше), если шелл их
                // передал (length>=33). НЕ через общую VFS SHM — см.
                // EXEC_CWD_MSG_SLOT/common.h, там его затирает
                // load_elf_from_disk() ниже (читает файл через ту же
                // физическую страницу).
                char exec_cwd_payload[64] = {0};
                if (seL4_MessageInfo_get_length(recv_info) >= 33) {
                    uint64_t* cwd_ptr = (uint64_t*)exec_cwd_payload;
                    for (int i = 0; i < 8; i++) {
                        cwd_ptr[i] = seL4_GetMR(i + 25);
                    }
                    exec_cwd_payload[63] = '\0';
                }

                // issuse.txt (пайпинг для /sbin-команд): MR33 = pipe_id
                // (сдвинуто с MR17, см. issuse.txt №30 у cwd выше), если
                // шелл спавнит эту команду как ЛЕВУЮ часть конвейера (см.
                // SYS_PIPE/case 20 — вызывающий получает pipe_id оттуда и
                // передаёт сюда без изменений). -1 (нет MR33) — обычный
                // exec, stdout на console_ep, как всегда.
                int exec_stdout_pipe_id = -1;
                if (seL4_MessageInfo_get_length(recv_info) >= 34) {
                    exec_stdout_pipe_id = (int)seL4_GetMR(33);
                }

                if (LOG_ROOT) {
                    uart_puts("[ROOT] Fetching ELF from disk: ");
                    uart_puts(app_name_and_args);
                    uart_puts("...\n");
                }

                int elf_size = load_elf_from_disk(blk_ep, app_name_and_args, g_elf_load_buffer);
                int new_pid = -1;

                if (elf_size > 0) {
                    if (LOG_ROOT) uart_puts("[ROOT] ELF loaded successfully! Spawning...\n");
                    elf_t elf;
                    if (elf_newFile(g_elf_load_buffer, elf_size, &elf) == 0) {
                        // issuse.txt п.3 — переиспользуемая RAM-арена для обычных
                        // команд (см. ram_retype()/acquire_cmd_arena() выше).
                        // Пул арен исчерпан (>16 одновременно) или
                        // ретайп самой арены не удался — cmd_arena_for_this_spawn
                        // остаётся 0, g_current_arena НЕ выставляется, спавн идёт
                        // обычным путём через общий пул, как раньше (деградация,
                        // не отказ).
                        seL4_CPtr cmd_arena_for_this_spawn = 0;
                        if (CMD_ARENA_ENABLED && (exec_is_driver == 253 || exec_is_driver == 254)) {
                            cmd_arena_for_this_spawn = acquire_cmd_arena(alloc, root_cnode);
                            if (cmd_arena_for_this_spawn != 0) {
                                g_current_arena = cmd_arena_for_this_spawn;
                                g_current_arena_bytes = 0;
                            }
                        }
                        new_pid = spawn_process(app_name_and_args, g_elf_load_buffer, elf_size, ep, med_ep, alloc, root_cnode, root_vspace, normal_untyped,
                                                shm_frames[0], exec_is_driver, console_ep, timer_ep, blk_ep, console_ep, console_ep, console_ep,
                                                0, 0, 0, args, 0, net_cmd_send_ep,
                                                0, // wifi_cmd_recv_ep
                                                0, // wifi_cmd_send_ep
                                                0, // mbox_regs_frame
                                                0, // mbox_buf_frame_param
                                                0, // mbox_buf_paddr_param
                                                0, // extra_ntfn_param
                                                0, // blk_dma_frame_param
                                                0, // blk_dma_paddr_param
                                                0, // extra_ntfn2_param
                                                0, // net_wifi_rx_badged_param
                                                0, // net_wifi_tx_wake_param
                                                0, // extra_ntfn3_param
                                                0, // mmc_irq_handler_param
                                                0, // blk_dma_frame2_param
                                                0, // blk_dma_paddr2_param
                                                vfs_mutex_ntfn, // только реально используется при exec_is_driver==253, см. spawn_process
                                                exec_cwd_payload,
                                                exec_stdout_pipe_id,
                                                0, // usb_cmd_recv_ep_param (exec'нутый /sbin — не usb_driver)
                                                nullptr, // usb_dma_param
                                                0, // pcie_rc_frame_param
                                                0, // pcie_err_frame_param
                                                // Milestone 9: только доверенные /sbin (253) получают
                                                // VFS-доступ к /mnt/usb0 — тот же принцип, что и с
                                                // VFS-мьютексом (vfs_mutex_ntfn) чуть выше.
                                                (exec_is_driver == 253) ? usb_cmd_ep : 0);
                        // issuse.txt: запоминаем реальный путь на диске, чтобы
                        // watchdog мог перечитать этот же файл при аварийном
                        // респавне (см. ProcessControlBlock::path) — тут name
                        // (app_name_and_args) и path это буквально одна и та
                        // же строка, в отличие от wifi_driver/init.conf-сервисов.
                        if (new_pid > 0) {
                            strncpy(pcbs[new_pid].path, app_name_and_args, 63);
                            pcbs[new_pid].path[63] = '\0';
                        }
                        // issuse.txt п.3 — арена приобреталась ДО spawn_process()
                        // (см. выше), g_current_arena нужно погасить в любом
                        // случае (успех/провал), иначе она "утечёт" в следующий,
                        // никак не связанный retype где-то ещё в главном цикле.
                        if (cmd_arena_for_this_spawn != 0) {
                            g_current_arena = 0;
                            if (new_pid > 0) {
                                pcbs[new_pid].cmd_arena = cmd_arena_for_this_spawn;
                                pcbs[new_pid].arena_bytes_used = g_current_arena_bytes;
                            } else {
                                // spawn_process не дошёл до конца — то немногое,
                                // что успело ретайпнуться в арену (если успело),
                                // всё равно чистим, арену возвращаем в свободный
                                // стек для следующей попытки.
                                release_cmd_arena(root_cnode, cmd_arena_for_this_spawn);
                                g_ram_bytes_used -= g_current_arena_bytes;
                            }
                        }
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
                        // Намеренно ограничено ПЕРВЫМИ 4 страницами (16KB) из 8 в
                        // shm_frames[8] — страницы 4-7 зарезервированы под Wi-Fi
                        // control-plane/link-state/TX-RX-мейлбокс и staging-буфер
                        // blk_driver'а (см. h/platform.h), файловому/ps-протоколу они
                        // не должны быть доступны ни при каких обстоятельствах.
                        // Резервируем запас на самую длинную возможную строку записи
                        // ("    " + PID + " [RUNNING] " + name[32] + "\n"), чтобы не выйти
                        // за пределы этих 4 страниц при большом числе процессов.
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

            // Фаза 6.1 (SMP, см. ROADMAP.md): ручной перенос уже запущенного
            // процесса на другое ядро в рантайме. Коды ответа (MR0):
            // 0=успех, 1=процесс не найден, 2=root зафиксирован на ядре 0,
            // 3=timer_driver нельзя переносить (PPI-опасность — см. ROADMAP),
            // 4=некорректное ядро. seL4_TCB_SetAffinity безопасно звать на
            // уже resumed/running TCB (см. kernel/src/object/tcb.c,
            // invokeTCB_SetAffinity — сам разруливает dequeue/remote-stall).
            case SYS_SET_AFFINITY: {
                seL4_Word target_pid = seL4_GetMR(1);
                seL4_Word target_core = seL4_GetMR(2);
                seL4_Word status;

                if (!is_admin_caller(sender_pid)) {
                    status = 5; // Фаза 9.C: доступ запрещён
                } else if (target_pid == 0) {
                    status = 2;
                } else if (target_pid >= 256 || !pcbs[target_pid].active) {
                    status = 1;
                } else if (!process_is_migratable(target_pid)) {
                    // issuse.txt №74, часть "б" — раньше здесь стоял запрет
                    // на ЛЮБОЙ драйвер (is_driver != 0): "в лоб" от
                    // cross-core зависания root'а в ipi_wait() при
                    // последующем Suspend'е (issuse.txt №73, JTAG 2026-08-25).
                    // Теперь причина устранена по существу —
                    // prepare_driver_for_suspend() не даёт вызвать Suspend
                    // над потоком, который может исполняться на чужом ядре, —
                    // и переносить можно всех, кроме timer_driver'а (см.
                    // process_is_migratable(): per-core PPI + он же кормит
                    // PM_WDOG и обслуживает сам механизм ожидания PARKED).
                    status = 3;
                } else if (target_core > 3) {
                    status = 4;
                } else if (!core_allowed_for_process((int)target_pid, (int)target_core)) {
                    status = 6; // драйверам ядро 0 запрещено — см. core_allowed_for_process()
                } else if (!migrate_process_to_core((int)target_pid, (int)target_core, timer_ep)) {
                    status = 7; // драйвер занят работой с железом, безопасного момента не дождались
                } else {
                    status = 0;
                }

                seL4_SetMR(0, status);
                seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                break;
            }

            // Фаза 6.1 (SMP, см. ROADMAP.md): разовый снимок нагрузки для
            // команды `top`. seL4_BenchmarkGetThreadUtilisation(tcb) отдаёт
            // через ipc-буфер вызывающего: BENCHMARK_TCB_UTILISATION — такты
            // занятости запрошенного потока; BENCHMARK_IDLE_TCBCPU_UTILISATION —
            // такты простоя ЯДРА, на котором этот поток сейчас крутится;
            // BENCHMARK_TOTAL_UTILISATION — общие такты периода на ядре
            // вызывающего (root, всегда ядро 0) — переиспользуем как общий
            // знаменатель для % на всех ядрах (период один и тот же для всех,
            // частота одна и та же). Короткое окно (~100мс) вместо "с момента
            // загрузки" — иначе на давно висящей системе load был бы
            // бессмысленным нулём.
            case SYS_TOP_STATS: {
                // MR1 (см. shell.cpp): 0 = разовый снимок по умолчанию —
                // компактная таблица "какое ядро насколько занято и кто на
                // нём" (как в примере пользователя); 1 = `top -l` —
                // подробная построчная таблица PID/CORE/%CPU/NAME (старый
                // формат), но с выровненными по ширине столбцами вместо
                // "плывущих" при разном числе цифр.
                seL4_Word mode = (seL4_MessageInfo_get_length(recv_info) >= 2) ? seL4_GetMR(1) : 0;

                LoadSnapshot snap;
                collect_load_snapshot(snap, console_ep, blk_ep, net_cmd_send_ep, wifi_cmd_send_ep, timer_ep);

                char *shm = rootserver_shm_base;
                int offset = 0;

                if (mode == 0) {
                    strcpy(shm, "CORE    %CPU  NAME\n"); offset = strlen(shm);
                    for (int c = 0; c < 4; c++) {
                        offset += append_udec_width(shm + offset, c, 4);
                        strcpy(shm + offset, " "); offset += 1;
                        if (snap.core_enabled[c]) {
                            uint64_t idle_pct = (snap.core_idle[c] >= snap.total[c]) ? 100 : (100 * snap.core_idle[c] / snap.total[c]);
                            offset += append_udec_width(shm + offset, 100 - idle_pct, 5);
                        } else {
                            offset += append_str_width(shm + offset, "?", 5);
                        }
                        strcpy(shm + offset, "  "); offset += 2;

                        bool first = true;
                        if (c == 0) {
                            strcpy(shm + offset, "rootserver"); offset = strlen(shm);
                            first = false;
                        }
                        for (int i = 1; i < 256; i++) {
                            if (!pcbs[i].active || pcbs[i].core != c) continue;
                            if (!first) { strcpy(shm + offset, ", "); offset += 2; }
                            strcpy(shm + offset, pcbs[i].name); offset = strlen(shm);
                            first = false;
                        }
                        if (first) { strcpy(shm + offset, "-"); offset = strlen(shm); }
                        strcpy(shm + offset, "\n"); offset = strlen(shm);
                    }
                } else {
                    strcpy(shm, "Ядро:"); offset = strlen(shm);
                    for (int c = 0; c < 4; c++) {
                        strcpy(shm + offset, "  "); offset += 2;
                        offset += append_udec(shm + offset, c);
                        strcpy(shm + offset, "="); offset += 1;
                        if (snap.core_enabled[c]) {
                            uint64_t idle_pct = (snap.core_idle[c] >= snap.total[c]) ? 100 : (100 * snap.core_idle[c] / snap.total[c]);
                            offset += append_udec(shm + offset, 100 - idle_pct);
                            strcpy(shm + offset, "%"); offset += 1;
                        } else {
                            strcpy(shm + offset, "нет данных"); offset = strlen(shm);
                        }
                    }
                    strcpy(shm + offset, "\n"); offset = strlen(shm);

                    offset += append_str_width(shm + offset, "PID", 5);
                    strcpy(shm + offset, " "); offset += 1;
                    offset += append_str_width(shm + offset, "CORE", 5);
                    strcpy(shm + offset, " "); offset += 1;
                    offset += append_str_width(shm + offset, "%CPU", 5);
                    strcpy(shm + offset, " NAME\n"); offset = strlen(shm);

                    offset += append_udec_width(shm + offset, 0, 5);
                    strcpy(shm + offset, " "); offset += 1;
                    offset += append_udec_width(shm + offset, 0, 5);
                    strcpy(shm + offset, " "); offset += 1;
                    offset += append_udec_width(shm + offset, 100 * snap.proc_util[0] / snap.total[0], 5);
                    strcpy(shm + offset, " rootserver\n"); offset = strlen(shm);

                    for (int i = 1; i < 256; i++) {
                        if (!pcbs[i].active) continue;
                        if (offset > 16384 - 64) { strcpy(shm + offset, "...\n"); offset += 4; break; }

                        offset += append_udec_width(shm + offset, i, 5);
                        strcpy(shm + offset, " "); offset += 1;
                        offset += append_udec_width(shm + offset, pcbs[i].core, 5);
                        strcpy(shm + offset, " "); offset += 1;
                        {
                            int c = pcbs[i].core;
                            if (c >= 0 && c < 4 && snap.core_enabled[c]) {
                                offset += append_udec_width(shm + offset, 100 * snap.proc_util[i] / snap.total[c], 5);
                            } else {
                                offset += append_str_width(shm + offset, "?", 5);
                            }
                        }
                        strcpy(shm + offset, " "); offset += 1;
                        strcpy(shm + offset, pcbs[i].name); offset = strlen(shm);
                        strcpy(shm + offset, "\n"); offset++;
                    }
                }

                flush_rootserver_shm();
                seL4_SetMR(0, 0);
                seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                break;
            }

            // Фаза 7 (DVFS), см. common.h — просто запоминаем момент
            // последней команды в шелле, ничего больше не считаем здесь
            // (сравнение с "сейчас" происходит в SYS_BALANCE ниже).
            case SYS_MARK_SHELL_ACTIVITY: {
                g_last_shell_activity_ms = seL4_GetMR(1);
                seL4_SetMR(0, 0);
                seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                break;
            }

            // issuse.txt №70 — клиент сам сообщает о взятии/отдаче
            // vfs_mutex_ep (см. vfs_lock()/vfs_unlock() в shell.cpp/
            // h/sys_client.h). Обычный fire-and-forget seL4_Send со стороны
            // клиента — БЕЗ seL4_Reply() здесь: отвечать некому, отправитель
            // уже продолжил работу, не дожидаясь ответа (тот же приём, что
            // heartbeat-тики/badge-нотификации в этом же цикле).
            case SYS_VFS_LOCK_NOTIFY: {
                if (arg1) g_vfs_lock_holder_pid = sender_pid;
                else if (g_vfs_lock_holder_pid == sender_pid) g_vfs_lock_holder_pid = 0;
                continue;
            }

            // Фаза 6.1 (продолжение, см. ROADMAP.md): `balance` — по команде
            // шелла, но цель переноса выбирает алгоритм. Политика: находим
            // САМОЕ занятое ядро (среди тех, где есть реальные данные и
            // >=2 резидентов), оставляем на нём "тяжёлый" (максимальный
            // %CPU) процесс в одиночестве, а всех остальных резидентов
            // (кроме root/timer_driver — те же две защиты, что у
            // SYS_SET_AFFINITY) раскидываем по наименее загруженным другим
            // ядрам. Идея пользователя: если на ядре один тяжёлый процесс и
            // несколько мелких — освободить ядро целиком под тяжёлый, а не
            // наоборот "увести самого тяжёлого".
            case SYS_BALANCE: {
                if (!is_admin_caller(sender_pid)) {
                    // Фаза 9.C: доступ запрещён — молча, без текста в SHM
                    // (у untrusted-вызывающего всё равно нет страницы VFS SHM,
                    // чтобы его прочитать, но сам перенос процессов не должен
                    // происходить вообще).
                    seL4_SetMR(0, (seL4_Word)-1);
                    seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                    break;
                }

                // "Тихий" режим (MR1==1, см. common.h/SYS_BALANCE): отчёт
                // складываем в собственный буфер root'а вместо общей
                // SHM-страницы. Нужен автотестам, гоняющим balance
                // одновременно с длинным чтением файла — та же страница в
                // этот момент занята данными файла (именно поэтому shell
                // берёт вокруг balance vfs_lock, но тогда параллельности и
                // не остаётся). Логика балансировки ниже не меняется ни на
                // строчку — отличается только адрес, куда пишется текст, и
                // отсутствие flush_rootserver_shm().
                bool balance_quiet = (seL4_MessageInfo_get_length(recv_info) >= 2) && (seL4_GetMR(1) == 1);

                LoadSnapshot snap;
                collect_load_snapshot(snap, console_ep, blk_ep, net_cmd_send_ep, wifi_cmd_send_ep, timer_ep);

                static char balance_quiet_sink[4096]; // тот же размер, что страница SHM — верхняя граница отчёта не меняется
                char *shm = balance_quiet ? balance_quiet_sink : rootserver_shm_base;
                int offset = 0;

                // Агрегатный % по ядру. Для ядра без данных (core_enabled
                // false) трактуем как 0 — но ТОЛЬКО как "привлекательную"
                // цель для переноса (пустое ядро без драйвера почти
                // наверняка простаивает); источником такое ядро ниже не
                // выбирается — без реальных данных нельзя утверждать, что
                // оно "самое занятое".
                int core_pct[4];
                for (int c = 0; c < 4; c++) {
                    if (snap.core_enabled[c]) {
                        uint64_t idle_pct = (snap.core_idle[c] >= snap.total[c]) ? 100 : (100 * snap.core_idle[c] / snap.total[c]);
                        core_pct[c] = (int)(100 - idle_pct);
                    } else {
                        core_pct[c] = 0;
                    }
                }

                // Фаза 7 (DVFS) — авто-губернатор ПЕРЕДЕЛАН: seL4_BenchmarkGetThreadUtilisation
                // считал busy-poll циклы драйверов (blk/wifi/GENET) и heartbeat-будильник
                // (~100мс) как реальную занятость, поэтому %busy почти никогда не опускался
                // ниже порога "уйти в low" (см. issuse.txt). Вместо этого — честный признак
                // простоя: давно ли был новый ввод в шелле (SYS_MARK_SHELL_ACTIVITY,
                // g_last_shell_activity_ms). Не идеально (долгая ОДНА команда вроде `wifi
                // scan` может словить low ближе к концу), но не завязан на busy-poll фон.
                {
                    seL4_SetMR(0, 4); // SYS_GET_UPTIME — свежее "сейчас" для сравнения
                    seL4_Call(timer_ep, seL4_MessageInfo_new(0, 0, 0, 1));
                    seL4_Word now_ms = seL4_GetMR(0);
                    constexpr seL4_Word CPUFREQ_IDLE_THRESHOLD_MS = 8000;
                    bool idle = (now_ms - g_last_shell_activity_ms) >= CPUFREQ_IDLE_THRESHOLD_MS;
                    seL4_SetMR(0, 12); // SYS_CPUFREQ_GOVERNOR_TICK, см. timer_driver.cpp
                    seL4_SetMR(1, idle ? 0 : 100); // переиспользуем готовый 10/30% гистерезис как чистый bool
                    seL4_Call(timer_ep, seL4_MessageInfo_new(0, 0, 0, 2));
                }

                int resident_count[4] = {0, 0, 0, 0};
                for (int i = 0; i < 256; i++) {
                    if (i != 0 && !pcbs[i].active) continue;
                    int c = (i == 0) ? 0 : pcbs[i].core;
                    if (c >= 0 && c < 4) resident_count[c]++;
                }

                int source = -1;
                for (int c = 0; c < 4; c++) {
                    if (!snap.core_enabled[c] || resident_count[c] < 2) continue;
                    if (source < 0 || core_pct[c] > core_pct[source]) source = c;
                }

                if (source < 0) {
                    strcpy(shm, "Балансировать нечего: ни на одном ядре с известной нагрузкой нет более одного процесса.\n");
                    if (!balance_quiet) flush_rootserver_shm();
                    seL4_SetMR(0, 0);
                    seL4_SetMR(1, 0); // перенесено процессов
                    seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 2));
                    break;
                }

                // "Тяжёлый" — резидент источника с максимальным %CPU (может
                // быть и root/timer_driver — тогда он и так не переносится
                // защитой в цикле переноса ниже, отдельно разбирать не надо).
                int heavy = -1;
                uint64_t heavy_util = 0;
                for (int i = 0; i < 256; i++) {
                    if (i != 0 && !pcbs[i].active) continue;
                    int c = (i == 0) ? 0 : pcbs[i].core;
                    if (c != source) continue;
                    if (heavy < 0 || snap.proc_util[i] > heavy_util) { heavy = i; heavy_util = snap.proc_util[i]; }
                }
                int working_pct[4];
                for (int c = 0; c < 4; c++) working_pct[c] = core_pct[c];

                strcpy(shm, "Ядро "); offset = strlen(shm);
                offset += append_udec(shm + offset, source);
                strcpy(shm + offset, " было самым занятым ("); offset = strlen(shm);
                offset += append_udec(shm + offset, core_pct[source]);
                strcpy(shm + offset, "%). Тяжёлый процесс остаётся: "); offset = strlen(shm);
                strcpy(shm + offset, (heavy == 0) ? "rootserver" : pcbs[heavy].name); offset = strlen(shm);
                strcpy(shm + offset, "\n"); offset = strlen(shm);

                int moved = 0;
                for (int i = 1; i < 256; i++) {
                    if (!pcbs[i].active) continue;
                    if (pcbs[i].core != source) continue;
                    if (i == heavy) continue;
                    // issuse.txt №74, часть "б": драйверы теперь
                    // РАВНОПРАВНЫЕ участники балансировки — раньше здесь
                    // отсекался любой is_driver != 0 (см. подробный разбор у
                    // того же условия в SYS_SET_AFFINITY выше). Исключение
                    // одно и то же на оба места — process_is_migratable().
                    if (!process_is_migratable(i)) continue;

                    int dest = -1;
                    for (int c = 0; c < 4; c++) {
                        if (c == source) continue;
                        // Ядро 0 — заповедное для драйверов (см.
                        // core_allowed_for_process(): зависший на PCIe
                        // драйвер уносит с собой ядро целиком, и если это
                        // ядро root'а — всю систему).
                        if (!core_allowed_for_process(i, c)) continue;
                        if (dest < 0 || working_pct[c] < working_pct[dest]) dest = c;
                    }
                    if (dest < 0) continue; // разрешённых получателей нет

                    // Занятый драйвер не переносим — перенесётся следующим
                    // проходом, когда встанет в свой seL4_Recv.
                    if (!migrate_process_to_core(i, dest, timer_ep)) continue;

                    int contrib_pct = (int)(100 * snap.proc_util[i] / snap.total[source]);
                    working_pct[dest] += contrib_pct;

                    strcpy(shm + offset, "  "); offset += 2;
                    strcpy(shm + offset, pcbs[i].name); offset = strlen(shm);
                    strcpy(shm + offset, " -> ядро "); offset = strlen(shm);
                    offset += append_udec(shm + offset, dest);
                    strcpy(shm + offset, "\n"); offset = strlen(shm);
                    moved++;
                }

                if (moved == 0) {
                    strcpy(shm + offset, "  (нечего переносить — остальные резиденты этого ядра тоже защищены)\n");
                    offset = strlen(shm);
                } else {
                    strcpy(shm + offset, "Перенесено процессов: "); offset = strlen(shm);
                    offset += append_udec(shm + offset, moved);
                    strcpy(shm + offset, "\n"); offset = strlen(shm);
                }

                if (!balance_quiet) flush_rootserver_shm();
                seL4_SetMR(0, 0);
                seL4_SetMR(1, (seL4_Word)moved);
                seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 2));
                break;
            }

            // issuse.txt №75 (доп.) — жёсткий reboot через PM_WDOG/PM_RSTC
            // (см. common.h/SYS_REBOOT, timer_driver.cpp/
            // pm_watchdog_expire_now()) — тот же admin-гейт, что
            // SYS_BALANCE/SYS_SET_AFFINITY выше. timer_driver — единственный
            // процесс с замапленными регистрами, просто форвардим и сразу
            // отвечаем вызывающему (реального ответа от timer_driver можно
            // не ждать — плата перезагрузится раньше, чем это имело бы
            // значение).
            case SYS_REBOOT: {
                if (!is_admin_caller(sender_pid)) {
                    seL4_SetMR(0, (seL4_Word)-1);
                    seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                    break;
                }
                uart_puts("[ROOT] SYS_REBOOT: запрошена аппаратная перезагрузка (PM_WDOG).\n");
                seL4_SetMR(0, SYS_REBOOT);
                seL4_Call(timer_ep, seL4_MessageInfo_new(0, 0, 0, 1));
                seL4_SetMR(0, 0);
                seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                break;
            }

            case SYS_PCIE_RESET: {
                // issuse.txt №74/в (см. situation.txt) — локальный сброс
                // ТОЛЬКО PCIe/VL805-шины, в отличие от SYS_REBOOT выше
                // (вся плата). Переэнумерация устройств теперь ВХОДИТ в
                // команду (34-я hw-попытка: DRIVER_SIGNAL_RESTART
                // форвардится автоматически) — ручной `driver usb_driver
                // restart` больше не нужен.
                if (!is_admin_caller(sender_pid)) {
                    seL4_SetMR(0, (seL4_Word)-1);
                    seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                    break;
                }
                if (!RPI4_ENABLE_USB) {
                    seL4_SetMR(0, (seL4_Word)-2);
                    seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                    break;
                }
                // Всё тело вынесено в root_usb_full_reset() — тот же путь
                // теперь запускает и heartbeat-watchdog автоматически (см.
                // главный цикл), а не только эта команда шелла.
                seL4_Word reset_status = root_usb_full_reset(timer_ep, usb_cmd_ep);
                if (reset_status == USB_RESET_DRIVER_STUCK) {
                    // Шина поднята, но переэнумерировать некому. Раньше
                    // здесь пересоздавался процесс — теперь нет: снос
                    // VSpace это deleteASID -> dsb sy, который при
                    // застопоренной шине замораживает ядро вместе со всей
                    // платой (JTAG 2026-09-06, см. generic_recover_process()).
                    uart_puts("[ROOT] SYS_PCIE_RESET: шина поднята, но usb_driver не отозвался. Пересоздание процесса для USB запрещено — повторите usbreset позже.\n");
                }
                seL4_SetMR(0, reset_status);
                seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                break;
            }

            // issuse.txt №74/в — SYS_PCIE_VL805_DEVSTATUS_PROBE (31-я
            // hw-попытка) УБРАН (37-я попытка): подтверждено на живом
            // железе, что этот IPC-вызов от usb_driver (ядро 2) сюда НЕ
            // мог нормально обслуживаться, пока root САМ висит в своём
            // seL4_Call на usb_cmd_ep (форвард DRIVER_SIGNAL_RESTART) —
            // probe-status возвращался = 0x99 = сам номер syscall'а
            // эхом, не реальный ответ этого case'а. usb_driver теперь
            // читает Device Status VL805 НАПРЯМУЮ через собственный
            // read-only маппинг PLAT_PCIE_CFG_DATA_USB_VADDR (см.
            // spawn_process()/read_vl805_devstatus_direct() в
            // usb_driver.cpp) — без IPC вообще.

            case SYS_KILL: {
                int target_pid = arg1;

                if (!is_admin_caller(sender_pid)) {
                    seL4_SetMR(0, (seL4_Word)-2); // Фаза 9.C: доступ запрещён
                    seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                    break;
                }

                if (target_pid == 0) {
                    uart_puts("\n[KERNEL PANIC] Attempted to kill Rootserver!\n");
                    seL4_SetMR(0, (seL4_Word)-1);
                } 
                else if (target_pid > 0 && target_pid < 256 && pcbs[target_pid].active) {
                    if (pcbs[target_pid].is_driver > 0 || strcmp(pcbs[target_pid].name, "shell") == 0) {
                        uart_puts("\n[KERNEL] Critical process killed manually. Triggering recovery...\n");                        
                        generic_recover_process(target_pid, ep, med_ep, alloc, root_cnode, root_vspace, normal_untyped, shm_frames[0], console_ep, timer_ep, blk_ep);
                    } else {
                    // issuse.txt №74/б — обычный (не драйверный) процесс тоже
                    // может оказаться на не-нулевом ядре: `balance` их как раз
                    // и раскидывает. Для не-драйвера эта функция никогда не
                    // отказывает — просто возвращает поток на ядро 0 и даёт
                    // старому ядру осесть, снимая условие remoteTCBStall().
                    prepare_driver_for_suspend(target_pid, timer_ep);
                    // issuse.txt №3 — та же уборка, что в SYS_EXIT: убитый
                    // процесс тоже мог оставить SYS_CLONE-потоки и держать
                    // VFS-мьютекс. Ветка драйверов выше уходит в
                    // generic_recover_process(), где это уже сделано.
                    reap_clone_threads_of(target_pid, alloc, root_cnode);
                    release_vfs_lock_if_held_by(target_pid);
                    seL4_TCB_Suspend(pcbs[target_pid].tcb);
                    // Отвязываем прерывание, только если оно было привязано (у драйверов)
                    if (pcbs[target_pid].irq_ntfn != 0) {
                        seL4_TCB_UnbindNotification(pcbs[target_pid].tcb);
                    }
                        pcbs[target_pid].active = false;
                        // issuse.txt п.3 (тот же баг, что чинили для SYS_EXIT, см.
                        // комментарий там же): удалять только 3 конкретные капы
                        // (badged_ep/tcb/vspace) БЕЗ полного цикла по cap_tracker
                        // течёт ещё сильнее, чем чинили для SYS_EXIT — child_cnode,
                        // страницы ELF/стека/IPC у обычной /sbin-команды тут вообще
                        // не освобождались. Тот же паттерн, что SYS_EXIT/
                        // generic_recover_process().
                        for (int i = 0; i < pcbs[target_pid].cap_tracker.count; i++) {
                            seL4_CPtr cap_to_free = pcbs[target_pid].cap_tracker.caps[i];
                            seL4_CNode_Revoke(root_cnode, cap_to_free, seL4_WordBits);
                            seL4_CNode_Delete(root_cnode, cap_to_free, seL4_WordBits);
                            alloc.free(cap_to_free);
                        }
                        pcbs[target_pid].cap_tracker.count = 0;
                        // issuse.txt п.3 — освобождение RAM-арены (см.
                        // ram_retype()/acquire_cmd_arena() выше), тот же приём,
                        // что в SYS_EXIT ниже.
                        if (pcbs[target_pid].cmd_arena != 0) {
                            release_cmd_arena(root_cnode, pcbs[target_pid].cmd_arena);
                            g_ram_bytes_used -= pcbs[target_pid].arena_bytes_used;
                            pcbs[target_pid].cmd_arena = 0;
                            pcbs[target_pid].arena_bytes_used = 0;
                        }
                        // issuse.txt №5 — та же течь SHM-копий, что чинили для
                        // SYS_EXIT (см. комментарий там же): kill обычной
                        // /sbin-команды, получившей SHM (is_driver==253), не
                        // освобождал shm_copies[9] вообще.
                        if (pcbs[target_pid].has_shm) {
                            for (int i = 0; i < SHM_TOTAL_PAGES; i++) {
                                if (pcbs[target_pid].shm_copies[i] != 0) {
                                    seL4_CNode_Delete(root_cnode, pcbs[target_pid].shm_copies[i], seL4_WordBits);
                                    alloc.free(pcbs[target_pid].shm_copies[i]);
                                    pcbs[target_pid].shm_copies[i] = 0;
                                }
                            }
                            pcbs[target_pid].has_shm = false;
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
                    // issuse.txt №3 — ПЕРВЫМ делом добрать за собой
                    // SYS_CLONE-потоки и отпустить глобальный VFS-мьютекс.
                    // Обязательно ДО цикла по cap_tracker ниже: он удаляет
                    // корни VSpace/CSpace, которые потоки разделяют с этим
                    // процессом. Разбор — у reap_clone_threads_of().
                    reap_clone_threads_of((int)sender_pid, alloc, root_cnode);
                    release_vfs_lock_if_held_by((int)sender_pid);

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
                    // Отменить отложенные ответы, адресованные этому
                    // процессу, ДО того как его TCB исчезнет.
                    //
                    // Ровно это generic_recover_process() делает много лет
                    // (см. комментарий там же: "иначе когда дедлайн/ввод
                    // наступит, они попробуют ответить по капе на уже не
                    // существующий TCB и уронят 'Attempted to invoke a null
                    // cap'"), а ОБЫЧНЫЙ выход процесса — единственный путь,
                    // которым завершается КАЖДАЯ /sbin-команда — не делал
                    // этого никогда. Асимметрия найдена при разборе
                    // плавающего падения тестового процесса (issuse.txt №3):
                    // сама она это падение, скорее всего, не объясняет
                    // (sleep/read у нас блокирующие, и уйти с висящим
                    // отложенным ответом трудно), но дыра настоящая и
                    // закрывается одной строкой симметрии.
                    if (timer_ep != 0 && pcbs[sender_pid].is_driver != 2) {
                        seL4_SetMR(0, SYS_CANCEL_PENDING_FOR_PID);
                        seL4_SetMR(1, (seL4_Word)sender_pid);
                        seL4_Call(timer_ep, seL4_MessageInfo_new(0, 0, 0, 2));
                    }
                    if (console_ep != 0 && pcbs[sender_pid].is_driver != 1) {
                        seL4_SetMR(0, SYS_CANCEL_PENDING_FOR_PID);
                        seL4_SetMR(1, (seL4_Word)sender_pid);
                        seL4_Call(console_ep, seL4_MessageInfo_new(0, 0, 0, 2));
                    }

                    seL4_TCB_Suspend(pcbs[sender_pid].tcb);
                    if (pcbs[sender_pid].irq_ntfn != 0) {
                        seL4_TCB_UnbindNotification(pcbs[sender_pid].tcb);
                    }
                    pcbs[sender_pid].active = false;

                    // НАЙДЕНО НА ЖИВОМ ЖЕЛЕЗЕ (см. ROADMAP.md/issuse.txt —
                    // "KERNEL PANIC: Out of CSlots during process allocation!"
                    // после десятка-другого обычных команд): раньше здесь
                    // удалялись только 3 конкретные капы (badged_ep/tcb/
                    // vspace) — БЕЗ единого alloc.free(), и совсем без учёта
                    // остальных ~7 капов из cap_tracker (child_cnode, page-
                    // table-слоты, stack/ipc-фреймы, см. spawn_process/
                    // alloc_and_track_cap), которые при обычном (не аварийном)
                    // выходе процесса не освобождались ВООБЩЕ — ни на уровне
                    // ядра (revoke/delete), ни в пуле слотов аллокатора. Это
                    // единственный путь выхода, которым пользуется КАЖДАЯ
                    // /sbin-команда после нормального завершения — то есть
                    // течь происходила при любом ls/ps/touch/cat/... и после
                    // достаточного числа команд слоты CNode заканчивались.
                    // Тот же паттерн полной очистки уже применяется в
                    // generic_recover_process()/case 105 (SYS_THREAD_EXIT)
                    // ниже — используем его здесь тоже, вместо трёх ручных
                    // удалений.
                    for (int i = 0; i < pcbs[sender_pid].cap_tracker.count; i++) {
                        seL4_CPtr cap_to_free = pcbs[sender_pid].cap_tracker.caps[i];
                        seL4_CNode_Revoke(root_cnode, cap_to_free, seL4_WordBits);
                        seL4_CNode_Delete(root_cnode, cap_to_free, seL4_WordBits);
                        alloc.free(cap_to_free);
                    }
                    pcbs[sender_pid].cap_tracker.count = 0;

                    // issuse.txt п.3 — освобождение RAM-арены (см.
                    // ram_retype()/acquire_cmd_arena() выше): вместо того, чтобы
                    // память навсегда оставалась занятой (как выше — revoke/
                    // delete по одной капе НЕ возвращает физическую память
                    // родительскому untyped'у), для команд, спавненных через
                    // арену, делаем revoke САМОЙ АРЕНЫ — сбрасывает её free
                    // index на 0, капа возвращается в свободный стек для
                    // следующей команды.
                    if (pcbs[sender_pid].cmd_arena != 0) {
                        release_cmd_arena(root_cnode, pcbs[sender_pid].cmd_arena);
                        g_ram_bytes_used -= pcbs[sender_pid].arena_bytes_used;
                        pcbs[sender_pid].cmd_arena = 0;
                        pcbs[sender_pid].arena_bytes_used = 0;
                    }

                    // Та же течь для SHM-копии (is_driver==253 /sbin-утилиты
                    // получают её при спавне, см. shm_pages_mask_for_role()) —
                    // тоже нигде не освобождалась при обычном выходе.
                    if (pcbs[sender_pid].has_shm) {
                        for (int i = 0; i < SHM_TOTAL_PAGES; i++) {
                            if (pcbs[sender_pid].shm_copies[i] != 0) {
                                seL4_CNode_Delete(root_cnode, pcbs[sender_pid].shm_copies[i], seL4_WordBits);
                                alloc.free(pcbs[sender_pid].shm_copies[i]);
                                pcbs[sender_pid].shm_copies[i] = 0;
                            }
                        }
                        pcbs[sender_pid].has_shm = false;
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
                // Печатаем "ready" для ЛЮБОГО настоящего драйвера (1..6,
                // включая wifi_driver — Фаза 4, Милстоун 4.1 — и usb_driver —
                // Фаза 14), чтобы он был виден в логе наравне с остальными.
                // Но в driver_ready[]/all_drivers_ready() по-прежнему
                // учитываются только 1..4 — wifi всё ещё экспериментальный
                // (см. RPI4_ENABLE_WIFI), USB опционален по определению
                // (ROADMAP.md Фаза 8) — зависание/провал их пробы не должно
                // блокировать загрузку остальных модулей и шелла.
                if (drv >= 1 && drv <= 6 && drv != 3) {
                    uart_puts("[ROOT] "); uart_puts(pcbs[sender_pid].name); uart_puts(" ready.\n");
                }
                if (drv == 5) {
                    g_wifi_driver_ready = true;
                }
                if (drv == 6) {
                    g_usb_driver_ready = true;
                }
                if (drv >= 1 && drv <= 4) {
                    driver_ready[drv] = true;

                    // Фаза 9.B (см. ROADMAP.md): автозапуск сервисов из
                    // /etc/init.conf — ровно в момент, когда все базовые
                    // драйверы (включая blk_driver — FAT32 уже смонтирован)
                    // впервые готовы, и ровно один раз (это событие в
                    // теории может прийти снова при respawn какого-то
                    // драйвера). ДО отпускания шелла ниже — сервисы успевают
                    // подняться раньше первого приглашения.
                    static bool init_services_started = false;
                    if (all_drivers_ready() && !init_services_started) {
                        init_services_started = true;
                        start_init_services(ep, med_ep, alloc, root_cnode, root_vspace, normal_untyped,
                                             blk_ep, console_ep, timer_ep, net_cmd_send_ep, vfs_mutex_ntfn);
                        // Фаза 4 плана "Сигналы драйверам" — та же точка,
                        // тот же гейт "ровно один раз, когда FAT32 уже
                        // смонтирован" (см. load_auto_restart_config()).
                        load_auto_restart_config(blk_ep);
                    }

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

                // issuse.txt №6 — раньше повторный SYS_SHM_GET безусловно
                // проходил весь путь ниже заново: новый vaddr из bump-
                // pointer'а, новые frame_copy-капы поверх shm_copies[9] — капы
                // ПЕРВОГО вызова просто терялись (их не освобождает ни этот
                // путь, ни SYS_EXIT/SYS_KILL/generic_recover_process(), они
                // смотрят только на ТЕКУЩИЙ snapshot). Единственный живой
                // повторный вызывающий — shell-команда `shm` (после
                // автоматического SHM_GET в sys_client_init()) — ожидает
                // рабочий доступ к ТОЙ ЖЕ памяти, а не новую область, так что
                // просто отдаём уже выданный vaddr повторно, идемпотентно.
                if (pcbs[pid].has_shm) {
                    seL4_ARM_Page_GetAddress_t res = seL4_ARM_Page_GetAddress(shm_frames[0]);
                    seL4_SetMR(0, pcbs[pid].shm_vaddr);
                    seL4_SetMR(1, res.paddr);
                    seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 2));
                    break;
                }

                if (!shm_role_has_any_pages(pcbs[pid].is_driver)) {
                    // Fail-closed (Фаза 5.2): этой роли не положено ни одной
                    // страницы — раньше вызывающий всё равно получал обратно
                    // ненулевой vaddr (просто "дыру", без реального маппинга
                    // за ней), что и есть fail-closed по факту доступа, но
                    // ломает контракт API (вызывающий не может отличить "SHM
                    // выдан" от "SHM не выдан" по одному только vaddr). Не
                    // трогаем bump-pointer и has_shm вообще — возвращаем 0,
                    // как в error-путях выше.
                    seL4_SetMR(0, 0);
                    seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                    break;
                }

                uintptr_t vaddr = pcbs[pid].vmap_bump_pointer;
                // Шаг фиксирован для ВСЕХ ролей, даже если роли положено меньше
                // страниц: только так смещение внутри SHM (например
                // VFS_PAYLOAD_OFFSET) означает одно и то же у всех, а не
                // зависит от того, кому что выдали.
                pcbs[pid].vmap_bump_pointer += (uintptr_t)SHM_TOTAL_PAGES * 0x1000;

                // Отмечаем, что этот процесс взял SHM
                pcbs[pid].has_shm = true;
                bool success = true;

                for (int i = 0; i < SHM_TOTAL_PAGES; i++) {
                    if (!shm_page_allowed_for_role(pcbs[pid].is_driver, i)) {
                        // Этой роли эта страница не положена (Фаза 5.2) — не
                        // мапим вообще, слот остаётся пустой дырой в VA-окне
                        // процесса; попытка обратиться туда даст честный
                        // Page Fault, а не тихий доступ по соглашению.
                        pcbs[pid].shm_copies[i] = 0;
                        continue;
                    }

                    seL4_CPtr frame_copy = alloc.alloc_slot();
                    if (frame_copy == 0) { success = false; pcbs[pid].shm_copies[i] = 0; }
                    else { pcbs[pid].shm_copies[i] = frame_copy; }

                    if (!success) break;

                    // 1. КОПИРУЕМ Capability (Обязательно для seL4). Фаза 5.3:
                    // там, где роль на этой странице только читает (см.
                    // shm_page_readonly_for_role() выше), минтим урезанную
                    // капу (read-only) вместо точной копии всех прав —
                    // seL4_CNode_Copy этого не умеет (сохраняет ВСЕ права
                    // источника), нужен именно Mint с явным seL4_CapRights_new.
                    seL4_Error err;
                    if (shm_page_readonly_for_role(pcbs[pid].is_driver, i)) {
                        err = seL4_CNode_Mint(
                            root_cnode, frame_copy, seL4_WordBits,
                            root_cnode, shm_frames[i], seL4_WordBits,
                            seL4_CapRights_new(0, 0, 1, 0), 0
                        );
                    } else {
                        err = seL4_CNode_Copy(
                            root_cnode, frame_copy, seL4_WordBits,
                            root_cnode, shm_frames[i], seL4_WordBits,
                            seL4_AllRights
                        );
                    }

                    if (err != seL4_NoError) { success = false; break; }

                    // ВОТ ОНО! Делегируем маппинг нашему On-Demand алгоритму.
                    // map_frame_robust() всегда просит seL4_AllRights при
                    // маппинге — это безопасно и для read-only копий выше:
                    // ядро обрезает реально применённые права пересечением с
                    // правами самой капы (maskVMRights), так что read-only
                    // frame_copy всё равно замапится read-only, что бы сюда
                    // ни передали.
                    if (!map_frame_robust(alloc, pcbs[pid], frame_copy, pcbs[pid].vspace, vaddr + (i * 0x1000), normal_untyped, root_cnode)) {
                        success = false; break;
                    }
                }

                if (!success) {
                    uart_puts("[ROOT] FATAL: Failed to dynamically map 36KB SHM!\n");
                    for (int i = 0; i < SHM_TOTAL_PAGES; i++) {
                        if (pcbs[pid].shm_copies[i] != 0) {
                            seL4_CNode_Delete(root_cnode, pcbs[pid].shm_copies[i], seL4_WordBits);
                            alloc.free(pcbs[pid].shm_copies[i]);
                        }
                    }
                    pcbs[pid].has_shm = false;
                    pcbs[pid].vmap_bump_pointer -= 0x9000;
                    seL4_SetMR(0, 0);
                    seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                    break;
                }

                // Успех! Возвращаем адреса драйверу
                pcbs[pid].shm_vaddr = vaddr; // issuse.txt №6 — для идемпотентного повтора выше
                seL4_ARM_Page_GetAddress_t res = seL4_ARM_Page_GetAddress(shm_frames[0]);
                seL4_SetMR(0, vaddr);
                seL4_SetMR(1, res.paddr);
                seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 2));
                break;
            }

            case SYS_RECOVER: {
                // issuse.txt №3 — в отличие от SYS_KILL (та же власть над
                // чужим процессом — форс-респавн вместо suspend), здесь не
                // было проверки прав вызывающего вообще: любой exec'нутый
                // процесс, даже недоверенный (is_driver==254), мог форсировать
                // респавн любого процесса по имени.
                if (!is_admin_caller(sender_pid)) {
                    seL4_SetMR(0, (seL4_Word)-2);
                    seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                    break;
                }
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

            // Интроспекция процесса по имени — для автотестов (см.
            // common.h/SYS_PROC_INFO и src/tests/coretest.cpp). Разбор
            // человекочитаемой таблицы `ps`/`top` из SHM для этого не
            // годится: формат меняется при любой правке вывода, а тест
            // обязан давать один и тот же ответ каждый раз. Только
            // чтение, ничего не меняет — admin-гейта нет, как и у `ps`.
            case SYS_PROC_INFO: {
                char info_name[32] = {0};
                uint64_t* info_name_ptr = (uint64_t*)info_name;
                info_name_ptr[0] = seL4_GetMR(1);
                info_name_ptr[1] = seL4_GetMR(2);
                info_name_ptr[2] = seL4_GetMR(3);
                info_name_ptr[3] = seL4_GetMR(4);
                info_name[31] = '\0';

                int info_pid = -1;
                for (int i = 1; i < 256; i++) {
                    if (pcbs[i].active && strcmp(pcbs[i].name, info_name) == 0) { info_pid = i; break; }
                }
                if (info_pid < 0) {
                    seL4_SetMR(0, (seL4_Word)-1);
                    seL4_SetMR(1, 0);
                    seL4_SetMR(2, 0);
                    seL4_SetMR(3, DRIVER_STATE_UNKNOWN);
                } else {
                    int info_is_driver = pcbs[info_pid].is_driver;
                    seL4_Word info_state = DRIVER_STATE_UNKNOWN;
                    if (g_driver_state_page != nullptr && info_is_driver >= 1 && info_is_driver <= 6) {
                        info_state = driver_state_read(info_is_driver);
                    }
                    seL4_SetMR(0, (seL4_Word)info_pid);
                    seL4_SetMR(1, (seL4_Word)pcbs[info_pid].core);
                    seL4_SetMR(2, (seL4_Word)info_is_driver);
                    seL4_SetMR(3, info_state);
                }
                seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 4));
                break;
            }

            case SYS_DRIVER_SIGNAL: {
                // План "Сигналы драйверам" — лёгкая альтернатива
                // generic_recover_process() для случая "драйвер жив,
                // просто нужно переинициализировать железо". Структура
                // 1:1 с SYS_RECOVER выше (admin-check + поиск по имени),
                // разница — вместо kill+respawn форвардим сигнал НА
                // командный endpoint самого драйвера, он обрабатывает
                // сам, без потери капабилити.
                if (!is_admin_caller(sender_pid)) {
                    seL4_SetMR(0, (seL4_Word)-2);
                    seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                    break;
                }
                char sig_driver_name[32] = {0};
                uint64_t* sig_name_ptr = (uint64_t*)sig_driver_name;
                sig_name_ptr[0] = seL4_GetMR(1);
                sig_name_ptr[1] = seL4_GetMR(2);
                sig_name_ptr[2] = seL4_GetMR(3);
                sig_name_ptr[3] = seL4_GetMR(4);
                sig_driver_name[31] = '\0';
                seL4_Word sig = seL4_GetMR(5);

                // wifi_driver: bring-up не факторизован в вызываемую
                // функцию (см. план) — "сигнал" здесь это уже
                // hw-проверенный kill+respawn путь (root_stop_wifi_driver/
                // root_start_wifi_driver), а не форвард на её endpoint.
                if (strcmp(sig_driver_name, "wifi_driver") == 0) {
                    seL4_Word status = (seL4_Word)-3;
                    if (sig == DRIVER_SIGNAL_STOP) {
                        root_stop_wifi_driver(ep, med_ep, alloc, root_cnode, root_vspace, normal_untyped, shm_frames[0],
                                               console_ep, timer_ep, blk_ep, &status);
                    } else if (sig == DRIVER_SIGNAL_START) {
                        root_start_wifi_driver(ep, med_ep, alloc, root_cnode, root_vspace, normal_untyped, shm_frames[0],
                                                console_ep, timer_ep, blk_ep, wifi_wake_ntfn, wifi_irq_ntfn, wifi_sdio_frame,
                                                wifi_cmd_recv_ep, net_wifi_rx_badged, vfs_mutex_ntfn, wifi_liveness_badged, &status);
                    } else if (sig == DRIVER_SIGNAL_RESTART) {
                        seL4_Word stop_status = (seL4_Word)-1;
                        root_stop_wifi_driver(ep, med_ep, alloc, root_cnode, root_vspace, normal_untyped, shm_frames[0],
                                               console_ep, timer_ep, blk_ep, &stop_status);
                        root_start_wifi_driver(ep, med_ep, alloc, root_cnode, root_vspace, normal_untyped, shm_frames[0],
                                                console_ep, timer_ep, blk_ep, wifi_wake_ntfn, wifi_irq_ntfn, wifi_sdio_frame,
                                                wifi_cmd_recv_ep, net_wifi_rx_badged, vfs_mutex_ntfn, wifi_liveness_badged, &status);
                    }
                    seL4_SetMR(0, status);
                    seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                    break;
                }

                int sig_target_pid = -1;
                for (int i = 1; i < 256; i++) {
                    if (pcbs[i].active && strcmp(pcbs[i].name, sig_driver_name) == 0) { sig_target_pid = i; break; }
                }
                if (sig_target_pid == -1) {
                    seL4_SetMR(0, (seL4_Word)-1);
                    seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                    break;
                }

                // Фаза 5 (опционально, см. план) — uart/timer подключены
                // последними: STOP/START есть у обоих, RESTART у обоих —
                // заглушка (нечего переинициализировать, см. сами
                // драйверы). timer_driver — единственный источник
                // SYS_SLEEP_MS/heartbeat для всей системы; STOP там
                // печатает явное предупреждение сама (см. timer_driver.cpp)
                // — здесь, на уровне маршрутизации, никаких дополнительных
                // ограничений не вводим (admin-check выше уже отсекает
                // untrusted-вызывающих).
                seL4_CPtr sig_target_ep = 0;
                switch (pcbs[sig_target_pid].is_driver) {
                    case 1: sig_target_ep = console_ep; break;
                    case 2: sig_target_ep = timer_ep; break;
                    case 3: sig_target_ep = blk_ep; break;
                    case 4: sig_target_ep = net_cmd_send_ep; break;
                    case 6: sig_target_ep = usb_cmd_ep; break;
                    default: sig_target_ep = 0; break;
                }
                if (sig_target_ep == 0) {
                    seL4_SetMR(0, (seL4_Word)-3);
                    seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                    break;
                }

                // issuse.txt №74/б — форвард это обычный блокирующий
                // seL4_Call. Пока драйверы жили на ядре 0, зависший драйвер
                // означал зависшее ядро root'а, и вопрос не стоял. Теперь
                // драйвер может висеть на СВОЁМ ядре, оставив root живым —
                // и этот Call утащил бы root за ним. Ждём PARKED (просто
                // занятый драйвер припаркуется в пределах бюджета), не
                // дождались — честно отвечаем ошибкой вместо зависания.
                {
                    int sig_is_driver = pcbs[sig_target_pid].is_driver;
                    if (sig_is_driver >= 1 && sig_is_driver <= 6 && pcbs[sig_target_pid].core != 0 &&
                        !wait_for_driver_parked(sig_is_driver, timer_ep, DRIVER_PARK_WAIT_BUDGET_MS)) {
                        uart_puts("[ROOT] SYS_DRIVER_SIGNAL: "); uart_puts(sig_driver_name);
                        uart_puts(" не отвечает (не встал в PARKED) — сигнал НЕ отправлен, иначе root повис бы на seL4_Call.\n");
                        seL4_SetMR(0, (seL4_Word)-4);
                        seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                        break;
                    }
                }

                seL4_SetMR(0, SYS_DRIVER_SIGNAL);
                seL4_SetMR(1, sig);
                seL4_Call(sig_target_ep, seL4_MessageInfo_new(0, 0, 0, 2));
                seL4_Word sig_driver_status = seL4_GetMR(0);
                seL4_SetMR(0, sig_driver_status);
                seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                break;
            }

            // Ручной жизненный цикл Wi-Fi (см. common.h — почему wifi_driver
            // больше не спавнится при загрузке). Все три сисколла ниже не
            // принимают имя процесса — рутсервер и так знает, что речь про
            // "wifi_driver" (единственный процесс с этим именем).
            case SYS_START_WIFI: {
                // Тело вынесено в root_start_wifi_driver() (см. план
                // "Сигналы драйверам") — переиспользуется и новым
                // SYS_DRIVER_SIGNAL для wifi_driver.
                seL4_Word status = (seL4_Word)-1;
                root_start_wifi_driver(ep, med_ep, alloc, root_cnode, root_vspace, normal_untyped, shm_frames[0],
                                        console_ep, timer_ep, blk_ep, wifi_wake_ntfn, wifi_irq_ntfn, wifi_sdio_frame,
                                        wifi_cmd_recv_ep, net_wifi_rx_badged, vfs_mutex_ntfn, wifi_liveness_badged, &status);
                seL4_SetMR(0, status);
                seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                break;
            }

            case SYS_STOP_WIFI: {
                // Тело вынесено в root_stop_wifi_driver() — см. комментарий у SYS_START_WIFI выше.
                seL4_Word status = (seL4_Word)-1;
                root_stop_wifi_driver(ep, med_ep, alloc, root_cnode, root_vspace, normal_untyped, shm_frames[0],
                                       console_ep, timer_ep, blk_ep, &status);
                seL4_SetMR(0, status);
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

            case SYS_USB_LIST: { // Фаза 14 (USB) — root-опосредованный проброс к usb_driver,
                                  // тот же приём, что seL4_Call(net_cmd_send_ep, ...) внутри
                                  // collect_load_snapshot() выше. usb_driver гарантированно
                                  // доходит до своего Recv-цикла за ограниченное время (весь
                                  // bring-up — с таймаутами на каждый шаг, см. usb_driver.cpp/
                                  // ROADMAP.md) — синхронный seL4_Call здесь безопасен.
                if (usb_cmd_ep == 0) {
                    seL4_SetMR(0, 0); // found=0 — USB выключен (RPI4_ENABLE_USB=false)
                    seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                    break;
                }
                // issuse.txt №74/б — синхронный seL4_Call в зависший
                // драйвер утащил бы root за собой (и вместе с ним всю
                // систему). Комментарий выше утверждал, что usb_driver
                // "гарантированно доходит до Recv за ограниченное время" —
                // живой прогон 2026-09-05 показал, что не гарантированно:
                // аппаратный стопор PCIe останавливает исполнение кода
                // драйвера полностью, никакой его собственный таймаут при
                // этом не срабатывает. Спрашиваем страницу состояния.
                if (!wait_for_driver_parked(6, timer_ep, DRIVER_PARK_WAIT_BUDGET_MS)) {
                    seL4_SetMR(0, 0); // found=0 — драйвер не отвечает
                    seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                    break;
                }
                seL4_SetMR(0, 1); // 1 = USB_CMD_LIST (см. usb_driver.cpp)
                seL4_Call(usb_cmd_ep, seL4_MessageInfo_new(0, 0, 0, 1));
                seL4_Word found = seL4_GetMR(0), vendor = seL4_GetMR(1), product = seL4_GetMR(2);
                seL4_Word dclass = seL4_GetMR(3), dsub = seL4_GetMR(4), dproto = seL4_GetMR(5);
                seL4_SetMR(0, found); seL4_SetMR(1, vendor); seL4_SetMR(2, product);
                seL4_SetMR(3, dclass); seL4_SetMR(4, dsub); seL4_SetMR(5, dproto);
                seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 6));
                break;
            }

            case SYS_FREE_STATS: { // Фаза 8 (мониторинг ресурсов) — `free`, см. common.h.
                                    // Все данные уже в g_ram_bytes_used/g_ram_bytes_total —
                                    // похода к драйверам не нужно, тот же SHM-текстовый
                                    // приём, что SYS_TOP_STATS выше.
                int active_procs = 1; // rootserver (тот же учёт, что `ps`/`top`)
                for (int i = 1; i < 256; i++) if (pcbs[i].active) active_procs++;

                uint64_t total_mb = g_ram_bytes_total / (1024 * 1024);
                uint64_t used_mb = g_ram_bytes_used / (1024 * 1024);
                uint64_t free_mb = (g_ram_bytes_total > g_ram_bytes_used) ? (g_ram_bytes_total - g_ram_bytes_used) / (1024 * 1024) : 0;

                char *shm = rootserver_shm_base;
                int offset = 0;

                strcpy(shm, "Total:  "); offset = strlen(shm);
                offset += append_udec_width(shm + offset, total_mb, 6);
                strcpy(shm + offset, " МиБ\n"); offset = strlen(shm);

                strcpy(shm + offset, "Used:   "); offset = strlen(shm);
                offset += append_udec_width(shm + offset, used_mb, 6);
                strcpy(shm + offset, " МиБ\n"); offset = strlen(shm);

                strcpy(shm + offset, "Free:   "); offset = strlen(shm);
                offset += append_udec_width(shm + offset, free_mb, 6);
                strcpy(shm + offset, " МиБ\n"); offset = strlen(shm);

                strcpy(shm + offset, "Процессов активно: "); offset = strlen(shm);
                offset += append_udec(shm + offset, (uint64_t)active_procs);
                strcpy(shm + offset, "\n"); offset = strlen(shm);

                flush_rootserver_shm();
                seL4_SetMR(0, 0);
                seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                break;
            }

            case SYS_DF_STATS: { // Фаза 8 (мониторинг ресурсов) — `df`[-h], см. common.h.
                                  // Root-опосредованный агрегатор: SD-карта (blk_ep,
                                  // синхронный seL4_Call — тот же приём безопасности,
                                  // что у SYS_USB_LIST выше) + все смонтированные тома
                                  // за usb_driver'ом (usb_cmd_ep, если USB включён).
                                  // MR1 (см. sbin/df.cpp) — 1, если передан флаг -h.
                seL4_Word human = (seL4_MessageInfo_get_length(recv_info) >= 2) ? seL4_GetMR(1) : 0;

                char *shm = rootserver_shm_base;
                int offset = 0;

                // Заголовок и позиции колонок — 1-в-1 с реальным `df -h`
                // (coreutils), см. df_format_row() выше. Английские
                // подписи не просто стиль — на чисто-ASCII байты и
                // видимые символы совпадают всегда, никакой UTF-8-ширины
                // считать не нужно (кириллица была не только некрасивой,
                // но и источником самого бага с шириной колонок).
                offset += append_str_left_width(shm + offset, "Filesystem", 29);
                // 6/6/7 — согласовано с шириной колонок в df_format_row()
                // (расширено там же ради суффикса "M" у не-human вывода).
                offset += append_str_width(shm + offset, "Size", 6);
                strcpy(shm + offset, "  "); offset += 2;
                offset += append_str_width(shm + offset, "Used", 6);
                strcpy(shm + offset, " "); offset += 1;
                offset += append_str_width(shm + offset, "Avail", 7);
                strcpy(shm + offset, " "); offset += 1;
                offset += append_str_width(shm + offset, "Use%", 4);
                strcpy(shm + offset, " "); offset += 1;
                strcpy(shm + offset, "Mounted on\n"); offset = strlen(shm);

                seL4_SetMR(0, SYS_GET_FS_SPACE);
                seL4_Call(blk_ep, seL4_MessageInfo_new(0, 0, 0, 1));
                uint64_t sd_total = seL4_GetMR(0), sd_free = seL4_GetMR(1);
                offset += df_format_row(shm + offset, "/ (SD)", sd_total, sd_free, "/", human != 0);

                // Тот же гейт, что у SYS_USB_LIST выше: зависший usb_driver
                // не должен уносить root'а через обычный `df`.
                if (usb_cmd_ep != 0 && wait_for_driver_parked(6, timer_ep, DRIVER_PARK_WAIT_BUDGET_MS)) {
                    seL4_SetMR(0, 4); // 4 = USB_CMD_GET_ALL_SPACE, см. usb_driver.cpp
                    seL4_Call(usb_cmd_ep, seL4_MessageInfo_new(0, 0, 0, 1));
                    seL4_Word mask = seL4_GetMR(0);
                    for (int i = 0; i < USB_MAX_DEVICES; i++) {
                        if (!(mask & (1u << i))) continue;
                        char name[32];
                        seL4_Word *words = (seL4_Word*)name;
                        int base = 1 + i * 6;
                        for (int w = 0; w < 4; w++) words[w] = seL4_GetMR(base + w);
                        name[31] = '\0';
                        uint64_t total = seL4_GetMR(base + 4);
                        uint64_t free_bytes = seL4_GetMR(base + 5);

                        char mnt[40];
                        int mlen = 0;
                        const char *prefix = "/mnt/";
                        while (prefix[mlen]) { mnt[mlen] = prefix[mlen]; mlen++; }
                        int k = 0;
                        while (name[k] && mlen < (int)sizeof(mnt) - 1) { mnt[mlen++] = name[k++]; }
                        mnt[mlen] = '\0';

                        offset += df_format_row(shm + offset, name, total, free_bytes, mnt, human != 0);
                    }
                }

                flush_rootserver_shm();
                seL4_SetMR(0, 0);
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

            case SYS_PROXY_WRITE_FILE: { // Фаза 5.4, см. common.h — узкий файловый доступ для exec-процессов без SHM
                int pid = sender_badge;
                if (pid <= 0 || pid >= 256 || !pcbs[pid].active) {
                    seL4_SetMR(0, (seL4_Word)-1);
                    seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                    break;
                }
                uint32_t path_len = seL4_GetMR(1);
                uint32_t data_len = seL4_GetMR(2);
                if (path_len > 63) path_len = 63;
                if (data_len > 100) data_len = 100; // тот же запас на кадр, что у sys_puts/остальных чанкованных syscall'ов
                // issuse.txt №4 — 63+100 клэмпы по отдельности не мешали сумме
                // 3+path_len+data_len доходить до 166, читая seL4_GetMR() за
                // границей seL4_MsgMaxLength=120 (соседние поля IPC-буфера).
                if (3 + path_len + data_len > seL4_MsgMaxLength) {
                    data_len = (path_len + 3 <= seL4_MsgMaxLength) ? (seL4_MsgMaxLength - 3 - path_len) : 0;
                }

                char path[64];
                for (uint32_t i = 0; i < path_len; i++) path[i] = (char)seL4_GetMR(3 + i);
                path[path_len] = '\0';

                // Тот же приём, что load_elf_from_disk() выше: собственный
                // scratch root'а в rootserver_shm_base (путь на 0, данные на
                // 128 — та же раскладка, что и остальной VFS-протокол), затем
                // обычный seL4_Call к blk_driver его штатной командой.
                char* shm = rootserver_shm_base;
                strncpy(shm, path, 63);
                shm[63] = '\0';
                for (uint32_t i = 0; i < data_len; i++) shm[128 + i] = (char)seL4_GetMR(3 + path_len + i);
                flush_rootserver_shm();

                seL4_SetMR(0, 113); // SYS_WRITE_FILE (blk_driver.cpp)
                seL4_SetMR(1, data_len);
                seL4_Call(blk_ep, seL4_MessageInfo_new(0, 0, 0, 2));
                seL4_Word status = seL4_GetMR(0);

                seL4_SetMR(0, status);
                seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                break;
            }

            case SYS_PROXY_READ_FILE: { // Фаза 5.4, см. common.h
                int pid = sender_badge;
                if (pid <= 0 || pid >= 256 || !pcbs[pid].active) {
                    seL4_SetMR(0, (seL4_Word)-1);
                    seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                    break;
                }
                uint32_t path_len = seL4_GetMR(1);
                uint32_t offset = seL4_GetMR(2);
                if (path_len > 63) path_len = 63;

                char path[64];
                for (uint32_t i = 0; i < path_len; i++) path[i] = (char)seL4_GetMR(3 + i);
                path[path_len] = '\0';

                char* shm = rootserver_shm_base;
                strncpy(shm, path, 63);
                shm[63] = '\0';
                flush_rootserver_shm();

                seL4_SetMR(0, 119); // SYS_READ_FILE (blk_driver.cpp)
                seL4_SetMR(1, offset);
                seL4_Call(blk_ep, seL4_MessageInfo_new(0, 0, 0, 2));
                seL4_Word status = seL4_GetMR(0);
                int32_t bytes_read = (int32_t)seL4_GetMR(1);
                if (status != 0 || bytes_read < 0) bytes_read = 0;
                if (bytes_read > 100) bytes_read = 100;
                if (status == 0 && bytes_read > 0) flush_rootserver_shm(); // читаем то, что blk_driver только что записал некэшируемо

                seL4_SetMR(0, status);
                seL4_SetMR(1, (seL4_Word)bytes_read);
                for (int32_t i = 0; i < bytes_read; i++) seL4_SetMR(2 + i, (seL4_Word)(uint8_t)shm[VFS_PAYLOAD_OFFSET + i]);
                seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 2 + bytes_read));
                break;
            }

            default:
                seL4_SetMR(0, (seL4_Word)-1); seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                break;
        }
    }
    return 0;
}