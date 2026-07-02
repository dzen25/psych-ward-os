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

// Глобальные указатели на регистры для функции flush_buffer
static volatile seL4_Uint32 *uart_dr = nullptr;
static volatile seL4_Uint32 *uart_fr = nullptr;
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
        while ((*uart_fr) & PL011_FR_TXFF);
        *uart_dr = '\r';
    }
    while ((*uart_fr) & PL011_FR_TXFF);
    *uart_dr = c;
}

static void flush_buffer() {
    // Записываем столько, сколько влезает в FIFO прямо сейчас.
    // Эта операция неблокирующая: если FIFO полон, цикл немедленно
    // завершится, и драйвер вернется к ожиданию новых событий.
    // Остаток данных будет отправлен на следующей итерации.
    while (tx_tail != tx_head && (((*uart_fr) & PL011_FR_TXFF) == 0)) {
        *uart_dr = tx_buffer[tx_tail];
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

    uart_dr = (volatile seL4_Uint32*)(PLAT_UART_VADDR + PL011_DR_OFFSET);
    uart_fr = (volatile seL4_Uint32*)(PLAT_UART_VADDR + PL011_FR_OFFSET);
    char kbd_buffer[128]; int head = 0, tail = 0;

    seL4_SetMR(0, SYS_DRIVER_READY);
    seL4_Call(root_ep, seL4_MessageInfo_new(0, 0, 0, 1));

    while(1) {
        seL4_Word badge = 0;
        seL4_MessageInfo_t info = seL4_Recv(my_ep, &badge);

        if (badge == 1) {
            // Прерывание от клавиатуры
            while (((*uart_fr) & PL011_FR_RXFE) == 0) {
                char c = *uart_dr;
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
                        // Выводим каждый символ напрямую в физический UART
                        uart_putc((char)seL4_GetMR(i + 1));
                    }
                } else {
                    // СТАРЫЙ WAY: Строка лежит в разделяемой памяти (SHM)
                    // (Для обратной совместимости с драйверами, которые пишут по-старому)
                    if (shm_vaddr) {
                        char* str = (char*)shm_vaddr;
                        while (*str) {
                            uart_putc(*str++);
                        }
                    }
                }
                
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
                if (head != tail) { seL4_SetMR(0, kbd_buffer[tail]); tail = (tail + 1) % 128; }
                else { seL4_SetMR(0, (seL4_Word)-1); }
                seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
            } else {
                seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 0));
            }
        }

        flush_buffer();
    }

    return 0;
}