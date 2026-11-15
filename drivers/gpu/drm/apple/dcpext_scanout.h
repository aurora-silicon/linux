/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef _APPLE_DCPEXT_SCANOUT_H
#define _APPLE_DCPEXT_SCANOUT_H

#include <linux/types.h>
struct apple_dcp;

struct dcpext_frame {
	void *pixels;
	size_t size;
};

bool dcpext_scanout_pageflips(struct apple_dcp *dcp);
/* begin holds the presentation mutex until end, including on discard.
 * Callers must hold drm_dev_enter() across the pair to exclude removal.
 */
int dcpext_scanout_begin_frame(struct apple_dcp *dcp, struct dcpext_frame *frame);
int dcpext_scanout_end_frame(struct apple_dcp *dcp, bool present);

/* Register the explicit one-shot diagnostic only after the iBoot service exists. */
int dcpext_scanout_register(struct apple_dcp *dcp);

/* Caller holds hpd_mutex when invalidating a physical link. */
void dcpext_scanout_fault(struct apple_dcp *dcp, int error);
void dcpext_scanout_invalidate(struct apple_dcp *dcp);
void dcpext_scanout_link_restored(struct apple_dcp *dcp);
bool dcpext_scanout_terminal(struct apple_dcp *dcp);
bool dcpext_scanout_requested(struct apple_dcp *dcp);

#endif
