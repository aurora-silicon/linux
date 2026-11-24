/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef _ASAHI_SLEEP_PD_H
#define _ASAHI_SLEEP_PD_H

struct device;

int asahi_sleep_pd_init(struct device *dev);
int asahi_sleep_pd_suspend_noirq(struct device *dev);
int asahi_sleep_pd_resume_noirq(struct device *dev);

#endif
