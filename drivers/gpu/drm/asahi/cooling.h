/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef ASAHI_COOLING_H
#define ASAHI_COOLING_H
#include <linux/types.h>
#include "cooling_policy.h"
struct device;
struct asahi_gpu_cooling;

/* Separate typed fields, retained until unregister drains every callback.
 * RegionC is WB; both admitted PowerState mappings are WC.
 */
struct asahi_gpu_power_words {
	u32 *max_power_mw;
	u32 *ppm_enabled;
	u32 *ppm_target_mw;
};
struct asahi_gpu_cooling *asahi_gpu_cooling_register(struct device *dev,
					const struct asahi_gpu_power_words *words);
int asahi_gpu_cooling_request(struct asahi_gpu_cooling *cooling,
			     enum asahi_gpu_cap_source source, u32 mw);
void asahi_gpu_cooling_unregister(struct asahi_gpu_cooling *cooling);
void asahi_gpu_cooling_rearm(struct asahi_gpu_cooling *cooling);
#endif
