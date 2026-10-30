.. SPDX-License-Identifier: GPL-2.0-only

Apple AVE video encoder
=======================

The AVE block in Apple T8140 (A18 Pro) SoCs, as used in the MacBook Neo,
encodes H.264 and HEVC. ``CONFIG_VIDEO_APPLE_AVE`` builds the ``apple-ave``
driver, which registers a V4L2 memory-to-memory stateful encoder following
Documentation/userspace-api/media/v4l/dev-encoder.rst. The OUTPUT queue
takes NV12 frames (64-byte aligned line stride), the CAPTURE queue returns
H.264 or HEVC.

Firmware
--------

The encoder runs firmware on its own coprocessor. The bootloader loads it into
two reserved, DART-mapped segments (TEXT and DATA) but does not start it. The
Aurora Stage 2 loader passes both regions in the device's ``memory-region``
property and records their CRC32 under ``/chosen`` as
``aurora,ave-text-crc32`` and ``aurora,ave-data-crc32``. No firmware file is
requested.

The driver only binds to firmware builds whose command formats it knows,
identified by the TEXT checksum, and only when DATA still matches the
checksum the loader recorded. It keeps a copy of DATA and restores it before
every firmware boot, so the coprocessor can lose power and start again from
the same state.

Power management
----------------

The firmware boots when the first encoding session starts. Runtime PM uses a
two second autosuspend delay. While the firmware runs the encoder stays
powered; across system sleep the power domains go down, open sessions fail
their next frame, and the next session boots the firmware again.

Status
------

Firmware start-up, the command rings, session open/close and the V4L2
interface are implemented. Session start and per-frame encoding are not yet
wired up, so ``VIDIOC_STREAMON`` on both queues currently fails with
``EOPNOTSUPP``.
