/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_PCI_APPLE_H
#define _LINUX_PCI_APPLE_H

struct device;
struct device_node;
struct notifier_block;

/* Atomic notification; data is the host's parent device, valid in callback. */
#define APPLE_PCIE_TUNNEL_LINK_DOWN 1

int apple_pcie_tunnel_register_notifier(struct notifier_block *nb);
void apple_pcie_tunnel_unregister_notifier(struct notifier_block *nb);

bool apple_pcie_tunnel_needs_cold_init(struct device_node *np);
int apple_pcie_tunnel_prepare(struct device *dev, struct device_node *tunnel);
int apple_pcie_tunnel_quiesce(struct device *dev);
int apple_pcie_tunnel_restore(struct device *dev);
int apple_pcie_tunnel_check_state(struct device *dev);
bool apple_pcie_tunnel_link_kept(struct device *dev);

/* NHI prepares/finishes; PCIe device noirq callbacks own the port and DART. */
int apple_pcie_tunnel_cio_prepare(struct device *dev);
bool apple_pcie_tunnel_cio_stopped(struct device *dev);
void apple_pcie_tunnel_cio_lost(struct device *dev);
int apple_pcie_tunnel_cio_finish(struct device *dev);

#endif /* _LINUX_PCI_APPLE_H */
