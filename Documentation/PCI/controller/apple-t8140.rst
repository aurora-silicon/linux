.. SPDX-License-Identifier: GPL-2.0-only

T8140/J700 experimental radio PCIe bootstrap
============================================

The J700 path trains port 0, validates a bounded PIODMA bootstrap request,
and enables native ECAM for the MediaTek Wi-Fi and Bluetooth functions.
The root bridge forwards their BARs and DART translates endpoint DMA.

This is a bring-up implementation, not general T8140 PCIe support.
Build ``CONFIG_PCIE_APPLE_PIODMA_DIAG=y``. The board device tree selects the
bootstrap by pointing the host's ``apple,piodma`` property at the engine, and
the kernel then runs it during the host probe. A host without that property,
or a kernel built without the option, uses the root-only path, which rejects
downstream configuration access.
Build the host and its DART in as well: the supplier retains published DMA
and has no removal contract. Keeping the supplier and host built in avoids
a module dependency on a provider whose lifetime cannot end at unload.

Suspend-to-idle suspend retains the arena after checking the completed request,
idle FIFO and command state. Resume restores the existing configuration
banks without issuing a second bootstrap. Hibernation, deeper suspend and
kexec are refused while admission or retained DMA ownership is active. The
runtime kexec guard permits the kernel to support kexec on other machines.
Host removal, sysfs reset, hotplug, VFIO assignment and native AER/DPC
recovery are withheld on this retained hierarchy. Sleep is refused until
radio admission completes; the existing suspend-to-idle path remains available
after successful admission. Failed or uncertain bootstrap state
requires a full external hardware reset; there is no command retry.

The PIODMA arena remains allocated until external reset. Controller removal,
teardown, memory reuse and arbitrary downstream devices are unqualified.
Fresh hardware qualification must check both radio functions across sleep.
Do not remove the controller or its IOMMU while this experiment is active.
These lifecycle contracts require further work before production support.

J700's ``wifi0`` alias and PCI endpoint node allow the public bootloader to
pass the unit's own Wi-Fi address through ``local-mac-address``. The zero
placeholder in the static device tree is not a usable station address.
