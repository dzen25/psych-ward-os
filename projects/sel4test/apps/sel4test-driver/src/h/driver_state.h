#pragma once
// h/driver_state.h — сторона ДРАЙВЕРА для общей страницы состояния
// (issuse.txt №74, часть "б" — адаптивный перенос драйверов между ядрами;
// полный проект механизма — в situation.txt).
//
// Зачем: seL4_TCB_Suspend() над потоком, который прямо сейчас реально
// исполняется на ДРУГОМ ядре, уходит в remoteTCBStall() -> ipi_wait() —
// синхронный барьер БЕЗ таймаута, вешающий root навсегда (hw-подтверждено
// JTAG'ом, issuse.txt №73). Пока драйверы были заперты на ядре 0, вопрос не
// вставал; теперь они переносятся, и root'у нужен способ УЗНАТЬ, что
// драйвер сейчас стоит в своём главном seL4_Recv (значит не исполняется
// нигде) — не спрашивая его по IPC, потому что зависший драйвер на IPC уже
// не ответит, а seL4_Call к нему повесит root ровно так же.
//
// Отсюда: одно слово в общей некэшируемой странице. Драйвер помечает
// PARKED непосредственно ПЕРЕД seL4_Recv и BUSY сразу ПОСЛЕ него — root
// читает обычной инструкцией загрузки, без единого syscall'а.
//
// Стоимость на драйвер — ровно две записи в MMIO-подобную память на каждую
// итерацию главного цикла; на фоне самого seL4_Recv (полноценный syscall с
// переключением контекста) не значима.
#include <sel4/sel4.h>
#include <stdint.h>
#include "common.h" // относительно ЭТОГО файла (h/), не относительно src/ — см. gcc "" -include

// Указатель на СВОЙ слот. nullptr, если root страницу не дал (старый
// вызывающий, не-драйвер, или сборка, где механизм выключен) — тогда все
// функции ниже молча ничего не делают, и root просто считает такой драйвер
// "состояние неизвестно" (см. driver_state_slot()/main.cpp).
static volatile seL4_Word *g_driver_state_slot = nullptr;

// Вызывается один раз при старте драйвера, ПОСЛЕ чтения boot-параметров.
// page_vaddr — PLAT_DRIVER_STATE_VADDR (драйверы 1-5) или
// PLAT_DRIVER_STATE_USB_VADDR (usb_driver); present — msg[
// BOOT_DRIVER_STATE_PRESENT] из своего же IPC-буфера.
static inline void driver_state_init(uintptr_t page_vaddr, seL4_Word is_driver, seL4_Word present)
{
    if (!present || is_driver == 0 || is_driver > 6) {
        g_driver_state_slot = nullptr;
        return;
    }
    g_driver_state_slot = (volatile seL4_Word *)(page_vaddr + (uintptr_t)is_driver * DRIVER_STATE_SLOT_STRIDE);
    g_driver_state_slot[DRIVER_STATE_WORD_PROGRESS] = 0;
    g_driver_state_slot[DRIVER_STATE_WORD_STEP] = 0;
    // Стартуем как BUSY: bring-up драйвера — это как раз тот случай, когда
    // трогать его Suspend'ом нельзя, а до первого seL4_Recv может пройти
    // немало времени (xHCI/EMMC/SDIO инициализация).
    *g_driver_state_slot = DRIVER_STATE_BUSY;
}

// Ставится НЕПОСРЕДСТВЕННО перед главным seL4_Recv.
static inline void driver_state_parked(void)
{
    if (g_driver_state_slot) *g_driver_state_slot = DRIVER_STATE_PARKED;
}

// Ставится СРАЗУ после возврата из главного seL4_Recv, ДО любых ветвлений
// и `continue` — иначе быстрые пути (heartbeat-тик и т.п.) оставляли бы
// драйвер вечно "припаркованным" и в момент реальной работы тоже.
static inline void driver_state_busy(void)
{
    if (g_driver_state_slot) *g_driver_state_slot = DRIVER_STATE_BUSY;
}

// --- Пошаговый прогресс (см. common.h/UsbStep и "зачем" у
// ROOT_WATCHDOG_TICK_BADGE). Объявить шаг ПЕРЕД входом в него;
// driver_state_progress() дёргать внутри длинных циклов ожидания.
//
// Разница между "занят" и "завис" не выводится из одного лишь факта
// молчания: легитимная операция с железом может занимать секунды, а
// сигнал живости драйвер шлёт, только вернувшись в свой Recv. Растущий
// счётчик — это "я в цикле ожидания, но я жив и дойду до своего
// таймаута"; замерший счётчик — "ядро физически не исполняет мой код"
// (аппаратный стопор шины, тот самый класс, что не берёт даже JTAG). ---
static inline void driver_state_progress(void)
{
    if (g_driver_state_slot) g_driver_state_slot[DRIVER_STATE_WORD_PROGRESS]++;
}

// Просил ли root не трогать железо (см. common.h/DRIVER_STATE_WORD_FREEZE).
static inline bool driver_state_freeze_requested(void)
{
    if (!g_driver_state_slot) return false;
    return g_driver_state_slot[DRIVER_STATE_WORD_FREEZE] != 0;
}

static inline void driver_state_step(seL4_Word step)
{
    if (!g_driver_state_slot) return;
    g_driver_state_slot[DRIVER_STATE_WORD_STEP] = step;
    g_driver_state_slot[DRIVER_STATE_WORD_PROGRESS]++;
}
