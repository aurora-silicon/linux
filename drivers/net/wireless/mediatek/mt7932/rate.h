/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef MT7932_RATE_H
#define MT7932_RATE_H

/* Qualified J700 group3 RXV subset. Reject unqualified modes/STBC instead
 * of manufacturing a rate from another MediaTek device's encoding.
 */
static inline bool mt7932_rx_rate(u32 rxv, u8 max_nss, struct rate_info *rate)
{
	u8 mcs = rxv & 127, nss = ((rxv >> 7) & 7) + 1;
	u8 width = (rxv >> 12) & 7, gi = (rxv >> 15) & 3;
	u8 stbc = (rxv >> 22) & 3, mode = (rxv >> 24) & 15;
	bool dcm = !!(rxv & (1U << 17));
	static const u16 cck[] = {10, 20, 55, 110, 0, 20, 55, 110};
	static const u16 ofdm[] = {480, 240, 120, 60, 540, 360, 180, 90};

	memset(rate, 0, sizeof(*rate));
	if (!max_nss || max_nss > 2)
		return false;
	if (!mode) {
		if (mcs > 7 || !cck[mcs])
			return false;
		rate->legacy = cck[mcs];
		return true;
	}
	if (mode == 1) {
		if (mcs < 8 || mcs > 15)
			return false;
		rate->legacy = ofdm[mcs - 8];
		return true;
	}
	if (nss > max_nss || stbc || width > 3)
		return false;
	switch (mode) {
	case 2:
	case 3:
		if (width > 1 || gi > 1 || mcs > 15 || nss != mcs / 8 + 1)
			return false;
		rate->flags = RATE_INFO_FLAGS_MCS;
		break;
	case 4:
		if (gi > 1 || mcs > 9)
			return false;
		rate->flags = RATE_INFO_FLAGS_VHT_MCS;
		break;
	case 8:
		if (mcs > 11 || gi > 2 || dcm)
			return false;
		rate->flags = RATE_INFO_FLAGS_HE_MCS;
		rate->he_gi = gi; /* nl80211 HE GI0/1/2 = 0.8/1.6/3.2us */
		break;
	default:
		return false; /* HE ER/TRIG/MU need separately qualified allocation metadata. */
	}
	if (mode != 8 && gi)
		rate->flags |= RATE_INFO_FLAGS_SHORT_GI;
	rate->mcs = mcs;
	rate->nss = nss;
	switch (width) {
	case 0: rate->bw = RATE_INFO_BW_20; break;
	case 1: rate->bw = RATE_INFO_BW_40; break;
	case 2: rate->bw = RATE_INFO_BW_80; break;
	case 3: rate->bw = RATE_INFO_BW_160; break;
	}
	return true;
}

/* Stock CID85 reply: exact version1 body, owned BSS/MAC and pending sequence.
 * Byte8's station identity and byte0a remain opaque; no invented WTBL test.
 */
static inline bool mt7932_tx_rate_reply(const struct mt7932_event *event,
				       u8 sequence, const u8 *peer, u8 max_nss,
				       struct rate_info *rate)
{
	const u8 *body;
	u32 a, b, c, rxv;

	if (!mt7932_event_matches(event, sequence) || event->eid != 0x21 ||
	    event->length != 344)
		return false;
	body = event->packet + 36;
	if (body[0] != 1 ||
	    !(get_unaligned_le32(body + 4) & 1) || body[9] ||
	    memcmp(body + 12, peer, 6))
		return false;
	a = get_unaligned_le32(body + 0x9c);
	b = get_unaligned_le32(body + 0xa0);
	c = get_unaligned_le32(body + 0xa4);
	if (((a >> 12) & 15) >= 5)
		return false; /* TX HE GI/DCM/allocation fields not qualified by this reply. */
	/* Normalize the independently qualified TX words to the shared fields. */
	rxv = (c & 127) | (((c >> 8) & 7) << 7) |
		(((a >> 8) & 7) << 12) | (((b >> 26) & 3) << 15) |
		(((a >> 6) & 3) << 22) | (((a >> 12) & 15) << 24);
	return mt7932_rx_rate(rxv, max_nss, rate);
}
#endif
