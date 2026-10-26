/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef APPLE_IOMFB_PEER_V27_H
#define APPLE_IOMFB_PEER_V27_H

#include <linux/types.h>
#include <linux/build_bug.h>
#include <linux/unaligned.h>
#include <linux/math64.h>
#include "iomfb_plane.h"
#include "iomfb_v27.h"

/* Native A364 and A365 share the video-data prefix, but not the final
 * flags: A364 has a requires-merge boolean before three pointer-null flags;
 * A365 has only two pointer-null flags and padding.
 */
struct dcp_peer_video_data_v27 {
	u8 timing[0x50];
	u8 color[0x20];
} __packed;

struct dcp_peer_get_video_req_v27 {
	struct dcp_peer_video_data_v27 video;
	__le32 timing_id, color_id;
	u8 requires_merge, timing_null, color_null, requires_merge_null;
} __packed;

struct dcp_peer_get_video_resp_v27 {
	struct dcp_peer_video_data_v27 video;
	u8 requires_merge, success, padding[2];
} __packed;

struct dcp_peer_insert_video_req_v27 {
	struct dcp_peer_video_data_v27 video;
	__le32 timing_id, color_id;
	u8 timing_null, color_null, padding[2];
} __packed;

struct dcp_peer_insert_video_resp_v27 {
	struct dcp_peer_video_data_v27 video;
	u8 success, padding[3];
} __packed;

static_assert(sizeof(struct dcp_peer_get_video_req_v27) == 0x7c);
static_assert(sizeof(struct dcp_peer_get_video_resp_v27) == 0x74);
static_assert(sizeof(struct dcp_peer_insert_video_req_v27) == 0x7c);
static_assert(sizeof(struct dcp_peer_insert_video_resp_v27) == 0x74);

/* Native 26A428 digital-out mode setter.
 * Keep vertical timings, color data and all unmodified fields intact.
 * The signed horizontal offset at0x18 uses division toward zero.
 */
static inline void dcp_peer_split_timing_v27(struct dcp_peer_video_data_v27 *v)
{
	static const u8 half_fields[] = { 4, 8, 12, 16, 20, 28, 68 };

	for (unsigned int i = 0; i < sizeof(half_fields); i++) {
		u8 *field = v->timing + half_fields[i];

		put_unaligned_le32(get_unaligned_le32(field) >> 1, field);
	}
	put_unaligned_le32((s32)get_unaligned_le32(v->timing + 24) / 2,
			   v->timing + 24);
}

/* VFTG splits a surface at the half-panel boundary, as native does.
 * Source and destination use pixels here, as in the existing IOMFB plane
 * path. Use rational arithmetic in the kernel rather than native FP;
 * round a shared source boundary once so neither half loses/duplicates it.
 * DRM has already clipped negative/offscreen destination coordinates.
 */
static inline bool dcp_peer_clip_surface_v27(const struct dcp_rect *src,
	const struct dcp_rect *dst, u32 tile_x, u32 tile_width,
	struct dcp_rect *tile_src, struct dcp_rect *tile_dst)
{
	u64 left = max_t(u64, dst->x, tile_x);
	u64 right = min_t(u64, (u64)dst->x + dst->w,
			  (u64)tile_x + tile_width);
	u64 first, last;

	memset(tile_src, 0, sizeof(*tile_src));
	memset(tile_dst, 0, sizeof(*tile_dst));
	if (!src->w || !src->h || !dst->w || !dst->h || left >= right)
		return false;
	first = div_u64((left - dst->x) * src->w + dst->w / 2, dst->w);
	last = div_u64((right - dst->x) * src->w + dst->w / 2, dst->w);
	if (first >= last || (u64)src->x + last > U32_MAX)
		return false;
	*tile_src = *src;
	tile_src->x += first;
	tile_src->w = last - first;
	*tile_dst = *dst;
	tile_dst->x = left - tile_x;
	tile_dst->w = right - left;
	return true;
}


/* Clone descriptors only: both DCPs use the same display DMA domain/IOVA. */
static inline void dcp_peer_split_swap_v27(struct dcp_swap_submit_req_v27 *left,
					  struct dcp_swap_submit_req_v27 *right,
					  u32 tile_width)
{
	*right = *left;
	for (unsigned int i = 0; i < ARRAY_SIZE(left->surf_null); i++) {
		struct dcp_rect src = left->swap.src_rect[i];
		struct dcp_rect dst = left->swap.dst_rect[i];
		bool present = !left->surf_null[i];
		struct dcp_swap_submit_req_v27 *halves[] = {left, right};

		for (unsigned int side = 0; side < 2; side++) {
			struct dcp_swap_submit_req_v27 *req = halves[side];
			struct dcp_rect clipped_src, clipped_dst;

			if (present && dcp_peer_clip_surface_v27(&src, &dst,
				side * tile_width, tile_width, &clipped_src, &clipped_dst)) {
				req->swap.src_rect[i] = clipped_src;
				req->swap.dst_rect[i] = clipped_dst;
			} else {
				req->surf_null[i] = true;
				req->surf_iova[i] = 0;
				req->swap.surf_ids[i] = 0;
				memset(&req->swap.src_rect[i], 0, sizeof(req->swap.src_rect[i]));
				memset(&req->swap.dst_rect[i], 0, sizeof(req->swap.dst_rect[i]));
			}
		}
	}
}

#endif
