.. SPDX-License-Identifier: GPL-2.0-only OR MIT

GPU thermal integration and qualification
=========================================

Live power owner
----------------

G17P/T8140 and G16S/T604x share the ``g17`` backend. The admitted live
actuator publishes an absolute ``u32`` milliwatt ceiling to the separately
allocated ``RegionC::max_power_mw`` at +0x84 for G17P 26A428 and G16S 25G83.
These are firmware fields, not offsets in a host service object. The obsolete
host-object mirror model has been removed. G16S 26A428 has a distinct typed
+0x88 prefix but no admitted initialization graph or live actuator. V25F84
also has no live-power admission. Never reuse the common field for either.

``PowerState::ppm_enabled`` (+0x128) and ``ppm_target_mw`` (+0x12c) are
separate typed fields in another object. The owner retains the initialized
PPM budget and bounds its published target by the composed ceiling. It never
aliases the RegionC maximum with the PPM target, a percent, an OPP, a PMP vote
or the performance effort floor. A zero/uninitialized PPM budget uses the
initialized RegionC maximum; a zero RegionC maximum rejects registration.
G16S uses the initialized per-chip maximum, not the zero placeholder in its
static configuration.

A dedicated mutex serializes thermal, fast-die host-policy, system-budget
and manual request slots and their publications. The thermal framework
aggregates zone demands by maximum cooling state. The owner takes the minimum
of all absolute requests, so one requester relaxing cannot lift another's
ceiling. All future host budget/manual producers must use this owner rather
than write the fields directly. The firmware fast-die limiter operates
independently and is not replaced by the host fast-die request slot.

State N requests ``max(25, 100 - N)`` percent of the initialized maximum,
rounded down in milliwatts, with a minimum of one milliwatt for tiny test
inputs. The 25 percent floor is **provisional**, not a qualified board minimum
or a claim about a physical pstate. State 100 does not publish zero or stop
the GPU. Absolute requests below the provisional floor are rejected, preserving
the previous restriction. Critical shutdown remains independent of GPU work.

Registration publishes the floor before INIT, port attachment and submission;
its initial cooling state is 100. The governor may subsequently relax its own
request. The previous applied value and all requests survive a failed
publication. The current mapped stores are infallible after registration
validates every destination. Recovery republishes the committed restriction
before reopening consumption. Teardown drains debugfs, sysfs and thermal
callbacks before stopping processors or freeing mappings.

RegionC is CPU write-back memory, so publication includes a clean to the point
of coherency. G17P PowerState also needs cleaning; G16S 25G83 PowerState is
write-combined. Store ordering is completed before host publication is
reported. There is no firmware mailbox, doorbell or acknowledgement for these
inputs. Shared-memory readback proves publication, not achieved GPU power.

Fast and outer protection
-------------------------

Before submission opens, the G17 firmware die limiter is initialized with
sensor mask ``0x82a``, target 95 C, release 80 C and its native 16 ms period.
The existing gains are preserved. These temperatures are provisional Linux
bring-up policy, not device safety ratings. The intentional missing-actuator
J700 initialization refusal is removed for the admitted layout; invalid
mappings, absent initialized power and unknown firmware still fail closed.

J700 has independent SMC thermal zones and critical shutdown actions:

=================== ===================== ============= ========
Zones               Cap temperatures (C)   Hot (C)       Critical
=================== ===================== ============= ========
Tp*/Tpx*            85, 88, 92, 95         92            98 C
Charger (TCMz)      80, 83, 88, 92         88            95 C
Chassis (Ts0A/I/M)  70, 75, 80, 85         80            88 C
=================== ===================== ============= ========

Cooling states 25, 50, 75 and 100 apply 75, 50, 25 and the provisional
25 percent floor, with 5 C hysteresis. The last two states deliberately
coalesce until hardware qualifies a lower minimum. DT uses ``active`` trips
with equal lower/upper states, supported by both power_allocator and
step_wise for this non-power-actor device. SMC polling at 100 ms cannot
accelerate source refresh and does not replace the fast firmware limiter.

Diagnostics
-----------

``/sys/devices/platform/.../gpu_power_ceiling`` reports the requested thermal
state, composed requested and applied ceilings in mW, and provisional floor.
``/sys/kernel/debug/asahi-<device-name>-power/ceiling`` additionally reports
the initialized maximum, both live words, PPM enable and each request slot
(0 thermal, 1 fast-die host policy, 2 system, 3 manual). Applied means the host
completed the shared-memory publication; achieved power remains explicitly
unsupported. These diagnostics are read-only. A firmware pstate alone is not
an acknowledgement of either power input.

Offline verification
--------------------

Run on a build host, without a GPU::

  python3 tools/testing/selftests/drivers/apple/gpu-cooling.py

The host suite exercises the production C policy plus the exact typed Rust
PPM and unadmitted G16S 26A428 layouts. It covers all 101 states, floor
rounding, integer limits, every thermal/non-thermal request pair in both
orders, four-owner composition, separate PPM budgets, startup, failed
publication and recovery. Run the same C suite under arm64 QEMU::

  python3 tools/testing/kunit/kunit.py run --arch arm64 --jobs 8 \
    --kunitconfig drivers/gpu/drm/asahi/.kunitconfig.cooling \
    --make_options LLVM=1 'asahi-gpu-cooling*'

Compile ``apple/t8140-j700.dtb`` and check the outer policy::

  python3 tools/testing/selftests/drivers/apple/j700-thermal-dt.py \
    --dtb out/arch/arm64/boot/dts/apple/t8140-j700.dtb

These tests do not qualify firmware-achieved power or a safe minimum.

Owner Neo one-shot plan
-----------------------

This revision performs no device test. Request a separate controller-approved
Neo one-shot, identifying the device, exact source and boot artifacts. Build
with the unique release suffix ``-aurora-t5062-gpu-cooling-v2-01``; verify
``uname -r`` matches it after boot. Preserve the fleet-default kernel/DTB,
artifact hashes, boot entry and bootguard fallback before staging. Record
candidate and fleet commits, firmware identity, Mesa, workload, power source,
ambient conditions and starting temperatures. Keep private lab records outside
the public source tree.

First boot with no GPU load. Verify the cooling device, independent thermal
zones, all critical trips, requested/applied/readback agreement and nonzero
PPM target. Set state 100 and verify the initialized maximum's 25 percent
floor (2387 mW for a 9550 mW maximum), never zero. Check states 75, 50, 25 and
0 at no load, returning to 100 after each. Confirm the PPM word remains a
separate target and never exceeds its retained budget or the applied ceiling.
Test that one thermal zone relaxing cannot release another zone's demand.
Capture invalid-state rejection, restrictive reset/recovery and callback
teardown. Prove the independent supervisor's stop latency before applying load.

Use existing fleet-default traces for the first comparison. If matched traces
are unavailable, request a separate bounded baseline test; do not recreate a
sustained unrestricted baseline. For both fleet default and candidate, record
the actual cap and compare identical workloads from matched cool temperatures.
Start with at most 100 ms GPU bursts, one workload at a time, with state 100
applied. Only step toward higher allowed power after readback and cooldown
succeed. No games, full CTS or multi-minute benchmarks in this one-shot.

Capture monotonic timestamps, requested/applied RegionC ceilings, PPM target,
all relevant temperatures and read errors, available firmware die telemetry,
achieved pstate/power quality, frame times and limiting reasons. Compare
frame-time p50/p95/p99/max, throughput, peak temperature, time above each trip
and cooling residency against fleet default, labeling different power caps.
Readback is not achieved-power evidence. Qualifying a lower floor requires
independent achieved-state evidence and a separately reviewed test.

Stop bursts on a failed cap update, readback mismatch, missing or implausible
telemetry, lost binding, GPU fault or supervisor action. Keep the strongest
request, preserve traces and restore the fleet-default kernel/DTB using the
one-shot fallback. Do not extend bursts unless source refresh and stop delay
bound overshoot below every critical trip and 100 C. After rollback verify
the restored release, boot entry, zone identities and cooldown. Retain failure
and rollback evidence; a rollback is a failed qualification, not a pass.
