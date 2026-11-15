# SPDX-License-Identifier: GPL-2.0-only
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch


spec = importlib.util.spec_from_file_location("notifier", Path(__file__).parents[1] / "notifier.py")
notifier = importlib.util.module_from_spec(spec)
spec.loader.exec_module(notifier)


class Topology(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.sysfs = Path(self.tmp.name).resolve()
        compatible = self.sysfs / "firmware/devicetree/base/compatible"
        compatible.parent.mkdir(parents=True)
        compatible.write_bytes(b"apple,j700\0apple,t8140\0apple,arm-platform\0")
        # Bus 7 deliberately differs from the observed bus 1.
        root = self.sysfs / "devices/platform/soc/40a280000.usb/xhci-hcd.9.auto/usb7"
        self.hub = self.device(root / "7-1", devpath="1", idVendor="2109", idProduct="2122")
        self.front = self.device(self.hub / "7-1.1", devpath="1.1", devnum="4")
        self.billboard = self.device(self.front / "7-1.1.5", devpath="1.1.5", bDeviceClass="11")

    def device(self, path, **attributes):
        path.mkdir(parents=True, exist_ok=True)
        for name, value in attributes.items():
            (path / name).write_text(value + "\n", encoding="ascii")
        return path

    def event(self, path, marker="1", kind="usb_device"):
        return SimpleNamespace(sys_path=str(path), device_type=kind,
                               properties={notifier.MARKER: marker})

    def test_machine_requires_exact_compatible_token(self):
        self.assertTrue(notifier.is_j700(self.sysfs))
        (self.sysfs / "firmware/devicetree/base/compatible").write_bytes(b"apple,j700-other\0")
        self.assertFalse(notifier.is_j700(self.sysfs))

    def test_machine_missing(self):
        self.assertFalse(notifier.is_j700(self.sysfs / "missing"))

    def test_front_nested_billboard(self):
        self.assertEqual(notifier.candidate(self.event(self.billboard), self.sysfs), self.front)

    def test_direct_front_billboard(self):
        (self.front / "bDeviceClass").write_text("11\n")
        self.assertEqual(notifier.candidate(self.event(self.front), self.sysfs), self.front)

    def test_rear_billboard_is_ignored_even_with_marker(self):
        rear = self.device(self.hub / "7-1.2", devpath="1.2", bDeviceClass="11")
        nested = self.device(rear / "7-1.2.5", devpath="1.2.5", bDeviceClass="11")
        for path in (rear, nested):
            self.assertIsNone(notifier.candidate(self.event(path), self.sysfs))

    def test_requires_marker_and_device_class(self):
        for marker, kind in (("", "usb_device"), ("1", "usb_interface")):
            self.assertIsNone(notifier.candidate(self.event(self.billboard, marker, kind), self.sysfs))
        (self.billboard / "bDeviceClass").write_text("09\n")
        self.assertIsNone(notifier.candidate(self.event(self.billboard), self.sysfs))

    def test_wrong_fixed_hub_is_ignored(self):
        (self.hub / "idProduct").write_text("0817\n")
        self.assertIsNone(notifier.candidate(self.event(self.billboard), self.sysfs))

    def test_wrong_controller_is_ignored(self):
        controller = self.hub.parents[2]
        controller.rename(controller.with_name("other.usb"))
        path = controller.with_name("other.usb") / self.billboard.relative_to(controller)
        self.assertIsNone(notifier.candidate(self.event(path), self.sysfs))

    def test_removed_device_and_outside_sysfs_are_ignored(self):
        self.assertIsNone(notifier.candidate(self.event(self.billboard / "gone"), self.sysfs))
        self.assertIsNone(notifier.front_device(Path("/tmp"), self.sysfs))

    def test_plug_identity_changes_with_device_number(self):
        first = notifier.plug_id(self.front, "1000")
        (self.front / "devnum").write_text("5\n")
        self.assertNotEqual(first, notifier.plug_id(self.front, "1001"))
        (self.front / "devnum").unlink()
        self.assertIsNone(notifier.plug_id(self.front, "1001"))

    def test_collect_coalesces_and_retains_live_front(self):
        second = self.device(self.front / "7-1.1.6", devpath="1.1.6", bDeviceClass="11")
        events = [self.event(self.front, ""), self.event(self.billboard), self.event(second)]
        context = SimpleNamespace(list_devices=lambda **kwargs: events)
        parent = SimpleNamespace(properties={"USEC_INITIALIZED": "1000"})
        live, eligible = notifier.collect(context, self.sysfs, lambda *args: parent)
        self.assertEqual(live, eligible)
        self.assertEqual(len(eligible), 1)
        # Billboard removal does not end the dock's plug.
        del events[1:]
        live2, eligible2 = notifier.collect(context, self.sysfs, lambda *args: parent)
        self.assertEqual(live, live2)
        self.assertEqual(eligible2, set())

    def test_collect_waits_for_front_initialization(self):
        context = SimpleNamespace(list_devices=lambda **kwargs: [self.event(self.billboard)])
        parent = SimpleNamespace(properties={})
        self.assertEqual(notifier.collect(context, self.sysfs, lambda *args: parent), (set(), set()))

    def test_collect_tolerates_unplug_race(self):
        context = SimpleNamespace(list_devices=lambda **kwargs: [self.event(self.billboard)])

        def disappeared(*args):
            raise OSError("device disappeared")

        self.assertEqual(notifier.collect(context, self.sysfs, disappeared), (set(), set()))
        self.assertEqual(notifier.collect(context, self.sysfs, lambda *args: None), (set(), set()))

    def test_lookup_handles_library_unplug_error(self):
        class DeviceGone(Exception):
            pass

        def disappeared(*args):
            raise DeviceGone()

        library = SimpleNamespace(DeviceNotFoundError=DeviceGone,
                                  Devices=SimpleNamespace(from_sys_path=disappeared))
        with patch.dict(sys.modules, {"pyudev": library}):
            self.assertIsNone(notifier.lookup_device(None, str(self.front)))


class Delivery(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.path = Path(self.tmp.name) / "state.json"
        self.now = 100.0
        self.sent = 0
        self.success = True
        self.active = True
        self.state = self.new_state()

    def new_state(self):
        return notifier.Notifier(self.path, self.send, lambda: self.active, lambda: self.now)

    def send(self):
        self.sent += 1
        return self.success

    def test_once_per_plug_including_restarts(self):
        self.state.reconcile({"plug-a"})
        self.now += 60
        self.state.reconcile({"plug-a"})
        self.new_state().reconcile({"plug-a"})
        self.assertEqual(self.sent, 1)

    def test_replug_waits_for_rate_limit(self):
        self.state.reconcile({"plug-a"})
        self.state.reconcile(set())
        self.now += 1
        self.state.reconcile({"plug-b"})
        self.assertEqual(self.sent, 1)
        self.now += 29
        self.state.reconcile({"plug-b"})
        self.assertEqual(self.sent, 2)

    def test_slow_session_check_does_not_shorten_rate_limit(self):
        sends = []
        checks = []

        def active():
            checks.append(self.now)
            if len(checks) == 1:
                self.now += 8
            return True

        def send():
            sends.append(self.now)
            # The dispatch timestamp must already be persisted for restarts.
            self.assertEqual(json.loads(self.path.read_text())["last_attempt"], self.now)
            return True

        state = notifier.Notifier(self.path, send, active, lambda: self.now)
        state.reconcile({"plug-a"})
        state.reconcile(set())
        state = notifier.Notifier(self.path, send, active, lambda: self.now)
        self.now = 130.0
        state.reconcile({"plug-b"})
        self.assertEqual(sends, [108.0])
        self.assertEqual(checks, [100.0])
        self.now = 138.0
        state.reconcile({"plug-b"})
        self.assertEqual(sends, [108.0, 138.0])
        self.assertGreaterEqual(sends[1] - sends[0], notifier.INTERVAL)

    def test_disconnected_pending_plug_is_never_delivered(self):
        self.state.reconcile({"plug-a"})
        self.now += 1
        self.state.reconcile({"plug-b"})
        self.state.reconcile(set())
        self.now += 60
        self.state.reconcile(set())
        self.assertEqual(self.sent, 1)

    def test_inactive_session_defers_until_active(self):
        self.active = False
        self.state.reconcile({"plug-a"})
        self.assertEqual(self.sent, 0)
        self.active = True
        self.state.reconcile({"plug-a"})
        self.assertEqual(self.sent, 1)

    def test_delivery_failure_retries_with_rate_limit(self):
        self.success = False
        self.state.reconcile({"plug-a"})
        self.assertEqual(self.state.shown, set())
        self.now += 29
        self.new_state().reconcile({"plug-a"})
        self.assertEqual(self.sent, 1)
        self.success = True
        self.now += 1
        self.state.reconcile({"plug-a"})
        self.assertEqual(self.sent, 2)
        self.assertEqual(self.state.shown, {"plug-a"})

    def test_corrupt_state_is_recoverable(self):
        for data in ("invalid", "[]", '{"shown": false, "last_attempt": []}'):
            self.path.write_text(data)
            state = self.new_state()
            self.assertEqual(state.shown, set())
            self.assertIsNone(state.last_attempt)

    def test_state_write_failure_prevents_untracked_send(self):
        with patch.object(self.state, "save", side_effect=OSError):
            with self.assertRaises(OSError):
                self.state.reconcile({"plug-a"})
        self.assertEqual(self.sent, 0)

    def test_empty_plugs_never_checks_session_or_notifies(self):
        with patch.object(self.state, "active", side_effect=AssertionError):
            self.state.reconcile(set())
        self.assertFalse(self.path.exists())

    def test_state_contains_no_peripheral_names(self):
        self.state.reconcile({"sysfs-path:4:123:1000"})
        data = json.loads(self.path.read_text())
        self.assertEqual(set(data), {"shown", "last_attempt"})


class SessionAndTransport(unittest.TestCase):
    def test_other_machine_never_checks_session(self):
        with patch.object(notifier, "is_j700", return_value=False), \
                patch.object(notifier.os, "getuid", side_effect=AssertionError):
            notifier.main()

    def test_root_and_missing_bus_never_start_notifier(self):
        with patch.object(notifier, "is_j700", return_value=True), \
                patch.object(notifier.os, "getuid", return_value=0):
            with self.assertRaises(SystemExit):
                notifier.main()
        with patch.object(notifier, "is_j700", return_value=True), \
                patch.object(notifier.os, "getuid", return_value=1000), \
                patch.dict(os.environ, {}, clear=True):
            with self.assertRaises(SystemExit):
                notifier.main()

    def test_session_gate(self):
        valid = "User=1000\nActive=yes\nRemote=no\nType=wayland\nSeat=seat0\n"
        cases = [(valid, True)] + [
            (valid.replace(old, new), False) for old, new in (
                ("User=1000", "User=1001"), ("Active=yes", "Active=no"),
                ("Remote=no", "Remote=yes"), ("Type=wayland", "Type=tty"),
                ("Seat=seat0", "Seat="),
            )
        ]
        cases.append((valid.replace("wayland", "x11"), True))
        for reply, expected in cases:
            with self.subTest(reply=reply), patch.object(notifier, "command", side_effect=["3\n", reply]):
                self.assertEqual(notifier.active_desktop(1000), expected)

    def test_logind_errors_fail_closed(self):
        for error in (FileNotFoundError(), subprocess.TimeoutExpired("loginctl", 5)):
            with patch.object(notifier, "command", side_effect=error):
                self.assertFalse(notifier.active_desktop(os.getuid()))

    def test_notification_is_plain_libnotify_on_inherited_bus(self):
        with patch.object(notifier, "command", return_value="") as call:
            self.assertTrue(notifier.notify())
        args = call.call_args.args[0]
        self.assertEqual(args[-3:], ["--", notifier.SUMMARY, notifier.BODY])
        self.assertIn("--urgency=normal", args)
        self.assertNotIn("--expire-time", " ".join(args))

    def test_notification_transport_failure(self):
        with patch.object(notifier, "command", side_effect=subprocess.TimeoutExpired("notify-send", 5)):
            self.assertFalse(notifier.notify())

    def test_subprocess_is_bounded_without_shell(self):
        with patch.object(subprocess, "run", return_value=SimpleNamespace(stdout="ok")) as run:
            self.assertEqual(notifier.command(["example", "literal"]), "ok")
        self.assertEqual(run.call_args.kwargs["timeout"], 5)
        self.assertNotIn("shell", run.call_args.kwargs)


if __name__ == "__main__":
    unittest.main()
