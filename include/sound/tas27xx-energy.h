/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __SOUND_TAS27XX_ENERGY_H
#define __SOUND_TAS27XX_ENERGY_H

#include <linux/types.h>
#include <linux/errno.h>
#include <linux/kconfig.h>

struct snd_soc_component;

/* Read-back facts; an HPF selector alone is not an energy certificate. */
struct tas27xx_playback_state {
	u32 gain_millidbv;
	u32 hpf;
	u32 left_slot;
	u32 right_slot;
	u32 source;
	u32 transfer;
	/* A qualified bound for arbitrary prior full-scale input and epochs. */
	u32 tail_us;
};

#if IS_REACHABLE(CONFIG_SND_SOC_TAS2764)
int tas2764_playback_state(struct snd_soc_component *component,
			  struct tas27xx_playback_state *state);
#else
static inline int tas2764_playback_state(struct snd_soc_component *component,
					struct tas27xx_playback_state *state)
{
	return -EOPNOTSUPP;
}
#endif

#if IS_REACHABLE(CONFIG_SND_SOC_TAS2770)
int tas2770_playback_state(struct snd_soc_component *component,
			  struct tas27xx_playback_state *state);
#else
static inline int tas2770_playback_state(struct snd_soc_component *component,
					struct tas27xx_playback_state *state)
{
	return -EOPNOTSUPP;
}
#endif

#endif
