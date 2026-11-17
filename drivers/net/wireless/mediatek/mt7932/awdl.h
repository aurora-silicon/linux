/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef MT7932_AWDL_H
#define MT7932_AWDL_H

#include <linux/if_ether.h>
#include <linux/types.h>
#include <net/cfg80211.h>

#define MT7932_AWDL_BSS_INDEX		1
/* The managed interface owns own-MAC slot 0 for its entire lifetime. */
#define MT7932_AWDL_OWN_MAC_INDEX		1
#define MT7932_AWDL_NETWORK_TYPE	5
#define MT7932_AWDL_BSS_RECORD_LEN	116
#define MT7932_AWDL_STA_RECORD_LEN	200
#define MT7932_AWDL_QUEUE_RECORD_LEN	32
#define MT7932_AWDL_ENCAP_LEN		16
#define MT7932_AWDL_MAX_PEERS		15
#define MT7932_AWDL_MAX_SCHEDULE	64

struct mt7932;
struct mt7932_event;
struct mt7932_rx_frame;

struct mt7932_awdl_peer_caps {
	u8 phy;
	u16 rates;
	u8 ht_mcs;
	u8 mcs32;
	u8 rx_mcs[10];
	u16 rx_highest;
	u16 ht_cap;
	u16 ht_ext_cap;
	u32 tx_bf_cap;
	u8 ampdu;
	u8 asel;
	u32 vht_cap;
	u16 vht_rx_mcs;
	u16 vht_rx_highest;
	u16 vht_tx_mcs;
	u16 vht_tx_highest;
	u8 he[34];
	u8 nss;
	u8 rcpi;
};

struct mt7932_awdl_peer {
	u8 addr[ETH_ALEN];
	u8 station;
	u8 wtbl;
	bool live;
	struct mt7932_awdl_peer_caps caps;
};

struct mt7932_awdl_event_info {
	u32 subtype;
	const u8 *data;
	size_t length;
};

struct mt7932_awdl {
	struct net_device *netdev;
	struct wireless_dev wdev;
	struct mt7932_awdl_peer peers[MT7932_AWDL_MAX_PEERS];
	u8 bssid[ETH_ALEN];
	u8 bmc_wtbl;
	u8 wmm;
	u16 tx_sequence;
	bool registered;
	bool interface_up;
	bool active;
	bool sync_enabled;
	bool slot_open;
	u64 events[11];
	u64 rx_actions;
	u64 rx_data;
};

/* The station iftype is only a cfg80211 representation of AWDL. Never
 * route the managed station (or an unrelated wdev) into BSS 1 peer ops.
 * Caller serializes interface/firmware state with command_mutex.
 */
static inline int mt7932_awdl_station_route(const struct mt7932_awdl *awdl,
					    const struct wireless_dev *wdev)
{
	if (wdev != &awdl->wdev)
		return -EOPNOTSUPP;
	if (!awdl->interface_up || !awdl->active)
		return -ENETDOWN;
	return 0;
}

int mt7932_awdl_build_queue(u8 out[MT7932_AWDL_QUEUE_RECORD_LEN],
			    u8 station, bool peer, bool enable);
int mt7932_awdl_build_bss_active(u8 out[12], const u8 *mac, u8 bmc_wtbl,
				 bool active);
int mt7932_awdl_build_bss(u8 out[MT7932_AWDL_BSS_RECORD_LEN], u8 bmc_wtbl,
			  u8 wmm, u8 phy, u8 nss);
int mt7932_awdl_build_sta(u8 out[MT7932_AWDL_STA_RECORD_LEN], u8 station,
			  u8 wtbl, const u8 *mac,
			  const struct mt7932_awdl_peer_caps *caps,
			  bool live, bool need_response);
int mt7932_awdl_build_tlv(u8 *out, size_t capacity, u32 tag,
			  const void *body, size_t body_len);
int mt7932_awdl_build_channel_sequence(u8 out[134], const u8 *channels,
				       const u8 *classes, size_t count);
int mt7932_awdl_event_parse(const struct mt7932_event *event,
			    struct mt7932_awdl_event_info *info);
int mt7932_awdl_encap(u8 *out, size_t capacity, const u8 *ethernet,
			 size_t length, u16 sequence);
int mt7932_awdl_decap(u8 *frame, size_t *length);

#if IS_ENABLED(CONFIG_MT7932_AWDL)
void mt_awdl_wiphy_setup(struct mt7932 *m);
int mt_awdl_register(struct mt7932 *m);
void mt_awdl_unregister(struct mt7932 *m);
int mt_awdl_activate(struct mt7932 *m);
int mt_awdl_deactivate(struct mt7932 *m);
void mt_awdl_receive(struct mt7932 *m, const struct mt7932_rx_frame *frame);
void mt_awdl_event(struct mt7932 *m, const struct mt7932_event *event);
int mt_awdl_add_station(struct wiphy *wiphy, struct wireless_dev *wdev,
			const u8 *mac, struct station_parameters *params);
int mt_awdl_del_station(struct wiphy *wiphy, struct wireless_dev *wdev,
			struct station_del_parameters *params);
int mt_awdl_change_station(struct wiphy *wiphy, struct wireless_dev *wdev,
			   const u8 *mac, struct station_parameters *params);
#else
static inline void mt_awdl_wiphy_setup(struct mt7932 *m) {}
static inline int mt_awdl_register(struct mt7932 *m) { return 0; }
static inline void mt_awdl_unregister(struct mt7932 *m) {}
static inline int mt_awdl_activate(struct mt7932 *m) { return 0; }
static inline void mt_awdl_receive(struct mt7932 *m,
				   const struct mt7932_rx_frame *frame) {}
static inline void mt_awdl_event(struct mt7932 *m,
				 const struct mt7932_event *event) {}
#endif

#endif
