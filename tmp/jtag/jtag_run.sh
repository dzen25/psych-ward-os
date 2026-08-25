#!/bin/bash
# АДАПТИРОВАНО 2026-08-25 для issuse.txt №15 (USB hub — намертво зависает
# железо на разных, не всегда одинаковых точках: exFAT-монтирование Шаг
# 15, обработка interrupt-эндпоинта хаба, GET_PORT_STATUS и т.д. — не
# детерминированно по номеру итерации, watchdog иногда не спасает).
#
# Цель — узнать по-настоящему, где застрял PC: в софтовом цикле (какая
# функция) или в самом MMIO-обращении (xHCI/PCIe регистр) — второе не
# лечится программно вообще, первое — вопрос ещё одного фикса.
#
# Отличия от предыдущей версии (issuse.txt №64, clonetest.elf):
#   - Раньше цель была ОДИН userspace-процесс (clonetest.elf) с известным
#     тестовым флагом. Сейчас зависает usb_driver (иногда — root/main.cpp,
#     если завис watchdog-рескью) — оба ДОЛГОЖИВУЩИЕ процессы, не
#     тестовые тулзы, поэтому вместо "искать clonetest потом shell"
#     смотрим "usb_driver потом sel4test-driver (root)" на каждом ядре.
#   - root (main.cpp) слинкован в build-rpi4/apps/sel4test-driver/
#     sel4test-driver — elfloader/rootserver ЭТО НЕ ОН (stripped обёртка
#     elfloader, без символов, проверено nm). usb_driver — отдельный ELF
#     там же.
#
# ПРЕДПОЛАГАЕТСЯ (проверить перед использованием):
#   - Плата уже прошита свежим build_and_sign.sh и загружена.
#   - Пользователь физически воспроизвёл зависание на живой плате (лог
#     подтверждает — нет новых строк в out.log) ПЕРЕД запуском этого
#     скрипта.
#   - Провода FT232H<->RPi4 разведены на carto (см. debug_of_FT232H).
set -u
cd /home/nikita/psych-ward-os || exit 1
if [ -f /tmp/ssh_agent_env.sh ]; then source /tmp/ssh_agent_env.sh > /dev/null; fi
TMP=tmp/jtag
CARTO=nikita@carto
LOCAL_ELF_DIR=build-rpi4/apps/sel4test-driver
REMOTE_CFG_DIR='~/jtag'

mkdir -p "$TMP"

echo "=== [1/4] SSH до carto + запуск OpenOCD там (FT232H, MPSSE) ==="
if ! ssh -o ConnectTimeout=5 "$CARTO" 'echo ok' > /dev/null 2>&1; then
    echo "!!! Нет SSH до carto (ключ не разблокирован в этом сеансе?). Прерываю."
    exit 1
fi

pkill -f "ssh.*-L 3333.*carto" 2>/dev/null
ssh "$CARTO" 'pkill -x openocd' 2>/dev/null
sleep 1

# Туннель ВСЕХ портов (3333-3336 gdb по ядру, 4444 telnet/Tcl, 6666 tcl) —
# держим в фоне на всё время сессии.
ssh -N -L 3333:localhost:3333 -L 3334:localhost:3334 -L 3335:localhost:3335 \
    -L 3336:localhost:3336 -L 4444:localhost:4444 -L 6666:localhost:6666 \
    "$CARTO" > "$TMP/tunnel.log" 2>&1 &
TUNNEL_PID=$!
sleep 2
if ! kill -0 "$TUNNEL_PID" 2>/dev/null; then
    echo "!!! Туннель не поднялся, см. $TMP/tunnel.log. Прерываю."
    exit 1
fi
echo "    туннель поднят (pid $TUNNEL_PID, локально удерживаем для очистки в конце)."

# OpenOCD НА carto, через -tt (форсированный pty) — иначе локальный kill
# не гарантированно убивает удалённый процесс.
ssh -tt "$CARTO" "cd $REMOTE_CFG_DIR && openocd -f ft232h-jtag.cfg -f board/rpi4b.cfg" \
    > "$TMP/openocd.log" 2>&1 &
OCD_SSH_PID=$!
sleep 3
if ! grep -q "Listening on port 3333 for gdb connections" "$TMP/openocd.log"; then
    echo "!!! OpenOCD не поднялся (нет tap found?), см. $TMP/openocd.log."
    echo "    Частая причина — цель не запитана/не прошла boot. Прерываю."
    kill "$OCD_SSH_PID" "$TUNNEL_PID" 2>/dev/null
    exit 1
fi
echo "    OpenOCD на carto готов, tap найден (см. $TMP/openocd.log)."

echo "=== [2/4] ищу, какое(ие) ядро(а) реально что-то держат ==="
{
    echo "targets"
    sleep 1
} | timeout 5 nc localhost 4444 > "$TMP/tcl_targets.log" 2>&1
cat "$TMP/tcl_targets.log"
echo "    (полный вывод также в $TMP/tcl_targets.log)"

REQUESTED_CPU="${1:-}"

echo "=== [3/4] halt + чтение состояния запрошенного/каждого ядра ==="
# ДВА отдельных прохода на ядро (сначала usb_driver, потом root/
# sel4test-driver) — оба EXEC (не PIE) на фиксированной базе, грузить оба
# ELF в одну gdb-сессию одновременно даёт неоднозначные символы по одному
# и тому же числовому адресу (тот же принцип, что раньше с clonetest/shell).
for cpu in 0 1 2 3; do
    if [ -n "$REQUESTED_CPU" ] && [ "$cpu" != "$REQUESTED_CPU" ]; then
        continue
    fi
    port=$((3333 + cpu))
    echo "--- cpu$cpu (порт $port), символы usb_driver ---"
    timeout 30 gdb-multiarch -q \
        -ex "set pagination off" \
        -ex "set confirm off" \
        -ex "set remotetimeout 20" \
        -ex "target extended-remote localhost:$port" \
        -ex "add-symbol-file $LOCAL_ELF_DIR/usb_driver" \
        -ex 'printf "\n===== cpu'"$cpu"' (символы: usb_driver) PC/символ =====\n"' \
        -ex "info symbol \$pc" \
        -ex "p/x \$pc" \
        -ex "p/x \$sp" \
        -ex 'printf "\n----- bt -----\n"' \
        -ex "bt" \
        -ex 'printf "\n----- disas вокруг pc -----\n"' \
        -ex "x/8i \$pc-16" \
        -ex "quit" \
        > "$TMP/gdb_cpu${cpu}_usb_driver.log" 2>&1
    grep -A3 "PC/символ" "$TMP/gdb_cpu${cpu}_usb_driver.log"

    echo "--- cpu$cpu (порт $port), символы root (sel4test-driver/main.cpp) ---"
    timeout 30 gdb-multiarch -q \
        -ex "set pagination off" \
        -ex "set confirm off" \
        -ex "set remotetimeout 20" \
        -ex "target extended-remote localhost:$port" \
        -ex "add-symbol-file $LOCAL_ELF_DIR/sel4test-driver" \
        -ex 'printf "\n===== cpu'"$cpu"' (символы: root) PC/символ =====\n"' \
        -ex "info symbol \$pc" \
        -ex "p/x \$pc" \
        -ex 'printf "\n----- bt -----\n"' \
        -ex "bt" \
        -ex "quit" \
        > "$TMP/gdb_cpu${cpu}_root.log" 2>&1
    grep -A3 "PC/символ" "$TMP/gdb_cpu${cpu}_root.log"
    echo "    полные логи: $TMP/gdb_cpu${cpu}_usb_driver.log, $TMP/gdb_cpu${cpu}_root.log"
done

echo "=== [4/4] ГЛАВНОЕ ПРАВИЛО ==="
echo "    Halted-ядро НЕ резюмится само по себе — ОБЯЗАТЕЛЬНО resume перед"
echo "    тем как отключаться, иначе плата замрёт навсегда независимо от"
echo "    результата теста. Резюмировать всё сразу:"
echo '    ssh '"$CARTO"' '"'"'(for c in 0 1 2 3; do echo "targets bcm2711.cpu$c"; echo resume; done; echo targets) | nc localhost 4444'"'"''
echo
echo "    Туннель (pid $TUNNEL_PID) и OpenOCD на carto оставлены работать —"
echo "    сначала резюмировать нужные ядра выше, потом (если больше не нужно):"
echo "    kill $TUNNEL_PID ; ssh $CARTO pkill -x openocd"
