// SPDX-License-Identifier: GPL-2.0-only
/* Native peer-to-peer AWDL virtual interface for the Apple N1 Centauri Alpha
 * Wi-Fi firmware.
 *
 * The firmware implements synchronization, master election, periodic/master
 * indication frames, channel-sequence hopping, peer cache tracking, and
 * optional LLC/SNAP encapsulation on hardware interface ID 1. This file binds
 * an awdl0 Ethernet netdev onto those control commands and ACIPC data rings.
 */

#include <linux/ctype.h>
#include <linux/debugfs.h>
#include <linux/etherdevice.h>
#include <linux/ethtool.h>
#include <linux/hex.h>
#include <linux/ieee80211.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/seq_file.h>
#include <linux/skbuff.h>
#include <linux/spinlock.h>
#include <linux/unaligned.h>
#include <linux/workqueue.h>
#include <net/ieee80211_radiotap.h>
#include "centauri.h"
#include "centauri_awdl.h"

static bool awdl_enable = true;
module_param(awdl_enable, bool, 0444);
MODULE_PARM_DESC(awdl_enable,
		 "Register the awdl0 peer-to-peer interface on Alpha (default: 1)");

/* Fall back to our own synchronization after this much parent silence. */
#define CEN_AWDL_PARENT_TIMEOUT		(5 * HZ)
/* Management frames carry this much radiotap when the header is unusable. */
#define CEN_AWDL_RX_HDR_LEN		0x60U
#define CEN_AWDL_RTAP_MAX		0x200U

static const u8 cen_awdl_default_bssid[ETH_ALEN] = {
	0x00, 0x25, 0x00, 0xff, 0x94, 0x73,
};

static const u8 cen_awdl_llc_snap[8] = {
	0xaa, 0xaa, 0x03, 0x00, 0x17, 0xf2, 0x08, 0x00,
};

static const u8 cen_awdl_rfc1042_llc[6] = {
	0xaa, 0xaa, 0x03, 0x00, 0x00, 0x00,
};

struct cen_awdl_tx_ring {
	u16 tr_id;
	u32 ring_offset;
	u32 buf_offset;
	bool opened;
	bool in_use;
	u8 peer_mac[ETH_ALEN];
	u16 tx_length[CEN_AWDL_TX_SLOTS];
	bool tx_busy[CEN_AWDL_TX_SLOTS];
	u32 submitted;
	u32 completed;
	u32 failed;
	u32 bad;
	unsigned long last_used;
};

struct cen_awdl_pending_peer {
	u8 mac[ETH_ALEN];
	bool want_ring;		/* unicast TX waits for it */
};

struct centauri_awdl {
	struct centauri_wifi *wifi;
	struct net_device *netdev;
	struct sk_buff_head tx_queue;
	struct cen_awdl_tx_ring rings[CEN_AWDL_TOTAL_TX_RINGS];
	struct work_struct peer_work;
	/* protects pending_peers, pending_election, active_ring_macs */
	spinlock_t peer_lock;
	struct cen_awdl_pending_peer pending_peers[CEN_AWDL_PENDING_PEERS_MAX];
	u8 pending_peer_count;
	bool pending_election;
	struct cen_awdl_parent pending_parent;
	struct delayed_work follow_work;
	struct delayed_work refresh_work;
	u32 refreshes;
	int refresh_rc;
	bool following;
	unsigned long parent_seen;
	unsigned long follow_retry_at;
	u8 active_ring_macs[CEN_AWDL_MAX_PEER_RINGS][ETH_ALEN];
	bool active_ring_valid[CEN_AWDL_MAX_PEER_RINGS];
	struct cen_awdl_recent_peer recent_peers[CEN_AWDL_RECENT_PEERS];
	struct dentry *status_file;
	struct dentry *peers_file;
	struct dentry *chanseq_file;
	struct dentry *arpa_file;
	struct dentry *svc_file;

	bool active;
	bool defaults_loaded;
	u8 max_peers;
	u8 master_channel;
	u8 sync_state;
	u8 country[3];
	u16 tx_seq;
	u16 extra_tlvs_len;
	u16 mif_tlvs_len;
	u8 vht_ie[CEN_AWDL_VHT_IE_MAX];
	u8 vht_ie_len;
	u8 svc_tlvs[CEN_AWDL_SVC_TLVS_MAX];
	u16 svc_tlvs_len;
	u16 svc_sui;
	unsigned long tlvs_pushed_at;
	u32 tlvs_pushes;
	u8 last_peer_mac[ETH_ALEN];
	u8 last_peer_state;
	u8 last_rx_peer_mac[ETH_ALEN];
	u32 last_rx_peer_metric;
	u8 last_rx_peer_chan;
	char arpa[CEN_AWDL_ARPA_MAX + 1];

	/* Cached firmware defaults retrieved via command 8 on interface 1. */
	struct cen_awdl_general_cfg general_cfg;
	struct cen_awdl_election_params election;
	u8 extra_election[CEN_AWDL_EXTRA_ELECTION_LEN];
	bool extra_election_valid;
	u8 sync_addr[ETH_ALEN];
	__le32 default_self_metric;
	struct cen_awdl_mif_sync_params default_mif_sync;
	u8 default_xel_head[4];	/* extra_election bytes 0-3 as the firmware gave them */
	struct cen_awdl_mif_sync_params mif_sync;
	u8 sync_params[CEN_AWDL_SYNC_PARAMS_MAX];
	u8 sync_out[CEN_AWDL_SYNC_PARAMS_MAX];	/* 0xff04 as sent */
	u16 sync_params_len;
	struct cen_awdl_chanseq chanseq;
	bool chanseq_written;	/* set through debugfs: kept over the station's */

	/* Telemetry & event counters. */
	u32 rx_frames;
	u32 rx_decap;
	u32 rx_dropped;
	u32 rx_action_frames;
	u32 rx_psf;
	u32 rx_mif;
	u32 tx_mcbc;
	u32 tx_unicast;
	u32 tx_encap;
	u32 auto_peers;
	u32 peer_ring_reuses;
	u32 peer_holds;
	u32 events_seen;
	u32 sync_events;
	u32 election_events;
	u32 peer_events;
	u32 last_event_id;
	u16 last_event_status;
	u32 adoptions;
	u32 reverts;
	u32 follow_fails;
	int follow_rc;
	u32 adopt_bad_sync;
	u32 adopt_no_time;
	u32 rx_root_us;
	u32 rx_timed;
	u16 rtap_len;
	u32 rtap_present;
	u32 rtap_fallbacks;

	/* Scratch buffers serialized by wifi->lock (kept off kernel stack). */
	u8 reply_buf[1536];
	u8 start_buf[1024];
	u8 extra_tlv_buf[CEN_AWDL_EXTRA_TLVS_MAX];
	u8 mif_tlv_buf[CEN_AWDL_EXTRA_TLVS_MAX];
	u8 tx_stage[CEN_AWDL_TX_BUF_SIZE];
};

static u16 cen_awdl_encode_channel(u8 channel)
{
	u8 opclass;

	if (!channel)
		return 0;
	if (channel >= 36 && channel <= 161)	/* 80 MHz class, as traced */
		return (128U << 8) | channel;
	if (channel <= 14)
		opclass = 81;
	else if (channel <= 48)
		opclass = 115;
	else if (channel <= 64)
		opclass = 118;
	else if (channel <= 144)
		opclass = 121;
	else
		opclass = 125;
	return ((u16)opclass << 8) | channel;
}

/* The election, 0xfe00 and channel sequence TLVs go out at the traced host's lengths. */
#define CEN_AWDL_EL_TX_LEN		19U
#define CEN_AWDL_XEL_TX_LEN		(CEN_AWDL_EXTRA_ELECTION_LEN - 1)
#define CEN_AWDL_CS_TX_LEN		(sizeof(struct cen_awdl_chanseq) - 2)

/*
 * Build a 16-slot channel sequence whose 4-slot dwell blocks match step_count=3
 * (slots 0..3, 4..7, 8..11, 12..15). A channel value of 0 leaves that 4-slot
 * window unscheduled so the station link retains airtime.
 */
static void cen_awdl_build_default_chanseq(struct cen_awdl_chanseq *seq,
					   u8 ch_2g, u8 ch_5g_lo, u8 ch_5g_hi)
{
	__le16 s2 = cpu_to_le16(cen_awdl_encode_channel(ch_2g));
	__le16 s5a = cpu_to_le16(cen_awdl_encode_channel(ch_5g_lo));
	__le16 s5b = cpu_to_le16(cen_awdl_encode_channel(ch_5g_hi));
	unsigned int i;

	memset(seq, 0, sizeof(*seq));
	seq->count_minus_1 = CEN_AWDL_CHANSEQ_SLOTS - 1;
	seq->encoding = 3;
	seq->dup_count = 0;
	seq->step_count = 3;
	seq->fill_channel = cpu_to_le16(0xffff);

	for (i = 0; i < CEN_AWDL_CHANSEQ_SLOTS; i++) {
		switch (i >> 2) {
		case 0:
		case 2:
			seq->channels[i] = s2;
			break;
		case 1:
			seq->channels[i] = s5a;
			break;
		default:
			seq->channels[i] = s5b;
			break;
		}
	}
}

/*
 * 0xff04 as the traced host sends it: the firmware's 33-byte header with the
 * extension counts at 3 and the master to synchronize to (none until a parent
 * is followed), then the channel sequence in encoding 1, a (flags, channel)
 * pair per slot: 0x2b for a 2.4 GHz channel, 0x1d with the 40 MHz centre for
 * the lower channel of a 5 GHz pair; other slots are left off. A firmware
 * default shorter than the header is sent as reported. Built into @o, returns
 * the length.
 */
#define CEN_AWDL_SYNC_HDR_LEN		33U
#define CEN_AWDL_SYNC_REF_LEN		(CEN_AWDL_SYNC_HDR_LEN + 6U + 2U * CEN_AWDL_CHANSEQ_SLOTS)

static size_t cen_awdl_sync_build(const struct centauri_awdl *awdl, u8 *o)
{
	unsigned int i;

	static_assert(CEN_AWDL_SYNC_REF_LEN <= CEN_AWDL_SYNC_PARAMS_MAX);
	if (awdl->sync_params_len < CEN_AWDL_SYNC_HDR_LEN + 6) {
		memcpy(o, awdl->sync_params, awdl->sync_params_len);
		return awdl->sync_params_len;
	}
	memset(o, 0, CEN_AWDL_SYNC_REF_LEN);
	memcpy(o, awdl->sync_params, CEN_AWDL_SYNC_HDR_LEN + 6);
	memset(o + 17, 3, 4);
	if (READ_ONCE(awdl->following)) {
		if (!is_zero_ether_addr(awdl->sync_addr))
			ether_addr_copy(o + 21, awdl->sync_addr);
		else if (awdl->netdev)
			ether_addr_copy(o + 21, awdl->netdev->dev_addr);
	}
	o[CEN_AWDL_SYNC_HDR_LEN + 1] = 1;
	for (i = 0; i < CEN_AWDL_CHANSEQ_SLOTS; i++) {
		u8 ch = le16_to_cpu(awdl->chanseq.channels[i]) & 0xff;
		u8 *e = o + CEN_AWDL_SYNC_HDR_LEN + 6 + 2 * i;

		if (ch && ch <= 14) {
			e[0] = 0x2b;
			e[1] = ch;
		} else if ((ch >= 36 && ch <= 140 && !((ch - 36) % 8)) ||
			   ch == 149 || ch == 157) {
			e[0] = 0x1d;
			e[1] = ch + 2;
		}
	}
	return CEN_AWDL_SYNC_REF_LEN;
}

static int cen_awdl_cmd(struct centauri_awdl *awdl, u32 id, const void *payload,
			size_t size, void *reply, size_t *reply_size)
{
	return centauri_wifi_cmd_iface(awdl->wifi, CEN_IF_AWDL, id, payload,
				       size, reply, reply_size);
}

/* Encapsulate an Ethernet II frame into 802.3 + LLC/SNAP + AWDL data header. */
static size_t cen_awdl_encap_frame(u8 *dst, size_t capacity,
				   const u8 *eth, size_t eth_len, u16 seq)
{
	size_t out_len;

	if (eth_len < ETH_HLEN || eth_len > capacity ||
	    capacity - eth_len < CEN_AWDL_DATA_HDR_LEN)
		return 0;
	out_len = eth_len + CEN_AWDL_DATA_HDR_LEN;
	memcpy(dst, eth, ETH_ALEN * 2);
	put_unaligned_be16(eth_len + 2, dst + 12);
	memcpy(dst + 14, cen_awdl_llc_snap, sizeof(cen_awdl_llc_snap));
	put_unaligned_le16(CEN_AWDL_DATA_HDR_MAGIC, dst + 22);
	put_unaligned_le16(seq & 0x0fffU, dst + 24);
	put_unaligned_le16(0, dst + 26);
	memcpy(dst + 28, eth + 12, eth_len - 12);
	return out_len;
}

/* Decapsulate 802.3 + LLC/SNAP (+ AWDL data) frames back into Ethernet II. */
static size_t cen_awdl_decap_frame(u8 *dst, size_t capacity,
				   const u8 *frame, size_t frame_len,
				   bool *was_encap)
{
	size_t out_len;

	if (was_encap)
		*was_encap = false;
	if (frame_len >= ETH_HLEN + CEN_AWDL_DATA_HDR_LEN &&
	    !memcmp(frame + 14, cen_awdl_llc_snap, sizeof(cen_awdl_llc_snap))) {
		out_len = frame_len - CEN_AWDL_DATA_HDR_LEN;
		if (out_len > capacity)
			return 0;
		memcpy(dst, frame, ETH_ALEN * 2);
		memcpy(dst + 12, frame + 28, out_len - 12);
		if (was_encap)
			*was_encap = true;
		return out_len;
	}
	if (frame_len >= ETH_HLEN + CEN_AWDL_RFC1042_HDR_LEN &&
	    !memcmp(frame + 14, cen_awdl_rfc1042_llc,
		    sizeof(cen_awdl_rfc1042_llc))) {
		out_len = frame_len - CEN_AWDL_RFC1042_HDR_LEN;
		if (out_len > capacity)
			return 0;
		memcpy(dst, frame, ETH_ALEN * 2);
		memcpy(dst + 12, frame + 20, out_len - 12);
		if (was_encap)
			*was_encap = true;
		return out_len;
	}
	if (frame_len < ETH_HLEN || frame_len > capacity)
		return 0;
	memcpy(dst, frame, frame_len);
	return frame_len;
}

static u16 cen_awdl_social_channel_map(const struct cen_awdl_chanseq *seq)
{
	unsigned int i;
	u16 map = 0;

	for (i = 0; i < CEN_AWDL_CHANSEQ_SLOTS; i++) {
		u8 ch = le16_to_cpu(seq->channels[i]) & 0xff;

		if (ch == 6)
			map |= 0x0001U;
		else if (ch == 44)
			map |= 0x0002U;
		else if (ch == 149)
			map |= 0x0004U;
	}
	return map ?: 0x0001U;
}

static size_t cen_awdl_append_ota_tlv(u8 *buf, size_t capacity, size_t offset,
				      u8 type, const void *val, u16 length)
{
	if (offset + 3 + length > capacity)
		return offset;
	buf[offset] = type;
	put_unaligned_le16(length, buf + offset + 1);
	if (length)
		memcpy(buf + offset + 3, val, length);
	return offset + 3 + length;
}

/*
 * 47-byte Data Path State payload (50 bytes including 3-byte TLV header):
 *   +0..1  le16 flags
 *   +2..4  u8   country[3]
 *   +5..6  le16 social_channel_map (bit0=6, bit1=44, bit2=149)
 *   +7..12 u8   infra_bssid[6]
 *   +13..14 le16 infra_channel
 *   +15..20 u8  awdl_mac[6]
 *   +21..22 le16 log_trigger_id (4)
 *   +27..28 le16 ext_flags
 */
static size_t cen_awdl_append_dp_state(const struct centauri_awdl *awdl,
				       u8 *buf, size_t capacity, size_t off)
{
	u8 dp_state[CEN_AWDL_OTA_DP_STATE_LEN];

	memset(dp_state, 0, sizeof(dp_state));
	put_unaligned_le16(0xab23U, dp_state);
	if (awdl->country[0] && awdl->country[1]) {
		dp_state[2] = awdl->country[0];
		dp_state[3] = awdl->country[1];
		dp_state[4] = awdl->country[2];
	} else {
		dp_state[2] = 'U';
		dp_state[3] = 'S';
	}
	put_unaligned_le16(cen_awdl_social_channel_map(&awdl->chanseq),
			   dp_state + 5);
	if (awdl->netdev)
		ether_addr_copy(dp_state + 15, awdl->netdev->dev_addr);
	put_unaligned_le16(0x0004U, dp_state + 21);
	put_unaligned_le16(0x0021U, dp_state + 27);
	return cen_awdl_append_ota_tlv(buf, capacity, off,
				       CEN_AWDL_OTA_TLV_DATA_PATH_STATE,
				       dp_state, sizeof(dp_state));
}

/*
 * ARPA hostname: flags 0x03, label length, label, then, as peers send it, a
 * compression pointer to ".local".
 */
static size_t cen_awdl_append_arpa(const struct centauri_awdl *awdl,
				   u8 *buf, size_t capacity, size_t off)
{
	u8 arpa_val[CEN_AWDL_ARPA_MAX + 4];
	size_t label_len = strnlen(awdl->arpa, CEN_AWDL_ARPA_MAX);
	size_t len;

	if (!label_len)
		return off;
	arpa_val[0] = 0x03;
	arpa_val[1] = (u8)label_len;
	memcpy(arpa_val + 2, awdl->arpa, label_len);
	len = label_len + 2;
	arpa_val[len++] = 0xc0;
	arpa_val[len++] = 0x0c;
	return cen_awdl_append_ota_tlv(buf, capacity, off,
				       CEN_AWDL_OTA_TLV_ARPA, arpa_val, len);
}

/*
 * HT capabilities as two-stream peers send them: reserved le16, HT
 * capability info 0x886f, A-MPDU parameters 0x1b, the 16-byte supported
 * MCS set (MCS 0-15, highest rate 150, TX set defined).
 */
static const u8 cen_awdl_ht_caps[20] = {
	0x00, 0x00, 0x6f, 0x88, 0x1b, 0xff, 0xff, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x96,
	0x00, 0x01, 0x00, 0x00,
};

/*
 * Election tree: master, sender, le32 master counter, distance, master
 * metric, self metric, two reserved words and the sender's counter.
 */
static size_t cen_awdl_append_election_tree(const struct centauri_awdl *awdl,
					    u8 *buf, size_t capacity,
					    size_t off)
{
	const u8 *self = awdl->netdev ? awdl->netdev->dev_addr : NULL;
	u8 tree[40];

	memset(tree, 0, sizeof(tree));
	if (!is_zero_ether_addr(awdl->election.top_master))
		ether_addr_copy(tree, awdl->election.top_master);
	else if (self)
		ether_addr_copy(tree, self);
	/* The second address is the parent in the tree; self as its root. */
	if (READ_ONCE(awdl->following) && !is_zero_ether_addr(awdl->sync_addr))
		ether_addr_copy(tree + 6, awdl->sync_addr);
	else if (self)
		ether_addr_copy(tree + 6, self);
	put_unaligned_le32(awdl->election.distance_from_top, tree + 16);
	put_unaligned_le32(le32_to_cpu(awdl->election.top_master_metric),
			   tree + 20);
	put_unaligned_le32(le32_to_cpu(awdl->election.self_metric), tree + 24);
	return cen_awdl_append_ota_tlv(buf, capacity, off,
				       CEN_AWDL_OTA_TLV_ELECTION_V2,
				       tree, sizeof(tree));
}

/*
 * The frame templates: the TLVs nearby peers append after the sync,
 * channel-sequence and election TLVs the firmware writes itself, in the
 * order they send them (as captured). Sync frames carry service parameters,
 * election tree, data-path state, HT and VHT capabilities and version;
 * master-indication frames carry election tree, data-path state, HT and VHT
 * capabilities, hostname, version, service parameters and service responses.
 */
static size_t cen_awdl_build_template(const struct centauri_awdl *awdl,
				      u8 *buf, size_t capacity, bool mif)
{
	const u8 version[2] = { CEN_AWDL_OTA_VERSION_BYTE, 0x01 };
	u8 svc_params[9] = { 0 };
	size_t off = 0;

	put_unaligned_le16(awdl->svc_sui, svc_params + 3);
	if (!mif)
		off = cen_awdl_append_ota_tlv(buf, capacity, off,
					      CEN_AWDL_OTA_TLV_SERVICE_PARAMS,
					      svc_params, sizeof(svc_params));
	off = cen_awdl_append_election_tree(awdl, buf, capacity, off);
	off = cen_awdl_append_dp_state(awdl, buf, capacity, off);
	off = cen_awdl_append_ota_tlv(buf, capacity, off,
				      CEN_AWDL_OTA_TLV_HT_CAPS,
				      cen_awdl_ht_caps, sizeof(cen_awdl_ht_caps));
	if (awdl->vht_ie_len)
		off = cen_awdl_append_ota_tlv(buf, capacity, off,
					      CEN_AWDL_OTA_TLV_IE_CONTAINER,
					      awdl->vht_ie, awdl->vht_ie_len);
	if (mif)
		off = cen_awdl_append_arpa(awdl, buf, capacity, off);
	off = cen_awdl_append_ota_tlv(buf, capacity, off,
				      CEN_AWDL_OTA_TLV_VERSION,
				      version, sizeof(version));
	if (mif) {
		off = cen_awdl_append_ota_tlv(buf, capacity, off,
					      CEN_AWDL_OTA_TLV_SERVICE_PARAMS,
					      svc_params, sizeof(svc_params));
		if (awdl->svc_tlvs_len && off + awdl->svc_tlvs_len <= capacity) {
			memcpy(buf + off, awdl->svc_tlvs, awdl->svc_tlvs_len);
			off += awdl->svc_tlvs_len;
		}
	}
	return off;
}

static void cen_awdl_build_tx_clientdata(struct cen_awdl_tx_clientdata *cd,
					 bool mcbc, const u8 *dst_mac)
{
	memset(cd, 0, sizeof(*cd));
	cd->version = 0;
	cd->if_id = CEN_IF_AWDL;
	if (mcbc) {
		cd->tid = 6;
		cd->ac = 3;
		eth_broadcast_addr(cd->dst_addr);
	} else {
		cd->tid = 0;
		cd->ac = 0;
		ether_addr_copy(cd->dst_addr, dst_mac);
	}
}

static bool cen_awdl_parse_action_frame(const u8 *frame, size_t frame_len,
					struct cen_awdl_rx_peer_info *info)
{
	size_t pos;

	if (!frame || !info || frame_len < 40)
		return false;
	if ((frame[0] & 0xfcU) != 0xd0U)
		return false;
	if (frame[24] != 0x7fU || frame[25] != 0x00U ||
	    frame[26] != 0x17U || frame[27] != 0xf2U ||
	    frame[28] != 0x08U)
		return false;

	memset(info, 0, sizeof(*info));
	ether_addr_copy(info->mac, frame + 10);
	info->subtype = frame[30];
	{
		/* Delay between the planned and the actual transmission. */
		u32 phy_tx = get_unaligned_le32(frame + 32);
		u32 target_tx = get_unaligned_le32(frame + 36);

		if (phy_tx && target_tx && phy_tx > target_tx)
			info->tx_delay = phy_tx - target_tx;
	}

	pos = 40;
	while (pos + 3 <= frame_len) {
		u8 type = frame[pos];
		u16 tlen = get_unaligned_le16(frame + pos + 1);
		const u8 *val = frame + pos + 3;

		if (tlen > frame_len - pos - 3)
			break;
		switch (type) {
		case CEN_AWDL_OTA_TLV_SYNC_PARAMS:
			if (tlen >= 4 && val[3])
				info->master_channel = val[3];
			/*
			 * TX channel, le16 down counter, master channel,
			 * guard time, le16 AW period, le16 AF period, le16
			 * flags, le16 AW extension length, le16 AW length,
			 * le16 remaining, four extension counts, master
			 * address, presence mode, reserved, le16 AW sequence.
			 */
			if (tlen >= 31) {
				info->down_counter = get_unaligned_le16(val + 1);
				info->aw_period = get_unaligned_le16(val + 5);
				info->aw_ext_length = get_unaligned_le16(val + 11);
				info->aw_length = get_unaligned_le16(val + 13);
				info->presence = val[27];
				info->aw_seq = get_unaligned_le16(val + 29);
				info->has_sync = true;
			}
			break;
		case CEN_AWDL_OTA_TLV_ELECTION:
			if (tlen >= 19) {
				info->distance_from_top = val[3];
				ether_addr_copy(info->top_master, val + 5);
				info->top_master_metric =
					get_unaligned_le32(val + 11);
				info->self_metric =
					get_unaligned_le32(val + 15);
				info->has_election = true;
			}
			break;
		case CEN_AWDL_OTA_TLV_ELECTION_V2:
			/*
			 * master[6], sender[6], le32 master counter,
			 * distance, master metric, self metric, two
			 * reserved words and the sender's counter.
			 */
			if (tlen >= 28 && !info->has_election) {
				u32 dist = get_unaligned_le32(val + 16);

				ether_addr_copy(info->top_master, val);
				info->distance_from_top =
					dist > 254U ? 254U : (u8)dist;
				info->top_master_metric =
					get_unaligned_le32(val + 20);
				info->self_metric =
					get_unaligned_le32(val + 24);
				info->has_election = true;
			}
			break;
		case CEN_AWDL_OTA_TLV_ARPA:
			if (tlen >= 3) {
				size_t llen = min_t(size_t, val[1],
						    min_t(size_t, tlen - 2U,
							  CEN_AWDL_ARPA_MAX));

				memcpy(info->arpa, val + 2, llen);
				info->arpa[llen] = '\0';
			}
			break;
		default:
			break;
		}
		pos += 3 + tlen;
	}
	return true;
}

static bool cen_awdl_peer_wins_election(u32 cur_metric,
					const u8 cur_master[ETH_ALEN],
					u32 peer_metric,
					const u8 peer_master[ETH_ALEN])
{
	if (peer_metric != cur_metric)
		return peer_metric > cur_metric;
	return memcmp(peer_master, cur_master, ETH_ALEN) > 0;
}

/* Alignment and size of the radiotap fields up to the timestamp field. */
static const u8 cen_awdl_rtap_fields[][2] = {
	{ 8, 8 }, { 1, 1 }, { 1, 1 }, { 2, 4 }, { 2, 2 }, { 1, 1 }, { 1, 1 },
	{ 2, 2 }, { 2, 2 }, { 2, 2 }, { 1, 1 }, { 1, 1 }, { 1, 1 }, { 1, 1 },
	{ 2, 2 }, { 2, 2 }, { 1, 1 }, { 1, 1 }, { 4, 8 }, { 1, 3 }, { 4, 8 },
	{ 2, 12 }, { 8, 12 },
};

/*
 * Walk the radiotap header in front of a received management frame for
 * its length and the low 32 bits of TSFT and of the timestamp field. Only
 * the first presence word is walked: its fields come first.
 */
static bool cen_awdl_parse_radiotap(const u8 *buf, size_t len,
				    struct cen_awdl_rtap_info *rt)
{
	size_t pos = 4, hdr_len;
	unsigned int bit;
	u32 word;

	memset(rt, 0, sizeof(*rt));
	if (len < 8 || buf[0])
		return false;
	hdr_len = get_unaligned_le16(buf + 2);
	if (hdr_len < 8 || hdr_len > len || hdr_len > CEN_AWDL_RTAP_MAX)
		return false;
	rt->present = get_unaligned_le32(buf + 4);
	for (word = rt->present; word & BIT(31);
	     word = get_unaligned_le32(buf + pos)) {
		pos += 4;
		if (pos + 4 > hdr_len)
			return false;
	}
	pos += 4;
	for (bit = 0; bit < ARRAY_SIZE(cen_awdl_rtap_fields); bit++) {
		if (!(rt->present & BIT(bit)))
			continue;
		pos = ALIGN(pos, cen_awdl_rtap_fields[bit][0]);
		if (pos + cen_awdl_rtap_fields[bit][1] > hdr_len)
			break;
		if (bit == IEEE80211_RADIOTAP_TSFT) {
			rt->tsft = get_unaligned_le32(buf + pos);
			rt->has_tsft = true;
		} else if (bit == IEEE80211_RADIOTAP_TIMESTAMP) {
			rt->timestamp = get_unaligned_le32(buf + pos);
			rt->has_timestamp = true;
		}
		pos += cen_awdl_rtap_fields[bit][1];
	}
	rt->len = hdr_len;
	return true;
}

/* A parent frame's receive time: the radiotap timestamp field, else TSFT. */
static bool cen_awdl_rx_time(const struct cen_awdl_rtap_info *rt, u32 *time)
{
	if (rt->has_timestamp) {
		*time = rt->timestamp;
		return true;
	}
	if (rt->has_tsft) {
		*time = rt->tsft;
		return true;
	}
	return false;
}

/*
 * Check whether a received frame offers a better top master than ours
 * and snapshot the sender's timing from that same frame. Returns 0 to
 * follow the sender, -EAGAIN when it does not win, -EINVAL when its sync
 * parameters are unusable and -ENODATA without a receive time.
 */
static int cen_awdl_parent_from_frame(const struct centauri_awdl *awdl,
				      const struct cen_awdl_rx_peer_info *info,
				      const struct cen_awdl_rtap_info *rt,
				      struct cen_awdl_parent *p)
{
	if (!info->has_election || info->distance_from_top >= 254 ||
	    !is_valid_ether_addr(info->top_master) ||
	    ether_addr_equal(info->top_master, awdl->netdev->dev_addr))
		return -EAGAIN;
	if (!cen_awdl_peer_wins_election(le32_to_cpu(awdl->election.top_master_metric),
					 awdl->election.top_master,
					 info->top_master_metric,
					 info->top_master))
		return -EAGAIN;
	if (!info->has_sync || info->aw_ext_length != CEN_AWDL_AW_EXT_LENGTH ||
	    !info->presence)
		return -EINVAL;
	memset(p, 0, sizeof(*p));
	if (!cen_awdl_rx_time(rt, &p->rx_time))
		return -ENODATA;
	ether_addr_copy(p->addr, info->mac);
	ether_addr_copy(p->top_master, info->top_master);
	p->top_metric = info->top_master_metric;
	p->distance = info->distance_from_top + 1;
	p->channel = info->master_channel;
	p->tx_delay = info->tx_delay;
	p->down_counter = info->down_counter;
	p->aw_period = info->aw_period;
	p->aw_length = info->aw_length;
	p->presence = info->presence;
	p->aw_seq = info->aw_seq;
	return 0;
}

static size_t cen_awdl_append_tlv(u8 *buf, size_t capacity, size_t offset,
				  u16 tag, const void *val, u16 length)
{
	struct cen_awdl_tlv_hdr hdr;

	if (offset + sizeof(hdr) + length > capacity)
		return offset;
	hdr.tag = cpu_to_le16(tag);
	hdr.length = cpu_to_le16(length);
	memcpy(buf + offset, &hdr, sizeof(hdr));
	if (length)
		memcpy(buf + offset + sizeof(hdr), val, length);
	return offset + sizeof(hdr) + length;
}

static void cen_awdl_parse_defaults(struct centauri_awdl *awdl,
				    const u8 *reply, size_t size)
{
	size_t pos = 0;

	while (pos + sizeof(struct cen_awdl_tlv_hdr) <= size) {
		const struct cen_awdl_tlv_hdr *hdr = (const void *)(reply + pos);
		u16 tag = le16_to_cpu(hdr->tag);
		u16 len = le16_to_cpu(hdr->length);
		const u8 *val = reply + pos + sizeof(*hdr);

		if (len > size - pos - sizeof(*hdr))
			break;
		switch (tag) {
		case CEN_AWDL_TLV_CAPS:
			if (len >= 1)
				awdl->max_peers = val[0];
			break;
		case CEN_AWDL_TLV_GENERAL_CFG:
			if (len >= sizeof(awdl->general_cfg))
				memcpy(&awdl->general_cfg, val,
				       sizeof(awdl->general_cfg));
			break;
		case CEN_AWDL_TLV_ELECTION:
			if (len >= sizeof(awdl->election))
				memcpy(&awdl->election, val,
				       sizeof(awdl->election));
			break;
		case CEN_AWDL_TLV_EXTRA_ELECTION:
			if (len >= sizeof(awdl->extra_election)) {
				memcpy(awdl->extra_election, val,
				       sizeof(awdl->extra_election));
				awdl->extra_election_valid = true;
			}
			break;
		case CEN_AWDL_TLV_MIF_SYNC:
			if (len >= sizeof(awdl->mif_sync))
				memcpy(&awdl->mif_sync, val,
				       sizeof(awdl->mif_sync));
			break;
		case CEN_AWDL_TLV_SYNC_PARAMS:
			if (len >= 4 && len <= sizeof(awdl->sync_params)) {
				memcpy(awdl->sync_params, val, len);
				awdl->sync_params_len = len;
				if (awdl->sync_params[3])
					awdl->master_channel =
						awdl->sync_params[3];
			}
			break;
		case CEN_AWDL_TLV_VHT_IE:
			if (len >= 2 && len <= sizeof(awdl->vht_ie) &&
			    val[0] == WLAN_EID_VHT_CAPABILITY) {
				memcpy(awdl->vht_ie, val, len);
				awdl->vht_ie_len = len;
			}
			break;
		case CEN_AWDL_TLV_CHANSEQ:
			/*
			 * The firmware reports a channel-6-only sequence. Keep
			 * the host sequence, which also covers channel 149.
			 */
			break;
		default:
			break;
		}
		pos += sizeof(*hdr) + len;
	}
}

/*
 * Fill the synchronization fields of a 0xfe00 copy: synchronize, the
 * election role and the master to synchronize to (our own address while we
 * are the top master).
 */
static void cen_awdl_fill_sync_state(const struct centauri_awdl *awdl, u8 *xel)
{
	xel[CEN_AWDL_XEL_SYNC_ON] = 1;
	xel[CEN_AWDL_XEL_ROLE] = CEN_AWDL_SYNC_ROLE_ELECTION;
	xel[CEN_AWDL_XEL_ADV_VALID] = 0;
	if (!is_zero_ether_addr(awdl->sync_addr))
		ether_addr_copy(xel + CEN_AWDL_XEL_SYNC_ADDR, awdl->sync_addr);
	else if (awdl->netdev)
		ether_addr_copy(xel + CEN_AWDL_XEL_SYNC_ADDR,
				awdl->netdev->dev_addr);
}

/*
 * 0xfe00 that hands the firmware a parent's timing: the master to
 * synchronize to, the valid flag and the parent frame's receive time, TX
 * delay, down counter, AW period, AW length, presence mode and AW
 * sequence. The valid flag is never cached; later 0xfe00 pushes clear it.
 */
static void cen_awdl_build_advert(const struct centauri_awdl *awdl,
				  const struct cen_awdl_parent *p, u8 *xel)
{
	memcpy(xel, awdl->extra_election, CEN_AWDL_EXTRA_ELECTION_LEN);
	cen_awdl_fill_sync_state(awdl, xel);
	ether_addr_copy(xel + CEN_AWDL_XEL_SYNC_ADDR, p->addr);
	xel[CEN_AWDL_XEL_ADV_VALID] = 1;
	put_unaligned_le32(p->rx_time, xel + CEN_AWDL_XEL_ADV_RX_TIME);
	put_unaligned_le32(p->tx_delay, xel + CEN_AWDL_XEL_ADV_TX_DELAY);
	put_unaligned_le16(p->down_counter, xel + CEN_AWDL_XEL_ADV_DOWN_COUNTER);
	put_unaligned_le16(p->aw_period, xel + CEN_AWDL_XEL_ADV_AW_PERIOD);
	put_unaligned_le16(p->aw_length, xel + CEN_AWDL_XEL_ADV_AW_LENGTH);
	xel[CEN_AWDL_XEL_ADV_PRESENCE] = p->presence;
	put_unaligned_le16(p->aw_seq, xel + CEN_AWDL_XEL_ADV_AW_SEQ);
}

static size_t cen_awdl_build_start_payload(const struct centauri_awdl *awdl,
					   u8 *payload, size_t capacity)
{
	struct cen_awdl_election_params el = awdl->election;
	struct cen_awdl_mif_sync_params mif = awdl->mif_sync;
	u8 extra_el[CEN_AWDL_EXTRA_ELECTION_LEN];
	u8 sync[CEN_AWDL_SYNC_PARAMS_MAX];
	size_t off = 5;

	if (capacity < 5)
		return 0;
	memset(payload, 0, 5);
	put_unaligned_le16(0x0602, payload);
	{
		static const u8 gcfg[3];

		off = cen_awdl_append_tlv(payload, capacity, off,
					  CEN_AWDL_TLV_GENERAL_CFG, gcfg,
					  sizeof(gcfg));
	}

	if (awdl->netdev && is_zero_ether_addr(el.top_master))
		ether_addr_copy(el.top_master, awdl->netdev->dev_addr);
	off = cen_awdl_append_tlv(payload, capacity, off,
				  CEN_AWDL_TLV_ELECTION, &el, CEN_AWDL_EL_TX_LEN);

	if (awdl->extra_election_valid) {
		memcpy(extra_el, awdl->extra_election, sizeof(extra_el));
		cen_awdl_fill_sync_state(awdl, extra_el);
		off = cen_awdl_append_tlv(payload, capacity, off,
					  CEN_AWDL_TLV_EXTRA_ELECTION,
					  extra_el, CEN_AWDL_XEL_TX_LEN);
	}

	mif.enable_mif = 1;
	mif.enable_psf = 1;
	if (is_zero_ether_addr(mif.bssid))
		ether_addr_copy(mif.bssid, cen_awdl_default_bssid);
	off = cen_awdl_append_tlv(payload, capacity, off,
				  CEN_AWDL_TLV_MIF_SYNC, &mif, sizeof(mif));

	if (awdl->sync_params_len)
		off = cen_awdl_append_tlv(payload, capacity, off,
					  CEN_AWDL_TLV_SYNC_PARAMS,
					  sync, cen_awdl_sync_build(awdl, sync));

	return off;
}

static void cen_awdl_build_peer_add(struct cen_awdl_peer_add_req *req,
				    const u8 *mac,
				    const struct cen_awdl_chanseq *chanseq)
{
	memset(req, 0, sizeof(*req));
	ether_addr_copy(req->mac, mac);
	req->chanseq = *chanseq;
}

/* 0xfe0a as the traced host sends it after START and every ~5 s; meaning unknown. */
static void cen_awdl_push_fe0a_locked(struct centauri_awdl *awdl)
{
	static const u8 val[2] = { 0x6e, 0x01 };
	u8 buf[8];
	size_t len;

	len = cen_awdl_append_tlv(buf, sizeof(buf), 0, CEN_AWDL_TLV_FE0A,
				  val, sizeof(val));
	cen_awdl_cmd(awdl, CEN_AWDL_CMD_SET_PARAMS, buf, len, NULL, NULL);
}

static void cen_awdl_init_defaults(struct centauri_awdl *awdl)
{
	awdl->max_peers = 16;
	awdl->master_channel = 6;
	awdl->country[0] = 'U';
	awdl->country[1] = 'S';
	awdl->country[2] = 0;
	awdl->general_cfg.fw_llc_encap = 0;
	awdl->general_cfg.virtual_if_disable = 0;

	memset(&awdl->election, 0, sizeof(awdl->election));
	awdl->election.self_metric = cpu_to_le32(60);
	awdl->election.top_master_metric = cpu_to_le32(60);

	memset(awdl->extra_election, 0, sizeof(awdl->extra_election));
	awdl->extra_election[CEN_AWDL_XEL_SYNC_ON] = 1;
	awdl->extra_election[CEN_AWDL_XEL_ROLE] = CEN_AWDL_SYNC_ROLE_ELECTION;
	awdl->extra_election_valid = true;

	memset(&awdl->mif_sync, 0, sizeof(awdl->mif_sync));
	awdl->mif_sync.enable_mif = 1;
	awdl->mif_sync.enable_psf = 1;
	awdl->mif_sync.base_mif_offset = cpu_to_le16(16);
	awdl->mif_sync.mif_multiplier = 4;
	awdl->mif_sync.max_psf_lifetime = 3;
	ether_addr_copy(awdl->mif_sync.bssid, cen_awdl_default_bssid);
	awdl->mif_sync.psf_period_master = cpu_to_le16(110);
	awdl->mif_sync.psf_period_non_master = cpu_to_le16(440);
	strscpy(awdl->arpa, "aurora", sizeof(awdl->arpa));

	cen_awdl_build_default_chanseq(&awdl->chanseq, 6, 0, 149);
}

/*
 * The channel sequence at start, unless one was written to debugfs: with the
 * station on the lower channel of a 5 GHz 40 MHz pair, 6 then that channel
 * (as traced, the host keeps its schedule on the station's channel but for
 * channel 6); on a 2.4 GHz station channel, that channel only; else 6 and 149.
 */
static void cen_awdl_follow_station_chanseq(struct centauri_awdl *awdl)
{
	unsigned int c = centauri_wifi_station_channel(awdl->wifi);

	if (awdl->chanseq_written)
		return;
	if (c && c <= 14) {
		cen_awdl_build_default_chanseq(&awdl->chanseq, c, 0, 0);
		awdl->master_channel = c;
	} else if ((c >= 36 && c <= 140 && !((c - 36) % 8)) || c == 149 ||
		   c == 157) {
		cen_awdl_build_default_chanseq(&awdl->chanseq, 6, c, c);
		awdl->master_channel = 6;
	} else {
		cen_awdl_build_default_chanseq(&awdl->chanseq, 6, 0, 149);
		awdl->master_channel = 6;
	}
}

static int cen_awdl_push_chanseq_locked(struct centauri_awdl *awdl)
{
	size_t cmd_len = 0;
	int ret;

	lockdep_assert_held(centauri_wifi_lock_ptr(awdl->wifi));
	if (awdl->sync_params_len >= 4)
		awdl->sync_params[3] = awdl->master_channel;
	cmd_len = cen_awdl_append_tlv(awdl->start_buf, sizeof(awdl->start_buf),
				      cmd_len, CEN_AWDL_TLV_CHANSEQ,
				      &awdl->chanseq, CEN_AWDL_CS_TX_LEN);
	if (awdl->sync_params_len)
		cmd_len = cen_awdl_append_tlv(awdl->start_buf,
					      sizeof(awdl->start_buf), cmd_len,
					      CEN_AWDL_TLV_SYNC_PARAMS,
					      awdl->sync_out,
					      cen_awdl_sync_build(awdl, awdl->sync_out));
	ret = cen_awdl_cmd(awdl,
			   CEN_AWDL_CMD_SET_PARAMS,
			   awdl->start_buf, cmd_len, NULL, NULL);
	if (ret)
		dev_dbg(centauri_wifi_dev(awdl->wifi),
			"AWDL channel-sequence update returned %d\n", ret);
	return ret;
}

static int cen_awdl_push_extra_tlvs_locked(struct centauri_awdl *awdl);

static int cen_awdl_push_election_locked(struct centauri_awdl *awdl)
{
	u8 extra_el[CEN_AWDL_EXTRA_ELECTION_LEN];
	size_t cmd_len = 0;
	int ret;

	lockdep_assert_held(centauri_wifi_lock_ptr(awdl->wifi));
	cmd_len = cen_awdl_append_tlv(awdl->start_buf, sizeof(awdl->start_buf),
				      cmd_len, CEN_AWDL_TLV_ELECTION,
				      &awdl->election, CEN_AWDL_EL_TX_LEN);
	if (awdl->extra_election_valid) {
		memcpy(extra_el, awdl->extra_election, sizeof(extra_el));
		cen_awdl_fill_sync_state(awdl, extra_el);
		cmd_len = cen_awdl_append_tlv(awdl->start_buf,
					      sizeof(awdl->start_buf), cmd_len,
					      CEN_AWDL_TLV_EXTRA_ELECTION,
					      extra_el, CEN_AWDL_XEL_TX_LEN);
	}
	if (awdl->sync_params_len >= 4) {
		awdl->sync_params[3] = awdl->master_channel;
		cmd_len = cen_awdl_append_tlv(awdl->start_buf,
					      sizeof(awdl->start_buf), cmd_len,
					      CEN_AWDL_TLV_SYNC_PARAMS,
					      awdl->sync_out,
					      cen_awdl_sync_build(awdl, awdl->sync_out));
	}
	ret = cen_awdl_cmd(awdl,
			   CEN_AWDL_CMD_SET_PARAMS,
			   awdl->start_buf, cmd_len, NULL, NULL);
	if (ret)
		dev_dbg(centauri_wifi_dev(awdl->wifi),
			"AWDL election update returned %d\n", ret);
	else if (time_after(jiffies, awdl->tlvs_pushed_at + HZ))
		/* The election tree changed; at most one rebuild a second. */
		cen_awdl_push_extra_tlvs_locked(awdl);
	return ret;
}

static int cen_awdl_push_extra_tlvs_locked(struct centauri_awdl *awdl)
{
	size_t extra_len, mif_len, cmd_len = 0;
	int ret;

	lockdep_assert_held(centauri_wifi_lock_ptr(awdl->wifi));
	extra_len = cen_awdl_build_template(awdl, awdl->extra_tlv_buf,
					    sizeof(awdl->extra_tlv_buf), false);
	mif_len = cen_awdl_build_template(awdl, awdl->mif_tlv_buf,
					  sizeof(awdl->mif_tlv_buf), true);
	if (!extra_len)
		return 0;

	cmd_len = cen_awdl_append_tlv(awdl->start_buf, sizeof(awdl->start_buf),
				      cmd_len, CEN_AWDL_TLV_PSF_EXTRA,
				      awdl->extra_tlv_buf, extra_len);
	cmd_len = cen_awdl_append_tlv(awdl->start_buf, sizeof(awdl->start_buf),
				      cmd_len, CEN_AWDL_TLV_MIF_EXTRA,
				      awdl->mif_tlv_buf, mif_len);
	ret = cen_awdl_cmd(awdl,
			   CEN_AWDL_CMD_SET_PARAMS,
			   awdl->start_buf, cmd_len, NULL, NULL);
	if (ret) {
		awdl->extra_tlvs_len = 0;
		awdl->mif_tlvs_len = 0;
		dev_dbg(centauri_wifi_dev(awdl->wifi),
			"AWDL frame template update (%zu/%zu B) returned %d\n",
			extra_len, mif_len, ret);
		return ret;
	}
	awdl->extra_tlvs_len = extra_len;
	awdl->mif_tlvs_len = mif_len;
	awdl->tlvs_pushed_at = jiffies;
	awdl->tlvs_pushes++;
	return 0;
}

/*
 * Follow a parent: hand the firmware its timing on its own first, then
 * the new election state and the parent as the master to synchronize to.
 */
static void cen_awdl_follow_parent_locked(struct centauri_awdl *awdl,
					  const struct cen_awdl_parent *p)
{
	u8 xel[CEN_AWDL_EXTRA_ELECTION_LEN];
	size_t cmd_len;
	int ret;

	lockdep_assert_held(centauri_wifi_lock_ptr(awdl->wifi));
	cen_awdl_build_advert(awdl, p, xel);
	cmd_len = cen_awdl_append_tlv(awdl->start_buf, sizeof(awdl->start_buf),
				      0, CEN_AWDL_TLV_EXTRA_ELECTION,
				      xel, CEN_AWDL_XEL_TX_LEN);
	ret = cen_awdl_cmd(awdl,
			   CEN_AWDL_CMD_SET_PARAMS,
			   awdl->start_buf, cmd_len, NULL, NULL);
	if (ret) {
		/* Do not retry on every frame of the same parent. */
		awdl->follow_fails++;
		awdl->follow_rc = ret;
		WRITE_ONCE(awdl->follow_retry_at, jiffies + HZ);
		dev_info_ratelimited(centauri_wifi_dev(awdl->wifi),
				     "AWDL parent timing for %pM rejected: %d\n",
				     p->addr, ret);
		return;
	}
	ether_addr_copy(awdl->election.top_master, p->top_master);
	awdl->election.top_master_metric = cpu_to_le32(p->top_metric);
	awdl->election.distance_from_top = p->distance;
	ether_addr_copy(awdl->sync_addr, p->addr);
	if (p->channel)
		awdl->master_channel = p->channel;
	WRITE_ONCE(awdl->parent_seen, jiffies);
	WRITE_ONCE(awdl->following, true);
	awdl->adoptions++;
	ret = cen_awdl_push_election_locked(awdl);
	/* The traced host rebuilds its frame templates for the new tree. */
	if (!ret)
		ret = cen_awdl_push_extra_tlvs_locked(awdl);
	if (ret) {
		awdl->follow_fails++;
		awdl->follow_rc = ret;
	}
	dev_info_ratelimited(centauri_wifi_dev(awdl->wifi),
			     "AWDL following %pM: top master %pM metric %u distance %u, rx time %u tx delay %u down counter %u, election push %d\n",
			     p->addr, p->top_master, p->top_metric, p->distance,
			     p->rx_time, p->tx_delay, p->down_counter, ret);
	mod_delayed_work(system_percpu_wq, &awdl->follow_work, HZ);
}

static void cen_awdl_become_root_locked(struct centauri_awdl *awdl)
{
	lockdep_assert_held(centauri_wifi_lock_ptr(awdl->wifi));
	WRITE_ONCE(awdl->following, false);
	awdl->election.distance_from_top = 0;
	ether_addr_copy(awdl->election.top_master, awdl->netdev->dev_addr);
	awdl->election.top_master_metric = awdl->election.self_metric;
	ether_addr_copy(awdl->sync_addr, awdl->netdev->dev_addr);
	awdl->reverts++;
	cen_awdl_push_election_locked(awdl);
}

static void cen_awdl_follow_work(struct work_struct *work)
{
	struct centauri_awdl *awdl = container_of(to_delayed_work(work),
						  struct centauri_awdl,
						  follow_work);

	mutex_lock(centauri_wifi_lock_ptr(awdl->wifi));
	if (!awdl->active || !READ_ONCE(awdl->following) ||
	    centauri_wifi_is_stopping(awdl->wifi) ||
	    centauri_wifi_is_paused(awdl->wifi))
		goto out;
	if (time_after(jiffies, READ_ONCE(awdl->parent_seen) +
				CEN_AWDL_PARENT_TIMEOUT)) {
		dev_info(centauri_wifi_dev(awdl->wifi),
			 "AWDL parent %pM went quiet; top master again\n",
			 awdl->sync_addr);
		cen_awdl_become_root_locked(awdl);
	} else {
		schedule_delayed_work(&awdl->follow_work, HZ);
	}
out:
	mutex_unlock(centauri_wifi_lock_ptr(awdl->wifi));
}

/* The traced host re-sends its election and sync state every ~3.15 s. */
#define CEN_AWDL_REFRESH_MS		3150U

static void cen_awdl_schedule_refresh(struct centauri_awdl *awdl)
{
	mod_delayed_work(system_percpu_wq, &awdl->refresh_work,
			 msecs_to_jiffies(CEN_AWDL_REFRESH_MS));
}

/*
 * The periodic election refresh: the election, 0xfe00 and sync parameters
 * as one SET_PARAMS, then the MIF/PSF sync parameters with the sync
 * parameters again, as the traced host sends them on every refresh.
 */
static void cen_awdl_refresh_work(struct work_struct *work)
{
	struct centauri_awdl *awdl = container_of(to_delayed_work(work),
						  struct centauri_awdl,
						  refresh_work);
	struct cen_awdl_mif_sync_params mif;
	size_t cmd_len = 0;
	int ret;

	mutex_lock(centauri_wifi_lock_ptr(awdl->wifi));
	if (!awdl->active || centauri_wifi_is_stopping(awdl->wifi) ||
	    centauri_wifi_is_paused(awdl->wifi))
		goto out;
	if (is_zero_ether_addr(awdl->election.top_master)) {
		ether_addr_copy(awdl->election.top_master,
				awdl->netdev->dev_addr);
		awdl->election.top_master_metric = awdl->election.self_metric;
	}
	ret = cen_awdl_push_election_locked(awdl);
	if (!ret) {
		mif = awdl->mif_sync;
		mif.enable_mif = 1;
		mif.enable_psf = 1;
		if (is_zero_ether_addr(mif.bssid))
			ether_addr_copy(mif.bssid, cen_awdl_default_bssid);
		cmd_len = cen_awdl_append_tlv(awdl->start_buf,
					      sizeof(awdl->start_buf), cmd_len,
					      CEN_AWDL_TLV_MIF_SYNC,
					      &mif, sizeof(mif));
		if (awdl->sync_params_len >= 4)
			cmd_len = cen_awdl_append_tlv(awdl->start_buf,
						      sizeof(awdl->start_buf),
						      cmd_len,
						      CEN_AWDL_TLV_SYNC_PARAMS,
						      awdl->sync_out,
						      cen_awdl_sync_build(awdl, awdl->sync_out));
		ret = cen_awdl_cmd(awdl,
				   CEN_AWDL_CMD_SET_PARAMS,
				   awdl->start_buf, cmd_len,
				   NULL, NULL);
	}
	/* The traced host re-sends its frame templates in the same bursts. */
	if (!ret)
		ret = cen_awdl_push_extra_tlvs_locked(awdl);
	if (!ret)
		cen_awdl_push_fe0a_locked(awdl);
	awdl->refreshes++;
	awdl->refresh_rc = ret;
	cen_awdl_schedule_refresh(awdl);
out:
	mutex_unlock(centauri_wifi_lock_ptr(awdl->wifi));
}

static void cen_awdl_sync_active_rings(struct centauri_awdl *awdl)
{
	unsigned long flags;
	unsigned int i;

	spin_lock_irqsave(&awdl->peer_lock, flags);
	for (i = 0; i < CEN_AWDL_MAX_PEER_RINGS; i++) {
		const struct cen_awdl_tx_ring *r = &awdl->rings[i + 1];

		awdl->active_ring_valid[i] = r->opened && r->in_use;
		if (awdl->active_ring_valid[i])
			ether_addr_copy(awdl->active_ring_macs[i], r->peer_mac);
	}
	spin_unlock_irqrestore(&awdl->peer_lock, flags);
}

/*
 * Queue @mac for the firmware's peer cache. Only unicast TX sets @want_ring:
 * a peer only heard needs no TX ring.
 */
static void cen_awdl_queue_peer(struct centauri_awdl *awdl, const u8 *mac,
				bool want_ring)
{
	unsigned long flags;
	unsigned int i;
	bool queued = false;

	if (!awdl || !READ_ONCE(awdl->active) || !mac ||
	    !is_valid_ether_addr(mac))
		return;
	if (awdl->netdev && ether_addr_equal(mac, awdl->netdev->dev_addr))
		return;

	spin_lock_irqsave(&awdl->peer_lock, flags);
	for (i = 0; i < CEN_AWDL_MAX_PEER_RINGS; i++) {
		if (awdl->active_ring_valid[i] &&
		    ether_addr_equal(awdl->active_ring_macs[i], mac))
			goto out_unlock;
	}
	for (i = 0; i < awdl->pending_peer_count; i++) {
		if (ether_addr_equal(awdl->pending_peers[i].mac, mac)) {
			awdl->pending_peers[i].want_ring |= want_ring;
			goto out_unlock;
		}
	}
	if (awdl->pending_peer_count < CEN_AWDL_PENDING_PEERS_MAX) {
		struct cen_awdl_pending_peer *slot =
			&awdl->pending_peers[awdl->pending_peer_count++];

		ether_addr_copy(slot->mac, mac);
		slot->want_ring = want_ring;
		queued = true;
	}
out_unlock:
	spin_unlock_irqrestore(&awdl->peer_lock, flags);
	if (queued)
		schedule_work(&awdl->peer_work);
}

static void cen_awdl_close_ring_locked(struct centauri_awdl *awdl,
				       struct cen_awdl_tx_ring *ring)
{
	int ret;

	lockdep_assert_held(centauri_wifi_lock_ptr(awdl->wifi));
	if (!ring->opened)
		return;
	ret = centauri_wifi_close_awdl_tx_ring(awdl->wifi, ring->tr_id);
	if (ret)
		dev_dbg(centauri_wifi_dev(awdl->wifi),
			"AWDL TX ring %u close returned %d\n",
			ring->tr_id, ret);
	ring->opened = false;
	ring->in_use = false;
	memset(ring->tx_busy, 0, sizeof(ring->tx_busy));
	eth_zero_addr(ring->peer_mac);
}

static bool cen_awdl_ring_idle(const struct cen_awdl_tx_ring *ring)
{
	unsigned int i;

	for (i = 0; i < CEN_AWDL_TX_SLOTS; i++) {
		if (ring->tx_busy[i])
			return false;
	}
	return true;
}

static int cen_awdl_open_ring_locked(struct centauri_awdl *awdl,
				     struct cen_awdl_tx_ring *ring,
				     bool mcbc, const u8 *dst_mac)
{
	struct cen_awdl_tx_clientdata cd;
	int ret;

	cen_awdl_build_tx_clientdata(&cd, mcbc, dst_mac);
	ret = centauri_wifi_open_awdl_tx_ring(awdl->wifi, ring->tr_id,
					      ring->ring_offset,
					      CEN_AWDL_TX_SLOTS, &cd);
	if (ret)
		return ret;
	ring->opened = true;
	ring->in_use = true;
	memset(ring->tx_busy, 0, sizeof(ring->tx_busy));
	if (mcbc)
		eth_broadcast_addr(ring->peer_mac);
	else
		ether_addr_copy(ring->peer_mac, dst_mac);
	ring->last_used = jiffies;
	return 0;
}

static int cen_awdl_start_locked(struct centauri_awdl *awdl)
{
	size_t reply_size = sizeof(awdl->reply_buf);
	size_t start_size;
	int ret;

	lockdep_assert_held(centauri_wifi_lock_ptr(awdl->wifi));
	if (awdl->active)
		return 0;
	if (centauri_wifi_is_stopping(awdl->wifi) ||
	    centauri_wifi_is_paused(awdl->wifi))
		return -ENODEV;

	ret = centauri_wifi_ensure_shared_data_rings(awdl->wifi);
	if (ret)
		return ret;

	if (!awdl->defaults_loaded) {
		ret = cen_awdl_cmd(awdl,
				   CEN_AWDL_CMD_GET_DEFAULTS,
				   NULL, 0, awdl->reply_buf,
				   &reply_size);
		if (!ret && reply_size) {
			cen_awdl_parse_defaults(awdl, awdl->reply_buf,
						min(reply_size,
						    sizeof(awdl->reply_buf)));
			dev_dbg(centauri_wifi_dev(awdl->wifi),
				"AWDL firmware sync defaults: %*ph\n",
				(int)sizeof(awdl->extra_election),
				awdl->extra_election);
		} else {
			dev_dbg(centauri_wifi_dev(awdl->wifi),
				"AWDL default query returned %d; using built-in defaults\n",
				ret);
		}
		awdl->defaults_loaded = true;
		awdl->default_self_metric = awdl->election.self_metric;
		awdl->default_mif_sync = awdl->mif_sync;
		memcpy(awdl->default_xel_head, awdl->extra_election,
		       sizeof(awdl->default_xel_head));
	}
	/*
	 * Start values as traced: self metric 65, PSF periods 110/0 and 0xfe00
	 * bytes 1-3 as ba 00 00 (the firmware's defaults say 100/429 and c9 ba ce).
	 */
	awdl->election.self_metric = cpu_to_le32(65);
	awdl->mif_sync = awdl->default_mif_sync;
	memcpy(awdl->extra_election, awdl->default_xel_head,
	       sizeof(awdl->default_xel_head));
	awdl->mif_sync.psf_period_master = cpu_to_le16(110);
	awdl->mif_sync.psf_period_non_master = 0;
	awdl->extra_election[1] = 0xba;
	awdl->extra_election[2] = 0;
	awdl->extra_election[3] = 0;
	WRITE_ONCE(awdl->following, false);

	centauri_wifi_get_country(awdl->wifi, awdl->country);
	awdl->election.distance_from_top = 0;
	ether_addr_copy(awdl->election.top_master, awdl->netdev->dev_addr);
	awdl->election.top_master_metric = awdl->election.self_metric;
	ether_addr_copy(awdl->sync_addr, awdl->netdev->dev_addr);

	ret = cen_awdl_cmd(awdl,
			   CEN_AWDL_CMD_CONFIG_MAC,
			   awdl->netdev->dev_addr, ETH_ALEN,
			   NULL, NULL);
	if (ret) {
		dev_err(centauri_wifi_dev(awdl->wifi),
			"AWDL MAC configuration failed: %d\n", ret);
		return ret;
	}

	cen_awdl_follow_station_chanseq(awdl);
	if (awdl->sync_params_len >= 4)
		awdl->sync_params[3] = awdl->master_channel;
	start_size = cen_awdl_build_start_payload(awdl, awdl->start_buf,
						  sizeof(awdl->start_buf));
	ret = cen_awdl_cmd(awdl,
			   CEN_AWDL_CMD_START,
			   awdl->start_buf, start_size, NULL, NULL);
	if (ret) {
		dev_err(centauri_wifi_dev(awdl->wifi),
			"AWDL interface start failed: %d\n", ret);
		return ret;
	}

	if (!awdl->rings[0].opened) {
		ret = cen_awdl_open_ring_locked(awdl, &awdl->rings[0],
						true, NULL);
		if (ret) {
			u8 stop[5] = {};

			dev_err(centauri_wifi_dev(awdl->wifi),
				"AWDL MCBC TX ring %u open failed: %d\n",
				awdl->rings[0].tr_id, ret);
			cen_awdl_cmd(awdl,
				     CEN_AWDL_CMD_STOP,
				     stop, sizeof(stop), NULL, NULL);
			return ret;
		}
	}

	awdl->active = true;
	cen_awdl_push_chanseq_locked(awdl);
	cen_awdl_push_fe0a_locked(awdl);
	cen_awdl_push_extra_tlvs_locked(awdl);
	cen_awdl_schedule_refresh(awdl);
	dev_info(centauri_wifi_dev(awdl->wifi),
		 "AWDL interface %s active (%pM, master ch %u)\n",
		 awdl->netdev->name, awdl->netdev->dev_addr,
		 awdl->master_channel);
	centauri_wifi_schedule_poll(awdl->wifi);
	return 0;
}

static void cen_awdl_stop_locked(struct centauri_awdl *awdl)
{
	unsigned long flags;
	unsigned int i;
	u8 stop[5] = {};
	int ret;

	lockdep_assert_held(centauri_wifi_lock_ptr(awdl->wifi));
	if (!awdl->active)
		return;
	awdl->active = false;
	WRITE_ONCE(awdl->following, false);
	spin_lock_irqsave(&awdl->peer_lock, flags);
	awdl->pending_peer_count = 0;
	awdl->pending_election = false;
	memset(awdl->active_ring_valid, 0, sizeof(awdl->active_ring_valid));
	spin_unlock_irqrestore(&awdl->peer_lock, flags);
	for (i = 0; i < CEN_AWDL_TOTAL_TX_RINGS; i++)
		cen_awdl_close_ring_locked(awdl, &awdl->rings[i]);
	/* The firmware forgets its peers with AWDL */
	memset(awdl->recent_peers, 0, sizeof(awdl->recent_peers));
	if (!centauri_wifi_is_paused(awdl->wifi)) {
		ret = cen_awdl_cmd(awdl,
				   CEN_AWDL_CMD_STOP,
				   stop, sizeof(stop), NULL, NULL);
		if (ret)
			dev_dbg(centauri_wifi_dev(awdl->wifi),
				"AWDL interface stop returned %d\n", ret);
	}
	skb_queue_purge(&awdl->tx_queue);
}

static struct cen_awdl_tx_ring *
cen_awdl_find_peer_ring(struct centauri_awdl *awdl, const u8 *mac)
{
	unsigned int i;

	for (i = 1; i < CEN_AWDL_TOTAL_TX_RINGS; i++) {
		if (awdl->rings[i].opened && awdl->rings[i].in_use &&
		    ether_addr_equal(awdl->rings[i].peer_mac, mac))
			return &awdl->rings[i];
	}
	return NULL;
}

static struct cen_awdl_tx_ring *
cen_awdl_pick_peer_ring_locked(struct centauri_awdl *awdl, bool *reused)
{
	const unsigned int end = 1 + CEN_AWDL_MAX_PEER_RINGS;
	struct cen_awdl_tx_ring *lru = NULL;
	unsigned int i;

	*reused = false;
	for (i = 1; i < end; i++) {
		if (!awdl->rings[i].opened)
			return &awdl->rings[i];
	}
	for (i = 1; i < end; i++) {
		if (!awdl->rings[i].in_use &&
		    cen_awdl_ring_idle(&awdl->rings[i])) {
			*reused = true;
			return &awdl->rings[i];
		}
	}
	for (i = 1; i < end; i++) {
		struct cen_awdl_tx_ring *r = &awdl->rings[i];

		if (!cen_awdl_ring_idle(r) ||
		    time_before(jiffies, r->last_used + CEN_AWDL_RING_HOLD))
			continue;
		if (!lru || time_before(r->last_used, lru->last_used))
			lru = r;
	}
	if (lru)
		*reused = true;
	return lru;
}

/*
 * Give @mac to the firmware's peer cache unless @known, and with @want_ring
 * a unicast TX ring. A ring is taken from another peer only when that peer
 * has not used it for CEN_AWDL_RING_HOLD; that peer stays in the cache.
 */
static int cen_awdl_add_peer_locked(struct centauri_awdl *awdl, const u8 *mac,
				    bool known, bool want_ring)
{
	struct cen_awdl_peer_add_req req;
	struct cen_awdl_tx_ring *ring;
	bool reused = false;
	int ret;

	lockdep_assert_held(centauri_wifi_lock_ptr(awdl->wifi));
	if (!awdl->active)
		return -ENETDOWN;
	if (!is_valid_ether_addr(mac))
		return -EINVAL;

	if (!known) {
		/* Our own channel sequence stands in for the peer's. */
		cen_awdl_build_peer_add(&req, mac, &awdl->chanseq);
		ret = cen_awdl_cmd(awdl,
				   CEN_AWDL_CMD_PEER_ADD,
				   &req, sizeof(req), NULL, NULL);
		if (ret)
			return ret;
	}
	if (!want_ring)
		return 0;

	ring = cen_awdl_find_peer_ring(awdl, mac);
	if (ring) {
		ring->last_used = jiffies;
		return 0;
	}
	ring = cen_awdl_pick_peer_ring_locked(awdl, &reused);
	if (!ring)
		return 0;
	if (reused && ring->opened)
		cen_awdl_close_ring_locked(awdl, ring);
	ret = cen_awdl_open_ring_locked(awdl, ring, false, mac);
	if (ret) {
		dev_dbg(centauri_wifi_dev(awdl->wifi),
			"AWDL peer TX ring %u open failed (%d); using MCBC fallback\n",
			ring->tr_id, ret);
		cen_awdl_sync_active_rings(awdl);
		return 0;
	}
	if (reused)
		awdl->peer_ring_reuses++;
	cen_awdl_sync_active_rings(awdl);
	return 0;
}

static int cen_awdl_del_peer_locked(struct centauri_awdl *awdl, const u8 *mac)
{
	struct cen_awdl_tx_ring *ring;
	int ret;

	lockdep_assert_held(centauri_wifi_lock_ptr(awdl->wifi));
	if (!awdl->active)
		return -ENETDOWN;
	if (!is_valid_ether_addr(mac))
		return -EINVAL;

	ret = cen_awdl_cmd(awdl,
			   CEN_AWDL_CMD_PEER_DEL,
			   mac, ETH_ALEN, NULL, NULL);
	ring = cen_awdl_find_peer_ring(awdl, mac);
	if (ring) {
		cen_awdl_close_ring_locked(awdl, ring);
		cen_awdl_sync_active_rings(awdl);
	}
	return ret;
}

/*
 * Whether the firmware was given @mac within CEN_AWDL_PEER_RETRY before
 * @now. When it was not, @mac is recorded as given at @now in place of the
 * oldest entry of @recent.
 */
static bool cen_awdl_peer_added_lately(struct cen_awdl_recent_peer *recent,
				       const u8 *mac, unsigned long now)
{
	struct cen_awdl_recent_peer *slot = &recent[0];
	unsigned int i;

	for (i = 0; i < CEN_AWDL_RECENT_PEERS; i++) {
		struct cen_awdl_recent_peer *r = &recent[i];

		if (ether_addr_equal(r->mac, mac)) {
			if (time_before(now, r->added + CEN_AWDL_PEER_RETRY))
				return true;
			slot = r;
			break;
		}
		if (is_zero_ether_addr(r->mac)) {
			slot = r;
			break;
		}
		if (time_before(r->added, slot->added))
			slot = r;
	}
	ether_addr_copy(slot->mac, mac);
	slot->added = now;
	return false;
}

static void cen_awdl_peer_work(struct work_struct *work)
{
	struct centauri_awdl *awdl = container_of(work, struct centauri_awdl,
						  peer_work);
	struct cen_awdl_pending_peer peers[CEN_AWDL_PENDING_PEERS_MAX];
	struct cen_awdl_parent parent;
	bool do_election = false;
	unsigned long flags;
	u8 count, i;

	spin_lock_irqsave(&awdl->peer_lock, flags);
	count = awdl->pending_peer_count;
	if (count) {
		memcpy(peers, awdl->pending_peers, count * sizeof(peers[0]));
		awdl->pending_peer_count = 0;
	}
	if (awdl->pending_election) {
		do_election = true;
		parent = awdl->pending_parent;
		awdl->pending_election = false;
	}
	spin_unlock_irqrestore(&awdl->peer_lock, flags);
	if (!count && !do_election)
		return;

	mutex_lock(centauri_wifi_lock_ptr(awdl->wifi));
	if (!awdl->active || centauri_wifi_is_stopping(awdl->wifi) ||
	    centauri_wifi_is_paused(awdl->wifi))
		goto out;
	/* Register peers first, the parent among them when it was queued. */
	for (i = 0; i < count; i++) {
		bool known;

		if (cen_awdl_find_peer_ring(awdl, peers[i].mac))
			continue;
		known = cen_awdl_peer_added_lately(awdl->recent_peers,
						   peers[i].mac, jiffies);
		if (known && !peers[i].want_ring) {
			awdl->peer_holds++;
			continue;
		}
		if (!cen_awdl_add_peer_locked(awdl, peers[i].mac, known,
					      peers[i].want_ring) &&
		    !known)
			awdl->auto_peers++;
	}
	if (do_election &&
	    cen_awdl_peer_wins_election(le32_to_cpu(awdl->election.top_master_metric),
					awdl->election.top_master,
					parent.top_metric, parent.top_master))
		cen_awdl_follow_parent_locked(awdl, &parent);
out:
	mutex_unlock(centauri_wifi_lock_ptr(awdl->wifi));
}

static int cen_awdl_net_open(struct net_device *netdev)
{
	struct centauri_awdl *awdl = *(struct centauri_awdl **)netdev_priv(netdev);
	int ret;

	mutex_lock(centauri_wifi_lock_ptr(awdl->wifi));
	ret = cen_awdl_start_locked(awdl);
	mutex_unlock(centauri_wifi_lock_ptr(awdl->wifi));
	if (ret)
		return ret;
	netif_carrier_on(netdev);
	netif_start_queue(netdev);
	return 0;
}

static int cen_awdl_net_stop(struct net_device *netdev)
{
	struct centauri_awdl *awdl = *(struct centauri_awdl **)netdev_priv(netdev);

	netif_stop_queue(netdev);
	netif_carrier_off(netdev);
	mutex_lock(centauri_wifi_lock_ptr(awdl->wifi));
	cen_awdl_stop_locked(awdl);
	mutex_unlock(centauri_wifi_lock_ptr(awdl->wifi));
	cancel_work_sync(&awdl->peer_work);
	cancel_delayed_work_sync(&awdl->follow_work);
	cancel_delayed_work_sync(&awdl->refresh_work);
	return 0;
}

static netdev_tx_t cen_awdl_net_xmit(struct sk_buff *skb,
				     struct net_device *netdev)
{
	struct centauri_awdl *awdl = *(struct centauri_awdl **)netdev_priv(netdev);
	unsigned int overhead = READ_ONCE(awdl->general_cfg.fw_llc_encap) ?
				0U : CEN_AWDL_DATA_HDR_LEN;

	if (centauri_wifi_is_paused(awdl->wifi)) {
		netif_stop_queue(netdev);
		return NETDEV_TX_BUSY;
	}
	if (centauri_wifi_is_stopping(awdl->wifi) || !READ_ONCE(awdl->active) ||
	    skb->len < ETH_HLEN ||
	    skb->len > CEN_AWDL_TX_BUF_SIZE - overhead) {
		netdev->stats.tx_dropped++;
		dev_kfree_skb_any(skb);
		return NETDEV_TX_OK;
	}
	if (skb_queue_len(&awdl->tx_queue) >= CEN_AWDL_TX_QUEUE_LIMIT) {
		netif_stop_queue(netdev);
		return NETDEV_TX_BUSY;
	}
	skb_queue_tail(&awdl->tx_queue, skb);
	if (skb_queue_len(&awdl->tx_queue) >= CEN_AWDL_TX_QUEUE_LIMIT)
		netif_stop_queue(netdev);
	centauri_wifi_schedule_poll(awdl->wifi);
	return NETDEV_TX_OK;
}

static int cen_awdl_net_set_mac(struct net_device *netdev, void *data)
{
	struct centauri_awdl *awdl = *(struct centauri_awdl **)netdev_priv(netdev);
	struct sockaddr *addr = data;
	int ret;

	ret = eth_prepare_mac_addr_change(netdev, data);
	if (ret)
		return ret;
	mutex_lock(centauri_wifi_lock_ptr(awdl->wifi));
	if (awdl->active) {
		ret = -EBUSY;
		goto out;
	}
	eth_commit_mac_addr_change(netdev, data);
	dev_dbg(centauri_wifi_dev(awdl->wifi),
		"AWDL MAC updated to %pM\n", addr->sa_data);
out:
	mutex_unlock(centauri_wifi_lock_ptr(awdl->wifi));
	return ret;
}

static void cen_awdl_get_drvinfo(struct net_device *netdev,
				 struct ethtool_drvinfo *info)
{
	strscpy(info->driver, "apple_centauri", sizeof(info->driver));
	strscpy(info->bus_info, dev_name(netdev->dev.parent),
		sizeof(info->bus_info));
}

static const struct ethtool_ops cen_awdl_ethtool_ops = {
	.get_drvinfo = cen_awdl_get_drvinfo,
};

static const struct net_device_ops cen_awdl_net_ops = {
	.ndo_open = cen_awdl_net_open,
	.ndo_stop = cen_awdl_net_stop,
	.ndo_start_xmit = cen_awdl_net_xmit,
	.ndo_validate_addr = eth_validate_addr,
	.ndo_set_mac_address = cen_awdl_net_set_mac,
};

bool centauri_awdl_is_active(const struct centauri_awdl *awdl)
{
	return awdl && READ_ONCE(awdl->active);
}

void centauri_awdl_rx_frame(struct centauri_awdl *awdl, const u8 *frame,
			    unsigned int length)
{
	struct sk_buff *skb;
	size_t eth_len;
	bool was_encap = false;

	if (!awdl || !READ_ONCE(awdl->active) || !netif_running(awdl->netdev)) {
		if (awdl)
			awdl->rx_dropped++;
		return;
	}
	if (!frame || length < ETH_HLEN || length > CEN_AWDL_TX_BUF_SIZE) {
		awdl->rx_dropped++;
		awdl->netdev->stats.rx_dropped++;
		return;
	}
	skb = netdev_alloc_skb_ip_align(awdl->netdev, length);
	if (!skb) {
		awdl->rx_dropped++;
		awdl->netdev->stats.rx_dropped++;
		return;
	}
	eth_len = cen_awdl_decap_frame(skb_put(skb, 0), length, frame, length,
				       &was_encap);
	if (!eth_len) {
		dev_kfree_skb_any(skb);
		awdl->rx_dropped++;
		awdl->netdev->stats.rx_dropped++;
		return;
	}
	skb_put(skb, eth_len);
	cen_awdl_queue_peer(awdl, frame + ETH_ALEN, false);
	skb->protocol = eth_type_trans(skb, awdl->netdev);
	skb->ip_summed = CHECKSUM_NONE;
	awdl->rx_frames++;
	if (was_encap)
		awdl->rx_decap++;
	awdl->netdev->stats.rx_packets++;
	awdl->netdev->stats.rx_bytes += eth_len;
	netif_rx(skb);
}

void centauri_awdl_rx_mgmt(struct centauri_awdl *awdl, const u8 *payload,
			   size_t size)
{
	struct cen_awdl_rx_peer_info info;
	struct cen_awdl_rtap_info rt;
	struct cen_awdl_parent parent;
	size_t hdr = CEN_AWDL_RX_HDR_LEN;
	unsigned long flags;
	bool queue_el = false;
	int ret;

	if (!awdl || !READ_ONCE(awdl->active) || !payload)
		return;
	if (cen_awdl_parse_radiotap(payload, size, &rt))
		hdr = rt.len;
	if (size < hdr + 40 ||
	    !cen_awdl_parse_action_frame(payload + hdr, size - hdr, &info)) {
		/* Not where radiotap says: try the usual header length. */
		if (hdr == CEN_AWDL_RX_HDR_LEN ||
		    size < CEN_AWDL_RX_HDR_LEN + 40 ||
		    !cen_awdl_parse_action_frame(payload + CEN_AWDL_RX_HDR_LEN,
						 size - CEN_AWDL_RX_HDR_LEN,
						 &info))
			return;
		hdr = CEN_AWDL_RX_HDR_LEN;
		awdl->rtap_fallbacks++;
		/* The header did not describe this frame; trust none of it. */
		rt.has_tsft = false;
		rt.has_timestamp = false;
	}
	if (!is_valid_ether_addr(info.mac) ||
	    ether_addr_equal(info.mac, awdl->netdev->dev_addr))
		return;

	awdl->rtap_len = rt.len;
	awdl->rtap_present = rt.present;
	if (rt.has_tsft || rt.has_timestamp)
		awdl->rx_timed++;
	awdl->rx_action_frames++;
	if (info.subtype == 0)
		awdl->rx_psf++;
	else if (info.subtype == 3)
		awdl->rx_mif++;
	ether_addr_copy(awdl->last_rx_peer_mac, info.mac);
	if (info.has_election) {
		awdl->last_rx_peer_metric = info.top_master_metric;
		if (ether_addr_equal(info.top_master, awdl->netdev->dev_addr))
			awdl->rx_root_us++;
	}
	if (info.master_channel)
		awdl->last_rx_peer_chan = info.master_channel;
	if (READ_ONCE(awdl->following) &&
	    ether_addr_equal(info.mac, awdl->sync_addr))
		WRITE_ONCE(awdl->parent_seen, jiffies);

	/* Every peer heard in a sync or master indication frame is registered. */
	cen_awdl_queue_peer(awdl, info.mac, false);

	/* Follow a better master, as the traced host does within ~50 ms. */
	if (time_before(jiffies, READ_ONCE(awdl->follow_retry_at)))
		return;
	ret = cen_awdl_parent_from_frame(awdl, &info, &rt, &parent);
	if (ret == -EINVAL)
		awdl->adopt_bad_sync++;
	else if (ret == -ENODATA)
		awdl->adopt_no_time++;
	if (ret)
		return;
	spin_lock_irqsave(&awdl->peer_lock, flags);
	if (!awdl->pending_election ||
	    cen_awdl_peer_wins_election(awdl->pending_parent.top_metric,
					awdl->pending_parent.top_master,
					parent.top_metric, parent.top_master)) {
		awdl->pending_parent = parent;
		awdl->pending_election = true;
		queue_el = true;
	}
	spin_unlock_irqrestore(&awdl->peer_lock, flags);
	if (queue_el)
		schedule_work(&awdl->peer_work);
}

bool centauri_awdl_tx_complete(struct centauri_awdl *awdl, u16 ring_id,
			       u16 cookie, u16 wire_count, u8 flags, u8 status)
{
	struct cen_awdl_tx_ring *ring;
	unsigned int idx, first, i;
	u16 count;

	if (!awdl || ring_id < CEN_AWDL_MCBC_TR_ID ||
	    ring_id >= CEN_AWDL_MCBC_TR_ID + CEN_AWDL_TOTAL_TX_RINGS)
		return false;
	idx = ring_id - CEN_AWDL_MCBC_TR_ID;
	ring = &awdl->rings[idx];
	if (!ring->opened)
		return true;

	if (!centauri_wifi_tx_completion_span(cookie, wire_count, flags,
					      CEN_AWDL_TX_SLOTS, &first,
					      &count)) {
		ring->bad++;
		return true;
	}
	for (i = 0; i < count; i++) {
		unsigned int slot = (first + i) % CEN_AWDL_TX_SLOTS;

		if (!ring->tx_busy[slot])
			continue;
		ring->tx_busy[slot] = false;
		ring->completed++;
		if (status == 0 || status == 0x83) {
			awdl->netdev->stats.tx_packets++;
			awdl->netdev->stats.tx_bytes += ring->tx_length[slot];
		} else {
			ring->failed++;
			awdl->netdev->stats.tx_errors++;
		}
	}
	return true;
}

void centauri_awdl_drain_tx(struct centauri_awdl *awdl)
{
	struct cen_alpha_resources *a;
	bool posted_any = false;
	unsigned int budget = CEN_AWDL_TX_SLOTS - 1;

	if (!awdl || !awdl->active || !netif_running(awdl->netdev))
		return;
	a = centauri_wifi_alpha(awdl->wifi);

	while (budget--) {
		struct sk_buff *skb = skb_peek(&awdl->tx_queue);
		struct cen_awdl_tx_ring *ring = &awdl->rings[0];
		u8 dst[ETH_ALEN];
		u16 post, next;
		u32 buffer;
		u8 *descriptor;
		size_t wire_len;
		u64 metadata = (6ULL << 27) | (CEN_AWDL_TX_PROTO_CLASS << 15);

		if (!skb)
			break;
		if (!skb_copy_bits(skb, 0, dst, ETH_ALEN) &&
		    !is_multicast_ether_addr(dst)) {
			struct cen_awdl_tx_ring *peer =
				cen_awdl_find_peer_ring(awdl, dst);
			if (peer)
				ring = peer;
			else
				cen_awdl_queue_peer(awdl, dst, true);
		}
		if (!ring->opened)
			break;

		post = centauri_wifi_index_read(awdl->wifi, 3, ring->tr_id);
		if (post >= CEN_AWDL_TX_SLOTS) {
			ring->bad++;
			break;
		}
		next = (post + 1) % CEN_AWDL_TX_SLOTS;
		if (ring->tx_busy[post] ||
		    next == centauri_wifi_index_read(awdl->wifi, 1, ring->tr_id))
			break;

		skb = skb_dequeue(&awdl->tx_queue);
		if (!skb)
			break;

		buffer = ring->buf_offset + post * CEN_AWDL_TX_BUF_SIZE;
		if (!awdl->general_cfg.fw_llc_encap) {
			if (skb->len > sizeof(awdl->tx_stage) ||
			    skb_copy_bits(skb, 0, awdl->tx_stage, skb->len)) {
				awdl->netdev->stats.tx_dropped++;
				dev_kfree_skb_any(skb);
				continue;
			}
			wire_len = cen_awdl_encap_frame(a->window + buffer,
							CEN_AWDL_TX_BUF_SIZE,
							awdl->tx_stage,
							skb->len,
							awdl->tx_seq);
			if (!wire_len) {
				awdl->netdev->stats.tx_dropped++;
				dev_kfree_skb_any(skb);
				continue;
			}
			awdl->tx_seq = (awdl->tx_seq + 1) & 0x0fffU;
			awdl->tx_encap++;
		} else {
			if (skb->len > CEN_AWDL_TX_BUF_SIZE ||
			    skb_copy_bits(skb, 0, a->window + buffer, skb->len)) {
				awdl->netdev->stats.tx_dropped++;
				dev_kfree_skb_any(skb);
				continue;
			}
			wire_len = skb->len;
		}
		descriptor = a->window + ring->ring_offset + post * 32;
		memset(descriptor, 0, 32);
		put_unaligned_le32(3 | ((u32)wire_len << 8), descriptor);
		put_unaligned_le64(a->window_dma + buffer, descriptor + 4);
		put_unaligned_le16(post + 1, descriptor + 12);
		put_unaligned_le64(metadata, descriptor + 16);
		put_unaligned_le64(2000000, descriptor + 24);

		ring->tx_length[post] = wire_len;
		ring->tx_busy[post] = true;
		ring->submitted++;
		ring->last_used = jiffies;
		if (ring == &awdl->rings[0])
			awdl->tx_mcbc++;
		else
			awdl->tx_unicast++;

		dma_wmb();
		centauri_wifi_index_write(awdl->wifi, 3, ring->tr_id, next);
		dev_consume_skb_any(skb);
		posted_any = true;
	}

	if (posted_any) {
		dma_wmb();
		writel(1, a->bar + 0x1030);
	}
	if (awdl->active && !centauri_wifi_is_stopping(awdl->wifi) &&
	    !centauri_wifi_is_paused(awdl->wifi) &&
	    netif_running(awdl->netdev) &&
	    skb_queue_len(&awdl->tx_queue) < CEN_AWDL_TX_QUEUE_LIMIT)
		netif_wake_queue(awdl->netdev);
}

void centauri_awdl_handle_event(struct centauri_awdl *awdl, u32 event_id,
				u16 status, const u8 *packet, u16 size)
{
	u32 code = event_id & 0xffffffU;

	if (!awdl)
		return;
	awdl->events_seen++;
	awdl->last_event_id = code;
	awdl->last_event_status = status;

	switch (code) {
	case CEN_AWDL_EV_SYNC_STATE:
		awdl->sync_events++;
		if (packet && size > 28)
			awdl->sync_state = packet[28];
		break;
	case CEN_AWDL_EV_ELECTION_STATE:
		awdl->election_events++;
		break;
	case CEN_AWDL_EV_PEER_OP_DONE:
	case CEN_AWDL_EV_PEER_STATE:
		awdl->peer_events++;
		/*
		 * PEER_OP_DONE completes our own add or delete: queueing its
		 * peer again would undo a ring handover.
		 */
		if (packet && size >= 30) {
			ether_addr_copy(awdl->last_peer_mac, packet + 24);
			if (code == CEN_AWDL_EV_PEER_STATE && size >= 35) {
				awdl->last_peer_state = packet[34];
				if (packet[34])
					cen_awdl_queue_peer(awdl, packet + 24,
							    false);
			}
		}
		break;
	default:
		break;
	}
	dev_dbg(centauri_wifi_dev(awdl->wifi),
		"AWDL event=%#x status=%u size=%u\n", code, status, size);
}

static int cen_awdl_status_show(struct seq_file *s, void *unused)
{
	struct centauri_awdl *awdl = s->private;
	unsigned int i;

	mutex_lock(centauri_wifi_lock_ptr(awdl->wifi));
	seq_printf(s, "interface=%s active=%u defaults_loaded=%u mac=%pM bssid=%pM\n",
		   awdl->netdev->name, awdl->active, awdl->defaults_loaded,
		   awdl->netdev->dev_addr, awdl->mif_sync.bssid);
	seq_printf(s, "sync_addr=%pM distance=%u top_master=%pM top_metric=%u self_metric=%u\n",
		   awdl->sync_addr,
		   awdl->election.distance_from_top, awdl->election.top_master,
		   le32_to_cpu(awdl->election.top_master_metric),
		   le32_to_cpu(awdl->election.self_metric));
	seq_printf(s, "following=%u adoptions=%u reverts=%u follow_fails=%u follow_rc=%d bad_sync=%u no_time=%u rx_root_us=%u rx_timed=%u rtap_len=%u rtap_present=%08x rtap_fallbacks=%u\n",
		   awdl->following,
		   awdl->adoptions, awdl->reverts, awdl->follow_fails,
		   awdl->follow_rc, awdl->adopt_bad_sync,
		   awdl->adopt_no_time, awdl->rx_root_us, awdl->rx_timed,
		   awdl->rtap_len, awdl->rtap_present, awdl->rtap_fallbacks);
	seq_printf(s, "refreshes=%u refresh_rc=%d\n", awdl->refreshes,
		   awdl->refresh_rc);
	seq_printf(s, "master_channel=%u sync_state=%u max_peers=%u fw_llc_encap=%u extra_tlvs=%u arpa=%s mif_tlvs=%u svc_tlvs=%u vht_ie=%u tlv_pushes=%u\n",
		   awdl->master_channel, awdl->sync_state, awdl->max_peers,
		   awdl->general_cfg.fw_llc_encap, awdl->extra_tlvs_len,
		   awdl->arpa, awdl->mif_tlvs_len,
		   awdl->svc_tlvs_len, awdl->vht_ie_len, awdl->tlvs_pushes);
	seq_printf(s, "rx_frames=%u rx_decap=%u rx_dropped=%u rx_action=%u rx_psf=%u rx_mif=%u tx_mcbc=%u tx_unicast=%u tx_encap=%u tx_queued=%u auto_peers=%u ring_reuses=%u peer_holds=%u\n",
		   awdl->rx_frames, awdl->rx_decap, awdl->rx_dropped,
		   awdl->rx_action_frames, awdl->rx_psf, awdl->rx_mif,
		   awdl->tx_mcbc, awdl->tx_unicast, awdl->tx_encap,
		   skb_queue_len(&awdl->tx_queue), awdl->auto_peers,
		   awdl->peer_ring_reuses, awdl->peer_holds);
	seq_printf(s, "events=%u sync=%u election=%u peer=%u last_event=%#x last_status=%u last_peer=%pM last_peer_state=%u rx_peer=%pM rx_metric=%u rx_chan=%u\n",
		   awdl->events_seen, awdl->sync_events, awdl->election_events,
		   awdl->peer_events, awdl->last_event_id,
		   awdl->last_event_status, awdl->last_peer_mac,
		   awdl->last_peer_state, awdl->last_rx_peer_mac,
		   awdl->last_rx_peer_metric, awdl->last_rx_peer_chan);
	for (i = 0; i < CEN_AWDL_TOTAL_TX_RINGS; i++) {
		const struct cen_awdl_tx_ring *r = &awdl->rings[i];

		if (!r->opened)
			continue;
		seq_printf(s, "ring=%u role=%s peer=%pM sub=%u comp=%u fail=%u bad=%u\n",
			   r->tr_id, i == 0 ? "mcbc" : "peer", r->peer_mac,
			   r->submitted, r->completed, r->failed, r->bad);
	}
	mutex_unlock(centauri_wifi_lock_ptr(awdl->wifi));
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(cen_awdl_status);

static int cen_awdl_peers_show(struct seq_file *s, void *unused)
{
	struct centauri_awdl *awdl = s->private;
	unsigned int i;

	mutex_lock(centauri_wifi_lock_ptr(awdl->wifi));
	for (i = 1; i < CEN_AWDL_TOTAL_TX_RINGS; i++) {
		const struct cen_awdl_tx_ring *r = &awdl->rings[i];

		if (r->opened && r->in_use)
			seq_printf(s, "%pM ring=%u sub=%u comp=%u\n",
				   r->peer_mac, r->tr_id, r->submitted,
				   r->completed);
	}
	mutex_unlock(centauri_wifi_lock_ptr(awdl->wifi));
	return 0;
}

static ssize_t cen_awdl_peers_write(struct file *file, const char __user *ubuf,
				    size_t count, loff_t *pos)
{
	struct centauri_awdl *awdl = file_inode(file)->i_private;
	char buf[64];
	u8 mac[ETH_ALEN];
	bool add;
	int ret;

	if (count >= sizeof(buf))
		return -EINVAL;
	if (copy_from_user(buf, ubuf, count))
		return -EFAULT;
	buf[count] = '\0';

	if (!strncmp(buf, "add ", 4))
		add = true;
	else if (!strncmp(buf, "del ", 4))
		add = false;
	else
		return -EINVAL;

	if (!mac_pton(buf + 4, mac))
		return -EINVAL;

	mutex_lock(centauri_wifi_lock_ptr(awdl->wifi));
	ret = add ? cen_awdl_add_peer_locked(awdl, mac, false, true) :
		    cen_awdl_del_peer_locked(awdl, mac);
	mutex_unlock(centauri_wifi_lock_ptr(awdl->wifi));
	return ret ?: count;
}

static int cen_awdl_peers_open(struct inode *inode, struct file *file)
{
	return single_open(file, cen_awdl_peers_show, inode->i_private);
}

static const struct file_operations cen_awdl_peers_fops = {
	.owner = THIS_MODULE,
	.open = cen_awdl_peers_open,
	.read = seq_read,
	.write = cen_awdl_peers_write,
	.llseek = seq_lseek,
	.release = single_release,
};

static int cen_awdl_chanseq_show(struct seq_file *s, void *unused)
{
	struct centauri_awdl *awdl = s->private;
	unsigned int i;

	mutex_lock(centauri_wifi_lock_ptr(awdl->wifi));
	for (i = 0; i < CEN_AWDL_CHANSEQ_SLOTS; i++) {
		u16 entry = le16_to_cpu(awdl->chanseq.channels[i]);

		seq_printf(s, "%s%u", i ? "," : "", entry & 0xff);
	}
	seq_puts(s, "\nopclass ");
	for (i = 0; i < CEN_AWDL_CHANSEQ_SLOTS; i++)
		seq_printf(s, "%s%u", i ? "," : "",
			   le16_to_cpu(awdl->chanseq.channels[i]) >> 8);
	seq_putc(s, '\n');
	mutex_unlock(centauri_wifi_lock_ptr(awdl->wifi));
	return 0;
}

static ssize_t cen_awdl_chanseq_write(struct file *file,
				      const char __user *ubuf,
				      size_t count, loff_t *pos)
{
	struct centauri_awdl *awdl = file_inode(file)->i_private;
	char buf[64];
	unsigned int ch0 = 6, ch1 = 0, ch2 = 0;
	u8 tlv_buf[sizeof(struct cen_awdl_tlv_hdr) * 2 +
		   sizeof(struct cen_awdl_chanseq) + CEN_AWDL_SYNC_PARAMS_MAX];
	size_t tlv_len = 0;
	int parsed, ret = 0;

	if (count >= sizeof(buf))
		return -EINVAL;
	if (copy_from_user(buf, ubuf, count))
		return -EFAULT;
	buf[count] = '\0';

	parsed = sscanf(buf, "%u,%u,%u", &ch0, &ch1, &ch2);
	if (parsed < 1 || ch0 > 196 || ch1 > 196 || ch2 > 196)
		return -EINVAL;
	if (parsed == 1) {
		ch1 = 0;
		ch2 = 0;
	} else if (parsed == 2) {
		ch2 = ch1;
	}

	mutex_lock(centauri_wifi_lock_ptr(awdl->wifi));
	cen_awdl_build_default_chanseq(&awdl->chanseq, ch0, ch1, ch2);
	awdl->chanseq_written = true;
	awdl->master_channel = ch0 ?: 6;
	if (awdl->sync_params_len >= 4)
		awdl->sync_params[3] = awdl->master_channel;
	if (awdl->active) {
		tlv_len = cen_awdl_append_tlv(tlv_buf, sizeof(tlv_buf), tlv_len,
					      CEN_AWDL_TLV_CHANSEQ,
					      &awdl->chanseq,
					      CEN_AWDL_CS_TX_LEN);
		if (awdl->sync_params_len)
			tlv_len = cen_awdl_append_tlv(tlv_buf, sizeof(tlv_buf),
						      tlv_len,
						      CEN_AWDL_TLV_SYNC_PARAMS,
						      awdl->sync_out,
						      cen_awdl_sync_build(awdl, awdl->sync_out));
		ret = cen_awdl_cmd(awdl,
				   CEN_AWDL_CMD_SET_PARAMS,
				   tlv_buf, tlv_len, NULL, NULL);
		if (!ret)
			cen_awdl_push_extra_tlvs_locked(awdl);
	}
	mutex_unlock(centauri_wifi_lock_ptr(awdl->wifi));
	return ret ?: count;
}

static int cen_awdl_chanseq_open(struct inode *inode, struct file *file)
{
	return single_open(file, cen_awdl_chanseq_show, inode->i_private);
}

static const struct file_operations cen_awdl_chanseq_fops = {
	.owner = THIS_MODULE,
	.open = cen_awdl_chanseq_open,
	.read = seq_read,
	.write = cen_awdl_chanseq_write,
	.llseek = seq_lseek,
	.release = single_release,
};

static int cen_awdl_arpa_show(struct seq_file *s, void *unused)
{
	struct centauri_awdl *awdl = s->private;

	mutex_lock(centauri_wifi_lock_ptr(awdl->wifi));
	seq_printf(s, "%s\n", awdl->arpa);
	mutex_unlock(centauri_wifi_lock_ptr(awdl->wifi));
	return 0;
}

static ssize_t cen_awdl_arpa_write(struct file *file, const char __user *ubuf,
				   size_t count, loff_t *pos)
{
	struct centauri_awdl *awdl = file_inode(file)->i_private;
	char buf[CEN_AWDL_ARPA_MAX + 16];
	size_t len, i;
	int ret = 0;

	if (count >= sizeof(buf))
		return -EINVAL;
	if (copy_from_user(buf, ubuf, count))
		return -EFAULT;
	buf[count] = '\0';

	len = strlen(buf);
	while (len && isspace((unsigned char)buf[len - 1]))
		buf[--len] = '\0';
	if (len > 7 && !strcmp(buf + len - 7, ".local.")) {
		len -= 7;
		buf[len] = '\0';
	} else if (len > 6 && !strcmp(buf + len - 6, ".local")) {
		len -= 6;
		buf[len] = '\0';
	}
	if (!len || len > CEN_AWDL_ARPA_MAX)
		return -EINVAL;
	for (i = 0; i < len; i++) {
		unsigned char c = buf[i];

		if (!isalnum(c) && c != '-')
			return -EINVAL;
	}

	mutex_lock(centauri_wifi_lock_ptr(awdl->wifi));
	strscpy(awdl->arpa, buf, sizeof(awdl->arpa));
	if (awdl->active)
		ret = cen_awdl_push_extra_tlvs_locked(awdl);
	mutex_unlock(centauri_wifi_lock_ptr(awdl->wifi));
	return ret ?: count;
}

static int cen_awdl_arpa_open(struct inode *inode, struct file *file)
{
	return single_open(file, cen_awdl_arpa_show, inode->i_private);
}

/*
 * Raw over-the-air TLVs (u8 type, le16 length, value) appended to
 * master-indication frames by templates 1 and 2, e.g. service responses
 * for the services userspace advertises. An empty write clears them.
 */
static ssize_t cen_awdl_svc_write(struct file *file, const char __user *ubuf,
				  size_t count, loff_t *ppos)
{
	struct centauri_awdl *awdl = file_inode(file)->i_private;
	u8 buf[CEN_AWDL_SVC_TLVS_MAX];
	size_t pos = 0;
	int ret = 0;

	if (count > sizeof(buf))
		return -EINVAL;
	if (copy_from_user(buf, ubuf, count))
		return -EFAULT;
	while (pos < count) {
		if (count - pos < 3)
			return -EINVAL;
		pos += 3 + get_unaligned_le16(buf + pos + 1);
	}
	if (pos != count)
		return -EINVAL;

	mutex_lock(centauri_wifi_lock_ptr(awdl->wifi));
	memcpy(awdl->svc_tlvs, buf, count);
	awdl->svc_tlvs_len = count;
	awdl->svc_sui++;
	if (awdl->active)
		ret = cen_awdl_push_extra_tlvs_locked(awdl);
	mutex_unlock(centauri_wifi_lock_ptr(awdl->wifi));
	return ret ?: count;
}

static const struct file_operations cen_awdl_svc_fops = {
	.owner = THIS_MODULE,
	.write = cen_awdl_svc_write,
	.llseek = noop_llseek,
};

static const struct file_operations cen_awdl_arpa_fops = {
	.owner = THIS_MODULE,
	.open = cen_awdl_arpa_open,
	.read = seq_read,
	.write = cen_awdl_arpa_write,
	.llseek = seq_lseek,
	.release = single_release,
};

int centauri_awdl_register(struct centauri_wifi *wifi,
			   struct centauri_awdl **out)
{
	struct centauri_awdl *awdl;
	struct net_device *netdev;
	struct dentry *debug;
	unsigned int i;
	int ret;

	*out = NULL;
	if (!awdl_enable)
		return 0;

	awdl = kzalloc_obj(*awdl);
	if (!awdl)
		return -ENOMEM;

	netdev = alloc_etherdev(sizeof(struct centauri_awdl *));
	if (!netdev) {
		kfree(awdl);
		return -ENOMEM;
	}

	awdl->wifi = wifi;
	awdl->netdev = netdev;
	skb_queue_head_init(&awdl->tx_queue);
	spin_lock_init(&awdl->peer_lock);
	INIT_WORK(&awdl->peer_work, cen_awdl_peer_work);
	INIT_DELAYED_WORK(&awdl->follow_work, cen_awdl_follow_work);
	INIT_DELAYED_WORK(&awdl->refresh_work, cen_awdl_refresh_work);
	cen_awdl_init_defaults(awdl);

	for (i = 0; i < CEN_AWDL_TOTAL_TX_RINGS; i++) {
		awdl->rings[i].tr_id = CEN_AWDL_MCBC_TR_ID + i;
		awdl->rings[i].ring_offset = CEN_AWDL_TR_BASE +
					     i * CEN_AWDL_TR_STRIDE;
		awdl->rings[i].buf_offset = CEN_AWDL_TX_BUF_BASE +
					    i * CEN_AWDL_TX_RING_BUF_SPAN;
	}

	*(struct centauri_awdl **)netdev_priv(netdev) = awdl;
	netdev->netdev_ops = &cen_awdl_net_ops;
	netdev->ethtool_ops = &cen_awdl_ethtool_ops;
	netdev->flags |= IFF_MULTICAST;
	netdev->max_mtu = ETH_DATA_LEN;
	eth_hw_addr_random(netdev);
	SET_NETDEV_DEV(netdev, centauri_wifi_dev(wifi));
	strscpy(netdev->name, "awdl%d", IFNAMSIZ);

	ret = register_netdev(netdev);
	if (ret) {
		free_netdev(netdev);
		kfree(awdl);
		return ret;
	}

	debug = centauri_wifi_debugfs(wifi);
	if (debug) {
		awdl->status_file = debugfs_create_file("awdl_status", 0400,
							debug, awdl,
							&cen_awdl_status_fops);
		awdl->peers_file = debugfs_create_file("awdl_peers", 0600,
						       debug, awdl,
						       &cen_awdl_peers_fops);
		awdl->chanseq_file = debugfs_create_file("awdl_chanseq", 0600,
							 debug, awdl,
							 &cen_awdl_chanseq_fops);
		awdl->arpa_file = debugfs_create_file("awdl_arpa", 0600,
						      debug, awdl,
						      &cen_awdl_arpa_fops);
		awdl->svc_file = debugfs_create_file("awdl_service_tlvs", 0200,
						     debug, awdl,
						     &cen_awdl_svc_fops);
	}

	dev_info(centauri_wifi_dev(wifi),
		 "AWDL interface %s registered (%pM)\n",
		 netdev->name, netdev->dev_addr);
	*out = awdl;
	return 0;
}

void centauri_awdl_suspend(struct centauri_awdl *awdl)
{
	if (!awdl)
		return;
	if (awdl->netdev && awdl->netdev->reg_state == NETREG_REGISTERED)
		netif_tx_disable(awdl->netdev);
	mutex_lock(centauri_wifi_lock_ptr(awdl->wifi));
	cen_awdl_stop_locked(awdl);
	mutex_unlock(centauri_wifi_lock_ptr(awdl->wifi));
	cancel_work_sync(&awdl->peer_work);
	cancel_delayed_work_sync(&awdl->follow_work);
	cancel_delayed_work_sync(&awdl->refresh_work);
}

void centauri_awdl_unregister(struct centauri_awdl *awdl)
{
	if (!awdl)
		return;
	debugfs_remove(awdl->status_file);
	debugfs_remove(awdl->peers_file);
	debugfs_remove(awdl->chanseq_file);
	debugfs_remove(awdl->arpa_file);
	debugfs_remove(awdl->svc_file);
	if (awdl->netdev && awdl->netdev->reg_state == NETREG_REGISTERED)
		netif_tx_disable(awdl->netdev);
	mutex_lock(centauri_wifi_lock_ptr(awdl->wifi));
	cen_awdl_stop_locked(awdl);
	mutex_unlock(centauri_wifi_lock_ptr(awdl->wifi));
	cancel_work_sync(&awdl->peer_work);
	cancel_delayed_work_sync(&awdl->follow_work);
	cancel_delayed_work_sync(&awdl->refresh_work);
	skb_queue_purge(&awdl->tx_queue);
	if (awdl->netdev && awdl->netdev->reg_state == NETREG_REGISTERED)
		unregister_netdev(awdl->netdev);
	if (awdl->netdev)
		free_netdev(awdl->netdev);
	kfree(awdl);
}
