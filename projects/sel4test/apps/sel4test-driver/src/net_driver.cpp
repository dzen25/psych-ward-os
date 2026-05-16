// net_driver.cpp
#include <sel4/sel4.h>
#include <stdint.h>

__attribute__((weak)) LIBSEL4_THREAD_LOCAL seL4_IPCBuffer *__sel4_ipc_buffer = nullptr;

void __assert_fail(const char *expr, const char *file, int line, const char *func) { while (1) seL4_Yield(); }

static void sys_puts(seL4_CPtr ep, const char *str) {
    while (*str) {
        seL4_SetMR(0, 8); seL4_SetMR(1, (seL4_Word)*str++);
        seL4_Call(ep, seL4_MessageInfo_new(0, 0, 0, 2));
    }
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

static int my_strlen(const char* s) { int len = 0; while (s[len]) len++; return len; }

static uint16_t htons(uint16_t hostshort) { return ((hostshort >> 8) & 0xFF) | ((hostshort & 0xFF) << 8); }

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

// ==========================================
// Структуры
// ==========================================
struct VirtioMmioRegs { uint32_t magic_value; uint32_t version; uint32_t device_id; uint32_t vendor_id; uint32_t host_features; uint32_t host_features_sel; uint32_t reserved_1[2]; uint32_t guest_features; uint32_t guest_features_sel; uint32_t guest_page_size; uint32_t reserved_2; uint32_t queue_sel; uint32_t queue_num_max; uint32_t queue_num; uint32_t queue_align; uint32_t queue_pfn; uint32_t reserved_3[3]; uint32_t queue_notify; uint32_t reserved_4[3]; uint32_t interrupt_status; uint32_t interrupt_ack; uint32_t reserved_5[2]; uint32_t status; };
struct virtq_desc { uint64_t addr; uint32_t len; uint16_t flags; uint16_t next; };
struct virtq_used_elem { uint32_t id; uint32_t len; };
struct virtq_avail_rx { uint16_t flags; uint16_t idx; uint16_t ring[4]; }; 
struct virtq_used_rx { uint16_t flags; uint16_t idx; virtq_used_elem ring[4]; }; 

struct __attribute__((packed)) virtio_net_hdr { uint8_t flags; uint8_t gso_type; uint16_t hdr_len; uint16_t gso_size; uint16_t csum_start; uint16_t csum_offset; };
struct __attribute__((packed)) ethernet_frame { uint8_t dest_mac[6]; uint8_t src_mac[6]; uint16_t ethertype; char payload[1500]; };
struct __attribute__((packed)) arp_ipv4 { uint16_t htype; uint16_t ptype; uint8_t hlen; uint8_t plen; uint16_t oper; uint8_t sha[6]; uint8_t spa[4]; uint8_t tha[6]; uint8_t tpa[4]; };
struct __attribute__((packed)) ipv4_header { uint8_t ihl_version; uint8_t tos; uint16_t tot_len; uint16_t id; uint16_t frag_off; uint8_t ttl; uint8_t protocol; uint16_t check; uint8_t saddr[4]; uint8_t daddr[4]; };
struct __attribute__((packed)) icmp_header { uint8_t type; uint8_t code; uint16_t checksum; uint16_t id; uint16_t sequence; };

// НОВОЕ: Заголовок UDP (Всего 8 байт!)
struct __attribute__((packed)) udp_header {
    uint16_t src_port;
    uint16_t dst_port;
    uint16_t len;
    uint16_t checksum;
};

static volatile VirtioMmioRegs* g_net_regs = nullptr;
static uint8_t my_mac[6] = {0};
static uint8_t router_mac[6] = {0}; 
static bool have_router_mac = false;

static uint16_t g_tx_avail_idx = 0;
static uint16_t g_rx_avail_idx = 0;
static uint16_t g_last_rx_used_idx = 0; 
static uintptr_t rx_buffer_offsets[4] = { 0x2800, 0x2E00, 0x3400, 0x3A00 };

enum NetCommand {
    NET_CMD_NONE = 0,
    NET_CMD_PING = 1,
    NET_CMD_SEND = 2,
    NET_CMD_STATUS = 3,
};

static uint8_t default_udp_ip[4] = {10, 0, 2, 2};
static uint16_t default_udp_port = 8080;
static uint8_t pending_ip[4] = {10, 0, 2, 2};
static uint8_t pending_udp_ip[4] = {10, 0, 2, 2};
static uint16_t pending_udp_port = 8080;
static char pending_udp[64];
static int pending_cmd = NET_CMD_NONE;


// ==========================================
// Сетевые функции
// ==========================================
static void net_send_packet(uint32_t total_len) {
    volatile virtq_desc* vq_desc = (volatile virtq_desc*)(0x502000 + 0x200);
    uint16_t* avail_ring = (uint16_t*)(0x502000 + 0x224);
    volatile uint16_t* avail_idx = (volatile uint16_t*)(0x502000 + 0x222);
    volatile uint16_t* used_idx = (volatile uint16_t*)(0x502000 + 0x242);
    vq_desc[0].addr = 0x60000000 + 0x280; vq_desc[0].len = total_len; vq_desc[0].flags = 0; vq_desc[0].next = 0;
    avail_ring[g_tx_avail_idx % 2] = 0; g_tx_avail_idx++; *avail_idx = g_tx_avail_idx;
    g_net_regs->queue_notify = 1; 
    while (*used_idx < g_tx_avail_idx) { seL4_Yield(); }
}

static void net_send_arp_request(seL4_CPtr root_ep) {
    volatile virtio_net_hdr* net_hdr = (volatile virtio_net_hdr*)(0x502000 + 0x280);
    net_hdr->flags = 0; net_hdr->gso_type = 0; net_hdr->hdr_len = 0; net_hdr->gso_size = 0; net_hdr->csum_start = 0; net_hdr->csum_offset = 0;
    volatile ethernet_frame* eth = (volatile ethernet_frame*)(0x502000 + 0x280 + sizeof(virtio_net_hdr));
    
    for(int i=0; i<6; i++) eth->dest_mac[i] = 0xFF;
    for(int i=0; i<6; i++) eth->src_mac[i] = my_mac[i];
    eth->ethertype = htons(0x0806); 
    
    volatile arp_ipv4* arp = (volatile arp_ipv4*)eth->payload;
    arp->htype = htons(1); arp->ptype = htons(0x0800); arp->hlen = 6; arp->plen = 4; arp->oper = htons(1);        
    for(int i=0; i<6; i++) arp->sha[i] = my_mac[i];
    arp->spa[0] = 10; arp->spa[1] = 0; arp->spa[2] = 2; arp->spa[3] = 15; 
    for(int i=0; i<6; i++) arp->tha[i] = 0;
    arp->tpa[0] = 10; arp->tpa[1] = 0; arp->tpa[2] = 2; arp->tpa[3] = 2;  

    sys_puts(root_ep, "\n[NET DRIVER] Broadcasting ARP Request for 10.0.2.2...\n");
    net_send_packet(sizeof(virtio_net_hdr) + 14 + 28);
}

static void net_send_ping(seL4_CPtr root_ep, const uint8_t dst_ip[4]) {
    volatile virtio_net_hdr* net_hdr = (volatile virtio_net_hdr*)(0x502000 + 0x280);
    net_hdr->flags = 0; net_hdr->gso_type = 0; net_hdr->hdr_len = 0; net_hdr->gso_size = 0; net_hdr->csum_start = 0; net_hdr->csum_offset = 0;
    volatile ethernet_frame* eth = (volatile ethernet_frame*)(0x502000 + 0x280 + sizeof(virtio_net_hdr));
    
    for(int i=0; i<6; i++) eth->dest_mac[i] = router_mac[i];
    for(int i=0; i<6; i++) eth->src_mac[i] = my_mac[i];
    eth->ethertype = htons(0x0800); 
    
    volatile ipv4_header* ip = (volatile ipv4_header*)eth->payload;
    ip->ihl_version = 0x45; ip->tos = 0; ip->tot_len = htons(sizeof(ipv4_header) + sizeof(icmp_header) + 4); 
    ip->id = htons(0x1234); ip->frag_off = 0; ip->ttl = 64; ip->protocol = 1; ip->check = 0; 
    ip->saddr[0] = 10; ip->saddr[1] = 0; ip->saddr[2] = 2; ip->saddr[3] = 15; 
    for (int i = 0; i < 4; i++) ip->daddr[i] = dst_ip[i];
    ip->check = calculate_checksum((void*)ip, sizeof(ipv4_header));

    volatile icmp_header* icmp = (volatile icmp_header*)(eth->payload + sizeof(ipv4_header));
    icmp->type = 8; icmp->code = 0; icmp->id = htons(0x1337); icmp->sequence = htons(1); icmp->checksum = 0;
    char* data = (char*)icmp + sizeof(icmp_header); data[0] = 'P'; data[1] = 'O'; data[2] = 'N'; data[3] = 'G';
    icmp->checksum = calculate_checksum((void*)icmp, sizeof(icmp_header) + 4);

    sys_puts(root_ep, "[NET DRIVER] Sending ICMP Ping (Echo Request) to Router...\n");
    net_send_packet(sizeof(virtio_net_hdr) + 14 + sizeof(ipv4_header) + sizeof(icmp_header) + 4);
}

// НОВОЕ: Функция отправки текста через UDP!
static void net_send_udp(seL4_CPtr root_ep, const uint8_t dst_ip[4], uint16_t dst_port, const char* message) {
    volatile virtio_net_hdr* net_hdr = (volatile virtio_net_hdr*)(0x502000 + 0x280);
    net_hdr->flags = 0; net_hdr->gso_type = 0; net_hdr->hdr_len = 0; net_hdr->gso_size = 0; net_hdr->csum_start = 0; net_hdr->csum_offset = 0;
    
    volatile ethernet_frame* eth = (volatile ethernet_frame*)(0x502000 + 0x280 + sizeof(virtio_net_hdr));
    for(int i=0; i<6; i++) eth->dest_mac[i] = router_mac[i];
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
    ip->saddr[0] = 10; ip->saddr[1] = 0; ip->saddr[2] = 2; ip->saddr[3] = 15; 
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

    sys_puts(root_ep, "[NET DRIVER] Firing UDP Datagram to ");
    put_ip(root_ep, dst_ip);
    sys_puts(root_ep, ":");
    put_dec(root_ep, dst_port);
    sys_puts(root_ep, "...\n");
    net_send_packet(sizeof(virtio_net_hdr) + 14 + sizeof(ipv4_header) + sizeof(udp_header) + msg_len);
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

static void net_handle_command(seL4_CPtr root_ep, seL4_CPtr net_cmd_ep) {
    if (net_cmd_ep == 0) return;

    seL4_Word sender_badge = 0;
    seL4_MessageInfo_t info = seL4_NBRecv(net_cmd_ep, &sender_badge);
    if (sender_badge == 0) return;

    int len = seL4_MessageInfo_get_length(info);
    if (len == 0) return;

    seL4_Word cmd = seL4_GetMR(0);

    if (cmd == NET_CMD_PING && len >= 3) {
        uint8_t dst_ip[4];
        unpack_ipv4(seL4_GetMR(1), dst_ip);

        sys_puts(root_ep, "[NET DRIVER] Shell requested ICMP Ping.\n");
        if (have_router_mac) {
            net_send_ping(root_ep, dst_ip);
        } else {
            for (int i = 0; i < 4; i++) pending_ip[i] = dst_ip[i];
            pending_cmd = NET_CMD_PING;
            net_send_arp_request(root_ep);
        }
    } else if (cmd == NET_CMD_SEND && len >= 4) {
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

        sys_puts(root_ep, "[NET DRIVER] Shell requested UDP send.\n");
        if (have_router_mac) {
            net_send_udp(root_ep, dst_ip, dst_port, msg);
        } else {
            for (int i = 0; i < 4; i++) pending_udp_ip[i] = dst_ip[i];
            pending_udp_port = dst_port;
            my_memcpy(pending_udp, msg, my_strlen(msg) + 1);
            pending_cmd = NET_CMD_SEND;
            net_send_arp_request(root_ep);
        }
    } else if (cmd == NET_CMD_STATUS) {
        sys_puts(root_ep, "[NET DRIVER] Status: virtio=up router_mac=");
        if (have_router_mac) {
            sys_puts(root_ep, "known ");
            for (int i = 0; i < 6; i++) {
                if (i > 0) sys_puts(root_ep, ":");
                put_hex_byte(root_ep, router_mac[i]);
            }
        } else {
            sys_puts(root_ep, "unknown");
        }
        sys_puts(root_ep, " tx_idx=");
        put_dec(root_ep, g_tx_avail_idx);
        sys_puts(root_ep, " rx_used_idx=");
        put_dec(root_ep, g_last_rx_used_idx);
        sys_puts(root_ep, " default_udp=");
        put_ip(root_ep, default_udp_ip);
        sys_puts(root_ep, ":");
        put_dec(root_ep, default_udp_port);
        sys_puts(root_ep, "\n");
    } else {
        sys_puts(root_ep, "[NET DRIVER] Unknown Shell command.\n");
    }
}

// ==========================================
// Чтение пакетов
// ==========================================
static void net_poll(seL4_CPtr root_ep) {
    volatile virtq_avail_rx* rx_avail = (volatile virtq_avail_rx*)(0x502000 + 0x2040);
    volatile virtq_used_rx* rx_used = (volatile virtq_used_rx*)(0x502000 + 0x2080);

    while (g_last_rx_used_idx != rx_used->idx) {
        uint16_t used_ring_idx = g_last_rx_used_idx % 4;
        uint32_t desc_id = rx_used->ring[used_ring_idx].id;
        volatile ethernet_frame* eth = (volatile ethernet_frame*)(0x502000 + rx_buffer_offsets[desc_id] + sizeof(virtio_net_hdr));

        uint16_t type = htons(eth->ethertype);

        if (type == 0x0806) { 
            volatile arp_ipv4* arp_reply = (volatile arp_ipv4*)eth->payload;
            if (htons(arp_reply->oper) == 2 && !have_router_mac) { 
                sys_puts(root_ep, ">>> [NET RX] ARP Reply Received! Saving Router MAC.\n");
                for(int i=0; i<6; i++) router_mac[i] = arp_reply->sha[i];
                have_router_mac = true;

                if (pending_cmd == NET_CMD_PING) {
                    net_send_ping(root_ep, pending_ip);
                    pending_cmd = NET_CMD_NONE;
                } else if (pending_cmd == NET_CMD_SEND) {
                    net_send_udp(root_ep, pending_udp_ip, pending_udp_port, pending_udp);
                    pending_cmd = NET_CMD_NONE;
                }
            }
        } 
        else if (type == 0x0800) { 
            volatile ipv4_header* ip = (volatile ipv4_header*)eth->payload;
            if (ip->protocol == 1) { 
                volatile icmp_header* icmp = (volatile icmp_header*)(eth->payload + (ip->ihl_version & 0x0F) * 4);
                if (icmp->type == 0) { 
                    sys_puts(root_ep, "\n===============================================\n");
                    sys_puts(root_ep, "   >>> SUCCESS! PING REPLY RECEIVED! <<< \n");
                    sys_puts(root_ep, "===============================================\n\n");
                    
                    // Reply is logged only; user-space shell decides what to send next.
                }
            }
        }

        rx_avail->ring[g_rx_avail_idx % 4] = desc_id;
        g_rx_avail_idx++;
        rx_avail->idx = g_rx_avail_idx;
        g_net_regs->queue_notify = 0; 
        g_last_rx_used_idx++;
    }
}

extern "C" void __sel4_start_c(void) {
    seL4_Word fake_tls_base = 0x501800; asm volatile("msr tpidr_el0, %0" :: "r"(fake_tls_base));
    __sel4_ipc_buffer = (seL4_IPCBuffer*)0x501000; seL4_CPtr root_ep = __sel4_ipc_buffer->userData;
    seL4_CPtr net_cmd_ep = __sel4_ipc_buffer->caps_or_badges[2];

    sys_puts(root_ep, "\n[NET DRIVER] Network Server Online!\n");
    uintptr_t base_addr = 0;
    for (int i = 0; i < 32; i++) {
        uintptr_t slot_addr = 0x200004000ULL + (i * 0x200);
        volatile VirtioMmioRegs* regs = (volatile VirtioMmioRegs*)slot_addr;
        if (regs->magic_value == 0x74726976 && regs->device_id == 1) { g_net_regs = regs; base_addr = slot_addr; break; }
    }

    if (g_net_regs) {
        g_net_regs->status = 0; g_net_regs->status |= 1; g_net_regs->status |= 2; 
        volatile uint8_t* config_space = (volatile uint8_t*)(base_addr + 0x100);
        for (int i = 0; i < 6; i++) my_mac[i] = config_space[i];

        g_net_regs->guest_page_size = 64; 
        g_net_regs->queue_sel = 0; g_net_regs->queue_num = 4; g_net_regs->queue_align = 64; g_net_regs->queue_pfn = (0x60000000 + 0x2000) / 64; 
        volatile virtq_desc* rx_desc = (volatile virtq_desc*)(0x502000 + 0x2000);
        volatile virtq_avail_rx* rx_avail = (volatile virtq_avail_rx*)(0x502000 + 0x2040);
        for (int i = 0; i < 4; i++) {
            rx_desc[i].addr = 0x60000000 + rx_buffer_offsets[i]; rx_desc[i].len = 1536; rx_desc[i].flags = 2; rx_desc[i].next = 0;
            rx_avail->ring[g_rx_avail_idx % 4] = i; g_rx_avail_idx++;
        }
        rx_avail->idx = g_rx_avail_idx;

        g_net_regs->queue_sel = 1; g_net_regs->queue_num = 2; g_net_regs->queue_align = 64; g_net_regs->queue_pfn = (0x60000000 + 0x200) / 64; 
        g_net_regs->status |= 8; g_net_regs->status |= 4; 
        sys_puts(root_ep, "[NET DRIVER] Virtio-Net TX & RX Queues Initialized. Waiting for Shell commands.\n");
        g_net_regs->queue_notify = 0; 
        for(volatile int i=0; i<10000000; i++); 
    }

    while(1) {
        if (g_net_regs) {
            net_poll(root_ep);
            net_handle_command(root_ep, net_cmd_ep);
        }
        seL4_Yield();
    }
}
