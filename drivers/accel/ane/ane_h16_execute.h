/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef __ANE_H16_EXECUTE_H__
#define __ANE_H16_EXECUTE_H__

#include <linux/limits.h>

#include "ane_h16_protocol.h"

#define ANE_H16_EXECUTE_BASE_SIZE 0xca8
#define ANE_H16_BUFFER_OFFSET 0x60
#define ANE_H16_BUFFER_STRIDE 0x30
#define ANE_H16_MAX_BUFFERS 64
#define ANE_H16_CUSTOM_BAR_STRIDE 0x18
/* Conservative observed-version ceiling, not a firmware maximum. */
#define ANE_H16_MAX_CUSTOM_BARS 2

struct ane_h16_request_key {
	u64 uuid;
	u32 program;
	u32 process;
	u32 procedure;
};

/* Kernel-owned, validated process resources and pinned mapping geometry. */
struct ane_h16_execute_buffer {
	u32 index;
	u32 class;
	u64 mapping_dva;
	u64 mapping_size;
	u64 offset;
	u64 size;
};

/* Selectors and value interpretation must come from validated program metadata. */
struct ane_h16_custom_bar {
	u32 configuration;
	u32 value_type;
	u32 bar;
	u8 bit_offset;
	u8 bit_width;
	u16 flags;
	u64 value;
};

struct ane_h16_execute {
	struct ane_h16_request_key key;
	const struct ane_h16_execute_buffer *buffers;
	unsigned int buffer_count;
	const struct ane_h16_custom_bar *bars;
	unsigned int bar_count;
};

/*
 * Encode only after matching resources, permissions and BAR selectors to the
 * immutable loaded program. This helper checks geometry, not mapping ownership.
 * Inputs must remain immutable and must not alias the destination. On error the
 * destination is untouched. No mapping is published or released by this helper.
 */
static inline int
ane_h16_execute_encode(void *command, size_t length, const struct ane_h16_execute *call)
{
	u8 *p = command;
	unsigned int i, j;
	size_t expected;

	if (!p || !call || !call->buffers || !call->buffer_count ||
	    call->buffer_count > ANE_H16_MAX_BUFFERS ||
	    call->bar_count > ANE_H16_MAX_CUSTOM_BARS ||
	    (call->bar_count && !call->bars))
		return -EINVAL;
	expected = ANE_H16_EXECUTE_BASE_SIZE + call->bar_count * ANE_H16_CUSTOM_BAR_STRIDE;
	if (length != expected)
		return -EINVAL;
	for (i = 0; i < call->buffer_count; i++) {
		const struct ane_h16_execute_buffer *b = &call->buffers[i];

		if (!b->size || b->mapping_size > U64_MAX - b->mapping_dva ||
		    b->offset > b->mapping_size || b->size > b->mapping_size - b->offset)
			return -EINVAL;
	}
	for (i = 0; i < call->bar_count; i++) {
		const struct ane_h16_custom_bar *b = &call->bars[i];
		unsigned int end = b->bit_offset + b->bit_width;

		/* Conservative 64-bit field bound until wider fields are established. */
		if (!b->bit_width || end > 64)
			return -EINVAL;
		for (j = 0; j < i; j++) {
			const struct ane_h16_custom_bar *a = &call->bars[j];

			if (a->configuration == b->configuration && a->bar == b->bar &&
			    a->bit_offset < end &&
			    b->bit_offset < a->bit_offset + a->bit_width)
				return -EINVAL;
		}
	}

	memset(p, 0, length);
	put_unaligned_le16(ANE_H16_EXECUTE, p + 4);
	put_unaligned_le32(call->key.program, p + 8);
	put_unaligned_le32(call->key.process, p + 0xc);
	put_unaligned_le32(call->key.procedure, p + 0x10);
	put_unaligned_le32(5, p + 0x18);
	put_unaligned_le64(call->key.uuid, p + 0x20);
	put_unaligned_le32(call->buffer_count, p + 0x28);
	for (i = 0; i < call->buffer_count; i++) {
		const struct ane_h16_execute_buffer *b = &call->buffers[i];
		u8 *d = p + ANE_H16_BUFFER_OFFSET + i * ANE_H16_BUFFER_STRIDE;

		put_unaligned_le32(1, d);
		put_unaligned_le32(b->index, d + 4);
		put_unaligned_le32(b->class, d + 8);
		put_unaligned_le32(3, d + 0x10);
		put_unaligned_le64(b->mapping_dva + b->offset, d + 0x18);
		put_unaligned_le64(b->size, d + 0x20);
	}
	put_unaligned_le32(call->bar_count, p + 0xca0);
	for (i = 0; i < call->bar_count; i++) {
		const struct ane_h16_custom_bar *b = &call->bars[i];
		u8 *d = p + ANE_H16_EXECUTE_BASE_SIZE + i * ANE_H16_CUSTOM_BAR_STRIDE;

		put_unaligned_le32(b->configuration, d);
		put_unaligned_le32(b->value_type, d + 4);
		put_unaligned_le32(b->bar, d + 8);
		d[0xc] = b->bit_offset;
		d[0xd] = b->bit_width;
		put_unaligned_le16(b->flags, d + 0xe);
		put_unaligned_le64(b->value, d + 0x10);
	}
	return 0;
}

enum ane_h16_event_state {
	ANE_H16_EVENT_START,
	ANE_H16_EVENT_SUCCESS,
	ANE_H16_EVENT_FAILURE,
	ANE_H16_EVENT_HW_START,
};

struct ane_h16_event {
	struct ane_h16_request_key key;
	u32 state;
	u32 cpu_load;
	u64 timestamp;
	u8 destination;
};

/* Parse a bounded host-owned snapshot. Diagnostic words are not predicates. */
static inline int
ane_h16_event_decode(const void *event, size_t length, unsigned int channel,
		     struct ane_h16_event *decoded)
{
	const u8 *p = event;
	struct ane_h16_event e = {};

	if (!decoded || ane_h16_event_report_status(event, length, channel))
		return -EPROTO;
	e.key.uuid = get_unaligned_le64(p + 8);
	e.key.program = get_unaligned_le32(p + 0x10);
	e.key.process = get_unaligned_le32(p + 0x14);
	e.key.procedure = get_unaligned_le32(p + 0x18);
	e.state = get_unaligned_le32(p + 0x1c);
	e.cpu_load = get_unaligned_le32(p + 0x20);
	e.timestamp = get_unaligned_le64(p + 0x28);
	e.destination = p[0x30];
	if (e.state > ANE_H16_EVENT_HW_START || e.destination > 1)
		return -EPROTO;
	*decoded = e;
	return 0;
}

enum ane_h16_event_result {
	ANE_H16_EVENT_PROGRESS,
	ANE_H16_EVENT_COMPLETE,
	ANE_H16_EVENT_FAILED,
};

/*
 * For the general scheduler only. Look up an outstanding UUID before calling;
 * remove it atomically on terminal state to reject duplicates and late reports.
 * COMPLETE/FAILED classify an event, not permission to unmap: DMA sync and
 * failure quarantine remain the caller's responsibility. Never call on ch7 ACK.
 */
static inline int
ane_h16_event_result(const void *event, size_t length, unsigned int channel,
		     const struct ane_h16_request_key *pending)
{
	struct ane_h16_event e;
	int ret;

	if (!pending)
		return -EINVAL;
	ret = ane_h16_event_decode(event, length, channel, &e);
	if (ret)
		return ret;
	if (e.destination)
		return -ENOMSG;
	if (e.key.uuid != pending->uuid || e.key.program != pending->program ||
	    e.key.process != pending->process || e.key.procedure != pending->procedure)
		return -EPROTO;
	if (e.state == ANE_H16_EVENT_SUCCESS)
		return ANE_H16_EVENT_COMPLETE;
	if (e.state == ANE_H16_EVENT_FAILURE)
		return ANE_H16_EVENT_FAILED;
	return ANE_H16_EVENT_PROGRESS;
}

#endif /* __ANE_H16_EXECUTE_H__ */
