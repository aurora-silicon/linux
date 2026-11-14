// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * Run-time switches for growing the tiler heap of a busy render pair ahead of
 * a render, one for the A18 Pro (default 1) and one for the M5 GPUs (default 0
 * until measured). 1 grows it by appending blocks to its inventory, as a
 * firmware grow request does; 0 grows the heap ahead of a render only when the
 * render is submitted to an idle pair. Read on every use, so either can be
 * flipped without reloading the driver.
 */
#include <linux/compiler.h>
#include <linux/moduleparam.h>

static unsigned int g17_busy_tvb_grow = 1;
module_param(g17_busy_tvb_grow, uint, 0644);
MODULE_PARM_DESC(g17_busy_tvb_grow,
		 "A18 Pro: grow a busy render pair's tiler heap ahead of a render (1: on, default; 0: idle pairs only)");

static unsigned int g17_m5_busy_tvb_grow;
module_param(g17_m5_busy_tvb_grow, uint, 0644);
MODULE_PARM_DESC(g17_m5_busy_tvb_grow,
		 "M5: grow a busy render pair's tiler heap ahead of a render (1: on; 0: idle pairs only, default)");

unsigned int asahi_g17_busy_tvb_grow(void);
unsigned int asahi_g17_m5_busy_tvb_grow(void);

unsigned int asahi_g17_busy_tvb_grow(void)
{
	return READ_ONCE(g17_busy_tvb_grow);
}

unsigned int asahi_g17_m5_busy_tvb_grow(void)
{
	return READ_ONCE(g17_m5_busy_tvb_grow);
}
