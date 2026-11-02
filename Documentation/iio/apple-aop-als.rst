.. SPDX-License-Identifier: GPL-2.0-only OR MIT

Apple AOP ambient light reporting
================================

The ALS driver consumes the payload of EPIC fake-HID reports, after the AOP
transport has removed the EPIC headers. These offsets do not describe macOS
host-side event objects.

Redbird (VD6286) illuminance is the little-endian firmware float at payload
byte 0x28. Linux converts it to microlux using integer arithmetic and exports
it through ``in_illuminance_input``. The driver requests a 200000 microsecond
report interval, or 5 Hz, and loads the device's factory calibration from
``apple/aop-als-cal.bin`` when installed. Calibration is device-specific and
must not be distributed with test fixtures.

The `public Redbird report description
<https://github.com/triutama133/ubuntu_asahi_patch/blob/3cb6569aaa56815e86b487111725a4d05406bd00/kernel-patches/README.md>`_
identifies a 122-byte report carrying a firmware-computed CCT float at byte
0x45. The driver exports that layout as ``in_cct_input``, in Kelvin, without
recalculating colour temperature from raw channels. Other lengths do not
supply a CCT reading. This layout must be verified on each firmware family
before its CCT readings are used for display policy. The existing CT720 and
CT817 illuminance layouts have no established CCT field in this driver.

Read both channels by finding the IIO device by name rather than assuming a
device number::

    for d in /sys/bus/iio/devices/iio:device*; do
        [ "$(cat "$d/name" 2>/dev/null)" = aop-sensors-als ] || continue
        cat "$d/in_illuminance_input"
        [ ! -e "$d/in_cct_input" ] || cat "$d/in_cct_input"
    done

These attributes contain the last valid reading, not a sample counter. Polling
them five times per second cannot prove a 5 Hz arrival cadence. Measure that
at the report callback or transport with tracing, and verify the payload
length and field correspondence there. Keep raw captures private. A CCT value
remaining unchanged while lux changes can also mean an unsupported payload
layout or a report with invalid CCT; it does not prove steady illumination.
