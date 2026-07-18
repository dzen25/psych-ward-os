// net_driver.cpp
#include <sel4/sel4.h>
#include "h/common.h"
#include "h/platform.h"
#include <stdint.h>
#include <kernel/gen_config.h>


void __assert_fail(const char *expr, const char *file, int line, const char *func) { while (1) seL4_Yield(); }

static int my_strlen(const char* s) { int len = 0; while (s[len]) len++; return len; }

// Прямое чтение аппаратного счётчика ARM generic timer (см. hw_timer.cpp/
// timer_driver.cpp — тот же приём, EXPORT_VCNT_USER=true в gen_config
// разрешает mrs с EL0 без трапа в ядро). Используется ТОЛЬКО для измерения
// RTT пинга — sys_get_uptime_ms() через IPC даёт лишь целые миллисекунды
// (разрешение timer_driver'а), чего мало для LAN-хостов с реальным RTT
// в десятые доли миллисекунды — там показывалось "0.000 ms"/"1.000 ms" для
// чего угодно быстрее 1мс. Здесь же — честные микросекунды без похода по IPC.
static inline uint64_t read_cntvct() {
    uint64_t val;
    asm volatile("mrs %0, cntvct_el0" : "=r"(val));
    return val;
}
static inline uint64_t read_cntfrq() {
    uint64_t val;
    asm volatile("mrs %0, cntfrq_el0" : "=r"(val));
    return val;
}
static uint64_t g_cntfrq = 0; // читается лениво один раз при первом использовании

static inline seL4_IPCBuffer* get_local_ipc() {
    seL4_Word tls_addr;
    // Добавлена буква 'ro'. crt0 не мог его стереть!
    asm volatile("mrs %0, tpidrro_el0" : "=r"(tls_addr)); 
    return (seL4_IPCBuffer*)(tls_addr - 1024);
}

// --- Динамические адреса разделяемой памяти ---
static char* g_shm_vaddr = nullptr;
static uint32_t g_shm_paddr = 0;

// Пример правильного sys_puts для драйвера:
static void sys_puts(seL4_CPtr console_ep, const char *str) {
    seL4_IPCBuffer *ipc = get_local_ipc();
    int len = 0;
    while(str[len]) len++;
    
    ipc->msg[0] = 8; // SYS_PUTS
    for (int i = 0; i < len; i++) {
        ipc->msg[i + 1] = str[i];
    }
    seL4_Call(console_ep, seL4_MessageInfo_new(0, 0, 0, len + 1));
}

static void put_hex_byte(seL4_CPtr ep, uint8_t val) {
    char hex_chars[] = "0123456789ABCDEF"; char buf[3];
    buf[0] = hex_chars[(val >> 4) & 0xF]; buf[1] = hex_chars[val & 0xF]; buf[2] = '\0';
    sys_puts(ep, buf);
}

static void my_memcpy(void *dest, const void *src, int n) {
    unsigned char *d = (unsigned char *)dest; const unsigned char *s = (const unsigned char *)src;
    while (n--) *d++ = *s++;
}

static uint16_t htons(uint16_t hostshort) { return ((hostshort >> 8) & 0xFF) | ((hostshort & 0xFF) << 8); }

// Симметричная операция (network <-> host), как и htons, но для 32-битных полей NTP-таймстампов.
static uint32_t bswap32(uint32_t v) {
    return ((v & 0x000000FFu) << 24) | ((v & 0x0000FF00u) << 8) |
           ((v & 0x00FF0000u) >> 8) | ((v & 0xFF000000u) >> 24);
}

static uint64_t sys_get_time_ms(seL4_CPtr timer_ep) {
    seL4_SetMR(0, 3); // SYS_GET_TIME
    seL4_Call(timer_ep, seL4_MessageInfo_new(0, 0, 0, 1));
    return (uint64_t)seL4_GetMR(0);
}

// Мс с момента запуска timer_driver — в отличие от sys_get_time_ms(), не сбивается
// NTP-коррекцией. Используется для планирования периодической ресинхронизации.
static uint64_t sys_get_uptime_ms(seL4_CPtr timer_ep) {
    seL4_SetMR(0, 4); // SYS_GET_UPTIME
    seL4_Call(timer_ep, seL4_MessageInfo_new(0, 0, 0, 1));
    return (uint64_t)seL4_GetMR(0);
}

static void put_dec(seL4_CPtr ep, uint32_t val) {
    char buf[12];
    int i = 11;
    buf[i] = '\0';
    if (val == 0) { sys_puts(ep, "0"); return; }
    while (val > 0 && i > 0) {
        buf[--i] = (char)('0' + (val % 10));
        val /= 10;
    }
    sys_puts(ep, &buf[i]);
}

static void put_u64(seL4_CPtr ep, uint64_t val) {
    char buf[24];
    int i = 23;
    buf[i] = '\0';
    if (val == 0) { sys_puts(ep, "0"); return; }
    while (val > 0 && i > 0) {
        buf[--i] = (char)('0' + (val % 10));
        val /= 10;
    }
    sys_puts(ep, &buf[i]);
}

static void put_three_digits(seL4_CPtr ep, uint32_t val) {
    char buf[4];
    if (val > 999) val = 999;
    buf[0] = (char)('0' + (val / 100));
    buf[1] = (char)('0' + ((val / 10) % 10));
    buf[2] = (char)('0' + (val % 10));
    buf[3] = '\0';
    sys_puts(ep, buf);
}

static void put_duration_us(seL4_CPtr ep, uint64_t us) {
    put_u64(ep, us / 1000ULL);
    sys_puts(ep, ".");
    put_three_digits(ep, (uint32_t)(us % 1000ULL));
    sys_puts(ep, " ms");
}

static void put_ip(seL4_CPtr ep, const uint8_t ip[4]) {
    for (int i = 0; i < 4; i++) {
        if (i > 0) sys_puts(ep, ".");
        put_dec(ep, ip[i]);
    }
}

static uint16_t calculate_checksum(void* vdata, uint32_t length) {
    uint8_t* data = (uint8_t*)vdata; uint32_t acc = 0xffff;
    for (uint32_t i = 0; i + 1 < length; i += 2) {
        uint16_t word = ((uint32_t)data[i] << 8) | data[i + 1]; acc += word; if (acc > 0xffff) acc -= 0xffff;
    }
    if (length & 1) { uint16_t word = (uint32_t)data[length - 1] << 8; acc += word; if (acc > 0xffff) acc -= 0xffff; }
    return htons(~acc);
}

struct __attribute__((packed)) ethernet_frame { uint8_t dest_mac[6]; uint8_t src_mac[6]; uint16_t ethertype; char payload[1500]; };
struct __attribute__((packed)) arp_ipv4 { uint16_t htype; uint16_t ptype; uint8_t hlen; uint8_t plen; uint16_t oper; uint8_t sha[6]; uint8_t spa[4]; uint8_t tha[6]; uint8_t tpa[4]; };
struct __attribute__((packed)) ipv4_header { uint8_t ihl_version; uint8_t tos; uint16_t tot_len; uint16_t id; uint16_t frag_off; uint8_t ttl; uint8_t protocol; uint16_t check; uint8_t saddr[4]; uint8_t daddr[4]; };
struct __attribute__((packed)) icmp_header { uint8_t type; uint8_t code; uint16_t checksum; uint16_t id; uint16_t sequence; };

struct __attribute__((packed)) udp_header {
    uint16_t src_port;
    uint16_t dst_port;
    uint16_t len;
    uint16_t checksum;
};

struct __attribute__((packed)) dns_header {
    uint16_t id;
    uint16_t flags;
    uint16_t q_count;
    uint16_t ans_count;
    uint16_t auth_count;
    uint16_t add_count;
};

// SNTPv4 (RFC 4330) заголовок, ровно 48 байт полезной нагрузки поверх UDP.
struct __attribute__((packed)) ntp_packet {
    uint8_t li_vn_mode;
    uint8_t stratum;
    uint8_t poll;
    int8_t precision;
    uint32_t root_delay;
    uint32_t root_dispersion;
    uint32_t ref_id;
    uint32_t ref_ts_sec;
    uint32_t ref_ts_frac;
    uint32_t orig_ts_sec;
    uint32_t orig_ts_frac;
    uint32_t rx_ts_sec;
    uint32_t rx_ts_frac;
    uint32_t tx_ts_sec;
    uint32_t tx_ts_frac;
};

// BOOTP/DHCP (RFC 2131) заголовок. options — TLV-поток переменной длины
// (code, len, data...), заканчивается байтом 0xFF; парсим/собираем вручную,
// как и остальные протоколы в этом файле.
struct __attribute__((packed)) dhcp_packet {
    uint8_t  op;       // 1 = BOOTREQUEST, 2 = BOOTREPLY
    uint8_t  htype;    // 1 = Ethernet
    uint8_t  hlen;     // 6
    uint8_t  hops;
    uint32_t xid;
    uint16_t secs;
    uint16_t flags;
    uint8_t  ciaddr[4];
    uint8_t  yiaddr[4];
    uint8_t  siaddr[4];
    uint8_t  giaddr[4];
    uint8_t  chaddr[16];
    uint8_t  sname[64];
    uint8_t  file[128];
    uint32_t magic_cookie; // 0x63825363
    uint8_t  options[312];
};

// Глобальные переменные для DNS-сессии
static char g_dns_pending_domain[64];
static bool g_dns_outstanding = false;
static uint16_t g_dns_id = 0xDEAF;

static uint8_t my_mac[6] = {0};
static uint8_t router_mac[6] = {0};
static bool have_router_mac = false;

// Кэш MAC соседа по локальной подсети — на один слот (последний резолвленный
// адресат), по аналогии с router_mac. Раньше ВЕСЬ трафик слался на MAC
// шлюза как next-hop, даже адресатам внутри своей же подсети — рабочий вариант
// для внешних хостов (обычная маршрутизация), но для соседей по LAN многие
// роутеры/свитчи не отражают такой кадр обратно (anti-spoofing/split-horizon),
// и пакет просто терялся, хотя адресат отвечает живым хостам напрямую. См.
// ip_is_onlink()/resolve_dest_mac() ниже.
static uint8_t g_onlink_ip[4] = {0, 0, 0, 0};
static uint8_t g_onlink_mac[6] = {0};
static bool g_have_onlink_mac = false;

// --- IP-конфигурация: раньше была захардкожена под QEMU user-mode networking
// (10.0.2.15/10.0.2.2 — гость/SLIRP-гейтвей), на реальном железе такого хоста
// физически нет, поэтому ARP на 10.0.2.2 никогда не отвечал. Теперь всё это
// получаем по DHCP (см. блок "DHCP-клиент" ниже) — до получения адреса
// my_ip/g_gateway_ip остаются 0.0.0.0, а сетевые команды (ping/send/resolve/
// ntp) ждут g_dhcp_bound, а не шлют ARP в пустоту.
static uint8_t my_ip[4] = {0, 0, 0, 0};
static uint8_t g_gateway_ip[4] = {0, 0, 0, 0};
static uint8_t g_subnet_mask[4] = {255, 255, 255, 0};
// Публичный DNS как запасной вариант, пока DHCP не пришлёт свой (опция 6).
static uint8_t g_dns_ip[4] = {8, 8, 8, 8};

static bool ip_is_onlink(const uint8_t ip[4]) {
    for (int i = 0; i < 4; i++) {
        if ((ip[i] & g_subnet_mask[i]) != (my_ip[i] & g_subnet_mask[i])) return false;
    }
    return true;
}

static bool ip_eq(const uint8_t a[4], const uint8_t b[4]) {
    for (int i = 0; i < 4; i++) if (a[i] != b[i]) return false;
    return true;
}

// Возвращает MAC, на который реально слать кадр для dst_ip: если адресат в
// нашей же подсети — его собственный (резолвленный отдельным ARP, см.
// g_onlink_ip/g_onlink_mac), иначе — MAC шлюза как next-hop (обычная
// маршрутизация). false — нужного MAC ещё нет, кто-то должен инициировать
// его резолв (см. net_send_arp_request/net_poll ниже).
static bool resolve_dest_mac(const uint8_t dst_ip[4], uint8_t out_mac[6]) {
    // Сам шлюз — ВСЕГДА через router_mac, даже если его адрес физически
    // попадает в нашу подсеть (стандартная ситуация: gateway почти всегда
    // внутри собственной объявленной подсети). Проверяем это раньше общего
    // ip_is_onlink(), иначе резолвился бы MAC шлюза, а искали бы его потом в
    // g_onlink_mac — рассинхрон, из-за которого `resolve` на DNS-сервер,
    // совпадающий с шлюзом (частый случай), никогда не находил уже
    // резолвленный MAC и вис навсегда (см. отчёт tcpdump — ARP на шлюз
    // получал ответ мгновенно, а DNS-запрос после этого так и не уходил).
    if (ip_eq(dst_ip, g_gateway_ip)) {
        if (!have_router_mac) return false;
        for (int i = 0; i < 6; i++) out_mac[i] = router_mac[i];
        return true;
    }
    if (ip_is_onlink(dst_ip)) {
        if (!g_have_onlink_mac || !ip_eq(dst_ip, g_onlink_ip)) return false;
        for (int i = 0; i < 6; i++) out_mac[i] = g_onlink_mac[i];
        return true;
    }
    if (!have_router_mac) return false;
    for (int i = 0; i < 6; i++) out_mac[i] = router_mac[i];
    return true;
}

// IP, который нужно ARP-резолвить, чтобы получить MAC для dst_ip — сам
// dst_ip, если он в нашей подсети (кроме самого шлюза — см. resolve_dest_mac
// выше, для него отдельная ветка не нужна: gateway_ip и так возвращается),
// иначе — шлюз.
static void arp_target_for(const uint8_t dst_ip[4], uint8_t out_target[4]) {
    const uint8_t* src;
    if (ip_eq(dst_ip, g_gateway_ip)) src = g_gateway_ip;
    else if (ip_is_onlink(dst_ip)) src = dst_ip;
    else src = g_gateway_ip;
    for (int i = 0; i < 4; i++) out_target[i] = src[i];
}

// GENET v5 (см. h/platform.h GENET_*) — заменяет virtio-net. g_net_up играет
// ту же роль, что раньше "g_net_regs != nullptr": есть ли вообще сетевое
// железо, инициализировано ли оно успешно.
static volatile uint8_t* g_genet_base = nullptr;
static bool g_net_up = false;
static uintptr_t rx_buffer_offsets[GENET_RX_DESCS] = { 0x2800, 0x2E00, 0x3400, 0x3A00 };
static uint32_t g_rx_c_index = 0;   // наш локальный "consumer index" (RDMA_CONS_INDEX)
static uint32_t g_rx_index = 0;     // индекс текущего RX-дескриптора (wrap по GENET_RX_DESCS)
static uint32_t g_tx_index = 0;     // индекс текущего TX-дескриптора (wrap по GENET_TX_DESCS)

enum NetCommand {
    NET_CMD_NONE = 0,
    NET_CMD_PING = 1,
    NET_CMD_SEND = 2,
    NET_CMD_STATUS = 3,
    NET_CMD_RESOLVE = 4,
    NET_CMD_RECV = 5,
    NET_CMD_NTP = 6,
};

// --- Настройки NTP-клиента (правьте здесь) ---
// Google Public NTP (time.google.com), стабильный anycast-адрес, отвечает
// в стандартном клиент-серверном режиме (mode=3) без leap smear проблем для нас.
static uint8_t g_ntp_server_ip[4] = {216, 239, 35, 0};
static const uint16_t NTP_SERVER_PORT = 123;
// Разница эпох NTP (1900-01-01) и Unix (1970-01-01) в секундах.
static const uint32_t NTP_UNIX_EPOCH_DELTA = 2208988800u;
// Интервал автоматической периодической ресинхронизации (мс). Отсчитывается
// по аптайму (не по показаниям часов), поэтому не зависит от самой коррекции.
static const uint64_t NTP_RESYNC_INTERVAL_MS = 30ull * 60 * 1000; // 30 минут

// Последняя принятая произвольная UDP-датаграмма (не DNS-ответ), ждущая
// команды `recv` из shell. Однослотовый почтовый ящик: новый пакет
// перезаписывает непрочитанный.
static bool g_udp_rx_ready = false;
static uint8_t g_udp_rx_src_ip[4] = {0, 0, 0, 0};
static uint16_t g_udp_rx_src_port = 0;
static char g_udp_rx_data[256];
static int g_udp_rx_len = 0;

// Capability до blk_driver (см. main.cpp: local_blk_ep=7, ipc->msg[7]) —
// раньше net_driver им не пользовался вообще, нужен только для журнала
// произвольных UDP-датаграмм ниже (net_log_udp) — LAN оказалась крайне
// разговорчивой (mDNS/NetBIOS-broadcast от соседей, десятки пакетов пачкой),
// и печать каждой в консоль через sys_puts (полноценный IPC + медленный UART)
// натурально тормозила главный цикл ровно тогда, когда мог прийти настоящий
// ответ на исходящий ping — прямое подозрение на часть "случайных" таймаутов.
static seL4_CPtr g_blk_ep = 0;
static const char* NET_LOG_PATH = PATH_NET_UDP_LOG; // см. platform.h — единый источник известных путей
// С запасом под лимит blk_driver'а (SYS_WRITE_FILE клампит len до 4096).
static const uint32_t NET_LOG_BUF_CAP = 3584;
static char g_udp_log_buf[NET_LOG_BUF_CAP];
static uint32_t g_udp_log_len = 0;

// Плейсхолдеры: раньше указывали на QEMU SLIRP-хост (10.0.2.2), сейчас
// нерелевантно — все реально используемые значения перезаписываются перед
// использованием (конкретным IP из команды shell либо DHCP-адресами).
static uint8_t default_udp_ip[4] = {0, 0, 0, 0};
static uint16_t default_udp_port = 8080;
static uint8_t pending_ip[4] = {0, 0, 0, 0};
static uint8_t pending_udp_ip[4] = {0, 0, 0, 0};
static uint16_t pending_udp_port = 8080;
static char pending_udp[64];
static int pending_cmd = NET_CMD_NONE;
static uint32_t pending_ping_count = 1;

static uint8_t g_ping_target_ip[4] = {0, 0, 0, 0};
static uint16_t g_ping_next_seq = 0;
static uint16_t g_ping_outstanding_seq = 0;
static bool g_ping_outstanding = false;
static uint32_t g_ping_series_remaining = 0;
static uint32_t g_ping_sent_count = 0;
static uint32_t g_ping_reply_count = 0;
static uint32_t g_ping_timeout_count = 0;
static uint64_t g_ping_last_rtt_us = 0;
static uint64_t g_ping_min_rtt_us = 0;
static uint64_t g_ping_max_rtt_us = 0;
static uint64_t g_ping_total_rtt_us = 0;
static uint64_t g_ping_next_send_ms = 0;
// Реальное время (мс, через timer_ep) отправки текущего echo request — раньше
// таймаут и RTT считались по числу итераций главного цикла ("~2 секунды =
// 100 000 циклов"), что было чистой оценкой на глаз и совершенно не отражало
// реальное время: цикл почти всегда пустой (пара MMIO-регистровых чтений) и
// крутится куда быстрее/медленнее, чем предполагал этот комментарий — отсюда
// таймаут на ping к хостам с честным ~65мс RTT (140.82.121.3) при том, что
// ~21мс (8.8.8.8) укладывался. Показанные раньше "21.195 ms" были не
// измерением, а произвольным loops*15 — теперь берём настоящее время.
static uint64_t g_ping_sent_ms = 0;
static const uint64_t PING_TIMEOUT_MS = 2000;
// Для RTT (не таймаута) — метка по аппаратному счётчику, см. read_cntvct()
// выше. g_ping_sent_ms остаётся для грубой проверки "не прошло ли 2 секунды" —
// там миллисекундного разрешения более чем достаточно.
static uint64_t g_ping_sent_cyc = 0;

// Стандартный размер полезной нагрузки ICMP echo (как у обычного unix ping) —
// 8 (заголовок ICMP) + 56 = 64 байта самого ICMP-сообщения, что и печатается
// в "64 bytes from ..." — раньше было 4 байта ("PONG"), что честно работало,
// но выглядело не как настоящий ping.
static const int PING_PAYLOAD_LEN = 56;

// --- Статистика ТЕКУЩЕЙ серии ping (в отличие от g_ping_*_count/rtt_us выше,
// которые накапливаются за всё время жизни процесса, для netstat) — нужна,
// чтобы shell мог напечатать "--- ... ping statistics ---" в конце команды
// `ping`, как обычный unix ping. Сбрасывается в net_start_ping_series(),
// публикуется в SHM (+4068..+4092) при завершении серии — см.
// net_schedule_next_ping() и shell.cpp.
static uint32_t g_ping_series_sent = 0;
static uint32_t g_ping_series_reply = 0;
static uint64_t g_ping_series_min_rtt_us = 0;
static uint64_t g_ping_series_max_rtt_us = 0;
static uint64_t g_ping_series_total_rtt_us = 0;
static uint64_t g_ping_series_total_rtt_sq_us = 0; // для mdev (среднеквадратичное отклонение)
static uint64_t g_ping_series_start_ms = 0;

// Целочисленный квадратный корень (метод Ньютона) — нужен только для mdev,
// плавающая точка/printf с дробными числами в этом окружении не заведены.
static uint64_t isqrt64(uint64_t n) {
    if (n == 0) return 0;
    uint64_t x = n, y = (x + 1) / 2;
    while (y < x) { x = y; y = (x + n / x) / 2; }
    return x;
}

// --- Состояние NTP-клиента ---
static bool g_ntp_outstanding = false;
// T1: наши (возможно, неточные) часы в момент отправки запроса, сек. с эпохи Unix.
static uint64_t g_ntp_t1_epoch_s = 0;
static uint32_t g_ntp_last_offset_s = 0; // Для диагностики (netstat/логов), знак хранится отдельно.
static bool g_ntp_last_offset_negative = false;
static bool g_ntp_synced = false;
// Аптайм (мс) следующей плановой автоматической ресинхронизации.
static uint64_t g_ntp_next_resync_uptime_ms = 0;

// --- Состояние DHCP-клиента ---
enum DhcpState { DHCP_IDLE = 0, DHCP_DISCOVERING, DHCP_REQUESTING, DHCP_BOUND };
static DhcpState g_dhcp_state = DHCP_IDLE;
static bool g_dhcp_bound = false;
static uint32_t g_dhcp_xid = 0;
static uint8_t g_dhcp_offered_ip[4] = {0, 0, 0, 0};
static uint8_t g_dhcp_server_ip[4] = {0, 0, 0, 0};
static uint64_t g_dhcp_retry_uptime_ms = 0;
static uint64_t g_dhcp_lease_deadline_uptime_ms = 0; // 0 = не планировать перезапрос
// Счётчик подряд неудачных попыток — растягивает интервал повтора (см.
// net_check_dhcp), чтобы при отсутствии DHCP-сервера в сети не заваливать
// консоль строкой каждые 4с вечно (мешает вводу команд в shell). Сбрасывается
// при получении ACK и при новом подключении кабеля (net_check_link_status).
static uint32_t g_dhcp_attempt = 0;
static const uint16_t DHCP_CLIENT_PORT = 68;
static const uint16_t DHCP_SERVER_PORT = 67;
static const uint32_t DHCP_MAGIC_COOKIE = 0x63825363u;
static const uint64_t DHCP_RETRY_MS = 4000;      // интервал первых попыток
static const uint64_t DHCP_RETRY_MAX_MS = 60000; // потолок для backoff после нескольких неудач
static const uint32_t DHCP_DEFAULT_LEASE_S = 3600; // если сервер не прислал опцию 51

// ========================================================
// АППАРАТНЫЙ УРОВЕНЬ: GENET v5 (BCM2711 Ethernet MAC), заменяет virtio-net.
// Регистровая карта и порядок инициализации — см. h/platform.h (GENET_*) и
// план Фазы 3.2: адаптировано 1:1 из проверенного рабочего референса
// /home/nikita/RPi4_SeL4/u-boot/drivers/net/bcmgenet.c (драйвер U-Boot для
// этой же платы). Протокольная логика ниже (ARP/IPv4/UDP/DNS/NTP) эту
// границу не видит — только через net_hw_send/poll_rx/rx_done.
// ========================================================
static inline void genet_write32(uintptr_t offset, uint32_t val) {
    *(volatile uint32_t*)(g_genet_base + offset) = val;
}
static inline uint32_t genet_read32(uintptr_t offset) {
    return *(volatile uint32_t*)(g_genet_base + offset);
}
// Короткая "слепая" пауза для reset-тайминга (10мкс/2мкс по референсу) — как
// и везде в этом порту, нет доступа к точному аппаратному таймеру на этом
// этапе, busy-wait с щедрым запасом (см. EMMC2/UART).
static void genet_delay() {
    for (volatile uint32_t i = 0; i < 20000; i++) { }
}

static bool genet_mdio_write(uint32_t reg, uint16_t value) {
    uint32_t val = GENET_MDIO_WR | (GENET_PHY_ADDR << GENET_MDIO_PMD_SHIFT) |
                   (reg << GENET_MDIO_REG_SHIFT) | (value & 0xFFFFu);
    genet_write32(GENET_MDIO_CMD_OFFSET, val);
    genet_write32(GENET_MDIO_CMD_OFFSET, val | GENET_MDIO_START_BUSY);
    uint32_t timeout = 200000;
    while (genet_read32(GENET_MDIO_CMD_OFFSET) & GENET_MDIO_START_BUSY) {
        if (--timeout == 0) return false;
        seL4_Yield();
    }
    return true;
}

static bool genet_mdio_read(uint32_t reg, uint16_t* out_value) {
    uint32_t val = GENET_MDIO_RD | (GENET_PHY_ADDR << GENET_MDIO_PMD_SHIFT) | (reg << GENET_MDIO_REG_SHIFT);
    genet_write32(GENET_MDIO_CMD_OFFSET, val);
    genet_write32(GENET_MDIO_CMD_OFFSET, val | GENET_MDIO_START_BUSY);
    uint32_t timeout = 200000;
    while (genet_read32(GENET_MDIO_CMD_OFFSET) & GENET_MDIO_START_BUSY) {
        if (--timeout == 0) return false;
        seL4_Yield();
    }
    *out_value = (uint16_t)(genet_read32(GENET_MDIO_CMD_OFFSET) & 0xFFFFu);
    return true;
}

// Текущее состояние линка (для динамического обнаружения подключения/
// отключения кабеля после старта — см. net_check_link_status() ниже).
static bool g_phy_link_up = false;
// Throttle для net_check_link_status(), чтобы не долбить MDIO на каждой
// итерации главного цикла — не имеет смысла опрашивать чаще, чем реально
// нужно для "быстро заметить, что кабель воткнули/выдернули".
static uint64_t g_link_next_check_uptime_ms = 0;
static const uint64_t LINK_CHECK_INTERVAL_MS = 1000;

// Определяет реально согласованную автопереговорами скорость через
// стандартные (Clause 22, не вендор-специфичные) MDIO-регистры — раньше
// скорость была захардкожена в 1000BASE-T "первой версией" (см. план Фазы
// 3.2), и это оказалось ошибкой: при несовпадении с реальным согласованием
// GENET внутренне считает TX/RX дескрипторы обработанными (счётчики
// продвигаются), но кадры на проводе оказываются битыми на уровне PHY —
// снаружи это неотличимо от "сеть не отвечает совсем", что мы и наблюдали
// (DHCP ни разу не получил ответ даже после починки RX-фильтров/смещения).
// Приоритет по IEEE 802.3: 1000 > 100 > 10 (дуплекс не разбираем — GENET
// здесь всегда работает в полном дуплексе, upstream-порт half-duplex на
// практике не встречается).
static uint32_t genet_resolve_link_speed(seL4_CPtr console_ep) {
    uint16_t stat1000 = 0, ctrl1000 = 0, lpa = 0, adv = 0;
    genet_mdio_read(MII_STAT1000, &stat1000);
    genet_mdio_read(MII_CTRL1000, &ctrl1000);
    genet_mdio_read(MII_LPA, &lpa);
    genet_mdio_read(MII_ADVERTISE, &adv);

    bool have_1000 = (ctrl1000 & (MII_CTRL1000_ADV_FULL | MII_CTRL1000_ADV_HALF)) &&
                      (stat1000 & (MII_STAT1000_LP_FULL | MII_STAT1000_LP_HALF));
    bool have_100 = (adv & (MII_ADV_100FULL | MII_ADV_100HALF)) &&
                     (lpa & (MII_ADV_100FULL | MII_ADV_100HALF));
    bool have_10 = (adv & (MII_ADV_10FULL | MII_ADV_10HALF)) &&
                    (lpa & (MII_ADV_10FULL | MII_ADV_10HALF));

    uint32_t speed; const char* speed_str;
    if (have_1000)      { speed = GENET_UMAC_SPEED_1000; speed_str = "1000"; }
    else if (have_100)  { speed = GENET_UMAC_SPEED_100;  speed_str = "100"; }
    else if (have_10)   { speed = GENET_UMAC_SPEED_10;   speed_str = "10"; }
    else                { speed = GENET_UMAC_SPEED_1000; speed_str = "1000 (ANEG не разрешился, догадка)"; }

    sys_puts(console_ep, "[NET] PHY negotiated speed: ");
    sys_puts(console_ep, speed_str);
    sys_puts(console_ep, "Mbps\n");
    return speed;
}

// Аналог bcmgenet_adjust_link: RGMII OOB control + скорость. rgmii-rxid по
// dts (см. platform.h), без ветвления по режиму. Вызывается и при старте, и
// при динамическом обнаружении подключения кабеля (см. net_check_link_status).
static void genet_apply_link(seL4_CPtr console_ep) {
    uint32_t reg = genet_read32(GENET_EXT_RGMII_OOB_CTRL_OFFSET);
    reg &= ~GENET_OOB_DISABLE;
    reg |= GENET_RGMII_LINK | GENET_RGMII_MODE_EN;
    genet_write32(GENET_EXT_RGMII_OOB_CTRL_OFFSET, reg);
    reg |= GENET_ID_MODE_DIS;
    genet_write32(GENET_EXT_RGMII_OOB_CTRL_OFFSET, reg);

    uint32_t speed = genet_resolve_link_speed(console_ep);

    // ВАЖНО: read-modify-write, а не голая перезапись регистра — иначе тут
    // же затирается CMD_PROMISC, выставленный в net_hw_init() (эта функция
    // вызывается повторно при каждом переподключении кабеля, см.
    // net_check_link_status).
    uint32_t cmd = genet_read32(GENET_UMAC_CMD_OFFSET);
    cmd &= ~(0x3u << GENET_CMD_SPEED_SHIFT);
    cmd |= (speed << GENET_CMD_SPEED_SHIFT);
    genet_write32(GENET_UMAC_CMD_OFFSET, cmd);
    genet_write32(GENET_UMAC_CMD_OFFSET,
                  genet_read32(GENET_UMAC_CMD_OFFSET) | GENET_CMD_TX_EN | GENET_CMD_RX_EN);
}

// Сброс PHY через MDIO + однократное ожидание линка при старте. НЕ фатально
// и НЕ вешает загрузку, если линк не поднялся (кабель не воткнут) — ждём не
// дольше LINK_CHECK_INTERVAL_MS реального времени (через уже рабочий
// timer_driver, Фаза 3.1), а не фиксированное число итераций: раньше здесь
// был busy-loop на 2 000 000 итераций MDIO-чтений, который при отсутствии
// линка мог реально тянуться минутами — с точки зрения пользователя это
// неотличимо от зависания. Дальнейшее отслеживание — см.
// net_check_link_status(), она подхватит кабель, воткнутый уже после старта.
static void genet_phy_init(seL4_CPtr console_ep, seL4_CPtr timer_ep) {
    genet_mdio_write(MII_BMCR, MII_BMCR_RESET);

    uint32_t timeout = 100000;
    uint16_t bmcr = MII_BMCR_RESET;
    while (bmcr & MII_BMCR_RESET) {
        if (!genet_mdio_read(MII_BMCR, &bmcr)) { sys_puts(console_ep, "[NET] WARNING: MDIO read failed during PHY reset.\n"); break; }
        if (--timeout == 0) { sys_puts(console_ep, "[NET] WARNING: PHY reset timeout.\n"); break; }
    }

    uint64_t deadline_ms = sys_get_uptime_ms(timer_ep) + 500; // короткое ожидание, не дольше 0.5с
    uint16_t bmsr = 0;
    bool link_up = false;
    while (sys_get_uptime_ms(timer_ep) < deadline_ms) {
        if (!genet_mdio_read(MII_BMSR, &bmsr)) break;
        if (bmsr & MII_BMSR_LINK_UP) { link_up = true; break; }
        seL4_Yield();
    }
    g_phy_link_up = link_up;
    g_link_next_check_uptime_ms = sys_get_uptime_ms(timer_ep) + LINK_CHECK_INTERVAL_MS;
    sys_puts(console_ep, link_up ? "[NET] PHY link up.\n"
                                  : "[NET] PHY link not up yet (cable unplugged?) — will keep watching, not blocking boot.\n");
}

// Периодическая (не чаще LINK_CHECK_INTERVAL_MS) проверка линка через MDIO —
// динамическое обнаружение подключения/отключения кабеля после старта, а не
// только один раз при инициализации. Дёшево (один MDIO read раз в секунду),
// вызывается из главного цикла net_driver'а наравне с другими net_check_*.
static void net_check_link_status(seL4_CPtr console_ep, seL4_CPtr timer_ep) {
    if (!g_net_up) return;
    uint64_t now = sys_get_uptime_ms(timer_ep);
    if (now < g_link_next_check_uptime_ms) return;
    g_link_next_check_uptime_ms = now + LINK_CHECK_INTERVAL_MS;

    uint16_t bmsr = 0;
    if (!genet_mdio_read(MII_BMSR, &bmsr)) return;
    bool link_up = (bmsr & MII_BMSR_LINK_UP) != 0;

    if (link_up && !g_phy_link_up) {
        sys_puts(console_ep, "[NET] PHY link up (cable connected).\n");
        genet_apply_link(console_ep);
        // Старый MAC гейтвея мог устареть (другая сеть/роутер) — просим
        // разрешить заново при следующей реальной отправке. Новая сеть — новый
        // DHCP: net_check_dhcp() подхватит DHCP_IDLE на следующей итерации.
        have_router_mac = false;
        g_have_onlink_mac = false;
        g_dhcp_state = DHCP_IDLE;
        g_dhcp_bound = false;
        g_dhcp_attempt = 0; // новая сеть — обратный отсчёт backoff'а начинаем с нуля
    } else if (!link_up && g_phy_link_up) {
        sys_puts(console_ep, "[NET] PHY link down (cable disconnected).\n");
        have_router_mac = false;
        g_have_onlink_mac = false;
        g_dhcp_state = DHCP_IDLE;
        g_dhcp_bound = false;
        g_dhcp_attempt = 0;
        // Адрес был выдан для уже отключённой сети — не используем его дальше.
        for (int i = 0; i < 4; i++) my_ip[i] = 0;
        for (int i = 0; i < 4; i++) g_gateway_ip[i] = 0;
    }
    g_phy_link_up = link_up;
}

static void genet_rx_ring_init() {
    genet_write32(GENET_RDMA_REG_BASE + GENET_DMA_SCB_BURST_SIZE_OFF, GENET_DMA_MAX_BURST_LENGTH);
    genet_write32(GENET_RDMA_RING_REG_BASE + GENET_DMA_START_ADDR_OFF, 0);
    genet_write32(GENET_RDMA_RING_REG_BASE + GENET_RDMA_READ_PTR_OFF, 0);
    genet_write32(GENET_RDMA_RING_REG_BASE + GENET_RDMA_WRITE_PTR_OFF, 0);
    genet_write32(GENET_RDMA_RING_REG_BASE + GENET_DMA_END_ADDR_OFF,
                  GENET_RX_DESCS * GENET_DMA_DESC_SIZE / 4 - 1);

    // PROD_INDEX нельзя обнулить (read-only счётчик желаза) — синхронизируем
    // CONS_INDEX под него, ровно как в референсе.
    g_rx_c_index = genet_read32(GENET_RDMA_RING_REG_BASE + GENET_RDMA_PROD_INDEX_OFF);
    genet_write32(GENET_RDMA_RING_REG_BASE + GENET_RDMA_CONS_INDEX_OFF, g_rx_c_index);
    g_rx_index = g_rx_c_index % GENET_RX_DESCS;

    genet_write32(GENET_RDMA_RING_REG_BASE + GENET_DMA_RING_BUF_SIZE_OFF,
                  (GENET_RX_DESCS << GENET_DMA_RING_SIZE_SHIFT) | GENET_RX_BUF_LENGTH);
    genet_write32(GENET_RDMA_RING_REG_BASE + GENET_RDMA_XON_XOFF_THRESH_OFF, GENET_DMA_FC_THRESH_VALUE);
    genet_write32(GENET_RDMA_REG_BASE + GENET_DMA_RING_CFG_OFF, 1u << GENET_DEFAULT_Q);
}

static void genet_rx_descs_init() {
    for (uint32_t i = 0; i < GENET_RX_DESCS; i++) {
        uintptr_t desc = GENET_RX_OFF + i * GENET_DMA_DESC_SIZE;
        uint32_t buf_paddr = g_shm_paddr + (uint32_t)rx_buffer_offsets[i];
        genet_write32(desc + GENET_DMA_DESC_ADDRESS_LO, buf_paddr);
        genet_write32(desc + GENET_DMA_DESC_ADDRESS_HI, 0);
        genet_write32(desc + GENET_DMA_DESC_LENGTH_STATUS, (GENET_RX_BUF_LENGTH << GENET_DMA_BUFLENGTH_SHIFT) | GENET_DMA_OWN);
    }
}

static void genet_tx_ring_init() {
    genet_write32(GENET_TDMA_REG_BASE + GENET_DMA_SCB_BURST_SIZE_OFF, GENET_DMA_MAX_BURST_LENGTH);
    genet_write32(GENET_TDMA_RING_REG_BASE + GENET_DMA_START_ADDR_OFF, 0);
    genet_write32(GENET_TDMA_RING_REG_BASE + GENET_TDMA_READ_PTR_OFF, 0);
    genet_write32(GENET_TDMA_RING_REG_BASE + GENET_TDMA_WRITE_PTR_OFF, 0);
    genet_write32(GENET_TDMA_RING_REG_BASE + GENET_DMA_END_ADDR_OFF,
                  GENET_TX_DESCS * GENET_DMA_DESC_SIZE / 4 - 1);

    // CONS_INDEX read-only — синхронизируем PROD_INDEX под него (см. rx выше).
    g_tx_index = genet_read32(GENET_TDMA_RING_REG_BASE + GENET_TDMA_CONS_INDEX_OFF);
    genet_write32(GENET_TDMA_RING_REG_BASE + GENET_TDMA_PROD_INDEX_OFF, g_tx_index);
    g_tx_index = g_tx_index % GENET_TX_DESCS;

    genet_write32(GENET_TDMA_RING_REG_BASE + GENET_DMA_MBUF_DONE_THRESH_OFF, 1);
    genet_write32(GENET_TDMA_RING_REG_BASE + GENET_TDMA_FLOW_PERIOD_OFF, 0);
    genet_write32(GENET_TDMA_RING_REG_BASE + GENET_DMA_RING_BUF_SIZE_OFF,
                  (GENET_TX_DESCS << GENET_DMA_RING_SIZE_SHIFT) | GENET_RX_BUF_LENGTH);
    genet_write32(GENET_TDMA_REG_BASE + GENET_DMA_RING_CFG_OFF, 1u << GENET_DEFAULT_Q);
}

// Инициализация железа GENET целиком — reset, MAC, кольца, PHY, включение
// TX/RX. Возвращает false только если само железо не отвечает вообще
// (SYS_REV_CTRL читается как 0xFFFFFFFF и т.п.) — отсутствие линка на PHY
// не считается ошибкой (см. genet_phy_init), плата просто не подключена.
bool net_hw_init(seL4_CPtr console_ep, seL4_CPtr timer_ep) {
    g_genet_base = (volatile uint8_t*)PLAT_GENET_VADDR;

    uint32_t rev = genet_read32(GENET_SYS_REV_CTRL_OFFSET);
    if (rev == 0xFFFFFFFFu) {
        sys_puts(console_ep, "[NET] ERROR: GENET SYS_REV_CTRL unreadable (0xFFFFFFFF) — no hardware?\n");
        return false;
    }

    // Этап 1 (аналог bcmgenet_eth_probe): ранний reset+loopback стабилизирует rxclk.
    genet_write32(GENET_SYS_PORT_CTRL_OFFSET, GENET_PORT_MODE_EXT_GPHY);
    genet_write32(GENET_SYS_RBUF_FLUSH_CTRL_OFFSET, 0);
    genet_delay();
    genet_write32(GENET_UMAC_CMD_OFFSET, 0);
    genet_write32(GENET_UMAC_CMD_OFFSET, GENET_CMD_SW_RESET | GENET_CMD_LCL_LOOP_EN);

    // Этап 2 (аналог bcmgenet_umac_reset): полный reset.
    uint32_t reg = genet_read32(GENET_SYS_RBUF_FLUSH_CTRL_OFFSET);
    reg |= (1u << 1);
    genet_write32(GENET_SYS_RBUF_FLUSH_CTRL_OFFSET, reg); genet_delay();
    reg &= ~(1u << 1);
    genet_write32(GENET_SYS_RBUF_FLUSH_CTRL_OFFSET, reg); genet_delay();
    genet_write32(GENET_SYS_RBUF_FLUSH_CTRL_OFFSET, 0); genet_delay();

    genet_write32(GENET_UMAC_CMD_OFFSET, 0);
    genet_write32(GENET_UMAC_CMD_OFFSET, GENET_CMD_SW_RESET | GENET_CMD_LCL_LOOP_EN);
    genet_delay();
    genet_write32(GENET_UMAC_CMD_OFFSET, 0);

    genet_write32(GENET_UMAC_MIB_CTRL_OFFSET, GENET_MIB_RESET_RX | GENET_MIB_RESET_TX | GENET_MIB_RESET_RUNT);
    genet_write32(GENET_UMAC_MIB_CTRL_OFFSET, 0);

    genet_write32(GENET_UMAC_MAX_FRAME_LEN_OFFSET, 1536);

    reg = genet_read32(GENET_RBUF_CTRL_OFFSET);
    reg |= GENET_RBUF_ALIGN_2B;
    genet_write32(GENET_RBUF_CTRL_OFFSET, reg);
    genet_write32(GENET_RBUF_TBUF_SIZE_CTRL_OFFSET, 1);

    // MAC-адрес: свой locally-administered (бит U/L корректно выставлен в
    // 0x02) — GENET не хранит заводской MAC сам по себе, реальный MAC платы
    // доступен только через VideoCore mailbox, который в этой же сессии уже
    // не отвечал на попытке с PL011 — не рискуем повторно (см. план Фазы 3.2).
    my_mac[0] = 0x02; my_mac[1] = 0x50; my_mac[2] = 0x57;
    my_mac[3] = 0x4F; my_mac[4] = 0x53; my_mac[5] = 0x01;
    uint32_t mac0 = ((uint32_t)my_mac[0] << 24) | ((uint32_t)my_mac[1] << 16) |
                    ((uint32_t)my_mac[2] << 8) | my_mac[3];
    uint32_t mac1 = ((uint32_t)my_mac[4] << 8) | my_mac[5];
    genet_write32(GENET_UMAC_MAC0_OFFSET, mac0);
    genet_write32(GENET_UMAC_MAC1_OFFSET, mac1);

    // Гасим MAC Destination Filter и включаем promiscuous — иначе приём
    // кадров зависит от состояния MDF-таблицы, оставшегося от предыдущего
    // инициализатора GENET (например, U-Boot — см. platform.h у
    // GENET_UMAC_MDF_CTRL_OFFSET). Без этого шага broadcast-ответы DHCP/ARP
    // могли отбрасываться самим железом ещё до RX-кольца.
    genet_write32(GENET_UMAC_MDF_CTRL_OFFSET, 0);
    genet_write32(GENET_UMAC_CMD_OFFSET, genet_read32(GENET_UMAC_CMD_OFFSET) | GENET_CMD_PROMISC);

    // DMA off перед настройкой колец.
    genet_write32(GENET_TDMA_REG_BASE + GENET_DMA_CTRL_OFF,
                  genet_read32(GENET_TDMA_REG_BASE + GENET_DMA_CTRL_OFF) & ~GENET_DMA_EN);
    genet_write32(GENET_RDMA_REG_BASE + GENET_DMA_CTRL_OFF,
                  genet_read32(GENET_RDMA_REG_BASE + GENET_DMA_CTRL_OFF) & ~GENET_DMA_EN);
    genet_write32(GENET_UMAC_TX_FLUSH_OFFSET, 1);
    genet_delay();
    genet_write32(GENET_UMAC_TX_FLUSH_OFFSET, 0);

    genet_rx_ring_init();
    genet_rx_descs_init();
    genet_tx_ring_init();

    // DMA on.
    uint32_t dma_ctrl = (1u << (GENET_DEFAULT_Q + GENET_DMA_RING_BUF_EN_SHIFT)) | GENET_DMA_EN;
    genet_write32(GENET_TDMA_REG_BASE + GENET_DMA_CTRL_OFF, dma_ctrl);
    genet_write32(GENET_RDMA_REG_BASE + GENET_DMA_CTRL_OFF,
                  genet_read32(GENET_RDMA_REG_BASE + GENET_DMA_CTRL_OFF) | dma_ctrl);

    genet_phy_init(console_ep, timer_ep);
    genet_apply_link(console_ep); // RGMII OOB control + скорость + TX/RX enable (см. genet_apply_link())

    g_net_up = true;
    return true;
}

// Синхронная отправка одного кадра (без virtio_net_hdr — чистый Ethernet-кадр).
bool net_hw_send(const void* frame, uint32_t len) {
    uintptr_t desc = GENET_TX_OFF + (g_tx_index % GENET_TX_DESCS) * GENET_DMA_DESC_SIZE;
    uint32_t buf_paddr = g_shm_paddr + (uint32_t)((const char*)frame - g_shm_vaddr);
    uint32_t len_stat = (len << GENET_DMA_BUFLENGTH_SHIFT) | (0x3Fu << GENET_DMA_TX_QTAG_SHIFT) |
                        GENET_DMA_TX_APPEND_CRC | GENET_DMA_SOP | GENET_DMA_EOP;

    uint32_t prod_index = genet_read32(GENET_TDMA_RING_REG_BASE + GENET_TDMA_PROD_INDEX_OFF);

    genet_write32(desc + GENET_DMA_DESC_ADDRESS_LO, buf_paddr);
    genet_write32(desc + GENET_DMA_DESC_ADDRESS_HI, 0);
    genet_write32(desc + GENET_DMA_DESC_LENGTH_STATUS, len_stat);

    g_tx_index = (g_tx_index + 1) % GENET_TX_DESCS;
    prod_index++;
    genet_write32(GENET_TDMA_RING_REG_BASE + GENET_TDMA_PROD_INDEX_OFF, prod_index);

    uint32_t timeout = 2000000;
    while ((genet_read32(GENET_TDMA_RING_REG_BASE + GENET_TDMA_CONS_INDEX_OFF) & 0xFFFFu) < prod_index) {
        if (--timeout == 0) return false;
        seL4_Yield();
    }
    return true;
}

// true, если готов новый кадр — *out_frame указывает на начало реального
// Ethernet-кадра в SHM. Железо (см. GENET_RX_BUF_OFFSET в platform.h) само
// вставляет 2 байта паддинга перед каждым принятым кадром при RBUF_ALIGN_2B,
// и засчитывает их в LENGTH_STATUS — пропускаем их здесь же, один раз, чтобы
// вся протокольная логика выше видела кадр как есть, без этой аппаратной
// специфики.
bool net_hw_poll_rx(uint8_t** out_frame, uint32_t* out_len) {
    uint32_t prod_index = genet_read32(GENET_RDMA_RING_REG_BASE + GENET_RDMA_PROD_INDEX_OFF);
    if ((prod_index & 0xFFFFu) == (g_rx_c_index & 0xFFFFu)) return false;

    uintptr_t desc = GENET_RX_OFF + g_rx_index * GENET_DMA_DESC_SIZE;
    uint32_t len_stat = genet_read32(desc + GENET_DMA_DESC_LENGTH_STATUS);
    uint32_t len = (len_stat >> GENET_DMA_BUFLENGTH_SHIFT) & 0x0FFFu;

    *out_frame = (uint8_t*)(g_shm_vaddr + rx_buffer_offsets[g_rx_index] + GENET_RX_BUF_OFFSET);
    *out_len = (len > GENET_RX_BUF_OFFSET) ? (len - GENET_RX_BUF_OFFSET) : 0;
    return true;
}

void net_hw_rx_done() {
    g_rx_c_index = (g_rx_c_index + 1) & 0xFFFFu;
    genet_write32(GENET_RDMA_RING_REG_BASE + GENET_RDMA_CONS_INDEX_OFF, g_rx_c_index);
    g_rx_index = (g_rx_index + 1) % GENET_RX_DESCS;
}

static void net_send_packet(uint32_t total_len, uint32_t tx_offset = 0x280) {
    net_hw_send(g_shm_vaddr + tx_offset, total_len);
}

// Резолвит MAC для произвольного target_ip — это либо шлюз (адресат вне
// нашей подсети, next-hop как обычно), либо сам адресат, если он в нашей
// подсети (см. arp_target_for()/resolve_dest_mac() выше) — иначе многие
// роутеры не отражают такой unicast-кадр обратно в LAN (anti-spoofing/
// split-horizon), и пакет до соседа по сети просто не доходил. До получения
// адреса по DHCP вызывающий код обязан сперва дождаться g_dhcp_bound (см.
// net_require_ip), иначе это ARP в никуда.
static void net_send_arp_request(seL4_CPtr root_ep, const uint8_t target_ip[4]) {
    volatile ethernet_frame* eth = (volatile ethernet_frame*)(g_shm_vaddr + 0x280);

    for(int i=0; i<6; i++) eth->dest_mac[i] = 0xFF;
    for(int i=0; i<6; i++) eth->src_mac[i] = my_mac[i];
    eth->ethertype = htons(0x0806);

    volatile arp_ipv4* arp = (volatile arp_ipv4*)eth->payload;
    arp->htype = htons(1); arp->ptype = htons(0x0800); arp->hlen = 6; arp->plen = 4; arp->oper = htons(1);
    for(int i=0; i<6; i++) arp->sha[i] = my_mac[i];
    for(int i=0; i<4; i++) arp->spa[i] = my_ip[i];
    for(int i=0; i<6; i++) arp->tha[i] = 0;
    for(int i=0; i<4; i++) arp->tpa[i] = target_ip[i];

    sys_puts(root_ep, "\n[NET] Broadcasting ARP Request for ");
    put_ip(root_ep, target_ip);
    sys_puts(root_ep, "...\n");
    net_send_packet(14 + 28, 0x280);
}

// Отвечает на входящий ARP-запрос "who-has my_ip" — БЕЗ этого любой сосед
// (в первую очередь сам роутер) рано или поздно старит свою ARP-запись про
// нас и перестаёт быть способен нам что-либо доставить: он честно шлёт
// "who-has 192.168.2.206" (обычно 3 раза, раз в секунду), не получает ответа
// и считает нас недостижимыми — именно так объясняется весь ранее
// наблюдавшийся паттерн "первые пинги проходят, потом внезапно перестают" —
// подтверждено tcpdump'ом на роутере: он трижды спрашивал наш MAC, мы молчали,
// и следующий же ICMP-ответ от 8.8.8.8 роутеру уже некуда было доставить.
static void net_send_arp_reply(seL4_CPtr console_ep, const uint8_t target_ip[4], const uint8_t target_mac[6]) {
    volatile ethernet_frame* eth = (volatile ethernet_frame*)(g_shm_vaddr + 0x280);

    for(int i=0; i<6; i++) eth->dest_mac[i] = target_mac[i];
    for(int i=0; i<6; i++) eth->src_mac[i] = my_mac[i];
    eth->ethertype = htons(0x0806);

    volatile arp_ipv4* arp = (volatile arp_ipv4*)eth->payload;
    arp->htype = htons(1); arp->ptype = htons(0x0800); arp->hlen = 6; arp->plen = 4; arp->oper = htons(2);
    for(int i=0; i<6; i++) arp->sha[i] = my_mac[i];
    for(int i=0; i<4; i++) arp->spa[i] = my_ip[i];
    for(int i=0; i<6; i++) arp->tha[i] = target_mac[i];
    for(int i=0; i<4; i++) arp->tpa[i] = target_ip[i];

    net_send_packet(14 + 28, 0x280);
}

// ========================================================
// DHCP-КЛИЕНТ (RFC 2131) — заменяет захардкоженные QEMU-адреса (10.0.2.15/
// 10.0.2.2) на реально выданные роутером my_ip/g_gateway_ip/g_subnet_mask/
// g_dns_ip. Работает всегда через broadcast (и на L2, и на L3) — единственный
// протокол здесь, которому не нужен предварительный ARP на шлюз, поэтому его
// не блокирует have_router_mac.
// ========================================================

// Записывает одну TLV-опцию (code, len, data) в буфер, возвращает число
// записанных байт (2 + len).
static int dhcp_put_option(uint8_t* opt, uint8_t code, uint8_t len, const uint8_t* data) {
    opt[0] = code; opt[1] = len;
    for (int i = 0; i < len; i++) opt[2 + i] = data[i];
    return 2 + len;
}

// msg_type: 1=DHCPDISCOVER, 3=DHCPREQUEST. Для REQUEST использует
// g_dhcp_offered_ip/g_dhcp_server_ip, заполненные при разборе DHCPOFFER.
static void net_send_dhcp_packet(seL4_CPtr console_ep, uint8_t msg_type) {
    uint32_t tx_offset = 0x280;
    volatile ethernet_frame* eth = (volatile ethernet_frame*)(g_shm_vaddr + tx_offset);

    for (int i = 0; i < 6; i++) eth->dest_mac[i] = 0xFF; // DHCP всегда широковещательно, MAC шлюза ещё не резолвим
    for (int i = 0; i < 6; i++) eth->src_mac[i] = my_mac[i];
    eth->ethertype = htons(0x0800);

    volatile ipv4_header* ip = (volatile ipv4_header*)eth->payload;
    volatile udp_header* udp = (volatile udp_header*)(eth->payload + sizeof(ipv4_header));
    volatile dhcp_packet* dhcp = (volatile dhcp_packet*)((char*)udp + sizeof(udp_header));

    for (uint32_t i = 0; i < sizeof(dhcp_packet); i++) ((volatile uint8_t*)dhcp)[i] = 0;
    dhcp->op = 1; dhcp->htype = 1; dhcp->hlen = 6; dhcp->hops = 0;
    dhcp->xid = bswap32(g_dhcp_xid);
    dhcp->secs = 0;
    dhcp->flags = htons(0x8000); // просим сервер отвечать broadcast'ом — unicast-приём нам ещё недоступен (нет IP)
    for (int i = 0; i < 6; i++) dhcp->chaddr[i] = my_mac[i];
    dhcp->magic_cookie = bswap32(DHCP_MAGIC_COOKIE);

    uint8_t* opt = (uint8_t*)dhcp->options;
    int pos = 0;
    uint8_t type_byte = msg_type;
    pos += dhcp_put_option(opt + pos, 53, 1, &type_byte); // DHCP Message Type
    if (msg_type == 3) { // DHCPREQUEST — подтверждаем конкретное предложение
        pos += dhcp_put_option(opt + pos, 50, 4, g_dhcp_offered_ip); // Requested IP Address
        pos += dhcp_put_option(opt + pos, 54, 4, g_dhcp_server_ip);  // Server Identifier
    }
    uint8_t params[3] = {1, 3, 6}; // Subnet Mask, Router, Domain Name Server
    pos += dhcp_put_option(opt + pos, 55, 3, params); // Parameter Request List
    opt[pos++] = 255; // End

    // Фиксированная часть заголовка (op..magic_cookie) — ровно 240 байт.
    int dhcp_len = 240 + pos;

    udp->src_port = htons(DHCP_CLIENT_PORT);
    udp->dst_port = htons(DHCP_SERVER_PORT);
    udp->len = htons(sizeof(udp_header) + dhcp_len);
    udp->checksum = 0;

    ip->ihl_version = 0x45; ip->tos = 0;
    ip->tot_len = htons(sizeof(ipv4_header) + sizeof(udp_header) + dhcp_len);
    ip->id = htons(0xD4C7); ip->frag_off = 0; ip->ttl = 64; ip->protocol = 17;
    ip->saddr[0] = 0; ip->saddr[1] = 0; ip->saddr[2] = 0; ip->saddr[3] = 0; // RFC 2131: до ACK клиент использует 0.0.0.0
    ip->daddr[0] = 255; ip->daddr[1] = 255; ip->daddr[2] = 255; ip->daddr[3] = 255;
    ip->check = 0;
    ip->check = calculate_checksum((void*)ip, sizeof(ipv4_header));

    sys_puts(console_ep, msg_type == 1 ? "[NET] DHCP: sending DISCOVER...\n" : "[NET] DHCP: sending REQUEST...\n");
    net_send_packet(14 + sizeof(ipv4_header) + sizeof(udp_header) + dhcp_len, tx_offset);
}

// Запускает/повторяет получение адреса — вызывается раз за итерацию главного
// цикла (см. main()), сама решает, нужно ли что-то слать. Не делает ничего,
// пока нет линка (net_check_link_status сбрасывает состояние в DHCP_IDLE при
// отключении кабеля — см. там же).
static void net_check_dhcp(seL4_CPtr console_ep, seL4_CPtr timer_ep) {
    if (!g_phy_link_up) return;
    uint64_t now = sys_get_uptime_ms(timer_ep);

    if (g_dhcp_state == DHCP_IDLE) {
        g_dhcp_xid = (uint32_t)now ^ 0xA5A5A5A5u; // без аппаратного RNG — сойдёт и так, лишь бы не 0
        sys_puts(console_ep, "[NET] Starting DHCP discovery...\n");
        net_send_dhcp_packet(console_ep, 1 /* DHCPDISCOVER */);
        g_dhcp_state = DHCP_DISCOVERING;
        // Backoff: 4с, 8с, 16с, 32с, дальше потолок 60с — если сервера в сети
        // просто нет, не заваливаем консоль строкой каждые 4 секунды вечно
        // (мешает вводу команд в shell).
        uint32_t shift = (g_dhcp_attempt < 5) ? g_dhcp_attempt : 5;
        uint64_t interval = DHCP_RETRY_MS << shift;
        if (interval > DHCP_RETRY_MAX_MS) interval = DHCP_RETRY_MAX_MS;
        g_dhcp_retry_uptime_ms = now + interval;
        g_dhcp_attempt++;
    } else if (g_dhcp_state == DHCP_DISCOVERING || g_dhcp_state == DHCP_REQUESTING) {
        if (now >= g_dhcp_retry_uptime_ms) {
            sys_puts(console_ep, "[NET] DHCP: no response, retrying...\n");
            g_dhcp_state = DHCP_IDLE; // следующая итерация начнёт DISCOVER заново
        }
    } else if (g_dhcp_state == DHCP_BOUND) {
        if (g_dhcp_lease_deadline_uptime_ms != 0 && now >= g_dhcp_lease_deadline_uptime_ms) {
            sys_puts(console_ep, "[NET] DHCP: lease renewal due, restarting discovery...\n");
            g_dhcp_state = DHCP_IDLE;
            g_dhcp_bound = false;
        }
    }
}

static void net_send_ping(seL4_CPtr console_ep, seL4_CPtr timer_ep, const uint8_t dst_ip[4]) {
    uint16_t seq = ++g_ping_next_seq;
    if (seq == 0) seq = ++g_ping_next_seq;
    volatile ethernet_frame* eth = (volatile ethernet_frame*)(g_shm_vaddr + 0x280);

    uint8_t dest_mac[6];
    resolve_dest_mac(dst_ip, dest_mac); // вызывающий код уже убедился, что резолв готов
    for(int i=0; i<6; i++) eth->dest_mac[i] = dest_mac[i];
    for(int i=0; i<6; i++) eth->src_mac[i] = my_mac[i];
    eth->ethertype = htons(0x0800); 
    
    volatile ipv4_header* ip = (volatile ipv4_header*)eth->payload;
    ip->ihl_version = 0x45; ip->tos = 0; ip->tot_len = htons(sizeof(ipv4_header) + sizeof(icmp_header) + PING_PAYLOAD_LEN);
    ip->id = htons(0x1234); ip->frag_off = 0; ip->ttl = 64; ip->protocol = 1; ip->check = 0;
    for (int i = 0; i < 4; i++) ip->saddr[i] = my_ip[i];
    for (int i = 0; i < 4; i++) ip->daddr[i] = dst_ip[i];
    ip->check = calculate_checksum((void*)ip, sizeof(ipv4_header));

    volatile icmp_header* icmp = (volatile icmp_header*)(eth->payload + sizeof(ipv4_header));
    icmp->type = 8; icmp->code = 0; icmp->id = htons(0x1337); icmp->sequence = htons(seq); icmp->checksum = 0;
    char* data = (char*)icmp + sizeof(icmp_header);
    for (int i = 0; i < PING_PAYLOAD_LEN; i++) data[i] = (char)i; // как у обычного ping — просто заполнитель
    icmp->checksum = calculate_checksum((void*)icmp, sizeof(icmp_header) + PING_PAYLOAD_LEN);

    // Не печатаем ничего при отправке — обычный unix ping тоже молчит до
    // ответа (или таймаута/потери, о которых он тоже не печатает построчно,
    // только в итоговой статистике, см. net_schedule_next_ping).

    g_ping_sent_ms = sys_get_uptime_ms(timer_ep); // грубый таймаут (2с) — этого разрешения достаточно
    if (g_cntfrq == 0) g_cntfrq = read_cntfrq();
    g_ping_sent_cyc = read_cntvct(); // точный RTT — см. read_cntvct() выше
    g_ping_outstanding_seq = seq;
    g_ping_outstanding = true;
    g_ping_sent_count++;
    g_ping_series_sent++;

    net_send_packet(14 + sizeof(ipv4_header) + sizeof(icmp_header) + PING_PAYLOAD_LEN, 0x280);
}

// Смещения в SHM для передачи shell'у статистики завершённой серии ping —
// той же по духу приём, что и резолвленный DNS IP на +4064 (см. net_poll):
// пишем перед тем, как разблокировать mailbox, shell читает сразу после и
// печатает "--- ... ping statistics ---" в стиле обычного unix ping.
// Каждое поле — uint32, все 7 умещаются перед концом первой страницы SHM.
static void net_publish_ping_stats(seL4_CPtr timer_ep) {
    uint32_t avg_us = g_ping_series_reply > 0 ? (uint32_t)(g_ping_series_total_rtt_us / g_ping_series_reply) : 0;
    uint64_t variance = 0;
    if (g_ping_series_reply > 0) {
        uint64_t mean_sq = (uint64_t)avg_us * (uint64_t)avg_us;
        uint64_t sq_avg = g_ping_series_total_rtt_sq_us / g_ping_series_reply;
        variance = (sq_avg > mean_sq) ? (sq_avg - mean_sq) : 0; // integer truncation могла бы дать чуть отрицательное
    }
    uint32_t mdev_us = (uint32_t)isqrt64(variance);
    uint32_t elapsed_ms = (uint32_t)(sys_get_uptime_ms(timer_ep) - g_ping_series_start_ms);

    *(uint32_t*)(g_shm_vaddr + 4068) = g_ping_series_sent;
    *(uint32_t*)(g_shm_vaddr + 4072) = g_ping_series_reply;
    *(uint32_t*)(g_shm_vaddr + 4076) = (uint32_t)g_ping_series_min_rtt_us;
    *(uint32_t*)(g_shm_vaddr + 4080) = (uint32_t)g_ping_series_max_rtt_us;
    *(uint32_t*)(g_shm_vaddr + 4084) = avg_us;
    *(uint32_t*)(g_shm_vaddr + 4088) = mdev_us;
    *(uint32_t*)(g_shm_vaddr + 4092) = elapsed_ms;
}

static void net_schedule_next_ping(seL4_CPtr timer_ep) {
    if (g_ping_series_remaining > 0) {
        g_ping_next_send_ms = sys_get_time_ms(timer_ep) + 1000; // Пауза ровно 1 секунда через RTC!
    } else {
        net_publish_ping_stats(timer_ep);
        volatile int* net_mailbox = (volatile int*)(g_shm_vaddr + 4060);
        net_mailbox[0] = 0; // Готово, разблокируем Shell
    }
}

static void net_send_next_ping(seL4_CPtr console_ep, seL4_CPtr timer_ep) {
    if (g_ping_outstanding || g_ping_series_remaining == 0) return;
    g_ping_series_remaining--;
    net_send_ping(console_ep, timer_ep, g_ping_target_ip);
}

static void net_check_ping_send(seL4_CPtr console_ep, seL4_CPtr timer_ep) {
    if (g_ping_outstanding || g_ping_series_remaining == 0) return;
    if (g_ping_next_send_ms == 0 || sys_get_time_ms(timer_ep) >= g_ping_next_send_ms) {
        g_ping_next_send_ms = 0;
        net_send_next_ping(console_ep, timer_ep);
    }
}

static void net_start_ping_series(seL4_CPtr console_ep, seL4_CPtr timer_ep, const uint8_t dst_ip[4], uint32_t count) {
    if (count == 0) count = 1; if (count > 16) count = 16;
    for (int i = 0; i < 4; i++) g_ping_target_ip[i] = dst_ip[i];
    g_ping_series_remaining = count; g_ping_outstanding = false; g_ping_next_send_ms = 0;
    g_ping_series_sent = 0; g_ping_series_reply = 0;
    g_ping_series_min_rtt_us = 0; g_ping_series_max_rtt_us = 0;
    g_ping_series_total_rtt_us = 0; g_ping_series_total_rtt_sq_us = 0;
    g_ping_series_start_ms = sys_get_uptime_ms(timer_ep);
    net_check_ping_send(console_ep, timer_ep); // Шлем первый пакет сразу
}

static void net_record_ping_rtt(uint64_t rtt_us) {
    g_ping_reply_count++; g_ping_last_rtt_us = rtt_us; g_ping_total_rtt_us += rtt_us;
    if (g_ping_min_rtt_us == 0 || rtt_us < g_ping_min_rtt_us) g_ping_min_rtt_us = rtt_us;
    if (rtt_us > g_ping_max_rtt_us) g_ping_max_rtt_us = rtt_us;

    g_ping_series_reply++;
    g_ping_series_total_rtt_us += rtt_us;
    g_ping_series_total_rtt_sq_us += rtt_us * rtt_us;
    if (g_ping_series_min_rtt_us == 0 || rtt_us < g_ping_series_min_rtt_us) g_ping_series_min_rtt_us = rtt_us;
    if (rtt_us > g_ping_series_max_rtt_us) g_ping_series_max_rtt_us = rtt_us;
}

static void net_check_ping_timeout(seL4_CPtr console_ep, seL4_CPtr timer_ep) {
    if (!g_ping_outstanding) return;

    if (sys_get_uptime_ms(timer_ep) - g_ping_sent_ms < PING_TIMEOUT_MS) return;

    // Молчим по каждому потерянному пакету — как обычный unix ping, который
    // тоже ничего не печатает построчно на таймаут, только в итоговом
    // packet loss% (см. net_publish_ping_stats). g_ping_timeout_count всё
    // равно считается — используется в netstat.
    g_ping_outstanding = false; g_ping_timeout_count++;
    net_schedule_next_ping(timer_ep);
}

static void net_send_udp(seL4_CPtr console_ep, const uint8_t dst_ip[4], uint16_t dst_port, const char* message) {
    volatile ethernet_frame* eth = (volatile ethernet_frame*)(g_shm_vaddr + 0x280);
    uint8_t dest_mac[6];
    resolve_dest_mac(dst_ip, dest_mac); // вызывающий код уже убедился, что резолв готов
    for(int i=0; i<6; i++) eth->dest_mac[i] = dest_mac[i];
    for(int i=0; i<6; i++) eth->src_mac[i] = my_mac[i];
    eth->ethertype = htons(0x0800);

    int msg_len = my_strlen(message);
    
    // IP Заголовок (Протокол 17 = UDP)
    volatile ipv4_header* ip = (volatile ipv4_header*)eth->payload;
    ip->ihl_version = 0x45; ip->tos = 0; 
    ip->tot_len = htons(sizeof(ipv4_header) + sizeof(udp_header) + msg_len); 
    ip->id = htons(0x7777); ip->frag_off = 0; ip->ttl = 64; 
    ip->protocol = 17; // 17 = UDP
    ip->check = 0;
    for (int i = 0; i < 4; i++) ip->saddr[i] = my_ip[i];
    for (int i = 0; i < 4; i++) ip->daddr[i] = dst_ip[i];
    ip->check = calculate_checksum((void*)ip, sizeof(ipv4_header));

    // UDP Заголовок
    volatile udp_header* udp = (volatile udp_header*)(eth->payload + sizeof(ipv4_header));
    udp->src_port = htons(50000);  // Любой случайный порт
    udp->dst_port = htons(dst_port);
    udp->len = htons(sizeof(udp_header) + msg_len);
    udp->checksum = 0; // Для UDP контрольная сумма необязательна! (Лень - двигатель прогресса)

    // Текст сообщения
    char* data = (char*)udp + sizeof(udp_header);
    my_memcpy(data, message, msg_len);

    sys_puts(console_ep, "[NET] Firing UDP Datagram to ");
    put_ip(console_ep, dst_ip);
    sys_puts(console_ep, ":");
    put_dec(console_ep, dst_port);
    sys_puts(console_ep, "...\n");
    net_send_packet(14 + sizeof(ipv4_header) + sizeof(udp_header) + my_strlen(message), 0x280);
    volatile int* net_mailbox = (volatile int*)(g_shm_vaddr + 4060);
    net_mailbox[0] = 0;
}

// Отправляет SNTP client-запрос (mode=3) напрямую на g_ntp_server_ip:123,
// маршрутизируя через уже разрешенный router_mac (тот же прием, что и DNS-запрос
// на 8.8.8.8 выше — адресат вне локальной подсети, но канальный уровень идет на гейтвей).
static void net_send_ntp_request(seL4_CPtr console_ep, seL4_CPtr timer_ep) {
    uint32_t tx_offset = 0x280;

    volatile ethernet_frame* eth = (volatile ethernet_frame*)(g_shm_vaddr + tx_offset);
    uint8_t dest_mac[6];
    resolve_dest_mac(g_ntp_server_ip, dest_mac); // вызывающий код уже убедился, что резолв готов
    for (int i = 0; i < 6; i++) eth->dest_mac[i] = dest_mac[i];
    for (int i = 0; i < 6; i++) eth->src_mac[i] = my_mac[i];
    eth->ethertype = htons(0x0800);

    volatile udp_header* udp = (volatile udp_header*)(eth->payload + sizeof(ipv4_header));
    udp->src_port = htons(50123);
    udp->dst_port = htons(NTP_SERVER_PORT);
    udp->len = htons(sizeof(udp_header) + sizeof(ntp_packet));
    udp->checksum = 0;

    volatile ntp_packet* ntp = (volatile ntp_packet*)((char*)udp + sizeof(udp_header));
    for (uint32_t i = 0; i < sizeof(ntp_packet); i++) ((volatile uint8_t*)ntp)[i] = 0;
    ntp->li_vn_mode = 0x23; // LI=0, VN=4, Mode=3 (client)

    // T1: наш "текущий" момент отправки — используется вместе с T2..T4 в формуле
    // офсета ((T2-T1)+(T3-T4))/2 ниже, при получении ответа.
    uint64_t our_epoch_s = sys_get_time_ms(timer_ep) / 1000ULL;
    g_ntp_t1_epoch_s = our_epoch_s;
    ntp->tx_ts_sec = bswap32((uint32_t)(our_epoch_s + NTP_UNIX_EPOCH_DELTA));
    ntp->tx_ts_frac = 0;

    volatile ipv4_header* ip = (volatile ipv4_header*)eth->payload;
    ip->ihl_version = 0x45; ip->tos = 0;
    ip->tot_len = htons(sizeof(ipv4_header) + sizeof(udp_header) + sizeof(ntp_packet));
    ip->id = htons(0x4E54); ip->frag_off = 0; ip->ttl = 64; ip->protocol = 17;
    for (int i = 0; i < 4; i++) ip->saddr[i] = my_ip[i];
    for (int i = 0; i < 4; i++) ip->daddr[i] = g_ntp_server_ip[i];
    ip->check = 0;
    ip->check = calculate_checksum((void*)ip, sizeof(ipv4_header));

    sys_puts(console_ep, "[NET] Sending NTP request to ");
    put_ip(console_ep, g_ntp_server_ip);
    sys_puts(console_ep, ":123...\n");

    net_send_packet(14 + sizeof(ipv4_header) + sizeof(udp_header) + sizeof(ntp_packet), tx_offset);
    g_ntp_outstanding = true;
}

// Ставит в план следующую автоматическую ресинхронизацию через NTP_RESYNC_INTERVAL_MS
// от текущего аптайма (не от показаний часов — не зависит от самой коррекции).
static void net_schedule_next_ntp_resync(seL4_CPtr timer_ep) {
    g_ntp_next_resync_uptime_ms = sys_get_uptime_ms(timer_ep) + NTP_RESYNC_INTERVAL_MS;
}

// Сигналит rootserver'у готовность net_driver. Вызывается один раз сразу
// после net_hw_init() (успешного или нет) — NTP-синхронизация (если вообще
// возможна) идёт полностью в фоне через обычный периодический ресинк (см.
// net_check_ntp_resync() ниже) и не задерживает загрузку/shell ни на
// секунду, даже если кабель не воткнут или сеть недоступна.
static void signal_net_driver_ready(seL4_CPtr root_ep) {
    seL4_SetMR(0, SYS_DRIVER_READY);
    seL4_Call(root_ep, seL4_MessageInfo_new(0, 0, 0, 1));
}

// Периодически перезапускает NTP-синхронизацию, не дожидаясь команды `ntp` из shell.
// Не мешает уже идущему запросу (ручному, загрузочному или предыдущему плановому).
static void net_check_ntp_resync(seL4_CPtr console_ep, seL4_CPtr timer_ep) {
    if (g_ntp_outstanding || pending_cmd == NET_CMD_NTP) return;
    if (sys_get_uptime_ms(timer_ep) < g_ntp_next_resync_uptime_ms) return;
    if (!g_dhcp_bound) { net_schedule_next_ntp_resync(timer_ep); return; } // нет IP — нечего слать, пробуем позже

    sys_puts(console_ep, "[NET] Periodic NTP resync...\n");
    uint8_t dummy_mac[6];
    if (resolve_dest_mac(g_ntp_server_ip, dummy_mac)) {
        net_send_ntp_request(console_ep, timer_ep);
    } else {
        pending_cmd = NET_CMD_NTP;
        uint8_t arp_target[4];
        arp_target_for(g_ntp_server_ip, arp_target);
        net_send_arp_request(console_ep, arp_target);
    }
    net_schedule_next_ntp_resync(timer_ep);
}

static void unpack_ipv4(seL4_Word packed, uint8_t out[4]) {
    out[0] = (uint8_t)((packed >> 24) & 0xFF);
    out[1] = (uint8_t)((packed >> 16) & 0xFF);
    out[2] = (uint8_t)((packed >> 8) & 0xFF);
    out[3] = (uint8_t)(packed & 0xFF);
}

static void copy_text_from_mrs(char *dst, int max_len, int text_len, int msg_words) {
    const int word_bytes = sizeof(seL4_Word);
    int available = (msg_words - 4) * word_bytes;
    if (text_len > available) text_len = available;
    if (text_len < 0) text_len = 0;
    if (text_len >= max_len) text_len = max_len - 1;

    for (int i = 0; i < text_len; i++) {
        seL4_Word word = seL4_GetMR(4 + (i / word_bytes));
        dst[i] = (char)((word >> ((i % word_bytes) * 8)) & 0xFF);
    }
    dst[text_len] = '\0';
}

static int dns_format_name(char* dst, const char* src) {
    int pos = 0, len_pos = 0, count = 0;
    for (int i = 0; src[i] != '\0'; i++) {
        if (src[i] == '.') {
            dst[len_pos] = (char)count;
            len_pos = pos + 1;
            count = 0;
        } else {
            dst[pos + 1] = src[i];
            count++;
        }
        pos++;
    }
    dst[len_pos] = (char)count;
    dst[pos + 1] = 0; // Нулевой байт в конце имени
    return pos + 2;
}

// Пропускает закодированное DNS-имя (в вопросе или в записи ответа) и
// возвращает указатель сразу после него. Имя может быть закодировано двумя
// способами (оба валидны по RFC 1035): последовательностью length-prefixed
// меток, завершённой нулевым байтом (как строит dns_format_name() выше), ИЛИ
// 2-байтовым compression-указателем (старшие 2 бита первого байта = 11) —
// сервер выбирает сам. Раньше код при разборе ОТВЕТА жёстко предполагал
// только указатель (фиксированные +=2), и на ответе с полным именем читал
// RDLENGTH из середины самого имени — отсюда бессмысленное "data_len=27904"
// на honest ответе от google.com.
static uint8_t* dns_skip_name(uint8_t* reader, uint8_t* buffer_end) {
    while (reader < buffer_end) {
        uint8_t len = *reader;
        if ((len & 0xC0) == 0xC0) return reader + 2; // указатель — всегда ровно 2 байта, имя тут кончается
        if (len == 0) return reader + 1;              // нулевой терминатор
        reader += (uint32_t)len + 1;
    }
    return reader;
}

static void net_send_dns_query(seL4_CPtr console_ep, const char* domain) {
    uint32_t tx_offset = 0x280;  // Возвращаем проверенное смещение

    volatile ethernet_frame* eth = (volatile ethernet_frame*)(g_shm_vaddr + tx_offset);

    uint8_t dest_mac[6];
    resolve_dest_mac(g_dns_ip, dest_mac); // вызывающий код уже убедился, что резолв готов
    for(int i = 0; i < 6; i++) eth->dest_mac[i] = dest_mac[i];
    for(int i = 0; i < 6; i++) eth->src_mac[i] = my_mac[i];
    eth->ethertype = htons(0x0800);

    char* qname = (char*)eth->payload + sizeof(ipv4_header) + sizeof(udp_header) + sizeof(dns_header);
    int name_len = dns_format_name(qname, domain);
    int dns_payload_len = sizeof(dns_header) + name_len + 4;
    int total_udp_len = sizeof(udp_header) + dns_payload_len;

    volatile udp_header* udp = (volatile udp_header*)(eth->payload + sizeof(ipv4_header));
    udp->src_port = htons(50053);
    udp->dst_port = htons(53);
    udp->len = htons(total_udp_len);
    udp->checksum = 0; 

    volatile ipv4_header* ip = (volatile ipv4_header*)eth->payload;
    ip->ihl_version = 0x45; ip->tos = 0;
    ip->tot_len = htons(sizeof(ipv4_header) + total_udp_len);
    ip->id = htons(0xABCD); ip->frag_off = 0; ip->ttl = 64; ip->protocol = 17;

    for (int i = 0; i < 4; i++) ip->saddr[i] = my_ip[i];

    // DNS-сервер: из DHCP (опция 6), либо запасной 8.8.8.8 (см. g_dns_ip) — он
    // переварит нулевую UDP чексумму.
    for (int i = 0; i < 4; i++) ip->daddr[i] = g_dns_ip[i];

    ip->check = 0;
    ip->check = calculate_checksum((void*)ip, sizeof(ipv4_header));

    volatile dns_header* dns = (volatile dns_header*)((char*)udp + sizeof(udp_header));
    dns->id = htons(g_dns_id);
    dns->flags = htons(0x0100);
    dns->q_count = htons(1);
    dns->ans_count = dns->auth_count = dns->add_count = 0;

    uint16_t* qtype = (uint16_t*)(qname + name_len);
    qtype[0] = htons(1); qtype[1] = htons(1);

    uint32_t packet_len = 14 + sizeof(ipv4_header) + total_udp_len;

    sys_puts(console_ep, "[NET] Sending DNS Query (");
    put_dec(console_ep, packet_len);
    sys_puts(console_ep, " bytes) to ");
    put_ip(console_ep, g_dns_ip);
    sys_puts(console_ep, "...\n");

    net_send_packet(packet_len, tx_offset);

    g_dns_outstanding = true;
}

// Команды, которым нужен собственный IP/шлюз (ping/send/resolve/ntp), должны
// дождаться g_dhcp_bound — иначе они уйдут слать ARP на g_gateway_ip=0.0.0.0,
// ответа не будет никогда, и shell зависнет на 10с до аварийного respawn'а
// net_driver'а (см. shell.cpp). Явная ошибка сразу + разблокировка mailbox —
// куда лучше такого зависания.
static bool net_require_ip(seL4_CPtr console_ep) {
    if (g_dhcp_bound) return true;
    sys_puts(console_ep, "[NET] Error: no IP address yet (DHCP pending). Try again shortly.\n");
    volatile int* net_mailbox = (volatile int*)(g_shm_vaddr + 4060);
    net_mailbox[0] = 0;
    return false;
}

static void net_handle_command(seL4_CPtr console_ep, seL4_CPtr timer_ep, seL4_CPtr net_cmd_ep) {
    if (net_cmd_ep == 0) return;

    seL4_Word sender_badge = 0;
    seL4_MessageInfo_t info = seL4_NBRecv(net_cmd_ep, &sender_badge);
    if (sender_badge == 0) return;

    int len = seL4_MessageInfo_get_length(info);
    if (len == 0) return;

    seL4_Word cmd = seL4_GetMR(0);

    if (cmd == NET_CMD_PING && len >= 3) {
        if (!net_require_ip(console_ep)) return;
        uint8_t dst_ip[4];
        unpack_ipv4(seL4_GetMR(1), dst_ip);
        uint32_t count = (uint32_t)seL4_GetMR(2);
        if (count == 0) count = 1;
        if (count > 16) count = 16;

        sys_puts(console_ep, "[NET] Shell requested ICMP Ping x");
        put_dec(console_ep, count);
        sys_puts(console_ep, ".\n");
        uint8_t dummy_mac[6];
        if (resolve_dest_mac(dst_ip, dummy_mac)) {
            net_start_ping_series(console_ep, timer_ep, dst_ip, count);
        } else {
            for (int i = 0; i < 4; i++) pending_ip[i] = dst_ip[i];
            pending_ping_count = count;
            pending_cmd = NET_CMD_PING;
            uint8_t arp_target[4];
            arp_target_for(dst_ip, arp_target);
            net_send_arp_request(console_ep, arp_target);
        }
    } else if (cmd == NET_CMD_SEND && len >= 4) {
        if (!net_require_ip(console_ep)) return;
        uint8_t dst_ip[4];
        unpack_ipv4(seL4_GetMR(1), dst_ip);
        uint16_t dst_port = (uint16_t)seL4_GetMR(2);
        int text_len = (int)seL4_GetMR(3);
        char msg[64];
        copy_text_from_mrs(msg, sizeof(msg), text_len, len);

        if (dst_port == 0) dst_port = default_udp_port;
        if (dst_ip[0] == 0 && dst_ip[1] == 0 && dst_ip[2] == 0 && dst_ip[3] == 0) {
            for (int i = 0; i < 4; i++) dst_ip[i] = default_udp_ip[i];
        }

        sys_puts(console_ep, "[NET] Shell requested UDP send.\n");
        uint8_t dummy_mac2[6];
        if (resolve_dest_mac(dst_ip, dummy_mac2)) {
            net_send_udp(console_ep, dst_ip, dst_port, msg);
        } else {
            for (int i = 0; i < 4; i++) pending_udp_ip[i] = dst_ip[i];
            pending_udp_port = dst_port;
            my_memcpy(pending_udp, msg, my_strlen(msg) + 1);
            pending_cmd = NET_CMD_SEND;
            uint8_t arp_target[4];
            arp_target_for(dst_ip, arp_target);
            net_send_arp_request(console_ep, arp_target);
        }
    } else if (cmd == NET_CMD_STATUS) {
        sys_puts(console_ep, "[NET] Status: genet=up link=");
        sys_puts(console_ep, g_phy_link_up ? "up" : "down");
        sys_puts(console_ep, " dhcp=");
        sys_puts(console_ep, g_dhcp_state == DHCP_BOUND ? "bound" :
                              g_dhcp_state == DHCP_DISCOVERING ? "discovering" :
                              g_dhcp_state == DHCP_REQUESTING ? "requesting" : "idle");
        sys_puts(console_ep, " ip=");
        put_ip(console_ep, my_ip);
        sys_puts(console_ep, " gw=");
        put_ip(console_ep, g_gateway_ip);
        sys_puts(console_ep, " mask=");
        put_ip(console_ep, g_subnet_mask);
        sys_puts(console_ep, " dns=");
        put_ip(console_ep, g_dns_ip);
        sys_puts(console_ep, " router_mac=");
        if (have_router_mac) {
            sys_puts(console_ep, "known ");
            for (int i = 0; i < 6; i++) {
                if (i > 0) sys_puts(console_ep, ":");
                put_hex_byte(console_ep, router_mac[i]);
            }
        } else {
            sys_puts(console_ep, "unknown");
        }
        sys_puts(console_ep, " onlink_peer=");
        if (g_have_onlink_mac) {
            put_ip(console_ep, g_onlink_ip);
            sys_puts(console_ep, "=");
            for (int i = 0; i < 6; i++) {
                if (i > 0) sys_puts(console_ep, ":");
                put_hex_byte(console_ep, g_onlink_mac[i]);
            }
        } else {
            sys_puts(console_ep, "none");
        }
        sys_puts(console_ep, " tx_idx=");
        put_dec(console_ep, g_tx_index);
        sys_puts(console_ep, " rx_c_idx=");
        put_dec(console_ep, g_rx_c_index);
        sys_puts(console_ep, " default_udp=");
        put_ip(console_ep, default_udp_ip);
        sys_puts(console_ep, ":");
        put_dec(console_ep, default_udp_port);
        sys_puts(console_ep, " ping_sent=");
        put_dec(console_ep, g_ping_sent_count);
        sys_puts(console_ep, " ping_reply=");
        put_dec(console_ep, g_ping_reply_count);
        sys_puts(console_ep, " ping_timeout=");
        put_dec(console_ep, g_ping_timeout_count);
        if (g_ping_reply_count > 0) {
            sys_puts(console_ep, " rtt_last=");
            put_duration_us(console_ep, g_ping_last_rtt_us);
            sys_puts(console_ep, " rtt_avg=");
            put_duration_us(console_ep, g_ping_total_rtt_us / g_ping_reply_count);
            sys_puts(console_ep, " rtt_min=");
            put_duration_us(console_ep, g_ping_min_rtt_us);
            sys_puts(console_ep, " rtt_max="); 
            put_duration_us(console_ep, g_ping_max_rtt_us);
        }
        sys_puts(console_ep, " ntp_synced=");
        sys_puts(console_ep, g_ntp_synced ? "yes" : "no");
        if (g_ntp_synced) {
            sys_puts(console_ep, " ntp_offset=");
            if (g_ntp_last_offset_negative) sys_puts(console_ep, "-");
            put_dec(console_ep, g_ntp_last_offset_s);
            sys_puts(console_ep, "s");
        }
        sys_puts(console_ep, "\n");
        volatile int* net_mailbox = (volatile int*)(g_shm_vaddr + 4060);
        net_mailbox[0] = 0;

    } else if (cmd == NET_CMD_RECV) {
        if (g_udp_rx_ready) {
            sys_puts(console_ep, "[NET] UDP datagram from ");
            put_ip(console_ep, g_udp_rx_src_ip);
            sys_puts(console_ep, ":");
            put_dec(console_ep, g_udp_rx_src_port);
            sys_puts(console_ep, " (");
            put_dec(console_ep, g_udp_rx_len);
            sys_puts(console_ep, " bytes): ");
            sys_puts(console_ep, g_udp_rx_data);
            sys_puts(console_ep, "\n");
            g_udp_rx_ready = false;
        } else {
            sys_puts(console_ep, "[NET] No pending UDP datagrams.\n");
        }
        volatile int* net_mailbox = (volatile int*)(g_shm_vaddr + 4060);
        net_mailbox[0] = 0;

    } else if (cmd == NET_CMD_RESOLVE) {
        if (!net_require_ip(console_ep)) return;
        int text_len = (int)seL4_GetMR(3);
        copy_text_from_mrs(g_dns_pending_domain, sizeof(g_dns_pending_domain), text_len, len);

        uint8_t dummy_mac3[6];
        if (resolve_dest_mac(g_dns_ip, dummy_mac3)) {
            net_send_dns_query(console_ep, g_dns_pending_domain);
        } else {
            pending_cmd = NET_CMD_RESOLVE;
            uint8_t arp_target[4];
            arp_target_for(g_dns_ip, arp_target);
            net_send_arp_request(console_ep, arp_target);
        }

    } else if (cmd == NET_CMD_NTP) {
        if (!net_require_ip(console_ep)) return;
        uint8_t dummy_mac4[6];
        if (g_ntp_outstanding) {
            sys_puts(console_ep, "[NET] NTP request already in flight.\n");
            volatile int* net_mailbox = (volatile int*)(g_shm_vaddr + 4060);
            net_mailbox[0] = 0;
        } else if (resolve_dest_mac(g_ntp_server_ip, dummy_mac4)) {
            net_send_ntp_request(console_ep, timer_ep);
        } else {
            pending_cmd = NET_CMD_NTP;
            uint8_t arp_target[4];
            arp_target_for(g_ntp_server_ip, arp_target);
            net_send_arp_request(console_ep, arp_target);
        }

    } else {
        sys_puts(console_ep, "[NET] Unknown Shell command.\n");
    }
}

// ========================================================
// ЖУРНАЛ ПРОИЗВОЛЬНЫХ UDP-ДАТАГРАММ (/root/net_udp.log)
// Раньше каждая такая датаграмма (mDNS/NetBIOS-broadcast от соседей по LAN —
// их оказалось МНОГО, десятками пачками) печаталась в консоль через
// sys_puts(), а это полноценный синхронный IPC + вывод на медленный UART —
// пока идёт печать, главный цикл не вызывает net_hw_poll_rx() и не может
// вовремя поймать настоящий ответ (например, на ping). См. blk_driver.cpp
// cmd=113 (SYS_WRITE_FILE, "echo > file") — тот же протокол, что и у shell.cpp
// (путь в SHM со смещения 0, данные со смещения 128). У blk_driver нет
// операции "дописать в конец" — поэтому храним весь журнал в своём буфере
// (g_udp_log_buf) и при каждой новой строке перезаписываем файл целиком его
// текущим содержимым — снаружи выглядит как append.
// ========================================================

static int fmt_dec(char* buf, uint32_t val) {
    char tmp[12]; int n = 0;
    if (val == 0) { buf[0] = '0'; return 1; }
    while (val > 0) { tmp[n++] = (char)('0' + (val % 10)); val /= 10; }
    for (int i = 0; i < n; i++) buf[i] = tmp[n - 1 - i];
    return n;
}

static int fmt_ip(char* buf, const uint8_t ip[4]) {
    int pos = 0;
    for (int i = 0; i < 4; i++) {
        if (i > 0) buf[pos++] = '.';
        pos += fmt_dec(buf + pos, ip[i]);
    }
    return pos;
}

// Общий межпроцессный спинлок на офсет 0/128 разделяемой SHM — та же
// физическая память и тот же офсет 4096, что и vfs_spinlock_ptr в shell.cpp
// (см. комментарий там). Нужен, потому что net_log_flush() ниже пишет в тот
// же офсет 0/128, что shell использует для ЛЮБОГО файлового syscall'а
// (ps/cat/touch/...) — без этого лока фоновая запись журнала (по приходу
// произвольного UDP-пакета, независимо от команд шелла) может перезаписать
// буфер посреди чужого запроса (был замечен на живом железе: `ps` иногда
// печатал "/root/net_udp.log" вместо таблицы процессов).
// ВАЖНО: обычные volatile чтение/запись, БЕЗ __sync_lock_test_and_set/
// __sync_lock_release. Эта SHM мапится некэшируемой Device-памятью
// (map_frame_robust(), main.cpp — ради когерентности с GENET DMA), а
// exclusive-load/store инструкции (LDXR/STXR, именно в них компилируются
// __sync_lock_*) на Device-памяти по спеке ARM имеют непредсказуемое
// поведение — именно это уронило ВЕСЬ kernel seL4 на живом железе
// необрабатываемым исключением шины ("halting... Kernel entry via Unknown
// (0)"), причём сразу при загрузке (net_log_flush() вызывается в main()
// безусловно), а не что-то специфичное для Wi-Fi. Настоящий hardware-atomic
// тут и не нужен: система однопроцессорная (SMP OFF, easy-settings.cmake) —
// переключение контекста происходит только на syscall/yield, а не посреди
// пары "прочитать/проверить -> записать" ниже.
static inline void net_vfs_lock() {
    volatile int* lock = (volatile int*)(g_shm_vaddr + 4096);
    while (*lock) seL4_Yield();
    *lock = 1;
}
static inline void net_vfs_unlock() {
    volatile int* lock = (volatile int*)(g_shm_vaddr + 4096);
    *lock = 0;
}

// Перезаписывает /root/net_udp.log целиком текущим содержимым g_udp_log_buf —
// вызывается и с пустым буфером один раз при старте (см. main()), чтобы
// журнал гарантированно очищался при каждом запуске net_driver, даже если за
// сессию не придёт ни одного пакета.
static void net_log_flush() {
    if (g_blk_ep == 0 || g_shm_vaddr == nullptr) return;
    net_vfs_lock();
    int path_len = my_strlen(NET_LOG_PATH);
    my_memcpy(g_shm_vaddr, NET_LOG_PATH, path_len + 1); // включая нуль-терминатор
    my_memcpy(g_shm_vaddr + 128, g_udp_log_buf, g_udp_log_len);
    seL4_SetMR(0, 113); // SYS_WRITE_FILE
    seL4_SetMR(1, g_udp_log_len);
    seL4_Call(g_blk_ep, seL4_MessageInfo_new(0, 0, 0, 2));
    net_vfs_unlock();
}

static void net_log_udp(seL4_CPtr timer_ep, const uint8_t src_ip[4], uint16_t src_port, int payload_len) {
    if (g_blk_ep == 0) return;

    char line[96];
    int pos = 0;
    line[pos++] = '[';
    pos += fmt_dec(line + pos, (uint32_t)sys_get_uptime_ms(timer_ep));
    line[pos++] = ']'; line[pos++] = ' ';
    pos += fmt_ip(line + pos, src_ip);
    line[pos++] = ':';
    pos += fmt_dec(line + pos, src_port);
    line[pos++] = ' '; line[pos++] = '(';
    pos += fmt_dec(line + pos, (uint32_t)payload_len);
    const char* suffix = " bytes)\n";
    for (const char* p = suffix; *p; p++) line[pos++] = *p;

    // Буфер почти заполнен — просто перестаём дописывать новые строки
    // (не бесконечно расти, не пытаться "докрутить" старые записи).
    if (g_udp_log_len + (uint32_t)pos > NET_LOG_BUF_CAP) return;

    my_memcpy(g_udp_log_buf + g_udp_log_len, line, pos);
    g_udp_log_len += (uint32_t)pos;
    net_log_flush();
}

static void net_poll(seL4_CPtr console_ep, seL4_CPtr timer_ep, seL4_CPtr root_ep) {
    uint8_t* frame_ptr;
    uint32_t frame_len;
    while (net_hw_poll_rx(&frame_ptr, &frame_len)) {
        volatile ethernet_frame* eth = (volatile ethernet_frame*)frame_ptr;

        uint16_t type = htons(eth->ethertype);

        if (type == 0x0806) { // ARP
            volatile arp_ipv4* arp_reply = (volatile arp_ipv4*)eth->payload;
            if (htons(arp_reply->oper) == 1) { // ARP-запрос от кого-то ещё
                uint8_t tpa[4]; for (int i = 0; i < 4; i++) tpa[i] = arp_reply->tpa[i];
                if (g_dhcp_bound && ip_eq(tpa, my_ip)) {
                    uint8_t requester_ip[4], requester_mac[6];
                    for (int i = 0; i < 4; i++) requester_ip[i] = arp_reply->spa[i];
                    for (int i = 0; i < 6; i++) requester_mac[i] = arp_reply->sha[i];
                    net_send_arp_reply(console_ep, requester_ip, requester_mac);
                }
            } else if (htons(arp_reply->oper) == 2) {
                uint8_t spa[4]; for (int i = 0; i < 4; i++) spa[i] = arp_reply->spa[i];
                bool is_gateway = ip_eq(spa, g_gateway_ip);
                bool already_had = is_gateway ? have_router_mac : (g_have_onlink_mac && ip_eq(spa, g_onlink_ip));

                if (!already_had) {
                    if (is_gateway) {
                        sys_puts(console_ep, "[NET RX] ARP Reply Received! Saving Router MAC.\n");
                        for(int i=0; i<6; i++) router_mac[i] = arp_reply->sha[i];
                        have_router_mac = true;
                    } else {
                        sys_puts(console_ep, "[NET RX] ARP Reply Received! Saving on-link peer MAC (");
                        put_ip(console_ep, spa);
                        sys_puts(console_ep, ").\n");
                        for (int i = 0; i < 4; i++) g_onlink_ip[i] = spa[i];
                        for(int i = 0; i < 6; i++) g_onlink_mac[i] = arp_reply->sha[i];
                        g_have_onlink_mac = true;
                    }

                    // Возобновляем ожидающую команду, только если резолвился именно
                    // тот MAC, которого она ждала — гейтвей и сосед по подсети
                    // резолвятся независимо и не мешают друг другу.
                    uint8_t dummy[6];
                    if (pending_cmd == NET_CMD_RESOLVE && resolve_dest_mac(g_dns_ip, dummy)) {
                        sys_puts(console_ep, "[NET] ARP ready. Launching DNS Query...\n");
                        pending_cmd = NET_CMD_NONE;
                        net_send_dns_query(console_ep, g_dns_pending_domain);
                    } else if (pending_cmd == NET_CMD_PING && resolve_dest_mac(pending_ip, dummy)) {
                        uint32_t count = pending_ping_count;
                        pending_cmd = NET_CMD_NONE;
                        net_start_ping_series(console_ep, timer_ep, pending_ip, count);
                    } else if (pending_cmd == NET_CMD_SEND && resolve_dest_mac(pending_udp_ip, dummy)) {
                        net_send_udp(console_ep, pending_udp_ip, pending_udp_port, pending_udp);
                        pending_cmd = NET_CMD_NONE;
                    } else if (pending_cmd == NET_CMD_NTP && resolve_dest_mac(g_ntp_server_ip, dummy)) {
                        pending_cmd = NET_CMD_NONE;
                        net_send_ntp_request(console_ep, timer_ep);
                    }
                }
            }
        }
        else if (type == 0x0800) { // IPv4
            volatile ipv4_header* ip = (volatile ipv4_header*)eth->payload;
            
            if (ip->protocol == 1) { // ICMP
                volatile icmp_header* icmp = (volatile icmp_header*)(eth->payload + (ip->ihl_version & 0x0F) * 4);
                if (icmp->type == 0 && htons(icmp->id) == 0x1337) { 
                    uint16_t seq = htons(icmp->sequence);
                    uint32_t ip_header_len = (ip->ihl_version & 0x0F) * 4;
                    uint32_t ip_total_len = htons(ip->tot_len);
                    uint32_t icmp_bytes = (ip_total_len > ip_header_len) ? (ip_total_len - ip_header_len) : 0;
                    bool matched = g_ping_outstanding && seq == g_ping_outstanding_seq;
                    // Честные микросекунды через аппаратный счётчик (read_cntvct/g_cntfrq
                    // выше) — раньше через timer_ep IPC, разрешение которого только целые
                    // миллисекунды (отсюда всегда ".000" для локальных хостов с реальным
                    // RTT сильно меньше 1мс), а до этого — вообще произвольная оценка по
                    // числу циклов главного цикла, никак не привязанная к настоящему времени.
                    uint64_t rtt_us = matched ? ((read_cntvct() - g_ping_sent_cyc) * 1000000ULL) / g_cntfrq : 0;
                    uint8_t src_ip[4];
                    for (int i = 0; i < 4; i++) src_ip[i] = ip->saddr[i];

                    // Молчим на неожиданные/опоздавшие ответы (не наш текущий
                    // seq) — как обычный unix ping, который тоже не печатает
                    // построчный шум на дубликаты/чужие эхо-ответы.
                    if (matched) {
                        put_dec(console_ep, icmp_bytes);
                        sys_puts(console_ep, " bytes from "); put_ip(console_ep, src_ip);
                        sys_puts(console_ep, ": icmp_seq="); put_dec(console_ep, seq);
                        sys_puts(console_ep, " ttl="); put_dec(console_ep, ip->ttl);
                        sys_puts(console_ep, " time=");
                        put_duration_us(console_ep, rtt_us);
                        sys_puts(console_ep, "\n");

                        g_ping_outstanding = false;
                        net_record_ping_rtt(rtt_us);
                        net_schedule_next_ping(timer_ep);
                    }
                }
            }
            
            else if (ip->protocol == 17) { // UDP
                volatile udp_header* udp = (volatile udp_header*)(eth->payload + (ip->ihl_version & 0x0F) * 4);

                if (htons(udp->src_port) == 53) {
                    sys_puts(console_ep, ">>> [NET RX] UDP Response from Port 53 captured!\n");
                    
                    volatile dns_header* dns = (volatile dns_header*)((char*)udp + sizeof(udp_header));
                    
                    if (htons(dns->id) == g_dns_id && htons(dns->ans_count) > 0) {
                        // Буфер этого RX-дескриптора занимает ровно GENET_RX_BUF_LENGTH
                        // байт начиная с eth (см. genet_rx_descs_init()); reader не должен
                        // уходить за эту границу, иначе — неограниченный OOB read при
                        // DNS-ответе без корректного нуль-терминатора имени.
                        uint8_t* buffer_end = (uint8_t*)eth + (GENET_RX_BUF_LENGTH - GENET_RX_BUF_OFFSET);
                        uint8_t* reader = (uint8_t*)dns + sizeof(dns_header);

                        // Пропускаем QNAME вопроса, затем QTYPE/QCLASS (4 байта).
                        reader = dns_skip_name(reader, buffer_end);
                        bool dns_ok = (reader + 4) <= buffer_end;
                        if (dns_ok) reader += 4;

                        // Пропускаем NAME первой записи ответа — сервер вправе прислать её
                        // и как 2-байтовый compression-указатель (обычно так), и как полное
                        // имя целиком (тоже валидно, просто реже встречается — именно так
                        // ответил этот роутер на google.com). dns_skip_name() понимает оба
                        // варианта; раньше здесь был жёсткий +=2 в расчёте только на
                        // указатель, и на полном имени RDLENGTH читался из середины самого
                        // имени — отсюда бессмысленное "Length=27904" на честном ответе.
                        if (dns_ok) {
                            reader = dns_skip_name(reader, buffer_end);
                            dns_ok = (reader + 8 + 2) <= buffer_end;
                        }

                        volatile int* net_mailbox = (volatile int*)(g_shm_vaddr + 4060);

                        if (dns_ok) {
                            reader += 8; // TYPE(2) + CLASS(2) + TTL(4)
                            uint16_t data_len = (reader[0] << 8) | reader[1];
                            reader += 2;

                            if (data_len == 4 && (reader + 4) <= buffer_end) { // Это точно IPv4 адрес!
                                uint8_t resolved_ip[4] = {reader[0], reader[1], reader[2], reader[3]};

                                sys_puts(console_ep, ">>> [NET RX] DNS SUCCESS: ");
                                put_ip(console_ep, resolved_ip); sys_puts(console_ep, "\n");

                                seL4_Word packed_ip = (resolved_ip[0] << 24) | (resolved_ip[1] << 16) |
                                                    (resolved_ip[2] << 8) | resolved_ip[3];
                                *((seL4_Word*)(g_shm_vaddr + 4064)) = packed_ip;

                                net_mailbox[0] = 0;
                                g_dns_outstanding = false;
                            } else {
                                sys_puts(console_ep, ">>> [NET RX] DNS Error: Unexpected data_len (CNAME?). Length=");
                                put_dec(console_ep, data_len); sys_puts(console_ep, "\n");
                                // Не A-запись (например, честный CNAME) — не блокируем shell
                                // на 10с/respawn, а сразу сообщаем, что резолвить нечего.
                                net_mailbox[0] = 0;
                                g_dns_outstanding = false;
                            }
                        } else {
                            sys_puts(console_ep, ">>> [NET RX] DNS Error: malformed/truncated response, ignored.\n");
                            net_mailbox[0] = 0;
                            g_dns_outstanding = false;
                        }
                    } else {
                        sys_puts(console_ep, ">>> [NET RX] DNS Ignored: ID mismatch or 0 answers.\n");
                    }
                } else if (htons(udp->src_port) == NTP_SERVER_PORT && g_ntp_outstanding) {
                    // Ответ SNTP-сервера на наш запрос из net_send_ntp_request().
                    uint8_t* buffer_end = (uint8_t*)eth + (GENET_RX_BUF_LENGTH - GENET_RX_BUF_OFFSET);
                    uint8_t* data_ptr = (uint8_t*)udp + sizeof(udp_header);
                    volatile int* net_mailbox = (volatile int*)(g_shm_vaddr + 4060);
                    g_ntp_outstanding = false;

                    if (data_ptr + sizeof(ntp_packet) > buffer_end) {
                        sys_puts(console_ep, "[NET] NTP Error: truncated response, ignored.\n");
                        net_mailbox[0] = 0;
                    } else {
                        volatile ntp_packet* ntp = (volatile ntp_packet*)data_ptr;
                        // Mode=4 (server) ожидается в ответ на наш Mode=3 (client) запрос;
                        // stratum=0 значит Kiss-o'-Death (сервер отказывается отвечать).
                        if (ntp->stratum == 0 || (ntp->li_vn_mode & 0x07) != 4) {
                            sys_puts(console_ep, "[NET] NTP Error: Kiss-o'-Death or unexpected mode, ignored.\n");
                            net_mailbox[0] = 0;
                        } else {
                            uint64_t t1 = g_ntp_t1_epoch_s;
                            uint64_t t2 = (uint64_t)bswap32(ntp->rx_ts_sec) - NTP_UNIX_EPOCH_DELTA;
                            uint64_t t3 = (uint64_t)bswap32(ntp->tx_ts_sec) - NTP_UNIX_EPOCH_DELTA;
                            uint64_t t4 = sys_get_time_ms(timer_ep) / 1000ULL;

                            // Стандартная формула офсета NTP; секундной точности достаточно
                            // для этой коррекции (округляем миллисекунды generic timer'а).
                            int64_t offset_s = ((int64_t)(t2 - t1) + (int64_t)(t3 - t4)) / 2;

                            seL4_SetMR(0, 5); // SYS_SET_TIME_OFFSET
                            seL4_SetMR(1, (seL4_Word)offset_s);
                            seL4_Call(timer_ep, seL4_MessageInfo_new(0, 0, 0, 2));

                            g_ntp_synced = true;
                            g_ntp_last_offset_negative = offset_s < 0;
                            g_ntp_last_offset_s = (uint32_t)(g_ntp_last_offset_negative ? -offset_s : offset_s);

                            sys_puts(console_ep, "[NET] NTP sync OK, offset ");
                            if (g_ntp_last_offset_negative) sys_puts(console_ep, "-");
                            put_dec(console_ep, g_ntp_last_offset_s);
                            sys_puts(console_ep, "s applied.\n");
                            net_mailbox[0] = 0;
                        }
                    }
                } else if (htons(udp->dst_port) == DHCP_CLIENT_PORT && htons(udp->src_port) == DHCP_SERVER_PORT) {
                    // Ответ DHCP-сервера (OFFER/ACK/NAK) на наш запрос из net_check_dhcp().
                    uint8_t* buffer_end = (uint8_t*)eth + (GENET_RX_BUF_LENGTH - GENET_RX_BUF_OFFSET);
                    volatile dhcp_packet* dhcp = (volatile dhcp_packet*)((char*)udp + sizeof(udp_header));

                    if ((uint8_t*)dhcp + 240 <= buffer_end && bswap32(dhcp->xid) == g_dhcp_xid &&
                        bswap32(dhcp->magic_cookie) == DHCP_MAGIC_COOKIE) {

                        uint8_t msg_type = 0;
                        uint8_t opt_mask[4] = {255, 255, 255, 0}; bool has_mask = false;
                        uint8_t opt_router[4] = {0, 0, 0, 0};     bool has_router = false;
                        uint8_t opt_dns[4] = {0, 0, 0, 0};        bool has_dns = false;
                        uint8_t opt_server[4] = {0, 0, 0, 0};
                        uint32_t lease_s = 0;

                        const uint8_t* p = (const uint8_t*)dhcp->options;
                        const uint8_t* opt_end = (const uint8_t*)dhcp + sizeof(dhcp_packet);
                        if (opt_end > buffer_end) opt_end = buffer_end;

                        while (p < opt_end && *p != 255) {
                            uint8_t code = *p++;
                            if (code == 0) continue; // pad
                            if (p >= opt_end) break;
                            uint8_t olen = *p++;
                            if (p + olen > opt_end) break;
                            if (code == 53 && olen >= 1) msg_type = p[0];
                            else if (code == 1 && olen >= 4) { for (int i = 0; i < 4; i++) opt_mask[i] = p[i]; has_mask = true; }
                            else if (code == 3 && olen >= 4) { for (int i = 0; i < 4; i++) opt_router[i] = p[i]; has_router = true; }
                            else if (code == 6 && olen >= 4) { for (int i = 0; i < 4; i++) opt_dns[i] = p[i]; has_dns = true; }
                            else if (code == 54 && olen >= 4) { for (int i = 0; i < 4; i++) opt_server[i] = p[i]; }
                            else if (code == 51 && olen >= 4) { lease_s = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
                            p += olen;
                        }

                        if (msg_type == 2 && g_dhcp_state == DHCP_DISCOVERING) { // DHCPOFFER
                            for (int i = 0; i < 4; i++) g_dhcp_offered_ip[i] = dhcp->yiaddr[i];
                            for (int i = 0; i < 4; i++) g_dhcp_server_ip[i] = opt_server[i];
                            sys_puts(console_ep, "[NET] DHCP: OFFER ");
                            put_ip(console_ep, g_dhcp_offered_ip);
                            sys_puts(console_ep, " received, requesting...\n");
                            net_send_dhcp_packet(console_ep, 3 /* DHCPREQUEST */);
                            g_dhcp_state = DHCP_REQUESTING;
                            g_dhcp_retry_uptime_ms = sys_get_uptime_ms(timer_ep) + DHCP_RETRY_MS;
                        } else if (msg_type == 5 && (g_dhcp_state == DHCP_REQUESTING || g_dhcp_state == DHCP_DISCOVERING)) { // DHCPACK
                            for (int i = 0; i < 4; i++) my_ip[i] = dhcp->yiaddr[i];
                            if (has_mask)   for (int i = 0; i < 4; i++) g_subnet_mask[i] = opt_mask[i];
                            if (has_router) for (int i = 0; i < 4; i++) g_gateway_ip[i] = opt_router[i];
                            if (has_dns)    for (int i = 0; i < 4; i++) g_dns_ip[i] = opt_dns[i];
                            if (lease_s == 0) lease_s = DHCP_DEFAULT_LEASE_S;

                            g_dhcp_state = DHCP_BOUND;
                            g_dhcp_bound = true;
                            g_dhcp_attempt = 0;
                            have_router_mac = false; // прежний MAC гейтвея (если был) мог устареть с новым адресом
                            g_have_onlink_mac = false; // новая подсеть — старый MAC соседа по ней уже неактуален

                            g_dhcp_lease_deadline_uptime_ms = sys_get_uptime_ms(timer_ep) + ((uint64_t)lease_s * 1000ULL) / 2ULL;

                            sys_puts(console_ep, "[NET] DHCP: bound ip="); put_ip(console_ep, my_ip);
                            sys_puts(console_ep, " gw="); put_ip(console_ep, g_gateway_ip);
                            sys_puts(console_ep, " mask="); put_ip(console_ep, g_subnet_mask);
                            sys_puts(console_ep, " dns="); put_ip(console_ep, g_dns_ip);
                            sys_puts(console_ep, " lease="); put_dec(console_ep, lease_s);
                            sys_puts(console_ep, "s\n");
                        } else if (msg_type == 6) { // DHCPNAK
                            sys_puts(console_ep, "[NET] DHCP: NAK received, restarting discovery.\n");
                            g_dhcp_state = DHCP_IDLE;
                            g_dhcp_bound = false;
                        }
                    }
                } else {
                    // Произвольная входящая UDP-датаграмма (не DNS) — сохраняем для команды `recv`.
                    // Границы буфера те же 1536 байт RX-дескриптора, что и в разборе DNS выше.
                    uint8_t* buffer_end = (uint8_t*)eth + (GENET_RX_BUF_LENGTH - GENET_RX_BUF_OFFSET);
                    uint8_t* data_ptr = (uint8_t*)udp + sizeof(udp_header);
                    uint16_t udp_len = htons(udp->len);
                    int payload_len = (udp_len > sizeof(udp_header)) ? (udp_len - sizeof(udp_header)) : 0;

                    int avail = (data_ptr < buffer_end) ? (int)(buffer_end - data_ptr) : 0;
                    if (payload_len > avail) payload_len = avail;
                    if (payload_len > (int)sizeof(g_udp_rx_data) - 1) payload_len = (int)sizeof(g_udp_rx_data) - 1;
                    if (payload_len < 0) payload_len = 0;

                    for (int i = 0; i < payload_len; i++) g_udp_rx_data[i] = (char)data_ptr[i];
                    g_udp_rx_data[payload_len] = '\0';
                    g_udp_rx_len = payload_len;

                    for (int i = 0; i < 4; i++) g_udp_rx_src_ip[i] = ip->saddr[i];
                    g_udp_rx_src_port = htons(udp->src_port);
                    g_udp_rx_ready = true;

                    // В консоль больше не пишем (см. комментарий у net_log_udp выше) —
                    // только в /root/net_udp.log, чтобы не тормозить главный цикл.
                    net_log_udp(timer_ep, g_udp_rx_src_ip, g_udp_rx_src_port, payload_len);
                }
            } // Конец проверки UDP
        } // Конец проверки IPv4

        net_hw_rx_done();
    }
}

int main(int argc, char *argv[]) {
    // 2. Достаем настоящий адрес буфера
    seL4_IPCBuffer *ipc = get_local_ipc();
    
    // 3. Отдаем его libsel4 (теперь её TLS инициализирован, и она сохранит его куда надо)
    seL4_SetIPCBuffer(ipc);

    seL4_CPtr console_ep = ipc->msg[BOOT_CONSOLE_EP];
    seL4_CPtr timer_ep   = ipc->msg[BOOT_TIMER_EP];
    seL4_CPtr net_cmd_ep = ipc->msg[BOOT_NET_EP];
    seL4_CPtr root_ep    = ipc->msg[BOOT_ROOT_EP];
    seL4_CPtr my_ep      = ipc->msg[BOOT_TIMER_EP];
    g_blk_ep = ipc->msg[7]; // см. main.cpp: local_blk_ep=7 — нужен только для net_log_udp()

    if (my_ep == 0) {
        __assert_fail("FATAL: Null Capability #0 Detected!", __FILE__, __LINE__, __func__);
    }

    sys_puts(console_ep, "\n[NET] Server online.\n");

    // =========================================================
    // 1. ДИНАМИЧЕСКИЙ ЗАПРОС SHM (Убираем хардкод)
    // =========================================================
    seL4_SetMR(0, 107); // 107 = SYS_SHM_GET
    seL4_MessageInfo_t msg = seL4_MessageInfo_new(0, 0, 0, 1);
    seL4_Call(root_ep, msg);
    
    g_shm_vaddr = (char*)seL4_GetMR(0);
    g_shm_paddr = (uint32_t)seL4_GetMR(1);

    if (g_shm_vaddr == nullptr || g_shm_paddr == 0) {
        sys_puts(console_ep, "[NET] FATAL: Failed to get dynamic SHM!\n");
        // Все равно сигналим готовность — иначе rootserver навечно зависнет
        // на wait_for_driver_ready() и не запустит остальные модули/shell.
        seL4_SetMR(0, SYS_DRIVER_READY);
        seL4_Call(root_ep, seL4_MessageInfo_new(0, 0, 0, 1));
        while(1) seL4_Yield();
    }
    // =========================================================
    // Очищаем /root/net_udp.log при каждом запуске (в т.ч. после watchdog-
    // респавна) — даже если за сессию не придёт ни одной датаграммы, старое
    // содержимое от прошлого запуска не должно вводить в заблуждение.
    net_log_flush();

    if (net_hw_init(console_ep, timer_ep)) {
        if (LOG_NET) sys_puts(console_ep, "[NET] GENET initialized.\n");
    } else {
        sys_puts(console_ep, "[NET] ERROR: GENET init failed, network unavailable.\n");
    }

    // Не ждём ни PHY-линка, ни NTP-синхронизации — сигналим готовность сразу,
    // иначе при отключённом кабеле/недоступной сети вся загрузка (включая
    // shell) вставала бы на паузу. NTP (если и когда сеть появится) идёт
    // полностью в фоне через обычный периодический ресинк — g_ntp_next_resync_uptime_ms
    // по умолчанию 0, поэтому net_check_ntp_resync() сама сделает первую
    // попытку уже на первой итерации цикла ниже, никакого отдельного
    // "boot-time sync" пути не нужно.
    signal_net_driver_ready(root_ep);

    while(1) {
        if (g_net_up) {
            net_check_link_status(console_ep, timer_ep);
            net_check_dhcp(console_ep, timer_ep);
            net_poll(console_ep, timer_ep, root_ep);
            net_handle_command(console_ep, timer_ep, net_cmd_ep);
            net_check_ping_timeout(console_ep, timer_ep);
            net_check_ping_send(console_ep, timer_ep);
            net_check_ntp_resync(console_ep, timer_ep);
        }
        seL4_Yield();
    }

    return 0;
}