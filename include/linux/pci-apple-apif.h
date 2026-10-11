/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LINUX_PCI_APPLE_APIF_H
#define _LINUX_PCI_APPLE_APIF_H
#include <linux/errno.h>
#include <linux/kconfig.h>
struct pci_dev;
#if IS_REACHABLE(CONFIG_PCIE_APPLE_APIF)
/* Preboot N1 transition only, before radio function enumeration or DMA. */
int apple_apif_pcie_cycle(struct pci_dev *pdev);
#else
static inline int apple_apif_pcie_cycle(struct pci_dev *pdev)
{
	return -EOPNOTSUPP;
}
#endif
#endif
