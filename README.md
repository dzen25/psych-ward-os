# 🧠 Psych Ward OS — Порт на Raspberry Pi 4

**Psych Ward OS** — экспериментальная ОС на базе микроядра **seL4** (принцип наименьших привилегий, полная изоляция компонентов, самовосстановление). Ветка `main` — стабильная версия для **QEMU** (`qemu-arm-virt`), разработка там завершена.

**Эта ветка (`RPi4`)** — активный порт на реальное железо: **Raspberry Pi 4 B (BCM2711, Cortex-A72, AArch64)**. Всё содержимое этого README относится только к порту; архитектура ядра ОС, команды shell и инструкции по сборке под QEMU описаны в README ветки `main`.

---

## Почему RPi4

seL4 официально поддерживает Raspberry Pi 4 (`-DPLATFORM=rpi4`, unverified, maintained by seL4 Foundation). Это резко снижает риск порта по сравнению со старой идеей таргета MT6260CA/ARMv5, который seL4 вообще не поддерживает.

Главные отличия от `qemu-arm-virt`, которые придётся закрыть:

| Подсистема | На QEMU | На RPi4 |
|---|---|---|
| Диск | virtio-blk | EMMC/SD-контроллер (нужен свой драйвер) |
| Сеть | virtio-net | genet — Ethernet MAC BCM2711 (нужен свой драйвер) |
| RTC | PL031 (battery-backed) | нет на плате — ARM generic timer + NTP |
| UART / GIC | PL011 / GICv2 | те же самые, только другие адреса |

---

## Структура порта: где что лежит

В репозитории две отдельные папки с файлами цепочки загрузки — не перепутать:

- **`load_chain_test/`** — файлы для Фазы 1 (проверка связи/тулчейна): чистый `sel4test` hello-world образ (`sel4test-driver-image-arm-bcm2711`), нужен только чтобы подтвердить, что железо, U-Boot и boot chain вообще работают. К самому `psych-ward-os` отношения не имеет.
- **`load_chain/`** — файлы для полноценной загрузки уже готового образа `psych-ward-os` (Фаза 3 и далее): та же цепочка (`start4.elf`, `fixup4.dat`, `config.txt`, `u-boot.bin`), но `kernel`-образ в ней — не тестовый hello-world, а итоговый собранный `psych-ward-os`. На момент написания в папке ещё нет финального образа — появятся после успешного прохождения Фазы 3.

На хост-машине (вне репозитория) заведена рабочая папка `RPi4_SeL4/` с исходниками: `load_chain/` (сырые файлы до копирования в репозиторий), `u-boot/` (клон и сборка U-Boot), `sel4-rpi4-hello/` (отдельное дерево sel4test для проверки hello-world, **не** `psych-ward-os`).

---

## Зависимости хоста

В дополнение к пакетам из ветки `main` (тулчейн `aarch64-linux-gnu`, `cmake`, `ninja`, `python3-venv`) для работы над портом нужны:

```bash
sudo apt install -y device-tree-compiler u-boot-tools minicom
```

- `device-tree-compiler` — `dtc`, декомпиляция `.dtb` → `.dts` (Фаза 2).
- `u-boot-tools` — `mkimage`, сборка `boot.scr` (Фаза 1.6).
- `minicom` (или `screen`/`picocom`) — serial-консоль к плате через USB-UART переходник.

Репозиторий `repo` (Google) нужен отдельно для сборки чистого sel4test hello-world (см. Фазу 1.3):

```bash
mkdir -p ~/.bin && curl https://storage.googleapis.com/git-repo-downloads/repo > ~/.bin/repo
chmod a+x ~/.bin/repo
export PATH=~/.bin:$PATH
```

Дерево `sel4-rpi4-hello` (Фаза 1.3) собирается тем же способом, что и `psych-ward-os` на `main` — нужно то же самое изолированное виртуальное окружение Python и те же зависимости из `requirements.txt`:

```bash
python3 -m venv ~/sel4-vibe
source ~/sel4-vibe/bin/activate
pip install -r requirements.txt
```

(если venv `~/sel4-vibe` уже создан для сборки под QEMU на `main` — просто активировать его заново, пересоздавать не нужно)

---

## Дорожная карта по фазам

### Фаза 0 — Подготовка ✅ (сделано)

- [x] Все платформо-зависимые адреса (UART/RTC/virtio-mmio, регистры PL011/PL031, GIC IRQ) вынесены в `src/h/platform.h`
- [x] Рефактор не сломал поведение на QEMU (boot/shell/net/disk/NTP работают)
- [x] Зафиксирован известный quirk: `timer_driver.cpp` использует offset `0x10` под именем «ICR» для PL031, хотя по датащиту `0x10` — это IMSC, а реальный ICR — `0x1C`. На QEMU работает, трогать не стали — при переходе на ARM generic timer этот код всё равно уходит целиком (см. Фазу 3.1)

---

### Фаза 1 — Hello World на живом железе

Цель — провалидировать тулчейн и цепочку загрузки **отдельно** от кода `psych-ward-os`, прежде чем его трогать.

#### 1.1 Цепочка загрузки RPi4 — как это работает

Все файлы лежат плоско в корне FAT32-раздела SD-карты. Порядок исполнения:

| # | Стадия | Файл | Источник |
|---|---|---|---|
| 1 | FSBL | — (в ROM платы) | не трогаем |
| 2 | GPU firmware | `start4.elf`, `fixup4.dat` | [raspberrypi/firmware/boot](https://github.com/raspberrypi/firmware/tree/master/boot) |
| 3 | Device tree | `bcm2711-rpi-4-b.dtb` (+ `overlays/*`) | тот же репозиторий |
| 4 | Конфиг | `config.txt` | пишем сами: `arm_64bit=1` + `kernel=u-boot.bin` |
| 5 | Bootloader | `u-boot.bin` | собираем сами (см. 1.2) |
| 6 | seL4-образ | `sel4test-driver-image-arm-bcm2711` | результат сборки sel4test (см. 1.3) |

Логика: ROM → GPU firmware (читает `config.txt`, грузит файл из `kernel=`) → это не seL4 напрямую, а U-Boot → U-Boot интерактивно (или по скрипту) грузит и запускает seL4-образ по адресу `0x10000000`.

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
- [x] `sel4test-driver-image-arm-bcm2711` — **не финальный**, это тестовая сборка hello-world из отдельного дерева `sel4-rpi4-hello`, будет заменён после Фазы 3

Открытый вопрос: уточнить, требуется ли что-то в `config.txt` кроме `arm_64bit=1`/`kernel=`.

#### 1.2 Собрать U-Boot

```bash
git clone https://github.com/u-boot/u-boot.git u-boot
cd u-boot
make CROSS_COMPILE=aarch64-linux-gnu- rpi_4_defconfig
make CROSS_COMPILE=aarch64-linux-gnu-
# результат: u-boot.bin
```

#### 1.3 Собрать sel4test под rpi4 (в ОТДЕЛЬНОЙ папке, не в psych-ward-os!)

```bash
mkdir -p ~/sel4-rpi4-hello && cd ~/sel4-rpi4-hello
repo init -u https://github.com/seL4/sel4test-manifest.git
repo sync
mkdir cbuild && cd cbuild
../init-build.sh -DPLATFORM=rpi4 -DAARCH64=1 -DRPI4_MEMORY=<реальный RAM платы: 1024|2048|4096|8192>
ninja
# результат: images/sel4test-driver-image-arm-bcm2711
```

⚠️ **Питфол:** если `-DRPI4_MEMORY` не указать — по умолчанию считает 8GB, и на плате с меньшим объёмом памяти загрузка может падать/вести себя странно. Всегда указывать реальный объём.

- [x] Сбор файлов закончен, образ лежит в `load_chain_test/sel4test-driver-image-arm-bcm2711`

#### 1.4 Собрать SD-карту

В корень FAT32-раздела SD-карты скопировать все файлы из [`load_chain_test/`](load_chain_test): `start4.elf`, `fixup4.dat`, `bcm2711-rpi-4-b.dtb`, `config.txt`, `u-boot.bin`, `sel4test-driver-image-arm-bcm2711`.

- [ ] Записать SD-карту

#### 1.5 Первый запуск (вручную, через serial-консоль)

Подключение: serial TX/RX — GPIO 14/15, через USB-UART переходник на 3.3V.

Прервать автобут U-Boot любой клавишей, затем:

```
fatls mmc 0
fatload mmc 0 0x10000000 sel4test-driver-image-arm-bcm2711
go 0x10000000
```

⚠️ **Питфол** (встречался у других на форуме seL4): ошибка `Reading file would overwrite reserved memory` при `fatload`/`go` на `0x10000000` — обычно означает рассинхрон `RPI4_MEMORY` с реальным объёмом RAM платы, либо слишком большой образ. Первым делом проверять `RPI4_MEMORY`.

- [ ] Получить вывод в UART при ручной загрузке — **это критерий успеха Фазы 1**

#### 1.6 Автоматизировать загрузку — `boot.scr`

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

---

### Фаза 2 — Извлечение адресов под свои драйверы

seL4-кернел на rpi4 уже знает адреса UART/GIC/generic timer — перевытаскивать их из dtb не нужно. DTB нужен только под железо, которого в seL4 нет «из коробки» — то, что понадобится собственным user-space драйверам:

- **genet** (Ethernet MAC) — замена virtio-net в `net_driver.cpp`
- **EMMC2 / SD-контроллер** — замена virtio-blk в `blk_driver.cpp`

```bash
dtc -I dtb -O dts bcm2711-rpi-4-b.dtb -o bcm2711-rpi-4-b.dts
```

(файл `bcm2711-rpi-4-b.dtb` уже есть в `load_chain_test/`, повторно скачивать не нужно)

- [ ] Декомпилировать `bcm2711-rpi-4-b.dtb`
- [ ] Найти узел `genet` (адрес, IRQ)
- [ ] Найти узел EMMC2 (адрес, IRQ)
- [ ] Занести найденные адреса в отдельную rpi4-секцию `platform.h`

---

### Фаза 3 — Порт дерева `psych-ward-os`

Начинается только после успешной Фазы 1 (подтверждённый boot на живом железе, не в QEMU).

- [x] Создана ветка `RPi4`, зафиксирован переход от стабильной QEMU-версии к порту
- [ ] Пересобрать `psych-ward-os` с `-DPLATFORM=rpi4 -DRPI4_MEMORY=...` (cmake-настройки это позволяют без правок `easy-settings.cmake`/`init-build.sh` — платформа нигде не захардкожена)
- [ ] Проверить, что элементарно собирается (ожидаемо — либо ошибки компиляции из-за qemu-специфичного кода в `main.cpp`, либо соберётся, но не заработает на железе из-за отсутствия virtio)

#### 3.1 Блокер: RTC (PL031 нет на реальном железе)

`timer_driver.cpp` сейчас использует PL031 (см. quirk из Фазы 0). На стоковой RPi4 battery-backed RTC нет.

- [ ] Заменить чтение аптайма на ARM generic timer (`CNTPCT_EL0`) — не зависит от драйвера, доступен всегда
- [ ] Wall-clock время: либо внешний I2C RTC-модуль, либо (проще, уже готово) — NTP как единственный источник. При старте без сети/NTP wall-clock будет не определено — решить, как деградировать

#### 3.2 Блокер: virtio-net → genet

`net_driver.cpp` — TX/RX ring код под virtio-net полностью заменяется на драйвер BCM2711 genet. Протокольная логика поверх (ARP/UDP/NTP/DNS парсинг) переиспользуется как есть — трогать не нужно.

- [ ] Написать минимальный genet-драйвер (init, TX, RX) по адресам из Фазы 2
- [ ] Подключить к существующему протокольному стеку `net_driver.cpp`

#### 3.3 Блокер: virtio-blk → EMMC/SD

`blk_driver.cpp` — virtio-blk код заменяется на реальный SD/EMMC-драйвер. FAT32-логика поверх переиспользуется как есть.

- [ ] Написать минимальный EMMC-драйвер (init, чтение/запись блоков) по адресам из Фазы 2
- [ ] Подключить к существующей FAT32-логике `blk_driver.cpp`

#### 3.4 UART / GIC — низкий риск

RPi4 имеет GICv2 и PL011-совместимый UART0 — ожидаются только изменения адресов в `platform.h`, без переписывания логики.

- [ ] Обновить адреса UART/GIC в `platform.h` под rpi4 (секция из Фазы 2)
- [ ] Проверить boot readiness протокол (`SYS_DRIVER_READY`/`SYS_WAIT_ALL_DRIVERS_READY`) — не завязан на конкретную платформу, должен работать как есть, но перепроверить порядок сигналов при живых genet/EMMC-драйверах

---

## Известные питфолы (не наступать повторно)

- **Boot readiness / bootstrap loop:** не заводить отдельный `seL4_Recv`-цикл между вызовами `spawn_process()` для ожидания готовности драйверов — он не обслуживает легитимные syscalls драйверов во время их инициализации (например `SYS_SHM_GET`), что уже один раз реально уронило `blk_driver` через порчу shm-указателя. Ждать нужно строго в уже работающем основном dispatch loop.
- **`RPI4_MEMORY`:** всегда указывать явно под реальный объём RAM платы — дефолт 8GB может ломать загрузку на платах с меньшим объёмом.
- **`fatload`/`go` reserved memory error:** см. Фазу 1.5 — обычно тот же рассинхрон `RPI4_MEMORY`.

---

## Текущий статус

**Сейчас на:** Фаза 1 (сборка hello-world под rpi4) — все файлы цепочки загрузки собраны в `load_chain_test/`, SD-карта ещё не записана и не тестировалась на живом железе.

---

## Работа с кодом

Приветствуются новые идеи и форки. Базовая архитектура Psych Ward OS (не связанная с портом) расписана в README ветки `main`.

---

## Полный roadmap (сводный чек-лист)

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
  - [ ] Определить какие узлы будут необходимы в целом
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
  - [ ] Собрать итоговую папку
