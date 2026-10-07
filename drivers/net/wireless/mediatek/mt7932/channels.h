/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef MT7932_CHANNELS_H
#define MT7932_CHANNELS_H

/* Bounded operational whitelist, not the full calibration channel namespace.
 * Firmware RF bands1/2 are distinct from single-interface MAC index0.
 */
static inline unsigned int mt7932_channel_band(unsigned int channel)
{
	if (channel >= 1 && channel <= 13)
		return 1;
	if (channel >= 36 && channel <= 48 && !(channel % 4))
		return 2;
	return 0;
}

#endif
