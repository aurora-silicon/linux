AV1 frame operations
====================

AV1 implements avd_codec_frame_ops and retains the legacy run/done/submit and
pp_kicks callbacks. The current open transports still require legacy segments
and codec-specific PP kicks. avd_get_frame_ops selects that compatibility path
when a legacy run callback exists, and selects frame_ops on Apple. Retirement
uses the operations selected at preparation, including across a backend reset.
The queue header is core's unmodified shared contract.

The compatibility adapter applies the request controls, invokes the same CPU
prepare implementation, and copies the recorded header/tile ranges into the
legacy job. It never finds boundaries by treating arbitrary instruction data
as opcodes. Scratch and auxiliary DMA allocations stay alive until done calls
retire. AV1 uses one legacy picture per context until the open transports accept
native frame_ops; the queued transport gets the exact number of PP kicks.
Open decode retains aurora-wip's grain-less fallback for APPLY_GRAIN requests.
Film-grain synthesis is not implemented by the existing instruction builder.

prepare consumes applied controls and the slot's src/dst. It never accesses an
m2m queue, completes a request, or touches MMIO. The little-endian open program
contains header EXEC, the existing header words, and one tile payload plus EXEC
per tile (END on the last tile). prog_words includes EXEC records. Only the
compatibility adapter accesses the legacy job; PP register writes live in the
transport. Core supplies fp only for an Apple submission.

Each live frame owns its pipe state and all above/left/MV scratch allocations.
CDFs and segmentation maps have reference-counted coherent DMA allocations.
Capture-index records retain output state; live frames retain their inputs and
outputs independently. Disabled updates inherit an immutable allocation rather
than copying unfinished hardware output. Reusing the original capture index
cannot free inherited state. Color/MV data in capture buffers follows stateless
reference-buffer lifetime rules. Segmentation inherits a map only when MiCols
and MiRows match, retaining the existing builder's size comparison. Reference
publication uses OUTPUT's timestamp:
core copies that metadata to CAPTURE only after prepare has returned.

The incoming dependency is PREV_DONE exactly when the current frame selects the
previous frame as primary reference and that previous frame enables frame-end
CDF updates. Intra/error-resilient/PRIMARY_REF_NONE defaults, another primary
reference, and disabled frame-end updates produce NONE. prepare does not read
hardware CDF/map output on the CPU. An unfinished older producer which that
rule cannot express returns EAGAIN. Native integration still needs preparation
retry and propagation of failed producers to already prepared dependants.
Legacy decode is serial and retires its producer before the next preparation.

Apple layout evidence and remaining work
----------------------------------------

The source is the clean-room Apple AVD protocol SPEC dated 2026-10-31, sections
5, 6.1, 6.5, 6.6 and 10. Public V3 layouts in eiln/avd cannot supply AV1 fields:
those generations have no AV1 hardware. avd-av1-fp.c keeps an unnamed [C] table
with sources for the missing fields. It stages proved Borage tile columns/rows
at 0x3e4/0x3e6, command destinations, work-count cap 0x1002 and the
fp+0x4f8 -> cmd+0x4c copy. Both Apple pack operations return EOPNOTSUPP:
the SPEC omits syntax/DVA mappings, source flag shifts and packed-address
encoding. These facts do not yet describe a usable Apple AV1 payload.

T6040 selects Borage. T8140 has a distinct TODO row tied to Daybreak's per-SoC
ledger task t-3862. Other Apple layouts stay unsupported. Hardware capability
masks still govern format exposure; T6000/T8112 have no AV1 capability or VP.

Verification
------------

Run ``python3 drivers/media/platform/apple/avd/tests/run-host-tests.py`` from a
Git checkout (sparse checkouts work). A host C compiler with ASan and UBSan is
required. Tests compile production CPU code and the production backend selector
with DMA/V4L2 doubles and the checkout's AV1 UAPI definitions. The licensed
packing oracle is frozen from aurora-wip
fbc4e7934ca7325f3483b72a51d24c74aa8536a4. Program comparison resolves the same
DMA addresses for both builders; changed ownership intentionally changes them.

Coverage includes 64 parity vectors (tiles, CDEF, restoration, superres, 8/10-bit,
inter frames and grain-less fallback), legacy multi-tile submission/PP counts,
submission failure cleanup, source-timestamp publication, producer errors, the
dependency table, sixteen live slots, inherited state and capture reuse,
allocation failure cleanup, payload/program bounds and Apple-layout refusal.
These are synthetic controls, not SVT-AV1 bitstreams or pixel validation.
Show-existing is a userspace reference redisplay without a new stateless decode
control. Native open execution, the missing Apple payload facts, retry/failure
integration and end-to-end stream qualification remain outstanding.
