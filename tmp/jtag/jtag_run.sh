#!/bin/bash
# МОДЕРНИЗИРОВАНО 2026-08-23 для issuse.txt №64 (SYS_CLONE/SYS_THREAD_EXIT
# никогда не доставляет вызов до root). Старая версия этого скрипта (см.
# git-историю/situation.txt) была написана под мост RP2040/DirtyJTAG
# (remote_bitbang, побитовый, медленный) и хрупкие hex-адреса точек
# останова — оба обстоятельства изменились:
#   - JTAG теперь идёт через FT232H (MPSSE, аппаратный, быстрый) с
#     отдельного хоста carto (см. uart_wifi_JTAG/debug_of_FT232H/) —
#     никакого моста/bitbang-Python не нужно, OpenOCD говорит с чипом
#     напрямую. Все JTAG-операции — ТОЛЬКО там, через ssh.
#   - Цель теперь НЕ мимолётный крах (SError -> WFI/idle, где чтение
#     регистров ломалось) — clonetest.elf (src/tests/clonetest.cpp)
#     нарочно виснет в while(1)/seL4_Yield() НАВСЕГДА, если баг
#     воспроизвёлся, и родитель, и (если дошёл) дочерний поток. Гонки со
#     временем нет вообще — можно подключаться когда угодно после того,
#     как лог (out.log) подтвердит зависание. Поэтому вместо
#     Tcl-breakpoint-ДО-краха здесь просто halt + инспекция по имени
#     символа (add-symbol-file), без хардкода hex-адресов.
#
# ПРЕДПОЛАГАЕТСЯ (проверить перед использованием):
#   - Плата уже прошита свежим build_and_sign.sh (включает clonetest.elf
#     в load_chain/RPI/sbin/tests/) и загружена.
#   - На плате УЖЕ выполнено `exec /sbin/tests/clonetest.elf` вручную
#     (физическая клавиатура/консоль пользователя — этот скрипт НЕ умеет
#     сам вводить команды в шелл, JTAG и обычная UART-консоль это разные
#     каналы) и лог подтвердил зависание (call_returned не стал 1 за 20с).
#   - Провода FT232H<->RPi4 разведены на carto (см. debug_of_FT232H).
#
# Использование: ./jtag_run.sh [cpu]
#   cpu — 0-3, какое ядро инспектировать (по умолчанию перебирает все 4 и
#         сам находит "интересные" — see [2/4] ниже). Обычно шелл/потоки
#         без явного SYS_SET_AFFINITY остаются на дефолтном ядре, но
#         точно не гадаем, проверяем все.
set -u
cd /home/nikita/psych-ward-os || exit 1
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
# не гарантированно убивает удалённый процесс (проверено эмпирически на
# этой же машине при настройке debug_of_FT232H, см. гайд).
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
# targets без предварительного poll/halt всё равно покажет закешированное
# состояние достаточно свежим (в отличие от старого DirtyJTAG-моста с
# ЯВНО отключённым poll — здесь MPSSE достаточно быстрый, poll по
# умолчанию включён).
{
    echo "targets"
    sleep 1
} | timeout 5 nc localhost 4444 > "$TMP/tcl_targets.log" 2>&1
cat "$TMP/tcl_targets.log"
echo "    (полный вывод также в $TMP/tcl_targets.log — halted-ядра смотреть руками,"
echo "     не гадаем автоматически, какое из них 'интересное')"

REQUESTED_CPU="${1:-}"

echo "=== [3/4] halt + чтение состояния запрошенного/каждого ядра ==="
# ВАЖНО: shell и КАЖДЫЙ /sbin/tests-бинарник (включая clonetest) — все
# EXEC (не PIE), линкуются на ОДИН И ТОТ ЖЕ фиксированный базовый адрес
# 0x400000 (проверено: readelf -h/-l обоих ELF, entry 0x4001d0 у обоих) —
# на живой системе это не конфликтует (разные VSpace у разных процессов),
# но грузить ОБА ELF в ОДНУ gdb-сессию одновременно даёт неоднозначные/
# перекрывающиеся символы по одному и тому же числовому адресу. Поэтому —
# ДВА отдельных прохода на ядро (сначала clonetest, потом shell), не один
# с обоими add-symbol-file сразу.
for cpu in 0 1 2 3; do
    if [ -n "$REQUESTED_CPU" ] && [ "$cpu" != "$REQUESTED_CPU" ]; then
        continue
    fi
    port=$((3333 + cpu))
    echo "--- cpu$cpu (порт $port), символы clonetest ---"
    timeout 30 gdb-multiarch -q \
        -ex "set pagination off" \
        -ex "set confirm off" \
        -ex "set remotetimeout 20" \
        -ex "target extended-remote localhost:$port" \
        -ex "add-symbol-file $LOCAL_ELF_DIR/sbtest_clonetest" \
        -ex 'printf "\n===== cpu'"$cpu"' (символы: clonetest) PC/символ =====\n"' \
        -ex "info symbol \$pc" \
        -ex "p/x \$pc" \
        -ex "p/x \$sp" \
        -ex 'printf "\n----- bt -----\n"' \
        -ex "bt" \
        -ex 'printf "\n----- флаги clonetest (валидны, только если pc реально внутри этого ELF) -----\n"' \
        -ex "p g_thread_reached_entry" \
        -ex "p g_thread_call_returned" \
        -ex "p/x g_thread_result" \
        -ex 'printf "\n----- disas вокруг pc -----\n"' \
        -ex "x/8i \$pc-16" \
        -ex "quit" \
        > "$TMP/gdb_cpu${cpu}_clonetest.log" 2>&1
    grep -A3 "PC/символ\|флаги clonetest" "$TMP/gdb_cpu${cpu}_clonetest.log"

    echo "--- cpu$cpu (порт $port), символы shell (на случай если pc не в clonetest) ---"
    timeout 30 gdb-multiarch -q \
        -ex "set pagination off" \
        -ex "set confirm off" \
        -ex "set remotetimeout 20" \
        -ex "target extended-remote localhost:$port" \
        -ex "add-symbol-file $LOCAL_ELF_DIR/shell" \
        -ex 'printf "\n===== cpu'"$cpu"' (символы: shell) PC/символ =====\n"' \
        -ex "info symbol \$pc" \
        -ex 'printf "\n----- bt -----\n"' \
        -ex "bt" \
        -ex "quit" \
        > "$TMP/gdb_cpu${cpu}_shell.log" 2>&1
    grep -A3 "PC/символ" "$TMP/gdb_cpu${cpu}_shell.log"
    echo "    полные логи: $TMP/gdb_cpu${cpu}_clonetest.log, $TMP/gdb_cpu${cpu}_shell.log"
done

echo "=== [4/4] ГЛАВНОЕ ПРАВИЛО (см. debug_of_RP2040/JTAG-GDB-cheatsheet.md) ==="
echo "    Halted-ядро НЕ резюмится само по себе — если инспекция выше делала"
echo "    halt (а не только read-only через уже-established gdb-сессию с"
echo "    'continue' в конце), ОБЯЗАТЕЛЬНО resume перед тем как отключаться,"
echo "    иначе шелл/плата замрёт навсегда независимо от результата теста."
echo "    Резюмировать всё сразу:"
echo '    ssh '"$CARTO"' '"'"'(for c in 0 1 2 3; do echo "targets bcm2711.cpu$c"; echo resume; done; echo targets) | nc localhost 4444'"'"''
echo
echo "    Туннель (pid $TUNNEL_PID) и OpenOCD на carto оставлены работать —"
echo "    сначала резюмировать нужные ядра выше, потом (если больше не нужно):"
echo "    kill $TUNNEL_PID ; ssh $CARTO pkill -x openocd"
