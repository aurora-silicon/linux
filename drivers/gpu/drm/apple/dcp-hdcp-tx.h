/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/* Copyright 2026 Ryan Murray */
#ifndef __APPLE_DCP_HDCP_TX_H__
#define __APPLE_DCP_HDCP_TX_H__

#include <linux/bits.h>
#include <linux/types.h>
#include <drm/display/drm_hdcp.h>

#include "dcp-hdcp-wire.h"

/*
 * One HDCP 2.2 transmitter authentication, as the AP runs it with the SEP
 * doing the cryptography: every message is built from the fields the SEP
 * returns and written to, or read from, the receiver's HDCP 2.2 registers
 * through DCP. The procedure runs once DCP has opened a session and started
 * it, and blocks: it waits for DCP's session events and sleeps where the
 * specification asks for it. It never touches the display.
 *
 * DCP owns the receiver timeouts and closes an attempt that runs out of
 * time; the waits below only guard against a session DCP forgets.
 */
#define DCP_HDCP_EVENT_SLACK_MS		500
/* Locality check attempts, each with a fresh r_n. */
#define DCP_HDCP_LC_TRIES		3

struct dcp_hdcp_tx_ops {
	/* Receiver registers (DP_HDCP_2_2_REG_*). */
	int (*read)(void *ctx, u32 addr, void *buf, size_t len);
	int (*write)(void *ctx, u32 addr, const void *buf, size_t len);
	/*
	 * Waits up to @ms for session event @op. Returns 0 when it came,
	 * -ETIMEDOUT, or -ECANCELED when the session was closed or the
	 * protection is no longer wanted.
	 */
	int (*wait)(void *ctx, enum dcp_hdcp_auth_op op, unsigned int ms);
	void (*report)(void *ctx, enum dcp_hdcp_progress_code code,
		       bool active, bool repeater, bool encrypted);
	/* drm_hdcp_check_ksvs_revoked(): the count of revoked IDs, or <0. */
	int (*revoked)(void *ctx, u8 *ids, unsigned int count);
	void (*sleep)(void *ctx, unsigned int ms);

	/* The SEP service, include/linux/apple-sep-hdcp.h. */
	int (*sep_open)(void *ctx, u32 ch, u8 *rtx);
	int (*sep_ake_cert)(void *ctx, u32 ch, const u8 *cert, const u8 *rrx,
			    bool *stored, u8 *ekh_km_m, u8 *ekpub_km);
	int (*sep_h_prime)(void *ctx, u32 ch, const u8 *rxcaps,
			   const u8 *txcaps, const u8 *h_prime);
	int (*sep_pairing)(void *ctx, u32 ch, const u8 *ekh_km);
	int (*sep_lc_init)(void *ctx, u32 ch, u8 *rn);
	int (*sep_l_prime)(void *ctx, u32 ch, const u8 *l_prime);
	int (*sep_ske)(void *ctx, u32 ch, u8 *edkey_ks, u8 *riv);
	int (*sep_repeater_list)(void *ctx, u32 ch, const u8 *ids,
				 unsigned int count, const u8 *rxinfo,
				 const u8 *seq_num_v, const u8 *v_prime, u8 *v);
	int (*sep_repeater_stream)(void *ctx, u32 ch, u8 stream_id, u8 type,
				   const u8 *seq_num_m, const u8 *m_prime);
	int (*sep_finish)(void *ctx, u32 ch);
	int (*sep_set_encryption)(void *ctx, u32 ch, bool on);
	void (*sep_close)(void *ctx, u32 ch);
};

struct dcp_hdcp_tx {
	const struct dcp_hdcp_tx_ops *ops;
	void *ctx;
	u32 channel;
	/* The content type asked for: 0 or 1. */
	u8 type;

	/* Results, valid after dcp_hdcp_authenticate() returned 0. */
	bool repeater;
	bool legacy_downstream;
	unsigned int device_count;
	unsigned int depth;

	/* Session state. */
	bool sep_open;
	bool encrypting;
	u32 seq_num_m;

	/* Scratch: the messages of one attempt. */
	u8 rxcaps[HDCP_2_2_RXCAPS_LEN];
	struct hdcp2_tx_caps txcaps;
	struct hdcp2_cert_rx cert;
	u8 rrx[HDCP_2_2_RRX_LEN];
	u8 ids[HDCP_2_2_RECEIVER_IDS_MAX_LEN];
};

/* The receiver says it does HDCP 2.2 over DP. */
static inline bool dcp_hdcp_rxcaps_capable(const u8 *rxcaps)
{
	return rxcaps[0] == HDCP_2_2_RX_CAPS_VERSION_VAL &&
	       HDCP_2_2_DP_HDCP_CAPABLE(rxcaps[2]);
}

/* Reads RxCaps: 1 when the receiver can do HDCP 2.2, 0 when not, or <0. */
int dcp_hdcp_capable(struct dcp_hdcp_tx *tx);

/*
 * Runs one authentication to encryption. On 0 the link is encrypted with
 * tx->type; on failure the caller ends it with dcp_hdcp_abort(). -EAGAIN
 * means the SEP was not available for now: no failure of the receiver.
 */
int dcp_hdcp_authenticate(struct dcp_hdcp_tx *tx);

/* Encryption off and the SEP session closed; safe at any point. */
void dcp_hdcp_abort(struct dcp_hdcp_tx *tx);

/*
 * The receiver's status on an encrypted link: 0 while it holds, -EAGAIN when
 * it asks for re-authentication or reports a link integrity failure.
 */
int dcp_hdcp_check_link(struct dcp_hdcp_tx *tx);

#endif /* __APPLE_DCP_HDCP_TX_H__ */
