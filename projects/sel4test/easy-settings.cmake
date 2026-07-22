#
# Copyright 2019, Data61, CSIRO (ABN 41 687 119 230)
#
# SPDX-License-Identifier: BSD-2-Clause
#

# Define our top level settings.  Whilst they have doc strings for readability
# here, they are hidden in the cmake-gui as they cannot be reliably changed
# after the initial configuration.  Enterprising users can still change them if
# they know what they are doing through advanced mode.
#
# Users should initialize a build directory by doing something like:
#
# mkdir build_sabre
# cd build_sabre
#
# Then
#
# ../griddle --PLATFORM=sabre --SIMULATION
# ninja
#
set(SIMULATION OFF CACHE BOOL "Include only simulation compatible tests")
set(RELEASE OFF CACHE BOOL "Performance optimized build")
set(VERIFICATION OFF CACHE BOOL "Only verification friendly kernel features")
set(BAMBOO OFF CACHE BOOL "Enable machine parseable output")
set(DOMAINS OFF CACHE BOOL "Test multiple domains")
set(SMP OFF CACHE BOOL "(if supported) Test SMP kernel")
set(NUM_NODES "" CACHE STRING "(if SMP) the number of nodes (default 4)")
set(PLATFORM "x86_64" CACHE STRING "Platform to test")
set(ARM_HYP OFF CACHE BOOL "Hyp mode for ARM platforms")
set(MCS OFF CACHE BOOL "MCS kernel")
set(KernelSel4Arch "" CACHE STRING "aarch32, aarch64, arm_hyp, ia32, x86_64, riscv32, riscv64")
set(KernelArmExportVCNTUser ON CACHE BOOL "Allow user-space to read CNTVCT/CNTFRQ for network RTT timing")
# Фаза 4.5 (событийный sleep, см. ROADMAP.md): даёт EL0 доступ к CNTP_CTL_EL0/
# CNTP_CVAL_EL0 (физический таймер), чтобы timer_driver мог сам взводить
# дедлайн и получать по нему настоящий IRQ (PPI 30, RPI4_TIMER_PPI_NONSECURE
# в platform.h) вместо busy-poll в sys_sleep(). НЕ KernelArmExportVTMRUser —
# тот же регистровый блок (CNTV_CTL/CNTV_CVAL), который ядро использует для
# СОБСТВЕННОГО тика планировщика на этой сборке (MCS выключен, ARM_HYP
# выключен — см. kernel/include/arch/arm/arch/64/mode/machine.h, CNT_CTL ==
# CNTV_CTL без CONFIG_ARM_HYPERVISOR_SUPPORT); дать userspace писать в него
# значило бы ломать планировщик ядра. Физический таймер ядро вообще не трогает.
set(KernelArmExportPTMRUser ON CACHE BOOL "Allow user-space to arm the physical timer (CNTP_CTL/CNTP_CVAL) for event-driven sleep")
set(LibSel4TestPrinterRegex ".*" CACHE STRING "A POSIX regex pattern used to filter tests")
set(LibSel4TestPrinterHaltOnTestFailure OFF CACHE BOOL "Halt on the first test failure")
mark_as_advanced(CLEAR LibSel4TestPrinterRegex LibSel4TestPrinterHaltOnTestFailure)
