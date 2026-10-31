AV1 frame operations
====================

This codec series targets avd_codec_frame_ops. It requires the accompanying
16-slot core and queued open-firmware backend before decode is usable. The
legacy core rejects a codec without a run callback instead of dereferencing it.
The queue header is the shared contract, unchanged.

prepare consumes controls already applied by the core and the slot's src/dst.
It never accesses an m2m queue, completes a request, or touches MMIO. The open
program is a little-endian stream: header EXEC, the existing header words, then
one tile payload and its EXEC per tile (END on the last tile). prog_words counts
all words, including EXEC records. The backend owns VP selection, FIFO assignment,
submission and VP/PP chaining. The core supplies fp only for an Apple submission;
NULL fp selects preparation of the open program.

Each live frame owns its pipe state and every above/left/MV scratch allocation.
CDFs and segmentation maps are separate coherent DMA allocations. Capture-index
records retain their output state; in-flight frames retain input and output
state independently. A disabled CDF/map update inherits an immutable allocation,
including its ownership reference, rather than copying a potentially unfinished
capture tail. Reusing the original capture index therefore cannot free inherited
state. Color/MV reference data already stored in capture buffers remains subject
to the stateless reference-buffer lifetime contract.

The incoming dependency is PREV_DONE exactly when the current frame uses the
previous frame as primary reference and that previous frame enables frame-end
CDF updates. Defaults (intra, error resilient or PRIMARY_REF_NONE), a different
primary reference, and disabled frame-end updates produce NONE. prepare does
not read hardware CDF/map output on the CPU. It refuses an unfinished older CDF
or segmentation producer with EAGAIN when the specified dependency rule cannot
express the dependency. The integrating core must retry preparation after an
earlier completion; the present contract does not define that retry behavior.
The core must also propagate failed producers to already prepared dependants,
serialize prepare calls per context, and drain/retire all slots before stop.

Apple layout evidence and remaining work
----------------------------------------

The source is the clean-room Apple AVD protocol SPEC dated 2026-10-31, sections
5, 6.1, 6.5, 6.6 and 10. Public V3 layouts in eiln/avd cannot supply AV1 fields:
those generations have no AV1 hardware. avd-av1-fp.c retains an unnamed [C]
table with the missing fields and their sources. It writes the proved Borage
tile columns/rows at 0x3e4/0x3e6 and stages the proved command destinations,
work-count bound 0x1002 and fp+0x4f8 -> cmd+0x4c copy. Both Apple pack operations
return EOPNOTSUPP: the SPEC omits the syntax/DVA layout, source flag shifts and
packed-address encoding needed for a valid command. No guessed payload is sent.
T6040 selects Borage; T8140 explicitly selects a TODO row pending the Daybreak
per-SoC ledger task t-3862. Other Apple layouts stay unsupported. Existing AV1
hardware capability bits are retained; T6000/T8112 have neither AV1 capability
nor an AV1 VP. Film grain synthesis is unsupported by the existing open builder
and an APPLY_GRAIN request is rejected explicitly.

Verification
------------

Run ``python3 drivers/media/platform/apple/avd/tests/run-host-tests.py`` from a
Git checkout, including a sparse checkout. A host C compiler with ASan and UBSan
is required. Tests compile the production CPU code with small DMA/V4L2 doubles
and the checkout's real AV1 UAPI definitions. They cover live slot isolation,
CDF inheritance, dependency cases, allocation failure cleanup, payload bounds,
program capacity and Apple-layout refusal. The frozen pre-conversion packing
oracle comes from aurora-wip fbc4e7934ca7325f3483b72a51d24c74aa8536a4, with
its original license notice. Program comparison uses the same resolved DMA
addresses in both builders; allocation ownership intentionally changes addresses.

These are synthetic control vectors, not SVT-AV1 bitstreams or hardware output
validation. Film grain is a rejection test. AV1 show-existing has no decode-frame
control in the stateless UAPI: userspace redisplays a reference without submitting
a new decode. Queued firmware execution, Apple payload completion, retry semantics
and end-to-end SVT-AV1 decode remain integration/qualification work.
