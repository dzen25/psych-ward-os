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

add_executable(shell src/shell.cpp src/pipe.cpp)
target_include_directories(shell PRIVATE "${CMAKE_BINARY_DIR}/libsel4/autoconf" "${CMAKE_BINARY_DIR}/kernel/gen_config" "${CMAKE_BINARY_DIR}/libsel4/gen_config")
target_link_options(shell PRIVATE "-nostdlib" "-static")
target_link_options(shell PRIVATE "-Wl,-e,_start")
target_link_libraries(shell sel4 sel4runtime muslc)

# 4. НОВЫЙ БЛОЧНЫЙ ДРАЙВЕР (Файловая система)
add_executable(blk_driver src/blk_driver.cpp)
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

# ===================================================================
# --- СБОРКА VFS (RAM-ДИСКА) ВРУЧНУЮ ---
# ===================================================================
set(CPIO_FILE "${CMAKE_CURRENT_BINARY_DIR}/archive.cpio")
set(ARCHIVE_S "${CMAKE_CURRENT_BINARY_DIR}/archive.S")

# ВНИМАНИЕ: fat32.img мы отсюда убрали, так как он теперь работает как реальный диск
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/pack_vfs.sh"
"#!/bin/bash
cd ${CMAKE_CURRENT_BINARY_DIR}
echo 'uart_driver' > file_list.txt
echo 'timer_driver' >> file_list.txt
echo 'shell' >> file_list.txt
echo 'blk_driver' >> file_list.txt
echo 'net_driver' >> file_list.txt # <--- ДОБАВЬ ЭТУ СТРОКУ
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
    COMMAND bash "${CMAKE_CURRENT_BINARY_DIR}/pack_vfs.sh"
    DEPENDS uart_driver timer_driver shell blk_driver net_driver
)

# ===================================================================
# --- СБОРКА ЯДРА (ROOTSERVER) ---
# ===================================================================
add_executable(sel4test-driver
    src/main.cpp
    src/common.cpp
    src/allocator.cpp
    src/uart.cpp
    src/hw_timer.cpp
    ${ARCHIVE_S} 
)

target_include_directories(sel4test-driver PRIVATE src)

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