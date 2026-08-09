#!/bin/bash
# setup_uboot_secure_boot.sh — Фаза 13: собрать U-Boot с проверкой FIT/RSA-
# подписи (CONFIG_FIT_SIGNATURE) и положить готовый u-boot.bin в load_chain/.
# Запускать ОДИН РАЗ после установки зависимостей (см. INSTRUCTIONS.md) —
# до первого ./build_and_sign.sh. Заново — только если сменился
# .fit-signing-key/ или конфиг U-Boot; повторный запуск безопасен (всегда
# пересобирает control dtb с нуля, чтобы не накапливать старые ключи).
#
# Дизайн (после хардварной находки на реальной плате, см. ROADMAP.md,
# Фаза 13): DTB, который передаёт GPU-прошивка, ОСТАЁТСЯ единственным
# источником аппаратного описания (CONFIG_OF_BOARD включён, как обычно) —
# полная замена на собственный DTB U-Boot ломала EMMC2 DMA на этой плате.
# Вместо замены — узел /signature (публичный ключ + required) ВПИСЫВАЕТСЯ
# в DTB прошивки патчем board_fdt_blob_setup() (см.
# tools/boot_fit/rpi_merge_signature.patch), применяемым этим скриптом.
#
# Использование: ./setup_uboot_secure_boot.sh
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
UBOOT_DIR="${UBOOT_DIR:-$HOME/u-boot}"
FIT_KEY_DIR="$ROOT/.fit-signing-key"
BOOT_ITS_TEMPLATE="$ROOT/tools/boot_fit/boot.its.template"
RPI_PATCH="$ROOT/tools/boot_fit/rpi_merge_signature.patch"
PCIE_PATCH="$ROOT/tools/boot_fit/pcie_no_os_prepare.patch"
XHCI_PATCH="$ROOT/tools/boot_fit/xhci_pci_no_os_prepare.patch"
LOAD_CHAIN="$ROOT/load_chain"
CROSS="aarch64-linux-gnu-"

echo "=== Psych Ward OS: U-Boot + FIT/RSA secure boot (Фаза 13) ==="

# Идемпотентное применение патча — общее для rpi.c (Фаза 13) и
# pcie_brcmstb.c (Фаза 14, см. ниже): пробуем применить вперёд, если не
# получилось — проверяем, не применён ли он УЖЕ (--reverse --check), и
# только если ни то ни другое — падаем с понятной причиной вместо тихого
# несоответствия исходников.
apply_patch_idempotent() {
    local patch="$1" desc="$2"
    if ! git apply --check "$patch" 2>/dev/null; then
        if git apply --reverse --check "$patch" 2>/dev/null; then
            echo "      Патч ($desc) уже применён."
        else
            echo "!!! Патч $patch не применяется (ни вперёд, ни назад) —"
            echo "!!! исходники U-Boot могли измениться. Разбирайтесь руками."
            exit 1
        fi
    else
        echo "      Применяю патч ($desc) ..."
        git apply "$patch"
    fi
}

# --- 1. RSA-ключ подписи загрузочного образа ---
if [ ! -d "$FIT_KEY_DIR" ]; then
    echo "[1/6] Ключ не найден — генерирую $FIT_KEY_DIR ..."
    mkdir -p "$FIT_KEY_DIR"
    openssl genpkey -algorithm RSA -out "$FIT_KEY_DIR/dev.key" \
        -pkeyopt rsa_keygen_bits:2048 -pkeyopt rsa_keygen_pubexp:65537
    openssl req -batch -new -x509 -key "$FIT_KEY_DIR/dev.key" \
        -out "$FIT_KEY_DIR/dev.crt" -days 36500 -subj /CN=psych-ward-os-boot-signing
else
    echo "[1/6] Ключ найден: $FIT_KEY_DIR"
fi

# --- 2. Клонировать U-Boot, если ещё нет ---
if [ ! -d "$UBOOT_DIR" ]; then
    echo "[2/6] Клонирую U-Boot в $UBOOT_DIR ..."
    git clone https://github.com/u-boot/u-boot.git "$UBOOT_DIR"
elif [ ! -d "$UBOOT_DIR/.git" ]; then
    echo "!!! $UBOOT_DIR существует и не похож на git-репозиторий U-Boot."
    echo "!!! Прерываю — разберитесь руками или укажите другой путь через UBOOT_DIR=..."
    exit 1
else
    echo "[2/6] U-Boot уже склонирован: $UBOOT_DIR"
fi

# --- Патч board_fdt_blob_setup() (merge /signature в DTB прошивки, не
# замена, Фаза 13) + два патча Фазы 14 (см. ROADMAP.md, живая находка про
# зависание на первом же чтении xHCI): pcie_brcmstb.c и xhci-pci.c ОБА
# зарегистрированы с DM_FLAG_OS_PREPARE (мост И само устройство xHCI по
# отдельности) — U-Boot вызывает remove() обоих перед прыжком в наше ядро,
# гася линию PCIe/останавливая контроллер; патча одного моста НЕ хватило,
# нужны оба. ---
(
    cd "$UBOOT_DIR"
    apply_patch_idempotent "$RPI_PATCH" "rpi.c, merge /signature вместо замены DTB"
    apply_patch_idempotent "$PCIE_PATCH" "pcie_brcmstb.c, убрать DM_FLAG_OS_PREPARE (мост)"
    apply_patch_idempotent "$XHCI_PATCH" "xhci-pci.c, убрать DM_FLAG_OS_PREPARE (само устройство)"
)

# --- 3. Конфиг: базовый rpi_4_defconfig + всё нужное для Фазы 13.
# Пересоздаётся с нуля при каждом запуске — предсказуемее, чем накапливать
# правки поверх правок. CONFIG_OF_BOARD НЕ трогаем — остаётся включённым
# (дефолт), DTB прошивки используется как обычно, см. дизайн выше. ---
echo "[3/6] Настраиваю конфиг (rpi_4_defconfig + FIT_SIGNATURE/RSA) ..."
(
    cd "$UBOOT_DIR"
    make CROSS_COMPILE="$CROSS" rpi_4_defconfig
    ./scripts/config --enable FIT --enable FIT_SIGNATURE --enable RSA --enable RSA_VERIFY \
        --disable LEGACY_IMAGE_FORMAT --disable OF_OMIT_DTB \
        --set-str BOOTCOMMAND "setenv autostart yes; fatload mmc 0 0x20000000 boot.itb; bootm 0x20000000"
    # Фаза 14 (USB, xHCI, см. ROADMAP.md) — финальная находка: "сырой" BAR0
    # (0xC0000000, см. предыдущие диагностические попытки в истории этого
    # файла/ROADMAP.md) — PCI-BUS-SIDE адрес, DT `ranges` PCIe-узла
    # транслирует его в CPU-адрес 0x600000000 (недоступен seL4). Сам DTB
    # на SD-карте (load_chain/bcm2711-rpi-4-b.dtb, НЕ этот U-Boot и не его
    # BOOTCOMMAND) отредактирован fdtput'ом — outbound-окно перенацелено
    # на 0xfd600000. U-Boot(host)/эта конфигурация тут больше ни при чём —
    # никаких дополнительных патчей/диагностики в BOOTCOMMAND не требуется.
    # ВРЕМЕННО (Фаза 14, см. ROADMAP.md): проверил в исходниках ядра seL4
    # (Arch_createObject, kernel/src/arch/arm/64/object/objecttype.c) — там
    # НЕТ зануления для обычных (non-reset) Frame-объектов, ни device, ни
    # non-device. Значит "нули" в наших чтениях — не seL4. Sentinel-тест
    # (тот же приём, что уже использовался в проекте для диагностики DMA):
    # пишем 0xdeadbeef по 0xC0000000, сразу читаем обратно. "Прилипло" —
    # обычная перезаписываемая память (адрес физически не ведёт к живым
    # регистрам устройства). НЕ прилипло — это настоящее железо (read-only
    # capability-регистры игнорируют запись).
    # Фаза 14 (USB, xHCI, см. ROADMAP.md) — диагностика на живом железе
    # (md.b/pci bar/echo-метки в BOOTCOMMAND, уже убраны) дала два вывода:
    # (1) CONFIG_USB_KEYBOARD/USB_STORAGE/CMD_USB должны быть ВКЛЮЧЕНЫ
    # (rpi_4_defconfig, дефолт) — именно xhci_pci.probe() (см.
    # xhci_pci_init()) включает PCI Memory Space/Bus Master для VL805,
    # без пробы устройство никогда не становится доступным по шине; (2)
    # реальный BAR0 после PCI-энумерации U-Boot'а — 0xC0000000 (4KB), а НЕ
    # задокументированный плоский алиас 0xfe9c0000/1MB — U-Boot'овский PCI
    # auto-config назначает адрес динамически. Дальнейшая диагностика (сам
    # md.b 0xfe9c0000 вешал уже сам U-Boot, до fatload/bootm) — теперь в
    # main.cpp (root печатает дамп boot-info untyped list через
    # seL4_DebugPutString, см. "[BRINGUP] Untyped list dump").
    # boot.itb грузится НЕ в 0x10000000 (то же самое место, куда bootm потом
    # копирует распакованный образ ядра, per load/entry в .its) — иначе
    # bootm пытается скопировать ядро поверх ещё не до конца разобранного
    # FIT-контейнера: "ERROR: new format image overwritten", подтверждено
    # на живом железе. 0x20000000 — просто отдельный, ничем не занятый
    # адрес под сам контейнер; 0x10000000 остаётся местом, куда реально
    # попадает распакованный образ ядра (see tools/boot_fit/boot.its.template).
    #
    # "setenv autostart yes" — тоже не опционально, подтверждено на живом
    # железе. Без сохранённого uboot.env переменная "autostart" не задана,
    # do_bootm_standalone() (обработчик os="u-boot") в этом случае молча
    # НЕ вызывает точку входа вообще (просто return, сразу) — elfloader не
    # запускается, и т.к. type FIT-образа = "kernel" (не "standalone" —
    # это специально: "standalone" не проходит валидацию типа в
    # boot/image-fit.c при ссылке через configurations/kernel=, см. ROADMAP.md),
    # boot_selected_os() трактует этот мгновенный возврат как фатальный и
    # ресетит плату. С autostart=yes elfloader реально запускается и, как
    # задуман, никогда не возвращается — до этой проверки просто не доходит.
    # --disable OF_OMIT_DTB — нужен СВОЙ (OF_SEPARATE) DTB как НОСИТЕЛЬ
    # ключа: board_fdt_blob_setup() берёт из него узел /signature и
    # вписывает в DTB прошивки. По умолчанию U-Boot вообще не собирал бы
    # и не встраивал бы u-boot.dtb, раз CONFIG_OF_BOARD включён.
    make CROSS_COMPILE="$CROSS" olddefconfig
)

# --- 4. Проход 1: чистая сборка БЕЗ ключа (нужна, чтобы получить валидный
# u-boot.dtb, куда потом встраивается ключ) ---
echo "[4/6] Собираю (проход 1/2, без ключа) ..."
(
    cd "$UBOOT_DIR"
    rm -f dts/dt.dtb u-boot.dtb u-boot-dtb.bin u-boot.bin
    make CROSS_COMPILE="$CROSS" -j"$(nproc)"
)

echo "      Встраиваю публичный ключ в u-boot.dtb (mkimage -K ... -r) ..."
dummy_its="$(mktemp --suffix=.its)"
dummy_data="$(mktemp)"
# Содержимое здесь не имеет значения — эта тестовая подпись сразу
# выбрасывается, нужен только побочный эффект: ключ+required=\"conf\",
# вписанные в u-boot.dtb (откуда их потом на живой плате заберёт
# board_fdt_blob_setup() и впишет в DTB прошивки). Реальный boot.itb с
# реальным образом ядра подписывает уже build_and_sign.sh отдельно.
echo -n "psych-ward-os-boot-fit-keygen" > "$dummy_data"
sed "s|@@IMAGE_PATH@@|$dummy_data|" "$BOOT_ITS_TEMPLATE" > "$dummy_its"
keygen_test_itb="$(mktemp --suffix=.itb)"
( cd "$UBOOT_DIR" && tools/mkimage -f "$dummy_its" -K u-boot.dtb -k "$FIT_KEY_DIR" -r "$keygen_test_itb" )
rm -f "$dummy_its" "$dummy_data"

# --- Проход 2: пересборка, чтобы обновлённый (уже с ключом) u-boot.dtb
# реально попал в финальный u-boot.bin — один проход этого не даёт. ---
echo "[4/6] Собираю (проход 2/2, с ключом) ..."
(
    cd "$UBOOT_DIR"
    cp u-boot.dtb dts/dt.dtb
    rm -f u-boot-dtb.bin u-boot.bin
    make CROSS_COMPILE="$CROSS" -j"$(nproc)"
)

# --- 5. Самопроверка (без железа) + деплой. Проверяет, что ключ реально
# лежит в СОБСТВЕННОМ (OF_SEPARATE) u-boot.dtb — то, что merge в DTB
# прошивки на реальной плате сработает именно так, как задумано,
# подтверждается только на живом железе (см. ROADMAP.md). ---
echo "[5/6] Проверяю и копирую в load_chain/u-boot.bin ..."
(
    cd "$UBOOT_DIR"
    # Через переменную, не пайпом: fdtdump выводит весь dtb (там огромные
    # RSA-массивы), а `grep -q` находит совпадение рано и сразу закрывает
    # пайп — fdtdump получает SIGPIPE, с `pipefail` это ложно валит проверку,
    # даже когда искомая строка реально найдена.
    dtb_dump="$(fdtdump u-boot.dtb 2>/dev/null)"
    if ! grep -q 'required = "conf"' <<< "$dtb_dump"; then
        echo "!!! В u-boot.dtb не нашёлся required-ключ — что-то пошло не так, прошивать не стоит." >&2
        exit 1
    fi
    # Не полагаемся на код возврата fit_check_sign целиком — он возвращает
    # код ОТ ДОПОЛНИТЕЛЬНОГО шага (bootm_host_load_images, симуляция полной
    # загрузки), не только от самой RSA-проверки (fit_config_verify). При
    # отладке (с type="standalone", позже отменено — см. Фазу 13) этот
    # host-инструмент спотыкался на симуляции ЗАГРУЗКИ ("No U-Boot Invalid
    # ARCH..."), хотя сама подпись проверялась чисто — оставляем эту более
    # надёжную проверку и с type="kernel", не полагаемся на конкретную
    # комбинацию полей: "Verified OK, loading images" в выводе доказывает
    # успех именно RSA-проверки, отдельно от итогового кода возврата.
    check_out="$(tools/fit_check_sign -f "$keygen_test_itb" -k u-boot.dtb -c conf-1 2>&1)" || true
    if ! grep -q 'Verified OK, loading images' <<< "$check_out"; then
        echo "!!! fit_check_sign не подтвердил подпись:" >&2
        echo "$check_out" >&2
        exit 1
    fi
)
rm -f "$keygen_test_itb"
cp "$UBOOT_DIR/u-boot.bin" "$LOAD_CHAIN/u-boot.bin"

# --- 6. Патч bcm2711-rpi-4-b.dtb (Фаза 14, USB/xHCI, см. ROADMAP.md) ---
# ВРЕМЕННО ОТКЛЮЧЕНО (см. ROADMAP.md, "Тринадцатая попытка"): этот шаг
# перенаправлял PCIe outbound-окно на 0xfd600000/8MB (было 0x600000000/1GB,
# недоступно seL4) — на живом железе это вызвало НАСТОЯЩИЙ краш САМОГО
# U-Boot ("Error" handler, esr 0xbf000002, x0=0xfd600000/x1=0x00800000 —
# ровно наши новые адрес/размер как аргументы упавшей функции) прямо на
# "starting USB..." — хуже, чем просто "xHCI не отвечает". DTB на SD-карте
# откачен обратно на 0x600000000/1GB (см. .bak-pre-pcie-ranges) — это
# возвращает стабильную загрузку (без краша), но БЕЗ рабочего xHCI. Блок
# ниже оставлен для истории/повторного включения ПОСЛЕ того, как будет
# понятна настоящая причина краша (вероятно: адрес 0xfd600000 не замаплен
# в собственных таблицах страниц U-Boot, либо PCI auto-config не
# справляется с таким маленьким/таким расположенным окном) — НЕ
# раскомментировать вслепую.
: <<'DISABLED_PCIE_DTB_PATCH'
echo "[6/6] Проверяю/патчу PCIe outbound-окно в bcm2711-rpi-4-b.dtb ..."
DTB_FILE="$LOAD_CHAIN/bcm2711-rpi-4-b.dtb"
DTB_PCIE_NODE="/scb/pcie@7d500000"
DTB_RANGES_OLD="2000000 0 c0000000 6 0 0 40000000"
DTB_RANGES_NEW="2000000 0 c0000000 0 fd600000 0 800000"
if [ -f "$DTB_FILE" ]; then
    current_ranges="$(fdtget -t x "$DTB_FILE" "$DTB_PCIE_NODE" ranges 2>/dev/null || true)"
    if [ "$current_ranges" = "$DTB_RANGES_NEW" ]; then
        echo "      Уже пропатчен (outbound-окно на 0xfd600000)."
    elif [ "$current_ranges" = "$DTB_RANGES_OLD" ]; then
        echo "      Переношу outbound-окно с 0x600000000 (недоступно seL4) на 0xfd600000 ..."
        cp "$DTB_FILE" "$DTB_FILE.bak-pre-pcie-ranges"
        fdtput -t x "$DTB_FILE" "$DTB_PCIE_NODE" ranges $DTB_RANGES_NEW
    else
        echo "!!! bcm2711-rpi-4-b.dtb: свойство 'ranges' PCIe-узла не совпадает ни со старым,"
        echo "!!! ни с новым ожидаемым значением (сейчас: '$current_ranges') — DTB мог измениться"
        echo "!!! (другая версия прошивки?). Разбирайтесь руками, не патчу вслепую."
        exit 1
    fi
else
    echo "      $DTB_FILE не найден — пропускаю (нечего патчить, load_chain ещё не разложен)."
fi
DISABLED_PCIE_DTB_PATCH

echo
echo "=== Готово: $LOAD_CHAIN/u-boot.bin собран, ключ встроен и проверен локально. ==="
echo "=== Дальше: ./build_and_sign.sh (соберёт и подпишет сам образ ядра в boot.itb), потом rt/flash.sh -br. ==="
