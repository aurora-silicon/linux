/* SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause */
#ifndef _APPLE_ATC_TUNNEL_H
#define _APPLE_ATC_TUNNEL_H

#include <linux/errno.h>
#include <linux/of.h>
#include <linux/types.h>

/*
 * The T602x per-DP-IN tunnel clock belongs to the T602x ATC IP block, which
 * every die of T6020/T6021/T6022 carries: qualify the PHY node, not the SoC.
 * Which ports carry tunnel routes is the connector's DT wiring.
 */
static inline bool apple_atc_t602x_qualified(const struct device_node *np)
{
	return of_device_is_compatible(np, "apple,t6020-atcphy");
}

/*
 * A T602x ATC core whose register window covers the tunnel clock registers
 * (up to 0x7048). Every port's core has the same layout, on either die.
 */
static inline bool apple_atc_t602x_core_valid(const struct device_node *np, u64 size)
{
	return apple_atc_t602x_qualified(np) && size >= 0x7048;
}

static inline int apple_atc_t602x_gate(bool own_wiring, bool routes_present, bool valid_core)
{
	if (!routes_present)
		return -EOPNOTSUPP;
	if (!valid_core)
		return -EINVAL;
	return own_wiring ? 0 : -EOPNOTSUPP;
}

#endif
