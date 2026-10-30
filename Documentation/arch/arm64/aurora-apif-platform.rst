.. SPDX-License-Identifier: GPL-2.0-only

M5 Pro non-GPU platform series
=============================

The APIF platform series separates transport/provider support from device
consumers. The shared ``t6050/base`` PR targets ``integration/aurora`` on
Linux 7.3; every other PR targets ``t6050/base``. Consumer dependencies are
recorded separately, so the review branches do not form a linear chain.
The source snapshot is ``t6050-gpu`` at ``ee68abb105200``. This review map
covers its non-GPU platform changes; hardware qualification of the rebased
combination remains separate.

.. list-table:: Review branches
   :header-rows: 1

   * - Area
     - Branch
   * - APIF transport, DART and SART
     - `t6050/base <https://github.com/aurora-silicon/linux/tree/t6050/base>`_
   * - Native ANS/NVMe queues
     - `t6050/nvme <https://github.com/aurora-silicon/linux/tree/t6050/nvme>`_
   * - Preinitialized PCIe and MSI
     - `t6050/pcie <https://github.com/aurora-silicon/linux/tree/t6050/pcie>`_
   * - PMP firmware registry
     - `t6050/pmp <https://github.com/aurora-silicon/linux/tree/t6050/pmp>`_
   * - Guest DT conversion
     - `t6050/guest-wiring <https://github.com/aurora-silicon/linux/tree/t6050/guest-wiring>`_
   * - Generation-4 SPMI
     - `t6050/spmi <https://github.com/aurora-silicon/linux/tree/t6050/spmi>`_
   * - SPMI PMU RTC
     - `t6050/pmu-rtc <https://github.com/aurora-silicon/linux/tree/t6050/pmu-rtc>`_
   * - SMC consumers and pcIO GPIO
     - `t6050/smc <https://github.com/aurora-silicon/linux/tree/t6050/smc>`_
   * - DP/USB combo PHY and crossbar
     - `t6050/display-phy <https://github.com/aurora-silicon/linux/tree/t6050/display-phy>`_
   * - CPU frequency profile
     - `t6050/cpufreq <https://github.com/aurora-silicon/linux/tree/t6050/cpufreq>`_
   * - N1 firmware and PCI lifetime
     - `t6050/centauri-core <https://github.com/aurora-silicon/linux/tree/t6050/centauri-core>`_
   * - N1 Wi-Fi
     - `t6050/centauri-wifi <https://github.com/aurora-silicon/linux/tree/t6050/centauri-wifi>`_
   * - N1 Bluetooth
     - `t6050/centauri-bluetooth <https://github.com/aurora-silicon/linux/tree/t6050/centauri-bluetooth>`_
   * - Firmware-27 AFK, DCP and DPTX
     - `t6050/dcp <https://github.com/aurora-silicon/linux/tree/t6050/dcp>`_
   * - Host GPIO interrupt groups
     - `t6050/gpio-irqs <https://github.com/aurora-silicon/linux/tree/t6050/gpio-irqs>`_
   * - Initial SPMI Type-C events
     - `t6050/typec-events <https://github.com/aurora-silicon/linux/tree/t6050/typec-events>`_
   * - DWC3 controller
     - `t6050/usb <https://github.com/aurora-silicon/linux/tree/t6050/usb>`_
   * - Power-domain handoff
     - `t6050/pmgr <https://github.com/aurora-silicon/linux/tree/t6050/pmgr>`_
   * - MTP keyboard reports
     - `t6050/mtp <https://github.com/aurora-silicon/linux/tree/t6050/mtp>`_
   * - Composed platform configuration
     - `t6050/platform-wiring <https://github.com/aurora-silicon/linux/tree/t6050/platform-wiring>`_

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
