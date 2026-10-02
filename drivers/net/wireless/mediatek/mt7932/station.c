// SPDX-License-Identifier: GPL-2.0-only
#include "mt7932.h"

/* Report the owned, validated EE4d association, not an AP advertisement
 * or a transient off-channel scan.
 */
int mt_get_channel(struct wiphy *wiphy, struct wireless_dev *wdev,
		   unsigned int link_id, struct cfg80211_chan_def *chandef)
{
	struct mt7932 *m = mt_from_wiphy(wiphy);
	unsigned long flags;
	int ret = -ENODATA;
	bool ht;

	if (wdev != &m->wdev)
		return -ENODEV;
	if (link_id)
		return -ENOENT;
	spin_lock_irqsave(&m->response_lock, flags);
	if (!m->connected || m->disconnecting || m->stopping || !m->peer_valid)
		goto out;
	if (!m->connect_center || !m->connect_chandef.chan)
		goto out;
	ht = cfg80211_find_elem(WLAN_EID_HT_CAPABILITY, m->assoc_request_ies,
				m->assoc_request_ie_len) != NULL;
	*chandef = m->connect_chandef;
	if (!ht && chandef->width == NL80211_CHAN_WIDTH_20)
		chandef->width = NL80211_CHAN_WIDTH_20_NOHT;
	ret = 0;
out:
	spin_unlock_irqrestore(&m->response_lock, flags);
	return ret;
}

/* Caller holds response_lock, which serializes peer publication/retirement.
 * These are host Ethernet counters, not firmware ACK/retry or MPDU counters.
 * Do not invent rate, retry or failed-transmission data from DMA completion.
 */
static int mt_station_snapshot(struct mt7932 *m, struct station_info *sinfo)
{
	if (!m->connected || m->disconnecting || m->stopping || !m->peer_valid)
		return -ENOENT;

	spin_lock(&m->data_lock);
	sinfo->connected_time = (jiffies - m->station_connected) / HZ;
	sinfo->tx_bytes = m->station_tx_bytes;
	sinfo->rx_bytes = m->station_rx_bytes;
	sinfo->tx_packets = m->station_tx_packets;
	sinfo->rx_packets = m->station_rx_packets;
	sinfo->filled |= BIT_ULL(NL80211_STA_INFO_CONNECTED_TIME) |
		BIT_ULL(NL80211_STA_INFO_TX_BYTES64) |
		BIT_ULL(NL80211_STA_INFO_RX_BYTES64) |
		BIT_ULL(NL80211_STA_INFO_TX_PACKETS) |
		BIT_ULL(NL80211_STA_INFO_RX_PACKETS);
	if (m->station_signal_valid) {
		sinfo->signal = m->station_signal;
		sinfo->filled |= BIT_ULL(NL80211_STA_INFO_SIGNAL);
	}
	if (m->station_rx_rate_valid &&
	    time_before(jiffies, m->station_rx_rate_time + HZ)) {
		sinfo->rxrate = m->station_rx_rate;
		sinfo->filled |= BIT_ULL(NL80211_STA_INFO_RX_BITRATE);
	}
	/* CID85 echoes station identity but its rate words come from a shared
	 * TX vector without a peer argument, atomicity or freshness attestation.
	 * Do not publish that vector as this station's current TX bitrate.
	 */
	spin_unlock(&m->data_lock);
	return 0;
}

int mt_get_station(struct wiphy *wiphy, struct wireless_dev *wdev,
		   const u8 *mac, struct station_info *sinfo)
{
	struct mt7932 *m = mt_from_wiphy(wiphy);
	unsigned long flags;
	int ret = -ENOENT;

	if (wdev != &m->wdev)
		return -ENODEV;
	spin_lock_irqsave(&m->response_lock, flags);
	if (ether_addr_equal(mac, m->connect_bssid))
		ret = mt_station_snapshot(m, sinfo);
	spin_unlock_irqrestore(&m->response_lock, flags);
	return ret;
}

int mt_dump_station(struct wiphy *wiphy, struct wireless_dev *wdev,
		    int idx, u8 *mac, struct station_info *sinfo)
{
	struct mt7932 *m = mt_from_wiphy(wiphy);
	unsigned long flags;
	int ret;

	if (wdev != &m->wdev)
		return -ENODEV;
	if (idx != 0)
		return -ENOENT;
	spin_lock_irqsave(&m->response_lock, flags);
	ret = mt_station_snapshot(m, sinfo);
	if (!ret)
		ether_addr_copy(mac, m->connect_bssid);
	spin_unlock_irqrestore(&m->response_lock, flags);
	return ret;
}
