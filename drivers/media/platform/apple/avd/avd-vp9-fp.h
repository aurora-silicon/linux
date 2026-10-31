/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef AVD_VP9_FP_H_
#define AVD_VP9_FP_H_

#include "avd-queue.h"

struct v4l2_ctrl_vp9_frame;
struct v4l2_ctrl_vp9_compressed_hdr;

/* Set only by SoC match data, never by a userspace control. */
enum avd_vp9_fp_generation {
	AVD_VP9_FP_UNKNOWN,
	AVD_VP9_FP_V3,
	AVD_VP9_FP_BORAGE,
	AVD_VP9_FP_T8112,
	AVD_VP9_FP_T8140,
	AVD_VP9_FP_T6000,
};

#define AVD_VP9_PROBS_SIZE 0x774

bool avd_vp9_needs_counts(u32 flags);
int avd_vp9_pack_fp(struct avd_ctx *ctx, struct avd_frame *f,
		    const struct v4l2_ctrl_vp9_frame *frame,
		    const struct v4l2_ctrl_vp9_compressed_hdr *hdr,
		    const void *probs, dma_addr_t probs_addr);
int avd_vp9_pack_cmd(struct avd_ctx *ctx, struct avd_frame *f);

extern const struct avd_codec_frame_ops avd_vp9_frame_ops;
#endif
