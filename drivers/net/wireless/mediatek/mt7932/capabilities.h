/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef MT7932_CAPABILITIES_H
#define MT7932_CAPABILITIES_H
#include "bandwidth.h"

/* Stock25G83 native IE producers, qualified with J700's two-stream PHY
 * response. SGI, STBC and beamforming remain firmware-owned. A different
 * profile needs its own accounting, not guessed PHY-TLV fields.
 */
static inline bool mt7932_phy_profile(const u8 phy[12])
{
	static const u8 qualified[] = {1,1,1,3,2,1,1,1,1,1,15,1};

	return !memcmp(phy, qualified, sizeof(qualified));
}

static inline void mt7932_band_capabilities(struct ieee80211_supported_band *band,
					  bool band5)
{
	struct ieee80211_sta_ht_cap *ht = &band->ht_cap;
	struct ieee80211_sta_vht_cap *vht = &band->vht_cap;
	u16 map = 0xffff;
	unsigned int i;

	memset(ht, 0, sizeof(*ht));
	memset(vht, 0, sizeof(*vht));
	ht->ht_supported = true;
	ht->cap = IEEE80211_HT_CAP_LDPC_CODING | IEEE80211_HT_CAP_SM_PS |
		IEEE80211_HT_CAP_SGI_20 | IEEE80211_HT_CAP_TX_STBC |
		(1 << IEEE80211_HT_CAP_RX_STBC_SHIFT) | IEEE80211_HT_CAP_MAX_AMSDU;
	if (!band5)
		ht->cap |= IEEE80211_HT_CAP_40MHZ_INTOLERANT;
	ht->ampdu_factor = IEEE80211_HT_MAX_AMPDU_64K;
	ht->ampdu_density = 0;
	ht->mcs.rx_mask[0] = ht->mcs.rx_mask[1] = 0xff;
	ht->mcs.tx_params = IEEE80211_HT_MCS_TX_DEFINED;
	if (!band5)
		return;
	/* NORMAL initializes the shared HT word with DSSS/CCK40 even on
	 * 5 GHz, clearing it together with width/SGI40 for a narrow peer.
	 * Account for that generated bit; legacy 5 GHz rates stay OFDM-only.
	 */
	if (MT7932_MAX_5G_BW)
		ht->cap |= IEEE80211_HT_CAP_SUP_WIDTH_20_40 | IEEE80211_HT_CAP_SGI_40 |
			IEEE80211_HT_CAP_DSSSCCK40;
	vht->vht_supported = true;
	vht->cap = IEEE80211_VHT_CAP_MAX_MPDU_LENGTH_11454 |
		IEEE80211_VHT_CAP_RXLDPC | IEEE80211_VHT_CAP_TXSTBC |
		IEEE80211_VHT_CAP_RXSTBC_1 | IEEE80211_VHT_CAP_SU_BEAMFORMEE_CAPABLE |
		IEEE80211_VHT_CAP_MU_BEAMFORMEE_CAPABLE |
		(3 << IEEE80211_VHT_CAP_BEAMFORMEE_STS_SHIFT) |
		(7 << IEEE80211_VHT_CAP_MAX_A_MPDU_LENGTH_EXPONENT_SHIFT);
	for (i = 0; i < 2; i++) {
		map &= ~(3 << (2 * i));
		map |= IEEE80211_VHT_MCS_SUPPORT_0_9 << (2 * i);
	}
	vht->vht_mcs.rx_mcs_map = cpu_to_le16(map);
	vht->vht_mcs.tx_mcs_map = cpu_to_le16(map);
	/* Generated EE5e has this standard capability bit, not a nonzero
	 * highest-rate value. EXT_NSS_BW bits in cap remain0; no extra widths.
	 */
	vht->vht_mcs.tx_highest = cpu_to_le16(IEEE80211_VHT_EXT_NSS_BW_CAPABLE);
	if (MT7932_MAX_5G_BW >= 2)
		vht->cap |= IEEE80211_VHT_CAP_SHORT_GI_80;
	/* VHT width bits0 do not mean20MHz: channel flags, domain policy and
	 * negotiated-width checks remain mandatory. Aggregate maxima are radio
	 * limits, not host DMA record sizes; never loosen RX bounds to match.
	 */
}

/* Inspect unmodified generated request IEs. Absence permits legacy peers;
 * presence must match the registered profile. The metadata exporter separately
 * bounds and rejects duplicate capability IEs; repeat these checks here so
 * this predicate is safe on any bounded IE list.
 */
static inline bool mt7932_assoc_phy_valid(const struct ieee80211_supported_band *band,
					const u8 *ies, size_t length)
{
	size_t offset = 0;
	bool ht_seen = false, vht_seen = false;

	while (offset < length) {
		const u8 *body;
		u8 id, size;

		if (length - offset < 2)
			return false;
		id = ies[offset];
		size = ies[offset + 1];
		if (size > length - offset - 2)
			return false;
		body = ies + offset + 2;
		if (id == WLAN_EID_HT_CAPABILITY) {
			const struct ieee80211_sta_ht_cap *ht = &band->ht_cap;
			u16 expected = ht->cap;

			if (ht_seen || size != 26 || !ht->ht_supported)
				return false;
			/* Native generated IEs may narrow to a20MHz peer. This is
			 * not permission to add a capability absent from the wiphy.
			 */
			if (!(get_unaligned_le16(body) & IEEE80211_HT_CAP_SUP_WIDTH_20_40))
				expected &= ~(IEEE80211_HT_CAP_SUP_WIDTH_20_40 | IEEE80211_HT_CAP_SGI_40 |
					      IEEE80211_HT_CAP_DSSSCCK40);
			if (get_unaligned_le16(body) != expected ||
			    body[2] != (ht->ampdu_factor | (ht->ampdu_density << 2)) ||
			    memcmp(body + 3, &ht->mcs, sizeof(ht->mcs)))
				return false;
			ht_seen = true;
		} else if (id == WLAN_EID_VHT_CAPABILITY) {
			const struct ieee80211_sta_vht_cap *vht = &band->vht_cap;
			u32 cap, sts;

			if (vht_seen || size != 12 || !vht->vht_supported)
				return false;
			cap = get_unaligned_le32(body);
			sts = (cap & IEEE80211_VHT_CAP_BEAMFORMEE_STS_MASK) >>
				IEEE80211_VHT_CAP_BEAMFORMEE_STS_SHIFT;
			/* Native peer-specific reduction3->2 is qualified. It is not
			 * a payload NSS count or permission for arbitrary reductions.
			 */
			if ((sts != 2 && sts != 3) ||
			    ((cap ^ vht->cap) & ~(IEEE80211_VHT_CAP_BEAMFORMEE_STS_MASK |
						 IEEE80211_VHT_CAP_SHORT_GI_80)) ||
			    ((cap & IEEE80211_VHT_CAP_SHORT_GI_80) &&
			     !(vht->cap & IEEE80211_VHT_CAP_SHORT_GI_80)) ||
			    memcmp(body + 4, &vht->vht_mcs, sizeof(vht->vht_mcs)))
				return false;
			vht_seen = true;
		}
		offset += size + 2;
	}
	return !vht_seen || ht_seen;
}

#endif
