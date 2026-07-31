#include "h/sys_client.h"

// Фаза A (см. ROADMAP.md): вынесено из shell.cpp (был SYS_PS=104 напрямую в
// диспетчере команд) — та же логика, теперь отдельный /sbin/ps.elf.
int main(int argc, char *argv[]) {
    SysClientEnv env;
    sys_client_init(env);

    vfs_lock();
    seL4_SetMR(0, 104); // SYS_PS
    seL4_Call(env.root_ep, seL4_MessageInfo_new(0, 0, 0, 1));
    sys_puts(0, env.shm);
    vfs_unlock();
    sys_exit(env.root_ep);
    return 0;
}
