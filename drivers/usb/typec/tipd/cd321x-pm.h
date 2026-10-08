/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __CD321X_PM_H__
#define __CD321X_PM_H__

#include <linux/types.h>

#define CD321X_RESUME_ATTEMPTS 3
#define CD321X_LINK_EVENTS 3

enum cd321x_pm_phase {
	CD321X_PM_RUNNING,
	CD321X_PM_PREPARED,
	CD321X_PM_REVALIDATE,
	CD321X_PM_APPLY,
	CD321X_PM_STALE,
	CD321X_PM_REMOVED,
	CD321X_PM_INITIALIZING,
};

/* All transitions and the associated work scheduling hold tps->lock. */
struct cd321x_pm_state {
	enum cd321x_pm_phase phase;
	unsigned int attempts_left;
	unsigned int link_events_left;
	bool force_reconnect;
	bool provider_busy;
	bool setup_pending;
	unsigned int setup_attempts_left;
};

static inline void cd321x_pm_new_connection(struct cd321x_pm_state *pm)
{
	pm->link_events_left = CD321X_LINK_EVENTS;
	pm->provider_busy = false;
}

static inline void cd321x_pm_init(struct cd321x_pm_state *pm)
{
	pm->phase = CD321X_PM_INITIALIZING;
	cd321x_pm_new_connection(pm);
}

static inline bool cd321x_pm_ready(struct cd321x_pm_state *pm)
{
	if (pm->phase != CD321X_PM_INITIALIZING)
		return false;
	pm->phase = CD321X_PM_REVALIDATE;
	pm->attempts_left = CD321X_RESUME_ATTEMPTS;
	if (pm->setup_pending)
		pm->setup_attempts_left = CD321X_RESUME_ATTEMPTS;
	return true;
}

static inline bool cd321x_pm_can_update(const struct cd321x_pm_state *pm)
{
	return !pm->setup_pending &&
	       (pm->phase == CD321X_PM_RUNNING || pm->phase == CD321X_PM_APPLY);
}

static inline void cd321x_pm_prepare(struct cd321x_pm_state *pm)
{
	if (pm->phase != CD321X_PM_REMOVED)
		pm->phase = CD321X_PM_PREPARED;
	pm->attempts_left = 0;
	pm->setup_attempts_left = 0;
	pm->provider_busy = false;
}

static inline bool cd321x_pm_resume(struct cd321x_pm_state *pm)
{
	if (pm->phase != CD321X_PM_PREPARED)
		return false;
	pm->phase = CD321X_PM_REVALIDATE;
	pm->attempts_left = CD321X_RESUME_ATTEMPTS;
	if (pm->setup_pending)
		pm->setup_attempts_left = CD321X_RESUME_ATTEMPTS;
	cd321x_pm_new_connection(pm);
	return true;
}

static inline bool cd321x_pm_link_event(struct cd321x_pm_state *pm)
{
	if (pm->phase == CD321X_PM_INITIALIZING ||
	    pm->phase == CD321X_PM_PREPARED || pm->phase == CD321X_PM_REMOVED ||
	    !pm->link_events_left)
		return false;

	/* Host reprobes must not create an unlimited series of recovery runs. */
	pm->link_events_left--;
	pm->provider_busy = false;
	pm->phase = CD321X_PM_REVALIDATE;
	pm->attempts_left = CD321X_RESUME_ATTEMPTS;
	/* A stale notification does not itself prove the current session failed. */
	return true;
}

static inline bool cd321x_pm_activation_ready(struct cd321x_pm_state *pm)
{
	/* A completion cannot refill failed recovery or restart a healthy cable. */
	if (!pm->provider_busy || pm->force_reconnect ||
	    (pm->phase != CD321X_PM_REVALIDATE && pm->phase != CD321X_PM_APPLY &&
	     pm->phase != CD321X_PM_STALE))
		return false;

	pm->provider_busy = false;
	pm->phase = CD321X_PM_REVALIDATE;
	if (!pm->attempts_left)
		pm->attempts_left = CD321X_RESUME_ATTEMPTS;
	return true;
}

#define CD321X_PM_FAULT_ACCEPTED	0x1U
#define CD321X_PM_FAULT_REFUSED		0x2U
#define CD321X_PM_READY_ACCEPTED	0x4U

/*
 * Apply the link fault and activation completion one worker pass collected.
 * An accepted fault clears provider_busy, so a completion that came with it
 * changes nothing more. A fault the budget refuses must not swallow the
 * completion.
 */
static inline unsigned int cd321x_pm_notifications(struct cd321x_pm_state *pm,
						   bool fault, bool ready)
{
	unsigned int res = 0;

	if (fault)
		res |= cd321x_pm_link_event(pm) ? CD321X_PM_FAULT_ACCEPTED :
						  CD321X_PM_FAULT_REFUSED;
	if (ready && cd321x_pm_activation_ready(pm))
		res |= CD321X_PM_READY_ACCEPTED;
	return res;
}

static inline bool cd321x_pm_begin_read(struct cd321x_pm_state *pm)
{
	if (pm->phase != CD321X_PM_REVALIDATE || !pm->attempts_left)
		return false;
	pm->attempts_left--;
	return true;
}

static inline void cd321x_pm_snapshot_ready(struct cd321x_pm_state *pm)
{
	pm->phase = CD321X_PM_APPLY;
}

static inline bool cd321x_pm_retry(struct cd321x_pm_state *pm, bool reconnect)
{
	/* Runtime cable updates need the same bounded fresh-read recovery. */
	if (pm->phase == CD321X_PM_RUNNING) {
		pm->provider_busy = !reconnect;
		pm->force_reconnect |= reconnect;
		pm->phase = CD321X_PM_REVALIDATE;
		pm->attempts_left = CD321X_RESUME_ATTEMPTS;
		return true;
	}
	if (pm->phase != CD321X_PM_REVALIDATE && pm->phase != CD321X_PM_APPLY)
		return false;
	pm->provider_busy = !reconnect;
	pm->force_reconnect |= reconnect;
	if (pm->attempts_left) {
		pm->phase = CD321X_PM_REVALIDATE;
		return true;
	}
	/* Only a fresh read, triggered by a later event, can reopen updates. */
	pm->phase = CD321X_PM_STALE;
	return false;
}

static inline bool cd321x_pm_event(struct cd321x_pm_state *pm)
{
	if (pm->phase != CD321X_PM_STALE)
		return false;
	pm->phase = CD321X_PM_REVALIDATE;
	pm->attempts_left = CD321X_RESUME_ATTEMPTS;
	return true;
}

static inline void cd321x_pm_complete(struct cd321x_pm_state *pm)
{
	if (pm->phase == CD321X_PM_APPLY) {
		pm->phase = CD321X_PM_RUNNING;
		pm->attempts_left = 0;
		pm->force_reconnect = false;
		pm->provider_busy = false;
	}
}

static inline void cd321x_pm_remove(struct cd321x_pm_state *pm)
{
	pm->phase = CD321X_PM_REMOVED;
	pm->attempts_left = 0;
}

#endif /* __CD321X_PM_H__ */
