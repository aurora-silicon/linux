.. SPDX-License-Identifier: GPL-2.0-only OR MIT

T6041 internal panel
===================

Desktop 26.6 firmware uses its own IOMFB instance. Panel brightness is in
integer nits from zero to the panel ceiling, with the same 250 ms fade used
by the H17P panel. The backlight interface reports the requested target and
``actual_brightness`` follows accepted, completed presents. A registration
default does not change the brightness inherited from the boot loader.
Until a loader hint or a completed user request supplies a known level,
``actual_brightness`` reports the target. Echoing the unchanged registration
value also leaves the panel alone while its level is unknown. A request for a
different target takes control. Once a hint or completed request establishes
the level, all targets are usable. Framebuffer unblank and core suspend
notifications also preserve an unknown inherited level.
The first explicit change fades from a known inherited level, or from the
reporting target when no hint is available. Firmware reports are observations,
not requests. After takeover they cannot replace the target, restart a fade or
cause a brightness write. Actual readback follows accepted, completed presents.
Ambient-light policy can use the normal backlight interface, just as a slider
does. Requests made while blanked or suspended preserve the restore target.

Fixed refresh modes keep the timing and color IDs supplied by DCP. The driver
does not synthesize lower refresh modes from a 120 Hz mode. Fractional rates
remain in 16.16 Hz until the DRM pixel clock is calculated. A ProMotion mode
needs desktop presentation deadlines even with adaptive sync disabled.

The DT panel compatible ``apple,panel-j616`` identifies the ProMotion panel and
enables adaptive sync support without a module parameter. The connector capability
requires the desktop firmware profile, the internal main display, and a valid
adaptive-sync range. Other panel compatibles do not acquire this capability. The
existing internal ProMotion convention supplies 24 Hz to the mode rate when
the firmware omits both range fields or reports both as zero placeholders;
a partial, duplicate or invalid range
leaves the timing usable at fixed refresh.

A rejected adaptive-sync parameter or mode selection gets one retry with
adaptive sync disabled. An adaptive swap that stops completing invokes the
swap watchdog and re-applies the active CRTC at fixed refresh. Either failure
latches adaptive sync off until reboot. A transport that stops replying cannot
be recovered by issuing another command on its busy channel; it still requires
a reboot. Use the previous kernel entry if a panel fails qualification.

Panel identity
--------------

The desktop internal panel has no external EDID service. Its ProductAttributes
supply the monitor identity, independently of optional colorimetry or timing
fields. The driver publishes an EDID using that product data, the panel dimensions
from the device tree, and a representable DCP timing plus catalog limits.
The EDID clock rounds to its required 10 kHz units. DRM still uses the exact
firmware timing list; the EDID adds no synthetic modes or color capabilities.

Compact powerlog reports may be bare serialized dictionaries or carry a command
descriptor. The receiver preserves the serialization header before handing the
report to systemep. Only the first valid report before Linux control can seed
takeover; later reports confirm output without changing the requested target.

Desktop mode changes use the D202 and D454 adapters for this firmware's records.
The callback table also includes physical mapping and the empty resize
notification. An unknown callback during a modeset gets a bounded unsupported
reply. Its enclosing command must ACK before the set fails, so a subsequent
mode change can reuse the drained channel. Accepted atomic modeset errors are
reported through the out-fence. A timed-out command retains its channel until
the firmware replies; another set cannot overwrite its stack.

Repeated or partial DisplayAttributes retain a validated fixed-panel identity.
They do not emit another identity hotplug event or revoke the EDID.

Identity accepts both chunked DisplayAttributes and the desktop dictionary
setters. A legacy PNP manufacturer code is decoded when the text code is absent.
Missing optional labels are represented by the firmware vendor/product code and
unspecified numeric serial, without inventing a per-device identity. Partial
updates for the same vendor/product retain known optional identity fields.

Desktop identity diagnostics and fallback
-----------------------------------------

Identity inspection uses the existing DRM KMS debug category (``drm.debug``
bit 2), with no driver-specific switch. When that category is enabled, D552, D561,
D114 and chunked property delivery report the outer key, serialized key names,
value type numbers and lengths. D000 identifies chunked delivery. Type numbers
are dictionary 1, array 2, integer 4, string 9, blob 10 and boolean 11. Values
are never printed. Each property walk stops at depth 4 or 64 objects, and the
DCP stops after 128 diagnostic records per callback class (512 total), with bounded
inspection-status lines in addition. Atomic reservations include framing records. D552, D561, D114 and chunked delivery have
separate budgets so periodic statistics cannot hide the panel setters. Service
dictionary setter D413 is inspected and can supply identity or a raw EDID too.
Scalar setters D102/D104/D414/D415/D563/D565/D567 report keys and types at
ingress when their public wire lengths match. They share the
fourth budget with chunked delivery. Null scalar values report size zero; no
scalar value or service handle is printed, and acknowledgements are unchanged.
When KMS debugging is disabled no inspection budget is consumed. Malformed
serialization only stops inspection.

A real EDID with a usable identity takes priority. It may arrive as raw bytes,
a serialized blob, or an ``EDID``, ``edid`` or ``IODisplayEDID`` blob nested
in a dictionary. Checksum-valid blocks without a usable manufacturer, model
and serial are rejected so they cannot reintroduce null userspace strings. Without
one, ProductAttributes remain usable, but are no longer a prerequisite for
publishing an identity. The fallback reads the original ADT from m1n1's
reserved-memory ``phram`` node with ``label = "adt"``. The reader accepts at
most 16 MiB, 4096 nodes, 4096 properties per node and 32 levels, and validates
all extents before committing any identity data.

The supported ADT identity properties are as follows. These are property
mappings, not a claim that a particular firmware supplies every property.

* ``/panel`` or ``/product/panel`` ``vendor-id`` or ``display-vendor-id`` is a
  little-endian 32-bit packed PNP manufacturer code.
* ``product-id``, ``display-product-id`` or ``panel-id`` supplies the low 16
  bits of the EDID product code.
* ``display-serial-number`` supplies a little-endian numeric serial.
  ``serial-number`` supplies panel serial bytes, hashed with FNV-1a into the
  numeric EDID serial. ``raw-panel-serial-number`` on ``/product`` or a panel
  node is handled the same way. Machine serials from other nodes are ignored.
* ``width-mm`` and ``height-mm`` supply bounded physical dimensions. Missing
  dimensions retain the OF panel node's ``width-mm`` and ``height-mm`` (or
  ``adj-height-mm`` when the notch is hidden). The full height is read first,
  so a missing optional adjusted height cannot leave the height at zero.
  J616s has a 345x223 mm full panel and a 345x216 mm notch-hidden viewport.
  The connector retains these exact millimetres after the EDID update.

A successfully parsed ADT does not necessarily contain a panel node. In
particular, a machine-level ``/product`` ID and ``/arm-io/disp0`` timing data
are not panel identity properties. They do not override OF panel dimensions
or prevent the generic fallback from being published.

If the ADT or its identity fields are absent, the desktop J616 fallback is
manufacturer APP, product 0616, name Apple Panel and unspecified numeric
serial zero, represented by the non-empty serial descriptor ``unspecified``.
This generic identity does not claim to identify an individual panel. The
preferred representable timing and range limits come from the retained DCP
TimingElements catalog. Exact mode enumeration continues to use that catalog.
For notch-hidden modes, the synthetic identity DTD retains the clock, active
area and total blanking but bounds the vertical front porch and sync pulse to
EDID's six-bit fields. It is an identity summary, never an enumerated or
programmed mode; the original DCP sync positions and mode IDs are unchanged.
This also lets a 118-line porch and 82-line notch-expanded pulse publish an
identity instead of failing the base EDID size check.
The EDID records model year 2024 (not a manufacture date) and a nominal D65
reference white point required by EDID parsing. When a complete valid optional
firmware colour description is available, the shared encoder supplies its
primaries, white and gamma instead; see :doc:`apple-dcp-panel-colorimetry`.
Missing colour data does not imply calibrated primaries or gamma. No new mode
or machine serial is claimed. The ADT reader,
EDID fallback and real EDID setters are restricted to desktop 26.6; T8140
continues to use its existing path.

Completed desktop brightness changes emit a backlight change uevent and notify
both ``brightness`` and ``actual_brightness``. The work runs outside the receive
path's locks and is drained during shutdown. Takeover sends the same event when
it updates the inherited level. Requested brightness remains the user's target
through the fade; notifications do not replace it with an intermediate level.

DT capability and boot-loader handoff
-----------------------------------

The J616 panel's existing DT compatible supplies adaptive-sync capability;
there is no T6041 VRR module parameter. Firmware and mode validation, the main
panel restriction, and the fixed-refresh failure latch still apply. EDID and
backlight notifications have no driver-specific tunables.

The kernel does not consume ``chosen.asahi,t6041-dcp``. Its early T6041 gate
already follows the handoff published by m1n1: ``apple,t6041-handoff = <1>`` on
DCP, display and PIODMA, firmware compatibility 26.6.2, the expected DART stream
and mailbox relationships, initially disabled nodes, and a running DCP.
The DCP driver separately validates register resources and the firmware memory
handoff. Missing or invalid handoff data leaves the boot framebuffer in place.
These checks must remain when the loader's policy changes.

The current m1n1 producer uses that chosen property to decide whether to attempt
publication. Removing it cannot be completed by a kernel change: a loader with
no published handoff provides nothing for the kernel to adopt. The producer
needs to select the T6041 handoff from its existing chip/board match and the
supported DCP/display/panel DT compatibles, then run the existing ADT, power,
running-session, firmware-segment, reservation and live DART mapping checks
without a chosen opt-in. Only a successful transaction may publish the existing
markers, memory regions and firmware properties. Keeping nodes disabled until
the kernel accepts that contract preserves compatibility with older kernels.
A failed transaction must publish no partial handoff. Loader host tests should
cover supported DT without the chosen property, an unsupported board/DT, and
each failed validation, as well as unchanged other-SoC policy. No new policy
flag or kernel bypass is needed. This kernel series does not replace Stage 2.


Desktop external audio (draft)
------------------------------

The desktop audio integration uses the existing DCP PCM and native link
lifecycle, with a separate compact service binding on endpoint 0x2b. Endpoint
0x29 retains AV device control. Only the external firmware role admits the
``dcpav-audio-epic`` unit-zero service; the internal panel does not acquire an
audio client. The existing ``hdmi_audio`` opt-in still controls service startup.

A PCM opens and pins one service instance. Prepare, Start, Stop and Complete
use that instance's element link data. Sink termination disconnects the card,
drains the DMA and completes its link before closing that instance. Replacement
services cannot inherit an old instance's Close or link completion. Compact
method replies must have the expected body length; serialized property replies
must contain their declared used bytes within the actual received body.

J616s DPA1 and DPA2 are described with SIO channels 0x66 and 0x68, connected
bidirectionally to DCPEXT0 and DCPEXT1 port zero, respectively. Both clients
remain disabled. This description does not enable SIO, its DART, its mailbox
or any DPA power domain. A generation-specific SIO firmware backend and loader
handoff are still required before a candidate may enable the clients.

The desktop service name/unit, shared 13.5 command numbers and native 14.7 link
and element layouts are provisional integration choices. Build and synthetic
regression results cannot qualify them against firmware. Qualification must
confirm those contracts and actual sink element catalogs before enabling these
outputs. No sink-specific rates or capabilities are synthesized here.
