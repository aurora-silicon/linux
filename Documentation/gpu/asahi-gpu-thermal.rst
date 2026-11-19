.. SPDX-License-Identifier: GPL-2.0-only OR MIT

=====================================
Apple GPU power ceiling and thermals
=====================================

The GPU firmware takes two absolute power inputs from the host: the maximum
GPU power in Region C and the power-management budget in the primary power
state. Both are written once at initialization. The driver turns them into
a thermal cooling device, so device-tree thermal zones can lower the GPU's
power the same way they lower CPU frequencies.

Device tree
===========

A GPU node with ``#cooling-cells = <2>`` is a cooling device. Cooling state
``N`` (0..100) requests ``100 - N`` percent of the initialized maximum power,
never less than 25 percent. State 0 republishes the initialized values, so a
GPU that no thermal zone references keeps the firmware's own policy. The
policy itself (sensors, trips, the states each trip asks for) belongs to the
board's thermal zones. The GPU is not a power actor; under the
``power_allocator`` governor its zones are ladders of ``active`` trips, which
the governor runs bang-bang, as it runs fans.

The firmware keeps its own die-temperature limiter, so the host ceiling is
an outer loop, not the overtemperature protection.

Firmware builds
===============

The driver writes the power inputs only where the running build's layout is
described (``Variant::live_power`` in ``g17/fw/variant.rs``): the A18 Pro
(26A428) and M4 Pro (25G83) builds. A GPU the device tree makes a cooling
device does not start on any other build, rather than run with the thermal
zones silently ignored. The M5 firmware takes its host budget at a different
Region C offset, which its own governor (``g17/thermal.rs``) writes; moving
that governor onto this cooling device is separate work.

The requesters (thermal, firmware fast-die, system, manual) compose by
minimum: one cannot relax another's ceiling. Recovery republishes the
committed ceiling before queues reopen.

Diagnostics
===========

``/sys/class/thermal/cooling_device*/`` with type ``gpu-power`` shows the
requested state. The GPU device's read-only ``gpu_power_ceiling`` attribute
and ``/sys/kernel/debug/asahi-<device>-power/ceiling`` report the requested
and published ceilings and the read-back words. A published ceiling is what
the host wrote; the firmware does not acknowledge it, so measured GPU power
and clocks are the evidence that it took effect.

Tests
=====

``drivers/gpu/drm/asahi/.kunitconfig.cooling`` runs the mapping, composition,
failure and recovery cases under KUnit::

    ./tools/testing/kunit/kunit.py run --arch=arm64 \
        --kunitconfig=drivers/gpu/drm/asahi/.kunitconfig.cooling

``tools/testing/selftests/drivers/apple/gpu-cooling.py`` runs the same cases
and the typed power-state layout checks on the build host.
