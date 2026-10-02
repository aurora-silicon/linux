.. SPDX-License-Identifier: GPL-2.0-only

T8140/J700 experimental radio PCIe bootstrap
==========================================

The J700 path trains port 0, validates a bounded PIODMA bootstrap request,
and enables native ECAM for the MediaTek Wi-Fi and Bluetooth functions.
The root bridge forwards their BARs and DART translates endpoint DMA.

This is an opt-in bring-up implementation, not general T8140 PCIe support.
Build ``CONFIG_PCIE_APPLE_PIODMA_DIAG=y`` and pass
``pcie_apple_piodma_diag.enumerate=1`` to the kernel. Without the opt-in, the
root-only diagnostic path continues to reject downstream configuration access.
The option excludes suspend and kexec. Failed or uncertain bootstrap state
requires a full external hardware reset; there is no command retry.

The PIODMA arena remains allocated until external reset. Controller removal,
teardown, memory reuse and arbitrary downstream devices are unqualified.
Do not remove the controller or its IOMMU while this experiment is active.
These lifecycle contracts require further work before production support.

J700's ``wifi0`` alias and PCI endpoint node allow the public bootloader to
pass the unit's own Wi-Fi address through ``local-mac-address``. The zero
placeholder in the static device tree is not a usable station address.
