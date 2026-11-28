.. SPDX-License-Identifier: GPL-2.0-only

==========================================
Apple display: the panel notch at run time
==========================================

A built-in panel with a camera housing has ``apple,notch-height`` rows at its
top; the boot loader fills that property from the hardware description (ADT).
The panel always runs its native timing. The driver offers each timing in two
geometries:

* **full**: the mode covers the whole panel, the notch rows included;
* **notch-free**: the mode is ``apple,notch-height`` rows shorter. The rows
  are folded into the vertical sync, so clock, totals and refresh rate are
  the full mode's. The frame is scanned out below the notch and the notch
  rows stay black.

The mode a commit carries decides what is scanned out. Both geometries are
always accepted, and a switch between the two geometries of one timing is no
mode set: the CRTC stays on and the display firmware is not asked for a new
mode.

The ``notch`` connector property
================================

Panels with notch rows have an enum connector property ``notch`` with the
values ``hidden`` and ``shown``. It selects the geometry compositors are
offered:

``shown``
  while a DRM master owns the device, the full modes are offered;
``hidden``
  the notch-free modes are offered.

The starting value is the boot loader's ``chosen/asahi,show-notch`` (``"1"``:
shown). A compositor can set the property in an atomic commit, usually
together with the matching mode. A change sends a hotplug event, so that
compositors and the console probe the connector again; a compositor that
follows hotplug events needs no further support.

Whenever no DRM master owns the device (a text-only system, a switch to a
text VT, a compositor that dropped master or exited), only the notch-free
modes are offered whatever the property says, and the in-kernel console
probes again and uses them: the console never draws under the notch. Its
surface is notch-free, and a probe limited to that surface is offered the
notch-free modes even while a compositor shows the full panel.

The same choice is ``/sys/class/drm/cardN-eDP-1/notch`` for scripts (see
``Documentation/ABI/testing/sysfs-class-drm-apple-notch``). Writing it commits
the property like a compositor would.

``/sys/kernel/debug/dri/N/eDP-1/notch`` shows the notch rows, the full
panel's rows, the policy, whether a DRM master owns the device, and the rows
above the last frame scanned out.

The physical height reported for the connector follows the offered
geometry: the panel's ``adj-height-mm`` for the notch-free one if the device
tree gives it, otherwise the full height scaled by the rows.

The standalone firmware 14.7 and 26.6 pipes build their one mode from the
boot framebuffer and keep the boot loader's geometry for the boot.
