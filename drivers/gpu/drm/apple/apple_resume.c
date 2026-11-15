// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* Copyright 2026 Ryan Murray */

#include <linux/bug.h>

#include <drm/drm_atomic.h>
#include <drm/drm_atomic_uapi.h>
#include <drm/drm_crtc.h>
#include <drm/drm_encoder.h>
#include <drm/drm_modes.h>
#include <drm/drm_modeset_helper_vtables.h>
#include <drm/drm_print.h>

#include "apple_resume.h"

/*
 * A mode can be admissible only while the link it was proven on is up: the
 * external H17P 180 Hz mode needs the current DPTX link training, and system
 * sleep releases that link.  Restoring such a CRTC as it was fails the whole
 * resume commit in its mode fixup, which leaves every output dark until
 * userspace reacts to the next hotplug.  Restore that CRTC disabled instead.
 * Userspace still believes the CRTC is on, and the DCP need not report the
 * same display again when its link comes back, so mark the link of each of
 * its connectors bad: userspace then sets the mode again, on the hotplug
 * event the caller sends, as after a failed link retraining.
 *
 * The suspend state holds every CRTC, plane and connector already, so it is
 * edited directly rather than through helpers that need the modeset locks.
 *
 * Returns the mask of the CRTCs to be resumed disabled.
 */
u32 apple_drm_drop_stale_crtcs(struct drm_atomic_commit *state)
{
	const struct drm_crtc_helper_funcs *funcs;
	struct drm_connector_state *conn_state;
	struct drm_plane_state *plane_state;
	struct drm_crtc_state *crtc_state;
	struct drm_connector *connector;
	struct drm_plane *plane;
	struct drm_crtc *crtc;
	u32 dropped = 0;
	int i, j;

	for_each_new_crtc_in_state(state, crtc, crtc_state, i) {
		funcs = crtc->helper_private;
		if (!crtc_state->enable || !funcs || !funcs->mode_fixup ||
		    funcs->mode_fixup(crtc, &crtc_state->mode,
				      &crtc_state->adjusted_mode))
			continue;

		drm_dbg_kms(crtc->dev,
			    "[CRTC:%d:%s] " DRM_MODE_FMT " needs a new link, resuming disabled\n",
			    crtc->base.id, crtc->name, DRM_MODE_ARG(&crtc_state->mode));

		for_each_new_plane_in_state(state, plane, plane_state, j) {
			if (plane_state->crtc != crtc)
				continue;
			crtc_state->plane_mask &= ~drm_plane_mask(plane);
			plane_state->crtc = NULL;
			drm_atomic_set_fb_for_plane(plane_state, NULL);
		}
		for_each_new_connector_in_state(state, connector, conn_state, j) {
			if (conn_state->crtc != crtc)
				continue;
			/* Detaching from a CRTC already in the state cannot fail. */
			if (WARN_ON(drm_atomic_set_crtc_for_connector(conn_state, NULL)))
				return dropped | drm_crtc_mask(crtc);
			/*
			 * Nor does a connector without a CRTC keep an encoder:
			 * the resume check warns about one left routed.
			 */
			if (conn_state->best_encoder)
				crtc_state->encoder_mask &=
					~drm_encoder_mask(conn_state->best_encoder);
			conn_state->best_encoder = NULL;
			conn_state->link_status = DRM_LINK_STATUS_BAD;
		}
		crtc_state->active = false;
		dropped |= drm_crtc_mask(crtc);
		if (WARN_ON(drm_atomic_set_mode_for_crtc(crtc_state, NULL)))
			break;
	}

	return dropped;
}
