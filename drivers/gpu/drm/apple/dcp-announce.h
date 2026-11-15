/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef __APPLE_DCP_ANNOUNCE_H__
#define __APPLE_DCP_ANNOUNCE_H__

#include <linux/jiffies.h>
#include <linux/types.h>

/*
 * The modes of an external display arrive in pieces after its link comes up
 * (see dcp_external_modes_ready()).  Userspace that probes before they are
 * all in picks a mode from an incomplete list, then switches again once the
 * rest arrives: a second mode set the monitor has to resynchronize to.
 *
 * - A new connection is reported to DRM once it is ready, or once
 *   DCP_ANNOUNCE_TIMEOUT_MS have passed without that.
 * - Once reported, a connection stays reported until the display or the
 *   cable goes away: a link retrained on the same cable, for instance after
 *   system sleep, does not withdraw it.  A cable that is gone is reported as
 *   such at once, even while DCP still holds the old link.
 * - A reported connection can hold its next hotplug event, never the
 *   connection itself, until it is ready again, or until
 *   DCP_ANNOUNCE_RESUME_TIMEOUT_MS have passed without that.  That is the
 *   event that has userspace set the mode of a CRTC resumed disabled.
 * - A reported connection whose link came back under an active CRTC, but
 *   could not be set up again, is lost: it is reported gone until userspace
 *   releases that CRTC or the cable generation changes, and is then a new
 *   connection.  Releasing the CRTC brings it back once per cable
 *   generation.  Userspace may keep driving an output it already uses
 *   whatever its link status says, but not a display that went away.
 *
 * The timeout of a hold runs from dcp_announce_start(), and only for the
 * cable generation it was started in.  A hold that outlives its generation,
 * as across another system sleep, waits for the next one from the start.
 *
 * A hold that ends with the connection reported owes userspace a hotplug
 * event, as does a reported connection that ends, until
 * dcp_announce_notify() hands that event out.
 */
#define DCP_ANNOUNCE_TIMEOUT_MS		1000
#define DCP_ANNOUNCE_RESUME_TIMEOUT_MS	2000

enum dcp_announce_status {
	DCP_ANNOUNCE_DISCONNECTED,
	DCP_ANNOUNCE_HOLD,		/* the hotplug event is held */
	DCP_ANNOUNCE_CONNECTED,
};

struct dcp_announce {
	bool announced;		/* DRM reports the connection */
	bool withdrawn;		/* a reported connection ended, no event sent yet */
	bool released;		/* a hold ended, no event sent yet */
	bool holding;		/* waiting for the connection to be ready */
	bool timed;		/* the hold of @generation ends at @deadline */
	bool stopped;		/* nothing may be queued for the announcement */
	bool lost;		/* the connection of @lost_generation is lost */
	bool retried;		/* a loss of @retried_generation was retried */
	u64 generation;		/* the cable generation of the hold */
	u64 lost_generation;
	u64 retried_generation;
	unsigned long deadline;
};

struct dcp_announce_facts {
	u64 generation;		/* the current cable generation */
	bool connected;		/* DCP reports the display */
	bool present;		/* the cable is not known to be gone */
	bool ready;		/* a probe would list the modes of this connection */
	bool failed;		/* the link failed; it will not get ready */
	bool lost;		/* see dcp_announce_lost() */
};

/* The connection ended, whether DRM was told about it or not. */
static inline void dcp_announce_end(struct dcp_announce *a)
{
	a->withdrawn |= a->announced;
	a->announced = false;
	a->holding = false;
}

/*
 * Hold the next hotplug event of a reported connection until it is ready on
 * cable generation @generation.
 */
static inline bool dcp_announce_hold(struct dcp_announce *a, u64 generation)
{
	if (!a->announced)
		return false;
	a->holding = true;
	a->timed = false;
	a->generation = generation;
	return true;
}

/* Start the timeout of a hold that has none for its generation yet. */
static inline void dcp_announce_start(struct dcp_announce *a, unsigned long now)
{
	if (!a->holding || a->timed)
		return;
	a->timed = true;
	a->deadline = now + msecs_to_jiffies(a->announced ? DCP_ANNOUNCE_RESUME_TIMEOUT_MS :
					     DCP_ANNOUNCE_TIMEOUT_MS);
}

static inline bool dcp_announce_expired(const struct dcp_announce *a, u64 generation,
					unsigned long now)
{
	return a->holding && a->timed && a->generation == generation &&
	       !time_before(now, a->deadline);
}

/*
 * The link of the active CRTC of the connection on cable generation
 * @generation could not be set up again.
 */
static inline void dcp_announce_lose(struct dcp_announce *a, u64 generation)
{
	a->lost = true;
	a->lost_generation = generation;
}

static inline bool dcp_announce_lost(const struct dcp_announce *a, u64 generation)
{
	return a->lost && a->lost_generation == generation;
}

/*
 * Userspace released the CRTC of the connection on cable generation
 * @generation.  A lost connection is a new connection from now on, once per
 * cable generation: one lost again after that stays lost until the cable
 * generation changes, so that a link that cannot come up does not have
 * userspace switch between the display and none for as long as it is
 * attached.  Returns whether the connection is to be announced again.
 */
static inline bool dcp_announce_retry(struct dcp_announce *a, u64 generation)
{
	if (!dcp_announce_lost(a, generation)) {
		a->lost = false;
		return false;
	}
	if (a->retried && a->retried_generation == generation)
		return false;
	a->lost = false;
	a->retried = true;
	a->retried_generation = generation;
	return true;
}

/* Whether a hotplug event is owed that dcp_announce_notify() has not handed out. */
static inline bool dcp_announce_owed(const struct dcp_announce *a)
{
	return a->withdrawn || (a->released && !a->holding);
}

static inline enum dcp_announce_status
dcp_announce_update(struct dcp_announce *a, const struct dcp_announce_facts *f,
		    unsigned long now)
{
	if (!f->connected || !f->present || f->lost) {
		dcp_announce_end(a);
		return DCP_ANNOUNCE_DISCONNECTED;
	}
	if (!a->announced && !a->holding) {
		a->holding = true;
		a->timed = false;
		a->generation = f->generation;
	}
	if (a->holding && a->generation != f->generation) {
		a->generation = f->generation;
		a->timed = false;
	}
	if (a->holding && !f->ready && !f->failed &&
	    !dcp_announce_expired(a, f->generation, now))
		return DCP_ANNOUNCE_HOLD;
	if (a->holding) {
		a->holding = false;
		a->released = true;
	}
	a->announced = true;
	return DCP_ANNOUNCE_CONNECTED;
}

/*
 * Whether a hotplug event is due, which hands out any event owed.  A held
 * connection, or one that stays gone, changes nothing userspace can see,
 * unless a reported one ended before it.  Repeating the event of a lost
 * connection would only restart a compositor's wait for it to stay gone.
 */
static inline bool dcp_announce_notify(struct dcp_announce *a,
				       enum dcp_announce_status status)
{
	bool due = status == DCP_ANNOUNCE_CONNECTED || a->withdrawn;

	if (due) {
		a->withdrawn = false;
		a->released = false;
	}
	return due;
}

#endif /* __APPLE_DCP_ANNOUNCE_H__ */
