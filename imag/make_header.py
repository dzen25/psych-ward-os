import struct

with open('psych_kernel.bin', 'rb') as f:
    kernel = f.read()

with open('extracted/vince_pure.dtb', 'rb') as f:
    dtb = f.read()

# Выравниваем ядро по границе 2048 байт (некоторые загрузчики Qualcomm это жестко требуют)
padding_needed = (2048 - (len(kernel) % 2048)) % 2048
kernel += b'\x00' * padding_needed

# Размер образа = 64 байта (заголовок) + размер выровненного ядра
image_size = 64 + len(kernel)

# --- Создаем 64-байтный заголовок Linux ARM64 ---
code0 = b'\x10\x00\x00\x14'         # Инструкция: b 0x40 (Прыжок на 64 байта вперед, прямо в твой код)
code1 = b'\x00\x00\x00\x00'         # Пусто
text_offset = struct.pack('<Q', 0x8000)      # Смещение 0x8000
image_size_bytes = struct.pack('<Q', image_size) # Размер ядра для загрузчика
flags = struct.pack('<Q', 8)                 # Флаги (little endian)
reserved = b'\x00' * 24                      # Резерв
magic = b'ARM\x64'                           # Магическая подпись Linux
res5 = b'\x00\x00\x00\x00'                   # Резерв

header = code0 + code1 + text_offset + image_size_bytes + flags + reserved + magic + res5

# Сшиваем: Заголовок + Ядро + DTB
with open('kernel_with_dtb.bin', 'wb') as f:
    f.write(header + kernel + dtb)

print(f"Фейковый заголовок создан! Размер ядра: {len(kernel)}, Смещение DTB: {image_size}")
