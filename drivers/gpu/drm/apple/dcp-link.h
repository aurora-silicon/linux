/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/* Copyright The Asahi Linux Contributors */

#ifndef __APPLE_DCP_LINK_H__
#define __APPLE_DCP_LINK_H__

#include <linux/bitfield.h>
#include <linux/bits.h>
#include <linux/byteorder/generic.h>
#include <linux/errno.h>
#include <linux/log2.h>
#include <linux/overflow.h>
#include <linux/types.h>

#define APPLE_DCP_LINK_PROTOCOL_VERSION		4
#define APPLE_DCP_LINK_ENDPOINT			0x37
#define APPLE_DCP_LINK_STREAM_COUNT		8
#define APPLE_DCP_LINK_STREAMS_PER_SIDE		4
#define APPLE_DCP_LINK_STREAM_STATE_SIZE	0x68
#define APPLE_DCP_LINK_STATE_SIZE		0x3d0
#define APPLE_DCP_LINK_LOCAL_STREAMS_OFFSET	0x078
#define APPLE_DCP_LINK_REMOTE_STREAMS_OFFSET	0x218
#define APPLE_DCP_LINK_RPC_MEMORY_SIZE		0x0c0000
#define APPLE_DCP_LINK_RPC_PAGE_SIZE		0x004000
#define APPLE_DCP_LINK_SIDE_MEMORY_SIZE		0x040000
#define APPLE_DCP_LINK_STREAM_HALF_SIZE		0x020000
#define APPLE_DCP_LINK_STREAM_BUFFER_SIZE	0x008000
#define APPLE_DCP_LINK_HEAP_OFFSET		0x080000
#define APPLE_DCP_LINK_HEAP_SIZE		0x040000
#define APPLE_DCP_LINK_RPC_ALIGNMENT		4
#define APPLE_DCP_LINK_SHARED_VAR_MEMORY_SIZE	0x4000000

#define APPLE_DCP_LINK_INIT_FLAG_64M		0x04000000

#define APPLE_DCP_LINK_MSG_FIRMWARE_INIT	0
#define APPLE_DCP_LINK_MSG_READY		1
#define APPLE_DCP_LINK_MSG_RPC			2
#define APPLE_DCP_LINK_MSG_RPC_REPLY		0x42
#define APPLE_DCP_LINK_MSG_INIT			0x40
#define APPLE_DCP_LINK_MSG_REMOTE		BIT_ULL(8)
#define APPLE_DCP_LINK_MSG_NESTED		BIT_ULL(9)
#define APPLE_DCP_LINK_MSG_STREAM		GENMASK_ULL(15, 10)
#define APPLE_DCP_LINK_MSG_OFFSET		GENMASK_ULL(31, 16)
#define APPLE_DCP_LINK_MSG_SIZE			GENMASK_ULL(47, 32)
#define APPLE_DCP_LINK_MSG_HASH			GENMASK_ULL(47, 16)
#define APPLE_DCP_LINK_MSG_VERSION		GENMASK_ULL(63, 48)
#define APPLE_DCP_LINK_REPLY_CONTROL_MASK	0xff3e

#define APPLE_DCP_LINK_CALL_D003		0x44303033
#define APPLE_DCP_H17P_CLOCK_ID_154		0x154
#define APPLE_DCP_H17P_CLOCK_ID_194		0x194

enum apple_dcp_link_state {
	APPLE_DCP_LINK_WAIT_READY,
	APPLE_DCP_LINK_READY,
};

enum apple_dcp_link_handoff_phase {
	APPLE_DCP_LINK_HANDOFF_NONE,
	APPLE_DCP_LINK_HANDOFF_FIRMWARE_INIT,
	APPLE_DCP_LINK_HANDOFF_READY,
};

enum apple_dcp_link_adopt_action {
	APPLE_DCP_LINK_ADOPT_INVALID,
	APPLE_DCP_LINK_ADOPT_START_ENDPOINT,
	APPLE_DCP_LINK_ADOPT_SEND_INIT,
	APPLE_DCP_LINK_ADOPT_START_IOMFB,
};

static inline bool
apple_dcp_link_handoff_message_valid(enum apple_dcp_link_handoff_phase phase, u64 message)
{
	u8 type = message & GENMASK_ULL(1, 0);

	if (phase == APPLE_DCP_LINK_HANDOFF_FIRMWARE_INIT)
		return type == APPLE_DCP_LINK_MSG_FIRMWARE_INIT &&
		       !(message & GENMASK_ULL(7, 6));
	if (phase == APPLE_DCP_LINK_HANDOFF_READY)
		return type == APPLE_DCP_LINK_MSG_READY;

	return false;
}

static inline enum apple_dcp_link_adopt_action
apple_dcp_link_adopt_decide(enum apple_dcp_link_handoff_phase phase,
			    u64 message, bool descriptor_zero,
			    bool descriptor_valid, bool streams_idle)
{
	if (!streams_idle)
		return APPLE_DCP_LINK_ADOPT_INVALID;

	switch (phase) {
	case APPLE_DCP_LINK_HANDOFF_NONE:
		return message == 0 && descriptor_zero ?
			APPLE_DCP_LINK_ADOPT_START_ENDPOINT :
			APPLE_DCP_LINK_ADOPT_INVALID;
	case APPLE_DCP_LINK_HANDOFF_FIRMWARE_INIT:
		return (descriptor_zero || descriptor_valid) &&
		       apple_dcp_link_handoff_message_valid(phase, message) ?
			APPLE_DCP_LINK_ADOPT_SEND_INIT :
			APPLE_DCP_LINK_ADOPT_INVALID;
	case APPLE_DCP_LINK_HANDOFF_READY:
		return descriptor_valid &&
		       apple_dcp_link_handoff_message_valid(phase, message) ?
			APPLE_DCP_LINK_ADOPT_START_IOMFB :
			APPLE_DCP_LINK_ADOPT_INVALID;
	default:
		return APPLE_DCP_LINK_ADOPT_INVALID;
	}
}

static inline u16 apple_dcp_link_negotiate_version(u16 remote_version)
{
	return remote_version < APPLE_DCP_LINK_PROTOCOL_VERSION ?
		remote_version : APPLE_DCP_LINK_PROTOCOL_VERSION;
}

enum apple_dcp_link_handshake_action {
	APPLE_DCP_LINK_HANDSHAKE_NONE,
	APPLE_DCP_LINK_HANDSHAKE_START_IOMFB,
};

enum apple_dcp_h17p_callback {
	APPLE_DCP_H17P_CB_DID_BOOT = 0x44303030,
	APPLE_DCP_H17P_CB_DID_POWER_ON = 0x44303031,
	APPLE_DCP_H17P_CB_WILL_POWER_OFF = 0x44303032,
	APPLE_DCP_H17P_CB_RT_BANDWIDTH = APPLE_DCP_LINK_CALL_D003,
	APPLE_DCP_H17P_CB_FRAME_SYNC = 0x44303036,
	APPLE_DCP_H17P_CB_MATCH_PMU = 0x44313030,
	APPLE_DCP_H17P_CB_START_HARDWARE_BOOT = 0x44313230,
	APPLE_DCP_H17P_CB_ALLOCATE_BANDWIDTH = 0x44313239,
	APPLE_DCP_H17P_CB_MAP_BUF = 0x44323031,
	APPLE_DCP_H17P_CB_UNMAP_BUF = 0x44323032,
	APPLE_DCP_H17P_CB_MATCH_PMU_2 = 0x44323036,
	APPLE_DCP_H17P_CB_MATCH_BACKLIGHT = 0x44323037,
	APPLE_DCP_H17P_CB_GET_TIME = 0x44323039,
	APPLE_DCP_H17P_CB_PROP_PUBLISH = 0x44333030,
	APPLE_DCP_H17P_CB_GET_UINT_PROP = 0x44343031,
	APPLE_DCP_H17P_CB_GET_FREQUENCY = 0x44343038,
	APPLE_DCP_H17P_CB_MAP_REG = 0x44343131,
	APPLE_DCP_H17P_CB_SET_PROPERTY_INT = 0x44343134,
	APPLE_DCP_H17P_CB_SET_PROPERTY_BOOL = 0x44343135,
	APPLE_DCP_H17P_CB_ALLOCATE_BUFFER = 0x44343531,
	APPLE_DCP_H17P_CB_MAP_PHYSICAL = 0x44343532,
	APPLE_DCP_H17P_CB_RELEASE_MEM_DESC = 0x44343534,
	APPLE_DCP_H17P_CB_POWER_UP_DART = 0x44353732,
	APPLE_DCP_H17P_CB_HOTPLUG = 0x44353735,
	APPLE_DCP_H17P_CB_POWERSTATE_NOTIFY = 0x44353736,
	APPLE_DCP_H17P_CB_CREATE_DEFAULT_FB = 0x44353836,
	APPLE_DCP_H17P_CB_CLEAR_DEFAULT_FB = 0x44353837,
	APPLE_DCP_H17P_CB_SWAP_NOTIFY = 0x44353838,
	APPLE_DCP_H17P_CB_SWAP_INFO = 0x44353839,
	APPLE_DCP_H17P_CB_SWAP_COMPLETE = 0x44353934,
	APPLE_DCP_H17P_CB_BATCHED_SWAP_COMPLETE = 0x44353935,
	APPLE_DCP_H17P_CB_SWAP_COMPLETE_INTENT = 0x44353936,
	APPLE_DCP_H17P_CB_ABORT_SWAP = 0x44353937,
	APPLE_DCP_H17P_CB_ENABLE_BACKLIGHT = 0x44353938,
};

enum apple_dcp_h17p_call {
	APPLE_DCP_H17P_CALL_LATE_INIT = 0x41303030,
	APPLE_DCP_H17P_CALL_SETUP_VIDEO_LIMITS = 0x41303234,
	APPLE_DCP_H17P_CALL_UP_SET_CREATE_DFB = 0x41333839,
	APPLE_DCP_H17P_CALL_VI_SET_TEMPERATURE_HINT = 0x41333930,
	APPLE_DCP_H17P_CALL_START_SIGNAL = 0x41343031,
	APPLE_DCP_H17P_CALL_SWAP_START = 0x41343036,
	APPLE_DCP_H17P_CALL_SWAP_SUBMIT = 0x41343037,
	APPLE_DCP_H17P_CALL_SET_DISPLAY_DEVICE = 0x41343131,
	APPLE_DCP_H17P_CALL_IS_MAIN_DISPLAY = 0x41343132,
	APPLE_DCP_H17P_CALL_SET_DIGITAL_OUT_MODE = 0x41343133,
	APPLE_DCP_H17P_CALL_SET_MATRIX = 0x41343233,
	APPLE_DCP_H17P_CALL_SET_PARAMETER = 0x41343439,
	APPLE_DCP_H17P_CALL_CREATE_DEFAULT_FB = 0x41343533,
	APPLE_DCP_H17P_CALL_VIDEO_POWER_SAVINGS = 0x41343537,
	APPLE_DCP_H17P_CALL_FIRST_CLIENT_OPEN = 0x41343635,
	APPLE_DCP_H17P_CALL_LAST_CLIENT_CLOSE = 0x41343636,
	APPLE_DCP_H17P_CALL_DISPLAY_REFRESH = 0x41343734,
	APPLE_DCP_H17P_CALL_FLUSH_SUPPORTS_POWER = 0x41343737,
	APPLE_DCP_H17P_CALL_ABORT_SWAPS = 0x41343738,
	APPLE_DCP_H17P_CALL_SET_POWER_STATE = 0x41343833,
};

struct apple_dcp_link_method {
	u32 call;
	u32 input_size;
	u32 output_size;
};

enum apple_dcp_link_side {
	APPLE_DCP_LINK_SIDE_AP = 0,
	APPLE_DCP_LINK_SIDE_DCP = 1,
};

struct apple_dcp_link_stream_layout {
	u32 local_offset;
	u32 remote_offset;
	u32 capacity;
};

struct apple_dcp_link_init_desc {
	__le64 shared_dva;
	__le32 flags;
	__le32 version;
} __packed;

struct apple_dcp_link_rpc_header {
	__le32 call;
	__le32 input_size;
	__le32 output_size;
} __packed;

struct apple_dcp_h17p_clock_request {
	__le32 service;
	__le32 clock_id;
} __packed;

struct apple_dcp_h17p_rt_bw_config {
	__le64 scratch;
	__le64 clock_request;
} __packed;

struct apple_dcp_h17p_rt_bw_request {
	__le32 config_null;
} __packed;

struct apple_dcp_h17p_rt_bw_reply {
	struct apple_dcp_h17p_rt_bw_config config;
	__le32 status;
} __packed;

struct apple_dcp_link_rpc_view {
	u32 call;
	u32 input_size;
	u32 output_size;
	u32 offset;
	u32 total_size;
	const void *input;
	void *output;
};

static_assert(sizeof(struct apple_dcp_link_init_desc) == 0x10);
static_assert(sizeof(struct apple_dcp_link_rpc_header) == 0x0c);
static_assert(sizeof(struct apple_dcp_h17p_clock_request) == 0x08);
static_assert(sizeof(struct apple_dcp_h17p_rt_bw_config) == 0x10);
static_assert(sizeof(struct apple_dcp_h17p_rt_bw_request) == 0x04);
static_assert(sizeof(struct apple_dcp_h17p_rt_bw_reply) == 0x14);
static_assert(APPLE_DCP_LINK_STREAM_BUFFER_SIZE *
	      APPLE_DCP_LINK_STREAMS_PER_SIDE ==
	      APPLE_DCP_LINK_STREAM_HALF_SIZE);
static_assert(APPLE_DCP_LINK_HEAP_OFFSET + APPLE_DCP_LINK_HEAP_SIZE ==
	      APPLE_DCP_LINK_RPC_MEMORY_SIZE);

static inline int
apple_dcp_h17p_clock_rate(const struct apple_dcp_h17p_clock_request *request,
			  u64 rate_154, u64 rate_194, u64 *rate)
{
	if (!request || !rate)
		return -EINVAL;

	switch (le32_to_cpu(request->clock_id)) {
	case APPLE_DCP_H17P_CLOCK_ID_154:
		*rate = rate_154;
		return 0;
	case APPLE_DCP_H17P_CLOCK_ID_194:
		*rate = rate_194;
		return 0;
	default:
		return -ENOENT;
	}
}

static inline int
apple_dcp_h17p_callback_method(u32 call, struct apple_dcp_link_method *method)
{
	struct apple_dcp_link_method found = { .call = call };

	if (!method)
		return -EINVAL;

	switch (call) {
	case APPLE_DCP_H17P_CB_WILL_POWER_OFF:
	case APPLE_DCP_H17P_CB_MATCH_PMU:
	case APPLE_DCP_H17P_CB_CLEAR_DEFAULT_FB:
		break;
	case APPLE_DCP_H17P_CB_DID_BOOT:
	case APPLE_DCP_H17P_CB_DID_POWER_ON:
	case APPLE_DCP_H17P_CB_START_HARDWARE_BOOT:
	case APPLE_DCP_H17P_CB_MATCH_PMU_2:
	case APPLE_DCP_H17P_CB_MATCH_BACKLIGHT:
		found.output_size = 4;
		break;
	case APPLE_DCP_H17P_CB_RT_BANDWIDTH:
		found.input_size = 4;
		found.output_size = 20;
		break;
	case APPLE_DCP_H17P_CB_FRAME_SYNC:
		found.input_size = 84;
		found.output_size = 80;
		break;
	case APPLE_DCP_H17P_CB_ALLOCATE_BANDWIDTH:
		found.input_size = 28;
		found.output_size = 20;
		break;
	case APPLE_DCP_H17P_CB_MAP_BUF:
		found.input_size = 12;
		found.output_size = 16;
		break;
	case APPLE_DCP_H17P_CB_UNMAP_BUF:
	case APPLE_DCP_H17P_CB_SWAP_NOTIFY:
		found.input_size = 24;
		break;
	case APPLE_DCP_H17P_CB_GET_TIME:
		found.output_size = 8;
		break;
	case APPLE_DCP_H17P_CB_POWERSTATE_NOTIFY:
	case APPLE_DCP_H17P_CB_ABORT_SWAP:
	case APPLE_DCP_H17P_CB_ENABLE_BACKLIGHT:
		found.input_size = 4;
		break;
	case APPLE_DCP_H17P_CB_PROP_PUBLISH:
		found.input_size = 16;
		break;
	case APPLE_DCP_H17P_CB_SWAP_COMPLETE:
		found.input_size = 1840;
		break;
	case APPLE_DCP_H17P_CB_SWAP_COMPLETE_INTENT:
		found.input_size = 20;
		break;
	case APPLE_DCP_H17P_CB_GET_UINT_PROP:
		found.input_size = 80;
		found.output_size = 12;
		break;
	case APPLE_DCP_H17P_CB_GET_FREQUENCY:
		found.input_size = 8;
		found.output_size = 8;
		break;
	case APPLE_DCP_H17P_CB_MAP_REG:
		found.input_size = 16;
		found.output_size = 28;
		break;
	case APPLE_DCP_H17P_CB_SET_PROPERTY_INT:
		found.input_size = 80;
		found.output_size = 4;
		break;
	case APPLE_DCP_H17P_CB_SET_PROPERTY_BOOL:
		found.input_size = 76;
		found.output_size = 4;
		break;
	case APPLE_DCP_H17P_CB_ALLOCATE_BUFFER:
		found.input_size = 20;
		found.output_size = 20;
		break;
	case APPLE_DCP_H17P_CB_MAP_PHYSICAL:
		found.input_size = 8;
		found.output_size = 4;
		break;
	case APPLE_DCP_H17P_CB_RELEASE_MEM_DESC:
	case APPLE_DCP_H17P_CB_POWER_UP_DART:
		found.input_size = 4;
		found.output_size = 4;
		break;
	case APPLE_DCP_H17P_CB_HOTPLUG:
		found.input_size = 88;
		found.output_size = 76;
		break;
	case APPLE_DCP_H17P_CB_CREATE_DEFAULT_FB:
		found.input_size = 8;
		found.output_size = 4;
		break;
	case APPLE_DCP_H17P_CB_SWAP_INFO:
		found.input_size = 228;
		found.output_size = 224;
		break;
	case APPLE_DCP_H17P_CB_BATCHED_SWAP_COMPLETE:
		found.output_size = 1024;
		break;
	default:
		return -ENOENT;
	}

	*method = found;
	return 0;
}

static inline int
apple_dcp_link_stream_layout(enum apple_dcp_link_side side, u8 stream,
			     bool remote,
			     struct apple_dcp_link_stream_layout *layout)
{
	u32 local_side;
	u32 remote_side;
	u32 stream_offset;

	if (!layout || side > APPLE_DCP_LINK_SIDE_DCP ||
	    stream >= APPLE_DCP_LINK_STREAMS_PER_SIDE)
		return -EINVAL;

	local_side = side * APPLE_DCP_LINK_SIDE_MEMORY_SIZE;
	remote_side = (side ^ 1) * APPLE_DCP_LINK_SIDE_MEMORY_SIZE;
	stream_offset = stream * APPLE_DCP_LINK_STREAM_BUFFER_SIZE;

	if (remote) {
		layout->local_offset = local_side +
				       APPLE_DCP_LINK_STREAM_HALF_SIZE +
				       stream_offset;
		layout->remote_offset = remote_side + stream_offset;
	} else {
		layout->local_offset = local_side + stream_offset;
		layout->remote_offset = remote_side +
					APPLE_DCP_LINK_STREAM_HALF_SIZE +
					stream_offset;
	}
	layout->capacity = APPLE_DCP_LINK_STREAM_BUFFER_SIZE;

	return 0;
}

static inline int apple_dcp_h17p_method(u32 call,
					struct apple_dcp_link_method *method)
{
	struct apple_dcp_link_method found = { .call = call };

	if (!method)
		return -EINVAL;

	switch (call) {
	case APPLE_DCP_H17P_CALL_UP_SET_CREATE_DFB:
	case APPLE_DCP_H17P_CALL_SETUP_VIDEO_LIMITS:
	case APPLE_DCP_H17P_CALL_FIRST_CLIENT_OPEN:
		break;
	case APPLE_DCP_H17P_CALL_LATE_INIT:
		found.input_size = 4;
		found.output_size = 4;
		break;
	case APPLE_DCP_H17P_CALL_START_SIGNAL:
	case APPLE_DCP_H17P_CALL_VI_SET_TEMPERATURE_HINT:
	case APPLE_DCP_H17P_CALL_IS_MAIN_DISPLAY:
	case APPLE_DCP_H17P_CALL_CREATE_DEFAULT_FB:
	case APPLE_DCP_H17P_CALL_DISPLAY_REFRESH:
		found.output_size = 4;
		break;
	case APPLE_DCP_H17P_CALL_SWAP_START:
	case APPLE_DCP_H17P_CALL_SET_POWER_STATE:
		found.input_size = 16;
		found.output_size = 8;
		break;
	case APPLE_DCP_H17P_CALL_SWAP_SUBMIT:
		found.input_size = 0x1d30;
		found.output_size = 12;
		break;
	case APPLE_DCP_H17P_CALL_SET_DISPLAY_DEVICE:
	case APPLE_DCP_H17P_CALL_VIDEO_POWER_SAVINGS:
	case APPLE_DCP_H17P_CALL_LAST_CLIENT_CLOSE:
		found.input_size = 4;
		found.output_size = 4;
		break;
	case APPLE_DCP_H17P_CALL_SET_DIGITAL_OUT_MODE:
	case APPLE_DCP_H17P_CALL_ABORT_SWAPS:
		found.input_size = 8;
		found.output_size = 4;
		break;
	case APPLE_DCP_H17P_CALL_SET_MATRIX:
		found.input_size = 80;
		found.output_size = 4;
		break;
	case APPLE_DCP_H17P_CALL_SET_PARAMETER:
		found.input_size = 40;
		found.output_size = 4;
		break;
	case APPLE_DCP_H17P_CALL_FLUSH_SUPPORTS_POWER:
		found.input_size = 4;
		break;
	default:
		return -ENOENT;
	}

	*method = found;
	return 0;
}

static inline void
apple_dcp_link_init_desc_encode(struct apple_dcp_link_init_desc *desc,
				u64 shared_var_dva, bool use_64m_window)
{
	desc->shared_dva = cpu_to_le64(shared_var_dva);
	desc->flags = cpu_to_le32(use_64m_window ? APPLE_DCP_LINK_INIT_FLAG_64M : 0);
	desc->version = cpu_to_le32(APPLE_DCP_LINK_PROTOCOL_VERSION);
}

static inline bool
apple_dcp_link_init_desc_valid(const struct apple_dcp_link_init_desc *desc)
{
	u64 shared_dva;
	u32 flags;

	if (!desc)
		return false;

	shared_dva = le64_to_cpu(desc->shared_dva);
	flags = le32_to_cpu(desc->flags);

	if (le32_to_cpu(desc->version) != APPLE_DCP_LINK_PROTOCOL_VERSION)
		return false;
	if (!shared_dva)
		return flags == 0;

	return flags == APPLE_DCP_LINK_INIT_FLAG_64M &&
	       !(shared_dva & (APPLE_DCP_LINK_RPC_PAGE_SIZE - 1)) &&
	       !(shared_dva & ~GENMASK_ULL(47, 0));
}

static inline bool
apple_dcp_link_init_desc_zero(const struct apple_dcp_link_init_desc *desc)
{
	return desc && !le64_to_cpu(desc->shared_dva) &&
	       !le32_to_cpu(desc->flags) && !le32_to_cpu(desc->version);
}

static inline bool
apple_dcp_link_stream_memory_idle(const void *memory, size_t size)
{
	const u8 *bytes = memory;
	size_t offset;

	if (!memory || size < APPLE_DCP_LINK_RPC_MEMORY_SIZE)
		return false;

	for (offset = sizeof(struct apple_dcp_link_init_desc);
	     offset < APPLE_DCP_LINK_RPC_MEMORY_SIZE; offset++) {
		if (bytes[offset])
			return false;
	}

	return true;
}

static inline int apple_dcp_link_init_message(u64 rpc_dva, u64 *message)
{
	if (!message)
		return -EINVAL;
	if (rpc_dva & ~GENMASK_ULL(47, 0))
		return -ERANGE;

	*message = (rpc_dva << 16) | APPLE_DCP_LINK_MSG_INIT;
	return 0;
}

static inline enum apple_dcp_link_handshake_action
apple_dcp_link_handshake_step(enum apple_dcp_link_state *state, u64 message)
{
	u8 type;

	if (!state)
		return APPLE_DCP_LINK_HANDSHAKE_NONE;

	type = message & GENMASK_ULL(1, 0);
	switch (type) {
	case APPLE_DCP_LINK_MSG_FIRMWARE_INIT:
		return APPLE_DCP_LINK_HANDSHAKE_NONE;
	case APPLE_DCP_LINK_MSG_READY:
		if (*state != APPLE_DCP_LINK_WAIT_READY)
			return APPLE_DCP_LINK_HANDSHAKE_NONE;
		*state = APPLE_DCP_LINK_READY;
		return APPLE_DCP_LINK_HANDSHAKE_START_IOMFB;
	default:
		return APPLE_DCP_LINK_HANDSHAKE_NONE;
	}
}

static inline int apple_dcp_link_ready_reply(u64 message, u16 *version,
					     u32 *firmware_hash)
{
	if (!version || !firmware_hash)
		return -EINVAL;
	if ((message & GENMASK_ULL(1, 0)) != APPLE_DCP_LINK_MSG_READY)
		return -EPROTO;

	*version = FIELD_GET(APPLE_DCP_LINK_MSG_VERSION, message);
	*firmware_hash = FIELD_GET(APPLE_DCP_LINK_MSG_HASH, message);
	return 0;
}

static inline int apple_dcp_link_rpc_payload_size(u32 input_size,
						  u32 output_size, u32 *size)
{
	u32 total;

	if (!size)
		return -EINVAL;
	if (check_add_overflow(input_size, output_size, &total) ||
	    check_add_overflow(total,
			       (u32)sizeof(struct apple_dcp_link_rpc_header),
			       &total))
		return -EOVERFLOW;

	*size = total;
	return 0;
}

static inline int apple_dcp_link_align_payload(u32 size, u32 alignment,
					       u32 *aligned_size)
{
	u32 padded;

	if (!aligned_size || !is_power_of_2(alignment))
		return -EINVAL;
	if (check_add_overflow(size, alignment - 1, &padded))
		return -EOVERFLOW;

	*aligned_size = padded & -alignment;
	return 0;
}

static inline int apple_dcp_link_rpc_message(u32 offset, u32 size, bool nested,
					     u64 *message)
{
	if (!message)
		return -EINVAL;
	if (offset > U16_MAX || size > U16_MAX)
		return -ERANGE;

	*message = APPLE_DCP_LINK_MSG_RPC |
		   FIELD_PREP(APPLE_DCP_LINK_MSG_OFFSET, offset) |
		   FIELD_PREP(APPLE_DCP_LINK_MSG_SIZE, size);
	if (nested)
		*message |= APPLE_DCP_LINK_MSG_NESTED;

	return 0;
}

static inline int apple_dcp_link_stream_message(u64 message, u8 stream,
						bool remote, u64 *encoded)
{
	if (!encoded)
		return -EINVAL;
	if (stream >= APPLE_DCP_LINK_STREAMS_PER_SIDE)
		return -ERANGE;

	message &= ~(APPLE_DCP_LINK_MSG_REMOTE | APPLE_DCP_LINK_MSG_STREAM);
	message |= FIELD_PREP(APPLE_DCP_LINK_MSG_STREAM, stream);
	if (remote)
		message |= APPLE_DCP_LINK_MSG_REMOTE;

	*encoded = message;
	return 0;
}

static inline u8 apple_dcp_link_message_stream(u64 message)
{
	return FIELD_GET(APPLE_DCP_LINK_MSG_STREAM, message);
}

static inline bool apple_dcp_link_message_is_remote(u64 message)
{
	return message & APPLE_DCP_LINK_MSG_REMOTE;
}

static inline u64 apple_dcp_link_rpc_reply(u64 request)
{
	return (request & (APPLE_DCP_LINK_MSG_VERSION |
			   APPLE_DCP_LINK_REPLY_CONTROL_MASK)) |
	       APPLE_DCP_LINK_MSG_RPC_REPLY;
}

static inline int apple_dcp_link_rpc_decode(u64 message, void *buffer,
					    u32 capacity,
					    struct apple_dcp_link_rpc_view *view)
{
	struct apple_dcp_link_rpc_header *header;
	u32 expected_size;
	u32 offset;
	u32 total_size;
	int ret;

	if (!buffer || !view)
		return -EINVAL;
	if ((message & GENMASK_ULL(1, 0)) != APPLE_DCP_LINK_MSG_RPC ||
	    (message & GENMASK_ULL(7, 6)) == BIT_ULL(6))
		return -EPROTO;

	offset = FIELD_GET(APPLE_DCP_LINK_MSG_OFFSET, message);
	total_size = FIELD_GET(APPLE_DCP_LINK_MSG_SIZE, message);
	if (offset > capacity || total_size > capacity - offset ||
	    total_size < sizeof(*header))
		return -EMSGSIZE;

	header = buffer + offset;
	ret = apple_dcp_link_rpc_payload_size(le32_to_cpu(header->input_size),
					      le32_to_cpu(header->output_size),
					      &expected_size);
	if (ret)
		return ret;
	if (expected_size != total_size)
		return -EMSGSIZE;

	view->call = le32_to_cpu(header->call);
	view->input_size = le32_to_cpu(header->input_size);
	view->output_size = le32_to_cpu(header->output_size);
	view->offset = offset;
	view->total_size = total_size;
	view->input = header + 1;
	view->output = (u8 *)(header + 1) + view->input_size;
	return 0;
}

static inline int apple_dcp_link_stack_push(u32 top, u32 capacity,
					    u32 payload_size, u32 alignment,
					    u32 *offset, u32 *next_top)
{
	u32 aligned_size;
	int ret;

	if (!offset || !next_top || top > capacity)
		return -EINVAL;

	ret = apple_dcp_link_align_payload(payload_size, alignment, &aligned_size);
	if (ret)
		return ret;
	if (aligned_size > capacity - top)
		return -ENOSPC;

	*offset = top;
	*next_top = top + aligned_size;
	return 0;
}

static inline int apple_dcp_link_stack_pop(u32 top, u32 payload_size,
					   u32 alignment, u32 *next_top)
{
	u32 aligned_size;
	int ret;

	if (!next_top)
		return -EINVAL;

	ret = apple_dcp_link_align_payload(payload_size, alignment, &aligned_size);
	if (ret)
		return ret;
	if (aligned_size > top)
		return -ERANGE;

	*next_top = top - aligned_size;
	return 0;
}

static inline void
apple_dcp_link_rpc_header_encode(struct apple_dcp_link_rpc_header *header,
				 u32 call, u32 input_size, u32 output_size)
{
	header->call = cpu_to_le32(call);
	header->input_size = cpu_to_le32(input_size);
	header->output_size = cpu_to_le32(output_size);
}

static inline void
apple_dcp_h17p_rt_bw_encode(struct apple_dcp_h17p_rt_bw_config *config,
			    u64 scratch, u64 clock_request)
{
	config->scratch = cpu_to_le64(scratch);
	config->clock_request = cpu_to_le64(clock_request);
}

static inline int
apple_dcp_h17p_rt_bw_encode_reply(struct apple_dcp_link_rpc_view *view,
				  u64 scratch, u64 clock_request)
{
	const struct apple_dcp_h17p_rt_bw_request *request;
	struct apple_dcp_h17p_rt_bw_reply *reply;

	if (!view || view->call != APPLE_DCP_LINK_CALL_D003)
		return -EINVAL;
	if (view->input_size < sizeof(*request) ||
	    view->output_size < sizeof(*reply))
		return -EMSGSIZE;

	request = view->input;
	if (le32_to_cpu(request->config_null) & 1)
		return -EINVAL;

	reply = view->output;
	apple_dcp_h17p_rt_bw_encode(&reply->config, scratch, clock_request);
	reply->status = cpu_to_le32(0);
	return 0;
}

#endif /* __APPLE_DCP_LINK_H__ */
