#pragma once
#include "common.h"

void uart_init(void *vaddr);
void uart_putchar(char c);
int uart_havechar();
char uart_getchar();
void uart_puts(const char *s);
void uart_puthex(uint64_t val);

// Функции для управления прерываниями UART
void uart_enable_interrupts();
void uart_clear_interrupts();

int my_strcmp(const char *s1, const char *s2);
int my_strlen(const char *s);
int my_isspace(char c);
void handle_command(char *cmd);

// ИСПРАВЛЕНИЕ: Обновленная сигнатура, принимает 4 аргумента
void read_line(char *buf, int max_len, seL4_CPtr irq_ntfn, seL4_CPtr irq_handler);