# load_chain_test — тестовая прошивка (sel4test hello-world, Фаза 1.3)

Полный, самодостаточный цикл: от чистой машины до подтверждённой загрузки
на живом Raspberry Pi 4. **Не пересекается** с основным `psych-ward-os` —
своя копия U-Boot (без Фазы 13/подписи), свой venv, свой repo sync, своя
разметка SD-карты. Если запутались, какую инструкцию читать — если задача
"проверить, что тулчейн/провода/карта вообще работают" — то эту; если
"собрать сам psych-ward-os" — то [../INSTRUCTIONS.md](../INSTRUCTIONS.md).

## Зачем это вообще нужно

`sel4test-driver-image-arm-bcm2711` тут — чистый образ от seL4 Foundation,
без единой строчки кода `psych-ward-os`. Смысл: проверить всю цепочку
загрузки (GPU-прошивка → device tree → U-Boot → seL4) полностью
изолированно. Если ЭТОТ образ не грузится — проблема гарантированно в
тулчейне, SD-карте или проводах, а не в собственном коде проекта. Только
после того как он подтверждённо загрузился на железе, есть смысл переходить
к сборке самого `psych-ward-os` (см. `../INSTRUCTIONS.md`).

**Осознанно без Фазы 12/13 (подписи)** — это отдельный тест именно
тулчейна, лишняя сложность здесь только мешает диагностике.

---

## 1. Зависимости

Общие с основным репозиторием пакеты (`build-essential`, `git`, `cmake`,
`ninja-build`, кросс-компилятор, `python3-venv`, `u-boot-tools`, `minicom`
и т.д.) — см. `../INSTRUCTIONS.md`, раздел «⚙️ Зависимости хоста», ставятся
один раз на всю машину.

Специфично для этого цикла — инструмент `repo` (Google), если ещё не стоит:

```bash
mkdir -p ~/.bin && curl https://storage.googleapis.com/git-repo-downloads/repo > ~/.bin/repo
chmod a+x ~/.bin/repo
export PATH=~/.bin:$PATH
```

---

## 2. Собрать U-Boot (обычный, БЕЗ Фазы 13)

Отдельная копия, не путать с `~/u-boot`, который собирает
`../setup_uboot_secure_boot.sh` для основной прошивки (та — с
`CONFIG_FIT_SIGNATURE` и зашитой boot-командой, эта — голый дефолт):

```bash
mkdir -p ~/u-boot-test && cd ~/u-boot-test
git clone https://github.com/u-boot/u-boot.git . 2>/dev/null || git clone https://github.com/u-boot/u-boot.git ~/u-boot-test
cd ~/u-boot-test
make CROSS_COMPILE=aarch64-linux-gnu- rpi_4_defconfig
make CROSS_COMPILE=aarch64-linux-gnu- -j"$(nproc)"
# результат: u-boot.bin
```

---

## 3. Собрать саму тестовую прошивку

Свой собственный `repo sync` (не тот, что в корне `psych-ward-os`) и своё
собственное `.venv` (venv из `psych-ward-os` этому дереву не виден, если не
смонтирован отдельно):

```bash
mkdir -p ~/sel4-rpi4-hello && cd ~/sel4-rpi4-hello
repo init -u https://github.com/seL4/sel4test-manifest.git && repo sync

python3 -m venv .venv
source .venv/bin/activate
pip install -r ~/psych-ward-os/requirements.txt    # тот же список зависимостей, своё отдельное .venv

mkdir cbuild && cd cbuild
../init-build.sh -DPLATFORM=rpi4 -DAARCH64=1 -DRPI4_MEMORY=<реальный RAM платы: 1024|2048|4096|8192>
ninja
# результат: images/sel4test-driver-image-arm-bcm2711
```

`init-build.sh`/`ninja` сами не проверяют и не активируют venv — без него
шаги кодогенерации упадут с `ModuleNotFoundError` (нужны
`Jinja2`/`ply`/`pyelftools`/`PyYAML` и т.д. из `requirements.txt`). Путь к
`~/psych-ward-os/requirements.txt` поправить, если репозиторий склонирован
не в домашний каталог.

⚠️ `-DRPI4_MEMORY` обязателен — без него по умолчанию считается 8GB, и на
плате с меньшим объёмом памяти загрузка может падать.

---

## 4. Автозагрузка (boot.scr)

`boot.cmd` вместо `setenv bootcmd; saveenv`, который прячет конфиг в
бинарный `uboot.env` — версионируемый скрипт лучше. Простейший вариант,
без всякой подписи (осознанно — см. «Зачем это вообще нужно» выше):

```bash
cat > boot.cmd <<'EOF'
fatload mmc 0 0x10000000 sel4test-driver-image-arm-bcm2711
go 0x10000000
EOF
mkimage -A arm64 -O linux -T script -C none -d boot.cmd boot.scr
```

`boot.scr` — в корень FAT32-раздела SD-карты, U-Boot подхватывает его сам.

---

## 5. Разметка и копирование на SD-карту

В отличие от основной прошивки (2 партиции, FAT32+exFAT, см.
`../INSTRUCTIONS.md`) — тестовой прошивке нужна **только ОДНА FAT32-
партиция**: hello-world не трогает диск, `blk_driver`/exFAT тут ни при чём.

### Разметка (macOS)

```bash
diskutil list                     # найти диск карты (например /dev/disk4) — НЕ системный
sudo diskutil eraseDisk FAT32 BOOT MBRFormat /dev/diskN
```

### Разметка (Linux)

```bash
lsblk                             # найти диск карты, НЕ системный
sudo umount /dev/sdX*
sudo mkfs.vfat -F 32 -n BOOT /dev/sdX1
```

### Копирование

На смонтированную карту (вручную, `cp`/Finder — отдельного скрипта для
этого дерева нет, `rt/flash.sh` жёстко привязан к `load_chain/` основной
прошивки, этой папки не касается):

```
start4.elf
fixup4.dat
bcm2711-rpi-4-b.dtb
config.txt
u-boot.bin        # из шага 2 (~/u-boot-test/u-boot.bin)
boot.scr          # из шага 4
sel4test-driver-image-arm-bcm2711   # из шага 3
```

Актуальные версии первых пяти файлов уже лежат прямо в этой папке
(`load_chain_test/`) — обновлять их заново не нужно, если не менялась
сама GPU-прошивка/dtb/`config.txt`.

---

## 6. Проверка на живом железе

Serial-консоль: USB-UART переходник на GPIO 14 (TXD)/15 (RXD), 3.3V (НЕ
5V!), земля — на любой GND-пин. `minicom`/`screen`/`picocom`, 115200 8N1.

Вставить карту, подать питание — в консоли должна появиться цепочка U-Boot
→ `## Starting application at 0x10000000` → лог самого seL4/sel4test.
Если тишина или зависание раньше этой точки — проблема в тулчейне/карте/
проводах (см. «Зачем это вообще нужно» выше), не в коде `psych-ward-os`.

---

## Как устроена цепочка загрузки, шаг за шагом

| # | Стадия | Файл | Источник |
|---|---|---|---|
| 1 | FSBL | — (в ROM платы) | не трогаем |
| 2 | GPU firmware | `start4.elf`, `fixup4.dat` | [raspberrypi/firmware/boot](https://github.com/raspberrypi/firmware/tree/master/boot) |
| 3 | Device tree | `bcm2711-rpi-4-b.dtb` (+ `overlays/*`) | тот же репозиторий |
| 4 | Конфиг | `config.txt` | пишем сами: `arm_64bit=1` + `kernel=u-boot.bin` |
| 5 | Bootloader | `u-boot.bin` | собираем сами (шаг 2 выше) |
| 6 | seL4-образ | `sel4test-driver-image-arm-bcm2711` | результат сборки sel4test (шаг 3 выше) |

1. **BootROM** — прошито в кристалл SoC на заводе, не на SD-карте. Читает
   конфигурацию из EEPROM платы, единственная задача — найти и запустить
   `start4.elf`. Дальше всё на GPU (VideoCore VI), ARM-ядра ещё в reset.
2. **`start4.elf` + `fixup4.dat`** — закрытая прошивка VideoCore VI.
   Поднимает и калибрует SDRAM (на этом этапе ARM-ядра ещё ничего не могут),
   читает `config.txt` и device tree с той же SD-карты. `start4.elf` —
   вариант под BCM2711/Pi4, `fixup4.dat` — согласованная с ним таблица
   разметки памяти между GPU и ARM; версии всегда брать из одного релиза,
   не смешивать.
3. **`config.txt`** — парсит GPU firmware (ARM его не видит). Два значимых
   параметра: `arm_64bit=1` (поднять ядра сразу в AArch64) и
   `kernel=u-boot.bin` (что считать «ядром» и передать на выполнение).
4. **`bcm2711-rpi-4-b.dtb`** — машиночитаемое описание железа платы. GPU
   firmware грузит его в память и передаёт указатель ARM-ядрам через
   регистр x0. `overlays/*` — опциональные фрагменты под конкретную HAT-
   периферию, для базового boot не нужны.
5. **GPU firmware выпускает ARM-ядра из reset**, передаёт управление по
   адресу `kernel=` (обычно физический `0x80000`) с указателем на dtb в
   x0 — протокол загрузчика Linux-стиля, U-Boot тоже так собран.
6. **`u-boot.bin`** — реально первое, что исполняется на ARM. seL4-образ
   нельзя указать напрямую в `kernel=` (GPU firmware понимает только
   zImage/Image-подобные загрузчики) — U-Boot даёт консоль и команды
   `fatload`/`go`, чтобы положить seL4-образ по адресу `0x10000000` и
   прыгнуть туда.
7. **`sel4test-driver-image-arm-bcm2711`** — seL4-ядро + тестовый root
   task, один плоский бинарник. U-Boot: `fatload mmc 0 0x10000000 <образ>`
   → `go 0x10000000`.

---

## Файлы в этой папке

- `start4.elf`, `fixup4.dat`, `bcm2711-rpi-4-b.dtb`, `config.txt` — готовые,
  из релиза `raspberrypi/firmware`, обновлять только при смене платы/dtb.
- `u-boot.bin` — результат шага 2 (без Фазы 13, обычная сборка).
- `sel4test-driver-image-arm-bcm2711` — результат шага 3 (hello-world от
  seL4 Foundation, не `psych-ward-os`).
