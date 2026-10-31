/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Apple AVD VPU codec driver
 *
 * Copyright The Asahi Linux Contributors
 * Copyright 2023 Eileen Yoon <eyn@gmx.com>
 *
 * Copyright (C) 2019 Collabora, Ltd.
 *	Boris Brezillon <boris.brezillon@collabora.com>
 * Copyright (C) 2021 Collabora, Ltd.
 *	Andrzej Pietrasiewicz <andrzej.p@collabora.com>
 *
 * Copyright (C) 2016 Rockchip Electronics Co., Ltd.
 *	Alpha Lin <Alpha.Lin@rock-chips.com>
 */

#include <linux/unaligned.h>

#include <media/v4l2-vp9.h>
#include <media/videobuf2-dma-contig.h>

#include "avd.h"
#include "avd-inst.h"
#include "avd-vp9-fp.h"

#define VP9_Q_IDX(v)	FIELD_PREP(GENMASK(31, 15), v)
#define VP9_Q_DC_Y(v)	FIELD_PREP(GENMASK(14, 10), v)
#define VP9_Q_DC_UV(v)	FIELD_PREP(GENMASK(9, 5), v)
#define VP9_Q_AC_UV(v)	FIELD_PREP(GENMASK(4, 0), v)

#define VP9_LF_SHARPNESS(v)	FIELD_PREP(GENMASK(31, 28), v)
#define VP9_LF_REF0(v)		FIELD_PREP(GENMASK(27, 21), v)
#define VP9_LF_REF1(v)		FIELD_PREP(GENMASK(20, 14), v)
#define VP9_LF_REF2(v)		FIELD_PREP(GENMASK(13, 7), v)
#define VP9_LF_REF3(v)		FIELD_PREP(GENMASK(6, 0), v)

#define VP9_LF_LV(v)	FIELD_PREP(GENMASK(31, 14), v)
#define VP9_LF_MODE0(v)	FIELD_PREP(GENMASK(13, 7), v)
#define VP9_LF_MODE1(v)	FIELD_PREP(GENMASK(6, 0), v)

#define VP9_FEAT_LVL_ALT_Q_EN(v)	FIELD_PREP(BIT(21), !!(v))
#define VP9_FEAT_LVL_ALT_Q(v)		FIELD_PREP(GENMASK(20, 12), v)
#define VP9_FEAT_LVL_ALT_L_EN(v)	FIELD_PREP(BIT(11), !!(v))
#define VP9_FEAT_LVL_ALT_L(v)		FIELD_PREP(GENMASK(10, 4), v)
#define VP9_FEAT_LVL_REF_FRAME_EN(v)	FIELD_PREP(BIT(3), !!(v))
#define VP9_FEAT_LVL_REF_FRAME(v)	FIELD_PREP(GENMASK(2, 1), v)
#define VP9_FEAT_LVL_SKIP_EN(v)		FIELD_PREP(BIT(0), !!(v))
#define VP9_FEAT_LVL_SKIP(v)

#define VP9_REF_SEL_LAST(v)	FIELD_PREP(GENMASK(2, 0), v)
#define VP9_REF_BIAS_LAST(v)	FIELD_PREP(BIT(3), !!(v))
#define VP9_REF_SEL_GOLDEN(v)	FIELD_PREP(GENMASK(6, 4), v)
#define VP9_REF_BIAS_GOLDEN(v)	FIELD_PREP(BIT(7), !!(v))
#define VP9_REF_SEL_ALT(v)	FIELD_PREP(GENMASK(10, 8), v)
#define VP9_REF_BIAS_ALT(v)	FIELD_PREP(BIT(11), !!(v))
#define VP9_REFERENCE_MODE(v)	FIELD_PREP(GENMASK(13, 12), v)
#define VP9_PARALLEL_DEC(v)	FIELD_PREP(BIT(14), !!(v))
#define VP9_REFRESH_CTX(v)	FIELD_PREP(BIT(15), !!(v))
#define VP9_INTERP_FILTER(v)	FIELD_PREP(GENMASK(18, 16), v)
#define VP9_HIGH_PREC_MV(v)	FIELD_PREP(BIT(19), !!(v))
#define VP9_ERR_RES(v)		FIELD_PREP(BIT(20), !!(v))
#define VP9_HAS_REF(v)		FIELD_PREP(BIT(21), !!(v))
#define VP9_SEG_ABS(v)		FIELD_PREP(BIT(22), !!(v))
#define VP9_SEG_UDATA_TEMP(v)	FIELD_PREP(BIT(23), !!(v))
#define VP9_SEG_UPDATE_MAP(v)	FIELD_PREP(BIT(24), !!(v))
#define VP9_SEG_ENABLED(v)	FIELD_PREP(BIT(25), !!(v))
#define VP9_SEG_RESET(v)	FIELD_PREP(BIT(26), !!(v))

#define VP9_MAX_TILE_COLS	(1 << 4)
#define VP9_REF_SCALE_SHIFT	14
#define VP9_LAST_FRAME		1
#define VP9_GOLDEN_FRAME	2
#define VP9_ALTREF_FRAME	3

struct avd_vp9_seg_probs {
	u8 tree_probs[7];
	u8 pred_probs[3];
};

struct avd_vp9_probs {
	struct avd_vp9_seg_probs seg;
	u8 tx8[2][1];
	u8 tx16[2][2];
	u8 tx32[2][3];
	/* [4][2][2][k=6][(k == 0) ? 3 : 6][3] */
	u8 coef[1584];
	u8 skip[3];
	u8 inter_mode[7][3];
	u8 interp_filter[4][2];
	u8 is_inter[4];
	u8 comp_mode[5];
	u8 single_ref[5][2];
	u8 comp_ref[5];
	u8 y_mode[4][9];
	u8 uv_mode[10][9];
	u8 partition[16][3];
	u8 joint[3];
	struct mv_comp {
		u8 sign;
		u8 classes[10];
		u8 class0_bit;
		u8 bits[10];
	} mv_comp[2];
	struct mv_fr {
		u8 class0_fr[2][3];
		u8 fr[3];
	} mv_fr[2];
	struct mv_hp {
		u8 class0_hp;
		u8 hp;
	} mv_hp[2];
};
static_assert(sizeof(struct avd_vp9_probs) == 1905);

struct avd_vp9_frame_symbol_counts {
	u32 padding;
	u32 tx8p[2][2];
	u32 tx16p[2][3];
	u32 tx32p[2][4];
	/* [4][2][2][k=6][(k == 0) ? 3 : 6] */
	u32 eob_0[528];
	/*
	 * struct ref_cnt { u32 coef[3]; u32 eob_1; }
	 * struct ref_cnt cef_counts[4][2][2][k=6][(k == 0) ? 3 : 6];
	 */
	u32 ref_cnt[2112];
	u32 skip[3][2];
	u32 mv_mode[7][4];
	u32 filter[4][3];
	u32 intra_inter[4][2];
	u32 comp[5][2];
	u32 single_ref[5][2][2];
	u32 comp_ref[5][2];
	u32 y_mode[4][10];
	u32 uv_mode[10][10];
	u32 partition[16][4];
	u32 mv_joint[4];
	struct mv_comp_ctn {
		u32 sign[2];
		u32 classes[11];
		u32 class0[2];
		u32 bits[10][2];
	} mv_comp[2];
	struct mv_fr_cnt {
		u32 class0_fr[2][4];
		u32 fr[4];
	} mv_fr[2];
	struct mv_hp_cnt {
		u32 class0_hp[2];
		u32 hp[2];
	} mv_hp[2];
};
static_assert(sizeof(struct avd_vp9_frame_symbol_counts) == 12252);

struct avd_vp9_frame_info {
	u32 valid : 1;
	u32 segmapid : 1;
	u32 frame_context_idx : 2;
	u32 reference_mode : 2;
	u32 tx_mode : 3;
	u32 interpolation_filter : 3;
	u32 flags;
	u64 timestamp;
	struct v4l2_vp9_segmentation seg;
	struct v4l2_vp9_loop_filter lf;
};

struct avd_vp9_slot;

struct avd_vp9_run {
	struct avd_frame *frame;
	struct avd_vp9_slot *slot;
	int error;
	struct avd_run base;

	const struct v4l2_ctrl_vp9_frame *decode_params;
	const struct v4l2_ctrl_vp9_compressed_hdr *prob_updates;
};

struct avd_vp9_segmap {
	struct avd_buf buf;
	unsigned int users;
};

struct avd_vp9_slot {
	unsigned int tiles;
	struct avd_vp9_segmap *seg_in, *seg_out;
	struct v4l2_vp9_frame_symbol_counts cnts;
	struct v4l2_vp9_frame_context probability_tables;
	struct v4l2_vp9_frame_context initial_probs;
	struct avd_vp9_bufs {
		struct avd_buf inst;
		struct avd_buf az_left;
		struct avd_buf above_info;
		struct avd_buf ip_above;
		struct avd_buf lf_above;
		struct avd_buf lf_left_info;
		struct avd_buf lf_left;
		struct avd_buf seg;
		struct avd_buf pipe_state;
		struct avd_buf counts;
		struct avd_buf probs;
	} bufs;
	struct avd_vp9_frame_info cur;
	struct avd_vp9_frame_info last;
};

struct avd_vp9_ctx {
	/* Transactional reset/forward updates, committed only after packing. */
	struct v4l2_vp9_frame_context working_context[4];
	struct v4l2_vp9_frame_context frame_context[4];
	struct avd_vp9_slot *slots[AVD_MAX_FRAMES];
	/* One output per active frame, plus the previous retired map. */
	struct avd_vp9_segmap segmaps[AVD_MAX_FRAMES + 1];
	struct avd_vp9_segmap *last_segmap;
	struct avd_vp9_frame_info last;
	/* CPU probability preparation must wait for this frame's counts. */
	struct avd_frame *pending;
	bool failed;
};

static bool avd_vp9_use_frame_ops(const struct avd_ctx *ctx)
{
	return ctx->frame_ops == &avd_vp9_frame_ops;
}

static int vp9_get_maps(struct avd_vp9_ctx *stream, struct avd_vp9_slot *slot)
{
	unsigned int idx;

	for (idx = 0; idx < ARRAY_SIZE(stream->segmaps); idx++)
		if (!stream->segmaps[idx].users)
			break;
	if (idx == ARRAY_SIZE(stream->segmaps))
		return -EAGAIN;
	slot->seg_out = &stream->segmaps[idx];
	slot->seg_in = stream->last_segmap ? stream->last_segmap : slot->seg_out;
	slot->seg_out->users++;
	slot->seg_in->users++;
	return 0;
}

static void vp9_put_maps(struct avd_vp9_slot *slot)
{
	slot->seg_in->users--;
	slot->seg_out->users--;
	slot->seg_in = NULL;
	slot->seg_out = NULL;
}

static void vp9_publish_map(struct avd_vp9_ctx *stream,
			    struct avd_vp9_slot *slot)
{
	if (stream->last_segmap)
		stream->last_segmap->users--;
	stream->last_segmap = slot->seg_out;
	stream->last_segmap->users++;
}

/* Build the same instruction words as the legacy builder, in DMA memory. */
#undef push
#undef pusha
static void vp9_push(struct avd_vp9_run *run, u32 word)
{
	struct avd_frame *f = run->frame;

	if (f->prog_words >= f->prog->size / sizeof(__le32)) {
		run->error = -ENOSPC;
		return;
	}
	((__le32 *)f->prog->cpu)[f->prog_words++] = cpu_to_le32(word);
}

static void vp9_push_address(struct avd_ctx *ctx, struct avd_vp9_run *run,
			     dma_addr_t addr)
{
	if (ctx->dev->variant->quirks & AVD_QUIRK_LSR) {
		vp9_push(run, addr >> 8);
	} else {
		vp9_push(run, lower_32_bits(addr));
		vp9_push(run, upper_32_bits(addr));
	}
}

static void vp9_push_comp(struct avd_ctx *ctx, struct avd_vp9_run *run,
			  dma_addr_t addr, const u32 offsets[4])
{
	for (int i = 0; i < 4; i++) {
		if (ctx->dev->variant->quirks & AVD_QUIRK_LSR)
			vp9_push(run, (addr + offsets[i]) >> 7);
		else
			vp9_push_address(ctx, run, addr + offsets[i]);
	}
}

#define push(word, name) vp9_push(run, word)
#define pusha(addr, name, i) vp9_push_address(ctx, run, addr)


/* Flatten the legacy avd_submit_job() EXEC boundaries into the DMA stream.
 * Firmware owns FIFO/VP pacing and PP submission; the codec never touches MMIO.
 */
static int vp9_finish_program(struct avd_ctx *ctx, struct avd_frame *f,
			      size_t header_words, unsigned int tiles)
{
	__le32 *words = f->prog->cpu;
	u32 mask = ctx->dev->variant->revision == 3 ?
		AVD_OP_EXEC_REV3_VP9_MASK : 0;
	u32 fifo = AVD_OP_EXEC_FIFO_IDX(avd_backend_is_qfw(ctx->dev) ?
				     f->idx : 0);
	size_t total = f->prog_words + tiles + 1;

	if (total > f->prog->size / sizeof(*words))
		return -ENOSPC;
	for (unsigned int i = tiles; i > 0; i--) {
		size_t src = header_words + (i - 1) * 5;
		size_t dst = header_words + 1 + (i - 1) * 6;

		memmove(words + dst, words + src, 5 * sizeof(*words));
		words[dst + 5] = cpu_to_le32(AVD_OP_EXEC | mask | fifo |
			AVD_OP_EXEC_FLAG_END(i == tiles));
	}
	memmove(words + 1, words, header_words * sizeof(*words));
	words[0] = cpu_to_le32(AVD_OP_EXEC | mask | fifo |
		AVD_OP_EXEC_FLAG_START_REV3(ctx->dev->variant->revision == 3) |
		AVD_OP_EXEC_FLAG_START_REV4(ctx->dev->variant->revision == 4));
	f->prog_words = total;
	return 0;
}

static void set_refs(struct avd_ctx *ctx, struct avd_vp9_run *run)
{
	const struct v4l2_ctrl_vp9_frame *frame = run->decode_params;
	struct avd_decoded_buffer *dst, *ref_buf[4];
	dma_addr_t addr;
	int xscale, yscale;

	dst = vb2_to_avd_decoded_buf(&run->base.bufs.dst->vb2_buf);

	ref_buf[0] = avd_get_ref_buf(ctx, &dst->base.vb, frame->last_frame_ts);
	ref_buf[1] =
		avd_get_ref_buf(ctx, &dst->base.vb, frame->golden_frame_ts);
	ref_buf[2] = avd_get_ref_buf(ctx, &dst->base.vb, frame->alt_frame_ts);

	push(0, "");
	push(0, "");
	push(0, "");

	for (int i = 0; i < V4L2_VP9_NUM_FRAME_CTX - 1; i++) {
		addr = vb2_dma_contig_plane_dma_addr(
			       &ref_buf[i]->base.vb.vb2_buf, 0) +
		       ref_buf[i]->comp.start_offset;

		push(AVD_REF_FLAG_CONST, "hdr_9c_ref_100");
		push(AVD_HDR_HEIGHT(ref_buf[i]->vp9.height - 1) |
			     AVD_HDR_WIDTH(ref_buf[i]->vp9.width - 1),
		     "hdr_70_ref_height_width");

		xscale = (ref_buf[i]->vp9.width << VP9_REF_SCALE_SHIFT) /
			 dst->vp9.width;
		yscale = (ref_buf[i]->vp9.height << VP9_REF_SCALE_SHIFT) /
			 dst->vp9.height;
		push(AVD_HDR_HEIGHT(xscale) | AVD_HDR_WIDTH(yscale),
		     "ref_scale");

		vp9_push_comp(ctx, run, addr, ref_buf[i]->comp.offsets);
	}
}

static u32 make_flags1(struct avd_ctx *ctx, struct avd_vp9_run *run)
{
	struct avd_vp9_slot *vp9_ctx = run->slot;
	const struct v4l2_ctrl_vp9_frame *frame = run->decode_params;
	struct avd_decoded_buffer *dst, *last, *golden, *alt;

	dst = vb2_to_avd_decoded_buf(&run->base.bufs.dst->vb2_buf);
	last = avd_get_ref_buf(ctx, &dst->base.vb, frame->last_frame_ts);
	golden = avd_get_ref_buf(ctx, &dst->base.vb, frame->golden_frame_ts);
	alt = avd_get_ref_buf(ctx, &dst->base.vb, frame->alt_frame_ts);

	/*
	 * i have only seen V4L2_VP9_SIGN_BIAS_ALT, im simply guessing the
	 * same applies for last and golden
	 */
	u32 flags = VP9_REF_SEL_LAST(VP9_LAST_FRAME);
	flags |= VP9_REF_BIAS_LAST(frame->ref_frame_sign_bias &
				   V4L2_VP9_SIGN_BIAS_LAST);
	flags |= VP9_REF_SEL_GOLDEN(golden == last ? VP9_LAST_FRAME :
						     VP9_GOLDEN_FRAME);
	flags |= VP9_REF_BIAS_GOLDEN(frame->ref_frame_sign_bias &
				     V4L2_VP9_SIGN_BIAS_GOLDEN);
	flags |= VP9_REF_SEL_ALT(alt != golden ? VP9_ALTREF_FRAME :
				 alt != last   ? VP9_GOLDEN_FRAME :
						 VP9_LAST_FRAME);
	flags |= VP9_REF_BIAS_ALT(frame->ref_frame_sign_bias &
				  V4L2_VP9_SIGN_BIAS_ALT);

	flags |= VP9_REFERENCE_MODE(frame->reference_mode);

	flags |= VP9_PARALLEL_DEC(frame->flags &
				  V4L2_VP9_FRAME_FLAG_PARALLEL_DEC_MODE);
	flags |= VP9_REFRESH_CTX(frame->flags &
				 V4L2_VP9_FRAME_FLAG_REFRESH_FRAME_CTX);
	flags |= VP9_INTERP_FILTER(frame->interpolation_filter);
	flags |= VP9_HIGH_PREC_MV(frame->flags &
				  V4L2_VP9_FRAME_FLAG_ALLOW_HIGH_PREC_MV);

	flags |=
		VP9_ERR_RES(frame->flags & V4L2_VP9_FRAME_FLAG_ERROR_RESILIENT);

	flags |= VP9_SEG_ABS(frame->seg.flags &
			     V4L2_VP9_SEGMENTATION_FLAG_ABS_OR_DELTA_UPDATE);
	flags |= VP9_SEG_UDATA_TEMP(
		frame->seg.flags & V4L2_VP9_SEGMENTATION_FLAG_UPDATE_DATA &&
		frame->seg.flags & V4L2_VP9_SEGMENTATION_FLAG_TEMPORAL_UPDATE);
	flags |= VP9_SEG_UPDATE_MAP(frame->seg.flags &
				    V4L2_VP9_SEGMENTATION_FLAG_UPDATE_MAP);
	flags |= VP9_SEG_ENABLED(frame->seg.flags &
				 V4L2_VP9_SEGMENTATION_FLAG_ENABLED);

	flags |= VP9_HAS_REF(
		!(frame->flags & V4L2_VP9_FRAME_FLAG_ERROR_RESILIENT) &&
		!(frame->flags & V4L2_VP9_FRAME_FLAG_KEY_FRAME) &&
		vp9_ctx->last.valid &&
		vp9_ctx->last.flags & V4L2_VP9_FRAME_FLAG_SHOW_FRAME &&
		!(vp9_ctx->last.flags & V4L2_VP9_FRAME_FLAG_KEY_FRAME));

	flags |= VP9_SEG_RESET(
		frame->seg.flags & V4L2_VP9_SEGMENTATION_FLAG_ENABLED &&
		(frame->flags & V4L2_VP9_FRAME_FLAG_KEY_FRAME ||
		 (vp9_ctx->last.valid &&
		  !(vp9_ctx->last.seg.flags &
		    V4L2_VP9_SEGMENTATION_FLAG_ENABLED) &&
		  vp9_ctx->last.flags & (V4L2_VP9_FRAME_FLAG_KEY_FRAME |
					 V4L2_VP9_FRAME_FLAG_INTRA_ONLY))));

	return flags;
}

static u32 seg_features(struct avd_ctx *ctx, struct avd_vp9_run *run,
			unsigned int segid)
{
	struct avd_vp9_slot *vp9_ctx = run->slot;
	const struct v4l2_vp9_segmentation *seg = &vp9_ctx->cur.seg;
	u32 feature_val = 0;
	int feature_id = 0;

	feature_id = V4L2_VP9_SEG_LVL_ALT_Q;
	feature_val |= VP9_FEAT_LVL_ALT_Q_EN(v4l2_vp9_seg_feat_enabled(
		seg->feature_enabled, feature_id, segid));
	feature_val |= VP9_FEAT_LVL_ALT_Q(seg->feature_data[segid][feature_id]);

	feature_id = V4L2_VP9_SEG_LVL_ALT_L;
	feature_val |= VP9_FEAT_LVL_ALT_L_EN(v4l2_vp9_seg_feat_enabled(
		seg->feature_enabled, feature_id, segid));
	feature_val |= VP9_FEAT_LVL_ALT_L(seg->feature_data[segid][feature_id]);

	feature_id = V4L2_VP9_SEG_LVL_REF_FRAME;
	feature_val |= VP9_FEAT_LVL_REF_FRAME_EN(v4l2_vp9_seg_feat_enabled(
		seg->feature_enabled, feature_id, segid));
	feature_val |=
		VP9_FEAT_LVL_REF_FRAME(seg->feature_data[segid][feature_id]);

	feature_id = V4L2_VP9_SEG_LVL_SKIP;
	feature_val |= VP9_FEAT_LVL_SKIP_EN(v4l2_vp9_seg_feat_enabled(
		seg->feature_enabled, feature_id, segid));

	return feature_val;
}

static void set_header(struct avd_ctx *ctx, struct avd_vp9_run *run)
{
	struct avd_vp9_slot *vp9_ctx = run->slot;
	const struct v4l2_ctrl_vp9_compressed_hdr *prob_updates =
		run->prob_updates;
	const struct v4l2_ctrl_vp9_frame *frame = run->decode_params;
	struct avd_dev *avd = ctx->dev;
	u32 bytesperline;

	bool intra_only = !!(frame->flags & (V4L2_VP9_FRAME_FLAG_KEY_FRAME |
					     V4L2_VP9_FRAME_FLAG_INTRA_ONLY));

	push(AVD_OP_HDR | AVD_OP_HDR_FLAG_DECOMP(ctx->decomp) |
		     AVD_OP_HDR_FLAG_INTRA(intra_only) | AVD_OP_HDR_CONST |
		     AVD_OP_HDR_FLAG_PIPE_STATE(
			     !(avd->variant->quirks & AVD_QUIRK_NO_PIPE_STATE)),
	     "hdr_34_start_hdr");

	push(AVD_HDR_CODEC_MODE(AVD_CODEC_VP9), "hdr_38_mode");

	push(AVD_HDR_HEIGHT(frame->frame_height_minus_1) |
		     AVD_HDR_WIDTH(frame->frame_width_minus_1),
	     "hdr_28_height_width_shift3");
	push(0, "");
	push(AVD_HDR_HEIGHT(frame->frame_height_minus_1) |
		     AVD_HDR_WIDTH(frame->frame_width_minus_1),
	     "hdr_38_height_width_shift3");

	push(AVD_HDR_COMMON_CHROMA_FORMAT(1) |
		     AVD_HDR_COMMON_BIT_DEPTH_C(frame->profile) |
		     AVD_HDR_COMMON_BIT_DEPTH_L(frame->bit_depth - 8) |
		     AVD_HDR_COMMON_LUMA_CBS(3) |
		     AVD_HDR_COMMON_LUMA_TBS(min(prob_updates->tx_mode, 3)) |
		     AVD_HDR_COMMON_FLAG0(prob_updates->tx_mode &
					  V4L2_VP9_TX_MODE_SELECT),
	     "hdr_2c_txfm_mode");

	push(make_flags1(ctx, run), "hdr_40_flags1_pt1");

	for (int i = 0; i < 8; i++)
		push(seg_features(ctx, run, i), "seg");

	/* some kind of feature enable? h26{2,5} has 3 instread */
	push(AVD_HDR_FEAT_VP9, "unk_const");
	push(0, "");
	push(0, "");

	pusha(vp9_ctx->bufs.counts.addr, "counts", 0);
	pusha(vp9_ctx->bufs.probs.addr, "probs", 0);
	pusha(vp9_ctx->bufs.above_info.addr, "above_info", 0);
	pusha(vp9_ctx->bufs.seg.addr, "seg", 1);
	pusha(vp9_ctx->bufs.seg.addr, "seg", 2);
	pusha(vp9_ctx->seg_in->buf.addr, "color", 3);
	pusha(vp9_ctx->seg_out->buf.addr, "color", 4);

	push(VP9_Q_IDX(frame->quant.base_q_idx) |
		     VP9_Q_DC_Y(frame->quant.delta_q_y_dc) |
		     VP9_Q_DC_UV(frame->quant.delta_q_uv_dc) |
		     VP9_Q_AC_UV(frame->quant.delta_q_uv_ac),
	     "hdr_4c_base_q_idx");
	/* filter related flags? */
	push(VP9_LF_SHARPNESS(frame->lf.sharpness) |
		     (frame->lf.flags &
				      V4L2_VP9_LOOP_FILTER_FLAG_DELTA_ENABLED ?
			      VP9_LF_REF0(frame->lf.ref_deltas[0]) |
				      VP9_LF_REF1(frame->lf.ref_deltas[1]) |
				      VP9_LF_REF2(frame->lf.ref_deltas[2]) |
				      VP9_LF_REF3(frame->lf.ref_deltas[3]) :
			      0),
	     "hdr_44_flags1_pt2");

	push(VP9_LF_LV(frame->lf.level) |
		     (frame->lf.flags &
				      V4L2_VP9_LOOP_FILTER_FLAG_DELTA_ENABLED ?
			      VP9_LF_MODE0(frame->lf.mode_deltas[0]) |
				      VP9_LF_MODE1(frame->lf.mode_deltas[1]) :
			      0),
	     "hdr_48_loop_filter_level");

	push(0, "");
	push(0, "");

	if (avd->variant->revision == 3)
		push(0, "");
	if (!(avd->variant->quirks & AVD_QUIRK_NO_PIPE_STATE))
		pusha(vp9_ctx->bufs.pipe_state.addr, "pipe_state", 0);

	pusha(vp9_ctx->bufs.ip_above.addr, "ip_above", 0);
	pusha(vp9_ctx->bufs.lf_above.addr, "lf_above", 0);
	/* no lf_above_info? */
	pusha((u64)0, "hdr_e8_sps0_tile_addr_lsb8", 0);
	pusha(vp9_ctx->bufs.lf_left.addr, "lf_left", 0);
	pusha(vp9_ctx->bufs.lf_left_info.addr, "lf_left_info", 0);
	pusha(vp9_ctx->bufs.az_left.addr, "az_left", 0);

	push(0, "");

	vp9_push_comp(ctx, run, run->base.comp_out, ctx->comp.offsets);

	pusha((u64)0, "packed_fmt_scratch", 0);

	bytesperline = ctx->decoded_fmt.fmt.pix_mp.plane_fmt[0].bytesperline;
	if (avd->variant->quirks & AVD_QUIRK_LSR)
		bytesperline = bytesperline >> 4;

	pusha(run->base.y_out, "hdr_168_y_addr_lsb8", 0);
	push(bytesperline, "hdr_170_width_align");
	pusha(run->base.uv_out, "hdr_16c_uv_addr_lsb8", 0);
	push(bytesperline, "hdr_174_width_align");
	push(0, "");
	push(AVD_HDR_HEIGHT(frame->frame_height_minus_1) |
		     AVD_HDR_WIDTH(frame->frame_width_minus_1),
	     "cm3_height_width");

	if (!(intra_only))
		set_refs(ctx, run);
}

static int set_tiles(struct avd_ctx *ctx, struct avd_vp9_run *run)
{
	const struct v4l2_ctrl_vp9_frame *frame = run->decode_params;
	struct vb2_v4l2_buffer *src = run->base.bufs.src;
	const u8 *data = vb2_plane_vaddr(&src->vb2_buf, 0);

	u32 offset =
		frame->uncompressed_header_size + frame->compressed_header_size;
	size_t payload = vb2_get_plane_payload(&src->vb2_buf, 0);
	u32 size;
	u32 num_tile_rows = 1 << frame->tile_rows_log2;
	u32 num_tile_cols = 1 << frame->tile_cols_log2;
	u32 tile_size;

	/* 6.2.6 Compute image size syntax */
	u32 sb_64_cols = (((frame->frame_width_minus_1 + 8) >> 3) + 7) >> 3;
	u32 sb_64_rows = (((frame->frame_height_minus_1 + 8) >> 3) + 7) >> 3;

	if (!data || offset >= payload || payload > U32_MAX)
		return -EINVAL;
	size = payload - offset;

	for (u32 row = 0; row < num_tile_rows; row++)
		for (u32 col = 0; col < num_tile_cols; col++) {
			if (row == num_tile_rows - 1 &&
			    col == num_tile_cols - 1) {
				tile_size = size;
			} else {
				if (size < 4)
					return -EINVAL;
				tile_size = get_unaligned_be32(&data[offset]);
				if (!tile_size || tile_size > size - 4)
					return -EINVAL;
				offset += 4;
				size -= 4;
			}
			if (!tile_size)
				return -EINVAL;
			push(AVD_OP_CODED_DATA |
				     AVD_OP_CODED_DATA_ADDR(
					     (run->base.coded_in + offset) >> 32),
			     "cm3_cmd_set_slice_data");
			push((u32)((run->base.coded_in + offset) & 0xffffffff),
			     "til_ab4_tile_addr_low");
			push(tile_size, "til_ab8_tile_size");
			push(AVD_OP_SL_DIM_START |
				     AVD_OP_SL_DIM_START_Y((row * sb_64_rows) /
							   num_tile_rows) |
				     AVD_OP_SL_DIM_START_X((col * sb_64_cols) /
							   num_tile_cols),
			     "i");

			push(AVD_SL_DIM_END_COL(col) |
				     AVD_SL_DIM_END_Y(((row + 1) * sb_64_rows) /
							      num_tile_rows - 1) |
				     AVD_SL_DIM_END_X(((col + 1) * sb_64_cols) /
							      num_tile_cols - 1),
			     "til_ac0_tile_dims");

			offset += tile_size;
			size -= tile_size;
		}
	return run->error;
}

static void update_dec_buf_info(struct avd_decoded_buffer *buf,
				const struct v4l2_ctrl_vp9_frame *dec_params)
{
	buf->vp9.width = dec_params->frame_width_minus_1 + 1;
	buf->vp9.height = dec_params->frame_height_minus_1 + 1;
	buf->vp9.bit_depth = dec_params->bit_depth;
}

static void update_ctx_cur_info(struct avd_vp9_slot *vp9_ctx,
				struct avd_decoded_buffer *buf,
				const struct v4l2_ctrl_vp9_frame *dec_params)
{
	vp9_ctx->cur.valid = true;
	vp9_ctx->cur.reference_mode = dec_params->reference_mode;
	vp9_ctx->cur.interpolation_filter = dec_params->interpolation_filter;
	vp9_ctx->cur.flags = dec_params->flags;
	vp9_ctx->cur.timestamp = buf->base.vb.vb2_buf.timestamp;
	vp9_ctx->cur.seg = dec_params->seg;
	vp9_ctx->cur.lf = dec_params->lf;
}

static void copy_vp9_frame_mv(struct avd_vp9_probs *avd_probs,
			      const struct v4l2_vp9_frame_context *probs)
{
	memcpy(avd_probs->joint, probs->mv.joint, sizeof(avd_probs->joint));
	for (int i = 0; i < 2; i++) {
		avd_probs->mv_comp[i].sign = probs->mv.sign[i];
		memcpy(avd_probs->mv_comp[i].bits, probs->mv.bits[i],
		       sizeof(avd_probs->mv_comp[i].bits));
		avd_probs->mv_comp[i].class0_bit = probs->mv.class0_bit[i];
		memcpy(avd_probs->mv_comp[i].classes, probs->mv.classes[i],
		       sizeof(avd_probs->mv_comp[i].bits));

		memcpy(avd_probs->mv_fr[i].class0_fr, probs->mv.class0_fr[i],
		       sizeof(avd_probs->mv_fr[i].class0_fr));
		memcpy(avd_probs->mv_fr[i].fr, probs->mv.fr[i],
		       sizeof(avd_probs->mv_fr[i].fr));

		avd_probs->mv_hp[i].class0_hp = probs->mv.class0_hp[i];
		avd_probs->mv_hp[i].hp = probs->mv.hp[i];
	}
}

static void init_probs(struct avd_ctx *ctx, const struct avd_vp9_run *run)
{
	const struct v4l2_ctrl_vp9_frame *dec_params;
	struct avd_vp9_slot *vp9_ctx = run->slot;
	struct avd_vp9_probs *avd_probs = vp9_ctx->bufs.probs.cpu;
	const struct v4l2_vp9_segmentation *seg;
	const struct v4l2_vp9_frame_context *probs;
	bool intra_only;
	int count = 0;

	dec_params = run->decode_params;
	probs = &vp9_ctx->probability_tables;
	seg = &dec_params->seg;

	memset(avd_probs, 0, sizeof(*avd_probs));

	intra_only = !!(dec_params->flags & (V4L2_VP9_FRAME_FLAG_KEY_FRAME |
					     V4L2_VP9_FRAME_FLAG_INTRA_ONLY));

	memcpy(avd_probs->tx8, probs->tx8, sizeof(avd_probs->tx8));
	memcpy(avd_probs->tx16, probs->tx16, sizeof(avd_probs->tx16));
	memcpy(avd_probs->tx32, probs->tx32, sizeof(avd_probs->tx32));
	memcpy(avd_probs->skip, probs->skip, sizeof(avd_probs->skip));
	memcpy(avd_probs->inter_mode, probs->inter_mode,
	       sizeof(avd_probs->inter_mode));
	memcpy(avd_probs->interp_filter, probs->interp_filter,
	       sizeof(avd_probs->interp_filter));
	memcpy(avd_probs->is_inter, probs->is_inter,
	       sizeof(avd_probs->is_inter));
	memcpy(avd_probs->comp_mode, probs->comp_mode,
	       sizeof(avd_probs->comp_mode));
	memcpy(avd_probs->single_ref, probs->single_ref,
	       sizeof(avd_probs->single_ref));
	memcpy(avd_probs->comp_ref, probs->comp_ref,
	       sizeof(avd_probs->comp_ref));
	memcpy(avd_probs->y_mode, probs->y_mode, sizeof(avd_probs->y_mode));

	memcpy(avd_probs->partition,
	       intra_only ? v4l2_vp9_kf_partition_probs : probs->partition,
	       sizeof(avd_probs->partition));
	memcpy(avd_probs->uv_mode,
	       intra_only ? v4l2_vp9_kf_uv_mode_prob : probs->uv_mode,
	       sizeof(avd_probs->uv_mode));

	copy_vp9_frame_mv(avd_probs, probs);

	/* please mister gcc optimise this away */
	for (int t = 0; t < 4; t++)
		for (int i = 0; i < 2; i++)
			for (int j = 0; j < 2; j++)
				for (int k = 0; k < 6; k++) {
					int max_l = (k == 0) ? 3 : 6;
					for (int l = 0; l < max_l; l++) {
						for (int n = 0; n < 3; n++)
							avd_probs->coef[count++] =
								probs->coef[t][i][j][k][l][n];
					}
				}

	memcpy(avd_probs->seg.pred_probs, seg->pred_probs,
	       sizeof(avd_probs->seg.pred_probs));
	memcpy(avd_probs->seg.tree_probs, seg->tree_probs,
	       sizeof(avd_probs->seg.tree_probs));
}

static int validate_dec_params(struct avd_ctx *ctx,
			       const struct v4l2_ctrl_vp9_frame *dec_params)
{
	unsigned int aligned_width, aligned_height;

	if ((dec_params->profile != 0 || dec_params->bit_depth != 8) &&
	    (dec_params->profile != 2 || dec_params->bit_depth != 10))
		return -EINVAL;
	if (!(dec_params->flags & V4L2_VP9_FRAME_FLAG_X_SUBSAMPLING) ||
	    !(dec_params->flags & V4L2_VP9_FRAME_FLAG_Y_SUBSAMPLING))
		return -EINVAL;
	/* The current instruction's column field is four bits. */
	if (dec_params->tile_cols_log2 > 4 || dec_params->tile_rows_log2 > 2)
		return -EINVAL;
	if ((1U << dec_params->tile_cols_log2) >
	    DIV_ROUND_UP(dec_params->frame_width_minus_1 + 1, 64) ||
	    (1U << dec_params->tile_rows_log2) >
	    DIV_ROUND_UP(dec_params->frame_height_minus_1 + 1, 64))
		return -EINVAL;

	if (dec_params->frame_height_minus_1 + 1 < 64 ||
	    dec_params->frame_width_minus_1 + 1 < 64)
		return -EINVAL;

	aligned_width = round_up(dec_params->frame_width_minus_1 + 1, 64);
	aligned_height = round_up(dec_params->frame_height_minus_1 + 1, 16);

	/*
	 * Userspace should update the capture/decoded format when the
	 * resolution changes.
	 */
	if (aligned_width != ctx->decoded_fmt.fmt.pix_mp.width ||
	    aligned_height != ctx->decoded_fmt.fmt.pix_mp.height) {
		dev_err(ctx->dev->dev,
			"unexpected bitstream resolution %dx%d\n",
			aligned_width, aligned_height);
		return -EINVAL;
	}

	return 0;
}

/* The open transports need one PP kick per tile, from this frame's snapshot. */
unsigned int avd_vp9_frame_pp_kicks(const struct avd_frame *f)
{
	const struct avd_vp9_slot *slot = f->codec_priv;

	return slot ? slot->tiles : 0;
}

static int avd_vp9_alloc_bufs(struct avd_ctx *ctx,
			      struct avd_vp9_slot *vp9_ctx)
{
	struct avd_dev *avd = ctx->dev;
	int ret, w, h, bit_depth;
	ret = avd_buf_alloc(avd, &vp9_ctx->bufs.inst, fifo_size());
	if (ret)
		return ret;
	w = fmt_width(ctx);
	h = fmt_height(ctx);
	bit_depth = (ctx->image_fmt == AVD_IMG_FMT_420_10BIT ||
		     ctx->image_fmt == AVD_IMG_FMT_422_10BIT) ?
			    10 :
			    8;

	if (!(avd->variant->quirks & AVD_QUIRK_NO_PIPE_STATE)) {
		ret = avd_buf_alloc(avd, &vp9_ctx->bufs.pipe_state, 0x200);
		if (ret)
			return ret;
	}

	ret = avd_buf_alloc(avd, &vp9_ctx->bufs.probs,
			    AVD_VP9_PROBS_SIZE);
	if (ret)
		return ret;

	ret = avd_buf_alloc(avd, &vp9_ctx->bufs.counts,
			    sizeof(struct avd_vp9_frame_symbol_counts));
	if (ret)
		return ret;

	ret = avd_buf_alloc(avd, &vp9_ctx->bufs.ip_above,
			    DIV_ROUND_UP(w, 16) * 4 * bit_depth +
				    (VP9_MAX_TILE_COLS - 1) * 128);
	if (ret)
		return ret;
	ret = avd_buf_alloc(avd, &vp9_ctx->bufs.lf_above,
			    DIV_ROUND_UP(w, 8) * 16 * bit_depth);
	if (ret)
		return ret;

	ret = avd_buf_alloc(avd, &vp9_ctx->bufs.seg, DIV_ROUND_UP(w, 8) * 24);
	if (ret)
		return ret;

	ret = avd_buf_alloc(avd, &vp9_ctx->bufs.lf_left,
			    DIV_ROUND_UP(h, 8) * 16 * bit_depth);
	if (ret)
		return ret;

	ret = avd_buf_alloc(avd, &vp9_ctx->bufs.lf_left_info,
			    DIV_ROUND_UP(h, 64) * 16);
	if (ret)
		return ret;

	ret = avd_buf_alloc(avd, &vp9_ctx->bufs.az_left,
			    bit_depth * 36 * DIV_ROUND_UP(h, 8) +
				    bit_depth * 18 * DIV_ROUND_UP(h, 16));
	if (ret)
		return ret;

	ret = avd_buf_alloc(avd, &vp9_ctx->bufs.above_info,
			    DIV_ROUND_UP(w, 64) * 288 +
				    (VP9_MAX_TILE_COLS - 1) * 128);
	if (ret)
		return ret;

	return 0;
}

#define copy_tx_and_skip(p1, p2)                                    \
	do {                                                        \
		memcpy((p1)->tx8, (p2)->tx8, sizeof((p1)->tx8));    \
		memcpy((p1)->tx16, (p2)->tx16, sizeof((p1)->tx16)); \
		memcpy((p1)->tx32, (p2)->tx32, sizeof((p1)->tx32)); \
		memcpy((p1)->skip, (p2)->skip, sizeof((p1)->skip)); \
	} while (0)

static void avd_vp9_retire(struct avd_ctx *ctx, struct avd_frame *f,
			   enum vb2_buffer_state result)
{
	struct avd_vp9_ctx *stream = ctx->priv;
	struct avd_vp9_slot *vp9_ctx = f->codec_priv;
	unsigned int fctx_idx;

	if (!vp9_ctx)
		return;

	/* v4l2-specific stuff */
	if (result == VB2_BUF_STATE_ERROR) {
		stream->failed = true;
		goto out_update_last;
	}

	/*
	 * vp9 stuff
	 *
	 * 6.1.2 refresh_probs()
	 *
	 * In the spec a complementary condition goes last in 6.1.2 refresh_probs(),
	 * but it makes no sense to perform all the activities from the first "if"
	 * there if we actually are not refreshing the frame context. On top of that,
	 * because of 6.2 uncompressed_header() whenever error_resilient_mode == 1,
	 * refresh_frame_context == 0. Consequently, if we don't jump to out_update_last
	 * it means error_resilient_mode must be 0.
	 */
	if (!(vp9_ctx->cur.flags & V4L2_VP9_FRAME_FLAG_REFRESH_FRAME_CTX))
		goto out_update_last;

	fctx_idx = vp9_ctx->cur.frame_context_idx;

	if (!(vp9_ctx->cur.flags & V4L2_VP9_FRAME_FLAG_PARALLEL_DEC_MODE)) {
		/* error_resilient_mode == 0 && frame_parallel_decoding_mode == 0 */
		struct v4l2_vp9_frame_context *probs =
			&vp9_ctx->probability_tables;
		bool frame_is_intra = vp9_ctx->cur.flags &
				      (V4L2_VP9_FRAME_FLAG_KEY_FRAME |
				       V4L2_VP9_FRAME_FLAG_INTRA_ONLY);
		struct tx_and_skip {
			u8 tx8[2][1];
			u8 tx16[2][2];
			u8 tx32[2][3];
			u8 skip[3];
		} _tx_skip, *tx_skip = &_tx_skip;
		struct v4l2_vp9_frame_symbol_counts *counts;

		/* buffer the forward-updated TX and skip probs */
		if (frame_is_intra)
			copy_tx_and_skip(tx_skip, probs);

		/* 6.1.2 refresh_probs(): load_probs() and load_probs2() */
		*probs = vp9_ctx->initial_probs;

		/* if FrameIsIntra then undo the effect of load_probs2() */
		if (frame_is_intra)
			copy_tx_and_skip(probs, tx_skip);

		counts = &vp9_ctx->cnts;

		v4l2_vp9_adapt_coef_probs(
			probs, counts,
			!vp9_ctx->last.valid ||
				vp9_ctx->last.flags &
					V4L2_VP9_FRAME_FLAG_KEY_FRAME,
			frame_is_intra);
		if (!frame_is_intra) {
			const struct avd_vp9_frame_symbol_counts *cnts;
			int i;
			u32 tx16p[2][4] = {};
			u32 sign[2][2];
			u32 classes[2][11];
			u32 class0[2][2];
			u32 bits[2][10][2];
			u32 class0_fp[2][2][4];
			u32 fp[2][4];
			u32 class0_hp[2][2];
			u32 hp[2][2];

			cnts = vp9_ctx->bufs.counts.cpu;

			for (i = 0; i < ARRAY_SIZE(cnts->tx16p); ++i)
				memcpy(tx16p[i], cnts->tx16p[i],
				       sizeof(cnts->tx16p[0]));

			for (i = 0; i < 2; i++) {
				memcpy(sign[i], cnts->mv_comp[i].sign,
				       sizeof(sign[0]));
				memcpy(classes[i], cnts->mv_comp[i].classes,
				       sizeof(classes[0]));
				memcpy(class0[i], cnts->mv_comp[i].class0,
				       sizeof(class0[0]));
				memcpy(bits[i], cnts->mv_comp[i].bits,
				       sizeof(bits[0]));
				memcpy(class0_fp[i], cnts->mv_fr[i].class0_fr,
				       sizeof(class0_fp[0]));
				memcpy(fp[i], cnts->mv_fr[i].fr, sizeof(fp[0]));
				memcpy(class0_hp[i], cnts->mv_hp[i].class0_hp,
				       sizeof(class0_hp[0]));
				memcpy(hp[i], cnts->mv_hp[i].hp, sizeof(hp[0]));
			}

			counts->tx16p = &tx16p;
			counts->sign = &sign;
			counts->classes = &classes;
			counts->class0 = &class0;
			counts->bits = &bits;
			counts->class0_fp = &class0_fp;
			counts->fp = &fp;
			counts->class0_hp = &class0_hp;
			counts->hp = &hp;

			/* load_probs2() already done */
			v4l2_vp9_adapt_noncoef_probs(
				&vp9_ctx->probability_tables, counts,
				vp9_ctx->cur.reference_mode,
				vp9_ctx->cur.interpolation_filter,
				vp9_ctx->cur.tx_mode, vp9_ctx->cur.flags);
		}
	}

	/* 6.1.2 refresh_probs(): save_probs(fctx_idx) */
	if (!(vp9_ctx->cur.flags & V4L2_VP9_FRAME_FLAG_PARALLEL_DEC_MODE) &&
	    !stream->failed)
		stream->frame_context[fctx_idx] = vp9_ctx->probability_tables;

out_update_last:
	if (stream->pending == f)
		stream->pending = NULL;
	vp9_put_maps(vp9_ctx);
	f->codec_priv = NULL;
}

static noinline_for_stack void
avd_init_v4l2_vp9_count_tbl(struct avd_vp9_slot *vp9_ctx)
{
	struct avd_vp9_frame_symbol_counts *cnts = vp9_ctx->bufs.counts.cpu;
	int coeff_cnts = 0, eob_cnts = 0;

	vp9_ctx->cnts.intra_inter = &cnts->intra_inter;
	vp9_ctx->cnts.y_mode = &cnts->y_mode;
	vp9_ctx->cnts.uv_mode = &cnts->uv_mode;
	vp9_ctx->cnts.comp = &cnts->comp;
	vp9_ctx->cnts.comp_ref = &cnts->comp_ref;
	vp9_ctx->cnts.single_ref = &cnts->single_ref;
	vp9_ctx->cnts.filter = &cnts->filter;
	vp9_ctx->cnts.mv_mode = &cnts->mv_mode;
	vp9_ctx->cnts.mv_joint = &cnts->mv_joint;

	/* all of the mv use a different structure, so they must all be copied */

	vp9_ctx->cnts.tx8p = &cnts->tx8p;
	/* avd also uses "u32 tx16p[2][3]" instead of "u32 tx16p[2][4]" */
	vp9_ctx->cnts.tx32p = &cnts->tx32p;

	/* it would suck if we need to reverse partition */
	vp9_ctx->cnts.partition = &cnts->partition;
	/* these should work */
	vp9_ctx->cnts.skip = &cnts->skip;

	for (int t = 0; t < 4; t++)
		for (int i = 0; i < 2; i++)
			for (int j = 0; j < 2; j++)
				for (int k = 0; k < 6; k++) {
					int max_l = (k == 0) ? 3 : 6;
					for (int l = 0; l < max_l; l++) {
						vp9_ctx->cnts.coeff[t][i][j][k][l] =
							(u32 (*)[3])&cnts->ref_cnt[coeff_cnts];
						coeff_cnts += 3;
						vp9_ctx->cnts.eob[t][i][j][k][l][0] =
							&cnts->eob_0[eob_cnts++];
						vp9_ctx->cnts.eob[t][i][j][k][l][1] =
							&cnts->ref_cnt[coeff_cnts++];
					}
				}
}

static void avd_vp9_free_slot(struct avd_ctx *ctx, struct avd_vp9_slot *slot)
{
	avd_buf_free(ctx->dev, &slot->bufs.inst);
	avd_buf_free(ctx->dev, &slot->bufs.az_left);
	avd_buf_free(ctx->dev, &slot->bufs.above_info);
	avd_buf_free(ctx->dev, &slot->bufs.ip_above);
	avd_buf_free(ctx->dev, &slot->bufs.lf_above);
	avd_buf_free(ctx->dev, &slot->bufs.lf_left_info);
	avd_buf_free(ctx->dev, &slot->bufs.lf_left);
	avd_buf_free(ctx->dev, &slot->bufs.seg);
	avd_buf_free(ctx->dev, &slot->bufs.pipe_state);
	avd_buf_free(ctx->dev, &slot->bufs.counts);
	avd_buf_free(ctx->dev, &slot->bufs.probs);
	kvfree(slot);
}

static void avd_vp9_stop(struct avd_ctx *ctx)
{
	struct avd_vp9_ctx *stream = ctx->priv;

	if (!avd_vp9_use_frame_ops(ctx)) {
		avd_vp9_legacy_fmt_ops.stop(ctx);
		return;
	}

	for (unsigned int i = 0; i < AVD_MAX_FRAMES; i++)
		if (stream->slots[i])
			avd_vp9_free_slot(ctx, stream->slots[i]);
	for (unsigned int i = 0; i < ARRAY_SIZE(stream->segmaps); i++)
		avd_buf_free(ctx->dev, &stream->segmaps[i].buf);
	kvfree(stream);
	ctx->priv = NULL;
}

static int avd_vp9_start(struct avd_ctx *ctx)
{
	struct avd_vp9_ctx *stream;

	if (!avd_vp9_use_frame_ops(ctx))
		return avd_vp9_legacy_fmt_ops.start(ctx);
	stream = kvzalloc(sizeof(*stream), GFP_KERNEL);
	if (!stream)
		return -ENOMEM;
	for (unsigned int i = 0; i < ARRAY_SIZE(stream->frame_context); i++)
		stream->frame_context[i] = v4l2_vp9_default_probs;
	ctx->priv = stream;
	return 0;
}

static int avd_vp9_prepare(struct avd_ctx *ctx, struct avd_frame *f)
{
	struct avd_vp9_ctx *stream = ctx->priv;
	struct avd_vp9_run run = { .frame = f };
	struct avd_vp9_slot *slot;
	struct avd_decoded_buffer *dst;
	struct v4l2_ctrl *ctrl;
	unsigned int idx, tiles;
	size_t header_words;
	int ret;

	if (f->idx >= AVD_MAX_FRAMES || !f->src || !f->dst ||
	    !f->prog || !f->prog->cpu)
		return -EINVAL;
	/* The core blocks the next prepare after a count-producing frame.
	 * Keep this guard for callers which do not use its readiness check. */
	f->dep = AVD_DEP_NONE;
	if (stream->pending) {
		f->dep = AVD_DEP_PREV_DONE;
		return -EAGAIN;
	}
	if (stream->failed)
		return -EIO;

	ctrl = v4l2_ctrl_find(&ctx->ctrl_hdl, V4L2_CID_STATELESS_VP9_FRAME);
	if (!ctrl)
		return -EINVAL;
	run.decode_params = ctrl->p_cur.p;
	ctrl = v4l2_ctrl_find(&ctx->ctrl_hdl,
			      V4L2_CID_STATELESS_VP9_COMPRESSED_HDR);
	if (!ctrl)
		return -EINVAL;
	run.prob_updates = ctrl->p_cur.p;
	ret = validate_dec_params(ctx, run.decode_params);
	if (ret)
		return ret;

	slot = stream->slots[f->idx];
	if (!slot) {
		slot = kvzalloc(sizeof(*slot), GFP_KERNEL);
		if (!slot)
			return -ENOMEM;
		ret = avd_vp9_alloc_bufs(ctx, slot);
		if (ret) {
			avd_vp9_free_slot(ctx, slot);
			return ret;
		}
		avd_init_v4l2_vp9_count_tbl(slot);
		stream->slots[f->idx] = slot;
	}
	ret = vp9_get_maps(stream, slot);
	if (ret)
		return ret;
	if (!slot->seg_out->buf.cpu) {
		ret = avd_buf_alloc(ctx->dev, &slot->seg_out->buf,
			DIV_ROUND_UP(fmt_width(ctx), 16) *
			DIV_ROUND_UP(fmt_height(ctx), 64) * 144);
		if (ret)
			goto err_put_maps;
	}
	run.slot = slot;
	run.base.bufs.src = f->src;
	run.base.bufs.dst = f->dst;
	run.base.coded_in = vb2_dma_contig_plane_dma_addr(&f->src->vb2_buf, 0);
	run.base.y_out = vb2_dma_contig_plane_dma_addr(&f->dst->vb2_buf, 0);
	run.base.uv_out = run.base.y_out +
		ctx->decoded_fmt.fmt.pix_mp.plane_fmt[0].bytesperline *
		ctx->decoded_fmt.fmt.pix_mp.height;
	run.base.comp_out = run.base.y_out + ctx->comp.start_offset;
	dst = vb2_to_avd_decoded_buf(&f->dst->vb2_buf);
	dst->comp = ctx->comp;
	update_dec_buf_info(dst, run.decode_params);
	slot->last = stream->last;
	update_ctx_cur_info(slot, dst, run.decode_params);
	slot->cur.tx_mode = run.prob_updates->tx_mode;

	/* Validate the complete bitstream before committing saved contexts. */
	f->prog_words = 0;
	set_header(ctx, &run);
	header_words = f->prog_words;
	ret = set_tiles(ctx, &run);
	if (ret)
		goto err_put_maps;
	memcpy(stream->working_context, stream->frame_context,
	       sizeof(stream->working_context));
	idx = v4l2_vp9_reset_frame_ctx(run.decode_params, stream->working_context);
	slot->cur.frame_context_idx = idx;
	slot->initial_probs = stream->working_context[idx];
	slot->probability_tables = slot->initial_probs;
	v4l2_vp9_fw_update_probs(&slot->probability_tables,
				 run.prob_updates, run.decode_params);
	memset(slot->bufs.counts.cpu, 0, slot->bufs.counts.size);
	init_probs(ctx, &run);
	if (avd_backend_is_apple(ctx->dev)) {
		ret = avd_vp9_pack_fp(ctx, f, run.decode_params, run.prob_updates,
				      slot->bufs.probs.cpu, slot->bufs.probs.addr);
		if (ret)
			goto err_put_maps;
	}
	tiles = (1U << run.decode_params->tile_cols_log2) *
		(1U << run.decode_params->tile_rows_log2);
	ret = vp9_finish_program(ctx, f, header_words, tiles);
	if (ret)
		goto err_put_maps;
	memcpy(stream->frame_context, stream->working_context,
	       sizeof(stream->frame_context));
	slot->tiles = tiles;
	f->fifo_iova = slot->bufs.inst.addr;
	if (!avd_backend_is_apple(ctx->dev))
		f->cmd[0] = AVD_CODEC_VP9;

	f->codec_priv = slot;
	f->dep = avd_vp9_needs_counts(slot->cur.flags) ?
		 AVD_DEP_PREV_DONE : AVD_DEP_NONE;
	if (slot->cur.flags & V4L2_VP9_FRAME_FLAG_REFRESH_FRAME_CTX) {
		if (avd_vp9_needs_counts(slot->cur.flags))
			stream->pending = f;
		else
			stream->frame_context[idx] = slot->probability_tables;
	}
	stream->last = slot->cur;
	vp9_publish_map(stream, slot);
	return 0;

err_put_maps:
	vp9_put_maps(slot);
	return ret;
}

static enum avd_image_fmt avd_vp9_get_image_fmt(struct avd_ctx *ctx,
						struct v4l2_ctrl *ctrl)
{
#define BIT_DEPTH(chroma)                                      \
	(frame->bit_depth == 8 ? AVD_IMG_FMT_##chroma##_8BIT : \
				 AVD_IMG_FMT_##chroma##_10BIT)
	const struct v4l2_ctrl_vp9_frame *frame = ctrl->p_new.p_vp9_frame;

	if (ctrl->id != V4L2_CID_STATELESS_VP9_FRAME)
		return AVD_IMG_FMT_ANY;

	/* 7.2.2 Color config semantics */
	if (frame->flags & V4L2_VP9_FRAME_FLAG_X_SUBSAMPLING) {
		if (frame->flags & V4L2_VP9_FRAME_FLAG_Y_SUBSAMPLING)
			return BIT_DEPTH(420);
		else
			return BIT_DEPTH(422);
	}

	return AVD_IMG_FMT_ANY;
#undef BIT_DEPTH
}

/* Stock firmware keeps the original builder and adaptation path verbatim. */
static int avd_vp9_legacy_run(struct avd_ctx *ctx)
{
	return avd_vp9_legacy_fmt_ops.run(ctx);
}

static void avd_vp9_legacy_done(struct avd_ctx *ctx,
			      struct vb2_v4l2_buffer *src,
			      struct vb2_v4l2_buffer *dst,
			      enum vb2_buffer_state state)
{
	avd_vp9_legacy_fmt_ops.done(ctx, src, dst, state);
}

static void avd_vp9_legacy_submit(struct avd_ctx *ctx)
{
	avd_vp9_legacy_fmt_ops.submit(ctx);
}

static unsigned int avd_vp9_legacy_pp_kicks(struct avd_ctx *ctx)
{
	return avd_vp9_legacy_fmt_ops.pp_kicks(ctx);
}

const struct avd_codec_frame_ops avd_vp9_frame_ops = {
	.prepare = avd_vp9_prepare,
	.pack_cmd = avd_vp9_pack_cmd,
	.retire = avd_vp9_retire,
};

const struct avd_coded_fmt_ops avd_vp9_fmt_ops = {
	.start = avd_vp9_start,
	.stop = avd_vp9_stop,
	.run = avd_vp9_legacy_run,
	.done = avd_vp9_legacy_done,
	.submit = avd_vp9_legacy_submit,
	.pp_kicks = avd_vp9_legacy_pp_kicks,
	.get_image_fmt = avd_vp9_get_image_fmt,
};
