#pragma once
#include <stdio.h>
#include <stdint.h>

extern "C" {
#include <sel4/sel4.h>
}

void __assert_fail(const char *assertion, const char *file, int line, const char *function);

enum BootIPCSlot {
    BOOT_CONSOLE_EP = 100,
    BOOT_TIMER_EP   = 101,
    BOOT_NET_EP     = 102,
    BOOT_ROOT_EP    = 103,
    BOOT_IRQ_EP     = 104,
    BOOT_WIFI_EP    = 105, // wifi_driver слушает команды шелла (см. ROADMAP.md Фаза 4)
    BOOT_MBOX_BUF_PADDR = 106, // физический адрес приватного буфера timer_driver под VideoCore mailbox (Фаза 4.6, см. ROADMAP.md/main.cpp)
    BOOT_HEARTBEAT_NTFN_CAP = 107, // капа, которой timer_driver сигналит net_driver'у периодический будильник (Фаза 4.5, см. NET_EVENT_HEARTBEAT ниже)
    BOOT_BLK_DMA_PADDR = 108, // физический адрес приватного некэшируемого DMA bounce-буфера blk_driver (Фаза 4.5/ADMA2, см. PLAT_BLK_DMA_VADDR/main.cpp)
    // Фаза 4.5 (Wi-Fi data-plane, см. ROADMAP.md/situation.txt) — три новых
    // слота под тот же принцип, что и BOOT_HEARTBEAT_NTFN_CAP выше, только
    // для новых межпроцессных нотификаций net_driver<->wifi_driver:
    BOOT_WIFI_HEARTBEAT_NTFN_CAP = 111, // капа, которой timer_driver сигналит wifi_driver'у периодический опрос SDIO data-канала (читает ТОЛЬКО wifi_driver, см. WIFI_EVENT_HEARTBEAT ниже)
    BOOT_WIFI_NET_RX_SIGNAL_CAP  = 112, // капа (badge NET_EVENT_WIFI_RX, минтится из net_event_ntfn), которой wifi_driver сигналит net_driver'у "кадр в RX-mailbox" — читает ТОЛЬКО wifi_driver, передаётся заново при каждом "wifi start" (см. wifi_cmd_recv_ep)
    BOOT_WIFI_TX_WAKE_CAP        = 113, // капа (badge WIFI_EVENT_TX_READY, минтится из wifi_wake_ntfn), которой net_driver сигналит wifi_driver'у "кадр в TX-mailbox" — читает ТОЛЬКО net_driver
    // Фикс живого зависания blk_driver (см. situation.txt): blk_driver
    // блокировался на seL4_Wait без таймаута, ожидая EMMC-прерывание — если
    // карта его пропустит, зависает навсегда, а вместе с ним и root (см.
    // load_elf_from_disk() — синхронный вызов из обработчика root'а). Тот
    // же принцип, что и BOOT_WIFI_HEARTBEAT_NTFN_CAP выше — timer_driver
    // периодически сигналит доп. badged-копию ТОГО ЖЕ notification-объекта,
    // на котором blk_driver уже блокируется (g_emmc_irq_ntfn), так что
    // seL4_Wait гарантированно просыпается каждые ~100мс независимо от
    // реального железного IRQ.
    BOOT_BLK_HEARTBEAT_NTFN_CAP  = 114,
    // Фикс дедлока (см. situation.txt): notify_root_irq_handled() раньше
    // звал root СИНХРОННЫМ seL4_Call (SYS_MMC_IRQ_ACK), чтобы root вызвал
    // seL4_IRQHandler_Ack() — но если ИМЕННО root является текущим
    // синхронным вызывающим blk_driver (SYS_EXEC -> load_elf_from_disk()),
    // root уже заблокирован в ожидании ЭТОГО САМОГО ответа, и обратный
    // вызов к нему мертво блокируется (root ждёт blk_driver, blk_driver
    // ждёт root). blk_driver получает СОБСТВЕННУЮ копию capability на
    // IRQHandler и вызывает Ack() сам, без какого-либо IPC к root —
    // безопасно, т.к. к этому моменту девайсный статус-бит уже снят (см.
    // emmc_wait_irpt_bit).
    BOOT_MMC_IRQ_HANDLER_CAP     = 115,
    // Фикс задержки (см. situation.txt): переход read/write на multi-block
    // (CMD18/25) — физический адрес ВТОРОЙ приватной страницы blk_driver'а
    // (только под ADMA2-дескриптор; данные больше не помещаются рядом с ним
    // в одной странице, см. main.cpp/blk_dma_frame2_param).
    BOOT_BLK_DMA2_PADDR          = 116,
    // Фаза 6 (SMP, см. ROADMAP.md): общий межпроцессный мьютекс на
    // нотификации для VFS-прокси staging области в SHM (офсет 4096) —
    // заменяет старые non-atomic busy-spin локи (vfs_lock/net_vfs_lock/
    // wifi_vfs_lock), которые были безопасны только на одном ядре. Капа
    // читается shell/net_driver/wifi_driver — тот же самый объект у всех
    // троих, без бейджа (чистый mutex, различать источник не нужно).
    BOOT_VFS_MUTEX_NTFN_CAP      = 117,
    // Фаза 6.1 (продолжение, см. ROADMAP.md): собственная копия TCB-капы
    // процесса на самого себя — нужна uart/blk/net/wifi (is_driver
    // 1/3/4/5), чтобы при просьбе root'а "финализируй бенчмарк и отдай
    // свой total/idle" вызвать seL4_BenchmarkGetThreadUtilisation(себя) —
    // единственный способ получить ЧЕСТНЫЙ total/idle СВОЕГО ядра (это поле
    // в ядре всегда берётся от ВЫЗЫВАЮЩЕГО, не от того, чей TCB спрашивают
    // — root, вызывая с ядра 0, иначе всегда получал бы период ядра 0).
    BOOT_SELF_TCB_CAP            = 118,
    // Фаза 14 (USB, xHCI) — usb_driver слушает команды шелла (тот же
    // принцип, что BOOT_WIFI_EP/BOOT_NET_EP выше), плюс физические адреса
    // приватных DMA-страниц (см. PLAT_XHCI_*_VADDR в platform.h — виртуальные
    // адреса фиксированы на этапе компиляции, физические решает root при
    // Retype в spawn_process() и передаёт здесь, та же схема, что
    // BOOT_BLK_DMA_PADDR).
    BOOT_USB_EP                       = 119,
    // issuse.txt №59: 120..126 сюда специально НЕ ставим. seL4_IPCBuffer —
    // это `tag; msg[120]; userData; caps_or_badges[3]; receiveCNode;
    // receiveIndex; receiveDepth;` (см. kernel/libsel4/include/sel4/
    // shared_types.h) — 128 слов ровно. msg[N] физически лежит по смещению
    // (N+1) слов от начала структуры, поэтому msg[120..126] — это не
    // "продолжение сообщения", а буквально userData/caps_or_badges[0..2]/
    // receiveCNode/receiveIndex/receiveDepth САМОГО ядра. Раньше 7
    // констант ниже (DCBAA..SCRATCHPAD_ARR) стояли на 120..126 и писали
    // прямо в эти поля — молча "работало" только потому, что usb_driver.cpp
    // никогда не читает caps_or_badges[]/не зовёт seL4_SetCapReceivePath.
    // msg[127] и дальше — уже настоящая свободная память СРАЗУ ПОСЛЕ конца
    // структуры (структура ровно 1024 байта = 2×512, ipc-буфер сидит на
    // 1024-выровненном адресе внутри 4КиБ-страницы — см. main.cpp,
    // child_ipc_ptr = ipc_temp_vaddr+2048), поэтому 127..167 ниже — НЕ
    // трогаем, они безопасны и так. Сами 7 констант переехали ниже, после
    // BOOT_USB_HEARTBEAT_NTFN_CAP (168..174) — свободная зона с тем же
    // запасом.
    BOOT_USB_SCRATCHPAD_COUNT         = 127, // не капа — просто seL4_Word N (см. USB_MAX_SCRATCHPAD_PAGES)
    BOOT_USB_SCRATCHPAD_BUF0_PADDR    = 128, // первый из N (<= USB_MAX_SCRATCHPAD_PAGES) подряд идущих слотов 128..(128+N-1)
    // Фаза 14 (закрытие, Mass Storage) — Milestone 1: настоящий EP0 Transfer
    // Ring (было: заглушка на Device Context, см. platform.h
    // PLAT_XHCI_EP0_TRRING_VADDR). Первый свободный слот после
    // scratchpad-диапазона (128..159).
    BOOT_USB_EP0_TRRING_PADDR         = 160,
    // Milestone 2 — буфер данных control-transfer'ов на EP0 (см.
    // PLAT_XHCI_CTRL_BUF_VADDR/platform.h).
    BOOT_USB_CTRL_BUF_PADDR           = 161,
    // Milestone 4 — Transfer Ring'и bulk OUT/IN эндпоинтов (см.
    // PLAT_XHCI_BULKOUT_TRRING_VADDR/PLAT_XHCI_BULKIN_TRRING_VADDR).
    BOOT_USB_BULKOUT_TRRING_PADDR     = 162,
    BOOT_USB_BULKIN_TRRING_PADDR      = 163,
    // Milestone 5 — CBW/CSW-страница и SCSI-данные bounce-буфер (см.
    // PLAT_XHCI_CBW_CSW_VADDR/PLAT_XHCI_BOUNCE_VADDR).
    BOOT_USB_CBW_CSW_PADDR            = 164,
    BOOT_USB_BOUNCE_PADDR             = 165,
    // Milestone 9 — клиентский Call-капа на usb_driver'ов командный
    // endpoint (та же send-капа, что root держит как usb_cmd_ep,
    // см. main.cpp), для VFS-команд (110/112/.../120, Milestone 8) на
    // /mnt/usb0. НЕ то же самое, что BOOT_USB_EP=119 — тот usb_driver сам
    // слушает (CanRead-только копия). Выдаётся только shell(0) и
    // доверенным /sbin(253), тот же принцип, что BOOT_BLK_EP.
    BOOT_USB_STORAGE_EP               = 166,
    // Milestone 11 (доп., по запросу пользователя) — badged-копия
    // usb_irq_ntfn (badge USB_EVENT_HEARTBEAT, см. выше) — читает ТОЛЬКО
    // timer_driver (is_driver==2), тот же принцип, что
    // BOOT_BLK_HEARTBEAT_NTFN_CAP.
    BOOT_USB_HEARTBEAT_NTFN_CAP        = 167,
    // issuse.txt №59 — см. комментарий у BOOT_USB_EP выше: эти 7 констант
    // раньше стояли на 120..126 и алиасили userData/caps_or_badges[]/
    // receive* самого seL4_IPCBuffer. Перенесены сюда, в свободную зону
    // сразу после конца используемого диапазона (168..174 — тот же
    // страничный запас, что уже безопасно использует 127..167).
    BOOT_USB_DCBAA_PADDR              = 168,
    BOOT_USB_CMDRING_PADDR            = 169,
    BOOT_USB_ERST_PADDR               = 170,
    BOOT_USB_EVTRING_PADDR            = 171,
    BOOT_USB_DEVCTX_PADDR             = 172,
    BOOT_USB_INPUTCTX_PADDR           = 173,
    BOOT_USB_SCRATCHPAD_ARR_PADDR     = 174,
    // Фаза 3b плана "Сигналы драйверам" — badged-копия root'ового
    // mmc_shared_irq_ntfn (см. DRIVER_LIVENESS_*_BADGE выше), которой сам
    // драйвер сигналит root'у "я жив" — ОБЫЧНОЕ копирование capability (не
    // TCB-bind, читает СВОЙ local-слот и зовёт seL4_Signal() сам, тот же
    // принцип, что и остальные *_NTFN_CAP выше, только направление сигнала
    // обратное — драйвер -> root). Продолжение той же свободной зоны 168-174.
    BOOT_BLK_LIVENESS_NTFN_CAP        = 175,
    BOOT_NET_LIVENESS_NTFN_CAP        = 176,
    BOOT_WIFI_LIVENESS_NTFN_CAP       = 177,
    BOOT_USB_LIVENESS_NTFN_CAP        = 178,
    // Фаза 3b — badged-копия blk_liveness_tick_ntfn (badge
    // BLK_LIVENESS_TICK_BADGE, см. выше) — читает ТОЛЬКО timer_driver
    // (is_driver==2), тот же принцип, что BOOT_BLK_HEARTBEAT_NTFN_CAP, но на
    // отдельном, не разделяемом ни с чем объекте (см. константу выше).
    BOOT_BLK_LIVENESS_TICK_NTFN_CAP   = 179,
};

// Фаза 9.A (см. ROADMAP.md): индекс msg[]-слова (НЕ капа, НЕ CNode-слот —
// отдельная нумерация), куда root кладёт cwd вызывающего шелла для
// доверенной /sbin-утилиты (is_driver==253) при SYS_EXEC. Раньше это
// передавалось через общую VFS SHM-страницу — оказалось, что
// load_elf_from_disk() (main.cpp) читает файл ЧЕРЕЗ ТУ ЖЕ физическую
// страницу (rootserver_shm_base) ПОСЛЕ того, как шелл туда написал cwd, но
// ДО того, как ребёнок успевает его прочитать — затирает содержимое
// байтами .elf-файла (найдено по факту на живом железе: "ls: directory not
// found" — cwd оказался мусором). msg[0..7] заняты именем+аргументами
// exec (см. args_payload/strcpy в spawn_process), msg[100+] — boot-капы;
// 8 слов (64 байта) начиная с 16 — safe zone между ними.
constexpr int EXEC_CWD_MSG_SLOT = 16;

// Общий IRQ 158 (EMMC2 + Wi-Fi SDIO — одна физическая GIC-линия на обоих
// контроллерах, см. platform.h/ROADMAP.md 4.5) слушает САМ root, а не
// какой-то конкретный драйвер — только один процесс вообще может держать
// IRQHandler-капу на этот номер. ПРОБОВАЛИ (см. situation.txt) TCB-bind
// напрямую к blk_driver, чтобы обойти задержку root-инициированных чтений
// (root не крутит свой Recv-цикл, пока сам синхронно ждёт ответа от
// blk_driver, и не может вовремя отрелеить реальное прерывание) — ОТКАЧЕНО:
// вызвало катастрофический регресс (чтение WiFi-прошивки: 800мс -> 27с),
// т.к. blk_driver копит пендинг-сигналы от каждого реального завершения
// EMMC-команды, пока сам занят обработкой (не в Recv/Wait), и следующий
// seL4_Recv в его главном цикле перехватывается этим накопленным сигналом
// вместо реального клиентского запроса. Схема снова: root релеит ОБОИМ
// процессам (blk_irq_ntfn/wifi_irq_ntfn), каждый сам проверяет свой
// статусный регистр. Badge заведомо вне диапазонов PID (1-255) и пайпов
// (1000-1015), чтобы не путаться с обычными сообщениями в главном цикле root'а.
constexpr seL4_Word IRQ_MMC_SHARED_BADGE = 2000;

// Бейдж badged-копии blk_irq_ntfn, которую держит timer_driver (см.
// BOOT_BLK_HEARTBEAT_NTFN_CAP выше) — конкретное значение некритично,
// blk_driver не различает бейджи на этой нотификации (перечитывает
// статусный регистр на любое пробуждение независимо от причины), но
// именованная константа лучше магического числа в main.cpp.
constexpr seL4_Word BLK_HEARTBEAT_BADGE = 0x8000;

// Фаза 4.5 (событийный GENET RX + периодический будильник для net_driver,
// см. ROADMAP.md) — биты нотификации net_driver'а. Оба минтятся из ОДНОГО
// net_event_ntfn (см. main.cpp) разными источниками (GENET IRQHandler и
// timer_driver соответственно) — seL4 при накоплении неснятых сигналов на
// одном объекте OR'ит бейджи, поэтому оба бита должны быть непересекающимися
// степенями двойки и заведомо вне диапазона обычных PID (1-255) и пайпов
// (1000-1015, см. PIPE_BASE_BADGE в main.cpp), чтобы не спутать событие с
// настоящим клиентским сообщением на net_cmd_ep.
constexpr seL4_Word NET_EVENT_GENET_RX  = 0x1000; // пришёл кадр (см. RPI4_GENET_IRQ_A/UMAC_IRQ_RXDMA_DONE)
constexpr seL4_Word NET_EVENT_HEARTBEAT = 0x2000; // периодическая проверка DHCP/ARP/ping/link-таймаутов (см. timer_driver.cpp)
// Фаза 4.5 (Wi-Fi data-plane) — третий бейдж того же net_event_ntfn: пришёл
// кадр от wifi_driver (см. WIFI_SHM_RX_LEN_OFFSET в net_driver.cpp), сигналит
// сам wifi_driver через badge NET_EVENT_WIFI_RX (BOOT_WIFI_NET_RX_SIGNAL_CAP).
constexpr seL4_Word NET_EVENT_WIFI_RX   = 0x4000;

// Фаза 4.5 (Wi-Fi data-plane) — биты СОБСТВЕННОЙ нотификации wifi_driver'а
// (wifi_wake_ntfn в main.cpp), тот же принцип OR'а бейджей одного объекта,
// что и у NET_EVENT_* выше, но в отдельном (не пересекающемся с net_driver)
// пространстве — эти два badge'а сравниваются только между собой и с
// PID/пайп-бейджами my_ep САМОГО wifi_driver, никогда не с NET_EVENT_*.
constexpr seL4_Word WIFI_EVENT_HEARTBEAT = 0x1000; // периодический опрос SDIO data-канала на входящие 802.11-кадры (см. BOOT_WIFI_HEARTBEAT_NTFN_CAP)
constexpr seL4_Word WIFI_EVENT_TX_READY  = 0x2000; // net_driver положил кадр в TX-mailbox — разбудить сразу, не ждать heartbeat (см. BOOT_WIFI_TX_WAKE_CAP)

// Фаза 14 (USB, xHCI) — собственный badge usb_driver'а (своё, не
// пересекающееся с NET_EVENT_*/WIFI_EVENT_* пространство, тот же принцип).
// Один источник (собственная, не шаренная SPI-линия — см. RPI4_XHCI_IRQ),
// поэтому один бит, без OR нескольких источников, как у net_driver/
// wifi_driver.
constexpr seL4_Word USB_EVENT_XHCI_IRQ = 0x1000;
// Milestone 11 (доп., по запросу пользователя) — периодический опрос
// PORTSC на подключение/отключение (Port Status Change Event на живом
// железе не подтвердился, см. ROADMAP.md — bulk-передача на "старом"
// слоте после переподключения провалилась, т.е. асинхронная доставка
// либо не пришла, либо обработка не успела до следующей команды).
// Опрос — та же badged-нотификация usb_irq_ntfn, что и XHCI IRQ выше
// (см. main.cpp), просто другой бит; timer_driver сигналит его на общем
// heartbeat-тике (см. BOOT_USB_HEARTBEAT_NTFN_CAP).
constexpr seL4_Word USB_EVENT_HEARTBEAT = 0x2000;

// НАЙДЕНО НА ЖИВОМ ЖЕЛЕЗЕ (см. ROADMAP.md, зависание после `kill 1`):
// клавиатурная нотификация uart_driver'а (badged-копия uart_ntfn,
// см. main.cpp) минтилась с бейджем 1 — буквально валидным PID. Пока
// uart_driver жив с самого старта, слот PID=1 навсегда занят им самим,
// коллизии не было. Но после watchdog-респавна uart_driver (PID=1
// освобождается, новый инстанс получает уже другой, больший PID) слот 1
// снова становится свободным и рано или поздно достаётся ОБЫЧНОМУ
// клиентскому процессу — его собственный badged console_ep (минтится с
// badge=PID) оказывается РАВЕН 1, и `if (badge == 1)` в uart_driver.cpp
// принимает его настоящий IPC-вызов (SYS_PUTS и т.п.) за клавиатурное
// прерывание: молча "обрабатывает" (тихо ничего не делает, ACK'ает IRQ) и
// НЕ отвечает — вызывающий процесс навсегда виснет в `seL4_Call`, а вместе
// с ним и шелл, ждущий его через `sys_wait`. Тот же принцип, что уже
// применён для IRQ_MMC_SHARED_BADGE/NET_EVENT_*/WIFI_EVENT_* выше — бейдж
// заведомо вне диапазона PID (1-255) и пайпов (1000-1015).
constexpr seL4_Word UART_KBD_IRQ_BADGE = 0x8001;

// Тот же баг, вторая жертва: timer_driver слушает клиентские SYS_SLEEP-
// вызовы (badge=PID) И собственную нотификацию физического таймера на ОДНОМ
// и том же timer_ep — нотификация тоже минтилась с бейджем 1. uart_driver
// (is_driver==1) — единственный процесс, чей исходный PID может освободиться
// и достаться обычному клиенту после watchdog-респавна; как только это
// происходит, следующий клиент с PID==1, позвавший sys_sleep, попадал бы в
// ту же ловушку. См. UART_KBD_IRQ_BADGE выше.
constexpr seL4_Word TIMER_IRQ_BADGE = 0x8002;

// Фаза 3b плана "Сигналы драйверам" (heartbeat-watchdog) — blk_driver
// единственный из мониторимых драйверов БЕЗ уже существующего периодического
// тика в своём главном диспетчер-цикле (net/usb/wifi уже потребляют
// HEARTBEAT-биты на СВОИХ TCB-bound нотификациях). Раньше у blk_driver ТОЖЕ
// был heartbeat-driven seL4_Wait (см. BLK_HEARTBEAT_BADGE выше), но его
// убрали — 20мс-гранулярность добавляла задержку поверх реальных multi-block
// EMMC-передач (см. emmc_wait_irpt_bit() в blk_driver.cpp). BLK_HEARTBEAT_BADGE
// и объект, на котором он живёт (blk_irq_ntfn/g_emmc_irq_ntfn), НАМЕРЕННО не
// переиспользуются здесь — тот же объект ещё и разделяет общую линию
// EMMC2/Wi-Fi SDIO (см. IRQ_MMC_SHARED_BADGE), а blk_driver сейчас держит на
// него только СЫРУЮ (небейджированную) капу без TCB-bind; связывать с ним
// новый тик рисковало бы смешать в один pending-бейдж настоящий IRQ (badge=0
// при сигнале от root) и тик (после OR'а неотличимо от "просто тик") — тот
// же класс проблемы, что уже документирован у IRQ_MMC_SHARED_BADGE. Вместо
// этого — полностью ОТДЕЛЬНЫЙ, выделенный notification-объект
// (blk_liveness_tick_ntfn, см. main.cpp), TCB-bind на blk_driver (пустовавший
// irq_ntfn-параметр его spawn_process()), сигналится ТОЛЬКО timer_driver'ом
// на общем heartbeat-тике — единственный источник, единственный бейдж,
// никакого разделения с чем-либо ещё.
constexpr seL4_Word BLK_LIVENESS_TICK_BADGE = 0x8003;

// Фаза 3b — "обратное" направление: badged-копии СОБСТВЕННОГО
// mmc_shared_irq_ntfn root'а (см. main.cpp), которыми КАЖДЫЙ из 4
// мониторимых драйверов (blk/net/wifi/usb) сам сигналит root'у "я жив" на
// каждом обработанном heartbeat-тике — root обновляет last_seen и по
// таймауту (WATCHDOG_TIMEOUT_MS) вызывает generic_recover_process(). ≥0x10000
// — заведомо выше диапазонов PID (1-255), пайпов (1000-1015) И битового
// паттерна IRQ_MMC_SHARED_BADGE=2000=0x7D0 (биты 4,6,7,8,9,10, максимум
// 0x7FF) — непересекающиеся одиночные биты, безопасны для битового OR друг с
// другом и с IRQ_MMC_SHARED_BADGE на одном объекте (см. main.cpp, тот же
// приём проверки "sender_badge >= порог && (sender_badge & бит)", что уже
// применён в Фазе 3a). uart_driver/timer_driver сюда намеренно не входят
// (см. план — uart без heartbeat-тика, timer не может обнаружить собственное
// зависание через тик, который сам же производит).
constexpr seL4_Word DRIVER_LIVENESS_BLK_BADGE  = 0x10000;
constexpr seL4_Word DRIVER_LIVENESS_NET_BADGE  = 0x20000;
constexpr seL4_Word DRIVER_LIVENESS_WIFI_BADGE = 0x40000;
constexpr seL4_Word DRIVER_LIVENESS_USB_BADGE  = 0x80000;

// Слот CSpace процесса, в который ядро минтит capability активного пайпа
// (см. SYS_PIPE/SYS_PIPE_CLOSE в main.cpp и запрос пайпа в shell.cpp).
// Должен отличаться от зарезервированных local_* слотов в main.cpp::spawn_process
// (console=1, timer=2, net_send=3, irq=4, net_recv=5, wifi_send=6, blk=7,
// wifi_recv=8, self_cnode=11, syscall=10) — раньше здесь был захардкожен слот 3,
// что уничтожало net_send_ep при закрытии пайпа.
constexpr seL4_Word PIPE_FD_SLOT = 20;

// Слот CSpace процесса, в который main.cpp минтит capability на СОБСТВЕННЫЙ
// (child) CNode процесса — не капа на какой-то endpoint внутри него, а на сам
// CNode целиком. Нужна для seL4_CNode_SaveCaller()/seL4_CNode_Delete() ВНУТРИ
// самого процесса, когда он хочет отложить IPC-reply вместо немедленного
// ответа (см. uart_driver.cpp SYS_READ — Фаза 4.5, ROADMAP.md; тот же приём,
// что main.cpp уже использует для СЕБЯ через seL4_CapInitThreadCNode).
constexpr seL4_Word SELF_CNODE_SLOT = 11;

// issuse.txt №69 — тот же приём, что UART_PENDING_REPLY_SLOT в
// uart_driver.cpp, но для СИНХРОННЫХ VFS-команд blk_driver.cpp/
// usb_driver.cpp: SaveCaller ПЕРЕД обработкой КАЖДОЙ VFS-команды (не только
// когда реально нечего ответить сразу, как у SYS_READ) кладёт reply-капу
// текущего клиента в этот фиксированный слот СВОЕГО CNode на всё время
// обработки. Если driver умирает/зависает посреди обработки, root (см.
// generic_recover_process() в main.cpp) успевает вытащить эту капу из
// CNode жертвы ДО его уничтожения и ответить клиенту сам, с ошибкой —
// вместо того чтобы клиент завис навсегда (обычный seL4_Call не даёт
// root'у такой возможности). 27 — следующий свободный слот после старших
// local_*-констант spawn_process() (см. main.cpp, up to local_blk_
// liveness_tick=26) — сознательно НЕ 12 (как у UART_PENDING_REPLY_SLOT),
// хотя коллизии не было бы (CNode каждого процесса свой, приватный) —
// просто чтобы не наводить на мысль о связи с uart-конкретной логикой.
constexpr seL4_Word VFS_PENDING_REPLY_SLOT = 27;

// Сисколл rootserver'у: "моя синхронная инициализация завершена, я готов
// обслуживать запросы". Каждый драйвер шлет его один раз перед входом в
// свой главный while(1); rootserver ждет этот сигнал между spawn_process()
// соседних драйверов (см. main.cpp), поэтому порядок готовности определяется
// самим порядком вызовов spawn_process(), а не отдельным списком где-то еще.
// Специально НЕ используется для "лучше-стараться" фоновых задач (например,
// первого NTP-синка в net_driver) — иначе загрузка могла бы зависнуть
// навсегда при недоступной сети.
constexpr seL4_Word SYS_DRIVER_READY = 109;

// Сисколл шелла: "заблокируй меня, пока не готовы ВСЕ драйверы (см.
// SYS_DRIVER_READY выше)". Шелл шлет его один раз при старте, до печати
// собственного баннера/приглашения — так его "sandbox[N] />" оказывается
// в логе строго после логов инициализации остальных модулей, без ручной
// синхронизации порядка где-либо еще.
constexpr seL4_Word SYS_WAIT_ALL_DRIVERS_READY = 110;

// wifi_driver больше не запускается при загрузке (см. ROADMAP.md/main.cpp —
// подозрение на гонку мапинга/таймингов при одновременном спавне с
// остальными драйверами, из-за которой изредка не успевал возникать
// готовый sdpcm-канал). Теперь его жизненным циклом управляет шелл через
// команду "wifi start/stop/restart" — три новых сисколла ниже, по образцу
// SYS_EXEC/SYS_KILL/SYS_RECOVER, но специфичные для wifi_driver (не требуют
// передачи имени процесса, рутсервер и так знает, кого спавнить/убивать).
constexpr seL4_Word SYS_START_WIFI  = 130; // спавнит wifi_driver, если ещё не запущен
constexpr seL4_Word SYS_STOP_WIFI   = 131; // убивает wifi_driver БЕЗ автореспавна
constexpr seL4_Word SYS_WIFI_STATUS = 132; // MR0: 0=не запущен, 1=запущен но не готов, 2=готов принимать команды

// Фаза 4.5 (см. ROADMAP.md): blk_driver зовёт это ПОСЛЕ того, как реально
// сбросил статусный бит в EMMC_INTERRUPT (device-level), а НЕ root сразу по
// получении сигнала. Общий IRQ 158 (EMMC2/Wi-Fi SDIO) level-triggered — GIC
// не доставит его ЗАНОВО, пока не будет явный seL4_IRQHandler_Ack; если бы
// root Ack'ал сразу (как раньше), а девайсный бит ещё не сброшен (blk_driver
// просто ещё не успел выполниться), GIC увидел бы линию всё ещё asserted и
// мгновенно повторил бы доставку — root (priority 255, выше любого ребёнка)
// зацикливался бы на "разбудили -> Ack -> тут же снова разбудили", ни разу
// не дав планировщику дойти до blk_driver (priority 254), который единственный
// может реально снять бит. Поэтому Ack откладывается до этого явного вызова.
constexpr seL4_Word SYS_MMC_IRQ_ACK = 133;

// Фаза 4.5 (см. ROADMAP.md) — тот же приём, что SYS_MMC_IRQ_ACK выше, но со
// стороны wifi_driver: общий IRQ 158 (EMMC2/Wi-Fi SDIO) теперь будит ОБА
// процесса на каждое срабатывание (root не знает заранее, чей это статус-
// бит — см. main.cpp), и root не Ack'ает GIC, пока КАЖДАЯ сторона, у которой
// реально был выставлен свой бит, не подтвердит, что сняла его. wifi_driver
// зовёт это ПОСЛЕ того, как реально сбросил I_HMB_FRAME_IND/I_HMB_HOST_INT в
// SDPCMD_INTSTATUS чипа (см. sdpcm_wait_and_read_ctrl в wifi_driver.cpp) —
// если у него самого бит не был выставлен (проснулся по чужому, EMMC2-
// событию), звонить сюда не нужно вообще, blk_driver сам вызовет
// SYS_MMC_IRQ_ACK для своей части.
constexpr seL4_Word SYS_WIFI_IRQ_ACK = 134;

// Фаза 5.4 (least-privilege, см. situation.txt/ROADMAP.md Фаза 5): узкий
// файловый доступ для exec-процессов (is_driver=254), которые с Фазы 5.2
// не получают ни одной страницы SHM — root выполняет операцию от их имени
// (тем же приёмом, что уже использует load_elf_from_disk(): собственный
// scratch в rootserver_shm_base + seL4_Call к blk_driver), путь и данные
// приходят прямо в MR, никакого SHM у вызывающего не требуется.
// MR1.. = путь (до 63 байт, нуль-терминированный, упакован по байту на слово
// — простота важнее плотности, это редкий/маленький вызов).
constexpr seL4_Word SYS_PROXY_READ_FILE  = 135; // MR1..=путь; ответ: MR0=статус(0=ok), MR1=длина, MR2..=данные (до ~100 байт за вызов)
constexpr seL4_Word SYS_PROXY_WRITE_FILE = 136; // MR1=длина данных, MR2..=путь+данные упакованы см. main.cpp; ответ: MR0=статус

// Фаза 6.1 (SMP, см. ROADMAP.md): ручной перенос уже запущенного процесса на
// другое ядро в рантайме (команда шелла `taskset <pid> <ядро>`). MR1=pid,
// MR2=целевое ядро (0..3). Ответ: MR0=статус (0=ok, иначе код отказа — см.
// таблицу в main.cpp: root/timer_driver нельзя переносить никогда, см.
// найденную PPI-опасность в ROADMAP Фазы 6.1).
constexpr seL4_Word SYS_SET_AFFINITY = 137;
// Разовый снимок нагрузки (команда шелла `top`) — без параметров. Ответ:
// MR0=статус; текстовая таблица (общая нагрузка по ядрам + PID/NAME/CORE/%CPU)
// пишется в rootserver_shm_base, тем же путём, что SYS_PS.
constexpr seL4_Word SYS_TOP_STATS = 138;

// Фаза 6.1 (продолжение): "вызови у себя seL4_BenchmarkResetLog() и ответь" —
// root не может включить учёт benchmark utilisation НА ДРУГОМ ядре сам
// (per-core состояние в ядре seL4), поэтому просит представителя (любой
// активный uart/blk/net/wifi процесс на этом ядре) сделать это самому.
// Общая для uart_driver/blk_driver (разбирают этот SYS_*-неймспейс
// напрямую по числу) — net_driver/wifi_driver используют собственные
// NetCommand/WIFI_CMD_* и получают свою локальную константу того же
// назначения (см. net_driver.cpp/wifi_driver.cpp). Без параметров, ответ:
// MR0=0.
constexpr seL4_Word SYS_BENCHMARK_RESET_LOCAL = 139;

// Фаза 6.1 (продолжение): "вызови у себя seL4_BenchmarkFinalizeLog(), затем
// seL4_BenchmarkGetThreadUtilisation(себя) и отдай МОЙ idle/total" —
// вызывается ПОСЛЕ сна (300мс), парой к SYS_BENCHMARK_RESET_LOCAL выше.
// Без этого root, читая total/idle через СВОЙ (ядро 0) вызов
// GetThreadUtilisation, получал бы total С ЯДРА 0 (это поле в ядре всегда
// от вызывающего, не от того, чей TCB спрашивают) — а не честный период
// того ядра, где реально исполняется представитель (найдено по факту:
// агрегат по ядру 1 совсем не сходился с суммой процессов на нём). Нужна
// собственная TCB-капа (см. BOOT_SELF_TCB_CAP) — без параметров, ответ:
// MR0=idle (BENCHMARK_IDLE_LOCALCPU_UTILISATION), MR1=total
// (BENCHMARK_TOTAL_UTILISATION), оба — 4 и 9 в
// benchmark_track_util_ipc_index.
constexpr seL4_Word SYS_BENCHMARK_FINALIZE_LOCAL = 140;

// Фаза 6.1 (продолжение): команда шелла `balance` — без параметров. Снимает
// ту же нагрузку, что и `top` (collect_load_snapshot()), находит САМОЕ
// занятое ядро с >=2 резидентами, оставляет на нём "тяжёлый" (максимальный
// %CPU) процесс, а всех остальных резидентов (кроме root/timer_driver —
// те же две защиты, что у SYS_SET_AFFINITY) раскидывает по наименее
// загруженным другим ядрам. Ответ: MR0=0, текстовый отчёт — в
// rootserver_shm_base, тем же путём, что SYS_TOP_STATS/SYS_PS.
constexpr seL4_Word SYS_BALANCE = 141;

// issuse.txt: убитый/восстанавливаемый watchdog'ом процесс мог оставить
// ОТЛОЖЕННЫЙ reply-cap в другом драйвере (uart_driver — SYS_READ ждёт
// ввода, timer_driver — SYS_SLEEP_MS ждёт дедлайна) — когда тот реально
// срабатывает, драйвер пытается ответить по капе, указывающей на уже
// не существующий TCB, и ловит "Attempted to invoke a null cap" (безвредный
// debug-принт ядра, но грязный и маскирующий реальные проблемы). root шлёт
// эту команду в generic_recover_process() ДО того, как убирает саму жертву
// — MR1=pid жертвы; драйвер молча отбрасывает свой отложенный слот (без
// seL4_Send — отвечать уже некому), если он принадлежал именно этому pid.
// Отправляется КАЖДОМУ драйверу, у которого есть такой механизм (сейчас:
// uart_driver, timer_driver) — если слот жертве не принадлежал, драйвер
// просто отвечает 0 и ничего не делает. Ответ: MR0=0 всегда.
constexpr seL4_Word SYS_CANCEL_PENDING_FOR_PID = 142;

// Фаза 7 (DVFS) — shell шлёт это ПРИ КАЖДОЙ новой введённой команде (MR1 =
// текущий аптайм в мс, см. sys_get_uptime()); root просто запоминает момент
// последней активности (g_last_shell_activity_ms) и использует его в
// SYS_BALANCE, чтобы понять "система реально простаивает" НЕ через
// seL4_BenchmarkGetThreadUtilisation (который считает busy-poll драйверов
// как занятость, см. issuse.txt) — а через честный признак "давно не было
// нового пользовательского ввода". Ответ: MR0=0 всегда.
constexpr seL4_Word SYS_MARK_SHELL_ACTIVITY = 143;

// Фаза 14 (USB, xHCI) — root-опосредованный запрос (тот же принцип, что
// SYS_WIFI_STATUS/SYS_TOP_STATS: любой процесс зовёт это на своём обычном
// syscall endpoint, root сам внутри синхронно вызывает usb_driver и
// пересылает результат — см. collect_load_snapshot() для точно такого же
// приёма). Данные (нашли ли устройство, vendor/product ID, класс) —
// единственный слот этой фазы, умещаются прямо в message registers,
// отдельная SHM-передача не нужна. Ответ: MR0=found(0/1), MR1=vendor_id,
// MR2=product_id, MR3=device_class, MR4=device_subclass, MR5=device_protocol.
constexpr seL4_Word SYS_USB_LIST = 144;

// Фаза 8 (мониторинг ресурсов) — `free`. Root-обслуживаемый, БЕЗ похода к
// драйверам (счётчик g_ram_bytes_used/g_ram_bytes_total уже живёт в
// main.cpp) — тот же SHM-текстовый приём, что SYS_TOP_STATS. Ответ:
// MR0=0, текст в rootserver_shm_base.
constexpr seL4_Word SYS_FREE_STATS = 145;

// Фаза 8 — `df`, ОДИН SD-накопитель (blk_driver.cpp), добавлена в тот же
// каскад cmd==110/112/.../120. Ответ: MR0=total_bytes, MR1=free_bytes
// (seL4_Word 64-битный на AArch64 — uint64_t умещается в одно слово).
constexpr seL4_Word SYS_GET_FS_SPACE = 146;

// Фаза 8 — `df`, root-опосредованный агрегатор (тот же приём, что
// SYS_USB_LIST): зовёт blk_ep с SYS_GET_FS_SPACE и (если включён)
// usb_cmd_ep с локальной командой USB_CMD_GET_ALL_SPACE (см.
// usb_driver.cpp), собирает текстовую таблицу в rootserver_shm_base.
// Ответ: MR0=0, текст в SHM.
constexpr seL4_Word SYS_DF_STATS = 147;

// Концепция сигналов драйверам (см. план — "Сигналы драйверам") —
// лёгкая альтернатива kill+full-respawn для случая "драйвер жив,
// просто нужно переинициализировать железо". MR1-4 = имя драйвера
// (32 байта, та же упаковка, что SYS_RECOVER), MR5 = тип сигнала
// (0=STOP, 1=START, 2=RESTART). Ответ MR0: 0=ok, -1=не найден,
// -2=admin-check не прошёл, -3=не поддержано для этого драйвера сейчас.
// Root форвардит РОВНО ТОТ ЖЕ номер и MR1=тип сигнала на командный
// endpoint целевого драйвера (см. main.cpp) — драйвер сам решает, что
// значит каждый сигнал для его железа.
constexpr seL4_Word SYS_DRIVER_SIGNAL = 148;
constexpr int DRIVER_SIGNAL_STOP = 0;
constexpr int DRIVER_SIGNAL_START = 1;
constexpr int DRIVER_SIGNAL_RESTART = 2;

// issuse.txt №69 (регрессионный тест, см. sbin/tests/killwindow.cpp) —
// сентинел-offset для SYS_READ_FILE (119), которого НИКОГДА не бывает в
// реальном chunked-протоколе (шелл/cat/timedread всегда идут от 0 вверх
// кратно 4096, а реальные файлы столько не весят) — blk_driver.cpp/
// usb_driver.cpp узнают его и делают искусственную паузу ПОСЛЕ
// SaveCaller(VFS_PENDING_REPLY_SLOT), ДО реальной работы, вместо честного
// чтения — даёт гарантированное окно в несколько секунд, чтобы вручную (или
// скриптом) поймать `kill <pid>` driver'а РОВНО посреди обработки
// VFS-команды. Без этого сентинела момент "именно посреди обработки" (а не
// "пока driver простаивал на seL4_Recv") — не воспроизводимая на глаз гонка
// (окно — миллисекунды на реальный 4КБ chunk).
constexpr uint32_t KILL_WINDOW_TEST_OFFSET = 0xFFFFFFF0;
constexpr int KILL_WINDOW_TEST_DELAY_MS = 5000;

// issuse.txt №70 — vfs_lock()/vfs_unlock() (shell.cpp/h/sys_client.h) сами
// сообщают root'у, кто СЕЙЧАС держит глобальный vfs_mutex_ep (MR1: 1=взял,
// 0=отдал) — обычный fire-and-forget seL4_Send, БЕЗ reply (root просто
// обновляет g_vfs_lock_holder_pid и не отвечает, тот же приём, что уже есть
// у heartbeat-тиков/badge-нотификаций). Если владелец умирает/убивается
// (см. generic_recover_process() в main.cpp) ДО того, как успел сам
// вызвать vfs_unlock() (обычный seL4_Signal тут бессилен — убитый
// процесс никогда до него не дойдёт), root форсирует seL4_Signal сам —
// иначе мьютекс, будучи голым notification-семафором без владельца/
// таймаута/robust-семантики, остаётся захваченным НАВСЕГДА, вешая
// абсолютно ЛЮБУЮ будущую VFS-команду от ЛЮБОГО процесса в системе.
constexpr seL4_Word SYS_VFS_LOCK_NOTIFY = 149;

// issuse.txt №75 (доп.) — жёсткий reboot через тот же PM_WDOG/PM_RSTC,
// что уже "кормит" timer_driver (см. platform.h/PLAT_PM_*,
// timer_driver.cpp/pm_watchdog_kick_ticks()). Shell -> root (admin-check,
// как SYS_BALANCE/SYS_KILL) -> forward на timer_ep — единственный
// процесс, у которого вообще замаплены эти регистры. Взводит watchdog
// на минимальный таймаут (10 тиков, ~150мкс — тот же приём, что
// bcm2835_wdt_expire_now() в /home/nikita/u-boot/drivers/watchdog/
// bcm2835_wdt.c) вместо обычных 12с — чип перезагружает всю плату
// аппаратно, почти мгновенно.
constexpr seL4_Word SYS_REBOOT = 150;

const char* sel4_err_str(seL4_Error err);
void check_err(seL4_Error err, const char *msg);