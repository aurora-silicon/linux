/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef _AURORA_G17_FANS_H
#define _AURORA_G17_FANS_H
struct device;
int aurora_zone_temp(const char *name, int *temp);
int aurora_fans_request(struct device *dev, unsigned int permille, unsigned int min_step_rpm);
#endif
