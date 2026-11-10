.. SPDX-License-Identifier: GPL-2.0-only

:orphan:

Apple JPEG encoder
==================

The driver exposes one V4L2 memory-to-memory encoder per JPEG engine. It takes
full-range single-buffer NV12 (4:2:0) or NV16 (4:2:2) input and produces
baseline JPEG with the standard Huffman tables. Quality and restart interval are V4L2 controls.
Decode is not exposed.

The device compatible selects immutable register-layout and identity data:

===================  =========  ========================
Compatible           Layout     Masked HWREV
===================  =========  ========================
apple,t6000-jpeg      legacy32   0x0d0013
apple,t6041-jpeg      wide64     0x100008
apple,t8112-jpeg      wide64     0x0e0008
apple,t8140-jpeg      wide64     0x110000
===================  =========  ========================

Identification powers the domain, writes MODE=0x147 and reads the selected
layout's HWREV register. The mask is 0xffff0fff: bits 12 through 15 are ignored.
Zero, all-ones and other identities are refused. The legacy selector 0x0a0103
is not a HWREV. The wide64 variants share register and reset data; legacy32
uses compact 32-bit DMA addresses and a separate reset image. DMA addresses
and inclusive boundaries must fit the selected address width. They are never
truncated to fit a register.

The legacy32 reset image is complete: 111 static defaults, including the
sparse scratch block, plus the tiling write the helper appends. Unlisted
registers in that block are left unwritten. T6000 completes jobs with
interrupt status bit 31.

Each engine has a 16 KiB MMIO resource and a separate adjacent DART at SID 0.
The engine's resource must not include the DART window. Both T8112 engines
and their DARTs are enabled in ``t8112.dtsi`` (qualified on the J413), and
both T6000 engines in ``t6000.dtsi`` (qualified on the J316s). T6001 and T6002
stay disabled. Enabling nodes on another SoC requires qualifying both of its
instances.

KUnit
-----

``VIDEO_APPLE_JPEG_KUNIT_TEST`` checks layout sharing, register offsets,
HWREV admission, quantisation tables, 4:2:0 and 4:2:2 job programming, and
address/capacity rejection. No JPEG hardware is needed. An arm64 QEMU run can
use the configuration fragment next to the driver::

  tools/testing/kunit/kunit.py run --arch=arm64 --make_options LLVM=1 \
    --kunitconfig=drivers/media/platform/apple/jpeg/.kunitconfig \
    --jobs=8 'apple-jpeg*'

Qualification
-------------

Before enabling a new board, qualify both instances: record raw HWREV,
terminal interrupt status (bit 30 or bit 31 for initial older-family
qualification), reset/idle handshakes, runtime PM, geometry, output-capacity
checks and error recovery. T8140 and T6000 use their qualified bit-31
completion mask. The other variants accept bits 30 and 31; status/error and
output-capacity checks still apply.

Check each encoder's advertised formats and use its actual video node. For
example, with a GStreamer V4L2 JPEG encoder element available::

  gst-launch-1.0 -e videotestsrc num-buffers=1 ! \
    'video/x-raw,format=NV12,width=640,height=480,colorimetry=1:4:5:4' ! \
    v4l2jpegenc device=/dev/videoN ! filesink location=encoded.jpg
  ffmpeg -v error -i encoded.jpg -frames:v 1 -f null -

Repeat with both supported sampling formats, 1920x1080, non-MCU-aligned
heights, consecutive jobs, quality/restart controls and undersized capture
buffers. Record negotiated range/caps. Verify that failed reset polls and
watchdog timeouts stop DMA authority before returning buffers. The current
watchdog's stop-and-defer-reset behavior still needs a reset/power-cycle
recovery implementation before new production enablement.
