---
title: "RPi4 — JTAG + UART: методичка"
created: 2026-07-04
tags: [sel4, rpi4, jtag, uart, openocd, debugging]
---

# RPi4 — отладка через JTAG + UART одновременно

Оборудование: **FT232H** (JTAG) + отдельный **USB-UART адаптер** (уже есть в наличии). Хост для отладки — macOS.

---

## 1. Общая схема

```
                ┌──────────────┐
   USB ─────────┤   FT232H     │──── JTAG (6 линий) ────┐
                └──────────────┘                        │
                                                         ▼
   Mac ─────────────────────────────────────────── Raspberry Pi 4
                                                         ▲
                ┌──────────────┐                        │
   USB ─────────┤  USB-UART    │──── TX/RX/GND ─────────┘
                └──────────────┘
```

Два независимых физических устройства, не мешают друг другу — разные GPIO, разные шины.

**Важно:** одноканальный FT232H не может одновременно быть в MPSSE/JTAG-режиме и держать полноценный async UART на тех же C-пинах (ACBUS) — поэтому используем отдельный UART-адаптер, а не пытаемся выжать оба протокола из одного FT232H. Если бы хотелось «одна коробочка на всё» — нужен двухканальный FT2232H (канал A — JTAG, канал B — UART), но раз FT232H уже куплен, следуем этой схеме с двумя устройствами.

---

## 2. Распиновка

### JTAG (FT232H → RPi4 GPIO22-27, режим Alt4)

Фиксированная привязка GPIO ↔ JTAG-сигнал (задаётся кремнием BCM2711, не меняется):

| JTAG-сигнал | GPIO (RPi4) |
|---|---|
| TRST | 22 |
| RTCK | 23 |
| TDO  | 24 |
| TCK  | 25 |
| TDI  | 26 |
| TMS  | 27 |

Привязка на стороне FT232H — D0-D3 фиксированы аппаратно под MPSSE, D4-D7 свободные GPIOL (роль задаётся в конфиге OpenOCD):

| Пин FT232H | Сигнал | → GPIO (RPi4) |
| ---------- | ------ | ------------- |
| D0         | TCK    | 25            |
| D1         | TDI    | 26            |
| D2         | TDO    | 24            |
| D3         | TMS    | 27            |
| D4         | TRST   | 22            |
| D7         | RTCK   | 23            |
| GND        | GND    | GND           |

RTCK подключать обязательно — без него JTAG либо не коннектится, либо коннектится, но ведёт себя нестабильно после halt/resume (частая жалоба в комьюнити).

### UART (отдельный адаптер → RPi4 GPIO14/15)

```
TX (адаптер) → GPIO15 (RXD Pi)
RX (адаптер) → GPIO14 (TXD Pi)
GND          → GND
```
Крест-накрест (TX↔RX). Если в терминале тишина — первое, что проверять.

Baudrate: **115200 8N1** (стандарт для RPi UART, если в коде явно не задано иное).

---

## 3. config.txt на SD-карте

```ini
arm_64bit=1
kernel=u-boot.bin
enable_jtag_gpio=1
gpio=22-27=np
enable_uart=1
```

- `enable_jtag_gpio=1` — переводит GPIO22-27 в Alt4 (JTAG), включает JTAG-модуль на кристалле.
- `gpio=22-27=np` — **обязательно для RPi4** (в отличие от предыдущих моделей): по умолчанию все GPIO имеют pull-down, без снятия этой подтяжки для 22-27 сигнал TDI/TDO не пройдёт.
- `enable_uart=1` — включает UART0.

---

## 4. Установка на macOS

### OpenOCD

```bash
brew install openocd
```

Ставит бинарник + полный набор `.cfg` (interface/board/target). Путь к ним:
```bash
brew --prefix openocd
# /opt/homebrew/share/openocd/scripts/
```
Нужные конфиги:
- `target/bcm2711.cfg` — готовый, лежит из коробки.
- Свой интерфейс-конфиг под FT232H — создаём сами (см. §5), готового под нестандартную распиновку D4/D7 может не быть.

### macOS-специфичный шаг — обязателен перед КАЖДЫМ запуском OpenOCD

Системный драйвер macOS сам подхватывает FTDI-чип как serial-устройство и мешает OpenOCD получить прямой доступ:
```bash
sudo kextunload -b com.apple.driver.AppleUSBFTDI
```
Без этого — `Permission denied` или устройство не находится.

### GDB

```bash
brew install gdb
```
Современный gdb собирается multiarch по умолчанию — отдельный `gdb-multiarch` (как на Linux) не нужен, обычный `gdb` уже умеет `target remote` к aarch64.

---

## 5. Конфиг интерфейса OpenOCD (свой файл)

`ft232h-jtag.cfg`:
```tcl
adapter driver ftdi
ftdi_vid_pid 0x0403 0x6014
ftdi_channel 0
ftdi_layout_init 0x0008 0x001b
adapter speed 1000
transport select jtag
```

- `0x0403 0x6014` — VID/PID FT232H (проверить у себя: `system_profiler SPUSBDataType` после подключения → Future Technology Devices).
- `ftdi_layout_init` — начальное состояние линий D0-D7 (значение/direction mask). Приведённое значение — рабочий пример из практики под стандартную раскладку TCK/TDI/TMS/TRST как выходы, TDO вход. Если распиновка D4/D7 нестандартная — возможно, потребуется подобрать маску методом проб, если tap не находится с первого раза.

---

## 6. Запуск

### Только JTAG:
```bash
sudo openocd -f ft232h-jtag.cfg -f target/bcm2711.cfg
```
Признак успеха:
```
Info : JTAG tap: bcm2711.cpu tap/device found: 0x4ba00477
Info : bcm2711.cpu0: hardware has 6 breakpoints, 4 watchpoints
...
Info : Listening on port 3333 for gdb connections
```

### UART (отдельно, любой терминал):
```bash
screen /dev/tty.usbserial-XXXX 115200
# или
minicom -b 115200 /dev/tty.usbserial-XXXX
```

### GDB поверх JTAG:
```bash
gdb build/images/sel4test-driver-image-arm-bcm2711
(gdb) target remote localhost:3333
(gdb) break some_function
(gdb) continue
```

---

## 7. Известные питфолы

| Симптом | Причина / решение |
|---|---|
| OpenOCD не видит устройство / `Permission denied` | Забыт `kextunload -b com.apple.driver.AppleUSBFTDI` перед запуском |
| JTAG подключается, но глючит после halt/resume | Не подключён RTCK (D7 → GPIO23) |
| Тишина в UART-терминале | Перепутаны TX/RX местами — проверить крест-накрест |
| `IR capture error` / `all ones` при JTAG-скане | Плохой контакт/слишком длинные провода — использовать короткие (10-15 см), проверить GND |
| После halt OpenOCD показывает "Mode: Failed" при повторном коннекте | Известный баг OpenOCD на RPi4 — прервать (Ctrl-C) и запустить заново, второй раз обычно отрабатывает штатно |
| JTAG работает нестабильно на новой прошивке платы | Разные версии EEPROM firmware ведут себя по-разному — проверить/обновить через `rpi-eeprom-update`, либо (если не помогает) откатить |
| Плата должна быть уже запитана и пройти firmware boot до подключения OpenOCD | JTAG не заменяет serial на самых ранних стадиях (start4.elf/U-Boot) — подключаться только когда ARM-ядра уже стартовали |

---

## Статус

- [ ] Спаять/развести провода по таблице из §2
- [x] Прописать `config.txt` (§3)
- [x] Установить OpenOCD + GDB на Mac (§4)
- [ ] Создать `ft232h-jtag.cfg` (§5)
- [ ] Первый запуск OpenOCD — получить `tap found`
- [ ] Параллельно поднять UART-терминал
- [ ] Проверить gdb `target remote` + breakpoint
