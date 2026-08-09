#pragma once
#include <stdint.h>

// Имя хоста а так же имя сбоки, которое net_driver.cpp шлёт в DHCP-опции 12 (Host Name) —
// видно в списке клиентов роутера.
constexpr char DHCP_HOSTNAME[] = "SeL4-CrouN";

// --- Флаги отладочных логов по компонентам. Гасят только рутинные
// info/diagnostic-сообщения (регистровые дампы, "X initialized" и т.п.) —
// ошибки/предупреждения печатаются всегда, независимо от этих флагов.
// Включайте по одному, когда реально отлаживаете конкретный драйвер на
// живом железе — не держите все разом, иначе лог захламляется. ---
constexpr bool LOG_BRINGUP = false; // main.cpp: alloc_device_frame() дампы найденных untyped-регионов
constexpr bool LOG_UART    = false;
constexpr bool LOG_TIMER   = false;
constexpr bool LOG_BLK     = false; // blk_driver.cpp: пошаговые дампы регистров EMMC2 при инициализации
constexpr bool LOG_NET     = false;  // net_driver.cpp — самый свежий/менее обкатанный компонент
constexpr bool LOG_WIFI    = false;  // wifi_driver.cpp — новые команды (start/stop/scan/connect-lifecycle); включён по умолчанию, т.к. Wi-Fi всё ещё в активной отладке на живом железе
constexpr bool LOG_USB     = false; // usb_driver.cpp — пошаговый xHCI bring-up (Шаги 0-15) и DIAG-дампы; ошибки/предупреждения и сообщения hot-plug (Milestone 11) печатаются всегда, независимо от флага. B1-B4 (HS-хаб) hw-подтверждены полностью; SS-хаб (Milestone B5) — обнаружение/топология работают, адресация устройства за ним не работает (Transaction Error), отложено по решению пользователя — см. ROADMAP.md/память.

// --- Известные пути в пользовательской FAT-файловой системе. ВСЕГДА
// абсолютные (с ведущего '/'), чтобы не зависеть от current_dir_cluster —
// blk_driver.cpp делает mkdir+cd в "/root" при старте (см. USER_ROOT_DIR
// там же), поэтому ЛЮБОЙ относительный (без ведущего '/') путь, отправленный
// через SYS_READ_FILE/SYS_WRITE_FILE, резолвится уже ВНУТРИ "/root", а не в
// истинном корне FAT-раздела — где живут файлы загрузчика (config.txt,
// U-BOOT.BIN и т.п.). 
constexpr const char* PATH_NET_UDP_LOG   = "/root/net_udp.log";
// Фаза 9.B (см. ROADMAP.md): перенесено из "/wifi/..." в "/conf/wifi_conf/..."
// — тот же принцип, что /conf/balancer_conf, теперь и Wi-Fi-конфиги/прошивка
// живут под /conf. "/wifi" (mkdir нигде не было — только ручное соглашение
// на SD-карте) больше не используется, весь контент переехал.
constexpr const char* PATH_WIFI_FW       = "/conf/wifi_conf/wifi_fw.bin";
constexpr const char* PATH_WIFI_NVRAM    = "/conf/wifi_conf/wifi_nvram.txt";
constexpr const char* PATH_WIFI_CLM      = "/conf/wifi_conf/wifi_clm.bin"; // regulatory/channel-таблица, см. brcmf_c_process_clm_blob()
constexpr const char* PATH_WIFI_PQW      = "/conf/wifi_conf/pqw.txt"; // знакомые сети: строки "имясети|пароль"

// CLM blob download ("clmload" iovar, см. brcmf_c_download()/brcmf_c_process_clm_blob()
// в эталоне) — без него регуляторная таблица прошивки пуста, и даже валидный
// ccode вроде "US" в iovar "country" отвергается (BCME_BADARG), а escan без
// country вовсе падает с BCME_NOTUP (подтверждено на живом железе).
// ИСПРАВЛЕНО: эталонное значение MAX_CHUNK_LEN=1400 приводит к TX-кадру
// >512 байт, что на нашей SDIO-реализации требует block-mode CMD53 (F2)
// — путь, который до сих пор нигде в этом коде не использовался и не
// проверен (на живом железе первый же такой чанк вызвал "ответ не влезает
// в RX-буфер", похоже на эхо собственного TX-кадра, а не реальный ответ
// прошивки — вероятно, баг в обработке multi-block записи на этом
// SDIO-контроллере). Пока не разобрались, режем чанки заведомо мельче
// 512 байт целиком (кадр = 12 sdpcm + 8 имя "clmload\0" + 12 dload-hdr +
// чанк), чтобы гарантированно оставаться в уже проверенном byte-mode.
constexpr uint32_t MAX_CLM_CHUNK_LEN    = 256;
constexpr uint16_t DLOAD_HANDLER_VER    = 1;
constexpr uint16_t DLOAD_FLAG_VER_SHIFT = 12;
constexpr uint16_t DL_BEGIN             = 0x0002;
constexpr uint16_t DL_END               = 0x0004;
constexpr uint16_t DL_TYPE_CLM          = 2;

// =====================================================================
// Платформенно-зависимый слой.
//
// Целевая платформа — Raspberry Pi 4 (BCM2711), AArch64/ARMv8. Порт идёт
// поэтапно (см. ROADMAP.md, Фаза 3): UART уже переключён на реальные
// адреса железа — mini-UART (UART1), а не PL011 (UART0, физически отдан
// Bluetooth-модулю и требует либо dtoverlay=disable-bt в config.txt, либо
// ручного включения тактовой частоты через VideoCore mailbox — оба пути
// опробованы и оба не сработали на этой прошивке/сборке, см. историю в
// ROADMAP.md/README.md). Таймер (Фаза 3.1) портирован на ARM generic timer
// (не MMIO, см. ниже). Диск (Фаза 3.3) портирован на EMMC2. Сеть (Фаза 3.2)
// портирована на GENET v5 (virtio-mmio полностью убран, был только
// QEMU-плейсхолдером). Все 5 фаз Фазы 3 закрыты — остаётся только
// опциональная периферия (USB/Wi-Fi/GPIO/HDMI/аудио/PCIe, Фаза 3.5).
// Драйверы (main.cpp, uart_driver.cpp, timer_driver.cpp, blk_driver.cpp,
// net_driver.cpp) ссылаются только на эти константы, а не на "магические"
// адреса напрямую по коду.
// =====================================================================

// --- Физические адреса устройств (см. alloc_device_frame() в main.cpp) ---
// UART: используем mini-UART (UART1, AUX-периферия), а не PL011 (UART0).
// PL011 на реальной плате физически отдан Bluetooth-модулю и требует
// либо dtoverlay=disable-bt в config.txt (это ломает elfloader — см.
// историю отладки), либо ручного включения тактовой частоты через
// VideoCore mailbox (не удалось добиться ответа от mailbox на этой
// прошивке). mini-UART же тактуется от VPU-ядра всегда и УЖЕ физически
// работает на GPIO14/15 без какой-либо настройки с нашей стороны — это
// подтверждено: именно через него видны все логи U-Boot/ядра при живой
// загрузке. Адрес — база AUX-периферии (см. RPI4_UART1_MINIUART_PADDR
// ниже), а не сам mini-UART блок (0xfe215040) — тот не выровнен на
// страницу, а alloc_device_frame() умеет мапить только целые страницы.
constexpr uintptr_t PLAT_UART_PADDR        = 0xfe215000ULL;             // RPi4: AUX-периферия (mini-UART внутри, +0x40)

// Таймер (Фаза 3.1) не MMIO-устройство и не имеет физического адреса вовсе —
// ARM generic timer читается прямой mrs-инструкцией из EL0 (CNTVCT_EL0/
// CNTFRQ_EL0), см. hw_timer.cpp/timer_driver.cpp. PL031 (QEMU-плейсхолдер)
// и его адрес/оффсеты полностью убраны, на реальном железе им замены нет.

// Диск: EMMC2 (Arasan SDHCI), реальный контроллер SD-карты на RPi4 — заменяет
// virtio-blk (Фаза 3.3, см. RPI4_EMMC2_* ниже, откуда взят адрес/IRQ).
constexpr uintptr_t PLAT_EMMC_PADDR = 0xfe340000ULL;
constexpr int        PLAT_EMMC_IRQ  = 158;                              // Не используется (PIO/polling), но фиксируем для симметрии

// Сеть: GENET v5 (BCM2711 встроенный Ethernet MAC), реальный контроллер на
// RPi4 — заменяет virtio-net (Фаза 3.2, см. RPI4_GENET_* ниже, откуда взят
// адрес). Занимает 64KB (0x10000) — заметно больше EMMC2/UART.
constexpr uintptr_t PLAT_GENET_PADDR = 0xfd580000ULL;

// Термодатчик: AVS RO thermal (единственный температурный датчик BCM2711,
// доступный без прошивки VideoCore — см. RPI4_AVS_MONITOR_* ниже, откуда
// взят адрес). Живёт в том же процессе, что и таймер (timer_driver) — оба
// читают железо напрямую без DMA/IRQ, отдельный процесс не нужен.
constexpr uintptr_t PLAT_AVS_PADDR = 0xfd5d2000ULL;

// Wi-Fi (Фаза 4, Милстоун 4.1): сам SDIO-хост-контроллер, на котором висит
// BCM43455 (см. RPI4_WIFI_SDIO_* ниже, откуда взят адрес). Это НЕ тот же
// контроллер, что EMMC2 (PLAT_EMMC_PADDR) — DT compatible "brcm,bcm2835-sdhci"
// против "brcm,bcm2711-emmc2" у EMMC2, другой физический блок, но тот же
// стандартный SDHCI Simplified Spec регистровый layout (см. EMMC_* ниже —
// переиспользуются как есть, просто с этим PADDR). В этом милстоуне —
// только сам хост-контроллер + сырые SDIO-команды (CMD5/CMD52), без
// backplane/прошивки/сетевого протокола (см. ROADMAP.md Фаза 4).
constexpr uintptr_t PLAT_WIFI_SDIO_PADDR = 0xfe300000ULL;

// VideoCore mailbox (Фаза 4.6, расследование DVFS — см. ROADMAP.md): ARM
// property-tag интерфейс, единственный штатный способ менять частоту/
// напряжение ARM-ядер на BCM2711 (нет прямых регистров, в отличие от
// AVS-термодатчика выше). MAILBOX_BASE = PERIPHERAL_BASE + 0xB880 —
// НЕ выровнен на страницу (см. тот же приём для mini-UART в
// PLAT_UART_PADDR выше: страница берётся ниже по адресу, регистры
// считаются оффсетом от неё, не от начала страницы). Раньше (см.
// комментарии у RPI4_UART1_MINIUART_PADDR ниже) уже пробовали дёрнуть
// смену тактовой PL011 через этот канал — mailbox не ответил; текущий
// пробник (`mboxprobe` в шелле) проверяет менее специфичным тегом
// (GET_FIRMWARE_REVISION), чтобы понять — мёртв канал целиком, или
// не так был сформирован именно тот, более ранний запрос.
constexpr uintptr_t PLAT_MBOX_PADDR = 0xfe00b000ULL;             // страница, кратная 0x1000

// USB (Фаза 14): xHCI-контроллер (VL805). НЕ плоский DTB-алиас
// (0xfe9c0000/1MB, "generic-xhci") — живое железо показало, что этот
// адрес больше не отвечает после того, как U-Boot реально проэнумерировал
// PCI-шину. Реальный BAR0 — 0xC0000000 (см. RPI4_XHCI_PADDR ниже,
// "pci bar 01.00.00" на живом железе, ROADMAP.md) — но это PCI-BUS-SIDE
// адрес, НЕ CPU-физический: единственное DT-окно PCIe-моста (win=0)
// транслирует его в 0x600000000 ("высокое" 64-битное PCIe-окно,
// недоступное seL4). ПОПЫТКА перенацелить ЭТО САМОЕ (единственное) окно
// через правку DT вызвала настоящий краш U-Boot (см. ROADMAP.md
// "Тринадцатая попытка") — откачена. Пятнадцатая попытка: usb_driver САМ
// добавил ВТОРОЕ outbound-окно (индекс 1, см. PLAT_PCIE_RC_MISC_PADDR/
// usb_driver.cpp/step0_setup_outbound_window) уже ПОСЛЕ хендовера — окно
// программируется УСПЕШНО (read-back регистров совпадает с расчётом), но
// ПЕРВОЕ ЖЕ реальное чтение через него вызвало SError (асинхронный
// внешний abort) -> фатальный halt ВСЕГО ядра (CONFIG_AARCH64_SERROR_IGNORE
// выключен в этой сборке, см. kernel/src/arch/arm/64/traps.S/cur_el_serr;
// halt() дополнительно маскирует SError перед печатью — kernel/src/arch/
// arm/64/idle.c) — качественно другой (более тяжёлый) отказ, чем "мёртвые
// нули" из Девятой-Двенадцатой попыток (тогда 0xC0000000-как-CPU-адрес
// читал обычную RAM, вообще не касаясь PCIe — см. Попытку #8).
//
// JTAG-сессия (Шестнадцатая попытка, см. ROADMAP.md) ПОЙМАЛА реальный
// SError под GDB на входе в lower_el_serr: ESR_EL1=0xbf000002
// (EC=0x2F=SError, IDS=1, implementation-defined синдром=0x000002),
// ELR_EL1 указывал внутрь seL4_Reply — abort пришёл АСИНХРОННО, не
// синхронно с самим MMIO-чтением (ожидаемо для SError по архитектуре).
// PCIE_MISC_PCIE_STATUS проверен ДО крашащего чтения — PHYLINKUP=1,
// DL_ACTIVE=1 (линк жив, версия "ушёл в низкое энергопотребление" отпадает).
//
// Семнадцатая попытка: увеличенные PCIE_MISC_UBUS_TIMEOUT/
// RC_CONFIG_RETRY_TIMEOUT (те же значения, что U-Boot ставит для
// BCM2712) + подавление ошибок (PCIE_MISC_UBUS_CTRL ERR_DIS/DECERR_DIS,
// read-back подтвердил применение) — SError ВСЁ РАВНО произошёл, та же
// сигнатура. Гипотеза "просто короткий таймаут/неподавленная UBUS-
// ошибка" ОПРОВЕРГНУТА живым тестом.
//
// Восемнадцатая попытка — ПРОРЫВ: диагностика boot-info (main.cpp)
// показала, что 0x600000000 (ТО САМОЕ "высокое" 64-битное окно,
// которое U-Boot САМ настроил и которое РЕАЛЬНО работает — см. "PCIe
// BRCM: link up"/"USB XHCI 1.00" в каждом логе) ВСЕГДА БЫЛ доступен
// seL4 как device Untyped (idx=25, покрывает [16GB, 32GB), isDevice=1) —
// KernelPaddrUserTop для Cortex-A72 (kernel/src/arch/arm/config.cmake)
// это 1<<44 (16TB), device Untyped создаются на ЛЮБОЙ незарезервированный
// пробел ниже этой границы (kernel/src/kernel/boot.c:create_untypeds），
// НЕ по списку конкретных устройств из DTB. Более ранняя проверка
// (Попытка #8) была неполной — не архитектурный предел. Всё окно 1
// (Пятнадцатая-Семнадцатая попытки) было НЕ НУЖНО: bus-адрес окна 0
// (0xc0000000) РОВНО совпадает с BAR0 xHCI — значит нужный CPU-адрес
// это просто 0x600000000 + 0. Используем ГОТОВОЕ окно U-Boot'а
// напрямую, моста вообще не трогаем.
//
// Живой тест Восемнадцатой попытки: краша (SError) больше нет, но
// CAPLENGTH/HCSPARAMS* читались как чистый 0x00000000 — ТРЕТИЙ, ранее не
// встречавшийся паттерн (не крах, не классический PCI 0xFFFFFFFF). Причина
// (Девятнадцатая попытка, см. ROADMAP.md): usb_driver.cpp всё ещё активно
// (1) программировал ненужное окно 1 на ТОТ ЖЕ bus-диапазон 0xC0000000,
// которым уже владеет окно 0, и (2) держал включённым
// PCIE_MISC_UBUS_CTRL.DECERR_DIS (Семнадцатая попытка) — по определению
// регистра это "подавлять decode error, возвращать 0 вместо него". Оба
// источника шума удалены из usb_driver.cpp — следующее чтение через окно 0
// должно быть честным (не подавленным) сигналом.
constexpr uintptr_t PLAT_XHCI_PADDR = 0x600000000ULL;
constexpr uintptr_t PLAT_XHCI_SIZE  = 0x1000ULL;
constexpr int        PLAT_XHCI_IRQ  = 208;

// Пятнадцатая попытка (продолжение) — физическая страница, покрывающая
// группу регистров PCIE_MISC_CPU_2_PCIE_MEM_WIN0_* (0x400c..0x408f
// от RPI4_PCIE_PADDR, см. ниже) одной 4KB-страницей — конкретные смещения/
// маски см. в usb_driver.cpp (там же — почему НЕ переиспользуется формула
// из заголовка U-Boot). RPI4_PCIE_PADDR определён ниже по файлу (числовое
// значение продублировано здесь буквально, чтобы не тянуть forward-
// declaration): 0xfd500000 + 0x4000 = 0xfd504000.
constexpr uintptr_t PLAT_PCIE_RC_MISC_PADDR = 0xfd504000ULL;

// Двадцать четвёртая попытка (см. ROADMAP.md) — регистры сообщения об
// ошибках ИСХОДЯЩИХ (от моста НА AXI/UBUS, т.е. включая DMA устройства ->
// RAM — направление, в котором сейчас HSE) транзакций: PCIE_OUTB_ERR_VALID/
// ACC_INFO/MEM_CAUSE/MEM_ADDR_LO/HI (0x6004/0x600c/0x6020/0x6018/0x601c от
// RPI4_PCIE_PADDR) — сверено с апстрим Linux (drivers/pci/controller/
// pcie-brcmstb.c, brcm_pcie_dump_err()), U-Boot(наш) их не знает. Другая
// страница, чем PLAT_PCIE_RC_MISC_PADDR (0xfd504000..0xfd504fff) —
// 0xfd500000 + 0x6000 = 0xfd506000, требует отдельного фрейма/маппинга.
constexpr uintptr_t PLAT_PCIE_ERR_PADDR = 0xfd506000ULL;

// --- Виртуальные адреса, куда эти устройства маппятся в VSpace драйвера
// (см. hw_vaddr в spawn_process(), main.cpp). Общие для всех процессов —
// каждый драйвер видит свое устройство по одному и тому же литералу. ---
constexpr uintptr_t PLAT_UART_VADDR         = 0x200000000ULL;
constexpr uintptr_t PLAT_EMMC_VADDR         = 0x200006000ULL;
// Фаза 4.5/ADMA2 (см. ROADMAP.md) — приватный НЕКЭШИРУЕМЫЙ DMA-буфер
// blk_driver (одна страница, bounce-буфер для ADMA2-дескрипторов — та же
// схема, что PLAT_MBOX_BUF_VADDR ниже и SHM net_driver/wifi_driver для
// GENET). Свободный слот между PLAT_EMMC_VADDR (1 страница) и
// PLAT_GENET_VADDR — тот же 2MB-регион, drv_pud/pd/pt для is_driver==3 уже
// создаются в spawn_process(), отдельная иерархия страничных таблиц не нужна.
constexpr uintptr_t PLAT_BLK_DMA_VADDR      = 0x200007000ULL;
constexpr uintptr_t PLAT_GENET_VADDR        = 0x200008000ULL;
constexpr uintptr_t PLAT_AVS_VADDR          = 0x200019000ULL;
constexpr uintptr_t PLAT_WIFI_SDIO_VADDR    = 0x20001a000ULL;
constexpr uintptr_t PLAT_MBOX_VADDR         = 0x20001b000ULL;    // регистры mailbox (см. PLAT_MBOX_PADDR)
constexpr uintptr_t PLAT_MBOX_BUF_VADDR     = 0x20001c000ULL;    // приватный некэшируемый буфер под property-tag запрос (см. main.cpp)

// USB (Фаза 14) — отдельное, СОБСТВЕННОЕ 2MB-окно usb_driver'а (не тот же
// регион, что UART/EMMC/GENET/... выше). Изначально рассчитывалось на 1MB
// MMIO (см. RPI4_XHCI_SIZE выше — с тех пор исправлено на реальные 4KB,
// живое железо показало другой BAR/размер) + ~23 4KB-страницы DMA — с
// нынешним, ещё меньшим MMIO-окном запас по месту в 2MB стал ещё больше,
// пересчитывать раскладку смысла нет. Вся структура usb_driver'а по-прежнему
// укладывается в ОДНУ иерархию drv_pud/drv_pd/drv_pt (main.cpp), как у
// остальных драйверов. Адрес — вне диапазонов, которые root временно
// занимает СВОИМИ СОБСТВЕННЫМИ скользящими окнами (global_elf_temp_vaddr:
// 0x200100000-0x200200000, global_ipc_temp_vaddr: 0x200800000-0x200900000,
// main.cpp) — реальной коллизии не было бы (разные vspace/ASID), но
// смущает при чтении, поэтому просто берём отдельный, ничем не занятый
// диапазон.
constexpr uintptr_t PLAT_XHCI_VADDR = 0x201000000ULL;             // xHCI MMIO (см. PLAT_XHCI_SIZE)
constexpr uintptr_t PLAT_PCIE_RC_VADDR = 0x201001000ULL;          // PCIe RC MISC-регистры (см. PLAT_PCIE_RC_MISC_PADDR) — 1 страница сразу за xHCI MMIO, до DMA-страниц на 0x201100000 огромный запас
constexpr uintptr_t PLAT_PCIE_ERR_VADDR = 0x201002000ULL;         // PCIE_OUTB_ERR_* (см. PLAT_PCIE_ERR_PADDR) — ещё одна страница сразу за PLAT_PCIE_RC_VADDR

// Приватные некэшируемые DMA-страницы usb_driver'а, сразу после MMIO-окна
// (0x201000000 + 0x100000 = 0x201100000), в ТОМ ЖЕ 2MB-окне — та же схема,
// что PLAT_BLK_DMA_VADDR у blk_driver (по одной странице на структуру, без
// общего "DMA-пула", см. ROADMAP.md). Каждый xHCI-ring segment — ровно одна
// 4KB-страница (256 TRB по 16 байт), длинные ring'и наращиваются Link TRB,
// а не многостраничным непрерывным DMA-регионом. Scratchpad — отдельный
// диапазон подряд идущих страниц, число реально используемых читается из
// HCSPARAMS2 в рантайме (см. USB_MAX_SCRATCHPAD_PAGES ниже).
// Фаза 15 (несколько накопителей + Hub Class, см. ROADMAP.md/план) —
// USB_MAX_DEVICES независимых "слотов" под накопители (обычные root-порты
// + позже устройства за хабом), USB_MAX_SLOTS_ENABLED — xHCI Slot ID
// budget (MaxSlotsEn), с запасом сверх USB_MAX_DEVICES под сами хабы (у
// хаба тоже есть свой Slot ID, не только у устройств за ним).
constexpr int USB_MAX_DEVICES        = 4;
constexpr int USB_MAX_SLOTS_ENABLED  = 8;

constexpr uintptr_t PLAT_XHCI_DCBAA_VADDR       = 0x201100000ULL; // Device Context Base Address Array
constexpr uintptr_t PLAT_XHCI_CMDRING_VADDR     = 0x201101000ULL; // Command Ring, 1 сегмент
constexpr uintptr_t PLAT_XHCI_ERST_VADDR        = 0x201102000ULL; // Event Ring Segment Table (ERST)
constexpr uintptr_t PLAT_XHCI_EVTRING_VADDR     = 0x201103000ULL; // Event Ring, 1 сегмент
// Device Context — БАЗА подряд идущих USB_MAX_SLOTS_ENABLED страниц (по
// одной на xHCI Slot ID, см. devctx_paddr_for()/usb_driver.cpp) — раньше
// был единственный слот (см. историю в ROADMAP.md), Фаза 15 требует по
// странице на каждый одновременно активный Slot ID.
constexpr uintptr_t PLAT_XHCI_DEVCTX_VADDR      = 0x201104000ULL; // + slot_id*0x1000, slot_id < USB_MAX_SLOTS_ENABLED
constexpr uintptr_t PLAT_XHCI_INPUTCTX_VADDR    = 0x20110C000ULL; // Input Context — по-прежнему ОДИН общий (транзитный буфер, один слот единовременно)
constexpr uintptr_t PLAT_XHCI_SCRATCHPAD_ARR_VADDR  = 0x20110D000ULL; // Scratchpad Buffer Array (указатели) — общий, контроллерный уровень
constexpr uintptr_t PLAT_XHCI_SCRATCHPAD_BUF_VADDR  = 0x20110E000ULL; // первая из подряд идущих scratchpad-страниц (до USB_MAX_SCRATCHPAD_PAGES)
// Девятнадцатая попытка (см. ROADMAP.md): живое железо (VL805) через
// реальный HCSPARAMS2 запросило 0x1f (31) scratchpad-страниц — бюджет 16
// оказался мал (явный, ожидаемый отказ по коду, не баг). Поднято до 32
// (round-число, покрывает 31 с одной страницей запаса). CNode/IPC-бюджет
// (MAX_TRACKED_CAPS=640, message-регистры BOOT_USB_SCRATCHPAD_BUF0_PADDR+i
// в пределах страницы IPC-буфера, см. common.h/ROADMAP.md) — оба с большим
// запасом, отдельно не пересчитывались.
constexpr int         USB_MAX_SCRATCHPAD_PAGES       = 32; // бюджет этой фазы — см. ROADMAP.md; больше HCSPARAMS2 просит -> явный отказ, не динамический аллокатор
// Последняя занятая scratchpad-страница: 0x20110E000 + 32*0x1000 =
// 0x20112E000 — с большим запасом до конца 2MB-окна (0x2011FFFFF).

// Фаза 14 (закрытие, Mass Storage), Milestone 1 — у EP0 раньше НЕ было
// настоящего Transfer Ring (заглушка на Device Context). Фаза 15 —
// КАЖДЫЙ из шести ресурсов ниже теперь БАЗА подряд идущих
// USB_MAX_DEVICES страниц (по одной на "наш" индекс устройства, см.
// ep0ring_vaddr()/ctrlbuf_vaddr()/... в usb_driver.cpp) вместо одной
// общей страницы — несколько накопителей одновременно, у каждого свой
// набор колец/буферов.
constexpr uintptr_t PLAT_XHCI_EP0_TRRING_VADDR      = 0x201130000ULL; // + idx*0x1000, idx < USB_MAX_DEVICES

// Milestone 2 — буфер данных control-transfer'ов на EP0 (GET_DESCRIPTOR
// Device/Configuration и т.д.) — одна страница с избытком на устройство.
constexpr uintptr_t PLAT_XHCI_CTRL_BUF_VADDR        = 0x201134000ULL; // + idx*0x1000

// Milestone 4 — Transfer Ring'и bulk OUT/IN эндпоинтов Mass Storage
// интерфейса (найдены в Milestone 3), активируются командой Configure
// Endpoint. Тот же приём, что EP0 Transfer Ring — отдельная честная
// страница на каждое кольцо КАЖДОГО устройства.
constexpr uintptr_t PLAT_XHCI_BULKOUT_TRRING_VADDR  = 0x201138000ULL; // + idx*0x1000
constexpr uintptr_t PLAT_XHCI_BULKIN_TRRING_VADDR   = 0x20113C000ULL; // + idx*0x1000

// Milestone 5 — Bulk-Only Transport: CBW(31 байт, offset 0)/CSW(13 байт,
// offset 64, выровнено) в ОДНОЙ странице на устройство; отдельная
// страница-"bounce" под данные SCSI-команд (INQUIRY/READ CAPACITY/
// READ(10)/WRITE(10)) — тот же приём, что ctrlbuf() у EP0.
constexpr uintptr_t PLAT_XHCI_CBW_CSW_VADDR         = 0x201140000ULL; // + idx*0x1000
constexpr uintptr_t PLAT_XHCI_BOUNCE_VADDR          = 0x201144000ULL; // + idx*0x1000
// Последняя занятая страница: 0x201144000 + (USB_MAX_DEVICES-1)*0x1000 —
// с запасом до конца 2MB-окна (0x2011FFFFF).

// --- Оффсеты/биты регистров VideoCore mailbox, считаются от MAILBOX_BASE
// (см. PLAT_MBOX_PADDR — сама страница начинается на 0x880 раньше). ---
// РЕАЛЬНАЯ ПРИЧИНА, почему mailbox "не отвечал" (Фаза 4.6/7 — было записано
// как принятая деградация): сверка с рабочим драйвером U-Boot этой же платы
// (`arch/arm/mach-bcm283x/mbox.c`, `struct bcm2835_mbox_regs`) показала, что
// физически это ДВА независимых почтовых ящика с ОТДЕЛЬНЫМИ status-регистрами:
// mail0 (VC -> ARM, читаем из `read`, статус в `mail0_status`) и mail1
// (ARM -> VC, пишем в `write`, статус в `mail1_status`) — старый код здесь
// использовал ОДИН и тот же MBOX_STATUS_OFFSET (это на самом деле mail0_status)
// и для проверки "не полон ли исходящий FIFO перед записью", и для проверки
// "не пуст ли входящий FIFO при чтении ответа". Проверка перед записью должна
// смотреть на mail1_status, а не mail0_status — иначе ждём бита совсем не той
// очереди и в худшем случае вечно не доходим до самой записи в `write`,
// что и объясняет "статус не меняется, ответа нет" на ВСЕХ вариантах
// bus-адреса разом (адрес тут был ни при чём).
constexpr uintptr_t MBOX_BASE_OFFSET         = 0x880;
constexpr uintptr_t MBOX_READ_OFFSET         = MBOX_BASE_OFFSET + 0x00;  // VC -> ARM (mail0)
constexpr uintptr_t MBOX_MAIL0_STATUS_OFFSET = MBOX_BASE_OFFSET + 0x18;  // статус очереди VC -> ARM (для чтения ответа)
constexpr uintptr_t MBOX_WRITE_OFFSET        = MBOX_BASE_OFFSET + 0x20;  // ARM -> VC (mail1)
constexpr uintptr_t MBOX_MAIL1_STATUS_OFFSET = MBOX_BASE_OFFSET + 0x38;  // статус очереди ARM -> VC (для проверки места перед записью)
constexpr uint32_t  MBOX_STATUS_FULL   = (1u << 31);               // писать нельзя, исходящий FIFO полон
constexpr uint32_t  MBOX_STATUS_EMPTY  = (1u << 30);                // читать нечего
constexpr uint32_t  MBOX_CHANNEL_PROP  = 8;                         // ARM -> VC property tags channel

// Property-tag протокол (https://github.com/raspberrypi/firmware/wiki/Mailbox-property-interface).
constexpr uint32_t MBOX_TAG_GET_FIRMWARE_REVISION = 0x00000001;
constexpr uint32_t MBOX_TAG_LAST                  = 0x00000000;
constexpr uint32_t MBOX_CODE_REQUEST              = 0x00000000;
constexpr uint32_t MBOX_CODE_RESPONSE_SUCCESS     = 0x80000000;

// Фаза 7 (DVFS) — теги смены/чтения частоты тактирования. Подтверждено на
// живом железе (см. ROADMAP.md): mailbox отвечает БЕЗ какого-либо
// bus-смещения (raw ARM physical), пока буфер лежит ниже 1ГиБ (см.
// low_untyped в main.cpp) — production-код (в отличие от диагностического
// mbox_probe() с перебором вариантов) сразу использует этот один
// подтверждённый вариант.
constexpr uint32_t MBOX_TAG_GET_CLOCK_RATE     = 0x00030002;
constexpr uint32_t MBOX_TAG_SET_CLOCK_RATE     = 0x00038002;
constexpr uint32_t MBOX_TAG_GET_MIN_CLOCK_RATE = 0x00030007;
constexpr uint32_t MBOX_TAG_GET_MAX_CLOCK_RATE = 0x00030004;
constexpr uint32_t MBOX_CLOCK_ID_ARM           = 3; // см. firmware wiki: "0x000000003: ARM"
// ВАЖНО (см. официальную вики-документацию): GET_CLOCK_RATE (0x00030002)
// отдаёт "next enable rate" — т.е. может быть ПРОСТО эхом последнего
// SET_CLOCK_RATE, даже если тактирование физически не работает. Реальную
// (измеренную, с учётом clamping/throttling) частоту отдаёт ТОЛЬКО этот тег —
// используем его для диагностики "правда ли DVFS меняет железо", а не только
// то, что подтвердила прошивка на запрос.
constexpr uint32_t MBOX_TAG_GET_CLOCK_RATE_MEASURED = 0x00030047;

// Фаза 7 (продолжение) — "новый" интерфейс энергодоменов VideoCore
// (сверено с Linux drivers/pmdomain/bcm/raspberrypi-power.c —
// RPI_FIRMWARE_GET/SET_DOMAIN_STATE; НЕ путать с MBOX_TAG_*_POWER_STATE,
// которого тут нет вообще — старый интерфейс реализован прошивкой лишь для
// пары legacy-устройств вроде USB, сюда не полез). Формат запроса/ответа —
// ТЕ ЖЕ 2 слова (domain, state), что и у старого POWER_STATE, включая
// сам вызов: `struct { u32 domain; u32 state; }`, 8 байт, и для GET, и для
// SET (Linux шлёт полный пакет в обоих случаях, не 4 байта только domain).
// Значения domain — это (индекс из dt-bindings/power/raspberrypi-power.h)
// + 1, так задан сам прошивочный протокол. ARM = 23 — это буквально наши
// же ARM-ядра, НИКОГДА не гасить. Ниже — только мультимедиа/камера-блоки,
// не нужные headless serial-only ОС (I2C0-2 и VIDEO_SCALER/VPU1
// сознательно не трогаем в первом заходе: I2C всё равно, скорее всего, и
// так не запитан за ненадобностью, а "VPU1" — неочевидное название, может
// быть завязано на сам VideoCore, который держит наш же mailbox-канал).
constexpr uint32_t MBOX_TAG_GET_DOMAIN_STATE = 0x00030030;
constexpr uint32_t MBOX_TAG_SET_DOMAIN_STATE = 0x00038030;
constexpr uint32_t MBOX_DOMAIN_HDMI       = 6;
constexpr uint32_t MBOX_DOMAIN_VEC        = 8;  // composite video
constexpr uint32_t MBOX_DOMAIN_JPEG       = 9;
constexpr uint32_t MBOX_DOMAIN_H264       = 10;
constexpr uint32_t MBOX_DOMAIN_V3D        = 11; // 3D GPU
constexpr uint32_t MBOX_DOMAIN_ISP        = 12;
constexpr uint32_t MBOX_DOMAIN_UNICAM0    = 13; // камера
constexpr uint32_t MBOX_DOMAIN_UNICAM1    = 14;
constexpr uint32_t MBOX_DOMAIN_CCP2RX     = 15;
constexpr uint32_t MBOX_DOMAIN_CSI2       = 16;
constexpr uint32_t MBOX_DOMAIN_CPI        = 17;
constexpr uint32_t MBOX_DOMAIN_DSI0       = 18; // дисплей (MIPI)
constexpr uint32_t MBOX_DOMAIN_DSI1       = 19;
constexpr uint32_t MBOX_DOMAIN_TRANSPOSER = 20;
constexpr uint32_t MBOX_DOMAIN_CCP2TX     = 21;
constexpr uint32_t MBOX_DOMAIN_CDP        = 22;

// --- Оффсеты регистров mini-UART (BCM2835-style AUX-периферия). Смещения —
// от PLAT_UART_PADDR (база AUX, 0xfe215000), НЕ PrimeCell-совместимый
// формат — отдельные однобитовые флаги в LSR вместо PL011's DR/FR. ---
constexpr uintptr_t AUX_ENABLES_OFFSET = 0x04;                          // Общий для aux-периферии (mini-UART/SPI1/SPI2)
constexpr uint32_t  AUX_ENABLES_UART   = (1u << 0);                     // Включить mini-UART
constexpr uintptr_t AUX_MU_IO_OFFSET   = 0x40;                          // Data Register (TX при записи, RX при чтении)
constexpr uintptr_t AUX_MU_IER_OFFSET  = 0x44;                          // Interrupt Enable
constexpr uintptr_t AUX_MU_IIR_OFFSET  = 0x48;                          // Interrupt Identify / запись чистит FIFO
constexpr uintptr_t AUX_MU_LCR_OFFSET  = 0x4C;                          // Line Control (биты 0-1: 11 = 8 бит данных)
constexpr uintptr_t AUX_MU_MCR_OFFSET  = 0x50;                          // Modem Control
constexpr uintptr_t AUX_MU_LSR_OFFSET  = 0x54;                          // Line Status
constexpr uintptr_t AUX_MU_CNTL_OFFSET = 0x60;                          // Extra Control (биты 0-1: RX/TX enable)
constexpr uintptr_t AUX_MU_BAUD_OFFSET = 0x68;                          // Baudrate
constexpr uint32_t  AUX_MU_LSR_RX_READY = (1u << 0);                    // Данные готовы к чтению (аналог "не RXFE")
constexpr uint32_t  AUX_MU_LSR_TX_EMPTY = (1u << 5);                    // Можно писать следующий байт (аналог "не TXFF")
constexpr uint32_t  AUX_MU_LCR_8BIT     = 0x3u;                         // 8 бит данных, без чётности, 1 стоп-бит
constexpr uint32_t  AUX_MU_CNTL_RX_EN   = (1u << 0);
constexpr uint32_t  AUX_MU_CNTL_TX_EN   = (1u << 1);
constexpr uint32_t  AUX_MU_IER_RX_INT   = (1u << 0);                    // Разрешить прерывание "есть данные для чтения"

// --- Номера IRQ (GIC SPI), см. seL4_IRQControl_Get() в main.cpp ---
constexpr int PLAT_UART_IRQ  = 125;                                     // RPi4: mini-UART, см. RPI4_UART1_MINIUART_IRQ ниже
                                                                          // (общий "aux" IRQ на mini-UART/SPI1/SPI2)
// Таймер IRQ отсутствует намеренно — ARM generic timer не даёт аппаратного
// прерывания из EL0 на этой сборке ядра (см. RPI4_TIMER_PPI_* ниже и
// hw_timer.cpp), поэтому регистрировать нечего.

// --- EMMC2 (Arasan SDHCI-совместимый) — регистровая карта, смещения от
// PLAT_EMMC_PADDR. Стандартный SD Host Controller layout (SDHCI Simplified
// Spec), окно регистров в DT — 0x100 байт (0x00..0xFC), см. PLAT_EMMC_PADDR
// выше. PIO/polling-режим (без DMA, без IRQ) — минимум подвижных частей для
// первого запуска на живом железе, см. ROADMAP.md Фаза 3.3. ---
constexpr uintptr_t EMMC_BLKSIZECNT_OFFSET = 0x04;  // [9:0]=размер блока, [31:16]=кол-во блоков
constexpr uintptr_t EMMC_ARG1_OFFSET       = 0x08;
constexpr uintptr_t EMMC_CMDTM_OFFSET      = 0x0C;  // [15:0]=transfer mode, [31:16]=команда+response type
constexpr uintptr_t EMMC_RESP0_OFFSET      = 0x10;
constexpr uintptr_t EMMC_RESP1_OFFSET      = 0x14;
constexpr uintptr_t EMMC_RESP2_OFFSET      = 0x18;
constexpr uintptr_t EMMC_RESP3_OFFSET      = 0x1C;
constexpr uintptr_t EMMC_DATA_OFFSET       = 0x20;  // PIO порт данных (чтение/запись по 4 байта)
constexpr uintptr_t EMMC_STATUS_OFFSET     = 0x24;
constexpr uintptr_t EMMC_CONTROL0_OFFSET   = 0x28;
constexpr uintptr_t EMMC_CONTROL1_OFFSET   = 0x2C;
constexpr uintptr_t EMMC_INTERRUPT_OFFSET  = 0x30;  // write-1-to-clear
constexpr uintptr_t EMMC_IRPT_MASK_OFFSET  = 0x34;
constexpr uintptr_t EMMC_IRPT_EN_OFFSET    = 0x38;
constexpr uintptr_t EMMC_CAP0_OFFSET       = 0x40;  // Capabilities: [13:8] = base clock frequency (МГц)
constexpr uint32_t EMMC_CAP0_ADMA2_SUPPORT = (1u << 19); // Фаза 4.5/ADMA2 (см. ROADMAP.md) — если этот бит не выставлен, ADMA2 контроллером не поддерживается вообще
constexpr uintptr_t EMMC_ADMA_SYSADDR_OFFSET = 0x58; // ADMA System Address (32-бит, см. ROADMAP.md 4.5/ADMA2) — физический адрес таблицы дескрипторов
constexpr uintptr_t EMMC_SLOTISR_VER_OFFSET = 0xFC; // [23:16] = Host Controller Version (0=v1,1=v2,2=v3)

// STATUS (0x24) — биты состояния линий команды/данных
constexpr uint32_t EMMC_STATUS_CMD_INHIBIT = (1u << 0);  // Нельзя слать новую команду
constexpr uint32_t EMMC_STATUS_DAT_INHIBIT = (1u << 1);  // Линия DAT занята
constexpr uint32_t EMMC_STATUS_DAT_ACTIVE  = (1u << 2);

// CONTROL1 (0x2C) — тактирование + software reset
constexpr uint32_t EMMC_C1_CLK_INTLEN   = (1u << 0);      // Включить внутренний клок
constexpr uint32_t EMMC_C1_CLK_STABLE   = (1u << 1);      // RO: клок стабилен
constexpr uint32_t EMMC_C1_CLK_EN       = (1u << 2);      // Подать клок на SD-шину
constexpr uint32_t EMMC_C1_CLK_GENSEL   = (1u << 5);      // 0 = Divided Clock Mode
constexpr uint32_t EMMC_C1_TOUNIT_MAX   = (0xEu << 16);   // Максимальный data timeout
constexpr uint32_t EMMC_C1_SRST_HC      = (1u << 24);     // Software reset всего контроллера
constexpr uint32_t EMMC_C1_SRST_CMD     = (1u << 25);
constexpr uint32_t EMMC_C1_SRST_DATA    = (1u << 26);
// Делитель клока (Divided Clock Mode, 8-bit): freq = base_clk / (2 * divisor), 0 => /1.
// Идентификационная стадия (~400kHz) и рабочая стадия (~25MHz) — делители
// подбираются в emmc_init() из фактической base clock (см. CAPABILITIES).
constexpr uint32_t EMMC_C1_CLK_FREQ_SHIFT = 8;

// INTERRUPT (0x30) / IRPT_MASK / IRPT_EN — общие биты статуса
constexpr uint32_t EMMC_INT_CMD_DONE   = (1u << 0);   // Command Complete
constexpr uint32_t EMMC_INT_DATA_DONE  = (1u << 1);   // Transfer Complete
constexpr uint32_t EMMC_INT_WRITE_RDY  = (1u << 4);   // Buffer Write Ready
constexpr uint32_t EMMC_INT_READ_RDY   = (1u << 5);   // Buffer Read Ready
constexpr uint32_t EMMC_INT_CARD_INT   = (1u << 8);   // Card Interrupt (SDHCI Simplified Spec, стандартный бит для ЛЮБОГО SDIO-совместимого контроллера) — карта сигналит хосту по DAT1 в-полосе; используется ТОЛЬКО в wifi_driver.cpp (Фаза 4.5, реальный GIC IRQ для sdpcm_wait_and_read_ctrl), EMMC2/blk_driver.cpp его не размаскирует (там нет SDIO-функций, сигналить некому)
constexpr uint32_t EMMC_INT_ERROR_MASK = 0xFFFF0000u; // Любая ошибка (Command/Data Error Status, верхние 16 бит)
constexpr uint32_t EMMC_INT_ALL_EN     = 0xFFFFFFFFu; // Маска "разрешить всё" для IRPT_MASK (статус-биты) и, начиная с Фазы 4.5, для IRPT_EN тоже (реальный GIC IRQ, см. blk_driver.cpp)

// CONTROL0 (0x28) — базовая настройка хоста
constexpr uint32_t EMMC_C0_USE_4BIT    = (1u << 1);   // Ширина шины 4 бита (не используется в первой версии — см. план)
// SD Bus Power (bits 8-11 в CONTROL0, аналог legacy SDHCI "Power Control"
// байта на 0x29): SRST_HC гасит питание шины, найдено эмпирически на живом
// железе (до сброса bits 8-11 = 0xF, после — 0x0) — без этого CMD_INHIBIT
// висит вечно и ни одна команда никогда не завершается.
constexpr uint32_t EMMC_C0_PWR_ON      = (1u << 8);
constexpr uint32_t EMMC_C0_PWR_3V3     = (0x7u << 9);
// DMA Select (биты [4:3] Host Control 1, тот же младший байт CONTROL0, что и
// USE_4BIT выше) — Фаза 4.5/ADMA2 (см. ROADMAP.md): 00=SDMA (не используем),
// 10=32-битный ADMA2 (наш случай — все физические адреса RPi4 укладываются
// в 32 бита, см. доступные регионы памяти при загрузке), 11=64-битный ADMA2.
constexpr uint32_t EMMC_C0_DMA_SEL_MASK      = (0x3u << 3);
constexpr uint32_t EMMC_C0_DMA_SEL_ADMA2_32  = (0x2u << 3);

// CMDTM (0x0C) — response type (биты [17:16]) и флаги данных
constexpr uint32_t EMMC_CMD_RSPNS_NONE = (0x0u << 16);
constexpr uint32_t EMMC_CMD_RSPNS_136  = (0x1u << 16);
constexpr uint32_t EMMC_CMD_RSPNS_48   = (0x2u << 16);
constexpr uint32_t EMMC_CMD_RSPNS_48B  = (0x3u << 16); // R1b (с busy-сигналом на DAT0)
constexpr uint32_t EMMC_CMD_CRCCHK_EN  = (1u << 19);
constexpr uint32_t EMMC_CMD_IXCHK_EN   = (1u << 20);
constexpr uint32_t EMMC_CMD_ISDATA     = (1u << 21);   // Data Present Select
constexpr uint32_t EMMC_CMD_INDEX_SHIFT = 24;           // Индекс команды в битах [29:24]
// Transfer Mode (биты [15:0] того же CMDTM)
constexpr uint32_t EMMC_TM_DMA_EN      = (1u << 0);     // Фаза 4.5/ADMA2 — БЕЗ этого бита контроллер игнорирует ADMA_SYSADDR и ждёт PIO, даже если DMA Select в CONTROL0 уже выставлен
constexpr uint32_t EMMC_TM_BLKCNT_EN   = (1u << 1);
constexpr uint32_t EMMC_TM_MULTI_BLOCK = (1u << 5);
constexpr uint32_t EMMC_TM_DAT_DIR_READ = (1u << 4);    // 1 = card->host (чтение)
// Auto CMD Enable, биты [3:2]: 00=выкл, 01=авто-CMD12 (Stop Transmission) после
// последнего блока, 10=авто-CMD23. Обязателен для CMD18/25 (multi-block) —
// без него контроллер не остановит передачу сам, и карта продолжит держать
// DAT-линию занятой в ожидании явного CMD12 (см. SD Host Controller Simplified
// Specification, "Transfer Mode Register").
constexpr uint32_t EMMC_TM_AUTO_CMD12  = (1u << 2);

// --- ADMA2 (32-битный) дескриптор, 8 байт (см. SD Host Controller Simplified
// Specification, "ADMA2 Descriptor Table") — Фаза 4.5 (ROADMAP.md): заменяет
// PIO-цикл по EMMC_DATA в hardware_emmc_read/write (blk_driver.cpp). Один
// дескриптор на один сектор (512 байт) — с запасом от лимита Length=65535.
// Табличка дескрипторов и bounce-буфер данных живут в приватной
// НЕКЭШИРУЕМОЙ странице blk_driver (см. PLAT_BLK_DMA_VADDR выше) — обычный
// (кэшируемый) стек/куча процесса для DMA не годится без явного cache
// maintenance (см. ROADMAP.md 4.5 — разбор, почему решили не рисковать
// aliasing'ом кэш-линий на произвольных стековых буферах fat32.cpp).
constexpr uint32_t ADMA2_ATTR_VALID     = (1u << 0);
constexpr uint32_t ADMA2_ATTR_END       = (1u << 1);
constexpr uint32_t ADMA2_ATTR_INT       = (1u << 2); // не используем — ждём общий Transfer Complete, не per-descriptor ADMA interrupt
constexpr uint32_t ADMA2_ATTR_ACT_TRAN  = (0x2u << 4); // Act=Transfer Data
struct __attribute__((packed)) Adma2Descriptor32 {
    uint16_t attr;
    uint16_t length;   // 0 означает 65536, нам не актуально (512 байт/сектор)
    uint32_t addr;      // физический адрес буфера данных
};

// Коды SD-команд (используются как (CMDn << EMMC_CMD_INDEX_SHIFT) | response type | флаги)
constexpr uint32_t EMMC_CMD_GO_IDLE        = 0;   // CMD0,  без ответа
constexpr uint32_t EMMC_CMD_SEND_IF_COND   = 8;   // CMD8,  R7 (как R1/48bit)
constexpr uint32_t EMMC_CMD_ALL_SEND_CID   = 2;   // CMD2,  R2/136bit
constexpr uint32_t EMMC_CMD_SEND_REL_ADDR  = 3;   // CMD3,  R6 (как R1/48bit)
constexpr uint32_t EMMC_CMD_SEND_CSD       = 9;   // CMD9,  R2/136bit
constexpr uint32_t EMMC_CMD_SELECT_CARD    = 7;   // CMD7,  R1b/48bit+busy
constexpr uint32_t EMMC_CMD_APP_CMD        = 55;  // CMD55, R1/48bit — префикс для ACMDn
constexpr uint32_t EMMC_ACMD_SD_SEND_OP_COND = 41; // ACMD41 (после CMD55), R3/48bit (без CRC)
constexpr uint32_t EMMC_CMD_READ_SINGLE    = 17;  // CMD17, R1/48bit + данные (host<-card)
constexpr uint32_t EMMC_CMD_READ_MULTI     = 18;  // CMD18, R1/48bit + данные, multi-block
constexpr uint32_t EMMC_CMD_WRITE_SINGLE   = 24;  // CMD24, R1/48bit + данные (host->card)
constexpr uint32_t EMMC_CMD_WRITE_MULTI    = 25;  // CMD25, R1/48bit + данные, multi-block
// ACMD41 HCS-бит (Host Capacity Support, запрашиваем поддержку SDHC/SDXC) и
// бит готовности в ответе (OCR, бит 31)
constexpr uint32_t EMMC_ACMD41_HCS      = (1u << 30);
constexpr uint32_t EMMC_ACMD41_VOLTAGE  = 0x00FF8000u; // 3.2-3.4V window
constexpr uint32_t EMMC_OCR_READY       = (1u << 31);

// --- GENET v5 (BCM2711 Ethernet MAC) — регистровая карта, смещения от
// PLAT_GENET_PADDR. Адаптировано 1:1 из проверенного рабочего референса —
// /home/nikita/RPi4_SeL4/u-boot/drivers/net/bcmgenet.c (драйвер GENETv5
// U-Boot, который этот же борд реально использует для сетевой части). См.
// ROADMAP.md Фаза 3.2. ---
constexpr uintptr_t GENET_SYS_REV_CTRL_OFFSET       = 0x00;
constexpr uintptr_t GENET_SYS_PORT_CTRL_OFFSET      = 0x04;
constexpr uintptr_t GENET_SYS_RBUF_FLUSH_CTRL_OFFSET = 0x08;
constexpr uintptr_t GENET_EXT_RGMII_OOB_CTRL_OFFSET = 0x08C;  // 0x80 (EXT off) + 0x0c
constexpr uintptr_t GENET_RBUF_CTRL_OFFSET          = 0x300;
constexpr uintptr_t GENET_RBUF_TBUF_SIZE_CTRL_OFFSET = 0x3B4; // 0x300 + 0xb4
constexpr uintptr_t GENET_UMAC_MAC0_OFFSET          = 0x80C;  // 0x800 (UMAC off) + 0x00c
constexpr uintptr_t GENET_UMAC_MAC1_OFFSET          = 0x810;
constexpr uintptr_t GENET_UMAC_CMD_OFFSET           = 0x808;
constexpr uintptr_t GENET_UMAC_MAX_FRAME_LEN_OFFSET = 0x814;
constexpr uintptr_t GENET_UMAC_TX_FLUSH_OFFSET      = 0xB34;  // 0x800 + 0x334
constexpr uintptr_t GENET_UMAC_MIB_CTRL_OFFSET      = 0xD80;  // 0x800 + 0x580
constexpr uintptr_t GENET_MDIO_CMD_OFFSET           = 0xE14;  // = RPI4_GENET_MDIO_OFFSET
// MAC Destination Filter — таблица из 17 записей (broadcast/unicast/multicast),
// не настраиваем её (нет реального смысла для диагностического трафика этой
// ОС) — вместо этого гасим её целиком (=0, как в bcmgenet_set_rx_mode()
// Linux-драйвера при promiscuous) и включаем CMD_PROMISC ниже. Без этого
// приём кадров зависел бы от того, в каком состоянии MDF оставил предыдущий
// инициализатор GENET (например, U-Boot, настроивший её под свой MAC) —
// см. /home/nikita/workspace_nofing/common/drivers/net/ethernet/broadcom/genet/bcmgenet.c.
constexpr uintptr_t GENET_UMAC_MDF_CTRL_OFFSET      = 0xE50;  // 0x800 + 0x650

// Контроллер прерываний GENET (INTRL2_0) — Фаза 4.5 (событийный RX, см.
// ROADMAP.md). НЕ было в проекте до этого момента (в отличие от остальных
// GENET-регистров выше, унаследованных от u-boot/bcmgenet.c, который сам
// прерываниями не пользуется — чисто polling-драйвер загрузчика). Смещения
// и биты сверены с реальным Linux-драйвером — /home/nikita/kernel_xiaomi_vince/
// drivers/net/ethernet/broadcom/genet/bcmgenet.h (тот же GENETv5, тот же
// SoC-блок, просто другая ОС). INTRL2_0 — основной, CPU-обращённый блок
// (link/DMA done/ошибки); INTRL2_1 (0x0240) — per-ring, не нужен при одной
// очереди по умолчанию (см. GENET_DEFAULT_Q) — не используем.
constexpr uintptr_t GENET_INTRL2_0_OFF        = 0x0200;
constexpr uintptr_t INTRL2_CPU_STAT           = GENET_INTRL2_0_OFF + 0x00; // read-only, сырые pending-биты
constexpr uintptr_t INTRL2_CPU_CLEAR          = GENET_INTRL2_0_OFF + 0x08; // write-1-to-clear (НЕ то же самое, что STAT!)
constexpr uintptr_t INTRL2_CPU_MASK_STATUS    = GENET_INTRL2_0_OFF + 0x0C; // read-only, 1 = замаскирован
constexpr uintptr_t INTRL2_CPU_MASK_SET       = GENET_INTRL2_0_OFF + 0x10; // write 1 = замаскировать (выключить)
constexpr uintptr_t INTRL2_CPU_MASK_CLEAR     = GENET_INTRL2_0_OFF + 0x14; // write 1 = размаскировать (включить)
constexpr uint32_t  UMAC_IRQ_LINK_UP          = (1u << 4);
constexpr uint32_t  UMAC_IRQ_LINK_DOWN        = (1u << 5);
constexpr uint32_t  UMAC_IRQ_RXDMA_DONE       = (1u << 13); // он же UMAC_IRQ_RXDMA_MBDONE в эталоне

constexpr uint32_t GENET_PORT_MODE_EXT_GPHY = 3;
constexpr uint32_t GENET_RGMII_LINK      = (1u << 4);
constexpr uint32_t GENET_OOB_DISABLE     = (1u << 5);
constexpr uint32_t GENET_RGMII_MODE_EN   = (1u << 6);
constexpr uint32_t GENET_ID_MODE_DIS     = (1u << 16);
constexpr uint32_t GENET_RBUF_ALIGN_2B   = (1u << 1);

constexpr uint32_t GENET_CMD_TX_EN       = (1u << 0);
constexpr uint32_t GENET_CMD_RX_EN       = (1u << 1);
constexpr uint32_t GENET_UMAC_SPEED_10   = 0;
constexpr uint32_t GENET_UMAC_SPEED_100  = 1;
constexpr uint32_t GENET_UMAC_SPEED_1000 = 2;
constexpr uint32_t GENET_CMD_SPEED_SHIFT = 2;
constexpr uint32_t GENET_CMD_PROMISC     = (1u << 4);
constexpr uint32_t GENET_CMD_SW_RESET    = (1u << 13);
constexpr uint32_t GENET_CMD_LCL_LOOP_EN = (1u << 15);
constexpr uint32_t GENET_MIB_RESET_RX    = (1u << 0);
constexpr uint32_t GENET_MIB_RESET_RUNT  = (1u << 1);
constexpr uint32_t GENET_MIB_RESET_TX    = (1u << 2);

// MDIO (доступ к регистрам PHY через GENET_MDIO_CMD_OFFSET)
constexpr uint32_t GENET_MDIO_START_BUSY = (1u << 29);
constexpr uint32_t GENET_MDIO_READ_FAIL  = (1u << 28);
constexpr uint32_t GENET_MDIO_RD         = (2u << 26);
constexpr uint32_t GENET_MDIO_WR         = (1u << 26);
constexpr uint32_t GENET_MDIO_PMD_SHIFT  = 21;
constexpr uint32_t GENET_MDIO_REG_SHIFT  = 16;
// Стандартные MII-регистры самой PHY (не GENET), адрес PHY = 1 (см. dts
// ethernet-phy@1) — хардкодим, DT-парсера у нас нет.
constexpr uint32_t GENET_PHY_ADDR        = 1;
constexpr uint32_t MII_BMCR              = 0x00; // Basic Mode Control (бит 15 = reset)
constexpr uint32_t MII_BMSR              = 0x01; // Basic Mode Status (бит 2 = link up)
constexpr uint32_t MII_ADVERTISE         = 0x04; // что рекламируем на автосогласовании (10/100)
constexpr uint32_t MII_LPA               = 0x05; // что рекламирует link partner (10/100)
constexpr uint32_t MII_CTRL1000          = 0x09; // что рекламируем на автосогласовании (1000BASE-T)
constexpr uint32_t MII_STAT1000          = 0x0A; // что рекламирует link partner (1000BASE-T)
constexpr uint32_t MII_BMCR_RESET        = (1u << 15);
constexpr uint32_t MII_BMSR_LINK_UP      = (1u << 2);
constexpr uint32_t MII_BMSR_ANEGCOMPLETE = (1u << 5);
// Стандартные (Clause 22, не вендор-специфичные) биты для вычисления реально
// согласованной скорости/дуплекса — см. genet_resolve_link_speed() в
// net_driver.cpp. Приоритет по IEEE 802.3: 1000FD > 1000HD > 100FD > 100HD
// > 10FD > 10HD.
constexpr uint32_t MII_ADV_10HALF   = (1u << 5);
constexpr uint32_t MII_ADV_10FULL   = (1u << 6);
constexpr uint32_t MII_ADV_100HALF  = (1u << 7);
constexpr uint32_t MII_ADV_100FULL  = (1u << 8);
constexpr uint32_t MII_CTRL1000_ADV_HALF = (1u << 8);
constexpr uint32_t MII_CTRL1000_ADV_FULL = (1u << 9);
constexpr uint32_t MII_STAT1000_LP_HALF  = (1u << 10);
constexpr uint32_t MII_STAT1000_LP_FULL  = (1u << 11);

// DMA-дескрипторы (256 фиксировано под адресацию регистровых блоков колец —
// см. комментарий в плане/ROADMAP; реально используем меньше, RX_DESCS/TX_DESCS).
constexpr uintptr_t GENET_RX_OFF          = 0x2000;
constexpr uintptr_t GENET_TX_OFF          = 0x4000;
constexpr uint32_t  GENET_TOTAL_DESCS     = 256;     // фиксировано железом (адресация блоков ниже)
constexpr uint32_t  GENET_DMA_DESC_SIZE   = 12;       // LENGTH_STATUS(4)+ADDR_LO(4)+ADDR_HI(4)
constexpr uintptr_t GENET_DMA_DESC_LENGTH_STATUS = 0x00;
constexpr uintptr_t GENET_DMA_DESC_ADDRESS_LO    = 0x04;
constexpr uintptr_t GENET_DMA_DESC_ADDRESS_HI    = 0x08;

constexpr uint32_t GENET_RX_DESCS = 4;   // реально используемая глубина RX-кольца
constexpr uint32_t GENET_TX_DESCS = 1;   // синхронная отправка по одному кадру

constexpr uintptr_t GENET_RDMA_REG_OFF = GENET_RX_OFF + GENET_TOTAL_DESCS * GENET_DMA_DESC_SIZE;
constexpr uintptr_t GENET_TDMA_REG_OFF = GENET_TX_OFF + GENET_TOTAL_DESCS * GENET_DMA_DESC_SIZE;
constexpr uint32_t  GENET_DEFAULT_Q    = 0x10;         // "default"-очередь (16), как в U-Boot
constexpr uintptr_t GENET_DMA_RING_SIZE = 0x40;
constexpr uintptr_t GENET_DMA_RINGS_SIZE = GENET_DMA_RING_SIZE * (GENET_DEFAULT_Q + 1);
constexpr uintptr_t GENET_TDMA_RING_REG_BASE = GENET_TDMA_REG_OFF + GENET_DEFAULT_Q * GENET_DMA_RING_SIZE;
constexpr uintptr_t GENET_RDMA_RING_REG_BASE = GENET_RDMA_REG_OFF + GENET_DEFAULT_Q * GENET_DMA_RING_SIZE;
// Оффсеты внутри *_RING_REG_BASE (общие для T/RDMA, см. bcmgenet.c)
constexpr uintptr_t GENET_DMA_RING_BUF_SIZE_OFF   = 0x10;
constexpr uintptr_t GENET_DMA_START_ADDR_OFF      = 0x14;
constexpr uintptr_t GENET_DMA_END_ADDR_OFF        = 0x1C;
constexpr uintptr_t GENET_DMA_MBUF_DONE_THRESH_OFF = 0x24;
constexpr uintptr_t GENET_TDMA_READ_PTR_OFF        = 0x00;
constexpr uintptr_t GENET_TDMA_CONS_INDEX_OFF      = 0x08;
constexpr uintptr_t GENET_TDMA_PROD_INDEX_OFF      = 0x0C;
constexpr uintptr_t GENET_TDMA_FLOW_PERIOD_OFF     = 0x28;
constexpr uintptr_t GENET_TDMA_WRITE_PTR_OFF       = 0x2C;
constexpr uintptr_t GENET_RDMA_WRITE_PTR_OFF       = 0x00;
constexpr uintptr_t GENET_RDMA_PROD_INDEX_OFF      = 0x08;
constexpr uintptr_t GENET_RDMA_CONS_INDEX_OFF      = 0x0C;
constexpr uintptr_t GENET_RDMA_XON_XOFF_THRESH_OFF = 0x28;
constexpr uintptr_t GENET_RDMA_READ_PTR_OFF        = 0x2C;
constexpr uintptr_t GENET_TDMA_REG_BASE = GENET_TDMA_REG_OFF + GENET_DMA_RINGS_SIZE;
constexpr uintptr_t GENET_RDMA_REG_BASE = GENET_RDMA_REG_OFF + GENET_DMA_RINGS_SIZE;
constexpr uintptr_t GENET_DMA_RING_CFG_OFF  = 0x00;
constexpr uintptr_t GENET_DMA_CTRL_OFF      = 0x04;
constexpr uintptr_t GENET_DMA_SCB_BURST_SIZE_OFF = 0x0C;

constexpr uint32_t GENET_DMA_EN                = (1u << 0);
constexpr uint32_t GENET_DMA_RING_BUF_EN_SHIFT = 1;
constexpr uint32_t GENET_DMA_BUFLENGTH_SHIFT   = 16;
constexpr uint32_t GENET_DMA_RING_SIZE_SHIFT   = 16;
constexpr uint32_t GENET_DMA_OWN               = 0x8000;
constexpr uint32_t GENET_DMA_EOP               = 0x4000;
constexpr uint32_t GENET_DMA_SOP               = 0x2000;
constexpr uint32_t GENET_DMA_MAX_BURST_LENGTH  = 0x8;
constexpr uint32_t GENET_DMA_TX_APPEND_CRC     = 0x0040;
constexpr uint32_t GENET_DMA_TX_QTAG_SHIFT     = 7;
constexpr uint32_t GENET_DMA_FC_THRESH_LO      = 5;
constexpr uint32_t GENET_DMA_FC_THRESH_HI      = GENET_RX_DESCS >> 4; // 0 при RX_DESCS<16, как и было бы у u-boot с малым кольцом
constexpr uint32_t GENET_DMA_FC_THRESH_VALUE   = (GENET_DMA_FC_THRESH_LO << 16) | GENET_DMA_FC_THRESH_HI;

// Буфер на пакет — как у существующего virtio-net кода (net_driver.cpp),
// с запасом на полный Ethernet-кадр.
constexpr uint32_t GENET_RX_BUF_LENGTH = 1536;
// Железо GENET само добавляет 2 байта паддинга в начало каждого принятого
// кадра при RBUF_ALIGN_2B (см. net_hw_init) — чтобы IP-заголовок (после
// 14-байтового Ethernet-заголовка) оказался выровнен на 4 байта. Дескриптор
// LENGTH_STATUS включает эти 2 байта в свою длину — их нужно вычитать/
// пропускать при чтении принятого кадра (см. bcmgenet.c: RX_BUF_OFFSET,
// bcmgenet_gmac_eth_recv()). Без этого каждый принятый кадр читается со
// сдвигом на 2 байта и вся протокольная логика видит мусор вместо реальных
// полей — отсюда "ничего не приходит" при отправке ARP/DHCP-запросов.
constexpr uint32_t GENET_RX_BUF_OFFSET = 2;

// --- AVS RO thermal (термодатчик BCM2711) — регистровая карта, смещение от
// PLAT_AVS_PADDR. Единственный регистр статуса температуры кристалла,
// найден по эталонному Linux-драйверу того же чипа —
// /home/nikita/workspace_nofing/common/drivers/thermal/broadcom/bcm2711_thermal.c
// (compatible "brcm,bcm2711-thermal", родительский syscon-узел
// "avs-monitor@7d5d2000"). В отличие от VideoCore mailbox (который в этой
// же сессии не отвечал при попытке достать реальный MAC-адрес, см.
// net_driver.cpp/README.md) этот регистр читается напрямую по MMIO, без
// прошивки/протокола — предпочтительный путь по уже устоявшейся в этом
// порту практике (UART/EMMC2/GENET тоже все raw MMIO). ---
constexpr uintptr_t AVS_RO_TEMP_STATUS_OFFSET = 0x200;
// Бит валидности показания — см. AVS_RO_TEMP_STATUS_VALID_MSK в референсе
// (BIT(16) | BIT(10)); данные — 10 бит [9:0].
constexpr uint32_t AVS_RO_TEMP_STATUS_VALID_MSK = (1u << 16) | (1u << 10);
constexpr uint32_t AVS_RO_TEMP_STATUS_DATA_MSK  = 0x3FFu;
// Линейное преобразование сырого кода АЦП в миллиградусы Цельсия:
// temp_mC = slope * raw + offset. Коэффициенты — из device tree ядра Linux
// для этого же SoC (bcm2711.dtsi: `&cpu_thermal { coefficients = <(-487) 410040>; }`),
// не датащит-константа общего вида — специфично для BCM2711.
constexpr int32_t AVS_TEMP_SLOPE_MC  = -487;
constexpr int32_t AVS_TEMP_OFFSET_MC = 410040;

// --- Wi-Fi (BCM43455) SDIO — команды и битовые поля аргументов, отдельные
// от SD-memory команд EMMC_CMD_* выше (EMMC_* регистровые СМЕЩЕНИЯ
// (CMDTM/ARG1/RESP/DATA/STATUS/CONTROL0/CONTROL1/INTERRUPT/CAP0) при этом
// переиспользуются как есть — это тот же стандартный SDHCI layout, см.
// PLAT_WIFI_SDIO_PADDR выше). Формат аргументов — из спеки SDIO (сверено с
// /home/nikita/workspace_nofing/common/include/linux/mmc/sdio.h). ---
constexpr uint32_t SDIO_CMD_SEND_OP_COND = 5;   // CMD5,  R4/48bit, БЕЗ CRC-проверки (в отличие от SD-memory команд)
constexpr uint32_t SDIO_CMD_RW_DIRECT    = 52;  // CMD52, R5/48bit — байтовое чтение/запись регистра функции
// CMD52 argument: [31]=R/W, [30:28]=номер функции, [27]=RAW, [25:9]=адрес регистра, [7:0]=данные (при записи)
constexpr uint32_t SDIO_ARG_RW_FLAG        = (1u << 31);  // 1 = запись
constexpr uint32_t SDIO_ARG_FUNC_SHIFT     = 28;
constexpr uint32_t SDIO_ARG_RAW_FLAG       = (1u << 27);  // Read After Write
constexpr uint32_t SDIO_ARG_REG_ADDR_SHIFT = 9;
constexpr uint32_t SDIO_ARG_DATA_MASK      = 0xFFu;
// Функция 0 (CCCR/FBR, общие регистры карты — не WLAN-данные, те в функциях 1/2)
constexpr uint32_t SDIO_FUNC_0 = 0;
// F0 CCCR offset 0x00 — версия CCCR/FBR (см. SDIO_CCCR_CCCR в linux/mmc/sdio.h).
// Читаем этот регистр в Милстоуне 4.1 как доказательство, что чип отвечает.
constexpr uint32_t SDIO_CCCR_CCCR_OFFSET = 0x00;
// CMD5 (R4) response, бит 31 — "card ready" (питание/OCR-договорённость
// завершена). Как и ACMD41 в blk_driver.cpp, CMD5 нужно слать в цикле, пока
// карта не выставит этот бит — иначе последующие команды (CMD52 и т.д.)
// могут не проходить, потому что чип ещё занят собственной инициализацией.
constexpr uint32_t SDIO_R4_READY = (1u << 31);

// F0 CCCR offset 0x02/0x03 — Input/Output Enable и Ready (см. SDIO_CCCR_IOEx/
// SDIO_CCCR_IORx в linux/mmc/sdio.h). Прежде чем к функции N (N>=1, здесь —
// backplane-функция 1) можно обращаться CMD52/CMD53, хост обязан выставить
// бит N в IOEx и дождаться того же бита в IORx (карта включает функцию не
// мгновенно) — это отдельный от CMD3/CMD7 шаг card-инициализации, специфичный
// для SDIO-функций, и без него CMD53 к F1 не проходит.
constexpr uint32_t SDIO_CCCR_IOEx_OFFSET = 0x02;
constexpr uint32_t SDIO_CCCR_IORx_OFFSET = 0x03;

// F0 CCCR offset 0x04 — Interrupt Enable (см. SDIO_CCCR_IENx в
// linux/mmc/sdio.h). Бит 0 — IENM (master interrupt enable), биты 1..7 —
// per-function enable (бит N разрешает in-band прерывание по DAT1 от
// функции N). Пока не трогался (Милстоуны 4.1-4.3 — чистый polling,
// см. ROADMAP.md 4.5) — карта физически МОЖЕТ сигнализировать прерывание
// (см. I_HMB_FRAME_IND/I_HMB_HOST_INT в SDPCMD_INTSTATUS ниже), просто хост
// никогда не просил её это делать. Тот же read-modify-write принцип, что и
// у sdio_enable_func()/IOEx — запись CMD52 перезаписывает байт целиком.
constexpr uint32_t SDIO_CCCR_IENx_OFFSET = 0x04;
constexpr uint32_t SDIO_CCCR_IEN_MASTER  = (1u << 0);

// F0 CCCR offset 0x07 — Bus Interface Control (см. SDIO_CCCR_IF в
// linux/mmc/sdio.h). Биты [1:0] — ширина шины данных (00=1-бит, 10=4-бит).
// Милстоуны 4.1-4.3 работали в 1-бит режиме (хост НИКОГДА не переводился в
// 4-бит, см. EMMC_C0_USE_4BIT — константа была объявлена, но нигде не
// использовалась), потому что весь PIO-транспорт (CMD53 word-by-word через
// EMMC_DATA) от ширины шины не зависит — работает одинаково что на 1, что
// на 4 линиях DATA. НО in-band SDIO-прерывание сигналится картой именно по
// DAT1 — стандартный quirk многих SDHCI-контроллеров: статус Card Interrupt
// надёжно распознаётся, только когда контроллер реально сконфигурирован на
// 4-бит (см. ROADMAP.md 4.5 — CARD_INT ни разу не появился за 3с прямого
// опроса при активной 1-бит шине). Переключение — ДВУСТОРОННЕЕ: сначала
// карта (этот регистр, CMD52), потом хост (EMMC_C0_USE_4BIT в CONTROL0) —
// CMD-линия ширины не имеет, поэтому сообщить карте первой безопасно.
constexpr uint32_t SDIO_CCCR_IF_OFFSET   = 0x07;
constexpr uint32_t SDIO_BUS_WIDTH_4BIT   = 0x02;
constexpr uint32_t SDIO_BUS_WIDTH_MASK   = 0x03;

// F0 CCCR offset 0x06 — I/O Abort (см. SDIO_CCCR_ABORT в linux/mmc/sdio.h).
// Бит 3 (RES) — программный сброс ВСЕХ SDIO-функций карты, определённый
// самой SDIO-спекой: "the card returns to its power-up default state, before
// card identification" — то же самое, что физический power-cycle, но без
// участия внешнего GPIO/regulator'а. Используется в wifi_sdio_probe() ПЕРЕД
// CMD5 при повторном "wifi start" после "wifi stop" — см. ROADMAP.md/память
// проекта: без физического power-cycle чип помнит состояние card-selected
// из прошлой сессии и не отвечает на CMD5 (валиден только в idle), а
// оказавшийся неэффективным CMD0 (GO_IDLE_STATE) SDIO-only картам не
// обязателен по спеке и на этом чипе, похоже, ничего не делает.
constexpr uint32_t SDIO_CCCR_ABORT_OFFSET = 0x06;
constexpr uint32_t SDIO_CCCR_ABORT_RES    = (1u << 3);

// --- Wi-Fi (BCM43455) Милстоун 4.2 — backplane (внутренняя шина чипа):
// enumeration ядер, сброс ARM-ядра, заливка прошивки/NVRAM. Все константы
// сверены построчно с эталонным
// /home/nikita/workspace_nofing/common/drivers/net/wireless/broadcom/brcm80211/brcmfmac/
// (chip.c, bcma_regs.h, chipcommon.h, soc.h) для семейства чипов 0x4345
// (43454/43455/43456 — один Chip ID, отличаются только chiprev). ---
constexpr uint32_t SDIO_CMD_RW_EXTENDED = 53; // CMD53, R5/48bit — блочное чтение/запись (SDIO_RW_EXTENDED)
// CMD53 argument: [31]=R/W, [30:28]=функция, [27]=Block mode, [26]=Increment
// address, [25:9]=адрес регистра, [8:0]=байт/блок-каунт (см. linux/mmc/sdio.h:26-35).
constexpr uint32_t SDIO_ARG_BLOCK_MODE_FLAG = (1u << 27);
constexpr uint32_t SDIO_ARG_INCR_ADDR_FLAG  = (1u << 26);
constexpr uint32_t SDIO_ARG_COUNT_MASK      = 0x1FFu; // [8:0]

// Функция 1 (backplane-доступ) — окно 32KB (0x8000): адрес окна выставляется
// тремя байтовыми регистрами (CMD52 к F1), офсет ВНУТРИ окна идёт как обычный
// F1-адрес в аргументе CMD52/53.
constexpr uint32_t SBSDIO_FUNC_1              = 1;
constexpr uint32_t SBSDIO_FUNC1_SBADDRLOW     = 0x1000A;
constexpr uint32_t SBSDIO_FUNC1_SBADDRMID     = 0x1000B;
constexpr uint32_t SBSDIO_FUNC1_SBADDRHIGH    = 0x1000C;
constexpr uint32_t SBSDIO_SBWINDOW_MASK       = 0xffff8000u;
constexpr uint32_t SBSDIO_SB_OFT_ADDR_MASK    = 0x07FFFu;   // офсет внутри окна -> младшие 15 бит F1-адреса
constexpr uint32_t SBSDIO_SB_ACCESS_2_4B_FLAG = 0x08000u;   // 32-битный (не байтовый) доступ
constexpr uint32_t SBSDIO_WINDOW_SIZE         = 0x8000u;    // 32KB — макс. кусок без переключения окна

// Тактовая частота САМОГО ЧИПА (F1-регистр CHIPCLKCSR) — не путать с частотой
// хостовой SDIO-шины (EMMC_CONTROL1/sdio_set_clock_divider выше). Обязательный
// шаг перед любой РЕАЛЬНОЙ backplane bulk-передачей (см. wifi_request_alp_
// clock()/wifi_request_ht_clock() в wifi_driver.cpp и эталонный
// brcmf_sdio_buscoreprep()/brcmf_sdio_clkctl() в sdio.c/sdio.h:83-95).
constexpr uint32_t SBSDIO_FUNC1_CHIPCLKCSR    = 0x1000E;
constexpr uint32_t SBSDIO_FUNC1_SDIOPULLUP    = 0x1000F;
constexpr uint8_t  SBSDIO_FORCE_ALP           = 0x01;
constexpr uint8_t  SBSDIO_FORCE_HT            = 0x02;
constexpr uint8_t  SBSDIO_ALP_AVAIL_REQ       = 0x08;
constexpr uint8_t  SBSDIO_HT_AVAIL_REQ        = 0x10;
constexpr uint8_t  SBSDIO_FORCE_HW_CLKREQ_OFF = 0x20;
constexpr uint8_t  SBSDIO_ALP_AVAIL           = 0x40;
constexpr uint8_t  SBSDIO_HT_AVAIL            = 0x80;
constexpr uint8_t  SBSDIO_AVBITS              = SBSDIO_HT_AVAIL | SBSDIO_ALP_AVAIL;

// Размер блока функции 1 (FBR — Function Basic Register, НЕ F1-собственное
// адресное пространство — поэтому CMD52 сюда идёт с функцией 0, как и CCCR,
// см. sdio_f0_write_byte() в wifi_driver.cpp!). SDIO_FBR_BASE(1)=1*0x100,
// +0x10 = I/O Block Size (2 байта, LE). См. эталонный bcmsdh.c:
// "sdio_set_block_size(sdiodev->func1, SDIO_FUNC1_BLOCKSIZE)" — 64 байта,
// используется как размер блока для ВСЕХ последующих CMD53 block-mode
// передач (заливка прошивки/NVRAM). Раньше вся заливка шла байтовым режимом
// без этого шага — похоже, именно из-за этого чип возвращал Data CRC Error
// на первом же куске крупнее одного слова.
constexpr uint32_t SDIO_FBR_BASE_FUNC1     = 0x100;
constexpr uint32_t SDIO_FBR_BLKSIZE_OFFSET = 0x10;
constexpr uint32_t SDIO_FUNC1_BLOCKSIZE    = 64;

// Chipcommon (ядро #0) — фиксированный, известный адрес на backplane (НЕ через
// EROM — это отправная точка самого EROM-перечисления).
constexpr uint32_t SI_ENUM_BASE          = 0x18000000u;
constexpr uint32_t CHIPCOMMON_CHIPID_OFFSET  = 0x00; // chip ID + rev + packaging
constexpr uint32_t CHIPCOMMON_EROMPTR_OFFSET = 0xFC; // адрес таблицы EROM (перечисление остальных ядер)
constexpr uint32_t BRCM_CC_4345_CHIP_ID  = 0x4345;   // 43454/43455/43456 — общий Chip ID

// EROM (Enumeration ROM) — дескрипторы по 4 байта, тип в младших 4 битах.
constexpr uint32_t DMP_DESC_TYPE_MSK      = 0x0000000Fu;
constexpr uint32_t DMP_DESC_EMPTY         = 0x00000000u;
constexpr uint32_t DMP_DESC_VALID         = 0x00000001u; // бит "валидный дескриптор" (LSB)
constexpr uint32_t DMP_DESC_COMPONENT     = 0x00000001u;
constexpr uint32_t DMP_DESC_MASTER_PORT   = 0x00000003u;
constexpr uint32_t DMP_DESC_ADDRESS       = 0x00000005u;
constexpr uint32_t DMP_DESC_ADDRSIZE_GT32 = 0x00000008u; // доп. бит: есть ещё слово (64-бит адрес/размер)
constexpr uint32_t DMP_DESC_EOT           = 0x0000000Fu; // конец таблицы

constexpr uint32_t DMP_COMP_PARTNUM   = 0x000FFF00u; // Core ID
constexpr uint32_t DMP_COMP_PARTNUM_S = 8;
constexpr uint32_t DMP_COMP_NUM_SWRAP   = 0x00F80000u;
constexpr uint32_t DMP_COMP_NUM_SWRAP_S = 19;
constexpr uint32_t DMP_COMP_NUM_MWRAP   = 0x0007C000u;
constexpr uint32_t DMP_COMP_NUM_MWRAP_S = 14;

constexpr uint32_t DMP_SLAVE_ADDR_BASE   = 0xFFFFF000u; // база региона (адрес & эта маска)
constexpr uint32_t DMP_SLAVE_TYPE        = 0x000000C0u;
constexpr uint32_t DMP_SLAVE_TYPE_S      = 6;
constexpr uint32_t DMP_SLAVE_TYPE_SLAVE  = 0; // обычные регистры ядра -> "base"
constexpr uint32_t DMP_SLAVE_TYPE_SWRAP  = 2; // wrapper (IOCTL/reset-control) у обычного ядра -> "wrap"
constexpr uint32_t DMP_SLAVE_TYPE_MWRAP  = 3; // wrapper у ядра с master-портом (см. DMP_DESC_MASTER_PORT)
constexpr uint32_t DMP_SLAVE_SIZE_TYPE   = 0x00000030u;
constexpr uint32_t DMP_SLAVE_SIZE_TYPE_S = 4;
constexpr uint32_t DMP_SLAVE_SIZE_4K     = 0;
constexpr uint32_t DMP_SLAVE_SIZE_8K     = 1;
constexpr uint32_t DMP_SLAVE_SIZE_DESC   = 3; // размер не 4K/8K — следующее слово содержит явный размер, пропустить

// Core ID нужных нам ядер (см. bcma_regs.h/bcma.h).
constexpr uint32_t BCMA_CORE_ARM_CR4 = 0x83E; // ARM-ядро, исполняет прошивку
constexpr uint32_t BCMA_CORE_80211   = 0x812; // D11 — 802.11 MAC

// BCMA wrapper-регистры (офсеты ОТ wrapbase найденного через EROM).
constexpr uint32_t BCMA_IOCTL           = 0x408;
constexpr uint32_t BCMA_IOCTL_CLK       = 0x1;
constexpr uint32_t BCMA_IOCTL_FGC       = 0x2;
constexpr uint32_t BCMA_RESET_CTL       = 0x800;
constexpr uint32_t BCMA_RESET_CTL_RESET = 0x1;

constexpr uint32_t ARMCR4_BCMA_IOCTL_CPUHALT = 0x0020; // держит ARM CR4 в halt (не running), но вне reset
constexpr uint32_t D11_BCMA_IOCTL_PHYCLOCKEN = 0x0004;
constexpr uint32_t D11_BCMA_IOCTL_PHYRESET   = 0x0008;

// ARMCR4 TCM RAM — регистры офсетом ОТ base ядра CR4 (НЕ wrapbase), нужны
// только для вычисления реального размера RAM (адрес начала RAM хардкожен
// для этого чипа, см. BRCM_4345_RAMBASE).
constexpr uint32_t ARMCR4_CAP      = 0x04;
constexpr uint32_t ARMCR4_BANKIDX  = 0x40;
constexpr uint32_t ARMCR4_BANKINFO = 0x44;
constexpr uint32_t ARMCR4_TCBANB_MASK  = 0xFu;
constexpr uint32_t ARMCR4_TCBANB_SHIFT = 0;
constexpr uint32_t ARMCR4_TCBBNB_MASK  = 0xF0u;
constexpr uint32_t ARMCR4_TCBBNB_SHIFT = 4;
constexpr uint32_t ARMCR4_BSZ_MASK  = 0x3Fu;
constexpr uint32_t ARMCR4_BSZ_MULT  = 8192u;

// TCM RAM base для чипов 0x4345/43454 (brcmf_chip_tcm_rambase) — хардкод,
// т.к. зависит только от Chip ID, не вычисляется динамически.
constexpr uint32_t BRCM_4345_RAMBASE = 0x198000u;

// =====================================================================
// Wi-Fi (BCM43455) Милстоун 4.3 — sdpcm-канал (SDIO-функция 2) + CDC/BDC
// IOCTL. Все константы сверены построчно с эталонным sdio.c/bcdc.c/fwil.c
// (см. wifi_driver.cpp, шапка соответствующего блока) агентом-исследователем
// с цитатами file:line — не догадки. ---
// =====================================================================

// Ещё одно ядро для EROM-скана (см. wifi_erom_scan()) — "SDIO/PCMCIA core",
// шлюз к sdpcm-регистрам (intstatus/mailbox) ниже.
constexpr uint32_t BCMA_CORE_SDIO_DEV = 0x829;

// SDIO-функция 2 — потоковый FIFO-порт данных (НЕ окно в адресное
// пространство чипа, как F1 — SBSDIO_SB_*/backplane_set_window() сюда не
// относятся вообще).
constexpr uint32_t SBSDIO_FUNC_2        = 2;
constexpr uint32_t SDIO_FBR_BASE_FUNC2  = 0x200; // SDIO_FBR_BASE(2) = 2*0x100
constexpr uint32_t SDIO_FUNC2_BLOCKSIZE = 512;

// sdpcmd_regs — офсеты ОТ base ядра BCMA_CORE_SDIO_DEV, читаются/пишутся
// уже существующими backplane_read32/write32 (F1, тем же путём, что и
// chipcommon/CR4/D11 регистры в Милстоуне 4.2 — это обычные backplane-
// регистры, просто на другом ядре).
constexpr uint32_t SDPCMD_INTSTATUS         = 0x020;
constexpr uint32_t SDPCMD_HOSTINTMASK       = 0x024;
constexpr uint32_t SDPCMD_TOSBMAILBOX       = 0x040;
constexpr uint32_t SDPCMD_TOHOSTMAILBOX     = 0x044;
constexpr uint32_t SDPCMD_TOSBMAILBOXDATA   = 0x048;
constexpr uint32_t SDPCMD_TOHOSTMAILBOXDATA = 0x04C;

// intstatus/hostintmask биты.
constexpr uint32_t I_HMB_SW_MASK   = 0x000000f0u;
constexpr uint32_t I_HMB_FRAME_IND = (1u << 6);
constexpr uint32_t I_HMB_HOST_INT  = (1u << 7);
constexpr uint32_t I_CHIPACTIVE    = (1u << 29);
constexpr uint32_t HOSTINTMASK     = I_HMB_SW_MASK | I_CHIPACTIVE; // 0x200000f0

// tosbmailbox / tohostmailboxdata биты.
constexpr uint32_t SMB_INT_ACK       = 2u;
constexpr uint32_t HMB_DATA_FWHALT   = 0x0010u;
constexpr uint32_t HMB_DATA_DEVREADY = 0x0002u;
constexpr uint32_t HMB_DATA_FWREADY  = 0x0008u;

constexpr uint32_t SDPCM_PROT_VERSION     = 4;
constexpr uint32_t SMB_DATA_VERSION_SHIFT = 16;

// sdpcm software header — 12 байт (4 hwhdr + 8 swhdr), см. таблицу байт в
// wifi_driver.cpp (sdpcm_send_ctrl()/sdpcm_wait_and_read_ctrl()).
constexpr uint32_t SDPCM_HDRLEN          = 12;
constexpr uint32_t SDPCM_CONTROL_CHANNEL = 0;
constexpr uint32_t BRCMF_FIRSTREAD       = 64;

// BCDC dcmd envelope — 16 байт (cmd/len/flags/status, все __le32).
constexpr uint32_t BCDC_DCMD_ERROR    = 0x01u;
constexpr uint32_t BCDC_DCMD_SET      = 0x02u;
constexpr uint32_t BCDC_DCMD_IF_SHIFT = 12;
constexpr uint32_t BCDC_DCMD_ID_SHIFT = 16;

// dcmd-команды.
constexpr uint32_t BRCMF_C_GET_VERSION = 1;
constexpr uint32_t BRCMF_C_GET_VAR     = 262;
constexpr uint32_t BRCMF_C_SET_VAR     = 263;

// Watermark/MES — специфично для чипа BCM43455 ("CY_43455" в эталоне).
constexpr uint32_t SBSDIO_WATERMARK         = 0x10008;
constexpr uint32_t CY_43455_F2_WATERMARK    = 0x60;
constexpr uint32_t SBSDIO_DEVICE_CTL        = 0x10009;
constexpr uint32_t SBSDIO_DEVCTL_F2WM_ENAB  = 0x10;
constexpr uint32_t SBSDIO_FUNC1_MESBUSYCTRL = 0x1001D;
constexpr uint32_t CY_43455_MESBUSYCTRL     = 0xD0; // MES_WATERMARK(0x50)|MESBUSYCTRL_ENAB(0x80)

// =====================================================================
// Милстоун 4.4 — подключение к точке доступа (WPA2-PSK). Все значения
// сверены построчно с эталоном (cfg80211.c/fwil.h/fwil_types.h/fweh.h/
// bcdc.c) — см. ROADMAP.md/память проекта для деталей исследования.
// =====================================================================

// Ещё несколько dcmd-команд (см. BRCMF_C_GET_VERSION/GET_VAR/SET_VAR выше).
constexpr uint32_t BRCMF_C_UP           = 2;   // fwil.h — "включить радио" (brcmf_config_dongle(): "make sure RF is ready for work", значение 0 — не булев флаг, просто аргумент dcmd)
constexpr uint32_t BRCMF_C_SET_INFRA    = 20;  // fwil.h — режим интерфейса: infra=1 (Infrastructure/станция) vs 0 (IBSS/ad-hoc). brcmf_config_dongle() -> brcmf_cfg80211_change_iface() шлёт это ОДИН РАЗ при переходе интерфейса в UP — без этого шага прошивка, похоже, не понимает, что мы клиент, подключающийся к AP, и join молча не запускает реальную ассоциацию.
constexpr uint32_t BRCMF_C_SET_PM       = 86;  // fwil.h — power-management режим (PM_OFF=0/PM_FAST=2, defs.h)
constexpr uint32_t BRCMF_C_SET_SSID     = 26;  // fwil.h — raw dcmd, fallback-путь join (не используется, есть iovar "join")
constexpr uint32_t BRCMF_C_SET_WSEC_PMK = 268; // fwil.h — raw dcmd, отправка готового 32-байтного PMK

// wsec/wpa_auth биты (brcmu_wifi.h) — итоговое значение wpa_auth, которое
// реально "приживается" после brcmf_set_key_mgmt() в эталоне — 0x80 (одна
// только PSK-часть), а не промежуточное 0xC0 (PSK|UNSPECIFIED) из
// brcmf_set_wpa_version().
constexpr uint32_t WSEC_AES_ENABLED       = 0x0004u;
constexpr uint32_t WPA2_AUTH_PSK          = 0x0080u;
constexpr uint32_t BRCMF_WSEC_MAX_PSK_LEN = 32;

// struct brcmf_wsec_pmk_le (fwil_types.h): key_len(le16)+flags(le16)+
// key[2*32+1]. flags всегда 0 у эталона (передаём готовый бинарный PMK,
// НЕ ASCII-пароль — прошивка не поддерживает WSEC_PASSPHRASE в этом пути,
// см. память проекта) — PBKDF2 делаем сами (см. wifi_driver.cpp).
constexpr uint32_t BRCMF_WSEC_PMK_KEY_BUF_LEN = 2 * BRCMF_WSEC_MAX_PSK_LEN + 1; // 65
constexpr uint32_t BRCMF_WSEC_PMK_LE_LEN = 4 + BRCMF_WSEC_PMK_KEY_BUF_LEN; // 2+2+65 = 69

// --- Милстоун 4.4, раунд 3: полный brcmf_config_dongle() ---
// Найдено расхождение с эталоном: BRCMF_C_UP отправлялся у нас ПОСЛЕДНИМ (по
// итогам расследования предыдущего раунда, где казалось, что SET_INFRA
// сбрасывает интерфейс обратно в down) — но в эталоне (cfg80211.c,
// brcmf_config_dongle(), вызывается ОДИН РАЗ при открытии интерфейса, до
// вообще какой-либо попытки join) порядок ровно обратный: UP отправляется
// ПЕРВЫМ, а SET_INFRA — куда позже, через brcmf_cfg80211_change_iface(),
// вместе с ещё несколькими шагами, которые мы раньше не отправляли вовсе
// (scantime/roam/ARP-ND offload/FAKEFRAG). Раз простой перенос UP в конец
// не решил NOTUP на escan (см. память проекта) — реплицируем всю
// последовательность brcmf_config_dongle() как есть, а не только его
// часть.
constexpr uint32_t BRCMF_C_SET_ROAM_TRIGGER       = 55;  // fwil.h
constexpr uint32_t BRCMF_C_SET_ROAM_DELTA         = 57;  // fwil.h
constexpr uint32_t BRCMF_C_SET_SCAN_CHANNEL_TIME  = 185; // fwil.h
constexpr uint32_t BRCMF_C_SET_SCAN_UNASSOC_TIME  = 187; // fwil.h
constexpr uint32_t BRCMF_C_SET_FAKEFRAG           = 219; // fwil.h
constexpr uint32_t BRCMF_C_SET_SCAN_PASSIVE_TIME  = 258; // fwil.h

constexpr int32_t  WL_ROAM_TRIGGER_LEVEL = -75; // cfg80211.c, dBm
constexpr uint32_t WL_ROAM_DELTA         = 20;  // cfg80211.c
constexpr uint32_t BRCM_BAND_ALL         = 3;   // defs.h — оба параметра выше применяются сразу ко всем диапазонам
constexpr uint32_t BRCMF_DEFAULT_BCN_TIMEOUT_ROAM_ON = 2; // cfg80211.h — используем ROAM_ON-вариант: внутренний роуминг прошивки НЕ отключаем (roam_off=0), т.к. в этом порте нет wpa_supplicant, который взял бы это на себя
constexpr uint32_t BRCMF_SCAN_CHANNEL_TIME  = 40;  // cfg80211.c, мс
constexpr uint32_t BRCMF_SCAN_UNASSOC_TIME  = 40;  // cfg80211.c, мс
constexpr uint32_t BRCMF_SCAN_PASSIVE_TIME  = 120; // cfg80211.c, мс

// ARP/ND offload (core.c: brcmf_configure_arp_nd_offload()) — все шаги
// best-effort, ошибки в эталоне тоже не считаются фатальными ("may fail,
// then it is simply not supported").
constexpr uint32_t BRCMF_ARP_OL_AGENT           = 0x00000001u; // fwil_types.h
constexpr uint32_t BRCMF_ARP_OL_PEER_AUTO_REPLY = 0x00000008u; // fwil_types.h

// struct brcmf_ssid_le: SSID_len(le32) + SSID[32] (IEEE80211_MAX_SSID_LEN).
constexpr uint32_t IEEE80211_MAX_SSID_LEN = 32;
constexpr uint32_t BRCMF_SSID_LE_LEN = 4 + IEEE80211_MAX_SSID_LEN; // 36

// struct brcmf_join_scan_params_le: scan_type(u8)+3pad+nprobes/active_time/
// passive_time/home_time (все le32) = 20 байт.
constexpr uint32_t BRCMF_JOIN_SCAN_PARAMS_LE_LEN = 20;

// Реально уезжающий на провод размер struct brcmf_ext_join_params_le —
// ТОЛЬКО до конца assoc_le.chanspec_num (без chanspec_list[]), когда канал
// не указан (offsetof(ext_join_params_le,assoc_le) + offsetof(assoc_params_le,
// chanspec_list) в эталоне, cfg80211.c) — bssid(6)+pad(2)+chanspec_num(4)=12.
constexpr uint32_t BRCMF_ASSOC_PARAMS_LE_TRUNC_LEN = 12;
constexpr uint32_t BRCMF_EXT_JOIN_PARAMS_LE_LEN =
    BRCMF_SSID_LE_LEN + BRCMF_JOIN_SCAN_PARAMS_LE_LEN + BRCMF_ASSOC_PARAMS_LE_TRUNC_LEN; // 36+20+12 = 68

// Фаза 5.3 (least-privilege, см. situation.txt/ROADMAP.md): раньше ВСЁ Wi-Fi
// SHM (control-plane + link-state + TX/RX-мейлбокс + канарейка) жило на одной
// странице — при постраничных capability-правах (main.cpp, seL4_CapRights_new)
// это означало, что любой процесс с доступом к ХОТЬ ОДНОМУ Wi-Fi полю получал
// те же права на ВСЕ остальные, включая пароль. Теперь это 3 отдельные
// страницы с разной ролевой видимостью — см. shm_pages_mask_for_role()/
// shm_page_readonly_for_role() в main.cpp:
//   - control-plane (эта страница, 16384-20479): shell RW, wifi_driver RO
//     (wifi_driver больше не пишет сюда — зануление пароля после использования
//     перенесено в shell.cpp, см. его комментарий у WIFI_CMD_CONNECT).
//   - link-state (следующая страница): wifi_driver RW, net_driver/shell RO.
//   - TX/RX-мейлбокс+канарейка (ещё одна страница): net_driver/wifi_driver RW
//     (оба и пишут, и читают — consumer сам обнуляет длину как сигнал "забрал",
//     honest read-only тут недостижим).
constexpr uint32_t WIFI_SHM_SSID_LEN_OFFSET   = 16384; // 4 байта
constexpr uint32_t WIFI_SHM_SSID_OFFSET       = 16388; // 32 байта, до 16420
constexpr uint32_t WIFI_SHM_PASS_LEN_OFFSET   = 16420; // 4 байта
constexpr uint32_t WIFI_SHM_PASS_OFFSET       = 16424; // 64 байта, до 16488
constexpr uint32_t WIFI_SHM_VERBOSE_OFFSET    = 16488; // 1 байт — "-l" для фонового bring-up при "wifi start"

// Link-state — своя страница (20480-24575), read-only для net_driver/shell.
// issuse.txt ("без арбитража записи" — расследовано, реальной гонки нет):
// два писателя (wifi_driver при join/connect, root при "wifi stop"), но НЕ
// одновременно — SYS_STOP_WIFI (main.cpp) вызывает generic_recover_process()
// с respawn=false, который ДЕЛАЕТ seL4_TCB_Suspend(wifi_driver) ПЕРВЫМ ДЕЛОМ,
// и только ПОСЛЕ этого root сам пишет сюда 0 — к этому моменту wifi_driver
// физически не может исполнять ни одной инструкции, значит и писать тоже.
// Явного мьютекса это не заменяет (если когда-нибудь появится ВТОРОЙ путь
// записи от root без предварительной остановки wifi_driver — гонка станет
// реальной), но при текущем единственном вызывающем месте — безопасно.
constexpr uint32_t WIFI_SHM_LINK_STATE_OFFSET = 20480; // 4 байта, 0/1, пишет wifi_driver (join)/root (stop), читает net_driver/shell
constexpr uint32_t WIFI_SHM_MAC_OFFSET        = 20484; // 6 из 8 байт — РЕАЛЬНЫЙ MAC чипа (cur_etheraddr), не выдуманный как у GENET
constexpr uint32_t WIFI_SHM_LINK_STATE_REASON_OFFSET = 20492; // диагностика (см. situation.txt) — уже сослужила службу, оставлена как регресс-проверка
constexpr uint32_t WIFI_LINK_REASON_NONE           = 0;
constexpr uint32_t WIFI_LINK_REASON_STARTUP_RESET  = 1; // защитный сброс при (ре)старте wifi_driver
constexpr uint32_t WIFI_LINK_REASON_CONNECT_OK     = 2; // успешный WIFI_CMD_CONNECT
constexpr uint32_t WIFI_LINK_REASON_CONNECT_FAIL   = 3; // неуспешный WIFI_CMD_CONNECT
constexpr uint32_t WIFI_LINK_REASON_SYS_STOP_WIFI  = 4; // ручной "wifi stop" (main.cpp)

// TX/RX-мейлбокс + канарейка — своя страница (24576-28671), RW для net_driver
// И wifi_driver (двусторонний обмен, см. комментарий выше).
// issuse.txt ("порядок доступа явно не задокументирован" — расследовано и
// задокументировано здесь): net_driver и wifi_driver — РАЗНЫЕ процессы,
// потенциально на РАЗНЫХ ядрах (wifi_driver пришпилен к ядру 1, см.
// ROADMAP.md Фаза 6). "Данные, потом длина" при записи / "длина, потом
// данные" при чтении (см. wifi_hw_send/wifi_hw_poll_rx в net_driver.cpp,
// TX/RX-обработчики в wifi_driver.cpp) безопасно БЕЗ явных барьеров памяти
// ТОЛЬКО потому, что вся эта страница замаплена как Device-память
// (VMAttributes=0 у map_frame_robust() на обеих сторонах, см. main.cpp) —
// архитектура ARM гарантирует, что обращения к Device-памяти наблюдаются В
// ПОРЯДКЕ ПРОГРАММЫ ЛЮБЫМ наблюдателем в том же shareability domain, без
// барьера (то же свойство, на которое неявно полагается любой MMIO-драйвер
// на SMP-системе). Если эта страница когда-нибудь станет Normal/кэшируемой
// (например, ради производительности) — эта гарантия ИСЧЕЗНЕТ, и потребуются
// явные dmb/dsb вокруг каждой пары запись-данные/запись-флага.
constexpr uint32_t WIFI_SHM_TX_LEN_OFFSET     = 24576; // 4 байта, 0=пусто, пишет net_driver, читает wifi_driver
constexpr uint32_t WIFI_SHM_TX_DATA_OFFSET    = 24580; // 1536 байт, до 26116
constexpr uint32_t WIFI_SHM_RX_LEN_OFFSET     = 26116; // 4 байта, 0=пусто, пишет wifi_driver, читает net_driver
constexpr uint32_t WIFI_SHM_RX_DATA_OFFSET    = 26120; // 1536 байт, до 27656
constexpr uint32_t WIFI_SHM_FRAME_CAP         = 1536;  // максимальный размер одного кадра в TX/RX-мейлбоксах
// Канарейка-регресс-проверка — специально на этой странице (не на link-state),
// т.к. net_driver и так RW здесь (пишет TX_LEN) — на link-state net_driver
// теперь read-only, канарейка там сломала бы это урезание прав.
constexpr uint32_t WIFI_SHM_CANARY_OFFSET = 27656;
constexpr uint32_t WIFI_SHM_CANARY_MAGIC  = 0xC0FFEEEEu;

// blk_driver'ов staging-буфер (см. комментарий у первого появления этого
// класса бага — GENET rx_buffer_offsets[] пересечение) — переехал на
// отдельную страницу (28672-32767), чтобы освободить 20480 под Wi-Fi
// link-state выше (Фаза 5.3, раскладка страниц пересчитана).
constexpr uint32_t BLK_SHM_STAGING_OFFSET = 28672;

// usb_driver'ов staging-буфер (Фаза 14, Milestone 8) — зеркалит
// BLK_SHM_STAGING_OFFSET один-в-один: 9-я SHM-страница (32768-36863),
// отдельная от blk_driver'овской (28672-32767), чтобы echo>/mv на
// /mnt/usb0 не пересекались физической памятью с теми же операциями на
// SD-карте (тот же класс бага, что уже чинили для GENET/BLK).
constexpr uint32_t USB_SHM_STAGING_OFFSET = 32768;

// BCDC data-заголовок (4 байта: flags/priority/flags2/data_offset) — идёт
// ПЕРЕД полезной нагрузкой на DATA(2)/EVENT(1) sdpcm-каналах, в отличие от
// 16-байтного dcmd-заголовка на CONTROL(0)-канале (см. BCDC_DCMD_* выше).
constexpr uint32_t BCDC_HEADER_LEN     = 4;
constexpr uint32_t BCDC_PROTO_VER      = 2;
constexpr uint32_t BCDC_FLAG_VER_SHIFT = 4;
constexpr uint32_t BCDC_FLAG_SUM_GOOD  = 0x04u;

// sdpcm software-заголовок: номер канала (byte5 & 0x0F, см. SDPCM_CONTROL_
// CHANNEL=0 выше) — данные и асинхронные события прошивки идут по ДРУГИМ
// каналам, которые этот проект раньше вообще не читал.
constexpr uint32_t SDPCM_EVENT_CHANNEL = 1;
constexpr uint32_t SDPCM_DATA_CHANNEL  = 2;

// Заголовок события прошивки (fweh.h): ethhdr(14) + brcm_ethhdr(10) +
// brcmf_event_msg_be(48) = 72 байта, ВСЕ поля big-endian (в отличие от
// остального sdpcm/BCDC, который little-endian!).
constexpr uint16_t ETH_P_LINK_CTL            = 0x886C;
constexpr uint8_t  BCMILCP_BCM_SUBTYPE_EVENT = 1;
// BRCM_OUI = {0x00, 0x10, 0x18} — см. использование в wifi_driver.cpp.
constexpr uint32_t ETHHDR_LEN       = 14; // dest[6]+src[6]+proto(be16)
constexpr uint32_t BRCM_ETHHDR_LEN  = 10; // subtype/length(be16 each)+version(1)+oui[3]+usr_subtype(be16)
constexpr uint32_t BRCMF_EVENT_MSG_BE_LEN = 48;
constexpr uint32_t BRCMF_EVENT_HDR_LEN = ETHHDR_LEN + BRCM_ETHHDR_LEN + BRCMF_EVENT_MSG_BE_LEN; // 72

// Коды событий (fweh.h, brcmf_fweh_event_code) — только те, что нужны для
// отслеживания join/handshake (успех = SET_SSID/SUCCESS И PSK_SUP/
// FWSUP_COMPLETED оба увидены; см. brcmf_is_linkup/brcmf_is_nonetwork).
constexpr uint32_t BRCMF_E_SET_SSID     = 0;
constexpr uint32_t BRCMF_E_AUTH         = 3;
constexpr uint32_t BRCMF_E_DEAUTH       = 5;
constexpr uint32_t BRCMF_E_DEAUTH_IND   = 6;
constexpr uint32_t BRCMF_E_ASSOC_IND    = 8;
constexpr uint32_t BRCMF_E_DISASSOC_IND = 12;
constexpr uint32_t BRCMF_E_LINK         = 16;
constexpr uint32_t BRCMF_E_PSK_SUP      = 46;
constexpr uint32_t BRCMF_E_ESCAN_RESULT = 69; // диагностика: "видит ли прошивка вообще что-то в эфире" независимо от join
constexpr uint32_t BRCMF_E_IF           = 54; // fweh.h — единственный бит, который эталон явно включает в event_msgs в brcmf_c_preinit_dcmds()

// event_msgs (см. brcmf_c_preinit_dcmds(), common.c) — битовая маска
// разрешённых событий, BRCMF_EVENTING_MASK_LEN = ceil(BRCMF_E_LAST(139)/8) = 18
// байт. Мы никогда её не трогали (полагались на дефолт прошивки) — эталон
// явно читает-модифицирует-пишет её на самом раннем этапе, до всего
// остального (даже до brcmf_config_dongle()).
constexpr uint32_t BRCMF_EVENTING_MASK_LEN = 18;

// Прочие шаги из brcmf_c_preinit_dcmds() (common.c) — самая ранняя стадия
// инициализации в эталоне, ДО brcmf_config_dongle(). Мы её вообще не
// реализовывали (см. память проекта/ROADMAP.md — MAC-адрес/revinfo/
// event_msgs/mpc никогда не устанавливались).
constexpr uint32_t BRCMF_C_GET_REVINFO = 98; // fwil.h, raw dcmd (не iovar)

// Принудительная остановка скана в прошивке (brcmf_notify_escan_complete(),
// fw_abort=true в эталоне) — raw dcmd BRCMF_C_SCAN, а не iovar "escan".
// Абортится специальным сигнальным значением channel_list[0]=-1, а не
// вызовом "escan" с action=ABORT (см. wifi_escan_abort() в wifi_driver.cpp).
constexpr uint32_t BRCMF_C_SCAN = 50;

constexpr uint32_t BRCMF_E_STATUS_SUCCESS         = 0;
constexpr uint32_t BRCMF_E_STATUS_NO_NETWORKS     = 3;
constexpr uint32_t BRCMF_E_STATUS_PARTIAL         = 8;  // escan: промежуточный результат (эталон: brcmf_cfg80211_escan_handler)
constexpr uint32_t BRCMF_E_STATUS_FWSUP_COMPLETED = 6; // ТОЛЬКО для события PSK_SUP — то же числовое значение, что и UNSOLICITED для остальных событий, разный смысл в зависимости от event_type
constexpr uint32_t BRCMF_E_STATUS_FWSUP_TIMEOUT   = 7;
constexpr uint32_t BRCMF_EVENT_MSG_LINK = 0x01u;

// Диагностика: iovar "escan" (не raw dcmd BRCMF_C_SCAN=50 — тот в эталоне
// используется только для ABORT, реальный скан всегда идёт через "escan").
// struct brcmf_escan_params_le { version(le32); action(le16); sync_id(le16);
// struct brcmf_scan_params_le params_le; } — без каналов/SSID-массива (слепой
// скан всех каналов/SSID) params_le заканчивается на channel_num, БЕЗ
// channel_list (BRCMF_SCAN_PARAMS_FIXED_SIZE=64 — фиксированная часть).
constexpr uint32_t BRCMF_ESCAN_REQ_VERSION   = 1;
constexpr uint16_t WL_ESCAN_ACTION_START     = 1;
constexpr uint32_t DOT11_BSSTYPE_ANY         = 2;
constexpr uint32_t BRCMF_SCANTYPE_ACTIVE     = 0;
constexpr uint32_t BRCMF_SCANTYPE_PASSIVE    = 1;
constexpr uint32_t BRCMF_SCAN_PARAMS_FIXED_SIZE = 64; // brcmf_scan_params_le до channel_num включительно, без channel_list
constexpr uint32_t BRCMF_ESCAN_PARAMS_HDR_LEN   = 8;  // version(4)+action(2)+sync_id(2)
constexpr uint32_t BRCMF_ESCAN_BLIND_LEN = BRCMF_ESCAN_PARAMS_HDR_LEN + BRCMF_SCAN_PARAMS_FIXED_SIZE; // 72

// Разбор результата escan — struct brcmf_escan_result_le (fwil_types.h),
// приходит как ДАННЫЕ события BRCMF_E_ESCAN_RESULT (сразу после 48-байтного
// brcmf_event_msg_be, см. BRCMF_EVENT_MSG_BE_LEN выше), все поля LITTLE-endian
// (в отличие от самого event_msg_be, который big-endian):
//   buflen(le32) + version(le32) + sync_id(le16) + bss_count(le16) = 12 байт
// фиксированной части, затем один struct brcmf_bss_info_le (при bss_count>=1
// — на практике прошивка шлёт ровно один BSS на PARTIAL-событие).
constexpr uint32_t BRCMF_ESCAN_RESULT_FIXED_LEN  = 12;
constexpr uint32_t BRCMF_ESCAN_RESULT_SYNCID_OFF   = 8;  // le16, смещение от начала brcmf_escan_result_le
constexpr uint32_t BRCMF_ESCAN_RESULT_BSSCOUNT_OFF = 10; // le16, смещение от начала brcmf_escan_result_le

// struct brcmf_bss_info_le — смещения нужных полей ОТ начала самой структуры
// (т.е. от escan_result_off + BRCMF_ESCAN_RESULT_FIXED_LEN). Полный список
// полей см. fwil_types.h — берём только то, что показываем пользователю.
// ИСПРАВЛЕНО: структура в эталоне НЕ помечена __packed — компилятор вставляет
// выравнивающие байты перед каждым полем, размер которого >1 байт, если
// текущее смещение не кратно его размеру. Наивный расчёт "по сумме размеров
// полей" (chanspec=71/RSSI=76) давал channel=0/RSSI=0 на живом железе —
// сверено побайтово с реальным дампом bss_info (version=109 совпал,
// BSSID/SSID совпали до offset 51, но rateset.count оказался на offset 52,
// не 51 — то есть после SSID[32] вставлен 1 паддинг-байт для 4-байтного
// выравнивания вложенной rateset-структуры; аналогично после dtim_period
// перед RSSI). Реальные смещения (подтверждены: channel=42, RSSI=-84 —
// правдоподобные значения для настоящей эфирной сети):
//   ...SSID[32](19..50) + 1 паддинг -> rateset.count(52) + rates[16](56) ->
//   chanspec(72) + atim_window(74) + dtim_period(76) + 1 паддинг -> RSSI(78)
constexpr uint32_t BRCMF_BSS_INFO_BSSID_OFF      = 8;  // u8[6]
constexpr uint32_t BRCMF_BSS_INFO_SSID_LEN_OFF   = 18; // u8
constexpr uint32_t BRCMF_BSS_INFO_SSID_OFF       = 19; // u8[32]
constexpr uint32_t BRCMF_BSS_INFO_CHANSPEC_OFF   = 72; // le16
constexpr uint32_t BRCMF_BSS_INFO_RSSI_OFF       = 78; // le16 (интерпретируется как s16, дБм)
constexpr uint32_t BRCMF_BSS_INFO_FIXED_LEN      = 128; // с учётом выравнивания, до конца SNR-поля, без переменных IE
constexpr uint32_t BRCMF_CHANSPEC_CH_MASK        = 0x00FF; // brcmu_d11.h — номер канала, одинаково в D11N и D11AC форматах chanspec

// =====================================================================
// Raspberry Pi 4 (BCM2711) — реальные физические адреса.
//
// Извлечены из bcm2711-rpi-4-b.dtb (офиц. firmware, /boot),
// декомпилированного в bcm2711-rpi-4-b.dts (см. ROADMAP.md) командой:
//   dtc -I dtb -O dts bcm2711-rpi-4-b.dtb -o bcm2711-rpi-4-b.dts
//
// В DT используются "legacy VideoCore-шинные" адреса вида 0x7exxxxxx —
// это НЕ адреса, по которым реально видит память ARM-ядро. Пересчёт
// в ARM-физический адрес берётся из /soc/ranges и /scb/ranges того же
// .dts и сводится к двум формулам ("low peripheral mode", режим по
// умолчанию у RPi4/RPi400/CM4):
//   - шинные 0x7e000000..0x7fffffff (блок /soc, /emmc2bus) -> + 0x80000000
//   - шинные 0x7c000000..0x7fffffff (блок /scb: genet, pcie, xhci)
//                                                          -> + 0x80000000
//   - локальные ARM-периферия/GIC 0x40000000..0x407fffff (блок /soc,
//     interrupt-controller@40041000)                       -> + 0xbf800000
// т.е. итог совпадает: ARM_PADDR = BUS_ADDR + 0x80000000 для всех
// периферийных блоков /soc и /scb, и отдельная константа для GIC.
//
// IRQ ниже (SPI) — уже переведены из "сырого" SPI-номера в DT (interrupts =
// <0 N 4>) в итоговый GIC-номер по формуле GIC_IRQ = N + 32, т.к. это
// то представление, которое ожидает seL4_IRQControl_Get() (см. main.cpp,
// сравни с PLAT_UART_IRQ выше — та же конвенция для QEMU 'virt': DT SPI 1 ->
// 33). PPI (RPI4_TIMER_PPI_*) — отдельная формула, GIC_IRQ = 16 + PPI.
//
// UART (RPI4_UART1_MINIUART_PADDR/IRQ) и таймер (RPI4_TIMER_PPI_*, хоть и
// не MMIO/IRQ в итоге — см. platform.h выше) уже портированы, драйверы
// реально работают с этими данными. Остальное (EMMC2/GENET и т.д.) пока
// используется как справочный набор адресов под будущее переключение (см.
// ROADMAP.md, Фаза 3.2), плюс задел под периферию, которая пока не нужна
// ОС, но появится в дальнейшем (USB, Wi-Fi/BT, GPIO общего назначения,
// HDMI, аудио, PCIe/NVMe).
// =====================================================================

// --- GIC-400 (тот же PL011/GICv2 стек, что и в QEMU, другие адреса) ---
constexpr uintptr_t RPI4_GICD_PADDR = 0xff841000ULL;                    // Distributor,   DT 0x40041000
constexpr uintptr_t RPI4_GICC_PADDR = 0xff842000ULL;                    // CPU interface, DT 0x40042000
constexpr uintptr_t RPI4_GICH_PADDR = 0xff844000ULL;                    // Virt iface,    DT 0x40044000 (hypervisor, не нужен без виртуализации)
constexpr uintptr_t RPI4_GICV_PADDR = 0xff846000ULL;                    // Virt CPU iface,DT 0x40046000

// --- UART: на RPi4 их несколько. PL011 (UART0) по умолчанию (в стоковом
// bcm2711-rpi-4-b.dts, узел uart0_pins) разведён на GPIO32/33, которые
// физически ведут на Bluetooth-модуль, а не на 40-пиновый заголовок;
// mini-UART (UART1) по умолчанию сидит на GPIO14/15 (ALT5) и УЖЕ работает
// без какой-либо настройки с нашей стороны — используем именно его (см.
// PLAT_UART_PADDR выше). У SoC есть альтернативный пинмукс для PL011
// (узел uart0-gpio14, ALT0 на тех же GPIO14/15), который в Raspberry Pi OS
// включается через dtoverlay=disable-bt — но на практике это либо ломает
// наш elfloader (пробовали — плата переставала грузиться дальше
// "Starting application"), либо требует ручного включения тактовой
// частоты PL011 через VideoCore mailbox (пробовали — mailbox не отвечал,
// STATUS/READ читались одинаковым неизменным значением). mini-UART
// избавляет от обеих проблем ценой другого (не PrimeCell) регистрового
// формата — см. AUX_MU_* константы выше. ---
constexpr uintptr_t RPI4_UART0_PL011_PADDR = 0xfe201000ULL;             // DT serial@7e201000, "arm,pl011"
constexpr int        RPI4_UART0_PL011_IRQ  = 153;                       // DT SPI 0x79=121 -> GIC 121+32
constexpr uintptr_t RPI4_UART1_MINIUART_PADDR = 0xfe215040ULL;          // DT serial@7e215040, "brcm,bcm2835-aux-uart" (не PrimeCell, другой регистровый формат!)
constexpr int        RPI4_UART1_MINIUART_IRQ  = 125;                    // DT SPI 0x5d=93 -> GIC 93+32 (общий "aux" IRQ)

// --- ARM Generic Timer — заменяет отсутствующий PL031 (Фаза 3.1, готово).
// PPI, поэтому номер на всех ядрах одинаковый; GIC_IRQ = 16 + PPI.
// CNTVCT_EL0/CNTFRQ_EL0 читаются прямой mrs-инструкцией из EL0 без всякого
// IRQ/маппинга (см. hw_timer.cpp) — это НЕ то же самое, что регистры
// сравнения/управления (CNTP_CTL/CNTP_CVAL), которые и генерируют PPI.
// Раньше (до Фазы 4.5) те были недоступны с EL0 (EXPORT_PTMR_USER=false) —
// с Фазы 4.5 KernelArmExportPTMRUser включён (см. easy-settings.cmake) для
// НЕ-secure физического таймера (RPI4_TIMER_PPI_NONSECURE ниже), что даёт
// timer_driver.cpp настоящий событийный sys_sleep() вместо busy-poll (см.
// PLAT_TIMER_IRQ ниже). ВАЖНО: НЕ VTMR (виртуальный таймер, CNTV_CTL) — тот
// регистровый блок на этой сборке ядра (MCS выключен, ARM_HYP выключен)
// использует само ядро для своего тика планировщика (см.
// kernel/include/arch/arm/arch/64/mode/machine.h, CNT_CTL == CNTV_CTL) —
// дать userspace писать в него значило бы ломать планировщик ядра.
// ВАЖНО: константы ниже — это УЖЕ готовые GIC INTID (16 + сырой номер PPI из
// devicetree, см. комментарий "DT PPI N" у каждой — тот самый сырой номер,
// а не значение константы). Т.е. RPI4_TIMER_PPI_NONSECURE == 30 — это и есть
// "GIC_IRQ = 16 + PPI" для DT PPI 14 (16+14=30), UЖЕ посчитано. Раньше здесь
// ниже было "PLAT_TIMER_IRQ = 16 + RPI4_TIMER_PPI_NONSECURE" = 46 — двойной
// сдвиг на 16, из-за которого timer_driver.cpp слушал СОВСЕМ ДРУГОЙ (никем
// не используемый) IRQ и никогда не получал прерывание физического таймера
// (см. живое зависание sleep(), ROADMAP.md 4.5) — seL4_IRQControl_Get на 46
// при этом успешно "получалось" (это валидный, просто чужой номер), поэтому
// ошибка не была видна по check_err().
constexpr int RPI4_TIMER_PPI_SECURE       = 29;                         // DT PPI 13 (0x0d) — secure phys timer
constexpr int RPI4_TIMER_PPI_NONSECURE    = 30;                         // DT PPI 14 (0x0e) — non-secure phys timer (используется, см. PLAT_TIMER_IRQ)
constexpr int RPI4_TIMER_PPI_VIRTUAL      = 27;                         // DT PPI 11 (0x0b) — НЕ трогать, тик планировщика ядра
constexpr int RPI4_TIMER_PPI_HYP          = 26;                         // DT PPI 10 (0x0a)
constexpr int PLAT_TIMER_IRQ = RPI4_TIMER_PPI_NONSECURE;                // GIC IRQ = 30 (уже посчитано выше, БЕЗ дополнительного +16)

// --- Термодатчик: AVS RO thermal — единственный сенсор температуры кристалла
// на BCM2711, доступный без прошивки VideoCore (см. AVS_RO_TEMP_STATUS_OFFSET
// выше). Родительский узел в DT — syscon "avs-monitor", ребёнок — "thermal"
// (#thermal-sensor-cells=0, сам сенсор регистров не добавляет, только имя). ---
constexpr uintptr_t RPI4_AVS_MONITOR_PADDR = 0xfd5d2000ULL;             // DT /soc/avs-monitor@7d5d2000, "brcm,bcm2711-avs-monitor"
constexpr uintptr_t RPI4_AVS_MONITOR_SIZE  = 0xf00ULL;

// --- Диск: EMMC2 (Arasan SDHCI) — реальный контроллер SD-карты,
// заменяет virtio-blk. FAT32-логика поверх (см. ROADMAP.md) переиспользуется.
// Фаза 3.3 сделана: адрес/IRQ уже зеркалированы в активные PLAT_EMMC_* выше
// (blk_driver.cpp реально работает с этими адресами), здесь остаются только
// как справочные RPI4_-константы для единообразия с остальным блоком. ---
constexpr uintptr_t RPI4_EMMC2_PADDR = 0xfe340000ULL;                   // DT /emmc2bus/mmc@7e340000, "brcm,bcm2711-emmc2"
constexpr uintptr_t RPI4_EMMC2_SIZE  = 0x100ULL;
constexpr int        RPI4_EMMC2_IRQ  = 158;                             // DT SPI 0x7e=126 -> GIC 126+32

// --- Сеть: GENET v5 (BCM2711 встроенный Ethernet MAC) — заменяет
// virtio-net. ARP/UDP/NTP/DNS-логика поверх переиспользуется. Фаза 3.2
// сделана: адрес уже зеркалирован в активный PLAT_GENET_PADDR выше
// (net_driver.cpp реально работает с этим адресом), IRQ не используется
// (net_driver — чистый polling, как и раньше с virtio-net). ---
constexpr uintptr_t RPI4_GENET_PADDR      = 0xfd580000ULL;              // DT /scb/ethernet@7d580000, "brcm,bcm2711-genet-v5"
constexpr uintptr_t RPI4_GENET_SIZE       = 0x10000ULL;
constexpr int        RPI4_GENET_IRQ_A     = 189;                        // DT SPI 0x9d=157 -> GIC 157+32
constexpr int        RPI4_GENET_IRQ_B     = 190;                        // DT SPI 0x9e=158 -> GIC 158+32
constexpr uintptr_t RPI4_GENET_MDIO_OFFSET = 0xe14ULL;                  // MDIO (PHY-регистры) внутри блока genet

// --- USB host-контроллер (для клавиатуры/внешних накопителей помимо SD).
// Реальный физический USB-хаб/хост на RPi4 — VL805 xHCI, физически за
// PCIe. ПЕРВОНАЧАЛЬНО предполагалось, что DTB-алиас "generic-xhci"
// (0xfe9c0000, плоский MMIO-блок без PCIe-энумерации) остаётся живым и
// после старта U-Boot — ЖИВОЕ ЖЕЛЕЗО ЭТО ОПРОВЕРГЛО (см. ROADMAP.md, Фаза
// 14): как только U-Boot реально проэнумерировал PCI-шину (обязательный
// побочный эффект штатного boot-device сканирования USB-накопителей),
// старый алиас перестал отвечать на чтение (зависание всей системы на
// шинном уровне). ВТОРОЙ слой находки: "сырой" BAR0 из PCI config space
// (0xC0000000, см. "pci bar 01.00.00" в U-Boot) — это PCI-BUS-SIDE адрес,
// а НЕ CPU-физический; DT `ranges` PCIe-узла транслирует его в
// 0x600000000 (64-битное "высокое" PCIe-окно, вне видимости seL4 вообще).
// ПОПЫТКА перенацелить (единственное) outbound-окно моста через правку DT
// (на низкий CPU-адрес 0xfd600000) вызвала настоящий краш U-Boot на живом
// железе — откачена (см. ROADMAP.md, "Тринадцатая попытка"). Пятнадцатая
// попытка: PADDR ниже (0xC0000000, "сырой" PCI-bus-side BAR0) теперь
// АКТИВНО ИСПОЛЬЗУЕТСЯ usb_driver'ом как источник (PCIE_ADDR) для
// ВТОРОГО, самостоятельно прописываемого окна (индекс 1) — см.
// PLAT_XHCI_PADDR/PLAT_PCIE_RC_MISC_PADDR выше и usb_driver.cpp. U-Boot
// и DT в этой попытке не трогаются вообще. ---
constexpr uintptr_t RPI4_XHCI_PADDR = 0xC0000000ULL;                    // PCI-bus-side BAR0 (не CPU-физический адрес!) — см. предупреждение выше
constexpr uintptr_t RPI4_XHCI_SIZE  = 0x1000ULL;                        // подтверждено "pci bar" на живом железе — 4KB, не 1MB
constexpr int        RPI4_XHCI_IRQ  = 208;                              // DT SPI 0xb0=176 -> GIC 176+32
// Альтернатива/legacy — встроенный DWC2 OTG-контроллер (не используется,
// т.к. реальный хост-путь идёт через xHCI выше; оставлено для справки).
constexpr uintptr_t RPI4_USB_DWC2_PADDR = 0xfe980000ULL;                // DT /soc/usb@7e980000, "brcm,bcm2708-usb"
constexpr int        RPI4_USB_DWC2_IRQ_USB  = 105;                      // DT SPI 0x49=73  -> GIC 73+32
constexpr int        RPI4_USB_DWC2_IRQ_SOFT = 72;                       // DT SPI 0x28=40  -> GIC 40+32

// --- Wi-Fi / Bluetooth (BCM43455) — отдельный SDIO-стек, сильно
// отличается от genet. Wi-Fi висит на выделенном SD-хосте (SD1,
// non-removable, узел brcm,bcm4329-fmac), Bluetooth — на PL011 UART0
// выше (RPI4_UART0_PL011_PADDR), НЕ отдельный MMIO-блок. Реализация — вне
// рамок текущего порта (проприетарная прошивка + brcmfmac-протокол, на
// порядок больше EMMC2/GENET вместе), см. wifi_driver.cpp (заглушка,
// не участвует в сборке) для архитектурного задела. ---
constexpr uintptr_t RPI4_WIFI_SDIO_PADDR = 0xfe300000ULL;               // DT /soc/mmcnr@7e300000, "brcm,bcm2835-mmc" (SD1, wifi@1 = bcm4329-fmac)
constexpr uintptr_t RPI4_WIFI_SDIO_SIZE  = 0x100ULL;
constexpr int        RPI4_WIFI_SDIO_IRQ  = 158;                         // DT SPI 0x7e=126 -> GIC 126+32 (та же линия, что и EMMC2 — так в самом DT, см. bcm2711-rpi-4-b.dts)

// --- GPIO (общее управление пинами, для периферии сверх UART/сети/диска) ---
constexpr uintptr_t RPI4_GPIO_PADDR = 0xfe200000ULL;                    // DT /soc/gpio@7e200000, "brcm,bcm2711-gpio"
constexpr uintptr_t RPI4_GPIO_SIZE  = 0xb4ULL;
constexpr int        RPI4_GPIO_IRQ_BANK0 = 145;                         // DT SPI 0x71=113 -> GIC 113+32
constexpr int        RPI4_GPIO_IRQ_BANK1 = 146;                         // DT SPI 0x72=114 -> GIC 114+32

// --- HDMI (видеовывод — сейчас не нужен, ОС работает через serial) ---
constexpr uintptr_t RPI4_HDMI0_PADDR = 0xfef00700ULL;                   // DT /soc/hdmi@7ef00700, "brcm,bcm2711-hdmi0" (первый из неск. sub-reg: hdmi/dvp/phy/rm/packet/metadata/csc/cec/hd/intr2)
constexpr uintptr_t RPI4_HDMI1_PADDR = 0xfef05700ULL;                   // DT /soc/hdmi@7ef05700, "brcm,bcm2711-hdmi1"
constexpr uintptr_t RPI4_HDMI_L2_INTC_PADDR = 0xfef00100ULL;            // DT interrupt-controller@7ef00100 — вторичный контроллер для CEC/HPD hdmi-прерываний

// --- Аудио (PWM/I2S audio out — не используется) ---
constexpr uintptr_t RPI4_PWM0_PADDR = 0xfe20c000ULL;                    // DT /soc/pwm@7e20c000, "brcm,bcm2835-pwm"
constexpr uintptr_t RPI4_PWM1_PADDR = 0xfe20c800ULL;                    // DT /soc/pwm@7e20c800
constexpr uintptr_t RPI4_I2S_PADDR  = 0xfe203000ULL;                    // DT /soc/i2s@7e203000, "brcm,bcm2835-i2s"
constexpr uintptr_t RPI4_I2S_SIZE   = 0x24ULL;

// --- PCIe (внешняя шина RPi4 — задел под будущие ускорители/NVMe/сетевые
// карты; сам xHCI выше физически висит за этим же мостом, но виден
// отдельным плоским MMIO-блоком и не требует поднятия PCIe-энумерации). ---
constexpr uintptr_t RPI4_PCIE_PADDR   = 0xfd500000ULL;                  // DT /scb/pcie@7d500000, "brcm,bcm2711-pcie"
constexpr uintptr_t RPI4_PCIE_SIZE    = 0x9310ULL;
constexpr int        RPI4_PCIE_IRQ    = 179;                            // DT SPI 0x93=147 -> GIC 147+32 (основной, INTx)
constexpr int        RPI4_PCIE_MSI_IRQ = 180;                           // DT SPI 0x94=148 -> GIC 148+32 (MSI)
// NVMe (план на будущее) подключается как обычное PCIe NVMe-устройство
// через этот же корневой мост — отдельного физического адреса не имеет,
// его BAR настраивается динамически PCIe-энумерацией поверх RPI4_PCIE_PADDR.
