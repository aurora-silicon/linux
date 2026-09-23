/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_PCI_APPLE_H
#define _LINUX_PCI_APPLE_H

struct device;
struct device_node;

int apple_pcie_tunnel_prepare(struct device *dev, struct device_node *tunnel);
int apple_pcie_tunnel_quiesce(struct device *dev);
int apple_pcie_tunnel_restore(struct device *dev);

#endif /* _LINUX_PCI_APPLE_H */
