# 🧠 Psych Ward OS: True Microkernel Edition

**Psych Ward OS** — это экспериментальная операционная система на базе микроядра **seL4**, реализующая принцип наименьших привилегий, полную изоляцию компонентов и систему автоматического самовосстановления (Self-Healing). В отличие от монолитных систем, здесь даже драйверы устройств и системные службы вынесены в пространство пользователя (User-space), а их падение не приводит к краху всей ОС.

## 🏗 Архитектура: Модульная Изоляция

Проект перешел от монолитного приложения к модульной структуре. Теперь пользовательское окружение разделено на независимые бинарники, каждый из которых работает в собственной песочнице:

1. **UART Driver (`uart_driver.cpp`):** Изолированный сервер терминала. Владеет физическим фреймом ввода-вывода и обрабатывает прерывания клавиатуры.
2. **Timer Driver (`timer_driver.cpp`):** Служба системного времени. Управляет RTC и предоставляет сервис засыпания через IPC.
3. **Block Driver & VFS (`blk_driver.cpp`):** Системный драйвер хранилища. Управляет виртуальной файловой системой (VFS) в RAM и напрямую общается с железом жесткого диска (Virtio MMIO) через настроенные очереди команд (Virtqueue) и DMA.
4. **Network Driver (`net_driver.cpp`):** Изолированный сетевой сервер. Управляет Virtio-Net MMIO, TX/RX virtqueue, ARP, ICMP Echo и UDP-датаграммами.
5. **Shell (`shell.cpp`):** Пользовательская оболочка, обеспечивающая интерфейс управления, порождение новых процессов и отправку сетевых команд драйверу через отдельный IPC Endpoint. Оснащена системой Soft-Crash таймаутов.

**Rootserver (The Doctor)** в `main.cpp` выполняет роль верховного судьи: управляет адресными пространствами, раздает права (Capabilities), мапит физическую память для DMA, координирует системные вызовы и выступает в роли **Универсального Watchdog-менеджера**.

---

## 🚀 Ключевые возможности

* **🛡 Self-Healing OS (Meta-Driven Recovery):** Двойной контур защиты от сбоев (Fault Tolerance). Универсальный Watchdog перехватывает фатальные аппаратные ошибки (Segfaults) системных драйверов и мгновенно пересоздает их "на лету" из сохраненных метаданных. Оболочка (Shell) защищена софт-таймаутами: при зависании драйвера в бесконечном цикле она не виснет сама, а отправляет Ядру сигнал `SYS_RECOVER` для его принудительной ликвидации и перезапуска.
* **User-Space Driver Isolation:** Драйверы работают как обычные процессы. Ошибки в коде драйвера не приводят к панике ядра (Kernel Panic).
* **Virtio & DMA Integration:** Настоящее рукопожатие с «железом» на шине MMIO. Чтение и запись секторов диска напрямую в защищенную Shared Memory без участия процессора.
* **Real FAT32 Support:** Полная поддержка файловой системы FAT32. Парсинг Boot Sector, Root Directory, чтение файлов, обновление метаданных и динамическая аллокация кластеров для создания новых файлов.
* **Virtio-Net User-Space Networking:** Изолированный сетевой драйвер работает с Virtio-Net через MMIO и DMA, принимает команды от Shell по IPC и умеет выполнять ARP, ICMP Ping (с точным программным таймером RTT) и UDP send через QEMU SLIRP.
* **Process Lifecycle (POSIX-like):** Реализованы системные вызовы `SYS_EXEC` (запуск ELF с передачей `argc/argv`), `SYS_WAIT` (ожидание завершения) и `SYS_EXIT` (самозавершение).
* **Demand Paging:** Динамический аллокатор страниц выделяет память на лету при возникновении Page Fault.
* **Zero-Copy Shared Memory Heap:** Процессы могут динамически запрашивать общие физические фреймы памяти (`SYS_SHM_GET`) для мгновенного обмена данными напрямую друг с другом, минуя ядро.
* **Hierarchical VFS 2.0:** Иерархическая файловая система, объединяющая RAM-диск и данные с реального FAT32-образа, с поддержкой абсолютных и относительных путей.
* **I/O Redirection & IPC Pipes:** Поддержка перенаправления вывода в файлы (`>`) и конвейеризации вывода между утилитами оболочки (`| grep`) в User-space.
* **Background Execution & Daemons:** Поддержка запуска независимых фоновых процессов (с помощью символа `&`) и режима `--daemon` для работы сервисов без блокировки TTY.

---

## 🛠 Команды оболочки

Оболочка поддерживает динамический промпт, отображающий текущий PID процесса и рабочую директорию (например, `sandbox[4] /mnt>`).

| Команда | Описание |
| --- | --- |
| `ps` | Список активных процессов с именами и PID |
| `pid` | Вывод PID текущей оболочки |
| `exec <file> [args] [&]` | Запуск процесса с аргументами. `&` в конце запускает процесс в фоне |
| `shm <id> <read/write> [text]` | Запись или чтение межпроцессной разделяемой памяти (Shared Memory) |
| `pwd` | Вывести текущую рабочую директорию (CWD) |
| `ls [path]` | Список файлов и папок в директории (показывает файлы с RAM и диска) |
| `cd <path>` | Смена директории (поддерживаются `..`, `/` и `/mnt`) |
| `mkdir <path>` | Создание дерева директорий в RAM (работает как `mkdir -p`) |
| `touch <file>` | Создание пустого файла |
| `echo <text> [> file]` | Вывод текста в консоль или прозрачная запись/создание файла на FAT32 |
| `cat <file>` | Чтение содержимого файла (в том числе скачивание данных с FAT32 через DMA) |
| `ping <ip> [count]` | Отправка ICMP Echo Request через сетевой драйвер. Авто-таймаут ожидания. |
| `send <text>` | Отправка UDP-датаграммы на хост QEMU SLIRP по умолчанию (`10.0.2.2:8080`) |
| `sendto <ip> <port> <text>` | Отправка UDP-датаграммы на указанный IPv4/порт |
| `netstat` | Печать состояния сетевого драйвера: Virtio, ARP/MAC, TX/RX индексы и статистика RTT |
| `<cmd> \| grep <text>` | Конвейерная фильтрация вывода команд (например, `ps \| grep shell`) |
| `kill <pid>` | Принудительная терминация процесса (убитые системные компоненты автоматически воскрешаются) |
| `crash_shell` | **[TEST]** Имитация фатальной ошибки (Null Pointer Dereference) в Оболочке для тестирования мгновенного воскрешения Ядром |
| `crash_disk` | **[TEST]** Отправка "ядовитой таблетки" дисковому драйверу (симуляция аппаратного сбоя и проверка двойного восстановления Watchdog'а) |
| `exit` | Завершение текущей оболочки |

*(Совет: используйте флаг `--daemon` при фоновом запуске `exec shell --daemon &`, чтобы процесс не пытался читать ввод с клавиатуры).*

---

## 💻 Сборка и запуск

### Зависимости:

Для сборки проекта вам понадобится Linux-окружение (рекомендуется Ubuntu 22.04/24.04). Установите базовые системные пакеты:

```bash
sudo apt update
sudo apt install -y build-essential git cmake ninja-build \
    gcc-aarch64-linux-gnu g++-aarch64-linux-gnu \
    qemu-system-arm python3-venv python3-pip dosfstools \
    netcat-openbsd tcpdump

# Разрешить QEMU (обычному пользователю) отправлять внешние ICMP пинги
sudo sysctl -w net.ipv4.ping_group_range="0 2147483647"

```

### Установка проекта и виртуального окружения:

```bash
# 1. Клонирование репозитория
cd ~
git clone [https://github.com/dzen25/psych-ward-os.git](https://github.com/dzen25/psych-ward-os.git)
cd psych-ward-os

# 2. Создание и активация изолированного виртуального окружения Python
python3 -m venv ~/sel4-vibe
source ~/sel4-vibe/bin/activate

# 3. Установка Python-зависимостей, необходимых для сборки seL4
pip install -r requirements.txt

```

### 💽 Создание образа диска (fat32.img)

Для работы драйвера файловой системы в QEMU требуется заранее подготовленный образ диска.

```bash
cd ~/psych-ward-os/projects/sel4test/apps/sel4test-driver/
dd if=/dev/zero of=fat32.img bs=1M count=16
mkfs.fat -F 32 fat32.img

mkdir -p /tmp/fat32_mount
sudo mount -o loop fat32.img /tmp/fat32_mount
sudo bash -c 'echo "Welcome to FAT32" > /tmp/fat32_mount/HELLO.TXT'
sudo umount /tmp/fat32_mount
rm -rf /tmp/fat32_mount

```

### Компиляция образа:

```bash
mkdir -p build && cd build
../init-build.sh -DPLATFORM=qemu-arm-virt -DAARCH64=1
ninja

```

---

### Запуск в QEMU (с FAT32-диском и Virtio-Net):

```bash
qemu-system-aarch64 \
    -machine virt,virtualization=on \
    -cpu cortex-a53 \
    -nographic \
    -serial mon:stdio \
    -m size=1024M \
    -kernel images/sel4test-driver-image-arm-qemu-arm-virt \
    -drive file=../projects/sel4test/apps/sel4test-driver/fat32.img,format=raw,if=none,id=mydrive \
    -device virtio-blk-device,drive=mydrive \
    -netdev user,id=net0,hostfwd=tcp::8888-:80 \
    -object filter-dump,id=netdump,netdev=net0,file=traffic.pcap \
    -device virtio-net-device,netdev=net0,mac=52:54:00:12:34:56 \
    | tee ../projects/sel4test/apps/sel4test-driver/src/qemu_output.log

```

### Проверка сети

Для проверки UDP откройте отдельный терминал на хосте:

```bash
nc -ul 8080

```

Внутри Shell в QEMU:

```text
netstat
ping 10.0.2.2
send Hello Linux
sendto 10.0.2.2 8080 Hello again

```

---

## 📈 План развития (Roadmap)

**Завершено:**

* [x] Разделение системы на независимые файлы (Rootserver, драйверы, shell)
* [x] Межпроцессное разделение памяти (Shared Memory Heap)
* [x] Настоящий exec (Запуск бинарников из памяти)
* [x] Корректная идентификация процессов (Badged Endpoints)
* [x] Передача аргументов командной строки (`argc/argv`)
* [x] Background Execution (Символ `&` и фоновые демоны)
* [x] **Иерархическая ФС (VFS 2.0):** Древовидная файловая система в RAM.
* [x] **Real Filesystem (FAT32):** Блочный драйвер (Virtio) с поддержкой очередей DMA и сквозного чтения файлов с накопителя.
* [x] **Virtio-Net User-Space Driver:** Сетевой драйвер в отдельной песочнице с ARP, ICMP Echo и UDP.
* [x] **Self-Healing Architecture:** Универсальный Watchdog (Meta-Driven Recovery), защищающий систему от Hard-Crash (Segfault) и Soft-Crash (Deadlock) с мгновенным автоматическим пересозданием системных процессов.

**В планах:**

* [ ] **Написание своих исполняемых внутренних программ**
* [ ] **Расширение сетевого стека:** UDP receive API (входящие соединения), DNS резолвер.
* [ ] **Портирование на реальное железо**

## Работа с кодом

Приветствуются новые идеи и форки.

В руководстве подробно расписано:

* 🧬 Базовую идея архитектуры Psych Ward OS.

👉 **[Ознакомиться с руководством (CONTRIBUTING.md)](https://www.google.com/search?q=CONTRIBUTING.md)**

