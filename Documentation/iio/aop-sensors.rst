.. SPDX-License-Identifier: GPL-2.0-only OR MIT

=================
Apple AOP sensors
=================

The AOP firmware announces sensor services over EPIC. The AOP core matches
each service to a child node in the device tree and forwards its fake-HID
reports to the corresponding IIO driver. Ambient light (``als``) and lid
angle (``las``) use this same transport; no HID bus device is created.

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

Verification
============

Host regression tests require Python 3 and a Rust compiler::

  tools/testing/selftests/drivers/apple/aop-las.py

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
