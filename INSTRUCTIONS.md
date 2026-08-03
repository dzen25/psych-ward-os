# Psych Ward OS — RPi4: инструкции по сборке

Все команды сборки и настройки хост-окружения для порта на Raspberry Pi 4 — в одном месте. Зачем и почему — в [README.md](README.md) и [ROADMAP.md](ROADMAP.md); здесь только команды.

---

<details>
<summary><b>⚙️ Зависимости хоста</b></summary>

### Базовые пакеты (общие с веткой `main`)

Нужно Linux-окружение (рекомендуется Ubuntu 22.04/24.04):

```bash
sudo apt update
sudo apt install -y build-essential git cmake ninja-build \
    gcc-aarch64-linux-gnu g++-aarch64-linux-gnu \
    qemu-system-arm python3-venv python3-pip dosfstools \
    netcat-openbsd tcpdump
```

### Пакеты, специфичные для порта на RPi4

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

### Клонирование проекта и виртуальное окружение

```bash
git clone https://github.com/dzen25/psych-ward-os.git
cd psych-ward-os
git checkout RPi4
#или
git clone https://github.com/dzen25/psych-ward-os/tree/RPi4
cd psych-ward-os


python3 -m venv .venv
source .venv/bin/activate
pip install -r requirements.txt
```

`.venv/` — прямо в корне репозитория (в `.gitignore`, не коммитится). `build_and_sign.sh` активирует его сам, если он есть — руками `source .venv/bin/activate` нужно только при работе напрямую с `ninja`/`init-build.sh` в обход скрипта.

Дерево `sel4-rpi4-hello` (Фаза 1.3, отдельно от этого репозитория) может использовать тот же `requirements.txt`, но своё собственное `.venv` — venv в корне `psych-ward-os` для него не виден, если оно не смонтировано куда-то ещё.

</details>

---

<details>
<summary><b>🔨 Сборка U-Boot</b></summary>

```bash
cd ~
git clone https://github.com/u-boot/u-boot.git u-boot && cd u-boot
make CROSS_COMPILE=aarch64-linux-gnu- rpi_4_defconfig
make CROSS_COMPILE=aarch64-linux-gnu-
ls
# результат: u-boot.bin
```

</details>

---

<details>
<summary><b>🔨 Сборка тестовой прошивки (sel4test hello-world, Фаза 1.3)</b></summary>

Собирается **вне** этого репозитория, в отдельном дереве (не в `psych-ward-os`):



```bash
mkdir -p ~/sel4-rpi4-hello && cd ~/sel4-rpi4-hello
repo init -u https://github.com/seL4/sel4test-manifest.git && repo sync
mkdir cbuild && cd cbuild
../init-build.sh -DPLATFORM=rpi4 -DAARCH64=1 -DRPI4_MEMORY=<реальный RAM платы: 1024|2048|4096|8192>
ninja
# результат: images/sel4test-driver-image-arm-bcm2711
```

⚠️ `-DRPI4_MEMORY` обязателен — без него по умолчанию считается 8GB, и на плате с меньшим объёмом памяти загрузка может падать.

</details>

---

<details>
<summary><b>🔨 Сборка основной прошивки (psych-ward-os, Фаза 3)</b></summary>

Собирается **в этом репозитории**, той же связкой `init-build.sh` + `ninja`, что и на ветке `main` под QEMU, но с платформой `rpi4` вместо `qemu-arm-virt`:

```bash
mkdir -p build-rpi4 && cd build-rpi4
../init-build.sh -DPLATFORM=rpi4 -DAARCH64=1 -DRPI4_MEMORY=<реальный RAM платы: 1024|2048|4096|8192>
ninja
# результат: images/sel4test-driver-image-arm-bcm2711 (root task — уже psych-ward-os, не hello-world)
```

Существующая папка `build/` в репозитории уже сконфигурирована под `-DPLATFORM=qemu-arm-virt` (наследие ветки `main`) — для rpi4 нужна отдельная чистая build-директория, а не пересборка в той же папке.

После компиляции образа вы можете убрать предупреждения об ошибках в `main`: В папке `psych-ward-os/projects/sel4test/apps/sel4test-driver/src/rt` есть файл `c_cpp_properties.json` который надо переместить в папку `.vscode` - это нужно чтобы указать анализатору `IntelliSense`, что он работает с кодом для `AArch64`.
</details>

---

<details>
<summary><b>🔏 Подпись бинарников (Ed25519, Фаза 12)</b></summary>

С Фазы 12 (см. [ROADMAP.md](ROADMAP.md)) `load_elf_from_disk()` в `main.cpp` отказывается использовать любой файл с диска без действительной Ed25519-подписи — это касается ВСЕХ файлов из `load_chain/sbin/`, `load_chain/service/` и `load_chain/etc/init.conf` (не только `.elf`).

### Быстрый способ (рекомендуется)

```bash
./build_and_sign.sh
```

Один скрипт в корне репозитория: сам соберёт `tools/sign_elf`, если его ещё нет; проверит наличие приватного ключа (`~/.psych-ward-os-signing-key`) и, если ключа нет, интерактивно предложит его сгенерировать (после чего попросит вставить напечатанный публичный ключ в `main.cpp` и перезапуститься); прогонит `ninja`; подпишет и разложит по `load_chain/` **всё**, что реально собралось в `build-rpi4/apps/sel4test-driver/` — `/sbin` (любой `sb_*`) и `/service` (любой `svc_*`) находятся автогенерацией списка по факту сборки, не хардкодом, так что новая `/sbin`-команда подхватится сама, без правки скрипта. После него — как обычно, `rt/flash.sh` (`-br`).

**`/etc/init.conf` — особый случай**: это не сборочный артефакт, а руками редактируемый текст, поэтому редактировать нужно `load_chain/etc/init.conf.src` (БЕЗ подписи) — `build_and_sign.sh` каждый раз подписывает его заново в `load_chain/etc/init.conf`. Редактировать `init.conf` напрямую нельзя — при повторном запуске скрипта он подпишет уже подписанный файл и трейлеры начнут копиться.

### Узлы схемы (что там внутри)

- **`tools/monocypher/`** — вендоренные исходники [Monocypher](https://monocypher.org/) (`src/monocypher.c`/`.h` + `src/optional/monocypher-ed25519.c`/`.h`), подключены напрямую в сборку `sel4test-driver` (см. `CMakeLists.txt`) — пересобираются автоматически вместе с ядром через `ninja`, отдельно ничего делать не нужно.
- **`tools/sign_elf/sign_elf.c`** — офлайн-утилита подписи, которую `build_and_sign.sh` вызывает сама. Собирается ОТДЕЛЬНО от основной прошивки, ХОСТОВЫМ `gcc` (не `aarch64-linux-gnu-gcc`) — этот бинарник никогда не попадает на плату; вручную (если нужно):
  ```bash
  cd tools/sign_elf
  gcc -O2 -o sign_elf sign_elf.c \
      ../monocypher/src/monocypher.c \
      ../monocypher/src/optional/monocypher-ed25519.c \
      -I../monocypher/src -I../monocypher/src/optional
  ```
- **Ключи** — генерируются ОДИН РАЗ (уже сделано на этой машине; при клонировании репозитория на новой машине — заново, `build_and_sign.sh` сам предложит):
  ```bash
  ./sign_elf genkey ~/.psych-ward-os-signing-key
  # печатает публичный ключ C-массивом — вставить в main.cpp как OS_PUBLIC_KEY[32]
  ```
  Приватный ключ (`~/.psych-ward-os-signing-key`, домашний каталог) — НЕ в git, НЕ на SD-карте, только на этой сборочной машине. Публичный (`OS_PUBLIC_KEY` в `main.cpp`) — не секрет, часть исходников.
- **Подписывание файла вручную** (когда скрипт не подходит — единичный файл и т.п.), вместо `cp`:
  ```bash
  tools/sign_elf/sign_elf sign ~/.psych-ward-os-signing-key <вход> <выход_в_load_chain>
  ```
  Файл без подписи (или с неверной) — `load_elf_from_disk()` вернёт ошибку, в логе будет `[SIG] отсутствует подпись: <путь>` либо `[SIG] ПОДПИСЬ НЕВЕРНА: <путь>` — команда/сервис просто не запустится, паники всей ОС нет.
- Образ ядра (`sel4test-driver-image-arm-bcm2711`) и встроенные в CPIO драйверы (`uart_driver`/`timer_driver`/`shell`/`blk_driver`/`net_driver`) подписи не требуют — они не проходят через `load_elf_from_disk()`. Загрузочная FAT32-партиция (`start4.elf`, `config.txt`, сам образ ядра) тоже вне этой схемы — U-Boot их грузит без проверки подписи вообще.

</details>

---

<details>
<summary><b>🚀 Автозагрузка (boot.scr)</b></summary>

`boot.cmd` вместо `setenv bootcmd; saveenv`, который прячет конфиг в бинарный `uboot.env` — версионируемый скрипт лучше:

```
fatload mmc 0 0x10000000 sel4test-driver-image-arm-bcm2711
go 0x10000000
```

```bash
mkimage -A arm64 -O linux -T script -C none -d boot.cmd boot.scr
```

`boot.scr` в корне SD-карты — U-Boot подхватывает его автоматически.

</details>

---

<details>
<summary><b>💾 Подготовка SD-карты (2 партиции: FAT32 + exFAT, Фаза 10)</b></summary>

С Фазы 10 (см. [ROADMAP.md](ROADMAP.md)) карта размечена ДВУМЯ MBR-партициями — прошивка RPi4/U-Boot читают только FAT32, `blk_driver` монтирует только exFAT:

- Партиция 1 (`BOOT`, FAT32) — `start4.elf`, `fixup4.dat`, `bcm2711-rpi-4-b.dtb`, `config.txt`, `u-boot.bin`, `boot.scr`, `overlays/`.
- Партиция 2 (`RPI`, exFAT) — `bin/`, `sbin/`, `etc/`, `conf/`, `service/`, `root/`.

Обе готовые структуры лежат в [`load_chain/`](load_chain) этого репозитория.

### Разметка (macOS)

```bash
diskutil list                     # найти диск карты (например /dev/disk4) — НЕ системный
sudo diskutil partitionDisk /dev/diskN MBR FAT32 BOOT 128MiB ExFAT RPI R
```

### Разметка (Linux)

```bash
lsblk                             # найти диск карты, НЕ системный
sudo umount /dev/sdX*
sudo parted /dev/sdX --script \
    mklabel msdos \
    mkpart primary fat32 1MiB 129MiB \
    mkpart primary 129MiB 100%
sudo mkfs.vfat -F 32 -n BOOT /dev/sdX1
sudo mkfs.exfat -n RPI /dev/sdX2   # нужен exfatprogs (apt install exfatprogs)
```

### Копирование файлов

Вручную (Finder/`cp -R`) — на `BOOT` содержимое первого списка выше, на `RPI` — второго.

Либо скриптом `projects/sel4test/apps/sel4test-driver/src/rt/flash.sh` (rsync с хоста сборки на смонтированные тома, запускается на машине с картой — `-h` за полным списком флагов):

```bash
./flash.sh -b       # только BOOT (обновился образ ядра/загрузочные файлы)
./flash.sh -r       # только RPI (обновились bin/sbin/etc/conf/service/root)
./flash.sh -br      # обе партиции сразу
```

</details>
