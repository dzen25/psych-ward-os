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
JTAG пока муть мутная -с ним надо будет разбиратся отдельно

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

Где менять номера пинов: `pico-dirtyJtag/dirtyJtagConfig.h` — макросы `PIN_TDI/TDO/TCK/TMS/RST/TRST`, `PIN_UART0_TX/RX` и `PIN_LED_TX/RX/ERROR`, `PIN_LED_JTAG_ACT/ERROR`. Ниже — уже финальные значения по факту распайки платы (см. §4, §5).

```c
// BOARD_QMTECH_RP2040_DAUGHTERBOARD оставлен как константа - на её значение
// ссылаются pio_jtag.c/dirtyJtag.c, переиспользовать этот номер нельзя.
#define BOARD_QMTECH_RP2040_DAUGHTERBOARD 3
#define BOARD_MYBOARD 6

#define BOARD_TYPE BOARD_MYBOARD

#define PIN_TDI  16
#define PIN_TDO  17
#define PIN_TCK  14
#define PIN_TMS  15
#define PIN_RST  2    // system reset цели - для RPi4 не используется, JTAG-гребёнка его не имеет, физически не разведён
#define PIN_TRST 22

#define CDC_UART_INTF_COUNT 1
#define PIN_UART0       uart0
#define PIN_UART0_TX    0
#define PIN_UART0_RX    1

// LED - пины по факту разводки платы, см. §4.
#define LED_INVERTED   0
#define PIN_LED_TX     5    // зелёный (UART TX)
#define PIN_LED_RX     3    // жёлтый (UART RX)
#define PIN_LED_ERROR  6    // красный (пара с UART)
#define PIN_LED_JTAG_ACT    11   // синий
#define PIN_LED_JTAG_ERROR  13   // красный (пара с JTAG)
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
| UART RX | жёлтый | GPIO3 | UART | приём байта с UART |
| UART TX | зелёный | GPIO5 | UART | передача байта на UART |
| UART Error | красный | GPIO6 | UART | framing/parity/overrun/break ошибка |
| JTAG Activity | синий | GPIO11 | JTAG | активность TCK/scan-операция (CMD_XFER/CMD_CLK) |
| JTAG Error | красный | GPIO13 | JTAG | повреждённый/нераспознанный байт команды от хоста |

Группы физически стоят рядом на плате — UART-тройка (GPIO3, 5, 6) отдельно, JTAG-пара (GPIO11, 13) отдельно, для интуитивного визуального чтения состояния стенда. Значения уже соответствуют фактической распайке платы.

**Подключение — какой пин куда:**
```
RP2040 GPIO3  → R 330Ω → анод LED UART RX (жёлтый)      → катод → GND
RP2040 GPIO5  → R 330Ω → анод LED UART TX (зелёный)     → катод → GND
RP2040 GPIO6  → R 330Ω → анод LED UART Error (красный)  → катод → GND
RP2040 GPIO11 → R 330Ω → анод LED JTAG Activity (синий) → катод → GND
RP2040 GPIO13 → R 330Ω → анод LED JTAG Error (красный)  → катод → GND
```
Все 5 катодов — на общий GND (см. §3: любая из точек 3/23/8/33, можно свести все на одну).

`LED_INVERTED = 0` → GPIO активен высоким уровнем (диод горит при `1`, гасится при `0`), поэтому анод каждого LED смотрит на свой резистор/GPIO, катод — на GND. Резистор 330Ω — общий номинал для всех пяти (RP2040 держит по умолчанию ~4мА на пин): для красного/жёлтого/зелёного (Vf≈2.0-2.2В) даёт ток ~3.5-4мА, для синего (Vf≈3.0В) — ~0.9мА (светит тусклее остальных, но заметно). Если нужна одинаковая яркость — уменьшить резистор синего LED (например до 150Ω).

Пины уже вписаны в `PIN_LED_*` блока `BOARD_MYBOARD` в `dirtyJtagConfig.h` (см. §2).

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
RP2040 GPIO16 (TDI)  → RPi4 GPIO26 (TDI)
RP2040 GPIO17 (TDO)  → RPi4 GPIO24 (TDO)
RP2040 GPIO14 (TCK)  → RPi4 GPIO25 (TCK)
RP2040 GPIO15 (TMS)  → RPi4 GPIO27 (TMS)
RP2040 GPIO22 (TRST) → RPi4 GPIO22 (TRST)
```

**UART (2 сигнала, крест-накрест):**
```
RP2040 GPIO0 (TX) → RPi4 GPIO15 (RXD)
RP2040 GPIO1 (RX) → RPi4 GPIO14 (TXD)
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

**JTAG (простая проверка, без отладки):**
```bash
openFPGALoader --cable dirtyJtag --detect
```
Готового драйвера `dirtyjtag` в OpenOCD нет — ни в апстриме, ни в Ubuntu-пакете 0.12.0 (`openocd -c "adapter driver list"` его не показывает). Единственная существующая реализация — древний форк `jeanthom/openocd-dirtyjtag`, отброшенный из-за глубоких багов (падал на `syncbb_scan`/таймаутах libusb). Поэтому полноценная отладка (halt/reg/GDB) идёт через собственный TCP↔USB мост — см. §8.

**UART:**
```bash
minicom -D /dev/tty.usbmodemXXXX -b 115200
```
(без танцев с socat — порт уже локальный физический serial-девайс, socat нужен был только для проброса TCP от ESP32-C3, здесь не требуется)

---

## 8. Полный цикл отладки: OpenOCD + GDB через собственный мост

Три процесса, три роли:

1. **Мост** `uart-wifi_JTAG/files/dirtyjtag_bitbang_bridge.py` — держит USB-соединение с платой, транслирует протокол `remote_bitbang` (стандартный "тупой" bit-bang драйвер OpenOCD, обычно для симуляторов) в USB-команды DirtyJTAG (`CMD_CLK`/`CMD_GETSIG`/`CMD_SETSIG`).
2. **OpenOCD** (стоковый, `apt install openocd`) — подключается к мосту как к `remote_bitbang`-цели, поднимает TAP/DAP/4 ядра `bcm2711`, открывает GDB-порты (3333=cpu0, 3334=cpu1, 3335=cpu2, 3336=cpu3).
3. **GDB** (`apt install gdb-multiarch` — обычный `gdb` собран только под x86_64, aarch64-цель не потянет) — подключается к одному из GDB-портов, грузит символы.

### Вручную, по шагам

```bash
# 1. Мост (терминал 1)
cd uart-wifi_JTAG/files
python3 dirtyjtag_bitbang_bridge.py --port 44444

# 2. OpenOCD (терминал 2) — ждать "Listening on port 3333 for gdb connections"
openocd -f dirtyjtag_remote_bitbang.cfg -f /usr/share/openocd/scripts/board/rpi4b.cfg \
  -c "init" -c "poll off"

# 3. GDB (терминал 3)
gdb-multiarch -q \
  -ex "set remotetimeout 120" \
  -ex "target extended-remote localhost:3333" \
  build-rpi4/kernel/kernel.elf
```

Порт моста — `44444`, а не дефолтный `3335`: `3335` занят GDB-портом cpu2 из `bcm2711.cfg`, при совпадении OpenOCD падает на `couldn't bind gdb to socket`.

`-c "poll off"` обязателен: по умолчанию OpenOCD фоново и непрерывно опрашивает состояние всех 4 целей. На медленном USB-bitbang-мосте (каждый JTAG-бит — отдельный Python↔USB round-trip) этот фоновый опрос конкурирует за мост с реальными запросами GDB и роняет GDB-сессию по таймауту (`Warn : keep_alive() was not invoked in the 1000 ms timelimit`).

`set remotetimeout 120` тоже обязателен: первое подключение GDB читает вообще все регистры цели, включая 32 SIMD-регистра `V0`-`V31` по 128 бит — через тот же медленный мост это занимает ~20-25 секунд. Дефолтный таймаут GDB (~2с) столько не ждёт и рвёт соединение.

Успешный результат выглядит так:
```
idle_thread () at kernel/src/arch/arm/64/idle.S:13
13	    b 1b
$1 = 0xffffff800001117c
#0  idle_thread () at kernel/src/arch/arm/64/idle.S:13
```

### Через VSCode (F5)

Настроено в `.vscode/tasks.json` + `.vscode/launch.json`:
- Задача `JTAG: bridge (DirtyJTAG↔remote_bitbang)` — поднимает мост в фоне.
- Задача `JTAG: OpenOCD (RPi4/bcm2711)` — зависит от моста, ждёт `Listening on port 3333 for gdb connections`.
- Конфиг отладки `JTAG: RPi4 kernel.elf через OpenOCD/DirtyJTAG (cpu0)` — запускает обе задачи через `preLaunchTask` и подключает `gdb-multiarch` к порту 3333; `remotetimeout 120` уже прописан в `setupCommands`.

Просто выбрать эту конфигурацию и нажать F5 — точки останова, регистры, бэктрейс, память работают как в обычной GDB-сессии (с поправкой на задержку ~20-25с на подключение).

Символы берутся из `build-rpi4/kernel/kernel.elf` (само ядро seL4, собрано с `-g`, не strip-нуто) — этого достаточно для отладки кода ядра; для userland (`sel4test-driver`) символы нужно грузить отдельно (`add-symbol-file`), в текущий цикл это не входит.

### Найденные и исправленные баги моста

При первом реальном запуске через OpenOCD (в отличие от автономного self-test-скрипта) сканирование цепи стабильно проваливалось (`JTAG scan chain interrogation failed: all ones`, затем `IR capture error`). Причина — два независимых бага в `dirtyjtag_bitbang_bridge.py`, оба исправлены:

1. **Инверсия полярности TRST.** `remote_bitbang` шлёт *логические* флаги ассерта (`1` = сброс активен), а прошивка (`cmd_setsig`/`jtag_set_trst` в `pio_jtag.c`) трактует бит `SIG_TRST` как сырой уровень GPIO (`1` = HIGH). Мост раньше пробрасывал бит напрямую без инверсии — единственная команда сброса, которую OpenOCD шлёт при подключении ("убедиться, что сброс отпущен"), из-за этого реально держала `TRST` в LOW (assert) на всю сессию → TAP стоял в вечном Test-Logic-Reset → все чтения TDO стабильно давали `1`.
2. **Задержка чтения на один такт.** `CMD_GETSIG` в прошивке возвращает не живое состояние пина, а закешированное `last_tdo`, которое обновляется ПРЕДЫДУЩИМ вызовом `CMD_CLK` (PIO-шейп в `pio_jtag_write_tms_blocking`). Протокол `remote_bitbang` читает TDO ДО отправки следующего такта — из-за кеша это давало значение, отставшее на один бит от того, что ожидал увидеть OpenOCD. Автономный self-test это не ловил, потому что его цикл сначала тактирует, потом читает — случайно компенсирует задержку. Исправлено: мост теперь сам тактирует «отложенный» (staged) бит в момент прихода запроса на чтение, воспроизводя порядок self-test, а соответствующий поздний такт от OpenOCD пропускает как уже выполненный.

После обоих фиксов: `IDCODE 0x4ba00477` совпадает точно, OpenOCD видит все 4 ядра `bcm2711`, `halt`/`reg`/`resume` и GDB с символами (`idle_thread` в `kernel/src/arch/arm/64/idle.S`) — всё проверено на реальном железе.

### Почему ~20-25с на подключение — это не «недоделанный батчинг», а предел протокола

Изначально в статусе был пункт «ускорить мост, батчинг битовых чтений». После разбора протокола `remote_bitbang` выяснилось, что батчить чтения на уровне моста нельзя в принципе: OpenOCD — синхронный, он ждёт ответ на каждый посланный бит (`R`) прежде чем решить, какой байт слать следующим (следующий бит буквально ещё не существует в TCP-сокете в момент, когда мост мог бы его "заглянуть вперёд"). Это не особенность нашей реализации, а конструкция самого протокола `remote_bitbang` (он проектировался для симуляторов, где round-trip ничего не стоит).

Основная стоимость — не в мосте, а в том, что при каждой остановке GDB перечитывает вообще все регистры цели через ARM ADIv5 DAP, включая 32 SIMD/FP-регистра `V0`-`V31` по 128 бит (это одно само по себе — тысячи JTAG-бит, каждый бит — отдельный round-trip Python↔USB). Это ARM-протокольная стоимость, которая была бы примерно такой же и с гипотетическим нативным драйвером `dirtyjtag` в OpenOCD — просто JTAG поверх bit-bang на USB так работает.

Единственный путь к реальному ускорению — сменить сам протокол моста с `remote_bitbang` (посимвольный) на что-то блочное (например `jtag_vpi` — бинарный протокол, пачками), но такой драйвер не собран в Ubuntu-пакете OpenOCD, и его пришлось бы собирать из исходников самостоятельно — это отдельный, довольно большой проект (сравнимый по объёму с уже отброшенной попыткой собрать `openocd-dirtyjtag`), не гарантированно более надёжный. Пока не делали — текущая скорость принята как рабочий компромисс.

---

## Статус

- [x] Склонировать репозиторий, собрать `dirtyJtag.uf2` (проверка тулчейна)
- [x] `dirtyJtagConfig.h` упрощён до одной платы `BOARD_MYBOARD`, лишние блоки других плат из апстрима удалены
- [x] Развести JTAG+UART провода на RPi4 (7 сигналов + GND) — сделано, пины по факту распайки платы вписаны в `dirtyJtagConfig.h` (см. §2, §5). `PIN_RST` (GPIO2) физически не подключён — RPi4 не даёт SRST на JTAG-гребёнке
- [x] Назначить реальные GPIO для 5 LED и вписать их в `BOARD_MYBOARD` — сделано (см. §4)
- [x] Прошить через UF2, проверить `lsusb`/DirtyJTAG + serial-порт
- [x] Проверить JTAG вживую — IDCODE `0x4ba00477` совпадает через `openFPGALoader --detect`
- [x] Встроить LED-хуки в исходники (`led.c`/`led.h`, `pio_jtag.c`, `cmd.c`, `cdc_uart.c`) — таймерное мигание с минимальной видимой длительностью (`LED_BLINK_MS`), ошибки светятся дольше (`LED_ERROR_HOLD_MS`)
- [x] Проверить LED вживую после прошивки — все 5 должны мигнуть дважды разом при старте (`led_boot_ready()`), затем реагировать на трафик/ошибки по отдельности
- [x] Полноценная отладка через OpenOCD + собственный мост (`remote_bitbang`) — TAP найден, все 4 ядра `bcm2711` определены, `halt`/`reg`/`resume` работают (см. §8)
- [x] GDB (`gdb-multiarch`) через GDB-порт OpenOCD — подключение, символы, бэктрейс проверены на реальном ядре (`idle_thread`)
- [x] Отладка из VSCode по F5 — задачи и `launch.json` настроены (мост → OpenOCD → gdb-multiarch, `remotetimeout 120`)
- [x] Символы userland (`sel4test-driver`) в той же GDB-сессии (`add-symbol-file` в `launch.json`) — EXEC, не PIE, offset не нужен
- [x] Разобраться с медленным подключением GDB (~20-25с) — не баг моста, а стоимость ARM ADIv5 register dump (32×128бит SIMD-регистры) поверх посимвольного `remote_bitbang`; реальное ускорение потребовало бы блочного протокола (`jtag_vpi`) вместо `remote_bitbang` — отдельный проект, пока не делали (см. разбор выше)
- [ ] `poll off` в OpenOCD (нужен, чтобы GDB вообще стабильно подключался) похоже мешает Pause/interrupt из VSCode — рабочий обход: `nc localhost 4444` → `halt`/`resume` напрямую через Tcl-консоль (см. `JTAG-GDB-cheatsheet.md`)
- [ ] **Важно**: Stop в VSCode не резюмит цель перед отключением GDB — если остановить сессию, пока ядро на паузе, оно останется halted навсегда (UART/шелл замирают). Всегда Continue перед Stop; если забыл — `nc localhost 4444` → `resume`
