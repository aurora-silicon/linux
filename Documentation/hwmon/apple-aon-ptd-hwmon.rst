.. SPDX-License-Identifier: GPL-2.0-only

Kernel driver apple-aon-ptd-hwmon
=================================

Supported hardware

    * Apple MacBook Pro 16-inch (M4 Pro, J616s, T6041)

Description
-----------

The always-on power telemetry (PTD) block of Apple SoCs holds records that
firmware keeps up to date. This driver reads the battery record and reports
its filtered samples. It maps only the block's peek window, whose reads do
not consume record updates, and reads only the battery words listed below.
It takes no interrupts, writes nothing to the block and sets no trips or
limits.

The record is located by the ``apple,battery-record`` property of the PTD
node (record id, layout version, first slot, slot count). Without it the
driver does not bind. Machines whose firmware has no such record, such as
the MacBook Neo (T8140), do not carry the property.

The SMC driver (macsmc-hwmon, macsmc-power) remains the source of battery
state and charge information. These channels add the 10 ms and 100 ms
filtered voltage, current and power that the PTD block carries.

Sysfs entries
-------------

=============== ===== ==================================================
in0_input       mV    battery voltage, 10 ms filter
in1_input       mV    battery voltage, 100 ms filter
curr1_input     mA    battery current, 10 ms filter (signed)
curr2_input     mA    battery current, 100 ms filter
power1_input    uW    battery power, 10 ms filter (signed)
power2_input    uW    battery power, 100 ms filter (signed)
\*_label              ``Battery 10 ms`` or ``Battery 100 ms``
=============== ===== ==================================================

Each read takes the six words twice and uses them only when both reads
agree; after eight disagreeing attempts the read fails with ``-EAGAIN``.
Reads within 10 ms of each other share one sample.
