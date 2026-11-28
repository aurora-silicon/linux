#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Host unit tests and negative controls for Centauri native AWDL helpers."""
import argparse
import os
from pathlib import Path
import re
import resource
import shlex
import signal
import subprocess
import tempfile


def extract_fn(source, name):
    match = re.search(
        r"^(?:static )?(?:inline )?(?:int|void|bool|u8|u16|size_t) "
        + re.escape(name)
        + r"\([^;]*?\)\n\{",
        source,
        re.M,
    )
    if not match:
        raise RuntimeError(f"missing production function: {name}")
    brace = source.index("{", match.start())
    depth, end = 1, brace + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[match.start():end]


PRELUDE = r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef uint8_t u8;
typedef int8_t s8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef uint16_t __le16;
typedef uint32_t __le32;

#define __packed __attribute__((packed))
#define ETH_ALEN 6
#define ETH_HLEN 14
#define CEN_IF_STA 0
#define CEN_IF_AWDL 1
#define CEN_AWDL_TLV_EXTRA_ELECTION 0xfe00U
#define CEN_AWDL_TLV_MIF_SYNC 0xfe01U
#define CEN_AWDL_TLV_GENERAL_CFG 0xfe08U
#define CEN_AWDL_TLV_CAPS 0xfe09U
#define CEN_AWDL_TLV_SYNC_PARAMS 0xff04U
#define CEN_AWDL_TLV_ELECTION 0xff05U
#define CEN_AWDL_TLV_CHANSEQ 0xff12U
#define CEN_AWDL_CHANSEQ_SLOTS 16
#define CEN_AWDL_EXTRA_ELECTION_LEN 44
#define CEN_AWDL_SYNC_PARAMS_MAX 72
#define CEN_AWDL_EXTRA_TLVS_MAX 384
#define CEN_AWDL_ARPA_MAX 63U
#define CEN_AWDL_OTA_TLV_CHANSEQ 0x12U
#define CEN_AWDL_OTA_TLV_SYNC_PARAMS 0x04U
#define CEN_AWDL_OTA_TLV_ELECTION 0x05U
#define CEN_AWDL_OTA_TLV_DATA_PATH_STATE 0x0cU
#define CEN_AWDL_OTA_TLV_ARPA 0x10U
#define CEN_AWDL_OTA_TLV_VERSION 0x15U
#define CEN_AWDL_OTA_TLV_ELECTION_V2 0x18U
#define CEN_AWDL_OTA_TLV_SERVICE_PARAMS 0x06U
#define CEN_AWDL_OTA_TLV_HT_CAPS 0x07U
#define CEN_AWDL_OTA_TLV_IE_CONTAINER 0x11U
#define CEN_AWDL_VHT_IE_MAX 16U
#define CEN_AWDL_TLV_VHT_IE 0x00bfU
#define WLAN_EID_VHT_CAPABILITY 191
#define CEN_AWDL_SVC_TLVS_MAX 220U
#define BIT(n) (1U << (n))
#undef static_assert
#define static_assert(expr, ...) _Static_assert(expr, #expr)
#define READ_ONCE(x) (x)
#define ALIGN(x, a) (((x) + (a) - 1) & ~((size_t)(a) - 1))
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define IEEE80211_RADIOTAP_TSFT 0
#define IEEE80211_RADIOTAP_TIMESTAMP 22
#define CEN_AWDL_OTA_VERSION_BYTE 0xa2U
#define CEN_AWDL_OTA_DP_STATE_LEN 47U
@HEADER_DEFINES@
#define CEN_ACIPC_MSG_TR_CLOSE 0x0003U
#define CEN_AWDL_DATA_HDR_LEN 16U
#define CEN_AWDL_RFC1042_HDR_LEN 8U
#define CEN_AWDL_DATA_HDR_MAGIC 0x0403U
#define min_t(type, x, y) ((type)(x) < (type)(y) ? (type)(x) : (type)(y))

static inline __le16 cpu_to_le16(u16 v) { return v; }
static inline u16 le16_to_cpu(__le16 v) { return v; }
static inline __le32 cpu_to_le32(u32 v) { return v; }
static inline u32 le32_to_cpu(__le32 v) { return v; }
static inline void put_unaligned_le16(u16 v, void *p) { memcpy(p, &v, 2); }
static inline u16 get_unaligned_le16(const void *p) { u16 v; memcpy(&v, p, 2); return v; }
static inline void put_unaligned_le32(u32 v, void *p) { memcpy(p, &v, 4); }
static inline u32 get_unaligned_le32(const void *p) { u32 v; memcpy(&v, p, 4); return v; }
static inline void put_unaligned_be16(u16 v, void *p)
{
	u8 *b = p;
	b[0] = v >> 8;
	b[1] = v & 0xff;
}
static inline u16 get_unaligned_be16(const void *p)
{
	const u8 *b = p;
	return ((u16)b[0] << 8) | b[1];
}
static inline void ether_addr_copy(u8 *dst, const u8 *src) { memcpy(dst, src, ETH_ALEN); }
static inline void eth_broadcast_addr(u8 *addr) { memset(addr, 0xff, ETH_ALEN); }
static inline bool is_zero_ether_addr(const u8 *a)
{
	return !(a[0] | a[1] | a[2] | a[3] | a[4] | a[5]);
}
static inline bool is_valid_ether_addr(const u8 *a)
{
	return !(a[0] & 1) && !is_zero_ether_addr(a);
}
static inline __attribute__((unused)) bool ether_addr_equal(const u8 *a, const u8 *b)
{
	return !memcmp(a, b, ETH_ALEN);
}
#define HZ 250
#define time_before(a, b) ((long)((a) - (b)) < 0)

static inline size_t strscpy(char *dst, const char *src, size_t size)
{
	size_t n;

	if (!size)
		return 0;
	n = strnlen(src, size - 1);
	memcpy(dst, src, n);
	dst[n] = '\0';
	return n;
}

static const u8 cen_awdl_default_bssid[ETH_ALEN] = {
	0x00, 0x25, 0x00, 0xff, 0x94, 0x73,
};

static const u8 cen_awdl_llc_snap[8] = {
	0xaa, 0xaa, 0x03, 0x00, 0x17, 0xf2, 0x08, 0x00,
};

static const u8 cen_awdl_rfc1042_llc[6] = {
	0xaa, 0xaa, 0x03, 0x00, 0x00, 0x00,
};

struct cen_awdl_tlv_hdr {
	__le16 tag;
	__le16 length;
} __packed;

struct cen_awdl_general_cfg {
	u8 fw_llc_encap;
	u8 virtual_if_disable;
	u8 reserved[2];
} __packed;

@ELECTION_PARAMS@

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

struct cen_awdl_chanseq {
	u8 count_minus_1;
	u8 encoding;
	u8 dup_count;
	u8 step_count;
	__le16 fill_channel;
	__le16 channels[CEN_AWDL_CHANSEQ_SLOTS];
	__le16 reserved;
} __packed;

struct cen_awdl_peer_add_req {
	u8 mac[ETH_ALEN];
	struct cen_awdl_chanseq chanseq;
	u8 flags[26];
} __packed;

struct cen_awdl_tx_clientdata {
	u8 version;
	u8 if_id;
	u8 tid;
	u8 ac;
	u8 dst_addr[ETH_ALEN];
} __packed;

@RX_STRUCTS@

struct net_device {
	u8 dev_addr[ETH_ALEN];
};

struct centauri_wifi;

static unsigned int test_station_channel;

static unsigned int centauri_wifi_station_channel(const struct centauri_wifi *wifi)
{
	(void)wifi;
	return test_station_channel;
}

struct centauri_awdl {
	struct centauri_wifi *wifi;
	struct net_device *netdev;
	bool following;
	u8 max_peers;
	u8 master_channel;
	u8 country[3];
	char arpa[CEN_AWDL_ARPA_MAX + 1];
	struct cen_awdl_general_cfg general_cfg;
	struct cen_awdl_election_params election;
	u8 extra_election[CEN_AWDL_EXTRA_ELECTION_LEN];
	bool extra_election_valid;
	u8 sync_addr[ETH_ALEN];
	struct cen_awdl_mif_sync_params mif_sync;
	u8 sync_params[CEN_AWDL_SYNC_PARAMS_MAX];
	u16 sync_params_len;
	struct cen_awdl_chanseq chanseq;
	bool chanseq_written;
	u8 vht_ie[CEN_AWDL_VHT_IE_MAX];
	u8 vht_ie_len;
	u8 svc_tlvs[CEN_AWDL_SVC_TLVS_MAX];
	u16 svc_tlvs_len;
	u16 svc_sui;
};
'''

TESTS = r'''
static bool find_tlv(const u8 *buf, size_t len, u16 wanted,
		     const u8 **out_val, u16 *out_len)
{
	size_t pos = 0;

	while (pos + 4 <= len) {
		u16 tag = get_unaligned_le16(buf + pos);
		u16 tlen = get_unaligned_le16(buf + pos + 2);

		if (tlen > len - pos - 4)
			return false;
		if (tag == wanted) {
			*out_val = buf + pos + 4;
			*out_len = tlen;
			return true;
		}
		pos += 4 + tlen;
	}
	return false;
}

int main(void)
{
	struct centauri_awdl awdl;
	struct cen_awdl_tx_clientdata cd;
	struct cen_awdl_peer_add_req peer_req;
	u8 reply[256];
	u8 start_buf[512];
	const u8 *val;
	u16 vlen;
	size_t off = 0, slen;
	u8 peer_mac[ETH_ALEN] = { 0x02, 0xaa, 0xbb, 0xcc, 0xdd, 0xee };

	/* 1. Struct layout invariants. */
	assert(sizeof(struct cen_awdl_general_cfg) == 4);
	assert(sizeof(struct cen_awdl_election_params) == 20);
	assert(sizeof(struct cen_awdl_mif_sync_params) == 20);
	assert(sizeof(struct cen_awdl_chanseq) == 40);
	assert(sizeof(struct cen_awdl_peer_add_req) == 72);
	assert(sizeof(struct cen_awdl_tx_clientdata) == 10);

	/* 2. RX footer interface ID demux (bits 10:8). */
	assert(cen_rx_footer_if_id(0x00000080U) == CEN_IF_STA);
	assert(cen_rx_footer_if_id(0x00000180U) == CEN_IF_AWDL);
	assert(cen_rx_footer_if_id(0x01002180U) == CEN_IF_AWDL);

	/* 3. Channel encoding and 4-slot dwell sequence matching step_count=3. */
	assert(cen_awdl_encode_channel(6) == ((81U << 8) | 6));
	assert(cen_awdl_encode_channel(44) == ((128U << 8) | 44));
	assert(cen_awdl_encode_channel(52) == ((128U << 8) | 52));
	assert(cen_awdl_encode_channel(100) == ((128U << 8) | 100));
	assert(cen_awdl_encode_channel(149) == ((128U << 8) | 149));
	assert(cen_awdl_encode_channel(165) == ((125U << 8) | 165));
	assert(cen_awdl_encode_channel(0) == 0);

	memset(&awdl, 0, sizeof(awdl));
	struct net_device fake_dev = { { 0x02, 0x11, 0x22, 0x33, 0x44, 0x55 } };
	awdl.netdev = &fake_dev;
	cen_awdl_init_defaults(&awdl);
	assert(awdl.general_cfg.fw_llc_encap == 0);
	assert(awdl.chanseq.count_minus_1 == 15);
	assert(awdl.chanseq.encoding == 3);
	assert(awdl.chanseq.step_count == 3);
	for (unsigned int i = 0; i < 4; i++) {
		assert(le16_to_cpu(awdl.chanseq.channels[i]) == ((81U << 8) | 6));
		assert(le16_to_cpu(awdl.chanseq.channels[4 + i]) == 0);
		assert(le16_to_cpu(awdl.chanseq.channels[8 + i]) == ((81U << 8) | 6));
		assert(le16_to_cpu(awdl.chanseq.channels[12 + i]) == ((128U << 8) | 149));
	}

	struct cen_awdl_chanseq multi;
	cen_awdl_build_default_chanseq(&multi, 6, 44, 149);
	assert(le16_to_cpu(multi.channels[0]) == ((81U << 8) | 6));
	assert(le16_to_cpu(multi.channels[3]) == ((81U << 8) | 6));
	assert(le16_to_cpu(multi.channels[4]) == ((128U << 8) | 44));
	assert(le16_to_cpu(multi.channels[7]) == ((128U << 8) | 44));
	assert(le16_to_cpu(multi.channels[8]) == ((81U << 8) | 6));
	assert(le16_to_cpu(multi.channels[12]) == ((128U << 8) | 149));
	assert(le16_to_cpu(multi.channels[15]) == ((128U << 8) | 149));

	/* 3b. The start sequence follows the station channel. */
	{
		struct centauri_awdl st;

		memset(&st, 0, sizeof(st));
		test_station_channel = 149;
		cen_awdl_follow_station_chanseq(&st);
		assert(st.master_channel == 6);
		assert(le16_to_cpu(st.chanseq.channels[0]) == ((81U << 8) | 6));
		assert(le16_to_cpu(st.chanseq.channels[4]) == ((128U << 8) | 149));
		assert(le16_to_cpu(st.chanseq.channels[12]) == ((128U << 8) | 149));
		test_station_channel = 11;
		cen_awdl_follow_station_chanseq(&st);
		assert(st.master_channel == 11);
		assert(le16_to_cpu(st.chanseq.channels[8]) == ((81U << 8) | 11));
		assert(st.chanseq.channels[4] == 0 && st.chanseq.channels[12] == 0);
		test_station_channel = 0;
		cen_awdl_follow_station_chanseq(&st);
		assert(st.chanseq.channels[4] == 0);
		assert(le16_to_cpu(st.chanseq.channels[12]) == ((128U << 8) | 149));
		test_station_channel = 153;	/* upper channel of a pair */
		cen_awdl_follow_station_chanseq(&st);
		assert(le16_to_cpu(st.chanseq.channels[12]) == ((128U << 8) | 149));
		test_station_channel = 44;
		st.chanseq_written = true;
		cen_awdl_follow_station_chanseq(&st);
		assert(le16_to_cpu(st.chanseq.channels[12]) == ((128U << 8) | 149));
		test_station_channel = 0;
	}

	/* 4. Host LLC/SNAP + AWDL data framing round-trip (TX encap / RX decap). */
	u8 eth_in[32], wire[64], eth_out[64];
	bool was_encap = false;
	for (unsigned int i = 0; i < sizeof(eth_in); i++)
		eth_in[i] = (u8)(0x10 + i);
	eth_in[12] = 0x86;
	eth_in[13] = 0xdd;
	size_t wlen = cen_awdl_encap_frame(wire, sizeof(wire), eth_in,
					   sizeof(eth_in), 0x1234);
	assert(wlen == sizeof(eth_in) + CEN_AWDL_DATA_HDR_LEN);
	assert(memcmp(wire, eth_in, 12) == 0);
	assert(get_unaligned_be16(wire + 12) == sizeof(eth_in) + 2);
	assert(memcmp(wire + 14, cen_awdl_llc_snap, 8) == 0);
	assert(get_unaligned_le16(wire + 22) == CEN_AWDL_DATA_HDR_MAGIC);
	assert(get_unaligned_le16(wire + 24) == 0x0234);
	assert(get_unaligned_le16(wire + 26) == 0);
	assert(wire[28] == 0x86 && wire[29] == 0xdd);

	size_t rlen = cen_awdl_decap_frame(eth_out, sizeof(eth_out), wire,
					   wlen, &was_encap);
	assert(was_encap && rlen == sizeof(eth_in));
	assert(memcmp(eth_out, eth_in, sizeof(eth_in)) == 0);

	/* Plain Ethernet II passes through untouched. */
	rlen = cen_awdl_decap_frame(eth_out, sizeof(eth_out), eth_in,
				    sizeof(eth_in), &was_encap);
	assert(!was_encap && rlen == sizeof(eth_in));
	assert(memcmp(eth_out, eth_in, sizeof(eth_in)) == 0);
	assert(cen_awdl_encap_frame(wire, 20, eth_in, sizeof(eth_in), 0) == 0);

	/* 8-byte RFC1042 LLC/SNAP frames decapsulate cleanly to Ethernet II. */
	u8 rfc_wire[64];
	memcpy(rfc_wire, eth_in, 12);
	put_unaligned_be16(sizeof(eth_in) - 12 + 6, rfc_wire + 12);
	memcpy(rfc_wire + 14, cen_awdl_rfc1042_llc, 6);
	memcpy(rfc_wire + 20, eth_in + 12, sizeof(eth_in) - 12);
	rlen = cen_awdl_decap_frame(eth_out, sizeof(eth_out), rfc_wire,
				    sizeof(eth_in) + CEN_AWDL_RFC1042_HDR_LEN,
				    &was_encap);
	assert(was_encap && rlen == sizeof(eth_in));
	assert(memcmp(eth_out, eth_in, sizeof(eth_in)) == 0);

	/* 4b. Over-the-air PSF/MIF extra TLVs (Version 0xa2, 47-byte Data Path State, ARPA). */
	assert(cen_awdl_social_channel_map(&awdl.chanseq) == 0x0005U);
	assert(cen_awdl_social_channel_map(&multi) == 0x0007U);
	u8 extra_buf[CEN_AWDL_EXTRA_TLVS_MAX];
	size_t elen = cen_awdl_append_dp_state(&awdl, extra_buf, sizeof(extra_buf), 0);
	elen = cen_awdl_append_arpa(&awdl, extra_buf, sizeof(extra_buf), elen);
	assert(elen == 50 + 13);
	assert(extra_buf[0] == CEN_AWDL_OTA_TLV_DATA_PATH_STATE);
	assert(get_unaligned_le16(extra_buf + 1) == CEN_AWDL_OTA_DP_STATE_LEN);
	assert(get_unaligned_le16(extra_buf + 3) == 0xab23U);
	assert(extra_buf[5] == 'U' && extra_buf[6] == 'S' && extra_buf[7] == 0);
	assert(get_unaligned_le16(extra_buf + 8) == 0x0005U);
	assert(memcmp(extra_buf + 18, fake_dev.dev_addr, ETH_ALEN) == 0);
	assert(get_unaligned_le16(extra_buf + 24) == 0x0004U);
	assert(get_unaligned_le16(extra_buf + 30) == 0x0021U);
	assert(extra_buf[50] == CEN_AWDL_OTA_TLV_ARPA);
	assert(get_unaligned_le16(extra_buf + 51) == 10);
	assert(extra_buf[53] == 0x03 && extra_buf[54] == 6);
	assert(memcmp(extra_buf + 55, "aurora", 6) == 0);
	assert(extra_buf[61] == 0xc0 && extra_buf[62] == 0x0c);

	/*
	 * 4c. Frame templates: TLV order of sync (PSF) and master-indication
	 * (MIF) frames as nearby peers send them, within the 384-byte limit.
	 */
	{
		static const u8 vht[14] = { 0xbf, 0x0c, 0x76, 0xf0, 0xb1, 0x13,
					    0xfa, 0xff, 0x00, 0x00, 0xfa, 0xff,
					    0x00, 0x20 };
		static const u8 psf_order[] = { 0x06, 0x18, 0x0c, 0x07, 0x11, 0x15 };
		static const u8 mif_order[] = { 0x18, 0x0c, 0x07, 0x11, 0x10,
						0x15, 0x06, 0x02 };
		u8 tbuf[CEN_AWDL_EXTRA_TLVS_MAX];
		size_t tlen, pos;
		unsigned int n;

		memcpy(awdl.vht_ie, vht, sizeof(vht));
		awdl.vht_ie_len = sizeof(vht);
		awdl.svc_tlvs[0] = 0x02;
		put_unaligned_le16(200, awdl.svc_tlvs + 1);
		awdl.svc_tlvs_len = 203;
		awdl.svc_sui = 7;

		tlen = cen_awdl_build_template(&awdl, tbuf, sizeof(tbuf), false);
		for (pos = 0, n = 0; pos < tlen; n++) {
			assert(n < sizeof(psf_order) && tbuf[pos] == psf_order[n]);
			if (tbuf[pos] == CEN_AWDL_OTA_TLV_SERVICE_PARAMS) {
				assert(get_unaligned_le16(tbuf + pos + 1) == 9);
				assert(get_unaligned_le16(tbuf + pos + 6) == 7);
			}
			if (tbuf[pos] == CEN_AWDL_OTA_TLV_ELECTION_V2) {
				assert(get_unaligned_le16(tbuf + pos + 1) == 40);
				assert(memcmp(tbuf + pos + 9, fake_dev.dev_addr, ETH_ALEN) == 0);
			}
			if (tbuf[pos] == CEN_AWDL_OTA_TLV_DATA_PATH_STATE)
				assert(get_unaligned_le16(tbuf + pos + 3) == 0xab23U);
			pos += 3 + get_unaligned_le16(tbuf + pos + 1);
		}
		assert(n == sizeof(psf_order) && pos == tlen);

		/* Service responses no longer fit next to the hostname. */
		tlen = cen_awdl_build_template(&awdl, tbuf, sizeof(tbuf), true);
		assert(tlen <= CEN_AWDL_EXTRA_TLVS_MAX);
		awdl.svc_tlvs_len = 23;
		put_unaligned_le16(20, awdl.svc_tlvs + 1);
		tlen = cen_awdl_build_template(&awdl, tbuf, sizeof(tbuf), true);
		for (pos = 0, n = 0; pos < tlen; n++) {
			assert(n < sizeof(mif_order) && tbuf[pos] == mif_order[n]);
			if (tbuf[pos] == CEN_AWDL_OTA_TLV_DATA_PATH_STATE) {
				assert(get_unaligned_le16(tbuf + pos + 3) == 0xab23U);
				assert(get_unaligned_le16(tbuf + pos + 30) == 0x0021U);
			}
			if (tbuf[pos] == CEN_AWDL_OTA_TLV_ARPA) {
				u16 alen = get_unaligned_le16(tbuf + pos + 1);

				assert(tbuf[pos + 3 + alen - 2] == 0xc0 &&
				       tbuf[pos + 3 + alen - 1] == 0x0c);
			}
			pos += 3 + get_unaligned_le16(tbuf + pos + 1);
		}
		assert(n == sizeof(mif_order) && pos == tlen);

		awdl.svc_tlvs_len = 0;
		awdl.vht_ie_len = 0;
	}

	/* 5. Firmware defaults TLV parsing + active sync override in START. */
	u8 cap = 24;
	u8 extra_el[CEN_AWDL_EXTRA_ELECTION_LEN];
	u8 sync_p[56];
	memset(extra_el, 0x5a, sizeof(extra_el));
	extra_el[8] = 0;
	extra_el[9] = 0;
	memset(sync_p, 0, sizeof(sync_p));
	sync_p[3] = 44;
	off = cen_awdl_append_tlv(reply, sizeof(reply), off, CEN_AWDL_TLV_CAPS, &cap, 1);
	off = cen_awdl_append_tlv(reply, sizeof(reply), off, CEN_AWDL_TLV_EXTRA_ELECTION,
				  extra_el, sizeof(extra_el));
	off = cen_awdl_append_tlv(reply, sizeof(reply), off, CEN_AWDL_TLV_SYNC_PARAMS,
				  sync_p, sizeof(sync_p));
	cen_awdl_parse_defaults(&awdl, reply, off);
	assert(awdl.max_peers == 24);
	assert(awdl.extra_election_valid);
	assert(awdl.sync_params_len == 56);
	assert(awdl.master_channel == 44);

	/*
	 * 5b. The start payload synchronizes in the election role towards
	 * our own address whatever the defaults said, with no parent timing.
	 */
	slen = cen_awdl_build_start_payload(&awdl, start_buf, sizeof(start_buf));
	assert(slen > 5);
	assert(get_unaligned_le16(start_buf) == 0x0602);
	assert(find_tlv(start_buf + 5, slen - 5, CEN_AWDL_TLV_GENERAL_CFG, &val, &vlen));
	assert(vlen == 3 && !val[0] && !val[1] && !val[2]);
	assert(find_tlv(start_buf + 5, slen - 5, CEN_AWDL_TLV_ELECTION, &val, &vlen));
	assert(vlen == 19);
	assert(find_tlv(start_buf + 5, slen - 5, CEN_AWDL_TLV_EXTRA_ELECTION, &val, &vlen));
	assert(vlen == CEN_AWDL_EXTRA_ELECTION_LEN - 1);
	assert(val[8] == 1 && val[9] == CEN_AWDL_SYNC_ROLE_ELECTION);
	assert(memcmp(val + 10, fake_dev.dev_addr, ETH_ALEN) == 0);
	assert(val[16] == 0);
	assert(val[17] == 0x5a);
	assert(find_tlv(start_buf + 5, slen - 5, CEN_AWDL_TLV_MIF_SYNC, &val, &vlen));
	assert(vlen == 20 && val[0] == 1 && val[1] == 1);
	assert(memcmp(val + 6, cen_awdl_default_bssid, ETH_ALEN) == 0);
	/*
	 * 5b'. 0xff04: the 33-byte header with extension counts 3 and no
	 * master to synchronize to while top master, then a (flags, channel)
	 * pair per slot: 0x2b for 2.4 GHz, 0x1d with the 40 MHz centre.
	 */
	assert(find_tlv(start_buf + 5, slen - 5, CEN_AWDL_TLV_SYNC_PARAMS, &val, &vlen));
	assert(vlen == 33 + 6 + 32);
	assert(val[3] == 44);
	assert(val[17] == 3 && val[20] == 3);
	assert(is_zero_ether_addr(val + 21));
	assert(val[34] == 1);
	assert(val[39] == 0x2b && val[40] == 6);
	assert(val[39 + 8] == 0 && val[40 + 8] == 0);
	assert(val[39 + 24] == 0x1d && val[40 + 24] == 151);
	awdl.following = true;
	memcpy(awdl.sync_addr, peer_mac, ETH_ALEN);
	slen = cen_awdl_build_start_payload(&awdl, start_buf, sizeof(start_buf));
	assert(find_tlv(start_buf + 5, slen - 5, CEN_AWDL_TLV_SYNC_PARAMS, &val, &vlen));
	assert(memcmp(val + 21, peer_mac, ETH_ALEN) == 0);
	awdl.following = false;
	memset(awdl.sync_addr, 0, ETH_ALEN);

	/* 5b''. An explicit sync address wins over our own. */
	memcpy(awdl.sync_addr, peer_mac, ETH_ALEN);
	slen = cen_awdl_build_start_payload(&awdl, start_buf, sizeof(start_buf));
	assert(find_tlv(start_buf + 5, slen - 5, CEN_AWDL_TLV_EXTRA_ELECTION, &val, &vlen));
	assert(memcmp(val + 10, peer_mac, ETH_ALEN) == 0);
	memset(awdl.sync_addr, 0, ETH_ALEN);

	/*
	 * 5c. Election parameters keep the over-the-air layout: the J815
	 * defaults carry metric 0x500 at +11 and +15, and a start as top
	 * master puts distance 0 at +3 and our address at +5.
	 */
	{
		static const u8 fw_el[20] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
					      5, 0, 0, 0, 5, 0, 0, 0 };
		size_t eoff = 0;

		assert(offsetof(struct cen_awdl_election_params, distance_from_top) == 3);
		assert(offsetof(struct cen_awdl_election_params, top_master) == 5);
		assert(offsetof(struct cen_awdl_election_params, top_master_metric) == 11);
		assert(offsetof(struct cen_awdl_election_params, self_metric) == 15);
		eoff = cen_awdl_append_tlv(reply, sizeof(reply), 0, CEN_AWDL_TLV_ELECTION,
					   fw_el, sizeof(fw_el));
		cen_awdl_parse_defaults(&awdl, reply, eoff);
		assert(le32_to_cpu(awdl.election.self_metric) == 0x500);
		awdl.election.distance_from_top = 0;
		ether_addr_copy(awdl.election.top_master, fake_dev.dev_addr);
		awdl.election.top_master_metric = awdl.election.self_metric;
		slen = cen_awdl_build_start_payload(&awdl, start_buf, sizeof(start_buf));
		assert(find_tlv(start_buf + 5, slen - 5, CEN_AWDL_TLV_ELECTION, &val, &vlen));
		assert(val[3] == 0);
		assert(memcmp(val + 5, fake_dev.dev_addr, ETH_ALEN) == 0);
		assert(get_unaligned_le32(val + 11) == 0x500);
		assert(get_unaligned_le32(val + 15) == 0x500);
	}

	/* 6. TX clientdata (MCBC tid=6 vs unicast tid=0) & ACIPC TR close message. */
	cen_awdl_build_tx_clientdata(&cd, true, NULL);
	assert(cd.version == 0 && cd.if_id == CEN_IF_AWDL && cd.tid == 6 && cd.ac == 3);
	assert(cd.dst_addr[0] == 0xff && cd.dst_addr[5] == 0xff);

	cen_awdl_build_tx_clientdata(&cd, false, peer_mac);
	assert(cd.version == 0 && cd.if_id == CEN_IF_AWDL && cd.tid == 0 && cd.ac == 0);
	assert(memcmp(cd.dst_addr, peer_mac, ETH_ALEN) == 0);

	u8 close_msg[4];
	cen_awdl_build_tr_close_msg(close_msg, 26);
	assert(get_unaligned_le16(close_msg) == CEN_ACIPC_MSG_TR_CLOSE);
	assert(get_unaligned_le16(close_msg + 2) == 26);

	/* 7. Peer cache add payload. */
	cen_awdl_build_peer_add(&peer_req, peer_mac, &awdl.chanseq);
	assert(memcmp(peer_req.mac, peer_mac, ETH_ALEN) == 0);
	assert(peer_req.chanseq.count_minus_1 == 15);

	/* 8. Received AWDL Action Frame (PSF/MIF) parser and election comparison. */
	u8 af[256];
	memset(af, 0, sizeof(af));
	af[0] = 0xd0;
	memcpy(af + 10, peer_mac, ETH_ALEN);
	af[24] = 0x7f;
	af[25] = 0x00;
	af[26] = 0x17;
	af[27] = 0xf2;
	af[28] = 0x08;
	af[29] = 0x10;
	af[30] = 0x03; /* MIF */
	size_t af_len = 40;
	u8 sync_tlv[4] = { 0, 0, 0, 149 };
	af_len = cen_awdl_append_ota_tlv(af, sizeof(af), af_len,
					 CEN_AWDL_OTA_TLV_SYNC_PARAMS,
					 sync_tlv, sizeof(sync_tlv));
	u8 el_tlv[21];
	memset(el_tlv, 0, sizeof(el_tlv));
	el_tlv[3] = 0;
	memcpy(el_tlv + 5, peer_mac, ETH_ALEN);
	put_unaligned_le32(120, el_tlv + 11);
	put_unaligned_le32(120, el_tlv + 15);
	af_len = cen_awdl_append_ota_tlv(af, sizeof(af), af_len,
					 CEN_AWDL_OTA_TLV_ELECTION,
					 el_tlv, sizeof(el_tlv));
	u8 cs_tlv[6 + 16 * 2];
	memset(cs_tlv, 0, sizeof(cs_tlv));
	cs_tlv[0] = 15;
	cs_tlv[1] = 3;
	cs_tlv[3] = 3;
	put_unaligned_le16(0xffff, cs_tlv + 4);
	for (unsigned int i = 0; i < 16; i++)
		put_unaligned_le16(cen_awdl_encode_channel(149), cs_tlv + 6 + i * 2);
	af_len = cen_awdl_append_ota_tlv(af, sizeof(af), af_len,
					 CEN_AWDL_OTA_TLV_CHANSEQ,
					 cs_tlv, sizeof(cs_tlv));
	struct cen_awdl_rx_peer_info pinfo;
	assert(cen_awdl_parse_action_frame(af, af_len, &pinfo));
	assert(memcmp(pinfo.mac, peer_mac, ETH_ALEN) == 0);
	assert(pinfo.subtype == 3 && pinfo.master_channel == 149);
	assert(pinfo.has_election && pinfo.top_master_metric == 120);
	assert(cen_awdl_peer_wins_election(60, fake_dev.dev_addr, 120, peer_mac));
	assert(!cen_awdl_peer_wins_election(120, peer_mac, 60, fake_dev.dev_addr));
	assert(cen_awdl_peer_wins_election(60, fake_dev.dev_addr, 60, peer_mac));
	af[28] = 0x09;
	assert(!cen_awdl_parse_action_frame(af, af_len, &pinfo));

	/*
	 * 8b. PSF shaped like one received from a nearby peer (addresses
	 * replaced): sync parameters, a 16-slot opclass-encoded sequence that
	 * is mostly channel 149 (80 MHz class) with channel 6 in slot 8, and
	 * an election-tree TLV whose master counter differs from the distance.
	 */
	memset(af, 0, sizeof(af));
	af[0] = 0xd0;
	memcpy(af + 10, peer_mac, ETH_ALEN);
	af[24] = 0x7f;
	af[26] = 0x17;
	af[27] = 0xf2;
	af[28] = 0x08;
	af[29] = 0x10;
	af[30] = 0x00; /* PSF */
	af_len = 40;
	u8 sync2[4] = { 6, 3, 0, 6 };
	af_len = cen_awdl_append_ota_tlv(af, sizeof(af), af_len,
					 CEN_AWDL_OTA_TLV_SYNC_PARAMS,
					 sync2, sizeof(sync2));
	static const u8 slots_ch[16] = {
		149, 149, 149, 0, 0, 0, 0, 149, 6, 149, 149, 149, 0, 0, 0, 149,
	};
	u8 cs2[6 + 16 * 2 + 3];
	memset(cs2, 0, sizeof(cs2));
	cs2[0] = 15;
	cs2[1] = 3;
	cs2[3] = 3;
	put_unaligned_le16(0xffff, cs2 + 4);
	for (unsigned int i = 0; i < 16; i++) {
		cs2[6 + i * 2] = slots_ch[i];
		cs2[7 + i * 2] = slots_ch[i] == 6 ? 0x51 :
				 slots_ch[i] ? 0x80 : 0x00;
	}
	af_len = cen_awdl_append_ota_tlv(af, sizeof(af), af_len,
					 CEN_AWDL_OTA_TLV_CHANSEQ,
					 cs2, sizeof(cs2));
	u8 tree[40];
	memset(tree, 0, sizeof(tree));
	memcpy(tree, peer_mac, ETH_ALEN);
	memcpy(tree + 6, peer_mac, ETH_ALEN);
	put_unaligned_le32(0xf1ec, tree + 12);
	put_unaligned_le32(2, tree + 16);
	put_unaligned_le32(521, tree + 20);
	put_unaligned_le32(300, tree + 24);
	put_unaligned_le32(0xf1ec, tree + 36);
	af_len = cen_awdl_append_ota_tlv(af, sizeof(af), af_len,
					 CEN_AWDL_OTA_TLV_ELECTION_V2,
					 tree, sizeof(tree));
	assert(cen_awdl_parse_action_frame(af, af_len, &pinfo));
	assert(pinfo.subtype == 0 && pinfo.master_channel == 6);
	assert(pinfo.has_election);
	assert(pinfo.distance_from_top == 2);
	assert(pinfo.top_master_metric == 521 && pinfo.self_metric == 300);
	assert(memcmp(pinfo.top_master, peer_mac, ETH_ALEN) == 0);

	/*
	 * 9. Radiotap walk: two presence words, TSFT, flags, channel, signal,
	 * antenna and RX flags ahead of the timestamp field, which needs
	 * 8-byte alignment (offset 40, not 36).
	 */
	{
		u8 rh[64];
		struct cen_awdl_rtap_info rt;
		u32 tsv;

		memset(rh, 0, sizeof(rh));
		put_unaligned_le16(56, rh + 2);
		put_unaligned_le32(BIT(0) | BIT(1) | BIT(3) | BIT(5) | BIT(11) |
				   BIT(14) | BIT(22) | BIT(29) | BIT(31), rh + 4);
		put_unaligned_le32(BIT(5), rh + 8);
		put_unaligned_le32(0x55667788, rh + 16);
		put_unaligned_le32(0x11223344, rh + 20);
		put_unaligned_le32(0x01020304, rh + 40);
		put_unaligned_le32(0xaabbccdd, rh + 44);
		put_unaligned_le32(0xdeadbeef, rh + 36);
		assert(cen_awdl_parse_radiotap(rh, sizeof(rh), &rt));
		assert(rt.len == 56);
		assert(rt.has_tsft && rt.tsft == 0x55667788);
		assert(rt.has_timestamp && rt.timestamp == 0x01020304);
		assert(cen_awdl_rx_time(&rt, &tsv) && tsv == 0x01020304);
		/* One presence word: TSFT right after it, at offset 8. */
		put_unaligned_le32(BIT(0) | BIT(1), rh + 4);
		put_unaligned_le32(0x55667788, rh + 8);
		assert(cen_awdl_parse_radiotap(rh, sizeof(rh), &rt));
		assert(!rt.has_timestamp);
		assert(cen_awdl_rx_time(&rt, &tsv) && tsv == 0x55667788);
		rt.has_tsft = false;
		assert(!cen_awdl_rx_time(&rt, &tsv));
		rh[0] = 1;
		assert(!cen_awdl_parse_radiotap(rh, sizeof(rh), &rt));
		rh[0] = 0;
		put_unaligned_le16(80, rh + 2);
		assert(!cen_awdl_parse_radiotap(rh, sizeof(rh), &rt));
		put_unaligned_le16(8, rh + 2);
		put_unaligned_le32(BIT(31), rh + 4);
		assert(!cen_awdl_parse_radiotap(rh, sizeof(rh), &rt));
	}

	/*
	 * 10. Sync parameters TLV as a nearby peer sends it (address
	 * replaced) and the TX delay from the action frame header.
	 */
	static const u8 peer_sync[31] = {
		0x06, 0x03, 0x00, 0x06, 0x00, 0x10, 0x00, 0x6e, 0x00, 0x00,
		0x10, 0x10, 0x00, 0x0c, 0x00, 0x03, 0x00, 0x03, 0x03, 0x03,
		0x03, 0x02, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0x04, 0x00, 0x43,
		0x96,
	};
	memset(af, 0, sizeof(af));
	af[0] = 0xd0;
	memcpy(af + 10, peer_mac, ETH_ALEN);
	af[24] = 0x7f;
	af[26] = 0x17;
	af[27] = 0xf2;
	af[28] = 0x08;
	af[29] = 0x10;
	put_unaligned_le32(1000, af + 32);
	put_unaligned_le32(900, af + 36);
	af_len = 40;
	af_len = cen_awdl_append_ota_tlv(af, sizeof(af), af_len,
					 CEN_AWDL_OTA_TLV_SYNC_PARAMS,
					 peer_sync, sizeof(peer_sync));
	u8 el2[21];
	memset(el2, 0, sizeof(el2));
	el2[3] = 1;
	memcpy(el2 + 5, peer_mac, ETH_ALEN);
	put_unaligned_le32(521, el2 + 11);
	put_unaligned_le32(300, el2 + 15);
	af_len = cen_awdl_append_ota_tlv(af, sizeof(af), af_len,
					 CEN_AWDL_OTA_TLV_ELECTION,
					 el2, sizeof(el2));
	assert(cen_awdl_parse_action_frame(af, af_len, &pinfo));
	assert(pinfo.has_sync && pinfo.master_channel == 6);
	assert(pinfo.down_counter == 3);
	assert(pinfo.aw_period == 0x10 && pinfo.aw_ext_length == 0x10);
	assert(pinfo.aw_length == 0x0c);
	assert(pinfo.presence == 4);
	assert(pinfo.aw_seq == 0x9643);
	assert(pinfo.tx_delay == 100);
	put_unaligned_le32(800, af + 32);
	assert(cen_awdl_parse_action_frame(af, af_len, &pinfo));
	assert(pinfo.tx_delay == 0);
	put_unaligned_le32(1000, af + 32);

	/*
	 * 11. Parent choice: the phone's metric 521 loses against our 0x500,
	 * wins against 100; never our own address as root; sync parameters
	 * must be usable and the frame must carry a receive time.
	 */
	{
		struct cen_awdl_rtap_info rt;
		struct cen_awdl_parent parent;
		int ret_lose, ret_win, ret_self, ret_ext, ret_pres, ret_nots;

		memset(&rt, 0, sizeof(rt));
		rt.has_timestamp = true;
		rt.timestamp = 0x01020304;
		awdl.election.distance_from_top = 0;
		memcpy(awdl.election.top_master, fake_dev.dev_addr, ETH_ALEN);
		awdl.election.top_master_metric = cpu_to_le32(0x500);
		assert(cen_awdl_parse_action_frame(af, af_len, &pinfo));
		ret_lose = cen_awdl_parent_from_frame(&awdl, &pinfo, &rt, &parent);
		assert(ret_lose == -EAGAIN);
		awdl.election.top_master_metric = cpu_to_le32(100);
		ret_win = cen_awdl_parent_from_frame(&awdl, &pinfo, &rt, &parent);
		assert(ret_win == 0);
		assert(memcmp(parent.addr, peer_mac, ETH_ALEN) == 0);
		assert(memcmp(parent.top_master, peer_mac, ETH_ALEN) == 0);
		assert(parent.top_metric == 521 && parent.distance == 2);
		assert(parent.rx_time == 0x01020304 && parent.tx_delay == 100);
		assert(parent.down_counter == 3 && parent.aw_period == 0x10);
		assert(parent.aw_length == 0x0c && parent.presence == 4);
		assert(parent.aw_seq == 0x9643 && parent.channel == 6);

		memcpy(pinfo.top_master, fake_dev.dev_addr, ETH_ALEN);
		ret_self = cen_awdl_parent_from_frame(&awdl, &pinfo, &rt, &parent);
		assert(ret_self == -EAGAIN);
		memcpy(pinfo.top_master, peer_mac, ETH_ALEN);

		pinfo.aw_ext_length = 8;
		ret_ext = cen_awdl_parent_from_frame(&awdl, &pinfo, &rt, &parent);
		assert(ret_ext == -EINVAL);
		pinfo.aw_ext_length = 16;
		pinfo.presence = 0;
		ret_pres = cen_awdl_parent_from_frame(&awdl, &pinfo, &rt, &parent);
		assert(ret_pres == -EINVAL);
		pinfo.presence = 4;
		rt.has_timestamp = false;
		ret_nots = cen_awdl_parent_from_frame(&awdl, &pinfo, &rt, &parent);
		assert(ret_nots == -ENODATA);
		rt.has_timestamp = true;

		/*
		 * 12. Parent timing in 0xfe00: role, parent address, valid
		 * flag and timing fields at their offsets; the cached copy
		 * keeps valid clear so later pushes never replay old timing.
		 */
		u8 adv[CEN_AWDL_EXTRA_ELECTION_LEN];

		assert(cen_awdl_parent_from_frame(&awdl, &pinfo, &rt, &parent) == 0);
		awdl.extra_election[16] = 0;
		cen_awdl_build_advert(&awdl, &parent, adv);
		assert(adv[8] == 1 && adv[9] == CEN_AWDL_SYNC_ROLE_ELECTION);
		assert(memcmp(adv + 10, peer_mac, ETH_ALEN) == 0);
		assert(adv[16] == 1);
		assert(get_unaligned_le32(adv + 17) == 0x01020304);
		assert(get_unaligned_le32(adv + 21) == 100);
		assert(get_unaligned_le16(adv + 25) == 3);
		assert(get_unaligned_le16(adv + 27) == 0x10);
		assert(get_unaligned_le16(adv + 29) == 0x0c);
		assert(adv[31] == 4);
		assert(get_unaligned_le16(adv + 32) == 0x9643);
		assert(awdl.extra_election[16] == 0);
	}

	{
		/*
		 * 13. Peer re-add hold: a peer is not given to the firmware
		 * again within CEN_AWDL_PEER_RETRY, also across a jiffies
		 * wrap, and a full table drops its oldest entry.
		 */
		struct cen_awdl_recent_peer recent[CEN_AWDL_RECENT_PEERS];
		const unsigned long t0 = (unsigned long)-2 * HZ;
		u8 a[ETH_ALEN] = { 0x02, 0, 0, 0, 0, 0xa1 };
		u8 b[ETH_ALEN] = { 0x02, 0, 0, 0, 0, 0xb2 };
		u8 m[ETH_ALEN] = { 0x02, 0, 0, 0, 1, 0 };
		unsigned int k;

		memset(recent, 0, sizeof(recent));
		assert(!cen_awdl_peer_added_lately(recent, a, t0));
		assert(cen_awdl_peer_added_lately(recent, a, t0 + 1));
		assert(!cen_awdl_peer_added_lately(recent, b, t0 + 2));
		/* 4 s later the counter has wrapped, a is still held */
		assert(cen_awdl_peer_added_lately(recent, a, t0 + 4 * HZ));
		assert(!cen_awdl_peer_added_lately(recent, a,
						   t0 + CEN_AWDL_PEER_RETRY));
		assert(cen_awdl_peer_added_lately(recent, a,
						  t0 + CEN_AWDL_PEER_RETRY + 1));
		/* A full table gives the oldest entry to a new peer */
		memset(recent, 0, sizeof(recent));
		for (k = 0; k <= CEN_AWDL_RECENT_PEERS; k++) {
			m[5] = k;
			assert(!cen_awdl_peer_added_lately(recent, m, t0 + k));
		}
		m[5] = 1;
		assert(cen_awdl_peer_added_lately(recent, m, t0 + 20));
		m[5] = CEN_AWDL_RECENT_PEERS;
		assert(cen_awdl_peer_added_lately(recent, m, t0 + 21));
		m[5] = 0;
		assert(!cen_awdl_peer_added_lately(recent, m, t0 + 22));
	}

	puts("PASS: Centauri AWDL protocol helpers");
	return 0;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path)
    parser.add_argument("--negative-controls", action="store_true")
    parser.add_argument("--source-root", type=Path)
    args = parser.parse_args()
    root = args.source_root or Path(__file__).resolve().parents[2]
    hdr_src = (
        root / "drivers/net/wireless/apple/centauri/centauri_awdl.h"
    ).read_text()
    c_src = (
        root / "drivers/net/wireless/apple/centauri/centauri_awdl.c"
    ).read_text()

    prod = "\n\n".join(
        [
            extract_fn(hdr_src, "cen_rx_footer_if_id"),
            extract_fn(hdr_src, "cen_awdl_build_tr_close_msg"),
            extract_fn(c_src, "cen_awdl_encode_channel"),
            extract_fn(c_src, "cen_awdl_build_default_chanseq"),
            extract_fn(c_src, "cen_awdl_follow_station_chanseq"),
            extract_fn(c_src, "cen_awdl_sync_build"),
            extract_fn(c_src, "cen_awdl_encap_frame"),
            extract_fn(c_src, "cen_awdl_decap_frame"),
            extract_fn(c_src, "cen_awdl_social_channel_map"),
            extract_fn(c_src, "cen_awdl_append_ota_tlv"),
            extract_fn(c_src, "cen_awdl_append_dp_state"),
            extract_fn(c_src, "cen_awdl_append_arpa"),
            "static const u8 cen_awdl_ht_caps[20] = {\n\t0x00, 0x00, 0x6f, 0x88, 0x1b, 0xff, 0xff, 0x00,\n\t0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x96,\n\t0x00, 0x01, 0x00, 0x00,\n};",
            extract_fn(c_src, "cen_awdl_append_election_tree"),
            extract_fn(c_src, "cen_awdl_build_template"),
            extract_fn(c_src, "cen_awdl_build_tx_clientdata"),
            extract_fn(c_src, "cen_awdl_parse_action_frame"),
            extract_fn(c_src, "cen_awdl_peer_wins_election"),
            re.search(r"^static const u8 cen_awdl_rtap_fields\[\]\[2\] = \{.*?^\};", c_src, re.S | re.M).group(0),
            extract_fn(c_src, "cen_awdl_parse_radiotap"),
            extract_fn(c_src, "cen_awdl_rx_time"),
            extract_fn(c_src, "cen_awdl_append_tlv"),
            extract_fn(c_src, "cen_awdl_parse_defaults"),
            extract_fn(c_src, "cen_awdl_fill_sync_state"),
            extract_fn(c_src, "cen_awdl_build_advert"),
            extract_fn(c_src, "cen_awdl_build_start_payload"),
            extract_fn(c_src, "cen_awdl_build_peer_add"),
            extract_fn(c_src, "cen_awdl_init_defaults"),
            extract_fn(c_src, "cen_awdl_parent_from_frame"),
            extract_fn(c_src, "cen_awdl_peer_added_lately"),
        ]
    )

    m = re.search(r"struct cen_awdl_election_params \{.*?\} __packed;", hdr_src, re.S)
    if not m:
        raise RuntimeError("struct cen_awdl_election_params not found")
    prelude = PRELUDE.replace("@ELECTION_PARAMS@", m.group(0))
    defines = re.findall(r"^#define CEN_AWDL_(?:SYNC_ROLE|XEL|AW_EXT)_\w+.*$", hdr_src, re.M)
    defines += re.findall(r"^#define CEN_AWDL_(?:RECENT_PEERS|PEER_RETRY)\b.*$", hdr_src, re.M)
    defines += re.findall(r"^#define CEN_AWDL_(?:RTAP_MAX|RX_HDR_LEN|EL_TX_LEN|XEL_TX_LEN|CS_TX_LEN|SYNC_HDR_LEN|SYNC_REF_LEN)\b.*$", c_src, re.M)
    structs = []
    for name in ("cen_awdl_rx_peer_info", "cen_awdl_rtap_info", "cen_awdl_parent",
                 "cen_awdl_recent_peer"):
        sm = re.search(r"^struct " + name + r" \{.*?^\};", hdr_src, re.S | re.M)
        if not sm:
            raise RuntimeError(f"struct {name} not found")
        structs.append(sm.group(0))
    prelude = prelude.replace("@RX_STRUCTS@", "\n\n".join(structs))
    prelude = prelude.replace("@HEADER_DEFINES@", "\n".join(defines))

    variants = [("awdl-production", prelude + prod + TESTS, 0, None)]
    if args.negative_controls:
        controls = [
            (
                "wrong-rx-ifid-shift",
                "return (footer >> 8) & 0x7;",
                "return (footer >> 12) & 0x7;",
                "CEN_IF_AWDL",
            ),
            (
                "wrong-tx-ifid",
                "cd->if_id = CEN_IF_AWDL;",
                "cd->if_id = CEN_IF_STA;",
                "cd.if_id == CEN_IF_AWDL",
            ),
            (
                "wrong-mcbc-tid",
                "cd->tid = 6;",
                "cd->tid = 7;",
                "cd.tid == 6",
            ),
            (
                "wrong-tr-close-opcode",
                "msg[0] = CEN_ACIPC_MSG_TR_CLOSE & 0xff;",
                "msg[0] = 0x01;",
                "CEN_ACIPC_MSG_TR_CLOSE",
            ),
            (
                "missing-sync-addr",
                "\telse if (awdl->netdev)\n\t\tether_addr_copy(xel + CEN_AWDL_XEL_SYNC_ADDR,",
                "\telse if (0)\n\t\tether_addr_copy(xel + CEN_AWDL_XEL_SYNC_ADDR,",
                "fake_dev.dev_addr, ETH_ALEN",
            ),
            (
                "wrong-sync-role",
                "\txel[CEN_AWDL_XEL_ROLE] = CEN_AWDL_SYNC_ROLE_ELECTION;",
                "\txel[CEN_AWDL_XEL_ROLE] = 1;",
                "CEN_AWDL_SYNC_ROLE_ELECTION",
            ),
            (
                "wrong-election-tx-len",
                "#define CEN_AWDL_EL_TX_LEN\t\t19U",
                "#define CEN_AWDL_EL_TX_LEN\t\t20U",
                "vlen == 19",
            ),
            (
                "sync-ref-wrong-flags",
                "\t\t\te[0] = 0x2b;",
                "\t\t\te[0] = 0x1d;",
                "val[39] == 0x2b",
            ),
            (
                "sync-ref-master-while-root",
                "\tif (READ_ONCE(awdl->following)) {\n\t\tif (!is_zero_ether_addr(awdl->sync_addr))",
                "\tif (1) {\n\t\tif (!is_zero_ether_addr(awdl->sync_addr))",
                "is_zero_ether_addr(val + 21)",
            ),
            (
                "station-chanseq-ignored",
                "\t\tcen_awdl_build_default_chanseq(&awdl->chanseq, 6, c, c);",
                "\t\tcen_awdl_build_default_chanseq(&awdl->chanseq, 6, 0, 149);",
                "st.chanseq.channels[4]",
            ),
            (
                "station-chanseq-overrides-debugfs",
                "\tif (awdl->chanseq_written)\n\t\treturn;\n",
                "",
                "st.chanseq.channels[12]) == ((128U << 8) | 149)",
            ),
            (
                "stale-parent-timing",
                "\txel[CEN_AWDL_XEL_ADV_VALID] = 0;\n",
                "",
                "val[16] == 0",
            ),
            (
                "wrong-start-prefix",
                "put_unaligned_le16(0x0602, payload);",
                "put_unaligned_le16(0x0b00, payload);",
                "0x0602",
            ),
            (
                "wrong-encap-magic",
                "put_unaligned_le16(CEN_AWDL_DATA_HDR_MAGIC, dst + 22);",
                "put_unaligned_le16(0x0000, dst + 22);",
                "CEN_AWDL_DATA_HDR_MAGIC",
            ),
            (
                "wrong-social-channel-map",
                "else if (ch == 149)\n\t\t\tmap |= 0x0004U;",
                "else if (ch == 149)\n\t\t\tmap |= 0x0000U;",
                "0x0005U",
            ),
            (
                "wrong-election-tree-distance",
                "u32 dist = get_unaligned_le32(val + 16);",
                "u32 dist = get_unaligned_le32(val + 12);",
                "pinfo.distance_from_top == 2",
            ),
            (
                "wrong-election-layout",
                "\tu8 flags;\n\t__le16 election_id;\n\tu8 distance_from_top;\n\tu8 reserved;\n\tu8 top_master[ETH_ALEN];\n\t__le32 top_master_metric;\n\t__le32 self_metric;\n\tu8 pad;",
                "\tu8 flags;\n\tu8 pad0;\n\t__le16 election_id;\n\tu8 distance_from_top;\n\tu8 reserved;\n\tu8 top_master[ETH_ALEN];\n\t__le32 top_master_metric;\n\t__le32 self_metric;",
                "distance_from_top) == 3",
            ),
            (
                "wrong-rtap-timestamp-align",
                "{ 2, 12 }, { 8, 12 },",
                "{ 2, 12 }, { 4, 12 },",
                "rt.timestamp == 0x01020304",
            ),
            (
                "rtap-ignores-ext-words",
                "\tpos += 4;\n\tfor (bit = 0;",
                "\tpos = 8;\n\tfor (bit = 0;",
                "rt.tsft == 0x55667788",
            ),
            (
                "wrong-sync-aw-seq",
                "info->aw_seq = get_unaligned_le16(val + 29);",
                "info->aw_seq = get_unaligned_le16(val + 28);",
                "pinfo.aw_seq == 0x9643",
            ),
            (
                "wrong-tx-delay-sign",
                "info->tx_delay = phy_tx - target_tx;",
                "info->tx_delay = target_tx - phy_tx;",
                "pinfo.tx_delay == 100",
            ),
            (
                "unsigned-tx-delay",
                "if (phy_tx && target_tx && phy_tx > target_tx)",
                "if (phy_tx && target_tx)",
                "pinfo.tx_delay == 0",
            ),
            (
                "adopt-own-root",
                " ||\n\t    ether_addr_equal(info->top_master, awdl->netdev->dev_addr))",
                ")",
                "ret_self == -EAGAIN",
            ),
            (
                "adopt-any-ext-length",
                "info->aw_ext_length != CEN_AWDL_AW_EXT_LENGTH ||",
                "",
                "ret_ext == -EINVAL",
            ),
            (
                "adopt-without-time",
                "\t\treturn -ENODATA;\n",
                "\t\tp->rx_time = 0;\n",
                "ret_nots == -ENODATA",
            ),
            (
                "advert-not-valid",
                "\txel[CEN_AWDL_XEL_ADV_VALID] = 1;\n",
                "",
                "adv[16] == 1",
            ),
            (
                "advert-period-length-swapped",
                "put_unaligned_le16(p->aw_period, xel + CEN_AWDL_XEL_ADV_AW_PERIOD);",
                "put_unaligned_le16(p->aw_length, xel + CEN_AWDL_XEL_ADV_AW_PERIOD);",
                "get_unaligned_le16(adv + 27) == 0x10",
            ),
            (
                "advert-cached-valid",
                "\tmemcpy(xel, awdl->extra_election, CEN_AWDL_EXTRA_ELECTION_LEN);\n\tcen_awdl_fill_sync_state(awdl, xel);",
                "\t((struct centauri_awdl *)awdl)->extra_election[CEN_AWDL_XEL_ADV_VALID] = 1;\n\tmemcpy(xel, awdl->extra_election, CEN_AWDL_EXTRA_ELECTION_LEN);\n\tcen_awdl_fill_sync_state(awdl, xel);",
                "awdl.extra_election[16] == 0",
            ),
            (
                "wrong-mif-hostname-order",
                "\tif (mif)\n\t\toff = cen_awdl_append_arpa(",
                "\tif (0)\n\t\toff = cen_awdl_append_arpa(",
                "mif_order[n]",
            ),
            (
                "wrong-election-tiebreak",
                "return memcmp(peer_master, cur_master, ETH_ALEN) > 0;",
                "return memcmp(peer_master, cur_master, ETH_ALEN) < 0;",
                "cen_awdl_peer_wins_election(60, fake_dev.dev_addr, 60, peer_mac)",
            ),
            (
                "peer-hold-ignores-time",
                "\t\t\tif (time_before(now, r->added + CEN_AWDL_PEER_RETRY))\n\t\t\t\treturn true;\n",
                "\t\t\treturn true;\n",
                "cen_awdl_peer_added_lately(recent, a,",
            ),
            (
                "peer-hold-evicts-newest",
                "\t\tif (time_before(r->added, slot->added))\n",
                "\t\tif (time_before(slot->added, r->added))\n",
                "cen_awdl_peer_added_lately(recent, m,",
            ),
        ]
        for name, old, new, expected_msg in controls:
            if old in prod:
                code = prelude + prod.replace(old, new, 1) + TESTS
            elif old in prelude:
                code = prelude.replace(old, new, 1) + prod + TESTS
            else:
                raise RuntimeError(f"missing pattern for {name}")
            variants.append((name, code, -signal.SIGABRT, expected_msg))

    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    compiler = shlex.split(os.environ.get("CC", "cc"))
    with tempfile.TemporaryDirectory(prefix="centauri-awdl-test-") as directory:
        out = args.out or Path(directory)
        out.mkdir(parents=True, exist_ok=True)
        for name, code, expected, msg in variants:
            harness, binary = out / (name + ".c"), out / name
            harness.write_text(code)
            subprocess.run(
                compiler + ["-std=gnu11", "-Wall", "-Wextra", "-Werror",
                            str(harness), "-o", str(binary)],
                check=True,
            )
            result = subprocess.run([str(binary)], capture_output=True, text=True)
            (out / (name + ".log")).write_text(result.stdout + result.stderr)
            if result.returncode != expected or (
                expected and ("Assertion" not in result.stderr or msg not in result.stderr)
            ):
                raise RuntimeError(
                    f"{name}: exit {result.returncode}, expected {expected}\n"
                    + result.stdout + result.stderr
                )
            print(f"{name}: {'PASS' if expected == 0 else 'correctly rejected'}")


if __name__ == "__main__":
    main()
