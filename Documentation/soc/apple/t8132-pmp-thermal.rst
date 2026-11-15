.. SPDX-License-Identifier: GPL-2.0-only OR MIT

T8132 PMP temperature ownership and policy status
================================================

The PMP driver owns its firmware, SRAM mapping and temperature records.
Consumers must use ``apple_pmp_telemetry_get_hotspot()`` with a retained PMP
device-tree node reference. They must not map a physical SRAM address or
restart the PMP themselves. Provider removal drains RCU readers before
releasing its mappings. A stopped provider, an observed firmware crash or
inconsistent records returns an error; cooling consumers retain their
restricted state on an error.

These checks establish ownership and record consistency. The ASC RUN bit and
the temperature valid flag do not establish firmware progress. No advancing
T8132 temperature counter, timestamp or heartbeat is qualified in this driver.
An unchanged temperature is not evidence of a hang, so the driver does not
invent a change timeout. Silent firmware freezes remain a qualification gap.

J713's temperature topology has been exercised on hardware, but its Linux
cooling and measured-rail budget are provisional operating policies. The
temperature trips, power envelope and unmanaged CPU ceiling must not be
described as Apple firmware limits or thermal parity with another SoC. T8132
does not currently have a qualified effort lane in the PMP report driver.

The historical emergency shutdown threshold is kept in
``pmp-emergency-policy.h`` separately from ordinary cooling tuning. This
separation does not qualify that threshold: it remains an existing development
restriction pending source-backed emergency policy. Removing it merely because
its contract is unknown would weaken the deployed backstop. An observed macOS
peak or successful desktop test does not establish an emergency limit.

CPU protection startup retries missing cpufreq policies independently of
platform deferred probing. The ``ready`` module parameter becomes true only
after both cooling devices, minimum-frequency QoS guards and thermal zone
bindings are installed and cpufreq ownership is transferred. Until then the
cpufreq driver's unmanaged ceiling remains in place. Last-CPU offline on T8132
keeps the policy's table and ownership alive; thermal removal restores the
unmanaged ceiling before removing its guards.

Before presenting this policy as complete firmware-driven thermal integration,
qualify the T8132 firmware effort or limit contract and an advancing liveness
source. Boot and thermal-policy tests must check ``ready`` and provider errors,
including startup ordering, whole-cluster hotplug and policy removal.
