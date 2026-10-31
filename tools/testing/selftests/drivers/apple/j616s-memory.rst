.. SPDX-License-Identifier: GPL-2.0-only OR MIT

J616s memory policy checks
=========================

The T6040 uses the ``apple,t6041-pmc`` and ``apple,t6041-clpc`` compatibles.
The J616s DT enables both the PMC and the PMP. ``apple,pmp`` gates PMC probe
on PMP readiness; static voters 0, 1 and 3 enable PMP, CLPC and AP requests on
the DCS and fabric rails. Do not enable the PMP voter before its owner is
running: its boot-time floor can be the maximum state.

For CPU demand, CLPC samples every 4 ms, qualifies three consecutive samples
with at least 50 percent busy time on a CPU in a cluster, and uses the cluster
frequency quartile to request DCS states 1, 2, 5, 5 and fabric states 0, 1, 2,
2. Sustained work in the upper half of a cluster's range requests DCS state 5
(the sixth state). On idle, the host floor is released after a 32 ms hold;
firmware and display clients may continue requesting memory states. The
host's zero floor does not imply that DRAM actually runs in state zero.

``/sys/kernel/debug/apple-pmc/*/status`` reports requested and honoured agent
floors and rail enable masks. The CLPC device's debugfs ``status`` reports its
CPU requests, current vote and error count. These are policy inputs, not DCS
achieved-state residency. In particular, neither an honoured floor nor the
``pmc-dcs`` thermal cooling state is a current DRAM clock.

The PMP report provider's ``dram/`` sysfs group contains static metadata::

    find /sys/devices/platform -type d -name dram -print

On a J616s DT carrying the DRAM description, its attributes report type
``lpddr5``, 6 states, width 256, maximum data rate 8533333333 transfers/s,
physical peak 273066666656 bytes/s, and published class 273000000000 bytes/s.
The group is conditional on valid boot DT metadata; a missing group with an
older boot DT does not imply a disabled DCS governor. It contains no current
rate or residency counters.

For a reproducible application bandwidth workload, build on Linux::

    cc -O2 -Wall -Wextra -Werror -pthread \
        tools/testing/selftests/drivers/apple/memcpy-bw.c -o /tmp/memcpy-bw
    /tmp/memcpy-bw 8 256 10

Each of eight threads copies between its own two 256 MiB buffers for ten
seconds, after first-touch initialization. Output counts one source read and
one destination write per completed libc copy, in decimal GB/s, over the
wall time until the last thread finishes. It is an application estimate, not
an AMCC bus measurement: write allocation, cache effects and libc algorithm
can change the physical traffic. Record libc, CPU governor, display topology,
thermal state and the exact kernel/DT alongside the result. Compare idle,
bounded CPU load, memcpy, and recovery using the same observation method.
Do not infer lower-state rates from bandwidth response thresholds.

The capture helper checks the board model, records available policy and IIO
attributes, then runs two seconds idle, five seconds of fourteen CPU workers,
two seconds recovery, the ten-second memcpy workload, and two seconds recovery::

    python3 tools/testing/selftests/drivers/apple/j616s-parity.py \
        --memcpy /tmp/memcpy-bw --output /tmp/j616s-parity-NEW

The output directory must be new. Each read failure is recorded in JSON, and
the helper terminates only its own workload processes on failure or interrupt.
The memcpy phase needs 4 GiB plus overhead. Polling at 20 Hz can itself cause
CPU wakeups; use low-overhead counter snapshots for final idle-residency and
power comparisons. Cached IIO values do not establish report arrival cadence.
