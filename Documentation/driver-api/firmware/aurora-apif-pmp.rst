.. SPDX-License-Identifier: GPL-2.0-only

Registry-v2 PMP guests
======================

The matched ``apple,t6050-pmp-v2`` firmware ABI uses 88-byte registry
descriptors, firmware-assigned 16-bit IDs, negotiated value buffers, and
update opcode ``0x36``. Linux validates descriptor sizes and names, rejects
duplicate live IDs, and bounds every shared-buffer copy. Boot arguments are
validated completely before any patch is written back. Board and calibration
values come from the boot description; no machine-name policy is embedded.

The existing T6000 protocol and T8140 42-bit DMA mask remain unchanged.
Registry-v2 configures a 48-bit mailbox DMA address mask and requires an
attached IOMMU domain before starting the processor. Its APIF DART description
must retain the firmware streams, aperture and final-page guard reported by
the producer. A missing provider must not permit direct-DMA fallback.

Probe accepts only the known cold context marker with the CPU stopped and no
retained DMA context. Unbind and reboot request RTKit idle, verify the matched
context-save marker/version and save counter, then clear CPU_RUN and check
its readback. Firmware allocations cannot be freed merely because RTKit
acknowledged a power state. A failed stop drains callbacks but retains both
shared RTKit backing and the PMP allocation owner until platform reset; it
also leaves the cold-reprobe check unable to admit the retained context.

The driver owns registry-v2 RTKit outside the callback context, avoiding an
Arc ownership cycle. The notifier is removed and drained before the RTKit
handle or context is released. Probe failure after firmware start follows
the same stop-or-retain ownership path. Endpoint-7 auxiliary buffers are
session-owned; identical requests reuse them rather than replacing live DMA.

This is support for the inspected firmware ABI, not a generic promise that
later PMP layouts use the same context registers. Hardware cold boot,
registry exchange, idle/save, reboot and unbind qualification remain pending.
