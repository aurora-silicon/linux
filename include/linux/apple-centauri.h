/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LINUX_APPLE_CENTAURI_H
#define _LINUX_APPLE_CENTAURI_H

#include <linux/types.h>

struct pci_dev;

/* The caller must hold a managed device link to the Control supplier. */
bool apple_centauri_control_ready(struct pci_dev *pdev);
/*
 * True in a firmware sleep's suspend callbacks when the N1 (function fn's) is warm-reset and
 * booted again after the wake: its functions' DMA must all be released before the link goes down.
 */
bool apple_centauri_sleep_reboot(struct pci_dev *fn);
/* From a resume that found its function torn down by a suspend that stopped short */
void apple_centauri_sleep_rewake(struct pci_dev *fn);

#endif
