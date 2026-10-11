.. SPDX-License-Identifier: GPL-2.0-only

DCP display in an Aura guest
===========================

The firmware-27 display profile uses the normal IOMFB shared-memory
transport and compact AFK framing. It supplies a separate method/callback
table and present serializer. The existing queued present, framebuffer
retention, backlight policy, Type-C routing and suspend drain are shared
with the public driver.

This is display-controller support. It does not add AGX acceleration.

Loader contract
---------------

The loader leaves each coprocessor running with its firmware translations
intact. The driver negotiates RTKit without writing its CPU run state or
restarting the coprocessor. A missing running session or a firmware ABI
other than the explicit apple,firmware-compat = <27 0 0> is rejected.

The internal and external providers use apple,t6050-dcp and
apple,t6050-dcpext respectively. The profile uses a 41-bit DMA
aperture for the coprocessor CPU; scanout and PIODMA retain their independent apertures.

Each provider describes its inherited framebuffer region using
memory-region and the framebuffer entry in memory-region-names.
Default-surface allocation uses firmware-reported geometry, format and
compression. Unsupported layouts or allocations larger than that region
are rejected.

apple,clock-ids describes the ordered firmware provider clock table.
Its entry count must match clocks. An explicitly empty property is
accepted for a provider with no clocks; a missing property is rejected.
A callback clock index selects the corresponding Linux clock.

Display register callbacks normally use resource physical addresses.
If Linux resources are translated guest addresses, the loader supplies
apple,firmware-physical-regs in complete register-table order,
including the bandwidth scratch and optional doorbell apertures. These
addresses are returned to the coprocessor, not dereferenced by Linux.

The loader must also grant the reserved firmware buffers needed by AFK
through the corresponding IOMMU provider. Remote requests remain bounded
and fail if their device addresses cannot be translated.

Direct DisplayPort routes
-------------------------

Protocol-27 DPTX uses endpoint 0x2c and its own drive-settings records.
The explicit service unit takes precedence over the transport interface
identifier. Drive requests validate lane count and voltage/pre-emphasis
before changing the PHY.

The PHY reports its live clock and coding mode to the crossbar consumer.
A missing required provider or conflicting clock configuration fails the
link operation. Other PHY types retain their existing routing behavior.

Session lifetime
----------------

An adopted session has no confirmed DMA stop boundary. Failed probes and
unbind therefore retain published buffers, mappings and device/domain
references until a fresh firmware boot. Rebinding a retained session is
unsupported. No timeout falls back to resetting a live coprocessor.

Validation
----------

Native arm64 display-driver builds and host tests cover the present wire
layout and framebuffer sizing/bounds. These checks do not establish boot,
physical hotplug, brightness or suspend behavior on hardware.
