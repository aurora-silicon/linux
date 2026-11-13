/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef _ASAHI_G17_DIETEMP_H_
#define _ASAHI_G17_DIETEMP_H_

#include <linux/types.h>

struct device;

int asahi_g17_dietemp_get(struct device *dev, void **out);
int asahi_g17_dietemp_sample(void *data, u8 *effort);
void asahi_g17_dietemp_put(void *data);

#endif /* _ASAHI_G17_DIETEMP_H_ */
