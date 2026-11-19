/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef _APPLE_DCP_IBOOTEP_H
#define _APPLE_DCP_IBOOTEP_H

#include <linux/types.h>

struct apple_dcp;

/* Read-only publication check; does not send firmware commands. */
bool ibootep_is_ready(struct apple_dcp *dcp);

/* Queue external HPD/timing/color queries; never call synchronously from AFK RX. */
int ibootep_query_modes(struct apple_dcp *dcp);

#endif
