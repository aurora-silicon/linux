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
a u32 Boolean at +8. The execute-size increment of 0x18 counts custom BAR
records. The channel numbers are observed table indices;
the clean cold-boot facts identify their channel names. The parser retains each
descriptor's original index rather than assuming name order is wire order.

The transport prepares the H16 envelope and rejects unrecognized runtime
IDs, wrong sizes, channels and power-gate values before publication.
H14's 0x0204 procedure packet and short CREATE_PROCESS body remain H14-only;
H16's standalone serializer and event classifier are described below;
they are not connected to inference submission.
The H16 runtime command table does not authorize startup commands.

H16 execute and event fields
---------------------------

The macOS 27.0 field contract has a fixed 0xca8-byte execute body followed
by zero, one or two observed 0x18-byte custom BAR records. The 64-slot
ordinary buffer array starts at +0x60 with a 0x30-byte stride; the valid
count is at +0x28. A descriptor carries flags at +0, resource index at +4,
numeric class at +8, the fixed value 3 at +0x10, a u64 DVA at +0x18 and a
u64 byte span at +0x20. Other descriptor bytes are zero. The UUID at
execute +0x20 is independent of program/process/procedure IDs at +8/+0xc/+0x10.
The observed priority is 5 at +0x18; request flags and reserved bytes are zero.

The custom BAR count is at +0xca0 and records start at +0xca8. Each carries
u32 configuration/value-type/BAR selectors, u8 bit offset/width, u16 flags
and a u64 value. ``ane_h16_execute_encode()`` accepts kernel-owned resource
metadata, checks nonempty counts up to 64 and overflow-free mapping spans,
and zeroes unused slots and reserved space. The caller must first match
indices, numeric classes, permissions, selectors and values to immutable
validated process metadata and keep mappings pinned. The helper does not
establish that authority. Its two-BAR ceiling, 64-bit field bound and
same-configuration/BAR overlap refusal are conservative draft restrictions,
not established firmware-wide limits. Unknown class and selector semantics
are not guessed. There is no userspace interface for these structures.

Channel 7 acknowledges submission only. Channel 6 carries 0x60-byte 0x0300
events, with UUID at +8, program/process/procedure at +0x10/+0x14/+0x18 and
state at +0x1c. State 0 is start, 3 hardware-start, 1 successful terminal
completion and 2 terminal failure. Failure was established from field use;
it was not present in the runtime sample. The first word selects report
slots or rolls independently; 0xfe00..0xfe03 is not a universal prefix or a
completion key. CPU-load and timestamp fields are diagnostic.

``ane_h16_event_decode()`` validates the channel, exact size, command and
zero u16 acknowledgment before returning fields. The result helper requires
all four identity fields to match an outstanding request. Destination 1 is
routed away from the general scheduler; unknown states/destinations refuse.
A caller must register a unique pending UUID before publication, consume it
atomically once on a terminal report, synchronize DMA, and apply quarantine
rules before considering any mapping release. A classifier result alone
cannot prove safe release or reject duplicate events without pending state.

The live H16 path still consumes reports as diagnostics. It has no pending
UUID table, serializer call or inference interface. The legacy 0x28-byte
completion predicate remains H14-only. Short channel-6 CPU-load reports and
channel-5 allocation operations are outside these helpers and remain gated.
Timeouts still retain mappings and quarantine without retry or reset.

KUnit covers byte-exact synthetic execute fields, all 1..64 buffer counts,
zero/one/two custom BAR lengths, wide DVAs/spans, reserved-zero output,
boundary and overflow refusal, all event states, interleaved identities,
ISP routing and malformed reports. No test packet is copied from a capture.

Shared H16/H17 firmware transport
--------------------------------

``ane_fw_transport.h`` supplies device-independent ring operations over owned
coherent surfaces and caller-provided memory, DMA-barrier, MMIO, clock and
allocation accessors. Engine-relative doorbell and pending/acknowledge offsets
come from ``ane_fw_platform`` match data. The caller must establish ownership,
firmware identity, power and the private DMA domain before using it. This
helper does not change probe or enable either generation.

Initialization copies exactly eight 0x100-byte descriptors from the IPC
surface and validates their names, types, bits, slot counts, alignment and
non-overlapping ranges, including exclusion of the table itself. IO and
DATA_CHAIN_H2T require type 0; SHAREDMALLOC and IO_T2H require type 1. Each
operation checks the immutable table snapshot. Only type-0 slots are armed,
in word-1, word-2, word-0 order, with values 0, 0, 1 and a DMA write barrier.
Descriptor geometry and doorbell bits remain authoritative. Logical channel
names are independent of their descriptor order.

Publication copies a command into a 64-byte-aligned arena inside the pool,
after the caller's reserved boot prefix. It publishes both length words,
a DMA write barrier, the command DVA, another barrier and the descriptor's
doorbell bit. There is one outstanding transaction per sending channel;
heads advance modulo the descriptor slot count only after a successful
reply. A reply requires the exact DVA with bit 0 set, unchanged length,
zero result word, zero command prefix, unchanged u16 ID and zero u16 ack.
Bit 1 is corruption even before ownership returns. Nonzero ack is returned
as ``-EREMOTEIO`` and retained for diagnosis.

IO_T2H is present when word 0 has bit 0 clear and either word 0 or word 1 is
nonzero. Reports must be four-byte aligned and wholly inside one owned
surface. The transport copies the report before setting word 0 bit 0 and
ringing the descriptor doorbell, without changing the other slot words.
Report dispatch and pending UUID lookup belong to the caller. In particular,
consuming a stale UUID report cannot complete an unrelated request, and the
DATA_CHAIN reply acknowledges submission rather than inference completion.

The SHAREDMALLOC provider accepts allocation tuples only. Requests are
rounded to 16 KiB, with a 1 MiB request ceiling, the exact 2 MiB allocation
with tag 0x45585050 exception, 12 MiB in rounded allocations and 64 handles.
The allocation callback supplies a zeroed, pinned private-domain mapping;
the transport checks its size, DVA alignment, overflow and exclusion from
all other owned surfaces before publishing it. Handles are nonzero ordinals.
FREE and distinguishable control tuples return ``-EOPNOTSUPP`` through the
mandatory refusal logger. An all-zero slot is empty and cannot distinguish
a control request. No FREE/control reply is invented while that form is
unspecified.

IRQ service reads pending once, rejects bits above 7 and writes the exact
nonzero snapshot to acknowledge. The wait helper services one allocation at
a time and checks the original absolute monotonic deadline, including time
spent inside the allocation callback. Servicing never starts a new timeout.

The caller serializes all entry points. Initialize a fresh transport only
before firmware starts or after a proved reset. The pool arena is monotonic
and returns ``-ENOSPC`` when exhausted; command storage is not recycled.
Successful reply bodies remain in the pool transaction span. A timeout,
malformed firmware tuple, allocation failure or firmware refusal latches an
error, retains the transaction and mappings and stops further publication.
No surface is freed by these helpers, including a rejected allocation result.
The backend must retain mappings, power and module references until reset.

The field contract follows the clean H16/H17 cold-boot facts, sections 1,
7 through 11, and the SPEC-r2 channel and deadline rules. Allocation limits
are conservative host policy observed sufficient on H17, rather than
firmware-advertised authority. The KUnit suite uses synthetic memory and
accessor traces, with no MMIO or hardware access. Its configuration fragment
is ``drivers/accel/ane/transport.kunitconfig`` and runs on x86_64 and arm64.
