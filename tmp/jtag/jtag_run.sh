#!/bin/bash
# ВРЕМЕННЫЙ скрипт для JTAG-репродукции SError из usb_driver (Фаза 14,
# см. ROADMAP.md "Пятнадцатая попытка"). Порядок действий: ЗАПУСТИТЬ
# ЭТОТ СКРИПТ ПЕРВЫМ, и только потом подать питание на RPi4 (прошитую
# build с PLAT_XHCI_PADDR=0xfd600000 + 90с паузой в usb_driver.cpp/main()).
#
# ИСТОРИЯ ПОДХОДОВ (не повторять):
# 1) 4 параллельные GDB-сессии - ЛОМАЛИ OpenOCD конкурентным доступом к
#    общему DAP через один физический мост. usb_driver работает только
#    на cpu0 (main.cpp:1421-1424) - 4 сессии были не нужны вообще.
# 2) Одна GDB-сессия, connect + continue - RAW connect в СЛУЧАЙНЫЙ
#    момент (до постановки breakpoint'ов) один раз поймал ядро посреди
#    входа в вектор IRQ, другой раз - посреди захвата ОБЩЕГО SMP-лока
#    ядра (clh_lock_acquire, риск подвесить всю систему). Сам GDB
#    "continue" через этот мост минимум один раз "потерял" момент
#    остановки (keep_alive-предупреждение, дальше "target is running").
# 3) Ожидание через Tcl-опрос (poll+targets) - ПРОВЕРЕНО: пассивный
#    "targets" без явного действия НЕ обновляет закешированное
#    состояние (poll off), просидели бы в цикле впустую.
# 4) Подключение GDB ПОСЛЕ того, как ядро уже дошло до финального
#    idle-цикла (halt() -> idle_thread(), после SError) - воспроизводимо
#    ломает чтение регистров ("warning: Selected architecture aarch64
#    is not compatible with reported target architecture arm",
#    "Truncated register 8", "No registers"). Природа не выяснена
#    (не DAIF-маскирование - тот же набор масок стоит и во время
#    успешного захвата IRQ-вектора, где регистры читаются чисто) -
#    похоже на что-то специфичное именно для WFI/idle-состояния ЭТОГО
#    моста/OpenOCD, а не архитектурное ограничение.
#
# ТЕКУЩИЙ ПОДХОД: breakpoint на ВХОДЕ в обработчик исключения (НЕ на
# финальном halt()/idle - именно та точка, что стабильно давала чистое
# чтение регистров при случайной поимке IRQ-вектора) ставится через Tcl
# ДО того, как код может дойти до крашащего чтения. Аппаратный
# breakpoint держит ядро там СКОЛЬКО УГОДНО ДОЛГО - ядро физически не
# может пройти дальше без явного resume от отладчика, так что точное
# время подключения GDB после этого уже не имеет значения - можно
# просто подождать с запасом и подключиться один раз, без гонки.
set -u
cd /home/nikita/psych-ward-os || exit 1
TMP=tmp
JTAG_FILES=uart-wifi_JTAG/files

echo "=== [1/6] убиваю старые мост/OpenOCD, если остались ==="
pkill -f dirtyjtag_bitbang_bridge.py 2>/dev/null
pkill -f "openocd -f dirtyjtag" 2>/dev/null
sleep 1

echo "=== [2/6] поднимаю мост (DirtyJTAG <-> remote_bitbang) ==="
( cd "$JTAG_FILES" && python3 dirtyjtag_bitbang_bridge.py --port 44444 ) > "$TMP/bridge.log" 2>&1 &
sleep 2

echo "=== [3/6] жду появления платы (можно подавать питание СЕЙЧАС) ==="
echo "    OpenOCD будет пытаться найти JTAG-цепочку до ~10 минут, проверяя каждые пару секунд."
READY=0
ATTEMPT=0
while [ "$ATTEMPT" -lt 200 ]; do
  ATTEMPT=$((ATTEMPT + 1))
  ( cd "$JTAG_FILES" && openocd -f dirtyjtag_remote_bitbang.cfg -f /usr/share/openocd/scripts/board/rpi4b.cfg -c "init" -c "poll off" ) > "$TMP/openocd.log" 2>&1 &
  OPENOCD_PID=$!
  # Ждём именно порт 4444 (telnet/Tcl) - он поднимается ПОСЛЕДНИМ, после
  # всех 4 GDB-портов и Tcl-порта 6666 - к этому моменту гарантированно
  # готово вообще всё.
  for i in $(seq 1 60); do
    if grep -q "Listening on port 4444 for telnet connections" "$TMP/openocd.log" 2>/dev/null; then
      READY=1
      break
    fi
    if ! kill -0 "$OPENOCD_PID" 2>/dev/null; then
      break
    fi
    sleep 1
  done
  if [ "$READY" = "1" ]; then
    echo "    OpenOCD готов (попытка $ATTEMPT) - плата обнаружена."
    break
  fi
  kill "$OPENOCD_PID" 2>/dev/null
  wait "$OPENOCD_PID" 2>/dev/null
  if [ $((ATTEMPT % 10)) -eq 0 ]; then
    echo "    ...всё ещё жду плату (попытка $ATTEMPT, ~$((ATTEMPT * 3))с прошло)"
  fi
  sleep 2
done
if [ "$READY" != "1" ]; then
  echo "!!! Плата так и не появилась за отведённое время. См. $TMP/openocd.log. Прерываю."
  exit 1
fi

echo "=== [4/6] ставлю breakpoint'ы на ВХОД в обработчик исключения через Tcl ==="
# invalid_vector_entry/cur_el_serr/lower_el_serr - НЕ halt() - именно
# эти адреса стабильно давали чистое чтение регистров при случайной
# поимке IRQ-вектора раньше (см. коммент выше). Как только КАКОЙ-ТО из
# них сработает - ядро остановится ТАМ ЖЕ и будет ждать нас сколько
# угодно, дальше в halt()/idle оно уже не пройдёт.
{
  echo "targets bcm2711.cpu0"
  echo "bp 0xffffff8000010784 4 hw"
  echo "bp 0xffffff80000107e8 4 hw"
  echo "bp 0xffffff8000010940 4 hw"
  sleep 1
} | timeout 8 nc localhost 4444 > "$TMP/tcl_bp.log" 2>&1
BP_COUNT=$(grep -c "breakpoint set" "$TMP/tcl_bp.log" 2>/dev/null || echo 0)
echo "    breakpoint'ов взведено: $BP_COUNT/3 (см. $TMP/tcl_bp.log)"
if [ "$BP_COUNT" != "3" ]; then
  echo "!!! Не все breakpoint'ы встали - см. $TMP/tcl_bp.log. Продолжаю всё равно."
fi

echo "=== [5/6] жду с запасом, пока usb_driver дойдёт до краша (breakpoint держит ядро сколько угодно) ==="
echo "    (90с пауза в коде + запас на сам bring-up - жду 110с; спешить некуда, ядро всё равно не уйдёт дальше)"
sleep 110

echo "=== [6/6] ОДНА короткая GDB-сессия ТОЛЬКО для чтения состояния (без continue-ожидания) ==="
timeout 90 gdb-multiarch -q \
    build-rpi4/kernel/kernel.elf \
    -ex "set pagination off" \
    -ex "set confirm off" \
    -ex "set remotetimeout 60" \
    -ex "target extended-remote localhost:3333" \
    -ex "add-symbol-file build-rpi4/apps/sel4test-driver/usb_driver" \
    -ex "printf \"\\n===== СОСТОЯНИЕ НА HALT =====\\n\"" \
    -ex "info symbol \$pc" \
    -ex "p/x \$pc" \
    -ex "info registers" \
    -ex "printf \"\\n----- system regs -----\\n\"" \
    -ex "info all-registers" \
    -ex "p/x \$ESR_EL1" \
    -ex "p/x \$esr_el1" \
    -ex "p/x \$FAR_EL1" \
    -ex "p/x \$far_el1" \
    -ex "p/x \$ELR_EL1" \
    -ex "p/x \$elr_el1" \
    -ex "printf \"\\n----- bt / disas -----\\n\"" \
    -ex "bt" \
    -ex "x/8i \$pc-16" \
    -ex "continue" \
    -ex "quit" \
    > "$TMP/gdb_cpu0.log" 2>&1

echo "=== финальная подстраховка - резюмирую cpu0, если он остался halted ==="
{
  echo "targets"
  sleep 1
  echo "targets bcm2711.cpu0"
  echo "resume"
  echo "targets"
  sleep 1
} | timeout 10 nc localhost 4444 > "$TMP/final_state.log" 2>&1

echo "=== ГОТОВО. Главный лог: $TMP/gdb_cpu0.log (плюс $TMP/tcl_bp.log, $TMP/openocd.log, $TMP/bridge.log, $TMP/final_state.log) ==="
