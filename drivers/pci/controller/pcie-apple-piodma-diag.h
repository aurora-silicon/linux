/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef PCIE_APPLE_PIODMA_DIAG_H
#define PCIE_APPLE_PIODMA_DIAG_H

#include <linux/errno.h>
#include <linux/kconfig.h>
#include <linux/types.h>

struct device;
struct pci_dev;

#if IS_ENABLED(CONFIG_PCIE_APPLE_PIODMA_DIAG)
bool apple_piodma_bootstrap_enabled(void);
int apple_piodma_bootstrap_get(struct device *host, struct device **supplier);
int apple_piodma_bootstrap_prime(struct device *supplier, struct pci_dev *root);
#else
static inline bool apple_piodma_bootstrap_enabled(void)
{
	return false;
}

static inline int apple_piodma_bootstrap_get(struct device *host, struct device **supplier)
{
	return -ENODEV;
}

static inline int apple_piodma_bootstrap_prime(struct device *supplier, struct pci_dev *root)
{
	return -ENODEV;
}
#endif
#endif
