/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef APPLE_DCP_M4_PROTOCOL_H
#define APPLE_DCP_M4_PROTOCOL_H

#include <linux/errno.h>
#include <linux/types.h>
#include <linux/unaligned.h>
#include "dcp-m4-profile.h"

/* J713 firmware 81BAF476-679D-3CC0-8C6A-E6B6FA659D5E only. */
#define DCP_M4_RPC_SIZE		0xc0000U
#define DCP_M4_STREAM_SIZE	0x8000U
#define DCP_M4_ALIGNMENT		64U
#define DCP_M4_READY_HASH		0x45bc9ef4U
#define DCP_M4_PACKET_HEADER	12U
#define DCP_M4_ENDPOINT		0x37U

struct dcp_m4_packet {
	u32 tag;
	u32 input_size;
	u32 output_size;
	u16 offset;
	u16 length;
	u8 stream;
	bool remote;
	bool reply;
	bool callback_flag;
};

static inline int dcp_m4_init_word(u64 address, u64 *word)
{
	if (!address || address >> 48 || address % DCP_M4_ALIGNMENT)
		return -EINVAL;
	*word = (address << 16) | 0x40;
	return 0;
}

static inline bool dcp_m4_ready(u64 word)
{
	/* Firmware sets the remote flag; it is not part of the identity. */
	return (word & 0xfeff) == 1 &&
		((word >> 16) & 0xffffffffULL) == DCP_M4_READY_HASH &&
		(word >> 48) == DCP_M4_ALIGNMENT;
}

/* Return zero for firmware-init, one for a fresh READY, or a protocol error. */
static inline int dcp_m4_ready_notification(u64 word, bool init_sent, u64 previous)
{
	if (previous)
		return -EPROTO;
	if (!(word & 0xc3))
		return 0;
	return init_sent && dcp_m4_ready(word) ? 1 : -EPROTO;
}

/* This profile admits stream zero only, not the older context-ID encoding. */
static inline int dcp_m4_decode_word_ext(u64 word, struct dcp_m4_packet *packet);

static inline int dcp_m4_decode_word(u64 word, struct dcp_m4_packet *packet)
{
	u16 control = word;

	u16 offset = word >> 16;
	u16 length = word >> 32;

	if (word >> 48 || (control & ~0x342U) || (control & 3) != 2)
		return -EPROTO;
	/* Qualified only on incoming remote-1 callbacks, never AP replies. */
	if ((control & 0x200) && control != 0x302)
		return -EPROTO;
	/* Replies carry routing only; output remains in the outstanding AP packet. */
	if (control & 0x40) {
		if (offset || length)
			return -EPROTO;
	} else if (offset % DCP_M4_ALIGNMENT || offset >= DCP_M4_STREAM_SIZE ||
		   length < DCP_M4_PACKET_HEADER ||
		   length > DCP_M4_STREAM_SIZE - offset) {
		return -EMSGSIZE;
	}
	*packet = (struct dcp_m4_packet) {
		.offset = offset,
		.length = length,
		.remote = !!(control & 0x100),
		.reply = !!(control & 0x40),
		.callback_flag = !!(control & 0x200),
	};
	return 0;
}

/*
 * External (DCPEXT) profile: incoming callbacks may use streams 0..3 and any
 * remote/bit-9 route; AP replies stay on stream 0 and carry routing only.
 */
static inline int dcp_m4_decode_word_ext(u64 word, struct dcp_m4_packet *packet)
{
	u16 control = word;
	u16 offset = word >> 16;
	u16 length = word >> 32;
	u8 stream = control >> 10;

	if (word >> 48 || (control & 0x00bc) || (control & 3) != 2 || stream >= 4)
		return -EPROTO;
	if (control & 0x40) {
		if (offset || length || stream || (control & 0x200))
			return -EPROTO;
	} else if (offset % DCP_M4_ALIGNMENT || offset >= DCP_M4_STREAM_SIZE ||
		   length < DCP_M4_PACKET_HEADER ||
		   length > DCP_M4_STREAM_SIZE - offset) {
		return -EMSGSIZE;
	}
	*packet = (struct dcp_m4_packet) {
		.offset = offset,
		.length = length,
		.stream = stream,
		.remote = !!(control & 0x100),
		.reply = !!(control & 0x40),
		.callback_flag = !!(control & 0x200),
	};
	return 0;
}

/*
 * While an AP call is outstanding on stream 1 (bit 10, the older contexts'
 * out-of-band +4), its reply and callbacks come on stream 1 as well, besides
 * any stream-0 traffic.
 */
static inline int dcp_m4_decode_word_oob(u64 word, struct dcp_m4_packet *packet)
{
	u8 stream = (word >> 10) & 3;
	int ret;

	if (!stream)
		return dcp_m4_decode_word(word, packet);
	if (stream != 1)
		return -EPROTO;
	ret = dcp_m4_decode_word(word & ~0x400ULL, packet);
	if (!ret)
		packet->stream = 1;
	return ret;
}

/*
 * Replies require their pending request record, not a new packet location.
 * Callbacks may name a nonzero stream (external profile, or stream 1 during
 * an out-of-band call); AP calls use stream 0, or stream 1 out of band.
 */
static inline int dcp_m4_packet_offset(const struct dcp_m4_packet *packet, bool callback)
{
	unsigned int base;

	if (packet->reply || (callback ? packet->stream >= 4 : packet->stream > 1) ||
	    (packet->callback_flag && (!callback || !packet->remote)) ||
	    packet->offset % DCP_M4_ALIGNMENT ||
	    packet->offset >= DCP_M4_STREAM_SIZE || packet->length < DCP_M4_PACKET_HEADER ||
	    packet->length > DCP_M4_STREAM_SIZE - packet->offset)
		return -EINVAL;
	base = callback ? (packet->remote ? 0x40000 : 0x60000) :
			  (packet->remote ? 0x20000 : 0);
	return base + packet->stream * DCP_M4_STREAM_SIZE + packet->offset;
}

static inline int dcp_m4_callback_ack(const struct dcp_m4_packet *packet, u64 *word)
{
	if (dcp_m4_packet_offset(packet, true) < 0)
		return -EINVAL;
	/* Preserve the incoming route: stream, remote and bit 9. */
	*word = 0x42 | (packet->remote ? 0x100 : 0) | (packet->callback_flag ? 0x200 : 0) |
		((u64)packet->stream << 10);
	return 0;
}

static inline int dcp_m4_nested_offset(const struct dcp_m4_packet *outer,
				       bool remote, u16 length, u16 *offset)
{
	unsigned int next = 0;

	if (outer) {
		if (dcp_m4_packet_offset(outer, false) < 0)
			return -EINVAL;
		if (outer->remote == remote)
			next = (outer->offset + outer->length + 63U) & ~63U;
	}
	if (length < DCP_M4_PACKET_HEADER || next >= DCP_M4_STREAM_SIZE ||
	    length > DCP_M4_STREAM_SIZE - next)
		return -EMSGSIZE;
	*offset = next;
	return 0;
}

/* Validate lengths before exposing either input or output to a handler. */
static inline int dcp_m4_decode_packet(const void *data, size_t available,
				       struct dcp_m4_packet *packet)
{
	const u8 *bytes = data;
	u32 input, output, payload;
	unsigned int i;

	if (available < DCP_M4_PACKET_HEADER || available != packet->length)
		return -EMSGSIZE;
	input = get_unaligned_le32(bytes + 4);
	output = get_unaligned_le32(bytes + 8);
	payload = packet->length - DCP_M4_PACKET_HEADER;
	if (input > payload || output != payload - input)
		return -EMSGSIZE;
	/* Remote selects a stream half, not the transaction's direction. */
	/* Method FourCC is reversed on the wire: A457 is bytes "754A".
	 * Keep logical tags with A/D in the low byte for the ownership helpers.
	 * Input/output lengths remain little-endian; they are not FourCCs.
	 */
	if (bytes[3] != 'A' && bytes[3] != 'D')
		return -EPROTO;
	for (i = 0; i < 3; i++)
		if (bytes[i] < '0' || bytes[i] > '9')
			return -EPROTO;
	packet->tag = get_unaligned_be32(bytes);
	packet->input_size = input;
	packet->output_size = output;
	return 0;
}

static inline int dcp_m4_packet_size(u32 input, u32 output, u16 *size)
{
	if (input > DCP_M4_STREAM_SIZE - DCP_M4_PACKET_HEADER ||
	    output > DCP_M4_STREAM_SIZE - DCP_M4_PACKET_HEADER - input)
		return -EMSGSIZE;
	*size = DCP_M4_PACKET_HEADER + input + output;
	return 0;
}

#endif
