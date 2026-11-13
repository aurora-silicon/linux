/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/*
 * Software camera indicator: pure state and policy.
 *
 * The kernel draws a small opaque square on a swap layer that userspace
 * cannot address. It is best effort: the camera never waits on it and never
 * depends on it. It is NOT the secure indicator Apple's firmware draws
 * outside the OS: root who can run its own kernel defeats it.
 */
#ifndef DCP_INDICATOR_H
#define DCP_INDICATOR_H

#include <linux/types.h>
#include <linux/errno.h>

enum dcp_ind_state {
	DCP_IND_OFF,
	/* Requested; no present carrying the square has completed yet. */
	DCP_IND_PENDING,
	/* A present carrying the square completed. */
	DCP_IND_LIT,
};

struct dcp_indicator {
	enum dcp_ind_state state;
	/* Requested and not ended: it survives a DRM unbind, so the next bind shows it again. */
	bool wanted;
	/* An off commit was refused for a pending flip and is being retried. */
	bool off_pending;
};

enum dcp_ind_off_result {
	DCP_IND_OFF_DONE,
	/* A flip is still pending: ask again shortly. */
	DCP_IND_OFF_RETRY,
	DCP_IND_OFF_FAILED,
};

/* One owner at a time: a second request cannot reset a running one. */
static inline int dcp_ind_begin(struct dcp_indicator *ind)
{
	if (ind->state != DCP_IND_OFF)
		return -EBUSY;
	ind->state = DCP_IND_PENDING;
	ind->wanted = true;
	/* This request puts the square on again, so a retried off has no meaning. */
	ind->off_pending = false;
	return 0;
}

/* A completed present: the first one that carried the square confirms it. */
static inline void dcp_ind_present_done(struct dcp_indicator *ind, bool dot_shown)
{
	if (ind->state == DCP_IND_PENDING && dot_shown)
		ind->state = DCP_IND_LIT;
}

/* The request is over: stopped, failed or withdrawn. */
static inline void dcp_ind_end(struct dcp_indicator *ind)
{
	ind->state = DCP_IND_OFF;
	ind->wanted = false;
}

/* The plane is gone (DRM unbind) but the camera may still stream: keep the request. */
static inline void dcp_ind_suspend(struct dcp_indicator *ind)
{
	ind->state = DCP_IND_OFF;
	ind->off_pending = false;
}

/*
 * The result of an off commit. The commit is nonblocking, so a flip that is
 * still pending on the CRTC refuses it with -EBUSY. The square must still come
 * off, so that refusal is retried until the commit lands; any other error is
 * final. The camera never waits for it.
 */
static inline enum dcp_ind_off_result dcp_ind_off_result(struct dcp_indicator *ind, int err)
{
	ind->off_pending = err == -EBUSY;
	if (!err)
		return DCP_IND_OFF_DONE;
	return err == -EBUSY ? DCP_IND_OFF_RETRY : DCP_IND_OFF_FAILED;
}

/* Wanted but not on the panel: the next present after a rebind asks again. */
static inline bool dcp_ind_wants_resume(const struct dcp_indicator *ind)
{
	return ind->wanted && ind->state == DCP_IND_OFF;
}

/*
 * The reserved plane belongs to the kernel. Any other commit that changes it
 * (FB_ID=0, CRTC_ID=0, a client's own framebuffer, a move) is put back to the
 * state the plane had, so a blind clear of every plane, as the console restore
 * does, still succeeds. Carried unchanged, or disabled with the CRTC it sits
 * on (DPMS off, suspend, shutdown), the plane is left alone.
 */
static inline bool dcp_ind_plane_must_restore(bool reserved_plane, bool kernel_commit,
					      bool unchanged, bool crtc_going_off)
{
	return reserved_plane && !kernel_commit && !unchanged && !crtc_going_off;
}

#endif /* DCP_INDICATOR_H */
