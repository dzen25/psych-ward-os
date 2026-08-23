---
title: "RPi4 — JTAG через FT232H с выделенного отладочного хоста (carto)"
created: 2026-08-23
tags: [sel4, rpi4, jtag, ftdi, ft232h, openocd, gdb, debugging]
---

# JTAG через FT232H — отладочный хост `carto`

Разделение ролей (важно, не путать):
- **Сборка** — этот сервер (`psych-ward-os`, где собирается `build-rpi4`/`load_chain`).
- **Все операции с JTAG** (OpenOCD, физическое подключение к FT232H) — только на выделенной машине `carto` (`ssh nikita@carto`, доступ по ключу с парольной фразой, уже настроен). На carto нет и не должно быть дерева сборки — только отладочные инструменты.
- **GDB** можно (и рекомендуется) запускать здесь же, где лежат собранные ELF с символами, пробросив порт OpenOCD по SSH — см. §6. Копировать билды на carto не нужно.

Общая схема (Mac-версия, если понадобится) описана в `../jtag-uart-rpi4-guide.md` — распиновка RPi4 (§2) и `config.txt` (§3) оттуда **не меняются** и здесь не дублируются построчно, только даны в сжатом виде для справки. Этот файл — про то, что специфично именно для FT232H на Linux-хосте carto, проверено вживую 2026-08-23.

---

## 1. Что уже подтверждено на carto (2026-08-23)

Проверено по SSH с этого сервера, ничего руками на carto не ставилось — всё уже было готово из коробки:

| Компонент | Статус |
|---|---|
| FT232H физически подключен | `lsusb` → `0403:6014 Future Technology Devices International, Ltd FT232H Single HS USB-UART/FIFO IC` |
| OpenOCD | `0.12.0`, стоит (`/usr/bin/openocd`) |
| `gdb-multiarch` | стоит (`/usr/bin/gdb-multiarch`) |
| `target/bcm2711.cfg` | уже есть в `/usr/share/openocd/scripts/target/` — свой не писать |
| `board/rpi4b.cfg` | уже есть, грузит `bcm2711.cfg` + `transport select jtag` + `reset_config trst_only` — тоже готовый |
| udev-права на FTDI | `/lib/udev/rules.d/60-openocd.rules` — `0403:6014` → `MODE=660 GROUP=plugdev TAG+=uaccess`, пользователь `nikita` в группе `plugdev` |
| Нужен ли `sudo` для запуска OpenOCD | **нет** — из-под обычного пользователя достаточно (см. §5) |

---

## 2. Распиновка (справочно, полная версия в `../jtag-uart-rpi4-guide.md`)

### JTAG: FT232H → RPi4 (GPIO22-27, Alt4, фиксировано кремнием BCM2711)

| Пин FT232H | Сигнал | → GPIO RPi4 |
|---|---|---|
| D0 | TCK  | 25 |
| D1 | TDI  | 26 |
| D2 | TDO  | 24 |
| D3 | TMS  | 27 |
| D4 | TRST | 22 |
| D7 | RTCK | 23 |
| GND | GND | GND |

RTCK (D7) — обязательно, иначе JTAG нестабилен после halt/resume.

### `config.txt` на SD-карте RPi4 (не меняется от хоста)

```ini
arm_64bit=1
kernel=u-boot.bin
enable_jtag_gpio=1
gpio=22-27=np
enable_uart=1
```

---

## 3. ⚠️ Важная особенность: FT232H на carto один, не два устройства

В отличие от исходного Mac-плана (FT232H под JTAG + отдельный USB-UART адаптер), на carto сейчас воткнут **только один** FT232H. По умолчанию Linux цепляет его как обычный serial-порт (`ftdi_sio`) — он же виден как `/dev/ttyUSB0`.

Это значит: **JTAG и serial-консоль на carto сейчас взаимно исключающие режимы одного и того же чипа**, не просто "конфликт драйверов ОС":
- Одноканальный FT232H физически либо в async-UART режиме (D0-D7 = TX/RX + прочее), либо в MPSSE/JTAG-режиме (D0-D3 = TCK/TDI/TDO/TMS, D4-D7 = GPIOL) — одновременно оба режима на одних и тех же 8 линиях в принципе невозможны, это состояние всего чипа, а не отдельных пинов.
- Если нужна одновременная UART-консоль RPi4 + JTAG — нужен второй физический адаптер на carto (как и было задумано изначально для Mac), либо использовать `debug_of_esp32c3` (беспроводной UART-мост, независимый канал, не занимает FT232H).

### Эмпирически найденное: OpenOCD не требует ручного `unbind`, но `/dev/ttyUSB0` после него не возвращается сам

Ожидалось, что перед запуском OpenOCD нужно руками отвязать `ftdi_sio` (`echo -n "1-4:1.0" > /sys/bus/usb/drivers/ftdi_sio/unbind`, как `kextunload` на Mac) — **на деле не потребовалось**: OpenOCD/libftdi через libusb сам отцепляет `ftdi_sio` при захвате интерфейса, `Permission denied`/`device busy` не возникает.

НО: после того как OpenOCD поработал с устройством, `/dev/ttyUSB0` не переподключается автоматически — узел пропадает и не появляется обратно сам по себе. Восстановить (оба варианта требуют пароль sudo на carto, не автоматизировано):
```bash
sudo modprobe -r ftdi_sio && sudo modprobe ftdi_sio
# либо физически переподключить FT232H в USB
```
Проверить состояние: `ls /dev/ttyUSB0` (есть/нет), `lsusb` (устройство остаётся видно на USB-шине в любом случае — пропадает только tty-нода).

---

## 4. Интерфейс-конфиг OpenOCD

Файл `file/ft232h-jtag.cfg` (актуальный синтаксис OpenOCD 0.12, без deprecated-предупреждений — старый Mac-вариант `ftdi_vid_pid`/`ftdi_channel`/`ftdi_layout_init` тоже работает, но ругается):

```tcl
adapter driver ftdi
ftdi vid_pid 0x0403 0x6014
ftdi channel 0
ftdi layout_init 0x0008 0x001b
ftdi layout_signal nTRST -data 0x0010
adapter speed 1000
transport select jtag
```

`ftdi layout_init 0x0008 0x001b` — data=`0x0008` (TMS высокий на старте), direction=`0x001b` (биты 0,1,3,4 — TCK/TDI/TMS/TRST как выходы; TDO=бит2 и RTCK=бит7 остаются входами) — соответствует распиновке из §2. Перенесено из старого Mac-плана (`../jtag-uart-rpi4-guide.md`, §5) — это свойство самой схемы подключения FT232H↔RPi4, не зависит от хоста.

**`ftdi layout_signal nTRST -data 0x0010` — добавлено 2026-08-23, без неё JTAG не работал.** Старый Mac-план задавал TRST только через статичный `layout_init` (бит4 = `0`, то есть LOW сразу после старта) и никогда не объявлял его как именованный сигнал — OpenOCD в такой конфигурации физически не может САМ активно управлять линией TRST (снять/поставить сброс в нужные моменты своей init-последовательности), она просто навсегда остаётся на уровне из `layout_init`. LOW на TRST = чип перманентно в Test-Logic-Reset → любой скан читает "all ones" — ровно тот же класс бага, что уже был найден (по другой причине, но с тем же симптомом) в мосте RP2040, см. `../debug_of_RP2040/Rp2040-jtag-uart-guide.md`, §8, пункт 1. Синтаксис/битовая маска сверены с рабочими эталонными конфигами из поставки OpenOCD (`interface/ftdi/olimex-arm-usb-ocd-h.cfg`, `tumpa.cfg`) — оба явно объявляют `nTRST`/`nSRST` через `layout_signal`, ни один не полагается на голый `layout_init`.

`adapter speed 1000` здесь фактически не финальное значение — `board/rpi4b.cfg` → `target/bcm2711.cfg` после него делает `adapter speed 4000` и переопределяет его (см. вывод OpenOCD: `clock speed 4000 kHz`). Не проблема, просто не удивляться при чтении лога.

---

## 5. Запуск на carto

```bash
ssh nikita@carto
scp <этот файл, если ещё не там> nikita@carto:~/jtag/ft232h-jtag.cfg   # один раз, дальше не нужен
cd ~/jtag   # или любая директория с ft232h-jtag.cfg
openocd -f ft232h-jtag.cfg -f board/rpi4b.cfg
```
Без `sudo` (см. §1). `board/rpi4b.cfg` берётся из системных скриптов OpenOCD (`/usr/share/openocd/scripts/`) по относительному пути — copy на carto не нужен.

Признак успеха (полный тап найден):
```
Info : JTAG tap: bcm2711.cpu tap/device found: 0x4ba00477
Info : bcm2711.cpu0: hardware has 6 breakpoints, 4 watchpoints
...
Info : Listening on port 3333 for gdb connections
```

**Подтверждено на железе 2026-08-23** (после добавления `nTRST` в §4 и физической разводки проводов):
```
Info : clock speed 4000 kHz
Info : JTAG tap: bcm2711.cpu tap/device found: 0x4ba00477 (mfg: 0x23b (ARM Ltd), part: 0xba00, ver: 0x4)
Info : bcm2711.cpu0: hardware has 6 breakpoints, 4 watchpoints
Info : bcm2711.cpu1: hardware has 6 breakpoints, 4 watchpoints
Info : bcm2711.cpu2: hardware has 6 breakpoints, 4 watchpoints
Info : bcm2711.cpu3: hardware has 6 breakpoints, 4 watchpoints
Info : starting gdb server for bcm2711.cpu0 on 3333
Info : Listening on port 3333 for gdb connections
... (аналогично cpu1/2/3 на портах 3334/3335/3336)
```
Все 4 ядра найдены, IDCODE совпадает с ожидаемым (`0x4ba00477`, см. `target/bcm2711.cfg`) — тот же чип, что и в `debug_of_RP2040`-варианте моста, ожидаемо идентичный.

До фикса (проверено на том же железе ДО добавления `nTRST` и ДО разводки проводов) была ошибка `JTAG scan chain interrogation failed: all ones` → `IR capture error` — см. разбор причины в §4.

---

## 6. GDB — через SSH-туннель, интегрировано в VSCode (F5)

Раз сборка живёт на этом сервере, а JTAG — на carto, не нужно тащить ELF туда-сюда: пробросить порты OpenOCD сюда по SSH и запустить `gdb-multiarch` локально, с локальным путём к символам. **Подтверждено на железе 2026-08-23** — оба варианта ниже работали end-to-end, подключение практически мгновенное (не 20-25с, как у побитового моста RP2040 — MPSSE через FT232H аппаратный, на нескольких МГц).

### Через VSCode (F5) — рекомендуется

Настроено в `.vscode/tasks.json` + `.vscode/launch.json` (тот же паттерн, что у RP2040-варианта):
- Задача `JTAG: SSH-туннель (carto)` — поднимает проброс портов 3333-3336/4444/6666 в фоне. Если ключ ещё не разблокирован в этом сеансе VSCode — спросит парольную фразу прямо в панели терминала.
- Задача `JTAG: OpenOCD на carto (FT232H/bcm2711)` — зависит от туннеля, выполняет `openocd -f ft232h-jtag.cfg -f board/rpi4b.cfg` НА carto через `ssh nikita@carto '...'` (требует, чтобы `~/jtag/ft232h-jtag.cfg` там уже лежал — см. §5).
- Конфиг отладки `JTAG: RPi4 kernel.elf через FT232H/carto (cpu0)` — F5 запускает обе задачи через `preLaunchTask`, подключает `gdb-multiarch` к `localhost:3333` (уже локальный порт благодаря туннелю), догружает символы userland (`sel4test-driver`) через `add-symbol-file`.

Панель **Run and Debug** (`Ctrl+Shift+D`) → выбрать `JTAG: RPi4 kernel.elf через FT232H/carto (cpu0)` → **F5**.

### Вручную, по шагам (для отладки самого туннеля/конфига)

```bash
# на этом сервере, отдельный терминал — держать открытым на всё время сессии
ssh -N -L 3333:localhost:3333 -L 3334:localhost:3334 -L 3335:localhost:3335 -L 3336:localhost:3336 -L 4444:localhost:4444 -L 6666:localhost:6666 nikita@carto

# на этом сервере, второй терминал — запускает OpenOCD НА carto, вывод идёт сюда
ssh nikita@carto 'cd ~/jtag && openocd -f ft232h-jtag.cfg -f board/rpi4b.cfg'

# на этом сервере, третий терминал — локальный gdb-multiarch через проброшенный порт
gdb-multiarch -q \
  -ex "set remotetimeout 20" \
  -ex "target extended-remote localhost:3333" \
  build-rpi4/kernel/kernel.elf
```

Порт 4444 (telnet Tcl-консоль OpenOCD, тоже проброшен туннелем) полезен для `halt`/`resume`/`targets` напрямую, независимо от GDB.

### ⚠️ КРИТИЧНО, найдено на железе 2026-08-23: cpu0 автоматически halted сразу при старте OpenOCD — ДО подключения GDB

Голый `openocd -f ft232h-jtag.cfg -f board/rpi4b.cfg` (без явных `-c` команд) сам, в рамках своей стандартной `init`/examine-последовательности, останавливает **cpu0** (`bcm2711.cpu0 halted in AArch64 state due to debug-request`) — cpu1-3 при этом остаются `running`. Подтверждено дважды через `targets`-команду в Tcl-консоли (порт 4444) сразу после старта, ещё до какого-либо подключения GDB.

Это не баг конкретно этого конфига — обычное поведение OpenOCD/ARM ADIv5-таргетов при examine, никак не связанное с `nTRST`-фиксом из §4. Но следствие то же, что уже задокументировано в `../debug_of_RP2040/JTAG-GDB-cheatsheet.md` ("Continue перед Stop"), только риск наступает РАНЬШЕ — ещё до того, как вы вообще успели подключить GDB:

- **Если запустить задачу `JTAG: OpenOCD на carto` и НЕ довести дело до GDB (отменить F5, просто хотели проверить туннель и т.п.) — cpu0 останется halted, шелл/UART платы замрёт навсегда**, пока кто-то явно не резюмнёт.
- **Убить процесс OpenOCD (Ctrl+C в панели задач, `kill`, закрыть терминал) НЕ резюмит цель** — сам факт остановки OpenOCD не трогает состояние halt на кристалле, оно держится независимо (проверено: kill без предварительного `resume` оставляет cpu0 halted и на следующем старте OpenOCD это подтверждается через `targets`).

**Правило то же, что и у RP2040-варианта: всегда `resume`/Continue перед тем, как отключаться или останавливать сессию.** Быстрое ручное восстановление, если платформа зависла (шелл не отвечает):
```bash
ssh nikita@carto '(echo "resume"; sleep 0.3; echo "targets"; echo "exit") | nc localhost 4444'
```
(если OpenOCD на carto уже не запущен — сначала поднять его заново; сам факт нового старта снова halt-нёт cpu0 — это нормально, `resume` сразу после решает).

---

## 7. Известные питфолы

| Симптом | Причина / решение |
|---|---|
| `JTAG scan chain interrogation failed: all ones` | Две независимые возможные причины, обе встречались: (1) в конфиге не объявлен `ftdi layout_signal nTRST` — TRST зависает на LOW из `layout_init`, чип перманентно в Test-Logic-Reset (см. §4, исправлено 2026-08-23); (2) провода реально не подключены/обрыв/цель не запитана. Если `nTRST` уже объявлен как в `file/ft232h-jtag.cfg` — проверять физику (целостность 7 проводов, питание RPi4, что она прошла ранние стадии загрузки). |
| `/dev/ttyUSB0` пропал после сессии OpenOCD | Ожидаемо (см. §3) — `sudo modprobe -r ftdi_sio && sudo modprobe ftdi_sio` или физический реплаг. Устройство на USB-шине (`lsusb`) при этом никуда не девается. |
| Нужен и JTAG, и живая UART-консоль одновременно | Один FT232H не может оба режима разом (см. §3) — нужен второй физический адаптер, либо `debug_of_esp32c3` (беспроводной, независимый канал). |
| `IR capture error` / `all ones` при уже разведённых проводах | Плохой контакт/длинные провода — короче (10-15см), проверить GND, проверить что `gpio=22-27=np` реально попал на SD (без снятия pull-down TDI/TDO не пройдут). |
| Deprecated-warning от OpenOCD (`ftdi_vid_pid`...) | Безвредно, просто старый синтаксис — используйте `file/ft232h-jtag.cfg` (уже актуальный). |
| Плата должна уже пройти firmware boot до подключения JTAG | JTAG не подменяет serial на самых ранних стадиях (start4.elf/U-Boot) — коннектиться только когда ARM-ядра уже стартовали. |
| Шелл/UART платы замер после JTAG-сессии | cpu0 остался halted — см. §6, "КРИТИЧНО". Резюмить через `nc localhost 4444` → `resume` (по туннелю или прямо на carto). Не помогает — цикл `openocd -f ... -f board/rpi4b.cfg` заново + `resume` через telnet, физическая перезагрузка не нужна. |
| Просто запустил задачу OpenOCD в VSCode "посмотреть", не дошёл до GDB | Та же ловушка — cpu0 уже halted самим фактом старта OpenOCD (§6). Либо довести до GDB (F5 целиком), либо явно резюмнуть через telnet перед остановкой задачи. |

---

## Статус

- [x] SSH-доступ на carto проверен
- [x] FT232H виден (`lsusb`), OpenOCD 0.12.0 + gdb-multiarch стоят
- [x] `board/rpi4b.cfg`/`target/bcm2711.cfg` — готовые, не писать самим
- [x] udev-права подтверждены — `sudo` для OpenOCD не нужен
- [x] Свой интерфейс-конфиг (`file/ft232h-jtag.cfg`) — актуальный синтаксис, `nTRST` явно объявлен (без этого не работало, см. §4)
- [x] Найдена и задокументирована особенность "один FT232H = JTAG или UART, не одновременно" + пропадание `/dev/ttyUSB0`
- [x] Физически разведены 6 JTAG-проводов + GND FT232H↔RPi4 на carto (см. §2)
- [x] Первый успешный `tap found` / `0x4ba00477` — все 4 ядра bcm2711, GDB-порты 3333-3336 открыты (см. §5)
- [x] GDB через SSH-туннель — подключение подтверждено вживую (`idle_thread ()`, символы ядра), практически мгновенное, задержка ~20-25с (как у RP2040-моста) не воспроизвелась — MPSSE быстрый
- [x] Интеграция в VSCode (`.vscode/tasks.json`+`launch.json`, F5) — те же файлы, что у RP2040-варианта, задачи протестированы вручную той же командной цепочкой (туннель → openocd на carto → gdb)
- [x] Найдено и задокументировано: cpu0 автоматически halted сразу при старте OpenOCD, до GDB — см. §6 "КРИТИЧНО"; после каждого теста явно резюмили перед остановкой, живая плата оставлена в running-состоянии
- [ ] Определиться: нужен ли второй UART-адаптер на carto, или обходиться `debug_of_esp32c3`
- [ ] Реальная отладочная сессия (breakpoint в userland-коде + срабатывание) — пока проверялось только подключение/символы/detach, не полный цикл breakpoint→trigger→inspect
