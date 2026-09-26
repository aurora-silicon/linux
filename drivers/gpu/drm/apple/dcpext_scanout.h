/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef _APPLE_DCPEXT_SCANOUT_H
#define _APPLE_DCPEXT_SCANOUT_H

#include <linux/types.h>
struct apple_dcp;

/* Register the explicit one-shot diagnostic only after the iBoot service exists. */
int dcpext_scanout_register(struct apple_dcp *dcp);

/* Caller holds hpd_mutex when invalidating a physical link. */
void dcpext_scanout_fault(struct apple_dcp *dcp, int error);
void dcpext_scanout_invalidate(struct apple_dcp *dcp);
void dcpext_scanout_link_restored(struct apple_dcp *dcp);
bool dcpext_scanout_terminal(struct apple_dcp *dcp);
bool dcpext_scanout_requested(struct apple_dcp *dcp);

#endif
