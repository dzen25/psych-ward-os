# Psych Ward OS — RPi4: инструкции по сборке

Все команды сборки и настройки хост-окружения для порта на Raspberry Pi 4 — в одном месте. Зачем и почему — в [README.md](../README.md) и [ROADMAP.md](ROADMAP.md); здесь только команды. **Разделы идут в порядке выполнения на новой машине** — сверху вниз.

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
sudo apt install -y device-tree-compiler u-boot-tools minicom openssl
```

- `device-tree-compiler` — `dtc`/`fdtdump`, декомпиляция `.dtb` → `.dts`, проверка встроенного ключа (Фаза 2, Фаза 13).
- `u-boot-tools` — `mkimage`, FIT-подпись загрузочного образа (Фаза 13).
- `minicom` (или `screen`/`picocom`) — serial-консоль к плате через USB-UART переходник.
- `openssl` — генерация RSA-ключа подписи загрузочного образа (Фаза 13). Обычно уже стоит в системе, но лучше не полагаться на это.

Репозиторий `repo` (Google) — нужен для `repo sync` (см. ниже), общий инструмент и для этого репозитория, и (отдельно) для тестовой прошивки — [load_chain_test/INSTRUCTIONS.md](../load_chain_test/INSTRUCTIONS.md):

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

repo init -u https://github.com/seL4/sel4test-manifest.git && repo sync

python3 -m venv .venv
source .venv/bin/activate
pip install -r requirements.txt
```

`repo sync` здесь обязателен, а не опционален для «сначала попробовать hello-world» — `init-build.sh` (используется и hello-world-, и основной сборкой ниже) сам является симлинком на `tools/seL4/cmake-tool/init-build.sh`, а `projects/sel4test/settings.cmake` напрямую ссылается на `tools/seL4`, `tools/nanopb`, `tools/opensbi`. Без `repo sync` эти папки пустые/отсутствуют, и `../init-build.sh` — битый симлинк, сборка (любая) не запустится.

`.venv/` — прямо в корне репозитория (в `.gitignore`, не коммитится). `build_and_sign.sh` активирует его сам, если он есть — руками `source .venv/bin/activate` нужно только при работе напрямую с `ninja`/`init-build.sh` в обход скрипта.

</details>

---

<details>
<summary><b>🚀 Пайплайны — коротко, весь путь целиком</b></summary>

Три шага, по порядку. Ниже — только команды; зачем каждая нужна и какие узлы за ней стоят — в соответствующих разделах дальше по файлу.

```bash
./setup_uboot_secure_boot.sh   # 1. ОДИН РАЗ (заново — только при смене .fit-signing-key/ или конфига U-Boot)
./build_and_sign.sh            # 2. КАЖДЫЙ билд — ninja + подпись + раскладка в load_chain/
./flash.sh -br                 # 3. На отдельной машине с картой (projects/sel4test/apps/sel4test-driver/src/rt/) — прошивка обеих партиций
```

- Оба скрипта в корне репозитория (`setup_uboot_secure_boot.sh`/`build_and_sign.sh`) **самовосстанавливающие** — если `load_chain/` отсутствует целиком (свежий клон) или урезана, оба сами создадут нужные подпапки (`mkdir -p`) при следующем запуске, руками ничего готовить не надо.
- `load_chain/` — **в `.gitignore`, не коммитится** (собирается заново из `build-rpi4/` каждым запуском `build_and_sign.sh`). Внутри — два подкаталога, 1:1 с партициями SD-карты (см. «💾 Подготовка SD-карты» ниже):
  - `load_chain/BOOT/` — FAT32-партиция (`start4.elf`, `fixup4.dat`, `config.txt`, `bcm2711-rpi-4-b.dtb`, `overlays/`, `u-boot.bin`, `boot.itb`). `u-boot.bin` кладёт `setup_uboot_secure_boot.sh`, `boot.itb` — `build_and_sign.sh` (шаг `[6/6]`, при каждой сборке); остальное (`start4.elf`/`fixup4.dat`/`config.txt`/`bcm2711-rpi-4-b.dtb`/`overlays/`) — статичные файлы прошивки RPi4, ни один скрипт их не генерирует, кладутся туда вручную один раз (взять из офиц. `raspberrypi/firmware/boot`, см. «💾 Подготовка SD-карты»).
  - `load_chain/RPI/` — exFAT-партиция (`bin/`, `sbin/` (+ `sbin/tests/`), `etc/`, `conf/`, `service/`, `root/`) — целиком собирается `build_and_sign.sh` при каждом запуске.
- `rt/flash.sh` (на отдельной машине, где физически смонтирована SD-карта — обычно macOS, `projects/sel4test/apps/sel4test-driver/src/rt/`) синхронизирует эти же два подкаталога на партиции `BOOT`/`RPI` по rsync через SSH (`BOOT_ITEMS`/`RPI_ITEMS` внутри скрипта уже знают про подкаталоги) — этот файл не запускается и не собирается на сборочной машине, только читается по SSH.
</details>

---

<details>
<summary><b>🔨 Тестовая прошивка (sel4test hello-world, Фаза 1.3)</b></summary>

Полностью отдельный, самодостаточный цикл (свой repo sync, свой venv, своя сборка U-Boot без Фазы 13, своя разметка карты) — специально вынесен из этого файла, чтобы не путаться с основной прошивкой ниже. Всё — от зависимостей до проверки на живом железе — в [load_chain_test/INSTRUCTIONS.md](../load_chain_test/INSTRUCTIONS.md).

Необязательный, но рекомендуемый шаг: если этот образ (от seL4 Foundation, без единой строчки кода `psych-ward-os`) не грузится на вашем железе/карте/проводах — проблема гарантированно в тулчейне, а не в шагах ниже. Дальше можно сразу перейти к следующему разделу.

</details>

---

Дальнейшие пункты это описание каждого процесса из `Пайплайны — коротко, весь путь целиком`. Дальше идти не обязательно если текущий процесс вас устраивает.

---

<details>
<summary><b>🔨 Сборка U-Boot (с проверкой подписи — Фаза 13)</b></summary>

Голый `rpi_4_defconfig` сюда больше не годится как есть — с Фазы 13 (см. [ROADMAP.md](ROADMAP.md)) сам U-Boot проверяет RSA/FIT-подпись загрузочного образа перед запуском, и это требует нескольких опций конфига поверх дефолта, плюс своей boot-команды (отдельный `boot.scr` на карте больше не используется), плюс встраивания публичного ключа.

### Быстрый способ (рекомендуется)

```bash
./setup_uboot_secure_boot.sh
```

Один скрипт в корне репозитория (запускать ОДИН РАЗ, до первого `build_and_sign.sh`; заново — только при смене `.fit-signing-key/` или конфига U-Boot): сгенерирует `.fit-signing-key/`, если его ещё нет; клонирует `~/u-boot`, если его ещё нет (переопределить путь — переменная `UBOOT_DIR`); применит патч `tools/boot_fit/rpi_merge_signature.patch` (идемпотентно — определяет, применён ли уже); настроит конфиг с нуля при каждом запуске (предсказуемее, чем копить правки); соберёт дважды (первый проход даёт "пустой" `u-boot.dtb`, между проходами публичный ключ встраивается в него через `mkimage -K ... -r`, второй проход пересобирает `u-boot.bin` уже с ключом внутри); проверит результат локально (`fdtdump` + `fit_check_sign`, без обращения к железу) и скопирует готовый `u-boot.bin` в `load_chain/`.

⚠️ **DTB прошивки НЕ подменяется — ключ вписывается в него патчем `board_fdt_blob_setup()`.** Первый вариант (полная замена DTB через `--disable CONFIG_OF_BOARD`) на реальном железе трижды ломался (нумерация MMC, режим SD-карты, и в итоге — настоящий сбой DMA EMMC2-контроллера, воспроизводимо: `mmc read` рапортует успех, но физически не пишет в память) — подробности и вся история находок в [ROADMAP.md](ROADMAP.md), Фаза 13. Вместо замены — `board_fdt_blob_setup()` (патч в `tools/boot_fit/rpi_merge_signature.patch`) копирует узел `/signature/<key>` из СВОЕГО (`OF_SEPARATE`) DTB в DTB прошивки, а не отбрасывает прошивочный DTB целиком — аппаратное описание остаётся ровно тем, на котором плата всегда работала.

### Вручную, по шагам (если скрипт не подходит)

```bash
cd ~
git clone https://github.com/u-boot/u-boot.git u-boot && cd u-boot
git apply /path/to/psych-ward-os/tools/boot_fit/rpi_merge_signature.patch
make CROSS_COMPILE=aarch64-linux-gnu- rpi_4_defconfig
./scripts/config --enable FIT --enable FIT_SIGNATURE --enable RSA --enable RSA_VERIFY \
    --disable LEGACY_IMAGE_FORMAT --disable OF_OMIT_DTB \
    --set-str BOOTCOMMAND "setenv autostart yes; fatload mmc 0 0x20000000 boot.itb; bootm 0x20000000"
make CROSS_COMPILE=aarch64-linux-gnu- olddefconfig
make CROSS_COMPILE=aarch64-linux-gnu- -j"$(nproc)"
# результат: u-boot.bin — но ключ в нём ЕЩЁ НЕ встроен, см. следующий раздел
```

`CONFIG_OF_BOARD` НЕ трогаем (остаётся включённым, дефолт) — DTB прошивки используется как обычно. `--disable OF_OMIT_DTB` всё ещё нужен: свой (`OF_SEPARATE`) DTB — это НОСИТЕЛЬ ключа, `board_fdt_blob_setup()` берёт `/signature` из него.

</details>

---

<details>
<summary><b>🔐 Secure boot загрузочного образа (FIT/RSA, Фаза 13)</b></summary>

С Фазы 13 (см. [ROADMAP.md](ROADMAP.md)) отдельный `boot.scr` на карте больше не используется — boot-команда зашита прямо в `u-boot.bin` (`CONFIG_BOOTCOMMAND`), и сам U-Boot проверяет RSA-подпись образа ядра перед загрузкой (`CONFIG_FIT_SIGNATURE`). Это ДРУГОЙ механизм, чем Ed25519 из Фазы 12 — `tools/sign_elf` тут не используется.

**Быстрый способ**: `./setup_uboot_secure_boot.sh` (см. раздел «🔨 Сборка U-Boot» выше) делает всё сразу — генерирует `.fit-signing-key/`, собирает U-Boot, встраивает в него ключ, проверяет и кладёт `u-boot.bin` в `load_chain/`. Дальше — обычный `./build_and_sign.sh`, он подпишет сам образ ядра.

### Вручную, по шагам (если скрипт не подходит)

Ключ (`.fit-signing-key/` — каталог, не файл, `mkimage -k` требует директорию; в `.gitignore`, только на этой машине):

```bash
mkdir -p .fit-signing-key
openssl genpkey -algorithm RSA -out .fit-signing-key/dev.key -pkeyopt rsa_keygen_bits:2048 -pkeyopt rsa_keygen_pubexp:65537
openssl req -batch -new -x509 -key .fit-signing-key/dev.key -out .fit-signing-key/dev.crt -days 36500 -subj /CN=psych-ward-os-boot-signing
```

Раздел «🔨 Сборка U-Boot» выше (ручные шаги) даёт `u-boot.dtb`, который умеет проверять подписи, но ещё БЕЗ самого ключа. Внедрить ключ и подписать тестовый образ одной командой (она же вписывает публичный ключ в `u-boot.dtb`, флаг `-r` делает проверку обязательной, не опциональной; на реальной плате этот ключ заберёт оттуда и впишет в DTB прошивки уже пропатченный `board_fdt_blob_setup()`):

```bash
cd ~/u-boot
tools/mkimage -f /path/to/boot.its -K u-boot.dtb -k ~/psych-ward-os/.fit-signing-key -r /tmp/test.itb
cp u-boot.dtb dts/dt.dtb   # иначе пересборка не подхватит ключ
rm -f u-boot-dtb.bin u-boot.bin
make CROSS_COMPILE=aarch64-linux-gnu- -j"$(nproc)"   # финальный u-boot.bin теперь с ключом внутри
```

`/path/to/boot.its` тут — готовый (не шаблонный) `.its`, например тот, что `build_and_sign.sh` временно генерирует из `tools/boot_fit/boot.its.template` (можно один раз собрать его вручную для этого шага — важен только сам факт подписи+внедрения ключа, не конкретное содержимое тестового образа).

Проверить локально без железа (входит в комплект самой сборки U-Boot):

```bash
tools/fit_check_sign -f /tmp/test.itb -k u-boot.dtb -c conf-1
```

Готовый `u-boot.bin` скопировать в `load_chain/BOOT/u-boot.bin` (как любой другой файл этой папки — прошивает пользователь через `rt/flash.sh`).

### Рутинная пересборка/подпись образа ядра

`./build_and_sign.sh` (см. ниже) делает это сам, шаг `[6/6]` — рендерит `tools/boot_fit/boot.its.template` под сырой образ прямо из `build-rpi4/apps/sel4test-driver/images/` (в `load_chain/` сырой образ больше не копируется — на SD-карту он не попадает, U-Boot с Фазы 13 грузит только `boot.itb`) и подписывает в `load_chain/BOOT/boot.itb`. `u-boot.bin` при этом НЕ пересобирается — этот шаг нужен отдельно, только при смене `.fit-signing-key/` или конфига U-Boot.

**Модель угроз, честно**: защищён образ ядра + boot-команда. `start4.elf`/`fixup4.dat`/`config.txt`/FAT32-копия `bcm2711-rpi-4-b.dtb`/сам `u-boot.bin` до своего запуска — вне схемы, грузятся GPU-прошивкой RPi4 раньше, чем U-Boot вообще существует. Настоящая защита этого требует OTP secure boot самой платы (прожиг физических fuse — необратимо) — сознательно не делается.

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

После компиляции образа вы можете убрать предупреждения об ошибках в `main`: В папке `psych-ward-os/projects/sel4test/apps/sel4test-driver/src/rt` есть файл `c_cpp_properties.json` который надо переместить в папку `.vscode` - это нужно чтобы указать анализатору `IntelliSense`, что он работает с кодом для `AArch64`.
</details>

---

<details>
<summary><b>🔏 Подпись бинарников (Ed25519, Фаза 12)</b></summary>

С Фазы 12 (см. [ROADMAP.md](ROADMAP.md)) `load_elf_from_disk()` в `main.cpp` отказывается использовать любой файл с диска без действительной Ed25519-подписи — это касается ВСЕХ файлов из `load_chain/RPI/sbin/`, `load_chain/RPI/service/` и `load_chain/RPI/etc/init.conf` (не только `.elf`).

### Быстрый способ (рекомендуется)

```bash
./build_and_sign.sh
```

Один скрипт в корне репозитория: сам соберёт `tools/sign_elf`, если его ещё нет; проверит наличие приватного ключа (`.signing-key`) и, если ключа нет, интерактивно предложит его сгенерировать (после чего попросит вставить напечатанный публичный ключ в `main.cpp` и перезапуститься); прогонит `ninja`; подпишет и разложит по `load_chain/` **всё**, что реально собралось в `build-rpi4/apps/sel4test-driver/` — `/sbin` (любой `sb_*`) и `/service` (любой `svc_*`) находятся автогенерацией списка по факту сборки, не хардкодом, так что новая `/sbin`-команда подхватится сама, без правки скрипта. После него — как обычно, `rt/flash.sh` (`-br`).

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
  ./sign_elf genkey ../../.signing-key
  # печатает публичный ключ C-массивом — вставить в main.cpp как OS_PUBLIC_KEY[32]
  ```
  Приватный ключ (`.signing-key` в корне репозитория) — в `.gitignore`, НЕ в git, НЕ на SD-карте, только на этой сборочной машине. Публичный (`OS_PUBLIC_KEY` в `main.cpp`) — не секрет, часть исходников.
- **Подписывание файла вручную** (когда скрипт не подходит — единичный файл и т.п.), вместо `cp`:
  ```bash
  tools/sign_elf/sign_elf sign .signing-key <вход> <выход_в_load_chain>
  ```
  Файл без подписи (или с неверной) — `load_elf_from_disk()` вернёт ошибку, в логе будет `[SIG] отсутствует подпись: <путь>` либо `[SIG] ПОДПИСЬ НЕВЕРНА: <путь>` — команда/сервис просто не запустится, паники всей ОС нет.
- Образ ядра (`sel4test-driver-image-arm-bcm2711`) и встроенные в CPIO драйверы (`uart_driver`/`timer_driver`/`shell`/`blk_driver`/`net_driver`) подписи Ed25519 не требуют — они не проходят через `load_elf_from_disk()`. Загрузочная FAT32-партиция защищена ОТДЕЛЬНЫМ механизмом — см. Фазу 13 выше (RSA/FIT, проверяется самим U-Boot).

</details>

---

<details>
<summary><b>💾 Подготовка SD-карты (2 партиции: FAT32 + exFAT, Фаза 10)</b></summary>

С Фазы 10 (см. [ROADMAP.md](ROADMAP.md)) карта размечена ДВУМЯ MBR-партициями — прошивка RPi4/U-Boot читают только FAT32, `blk_driver` монтирует только exFAT:

- Партиция 1 (`BOOT`, FAT32) — `start4.elf`, `fixup4.dat`, `bcm2711-rpi-4-b.dtb`, `config.txt`, `u-boot.bin`, `boot.itb` (подписанный образ ядра, Фаза 13 — `boot.scr` больше не используется), `overlays/`.
- Партиция 2 (`RPI`, exFAT) — `bin/`, `sbin/`, `etc/`, `conf/`, `service/`, `root/`.

Обе готовые структуры лежат в `load_chain/` этого репозитория, каждая в своём подкаталоге 1:1 с именем партиции — `load_chain/BOOT/` и `load_chain/RPI/` (сам `load_chain/` в `.gitignore`, не в git, собирается локально — см. «🚀 Пайплайны» в начале файла).

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
