.. SPDX-License-Identifier: GPL-2.0-only

Apple AVE firmware ABIs
======================

The firmware TEXT CRC chooses the wire layout. SoC match data supplies register
addresses, boot arguments and device identity. These are separate from streaming
qualification: identifying a firmware does not qualify its encoder payloads.

9003.78.0 keeps its existing boot request, host clock synchronization, command
IDs, contiguous surface descriptors and reply envelope. The KUnit oracle in
``tests/ave-9003-reference.h`` freezes the driver builders and layout from Linux
commit 23e431a98962. Tests compare every byte of AVC and HEVC Start and Process
at 1920x1080 with B-frames enabled/disabled, and 3840x2160 with B-frames enabled.
They compare both deterministic synthetic DART addresses and masked images.
The constant image tables in ave-image.c are unchanged by this refactor.

6070.11.1 is selected for T6000 TEXT CRC 460c7a7c, T8112 TEXT CRC 25bfd039 or
T6021 TEXT CRC 0d56db87, with the corresponding SoC identity. T6021 runs a
larger build of the same version (TEXT 0xf0000, DATA 0x134000) with T8112's
registers and boot arguments. Boot, seven-channel discovery, Config,
Open, Close and scratch-confirmed Halt are implemented from the clean derived
SPEC-6070-lifecycle.md ledger. Start and ordinary Process writers implement the
clean revision-2/P1 field ledgers: fixed-capacity address banks, split output
arrays, four interleaved entropy sets, rational frame rate, one initial-QP word,
retained codec-state partitions and per-slot HEVC slice-header tables. The
session allocator provides distinct backing for every live bank and rejects
non-16-KiB-aligned IOVAs.

Streaming is enabled for up to two quality-first NV12 sessions per firmware
boot, each of 8-bit H.264 (Baseline, Main or High) or HEVC Main, with up to one
B picture per P picture, frame widths in whole macroblocks, at constant
quality, CBR or VBR. Main 10, speed priority, standalone LRME and Reset remain
unavailable and are refused. No Priority command is sent. Complete is the normal drain barrier; Flush is
reserved for cancellation before Stop and Close. KUnit checks the 1080p and 4K
fields directly from the clean ledgers and embeds no Apple command image.

T6021 differs in these points, each checked on a J475c; they are not from the
ledgers above. Config carries a shared area, its address at +0x60 and its size
at +0x68: with both left zero, as on T8112, the firmware never answers Config,
and with a 64 KiB, 16 KiB-aligned area both encoders answer Config, Open and
Start and encode. With boot device ID 20 and device type 17 both encoders boot
and answer the handshake. The TEXT word at 0x423c holds the low 32 bits of
DATA's device address on both encoders, and TEXT with that word cleared has
CRC 0d56db87.

Some 6070.11.1 settings were checked on T6021 (J475c) only. T6000 and T8112 run
the same firmware version and get the same settings, untested:

- Two sessions per boot. Four H.264 sessions, two on each encoder, ran in real
  time for five minutes at their target rates; a fifth STREAMON got EBUSY.
- H.264 P pictures as references. With the Process timescale word nonzero,
  every P slice had nal_ref_idc 0; with it zero, P slices have nal_ref_idc 1
  and frame_num counts up. The driver writes zero for H.264.
- H.264 motion lambdas, Start 0xff98 (full-pel search), 0xffa4 (inter mode
  decision) and 0xffa8 (intra mode decision). With all three zero, about 94% of
  P macroblocks were 8x8 partitions and constant quality 0 on a 720p lecture
  clip came out at 8.4 Mbit/s. With 64, 256 and 64, 8x8 partitions became rare,
  the same clip came out at 0.30 Mbit/s, and quality 50 took 1.8 times the bits
  of libx264 at the same PSNR.
- CBR and VBR. Rate-control mode 2, the 9003 CBR value, held the Start QP; mode
  1 moved slice QP with the bit rate. Both CBR and VBR submit mode 1. Without B
  pictures the coded rate stayed under the target, at 46% to 96% of it. For CBR
  the host pads to the target with filler data; with the filler, CBR at 1.0 and
  2.5 Mbit/s (720p) and 4.0 Mbit/s (1080p) came within 15% of the target over
  the file and over its last 150 frames.
- Integer frame rate. With a frame interval of 1000/30000 s every frame sat at
  the QP ceiling (about 0.31 Mbit/s coded for a 1.0 Mbit/s target), so the
  firmware is given the rounded rate over 1. The session keeps the client's
  timeperframe exactly as well, for the VUI timing.
- Frame rate while streaming. A new rate written into the Process frame-rate
  words stepped the HEVC picture clock backwards. Instead the next Process bit
  rate, and VBR's limit, are scaled by the Start rate over the new one; an HEVC
  stream at 30, then 15, then 30 frames per second kept every picture, its
  frames at 15 about 1.7 times the size.
- One frame at a time. B pictures default to off, and without them each frame
  is answered before the next is submitted. Every T6021 result above was
  measured this way; submitting frames ahead without B pictures was not
  measured on 6070.11.1.

6070 firmware writes the parameter sets to a buffer of their own. On T6021 the
SPS there had no VUI timing, and with B pictures off no VUI at all, so a raw
Annex B reader guessed 25 fps. The driver adds timing to an SPS from that buffer
that has none, from the client's timeperframe exactly, not from the integer
rate the firmware gets: H.264 num_units_in_tick the denominator and time_scale
twice the numerator, with fixed_frame_rate_flag; HEVC vui_num_units_in_tick the
denominator and vui_time_scale the numerator. The rate is taken in lowest
terms, so 30/1 gives 1 and 60 (H.264) or 1 and 30 (HEVC), and 30000/1001 gives
1001 and 60000 or 1001 and 30000. ffprobe on a raw H.264 stream prints the
field rate (60/1 at 30 fps, 60000/1001 at 30000/1001; an MP4 mux reports
30/1 and 30000/1001). When twice the numerator does not fit 32 bits, an even
denominator is halved instead; otherwise the SPS is left as written. An SPS
that has timing, or that the walk cannot follow, is left as written.

Lifecycle-only validation
-------------------------

Build with CONFIG_DEBUG_FS. On a supported 6070 device, write ``1`` (AVC) or ``2``
(HEVC) to ``/sys/kernel/debug/apple-ave-<device>/lifecycle_test``, where
``<device>`` is the encoder's device name. This synchronous operation
runs boot, Config, Open, Close and Halt, then requests runtime suspend. It does
not allocate encoding surfaces or issue Start, Process or uninit for an encoder
that was never initialized. A second write exercises a cold restart and DATA
restore. An existing session makes the operation return EBUSY.

The platform must supply the extra-0 register resource as ``extra0`` or register
resource index 3, with at least 0x24 bytes on T6000, 0x4000 bytes on T8112 or
0x3dfc bytes on T6021. The driver uses the translated platform resource address,
maps the 16 KiB pages that contain it through the device DMA API and puts that
dynamically allocated IOVA in Config. The base need not be page aligned: Config
then gets the IOVA of the base, not of its page. Platform DT/binding changes are
separate from this driver change.
No fixed firmware or register IOVA is used as an ordinary DMA allocation.

Before every boot the driver checks the firmware's IOMMU translations. DATA must
match the loader-exported pre-start CRC at probe; subsequent cold boots restore
that pristine image and verify its CRC. Firmware reservations and translations
remain persistent. IPC and extra-0 mappings are per-boot resources.

Halt clears scratch0, sends ID 14 without expecting a ring reply, and waits up to
five seconds for scratch0 0x08042006. Only then does the driver stop the IOP and
check control readback. Runtime PM performs the power transition; a second boot
is refused until suspend/resume has intervened. A missing Halt confirmation or
failed lifecycle leaves the firmware broken and retains DMA mappings, runtime
power and the module reference. A forced unload or in-place restart is not a
recovery procedure.

Hardware acceptance remains independent of KUnit: check both SoC VCBAR readbacks,
exact hello, ring dimensions, Config/Open/Close completion IDs and status, Halt
scratch confirmation, actual power gating and a second cold boot. Encoding on
9003 requires the existing hardware regression lane; these software tests do not
establish hardware equivalence.

The 9003 runtime recovery path retains its upstream quarantine and reset-witness
checks. It is deliberately unavailable for 6070: a broken lifecycle must keep
its mappings and power until scratch-confirmed Halt can establish quiescence.
