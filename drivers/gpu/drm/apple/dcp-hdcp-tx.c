// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* Copyright 2026 Ryan Murray */

#include <linux/bitops.h>
#include <linux/errno.h>
#include <linux/unaligned.h>
#include <drm/display/drm_dp.h>
#include <drm/display/drm_hdcp.h>

#include "dcp-hdcp-tx.h"

/* Each step names itself, so a failed attempt says where it stopped. */
#define TX_READ(tx, reg, buf, len) \
	((tx)->step = "read " #reg, (tx)->ops->read((tx)->ctx, reg, buf, len))
#define TX_WRITE(tx, reg, buf, len) \
	((tx)->step = "write " #reg, (tx)->ops->write((tx)->ctx, reg, buf, len))
#define TX_SEP(tx, fn) ((tx)->step = #fn, (tx)->ops->fn)

static void tx_report(struct dcp_hdcp_tx *tx, enum dcp_hdcp_progress_code code,
		      bool encrypted)
{
	tx->ops->report(tx->ctx, code, true, tx->repeater, encrypted);
}

/*
 * Waits for a session event. A missed event is not fatal when the receiver
 * already says, in RxStatus, that what the event announces is there.
 */
static int tx_wait(struct dcp_hdcp_tx *tx, enum dcp_hdcp_auth_op op,
		   unsigned int spec_ms)
{
	u8 status;
	int ret;

	tx->step = "wait for DCP event";
	ret = tx->ops->wait(tx->ctx, op, spec_ms + DCP_HDCP_EVENT_SLACK_MS);
	if (ret != -ETIMEDOUT)
		return ret;
	if (TX_READ(tx, DP_HDCP_2_2_REG_RXSTATUS_OFFSET, &status, 1))
		return -ETIMEDOUT;
	switch (op) {
	case DCP_HDCP_AUTH_H_PRIME:
		return HDCP_2_2_DP_RXSTATUS_H_PRIME(status) ? 0 : -ETIMEDOUT;
	case DCP_HDCP_AUTH_PAIRING:
		return HDCP_2_2_DP_RXSTATUS_PAIRING(status) ? 0 : -ETIMEDOUT;
	case DCP_HDCP_AUTH_LIST:
		return HDCP_2_2_DP_RXSTATUS_READY(status) ? 0 : -ETIMEDOUT;
	default:
		return -ETIMEDOUT;
	}
}

/* Any revoked ID, or an SRM that cannot be read, fails the attempt. */
static int tx_revocation(struct dcp_hdcp_tx *tx, u8 *ids, unsigned int count)
{
	int ret = tx->ops->revoked(tx->ctx, ids, count);

	if (ret > 0)
		return -EPERM;
	return ret;
}

/* Twenty ones and twenty zeros, as every HDCP Receiver ID. */
static bool dcp_hdcp_receiver_id_valid(const u8 *id)
{
	unsigned int i, ones = 0;

	for (i = 0; i < HDCP_2_2_RECEIVER_ID_LEN; i++)
		ones += hweight8(id[i]);
	return ones == 20;
}

int dcp_hdcp_capable(struct dcp_hdcp_tx *tx)
{
	int ret = TX_READ(tx, DP_HDCP_2_2_REG_RX_CAPS_OFFSET, tx->rxcaps,
			  sizeof(tx->rxcaps));

	if (ret)
		return ret;
	return dcp_hdcp_rxcaps_capable(tx->rxcaps);
}

static int tx_ake(struct dcp_hdcp_tx *tx, bool *stored)
{
	u8 rtx[HDCP_2_2_RTX_LEN];
	u8 ekh_km_m[HDCP_2_2_E_KH_KM_M_LEN];
	u8 ekpub_km[HDCP_2_2_E_KPUB_KM_LEN];
	u8 h_prime[HDCP_2_2_H_PRIME_LEN];
	u8 ekh_km[HDCP_2_2_E_KH_KM_LEN];
	int ret;

	ret = TX_SEP(tx, sep_open)(tx->ctx, tx->channel, rtx);
	if (ret)
		return ret;
	tx->sep_open = true;

	/* AKE_Init */
	tx->txcaps.version = HDCP_2_2_RX_CAPS_VERSION_VAL;
	tx->txcaps.tx_cap_mask[0] = 0;
	tx->txcaps.tx_cap_mask[1] = 0;
	ret = TX_WRITE(tx, DP_HDCP_2_2_REG_RTX_OFFSET, rtx, sizeof(rtx));
	if (!ret)
		ret = TX_WRITE(tx, DP_HDCP_2_2_REG_TXCAPS_OFFSET, &tx->txcaps,
			       sizeof(tx->txcaps));
	if (ret)
		return ret;

	/* AKE_Send_Cert: the receiver has 100 ms to make it readable. */
	tx->step = "wait for AKE_Send_Cert";
	tx->ops->sleep(tx->ctx, HDCP_2_2_CERT_TIMEOUT_MS);
	ret = TX_READ(tx, DP_HDCP_2_2_REG_CERT_RX_OFFSET, &tx->cert,
		      sizeof(tx->cert));
	/* A Receiver ID has twenty ones and twenty zeros; anything else is a bad read. */
	if (!ret && !dcp_hdcp_receiver_id_valid(tx->cert.receiver_id)) {
		tx->step = "check Receiver ID";
		ret = -EPROTO;
	}
	if (!ret)
		ret = TX_READ(tx, DP_HDCP_2_2_REG_RRX_OFFSET, tx->rrx,
			      sizeof(tx->rrx));
	if (!ret)
		ret = TX_READ(tx, DP_HDCP_2_2_REG_RX_CAPS_OFFSET, tx->rxcaps,
			      sizeof(tx->rxcaps));
	if (ret)
		return ret;
	if (!dcp_hdcp_rxcaps_capable(tx->rxcaps))
		return -ENODEV;
	tx->repeater = HDCP_2_2_RX_REPEATER(tx->rxcaps[2]);
	ret = tx_revocation(tx, tx->cert.receiver_id, 1);
	if (ret)
		return ret;
	ret = TX_SEP(tx, sep_ake_cert)(tx->ctx, tx->channel, (const u8 *)&tx->cert,
				    tx->rrx, stored, ekh_km_m, ekpub_km);
	if (ret)
		return ret;

	/* AKE_Stored_km (Ekh(km) then m, contiguous) or AKE_No_Stored_km */
	if (*stored)
		ret = TX_WRITE(tx, DP_HDCP_2_2_REG_EKH_KM_WR_OFFSET, ekh_km_m,
			       sizeof(ekh_km_m));
	else
		ret = TX_WRITE(tx, DP_HDCP_2_2_REG_EKPUB_KM_OFFSET, ekpub_km,
			       sizeof(ekpub_km));
	if (ret)
		return ret;
	tx_report(tx, DCP_HDCP_PROGRESS_KM_SENT, false);

	/* AKE_Send_H_prime */
	ret = tx_wait(tx, DCP_HDCP_AUTH_H_PRIME,
		      *stored ? HDCP_2_2_HPRIME_PAIRED_TIMEOUT_MS :
				HDCP_2_2_HPRIME_NO_PAIRED_TIMEOUT_MS);
	if (!ret)
		ret = TX_READ(tx, DP_HDCP_2_2_REG_HPRIME_OFFSET, h_prime,
			      sizeof(h_prime));
	if (!ret)
		ret = TX_SEP(tx, sep_h_prime)(tx->ctx, tx->channel, tx->rxcaps,
					   (const u8 *)&tx->txcaps, h_prime);
	if (ret)
		return ret;
	if (*stored)
		return 0;

	/* AKE_Send_Pairing_Info */
	tx_report(tx, DCP_HDCP_PROGRESS_H_VERIFIED, false);
	ret = tx_wait(tx, DCP_HDCP_AUTH_PAIRING, HDCP_2_2_PAIRING_TIMEOUT_MS);
	if (!ret)
		ret = TX_READ(tx, DP_HDCP_2_2_REG_EKH_KM_RD_OFFSET, ekh_km,
			      sizeof(ekh_km));
	if (!ret)
		ret = TX_SEP(tx, sep_pairing)(tx->ctx, tx->channel, ekh_km);
	return ret;
}

static int tx_lc_ske(struct dcp_hdcp_tx *tx)
{
	u8 rn[HDCP_2_2_RN_LEN];
	u8 l_prime[HDCP_2_2_L_PRIME_LEN];
	u8 edkey_ks[HDCP_2_2_E_DKEY_KS_LEN];
	u8 riv[HDCP_2_2_RIV_LEN];
	int attempt, ret;

	/*
	 * LC_Init, then LC_Send_L_prime once the receiver had its time. A
	 * wrong L' is retried with a fresh r_n, a bounded number of times.
	 */
	for (attempt = 0; attempt < DCP_HDCP_LC_TRIES; attempt++) {
		ret = TX_SEP(tx, sep_lc_init)(tx->ctx, tx->channel, rn);
		if (!ret)
			ret = TX_WRITE(tx, DP_HDCP_2_2_REG_RN_OFFSET, rn, sizeof(rn));
		if (ret)
			return ret;
		tx->ops->sleep(tx->ctx, HDCP_2_2_DP_LPRIME_TIMEOUT_MS);
		ret = TX_READ(tx, DP_HDCP_2_2_REG_LPRIME_OFFSET, l_prime,
			      sizeof(l_prime));
		if (!ret)
			ret = TX_SEP(tx, sep_l_prime)(tx->ctx, tx->channel, l_prime);
		if (ret != -EACCES)
			break;
	}
	if (ret)
		return ret;

	/* SKE_Send_Eks */
	ret = TX_SEP(tx, sep_ske)(tx->ctx, tx->channel, edkey_ks, riv);
	if (!ret)
		ret = TX_WRITE(tx, DP_HDCP_2_2_REG_EDKEY_KS_OFFSET, edkey_ks,
			       sizeof(edkey_ks));
	if (!ret)
		ret = TX_WRITE(tx, DP_HDCP_2_2_REG_RIV_OFFSET, riv, sizeof(riv));
	if (!ret)
		tx_report(tx, DCP_HDCP_PROGRESS_SKE_SENT, false);
	return ret;
}

static int tx_receiver_list(struct dcp_hdcp_tx *tx)
{
	u8 rxinfo[HDCP_2_2_RXINFO_LEN];
	u8 seq_num_v[HDCP_2_2_SEQ_NUM_LEN];
	u8 v_prime[HDCP_2_2_V_PRIME_HALF_LEN];
	u8 v[2 * HDCP_2_2_V_PRIME_HALF_LEN];
	unsigned int count;
	int ret;

	ret = tx_wait(tx, DCP_HDCP_AUTH_LIST, HDCP_2_2_RECVID_LIST_TIMEOUT_MS);
	if (!ret)
		ret = TX_READ(tx, DP_HDCP_2_2_REG_RXINFO_OFFSET, rxinfo,
			      sizeof(rxinfo));
	if (!ret)
		ret = TX_READ(tx, DP_HDCP_2_2_REG_SEQ_NUM_V_OFFSET, seq_num_v,
			      sizeof(seq_num_v));
	if (!ret)
		ret = TX_READ(tx, DP_HDCP_2_2_REG_VPRIME_OFFSET, v_prime,
			      sizeof(v_prime));
	if (ret)
		return ret;

	/* Topology: too many devices or levels downstream fails the attempt. */
	if (HDCP_2_2_MAX_CASCADE_EXCEEDED(rxinfo[1]) ||
	    HDCP_2_2_MAX_DEVS_EXCEEDED(rxinfo[1]))
		return -E2BIG;
	count = (HDCP_2_2_DEV_COUNT_HI(rxinfo[0]) << 4) |
		HDCP_2_2_DEV_COUNT_LO(rxinfo[1]);
	if (!count || count > HDCP_2_2_MAX_DEVICE_COUNT)
		return -E2BIG;
	tx->device_count = count;
	tx->depth = HDCP_2_2_DEPTH(rxinfo[0]);
	tx->legacy_downstream = HDCP_2_2_HDCP1_DEVICE_CONNECTED(rxinfo[1]) ||
				HDCP_2_2_HDCP_2_0_REP_CONNECTED(rxinfo[1]);
	/* The first list of a session carries seq_num_V 0. */
	if (get_unaligned_be24(seq_num_v))
		return -EPROTO;

	ret = TX_READ(tx, DP_HDCP_2_2_REG_RECV_ID_LIST_OFFSET, tx->ids,
		      count * HDCP_2_2_RECEIVER_ID_LEN);
	if (ret)
		return ret;
	ret = tx_revocation(tx, tx->ids, count);
	if (ret)
		return ret;
	ret = TX_SEP(tx, sep_repeater_list)(tx->ctx, tx->channel, tx->ids, count,
					 rxinfo, seq_num_v, v_prime, v);
	if (ret)
		return ret;

	/* RepeaterAuth_Send_Ack: the least significant 128 bits of V. */
	ret = TX_WRITE(tx, DP_HDCP_2_2_REG_V_OFFSET, v + HDCP_2_2_V_PRIME_HALF_LEN,
		       HDCP_2_2_V_PRIME_HALF_LEN);
	if (!ret)
		tx_report(tx, DCP_HDCP_PROGRESS_LIST_VERIFIED, false);
	return ret;
}

/*
 * RepeaterAuth_Stream_Manage for the one stream, then Stream_Ready. A wrong
 * M' is retried once with the next seq_num_M, as the specification allows.
 */
static int tx_stream_manage(struct dcp_hdcp_tx *tx)
{
	u8 msg[HDCP_2_2_SEQ_NUM_LEN + 2 + sizeof(struct hdcp2_streamid_type)];
	u8 m_prime[HDCP_2_2_MPRIME_LEN];
	int attempt, ret = -EACCES;

	/* Type 1 must not reach an HDCP 1.x device or an HDCP 2.0 repeater. */
	if (tx->type && tx->legacy_downstream)
		return -EPERM;

	for (attempt = 0; attempt < 2 && ret == -EACCES; attempt++) {
		if (tx->seq_num_m > HDCP_2_2_SEQ_NUM_MAX)
			return -ERANGE;
		put_unaligned_be24(tx->seq_num_m, msg);
		put_unaligned_be16(1, msg + HDCP_2_2_SEQ_NUM_LEN);
		msg[HDCP_2_2_SEQ_NUM_LEN + 2] = 0;		/* stream ID */
		msg[HDCP_2_2_SEQ_NUM_LEN + 3] = tx->type;
		ret = TX_WRITE(tx, DP_HDCP_2_2_REG_SEQ_NUM_M_OFFSET, msg,
			       sizeof(msg));
		if (ret)
			return ret;
		tx->ops->sleep(tx->ctx, HDCP_2_2_STREAM_READY_TIMEOUT_MS);
		ret = TX_READ(tx, DP_HDCP_2_2_REG_MPRIME_OFFSET, m_prime,
			      sizeof(m_prime));
		if (ret)
			return ret;
		ret = TX_SEP(tx, sep_repeater_stream)(tx->ctx, tx->channel, 0,
						   tx->type, msg, m_prime);
		tx->seq_num_m++;
	}
	if (!ret)
		tx_report(tx, DCP_HDCP_PROGRESS_STREAM_READY, false);
	return ret;
}

int dcp_hdcp_authenticate(struct dcp_hdcp_tx *tx)
{
	bool stored = false;
	int ret;

	tx->step = NULL;
	if (tx->type > 1)
		return -EINVAL;
	tx->repeater = false;
	tx->legacy_downstream = false;
	tx->device_count = 0;
	tx->depth = 0;
	tx->seq_num_m = 0;
	tx_report(tx, DCP_HDCP_PROGRESS_AKE, false);

	ret = tx_ake(tx, &stored);
	if (ret)
		return ret;
	tx_report(tx, DCP_HDCP_PROGRESS_AKE_DONE, false);
	ret = tx_lc_ske(tx);
	if (ret)
		return ret;

	if (tx->repeater) {
		ret = tx_receiver_list(tx);
		if (!ret)
			ret = tx_stream_manage(tx);
	} else {
		ret = TX_WRITE(tx, DP_HDCP_2_2_REG_STREAM_TYPE_OFFSET, &tx->type,
			       sizeof(tx->type));
	}
	if (ret)
		return ret;

	/* Encryption no sooner than 200 ms after SKE_Send_Eks. */
	ret = TX_SEP(tx, sep_finish)(tx->ctx, tx->channel);
	if (ret)
		return ret;
	tx->ops->sleep(tx->ctx, HDCP_2_2_DELAY_BEFORE_ENCRYPTION_EN);
	tx->encrypting = true;
	ret = TX_SEP(tx, sep_set_encryption)(tx->ctx, tx->channel, true);
	if (ret)
		return ret;
	tx_report(tx, DCP_HDCP_PROGRESS_ENCRYPTED, true);
	return 0;
}

void dcp_hdcp_abort(struct dcp_hdcp_tx *tx)
{
	if (tx->encrypting)
		TX_SEP(tx, sep_set_encryption)(tx->ctx, tx->channel, false);
	tx->encrypting = false;
	if (tx->sep_open)
		TX_SEP(tx, sep_close)(tx->ctx, tx->channel);
	tx->sep_open = false;
}

int dcp_hdcp_check_link(struct dcp_hdcp_tx *tx)
{
	u8 status;
	int ret = TX_READ(tx, DP_HDCP_2_2_REG_RXSTATUS_OFFSET, &status, 1);

	if (ret)
		return ret;
	if (HDCP_2_2_DP_RXSTATUS_REAUTH_REQ(status) ||
	    HDCP_2_2_DP_RXSTATUS_LINK_FAILED(status))
		return -EAGAIN;
	return 0;
}

#if IS_ENABLED(CONFIG_DRM_APPLE_KUNIT_TEST)
#include "dcp-hdcp-tx-test.c"
#endif
