---
title: "RPi4 — отладочный стенд на одной плате: RP2040 (JTAG + UART)"
created: 2026-07-14
tags: [sel4, rpi4, rp2040, dirtyjtag, jtag, uart, debugging]
---

# Отладочный стенд для RPi4 на одной плате RP2040 (DirtyJTAG)

Одна плата, оба канала — JTAG и UART, оба проводные, через один USB-кабель к Mac.

```
Mac ──USB──── RP2040 (DirtyJTAG: JTAG + UART) ──7 проводов──> RPi4
```

Компьютер видит одно USB-устройство с двумя интерфейсами: JTAG (для OpenOCD/openFPGALoader) и обычный `/dev/tty.usbmodemXXXX` для serial-консоли — оба канала идут по одному кабелю.

---

<details>
<summary><b>1. Репозиторий и сборка прошивки (клик, чтобы развернуть)</b></summary>

Необходимо установить Pico SDK:

```bash
cd ~
git clone -b master https://github.com/raspberrypi/pico-sdk.git
cd pico-sdk
git submodule update --init
export PICO_SDK_PATH=~/pico-sdk
sudo apt install -y gcc-arm-none-eabi
```

```bash
git clone https://github.com/phdussud/pico-dirtyJtag.git
cd pico-dirtyJtag
mkdir build && cd build
cmake ..
make -j$(nproc)
```
Результат — `dirtyJtag.uf2` в папке `build/`.

</details>

---

<details>
<summary><b>2. Конфигурация под свою плату (клик, чтобы развернуть)</b></summary>

Файл: `dirtyJtagConfig.h` — прошивка собирается под одну-единственную плату (`BOARD_MYBOARD`), без выбора через `#elif`/`cmake -DBOARD_TYPE=...`: всё зашито напрямую, лишние блоки под другие платы из апстрима удалены.

Где менять номера пинов: `pico-dirtyJtag/dirtyJtagConfig.h` — макросы `PIN_TDI/TDO/TCK/TMS/RST/TRST`, `PIN_UART0_TX/RX` и `PIN_LED_TX/RX/ERROR`, `PIN_LED_JTAG_ACT/ERROR`. Значения LED сейчас `-1` (отключены) — заменить на реальные GPIO после разводки/пайки платы, ориентируясь на предварительное назначение GPIO6-10 из §4.

```c
// BOARD_QMTECH_RP2040_DAUGHTERBOARD оставлен как константа - на её значение
// ссылаются pio_jtag.c/dirtyJtag.c, переиспользовать этот номер нельзя.
#define BOARD_QMTECH_RP2040_DAUGHTERBOARD 3
#define BOARD_MYBOARD 6

#define BOARD_TYPE BOARD_MYBOARD

#define PIN_TDI  21
#define PIN_TDO  22
#define PIN_TCK  24
#define PIN_TMS  25
#define PIN_RST  26   // system reset цели - для RPi4 не используется, JTAG-гребёнка его не имеет
#define PIN_TRST 27

#define CDC_UART_INTF_COUNT 1
#define PIN_UART0       uart0
#define PIN_UART0_TX    1
#define PIN_UART0_RX    2

// LED - см. §4. Номера GPIO предварительные (этап схемотехники),
// после разводки платы подставить фактические.
#define LED_INVERTED   0
#define PIN_LED_TX     -1   // зелёный (UART TX)
#define PIN_LED_RX     -1   // жёлтый (UART RX)
#define PIN_LED_ERROR  -1   // красный (пара с UART)
#define PIN_LED_JTAG_ACT    -1   // синий
#define PIN_LED_JTAG_ERROR  -1   // красный (пара с JTAG)
```

Сборка — как обычно, без флага `-DBOARD_TYPE`:
```bash
cmake ..
make -j$(nproc)
```

</details>

---

## 3. Пины GND на плате

Указанные GND-точки платы: **3, 23, 8, 33**. Один из них — на общий GND с RPi4 (обязательно для корректной работы любых цифровых сигналов между независимо запитанными устройствами).

---

<details>
<summary><b>4. LED-индикация — 5 светодиодов, 2 группы (клик, чтобы развернуть)</b></summary>

| LED | Цвет | GPIO | Группа | Сигнализирует |
|---|---|---|---|---|
| UART RX | жёлтый | GPIO6 | UART | приём байта с UART |
| UART TX | зелёный | GPIO7 | UART | передача байта на UART |
| UART Error | красный | GPIO8 | UART | framing/parity/overrun/break ошибка |
| JTAG Activity | синий | GPIO9 | JTAG | активность TCK/scan-операция (CMD_XFER/CMD_CLK) |
| JTAG Error | красный | GPIO10 | JTAG | повреждённый/нераспознанный байт команды от хоста |

Группы физически стоят парами рядом — UART-тройка (GPIO6-8) отдельно, JTAG-пара (GPIO9-10) отдельно, для интуитивного визуального чтения состояния стенда. GPIO6-10 — свободный непрерывный блок пинов, не занятый JTAG (21,22,24-27) и UART0 (1,2), удобно разводить одной группой на плате.

**Подключение — какой пин куда:**
```
RP2040 GPIO6  → R 330Ω → анод LED UART RX (жёлтый)      → катод → GND
RP2040 GPIO7  → R 330Ω → анод LED UART TX (зелёный)     → катод → GND
RP2040 GPIO8  → R 330Ω → анод LED UART Error (красный)  → катод → GND
RP2040 GPIO9  → R 330Ω → анод LED JTAG Activity (синий) → катод → GND
RP2040 GPIO10 → R 330Ω → анод LED JTAG Error (красный)  → катод → GND
```
Все 5 катодов — на общий GND (см. §3: любая из точек 3/23/8/33, можно свести все на одну).

`LED_INVERTED = 0` → GPIO активен высоким уровнем (диод горит при `1`, гасится при `0`), поэтому анод каждого LED смотрит на свой резистор/GPIO, катод — на GND. Резистор 330Ω — общий номинал для всех пяти (RP2040 держит по умолчанию ~4мА на пин): для красного/жёлтого/зелёного (Vf≈2.0-2.2В) даёт ток ~3.5-4мА, для синего (Vf≈3.0В) — ~0.9мА (светит тусклее остальных, но заметно). Если нужна одинаковая яркость — уменьшить резистор синего LED (например до 150Ω) после выбора конкретной детали и Vf/If из её даташита.

GPIO6-10 — предварительное назначение на этапе схемотехники, до физической разводки платы. После пайки перенести те же (или скорректированные под факт разводки) номера в `PIN_LED_*` блока `BOARD_MYBOARD` в `dirtyJtagConfig.h` (сейчас там `-1` — LED отключены, см. §2).

**Логика мигания:**
```c
// Короткая вспышка (~30мс) на каждый байт/транзакцию - обычный трафик
void onUartTx(uint8_t byte) { gpio_put(PIN_LED_UART_TX, 1); /* таймер на выключение через BLINK_MS */ }
void onUartRx(uint8_t byte) { gpio_put(PIN_LED_UART_RX, 1); /* таймер на выключение через BLINK_MS */ }
void onJtagActivity()       { gpio_put(PIN_LED_JTAG_ACT, 1); /* таймер на выключение через BLINK_MS */ }

// Устойчивый паттерн, отличимый от обычного трафика - сигнал проблемы
void onUartError() { gpio_put(PIN_LED_UART_ERROR, 1); /* держать дольше или мигнуть 3 раза быстро */ }
void onJtagError()  { gpio_put(PIN_LED_JTAG_ERROR, 1); }
```

</details>

<details>
<summary><b>Что было изменено в клонированном репозитории (клик, чтобы развернуть)</b></summary>

Хуки нужно встроить в основной код репозитория:
- `onUartTx`/`onUartRx`/`onUartError` — там, где реализован UART CDC bridge (искать по использованию `PIN_UART0_TX`/`PIN_UART0_RX` в `.c`-файлах, вне конфига).
- `onJtagActivity`/`onJtagError` — там, где реализована сама JTAG-транзакция (обычно файл вроде `dirtyJtag.c`/`jtag.c`, искать по вызовам PIO SM push/pull для TDI/TDO).

**Реализовано** в `pico-dirtyJtag/`:
- `led.h`/`led.c` — 5 независимых каналов (`led_tx`, `led_rx`, `led_error`, `led_jtag_activity`, `led_jtag_error`), таймерное мигание через `add_alarm_in_ms`: вызов `led_x(true)` зажигает светодиод и (пере)взводит таймер выключения через `LED_BLINK_MS` (30мс) — при сплошном трафике диод горит непрерывно, при редких событиях виден короткий, но гарантированно заметный blink. Ошибки держатся дольше — `LED_ERROR_HOLD_MS` (150мс), чтобы визуально отличаться от обычного трафика.
- `pio_jtag.c: jtag_transfer()/jtag_strobe()` → `led_jtag_activity(true)` на каждую реальную scan-операцию (CMD_XFER/CMD_CLK).
- `cmd.c: cmd_handle()` default-ветка → `led_jtag_error(true)` на нераспознанный/повреждённый байт команды от хоста.
- `cdc_uart.c: cdc_uart_task()` → проверка бит framing/parity/break/overrun в `UARTRSR` (PL011) → `led_error(true)`.
- `dirtyJtagConfig.h` — добавлены `PIN_LED_JTAG_ACT`/`PIN_LED_JTAG_ERROR` (по умолчанию `-1`/отключены для плат, где их нет).
- Попутно исправлен баг апстрима: `led_tx`/`led_rx` писали в один и тот же GPIO (`LedRxPin`), из-за чего на платах с разными пинами TX/RX (например `BOARD_SPOKE_RP2040`) RX-светодиод никогда не загорался.

</details>

---

## 5. Разводка на RPi4

**JTAG (5 сигналов):**
```
RP2040 GPIO21 (TDI)  → RPi4 GPIO26 (TDI)
RP2040 GPIO22 (TDO)  → RPi4 GPIO24 (TDO)
RP2040 GPIO24 (TCK)  → RPi4 GPIO25 (TCK)
RP2040 GPIO25 (TMS)  → RPi4 GPIO27 (TMS)
RP2040 GPIO27 (TRST) → RPi4 GPIO22 (TRST)
```

**UART (2 сигнала, крест-накрест):**
```
RP2040 GPIO1 (TX) → RPi4 GPIO15 (RXD)
RP2040 GPIO2 (RX) → RPi4 GPIO14 (TXD)
```

**Общий GND:**
```
RP2040 GND (3/23/8/33) → RPi4 GND
```

Итого 7 сигнальных проводов + GND.

RTCK не используется — PIO-реализация тактирует JTAG жёстко заданными таймингами, не требует обратной связи от цели (в отличие от FTDI/MPSSE, где RTCK был обязателен для стабильности после halt/resume).

`config.txt` на SD-карте RPi4 не меняется относительно уже настроенного для JTAG+UART (`enable_jtag_gpio=1`, `gpio=22-27=np`, `enable_uart=1`).

---

## 6. Прошивка RP2040 (UF2, drag-and-drop)

1. Зажать кнопку **BOOTSEL** на плате.
2. Не отпуская BOOTSEL, подключить плату к Mac по USB.
3. Отпустить BOOTSEL — плата смонтируется как USB-накопитель (обычно `RPI-RP2`).
4. Перетащить `dirtyJtag.uf2` (собранный в §1) на этот накопитель.
5. Плата сама перезагрузится и запустит прошивку, накопитель исчезнет.

Проверка:
```bash
lsusb
# или на macOS:
system_profiler SPUSBDataType
```
Должно появиться устройство **DirtyJTAG**, и рядом — отдельный serial-порт (`/dev/tty.usbmodemXXXX`) для UART.

---

## 7. Использование

**JTAG:**
```bash
sudo openocd -f interface/<dirtyjtag-cfg> -f target/bcm2711.cfg
```
(точное имя interface-конфига под DirtyJtag уточнить в документации репозитория)

Либо через `openFPGALoader`:
```bash
openFPGALoader --cable dirtyJtag --detect
```

**UART:**
```bash
minicom -D /dev/tty.usbmodemXXXX -b 115200
```
(без танцев с socat — порт уже локальный физический serial-девайс, socat нужен был только для проброса TCP от ESP32-C3, здесь не требуется)

---

## Статус

- [ ] Склонировать репозиторий, собрать `dirtyJtag.uf2` (проверка тулчейна)
- [x] `dirtyJtagConfig.h` упрощён до одной платы `BOARD_MYBOARD` (TDI/TDO/TCK/TMS/TRST, UART0), лишние блоки других плат из апстрима удалены — пины LED пока `-1` (отключены), подставить после разводки платы
- [ ] Развести JTAG+UART провода на RPi4 (7 сигналов + GND), назначить реальные GPIO для 5 LED и вписать их в `BOARD_MYBOARD`
- [ ] Прошить через UF2, проверить `lsusb`/DirtyJTAG + serial-порт
- [ ] Проверить JTAG без TRST сначала (4 провода), при нестабильности добавить TRST
- [x] Встроить LED-хуки в исходники (`led.c`/`led.h`, `pio_jtag.c`, `cmd.c`, `cdc_uart.c`) — таймерное мигание с минимальной видимой длительностью (`LED_BLINK_MS`), ошибки светятся дольше (`LED_ERROR_HOLD_MS`); проверить в железе после назначения GPIO
