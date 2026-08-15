#include <sel4/sel4.h>
#include "h/common.h"

// Эта функция вызывается макросом assert() при ошибке.
// Так как мы не линкуем стандартную библиотеку, нам нужно определить её самим.
void __assert_fail(const char *assertion, const char *file, int line, const char *function) {
    while(1);
}

static void sys_puts(seL4_CPtr console_ep, const char *str) {
    seL4_IPCBuffer *ipc = (seL4_IPCBuffer *)seL4_GetIPCBuffer();
    int total_len = 0;
    while(str[total_len]) total_len++;
    
    int offset = 0;
    while (offset < total_len) {
        int chunk = total_len - offset;
        // Строгий лимит IPC Message Registers для ARM64 (оставляем запас безопасности)
        if (chunk > 100) chunk = 100; 
        
        ipc->msg[0] = 8; // SYS_PUTS ID
        for (int i = 0; i < chunk; i++) {
            ipc->msg[i + 1] = str[offset + i];
        }
        seL4_Call(console_ep, seL4_MessageInfo_new(0, 0, 0, chunk + 1));
        offset += chunk;
    }
}

static inline seL4_IPCBuffer* get_local_ipc() {
    seL4_Word tls_addr;
    asm volatile("mrs %0, tpidrro_el0" : "=r"(tls_addr)); 
    return (seL4_IPCBuffer*)(tls_addr - 1024);
}

static void sys_exit(seL4_CPtr root_ep) {
    seL4_IPCBuffer *ipc = get_local_ipc();
    ipc->msg[0] = 103; // SYS_EXIT
    seL4_Call(root_ep, seL4_MessageInfo_new(0, 0, 0, 1));
    while(1) seL4_Yield(); // Сюда выполнение никогда не дойдет
}

// Фаза 5.4 — узкий файловый доступ для exec-процессов (это ОНИ и есть,
// is_driver=254): путь/данные идут прямо в MR, никакого SHM не требуется
// (см. h/common.h SYS_PROXY_READ_FILE/WRITE_FILE, main.cpp — обработчик).
static seL4_Word sys_proxy_write_file(seL4_CPtr root_ep, const char* path, const char* data, uint32_t data_len) {
    seL4_IPCBuffer *ipc = get_local_ipc();
    uint32_t path_len = 0; while (path[path_len] && path_len < 63) path_len++;
    // issuse.txt №60: msg[] вмещает ровно 120 слов (seL4_MsgMaxLength), а
    // msg[0..2] уже заняты опкодом/path_len/data_len — 3+path_len+data_len
    // не должно уходить дальше msg[119], иначе запись (и следом сама длина
    // сообщения) уезжает в соседние поля seL4_IPCBuffer за msg[].
    uint32_t max_data_len = (117 > path_len) ? (117 - path_len) : 0;
    if (data_len > max_data_len) data_len = max_data_len;

    ipc->msg[0] = 136; // SYS_PROXY_WRITE_FILE
    ipc->msg[1] = path_len;
    ipc->msg[2] = data_len;
    for (uint32_t i = 0; i < path_len; i++) ipc->msg[3 + i] = (seL4_Word)(uint8_t)path[i];
    for (uint32_t i = 0; i < data_len; i++) ipc->msg[3 + path_len + i] = (seL4_Word)(uint8_t)data[i];
    seL4_Call(root_ep, seL4_MessageInfo_new(0, 0, 0, 3 + path_len + data_len));
    return seL4_GetMR(0);
}

static int32_t sys_proxy_read_file(seL4_CPtr root_ep, const char* path, uint32_t offset, char* out_buf, uint32_t max_len) {
    seL4_IPCBuffer *ipc = get_local_ipc();
    uint32_t path_len = 0; while (path[path_len] && path_len < 63) path_len++;

    ipc->msg[0] = 135; // SYS_PROXY_READ_FILE
    ipc->msg[1] = path_len;
    ipc->msg[2] = offset;
    for (uint32_t i = 0; i < path_len; i++) ipc->msg[3 + i] = (seL4_Word)(uint8_t)path[i];
    seL4_Call(root_ep, seL4_MessageInfo_new(0, 0, 0, 3 + path_len));

    seL4_Word status = seL4_GetMR(0);
    uint32_t bytes_read = (uint32_t)seL4_GetMR(1);
    if (status != 0) return -1;
    if (bytes_read > max_len) bytes_read = max_len;
    for (uint32_t i = 0; i < bytes_read; i++) out_buf[i] = (char)seL4_GetMR(2 + i);
    return (int32_t)bytes_read;
}

// Негативная проверка Фазы 5.2/5.4: exec-процессы по-прежнему НЕ должны
// получать ни одной страницы разделяемой памяти напрямую через сырой
// SYS_SHM_GET — только через узкий прокси-протокол выше. Успешный вызов
// возвращает ненулевой vaddr в MR0; 0 означает отказ (fail-closed).
static seL4_Word sys_raw_shm_get_probe(seL4_CPtr root_ep) {
    seL4_IPCBuffer *ipc = get_local_ipc();
    ipc->msg[0] = 107; // SYS_SHM_GET
    seL4_Call(root_ep, seL4_MessageInfo_new(0, 0, 0, 1));
    return seL4_GetMR(0);
}

static void sys_putdec(seL4_CPtr console_ep, seL4_Word val) {
    char buf[21]; int j = 0;
    if (val == 0) buf[j++] = '0';
    while (val > 0) { buf[j++] = (char)('0' + (val % 10)); val /= 10; }
    while (j > 0) { char c[2] = { buf[--j], '\0' }; sys_puts(console_ep, c); }
}

// Теперь мы используем стандартный main!
int main(int argc, char *argv[]) {
    // 1. Получаем IPC-буфер и инициализируем libsel4, как и другие процессы
    seL4_IPCBuffer *ipc = get_local_ipc();
    seL4_SetIPCBuffer(ipc);

    // 2. Достаем Endpoint консоли из стартового сообщения от rootserver
    seL4_CPtr console_ep = ipc->msg[BOOT_CONSOLE_EP];
    seL4_CPtr root_ep = ipc->msg[BOOT_ROOT_EP];

    // 3. Печатаем баннер. С новым мультиплексором в uart_driver, гонки больше нет.
    const char* banner = 
        "\n======================================\n"
        "  SUCCESS: HELLO FROM FAT32 64 DISK!!!\n"
        "  (Standard main() execution)\n"
        "======================================\n\n";
    sys_puts(console_ep, banner);

    // 4. Фаза 5.4: проверка VFS-прокси через root (см. h/common.h
    // SYS_PROXY_READ_FILE/WRITE_FILE, main.cpp) — этот процесс (is_driver=254)
    // не имеет ни одной страницы SHM (Фаза 5.2), весь файловый доступ идёт
    // через узкий syscall-прокси, путь/данные прямо в MR.
    const char* test_path = "/root/exectest.txt";
    const char* test_data = "hello from exec proxy";
    uint32_t test_len = 0; while (test_data[test_len]) test_len++;

    seL4_Word wr_status = sys_proxy_write_file(root_ep, test_path, test_data, test_len);
    sys_puts(console_ep, "[TEST] proxy write status: "); sys_putdec(console_ep, wr_status); sys_puts(console_ep, "\n");

    char read_buf[128];
    int32_t rd_len = sys_proxy_read_file(root_ep, test_path, 0, read_buf, sizeof(read_buf) - 1);
    if (rd_len >= 0) {
        read_buf[rd_len] = '\0';
        sys_puts(console_ep, "[TEST] proxy read back (" );
        sys_putdec(console_ep, (seL4_Word)rd_len);
        sys_puts(console_ep, " bytes): \"");
        sys_puts(console_ep, read_buf);
        sys_puts(console_ep, "\"\n");

        bool matches = ((uint32_t)rd_len == test_len);
        for (int32_t i = 0; matches && i < rd_len; i++) if (read_buf[i] != test_data[i]) matches = false;
        sys_puts(console_ep, matches ? "[TEST] write/read roundtrip: MATCH\n" : "[TEST] write/read roundtrip: MISMATCH\n");
    } else {
        sys_puts(console_ep, "[TEST] proxy read FAILED\n");
    }

    // 5. Негативная проверка: сырой SYS_SHM_GET должен по-прежнему давать 0
    // страниц (fail-closed, Фаза 5.2) — этот процесс не должен получить
    // никакого прямого доступа к разделяемой памяти в обход прокси выше.
    seL4_Word raw_shm_vaddr = sys_raw_shm_get_probe(root_ep);
    sys_puts(console_ep, raw_shm_vaddr == 0
        ? "[TEST] raw SYS_SHM_GET: correctly got 0 (fail-closed intact)\n"
        : "[TEST] raw SYS_SHM_GET: UNEXPECTED non-zero vaddr — fail-closed BROKEN\n");

    // 6. Завершаем процесс, чтобы вернуть управление оболочке
    sys_exit(root_ep);

    return 0;
}