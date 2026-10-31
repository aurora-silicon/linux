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
the strict preload validator, original-container HWX ABI, firmware process
resource decoder and cold boot backend are incomplete. The section-only
load ABI is explicitly refused for H16. No accel node or program-loading interface is published.

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

H16 task streams begin with little-endian ``u32 1`` and twelve reserved zero
bytes. This marker precedes the first aligned task; it is not a task count.
The initial H16 gate rejects a zero marker, other values and nonzero reserved
bytes. The public H16 encoder fixes this prologue at commit
``927013f5480768b07c8e62a866e1514c41aba778`` of
``joshuaswarren/mil-hwx-compiler``, in
``plugins/H16G/Encoding/H16GALUEncoder.mm``.

H16 validation helpers walk nine fixed task-header words, the documented
conditional extra word, dense/scatter records and the nine allowed register
blocks. A separate helper checks the eight-byte Mach-O relocation entries,
unique aligned offsets, the actual kernel-section ordinal and addend bounds.
Original-container, tensor, program and aperture validation is provided
separately as described below.

The device-free selftest in ``tools/testing/selftests/drivers/ane`` generates
the SPEC-r2 fp16 add-128 inputs and CPU oracle and verifies all three pinned
buffer hashes. It can also check a supplied HWX against the pinned size/hash.
It never opens an accel device or claims a Linux inference result.

Remaining implementation gates
------------------------------

SPEC-r3 supplies the custom descriptor/aperture layouts and a normalized
immutable preload digest with 42 ordered patchbay records. The complete
preload validator, original-container ABI, process-resource decoder and
physical boot backend still need implementation. Off-list preload mutations
remain refused.

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
their mapping to channel names remains unproved. The parser retains each
descriptor's original index rather than assuming name order is wire order.

The transport prepares the H16 envelope and rejects unrecognized runtime
IDs, wrong sizes, channels and power-gate values before publication.
H14's 0x0204 procedure packet and short CREATE_PROCESS body remain H14-only;
H16's serializer and UUID completion table are connected to the shared
submission and event paths, subject to validated process resources.
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

The shared H16 submission path allocates a non-reused 64-bit UUID, installs
it in a bounded pending table before publishing channel 7, and waits for
both the transport reply and its own terminal event. Events may arrive
before the acknowledgment. States 0 and 3 leave the UUID pending; states
1 and 2 retire it once after matching every identifier. Unknown, duplicate
and late UUIDs cannot retire another request. An identifier mismatch for a pending UUID or a malformed report
quarantines the queue. ISP-destination reports and short
CPU-load notifications cannot complete a general-scheduler request.

One absolute deadline covers acknowledgment and terminal completion; a
report at or beyond that deadline cannot succeed. Failure state 2,
transport refusal, timeout or corruption prevents output copy-back and
quarantines the queue without retry or reset. Firmware-visible process
mappings remain resident, including after file close. Closing a file
revokes its process capabilities before freeing its staging BO handles.

Only private coherent DMA mappings may appear in H16 execute descriptors.
Userspace BOs serve as staging buffers: inputs are snapshotted before
publication and outputs are copied back after validated terminal success.
Write barriers order input and command stores before publication; DMA read
barriers order output reads after completion. Streaming DMA sync APIs must
not be applied to these coherent allocations. There is no userspace alias
to code, scratch or live firmware buffers.

The resource map must come from the original HWX and a decoded firmware
process reply. A userspace index, numeric class or program/process ID cannot
create that map or confer ownership. Only established input/output classes
are admitted; custom BAR requests require a validated selector map and
remain unavailable. The legacy 0x28-byte completion predicate remains
H14-only. Channel-5 allocation/control/free operations require a validated
H16 reply provider and remain unavailable; H14 allocation replies are never
reused for H16.

No original-container load ABI or H16 process-resource decoder is supplied
by this change. Consequently no H16 process can yet be installed, inference
remains refused and no accel node is registered. The submission integration
is preparatory and does not open the boot, preload, ownership, firmware
version or resource gates.

KUnit covers byte-exact synthetic execute fields, all 1..64 buffer counts,
zero/one/two custom BAR lengths, wide DVAs/spans, reserved-zero output,
boundary and overflow refusal, all event states, interleaved identities,
ISP routing and malformed reports. No test packet is copied from a capture.

Original HWX validation
-----------------------

The validator consumes one kernel-owned immutable snapshot of the complete
HWX. It requires subtype 7 and bounded, exactly consumed load commands;
rejects unknown command kinds, duplicate descriptors, overlapping segments,
sections and file regions; and validates symbols against the complete
section-ordinal table. Tensor names, directions, normalized fp16 dimensions,
overflow-checked strides and full batched spans must agree with their
surfaces and buffer references. There must be at least one input and one
output, with the single output last.

The linear and scratch program descriptors have exact fixed fields and
zero reserved bytes. All five 16-byte aperture records must equal whole
surface segment bases in order, followed by the optional kernel base;
reserved halves and unused records are zero. Interior, duplicate, reordered
and scratch/text aperture addresses are refused. Task counts and word counts
must agree with the independent stream walk. Relocations use the actual
kernel section ordinal, unique bounded offsets and valid addends, and may
patch only packed-record payload words. Headers, register selectors and
padding remain immutable.

SHA-256 covers the entire original snapshot, including load commands and
padding. The snapshot digest is rechecked before execution. Validation
provides metadata, not allocation authority or permission to load a program;
the original-container ABI and firmware resource map remain required.

The hardware-free KUnit suites cover pending UUID lifecycle, acknowledgment
versus completion, interleaving, progress, failures, malformed and mismatched
reports, routing, duplicates, timeouts, capacity and UUID exhaustion. HWX
tests generate synthetic linear/scratch forms with optional kernel data,
then exercise command, geometry, tensor, aperture, symbol, relocation and
digest refusal paths. No hardware inference result is implied.

H13 timeout and engine recovery
==============================

The host task-manager path (``ane.ko``) gives each submission one second,
including DART fault-drain retries. An expired submission returns
``-ETIMEDOUT``; recovery never silently retries a program whose output may be
partial. The task's GEM lookup references are released when the ioctl returns.

Recovery holds the engine and PM-entry locks, drops the current consumer
runtime-PM reference, synchronously suspends its power-domain suppliers, and
checks both the PMGR ACTUAL fields and every attached genpd for power-off.
The SET window omits the shared SYS_CPU parent, whose DART consumers can
suspend asynchronously; it must also turn off before the consumer resumes.
Recovery then restores the consumer
reference, checks power-on before engine MMIO, and runs the probe task-manager
enable sequence. BO handles and mappings remain valid for resubmission.
The system-sleep force helpers are unsuitable here: a forced, in-use supplier
can retain its active runtime status and keep its parent domains powered.

A power-off acknowledgment is required before reinitializing the engine.
Only after power-on and task-manager readiness are also verified does recovery
release the exceptional module pin and reclaim preserved BOs. If any step
fails, mappings, backing pages and the pin remain held; new submissions fail.
This also covers a partially resumed engine whose retained queue state is
uncertain. The ``reset`` sysfs write retries recovery. Module reload cannot
recover a device while that pin is held. Direct PMGR SET writes and speculative
writes to undocumented task-error registers are not part of this reset path.

The ``ane-pm-recovery`` KUnit suite exercises the shared PM transaction against
real runtime-PM device links, including repeated cycles, single-domain devices,
consumer/supplier suspend errors, a missing power-off acknowledgment and a
failed resume. It checks that every exit restores the caller's PM reference.
The minimal config is
``tools/testing/selftests/drivers/ane/pm.kunitconfig``.

Destructive recovery selftest
----------------------------

``tools/testing/selftests/drivers/ane/recovery.py`` is an explicitly invoked
hardware test, excluded from the default device-free selftests. It requires
exclusive ownership of an M1-family ANE, Python 3, the packaged H13 add fixture,
and a deliberately invalid convolution program. Run it through the hardware
controller with a watchdog and kernel-log capture::

  python3 recovery.py /usr/share/omarchy-ane/fixtures/h13-anec/add/program-0.anec hang.anec

The test keeps both clients open, submits the invalid program three times,
requires a bounded ``ETIMEDOUT`` and ``wedged=0`` each time, and runs twenty
bit-exact add jobs on the surviving client's original fd and mappings after
each reset. Every call changes its inputs and poisons the valid output lanes,
so stale output cannot pass. Its oracle uses exact fp16 quarter values and checks the complete
output, including padding. Closing both clients must return the module
reference count to its initial value; the boot identity must remain unchanged.
An operator should also verify three power-off acknowledgments in the kernel
log and a normal module unload/reload after all clients close. A failed reset
is a test failure, not permission to force unload the module.

Generate ``hang.anec`` on a build host with ``make-hang-fixture.py`` and a clean
checkout of the MIT-licensed public compiler at commit
``ef2dbceefedff38ecce4f3dc593e148c5e7a6d54`` from
https://github.com/aurora-silicon/mil-hwx-compiler/pull/2::

  python3 make-hang-fixture.py /path/to/mil-hwx-compiler hang.anec --cxx clang++

This first checks that the normal encoder refuses the shape. It bypasses only
that refusal in a temporary translation unit to generate a deterministic
stride-2 1x1 128-to-256 convolution at 56-to-28 spatial size, the refused family
reported to hang the M1 Pro. The compiler checkout is unchanged. No Apple
binary or private program capture is used. The generator writes a hash receipt;
the hardware test must fail if this particular instance does not actually
produce a timeout. The loader pins both fixture hashes and uses ABI 1 ioctls directly, so no
shared libane library is needed. ``--check-fixtures`` validates the two containers without
opening a device.

H14 recovery lifecycle
----------------------

The firmware path (``ane_t6021.ko``) uses one bounded deadline for the command
acknowledgment and its finish event. H14 engine reset is not enabled by this
change. Reusing only the H13 task-manager re-arm would leave the firmware and
its program cache invalid. The corresponding recovery implementation needs to:

* Serialize firmware commands and stop the RX worker before releasing the
  consumer's runtime-PM reference and synchronously suspending its suppliers.
* Verify power-off for the CPU and engine partitions through their providers
  before dropping boot pins or reclaiming firmware-visible allocations.
* Retain metadata for every firmware-owned BO and boot allocation, including
  objects whose last userspace reference disappeared. The current release
  path discards some of this metadata while retaining the backing memory.
* Restore the validated firmware image and run the existing fenced probe boot
  sequence, including channel-table validation, host acknowledgment and
  CONFIG_GET. A failed boot remains quarantined with its DMA backing held.
* Invalidate the cached program/process IDs across the restart and prevent a
  stale userspace ID from aliasing a newly allocated firmware ID. Clients must
  reload their programs before resubmitting.

Those ownership and identifier changes require their own H14 reset test.
Quarantine must not be cleared solely because a power transition returned zero.
