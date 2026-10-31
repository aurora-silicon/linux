VP9 frame queue integration (draft)
==================================

The codec builds frame-owned programs through avd_codec_frame_ops on queued
open firmware. Stock firmware keeps its original compatibility builder. The
stream selects its callbacks at STREAMON and retains that selection through
drain and stop, including error recovery. avd-queue.h is the integration core's
extended contract without codec changes.

Compatibility and FIFO ownership
--------------------------------

avd-vp9-legacy.c is avd-vp9.c from integration commit d826cca0dff6, with only
its exported ops symbol renamed. Its run, adaptation, PP kicks and program
construction remain unchanged. The compatibility transport sees no native
frame_ops and continues to call those original callbacks.

Queued open firmware accepts native VP9 programs. Each frame owns its VP-to-PP
FIFO storage and snapshots its tile count, which supplies that frame's PP kick
count. Backend submission does not consult a later frame's mutable tile count.
The slot pool has 16 entries; actual in-flight capacity is capped by the
backend's physical FIFO count (15 or 7). Reservation uses only those indices,
so a slot can never alias another active frame's physical FIFO.

Probability and segmentation ownership
-------------------------------------

Each slot has independent probabilities, counts, scratch, current/previous
headers and entropy-context snapshots. Saved contexts start from the V4L2
defaults. Reset and forward updates are staged and committed only after all
program and Apple parameter validation succeeds. The Apple packer sees the
current frame's initialized probabilities.

Forward-only refreshes publish during prepare. A backward-refresh producer sets
PREV_DONE when refresh_frame_context && !frame_parallel_decoding_mode. The core
uses that producer's dependency to block the next CPU prepare until in-order
retirement consumes its counts. Other frames set NONE and may overlap. A
pending-count guard also rejects premature direct callers with -EAGAIN; normal
queue operation must satisfy the dependency before calling prepare. Error
retirement poisons native stream probability state until streamoff.

Segmentation uses a pool of 17 maps: each prepared frame pins its input and
output, and the stream retains the latest output. A completed slot's map stays
pinned while a later frame reads it. Reusing a slot does not reuse a live map.
Line scratch is per slot. Stop requires the core to drain this stream's frames.

Apple payload status
--------------------

The V3 table derives from Eileen Yoon's MIT-licensed avid/vp9/fp.py and probs.py
in https://github.com/eiln/avd. It materializes the known header and tile fields.
Borage materializes tile columns/rows at 0x218/0x21a and the proved command words
from protocol section 6.4. Their product is capped at 256.

Both Apple packers still return -EOPNOTSUPP for incomplete payloads. V3 needs
FIFO/PIO work-record patching; Borage needs syntax/surface mappings, the first
work source and packed address words at command 0x2c/0x34. No partial payload
may reach firmware. T8112 and T8140 have explicit empty rows pending Daybreak
ledger task t-3862; neither aliases V3 or Borage. Apple's backend remains
inactive in the integration core, so these incomplete layouts do not replace
the working stock path.

Native open preparation accepts Profile 0/8-bit and Profile 2/10-bit 4:2:0. This
is syntax support, without new hardware qualification or format advertising.
Apple Profile 2 requires proved bit-depth fields. The existing four-bit tile
column instruction field limits the open path to 16 columns, independently of
Borage's 256-tile validation limit.

Validation
----------

Run ``sh drivers/media/platform/apple/avd/tests/run-vp9.sh``. Host tests compile
production packing/tile code and the full codec with real V4L2 entropy helpers.
They cover 16 prepared slots, probability isolation, backward-refresh producer
and consumer dependencies, map lifetime, malformed-input rollback, capacity,
DMA carry, EXEC framing, Profile 2 and all compatibility callback dispatches.
A checksum keeps the entire stock builder identical to d826cca0 (export rename
only). A second test compiles both complete C builders and compares 360 valid
1080p/4K programs across V3/V4, 8/10-bit, key/inter, tiles and segmentation,
normalizing scratch DVAs. These tests also run with address/undefined sanitizers.

DMA, V4L2 lookup and transport are simulated. These tests do not establish
libvpx-to-stateless-control parsing, show-existing display behavior, hardware
CRCs, FIFO timing or throughput. Those require the separate decoder qualification
suite. Apple payload completion remains a prerequisite for Apple-backend use.
