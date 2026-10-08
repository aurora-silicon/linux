.. SPDX-License-Identifier: GPL-2.0-only

Experimental T8140 video encoding
================================

The J700 AVE driver exposes a root-only debugfs firmware transport for
hardware encoding experiments. It is not a V4L2 encoder, has no stable
userspace ABI, and does not make an FFmpeg encoder available. The initial
scope is the existing fixed 320x240 NV12 H.264/HEVC protocol experiment.

Build ``CONFIG_VIDEO_APPLE_AVE_EXPERIMENTAL=m`` on a 16 KiB-page Apple kernel
with debugfs and translated DMA. Suspend, hibernation and kexec must be off.
Load with ``modprobe apple-ave enable=1``; probing is disabled otherwise.

Firmware and boot requirements
------------------------------

Use the public Aurora m1n1 J700 AVE handoff. It reserves the firmware already
loaded by iBoot, fills the device's ``memory-region`` references and exports
``aurora,ave-text-crc32`` and ``aurora,ave-data-crc32`` strings under
``/chosen``. No firmware file is included or requested by this driver.
The current transport requires the qualified 25G76 two-segment layout:
0x14c000 text bytes and 0x130000 data bytes, mapped starting at IOVA
0x10000000000. Every mapping and both content CRCs are checked before use.
Text CRCs are restricted to the two inspected 9003.78.0 images (0x2ce7359e
and 0x69c54454). These checks detect build drift, not firmware authenticity.
Different firmware is unsupported, even if the device has the same SoC.

The driver holds the DMA, pipe4, pipe5, me0 and me1 power domains and owns
the IPC, queue, log and firmware-requested heap allocations. Debugfs lives
under ``/sys/kernel/debug/285100000.video-codec``:

* ``ave-status`` reports mappings, protocol phase and reply counters.
* ``ave-start-once`` accepts one explicit firmware start per boot.
* ``ave-command`` accepts one bounded packet and returns a framed reply.
* ``ave-arena`` stages image/reference/output surfaces within owned memory.
* ``ave-poll`` drains later firmware replies without another submission.

Writes require ``CAP_SYS_RAWIO``. Command packets must come from a trusted,
matching local protocol experiment with all embedded surface addresses
relocated into this boot's owned arena. Root access does not make arbitrary
firmware packets valid. Binary command captures are supplied separately and
are not included in this contribution.

Lifecycle and verification
--------------------------

After the start attempt, the module, DMA buffers and power references remain
pinned until an external hardware reset, including after failure. Do not
force-unload, unbind the DARTs, reuse uncertain buffers or retry a failed
command. Firmware quiescence and normal production teardown are unfinished.

A transport acknowledgement is not an encoded frame. For physical tests,
record the exact kernel/module, DT and firmware hashes; submit changing
images; validate each codec reply; independently decode the current DMA
output; check every frame's dimensions and quality; then verify Stop/Close,
kernel health and the existing desktop/radio stack. Keep the generated
inputs, bitstreams, decoded pixels, logs and hashes as a repeatable artifact.

The prior implementation was authored by Ace (Acelogic). This contribution
ports that existing GPL work; it does not claim independent clean-room
provenance. Arbitrary resolutions, standard media APIs, throughput,
concurrent clients and other codecs or SoCs are outside this experiment.
