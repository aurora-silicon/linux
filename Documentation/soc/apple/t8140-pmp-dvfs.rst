.. SPDX-License-Identifier: GPL-2.0-only

T8140 PMP DVFS diagnostics
=========================

The existing PMP report provider exposes a read-only ``dvfs`` file under
``/sys/kernel/debug/apple-pmp-<device>/`` on T8140. The provider must be
bound; the file is readable by root. Its output lists ADT report entries
8 through 11 and both raw 64-bit words of each 16-byte entry. Only those four entries
are read. The report aperture is not dumped.

Reading returns ``EAGAIN`` if the PMP running bit is clear before or after
sampling. Entries are sampled individually, rather than as an atomic
firmware snapshot. A running PMP alone does not establish freshness of
each value.

The field encoding and conversion to clock rates are not yet established.
Values must not be interpreted as Hz, temperatures, or verified hardware
clock states. This interface changes neither firmware requests nor cache
allocation nor thermal policy.

CPU temperatures remain available through ``macsmc-hwmon``. CPU cooling
and firmware frequency limits remain under their existing drivers. The
DVFS diagnostic file is not a thermal sensor.
