#!/usr/bin/env python3
"""
jtag_ocd.py — минимальный клиент telnet-консоли OpenOCD (порт 4444).

Зачем отдельный клиент, а не `echo ... | nc localhost 4444` (приём из
../../debug_of_FT232H/FT232H-jtag-guide.md, §6): для коротких команд
(`resume`, `targets`) nc годится, но `load_image` 1.9 МБ блокирует консоль
на десятки секунд, а nc закрывает сокет сразу, как только кончится stdin —
OpenOCD получает EOF посреди заливки. Обходить это `sleep`'ом наугад значит
либо рвать заливку, либо всегда ждать worst-case. Здесь вместо этого сокет
держится открытым до маркера, который печатает сам OpenOCD (`echo <маркер>`
последней командой), и клиент выходит ровно тогда, когда цель реально
дошла до конца списка.

Использование:
    jtag_ocd.py -c "halt" -c "targets" [--marker DONE] [--timeout 300]
    echo "resume" | jtag_ocd.py --stdin

Код возврата: 0 — маркер получен (или маркер не задан и все команды ушли),
1 — таймаут, 2 — не удалось подключиться.
"""

import argparse
import socket
import sys
import time

IAC = 0xFF  # начало telnet-команды


def strip_telnet(buf: bytes) -> bytes:
    """Выкинуть telnet-негоциацию (IAC + опция). OpenOCD шлёт WILL ECHO /
    WILL SUPPRESS_GO_AHEAD при подключении — в текст лога они попадать
    не должны."""
    out = bytearray()
    i = 0
    n = len(buf)
    while i < n:
        b = buf[i]
        if b == IAC:
            if i + 1 < n and buf[i + 1] == IAC:  # экранированный 0xFF
                out.append(IAC)
                i += 2
                continue
            # IAC WILL/WONT/DO/DONT <опция> — 3 байта; IAC SB ... IAC SE — пропустим грубо
            i += 3
            continue
        out.append(b)
        i += 1
    return bytes(out)


def main() -> int:
    ap = argparse.ArgumentParser(description="Клиент telnet-консоли OpenOCD")
    ap.add_argument("--host", default="localhost")
    ap.add_argument("--port", type=int, default=4444)
    ap.add_argument("-c", "--command", action="append", default=[],
                    help="команда OpenOCD (можно повторять, порядок сохраняется)")
    ap.add_argument("--stdin", action="store_true",
                    help="дочитать команды из stdin (по одной на строку)")
    ap.add_argument("--marker", default=None,
                    help="строка, при появлении которой в выводе клиент выходит с кодом 0")
    ap.add_argument("--timeout", type=float, default=300.0,
                    help="общий бюджет ожидания в секундах (по умолчанию 300)")
    ap.add_argument("--quiet", action="store_true", help="не печатать вывод OpenOCD")
    args = ap.parse_args()

    cmds = list(args.command)
    if args.stdin:
        cmds += [ln.rstrip("\n") for ln in sys.stdin if ln.strip()]
    if not cmds:
        print("jtag_ocd.py: не задано ни одной команды", file=sys.stderr)
        return 2

    try:
        sock = socket.create_connection((args.host, args.port), timeout=10)
    except OSError as exc:
        print(f"jtag_ocd.py: не могу подключиться к {args.host}:{args.port} — {exc}",
              file=sys.stderr)
        print("  OpenOCD не запущен на carto, либо не поднят SSH-туннель "
              "(задача 'JTAG: SSH-туннель (carto)' в VSCode).", file=sys.stderr)
        return 2

    sock.settimeout(1.0)
    deadline = time.monotonic() + args.timeout
    pending = "".join(c + "\n" for c in cmds).encode()
    sock.sendall(pending)

    tail = ""
    found = args.marker is None
    try:
        while time.monotonic() < deadline:
            try:
                chunk = sock.recv(4096)
            except socket.timeout:
                continue
            if not chunk:
                break
            text = strip_telnet(chunk).decode("utf-8", errors="replace")
            if not args.quiet:
                sys.stdout.write(text)
                sys.stdout.flush()
            if args.marker is not None:
                tail = (tail + text)[-4096:]
                if args.marker in tail:
                    found = True
                    break
    finally:
        try:
            sock.close()
        except OSError:
            pass

    if not args.quiet:
        sys.stdout.write("\n")
        sys.stdout.flush()
    if not found:
        print(f"jtag_ocd.py: маркер '{args.marker}' не получен за {args.timeout:.0f}с",
              file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
