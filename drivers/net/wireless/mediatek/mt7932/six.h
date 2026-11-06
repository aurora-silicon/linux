/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef MT7932_SIX_H
#define MT7932_SIX_H

#include <linux/bitfield.h>
#include <net/cfg80211.h>

#include "channels.h"

/* Firmware class numbering, distinct from the J7RP archive class IDs. */
enum mt7932_6g_class {
	MT7932_6G_SP,
	MT7932_6G_LPI,
	MT7932_6G_VLP,
};

struct mt7932_6g_ap {
	u8 mode, primary, center, bandwidth;
	u8 count;
	s8 limit[4]; /* half dBm EIRP, 0x7f means no applicable limit */
};

static inline int mt7932_6g_class(u8 regulatory, u32 flags)
{
	if (flags & (IEEE80211_CHAN_DISABLED | IEEE80211_CHAN_NO_IR |
		     IEEE80211_CHAN_NO_HE | IEEE80211_CHAN_NO_20MHZ |
		     IEEE80211_CHAN_RADAR))
		return -EACCES;
	switch (regulatory) {
	case 0:
		return MT7932_6G_LPI;
	case 1:
		/* SP requires AFC; there is no SP-to-LPI fallback. */
		return -EOPNOTSUPP;
	case 2:
		return flags & IEEE80211_CHAN_NO_6GHZ_VLP_CLIENT ?
			       -EACCES :
			       MT7932_6G_VLP;
	default:
		/* The static mapping's catch-all is not class authorization. */
		return -EOPNOTSUPP;
	}
}

/* A conservative reduction across every advertised 20 MHz PSD value also
 * bounds each narrower allocation, without assuming per-subchannel ordering.
 * floor(2 * 10log10(BW_MHz)) always rounds toward the restrictive limit.
 */
static inline int mt7932_6g_tpe(struct mt7932_6g_ap *ap, const u8 *data,
				u8 length)
{
	static const u8 psd_gain[] = { 26, 32, 38, 44 };
	unsigned int count, interpretation, category, n, i;
	int psd = 127;

	if (!ieee80211_valid_tpe_element(data, length))
		return -EINVAL;
	count = FIELD_GET(IEEE80211_TX_PWR_ENV_INFO_COUNT, data[0]);
	interpretation =
		FIELD_GET(IEEE80211_TX_PWR_ENV_INFO_INTERPRET, data[0]);
	category = FIELD_GET(IEEE80211_TX_PWR_ENV_INFO_CATEGORY, data[0]);
	/* A station is a default client, never a subordinate AP. */
	if (category != IEEE80211_TPE_CAT_6GHZ_DEFAULT)
		return 0;
	if (interpretation == IEEE80211_TPE_LOCAL_EIRP ||
	    interpretation == IEEE80211_TPE_REG_CLIENT_EIRP) {
		n = min(count + 1, 4U);
		for (i = 0; i < n; i++)
			ap->limit[i] = min_t(s8, ap->limit[i], data[i + 1]);
		ap->count = max_t(u8, ap->count, n);
		return 0;
	}
	if (interpretation != IEEE80211_TPE_LOCAL_EIRP_PSD &&
	    interpretation != IEEE80211_TPE_REG_CLIENT_EIRP_PSD)
		return -EOPNOTSUPP;
	n = count ? BIT(count - 1) : 1;
	/* Extended/320 MHz PSD layouts are outside the established binding. */
	if (count > 4 || length != n + 1)
		return -EOPNOTSUPP;
	for (i = 0; i < n; i++) {
		/* A PSD byte of -128 forbids that 20 MHz subchannel. There
		 * is no supported puncturing path that could omit it here.
		 */
		if (data[i + 1] == 0x80)
			return -EACCES;
		psd = min(psd, (int)(s8)data[i + 1]);
	}
	for (i = 0; i < 4; i++) {
		int limit = psd == 127 ? 127 : min(psd + psd_gain[i], 126);

		ap->limit[i] = min_t(int, ap->limit[i], limit);
	}
	ap->count = 4;
	return 0;
}

/* Full TLV validation prevents a truncated or duplicate HE element from
 * becoming authority for LPI. TPE never selects the class.
 */
static inline int mt7932_6g_ap_parse(struct mt7932_6g_ap *ap, const u8 *ies,
				     size_t length, u8 primary, u32 flags)
{
	const struct element *elem;
	bool operation = false;
	size_t parsed = 0;
	int mode, ret;

	memset(ap, 0, sizeof(*ap));
	memset(ap->limit, 127, sizeof(ap->limit));
	for_each_element(elem, ies, length) {
		parsed += elem->datalen + 2;
		if (elem->id == WLAN_EID_TX_POWER_ENVELOPE) {
			ret = mt7932_6g_tpe(ap, elem->data, elem->datalen);
			if (ret)
				return ret;
		}
		if (elem->id == WLAN_EID_EXTENSION && elem->datalen &&
		    elem->data[0] == WLAN_EID_EXT_HE_OPERATION) {
			const struct ieee80211_he_operation *he;
			const struct ieee80211_he_6ghz_oper *six;
			u8 width, center, regulatory;

			if (operation || elem->datalen < 1 + sizeof(*he) ||
			    elem->datalen !=
				    ieee80211_he_oper_size(elem->data + 1))
				return -EINVAL;
			he = (const void *)(elem->data + 1);
			six = ieee80211_he_6ghz_oper(he);
			if (!six || six->primary != primary ||
			    !mt7932_channel_band(NL80211_BAND_6GHZ, primary))
				return -EINVAL;
			regulatory =
				FIELD_GET(IEEE80211_HE_6GHZ_OPER_CTRL_REG_INFO,
					  six->control);
			mode = mt7932_6g_class(regulatory, flags);
			if (mode < 0)
				return mode;
			width = FIELD_GET(IEEE80211_HE_6GHZ_OPER_CTRL_CHANWIDTH,
					  six->control);
			if (width > 2 || six->ccfs1)
				return -EOPNOTSUPP;
			center = width ? mt7932_channel_6g_center(primary,
								  width == 2) :
					 primary;
			if (!center || six->ccfs0 != center)
				return -EINVAL;
			ap->mode = mode;
			ap->primary = primary;
			ap->center = center;
			ap->bandwidth = width;
			operation = true;
		}
	}
	if (parsed != length || !operation || !ap->count)
		return -EINVAL;
	/* Every bandwidth we may use needs an advertised bound. */
	return ap->count > ap->bandwidth ? 0 : -EOPNOTSUPP;
}

/* Return 1 for the existing v2 VLP association sequence. Its admission and
 * commands never depended on TPE, including finite TPE or AP-wide 160 MHz.
 * LPI and newly available VLP channels use the strict class/TPE path.
 */
static inline int mt7932_6g_connect_ap(struct mt7932_6g_ap *ap, const u8 *ies,
				       size_t length, u8 primary, u32 flags,
				       u64 legacy_channels)
{
	const struct element *elem;
	const struct ieee80211_he_operation *he;
	const struct ieee80211_he_6ghz_oper *six;
	u8 regulatory;

	elem = cfg80211_find_ext_elem(WLAN_EID_EXT_HE_OPERATION, ies, length);
	if (!elem || elem->datalen < 1 + sizeof(*he) ||
	    elem->datalen != ieee80211_he_oper_size(elem->data + 1))
		return -EINVAL;
	he = (const void *)(elem->data + 1);
	six = ieee80211_he_6ghz_oper(he);
	if (!six || six->primary != primary ||
	    !mt7932_channel_band(NL80211_BAND_6GHZ, primary))
		return -EINVAL;
	regulatory = FIELD_GET(IEEE80211_HE_6GHZ_OPER_CTRL_REG_INFO, six->control);
	if (regulatory == 2 &&
	    (legacy_channels & BIT_ULL((primary - 1) / 4)))
		return 1;
	return mt7932_6g_ap_parse(ap, ies, length, primary, flags);
}

/* Immutable J700 board properties, in signed half-dB ticks, from the
 * t-3573 antenna-gain SPEC sections 1 and 3. The probe is J700-only.
 * The bandwidth map deliberately differs from a frequency-only lookup.
 */
static inline int mt7932_j700_6g_gain(u8 primary, unsigned int bw, s8 gain[3])
{
	static const s8 groups[6][3] = {
		{ 16, 16, 10 }, { 14, 14, 8 },	{ 15, 16, 10 },
		{ 15, 15, 9 },	{ 16, 16, 10 }, { 15, 16, 10 },
	};
	unsigned int group;

	if (bw > 3 || (primary != 2 &&
		       (primary < 1 || primary > 233 || (primary - 1) % 4)))
		return -EINVAL;
	if ((primary == 2 && bw) || (primary == 233 && bw) ||
	    (primary >= 225 && bw >= 2))
		return -EOPNOTSUPP;
	if (primary == 2 || primary <= 29)
		group = 0;
	else if (primary <= 61)
		group = 1;
	else if (primary <= 93)
		group = 2;
	else if (primary <= 109)
		group = 3;
	else if (primary == 113)
		group = bw == 2 ? 4 : 3;
	else if (primary == 117)
		group = bw == 1 || bw == 3 ? 3 : 4;
	else if (primary <= 125)
		group = bw == 3 ? 3 : 4;
	else if (primary <= 181)
		group = 4;
	else if (primary == 185)
		group = bw == 1 ? 5 : 4;
	else if (primary == 189)
		group = bw < 2 ? 5 : 4;
	else
		group = 5;
	memcpy(gain, groups[group], 3);
	return 0;
}

static inline int mt7932_j700_6g_gains(const struct mt7932_6g_ap *ap,
				       s8 gain[3][4])
{
	unsigned int bw, mode;

	memset(gain, 0, 12);
	for (bw = 0; bw < ap->count && bw < 4; bw++) {
		s8 values[3];
		int ret;

		if (ap->limit[bw] == 127)
			continue;
		ret = mt7932_j700_6g_gain(ap->primary, bw, values);
		if (ret)
			return ret;
		for (mode = 0; mode < 3; mode++)
			gain[mode][bw] = values[mode];
	}
	return 0;
}

/* Gains use the same half-dB units as the AP limit. A NULL gain source is
 * usable only for unconstrained limits: zero gain must never be guessed.
 */
static inline int mt7932_6g_tpe_body(u8 *body, size_t capacity,
				     const struct mt7932_6g_ap *ap,
				     const s8 gain[3][4])
{
	unsigned int group, i;

	if (capacity < 60 || !ap->count || ap->count > 4 ||
	    (ap->mode != MT7932_6G_LPI && ap->mode != MT7932_6G_VLP) ||
	    !mt7932_channel_band(NL80211_BAND_6GHZ, ap->primary))
		return -EINVAL;
	for (i = 0; i < ap->count; i++)
		if (ap->limit[i] != 127 && !gain)
			return -EOPNOTSUPP;
	memset(body, 0, 60);
	body[1] = 2;
	put_unaligned_le16(60, body + 2);
	body[4] = 1;
	body[5] = 3;
	body[6] = 1;
	body[7] = 3;
	body[44] = 1;
	body[45] = 3;
	body[46] = ap->primary;
	body[47] = ap->count;
	memset(body + 48, 127, 12);
	for (group = 0; group < 3; group++) {
		for (i = 0; i < ap->count; i++) {
			int limit;

			if (ap->limit[i] == 127)
				continue;
			limit = ap->limit[i] - gain[group][i];
			/* Saturating upward at -128 would relax an unrepresentable cap. */
			if (limit < -128 || limit > 126)
				return -ERANGE;
			body[48 + group * 4 + i] = limit;
		}
	}
	return 60;
}

#endif
