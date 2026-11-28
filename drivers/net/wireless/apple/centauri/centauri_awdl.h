/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef AURORA_CENTAURI_AWDL_H
#define AURORA_CENTAURI_AWDL_H

#include <linux/if_ether.h>
#include <linux/netdevice.h>
#include <linux/types.h>

struct centauri_wifi;
struct centauri_awdl;

/* Hardware interface IDs in ACI command headers, RX footers, and TX clientdata. */
#define CEN_IF_STA			0
#define CEN_IF_AWDL			1

/* ACI control command opcodes for interface 1 (AWDL). */
#define CEN_AWDL_CMD_CONFIG_MAC		0x00000001U
#define CEN_AWDL_CMD_START		0x00000002U
#define CEN_AWDL_CMD_STOP		0x00000003U
#define CEN_AWDL_CMD_GET_DEFAULTS	0x00000008U
#define CEN_AWDL_CMD_SET_PARAMS		0x0000000cU
#define CEN_AWDL_CMD_GET_PARAMS		0x0000000dU
#define CEN_AWDL_CMD_PEER_ADD		0x00050000U
#define CEN_AWDL_CMD_PEER_DEL		0x00050001U

/* ACI management events delivered on interface 1 (low 24 bits). */
#define CEN_AWDL_EV_LINK_START		0x000000U
#define CEN_AWDL_EV_SYNC_STATE		0x00001cU
#define CEN_AWDL_EV_ELECTION_STATE	0x00001dU
#define CEN_AWDL_EV_PEER_OP_DONE	0x00002bU
#define CEN_AWDL_EV_PEER_STATE		0x00002cU

/* AWDL configuration TLV tags (u16 tag, u16 length, value). */
#define CEN_AWDL_TLV_EXTRA_ELECTION	0xfe00U
#define CEN_AWDL_TLV_MIF_SYNC		0xfe01U
#define CEN_AWDL_TLV_PSF_EXTRA		0xfe02U
#define CEN_AWDL_TLV_MIF_EXTRA		0xfe03U
#define CEN_AWDL_TLV_GENERAL_CFG	0xfe08U
#define CEN_AWDL_TLV_FE0A		0xfe0aU
#define CEN_AWDL_TLV_CAPS		0xfe09U
#define CEN_AWDL_TLV_SYNC_PARAMS	0xff04U
#define CEN_AWDL_TLV_ELECTION		0xff05U
#define CEN_AWDL_TLV_CHANSEQ		0xff12U
/* Raw VHT capabilities element reported in the command 8 defaults. */
#define CEN_AWDL_TLV_VHT_IE		0x00bfU

#define CEN_AWDL_CHANSEQ_SLOTS		16
#define CEN_AWDL_EXTRA_ELECTION_LEN	44
#define CEN_AWDL_SYNC_PARAMS_MAX	72
#define CEN_AWDL_EXTRA_TLVS_MAX		384
#define CEN_AWDL_VHT_IE_MAX		16U
#define CEN_AWDL_SVC_TLVS_MAX		220U
#define CEN_AWDL_ARPA_MAX		63U
#define CEN_AWDL_PENDING_PEERS_MAX	4U
#define CEN_AWDL_RECENT_PEERS		8U
/* A peer given to the firmware is not given again for this long. */
#define CEN_AWDL_PEER_RETRY		(5 * HZ)
/* A peer TX ring used this lately is not handed to another peer. */
#define CEN_AWDL_RING_HOLD		(5 * HZ)

/* Over-the-air AWDL action-frame TLV types (u8 type, le16 len, value). */
#define CEN_AWDL_OTA_TLV_SYNC_PARAMS		0x04U
#define CEN_AWDL_OTA_TLV_ELECTION		0x05U
#define CEN_AWDL_OTA_TLV_DATA_PATH_STATE	0x0cU
#define CEN_AWDL_OTA_TLV_ARPA			0x10U
#define CEN_AWDL_OTA_TLV_VERSION		0x15U
#define CEN_AWDL_OTA_TLV_ELECTION_V2		0x18U

#define CEN_AWDL_OTA_TLV_SERVICE_PARAMS	0x06U
#define CEN_AWDL_OTA_TLV_HT_CAPS		0x07U
#define CEN_AWDL_OTA_TLV_IE_CONTAINER		0x11U
#define CEN_AWDL_OTA_VERSION_BYTE		0xa2U
#define CEN_AWDL_OTA_DP_STATE_LEN		47U

/*
 * Synchronization role in the extra-election TLV (0xfe00) byte 9: 2 is the
 * firmware's own default and what traced peers program both as top master
 * and while following a parent.
 */
#define CEN_AWDL_SYNC_ROLE_ELECTION		2U

/* Extra-election TLV (0xfe00) field offsets. */
#define CEN_AWDL_XEL_SYNC_ON			8	/* u8, 1 = synchronize */
#define CEN_AWDL_XEL_ROLE			9	/* u8, sync role */
#define CEN_AWDL_XEL_SYNC_ADDR			10	/* master to sync to */
#define CEN_AWDL_XEL_ADV_VALID			16	/* 1 = apply the parent timing */
#define CEN_AWDL_XEL_ADV_RX_TIME		17	/* le32, parent frame RX time */
#define CEN_AWDL_XEL_ADV_TX_DELAY		21	/* le32, parent TX delay */
#define CEN_AWDL_XEL_ADV_DOWN_COUNTER		25	/* le16 */
#define CEN_AWDL_XEL_ADV_AW_PERIOD		27	/* le16 */
#define CEN_AWDL_XEL_ADV_AW_LENGTH		29	/* le16 */
#define CEN_AWDL_XEL_ADV_PRESENCE		31	/* u8 */
#define CEN_AWDL_XEL_ADV_AW_SEQ			32	/* le16 */

/* Extension length every synchronizing peer advertises. */
#define CEN_AWDL_AW_EXT_LENGTH			16U

/* ACIPC transfer ring close control message opcode (4-byte message). */
#define CEN_ACIPC_MSG_TR_CLOSE			0x0003U

/* Host LLC/SNAP + AWDL data framing header prepended when fw_llc_encap == 0.
 * Wire layout after dst[6] + src[6]:
 *   be16 length_8023 (16 + payload_len)
 *   u8   llc_snap[8] = { aa, aa, 03, 00, 17, f2, 08, 00 }
 *   le16 awdl_data_hdr = 0x0403 (type 3, version 4)
 *   le16 seq (12-bit sequence counter)
 *   le16 pad = 0x0000
 *   be16 ethertype (original Ethernet II protocol)
 *   u8   payload[...]
 */
#define CEN_AWDL_DATA_HDR_LEN		16U
#define CEN_AWDL_RFC1042_HDR_LEN	8U
#define CEN_AWDL_DATA_HDR_MAGIC		0x0403U
#define CEN_AWDL_TX_PROTO_CLASS		6ULL

/* ACIPC TX transfer rings dedicated to AWDL (completing on shared CR 76 / vector 192). */
#define CEN_AWDL_MCBC_TR_ID		26U
#define CEN_AWDL_PEER_TR_BASE		27U
/* One unicast ring: opening a second has faulted the firmware. */
#define CEN_AWDL_MAX_PEER_RINGS		1U
#define CEN_AWDL_TOTAL_TX_RINGS		(1U + CEN_AWDL_MAX_PEER_RINGS)
#define CEN_AWDL_TX_SLOTS		64U
#define CEN_AWDL_TX_QUEUE_LIMIT		64U

/* Non-overlapping DMA offsets inside the 16 MiB Alpha window. */
#define CEN_AWDL_TR_BASE		0x560000U
#define CEN_AWDL_TR_STRIDE		0x20000U
#define CEN_AWDL_TX_BUF_BASE		0xd40000U
#define CEN_AWDL_TX_BUF_SIZE		2048U
#define CEN_AWDL_TX_RING_BUF_SPAN	(CEN_AWDL_TX_SLOTS * CEN_AWDL_TX_BUF_SIZE)

struct cen_awdl_tlv_hdr {
	__le16 tag;
	__le16 length;
} __packed;
static_assert(sizeof(struct cen_awdl_tlv_hdr) == 4);

struct cen_awdl_general_cfg {
	u8 fw_llc_encap;
	u8 virtual_if_disable;
	u8 reserved[2];
} __packed;
static_assert(sizeof(struct cen_awdl_general_cfg) == 4);

/*
 * Election parameters (0xff05) use the over-the-air election TLV layout:
 * 19 bytes, then one byte of padding to keep the next TLV aligned.
 */
struct cen_awdl_election_params {
	u8 flags;
	__le16 election_id;
	u8 distance_from_top;
	u8 reserved;
	u8 top_master[ETH_ALEN];
	__le32 top_master_metric;
	__le32 self_metric;
	u8 pad;
} __packed;
static_assert(sizeof(struct cen_awdl_election_params) == 20);

struct cen_awdl_mif_sync_params {
	u8 enable_mif;
	u8 enable_psf;
	__le16 base_mif_offset;
	u8 mif_multiplier;
	u8 max_psf_lifetime;
	u8 bssid[ETH_ALEN];
	__le16 psf_period_master;
	__le16 psf_period_non_master;
	u8 reserved[4];
} __packed;
static_assert(sizeof(struct cen_awdl_mif_sync_params) == 20);

struct cen_awdl_chanseq {
	u8 count_minus_1;
	u8 encoding;
	u8 dup_count;
	u8 step_count;
	__le16 fill_channel;
	__le16 channels[CEN_AWDL_CHANSEQ_SLOTS];
	__le16 reserved;
} __packed;
static_assert(sizeof(struct cen_awdl_chanseq) == 40);

struct cen_awdl_peer_add_req {
	u8 mac[ETH_ALEN];
	struct cen_awdl_chanseq chanseq;
	u8 flags[26];
} __packed;
static_assert(sizeof(struct cen_awdl_peer_add_req) == 72);

/* 10-byte ACIPC clientdata appended to data TX transfer ring open messages. */
struct cen_awdl_tx_clientdata {
	u8 version;
	u8 if_id;
	u8 tid;
	u8 ac;
	u8 dst_addr[ETH_ALEN];
} __packed;
static_assert(sizeof(struct cen_awdl_tx_clientdata) == 10);

/* RX completion footer interface ID extraction (bits 10:8). */
static inline u8 cen_rx_footer_if_id(u32 footer)
{
	return (footer >> 8) & 0x7;
}

/* Build the 4-byte ACIPC transfer-ring close control message. */
static inline void cen_awdl_build_tr_close_msg(u8 msg[4], u16 tr_id)
{
	msg[0] = CEN_ACIPC_MSG_TR_CLOSE & 0xff;
	msg[1] = CEN_ACIPC_MSG_TR_CLOSE >> 8;
	msg[2] = tr_id & 0xff;
	msg[3] = tr_id >> 8;
}

/* Parsed fields from a received AWDL PSF/MIF vendor action frame. */
struct cen_awdl_rx_peer_info {
	u8 mac[ETH_ALEN];
	u8 subtype;
	u8 master_channel;
	bool has_election;
	u8 distance_from_top;
	u8 top_master[ETH_ALEN];
	u32 top_master_metric;
	u32 self_metric;
	char arpa[CEN_AWDL_ARPA_MAX + 1];
	/* Sync parameters TLV and action frame header timing. */
	bool has_sync;
	u16 down_counter;
	u16 aw_period;
	u16 aw_ext_length;
	u16 aw_length;
	u8 presence;
	u16 aw_seq;
	u32 tx_delay;
};

/* Radiotap fields of a received management frame used for AWDL. */
struct cen_awdl_rtap_info {
	u16 len;
	u32 present;
	bool has_tsft;
	bool has_timestamp;
	u32 tsft;
	u32 timestamp;
};

/* A parent to synchronize to, snapshotted from one of its frames. */
struct cen_awdl_parent {
	u8 addr[ETH_ALEN];
	u8 top_master[ETH_ALEN];
	u32 top_metric;
	u8 distance;
	u8 channel;
	u32 rx_time;
	u32 tx_delay;
	u16 down_counter;
	u16 aw_period;
	u16 aw_length;
	u8 presence;
	u16 aw_seq;
};

/* A peer the firmware was given lately, so it is not given again at once. */
struct cen_awdl_recent_peer {
	u8 mac[ETH_ALEN];
	unsigned long added;
};

/* Host/transport helpers exposed by centauri_wifi.c (called with wifi->lock held). */
struct mutex *centauri_wifi_lock_ptr(struct centauri_wifi *wifi);
struct device *centauri_wifi_dev(struct centauri_wifi *wifi);
struct dentry *centauri_wifi_debugfs(struct centauri_wifi *wifi);
struct cen_alpha_resources *centauri_wifi_alpha(struct centauri_wifi *wifi);
bool centauri_wifi_is_stopping(struct centauri_wifi *wifi);
bool centauri_wifi_is_paused(struct centauri_wifi *wifi);
void centauri_wifi_get_country(const struct centauri_wifi *wifi, u8 out[3]);
unsigned int centauri_wifi_station_channel(const struct centauri_wifi *wifi);
void centauri_wifi_schedule_poll(struct centauri_wifi *wifi);
int centauri_wifi_cmd_iface(struct centauri_wifi *wifi, u8 if_id, u32 id,
			    const void *payload, size_t size,
			    void *reply, size_t *reply_size);
int centauri_wifi_ensure_shared_data_rings(struct centauri_wifi *wifi);
int centauri_wifi_open_awdl_tx_ring(struct centauri_wifi *wifi, u16 tr_id,
				    u32 ring_offset, u16 slots,
				    const struct cen_awdl_tx_clientdata *cd);
int centauri_wifi_close_awdl_tx_ring(struct centauri_wifi *wifi, u16 tr_id);
u16 centauri_wifi_index_read(struct centauri_wifi *wifi, unsigned int array,
			     unsigned int index);
void centauri_wifi_index_write(struct centauri_wifi *wifi, unsigned int array,
			       unsigned int index, u16 value);
bool centauri_wifi_tx_completion_span(u16 cookie, u16 wire_count, u8 flags,
				      u16 ring_slots, unsigned int *first,
				      u16 *count);

/* AWDL lifecycle & data-path callbacks invoked by centauri_wifi.c. */
int centauri_awdl_register(struct centauri_wifi *wifi, struct centauri_awdl **out);
void centauri_awdl_unregister(struct centauri_awdl *awdl);
void centauri_awdl_suspend(struct centauri_awdl *awdl);
bool centauri_awdl_is_active(const struct centauri_awdl *awdl);
void centauri_awdl_rx_frame(struct centauri_awdl *awdl, const u8 *frame,
			    unsigned int length);
void centauri_awdl_rx_mgmt(struct centauri_awdl *awdl, const u8 *payload,
			   size_t size);
bool centauri_awdl_tx_complete(struct centauri_awdl *awdl, u16 ring,
			       u16 cookie, u16 wire_count, u8 flags, u8 status);
void centauri_awdl_drain_tx(struct centauri_awdl *awdl);
void centauri_awdl_handle_event(struct centauri_awdl *awdl, u32 event_id,
				u16 status, const u8 *packet, u16 size);

#endif
