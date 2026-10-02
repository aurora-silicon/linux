/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef MT7932_ASSOCIATION_H
#define MT7932_ASSOCIATION_H

#include "protocol.h"
#include "channels.h"

struct mt7932_peer_ids {
	u8 station[2], wtbl[4];
};

/* Firmware owns association IE generation. iwd always declares BSS transition
 * and FILS capability bits even for ordinary OPEN authentication. Accept only
 * those declarations (or zero subsets), not a FILS operation or arbitrary IE
 * carriage request. The generated capabilities are reported separately.
 */
static inline bool mt7932_open_ies(const u8 *ies, size_t length)
{
	unsigned int i;

	if (!length)
		return true;
	if (!ies || length != 12 || ies[0] != 127 || ies[1] != 10)
		return false;
	for (i = 0; i < 10; i++) {
		u8 allowed = i == 2 ? 8 : i == 9 ? 1 : 0;

		if (ies[i + 2] & ~allowed)
			return false;
	}
	return true;
}

/* Bounded, nonsecret subset of firmware-generated request IEs. Unknown/vendor
 * contents are not exported. These bytes are actual generated capabilities,
 * not an echo of the caller's capability declaration or replacement RF IEs.
 */
static inline int mt7932_assoc_capabilities(u8 *out, size_t capacity,
					 const u8 *ies, size_t length, bool open)
{
	size_t offset = 0, used = 0;
	unsigned int seen = 0;

	while (offset < length) {
		u8 id, size;
		unsigned int bit = 0, expected = 0;

		if (length - offset < 2)
			return -EINVAL;
		id = ies[offset];
		size = ies[offset + 1];
		if (size > length - offset - 2)
			return -EINVAL;
		if (open && (id == 48 || (id == 221 && size >= 4 &&
		    !memcmp(ies + offset + 2, "\x00\x50\xf2\x01", 4))))
			return -EINVAL;
		switch (id) {
		case 45: bit = 1; expected = 26; break;
		case 61: bit = 2; expected = 22; break;
		case 191: bit = 4; expected = 12; break;
		case 192: bit = 8; expected = 5; break;
		case 127: bit = 16; expected = size; break;
		case WLAN_EID_EXTENSION:
			if (!size)
				return -EINVAL;
			if (ies[offset + 2] != WLAN_EID_EXT_HE_CAPABILITY)
				break;
			/* Validate the complete standard HE layout, including optional
			 * wider MCS maps and PPE thresholds, before reporting the
			 * firmware's actual request. This does not advertise new local
			 * PHY modes or alter the native association/rate owner.
			 */
			if (!ieee80211_he_capa_size_ok(ies + offset + 3, size - 1))
				return -EINVAL;
			bit = 32;
			expected = size;
			break;
		}
		if (bit) {
			if ((seen & bit) || size != expected ||
			    (id == 127 && (!size || size > 16)) ||
			    size + 2U > capacity - used)
				return -EINVAL;
			memcpy(out + used, ies + offset, size + 2);
			used += size + 2;
			seen |= bit;
		}
		offset += size + 2;
	}
	return used;
}

/* Original25G83 pure WPA2 binary-key result: structured type6, not a string.
 * NEO_TYPE6_BINARY_PMK_NATIVE_CONTRACT: standard nl80211 PMK maps directly;
 * no passphrase API, key derivation, duplicate independent PMK or extra flags.
 */
static inline int mt7932_connect_binary_pmk(u8 *body, size_t capacity,
					 const u8 *pmk, size_t length)
{
	if (capacity < 856 || length != 32)
		return -EINVAL;
	memset(body + 0x40, 0, 0x90);
	put_unaligned_le32(32, body + 0x40);
	put_unaligned_le32(6, body + 0x44);
	memcpy(body + 0x4c, pmk, 32);
	/* Clear length, all64 independent storage bytes and explicit allow-auth.
	 * Policy0 is the coherent original64-hex-input RESULT, not open security.
	 */
	memset(body + 0x1c8, 0, 4 + 64 + 4);
	return 0;
}

/* Explicit OPEN or PMK-only WPA2/CCMP; never an automatic security fallback.
 * Caller owns identities/resources and validated BSS state.
 * NEO_WPA2_FULLMAC_ASSOCIATION_AND_ETHERNET_CONTRACT. Never log this body.
 */
static inline int mt7932_connect_body(u8 *body, size_t capacity, const u8 *ssid,
				      size_t ssid_length, const u8 pmk[32], const u8 bssid[6],
				      u8 channel, unsigned int listen_interval, u8 connection_flags,
				      const struct mt7932_peer_ids *ids, bool open)
{
	u8 resources[6];
	unsigned int i, j;
	int ret;

	if ((open ? pmk != NULL : pmk == NULL) ||
	    capacity < 856 || !ssid_length || ssid_length > 32 ||
	    !mt7932_channel_band(channel) || connection_flags > 63 || (bssid[0] & 1))
		return -EINVAL;
	memcpy(resources, ids->station, 2);
	memcpy(resources + 2, ids->wtbl, 4);
	for (i = 0; i < 6; i++) {
		/* Global0 and per-interface BMC19 remain separately reserved. */
		if (!resources[i] || resources[i] >= (i < 2 ? 15 : 14))
			return -EINVAL;
		for (j = 0; j < i; j++)
			if (resources[i] == resources[j])
				return -EINVAL;
	}
	memset(body, 0, 856);
	/* NORMAL25G83 original strict keyless OPEN branch, without optional
	 * automatic-authentication policy. Keep this coherent version/auth/
	 * policy tuple together; no dummy keys or stale security state.
	 * NEO_NORMAL_STRICT_OPEN_PRODUCER_CONTRACT.
	 */
	if (open) {
		body[1] = 1;
		put_unaligned_le32(0x78, body + 8);
		put_unaligned_le16(1, body + 0x12);
	} else {
		body[1] = 4;
		put_unaligned_le32(0x10, body + 4);
		put_unaligned_le32(0x78, body + 8);
		put_unaligned_le16(1, body + 0x12);
		put_unaligned_le16(8, body + 0x14);
	}
	put_unaligned_le32(ssid_length, body + 0x18);
	memcpy(body + 0x1c, ssid, ssid_length);
	/* WCLJoinRequest projects six flags here, NOT an environmental count. */
	body[0x1c0] = connection_flags;
	/* Fresh HOST AIS media state1 selects CCMP type5 here. This is not
	 * structured-key type0x44 or the association event's RSN suite encoding.
	 * This builder is only used for the first, unassociated attempt.
	 */
	put_unaligned_le32(open ? 0 : 5, body + 0x220);
	put_unaligned_le16(listen_interval > 20 ? 20 : listen_interval, body + 0x228);
	memcpy(body + 0x32c, bssid, 6);
	body[0x338] = mt7932_channel_band(channel);
	body[0x339] = channel;
	body[0x33c] = 1;
	body[0x33d] = 2;
	body[0x33e] = 4;
	memcpy(body + 0x340, ids->station, 2);
	memcpy(body + 0x348, ids->wtbl, 4);
	body[0x354] = 1;
	ret = open ? 0 : mt7932_connect_binary_pmk(body, capacity, pmk, 32);
	return ret ? ret : 856;
}

static inline void mt7932_peer_queue(u8 body[32], u8 station, bool bind)
{
	static const u8 tids[] = {1,0,0,1,2,2,3,3};

	memset(body, 0, 32);
	put_unaligned_le16(15, body);
	body[2] = 5;
	body[14] = 1;
	body[15] = station;
	put_unaligned_le16(bind ? 15 : 2, body + 16);
	memcpy(body + 20, tids, sizeof(tids));
}

static inline bool mt7932_assoc_resources(const u8 *body, size_t length,
					 const struct mt7932_peer_ids *ids, u8 channel, bool open)
{
	unsigned int i;

	if (length < 52 || get_unaligned_le16(body) || body[16] ||
	    body[17] != ids->wtbl[0] || body[20] != ids->station[0] ||
	    !mt7932_channel_band(channel) || body[11] != mt7932_channel_band(channel) || body[12] != channel ||
	    (!open && get_unaligned_le32(body + 48) != 0x000fac04))
		return false;
	for (i = 0; i < 4; i++)
		if (body[21] == ids->wtbl[i])
			return true;
	return false;
}

#endif
