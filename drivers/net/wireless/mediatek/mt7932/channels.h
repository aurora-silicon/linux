/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef MT7932_CHANNELS_H
#define MT7932_CHANNELS_H

#include <linux/nl80211.h>

/* Bounded operational whitelist, not the full calibration channel namespace.
 * Firmware RF bands1/2 are distinct from single-interface MAC index0.
 * 5 GHz covers the 20 MHz primaries of UNII-1/2 (36..64), UNII-2e (100..144)
 * and UNII-3 (149..165); regulatory flags decide which of them are usable.
 */
#define MT7932_CHANNELS_2G 13
#define MT7932_CHANNELS_5G 25
#define MT7932_CHANNELS (MT7932_CHANNELS_2G + MT7932_CHANNELS_5G + MT7932_CHANNELS_6G)

/* Channel numbers are meaningful only together with their nl80211 band. */
static inline unsigned int mt7932_channel_band(enum nl80211_band band,
					       unsigned int channel)
{
	switch (band) {
	case NL80211_BAND_2GHZ:
		return channel >= 1 && channel <= 13 ? 1 : 0;
	case NL80211_BAND_5GHZ:
		if (((channel >= 36 && channel <= 64) ||
		     (channel >= 100 && channel <= 144)) && !(channel % 4))
			return 2;
		return channel >= 149 && channel <= 165 &&
			!((channel - 149) % 4) ? 2 : 0;
	case NL80211_BAND_6GHZ:
		return channel >= 1 && channel <= 233 && !((channel - 1) % 4) ? 3 : 0;
	default:
		return 0;
	}
}

/* UNII-5..8 20 MHz primaries. Channel 2 (5935 MHz) is not in these blocks.
 * Enumeration alone grants no permission to scan or transmit.
 */
#define MT7932_CHANNELS_6G 59

static inline unsigned int mt7932_channel_6g(unsigned int index)
{
	return index < MT7932_CHANNELS_6G ? 1 + 4 * index : 0;
}

static inline bool mt7932_channel_psc(enum nl80211_band band, unsigned int channel)
{
	return band == NL80211_BAND_6GHZ && mt7932_channel_band(band, channel) &&
		channel >= 5 && !((channel - 5) % 16);
}

/* Band-qualified 40/80 MHz centers; incomplete upper-edge blocks stay 20 MHz. */
static inline unsigned int mt7932_channel_6g_center(unsigned int primary, bool wide)
{
	if (!mt7932_channel_band(NL80211_BAND_6GHZ, primary) ||
	    primary > (wide ? 221 : 229))
		return 0;
	return wide ? 7 + 16 * ((primary - 1) / 16) : 3 + 8 * ((primary - 1) / 8);
}

/* Connac RX CH_FREQ encodes 6 GHz primaries as 181..239, independently
 * of the single-interface MAC index. See mt792x_get_status_freq_info().
 */
static inline bool mt7932_channel_from_rx(u8 encoded, enum nl80211_band *band,
					  u8 *channel)
{
	if (encoded >= 181) {
		if (encoded > 239)
			return false;
		*band = NL80211_BAND_6GHZ;
		*channel = 1 + 4 * (encoded - 181);
	} else {
		*band = encoded <= 14 ? NL80211_BAND_2GHZ : NL80211_BAND_5GHZ;
		*channel = encoded;
	}
	return mt7932_channel_band(*band, *channel) != 0;
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

	if (mt7932_channel_band(NL80211_BAND_5GHZ, primary) != 2 || primary == 165)
		return 0;
	slot = mt7932_channel_slot(primary, &base);
	return base + 4 * (slot & ~1U) + 2;
}

/* 80 MHz block centre of a 5 GHz primary; 0 where no block exists (165). */
static inline unsigned int mt7932_channel_vht80_centre(unsigned int primary)
{
	unsigned int base, slot;

	if (mt7932_channel_band(NL80211_BAND_5GHZ, primary) != 2 || primary == 165)
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
