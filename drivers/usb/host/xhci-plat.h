/* SPDX-License-Identifier: GPL-2.0 */
/*
 * xhci-plat.h - xHCI host controller driver platform Bus Glue.
 *
 * Copyright (C) 2015 Renesas Electronics Corporation
 */

#ifndef _XHCI_PLAT_H
#define _XHCI_PLAT_H

struct device;
struct platform_device;
struct usb_hcd;

struct xhci_plat_priv {
	const char *firmware_name;
	unsigned long long quirks;
	bool power_lost;
	unsigned sideband_at_suspend:1;
	void (*plat_start)(struct usb_hcd *);
	int (*init_quirk)(struct usb_hcd *);
	int (*suspend_quirk)(struct usb_hcd *);
	int (*resume_quirk)(struct usb_hcd *);
	int (*post_resume_quirk)(struct usb_hcd *);
};

#define hcd_to_xhci_priv(h) ((struct xhci_plat_priv *)hcd_to_xhci(h)->priv)
#define xhci_to_priv(x) ((struct xhci_plat_priv *)(x)->priv)

int xhci_plat_probe(struct platform_device *pdev, struct device *sysdev,
		    const struct xhci_plat_priv *priv_match);

void xhci_plat_remove(struct platform_device *dev);
extern const struct dev_pm_ops xhci_plat_pm_ops;

#if IS_ENABLED(CONFIG_KUNIT)
#include <linux/suspend.h>

u64 xhci_plat_property_quirks(struct device *dev);
unsigned long xhci_plat_irq_flags(u64 quirks);
bool xhci_plat_keep_running(u64 quirks, suspend_state_t target, bool may_wakeup,
			    bool usb_wakeup);
bool xhci_plat_usb_wakeup(struct usb_hcd *hcd);
bool xhci_plat_stopped(u32 usbsts);
int xhci_plat_resume_from_runtime(struct device *dev);
#endif

#endif	/* _XHCI_PLAT_H */
