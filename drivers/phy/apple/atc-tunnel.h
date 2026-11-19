/* SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause */
#ifndef _APPLE_ATC_TUNNEL_H
#define _APPLE_ATC_TUNNEL_H

#include <linux/errno.h>
#include <linux/types.h>

/*
 * May a PHY with a pixel clock per DP IN adapter (@dpin_clocks, from its
 * match data) run a DP tunnel clock? Only if the Type-C connector it serves
 * is wired to both DP IN adapters of its router (@dual_stream, from the
 * connector's display routes, apple_dp_tunnel_dual_stream()).
 */
static inline int apple_atc_dpin_clock_gate(bool dpin_clocks, bool dual_stream)
{
	return dpin_clocks && dual_stream ? 0 : -EOPNOTSUPP;
}

#endif
