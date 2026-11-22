/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef APPLE_DPTX_CAPS_H
#define APPLE_DPTX_CAPS_H

#include <linux/types.h>

struct dcp_dptx_caps {
	u32 width, total_rate, active_rate;
};

#endif
