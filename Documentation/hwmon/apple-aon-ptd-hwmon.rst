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
state, current and charge information. The PTD record adds the battery
voltage and the system power as the producer filters them.

Sysfs entries
-------------

=============== ===== ==================================================
in0_input       mV    battery voltage
in0_label             ``Battery``
power1_input    uW    system power (never negative; this is not the
                      battery charge or discharge power)
power1_label          ``System``
=============== ===== ==================================================

The record holds both values as unsigned Q16.16 fixed point: the voltage in
millivolts, the power in watts. Its other words read as zero or as a
producer counter and are not reported.

Each read takes the two words twice and uses them only when both reads
agree; after eight disagreeing attempts the read fails with ``-EAGAIN``.
Reads within 10 ms of each other share one sample.
