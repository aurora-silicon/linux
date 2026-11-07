/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef _LINUX_SOC_APPLE_T8132_H
#define _LINUX_SOC_APPLE_T8132_H

#include <linux/of.h>
#include <linux/types.h>

/*
 * The M4 MacBook Airs, J713 (13-inch) and J715 (15-inch): one board design
 * and boot loader handoff. Their ADTs differ in the panel, the speakers, the
 * radio's firmware island and per-machine trims, all of which their device
 * trees describe.
 */
static inline bool apple_t8132_air(void)
{
	return of_machine_is_compatible("apple,j713") ||
	       of_machine_is_compatible("apple,j715");
}

#endif
