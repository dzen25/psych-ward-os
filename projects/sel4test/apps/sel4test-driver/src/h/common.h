#pragma once
#include <stdio.h>
#include <stdint.h>

extern "C" {
#include <sel4/sel4.h>
}

void __assert_fail(const char *assertion, const char *file, int line, const char *function);

enum BootIPCSlot {
    BOOT_CONSOLE_EP = 100,
    BOOT_TIMER_EP   = 101,
    BOOT_NET_EP     = 102,
    BOOT_ROOT_EP    = 103,
    BOOT_IRQ_EP     = 104,
    BOOT_WIFI_EP    = 105, // wifi_driver слушает команды шелла (см. ROADMAP.md Фаза 4)
    BOOT_MBOX_BUF_PADDR = 106, // физический адрес приватного буфера timer_driver под VideoCore mailbox (Фаза 4.6, см. ROADMAP.md/main.cpp)
    BOOT_HEARTBEAT_NTFN_CAP = 107, // капа, которой timer_driver сигналит net_driver'у периодический будильник (Фаза 4.5, см. NET_EVENT_HEARTBEAT ниже)
    BOOT_BLK_DMA_PADDR = 108, // физический адрес приватного некэшируемого DMA bounce-буфера blk_driver (Фаза 4.5/ADMA2, см. PLAT_BLK_DMA_VADDR/main.cpp)
    // Фаза 4.5 (Wi-Fi data-plane, см. ROADMAP.md/situation.txt) — три новых
    // слота под тот же принцип, что и BOOT_HEARTBEAT_NTFN_CAP выше, только
    // для новых межпроцессных нотификаций net_driver<->wifi_driver:
    BOOT_WIFI_HEARTBEAT_NTFN_CAP = 111, // капа, которой timer_driver сигналит wifi_driver'у периодический опрос SDIO data-канала (читает ТОЛЬКО wifi_driver, см. WIFI_EVENT_HEARTBEAT ниже)
    BOOT_WIFI_NET_RX_SIGNAL_CAP  = 112, // капа (badge NET_EVENT_WIFI_RX, минтится из net_event_ntfn), которой wifi_driver сигналит net_driver'у "кадр в RX-mailbox" — читает ТОЛЬКО wifi_driver, передаётся заново при каждом "wifi start" (см. wifi_cmd_recv_ep)
    BOOT_WIFI_TX_WAKE_CAP        = 113, // капа (badge WIFI_EVENT_TX_READY, минтится из wifi_wake_ntfn), которой net_driver сигналит wifi_driver'у "кадр в TX-mailbox" — читает ТОЛЬКО net_driver
    // Фикс живого зависания blk_driver (см. situation.txt): blk_driver
    // блокировался на seL4_Wait без таймаута, ожидая EMMC-прерывание — если
    // карта его пропустит, зависает навсегда, а вместе с ним и root (см.
    // load_elf_from_disk() — синхронный вызов из обработчика root'а). Тот
    // же принцип, что и BOOT_WIFI_HEARTBEAT_NTFN_CAP выше — timer_driver
    // периодически сигналит доп. badged-копию ТОГО ЖЕ notification-объекта,
    // на котором blk_driver уже блокируется (g_emmc_irq_ntfn), так что
    // seL4_Wait гарантированно просыпается каждые ~100мс независимо от
    // реального железного IRQ.
    BOOT_BLK_HEARTBEAT_NTFN_CAP  = 114,
    // Фикс дедлока (см. situation.txt): notify_root_irq_handled() раньше
    // звал root СИНХРОННЫМ seL4_Call (SYS_MMC_IRQ_ACK), чтобы root вызвал
    // seL4_IRQHandler_Ack() — но если ИМЕННО root является текущим
    // синхронным вызывающим blk_driver (SYS_EXEC -> load_elf_from_disk()),
    // root уже заблокирован в ожидании ЭТОГО САМОГО ответа, и обратный
    // вызов к нему мертво блокируется (root ждёт blk_driver, blk_driver
    // ждёт root). blk_driver получает СОБСТВЕННУЮ копию capability на
    // IRQHandler и вызывает Ack() сам, без какого-либо IPC к root —
    // безопасно, т.к. к этому моменту девайсный статус-бит уже снят (см.
    // emmc_wait_irpt_bit).
    BOOT_MMC_IRQ_HANDLER_CAP     = 115,
    // Фикс задержки (см. situation.txt): переход read/write на multi-block
    // (CMD18/25) — физический адрес ВТОРОЙ приватной страницы blk_driver'а
    // (только под ADMA2-дескриптор; данные больше не помещаются рядом с ним
    // в одной странице, см. main.cpp/blk_dma_frame2_param).
    BOOT_BLK_DMA2_PADDR          = 116,
    // Фаза 6 (SMP, см. ROADMAP.md): общий межпроцессный мьютекс на
    // нотификации для VFS-прокси staging области в SHM (офсет 4096) —
    // заменяет старые non-atomic busy-spin локи (vfs_lock/net_vfs_lock/
    // wifi_vfs_lock), которые были безопасны только на одном ядре. Капа
    // читается shell/net_driver/wifi_driver — тот же самый объект у всех
    // троих, без бейджа (чистый mutex, различать источник не нужно).
    BOOT_VFS_MUTEX_NTFN_CAP      = 117,
    // Фаза 6.1 (продолжение, см. ROADMAP.md): собственная копия TCB-капы
    // процесса на самого себя — нужна uart/blk/net/wifi (is_driver
    // 1/3/4/5), чтобы при просьбе root'а "финализируй бенчмарк и отдай
    // свой total/idle" вызвать seL4_BenchmarkGetThreadUtilisation(себя) —
    // единственный способ получить ЧЕСТНЫЙ total/idle СВОЕГО ядра (это поле
    // в ядре всегда берётся от ВЫЗЫВАЮЩЕГО, не от того, чей TCB спрашивают
    // — root, вызывая с ядра 0, иначе всегда получал бы период ядра 0).
    BOOT_SELF_TCB_CAP            = 118,
};

// Общий IRQ 158 (EMMC2 + Wi-Fi SDIO — одна физическая GIC-линия на обоих
// контроллерах, см. platform.h/ROADMAP.md 4.5) слушает САМ root, а не
// какой-то конкретный драйвер — только один процесс вообще может держать
// IRQHandler-капу на этот номер. ПРОБОВАЛИ (см. situation.txt) TCB-bind
// напрямую к blk_driver, чтобы обойти задержку root-инициированных чтений
// (root не крутит свой Recv-цикл, пока сам синхронно ждёт ответа от
// blk_driver, и не может вовремя отрелеить реальное прерывание) — ОТКАЧЕНО:
// вызвало катастрофический регресс (чтение WiFi-прошивки: 800мс -> 27с),
// т.к. blk_driver копит пендинг-сигналы от каждого реального завершения
// EMMC-команды, пока сам занят обработкой (не в Recv/Wait), и следующий
// seL4_Recv в его главном цикле перехватывается этим накопленным сигналом
// вместо реального клиентского запроса. Схема снова: root релеит ОБОИМ
// процессам (blk_irq_ntfn/wifi_irq_ntfn), каждый сам проверяет свой
// статусный регистр. Badge заведомо вне диапазонов PID (1-255) и пайпов
// (1000-1015), чтобы не путаться с обычными сообщениями в главном цикле root'а.
constexpr seL4_Word IRQ_MMC_SHARED_BADGE = 2000;

// Бейдж badged-копии blk_irq_ntfn, которую держит timer_driver (см.
// BOOT_BLK_HEARTBEAT_NTFN_CAP выше) — конкретное значение некритично,
// blk_driver не различает бейджи на этой нотификации (перечитывает
// статусный регистр на любое пробуждение независимо от причины), но
// именованная константа лучше магического числа в main.cpp.
constexpr seL4_Word BLK_HEARTBEAT_BADGE = 0x8000;

// Фаза 4.5 (событийный GENET RX + периодический будильник для net_driver,
// см. ROADMAP.md) — биты нотификации net_driver'а. Оба минтятся из ОДНОГО
// net_event_ntfn (см. main.cpp) разными источниками (GENET IRQHandler и
// timer_driver соответственно) — seL4 при накоплении неснятых сигналов на
// одном объекте OR'ит бейджи, поэтому оба бита должны быть непересекающимися
// степенями двойки и заведомо вне диапазона обычных PID (1-255) и пайпов
// (1000-1015, см. PIPE_BASE_BADGE в main.cpp), чтобы не спутать событие с
// настоящим клиентским сообщением на net_cmd_ep.
constexpr seL4_Word NET_EVENT_GENET_RX  = 0x1000; // пришёл кадр (см. RPI4_GENET_IRQ_A/UMAC_IRQ_RXDMA_DONE)
constexpr seL4_Word NET_EVENT_HEARTBEAT = 0x2000; // периодическая проверка DHCP/ARP/ping/link-таймаутов (см. timer_driver.cpp)
// Фаза 4.5 (Wi-Fi data-plane) — третий бейдж того же net_event_ntfn: пришёл
// кадр от wifi_driver (см. WIFI_SHM_RX_LEN_OFFSET в net_driver.cpp), сигналит
// сам wifi_driver через badge NET_EVENT_WIFI_RX (BOOT_WIFI_NET_RX_SIGNAL_CAP).
constexpr seL4_Word NET_EVENT_WIFI_RX   = 0x4000;

// Фаза 4.5 (Wi-Fi data-plane) — биты СОБСТВЕННОЙ нотификации wifi_driver'а
// (wifi_wake_ntfn в main.cpp), тот же принцип OR'а бейджей одного объекта,
// что и у NET_EVENT_* выше, но в отдельном (не пересекающемся с net_driver)
// пространстве — эти два badge'а сравниваются только между собой и с
// PID/пайп-бейджами my_ep САМОГО wifi_driver, никогда не с NET_EVENT_*.
constexpr seL4_Word WIFI_EVENT_HEARTBEAT = 0x1000; // периодический опрос SDIO data-канала на входящие 802.11-кадры (см. BOOT_WIFI_HEARTBEAT_NTFN_CAP)
constexpr seL4_Word WIFI_EVENT_TX_READY  = 0x2000; // net_driver положил кадр в TX-mailbox — разбудить сразу, не ждать heartbeat (см. BOOT_WIFI_TX_WAKE_CAP)

// Слот CSpace процесса, в который ядро минтит capability активного пайпа
// (см. SYS_PIPE/SYS_PIPE_CLOSE в main.cpp и запрос пайпа в shell.cpp).
// Должен отличаться от зарезервированных local_* слотов в main.cpp::spawn_process
// (console=1, timer=2, net_send=3, irq=4, net_recv=5, wifi_send=6, blk=7,
// wifi_recv=8, self_cnode=11, syscall=10) — раньше здесь был захардкожен слот 3,
// что уничтожало net_send_ep при закрытии пайпа.
constexpr seL4_Word PIPE_FD_SLOT = 20;

// Слот CSpace процесса, в который main.cpp минтит capability на СОБСТВЕННЫЙ
// (child) CNode процесса — не капа на какой-то endpoint внутри него, а на сам
// CNode целиком. Нужна для seL4_CNode_SaveCaller()/seL4_CNode_Delete() ВНУТРИ
// самого процесса, когда он хочет отложить IPC-reply вместо немедленного
// ответа (см. uart_driver.cpp SYS_READ — Фаза 4.5, ROADMAP.md; тот же приём,
// что main.cpp уже использует для СЕБЯ через seL4_CapInitThreadCNode).
constexpr seL4_Word SELF_CNODE_SLOT = 11;

// Сисколл rootserver'у: "моя синхронная инициализация завершена, я готов
// обслуживать запросы". Каждый драйвер шлет его один раз перед входом в
// свой главный while(1); rootserver ждет этот сигнал между spawn_process()
// соседних драйверов (см. main.cpp), поэтому порядок готовности определяется
// самим порядком вызовов spawn_process(), а не отдельным списком где-то еще.
// Специально НЕ используется для "лучше-стараться" фоновых задач (например,
// первого NTP-синка в net_driver) — иначе загрузка могла бы зависнуть
// навсегда при недоступной сети.
constexpr seL4_Word SYS_DRIVER_READY = 109;

// Сисколл шелла: "заблокируй меня, пока не готовы ВСЕ драйверы (см.
// SYS_DRIVER_READY выше)". Шелл шлет его один раз при старте, до печати
// собственного баннера/приглашения — так его "sandbox[N] />" оказывается
// в логе строго после логов инициализации остальных модулей, без ручной
// синхронизации порядка где-либо еще.
constexpr seL4_Word SYS_WAIT_ALL_DRIVERS_READY = 110;

// wifi_driver больше не запускается при загрузке (см. ROADMAP.md/main.cpp —
// подозрение на гонку мапинга/таймингов при одновременном спавне с
// остальными драйверами, из-за которой изредка не успевал возникать
// готовый sdpcm-канал). Теперь его жизненным циклом управляет шелл через
// команду "wifi start/stop/restart" — три новых сисколла ниже, по образцу
// SYS_EXEC/SYS_KILL/SYS_RECOVER, но специфичные для wifi_driver (не требуют
// передачи имени процесса, рутсервер и так знает, кого спавнить/убивать).
constexpr seL4_Word SYS_START_WIFI  = 130; // спавнит wifi_driver, если ещё не запущен
constexpr seL4_Word SYS_STOP_WIFI   = 131; // убивает wifi_driver БЕЗ автореспавна
constexpr seL4_Word SYS_WIFI_STATUS = 132; // MR0: 0=не запущен, 1=запущен но не готов, 2=готов принимать команды

// Фаза 4.5 (см. ROADMAP.md): blk_driver зовёт это ПОСЛЕ того, как реально
// сбросил статусный бит в EMMC_INTERRUPT (device-level), а НЕ root сразу по
// получении сигнала. Общий IRQ 158 (EMMC2/Wi-Fi SDIO) level-triggered — GIC
// не доставит его ЗАНОВО, пока не будет явный seL4_IRQHandler_Ack; если бы
// root Ack'ал сразу (как раньше), а девайсный бит ещё не сброшен (blk_driver
// просто ещё не успел выполниться), GIC увидел бы линию всё ещё asserted и
// мгновенно повторил бы доставку — root (priority 255, выше любого ребёнка)
// зацикливался бы на "разбудили -> Ack -> тут же снова разбудили", ни разу
// не дав планировщику дойти до blk_driver (priority 254), который единственный
// может реально снять бит. Поэтому Ack откладывается до этого явного вызова.
constexpr seL4_Word SYS_MMC_IRQ_ACK = 133;

// Фаза 4.5 (см. ROADMAP.md) — тот же приём, что SYS_MMC_IRQ_ACK выше, но со
// стороны wifi_driver: общий IRQ 158 (EMMC2/Wi-Fi SDIO) теперь будит ОБА
// процесса на каждое срабатывание (root не знает заранее, чей это статус-
// бит — см. main.cpp), и root не Ack'ает GIC, пока КАЖДАЯ сторона, у которой
// реально был выставлен свой бит, не подтвердит, что сняла его. wifi_driver
// зовёт это ПОСЛЕ того, как реально сбросил I_HMB_FRAME_IND/I_HMB_HOST_INT в
// SDPCMD_INTSTATUS чипа (см. sdpcm_wait_and_read_ctrl в wifi_driver.cpp) —
// если у него самого бит не был выставлен (проснулся по чужому, EMMC2-
// событию), звонить сюда не нужно вообще, blk_driver сам вызовет
// SYS_MMC_IRQ_ACK для своей части.
constexpr seL4_Word SYS_WIFI_IRQ_ACK = 134;

// Фаза 5.4 (least-privilege, см. situation.txt/ROADMAP.md Фаза 5): узкий
// файловый доступ для exec-процессов (is_driver=254), которые с Фазы 5.2
// не получают ни одной страницы SHM — root выполняет операцию от их имени
// (тем же приёмом, что уже использует load_elf_from_disk(): собственный
// scratch в rootserver_shm_base + seL4_Call к blk_driver), путь и данные
// приходят прямо в MR, никакого SHM у вызывающего не требуется.
// MR1.. = путь (до 63 байт, нуль-терминированный, упакован по байту на слово
// — простота важнее плотности, это редкий/маленький вызов).
constexpr seL4_Word SYS_PROXY_READ_FILE  = 135; // MR1..=путь; ответ: MR0=статус(0=ok), MR1=длина, MR2..=данные (до ~100 байт за вызов)
constexpr seL4_Word SYS_PROXY_WRITE_FILE = 136; // MR1=длина данных, MR2..=путь+данные упакованы см. main.cpp; ответ: MR0=статус

// Фаза 6.1 (SMP, см. ROADMAP.md): ручной перенос уже запущенного процесса на
// другое ядро в рантайме (команда шелла `taskset <pid> <ядро>`). MR1=pid,
// MR2=целевое ядро (0..3). Ответ: MR0=статус (0=ok, иначе код отказа — см.
// таблицу в main.cpp: root/timer_driver нельзя переносить никогда, см.
// найденную PPI-опасность в ROADMAP Фазы 6.1).
constexpr seL4_Word SYS_SET_AFFINITY = 137;
// Разовый снимок нагрузки (команда шелла `top`) — без параметров. Ответ:
// MR0=статус; текстовая таблица (общая нагрузка по ядрам + PID/NAME/CORE/%CPU)
// пишется в rootserver_shm_base, тем же путём, что SYS_PS.
constexpr seL4_Word SYS_TOP_STATS = 138;

// Фаза 6.1 (продолжение): "вызови у себя seL4_BenchmarkResetLog() и ответь" —
// root не может включить учёт benchmark utilisation НА ДРУГОМ ядре сам
// (per-core состояние в ядре seL4), поэтому просит представителя (любой
// активный uart/blk/net/wifi процесс на этом ядре) сделать это самому.
// Общая для uart_driver/blk_driver (разбирают этот SYS_*-неймспейс
// напрямую по числу) — net_driver/wifi_driver используют собственные
// NetCommand/WIFI_CMD_* и получают свою локальную константу того же
// назначения (см. net_driver.cpp/wifi_driver.cpp). Без параметров, ответ:
// MR0=0.
constexpr seL4_Word SYS_BENCHMARK_RESET_LOCAL = 139;

// Фаза 6.1 (продолжение): "вызови у себя seL4_BenchmarkFinalizeLog(), затем
// seL4_BenchmarkGetThreadUtilisation(себя) и отдай МОЙ idle/total" —
// вызывается ПОСЛЕ сна (300мс), парой к SYS_BENCHMARK_RESET_LOCAL выше.
// Без этого root, читая total/idle через СВОЙ (ядро 0) вызов
// GetThreadUtilisation, получал бы total С ЯДРА 0 (это поле в ядре всегда
// от вызывающего, не от того, чей TCB спрашивают) — а не честный период
// того ядра, где реально исполняется представитель (найдено по факту:
// агрегат по ядру 1 совсем не сходился с суммой процессов на нём). Нужна
// собственная TCB-капа (см. BOOT_SELF_TCB_CAP) — без параметров, ответ:
// MR0=idle (BENCHMARK_IDLE_LOCALCPU_UTILISATION), MR1=total
// (BENCHMARK_TOTAL_UTILISATION), оба — 4 и 9 в
// benchmark_track_util_ipc_index.
constexpr seL4_Word SYS_BENCHMARK_FINALIZE_LOCAL = 140;

// Фаза 6.1 (продолжение): команда шелла `balance` — без параметров. Снимает
// ту же нагрузку, что и `top` (collect_load_snapshot()), находит САМОЕ
// занятое ядро с >=2 резидентами, оставляет на нём "тяжёлый" (максимальный
// %CPU) процесс, а всех остальных резидентов (кроме root/timer_driver —
// те же две защиты, что у SYS_SET_AFFINITY) раскидывает по наименее
// загруженным другим ядрам. Ответ: MR0=0, текстовый отчёт — в
// rootserver_shm_base, тем же путём, что SYS_TOP_STATS/SYS_PS.
constexpr seL4_Word SYS_BALANCE = 141;

const char* sel4_err_str(seL4_Error err);
void check_err(seL4_Error err, const char *msg);