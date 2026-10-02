/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef M3_DCP_BRIGHTNESS_H
#define M3_DCP_BRIGHTNESS_H

#include <linux/bitops.h>
#include <linux/unaligned.h>

#define M3_PANEL_MAX_NITS 500
#define M3_PANEL_DEFAULT_NITS 140

/* J514S TEXT SHA a0af36d10a8abf2435918ba360e697529ddfaed56b89f9466e76d566b296e876:
 * 133880..13389c copies wire[354] and wire[35c] to transaction[76a]/[778].
 * 17cd68..17cdc0 converts binary64 nits to runtime property 19 (16.16).
 * The M4 double is at 35e: that layout must not be reused on M3.
 * Use firmware panel calibration; this is an SDR brightness range.
 */
static inline void m3_dcp_encode_brightness(u8 *swap, unsigned int nits)
{
	u64 bits = 0;

	/* Encode an exactly representable integer as IEEE754 binary64 without
	 * using floating point in the kernel. Caller bounds nits to 0..500.
	 */
	if (nits) {
		unsigned int exponent = fls(nits) - 1;

		bits = ((u64)(1023 + exponent) << 52) |
		       (((u64)nits << (52 - exponent)) & ((1ULL << 52) - 1));
	}
	swap[0x354] = 1;
	put_unaligned_le64(bits, swap + 0x35c);
}
#endif
