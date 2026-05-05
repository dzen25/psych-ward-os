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

# --- ПРОГРАММА ПАЦИЕНТА (Независимый голый бинарник) ---
add_executable(patient_app src/patient_app.cpp)

# ИСПРАВЛЕНИЕ: Вручную указываем пути к сгенерированным конфигам (autoconf.h), 
# так как мы больше не используем стандартные обертки!
target_include_directories(patient_app PRIVATE
    "${CMAKE_BINARY_DIR}/libsel4/autoconf"
    "${CMAKE_BINARY_DIR}/kernel/gen_config"
    "${CMAKE_BINARY_DIR}/libsel4/gen_config"
)

# Отключаем стандартные библиотеки (-nostdlib), чтобы контролировать точку входа!
target_link_options(patient_app PRIVATE "-nostdlib" "-static") # "-Wl,-e_my_start")
target_link_libraries(patient_app sel4)

MakeCPIO(archive.o "$<TARGET_FILE:patient_app>")
# ----------------------------------------------------
# ----------------------------------------------------

add_executable(sel4test-driver
    src/main.cpp
    src/common.cpp
    src/allocator.cpp
    src/uart.cpp
    src/hw_timer.cpp
    src/patient.cpp
    archive.o # <-- ИСПРАВЛЕНИЕ: Теперь CMake знает, что это часть бинарника
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