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
while DATA still matches the recorded checksum, and keeps a copy of DATA that
it restores before every firmware boot.

No firmware files are needed. The driver builds every command itself,
including the two large session commands (Start and Process), from the
session settings and a set of constant fields known for one firmware build:
AppleAVE2FW 9003.78.0, TEXT checksum ``69c54454``. T8140 rejects an unrecognized firmware at probe. Probe-only devices may
bind with another loader-verified build, but starting a stream fails with
``ENOTSUPP`` and a message naming the checksum until its command layout and
hardware boot sequence have been qualified.

T6000 (M1 Pro) and T8112 (M2) have probe-only support in this driver. Their
device tree nodes and both encoder DARTs remain disabled. The firmware
handoff must reserve their preloaded TEXT (0xd0000 bytes) and DATA
(0x128000 bytes), preserve the ADT remap addresses and export both CRCs.
An enabled node with a valid handoff may bind, but encoding is refused with
an informational message until per-SoC command constants, MCC DSID and the
firmware CRCs have been qualified. The T8140 command constants are never a
fallback for these devices. Enabling a node alone does not enable encoding.

The shared Start layout uses a smaller LFS reference surface on both
pre-M4 devices: at 1920x1080 its exact descriptor size is 0x85200 for AVC
and 0x1fe00 for HEVC, before the DMA allocation is page rounded. The
``apple-ave-sizes`` KUnit suite checks these sizes and alignment boundaries.

T6041 (M4 Pro, including J616s reporting chip ID T6040) also has probe-only
support. Its TEXT and DATA reservations must be 0x14c000 and 0x12c000 bytes.
The existing AVE node and both DARTs remain disabled; the loader must publish
and preserve the live reservations and both pre-start CRCs before enabling
those nodes. A probe with a valid handoff reports the verified CRCs without
starting the coprocessor. An unknown layout refuses stream start before
allocating session buffers or acquiring runtime power.

Command sizes, offsets, descriptor groups, constants, Process subtype and
buffer sizes are selected by the verified TEXT CRC, independently of the
SoC match data. Only ``69c54454`` currently selects an encoding layout.
There is no fallback layout and zero is not a wildcard CRC. Hardware boot
qualification is a separate requirement; presenting the J700 CRC on T6041
does not authorize the J700 boot sequence.

The driver describes the command layout observed on J616s with macOS 27.0
26A428, including counted Start groups, packed AVC fields and sized Process
descriptors. It is deliberately absent from the CRC lookup: its TEXT CRC
and its relationship to the firmware supplied to Linux are still unknown.
The boot-config words, coprocessor start registers, DMA aperture and output
metadata/slice-header extraction also need qualification before encoding
can be enabled. The 26A428 capture and the J700 capture used different OS
versions; a SoC compatible cannot identify a command ABI.

The public API continues to offer 8-bit NV12 and VBR/CBR. CQ and Main10
layout facts are exercised by host tests without exposing new V4L2 modes.
The 26A428 layout has no proven fields for runtime bit-rate updates, forced
IDR or HEVC low delay. Such updates are refused rather than written at J700
offsets. Frame rates other than 30 fps and unobserved no-reordering modes
are also refused for that layout.

Host validation needs a C compiler with AddressSanitizer and UBSan and Python::

  python3 drivers/media/platform/apple/ave/tests/run.py

The tests compile the actual builder, allocation and reply functions with a
fake IPC/DMA boundary. They require no encoder, firmware or captured command
blocks. To compare all bytes of 288 synthetic J700 sessions against an older
checkout, add ``--compare-baseline /path/to/old/drivers/media/platform/apple/ave``.
Private capture comparisons must keep captured images outside the source tree
and mask only addresses, session tokens, clocks and host bookkeeping fields.

With debugfs mounted, ``apple-ave/last_start_h264``,
``last_start_hevc``, ``last_process0_h264`` and ``last_process0_hevc`` hold the
last Start command built and the first Process command of the last session,
for comparison with reference captures.

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
  (twice the average when left at 0), or CBR. In CBR mode, frames are padded
  with filler data once more than half a second of bit rate has gone unused,
  as macOS does; the driver adds the filler itself if the firmware does not.
  Constant quality is not offered. ``BITRATE`` may change while streaming and applies from the next
  frame; ``FORCE_KEY_FRAME`` makes the next frame an IDR.
- Either three B-frames or none (``B_FRAMES`` 3 or 0). H.264 Baseline and a
  GOP size of 1 never use them.
- The level control is a minimum: the driver raises it to what the size,
  frame rate and bit rate need.
- Buffers are mapped through the encoder's 256 MiB DMA window, so both queues
  are limited to what fits next to the encoder's working memory (fewer
  buffers at larger sizes).

T8112 temporary stage-1 diagnostics
---------------------------------

The 6070.11.1 lifecycle snapshots at 50 us, 1 ms and hello timeout include a
read-only software walk for the next reset instruction (TEXT VA +0x574) and
the first final-table store (linked DATA VA +0x20000). These diagnostics never
decide whether to start or recover firmware. A successful walk is not a
firmware hello, and an AP-visible zero after cache enable is not proof that
firmware failed to retire a store.

The temporary input geometry is the qualified reset TCR low word
``0x40808126``: 16 KiB pages, 26-bit TTBR0 input and a two-entry level-2 root
at DATA IOVA +0x1fff0. This is reset-derived geometry, not a host read of the
core TCR or TTBR registers; the final geometry words at DATA +0x60000 and
+0x60008 must not be substituted for the temporary TCR. Child addresses are
AVE output addresses (DART IOVAs), converted to offsets in the existing DATA
snapshot. Only DATA +0x18000..+0x1ffff is readable by this decoder.

Each access reports its translation, EL1 execute/write permissions and the
first failing descriptor's level, DATA offset and value. The decoder checks
validity, descriptor type, alignment, access flag, output address and
hierarchical/leaf permissions. Unsupported geometry is reported explicitly.
It issues no IOVA mapping operation, TLB invalidation, fault acknowledgment,
mailbox transaction or descriptor write.

The complete 4096-word temporary window is compared with the exact H14G reset
generator. It writes two root descriptors for the two-page ``_rtk_boot`` span,
then two consecutive read-only executable TEXT leaves, with all other words
zero. It does not linearly map the complete TEXT and DATA spans. Loader
linkage, page geometry, final geometry and table overrides are captured
independently. Nonzero table overrides or unsupported loader inputs mark the
model unavailable rather than reporting a match. Model availability never
replaces a decoded walk verdict. With unsupported address overrides the
displayed store VA is a candidate rather than a qualified first-store target.

The generator orders the first TEXT leaf, a system-wide barrier, the second
TEXT leaf and another barrier before enabling the temporary MMU. Consequently,
a snapshot containing both roots and only the first leaf proves that the MMU
enable was not reached. The first final-table store is invalid in this minimal
temporary tree by construction; that decode is a consistency fact, not proof
that execution reached the store.

The copies are bounded reads, not atomic snapshots of firmware writes. An
early partial table is evidence to compare with the later snapshots, never a
reason to suppress RUN or recovery. The IPS field and actual core register
contents are not independently captured by this diagnostic.

Only bounded summaries enter the kernel log. Root-only debugfs blobs
``temp_6070_50us``, ``temp_6070_1ms`` and ``temp_6070_timeout`` retain each
observed window; ``temp_6070_model`` retains the expected window. Each is
32768 bytes of little-endian 64-bit words, beginning at DATA +0x18000.
``last_6070_data`` retains the existing timeout DATA capture. Keep these raw
ledgers in private run evidence. They are replaced by the next lifecycle
attempt; collect them before starting another attempt. Never unload the
module after a firmware start; recovery/rollback requires the established
lifecycle or a reboot.

Synthetic decoder tests run independently of AVE hardware::

  ./tools/testing/kunit/kunit.py run \
    --kunitconfig=drivers/media/platform/apple/ave/.kunitconfig-temp \
    --jobs=8 'apple-ave-temp*'
