# jtagload.gdb — заливка образа в ОЗУ RPi4 средствами самого GDB.
#
# Альтернатива jtagload.sh: медленнее (GDB бьёт запись на мелкие пакеты
# remote-протокола, `load_image` OpenOCD этого не делает), зато сразу
# оставляет живую сессию с символами — можно поставить брейкпоинт ДО
# старта образа и увидеть первые инструкции elfloader'а.
#
# Запуск (с сборочного сервера, туннель и OpenOCD на carto уже подняты):
#   gdb-multiarch -q -x uart_wifi_JTAG/load_over_JTAG/file/jtagload.gdb
#
# Предусловие то же, что у jtagload.sh: плата стоит в приглашении U-Boot
# (см. ../JTAG-load-guide.md, §4). Пути ниже — относительно корня репозитория,
# запускать gdb из него.

set confirm off
set pagination off
set remotetimeout 60

# elfloader — EXEC (не PIE), единственный PT_LOAD по 0x10000000; `load`
# кладёт ровно те же байты, что U-Boot копирует из FIT (проверено cmp,
# см. ../JTAG-load-guide.md, §2).
file build-rpi4/elfloader/elfloader

target extended-remote localhost:3333

# OpenOCD останавливает cpu0 сам при старте, но если сессия уже шла —
# убедимся, что цель стоит.
monitor halt

load

# Символы ядра и юзерленда — чтобы после старта можно было ставить
# брейкпоинты в seL4 и в rootserver'е. EXEC, адреса фиксированные,
# смещение не нужно.
add-symbol-file build-rpi4/kernel/kernel.elf
add-symbol-file build-rpi4/apps/sel4test-driver/sel4test-driver

set $pc = 0x10000000
set $x0 = 0
set $x1 = 0

printf "\n=== Образ залит, PC=0x%llx. 'continue' — старт. ===\n", $pc
info registers pc sp
