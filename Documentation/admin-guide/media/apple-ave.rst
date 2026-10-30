.. SPDX-License-Identifier: GPL-2.0-only

Apple AVE video encoder
=======================

The AVE block in Apple T8140 (A18 Pro) SoCs, as used in the MacBook Neo,
encodes H.264 and HEVC. ``CONFIG_VIDEO_APPLE_AVE`` builds the ``apple-ave``
driver, a V4L2 memory-to-memory stateful encoder following
Documentation/userspace-api/media/v4l/dev-encoder.rst. The OUTPUT queue
takes NV12 frames, the CAPTURE queue returns H.264 (Baseline, Main, High) or
HEVC (Main) as Annex B.

Usage
-----

The encoder shows up as a ``/dev/videoN`` node named "Apple AVE video
encoder". GStreamer's ``v4l2h264enc`` and ``v4l2h265enc`` and ffmpeg's
``h264_v4l2m2m`` and ``hevc_v4l2m2m`` work with it, for example::

  gst-launch-1.0 -e videotestsrc num-buffers=300 ! \
    video/x-raw,format=NV12,width=1920,height=1080,framerate=30/1 ! \
    v4l2h264enc extra-controls="controls,video_bitrate=8000000" ! \
    h264parse ! filesink location=out.h264

  ffmpeg -i in.mkv -pix_fmt nv12 -c:v h264_v4l2m2m -b:v 8M -g 30 out.h264

Up to two streams can be encoded at the same time.

Firmware
--------

The encoder runs firmware on its own coprocessor. The bootloader loads it into
two reserved, DART-mapped segments (TEXT and DATA) but does not start it. The
Aurora Stage 2 loader passes both regions in the device's ``memory-region``
property and records their CRC32 under ``/chosen`` as
``aurora,ave-text-crc32`` and ``aurora,ave-data-crc32``. The driver only binds
to firmware builds whose command formats it knows, identified by the TEXT
checksum, and only while DATA still matches the recorded checksum. It keeps
a copy of DATA and restores it before every firmware boot.

Session images
--------------

Most words of the two large session commands, Start and Process, are
constant for a machine and its firmware. They are not part of the kernel.
The driver loads them from four files named after the machine (the first
root compatible without its vendor prefix, ``j700`` on the MacBook Neo)::

  apple/ave-j700-h264-start.bin     (0x3180 bytes)
  apple/ave-j700-h264-process.bin   (0x63d8 bytes)
  apple/ave-j700-hevc-start.bin     (0x13f28 bytes)
  apple/ave-j700-hevc-process.bin   (0xb1c0 bytes)

The files are extracted from the user's own macOS installation by the
Aurora firmware extractor, in the same way as other firmware the machine
needs, and installed under the firmware search path (usually
``/lib/firmware``). No Apple data is shipped with the kernel. The driver
writes every field that depends on the session or the frame itself, so the
same files serve every size, profile and rate. A missing or wrong-sized
file makes ``VIDIOC_STREAMON`` fail with a message naming it.

Sessions and power management
-----------------------------

The firmware boots when the first stream starts and is halted when the last
one ends, as macOS does; the next stream starts it from reset. Runtime PM
powers the encoder down two seconds after the last stream.

Up to five frames per stream are inside the firmware at once (three without
B-frames). The firmware reorders B-frames itself, so CAPTURE buffers come
back in bitstream order, each with the timestamp of the frame it codes.

Across system sleep, open streams are drained, closed and the firmware is
halted. On resume each stream reopens on its next frame, which starts with
an IDR and fresh parameter sets; clients see no error.

Every wait for the firmware is bounded. A command or frame that gets no
answer within 5 seconds (Start: 10 seconds) fails the stream with ``EIO``;
a firmware that stops answering keeps the encoder powered and is not used
again until reboot. The driver polls the firmware rings with sleeping waits.

Limits
------

- Picture sizes from 320x240 to 2560x1600 in steps of two, up to 240 frames
  per second.
- The encoder reads NV12 with a 64-byte aligned line stride, which
  ``VIDIOC_S_FMT`` reports. Clients that fill buffers with a smaller line
  size (ffmpeg does, for widths that are not a multiple of 64) are detected
  from the payload size and their frames copied to the encoder's layout.
- Rate control is VBR, with ``BITRATE_PEAK`` as a one-second data-rate cap
  (twice the average when left at 0), or CBR. Constant quality is not
  offered. ``BITRATE`` may change while streaming and applies from the next
  frame; ``FORCE_KEY_FRAME`` makes the next frame an IDR.
- H.264 uses either three B-frames or none (Baseline and a GOP size of 1 never
  use them). HEVC always uses them; with ``B_FRAMES`` set to 0 every frame is
  flushed through at once instead.
- The level control is a minimum: the driver raises it to what the size,
  frame rate and bit rate need.
- Buffers are mapped through the encoder's 256 MiB DMA window, so both queues
  are limited to what fits next to the encoder's working memory (fewer
  buffers at larger sizes).
