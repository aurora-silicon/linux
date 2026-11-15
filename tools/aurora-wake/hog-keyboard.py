#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""
Emulated BLE HID-over-GATT keyboard on a BlueZ host, for unattended
Bluetooth wake tests of another machine.

It behaves like a real BLE keyboard that went idle: it advertises only when a
key is "pressed" (a command on the control FIFO), and stops advertising as soon
as a central connects, so a central that disconnects it for suspend is not
woken again until the next key.

Run as root:  hog-keyboard.py [--ctl /run/hogkb/ctl] [--log /run/hogkb/log]
Commands (one per line, written to the FIFO):
  pairable on|off   adapter discoverable + pairable, advertise for pairing
  key [usage [secs]] advertise if needed, wait up to secs (60) for the
                    central to connect and subscribe, then press+release
  adv on|off        advertise / stop (debug)
  status            log connection state
  quit
"""
import argparse
import os
import subprocess
import sys
import threading
import time

import dbus
import dbus.exceptions
import dbus.mainloop.glib
import dbus.service
from gi.repository import GLib

BLUEZ = "org.bluez"
OM = "org.freedesktop.DBus.ObjectManager"
PROPS = "org.freedesktop.DBus.Properties"
GATT_MGR = "org.bluez.GattManager1"
SVC = "org.bluez.GattService1"
CHR = "org.bluez.GattCharacteristic1"
DSC = "org.bluez.GattDescriptor1"
AGENT = "org.bluez.Agent1"
ADV_INSTANCE = "1"

# Boot-compatible keyboard with report ID 1: modifiers, reserved, 6 keys.
REPORT_MAP = bytes([
    0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x85, 0x01,
    0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7, 0x15, 0x00, 0x25, 0x01,
    0x75, 0x01, 0x95, 0x08, 0x81, 0x02,
    0x95, 0x01, 0x75, 0x08, 0x81, 0x01,
    0x95, 0x06, 0x75, 0x08, 0x15, 0x00, 0x25, 0x65,
    0x05, 0x07, 0x19, 0x00, 0x29, 0x65, 0x81, 0x00,
    0xC0,
])

LOG = None


def log(msg):
    line = "%s %s" % (time.strftime("%Y-%m-%dT%H:%M:%S", time.localtime())
                      + ".%03d" % (int(time.time() * 1000) % 1000), msg)
    print(line, flush=True)
    if LOG:
        with open(LOG, "a") as f:
            f.write(line + "\n")


def ay(data):
    return dbus.Array([dbus.Byte(b) for b in data], signature="y")


class Application(dbus.service.Object):
    def __init__(self, bus):
        self.path = "/org/aurora/hogkb"
        self.services = []
        super().__init__(bus, self.path)

    @dbus.service.method(OM, out_signature="a{oa{sa{sv}}}")
    def GetManagedObjects(self):
        out = {}
        for s in self.services:
            out[s.path] = s.props()
            for c in s.chars:
                out[c.path] = c.props()
                for d in c.descs:
                    out[d.path] = d.props()
        return out


class Service(dbus.service.Object):
    def __init__(self, bus, app, index, uuid):
        self.path = "%s/service%d" % (app.path, index)
        self.uuid = uuid
        self.chars = []
        super().__init__(bus, self.path)
        app.services.append(self)

    def props(self):
        return {SVC: {"UUID": self.uuid, "Primary": True,
                      "Characteristics": dbus.Array([c.path for c in self.chars], signature="o")}}


class Characteristic(dbus.service.Object):
    def __init__(self, bus, svc, index, uuid, flags, value=b""):
        self.path = "%s/char%d" % (svc.path, index)
        self.svc = svc
        self.uuid = uuid
        self.flags = flags
        self.value = bytes(value)
        self.descs = []
        self.notifying = False
        super().__init__(bus, self.path)
        svc.chars.append(self)

    def props(self):
        return {CHR: {"Service": dbus.ObjectPath(self.svc.path), "UUID": self.uuid,
                      "Flags": self.flags,
                      "Descriptors": dbus.Array([d.path for d in self.descs], signature="o")}}

    @dbus.service.method(CHR, in_signature="a{sv}", out_signature="ay")
    def ReadValue(self, options):
        return ay(self.value)

    @dbus.service.method(CHR, in_signature="aya{sv}")
    def WriteValue(self, value, options):
        self.value = bytes(value)

    @dbus.service.method(CHR)
    def StartNotify(self):
        self.notifying = True
        log("notify on %s" % self.uuid)

    @dbus.service.method(CHR)
    def StopNotify(self):
        self.notifying = False
        log("notify off %s" % self.uuid)

    @dbus.service.signal(PROPS, signature="sa{sv}as")
    def PropertiesChanged(self, interface, changed, invalidated):
        pass

    def notify(self, value):
        self.value = bytes(value)
        self.PropertiesChanged(CHR, {"Value": ay(value)}, [])


class Descriptor(dbus.service.Object):
    def __init__(self, bus, chrc, index, uuid, flags, value):
        self.path = "%s/desc%d" % (chrc.path, index)
        self.chrc = chrc
        self.uuid = uuid
        self.flags = flags
        self.value = bytes(value)
        super().__init__(bus, self.path)
        chrc.descs.append(self)

    def props(self):
        return {DSC: {"Characteristic": dbus.ObjectPath(self.chrc.path), "UUID": self.uuid,
                      "Flags": self.flags}}

    @dbus.service.method(DSC, in_signature="a{sv}", out_signature="ay")
    def ReadValue(self, options):
        return ay(self.value)


class Agent(dbus.service.Object):
    """Just Works: accept everything (test peripheral only)."""

    @dbus.service.method(AGENT)
    def Release(self):
        pass

    @dbus.service.method(AGENT, in_signature="os")
    def AuthorizeService(self, device, uuid):
        log("authorize service %s %s" % (device, uuid))

    @dbus.service.method(AGENT, in_signature="ou")
    def RequestConfirmation(self, device, passkey):
        log("confirm %s %06d" % (device, passkey))

    @dbus.service.method(AGENT, in_signature="o")
    def RequestAuthorization(self, device):
        log("authorize %s" % device)

    @dbus.service.method(AGENT)
    def Cancel(self):
        log("agent cancel")


class Keyboard:
    def __init__(self, bus, adapter):
        self.bus = bus
        self.adapter_path = adapter
        self.adapter = dbus.Interface(bus.get_object(BLUEZ, adapter), PROPS)
        self.app = Application(bus)
        hid = Service(bus, self.app, 0, "1812")
        Characteristic(bus, hid, 0, "2a4a", ["read"], [0x11, 0x01, 0x00, 0x02])
        Characteristic(bus, hid, 1, "2a4b", ["encrypt-read"], REPORT_MAP)
        Characteristic(bus, hid, 2, "2a4c", ["write-without-response"], [0])
        Characteristic(bus, hid, 3, "2a4e", ["read", "write-without-response"], [1])
        self.report = Characteristic(bus, hid, 4, "2a4d", ["encrypt-read", "notify"], [0] * 8)
        Descriptor(bus, self.report, 0, "2908", ["read"], [0x01, 0x01])
        bat = Service(bus, self.app, 1, "180f")
        Characteristic(bus, bat, 0, "2a19", ["read", "notify"], [100])
        dis = Service(bus, self.app, 2, "180a")
        # PnP ID: USB vendor source, Linux Foundation, test product.
        Characteristic(bus, dis, 0, "2a50", ["read"], [0x02, 0x6b, 0x1d, 0x46, 0x02, 0x01, 0x00])
        self.index = adapter.rsplit("hci", 1)[1]
        self.adv = False
        self.connected = set()
        mgr = dbus.Interface(bus.get_object(BLUEZ, adapter), GATT_MGR)
        mgr.RegisterApplication(self.app.path, {},
                                reply_handler=lambda: log("GATT application registered"),
                                error_handler=lambda e: log("GATT register failed: %s" % e))
        self.agent = Agent(bus, "/org/aurora/hogkb/agent")
        am = dbus.Interface(bus.get_object(BLUEZ, "/org/bluez"), "org.bluez.AgentManager1")
        am.RegisterAgent(self.agent._object_path, "NoInputNoOutput")
        am.RequestDefaultAgent(self.agent._object_path)
        bus.add_signal_receiver(self.props_changed, dbus_interface=PROPS,
                                signal_name="PropertiesChanged", path_keyword="path")

    def props_changed(self, interface, changed, invalidated, path=None):
        if interface != "org.bluez.Device1" or "Connected" not in changed:
            return
        if changed["Connected"]:
            self.connected.add(path)
            log("connected %s" % path)
            # A real keyboard stops advertising once its host is back.
            self.set_adv(False)
        else:
            self.connected.discard(path)
            log("disconnected %s" % path)

    def set_adv(self, on, discoverable=False):
        """
        Legacy MGMT advertising through btmgmt: on some kernels bluetoothd's
        extended advertising path rejects the data (Add Extended Advertising
        Data: Invalid Parameters), while Add Advertising works. Instance 1,
        connectable, HID service UUID; appearance and name in scan response.
        """
        if on and not self.adv:
            scan_rsp = "0319c103" + "0d09" + b"AuroraTestKB".hex()
            cmd = ["btmgmt", "--index", self.index, "add-adv", "-c", "-u", "1812",
                   "-s", scan_rsp]
            if discoverable:
                cmd.append("-g")
            ret = subprocess.run(cmd + [ADV_INSTANCE], capture_output=True, text=True,
                                 timeout=10, stdin=subprocess.PIPE)
            self.adv = "Instance added" in ret.stdout
            log("advertising" if self.adv else "advertise failed: %s" % ret.stdout.strip())
        elif not on and self.adv:
            subprocess.run(["btmgmt", "--index", self.index, "rm-adv", ADV_INSTANCE],
                           capture_output=True, timeout=10, stdin=subprocess.PIPE)
            self.adv = False
            log("advertising stopped")

    def pairable(self, on):
        self.adapter.Set("org.bluez.Adapter1", "Pairable", dbus.Boolean(on))
        self.adapter.Set("org.bluez.Adapter1", "Discoverable", dbus.Boolean(on))
        self.set_adv(False)
        if on:
            self.set_adv(True, discoverable=True)
        log("pairable %s" % on)

    def key(self, usage, deadline):
        """Advertise until a central is connected and subscribed, then type."""
        if not (self.connected and self.report.notifying):
            if not self.adv:
                self.set_adv(True)
            if time.monotonic() < deadline:
                GLib.timeout_add(50, self.key, usage, deadline)
            else:
                log("key %#x: no central subscribed before the deadline" % usage)
                self.set_adv(False)
            return False
        self.report.notify([0, 0, usage, 0, 0, 0, 0, 0])
        GLib.timeout_add(30, lambda: (self.report.notify([0] * 8), False)[1])
        log("key %#x sent" % usage)
        return False


def fifo_reader(path, kb):
    while True:
        with open(path) as f:
            for line in f:
                words = line.split()
                if not words:
                    continue
                GLib.idle_add(command, kb, words)


def command(kb, words):
    cmd = words[0]
    log("command: %s" % " ".join(words))
    if cmd == "pairable":
        kb.pairable(words[1:] == ["on"])
    elif cmd == "adv":
        kb.set_adv(words[1:] == ["on"])
    elif cmd == "key":
        usage = int(words[1], 0) if len(words) > 1 else 0x2c  # space
        wait = float(words[2]) if len(words) > 2 else 60
        kb.key(usage, time.monotonic() + wait)
    elif cmd == "status":
        log("connected=%s notifying=%s advertising=%s" %
            (sorted(kb.connected), kb.report.notifying, kb.adv))
    elif cmd == "quit":
        kb.set_adv(False)
        loop.quit()
    return False


def main():
    global LOG, loop
    ap = argparse.ArgumentParser()
    ap.add_argument("--ctl", default="/run/hogkb/ctl")
    ap.add_argument("--log", default="/run/hogkb/log")
    ap.add_argument("--adapter", default="/org/bluez/hci0")
    args = ap.parse_args()
    os.makedirs(os.path.dirname(args.ctl), exist_ok=True)
    if not os.path.exists(args.ctl):
        os.mkfifo(args.ctl, 0o600)
    LOG = args.log
    dbus.mainloop.glib.DBusGMainLoop(set_as_default=True)
    bus = dbus.SystemBus()
    kb = Keyboard(bus, args.adapter)
    threading.Thread(target=fifo_reader, args=(args.ctl, kb), daemon=True).start()
    loop = GLib.MainLoop()
    log("ready, control FIFO %s" % args.ctl)
    loop.run()


if __name__ == "__main__":
    sys.exit(main())
