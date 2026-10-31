VP9 frame queue integration (draft)
==================================

This branch targets the avd_codec_frame_ops queue contract. It is not ready to
merge independently of the queue core. The original queue header is unchanged.
The legacy .run callback returns -EOPNOTSUPP: it must not feed frame-owned DMA
programs to the old, context-wide job path.

The core applies request controls before prepare, owns the source and destination
buffers, and serializes prepare and in-order retire for a context. In particular,
a prepare result of -EAGAIN leaves both buffers and request controls pending:
retry it after retiring the earlier frame. Do not mark the buffers erroneous or
submit the partially built program. Waiting only at backend submit is too late,
because prepare itself consumes probability state produced by retire.

Probability and segmentation ownership
-------------------------------------

Each of the 16 slots has independent probability, count, scratch, current-header,
previous-header and entropy-context snapshots. Saved contexts are initialized to
the V4L2 defaults. Forward-only refreshes are committed during prepare; backward
refreshes are committed at retire. A pending backward refresh prevents another
CPU probability preparation until its counts have been consumed. Error retirement
poisons this stream until streamoff rather than saving corrupted contexts.

The dependency belongs to the consumer: PREV_DONE while an earlier backward
refresh is pending, NONE otherwise. The predicate for the producer is
refresh_frame_context && !frame_parallel_decoding_mode. Merely setting the
consumer's dep after preparing stale probabilities would be incorrect.

Segmentation state uses a pool of 17 maps: each prepared frame pins its input and
output and the stream retains the latest output. A completed slot's map remains
pinned while a later frame reads it. Reusing a slot does not reuse a live map.
The line scratch buffer remains per-slot. Stop requires all frames drained.

The open program is the legacy header/tile word sequence with EXEC boundaries
materialized in little-endian DMA memory. The firmware backend owns FIFO pacing,
VP-to-PP chaining, and physical FIFO reuse. It must not activate two frames which
alias the same physical FIFO index (slot & fifo mask).

Apple payload status
--------------------

The V3 table derives from Eileen Yoon's MIT-licensed avid/vp9/fp.py and probs.py
in https://github.com/eiln/avd. It materializes the known header and tile fields.
The Borage table materializes tile columns/rows at 0x218/0x21a and the proved
command words from protocol section 6.4. Their product is capped at 256.

Both Apple packers deliberately return -EOPNOTSUPP. V3 still needs FIFO/PIO work
record patching; Borage still needs syntax/surface mappings, the first-work
source and packed address words at command 0x2c/0x34. No partially filled payload
may reach firmware. T8112 and T8140 have explicit empty rows pending the Daybreak
per-SoC ledger task t-3862; they never fall back to V3 or Borage. The T6040 match
selects Borage. Command packing preserves only the proved prefix bits and rejects
addresses which cannot fit the proved direct u32 fields.

The open path accepts Profile 0/8-bit and Profile 2/10-bit 4:2:0. This is syntax
support, not new per-SoC hardware qualification. Apple Profile 2 cannot be enabled
until its bit-depth fields are proved. The existing four-bit tile-column
instruction field limits the open path to 16 columns. Extending to 64 needs a
proved instruction encoding, independently of the Borage limit of 256 tiles.

Tests and remaining validation
------------------------------

Run ``sh drivers/media/platform/apple/avd/tests/run-vp9.sh`` on a host. The tests
compile the production tile writer, compare 15 tile geometries against the
legacy word sequence, reject malformed/truncated tiles, check output capacity
a 4-GiB DMA carry, EXEC boundaries and segmentation map pins, check the full dependency flag table, and verify known
V3/Borage offsets and isolation of 16 frame buffers. A full codec host test uses
the real kernel V4L2 entropy helpers and simulated DMA/control lookup to exercise
16 prepares, segmentation controls, malformed-input rollback, count-producer
retries, adaptation, error retirement and Profile 2 bit-depth fields. The host
suite also passes with AddressSanitizer. These
tests are not a substitute
for libvpx streams through the full stateless request/control and adaptation path.
That differential suite, queue-core retry/drain integration, physical-FIFO
ownership tests, and hardware decode/CRC qualification remain outstanding.
