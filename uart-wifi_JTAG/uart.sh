#!/bin/bash
#
# uart.sh — безопасное подключение к ESP32-C3 UART-мосту 
# через socat + minicom, с очисткой зависших процессов и проверками на каждом шаге.
#
# Использование:
    # chmod +x uart.sh    # когда скопируешь файл - не забудь

#   ./uart.sh <IP-адрес> [порт] [baudrate] - не обязательно нужны если у тебя не инные
#
# Пример:
#   ./uart.sh 192.168.2.240
#   ./uart.sh 192.168.2.240 23 115200
#

set -u

IP="${1:-}"
PORT="${2:-23}"
BAUD="${3:-115200}"
LINK="/tmp/esp32c3"

if [ -z "$IP" ]; then
    echo "Использование: $0 <IP-адрес> [порт] [baudrate]"
    exit 1
fi

echo "==> Завершаем зависшие процессы socat/minicom..."
pkill -f "socat.*${LINK}" 2>/dev/null
pkill -f "minicom -D ${LINK}" 2>/dev/null
sleep 0.5

echo "==> Удаляем старый файл ${LINK}, если остался..."
rm -f "${LINK}"

echo "==> Проверяем доступность ${IP}:${PORT}..."
if ! nc -z -w 3 "${IP}" "${PORT}"; then
    echo "ОШИБКА: ${IP}:${PORT} недоступен (timeout за 3 секунды)."
    echo "Проверь: жива ли плата, не сменился ли IP, не завис ли WiFiServer на плате."
    exit 1
fi
echo "    Порт отвечает, продолжаем."

echo "==> Запускаем socat в фоне..."
socat pty,link="${LINK}",raw,echo=0 tcp:"${IP}":"${PORT}" &
SOCAT_PID=$!

echo "==> Ждём появления ${LINK} (до 5 секунд)..."
for i in $(seq 1 50); do
    if [ -e "${LINK}" ]; then
        break
    fi
    sleep 0.1
done

if [ ! -e "${LINK}" ]; then
    echo "ОШИБКА: ${LINK} не появился за 5 секунд — socat не смог установить соединение."
    kill "${SOCAT_PID}" 2>/dev/null
    exit 1
fi
echo "    Файл ${LINK} создан, socat (pid ${SOCAT_PID}) работает."

echo "==> Запускаем minicom..."
minicom -D "${LINK}" -b "${BAUD}"

echo "==> minicom завершён, останавливаем socat (pid ${SOCAT_PID})..."
kill "${SOCAT_PID}" 2>/dev/null
rm -f "${LINK}"
echo "==> Готово."