/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LINUX_SOC_APPLE_PCI_PIODMA_H
#define _LINUX_SOC_APPLE_PCI_PIODMA_H

#include <linux/errno.h>
#include <linux/kconfig.h>

struct module;
struct pci_dev;

#if IS_ENABLED(CONFIG_PCIE_APPLE_T8140_BOOTSTRAP)
/* Require completed Neo admission and permanent retained-DMA protection. */
int apple_piodma_radio_check(struct pci_dev *pdev);
/* A function that a sleep left unusable asks for both to be restarted from cold. */
void apple_piodma_radio_recover(void);
/* What the host calls before it removes Wi-Fi for that. A failure keeps it bound. */
void __apple_piodma_radio_set_stop(int (*stop)(struct pci_dev *pdev), struct module *owner);
#define apple_piodma_radio_set_stop(stop) __apple_piodma_radio_set_stop(stop, THIS_MODULE)
#else
static inline int apple_piodma_radio_check(struct pci_dev *pdev)
{
	return -ENODEV;
}

static inline void apple_piodma_radio_recover(void)
{
}

static inline void apple_piodma_radio_set_stop(int (*stop)(struct pci_dev *pdev))
{
}
#endif

#endif /* _LINUX_SOC_APPLE_PCI_PIODMA_H */
