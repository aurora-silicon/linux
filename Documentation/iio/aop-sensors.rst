.. SPDX-License-Identifier: GPL-2.0-only OR MIT

=================
Apple AOP sensors
=================

The AOP firmware announces sensor services over EPIC. The AOP core matches
each service to a child node in the device tree. Most sensors send fake-HID
reports over EPIC; fused device motion (``devmotion6``) instead writes a
shared fast-path buffer opened by its IIO driver. No HID bus device is
created.

Lid angle
=========

Enable ``CONFIG_APPLE_AOP`` and ``CONFIG_IIO_AOP_SENSOR_LAS``. The AOP must
have a ``las`` child with a SoC-specific compatible followed by
``apple,aop-las``. The T6040 device tree already supplies this child for
J616s, and ``asahi.config`` enables the driver. The bootloader must hand
over the AOP firmware memory regions before the AOP can start.

The device is named ``aop-sensors-las``. Its ``in_angl_raw`` attribute is
the latest valid report-1 angle, in whole degree counts from 0 to 360.
Multiply it by ``in_angl_scale`` (0.017453293) to obtain radians as
specified by the IIO angle ABI. The scale is fixed and can be read before
the first sample; a raw read before the first valid sample returns
``ENODATA``. Other report IDs, truncated reports and out-of-range angles
leave the last valid reading unchanged.

Report 1 carries a 9-bit little-endian value following the report ID.
The remaining bits in the last byte are padding. Reads return the cached
unsolicited report, rather than issuing a synchronous HID feature request.

Accelerometer and gyroscope
===========================

Enable ``CONFIG_IIO_AOP_SENSOR_MOTION``. The AOP must have ``accel`` and
``gyro`` children with a SoC-specific compatible followed by
``apple,aop-accel`` or ``apple,aop-gyro``. Each service reports only after
it has been sent the machine's three calibration packets, which the
bootloader copies from the Apple device tree into the child's
``apple,range-intrinsic-cal``, ``apple,nominal-extrinsic-cal`` and
``apple,range-extrinsic-cal`` properties. Without them the driver leaves
the sensor off and does not bind. The packets are sent unchanged, then the
50 Hz report interval.

The devices are named ``aop-sensors-accel`` and ``aop-sensors-gyro``. Each
has ``in_accel_{x,y,z}_raw`` or ``in_anglvel_{x,y,z}_raw``, a shared scale
(acceleration in m/s^2 including gravity, angular rate in rad/s) and
``sampling_frequency``. The axes are the sensor's, in the order it reports
them; the mapping to the machine's axes is not established yet. A raw read
before the first sample returns ``ENODATA``.

Both devices have a buffer that takes X, Y and Z together, with an optional
timestamp taken when the report arrives. The reports stop when the system
goes to sleep and start again, with the calibration resent, when it wakes.

If both devices bind but every raw read returns ``ENODATA``, check the AOP
core's ``nCal`` boot argument for the SoC. The T6040 firmware accepts the
calibration and the report interval with ``nCal`` 0, but sends no reports;
its own default, which the core keeps, is 1.

Device-motion fusion
====================

Enable ``CONFIG_IIO_AOP_SENSOR_DEVMOTION``. The device is named
``aop-sensors-devmotion6`` and exposes one ``IIO_ROT`` channel modified as a
quaternion. Direct reads return four signed values in W, X, Y, Z order; their
fixed scale is ``1 / 2^30``. The buffered channel uses the same four-value
Q30 representation followed by an optional host timestamp. At rest the
quaternion norm should be close to one, and tilting the laptop should move at
least two components smoothly while the norm remains close to one.

The service's current report has a type byte followed by a versioned fixed
layout. Type 2 is 108 bytes and carries little-endian IEEE-754 X, Y, Z and W
at offset 28. Types 3 and 4 carry the same fields at offset 12; their length
depends on the firmware (the M4 Pro sends 52-byte type 3 reports). Type 1 is a five-byte state report and does not update the IIO sample.
The driver writes value 1 to property 61 first. Some firmware answers status
0xe00002c7 (unsupported) for this service, which is harmless. The stream is
armed by the consumer registration: the driver reads property 81 for the
report size, allocates a coherent two-slot ping-pong buffer, and opens it
with EPIC command 25. Only then does the driver write the non-zero interval in microseconds to
property 0. Reports are polled from the published shared-memory slot rather
than received as fake-HID subtype 0xc4 messages.

The firmware records the property 0 interval but refreshes the shared buffer
at the fusion engine's own rate (every 5 ms on the M4 Pro) whatever value it
holds, and keeps refreshing it until the client is closed. The driver
therefore paces itself: ``sampling_frequency`` (default 50 Hz, 1 to 200 Hz)
sets how often it polls the buffer, and the client is open only while samples
are consumed. Enabling the buffer opens the client and disabling it closes it
with command 26 after writing a zero interval. A direct read without an
enabled buffer opens the client, waits up to 500 ms for the first sample and
closes it again. After the AOP starts the fusion engine, the service publishes
the exact identity quaternion until the filter has its first estimate (about
140 ms on the M4 Pro); the driver does not report those placeholders. Around system sleep an open client is closed and reopened
after resume. A failed or timed-out close retains the DMA mapping because the
firmware may still hold its address. This fused stream needs no separate
calibration blob.

Board temperature
=================

Enable ``CONFIG_IIO_AOP_SENSOR_TEMP``. The device is named
``aop-sensors-als-temp``. ``in_temp_input`` and buffered samples are in
millidegrees Celsius, following the IIO temperature ABI, and
``sampling_frequency`` is one hertz. The 14-byte report stores signed Q16.16
degrees Celsius at offset 2; the driver timestamps it on receipt.

The bootloader may provide ``apple,temp-offset-calibration``,
``apple,absolute-temp-offset-cal``, ``apple,absolute-temp-offset-fcm-cal`` and
``apple,absolute-temp-offset-bcm-cal`` containers. Location-zero packets are
sent unchanged before the one-hertz interval. If none are present the driver
warns and selects the firmware's default calibration state; otherwise it
selects the calibrated state. A missing optional calibration therefore does
not hide an otherwise usable sensor.


Host regression tests require Python 3 and a Rust compiler::

  tools/testing/selftests/drivers/apple/aop-las.py
  tools/testing/selftests/drivers/apple/aop-motion.py

The tests compile the production decoder and shared IIO read callback
against host mocks. They cover all 16-bit payloads (including padding),
report IDs, short reports, invalid readings, the scale before a sample,
last-valid-sample behaviour and processed channels without a scale.
They do not test EPIC transport or firmware behaviour.

On J616s, or another supported laptop:

1. Build with ``asahi.config`` and the board's DTB. Check that the AOP,
   LAS and ALS options survived configuration, and that the bootloader
   supplies the AOP memory regions.
2. Boot and locate the IIO device whose ``name`` is ``aop-sensors-las``.
   Check ``in_angl_scale`` is 0.017453293. Read ``in_angl_raw`` repeatedly
   while moving the lid through several safe positions. Expect whole
   degree counts that follow the hinge position. Do not force the hinge
   beyond its mechanical limit to test values above 255; the host tests
   cover that encoding.
3. Confirm the value updates after boot and after resume, and that ALS
   still reports illuminance. Check the kernel log for AOP timeouts,
   DART faults or warnings. Repeat on an older supported AOP platform
   to check the shared receive path.
4. If raw reads keep returning ``ENODATA``, check that firmware announces
   ``las`` and sends EPIC report subtype 0xc4 on that service's channel.
   A readable feature report alone does not prove unsolicited reports
   arrive. Establish that transport behaviour before adding any polling
   or firmware setup request.

To test the two additional J616s services without rebooting, replace the
module names below with their installed paths if the distribution does not
put them in the normal module tree::

  modprobe -r aop_devmotion aop_temp
  modprobe aop_devmotion
  modprobe aop_temp

Find the devices by name rather than assuming IIO numbers::

  for d in /sys/bus/iio/devices/iio:device*; do
          printf '%s: ' "$d"; cat "$d/name"
  done
  iio_info

For ``aop-sensors-devmotion6``, enable ``in_rot_quaternion_en`` and the
timestamp if desired, then run ``iio_readdev``. Four signed Q30 values make
one quaternion sample. Flat and stationary, the values should be stable and
their scaled squared norm close to one. Tilt the left, right and display
edges in turn; components should change continuously rather than jump or
remain zero. For ``aop-sensors-als-temp``, ``cat in_temp_input`` should show a
plausible board temperature in millidegrees and ``iio_readdev`` should yield
one signed 32-bit temperature plus the optional timestamp each second.

After testing, suspend and resume once and verify both streams restart. A
missing device means the AOP did not announce the service or its child did
not bind; ``ENODATA`` means it announced and bound but has not delivered a
valid sample. Record ``dmesg`` around either case, including property refusal,
report-size, AOP timeout and DART messages.
