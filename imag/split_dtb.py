with open('extracted/kernel', 'rb') as f:
    data = f.read()

# Ищем магическую подпись Qualcomm (QCDT)
idx = data.find(b'QCDT')
if idx == -1:
    # Если QCDT нет, ищем стандартный Flat Device Tree
    idx = data.find(b'\xd0\x0d\xfe\xed')

if idx != -1:
    with open('extracted/vince_pure.dtb', 'wb') as out:
        out.write(data[idx:])
    print(f"УСПЕХ! DTB найден и отрезан по смещению {idx} байт.")
else:
    print("ОШИБКА: DTB не найден внутри ядра.")
