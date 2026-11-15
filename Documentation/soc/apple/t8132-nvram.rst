.. SPDX-License-Identifier: GPL-2.0-only OR MIT

J713 NVRAM and boot-volume selection
===================================

The J713 (MacBook Air M4) boot NOR is connected to the T8132 QSPIMC at
0x3ad118000. Its NVRAM partition occupies flash offsets 0x700000..0x7fffff.
The Apple QSPI driver exposes this partition through MTD for userspace tools
such as ``asahi-nvram`` and ``asahi-bless``. The latter selects an existing
Apple boot volume; it does not install or sign a new boot policy.

The controller starts read-only. Root can enable writes after probe through
``/sys/module/spi_apple_qspi/parameters/read_only``. Write admission requires
the J713 board, T8132 controller, matching aperture and NVRAM partition.
Programs and erases remain restricted to that partition, single-wire
three-byte addressing, 256-byte pages and aligned 4 KiB erase blocks. The
existing JEDEC identity and protection checks apply before every mutation.
The driver does not change flash protection registers. Each mutation is
verified by readback; a write fault latches the driver back to read-only.

For a one-time macOS boot while retaining the persistent Linux boot target::

    modprobe spi-apple-qspi
    asahi-bless --get-boot
    asahi-bless --get-boot --next
    asahi-bless --list-volumes
    printf N > /sys/module/spi_apple_qspi/parameters/read_only
    asahi-bless --set-boot-macos --next --yes
    printf Y > /sys/module/spi_apple_qspi/parameters/read_only
    asahi-bless --get-boot
    asahi-bless --get-boot --next

Verify that the persistent target is unchanged and the one-time target is
the intended macOS volume before rebooting. If an update fails, retain the
running system for inspection rather than retrying or rebooting with an
uncertain selection. Returning from macOS requires a restart or a selection
in Apple's startup picker; Linux cannot initiate that restart while macOS
is running.
