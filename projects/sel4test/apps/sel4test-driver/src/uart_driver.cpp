#include <sel4/sel4.h>
#include "h/common.h"
#include "h/platform.h"
#include <stdint.h>

// ИСПРАВЛЕНО: Переменные для очереди вывода вынесены в глобальную область видимости файла.
// Общий буфер для асинхронной отправки в железо
#define TX_BUFFER_SIZE 4096
static char tx_buffer[TX_BUFFER_SIZE];
static volatile int tx_head = 0;
static volatile int tx_tail = 0;

// ИДЕАЛЬНОЕ РЕШЕНИЕ: Буферы для каждой строки от каждого процесса (мультиплексирование)
#define MAX_CLIENTS 256
#define LINE_BUFFER_SIZE 256
static char line_buffers[MAX_CLIENTS][LINE_BUFFER_SIZE];
static int line_buffer_pos[MAX_CLIENTS] = {0};

// Глобальные указатели на регистры для функции flush_buffer (mini-UART, см. platform.h)
static volatile seL4_Uint32 *uart_io = nullptr;
static volatile seL4_Uint32 *uart_lsr = nullptr;
static char* shm_vaddr = nullptr;

static inline seL4_IPCBuffer* get_local_ipc() {
    seL4_Word tls_addr;
    // Добавлена буква 'ro'. crt0 не мог его стереть!
    asm volatile("mrs %0, tpidrro_el0" : "=r"(tls_addr)); 
    return (seL4_IPCBuffer*)(tls_addr - 1024);
}

void __assert_fail(const char *assertion, const char *file, int line, const char *function) { while(1); }

static void uart_putc(char c) {
    if (c == '\n') {
        while (!((*uart_lsr) & AUX_MU_LSR_TX_EMPTY));
        *uart_io = '\r';
    }
    while (!((*uart_lsr) & AUX_MU_LSR_TX_EMPTY));
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

static void flush_buffer() {
    // Записываем столько, сколько влезает в FIFO прямо сейчас.
    // Эта операция неблокирующая: если FIFO полон, цикл немедленно
    // завершится, и драйвер вернется к ожиданию новых событий.
    // Остаток данных будет отправлен на следующей итерации.
    while (tx_tail != tx_head && ((*uart_lsr) & AUX_MU_LSR_TX_EMPTY)) {
        *uart_io = tx_buffer[tx_tail];
        tx_tail = (tx_tail + 1) % TX_BUFFER_SIZE;
    }
}

int main(int argc, char *argv[]) {
    // 2. Достаем настоящий адрес буфера
    seL4_IPCBuffer *ipc = get_local_ipc();
    
    // 3. Отдаем его libsel4 (теперь её TLS инициализирован, и она сохранит его куда надо)
    seL4_SetIPCBuffer(ipc);

    // 2. Теперь безопасно получаем root_ep
    seL4_CPtr root_ep = ipc->msg[BOOT_ROOT_EP];
    seL4_CPtr my_ep   = ipc->msg[BOOT_CONSOLE_EP];
    seL4_CPtr irq_ep  = ipc->msg[BOOT_IRQ_EP];

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
    char kbd_buffer[128]; int head = 0, tail = 0;

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

    while(1) {
        seL4_Word badge = 0;
        seL4_MessageInfo_t info = seL4_Recv(my_ep, &badge);

        if (badge == 1) {
            // Прерывание от клавиатуры
            while ((*uart_lsr) & AUX_MU_LSR_RX_READY) {
                char c = *uart_io;
                int next_head = (head + 1) % 128;
                if (next_head == tail) {
                    // Буфер полон — читатель не успевает вычитывать. Отбрасываем
                    // символ вместо того, чтобы затирать непрочитанные данные и
                    // рассинхронизировать head/tail.
                    break;
                }
                kbd_buffer[head] = c; head = next_head;
            }
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
            if (sender_pid <= 0 || sender_pid >= MAX_CLIENTS) {
                // Невалидный PID, игнорируем
                seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 0));
                continue;
            }

            seL4_Word sys = seL4_GetMR(0);
            if (sys == 8) { // SYS_PUTS
                int len = seL4_MessageInfo_get_length(info) - 1;
                
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
            } else if (sys == 9) { // SYS_FLUSH 
                for (int i = 0; i < line_buffer_pos[sender_pid]; i++) {
                    int next_head = (tx_head + 1) % TX_BUFFER_SIZE;
                    if (next_head == tx_tail) break; 
                    tx_buffer[tx_head] = line_buffers[sender_pid][i];
                    tx_head = next_head;
                }
                line_buffer_pos[sender_pid] = 0;
                seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 0));
            } else if (sys == 6) { // SYS_READ
                if (head != tail) {
                    seL4_SetMR(0, kbd_buffer[tail]); tail = (tail + 1) % 128;
                    seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                } else if (!pending_reader) {
                    // Нет данных — откладываем reply вместо "-1" (см. пометку
                    // про UART_PENDING_REPLY_SLOT выше по функции).
                    seL4_CNode_SaveCaller(SELF_CNODE_SLOT, UART_PENDING_REPLY_SLOT, 8); // depth=8, см. комментарий у seL4_CNode_Delete ниже
                    pending_reader = true;
                } else {
                    // Уже есть отложенный читатель (см. комментарий выше про
                    // единственность) — не зависаем, отвечаем сразу "нет данных".
                    seL4_SetMR(0, (seL4_Word)-1);
                    seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
                }
            } else {
                seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 0));
            }
        }

        flush_buffer();
    }

    return 0;
}