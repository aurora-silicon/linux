// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* Copyright 2023 */

#include <linux/completion.h>

#include "afk.h"
#include "dcp.h"

static void disp_service_init(struct neo_apple_epic_service *service, const char *name,
			const char *class, s64 unit)
{
}


static const struct neo_apple_epic_service_ops neo_ibootep_ops[] = {
	{
		.name = "disp0-service",
		.init = disp_service_init,
	},
	{}
};

int neo_ibootep_init(struct neo_apple_dcp *neo_dcp)
{
	neo_dcp->neo_ibootep = neo_afk_init(neo_dcp, DISP0_ENDPOINT, neo_ibootep_ops);
	neo_afk_start(neo_dcp->neo_ibootep);

	return 0;
}
