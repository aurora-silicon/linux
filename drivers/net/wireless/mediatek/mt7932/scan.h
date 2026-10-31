/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef MT7932_SCAN_H
#define MT7932_SCAN_H

#include "protocol.h"
#include "channels.h"

/* CID03 carries this many band/channel slots, from 0xa0 up to 0xe0. */
#define MT7932_SCAN_CHANNELS 32

/* cfg80211 owns PSC selection and RNR expansion for split scans. Until the
 * firmware target list can carry its BSSID/short-SSID constraints, listen on
 * those channels even when the request also contains an ordinary SSID.
 */
static inline bool
mt7932_scan_listen(const struct cfg80211_scan_request *request,
		   const struct ieee80211_channel *channel, u64 legacy_channels)
{
	unsigned int i;

	if (!request->n_ssids ||
	    (channel->flags & (IEEE80211_CHAN_NO_IR | IEEE80211_CHAN_RADAR)))
		return true;
	if (channel->band != NL80211_BAND_6GHZ)
		return false;
	if ((request->flags & NL80211_SCAN_FLAG_COLOCATED_6GHZ) ||
	    request->n_6ghz_params ||
	    !mt7932_channel_band(channel->band, channel->hw_value) ||
	    !(legacy_channels & BIT_ULL((channel->hw_value - 1) / 4)))
		return true;
	for (i = 0; i < request->n_ssids; i++)
		if (request->ssids[i].ssid_len)
			return false;
	return true;
}

/* Selected ordinary TX17/fullmac wildcard scan, not a pre-RF gate.
 * Caller must own BSS0 and filter cfg80211 DISABLED channels first. NO_IR
 * channels require mt7932_scan_passive() before submission. The body uses
 * a 20 ms dwell and fills the MT7932-specific tail.
 */
static inline int mt7932_scan_body(u8 *body, size_t capacity, u8 seq, enum nl80211_band band,
				   const u8 *channels, unsigned int count)
{
	static const u8 ies[] = {0x7f,0x0a,1,0,8,0x8c,1,0x40,0,0,0,0,
				0x6b,7,0x0f,0xff,0xff,0xff,0xff,0xff,0xff};
	unsigned int i, j;

	if (capacity < 0x4d4 || !seq || seq > 127 || !count || count > MT7932_SCAN_CHANNELS)
		return -EINVAL;
	for (i = 0; i < count; i++) {
		if (!mt7932_channel_band(band, channels[i]))
			return -EINVAL;
		for (j = 0; j < i; j++)
			if (channels[i] == channels[j])
				return -EINVAL;
	}
	memset(body, 0, 0x4d4);
	body[0] = seq;
	body[2] = 1;
	body[3] = 1;
	body[6] = 0x12;
	body[7] = 1;
	put_unaligned_le16(20, body + 0x9a);
	body[0x9e] = 4;
	body[0x9f] = count;
	for (i = 0; i < count; i++) {
		body[0xa0 + 2 * i] = mt7932_channel_band(band, channels[i]);
		body[0xa1 + 2 * i] = channels[i];
	}
	put_unaligned_le16(sizeof(ies), body + 0xe0);
	memcpy(body + 0xe2, ies, sizeof(ies));
	put_unaligned_le16(20, body + 0x33c);
	memset(body + 0x458, 0xff, 6);
	put_unaligned_le32(0x1800, body + 0x4b8);
	put_unaligned_le32(45, body + 0x4bc);
	put_unaligned_le16(110, body + 0x4c2);
	put_unaligned_le16(110, body + 0x4ca);
	return 0x4d4;
}

/* CID03's type selects passive operation; probe-count zero alone does not.
 * Retain the source-recorded 40ms normal/minimum and 110ms passive/budget
 * inputs. Firmware selects a 97ms passive dwell after its 13ms reserve;
 * normal/minimum are not the passive timer. Do not request a random address.
 * This still requires normal
 * calibrated RF readiness and is not a global transmitter-disable command.
 */
static inline void mt7932_scan_passive(u8 body[0x4d4])
{
	body[2] = 0;
	body[3] = 0;
	body[4] = 0;
	body[5] = 0;
	memset(body + 8, 0, 4 * 36);
	put_unaligned_le16(40, body + 0x9a);
	put_unaligned_le16(40, body + 0x33c);
	memset(body + 0x458, 0, 6);
}

/* Unassociated discovery has no data home-channel latency to protect. Allow
 * two ordinary 100TU beacon periods plus margin, instead of a selected97ms
 * timer shorter than one such period. Native normalization reserves13ms from
 * the budget;233 retains the requested220ms. This is host timing policy, not
 * a promise of uninterrupted reception. Keep connected scans unchanged.
 */
static inline void mt7932_scan_disconnected_passive(u8 body[0x4d4])
{
	mt7932_scan_passive(body);
	put_unaligned_le16(220, body + 0x4c2);
	put_unaligned_le16(233, body + 0x4ca);
}

/* Wait for the whole disconnected passive batch, not just one channel.
 * Retain the existing five-second command/completion margin and active wait.
 */
static inline unsigned int mt7932_scan_discovery_timeout(const u8 body[0x4d4])
{
	return 5000 + (body[2] ? 0 :
		      body[0x9f] * get_unaligned_le16(body + 0x4ca));
}

/* CID03 carries up to four directed SSIDs in 36-byte slots (le32 length,
 * 32 bytes) at offset 8; selector 8 marks a directed request and byte 4
 * counts the populated slots. Wildcard entries occupy no slot. A directed
 * request carries no probe IEs.
 */
#define MT7932_SCAN_SSIDS 4

static inline int mt7932_scan_ssid_slot(u8 body[0x4d4], const u8 *ssid,
					size_t ssid_length)
{
	u8 slot = body[4];

	if (!ssid_length)
		return 0;
	if (ssid_length > 32 || !body[2])
		return -EINVAL;
	if (slot >= MT7932_SCAN_SSIDS)
		return -ENOSPC;
	body[3] = 8;
	put_unaligned_le32(ssid_length, body + 8 + 36 * slot);
	memcpy(body + 12 + 36 * slot, ssid, ssid_length);
	body[4] = slot + 1;
	memset(body + 0xe2, 0, 600);
	put_unaligned_le16(0, body + 0xe0);
	return 0;
}

/* This firmware requires its native wildcard template. Directed scans use
 * no probe IEs. Forwarding the normal supplicant extras stalled the scan
 * on J700; arbitrary caller probe-IE carriage remains unsupported here.
 */
static inline int mt7932_scan_options(u8 body[0x4d4], const u8 *ssid,
				      size_t ssid_length, const u8 *ies, size_t ie_length)
{
	size_t offset = 0;

	if (ssid_length > 32 || ie_length > 600 || (!body[2] && ssid_length))
		return -EINVAL;
	while (offset < ie_length) {
		if (ie_length - offset < 2 || ies[offset + 1] > ie_length - offset - 2)
			return -EINVAL;
		offset += 2 + ies[offset + 1];
	}
	return mt7932_scan_ssid_slot(body, ssid, ssid_length);
}

/* Original online-scan state retains the BSS/keys and supplies its home
 * channel at the end of CID03. This never narrows the requested scan list.
 * Keep the batch's active/passive dwell settings and native home timing tail.
 */
static inline int mt7932_scan_home(u8 body[0x4d4], enum nl80211_band band, u8 channel)
{
	u8 firmware_band = channel ? mt7932_channel_band(band, channel) : 0;

	if (channel && !firmware_band)
		return -EINVAL;
	body[0x4cc] = channel;
	body[0x4cd] = firmware_band;
	return 0;
}

#endif
