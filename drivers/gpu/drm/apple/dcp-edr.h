/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/* Copyright 2026 Ryan Murray */

#ifndef __APPLE_DCP_EDR_H__
#define __APPLE_DCP_EDR_H__

#include <linux/bits.h>
#include <linux/limits.h>
#include <linux/math64.h>
#include <linux/minmax.h>
#include <linux/types.h>

/*
 * EDR headroom on a desktop 26.x panel, x1000: 1000 is SDR white at the
 * panel level.  A swap asks for brightness SDR white x H and scales the
 * picture by 1 / H (see dcp_edr_prepare()).
 */
#define DCP_EDR_UNITY		1000U
#define DCP_EDR_STEP_UP		1047U	/* x1000 per swap */

/*
 * The panel's potential headroom, the ADT backlight node's
 * EDRPotentialHeadroom (16.16), as a ceiling x1000.  macOS never gives the
 * panel more.  0: no ceiling, for a value that is missing or not above 1.0.
 */
static inline u32 dcp_edr_ceiling(u32 fixed)
{
	u64 hm = ((u64)fixed * DCP_EDR_UNITY) >> 16;

	return hm > DCP_EDR_UNITY && hm <= U32_MAX ? hm : 0;
}

/* The headroom the panel has above @sdr nits: peak / sdr, up to @ceiling. */
static inline u32 dcp_edr_target(u32 peak, u32 sdr, u32 ceiling)
{
	u32 target = DCP_EDR_UNITY;

	if (sdr && peak > sdr)
		target = div_u64((u64)peak * DCP_EDR_UNITY, sdr);
	if (ceiling)
		target = clamp(target, DCP_EDR_UNITY, ceiling);
	return target;
}

/*
 * The headroom for the next swap: up one step per swap, down at once.  A
 * swap that lowers SDR white from @prev_sdr to @sdr while the headroom is
 * below its target raises the headroom just enough to hold the panel level,
 * as far as the target allows, and no further: the firmware eases the panel
 * level over several frames but applies the scale at once, so a level that
 * moved against the fade would show as a dip and a recovery.  Held, only the
 * scale moves and SDR content dims in the frame that asks for it.  A ramp
 * cut short continues once SDR white settles.
 */
static inline u32 dcp_edr_next(u32 cur, u32 target, u32 prev_sdr, u32 sdr)
{
	if (target < cur)
		return target;
	if (target == cur)
		return cur;
	if (sdr && prev_sdr > sdr)
		return min_t(u64, div_u64((u64)cur * prev_sdr + sdr / 2, sdr), target);
	return min_t(u64, div_u64((u64)cur * DCP_EDR_STEP_UP, DCP_EDR_UNITY) + 1,
		     target);
}

/* The binary64 nearest @num / @den, for small positive integers. */
static inline u64 dcp_binary64_ratio(u64 num, u64 den)
{
	u64 mantissa = 0;
	int exponent = 0;

	while (num >= 2 * den) {
		den <<= 1;
		exponent++;
	}
	while (num < den) {
		num <<= 1;
		exponent--;
	}
	/* den <= num < 2 den: 53 quotient bits, then round to nearest */
	for (unsigned int i = 0; i < 53; i++) {
		mantissa <<= 1;
		if (num >= den) {
			mantissa |= 1;
			num -= den;
		}
		num <<= 1;
	}
	if (num >= den && ++mantissa == 1ULL << 53) {
		mantissa >>= 1;
		exponent++;
	}
	return (u64)(exponent + 1023) << 52 | (mantissa & GENMASK_ULL(51, 0));
}

#endif /* __APPLE_DCP_EDR_H__ */
