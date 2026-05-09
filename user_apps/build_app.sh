#!/bin/bash

# Проверяем, передали ли имя файла
if [ -z "$1" ]; then
    echo "Usage: ./build_app.sh <source_file.c>"
    exit 1
fi

SRC_FILE=$1
# Достаем имя файла без расширения .c
BASENAME=$(basename "$SRC_FILE" .c)
OUT_FILE="bin/${BASENAME}.elf"

echo "Compiling $SRC_FILE for Psych Ward OS (AARCH64)..."

# Компилируем!
# -static: Встраиваем всё внутрь (нет динамических библиотек .so)
# -nostdlib: Отключаем стандартную библиотеку Linux (libc), так как мы на голом железе
# -fPIE -pie: Position Independent Executable (чтобы ОС могла загрузить файл по любому адресу в памяти)
# -I./include: Указываем, где искать наш psych_os.h

aarch64-linux-gnu-gcc -static -nostdlib -fPIE -pie -I./include "src/$SRC_FILE" -o "$OUT_FILE"

if [ $? -eq 0 ]; then
    echo "Success! Binary saved to: user_apps/$OUT_FILE"
else
    echo "Compilation failed!"
fi