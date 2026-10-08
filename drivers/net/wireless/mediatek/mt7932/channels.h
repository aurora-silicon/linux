/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef MT7932_CHANNELS_H
#define MT7932_CHANNELS_H

/* Bounded operational whitelist, not the full calibration channel namespace.
 * Firmware RF bands1/2 are distinct from single-interface MAC index0.
 * 5 GHz covers the 20 MHz primaries of UNII-1/2 (36..64), UNII-2e (100..144)
 * and UNII-3 (149..165); regulatory flags decide which of them are usable.
 */
#define MT7932_CHANNELS_2G 13
#define MT7932_CHANNELS_5G 25
#define MT7932_CHANNELS (MT7932_CHANNELS_2G + MT7932_CHANNELS_5G)

static inline unsigned int mt7932_channel_band(unsigned int channel)
{
	if (channel >= 1 && channel <= 13)
		return 1;
	if (((channel >= 36 && channel <= 64) || (channel >= 100 && channel <= 144)) &&
	    !(channel % 4))
		return 2;
	if (channel >= 149 && channel <= 165 && !((channel - 149) % 4))
		return 2;
	return 0;
}

/* The nth 5 GHz primary, in the order the wiphy advertises them. */
static inline unsigned int mt7932_channel_5g(unsigned int index)
{
	if (index < 8)
		return 36 + 4 * index;
	if (index < 20)
		return 100 + 4 * (index - 8);
	return 149 + 4 * (index - 20);
}

/* Offset of a 5 GHz primary within its 80 MHz block numbering. */
static inline unsigned int mt7932_channel_slot(unsigned int primary, unsigned int *base)
{
	*base = primary >= 149 ? 149 : primary >= 100 ? 100 : 36;
	return (primary - *base) / 4;
}

/* 40 MHz pair centre of a 5 GHz primary; 0 where no pair exists (165). */
static inline unsigned int mt7932_channel_ht40_centre(unsigned int primary)
{
	unsigned int base, slot;

	if (mt7932_channel_band(primary) != 2 || primary == 165)
		return 0;
	slot = mt7932_channel_slot(primary, &base);
	return base + 4 * (slot & ~1U) + 2;
}

/* 80 MHz block centre of a 5 GHz primary; 0 where no block exists (165). */
static inline unsigned int mt7932_channel_vht80_centre(unsigned int primary)
{
	unsigned int base, slot;

	if (mt7932_channel_band(primary) != 2 || primary == 165)
		return 0;
	slot = mt7932_channel_slot(primary, &base);
	return base + 4 * (slot & ~3U) + 6;
}

/* A 5 GHz primary or a 40/80 MHz centre that calibration can be asked for. */
static inline bool mt7932_channel_5g_cal(unsigned int channel)
{
	if ((channel >= 36 && channel <= 64) || (channel >= 100 && channel <= 144))
		return !(channel % 2);
	return channel >= 149 && channel <= 167 && (channel % 2);
}

/* Calibration centre for a 5 GHz primary. 165 has no 40 MHz pair; its own
 * calibration package carries the record for the 165/169 centre, 167.
 */
static inline unsigned int mt7932_channel_cal_centre(unsigned int primary)
{
	return primary == 165 ? 167 : mt7932_channel_ht40_centre(primary);
}

#endif
