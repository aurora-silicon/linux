/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LINUX_APPLE_CENTAURI_H
#define _LINUX_APPLE_CENTAURI_H

#include <linux/types.h>

struct pci_dev;

/* The caller must hold a managed device link to the Control supplier. */
bool apple_centauri_control_ready(struct pci_dev *pdev);

#endif
