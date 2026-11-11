/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Prefetchable-window mirror decision for apple_pcie_config_write().
 *
 * When this returns true, a PCI-core write to a root port's prefetchable
 * window (config offsets 0x24-0x30) also reaches the port block, so its
 * decode follows the assigned layout instead of a stale latch.
 */

#include <linux/types.h>

/* static inline: included by the driver and by the KUNIT test only. */
static inline bool apple_pcie_mirror_pref_window(bool kernel_init,
						 bool tunnel_cold_init,
						 bool tunnel_intr2axi)
{
	return kernel_init || tunnel_cold_init;
}
