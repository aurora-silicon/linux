/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef MT7932_BANDWIDTH_H
#define MT7932_BANDWIDTH_H

#include "channels.h"

/* One startup ceiling: 20/40/80 are 0/1/2 here, not CID0f's encoding. */
#define MT7932_MAX_5G_BW 2

static inline u8 mt7932_domain_5g_bw(void)
{
	return MT7932_MAX_5G_BW == 0 ? 1 : MT7932_MAX_5G_BW == 1 ? 0 : 2;
}

/* EE4d width 0 combines 20/40; primary and extension select the center.
 * Keep all operational primaries inside 36..48, and 2 GHz at 20 MHz.
 */
static inline bool mt7932_channel_layout(u8 primary, u8 width, u8 extension,
					u8 maximum, u8 *center, u8 *bw)
{
	u8 band = mt7932_channel_band(primary);
	u8 expected = primary == 36 || primary == 44 ? 1 : 3;

	if (!band || maximum > 2 || width > 1)
		return false;
	if (!width && !extension) {
		*center = primary;
		*bw = 0;
		return true;
	}
	if (band != 2 || extension != expected)
		return false;
	*bw = width ? 2 : 1;
	if (*bw > maximum)
		return false;
	*center = width ? 42 : extension == 1 ? primary + 2 : primary - 2;
	return true;
}

#endif
