#include "h/uart.h"
#include "h/hw_timer.h"
#include "h/platform.h"

volatile uint32_t *uart_io;   // AUX_MU_IO — TX при записи, RX при чтении
volatile uint32_t *uart_lsr;  // AUX_MU_LSR — статусные биты (готовность RX/TX)
volatile uint32_t *uart_ier;  // AUX_MU_IER — маска прерываний
volatile uint32_t *uart_iir;  // AUX_MU_IIR — идентификация/сброс прерывания

void uart_init(void *vaddr) {
    volatile uint32_t *aux_enables = (volatile uint32_t*)((char*)vaddr + AUX_ENABLES_OFFSET);
    uart_io  = (volatile uint32_t*)((char*)vaddr + AUX_MU_IO_OFFSET);
    uart_ier = (volatile uint32_t*)((char*)vaddr + AUX_MU_IER_OFFSET);
    uart_iir = (volatile uint32_t*)((char*)vaddr + AUX_MU_IIR_OFFSET);
    uart_lsr = (volatile uint32_t*)((char*)vaddr + AUX_MU_LSR_OFFSET);
    volatile uint32_t *uart_lcr  = (volatile uint32_t*)((char*)vaddr + AUX_MU_LCR_OFFSET);
    volatile uint32_t *uart_cntl = (volatile uint32_t*)((char*)vaddr + AUX_MU_CNTL_OFFSET);

    // mini-UART уже включена и настроена прошивкой (baud рег. НЕ трогаем —
    // тактуется от VPU-ядра, риск неверно посчитать делитель; см. platform.h).
    // Но явно включаем AUX/8-бит/RX/TX на случай, если это первый доступ —
    // идемпотентно, не сбивает уже работающую конфигурацию прошивки/ядра.
    *aux_enables |= AUX_ENABLES_UART;
    *uart_lcr = AUX_MU_LCR_8BIT;
    *uart_cntl = AUX_MU_CNTL_RX_EN | AUX_MU_CNTL_TX_EN;
}

void uart_enable_interrupts() {
    *uart_ier |= AUX_MU_IER_RX_INT;
}

void uart_clear_interrupts() {
    // У mini-UART нет отдельного "write 1 to clear" регистра как ICR у
    // PL011 — запись в IIR сбрасывает FIFO/подтверждает прерывание.
    *uart_iir = 0;
}

void uart_putchar(char c) {
    while (!((*uart_lsr) & AUX_MU_LSR_TX_EMPTY));
    *uart_io = c;
}

int uart_havechar() {
    return (*uart_lsr) & AUX_MU_LSR_RX_READY;
}

char uart_getchar() {
    while (!uart_havechar()) seL4_Yield();
    return (char)(*uart_io & 0xFF);
}

void uart_puts(const char *s) {
    while (*s) {
        if (*s == '\n') uart_putchar('\r');
        uart_putchar(*s++);
    }
}

void uart_puthex(uint64_t val) {
    const char hex[] = "0123456789abcdef";
    char buf[17];
    buf[16] = 0;
    for (int i = 15; i >= 0; i--) {
        buf[i] = hex[val & 0xF];
        val >>= 4;
    }
    uart_puts(buf);
}

int my_strcmp(const char *s1, const char *s2) {
    while (*s1 && (*s1 == *s2)) {
        s1++;
        s2++;
    }
    return *(const unsigned char*)s1 - *(const unsigned char*)s2;
}

int my_strlen(const char *s) {
    int len = 0;
    while (*s++) len++;
    return len;
}

int my_isspace(char c) {
    return (c == ' ' || c == '\t' || c == '\n' || c == '\r');
}

void handle_command(char *cmd) {
    int len = my_strlen(cmd);
    while (len > 0 && my_isspace(cmd[len-1])) {
        cmd[len-1] = 0;
        len--;
    }
    if (len == 0) return;

    if (my_strcmp(cmd, "help") == 0) {
        uart_puts("\nAvailable commands:\n");
        uart_puts("  help   - show this help\n");
        uart_puts("  hello  - greet the system\n");
        uart_puts("  status - show system status\n");
        uart_puts("  sleep  - busy-wait for ~1 second\n");
        uart_puts("  panic  - trigger system panic\n");
    } else if (my_strcmp(cmd, "hello") == 0) {
        uart_puts("\nHello from Psych Ward OS!\n");
    } else if (my_strcmp(cmd, "status") == 0) {
        uart_puts("\n--- System Status ---\n");
        uart_puts("UART: mapped at 0x200000000\n");
        uart_puts("Timer freq: ");
        uart_puthex(32768); // Заглушка (этот built-in "status" не читает реальный CNTFRQ_EL0)
        uart_puts(" Hz\n");
        uart_puts("----------------------\n");
    } else if (my_strcmp(cmd, "sleep") == 0) {
        uart_puts("\nSleep is now handled in user-space shell via SYS_SLEEP.\n");
    } else if (my_strcmp(cmd, "panic") == 0) {
        __assert_fail("User requested lockdown", "uart.cpp", __LINE__, __func__);
    } else {
        uart_puts("\nUnknown command\n");
    }
}

void read_line(char *buf, int max_len, seL4_CPtr irq_ntfn, seL4_CPtr irq_handler) {
    int pos = 0;
    while (1) {
        // Засыпаем и ждем прерывания, если в буфере UART нет символов
        while (!uart_havechar()) {
            seL4_Word badge;
            seL4_Wait(irq_ntfn, &badge); // Поток спит, 0% CPU
            
            uart_clear_interrupts();     // Говорим железу (PL011), что прерывание обслужено
            seL4_IRQHandler_Ack(irq_handler); // Говорим ядру seL4, что мы готовы к новым IRQ
        }

        char c = uart_getchar();
        if (c == '\r' || c == '\n') {
            uart_putchar('\r');
            uart_putchar('\n');
            buf[pos] = 0;
            return;
        } else if (c == '\b' || c == 127) {
            if (pos > 0) {
                pos--;
                uart_puts("\b \b");
            }
        } else if (pos < max_len - 1 && c >= 32 && c <= 126) {
            buf[pos++] = c;
            uart_putchar(c);
        }
    }
}