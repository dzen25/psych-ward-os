#pragma once
#include <stdint.h>
#include <sel4/sel4.h>

#define PIPE_BUF_SIZE 4096

// Эта структура должна лежать в Shared Memory!
struct ipc_pipe {
    volatile uint32_t head;       // Индекс записи (Писатель)
    volatile uint32_t tail;       // Индекс чтения (Читатель)
    volatile bool writer_closed;  // Флаг EOF (конец файла/потока)
    volatile bool reader_closed;  // Флаг SIGPIPE (читатель умер)
    uint8_t data[PIPE_BUF_SIZE];
};

// Объявления функций
void pipe_init(volatile ipc_pipe* pipe);
int pipe_write(volatile ipc_pipe* pipe, seL4_CPtr write_notify_ep, seL4_CPtr read_notify_ep, const uint8_t* buf, int len);
int pipe_read(volatile ipc_pipe* pipe, seL4_CPtr read_notify_ep, seL4_CPtr write_notify_ep, uint8_t* buf, int len);