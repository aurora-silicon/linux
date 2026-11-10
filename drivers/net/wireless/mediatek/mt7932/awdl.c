// SPDX-License-Identifier: GPL-2.0-only
/*
 * Clean-room MT7932 AWDL support.  Firmware ABI values in this file come
 * from the two CLEAN-FACTS handoffs; Ethernet framing follows the licensed
 * brcmfmac AWDL helper in this tree.  Unlisted command bytes stay zero.
 */
#include "mt7932.h"

#include <linux/hex.h>

#define MT7932_AWDL_STA_SLOTS	15
#define MT7932_AWDL_WTBL_SLOTS	14

static bool awdl_sync_enable;
module_param_named(awdl_sync_enable, awdl_sync_enable, bool, 0444);
MODULE_PARM_DESC(awdl_sync_enable,
	"enable AWDL synchronization after complete templates/schedule are supplied (default false)");

/* TODO(t-5787 live payload capture): replace these unknown policy defaults. */
static uint awdl_psf_period;
module_param_named(awdl_psf_period, awdl_psf_period, uint, 0444);
MODULE_PARM_DESC(awdl_psf_period, "tag 0x03 PSF period (default 0; capture missing)");
static uint awdl_min_af_rate;
module_param_named(awdl_min_af_rate, awdl_min_af_rate, uint, 0444);
MODULE_PARM_DESC(awdl_min_af_rate, "tag 0x0d AF rate selector (default 0; capture missing)");
static int awdl_edge_rssi;
module_param_named(awdl_edge_rssi, awdl_edge_rssi, int, 0444);
MODULE_PARM_DESC(awdl_edge_rssi, "tag 0x07 edge RSSI (default 0; capture missing)");
static uint awdl_af_tx_mode;
module_param_named(awdl_af_tx_mode, awdl_af_tx_mode, uint, 0444);
MODULE_PARM_DESC(awdl_af_tx_mode, "tag 0x0c AF TX mode (default 0; capture missing)");
static uint awdl_election_metric;
module_param_named(awdl_election_metric, awdl_election_metric, uint, 0444);
MODULE_PARM_DESC(awdl_election_metric, "tag 0x0b election metric (default 0; capture missing)");

/* These words are copied, not decoded: their bit allocation is not known. */
static uint awdl_primary_master_channel;
module_param_named(awdl_primary_master_channel, awdl_primary_master_channel, uint, 0444);
MODULE_PARM_DESC(awdl_primary_master_channel, "verbatim tag 0x0e channel word (default 0)");
static uint awdl_secondary_master_channel;
module_param_named(awdl_secondary_master_channel, awdl_secondary_master_channel, uint, 0444);
MODULE_PARM_DESC(awdl_secondary_master_channel, "verbatim tag 0x0f channel word (default 0)");

/* TODO(t-5787 live payload capture): exact normal count/order is unknown. */
static uint awdl_channels[MT7932_AWDL_MAX_SCHEDULE];
static unsigned int awdl_channel_count;
module_param_array_named(awdl_channels, awdl_channels, uint, &awdl_channel_count, 0444);
MODULE_PARM_DESC(awdl_channels, "tag 0x02 channel list (default empty; capture missing)");
static uint awdl_operating_classes[MT7932_AWDL_MAX_SCHEDULE];
static unsigned int awdl_class_count;
module_param_array_named(awdl_operating_classes, awdl_operating_classes, uint,
			 &awdl_class_count, 0444);
MODULE_PARM_DESC(awdl_operating_classes,
	"tag 0x02 operating-class list (default empty; capture missing; examples 149/128 and 6/81)");

static char *awdl_bssid = "00:25:00:ff:94:73";
module_param_named(awdl_bssid, awdl_bssid, charp, 0444);
MODULE_PARM_DESC(awdl_bssid, "AWDL BSSID (default public common AWDL BSSID)");
static char *awdl_sync_master = "00:00:00:00:00:00";
module_param_named(awdl_sync_master, awdl_sync_master, charp, 0444);
MODULE_PARM_DESC(awdl_sync_master, "host-selected tag 0x09 sync-master MAC");
static char *awdl_top_master = "0000000000000000";
module_param_named(awdl_top_master, awdl_top_master, charp, 0444);
MODULE_PARM_DESC(awdl_top_master, "host-selected tag 0x0a 8-byte top-master identity as hex");
static uint awdl_election_id;
module_param_named(awdl_election_id, awdl_election_id, uint, 0444);
static uint awdl_distance;
module_param_named(awdl_distance, awdl_distance, uint, 0444);
static uint awdl_top_master_metric;
module_param_named(awdl_top_master_metric, awdl_top_master_metric, uint, 0444);
static uint awdl_self_metric;
module_param_named(awdl_self_metric, awdl_self_metric, uint, 0444);

static char *awdl_broadcast_template = "";
module_param_named(awdl_broadcast_template, awdl_broadcast_template, charp, 0444);
MODULE_PARM_DESC(awdl_broadcast_template, "broadcast AWDL payload/TLV bytes as hex (default empty)");
static char *awdl_psf_template = "";
module_param_named(awdl_psf_template, awdl_psf_template, charp, 0444);
MODULE_PARM_DESC(awdl_psf_template, "PSF AWDL payload/TLV bytes as hex (default empty)");

static const u8 mt7932_awdl_snap[10] = {
	0xaa, 0xaa, 0x03, 0x00, 0x17, 0xf2, 0x08, 0x00, 0x03, 0x04,
};

int mt7932_awdl_build_queue(u8 out[MT7932_AWDL_QUEUE_RECORD_LEN],
			    u8 station, bool peer, bool enable)
{
	static const u8 tids[8] = { 1, 0, 0, 1, 2, 2, 3, 3 };

	memset(out, 0, MT7932_AWDL_QUEUE_RECORD_LEN);
	if (!peer) {
		put_unaligned_le16(enable ? 2 : 0, out);
		out[2] = 1;
		out[3] = MT7932_AWDL_BSS_INDEX;
		if (enable)
			out[14] = 1;
		return MT7932_AWDL_QUEUE_RECORD_LEN;
	}
	if (station >= MT7932_AWDL_STA_SLOTS)
		return -EINVAL;
	put_unaligned_le16(15, out);
	out[2] = 5;
	out[14] = 1;
	out[15] = station;
	put_unaligned_le16(enable ? 15 : 2, out + 16);
	memcpy(out + 20, tids, sizeof(tids));
	return MT7932_AWDL_QUEUE_RECORD_LEN;
}

int mt7932_awdl_build_bss_active(u8 out[12], const u8 *mac, u8 bmc_wtbl,
				 bool active)
{
	if (!mac || !is_valid_ether_addr(mac) || bmc_wtbl >= MT7932_AWDL_WTBL_SLOTS)
		return -EINVAL;
	memset(out, 0, 12);
	out[0] = MT7932_AWDL_BSS_INDEX;
	out[1] = active;
	out[2] = MT7932_AWDL_NETWORK_TYPE;
	out[3] = 0; /* own-MAC index, deliberately not the BSS index */
	if (active)
		ether_addr_copy(out + 4, mac);
	out[10] = bmc_wtbl;
	out[11] = 1;
	return 12;
}

int mt7932_awdl_build_bss(u8 out[MT7932_AWDL_BSS_RECORD_LEN], u8 bmc_wtbl,
			  u8 wmm, u8 phy, u8 nss)
{
	if (bmc_wtbl >= MT7932_AWDL_WTBL_SLOTS || !nss)
		return -EINVAL;
	memset(out, 0, MT7932_AWDL_BSS_RECORD_LEN);
	out[0] = 1;
	out[1] = 1;
	out[2] = 7;
	out[42] = 1;
	put_unaligned_le16(0x3fc0, out + 44);
	put_unaligned_le16(0x0540, out + 46);
	put_unaligned_le16(0x004b, out + 50);
	out[52] = 2;
	out[55] = phy;
	out[58] = bmc_wtbl;
	out[68] = 1;
	out[69] = 2;
	out[70] = 44;
	out[81] = 1;
	out[82] = 1;
	out[83] = 2;
	out[84] = 155;
	put_unaligned_le16(0x0540, out + 86);
	out[88] = nss;
	out[89] = nss;
	out[90] = 4;
	out[91] = wmm;
	out[93] = nss;
	put_unaligned_le16(0x3ff0, out + 96);
	return MT7932_AWDL_BSS_RECORD_LEN;
}

int mt7932_awdl_build_sta(u8 out[MT7932_AWDL_STA_RECORD_LEN], u8 station,
			  u8 wtbl, const u8 *mac,
			  const struct mt7932_awdl_peer_caps *caps,
			  bool live, bool need_response)
{
	if (station >= MT7932_AWDL_STA_SLOTS || wtbl >= MT7932_AWDL_WTBL_SLOTS ||
	    !mac || !is_valid_ether_addr(mac) || !caps)
		return -EINVAL;
	memset(out, 0, MT7932_AWDL_STA_RECORD_LEN);
	out[0] = station;
	out[1] = 0x10;
	ether_addr_copy(out + 2, mac);
	out[12] = 1;
	out[13] = caps->phy;
	put_unaligned_le16(caps->rates, out + 14);
	put_unaligned_le16(0x0540, out + 16);
	out[18] = 1;
	out[20] = live ? 2 : 0;
	out[21] = caps->ht_mcs;
	out[22] = caps->mcs32;
	out[23] = 1;
	memcpy(out + 24, caps->rx_mcs, sizeof(caps->rx_mcs));
	put_unaligned_le16(caps->rx_highest, out + 34);
	put_unaligned_le16(caps->ht_cap, out + 40);
	put_unaligned_le16(caps->ht_ext_cap, out + 42);
	put_unaligned_le32(caps->tx_bf_cap, out + 44);
	out[48] = caps->ampdu;
	out[49] = caps->asel;
	out[50] = caps->rcpi;
	out[51] = need_response;
	out[54] = wtbl;
	out[55] = 0xff;
	put_unaligned_le32(caps->vht_cap, out + 56);
	put_unaligned_le16(caps->vht_rx_mcs, out + 60);
	put_unaligned_le16(caps->vht_rx_highest, out + 62);
	put_unaligned_le16(caps->vht_tx_mcs, out + 64);
	put_unaligned_le16(caps->vht_tx_highest, out + 66);
	out[68] = caps->nss > 1 ? 2 : 1;
	out[125] = 1;
	memcpy(out + 136, caps->he, sizeof(caps->he));
	return MT7932_AWDL_STA_RECORD_LEN;
}

int mt7932_awdl_build_tlv(u8 *out, size_t capacity, u32 tag,
			  const void *body, size_t body_len)
{
	if (!out || body_len > U32_MAX || body_len + 12 > capacity ||
	    (body_len && !body))
		return -EINVAL;
	memset(out, 0, body_len + 12);
	put_unaligned_le16(1, out);
	put_unaligned_le32(tag, out + 4);
	put_unaligned_le32(body_len, out + 8);
	if (body_len)
		memcpy(out + 12, body, body_len);
	return body_len + 12;
}

int mt7932_awdl_build_channel_sequence(u8 out[134], const u8 *channels,
				       const u8 *classes, size_t count)
{
	if (count > MT7932_AWDL_MAX_SCHEDULE || (count && (!channels || !classes)))
		return -EINVAL;
	memset(out, 0, 134);
	put_unaligned_le32(MT7932_AWDL_BSS_INDEX, out);
	out[4] = 4;
	out[5] = count;
	if (count) {
		memcpy(out + 6, channels, count);
		memcpy(out + 70, classes, count);
	}
	return 134;
}

int mt7932_awdl_event_parse(const struct mt7932_event *event,
			    struct mt7932_awdl_event_info *info)
{
	if (!event || !info || event->eid != 0xf7)
		return -ENOMSG;
	if (event->length < 44)
		return -EMSGSIZE;
	info->subtype = get_unaligned_le32(event->packet + 40);
	info->data = event->packet + 44;
	info->length = event->length - 44;
	return 0;
}

int mt7932_awdl_encap(u8 *out, size_t capacity, const u8 *ethernet,
			 size_t length, u16 sequence)
{
	if (!out || !ethernet || length < ETH_HLEN ||
	    length + MT7932_AWDL_ENCAP_LEN > capacity ||
	    length - ETH_HLEN + MT7932_AWDL_ENCAP_LEN > ETH_DATA_LEN)
		return -EMSGSIZE;
	if (get_unaligned_be16(ethernet + 12) < ETH_P_802_3_MIN) {
		memcpy(out, ethernet, length);
		return length;
	}
	memcpy(out, ethernet, 2 * ETH_ALEN);
	put_unaligned_be16(length - ETH_HLEN + MT7932_AWDL_ENCAP_LEN, out + 12);
	memcpy(out + ETH_HLEN, mt7932_awdl_snap, sizeof(mt7932_awdl_snap));
	put_unaligned_le16(sequence, out + ETH_HLEN + sizeof(mt7932_awdl_snap));
	out[ETH_HLEN + sizeof(mt7932_awdl_snap) + 2] = 0;
	out[ETH_HLEN + sizeof(mt7932_awdl_snap) + 3] = 0;
	/* The new 16-byte area replaces the old ethertype position; retain the
	 * original ethertype immediately after that area.
	 */
	memcpy(out + 2 * ETH_ALEN + MT7932_AWDL_ENCAP_LEN,
	       ethernet + 2 * ETH_ALEN, length - 2 * ETH_ALEN);
	return length + MT7932_AWDL_ENCAP_LEN;
}

int mt7932_awdl_decap(u8 *frame, size_t *length)
{
	if (!frame || !length || *length < ETH_HLEN + MT7932_AWDL_ENCAP_LEN)
		return -EMSGSIZE;
	if (get_unaligned_be16(frame + 12) > ETH_DATA_LEN ||
	    memcmp(frame + ETH_HLEN, mt7932_awdl_snap, sizeof(mt7932_awdl_snap)))
		return -EPROTO;
	memmove(frame + MT7932_AWDL_ENCAP_LEN, frame, 2 * ETH_ALEN);
	memmove(frame, frame + MT7932_AWDL_ENCAP_LEN, *length - MT7932_AWDL_ENCAP_LEN);
	*length -= MT7932_AWDL_ENCAP_LEN;
	return 0;
}

static int mt_awdl_send_tlv(struct mt7932 *m, u32 tag, const void *body,
			    size_t body_len)
{
	u8 *payload;
	int length, ret;

	payload = kmalloc(body_len + 12, GFP_KERNEL);
	if (!payload)
		return -ENOMEM;
	length = mt7932_awdl_build_tlv(payload, body_len + 12, tag, body, body_len);
	ret = length < 0 ? length : mt_request(m, 0xf7, true, true, false,
					       payload, length);
	kfree(payload);
	return ret;
}

static int mt_awdl_fixed_u32(struct mt7932 *m, u32 tag, u32 value)
{
	u8 body[8] = {};

	put_unaligned_le32(1, body);
	put_unaligned_le32(value, body + 4);
	return mt_awdl_send_tlv(m, tag, body, sizeof(body));
}

static int mt_awdl_capability_prefix(struct mt7932 *m, u8 out[40])
{
	const struct ieee80211_sta_ht_cap *ht = &m->band5.ht_cap;
	const struct ieee80211_sta_vht_cap *vht = &m->band5.vht_cap;

	if (!ht->ht_supported || !vht->vht_supported)
		return -EOPNOTSUPP;
	memset(out, 0, 40);
	out[0] = 0x07;
	put_unaligned_le16(20, out + 1);
	put_unaligned_le16(ht->cap, out + 5);
	out[7] = ht->ampdu_factor | (ht->ampdu_density << 2);
	memcpy(out + 8, ht->mcs.rx_mask, 4);
	out[23] = 0x11;
	put_unaligned_le16(14, out + 24);
	out[26] = WLAN_EID_VHT_CAPABILITY;
	out[27] = 12;
	put_unaligned_le32(vht->cap, out + 28);
	memcpy(out + 32, &vht->vht_mcs, sizeof(vht->vht_mcs));
	return 40;
}

static int mt_awdl_template(struct mt7932 *m, u32 tag, const char *hex)
{
	u8 prefix[40], *body;
	size_t chars = strlen(hex), bytes;
	int prefix_len, ret;

	if (chars & 1)
		return -EINVAL;
	bytes = chars / 2;
	if (bytes > 1024)
		return -E2BIG;
	prefix_len = mt_awdl_capability_prefix(m, prefix);
	if (prefix_len < 0)
		return prefix_len;
	body = kzalloc(8 + prefix_len + bytes, GFP_KERNEL);
	if (!body)
		return -ENOMEM;
	put_unaligned_le32(1, body);
	put_unaligned_le32(prefix_len + bytes, body + 4);
	memcpy(body + 8, prefix, prefix_len);
	if (bytes && hex2bin(body + 8 + prefix_len, hex, bytes)) {
		ret = -EINVAL;
		goto out;
	}
	ret = mt_awdl_send_tlv(m, tag, body, 8 + prefix_len + bytes);
out:
	kfree(body);
	return ret;
}

static int mt_awdl_configure_sync(struct mt7932 *m)
{
	u8 body[134], mac[ETH_ALEN], top[8], channels[64], classes[64];
	u8 election[21] = {}, enable[6] = {};
	unsigned int i;
	int ret;

	if (!awdl_sync_enable)
		return 0;
	if (!awdl_channel_count || awdl_channel_count != awdl_class_count ||
	    !*awdl_broadcast_template || !*awdl_psf_template)
		return -EINVAL;
	for (i = 0; i < awdl_channel_count; i++) {
		if (awdl_channels[i] > U8_MAX || awdl_operating_classes[i] > U8_MAX)
			return -ERANGE;
		channels[i] = awdl_channels[i];
		classes[i] = awdl_operating_classes[i];
	}
	if (!mac_pton(awdl_sync_master, mac) || strlen(awdl_top_master) != 16 ||
	    hex2bin(top, awdl_top_master, sizeof(top)))
		return -EINVAL;
	ret = mt_awdl_fixed_u32(m, 0x0e, awdl_primary_master_channel);
	ret = ret ?: mt_awdl_fixed_u32(m, 0x0f, awdl_secondary_master_channel);
	memset(body, 0, sizeof(body));
	put_unaligned_le32(1, body);
	body[4] = awdl_min_af_rate;
	ret = ret ?: mt_awdl_send_tlv(m, 0x0d, body, 5);
	body[4] = clamp_t(int, awdl_edge_rssi, S8_MIN, S8_MAX);
	ret = ret ?: mt_awdl_send_tlv(m, 0x07, body, 5);
	if (ret)
		return ret;
	mt7932_awdl_build_channel_sequence(body, channels, classes, awdl_channel_count);
	ret = mt_awdl_send_tlv(m, 0x02, body, 134);
	memset(body, 0, 30);
	put_unaligned_le32(1, body);
	ether_addr_copy(body + 4, mac);
	ret = ret ?: mt_awdl_send_tlv(m, 0x09, body, 30);
	put_unaligned_le32(1, election);
	put_unaligned_le16(awdl_election_id, election + 4);
	election[6] = awdl_distance;
	memcpy(election + 7, top, sizeof(top));
	put_unaligned_le32(awdl_top_master_metric, election + 15);
	put_unaligned_le16(awdl_self_metric, election + 19);
	ret = ret ?: mt_awdl_send_tlv(m, 0x0a, election, sizeof(election));
	ret = ret ?: mt_awdl_fixed_u32(m, 0x0b, awdl_election_metric);
	ret = ret ?: mt_awdl_fixed_u32(m, 0x0c, awdl_af_tx_mode);
	ret = ret ?: mt_awdl_fixed_u32(m, 0x03, awdl_psf_period);
	ret = ret ?: mt_awdl_template(m, 0x04, awdl_broadcast_template);
	ret = ret ?: mt_awdl_template(m, 0x05, awdl_psf_template);
	put_unaligned_le32(1, enable);
	put_unaligned_le16(1, enable + 4);
	ret = ret ?: mt_awdl_send_tlv(m, 0x08, enable, sizeof(enable));
	if (!ret)
		m->awdl.sync_enabled = true;
	return ret;
}

static int mt_awdl_alloc_id(unsigned long *map, unsigned int limit)
{
	unsigned int id = find_first_zero_bit(map, limit);

	if (id >= limit)
		return -ENOSPC;
	__set_bit(id, map);
	return id;
}

static void mt_awdl_release_peer(struct mt7932 *m, struct mt7932_awdl_peer *peer)
{
	__clear_bit(peer->station, &m->station_alloc);
	__clear_bit(peer->wtbl, &m->wtbl_alloc);
	memset(peer, 0, sizeof(*peer));
}

static struct mt7932_awdl_peer *mt_awdl_find_peer(struct mt7932 *m,
						   const u8 *mac)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(m->awdl.peers); i++)
		if (m->awdl.peers[i].live &&
		    ether_addr_equal(m->awdl.peers[i].addr, mac))
			return &m->awdl.peers[i];
	return NULL;
}

static void mt_awdl_caps(struct mt7932_awdl_peer_caps *caps,
			 const struct station_parameters *params)
{
	const struct link_station_parameters *link = &params->link_sta_params;
	static const u8 rates[8] = { 12, 18, 24, 36, 48, 72, 96, 108 };
	unsigned int i, j;

	memset(caps, 0, sizeof(*caps));
	caps->phy = 0x7a;
	caps->nss = 1;
	for (i = 0; i < link->supported_rates_len; i++)
		for (j = 0; j < ARRAY_SIZE(rates); j++)
			if ((link->supported_rates[i] & 0x7f) == rates[j])
				caps->rates |= BIT(6 + j);
	if (!caps->rates)
		caps->rates = 0x3fc0;
	if (link->ht_capa) {
		caps->ht_cap = le16_to_cpu(link->ht_capa->cap_info);
		caps->ampdu = link->ht_capa->ampdu_params_info;
		memcpy(caps->rx_mcs, link->ht_capa->mcs.rx_mask, sizeof(caps->rx_mcs));
		caps->ht_mcs = caps->rx_mcs[0];
		caps->mcs32 = !!(caps->rx_mcs[4] & 1);
		caps->rx_highest = le16_to_cpu(link->ht_capa->mcs.rx_highest);
		caps->nss = caps->rx_mcs[1] ? 2 : 1;
	}
	if (link->vht_capa) {
		caps->vht_cap = le32_to_cpu(link->vht_capa->vht_cap_info);
		caps->vht_rx_mcs = le16_to_cpu(link->vht_capa->supp_mcs.rx_mcs_map);
		caps->vht_rx_highest = le16_to_cpu(link->vht_capa->supp_mcs.rx_highest);
		caps->vht_tx_mcs = le16_to_cpu(link->vht_capa->supp_mcs.tx_mcs_map);
		caps->vht_tx_highest = le16_to_cpu(link->vht_capa->supp_mcs.tx_highest);
	}
}

int mt_awdl_add_station(struct wiphy *wiphy, struct wireless_dev *wdev,
			const u8 *mac, struct station_parameters *params)
{
	struct mt7932 *m = mt_from_wiphy(wiphy);
	struct mt7932_awdl_peer *peer = NULL;
	u8 queue[32], sta[200];
	unsigned long flags;
	int station, wtbl, ret = 0;
	unsigned int i;

	if (wdev != &m->awdl.wdev || !is_valid_ether_addr(mac))
		return -ENODEV;
	/* The clean tables do not identify the byte mapping of HE capabilities. */
	if (params->link_sta_params.he_capa)
		return -EOPNOTSUPP;
	mutex_lock(&m->command_mutex);
	if (!m->awdl.active) {
		ret = -ENETDOWN;
		goto out;
	}
	if (mt_awdl_find_peer(m, mac)) {
		ret = -EEXIST;
		goto out;
	}
	for (i = 0; i < ARRAY_SIZE(m->awdl.peers); i++)
		if (!is_valid_ether_addr(m->awdl.peers[i].addr)) {
			peer = &m->awdl.peers[i];
			break;
		}
	if (!peer) {
		ret = -ENOSPC;
		goto out;
	}
	station = mt_awdl_alloc_id(&m->station_alloc, MT7932_AWDL_STA_SLOTS);
	if (station < 0) {
		ret = station;
		goto out;
	}
	wtbl = mt_awdl_alloc_id(&m->wtbl_alloc, MT7932_AWDL_WTBL_SLOTS);
	if (wtbl < 0) {
		__clear_bit(station, &m->station_alloc);
		ret = wtbl;
		goto out;
	}
	ether_addr_copy(peer->addr, mac);
	peer->station = station;
	peer->wtbl = wtbl;
	mt_awdl_caps(&peer->caps, params);
	mt7932_awdl_build_queue(queue, station, true, true);
	ret = mt_request(m, 0x6b, true, true, true, queue, sizeof(queue));
	if (!ret) {
		mt7932_awdl_build_sta(sta, station, wtbl, mac, &peer->caps, true, true);
		ret = mt_request(m, 0x13, true, true, true, sta, sizeof(sta));
	}
	if (ret) {
		mt7932_awdl_build_queue(queue, station, true, false);
		mt_request(m, 0x6b, true, true, true, queue, sizeof(queue));
		mt_awdl_release_peer(m, peer);
	} else {
		spin_lock_irqsave(&m->response_lock, flags);
		peer->live = true;
		spin_unlock_irqrestore(&m->response_lock, flags);
	}
out:
	mutex_unlock(&m->command_mutex);
	return ret;
}

static int mt_awdl_remove_peer(struct mt7932 *m, struct mt7932_awdl_peer *peer)
{
	u8 queue[32], sta[200];
	unsigned long flags;
	int ret;

	spin_lock_irqsave(&m->response_lock, flags);
	peer->live = false;
	spin_unlock_irqrestore(&m->response_lock, flags);
	mt7932_awdl_build_sta(sta, peer->station, peer->wtbl, peer->addr,
			       &peer->caps, false, true);
	ret = mt_request(m, 0x13, true, true, true, sta, sizeof(sta));
	mt7932_awdl_build_queue(queue, peer->station, true, false);
	ret = ret ?: mt_request(m, 0x6b, true, true, true, queue, sizeof(queue));
	if (!ret)
		mt_awdl_release_peer(m, peer);
	return ret;
}

int mt_awdl_del_station(struct wiphy *wiphy, struct wireless_dev *wdev,
			struct station_del_parameters *params)
{
	struct mt7932 *m = mt_from_wiphy(wiphy);
	unsigned int i;
	int ret = 0;

	if (wdev != &m->awdl.wdev)
		return -ENODEV;
	mutex_lock(&m->command_mutex);
	for (i = 0; i < ARRAY_SIZE(m->awdl.peers); i++) {
		struct mt7932_awdl_peer *peer = &m->awdl.peers[i];

		if (!is_valid_ether_addr(peer->addr) ||
		    (params->mac && !ether_addr_equal(params->mac, peer->addr)))
			continue;
		ret = mt_awdl_remove_peer(m, peer);
		if (ret || params->mac)
			break;
	}
	mutex_unlock(&m->command_mutex);
	return ret;
}

int mt_awdl_change_station(struct wiphy *wiphy, struct wireless_dev *wdev,
			   const u8 *mac, struct station_parameters *params)
{
	struct mt7932 *m = mt_from_wiphy(wiphy);
	struct mt7932_awdl_peer_caps caps;
	struct mt7932_awdl_peer *peer;
	u8 sta[MT7932_AWDL_STA_RECORD_LEN];
	unsigned long flags;
	int ret = 0;

	if (wdev != &m->awdl.wdev || !is_valid_ether_addr(mac))
		return -ENODEV;
	if (params->link_sta_params.he_capa)
		return -EOPNOTSUPP;
	mt_awdl_caps(&caps, params);
	mutex_lock(&m->command_mutex);
	peer = mt_awdl_find_peer(m, mac);
	if (!peer) {
		ret = -ENOENT;
		goto out;
	}
	if (!memcmp(&caps, &peer->caps, sizeof(caps)))
		goto out;
	mt7932_awdl_build_sta(sta, peer->station, peer->wtbl, peer->addr,
			       &caps, true, true);
	ret = mt_request(m, 0x13, true, true, true, sta, sizeof(sta));
	if (!ret) {
		spin_lock_irqsave(&m->response_lock, flags);
		peer->caps = caps;
		spin_unlock_irqrestore(&m->response_lock, flags);
	}
out:
	mutex_unlock(&m->command_mutex);
	return ret;
}

static int mt_awdl_clear_template(struct mt7932 *m, u32 tag)
{
	u8 body[8] = {};

	put_unaligned_le32(1, body);
	return mt_awdl_send_tlv(m, tag, body, sizeof(body));
}

int mt_awdl_activate(struct mt7932 *m)
{
	u8 queue[32], bss[116], active[12], tag_bssid[10];
	int bmc, wmm, ret;

	if (m->awdl.active || !m->awdl.interface_up)
		return 0;
	bmc = mt_awdl_alloc_id(&m->wtbl_alloc, MT7932_AWDL_WTBL_SLOTS);
	if (bmc < 0)
		return bmc;
	m->awdl.bmc_wtbl = bmc;
	wmm = mt_awdl_alloc_id(&m->wmm_alloc, 2);
	if (wmm < 0) {
		__clear_bit(bmc, &m->wtbl_alloc);
		return wmm;
	}
	m->awdl.wmm = wmm;
	mt7932_awdl_build_queue(queue, 0, false, true);
	ret = mt_request(m, 0x6b, true, true, true, queue, sizeof(queue));
	if (!ret) {
		mt7932_awdl_build_bss(bss, bmc, wmm, 0x7a,
				      clamp_t(u8, m->phy_cap[4], 1, 2));
		ret = mt_request(m, 0x12, true, true, true, bss, sizeof(bss));
	}
	if (!ret) {
		mt7932_awdl_build_bss_active(active, m->awdl.netdev->dev_addr, bmc, true);
		ret = mt_request(m, 0x11, true, true, true, active, sizeof(active));
	}
	ret = ret ?: mt_awdl_clear_template(m, 0x04);
	ret = ret ?: mt_awdl_clear_template(m, 0x05);
	if (ret)
		goto rollback;
	m->awdl.active = true;
	put_unaligned_le32(1, tag_bssid);
	ether_addr_copy(tag_bssid + 4, m->awdl.bssid);
	ret = mt_awdl_send_tlv(m, 0x00, tag_bssid, sizeof(tag_bssid));
	ret = ret ?: mt_awdl_configure_sync(m);
	if (!ret)
		return 0;
	/* A rejected optional sync policy must leave a safely created, disabled BSS. */
	if (!m->awdl.sync_enabled && ret == -EINVAL) {
		dev_warn(&m->pdev->dev,
			 "AWDL sync remains disabled: complete schedule/templates required\n");
		return 0;
	}
rollback:
	mt7932_awdl_build_bss_active(active, m->awdl.netdev->dev_addr, bmc, false);
	mt_request(m, 0x11, true, true, true, active, sizeof(active));
	mt7932_awdl_build_queue(queue, 0, false, false);
	mt_request(m, 0x6b, true, true, true, queue, sizeof(queue));
	__clear_bit(bmc, &m->wtbl_alloc);
	__clear_bit(wmm, &m->wmm_alloc);
	m->awdl.active = false;
	return ret;
}

static int mt_awdl_deactivate(struct mt7932 *m)
{
	u8 body[116], active[12], queue[32], enable[6] = {};
	unsigned int i;
	int ret = 0;

	if (!m->awdl.active)
		return 0;
	netif_carrier_off(m->awdl.netdev);
	netif_stop_queue(m->awdl.netdev);
	for (i = 0; i < ARRAY_SIZE(m->awdl.peers); i++)
		if (is_valid_ether_addr(m->awdl.peers[i].addr)) {
			ret = mt_awdl_remove_peer(m, &m->awdl.peers[i]);
			if (ret)
				return ret;
		}
	put_unaligned_le32(1, enable);
	ret = mt_awdl_send_tlv(m, 0x08, enable, sizeof(enable));
	mt7932_awdl_build_bss_active(active, m->awdl.netdev->dev_addr,
				       m->awdl.bmc_wtbl, false);
	ret = ret ?: mt_request(m, 0x11, true, true, true, active, sizeof(active));
	mt7932_awdl_build_bss(body, m->awdl.bmc_wtbl, m->awdl.wmm, 0x7a,
			       clamp_t(u8, m->phy_cap[4], 1, 2));
	body[1] = 0;
	ret = ret ?: mt_request(m, 0x12, true, true, true, body, sizeof(body));
	mt7932_awdl_build_queue(queue, 0, false, false);
	ret = ret ?: mt_request(m, 0x6b, true, true, true, queue, sizeof(queue));
	if (!ret) {
		__clear_bit(m->awdl.bmc_wtbl, &m->wtbl_alloc);
		__clear_bit(m->awdl.wmm, &m->wmm_alloc);
		m->awdl.active = false;
		m->awdl.sync_enabled = false;
		m->awdl.slot_open = false;
	}
	return ret;
}

static int mt_awdl_net_open(struct net_device *netdev)
{
	struct mt7932 *m = *(struct mt7932 **)netdev_priv(netdev);
	int ret = 0;

	mutex_lock(&m->command_mutex);
	if (m->stopping) {
		ret = -ESHUTDOWN;
	} else if (m->link_failed || m->cal_state.error) {
		ret = m->cal_state.error ?: -EIO;
	} else {
		m->awdl.interface_up = true;
		if (m->rf_ready)
			ret = mt_awdl_activate(m);
		if (ret)
			m->awdl.interface_up = false;
	}
	mutex_unlock(&m->command_mutex);
	netif_carrier_off(netdev);
	netif_stop_queue(netdev);
	return ret;
}

static int mt_awdl_net_stop(struct net_device *netdev)
{
	struct mt7932 *m = *(struct mt7932 **)netdev_priv(netdev);

	m->awdl.interface_up = false;
	if (!m->stopping) {
		mutex_lock(&m->command_mutex);
		mt_awdl_deactivate(m);
		mutex_unlock(&m->command_mutex);
	}
	netif_carrier_off(netdev);
	netif_stop_queue(netdev);
	return 0;
}

static netdev_tx_t mt_awdl_xmit(struct sk_buff *skb, struct net_device *netdev)
{
	struct mt7932 *m = *(struct mt7932 **)netdev_priv(netdev);
	struct mt7932_ring *q = &m->tx[0];
	struct mt7932_desc *desc;
	unsigned long flags;
	u8 *payload, *header;
	dma_addr_t dma;
	unsigned int slot, wtbl;
	int length, ret;

	if (!m->awdl.active || !m->awdl.sync_enabled || !m->awdl.slot_open ||
	    !m->data_ready || skb->len < ETH_HLEN || skb->len > netdev->mtu + ETH_HLEN)
		goto drop;
	spin_lock_irqsave(&m->response_lock, flags);
	if (is_multicast_ether_addr(skb->data)) {
		wtbl = m->awdl.bmc_wtbl;
	} else {
		struct mt7932_awdl_peer *peer = mt_awdl_find_peer(m, skb->data);

		if (!peer) {
			spin_unlock_irqrestore(&m->response_lock, flags);
			goto drop;
		}
		wtbl = peer->wtbl;
	}
	spin_unlock_irqrestore(&m->response_lock, flags);
	spin_lock_irqsave(&m->data_lock, flags);
	mt_data_clean_locked(m);
	slot = q->head;
	if (!m->awdl.slot_open || m->data_live[slot] || q->queued >= q->count - 1) {
		netif_stop_queue(netdev);
		mt_data_kick_locked(m);
		spin_unlock_irqrestore(&m->data_lock, flags);
		return NETDEV_TX_BUSY;
	}
	payload = m->data_payloads + 4096 * slot;
	header = m->data_headers + 64 * slot;
	length = mt7932_awdl_encap(payload, 4096, skb->data, skb->len,
				    m->awdl.tx_sequence++);
	ret = length < 0 ? length :
		mt7932_data_header(header, payload, length,
				   m->data_payloads_dma + 4096 * slot, slot,
				   wtbl, 0, 0, false);
	if (ret) {
		spin_unlock_irqrestore(&m->data_lock, flags);
		goto drop;
	}
	dma = m->data_headers_dma + 64 * slot;
	desc = &q->desc[slot];
	desc->address = cpu_to_le32(lower_32_bits(dma));
	desc->second = 0;
	desc->info = cpu_to_le32(upper_32_bits(dma));
	m->data_live[slot] = m->data_token[slot] = true;
	m->data_retired[slot] = false;
	dma_wmb();
	desc->control = cpu_to_le32((64 << 16) | BIT(30));
	q->head = (slot + 1) % q->count;
	q->queued++;
	dma_wmb();
	m->data_unkicked++;
	if (!netdev_xmit_more() || q->queued >= q->count - 1)
		mt_data_kick_locked(m);
	spin_unlock_irqrestore(&m->data_lock, flags);
	netdev->stats.tx_packets++;
	netdev->stats.tx_bytes += skb->len;
	dev_kfree_skb_any(skb);
	return NETDEV_TX_OK;
drop:
	if (!netdev_xmit_more() && READ_ONCE(m->data_ready)) {
		spin_lock_irqsave(&m->data_lock, flags);
		mt_data_kick_locked(m);
		spin_unlock_irqrestore(&m->data_lock, flags);
	}
	netdev->stats.tx_dropped++;
	dev_kfree_skb_any(skb);
	return NETDEV_TX_OK;
}

static const struct net_device_ops mt_awdl_net_ops = {
	.ndo_open = mt_awdl_net_open,
	.ndo_stop = mt_awdl_net_stop,
	.ndo_start_xmit = mt_awdl_xmit,
	.ndo_set_mac_address = eth_mac_addr,
};

void mt_awdl_wiphy_setup(struct mt7932 *m)
{
	static const struct ieee80211_iface_limit limits[] = {
		{ .max = 2, .types = BIT(NL80211_IFTYPE_STATION) },
	};
	static const struct ieee80211_iface_combination combinations[] = {
		{
			.limits = limits,
			.n_limits = ARRAY_SIZE(limits),
			.max_interfaces = 2,
			.num_different_channels = 2,
		},
	};

	m->wiphy->iface_combinations = combinations;
	m->wiphy->n_iface_combinations = ARRAY_SIZE(combinations);
}

int mt_awdl_register(struct mt7932 *m)
{
	struct net_device *netdev;
	u8 mac[ETH_ALEN];
	int ret;

	if (!mac_pton(awdl_bssid, m->awdl.bssid))
		return -EINVAL;
	netdev = alloc_etherdev(sizeof(m));
	if (!netdev)
		return -ENOMEM;
	*(struct mt7932 **)netdev_priv(netdev) = m;
	m->awdl.wdev.wiphy = m->wiphy;
	/*
	 * cfg80211 has no AWDL type and rejects a wiphy that advertises NAN
	 * without the NAN operations, so awdl0 is a station-typed wdev, as in
	 * brcmfmac; the firmware BSS 1 records define its behaviour.
	 */
	m->awdl.wdev.iftype = NL80211_IFTYPE_STATION;
	m->awdl.wdev.netdev = netdev;
	netdev->ieee80211_ptr = &m->awdl.wdev;
	netdev->netdev_ops = &mt_awdl_net_ops;
	netdev->min_mtu = ETH_MIN_MTU;
	netdev->max_mtu = ETH_DATA_LEN - MT7932_AWDL_ENCAP_LEN;
	netdev->needed_headroom = MT7932_AWDL_ENCAP_LEN;
	SET_NETDEV_DEV(netdev, &m->pdev->dev);
	ether_addr_copy(mac, m->mac);
	mac[0] |= 0x02;
	mac[5] ^= 0x01;
	eth_hw_addr_set(netdev, mac);
	ether_addr_copy(netdev->perm_addr, mac);
	strscpy(netdev->name, "awdl0", IFNAMSIZ);
	netif_carrier_off(netdev);
	m->awdl.netdev = netdev;
	ret = register_netdev(netdev);
	if (ret) {
		m->awdl.netdev = NULL;
		free_netdev(netdev);
		return ret;
	}
	m->awdl.registered = true;
	return 0;
}

void mt_awdl_unregister(struct mt7932 *m)
{
	if (!m->awdl.netdev)
		return;
	if (m->awdl.registered)
		unregister_netdev(m->awdl.netdev);
	free_netdev(m->awdl.netdev);
	m->awdl.netdev = NULL;
	m->awdl.registered = false;
}

void mt_awdl_receive(struct mt7932 *m, const struct mt7932_rx_frame *frame)
{
	struct sk_buff *skb;
	unsigned long flags;
	unsigned int i;
	bool owned = false;
	size_t length;

	if (!m->awdl.active || frame->bss != 1)
		return;
	if (!frame->translated) {
		if (frame->length < 28 || (frame->frame_control & 0xfc) != IEEE80211_STYPE_ACTION ||
		    frame->data[24] != 0x7f || memcmp(frame->data + 25, "\x00\x17\xf2", 3) ||
		    !ether_addr_equal(frame->data + 16, m->awdl.bssid))
			return;
		m->awdl.rx_actions++;
		cfg80211_rx_mgmt(&m->awdl.wdev,
			ieee80211_channel_to_frequency(frame->channel, frame->band),
			mt7932_rx_signal(frame, m->antenna_mask), frame->data,
			frame->length, 0);
		return;
	}
	spin_lock_irqsave(&m->response_lock, flags);
	if (!frame->unicast) {
		owned = frame->wtbl == m->awdl.bmc_wtbl;
	} else {
		for (i = 0; i < ARRAY_SIZE(m->awdl.peers); i++)
			if (m->awdl.peers[i].live &&
			    m->awdl.peers[i].wtbl == frame->wtbl) {
				owned = true;
				break;
			}
	}
	spin_unlock_irqrestore(&m->response_lock, flags);
	if (!owned) {
		m->awdl.netdev->stats.rx_dropped++;
		return;
	}
	skb = netdev_alloc_skb_ip_align(m->awdl.netdev, frame->length);
	if (!skb) {
		m->awdl.netdev->stats.rx_dropped++;
		return;
	}
	skb_put_data(skb, frame->data, frame->length);
	length = skb->len;
	if (mt7932_awdl_decap(skb->data, &length)) {
		m->awdl.netdev->stats.rx_dropped++;
		dev_kfree_skb_any(skb);
		return;
	}
	skb_trim(skb, length);
	skb->protocol = eth_type_trans(skb, m->awdl.netdev);
	skb->ip_summed = CHECKSUM_NONE;
	m->awdl.netdev->stats.rx_packets++;
	m->awdl.netdev->stats.rx_bytes += length;
	m->awdl.rx_data++;
	netif_rx(skb);
}

void mt_awdl_event(struct mt7932 *m, const struct mt7932_event *event)
{
	struct mt7932_awdl_event_info info;

	if (mt7932_awdl_event_parse(event, &info))
		return;
	if (info.subtype < ARRAY_SIZE(m->awdl.events))
		m->awdl.events[info.subtype]++;
	if (!m->awdl.netdev || !m->awdl.active || !m->awdl.sync_enabled)
		return;
	switch (info.subtype) {
	case 0: /* availability slot start */
		m->awdl.slot_open = true;
		netif_carrier_on(m->awdl.netdev);
		netif_wake_queue(m->awdl.netdev);
		break;
	case 1: /* availability slot end */
		m->awdl.slot_open = false;
		netif_stop_queue(m->awdl.netdev);
		break;
	case 2:
		/* Intentionally not decoded: its static layout conflicts with the
		 * proven normal-RX action report path.
		 */
		break;
	case 3:
	case 4:
		break;
	case 5: /* action-frame TX status */
	case 6: /* sync changed; this host ignores the payload */
	case 7: /* channel switch */
	case 8: /* channel switch complete */
	case 10: /* diagnostic trigger; accounted only */
		break;
	case 9: /* conservative suspend/resume handling without payload layout */
		m->awdl.slot_open = false;
		netif_stop_queue(m->awdl.netdev);
		netif_carrier_off(m->awdl.netdev);
		break;
	default:
		break;
	}
}

