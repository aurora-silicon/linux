.. SPDX-License-Identifier: GPL-2.0-only

M5 Pro non-GPU platform series
=============================

The APIF platform series separates transport/provider support from device
consumers. The PRs are based on apif-mmio or on their immediate dependency.
This is a review map for the non-GPU functionality represented in the series,
not a claim that the cleaned stack has been hardware-qualified.

===============================  =======
Area                             Review
===============================  =======
APIF transport, DART and SART     PR238_
Native ANS/NVMe queues            PR241_
Preinitialized PCIe and MSI       PR242_
PMP firmware registry            PR243_
Guest DT conversion              PR244_
Generation-4 SPMI                PR245_
SPMI PMU RTC                     PR246_
SMC consumers and pcIO GPIO       PR247_
DP PHY and crossbar              PR248_
CPU frequency profile            PR249_
N1 shared firmware/PCI lifetime   PR250_
N1 Wi-Fi                         PR251_
N1 Bluetooth                     PR252_
Firmware-27 AFK transport         PR253_
DCP display and DPTX              PR254_
Host GPIO interrupt groups       PR255_
Initial SPMI Type-C events        PR256_
===============================  =======

.. _PR238: https://github.com/aurora-silicon/linux/pull/238
.. _PR241: https://github.com/aurora-silicon/linux/pull/241
.. _PR242: https://github.com/aurora-silicon/linux/pull/242
.. _PR243: https://github.com/aurora-silicon/linux/pull/243
.. _PR244: https://github.com/aurora-silicon/linux/pull/244
.. _PR245: https://github.com/aurora-silicon/linux/pull/245
.. _PR246: https://github.com/aurora-silicon/linux/pull/246
.. _PR247: https://github.com/aurora-silicon/linux/pull/247
.. _PR248: https://github.com/aurora-silicon/linux/pull/248
.. _PR249: https://github.com/aurora-silicon/linux/pull/249
.. _PR250: https://github.com/aurora-silicon/linux/pull/250
.. _PR251: https://github.com/aurora-silicon/linux/pull/251
.. _PR252: https://github.com/aurora-silicon/linux/pull/252
.. _PR253: https://github.com/aurora-silicon/linux/pull/253
.. _PR254: https://github.com/aurora-silicon/linux/pull/254
.. _PR255: https://github.com/aurora-silicon/linux/pull/255
.. _PR256: https://github.com/aurora-silicon/linux/pull/256

CPU identification, idle register preservation and delay handling are
already represented by the base. Existing public interrupt-controller,
USB, I2C/SPI, DockChannel HID, MTP helper and standard platform drivers are
preserved rather than replaced with older copies. The fragment enables
their dependencies alongside the new providers and consumers.

AGX acceleration, its dedicated mailbox transports and diagnostic system
register probes are outside this series. Firmware binaries, board
calibration data and factory addresses are not included.

Configuration
-------------

Apply aurora-apif.config after defconfig and asahi.config, then run
olddefconfig. The fragment makes the selected platform consumers built-in;
firmware and loader metadata are still required.

The existing HID interface uses explicit apple,power-method and
apple,firmware-managed-reset properties. The latter is valid only for a
power-method-2 multi-touch interface with firmware-owned reset. The loader
must describe the actual ownership; no board-name or firmware-filename
guess selects it.

Loader and qualification requirements
-------------------------------------

* NVMe needs a stopped RTKit session and four distinct, reserved queue
  pages. The new profile does not adopt an unqualified live queue session.
* N1 cold bootstrap needs its packed platform identifier, protocol 27,
  factory Bluetooth address and an exclusively owned PCIe/DART unit.
  Unqualified live-session adoption is not enabled.
* CPU frequency control requires an explicit, qualified firmware thermal
  ownership claim covering every exposed OPP, including after PMP setup.
  The DT conversion tool does not invent that claim.
* DCP needs a running firmware-27 session, inherited framebuffer region,
  ordered provider clock table and the coprocessor physical register view
  when guest resource addresses differ.
* GPIO interrupt groups and HID reset ownership must match the loader's
  retained firmware ownership.

The conversion tool validates supplied metadata; it cannot establish that
firmware has stopped using a queue, stream, reset line or clock policy.

Validation
----------

The series has native driver builds and host protocol/bounds tests.
Hardware qualification must separately cover boot, storage I/O, network
traffic, physical Type-C attach/detach, display, brightness and suspend.
Retained firmware/DMA state is not reset as a fallback after a timeout.
