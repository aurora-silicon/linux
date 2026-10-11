// SPDX-License-Identifier: GPL-2.0-only
#ifndef _APIF_T6050_H
#define _APIF_T6050_H
#include <linux/pci.h>
#include <linux/platform_device.h>
struct t6050_pcie;
struct t6050_pcie *t6050_pcie_init(struct platform_device *pdev, u64 doorbell,
				   unsigned int nvecs);
int t6050_pcie_cycle(struct t6050_pcie *pcie, struct pci_dev *endpoint);
#endif
