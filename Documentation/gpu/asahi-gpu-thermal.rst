.. SPDX-License-Identifier: GPL-2.0-only OR MIT

GPU thermal integration and qualification
=========================================

J700 exposes additional SMC SoC, charger and enclosure readings through
reporting-only thermal zones. They have no trips or cooling maps and do not
throttle the GPU. The ``Tp*`` zone names identify keys, not verified physical
sensor locations. Do not infer a GPU junction temperature from their names.

Source contracts
----------------

G17P/T8140 and G16S/T6040 share the ``g17`` backend. The ``g16`` backend is
the separate G16G/T8132 implementation; its PMP consumer and nine-state
ceiling must not be copied as a board policy for J700.

The T8140 configuration selects 26A428. Its power configuration initializes
a die limiter with sensor mask ``0x82a``, a 110.00 C target and an 80.00 C
release point. T6040/25G83 instead initializes its power-state object with
101.00 C and 96.00 C. These describe the driver's firmware setup. They do
not establish macOS board policy, SMC sensor equivalence, or safe charger
and enclosure temperatures.

The live effort input requests a performance floor. Reducing that request
does not establish a maximum performance state. Similarly, a maximum-power
value written while building firmware configuration does not establish a
live power-limit interface. A cooling device needs a qualified ceiling
interface, including units, update ordering and firmware ownership.

Integration requirements
------------------------

Use a single cooling implementation for G17P and G16S, with a firmware
variant supplying any ABI differences. Derive the state range and conversion
from validated performance tables and loader/firmware data. Keep board
trips and cooling maps in DT. Do not add a module parameter or use direct
clock or voltage writes to implement a thermal limit.

The cooling state is an upper bound, independent of the work-driven effort
request. State zero permits the full validated table; increasing cooling
must monotonically reduce the ceiling. Multiple zones must compose by the
most restrictive request, and any other budget must not relax that request.
Report requested and applied limits separately. A successful host write
alone is not evidence of firmware acceptance or achieved frequency.

Thermal callbacks must not invert the GPU lock order. A bounded owner-side
service must enforce updates while already submitted work runs, including
a single long dispatch with no completions. Enforce the startup limit before
the first publication; cover direct submissions, render and compute, recovery,
idle wake and teardown. Firmware state must not be written after its owner
stops. Preserve a restrictive ceiling across provider faults, recovery and
missing policy until revalidated. Define which sensor errors can be detected:
repeated identical SMC values are not by themselves a freshness witness.

SMC polling is an outer loop. Polling faster than the sensor refresh cannot
provide a fast-die backstop. Qualification must establish the independent
fast signal and actuator response before using SMC trips to protect rapid
load transients. Charger and enclosure policy requires its own evidence.

Offline checks
--------------

Build ``apple/t8140-j700.dtb``, then run::

  python3 tools/testing/selftests/drivers/apple/j700-thermal-dt.py \
    --dtb out/arch/arm64/boot/dts/apple/t8140-j700.dtb

This resolves each compiled sensor reference to its key, including inherited
temperature children. It also checks the six existing CPU references. A
missing SMC key keeps its channel slot in the driver; it must never silently
move a later zone to another key. Exercise that driver behavior and the SMC
readers without an SMC using::

  python3 tools/testing/kunit/kunit.py run --arch arm64 \
    --kunitconfig drivers/hwmon/.kunitconfig.macsmc 'macsmc*'

Ceiling implementation tests must additionally cover every valid state and
invalid endpoint, composition of simultaneous requests, startup restriction,
provider failure and recovery, publication ordering and teardown races. Run
the same production policy in host tests and KUnit with a fake actuator that
can reject or delay updates. Tests must verify that failed application cannot
be reported as an applied limit.

Owner qualification
-------------------

First record the exact board, kernel, DTB, Mesa and both firmware versions.
Verify every zone's key identity, cooling binding and state range without a
GPU load. Missing sensors, missing bindings or an unconfirmed applied ceiling
must prevent progression to load tests. Retain the known-good boot entry and
one-shot boot fallback throughout qualification.

On matching macOS and Linux configurations, record synchronized sensor,
power, requested/applied state, achieved frequency and frame-time traces.
Use the same workload, resolution, presentation mode, ambient conditions,
power source and comparable starting temperatures. Record sensor update
cadence, missing values and timestamp domains. Preserve raw traces for the
owner; publish only reviewed summaries without device identifiers.

Before a Linux GPU experiment, obtain the required device approval and arm
a verified restrictive ceiling and independent temperature supervisor. Prove
the stop action and its latency before generating heat. Start with short,
bounded work at the lowest qualified ceiling. Stop before a non-CPU reading
reaches 100 C, with additional margin for refresh and stopping latency. The
owner's absolute abort rule also applies; it is not a normal control target.
If the available sampling cannot bound overshoot, do not extend the load.

Once the actuator and trips have been independently qualified and integrated,
run vkmark cube for ten minutes, glmark2 terrain for ten minutes, and a fixed
game replay. Include warm starts, mixed CPU/GPU load, idle transitions and
both supported firmware variants. Plot temperature and limiting reason
against frame time; report maxima, time above each trip, cooling residency,
frame-time percentiles and sustained throughput. Require every relevant
non-CPU sensor to remain below 100 C and the derived board limits, with no
GPU fault or lost binding. Compare matched macOS traces without treating
unavailable telemetry as zero. These tests remain pending until the cooling
actuator and board policy exist.

On any failure, stop the workload, retain the restrictive ceiling, preserve
traces and restore the known-good kernel/DTB through the boot fallback. Check
the restored identities and temperatures. A rollback is evidence of a failed
qualification, not a passing sustained-load result.
