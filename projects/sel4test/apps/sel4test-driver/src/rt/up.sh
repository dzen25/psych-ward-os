#!/bin/bash

set -euo pipefail # Более надежная обработка ошибок

# Проверяем, что скрипт запущен с sudo
if [ "$EUID" -ne 0 ]; then
  echo "Пожалуйста, запустите этот скрипт с sudo."
  exit 1
fi

# Путь к скомпилированному приложению относительно папки 'build'
BUILD_APP_PATH="apps/sel4test-driver/test_app"
# Путь к образу диска относительно папки 'build'
IMG_PATH="../projects/sel4test/apps/sel4test-driver/fat32.img"
# Имя файла на образе диска (формат 8.3 для FAT32)
TARGET_FILENAME="TEST.ELF"

echo "=> [1/5] Проверка исходных файлов..."
if [ ! -f "$BUILD_APP_PATH" ]; then
    echo "ОШИБКА: Скомпилированное приложение не найдено: $BUILD_APP_PATH" >&2
    echo "Пожалуйста, выполните 'ninja' в папке 'build' перед запуском." >&2
    exit 1
fi
if [ ! -f "$IMG_PATH" ]; then
    echo "ОШИБКА: Образ диска не найден: $IMG_PATH" >&2
    echo "Пожалуйста, создайте 'fat32.img' согласно README.md." >&2
    exit 1
fi

MOUNT_POINT=$(mktemp -d)
trap 'echo "=> [5/5] Очистка..."; umount "$MOUNT_POINT" 2>/dev/null; rm -rf "$MOUNT_POINT"' EXIT

echo "=> [2/5] Монтирование образа во временную папку: $MOUNT_POINT..."
mount -o loop "$IMG_PATH" "$MOUNT_POINT"

echo "=> [3/5] Копирование '$BUILD_APP_PATH' в '$MOUNT_POINT/$TARGET_FILENAME'..."
cp "$BUILD_APP_PATH" "$MOUNT_POINT/$TARGET_FILENAME"

echo "=> [4/5] Синхронизация кэша диска..."
sync

echo "====================================================="
echo " УСПЕХ! Образ диска обновлен и готов для QEMU."
echo "====================================================="