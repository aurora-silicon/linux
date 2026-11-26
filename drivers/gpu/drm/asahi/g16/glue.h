/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef _ASAHI_G16_GLUE_H
#define _ASAHI_G16_GLUE_H

struct asahi_g16_cooling;
struct device;

u32 asahi_g16_timebase_hz(void);
bool asahi_g16_cooling_has_pmp(const struct asahi_g16_cooling *cooling);
int asahi_g16_display_prepare(struct device *dev, const char *display_compat);

#endif /* _ASAHI_G16_GLUE_H */
