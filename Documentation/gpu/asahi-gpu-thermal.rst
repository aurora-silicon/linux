.. SPDX-License-Identifier: GPL-2.0-only OR MIT

GPU thermal integration and qualification
=========================================

Status
------

This branch is an incomplete cooling integration, with a deliberate safety
interlock. J700 requests GPU cooling in DT. Both firmware ABI variants still
lack a qualified allocation-relative ceiling layout, so J700 GPU probe fails
with ``EOPNOTSUPP`` before either GPU processor boots. This is expected and
must not be presented as successful hardware throttling. Existing G16S DTs
without cooling bindings retain their existing behavior.

J700's SMC zones have independent critical shutdown trips. They register
through hwmon/thermal even when the GPU driver is absent or probe fails.
The firmware backstop target is provisionally lowered to 95 C with its
80 C release, sensor mask and gains preserved. It cannot take effect while
GPU probe is refused. Polling SMC at 100 ms does not accelerate its source
refresh or establish a fast transient safety guarantee.

Source contract and unresolved ABI
----------------------------------

G17P/T8140 and G16S/T6040 use the same ``g17`` backend. G16G/T8132 and its
PMP cooling consumer are a separate backend.

The clean facts from AGXG17G 353.14, macOS 26.6.2 build 25G83,
``AGXArmFirmware::setGPUMaxPower``, describe three synchronous u32 writes,
clamped to 0..100 percent, at offsets ``0xf150``, ``0xf2d0`` and ``0xf39c``
from a power-control object. No mailbox, sequence or acknowledgement is
specified. This ceiling is separate from the effort floor, PMP and CLPC.

Those offsets alone are insufficient to choose a Linux allocation. They
exceed both existing power configuration structures: 26A428 is ``0x2dc0``
bytes, and 25G83 is ``0x6510`` bytes. The shared bundle also contains work
rings and control data; fitting in that larger allocation is not proof of
field ownership. The clean facts do not resolve the pointer chain from
that power-control object to the bundle, status block, firmware data or
another allocation. No G16S-specific ceiling layout or lowest validated
nonzero percent endpoint is established.

``fw::Variant::power_ceiling`` therefore remains ``None`` for both builds.
Do not populate it by adding those offsets to an arbitrary mapped base.
A follow-up must supply a facts-only pointer/ownership contract and exact
per-build layout, validate the mapping and memory attributes, and then
populate the ABI tables. DT never supplies addresses or firmware offsets.

Common cooling owner
--------------------

The common implementation accepts three checked, distinct, aligned WC
words from an admitted firmware ABI table. It validates every destination
before writing any of them. A per-device mutex serializes all three stores
and a write barrier; callbacks do not acquire the GPU mutex or wait for a
completion. Registration publishes a 25 percent backstop before INIT,
port attachment and work publication. Recovery republishes the strictest
retained cap before reopening queues. Stop and teardown unregister and drain
thermal/sysfs callbacks before stopping the processors or releasing mappings.

Cooling state N requests ``100 - N`` percent for N in 0..100. It is an
interface percentage, not an OPP selector or measured watts. The thermal
framework composes multiple zones by maximum state. The effective host
publication also cannot exceed the 25 percent qualification backstop.
There is no userspace knob to release it. Failed publication does not commit
a new request or published value. Matching host words are never called
applied or achieved; the diagnostic attribute labels both unsupported.
A firmware-reported pstate alone cannot acknowledge this percent ceiling.

Provisional J700 outer policy
----------------------------

These thresholds are a conservative Linux bring-up proposal, not recovered
Apple board limits or safety ratings. Independent zones make the most
restrictive correlated sensor win; they do not imply that SMC keys are
GPU-local MTR sensors.

=================== ===================== ============= ========
Zones               Cap temperatures (C)   Hot (C)       Critical
=================== ===================== ============= ========
Tp*/Tpx*            85, 88, 92, 95         92            98 C
Charger (TCMz)      80, 83, 88, 92         88            95 C
Chassis (Ts0A/I/M)  70, 75, 80, 85         80            88 C
=================== ===================== ============= ========

The four cap requests are 75, 50, 25 and 0 percent, with 5 C hysteresis.
The final zero request is deliberately stricter than the proposed lowest
validated nonzero endpoint, which remains unknown. Zero has no demonstrated
hardware stop semantics; critical shutdown must never depend on that request.
Do not replace zero with an invented minimum pstate or a percentage inferred
from an OPP's power. J700 GPU initialization remains refused independently.

DT uses ``active`` trip types for this non-power-actor ladder, with equal
lower/upper cooling states. The tree's power_allocator governor explicitly
services such active instances, and step_wise also honors these exact
bounds. Passive trips on a device without power-actor callbacks would not
implement the requested cap under power_allocator. Hot trips carry policy
information; independent ``critical-action = "shutdown"`` provides the
shutdown action without any GPU callback.

Offline verification
--------------------

Run the same production mapping and fake-publication suite on the host::

  python3 tools/testing/selftests/drivers/apple/gpu-cooling.py

Run it in arm64 QEMU without a GPU or Rust toolchain::

  python3 tools/testing/kunit/kunit.py run --arch arm64 --jobs 8 \
    --kunitconfig drivers/gpu/drm/asahi/.kunitconfig.cooling \
    --make_options LLVM=1 'asahi-gpu-cooling*'

The suite covers all 101 states, invalid endpoints, all pairs of demand and
budget, startup restriction, identical mirror transactions, rejected
publication, and restoration after reset. It does not qualify a live ABI,
firmware acceptance, a lowest nonzero endpoint, or a thermal response time.

Compile ``apple/t8140-j700.dtb``, then verify sensor identities, all cap maps,
hysteresis, polling and critical actions in the flattened tree::

  python3 tools/testing/selftests/drivers/apple/j700-thermal-dt.py \
    --dtb out/arch/arm64/boot/dts/apple/t8140-j700.dtb

Owner one-shot plan
-------------------

No device run is performed by the implementation worker. The owner first
reviews the missing ABI/endpoint facts and requests a device-specific test
through the controller. Preserve a known-good kernel/DTB and bootguard's
one-shot fallback. Record exact candidate and baseline source commits,
firmware identities, kernel, DTB, Mesa, workload, power source and ambient
conditions. Preserve hashes and the rollback entry before staging a candidate.

For this incomplete revision, the only candidate check is a no-load boot:
verify independent thermal zones, critical trips and the expected GPU probe
refusal, then restore the known-good entry. Do not use this revision for a
rendering or performance acceptance test and do not remove its cooling
binding to bypass the interlock.

After a qualified ABI and nonzero endpoint are integrated, rebuild and review
that exact candidate. Prove all three mirror publications and independent
achieved pstate/power readback with the pre-armed 25 percent ceiling. Prove
sensor-error handling, restrictive reset/recovery, callback teardown and the
independent shutdown path with no heat-generating workload first. Keep missing
or implausible fast-die information restrictive; repeated SMC values alone do
not identify a stale source.

Use existing baseline traces from the current kernel; do not recreate an
uncapped baseline. With a separately approved capped baseline and candidate,
start at matched cool temperatures and issue only short, bounded bursts
(initially at most 100 ms), one workload at a time. Keep a temperature
supervisor independent of GPU progress and prove its stop latency before
load. Do not extend bursts if source-refresh and stopping delay cannot bound
overshoot below every critical threshold and 100 C. No sustained games,
full CTS, or multi-minute benchmarks belong to this plan.

Capture monotonic timestamps, all relevant SMC temperatures and read errors,
firmware die readings if qualified, requested/published/applied cap quality,
achieved pstate or power, frame times and limiting reasons. Never substitute
zero for unsupported telemetry. Compare the same workload's frame-time
p50/p95/p99/max and throughput against the capped current kernel, together
with peak temperatures, time above each trip and cooling residency. A 25
percent retained backstop is expected to affect throughput; report that limit.

Stop on missing telemetry, a failed cap update, a lost binding, GPU fault,
excessive excursion or an independent supervisor action. Retain the strongest
cap, preserve failure traces and restore the known-good kernel/DTB through
the fallback. Verify the restored boot identity, zone identities and cooldown.
Record the rollback as a failed qualification, not a passing result. Sustained
load remains outside authorization until a separate thermal qualification.
