#include "pipe.h"

void pipe_init(volatile ipc_pipe* pipe) {
    pipe->head = 0;
    pipe->tail = 0;
    pipe->writer_closed = false;
    pipe->reader_closed = false;
}

int pipe_write(volatile ipc_pipe* pipe, seL4_CPtr write_notify_ep, seL4_CPtr read_notify_ep, const uint8_t* buf, int len) {
    int written = 0;
    while (written < len) {
        if (pipe->reader_closed) return -1; // Ошибка: читать больше некому

        // Если буфер полон (следующий шаг head упрется в tail)
        while (((pipe->head + 1) % PIPE_BUF_SIZE) == pipe->tail) {
            if (write_notify_ep != 0) {
                seL4_Wait(write_notify_ep, nullptr);
            } else {
                break; // Защита: в однопоточном режиме просто выходим, чтобы не зависнуть
            }
        }

        pipe->data[pipe->head] = buf[written++];
        pipe->head = (pipe->head + 1) % PIPE_BUF_SIZE;

        // Будим Читателя (сообщаем, что есть новые данные)
        if (read_notify_ep != 0) {
            seL4_Signal(read_notify_ep);
        }
    }
    return written;
}

int pipe_read(volatile ipc_pipe* pipe, seL4_CPtr read_notify_ep, seL4_CPtr write_notify_ep, uint8_t* buf, int len) {
    int bytes_read = 0;
    while (bytes_read < len) {
        // Если буфер пуст
        while (pipe->head == pipe->tail) {
            if (pipe->writer_closed) {
                return bytes_read; // Конец файла (Писатель закончил)
            }
            if (read_notify_ep != 0) {
                seL4_Wait(read_notify_ep, nullptr);
            } else {
                // Защита для однопоточного режима: если буфер пуст и спать нельзя - выходим
                return bytes_read; 
            }
        }

        buf[bytes_read++] = pipe->data[pipe->tail];
        pipe->tail = (pipe->tail + 1) % PIPE_BUF_SIZE;

        // Будим Писателя (сообщаем, что освободили место)
        if (write_notify_ep != 0) {
            seL4_Signal(write_notify_ep);
        }
    }
    return bytes_read;
}