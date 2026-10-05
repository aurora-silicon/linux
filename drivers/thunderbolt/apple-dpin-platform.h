/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _APPLE_DPIN_PLATFORM_H
#define _APPLE_DPIN_PLATFORM_H

#include <linux/types.h>

struct apple_dpin_policy;
struct device_node;

const struct apple_dpin_policy *
apple_dpin_policy_select(const struct apple_dpin_policy *hw,
			 const struct device_node *root, bool dual_stream);

#endif
