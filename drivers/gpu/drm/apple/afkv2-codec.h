/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/* Copyright 2026 Aurora Silicon */
/* Wire layout: public Linux PR254 and m1n1 AFKV2 implementations. */
#ifndef AURORA_AFKV2_CODEC_H
#define AURORA_AFKV2_CODEC_H

#include <linux/errno.h>
#include <linux/string.h>
#include <linux/types.h>

#define AFKV2_HEADER_SIZE 24U
#define AFKV2_COMMAND_SIZE 8U
#define AFKV2_OOB_SIZE 24U
#define AFKV2_MAX_PAYLOAD 1048576U
#define AFKV2_REPORT 0U
#define AFKV2_COMMAND 1U
#define AFKV2_RESPONSE 2U
#define AFKV2_OPEN 0x12U
#define AFKV2_CLOSE 0x13U
#define AFKV2_CLOSED 0x14U

struct afkv2_frame {
	u8 sequence;
	u8 transport_flags;
	u16 interface;
	u64 token;
	u8 type;
	u8 category;
	u16 interface_flags;
	u32 reserved;
	const u8 *payload;
	size_t payload_size;
	size_t padding_size;
};

struct afkv2_command {
	u8 id;
	u32 value;
	bool oob;
	u64 rx_address;
	u64 tx_address;
	u32 rx_size;
	u32 tx_size;
	const u8 *inline_data;
	size_t inline_size;
};

static inline u16 afkv2_get16(const u8 *p)
{
	return (u16)p[0] | (u16)p[1] << 8;
}

static inline u32 afkv2_get32(const u8 *p)
{
	return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24;
}

static inline u64 afkv2_get64(const u8 *p)
{
	return (u64)afkv2_get32(p) | (u64)afkv2_get32(p + 4) << 32;
}

static inline void afkv2_put16(u8 *p, u16 value)
{
	p[0] = value;
	p[1] = value >> 8;
}

static inline void afkv2_put32(u8 *p, u32 value)
{
	p[0] = value;
	p[1] = value >> 8;
	p[2] = value >> 16;
	p[3] = value >> 24;
}

static inline void afkv2_put64(u8 *p, u64 value)
{
	afkv2_put32(p, value);
	afkv2_put32(p + 4, value >> 32);
}

/* Decode metadata only. Never follow an address supplied by the peer. */
static inline int afkv2_decode_frame(const void *record, size_t size,
				     bool allow_padding, struct afkv2_frame *out)
{
	const u8 *p = record;
	u32 length;
	struct afkv2_frame result = {0};

	if (!record || !out || size < AFKV2_HEADER_SIZE)
		return -EINVAL;
	if (size > AFKV2_HEADER_SIZE + AFKV2_MAX_PAYLOAD)
		return -E2BIG;
	length = afkv2_get32(p + 4);
	if (length < 16 || length - 16 > AFKV2_MAX_PAYLOAD || length > size - 8)
		return -EINVAL;
	if (!allow_padding && length != size - 8)
		return -EINVAL;
	if (p[17] > AFKV2_RESPONSE)
		return -EPROTO;
	result.sequence = p[0];
	result.transport_flags = p[1];
	result.interface = afkv2_get16(p + 2);
	result.token = afkv2_get64(p + 8);
	result.type = p[16];
	result.category = p[17];
	result.interface_flags = afkv2_get16(p + 18);
	result.reserved = afkv2_get32(p + 20);
	result.payload = p + AFKV2_HEADER_SIZE;
	result.payload_size = length - 16;
	result.padding_size = size - 8 - length;
	*out = result;
	return 0;
}

/* Closed notification: flags occupy the low byte; the high byte is opaque. */
static inline bool afkv2_closed_report(const struct afkv2_frame *frame)
{
	return frame && !frame->token && frame->category == AFKV2_REPORT &&
		frame->type == AFKV2_CLOSED &&
		(frame->interface_flags & 0xff) == 1 && frame->payload_size == 1 &&
		frame->payload && !frame->payload[0];
}

static inline int afkv2_encode_frame(void *record, size_t capacity, u8 sequence,
				     u16 interface, u64 token, u8 type, u8 category,
				     u16 flags, const void *payload, size_t payload_size,
				     size_t *written)
{
	u8 *p = record;

	if (!record || !written || !interface || category > AFKV2_RESPONSE ||
	    (payload_size && !payload))
		return -EINVAL;
	if (payload_size > AFKV2_MAX_PAYLOAD)
		return -E2BIG;
	if (capacity < AFKV2_HEADER_SIZE || payload_size > capacity - AFKV2_HEADER_SIZE)
		return -ENOSPC;
	/* Move first so an in-buffer payload is safe even when it overlaps. */
	if (payload_size)
		memmove(p + AFKV2_HEADER_SIZE, payload, payload_size);
	memset(p, 0, AFKV2_HEADER_SIZE);
	p[0] = sequence;
	afkv2_put16(p + 2, interface);
	afkv2_put32(p + 4, payload_size + 16);
	afkv2_put64(p + 8, token);
	p[16] = type;
	p[17] = category;
	afkv2_put16(p + 18, flags);
	*written = AFKV2_HEADER_SIZE + payload_size;
	return 0;
}

static inline int afkv2_encode_open(void *record, size_t capacity, u8 sequence,
				    u16 interface, size_t *written)
{
	return afkv2_encode_frame(record, capacity, sequence, interface, 0,
				  AFKV2_OPEN, AFKV2_REPORT, 1, NULL, 0, written);
}

static inline bool afkv2_valid_span(u64 address, u32 size)
{
	if (!size)
		return !address;
	return address && size <= AFKV2_MAX_PAYLOAD &&
	       address <= ~(u64)0 - (size - 1);
}

static inline int afkv2_encode_oob_command(void *payload, size_t capacity,
					   u8 id, u64 rx_address, u32 rx_size,
					   u64 tx_address, u32 tx_size)
{
	u8 *p = payload;

	if (!payload || !afkv2_valid_span(rx_address, rx_size) ||
	    !afkv2_valid_span(tx_address, tx_size))
		return -EINVAL;
	if (capacity < AFKV2_COMMAND_SIZE + AFKV2_OOB_SIZE)
		return -ENOSPC;
	memset(p, 0, AFKV2_COMMAND_SIZE + AFKV2_OOB_SIZE);
	p[0] = 1;
	p[1] = id;
	afkv2_put32(p + 4, rx_size);
	afkv2_put64(p + 8, rx_address);
	afkv2_put64(p + 16, tx_address);
	afkv2_put32(p + 24, rx_size);
	afkv2_put32(p + 28, tx_size);
	return 0;
}

/* Inline requests carry a response capacity separately from their body. */
static inline int afkv2_encode_inline_command(void *payload, size_t capacity,
					      u8 id, const void *body,
					      size_t size, u32 response_capacity)
{
	u8 *p = payload;

	if (!payload || (size && !body) || size > AFKV2_MAX_PAYLOAD - AFKV2_COMMAND_SIZE ||
	    response_capacity > AFKV2_MAX_PAYLOAD)
		return -EINVAL;
	if (capacity < AFKV2_COMMAND_SIZE || size > capacity - AFKV2_COMMAND_SIZE)
		return -ENOSPC;
	if (size)
		memmove(p + AFKV2_COMMAND_SIZE, body, size);
	memset(p, 0, AFKV2_COMMAND_SIZE);
	p[1] = id;
	afkv2_put32(p + 4, response_capacity);
	return 0;
}

static inline int afkv2_decode_command(const void *payload, size_t size,
				       struct afkv2_command *out)
{
	const u8 *p = payload;
	struct afkv2_command result = {0};
	size_t header = AFKV2_COMMAND_SIZE;

	if (!payload || !out || size < header || p[0] & ~1U || afkv2_get16(p + 2))
		return -EINVAL;
	result.id = p[1];
	result.value = afkv2_get32(p + 4);
	result.oob = p[0] & 1;
	if (result.oob) {
		header += AFKV2_OOB_SIZE;
		if (size < header)
			return -EINVAL;
		result.rx_address = afkv2_get64(p + 8);
		result.tx_address = afkv2_get64(p + 16);
		result.rx_size = afkv2_get32(p + 24);
		result.tx_size = afkv2_get32(p + 28);
		/* A failed response may retain addresses with zero lengths. */
		if ((result.rx_size && !afkv2_valid_span(result.rx_address, result.rx_size)) ||
		    (result.tx_size && !afkv2_valid_span(result.tx_address, result.tx_size)))
			return -EINVAL;
	}
	result.inline_data = p + header;
	result.inline_size = size - header;
	if (result.inline_size > AFKV2_MAX_PAYLOAD)
		return -E2BIG;
	*out = result;
	return 0;
}

/* Requests and responses may have different owned-buffer capacities. */
static inline int afkv2_callback_lengths(const struct afkv2_command *command,
					 size_t minimum, size_t limit,
					 size_t *request, size_t *response)
{
	size_t in, out;

	if (!command || !request || !response || minimum > limit)
		return -EINVAL;
	in = command->oob ? command->tx_size : command->inline_size;
	out = command->oob ? command->rx_size : command->inline_size;
	if (in < minimum || out < minimum || in > limit || out > limit ||
	    in > command->value || out > command->value)
		return -EINVAL;
	*request = in;
	*response = out;
	return 0;
}

/* Correlate only; no buffer is read, written or freed by this helper. */
static inline int afkv2_check_response(const struct afkv2_frame *frame,
				       const struct afkv2_command *response,
				       u16 interface, u8 type, u8 id,
				       u64 rx_address, u32 rx_capacity,
				       u64 tx_address, u32 tx_capacity)
{
	if (!frame || !response || frame->category != AFKV2_RESPONSE ||
	    frame->interface != interface || frame->type != type || response->id != id)
		return -EPROTO;
	if (response->value)
		return 0;
	if (!response->oob || response->inline_size ||
	    response->rx_address != rx_address || response->tx_address != tx_address ||
	    response->rx_size > rx_capacity || response->tx_size > tx_capacity)
		return -EPROTO;
	return 0;
}

/* Kept separate so DPAV inline replies cannot loosen owned-DMA checks.
 * Transport/interface flags and the reserved word are opaque firmware data.
 */
static inline int afkv2_check_inline_response(const struct afkv2_frame *frame,
					      const struct afkv2_command *response,
					      u16 interface, u8 type, u8 id,
					      size_t capacity)
{
	if (!frame || !response || frame->category != AFKV2_RESPONSE ||
	    frame->interface != interface || frame->type != type || response->id != id ||
	    frame->token || response->oob || response->inline_size > capacity)
		return -EPROTO;
	return 0;
}

/* The standard service envelope precedes the actual, possibly short reply. */
static inline int afkv2_service_reply_size(const void *body, size_t received,
					   u16 group, u32 command, size_t capacity,
					   size_t *size)
{
	const u8 *p = body;
	u32 length;

	if (!body || !size || received < 64 || afkv2_get32(p + 12) != 0x69706378 ||
	    afkv2_get16(p + 2) != group || afkv2_get32(p + 4) != command)
		return -EPROTO;
	length = afkv2_get32(p + 8);
	if (length > received - 64 || length > capacity)
		return -EMSGSIZE;
	*size = length;
	return 0;
}

/* Reject a peer command without touching its advertised DMA buffers. */
static inline int afkv2_encode_error_reply(void *record, size_t capacity, u8 sequence,
					   const struct afkv2_frame *request,
					   const struct afkv2_command *command,
					   u32 status, size_t *written)
{
	u8 payload[AFKV2_COMMAND_SIZE + AFKV2_OOB_SIZE] = {0};
	size_t size = AFKV2_COMMAND_SIZE;

	if (!request || !command || request->category != AFKV2_COMMAND || !status)
		return -EINVAL;
	payload[0] = command->oob ? 1 : 0;
	payload[1] = command->id;
	afkv2_put32(payload + 4, status);
	if (command->oob) {
		afkv2_put64(payload + 8, command->rx_address);
		afkv2_put64(payload + 16, command->tx_address);
		size += AFKV2_OOB_SIZE;
	}
	return afkv2_encode_frame(record, capacity, sequence, request->interface,
				  request->token, request->type, AFKV2_RESPONSE, 0,
				  payload, size, written);
}

static inline u8 afkv2_command_id(u8 generation, u8 slot)
{
	return ((generation & 15) << 4) | (slot & 15);
}

static inline bool afkv2_owned_span(u64 base, size_t length, u64 address, size_t size)
{
	if (!length || base > ~(u64)0 - (length - 1))
		return false;
	return address >= base && size <= length && address - base <= length - size;
}

#endif
