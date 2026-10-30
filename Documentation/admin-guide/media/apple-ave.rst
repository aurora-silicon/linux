.. SPDX-License-Identifier: GPL-2.0-only

Apple AVE video encoder
=======================

The AVE block in Apple T8140 (A18 Pro) SoCs, as used in the MacBook Neo,
encodes H.264 and HEVC. ``CONFIG_VIDEO_APPLE_AVE`` builds the ``apple-ave``
driver, which registers a V4L2 memory-to-memory stateful encoder following
Documentation/userspace-api/media/v4l/dev-encoder.rst. The OUTPUT queue
takes NV12 frames (64-byte aligned line stride), the CAPTURE queue returns
H.264 (Baseline, Main, High) or HEVC (Main) as Annex B.

Firmware
--------

The encoder runs firmware on its own coprocessor. The bootloader loads it into
two reserved, DART-mapped segments (TEXT and DATA) but does not start it. The
Aurora Stage 2 loader passes both regions in the device's ``memory-region``
property and records their CRC32 under ``/chosen`` as
``aurora,ave-text-crc32`` and ``aurora,ave-data-crc32``.

The driver only binds to firmware builds whose command formats it knows,
identified by the TEXT checksum, and only when DATA still matches the
checksum the loader recorded. It keeps a copy of DATA and restores it before
every firmware boot, so the firmware can be started again from the same state.

Session images
--------------

The two large session commands, Start and Process, are mostly constant for a
given firmware build. The driver writes every field that depends on the
session or the frame (sizes, rate control, GOP structure, levels, cropping,
buffer addresses, frame numbers and times) and takes the remaining constant
words from four files per firmware build::

  apple/ave-t8140-<text crc>-h264-start.bin     (0x3180 bytes)
  apple/ave-t8140-<text crc>-h264-process.bin   (0x63d8 bytes)
  apple/ave-t8140-<text crc>-hevc-start.bin     (0x13f28 bytes)
  apple/ave-t8140-<text crc>-hevc-process.bin   (0xb1c0 bytes)

Each file is a complete command image. Address and size fields in it are
ignored. Without these files, ``VIDIOC_STREAMON`` fails with ``ENOENT``.

Sessions and power management
-----------------------------

Each encoding session boots the firmware, opens and starts a session, encodes
and, at the end, drains, stops and closes the session and halts the firmware.
The next session restarts the firmware through the boot path. One session
runs at a time; a second one gets ``EBUSY``.

Up to five frames are inside the firmware at once (three without B-frames).
The firmware reorders B-frames itself, so CAPTURE buffers come back in
bitstream order, each carrying the timestamp of the frame it codes. The driver
asks for at least six OUTPUT buffers, and limits both queues so a full set fits
the encoder's 256 MiB DMA window (fewer buffers at larger sizes).

Runtime PM uses a two second autosuspend delay. A firmware that stops
answering keeps the encoder powered and is not used again until reboot.
Across system sleep an open session ends; the next one starts afresh.

Limits
------

Picture sizes from 320x240 to 2560x1600 (in steps of two), up to 240 frames
per second. Rate control is VBR (with ``BITRATE_PEAK`` as a one-second data
rate cap, twice the average when left at 0) or CBR; constant quality is not offered. ``BITRATE`` may change
while streaming and applies from the next frame; ``FORCE_KEY_FRAME`` makes
the next frame an IDR. H.264 uses either three B-frames or none (Baseline and
a GOP size of 1 never use them). HEVC always uses them for now; with
``B_FRAMES`` set to 0 every frame is flushed through at once instead. The level
control is a minimum: the driver raises it to what the size, frame rate and
bit rate need.
