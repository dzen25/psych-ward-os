#include "h/sys_client.h"

// logtest — проверка накопителя в режиме бортового самописца / журнала.
//
// ЗАЧЕМ. Отдельный вопрос от «файл читается/пишется»: выдержит ли система
// ТЫСЯЧИ мелких записей подряд, не деградируя и не портя данные. Именно так
// ведёт себя журнал или самописец — много коротких записей в один и тот же
// файл, годами.
//
// ЧЕТЫРЕ НЕЗАВИСИМЫЕ ОСИ (порядок аргументов после пути произвольный):
//
//   rewrite | append   ЧТО делаем с файлом.
//     rewrite — каждая итерация перезаписывает файл ЦЕЛИКОМ
//               (SYS_WRITE_FILE, cmd 113). Размер постоянный, потолок
//               4096 байт на вызов (одна staging-страница SHM драйвера,
//               см. BLK_SHM_STAGING_OFFSET в platform.h). Это износ
//               одного и того же места носителя.
//     append  — каждая итерация ДОПИСЫВАЕТ в конец (SYS_APPEND_FILE,
//               cmd 121). Файл растёт, потолка на размер файла нет.
//
//   same | random      ЧТО пишем.
//     same    — одна и та же строка на каждой итерации (по умолчанию).
//               Ровно так ведёт себя самописец, перезаписывающий слот.
//     random  — псевдослучайное содержимое, своё на каждую итерацию
//               (зерно фиксированное, ожидаемое пересчитывается).
//     ВАЖНО, ЧТО ЭТО ЗНАЧИТ ДЛЯ ПРОВЕРКИ: при `same` сверка чтением НЕ
//     способна отличить «записалось верно» от «прочитались старые данные
//     с прошлой итерации» — содержимое-то одинаковое. Поэтому `same`
//     проверяет живучесть пути записи, а `random` — ещё и то, что данные
//     реально доехали. Для настоящей уверенности гонять оба.
//
//   nostream | stream  КАК меряем.
//     nostream — после каждой записи сразу чтение и сверка (по умолчанию).
//                Даёт честную задержку одной ПОДТВЕРЖДЁННОЙ записи.
//     stream   — пишем подряд, без посверки; сверка одна, в конце. Даёт
//                пропускную способность. VFS-мьютекс по-прежнему берётся
//                на каждый вызов — иначе пришлось бы держать глобальную
//                блокировку весь прогон и подвесить остальную систему;
//                разница между режимами именно в проверке, а не в
//                блокировке, и это честно, а не «почти потоково».
//
//   <число> <число>    Первое — итераций (по умолчанию 1500),
//                      второе — байт за итерацию (по умолчанию 256).
//
// ИСТОРИЯ: настоящего append в системе не было до 2026-09-06 — единственной
// записью была перезаписывающая файл целиком. Он добавлен вместе с этим
// тестом (exfat_append_file(), cmd 121 в blk_driver/usb_driver).
//
// Запуск:
//   exec /sbin/tests/logtest.elf /mnt/Mass-Storage_Device
//   exec /sbin/tests/logtest.elf /mnt/Mass-Storage_Device 1500 rewrite same
//   exec /sbin/tests/logtest.elf /mnt/Mass-Storage_Device 5000 append stream 512
//   exec /sbin/tests/logtest.elf /mnt/Mass-Storage_Device 1500 append random
// Путь — каталог тома (файл будет <путь>/logtest.log) либо сразу имя файла,
// если в последнем сегменте есть точка.

static SysClientEnv env;

constexpr int VFS_WRITE_MAX = (int)VFS_PAYLOAD_MAX; // потолок одного вызова, см. platform.h
constexpr int PATH_OFFSET   = 0;
constexpr int DATA_OFFSET   = (int)VFS_PAYLOAD_OFFSET; // путь — в странице 0, содержимое — в области нагрузки

static char g_path[256];
// aligned(8): и подготовка эталона, и сверка ходят по SHM 8-байтными
// словами (SHM — Device-память, побайтный доступ к ней стоит на порядок
// дороже), а невыровненное слово там даёт Alignment Fault.
static char g_expect[VFS_WRITE_MAX + 1] __attribute__((aligned(8)));
static seL4_CPtr g_ep = 0;

static int  g_chunk = 256;
static bool g_same = true;      // одна и та же строка (см. шапку)
static bool g_stream = false;   // без посверки каждой итерации
static bool g_delete = false;   // удалить файл после прогона
// Потоковая запись: открыли один раз, зарезервировали место, дальше пишем
// прямо. Отличие от append не в объёме данных, а в том, что НЕ делается на
// каждую запись: не проверяется размер экстента и не перезаписывается запись
// каталога (мелкая запись по далёкому адресу — самое дорогое для флеша).
// Длина файла попадает в каталог один раз, при закрытии.
static bool g_direct = false;
static bool g_append = false;

// Постоянная строка для режима `same`. Печатная и узнаваемая — файл потом
// можно просто открыть `cat`-ом и глазами убедиться, что там ожидаемое.
static const char SAME_PATTERN[] = "PSYCH-WARD-OS LOGTEST RECORD 0123456789 ";
constexpr int SAME_PATTERN_LEN = (int)(sizeof(SAME_PATTERN) - 1);

static void putdec(long long val) {
    char buf[24]; int j = 0;
    if (val < 0) { sys_puts(0, "-"); val = -val; }
    if (val == 0) buf[j++] = '0';
    while (val > 0) { buf[j++] = (char)('0' + (val % 10)); val /= 10; }
    while (j > 0) { char c[2] = {buf[--j], 0}; sys_puts(0, c); }
}

static seL4_Word now_ms() {
    seL4_SetMR(0, 3); // SYS_GET_TIME
    seL4_Call(env.timer_ep, seL4_MessageInfo_new(0, 0, 0, 1));
    return seL4_GetMR(0);
}

// Микросекундные часы — прямое чтение системного счётчика, без IPC.
//
// Зачем: now_ms() имеет разрешение 1 мс И стоит полного IPC к
// timer_driver. Одна запись в 128 КБ занимает ~0.5 мс, то есть КАЖДЫЙ
// замер округлялся до 0 или 1 мс, а сумма двухсот таких округлений — уже
// не измерение. На живом прогоне это дало слой [3] ВЫШЕ слоя [2], чего
// физически быть не может: под ним лежит тот же обмен плюс накладные
// расходы. Плюс сам IPC (два на итерацию) попадал внутрь измеряемого
// интервала. CNTVCT_EL0/CNTFRQ_EL0 читаются из пользовательского режима
// (так же делает usb_driver), частота берётся один раз.
static uint64_t g_cntfrq = 0;
static inline uint64_t now_us() {
    uint64_t v;
    if (g_cntfrq == 0) { asm volatile("mrs %0, cntfrq_el0" : "=r"(g_cntfrq)); }
    asm volatile("isb; mrs %0, cntvct_el0" : "=r"(v));
    return g_cntfrq ? (v * 1000000ull / g_cntfrq) : 0;
}

// --- содержимое записи ---
// Псевдослучайность с фиксированным зерном (LCG): важна не «качественная»
// случайность, а воспроизводимость — ожидаемое содержимое пересчитывается,
// а не хранится, поэтому проверка не упирается в память.
static uint64_t g_rng = 0;
static void rng_seed(uint64_t s) { g_rng = s * 6364136223846793005ULL + 1442695040888963407ULL; }
static char rng_char() {
    g_rng = g_rng * 6364136223846793005ULL + 1442695040888963407ULL;
    // 0x21..0x7E — печатные без пробела и без нуля, чтобы сверять как строку
    return (char)(0x21 + (int)((g_rng >> 33) % (0x7E - 0x21 + 1)));
}

// Заполняет ОДНОВРЕМЕННО эталон и буфер SHM.
//
// Раньше запись готовилась в g_expect, а потом побайтово копировалась в
// SHM ВНУТРИ замеряемого участка. На 128 КБ за итерацию это давало ~670 мс
// из 725 и полностью маскировало реальную скорость: замер 2026-09-06 на
// SuperSpeed-накопителе показал фазу данных 45 мс, а всё остальное время
// уходило на наше же перекладывание байтов по некэшируемой памяти.
//
// Настоящему журналу промежуточный буфер не нужен вовсе — данные сразу
// формируются там, откуда их заберёт драйвер. Эталон рядом нужен только
// тесту, для сверки.
static void fill_record(long record) {
    char *dst = env.shm + DATA_OFFSET;
    if (g_same) {
        for (int i = 0; i < g_chunk; i++) { char c = SAME_PATTERN[i % SAME_PATTERN_LEN]; g_expect[i] = c; dst[i] = c; }
    } else {
        rng_seed((uint64_t)(record + 1));
        for (int i = 0; i < g_chunk; i++) { char c = rng_char(); g_expect[i] = c; dst[i] = c; }
    }
    g_expect[g_chunk] = '\0'; // у эталона есть запасной байт (VFS_WRITE_MAX + 1)
    // В SHM терминатор НЕ пишем: область нагрузки ровно VFS_PAYLOAD_MAX
    // байт, и запись такого же размера занимает её целиком — лишний байт
    // уходил в соседнюю страницу, которой этой роли не выдано (hw
    // 2026-09-06: FATAL FAULT по адресу конца области). Драйверу
    // терминатор и не нужен, длину он берёт из регистра сообщения.
}

// --- VFS ---

// Разбор round-trip'а дописывания на три части. Появился потому, что
// замер клиента оказался вдвое больше времени внутри exfat_append_file, а
// чтение через ту же обёртку показывает разницу в 30 мкс — то есть дело не
// в IPC как таковом, и надо не гадать, а разделить.
static uint64_t g_us_lock = 0, g_us_call = 0, g_us_unlock = 0;

static int vfs_put(const char *data, int len, bool append) {
    // Данные уже лежат в SHM — их положил туда fill_record(). Копирования
    // здесь больше нет намеренно, см. комментарий у fill_record.
    (void)data;
    uint64_t t = now_us();
    my_strlcpy(env.shm + PATH_OFFSET, g_path, 128);
    vfs_lock();
    g_us_lock += now_us() - t;
    t = now_us();
    seL4_SetMR(0, append ? 121 : 113); // SYS_APPEND_FILE / SYS_WRITE_FILE
    seL4_SetMR(1, (seL4_Word)len);
    seL4_Call(g_ep, seL4_MessageInfo_new(0, 0, 0, 2));
    int rc = (int)seL4_GetMR(0);
    g_us_call += now_us() - t;
    t = now_us();
    vfs_unlock();
    g_us_unlock += now_us() - t;
    return rc;
}

// Кусок файла по смещению (cmd 119). Возвращает число прочитанных байт
// (0 = конец файла) или -1. Данные — в начале SHM.
static int vfs_read_at(uint32_t offset) {
    my_strlcpy(env.shm + PATH_OFFSET, g_path, 128);
    vfs_lock();
    seL4_SetMR(0, 119); // SYS_READ_FILE
    seL4_SetMR(1, (seL4_Word)offset);
    seL4_Call(g_ep, seL4_MessageInfo_new(0, 0, 0, 2));
    int rc = (int)seL4_GetMR(0);
    int n = (int)seL4_GetMR(1);
    vfs_unlock();
    if (rc != 0) return -1;
    return n;
}

static int vfs_rm(void) {
    my_strlcpy(env.shm + PATH_OFFSET, g_path, 128);
    vfs_lock();
    seL4_SetMR(0, 120); // SYS_RM
    seL4_Call(g_ep, seL4_MessageInfo_new(0, 0, 0, 1));
    int rc = (int)seL4_GetMR(0);
    vfs_unlock();
    return rc;
}

// --- потоковая запись (cmd 122/123/124, см. h/exfat.h) ---
static int vfs_stream_open(uint64_t reserve) {
    my_strlcpy(env.shm + PATH_OFFSET, g_path, 128);
    vfs_lock();
    seL4_SetMR(0, 122);
    seL4_SetMR(1, (seL4_Word)reserve);
    seL4_Call(g_ep, seL4_MessageInfo_new(0, 0, 0, 2));
    int rc = (int)seL4_GetMR(0);
    vfs_unlock();
    return rc;
}
static int vfs_stream_write(const char *data, int len) {
    (void)data; // данные уже в SHM, см. fill_record
    // Путь переписывается ПЕРЕД КАЖДЫМ вызовом, как и во всех остальных
    // VFS-обёртках: resolve_device_by_path() в usb_driver срезает имя тома
    // ПРЯМО В БУФЕРЕ, поэтому оставшийся от прошлого вызова путь уже не
    // маршрутизируется. Без этой строки закрытие потока возвращало -1
    // (отказ маршрутизации), а не код собственного обработчика.
    my_strlcpy(env.shm + PATH_OFFSET, g_path, 128);
    vfs_lock();
    seL4_SetMR(0, 123);
    seL4_SetMR(1, (seL4_Word)len);
    seL4_Call(g_ep, seL4_MessageInfo_new(0, 0, 0, 2));
    int rc = (int)seL4_GetMR(0);
    vfs_unlock();
    return rc;
}
static int vfs_stream_close(void) {
    my_strlcpy(env.shm + PATH_OFFSET, g_path, 128); // см. комментарий в vfs_stream_write
    vfs_lock();
    seL4_SetMR(0, 124);
    seL4_Call(g_ep, seL4_MessageInfo_new(0, 0, 0, 1));
    int rc = (int)seL4_GetMR(0);
    vfs_unlock();
    return rc;
}

// --- статистика по фазам SCSI (только usb_driver, cmd 125/126) ---
static void scsi_stats_reset(void) {
    vfs_lock();
    seL4_SetMR(0, 125);
    seL4_Call(g_ep, seL4_MessageInfo_new(0, 0, 0, 1));
    vfs_unlock();
}
static const char *IO_TAG_NAME[] = {
    "прочее", "запись потока", "поиск в битмапе", "разметка битмапа",
    "обход каталога", "запись записи каталога", "подсчёт свободного",
    "чтение экстента", "FAT"
};

// Разбивка блочных операций по вызывающему — отвечает на вопрос "кто именно
// генерирует трафик", а не "сколько его всего".
static void scsi_tags_print(void) {
    my_strlcpy(env.shm + PATH_OFFSET, g_path, 128);
    vfs_lock();
    seL4_SetMR(0, 127);
    seL4_MessageInfo_t r127 = seL4_Call(g_ep, seL4_MessageInfo_new(0, 0, 0, 1));
    // Драйвер мог не знать этой команды: тогда ответ короткий, а регистры
    // сообщения содержат что угодно от прошлого обмена. Раньше тест печатал
    // их как настоящие цифры — на SD выходило "всего кластеров 187" и
    // "команд 5245624" вперемешку с осмысленными числами.
    if (seL4_MessageInfo_get_length(r127) < 3 * 9 + 7) {
        vfs_unlock();
        sys_puts(0, "  --- разбивка блочных операций недоступна: драйвер тома не ведёт эту статистику ---\n");
        return;
    }
    long long c[9], b[9], u[9];
    for (int t = 0; t < 9; t++) { c[t] = (long long)seL4_GetMR(2*t); b[t] = (long long)seL4_GetMR(2*t+1); }
    long long cpy = (long long)seL4_GetMR(18);
    long long slw = (long long)seL4_GetMR(19);
    long long apc = (long long)seL4_GetMR(20);
    long long stw = (long long)seL4_GetMR(21);
    for (int t = 0; t < 9; t++) u[t] = (long long)seL4_GetMR(22 + t);
    long long ap_us = (long long)seL4_GetMR(31);
    long long ap_n  = (long long)seL4_GetMR(32);
    long long h_us  = (long long)seL4_GetMR(33);
    vfs_unlock();
    sys_puts(0, "  --- вызовы ---\n");
    sys_puts(0, "  copy_extent: "); putdec(cpy);
    sys_puts(0, ", медленный путь append: "); putdec(slw);
    sys_puts(0, ", append всего: "); putdec(apc);
    sys_puts(0, ", stream_write: "); putdec(stw); sys_puts(0, "\n");
    sys_puts(0, "  --- кто делает блочные операции ---\n");
    long long us_all = 0;
    for (int t = 0; t < 9; t++) us_all += u[t];
    for (int t = 0; t < 9; t++) {
        if (c[t] == 0) continue;
        sys_puts(0, "  "); sys_puts(0, IO_TAG_NAME[t]);
        sys_puts(0, ": команд "); putdec(c[t]);
        sys_puts(0, ", байт "); putdec(b[t]);
        // Время, а не только количество: команда на 512 байт и команда на
        // 128 КБ стоят разного, и по счётчикам команд нельзя понять, что
        // именно съедает время. Доля — сразу видно, за что браться.
        sys_puts(0, ", мкс "); putdec(u[t]);
        if (us_all > 0) { sys_puts(0, " ("); putdec(u[t] * 100 / us_all); sys_puts(0, "%)"); }
        sys_puts(0, "\n");
    }
    sys_puts(0, "  ИТОГО в блочных операциях, мкс: "); putdec(us_all); sys_puts(0, "\n");
    // Три числа рядом отвечают на вопрос "что оптимизировать дальше":
    // блочные операции -> сколько ждёт диск, exfat_append_file -> плюс CPU
    // самой ФС, замер клиента -> плюс IPC и маршрутизация VFS.
    if (ap_n > 0) {
        sys_puts(0, "  --- где время дописывания, сверху вниз (мкс на "); putdec(ap_n); sys_puts(0, " вызовов) ---\n");
        sys_puts(0, "  клиент: путь+vfs_lock "); putdec((long long)(g_us_lock));
        sys_puts(0, ", seL4_Call "); putdec((long long)(g_us_call));
        sys_puts(0, ", vfs_unlock "); putdec((long long)(g_us_unlock)); sys_puts(0, "\n");
        sys_puts(0, "  драйвер: весь обработчик cmd 121 "); putdec(h_us);
        sys_puts(0, ", из него exfat_append_file "); putdec(ap_us); sys_puts(0, "\n");
    }
}

// Печатает скорость СЛОЯМИ, снизу вверх. Раньше здесь было пять разных
// чисел вперемешку ("ЧТЕНИЕ", "ЗАПИСЬ", "крупными кусками", "в фазе
// данных", "скорость записи"), и по ним нельзя было понять, куда
// смотреть: они меряют РАЗНОЕ и обязаны отличаться. Теперь у каждого
// числа назван свой слой, а строкой ниже сказано, какое из них
// характеризует что.
//
// e2e_bytes/e2e_us — сквозные данные вызывающего (сколько байт он записал
// и сколько микросекунд это заняло по его часам): без них верхний слой
// пришлось бы печатать в другом месте, и сравнивать снова было бы не с чем.
static void scsi_stats_print(long long e2e_bytes, long long e2e_us) {
    vfs_lock();
    seL4_SetMR(0, 126);
    seL4_MessageInfo_t r126 = seL4_Call(g_ep, seL4_MessageInfo_new(0, 0, 0, 1));
    if (seL4_MessageInfo_get_length(r126) < 13) {
        vfs_unlock();
        // У SD нет ни CBW, ни CSW — это не SCSI-транспорт, и слои [1]/[2]
        // для него не определены. Печатаем то единственное, что измерено
        // честно: сквозную скорость по часам самого вызывающего.
        sys_puts(0, "\n  --- СКОРОСТЬ ---\n        слои [1] шина и [2] SCSI-команда для этого тома не определены\n");
        sys_puts(0, "        (драйвер не SCSI: у SD нет фаз CBW/CSW)\n");
        if (e2e_us > 0) {
            sys_puts(0, "        [3] вызов программы   ");
            putdec(e2e_bytes * 1000000LL / e2e_us / 1024); sys_puts(0, " КБ/с   (");
            putdec(e2e_us / 1000); sys_puts(0, " мс)\n");
        }
        return;
    }
    long long cmds = (long long)seL4_GetMR(0);
    long long cbw  = (long long)seL4_GetMR(1);
    long long dat  = (long long)seL4_GetMR(2);
    long long csw  = (long long)seL4_GetMR(3);
    long long byt  = (long long)seL4_GetMR(4);
    long long rd   = (long long)seL4_GetMR(5);
    long long rdb  = (long long)seL4_GetMR(6);
    long long wr   = (long long)seL4_GetMR(7);
    long long wrb  = (long long)seL4_GetMR(8);
    long long rmax = (long long)seL4_GetMR(9);
    long long wmax = (long long)seL4_GetMR(10);
    long long lim  = (long long)seL4_GetMR(11);
    long long sfc  = (long long)seL4_GetMR(12);
    long long bmc  = (long long)seL4_GetMR(13);
    long long rtc  = (long long)seL4_GetMR(14);
    long long ccnt = (long long)seL4_GetMR(15);
    long long drd  = (long long)seL4_GetMR(16);
    long long dwr  = (long long)seL4_GetMR(17);
    long long bru  = (long long)seL4_GetMR(18);
    long long brb  = (long long)seL4_GetMR(19);
    vfs_unlock();
    if (cmds <= 0) return; // не USB-том (blk_driver команду не знает) — молча пропускаем
    sys_puts(0, "  --- фазы SCSI ---\n");
    sys_puts(0, "  команд:      "); putdec(cmds);
    sys_puts(0, ", байт в фазе данных: "); putdec(byt); sys_puts(0, "\n");
    sys_puts(0, "  CBW  (31 Б):  "); putdec(cbw / 1000); sys_puts(0, " мс, на команду "); putdec(cbw / cmds); sys_puts(0, " мкс\n");
    sys_puts(0, "  данные:       "); putdec(dat / 1000); sys_puts(0, " мс, на команду "); putdec(dat / cmds); sys_puts(0, " мкс\n");
    sys_puts(0, "  CSW  (13 Б):  "); putdec(csw / 1000); sys_puts(0, " мс, на команду "); putdec(csw / cmds); sys_puts(0, " мкс\n");
    sys_puts(0, "  чтений:      "); putdec(rd); sys_puts(0, ", байт "); putdec(rdb); sys_puts(0, "\n");
    sys_puts(0, "  записей:     "); putdec(wr); sys_puts(0, ", байт "); putdec(wrb);
    sys_puts(0, ", макс секторов "); putdec(wmax); sys_puts(0, "\n");
    sys_puts(0, "  макс секторов в чтении: "); putdec(rmax);
    sys_puts(0, ", объявленный лимит тома: "); putdec(lim); sys_puts(0, "\n");
    sys_puts(0, "  экстент потока с кластера "); putdec(sfc);
    sys_puts(0, "; битмап "); putdec(bmc);
    sys_puts(0, ", корень "); putdec(rtc);
    sys_puts(0, ", всего кластеров "); putdec(ccnt); sys_puts(0, "\n");

    sys_puts(0, "\n  --- СКОРОСТЬ ПО СЛОЯМ (снизу вверх) ---\n");

    sys_puts(0, "  [1] ШИНА — только фаза данных, без Command/Sense IU:\n");
    if (dwr > 0) {
        sys_puts(0, "        запись            "); putdec(wrb * 1000000LL / dwr / 1024);
        sys_puts(0, " КБ/с   ("); putdec(dwr / 1000); sys_puts(0, " мс на "); putdec(wrb);
        sys_puts(0, " Б, команд "); putdec(wr); sys_puts(0, ")\n");
    }
    if (bru > 0) {
        sys_puts(0, "        чтение крупное    "); putdec(brb * 1000000LL / bru / 1024);
        sys_puts(0, " КБ/с   (куски от 16 КБ, "); putdec(brb); sys_puts(0, " Б)\n");
    }
    if (drd > 0) {
        long long small_us = drd - bru, small_b = rdb - brb;
        if (small_us > 0 && small_b > 0) {
            sys_puts(0, "        чтение мелкое     "); putdec(small_b * 1000000LL / small_us / 1024);
            sys_puts(0, " КБ/с   (метаданные; здесь важна не скорость, а ЧИСЛО команд)\n");
        }
    }

    // Накладные расходы транспорта делим между чтением и записью по доле
    // команд: отдельного разбиения Command/Sense IU по направлению драйвер
    // не ведёт, а пропорция честнее, чем приписать всё одному из них.
    if (dwr > 0 && cmds > 0 && wr > 0) {
        long long ovh = (cbw + csw) * wr / cmds;      // мкс накладных расходов, приходящихся на записи
        long long full = dwr + ovh;
        sys_puts(0, "  [2] SCSI-КОМАНДА целиком — шина плюс Command IU и Sense IU:\n");
        sys_puts(0, "        запись            "); putdec(wrb * 1000000LL / full / 1024);
        sys_puts(0, " КБ/с   (накладные "); putdec(ovh / wr); sys_puts(0, " мкс на команду:");
        sys_puts(0, " Command IU "); putdec(cbw / cmds);
        sys_puts(0, " + Sense IU "); putdec(csw / cmds); sys_puts(0, ")\n");
    }

    if (e2e_us > 0 && e2e_bytes > 0) {
        sys_puts(0, "  [3] ВЫЗОВ ПРОГРАММЫ — VFS + exFAT + IPC поверх слоя 2:\n");
        sys_puts(0, "        запись            "); putdec(e2e_bytes * 1000000LL / e2e_us / 1024);
        sys_puts(0, " КБ/с   ("); putdec(e2e_us / 1000); sys_puts(0, " мс)   <<< ЭТО и видит приложение\n");
    }

    sys_puts(0, "  Куда смотреть: [3] — реальная скорость программы; [1] — потолок\n");
    sys_puts(0, "  железа; разрыв между ними и есть цена файловой системы.\n");
}

// Чистый отчёт по ЧТЕНИЮ. Отдельно от scsi_stats_print(), потому что тот
// про запись, а сравнивать чтение с записью по общей строке нельзя: в
// общую корзину "крупных чтений" попадали куски и по 128 КБ (сверка), и
// по 64 КБ (копирование экстента), тогда как записи все по 128 КБ. Здесь
// замер однородный: один проход по файлу кусками одного размера.
static void read_stats_print(long long bytes, long long us) {
    vfs_lock();
    seL4_SetMR(0, 126);
    seL4_MessageInfo_t rr = seL4_Call(g_ep, seL4_MessageInfo_new(0, 0, 0, 1));
    bool have_bus = seL4_MessageInfo_get_length(rr) >= 17; // см. проверку в scsi_stats_print
    long long cmds = (long long)seL4_GetMR(0);
    long long rd   = (long long)seL4_GetMR(5);
    long long rdb  = (long long)seL4_GetMR(6);
    long long rmax = (long long)seL4_GetMR(9);
    long long drd  = (long long)seL4_GetMR(16);
    vfs_unlock();
    (void)cmds;
    sys_puts(0, "  --- ЧТЕНИЕ, чистый замер (тот же объём, БЕЗ сравнения байтов) ---\n");
    sys_puts(0, "        прочитано         "); putdec(bytes);
    sys_puts(0, " Б за "); putdec(us / 1000); sys_puts(0, " мс\n");
    if (us > 0) {
        sys_puts(0, "        [3] программа     "); putdec(bytes * 1000000LL / us / 1024);
        sys_puts(0, " КБ/с\n");
    }
    if (have_bus && drd > 0 && rdb > 0) {
        sys_puts(0, "        [1] шина          "); putdec(rdb * 1000000LL / drd / 1024);
        sys_puts(0, " КБ/с   (команд "); putdec(rd);
        sys_puts(0, ", макс "); putdec(rmax); sys_puts(0, " секторов)\n");
    }
}

static int vfs_touch(void) {
    my_strlcpy(env.shm + PATH_OFFSET, g_path, 128);
    vfs_lock();
    seL4_SetMR(0, 112); // SYS_TOUCH
    seL4_Call(g_ep, seL4_MessageInfo_new(0, 0, 0, 1));
    int rc = (int)seL4_GetMR(0);
    vfs_unlock();
    return rc;
}

// Аварийный выход с уборкой.
//
// Зачем отдельно: прогон может упасть на любом шаге, а файл к этому моменту
// уже занимает место — в потоковом режиме сразу ВЕСЬ зарезервированный
// объём. Без уборки следующий запуск не находит непрерывного участка и
// падает уже на открытии (наступали на это: 12.5 МБ висели после
// неудачного закрытия, и повторный прогон открыться не смог).
static void cleanup_and_exit(void) {
    if (g_delete) {
        if (vfs_rm() == 0) sys_puts(0, "  файл удалён (уборка после ошибки)\n");
        else               sys_puts(0, "  ВНИМАНИЕ: удалить файл не удалось — место осталось занято\n");
    } else {
        sys_puts(0, "  файл оставлен на носителе (без флага delete). Если место кончится — rm вручную.\n");
    }
    sys_exit(env.root_ep);
}

// Полная сверка файла от нуля по смещениям. Возвращает true, если всё на
// месте; при расхождении печатает, ГДЕ именно, и возвращает false.
// Один и тот же код обслуживает оба режима: в rewrite ожидаемая длина —
// одна запись, в append — все.
// fixed_record >= 0 — весь файл считать содержимым ЭТОЙ записи (режим
// rewrite: там лежит результат последней итерации, а не первой). -1 —
// запись определяется смещением (режим append).
static bool verify_whole_file(long long expect_len, long fixed_record) {
    long long off = 0;
    while (off < expect_len) {
        int n = vfs_read_at((uint32_t)off);
        if (n < 0) { sys_puts(0, "  чтение по смещению "); putdec(off); sys_puts(0, " не удалось\n"); return false; }
        if (n == 0) break;
        for (int i = 0; i < n; i++) {
            long long abs_pos = off + i;
            if (abs_pos >= expect_len) break;
            long which = (fixed_record >= 0) ? fixed_record : (long)(abs_pos / g_chunk);
            int inside  = (int)(abs_pos % g_chunk);
            char expect;
            if (g_same) {
                expect = SAME_PATTERN[inside % SAME_PATTERN_LEN];
            } else {
                // Генератор переинициализируется на границе записи; если
                // прочитанный кусок начался ПОСРЕДИ записи — отматываем.
                if (inside == 0) rng_seed((uint64_t)(which + 1));
                else if (i == 0) { rng_seed((uint64_t)(which + 1)); for (int k = 0; k < inside; k++) rng_char(); }
                expect = rng_char();
            }
            if (env.shm[DATA_OFFSET + i] != expect) {
                sys_puts(0, "  расхождение на байте "); putdec(abs_pos);
                sys_puts(0, " (запись № "); putdec(which + 1);
                sys_puts(0, ", смещение внутри записи "); putdec(inside); sys_puts(0, ")\n");
                return false;
            }
        }
        off += n;
    }
    if (off < expect_len) {
        sys_puts(0, "  файл кончился на "); putdec(off); sys_puts(0, " Б вместо "); putdec(expect_len); sys_puts(0, "\n");
        return false;
    }
    return true;
}

// --- каркас шагов ---
static int g_step = 0, g_passed = 0;
static void step_begin(int n, const char *title) {
    g_step = n;
    sys_puts(0, "\nТЕСТ "); putdec(g_step); sys_puts(0, ": "); sys_puts(0, title); sys_puts(0, "\n");
}
static void step_ok() { g_passed++; sys_puts(0, "ТЕСТ "); putdec(g_step); sys_puts(0, " ЗАВЕРШЁН\n"); }
static void step_fail(const char *why) {
    sys_puts(0, "ТЕСТ "); putdec(g_step); sys_puts(0, " ОСТАНОВЛЕН — ОШИБКА: "); sys_puts(0, why); sys_puts(0, "\n");
}

static bool tok_is(const char *t, const char *word) {
    int i = 0;
    while (t[i] && word[i] && t[i] == word[i]) i++;
    return t[i] == '\0' && word[i] == '\0';
}

int main(int argc, char *argv[]) {
    sys_client_init(env);
    if (!env.shm) { sys_puts(0, "logtest: нет доступа к SHM — запускать через exec.\n"); cleanup_and_exit(); return 1; }

    char where[192]; where[0] = '\0';
    long iterations = 1500;

    if (env.arg && *env.arg) {
        char buf[256]; my_strlcpy(buf, env.arg, sizeof(buf));
        char *cur = buf;
        int numbers_seen = 0;
        for (;;) {
            char *t = next_token(&cur);
            if (!t || !*t) break;
            if (is_all_digits(t)) {
                int v = simple_atoi(t);
                if (numbers_seen == 0) { if (v > 0) iterations = v; }
                else if (numbers_seen == 1) { if (v > 0) g_chunk = v; }
                numbers_seen++;
            }
            else if (tok_is(t, "append"))   g_append = true;
            else if (tok_is(t, "rewrite"))  g_append = false;
            else if (tok_is(t, "same"))     g_same = true;
            else if (tok_is(t, "random"))   g_same = false;
            else if (tok_is(t, "stream"))   g_stream = true;
            else if (tok_is(t, "nostream")) g_stream = false;
            else if (tok_is(t, "direct"))   { g_direct = true; g_append = true; g_stream = true; }
            else if (tok_is(t, "delete"))   g_delete = true;
            else if (tok_is(t, "keep"))     g_delete = false;
            else if (!where[0])             my_strlcpy(where, t, sizeof(where));
            else {
                sys_puts(0, "logtest: непонятный аргумент '"); sys_puts(0, t); sys_puts(0, "'\n");
                sys_puts(0, "  ожидаются: <путь> [итераций] [байт] [rewrite|append] [same|random] [stream|nostream] [keep|delete]\n");
                cleanup_and_exit(); return 1;
            }
        }
    }

    if (!where[0]) {
        sys_puts(0, "logtest: не задан путь к накопителю.\n");
        sys_puts(0, "  exec /sbin/tests/logtest.elf <путь> [итераций] [байт] [rewrite|append] [same|random] [stream|nostream] [keep|delete]\n");
        cleanup_and_exit(); return 1;
    }
    if (g_chunk > VFS_WRITE_MAX) g_chunk = VFS_WRITE_MAX;

    // Путь: если в последнем сегменте нет точки — считаем его каталогом
    // тома и дописываем имя файла сами.
    {
        int n = (int)my_strlen(where);
        const char *last = where;
        for (int i = 0; i < n; i++) if (where[i] == '/') last = where + i + 1;
        bool has_dot = false;
        for (const char *p = last; *p; p++) if (*p == '.') has_dot = true;
        char joined[256];
        my_strlcpy(joined, where, sizeof(joined));
        if (!has_dot) {
            int j = (int)my_strlen(joined);
            if (j > 0 && joined[j - 1] != '/') { joined[j++] = '/'; joined[j] = '\0'; }
            my_strlcpy(joined + my_strlen(joined), "logtest.log", sizeof(joined) - my_strlen(joined));
        }
        build_absolute_path(env.shm, joined, 128);
    }
    // ВАЖЕН ПОРЯДОК: route_vfs_path() правит буфер НА МЕСТЕ — срезает
    // ведущий "/mnt", потому что драйвер тома ожидает путь уже без него.
    // Вызывать её надо на самом env.shm и только ПОТОМ забирать результат
    // (иначе драйверу уедет несрезанный путь и любая операция вернёт
    // "не найдено" — на этом первый прогон и споткнулся).
    g_ep = route_vfs_path(env.shm, env.blk_ep, env.usb_storage_ep);
    my_strlcpy(g_path, env.shm, sizeof(g_path));

    sys_puts(0, "==========================================================\n");
    sys_puts(0, "logtest — накопитель в режиме журнала/самописца\n");
    sys_puts(0, "файл:     "); sys_puts(0, g_path); sys_puts(0, "\n");
    sys_puts(0, "режим:    "); sys_puts(0, g_append ? "append (дописывание, файл растёт)"
                                                    : "rewrite (перезапись файла целиком)");
    sys_puts(0, "\nданные:   "); sys_puts(0, g_same ? "same (одна и та же строка)"
                                                    : "random (своё содержимое на каждой итерации)");
    if (g_direct) {
        sys_puts(0, "\nзапись:   direct (поток: открыли, зарезервировали, пишем прямо)");
    }
    sys_puts(0, "\nсверка:   "); sys_puts(0, g_stream ? "stream (только в конце — меряем пропускную способность)"
                                                      : "nostream (после каждой записи — меряем задержку)");
    sys_puts(0, "\nитераций: "); putdec(iterations);
    sys_puts(0, ", байт за итерацию: "); putdec(g_chunk);
    sys_puts(0, "\nпосле:    "); sys_puts(0, g_delete ? "delete (файл будет удалён)" : "keep (файл останется на носителе)");
    sys_puts(0, "\nитоговый размер файла: ");
    putdec(g_append ? (long long)iterations * g_chunk : (long long)g_chunk);
    sys_puts(0, " Б\n");
    if (g_same && !g_stream) {
        sys_puts(0, "ПРИМЕЧАНИЕ: при `same` посверка не отличит новую запись от\n");
        sys_puts(0, "  прошлой — содержимое одинаковое. Для проверки, что данные\n");
        sys_puts(0, "  реально доехали, прогоните ещё раз с `random`.\n");
    }
    sys_puts(0, "==========================================================\n");

    // ТЕСТ 1 — файл создаётся и обнуляется
    step_begin(1, "файл создан и обнулён — прогон начинается с чистого листа");
    if (vfs_touch() < 0) { step_fail("не удалось создать файл — проверьте путь и что том смонтирован"), cleanup_and_exit(); return 1; }
    if (vfs_put(g_expect, 0, false) != 0) { step_fail("не удалось обнулить файл"), cleanup_and_exit(); return 1; }
    sys_puts(0, "  файл готов, длина 0\n");
    step_ok();

    // ТЕСТ 2 — основной цикл
    step_begin(2, g_stream ? "поток записей без посверки"
                           : "цикл записей с посверкой каждой итерации");
    long done = 0;
    long long total_bytes = 0, file_len = 0;
    uint64_t us_write = 0, us_read = 0; // микросекунды: см. now_us()
    // Время, которое тест тратит НА СЕБЯ — подготовку эталона в SHM и
    // побайтную сверку прочитанного. Раньше оно не попадало ни в один
    // счётчик, и итог выглядел загадкой: цикл шёл 5188 мс, а запись плюс
    // посверка объясняли из них 682 мс. Остальное съедала работа с SHM:
    // это Device-память, и 26 МБ туда-обратно по одному байту стоят
    // дороже, чем весь дисковый ввод-вывод прогона.
    uint64_t us_shm = 0;
    {
        if (g_direct) {
            // Резервируем сразу под весь прогон: расширять экстент по ходу
            // значило бы вернуть ту самую работу, ради ухода от которой
            // режим и заводился.
            int oc = vfs_stream_open((uint64_t)iterations * g_chunk);
            if (oc != 0) {
                sys_puts(0, "  код ошибки открытия: "); putdec(oc);
                sys_puts(0, " (-1 открытие не удалось, -4 поток уже открыт)\n");
                step_fail("не удалось открыть поток (нет непрерывного места нужного размера?)");
                cleanup_and_exit(); return 1;
            }
            sys_puts(0, "  поток открыт, зарезервировано "); putdec((long long)iterations * g_chunk); sys_puts(0, " Б\n");
        }
        scsi_stats_reset(); // мерим только основной цикл, без подготовки
        seL4_Word t_all = now_ms();
        for (long it = 1; it <= iterations; it++) {
            uint64_t t_shm = now_us();
            fill_record(it - 1);
            us_shm += now_us() - t_shm;

            uint64_t t0 = now_us();
            // В потоковом режиме запись идёт через SYS_STREAM_WRITE, а не
            // через обычное дописывание. Эта развилка отсутствовала: замер
            // показал stream_write=0 при append=200, то есть режим `direct`
            // печатался в баннере, но фактически не использовался.
            int rc = g_direct ? vfs_stream_write(g_expect, g_chunk)
                              : vfs_put(g_expect, g_chunk, g_append);
            us_write += now_us() - t0;
            if (rc != 0) {
                sys_puts(0, "  итерация "); putdec(it); sys_puts(0, ": запись вернула "); putdec(rc); sys_puts(0, "\n");
                step_fail(g_append ? "дописывание перестало проходить" : "запись перестала проходить");
                cleanup_and_exit(); return 1;
            }

            if (!g_stream) {
                // Читаем ровно ту область, куда только что писали: в append
                // это хвост по смещению (файл уже может быть большим и
                // перечитывать его целиком значило бы мерить проверку, а не
                // запись), в rewrite — начало файла.
                t0 = now_us();
                int got = vfs_read_at((uint32_t)(g_append ? file_len : 0));
                us_read += now_us() - t0;
                if (got < 0) {
                    sys_puts(0, "  итерация "); putdec(it); sys_puts(0, ": чтение не удалось\n");
                    step_fail("файл перестал читаться");
                    cleanup_and_exit(); return 1;
                }
                if (got != g_chunk) {
                    sys_puts(0, "  итерация "); putdec(it);
                    sys_puts(0, ": прочитано "); putdec(got); sys_puts(0, " Б вместо "); putdec(g_chunk); sys_puts(0, "\n");
                    step_fail("длина не совпала — записалось не то количество");
                    cleanup_and_exit(); return 1;
                }
                int bad = -1;
                // Прочитанное приходит в ту же область полезной нагрузки,
                // куда мы пишем (DATA_OFFSET), а не в начало SHM.
                // Сверка идёт 8-байтными словами: SHM — Device-память,
                // побайтный проход по 128 КБ стоил больше, чем сама
                // запись на диск, и при этом не попадал ни в один замер.
                t_shm = now_us();
                {
                    const uint64_t *a64 = (const uint64_t *)(env.shm + DATA_OFFSET);
                    const uint64_t *b64 = (const uint64_t *)g_expect;
                    int words = g_chunk / 8;
                    for (int w = 0; w < words; w++) {
                        if (a64[w] != b64[w]) { // разошлось — сузить до байта уже дёшево
                            for (int i = w * 8; i < w * 8 + 8; i++)
                                if (env.shm[DATA_OFFSET + i] != g_expect[i]) { bad = i; break; }
                            break;
                        }
                    }
                    if (bad < 0)
                        for (int i = words * 8; i < g_chunk; i++) // хвост, если длина не кратна 8
                            if (env.shm[DATA_OFFSET + i] != g_expect[i]) { bad = i; break; }
                }
                us_shm += now_us() - t_shm;
                if (bad >= 0) {
                    sys_puts(0, "  итерация "); putdec(it);
                    sys_puts(0, ": расхождение по смещению "); putdec(bad); sys_puts(0, " внутри записи\n");
                    step_fail("содержимое разошлось с записанным");
                    cleanup_and_exit(); return 1;
                }
            }

            if (g_append) file_len += g_chunk; else file_len = g_chunk;
            total_bytes += g_chunk;
            done = it;
            if (it % 100 == 0) {
                sys_puts(0, "  итерация "); putdec(it); sys_puts(0, "/"); putdec(iterations);
                sys_puts(0, " — файл "); putdec(file_len);
                sys_puts(0, " Б, прошло "); putdec((long long)(now_ms() - t_all)); sys_puts(0, " мс\n");
            }
        }
        if (g_direct) {
            seL4_Word t0 = now_ms();
            int cc = vfs_stream_close();
            if (cc != 0) {
                sys_puts(0, "  код ошибки закрытия: "); putdec(cc);
                sys_puts(0, " (-3 сброс длины не удался, -4 поток не открыт)\n");
                step_fail("не удалось закрыть поток");
                cleanup_and_exit(); return 1;
            }
            sys_puts(0, "  поток закрыт (длина записана в каталог), "); putdec((long long)(now_ms() - t0)); sys_puts(0, " мс\n");
        }
        scsi_stats_print(total_bytes, (long long)us_write);
        scsi_tags_print();
        step_ok();
    }

    // ТЕСТ 3 — целостность всего файла
    step_begin(3, "весь файл перечитан от начала и совпал");
    {
        // В rewrite файл содержит результат ПОСЛЕДНЕЙ итерации (done-1),
        // а не первой — первый прогон на железе поймал ровно это
        // расхождение в режиме `random`; при `same` оно не проявлялось,
        // потому что все записи одинаковые.
        // Сперва ЧИСТЫЙ замер чтения: тот же файл целиком, кусками
        // одного размера и без побайтового сравнения. Сравнение идёт
        // следом, отдельным проходом — иначе в замер попало бы время
        // сравнения 26 МБ в НЕкэшируемой SHM, а это десятки миллисекунд
        // на мегабайт и к скорости чтения отношения не имеет.
        {
            scsi_stats_reset();
            uint64_t t_rd = now_us();
            long long rd_bytes = 0;
            for (long long off = 0; off < file_len; ) {
                int got = vfs_read_at((uint32_t)off);
                if (got <= 0) break;
                rd_bytes += got; off += got;
            }
            uint64_t rd_us = now_us() - t_rd;
            if (rd_bytes > 0 && rd_us > 0) read_stats_print(rd_bytes, (long long)rd_us);
        }
        if (!verify_whole_file(file_len, g_append ? -1 : (done - 1))) {
            step_fail("итоговое содержимое не совпало с записанным");
            cleanup_and_exit(); return 1;
        }
        sys_puts(0, "  перечитано "); putdec(file_len); sys_puts(0, " Б — всё на своих местах\n");
        step_ok();
    }

    // ТЕСТ 4 — уборка за собой (только если попросили)
    int total_steps = 3;
    if (g_delete) {
        total_steps = 4;
        step_begin(4, "файл удалён с носителя");
        if (vfs_rm() != 0) { step_fail("удаление не удалось — файл остался на носителе"), cleanup_and_exit(); return 1; }
        // Проверяем именно ОТСУТСТВИЕ, а не только код возврата: удаление,
        // сообщившее об успехе и оставившее файл, — самый неприятный
        // возможный исход, потому что следующий прогон стартовал бы с
        // непустого файла и мы бы искали причину не там.
        if (vfs_read_at(0) >= 0) { step_fail("файл всё ещё читается после удаления"), cleanup_and_exit(); return 1; }
        sys_puts(0, "  файл удалён и больше не читается\n");
        step_ok();
    }

    sys_puts(0, "\n==========================================================\n");
    sys_puts(0, "ИТОГ: пройдено шагов "); putdec(g_passed); sys_puts(0, " из "); putdec(total_steps); sys_puts(0, ".\n");
    sys_puts(0, "  итераций выполнено: "); putdec(done); sys_puts(0, " из "); putdec(iterations);
    sys_puts(0, "\n  записано суммарно:  "); putdec(total_bytes); sys_puts(0, " Б\n");
    sys_puts(0, "  время записи:       "); putdec((long long)(us_write / 1000)); sys_puts(0, " мс\n");
    if (!g_stream) { sys_puts(0, "  время посверки:     "); putdec((long long)(us_read / 1000)); sys_puts(0, " мс\n"); }
    sys_puts(0, "  работа теста с SHM: "); putdec((long long)(us_shm / 1000));
    sys_puts(0, " мс (эталон + сверка; к скорости ФС не относится)\n");
    if (done > 0) {
        sys_puts(0, "  на запись:          "); putdec((long long)(us_write / (uint64_t)done)); sys_puts(0, " мкс\n");
        if (!g_stream) {
            sys_puts(0, "  на итерацию:        "); putdec((long long)((us_write + us_read) / (uint64_t)done));
            sys_puts(0, " мкс (запись+сверка)\n");
        }
    }
    if (us_write > 0) {
        sys_puts(0, "  скорость записи:    "); putdec(total_bytes * 1000000LL / (long long)us_write / 1024);
        sys_puts(0, " КБ/с  (слой [3], см. разбор выше)\n");
    }
    sys_puts(0, "==========================================================\n");

    sys_exit(env.root_ep);
    return 0;
}
