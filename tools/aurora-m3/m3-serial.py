#!/usr/bin/env python3
"""m3-serial.py: record an Apple Silicon Mac's serial console from a second Mac.

Run on the helper Mac (macOS), with the two Macs' DFU ports joined by a
USB 3 USB-C cable and macvdmtool installed:

  sudo python3 m3-serial.py check     is the other Mac connected?
  sudo python3 m3-serial.py reboot    reboot it and record the whole boot
  sudo python3 m3-serial.py watch     record from now on

It writes everything to serial-<date>.log in the current folder and shows it
on screen. Press Ctrl-C to stop. It only reads: it never types on the console.

A Mac leaves serial mode whenever it resets, so after a reboot this re-arms
serial mode every few seconds until the console talks, then stops: every
re-arm puts a stray byte on the line, which would otherwise stop the boot
loader's countdown.
"""
import os
import select
import shutil
import signal
import subprocess
import sys
import termios
import time

DEV = "/dev/cu.debug-console"
TOOL = "macvdmtool"


def find_tool():
    # sudo on macOS runs with PATH=/usr/bin:/bin:/usr/sbin:/sbin, which misses
    # where macvdmtool usually lives.
    here = os.path.dirname(os.path.abspath(__file__))
    for p in (shutil.which(TOOL), "/usr/local/bin/macvdmtool",
              "/opt/homebrew/bin/macvdmtool", os.path.join(here, "macvdmtool"),
              # the built tool inside a macvdmtool clone next to this script
              os.path.join(here, "macvdmtool", "macvdmtool")):
        if p and os.path.isfile(p) and os.access(p, os.X_OK):
            return p
    sys.exit("macvdmtool not found: install it in /usr/local/bin, or put it next to this script.")


def tool(*args):
    try:
        r = subprocess.run([TOOL, *args], capture_output=True, text=True, timeout=30)
    except subprocess.TimeoutExpired:
        return False, "macvdmtool timed out"
    return r.returncode == 0, (r.stdout + r.stderr).strip()


def open_serial():
    fd = os.open(DEV, os.O_RDONLY | os.O_NOCTTY | os.O_NONBLOCK)
    attrs = termios.tcgetattr(fd)
    attrs[0] = 0                                                # iflag: raw
    attrs[1] = 0                                                # oflag
    attrs[2] = termios.CS8 | termios.CREAD | termios.CLOCAL    # cflag
    attrs[3] = 0                                                # lflag: no echo
    attrs[4] = attrs[5] = termios.B115200
    termios.tcsetattr(fd, termios.TCSANOW, attrs)
    return fd


def main():
    mode = sys.argv[1] if len(sys.argv) > 1 else ""
    if mode not in ("check", "reboot", "watch"):
        sys.exit(__doc__)
    if os.geteuid() != 0:
        sys.exit("run it with sudo: sudo python3 m3-serial.py " + mode)
    global TOOL
    TOOL = find_tool()

    ok, out = tool("nop")
    if not ok:
        sys.exit("macvdmtool can't reach the other Mac:\n" + out +
                 "\nCheck the cable is USB 3 and in the DFU port on BOTH Macs, then try again.")
    if mode == "check":
        print("Connected:\n" + out)
        return

    try:
        fd = open_serial()
    except FileNotFoundError:
        tool("serial")              # the device can appear only once serial mode is on
        time.sleep(1)
        fd = open_serial()
    name = time.strftime("serial-%Y%m%d-%H%M%S.log")
    log = open(name, "ab")
    print(f"Recording to {name}. Ctrl-C to stop.\n")

    # Ctrl-C, or a kill, stops it cleanly even when it was started in the
    # background (which starts it with SIGINT ignored).
    signal.signal(signal.SIGINT, signal.default_int_handler)
    signal.signal(signal.SIGTERM, signal.default_int_handler)

    if mode == "reboot":
        ok, out = tool("reboot")
        print(f"[m3-serial] reboot: {'sent' if ok else 'FAILED: ' + out}")
        if not ok:
            sys.exit(1)
        arm_until = time.time() + 180      # keep trying for three minutes
        # Re-arm back to back while it boots: m1n1's first lines come a few
        # seconds after the reset, and serial mode only sticks once the port
        # is back. Arms before the reset itself are lost, so wait for it first.
        time.sleep(2)
        gap = 0.5
    else:
        arm_until = time.time() + 10
        gap = 4
    armed = 0.0          # time of our last re-arm
    last_rx = 0.0        # time the console last sent text
    arming = True

    try:
        while True:
            r, _, _ = select.select([fd], [], [], 0.2 if arming else 1.0)
            now = time.time()
            if r:
                try:
                    data = os.read(fd, 4096)
                except BlockingIOError:
                    data = b""
                if data:
                    # A reset leaves 0xff/0x00 noise on the line; only text
                    # proves the console is back.
                    if any(32 <= b < 127 for b in data):
                        last_rx = now
                    log.write(data)
                    log.flush()
                    sys.stdout.write(data.decode("utf-8", "replace"))
                    sys.stdout.flush()
            if arming:
                if armed and last_rx > armed:
                    arming = False            # it talks after our re-arm: stop
                    print("\n[m3-serial] console is talking; no more re-arms\n")
                elif now > arm_until:
                    arming = False
                    print("\n[m3-serial] gave up re-arming; still recording\n")
                elif now - last_rx > 3 and now - armed > gap:
                    ok, _ = tool("serial")
                    if ok:
                        armed = now
    except KeyboardInterrupt:
        print(f"\nStopped. The log is {name}")


if __name__ == "__main__":
    main()
