#!/usr/bin/python3
# SPDX-License-Identifier: GPL-2.0-only
"""Explain the J700 front USB-C port's display limitation in the user's session."""

import fcntl
import json
import logging
import os
from pathlib import Path
import subprocess
import time


SUMMARY = "This port doesn't support displays."
BODY = "To connect a display, use the rear USB-C port."
MARKER = "AURORA_DISPLAY_UNSUPPORTED"
INTERVAL = 30.0


def read_attr(path, name):
    try:
        return (path / name).read_text(encoding="ascii").strip()
    except (OSError, UnicodeError):
        return ""


def is_j700(sysfs):
    try:
        compatible = (sysfs / "firmware/devicetree/base/compatible").read_bytes()
    except OSError:
        return False
    return b"apple,j700" in compatible.split(b"\0")


def front_device(path, sysfs):
    """Match the fixed hub's front branch, independently of USB bus numbers."""
    path = path.resolve()
    if not path.is_relative_to((sysfs / "devices").resolve()):
        return None
    for front in (path, *path.parents):
        if read_attr(front, "devpath") != "1.1":
            continue
        hub = front.parent
        if (read_attr(hub, "devpath") == "1"
                and read_attr(hub, "idVendor") == "2109"
                and read_attr(hub, "idProduct") == "2122"
                and any(p.name == "40a280000.usb" for p in hub.parents)):
            return front
    return None


def candidate(device, sysfs):
    if (device.device_type != "usb_device"
            or device.properties.get(MARKER) != "1"):
        return None
    path = Path(device.sys_path)
    if read_attr(path, "bDeviceClass") != "11":
        return None
    return front_device(path, sysfs)


def plug_id(front, initialized):
    """A new USB device instance on the front branch starts a new plug."""
    number = read_attr(front, "devnum")
    try:
        inode = front.stat().st_ino
    except OSError:
        return None
    if not number.isdecimal():
        return None
    return f"{front}:{number}:{inode}:{initialized}"


def command(args):
    return subprocess.run(args, check=True, capture_output=True, text=True, timeout=5).stdout


def active_desktop(uid):
    """The user manager may outlive the local graphical login (lingering/SSH)."""
    try:
        sessions = command(["loginctl", "show-user", str(uid), "-p", "Sessions", "--value"])
        for session in sessions.split():
            data = command([
                "loginctl", "show-session", session, "-p", "User", "-p", "Active",
                "-p", "Remote", "-p", "Type", "-p", "Seat",
            ])
            props = dict(line.split("=", 1) for line in data.splitlines() if "=" in line)
            if (props.get("User") == str(uid) and props.get("Active") == "yes"
                    and props.get("Remote") == "no" and props.get("Seat")
                    and props.get("Type") in ("wayland", "x11")):
                return True
    except (OSError, subprocess.SubprocessError):
        pass
    return False


def notify():
    try:
        command([
            "notify-send", "--app-name=Aurora Display Hint", "--urgency=normal",
            "--icon=video-display-symbolic",
            "--hint=string:desktop-entry:org.aurorasilicon.DisplayHint",
            "--", SUMMARY, BODY,
        ])
    except (OSError, subprocess.SubprocessError):
        logging.warning("Could not deliver the display hint; retrying in 30 seconds")
        return False
    logging.info("Sent the front USB-C display hint")
    return True


class Notifier:
    def __init__(self, state_path, send=notify, active=None, clock=time.monotonic):
        self.path = state_path
        self.send = send
        self.active = active if active is not None else lambda: active_desktop(os.getuid())
        self.clock = clock
        self.shown = set()
        self.last_attempt = None
        try:
            data = json.loads(self.path.read_text(encoding="utf-8"))
            shown = data["shown"]
            stamp = data["last_attempt"]
            if (isinstance(shown, list) and all(isinstance(x, str) for x in shown)
                    and (stamp is None or isinstance(stamp, (int, float)))):
                self.shown = set(shown)
                self.last_attempt = stamp
        except (OSError, ValueError, KeyError, TypeError):
            pass

    def save(self):
        temporary = self.path.with_suffix(".tmp")
        temporary.write_text(json.dumps({
            "shown": sorted(self.shown), "last_attempt": self.last_attempt,
        }) + "\n", encoding="utf-8")
        temporary.replace(self.path)

    def reconcile(self, plugs):
        # Keep successful delivery across restarts, but forget disconnected plugs.
        current = self.shown & plugs
        if current != self.shown:
            self.shown = current
            self.save()
        if not plugs - self.shown:
            return
        now = self.clock()
        if self.last_attempt is not None and now - self.last_attempt < INTERVAL:
            return
        if not self.active():
            return
        self.last_attempt = self.clock()
        self.save()
        if self.send():
            self.shown.update(plugs)
            self.save()


def lookup_device(context, path):
    import pyudev

    try:
        return pyudev.Devices.from_sys_path(context, path)
    except pyudev.DeviceNotFoundError:
        return None


def collect(context, sysfs, device_from_path):
    # Live front devices retain deduplication even if the Billboard function
    # disappears and reappears while the dock stays connected.
    live = set()
    eligible = set()
    for device in context.list_devices(subsystem="usb", DEVTYPE="usb_device"):
        path = Path(device.sys_path)
        front = front_device(path, sysfs)
        if front is None:
            continue
        try:
            parent = device_from_path(context, str(front))
            if parent is None:
                continue
            initialized = parent.properties.get("USEC_INITIALIZED", "")
            if not initialized:
                continue
            identity = plug_id(front, initialized)
        except (OSError, ValueError):
            continue
        if identity is None:
            continue
        live.add(identity)
        if candidate(device, sysfs) is not None:
            eligible.add(identity)
    return live, eligible


def run(context, monitor, sysfs, state, device_from_path):
    # Subscribe before enumeration so a plug during startup cannot be lost.
    monitor.start()
    while True:
        live, eligible = collect(context, sysfs, device_from_path)
        # Previously shown plugs remain tracked after their Billboard disappears.
        state.reconcile(eligible | (live & state.shown))
        # Periodic reconciliation also handles login, failed delivery and a
        # dropped uevent. Bound work during a burst of USB enumeration events.
        deadline = time.monotonic() + 2
        while time.monotonic() < deadline:
            monitor.poll(timeout=max(0, deadline - time.monotonic()))


def main():
    logging.basicConfig(level=logging.INFO, format="%(levelname)s: %(message)s")
    sysfs = Path("/sys")
    if not is_j700(sysfs):
        return
    if os.getuid() == 0:
        raise SystemExit("Run this notifier in a regular user's graphical session")
    if not os.environ.get("DBUS_SESSION_BUS_ADDRESS"):
        raise SystemExit("The user session bus is unavailable")
    runtime = Path(os.environ["XDG_RUNTIME_DIR"]) / "aurora-display-notifier"
    runtime.mkdir(mode=0o700, exist_ok=True)
    # One user manager, one session bus, one notifier, including manual starts.
    with (runtime / "lock").open("w", encoding="ascii") as lock:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            return
        import pyudev

        context = pyudev.Context()
        monitor = pyudev.Monitor.from_netlink(context, source="udev")
        monitor.filter_by(subsystem="usb", device_type="usb_device")
        state = Notifier(runtime / "state.json")
        run(context, monitor, sysfs, state, lookup_device)


if __name__ == "__main__":
    main()
