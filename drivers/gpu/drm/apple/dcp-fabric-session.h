/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef __APPLE_DCP_FABRIC_SESSION_H__
#define __APPLE_DCP_FABRIC_SESSION_H__

#include <linux/errno.h>
#include <linux/types.h>

struct dcp_fabric_session {
	u64 generation;
	u64 cookie;
};

/* The live binding is a lease, not a tombstone across TB module reloads. */
static inline int dcp_fabric_binding_request(u64 live, u64 cookie, bool active,
					     bool same_binding)
{
	if (!cookie)
		return -EINVAL;
	if (!live)
		return active ? 0 : -ESTALE;
	if (cookie != live || (active && !same_binding))
		return -ESTALE;
	return active ? 1 : 0;
}

static inline bool dcp_fabric_callback_valid(u64 route, u64 admitted)
{
	return admitted && admitted == route;
}

/*
 * T8132 dcpext: whether its tunnel pixel clock stands refused after a
 * SetLinkRate of @rate that returned @ret, @refused before it. A stop (rate
 * 0), failed or not, is no usable clock and leaves a refusal as it is; only
 * a nonzero rate taken clears one. The route's release clears it apart.
 */
static inline bool dcp_fabric_clock_refused(bool refused, u32 rate, int ret)
{
	return rate ? ret != 0 : refused;
}

struct dcp_fabric_drain_ops {
	void (*reserve_revoke)(void *ctx);
	void (*invalidate)(void *ctx);
	void (*unlock)(void *ctx);
	void (*drain)(void *ctx);
	void (*lock)(void *ctx);
};

/*
 * The source's production ordering; drain waits with neither fabric nor tb
 * held. This tree has no tb_retiring to keep the pipeline reserved while the
 * fabric lock is dropped, so its fabric retires under the lock instead (see
 * dcp_tb_binding_retire()); the KUnit cases keep the ordering contract.
 */
static inline void dcp_fabric_drain_binding(const struct dcp_fabric_drain_ops *ops,
					    void *ctx)
{
	ops->reserve_revoke(ctx);
	ops->invalidate(ctx);
	ops->unlock(ctx);
	ops->drain(ctx);
	ops->lock(ctx);
}

static inline bool dcp_fabric_session_valid(struct dcp_fabric_session expected,
					    struct dcp_fabric_session live_session,
					    bool cable, bool retiring)
{
	return !retiring && cable && expected.generation == live_session.generation &&
	       expected.cookie == live_session.cookie;
}

#endif /* __APPLE_DCP_FABRIC_SESSION_H__ */
