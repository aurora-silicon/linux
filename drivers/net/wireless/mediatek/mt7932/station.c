// SPDX-License-Identifier: GPL-2.0-only
#include "mt7932.h"

static const char mt_rx_stat_names[][ETH_GSTRING_LEN] = {
	"host_rx_integrity_rejected",
	"host_rx_malformed",
	"host_rx_unsupported",
	"host_rx_wrong_peer",
	"host_rx_unprotected",
	"host_rx_retry_duplicate",
	"host_rx_no_memory",
	"host_rx_retry_records",
	"host_rx_amsdu_records",
	"irq_service_passes",
	"irq_unmasked_passes",
	"irq_unmasked_bits",
	"rx_ring0_records",
	"rx_ring1_records",
	"rx_ring2_records",
	"rx_ring3_records",
	"rx_ring4_records",
	"rx_ring5_records",
	"rx_ring6_records",
	"rx_ring7_records",
	"rx_ring8_records",
	"tx_free_events",
	"bss_absent_events",
	"bss_absent_ms",
	"home_beacons",
	"rx_2ghz_records",
	"rx_5ghz_records",
	"rx_6ghz_records",
};

static int mt_get_sset_count(struct net_device *netdev, int set)
{
	return set == ETH_SS_STATS ? ARRAY_SIZE(mt_rx_stat_names) : -EOPNOTSUPP;
}

static void mt_get_strings(struct net_device *netdev, u32 set, u8 *data)
{
	if (set == ETH_SS_STATS)
		memcpy(data, mt_rx_stat_names, sizeof(mt_rx_stat_names));
}

/* Host observations only: no firmware commands or invented ACK/BA counters.
 * They span associations so a test can take deltas across a reconnect. Retry
 * and A-MSDU counts are MSDU records, not over-air MPDU/aggregate counts.
 */
static void mt_get_ethtool_stats(struct net_device *netdev,
				 struct ethtool_stats *stats, u64 *data)
{
	struct mt7932 *m = *(struct mt7932 **)netdev_priv(netdev);
	unsigned long flags;
	unsigned int i;

	BUILD_BUG_ON(ARRAY_SIZE(mt_rx_stat_names) != 28);
	/* The interrupt thread alone writes these; a torn pass is harmless. */
	data[9] = READ_ONCE(m->irq_passes);
	data[10] = READ_ONCE(m->irq_unmasked_passes);
	data[11] = READ_ONCE(m->irq_unmasked);
	for (i = 0; i < ARRAY_SIZE(m->rx_ring_records); i++)
		data[12 + i] = READ_ONCE(m->rx_ring_records[i]);
	spin_lock_irqsave(&m->response_lock, flags);
	data[22] = m->bss_absent_events;
	data[23] = m->bss_absent_ms;
	data[24] = m->home_beacons;
	data[0] = m->rx_integrity;
	data[1] = m->rx_malformed;
	data[2] = m->rx_unsupported;
	data[3] = m->rx_wrong_peer;
	data[4] = m->rx_unprotected;
	data[5] = m->rx_duplicate;
	data[6] = m->rx_no_memory;
	data[7] = m->rx_retry;
	data[8] = m->rx_amsdu;
	spin_unlock_irqrestore(&m->response_lock, flags);
	if (READ_ONCE(m->data_ready)) {
		spin_lock_irqsave(&m->data_lock, flags);
		data[21] = m->tx_free_events;
		for (i = 0; i < ARRAY_SIZE(m->rx_band_records); i++)
			data[25 + i] = m->rx_band_records[i];
		spin_unlock_irqrestore(&m->data_lock, flags);
	} else {
		data[21] = 0;
		for (i = 0; i < ARRAY_SIZE(m->rx_band_records); i++)
			data[25 + i] = 0;
	}
}

const struct ethtool_ops mt_ethtool_ops = {
	.get_sset_count = mt_get_sset_count,
	.get_strings = mt_get_strings,
	.get_ethtool_stats = mt_get_ethtool_stats,
};

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

/*
 * The firmware reports no signal threshold events, so the connection quality
 * monitor runs on the host, as mac80211 runs it for drivers without one:
 * average the signal of the frames the associated access point sends and
 * report when the average crosses the threshold. Sample at most once per
 * beacon interval, so that the average follows time rather than traffic.
 */
#define MT7932_CQM_INTERVAL	(HZ / 10)
#define MT7932_CQM_MIN_SAMPLES	4

/* Caller holds response_lock; a new association starts a new average. */
void mt_cqm_reset(struct mt7932 *m)
{
	ewma_mt7932_signal_init(&m->cqm_signal);
	m->cqm_samples = 0;
	m->cqm_last_event = 0;
}

/* Caller holds response_lock; @signal is a frame from the associated AP. */
void mt_cqm_signal(struct mt7932 *m, int signal)
{
	enum nl80211_cqm_rssi_threshold_event event;
	int average, last = m->cqm_last_event;
	int hysteresis = m->cqm_hysteresis;

	if (signal >= 0 || signal <= -128)
		return;
	if (m->cqm_samples &&
	    time_before(jiffies, m->cqm_sample_time + MT7932_CQM_INTERVAL))
		return;
	m->cqm_sample_time = jiffies;
	ewma_mt7932_signal_add(&m->cqm_signal, -signal);
	if (m->cqm_samples < MT7932_CQM_MIN_SAMPLES)
		m->cqm_samples++;
	if (!m->cqm_threshold || m->cqm_samples < MT7932_CQM_MIN_SAMPLES)
		return;

	/* The first event after (re)configuration reports the current side. */
	average = -(int)ewma_mt7932_signal_read(&m->cqm_signal);
	if (average < m->cqm_threshold && (!last || average < last - hysteresis))
		event = NL80211_CQM_RSSI_THRESHOLD_EVENT_LOW;
	else if (average > m->cqm_threshold && (!last || average > last + hysteresis))
		event = NL80211_CQM_RSSI_THRESHOLD_EVENT_HIGH;
	else
		return;
	m->cqm_last_event = average;
	cfg80211_cqm_rssi_notify(m->netdev, event, average, GFP_ATOMIC);
}

int mt_set_cqm_rssi_config(struct wiphy *wiphy, struct net_device *netdev,
			   s32 rssi_thold, u32 rssi_hyst)
{
	struct mt7932 *m = mt_from_wiphy(wiphy);
	unsigned long flags;

	if (netdev != m->netdev)
		return -ENODEV;
	spin_lock_irqsave(&m->response_lock, flags);
	/* A zero threshold disables the monitor. */
	m->cqm_threshold = rssi_thold;
	m->cqm_hysteresis = min_t(u32, rssi_hyst, S8_MAX);
	m->cqm_last_event = 0;
	spin_unlock_irqrestore(&m->response_lock, flags);
	return 0;
}
