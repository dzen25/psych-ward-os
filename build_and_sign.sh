#!/bin/bash
# build_and_sign.sh — собрать psych-ward-os (RPi4) и разложить всё, что
# load_elf_from_disk() читает с диска, в load_chain/ УЖЕ ПОДПИСАННЫМ
# (Фаза 12, см. ROADMAP.md/INSTRUCTIONS.md). Не трогает rt/flash.sh — сама
# прошивка SD-карты по-прежнему делается пользователем отдельно.
#
# Использование: ./build_and_sign.sh
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="$ROOT/build-rpi4"
LOAD_CHAIN="$ROOT/load_chain"
SIGN_ELF="$ROOT/tools/sign_elf/sign_elf"
SIGN_KEY="$ROOT/.signing-key"
B="$BUILD_DIR/apps/sel4test-driver"

# Фаза 13 — FIT-подпись загрузочного образа (RSA, отдельно от Ed25519 выше).
MKIMAGE="$(command -v mkimage || true)"
FIT_KEY_DIR="$ROOT/.fit-signing-key"
BOOT_ITS_TEMPLATE="$ROOT/tools/boot_fit/boot.its.template"

echo "=== Psych Ward OS: сборка + подпись ==="

# --- 1. sign_elf: пересобрать, если отсутствует или устарел ---
if [ ! -x "$SIGN_ELF" ] || [ "$ROOT/tools/sign_elf/sign_elf.c" -nt "$SIGN_ELF" ]; then
    echo "[1/6] Собираю tools/sign_elf/sign_elf..."
    gcc -O2 -o "$SIGN_ELF" "$ROOT/tools/sign_elf/sign_elf.c" \
        "$ROOT/tools/monocypher/src/monocypher.c" \
        "$ROOT/tools/monocypher/src/optional/monocypher-ed25519.c" \
        -I"$ROOT/tools/monocypher/src" -I"$ROOT/tools/monocypher/src/optional"
else
    echo "[1/6] sign_elf уже собран."
fi

# --- 2. Приватный ключ подписи ---
if [ ! -f "$SIGN_KEY" ]; then
    echo "[2/6] Приватный ключ подписи не найден: $SIGN_KEY"
    read -r -p "      Сгенерировать новый сейчас? [y/N] " ans
    if [[ "$ans" =~ ^[Yy]$ ]]; then
        "$SIGN_ELF" genkey "$SIGN_KEY"
        echo
        echo "!!! Вставьте напечатанный выше публичный ключ в main.cpp (OS_PUBLIC_KEY[32]),"
        echo "!!! затем запустите ./build_and_sign.sh заново."
        exit 0
    else
        echo "Без ключа подписывать нечем. Прерываю."
        exit 1
    fi
else
    echo "[2/6] Ключ подписи найден: $SIGN_KEY"
fi

# --- 3. Сборка (ninja) ---
echo "[3/6] ninja в $BUILD_DIR ..."
if [ -f "$ROOT/.venv/bin/activate" ]; then
    # shellcheck disable=SC1091
    source "$ROOT/.venv/bin/activate"
fi
( cd "$BUILD_DIR" && ninja )

# --- 4. Раскладка сырых (ещё не подписанных) артефактов под их итоговые
# имена в load_chain/ ---
echo "[4/6] Раскладываю сырые артефакты в load_chain/ ..."

# Сырой образ ядра НЕ копируется в load_chain/ — на SD-карту он больше не
# попадает (с Фазы 13 U-Boot грузит только подписанный boot.itb, см. шаг
# [6/6] ниже, который берёт его прямо из $BUILD_DIR). Копия в load_chain/
# была бы мёртвым грузом, который никто не читает.

# /sbin, /service — любой sb_<имя>/svc_<имя> из сборки -> <имя>.elf.
# Список НЕ хардкодится — что CMake реально собрал (PSYCH_SBIN_TOOLS/
# PSYCH_SERVICE_TOOLS в CMakeLists.txt), то сюда и попадёт, включая будущие
# новые команды/сервисы — этот цикл трогать не придётся.
shopt -s nullglob
for built in "$B"/sb_*; do
    name="$(basename "$built")"; name="${name#sb_}"
    cp "$built" "$LOAD_CHAIN/sbin/${name}.elf"
done
# issuse.txt №62 (расследование) — тестовые хуки (holdshm/proxytest/
# recovertest/stresstest, PSYCH_TEST_TOOLS в CMakeLists.txt) собираются с
# префиксом sbtest_ (не sb_!) специально, чтобы не путаться с обычными
# /sbin-командами — раскладываем в отдельную load_chain/sbin/tests/.
mkdir -p "$LOAD_CHAIN/sbin/tests"
for built in "$B"/sbtest_*; do
    name="$(basename "$built")"; name="${name#sbtest_}"
    cp "$built" "$LOAD_CHAIN/sbin/tests/${name}.elf"
done
for built in "$B"/svc_*; do
    name="$(basename "$built")"; name="${name#svc_}"
    cp "$built" "$LOAD_CHAIN/service/${name}.elf"
done
shopt -u nullglob

# wifi_driver и test_app — свои имена цели, не sb_*/svc_*, поэтому явно.
cp "$B/wifi_driver" "$LOAD_CHAIN/service/wifi.elf"
cp "$B/test_app" "$LOAD_CHAIN/bin/test_app.elf"
cp "$B/test_app" "$LOAD_CHAIN/root/test_app.elf"

# /etc/init.conf, /etc/auto_restart.conf — по просьбе пользователя
# (2026-08-16) больше НЕ сборочные артефакты и НЕ подписываются (см.
# load_text_config_from_disk() в main.cpp) — простой текст, редактируется
# ПРЯМО на устройстве (touch/echo>файл, в будущем — полноценный редактор).
# Копий .src больше нет — load_chain/etc/{init.conf,auto_restart.conf}
# сами по себе каноничны, этот скрипт их не трогает вообще.

# --- 5. Подпись — просто список ПАПОК, без знания конкретных имён файлов.
# Подписывает КАЖДЫЙ .elf, что найдёт внутри, на месте. Новый .elf в любой
# из перечисленных папок подхватится сам — редактировать этот список нужно,
# только если появится СОВСЕМ НОВАЯ папка (не новый файл в существующей). ---
echo "[5/6] Подписываю все .elf в load_chain/{sbin,sbin/tests,service,bin,root} (/etc — простой текст, не подписывается) ..."

SIGN_DIRS=("$LOAD_CHAIN/sbin" "$LOAD_CHAIN/sbin/tests" "$LOAD_CHAIN/service" "$LOAD_CHAIN/bin" "$LOAD_CHAIN/root")
for dir in "${SIGN_DIRS[@]}"; do
    for f in "$dir"/*.elf; do
        [ -e "$f" ] || continue
        tmp="$(mktemp)"
        "$SIGN_ELF" sign "$SIGN_KEY" "$f" "$tmp" && mv "$tmp" "$f" && chmod 755 "$f"
    done
done

# --- 6. FIT-подпись загрузочного образа (Фаза 13, RSA — отдельный механизм
# от Ed25519 выше, проверяется САМИМ U-Boot, не rootserver'ом). Ключ здесь
# только ПОДПИСЫВАЕТ — публичная половина уже зашита в load_chain/u-boot.bin
# при его отдельной (не автоматизированной этим скриптом) пересборке, см.
# INSTRUCTIONS.md. Если поменяли .fit-signing-key — u-boot.bin нужно
# пересобрать и передать заново, иначе новый boot.itb не пройдёт проверку. ---
echo "[6/6] Подписываю загрузочный образ (FIT/RSA) -> load_chain/boot.itb ..."

if [ -z "$MKIMAGE" ]; then
    echo "!!! mkimage не найден (пакет u-boot-tools) — пропускаю FIT-подпись."
elif [ ! -d "$FIT_KEY_DIR" ]; then
    echo "!!! Ключ подписи загрузочного образа не найден: $FIT_KEY_DIR"
    echo "!!! Сгенерировать вручную (см. INSTRUCTIONS.md, раздел про Фазу 13):"
    echo "!!!   mkdir -p $FIT_KEY_DIR"
    echo "!!!   openssl genpkey -algorithm RSA -out $FIT_KEY_DIR/dev.key -pkeyopt rsa_keygen_bits:2048 -pkeyopt rsa_keygen_pubexp:65537"
    echo "!!!   openssl req -batch -new -x509 -key $FIT_KEY_DIR/dev.key -out $FIT_KEY_DIR/dev.crt -days 36500 -subj /CN=psych-ward-os-boot-signing"
    echo "!!! Затем u-boot.bin нужно пересобрать с этим ключом (см. INSTRUCTIONS.md) — пропускаю FIT-подпись."
else
    its_tmp="$(mktemp --suffix=.its)"
    itb_tmp="$(mktemp --suffix=.itb)"
    sed "s|@@IMAGE_PATH@@|$BUILD_DIR/images/sel4test-driver-image-arm-bcm2711|" \
        "$BOOT_ITS_TEMPLATE" > "$its_tmp"
    "$MKIMAGE" -f "$its_tmp" -k "$FIT_KEY_DIR" -r "$itb_tmp"
    mv "$itb_tmp" "$LOAD_CHAIN/boot.itb"
    rm -f "$its_tmp"
fi

echo
echo "=== Готово. Прошивайте: rt/flash.sh (обычно -br). ==="
