#pragma once
#include <stdint.h>

// --- Флаги отладочных логов по компонентам. Гасят только рутинные
// info/diagnostic-сообщения (регистровые дампы, "X initialized" и т.п.) —
// ошибки/предупреждения печатаются всегда, независимо от этих флагов.
// Включайте по одному, когда реально отлаживаете конкретный драйвер на
// живом железе — не держите все разом, иначе лог захламляется. ---
constexpr bool LOG_BRINGUP = false; // main.cpp: alloc_device_frame() дампы найденных untyped-регионов
constexpr bool LOG_UART    = false;
constexpr bool LOG_TIMER   = false;
constexpr bool LOG_BLK     = false; // blk_driver.cpp: пошаговые дампы регистров EMMC2 при инициализации
constexpr bool LOG_NET     = true;  // net_driver.cpp — самый свежий/менее обкатанный компонент

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

// --- Виртуальные адреса, куда эти устройства маппятся в VSpace драйвера
// (см. hw_vaddr в spawn_process(), main.cpp). Общие для всех процессов —
// каждый драйвер видит свое устройство по одному и тому же литералу. ---
constexpr uintptr_t PLAT_UART_VADDR         = 0x200000000ULL;
constexpr uintptr_t PLAT_EMMC_VADDR         = 0x200006000ULL;
constexpr uintptr_t PLAT_GENET_VADDR        = 0x200008000ULL;

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
constexpr uint32_t EMMC_INT_ERROR_MASK = 0xFFFF0000u; // Любая ошибка (Command/Data Error Status, верхние 16 бит)
constexpr uint32_t EMMC_INT_ALL_EN     = 0xFFFFFFFFu; // Маска "разрешить всё" для IRPT_MASK/IRPT_EN (нужна для чтения статусов даже без реальных IRQ)

// CONTROL0 (0x28) — базовая настройка хоста
constexpr uint32_t EMMC_C0_USE_4BIT    = (1u << 1);   // Ширина шины 4 бита (не используется в первой версии — см. план)
// SD Bus Power (bits 8-11 в CONTROL0, аналог legacy SDHCI "Power Control"
// байта на 0x29): SRST_HC гасит питание шины, найдено эмпирически на живом
// железе (до сброса bits 8-11 = 0xF, после — 0x0) — без этого CMD_INHIBIT
// висит вечно и ни одна команда никогда не завершается.
constexpr uint32_t EMMC_C0_PWR_ON      = (1u << 8);
constexpr uint32_t EMMC_C0_PWR_3V3     = (0x7u << 9);

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
constexpr uint32_t EMMC_TM_BLKCNT_EN   = (1u << 1);
constexpr uint32_t EMMC_TM_MULTI_BLOCK = (1u << 5);
constexpr uint32_t EMMC_TM_DAT_DIR_READ = (1u << 4);    // 1 = card->host (чтение)

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
// PPI, поэтому номер на всех ядрах одинаковый; GIC_IRQ = 16 + PPI. На
// практике не используются: CNTVCT_EL0/CNTFRQ_EL0 читаются прямой
// mrs-инструкцией из EL0 без всякого IRQ/маппинга (см. hw_timer.cpp), а
// регистры сравнения/управления (которые и генерировали бы эти PPI) с EL0
// на этой сборке ядра недоступны (EXPORT_PTMR_USER/VTMR_USER=false в
// gen_config) — оставлены как справочные на случай смены конфигурации ядра
// (например, MCS + scheduling contexts) в будущем. ---
constexpr int RPI4_TIMER_PPI_SECURE       = 29;                         // DT PPI 13 (0x0d) — secure phys timer
constexpr int RPI4_TIMER_PPI_NONSECURE    = 30;                         // DT PPI 14 (0x0e) — non-secure phys timer
constexpr int RPI4_TIMER_PPI_VIRTUAL      = 27;                         // DT PPI 11 (0x0b)
constexpr int RPI4_TIMER_PPI_HYP          = 26;                         // DT PPI 10 (0x0a)

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
// Реальный физический USB-хаб/хост на RPi4 — VL805 xHCI, который
// firmware настраивает так, что регистры видны как плоский MMIO-блок
// без необходимости поднимать сам PCIe (тот же трюк использует
// U-Boot/Linux до полной инициализации PCIe-моста). ---
constexpr uintptr_t RPI4_XHCI_PADDR = 0xfe9c0000ULL;                    // DT /scb/xhci@7e9c0000, "generic-xhci"
constexpr uintptr_t RPI4_XHCI_SIZE  = 0x100000ULL;
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
