#!/usr/bin/python3
# SPDX-License-Identifier: GPL-2.0-only
"""Exercise notify-send against a mock server in a PRIVATE dbus-run-session."""
import os
from pathlib import Path
import threading

from gi.repository import Gio, GLib

from test_notifier import Topology, notifier


XML = """
<node><interface name="org.freedesktop.Notifications">
  <method name="GetCapabilities"><arg type="as" direction="out"/></method>
  <method name="GetServerInformation">
    <arg type="s" direction="out"/><arg type="s" direction="out"/>
    <arg type="s" direction="out"/><arg type="s" direction="out"/>
  </method>
  <method name="Notify">
    <arg type="s" direction="in"/><arg type="u" direction="in"/>
    <arg type="s" direction="in"/><arg type="s" direction="in"/>
    <arg type="s" direction="in"/><arg type="as" direction="in"/>
    <arg type="a{sv}" direction="in"/><arg type="i" direction="in"/>
    <arg type="u" direction="out"/>
  </method>
</interface></node>
"""


def main():
    if os.environ.get("AURORA_TEST_PRIVATE_BUS") != "1":
        raise SystemExit("Use dbus-run-session with AURORA_TEST_PRIVATE_BUS=1")
    bus = Gio.bus_get_sync(Gio.BusType.SESSION, None)
    result = bus.call_sync(
        "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
        "RequestName", GLib.Variant("(su)", ("org.freedesktop.Notifications", 4)),
        GLib.VariantType.new("(u)"), Gio.DBusCallFlags.NONE, 5000, None,
    )
    assert result.unpack() == (1,), "The private test bus must have no notification server"
    calls = []

    def method(connection, sender, path, interface, name, params, invocation):
        if name == "GetCapabilities":
            reply = GLib.Variant("(as)", (["body"],))
        elif name == "GetServerInformation":
            reply = GLib.Variant("(ssss)", ("Test server", "Aurora", "1", "1.2"))
        else:
            assert name == "Notify"
            calls.append(params.unpack())
            reply = GLib.Variant("(u)", (42,))
        invocation.return_value(reply)

    info = Gio.DBusNodeInfo.new_for_xml(XML).interfaces[0]
    registration = bus.register_object("/org/freedesktop/Notifications", info, method, None, None)
    loop = GLib.MainLoop()
    errors = []

    def exercise():
        fixture = Topology()
        fixture.setUp()
        try:
            assert notifier.is_j700(fixture.sysfs)
            events = [fixture.event(fixture.front, ""), fixture.event(fixture.billboard)]
            context = type("Context", (), {"list_devices": lambda *args, **kwargs: events})()
            parent = type("Device", (), {"properties": {"USEC_INITIALIZED": "1000"}})()
            live, eligible = notifier.collect(context, fixture.sysfs, lambda *args: parent)
            state = notifier.Notifier(Path(fixture.tmp.name) / "state.json", active=lambda: True)
            state.reconcile(eligible)
            state.reconcile(eligible)
            # A service restart must retain deduplication.
            restarted = notifier.Notifier(state.path, active=lambda: True)
            restarted.reconcile(eligible)
            # Remove the Billboard function; the dock's instance remains live.
            events.pop()
            live, eligible = notifier.collect(context, fixture.sysfs, lambda *args: parent)
            restarted.reconcile(eligible | (live & restarted.shown))
            assert restarted.shown == state.shown
            assert len(calls) == 1, calls
            app, replacement, icon, summary, body, actions, hints, expiry = calls[0]
            assert app == "Aurora Display Hint"
            assert replacement == 0
            # libnotify versions may send the icon in app_icon or image-path.
            assert (icon == "video-display-symbolic"
                    or hints.get("image-path") == "video-display-symbolic"), calls[0]
            assert (summary, body) == (notifier.SUMMARY, notifier.BODY)
            assert actions == []
            assert hints["urgency"] == 1
            assert hints["desktop-entry"] == "org.aurorasilicon.DisplayHint"
            assert expiry == -1  # Respect the notification server's expiration policy.
        except BaseException as error:
            errors.append(error)
        finally:
            fixture.doCleanups()
            GLib.idle_add(loop.quit)

    worker = threading.Thread(target=exercise, daemon=True)
    worker.start()
    timeout = GLib.timeout_add_seconds(15, loop.quit)
    loop.run()
    GLib.source_remove(timeout)
    worker.join(timeout=1)
    bus.unregister_object(registration)
    assert not worker.is_alive(), "Timed out waiting for the notifier"
    if errors:
        raise errors[0]
    print("PASS: synthetic front plug -> libnotify -> private D-Bus server; one notification")


if __name__ == "__main__":
    main()
