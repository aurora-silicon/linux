/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef AVD_AV1_FP_H_
#define AVD_AV1_FP_H_

#include <linux/types.h>

struct avd_ctx;
struct avd_frame;
struct v4l2_ctrl_av1_sequence;
struct v4l2_ctrl_av1_frame;
struct v4l2_ctrl_av1_tile_group_entry;
struct v4l2_ctrl_av1_film_grain;

enum avd_av1_fp_layout {
	AVD_AV1_FP_UNKNOWN,
	AVD_AV1_FP_BORAGE,
	AVD_AV1_FP_T8140_TODO,
};

/* Incoming edge: the primary reference selects the previous HW CDF output. */
static inline bool avd_av1_cdf_dep(bool uses_primary, u64 primary_timestamp,
				 bool have_prev, u64 prev_timestamp,
				 bool prev_updates_cdf)
{
	return uses_primary && have_prev && prev_updates_cdf &&
		primary_timestamp == prev_timestamp;
}

int avd_av1_validate_tiles(const struct v4l2_ctrl_av1_frame *frame,
			   const struct v4l2_ctrl_av1_tile_group_entry *tiles,
			   unsigned int count, size_t payload);
int avd_av1_pack_fp(struct avd_ctx *ctx, struct avd_frame *f,
		    const struct v4l2_ctrl_av1_sequence *seq,
		    const struct v4l2_ctrl_av1_frame *frame,
		    const struct v4l2_ctrl_av1_tile_group_entry *tiles,
		    const struct v4l2_ctrl_av1_film_grain *grain);
int avd_av1_pack_cmd(struct avd_ctx *ctx, struct avd_frame *f);

#endif
