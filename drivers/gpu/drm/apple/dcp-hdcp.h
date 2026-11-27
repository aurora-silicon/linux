/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/* Copyright 2026 Ryan Murray */
#ifndef __APPLE_DCP_HDCP_H__
#define __APPLE_DCP_HDCP_H__

#include <linux/build_bug.h>
#include <linux/errno.h>
#include <linux/jiffies.h>
#include <linux/minmax.h>
#include <linux/types.h>
#include <drm/display/drm_hdcp.h>
#include <drm/drm_mode.h>

/*
 * HDCP 2.2 on an external connector, as the AP runs it: the AP is the
 * transmitter, the SEP holds its secrets and does the cryptography, and DCP
 * carries the receiver's messages over the link (see dcp-hdcp-wire.h). The
 * built-in panel never gets a receiver session, so this never applies to it.
 *
 * This file is the Content Protection policy, with no hardware behind it:
 *
 * - DCP opens an authentication session after the link comes up. The AP
 *   takes it only while protection is DESIRED, the SEP service and the DCP
 *   relay are bound and no backoff is pending; otherwise it declines.
 * - ENABLED needs a complete authentication, the 200 ms wait and an
 *   acknowledged encryption enable, all in one session on one link
 *   generation.
 * - A failure, a session DCP closes before the end, a new link generation,
 *   or the SEP service or relay going away drops the session to DESIRED and
 *   asks for teardown: encryption off and the SEP session closed. A late
 *   result from a session or generation that is gone is ignored. The display
 *   itself is never touched: a failed session leaves an unprotected, working
 *   output.
 * - A changed content type re-authenticates; UNDESIRED tears down.
 * - Retries back off: DCP_HDCP_RETRY_MS doubled per failure, capped at
 *   DCP_HDCP_RETRY_MAX_MS, reset by a success or a new link generation.
 */
#define DCP_HDCP_RETRY_MS	1000
#define DCP_HDCP_RETRY_MAX_MS	60000

/*
 * Message sizes, msg_id byte included, as the public structures define
 * them. The register blocks DCP moves are these without the msg_id byte.
 */
static_assert(sizeof(struct hdcp2_ake_init) == 12);
static_assert(sizeof(struct hdcp2_ake_send_cert) == 534);
static_assert(sizeof(struct hdcp2_ake_stored_km) == 33);
static_assert(sizeof(struct hdcp2_ake_send_hprime) == 33);
static_assert(sizeof(struct hdcp2_lc_init) == 9);
static_assert(sizeof(struct hdcp2_lc_send_lprime) == 33);
static_assert(sizeof(struct hdcp2_ske_send_eks) == 25);

enum dcp_hdcp_state {
	DCP_HDCP_OFF,		/* userspace does not want protection */
	DCP_HDCP_DESIRED,	/* wanted; no session running */
	DCP_HDCP_AUTH,		/* a session is running */
	DCP_HDCP_ENABLED,	/* encryption on, acknowledged */
};

enum dcp_hdcp_action {
	DCP_HDCP_NONE,
	DCP_HDCP_TEARDOWN,	/* encryption off and close, display untouched */
	DCP_HDCP_KICK,		/* ready: the next session DCP offers is taken */
};

struct dcp_hdcp_link {
	enum dcp_hdcp_state state;
	u8 type;		/* the content type asked for: 0 or 1 */
	u32 generation;		/* link generation of the running session */
	unsigned long retry_at;
	unsigned int failures;
	bool hold;		/* the SEP was busy: wait until retry_at */
	bool relay;		/* the DCP relay is bound */
	bool sep;		/* the SEP service is up */
};

/*
 * The property exists on an external connector whose display processor has
 * an HDCP channel, with the SEP service built in. Whether a session can run
 * is decided at runtime: until then the property stays DESIRED.
 */
static inline bool dcp_hdcp_attach_property(bool external, bool channel, bool sep)
{
	return external && channel && sep;
}

static inline u64 dcp_hdcp_property(const struct dcp_hdcp_link *l)
{
	switch (l->state) {
	case DCP_HDCP_OFF:
		return DRM_MODE_CONTENT_PROTECTION_UNDESIRED;
	case DCP_HDCP_ENABLED:
		return DRM_MODE_CONTENT_PROTECTION_ENABLED;
	default:
		return DRM_MODE_CONTENT_PROTECTION_DESIRED;
	}
}

static inline unsigned long dcp_hdcp_backoff(unsigned int failures)
{
	unsigned long ms = DCP_HDCP_RETRY_MS;

	while (failures-- > 1 && ms < DCP_HDCP_RETRY_MAX_MS)
		ms *= 2;
	return msecs_to_jiffies(min_t(unsigned long, ms, DCP_HDCP_RETRY_MAX_MS));
}

/* A session may start now. */
static inline bool dcp_hdcp_ready(const struct dcp_hdcp_link *l,
				  unsigned long now)
{
	return l->state == DCP_HDCP_DESIRED && l->relay && l->sep &&
	       !((l->failures || l->hold) && time_before(now, l->retry_at));
}

/* Drops a session; teardown is owed if one was running. */
static inline enum dcp_hdcp_action dcp_hdcp_drop(struct dcp_hdcp_link *l,
						 bool failed, unsigned long now)
{
	bool running = l->state == DCP_HDCP_AUTH || l->state == DCP_HDCP_ENABLED;

	if (l->state != DCP_HDCP_OFF)
		l->state = DCP_HDCP_DESIRED;
	if (failed) {
		l->failures++;
		l->retry_at = now + dcp_hdcp_backoff(l->failures);
	}
	return running ? DCP_HDCP_TEARDOWN : DCP_HDCP_NONE;
}

/*
 * Userspace set the property: @want is DESIRED (true) or UNDESIRED, @type
 * the content type.
 */
static inline enum dcp_hdcp_action dcp_hdcp_request(struct dcp_hdcp_link *l,
						    bool want, u8 type,
						    unsigned long now)
{
	if (!want) {
		enum dcp_hdcp_action a = dcp_hdcp_drop(l, false, now);

		l->state = DCP_HDCP_OFF;
		return a;
	}
	if (l->state == DCP_HDCP_OFF)
		l->state = DCP_HDCP_DESIRED;
	if (type != l->type) {
		l->type = type;
		/* A running session protects the old type: start again. */
		if (dcp_hdcp_drop(l, false, now) == DCP_HDCP_TEARDOWN)
			return DCP_HDCP_TEARDOWN;
	}
	return dcp_hdcp_ready(l, now) ? DCP_HDCP_KICK : DCP_HDCP_NONE;
}

/* DCP opened a session on link generation @gen: true if the AP takes it. */
static inline bool dcp_hdcp_open(struct dcp_hdcp_link *l, u32 gen,
				 unsigned long now)
{
	if (!dcp_hdcp_ready(l, now))
		return false;
	l->state = DCP_HDCP_AUTH;
	l->generation = gen;
	return true;
}

/*
 * The session on generation @gen ended its authentication with @result: 0,
 * -EAGAIN when the SEP was not available for now (retried after the first
 * backoff step, not counted as a failure), or another error.
 */
static inline enum dcp_hdcp_action dcp_hdcp_auth_done(struct dcp_hdcp_link *l,
						      u32 gen, int result,
						      unsigned long now)
{
	/* A late result, from a session or generation that is gone. */
	if (l->state != DCP_HDCP_AUTH || gen != l->generation)
		return DCP_HDCP_NONE;
	if (result == -EAGAIN) {
		l->hold = true;
		l->retry_at = now + dcp_hdcp_backoff(1);
		return dcp_hdcp_drop(l, false, now);
	}
	if (result)
		return dcp_hdcp_drop(l, true, now);
	l->state = DCP_HDCP_ENABLED;
	l->failures = 0;
	l->hold = false;
	return DCP_HDCP_NONE;
}

/*
 * DCP closed the session, or the receiver asked for re-authentication: a
 * session that had not finished failed; an enabled one is redone at once.
 */
static inline enum dcp_hdcp_action dcp_hdcp_closed(struct dcp_hdcp_link *l,
						   u32 gen, unsigned long now)
{
	if (gen != l->generation)
		return DCP_HDCP_NONE;
	if (l->state == DCP_HDCP_AUTH)
		return dcp_hdcp_drop(l, true, now);
	return dcp_hdcp_drop(l, false, now);
}

/* The link retrained, went away or came back: it is now generation @gen. */
static inline enum dcp_hdcp_action dcp_hdcp_link_event(struct dcp_hdcp_link *l,
						       u32 gen, unsigned long now)
{
	if (gen == l->generation)
		return DCP_HDCP_NONE;
	l->failures = 0;
	l->hold = false;
	l->generation = gen;
	return dcp_hdcp_drop(l, false, now);
}

/* The SEP service or the DCP relay came or went. */
static inline enum dcp_hdcp_action dcp_hdcp_bind(struct dcp_hdcp_link *l,
						 bool relay, bool sep,
						 unsigned long now)
{
	l->relay = relay;
	l->sep = sep;
	if (relay && sep)
		return dcp_hdcp_ready(l, now) ? DCP_HDCP_KICK : DCP_HDCP_NONE;
	return dcp_hdcp_drop(l, false, now);
}

#endif /* __APPLE_DCP_HDCP_H__ */
