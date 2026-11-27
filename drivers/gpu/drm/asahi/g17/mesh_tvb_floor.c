// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * A run-time switch for sizing an A18 Pro render pair's tiler heap ahead of a
 * pass with mesh shading. 1 (the default) grows the heap to the size its
 * automatic growth reaches before such a render; 0 sizes it from the pass's
 * tile count only. Read on every render submission.
 */
#include <linux/compiler.h>
#include <linux/moduleparam.h>

static unsigned int g17_mesh_tvb_floor = 1;
module_param(g17_mesh_tvb_floor, uint, 0644);
MODULE_PARM_DESC(g17_mesh_tvb_floor,
		 "A18 Pro: size the tiler heap ahead of a pass with mesh shading (1: on, default; 0: tile-count estimate only)");

unsigned int asahi_g17_mesh_tvb_floor(void);

unsigned int asahi_g17_mesh_tvb_floor(void)
{
	return READ_ONCE(g17_mesh_tvb_floor);
}
