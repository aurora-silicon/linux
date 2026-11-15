// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * A run-time switch for growing the tiler heap of a busy A18 Pro render pair
 * ahead of a render. 1 (the default) grows it by appending blocks to its
 * inventory, as a firmware grow request does; 0 grows the heap ahead of a
 * render only when the render is submitted to an idle pair. Read on every
 * use, so it can be flipped without reloading the driver.
 */
#include <linux/compiler.h>
#include <linux/moduleparam.h>

static unsigned int g17_busy_tvb_grow = 1;
module_param(g17_busy_tvb_grow, uint, 0644);
MODULE_PARM_DESC(g17_busy_tvb_grow,
		 "A18 Pro: grow a busy render pair's tiler heap ahead of a render (1: on, default; 0: idle pairs only)");

unsigned int asahi_g17_busy_tvb_grow(void);

unsigned int asahi_g17_busy_tvb_grow(void)
{
	return READ_ONCE(g17_busy_tvb_grow);
}
