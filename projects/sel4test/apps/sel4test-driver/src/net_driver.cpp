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

static bool ip_eq(const uint8_t a[4], const uint8_t b[4]) {
    for (int i = 0; i < 4; i++) if (a[i] != b[i]) return false;
    return true;
}

// --- DHCP-состояние (см. блок "DHCP-КЛИЕНТ" ниже за константами) ---
// DHCP_RENEWING — RFC 2131 RENEWING: на дедлайне T1 (половина lease) шлём
// UNICAST DHCPREQUEST напрямую серверу с ciaddr=текущий IP, НЕ бросая уже
// рабочий адрес сразу же (см. net_check_dhcp()/net_send_dhcp_packet()).
enum DhcpState { DHCP_IDLE = 0, DHCP_DISCOVERING, DHCP_REQUESTING, DHCP_BOUND, DHCP_RENEWING };

// Фаза 4.5 (Wi-Fi data-plane, см. situation.txt/план) — GENET и Wi-Fi работают
// ОДНОВРЕМЕННО, каждый со своим IP/шлюзом/маской/DNS/DHCP-арендой/ARP-кэшем/
// ping-DNS-NTP-состоянием. Раньше ВСЁ это (my_ip, g_gateway_ip, g_dhcp_state,
// router_mac, g_ping_*, g_dns_*, g_ntp_*, pending_cmd и т.д.) было плоскими
// глобалами на единственный (GENET) интерфейс — теперь это поля одной
// структуры, по экземпляру на интерфейс, и почти каждая функция протокольного
// слоя ниже получает параметр NetIface, говорящий, чей это вызов.
//
// НЕ входит сюда (остаётся аппаратно-специфичным, вне протокольного слоя):
// GENET-регистры/DMA-кольца/PHY (g_genet_base, g_net_up, rx_buffer_offsets,
// g_rx_c_index/g_rx_index/g_tx_index) и Wi-Fi SHM-мейлбокс (см. дальше по
// файлу, Фаза 4.5.3+) — это детали конкретного net_hw_*-бэкенда, видны
// только изнутри net_hw_send/poll_rx/rx_done.
enum NetIface { IFACE_GENET = 0, IFACE_WIFI = 1, IFACE_COUNT = 2 };

struct NetIfaceState {
    // --- Линк/адресация ---
    bool link_up;
    uint8_t mac[6];
    uint8_t ip[4];
    uint8_t gateway_ip[4];
    uint8_t subnet_mask[4];
    uint8_t dns_ip[4]; // публичный DNS как запасной вариант, пока DHCP не пришлёт свой (опция 6)

    // --- ARP ---
    uint8_t router_mac[6];
    bool have_router_mac;
    // Кэш MAC соседа по локальной подсети — на один слот (последний
    // резолвленный адресат), по аналогии с router_mac (см. ip_is_onlink()/
    // resolve_dest_mac() ниже — многие роутеры не отражают unicast-кадр
    // соседу по LAN обратно без этого, anti-spoofing/split-horizon).
    uint8_t onlink_ip[4];
    uint8_t onlink_mac[6];
    bool have_onlink_mac;

    // --- DHCP-клиент (RFC 2131) ---
    DhcpState dhcp_state;
    bool dhcp_bound;
    uint32_t dhcp_xid;
    uint8_t dhcp_offered_ip[4];
    uint8_t dhcp_server_ip[4];
    uint64_t dhcp_retry_uptime_ms;
    uint64_t dhcp_lease_deadline_uptime_ms; // 0 = не планировать перезапрос
    // Счётчик подряд неудачных попыток — растягивает интервал повтора (см.
    // net_check_dhcp), чтобы при отсутствии DHCP-сервера в сети не заваливать
    // консоль строкой каждые 4с вечно. Сбрасывается при ACK и новом линке.
    uint32_t dhcp_attempt;

    // --- Ping: накопительная статистика (для netstat) ---
    uint32_t ping_sent_count, ping_reply_count, ping_timeout_count;
    uint64_t ping_last_rtt_us, ping_min_rtt_us, ping_max_rtt_us, ping_total_rtt_us;
    // --- Ping: состояние ТЕКУЩЕГО одиночного echo request ---
    uint16_t ping_next_seq, ping_outstanding_seq;
    bool ping_outstanding;
    uint64_t ping_next_send_ms, ping_sent_ms, ping_sent_cyc;
    uint8_t ping_target_ip[4];
    // --- Ping: статистика ТЕКУЩЕЙ серии (для "--- ... ping statistics ---") ---
    uint32_t ping_series_remaining;
    uint32_t ping_series_sent, ping_series_reply;
    uint64_t ping_series_min_rtt_us, ping_series_max_rtt_us;
    uint64_t ping_series_total_rtt_us, ping_series_total_rtt_sq_us; // для mdev
    uint64_t ping_series_start_ms;

    // --- DNS ---
    char dns_pending_domain[64];
    bool dns_outstanding;
    uint16_t dns_id;

    // --- NTP ---
    bool ntp_outstanding;
    uint64_t ntp_t1_epoch_s; // T1: наши часы в момент отправки запроса, сек. с эпохи Unix
    uint32_t ntp_last_offset_s; // для диагностики (netstat) — знак отдельно
    bool ntp_last_offset_negative;
    bool ntp_synced;
    uint64_t ntp_next_resync_uptime_ms;
    uint64_t ntp_last_sync_uptime_ms; // для `ntp status` — 0, если ни разу не синхронизировались
    // Различает периодический авторесинк от ручной команды `ntp` — периодика
    // не должна печатать ничего в консоль (см. situation.txt), ручная должна.
    // Переживает ARP-резолв (см. pending_cmd==NET_CMD_NTP) — выставляется ДО
    // него, читается в net_send_ntp_request()/обработчике ответа.
    bool ntp_is_periodic;

    // --- Однослотовый мейлбокс произвольных входящих UDP-датаграмм (`recv`) ---
    bool udp_rx_ready;
    uint8_t udp_rx_src_ip[4];
    uint16_t udp_rx_src_port;
    char udp_rx_data[256];
    int udp_rx_len;

    // --- "Ожидающая резолва ARP" команда (см. net_handle_command/net_poll) ---
    int pending_cmd;
    uint8_t pending_ip[4];
    uint32_t pending_ping_count;
    uint8_t pending_udp_ip[4];
    uint16_t pending_udp_port;
    char pending_udp[64];
    // Фикс мейлбоксов->IPC (см. situation.txt): pending_cmd==NET_CMD_PING
    // ждёт резолва ARP БЕЗ вообще какого-либо таймаута (та же категория
    // бага, что чинили у blk_driver) — раньше это маскировалось тем, что
    // шелл сам ждал через wait_for_net_mailbox() с собственным таймаутом;
    // теперь шелл блокируется НАСТОЯЩИМ seL4_Call, и без явного дедлайна
    // здесь недостижимый ARP означал бы вечное зависание шелла. 0 = не ждём.
    uint64_t ping_arp_deadline_ms;

    // --- Диагностика (netstat) ---
    uint32_t rx_irq_wakeups;
};

static NetIfaceState g_iface[IFACE_COUNT];

// Провязка "начальных" значений, которые у голых глобалов раньше были
// нетривиальными дефолтами (не просто 0) — вызывается один раз из main()
// для каждого элемента g_iface[] до первого реального использования.
static void net_iface_init_defaults(NetIface iface) {
    NetIfaceState &s = g_iface[iface];
    s.subnet_mask[0] = 255; s.subnet_mask[1] = 255; s.subnet_mask[2] = 255; s.subnet_mask[3] = 0;
    s.dns_ip[0] = 8; s.dns_ip[1] = 8; s.dns_ip[2] = 8; s.dns_ip[3] = 8;
    s.dhcp_state = DHCP_IDLE;
    s.dns_id = 0xDEAF;
}

// Фаза 4.5.6: per-iface копии readiness-флага/DNS-IP/ping-статистики в общей
// SHM — GENET остаётся на историческом месте (4060-4095, обратная
// совместимость), Wi-Fi получает отдельный диапазон дальше (4100-8191 между
// net_vfs_lock на 4096 и WIFI_SHM_* на 8192 — свободно, с большим запасом).
// Числа ДОЛЖНЫ совпадать с одноимёнными функциями в shell.cpp.
// ВАЖНО: SHM в дочерних процессах (net_driver, shell, wifi_driver) мапится
// через map_frame_robust() с VMAttributes=0 — Device-память (некэшируемая,
// ради когерентности с GENET DMA; см. main.cpp), а НЕ Normal cacheable, как
// у самого rootserver'а. На Device-памяти ARM требует строгого выравнивания
// ЛЮБОГО доступа — 8-байтовая запись (dns-ip хранится как seL4_Word,
// 8 байт на aarch64!) по невыровненному на 8 байт адресу ловит Alignment
// Fault (живой краш на железе: 4204 % 8 == 4, а не 0 — FATAL FAULT PID 4,
// PC внутри net_poll(), Mem Addr ровно g_shm_vaddr+4204). GENET-диапазон
// (4064) исторически выровнен по 8 случайно — Wi-Fi-диапазон исправлен явно.
static inline uint32_t net_mailbox_ready_offset(NetIface iface) { return (iface == IFACE_WIFI) ? 4200 : 4060; }
static inline uint32_t net_mailbox_dns_ip_offset(NetIface iface) { return (iface == IFACE_WIFI) ? 4208 : 4064; } // 8-byte aligned — обязательно (seL4_Word*)
static inline uint32_t net_mailbox_ping_stats_offset(NetIface iface) { return (iface == IFACE_WIFI) ? 4216 : 4068; }

// По просьбе пользователя: shell должен сам выбирать интерфейс по умолчанию
// (GENET, если у него реально есть IP; иначе Wi-Fi, если есть у него; иначе
// сразу ошибка "нет сети", без обращения к net_driver вообще) — без флага
// `-W` дожидаться таймаута DHCP на мёртвом интерфейсе не нужно. shell не
// может сам знать dhcp_bound (это внутреннее состояние net_driver), поэтому
// net_driver публикует его в SHM каждый тик — обычный uint32_t (4-byte
// aligned офсеты, никакого seL4_Word — см. предупреждение выше про
// Device-память). Числа ДОЛЖНЫ совпадать с shell.cpp.
static inline uint32_t net_ready_flag_offset(NetIface iface) { return (iface == IFACE_WIFI) ? 4248 : 4244; }
static void net_publish_iface_ready(NetIface iface) {
    if (g_shm_vaddr == nullptr) return;
    *(volatile uint32_t*)(g_shm_vaddr + net_ready_flag_offset(iface)) = g_iface[iface].dhcp_bound ? 1u : 0u;
}

static bool ip_is_onlink(NetIface iface, const uint8_t ip[4]) {
    const NetIfaceState &s = g_iface[iface];
    for (int i = 0; i < 4; i++) {
        if ((ip[i] & s.subnet_mask[i]) != (s.ip[i] & s.subnet_mask[i])) return false;
    }
    return true;
}

// Возвращает MAC, на который реально слать кадр для dst_ip: если адресат в
// нашей же подсети — его собственный (резолвленный отдельным ARP, см.
// g_onlink_ip/g_onlink_mac), иначе — MAC шлюза как next-hop (обычная
// маршрутизация). false — нужного MAC ещё нет, кто-то должен инициировать
// его резолв (см. net_send_arp_request/net_poll ниже).
static bool resolve_dest_mac(NetIface iface, const uint8_t dst_ip[4], uint8_t out_mac[6]) {
    NetIfaceState &s = g_iface[iface];
    // Сам шлюз — ВСЕГДА через router_mac, даже если его адрес физически
    // попадает в нашу подсеть (стандартная ситуация: gateway почти всегда
    // внутри собственной объявленной подсети). Проверяем это раньше общего
    // ip_is_onlink(), иначе резолвился бы MAC шлюза, а искали бы его потом в
    // onlink_mac — рассинхрон, из-за которого `resolve` на DNS-сервер,
    // совпадающий с шлюзом (частый случай), никогда не находил уже
    // резолвленный MAC и вис навсегда (см. отчёт tcpdump — ARP на шлюз
    // получал ответ мгновенно, а DNS-запрос после этого так и не уходил).
    if (ip_eq(dst_ip, s.gateway_ip)) {
        if (!s.have_router_mac) return false;
        for (int i = 0; i < 6; i++) out_mac[i] = s.router_mac[i];
        return true;
    }
    if (ip_is_onlink(iface, dst_ip)) {
        if (!s.have_onlink_mac || !ip_eq(dst_ip, s.onlink_ip)) return false;
        for (int i = 0; i < 6; i++) out_mac[i] = s.onlink_mac[i];
        return true;
    }
    if (!s.have_router_mac) return false;
    for (int i = 0; i < 6; i++) out_mac[i] = s.router_mac[i];
    return true;
}

// IP, который нужно ARP-резолвить, чтобы получить MAC для dst_ip — сам
// dst_ip, если он в нашей подсети (кроме самого шлюза — см. resolve_dest_mac
// выше, для него отдельная ветка не нужна: gateway_ip и так возвращается),
// иначе — шлюз.
static void arp_target_for(NetIface iface, const uint8_t dst_ip[4], uint8_t out_target[4]) {
    const NetIfaceState &s = g_iface[iface];
    const uint8_t* src;
    if (ip_eq(dst_ip, s.gateway_ip)) src = s.gateway_ip;
    else if (ip_is_onlink(iface, dst_ip)) src = dst_ip;
    else src = s.gateway_ip;
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

// Счётчик пробуждений главного цикла по НЕ привязанному к интерфейсу биту
// NET_EVENT_HEARTBEAT (периодический 100мс тик, см. timer_driver.cpp) —
// печатается в `netstat`. Аналогичный счётчик ПО ИНТЕРФЕЙСУ (реальный IRQ/
// сигнал приёма — NET_EVENT_GENET_RX для GENET, NET_EVENT_WIFI_RX для
// Wi-Fi) — g_iface[iface].rx_irq_wakeups (см. NetIfaceState выше).
// Единственный практический способ подтвердить на живом железе, что RX
// действительно приходит по прерыванию, а не только "кажется рабочим" из-за
// heartbeat, который тоже дренирует кольцо (см. ROADMAP.md 4.5, живой баг
// ethertype=0/разрыв SHM).
static uint32_t g_heartbeat_wakeups = 0;

enum NetCommand {
    NET_CMD_NONE = 0,
    NET_CMD_PING = 1,
    NET_CMD_SEND = 2,
    NET_CMD_STATUS = 3,
    NET_CMD_RESOLVE = 4,
    NET_CMD_RECV = 5,
    NET_CMD_NTP = 6,
    NET_CMD_NTP_STATUS = 7,
    // Фаза 6.1 (продолжение, см. ROADMAP.md): "вызови у себя
    // seL4_BenchmarkResetLog() и ответь" — root не может включить учёт
    // benchmark utilisation на чужом ядре сам (per-core состояние в ядре),
    // просит net_driver сделать это самому, на своём текущем ядре.
    NET_CMD_BENCHMARK_RESET = 8,
    // Пара к NET_CMD_BENCHMARK_RESET выше — см. h/common.h/SYS_BENCHMARK_FINALIZE_LOCAL.
    NET_CMD_BENCHMARK_FINALIZE = 9,
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

// Capability до blk_driver (см. main.cpp: local_blk_ep=7, ipc->msg[7]) —
// раньше net_driver им не пользовался вообще, нужен только для журнала
// произвольных UDP-датаграмм ниже (net_log_udp) — LAN оказалась крайне
// разговорчивой (mDNS/NetBIOS-broadcast от соседей, десятки пакетов пачкой),
// и печать каждой в консоль через sys_puts (полноценный IPC + медленный UART)
// натурально тормозила главный цикл ровно тогда, когда мог прийти настоящий
// ответ на исходящий ping — прямое подозрение на часть "случайных" таймаутов.
static seL4_CPtr g_blk_ep = 0;
static seL4_CPtr g_vfs_mutex_ep = 0; // Фаза 6 (SMP): общий мьютекс на нотификации, см. main.cpp/vfs_mutex_ntfn
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

// Реальное время (мс, через timer_ep) отправки текущего echo request — раньше
// таймаут и RTT считались по числу итераций главного цикла ("~2 секунды =
// 100 000 циклов"), что было чистой оценкой на глаз и совершенно не отражало
// реальное время: цикл почти всегда пустой (пара MMIO-регистровых чтений) и
// крутится куда быстрее/медленнее, чем предполагал этот комментарий — отсюда
// таймаут на ping к хостам с честным ~65мс RTT (140.82.121.3) при том, что
// ~21мс (8.8.8.8) укладывался. Показанные раньше "21.195 ms" были не
// измерением, а произвольным loops*15 — теперь берём настоящее время.
static const uint64_t PING_TIMEOUT_MS = 2000;

// Фикс мейлбоксов->IPC (см. situation.txt): раньше NET_CMD_PING отвечал
// шеллу через SHM-мейлбокс (net_mailbox_ready_offset), который шелл сам
// поллил с собственным таймаутом (wait_for_net_mailbox). Теперь шелл шлёт
// ping через блокирующий seL4_Call, а net_driver откладывает reply тем же
// приёмом, что uart_driver.cpp (SYS_READ) и timer_driver.cpp (SYS_SLEEP_MS):
// seL4_CNode_SaveCaller при получении команды, seL4_Send+seL4_CNode_Delete,
// когда серия пингов реально завершена (см. net_schedule_next_ping). Два
// слота, не один — по одному на интерфейс, хотя одновременно активен только
// один (шелл однопоточный) — не завязываемся на это специально.
constexpr seL4_Word PING_REPLY_SLOT_GENET = 20;
constexpr seL4_Word PING_REPLY_SLOT_WIFI  = 21;
static inline seL4_Word ping_reply_slot_for(NetIface iface) {
    return (iface == IFACE_WIFI) ? PING_REPLY_SLOT_WIFI : PING_REPLY_SLOT_GENET;
}
// Общая часть отложенного ответа (см. выше) — публикация статистики/
// зануление мейлбокса делает вызывающий код ДО этого вызова, тут только
// сам IPC-реплай и освобождение слота.
static void net_ping_reply(NetIface iface) {
    seL4_Word slot = ping_reply_slot_for(iface);
    seL4_Send(slot, seL4_MessageInfo_new(0, 0, 0, 0));
    seL4_CNode_Delete(SELF_CNODE_SLOT, slot, 8);
}
// ARP на цель ping может никогда не резолвиться (недостижимый хост) — без
// явного дедлайна pending_cmd==NET_CMD_PING висел бы вечно, и вместе с ним
// блокирующий seL4_Call шелла (см. ping_arp_deadline_ms/NetIfaceState).
static const uint64_t PING_ARP_TIMEOUT_MS = 3000;

// Стандартный размер полезной нагрузки ICMP echo (как у обычного unix ping) —
// 8 (заголовок ICMP) + 56 = 64 байта самого ICMP-сообщения, что и печатается
// в "64 bytes from ..." — раньше было 4 байта ("PONG"), что честно работало,
// но выглядело не как настоящий ping.
static const int PING_PAYLOAD_LEN = 56;

// Целочисленный квадратный корень (метод Ньютона) — нужен только для mdev,
// плавающая точка/printf с дробными числами в этом окружении не заведены.
static uint64_t isqrt64(uint64_t n) {
    if (n == 0) return 0;
    uint64_t x = n, y = (x + 1) / 2;
    while (y < x) { x = y; y = (x + n / x) / 2; }
    return x;
}

// --- Константы DHCP-клиента (сами по себе не per-interface — состояние
// DHCP теперь в NetIfaceState/g_iface[], см. выше) ---
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

// Текущее состояние линка живёт в g_iface[IFACE_GENET].link_up (см.
// NetIfaceState выше) — для динамического обнаружения подключения/
// отключения кабеля после старта, см. net_check_link_status() ниже.
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
    g_iface[IFACE_GENET].link_up = link_up;
    g_link_next_check_uptime_ms = sys_get_uptime_ms(timer_ep) + LINK_CHECK_INTERVAL_MS;
    if (link_up) sys_puts(console_ep, "[NET] PHY link up.\n");
}

// Периодическая (не чаще LINK_CHECK_INTERVAL_MS) проверка линка через MDIO —
// динамическое обнаружение подключения/отключения кабеля после старта, а не
// только один раз при инициализации. Дёшево (один MDIO read раз в секунду),
// вызывается из главного цикла net_driver'а наравне с другими net_check_*.
// GENET-специфична (реальный MDIO read) — аналог для Wi-Fi это отдельная
// net_check_wifi_link() (см. Фазу 4.5.3), т.к. там источник состояния линка
// совсем другой (SHM-флаг от wifi_driver, не MDIO-регистр).
static void net_check_link_status(seL4_CPtr console_ep, seL4_CPtr timer_ep) {
    NetIfaceState &s = g_iface[IFACE_GENET];
    if (!g_net_up) return;
    uint64_t now = sys_get_uptime_ms(timer_ep);
    if (now < g_link_next_check_uptime_ms) return;
    g_link_next_check_uptime_ms = now + LINK_CHECK_INTERVAL_MS;

    uint16_t bmsr = 0;
    if (!genet_mdio_read(MII_BMSR, &bmsr)) return;
    bool link_up = (bmsr & MII_BMSR_LINK_UP) != 0;

    if (link_up && !s.link_up) {
        sys_puts(console_ep, "[NET] PHY link up (cable connected).\n");
        genet_apply_link(console_ep);
        // Старый MAC гейтвея мог устареть (другая сеть/роутер) — просим
        // разрешить заново при следующей реальной отправке. Новая сеть — новый
        // DHCP: net_check_dhcp() подхватит DHCP_IDLE на следующей итерации.
        s.have_router_mac = false;
        s.have_onlink_mac = false;
        s.dhcp_state = DHCP_IDLE;
        s.dhcp_bound = false;
        s.dhcp_attempt = 0; // новая сеть — обратный отсчёт backoff'а начинаем с нуля
    } else if (!link_up && s.link_up) {
        sys_puts(console_ep, "[NET] PHY link down (cable disconnected).\n");
        s.have_router_mac = false;
        s.have_onlink_mac = false;
        s.dhcp_state = DHCP_IDLE;
        s.dhcp_bound = false;
        s.dhcp_attempt = 0;
        // Адрес был выдан для уже отключённой сети — не используем его дальше.
        for (int i = 0; i < 4; i++) s.ip[i] = 0;
        for (int i = 0; i < 4; i++) s.gateway_ip[i] = 0;
    }
    s.link_up = link_up;
}

// Фаза 4.5.3: Wi-Fi-аналог net_check_link_status() выше — но источник
// состояния линка не MDIO-регистр, а SHM-мейлбокс, который пишет
// wifi_driver (успешный WIFI_CMD_CONNECT) или root (SYS_STOP_WIFI, гасит
// на случай ручной остановки/краша wifi_driver). Дёшево читать каждый тик
// (один volatile load), поэтому throttle как у GENET-варианта не нужен.
//
// НАЙДЕН И ПОЧИНЕН живой баг (см. situation.txt): LINK_STATE_OFFSET читался
// как противоположное значение без единого известного писателя — оказалось,
// весь Wi-Fi control-plane (SSID/пароль/verbose) исторически жил на
// 8192-8296, ровно ВНУТРИ зарезервированного staging-буфера blk_driver'а
// (SYS_WRITE_FILE зануляет 8192-12287 при КАЖДОЙ записи файла, включая
// автоматический net_log_udp() на любую входящую non-DNS/NTP/DHCP UDP-
// датаграмму — mDNS/SSDP/NetBIOS-шум от соседей по LAN, отсюда
// непериодичность). Весь Wi-Fi SHM (control+data-plane) перенесён на
// отдельную 5-ю страницу (h/platform.h) — GENET/VFS/blk_driver туда не
// дотягиваются. Дебаунс и канарейка ниже добавлены во время поиска этого
// бага — оставлены как дешёвая регресс-защита на случай повторения
// подобного столкновения в будущем.
static bool g_wifi_link_canary_init = false;
static bool g_wifi_link_pending_value = false;
static int  g_wifi_link_pending_ticks = 0;

static void net_check_wifi_link(seL4_CPtr console_ep) {
    if (g_shm_vaddr == nullptr) return;
    NetIfaceState &s = g_iface[IFACE_WIFI];

    if (!g_wifi_link_canary_init) {
        *(volatile uint32_t*)(g_shm_vaddr + WIFI_SHM_CANARY_OFFSET) = WIFI_SHM_CANARY_MAGIC;
        g_wifi_link_canary_init = true;
    } else {
        uint32_t canary = *(volatile uint32_t*)(g_shm_vaddr + WIFI_SHM_CANARY_OFFSET);
        if (canary != WIFI_SHM_CANARY_MAGIC) {
            sys_puts(console_ep, "[NET] ДИАГНОСТИКА: канарейка на 5-й странице изменилась! значение=");
            put_dec(console_ep, canary);
            sys_puts(console_ep, " — что-то ещё пишет в зону, зарезервированную под Wi-Fi.\n");
            *(volatile uint32_t*)(g_shm_vaddr + WIFI_SHM_CANARY_OFFSET) = WIFI_SHM_CANARY_MAGIC;
        }
    }

    bool link_up = (*(volatile uint32_t*)(g_shm_vaddr + WIFI_SHM_LINK_STATE_OFFSET)) != 0;

    if (link_up != s.link_up) {
        if (g_wifi_link_pending_ticks > 0 && g_wifi_link_pending_value == link_up) {
            g_wifi_link_pending_ticks++;
        } else {
            g_wifi_link_pending_value = link_up;
            g_wifi_link_pending_ticks = 1;
        }
        if (g_wifi_link_pending_ticks < 2) {
            sys_puts(console_ep, link_up ? "[NET] ДИАГНОСТИКА: Wi-Fi link=up на один тик, жду подтверждения...\n"
                                          : "[NET] ДИАГНОСТИКА: Wi-Fi link=down на один тик, жду подтверждения...\n");
            return; // не действуем, пока не подтвердится на следующем тике
        }
    } else {
        g_wifi_link_pending_ticks = 0;
    }

    if (link_up && !s.link_up) {
        sys_puts(console_ep, "[NET] Wi-Fi link up (associated), reason=");
        put_dec(console_ep, *(volatile uint32_t*)(g_shm_vaddr + WIFI_SHM_LINK_STATE_REASON_OFFSET));
        sys_puts(console_ep, ".\n");
        for (int i = 0; i < 6; i++) s.mac[i] = (uint8_t)g_shm_vaddr[WIFI_SHM_MAC_OFFSET + i];
        s.have_router_mac = false;
        s.have_onlink_mac = false;
        s.dhcp_state = DHCP_IDLE;
        s.dhcp_bound = false;
        s.dhcp_attempt = 0;
    } else if (!link_up && s.link_up) {
        // Диагностика живого бага (см. platform.h/situation.txt): reason
        // должен быть один из WIFI_LINK_REASON_* (STARTUP_RESET/CONNECT_FAIL/
        // SYS_STOP_WIFI), СВЕЖЕ записанный ИМЕННО этим переходом. Если тут
        // окажется 0 (WIFI_LINK_REASON_NONE) или явно устаревшее значение —
        // это прямое доказательство, что пишет НЕ один из 4 известных сайтов.
        sys_puts(console_ep, "[NET] Wi-Fi link down (disconnected), reason=");
        put_dec(console_ep, *(volatile uint32_t*)(g_shm_vaddr + WIFI_SHM_LINK_STATE_REASON_OFFSET));
        sys_puts(console_ep, ".\n");
        s.have_router_mac = false;
        s.have_onlink_mac = false;
        s.dhcp_state = DHCP_IDLE;
        s.dhcp_bound = false;
        s.dhcp_attempt = 0;
        for (int i = 0; i < 4; i++) s.ip[i] = 0;
        for (int i = 0; i < 4; i++) s.gateway_ip[i] = 0;
    }
    s.link_up = link_up;
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
    uint8_t *genet_mac = g_iface[IFACE_GENET].mac;
    genet_mac[0] = 0x02; genet_mac[1] = 0x50; genet_mac[2] = 0x57;
    genet_mac[3] = 0x4F; genet_mac[4] = 0x53; genet_mac[5] = 0x01;
    uint32_t mac0 = ((uint32_t)genet_mac[0] << 24) | ((uint32_t)genet_mac[1] << 16) |
                    ((uint32_t)genet_mac[2] << 8) | genet_mac[3];
    uint32_t mac1 = ((uint32_t)genet_mac[4] << 8) | genet_mac[5];
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
    // Если кабель ещё не воткнут — нечего резолвить (ANEG не проходил),
    // genet_apply_link() тут дал бы только гадательное "1000 (ANEG не
    // разрешился, догадка)". net_check_link_status() вызовет её по-настоящему,
    // когда линк реально появится (см. там же).
    if (g_iface[IFACE_GENET].link_up) genet_apply_link(console_ep); // RGMII OOB control + скорость + TX/RX enable (см. genet_apply_link())

    // Фаза 4.5 (см. ROADMAP.md) — реальный GIC IRQ на приём кадра вместо
    // безусловного опроса net_hw_poll_rx() на каждой итерации главного
    // цикла. Порядок — как в эталонном bcmgenet.c: замаскировать всё,
    // сбросить залежавшиеся pending-биты, потом размаскировать только то,
    // что нужно (см. platform.h/INTRL2_CPU_* — НЕ было в проекте раньше,
    // сверено с /home/nikita/kernel_xiaomi_vince/.../bcmgenet.h).
    genet_write32(INTRL2_CPU_MASK_SET, 0xFFFFFFFFu);
    genet_write32(INTRL2_CPU_CLEAR, 0xFFFFFFFFu);
    genet_write32(INTRL2_CPU_MASK_CLEAR, UMAC_IRQ_RXDMA_DONE);

    g_net_up = true;
    return true;
}

// Синхронная отправка одного кадра (без virtio_net_hdr — чистый Ethernet-кадр).
static bool genet_hw_send(const void* frame, uint32_t len) {
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
static bool genet_hw_poll_rx(uint8_t** out_frame, uint32_t* out_len) {
    uint32_t prod_index = genet_read32(GENET_RDMA_RING_REG_BASE + GENET_RDMA_PROD_INDEX_OFF);
    if ((prod_index & 0xFFFFu) == (g_rx_c_index & 0xFFFFu)) return false;

    uintptr_t desc = GENET_RX_OFF + g_rx_index * GENET_DMA_DESC_SIZE;
    uint32_t len_stat = genet_read32(desc + GENET_DMA_DESC_LENGTH_STATUS);
    uint32_t len = (len_stat >> GENET_DMA_BUFLENGTH_SHIFT) & 0x0FFFu;

    *out_frame = (uint8_t*)(g_shm_vaddr + rx_buffer_offsets[g_rx_index] + GENET_RX_BUF_OFFSET);
    *out_len = (len > GENET_RX_BUF_OFFSET) ? (len - GENET_RX_BUF_OFFSET) : 0;
    return true;
}

static void genet_hw_rx_done() {
    g_rx_c_index = (g_rx_c_index + 1) & 0xFFFFu;
    genet_write32(GENET_RDMA_RING_REG_BASE + GENET_RDMA_CONS_INDEX_OFF, g_rx_c_index);
    g_rx_index = (g_rx_index + 1) % GENET_RX_DESCS;
}

// --- Wi-Fi backend (Фаза 4.5.3+, см. situation.txt/план). RX (4.5.5) пока
// заглушка. Капа сигнала TX (см. BOOT_WIFI_TX_WAKE_CAP/main.cpp) читается
// один раз в main() ниже и живёт тут же — используется только внутри
// wifi_hw_send().
static seL4_CPtr g_wifi_tx_wake_ntfn = 0;

// Фаза 4.5.4 (TX-путь): single-producer(net_driver)/single-consumer
// (wifi_driver) mailbox в ТОЙ ЖЕ физической SHM, что и control-plane
// (WIFI_SHM_SSID_OFFSET и т.д.) — просто более высокие офсеты (см.
// platform.h). Блокировка не нужна (тот же довод, что у lock-free GENET
// RX-кольца): длина — последнее, что пишет producer, и первое, что читает
// consumer, поэтому используется как flag "занято/свободно". Если mailbox
// ещё не опустел (wifi_driver не успел забрать предыдущий кадр) — кадр
// дропается, не блокируем net_driver (та же семантика, что genet_hw_send()
// имел бы при переполнении TX-кольца, только тут кольцо глубиной 1).
static bool wifi_hw_send(const void* frame, uint32_t len) {
    if (g_shm_vaddr == nullptr || len == 0 || len > WIFI_SHM_FRAME_CAP) return false;
    if (*(volatile uint32_t*)(g_shm_vaddr + WIFI_SHM_TX_LEN_OFFSET) != 0) return false; // mailbox занят
    for (uint32_t i = 0; i < len; i++) g_shm_vaddr[WIFI_SHM_TX_DATA_OFFSET + i] = ((const char*)frame)[i];
    *(volatile uint32_t*)(g_shm_vaddr + WIFI_SHM_TX_LEN_OFFSET) = len;
    if (g_wifi_tx_wake_ntfn != 0) seL4_Signal(g_wifi_tx_wake_ntfn);
    return true;
}
// Фаза 4.5.5 (RX-путь): зеркало wifi_hw_send() выше — тот же mailbox
// глубиной 1, но в обратную сторону (пишет wifi_driver, читает net_driver).
// *out_frame указывает ПРЯМО в SHM (WIFI_SHM_RX_DATA_OFFSET) — протокольный
// слой (net_poll) обрабатывает кадр НА МЕСТЕ, как и с GENET RX-кольцом, без
// лишнего копирования; mailbox освобождается для wifi_driver только в
// wifi_hw_rx_done() (см. ниже), после того как net_poll закончил с кадром.
static bool wifi_hw_poll_rx(uint8_t** out_frame, uint32_t* out_len) {
    if (g_shm_vaddr == nullptr) return false;
    uint32_t len = *(volatile uint32_t*)(g_shm_vaddr + WIFI_SHM_RX_LEN_OFFSET);
    if (len == 0 || len > WIFI_SHM_FRAME_CAP) return false;
    *out_frame = (uint8_t*)(g_shm_vaddr + WIFI_SHM_RX_DATA_OFFSET);
    *out_len = len;
    return true;
}
static void wifi_hw_rx_done() {
    *(volatile uint32_t*)(g_shm_vaddr + WIFI_SHM_RX_LEN_OFFSET) = 0;
}

// --- Диспетчер аппаратного уровня по интерфейсу (Фаза 4.5, см. NetIfaceState
// выше) — единственное место, где протокольный слой (ARP/DHCP/ping/DNS/NTP/
// net_poll) соприкасается с конкретным железом. GENET и Wi-Fi ниже этой
// границы совершенно не похожи друг на друга (DMA-кольца регистров против
// SHM-мейлбокса до отдельного процесса) — протокольный слой видит только
// (iface, frame, len), как и раньше видел только (frame, len) для GENET.
static bool net_hw_send(NetIface iface, const void* frame, uint32_t len) {
    return (iface == IFACE_GENET) ? genet_hw_send(frame, len) : wifi_hw_send(frame, len);
}
static bool net_hw_poll_rx(NetIface iface, uint8_t** out_frame, uint32_t* out_len) {
    return (iface == IFACE_GENET) ? genet_hw_poll_rx(out_frame, out_len) : wifi_hw_poll_rx(out_frame, out_len);
}
static void net_hw_rx_done(NetIface iface) {
    if (iface == IFACE_GENET) genet_hw_rx_done(); else wifi_hw_rx_done();
}

static void net_send_packet(NetIface iface, uint32_t total_len, uint32_t tx_offset = 0x280) {
    net_hw_send(iface, g_shm_vaddr + tx_offset, total_len);
}

// Резолвит MAC для произвольного target_ip — это либо шлюз (адресат вне
// нашей подсети, next-hop как обычно), либо сам адресат, если он в нашей
// подсети (см. arp_target_for()/resolve_dest_mac() выше) — иначе многие
// роутеры не отражают такой unicast-кадр обратно в LAN (anti-spoofing/
// split-horizon), и пакет до соседа по сети просто не доходил. До получения
// адреса по DHCP вызывающий код обязан сперва дождаться g_dhcp_bound (см.
// net_require_ip), иначе это ARP в никуда.
static void net_send_arp_request(NetIface iface, seL4_CPtr root_ep, const uint8_t target_ip[4]) {
    NetIfaceState &s = g_iface[iface];
    volatile ethernet_frame* eth = (volatile ethernet_frame*)(g_shm_vaddr + 0x280);

    for(int i=0; i<6; i++) eth->dest_mac[i] = 0xFF;
    for(int i=0; i<6; i++) eth->src_mac[i] = s.mac[i];
    eth->ethertype = htons(0x0806);

    volatile arp_ipv4* arp = (volatile arp_ipv4*)eth->payload;
    arp->htype = htons(1); arp->ptype = htons(0x0800); arp->hlen = 6; arp->plen = 4; arp->oper = htons(1);
    for(int i=0; i<6; i++) arp->sha[i] = s.mac[i];
    for(int i=0; i<4; i++) arp->spa[i] = s.ip[i];
    for(int i=0; i<6; i++) arp->tha[i] = 0;
    for(int i=0; i<4; i++) arp->tpa[i] = target_ip[i];

    sys_puts(root_ep, "\n[NET] Broadcasting ARP Request for ");
    put_ip(root_ep, target_ip);
    sys_puts(root_ep, "...\n");
    net_send_packet(iface, 14 + 28, 0x280);
}

// Отвечает на входящий ARP-запрос "who-has my_ip" — БЕЗ этого любой сосед
// (в первую очередь сам роутер) рано или поздно старит свою ARP-запись про
// нас и перестаёт быть способен нам что-либо доставить: он честно шлёт
// "who-has 192.168.2.206" (обычно 3 раза, раз в секунду), не получает ответа
// и считает нас недостижимыми — именно так объясняется весь ранее
// наблюдавшийся паттерн "первые пинги проходят, потом внезапно перестают" —
// подтверждено tcpdump'ом на роутере: он трижды спрашивал наш MAC, мы молчали,
// и следующий же ICMP-ответ от 8.8.8.8 роутеру уже некуда было доставить.
static void net_send_arp_reply(NetIface iface, seL4_CPtr console_ep, const uint8_t target_ip[4], const uint8_t target_mac[6]) {
    NetIfaceState &s = g_iface[iface];
    volatile ethernet_frame* eth = (volatile ethernet_frame*)(g_shm_vaddr + 0x280);

    for(int i=0; i<6; i++) eth->dest_mac[i] = target_mac[i];
    for(int i=0; i<6; i++) eth->src_mac[i] = s.mac[i];
    eth->ethertype = htons(0x0806);

    volatile arp_ipv4* arp = (volatile arp_ipv4*)eth->payload;
    arp->htype = htons(1); arp->ptype = htons(0x0800); arp->hlen = 6; arp->plen = 4; arp->oper = htons(2);
    for(int i=0; i<6; i++) arp->sha[i] = s.mac[i];
    for(int i=0; i<4; i++) arp->spa[i] = s.ip[i];
    for(int i=0; i<6; i++) arp->tha[i] = target_mac[i];
    for(int i=0; i<4; i++) arp->tpa[i] = target_ip[i];

    net_send_packet(iface, 14 + 28, 0x280);
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

// msg_type: 1=DHCPDISCOVER, 3=DHCPREQUEST. Для обычного REQUEST (SELECTING,
// после OFFER) использует dhcp_offered_ip/dhcp_server_ip + широковещательно,
// ciaddr=0 — как раньше. renew=true — RFC 2131 RENEWING: UNICAST прямо на
// dhcp_server_ip, ciaddr=текущий s.ip (это САМ по себе несёт "какой адрес
// продлеваем", поэтому БЕЗ option 50/54 — see RFC 2131 table 5), flags=0
// (unicast-ответ нам уже доступен, IP-то рабочий). Раньше net_check_dhcp()
// на дедлайне T1 просто бросал рабочий IP и слал DISCOVER с нуля — часть
// DHCP-серверов (включая dnsmasq/OpenWrt) не всегда охотно переотвечают на
// повторный DISCOVER от MAC, для которого у них уже есть активная аренда,
// из-за чего клиент завис в вечном "no response, retrying" (живой баг,
// см. situation.txt) — настоящий unicast-RENEW почти всегда чинит это.
static void net_send_dhcp_packet(NetIface iface, seL4_CPtr console_ep, uint8_t msg_type, bool renew = false) {
    NetIfaceState &s = g_iface[iface];
    uint32_t tx_offset = 0x280;
    volatile ethernet_frame* eth = (volatile ethernet_frame*)(g_shm_vaddr + tx_offset);

    if (renew) {
        uint8_t dest_mac[6];
        if (!resolve_dest_mac(iface, s.dhcp_server_ip, dest_mac)) {
            // MAC сервера не резолвлен (нетипичный случай — обычно он же
            // гейтвей, уже резолвленный) — не ждать ARP ради необязательного
            // renew, вызывающий код (net_check_dhcp) сам скатится к полному
            // re-discover по таймауту, как и раньше.
            sys_puts(console_ep, "[NET] DHCP: renew skipped (server MAC unknown), will fall back to discovery.\n");
            return;
        }
        for (int i = 0; i < 6; i++) eth->dest_mac[i] = dest_mac[i];
    } else {
        for (int i = 0; i < 6; i++) eth->dest_mac[i] = 0xFF; // DHCP широковещательно, MAC сервера ещё не резолвим
    }
    for (int i = 0; i < 6; i++) eth->src_mac[i] = s.mac[i];
    eth->ethertype = htons(0x0800);

    volatile ipv4_header* ip = (volatile ipv4_header*)eth->payload;
    volatile udp_header* udp = (volatile udp_header*)(eth->payload + sizeof(ipv4_header));
    volatile dhcp_packet* dhcp = (volatile dhcp_packet*)((char*)udp + sizeof(udp_header));

    for (uint32_t i = 0; i < sizeof(dhcp_packet); i++) ((volatile uint8_t*)dhcp)[i] = 0;
    dhcp->op = 1; dhcp->htype = 1; dhcp->hlen = 6; dhcp->hops = 0;
    dhcp->xid = bswap32(s.dhcp_xid);
    dhcp->secs = 0;
    dhcp->flags = renew ? 0 : htons(0x8000); // RENEWING шлёт unicast и ждёт unicast-ответ — broadcast-бит не нужен
    if (renew) for (int i = 0; i < 4; i++) dhcp->ciaddr[i] = s.ip[i]; // RFC 2131: адрес, который продлеваем
    for (int i = 0; i < 6; i++) dhcp->chaddr[i] = s.mac[i];
    dhcp->magic_cookie = bswap32(DHCP_MAGIC_COOKIE);

    uint8_t* opt = (uint8_t*)dhcp->options;
    int pos = 0;
    uint8_t type_byte = msg_type;
    pos += dhcp_put_option(opt + pos, 53, 1, &type_byte); // DHCP Message Type
    pos += dhcp_put_option(opt + pos, 12, sizeof(DHCP_HOSTNAME) - 1, (const uint8_t*)DHCP_HOSTNAME); // Host Name
    if (msg_type == 3 && !renew) { // DHCPREQUEST (SELECTING) — подтверждаем конкретное предложение
        pos += dhcp_put_option(opt + pos, 50, 4, s.dhcp_offered_ip); // Requested IP Address
        pos += dhcp_put_option(opt + pos, 54, 4, s.dhcp_server_ip);  // Server Identifier
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
    if (renew) {
        for (int i = 0; i < 4; i++) ip->saddr[i] = s.ip[i];
        for (int i = 0; i < 4; i++) ip->daddr[i] = s.dhcp_server_ip[i];
    } else {
        ip->saddr[0] = 0; ip->saddr[1] = 0; ip->saddr[2] = 0; ip->saddr[3] = 0; // RFC 2131: до ACK клиент использует 0.0.0.0
        ip->daddr[0] = 255; ip->daddr[1] = 255; ip->daddr[2] = 255; ip->daddr[3] = 255;
    }
    ip->check = 0;
    ip->check = calculate_checksum((void*)ip, sizeof(ipv4_header));

    sys_puts(console_ep, renew ? "[NET] DHCP: sending unicast RENEW...\n" :
                          msg_type == 1 ? "[NET] DHCP: sending DISCOVER...\n" : "[NET] DHCP: sending REQUEST...\n");
    net_send_packet(iface, 14 + sizeof(ipv4_header) + sizeof(udp_header) + dhcp_len, tx_offset);
}

// Запускает/повторяет получение адреса — вызывается раз за итерацию главного
// цикла (см. main()), сама решает, нужно ли что-то слать. Не делает ничего,
// пока нет линка (net_check_link_status/net_check_wifi_link сбрасывают
// состояние в DHCP_IDLE при отключении — см. там же).
static void net_check_dhcp(NetIface iface, seL4_CPtr console_ep, seL4_CPtr timer_ep) {
    NetIfaceState &s = g_iface[iface];
    if (!s.link_up) return;
    uint64_t now = sys_get_uptime_ms(timer_ep);

    if (s.dhcp_state == DHCP_IDLE) {
        s.dhcp_xid = (uint32_t)now ^ 0xA5A5A5A5u; // без аппаратного RNG — сойдёт и так, лишь бы не 0
        sys_puts(console_ep, "[NET] Starting DHCP discovery...\n");
        net_send_dhcp_packet(iface, console_ep, 1 /* DHCPDISCOVER */);
        s.dhcp_state = DHCP_DISCOVERING;
        // Backoff: 4с, 8с, 16с, 32с, дальше потолок 60с — если сервера в сети
        // просто нет, не заваливаем консоль строкой каждые 4 секунды вечно
        // (мешает вводу команд в shell).
        uint32_t shift = (s.dhcp_attempt < 5) ? s.dhcp_attempt : 5;
        uint64_t interval = DHCP_RETRY_MS << shift;
        if (interval > DHCP_RETRY_MAX_MS) interval = DHCP_RETRY_MAX_MS;
        s.dhcp_retry_uptime_ms = now + interval;
        s.dhcp_attempt++;
    } else if (s.dhcp_state == DHCP_DISCOVERING || s.dhcp_state == DHCP_REQUESTING) {
        if (now >= s.dhcp_retry_uptime_ms) {
            sys_puts(console_ep, "[NET] DHCP: no response, retrying...\n");
            s.dhcp_state = DHCP_IDLE; // следующая итерация начнёт DISCOVER заново
        }
    } else if (s.dhcp_state == DHCP_RENEWING) {
        // Unicast RENEW не ответил вовремя — сервер мог быть недоступен
        // (не только истинный отказ, за который отвечал бы явный DHCPNAK,
        // см. net_poll()). Не держимся за renew бесконечно — один короткий
        // повтор, потом честный полный re-discover (см. DHCP_IDLE выше).
        // IP уже брошен здесь же (см. переход в DHCP_BOUND-ветке ниже) —
        // именно поэтому это НЕ регрессия к старому "мгновенно теряем IP"
        // поведению: renew либо тихо продлевает адрес без единой потери
        // связности, либо (редко) откатывается на полный re-discover ровно
        // так же, как раньше.
        if (now >= s.dhcp_retry_uptime_ms) {
            sys_puts(console_ep, "[NET] DHCP: renew got no response, falling back to full discovery.\n");
            s.dhcp_state = DHCP_IDLE;
            s.dhcp_bound = false;
        }
    } else if (s.dhcp_state == DHCP_BOUND) {
        if (s.dhcp_lease_deadline_uptime_ms != 0 && now >= s.dhcp_lease_deadline_uptime_ms) {
            sys_puts(console_ep, "[NET] DHCP: lease renewal due, sending unicast renew...\n");
            s.dhcp_xid = (uint32_t)now ^ 0x5A5A5A5Au; // свежий xid — иначе сервер может принять его за ретрансмит старого REQUEST
            net_send_dhcp_packet(iface, console_ep, 3 /* DHCPREQUEST */, true /* renew */);
            s.dhcp_state = DHCP_RENEWING;
            s.dhcp_retry_uptime_ms = now + DHCP_RETRY_MS; // один короткий таймаут на unicast-ответ, см. ветку выше
        }
    }
}

static void net_send_ping(NetIface iface, seL4_CPtr console_ep, seL4_CPtr timer_ep, const uint8_t dst_ip[4]) {
    NetIfaceState &s = g_iface[iface];
    uint16_t seq = ++s.ping_next_seq;
    if (seq == 0) seq = ++s.ping_next_seq;
    volatile ethernet_frame* eth = (volatile ethernet_frame*)(g_shm_vaddr + 0x280);

    uint8_t dest_mac[6];
    resolve_dest_mac(iface, dst_ip, dest_mac); // вызывающий код уже убедился, что резолв готов
    for(int i=0; i<6; i++) eth->dest_mac[i] = dest_mac[i];
    for(int i=0; i<6; i++) eth->src_mac[i] = s.mac[i];
    eth->ethertype = htons(0x0800);

    volatile ipv4_header* ip = (volatile ipv4_header*)eth->payload;
    ip->ihl_version = 0x45; ip->tos = 0; ip->tot_len = htons(sizeof(ipv4_header) + sizeof(icmp_header) + PING_PAYLOAD_LEN);
    ip->id = htons(0x1234); ip->frag_off = 0; ip->ttl = 64; ip->protocol = 1; ip->check = 0;
    for (int i = 0; i < 4; i++) ip->saddr[i] = s.ip[i];
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

    s.ping_sent_ms = sys_get_uptime_ms(timer_ep); // грубый таймаут (2с) — этого разрешения достаточно
    if (g_cntfrq == 0) g_cntfrq = read_cntfrq();
    s.ping_sent_cyc = read_cntvct(); // точный RTT — см. read_cntvct() выше
    s.ping_outstanding_seq = seq;
    s.ping_outstanding = true;
    s.ping_sent_count++;
    s.ping_series_sent++;

    net_send_packet(iface, 14 + sizeof(ipv4_header) + sizeof(icmp_header) + PING_PAYLOAD_LEN, 0x280);
}

// Смещения в SHM для передачи shell'у статистики завершённой серии ping —
// той же по духу приём, что и резолвленный DNS IP на +4064 (см. net_poll):
// пишем перед тем, как разблокировать mailbox, shell читает сразу после и
// печатает "--- ... ping statistics ---" в стиле обычного unix ping.
// Каждое поле — uint32, все 7 умещаются перед концом первой страницы SHM.
static void net_publish_ping_stats(NetIface iface, seL4_CPtr timer_ep) {
    NetIfaceState &s = g_iface[iface];
    uint32_t avg_us = s.ping_series_reply > 0 ? (uint32_t)(s.ping_series_total_rtt_us / s.ping_series_reply) : 0;
    uint64_t variance = 0;
    if (s.ping_series_reply > 0) {
        uint64_t mean_sq = (uint64_t)avg_us * (uint64_t)avg_us;
        uint64_t sq_avg = s.ping_series_total_rtt_sq_us / s.ping_series_reply;
        variance = (sq_avg > mean_sq) ? (sq_avg - mean_sq) : 0; // integer truncation могла бы дать чуть отрицательное
    }
    uint32_t mdev_us = (uint32_t)isqrt64(variance);
    uint32_t elapsed_ms = (uint32_t)(sys_get_uptime_ms(timer_ep) - s.ping_series_start_ms);

    // Фаза 4.5.6: отдельный диапазон офсетов на интерфейс (см.
    // net_mailbox_ping_stats_offset()) — GENET и Wi-Fi больше не делят один
    // и тот же блок статистики.
    uint32_t stats_off = net_mailbox_ping_stats_offset(iface);
    *(uint32_t*)(g_shm_vaddr + stats_off + 0)  = s.ping_series_sent;
    *(uint32_t*)(g_shm_vaddr + stats_off + 4)  = s.ping_series_reply;
    *(uint32_t*)(g_shm_vaddr + stats_off + 8)  = (uint32_t)s.ping_series_min_rtt_us;
    *(uint32_t*)(g_shm_vaddr + stats_off + 12) = (uint32_t)s.ping_series_max_rtt_us;
    *(uint32_t*)(g_shm_vaddr + stats_off + 16) = avg_us;
    *(uint32_t*)(g_shm_vaddr + stats_off + 20) = mdev_us;
    *(uint32_t*)(g_shm_vaddr + stats_off + 24) = elapsed_ms;
}

static void net_schedule_next_ping(NetIface iface, seL4_CPtr timer_ep) {
    NetIfaceState &s = g_iface[iface];
    if (s.ping_series_remaining > 0) {
        s.ping_next_send_ms = sys_get_time_ms(timer_ep) + 1000; // Пауза ровно 1 секунда через RTC!
    } else {
        net_publish_ping_stats(iface, timer_ep);
        volatile int* net_mailbox = (volatile int*)(g_shm_vaddr + net_mailbox_ready_offset(iface));
        net_mailbox[0] = 0; // легаси-мейлбокс, больше никто не поллит, оставлен безвредным
        net_ping_reply(iface); // фикс мейлбоксов->IPC — настоящий разблокирующий reply шеллу
    }
}

static void net_send_next_ping(NetIface iface, seL4_CPtr console_ep, seL4_CPtr timer_ep) {
    NetIfaceState &s = g_iface[iface];
    if (s.ping_outstanding || s.ping_series_remaining == 0) return;
    s.ping_series_remaining--;
    net_send_ping(iface, console_ep, timer_ep, s.ping_target_ip);
}

static void net_check_ping_send(NetIface iface, seL4_CPtr console_ep, seL4_CPtr timer_ep) {
    NetIfaceState &s = g_iface[iface];
    if (s.ping_outstanding || s.ping_series_remaining == 0) return;
    if (s.ping_next_send_ms == 0 || sys_get_time_ms(timer_ep) >= s.ping_next_send_ms) {
        s.ping_next_send_ms = 0;
        net_send_next_ping(iface, console_ep, timer_ep);
    }
}

static void net_start_ping_series(NetIface iface, seL4_CPtr console_ep, seL4_CPtr timer_ep, const uint8_t dst_ip[4], uint32_t count) {
    NetIfaceState &s = g_iface[iface];
    if (count == 0) count = 1; if (count > 16) count = 16;
    for (int i = 0; i < 4; i++) s.ping_target_ip[i] = dst_ip[i];
    s.ping_series_remaining = count; s.ping_outstanding = false; s.ping_next_send_ms = 0;
    s.ping_series_sent = 0; s.ping_series_reply = 0;
    s.ping_series_min_rtt_us = 0; s.ping_series_max_rtt_us = 0;
    s.ping_series_total_rtt_us = 0; s.ping_series_total_rtt_sq_us = 0;
    s.ping_series_start_ms = sys_get_uptime_ms(timer_ep);
    net_check_ping_send(iface, console_ep, timer_ep); // Шлем первый пакет сразу
}

static void net_record_ping_rtt(NetIface iface, uint64_t rtt_us) {
    NetIfaceState &s = g_iface[iface];
    s.ping_reply_count++; s.ping_last_rtt_us = rtt_us; s.ping_total_rtt_us += rtt_us;
    if (s.ping_min_rtt_us == 0 || rtt_us < s.ping_min_rtt_us) s.ping_min_rtt_us = rtt_us;
    if (rtt_us > s.ping_max_rtt_us) s.ping_max_rtt_us = rtt_us;

    s.ping_series_reply++;
    s.ping_series_total_rtt_us += rtt_us;
    s.ping_series_total_rtt_sq_us += rtt_us * rtt_us;
    if (s.ping_series_min_rtt_us == 0 || rtt_us < s.ping_series_min_rtt_us) s.ping_series_min_rtt_us = rtt_us;
    if (rtt_us > s.ping_series_max_rtt_us) s.ping_series_max_rtt_us = rtt_us;
}

static void net_check_ping_timeout(NetIface iface, seL4_CPtr console_ep, seL4_CPtr timer_ep) {
    NetIfaceState &s = g_iface[iface];
    if (!s.ping_outstanding) return;

    if (sys_get_uptime_ms(timer_ep) - s.ping_sent_ms < PING_TIMEOUT_MS) return;

    // Молчим по каждому потерянному пакету — как обычный unix ping, который
    // тоже ничего не печатает построчно на таймаут, только в итоговом
    // packet loss% (см. net_publish_ping_stats). ping_timeout_count всё
    // равно считается — используется в netstat.
    s.ping_outstanding = false; s.ping_timeout_count++;
    net_schedule_next_ping(iface, timer_ep);
}

// Фикс мейлбоксов->IPC (см. situation.txt/PING_ARP_TIMEOUT_MS выше) —
// недостижимый хост никогда не ответит на ARP, и без этой проверки
// pending_cmd==NET_CMD_PING (и вместе с ним заблокированный seL4_Call
// шелла) висел бы вечно.
static void net_check_ping_arp_timeout(NetIface iface, seL4_CPtr timer_ep) {
    NetIfaceState &s = g_iface[iface];
    if (s.pending_cmd != NET_CMD_PING || s.ping_arp_deadline_ms == 0) return;
    if (sys_get_uptime_ms(timer_ep) < s.ping_arp_deadline_ms) return;

    s.pending_cmd = NET_CMD_NONE;
    s.ping_arp_deadline_ms = 0;
    uint32_t stats_off = net_mailbox_ping_stats_offset(iface);
    for (int i = 0; i < 7; i++) *(uint32_t*)(g_shm_vaddr + stats_off + i * 4) = 0;
    volatile int* net_mailbox = (volatile int*)(g_shm_vaddr + net_mailbox_ready_offset(iface));
    net_mailbox[0] = 0;
    net_ping_reply(iface);
}

static void net_send_udp(NetIface iface, seL4_CPtr console_ep, const uint8_t dst_ip[4], uint16_t dst_port, const char* message) {
    NetIfaceState &s = g_iface[iface];
    volatile ethernet_frame* eth = (volatile ethernet_frame*)(g_shm_vaddr + 0x280);
    uint8_t dest_mac[6];
    resolve_dest_mac(iface, dst_ip, dest_mac); // вызывающий код уже убедился, что резолв готов
    for(int i=0; i<6; i++) eth->dest_mac[i] = dest_mac[i];
    for(int i=0; i<6; i++) eth->src_mac[i] = s.mac[i];
    eth->ethertype = htons(0x0800);

    int msg_len = my_strlen(message);

    // IP Заголовок (Протокол 17 = UDP)
    volatile ipv4_header* ip = (volatile ipv4_header*)eth->payload;
    ip->ihl_version = 0x45; ip->tos = 0;
    ip->tot_len = htons(sizeof(ipv4_header) + sizeof(udp_header) + msg_len);
    ip->id = htons(0x7777); ip->frag_off = 0; ip->ttl = 64;
    ip->protocol = 17; // 17 = UDP
    ip->check = 0;
    for (int i = 0; i < 4; i++) ip->saddr[i] = s.ip[i];
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
    net_send_packet(iface, 14 + sizeof(ipv4_header) + sizeof(udp_header) + my_strlen(message), 0x280);
    volatile int* net_mailbox = (volatile int*)(g_shm_vaddr + net_mailbox_ready_offset(iface));
    net_mailbox[0] = 0;
}

// Отправляет SNTP client-запрос (mode=3) напрямую на g_ntp_server_ip:123,
// маршрутизируя через уже разрешенный router_mac (тот же прием, что и DNS-запрос
// на 8.8.8.8 выше — адресат вне локальной подсети, но канальный уровень идет на гейтвей).
static void net_send_ntp_request(NetIface iface, seL4_CPtr console_ep, seL4_CPtr timer_ep) {
    NetIfaceState &s = g_iface[iface];
    uint32_t tx_offset = 0x280;

    volatile ethernet_frame* eth = (volatile ethernet_frame*)(g_shm_vaddr + tx_offset);
    uint8_t dest_mac[6];
    resolve_dest_mac(iface, g_ntp_server_ip, dest_mac); // вызывающий код уже убедился, что резолв готов
    for (int i = 0; i < 6; i++) eth->dest_mac[i] = dest_mac[i];
    for (int i = 0; i < 6; i++) eth->src_mac[i] = s.mac[i];
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
    // офсета ((T2-T1)+(T3-T4))/2 ниже, при получении ответа. ИСПРАВЛЕНО (см.
    // situation.txt): T1 должен быть RAW uptime (sys_get_uptime_ms), а НЕ уже
    // скорректированное время (sys_get_time_ms) — иначе на ПЕРИОДИЧЕСКОЙ
    // ресинхронизации T1 уже включает предыдущую NTP-поправку, формула
    // считает лишь маленькую дельту дрейфа (например -1с) вместо полного
    // офсета, а SYS_SET_TIME_OFFSET ЗАМЕНЯЕТ (не складывает) сохранённое
    // смещение — обнуляя предыдущую поправку и отбрасывая часы к 1970 году.
    // Поле ntp->tx_ts_sec, отправляемое СЕРВЕРУ, — отдельно, ему нужен
    // best-effort реальный эпох (не участвует в нашей же формуле офсета).
    uint64_t our_epoch_s = sys_get_time_ms(timer_ep) / 1000ULL;
    s.ntp_t1_epoch_s = sys_get_uptime_ms(timer_ep) / 1000ULL;
    ntp->tx_ts_sec = bswap32((uint32_t)(our_epoch_s + NTP_UNIX_EPOCH_DELTA));
    ntp->tx_ts_frac = 0;

    volatile ipv4_header* ip = (volatile ipv4_header*)eth->payload;
    ip->ihl_version = 0x45; ip->tos = 0;
    ip->tot_len = htons(sizeof(ipv4_header) + sizeof(udp_header) + sizeof(ntp_packet));
    ip->id = htons(0x4E54); ip->frag_off = 0; ip->ttl = 64; ip->protocol = 17;
    for (int i = 0; i < 4; i++) ip->saddr[i] = s.ip[i];
    for (int i = 0; i < 4; i++) ip->daddr[i] = g_ntp_server_ip[i];
    ip->check = 0;
    ip->check = calculate_checksum((void*)ip, sizeof(ipv4_header));

    if (!s.ntp_is_periodic) {
        sys_puts(console_ep, "[NET] Sending NTP request to ");
        put_ip(console_ep, g_ntp_server_ip);
        sys_puts(console_ep, ":123...\n");
    }

    net_send_packet(iface, 14 + sizeof(ipv4_header) + sizeof(udp_header) + sizeof(ntp_packet), tx_offset);
    s.ntp_outstanding = true;
}

// Ставит в план следующую автоматическую ресинхронизацию через NTP_RESYNC_INTERVAL_MS
// от текущего аптайма (не от показаний часов — не зависит от самой коррекции).
static void net_schedule_next_ntp_resync(NetIface iface, seL4_CPtr timer_ep) {
    g_iface[iface].ntp_next_resync_uptime_ms = sys_get_uptime_ms(timer_ep) + NTP_RESYNC_INTERVAL_MS;
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
static void net_check_ntp_resync(NetIface iface, seL4_CPtr console_ep, seL4_CPtr timer_ep) {
    NetIfaceState &s = g_iface[iface];
    if (s.ntp_outstanding || s.pending_cmd == NET_CMD_NTP) return;
    if (sys_get_uptime_ms(timer_ep) < s.ntp_next_resync_uptime_ms) return;
    if (!s.dhcp_bound) { net_schedule_next_ntp_resync(iface, timer_ep); return; } // нет IP — нечего слать, пробуем позже

    // Фикс шума в консоли (см. situation.txt): периодический авторесинк
    // больше ничего не печатает — ни этого объявления, ни "Sending NTP
    // request"/"NTP sync OK" внутри net_send_ntp_request()/обработчика
    // ответа (см. ntp_is_periodic). Ручная команда `ntp` по-прежнему
    // печатает всё как раньше.
    s.ntp_is_periodic = true;
    uint8_t dummy_mac[6];
    if (resolve_dest_mac(iface, g_ntp_server_ip, dummy_mac)) {
        net_send_ntp_request(iface, console_ep, timer_ep);
    } else {
        s.pending_cmd = NET_CMD_NTP;
        uint8_t arp_target[4];
        arp_target_for(iface, g_ntp_server_ip, arp_target);
        net_send_arp_request(iface, console_ep, arp_target);
    }
    net_schedule_next_ntp_resync(iface, timer_ep);
}

static void unpack_ipv4(seL4_Word packed, uint8_t out[4]) {
    out[0] = (uint8_t)((packed >> 24) & 0xFF);
    out[1] = (uint8_t)((packed >> 16) & 0xFF);
    out[2] = (uint8_t)((packed >> 8) & 0xFF);
    out[3] = (uint8_t)(packed & 0xFF);
}

// Фаза 4.5.6: заголовок команды вырос с 4 слов (cmd/ip/port/text_len) до 5
// (добавился iface в MR4, см. net_handle_command) — текст теперь с MR5, не MR4.
static void copy_text_from_mrs(char *dst, int max_len, int text_len, int msg_words) {
    const int word_bytes = sizeof(seL4_Word);
    int available = (msg_words - 5) * word_bytes;
    if (text_len > available) text_len = available;
    if (text_len < 0) text_len = 0;
    if (text_len >= max_len) text_len = max_len - 1;

    for (int i = 0; i < text_len; i++) {
        seL4_Word word = seL4_GetMR(5 + (i / word_bytes));
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

static void net_send_dns_query(NetIface iface, seL4_CPtr console_ep, const char* domain) {
    NetIfaceState &s = g_iface[iface];
    uint32_t tx_offset = 0x280;  // Возвращаем проверенное смещение

    volatile ethernet_frame* eth = (volatile ethernet_frame*)(g_shm_vaddr + tx_offset);

    uint8_t dest_mac[6];
    resolve_dest_mac(iface, s.dns_ip, dest_mac); // вызывающий код уже убедился, что резолв готов
    for(int i = 0; i < 6; i++) eth->dest_mac[i] = dest_mac[i];
    for(int i = 0; i < 6; i++) eth->src_mac[i] = s.mac[i];
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

    for (int i = 0; i < 4; i++) ip->saddr[i] = s.ip[i];

    // DNS-сервер: из DHCP (опция 6), либо запасной 8.8.8.8 (см. dns_ip) — он
    // переварит нулевую UDP чексумму.
    for (int i = 0; i < 4; i++) ip->daddr[i] = s.dns_ip[i];

    ip->check = 0;
    ip->check = calculate_checksum((void*)ip, sizeof(ipv4_header));

    volatile dns_header* dns = (volatile dns_header*)((char*)udp + sizeof(udp_header));
    dns->id = htons(s.dns_id);
    dns->flags = htons(0x0100);
    dns->q_count = htons(1);
    dns->ans_count = dns->auth_count = dns->add_count = 0;

    uint16_t* qtype = (uint16_t*)(qname + name_len);
    qtype[0] = htons(1); qtype[1] = htons(1);

    uint32_t packet_len = 14 + sizeof(ipv4_header) + total_udp_len;

    sys_puts(console_ep, "[NET] Sending DNS Query (");
    put_dec(console_ep, packet_len);
    sys_puts(console_ep, " bytes) to ");
    put_ip(console_ep, s.dns_ip);
    sys_puts(console_ep, "...\n");

    net_send_packet(iface, packet_len, tx_offset);

    s.dns_outstanding = true;
}

// Команды, которым нужен собственный IP/шлюз (ping/send/resolve/ntp), должны
// дождаться g_dhcp_bound — иначе они уйдут слать ARP на g_gateway_ip=0.0.0.0,
// ответа не будет никогда, и shell зависнет на 10с до аварийного respawn'а
// net_driver'а (см. shell.cpp). Явная ошибка сразу + разблокировка mailbox —
// куда лучше такого зависания.
static bool net_require_ip(NetIface iface, seL4_CPtr console_ep) {
    if (g_iface[iface].dhcp_bound) return true;
    sys_puts(console_ep, "[NET] Error: no IP address yet (DHCP pending). Try again shortly.\n");
    volatile int* net_mailbox = (volatile int*)(g_shm_vaddr + net_mailbox_ready_offset(iface));
    net_mailbox[0] = 0;
    return false;
}

// Фаза 4.5 (см. ROADMAP.md): раньше сама делала seL4_NBRecv(net_cmd_ep) —
// теперь сообщение уже получено ГЛАВНЫМ циклом (обычный блокирующий
// seL4_Recv, комбинированный с GENET RX/heartbeat нотификацией, см. main()
// ниже), эта функция только разбирает уже лежащие в IPC-буфере MR.
//
// ПОКА (Фаза 4.5.2) команды шелла всегда выполняются на IFACE_GENET —
// протокол IPC шелл<->net_driver ещё не несёт признак интерфейса (это
// Фаза 4.5.6, вместе с переделкой shell.cpp под "-i genet|wifi"). До тех пор
// это ЧИСТАЯ регрессия: Wi-Fi всё равно инертен, поведение не отличается от
// того, что было до этой сессии.
static void net_handle_command(seL4_CPtr console_ep, seL4_CPtr timer_ep, seL4_MessageInfo_t info) {
    int len = seL4_MessageInfo_get_length(info);
    if (len == 0) return;

    seL4_Word cmd = seL4_GetMR(0);
    // Фаза 4.5.6: MR4 — какой интерфейс выбрал шелл (см. net_send_text_command()
    // в shell.cpp, "-i genet|wifi"). len>4 всегда истинно для обновлённого
    // протокола (минимум 5 слов), проверка — чисто защитная.
    NetIface iface = (len > 4 && seL4_GetMR(4) == 1) ? IFACE_WIFI : IFACE_GENET;

    NetIfaceState &s = g_iface[iface];

    if (cmd == NET_CMD_PING && len >= 3) {
        // ВАЖНО: SaveCaller — ПЕРВЫМ делом, до net_require_ip() (внутри
        // которого sys_puts() делает seL4_Call к uart_driver) и вообще
        // любого чужого IPC — тот же урок, что уже задокументирован в
        // timer_driver.cpp у SYS_SLEEP_MS: неявное право на reply держится
        // только до СЛЕДУЮЩЕГО Recv/Call этого треда.
        seL4_Word ping_slot = ping_reply_slot_for(iface);
        seL4_CNode_SaveCaller(SELF_CNODE_SLOT, ping_slot, 8);

        if (!net_require_ip(iface, console_ep)) {
            // Иначе шелл напечатал бы "ping statistics" с мусором предыдущей
            // серии (или вовсе непроинициализированной памятью, если серия
            // ни разу не стартовала в эту сессию) для запроса, который даже
            // не отправил ни одного пакета.
            uint32_t stats_off = net_mailbox_ping_stats_offset(iface);
            for (int i = 0; i < 7; i++) *(uint32_t*)(g_shm_vaddr + stats_off + i * 4) = 0;
            net_ping_reply(iface);
            return;
        }
        uint8_t dst_ip[4];
        unpack_ipv4(seL4_GetMR(1), dst_ip);
        uint32_t count = (uint32_t)seL4_GetMR(2);
        if (count == 0) count = 1;
        if (count > 16) count = 16;

        sys_puts(console_ep, "[NET] Shell requested ICMP Ping x");
        put_dec(console_ep, count);
        sys_puts(console_ep, ".\n");
        uint8_t dummy_mac[6];
        if (resolve_dest_mac(iface, dst_ip, dummy_mac)) {
            net_start_ping_series(iface, console_ep, timer_ep, dst_ip, count);
        } else {
            for (int i = 0; i < 4; i++) s.pending_ip[i] = dst_ip[i];
            s.pending_ping_count = count;
            s.pending_cmd = NET_CMD_PING;
            s.ping_arp_deadline_ms = sys_get_uptime_ms(timer_ep) + PING_ARP_TIMEOUT_MS;
            uint8_t arp_target[4];
            arp_target_for(iface, dst_ip, arp_target);
            net_send_arp_request(iface, console_ep, arp_target);
        }
    } else if (cmd == NET_CMD_SEND && len >= 4) {
        if (!net_require_ip(iface, console_ep)) return;
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
        if (resolve_dest_mac(iface, dst_ip, dummy_mac2)) {
            net_send_udp(iface, console_ep, dst_ip, dst_port, msg);
        } else {
            for (int i = 0; i < 4; i++) s.pending_udp_ip[i] = dst_ip[i];
            s.pending_udp_port = dst_port;
            my_memcpy(s.pending_udp, msg, my_strlen(msg) + 1);
            s.pending_cmd = NET_CMD_SEND;
            uint8_t arp_target[4];
            arp_target_for(iface, dst_ip, arp_target);
            net_send_arp_request(iface, console_ep, arp_target);
        }
    } else if (cmd == NET_CMD_STATUS) {
        sys_puts(console_ep, iface == IFACE_WIFI ? "[NET] Status: iface=wifi link=" : "[NET] Status: iface=genet link=");
        sys_puts(console_ep, s.link_up ? "up" : "down");
        sys_puts(console_ep, " dhcp=");
        sys_puts(console_ep, s.dhcp_state == DHCP_BOUND ? "bound" :
                              s.dhcp_state == DHCP_DISCOVERING ? "discovering" :
                              s.dhcp_state == DHCP_REQUESTING ? "requesting" :
                              s.dhcp_state == DHCP_RENEWING ? "renewing" : "idle");
        sys_puts(console_ep, " ip=");
        put_ip(console_ep, s.ip);
        sys_puts(console_ep, " gw=");
        put_ip(console_ep, s.gateway_ip);
        sys_puts(console_ep, " mask=");
        put_ip(console_ep, s.subnet_mask);
        sys_puts(console_ep, " dns=");
        put_ip(console_ep, s.dns_ip);
        sys_puts(console_ep, " router_mac=");
        if (s.have_router_mac) {
            sys_puts(console_ep, "known ");
            for (int i = 0; i < 6; i++) {
                if (i > 0) sys_puts(console_ep, ":");
                put_hex_byte(console_ep, s.router_mac[i]);
            }
        } else {
            sys_puts(console_ep, "unknown");
        }
        sys_puts(console_ep, " onlink_peer=");
        if (s.have_onlink_mac) {
            put_ip(console_ep, s.onlink_ip);
            sys_puts(console_ep, "=");
            for (int i = 0; i < 6; i++) {
                if (i > 0) sys_puts(console_ep, ":");
                put_hex_byte(console_ep, s.onlink_mac[i]);
            }
        } else {
            sys_puts(console_ep, "none");
        }
        if (iface == IFACE_GENET) { // tx/rx-кольцо — GENET-специфичная деталь, у Wi-Fi (SHM-mailbox) нет аналога
            sys_puts(console_ep, " tx_idx=");
            put_dec(console_ep, g_tx_index);
            sys_puts(console_ep, " rx_c_idx=");
            put_dec(console_ep, g_rx_c_index);
        }
        sys_puts(console_ep, " rx_irq_wakeups=");
        put_dec(console_ep, s.rx_irq_wakeups);
        sys_puts(console_ep, " heartbeat_wakeups=");
        put_dec(console_ep, g_heartbeat_wakeups);
        sys_puts(console_ep, " default_udp=");
        put_ip(console_ep, default_udp_ip);
        sys_puts(console_ep, ":");
        put_dec(console_ep, default_udp_port);
        sys_puts(console_ep, " ping_sent=");
        put_dec(console_ep, s.ping_sent_count);
        sys_puts(console_ep, " ping_reply=");
        put_dec(console_ep, s.ping_reply_count);
        sys_puts(console_ep, " ping_timeout=");
        put_dec(console_ep, s.ping_timeout_count);
        if (s.ping_reply_count > 0) {
            sys_puts(console_ep, " rtt_last=");
            put_duration_us(console_ep, s.ping_last_rtt_us);
            sys_puts(console_ep, " rtt_avg=");
            put_duration_us(console_ep, s.ping_total_rtt_us / s.ping_reply_count);
            sys_puts(console_ep, " rtt_min=");
            put_duration_us(console_ep, s.ping_min_rtt_us);
            sys_puts(console_ep, " rtt_max=");
            put_duration_us(console_ep, s.ping_max_rtt_us);
        }
        sys_puts(console_ep, " ntp_synced=");
        sys_puts(console_ep, s.ntp_synced ? "yes" : "no");
        if (s.ntp_synced) {
            sys_puts(console_ep, " ntp_offset=");
            if (s.ntp_last_offset_negative) sys_puts(console_ep, "-");
            put_dec(console_ep, s.ntp_last_offset_s);
            sys_puts(console_ep, "s");
        }
        sys_puts(console_ep, "\n");
        volatile int* net_mailbox = (volatile int*)(g_shm_vaddr + net_mailbox_ready_offset(iface));
        net_mailbox[0] = 0;

    } else if (cmd == NET_CMD_RECV) {
        if (s.udp_rx_ready) {
            sys_puts(console_ep, "[NET] UDP datagram from ");
            put_ip(console_ep, s.udp_rx_src_ip);
            sys_puts(console_ep, ":");
            put_dec(console_ep, s.udp_rx_src_port);
            sys_puts(console_ep, " (");
            put_dec(console_ep, s.udp_rx_len);
            sys_puts(console_ep, " bytes): ");
            sys_puts(console_ep, s.udp_rx_data);
            sys_puts(console_ep, "\n");
            s.udp_rx_ready = false;
        } else {
            sys_puts(console_ep, "[NET] No pending UDP datagrams.\n");
        }
        volatile int* net_mailbox = (volatile int*)(g_shm_vaddr + net_mailbox_ready_offset(iface));
        net_mailbox[0] = 0;

    } else if (cmd == NET_CMD_RESOLVE) {
        if (!net_require_ip(iface, console_ep)) return;
        int text_len = (int)seL4_GetMR(3);
        copy_text_from_mrs(s.dns_pending_domain, sizeof(s.dns_pending_domain), text_len, len);

        uint8_t dummy_mac3[6];
        if (resolve_dest_mac(iface, s.dns_ip, dummy_mac3)) {
            net_send_dns_query(iface, console_ep, s.dns_pending_domain);
        } else {
            s.pending_cmd = NET_CMD_RESOLVE;
            uint8_t arp_target[4];
            arp_target_for(iface, s.dns_ip, arp_target);
            net_send_arp_request(iface, console_ep, arp_target);
        }

    } else if (cmd == NET_CMD_NTP) {
        if (!net_require_ip(iface, console_ep)) return;
        s.ntp_is_periodic = false; // ручная команда — печатаем всё, см. ntp_is_periodic
        uint8_t dummy_mac4[6];
        if (s.ntp_outstanding) {
            sys_puts(console_ep, "[NET] NTP request already in flight.\n");
            volatile int* net_mailbox = (volatile int*)(g_shm_vaddr + net_mailbox_ready_offset(iface));
            net_mailbox[0] = 0;
        } else if (resolve_dest_mac(iface, g_ntp_server_ip, dummy_mac4)) {
            net_send_ntp_request(iface, console_ep, timer_ep);
        } else {
            s.pending_cmd = NET_CMD_NTP;
            uint8_t arp_target[4];
            arp_target_for(iface, g_ntp_server_ip, arp_target);
            net_send_arp_request(iface, console_ep, arp_target);
        }

    } else if (cmd == NET_CMD_NTP_STATUS) {
        // Чисто локальный запрос (см. ROADMAP.md/situation.txt) — никакого
        // сетевого обмена, только текущее состояние; в отличие от `ntp`
        // (ручная ресинхронизация), это просто чтение, поэтому обычный
        // Send+mailbox-poll вместо блокирующего Call — как у NET_CMD_STATUS.
        uint64_t now_up = sys_get_uptime_ms(timer_ep);
        sys_puts(console_ep, iface == IFACE_WIFI ? "[NET] NTP status (wifi): synced=" : "[NET] NTP status (genet): synced=");
        sys_puts(console_ep, s.ntp_synced ? "yes" : "no");
        if (s.ntp_synced) {
            sys_puts(console_ep, " offset=");
            if (s.ntp_last_offset_negative) sys_puts(console_ep, "-");
            put_dec(console_ep, s.ntp_last_offset_s);
            sys_puts(console_ep, "s last_sync=");
            put_dec(console_ep, (uint32_t)((now_up - s.ntp_last_sync_uptime_ms) / 1000));
            sys_puts(console_ep, "s ago");
        }
        if (s.ntp_next_resync_uptime_ms > now_up) {
            sys_puts(console_ep, " next_resync=");
            put_dec(console_ep, (uint32_t)((s.ntp_next_resync_uptime_ms - now_up) / 1000));
            sys_puts(console_ep, "s");
        }
        sys_puts(console_ep, "\n");
        volatile int* net_mailbox = (volatile int*)(g_shm_vaddr + net_mailbox_ready_offset(iface));
        net_mailbox[0] = 0;
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

// Фаза 6 (SMP): общий межпроцессный мьютекс на нотификации (shell/
// net_driver/wifi_driver, см. main.cpp/vfs_mutex_ntfn) вместо старого
// non-atomic busy-spin флага на офсете 4096 разделяемой SHM. Нужен, потому
// что net_log_flush() ниже пишет в тот же офсет 0/128, что shell использует
// для ЛЮБОГО файлового syscall'а (ps/cat/touch/...) — без лока фоновая
// запись журнала (по приходу произвольного UDP-пакета, независимо от команд
// шелла) может перезаписать буфер посреди чужого запроса (был замечен на
// живом железе: `ps` иногда печатал "/root/net_udp.log" вместо таблицы
// процессов). Старый флаг был безопасен только пока не было настоящего
// межъядерного параллелизма: та SHM мапится некэшируемой Device-памятью
// (ради когерентности с GENET DMA), а exclusive-load/store инструкции
// (LDXR/STXR — hardware atomic) на Device-памяти по спеке ARM дают
// непредсказуемое поведение — на живом железе это уже роняло ВЕСЬ kernel
// seL4 необрабатываемым исключением шины ("halting... Kernel entry via
// Unknown (0)"). Мьютекс на нотификации не трогает эту память вообще —
// состояние живёт в ядре (seL4_Wait/Signal).
static inline void net_vfs_lock() {
    if (!g_vfs_mutex_ep) return;
    seL4_Word badge;
    seL4_Wait(g_vfs_mutex_ep, &badge);
}
static inline void net_vfs_unlock() {
    if (!g_vfs_mutex_ep) return;
    seL4_Signal(g_vfs_mutex_ep);
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

// Верхняя граница реального буфера RX-кадра, ЗАВИСИТ ОТ ИНТЕРФЕЙСА — GENET
// использует физический RX-дескриптор (GENET_RX_BUF_LENGTH байт с начала eth,
// см. genet_rx_descs_init()), Wi-Fi — SHM-мейлбокс фиксированного размера
// (см. WIFI_SHM_RX_DATA_OFFSET, Фаза 4.5.3). Используется, чтобы разбор
// DNS/NTP/DHCP-ответов и произвольных UDP-датаграмм не ушёл за пределы
// реально выделенного буфера, независимо от того, что о своей длине
// заявляют поля самого пакета.
constexpr uint32_t WIFI_RX_FRAME_BUF_CAP = 1536; // должно совпадать с размером WIFI_SHM_RX_DATA_OFFSET (Фаза 4.5.3)
static inline uint8_t* net_rx_buffer_end(NetIface iface, volatile ethernet_frame* eth) {
    uint32_t cap = (iface == IFACE_GENET) ? (GENET_RX_BUF_LENGTH - GENET_RX_BUF_OFFSET) : WIFI_RX_FRAME_BUF_CAP;
    return (uint8_t*)eth + cap;
}

static void net_poll(NetIface iface, seL4_CPtr console_ep, seL4_CPtr timer_ep, seL4_CPtr root_ep) {
    NetIfaceState &s = g_iface[iface];
    uint8_t* frame_ptr;
    uint32_t frame_len;
    while (net_hw_poll_rx(iface, &frame_ptr, &frame_len)) {
        volatile ethernet_frame* eth = (volatile ethernet_frame*)frame_ptr;

        uint16_t type = htons(eth->ethertype);

        if (type == 0x0806) { // ARP
            volatile arp_ipv4* arp_reply = (volatile arp_ipv4*)eth->payload;
            if (htons(arp_reply->oper) == 1) { // ARP-запрос от кого-то ещё
                uint8_t tpa[4]; for (int i = 0; i < 4; i++) tpa[i] = arp_reply->tpa[i];
                if (s.dhcp_bound && ip_eq(tpa, s.ip)) {
                    uint8_t requester_ip[4], requester_mac[6];
                    for (int i = 0; i < 4; i++) requester_ip[i] = arp_reply->spa[i];
                    for (int i = 0; i < 6; i++) requester_mac[i] = arp_reply->sha[i];
                    net_send_arp_reply(iface, console_ep, requester_ip, requester_mac);
                }
            } else if (htons(arp_reply->oper) == 2) {
                uint8_t spa[4]; for (int i = 0; i < 4; i++) spa[i] = arp_reply->spa[i];
                bool is_gateway = ip_eq(spa, s.gateway_ip);
                bool already_had = is_gateway ? s.have_router_mac : (s.have_onlink_mac && ip_eq(spa, s.onlink_ip));

                if (!already_had) {
                    if (is_gateway) {
                        sys_puts(console_ep, "[NET RX] ARP Reply Received! Saving Router MAC.\n");
                        for(int i=0; i<6; i++) s.router_mac[i] = arp_reply->sha[i];
                        s.have_router_mac = true;
                    } else {
                        sys_puts(console_ep, "[NET RX] ARP Reply Received! Saving on-link peer MAC (");
                        put_ip(console_ep, spa);
                        sys_puts(console_ep, ").\n");
                        for (int i = 0; i < 4; i++) s.onlink_ip[i] = spa[i];
                        for(int i = 0; i < 6; i++) s.onlink_mac[i] = arp_reply->sha[i];
                        s.have_onlink_mac = true;
                    }

                    // Возобновляем ожидающую команду, только если резолвился именно
                    // тот MAC, которого она ждала — гейтвей и сосед по подсети
                    // резолвятся независимо и не мешают друг другу.
                    uint8_t dummy[6];
                    if (s.pending_cmd == NET_CMD_RESOLVE && resolve_dest_mac(iface, s.dns_ip, dummy)) {
                        sys_puts(console_ep, "[NET] ARP ready. Launching DNS Query...\n");
                        s.pending_cmd = NET_CMD_NONE;
                        net_send_dns_query(iface, console_ep, s.dns_pending_domain);
                    } else if (s.pending_cmd == NET_CMD_PING && resolve_dest_mac(iface, s.pending_ip, dummy)) {
                        uint32_t count = s.pending_ping_count;
                        s.pending_cmd = NET_CMD_NONE;
                        s.ping_arp_deadline_ms = 0;
                        net_start_ping_series(iface, console_ep, timer_ep, s.pending_ip, count);
                    } else if (s.pending_cmd == NET_CMD_SEND && resolve_dest_mac(iface, s.pending_udp_ip, dummy)) {
                        net_send_udp(iface, console_ep, s.pending_udp_ip, s.pending_udp_port, s.pending_udp);
                        s.pending_cmd = NET_CMD_NONE;
                    } else if (s.pending_cmd == NET_CMD_NTP && resolve_dest_mac(iface, g_ntp_server_ip, dummy)) {
                        s.pending_cmd = NET_CMD_NONE;
                        net_send_ntp_request(iface, console_ep, timer_ep);
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
                    bool matched = s.ping_outstanding && seq == s.ping_outstanding_seq;
                    // Честные микросекунды через аппаратный счётчик (read_cntvct/g_cntfrq
                    // выше) — раньше через timer_ep IPC, разрешение которого только целые
                    // миллисекунды (отсюда всегда ".000" для локальных хостов с реальным
                    // RTT сильно меньше 1мс), а до этого — вообще произвольная оценка по
                    // числу циклов главного цикла, никак не привязанная к настоящему времени.
                    uint64_t rtt_us = matched ? ((read_cntvct() - s.ping_sent_cyc) * 1000000ULL) / g_cntfrq : 0;
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

                        s.ping_outstanding = false;
                        net_record_ping_rtt(iface, rtt_us);
                        net_schedule_next_ping(iface, timer_ep);
                    }
                }
            }

            else if (ip->protocol == 17) { // UDP
                volatile udp_header* udp = (volatile udp_header*)(eth->payload + (ip->ihl_version & 0x0F) * 4);

                if (htons(udp->src_port) == 53) {
                    sys_puts(console_ep, ">>> [NET RX] UDP Response from Port 53 captured!\n");

                    volatile dns_header* dns = (volatile dns_header*)((char*)udp + sizeof(udp_header));

                    if (htons(dns->id) == s.dns_id && htons(dns->ans_count) > 0) {
                        // Буфер этого RX-кадра ограничен net_rx_buffer_end() (зависит от
                        // интерфейса — GENET-дескриптор или Wi-Fi SHM-мейлбокс); reader не
                        // должен уходить за эту границу, иначе — неограниченный OOB read при
                        // DNS-ответе без корректного нуль-терминатора имени.
                        uint8_t* buffer_end = net_rx_buffer_end(iface, eth);
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

                        volatile int* net_mailbox = (volatile int*)(g_shm_vaddr + net_mailbox_ready_offset(iface));

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
                                *((seL4_Word*)(g_shm_vaddr + net_mailbox_dns_ip_offset(iface))) = packed_ip;

                                net_mailbox[0] = 0;
                                s.dns_outstanding = false;
                            } else {
                                sys_puts(console_ep, ">>> [NET RX] DNS Error: Unexpected data_len (CNAME?). Length=");
                                put_dec(console_ep, data_len); sys_puts(console_ep, "\n");
                                // Не A-запись (например, честный CNAME) — не блокируем shell
                                // на 10с/respawn, а сразу сообщаем, что резолвить нечего.
                                net_mailbox[0] = 0;
                                s.dns_outstanding = false;
                            }
                        } else {
                            sys_puts(console_ep, ">>> [NET RX] DNS Error: malformed/truncated response, ignored.\n");
                            net_mailbox[0] = 0;
                            s.dns_outstanding = false;
                        }
                    } else {
                        sys_puts(console_ep, ">>> [NET RX] DNS Ignored: ID mismatch or 0 answers.\n");
                    }
                } else if (htons(udp->src_port) == NTP_SERVER_PORT && s.ntp_outstanding) {
                    // Ответ SNTP-сервера на наш запрос из net_send_ntp_request().
                    uint8_t* buffer_end = net_rx_buffer_end(iface, eth);
                    uint8_t* data_ptr = (uint8_t*)udp + sizeof(udp_header);
                    volatile int* net_mailbox = (volatile int*)(g_shm_vaddr + net_mailbox_ready_offset(iface));
                    s.ntp_outstanding = false;

                    if (data_ptr + sizeof(ntp_packet) > buffer_end) {
                        if (!s.ntp_is_periodic) sys_puts(console_ep, "[NET] NTP Error: truncated response, ignored.\n");
                        net_mailbox[0] = 0;
                    } else {
                        volatile ntp_packet* ntp = (volatile ntp_packet*)data_ptr;
                        // Mode=4 (server) ожидается в ответ на наш Mode=3 (client) запрос;
                        // stratum=0 значит Kiss-o'-Death (сервер отказывается отвечать).
                        if (ntp->stratum == 0 || (ntp->li_vn_mode & 0x07) != 4) {
                            if (!s.ntp_is_periodic) sys_puts(console_ep, "[NET] NTP Error: Kiss-o'-Death or unexpected mode, ignored.\n");
                            net_mailbox[0] = 0;
                        } else {
                            uint64_t t1 = s.ntp_t1_epoch_s; // raw uptime, см. фикс в net_send_ntp_request()
                            uint64_t t2 = (uint64_t)bswap32(ntp->rx_ts_sec) - NTP_UNIX_EPOCH_DELTA;
                            uint64_t t3 = (uint64_t)bswap32(ntp->tx_ts_sec) - NTP_UNIX_EPOCH_DELTA;
                            uint64_t t4 = sys_get_uptime_ms(timer_ep) / 1000ULL; // ИСПРАВЛЕНО: тоже raw uptime, не sys_get_time_ms()

                            // Стандартная формула офсета NTP; секундной точности достаточно
                            // для этой коррекции (округляем миллисекунды generic timer'а).
                            int64_t offset_s = ((int64_t)(t2 - t1) + (int64_t)(t3 - t4)) / 2;

                            seL4_SetMR(0, 5); // SYS_SET_TIME_OFFSET
                            seL4_SetMR(1, (seL4_Word)offset_s);
                            seL4_Call(timer_ep, seL4_MessageInfo_new(0, 0, 0, 2));

                            s.ntp_synced = true;
                            s.ntp_last_offset_negative = offset_s < 0;
                            s.ntp_last_offset_s = (uint32_t)(s.ntp_last_offset_negative ? -offset_s : offset_s);
                            s.ntp_last_sync_uptime_ms = sys_get_uptime_ms(timer_ep); // для `ntp status`

                            if (!s.ntp_is_periodic) {
                                sys_puts(console_ep, "[NET] NTP sync OK, offset ");
                                if (s.ntp_last_offset_negative) sys_puts(console_ep, "-");
                                put_dec(console_ep, s.ntp_last_offset_s);
                                sys_puts(console_ep, "s applied.\n");
                            }
                            net_mailbox[0] = 0;
                        }
                    }
                } else if (htons(udp->dst_port) == DHCP_CLIENT_PORT && htons(udp->src_port) == DHCP_SERVER_PORT) {
                    // Ответ DHCP-сервера (OFFER/ACK/NAK) на наш запрос из net_check_dhcp().
                    uint8_t* buffer_end = net_rx_buffer_end(iface, eth);
                    volatile dhcp_packet* dhcp = (volatile dhcp_packet*)((char*)udp + sizeof(udp_header));

                    if ((uint8_t*)dhcp + 240 <= buffer_end && bswap32(dhcp->xid) == s.dhcp_xid &&
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

                        if (msg_type == 2 && s.dhcp_state == DHCP_DISCOVERING) { // DHCPOFFER
                            for (int i = 0; i < 4; i++) s.dhcp_offered_ip[i] = dhcp->yiaddr[i];
                            for (int i = 0; i < 4; i++) s.dhcp_server_ip[i] = opt_server[i];
                            sys_puts(console_ep, "[NET] DHCP: OFFER ");
                            put_ip(console_ep, s.dhcp_offered_ip);
                            sys_puts(console_ep, " received, requesting...\n");
                            net_send_dhcp_packet(iface, console_ep, 3 /* DHCPREQUEST */);
                            s.dhcp_state = DHCP_REQUESTING;
                            s.dhcp_retry_uptime_ms = sys_get_uptime_ms(timer_ep) + DHCP_RETRY_MS;
                        } else if (msg_type == 5 && (s.dhcp_state == DHCP_REQUESTING || s.dhcp_state == DHCP_DISCOVERING || s.dhcp_state == DHCP_RENEWING)) { // DHCPACK
                            bool was_renewing = (s.dhcp_state == DHCP_RENEWING);
                            for (int i = 0; i < 4; i++) s.ip[i] = dhcp->yiaddr[i];
                            if (has_mask)   for (int i = 0; i < 4; i++) s.subnet_mask[i] = opt_mask[i];
                            if (has_router) for (int i = 0; i < 4; i++) s.gateway_ip[i] = opt_router[i];
                            if (has_dns)    for (int i = 0; i < 4; i++) s.dns_ip[i] = opt_dns[i];
                            if (lease_s == 0) lease_s = DHCP_DEFAULT_LEASE_S;

                            s.dhcp_state = DHCP_BOUND;
                            s.dhcp_bound = true;
                            s.dhcp_attempt = 0;
                            // На простом renew той же аренды сеть не менялась — старый
                            // MAC гейтвея/соседа всё ещё валиден, не сбрасываем (иначе
                            // лишний ARP сразу после каждого renew, без всякой нужды).
                            if (!was_renewing) {
                                s.have_router_mac = false; // прежний MAC гейтвея (если был) мог устареть с новым адресом
                                s.have_onlink_mac = false; // новая подсеть — старый MAC соседа по ней уже неактуален
                            }

                            s.dhcp_lease_deadline_uptime_ms = sys_get_uptime_ms(timer_ep) + ((uint64_t)lease_s * 1000ULL) / 2ULL;

                            sys_puts(console_ep, was_renewing ? "[NET] DHCP: lease renewed, ip=" : "[NET] DHCP: bound ip=");
                            put_ip(console_ep, s.ip);
                            sys_puts(console_ep, " gw="); put_ip(console_ep, s.gateway_ip);
                            sys_puts(console_ep, " mask="); put_ip(console_ep, s.subnet_mask);
                            sys_puts(console_ep, " dns="); put_ip(console_ep, s.dns_ip);
                            sys_puts(console_ep, " lease="); put_dec(console_ep, lease_s);
                            sys_puts(console_ep, "s\n");
                        } else if (msg_type == 6) { // DHCPNAK
                            sys_puts(console_ep, "[NET] DHCP: NAK received, restarting discovery.\n");
                            s.dhcp_state = DHCP_IDLE;
                            s.dhcp_bound = false;
                        }
                    }
                } else {
                    // Произвольная входящая UDP-датаграмма (не DNS) — сохраняем для команды `recv`.
                    uint8_t* buffer_end = net_rx_buffer_end(iface, eth);
                    uint8_t* data_ptr = (uint8_t*)udp + sizeof(udp_header);
                    uint16_t udp_len = htons(udp->len);
                    int payload_len = (udp_len > sizeof(udp_header)) ? (udp_len - sizeof(udp_header)) : 0;

                    int avail = (data_ptr < buffer_end) ? (int)(buffer_end - data_ptr) : 0;
                    if (payload_len > avail) payload_len = avail;
                    if (payload_len > (int)sizeof(s.udp_rx_data) - 1) payload_len = (int)sizeof(s.udp_rx_data) - 1;
                    if (payload_len < 0) payload_len = 0;

                    for (int i = 0; i < payload_len; i++) s.udp_rx_data[i] = (char)data_ptr[i];
                    s.udp_rx_data[payload_len] = '\0';
                    s.udp_rx_len = payload_len;

                    for (int i = 0; i < 4; i++) s.udp_rx_src_ip[i] = ip->saddr[i];
                    s.udp_rx_src_port = htons(udp->src_port);
                    s.udp_rx_ready = true;

                    // В консоль больше не пишем (см. комментарий у net_log_udp выше) —
                    // только в /root/net_udp.log, чтобы не тормозить главный цикл.
                    net_log_udp(timer_ep, s.udp_rx_src_ip, s.udp_rx_src_port, payload_len);
                }
            } // Конец проверки UDP
        } // Конец проверки IPv4

        net_hw_rx_done(iface);
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
    seL4_CPtr irq_ep     = ipc->msg[BOOT_IRQ_EP]; // Фаза 4.5: настоящая IRQHandler-капа GENET RX (RPI4_GENET_IRQ_A), не общая ни с кем
    g_blk_ep = ipc->msg[7]; // см. main.cpp: local_blk_ep=7 — нужен только для net_log_udp()
    g_wifi_tx_wake_ntfn = ipc->msg[BOOT_WIFI_TX_WAKE_CAP]; // Фаза 4.5.4: капа сигнала wifi_driver'у "кадр в TX-mailbox" (см. wifi_hw_send())
    g_vfs_mutex_ep = ipc->msg[BOOT_VFS_MUTEX_NTFN_CAP]; // Фаза 6 (SMP, см. common.h)
    seL4_CPtr self_tcb = ipc->msg[BOOT_SELF_TCB_CAP]; // Фаза 6.1 (продолжение, см. ROADMAP.md)

    if (my_ep == 0) {
        __assert_fail("FATAL: Null Capability #0 Detected!", __FILE__, __LINE__, __func__);
    }

    // =========================================================
    // 1. ДИНАМИЧЕСКИЙ ЗАПРОС SHM (Убираем хардкод)
    // =========================================================
    seL4_SetMR(0, 107); // 107 = SYS_SHM_GET
    seL4_MessageInfo_t msg = seL4_MessageInfo_new(0, 0, 0, 1);
    seL4_Call(root_ep, msg);
    
    g_shm_vaddr = (char*)seL4_GetMR(0);
    g_shm_paddr = (uint32_t)seL4_GetMR(1);

    // Фаза 4.5 (Wi-Fi data-plane) — дефолты обоих интерфейсов ДО первого
    // реального использования (см. NetIfaceState/net_iface_init_defaults()
    // выше). IFACE_WIFI остаётся полностью инертным (link_up=false) до
    // Фазы 4.5.3 — здесь просто готовим структуру, ничего ещё не включаем.
    net_iface_init_defaults(IFACE_GENET);
    net_iface_init_defaults(IFACE_WIFI);

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

    // Фаза 4.5 (см. ROADMAP.md): подписка на периодический будильник от
    // timer_driver — DHCP/ARP/ping/link-таймауты завязаны на wall-clock, а
    // не на GENET RX IRQ, им нужен отдельный "тик" даже когда кадры не
    // приходят вообще (см. SYS_TIMER_HEARTBEAT_SUBSCRIBE в timer_driver.cpp).
    // 20мс — с большим запасом ниже самого мелкого из реальных интервалов
    // (LINK_CHECK_INTERVAL_MS=1000 и т.д., см. константы выше) — не задержит
    // ничего заметно. Период общий на весь процесс (см. timer_driver.cpp,
    // heartbeat_period_ticks не per-подписчик) — ЭТО ЖЕ значение должно
    // совпадать с blk_driver.cpp's подпиской ниже, иначе кто подписался
    // последним молча переустановит период для всех (см. situation.txt:
    // уменьшено с 100мс, чтобы сократить худший случай ожидания EMMC-
    // прерывания в blk_driver с ~3с до ~0.6с).
    seL4_SetMR(0, 9); // 9 = SYS_TIMER_HEARTBEAT_SUBSCRIBE
    seL4_SetMR(1, 20);
    seL4_Call(timer_ep, seL4_MessageInfo_new(0, 0, 0, 2));

    // Не ждём ни PHY-линка, ни NTP-синхронизации — сигналим готовность сразу,
    // иначе при отключённом кабеле/недоступной сети вся загрузка (включая
    // shell) вставала бы на паузу. NTP (если и когда сеть появится) идёт
    // полностью в фоне через обычный периодический ресинк — g_ntp_next_resync_uptime_ms
    // по умолчанию 0, поэтому net_check_ntp_resync() сама сделает первую
    // попытку уже на первой итерации цикла ниже, никакого отдельного
    // "boot-time sync" пути не нужно.
    signal_net_driver_ready(root_ep);

    // Главный цикл (Фаза 4.5) — раньше был busy-yield с net_hw_poll_rx()
    // на КАЖДОЙ итерации и seL4_NBRecv() на команды клиента; теперь обычный
    // блокирующий seL4_Recv(), скомбинированный с нотификацией net_driver'а
    // (см. NET_EVENT_GENET_RX/NET_EVENT_HEARTBEAT в common.h, TCB-bind в
    // main.cpp) — поток реально спит, пока не случится ЛИБО кадр, ЛИБО тик
    // будильника, ЛИБО настоящее клиентское сообщение на net_cmd_ep.
    while (1) {
        seL4_Word badge = 0;
        seL4_MessageInfo_t info = seL4_Recv(net_cmd_ep, &badge);

        if (badge & (NET_EVENT_GENET_RX | NET_EVENT_HEARTBEAT | NET_EVENT_WIFI_RX)) {
            if (badge & NET_EVENT_GENET_RX) {
                // Сброс sticky-бита ПЕРЕД Ack (см. живой урок с EMMC2 в
                // blk_driver.cpp/ROADMAP.md 4.5) — порядок как в эталонном
                // bcmgenet.c: снимок текущего статуса, CLEAR, потом Ack.
                // Здесь это не грозит межпроцессным голоданием (собственная
                // линия, Ack делает тот же поток, что и обработку), но
                // порядок всё равно важен для чистоты — иначе GIC увидит
                // линию ещё "поднятой" и тут же передоставит то же событие.
                genet_write32(INTRL2_CPU_CLEAR, UMAC_IRQ_RXDMA_DONE);
                seL4_IRQHandler_Ack(irq_ep);
                g_iface[IFACE_GENET].rx_irq_wakeups++;
            }
            // Фаза 4.5.3+: wifi_driver сигналит этим же битом net_event_ntfn
            // при постановке кадра в свой RX-mailbox (см. common.h) — пока
            // (4.5.2) этот бит физически никогда не приходит (wifi_driver ещё
            // не умеет его слать), но счётчик уже готов.
            if (badge & NET_EVENT_WIFI_RX) g_iface[IFACE_WIFI].rx_irq_wakeups++;
            if (badge & NET_EVENT_HEARTBEAT) g_heartbeat_wakeups++;
            // GENET-специфичная проверка линка (реальный MDIO read) и Wi-Fi
            // (SHM-флаг от wifi_driver, Фаза 4.5.3) — независимые источники,
            // оба дёшевы, проверяем каждый тик.
            if (g_net_up) net_check_link_status(console_ep, timer_ep);
            net_check_wifi_link(console_ep);
            for (int i = 0; i < IFACE_COUNT; i++) {
                NetIface iface = (NetIface)i;
                if (iface == IFACE_GENET && !g_net_up) continue;
                net_check_dhcp(iface, console_ep, timer_ep);
                net_poll(iface, console_ep, timer_ep, root_ep);
                net_check_ping_timeout(iface, console_ep, timer_ep);
                net_check_ping_arp_timeout(iface, timer_ep);
                net_check_ping_send(iface, console_ep, timer_ep);
                net_check_ntp_resync(iface, console_ep, timer_ep);
                net_publish_iface_ready(iface);
            }
            continue;
        }

        // Фаза 6.1 (продолжение, см. ROADMAP.md): проверяем ДО "if (g_net_up)"
        // ниже — иначе root, вызывая эту команду через обычный блокирующий
        // seL4_Call, завис бы навсегда, если сеть физически не поднята
        // (кабель не подключён), потому что net_handle_command() (и вместе с
        // ним любой Reply) тогда вообще не вызывается. Операция мгновенная и
        // локальная, от состояния сети не зависит.
        if (seL4_MessageInfo_get_length(info) >= 1 && seL4_GetMR(0) == NET_CMD_BENCHMARK_RESET) {
            seL4_BenchmarkResetLog();
            seL4_SetMR(0, 0);
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 1));
            continue;
        }
        if (seL4_MessageInfo_get_length(info) >= 1 && seL4_GetMR(0) == NET_CMD_BENCHMARK_FINALIZE) {
            seL4_BenchmarkFinalizeLog();
            seL4_BenchmarkGetThreadUtilisation(self_tcb);
            seL4_Word idle_local = seL4_GetMR(4);  // BENCHMARK_IDLE_LOCALCPU_UTILISATION
            seL4_Word total_local = seL4_GetMR(9); // BENCHMARK_TOTAL_UTILISATION
            seL4_SetMR(0, idle_local);
            seL4_SetMR(1, total_local);
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 2));
            continue;
        }

        // Обычное сообщение от клиента (badge = pid, не бит нотификации).
        if (g_net_up) net_handle_command(console_ep, timer_ep, info);
    }

    return 0;
}