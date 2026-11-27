/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef APPLE_DCP_M4_PEER_H
#define APPLE_DCP_M4_PEER_H

#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/stddef.h>
#include <linux/unaligned.h>

#include "dcp-peer-v26_6.h"

/*
 * Single T8132 external display pipe limits: 6144 px width and
 * 1,274,019,840 active px/s (6144 x 3456 @ 60 Hz, matching the DCP
 * firmware's UnifiedPipeline single-pipe bandwidth ceiling).
 */
#define DCP_M4_EXT_PIPE_MAX_WIDTH	6144U
#define DCP_M4_EXT_PIPE_MAX_ACTIVE_RATE	1274019840U

/*
 * Whether a 160-byte SET_TILED_DISPLAY_HINTS record needs one idle peer pipe
 * (non-tiled sink exceeding one T8132 pipe in width or active pixel rate,
 * while fitting within two pipes).
 */
static inline bool dcp_m4_peer_needs_pair(const struct dcp_display_hints *h)
{
	u32 width = get_unaligned_le32(h->hints + 0x38);
	u32 height = get_unaligned_le32(h->hints + 0x3c);
	u32 active = get_unaligned_le32(h->hints + 0x44);

	if (!(h->hints[0] & 1) || (h->hints[0] & 2) || !width || !height)
		return false;
	if (width <= DCP_M4_EXT_PIPE_MAX_WIDTH &&
	    active <= DCP_M4_EXT_PIPE_MAX_ACTIVE_RATE)
		return false;
	if (width > 2U * DCP_M4_EXT_PIPE_MAX_WIDTH ||
	    (u64)active > 2ULL * DCP_M4_EXT_PIPE_MAX_ACTIVE_RATE)
		return false;
	return true;
}

/* Halve horizontal fields of an 80-byte IOMFB timing record for dual-pipe merge. */
static inline void dcp_m4_peer_split_timing(u8 *timing)
{
	static const u8 half_fields[] = { 4, 8, 12, 16, 20, 28, 68 };

	for (unsigned int i = 0; i < sizeof(half_fields); i++) {
		u8 *field = timing + half_fields[i];

		put_unaligned_le32(get_unaligned_le32(field) >> 1, field);
	}
	put_unaligned_le32((s32)get_unaligned_le32(timing + 24) / 2,
			   timing + 24);
}

/* A364/A365 video-data records for 26.6.2 dual-pipe merge modes. */
struct dcp_m4_peer_video_data {
	u8 timing[0x50];
	u8 color[0x20];
} __packed;

struct dcp_m4_peer_get_video_req {
	struct dcp_m4_peer_video_data video;
	__le32 timing_id, color_id;
	u8 requires_merge, timing_null, color_null, requires_merge_null;
} __packed;

struct dcp_m4_peer_get_video_resp {
	struct dcp_m4_peer_video_data video;
	u8 requires_merge, success, padding[2];
} __packed;

struct dcp_m4_peer_insert_video_req {
	struct dcp_m4_peer_video_data video;
	__le32 timing_id, color_id;
	u8 timing_null, color_null, padding[2];
} __packed;

struct dcp_m4_peer_insert_video_resp {
	struct dcp_m4_peer_video_data video;
	u8 success, padding[3];
} __packed;

static_assert(sizeof(struct dcp_m4_peer_get_video_req) == 0x7c);
static_assert(sizeof(struct dcp_m4_peer_get_video_resp) == 0x74);
static_assert(sizeof(struct dcp_m4_peer_insert_video_req) == 0x7c);
static_assert(sizeof(struct dcp_m4_peer_insert_video_resp) == 0x74);

#endif
