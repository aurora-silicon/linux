/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/* Copyright 2026 Ryan Murray */
#ifndef __APPLE_DCP_HDCP_WIRE_H__
#define __APPLE_DCP_HDCP_WIRE_H__

#include <linux/build_bug.h>
#include <linux/errno.h>
#include <linux/minmax.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/unaligned.h>

/*
 * The DCP services that carry HDCP 2.2, as standard EPIC service calls
 * (struct epic_service_call, then the body described here):
 *
 * - The sink's DP device service ("dcpdp-device-epic") reads and writes the
 *   receiver's HDCP 2.2 registers, addressed as in the DisplayPort HDCP 2.2
 *   register map (DP_HDCP_2_2_REG_*). DCP runs the AUX transaction. A
 *   built-in HDMI port behind an on-board DP to HDMI converter is reached
 *   the same way: the converter is the HDCP repeater.
 * - The HDCP interface service ("dcpdptx-hdcp-interface"), one per port,
 *   takes the transmitter's capability once it is opened.
 * - An authentication session service ("dcpdptx-hdcp-auth-session"),
 *   announced by DCP for each attempt, calls the AP with the session's
 *   events (open, start, H' ready, pairing ready, ReceiverID list ready,
 *   close) and takes progress reports. DCP owns the receiver timeouts: an
 *   attempt that runs out of time is closed by DCP and opened again.
 *
 * Every body field is a little-endian 32-bit word at a 16-byte stride.
 */

/* Register access on the DP device service. */
#define DCP_HDCP_REG_GROUP		1
#define DCP_HDCP_REG_READ		6
#define DCP_HDCP_REG_WRITE		7
/* The register arguments, then the data. */
#define DCP_HDCP_REG_ARGS_LEN		64
/* The longest register block: the receiver certificate. */
#define DCP_HDCP_REG_MAX		1024
/* The read timeout DCP applies on the AUX side. */
#define DCP_HDCP_REG_TIMEOUT_MS		500

/* The device service's query, the first call of a session. */
#define DCP_HDCP_DEV_GROUP		0
#define DCP_HDCP_DEV_QUERY		4
#define DCP_HDCP_DEV_QUERY_LEN		0x20
/* The device service's information call, once after it is opened. */
#define DCP_HDCP_DEV_INFO		0xc
#define DCP_HDCP_DEV_INFO_LEN		0x50

/* The HDCP interface service's capability call. */
#define DCP_HDCP_IF_GROUP		4
#define DCP_HDCP_IF_CAPS		6
#define DCP_HDCP_IF_CAPS_LEN		0x20
#define DCP_HDCP_IF_CAPS_VALUE		3

/* The authentication session's calls to the AP (group 0). */
enum dcp_hdcp_auth_op {
	DCP_HDCP_AUTH_START	= 0x0,
	DCP_HDCP_AUTH_CONFIG	= 0x1,
	DCP_HDCP_AUTH_OPEN	= 0x7,
	DCP_HDCP_AUTH_CLOSE	= 0x8,
	DCP_HDCP_AUTH_H_PRIME	= 0x9,	/* H' is available */
	DCP_HDCP_AUTH_PAIRING	= 0xa,	/* pairing information is available */
	DCP_HDCP_AUTH_LIST	= 0xb,	/* the ReceiverID list is available */
};

/* The AP's progress report on the session. */
#define DCP_HDCP_REPORT_GROUP		2
#define DCP_HDCP_REPORT_PROGRESS	6
#define DCP_HDCP_REPORT_LEN		0x50

enum dcp_hdcp_progress_code {
	DCP_HDCP_PROGRESS_IDLE		= 0x65,	/* opened, or closed again */
	DCP_HDCP_PROGRESS_STARTED	= 0x66,
	DCP_HDCP_PROGRESS_AKE		= 0x69,	/* AKE running */
	DCP_HDCP_PROGRESS_KM_SENT	= 0x6c,
	DCP_HDCP_PROGRESS_H_VERIFIED	= 0x6d,	/* pairing still to come */
	DCP_HDCP_PROGRESS_AKE_DONE	= 0x70,
	DCP_HDCP_PROGRESS_STREAM_READY	= 0x72,
	DCP_HDCP_PROGRESS_ENCRYPTED	= 0x74,
	DCP_HDCP_PROGRESS_SKE_SENT	= 0x75,
	DCP_HDCP_PROGRESS_LIST_VERIFIED	= 0x78,
};

struct dcp_hdcp_reg_read {
	__le32 addr;
	__le32 rsvd0[3];
	__le32 len;
	__le32 rsvd1[3];
	__le32 timeout_ms;
	__le32 rsvd2[7];
} __packed;
static_assert(sizeof(struct dcp_hdcp_reg_read) == DCP_HDCP_REG_ARGS_LEN);

struct dcp_hdcp_reg_write {
	__le32 rsvd0[4];
	__le32 addr;
	__le32 rsvd1[3];
	__le32 len;
	__le32 rsvd2[7];
} __packed;
static_assert(sizeof(struct dcp_hdcp_reg_write) == DCP_HDCP_REG_ARGS_LEN);

/* One word at offset 16: the interface capability, the open's acceptance. */
struct dcp_hdcp_word {
	__le32 rsvd0[4];
	__le32 value;
	__le32 rsvd1[3];
} __packed;
static_assert(sizeof(struct dcp_hdcp_word) == DCP_HDCP_IF_CAPS_LEN);

struct dcp_hdcp_report {
	__le32 authenticated;
	__le32 rsvd0[3];
	__le32 active;
	__le32 rsvd1[3];
	__le32 encrypted;
	__le32 rsvd2[3];
	__le32 repeater;
	__le32 rsvd3[3];
	__le32 code;
	__le32 rsvd4[3];
} __packed;
static_assert(sizeof(struct dcp_hdcp_report) == DCP_HDCP_REPORT_LEN);

static inline bool dcp_hdcp_reg_len_ok(size_t len)
{
	return len && len <= DCP_HDCP_REG_MAX;
}

/* A read request: the arguments, then @len zero bytes for the data. */
static inline int dcp_hdcp_reg_read_encode(void *body, size_t room, u32 addr,
					   size_t len, u32 timeout_ms)
{
	struct dcp_hdcp_reg_read *a = body;

	if (!dcp_hdcp_reg_len_ok(len) || room < DCP_HDCP_REG_ARGS_LEN + len)
		return -EINVAL;
	memset(body, 0, DCP_HDCP_REG_ARGS_LEN + len);
	a->addr = cpu_to_le32(addr);
	a->len = cpu_to_le32(len);
	a->timeout_ms = cpu_to_le32(timeout_ms);
	return DCP_HDCP_REG_ARGS_LEN + len;
}

/*
 * A read answer: it must name the register block asked for and carry all of
 * its bytes. On success the data starts at body + DCP_HDCP_REG_ARGS_LEN.
 */
static inline int dcp_hdcp_reg_read_check(const void *body, size_t received,
					  u32 addr, size_t len)
{
	const struct dcp_hdcp_reg_read *a = body;

	if (!dcp_hdcp_reg_len_ok(len) || received < DCP_HDCP_REG_ARGS_LEN + len)
		return -EPROTO;
	if (le32_to_cpu(a->addr) != addr || le32_to_cpu(a->len) != len)
		return -EPROTO;
	return 0;
}

static inline int dcp_hdcp_reg_write_encode(void *body, size_t room, u32 addr,
					    const void *data, size_t len)
{
	struct dcp_hdcp_reg_write *a = body;

	if (!dcp_hdcp_reg_len_ok(len) || room < DCP_HDCP_REG_ARGS_LEN + len)
		return -EINVAL;
	memset(body, 0, DCP_HDCP_REG_ARGS_LEN);
	a->addr = cpu_to_le32(addr);
	a->len = cpu_to_le32(len);
	memcpy(body + DCP_HDCP_REG_ARGS_LEN, data, len);
	return DCP_HDCP_REG_ARGS_LEN + len;
}

static inline int dcp_hdcp_reg_write_check(const void *body, size_t received,
					   u32 addr, size_t len)
{
	const struct dcp_hdcp_reg_write *a = body;

	if (received < DCP_HDCP_REG_ARGS_LEN)
		return -EPROTO;
	if (le32_to_cpu(a->addr) != addr || le32_to_cpu(a->len) != len)
		return -EPROTO;
	return 0;
}

static inline bool dcp_hdcp_auth_op_known(u32 op)
{
	switch (op) {
	case DCP_HDCP_AUTH_START:
	case DCP_HDCP_AUTH_CONFIG:
	case DCP_HDCP_AUTH_OPEN:
	case DCP_HDCP_AUTH_CLOSE:
	case DCP_HDCP_AUTH_H_PRIME:
	case DCP_HDCP_AUTH_PAIRING:
	case DCP_HDCP_AUTH_LIST:
		return true;
	default:
		return false;
	}
}

/*
 * The AP's answer to a session call: the call's body back, and for the open
 * whether the AP takes the session (1) or not (0).
 */
static inline int dcp_hdcp_auth_reply(u16 group, u32 op, const void *data,
				      size_t size, void *reply, size_t room,
				      bool accept)
{
	if (group || !dcp_hdcp_auth_op_known(op))
		return -EOPNOTSUPP;
	memset(reply, 0, room);
	memcpy(reply, data, min(size, room));
	if (op == DCP_HDCP_AUTH_OPEN) {
		struct dcp_hdcp_word *w = reply;

		if (room < sizeof(*w))
			return -EPROTO;
		w->value = cpu_to_le32(accept);
	}
	return 0;
}

static inline void dcp_hdcp_report_encode(struct dcp_hdcp_report *r,
					  enum dcp_hdcp_progress_code code,
					  bool active, bool repeater,
					  bool encrypted)
{
	memset(r, 0, sizeof(*r));
	r->code = cpu_to_le32(code);
	r->active = cpu_to_le32(active);
	r->repeater = cpu_to_le32(repeater);
	r->encrypted = cpu_to_le32(encrypted);
	r->authenticated = cpu_to_le32(encrypted);
}

#endif /* __APPLE_DCP_HDCP_WIRE_H__ */
