.. SPDX-License-Identifier: GPL-2.0-only OR MIT

Asahi G16 on M4
===============

The J713/T8132 GPU uses the shared Rust Asahi DRM driver with the G16 firmware
backend in ``drivers/gpu/drm/asahi/g16``. The backend implements the generation's
firmware records and page-table handoff; userspace uses the public Asahi DRM
interface and native AGX3 command streams. The common Apple display driver owns
DCP and scanout; see :doc:`apple-dcp-m4`.

Firmware admission and ownership
-------------------------------

Startup accepts only a qualified board and firmware layout. It validates the
preserved firmware image, original ADT performance tables and inherited memory
before publishing work. Other GPU generations retain their existing backends.
An unrecognized firmware layout must not become accepted through a userspace
version or renderer override.

AP changes to shared page tables use one host exclusion guard and the firmware
handoff protocol. Mapping lifetime follows command retirement and authenticated
owner receipts. Unpublished submission failures can release their private
resources. A timeout or ambiguous firmware-visible result cannot establish that
firmware stopped accessing memory, so the affected owners are retained.

Compute, TA and fragment work records have firmware and kernel GPU mappings;
application GPU address spaces have no aliases of those pages. Their numerical
addresses remain reserved against application binds. Mutable pools, context
storage and auxiliary working buffers are private to each VM, with fresh backing
for another VM even at the same numerical address. Session UMA and parameter
initdata have only their context-0 lower aliases. This closes the former shared
scratch disclosure; it does not establish that every firmware parser is safe
against a client corrupting its own GPU-writable metadata.

Private compute and render backing is allocated lazily in SUBMIT, before an output
fence or accepted ordinal exists. The fixed initial arenas cost approximately
65 MiB for compute and 0.7 GiB for render per VM. Physical memory remains charged
to the submitting memory cgroup. VM count is retained through deferred arena
teardown; no firmware-visible alias outlives its ASID lease. Prepared render epochs
are capped at 256 per VM, one hardware queue's capacity, including jobs blocked on
input fences. Admission waits interruptibly for deferred unmap to return credits,
with a ten-second administrative deadline, before allocating more epochs. Expiry
returns EBUSY without making the VM sticky-failed; a signal restarts admission
without modifying output syncobjs. Each
DRM file may retain at most eight VM bindings, including deferred arena owners.
The limit does not constitute a global RAM quota: use memory-cgroup limits where
untrusted clients may otherwise consume the machine's available memory.

Each render command receives fresh NC PB manager/list/InitBM records. A full InitBM
installs that binding before TA. The prior PB-slot and VM leases remain until the
replacement has complete normal TA/fragment, allocator and queue/pipe receipts.
Firmware-mutated UMA and PB state is never reset by the host. The conservative
two-position global lifetime ledger is preserved. Its quarantine bound comes from
loss of that hardware class's availability; an abort can therefore disable that
class for all clients until reboot. Restoring availability with per-arena quarantine
would also require an explicit quarantined-memory limit and guilty-client policy.

Submission and synchronization
------------------------------

Commands are validated before admission, with real input fences checked at
command boundaries. Exact queue-local retirement frontiers satisfy historical
prefix dependencies. Native stream checks bound obvious malformed walks and
provide diagnostics; GPU-writable command buffers, indirect execution and
shaders prevent them from being a security boundary or proof of termination.

Firmware publication remains serialized. Command timestamps are written at
retirement, with overlapping CPU destinations serialized by buffer-object
ownership. Feature bits describe implemented generation-specific contracts;
sharing the public interface does not imply every G17 feature is available.

Queue failures and watchdogs
----------------------------

A failed producer input fence becomes ``ECANCELED``. The queue records the first
cancelled ordinal for each affected engine. Commands at or beyond that ordinal,
or depending on its failed prefix, cancel; earlier independent accepted work
continues. Accepted ordinals are not synthetically skipped or retired. Kernel VM
admission, mappings, other queues and newly created queues remain available.
The VM status mirror still reports accepted-work failure; userspace can elect to
treat that report as device loss.

``g16_deadline_ms`` controls the host administrative watchdog for render or
mixed jobs; ``g16_compute_deadline_ms`` controls compute-only jobs. Both default
to 5000 ms and zero disables the respective watchdog. Lack of completion-stamp
progress is not proof that a shader is stuck. Legitimate longer compute work
must select a larger compute deadline or zero; with the watchdog disabled, a
silent hang can require reboot. Diagnostics warn after five seconds even when
the watchdog is disabled. Firmware recovery and thermal control remain active.

A host timeout retains uncertain firmware owners and mappings and fail-stops.
Error signalling alone does not prove physical quiescence or safe external
DMA-BUF reuse. No qualified physical stop/reset boundary is claimed.

Native completion stamps wrap through zero after ``0xffffff`` wire ordinals.
Software ordinals and retirement frontiers remain 64-bit. Modular comparisons
require an ambiguity window smaller than ``2^23`` commands; serialized firmware
publication and authenticated full receipts with owner displacement consume wire
dependencies before subsequent reuse. A zero stamp alone is not a retirement
receipt. Creating a new logical queue does not replace firmware graph ownership.
The firmware's ordinary stamp waits use signed modular comparisons, including
zero. This source contract still requires a hardware rollover test.

An exhausted owner class cancels the affected queue and reports degraded health
with a class-specific error; the other class and kernel VM admission can remain
available. Ordinary accepted failures before firmware publication, such as
allocation or validation errors, close only the affected queue and report the
underlying error. A wedged uevent reports class loss or complete fail-stop.
Recovery of quarantined retained resources is unknown; reboot is currently
required. Recovery storms also fail-stop rather than speculatively recycling
firmware-visible owners.

Power management
----------------

The GPU obtains telemetry from the PMP provider and composes its raw-temperature
protection with thermal-governor and package-power requests. Missing or invalid
telemetry selects a restricted state. The power provider owns sensor decoding;
the GPU does not map the firmware telemetry SRAM independently. No qualified
firmware sample timestamp or heartbeat is available: a silently wedged PMP can
leave stale plausible readings. Host receipt time does not prove fresh conversion.

Policy constants and their provenance belong to the PMP policy documentation.
Passing a rendering test does not qualify emergency limits, power removal or
firmware recovery.

Validation and limitations
--------------------------

CPU regressions are under ``tools/testing/selftests/drivers/gpu/asahi-m4``.
Normal kselftest build and installation do not run them or require a Rust
compiler. ``make run_tests`` and ``make cpu`` execute host tests; missing host
prerequisites produce a kselftest skip. The suite compiles complete helper
modules where available; remaining isolated-method harnesses use kernel API
stand-ins and provide narrower coverage than a kernel build or KUnit test.

Frozen encoder fixtures preserve independent historical C outputs. They check
wire-format drift without restoring the removed C driver, and must not be
regenerated from the Rust implementation under test. Their historical inputs
are not a complete stand-alone generator source distribution. The test README
records that limitation and the exact fixture provenance.

Hardware tests require explicit execution after booting the changed kernel.
Verify the exact source, Image and module identity, native display and input,
compositor Mesa mapping, completed native OpenGL work, GPU recovery counters,
unresolved probes, thermal telemetry and firmware diagnostics. Host tests and
a successful kernel build do not establish graphics API conformance.

The retained firmware session has no qualified final release/reset boundary.
Suspend, full power removal, unload and sustained abort recovery require further
qualification. Aborted owners can exhaust retained resources; this is reported
as a limitation rather than treated as proof that memory can be recycled.
Machine-specific packaging, boot-menu changes, watchdog supervision and
one-shot deployment are maintained outside the kernel source tree.

Building on basic bring-up
-------------------------

This series is stacked on the basic J713 platform support. Like every other
Apple machine, J713 builds from the shared configuration: ``make defconfig
asahi.config``, then ``olddefconfig`` before building ``Image modules dtbs``.
``asahi.config`` enables the G16 backend, the T8132 DCP backend and the
retained J713 display DART next to the modular Rust Asahi GPU and Apple
display drivers.

Linux 7.2 uses flat render/compute UAPI declarations. G16 reads their existing
wire offsets and rejects unsupported ray-query and scratch tails. Regression
helpers compare the historical nested declarations against those same wire
prefixes; the independent binary encoder fixtures are unchanged. SPDX headers
on historical source inputs do not change their generated data; the manifest
also records their original hashes before those license notices.

Platform limits
---------------

The retained DCP/DART session has no qualified suspend or physical DMA-stop
sequence. Suspend and hibernate admission is refused while it is owned. Retained
DMA also blocks normal and crash kexec; booting another kernel requires a platform
reboot. These are current platform limits, not a demonstrated firmware reset path.

The two retained display DARTs share one level interrupt. The aggregate action
is attached only after both suppliers have been admitted. A partial or failed
admission leaves that interrupt unattached, so faults in that window are not
observed. Full native-display activation requires both suppliers.
