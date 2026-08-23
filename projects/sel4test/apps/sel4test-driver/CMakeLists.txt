cmake_minimum_required(VERSION 3.16.0)

project(sel4test-driver C CXX ASM)

find_package(musllibc REQUIRED)
find_package(util_libs REQUIRED)
find_package(seL4_libs REQUIRED)

set(UserLinkerGCSections OFF CACHE BOOL "" FORCE)

musllibc_setup_build_environment_with_sel4runtime()
sel4_import_libsel4()
util_libs_import_libraries()
sel4_libs_import_libraries()
 
# ===================================================================
# --- ИЗОЛИРОВАННЫЕ ПРОГРАММЫ (USER-SPACE BINARIES) ---
# ===================================================================

add_executable(uart_driver src/uart_driver.cpp)
target_include_directories(uart_driver PRIVATE "${CMAKE_BINARY_DIR}/libsel4/autoconf" "${CMAKE_BINARY_DIR}/kernel/gen_config" "${CMAKE_BINARY_DIR}/libsel4/gen_config")
target_link_options(uart_driver PRIVATE "-nostdlib" "-static")
target_link_options(uart_driver PRIVATE "-Wl,-e,_start")
target_link_libraries(uart_driver sel4 sel4runtime muslc)

add_executable(timer_driver src/timer_driver.cpp)
target_include_directories(timer_driver PRIVATE "${CMAKE_BINARY_DIR}/libsel4/autoconf" "${CMAKE_BINARY_DIR}/kernel/gen_config" "${CMAKE_BINARY_DIR}/libsel4/gen_config")
target_link_options(timer_driver PRIVATE "-nostdlib" "-static")
target_link_options(timer_driver PRIVATE "-Wl,-e,_start")
target_link_libraries(timer_driver sel4 sel4runtime muslc)

add_executable(shell src/shell.cpp)
target_include_directories(shell PRIVATE "${CMAKE_BINARY_DIR}/libsel4/autoconf" "${CMAKE_BINARY_DIR}/kernel/gen_config" "${CMAKE_BINARY_DIR}/libsel4/gen_config")
target_link_options(shell PRIVATE "-nostdlib" "-static")
target_link_options(shell PRIVATE "-Wl,-e,_start")
target_link_libraries(shell sel4 sel4runtime muslc)

# 4. НОВЫЙ БЛОЧНЫЙ ДРАЙВЕР (Файловая система)
# Уход от FAT32/8.3 на exFAT (см. ROADMAP.md/issuse.txt, план) — src/fat32.cpp
# больше не подключается: вторая (единственная монтируемая нами) партиция
# карты теперь exFAT, src/exfat.cpp. Сам fat32.cpp/h/fat32.h не удалены (не
# критично, см. план), просто больше нигде не используются.
add_executable(blk_driver src/blk_driver.cpp src/exfat.cpp)
target_include_directories(blk_driver PRIVATE "${CMAKE_BINARY_DIR}/libsel4/autoconf" "${CMAKE_BINARY_DIR}/kernel/gen_config" "${CMAKE_BINARY_DIR}/libsel4/gen_config")
target_link_options(blk_driver PRIVATE "-nostdlib" "-static")
target_link_options(blk_driver PRIVATE "-Wl,-e,_start")
target_link_libraries(blk_driver sel4 sel4runtime muslc)

# 4. НОВЫЙ СЕТЕВОЙ ДРАЙВЕР
add_executable(net_driver src/net_driver.cpp)
target_include_directories(net_driver PRIVATE "${CMAKE_BINARY_DIR}/libsel4/autoconf" "${CMAKE_BINARY_DIR}/kernel/gen_config" "${CMAKE_BINARY_DIR}/libsel4/gen_config")
target_link_options(net_driver PRIVATE "-nostdlib" "-static")
target_link_options(net_driver PRIVATE "-Wl,-e,_start")
target_link_libraries(net_driver sel4 sel4runtime muslc)

# USB драйвер (Фаза 14 — xHCI bring-up + перечисление, см. ROADMAP.md).
# Компилируется в CPIO вместе с uart/timer/blk/net (не отдельно грузимый с
# диска, как wifi_driver) — см. RPI4_ENABLE_USB/is_driver==6 в main.cpp.
add_executable(usb_driver src/usb_driver.cpp src/exfat.cpp)
target_include_directories(usb_driver PRIVATE "${CMAKE_BINARY_DIR}/libsel4/autoconf" "${CMAKE_BINARY_DIR}/kernel/gen_config" "${CMAKE_BINARY_DIR}/libsel4/gen_config")
target_link_options(usb_driver PRIVATE "-nostdlib" "-static")
target_link_options(usb_driver PRIVATE "-Wl,-e,_start")
target_link_libraries(usb_driver sel4 sel4runtime muslc)

# Wi-Fi драйвер (Фаза 4, Милстоун 4.1 — SDIO host bring-up, см. ROADMAP.md)
add_executable(wifi_driver src/wifi_driver.cpp)
target_include_directories(wifi_driver PRIVATE "${CMAKE_BINARY_DIR}/libsel4/autoconf" "${CMAKE_BINARY_DIR}/kernel/gen_config" "${CMAKE_BINARY_DIR}/libsel4/gen_config")
target_link_options(wifi_driver PRIVATE "-nostdlib" "-static")
target_link_options(wifi_driver PRIVATE "-Wl,-e,_start")
target_link_libraries(wifi_driver sel4 sel4runtime muslc)

# ===================================================================
# --- СБОРКА VFS (RAM-ДИСКА) ВРУЧНУЮ ---
# ===================================================================
set(CPIO_FILE "${CMAKE_CURRENT_BINARY_DIR}/archive.cpio")
set(ARCHIVE_S "${CMAKE_CURRENT_BINARY_DIR}/archive.S")

# ВНИМАНИЕ: fat32.img мы отсюда убрали, так как он теперь работает как реальный диск
#
# Фаза 9.B (см. ROADMAP.md): wifi_driver БОЛЬШЕ НЕ здесь — вынесен на диск
# (/service/wifi.elf, грузится SYS_START_WIFI через load_elf_from_disk()),
# в отличие от uart/timer/blk/net, которые остаются встроенными в образ.
# CMake-цель wifi_driver ниже (строка ~53) никуда не делась — просто больше
# не участвует в CPIO-архиве, а собирается как обычный отдельный бинарник
# (см. деплой в load_chain/RPI/service/wifi.elf).
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/pack_vfs.sh"
"#!/bin/bash
cd ${CMAKE_CURRENT_BINARY_DIR}
echo 'uart_driver' > file_list.txt
echo 'timer_driver' >> file_list.txt
echo 'shell' >> file_list.txt
echo 'blk_driver' >> file_list.txt
echo 'net_driver' >> file_list.txt # <--- ДОБАВЬ ЭТУ СТРОКУ
echo 'usb_driver' >> file_list.txt
cat file_list.txt | cpio -o -H newc > archive.cpio

echo '.section ._archive_cpio,\"aw\"' > archive.S
echo '.globl _cpio_archive, _cpio_archive_end' >> archive.S
echo '_cpio_archive:' >> archive.S
echo '.incbin \"${CMAKE_CURRENT_BINARY_DIR}/archive.cpio\"' >> archive.S
echo '_cpio_archive_end:' >> archive.S
")

add_custom_command(
    OUTPUT "${CPIO_FILE}" "${ARCHIVE_S}"
    COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:uart_driver>" "${CMAKE_CURRENT_BINARY_DIR}/uart_driver"
    COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:timer_driver>" "${CMAKE_CURRENT_BINARY_DIR}/timer_driver"
    COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:shell>" "${CMAKE_CURRENT_BINARY_DIR}/shell"
    COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:blk_driver>" "${CMAKE_CURRENT_BINARY_DIR}/blk_driver"
    COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:net_driver>" "${CMAKE_CURRENT_BINARY_DIR}/net_driver"
    COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:usb_driver>" "${CMAKE_CURRENT_BINARY_DIR}/usb_driver"
    COMMAND bash "${CMAKE_CURRENT_BINARY_DIR}/pack_vfs.sh"
    DEPENDS uart_driver timer_driver shell blk_driver net_driver usb_driver
)

# ===================================================================
# --- СБОРКА ЯДРА (ROOTSERVER) ---
# ===================================================================
# Фаза 12 (см. ROADMAP.md) — Monocypher (Ed25519), для проверки подписи
# /sbin, /service, /etc/init.conf перед load_elf_from_disk() в main.cpp.
# Просто исходники в общий список сборки rootserver'а — как и весь остальной
# проект, никакого code-gen шага тут нет (в отличие от nanopb), отдельный
# CMake-хелпер/библиотечная цель не нужны.
set(MONOCYPHER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../../tools/monocypher/src")

add_executable(sel4test-driver
    src/main.cpp
    src/common.cpp
    src/allocator.cpp
    src/uart.cpp
    src/hw_timer.cpp
    "${MONOCYPHER_DIR}/monocypher.c"
    "${MONOCYPHER_DIR}/optional/monocypher-ed25519.c"
    ${ARCHIVE_S}
)

target_include_directories(sel4test-driver PRIVATE src "${MONOCYPHER_DIR}" "${MONOCYPHER_DIR}/optional")

target_link_libraries(sel4test-driver
    sel4
    muslc
    sel4muslcsys
    sel4platsupport
    sel4utils
    sel4debug
    sel4runtime
    utils
)

set_target_properties(sel4test-driver PROPERTIES LINKER_LANGUAGE CXX)

include(rootserver)
DeclareRootserver(sel4test-driver)

add_executable(test_app src/test.cpp)
target_include_directories(test_app PRIVATE "${CMAKE_BINARY_DIR}/libsel4/autoconf" "${CMAKE_BINARY_DIR}/kernel/gen_config" "${CMAKE_BINARY_DIR}/libsel4/gen_config")
target_link_options(test_app PRIVATE "-static")
target_link_libraries(test_app sel4 sel4runtime muslc)

# По просьбе пользователя (2026-08-23): команды, которые раньше жили здесь
# как отдельные /sbin/*.elf (Фаза A), слиты обратно в shell.cpp как обычные
# функции (run_ls()/run_cat()/... — см. shell.cpp) — слишком маленькие
# (~15-100 строк каждая), чтобы оправдывать накладные расходы полного
# SYS_EXEC-спавна процесса при каждом вызове, особенно из скриптов/
# init.conf. src/sbin/ больше не участвует в сборке.

# issuse.txt №62 (расследование) — префикс sbtest_ (не sb_!) — тестовые хуки
# (см. src/tests/) специально НЕ трогали при слиянии выше, остаются
# отдельными ELF; build_and_sign.sh раскладывает их в load_chain/RPI/sbin/tests/.
set(PSYCH_TEST_TOOLS holdshm proxytest recovertest stresstest ctrlbytetest timedread clonetest)
foreach(tool ${PSYCH_TEST_TOOLS})
    add_executable(sbtest_${tool} src/tests/${tool}.cpp)
    target_include_directories(sbtest_${tool} PRIVATE src "${CMAKE_BINARY_DIR}/libsel4/autoconf" "${CMAKE_BINARY_DIR}/kernel/gen_config" "${CMAKE_BINARY_DIR}/libsel4/gen_config")
    target_link_options(sbtest_${tool} PRIVATE "-static")
    target_link_libraries(sbtest_${tool} sel4 sel4runtime muslc)
endforeach()

# Фаза 9.B (см. ROADMAP.md): фоновые сервисы для автозапуска из
# /etc/init.conf (см. src/service/) — тот же паттерн сборки, что sb_*/
# test_app выше. Префикс svc_ по той же причине, что sb_ у команд.
set(PSYCH_SERVICE_TOOLS balancer)
foreach(svc ${PSYCH_SERVICE_TOOLS})
    add_executable(svc_${svc} src/service/${svc}.cpp)
    target_include_directories(svc_${svc} PRIVATE src "${CMAKE_BINARY_DIR}/libsel4/autoconf" "${CMAKE_BINARY_DIR}/kernel/gen_config" "${CMAKE_BINARY_DIR}/libsel4/gen_config")
    target_link_options(svc_${svc} PRIVATE "-static")
    target_link_libraries(svc_${svc} sel4 sel4runtime muslc)
endforeach()