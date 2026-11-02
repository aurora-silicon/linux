/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LINUX_SOC_APPLE_PCI_PIODMA_H
#define _LINUX_SOC_APPLE_PCI_PIODMA_H

#include <linux/errno.h>
#include <linux/kconfig.h>

struct pci_dev;

#if IS_ENABLED(CONFIG_PCIE_APPLE_T8140_BOOTSTRAP)
/* Require completed Neo admission and permanent retained-DMA protection. */
int apple_piodma_radio_check(struct pci_dev *pdev);
#else
static inline int apple_piodma_radio_check(struct pci_dev *pdev)
{
	return -ENODEV;
}
#endif

#endif /* _LINUX_SOC_APPLE_PCI_PIODMA_H */
