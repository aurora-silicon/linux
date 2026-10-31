H.264 picture preparation and ownership
======================================

H.264 exports CPU prepare/pack_cmd/retire operations. The queued open backend
accepts its complete instruction program; the older open backend selects the
legacy callbacks. Both paths collect slice descriptors and request-control
snapshots until the final request of a HOLD_CAPTURE_BUF picture. Only metadata
is copied. Each descriptor points at the OUTPUT DMA address plus data_offset
and the emulation-prevention-aware slice-header offset.

The core removes intermediate OUTPUTs from the scheduler without completing
them. It transfers their ownership to the final frame and completes them after
CAPTURE retirement. Incomplete pictures release their OUTPUTs and codec state
on streamoff or preparation failure. Userspace must queue every slice of a
picture before waiting for its OUTPUT buffers; it cannot recycle a held slice.
The codec never operates mem2mem queues or MMIO in its frame callbacks.

Each live picture owns independent scratch and a VP-to-PP instruction FIFO.
Sixteen owners are cached per context, with reuse only after retirement. Motion
vectors remain in their reference CAPTURE allocation. H.264 uses DEP_NONE.

Apple layout boundary
---------------------

The public MIT eiln/avd avid/h264/fp.py and halv3.py describe Viola/V3. The
packer writes its SPS/PPS/scaling, DPB, output, weights and slice fields, with
600 separately bounded slice records. This public comparison does not establish
the later LilyD layout selected by T6000 firmware. Match data must select a
proved layout rather than infer one from the hardware register revision.

Borage has an independent table with the command projection proved by SPEC 6.2.
The exact +0x3c/+0x44 packed-address rules, both work-count bounds, and the full
parser-compatible syntax layout remain unproved. These fields retain numeric
source offsets and return EOPNOTSUPP; this draft cannot enable Apple decoding.
T8112 and T8140 remain explicit per-SoC ledger TODO rows (t-3862). No later-class
layout falls back to V3.

Verification
------------

On a Linux build host run tests/run-host-tests.py. It compiles the production
builder and parameter writer, compares 96 synthetic I/P/B 1/4-slice programs
against the licensed frozen Linux builder, and exercises the actual prepare and
retire functions across separate held requests and sixteen live pictures.
Additional tests compile the core source-ownership helpers and check final
completion, native/legacy partial abort, V3 field bounds and Borage projection.
These are host tests, not target decode or GStreamer/ffmpeg conformance results.
The encoded-stream comparison with eiln's decoder remains separate validation.
