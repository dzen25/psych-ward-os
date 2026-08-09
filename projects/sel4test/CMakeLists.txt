#
# Copyright 2019, Data61, CSIRO (ABN 41 687 119 230)
#
# SPDX-License-Identifier: BSD-2-Clause
#

cmake_minimum_required(VERSION 3.16.0)

include(settings.cmake)

project(sel4test C CXX ASM)

find_package(seL4 REQUIRED)
find_package(elfloader-tool REQUIRED)

# Set the roottask CNode size to be large enough for DTB, timer caps, etc
# Note that this works for the platforms that we have at the moment, and may
# need to be increased in the future
#
# Фаза 14 (USB, xHCI, см. ROADMAP.md) — 13 (8192 слотов) НЕ ХВАТИЛО на
# живом железе: alloc_device_frame() для xHCI (PADDR 0xfe9c0000) должен
# "прошагать" физический пробел от предыдущего использованного адреса в
# том же device-untyped регионе (EMMC2, 0xfe340000) — ~1650 страниц, КАЖДАЯ
# безвозвратно съедает один CSlot (см. комментарий "target_paddr BEHIND
# watermark" у alloc_device_frame() в main.cpp — это неотъемлемое свойство
# seL4 Untyped Retype, не баг конкретно в нашем аллокаторе: ретайп из
# Untyped всегда идёт от текущего free-offset вперёд, "перепрыгнуть" пробел
# без материализации промежуточных объектов нельзя). PLAT_XHCI_PADDR —
# самый высокий физический адрес среди всех устройств, что мы маппим, а
# 1MB (256 страниц) самого xHCI BAR — дополнительная нагрузка сверху.
# Живая ошибка была "Untyped Retype: Slot #0 in destination window
# non-empty" сразу после "dropped to user space", ДО старта uart_driver
# (root ещё не UART) — alloc_slot() тихо вернул 0 (OOM), Retype в слот 0
# root_cnode закономерно провалился. Подняли с запасом на будущие фазы
# (класс-драйверы потребуют ещё DMA-страниц).
set(KernelRootCNodeSizeBits 15 CACHE INTERNAL "")

sel4_import_kernel()

if((NOT Sel4testAllowSettingsOverride) AND (KernelArchARM OR KernelArchRiscV))
  # Elfloader settings that correspond to how Data61 sets its boards up.
  ApplyData61ElfLoaderSettings(${KernelPlatform} ${KernelSel4Arch})
endif()
elfloader_import_project()

add_subdirectory(apps/sel4test-driver)

if(SIMULATION)
  include(simulation)
  if(KernelSel4ArchX86_64)
    SetSimulationScriptProperty(MEM_SIZE "3G")
  endif()
  if(KernelPlatformQEMUArmVirt)
    SetSimulationScriptProperty(MEM_SIZE "2G")
  endif()
  GenerateSimulateScript()
endif()
