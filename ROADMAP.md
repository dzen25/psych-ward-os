# Psych Ward OS — RPi4 Port Roadmap

Детальная дорожная карта порта на Raspberry Pi 4 (BCM2711). Общий обзор, обоснование выбора платформы и практические инструкции — в [README.md](README.md); здесь — по шагам, с чекбоксами, обновляется по ходу работы.

---

## Фаза 0 — Подготовка ✅ (сделано)

- [x] Все платформо-зависимые адреса (UART/RTC/virtio-mmio, регистры PL011/PL031, GIC IRQ) вынесены в `src/h/platform.h`
- [x] Рефактор не сломал поведение на QEMU (boot/shell/net/disk/NTP работают)
- [x] Зафиксирован известный quirk: `timer_driver.cpp` использует offset `0x10` под именем «ICR» для PL031, хотя по датащиту `0x10` — это IMSC, а реальный ICR — `0x1C`. На QEMU работает, трогать не стали — при переходе на ARM generic timer этот код всё равно уходит целиком (см. Фазу 3.1)

---

## Фаза 1 — Hello World на живом железе

Цель — провалидировать тулчейн и цепочку загрузки **отдельно** от кода `psych-ward-os`, прежде чем его трогать.

### 1.1 Цепочка загрузки RPi4 — как это работает

Все файлы лежат плоско в корне FAT32-раздела SD-карты. Порядок исполнения:

| # | Стадия | Файл | Источник |
|---|---|---|---|
| 1 | FSBL | — (в ROM платы) | не трогаем |
| 2 | GPU firmware | `start4.elf`, `fixup4.dat` | [raspberrypi/firmware/boot](https://github.com/raspberrypi/firmware/tree/master/boot) |
| 3 | Device tree | `bcm2711-rpi-4-b.dtb` (+ `overlays/*`) | тот же репозиторий |
| 4 | Конфиг | `config.txt` | пишем сами: `arm_64bit=1` + `kernel=u-boot.bin` |
| 5 | Bootloader | `u-boot.bin` | собираем сами (см. 1.2) |
| 6 | seL4-образ | `sel4test-driver-image-arm-bcm2711` | результат сборки sel4test (см. 1.3) |

<details>
<summary><b>Подробно, что происходит на каждом шаге и зачем нужен каждый файл: (клик, чтобы развернуть)</b></summary>

1. **BootROM (шаг 1, в таблице выше)** — код, прошитый в кристалл SoC на заводе, не лежит на SD-карте и не изменяется. На RPi4 он читает конфигурацию загрузки из EEPROM платы (не из FAT-раздела) и его единственная задача — найти на SD-карте (или другом носителе) и запустить `start4.elf`. Дальше вся логика на GPU (VideoCore VI), не на ARM-ядрах — ARM-ядра всё ещё в состоянии reset.
2. **`start4.elf` + `fixup4.dat` (GPU firmware)** — полноценная закрытая прошивка VideoCore VI. Она поднимает и калибрует SDRAM (на этом этапе ARM-ядра ещё ничего не могут — контроллер памяти инициализирует именно GPU), затем читает `config.txt` и device tree с той же SD-карты. `start4.elf` — вариант прошивки конкретно под BCM2711/Pi4 (в отличие от `start.elf` для более старых моделей), `fixup4.dat` — согласованная с ней таблица поправок разметки памяти между GPU и ARM (сколько RAM отдать видеоядру, где начинается память ARM и т.д.) — версии `start*.elf`/`fixup*.dat` всегда должны браться из одного и того же релиза прошивки, не смешивать.
3. **`config.txt`** — простой текстовый конфиг, который парсит GPU firmware (ARM-ядра его никогда не видят). У нас всего два значимых параметра: `arm_64bit=1` — просит firmware поднять ARM-ядра сразу в AArch64 (иначе по умолчанию 32-бит), и `kernel=u-boot.bin` — говорит, какой файл считать «ядром» и передать на выполнение ARM-ядрам после инициализации.
4. **`bcm2711-rpi-4-b.dtb` (+ `overlays/*`)** — device tree: машиночитаемое описание железа платы (адреса периферии, IRQ, что физически распаяно). GPU firmware загружает его в память и передаёт указатель на него ARM-ядрам (через регистр x0) вместе с управлением. `overlays/*` — опциональные фрагменты dtb для конкретной подключенной периферии (HAT-платы и т.п.), которые firmware может домешать в основной dtb перед стартом — для базового boot не нужны.
5. **GPU firmware выпускает ARM-ядра из reset** и передаёт им управление по адресу, куда сама же загрузила файл из `kernel=` (для 64-бит режима это, как правило, физический адрес `0x80000`) — вместе с указателем на dtb в x0, как того ожидает загрузчик Linux-стиля (U-Boot тоже собран с расчётом на этот протокол).
6. **`u-boot.bin` (Bootloader)** — то, что реально запускается первым на ARM-ядрах. seL4-образ нельзя указать напрямую в `kernel=`, потому что GPU firmware понимает только загрузчики линуксового вида (zImage/Image), а не сырой seL4-бинарник без такого заголовка — поэтому нужен промежуточный шаг. U-Boot даёт интерактивную консоль и команды для чтения файлов с FAT-раздела SD-карты (`fatls`, `fatload`) и передачи управления по произвольному адресу памяти (`go`) — то есть именно U-Boot, а не GPU firmware, кладёт seL4-образ по адресу `0x10000000` и прыгает туда.
7. **`sel4test-driver-image-arm-bcm2711` (или, после Фазы 3, готовый образ `psych-ward-os`)** — собственно seL4-ядро вместе с корневой задачей (root task), слитые в один плоский бинарник. U-Boot загружает его командой `fatload mmc 0 0x10000000 <образ>` и передаёт управление командой `go 0x10000000` — с этого момента исполняется уже seL4, а не firmware/U-Boot.
</details>

Получить GPU firmware и device tree (напрямую из [raspberrypi/firmware/boot](https://github.com/raspberrypi/firmware/tree/master/boot)):

```bash
wget https://github.com/raspberrypi/firmware/raw/master/boot/start4.elf
wget https://github.com/raspberrypi/firmware/raw/master/boot/fixup4.dat
wget https://github.com/raspberrypi/firmware/raw/master/boot/bcm2711-rpi-4-b.dtb
```

И создать `config.txt`:

```bash
cat > config.txt << 'EOF'
arm_64bit=1
kernel=u-boot.bin
EOF
```

**Статус сбора файлов:** все файлы цепочки уже собраны и лежат в [`load_chain_test/`](load_chain_test):

- [x] `start4.elf`
- [x] `fixup4.dat`
- [x] `bcm2711-rpi-4-b.dtb`
- [ ] `overlays/*` — не скопированы; нужны только при необходимости конкретных оверлеев, для базового boot не обязательны
- [x] `config.txt` — самописный, содержимое: `arm_64bit=1` / `kernel=u-boot.bin`
- [x] `u-boot.bin`
- [x] `sel4test-driver-image-arm-bcm2711` — **не финальный**, это тестовая сборка hello-world из отдельного дерева `sel4-rpi4-hello`

Открытый вопрос: уточнить, требуется ли что-то в `config.txt` кроме `arm_64bit=1`/`kernel=`.

- [x] Сбор файлов закончен


<details>
<summary><b>1.2 Собрать U-Boot (клик, чтобы развернуть)</b></summary>

```bash
git clone https://github.com/u-boot/u-boot.git u-boot
cd u-boot
make CROSS_COMPILE=aarch64-linux-gnu- rpi_4_defconfig
make CROSS_COMPILE=aarch64-linux-gnu-
# результат: u-boot.bin
```
</details>

<details>
<summary><b>1.3 Собрать sel4test под rpi4 (клик, чтобы развернуть)</b></summary>

```bash
#в ОТДЕЛЬНОЙ папке, не в psych-ward-os!
mkdir -p ~/sel4-rpi4-hello && cd ~/sel4-rpi4-hello
repo init -u https://github.com/seL4/sel4test-manifest.git
repo sync
mkdir cbuild && cd cbuild
../init-build.sh -DPLATFORM=rpi4 -DAARCH64=1 -DRPI4_MEMORY=<реальный RAM платы: 1024|2048|4096|8192>
ninja
# результат: images/sel4test-driver-image-arm-bcm2711
```

⚠️ **Питфол:** если `-DRPI4_MEMORY` не указать — по умолчанию считает 8GB, и на плате с меньшим объёмом памяти загрузка может падать/вести себя странно. Всегда указывать реальный объём.
</details>



<details>
<summary><b>1.4 Собрать SD-карту (клик, чтобы развернуть)</b></summary>

В корень FAT32-раздела SD-карты скопировать все файлы из [`load_chain_test/`](load_chain_test): `start4.elf`, `fixup4.dat`, `bcm2711-rpi-4-b.dtb`, `config.txt`, `u-boot.bin`, `sel4test-driver-image-arm-bcm2711`.

- [ ] Записать SD-карту

</details>

<details>
<summary><b>1.5 Первый запуск (вручную, через serial-консоль) (клик, чтобы развернуть)</b></summary>

Подключение: serial TX/RX — GPIO 14/15, через USB-UART переходник на 3.3V.

Прервать автобут U-Boot любой клавишей, затем:

```
fatls mmc 0
fatload mmc 0 0x10000000 sel4test-driver-image-arm-bcm2711
go 0x10000000
```

⚠️ **Питфол** (встречался у других на форуме seL4): ошибка `Reading file would overwrite reserved memory` при `fatload`/`go` на `0x10000000` — обычно означает рассинхрон `RPI4_MEMORY` с реальным объёмом RAM платы, либо слишком большой образ. Первым делом проверять `RPI4_MEMORY`.

- [ ] Получить вывод в UART при ручной загрузке — **это критерий успеха Фазы 1**

</details>

<details>
<summary><b>1.6 Автоматизировать загрузку — `boot.scr` (клик, чтобы развернуть)</b></summary>

Когда ручной запуск подтверждён, автоматизировать через скрипт U-Boot (не через `setenv bootcmd; saveenv` — тот подход прячет конфиг в бинарный `uboot.env` на карте; версионируемый файл-скрипт лучше).

`boot.cmd`:
```
fatload mmc 0 0x10000000 sel4test-driver-image-arm-bcm2711
go 0x10000000
```

Компиляция:
```bash
mkimage -A arm64 -O linux -T script -C none -d boot.cmd boot.scr
```

`boot.scr` кладётся в корень SD-карты — U-Boot подхватывает его автоматически при старте.

- [ ] Написать `boot.cmd`, закоммитить в репозиторий (например `rpi4/boot.cmd`)
- [ ] Скомпилировать в `boot.scr`, проверить автозагрузку без ручного вмешательства
- [ ] (опционально) добавить в скрипт диагностику через `if`/`echo` на случай отсутствия файла образа

</details>

---
## Фаза 2 
<details>
<summary><b> Извлечение адресов под свои драйверы (клик, чтобы развернуть)</b></summary>

seL4-кернел на rpi4 уже знает адреса UART/GIC/generic timer — перевытаскивать их из dtb не нужно. DTB нужен только под железо, которого в seL4 нет «из коробки» — то, что понадобится собственным user-space драйверам:

- **genet** (Ethernet MAC) — замена virtio-net в `net_driver.cpp`
- **EMMC2 / SD-контроллер** — замена virtio-blk в `blk_driver.cpp`

```bash
dtc -I dtb -O dts bcm2711-rpi-4-b.dtb -o bcm2711-rpi-4-b.dts
```

(файл `bcm2711-rpi-4-b.dtb` уже есть в `load_chain_test/`, повторно скачивать не нужно)

- [ ] Декомпилировать `bcm2711-rpi-4-b.dtb`
- [ ] Определить и указать ниже, какие узлы будут нужны в целом (не только genet/EMMC2 — см. также раздел «Что нужно портировать» в README)
- [ ] Найти узел `genet` (адрес, IRQ)
- [ ] Найти узел EMMC2 (адрес, IRQ)
- [ ] Занести найденные адреса в отдельную rpi4-секцию `platform.h`
</details>

---

## Фаза 3

<details>
<summary><b>Порт дерева `psych-ward-os` (клик, чтобы развернуть)</b></summary>

Начинается только после успешной Фазы 1 (подтверждённый boot на живом железе, не в QEMU).

- [x] Создана ветка `RPi4`, зафиксирован переход от стабильной QEMU-версии к порту
- [ ] Пересобрать `psych-ward-os` с `-DPLATFORM=rpi4 -DRPI4_MEMORY=...` (cmake-настройки это позволяют без правок `easy-settings.cmake`/`init-build.sh` — платформа нигде не захардкожена)
- [ ] Проверить, что элементарно собирается (ожидаемо — либо ошибки компиляции из-за qemu-специфичного кода в `main.cpp`, либо соберётся, но не заработает на железе из-за отсутствия virtio)

### 3.1 Блокер: RTC (PL031 нет на реальном железе)

`timer_driver.cpp` сейчас использует PL031 (см. quirk из Фазы 0). На стоковой RPi4 battery-backed RTC нет.

- [ ] Заменить чтение аптайма на ARM generic timer (`CNTPCT_EL0`) — не зависит от драйвера, доступен всегда
- [ ] Wall-clock время: либо внешний I2C RTC-модуль, либо (проще, уже готово) — NTP как единственный источник. При старте без сети/NTP wall-clock будет не определено — решить, как деградировать

### 3.2 Блокер: virtio-net → genet

`net_driver.cpp` — TX/RX ring код под virtio-net полностью заменяется на драйвер BCM2711 genet. Протокольная логика поверх (ARP/UDP/NTP/DNS парсинг) переиспользуется как есть — трогать не нужно.

- [ ] Написать минимальный genet-драйвер (init, TX, RX) по адресам из Фазы 2
- [ ] Подключить к существующему протокольному стеку `net_driver.cpp`

### 3.3 Блокер: virtio-blk → EMMC/SD

`blk_driver.cpp` — virtio-blk код заменяется на реальный SD/EMMC-драйвер. FAT32-логика поверх переиспользуется как есть.

- [ ] Написать минимальный EMMC-драйвер (init, чтение/запись блоков) по адресам из Фазы 2
- [ ] Подключить к существующей FAT32-логике `blk_driver.cpp`

### 3.4 UART / GIC — низкий риск

RPi4 имеет GICv2 и PL011-совместимый UART0 — ожидаются только изменения адресов в `platform.h`, без переписывания логики.

- [ ] Обновить адреса UART/GIC в `platform.h` под rpi4 (секция из Фазы 2)
- [ ] Проверить boot readiness протокол (`SYS_DRIVER_READY`/`SYS_WAIT_ALL_DRIVERS_READY`) — не завязан на конкретную платформу, должен работать как есть, но перепроверить порядок сигналов при живых genet/EMMC-драйверах

### 3.5 Опционально / на будущее: остальная периферия BCM2711

Не задействовано в текущей архитектуре Psych Ward OS (headless, только UART), но каталогизировано на будущее — см. подробности в README:

- [ ] USB (host-контроллер)
- [ ] Wi-Fi / Bluetooth (BCM43455)
- [ ] GPIO (общее управление пинами)
- [ ] HDMI (видеовывод)
- [ ] Аудио (PWM/I2S)
- [ ] PCIe
</details>

---

## Полный roadmap

<details>
<summary><b>Cводный чек-лист (клик, чтобы развернуть)</b></summary>

- [x] **Фаза 0 — Подготовка**
  - [x] Вынести все платформо-зависимые адреса (UART/RTC/virtio-mmio, регистры PL011/PL031, GIC IRQ) в `src/h/platform.h`
  - [x] Убедиться, что рефактор не сломал поведение на QEMU (boot/shell/net/disk/NTP работают)
  - [x] Зафиксировать известный quirk: `timer_driver.cpp` использует offset `0x10` под именем «ICR» для PL031 (по датащиту `0x10` — это IMSC, реальный ICR — `0x1C`); не трогать, код всё равно уйдёт целиком в Фазе 3.1

- [ ] **Фаза 1 — Hello World на живом железе**
  - [ ] 1.1 Собрать файлы цепочки загрузки
    - [x] `start4.elf` (`wget` из raspberrypi/firmware)
    - [x] `fixup4.dat` (`wget` из raspberrypi/firmware)
    - [x] `bcm2711-rpi-4-b.dtb` (`wget` из raspberrypi/firmware)
    - [ ] `overlays/*` — по необходимости, для базового boot не обязательны
    - [x] `config.txt` — самописный (`arm_64bit=1` / `kernel=u-boot.bin`)
    - [x] `u-boot.bin` — собран (см. 1.2)
    - [x] `sel4test-driver-image-arm-bcm2711` — собран (см. 1.3), тестовый, не финальный
    - [ ] Уточнить, требуется ли что-то в `config.txt` кроме `arm_64bit=1`/`kernel=`
  - [x] 1.2 Собрать U-Boot (`rpi_4_defconfig`, `aarch64-linux-gnu-`)
  - [x] 1.3 Собрать sel4test hello-world под `-DPLATFORM=rpi4` (в отдельном дереве `sel4-rpi4-hello`, не в `psych-ward-os`)
  - [ ] 1.4 Собрать SD-карту — скопировать все файлы из `load_chain_test/` в корень FAT32-раздела
  - [ ] 1.5 Первый запуск вручную через serial-консоль (GPIO 14/15, USB-UART, 3.3V) — **критерий успеха Фазы 1**
  - [ ] 1.6 Автоматизировать загрузку через `boot.scr`
    - [ ] Написать `boot.cmd`, закоммитить (например `rpi4/boot.cmd`)
    - [ ] Скомпилировать `mkimage` → `boot.scr`, проверить автозагрузку без ручного вмешательства
    - [ ] (опционально) диагностика в скрипте на случай отсутствия файла образа

- [ ] **Фаза 2 — Извлечение адресов под свои драйверы**
  - [ ] Декомпилировать `bcm2711-rpi-4-b.dtb` → `.dts` (`dtc`)
  - [ ] Определить, какие узлы будут необходимы в целом
  - [ ] Найти узел `genet` (адрес, IRQ)
  - [ ] Найти узел EMMC2 (адрес, IRQ)
  - [ ] Занести найденные адреса в отдельную rpi4-секцию `platform.h`

- [ ] **Фаза 3 — Порт дерева `psych-ward-os`**
  - [x] Создать ветку `RPi4`, зафиксировать переход от стабильной QEMU-версии к порту
  - [ ] Пересобрать `psych-ward-os` с `-DPLATFORM=rpi4 -DRPI4_MEMORY=...`
  - [ ] Проверить, что элементарно собирается
  - [ ] 3.1 Блокер RTC (PL031 нет на реальном железе)
    - [ ] Заменить чтение аптайма на ARM generic timer (`CNTPCT_EL0`)
    - [ ] Wall-clock: внешний I2C RTC-модуль либо NTP как единственный источник; решить деградацию при старте без сети
  - [ ] 3.2 Блокер virtio-net → genet
    - [ ] Написать минимальный genet-драйвер (init, TX, RX) по адресам из Фазы 2
    - [ ] Подключить к существующему протокольному стеку `net_driver.cpp` (ARP/UDP/NTP/DNS переиспользуются как есть)
  - [ ] 3.3 Блокер virtio-blk → EMMC/SD
    - [ ] Написать минимальный EMMC-драйвер (init, чтение/запись блоков) по адресам из Фазы 2
    - [ ] Подключить к существующей FAT32-логике `blk_driver.cpp`
  - [ ] 3.4 UART / GIC — низкий риск
    - [ ] Обновить адреса UART/GIC в `platform.h` под rpi4
    - [ ] Проверить boot readiness протокол (`SYS_DRIVER_READY`/`SYS_WAIT_ALL_DRIVERS_READY`) с живыми genet/EMMC-драйверами
  - [ ] 3.5 Опционально: USB / Wi-Fi-Bluetooth / GPIO / HDMI / аудио / PCIe
  - [ ] Собрать итоговую папку `load_chain/` с финальным образом `psych-ward-os`

</details>

---

## Открытые вопросы

Фиксировать здесь по мере возникновения:

-
