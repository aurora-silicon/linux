.. SPDX-License-Identifier: GPL-2.0-only OR MIT

Apple T600x DCS bandwidth votes
==============================

The PMGR miscellaneous driver can provide one shared interconnect path for
host-owned T600x DCS. Every client describes the path ``<&pmgr_misc 0>`` to
``<&pmgr_misc 1>`` and uses the usual interconnect consumer API. Average
requests sum with saturation; peak requests take the maximum. The larger
aggregate selects the lowest measured OPP that can satisfy it. Requests above
the largest OPP fail with ``-ERANGE`` instead of silently under-voting.

The provider is enabled only by an explicit ``operating-points-v2`` phandle
and ``#interconnect-cells = <1>`` on the existing PMGR miscellaneous node. Its
table uses ``operating-points-v2-apple-dcs``. Each enabled OPP supplies a raw
``opp-level`` and one ``opp-peak-kBps`` value. The driver validates duplicate
states, zero or decreasing capacities, the T600x sleep floor of 7, and the
boot-state ceiling before publishing the provider. It does not derive states
from child order or convert firmware thresholds to bandwidth. Existing DTs
without this table retain their boot and suspend policy.

The highest table entry must equal the boot desired state. That entry remains
the initial vote until interconnect sync_state releases boot constraints.
Nonzero bandwidth also holds the fabric at its boot state so untracked DMA
clients cannot lose their fabric bandwidth to the idle governor. Requests
change only the desired low nibble and wait for the actual field to
confirm the requested state. A timeout restores the previous confirmed vote.
If restoration also times out, the fabric hold remains asserted and subsequent requests and system sleep fail with
``-EIO`` until reboot. Errors propagate to consumers, which must not start work requiring a
bandwidth request that failed. Firmware ownership and system sleep reject
requests with ``-EBUSY``. Suspend keeps the existing T600x floor; resume
restores the last successful aggregate request.

Enabling PMP and the host interconnect provider together is rejected. This
interface neither starts PMP nor reclaims a controller from running firmware.
No module parameter supplies bandwidth data or changes ownership.

Qualification before adding a board table
----------------------------------------

No board table is supplied here. A DRAM transfer-rate estimate or a PMP
policy threshold does not establish sustainable client bandwidth. Before
adding one, establish the raw desired/actual state mapping for the exact SoC
and firmware, verify each selected state's fabric dependencies, and measure
sustained read, write and copy bandwidth under controlled CPU and DMA load.
Use a conservative capacity supported by repeated measurements, with the
workload, sample duration, thermal conditions and variance recorded in the
qualification report. Do not publish private target records in source.

Qualify concurrent consumers, request release, failed raises and rollback,
suspend/resume and idle/load power through the hardware controller. Keep a
bootguard fallback and validate the default policy before enabling any board
DT. The KUnit suite uses synthetic numbers and memory-backed registers; it
validates software behavior only, not state encodings, rates, firmware
ownership or hardware completion.
