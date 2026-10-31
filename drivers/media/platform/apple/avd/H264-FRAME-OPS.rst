H.264 frame operations (integration draft)
=========================================

The descriptor exports ``avd_h264_frame_ops`` alongside the legacy callbacks so
this codec branch builds on aurora-wip while the queue core is developed. The
legacy entry points must be removed when integrating the frame queue. Neither
prepare nor retire operates hardware or removes mem2mem buffers. The queue owns
request control setup/completion, source/destination lifetime and error retire.
The core must call retire after prepare failures that acquired codec_priv.

Open prepare writes little-endian instruction words directly into frame->prog,
with a capacity check before every word. Coded data uses the OUTPUT DMA address
plus the plane's data_offset and the validated emulation-prevention-aware header
offset. No coherent allocation or copy of coded slice bytes remains. Scratch is
cached separately for each of the sixteen frame indices and retained until the
core drains the context. Motion vectors remain in each CAPTURE allocation's
reserved tail, so references retain their own vectors. H.264 declares DEP_NONE.

Unresolved contract and protocol facts
--------------------------------------

A multi-buffer HOLD_CAPTURE_BUF picture cannot safely return intermediate
OUTPUT buffers before the final decode. Userspace may immediately overwrite a
returned OUTPUT, even if a kernel reference keeps its allocation alive. The
current shared contract says to return those buffers immediately. Until core
retains every source through the picture's completion, prepare and the legacy
path reject HOLD_CAPTURE_BUF with EOPNOTSUPP. This is an explicit limitation,
not successful multi-slice support.

The V3 payload projection uses the MIT public layout in eiln/avd
``avid/h264/fp.py`` and its ``halv3.py`` field encoding. Transport PIODMA words
and FIFO configuration remain transport-owned and are not fabricated. The
public sources do not establish a Borage syntax layout. SPEC 6.2 establishes
only part of the command: the exact +0x3c/+0x44 packed-address rules and both
work-count bounds are still absent. The known Borage command projection returns
EOPNOTSUPP; it must never be published. V3 commands also return EOPNOTSUPP because
they cannot use the Borage command ABI. Apple decoding is not yet enabled.
T8112/T8140 are explicit TODO rows tied to per-SoC ledger task t-3862.

Verification
------------

Run on a Linux build host::

  python3 drivers/media/platform/apple/avd/tests/run-host-tests.py

The host harness compiles the production builder and parameter writer with
kernel plumbing replaced by a small shim. It compares the word stream against
a frozen, licensed aurora-wip builder for synthetic V4L2 I/P/B controls,
1/4-slice programs, CABAC/CAVLC, scaling, weights and both address formats.
Four-slice program equivalence does not imply that the queue lifetime problem
above has been solved. Parameter tests check V3 offsets, address alignment,
array capacity, known Borage command offsets and unsupported-layout rejection.
These tests do not feed an encoded ffmpeg clip through GStreamer or compare
against an execution of eiln's bitstream decoder; those remain integration work.
