#!/bin/bash
# jtag_run.sh — ПЕРЕПИСАН 2026-09-07 под текущую задачу: bring-up UAS
# (USB Attached SCSI) падает на живом железе с USB Transaction Error
# (код завершения 4) по трубе СТАТУСА, при том что Command IU по трубе
# команд уходит успешно, а следом перестаёт отвечать и EP0.
#
# Прошлая версия этого файла искала, где застрял PC (issuse.txt №15,
# зависания USB-хаба). Та задача закрыта, техника "halt + info symbol"
# здесь не нужна — нужны ДАННЫЕ, а не адрес.
#
# ЧТО ИМЕННО ЧИТАЕМ И ЗАЧЕМ (два открытых вопроса, оба решаются чтением,
# а не рассуждением):
#   1. Сырая цепочка дескрипторов UAS-интерфейса из буфера control-
#      transfer'ов драйвера. Разбор по bPipeID дал НЕОБЫЧНУЮ раскладку
#      (cmd 0x04, status 0x83, data-in 0x81, data-out 0x02). Алгоритм
#      совпадает с uas_find_endpoints() ядра Linux, но проверить надо
#      байты, а не алгоритм: если 0x83 — не труба статуса, всё поведение
#      объясняется сразу.
#   2. Живой Device Context слота ПОСЛЕ Configure Endpoint. Код
#      завершения 1 означает лишь "команда принята"; реальное состояние
#      эндпоинта — dword0 биты[2:0] (0=Disabled, 1=Running, 2=Halted,
#      3=Stopped, 4=Error). Если труба статуса не Running — искать надо в
#      Configure Endpoint, а не в протоколе UAS.
#
# ПРЕДУСЛОВИЯ (скрипт проверяет их сам и внятно ругается):
#   - SSH-ключ до carto разблокирован в этом сеансе (см. §"ключ" ниже);
#   - плата ЗАГРУЖЕНА и шелл отвечает (не U-Boot — здесь мы ничего не
#     заливаем, только читаем живую систему);
#   - на плате исполняется РОВНО тот образ, что лежит в build-rpi4/.
#     Это критично и проверяется автоматически: все процессы слинкованы
#     на один и тот же 0x400000, поэтому несовпадение сборки даёт не
#     ошибку, а тихо неверные символы и мусор в дампах.
#
# КЛЮЧ. Парольная фраза в этот файл НЕ ЗАПИСАНА намеренно (файл лежит в
# репозитории). Разблокировать в своей сессии один раз:
#     eval "$(ssh-agent -s)" | tee /tmp/ssh_agent_env.sh >/dev/null
#     ssh-add ~/.ssh/id_ed25519          # спросит парольную фразу
#
# ГЛАВНОЕ ПРАВИЛО JTAG на этом стенде: остановленное ядро НЕ резюмится
# само, ни от detach в GDB, ни от убийства OpenOCD — плата замрёт навсегда.
# Поэтому здесь стоит EXIT-ловушка, которая резюмит все четыре ядра при
# ЛЮБОМ выходе, включая Ctrl+C и падение на середине.
set -u

cd /home/nikita/psych-ward-os || exit 1
[ -f /tmp/ssh_agent_env.sh ] && source /tmp/ssh_agent_env.sh >/dev/null 2>&1

TMP=tmp/jtag
CARTO="${CARTO_HOST:-nikita@carto}"
REMOTE_CFG_DIR='~/jtag'
ELF="${PSYCH_USB_ELF:-build-rpi4/apps/sel4test-driver/usb_driver}"
GDB="${GDB:-gdb-multiarch}"
CPU_PORT=3333          # cpu0: драйверы прибиты к ядру 0 (см. память проекта)
TELNET_PORT=4444
WAIT_HIT="${JTAG_WAIT_HIT:-300}"   # сколько ждать usbreset от пользователя, с

# Фиксированные виртуальные адреса приватных DMA-страниц usb_driver'а
# (h/platform.h). Их не надо искать в символах — они константы.
CTRL_BUF_VADDR=0x201138000     # + idx*4096  — буфер control-transfer'ов
DEVCTX_VADDR=0x201104000       # + slot_id*4096 — Device Context

mkdir -p "$TMP"
MODE="${1:-run}"

resume_all() {
    { for c in 0 1 2 3; do echo "targets bcm2711.cpu$c"; echo "resume"; done; echo "targets"; sleep 1; } \
        | timeout 15 nc localhost "$TELNET_PORT" 2>/dev/null | tail -8
}

cleanup() {
    local rc=$?
    echo
    echo "=== ВЫХОД: резюмирую все ядра (иначе плата замрёт навсегда) ==="
    resume_all || echo "    !!! resume через telnet не прошёл — плата может быть заморожена."
    [ -n "${OCD_SSH_PID:-}" ] && kill "$OCD_SSH_PID" 2>/dev/null
    ssh -o ConnectTimeout=5 "$CARTO" 'pkill -x openocd' 2>/dev/null
    [ -n "${TUNNEL_PID:-}" ] && kill "$TUNNEL_PID" 2>/dev/null
    echo "=== OpenOCD и туннель остановлены. Код выхода $rc ==="
    exit $rc
}

if [ "$MODE" = "--resume" ]; then
    # Аварийный режим: НИЧЕГО не поднимаем, просто оживить зависшую плату
    # через уже работающий туннель.
    echo "=== Аварийный resume всех ядер через localhost:$TELNET_PORT ==="
    resume_all
    exit 0
fi

echo "=== [1/6] Предполётная проверка ==="
[ -f "$ELF" ] || { echo "!!! Нет $ELF — сначала ./build_and_sign.sh. Прерываю."; exit 1; }
command -v "$GDB" >/dev/null || { echo "!!! Нет $GDB. Прерываю."; exit 1; }
if ! ssh -o BatchMode=yes -o ConnectTimeout=5 "$CARTO" 'echo ok' >/dev/null 2>&1; then
    echo "!!! Нет SSH до carto — ключ не разблокирован в этом сеансе."
    echo "    eval \"\$(ssh-agent -s)\" | tee /tmp/ssh_agent_env.sh >/dev/null"
    echo "    ssh-add ~/.ssh/id_ed25519"
    exit 1
fi
# Эталонные байты точки входа usb_driver'а ИЗ ФАЙЛА — ими же потом
# сверяем, что на плате тот самый образ И что мы в нужном процессе.
ENTRY=$("$GDB" -batch -ex 'printf "0x%x\n", &_start' "$ELF" 2>/dev/null | tail -1)
[ -n "$ENTRY" ] || ENTRY=$(readelf -h "$ELF" | awk '/Entry point/{print $4}')
EXPECT=$("$GDB" -batch -ex "x/2xw $ENTRY" "$ELF" 2>/dev/null | tail -1 | tr -s ' ' | cut -d: -f2)
echo "    usb_driver: $ELF"
echo "    точка входа $ENTRY, эталонные слова из файла:$EXPECT"

echo "=== [2/6] SSH-туннель + OpenOCD на carto ==="
pkill -f "ssh.*-L 3333.*carto" 2>/dev/null
ssh -o ConnectTimeout=5 "$CARTO" 'pkill -x openocd' 2>/dev/null
sleep 1
ssh -N -L 3333:localhost:3333 -L 3334:localhost:3334 -L 3335:localhost:3335 \
    -L 3336:localhost:3336 -L 4444:localhost:4444 -L 6666:localhost:6666 \
    "$CARTO" > "$TMP/tunnel.log" 2>&1 &
TUNNEL_PID=$!
sleep 2
kill -0 "$TUNNEL_PID" 2>/dev/null || { echo "!!! Туннель не поднялся, см. $TMP/tunnel.log."; exit 1; }
echo "    туннель поднят (pid $TUNNEL_PID)."

trap cleanup EXIT INT TERM

ssh -tt "$CARTO" "cd $REMOTE_CFG_DIR && openocd -f ft232h-jtag.cfg -f board/rpi4b.cfg" \
    > "$TMP/openocd.log" 2>&1 &
OCD_SSH_PID=$!
for i in $(seq 1 20); do
    grep -q "Listening on port 3333 for gdb connections" "$TMP/openocd.log" && break
    sleep 1
done
grep -q "Listening on port 3333 for gdb connections" "$TMP/openocd.log" || {
    echo "!!! OpenOCD не поднялся, см. $TMP/openocd.log. Частая причина — цель не запитана."
    exit 1
}
echo "    OpenOCD готов (см. $TMP/openocd.log)."

echo "=== [3/6] Немедленный resume: OpenOCD останавливает cpu0 на старте ==="
# Без этого шелл платы заморожен и пользователь физически не сможет
# набрать usbreset, которого мы ждём ниже.
resume_all > "$TMP/resume_after_start.log" 2>&1
echo "    cpu0 отпущен, шелл платы снова живой."

echo "=== [4/6] Сверка: на плате действительно ТОТ образ? ==="
cat > "$TMP/check.gdb" <<EOF
set pagination off
set confirm off
set remotetimeout 20
target extended-remote localhost:$CPU_PORT
printf "\n----- слова на плате по адресу точки входа usb_driver -----\n"
x/2xw $ENTRY
detach
EOF
timeout 60 "$GDB" -q -batch -x "$TMP/check.gdb" > "$TMP/check.log" 2>&1
GOT=$(grep -A1 'слова на плате' "$TMP/check.log" | tail -1 | tr -s ' ' | cut -d: -f2)
echo "    из файла:  $EXPECT"
echo "    с платы:   $GOT"
if [ "$EXPECT" = "$GOT" ]; then
    echo "    СОВПАЛО — по адресу 0x400000 сейчас код usb_driver'а той же сборки."
else
    echo "    НЕ СОВПАЛО. Два возможных объяснения, оба важные:"
    echo "      а) на плате СТАРЫЙ образ — тогда любые символы отсюда врут,"
    echo "         дампы будут мусором; сначала залить свежий (jtagload.sh)."
    echo "      б) по 0x400000 сейчас просто ДРУГОЙ процесс (root/shell) —"
    echo "         это нормально, все процессы слинкованы на один адрес."
    echo "    Отличить (а) от (б) нельзя одним снимком, поэтому ниже сверка"
    echo "    повторяется в момент срабатывания точки останова, когда мы"
    echo "    заведомо внутри usb_driver'а."
fi

# GDB-сессия выше отсоединилась — у OpenOCD detach оставляет цель
# остановленной, а нам нужен живой шелл, чтобы пользователь набрал usbreset.
resume_all > "$TMP/resume_after_check.log" 2>&1

echo "=== [5/6] Точка останова на пути UAS; жду usbreset с платы ==="
echo
echo "    >>> НАБЕРИТЕ НА ПЛАТЕ:  usbreset"
echo "    (перечисление пойдёт заново и упрётся в точку останова)"
echo
cat > "$TMP/uas.gdb" <<EOF
set pagination off
set confirm off
set remotetimeout 20
target extended-remote localhost:$CPU_PORT
add-symbol-file $ELF

# Аппаратная точка останова: ОЗУ цели через JTAG-софтбрейк трогать не
# хотим, да и страницы кода отображены только для своего процесса.
hbreak step_uas_setup
continue

printf "\n=============== ПОПАЛИ В step_uas_setup ===============\n"
printf "\n----- сверка образа: слова по точке входа (должны совпасть с файлом) -----\n"
x/2xw $ENTRY
printf "\n----- аргументы -----\n"
p idx
p/x slot_id
p port
p/x port_speed

printf "\n----- разобранные трубы UAS (то, во что мы поверили) -----\n"
p/x g_usb_devices[idx].uas

printf "\n----- СЫРОЙ Configuration Descriptor (первые 256 Б буфера control) -----\n"
printf "  (ищем: 04=Interface, 05=Endpoint, 24=Pipe Usage; у Pipe Usage байт+2 = bPipeID)\n"
x/256xb ($CTRL_BUF_VADDR + idx*4096)

printf "\n----- xHCI: базовые указатели и USBSTS -----\n"
p/x g_op_base
x/4xw g_op_base

printf "\n=============== доходим до первой команды по UAS ===============\n"
hbreak uas_scsi_command
continue

printf "\n----- ЖИВОЙ Device Context слота ПОСЛЕ Configure Endpoint -----\n"
printf "  (контекст N = DCI N; dword0 биты[2:0]: 0=Disabled 1=Running 2=Halted 3=Stopped 4=Error)\n"
p/x g_ctx_size
x/160xw ($DEVCTX_VADDR + g_usb_devices[idx].slot_id*4096)

printf "\n----- DCI, которыми пользуется драйвер -----\n"
p/x g_usb_devices[idx].uas_cmd_dci
p/x g_usb_devices[idx].uas_status_dci
p/x g_usb_devices[idx].bulk_in_dci
p/x g_usb_devices[idx].bulk_out_dci

printf "\n----- снимаю точки останова -----\n"
delete
detach
EOF
timeout "$WAIT_HIT" "$GDB" -q -batch -x "$TMP/uas.gdb" 2>&1 | tee "$TMP/uas_dump.log"
RC=${PIPESTATUS[0]}

echo
echo "=== [6/6] Итог ==="
if [ "$RC" -eq 124 ]; then
    echo "    Точка останова так и не сработала за ${WAIT_HIT}с."
    echo "    Вероятные причины: usbreset не набран; на плате старый образ"
    echo "    (см. сверку в шаге 4); перечисление не доходит до UAS."
else
    echo "    Дамп полностью сохранён: $TMP/uas_dump.log"
fi
echo "    Ловушка выхода ниже резюмит все ядра в любом случае."
