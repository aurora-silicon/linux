/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/* Copyright 2026 Ryan Murray */
#ifndef __APPLE_DCP_HDCP_H__
#define __APPLE_DCP_HDCP_H__

#include <linux/build_bug.h>
#include <linux/jiffies.h>
#include <linux/types.h>
#include <drm/display/drm_hdcp.h>
#include <drm/drm_mode.h>

/*
 * HDCP 2.2 on an external DisplayPort connector, as the AP runs it: the AP
 * is the transmitter, the SEP holds its secrets (apple_sep endpoint 0x0e),
 * and DCP carries the receiver's messages over the link.  macOS runs this
 * on the external connectors only; the built-in eDP panel never gets a
 * receiver session, so this never applies to it.
 *
 * This file is the policy, with no hardware behind it:
 *
 * - Content Protection is attached to an external DP connector only when
 *   both the SEP client and a DCP relay for that connector are bound.  With
 *   either missing, the property does not exist and nothing changes.
 * - A session follows the transmitter order below, stored km and Type 0
 *   only.  Each step reports success for the link generation it ran on.
 * - ENABLED needs every step, the 200 ms wait before encryption, and an
 *   acknowledged encryption enable, all on one link generation.
 * - Any failed step, a new link generation, or the SEP client or relay
 *   going away drops the session to DESIRED and asks for teardown.  A late
 *   report from a session or generation that is gone is ignored.  The
 *   display itself is never touched: a session that fails leaves an
 *   unprotected, working output.
 * - Retries back off: DCP_HDCP_RETRY_MS doubled per failure, capped at
 *   DCP_HDCP_RETRY_MAX_MS, reset by a success or a new link generation.
 */
#define DCP_HDCP_RETRY_MS	1000
#define DCP_HDCP_RETRY_MAX_MS	60000

enum dcp_hdcp_step {
	DCP_HDCP_STEP_OPEN,		/* SEP session open, r_tx from SEP */
	DCP_HDCP_STEP_AKE_INIT,		/* write AKE_Init */
	DCP_HDCP_STEP_CERT,		/* read AKE_Send_Cert, SEP checks it */
	DCP_HDCP_STEP_STORED_KM,	/* write AKE_Stored_km from SEP */
	DCP_HDCP_STEP_H_PRIME,		/* read AKE_Send_H_prime, SEP checks it */
	DCP_HDCP_STEP_LC_INIT,		/* write LC_Init (r_n from SEP) */
	DCP_HDCP_STEP_L_PRIME,		/* read LC_Send_L_prime, SEP checks it */
	DCP_HDCP_STEP_SKE,		/* write SKE_Send_Eks from SEP */
	DCP_HDCP_STEP_TYPE,		/* write the stream type (Type 0) */
	DCP_HDCP_STEP_ENCRYPT,		/* enable encryption, after the wait */
	DCP_HDCP_STEPS,
};

/*
 * Message sizes, msg_id byte included, as the public structures define
 * them.  They match the lengths seen at the DCP boundary under macOS
 * (12/534/33/33/9/33/25), which ties the step order above to the capture.
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
	DCP_HDCP_RUN,		/* run session step link->step */
	DCP_HDCP_WAIT,		/* run DCP_HDCP_STEP_ENCRYPT at link->encrypt_at */
	DCP_HDCP_TEARDOWN,	/* encryption off and close, display untouched */
};

struct dcp_hdcp_link {
	enum dcp_hdcp_state state;
	enum dcp_hdcp_step step;
	u32 generation;		/* link generation of the running session */
	unsigned long encrypt_at;
	unsigned long retry_at;
	unsigned int failures;
	bool relay;		/* a DCP relay is bound for this connector */
	bool sep;		/* the SEP client is up */
};

static inline bool dcp_hdcp_attach_property(bool external_dp, bool relay, bool sep)
{
	return external_dp && relay && sep;
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

/* Userspace set the property: @want is DESIRED (true) or UNDESIRED. */
static inline enum dcp_hdcp_action dcp_hdcp_request(struct dcp_hdcp_link *l,
						    bool want, unsigned long now)
{
	if (!want) {
		enum dcp_hdcp_action a = dcp_hdcp_drop(l, false, now);

		l->state = DCP_HDCP_OFF;
		return a;
	}
	if (l->state == DCP_HDCP_OFF)
		l->state = DCP_HDCP_DESIRED;
	return DCP_HDCP_NONE;
}

/*
 * Starts a session on link generation @gen if protection is wanted, both
 * sides are bound and no backoff is pending.
 */
static inline enum dcp_hdcp_action dcp_hdcp_start(struct dcp_hdcp_link *l,
						  u32 gen, unsigned long now)
{
	if (l->state != DCP_HDCP_DESIRED || !l->relay || !l->sep)
		return DCP_HDCP_NONE;
	if (l->failures && time_before(now, l->retry_at))
		return DCP_HDCP_NONE;
	l->state = DCP_HDCP_AUTH;
	l->step = DCP_HDCP_STEP_OPEN;
	l->generation = gen;
	return DCP_HDCP_RUN;
}

/* Step @step of the session on generation @gen finished, @ok or not. */
static inline enum dcp_hdcp_action dcp_hdcp_step_done(struct dcp_hdcp_link *l,
						      u32 gen,
						      enum dcp_hdcp_step step,
						      bool ok, unsigned long now)
{
	/* A late report, from a session or generation that is gone. */
	if (l->state != DCP_HDCP_AUTH || gen != l->generation || step != l->step)
		return DCP_HDCP_NONE;
	if (!ok)
		return dcp_hdcp_drop(l, true, now);
	if (step == DCP_HDCP_STEP_ENCRYPT) {
		l->state = DCP_HDCP_ENABLED;
		l->failures = 0;
		return DCP_HDCP_NONE;
	}
	l->step++;
	if (l->step == DCP_HDCP_STEP_ENCRYPT) {
		l->encrypt_at = now +
			msecs_to_jiffies(HDCP_2_2_DELAY_BEFORE_ENCRYPTION_EN);
		return DCP_HDCP_WAIT;
	}
	return DCP_HDCP_RUN;
}

/* The encryption step may run once the wait is over. */
static inline bool dcp_hdcp_may_encrypt(const struct dcp_hdcp_link *l,
					u32 gen, unsigned long now)
{
	return l->state == DCP_HDCP_AUTH && l->step == DCP_HDCP_STEP_ENCRYPT &&
	       gen == l->generation && !time_before(now, l->encrypt_at);
}

/* The link retrained, went away or came back: it is now generation @gen. */
static inline enum dcp_hdcp_action dcp_hdcp_link_event(struct dcp_hdcp_link *l,
						       u32 gen, unsigned long now)
{
	if (gen == l->generation)
		return DCP_HDCP_NONE;
	l->failures = 0;
	l->generation = gen;
	return dcp_hdcp_drop(l, false, now);
}

/* The SEP client or the DCP relay came or went. */
static inline enum dcp_hdcp_action dcp_hdcp_bind(struct dcp_hdcp_link *l,
						 bool relay, bool sep,
						 unsigned long now)
{
	l->relay = relay;
	l->sep = sep;
	if (relay && sep)
		return DCP_HDCP_NONE;
	return dcp_hdcp_drop(l, false, now);
}

#endif /* __APPLE_DCP_HDCP_H__ */
