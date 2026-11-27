// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * The SEP HDCP 2.2 transmitter service for the display driver
 * (include/linux/apple-sep-hdcp.h). The SEP driver registers its steps here;
 * this file checks the arguments, flattens them in the order the SEP takes
 * them and unpacks the results. It is built in, so the display driver links
 * against it whether or not the SEP driver is loaded.
 */
#include <linux/apple-sep-hdcp.h>
#include <linux/errno.h>
#include <linux/export.h>
#include <linux/mutex.h>
#include <linux/rwsem.h>
#include <linux/slab.h>
#include <linux/string.h>

#include "shim.h"

static DECLARE_RWSEM(apple_sep_hdcp_registration_lock);
static DEFINE_MUTEX(apple_sep_hdcp_call_lock);
static struct sep_hdcp_ops apple_sep_hdcp_ops;
static void *apple_sep_hdcp_context;

int sep_hdcp_register(void *context, const struct sep_hdcp_ops *ops)
{
	int ret = 0;

	if (!context || !ops || !ops->available || !ops->step || !ops->close)
		return -EINVAL;

	down_write(&apple_sep_hdcp_registration_lock);
	if (apple_sep_hdcp_context) {
		ret = -EBUSY;
	} else {
		apple_sep_hdcp_ops = *ops;
		apple_sep_hdcp_context = context;
	}
	up_write(&apple_sep_hdcp_registration_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(sep_hdcp_register);

void sep_hdcp_unregister(void *context)
{
	down_write(&apple_sep_hdcp_registration_lock);
	if (apple_sep_hdcp_context == context) {
		apple_sep_hdcp_context = NULL;
		memzero_explicit(&apple_sep_hdcp_ops,
				 sizeof(apple_sep_hdcp_ops));
	}
	up_write(&apple_sep_hdcp_registration_lock);
}
EXPORT_SYMBOL_GPL(sep_hdcp_unregister);

static int apple_sep_hdcp_step(u32 channel, enum sep_hdcp_step step,
			       const u8 *in, size_t in_len, u8 *out,
			       size_t out_len)
{
	int ret;

	down_read(&apple_sep_hdcp_registration_lock);
	if (!apple_sep_hdcp_context) {
		ret = -ENODEV;
		goto out;
	}
	mutex_lock(&apple_sep_hdcp_call_lock);
	ret = apple_sep_hdcp_ops.step(apple_sep_hdcp_context, channel, step,
				      in, in_len, out, out_len);
	mutex_unlock(&apple_sep_hdcp_call_lock);
out:
	up_read(&apple_sep_hdcp_registration_lock);
	if (ret && out_len)
		memzero_explicit(out, out_len);
	return ret;
}

bool apple_sep_hdcp_available(void)
{
	bool ret = false;

	down_read(&apple_sep_hdcp_registration_lock);
	if (apple_sep_hdcp_context)
		ret = apple_sep_hdcp_ops.available(apple_sep_hdcp_context);
	up_read(&apple_sep_hdcp_registration_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(apple_sep_hdcp_available);

int apple_sep_hdcp_open(u32 channel, u8 rtx[APPLE_SEP_HDCP_RTX_LEN])
{
	if (!rtx)
		return -EINVAL;
	return apple_sep_hdcp_step(channel, SEP_HDCP_STEP_OPEN, NULL, 0, rtx,
				   APPLE_SEP_HDCP_RTX_LEN);
}
EXPORT_SYMBOL_GPL(apple_sep_hdcp_open);

#define AKE_CERT_IN	(APPLE_SEP_HDCP_CERT_LEN + APPLE_SEP_HDCP_RRX_LEN)
#define AKE_CERT_OUT	(1 + APPLE_SEP_HDCP_EKPUB_KM_LEN)

int apple_sep_hdcp_ake_cert(u32 channel,
			    const u8 cert[APPLE_SEP_HDCP_CERT_LEN],
			    const u8 rrx[APPLE_SEP_HDCP_RRX_LEN],
			    bool *stored, struct apple_sep_hdcp_stored_km *km,
			    u8 ekpub_km[APPLE_SEP_HDCP_EKPUB_KM_LEN])
{
	u8 *in, *out;
	int ret;

	if (!cert || !rrx || !stored || !km || !ekpub_km)
		return -EINVAL;
	*stored = false;
	memzero_explicit(km, sizeof(*km));
	memzero_explicit(ekpub_km, APPLE_SEP_HDCP_EKPUB_KM_LEN);

	in = kmalloc(AKE_CERT_IN + AKE_CERT_OUT, GFP_KERNEL);
	if (!in)
		return -ENOMEM;
	out = in + AKE_CERT_IN;
	memcpy(in, cert, APPLE_SEP_HDCP_CERT_LEN);
	memcpy(in + APPLE_SEP_HDCP_CERT_LEN, rrx, APPLE_SEP_HDCP_RRX_LEN);

	ret = apple_sep_hdcp_step(channel, SEP_HDCP_STEP_AKE_CERT, in,
				  AKE_CERT_IN, out, AKE_CERT_OUT);
	if (!ret) {
		if (out[0]) {
			*stored = true;
			memcpy(km->ekh_km, out + 1, APPLE_SEP_HDCP_EKH_KM_LEN);
			memcpy(km->m, out + 1 + APPLE_SEP_HDCP_EKH_KM_LEN,
			       APPLE_SEP_HDCP_M_LEN);
		} else {
			memcpy(ekpub_km, out + 1, APPLE_SEP_HDCP_EKPUB_KM_LEN);
		}
	}
	kfree_sensitive(in);
	return ret;
}
EXPORT_SYMBOL_GPL(apple_sep_hdcp_ake_cert);

int apple_sep_hdcp_ake_h_prime(u32 channel,
			       const u8 rxcaps[APPLE_SEP_HDCP_CAPS_LEN],
			       const u8 txcaps[APPLE_SEP_HDCP_CAPS_LEN],
			       const u8 h_prime[APPLE_SEP_HDCP_H_PRIME_LEN])
{
	u8 in[2 * APPLE_SEP_HDCP_CAPS_LEN + APPLE_SEP_HDCP_H_PRIME_LEN];
	int ret;

	if (!rxcaps || !txcaps || !h_prime)
		return -EINVAL;
	memcpy(in, rxcaps, APPLE_SEP_HDCP_CAPS_LEN);
	memcpy(in + APPLE_SEP_HDCP_CAPS_LEN, txcaps, APPLE_SEP_HDCP_CAPS_LEN);
	memcpy(in + 2 * APPLE_SEP_HDCP_CAPS_LEN, h_prime,
	       APPLE_SEP_HDCP_H_PRIME_LEN);
	ret = apple_sep_hdcp_step(channel, SEP_HDCP_STEP_H_PRIME, in,
				  sizeof(in), NULL, 0);
	memzero_explicit(in, sizeof(in));
	return ret;
}
EXPORT_SYMBOL_GPL(apple_sep_hdcp_ake_h_prime);

int apple_sep_hdcp_ake_pairing(u32 channel,
			       const u8 ekh_km[APPLE_SEP_HDCP_EKH_KM_LEN])
{
	if (!ekh_km)
		return -EINVAL;
	return apple_sep_hdcp_step(channel, SEP_HDCP_STEP_PAIRING, ekh_km,
				   APPLE_SEP_HDCP_EKH_KM_LEN, NULL, 0);
}
EXPORT_SYMBOL_GPL(apple_sep_hdcp_ake_pairing);

int apple_sep_hdcp_lc_init(u32 channel, u8 rn[APPLE_SEP_HDCP_RN_LEN])
{
	if (!rn)
		return -EINVAL;
	return apple_sep_hdcp_step(channel, SEP_HDCP_STEP_LC_INIT, NULL, 0, rn,
				   APPLE_SEP_HDCP_RN_LEN);
}
EXPORT_SYMBOL_GPL(apple_sep_hdcp_lc_init);

int apple_sep_hdcp_lc_l_prime(u32 channel,
			      const u8 l_prime[APPLE_SEP_HDCP_L_PRIME_LEN])
{
	if (!l_prime)
		return -EINVAL;
	return apple_sep_hdcp_step(channel, SEP_HDCP_STEP_L_PRIME, l_prime,
				   APPLE_SEP_HDCP_L_PRIME_LEN, NULL, 0);
}
EXPORT_SYMBOL_GPL(apple_sep_hdcp_lc_l_prime);

int apple_sep_hdcp_ske(u32 channel,
		       u8 edkey_ks[APPLE_SEP_HDCP_EDKEY_KS_LEN],
		       u8 riv[APPLE_SEP_HDCP_RIV_LEN])
{
	u8 out[APPLE_SEP_HDCP_EDKEY_KS_LEN + APPLE_SEP_HDCP_RIV_LEN];
	int ret;

	if (!edkey_ks || !riv)
		return -EINVAL;
	ret = apple_sep_hdcp_step(channel, SEP_HDCP_STEP_SKE, NULL, 0, out,
				  sizeof(out));
	memcpy(edkey_ks, out, APPLE_SEP_HDCP_EDKEY_KS_LEN);
	memcpy(riv, out + APPLE_SEP_HDCP_EDKEY_KS_LEN, APPLE_SEP_HDCP_RIV_LEN);
	memzero_explicit(out, sizeof(out));
	return ret;
}
EXPORT_SYMBOL_GPL(apple_sep_hdcp_ske);

#define LIST_IN_MAX (APPLE_SEP_HDCP_MAX_DEVICES * APPLE_SEP_HDCP_RECEIVER_ID_LEN + \
		     APPLE_SEP_HDCP_RXINFO_LEN + APPLE_SEP_HDCP_SEQ_NUM_LEN + \
		     APPLE_SEP_HDCP_V_PRIME_LEN)

int apple_sep_hdcp_repeater_list(u32 channel, const u8 *ids, unsigned int count,
				 const u8 rxinfo[APPLE_SEP_HDCP_RXINFO_LEN],
				 const u8 seq_num_v[APPLE_SEP_HDCP_SEQ_NUM_LEN],
				 const u8 v_prime[APPLE_SEP_HDCP_V_PRIME_LEN],
				 u8 v[APPLE_SEP_HDCP_V_LEN])
{
	u8 in[LIST_IN_MAX];
	size_t n;
	int ret;

	if (!v)
		return -EINVAL;
	memzero_explicit(v, APPLE_SEP_HDCP_V_LEN);
	if (!ids || !count || count > APPLE_SEP_HDCP_MAX_DEVICES || !rxinfo ||
	    !seq_num_v || !v_prime)
		return -EINVAL;

	/* The order V is computed over: the list, RxInfo, seq_num_V. */
	n = count * APPLE_SEP_HDCP_RECEIVER_ID_LEN;
	memcpy(in, ids, n);
	memcpy(in + n, rxinfo, APPLE_SEP_HDCP_RXINFO_LEN);
	n += APPLE_SEP_HDCP_RXINFO_LEN;
	memcpy(in + n, seq_num_v, APPLE_SEP_HDCP_SEQ_NUM_LEN);
	n += APPLE_SEP_HDCP_SEQ_NUM_LEN;
	memcpy(in + n, v_prime, APPLE_SEP_HDCP_V_PRIME_LEN);
	n += APPLE_SEP_HDCP_V_PRIME_LEN;
	ret = apple_sep_hdcp_step(channel, SEP_HDCP_STEP_RECEIVER_LIST, in, n,
				  v, APPLE_SEP_HDCP_V_LEN);
	memzero_explicit(in, sizeof(in));
	return ret;
}
EXPORT_SYMBOL_GPL(apple_sep_hdcp_repeater_list);

int apple_sep_hdcp_repeater_stream(u32 channel, u8 stream_id, u8 type,
				   const u8 seq_num_m[APPLE_SEP_HDCP_SEQ_NUM_LEN],
				   const u8 m_prime[APPLE_SEP_HDCP_M_PRIME_LEN])
{
	u8 in[2 + APPLE_SEP_HDCP_SEQ_NUM_LEN + APPLE_SEP_HDCP_M_PRIME_LEN];
	int ret;

	if (type > 1 || !seq_num_m || !m_prime)
		return -EINVAL;
	/* The order M is computed over: StreamID_Type, then seq_num_M. */
	in[0] = stream_id;
	in[1] = type;
	memcpy(in + 2, seq_num_m, APPLE_SEP_HDCP_SEQ_NUM_LEN);
	memcpy(in + 2 + APPLE_SEP_HDCP_SEQ_NUM_LEN, m_prime,
	       APPLE_SEP_HDCP_M_PRIME_LEN);
	ret = apple_sep_hdcp_step(channel, SEP_HDCP_STEP_STREAM, in,
				  sizeof(in), NULL, 0);
	memzero_explicit(in, sizeof(in));
	return ret;
}
EXPORT_SYMBOL_GPL(apple_sep_hdcp_repeater_stream);

int apple_sep_hdcp_finish(u32 channel)
{
	return apple_sep_hdcp_step(channel, SEP_HDCP_STEP_FINISH, NULL, 0,
				   NULL, 0);
}
EXPORT_SYMBOL_GPL(apple_sep_hdcp_finish);

int apple_sep_hdcp_set_encryption(u32 channel, bool on)
{
	return apple_sep_hdcp_step(channel,
				   on ? SEP_HDCP_STEP_ENCRYPTION_ON :
					SEP_HDCP_STEP_ENCRYPTION_OFF,
				   NULL, 0, NULL, 0);
}
EXPORT_SYMBOL_GPL(apple_sep_hdcp_set_encryption);

void apple_sep_hdcp_close(u32 channel)
{
	down_read(&apple_sep_hdcp_registration_lock);
	if (apple_sep_hdcp_context) {
		mutex_lock(&apple_sep_hdcp_call_lock);
		apple_sep_hdcp_ops.close(apple_sep_hdcp_context, channel);
		mutex_unlock(&apple_sep_hdcp_call_lock);
	}
	up_read(&apple_sep_hdcp_registration_lock);
}
EXPORT_SYMBOL_GPL(apple_sep_hdcp_close);

#if IS_BUILTIN(CONFIG_KUNIT) && IS_ENABLED(CONFIG_APPLE_SEP_KUNIT_TEST)
#include "hdcp_shim_test.c"
#endif
