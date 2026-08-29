/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_PCI_APPLE_H
#define _LINUX_PCI_APPLE_H

struct device;

int apple_pcie_tunnel_quiesce(struct device *dev);

#endif /* _LINUX_PCI_APPLE_H */
