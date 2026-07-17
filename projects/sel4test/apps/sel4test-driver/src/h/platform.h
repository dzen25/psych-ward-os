#pragma once
#include <stdint.h>

// =====================================================================
// Платформенно-зависимый слой.
//
// Целевая платформа — Raspberry Pi 4 (BCM2711), AArch64/ARMv8. Порт идёт
// поэтапно (см. ROADMAP.md, Фаза 3): UART уже переключён на реальные
// адреса железа — mini-UART (UART1), а не PL011 (UART0, физически отдан
// Bluetooth-модулю и требует либо dtoverlay=disable-bt в config.txt, либо
// ручного включения тактовой частоты через VideoCore mailbox — оба пути
// опробованы и оба не сработали на этой прошивке/сборке, см. историю в
// ROADMAP.md/README.md). RTC и virtio-mmio (сеть/диск) — ещё нет, там
// сохранены значения под QEMU 'virt', пока
// timer_driver.cpp/blk_driver.cpp/net_driver.cpp не переписаны под
// ARM generic timer / GENET / EMMC2 (блокеры Фазы 3.1-3.3 в ROADMAP.md).
// Драйверы (main.cpp, uart_driver.cpp, timer_driver.cpp, blk_driver.cpp,
// net_driver.cpp) ссылаются только на эти константы, а не на "магические"
// адреса напрямую по коду — так что после переписывания RTC/virtio-mmio
// драйверов останется поменять только значения здесь.
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
constexpr uintptr_t PLAT_RTC_PADDR         = 0x09010000ULL;             // PL031 (QEMU virt) — на RPi4 нет, блокер Фазы 3.1
constexpr uintptr_t PLAT_VIRTIO_MMIO_PADDR = 0x0a000000ULL;             // virtio-mmio (QEMU virt) — на RPi4 нет, блокеры Фазы 3.2/3.3
constexpr int        PLAT_VIRTIO_MMIO_SLOTS = 4;                        // Сколько слотов резервирует rootserver

// --- Виртуальные адреса, куда эти устройства маппятся в VSpace драйвера
// (см. hw_vaddr в spawn_process(), main.cpp). Общие для всех процессов —
// каждый драйвер видит свое устройство по одному и тому же литералу. ---
constexpr uintptr_t PLAT_UART_VADDR         = 0x200000000ULL;
constexpr uintptr_t PLAT_RTC_VADDR          = 0x200002000ULL;
constexpr uintptr_t PLAT_VIRTIO_MMIO_VADDR  = 0x200004000ULL;
constexpr uintptr_t PLAT_VIRTIO_MMIO_STRIDE = 0x200ULL; // Шаг перебора слотов при поиске устройства

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

// Оффсеты по датащиту PL031 (RTCDR=0x00, RTCMR=0x04, RTCIMSC=0x10, RTCICR=0x1C).
constexpr uintptr_t PL031_DR_OFFSET   = 0x00;                           // Data Register (текущее время, сек. с эпохи)
constexpr uintptr_t PL031_MR_OFFSET   = 0x04;                           // Match Register
constexpr uintptr_t PL031_IMSC_OFFSET = 0x10;                           // Interrupt Mask Set/Clear
constexpr uintptr_t PL031_ICR_OFFSET  = 0x1C;                           // Interrupt Clear Register
// ВНИМАНИЕ: timer_driver.cpp исторически читает/пишет IMSC (0x10) там, где по
// датащиту нужен был бы ICR (0x1C), но это существующее (работающее в QEMU)
// поведение — сохранено как есть, здесь только вынесено значение в константу.

// --- Номера IRQ (GIC SPI), см. seL4_IRQControl_Get() в main.cpp ---
constexpr int PLAT_UART_IRQ  = 125;                                     // RPi4: mini-UART, см. RPI4_UART1_MINIUART_IRQ ниже
                                                                          // (общий "aux" IRQ на mini-UART/SPI1/SPI2)
constexpr int PLAT_TIMER_IRQ = 34;                                      // PL031 (QEMU virt) — блокер Фазы 3.1, см. PLAT_RTC_PADDR

// --- virtio-mmio: константы самой спецификации virtio (не завязаны на
// плату), но актуальны только пока есть virtio-net/virtio-blk. На
// платформе с реальными контроллерами (Ethernet MAC, SDHCI и т.п.) этот
// блок целиком уходит вместе с соответствующим кодом сканирования. ---
constexpr uint32_t VIRTIO_MMIO_MAGIC      = 0x74726976u;                // "virt" в LE
constexpr uint32_t VIRTIO_DEVICE_ID_NET   = 1;
constexpr uint32_t VIRTIO_DEVICE_ID_BLOCK = 2;

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
// IRQ ниже — уже переведены из "сырого" SPI-номера в DT (interrupts =
// <0 N 4>) в итоговый GIC-номер по формуле GIC_IRQ = N + 32, т.к. это
// то представление, которое ожидает seL4_IRQControl_Get() (см. main.cpp,
// сравни с PLAT_UART_IRQ/PLAT_TIMER_IRQ выше — там та же конвенция для
// QEMU 'virt': DT SPI 1 -> 33, SPI 2 -> 34).
//
// UART (RPI4_UART1_MINIUART_PADDR/IRQ) уже зеркалирован в активные
// PLAT_UART_* выше — драйверы (main.cpp/uart_driver.cpp) реально работают
// с этими адресами. Остальное (RTC/EMMC2/GENET и т.д.) пока используется
// только как справочный набор адресов под будущее переключение (см.
// ROADMAP.md, Фаза 3.1-3.3), плюс задел под периферию, которая пока не
// нужна ОС, но появится в дальнейшем (USB, Wi-Fi/BT, GPIO общего
// назначения, HDMI, аудио, PCIe/NVMe).
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

// --- ARM Generic Timer (CNTPCT_EL0) — заменяет отсутствующий PL031.
// PPI, поэтому номер на всех ядрах одинаковый; GIC_IRQ = 16 + PPI. ---
constexpr int RPI4_TIMER_PPI_SECURE       = 29;                         // DT PPI 13 (0x0d) — secure phys timer
constexpr int RPI4_TIMER_PPI_NONSECURE    = 30;                         // DT PPI 14 (0x0e) — non-secure phys timer (этот используем из EL1)
constexpr int RPI4_TIMER_PPI_VIRTUAL      = 27;                         // DT PPI 11 (0x0b)
constexpr int RPI4_TIMER_PPI_HYP          = 26;                         // DT PPI 10 (0x0a)

// --- Диск: EMMC2 (Arasan SDHCI) — реальный контроллер SD-карты,
// заменяет virtio-blk. FAT32-логика поверх (см. ROADMAP.md) переиспользуется. ---
constexpr uintptr_t RPI4_EMMC2_PADDR = 0xfe340000ULL;                   // DT /emmc2bus/mmc@7e340000, "brcm,bcm2711-emmc2"
constexpr uintptr_t RPI4_EMMC2_SIZE  = 0x100ULL;
constexpr int        RPI4_EMMC2_IRQ  = 158;                             // DT SPI 0x7e=126 -> GIC 126+32

// --- Сеть: GENET v5 (BCM2711 встроенный Ethernet MAC) — заменяет
// virtio-net. ARP/UDP/NTP/DNS-логика поверх переиспользуется. ---
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
// выше (RPI4_UART0_PL011_PADDR), НЕ отдельный MMIO-блок. ---
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
