#include "h/sys_client.h"

// Фаза 14 (см. ROADMAP.md): тонкая обёртка вокруг SYS_USB_LIST —
// root-опосредованный запрос к usb_driver (тот же принцип, что ps/top
// у SYS_PS/SYS_TOP_STATS), единственная команда этой фазы (bring-up +
// перечисление, без класс-драйверов).

static void put_hex16(uint16_t val) {
    char buf[5];
    for (int i = 0; i < 4; i++) buf[i] = "0123456789abcdef"[(val >> ((3 - i) * 4)) & 0xF];
    buf[4] = '\0';
    sys_puts(0, buf);
}

static void put_dec(uint32_t val) {
    char buf[12]; int i = 11; buf[i--] = '\0';
    if (val == 0) buf[i--] = '0';
    while (val > 0) { buf[i--] = '0' + (val % 10); val /= 10; }
    sys_puts(0, &buf[i + 1]);
}

int main(int argc, char *argv[]) {
    SysClientEnv env;
    sys_client_init(env);

    seL4_SetMR(0, 144); // SYS_USB_LIST
    seL4_Call(env.root_ep, seL4_MessageInfo_new(0, 0, 0, 1));
    seL4_Word found = seL4_GetMR(0);
    seL4_Word vendor = seL4_GetMR(1);
    seL4_Word product = seL4_GetMR(2);
    seL4_Word dclass = seL4_GetMR(3);
    seL4_Word dsub = seL4_GetMR(4);
    seL4_Word dproto = seL4_GetMR(5);

    if (!found) {
        sys_puts(0, "USB: устройств не найдено (или usb_driver выключен — см. RPI4_ENABLE_USB).\n");
    } else {
        sys_puts(0, "USB устройство: ");
        put_hex16((uint16_t)vendor); sys_puts(0, ":"); put_hex16((uint16_t)product);
        sys_puts(0, "  class="); put_dec(dclass);
        sys_puts(0, " subclass="); put_dec(dsub);
        sys_puts(0, " protocol="); put_dec(dproto);
        sys_puts(0, "\n");
    }

    sys_exit(env.root_ep);
    return 0;
}
