/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef __APPLE_DCP_RELINK_H__
#define __APPLE_DCP_RELINK_H__

#include <linux/jiffies.h>
#include <linux/types.h>

/*
 * Relinking a Type-C sink: replaying an unplug and a replug of a sink that
 * DCP dropped right after its link came up, although it still asserts HPD,
 * see dcp_typec_relink(). These decisions take the state under the DCP's
 * relink lock.
 */

/* relinks per attachment or DPMS wake */
#define DCP_RELINK_RETRIES	3
#define DCP_RELINK_UNPLUG_MS	1000
/*
 * DCP drops such a sink about 2 s after HPD was asserted for it. A sink it
 * drops later, with HPD still high (a TV behind a DP-to-HDMI adapter that
 * goes to standby), is handled as unplugged, as before.
 */
#define DCP_RELINK_LINKUP_MS	10000

struct dcp_relink_state {
	u64 generation;		/* the Type-C attachment described */
	unsigned long linked;	/* when HPD was last asserted for it, 0: unknown */
	u32 tries;
	bool active;		/* a relink is queued or runs */
	bool enabled;		/* relinks may be queued */
};

static inline unsigned long dcp_relink_stamp(unsigned long now)
{
	return now ?: 1;
}

/*
 * Another attachment starts afresh. A running relink stays marked: its own
 * unplug and replug change the generation.
 */
static inline void dcp_relink_sync(struct dcp_relink_state *s, u64 generation)
{
	if (s->generation == generation)
		return;
	s->generation = generation;
	s->linked = 0;
	s->tries = 0;
}

/* A display was attached as @generation. */
static inline void dcp_relink_attach(struct dcp_relink_state *s, u64 generation)
{
	dcp_relink_sync(s, generation);
}

/*
 * HPD is asserted for attachment @generation, @now: its link comes up. Not
 * for a reconnect that made a new generation without an attachment.
 */
static inline void dcp_relink_link_up(struct dcp_relink_state *s,
				      u64 generation, unsigned long now)
{
	if (s->generation == generation)
		s->linked = dcp_relink_stamp(now);
}

/*
 * DCP dropped a sink that still asserts HPD, @now. True: queue a relink.
 * False: handle the drop as an unplug, as before. One relink runs at a time.
 */
static inline bool dcp_relink_unplug(struct dcp_relink_state *s, u64 generation,
				     unsigned long now)
{
	if (s->active || !s->enabled)
		return false;
	dcp_relink_sync(s, generation);
	if (!s->linked ||
	    !time_before(now, s->linked + msecs_to_jiffies(DCP_RELINK_LINKUP_MS)) ||
	    s->tries >= DCP_RELINK_RETRIES)
		return false;
	s->tries++;
	s->active = true;
	return true;
}

/*
 * The relink attached the sink again, as @generation, @now. It keeps the
 * retries of the attachment it replays.
 */
static inline void dcp_relink_reattach(struct dcp_relink_state *s,
				       u64 generation, unsigned long now)
{
	s->generation = generation;
	s->linked = dcp_relink_stamp(now);
}

static inline void dcp_relink_end(struct dcp_relink_state *s)
{
	s->active = false;
}

/* A DPMS wake of @generation, @now, trains the link anew as a replug does. */
static inline void dcp_relink_wake(struct dcp_relink_state *s, u64 generation,
				   unsigned long now)
{
	dcp_relink_sync(s, generation);
	s->tries = 0;
	s->linked = dcp_relink_stamp(now);
}

/* Relinks may (not) be queued; nothing runs once they may not. */
static inline void dcp_relink_enable(struct dcp_relink_state *s, bool enable)
{
	s->enabled = enable;
	if (!enable)
		s->active = false;
}

#endif /* __APPLE_DCP_RELINK_H__ */
