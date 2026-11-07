/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef PCIE_APPLE_T8140_BOOTSTRAP_H
#define PCIE_APPLE_T8140_BOOTSTRAP_H

#include <linux/errno.h>
#include <linux/kconfig.h>
#include <linux/types.h>

struct device;
struct pci_dev;

#if IS_ENABLED(CONFIG_PCIE_APPLE_T8140_BOOTSTRAP)
int apple_piodma_bootstrap_get(struct device *host, struct device **supplier);
int apple_piodma_bootstrap_guard_begin(struct device *supplier,
				       struct device *host);
void apple_piodma_bootstrap_guard_end(struct device *supplier);
int apple_piodma_bootstrap_prime(struct device *supplier, struct pci_dev *root);
void apple_piodma_bootstrap_admitted(struct device *supplier);
bool apple_piodma_bootstrap_retained(struct device *supplier);
void apple_piodma_bootstrap_set_sleep(struct device *supplier, bool armed);
int apple_piodma_bootstrap_power_lost(struct device *supplier);
int apple_piodma_bootstrap_radio_stop(struct device *supplier, struct pci_dev *pdev);
void apple_piodma_bootstrap_set_recover(struct device *supplier, void (*recover)(void *data),
					void *data);
#else
static inline int apple_piodma_bootstrap_get(struct device *host, struct device **supplier)
{
	return -ENODEV;
}

static inline int apple_piodma_bootstrap_guard_begin(struct device *supplier,
						     struct device *host)
{
	return -ENODEV;
}

static inline void apple_piodma_bootstrap_guard_end(struct device *supplier)
{
}

static inline int apple_piodma_bootstrap_prime(struct device *supplier, struct pci_dev *root)
{
	return -ENODEV;
}

static inline void apple_piodma_bootstrap_admitted(struct device *supplier)
{
}

static inline bool apple_piodma_bootstrap_retained(struct device *supplier)
{
	return false;
}

static inline void apple_piodma_bootstrap_set_sleep(struct device *supplier, bool armed)
{
}

static inline int apple_piodma_bootstrap_power_lost(struct device *supplier)
{
	return -ENODEV;
}

static inline int apple_piodma_bootstrap_radio_stop(struct device *supplier, struct pci_dev *pdev)
{
	return -ENODEV;
}

static inline void apple_piodma_bootstrap_set_recover(struct device *supplier,
						      void (*recover)(void *data), void *data)
{
}
#endif
#endif
