.. SPDX-License-Identifier: GPL-2.0

=====================================
Apple J813 initial native Linux boot
=====================================

J813 is the 13-inch M5 MacBook Air (T8142, Mac17,3). This configuration
provides a RAM-only boot path with AICv3, architectural timers, DockChannel
console and the framebuffer left by the boot firmware. The J813 tree also
describes SMC temperature monitoring, MTP input and the keyboard backlight.

The device tree describes all ten CPU affinities. For the initial one-CPU
baseline, keep secondary cores stopped in m1n1 and use ``maxcpus=1``.
The loader removes unavailable CPUs and fills the release addresses of the
remaining nodes. The P-core
affinities are 0x10100 through 0x10103, rather than the 0x100 through 0x103
values in the Apple Device Tree.

The CPU compatible strings identify T8142 explicitly. The firmware's reused
``sawtooth`` and ``everest`` names do not identify the M5 microarchitecture.

SMP bring-up
============

M5 secondary CPUs can return from WFI with cleared general-purpose registers.
Both M5 MIDRs (parts 0x062 and 0x063) select the Apple register-preserving idle
path. Without it, all ten CPUs can start and then panic in their idle tasks.

SMP also requires the M5-capable m1n1 secondary-start path: dense PMGR CPU
masks, correct reset vectors and per-core stacks. Verify all ten cores there
before preparing the FDT, and use ``maxcpus=10`` for the Linux test. The
existing J813 DT and configuration support this handoff.

On J813, this path reached the RAM shell with CPUs 0-9 online. A five-second
sleep advanced every CPU's architectural timer count by 1,251 interrupts.
Ten concurrent, individually pinned jobs each hashed 16 MiB of zeros to
``080acf35a507ac9849cfcba47dc2ad83e01b75663a516279c8b9d243b719643e``.
Verify affinity through ``Cpus_allowed_list`` and retain the console output,
kernel configuration hash and payload hashes when repeating the test.
Hotplug, deep sleep and KVM remain unqualified.

CPU frequency bring-up
======================

The T8142 cluster controllers use five-bit P-state requests with busy and
command-state readback. The driver preserves firmware voltage, PLL and
CLPC/PMP configuration, and stops issuing requests after a timeout or an
unexpected command state. Unlike T8140, this initial J813 path does not
register a Linux SMC thermal policy. The DT exposes only states up to the
measured firmware handoff values; higher states remain unqualified.

State 1 is 300 MHz on both clusters. The firmware SRAM voltage tables begin
at state 2: the E cluster exposes 300/972 MHz, and the P cluster exposes
300/1308/1608/1932/2220/2508/2820/3072/3300/3516 MHz. A dependent decrement
loop timed against CNTPCT_EL0 in m1n1 confirmed this mapping on J813.
``scaling_cur_freq`` reports the requested state, not an independent clock
measurement or a guarantee against firmware throttling.

The configuration enables the userspace governor. On a fresh ten-core boot,
capture a sweep of both policies to the host serial log::

  set -e
  for p in /sys/devices/system/cpu/cpufreq/policy*; do
      cat "$p/affected_cpus"
      echo userspace > "$p/scaling_governor"
      for round in 1 2; do
          for f in $(cat "$p/scaling_available_frequencies"); do
              echo "$f" > "$p/scaling_setspeed"
              test "$(cat "$p/scaling_cur_freq")" = "$f"
              echo "$p $f"
              sleep 0.02
          done
      done
      cat "$p/cpuinfo_max_freq" > "$p/scaling_setspeed"
      cat "$p/stats/total_trans"
  done
  dmesg

Retain the transcript and payload hashes. Qualification also compared three
pairs of pinned, checksum-verified integer workloads at each cluster's
minimum and maximum states and checked every CPU's timer and pinned SHA-256
job afterwards. Short frequency sweeps do not qualify sustained thermal
behavior, automatic governors, suspend or states above the handoff limit.

T8142 can also lose registers on WFIT in delay loops. Frequency transition
polling exposed this as a cleared pointer in the cpufreq driver. T8142 uses
the existing counter delay loop, avoiding low-power waits with live kernel
register state.

Bootloader requirements
=======================

Use the J813-capable Aurora m1n1 and U-Boot bring-up stack. A stock M1/M2
loader is not sufficient. Before entering Linux, the loader must:

* Supply usable memory and reserved regions. T8142 DRAM starts at
  0x10000000000; the DTS memory size is only a loader placeholder.
* Fill CPU release addresses and remove CPUs which have not been started.
* Fill the firmware framebuffer's address, size, geometry and format before
  enabling it.
* Prepare DMA protection only for devices enabled in the supplied FDT. The
  ``mtp`` alias enables the MTP DAPF handoff. Leave the protection state of
  unrelated AOP, PMP and ISP firmware intact.
* Leave the console, interrupt controller and display powered. This minimal
  DT does not describe their power domains.
* Give U-Boot a T8142 memory map covering its MMIO and high DRAM addresses.

The AIC driver selects the T8142-specific guest timer masking path from
``apple,t8142-aic3``. Writes to the legacy VM_TMR_FIQ_ENA_EL2 register trap
on this SoC. The driver uses the architectural EL02 timer control masks
instead. Older AIC variants retain their existing path.

Build
=====

On an arm64 Linux build host with the usual kernel build dependencies::

  out="$PWD/../j813-build"
  make ARCH=arm64 O="$out" \
      KCONFIG_ALLCONFIG=arch/arm64/configs/j813.config allnoconfig
  make ARCH=arm64 O="$out" -j"$(nproc)" W=1 Image apple/t8142-j813.dtb

The fragment does not enable KVM or storage drivers. Add
``CROSS_COMPILE=aarch64-linux-gnu-`` to both commands when
cross-compiling with GCC from another architecture.

With dtschema installed, check the affected bindings and DTB::

  make ARCH=arm64 O="$out" CHECK_DTBS=y \
      DT_SCHEMA_FILES=arm/apple.yaml:arm/cpus.yaml:cpufreq/apple,cluster-cpufreq.yaml:opp/opp-v2-base.yaml:interrupt-controller/apple,aic2.yaml:serial/apple,dockchannel-uart.yaml:serial/samsung_uart.yaml:display/simple-framebuffer.yaml:apple,pmgr.yaml:apple,smc-hwmon.yaml:apple,dockchannel-hid.yaml:apple,dart.yaml:apple,mailbox.yaml:apple,smc.yaml:apple,pmgr-pwrstate.yaml:apple,s5l-fpwm.yaml:apple,dockchannel.yaml:apple,rtk-helper-asc4.yaml \
      apple/t8142-j813.dtb

Boot and qualification
======================

Use a RAM-only initramfs with a serial shell and BusyBox (including
``mount``, ``sleep``, ``cat``, ``uname`` and ``sha256sum``). Its init must
mount proc, sysfs and devtmpfs, keep PID 1 alive and make no disk mounts.
Record the source commit, any uncommitted patch, final kernel config and
SHA-256 hashes of Image, DTB, initramfs, m1n1 and U-Boot with the console
capture. Verify uploaded payload bytes before handing over to U-Boot.

The initial command line is::

  earlycon=dockchannel,0x38812c000 console=tty0 console=ttyDC0 keep_bootcon maxcpus=1 rdinit=/init panic=0

Use m1n1 to load Image, the initramfs, the DTB and U-Boot into RAM and
prepare the FDT. Pass that prepared FDT through U-Boot's ``booti`` command.
Do not install this experimental payload into the resident boot image.

At the shell, capture the following into the host's serial log::

  echo J813_BOOT_PROOF_BEGIN
  uname -a
  tr '\000' '\n' < /proc/device-tree/compatible
  cat /sys/devices/system/cpu/online
  cat /proc/uptime
  cat /proc/interrupts
  sleep 2
  cat /proc/uptime
  cat /proc/interrupts
  head -5 /proc/meminfo
  cat /proc/mounts
  cat /proc/fb
  dmesg
  echo J813_BOOT_PROOF_END

Verify T8142/J813 identity, one online CPU, advancing uptime and timer
interrupt counts, an interactive shell, a registered framebuffer and only
RAM-backed filesystems. Repeat after an idle interval and after a fresh
RAM boot. Preserve the complete console transcript and a hash manifest;
an Image build or successful loader exit alone is not a boot result.
Review captures for device identifiers and boot entropy before publishing.

These boot tests do not establish CPU hotplug, KVM guest timers,
sustained thermal behavior, suspend, GPU acceleration, USB,
storage, networking or audio support. Input and monitoring require their
separate checks below. The disabled Samsung UART node
records its measured resources; the qualified console path is DockChannel.

Temperature monitoring and built-in input
========================================

Temperature monitoring enumerates the running SMC's readable, non-function
``T`` keys with four-byte floating-point values. It exposes their identifiers
through ``/sys/class/hwmon/*/temp*_label`` and millidegrees Celsius through
``temp*_input``. It does not embed a sensor inventory, calibration, inferred
sensor locations, trip temperatures or a new CPU cooling policy. Channel
numbers can change with firmware; identify channels by label. These keys can
include virtual or inactive channels; their number is not a count of physical
sensors, and a readable value does not establish a sensor's location. Read failures,
NaNs and infinities remain errors, rather than fabricated temperatures.
The existing J700 thermal provider and bounded J813 frequency states are
unchanged. Temperature reporting alone does not qualify sustained load.

MTP provides keyboard and trackpad HID descriptors and identifiers at runtime.
The ``keyboard`` alias lets m1n1 fill ``hid-country-code`` and
``apple,keyboard-layout-id`` from this machine's own boot environment. The
source tree does not assume a US keyboard or contain another machine's layout.
The J813 trackpad uses power method 2 with firmware-owned reset; the host
does not operate an AFE GPIO. Other machines retain their existing sequence.

The trackpad requires an externally supplied
``/lib/firmware/apple/tpmtfw-j813.bin``. The existing DockChannel firmware
container has a 20-byte little-endian header: magic ``0x46444948`` (``HIDF``),
version 1, header length, data length and interface-byte offset. See
``struct fw_header`` and ``dchid_get_firmware()`` for validation. An interface
offset of zero means no patching. The payload must
be prepared from firmware and any required calibration belonging to the
user's own machine. A filename is a lookup contract, not a firmware payload.
No Apple firmware, captured command buffers or calibration bytes are bundled.
Future IPSW/macOS extraction tools can provide this file without changing the
driver. A missing or invalid file fails trackpad startup; it does not prevent
the keyboard or temperature monitor from registering.

The loader must preserve the iBoot-loaded MTP image and SRAM, prepare its DMA
protection, and leave MTP ready for Linux's RTKit startup. A prior-stage MTP
client must complete a compatible shutdown before Linux starts; a partially
initialized or crashed coprocessor is not a supported handoff. On J813, U-Boot
must not start and stop MTP before Linux: Linux's later restart crashes the
firmware and takes the keyboard and trackpad down.

The backlight uses the existing PWM and LED drivers. Its boot brightness is
retained when the initial PWM period is valid, otherwise it starts off.
Userspace controls ``/sys/class/leds/kbd_backlight/brightness``
on a linear 0--255 scale. No captured brightness curve or factory calibration
is included.

Physical verification
---------------------

Place the user's touch firmware in a private RAM initramfs, then run::

  sh tools/testing/selftests/drivers/apple/j813-peripherals.sh 30

The check reads temperatures twice, exercises and restores backlight
brightness, and counts keyboard and trackpad events during a bounded physical
interaction window. It records no key codes or pointer coordinates. Press
and release keys and move/click the built-in trackpad during that window.
Confirm visible brightness changes separately; sysfs readback is not optical
proof. Preserve stdout, the source revision and SHA-256 hashes of the exact
Image, DTB, configuration, initramfs and bootloaders as the repeatable result.
Keep firmware and raw machine data private. Without physical events the input
check fails rather than crediting device registration as working input.

Native display
==============

The internal panel is driven by the display coprocessor (DCP) through the
Apple DRM driver. iBoot leaves DCP running with H17-generation (macOS 26)
firmware mapped through locked DARTs. Linux attaches to that live session:
it sends the standard RTKit wake handshake and never stops, resets or reloads
the coprocessor. No firmware file is loaded by Linux.

The loader must:

* reserve every live DCP firmware segment and the boot framebuffer, and
  export their IOVAs to the ``dcp``, ``disp0`` and ``disp0_piodma`` nodes;
* enable the display DART (the first ``iommus`` entry of ``disp0``) and the
  ``dcp`` node only after doing so.

The display DARTs keep the firmware's root page-table slots. Linux publishes
its own translations only into vacant slots. On ``apple,t8142-dart``, a
firmware slot is accepted when it already translates every page Linux mapped
there to the same physical address, and later Linux mappings into it must
match the firmware's translation. Firmware buffers DCP later asks Linux to
share are resolved through those firmware tables.

The DCP firmware reports the panel dimensions at startup; the driver does not
embed a panel timing or geometry. The J813 panel reports 2560x1664; the driver
hides its 64-pixel notch area by default, so the mode is 2560x1600 at 60 Hz.
The primary plane scans out IOMFB surface 2. Backlight control and a cursor
plane are not provided yet.

Boot the RAM payload as above with ``CONFIG_DRM_APPLE``. On J813 this reached
``DCP booted``, registered ``appledrmfb`` as the console framebuffer, and ran
a KDE Plasma Wayland session with the built-in keyboard and trackpad. The
desktop renders in software; GPU acceleration is separate work.
