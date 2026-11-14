/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/*
 * 27.3 swap_submit request. The 27.0 request is 0xff4 bytes: a 0x6e0-byte swap, then the
 * surfaces and the tail. In 27.3 (kernelcache, swap_submit_dcp_main) it is 0x1144 bytes: the swap
 * grew to 0x830, the rest is the same. The swap object's own init puts swap_enabled and
 * swap_completed at 0x18c, 0x40 above the 27.0 offset 0x14c, so the fields from swap_id on moved.
 * MEASURED: both sizes, the tail layout. INFERENCE: the whole 0x98..0x6e0 block moves by 0x40; the
 * 0x110 bytes the swap gained beyond that are zero here and their meaning is unknown.
 */
#ifndef _APPLE_IOMFB_V27_3_SWAP_H
#define _APPLE_IOMFB_V27_3_SWAP_H

#include <linux/string.h>
#include <linux/types.h>

#define DCP_V27_SWAP_SIZE	0x6e0
#define DCP_V27_REQ_SIZE	0xff4
#define DCP_V27_3_SWAP_SIZE	0x830
#define DCP_V27_3_REQ_SIZE	0x1144
#define DCP_V27_3_SWAP_ID_OFF	0x98
#define DCP_V27_3_SWAP_ID_SHIFT	0x40

/* Build the 27.3 request from the 27.0 one. dst is DCP_V27_3_REQ_SIZE bytes. */
static inline void dcp_v27_3_swap_req(u8 *dst, const u8 *src)
{
	memset(dst, 0, DCP_V27_3_REQ_SIZE);
	memcpy(dst, src, DCP_V27_3_SWAP_ID_OFF);
	memcpy(dst + DCP_V27_3_SWAP_ID_OFF + DCP_V27_3_SWAP_ID_SHIFT, src + DCP_V27_3_SWAP_ID_OFF,
	       DCP_V27_SWAP_SIZE - DCP_V27_3_SWAP_ID_OFF);
	memcpy(dst + DCP_V27_3_SWAP_SIZE, src + DCP_V27_SWAP_SIZE,
	       DCP_V27_REQ_SIZE - DCP_V27_SWAP_SIZE);
}

#endif
