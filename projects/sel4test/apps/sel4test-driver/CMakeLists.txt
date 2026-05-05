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

# 1. Драйвер UART
add_executable(uart_driver src/uart_driver.cpp)
target_include_directories(uart_driver PRIVATE "${CMAKE_BINARY_DIR}/libsel4/autoconf" "${CMAKE_BINARY_DIR}/kernel/gen_config" "${CMAKE_BINARY_DIR}/libsel4/gen_config")
target_link_options(uart_driver PRIVATE "-nostdlib" "-static")
target_link_libraries(uart_driver sel4 sel4runtime muslc) # <-- ДОБАВЛЕНЫ ЗАВИСИМОСТИ

# 2. Драйвер Таймера
add_executable(timer_driver src/timer_driver.cpp)
target_include_directories(timer_driver PRIVATE "${CMAKE_BINARY_DIR}/libsel4/autoconf" "${CMAKE_BINARY_DIR}/kernel/gen_config" "${CMAKE_BINARY_DIR}/libsel4/gen_config")
target_link_options(timer_driver PRIVATE "-nostdlib" "-static")
target_link_libraries(timer_driver sel4 sel4runtime muslc) # <-- ДОБАВЛЕНЫ ЗАВИСИМОСТИ

# 3. Оболочка (Shell)
add_executable(shell src/shell.cpp)
target_include_directories(shell PRIVATE "${CMAKE_BINARY_DIR}/libsel4/autoconf" "${CMAKE_BINARY_DIR}/kernel/gen_config" "${CMAKE_BINARY_DIR}/libsel4/gen_config")
target_link_options(shell PRIVATE "-nostdlib" "-static")
target_link_libraries(shell sel4 sel4runtime muslc) # <-- ДОБАВЛЕНЫ ЗАВИСИМОСТИ

# --- УПАКОВЫВАЕМ ВСЕ БИНАРНИКИ В ОДИН СПИСОК ---
set(VFS_FILES 
    "$<TARGET_FILE:uart_driver>"
    "$<TARGET_FILE:timer_driver>"
    "$<TARGET_FILE:shell>"
)

# ===================================================================
# --- СБОРКА VFS (RAM-ДИСКА) ВРУЧНУЮ (БРОНЕБОЙНАЯ ВЕРСИЯ) ---
# ===================================================================
set(CPIO_FILE "${CMAKE_CURRENT_BINARY_DIR}/archive.cpio")
set(ARCHIVE_S "${CMAKE_CURRENT_BINARY_DIR}/archive.S")

# 1. Генерируем bash-скрипт один раз при конфигурации (никаких проблем с кавычками!)
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/pack_vfs.sh"
"#!/bin/bash
cd ${CMAKE_CURRENT_BINARY_DIR}
echo 'uart_driver' > file_list.txt
echo 'timer_driver' >> file_list.txt
echo 'shell' >> file_list.txt
cat file_list.txt | cpio -o -H newc > archive.cpio

echo '.section ._archive_cpio,\"aw\"' > archive.S
echo '.globl _cpio_archive, _cpio_archive_end' >> archive.S
echo '_cpio_archive:' >> archive.S
echo '.incbin \"${CMAKE_CURRENT_BINARY_DIR}/archive.cpio\"' >> archive.S
echo '_cpio_archive_end:' >> archive.S
")

# 2. Правило сборки, которое просто запускает этот скрипт
add_custom_command(
    OUTPUT "${CPIO_FILE}" "${ARCHIVE_S}"
    COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:uart_driver>" "${CMAKE_CURRENT_BINARY_DIR}/uart_driver"
    COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:timer_driver>" "${CMAKE_CURRENT_BINARY_DIR}/timer_driver"
    COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:shell>" "${CMAKE_CURRENT_BINARY_DIR}/shell"
    COMMAND bash "${CMAKE_CURRENT_BINARY_DIR}/pack_vfs.sh"
    DEPENDS uart_driver timer_driver shell
)
# ===================================================================

# Эта команда создаст папку vfs_root и скопирует туда бинарники после их компиляции
add_custom_command(
    OUTPUT "${VFS_DIR}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${VFS_DIR}"
    COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:uart_driver>" "${VFS_DIR}/uart_driver"
    COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:timer_driver>" "${VFS_DIR}/timer_driver"
    COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:shell>" "${VFS_DIR}/shell"
    DEPENDS uart_driver timer_driver shell
)

# Передаем всю папку целиком в скрипт упаковки RAM-диска
MakeCPIO(archive.o "${VFS_DIR}")
# ===================================================================

add_executable(sel4test-driver
    src/main.cpp
    src/common.cpp
    src/allocator.cpp
    src/uart.cpp
    src/hw_timer.cpp
    # src/patient.cpp <-- УДАЛЕНО: Этот старый монолитный код нам больше не нужен
    ${ARCHIVE_S}  # <--- ИСПРАВЛЕНИЕ: CMake сам скомпилирует наш сгенерированный .S файл!
)

target_include_directories(sel4test-driver PRIVATE src)

# Из линковки мы его убрали, здесь остались только библиотеки
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