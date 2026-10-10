/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/* Copyright The Asahi Linux Contributors */

#ifndef __APPLE_DCP_LINK_H__
#define __APPLE_DCP_LINK_H__

#include <linux/build_bug.h>
#include <linux/errno.h>
#include <linux/types.h>

#include <asm/byteorder.h>

/*
 * H17-generation DCP firmware uses the ordinary IOMFB transport once the
 * INIT/SetShmem handshake is done: the same six fixed channels (CMD 0x00000,
 * OOBCMD 0x08000, ASYNC 0x40000, OOBASYNC 0x48000, CB 0x60000,
 * OOBCB 0x68000), selected by the context field of the message.  Records are
 * allocated on each channel's stack at
 *   off_next = off_cur + align_up(12 + in_len + out_len, 0x40)
 * which is the rule dcp_packet_start() and DCP_PACKET_ALIGNMENT implement in
 * iomfb.c.
 *
 * This header only describes the callback payloads whose layout differs from
 * older firmware.
 */

#define APPLE_DCP_H17P_CLOCK_ID_154		0x154
#define APPLE_DCP_H17P_CLOCK_ID_194		0x194

struct neo_apple_dcp_h17p_clock_request {
	__le32 service;
	__le32 clock_id;
} __packed;

struct neo_apple_dcp_h17p_rt_bw_config {
	__le64 scratch;
	__le64 clock_request;
} __packed;

struct neo_apple_dcp_h17p_rt_bw_request {
	__le32 config_null;
} __packed;

struct neo_apple_dcp_h17p_rt_bw_reply {
	struct neo_apple_dcp_h17p_rt_bw_config config;
	__le32 status;
} __packed;

static_assert(sizeof(struct neo_apple_dcp_h17p_clock_request) == 0x08);
static_assert(sizeof(struct neo_apple_dcp_h17p_rt_bw_config) == 0x10);
static_assert(sizeof(struct neo_apple_dcp_h17p_rt_bw_request) == 0x04);
static_assert(sizeof(struct neo_apple_dcp_h17p_rt_bw_reply) == 0x14);

static inline int
neo_apple_dcp_h17p_clock_rate(const struct neo_apple_dcp_h17p_clock_request *request,
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

static inline void
neo_apple_dcp_h17p_rt_bw_encode(struct neo_apple_dcp_h17p_rt_bw_config *config,
			    u64 scratch, u64 clock_request)
{
	config->scratch = cpu_to_le64(scratch);
	config->clock_request = cpu_to_le64(clock_request);
}

/* D003: fill in the bandwidth config unless the firmware passed it as null */
static inline int
neo_apple_dcp_h17p_rt_bw_encode_reply(const struct neo_apple_dcp_h17p_rt_bw_request *request,
				  struct neo_apple_dcp_h17p_rt_bw_reply *reply,
				  u64 scratch, u64 clock_request)
{
	if (le32_to_cpu(request->config_null) & 1)
		return -EINVAL;

	neo_apple_dcp_h17p_rt_bw_encode(&reply->config, scratch, clock_request);
	reply->status = cpu_to_le32(0);
	return 0;
}

#endif /* __APPLE_DCP_LINK_H__ */
