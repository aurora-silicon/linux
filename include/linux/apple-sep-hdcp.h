/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/*
 * Apple SEP HDCP 2.2 transmitter service, for the display driver.
 *
 * The SEP holds the transmitter's device keys and does every HDCP 2.2
 * cryptographic step itself. The display driver builds the protocol
 * messages from the fields below and relays them to the receiver through
 * DCP. Only fields that the HDCP 2.2 protocol sends in the clear cross this
 * interface. No call returns or takes km, ks, kd or a device key.
 *
 * Every call runs on one HDCP interface channel of the SEP (the display
 * node's "apple,hdcp-channels" value). A session is opened with
 * apple_sep_hdcp_open() and ends with apple_sep_hdcp_close(). The SEP
 * driver enforces the protocol order per channel, so a call out of order
 * fails with -EPROTO before the SEP sees it. Bounded verification retries
 * permitted by HDCP 2.2 (locality check L' and repeater stream M') leave
 * the session open for retry while attempts remain; any other SEP error
 * leaves the session failed until it is closed. If the SEP does not answer,
 * every channel fails with -EIO until the SEP is reset.
 *
 * All calls may sleep. They return 0 or a negative errno: -ENODEV when no
 * SEP driver is bound or the endpoint is absent, -EAGAIN when the SEP is
 * still finishing bring-up or transiently gated for system sleep, -EINVAL
 * for a bad argument, -EPROTO for a call out of order, -EACCES when the
 * SEP rejects a verification (certificate, H', L', ReceiverID list, V', M'),
 * -EIO when the SEP fails or does not answer.
 */
#ifndef _LINUX_APPLE_SEP_HDCP_H
#define _LINUX_APPLE_SEP_HDCP_H

#include <linux/errno.h>
#include <linux/kconfig.h>
#include <linux/types.h>

#define APPLE_SEP_HDCP_RTX_LEN		8
#define APPLE_SEP_HDCP_RRX_LEN		8
#define APPLE_SEP_HDCP_CAPS_LEN		3
#define APPLE_SEP_HDCP_CERT_LEN		522
#define APPLE_SEP_HDCP_EKPUB_KM_LEN	128
#define APPLE_SEP_HDCP_EKH_KM_LEN	16
#define APPLE_SEP_HDCP_M_LEN		16
#define APPLE_SEP_HDCP_H_PRIME_LEN	32
#define APPLE_SEP_HDCP_RN_LEN		8
#define APPLE_SEP_HDCP_L_PRIME_LEN	32
#define APPLE_SEP_HDCP_EDKEY_KS_LEN	16
#define APPLE_SEP_HDCP_RIV_LEN		8
#define APPLE_SEP_HDCP_RXINFO_LEN	2
#define APPLE_SEP_HDCP_SEQ_NUM_LEN	3
#define APPLE_SEP_HDCP_RECEIVER_ID_LEN	5
#define APPLE_SEP_HDCP_MAX_DEVICES	31
#define APPLE_SEP_HDCP_V_PRIME_LEN	16
#define APPLE_SEP_HDCP_V_LEN		32
#define APPLE_SEP_HDCP_M_PRIME_LEN	32

/* The km the SEP has stored for this receiver, as AKE_Stored_km carries it. */
struct apple_sep_hdcp_stored_km {
	u8 ekh_km[APPLE_SEP_HDCP_EKH_KM_LEN];
	u8 m[APPLE_SEP_HDCP_M_LEN];
};

#if IS_ENABLED(CONFIG_APPLE_SEP_HDCP_INTERFACE)

/* Session start: the SEP draws r_tx for AKE_Init. */
int apple_sep_hdcp_open(u32 channel, u8 rtx[APPLE_SEP_HDCP_RTX_LEN]);

/*
 * AKE_Send_Cert and r_rx. The SEP checks the certificate signature.
 * On return *stored is true and @km holds AKE_Stored_km's body when the SEP
 * has paired with this receiver before; otherwise @ekpub_km holds
 * AKE_No_Stored_km's body.
 */
int apple_sep_hdcp_ake_cert(u32 channel,
			    const u8 cert[APPLE_SEP_HDCP_CERT_LEN],
			    const u8 rrx[APPLE_SEP_HDCP_RRX_LEN],
			    bool *stored, struct apple_sep_hdcp_stored_km *km,
			    u8 ekpub_km[APPLE_SEP_HDCP_EKPUB_KM_LEN]);

/* AKE_Send_H_prime: the SEP checks H' over r_tx, RxCaps and TxCaps. */
int apple_sep_hdcp_ake_h_prime(u32 channel,
			       const u8 rxcaps[APPLE_SEP_HDCP_CAPS_LEN],
			       const u8 txcaps[APPLE_SEP_HDCP_CAPS_LEN],
			       const u8 h_prime[APPLE_SEP_HDCP_H_PRIME_LEN]);

/* AKE_Send_Pairing_Info (first pairing only): the SEP stores km. */
int apple_sep_hdcp_ake_pairing(u32 channel,
			       const u8 ekh_km[APPLE_SEP_HDCP_EKH_KM_LEN]);

/* LC_Init: r_n from the SEP. */
int apple_sep_hdcp_lc_init(u32 channel, u8 rn[APPLE_SEP_HDCP_RN_LEN]);

/* LC_Send_L_prime: the SEP checks L'. */
int apple_sep_hdcp_lc_l_prime(u32 channel,
			      const u8 l_prime[APPLE_SEP_HDCP_L_PRIME_LEN]);

/* SKE_Send_Eks: the SEP returns Edkey(ks) and r_iv for the receiver. */
int apple_sep_hdcp_ske(u32 channel,
		       u8 edkey_ks[APPLE_SEP_HDCP_EDKEY_KS_LEN],
		       u8 riv[APPLE_SEP_HDCP_RIV_LEN]);

/*
 * RepeaterAuth_Send_ReceiverID_List: @ids holds @count Receiver IDs. The SEP
 * checks V' and returns V; the transmitter sends its least significant
 * 128 bits in RepeaterAuth_Send_Ack.
 */
int apple_sep_hdcp_repeater_list(u32 channel, const u8 *ids, unsigned int count,
				 const u8 rxinfo[APPLE_SEP_HDCP_RXINFO_LEN],
				 const u8 seq_num_v[APPLE_SEP_HDCP_SEQ_NUM_LEN],
				 const u8 v_prime[APPLE_SEP_HDCP_V_PRIME_LEN],
				 u8 v[APPLE_SEP_HDCP_V_LEN]);

/*
 * RepeaterAuth_Stream_Manage / Stream_Ready for one stream: the SEP checks
 * M' over StreamID_Type and seq_num_M. @type is 0 or 1.
 */
int apple_sep_hdcp_repeater_stream(u32 channel, u8 stream_id, u8 type,
				   const u8 seq_num_m[APPLE_SEP_HDCP_SEQ_NUM_LEN],
				   const u8 m_prime[APPLE_SEP_HDCP_M_PRIME_LEN]);

/* Authentication complete. The caller waits 200 ms before encryption. */
int apple_sep_hdcp_finish(u32 channel);

/* Link encryption on or off for this channel. */
int apple_sep_hdcp_set_encryption(u32 channel, bool on);

/* Ends the session from any state; safe to call when none is open. */
void apple_sep_hdcp_close(u32 channel);

/* True when a SEP driver with the HDCP service is bound and ready. */
bool apple_sep_hdcp_available(void);

#else /* !IS_ENABLED(CONFIG_APPLE_SEP_HDCP_INTERFACE) */

static inline int apple_sep_hdcp_open(u32 channel,
				      u8 rtx[APPLE_SEP_HDCP_RTX_LEN])
{
	return -ENODEV;
}

static inline int apple_sep_hdcp_ake_cert(u32 channel,
					  const u8 cert[APPLE_SEP_HDCP_CERT_LEN],
					  const u8 rrx[APPLE_SEP_HDCP_RRX_LEN],
					  bool *stored,
					  struct apple_sep_hdcp_stored_km *km,
					  u8 ekpub_km[APPLE_SEP_HDCP_EKPUB_KM_LEN])
{
	return -ENODEV;
}

static inline int apple_sep_hdcp_ake_h_prime(u32 channel,
					     const u8 rxcaps[APPLE_SEP_HDCP_CAPS_LEN],
					     const u8 txcaps[APPLE_SEP_HDCP_CAPS_LEN],
					     const u8 h_prime[APPLE_SEP_HDCP_H_PRIME_LEN])
{
	return -ENODEV;
}

static inline int apple_sep_hdcp_ake_pairing(u32 channel,
					     const u8 ekh_km[APPLE_SEP_HDCP_EKH_KM_LEN])
{
	return -ENODEV;
}

static inline int apple_sep_hdcp_lc_init(u32 channel,
					 u8 rn[APPLE_SEP_HDCP_RN_LEN])
{
	return -ENODEV;
}

static inline int apple_sep_hdcp_lc_l_prime(u32 channel,
					    const u8 l_prime[APPLE_SEP_HDCP_L_PRIME_LEN])
{
	return -ENODEV;
}

static inline int apple_sep_hdcp_ske(u32 channel,
				     u8 edkey_ks[APPLE_SEP_HDCP_EDKEY_KS_LEN],
				     u8 riv[APPLE_SEP_HDCP_RIV_LEN])
{
	return -ENODEV;
}

static inline int apple_sep_hdcp_repeater_list(u32 channel, const u8 *ids,
					       unsigned int count,
					       const u8 rxinfo[APPLE_SEP_HDCP_RXINFO_LEN],
					       const u8 seq_num_v[APPLE_SEP_HDCP_SEQ_NUM_LEN],
					       const u8 v_prime[APPLE_SEP_HDCP_V_PRIME_LEN],
					       u8 v[APPLE_SEP_HDCP_V_LEN])
{
	return -ENODEV;
}

static inline int apple_sep_hdcp_repeater_stream(u32 channel, u8 stream_id,
						 u8 type,
						 const u8 seq_num_m[APPLE_SEP_HDCP_SEQ_NUM_LEN],
						 const u8 m_prime[APPLE_SEP_HDCP_M_PRIME_LEN])
{
	return -ENODEV;
}

static inline int apple_sep_hdcp_finish(u32 channel)
{
	return -ENODEV;
}

static inline int apple_sep_hdcp_set_encryption(u32 channel, bool on)
{
	return -ENODEV;
}

static inline void apple_sep_hdcp_close(u32 channel)
{
}

static inline bool apple_sep_hdcp_available(void)
{
	return false;
}

#endif /* IS_ENABLED(CONFIG_APPLE_SEP_HDCP_INTERFACE) */

#endif /* _LINUX_APPLE_SEP_HDCP_H */
