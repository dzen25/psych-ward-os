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

echo "=== Psych Ward OS: сборка + подпись ==="

# --- 1. sign_elf: пересобрать, если отсутствует или устарел ---
if [ ! -x "$SIGN_ELF" ] || [ "$ROOT/tools/sign_elf/sign_elf.c" -nt "$SIGN_ELF" ]; then
    echo "[1/5] Собираю tools/sign_elf/sign_elf..."
    gcc -O2 -o "$SIGN_ELF" "$ROOT/tools/sign_elf/sign_elf.c" \
        "$ROOT/tools/monocypher/src/monocypher.c" \
        "$ROOT/tools/monocypher/src/optional/monocypher-ed25519.c" \
        -I"$ROOT/tools/monocypher/src" -I"$ROOT/tools/monocypher/src/optional"
else
    echo "[1/5] sign_elf уже собран."
fi

# --- 2. Приватный ключ подписи ---
if [ ! -f "$SIGN_KEY" ]; then
    echo "[2/5] Приватный ключ подписи не найден: $SIGN_KEY"
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
    echo "[2/5] Ключ подписи найден: $SIGN_KEY"
fi

# --- 3. Сборка (ninja) ---
echo "[3/5] ninja в $BUILD_DIR ..."
if [ -f "$ROOT/.venv/bin/activate" ]; then
    # shellcheck disable=SC1091
    source "$ROOT/.venv/bin/activate"
fi
( cd "$BUILD_DIR" && ninja )

# --- 4. Раскладка сырых (ещё не подписанных) артефактов под их итоговые
# имена в load_chain/ ---
echo "[4/5] Раскладываю сырые артефакты в load_chain/ ..."

cp "$BUILD_DIR/images/sel4test-driver-image-arm-bcm2711" "$LOAD_CHAIN/"

# /sbin, /service — любой sb_<имя>/svc_<имя> из сборки -> <имя>.elf.
# Список НЕ хардкодится — что CMake реально собрал (PSYCH_SBIN_TOOLS/
# PSYCH_SERVICE_TOOLS в CMakeLists.txt), то сюда и попадёт, включая будущие
# новые команды/сервисы — этот цикл трогать не придётся.
shopt -s nullglob
for built in "$B"/sb_*; do
    name="$(basename "$built")"; name="${name#sb_}"
    cp "$built" "$LOAD_CHAIN/sbin/${name}.elf"
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

# /etc/init.conf — обычный текст, не сборочный артефакт. Редактировать
# ТОЛЬКО load_chain/etc/init.conf.src (без подписи) — ниже он подписывается
# заново при каждом запуске.
if [ -f "$LOAD_CHAIN/etc/init.conf.src" ]; then
    cp "$LOAD_CHAIN/etc/init.conf.src" "$LOAD_CHAIN/etc/init.conf"
fi

# --- 5. Подпись — просто список ПАПОК, без знания конкретных имён файлов.
# Подписывает КАЖДЫЙ .elf, что найдёт внутри, на месте. Новый .elf в любой
# из перечисленных папок подхватится сам — редактировать этот список нужно,
# только если появится СОВСЕМ НОВАЯ папка (не новый файл в существующей). ---
echo "[5/5] Подписываю все .elf в load_chain/{sbin,service,bin,root} + init.conf ..."

SIGN_DIRS=("$LOAD_CHAIN/sbin" "$LOAD_CHAIN/service" "$LOAD_CHAIN/bin" "$LOAD_CHAIN/root")
for dir in "${SIGN_DIRS[@]}"; do
    for f in "$dir"/*.elf; do
        [ -e "$f" ] || continue
        tmp="$(mktemp)"
        "$SIGN_ELF" sign "$SIGN_KEY" "$f" "$tmp" && mv "$tmp" "$f" && chmod 755 "$f"
    done
done

if [ -f "$LOAD_CHAIN/etc/init.conf" ]; then
    tmp="$(mktemp)"
    "$SIGN_ELF" sign "$SIGN_KEY" "$LOAD_CHAIN/etc/init.conf" "$tmp" && mv "$tmp" "$LOAD_CHAIN/etc/init.conf"
fi

echo
echo "=== Готово. Прошивайте: rt/flash.sh (обычно -br). ==="
