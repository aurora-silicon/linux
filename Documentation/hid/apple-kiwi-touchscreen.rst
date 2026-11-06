.. SPDX-License-Identifier: GPL-2.0

Apple ``kiwi`` touchscreen transport
====================================

The touchscreen in the K114 and K116 machines is an HID interface of the
Apple MTP DockChannel transport.  The interface announces itself as
``kiwi``.  Its active sensing input uses report ID ``0x5a`` and carries raw
sensor frames rather than finished contacts.

User-space ABI
--------------

The kernel exposes the HID device through the standard ``hidraw`` ABI and
passes input reports through unchanged.  It deliberately does not create an
input multitouch device.  Turning sensor matrices into contacts requires
per-machine calibration, baseline tracking, multi-part frame assembly, seam
mapping, palm rejection and path tracking.  Those policies do not belong in
the transport driver, and the available clean-room description does not
define enough stable fields for an in-kernel contact parser.

A user-space implementation must treat dimensions and lengths in each frame
as untrusted.  It must reject truncation, arithmetic overflow, impossible
dimensions and packets extending beyond a report.  A rejected or
discontinuous frame must release all active contacts instead of retaining
stale state.  The resulting virtual device should use Linux multitouch
type-B slots and ``INPUT_PROP_DIRECT``.

Firmware and power boundary
---------------------------

The child node names one installer-generated firmware container.  The
DockChannel driver sends its payload with the transport's generic firmware
command.  Only the existing ``multi-touch`` trackpad interface receives the
trackpad-specific post-load reset sequence.  Loading another interface never
implies a power transition.

The ``kiwi`` nodes remain disabled.  Enabling them requires hardware evidence
for the complete control, acknowledgment, suspend, resume and wake sequence.
The driver must not guess those operations from trackpad behaviour.

Installer inputs
----------------

The installer must generate ``apple/kiwi/firmware.hidf`` for the individual
machine.  It must obtain the following inputs without placing their values in
the kernel tree:

* the licensed firmware payload selected for the machine's exact board;
* every per-machine FDR property referenced by the firmware manifest,
  including its original type and byte length, for substitution before the
  container is installed;
* the manifest's requirement and target metadata needed to omit records that
  do not apply to that machine;
* the panel's active area, orientation, part/target layout, seam transform and
  non-touchable regions for the user-space calibration file; and
* per-cell calibration, invalid-cell masks and calibration provenance needed
  by the user-space sensing service.

Missing FDR properties, type mismatches and length mismatches are fatal to
container generation.  The installer must not substitute zeroes or values
from another machine.  Per-unit values belong only in the installed firmware
and calibration artifacts; they must not appear in logs, public bug reports
or source control.
