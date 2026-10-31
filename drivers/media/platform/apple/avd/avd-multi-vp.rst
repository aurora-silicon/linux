.. SPDX-License-Identifier: GPL-2.0-only OR MIT

Open firmware multi-VP queue
===========================

The open backend discovers avd-fw ABI 2 with multi-VP metadata after CM3 boot. Match data describes
both the base register index and engine count for each codec pool. Firmware
selects a free VP from that pool; the host submits a tagged instruction program and
receives a tagged PP completion. The host never handles a queued VP-to-PP kick.

The total admitted slot count is the minimum of AVD_MAX_FRAMES, match-data
fifo_slots and the backend limit. Slots are allocated only from this range,
so frame index equals hardware FIFO index. There are at most 7 independent
FIFOs on T8103/T8140 and 15 on other supported parts. Each open slot gets its
own instruction FIFO storage and program buffer, retained until retirement.
These buffers are allocated lazily; FIFO storage uses the existing 12-MiB size.

The firmware image names select both register addressing and engine pools.
T6020 selects avd-fw-v3-t2 (non-LSR FIFO addressing, existing v3t1 tuning).
T6040 selects avd-fw-v5-t2 (its SRAM aperture and v5t2 tuning). Its full-size
VP counts remain an inference requiring device validation. Codec capability
restrictions in match data remain authoritative.

ABI and dependency contract
---------------------------

The wire layout is in avd-open-protocol.h and in avd-fw src/queue.h.
ABI 2 preserves the original 64-byte prefix and adds SRAM-ring flags and
legacy PP continuations. Bit 31 of root codec capabilities and an AVM1 block at
SRAM +0x7f80 negotiate multi-VP metadata. Descriptor flag bit 31 reinterprets
continuation/tail words as context/dependency, FIFO IOVA and full PP count.
Original ABI 2 callers retain their first/continuation PP words and FIFO setup.
Images without a matching ABI/aperture/geometry retain the legacy transport.

The native queue streams words through the advertised power-of-two SRAM ring;
no PIODMA mapping or second DART stream is required for program delivery.
The decoder's ordinary DMA domain and buffer lifetime rules remain mandatory.
A bounded producer pump runs from queue work until long programs are fully
published. Invalid ring counts, rejected entries or fatal notifications reset
the whole device before mappings can retire. Geometry and u32-wrap tests cover
the 4096-word ring, which streams programs longer than its capacity.

Firmware dispatch only sees normalized work and a source-readiness callback.
PIODMA tests can bypass unrelated blocked entries, while the SRAM source preserves
global word order before reserving a VP. Both sources share VP pools, context
dependencies, serialized PP and completion handling.

Codec prepare defaults to AVD_DEP_PREV_DONE. H.264 and HEVC keep this policy:
VP-stage reference independence and mutable scratch safety are not proven.
Different contexts can run on different VPs. Independent same-context work
may opt into AVD_DEP_NONE only after proving those properties. Firmware also
checks context dependencies and serializes PP. Both endpoints preserve
submission order within each context when retiring completions.

For open multi-VP work, prepare fills cmd[0] with the codec and cmd[1] with the number
of PP EXECs, which can differ from VP instruction-program segments. Every program EXEC uses f->idx as its FIFO index.
The compatibility CPU builder supplies these fields for existing codecs.
A completed picture releases the m2m scheduler job as soon as the slot owns
its buffers; earlier HOLD_CAPTURE_BUF slice requests complete immediately.

A fatal firmware error or malformed completion schedules the whole-device
reset before buffers can retire. Error records alone do not prove DMA has
stopped. The existing per-frame watchdog and streamoff/reset paths still own
recovery. No queue-depth module parameter or runtime policy override is added.

Host validation
---------------

With an avd-fw checkout at the matching ABI revision::

  sh drivers/media/platform/apple/avd/tests/run-host.sh /tmp/avd-host-test
  sh drivers/media/platform/apple/avd/tests/run-multi-vp.sh /path/to/avd-fw /tmp/avd-multi-vp-test

The second test links the real host slot helpers and the real firmware
scheduler. It checks two contexts on distinct VPs/FIFOs across all eight image
tables, reverse completion across contexts, ordered completion within a context,
and the 7/15 FIFO caps. Firmware's Unicorn transport tests additionally execute
the actual CM3 machine code and IRQ wrappers with a synthetic MMIO/PIODMA model.
KUnit covers core admission, FIFO/backend limits and existing tag/retirement
helpers. These tests establish software dispatch behavior, not pixel decoding.

Build the driver and KUnit objects with ARCH=arm64 LLVM=1 W=1 WERROR=1 in a
controller build slot. Enable VIDEO_APPLE_AVD and VIDEO_APPLE_AVD_KUNIT_TEST,
with their media-controller, platform-driver and KUnit dependencies.

Device commands for the lead
---------------------------

Run these command bodies only through controller-approved requests naming the
specific device. First install the matching kernel, DT SRAM resources and a matching multi-VP image through the normal reversible packaging workflow. Preserve
the previous package and bootguard fallback. Use a licensed local input clip.

Identify the video/media pair belonging to the same AVD instance::

  v4l2-ctl --list-devices
  v4l2-ctl -d /dev/videoN --info --list-formats-out

Set VIDEO and MEDIA to that explicit pair, BENCH to the avd-bench executable,
CLIP to the test stream, and TESTS to this driver's tests directory. Each
process opens its own context on that same pair. Run the exact N=1,2,4 cases::

  sh "$TESTS/run-multi-stream.sh" "$BENCH" "$VIDEO" "$MEDIA" "$CLIP" 1 /tmp/avd-multi-vp-n1
  sh "$TESTS/run-multi-stream.sh" "$BENCH" "$VIDEO" "$MEDIA" "$CLIP" 2 /tmp/avd-multi-vp-n2
  sh "$TESTS/run-multi-stream.sh" "$BENCH" "$VIDEO" "$MEDIA" "$CLIP" 4 /tmp/avd-multi-vp-n4

Each stream uses avd-bench --queue 4 --passes 6 with separate JSONL, stderr and
per-frame hash files. The script creates a fresh directory and preserves all
failures. Repeat with distinct fresh output names for H.264, HEVC, VP9 and AV1
where supported, including 8/10-bit output. The benchmark's mandatory software
pixel comparison must pass before interpreting throughput. Aggregate wall time
and per-stream fps separately; concurrent process startup is not synchronized.

Also exercise streamoff with another context active, watchdog/reset, repeated
slot reuse, reverse VP completion and both 7/15-slot variants. Confirm distinct
VP indices and FIFO indices in the lead's trace. Request count is not physical
hardware concurrency evidence. This change makes no silicon performance claim.
