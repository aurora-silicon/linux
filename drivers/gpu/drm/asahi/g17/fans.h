/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef _ASAHI_G17_FANS_H
#define _ASAHI_G17_FANS_H
#include <linux/types.h>
struct device;
int asahi_zone_temp(const char *name, int *temp);
int asahi_zone_passive(const char *name, int *temp);
int asahi_fans_request(struct device *dev, unsigned int permille, unsigned int min_step_rpm);
bool asahi_fans_hotspot_described(void);
int asahi_fans_hotspot_policy(struct device *dev, int *trigger_mc, int *release_mc,
			      unsigned int *hold_ms);
int asahi_fans_request_described(struct device *dev, unsigned int permille,
				 unsigned int min_step_rpm);
#endif
