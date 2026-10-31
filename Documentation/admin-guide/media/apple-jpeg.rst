.. SPDX-License-Identifier: GPL-2.0-only

:orphan:

Apple JPEG scaffold
===================

This driver provides a common platform and V4L2 memory-to-memory scaffold
for the Apple T6041 and T8140 JPEG engines. Each DT platform device owns one
independent m2m scheduler, with formats and VB2 queues allocated per open file.
T6041's nodes reside in ``t6040.dtsi`` in this source tree. All JPEG engine
nodes and the new T8140 DART remain disabled. SoC DART defaults are disabled;
existing board overrides are preserved. In particular, J700 already enables
its first JPEG DART independently of the disabled codec engines.

No target HWREV is qualified. Runtime resume reads HWREV with the power domain
active and returns ``-ENODEV`` for an unlisted revision, including zero and
all-ones readbacks. Both per-compatible allow-lists are deliberately empty.
The driver does not infer a revision from a compatible, an instance number,
an ADT hw-type, or a revision observed on another SoC.

Identification precedes reset: the public reset sequence has not been qualified
on these targets, so issuing it before admission would undermine the refusal
boundary. There are no MMIO writes, interrupt requests, reset sequences or DMA
jobs in this scaffold. Probe failure balances runtime PM and never registers
a video node. The power domain and the IOMMU are managed by their normal kernel
providers; the scaffold requires a SID-0 IOMMU attachment and the binding's
exact high DMA window. This is a topology check, not proof of pointer aliasing.

The future video node has JPEG/NV12M format negotiation for encode and decode,
buffer allocation/import, and queue teardown. STREAMON returns ``-EOPNOTSUPP``
and returns queued buffers according to the VB2 failure contract. The scheduler
also refuses jobs. The advertised allocation dimensions and formats are draft
API scaffolding, not a tested codec capability. Open video files retain the
m2m/device allocation through final close after platform removal; they cannot
access removed MMIO resources.

Before enabling nodes or adding a target revision, qualification must establish
the revision, bounded reset/idle sequence, interrupt acknowledgment/deassertion,
high DVA alias, timeout/recovery and supported geometry on both instances of
each SoC. Filling an HWREV list alone does not implement streaming. Reset and
job code must validate revisions again on every runtime resume, preserve the
shared power-domain accounting, and return DMA buffers only after quiescing
hardware. Encode/decode controls, JPEG header validation, media-controller
topology and actual job programming belong to the subsequent codec work.

The HWREV register offset comes from the MIT-licensed Asahi Linux m1n1 register
description at commit ``f3ac615eb1376d3bbba24cd84c414b9e41892adb``:

https://github.com/AsahiLinux/m1n1/blob/f3ac615eb1376d3bbba24cd84c414b9e41892adb/proxyclient/m1n1/hw/jpeg.py

The scaffold deliberately does not translate the experiment's unqualified
reset or interrupt writes. ``VIDEO_APPLE_JPEG_KUNIT_TEST`` tests empty target
allow-lists, invalid readbacks and exact matching with synthetic revisions.
