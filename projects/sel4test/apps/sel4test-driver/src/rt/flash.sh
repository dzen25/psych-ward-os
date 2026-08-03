#!/bin/bash
#
# flash_sync.sh — синхронизация файлов с удалённого хоста на SD-карту
# для загрузки Raspberry Pi 4 (Psych Ward OS).
#
# Работает с двумя партициями:
#   BOOT (FAT32) — загрузочные файлы (config.txt, u-boot.bin, start4.elf и т.д.)
#   RPI  (exFAT) — рабочие каталоги ОС (bin, sbin, etc, conf, service, root и т.д.)
#

set -u

# ===== НАСТРОЙКИ =====

BOOT_VOLUME="BOOT"
RPI_VOLUME="RPI"

REMOTE_HOST="nikita@compni"
REMOTE_PORT="22"
REMOTE_DIR="/home/nikita/psych-ward-os/load_chain"

# Что копируется на партицию BOOT (список путей относительно REMOTE_DIR)
BOOT_ITEMS=(
    "bcm2711-rpi-4-b.dtb"
    "boot.itb"
    "config.txt"
    "fixup4.dat"
    "start4.elf"
    "u-boot.bin"
    "overlays"
)

# Что копируется на партицию RPI (список путей относительно REMOTE_DIR)
RPI_ITEMS=(
    "bin"
    "sbin"
    "etc"
    "conf"
    "service"
    "root"
)

# 1 = удалить всё видимое содержимое партиции перед копированием
# 0 = просто скопировать поверх
CLEAN_BEFORE_COPY=1

# ===== КОНЕЦ НАСТРОЕК =====

EXCLUDE_DIRS=()
EXCLUDE_FILES=()
COPY_BOOT=0
COPY_RPI=0

usage() {
    cat <<HELP
flash_sync.sh — синхронизация файлов на SD-карту RPi4

ФЛАГИ ПАРТИЦИЙ (обязательно указать хотя бы один):
  -b        копировать на партицию BOOT (FAT32) — загрузочные файлы
  -r        копировать на партицию RPI  (exFAT) — рабочие каталоги ОС
  -br, -rb  копировать на обе партиции сразу

ФЛАГИ ИСКЛЮЧЕНИЙ (опционально, указываются один раз, за ними
идут абсолютные пути через пробел, относительно корня удалённой папки):
  -ncpd <path1> <path2> ...   не копировать эти папки
                              и НЕ УДАЛЯТЬ их с флешки при очистке
  -ncpf <path1> <path2> ...   не копировать эти файлы
                              и НЕ УДАЛЯТЬ их с флешки при очистке

  -h, --help                  показать эту справку

ПРИМЕРЫ:
  ./flash_sync.sh -b                        # только загрузочная часть
  ./flash_sync.sh -r                        # только рабочие каталоги ОС
  ./flash_sync.sh -br                       # обе партиции
  ./flash_sync.sh -r -ncpd /root            # RPI, но без папки root
  ./flash_sync.sh -b -ncpf /config.txt      # BOOT, но без config.txt
  ./flash_sync.sh -br -ncpd /root /service  # обе, без root и service
HELP
    exit 0
}

# Парсим объединённый флаг -b/-r/-br/-rb
parse_br_flag() {
    local flag="$1"
    local rest="${flag#-}"
    while [ -n "${rest}" ]; do
        local ch="${rest:0:1}"
        case "${ch}" in
            b) COPY_BOOT=1 ;;
            r) COPY_RPI=1 ;;
            *) return 1 ;;
        esac
        rest="${rest:1}"
    done
    return 0
}

# Парсим аргументы
current_mode=""
while [ $# -gt 0 ]; do
    case "$1" in
        -ncpd)
            current_mode="dirs"
            shift
            ;;
        -ncpf)
            current_mode="files"
            shift
            ;;
        -h|--help)
            usage
            ;;
        -b|-r|-br|-rb)
            if parse_br_flag "$1"; then
                current_mode=""
                shift
            else
                echo "ОШИБКА: неизвестный флаг '$1'"
                exit 1
            fi
            ;;
        -*)
            echo "ОШИБКА: неизвестный флаг '$1'"
            echo "Используй -h для справки"
            exit 1
            ;;
        *)
            if [ "${current_mode}" = "dirs" ]; then
                if [[ "$1" != /* ]]; then
                    echo "ОШИБКА: путь '$1' должен быть абсолютным (начинаться с /)"
                    exit 1
                fi
                EXCLUDE_DIRS+=("$1")
            elif [ "${current_mode}" = "files" ]; then
                if [[ "$1" != /* ]]; then
                    echo "ОШИБКА: путь '$1' должен быть абсолютным (начинаться с /)"
                    exit 1
                fi
                EXCLUDE_FILES+=("$1")
            else
                echo "ОШИБКА: путь '$1' указан без предшествующего флага (-ncpd или -ncpf)"
                exit 1
            fi
            shift
            ;;
    esac
done

# Требуем явное указание партиции
if [ "${COPY_BOOT}" -eq 0 ] && [ "${COPY_RPI}" -eq 0 ]; then
    cat <<ERR
ОШИБКА: не указано, какие партиции копировать.

Укажи хотя бы один из флагов:
  -b        только BOOT (FAT32, загрузочные файлы)
  -r        только RPI  (exFAT, рабочие каталоги ОС)
  -br       обе партиции

Полная справка: ./flash_sync.sh -h
ERR
    exit 1
fi

# Проверка, что имя входит в список исключений
is_excluded() {
    local name="$1"
    local path
    if [ ${#EXCLUDE_DIRS[@]} -gt 0 ]; then
        for path in "${EXCLUDE_DIRS[@]}"; do
            if [ "${path#/}" = "${name}" ]; then
                return 0
            fi
        done
    fi
    if [ ${#EXCLUDE_FILES[@]} -gt 0 ]; then
        for path in "${EXCLUDE_FILES[@]}"; do
            if [ "${path#/}" = "${name}" ]; then
                return 0
            fi
        done
    fi
    return 1
}

# Обрабатывает партицию: очистка + rsync элементов
# $1 - точка монтирования
# $2 - имя партиции (для вывода)
# $3, $4, ... - элементы для копирования
process_partition() {
    local mount_point="$1"
    local partition_name="$2"
    shift 2
    local items=("$@")

    echo "==> Партиция ${partition_name} (${mount_point})"

    if [ ! -d "${mount_point}" ]; then
        echo "    ПРОПУСК: партиция не смонтирована"
        return 1
    fi

    if [ "${CLEAN_BEFORE_COPY}" -eq 1 ]; then
        echo "    Очищаем содержимое (кроме исключённых путей)..."
        while IFS= read -r -d '' item; do
            local name
            name=$(basename "${item}")
            case "${name}" in
                .fseventsd|.Spotlight-V100|.Trashes)
                    continue
                    ;;
            esac
            if is_excluded "${name}"; then
                echo "    Пропускаем (в списке исключений): ${name}"
                continue
            fi
            rm -rf "${item}"
        done < <(find "${mount_point}" -mindepth 1 -maxdepth 1 -print0)
    fi

    echo "    Копируем элементы через rsync..."
    local item
    for item in "${items[@]}"; do
        if is_excluded "${item}"; then
            echo "    - ${item} — в списке исключений, пропускаем"
            continue
        fi
        echo "    - ${item}"
        if ! rsync -avz --progress -e "ssh -p ${REMOTE_PORT}" \
             "${REMOTE_HOST}:${REMOTE_DIR}/${item}" "${mount_point}/"; then
            echo "    ОШИБКА при копировании ${item}"
            return 1
        fi
    done

    echo "    Чистим macOS-мусор..."
    find "${mount_point}" -name ".DS_Store" -delete 2>/dev/null
    find "${mount_point}" -name "._*" -delete 2>/dev/null
    rm -rf "${mount_point}/.Trashes" 2>/dev/null
    rm -rf "${mount_point}/.fseventsd" 2>/dev/null
    rm -rf "${mount_point}/.Spotlight-V100" 2>/dev/null
    dot_clean -m "${mount_point}" 2>/dev/null || true

    return 0
}

# Определяем какие партиции ждём
WAIT_VOLUMES=()
[ "${COPY_BOOT}" -eq 1 ] && WAIT_VOLUMES+=("${BOOT_VOLUME}")
[ "${COPY_RPI}" -eq 1 ] && WAIT_VOLUMES+=("${RPI_VOLUME}")

echo "==> Жду появления партиций: ${WAIT_VOLUMES[*]}"
if [ ${#EXCLUDE_DIRS[@]} -gt 0 ]; then
    echo "    Исключённые папки: ${EXCLUDE_DIRS[*]}"
fi
if [ ${#EXCLUDE_FILES[@]} -gt 0 ]; then
    echo "    Исключённые файлы: ${EXCLUDE_FILES[*]}"
fi
echo "    (вставь флешку сейчас, Ctrl-C для отмены)"

while :; do
    all_present=1
    for vol in "${WAIT_VOLUMES[@]}"; do
        if [ ! -d "/Volumes/${vol}" ]; then
            all_present=0
            break
        fi
    done
    [ "${all_present}" -eq 1 ] && break
    sleep 1
done

echo "    Все нужные партиции найдены."
sleep 2

PROCESS_ERRORS=0

if [ "${COPY_BOOT}" -eq 1 ]; then
    if ! process_partition "/Volumes/${BOOT_VOLUME}" "${BOOT_VOLUME}" "${BOOT_ITEMS[@]}"; then
        PROCESS_ERRORS=1
    fi
fi

if [ "${COPY_RPI}" -eq 1 ]; then
    if ! process_partition "/Volumes/${RPI_VOLUME}" "${RPI_VOLUME}" "${RPI_ITEMS[@]}"; then
        PROCESS_ERRORS=1
    fi
fi

if [ "${PROCESS_ERRORS}" -ne 0 ]; then
    echo "==> ВНИМАНИЕ: были ошибки при обработке партиций."
    echo "    Флешка НЕ отмонтирована - разберись и повтори вручную."
    exit 1
fi

echo "==> Отмонтируем обе партиции флешки..."
diskutil eject "/Volumes/${BOOT_VOLUME}" 2>/dev/null || true
diskutil eject "/Volumes/${RPI_VOLUME}" 2>/dev/null || true
echo "==> Готово! Можно вынимать."