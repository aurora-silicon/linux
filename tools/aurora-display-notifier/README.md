<!-- SPDX-License-Identifier: GPL-2.0-only -->
# J700 front USB-C display hint

The MacBook Neo (J700) front USB-C connector carries USB 2.0 and power,
but has no DisplayPort lanes. A display adapter can enumerate a USB
Billboard device when its alternate mode cannot be entered. This tool
shows an informational desktop notification:

> This port doesn't support displays.
> To connect a display, use the rear USB-C port.

It changes no display, USB authorization, power, notification, or saved
device settings. The desktop's notification preferences continue to apply.

## Recommended split and upstream prospects

Ship the hint in userspace. The udev rule only marks device-class `0x11`
Billboards on the fixed hub's front branch. An unprivileged user service
verifies the J700 identity and controller topology, then calls libnotify's
`notify-send` through its inherited session bus. There is no root-to-user
bus discovery, udev `RUN`, system service, or desktop-specific extension.
This works with GNOME Shell and with a notification server such as mako.

No kernel change is included. At the `a837bd5f9a21160256580aeb0829f6930440950c`
base, the public [J700 DT](https://github.com/aurora-silicon/linux/blob/a837bd5f9a21160256580aeb0829f6930440950c/arch/arm64/boot/dts/apple/t8140-j700.dts)
already describes the fixed VIA `2109:2122` hub and its connector graph:
USB downstream path `1.1` is front, `1.2` is rear. Only rear has the
SuperSpeed/DP PHY connection. The controller is `40a280000.usb`. USB bus
numbers, device addresses, and Linux `port0`/`port1` registration order
are deliberately not used to identify the connector. DT source labels
such as `typec1` are also independent of Linux Type-C class numbering.

The present firmware-managed
[tipd driver](https://github.com/aurora-silicon/linux/blob/a837bd5f9a21160256580aeb0829f6930440950c/drivers/usb/typec/tipd/core.c)
does not register discovered partner altmodes. Absence of those objects
therefore cannot establish whether a port or partner supports DP. The
notifier uses the board's explicit wiring, rather than inferring a missing
capability from missing sysfs objects.

A generic kernel improvement to expose firmware-discovered partner
capabilities could be upstream-plausible, separately: obtain actual
Discover SVIDs **and Discover Modes VDOs**, register/unregister partner
altmodes on connection changes, and report active state while preserving
firmware ownership of mode entry. A SVID alone is insufficient to invent
a mode descriptor. This needs public controller documentation and tests
for the specific controller variant, discovery failure, disconnect races,
and suspend. It is unnecessary for this hint. A `dev_info` on every PD
connection cannot distinguish a charger from a display adapter; a generic
tipd message naming J700's front/rear ports would mix board UI into a
controller driver. Neither is recommended here.

Upstream placement for a generic notifier would be a desktop/session
component backed by reliable connector capability and USB-to-Type-C
association data. This implementation is a small Aurora userspace feature
staged under `tools/`, not a proposed Linux kernel interface. Its J700
mapping belongs in a board support package until a generic interface can
replace it. Upstream acceptance has not been requested or established.

## Existing public implementations checked

Source review on 2026-10-16 was limited to public implementation files;
upstream agent/AI instruction files were not read. No implementation was
copied from proprietary sources.

* [Asahi tipd at 94fb23346d52](https://github.com/AsahiLinux/linux/blob/94fb23346d522edf53722357c426a3e58030beea/drivers/usb/typec/tipd/core.c)
  registers port DP/TBT altmodes and partner identity, but has no
  `typec_partner_register_altmode` call or Billboard desktop hint in that
  driver. This is a bounded driver inspection, not a claim about every
  Asahi package.
* [Linux Type-C class API](https://docs.kernel.org/driver-api/usb/typec.html)
  represents partner and port modes separately. Exposing real discovered
  modes is a driver concern; displaying a desktop notification is a
  session concern. The [upstream class implementation at 90201351a862](https://github.com/torvalds/linux/blob/90201351a862182bdabfc1ad1c97b1e1b6ed5931/drivers/usb/typec/class.c)
  recognizes the PD identity's Billboard capability as USB 2.0 capability;
  that handling does not send a desktop hint.
* [GNOME settings daemon at 4a75955f7481](https://github.com/GNOME/gnome-settings-daemon/tree/4a75955f7481e2bb19fa73fa67456e83d67e8563/plugins)
  had no Billboard/alternate-mode handler in the plugin C/header files
  searched. Its [USB protection notifications](https://github.com/GNOME/gnome-settings-daemon/blob/4a75955f7481e2bb19fa73fa67456e83d67e8563/plugins/usb-protection/gsd-usb-protection-manager.c)
  concern USBGuard authorization, not a connector's DP capability.
  This feature does not change those policies.
* [fwupd Analogix at 50181eeefa62](https://github.com/fwupd/fwupd/blob/50181eeefa622a68b123c2994dd4385fe1430765/plugins/analogix/README.md)
  uses selected Billboard devices for firmware updates. Its plugin is
  not a generic wrong-port display notifier. This tool never claims USB
  interfaces, reads BOS through libusb, or sends USB control requests.
* [systemd hwdb at f9d375fc3daf](https://github.com/systemd/systemd/blob/f9d375fc3daff274f7113d757e0cba59e7a332d4/hwdb.d/20-usb-classes.hwdb)
  names class `11` as Billboard. The standard rules import that descriptive
  metadata; they do not attach a front/rear display hint to it. The
  [udev contract](https://github.com/systemd/systemd/blob/f9d375fc3daff274f7113d757e0cba59e7a332d4/man/udev.xml)
  also excludes long-running desktop processes from rules.

## Matching and delivery

The rule matches `usb_device`, `bDeviceClass=11`, downstream device paths
`1.1` or `1.1.*`, and a common ancestor with USB ID `2109:2122`. The two
`ATTRS` matches refer to the same ancestor. The user process additionally
requires the exact `apple,j700` compatible token, a fixed hub at downstream
path `1`, the direct front child at `1.1`, and controller `40a280000.usb`.
Other machines and rear-port Billboards fail these checks. The rule's
property is a candidate marker, not an authorization or a generic DP ABI.

The service subscribes to processed udev events before enumerating, and
reconciles every two seconds. It also handles an adapter already attached
at login. All Billboard functions on the same front device instance share
one notification. Disappearance of just the Billboard function does not
reset deduplication while its parent dock stays attached.

State in `$XDG_RUNTIME_DIR/aurora-display-notifier/` records successful
delivery per USB instance (sysfs path, address, inode, udev initialization
timestamp), plus the last attempt time. It survives service restarts during
the login, contains no peripheral names/serials, and expires with the user
runtime directory. A lock prevents multiple instances for the same user.
Delivery attempts are at least 30 seconds apart. A rapid replug or a
temporarily missing notification server is retried while still attached;
unplugged pending events are discarded. There is a small unavoidable
crash window between notification acceptance and saving success.

Before sending, logind must report an active local graphical session
owned by the service's UID and attached to a seat. SSH, TTY, inactive
sessions, and logind failures cannot trigger delivery. If the service is
started before the desktop, it waits until that condition holds. It never
selects another user's bus. The user manager/session bus is per UID;
simultaneous graphical logins of the *same UID* share that bus and its
notification server, as they do for other user services.

Notifications have normal urgency and the server's default expiry. The
[standard notification protocol](https://specifications.freedesktop.org/notification/latest/protocol.html)
is used without actions or settings changes. Users can disable or mask
the service, or use GNOME/mako notification preferences normally.

Billboard means an alternate-mode failure, not proof of a connected DP
display. Thus the body is conditional on connecting a display. An adapter
advertising another failed alternate mode can also receive this truthful
port-capability hint. A display adapter that never enumerates a Billboard,
or only uses an interface-class Billboard with another device class, is
not detected. USB graphics such as DisplayLink are outside this feature.

## Install and enable

Runtime requirements: Python 3.9+, pyudev, libudev/udev, systemd with
logind and a user manager, libnotify's `notify-send`, and a session
notification server. For Fedora these are typically `python3-pyudev` and
`libnotify`; for Arch/Omarchy, `python-pyudev` and `libnotify` (plus mako or
another notification server). The staging Makefile expects GNU install.

Packagers can stage all files without running hooks:

```sh
make DESTDIR="$pkgdir" install
```

`PREFIX`, `LIBEXECDIR`, `UDEVDIR`, `USERUNITDIR`, and `DATADIR` are
overridable. No service is enabled by the Makefile. After an authorized
installation, reload udev rules with `sudo udevadm control --reload-rules`
and replug the adapter. Existing devices need a new processed add/change
event to obtain the marker; a rules reload alone does not retroactively
mark them. Avoid a global USB trigger.

For the desktop user:

```sh
systemctl --user daemon-reload
systemctl --user enable --now aurora-display-notifier.service
```

GNOME and [UWSM-managed Hyprland](https://wiki.hypr.land/Useful-Utilities/Systemd-start/)
supply `graphical-session.target`. A Hyprland session that does not manage
that target can execute the following commands from its startup and
shutdown hooks, respectively (use the syntax for the installed compositor
version):

```sh
systemctl --user start aurora-display-notifier.service
systemctl --user stop aurora-display-notifier.service
```

The user service still requires its inherited session bus and logind's
local graphical session. It requires neither DISPLAY/WAYLAND_DISPLAY
imports nor any guessed bus address.
Launching a compositor inside a logind session still classified as `tty`
does not satisfy the graphical-session check; use a properly registered
graphical login for delivery.

## Host verification

```sh
make test
dbus-run-session -- env AURORA_TEST_PRIVATE_BUS=1 PYTHONDONTWRITEBYTECODE=1 \
    python3 tests/integration_dbus.py
```

The unit tests use synthetic sysfs and udev objects. The second test needs
PyGObject/Gio and `notify-send`; it sends through a fresh private bus to a
mock notification server, never the desktop's real notification server.
It verifies the libnotify message, default expiration, and persistent
deduplication. Neither command requires J700 access or USB writes.

Validate a staged install on Linux with `udevadm verify` on the rules,
`systemd-analyze --user verify` on the generated service (with its staged
ExecStart path substituted for validation), and `desktop-file-validate`.
There is no kernel/DT change, so kernel builds, KUnit and dtbs_check do
not validate this userspace-only addition.

## Attended on-Neo acceptance protocol (not executed here)

The integration operator must obtain controller policy approval and a
session/resource lease explicitly identifying the J700 before any target
inspection, install, service start, or USB replug. Use that controller
session for every target command. Agree the plug sequence with the person
at the Neo. Do not infer that this document grants hardware access.

1. Record the candidate commit, installed paths and package version. Save
   the existing enable/mask/active status of this service and any previous
   versions of these four installed files for rollback. Confirm a J700
   identity, a local GNOME or Hyprland graphical login, logind `Active=yes`,
   `Remote=no`, matching `User`, and `Type=wayland` or `x11`. Confirm a
   notification server owns `org.freedesktop.Notifications` on that user's
   session bus. Preserve existing notification preferences; if Do Not
   Disturb hides banners, record that and check the notification history.
2. Install this userspace candidate and dependencies with the approved
   integration method, reload only these udev rules, then enable/start
   the user unit as above. Record `systemctl --user status
   aurora-display-notifier.service`. Do not change kernel/DT or display
   settings. Start `udevadm monitor --udev --property --subsystem-match=usb`
   and `journalctl --user -fu aurora-display-notifier.service` in the
   approved session, saving their output as private qualification evidence.
3. With the dock/display disconnected, plug it into **front**. Within a
   few seconds of Billboard enumeration, expect exactly one hint with the
   text above. Check the Billboard device's `bDeviceClass` is `11` and
   `udevadm info --query=property --path=<its sysfs path>` contains
   `AURORA_DISPLAY_UNSUPPORTED=1`. Verify ancestry is the fixed hub's
   `1.1` branch under `40a280000.usb`. Do not assume bus number `1` or
   Billboard address `1-1.1.5` stays constant. Leave it connected for at
   least 35 seconds: no second hint. Restart the user service: no duplicate.
4. Unplug/replug the whole dock in **front** to establish a fresh hint
   (the previous step waited over 30 seconds). Immediately unplug/replug
   again and keep it connected: at most one new hint, delayed until the
   30-second limit expires. Repeat the rapid unplug/replug cycle, but
   remove the dock during that delay: no stale hint after removal. Wait
   over 30 seconds from the last hint, then replug and keep it attached:
   one new hint.
5. Unplug and connect the same dock/display to **rear**. Expect no new
   notification from this service, even if a Billboard enumerates because
   of another failure. Verify rear USB 2.0 ancestry uses `1.2`, and check
   the external display works with the existing driver. A front-port hint
   already in desktop history is not a new rear-port event.
6. Test a plain USB data device and a charger in front: no hint. With no
   local active graphical session, test front Billboard enumeration: no
   delivery to SSH/TTY or an inactive user's session. On returning to the
   local graphical session, a still-connected eligible dock may produce
   the pending hint. Confirm user preferences and display configuration
   were not changed. Repeat steps 3 and 5 in GNOME and Hyprland/mako.
7. Save pass/fail evidence including failed attempts and the final user
   unit journal. Raw USB/session metadata remains private. Stop/disable
   the service if it was enabled only for qualification, restore its
   original enable/mask/active state and the previous files (or remove
   just the four newly installed files), reload rules and user units.
   Restart the restored service only if it was previously running. No
   kernel rollback is needed. Runtime state can be removed after stopping
   all instances if the operator wants to reset the hint for another test.

Acceptance requires both the visible front-port hint and absence of new
rear-port hints in each desktop. Host tests alone do not prove those
attended results or validate the physical topology on the running DT.
