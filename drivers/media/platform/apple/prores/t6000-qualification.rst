.. SPDX-License-Identifier: GPL-2.0-only

T6000 APR a0 qualification
=========================

This port describes an encoder, not a decoder. T6000 enables APR0 and its
DART in ``t6000.dtsi``; T6001 and T6002 keep theirs disabled.

Status
------

Run on a J316s (M1 Pro) on 2026-11-08 with the shipped node descriptions,
without a performance request. The engine identifies as 0x10001. The
four input formats and all six profiles were encoded at 1920x1080, plus
Standard, HQ and 4444 at 3840x2160. ffmpeg decoded every frame at the
expected size and chroma format. There were no DART faults, and the
engine runtime-suspended with its power domain off afterwards.

Rate control matches macOS: with the per-profile budgets, frame sizes
equal VideoToolbox's on the same input (measured against macOS on the
T8140 engine), on easy, detailed and noisy clips at 1080p and 2160p.

Not yet covered: cancellation, failed-reset retention (#925), system
suspend and the SCODEC performance request below.

Implementation sources
----------------------

The generation/resource contract is t-5068's CLEANROOM.md (2026-11-07).
The licensed implementation source is MIT m1n1 commit
f3ac615eb1376d3bbba24cd84c414b9e41892adb:

* ``proxyclient/experiments/prores.py:42-89``: all 48 core RMWs, in order.
* ``proxyclient/m1n1/hw/prores.py:7-111``: the v0 descriptor layout.
* ``proxyclient/experiments/prores.py:592-644``: legacy header, slice and
  rate-control words. The v0 rate-control block is the v1 one, 0x20 bytes
  earlier; the builder writes the per-profile target and maximum budgets
  at 0xf0/0xf2 instead of the experiment's unbudgeted 0xffff. Profile
  matrices and initial quantisers follow the existing driver's policy.

The MIT notice for the adapted constants is retained in ``m1n1-LICENSE``.
T6000 selects neither wrapper nor SCODEC-system writes. It has no BWR entry;
this port adds no DCS floor. T6041/J616s resources and initialization are
outside this implementation.

Prerequisites to first DMA
-------------------------

1. Use an approved controller request explicitly identifying ``m1pro``
   (T6000/J316s), the exact kernel and DTB-set hashes, source ownership and
   the existing boot fallback. No target access is needed for this patch.
2. Land the DMA-ownership repair described by Linux PR #925, or a reviewed
   equivalent: an uncertain completion or failed reset must retain buffers,
   mappings, runtime PM, scheduler ownership and the fixed platform device.
   The base driver is not qualified for an intentional hang/reset test.
3. Qualify the PMP report provider's update aperture and a narrow allow-list
   for entry 0x115. The disabled report@14 describes level 2, but the provider
   at this patch's base cannot admit that update. Require physical SCODEC
   residency, record-20 request/ACK, then level 2 before starting APR. Publish
   level 0 before releasing record 20 or SCODEC. Never replace this with an
   unmeasured bandwidth or DCS vote.
4. Prepare a J316s-only trial DTB enabling ``apr0_dart``, ``apr0`` and
   ``pmp_report_scodec`` after the provider prerequisite is met. Preserve
   other boards' disabled nodes. Check SID 0, the DART aperture and the
   firmware DVA window 0x4000 + 0xfffe00000; verify every DMA mapping fits.
   Record the APR ID/version and confirm the expected a0/v0 engine before
   submitting a descriptor. The ID must never select a different generation.

Boot the actual DTB set
-----------------------

The M1 Pro obtains its DTBs from ``boot.bin``, not from the kernel package's
loose DTB. Use lane DT2's refmac DTB-set trial procedure in
``kits/aura/20261107-refmac-dtb-int/README.md``. Freeze the complete trial set
and its manifest, incorporating the J316s trial DTB. Preserve the known-good
production set and ``boot.bin`` backup.

Run the recipe's status/reproduction checks, stage, enable the trial guard,
arm a one-shot and reboot through the approved controller operation. Confirm
``chosen/aurora,trial``, the running APR/DART/report properties, the DTB hash,
and a successfully armed trial watchdog before DMA. Keep bootguard fallback
and the trial watchdog throughout. Preserve the reference-Mac regression
results required by DT2. This patch does not authorize making the trial set
permanent. On failure use the spent-marker fallback and the kit's clean or
rollback procedure, preserving the failed trial evidence.

Bounded first job
-----------------

* Keep the normal speaker protection service governing; this test is silent.
* Allocate a coherent 128 KiB ring, a coherent 2 MiB statistics area and one
  contiguous P216 input: 320x240, Y/C strides 640 bytes, padded to whole
  16-row macroblocks. Use one CAPTURE buffer of at least 4 MiB, with guard
  mappings where supported. Submit one Standard 4:2:2 frame only.
* With MODE OFF and IRQs masked, apply exactly the public 48 core RMWs.
  Install ring size/IOVA with head and tail zero. Use statistics consumers at
  0x278/0x27c and 0x358/0x35c, checking capacities 0x280/0x360 against the
  allocation after initialization as well as at probe. Set RUN, program
  matrices/DC scale, clear stale status and enable DONE|ERROR.
* Inspect the v0 descriptor before publication: frame header at 0x58,
  deprecated slice count zero at 0xec, slice/quantiser at 0xee/0xef, public
  legacy words at 0xf0..0x14f, matrix selector 0x23 at 0x150, zero trailing
  bytes. Zero the first eight output bytes, execute dma_wmb(), then publish
  head 0x180. Start the existing one-second watchdog; do not auto-retry.
* Accept only DONE with tail low 24 bits 0x180. After the sampled W1C ACK and
  tail readback, execute dma_rmb(). Require a bounded big-endian frame length
  and ``icpf`` signature. Software-decode exactly that frame and check
  320x240 dimensions, 4:2:2/header colour codes, Standard matrix selection,
  and decoded pixels against the synthetic source using a recorded error
  metric. ProRes is lossy; do not require raw-source byte equality.
* On timeout, ERROR, unexpected tail or a DART fault, collect bounded status,
  head/tail, IRQ, DART and PMP request/ACK evidence. Mask IRQs, request the
  qualified reset and retain DMA ownership until quiescence is proven. An
  unproven reset requires retained resources and the boot fallback, never a
  second submission or freeing memory still reachable by DMA.

Retain source/DTB hashes, descriptor bytes, test input/output hashes, decoded
metrics, IRQ timing, reset outcome and rollback result in the qualification
record. Publish only reviewed, non-identifying extracts. A first-job pass
still leaves cancellation, runtime/system suspend, failed-reset retention,
all four input formats, all profiles and dimensions, ring reinitialization
at capacity, and repeated-job completion to qualify. Keep one in-flight job;
ring wrap and batching require separate evidence before expanding that limit.
