/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Interface between the functions of the J700 MT7932 radio.
 */
#ifndef __LINUX_MT7932_H
#define __LINUX_MT7932_H

#include <linux/types.h>

struct pci_dev;

bool mt7932_fullmac_ready(struct pci_dev *pdev);

#endif /* __LINUX_MT7932_H */
