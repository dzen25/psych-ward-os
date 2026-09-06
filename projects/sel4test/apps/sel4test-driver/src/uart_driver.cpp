#include <sel4/sel4.h>
#include "h/common.h"
#include "h/driver_state.h"
#include "h/platform.h"
#include <stdint.h>

// issuse.txt №29: раньше здесь был мёртвый код — tx_buffer/flush_buffer()
// (асинхронная очередь на отправку в железо) и line_buffers/
// line_buffer_pos (буфер строки на клиента), задуманные для отложенного
// SYS_FLUSH. Ни в line_buffers, ни в tx_buffer НИКТО ничего не писал —
// SYS_PUTS всегда шёл напрямую в железо через uart_putc_wrapped()
// (busy-wait на TX_EMPTY, см. ниже), так что к моменту ответа на
// SYS_PUTS данные УЖЕ на проводе — "флашить" в буфере просто нечего.
// SYS_FLUSH оставлен ниже как честный no-op (мгновенный ответ) —
// shell.cpp его по-прежнему зовёт, менять клиентский код не нужно.
#define MAX_CLIENTS 256 // верхняя граница валидного PID (см. проверку sender_pid ниже) — не связано с удалённым line_buffers

// Глобальные указатели на регистры (mini-UART, см. platform.h)
static volatile seL4_Uint32 *uart_io = nullptr;
static volatile seL4_Uint32 *uart_lsr = nullptr;
static char* shm_vaddr = nullptr;

// Фаза 5 плана "Сигналы драйверам" (опционально, последняя фаза) —
// SYS_DRIVER_SIGNAL(STOP) гейтит SYS_PUTS (молча "проглатывает" — не
// пишет в железо, но ВСЕГДА отвечает, чтобы не повесить вызывающего,
// см. тот же принцип у blk_driver.cpp) и SYS_READ (немедленный -1 вместо
// отложенного reply — не оставляем читателя висеть, пока STOP активен).
// RESTART — заглушка (см. main() ниже): uart_io/uart_lsr/IRQ уже
// настроены один раз при спавне, переинициализировать нечего.
static bool g_uart_stopped = false;

// issuse.txt №30 — раньше kbd_buffer/head/tail были локальными в main(),
// недоступными из uart_putc() ниже. Подняты до file-scope, чтобы
// uart_putc() могла вычитывать RX ПРЯМО ВНУТРИ своего busy-wait на
// TX_EMPTY (см. uart_drain_rx() и её вызовы в uart_putc()) — иначе байты,
// пришедшие ПОКА мы ждём отправки эха предыдущего символа, копятся в
// крошечном АППАРАТНОМ RX FIFO мини-UART и теряются НЕОБРАТИМО ещё до
// того, как мы вообще вернёмся в seL4_Recv() и увидим IRQ-нотификацию —
// софтовый 128-слотовый kbd_buffer тут бессилен, физический байт уже
// затёрт на уровне контроллера. Живое подтверждение: `touch` с длинным
// (51 символ) именем создал файл с именем короче на 10 символов — без
// какого-либо конкурентного большого вывода в этот момент, просто эхо
// быстро вставленной длинной строки уже создавало нужное окно.
static char g_kbd_buffer[128];
static int g_kbd_head = 0, g_kbd_tail = 0;

// Общий с IRQ-веткой в main() код вычитывания FIFO — вызывается И оттуда
// (после реальной IRQ-нотификации), И из uart_putc() (см. выше). Дважды
// вызвать подряд без новых байт безопасно — while сам ничего не найдёт.
static void uart_drain_rx() {
    while ((*uart_lsr) & AUX_MU_LSR_RX_READY) {
        char c = *uart_io;
        int next_head = (g_kbd_head + 1) % 128;
        if (next_head == g_kbd_tail) break; // буфер полон — честно отбрасываем, не рассинхронизируем head/tail
        g_kbd_buffer[g_kbd_head] = c; g_kbd_head = next_head;
    }
}

static inline seL4_IPCBuffer* get_local_ipc() {
    seL4_Word tls_addr;
    // Добавлена буква 'ro'. crt0 не мог его стереть!
    asm volatile("mrs %0, tpidrro_el0" : "=r"(tls_addr)); 
    return (seL4_IPCBuffer*)(tls_addr - 1024);
}

void __assert_fail(const char *assertion, const char *file, int line, const char *function) { while(1); }

static void uart_putc(char c) {
    if (c == '\n') {
        while (!((*uart_lsr) & AUX_MU_LSR_TX_EMPTY)) uart_drain_rx();
        *uart_io = '\r';
    }
    while (!((*uart_lsr) & AUX_MU_LSR_TX_EMPTY)) uart_drain_rx();
    *uart_io = c;
}

// Перенос длинных строк по границам слов — раньше жил в shell.cpp и
// применялся только к тому, что печатает сам шелл (у wifi_driver.cpp/
// net_driver.cpp/blk_driver.cpp свои собственные простые sys_puts без
// переноса вообще — их длинные диагностические строки никогда не
// переносились). Перенесено сюда, в единственный процесс, через который
// реально проходит SYS_PUTS от ЛЮБОГО клиента — перенос теперь работает
// одинаково для всех, а не только для шелла.
//
// ИСПРАВЛЕНО: старая версия в shell.cpp считала ширину строки в БАЙТАХ
// (`word_len` рос на 1 за каждый байт), а не в отображаемых символах —
// для ASCII это одно и то же, но кириллица в UTF-8 — 2 байта на символ, а
// "—" (длинное тире) — 3, так что строка вроде "версия прошивки" (52
// видимых символа) байтово оказывалась ~81 байт и переносилась
// преждевременно ещё до реальной границы в 80 колонок. Здесь длина слова
// считается в КОЛОНКАХ — продолжающие UTF-8-байты (10xxxxxx) не увеличивают
// счётчик колонок, хотя сами байты всё равно копируются и печатаются.
constexpr int UART_TERM_WIDTH = 110;
constexpr int UART_WORD_BUF_CAP = 512; // байтовая ёмкость — с учётом многобайтных символов больше, чем ширина в колонках
static int  g_console_col = 0;
static bool g_console_in_escape = false;
static char g_word_buf[UART_WORD_BUF_CAP];
static int  g_word_len = 0;  // байт накоплено в g_word_buf
static int  g_word_cols = 0; // видимых колонок в накопленном слове

static void uart_flush_word() {
    if (g_word_len == 0) return;
    if (g_console_col > 0 && g_console_col + g_word_cols > UART_TERM_WIDTH) {
        uart_putc('\n');
        g_console_col = 0;
    }
    for (int i = 0; i < g_word_len; i++) uart_putc(g_word_buf[i]);
    g_console_col += g_word_cols;
    g_word_len = 0;
    g_word_cols = 0;
}

static void uart_putc_wrapped(char ch) {
    unsigned char c = (unsigned char)ch;

    if (g_console_in_escape) {
        uart_putc(ch);
        if (c >= 0x40 && c <= 0x7E) g_console_in_escape = false;
        return;
    }
    if (c == 27) { // ESC — начало ANSI CSI-последовательности (курсор/очистка строки в редакторе шелла)
        uart_flush_word();
        g_console_in_escape = true;
        uart_putc(ch);
        return;
    }
    if (c == '\n' || c == '\r') {
        uart_flush_word();
        uart_putc(ch);
        g_console_col = 0;
        return;
    }
    if (c == ' ') {
        uart_flush_word();
        if (g_console_col >= UART_TERM_WIDTH) {
            uart_putc('\n');
            g_console_col = 0;
        } else {
            uart_putc(' ');
            g_console_col++;
        }
        return;
    }

    bool is_continuation = (c & 0xC0) == 0x80; // продолжающий байт UTF-8 — не новый видимый символ
    if (!is_continuation && g_word_cols >= UART_TERM_WIDTH) {
        uart_flush_word(); // само "слово" длиннее ширины терминала — форсируем разрыв на границе символа
    }
    if (g_word_len >= UART_WORD_BUF_CAP - 1) {
        uart_flush_word(); // защита от переполнения буфера на аномально длинном "слове" без пробелов
    }
    g_word_buf[g_word_len++] = ch;
    if (!is_continuation) g_word_cols++;
}

int main(int argc, char *argv[]) {
    // 2. Достаем настоящий адрес буфера
    seL4_IPCBuffer *ipc = get_local_ipc();
    
    // 3. Отдаем его libsel4 (теперь её TLS инициализирован, и она сохранит его куда надо)
    seL4_SetIPCBuffer(ipc);

    // 2. Теперь безопасно получаем root_ep
    seL4_CPtr root_ep = ipc->msg[BOOT_ROOT_EP];
    // issuse.txt №74, часть "б" (адаптивный перенос драйверов между
    // ядрами) — общая страница состояния PARKED/BUSY, см.
    // h/driver_state.h. Инициализируем как можно раньше: до первого
    // seL4_Recv драйвер занят своим bring-up'ом, и root обязан это
    // видеть, иначе может счесть безопасным суспендить его посреди
    // инициализации железа.
    driver_state_init(PLAT_DRIVER_STATE_VADDR, 1, ipc->msg[BOOT_DRIVER_STATE_PRESENT]);
    seL4_CPtr my_ep   = ipc->msg[BOOT_CONSOLE_EP];
    seL4_CPtr irq_ep  = ipc->msg[BOOT_IRQ_EP];
    seL4_CPtr self_tcb = ipc->msg[BOOT_SELF_TCB_CAP]; // Фаза 6.1 (продолжение, см. ROADMAP.md)

    // Запрашиваем SHM для обратной совместимости
    seL4_SetMR(0, 107); // SYS_SHM_GET
    seL4_MessageInfo_t msg = seL4_MessageInfo_new(0, 0, 0, 1);
    seL4_Call(root_ep, msg);
    shm_vaddr = (char*)seL4_GetMR(0);

    if (my_ep == 0 || irq_ep == 0) {
        __assert_fail("Null Capability Detected in Driver Init!", __FILE__, __LINE__, __func__);
    }

    uart_io  = (volatile seL4_Uint32*)(PLAT_UART_VADDR + AUX_MU_IO_OFFSET);
    uart_lsr = (volatile seL4_Uint32*)(PLAT_UART_VADDR + AUX_MU_LSR_OFFSET);
    // issuse.txt №30 — kbd_buffer/head/tail теперь file-scope globals
    // (g_kbd_buffer/g_kbd_head/g_kbd_tail, см. их объявление выше) —
    // не передекларируем здесь, просто используем те же имена локально
    // через ссылки для минимального диффа в остальном коде функции.
    char (&kbd_buffer)[128] = g_kbd_buffer;
    int &head = g_kbd_head, &tail = g_kbd_tail;

    seL4_SetMR(0, SYS_DRIVER_READY);
    seL4_Call(root_ep, seL4_MessageInfo_new(0, 0, 0, 1));

    // Отложенный reply на SYS_READ (Фаза 4.5, см. ROADMAP.md): вместо
    // немедленного "-1, данных нет" и busy-poll на СТОРОНЕ ВЫЗЫВАЮЩЕГО
    // (shell.cpp::sys_read_blocking() крутил seL4_Yield() в цикле), теперь
    // не отвечаем на SYS_READ, если буфер пуст — сохраняем reply-cap
    // вызывающего (см. SELF_CNODE_SLOT в common.h) и достаём его из
    // ветки IRQ ниже, как только реально придёт символ. Только ОДИН
    // отложенный читатель одновременно — той же однопоточной модели, что и
    // раньше (kbd_buffer общий на все fd=0, конкурентных читателей не было
    // и до этой правки).
    constexpr seL4_Word UART_PENDING_REPLY_SLOT = 12;
    bool pending_reader = false;
    // issuse.txt: PID владельца отложенного SYS_READ — нужен для
    // SYS_CANCEL_PENDING_FOR_PID (см. common.h), иначе убитый/восстановленный
    // watchdog'ом читатель оставляет здесь reply-cap на несуществующий TCB,
    // и следующая же клавиша роняет "Attempted to invoke a null cap".
    seL4_Word pending_reader_pid = 0;

    while(1) {
        seL4_Word badge = 0;
        driver_state_parked(); // см. h/driver_state.h — с этой точки root вправе нас суспендить/переносить
        seL4_MessageInfo_t info = seL4_Recv(my_ep, &badge);
        driver_state_busy();   // ДО любых ветвлений/continue ниже

        if (badge == UART_KBD_IRQ_BADGE) {
            // Прерывание от клавиатуры — общий с uart_putc() drain (см.
            // issuse.txt №30 у объявления uart_drain_rx() выше). Обычно
            // здесь уже нечего вычитывать (uart_putc() успела всё забрать
            // по дороге) — цикл внутри просто сразу ничего не найдёт.
            uart_drain_rx();
            seL4_IRQHandler_Ack(irq_ep);

            if (pending_reader && head != tail) {
                seL4_SetMR(0, kbd_buffer[tail]); tail = (tail + 1) % 128;
                seL4_Send(UART_PENDING_REPLY_SLOT, seL4_MessageInfo_new(0, 0, 0, 1));
                // depth=8, не seL4_WordBits: SELF_CNODE_SLOT резолвится через
                // ГВАРДИРОВАННЫЙ корень треда (это отдельный механизм для
                // самого аргумента _service), а вот ВНУТРИ найденного CNode
                // (голый объект, скопированный как есть в main.cpp — без
                // гварда) слот 12 адресуется его РЕАЛЬНЫМ радиксом, 8 бит —
                // ровно как ROOT-задача всегда адресует child_cnode напрямую
                // (см. все seL4_CNode_Copy(child_cnode, ..., 8, ...) в
                // main.cpp). seL4_WordBits здесь резолвил не тот слот и ронял
                // "CNode operation: Target slot invalid" / "null cap #12" на
                // живом железе.
                seL4_CNode_Delete(SELF_CNODE_SLOT, UART_PENDING_REPLY_SLOT, 8);
                pending_reader = false;
            }
            // Не делаем 'continue', чтобы после IRQ тоже можно было сбросить буфер на печать
        } else {
            // Сообщение IPC от клиента. Badge - это PID отправителя.
            seL4_Word sender_pid = badge;
            seL4_Word sys = seL4_GetMR(0);

            // Фаза 5 плана "Сигналы драйверам" — проверяется БЕЗУСЛОВНО,
            // ДО проверки валидности sender_pid ниже (root форвардит через
            // свою badge=0 копию console_ep — та же ситуация, что уже
            // описана у SYS_BENCHMARK_*/SYS_CANCEL_PENDING_FOR_PID ниже, и
            // ДО stopped-гейта в самих ветках — сигнал обязан доходить,
            // даже если уже остановлен).
            if (sys == SYS_DRIVER_SIGNAL) {
                seL4_Word sig = seL4_GetMR(1);
                if (sig == DRIVER_SIGNAL_STOP) {
                    g_uart_stopped = true;
                } else if (sig == DRIVER_SIGNAL_START) {
                    g_uart_stopped = false;
                } else if (sig == DRIVER_SIGNAL_RESTART) {
                    // Заглушка (см. план) — uart_io/uart_lsr/IRQ-нотификация
                    // настроены один раз при спавне и не деградируют со
                    // временем, переинициализировать нечего; RESTART для
                    // uart_driver эквивалентен START.
                    g_uart_stopped = false;
                }
                seL4_SetMR(0, 0);
                seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                continue;
            }

            // SYS_BENCHMARK_RESET_LOCAL/FINALIZE_LOCAL — единственные команды,
            // которые шлёт САМ root (см. collect_load_snapshot() в main.cpp)
            // через свою НЕ-минченную копию console_ep (badge=0 — root держит
            // исходный объект от seL4_Untyped_Retype, а клиентам минтится
            // копия с badge=pid, см. main.cpp). Проверка "badge похож на
            // валидный PID" ниже такое отбраковывала — найдено по факту на
            // живом железе (`balance` перенёс uart_driver на пустое ядро,
            // `top` показал по нему 100% при 0% у самого процесса): root
            // получал в ответ пустое сообщение, ResetLog/FinalizeLog у
            // uart_driver ни разу реально не вызывались. Эти две команды не
            // трогают никакое pid-индексированное состояние — обходить
            // проверку для них безопасно. SYS_CANCEL_PENDING_FOR_PID — та же
            // история (тоже шлёт root через неминченную копию, см.
            // generic_recover_process()/common.h) — целевой PID передаётся
            // явно в MR1, а не через badge.
            if (sys != SYS_BENCHMARK_RESET_LOCAL && sys != SYS_BENCHMARK_FINALIZE_LOCAL && sys != SYS_CANCEL_PENDING_FOR_PID) {
                if (sender_pid <= 0 || sender_pid >= MAX_CLIENTS) {
                    // Невалидный PID, игнорируем
                    seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 0));
                    continue;
                }
            }

            if (sys == 8) { // SYS_PUTS
                int len = seL4_MessageInfo_get_length(info) - 1;

                // Фаза 5 — остановлен сигналом STOP: молча не пишем в
                // железо, но ВСЕГДА отвечаем (см. sys_write() — вызывающий
                // не проверяет статус, только ждёт сам факт ответа; без
                // Reply здесь он завис бы навсегда).
                if (g_uart_stopped) {
                    seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 0));
                    continue;
                }

                if (len > 0) {
                    // НОВЫЙ UNIX-WAY: Строка пришла в регистрах (от sys_write)
                    for (int i = 0; i < len; i++) {
                        uart_putc_wrapped((char)seL4_GetMR(i + 1));
                    }
                } else {
                    // СТАРЫЙ WAY: Строка лежит в разделяемой памяти (SHM)
                    // (Для обратной совместимости с драйверами, которые пишут по-старому)
                    if (shm_vaddr) {
                        char* str = (char*)shm_vaddr;
                        while (*str) {
                            uart_putc_wrapped(*str++);
                        }
                    }
                }
                // ВАЖНО: принудительно допечатываем накопленное "слово" в конце
                // КАЖДОГО отдельного SYS_PUTS-вызова, не дожидаясь пробела/новой
                // строки. Интерактивный ввод с клавиатуры шлёт по одному символу
                // за вызов (см. shell.cpp: эхо при наборе) — без этого сброса
                // символ молча оседал бы в буфере слова и не появлялся на экране,
                // пока не придёт пробел (именно так и было, когда этот сброс
                // отсутствовал — g_console_col/g_word_buf теперь ПЕРСИСТЕНТНЫ
                // между вызовами ради переноса многословных строк, но сам
                // буфер слова обязан опустошаться к концу каждого вызова,
                // как раньше он гарантированно опустошался в конце каждого
                // отдельного sys_puts() в shell.cpp).
                uart_flush_word();

                seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 0));
            } else if (sys == 9) { // SYS_FLUSH — issuse.txt №29: честный no-op, см. комментарий в начале файла
                seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 0));
            } else if (sys == 6) { // SYS_READ
                // Фаза 5 — остановлен: честное "нет данных" немедленно,
                // вместо откладывания reply на неопределённый срок (тот же
                // принцип, что у SYS_PUTS выше — не вешаем вызывающего).
                if (g_uart_stopped) {
                    seL4_SetMR(0, (seL4_Word)-1);
                    seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                    continue;
                }
                if (head != tail) {
                    seL4_SetMR(0, kbd_buffer[tail]); tail = (tail + 1) % 128;
                    seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                } else if (!pending_reader) {
                    // Нет данных — откладываем reply вместо "-1" (см. пометку
                    // про UART_PENDING_REPLY_SLOT выше по функции).
                    seL4_CNode_SaveCaller(SELF_CNODE_SLOT, UART_PENDING_REPLY_SLOT, 8); // depth=8, см. комментарий у seL4_CNode_Delete ниже
                    pending_reader = true;
                    pending_reader_pid = sender_pid;
                } else {
                    // Уже есть отложенный читатель (см. комментарий выше про
                    // единственность) — не зависаем, отвечаем сразу "нет данных".
                    seL4_SetMR(0, (seL4_Word)-1);
                    seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                }
            } else if (sys == SYS_BENCHMARK_RESET_LOCAL) { // Фаза 6.1 (продолжение, см. ROADMAP.md)
                seL4_BenchmarkResetLog();
                seL4_SetMR(0, 0);
                seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
            } else if (sys == SYS_BENCHMARK_FINALIZE_LOCAL) { // пара к RESET выше, см. h/common.h
                seL4_BenchmarkFinalizeLog();
                seL4_BenchmarkGetThreadUtilisation(self_tcb);
                seL4_Word idle_local = seL4_GetMR(4);  // BENCHMARK_IDLE_LOCALCPU_UTILISATION
                seL4_Word total_local = seL4_GetMR(9); // BENCHMARK_TOTAL_UTILISATION
                seL4_SetMR(0, idle_local);
                seL4_SetMR(1, total_local);
                seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 2));
            } else if (sys == SYS_CANCEL_PENDING_FOR_PID) {
                // issuse.txt: root шлёт это ПЕРЕД тем, как окончательно
                // убрать жертву (kill/watchdog) — если это она сейчас
                // отложенный читатель, отбрасываем слот молча (без
                // seL4_Send — TCB жертвы уже не существует или вот-вот
                // перестанет, отвечать некому).
                seL4_Word target_pid = seL4_GetMR(1);
                if (pending_reader && pending_reader_pid == target_pid) {
                    seL4_CNode_Delete(SELF_CNODE_SLOT, UART_PENDING_REPLY_SLOT, 8);
                    pending_reader = false;
                }
                seL4_SetMR(0, 0);
                seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
            } else {
                seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 0));
            }
        }
    }

    return 0;
}