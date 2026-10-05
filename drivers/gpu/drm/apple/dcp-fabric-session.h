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

struct dcp_fabric_drain_ops {
	void (*reserve_revoke)(void *ctx);
	void (*invalidate)(void *ctx);
	void (*unlock)(void *ctx);
	void (*drain)(void *ctx);
	void (*lock)(void *ctx);
};

/* Shared production ordering; drain waits with neither fabric nor tb held. */
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
