#!/bin/bash

set -euo pipefail # Более надежная обработка ошибок

# Проверяем, что скрипт запущен с sudo
if [ "$EUID" -ne 0 ]; then
  echo "Пожалуйста, запустите этот скрипт с sudo."
  exit 1
fi

#Для начала стороняя сборка исходников
#Их расположение
PATH_libpsych="../projects/sel4test/apps/sel4test-driver/src/libpsych.cpp"
PATH_dyn_test="../projects/sel4test/apps/sel4test-driver/src/dyn_test.cpp"

TARGET_so="libpsych.so"
TARGET_elf="dyn_test.elf"

# --- ШАГ 1: Компиляция ---
echo "=> [1/4] Компиляция разделяемой библиотеки $TARGET_so..."
aarch64-linux-gnu-g++ -fPIC -shared -nostdlib -ffreestanding -O2 -o "$TARGET_so" "$PATH_libpsych"
echo "    $TARGET_so успешно скомпилирована."

echo "=> [2/4] Компиляция исполняемого файла $TARGET_elf..."
aarch64-linux-gnu-g++ -nostdlib -ffreestanding -O2 -o "$TARGET_elf" "$PATH_dyn_test" -L. -lpsych -Wl,-rpath,.
echo "    $TARGET_elf успешно скомпилирован."

# Путь к образу диска относительно папки 'build'
IMG_PATH="../projects/sel4test/apps/sel4test-driver/fat32.img"

# Проверка наличия образа диска
if [ ! -f "$IMG_PATH" ]; then
    echo "ОШИБКА: Образ диска не найден: $IMG_PATH" >&2
    echo "Пожалуйста, создайте 'fat32.img' согласно README.md." >&2
    exit 1
fi

# --- ШАГ 2: Монтирование и копирование ---
MOUNT_POINT=$(mktemp -d)
# trap для автоматической очистки при выходе
trap 'echo "=> [4/4] Очистка..."; umount "$MOUNT_POINT" 2>/dev/null; rm -rf "$MOUNT_POINT"' EXIT

echo "=> [3/4] Монтирование образа и копирование файлов..."
mount -o loop "$IMG_PATH" "$MOUNT_POINT"

echo "    -> Копирование '$TARGET_elf' в '$MOUNT_POINT/$TARGET_elf'..."
cp "$TARGET_elf" "$MOUNT_POINT/$TARGET_elf"

echo "    -> Копирование '$TARGET_so' в '$MOUNT_POINT/$TARGET_so'..."
cp "$TARGET_so" "$MOUNT_POINT/$TARGET_so"

echo "====================================================="
echo " УСПЕХ! Файлы скомпилированы, образ диска обновлен."
echo "====================================================="