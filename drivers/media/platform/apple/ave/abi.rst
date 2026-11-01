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

6070.11.1 is selected only for T6000 TEXT CRC 460c7a7c or T8112 TEXT CRC 25bfd039,
with the corresponding SoC identity. Boot, seven-channel discovery, Config,
Open, Close and scratch-confirmed Halt are implemented from the clean derived
SPEC-6070-lifecycle.md ledger. Start revision 2 and Process revision P1 supply
the named payload fields. The 6070 writer uses rational Start fps, one initial
QP word, split coded/metadata arrays, address-only neighbour surfaces and four
independent interleaved entropy sets. It does not serialize an MB/LRME-stat
buffer in Start. Process inputs carry stride at descriptor +8 and zero at +12;
normal HEVC uses ID 8, subtype 0x15 + slot, and a 256-entry slice-header table.
Complete is the normal drain barrier. Each 6070 Complete is settled before
another is sent; Flush remains a separate cancel/purge operation.

Streaming is refused for both CRCs by the single source-only
``AVE_6070_STREAMING_QUALIFIED`` gate in ave-layout.c. There is no module
parameter. Lifecycle validation remains available. Reset remains unavailable:
its request has a codec-dependent body. No Priority command is sent on 6070.

This is a partial payload implementation, not a hardware qualification build.
The current ledger names primary reference banks but does not specify all
secondary reference unions, the fourth neighbour-FW resource, invariant codec
words or the initializer for the retained Process codec partitions. Those must
be completed before flipping the gate for a hardware test. A Process build
without explicitly prepared retained state returns EOPNOTSUPP without changing
the command. Synthetic state used by tests proves partition preservation only;
it is never installed by session setup or accepted as a codec initializer.

The reply parser normalizes 6070 success 0x00ee0000, validates exact scalar and
encode lengths, routes client/subtype independently of 9003 and preserves the
raw done ID and EncodeDone extra word. The extra word's semantics are not in
the supplied ledger, so it is retained as opaque data. Standalone LRME (0xe07)
and internal MCTF (0xe08) are recognized and refused with EOPNOTSUPP; they cannot
retire a normal encode slot. Metadata interpretation still needs 6070
qualification. Field tests compare complete synthetic command buffers against
literal wire offsets, including all named scalars, absent fields, split output
banks and all four entropy sets, with B frames on and off for 1080p AVC/HEVC.

Lifecycle-only validation
-------------------------

Build with CONFIG_DEBUG_FS. On a supported 6070 device, write ``1`` (AVC) or ``2``
(HEVC) to ``/sys/kernel/debug/apple-ave/lifecycle_test``. This synchronous operation
runs boot, Config, Open, Close and Halt, then requests runtime suspend. It does
not allocate encoding surfaces or issue Start, Process or uninit for an encoder
that was never initialized. A second write exercises a cold restart and DATA
restore. An existing session makes the operation return EBUSY.

The platform must supply the extra-0 register resource as ``extra0`` or register
resource index 3, with at least 0x24 bytes on T6000 or 0x4000 bytes on T8112.
The driver uses the translated platform resource address, maps its containing
16 KiB page through the device DMA API and puts that dynamically allocated IOVA
in Config. Platform DT/binding changes are separate from this driver change.
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
