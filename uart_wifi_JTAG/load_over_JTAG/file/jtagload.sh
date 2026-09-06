#!/bin/bash
# jtagload.sh — залить свежесобранный образ Psych Ward OS прямо в ОЗУ RPi4
# через JTAG и запустить, минуя перепрошивку SD-карты.
#
# Запускается НА СБОРОЧНОМ СЕРВЕРЕ (там, где лежит build-rpi4/). Физический
# FT232H висит на carto — см. ../../debug_of_FT232H/FT232H-jtag-guide.md.
# Предполагается, что уже запущены (обе — задачи VSCode или вручную):
#   1. SSH-туннель carto -> сюда (порты 3333-3336/4444/6666)
#   2. OpenOCD на carto (openocd -f ft232h-jtag.cfg -f board/rpi4b.cfg)
#
# Полная методичка, включая ОБЯЗАТЕЛЬНОЕ предусловие "плата стоит в
# приглашении U-Boot" — ../JTAG-load-guide.md.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="${PSYCH_ROOT:-$(cd "$HERE/../../.." && pwd)}"

IMAGE="${PSYCH_IMAGE:-$ROOT/build-rpi4/images/sel4test-driver-image-arm-bcm2711}"
ELFLOADER="${PSYCH_ELFLOADER:-$ROOT/build-rpi4/elfloader/elfloader}"

CARTO="${CARTO_HOST:-nikita@carto}"
CARTO_DIR="${CARTO_DIR:-jtag}"            # относительно $HOME на carto
REMOTE_IMAGE="${CARTO_DIR}/psych-ward-os.bin"

# Конфиги OpenOCD, которые обязаны лежать на carto (OpenOCD запускается там
# и читает их СВОЕЙ файловой системой). Каноничные копии — здесь, в репозитории;
# на carto они только доставляются, править их там нельзя, затрёт следующий
# запуск. Список расширяется одной строкой.
CARTO_CFGS=(
    "$HERE/../../debug_of_FT232H/file/ft232h-jtag.cfg"   # отладка (F5 / VSCode)
    "$HERE/ft232h-jtag-load.cfg"                         # загрузка в ОЗУ (psych_boot)
)

OCD_HOST="${OCD_HOST:-localhost}"
OCD_PORT="${OCD_PORT:-4444}"
TARGET="${JTAG_TARGET:-bcm2711.cpu0}"

# Адрес совпадает с IMAGE_START_ADDR сборки и с load/entry в
# tools/boot_fit/boot.its.template — менять только вместе с ними.
LOAD_ADDR="${LOAD_ADDR:-0x10000000}"

OCD="$HERE/jtag_ocd.py"
MARKER="JTAGLOAD-DONE"
WAIT="${JTAG_WAIT:-600}"

DO_BUILD=0
DO_SCP=1
DO_LOAD=1
DO_RUN=1

usage() {
    cat <<'USAGE'
jtagload.sh [опции]

  -b, --build      сначала выполнить ./build_and_sign.sh
  -n, --no-scp     ничего не копировать на carto (конфиги и образ там актуальны)
      --load-only  залить в ОЗУ, но НЕ запускать (оставить cpu0 halted)
      --run-only   не заливать, только выставить PC и запустить
      --resume     аварийное спасение: просто resume зависшего cpu0 и выход
      --halt       остановить cpu0 и выйти
      --provision  только разложить конфиги OpenOCD на carto и выйти
  -h, --help       эта справка

Переменные окружения: PSYCH_ROOT, PSYCH_IMAGE, CARTO_HOST, CARTO_DIR,
OCD_HOST, OCD_PORT, JTAG_TARGET, LOAD_ADDR, JTAG_WAIT.
USAGE
}

ocd() { python3 "$OCD" --host "$OCD_HOST" --port "$OCD_PORT" --timeout "$WAIT" "$@"; }

# Разложить конфиги OpenOCD на carto. Вызывается при каждом запуске — файлы
# по 2-3 КБ, на фоне 1.9 МБ образа не заметны, зато carto никогда не отстаёт
# от репозитория. Раньше это был ручной шаг ("scp ... один раз, дальше не
# нужен"), и ровно он и забывался после правки конфига здесь.
provision_carto() {
    local cfg missing=0
    for cfg in "${CARTO_CFGS[@]}"; do
        [ -f "$cfg" ] || { echo "jtagload.sh: нет файла $cfg" >&2; missing=1; }
    done
    [ "$missing" = 0 ] || return 1
    ssh "$CARTO" "mkdir -p ~/$CARTO_DIR"
    scp -q "${CARTO_CFGS[@]}" "$CARTO:$CARTO_DIR/"
    for cfg in "${CARTO_CFGS[@]}"; do
        echo "[jtagload]     -> ~/$CARTO_DIR/$(basename "$cfg")"
    done
}

# Самая неприятная возможная концовка — упасть между halt и resume: плата
# останется физически замороженной, и человек будет искать причину в ОС, а
# не в оборванном скрипте. Поэтому о таком выходе говорим прямым текстом.
HALTED=0
on_exit() {
    local rc=$?
    if [ "$rc" != 0 ] && [ "$HALTED" = 1 ]; then
        echo
        echo "[jtagload] ОШИБКА (код $rc) уже ПОСЛЕ halt — cpu0 остался остановлен!"
        echo "[jtagload] Плата сейчас физически заморожена. Резюмнуть:"
        echo "[jtagload]     $0 --resume"
    fi
    return $rc
}
trap on_exit EXIT

while [ $# -gt 0 ]; do
    case "$1" in
        -b|--build)  DO_BUILD=1 ;;
        -n|--no-scp) DO_SCP=0 ;;
        --load-only) DO_RUN=0 ;;
        --run-only)  DO_SCP=0; DO_LOAD=0 ;;
        --resume)
            echo "[jtagload] Аварийный resume $TARGET ..."
            ocd --marker "$MARKER" \
                -c "targets $TARGET" -c "resume" -c "targets" -c "echo $MARKER"
            exit $?
            ;;
        --halt)
            echo "[jtagload] Останавливаю $TARGET ..."
            ocd --marker "$MARKER" \
                -c "targets $TARGET" -c "halt" -c "targets" -c "echo $MARKER"
            exit $?
            ;;
        --provision)
            echo "[jtagload] Раскладываю конфиги OpenOCD на $CARTO:~/$CARTO_DIR/ ..."
            provision_carto
            echo "[jtagload] Готово. Запуск OpenOCD на carto:"
            echo "[jtagload]     ssh $CARTO 'cd ~/$CARTO_DIR && openocd -f ft232h-jtag-load.cfg'"
            exit 0
            ;;
        -h|--help) usage; exit 0 ;;
        *) echo "jtagload.sh: неизвестный аргумент '$1'" >&2; usage; exit 2 ;;
    esac
    shift
done

# --- 1. Сборка (опционально) ---
if [ "$DO_BUILD" = 1 ]; then
    echo "[jtagload] 1/4 Сборка: $ROOT/build_and_sign.sh"
    ( cd "$ROOT" && ./build_and_sign.sh )
else
    echo "[jtagload] 1/4 Сборка пропущена (-b, чтобы собрать)."
fi

[ -f "$IMAGE" ] || { echo "jtagload.sh: образ не найден: $IMAGE" >&2; exit 1; }

# Страховка от самой тихой возможной ошибки этого пути: залить в ОЗУ один
# образ, а символы в GDB подгрузить от другого. Плоский образ — это ровно
# `objcopy -O binary` от elfloader'а (проверено `cmp`, см. ../JTAG-load-guide.md,
# §2), так что расхождение по времени сборки означает рассинхрон дерева.
if [ -f "$ELFLOADER" ] && [ "$ELFLOADER" -nt "$IMAGE" ]; then
    echo "[jtagload] ВНИМАНИЕ: elfloader новее образа — образ, похоже, не пересобран."
fi
echo "[jtagload]     образ: $IMAGE ($(stat -c %s "$IMAGE") байт, $(date -r "$IMAGE" '+%F %T'))"

# --- 2. Доставка на carto: сначала конфиги OpenOCD, затем сам образ ---
if [ "$DO_SCP" = 1 ]; then
    echo "[jtagload] 2/4 Раскладываю файлы на $CARTO:~/$CARTO_DIR/ ..."
    provision_carto
    scp -q "$IMAGE" "$CARTO:$REMOTE_IMAGE"
    echo "[jtagload]     -> ~/$REMOTE_IMAGE (образ)"
else
    echo "[jtagload] 2/4 Копирование пропущено (-n)."
fi

# --- 3. Заливка в ОЗУ ---
if [ "$DO_LOAD" = 1 ]; then
    # OpenOCD читает файл СВОЕЙ файловой системой (он живёт на carto),
    # поэтому путь ниже — абсолютный путь НА CARTO, не здешний.
    REMOTE_ABS="$(ssh "$CARTO" "cd ~/$CARTO_DIR && pwd")/psych-ward-os.bin"
    echo "[jtagload] 3/4 Заливаю в ОЗУ по $LOAD_ADDR (это десятки секунд, MPSSE 4 МГц) ..."
    HALTED=1
    ocd --marker "$MARKER" \
        -c "targets $TARGET" \
        -c "halt" \
        -c "load_image $REMOTE_ABS $LOAD_ADDR bin" \
        -c "echo $MARKER"
else
    echo "[jtagload] 3/4 Заливка пропущена (--run-only)."
fi

# --- 4. Запуск ---
if [ "$DO_RUN" = 1 ]; then
    echo "[jtagload] 4/4 Ставлю PC=$LOAD_ADDR и запускаю ..."
    # x0/x1 — аргументы, которые U-Boot передал бы в do_bootm_standalone
    # (argc/argv). elfloader собран как CONFIG_IMAGE_BINARY с вшитым DTB
    # (CONFIG_ELFLOADER_INCLUDE_DTB=1) и их не использует, но обнуляем ради
    # воспроизводимости состояния между запусками.
    ocd --marker "$MARKER" \
        -c "targets $TARGET" \
        -c "reg pc $LOAD_ADDR" \
        -c "reg x0 0" \
        -c "reg x1 0" \
        -c "resume" \
        -c "targets" \
        -c "echo $MARKER"
    HALTED=0
    echo
    echo "[jtagload] Готово. Смотрите UART-консоль — там должен пойти вывод elfloader'а."
    echo "[jtagload] ВАЖНО: /sbin, /sbin/tests, /service (в т.ч. wifi.elf), /bin, /etc"
    echo "[jtagload]        по-прежнему читаются с SD-карты. Этот путь обновляет только"
    echo "[jtagload]        ядро + rootserver + uart/timer/shell/blk/net/usb (см. §5 методички)."
else
    echo "[jtagload] 4/4 Запуск пропущен (--load-only). cpu0 ОСТАЁТСЯ halted!"
    echo "[jtagload]     Не забудьте './jtagload.sh --resume' или запуск вручную —"
    echo "[jtagload]     иначе плата останется физически замороженной."
fi
