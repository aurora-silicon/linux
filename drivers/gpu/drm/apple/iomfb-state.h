/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef __APPLE_IOMFB_STATE_H__
#define __APPLE_IOMFB_STATE_H__

#include <linux/spinlock.h>

/* Shared by the atomic commit worker and the firmware callback thread. */
struct dcp_mode_state {
	spinlock_t lock; /* Protects all state and hotplug connector updates. */
	bool valid;
	bool changing;
	bool invalidated;
	bool hotplug_pending;
	bool connected_pending;
};

enum dcp_hotplug_action {
	DCP_HOTPLUG_NONE = 0,
	DCP_HOTPLUG_VBLANK = BIT(0),
	DCP_HOTPLUG_NOTIFY = BIT(1),
};

static inline void dcp_mode_begin(struct dcp_mode_state *state)
{
	unsigned long flags;

	spin_lock_irqsave(&state->lock, flags);
	state->valid = false;
	state->changing = true;
	state->invalidated = false;
	state->hotplug_pending = false;
	spin_unlock_irqrestore(&state->lock, flags);
}

/* For firmware that keeps a single mode and has no modeset in flight. */
static inline void dcp_mode_set_valid(struct dcp_mode_state *state, bool valid)
{
	unsigned long flags;

	spin_lock_irqsave(&state->lock, flags);
	state->valid = valid;
	spin_unlock_irqrestore(&state->lock, flags);
}

static inline void dcp_mode_invalidate(struct dcp_mode_state *state)
{
	unsigned long flags;

	spin_lock_irqsave(&state->lock, flags);
	state->valid = false;
	state->invalidated = true;
	spin_unlock_irqrestore(&state->lock, flags);
}

/* The caller holds state->lock. */
static inline unsigned int
dcp_mode_apply_hotplug(struct dcp_mode_state *state, bool connected,
		       bool *connector_connected)
{
	unsigned int action = DCP_HOTPLUG_NONE;

	if (!connected) {
		state->valid = false;
		action |= DCP_HOTPLUG_VBLANK;
	}
	if (connector_connected && *connector_connected != connected) {
		*connector_connected = connected;
		state->valid = false;
		action |= DCP_HOTPLUG_NOTIFY;
	}
	return action;
}

static inline unsigned int
dcp_mode_hotplug(struct dcp_mode_state *state, bool connected,
		 bool *connector_connected)
{
	unsigned long flags;
	unsigned int action = DCP_HOTPLUG_NONE;

	spin_lock_irqsave(&state->lock, flags);
	if (state->changing) {
		state->hotplug_pending = true;
		state->connected_pending = connected;
		/* A later connect must not erase a link loss during this call. */
		if (!connected)
			state->invalidated = true;
	} else {
		action = dcp_mode_apply_hotplug(state, connected,
						connector_connected);
	}
	spin_unlock_irqrestore(&state->lock, flags);
	return action;
}

static inline unsigned int
dcp_mode_finish(struct dcp_mode_state *state, bool completed,
		bool *connector_connected)
{
	unsigned long flags;
	unsigned int action = DCP_HOTPLUG_NONE;

	spin_lock_irqsave(&state->lock, flags);
	state->valid = completed && !state->invalidated;
	state->changing = false;
	if (state->hotplug_pending) {
		state->hotplug_pending = false;
		action = dcp_mode_apply_hotplug(state, state->connected_pending,
						connector_connected);
	}
	if (connector_connected && !*connector_connected)
		state->valid = false;
	/* A transient unplug also needs recovery if the final state is up. */
	if (state->invalidated && connector_connected && *connector_connected)
		action |= DCP_HOTPLUG_NOTIFY;
	spin_unlock_irqrestore(&state->lock, flags);
	return action;
}

#endif
