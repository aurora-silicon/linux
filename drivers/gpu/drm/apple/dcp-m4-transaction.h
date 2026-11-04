/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef APPLE_DCP_M4_TRANSACTION_H
#define APPLE_DCP_M4_TRANSACTION_H

#include <linux/string.h>
#include "dcp-m4-protocol.h"

/*
 * Pure ownership core: one serialized owner, not called from the mailbox IRQ.
 * The transport must queue notifications in order, synchronize DMA before reads
 * and before sends, enforce deadlines, and latch queue/send/firmware failures.
 * Buffers and state must remain pinned after failure; resetting this structure
 * is not firmware recovery. No timeout, MMIO or mailbox operation happens here.
 */
#define DCP_M4_CALL_DEPTH 2
#define DCP_M4_CALLBACK_DEPTH 8

struct dcp_m4_call {
	struct dcp_m4_packet packet;
	unsigned int callbacks;
};

struct dcp_m4_callback {
	struct dcp_m4_packet packet;
	unsigned int calls;
};

struct dcp_m4_transaction {
	struct dcp_m4_call call[DCP_M4_CALL_DEPTH];
	struct dcp_m4_callback callback[DCP_M4_CALLBACK_DEPTH];
	unsigned int calls;
	unsigned int callbacks;
	int error;
	bool external;	/* DCPEXT profile: streams 0..3 and any flagged callback route */
	bool oob;	/* an AP call is outstanding on stream 1 */
};

static inline int dcp_m4_transaction_decode(const struct dcp_m4_transaction *state, u64 word,
					    struct dcp_m4_packet *packet)
{
	if (state->external)
		return dcp_m4_decode_word_ext(word, packet);
	return state->oob ? dcp_m4_decode_word_oob(word, packet) :
			    dcp_m4_decode_word(word, packet);
}

static inline int dcp_m4_transaction_fail(struct dcp_m4_transaction *state, int error)
{
	if (!state->error)
		state->error = error < 0 ? error : -EPROTO;
	return state->error;
}

static inline bool dcp_m4_tag(u32 tag, u8 direction)
{
	if ((tag & 0xff) != direction)
		return false;
	for (unsigned int shift = 8; shift < 32; shift += 8)
		if (((tag >> shift) & 0xff) < '0' || ((tag >> shift) & 0xff) > '9')
			return false;
	return true;
}

/* Prepare only; the caller publishes with its DMA barrier and mailbox send. */
/* Stream 1 is out of band: top-level calls only, such as setPowerState. */
static inline int dcp_m4_call_begin_stream(struct dcp_m4_transaction *state, void *rpc,
					   size_t rpc_size, u32 tag, u8 stream, bool remote,
					   const void *input, u32 input_size, u32 output_size,
					   u64 *word)
{
	struct dcp_m4_packet packet = {
		.tag = tag, .input_size = input_size, .output_size = output_size,
		.remote = remote, .stream = stream,
	};
	const struct dcp_m4_packet *outer = NULL;
	u8 *data;
	int ret, offset;

	if (state->error)
		return state->error;
	if (!rpc || rpc_size != DCP_M4_RPC_SIZE || !word || (input_size && !input) ||
	    !dcp_m4_tag(tag, 'A') || state->calls >= DCP_M4_CALL_DEPTH)
		return dcp_m4_transaction_fail(state, -EINVAL);
	if (stream > 1 || (stream && (state->calls || state->callbacks || state->external)))
		return dcp_m4_transaction_fail(state, -EINVAL);
	if (state->calls) {
		struct dcp_m4_call *pending = &state->call[state->calls - 1];

		/* Merely queuing a callback does not grant nested-call ownership. */
		if (state->callbacks <= pending->callbacks)
			return dcp_m4_transaction_fail(state, -EBUSY);
		outer = &pending->packet;
	}
	if (state->callbacks &&
	    remote != state->callback[state->callbacks - 1].packet.remote)
		return dcp_m4_transaction_fail(state, -EPROTO);
	/*
	 * AP calls use stream 0; a callback on another stream cannot recurse,
	 * except that calls nested in an out-of-band callback stay out of band.
	 */
	if (state->callbacks && state->callback[state->callbacks - 1].packet.stream) {
		if (!state->oob || state->callback[state->callbacks - 1].packet.stream != 1)
			return dcp_m4_transaction_fail(state, -EOPNOTSUPP);
		stream = 1;
		packet.stream = stream;
	}
	ret = dcp_m4_packet_size(input_size, output_size, &packet.length);
	if (ret)
		return dcp_m4_transaction_fail(state, ret);
	ret = dcp_m4_nested_offset(outer, remote, packet.length, &packet.offset);
	if (ret)
		return dcp_m4_transaction_fail(state, ret);
	offset = dcp_m4_packet_offset(&packet, false);
	if (offset < 0)
		return dcp_m4_transaction_fail(state, offset);
	data = (u8 *)rpc + offset;
	/* Check before writing the header: input must not alias this AP packet. */
	if (input_size) {
		unsigned long src = (unsigned long)input;
		unsigned long dst = (unsigned long)data;

		if (src > ~0UL - input_size ||
		    (src < dst + packet.length && dst < src + input_size))
			return dcp_m4_transaction_fail(state, -EINVAL);
	}
	put_unaligned_be32(tag, data);
	put_unaligned_le32(input_size, data + 4);
	put_unaligned_le32(output_size, data + 8);
	if (input_size)
		memcpy(data + DCP_M4_PACKET_HEADER, input, input_size);
	memset(data + DCP_M4_PACKET_HEADER + input_size, 0, output_size);
	state->call[state->calls++] = (struct dcp_m4_call) {
		.packet = packet, .callbacks = state->callbacks,
	};
	*word = 2 | (remote ? 0x100 : 0) | ((u64)stream << 10) | ((u64)packet.offset << 16) |
		((u64)packet.length << 32);
	if (stream)
		state->oob = true;
	return 0;
}

static inline int dcp_m4_call_begin(struct dcp_m4_transaction *state, void *rpc,
				    size_t rpc_size, u32 tag, bool remote,
				    const void *input, u32 input_size, u32 output_size,
				    u64 *word)
{
	return dcp_m4_call_begin_stream(state, rpc, rpc_size, tag, 0, remote, input,
					input_size, output_size, word);
}

/* Firmware may write output, but must not change the owned packet header. */
static inline int dcp_m4_owned_packet(const void *rpc, const struct dcp_m4_packet *owned,
				    bool callback)
{
	struct dcp_m4_packet actual = *owned;
	int offset = dcp_m4_packet_offset(owned, callback);
	int ret;

	if (offset < 0)
		return offset;
	ret = dcp_m4_decode_packet((const u8 *)rpc + offset, owned->length, &actual);
	if (ret)
		return ret;
	if (actual.tag != owned->tag || actual.input_size != owned->input_size ||
	    actual.output_size != owned->output_size)
		return -EPROTO;
	return offset;
}

/* Accept only the innermost matching reply; output stays in its original AP slot. */
static inline int dcp_m4_call_reply(struct dcp_m4_transaction *state, const void *rpc,
				  size_t rpc_size, u64 word, void *output, u32 output_size)
{
	struct dcp_m4_packet reply;
	struct dcp_m4_call *call;
	int ret, offset;

	if (state->error)
		return state->error;
	if (!rpc || rpc_size != DCP_M4_RPC_SIZE || !state->calls)
		return dcp_m4_transaction_fail(state, -EPROTO);
	ret = dcp_m4_transaction_decode(state, word, &reply);
	if (ret)
		return dcp_m4_transaction_fail(state, ret);
	call = &state->call[state->calls - 1];
	if (!reply.reply || reply.remote != call->packet.remote ||
	    reply.stream != call->packet.stream ||
	    state->callbacks != call->callbacks || output_size != call->packet.output_size ||
	    (output_size && !output))
		return dcp_m4_transaction_fail(state, -EPROTO);
	offset = dcp_m4_owned_packet(rpc, &call->packet, false);
	if (offset < 0)
		return dcp_m4_transaction_fail(state, offset);
	if (output_size)
		memcpy(output, (const u8 *)rpc + offset + DCP_M4_PACKET_HEADER +
		       call->packet.input_size, output_size);
	/* The out-of-band call itself, not a call nested in its callbacks. */
	if (call->packet.stream && state->calls == 1)
		state->oob = false;
	state->calls--;
	return 0;
}

/* Called when the serialized owner begins handling a queued callback. */
static inline int dcp_m4_callback_begin(struct dcp_m4_transaction *state, void *rpc,
				      size_t rpc_size, u64 word, struct dcp_m4_packet *out)
{
	struct dcp_m4_packet packet;
	u8 *data;
	int ret, offset;

	if (state->error)
		return state->error;
	if (!rpc || rpc_size != DCP_M4_RPC_SIZE || !out ||
	    state->callbacks >= DCP_M4_CALLBACK_DEPTH)
		return dcp_m4_transaction_fail(state, -EINVAL);
	ret = dcp_m4_transaction_decode(state, word, &packet);
	if (ret)
		return dcp_m4_transaction_fail(state, ret);
	offset = dcp_m4_packet_offset(&packet, true);
	if (offset < 0)
		return dcp_m4_transaction_fail(state, offset);
	for (unsigned int i = 0; i < state->callbacks; i++) {
		const struct dcp_m4_packet *active = &state->callback[i].packet;

		if (active->remote == packet.remote && active->stream == packet.stream &&
		    packet.offset < active->offset + active->length &&
		    active->offset < packet.offset + packet.length)
			return dcp_m4_transaction_fail(state, -EPROTO);
	}
	data = (u8 *)rpc + offset;
	ret = dcp_m4_decode_packet(data, packet.length, &packet);
	if (ret)
		return dcp_m4_transaction_fail(state, ret);
	if (!dcp_m4_tag(packet.tag, 'D'))
		return dcp_m4_transaction_fail(state, -EPROTO);
	/* Bit 9 only marks a new-stream notification route; it says nothing about
	 * the method. Firmware sends any callback that way (e.g. the hourly D114
	 * CoreAnalyticsSendEvent), so admission is left to the exact-shape
	 * dispatch table, as for unflagged callbacks.
	 */
	memset(data + DCP_M4_PACKET_HEADER + packet.input_size, 0, packet.output_size);
	state->callback[state->callbacks++] = (struct dcp_m4_callback) {
		.packet = packet, .calls = state->calls,
	};
	*out = packet;
	return 0;
}

/* Caller must publish handler output before sending the returned ACK word. */
static inline int dcp_m4_callback_end(struct dcp_m4_transaction *state, const void *rpc,
				    size_t rpc_size, u64 *word)
{
	struct dcp_m4_callback *callback;
	int ret;

	if (state->error)
		return state->error;
	if (!rpc || rpc_size != DCP_M4_RPC_SIZE || !word || !state->callbacks)
		return dcp_m4_transaction_fail(state, -EPROTO);
	callback = &state->callback[state->callbacks - 1];
	if (state->calls != callback->calls)
		return dcp_m4_transaction_fail(state, -EBUSY);
	ret = dcp_m4_owned_packet(rpc, &callback->packet, true);
	if (ret < 0)
		return dcp_m4_transaction_fail(state, ret);
	ret = dcp_m4_callback_ack(&callback->packet, word);
	if (ret)
		return dcp_m4_transaction_fail(state, ret);
	state->callbacks--;
	return 0;
}

#endif
