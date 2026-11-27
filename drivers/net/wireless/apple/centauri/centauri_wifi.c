// SPDX-License-Identifier: GPL-2.0-only
/* cfg80211 driver for the Apple N1 Alpha Wi-Fi ACIPC transport. */
#include <linux/bitfield.h>
#include <linux/etherdevice.h>
#include <linux/ethtool.h>
#include <linux/debugfs.h>
#include <linux/ieee80211.h>
#include <linux/iopoll.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_net.h>
#include <linux/rtnetlink.h>
#include <linux/seq_file.h>
#include <linux/unaligned.h>
#include <linux/workqueue.h>
#include <net/cfg80211.h>
#include <net/regulatory.h>
#include <net/sock.h>
#include "centauri.h"

#define CR_BASE 0x100000
#define TR_BASE 0x400000
#define RING_SPAN 0x20000
#define TX_BUFFER 0x600000
#define BUFFER_SIZE 2048
#define REPLY_SIZE (BUFFER_SIZE - 24)
/* The band plan: 2.4 GHz 1-14, 5 GHz 36-64, 100-144 and 149-177, 6 GHz 1-233. */
#define CEN_CHANNELS (14 + 28 + 59)
#define DATA_TX_BUFFER 0xd00000
#define DATA_RX_BUFFER 0xe00000
#define DATA_TX_SLOTS 128
#define DATA_TX_QUEUE_LIMIT 64
#define DATA_TX_INFLIGHT_MAX 64
#define DATA_TX_PACING_SHIFT 7

/* The firmware owns window +8 MiB through +13 MiB as scratch memory. */
static_assert(DATA_TX_BUFFER >= CEN_ALPHA_SCRATCH_OFFSET + CEN_ALPHA_SCRATCH_SIZE);
static_assert(DATA_TX_BUFFER + DATA_TX_SLOTS * BUFFER_SIZE <= DATA_RX_BUFFER);

static bool auto_rings = true;
module_param(auto_rings, bool, 0644);
MODULE_PARM_DESC(auto_rings, "Open the data rings (data_probe ids 76, 24, 15, 12 before the join, 25 after it) from the connect path; 1 = on (default). The data_probe debugfs files still work and are skipped for rings already open");

static int data_probe_locked(struct centauri_wifi *wifi, unsigned int id, bool internal);

static bool roam_scan;
module_param(roam_scan, bool, 0644);
MODULE_PARM_DESC(roam_scan, "Let the firmware run its own roam and background scans after a join; 0 = off (default). The scans take the radio off channel for seconds (72 percent ping loss measured) until the host programs a roam profile");

static bool host_setup = true;
module_param(host_setup, bool, 0644);
MODULE_PARM_DESC(host_setup, "Before a join, tell the firmware the host is awake (command 6) and apply the reference TX defaults (commands 43 and 52) when the tx_config files have not; 1 = on (default)");

static bool verify_pmk;
module_param(verify_pmk, bool, 0400);
MODULE_PARM_DESC(verify_pmk, "Compare firmware PMK with the requested key without exposing either");

static bool eapol_after_assoc;
module_param(eapol_after_assoc, bool, 0644);
MODULE_PARM_DESC(eapol_after_assoc, "One EAPOL-Start probe at the next successful association event");

static bool link_diagnostics;
module_param(link_diagnostics, bool, 0644);
MODULE_PARM_DESC(link_diagnostics, "Query non-secret firmware link status during association");

static bool eapol_control_profile;
module_param(eapol_control_profile, bool, 0644);
MODULE_PARM_DESC(eapol_control_profile, "Use the measured control TX profile for the EAPOL-Start probe");

static bool eapol_unicast;
module_param(eapol_unicast, bool, 0644);
MODULE_PARM_DESC(eapol_unicast, "Address the bounded EAPOL-Start probe to the associated AP");

static bool eapol_relative_lifetime;
module_param(eapol_relative_lifetime, bool, 0644);
MODULE_PARM_DESC(eapol_relative_lifetime, "Use a measured two-second relative expiration for the probe");

static bool data_tx_broadcast;
module_param(data_tx_broadcast, bool, 0400);
MODULE_PARM_DESC(data_tx_broadcast, "Diagnostic: open TR 25 with measured broadcast metadata; transmit only broadcast Ethernet frames");

static bool data_tx_control_profile;
module_param(data_tx_control_profile, bool, 0600);
MODULE_PARM_DESC(data_tx_control_profile, "Diagnostic: select configured TX policy 1 for normal data; apply tx_config action 2 first");

static bool data_tx_native_prepare = true;
module_param(data_tx_native_prepare, bool, 0600);
MODULE_PARM_DESC(data_tx_native_prepare, "Use native packet preparation value 4; disable only to reproduce the legacy data TX failure");

static bool station_link_addresses;
module_param(station_link_addresses, bool, 0400);
MODULE_PARM_DESC(station_link_addresses, "Diagnostic: use the netdev address for each firmware link in this single-link WPA2 client");

static bool alpha_sleep_untested;
module_param(alpha_sleep_untested, bool, 0600);
MODULE_PARM_DESC(alpha_sleep_untested, "TEST ONLY: J813 Alpha host-power and ACIPC sleep handshake");

struct centauri_rx_snapshot {
	__le32 version, count;
	struct {
		u8 completion[32];
		u8 packet[4096];
	} sample[8];
};

struct centauri_wifi {
	struct centauri *control;
	struct cen_alpha_resources a;
	struct wiphy *wiphy;
	struct wireless_dev wdev;
	struct net_device *netdev;
	struct work_struct scan_work;
	struct work_struct connect_work;
	struct delayed_work link_work;
	struct cfg80211_scan_request *scan_request;
	bool scan_abort, connect_abort;
	u8 scan_sequence;
	struct mutex lock;
	/* The logger has its own coherent allocation. Reads/acknowledgements
	 * must remain possible while scan/connect hold the command mutex.
	 * Initialization takes lock then fwlog_lock; readers take only the latter.
	 */
	struct mutex fwlog_lock;
	u16 main_head, command_head, sequence;
	u32 rx_seen[5];
	u16 rx_next[5], rx_head[5];
	u16 reply_sequence, reply_status, reply_capture;
	u32 reply_command, channels_seen, scans_done, beacons_seen;
	bool reply_valid, registered, configured;
	u8 *reply_buffer;
	size_t reply_capacity, reply_size;
	bool stopping;
	bool transport_paused, transport_sleep_sent, host_sleep_sent;
	bool host_events_fallback_logged;
	bool connecting, connected, connect_event, disconnect_event;
	bool connect_privacy;
	bool associated, pmk_seen, pmk_matches;
	u8 expected_pmk[32];
	u16 connect_status, disconnect_reason;
	bool disconnect_local;
	struct ieee80211_channel *connect_channel;
	u8 bssid[ETH_ALEN], connect_payload[1024], request_ie[256];
	u8 ssid[IEEE80211_MAX_SSID_LEN];
	size_t connect_size, request_ie_size, ssid_len;
	struct dentry *data_probe_file;
	struct dentry *data_status_file;
	struct dentry *data_rx_snapshot_file;
	struct centauri_rx_snapshot rx_snapshot;
	u32 data_rx_received, data_rx_reposted, data_rx_bad;
	struct sk_buff_head data_tx_queue;
	u16 data_tx_length[DATA_TX_SLOTS];
	bool data_tx_busy[DATA_TX_SLOTS];
	u32 data_tx_submitted, data_tx_completed, data_tx_failed;
	u32 data_tx_coalesced, data_tx_bad;
	u32 data_tx_capped, data_tx_cap_bypassed;
	unsigned long data_tx_completed_at;
	struct napi_struct rx_napi;
	struct sk_buff_head rx_gro_queue;
	s8 station_signal, station_noise;
	bool station_signal_valid, station_noise_valid;
	s8 station_chain_signal[2], station_phy_noise;
	u8 station_chains;
	bool station_phy_noise_valid;
	u32 station_tx_retrans, station_tx_retrans_base;
	bool station_tx_retrans_valid, station_tx_retrans_base_valid;
	u32 station_tx_fail, station_tx_fail_base;
	bool station_tx_fail_valid, station_tx_fail_base_valid;
	u32 station_rx_rate_kbps, station_chip_tx_rate_kbps;
	bool station_rx_rate_valid, station_chip_tx_rate_valid;
	u32 station_beacon_rx, station_beacon_rx_base;
	u32 station_beacon_loss, station_beacon_loss_base;
	bool station_beacon_valid, station_beacon_base_valid;
	u64 station_cca_active_ms, station_cca_busy_ms, station_cca_tx_ms;
	bool station_cca_valid;
	struct rate_info station_tx_rate;
	int station_tx_power_dbm;
	bool station_tx_rate_valid, station_tx_power_valid;
	struct cfg80211_chan_def station_chandef;
	u8 station_bw_primary;
	bool station_chandef_valid;
	unsigned long station_signal_at, station_noise_at, station_counters_at;
	unsigned long station_cca_at, station_tx_info_at, station_beacon_at;
	unsigned long station_phy_dump_at, connected_at;
	struct net_device_stats station_start;
	struct dentry *station_stats_file;
	struct dentry *eapol_start_file;
	struct dentry *country_file;
	struct dentry *link_status_file;
	struct dentry *tx_config_file;
	struct dentry *host_wakeup_file;
	struct dentry *fwlog_start_file, *fwlog_status_file, *fwlog_buffer_file, *fwlog_ack_file;
	void *fwlog_buffer;
	dma_addr_t fwlog_dma;
	size_t fwlog_size;
	bool fwlog_attempted, fwlog_ready;
	u8 tx_profile[10], tx_defaults_applied;
	bool host_awake_sent;
	bool tx_profile_valid;
	u8 link_count, link_prime[3], link_channel[3], link_band[3], emlsr_mode;
	bool link_status_valid, emlsr_valid;
	u16 link_status_size;
	u16 link_tags[12], link_lengths[12];
	u8 link_tlv_count;
	u8 country[3];
	bool country_valid;
	u8 station_stats[0x378];
	u16 station_stats_len;
	bool station_stats_valid;
	u8 rx_crypto_stats[64];
	bool rx_crypto_stats_valid;
	struct dentry *rx_crypto_stats_file;
	u8 dot11_mib_stats[192];
	bool dot11_mib_stats_valid;
	struct dentry *dot11_mib_stats_file;
	u8 tx_power_report[280];
	bool tx_power_report_valid;
	struct dentry *tx_power_report_file;
	u16 stats_reply_size, stats_tag, stats_length;
	u32 data_probe_done;
	struct ieee80211_supported_band bands[3];
	struct ieee80211_channel channels[CEN_CHANNELS];
	struct ieee80211_sband_iftype_data iftype_data[3];
	u8 hw_channels[REPLY_SIZE], system_caps[REPLY_SIZE];
	u8 stats_reply[REPLY_SIZE];
	struct debugfs_blob_wrapper hw_channels_blob, system_caps_blob;
};

/* These helpers require wifi->lock, including from the connect worker. */
static int transmit_eapol_start(struct centauri_wifi *wifi);
static int query_station_stats(struct centauri_wifi *wifi);
static int query_link_status(struct centauri_wifi *wifi);

static const u32 rx_buffer[5] = { 0, 0, 0x620000, 0x680000, 0x700000 };
static const u16 ring_size[5] = { 0, 64, 192, 96, 192 };
static const struct {
	enum nl80211_band band;
	u8 first, last, step;
} band_plan[] = {
	{ NL80211_BAND_2GHZ, 1, 14, 1 },
	{ NL80211_BAND_5GHZ, 36, 64, 4 },
	{ NL80211_BAND_5GHZ, 100, 144, 4 },
	{ NL80211_BAND_5GHZ, 149, 177, 4 },
	{ NL80211_BAND_6GHZ, 1, 233, 4 },
};
static struct ieee80211_rate rates[] = {
	{ .bitrate = 10 }, { .bitrate = 20 }, { .bitrate = 55 }, { .bitrate = 110 },
	{ .bitrate = 60 }, { .bitrate = 90 }, { .bitrate = 120 }, { .bitrate = 180 },
	{ .bitrate = 240 }, { .bitrate = 360 }, { .bitrate = 480 }, { .bitrate = 540 },
};

static const u32 akm_suites[] = {
	WLAN_AKM_SUITE_PSK,
	WLAN_AKM_SUITE_PSK_SHA256,
	WLAN_AKM_SUITE_SAE,
};
static const u32 cipher_suites[] = {
	WLAN_CIPHER_SUITE_TKIP,
	WLAN_CIPHER_SUITE_CCMP,
	WLAN_CIPHER_SUITE_GCMP,
	WLAN_CIPHER_SUITE_GCMP_256,
	WLAN_CIPHER_SUITE_AES_CMAC,
};

/* The firmware's band numbers: 0 is 2.4 GHz, 1 is 5 GHz, 2 is 6 GHz. */
static int firmware_band(enum nl80211_band band)
{
	switch (band) {
	case NL80211_BAND_2GHZ:
		return 0;
	case NL80211_BAND_5GHZ:
		return 1;
	case NL80211_BAND_6GHZ:
		return 2;
	default:
		return -EOPNOTSUPP;
	}
}

static u16 index_read(struct centauri_wifi *wifi, unsigned int array, unsigned int index)
{
	return le16_to_cpu(READ_ONCE(*(__le16 *)(wifi->a.context + wifi->a.index[array] + 2 * index)));
}

static void index_write(struct centauri_wifi *wifi, unsigned int array,
			unsigned int index, u16 value)
{
	WRITE_ONCE(*(__le16 *)(wifi->a.context + wifi->a.index[array] + 2 * index), cpu_to_le16(value));
}

static void completions(struct centauri_wifi *wifi)
{
	unsigned int vector;

	for (vector = 0; vector <= 4; vector++)
		index_write(wifi, 2, vector, index_read(wifi, 0, vector));
	dma_wmb();
}

static int alpha_message(struct centauri_wifi *wifi, const void *message, size_t size)
{
	u16 head = wifi->main_head;
	u8 *descriptor;
	unsigned long deadline = jiffies + msecs_to_jiffies(2000);

	if (READ_ONCE(wifi->transport_paused))
		return -EBUSY;
	if (head >= 15 || size > 512)
		return -ENOSPC;
	memcpy(wifi->a.messages + (head + 1) * 512, message, size);
	descriptor = wifi->a.context + wifi->a.mtr + 16 * head;
	put_unaligned_le32(1 | (size << 8), descriptor);
	put_unaligned_le64(wifi->a.messages_dma + (head + 1) * 512, descriptor + 4);
	put_unaligned_le32(head + 1, descriptor + 12);
	index_write(wifi, 3, 0, head + 1);
	if (head)
		index_write(wifi, 3, 8, head + 1);
	dma_wmb();
	wifi->main_head++;
	writel(1, wifi->a.bar + 0x1004);
	do {
		if (readl(wifi->a.bar + 8) != 2)
			return -EIO;
		if (index_read(wifi, 1, 0) >= head + 1) {
			dma_rmb();
			completions(wifi);
			return 0;
		}
		usleep_range(100, 200);
	} while (time_before(jiffies, deadline));
	return -ETIMEDOUT;
}

static int open_pair(struct centauri_wifi *wifi, u16 id)
{
	u8 cr[0x2c] = { 2 }, tr[0x34] = { 1 };
	int ret;

	put_unaligned_le32(id | (id << 16), cr + 4);
	put_unaligned_le64(wifi->a.window_dma + CR_BASE + (id - 1) * RING_SPAN, cr + 8);
	put_unaligned_le16(1024, cr + 0x10);
	put_unaligned_le16(0xffff, cr + 0x12);
	ret = alpha_message(wifi, cr, sizeof(cr));
	if (ret)
		return ret;
	put_unaligned_le32(id | (id << 16), tr + 4);
	put_unaligned_le64(wifi->a.window_dma + TR_BASE + (id - 1) * RING_SPAN, tr + 8);
	put_unaligned_le64(~0ULL, tr + 0x10);
	put_unaligned_le16(ring_size[id], tr + 0x18);
	put_unaligned_le16(id, tr + 0x1a);
	put_unaligned_le16(id, tr + 0x1c);
	put_unaligned_le16(0x40, tr + 0x1e);
	put_unaligned_le16(2, tr + 0x24);
	return alpha_message(wifi, tr, sizeof(tr));
}

static void report_beacon(struct centauri_wifi *wifi, const u8 *payload, size_t size)
{
	const u8 *frame, *ie, *end;
	struct ieee80211_channel *channel;
	struct cfg80211_bss *bss;
	u16 frame_size;
	int number = 0, frequency, signal;
	enum nl80211_band band = NL80211_BAND_2GHZ;

	if (!wifi->registered || size < 0x60 + 36)
		return;
	frame_size = size - 0x60;
	frame = payload + 0x60;
	if (!ieee80211_is_beacon(*(__le16 *)frame) && !ieee80211_is_probe_resp(*(__le16 *)frame))
		return;
	end = frame + frame_size;
	for (ie = frame + 36; ie + 2 <= end && ie + 2 + ie[1] <= end; ie += 2 + ie[1]) {
		if ((ie[0] == WLAN_EID_DS_PARAMS || ie[0] == WLAN_EID_HT_OPERATION) && ie[1]) {
			number = ie[2];
			band = number <= 14 ? NL80211_BAND_2GHZ : NL80211_BAND_5GHZ;
		} else if (ie[0] == WLAN_EID_EXTENSION && ie[1] > 1 &&
			   ie[2] == WLAN_EID_EXT_HE_OPERATION &&
			   ie[1] - 1 >= sizeof(struct ieee80211_he_operation) &&
			   ie[1] >= ieee80211_he_oper_size(ie + 3)) {
			const struct ieee80211_he_6ghz_oper *oper =
				ieee80211_he_6ghz_oper((const void *)(ie + 3));

			if (oper && !number) {
				number = oper->primary;
				band = NL80211_BAND_6GHZ;
			}
		}
	}
	if (!number)
		return;
	frequency = ieee80211_channel_to_frequency(number, band);
	channel = ieee80211_get_channel(wifi->wiphy, frequency);
	if (!channel || channel->flags & IEEE80211_CHAN_DISABLED)
		return;
	signal = max((s8)payload[0x16], (s8)payload[0x17]);
	if (signal >= 0 || signal < -127)
		signal = -95;
	bss = cfg80211_inform_bss_frame(wifi->wiphy, channel,
		(struct ieee80211_mgmt *)frame, frame_size, signal * 100, GFP_KERNEL);
	if (bss) {
		wifi->beacons_seen++;
		cfg80211_put_bss(wifi->wiphy, bss);
	}
}

static void post_receive(struct centauri_wifi *wifi, unsigned int ring, u16 slot)
{
	u8 *descriptor = wifi->a.window + TR_BASE + (ring - 1) * RING_SPAN + slot * 16;
	u32 offset = rx_buffer[ring] + slot * BUFFER_SIZE;

	memset(wifi->a.window + offset, 0, BUFFER_SIZE);
	put_unaligned_le32(1 | (BUFFER_SIZE << 8), descriptor);
	put_unaligned_le64(wifi->a.window_dma + offset, descriptor + 4);
	put_unaligned_le32(slot + 1, descriptor + 12);
}

static bool data_rx_frame(const u8 *packet, unsigned int capacity,
			  unsigned int length, u32 footer, unsigned int *offset,
			  unsigned int *frame_length)
{
	unsigned int padding = (footer >> 12) & 15;

	if (!(footer & BIT(7)))
		return false;
	if (length < padding + ETH_HLEN || length > capacity)
		return false;
	if (footer & BIT(24)) {
		if (capacity < 4)
			return false;
		*offset = get_unaligned_le16(packet + 2);
	} else {
		*offset = padding;
	}
	*frame_length = length - padding;
	return *offset <= capacity && *frame_length <= capacity - *offset;
}

static int rx_poll(struct napi_struct *napi, int budget)
{
	struct centauri_wifi *wifi =
		container_of(napi, struct centauri_wifi, rx_napi);
	struct sk_buff *skb;
	int done = 0;

	while (done < budget && (skb = skb_dequeue(&wifi->rx_gro_queue))) {
		napi_gro_receive(napi, skb);
		done++;
	}
	if (done < budget && napi_complete_done(napi, done) &&
	    !skb_queue_empty(&wifi->rx_gro_queue))
		napi_schedule(napi);
	return done;
}

/* Called with wifi->lock, including during connect/command polling. */
static void drain_data_rx(struct centauri_wifi *wifi)
{
	u16 head, tail, post;
	unsigned int processed = 0;
	bool consumed = false, reposted = false;
	struct sk_buff_head frames;

	__skb_queue_head_init(&frames);

	if (READ_ONCE(wifi->transport_paused) ||
	    !(wifi->data_probe_done & BIT(4)) || readl(wifi->a.bar + 8) != 2)
		return;
	head = index_read(wifi, 0, 207);
	tail = index_read(wifi, 2, 207);
	post = index_read(wifi, 3, 12);
	if (head >= 128 || tail >= 128 || post >= 128) {
		wifi->data_rx_bad++;
		return;
	}
	dma_rmb();
	while (tail != head && processed < 127) {
		u8 *completion = wifi->a.window + 0x240000 + tail * 32;
		u32 header = get_unaligned_le32(completion);
		u32 info = get_unaligned_le32(completion + 4);
		u32 status = get_unaligned_le32(completion + 8);
		u16 cookie = info & 0xffff, length = info >> 16;
		u16 next = (post + 1) % 128;
		bool valid_cookie = cookie >= 1 && cookie <= 127;
		u32 count = le32_to_cpu(wifi->rx_snapshot.count);
		u8 *packet, *descriptor;
		u32 buffer;

		if ((header >> 16) != 12 || !valid_cookie || length > 4096 ||
		    status) {
			if ((header >> 16) == 12 && valid_cookie &&
			    next == index_read(wifi, 1, 12))
				break;
			wifi->data_rx_bad++;
			wifi->netdev->stats.rx_errors++;
			dev_warn_ratelimited(&wifi->control->pdev->dev,
					     "Wi-Fi data RX bad completion: ring=%u cookie=%u len=%u status=%#x\n",
					     header >> 16, cookie, length,
					     status);
			if ((header >> 16) == 12 && valid_cookie) {
				buffer = DATA_RX_BUFFER + (cookie - 1) * 4096;
				descriptor = wifi->a.window + 0x520000 +
					     post * 16;
				put_unaligned_le32(1 | (4096 << 8), descriptor);
				put_unaligned_le64(wifi->a.window_dma + buffer,
						   descriptor + 4);
				put_unaligned_le32(cookie, descriptor + 12);
				wifi->data_rx_reposted++;
				post = next;
				reposted = true;
			}
			tail = (tail + 1) % 128;
			consumed = true;
			processed++;
			continue;
		}
		/* Do not overwrite a descriptor the receive engine still owns. */
		if (next == index_read(wifi, 1, 12))
			break;
		buffer = DATA_RX_BUFFER + (cookie - 1) * 4096;
		packet = wifi->a.window + buffer;
		if (count < ARRAY_SIZE(wifi->rx_snapshot.sample)) {
			memcpy(wifi->rx_snapshot.sample[count].completion, completion, 32);
			memcpy(wifi->rx_snapshot.sample[count].packet, packet, length);
			wifi->rx_snapshot.version = cpu_to_le32(1);
			wifi->rx_snapshot.count = cpu_to_le32(count + 1);
		}
		wifi->data_rx_received++;
		if (wifi->connected && netif_running(wifi->netdev)) {
			u32 footer = get_unaligned_le32(completion + 12);
			unsigned int offset, frame_length;
			struct sk_buff *skb = NULL;

			if (data_rx_frame(packet, 4096, length, footer, &offset,
					  &frame_length))
				skb = netdev_alloc_skb_ip_align(wifi->netdev,
								frame_length);
			if (skb) {
				skb_put_data(skb, packet + offset, frame_length);
				skb->protocol = eth_type_trans(skb, wifi->netdev);
				skb->ip_summed = CHECKSUM_NONE;
				wifi->netdev->stats.rx_packets++;
				wifi->netdev->stats.rx_bytes += frame_length;
				__skb_queue_tail(&frames, skb);
			} else {
				wifi->netdev->stats.rx_dropped++;
			}
		} else {
			wifi->netdev->stats.rx_dropped++;
		}
		descriptor = wifi->a.window + 0x520000 + post * 16;
		put_unaligned_le32(1 | (4096 << 8), descriptor);
		put_unaligned_le64(wifi->a.window_dma + buffer, descriptor + 4);
		put_unaligned_le32(cookie, descriptor + 12);
		wifi->data_rx_reposted++;
		tail = (tail + 1) % 128;
		post = next;
		consumed = true;
		reposted = true;
		processed++;
	}
	if (consumed || reposted) {
		dma_wmb();
		if (consumed)
			index_write(wifi, 2, 207, tail);
		if (reposted)
			index_write(wifi, 3, 12, post);
		dma_wmb();
		writel(1, wifi->a.bar + 0x1050);
	}
	if (!skb_queue_empty(&frames)) {
		spin_lock_bh(&wifi->rx_gro_queue.lock);
		skb_queue_splice_tail_init(&frames, &wifi->rx_gro_queue);
		spin_unlock_bh(&wifi->rx_gro_queue.lock);
		local_bh_disable();
		napi_schedule(&wifi->rx_napi);
		local_bh_enable();
	}
}

static bool data_tx_completion_span(u16 cookie, u16 wire_count, u8 flags,
				    unsigned int *first, u16 *count)
{
	bool counted = (flags & BIT(0)) || (flags & 0x0e) == 8;

	if (!cookie || cookie > DATA_TX_SLOTS || counted != !!wire_count ||
	    wire_count >= DATA_TX_SLOTS)
		return false;
	*count = counted ? wire_count : 1;
	*first = (cookie + DATA_TX_SLOTS - *count) % DATA_TX_SLOTS;
	return true;
}

/* Serialized with the firmware poller. Payload slots remain owned until a
 * CR completion, even if the device has already consumed the TR descriptor.
 */
static void drain_data_tx(struct centauri_wifi *wifi)
{
	u16 head, tail, post;
	unsigned int budget = 256;
	bool posted = false, completed = false;

	if (READ_ONCE(wifi->transport_paused) ||
	    !(wifi->data_probe_done & BIT(5)) || readl(wifi->a.bar + 8) != 2)
		return;
	head = index_read(wifi, 0, 192);
	tail = index_read(wifi, 2, 192);
	post = index_read(wifi, 3, 25);
	if (head >= 2048 || tail >= 2048 || post >= DATA_TX_SLOTS) {
		wifi->data_tx_bad++;
		return;
	}
	dma_rmb();
	while (tail != head && budget--) {
		u8 *entry = wifi->a.window + 0x200000 + tail * 32;
		u16 ring = get_unaligned_le32(entry) >> 16;
		u16 cookie = get_unaligned_le16(entry + 4);
		u16 count = get_unaligned_le16(entry + 9);
		u8 status = entry[12];

		if (ring == 25) {
			unsigned int first, i;

			if (!data_tx_completion_span(cookie, count, entry[1],
						     &first, &count)) {
				wifi->data_tx_bad++;
				dev_warn_ratelimited(&wifi->control->pdev->dev,
						     "Wi-Fi data TX bad completion: cookie=%u count=%u flags=%#x status=%#x\n",
						     cookie, count, entry[1],
						     status);
				tail = (tail + 1) % 2048;
				completed = true;
				continue;
			}
			for (i = 0; i < count; i++)
				if (!wifi->data_tx_busy[(first + i) % DATA_TX_SLOTS])
					break;
			if (i != count) {
				wifi->data_tx_bad++;
				dev_warn_ratelimited(&wifi->control->pdev->dev,
						     "Wi-Fi data TX unexpected slot state: first=%u count=%u status=%#x\n",
						     first, count, status);
				for (i = 0; i < count; i++) {
					unsigned int slot = (first + i) %
							    DATA_TX_SLOTS;

					if (!wifi->data_tx_busy[slot])
						continue;
					wifi->data_tx_busy[slot] = false;
					wifi->data_tx_completed++;
					wifi->data_tx_failed++;
					wifi->netdev->stats.tx_errors++;
				}
				tail = (tail + 1) % 2048;
				completed = true;
				continue;
			}
			if (count > 1)
				wifi->data_tx_coalesced++;
			wifi->data_tx_completed_at = jiffies;
			for (i = 0; i < count; i++) {
				unsigned int slot = (first + i) % DATA_TX_SLOTS;

				wifi->data_tx_completed++;
				if (status == 0 || status == 0x83) {
					wifi->netdev->stats.tx_packets++;
					wifi->netdev->stats.tx_bytes += wifi->data_tx_length[slot];
				} else {
					wifi->data_tx_failed++;
					wifi->netdev->stats.tx_errors++;
				}
				wifi->data_tx_busy[slot] = false;
			}
		}
		tail = (tail + 1) % 2048;
		completed = true;
	}
	if (completed)
		index_write(wifi, 2, 192, tail);
	budget = DATA_TX_SLOTS - 1;
	while (wifi->connected && !READ_ONCE(wifi->stopping) &&
	       !READ_ONCE(wifi->transport_paused) &&
	       netif_running(wifi->netdev) && budget--) {
		u16 next = (post + 1) % DATA_TX_SLOTS;
		u32 buffer = DATA_TX_BUFFER + post * BUFFER_SIZE;
		u8 *descriptor = wifi->a.window + 0x540000 + post * 32;
		struct sk_buff *skb;
		u8 destination[ETH_ALEN];
		u64 metadata = 6ULL << 27;
		u32 inflight = wifi->data_tx_submitted - wifi->data_tx_completed;

		if (wifi->data_tx_busy[post] || next == index_read(wifi, 1, 25))
			break;
		if (inflight >= DATA_TX_INFLIGHT_MAX &&
		    !skb_queue_empty(&wifi->data_tx_queue)) {
			if (time_before(jiffies,
					wifi->data_tx_completed_at + HZ / 2)) {
				wifi->data_tx_capped++;
				break;
			}
			if (!wifi->data_tx_cap_bypassed++)
				dev_warn(&wifi->control->pdev->dev,
					 "Wi-Fi TX: no completion for %u in-flight frames; posting past the cap\n",
					 inflight);
		}
		skb = skb_dequeue(&wifi->data_tx_queue);
		if (!skb)
			break;
		/* This diagnostic ring is only valid for its broadcast destination.
		 * Do not silently send unicast or IPv6 multicast through that flow.
		 */
		if (data_tx_broadcast &&
		    (skb_copy_bits(skb, 0, destination, ETH_ALEN) ||
		     !is_broadcast_ether_addr(destination))) {
			wifi->netdev->stats.tx_dropped++;
			dev_kfree_skb_any(skb);
			continue;
		}
		if (skb_copy_bits(skb, 0, wifi->a.window + buffer, skb->len)) {
			wifi->netdev->stats.tx_dropped++;
			dev_kfree_skb_any(skb);
			continue;
		}
		memset(descriptor, 0, 32);
		put_unaligned_le32(3 | (skb->len << 8), descriptor);
		put_unaligned_le64(wifi->a.window_dma + buffer, descriptor + 4);
		put_unaligned_le16(post + 1, descriptor + 12);
		/* Optional TX policy ID 1 (validity bit 60 and ID bit 48). */
		if (READ_ONCE(data_tx_control_profile) &&
		    (wifi->tx_defaults_applied & BIT(1)))
			metadata |= BIT_ULL(60) | BIT_ULL(48);
		/* Bits 17:15 carry the 3-bit packet class (4 for normal data). */
		if (READ_ONCE(data_tx_native_prepare))
			metadata |= 4ULL << 15;
		put_unaligned_le64(metadata, descriptor + 16);
		put_unaligned_le64(2000000, descriptor + 24);
		if (wifi->data_tx_submitted == wifi->data_tx_completed)
			wifi->data_tx_completed_at = jiffies;
		wifi->data_tx_length[post] = skb->len;
		wifi->data_tx_busy[post] = true;
		wifi->data_tx_submitted++;
		dev_consume_skb_any(skb);
		post = next;
		posted = true;
	}
	if (posted || completed) {
		dma_wmb();
		if (posted)
			index_write(wifi, 3, 25, post);
		dma_wmb();
		writel(1, wifi->a.bar + 0x1030);
	}
	if (wifi->connected && !READ_ONCE(wifi->stopping) &&
	    !READ_ONCE(wifi->transport_paused) && netif_running(wifi->netdev) &&
	    skb_queue_len(&wifi->data_tx_queue) < DATA_TX_QUEUE_LIMIT)
		netif_wake_queue(wifi->netdev);
}

/* Record a reply to the statistics command 0xb0001. */
static void record_stats_reply(struct centauri_wifi *wifi, const u8 *packet,
			       u16 size, u16 status)
{
	wifi->stats_reply_size = size;
	wifi->stats_tag = size >= 28 ? get_unaligned_le16(packet + 24) : 0;
	wifi->stats_length = size >= 28 ? get_unaligned_le16(packet + 26) : 0;
	if (!status && wifi->stats_tag == 0xf928 &&
	    (wifi->stats_length == 752 || wifi->stats_length == 888) &&
	    size >= 28 + wifi->stats_length) {
		/* Native firmware returns the older 752-byte counter record. */
		memset(wifi->station_stats, 0, sizeof(wifi->station_stats));
		memcpy(wifi->station_stats, packet + 28, wifi->stats_length);
		/* A later reply can change stats_length; keep this record's own. */
		wifi->station_stats_len = wifi->stats_length;
		wifi->station_stats_valid = true;
	}
	if (!status && wifi->stats_tag == 0xf921 &&
	    wifi->stats_length == sizeof(wifi->rx_crypto_stats) &&
	    size >= 28 + sizeof(wifi->rx_crypto_stats)) {
		memcpy(wifi->rx_crypto_stats, packet + 28, sizeof(wifi->rx_crypto_stats));
		wifi->rx_crypto_stats_valid = true;
	}
	if (!status && wifi->stats_tag == 0xf91b &&
	    wifi->stats_length == sizeof(wifi->dot11_mib_stats) &&
	    size >= 28 + sizeof(wifi->dot11_mib_stats)) {
		memcpy(wifi->dot11_mib_stats, packet + 28, sizeof(wifi->dot11_mib_stats));
		wifi->dot11_mib_stats_valid = true;
	}
	if (!status && wifi->stats_tag == 0xf920 &&
	    wifi->stats_length == sizeof(wifi->tx_power_report) &&
	    size >= 28 + sizeof(wifi->tx_power_report)) {
		memcpy(wifi->tx_power_report, packet + 28, sizeof(wifi->tx_power_report));
		wifi->tx_power_report_valid = true;
	}
}

struct cen_tlv_hdr {
	__le16 tag;
	__le16 length;
} __packed;

struct cen_sta_info_report {
	u8 reserved[2];
	s8 noise_dbm;
} __packed;
static_assert(sizeof(struct cen_sta_info_report) == 3);

struct cen_chip_sta_counters {
	__le32 reserved0[2];
	__le32 tx_retrans;
	__le32 tx_fail_retry;
	__le32 tx_fail_drop;
	__le32 reserved1[4];
	__le32 tx_rate_kbps;
	__le32 reserved2[34];
	__le32 rx_rate_kbps;
} __packed;
static_assert(offsetof(struct cen_chip_sta_counters, tx_retrans) == 0x08);
static_assert(offsetof(struct cen_chip_sta_counters, tx_fail_retry) == 0x0c);
static_assert(offsetof(struct cen_chip_sta_counters, tx_fail_drop) == 0x10);
static_assert(offsetof(struct cen_chip_sta_counters, tx_rate_kbps) == 0x24);
static_assert(offsetof(struct cen_chip_sta_counters, rx_rate_kbps) == 0xb0);
static_assert(sizeof(struct cen_chip_sta_counters) == 0xb4);

struct cen_cca_chan_slot {
	u8 channel;
	u8 band_flags;
	u8 bw_primary;
	u8 reserved;
} __packed;
static_assert(sizeof(struct cen_cca_chan_slot) == 4);

#define CEN_CCA_SLOTS 12

struct cen_cca_report {
	__le32 reserved0[CEN_CCA_SLOTS];
	__le32 obss_us[CEN_CCA_SLOTS];
	__le32 interference_us[CEN_CCA_SLOTS];
	__le32 tx_primary_us[CEN_CCA_SLOTS];
	__le32 reserved1[CEN_CCA_SLOTS];
	__le32 rx_self_us[CEN_CCA_SLOTS];
	__le32 rx_other_us[CEN_CCA_SLOTS];
	__le32 tx_secondary_us[CEN_CCA_SLOTS];
	__le32 reserved2[3];
	struct cen_cca_chan_slot channels[CEN_CCA_SLOTS];
	__le64 dur_awake_us;
	__le64 dur_sleep_us;
} __packed;
static_assert(sizeof(struct cen_cca_report) == 0x1cc);

struct cen_tx_power_record {
	u8 reserved0[14];
	u8 flags[2];
	u8 power_qdbm;
	u8 preamble;
	u8 reserved1[2];
	u8 mcs;
	u8 nss_minus1;
	u8 reserved2[2];
	u8 bw;
	u8 reserved3[2];
	u8 gi;
} __packed;
static_assert(sizeof(struct cen_tx_power_record) == 28);

struct cen_beacon_stats_report {
	__le32 reserved0[46];
	__le32 missed_beacons;
	__le32 reserved1[32];
	__le32 rx_beacons;
} __packed;
static_assert(offsetof(struct cen_beacon_stats_report, missed_beacons) == 0xb8);
static_assert(offsetof(struct cen_beacon_stats_report, rx_beacons) == 0x13c);
static_assert(sizeof(struct cen_beacon_stats_report) == 0x140);

struct cen_phy_dump_record {
	__le32 reserved0[2];
	__le32 noise_chain[2];
	__le32 reserved1[2];
	__le32 rssi_chain[2];
	__le32 reserved2;
	__le32 valid;
} __packed;
static_assert(sizeof(struct cen_phy_dump_record) == 40);

#define CEN_PHY_DUMP_RECORDS_PER_CORE 10

struct cen_fw_version_hdr {
	__le32 reserved;
	__le16 api_major;
	__le16 api_minor;
} __packed;
static_assert(sizeof(struct cen_fw_version_hdr) == 8);

static const void *cen_find_tlv(const u8 *payload, size_t size, u16 wanted_tag,
				u16 *out_len)
{
	size_t pos = 0;

	while (pos + sizeof(struct cen_tlv_hdr) <= size) {
		const struct cen_tlv_hdr *hdr = (const void *)(payload + pos);
		u16 tag = le16_to_cpu(hdr->tag);
		u16 length = le16_to_cpu(hdr->length);

		if (length > size - pos - sizeof(*hdr))
			break;
		if (tag == wanted_tag) {
			*out_len = length;
			return payload + pos + sizeof(*hdr);
		}
		pos += sizeof(*hdr) + length;
	}
	return NULL;
}

static bool sta_info_noise_read(const u8 *payload, size_t size, s8 *noise)
{
	const struct cen_sta_info_report *rep;
	u16 length;

	rep = cen_find_tlv(payload, size, 0xf908, &length);
	if (!rep || length < sizeof(*rep))
		return false;
	if (rep->noise_dbm < -120 || rep->noise_dbm > -40)
		return false;
	*noise = rep->noise_dbm;
	return true;
}

static bool chip_sta_counters_read(const u8 *payload, size_t size, u32 *retrans,
				   u32 *tx_fail, bool *tx_fail_valid,
				   u32 *tx_rate_kbps, u32 *rx_rate_kbps)
{
	const struct cen_chip_sta_counters *cnt;
	u16 length;

	*tx_fail = 0;
	*tx_fail_valid = false;
	*tx_rate_kbps = 0;
	*rx_rate_kbps = 0;
	cnt = cen_find_tlv(payload, size, 0xf901, &length);
	if (!cnt || length < offsetofend(struct cen_chip_sta_counters, tx_retrans))
		return false;
	*retrans = le32_to_cpu(cnt->tx_retrans);
	if (length >= offsetofend(struct cen_chip_sta_counters, tx_fail_drop)) {
		*tx_fail = le32_to_cpu(cnt->tx_fail_retry) +
			   le32_to_cpu(cnt->tx_fail_drop);
		*tx_fail_valid = true;
	}
	if (length >= offsetofend(struct cen_chip_sta_counters, tx_rate_kbps))
		*tx_rate_kbps = le32_to_cpu(cnt->tx_rate_kbps);
	if (length >= offsetofend(struct cen_chip_sta_counters, rx_rate_kbps))
		*rx_rate_kbps = le32_to_cpu(cnt->rx_rate_kbps);
	return true;
}

static bool cca_survey_read(const u8 *payload, size_t size, u8 primary_chan,
			    u8 fw_band, u8 bw_primary, bool bw_valid,
			    u64 *active_ms, u64 *busy_ms, u64 *tx_ms)
{
	const struct cen_cca_report *cca;
	u64 dur0, dur1, active_us, busy_us, tx_us, act_ms;
	unsigned int i;
	int slot = -1;
	u16 length;

	cca = cen_find_tlv(payload, size, 0xf903, &length);
	if (!cca || length < sizeof(*cca))
		return false;
	for (i = 0; i < CEN_CCA_SLOTS; i++) {
		const struct cen_cca_chan_slot *ch = &cca->channels[i];

		if (ch->channel != primary_chan ||
		    (ch->band_flags & 3) != fw_band)
			continue;
		if (bw_valid && ch->bw_primary == bw_primary) {
			slot = i;
			break;
		}
		if (slot < 0)
			slot = i;
	}
	if (slot < 0)
		return false;
	dur0 = le64_to_cpu(cca->dur_awake_us);
	dur1 = le64_to_cpu(cca->dur_sleep_us);
	active_us = dur0 + dur1;
	if (active_us < dur0)
		return false;
	act_ms = div_u64(active_us, 1000);
	if (!act_ms)
		return false;
	tx_us = (u64)le32_to_cpu(cca->tx_primary_us[slot]) +
		le32_to_cpu(cca->tx_secondary_us[slot]);
	busy_us = (u64)le32_to_cpu(cca->obss_us[slot]) +
		  le32_to_cpu(cca->interference_us[slot]) +
		  le32_to_cpu(cca->rx_self_us[slot]) +
		  le32_to_cpu(cca->rx_other_us[slot]) +
		  tx_us;
	*active_ms = act_ms;
	*busy_ms = min(div_u64(busy_us, 1000), act_ms);
	*tx_ms = min(div_u64(tx_us, 1000), *busy_ms);
	return true;
}

static bool tx_power_rate_read(const u8 *payload, size_t size,
			       struct rate_info *rate, bool *rate_valid,
			       int *tx_power_dbm, bool *tx_power_valid)
{
	static const u8 preamble_mode[13] = {
		0, 1, 2, 3, 4, 5, 5, 5, 5, 4, 4, 6, 6,
	};
	static const u16 legacy_rates[16] = {
		[0] = 10,   [1] = 20,   [2] = 55,   [3] = 110,
		[8] = 480,  [9] = 240,  [10] = 120, [11] = 60,
		[12] = 540, [13] = 360, [14] = 180, [15] = 90,
	};
	static const u8 bw_map[4] = {
		RATE_INFO_BW_20,
		RATE_INFO_BW_40,
		RATE_INFO_BW_80,
		RATE_INFO_BW_160,
	};
	const u8 *v;
	size_t stride;
	int best_mode = -1;
	unsigned int i;
	u16 length;

	*rate_valid = false;
	*tx_power_valid = false;
	v = cen_find_tlv(payload, size, 0xf920, &length);
	if (!v || (length != 280 && length != 320))
		return false;
	stride = length / 10;

	for (i = 0; i < 5; i++) {
		const struct cen_tx_power_record *rec = (const void *)(v + i * stride);
		u8 qdb = rec->power_qdbm;
		u8 pre = rec->preamble;
		u8 raw_mcs = rec->mcs;
		u8 nss = rec->nss_minus1 + 1;
		u8 hw_bw = rec->bw;
		u8 hw_gi = rec->gi;
		struct rate_info cand = {};
		u8 mode;

		if (!(rec->flags[0] | rec->flags[1] | qdb | pre | raw_mcs |
		      rec->nss_minus1 | hw_bw | hw_gi))
			continue;
		if (qdb && qdb < 128 && !*tx_power_valid) {
			*tx_power_dbm = qdb / 4;
			*tx_power_valid = true;
		}
		if (pre >= ARRAY_SIZE(preamble_mode))
			continue;
		mode = preamble_mode[pre];
		if ((int)mode <= best_mode)
			continue;
		if (mode <= 2) {
			if (raw_mcs >= ARRAY_SIZE(legacy_rates) ||
			    !legacy_rates[raw_mcs])
				continue;
			cand.legacy = legacy_rates[raw_mcs];
		} else if (hw_bw >= ARRAY_SIZE(bw_map)) {
			continue;
		} else if (mode == 3) {
			if (hw_bw > 1)
				continue;
			if (raw_mcs <= 7 && nss >= 1 && nss <= 4)
				cand.mcs = (nss - 1) * 8 + raw_mcs;
			else if (raw_mcs <= 31)
				cand.mcs = raw_mcs;
			else
				continue;
			cand.bw = bw_map[hw_bw];
			cand.flags = RATE_INFO_FLAGS_MCS;
			if (hw_gi)
				cand.flags |= RATE_INFO_FLAGS_SHORT_GI;
		} else if (mode == 4) {
			if (raw_mcs > 11 || nss < 1 || nss > 4)
				continue;
			cand.bw = bw_map[hw_bw];
			cand.flags = RATE_INFO_FLAGS_VHT_MCS;
			cand.mcs = raw_mcs;
			cand.nss = nss;
			if (hw_gi)
				cand.flags |= RATE_INFO_FLAGS_SHORT_GI;
		} else if (mode == 5) {
			if (raw_mcs > 11 || nss < 1 || nss > 4 ||
			    hw_gi > NL80211_RATE_INFO_HE_GI_3_2)
				continue;
			cand.bw = bw_map[hw_bw];
			cand.flags = RATE_INFO_FLAGS_HE_MCS;
			cand.mcs = raw_mcs;
			cand.nss = nss;
			cand.he_gi = hw_gi;
		} else {
			if (raw_mcs > 13 || nss < 1 || nss > 4 ||
			    hw_gi > NL80211_RATE_INFO_EHT_GI_3_2)
				continue;
			cand.bw = bw_map[hw_bw];
			cand.flags = RATE_INFO_FLAGS_EHT_MCS;
			cand.mcs = raw_mcs;
			cand.nss = nss;
			cand.eht_gi = hw_gi;
		}
		best_mode = mode;
		*rate = cand;
		*rate_valid = true;
		if (qdb && qdb < 128) {
			*tx_power_dbm = qdb / 4;
			*tx_power_valid = true;
		}
	}
	return *rate_valid || *tx_power_valid;
}

static bool beacon_stats_read(const u8 *payload, size_t size,
			      u32 *rx_beacons, u32 *missed_beacons)
{
	const struct cen_beacon_stats_report *bcn;
	u16 length;

	bcn = cen_find_tlv(payload, size, 0xf915, &length);
	if (!bcn || length < sizeof(*bcn))
		return false;
	*missed_beacons = le32_to_cpu(bcn->missed_beacons);
	*rx_beacons = le32_to_cpu(bcn->rx_beacons);
	return true;
}

static bool phy_dump_read(const u8 *payload, size_t size, u8 fw_band,
			  s8 chain_rssi[2], u8 *chains,
			  s8 *noise, bool *noise_valid)
{
	const struct cen_phy_dump_record *records;
	unsigned int core;
	u16 length;

	*chains = 0;
	*noise_valid = false;
	records = cen_find_tlv(payload, size, 0xf923, &length);
	if (!records || length < 2 * CEN_PHY_DUMP_RECORDS_PER_CORE * sizeof(*records))
		return false;

	core = fw_band == 0 ? 0 : 1;
	for (;;) {
		const struct cen_phy_dump_record *base =
			records + core * CEN_PHY_DUMP_RECORDS_PER_CORE;
		u64 sum0 = 0, sum1 = 0, nsum0 = 0, nsum1 = 0;
		u32 cnt0 = 0, cnt1 = 0, ncnt0 = 0, ncnt1 = 0;
		s32 ntotal = 0;
		u32 nvalid = 0;
		unsigned int i;

		for (i = 0; i < CEN_PHY_DUMP_RECORDS_PER_CORE; i++) {
			const struct cen_phy_dump_record *rec = &base[i];
			u32 n0, n1, r0, r1;

			if (!le32_to_cpu(rec->valid))
				continue;
			n0 = le32_to_cpu(rec->noise_chain[0]);
			n1 = le32_to_cpu(rec->noise_chain[1]);
			r0 = le32_to_cpu(rec->rssi_chain[0]);
			r1 = le32_to_cpu(rec->rssi_chain[1]);
			if (n0) {
				nsum0 += n0;
				ncnt0++;
			}
			if (n1) {
				nsum1 += n1;
				ncnt1++;
			}
			if (r0) {
				sum0 += r0;
				cnt0++;
			}
			if (r1) {
				sum1 += r1;
				cnt1++;
			}
		}
		if (cnt0) {
			s64 val = -(s64)div_u64(sum0, cnt0 * 2);

			if (val >= -120 && val <= -1) {
				chain_rssi[0] = val;
				*chains |= BIT(0);
			}
		}
		if (cnt1) {
			s64 val = -(s64)div_u64(sum1, cnt1 * 2);

			if (val >= -120 && val <= -1) {
				chain_rssi[1] = val;
				*chains |= BIT(1);
			}
		}
		if (ncnt0) {
			s64 val = -(s64)div_u64(nsum0, ncnt0 * 2);

			if (val >= -120 && val <= -40) {
				ntotal += val;
				nvalid++;
			}
		}
		if (ncnt1) {
			s64 val = -(s64)div_u64(nsum1, ncnt1 * 2);

			if (val >= -120 && val <= -40) {
				ntotal += val;
				nvalid++;
			}
		}
		if (nvalid) {
			*noise = ntotal / (s32)nvalid;
			*noise_valid = true;
		}
		if (*chains || *noise_valid || !core)
			break;
		core = 0;
	}
	return *chains != 0 || *noise_valid;
}

static bool fw_version_read(const u8 *payload, size_t size,
			    char *out, size_t out_size)
{
	const struct cen_fw_version_hdr *hdr = (const void *)payload;
	u16 api_major, api_minor;
	size_t i, len = 0;
	char fw[40];

	if (!out_size || size < sizeof(*hdr))
		return false;
	api_major = le16_to_cpu(hdr->api_major);
	api_minor = le16_to_cpu(hdr->api_minor);
	if (size > sizeof(*hdr)) {
		size_t max_len = min_t(size_t, size - sizeof(*hdr), sizeof(fw) - 1);

		for (i = 0; i < max_len && payload[sizeof(*hdr) + i]; i++) {
			u8 c = payload[sizeof(*hdr) + i];

			if (c < 0x20 || c > 0x7e)
				continue;
			if (!len && c == ' ')
				continue;
			fw[len++] = c;
		}
		while (len && fw[len - 1] == ' ')
			len--;
		fw[len] = '\0';
	}
	if (len) {
		snprintf(out, out_size, "%s", fw);
		return true;
	}
	if (!api_major && !api_minor)
		return false;
	snprintf(out, out_size, "api %u.%u", api_major, api_minor);
	return true;
}

static bool country_code_valid(const u8 *code, size_t size)
{
	if (size < 2)
		return false;
	if (code[0] < 'A' || code[0] > 'Z' || code[1] < 'A' || code[1] > 'Z')
		return false;
	if ((code[0] == 'X' && (code[1] == 'Z' || code[1] == 'X')) ||
	    (code[0] == 'Z' && code[1] == 'Z'))
		return false;
	return true;
}

static void disconnect_reason_decode(const u8 *packet, size_t size,
				     u16 *reason, bool *local)
{
	if (size >= 31) {
		u8 code = packet[24];

		if (code) {
			*reason = code;
			*local = false;
		} else {
			*reason = WLAN_REASON_UNSPECIFIED;
			*local = true;
		}
	} else {
		*reason = WLAN_REASON_UNSPECIFIED;
		*local = false;
	}
}

static bool link_channel_decode(struct wiphy *wiphy, u8 center_chan,
				u8 band_flags, u8 bw_primary,
				struct cfg80211_chan_def *chandef)
{
	static const s8 primary_offset[15] = {
		0,
		2, -2,
		6, 2, -2, -6,
		14, 10, 6, 2, -2, -6, -10, -14,
	};
	enum nl80211_chan_width width;
	enum nl80211_band band;
	int primary, primary_freq, center_freq;
	struct ieee80211_channel *chan;

	if (!center_chan || bw_primary >= ARRAY_SIZE(primary_offset))
		return false;
	switch (band_flags & 3) {
	case 0:
		band = NL80211_BAND_2GHZ;
		break;
	case 1:
		band = NL80211_BAND_5GHZ;
		break;
	case 2:
		band = NL80211_BAND_6GHZ;
		break;
	default:
		return false;
	}
	if (bw_primary == 0)
		width = NL80211_CHAN_WIDTH_20;
	else if (bw_primary <= 2)
		width = NL80211_CHAN_WIDTH_40;
	else if (bw_primary <= 6)
		width = NL80211_CHAN_WIDTH_80;
	else
		width = NL80211_CHAN_WIDTH_160;

	primary = (int)center_chan + primary_offset[bw_primary];
	if (primary <= 0 || primary > 255)
		return false;
	primary_freq = ieee80211_channel_to_frequency(primary, band);
	center_freq = ieee80211_channel_to_frequency(center_chan, band);
	if (!primary_freq || !center_freq)
		return false;
	chan = ieee80211_get_channel(wiphy, primary_freq);
	if (!chan)
		return false;

	memset(chandef, 0, sizeof(*chandef));
	chandef->chan = chan;
	chandef->width = width;
	chandef->center_freq1 = center_freq;
	return cfg80211_chandef_valid(chandef);
}

static void record_link_status_reply(struct centauri_wifi *wifi,
				     const u8 *packet, u16 size)
{
	/* Interface status reply has 8 fixed bytes after the 24-byte header. */
	unsigned int pos = 32;

	wifi->link_status_size = size;
	while (pos + 4 <= size) {
		u16 tag = get_unaligned_le16(packet + pos);
		u16 length = get_unaligned_le16(packet + pos + 2);
		const u8 *value = packet + pos + 4;
		unsigned int i;

		if (length > size - pos - 4)
			break;
		if (wifi->link_tlv_count < ARRAY_SIZE(wifi->link_tags)) {
			wifi->link_tags[wifi->link_tlv_count] = tag;
			wifi->link_lengths[wifi->link_tlv_count++] = length;
		}
		if (tag == 268 && length >= 115) {
			s8 signal = (s8)value[1];

			if (signal) {
				wifi->station_signal = signal;
				wifi->station_signal_valid = true;
			}
			wifi->station_chandef_valid =
				link_channel_decode(wifi->wiphy, value[16],
						    value[17], value[18],
						    &wifi->station_chandef);
			if (wifi->station_chandef_valid)
				wifi->station_bw_primary = value[18];
		}
		/* MLO status report, TLVs 377/378: keep link count/channel/role only. */
		if (tag == 377 && length >= 64 && value[0] <= 3) {
			wifi->link_count = value[0];
			wifi->link_status_valid = true;
			for (i = 0; i < value[0]; i++) {
				u16 chanspec = get_unaligned_le16(value + 26 + 17 * i);

				wifi->link_prime[i] = value[25 + 17 * i];
				wifi->link_channel[i] = chanspec & 0xff;
				wifi->link_band[i] = (chanspec >> 14) & 3;
			}
		}
		if (tag == 378 && length >= 9) {
			wifi->emlsr_valid = true;
			wifi->emlsr_mode = value[0];
		}
		pos += 4 + length;
	}
}

static void drain_control(struct centauri_wifi *wifi, bool pm_command)
{
	unsigned int ring;
	bool posted = false;

	if (READ_ONCE(wifi->transport_paused) && !pm_command)
		return;
	for (ring = 2; ring <= 4; ring++) {
		unsigned int budget = ring_size[ring];

		while (budget--) {
			u16 slot = wifi->rx_next[ring], size, status, sequence;
			u32 command;
			u8 *descriptor = wifi->a.window + TR_BASE + (ring - 1) * RING_SPAN + slot * 16;
			u8 *packet = wifi->a.window + rx_buffer[ring] + slot * BUFFER_SIZE;

			if (!(READ_ONCE(descriptor[15]) & 4))
				break;
			dma_rmb();
			size = get_unaligned_le16(packet + 0x10);
			status = get_unaligned_le16(packet + 0xe);
			sequence = get_unaligned_le16(packet + 0x12);
			command = get_unaligned_le32(packet + 0x14) & ~BIT(30);
			wifi->rx_seen[ring]++;
			wifi->rx_next[ring] = (slot + 1) % ring_size[ring];
			if (size < 24 || size > BUFFER_SIZE)
				goto refill;
			if (ring == 4) {
				report_beacon(wifi, packet + 24, size - 24);
			} else if (command & BIT(31)) {
				if (command == 0x80000030)
					wifi->channels_seen++;
				if (command == 0x80000005)
					wifi->scans_done++;
				if (command == 0x80000000 && wifi->connecting) {
					wifi->connect_event = true;
					wifi->connect_status = status;
				}
				if (command == 0x80000001) {
					wifi->associated = status == 0;
					dev_info(&wifi->control->pdev->dev, "Wi-Fi association event: status=%u\n", status);
					if (eapol_after_assoc && wifi->associated && wifi->connecting &&
					    !wifi->connect_event) {
						int probe_ret = transmit_eapol_start(wifi);

						eapol_after_assoc = false;
						dev_info(&wifi->control->pdev->dev,
							 "association-event EAPOL-Start: rc=%d\n", probe_ret);
					}
				}
				/* ACI event 0x50 carries fixed, non-secret supplicant
				 * diagnostics: result, phase and handshake timestamps.
				 * Do not dump the event or any EAPOL/key material.
				 */
				if (command == 0x80000050 && size >= 64)
					dev_info(&wifi->control->pdev->dev,
						 "Wi-Fi supplicant: result=%u phase=%u times=%u/%u/%u/%u/%u\n",
						 get_unaligned_le32(packet + 0x18), packet[0x1c],
						 get_unaligned_le32(packet + 0x24),
						 get_unaligned_le32(packet + 0x28),
						 get_unaligned_le32(packet + 0x2c),
						 get_unaligned_le32(packet + 0x30),
						 get_unaligned_le32(packet + 0x34));
				if (command == 0x80000003) {
					disconnect_reason_decode(packet, size,
								 &wifi->disconnect_reason,
								 &wifi->disconnect_local);
					wifi->disconnect_event = true;
				}
				if (command == 0x80000004)
					wifi->disconnect_event = true;
				if (wifi->connecting && command != 0x80000000 &&
				    command != 0x80000001 && command != 0x80000030 &&
				    command != 0x80000005 && command != 0x80000050)
					dev_info_ratelimited(&wifi->control->pdev->dev,
						"Wi-Fi event: id=%#x status=%u size=%u\n",
						command, status, size);
			} else {
				wifi->reply_valid = true;
				wifi->reply_sequence = sequence;
				wifi->reply_status = status;
				wifi->reply_command = command;
				if (wifi->reply_buffer && sequence == wifi->reply_capture) {
					wifi->reply_size = size - 24;
					memcpy(wifi->reply_buffer, packet + 24,
					       min(wifi->reply_size, wifi->reply_capacity));
				}
				if (command == 10) {
					wifi->country_valid = !status && size >= 27;
					if (wifi->country_valid)
						memcpy(wifi->country, packet + 24, 3);
				}
				if (command == 56) {
					wifi->tx_profile_valid = !status && size >= 34;
					if (wifi->tx_profile_valid)
						memcpy(wifi->tx_profile, packet + 24, sizeof(wifi->tx_profile));
				}
				if (command == 20 && !status)
					record_link_status_reply(wifi, packet, size);
				if (command == 0xb0001)
					record_stats_reply(wifi, packet, size, status);
				if (command == 0xf0025) {
					wifi->pmk_seen = !status && size >= 60;
					wifi->pmk_matches = wifi->pmk_seen &&
						get_unaligned_le16(packet + 26) == 32 &&
						!memcmp(packet + 28, wifi->expected_pmk, 32);
					memzero_explicit(packet + 24, size - 24);
				}
			}
		refill:
			post_receive(wifi, ring, wifi->rx_head[ring]);
			wifi->rx_head[ring] = (wifi->rx_head[ring] + 1) % ring_size[ring];
			dma_wmb();
			index_write(wifi, 3, ring, wifi->rx_head[ring]);
			posted = true;
		}
	}
	completions(wifi);
	if (posted)
		writel(1, wifi->a.bar + 0x1004);
	if (!READ_ONCE(wifi->transport_paused)) {
		drain_data_rx(wifi);
		drain_data_tx(wifi);
	}
}

static void drain(struct centauri_wifi *wifi)
{
	drain_control(wifi, false);
}

static int command_reply_control(struct centauri_wifi *wifi, u32 id, const void *payload,
				 size_t size, void *reply, size_t *reply_size,
				 bool pm_command)
{
	u16 slot = wifi->command_head, sequence = ++wifi->sequence;
	u8 *packet, *descriptor;
	unsigned long deadline = jiffies + msecs_to_jiffies(1500);
	int ret;

	if (READ_ONCE(wifi->transport_paused) && !pm_command)
		return -EBUSY;
	if (size + 24 > BUFFER_SIZE)
		return -EMSGSIZE;
	if ((slot + 1) % ring_size[1] == index_read(wifi, 1, 1)) {
		centauri_recovery_request(wifi->control, "command ring full");
		return -ENOSPC;
	}
	packet = wifi->a.window + TX_BUFFER + slot * BUFFER_SIZE;
	memset(packet, 0, 24 + size);
	put_unaligned_le16(24 + size, packet + 0x10);
	put_unaligned_le16(sequence, packet + 0x12);
	put_unaligned_le32(id, packet + 0x14);
	if (size)
		memcpy(packet + 24, payload, size);
	descriptor = wifi->a.window + TR_BASE + slot * 16;
	put_unaligned_le32(1 | ((24 + size) << 8), descriptor);
	put_unaligned_le64(wifi->a.window_dma + TX_BUFFER + slot * BUFFER_SIZE, descriptor + 4);
	put_unaligned_le32(slot + 1, descriptor + 12);
	dma_wmb();
	wifi->command_head = (slot + 1) % ring_size[1];
	index_write(wifi, 3, 1, wifi->command_head);
	wifi->reply_valid = false;
	wifi->reply_buffer = reply;
	wifi->reply_capacity = reply ? *reply_size : 0;
	wifi->reply_capture = sequence;
	wifi->reply_size = 0;
	dma_wmb();
	writel(1, wifi->a.bar + 0x1004);
	do {
		drain_control(wifi, pm_command);
		if (readl(wifi->a.bar + 8) != 2) {
			dev_warn_ratelimited(&wifi->control->pdev->dev,
					     "Alpha execution stage %#x\n",
					     readl(wifi->a.bar + 8));
			centauri_recovery_request(wifi->control, "command: Alpha stage");
			ret = -EIO;
			goto out;
		}
		/* Bit 30 is more-data-pending, not part of the command identity.
		 * Scan/connect replies set it before their later completion event.
		 */
		if (wifi->reply_valid && wifi->reply_sequence == sequence &&
		    (wifi->reply_command & ~BIT(30)) == id) {
			if (wifi->reply_status)
				dev_dbg(&wifi->control->pdev->dev,
					"Wi-Fi command %#x rejected: firmware_status=%u\n",
					id, wifi->reply_status);
			if (reply)
				*reply_size = wifi->reply_size;
			ret = wifi->reply_status ? -EREMOTEIO : 0;
			goto out;
		}
		usleep_range(500, 1000);
	} while (time_before(jiffies, deadline));
	dev_err(&wifi->control->pdev->dev, "Wi-Fi command %#x timed out (rx=%u)\n", id, wifi->rx_seen[2]);
	centauri_recovery_request(wifi->control, "command timeout");
	ret = -ETIMEDOUT;
out:
	wifi->reply_buffer = NULL;
	return ret;
}

static int command_control(struct centauri_wifi *wifi, u32 id, const void *payload,
			   size_t size, bool pm_command)
{
	return command_reply_control(wifi, id, payload, size, NULL, NULL, pm_command);
}

static int command_reply(struct centauri_wifi *wifi, u32 id, const void *payload,
			 size_t size, void *reply, size_t *reply_size)
{
	return command_reply_control(wifi, id, payload, size, reply, reply_size, false);
}

/*
 * The firmware has stopped: Alpha left execution stage 2, or the command ring stays full
 * (a running firmware drains each command before the next is queued). Not while the
 * transport is paused for a system sleep.
 */
bool centauri_wifi_stalled(struct centauri *c)
{
	struct centauri_wifi *wifi = c->wifi;

	if (!wifi || READ_ONCE(wifi->transport_paused) || READ_ONCE(wifi->stopping))
		return false;
	if (readl(wifi->a.bar + 8) != 2)
		return true;
	return (READ_ONCE(wifi->command_head) + 1) % ring_size[1] == index_read(wifi, 1, 1);
}

static int command(struct centauri_wifi *wifi, u32 id, const void *payload, size_t size)
{
	return command_control(wifi, id, payload, size, false);
}

static int station_configure(struct centauri_wifi *wifi, const u8 *address)
{
	u8 start[5 + 4 + 3 * ETH_ALEN] = { 0, 0x0b, 0, 0, 0 };
	size_t start_size = 5;
	unsigned int slot;
	int ret;

	/* Optional diagnostic: bind all three firmware link slots to the station address. */
	if (station_link_addresses) {
		const struct centauri_board *board = centauri_board();

		put_unaligned_le16(board ? board->link_tlv : 385, start + 5);
		put_unaligned_le16(3 * ETH_ALEN, start + 7);
		for (slot = 0; slot < 3; slot++)
			ether_addr_copy(start + 9 + slot * ETH_ALEN, address);
		start_size = sizeof(start);
	}
	ret = command(wifi, 1, address, ETH_ALEN);
	if (!ret)
		ret = command(wifi, 2, start, start_size);
	return ret;
}

static void firmware_report(struct centauri_wifi *wifi, u32 id, u8 *buffer,
			    struct debugfs_blob_wrapper *blob)
{
	size_t size = REPLY_SIZE;
	int ret;

	ret = command_reply(wifi, id, NULL, 0, buffer, &size);
	if (ret) {
		dev_dbg(&wifi->control->pdev->dev,
			"Wi-Fi firmware report %u unavailable: %d (status=%u)\n",
			id, ret, wifi->reply_status);
		size = 0;
	} else if (size > REPLY_SIZE) {
		dev_warn(&wifi->control->pdev->dev,
			 "Wi-Fi command %u reply of %zu bytes truncated\n",
			 id, size);
		size = REPLY_SIZE;
	}
	blob->data = buffer;
	blob->size = size;
}

#define CHANNEL_PASSIVE BIT(4)
#define CHANNEL_RADAR BIT(6)

static const u8 *channel_entry(const u8 *list, size_t size, int band,
			       unsigned int width, unsigned int number)
{
	unsigned int count = size ? list[0] : 0, i, span = 2 * (1 << width) - 2;

	if (1 + 4 * count > size)
		count = size ? (size - 1) / 4 : 0;
	for (i = 0; i < count; i++) {
		const u8 *entry = list + 1 + 4 * i;

		if ((entry[1] & 3) != band || ((entry[1] >> 2) & 3) != width)
			continue;
		if (number + span >= entry[0] && number <= entry[0] + span)
			return entry;
	}
	return NULL;
}

/* Whether the list describes channels of this width in the band at all. */
static bool channel_width_listed(const u8 *list, size_t size, int band,
				 unsigned int width)
{
	unsigned int count = size ? list[0] : 0, i;

	if (1 + 4 * count > size)
		count = size ? (size - 1) / 4 : 0;
	for (i = 0; i < count; i++) {
		const u8 *entry = list + 1 + 4 * i;

		if ((entry[1] & 3) == band && ((entry[1] >> 2) & 3) == width)
			return true;
	}
	return false;
}

static void channels_apply(struct centauri_wifi *wifi)
{
	const u8 *list = wifi->hw_channels;
	size_t size = wifi->hw_channels_blob.size;
	unsigned int counted[3] = {}, i;

	if (!size)
		return;
	for (i = 0; i < CEN_CHANNELS; i++) {
		struct ieee80211_channel *channel = &wifi->channels[i];
		int band = firmware_band(channel->band);
		const u8 *entry = channel_entry(list, size, band, 0,
						channel->hw_value);

		if (!entry) {
			channel->flags |= IEEE80211_CHAN_DISABLED;
			continue;
		}
		if (entry[1] & CHANNEL_PASSIVE)
			channel->flags |= IEEE80211_CHAN_NO_IR;
		if (entry[1] & CHANNEL_RADAR)
			channel->flags |= IEEE80211_CHAN_RADAR;
		/*
		 * A list of 20 MHz entries only says nothing about wider
		 * channels; restrict a width only where the list covers it.
		 */
		if (channel_width_listed(list, size, band, 1) &&
		    !channel_entry(list, size, band, 1, channel->hw_value))
			channel->flags |= IEEE80211_CHAN_NO_HT40;
		if (channel_width_listed(list, size, band, 2) &&
		    !channel_entry(list, size, band, 2, channel->hw_value))
			channel->flags |= IEEE80211_CHAN_NO_80MHZ;
		if (channel_width_listed(list, size, band, 3) &&
		    !channel_entry(list, size, band, 3, channel->hw_value))
			channel->flags |= IEEE80211_CHAN_NO_160MHZ;
		if (!(channel->flags & IEEE80211_CHAN_DISABLED))
			counted[band]++;
	}
	dev_info(&wifi->control->pdev->dev,
		 "Wi-Fi firmware channels: 2.4 GHz %u, 5 GHz %u, 6 GHz %u (%u entries)\n",
		 counted[0], counted[1], counted[2], list[0]);
}

static bool band_caps_from_firmware(struct centauri_wifi *wifi)
{
	const u8 *caps = wifi->system_caps;
	const u8 *ht_elem = NULL, *vht_elem = NULL, *he_elem = NULL;
	size_t size = wifi->system_caps_blob.size, pos = 0, mcs;
	u32 vht_cap = 0;

	while (pos + 4 <= size) {
		u16 tag = get_unaligned_le16(caps + pos);
		u16 length = get_unaligned_le16(caps + pos + 2);
		const u8 *value = caps + pos + 4;

		if (length > size - pos - 4)
			break;
		if (tag == 262 &&
		    length >= 87 + 2 + sizeof(struct ieee80211_ht_cap) &&
		    value[87] == WLAN_EID_HT_CAPABILITY &&
		    value[88] == sizeof(struct ieee80211_ht_cap))
			ht_elem = value + 87;
		if (tag == WLAN_EID_VHT_CAPABILITY &&
		    length >= 2 + sizeof(struct ieee80211_vht_cap) &&
		    value[0] == WLAN_EID_VHT_CAPABILITY &&
		    value[1] == sizeof(struct ieee80211_vht_cap))
			vht_elem = value;
		if (tag == WLAN_EID_EXTENSION && length >= 3 + 6 + 11 + 4 &&
		    value[0] == WLAN_EID_EXTENSION && value[1] == length - 2 &&
		    value[2] == WLAN_EID_EXT_HE_CAPABILITY)
			he_elem = value;
		pos += 4 + length;
	}

	if (ht_elem) {
		struct ieee80211_sta_ht_cap ht_cap = {};
		const struct ieee80211_ht_cap *ht =
			(const struct ieee80211_ht_cap *)(ht_elem + 2);

		ht_cap.ht_supported = true;
		ht_cap.cap = le16_to_cpu(ht->cap_info);
		ht_cap.ampdu_factor =
			ht->ampdu_params_info & IEEE80211_HT_AMPDU_PARM_FACTOR;
		ht_cap.ampdu_density =
			FIELD_GET(IEEE80211_HT_AMPDU_PARM_DENSITY,
				  ht->ampdu_params_info);
		ht_cap.mcs = ht->mcs;
		wifi->bands[0].ht_cap = ht_cap;
		wifi->bands[1].ht_cap = ht_cap;
	}

	if (vht_elem) {
		struct ieee80211_sta_vht_cap *vht = &wifi->bands[1].vht_cap;

		vht_cap = get_unaligned_le32(vht_elem + 2);
		vht->vht_supported = true;
		vht->cap = vht_cap;
		memcpy(&vht->vht_mcs, vht_elem + 6, sizeof(vht->vht_mcs));
	}

	if (!he_elem)
		return false;

	{
		struct ieee80211_sta_he_cap base_he = {};
		unsigned int band;

		memcpy(&base_he.he_cap_elem, he_elem + 3,
		       sizeof(base_he.he_cap_elem));
		mcs = he_elem[1] - 1 - sizeof(base_he.he_cap_elem);
		memcpy(&base_he.he_mcs_nss_supp,
		       he_elem + 3 + sizeof(base_he.he_cap_elem),
		       min(mcs, sizeof(base_he.he_mcs_nss_supp)));
		if (mcs > sizeof(base_he.he_mcs_nss_supp))
			memcpy(base_he.ppe_thres,
			       he_elem + 3 + sizeof(base_he.he_cap_elem) +
				       sizeof(base_he.he_mcs_nss_supp),
			       min(mcs - sizeof(base_he.he_mcs_nss_supp),
				   sizeof(base_he.ppe_thres)));
		base_he.has_he = true;

		for (band = 0; band < 3; band++) {
			struct ieee80211_sband_iftype_data *data =
				&wifi->iftype_data[band];

			memset(data, 0, sizeof(*data));
			data->types_mask = BIT(NL80211_IFTYPE_STATION);
			data->he_cap = base_he;
			if (band == 0) {
				data->he_cap.he_cap_elem.phy_cap_info[0] &=
					IEEE80211_HE_PHY_CAP0_CHANNEL_WIDTH_SET_40MHZ_IN_2G;
				data->he_cap.he_mcs_nss_supp.rx_mcs_160 =
					cpu_to_le16(0xffff);
				data->he_cap.he_mcs_nss_supp.tx_mcs_160 =
					cpu_to_le16(0xffff);
				data->he_cap.he_mcs_nss_supp.rx_mcs_80p80 =
					cpu_to_le16(0xffff);
				data->he_cap.he_mcs_nss_supp.tx_mcs_80p80 =
					cpu_to_le16(0xffff);
			} else {
				data->he_cap.he_cap_elem.phy_cap_info[0] &=
					~IEEE80211_HE_PHY_CAP0_CHANNEL_WIDTH_SET_40MHZ_IN_2G;
			}
			if (band == 2) {
				u32 ampdu_exp = u32_get_bits(vht_cap,
					IEEE80211_VHT_CAP_MAX_A_MPDU_LENGTH_EXPONENT_MASK);
				u16 capa;

				capa = u16_encode_bits(ampdu_exp,
						       IEEE80211_HE_6GHZ_CAP_MAX_AMPDU_LEN_EXP) |
				       u16_encode_bits(vht_cap & IEEE80211_VHT_CAP_MAX_MPDU_MASK,
						       IEEE80211_HE_6GHZ_CAP_MAX_MPDU_LEN) |
				       u16_encode_bits(WLAN_HT_CAP_SM_PS_DISABLED,
						       IEEE80211_HE_6GHZ_CAP_SM_PS);
				if (vht_cap & IEEE80211_VHT_CAP_RX_ANTENNA_PATTERN)
					capa |= IEEE80211_HE_6GHZ_CAP_RX_ANTPAT_CONS;
				if (vht_cap & IEEE80211_VHT_CAP_TX_ANTENNA_PATTERN)
					capa |= IEEE80211_HE_6GHZ_CAP_TX_ANTPAT_CONS;
				data->he_6ghz_capa.capa = cpu_to_le16(capa);
			}
			_ieee80211_set_sband_iftype_data(&wifi->bands[band],
							 data, 1);
		}
	}
	return true;
}

static void bands_init(struct centauri_wifi *wifi)
{
	unsigned int i, n = 0, c;

	for (i = 0; i < ARRAY_SIZE(band_plan); i++) {
		enum nl80211_band band = band_plan[i].band;
		struct ieee80211_supported_band *sband =
			&wifi->bands[firmware_band(band)];

		if (!sband->channels)
			sband->channels = wifi->channels + n;
		for (c = band_plan[i].first; c <= band_plan[i].last;
		     c += band_plan[i].step) {
			wifi->channels[n].band = band;
			wifi->channels[n].hw_value = c;
			wifi->channels[n].center_freq =
				ieee80211_channel_to_frequency(c, band);
			wifi->channels[n].max_power = 20;
			sband->n_channels++;
			n++;
		}
		sband->band = band;
		sband->bitrates = band == NL80211_BAND_2GHZ ? rates : rates + 4;
		sband->n_bitrates = band == NL80211_BAND_2GHZ ?
					    ARRAY_SIZE(rates) :
					    ARRAY_SIZE(rates) - 4;
	}
	wifi->wiphy->bands[NL80211_BAND_2GHZ] = &wifi->bands[0];
	wifi->wiphy->bands[NL80211_BAND_5GHZ] = &wifi->bands[1];
	if (band_caps_from_firmware(wifi)) {
		wifi->wiphy->bands[NL80211_BAND_6GHZ] = &wifi->bands[2];
	} else {
		dev_info(&wifi->control->pdev->dev,
			 "Wi-Fi: no HE capabilities from firmware; 6 GHz band disabled\n");
	}
}

static int radio_start(struct centauri_wifi *wifi)
{
	size_t size = sizeof(wifi->stats_reply);
	unsigned int ring, slot;
	int ret;

	for (ring = 1; ring <= 4; ring++) {
		ret = open_pair(wifi, ring);
		if (ret)
			return ret;
	}
	for (ring = 2; ring <= 4; ring++) {
		for (slot = 0; slot < ring_size[ring] - 1; slot++)
			post_receive(wifi, ring, slot);
		wifi->rx_head[ring] = ring_size[ring] - 1;
		dma_wmb();
		index_write(wifi, 3, ring, ring_size[ring] - 1);
	}
	dma_wmb();
	writel(1, wifi->a.bar + 0x1004);
	msleep(400);
	ret = command_reply(wifi, 0, NULL, 0, wifi->stats_reply, &size);
	if (!ret) {
		fw_version_read(wifi->stats_reply,
				min(size, sizeof(wifi->stats_reply)),
				wifi->wiphy->fw_version,
				sizeof(wifi->wiphy->fw_version));
		ret = station_configure(wifi, wifi->netdev->dev_addr);
	}
	if (ret)
		return ret;
	msleep(1000);
	firmware_report(wifi, 19, wifi->hw_channels, &wifi->hw_channels_blob);
	firmware_report(wifi, 8, wifi->system_caps, &wifi->system_caps_blob);
	wifi->configured = true;
	if (wifi->wiphy->fw_version[0])
		dev_info(&wifi->control->pdev->dev,
			 "Wi-Fi command rings ready; interface started (firmware %s)\n",
			 wifi->wiphy->fw_version);
	else
		dev_info(&wifi->control->pdev->dev,
			 "Wi-Fi command rings ready; interface started\n");
	return 0;
}

static void append_tlv(u8 *payload, size_t *size, u16 tag, const void *data, u16 length);

#define CEN_SCAN_CHANNELS_MAX 59
#define CEN_SCAN_SSIDS_MAX 4
#define SCAN_STATUS_INVALID_HANDLE 342
#define SCAN_ACTIVE_MIN_MS 40
#define SCAN_LISTEN_MS 110

static bool scan_transport_stopped(struct centauri_wifi *wifi)
{
	return READ_ONCE(wifi->stopping) ||
	       READ_ONCE(wifi->transport_paused) ||
	       readl(wifi->a.bar + 8) != 2;
}

static int scan_wait_stop(struct centauri_wifi *wifi, u32 done)
{
	unsigned long deadline = jiffies + msecs_to_jiffies(1000);

	do {
		if (scan_transport_stopped(wifi))
			return -EIO;
		drain(wifi);
		if (wifi->scans_done != done)
			return 0;
		mutex_unlock(&wifi->lock);
		usleep_range(1000, 2000);
		mutex_lock(&wifi->lock);
	} while (time_before(jiffies, deadline));
	if (scan_transport_stopped(wifi))
		return -EIO;
	drain(wifi);
	return wifi->scans_done != done ? 0 : -ETIMEDOUT;
}

static int scan_stop_chunk_id(struct centauri_wifi *wifi, u8 scan_id, u32 done)
{
	int ret;

	if (scan_transport_stopped(wifi))
		return -EIO;
	ret = scan_id ? command(wifi, 65537, &scan_id, sizeof(scan_id)) :
			command(wifi, 65537, NULL, 0);
	if (wifi->scans_done != done)
		return 0;
	if (ret == -EREMOTEIO && wifi->reply_status == SCAN_STATUS_INVALID_HANDLE)
		return 0;
	if (ret)
		return ret;
	return scan_wait_stop(wifi, done);
}

static int scan_stop_chunk(struct centauri_wifi *wifi, u32 done)
{
	int ret;

	if (scan_transport_stopped(wifi))
		return -EIO;
	ret = command(wifi, 65537, NULL, 0);
	if (wifi->scans_done != done)
		return 0;
	if (ret == -EREMOTEIO && wifi->reply_status == SCAN_STATUS_INVALID_HANDLE)
		return 0;
	if (ret)
		return ret;
	return scan_wait_stop(wifi, done);
}

static int run_scan_tlvs(struct centauri_wifi *wifi,
			 struct cfg80211_scan_request *request)
{
	u8 scan[14 + 5 + 4 * CEN_SCAN_CHANNELS_MAX + 9 +
		CEN_SCAN_SSIDS_MAX * (4 + 2 + IEEE80211_MAX_SSID_LEN)];
	unsigned int next = 0;

	if (!request || !request->n_channels ||
	    request->n_ssids > CEN_SCAN_SSIDS_MAX)
		return -EINVAL;
	while (next < request->n_channels) {
		u8 channels[1 + 4 * CEN_SCAN_CHANNELS_MAX] = {};
		u8 options[5] = { 0, 0, 1, 0, 0 };
		unsigned int count = 0, s;
		u32 done = wifi->scans_done;
		size_t size = 14;
		unsigned long deadline;
		u8 scan_id;
		int ret;

		if (scan_transport_stopped(wifi))
			return -EIO;
		if (READ_ONCE(wifi->scan_abort))
			return -ECANCELED;
		memset(scan, 0, sizeof(scan));
		wifi->scan_sequence = (wifi->scan_sequence % 31) + 1;
		scan_id = wifi->scan_sequence;
		scan[0] = scan_id;
		scan[3] = 0x80;
		scan[5] = 2;
		scan[7] = 0x10;
		put_unaligned_le16(SCAN_ACTIVE_MIN_MS, scan + 8);
		put_unaligned_le16(SCAN_LISTEN_MS, scan + 10);
		put_unaligned_le16(SCAN_LISTEN_MS, scan + 12);

		while (next < request->n_channels && count < CEN_SCAN_CHANNELS_MAX) {
			struct ieee80211_channel *channel = request->channels[next++];
			unsigned int number;
			int band;

			if (channel->flags & IEEE80211_CHAN_DISABLED)
				continue;
			band = firmware_band(channel->band);
			if (band < 0)
				return band;
			number = ieee80211_frequency_to_channel(channel->center_freq);
			if (!number || number > 255)
				return -EINVAL;
			channels[1 + 4 * count] = number;
			channels[2 + 4 * count] = band;
			if (channel->flags &
			    (IEEE80211_CHAN_NO_IR | IEEE80211_CHAN_RADAR))
				channels[2 + 4 * count] |= CHANNEL_PASSIVE;
			if (channel->flags & IEEE80211_CHAN_RADAR)
				channels[2 + 4 * count] |= CHANNEL_RADAR;
			count++;
		}
		if (!count)
			continue;
		channels[0] = count;
		append_tlv(scan, &size, 260, channels, 1 + 4 * count);
		append_tlv(scan, &size, 357, options, sizeof(options));
		for (s = 0; s < request->n_ssids; s++) {
			const struct cfg80211_ssid *ssid = &request->ssids[s];
			u8 entry[2 + IEEE80211_MAX_SSID_LEN];

			if (ssid->ssid_len > IEEE80211_MAX_SSID_LEN)
				return -EINVAL;
			if (ssid->ssid_len)
				scan[2] = 1;
			entry[0] = ssid->ssid_len ? 5 : 4;
			entry[1] = ssid->ssid_len;
			memcpy(entry + 2, ssid->ssid, ssid->ssid_len);
			append_tlv(scan, &size, 258, entry, 2 + ssid->ssid_len);
		}
		ret = command(wifi, 65551, scan, size);
		if (ret)
			return ret;
		deadline = jiffies + msecs_to_jiffies(max(8000U, count * 220 + 2000));
		do {
			if (scan_transport_stopped(wifi)) {
				if (!READ_ONCE(wifi->stopping) &&
				    !READ_ONCE(wifi->transport_paused) &&
				    readl(wifi->a.bar + 8) != 2)
					centauri_recovery_request(wifi->control,
								  "scan: Alpha stage");
				return -EIO;
			}
			drain(wifi);
			if (wifi->scans_done != done)
				break;
			if (READ_ONCE(wifi->scan_abort)) {
				int stop_ret = scan_stop_chunk_id(wifi, scan_id, done);

				return stop_ret == -EIO ? -EIO : -ECANCELED;
			}
			mutex_unlock(&wifi->lock);
			usleep_range(1000, 2000);
			mutex_lock(&wifi->lock);
		} while (time_before(jiffies, deadline));
		if (wifi->scans_done == done) {
			if (scan_transport_stopped(wifi))
				return -EIO;
			drain(wifi);
			if (wifi->scans_done == done) {
				int stop_ret;

				dev_warn(&wifi->control->pdev->dev,
					 "Wi-Fi scan of %u channels: no completion event\n",
					 count);
				stop_ret = scan_stop_chunk_id(wifi, scan_id, done);
				return stop_ret == -EIO ? -EIO : -ETIMEDOUT;
			}
		}
		if (READ_ONCE(wifi->scan_abort))
			return -ECANCELED;
	}
	return 0;
}

static int run_scan_legacy(struct centauri_wifi *wifi)
{
	u8 scan[14] = { 0, 0, 0, 0x80, 0, 2, 0, 0x10 };
	unsigned int chunk;
	int ret = 0;

	put_unaligned_le16(SCAN_LISTEN_MS, scan + 8);
	put_unaligned_le16(SCAN_LISTEN_MS, scan + 10);
	put_unaligned_le16(SCAN_LISTEN_MS, scan + 12);
	for (chunk = 0; chunk < 3 && !ret; chunk++) {
		u32 channels = wifi->channels_seen, done = wifi->scans_done;
		u32 seen = channels;
		unsigned long deadline, progress;
		bool stopped = false, refused = false;
		u8 scan_id;

		if (scan_transport_stopped(wifi)) {
			ret = -EIO;
			break;
		}
		if (READ_ONCE(wifi->scan_abort)) {
			ret = -ECANCELED;
			break;
		}
		wifi->scan_sequence = (wifi->scan_sequence % 31) + 1;
		scan_id = wifi->scan_sequence;
		scan[0] = scan_id;
		ret = command(wifi, 65551, scan, sizeof(scan));
		if (ret)
			break;
		deadline = jiffies + msecs_to_jiffies(8000);
		progress = jiffies;
		do {
			if (scan_transport_stopped(wifi)) {
				if (!READ_ONCE(wifi->stopping) &&
				    !READ_ONCE(wifi->transport_paused) &&
				    readl(wifi->a.bar + 8) != 2)
					centauri_recovery_request(wifi->control,
								  "scan: Alpha stage");
				ret = -EIO;
				break;
			}
			drain(wifi);
			if (wifi->scans_done != done)
				break;
			if (READ_ONCE(wifi->scan_abort)) {
				int stop_ret = stopped ? scan_wait_stop(wifi, done) :
					       scan_stop_chunk(wifi, done);

				ret = stop_ret == -EIO ? -EIO : -ECANCELED;
				break;
			}
			if (wifi->channels_seen != seen) {
				seen = wifi->channels_seen;
				progress = jiffies;
			}
			if (!stopped &&
			    (seen - channels >= 13 ||
			     (seen != channels &&
			      time_after(jiffies, progress + msecs_to_jiffies(1000))))) {
				stopped = true;
				if (command(wifi, 65537, NULL, 0)) {
					refused = true;
					break;
				}
				deadline = jiffies + msecs_to_jiffies(1000);
			}
			mutex_unlock(&wifi->lock);
			usleep_range(1000, 2000);
			mutex_lock(&wifi->lock);
		} while (time_before(jiffies, deadline));
		if (!ret && !refused && wifi->scans_done == done) {
			if (scan_transport_stopped(wifi)) {
				ret = -EIO;
			} else {
				drain(wifi);
				if (wifi->scans_done == done) {
					ret = stopped ? -ETIMEDOUT :
					      scan_stop_chunk(wifi, done);
				}
			}
		}
	}
	return ret;
}

static void scan_worker(struct work_struct *work)
{
	struct centauri_wifi *wifi = container_of(work, struct centauri_wifi, scan_work);
	const struct centauri_board *board = centauri_board();
	struct cfg80211_scan_info info = {};
	struct cfg80211_scan_request *request;
	u32 channels_start, beacons_start;
	bool alpha_halted;
	int ret = 0;

	mutex_lock(&wifi->lock);
	channels_start = wifi->channels_seen;
	beacons_start = wifi->beacons_seen;
	if (scan_transport_stopped(wifi))
		ret = -EIO;
	else if (READ_ONCE(wifi->scan_abort))
		ret = -ECANCELED;
	else if (board && board->scan_tlvs)
		ret = run_scan_tlvs(wifi, wifi->scan_request);
	else
		ret = run_scan_legacy(wifi);
	/* Alpha can leave stage 2 without a fault interrupt: restart it. */
	alpha_halted = !READ_ONCE(wifi->stopping) && !wifi->transport_paused &&
		       readl(wifi->a.bar + 8) != 2;
	info.aborted = !!ret || READ_ONCE(wifi->scan_abort) ||
		       scan_transport_stopped(wifi);
	request = wifi->scan_request;
	wifi->scan_request = NULL;
	mutex_unlock(&wifi->lock);
	if (alpha_halted)
		centauri_recovery_request(wifi->control, "scan: Alpha stage");
	dev_dbg(&wifi->control->pdev->dev,
		"Wi-Fi scan finished: rc=%d channels=%u beacons=%u\n",
		ret, wifi->channels_seen - channels_start,
		wifi->beacons_seen - beacons_start);
	if (request)
		cfg80211_scan_done(request, &info);
}

static int wifi_scan(struct wiphy *wiphy, struct cfg80211_scan_request *request)
{
	struct centauri_wifi *wifi = *(struct centauri_wifi **)wiphy_priv(wiphy);
	int ret = 0;

	mutex_lock(&wifi->lock);
	if (READ_ONCE(wifi->stopping)) {
		ret = -ENODEV;
	} else if (!wifi->configured) {
		ret = -ENETDOWN;
	} else if (wifi->scan_request || wifi->connecting) {
		ret = -EBUSY;
	} else {
		WRITE_ONCE(wifi->scan_abort, false);
		wifi->scan_request = request;
		schedule_work(&wifi->scan_work);
	}
	mutex_unlock(&wifi->lock);
	return ret;
}

static void wifi_abort_scan(struct wiphy *wiphy, struct wireless_dev *wdev)
{
	struct centauri_wifi *wifi = *(struct centauri_wifi **)wiphy_priv(wiphy);

	if (wdev == &wifi->wdev)
		WRITE_ONCE(wifi->scan_abort, true);
}

static void cancel_scan(struct centauri_wifi *wifi)
{
	struct cfg80211_scan_info info = { .aborted = true };
	struct cfg80211_scan_request *request;

	WRITE_ONCE(wifi->scan_abort, true);
	cancel_work_sync(&wifi->scan_work);
	mutex_lock(&wifi->lock);
	request = wifi->scan_request;
	wifi->scan_request = NULL;
	mutex_unlock(&wifi->lock);
	if (request)
		cfg80211_scan_done(request, &info);
}

static void cancel_connect_locked(struct centauri_wifi *wifi)
{
	if (!wifi->connecting)
		return;
	wifi->connecting = false;
	wifi->connected = false;
	memzero_explicit(wifi->connect_payload, sizeof(wifi->connect_payload));
	cfg80211_connect_timeout(wifi->netdev, wifi->bssid, wifi->request_ie,
				 wifi->request_ie_size, GFP_KERNEL,
				 NL80211_TIMEOUT_UNSPECIFIED);
}

static int rsn_caps_offset(const u8 *body, size_t size)
{
	size_t offset = 6;
	u16 count;
	unsigned int i;

	for (i = 0; i < 2; i++) {
		if (offset + 2 > size)
			return -EINVAL;
		count = get_unaligned_le16(body + offset);
		offset += 2 + 4 * count;
		if (offset > size)
			return -EINVAL;
	}
	return offset + 2 <= size ? offset : -EINVAL;
}

static void append_tlv(u8 *payload, size_t *size, u16 tag, const void *data, u16 length)
{
	put_unaligned_le16(tag, payload + *size);
	put_unaligned_le16(length, payload + *size + 2);
	memcpy(payload + *size + 4, data, length);
	*size += 4 + length;
}

static int rsn_akm_offset(const u8 *body, size_t size)
{
	size_t offset;
	u16 count;

	if (size < 8 || get_unaligned_le16(body) != 1)
		return -EINVAL;
	count = get_unaligned_le16(body + 6);
	offset = 8 + 4 * count;
	if (!count || offset + 2 > size)
		return -EINVAL;
	count = get_unaligned_le16(body + offset);
	if (!count || offset + 2 + 4 * count > size)
		return -EINVAL;
	return offset;
}

static bool rsn_lists_akm(const u8 *body, size_t size, u32 akm)
{
	int offset = rsn_akm_offset(body, size);
	unsigned int i;

	if (offset < 0)
		return false;
	for (i = 0; i < get_unaligned_le16(body + offset); i++)
		if (get_unaligned_be32(body + offset + 2 + 4 * i) == akm)
			return true;
	return false;
}

#define CONNECT_AUTH_OPEN 0
#define CONNECT_AUTH_SAE 3
#define CONNECT_AKM_NONE 0
#define CONNECT_AKM_PSK 4
#define CONNECT_AKM_PSK_SHA256 6
#define CONNECT_AKM_SAE 8
#define CONNECT_CIPHER_NONE 0
#define CONNECT_CIPHER_CCMP 4
#define CONNECT_CIPHER_GCMP 7
#define CONNECT_CIPHER_GCMP_256 8

static int firmware_pairwise_cipher(u32 suite)
{
	switch (suite) {
	case WLAN_CIPHER_SUITE_CCMP:
		return CONNECT_CIPHER_CCMP;
	case WLAN_CIPHER_SUITE_GCMP:
		return CONNECT_CIPHER_GCMP;
	case WLAN_CIPHER_SUITE_GCMP_256:
		return CONNECT_CIPHER_GCMP_256;
	default:
		return -EOPNOTSUPP;
	}
}

static u8 firmware_akm(u32 akm)
{
	switch (akm) {
	case WLAN_AKM_SUITE_PSK:
		return CONNECT_AKM_PSK;
	case WLAN_AKM_SUITE_PSK_SHA256:
		return CONNECT_AKM_PSK_SHA256;
	case WLAN_AKM_SUITE_SAE:
		return CONNECT_AKM_SAE;
	default:
		return CONNECT_AKM_NONE;
	}
}

static bool cipher_pair_supported(u32 pairwise, u32 group, u32 akm)
{
	if (firmware_pairwise_cipher(pairwise) < 0)
		return false;
	if (group == pairwise)
		return true;
	if (group == WLAN_CIPHER_SUITE_TKIP)
		return akm != WLAN_AKM_SUITE_SAE &&
		       pairwise == WLAN_CIPHER_SUITE_CCMP;
	if (group == WLAN_CIPHER_SUITE_CCMP || group == WLAN_CIPHER_SUITE_GCMP)
		return pairwise == WLAN_CIPHER_SUITE_GCMP ||
		       pairwise == WLAN_CIPHER_SUITE_GCMP_256;
	return false;
}

static int select_rsn(const u8 *body, size_t size, u32 akm, u32 pairwise,
		      u32 group, u8 *selected)
{
	size_t tail;
	int offset;

	offset = rsn_akm_offset(body, size);
	if (offset < 0)
		return offset;
	tail = offset + 2 + 4 * get_unaligned_le16(body + offset);
	if (tail + 2 > size)
		return -EINVAL;
	if (!cipher_pair_supported(pairwise, group, akm))
		return -EOPNOTSUPP;
	if (get_unaligned_be32(body + 2) != group ||
	    get_unaligned_le16(body + 6) != 1 ||
	    get_unaligned_be32(body + 8) != pairwise)
		return -EOPNOTSUPP;
	if (!rsn_lists_akm(body, size, akm) && tail + 4 <= size &&
	    get_unaligned_le16(body + tail + 2))
		return -EOPNOTSUPP;
	memcpy(selected, body, offset);
	put_unaligned_le16(1, selected + offset);
	put_unaligned_be32(akm, selected + offset + 2);
	memcpy(selected + offset + 6, body + tail, size - tail);
	return offset + 6 + size - tail;
}

static const u32 connect_akms[] = {
	WLAN_AKM_SUITE_SAE,
	WLAN_AKM_SUITE_PSK_SHA256,
	WLAN_AKM_SUITE_PSK,
};

static bool akm_usable(const struct cfg80211_crypto_settings *crypto, u32 akm)
{
	unsigned int i;

	for (i = 0; i < crypto->n_akm_suites; i++)
		if (crypto->akm_suites[i] == akm)
			return akm == WLAN_AKM_SUITE_SAE ? crypto->sae_pwd_len :
							   !!crypto->psk;
	return false;
}

static u32 connect_akm(const struct cfg80211_crypto_settings *crypto,
		       const u8 *own, size_t own_size, const u8 *target,
		       size_t target_size, bool mfpc, u32 pairwise, u32 group)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(connect_akms); i++)
		if (akm_usable(crypto, connect_akms[i]) &&
		    cipher_pair_supported(pairwise, group, connect_akms[i]) &&
		    rsn_lists_akm(own, own_size, connect_akms[i]))
			return connect_akms[i];
	for (i = 0; i < ARRAY_SIZE(connect_akms); i++) {
		if (!akm_usable(crypto, connect_akms[i]) ||
		    !cipher_pair_supported(pairwise, group, connect_akms[i]))
			continue;
		if (connect_akms[i] == WLAN_AKM_SUITE_SAE && !mfpc)
			continue;
		if (!target_size ||
		    rsn_lists_akm(target, target_size, connect_akms[i]))
			return connect_akms[i];
	}
	return 0;
}

static size_t target_rsn(struct wiphy *wiphy,
			 const struct cfg80211_connect_params *params, u8 *body)
{
	const struct element *element;
	struct cfg80211_bss *bss;
	size_t size = 0;

	bss = cfg80211_get_bss(wiphy, params->channel ?: params->channel_hint,
			       params->bssid ?: params->bssid_hint, params->ssid,
			       params->ssid_len, IEEE80211_BSS_TYPE_ESS,
			       IEEE80211_PRIVACY_ON);
	if (!bss)
		return 0;
	rcu_read_lock();
	element = ieee80211_bss_get_elem(bss, WLAN_EID_RSN);
	if (element) {
		size = element->datalen;
		memcpy(body, element->data, size);
	}
	rcu_read_unlock();
	cfg80211_put_bss(wiphy, bss);
	return size;
}

static int wifi_connect(struct wiphy *wiphy, struct net_device *netdev,
			struct cfg80211_connect_params *params)
{
	static const u8 mld_defaults[14] = {
		1, 0, 1, 0xff, 0xff, 0xff, 0xff, 0xff,
		0xff, 0xff, 0xff, 3, 0, 0,
	};
	/* Firmware connection timer budgets (ms): probe, auth, assoc, EAPOL, retry. */
	static const __le32 connection_timeouts[] = {
		cpu_to_le32(260), cpu_to_le32(6000), cpu_to_le32(2000),
		cpu_to_le32(15000), cpu_to_le32(300),
	};
	struct centauri_wifi *wifi = *(struct centauri_wifi **)wiphy_priv(wiphy);
	const struct centauri_board *board = centauri_board();
	const u8 *bssid = params->bssid ?: params->bssid_hint;
	struct ieee80211_channel *channel = params->channel ?: params->channel_hint;
	struct cfg80211_bss *bss = NULL;
	const u8 *rsn = NULL;
	u8 rsn_body[255], target[255], ssid[35];
	u8 *payload = wifi->connect_payload;
	size_t size = 130, rsn_size = 0, rsn_prefix, rsn_tail;
	u8 join_mode = (board && board->scan_tlvs) ? 5 : 0;
	u8 roaming = 0;
	u8 password[2 + SAE_PASSWORD_MAX_LEN];
	int ret = 0, offset, band, fw_cipher = CONNECT_CIPHER_NONE;
	bool open_net, sae = false, mfpc = false;
	u32 akm = 0, pairwise = 0, group = 0;

	if (!params->ssid_len || params->ssid_len > IEEE80211_MAX_SSID_LEN ||
	    params->crypto.sae_pwd_len > SAE_PASSWORD_MAX_LEN ||
	    params->ie_len > sizeof(wifi->request_ie)) {
		dev_dbg(&wifi->control->pdev->dev,
			"connect refused: ssid_len=%zu sae_pwd_len=%u ie_len=%zu\n",
			params->ssid_len, params->crypto.sae_pwd_len,
			params->ie_len);
		return -EOPNOTSUPP;
	}
	open_net = !params->privacy && !params->crypto.wpa_versions &&
		   !params->crypto.n_akm_suites &&
		   !params->crypto.n_ciphers_pairwise &&
		   !params->crypto.cipher_group;
	if (open_net) {
		if (params->auth_type != NL80211_AUTHTYPE_OPEN_SYSTEM &&
		    params->auth_type != NL80211_AUTHTYPE_AUTOMATIC)
			return -EOPNOTSUPP;
	} else {
		if (params->crypto.n_ciphers_pairwise != 1)
			return -EOPNOTSUPP;
		pairwise = params->crypto.ciphers_pairwise[0];
		group = params->crypto.cipher_group;
		fw_cipher = firmware_pairwise_cipher(pairwise);
		if (fw_cipher < 0)
			return -EOPNOTSUPP;
		rsn = cfg80211_find_ie(WLAN_EID_RSN, params->ie, params->ie_len);
		if (!rsn)
			return -EINVAL;
		offset = rsn_caps_offset(rsn + 2, rsn[1]);
		if (offset < 0)
			return -EINVAL;
		mfpc = get_unaligned_le16(rsn + 2 + offset) & BIT(7);
		akm = connect_akm(&params->crypto, rsn + 2, rsn[1], target,
				  target_rsn(wiphy, params, target), mfpc,
				  pairwise, group);
		ret = akm ? select_rsn(rsn + 2, rsn[1], akm, pairwise, group,
				       rsn_body) :
			    -EOPNOTSUPP;
		if (ret < 0)
			return ret;
		rsn_size = ret;
		sae = akm == WLAN_AKM_SUITE_SAE;
		ret = 0;
	}
	mutex_lock(&wifi->lock);
	if (READ_ONCE(wifi->stopping)) {
		ret = -ENODEV;
		goto out;
	}
	if (!wifi->configured || wifi->connecting || wifi->connected ||
	    wifi->scan_request) {
		ret = -EBUSY;
		goto out;
	}
	if (bssid && channel) {
		memcpy(wifi->bssid, bssid, ETH_ALEN);
	} else {
		bss = cfg80211_get_bss(wiphy, channel, bssid,
				       params->ssid, params->ssid_len,
				       IEEE80211_BSS_TYPE_ESS,
				       open_net ? IEEE80211_PRIVACY_OFF :
						  IEEE80211_PRIVACY_ON);
		if (!bss) {
			ret = -ENOENT;
			goto out;
		}
		memcpy(wifi->bssid, bss->bssid, ETH_ALEN);
		channel = bss->channel;
		cfg80211_put_bss(wiphy, bss);
	}
	ret = firmware_band(channel->band);
	if (ret < 0)
		goto out;
	band = ret;
	ret = 0;
	if (channel->band == NL80211_BAND_6GHZ && (open_net || !sae || !mfpc)) {
		ret = -EOPNOTSUPP;
		goto out;
	}
	wifi->connect_channel = channel;
	wifi->connect_privacy = !open_net;
	memcpy(wifi->ssid, params->ssid, params->ssid_len);
	wifi->ssid_len = params->ssid_len;
	memset(payload, 0, sizeof(wifi->connect_payload));
	payload[0] = 1;
	memcpy(payload + 1, wifi->bssid, ETH_ALEN);
	payload[7] = ieee80211_frequency_to_channel(channel->center_freq);
	payload[8] = band;
	payload[0x46] = sae ? CONNECT_AUTH_SAE : CONNECT_AUTH_OPEN;
	payload[0x47] = open_net ? CONNECT_AKM_NONE : firmware_akm(akm);
	put_unaligned_le16(fw_cipher, payload + 0x48);
	if (!open_net && !sae) {
		put_unaligned_le16(32, payload + 0x4a);
		memcpy(payload + 0x4c, params->crypto.psk, 32);
		if (verify_pmk)
			memcpy(wifi->expected_pmk, params->crypto.psk, 32);
	}
	append_tlv(payload, &size, 304, &join_mode, 1);
	append_tlv(payload, &size, 63763, connection_timeouts, sizeof(connection_timeouts));
	ssid[0] = 1;
	ssid[1] = 4;
	ssid[2] = params->ssid_len;
	memcpy(ssid + 3, params->ssid, params->ssid_len);
	append_tlv(payload, &size, 342, ssid, 3 + params->ssid_len);
	if (!open_net) {
		append_tlv(payload, &size, WLAN_EID_RSN, rsn_body, rsn_size);
		if (sae) {
			put_unaligned_le16(params->crypto.sae_pwd_len, password);
			memcpy(password + 2, params->crypto.sae_pwd,
			       params->crypto.sae_pwd_len);
			append_tlv(payload, &size, 313, password,
				   2 + params->crypto.sae_pwd_len);
			memzero_explicit(password, sizeof(password));
		}
		append_tlv(payload, &size, 316, &roaming, 1);
	}
	append_tlv(payload, &size, 376, mld_defaults, sizeof(mld_defaults));
	wifi->connect_size = size;
	if (open_net) {
		if (params->ie_len)
			memcpy(wifi->request_ie, params->ie, params->ie_len);
		wifi->request_ie_size = params->ie_len;
	} else {
		rsn_prefix = rsn - params->ie;
		rsn_tail = rsn_prefix + 2 + rsn[1];
		memcpy(wifi->request_ie, params->ie, rsn_prefix);
		wifi->request_ie[rsn_prefix] = WLAN_EID_RSN;
		wifi->request_ie[rsn_prefix + 1] = rsn_size;
		memcpy(wifi->request_ie + rsn_prefix + 2, rsn_body, rsn_size);
		memcpy(wifi->request_ie + rsn_prefix + 2 + rsn_size,
		       params->ie + rsn_tail, params->ie_len - rsn_tail);
		wifi->request_ie_size = params->ie_len - rsn[1] + rsn_size;
	}
	WRITE_ONCE(wifi->connect_abort, false);
	wifi->connecting = true;
	wifi->connect_status = 0;
	wifi->disconnect_reason = 0;
	wifi->disconnect_local = false;
	wifi->associated = wifi->pmk_seen = wifi->pmk_matches = false;
	wifi->connect_event = wifi->disconnect_event = false;
	schedule_work(&wifi->connect_work);
out:
	mutex_unlock(&wifi->lock);
	return ret;
}

static struct cfg80211_bss *connected_bss(struct centauri_wifi *wifi)
{
	struct cfg80211_bss *bss;
	u8 ie[2 + IEEE80211_MAX_SSID_LEN];

	bss = cfg80211_get_bss(wifi->wiphy, wifi->connect_channel, wifi->bssid,
			       NULL, 0, IEEE80211_BSS_TYPE_ESS,
			       IEEE80211_PRIVACY_ANY);
	if (bss)
		return bss;
	ie[0] = WLAN_EID_SSID;
	ie[1] = wifi->ssid_len;
	memcpy(ie + 2, wifi->ssid, wifi->ssid_len);
	return cfg80211_inform_bss(wifi->wiphy, wifi->connect_channel,
				   CFG80211_BSS_FTYPE_UNKNOWN, wifi->bssid, 0,
				   WLAN_CAPABILITY_ESS |
					   (wifi->connect_privacy ?
						    WLAN_CAPABILITY_PRIVACY :
						    0),
				   100, ie, 2 + wifi->ssid_len, -95 * 100,
				   GFP_KERNEL);
}

/* Without the host power state the firmware treats the host as asleep, powers the PHYs down and traps about 15 s after the
 * join. Same three commands as the host_wakeup and tx_config debugfs files; called with wifi->lock held.
 */
static void host_setup_commands(struct centauri_wifi *wifi)
{
	static const u8 awake[3] = {};
	static const u8 control_profile[] = { 0x54, 1, 10, 0, 0, 8, 0, 0, 0xff, 0xff, 0xff, 0xff, 1, 1 };
	u8 lifetime[20] = {};
	int ret, i;

	if (!host_setup)
		return;
	if (!wifi->host_awake_sent) {
		ret = command(wifi, 6, awake, sizeof(awake));
		dev_info(&wifi->control->pdev->dev, "host power state awake: rc=%d status=%u\n", ret, wifi->reply_status);
		wifi->host_awake_sent = !ret;
	}
	if (!(wifi->tx_defaults_applied & BIT(0))) {
		for (i = 0; i < 4; i++)
			put_unaligned_le32(2000000, lifetime + 4 + 4 * i);
		ret = command(wifi, 43, lifetime, sizeof(lifetime));
		if (!ret)
			wifi->tx_defaults_applied |= BIT(0);
		dev_info(&wifi->control->pdev->dev, "TX packet lifetime defaults: rc=%d\n", ret);
	}
	if (!(wifi->tx_defaults_applied & BIT(1))) {
		ret = command(wifi, 52, control_profile, sizeof(control_profile));
		if (!ret)
			wifi->tx_defaults_applied |= BIT(1);
		dev_info(&wifi->control->pdev->dev, "TX control profile defaults: rc=%d\n", ret);
	}
}

static void connect_worker(struct work_struct *work)
{
	struct centauri_wifi *wifi = container_of(work, struct centauri_wifi, connect_work);
	unsigned long deadline;
	u16 slot, status, command_status;
	bool checked_pmk = false, probed_eapol = false, queried_link = false;
	int ret;

	mutex_lock(&wifi->lock);
	if (READ_ONCE(wifi->connect_abort) || READ_ONCE(wifi->stopping)) {
		cancel_connect_locked(wifi);
		mutex_unlock(&wifi->lock);
		return;
	}
	if (auto_rings) {
		static const unsigned int ring_ids[] = { 76, 24, 15, 12 };
		unsigned int n;

		for (n = 0; n < ARRAY_SIZE(ring_ids); n++) {
			int ring_ret = data_probe_locked(wifi, ring_ids[n], true);

			if (ring_ret && ring_ret != -EBUSY)
				dev_warn(&wifi->control->pdev->dev, "data ring %u setup failed: %d\n", ring_ids[n], ring_ret);
		}
	}
	host_setup_commands(wifi);
	drain(wifi);
	wifi->connect_event = false;
	wifi->disconnect_event = false;
	slot = wifi->command_head;
	ret = command(wifi, 131072, wifi->connect_payload, wifi->connect_size);
	command_status = wifi->reply_status;
	memzero_explicit(wifi->connect_payload, sizeof(wifi->connect_payload));
	deadline = jiffies + msecs_to_jiffies(15000);
	while (!ret && !wifi->connect_event &&
	       !READ_ONCE(wifi->connect_abort) && !READ_ONCE(wifi->stopping) &&
	       time_before(jiffies, deadline)) {
		drain(wifi);
		if (wifi->disconnect_event)
			ret = -ENOTCONN;
		if (link_diagnostics && wifi->associated && !queried_link) {
			unsigned int i;
			int link_ret;

			queried_link = true;
			link_ret = query_link_status(wifi);
			dev_info(&wifi->control->pdev->dev,
				 "connection link status: rc=%d complete=%d valid=%d count=%u emlsr=%d/%u\n",
				 link_ret, wifi->connect_event, wifi->link_status_valid, wifi->link_count,
				 wifi->emlsr_valid, wifi->emlsr_mode);
			for (i = 0; i < wifi->link_tlv_count; i++)
				dev_info(&wifi->control->pdev->dev, "connection link TLV: tag=%u length=%u\n",
					 wifi->link_tags[i], wifi->link_lengths[i]);
			if (wifi->link_status_valid)
				for (i = 0; i < wifi->link_count; i++)
					dev_info(&wifi->control->pdev->dev,
						 "connection link %u: prime=%u channel=%u band=%u\n",
						 i, wifi->link_prime[i], wifi->link_channel[i], wifi->link_band[i]);
		}
		if (eapol_after_assoc && wifi->associated && !probed_eapol) {
			unsigned int i;
			int probe_ret;

			probed_eapol = true;
			probe_ret = query_station_stats(wifi);
			dev_info(&wifi->control->pdev->dev,
				 "connection counters: complete=%d rc=%d valid=%d length=%u\n",
				 wifi->connect_event, probe_ret, wifi->station_stats_valid,
				 wifi->station_stats_len);
			if (!probe_ret && wifi->station_stats_valid)
				for (i = 0; i < wifi->station_stats_len; i += 4) {
					u32 value = get_unaligned_le32(wifi->station_stats + i);

					if (value)
						dev_info(&wifi->control->pdev->dev,
							 "station counter %03x=%u\n", i, value);
				}
		}
		if (verify_pmk && wifi->associated && !checked_pmk) {
			int key_ret = command(wifi, 0xf0025, wifi->bssid, ETH_ALEN);

			checked_pmk = true;
			dev_info(&wifi->control->pdev->dev,
				 "Wi-Fi firmware key check: rc=%d available=%d matches=%d\n",
				 key_ret, wifi->pmk_seen, wifi->pmk_matches);
		}
		if (readl(wifi->a.bar + 8) != 2)
			ret = -EIO;
		usleep_range(1000, 2000);
	}
	if (READ_ONCE(wifi->connect_abort) || READ_ONCE(wifi->stopping))
		ret = -ECANCELED;
	else if (!ret && !wifi->connect_event)
		ret = -ETIMEDOUT;
	status = !ret && wifi->connect_status == 0 ? WLAN_STATUS_SUCCESS : WLAN_STATUS_UNSPECIFIED_FAILURE;
	wifi->connecting = false;
	wifi->connected = status == WLAN_STATUS_SUCCESS;
	wifi->station_signal_valid = false;
	wifi->station_noise_valid = false;
	wifi->station_tx_retrans_valid = false;
	wifi->station_tx_retrans_base_valid = false;
	wifi->station_tx_fail_valid = false;
	wifi->station_tx_fail_base_valid = false;
	wifi->station_beacon_valid = false;
	wifi->station_beacon_base_valid = false;
	wifi->station_cca_valid = false;
	wifi->station_tx_rate_valid = false;
	wifi->station_tx_power_valid = false;
	wifi->station_rx_rate_valid = false;
	wifi->station_chip_tx_rate_valid = false;
	wifi->station_chains = 0;
	wifi->station_phy_noise_valid = false;
	wifi->station_chandef_valid = false;
	wifi->station_signal_at = jiffies - HZ;
	wifi->station_noise_at = jiffies - HZ;
	wifi->station_counters_at = jiffies - HZ;
	wifi->station_cca_at = jiffies - HZ;
	wifi->station_tx_info_at = jiffies - HZ;
	wifi->station_beacon_at = jiffies - HZ;
	wifi->station_phy_dump_at = jiffies - HZ;
	if (wifi->connected) {
		wifi->connected_at = jiffies;
		wifi->station_start = wifi->netdev->stats;
	}
	if (wifi->connected && !roam_scan) {
		int roam_ret = command(wifi, 0x20016, NULL, 0);

		dev_dbg(&wifi->control->pdev->dev, "roam scan off: rc=%d status=%u\n",
			roam_ret, wifi->reply_status);
	}
	memzero_explicit(wifi->expected_pmk, sizeof(wifi->expected_pmk));
	/* The command and connection completion have consumed the credential buffer. */
	if (slot < ring_size[1])
		memzero_explicit(wifi->a.window + TX_BUFFER + slot * BUFFER_SIZE, BUFFER_SIZE);
	dev_dbg(&wifi->control->pdev->dev,
		"Wi-Fi authentication result: rc=%d command_status=%u event_status=%u complete_event=%d\n",
		ret, command_status, wifi->connect_status, wifi->connect_event);
	if (ret == -ECANCELED || ret == -ETIMEDOUT)
		cfg80211_connect_timeout(wifi->netdev, wifi->bssid,
					 wifi->request_ie,
					 wifi->request_ie_size, GFP_KERNEL,
					 NL80211_TIMEOUT_UNSPECIFIED);
	else
		cfg80211_connect_bss(wifi->netdev, wifi->bssid,
				     status == WLAN_STATUS_SUCCESS ?
					     connected_bss(wifi) :
					     NULL,
				     wifi->request_ie, wifi->request_ie_size,
				     NULL, 0, status, GFP_KERNEL,
				     NL80211_TIMEOUT_UNSPECIFIED);
	if (status == WLAN_STATUS_SUCCESS && auto_rings && !READ_ONCE(wifi->stopping)) {
		int ring_ret = data_probe_locked(wifi, 25, true);

		if (ring_ret && ring_ret != -EBUSY)
			dev_warn(&wifi->control->pdev->dev, "data ring 25 setup failed: %d\n", ring_ret);
	}
	if (status == WLAN_STATUS_SUCCESS && !READ_ONCE(wifi->stopping)) {
		if (wifi->connect_privacy)
			cfg80211_port_authorized(wifi->netdev, wifi->bssid,
						 NULL, 0, GFP_KERNEL);
		netif_carrier_on(wifi->netdev);
		if (wifi->data_probe_done & BIT(5))
			netif_wake_queue(wifi->netdev);
		mod_delayed_work(system_percpu_wq, &wifi->link_work,
				 msecs_to_jiffies(20));
	}
	mutex_unlock(&wifi->lock);
}

static void link_worker(struct work_struct *work)
{
	struct centauri_wifi *wifi = container_of(to_delayed_work(work), struct centauri_wifi, link_work);
	bool disconnected, alpha_halted, local = false;
	u16 reason = WLAN_REASON_UNSPECIFIED;

	mutex_lock(&wifi->lock);
	if (wifi->transport_paused) {
		mutex_unlock(&wifi->lock);
		return;
	}
	drain(wifi);
	alpha_halted = !READ_ONCE(wifi->stopping) && readl(wifi->a.bar + 8) != 2;
	disconnected = wifi->disconnect_event || READ_ONCE(wifi->stopping) ||
		alpha_halted;
	if (disconnected) {
		reason = wifi->disconnect_reason ?: WLAN_REASON_UNSPECIFIED;
		local = wifi->disconnect_local;
		wifi->connected = false;
		wifi->station_signal_valid = false;
		wifi->station_noise_valid = false;
		wifi->station_tx_retrans_valid = false;
		wifi->station_tx_retrans_base_valid = false;
		wifi->station_tx_fail_valid = false;
		wifi->station_tx_fail_base_valid = false;
		wifi->station_beacon_valid = false;
		wifi->station_beacon_base_valid = false;
		wifi->station_cca_valid = false;
		wifi->station_tx_rate_valid = false;
		wifi->station_tx_power_valid = false;
		wifi->station_rx_rate_valid = false;
		wifi->station_chip_tx_rate_valid = false;
		wifi->station_chains = 0;
		wifi->station_phy_noise_valid = false;
		wifi->station_chandef_valid = false;
		wifi->station_signal_at = jiffies - HZ;
		wifi->station_noise_at = jiffies - HZ;
		wifi->station_counters_at = jiffies - HZ;
		wifi->station_cca_at = jiffies - HZ;
		wifi->station_tx_info_at = jiffies - HZ;
		wifi->station_beacon_at = jiffies - HZ;
		wifi->station_phy_dump_at = jiffies - HZ;
	}
	mutex_unlock(&wifi->lock);
	if (disconnected) {
		netif_stop_queue(wifi->netdev);
		skb_queue_purge(&wifi->data_tx_queue);
		netif_carrier_off(wifi->netdev);
		cfg80211_disconnected(wifi->netdev, reason, NULL, 0, local, GFP_KERNEL);
	} else {
		schedule_delayed_work(&wifi->link_work, msecs_to_jiffies(20));
	}
	if (alpha_halted) {
		dev_warn_ratelimited(&wifi->control->pdev->dev,
				     "Alpha execution stage %#x\n",
				     readl(wifi->a.bar + 8));
		centauri_recovery_request(wifi->control, "link: Alpha stage");
	}
}

/* MSI from Control or Alpha: service the data rings now instead of at the next poll. */
void centauri_wifi_kick(struct centauri *c)
{
	struct centauri_wifi *wifi = READ_ONCE(c->wifi);

	if (wifi && READ_ONCE(wifi->connected) && !READ_ONCE(wifi->stopping) &&
	    !READ_ONCE(wifi->transport_paused))
		mod_delayed_work(system_percpu_wq, &wifi->link_work, 0);
}

static int wifi_disconnect(struct wiphy *wiphy, struct net_device *netdev, u16 reason)
{
	struct centauri_wifi *wifi = *(struct centauri_wifi **)wiphy_priv(wiphy);
	u8 payload[16] = {};
	bool connected;
	u16 reply_status;
	int ret;

	WRITE_ONCE(wifi->connect_abort, true);
	cancel_scan(wifi);
	cancel_work_sync(&wifi->connect_work);
	mutex_lock(&wifi->lock);
	cancel_connect_locked(wifi);
	put_unaligned_le16(reason, payload);
	put_unaligned_le16(328, payload + 8);
	put_unaligned_le16(4, payload + 10);
	/* Preserve the firmware's default disconnect budget. */
	put_unaligned_le32(500, payload + 12);
	ret = command(wifi, 131073, payload, sizeof(payload));
	reply_status = wifi->reply_status;
	connected = wifi->connected;
	wifi->connected = false;
	wifi->station_signal_valid = false;
	wifi->station_noise_valid = false;
	wifi->station_tx_retrans_valid = false;
	wifi->station_tx_retrans_base_valid = false;
	wifi->station_tx_fail_valid = false;
	wifi->station_tx_fail_base_valid = false;
	wifi->station_beacon_valid = false;
	wifi->station_beacon_base_valid = false;
	wifi->station_cca_valid = false;
	wifi->station_tx_rate_valid = false;
	wifi->station_tx_power_valid = false;
	wifi->station_rx_rate_valid = false;
	wifi->station_chip_tx_rate_valid = false;
	wifi->station_chains = 0;
	wifi->station_phy_noise_valid = false;
	wifi->station_chandef_valid = false;
	wifi->station_signal_at = jiffies - HZ;
	wifi->station_noise_at = jiffies - HZ;
	wifi->station_counters_at = jiffies - HZ;
	wifi->station_cca_at = jiffies - HZ;
	wifi->station_tx_info_at = jiffies - HZ;
	wifi->station_beacon_at = jiffies - HZ;
	wifi->station_phy_dump_at = jiffies - HZ;
	mutex_unlock(&wifi->lock);
	cancel_delayed_work_sync(&wifi->link_work);
	netif_stop_queue(netdev);
	skb_queue_purge(&wifi->data_tx_queue);
	netif_carrier_off(netdev);
	if (connected)
		cfg80211_disconnected(netdev, reason, NULL, 0, true, GFP_KERNEL);
	return ret == -EREMOTEIO && reply_status == 326 ? 0 : ret;
}

static int wifi_power_mgmt(struct wiphy *wiphy, struct net_device *netdev,
			   bool enabled, int timeout)
{
	struct centauri_wifi *wifi = *(struct centauri_wifi **)wiphy_priv(wiphy);
	u8 mode = enabled ? 0 : 3;
	int ret;

	/* Power-save mode, command 7. */
	mutex_lock(&wifi->lock);
	if (READ_ONCE(wifi->transport_paused))
		ret = -EBUSY;
	else
		ret = wifi->configured ? command(wifi, 7, &mode, sizeof(mode)) :
					 -ENETDOWN;
	mutex_unlock(&wifi->lock);
	return ret;
}

static void wifi_regulatory(struct wiphy *wiphy, struct regulatory_request *request)
{
	struct centauri_wifi *wifi = *(struct centauri_wifi **)wiphy_priv(wiphy);
	u8 country[3] = { request->alpha2[0], request->alpha2[1], 1 };
	bool named = country[0] >= 'A' && country[0] <= 'Z' &&
		     country[1] >= 'A' && country[1] <= 'Z';
	int ret;

	mutex_lock(&wifi->lock);
	if (wifi->configured && named && !READ_ONCE(wifi->transport_paused) &&
	    request->initiator != NL80211_REGDOM_SET_BY_DRIVER) {
		ret = command(wifi, 9, country, sizeof(country));
		if (ret)
			dev_err(&wifi->control->pdev->dev,
				"Wi-Fi country %c%c update failed: %d\n",
				country[0], country[1], ret);
		else
			firmware_report(wifi, 19, wifi->hw_channels,
					&wifi->hw_channels_blob);
	}
	channels_apply(wifi);
	mutex_unlock(&wifi->lock);
}

static struct cfg80211_bss *wifi_joined_bss(struct centauri_wifi *wifi)
{
	if (!wifi->connected)
		return NULL;
	return cfg80211_get_bss(wifi->wiphy, wifi->connect_channel, wifi->bssid,
				NULL, 0, IEEE80211_BSS_TYPE_ESS,
				IEEE80211_PRIVACY_ANY);
}

static int query_sta_counters(struct centauri_wifi *wifi)
{
	u8 payload[22] = {};
	size_t size = sizeof(wifi->stats_reply);
	u32 retrans, tx_fail, tx_rate_kbps, rx_rate_kbps;
	bool tx_fail_valid;
	int ret;

	if (READ_ONCE(wifi->transport_paused))
		return -EBUSY;
	wifi->station_tx_retrans_valid = false;
	wifi->station_tx_fail_valid = false;
	wifi->station_rx_rate_valid = false;
	wifi->station_chip_tx_rate_valid = false;
	wifi->station_counters_at = jiffies;
	if (!wifi->configured)
		return -ENETDOWN;
	/* Statistics bit 1 (chip counters), TLV 363 scope 2, reply TLV 0xf901. */
	put_unaligned_le64(BIT_ULL(1), payload);
	payload[8] = 1;
	put_unaligned_le16(363, payload + 9);
	put_unaligned_le16(9, payload + 11);
	payload[21] = 2;
	ret = command_reply(wifi, 0xb0001, payload, sizeof(payload),
			    wifi->stats_reply, &size);
	if (!ret && chip_sta_counters_read(wifi->stats_reply,
					   min(size, sizeof(wifi->stats_reply)),
					   &retrans, &tx_fail, &tx_fail_valid,
					   &tx_rate_kbps, &rx_rate_kbps)) {
		if (!wifi->station_tx_retrans_base_valid) {
			wifi->station_tx_retrans_base = retrans;
			wifi->station_tx_retrans_base_valid = true;
		}
		wifi->station_tx_retrans = retrans;
		wifi->station_tx_retrans_valid = true;
		if (tx_fail_valid) {
			if (!wifi->station_tx_fail_base_valid) {
				wifi->station_tx_fail_base = tx_fail;
				wifi->station_tx_fail_base_valid = true;
			}
			wifi->station_tx_fail = tx_fail;
			wifi->station_tx_fail_valid = true;
		}
		if (tx_rate_kbps >= 1000 && tx_rate_kbps <= 6500000) {
			wifi->station_chip_tx_rate_kbps = tx_rate_kbps;
			wifi->station_chip_tx_rate_valid = true;
		}
		if (rx_rate_kbps >= 1000 && rx_rate_kbps <= 6500000) {
			wifi->station_rx_rate_kbps = rx_rate_kbps;
			wifi->station_rx_rate_valid = true;
		}
	}
	return ret;
}

static int query_tx_info(struct centauri_wifi *wifi,
			 const struct ieee80211_channel *chan)
{
	u8 payload[22] = {};
	struct rate_info rate = {};
	bool rate_valid = false, power_valid = false;
	int fw_band, dbm = 0, ret;
	u64 slice;

	if (READ_ONCE(wifi->transport_paused))
		return -EBUSY;
	wifi->station_tx_rate_valid = false;
	wifi->station_tx_power_valid = false;
	wifi->station_tx_info_at = jiffies;
	if (!wifi->configured)
		return -ENETDOWN;
	fw_band = firmware_band(chan->band);
	if (fw_band < 0)
		return fw_band;
	/* Statistics bit 29 (TX power/rate report), TLV 363 scope 1, reply TLV 0xf920. */
	put_unaligned_le64(BIT_ULL(29), payload);
	payload[8] = 1;
	put_unaligned_le16(363, payload + 9);
	put_unaligned_le16(9, payload + 11);
	payload[21] = 1;
	slice = fw_band == 0 ? 1 : 2;
	for (;;) {
		size_t size = sizeof(wifi->stats_reply);

		put_unaligned_le64(slice, payload + 13);
		ret = command_reply(wifi, 0xb0001, payload, sizeof(payload),
				    wifi->stats_reply, &size);
		if (!ret && tx_power_rate_read(wifi->stats_reply,
					       min(size, sizeof(wifi->stats_reply)),
					       &rate, &rate_valid, &dbm,
					       &power_valid)) {
			if (rate_valid) {
				wifi->station_tx_rate = rate;
				wifi->station_tx_rate_valid = true;
			}
			if (power_valid) {
				wifi->station_tx_power_dbm = dbm;
				wifi->station_tx_power_valid = true;
			}
			return 0;
		}
		if (slice == 1)
			break;
		slice = 1;
	}
	return ret;
}

static int query_beacon_stats(struct centauri_wifi *wifi)
{
	u8 payload[22] = {};
	size_t size = sizeof(wifi->stats_reply);
	u32 rx_beacons, missed_beacons;
	int ret;

	if (READ_ONCE(wifi->transport_paused))
		return -EBUSY;
	wifi->station_beacon_valid = false;
	wifi->station_beacon_at = jiffies;
	if (!wifi->configured)
		return -ENETDOWN;
	/* Statistics bit 19 (frame counters), TLV 363 scope 0, reply TLV 0xf915. */
	put_unaligned_le64(BIT_ULL(19), payload);
	payload[8] = 1;
	put_unaligned_le16(363, payload + 9);
	put_unaligned_le16(9, payload + 11);
	ret = command_reply(wifi, 0xb0001, payload, sizeof(payload),
			    wifi->stats_reply, &size);
	if (!ret && beacon_stats_read(wifi->stats_reply,
				      min(size, sizeof(wifi->stats_reply)),
				      &rx_beacons, &missed_beacons)) {
		if (!wifi->station_beacon_base_valid) {
			wifi->station_beacon_rx_base = rx_beacons;
			wifi->station_beacon_loss_base = missed_beacons;
			wifi->station_beacon_base_valid = true;
		}
		wifi->station_beacon_rx = rx_beacons;
		wifi->station_beacon_loss = missed_beacons;
		wifi->station_beacon_valid = true;
	}
	return ret;
}

static int query_phy_dump(struct centauri_wifi *wifi,
			  const struct ieee80211_channel *chan)
{
	u8 payload[22] = {};
	size_t size = sizeof(wifi->stats_reply);
	s8 chain_rssi[2] = {}, noise = 0;
	bool noise_valid = false;
	u8 chains = 0;
	int fw_band, ret;

	if (READ_ONCE(wifi->transport_paused))
		return -EBUSY;
	wifi->station_chains = 0;
	wifi->station_phy_noise_valid = false;
	wifi->station_phy_dump_at = jiffies;
	if (!wifi->configured)
		return -ENETDOWN;
	fw_band = firmware_band(chan->band);
	if (fw_band < 0)
		return fw_band;
	/* Statistics bit 32 (PHY dump), TLV 363 scope 0, reply TLV 0xf923. */
	put_unaligned_le64(BIT_ULL(32), payload);
	payload[8] = 1;
	put_unaligned_le16(363, payload + 9);
	put_unaligned_le16(9, payload + 11);
	ret = command_reply(wifi, 0xb0001, payload, sizeof(payload),
			    wifi->stats_reply, &size);
	if (!ret && phy_dump_read(wifi->stats_reply,
				  min(size, sizeof(wifi->stats_reply)),
				  fw_band, chain_rssi, &chains,
				  &noise, &noise_valid)) {
		wifi->station_chain_signal[0] = chain_rssi[0];
		wifi->station_chain_signal[1] = chain_rssi[1];
		wifi->station_chains = chains;
		if (noise_valid) {
			wifi->station_phy_noise = noise;
			wifi->station_phy_noise_valid = true;
		}
	}
	return ret;
}

static void fill_station(struct centauri_wifi *wifi, struct station_info *sinfo)
{
	const struct net_device_stats *stats = &wifi->netdev->stats;
	struct ieee80211_channel *chan;
	struct cfg80211_bss *bss;

	if (time_after_eq(jiffies, wifi->station_signal_at + HZ))
		query_link_status(wifi);
	if (time_after_eq(jiffies, wifi->station_counters_at + HZ))
		query_sta_counters(wifi);
	if (time_after_eq(jiffies, wifi->station_beacon_at + HZ))
		query_beacon_stats(wifi);
	chan = wifi->station_chandef_valid ? wifi->station_chandef.chan :
					     wifi->connect_channel;
	if (chan && time_after_eq(jiffies, wifi->station_tx_info_at + HZ))
		query_tx_info(wifi, chan);
	if (chan && time_after_eq(jiffies, wifi->station_phy_dump_at + HZ))
		query_phy_dump(wifi, chan);
	sinfo->filled |= BIT_ULL(NL80211_STA_INFO_CONNECTED_TIME) |
			 BIT_ULL(NL80211_STA_INFO_RX_PACKETS) |
			 BIT_ULL(NL80211_STA_INFO_TX_PACKETS) |
			 BIT_ULL(NL80211_STA_INFO_RX_BYTES64) |
			 BIT_ULL(NL80211_STA_INFO_TX_BYTES64) |
			 BIT_ULL(NL80211_STA_INFO_TX_FAILED) |
			 BIT_ULL(NL80211_STA_INFO_RX_DROP_MISC);
	sinfo->connected_time = (jiffies - wifi->connected_at) / HZ;
	sinfo->rx_packets = stats->rx_packets - wifi->station_start.rx_packets;
	sinfo->tx_packets = stats->tx_packets - wifi->station_start.tx_packets;
	sinfo->rx_bytes = stats->rx_bytes - wifi->station_start.rx_bytes;
	sinfo->tx_bytes = stats->tx_bytes - wifi->station_start.tx_bytes;
	sinfo->tx_failed = stats->tx_errors - wifi->station_start.tx_errors;
	if (wifi->station_tx_fail_valid && wifi->station_tx_fail_base_valid)
		sinfo->tx_failed +=
			wifi->station_tx_fail - wifi->station_tx_fail_base;
	sinfo->rx_dropped_misc = stats->rx_dropped - wifi->station_start.rx_dropped;
	if (wifi->station_tx_retrans_valid &&
	    wifi->station_tx_retrans_base_valid) {
		sinfo->filled |= BIT_ULL(NL80211_STA_INFO_TX_RETRIES);
		sinfo->tx_retries =
			wifi->station_tx_retrans - wifi->station_tx_retrans_base;
	}
	if (wifi->station_beacon_valid && wifi->station_beacon_base_valid) {
		sinfo->filled |= BIT_ULL(NL80211_STA_INFO_BEACON_RX) |
				 BIT_ULL(NL80211_STA_INFO_BEACON_LOSS);
		sinfo->rx_beacon =
			wifi->station_beacon_rx - wifi->station_beacon_rx_base;
		sinfo->beacon_loss_count =
			wifi->station_beacon_loss -
			wifi->station_beacon_loss_base;
	}
	if (wifi->station_tx_rate_valid) {
		sinfo->filled |= BIT_ULL(NL80211_STA_INFO_TX_BITRATE);
		sinfo->txrate = wifi->station_tx_rate;
	} else if (wifi->station_chip_tx_rate_valid) {
		sinfo->filled |= BIT_ULL(NL80211_STA_INFO_TX_BITRATE);
		sinfo->txrate = (struct rate_info){
			.legacy = DIV_ROUND_CLOSEST(wifi->station_chip_tx_rate_kbps,
						    100),
		};
	}
	if (wifi->station_rx_rate_valid) {
		sinfo->filled |= BIT_ULL(NL80211_STA_INFO_RX_BITRATE);
		sinfo->rxrate = (struct rate_info){
			.legacy = DIV_ROUND_CLOSEST(wifi->station_rx_rate_kbps,
						    100),
		};
	}
	if (wifi->station_signal_valid) {
		sinfo->filled |= BIT_ULL(NL80211_STA_INFO_SIGNAL);
		sinfo->signal = wifi->station_signal;
	} else {
		bss = wifi_joined_bss(wifi);
		if (bss) {
			sinfo->filled |= BIT_ULL(NL80211_STA_INFO_SIGNAL);
			sinfo->signal = bss->signal / 100;
			cfg80211_put_bss(wifi->wiphy, bss);
		}
	}
	if (wifi->station_chains) {
		sinfo->filled |= BIT_ULL(NL80211_STA_INFO_CHAIN_SIGNAL);
		sinfo->chains = wifi->station_chains;
		sinfo->chain_signal[0] = wifi->station_chain_signal[0];
		sinfo->chain_signal[1] = wifi->station_chain_signal[1];
	}
}

static int wifi_get_tx_power(struct wiphy *wiphy, struct wireless_dev *wdev,
			     int radio_idx, unsigned int link_id, int *dbm)
{
	struct centauri_wifi *wifi = *(struct centauri_wifi **)wiphy_priv(wiphy);
	struct ieee80211_channel *chan;
	int ret = 0;

	mutex_lock(&wifi->lock);
	if (wdev != &wifi->wdev || !wifi->connected ||
	    READ_ONCE(wifi->stopping) || !wifi->connect_channel) {
		ret = -ENODATA;
		goto out;
	}
	chan = wifi->station_chandef_valid ? wifi->station_chandef.chan :
					     wifi->connect_channel;
	if (time_after_eq(jiffies, wifi->station_tx_info_at + HZ))
		query_tx_info(wifi, chan);
	if (!wifi->station_tx_power_valid) {
		ret = -ENODATA;
		goto out;
	}
	*dbm = wifi->station_tx_power_dbm;
out:
	mutex_unlock(&wifi->lock);
	return ret;
}

static int wifi_get_station(struct wiphy *wiphy, struct wireless_dev *wdev,
			    const u8 *mac, struct station_info *sinfo)
{
	struct centauri_wifi *wifi = *(struct centauri_wifi **)wiphy_priv(wiphy);
	int ret = 0;

	mutex_lock(&wifi->lock);
	if (wdev != &wifi->wdev || !wifi->connected ||
	    READ_ONCE(wifi->stopping) || !ether_addr_equal(mac, wifi->bssid))
		ret = -ENOENT;
	else
		fill_station(wifi, sinfo);
	mutex_unlock(&wifi->lock);
	return ret;
}

static int wifi_dump_station(struct wiphy *wiphy, struct wireless_dev *wdev,
			     int index, u8 *mac, struct station_info *sinfo)
{
	struct centauri_wifi *wifi = *(struct centauri_wifi **)wiphy_priv(wiphy);
	int ret = 0;

	mutex_lock(&wifi->lock);
	if (index || wdev != &wifi->wdev || !wifi->connected ||
	    READ_ONCE(wifi->stopping)) {
		ret = -ENOENT;
	} else {
		ether_addr_copy(mac, wifi->bssid);
		fill_station(wifi, sinfo);
	}
	mutex_unlock(&wifi->lock);
	return ret;
}

static int wifi_get_channel(struct wiphy *wiphy, struct wireless_dev *wdev,
			    unsigned int link_id, struct cfg80211_chan_def *chandef)
{
	struct centauri_wifi *wifi = *(struct centauri_wifi **)wiphy_priv(wiphy);
	int ret = 0;

	mutex_lock(&wifi->lock);
	if (wdev != &wifi->wdev || !wifi->connected ||
	    READ_ONCE(wifi->stopping) || !wifi->connect_channel) {
		ret = -ENODATA;
		goto out;
	}
	if (time_after_eq(jiffies, wifi->station_signal_at + HZ))
		query_link_status(wifi);
	if (wifi->station_chandef_valid)
		*chandef = wifi->station_chandef;
	else
		cfg80211_chandef_create(chandef, wifi->connect_channel,
					NL80211_CHAN_HT20);
out:
	mutex_unlock(&wifi->lock);
	return ret;
}

/*
 * The factory address, which the loader copies from ADT /chosen
 * mac-address-wifi0 into the wifi0 alias target's local-mac-address.
 * A board without that node keeps a random address.
 */
static void centauri_wifi_set_address(struct centauri_wifi *wifi)
{
	struct device_node *np = of_find_node_by_path("wifi0");
	u8 addr[ETH_ALEN];
	int ret = np ? of_get_mac_address(np, addr) : -ENODEV;

	of_node_put(np);
	if (!ret && is_valid_ether_addr(addr)) {
		eth_hw_addr_set(wifi->netdev, addr);
		ether_addr_copy(wifi->wiphy->perm_addr, addr);
		return;
	}
	eth_hw_addr_random(wifi->netdev);
}

static int query_sta_noise(struct centauri_wifi *wifi)
{
	u8 payload[22] = {};
	size_t size = sizeof(wifi->stats_reply);
	s8 noise;
	int ret;

	if (READ_ONCE(wifi->transport_paused))
		return -EBUSY;
	wifi->station_noise_valid = false;
	wifi->station_noise_at = jiffies;
	if (!wifi->configured)
		return -ENETDOWN;
	/* Statistics bit 7 (station info), TLV 363 scope 2, reply TLV 0xf908. */
	put_unaligned_le64(BIT_ULL(7), payload);
	payload[8] = 1;
	put_unaligned_le16(363, payload + 9);
	put_unaligned_le16(9, payload + 11);
	payload[21] = 2;
	ret = command_reply(wifi, 0xb0001, payload, sizeof(payload),
			    wifi->stats_reply, &size);
	if (!ret && sta_info_noise_read(wifi->stats_reply,
					min(size, sizeof(wifi->stats_reply)),
					&noise)) {
		wifi->station_noise = noise;
		wifi->station_noise_valid = true;
	}
	return ret;
}

static int query_cca_survey(struct centauri_wifi *wifi,
			    const struct ieee80211_channel *chan)
{
	u8 payload[22] = {};
	u64 active_ms, busy_ms, tx_ms, slice;
	int fw_band, ret;

	if (READ_ONCE(wifi->transport_paused))
		return -EBUSY;
	wifi->station_cca_valid = false;
	wifi->station_cca_at = jiffies;
	if (!wifi->configured)
		return -ENETDOWN;
	fw_band = firmware_band(chan->band);
	if (fw_band < 0)
		return fw_band;
	/* Statistics bit 3 (CCA report), TLV 363 slice/scope 1, reply TLV 0xf903. */
	put_unaligned_le64(BIT_ULL(3), payload);
	payload[8] = 1;
	put_unaligned_le16(363, payload + 9);
	put_unaligned_le16(9, payload + 11);
	payload[21] = 1;
	slice = fw_band == 0 ? 1 : 2;
	for (;;) {
		size_t size = sizeof(wifi->stats_reply);

		put_unaligned_le64(slice, payload + 13);
		ret = command_reply(wifi, 0xb0001, payload, sizeof(payload),
				    wifi->stats_reply, &size);
		if (!ret && cca_survey_read(wifi->stats_reply,
					    min(size, sizeof(wifi->stats_reply)),
					    chan->hw_value, fw_band,
					    wifi->station_bw_primary,
					    wifi->station_chandef_valid,
					    &active_ms, &busy_ms, &tx_ms)) {
			wifi->station_cca_active_ms = active_ms;
			wifi->station_cca_busy_ms = busy_ms;
			wifi->station_cca_tx_ms = tx_ms;
			wifi->station_cca_valid = true;
			return 0;
		}
		if (slice == 1)
			break;
		slice = 1;
	}
	return ret;
}

static int wifi_dump_survey(struct wiphy *wiphy, struct net_device *netdev,
			    int idx, struct survey_info *survey)
{
	struct centauri_wifi *wifi = *(struct centauri_wifi **)wiphy_priv(wiphy);
	struct ieee80211_channel *chan;
	int ret = 0;

	if (idx)
		return -ENOENT;
	mutex_lock(&wifi->lock);
	if (!wifi->connected || READ_ONCE(wifi->stopping) ||
	    !wifi->connect_channel) {
		ret = -ENOENT;
		goto out;
	}
	if (!wifi->station_chandef_valid &&
	    time_after_eq(jiffies, wifi->station_signal_at + HZ))
		query_link_status(wifi);
	if (time_after_eq(jiffies, wifi->station_noise_at + HZ))
		query_sta_noise(wifi);
	chan = wifi->station_chandef_valid ? wifi->station_chandef.chan :
					     wifi->connect_channel;
	if (time_after_eq(jiffies, wifi->station_cca_at + HZ))
		query_cca_survey(wifi, chan);
	if (!wifi->station_noise_valid &&
	    time_after_eq(jiffies, wifi->station_phy_dump_at + HZ))
		query_phy_dump(wifi, chan);
	survey->channel = chan;
	survey->filled = SURVEY_INFO_IN_USE;
	if (wifi->station_noise_valid) {
		survey->filled |= SURVEY_INFO_NOISE_DBM;
		survey->noise = wifi->station_noise;
	} else if (wifi->station_phy_noise_valid) {
		survey->filled |= SURVEY_INFO_NOISE_DBM;
		survey->noise = wifi->station_phy_noise;
	}
	if (wifi->station_cca_valid) {
		survey->filled |= SURVEY_INFO_TIME | SURVEY_INFO_TIME_BUSY |
				  SURVEY_INFO_TIME_TX;
		survey->time = wifi->station_cca_active_ms;
		survey->time_busy = wifi->station_cca_busy_ms;
		survey->time_tx = wifi->station_cca_tx_ms;
	}
out:
	mutex_unlock(&wifi->lock);
	return ret;
}

static const struct cfg80211_ops wifi_ops = {
	.scan = wifi_scan,
	.abort_scan = wifi_abort_scan,
	.connect = wifi_connect,
	.disconnect = wifi_disconnect,
	.set_power_mgmt = wifi_power_mgmt,
	.get_tx_power = wifi_get_tx_power,
	.get_station = wifi_get_station,
	.dump_station = wifi_dump_station,
	.get_channel = wifi_get_channel,
	.dump_survey = wifi_dump_survey,
};

static int net_open(struct net_device *netdev)
{
	struct centauri_wifi *wifi = *(struct centauri_wifi **)netdev_priv(netdev);

	netif_carrier_off(netdev);
	netif_stop_queue(netdev);
	skb_queue_purge(&wifi->rx_gro_queue);
	napi_enable(&wifi->rx_napi);
	return 0;
}

static int net_stop(struct net_device *netdev)
{
	struct centauri_wifi *wifi = *(struct centauri_wifi **)netdev_priv(netdev);

	cancel_scan(wifi);
	netif_stop_queue(netdev);
	skb_queue_purge(&wifi->data_tx_queue);
	napi_disable(&wifi->rx_napi);
	skb_queue_purge(&wifi->rx_gro_queue);
	return 0;
}

static netdev_tx_t net_transmit(struct sk_buff *skb, struct net_device *netdev)
{
	struct centauri_wifi *wifi = *(struct centauri_wifi **)netdev_priv(netdev);

	if (READ_ONCE(wifi->transport_paused)) {
		netif_stop_queue(netdev);
		return NETDEV_TX_BUSY;
	}
	if (READ_ONCE(wifi->stopping) || !READ_ONCE(wifi->connected) ||
	    skb->len < ETH_HLEN || skb->len > BUFFER_SIZE) {
		netdev->stats.tx_dropped++;
		dev_kfree_skb_any(skb);
		return NETDEV_TX_OK;
	}
	if (skb_queue_len(&wifi->data_tx_queue) >= DATA_TX_QUEUE_LIMIT) {
		netif_stop_queue(netdev);
		return NETDEV_TX_BUSY;
	}
	sk_pacing_shift_update(skb->sk, DATA_TX_PACING_SHIFT);
	skb_queue_tail(&wifi->data_tx_queue, skb);
	if (skb_queue_len(&wifi->data_tx_queue) >= DATA_TX_QUEUE_LIMIT)
		netif_stop_queue(netdev);
	mod_delayed_work(system_percpu_wq, &wifi->link_work, 0);
	return NETDEV_TX_OK;
}

static int station_reconfigure(struct centauri_wifi *wifi, const u8 *address)
{
	const u8 stop[5] = {};
	int ret;

	wifi->configured = false;
	ret = command(wifi, 3, stop, sizeof(stop));
	if (!ret)
		ret = station_configure(wifi, address);
	if (!ret)
		wifi->configured = true;
	return ret;
}

static int net_set_mac_address(struct net_device *netdev, void *data)
{
	struct centauri_wifi *wifi = *(struct centauri_wifi **)netdev_priv(netdev);
	struct sockaddr *address = data;
	int ret, restore;

	ret = eth_prepare_mac_addr_change(netdev, data);
	if (ret)
		return ret;
	mutex_lock(&wifi->lock);
	if (READ_ONCE(wifi->stopping)) {
		ret = -ENODEV;
		goto out;
	}
	if (!wifi->configured) {
		ret = -ENETDOWN;
		goto out;
	}
	if (wifi->connecting || wifi->connected || wifi->scan_request) {
		ret = -EBUSY;
		goto out;
	}
	if (ether_addr_equal(netdev->dev_addr, address->sa_data))
		goto out;

	ret = station_reconfigure(wifi, (const u8 *)address->sa_data);
	if (!ret) {
		eth_commit_mac_addr_change(netdev, data);
	} else {
		restore = station_reconfigure(wifi, netdev->dev_addr);
		if (restore)
			dev_err(&wifi->control->pdev->dev,
				"Wi-Fi MAC restore failed: %d (change: %d)\n",
				restore, ret);
	}
out:
	mutex_unlock(&wifi->lock);
	return ret;
}

static const struct ethtool_ops centauri_ethtool_ops = {
	.get_drvinfo = cfg80211_get_drvinfo,
};

static const struct net_device_ops net_ops = {
	.ndo_open = net_open,
	.ndo_stop = net_stop,
	.ndo_start_xmit = net_transmit,
	.ndo_validate_addr = eth_validate_addr,
	.ndo_set_mac_address = net_set_mac_address,
};

/* Direct memory region map (opcode 8, region ID 1) attaches a coherent
 * diagnostic DRAM log ring buffer. Firmware writes a control header at the
 * start of the buffer; log contents stay root-only via debugfs.
 */
static ssize_t fwlog_start_write(struct file *file, const char __user *input,
				size_t count, loff_t *position)
{
	struct centauri_wifi *wifi = file->private_data;
	u8 message[24] = { 8, 0, 1, 0 };
	unsigned int action;
	unsigned long deadline;
	int ret;

	ret = kstrtouint_from_user(input, count, 0, &action);
	if (ret || action != 1)
		return ret ?: -EINVAL;
	mutex_lock(&wifi->lock);
	mutex_lock(&wifi->fwlog_lock);
	if (!wifi->configured || wifi->connecting || wifi->connected || wifi->scan_request) {
		ret = -EBUSY;
		goto out;
	}
	if (wifi->fwlog_attempted) {
		ret = -EALREADY;
		goto out;
	}
	ret = centauri_alpha_log_buffer(wifi->control, &wifi->fwlog_buffer,
					 &wifi->fwlog_dma, &wifi->fwlog_size);
	if (ret)
		goto out;
	put_unaligned_le32(wifi->fwlog_size, message + 4);
	put_unaligned_le64(wifi->fwlog_dma, message + 8);
	wifi->fwlog_attempted = true;
	ret = alpha_message(wifi, message, sizeof(message));
	if (ret)
		goto report;
	deadline = jiffies + msecs_to_jiffies(2000);
	do {
		if (readl(wifi->a.bar + 8) != 2) {
			ret = -EIO;
			goto report;
		}
		dma_rmb();
		if (get_unaligned_le16(wifi->fwlog_buffer) == 0x201) {
			wifi->fwlog_ready = true;
			goto report;
		}
		usleep_range(1000, 2000);
	} while (time_before(jiffies, deadline));
	ret = -ETIMEDOUT;
report:
	dev_info(&wifi->control->pdev->dev,
		 "Wi-Fi DRAM logger: rc=%d initialized=%u stage=%u\n",
		 ret, wifi->fwlog_ready, readl(wifi->a.bar + 8));
out:
	mutex_unlock(&wifi->fwlog_lock);
	mutex_unlock(&wifi->lock);
	return ret ?: count;
}

static const struct file_operations fwlog_start_ops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.write = fwlog_start_write,
};

static ssize_t fwlog_status_read(struct file *file, char __user *output,
				size_t count, loff_t *position)
{
	struct centauri_wifi *wifi = file->private_data;
	char text[320];
	u8 header[0xb4] = { 0 };
	int length;

	mutex_lock(&wifi->fwlog_lock);
	if (wifi->fwlog_buffer) {
		dma_rmb();
		memcpy(header, wifi->fwlog_buffer, sizeof(header));
	}
	length = scnprintf(text, sizeof(text),
		"attempted=%u initialized=%u bytes=%zu version=%#x blocks=%u block_size=%u mask=%#x data_offset=%u consumer=%u producer=%u header14=%u header18=%u header1c=%u connecting=%u scanning=%u\n",
		wifi->fwlog_attempted, wifi->fwlog_ready, wifi->fwlog_size,
		get_unaligned_le16(header), get_unaligned_le16(header + 2),
		get_unaligned_le32(header + 4), get_unaligned_le32(header + 8),
		get_unaligned_le32(header + 12), get_unaligned_le32(header + 16),
		get_unaligned_le32(header + 0xb0),
		get_unaligned_le32(header + 20), get_unaligned_le32(header + 24),
		get_unaligned_le32(header + 28),
		!!READ_ONCE(wifi->connecting), !!READ_ONCE(wifi->scan_request));
	mutex_unlock(&wifi->fwlog_lock);
	return simple_read_from_buffer(output, count, position, text, length);
}

static const struct file_operations fwlog_status_ops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.read = fwlog_status_read,
	.llseek = default_llseek,
};

/* Advance the host consumer index (+0x10) against the firmware producer
 * (+0xb0), keeping three descriptors reserved for in-flight DMA writes.
 */
static ssize_t fwlog_ack_write(struct file *file, const char __user *input,
			       size_t count, loff_t *position)
{
	struct centauri_wifi *wifi = file->private_data;
	u8 *header;
	u32 requested, consumer, producer, pending;
	u16 blocks;
	int ret;

	ret = kstrtou32_from_user(input, count, 0, &requested);
	if (ret)
		return ret;
	mutex_lock(&wifi->fwlog_lock);
	if (READ_ONCE(wifi->transport_paused)) {
		ret = -EBUSY;
		goto out;
	}
	if (!wifi->fwlog_ready || readl(wifi->a.bar + 8) != 2) {
		ret = -ENETDOWN;
		goto out;
	}
	header = wifi->fwlog_buffer;
	dma_rmb();
	blocks = get_unaligned_le16(header + 2);
	consumer = get_unaligned_le32(header + 0x10);
	producer = get_unaligned_le32(header + 0xb0);
	pending = producer - consumer;
	if (get_unaligned_le16(header) != 0x201 || blocks < 8 ||
	    pending > blocks || pending < 3 || requested - consumer > pending - 3) {
		ret = -EINVAL;
		goto out;
	}
	WRITE_ONCE(*(__le32 *)(header + 0x10), cpu_to_le32(requested));
	dma_wmb();
	ret = 0;
out:
	mutex_unlock(&wifi->fwlog_lock);
	return ret ?: count;
}

static const struct file_operations fwlog_ack_ops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.write = fwlog_ack_write,
};

/* Root-only binary diagnostics. Save directly to a protected artifact, never
 * the console or a text log. The device may update blocks during a read.
 */
static ssize_t fwlog_buffer_read(struct file *file, char __user *output,
				size_t count, loff_t *position)
{
	struct centauri_wifi *wifi = file->private_data;
	ssize_t ret;

	mutex_lock(&wifi->fwlog_lock);
	if (!wifi->fwlog_ready) {
		ret = -ENODATA;
	} else {
		dma_rmb();
		ret = simple_read_from_buffer(output, count, position,
					    wifi->fwlog_buffer, wifi->fwlog_size);
	}
	mutex_unlock(&wifi->fwlog_lock);
	return ret;
}

static const struct file_operations fwlog_buffer_ops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.read = fwlog_buffer_read,
	.llseek = default_llseek,
};

/* Data-ring open messages append a 10-byte client extension after the
 * ACIPC ring header. Called with wifi->lock held.
 */
static int data_probe_locked(struct centauri_wifi *wifi, unsigned int id, bool internal)
{
	u8 message[0x34 + 10] = { 0 };
	unsigned int vector, offset, size, bit, doorbell, irq;
	bool transfer = false;
	int ret;

	if (id == 76) {
		vector = 192;
		offset = 0x200000;
		size = 2048;
		bit = 0;
		doorbell = 18;
		irq = 3;
	} else if (id == 14) {
		vector = 206;
		offset = 0x220000;
		size = 256;
		bit = 1;
		doorbell = 11;
		irq = 8;
	} else if (id == 15) {
		vector = 207;
		offset = 0x240000;
		size = 128;
		bit = 2;
		doorbell = 11;
		irq = 8;
	} else if (id == 24 || id == 25 || id == 12) {
		transfer = true;
		vector = id;
		offset = id == 12 ? 0x520000 : id == 24 ? 0x500000 : 0x540000;
		size = 128;
		bit = id == 12 ? 4 : id == 24 ? 3 : 5;
		doorbell = id == 12 ? 11 : 18;
		irq = id == 12 ? 8 : 3;
	} else {
		return -EINVAL;
	}
	if (readl(wifi->a.bar + 8) != 2) {
		ret = -EIO;
		return ret;
	}
	if ((!internal && wifi->connecting) || wifi->scan_request ||
	    (wifi->data_probe_done & BIT(bit))) {
		ret = -EBUSY;
		return ret;
	}
	wifi->data_probe_done |= BIT(bit);
	memset(wifi->a.window + offset, 0, size * 32);
	index_write(wifi, transfer ? 1 : 0, vector, 0);
	index_write(wifi, transfer ? 3 : 2, vector, 0);
	message[0] = transfer ? 1 : 2;
	message[2] = transfer && id == 12 ? 0 : 4;
	put_unaligned_le16(id, message + 4);
	put_unaligned_le16(vector, message + 6);
	put_unaligned_le64(wifi->a.window_dma + offset, message + 8);
	if (transfer) {
		put_unaligned_le64(~0ULL, message + 0x10);
		put_unaligned_le16(size, message + 0x18);
		put_unaligned_le16(id == 12 ? 12 : 76, message + 0x1a);
		put_unaligned_le16(doorbell, message + 0x1c);
		put_unaligned_le16(id == 12 ? 0x220 : 0, message + 0x1e);
		put_unaligned_le32(16, message + 0x20);
		put_unaligned_le16(irq, message + 0x24);
	} else {
		put_unaligned_le16(size, message + 0x10);
		put_unaligned_le16(id == 15 ? 12 : 0xffff, message + 0x12);
		put_unaligned_le16(doorbell, message + 0x14);
		put_unaligned_le32(1, message + 0x18);
		put_unaligned_le16(irq, message + 0x1c);
	}
	/* The host MCBC transfer ring takes version 0, hardware
	 * interface 0, TID 6, AC 3, and
	 * six broadcast destination bytes. The default probe is unchanged.
	 */
	if (id == 25 && data_tx_broadcast) {
		message[0x34 + 2] = 6;
		message[0x34 + 3] = 3;
		eth_broadcast_addr(message + 0x34 + 4);
	}
	ret = alpha_message(wifi, message, (transfer ? 0x34 : 0x2c) + 10);
	msleep(1000);
	dev_info(&wifi->control->pdev->dev,
		 "data %s probe id=%u index=%u extension=10 rc=%d stage=%u head=%u tail=%u\n",
		 transfer ? "TR" : "CR", id, vector, ret, readl(wifi->a.bar + 8),
		 index_read(wifi, transfer ? 3 : 0, vector),
		 index_read(wifi, transfer ? 1 : 2, vector));
	if (!ret && readl(wifi->a.bar + 8) != 2)
		ret = -EIO;
	if (!ret && transfer && id == 12) {
		unsigned int slot;

		/* Give the receive engine bounded, individually identifiable buffers.
		 * No recycling or network delivery until the completion ABI is proven.
		 */
		for (slot = 0; slot < size - 1; slot++) {
			u8 *descriptor = wifi->a.window + offset + slot * 16;
			u32 buffer = DATA_RX_BUFFER + slot * 4096;

			memset(wifi->a.window + buffer, 0, 4096);
			put_unaligned_le32(1 | (4096 << 8), descriptor);
			put_unaligned_le64(wifi->a.window_dma + buffer, descriptor + 4);
			put_unaligned_le32(slot + 1, descriptor + 12);
		}
		dma_wmb();
		index_write(wifi, 3, vector, size - 1);
		dma_wmb();
		writel(1, wifi->a.bar + 0x1050);
		msleep(100);
		dev_info(&wifi->control->pdev->dev,
			 "data RX buffers posted=%u stage=%u tr_tail=%u cr_head=%u\n",
			 size - 1, readl(wifi->a.bar + 8), index_read(wifi, 1, 12),
			 index_read(wifi, 0, 207));
	}
	return ret;
}

static ssize_t data_probe_write(struct file *file, const char __user *input,
			       size_t count, loff_t *position)
{
	struct centauri_wifi *wifi = file->private_data;
	unsigned int id;
	int ret;

	ret = kstrtouint_from_user(input, count, 0, &id);
	if (ret)
		return ret;
	mutex_lock(&wifi->lock);
	ret = data_probe_locked(wifi, id, false);
	mutex_unlock(&wifi->lock);
	return ret ?: count;
}

static int transmit_eapol_start(struct centauri_wifi *wifi)
{
	u16 head = index_read(wifi, 3, 24);
	u8 *descriptor = wifi->a.window + 0x500000 + head * 32;
	u8 *packet = wifi->a.window + 0xf00000 + head * 64;
	u64 metadata = 0;
	unsigned int attempt;

	if (READ_ONCE(wifi->transport_paused))
		return -EBUSY;
	if (readl(wifi->a.bar + 8) != 2 || !(wifi->data_probe_done & BIT(3)))
		return -ENETDOWN;
	/* Eight independent buffers bound the probe without ring reuse. */
	if (head >= 8 || index_read(wifi, 1, 24) != head)
		return -EBUSY;
	if (eapol_unicast && (!wifi->associated || !is_valid_ether_addr(wifi->bssid)))
		return -ENOTCONN;
	/* A standards-defined EAPOL-Start, with no credential/key material.
	 * Descriptor flags 3 mean a DMA buffer plus the 16-byte WLAN footer.
	 * Interface zero and default TX policy have a zero footer.
	 */
	memset(packet, 0, 64);
	if (eapol_unicast)
		memcpy(packet, wifi->bssid, ETH_ALEN);
	else
		memcpy(packet, "\x01\x80\xc2\x00\x00\x03", ETH_ALEN);
	memcpy(packet + ETH_ALEN, wifi->netdev->dev_addr, ETH_ALEN);
	put_unaligned_be16(ETH_P_PAE, packet + 12);
	packet[14] = 2;
	packet[15] = 1;
	memset(descriptor, 0, 32);
	put_unaligned_le32(3 | (18 << 8), descriptor);
	put_unaligned_le64(wifi->a.window_dma + 0xf00000 + head * 64, descriptor + 4);
	put_unaligned_le16(head + 1, descriptor + 12);
	if (eapol_control_profile)
		metadata |= BIT_ULL(60) | BIT_ULL(48);
	if (eapol_relative_lifetime) {
		/* A relative expiration time is mode 6 in
		 * bits 29:27 and microseconds in the second metadata word.
		 */
		metadata |= 6ULL << 27;
		put_unaligned_le64(2000000, descriptor + 24);
	}
	put_unaligned_le64(metadata, descriptor + 16);
	dma_wmb();
	index_write(wifi, 3, 24, head + 1);
	dma_wmb();
	writel(1, wifi->a.bar + 0x1030);
	for (attempt = 0; attempt < 20; attempt++) {
		if (index_read(wifi, 1, 24) == head + 1 || readl(wifi->a.bar + 8) != 2)
			break;
		usleep_range(5000, 6000);
	}
	dev_info(&wifi->control->pdev->dev,
		 "EAPOL-Start DMA probe: slot=%u associated=%d unicast=%d relative_expiry=%d stage=%u tr_tail=%u cr_head=%u\n",
		 head, wifi->associated, eapol_unicast, eapol_relative_lifetime, readl(wifi->a.bar + 8),
		 index_read(wifi, 1, 24), index_read(wifi, 0, 192));
	return readl(wifi->a.bar + 8) == 2 ? 0 : -EIO;
}

static ssize_t eapol_start_write(struct file *file, const char __user *input,
				size_t count, loff_t *position)
{
	struct centauri_wifi *wifi = file->private_data;
	bool enabled;
	int ret;

	ret = kstrtobool_from_user(input, count, &enabled);
	if (ret || !enabled)
		return ret ?: -EINVAL;
	mutex_lock(&wifi->lock);
	if (wifi->connecting || wifi->scan_request) {
		ret = -EBUSY;
		goto out;
	}
	ret = transmit_eapol_start(wifi);
out:
	mutex_unlock(&wifi->lock);
	return ret ?: count;
}

static const struct file_operations eapol_start_ops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.write = eapol_start_write,
};

static const struct file_operations data_probe_ops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.write = data_probe_write,
};

static ssize_t data_status_read(struct file *file, char __user *output,
			       size_t count, loff_t *position)
{
	struct centauri_wifi *wifi = file->private_data;
	char status[1536];
	unsigned int i, length;

	mutex_lock(&wifi->lock);
	dma_rmb();
	length = scnprintf(status, sizeof(status),
			   "stage=%u tx_cr=%u tx_tr=%u/%u rx_cr=%u rx_tr=%u/%u received=%u reposted=%u bad=%u samples=%u normal_tx_tr=%u/%u submitted=%u completed=%u failed=%u queued=%u coalesced=%u tx_bad=%u\n",
			   readl(wifi->a.bar + 8), index_read(wifi, 0, 192),
			   index_read(wifi, 1, 24), index_read(wifi, 3, 24),
			   index_read(wifi, 0, 207), index_read(wifi, 1, 12),
			   index_read(wifi, 3, 12), wifi->data_rx_received,
			   wifi->data_rx_reposted, wifi->data_rx_bad,
			   le32_to_cpu(wifi->rx_snapshot.count),
			   index_read(wifi, 1, 25), index_read(wifi, 3, 25),
			   wifi->data_tx_submitted, wifi->data_tx_completed,
			   wifi->data_tx_failed, skb_queue_len(&wifi->data_tx_queue),
			   wifi->data_tx_coalesced, wifi->data_tx_bad);
	for (i = 0; i < 8; i++) {
		u8 *entry = wifi->a.window + 0x240000 + i * 32;

		length += scnprintf(status + length, sizeof(status) - length,
			"rx_completion[%u]=%08x %08x %08x %08x %08x %08x %08x %08x\n",
			i, get_unaligned_le32(entry), get_unaligned_le32(entry + 4),
			get_unaligned_le32(entry + 8), get_unaligned_le32(entry + 12),
			get_unaligned_le32(entry + 16), get_unaligned_le32(entry + 20),
			get_unaligned_le32(entry + 24), get_unaligned_le32(entry + 28));
	}
	for (i = 0; i < 8; i++) {
		u8 *entry = wifi->a.window + 0x200000 + i * 32;

		length += scnprintf(status + length, sizeof(status) - length,
			"tx_completion[%u]=%08x %08x %08x %08x %08x %08x %08x %08x\n",
			i, get_unaligned_le32(entry), get_unaligned_le32(entry + 4),
			get_unaligned_le32(entry + 8), get_unaligned_le32(entry + 12),
			get_unaligned_le32(entry + 16), get_unaligned_le32(entry + 20),
			get_unaligned_le32(entry + 24), get_unaligned_le32(entry + 28));
	}
	mutex_unlock(&wifi->lock);
	return simple_read_from_buffer(output, count, position, status, length);
}

static const struct file_operations data_status_ops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.read = data_status_read,
};

/* Contains private network traffic: retrieve only as a protected binary file.
 * Samples are copied once into host memory and never change after count 8.
 */
static ssize_t data_rx_snapshot_read(struct file *file, char __user *output,
				    size_t count, loff_t *position)
{
	struct centauri_wifi *wifi = file->private_data;
	ssize_t ret;

	mutex_lock(&wifi->lock);
	ret = simple_read_from_buffer(output, count, position, &wifi->rx_snapshot,
				      sizeof(wifi->rx_snapshot));
	mutex_unlock(&wifi->lock);
	return ret;
}

static const struct file_operations data_rx_snapshot_ops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.read = data_rx_snapshot_read,
	.llseek = default_llseek,
};

static ssize_t country_status_read(struct file *file, char __user *output,
				  size_t count, loff_t *position)
{
	struct centauri_wifi *wifi = file->private_data;
	char status[96];
	unsigned int length;
	int ret;

	mutex_lock(&wifi->lock);
	wifi->country_valid = false;
	ret = wifi->configured ? command(wifi, 10, NULL, 0) : -ENETDOWN;
	length = scnprintf(status, sizeof(status), "rc=%d valid=%d country_bytes=%02x/%02x/%02x\n",
			   ret, wifi->country_valid, wifi->country[0], wifi->country[1], wifi->country[2]);
	mutex_unlock(&wifi->lock);
	return simple_read_from_buffer(output, count, position, status, length);
}

static const struct file_operations country_ops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.read = country_status_read,
};

static int query_link_status(struct centauri_wifi *wifi)
{
	if (READ_ONCE(wifi->transport_paused))
		return -EBUSY;
	wifi->link_status_valid = wifi->emlsr_valid = false;
	wifi->link_count = wifi->link_status_size = 0;
	wifi->link_tlv_count = 0;
	wifi->station_signal_valid = false;
	wifi->station_chandef_valid = false;
	wifi->station_signal_at = jiffies;
	return wifi->configured ? command(wifi, 20, NULL, 0) : -ENETDOWN;
}

static int link_status_show(struct seq_file *seq, void *unused)
{
	struct centauri_wifi *wifi = seq->private;
	unsigned int i;
	int ret;

	mutex_lock(&wifi->lock);
	ret = query_link_status(wifi);
	seq_printf(seq, "rc=%d valid=%d size=%u count=%u emlsr=%d/%u\n", ret,
		   wifi->link_status_valid, wifi->link_status_size, wifi->link_count,
		   wifi->emlsr_valid, wifi->emlsr_mode);
	for (i = 0; i < wifi->link_tlv_count; i++)
		seq_printf(seq, "tag=%u length=%u\n", wifi->link_tags[i], wifi->link_lengths[i]);
	if (wifi->link_status_valid)
		for (i = 0; i < wifi->link_count; i++)
			seq_printf(seq, "link=%u prime=%u channel=%u band=%u\n", i,
				   wifi->link_prime[i], wifi->link_channel[i], wifi->link_band[i]);
	mutex_unlock(&wifi->lock);
	return 0;
}

static int link_status_open(struct inode *inode, struct file *file)
{
	return single_open(file, link_status_show, inode->i_private);
}

static const struct file_operations link_status_ops = {
	.owner = THIS_MODULE,
	.open = link_status_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

static int tx_config_show(struct seq_file *seq, void *unused)
{
	struct centauri_wifi *wifi = seq->private;
	u8 id;
	int ret;

	mutex_lock(&wifi->lock);
	seq_printf(seq, "applied=%u\n", wifi->tx_defaults_applied);
	for (id = 0; id <= 1; id++) {
		wifi->tx_profile_valid = false;
		ret = wifi->configured ? command(wifi, 56, &id, sizeof(id)) : -ENETDOWN;
		seq_printf(seq, "profile=%u rc=%d status=%u valid=%d", id, ret,
			   wifi->reply_status, wifi->tx_profile_valid);
		if (wifi->tx_profile_valid)
			seq_printf(seq, " rate=%#x rts_threshold=%u rts_retries=%u retries=%u type=%u id=%u",
				   get_unaligned_le32(wifi->tx_profile),
				   get_unaligned_le16(wifi->tx_profile + 4), wifi->tx_profile[6],
				   wifi->tx_profile[7], wifi->tx_profile[8], wifi->tx_profile[9]);
		seq_putc(seq, '\n');
	}
	mutex_unlock(&wifi->lock);
	return 0;
}

static int tx_config_open(struct inode *inode, struct file *file)
{
	return single_open(file, tx_config_show, inode->i_private);
}

static ssize_t tx_config_write(struct file *file, const char __user *input,
			       size_t count, loff_t *position)
{
	struct centauri_wifi *wifi = ((struct seq_file *)file->private_data)->private;
	/* Exact configureTxDataCtrlPacketProfile: command 52, TLV 340/10.
	 * 0xa0154 is the TLV header, not the command ID.
	 */
	static const u8 control_profile[] = {
		0x54, 1, 10, 0, 0, 8, 0, 0, 0xff, 0xff, 0xff, 0xff, 1, 1,
	};
	u8 lifetime[20] = {};
	unsigned int action, i;
	int ret;

	ret = kstrtouint_from_user(input, count, 0, &action);
	if (ret || (action != 1 && action != 2))
		return ret ?: -EINVAL;
	mutex_lock(&wifi->lock);
	if (!wifi->configured || wifi->connecting || wifi->scan_request) {
		ret = -EBUSY;
		goto out;
	}
	if (action == 1) {
		/* The default TX packet lifetime is 2000 ms for each AC,
		 * sent to the firmware in microseconds.
		 */
		for (i = 0; i < 4; i++)
			put_unaligned_le32(2000000, lifetime + 4 + 4 * i);
		ret = command(wifi, 43, lifetime, sizeof(lifetime));
	} else {
		ret = command(wifi, 52, control_profile, sizeof(control_profile));
	}
	if (!ret)
		wifi->tx_defaults_applied |= BIT(action - 1);
	dev_info(&wifi->control->pdev->dev, "reference TX setup action=%u rc=%d\n", action, ret);
out:
	mutex_unlock(&wifi->lock);
	return ret ?: count;
}

static const struct file_operations tx_config_ops = {
	.owner = THIS_MODULE,
	.open = tx_config_open,
	.read = seq_read,
	.write = tx_config_write,
	.llseek = seq_lseek,
	.release = single_release,
};

static ssize_t host_wakeup_write(struct file *file, const char __user *input,
				size_t count, loff_t *position)
{
	struct centauri_wifi *wifi = file->private_data;
	/* Host wakeup is state 0 in command 6.
	 * This firmware's SysCmdSetHostPowerState accepts the fixed 27-byte
	 * request without TLVs. Keep its existing event filter: the newer host
	 * adapter's 92-byte event list exceeds this firmware's 86-byte limit.
	 */
	const u8 payload[3] = {};
	unsigned int action;
	int ret;

	ret = kstrtouint_from_user(input, count, 0, &action);
	if (ret || action != 1)
		return ret ?: -EINVAL;
	mutex_lock(&wifi->lock);
	if (!wifi->configured || wifi->connecting || wifi->scan_request || wifi->connected) {
		ret = -EBUSY;
		goto out;
	}
	ret = command(wifi, 6, payload, sizeof(payload));
	dev_info(&wifi->control->pdev->dev, "host wakeup state=0 rc=%d status=%u stage=%u\n",
		 ret, wifi->reply_status, readl(wifi->a.bar + 8));
out:
	mutex_unlock(&wifi->lock);
	return ret ?: count;
}

static const struct file_operations host_wakeup_ops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.write = host_wakeup_write,
};

static int query_station_stats(struct centauri_wifi *wifi)
{
	u8 payload[22] = {};
	int ret;

	/* Station counters: statistic bit 37, all peers.
	 * Its fixed counter record contains no key or packet payload bytes.
	 */
	put_unaligned_le64(BIT_ULL(37), payload);
	payload[8] = 1;
	put_unaligned_le16(363, payload + 9);
	put_unaligned_le16(9, payload + 11);
	wifi->station_stats_valid = false;
	wifi->station_stats_len = 0;
	wifi->stats_reply_size = wifi->stats_tag = wifi->stats_length = 0;
	ret = wifi->configured ? command(wifi, 0xb0001, payload, sizeof(payload)) : -ENETDOWN;
	if (!ret) {
		usleep_range(1000, 2000);
		drain(wifi);
	}
	return ret;
}

static int station_stats_show(struct seq_file *seq, void *unused)
{
	struct centauri_wifi *wifi = seq->private;
	unsigned int i;
	int ret;

	mutex_lock(&wifi->lock);
	ret = query_station_stats(wifi);
	seq_printf(seq, "rc=%d valid=%d size=%u tag=%#x length=%u counters=%u\n", ret,
		   wifi->station_stats_valid, wifi->stats_reply_size, wifi->stats_tag,
		   wifi->stats_length, wifi->station_stats_len);
	if (!ret && wifi->station_stats_valid)
		for (i = 0; i < wifi->station_stats_len; i += 4) {
			u32 value = get_unaligned_le32(wifi->station_stats + i);

			if (value)
				seq_printf(seq, "%03x %u\n", i, value);
		}
	mutex_unlock(&wifi->lock);
	return 0;
}

static int station_stats_open(struct inode *inode, struct file *file)
{
	return single_open(file, station_stats_show, inode->i_private);
}

static const struct file_operations station_stats_ops = {
	.owner = THIS_MODULE,
	.open = station_stats_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

static int rx_crypto_stats_show(struct seq_file *seq, void *unused)
{
	/* RX chip counters: statistic bit 30, response TLV 0xf921, sixteen LE32 counters.
	 * Unlike the station record, this fixed structure contains no MACs.
	 */
	static const char * const names[] = {
		"rx_duplicate_errors", "rx_ampdu_duplicate_errors",
		"rx_unicast_tkip_replay_errors", "rx_unicast_tkip_decrypt_errors",
		"rx_multicast_tkip_replay_errors", "rx_multicast_tkip_decrypt_errors",
		"rx_unicast_ccmp_replay_errors", "rx_unicast_ccmp_decrypt_errors",
		"rx_multicast_ccmp_replay_errors", "rx_multicast_ccmp_decrypt_errors",
		"phy_total_detect_count", "phy_ofdm_detect_count", "phy_11b_detect_count",
		"phy_plcp_error_count", "phy_total_receive_count", "phy_plcp_good_count",
	};
	struct centauri_wifi *wifi = seq->private;
	u8 payload[22] = {};
	unsigned int i;
	int ret;

	put_unaligned_le64(BIT_ULL(30), payload);
	payload[8] = 1;
	put_unaligned_le16(363, payload + 9);
	put_unaligned_le16(9, payload + 11);
	mutex_lock(&wifi->lock);
	wifi->rx_crypto_stats_valid = false;
	wifi->stats_reply_size = wifi->stats_tag = wifi->stats_length = 0;
	ret = wifi->configured ? command(wifi, 0xb0001, payload, sizeof(payload)) : -ENETDOWN;
	if (!ret) {
		usleep_range(1000, 2000);
		drain(wifi);
	}
	seq_printf(seq, "rc=%d valid=%d size=%u tag=%#x length=%u\n", ret,
		   wifi->rx_crypto_stats_valid, wifi->stats_reply_size, wifi->stats_tag, wifi->stats_length);
	if (!ret && wifi->rx_crypto_stats_valid)
		for (i = 0; i < ARRAY_SIZE(names); i++)
			seq_printf(seq, "%s=%u\n", names[i],
				   get_unaligned_le32(wifi->rx_crypto_stats + i * 4));
	mutex_unlock(&wifi->lock);
	return 0;
}

static int rx_crypto_stats_open(struct inode *inode, struct file *file)
{
	return single_open(file, rx_crypto_stats_show, inode->i_private);
}

static const struct file_operations rx_crypto_stats_ops = {
	.owner = THIS_MODULE,
	.open = rx_crypto_stats_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

static int dot11_mib_stats_show(struct seq_file *seq, void *unused)
{
	/* 802.11 MIB counters: statistic bit 25, scope 2/interface 0, TLV 0xf91b, 24 LE64s.
	 * Keep the host field names: tx_status_success is firmware accounting,
	 * not independent proof of over-the-air acknowledgement or AP delivery.
	 */
	static const char * const names[] = {
		"rx_msdu_pushed", "rx_msdu_pushed_multi", "rx_mpdu", "rx_mpdu_dup",
		"rx_ampdu_dup", "rx_mpdu_drop_error", "rx_msdu_drop_filter",
		"rx_holes_0", "rx_holes_1", "rx_holes_2", "rx_holes_3", "rx_holes_4",
		"rx_holes_5", "rx_holes_6", "rx_holes_7", "rx_holes_8",
		"tx_pulled", "tx_pulled_multi", "tx_pulled_core", "tx_status_success",
		"tx_status_abort_request", "tx_status_peer_gone",
		"tx_status_retries_exhausted", "tx_status_lifetime_expired",
	};
	struct centauri_wifi *wifi = seq->private;
	u8 payload[22] = {};
	unsigned int i;
	int ret;

	put_unaligned_le64(BIT_ULL(25), payload);
	payload[8] = 1;
	put_unaligned_le16(363, payload + 9);
	put_unaligned_le16(9, payload + 11);
	/* The scope byte follows the eight-byte interface value in TLV 363. */
	payload[21] = 2;
	mutex_lock(&wifi->lock);
	wifi->dot11_mib_stats_valid = false;
	wifi->stats_reply_size = wifi->stats_tag = wifi->stats_length = 0;
	ret = wifi->configured ? command(wifi, 0xb0001, payload, sizeof(payload)) : -ENETDOWN;
	if (!ret) {
		usleep_range(1000, 2000);
		drain(wifi);
	}
	seq_printf(seq, "rc=%d valid=%d size=%u tag=%#x length=%u\n", ret,
		   wifi->dot11_mib_stats_valid, wifi->stats_reply_size, wifi->stats_tag, wifi->stats_length);
	if (!ret && wifi->dot11_mib_stats_valid)
		for (i = 0; i < ARRAY_SIZE(names); i++)
			seq_printf(seq, "%s=%llu\n", names[i],
				   (unsigned long long)get_unaligned_le64(wifi->dot11_mib_stats + i * 8));
	mutex_unlock(&wifi->lock);
	return 0;
}

static int dot11_mib_stats_open(struct inode *inode, struct file *file)
{
	return single_open(file, dot11_mib_stats_show, inode->i_private);
}

static const struct file_operations dot11_mib_stats_ops = {
	.owner = THIS_MODULE,
	.open = dot11_mib_stats_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

static int tx_power_report_show(struct seq_file *seq, void *unused)
{
	/* Last TX stats: bit 29, scope 1/radio bitmap, TLV 0xf920 with 280
	 * bytes on this firmware. Newer firmware returns 320 bytes with a
	 * different record stride. The first five 28-byte records are read.
	 * These are rate/power observations, not proof of ACK or AP delivery.
	 */
	struct centauri_wifi *wifi = seq->private;
	u8 payload[22] = {};
	unsigned int radio, i;
	int ret;

	put_unaligned_le64(BIT_ULL(29), payload);
	payload[8] = 1;
	put_unaligned_le16(363, payload + 9);
	put_unaligned_le16(9, payload + 11);
	payload[21] = 1;
	mutex_lock(&wifi->lock);
	for (radio = 1; radio <= 2; radio++) {
		put_unaligned_le64(radio, payload + 13);
		wifi->tx_power_report_valid = false;
		wifi->stats_reply_size = wifi->stats_tag = wifi->stats_length = 0;
		ret = wifi->configured ? command(wifi, 0xb0001, payload, sizeof(payload)) : -ENETDOWN;
		if (!ret) {
			usleep_range(1000, 2000);
			drain(wifi);
		}
		seq_printf(seq, "radio_bitmap=%u rc=%d valid=%d size=%u tag=%#x length=%u\n",
			   radio, ret, wifi->tx_power_report_valid, wifi->stats_reply_size,
			   wifi->stats_tag, wifi->stats_length);
		if (ret || !wifi->tx_power_report_valid)
			continue;
		for (i = 0; i < 5; i++) {
			const u8 *record = wifi->tx_power_report + i * 28;

			/* Keep hardware enums and power bytes raw until interpreted
			 * against this exact firmware; omit unreviewed record fields.
			 */
			seq_printf(seq, "sample=%u power0_qdb_raw=%u power1_qdb_raw=%u power_qdb_raw=%u "
				   "hw_preamble=%u hw_rate=%u nss=%u hw_bw=%u hw_gi=%u\n",
				   i, record[14], record[15], record[16], record[17],
				   record[20], record[21] + 1, record[24], record[27]);
		}
	}
	mutex_unlock(&wifi->lock);
	return 0;
}

static int tx_power_report_open(struct inode *inode, struct file *file)
{
	return single_open(file, tx_power_report_show, inode->i_private);
}

static const struct file_operations tx_power_report_ops = {
	.owner = THIS_MODULE,
	.open = tx_power_report_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

static int data_stats_show(struct seq_file *m, void *unused)
{
	struct centauri_wifi *wifi = m->private;

	mutex_lock(&wifi->lock);
	seq_printf(m,
		   "rx received %u reposted %u bad %u\n"
		   "tx submitted %u completed %u in_flight %u failed %u bad %u coalesced %u\n"
		   "tx queued %u capped %u cap_bypassed %u\n",
		   wifi->data_rx_received, wifi->data_rx_reposted,
		   wifi->data_rx_bad, wifi->data_tx_submitted,
		   wifi->data_tx_completed,
		   wifi->data_tx_submitted - wifi->data_tx_completed,
		   wifi->data_tx_failed, wifi->data_tx_bad,
		   wifi->data_tx_coalesced, skb_queue_len(&wifi->data_tx_queue),
		   wifi->data_tx_capped, wifi->data_tx_cap_bypassed);
	mutex_unlock(&wifi->lock);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(data_stats);

static void wifi_hint_country(struct centauri_wifi *wifi)
{
	char alpha2[3] = {};
	bool have_country = false;

	mutex_lock(&wifi->lock);
	wifi->country_valid = false;
	if (wifi->configured && !command(wifi, 10, NULL, 0) &&
	    wifi->country_valid &&
	    country_code_valid(wifi->country, sizeof(wifi->country))) {
		alpha2[0] = wifi->country[0];
		alpha2[1] = wifi->country[1];
		have_country = true;
	}
	mutex_unlock(&wifi->lock);
	if (have_country)
		regulatory_hint(wifi->wiphy, alpha2);
}

int centauri_wifi_register(struct centauri *c)
{
	struct centauri_wifi *wifi;
	int ret;

	wifi = kzalloc(sizeof(*wifi), GFP_KERNEL);
	if (!wifi)
		return -ENOMEM;
	c->wifi = wifi;
	wifi->control = c;
	mutex_init(&wifi->lock);
	mutex_init(&wifi->fwlog_lock);
	skb_queue_head_init(&wifi->data_tx_queue);
	skb_queue_head_init(&wifi->rx_gro_queue);
	INIT_WORK(&wifi->scan_work, scan_worker);
	INIT_WORK(&wifi->connect_work, connect_worker);
	INIT_DELAYED_WORK(&wifi->link_work, link_worker);
	ret = centauri_alpha_resources(c, &wifi->a);
	if (ret)
		goto failed;
	wifi->wiphy = wiphy_new(&wifi_ops, sizeof(struct centauri_wifi *));
	wifi->netdev = alloc_etherdev(sizeof(struct centauri_wifi *));
	if (!wifi->wiphy || !wifi->netdev) {
		ret = -ENOMEM;
		goto failed;
	}
	*(struct centauri_wifi **)wiphy_priv(wifi->wiphy) = wifi;
	*(struct centauri_wifi **)netdev_priv(wifi->netdev) = wifi;
	netif_napi_add(wifi->netdev, &wifi->rx_napi, rx_poll);
	set_wiphy_dev(wifi->wiphy, &c->pdev->dev);
	wifi->wiphy->interface_modes = BIT(NL80211_IFTYPE_STATION);
	wifi->wiphy->max_scan_ssids = CEN_SCAN_SSIDS_MAX;
	wifi->wiphy->max_scan_ie_len = 512;
	wifi->wiphy->flags |= WIPHY_FLAG_SPLIT_SCAN_6GHZ;
	wifi->wiphy->support_mbssid = true;
	wifi->wiphy->signal_type = CFG80211_SIGNAL_TYPE_MBM;
	wifi->wiphy->reg_notifier = wifi_regulatory;
	wifi->wiphy->cipher_suites = cipher_suites;
	wifi->wiphy->n_cipher_suites = ARRAY_SIZE(cipher_suites);
	wifi->wiphy->akm_suites = akm_suites;
	wifi->wiphy->n_akm_suites = ARRAY_SIZE(akm_suites);
	wifi->wiphy->max_num_akm_suites = CFG80211_MAX_NUM_AKM_SUITES;
	wiphy_ext_feature_set(wifi->wiphy, NL80211_EXT_FEATURE_4WAY_HANDSHAKE_STA_PSK);
	wiphy_ext_feature_set(wifi->wiphy, NL80211_EXT_FEATURE_SAE_OFFLOAD);
	centauri_wifi_set_address(wifi);
	ret = radio_start(wifi);
	if (ret) {
		dev_err(&c->pdev->dev, "Wi-Fi ring/interface setup failed: %d\n", ret);
		goto failed;
	}
	bands_init(wifi);
	ret = wiphy_register(wifi->wiphy);
	if (ret)
		goto failed;
	wifi->registered = true;
	debugfs_create_blob("hw_channels", 0400, wifi->wiphy->debugfsdir,
			    &wifi->hw_channels_blob);
	debugfs_create_blob("system_caps", 0400, wifi->wiphy->debugfsdir,
			    &wifi->system_caps_blob);
	debugfs_create_file("data_stats", 0400, wifi->wiphy->debugfsdir, wifi,
			    &data_stats_fops);
	rtnl_lock();
	channels_apply(wifi);
	rtnl_unlock();
	wifi_hint_country(wifi);
	wifi->wdev.wiphy = wifi->wiphy;
	wifi->wdev.iftype = NL80211_IFTYPE_STATION;
	wifi->wdev.netdev = wifi->netdev;
	wifi->netdev->ieee80211_ptr = &wifi->wdev;
	wifi->netdev->netdev_ops = &net_ops;
	wifi->netdev->ethtool_ops = &centauri_ethtool_ops;
	SET_NETDEV_DEV(wifi->netdev, &c->pdev->dev);
	strscpy(wifi->netdev->name, "wlan%d", IFNAMSIZ);
	wifi->netdev->max_mtu = ETH_DATA_LEN;
	ret = register_netdev(wifi->netdev);
	if (ret)
		goto failed;
	dev_info(&c->pdev->dev, "cfg80211 interface %s registered\n",
		 wifi->netdev->name);
	wifi->data_probe_file = debugfs_create_file("data_probe", 0200, c->debug, wifi, &data_probe_ops);
	wifi->data_status_file = debugfs_create_file("data_status", 0400, c->debug, wifi, &data_status_ops);
	wifi->data_rx_snapshot_file = debugfs_create_file("data_rx_snapshot", 0400,
		c->debug, wifi, &data_rx_snapshot_ops);
	wifi->station_stats_file = debugfs_create_file("station_stats", 0400, c->debug, wifi, &station_stats_ops);
	wifi->rx_crypto_stats_file = debugfs_create_file("rx_crypto_stats", 0400,
		c->debug, wifi, &rx_crypto_stats_ops);
	wifi->dot11_mib_stats_file = debugfs_create_file("dot11_mib_stats", 0400,
		c->debug, wifi, &dot11_mib_stats_ops);
	wifi->tx_power_report_file = debugfs_create_file("tx_power_report", 0400,
		c->debug, wifi, &tx_power_report_ops);
	wifi->eapol_start_file = debugfs_create_file("eapol_start", 0200, c->debug, wifi, &eapol_start_ops);
	wifi->country_file = debugfs_create_file("country", 0400, c->debug, wifi, &country_ops);
	wifi->link_status_file = debugfs_create_file("link_status", 0400, c->debug, wifi, &link_status_ops);
	wifi->tx_config_file = debugfs_create_file("tx_config", 0600, c->debug, wifi, &tx_config_ops);
	wifi->host_wakeup_file = debugfs_create_file("host_wakeup", 0200, c->debug, wifi, &host_wakeup_ops);
	wifi->fwlog_start_file = debugfs_create_file("fwlog_start", 0200, c->debug, wifi, &fwlog_start_ops);
	wifi->fwlog_status_file = debugfs_create_file("fwlog_status", 0400, c->debug, wifi, &fwlog_status_ops);
	wifi->fwlog_buffer_file = debugfs_create_file("fwlog_buffer", 0400, c->debug, wifi, &fwlog_buffer_ops);
	wifi->fwlog_ack_file = debugfs_create_file("fwlog_ack", 0200, c->debug, wifi, &fwlog_ack_ops);
	return 0;
failed:
	centauri_wifi_unregister(c);
	return ret;
}

/*
 * Alpha ACIPC host-sleep handshake uses BAR0 + 0x1024 for sleep_control
 * (3 = sleep, 0 = wake) and polls peripheral_info.sleep_status at context + 0x70
 * (3 = asleep, <= 1 = awake). Keep ring producers paused across the transition.
 */
static u32 alpha_sleep_status(struct centauri_wifi *wifi)
{
	return le32_to_cpu(READ_ONCE(*(__le32 *)(wifi->a.context + 0x70)));
}

static int alpha_transport_state(struct centauri_wifi *wifi, bool asleep)
{
	u32 status;
	int ret;

	lockdep_assert_held(&wifi->lock);
	if (readl(wifi->a.bar + 8) != 2 || readl(wifi->a.bar + 0xc) != 2)
		return -EIO;
	dma_wmb();
	writel(asleep ? 3 : 0, wifi->a.bar + 0x1024);
	ret = read_poll_timeout(alpha_sleep_status, status,
				(asleep ? status == 3 : status <= 1) ||
				readl(wifi->a.bar + 8) != 2,
				100, 2000000, false, wifi);
	dma_rmb();
	if (readl(wifi->a.bar + 8) != 2)
		ret = -EIO;
	dev_dbg(&wifi->control->pdev->dev,
		"Alpha ACIPC host %s: ret=%d status=%u exec=%u ipc=%u\n",
		asleep ? "sleep" : "wake", ret, status,
		readl(wifi->a.bar + 8), readl(wifi->a.bar + 0xc));
	return ret;
}

#define CEN_HOST_EVENTS_LEGACY	85
#define CEN_HOST_EVENTS_FULL	91

static int alpha_host_power_send(struct centauri_wifi *wifi, bool asleep,
				 u8 event_count)
{
	static const u8 sleep_events[] = { 24, 26, 27, 15, 51, 8, 9, 10, 84 };
	u8 payload[3 + 5 + 4 + 1 + CEN_HOST_EVENTS_FULL] = {};
	u8 events[1 + CEN_HOST_EVENTS_FULL] = {};
	u8 sleep_option = 1;
	size_t size = 3;
	unsigned int i;
	int ret;

	lockdep_assert_held(&wifi->lock);
	if (readl(wifi->a.bar + 8) != 2)
		return -EIO;
	if (asleep) {
		put_unaligned_le16(3, payload);
		payload[2] = 3;
		append_tlv(payload, &size, 303, &sleep_option, sizeof(sleep_option));
		events[0] = ARRAY_SIZE(sleep_events);
		memcpy(events + 1, sleep_events, sizeof(sleep_events));
	} else {
		events[0] = event_count;
		for (i = 0; i < event_count; i++)
			events[i + 1] = i;
	}
	append_tlv(payload, &size, 280, events, 1 + event_count);
	ret = command_control(wifi, 6, payload, size, true);
	dev_dbg(&wifi->control->pdev->dev,
		"Alpha firmware host %s (events=%u tlv280=%u): ret=%d status=%u exec=%u\n",
		asleep ? "sleep" : "awake", events[0], 1 + event_count, ret,
		wifi->reply_status, readl(wifi->a.bar + 8));
	return ret;
}

static int alpha_host_power(struct centauri_wifi *wifi, bool asleep)
{
	u16 full_status;
	int ret;

	if (asleep)
		return alpha_host_power_send(wifi, true, CEN_HOST_EVENTS_LEGACY);

	ret = alpha_host_power_send(wifi, false, CEN_HOST_EVENTS_FULL);
	if (ret == -EREMOTEIO && readl(wifi->a.bar + 8) == 2) {
		full_status = wifi->reply_status;
		ret = alpha_host_power_send(wifi, false, CEN_HOST_EVENTS_LEGACY);
		if (!ret && !wifi->host_events_fallback_logged) {
			wifi->host_events_fallback_logged = true;
			dev_info(&wifi->control->pdev->dev,
				 "Alpha host wake fell back from %u to %u events (status=%u)\n",
				 CEN_HOST_EVENTS_FULL, CEN_HOST_EVENTS_LEGACY,
				 full_status);
		}
	}
	return ret;
}

static int alpha_resume_locked(struct centauri_wifi *wifi)
{
	int ret;

	lockdep_assert_held(&wifi->lock);
	if (!wifi->transport_sleep_sent && !wifi->host_sleep_sent)
		return 0;
	if (wifi->transport_sleep_sent) {
		ret = alpha_transport_state(wifi, false);
		if (ret)
			return ret;
		wifi->transport_sleep_sent = false;
	}
	if (wifi->host_sleep_sent) {
		ret = alpha_host_power(wifi, false);
		if (ret)
			return ret;
		wifi->host_sleep_sent = false;
	}
	mutex_lock(&wifi->fwlog_lock);
	WRITE_ONCE(wifi->transport_paused, false);
	mutex_unlock(&wifi->fwlog_lock);
	wifi->station_signal_valid = false;
	wifi->station_noise_valid = false;
	wifi->station_tx_retrans_valid = false;
	wifi->station_tx_fail_valid = false;
	wifi->station_beacon_valid = false;
	wifi->station_cca_valid = false;
	wifi->station_tx_rate_valid = false;
	wifi->station_tx_power_valid = false;
	wifi->station_rx_rate_valid = false;
	wifi->station_chip_tx_rate_valid = false;
	wifi->station_chains = 0;
	wifi->station_phy_noise_valid = false;
	wifi->station_chandef_valid = false;
	wifi->station_signal_at = jiffies - HZ;
	wifi->station_noise_at = jiffies - HZ;
	wifi->station_counters_at = jiffies - HZ;
	wifi->station_cca_at = jiffies - HZ;
	wifi->station_tx_info_at = jiffies - HZ;
	wifi->station_beacon_at = jiffies - HZ;
	wifi->station_phy_dump_at = jiffies - HZ;
	if (wifi->connected && !READ_ONCE(wifi->stopping)) {
		if (netif_running(wifi->netdev))
			netif_wake_queue(wifi->netdev);
		mod_delayed_work(system_percpu_wq, &wifi->link_work, 0);
	}
	return 0;
}

int centauri_wifi_resume(struct centauri *c)
{
	struct centauri_wifi *wifi = c->wifi;
	int ret;

	if (!wifi || !of_machine_is_compatible("apple,j813"))
		return 0;
	mutex_lock(&wifi->lock);
	ret = alpha_resume_locked(wifi);
	mutex_unlock(&wifi->lock);
	return ret;
}

int centauri_wifi_suspend(struct centauri *c)
{
	struct centauri_wifi *wifi = c->wifi;
	int ret, restore;

	if (!wifi || !of_machine_is_compatible("apple,j813") ||
	    !READ_ONCE(alpha_sleep_untested))
		return 0;
	mutex_lock(&wifi->lock);
	if (wifi->transport_paused || wifi->connected || wifi->connecting || wifi->scan_request ||
	    READ_ONCE(wifi->stopping)) {
		mutex_unlock(&wifi->lock);
		return -EBUSY;
	}
	mutex_lock(&wifi->fwlog_lock);
	WRITE_ONCE(wifi->transport_paused, true);
	mutex_unlock(&wifi->fwlog_lock);
	mutex_unlock(&wifi->lock);
	netif_tx_disable(wifi->netdev);
	cancel_work_sync(&wifi->scan_work);
	cancel_work_sync(&wifi->connect_work);
	cancel_delayed_work_sync(&wifi->link_work);

	mutex_lock(&wifi->lock);
	wifi->host_sleep_sent = true;
	ret = alpha_host_power(wifi, true);
	if (!ret) {
		wifi->transport_sleep_sent = true;
		ret = alpha_transport_state(wifi, true);
	}
	if (ret) {
		restore = alpha_resume_locked(wifi);
		if (restore)
			dev_err(&c->pdev->dev,
				"Alpha transport remains paused after refused sleep: %d\n",
				restore);
	}
	mutex_unlock(&wifi->lock);
	return ret;
}

void centauri_wifi_unregister(struct centauri *c)
{
	struct centauri_wifi *wifi = c->wifi;

	if (!wifi)
		return;
	WRITE_ONCE(wifi->stopping, true);
	WRITE_ONCE(wifi->connect_abort, true);
	WRITE_ONCE(wifi->scan_abort, true);
	if (wifi->netdev && wifi->netdev->reg_state == NETREG_REGISTERED)
		netif_tx_disable(wifi->netdev);
	debugfs_remove(wifi->data_probe_file);
	debugfs_remove(wifi->data_status_file);
	debugfs_remove(wifi->data_rx_snapshot_file);
	debugfs_remove(wifi->station_stats_file);
	debugfs_remove(wifi->rx_crypto_stats_file);
	debugfs_remove(wifi->dot11_mib_stats_file);
	debugfs_remove(wifi->tx_power_report_file);
	debugfs_remove(wifi->eapol_start_file);
	debugfs_remove(wifi->country_file);
	debugfs_remove(wifi->link_status_file);
	debugfs_remove(wifi->tx_config_file);
	debugfs_remove(wifi->host_wakeup_file);
	debugfs_remove(wifi->fwlog_start_file);
	debugfs_remove(wifi->fwlog_status_file);
	debugfs_remove(wifi->fwlog_buffer_file);
	debugfs_remove(wifi->fwlog_ack_file);
	cancel_scan(wifi);
	cancel_work_sync(&wifi->connect_work);
	cancel_delayed_work_sync(&wifi->link_work);
	mutex_lock(&wifi->lock);
	cancel_connect_locked(wifi);
	mutex_unlock(&wifi->lock);
	skb_queue_purge(&wifi->data_tx_queue);
	if (wifi->netdev && wifi->netdev->reg_state == NETREG_REGISTERED)
		unregister_netdev(wifi->netdev);
	skb_queue_purge(&wifi->rx_gro_queue);
	if (wifi->registered)
		wiphy_unregister(wifi->wiphy);
	if (wifi->wiphy)
		wiphy_free(wifi->wiphy);
	if (wifi->netdev)
		free_netdev(wifi->netdev);
	memzero_explicit(wifi, sizeof(*wifi));
	kfree(wifi);
	c->wifi = NULL;
}

#if IS_ENABLED(CONFIG_APPLE_CENTAURI_KUNIT_TEST)
#include "centauri_wifi_test.c"
#endif
