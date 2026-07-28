#!/usr/bin/env python3
"""
remote_bitbang <-> DirtyJTAG (RP2040) USB bridge.

Lets OpenOCD's stock `remote_bitbang` driver (any modern, unpatched OpenOCD
build) drive a DirtyJTAG-protocol RP2040 board one JTAG step at a time. No
custom OpenOCD driver/build needed - this is a plain TCP<->USB translator.

remote_bitbang wire protocol (OpenOCD's src/jtag/drivers/remote_bitbang.c):
    '0'-'7'  write tck/tms/tdi : bit2=tck bit1=tms bit0=tdi (base '0')
    'r'-'u'  write trst/srst   : bit1=trst bit0=srst        (base 'r')
    'R'      read tdo -> reply '0' or '1'
    'B'/'b'  blink on/off              (ignored - our own LEDs handle this)
    'Z'/'z'  sleep 1ms / 1us           (best-effort)
    'Q'      end this session (server keeps listening for the next one)
    'd'-'g','O','o','c'  SWD-only ops  (never sent for JTAG transport)

DirtyJTAG protocol (see pico-dirtyJtag/cmd.c):
    CMD_SETSIG (0x04) mask value       - 3 bytes, sets TRST/SRST (only)
    CMD_CLK    (0x06) signals pulses   - 3 bytes, jtag_strobe(): sets TMS via
                                          gpio_put() *and* clocks TDI/TDO
                                          through the PIO in one atomic call
    CMD_GETSIG (0x05)                  - 1 byte in, 1 byte out (bit3 = TDO)

CMD_CLK (jtag_strobe -> pio_jtag_write_tms_blocking), not CMD_SETSIG's
jtag_set_clk(), is what drives TCK/TMS/TDI here. jtag_set_clk() reuses
pio_jtag_write_read_blocking(), the same primitive jtag_transfer() uses for
CMD_XFER - and jtag_transfer() explicitly pins TMS low before calling it, so
it assumes TMS stays constant for the whole shift. That's fine for shifting
DR/IR data, but wrong for walking the TAP state machine with a changing TMS,
which is exactly what remote_bitbang needs. jtag_strobe() sets TMS itself,
in the right order, before clocking - so that's the primitive this bridge
uses instead. All commands round-trip through the same USB vendor bulk
endpoints the firmware already uses for CMD_XFER etc. (VID 0x1209 / PID
0xC0CA, OUT 0x01 / IN 0x82 - see pico-dirtyJtag/usb_descriptors.c).

Pending writes are packed into a single 64-byte USB packet and only flushed
when a read ('R') needs an actual reply, when the pack would overflow, or
when the input socket has nothing more immediately queued - a correctness
first pass, not a throughput-optimized one.

Install:
    python3 -m pip install pyusb

Run:
    python3 dirtyjtag_bitbang_bridge.py [--port 3335] [--vid 0x1209] [--pid 0xc0ca]

Pair with OpenOCD (see dirtyjtag_remote_bitbang.cfg in this same folder):
    openocd -f dirtyjtag_remote_bitbang.cfg -f bcm2711.cfg
"""

import argparse
import socket
import sys
import time

import usb.core
import usb.util

CMD_SETSIG = 0x04
CMD_GETSIG = 0x05
CMD_CLK = 0x06

SIG_TCK = 1 << 1
SIG_TDI = 1 << 2
SIG_TDO = 1 << 3
SIG_TMS = 1 << 4
SIG_TRST = 1 << 5
SIG_SRST = 1 << 6

RESET_MASK = SIG_TRST | SIG_SRST

MAX_PACKET = 64
USB_TIMEOUT_MS = 1000


class DirtyJtagUsb:
    def __init__(self, vid, pid):
        dev = usb.core.find(idVendor=vid, idProduct=pid)
        if dev is None:
            sys.exit(f"DirtyJTAG device {vid:#06x}:{pid:#06x} not found "
                     f"(check `system_profiler SPUSBDataType` / lsusb)")
        try:
            dev.set_configuration()
        except usb.core.USBError:
            pass  # already configured

        cfg = dev.get_active_configuration()
        intf = cfg[(0, 0)]  # probe/vendor interface is interface 0
        try:
            usb.util.claim_interface(dev, intf.bInterfaceNumber)
        except (usb.core.USBError, NotImplementedError):
            pass

        self.ep_out = usb.util.find_descriptor(
            intf, custom_match=lambda e:
                usb.util.endpoint_direction(e.bEndpointAddress) == usb.util.ENDPOINT_OUT)
        self.ep_in = usb.util.find_descriptor(
            intf, custom_match=lambda e:
                usb.util.endpoint_direction(e.bEndpointAddress) == usb.util.ENDPOINT_IN)
        if self.ep_out is None or self.ep_in is None:
            sys.exit("Could not find DirtyJTAG bulk endpoints on interface 0")

    def flush_write(self, buf):
        if buf:
            self.ep_out.write(bytes(buf), timeout=USB_TIMEOUT_MS)

    def flush_read(self, buf, n_expect):
        self.ep_out.write(bytes(buf), timeout=USB_TIMEOUT_MS)
        data = self.ep_in.read(MAX_PACKET, timeout=USB_TIMEOUT_MS)
        return bytes(data)[:n_expect]


def decode_reset_char(b):
    # remote_bitbang's trst/srst bits are OpenOCD's logical assert flags
    # (1 = reset asserted). Both lines are hardware active-low, but
    # cmd_setsig's SIG_TRST/SIG_SRST bits are raw GPIO levels (1 = pin
    # driven HIGH, see jtag_set_trst()/jtag_set_rst() in pio_jtag.c) - so
    # asserted (1) must map to the pin bit CLEAR (LOW), not set.
    v = b - ord('r')
    trst_asserted = (v >> 1) & 1
    srst_asserted = v & 1
    val = ((not trst_asserted) * SIG_TRST) | ((not srst_asserted) * SIG_SRST)
    return bytes([CMD_SETSIG, RESET_MASK, val])


class Stats:
    def __init__(self):
        self.writes = 0
        self.resets = 0
        self.reads_0 = 0
        self.reads_1 = 0

    def __str__(self):
        return (f"writes={self.writes} resets={self.resets} "
                f"reads=({self.reads_1} ones, {self.reads_0} zeros)")


def handle_client(jtag, conn, debug):
    pending = bytearray()
    stats = Stats()
    try:
        return handle_client_inner(jtag, conn, pending, stats, debug)
    finally:
        print(f"Session stats: {stats}", file=sys.stderr)


def handle_client_inner(jtag, conn, pending, stats, debug):
    # CMD_GETSIG's TDO bit is a cached value from the *previous* CMD_CLK call
    # (jtag_get_tdo() returns a static last_tdo set by the prior PIO shift),
    # not a live pin read. remote_bitbang's own convention is read-before-
    # advance (sample tdo for bit N, *then* send the rising edge for bit N+1),
    # so a plain immediate GETSIG on 'R' always retrieves the result of
    # whatever CMD_CLK happened before the *last* one - a constant one-clock
    # lag on every single bit. Compensate by clocking the already-staged
    # tms/tdi (from the most recent tck=0 write) right when the read is
    # requested, then GETSIG - mirroring the clock-then-read loop that the
    # standalone self-test uses successfully. The later tck=1 write for that
    # same bit is then a no-op (clocked_ahead), since we already issued it.
    staged = None
    clocked_ahead = False

    while True:
        chunk = conn.recv(4096)
        if not chunk:
            return

        # opportunistically drain whatever else is already queued, so a
        # burst of writes gets packed instead of round-tripping one at a time
        conn.setblocking(False)
        try:
            while True:
                more = conn.recv(4096)
                if not more:
                    return
                chunk += more
        except BlockingIOError:
            pass
        finally:
            conn.setblocking(True)

        for b in chunk:
            c = chr(b)
            if c in '01234567':
                stats.writes += 1
                v = b - ord('0')
                tck = (v >> 2) & 1
                tms = (v >> 1) & 1
                tdi = v & 1
                if debug:
                    print(f"W tck={tck} tms={tms} tdi={tdi}", file=sys.stderr)
                if tck == 0:
                    staged = (tms, tdi)
                elif clocked_ahead:
                    clocked_ahead = False
                else:
                    cmd_bytes = bytes([CMD_CLK, (tms * SIG_TMS) | (tdi * SIG_TDI), 1])
                    if len(pending) + len(cmd_bytes) > MAX_PACKET:
                        jtag.flush_write(pending)
                        pending.clear()
                    pending += cmd_bytes
            elif c in 'rstu':
                stats.resets += 1
                if debug:
                    v = b - ord('r')
                    print(f"RESET trst={(v>>1)&1} srst={v&1}", file=sys.stderr)
                if len(pending) + 3 > MAX_PACKET:
                    jtag.flush_write(pending)
                    pending.clear()
                pending += decode_reset_char(b)
            elif c == 'R':
                if staged is not None:
                    tms, tdi = staged
                    cmd_bytes = bytes([CMD_CLK, (tms * SIG_TMS) | (tdi * SIG_TDI), 1])
                    if len(pending) + len(cmd_bytes) + 1 > MAX_PACKET:
                        jtag.flush_write(pending)
                        pending.clear()
                    pending += cmd_bytes
                    staged = None
                    clocked_ahead = True
                pending.append(CMD_GETSIG)
                reply = jtag.flush_read(pending, 1)
                pending.clear()
                is_one = bool(reply) and bool(reply[0] & SIG_TDO)
                if is_one:
                    stats.reads_1 += 1
                else:
                    stats.reads_0 += 1
                if debug:
                    print(f"READ -> tdo={'1' if is_one else '0'} (raw={reply.hex() if reply else None})",
                          file=sys.stderr)
                conn.sendall(b'1' if is_one else b'0')
            elif c in 'Bb':
                pass
            elif c == 'Z':
                time.sleep(0.001)
            elif c == 'z':
                time.sleep(0.000001)
            elif c == 'Q':
                if debug:
                    print("QUIT", file=sys.stderr)
                jtag.flush_write(pending)
                pending.clear()
                return
            elif c in 'defgOoc':
                pass  # SWD-only, never sent for our JTAG transport
            else:
                print(f"warning: unknown remote_bitbang byte {b!r}", file=sys.stderr)

        if pending:
            jtag.flush_write(pending)
            pending.clear()


def serve(jtag, host, port, debug):
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((host, port))
    srv.listen(1)
    print(f"Listening on {host}:{port} for OpenOCD remote_bitbang ...")

    while True:
        conn, addr = srv.accept()
        print(f"OpenOCD connected from {addr}")
        try:
            handle_client(jtag, conn, debug)
        except (ConnectionResetError, BrokenPipeError):
            pass
        finally:
            conn.close()
            print("OpenOCD disconnected")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                  formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--host', default='127.0.0.1')
    ap.add_argument('--port', type=int, default=3335)
    ap.add_argument('--vid', type=lambda s: int(s, 0), default=0x1209)
    ap.add_argument('--pid', type=lambda s: int(s, 0), default=0xC0CA)
    ap.add_argument('--debug', action='store_true',
                     help='trace every decoded write/reset/read on stderr (verbose)')
    args = ap.parse_args()

    jtag = DirtyJtagUsb(args.vid, args.pid)
    serve(jtag, args.host, args.port, args.debug)


if __name__ == '__main__':
    main()
