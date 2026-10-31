.. SPDX-License-Identifier: GPL-2.0-only

Apple ANE firmware preflight
===========================

The shared ANE firmware module recognizes the H16 (``apple,t6041-ane``,
physical T6040/M4 Pro) and H17 (``apple,t8140-ane``) generations. Recognition
is not an inference support claim: both remain unavailable for execution.
They never enter the H13 task-manager or H14 disk-firmware boot paths.

H17 ownership
-------------

Linux has no ANE exclave ownership handoff. The module refuses registration
with ``-EOPNOTSUPP`` and one informational message when an H17 node is
present, including a disabled node. This check occurs before platform core
DMA configuration and automatic power-domain attachment. A probe-time
check alone would be too late. Removing ``apple,exclave-assigned`` does not
establish ownership and does not bypass refusal.

H16 preload handoff
------------------

The bootloader must reserve the personalized TEXT and DATA preload segments
as separate, non-reusable ``no-map`` reserved-memory nodes, attach them in
that order through ``memory-region`` and name them ``firmware-text`` and
``firmware-data``. It must also supply ``apple,firmware-iovas`` as two 64-bit
values: ``0x10000000000`` and ``0x100000c0000``. Their sizes must be
``0xc0000`` and ``0x2ac000``. Physical placement is boot-dependent; board DT
sources must not contain captured physical addresses. The reservations must
remain intact until reboot, including after driver failure.

H16 preflight checks resource geometry, firmware identity and these ranges
without MMIO or power access. No disk firmware is requested. Missing
reservations fail with ``-ENODATA``. Malformed reservations fail with
``-EINVAL``. Even a valid handoff currently fails with ``-EOPNOTSUPP``:
the strict preload validator, original-container HWX ABI and cold boot
backend are incomplete. The section-only load ABI is explicitly refused
for H16. No accel node or program-loading interface is published.

The H16 DT nodes stay disabled. The five real PMGR providers already encode
the system/CPU/MPM/task/base dependency graph. The PMGR binding does not
represent the virtual no-state gates, so no fictitious register-backed
domains are created for them. SID 0 and SID 11 on all three DARTs use the
shared DMA domain; no untested stream remapping is added.

Implemented SPEC-r2 components
------------------------------

The shared transport has an eight-descriptor dynamic channel parser with
unique names, bounded counts, owned-surface bounds and overlap checks. The
runtime uses copied descriptor geometry, verifies the descriptor snapshot,
and checks the complete IO reply envelope. Zero acknowledgment is success;
a nonzero acknowledgment is a firmware refusal, never a retry or success.
Malformed replies, guard changes and timeouts quarantine the instance. A
quarantined queue cannot allocate another BO, and no reset is attempted.

The version-12 RTKit management state machine validates HELLO, endpoint maps,
system endpoint start, IOP/AP power acknowledgments and application endpoint
start ordering. It does not invent responses for unknown message types or
unimplemented system endpoint buffer requests. Its caller must provide one
absolute session deadline and a bounded 64-bit mailbox transport.

H16 validation helpers walk nine fixed task-header words, the documented
conditional extra word, dense/scatter records and the nine allowed register
blocks. A separate helper checks the eight-byte Mach-O relocation entries,
unique aligned offsets, the actual kernel-section ordinal and addend bounds.
These helpers do not substitute for complete container/tensor/program and
16-byte task-aperture metadata validation.

The device-free selftest in ``tools/testing/selftests/drivers/ane`` generates
the SPEC-r2 fp16 add-128 inputs and CPU oracle and verifies all three pinned
buffer hashes. It can also check a supplied HWX against the pinned size/hash.
It never opens an accel device or claims a Linux inference result.

Remaining implementation gates
------------------------------

SPEC-r3 supplies the custom descriptor/aperture layouts and a normalized
immutable preload digest with 42 ordered patchbay records. The complete
validators, original-container ABI and physical boot backend still need
implementation. Off-list preload mutations remain refused.

The 2026-10-31 H16 runtime observation did not capture cold boot, scratch
negotiation, RTKit management, CONFIG_GET or RESOURCE_INFO_GET. The version-12
and 24 MHz predicates remain conservative proposed startup gates; runtime
traffic does not prove them. Resource and allocation ceilings stay unchanged.
H17 additionally requires a Linux ownership backend before direct access.
There are no module parameters to bypass these gates.

H16 macOS 27.0 runtime contract
------------------------------

J616s has one ANE interrupt, 1054, used as the shared-memory channel
doorbell/completion line for all firmware-to-host replies and events.
It has no ANE exclave assignment. This identifies the interrupt role, not
its cold-start masking/acknowledgment sequence. H17's two IRQ roles remain
unknown. ANE runtime uses its own channel transport; it does not establish
an A7IOP/RTBuddy boot sequence.

Host command bytes 0..3 are zero, the little-endian u16 ID is at +4 and
the u16 acknowledgment at +6. All observed successful replies had zero
acknowledgment. Nonzero meanings remain unknown and are refused.

==================  ======  =======  ==================
Command             ID      Channel  Bytes
==================  ======  =======  ==================
LOAD_PROGRAM        0x0200  1        0x1c0
UNLOAD_PROGRAM      0x0201  1        0xc
CREATE_PROCESS      0x0202  1        0x258
TERMINATE_PROCESS   0x0203  1        0x10
EXECUTE             0x020c  7        0xca8/0xcc0/0xcd8
POWER_GATE          0x002d  7        0xc
EVENT_REPORT        0x0300  6        0x60 (firmware to host)
==================  ======  =======  ==================

These sizes bound the observed version, not future firmware. POWER_GATE has
a u32 Boolean at +8. The execute-size increment of 0x18 does not establish
buffer-record semantics. The channel numbers are observed table indices;
their mapping to channel names remains unproved. The parser retains each
descriptor's original index rather than assuming name order is wire order.

The transport prepares the H16 envelope and rejects unrecognized runtime
IDs, wrong sizes, channels and power-gate values before publication.
H14's 0x0204 procedure packet and short CREATE_PROCESS body remain H14-only;
H16's body serializers and completion correlation are not implemented.
The H16 runtime command table does not authorize startup commands.

The event report starts with u32 0x0000fe00 through 0x0000fe03, then u16
0x0300 and zero u16 at +6. Its body is undecoded. Valid reports are consumed
as diagnostics and never complete an inference or release its buffers.
The legacy 0x28-byte state-1 completion predicate must not be used for H16.
Unknown short channel-6 messages fail closed. A timeout retains mappings
and quarantines the instance; it does not trigger a retry or reset.

KUnit covers all seven runtime IDs and their sizes/directions/channels,
the u16 envelope and refusal semantics, and report validation without
inventing completion fields. All test packets are synthetic.
