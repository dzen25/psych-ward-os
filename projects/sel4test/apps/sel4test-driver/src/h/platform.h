#pragma once
#include <stdint.h>

// =====================================================================
// Платформенно-зависимый слой.
//
// Все физические/виртуальные адреса устройств, номера IRQ и оффсеты
// регистров, специфичные для ТЕКУЩЕЙ целевой машины (QEMU 'virt',
// AArch64/ARMv8: PL011 UART + PL031 RTC + virtio-mmio), собраны здесь.
// Драйверы (main.cpp, uart_driver.cpp, timer_driver.cpp, blk_driver.cpp,
// net_driver.cpp) ссылаются только на эти константы, а не на "магические"
// адреса напрямую по коду.
//
// При портировании на другую платформу (например, Raspberry Pi 4 —
// BCM2711, тоже AArch64, но другой UART/RTC/сеть без virtio) нужно менять
// именно этот файл, а не искать адреса по всем .cpp. Часть констант ниже
// (virtio-mmio) на платформе без virtio просто не понадобится вовсе —
// это сигнал, какие драйверы придется переписывать целиком, а не адаптировать.
// =====================================================================

// --- Физические адреса устройств (см. alloc_device_frame() в main.cpp) ---
constexpr uintptr_t PLAT_UART_PADDR        = 0x09000000ULL; // PL011
constexpr uintptr_t PLAT_RTC_PADDR         = 0x09010000ULL; // PL031
constexpr uintptr_t PLAT_VIRTIO_MMIO_PADDR = 0x0a000000ULL; // База слотов virtio-mmio
constexpr int        PLAT_VIRTIO_MMIO_SLOTS = 4;             // Сколько слотов резервирует rootserver

// --- Виртуальные адреса, куда эти устройства маппятся в VSpace драйвера
// (см. hw_vaddr в spawn_process(), main.cpp). Общие для всех процессов —
// каждый драйвер видит свое устройство по одному и тому же литералу. ---
constexpr uintptr_t PLAT_UART_VADDR         = 0x200000000ULL;
constexpr uintptr_t PLAT_RTC_VADDR          = 0x200002000ULL;
constexpr uintptr_t PLAT_VIRTIO_MMIO_VADDR  = 0x200004000ULL;
constexpr uintptr_t PLAT_VIRTIO_MMIO_STRIDE = 0x200ULL; // Шаг перебора слотов при поиске устройства

// --- Оффсеты регистров внутри блоков (ARM PrimeCell PL011/PL031) ---
constexpr uintptr_t PL011_DR_OFFSET   = 0x00; // Data Register
constexpr uintptr_t PL011_FR_OFFSET   = 0x18; // Flag Register
constexpr uintptr_t PL011_IMSC_OFFSET = 0x38; // Interrupt Mask Set/Clear
constexpr uintptr_t PL011_ICR_OFFSET  = 0x44; // Interrupt Clear Register
constexpr uint32_t  PL011_FR_TXFF     = (1u << 5); // TX FIFO full
constexpr uint32_t  PL011_FR_RXFE     = (1u << 4); // RX FIFO empty
constexpr uint32_t  PL011_INT_RX_BIT  = (1u << 4); // Бит Receive Interrupt в IMSC/ICR

// Оффсеты по датащиту PL031 (RTCDR=0x00, RTCMR=0x04, RTCIMSC=0x10, RTCICR=0x1C).
constexpr uintptr_t PL031_DR_OFFSET   = 0x00; // Data Register (текущее время, сек. с эпохи)
constexpr uintptr_t PL031_MR_OFFSET   = 0x04; // Match Register
constexpr uintptr_t PL031_IMSC_OFFSET = 0x10; // Interrupt Mask Set/Clear
constexpr uintptr_t PL031_ICR_OFFSET  = 0x1C; // Interrupt Clear Register
// ВНИМАНИЕ: timer_driver.cpp исторически читает/пишет IMSC (0x10) там, где по
// датащиту нужен был бы ICR (0x1C), но это существующее (работающее в QEMU)
// поведение — сохранено как есть, здесь только вынесено значение в константу.

// --- Номера IRQ (GIC SPI), см. seL4_IRQControl_Get() в main.cpp ---
constexpr int PLAT_UART_IRQ  = 33;
constexpr int PLAT_TIMER_IRQ = 34;

// --- virtio-mmio: константы самой спецификации virtio (не завязаны на
// плату), но актуальны только пока есть virtio-net/virtio-blk. На
// платформе с реальными контроллерами (Ethernet MAC, SDHCI и т.п.) этот
// блок целиком уходит вместе с соответствующим кодом сканирования. ---
constexpr uint32_t VIRTIO_MMIO_MAGIC      = 0x74726976u; // "virt" в LE
constexpr uint32_t VIRTIO_DEVICE_ID_NET   = 1;
constexpr uint32_t VIRTIO_DEVICE_ID_BLOCK = 2;
