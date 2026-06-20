#include "pipe.h"

// ИСПРАВЛЕНО: Non-Atomic Shared Memory Access
// Использование встроенных атомарных операций GCC/Clang для lock-free структур

void pipe_init(volatile ipc_pipe* pipe) {
    __atomic_store_n(&pipe->head, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&pipe->tail, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&pipe->writer_closed, false, __ATOMIC_RELEASE);
    __atomic_store_n(&pipe->reader_closed, false, __ATOMIC_RELEASE);
}

int pipe_write(volatile ipc_pipe* pipe, seL4_CPtr write_notify_ep, seL4_CPtr read_notify_ep, const uint8_t* buf, int len) {
    int written = 0;
    while (written < len) {
        if (__atomic_load_n(&pipe->reader_closed, __ATOMIC_ACQUIRE)) return -1;

        int current_head = __atomic_load_n(&pipe->head, __ATOMIC_RELAXED);
        int current_tail = __atomic_load_n(&pipe->tail, __ATOMIC_ACQUIRE); // Барьер чтения

        while (((current_head + 1) % PIPE_BUF_SIZE) == current_tail) {
            if (write_notify_ep != 0) {
                seL4_Wait(write_notify_ep, nullptr);
                current_tail = __atomic_load_n(&pipe->tail, __ATOMIC_ACQUIRE);
            } else {
                break; 
            }
        }

        pipe->data[current_head] = buf[written++];
        // Барьер записи: данные в массиве data гарантированно сохранятся до обновления head
        __atomic_store_n(&pipe->head, (current_head + 1) % PIPE_BUF_SIZE, __ATOMIC_RELEASE);

        if (read_notify_ep != 0) seL4_Signal(read_notify_ep);
    }
    return written;
}

int pipe_read(volatile ipc_pipe* pipe, seL4_CPtr read_notify_ep, seL4_CPtr write_notify_ep, uint8_t* buf, int len) {
    int bytes_read = 0;
    while (bytes_read < len) {
        int current_head = __atomic_load_n(&pipe->head, __ATOMIC_ACQUIRE); // Барьер чтения
        int current_tail = __atomic_load_n(&pipe->tail, __ATOMIC_RELAXED);

        while (current_head == current_tail) {
            if (__atomic_load_n(&pipe->writer_closed, __ATOMIC_ACQUIRE)) {
                return bytes_read; 
            }
            if (read_notify_ep != 0) {
                seL4_Wait(read_notify_ep, nullptr);
                current_head = __atomic_load_n(&pipe->head, __ATOMIC_ACQUIRE);
            } else {
                return bytes_read; 
            }
        }

        buf[bytes_read++] = pipe->data[current_tail];
        // Барьер записи
        __atomic_store_n(&pipe->tail, (current_tail + 1) % PIPE_BUF_SIZE, __ATOMIC_RELEASE);

        if (write_notify_ep != 0) seL4_Signal(write_notify_ep);
    }
    return bytes_read;
}