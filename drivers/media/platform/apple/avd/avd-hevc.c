/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Apple AVD VPU codec driver
 *
 * Copyright The Asahi Linux Contributors
 * Copyright 2023 Eileen Yoon <eyn@gmx.com>
 *
 * Copyright (c) 2014 Rockchip Electronics Co., Ltd.
 *	Hertz Wong <hertz.wong@rock-chips.com>
 *	Herman Chen <herman.chen@rock-chips.com>
 *
 * Copyright (C) 2014 Google, Inc.
 *	Tomasz Figa <tfiga@chromium.org>
 */

#include <media/videobuf2-dma-contig.h>

#include "avd.h"
#include "avd-inst.h"
#include "avd-hevc-fp.h"

#define NEW_TILE_ID	BIT(0)
#define NEW_SLICE	BIT(1)

#define HEVC_PCM_EN(v)		FIELD_PREP(BIT(12), !!(v))
#define HEVC_PCM_BD_LUMA(v)	FIELD_PREP(GENMASK(11, 8), v)
#define HEVC_PCM_BD_CHROMA(v)	FIELD_PREP(GENMASK(7, 4), v)
#define HEVC_PCM_CB_MIN_LUMA(v)	FIELD_PREP(GENMASK(3, 2), v)
#define HEVC_PCM_CB_LUMA(v)	FIELD_PREP(GENMASK(1, 0), v)

#define HEVC_UNK_ISM_EN(v)	FIELD_PREP(BIT(9), !!(v))
#define HEVC_UNK_FLAG		FIELD_PREP(BIT(3), 1)

#define HEVC_CTB_SIZE(v)	FIELD_PREP(GENMASK(8, 3), v)
#define HEVC_MERGE_LV(v)	FIELD_PREP(GENMASK(11, 9), v)
#define HEVC_FLAG_ENTROPY_EN(v)	FIELD_PREP(BIT(12), !!(v))
#define HEVC_FLAG_TILES_EN(v)	FIELD_PREP(BIT(13), !!(v))
#define HEVC_FLAG_TQB_EN(v)	FIELD_PREP(BIT(14), !!(v))
#define HEVC_CU_QP_DD(v)	FIELD_PREP(GENMASK(16, 15), v)
#define HEVC_FLAG_CU_QP_EN(v)	FIELD_PREP(BIT(17), !!(v))
#define HEVC_FLAG_TSKIP_EN(v)	FIELD_PREP(BIT(18), !!(v))
#define HEVC_FLAG_CI_PRED(v)	FIELD_PREP(BIT(19), !!(v))
#define HEVC_FLAG_SDH_EN(v)	FIELD_PREP(BIT(20), !!(v))
#define HEVC_FLAG_TMVP_EN(v)	FIELD_PREP(BIT(21), !!(v))

#define HEVC_SCL_DIMS	0x127ffff
#define HEVC_DMA_CONFIG	BIT(26)

static inline u32 mv_color_size(u32 w, u32 h)
{
	/* this will waste some memory when max cu size != 64 */
	return DIV_ROUND_UP(w, 64) * DIV_ROUND_UP(h, 64) * 256;
}

struct avd_hevc_tile_info {
	u16 col_width[22];
	u16 row_height[22];
	u32 col_bd[23];
	u32 row_bd[23];
	u32 *ctb_addr_rs_to_ts;
	u32 *tile_ids;
};

struct avd_hevc_bufs {
	struct avd_buf mv_above_info;
	struct avd_buf az_above;
	struct avd_buf ip_above;
	struct avd_buf lf_above;
	struct avd_buf lf_above_info;
	struct avd_buf lf_left;
	struct avd_buf lf_left_info;
	struct avd_buf sw_left;
	struct avd_buf pipe_state;
};

struct avd_hevc_run {
	size_t *segment_ends;
	unsigned int segments;
	unsigned int submit_num;
	struct avd_frame *frame;
	struct avd_hevc_bufs *bufs;
	struct avd_hevc_slice_range *ranges;
	struct avd_hevc_fp_builder fp;
	int error;
	struct avd_run base;
	const struct v4l2_ctrl_hevc_slice_params *sl;
	const struct v4l2_ctrl_hevc_decode_params *decode;
	const struct v4l2_ctrl_hevc_sps *sps;
	const struct v4l2_ctrl_hevc_pps *pps;
	const struct v4l2_ctrl_hevc_scaling_matrix *scaling_matrix;
	const u32 *entry_point_offsets;

	int num_entry_point_offsets;
	int num_slices;

	struct run_addr {
		dma_addr_t mv_color;
	} addresses;

	struct avd_hevc_tile_info tile_info;
};

struct avd_hevc_ctx {
	struct avd_hevc_bufs *slots;
};

/* Preserve instruction words while changing their destination to slot DMA. */
static void avd_hevc_push(struct avd_hevc_run *run, u32 word,
			 enum avd_hevc_fp_field field)
{
	struct avd_frame *f = run->frame;

	if (f->prog_words >= f->prog->size / sizeof(u32)) {
		run->error = -ENOSPC;
		return;
	}
	((__le32 *)f->prog->cpu)[f->prog_words++] = cpu_to_le32(word);
	avd_hevc_fp_emit(&run->fp, field, word);
}

static void avd_hevc_address(struct avd_ctx *ctx, struct avd_hevc_run *run,
			     dma_addr_t addr, enum avd_hevc_fp_field field,
			     unsigned int index)
{
	avd_hevc_fp_address(&run->fp, field, addr, index);
	if (ctx->dev->variant->quirks & AVD_QUIRK_LSR) {
		avd_hevc_push(run, addr >> 8, HEVC_FP_NONE);
	} else {
		avd_hevc_push(run, lower_32_bits(addr), HEVC_FP_NONE);
		avd_hevc_push(run, upper_32_bits(addr), HEVC_FP_NONE);
	}
}

static void avd_hevc_comp(struct avd_ctx *ctx, struct avd_hevc_run *run,
			  dma_addr_t addr, u32 offsets[4], unsigned int index)
{
	unsigned int i;

	avd_hevc_fp_reference(&run->fp, index, addr, offsets);
	for (i = 0; i < 4; i++) {
		if (ctx->dev->variant->quirks & AVD_QUIRK_LSR)
			avd_hevc_push(run, (addr + offsets[i]) >> 7, HEVC_FP_NONE);
		else
			avd_hevc_address(ctx, run, addr + offsets[i], HEVC_FP_NONE, 0);
	}
}

#undef push
#undef pusha
#define push(word, field) avd_hevc_push(run, word, field)
#define pusha(addr, field, index) avd_hevc_address(ctx, run, addr, field, index)

static void stream_refs(struct avd_ctx *ctx, struct avd_hevc_run *run)
{
	const struct v4l2_ctrl_hevc_decode_params *decode = run->decode;
	const struct v4l2_ctrl_hevc_slice_params *sl = &run->sl[0];
	const struct v4l2_hevc_dpb_entry *dpb;
	struct avd_decoded_buffer *dst, *ref_buf;

	dst = vb2_to_avd_decoded_buf(&run->base.bufs.dst->vb2_buf);

	push(ctx->dev->variant->quirks & AVD_QUIRK_HEVC_V5_HEADER ?
		     HEVC_DMA_CONFIG : 0, HEVC_FP_NONE);
	pusha(run->bufs->mv_above_info.addr, HEVC_FP_NONE, 7);
	pusha(run->addresses.mv_color, HEVC_FP_NONE, 0);

	push(0, HEVC_FP_NONE);
	push(0, HEVC_FP_NONE);
	push(0, HEVC_FP_NONE);
	push(0, HEVC_FP_NONE);

	for (int i = 0; i < decode->num_active_dpb_entries; i++) {
		dpb = &decode->dpb[i];

		ref_buf = avd_get_ref_buf(ctx, &dst->base.vb, dpb->timestamp);

		dma_addr_t comp_addr = vb2_dma_contig_plane_dma_addr(
					       &ref_buf->base.vb.vb2_buf, 0) +
				       ref_buf->comp.start_offset;

		push(AVD_REF_NUM(decode->num_active_dpb_entries - 1) |
			     AVD_REF_FLAG_CONST |
			     AVD_REF_FLAG_LONG(
				     dpb->flags &
				     V4L2_HEVC_DPB_ENTRY_LONG_TERM_REFERENCE) |
			     AVD_REF_DELTA_POC(sl->slice_pic_order_cnt -
					       dpb->pic_order_cnt_val),
		     HEVC_FP_REF_HDR);

		avd_hevc_comp(ctx, run, comp_addr, ref_buf->comp.offsets, i + 1);
	}
}

static void set_scaling_lists(struct avd_ctx *ctx, struct avd_hevc_run *run)
{
	const struct v4l2_ctrl_hevc_scaling_matrix *s = run->scaling_matrix;
	int i, j, k;

	u8 (*dc_16x16)[3] = (u8(*)[3])s->scaling_list_dc_coef_16x16;
	u8 (*sc_4x4)[4][4] = (u8(*)[4][4])s->scaling_list_4x4;
	u8 (*sc_8x8)[2][4][8] = (u8(*)[2][4][8])s->scaling_list_8x8;
	u8 (*sc_16x16)[2][4][8] = (u8(*)[2][4][8])s->scaling_list_16x16;
	u8 (*sc_32x32)[2][4][8] = (u8(*)[2][4][8])s->scaling_list_32x32;

	/*
	 * this should presumably be how many of each are enabled?
	 * or some other scaling related thing
	 */
	push(HEVC_SCL_DIMS, HEVC_FP_SCALING_DIMS);

	for (i = 0; i < 2; i++)
		push(AVD_SCALING_I2(dc_16x16[i][0]) |
			     AVD_SCALING_I1(dc_16x16[i][1]) |
			     AVD_SCALING_I0(dc_16x16[i][2]),
		     HEVC_FP_DC16);

	for (i = 0; i < 2; i++)
		push(AVD_SCALING_I2(s->scaling_list_dc_coef_32x32[i]),
		     HEVC_FP_DC32);

	/* transposed in stride 4 */
	for (i = 0; i < 6; i++)
		for (j = 0; j < 4; j++)
			push(AVD_SCALING_I3(sc_4x4[i][0][j]) |
				     AVD_SCALING_I2(sc_4x4[i][1][j]) |
				     AVD_SCALING_I1(sc_4x4[i][2][j]) |
				     AVD_SCALING_I0(sc_4x4[i][3][j]),
			     HEVC_FP_SCALING4);

	/* transposed in stride 8 */
	for (i = 0; i < 6; i++)
		for (j = 0; j < 2; j++)
			for (k = 0; k < 8; k++)
				push(AVD_SCALING_I3(sc_8x8[i][j][0][k]) |
					     AVD_SCALING_I2(
						     sc_8x8[i][j][1][k]) |
					     AVD_SCALING_I1(
						     sc_8x8[i][j][2][k]) |
					     AVD_SCALING_I0(sc_8x8[i][j][3][k]),
				     HEVC_FP_SCALING8);

	for (i = 0; i < 6; i++)
		for (j = 0; j < 2; j++)
			for (k = 0; k < 8; k++)
				push(AVD_SCALING_I3(sc_16x16[i][j][0][k]) |
					     AVD_SCALING_I2(
						     sc_16x16[i][j][1][k]) |
					     AVD_SCALING_I1(
						     sc_16x16[i][j][2][k]) |
					     AVD_SCALING_I0(
						     sc_16x16[i][j][3][k]),
				     HEVC_FP_SCALING16);

	for (i = 0; i < 2; i++)
		for (j = 0; j < 2; j++)
			for (k = 0; k < 8; k++)
				push(AVD_SCALING_I3(sc_32x32[i][j][0][k]) |
					     AVD_SCALING_I2(
						     sc_32x32[i][j][1][k]) |
					     AVD_SCALING_I1(
						     sc_32x32[i][j][2][k]) |
					     AVD_SCALING_I0(
						     sc_32x32[i][j][3][k]),
				     HEVC_FP_SCALING32);
}

static void hevc_set_flags(struct avd_ctx *ctx, struct avd_hevc_run *run)
{
	const struct v4l2_ctrl_hevc_decode_params *decode = run->decode;
	const struct v4l2_ctrl_hevc_sps *sps = run->sps;
	const struct v4l2_ctrl_hevc_pps *pps = run->pps;

	u32 log2_ctb_size = ((sps->log2_min_luma_coding_block_size_minus3) +
			     sps->log2_diff_max_min_luma_coding_block_size);

	push(HEVC_PCM_EN(sps->flags & V4L2_HEVC_SPS_FLAG_PCM_ENABLED) |
		     HEVC_PCM_BD_LUMA(sps->pcm_sample_bit_depth_luma_minus1) |
		     HEVC_PCM_BD_CHROMA(
			     sps->pcm_sample_bit_depth_chroma_minus1) |
		     HEVC_PCM_CB_MIN_LUMA(
			     sps->log2_min_pcm_luma_coding_block_size_minus3) |
		     HEVC_PCM_CB_LUMA(
			     sps->log2_diff_max_min_pcm_luma_coding_block_size +
			     sps->log2_min_pcm_luma_coding_block_size_minus3),
	     HEVC_FP_PCM);

	/*
	 * RExt sets a few new flags here
	 */
	push(HEVC_UNK_FLAG |
		     HEVC_UNK_ISM_EN(
			     sps->flags &
			     V4L2_HEVC_SPS_FLAG_STRONG_INTRA_SMOOTHING_ENABLED),
	     HEVC_FP_FLAGS);

	push(HEVC_CTB_SIZE(log2_ctb_size) |
		     HEVC_MERGE_LV(pps->log2_parallel_merge_level_minus2) |
		     HEVC_FLAG_ENTROPY_EN(
			     pps->flags &
			     V4L2_HEVC_PPS_FLAG_ENTROPY_CODING_SYNC_ENABLED) |
		     HEVC_FLAG_TILES_EN(pps->flags &
					V4L2_HEVC_PPS_FLAG_TILES_ENABLED) |
		     HEVC_FLAG_TQB_EN(
			     pps->flags &
			     V4L2_HEVC_PPS_FLAG_TRANSQUANT_BYPASS_ENABLED) |
		     HEVC_CU_QP_DD(log2_ctb_size -
				   pps->diff_cu_qp_delta_depth) |
		     HEVC_FLAG_CU_QP_EN(
			     pps->flags &
			     V4L2_HEVC_PPS_FLAG_CU_QP_DELTA_ENABLED) |
		     HEVC_FLAG_TSKIP_EN(
			     pps->flags &
			     V4L2_HEVC_PPS_FLAG_TRANSFORM_SKIP_ENABLED) |
		     HEVC_FLAG_CI_PRED(
			     pps->flags &
			     V4L2_HEVC_PPS_FLAG_CONSTRAINED_INTRA_PRED) |
		     HEVC_FLAG_SDH_EN(
			     pps->flags &
			     V4L2_HEVC_PPS_FLAG_SIGN_DATA_HIDING_ENABLED) |
		     HEVC_FLAG_TMVP_EN(
			     !(decode->flags &
			       V4L2_HEVC_DECODE_PARAM_FLAG_IDR_PIC) &&
			     sps->flags &
				     V4L2_HEVC_SPS_FLAG_SPS_TEMPORAL_MVP_ENABLED),
	     HEVC_FP_PPS_FLAGS);

	push(AVD_HDR_H26X_QP_OFFSET_CB(pps->pps_cb_qp_offset) |
		     AVD_HDR_H26X_QP_OFFSET_CR(pps->pps_cr_qp_offset),
	     HEVC_FP_PPS_QP);

	/*
	 * something like segment features?
	 */
	push(0, HEVC_FP_NONE);
	push(0, HEVC_FP_NONE);
	push(0, HEVC_FP_NONE);
	push(0, HEVC_FP_NONE);
	push(0, HEVC_FP_NONE);
	push(0, HEVC_FP_NONE);
}

static void set_header(struct avd_ctx *ctx, struct avd_hevc_run *run)
{
	const struct v4l2_ctrl_hevc_sps *sps = run->sps;
	const struct v4l2_ctrl_hevc_pps *pps = run->pps;
	struct avd_dev *avd = ctx->dev;
	u32 bytesperline;
	u32 width = sps->pic_width_in_luma_samples;
	u32 height = sps->pic_height_in_luma_samples;
	u32 dma_config = avd->variant->quirks & AVD_QUIRK_HEVC_V5_HEADER ?
			 HEVC_DMA_CONFIG : 0;

	bool is_intra = run->sl[0].slice_type == V4L2_HEVC_SLICE_TYPE_I;

	push(AVD_OP_HDR | AVD_OP_HDR_FLAG_DECOMP(ctx->decomp) |
		     AVD_OP_HDR_FLAG_INTRA(is_intra) | AVD_OP_HDR_CONST |
		     AVD_OP_HDR_FLAG_PIPE_STATE(
			     !(avd->variant->quirks & AVD_QUIRK_NO_PIPE_STATE)),
	     HEVC_FP_START);

	push(AVD_HDR_CODEC_MODE(AVD_CODEC_HEVC), HEVC_FP_MODE);
	push(AVD_HDR_HEIGHT(height - 1) | AVD_HDR_WIDTH(width - 1),
	     HEVC_FP_DIMS);
	push(0, HEVC_FP_PIXFMT);

	push(AVD_HDR_HEIGHT((height - 1) >> 3) |
		     AVD_HDR_WIDTH((width - 1) >> 3),
	     HEVC_FP_DIMS8);

	push(AVD_HDR_COMMON_CHROMA_FORMAT(sps->chroma_format_idc) |
		     AVD_HDR_COMMON_BIT_DEPTH_C(sps->bit_depth_chroma_minus8) |
		     AVD_HDR_COMMON_BIT_DEPTH_L(sps->bit_depth_luma_minus8) |
		     AVD_HDR_COMMON_MIN_LUMA_CBS(
			     sps->log2_min_luma_coding_block_size_minus3) |
		     AVD_HDR_COMMON_LUMA_CBS(
			     sps->log2_diff_max_min_luma_coding_block_size +
			     sps->log2_min_luma_coding_block_size_minus3) |
		     AVD_HDR_COMMON_MIN_LUMA_TBS(
			     sps->log2_min_luma_transform_block_size_minus2) |
		     AVD_HDR_COMMON_LUMA_TBS(
			     sps->log2_diff_max_min_luma_transform_block_size +
			     +sps->log2_min_luma_transform_block_size_minus2) |
		     AVD_HDR_COMMON_TR_INTER(
			     sps->max_transform_hierarchy_depth_inter) |
		     AVD_HDR_COMMON_TR_INTRA(
			     sps->max_transform_hierarchy_depth_intra) |
		     AVD_HDR_COMMON_FLAG0(sps->flags &
					  V4L2_HEVC_SPS_FLAG_AMP_ENABLED),
	     HEVC_FP_TXFM);

	hevc_set_flags(ctx, run);

	push(AVD_HDR_FEAT_H26X | AVD_HDR_FEAT_COMMON |
		     AVD_HDR_FEAT_PIPE_STATE_EN(
			     !(avd->variant->quirks & AVD_QUIRK_NO_PIPE_STATE)),
	     HEVC_FP_FEATURES);

	push(dma_config, HEVC_FP_NONE);
	push(0, HEVC_FP_NONE);

	if (avd->variant->revision == 3)
		push(0, HEVC_FP_NONE);

	push(dma_config, HEVC_FP_NONE);
	push(dma_config, HEVC_FP_NONE);

	if (avd->variant->revision == 3)
		push(0, HEVC_FP_NONE);
	else if (!(avd->variant->quirks & AVD_QUIRK_NO_PIPE_STATE))
		pusha(run->bufs->pipe_state.addr, HEVC_FP_NONE, 0);

	pusha(run->bufs->ip_above.addr, HEVC_FP_SCRATCH, 0);
	pusha(run->bufs->lf_above.addr, HEVC_FP_SCRATCH, 1);
	pusha(run->bufs->lf_above_info.addr, HEVC_FP_SCRATCH, 2);

	if ((pps->flags & V4L2_HEVC_PPS_FLAG_TILES_ENABLED) ||
	    !(avd->variant->quirks & AVD_QUIRK_HEVC_V5_HEADER)) {
		pusha(run->bufs->lf_left.addr,
		      HEVC_FP_SCRATCH, 3);
		pusha(run->bufs->lf_left_info.addr,
		      HEVC_FP_SCRATCH, 4);
		if (avd->variant->quirks & AVD_QUIRK_HEVC_V5_HEADER)
			pusha(0, HEVC_FP_NONE, 0);
		pusha(run->bufs->az_above.addr,
		      HEVC_FP_SCRATCH, 8);
		pusha(run->bufs->sw_left.addr,
		      HEVC_FP_SCRATCH, 9);
	} else {
		pusha(0, HEVC_FP_NONE, 3);
		pusha(0, HEVC_FP_NONE, 4);
		if (avd->variant->quirks & AVD_QUIRK_HEVC_V5_HEADER)
			pusha(0, HEVC_FP_NONE, 0);
		pusha(run->bufs->az_above.addr,
		      HEVC_FP_SCRATCH, 8);
		pusha(0, HEVC_FP_NONE, 9);
	}

	push(0, HEVC_FP_NONE);

	avd_hevc_comp(ctx, run, run->base.comp_out, ctx->comp.offsets, 0);

	pusha((u64)0, HEVC_FP_NONE, 0);

	bytesperline = ctx->decoded_fmt.fmt.pix_mp.plane_fmt[0].bytesperline;
	if (avd->variant->quirks & AVD_QUIRK_LSR)
		bytesperline = bytesperline >> 4;

	pusha(run->base.y_out, HEVC_FP_Y, 0);
	push(bytesperline, HEVC_FP_Y_STRIDE);
	pusha(run->base.uv_out, HEVC_FP_UV, 0);
	push(bytesperline, HEVC_FP_UV_STRIDE);
	push(0, HEVC_FP_NONE);
	push(AVD_HDR_HEIGHT(height - 1) | AVD_HDR_WIDTH(width - 1),
	     HEVC_FP_DIMS);

	if (!is_intra)
		stream_refs(ctx, run);

	if ((sps->flags & V4L2_HEVC_SPS_FLAG_SCALING_LIST_ENABLED))
		set_scaling_lists(ctx, run);
	else
		push(0, HEVC_FP_NONE);
}

static void stream_weights(struct avd_ctx *ctx, struct avd_hevc_run *run,
			   const struct v4l2_ctrl_hevc_slice_params *sl)
{
	int luma_weight_denom, chroma_weight_denom;
	u8 chroma_log2_weight_denom;
	const struct v4l2_ctrl_hevc_pps *pps = run->pps;
	const struct v4l2_hevc_pred_weight_table *pred = &sl->pred_weight_table;
	bool has_luma_weights =
		((pps->flags & V4L2_HEVC_PPS_FLAG_WEIGHTED_PRED) &&
		 sl->slice_type == V4L2_HEVC_SLICE_TYPE_P) ||
		((pps->flags & V4L2_HEVC_PPS_FLAG_WEIGHTED_BIPRED) &&
		 sl->slice_type == V4L2_HEVC_SLICE_TYPE_B);

	const s8(*delta_chroma_weights)[2];
	const s8(*chroma_offsets)[2];
	const s8 *delta_luma_weights;
	const s8 *luma_offsets;

	if (!has_luma_weights) {
		push(AVD_OP_WEIGHTS_HDR, HEVC_FP_WEIGHT_DENOM);
		return;
	}

	chroma_log2_weight_denom = pred->luma_log2_weight_denom +
				   pred->delta_chroma_log2_weight_denom;

	push(AVD_OP_WEIGHTS_HDR | AVD_OP_WEIGHTS_HDR_FLAG1(!has_luma_weights) |
		     AVD_OP_WEIGHTS_HDR_FLAG0(has_luma_weights) |
		     AVD_OP_WEIGHTS_HDR_LUMA(pred->luma_log2_weight_denom) |
		     AVD_OP_WEIGHTS_HDR_CHROMA(chroma_log2_weight_denom),
	     HEVC_FP_WEIGHT_DENOM);

	luma_weight_denom = 1 << pred->luma_log2_weight_denom;
	chroma_weight_denom = 1 << chroma_log2_weight_denom;

	/* comes from 7.4.7.3 */

	for (int y = 0; y < 2; y++) {
		if (y == 1 && sl->slice_type != V4L2_HEVC_SLICE_TYPE_B)
			break;
		luma_offsets = y == 0 ? pred->luma_offset_l0 :
					pred->luma_offset_l1;
		delta_luma_weights =
			(s8(*))(y == 0 ? pred->delta_luma_weight_l0 :
					 pred->delta_luma_weight_l1);
		chroma_offsets = (s8(*)[2])(y == 0 ? pred->chroma_offset_l0 :
						     pred->chroma_offset_l1);
		delta_chroma_weights =
			(s8(*)[2])(y == 0 ? pred->delta_chroma_weight_l0 :
					    pred->delta_chroma_weight_l1);

		int to = y == 0 ? sl->num_ref_idx_l0_active_minus1 :
				  sl->num_ref_idx_l1_active_minus1;
		for (int i = 0; i < to + 1; i++) {
			/* Avd only expects offsets/weights if they are not the default
			 * ones, otherwise we get artifacts */
			if ((delta_luma_weights[i] != 0) ||
			    (luma_offsets[i] != 0)) {
				push(AVD_OP_WEIGHTS | AVD_OP_WEIGHTS_IDENT(1) |
					     AVD_OP_WEIGHTS_LIST_IDX(y) |
					     AVD_OP_WEIGHTS_INDEX(i) |
					     AVD_OP_WEIGHTS_WEIGHT(
						     delta_luma_weights[i] +
						     luma_weight_denom),
				     HEVC_FP_WEIGHTS);
				push(AVD_OP_OFFSETS | AVD_OP_OFFSETS_OFFSET(
							      luma_offsets[i]),
				     HEVC_FP_WEIGHT_OFFSETS);
			}

			if ((delta_chroma_weights[i][0] != 0) ||
			    (chroma_offsets[i][0] != 0) ||
			    (delta_chroma_weights[i][1] != 0) ||
			    (chroma_offsets[i][1] != 0)) {
				push(AVD_OP_WEIGHTS | AVD_OP_WEIGHTS_IDENT(2) |
					     AVD_OP_WEIGHTS_LIST_IDX(y) |
					     AVD_OP_WEIGHTS_INDEX(i) |
					     AVD_OP_WEIGHTS_WEIGHT(
						     delta_chroma_weights[i][0] +
						     chroma_weight_denom),
				     HEVC_FP_WEIGHTS);
				push(AVD_OP_OFFSETS |
					     AVD_OP_OFFSETS_OFFSET(
						     chroma_offsets[i][0]),
				     HEVC_FP_WEIGHT_OFFSETS);
				push(AVD_OP_WEIGHTS | AVD_OP_WEIGHTS_IDENT(3) |
					     AVD_OP_WEIGHTS_LIST_IDX(y) |
					     AVD_OP_WEIGHTS_INDEX(i) |
					     AVD_OP_WEIGHTS_WEIGHT(
						     delta_chroma_weights[i][1] +
						     chroma_weight_denom),
				     HEVC_FP_WEIGHTS);
				push(AVD_OP_OFFSETS |
					     AVD_OP_OFFSETS_OFFSET(
						     chroma_offsets[i][1]),
				     HEVC_FP_WEIGHT_OFFSETS);
			}
		}
	}
}

static void stream_slice_dqtblk(struct avd_ctx *ctx, struct avd_hevc_run *run,
				const struct v4l2_ctrl_hevc_slice_params *sl)
{
	const struct v4l2_ctrl_hevc_pps *pps = run->pps;
	const struct v4l2_ctrl_hevc_sps *sps = run->sps;

	push(AVD_OP_QP |
		     AVD_OP_QP_VAL(pps->init_qp_minus26 + 26 +
				   sl->slice_qp_delta) |
		     AVD_OP_QP_CB_OFF(pps->pps_cb_qp_offset +
				      sl->slice_cb_qp_offset) |
		     AVD_OP_QP_CR_OFF(pps->pps_cr_qp_offset +
				      sl->slice_cr_qp_offset),
	     HEVC_FP_QUANT);

	push(AVD_OP_DBLK |
		     AVD_OP_DBLK_FLAG_SAO_CHROMA(
			     sl->flags &
			     V4L2_HEVC_SLICE_PARAMS_FLAG_SLICE_SAO_CHROMA) |
		     AVD_OP_DBLK_FLAG_SAO_LUMA(
			     sl->flags &
			     V4L2_HEVC_SLICE_PARAMS_FLAG_SLICE_SAO_LUMA) |
		     AVD_OP_DBLK_OFF0(sl->slice_tc_offset_div2) |
		     AVD_OP_DBLK_OFF1(sl->slice_beta_offset_div2) |
		     /* i wonder what this should actually be */
		     AVD_OP_DBLK_FLAG_EN(
			     !(sl->flags &
			       V4L2_HEVC_SLICE_PARAMS_FLAG_SLICE_DEBLOCKING_FILTER_DISABLED) &&
			     (sps->flags &
				      V4L2_HEVC_SPS_FLAG_STRONG_INTRA_SMOOTHING_ENABLED ||
			      sl->flags &
				      V4L2_HEVC_SLICE_PARAMS_FLAG_SLICE_SAO_LUMA ||
			      pps->flags &
				      (V4L2_HEVC_PPS_FLAG_DEBLOCKING_FILTER_OVERRIDE_ENABLED |
				       V4L2_HEVC_PPS_FLAG_DEBLOCKING_FILTER_CONTROL_PRESENT))) |
		     AVD_OP_DBLK_FLAG_FULL_EN(
			     sl->flags &
			     V4L2_HEVC_SLICE_PARAMS_FLAG_SLICE_LOOP_FILTER_ACROSS_SLICES_ENABLED) |
		     AVD_OP_DBLK_FLAG_TILES_EN(
			     !(pps->flags & V4L2_HEVC_PPS_FLAG_TILES_ENABLED) ||
			     (pps->flags &
			      V4L2_HEVC_PPS_FLAG_LOOP_FILTER_ACROSS_TILES_ENABLED)) |
		     AVD_OP_DBLK_FLAG_PCM_EN(
			     (sps->flags & V4L2_HEVC_SPS_FLAG_PCM_ENABLED) &&
			     !(sps->flags &
			       V4L2_HEVC_SPS_FLAG_PCM_LOOP_FILTER_DISABLED)),
	     HEVC_FP_DEBLOCK);

	if (sl->slice_type == V4L2_HEVC_SLICE_TYPE_B ||
	    sl->slice_type == V4L2_HEVC_SLICE_TYPE_P) {
		for (int i = 0; i < sl->num_ref_idx_l0_active_minus1 + 1; i++)
			push(AVD_OP_REF | AVD_OP_REF_LIST_IDX(0) |
				     AVD_OP_REF_LOOP_IDX(i) |
				     AVD_OP_REF_DBP_IDX(sl->ref_idx_l0[i]),
			     HEVC_FP_REF_LIST);
		if (sl->slice_type == V4L2_HEVC_SLICE_TYPE_B)
			for (int i = 0;
			     i < sl->num_ref_idx_l1_active_minus1 + 1; i++)
				push(AVD_OP_REF | AVD_OP_REF_LIST_IDX(1) |
					     AVD_OP_REF_LOOP_IDX(i) |
					     AVD_OP_REF_DBP_IDX(
						     sl->ref_idx_l1[i]),
				     HEVC_FP_REF_LIST);

		stream_weights(ctx, run, sl);
	}
}
static void stream_slice_mv(struct avd_ctx *ctx, struct avd_hevc_run *run,
			    const struct v4l2_ctrl_hevc_slice_params *sl,
			    bool is_first)
{
	const struct v4l2_ctrl_hevc_decode_params *decode = run->decode;
	struct avd_decoded_buffer *dst, *ref;
	bool ref_valid;
	const u8 *ref_list;

	if (sl->slice_type == V4L2_HEVC_SLICE_TYPE_I) {
		push(AVD_OP_SL_REF |
			     AVD_OP_SL_REF_SLICE_I(sl->slice_type ==
						   V4L2_HEVC_SLICE_TYPE_I),
		     HEVC_FP_REF_TYPE);
		return;
	}
	/* bidirectional prediction */

	ref_list = sl->slice_type == V4L2_HEVC_SLICE_TYPE_P ? sl->ref_idx_l0 :
		   sl->flags & V4L2_HEVC_SLICE_PARAMS_FLAG_COLLOCATED_FROM_L0 ?
							      sl->ref_idx_l0 :
							      sl->ref_idx_l1;

	dst = vb2_to_avd_decoded_buf(&run->base.bufs.dst->vb2_buf);
	ref = avd_get_ref_buf(
		ctx, &dst->base.vb,
		decode->dpb[ref_list[sl->collocated_ref_idx]].timestamp);

	ref_valid = !(sl->flags &
		      V4L2_HEVC_SLICE_PARAMS_FLAG_DEPENDENT_SLICE_SEGMENT) &&
		    (sl->flags &
		     V4L2_HEVC_SLICE_PARAMS_FLAG_SLICE_TEMPORAL_MVP_ENABLED) &&
		    is_first && !ref->hevc.is_intra;

	push(AVD_OP_SL_REF |
		     AVD_OP_SL_REF_MAX_MERGE(
			     5 - sl->five_minus_max_num_merge_cand) |
		     AVD_OP_SL_REF_FLAG_CABAC(
			     sl->flags &
			     V4L2_HEVC_SLICE_PARAMS_FLAG_CABAC_INIT) |
		     AVD_OP_SL_REF_FLAG0(
			     (sl->flags &
			      V4L2_HEVC_SLICE_PARAMS_FLAG_SLICE_TEMPORAL_MVP_ENABLED) &&
			     !(sl->flags &
			       V4L2_HEVC_SLICE_PARAMS_FLAG_COLLOCATED_FROM_L0)) |
		     AVD_OP_SL_REF_FLAG1(
			     !(sl->flags &
			       V4L2_HEVC_SLICE_PARAMS_FLAG_MVD_L1_ZERO)) |
		     AVD_OP_SL_REF_FLAG2(
			     (sl->flags &
			      V4L2_HEVC_SLICE_PARAMS_FLAG_SLICE_TEMPORAL_MVP_ENABLED) ||
			     (sl->flags &
			      V4L2_HEVC_SLICE_PARAMS_FLAG_DEPENDENT_SLICE_SEGMENT)) |
		     AVD_OP_SL_REF_NUM_L0(sl->num_ref_idx_l0_active_minus1) |
		     AVD_OP_SL_REF_NUM_L1(sl->num_ref_idx_l1_active_minus1) |
		     AVD_OP_SL_REF_SLICE_P(sl->slice_type ==
					   V4L2_HEVC_SLICE_TYPE_P) |
		     AVD_OP_SL_REF_SLICE_B(ref_valid),
	     HEVC_FP_REF_TYPE);

	if (ref_valid) {
		dma_addr_t mv_color_addr =
			vb2_dma_contig_plane_dma_addr(&ref->base.vb.vb2_buf,
						      0) +
			round_down(ref->base.vb.planes[0].length -
				   mv_color_size(fmt_width(ctx), fmt_height(ctx)),
				   AVD_ALIGN);
		pusha(mv_color_addr, HEVC_FP_MV,
		      decode->dpb[ref_list[sl->collocated_ref_idx]]
			      .pic_order_cnt_val);
	}
}

static void set_slice(struct avd_ctx *ctx, struct avd_hevc_run *run,
		      const struct v4l2_ctrl_hevc_slice_params *sl, u32 size,
		      u32 offset, u32 flags)
{
	dma_addr_t slc_addr =
		run->base.coded_in + offset;
	push(AVD_OP_CODED_DATA | flags | AVD_OP_CODED_DATA_ADDR(slc_addr >> 32),
	     HEVC_FP_NONE);
	push((u32)(slc_addr & 0xffffffff), HEVC_FP_SLICE_ADDR);
	push(size, HEVC_FP_SLICE_SIZE);
}

static int submit_slice_segment(struct avd_ctx *ctx, struct avd_hevc_run *run,
				const struct v4l2_ctrl_hevc_slice_params *sl,
				int row, int col, u32 col_bd[23],
				u32 row_bd[23], u32 pic_in_cts_width,
				u32 pic_in_cts_height, bool first_slice,
				bool hflip, bool vflip, u32 coded_flags,
				u32 last_tile_block)
{
	const struct v4l2_ctrl_hevc_pps *pps = run->pps;
	u32 tb_x, tb_y, tile_block, tile_boundary;

	if (coded_flags & NEW_SLICE) {
		tb_x = sl->slice_segment_addr % pic_in_cts_width;
		tb_y = sl->slice_segment_addr / pic_in_cts_width;

		tile_block = AVD_OP_SL_LOC_Y(tb_y) | AVD_OP_SL_LOC_X(tb_x);

		if (!(sl->flags &
		      V4L2_HEVC_SLICE_PARAMS_FLAG_DEPENDENT_SLICE_SEGMENT))
			last_tile_block = tile_block;

		/*
		 * tile block start
		 * CABAC window
		 */
		push(AVD_OP_SL_LOC | last_tile_block, HEVC_FP_NONE);

		stream_slice_dqtblk(ctx, run, sl);
	} else {
		tile_boundary = AVD_OP_SL_LOC_Y(row_bd[row]) |
				AVD_OP_SL_LOC_X(col_bd[col]);
	}

	if (coded_flags & NEW_TILE_ID) {
		push(AVD_OP_SL_DIM_START |
			     (coded_flags & NEW_SLICE ? tile_block :
							tile_boundary),
		     HEVC_FP_NONE);

		/* tile boundary end */
		if (pps->flags & V4L2_HEVC_PPS_FLAG_TILES_ENABLED)
			push(AVD_SL_DIM_END_ROW((hflip ? 4 : 0) |
						(vflip ? 8 : 0)) |
				     AVD_SL_DIM_END_COL(col) |
				     AVD_SL_DIM_END_Y(row_bd[row + 1] - 1) |
				     AVD_SL_DIM_END_X(col_bd[col + 1] - 1),
			     HEVC_FP_NONE);
		else /* first slice, one CTB */
			push(AVD_SL_DIM_END_Y(pic_in_cts_height - 1) |
				     AVD_SL_DIM_END_X(pic_in_cts_width - 1),
			     HEVC_FP_NONE);
	}

	if (coded_flags & NEW_SLICE)
		stream_slice_mv(ctx, run, sl, first_slice);

	/* current tile block / boundary ?? */
	/* Unlike entropy, motion vector window resets every time */
	push(AVD_SL_DIM_END_COL(1) |
		     (coded_flags & NEW_SLICE ? tile_block : tile_boundary),
	     HEVC_FP_NONE);

	return last_tile_block;
}

static void compute_tiles_uniform(struct avd_hevc_run *run,
				  u16 log2_min_cb_size, u16 width, u16 height,
				  s32 pic_in_cts_width, s32 pic_in_cts_height,
				  u16 *column_width, u16 *row_height)
{
	const struct v4l2_ctrl_hevc_pps *pps = run->pps;
	int i;

	for (i = 0; i < pps->num_tile_columns_minus1 + 1; i++)
		column_width[i] = ((i + 1) * pic_in_cts_width) /
					  (pps->num_tile_columns_minus1 + 1) -
				  (i * pic_in_cts_width) /
					  (pps->num_tile_columns_minus1 + 1);

	for (i = 0; i < pps->num_tile_rows_minus1 + 1; i++)
		row_height[i] = ((i + 1) * pic_in_cts_height) /
					(pps->num_tile_rows_minus1 + 1) -
				(i * pic_in_cts_height) /
					(pps->num_tile_rows_minus1 + 1);
}

static void compute_tiles_non_uniform(struct avd_hevc_run *run,
				      u16 log2_min_cb_size, u16 width,
				      u16 height, s32 pic_in_cts_width,
				      s32 pic_in_cts_height, u16 *column_width,
				      u16 *row_height)
{
	const struct v4l2_ctrl_hevc_pps *pps = run->pps;
	s32 sum = 0;
	int i;

	for (i = 0; i < pps->num_tile_columns_minus1; i++) {
		column_width[i] = pps->column_width_minus1[i] + 1;
		sum += column_width[i];
	}
	column_width[i] = pic_in_cts_width - sum;

	sum = 0;
	for (i = 0; i < pps->num_tile_rows_minus1; i++) {
		row_height[i] = pps->row_height_minus1[i] + 1;
		sum += row_height[i];
	}
	row_height[i] = pic_in_cts_height - sum;
}

static void compute_bd(struct avd_hevc_run *run, u32 *col_bd, u32 *row_bd,
		       u16 *column_width, u16 *row_height)
{
	const struct v4l2_ctrl_hevc_pps *pps = run->pps;
	int i;

	for (col_bd[0] = 0, i = 0; i <= pps->num_tile_columns_minus1; i++)
		col_bd[i + 1] = col_bd[i] + column_width[i];

	for (row_bd[0] = 0, i = 0; i <= pps->num_tile_rows_minus1; i++)
		row_bd[i + 1] = row_bd[i] + row_height[i];
}

static void compute_rs_to_ts(struct avd_hevc_run *run, u32 pic_in_ctbs_size,
			     u32 pic_in_ctbs_width, u32 *col_bd, u32 *row_bd,
			     u16 *col_width, u16 *row_height,
			     u32 *ctb_addr_rs_to_ts)
{
	const struct v4l2_ctrl_hevc_pps *pps = run->pps;
	int i, j, tb_x, tb_y, tile_x = 0, tile_y = 0;
	u32 ctb_addr_rs;

	for (ctb_addr_rs = 0; ctb_addr_rs < pic_in_ctbs_size; ctb_addr_rs++) {
		tb_x = ctb_addr_rs % pic_in_ctbs_width;
		tb_y = ctb_addr_rs / pic_in_ctbs_width;
		for (i = 0; i <= pps->num_tile_columns_minus1; i++)
			if (tb_x >= col_bd[i])
				tile_x = i;
		for (j = 0; j <= pps->num_tile_rows_minus1; j++)
			if (tb_y >= row_bd[j])
				tile_y = j;
		ctb_addr_rs_to_ts[ctb_addr_rs] = 0;
		for (i = 0; i < tile_x; i++)
			ctb_addr_rs_to_ts[ctb_addr_rs] +=
				row_height[tile_y] * col_width[i];
		for (j = 0; j < tile_y; j++)
			ctb_addr_rs_to_ts[ctb_addr_rs] +=
				pic_in_ctbs_width * row_height[j];
		ctb_addr_rs_to_ts[ctb_addr_rs] +=
			(tb_y - row_bd[tile_y]) * col_width[tile_x] + tb_x -
			col_bd[tile_x];
	}
}

static void compute_tile_ids(struct avd_hevc_run *run, u32 pic_in_ctbs_width,
			     u32 *col_bd, u32 *row_bd, u32 *ctb_addr_rs_to_ts,
			     u32 *tile_ids)
{
	const struct v4l2_ctrl_hevc_pps *pps = run->pps;
	int j, i, y, x;
	u32 tile_idx;
	for (j = 0, tile_idx = 0; j <= pps->num_tile_rows_minus1; j++)
		for (i = 0; i <= pps->num_tile_columns_minus1; i++, tile_idx++)
			for (y = row_bd[j]; y < row_bd[j + 1]; y++)
				for (x = col_bd[i]; x < col_bd[i + 1]; x++)
					tile_ids[ctb_addr_rs_to_ts
							 [y * pic_in_ctbs_width +
							  x]] = tile_idx;
}

struct sl_ctx {
	u32 ctx_col;
	u32 ctx_row;
	s32 q1_col;
	s32 q1_row;
};

/* TODO */
static int stream_slices(struct avd_ctx *ctx, struct avd_hevc_run *run)
{
	const struct v4l2_ctrl_hevc_sps *sps = run->sps;
	const struct v4l2_ctrl_hevc_pps *pps = run->pps;
	const struct v4l2_ctrl_hevc_slice_params *sl;
	struct avd_hevc_tile_info *tile_info = &run->tile_info;
	bool tiles_enabled, first_slice, first_segment;
	bool hflip, vflip;
	int slice_segment_offset, entry_point_idx = 0, pos = 0, offset = 0;
	int row, col, i, s, to;
	int slice_flag, size, new_offset;
	int tile_id, last_tile_id;
	u16 log2_min_cb_size, width, height;
	s32 max_cu_width, pic_in_ctbs_width, pic_in_ctbs_height;
	u32 num_cols, last_tile_block = 0;
	struct sl_ctx last = {
		.q1_col = -1,
		.q1_row = -1,
	};

	width = sps->pic_width_in_luma_samples;
	height = sps->pic_height_in_luma_samples;

	tiles_enabled = !!(pps->flags & V4L2_HEVC_PPS_FLAG_TILES_ENABLED);

	log2_min_cb_size = sps->log2_min_luma_coding_block_size_minus3 + 3;

	num_cols = pps->num_tile_columns_minus1 + 1;

	max_cu_width = 1 << (sps->log2_diff_max_min_luma_coding_block_size +
			     log2_min_cb_size);
	pic_in_ctbs_width = (width + max_cu_width - 1) / max_cu_width;
	pic_in_ctbs_height = (height + max_cu_width - 1) / max_cu_width;

	for (s = 0; s < run->num_slices; s++) {
		sl = &run->sl[s];
		slice_segment_offset = 0;
		offset = run->ranges[s].offset;
		to = tiles_enabled ? sl->num_entry_point_offsets + 1 : 1;
		if (tiles_enabled &&
		    sl->num_entry_point_offsets + entry_point_idx >
			    run->num_entry_point_offsets) {
			dev_err(ctx->dev->dev,
				"too few entry points: has %d, needs %d",
				run->num_entry_point_offsets,
				sl->num_entry_point_offsets + entry_point_idx);
			return -EINVAL;
		}
		for (i = 0; i < to; i++) {
			first_segment = i == 0;
			first_slice = s == 0;

			run->fp.segment++;

			if (tiles_enabled && to > 1) {
				if (i < sl->num_entry_point_offsets) {
					size = run->entry_point_offsets
						       [entry_point_idx++];
					new_offset = size;
				} else {
					size = run->ranges[s].size - slice_segment_offset;
					new_offset = size;
				}
			} else {
				size = run->ranges[s].size;
				new_offset = size;
			}

			tile_id = tile_info->tile_ids
					  [tile_info->ctb_addr_rs_to_ts
						   [sl->slice_segment_addr]];
			last_tile_id =
				first_slice ?
					-1 :
					tile_info->tile_ids
						[tile_info->ctb_addr_rs_to_ts
							 [run->sl[s - 1]
								  .slice_segment_addr]];

			slice_flag = 0;
			if (first_slice || tile_id != last_tile_id ||
			    !first_segment)
				slice_flag |= NEW_TILE_ID;

			if (first_segment)
				slice_flag |= NEW_SLICE;

			vflip = false;
			hflip = false;

			row = pos / num_cols;
			col = pos % num_cols;
			if (tiles_enabled && row > pps->num_tile_rows_minus1)
				return -EINVAL;

			/*
			 * not a fan of this, in JCT-VC-HEVC_V1 only TILES_B_Cisco_1
			 * seems affected and it only seems to need hflip.
			 */
			if (slice_flag & NEW_TILE_ID) {
				if ((col >= last.ctx_col &&
				     row > last.ctx_row) ||
				    (col <= last.q1_col && row > last.q1_row))
					vflip = true;

				if (!(slice_flag & NEW_SLICE)) {
					if (row && row == last.ctx_row + 1) {
						hflip = true;
						if (!vflip) {
							last.q1_row = row;
							last.q1_col = col;
						}
					}
				} else {
					last.ctx_row = row;
					last.ctx_col = col;
					last.q1_row = -1;
					last.q1_col = -1;
				}
			}

			set_slice(
				ctx, run, sl, size,
				offset + slice_segment_offset,
				/* makes WPP more reliable? But not really ?? */
				(sl->flags & V4L2_HEVC_SLICE_PARAMS_FLAG_DEPENDENT_SLICE_SEGMENT ?
					 slice_flag & ~NEW_SLICE :
					 slice_flag)
					<< 13);

			last_tile_block = submit_slice_segment(
				ctx, run, sl, row, col, tile_info->col_bd,
				tile_info->row_bd, pic_in_ctbs_width,
				pic_in_ctbs_height, first_slice, hflip, vflip,
				slice_flag, last_tile_block);

			run->segment_ends[++run->segments] = run->frame->prog_words;

			if (slice_flag & NEW_TILE_ID)
				pos++;

			slice_segment_offset += new_offset;
		}
	}

	run->submit_num = pos;
	return 0;
}

static void update_dec_buf_info(struct avd_decoded_buffer *buf,
				const struct v4l2_ctrl_hevc_slice_params *sl)
{
	buf->hevc.is_intra = sl->slice_type == V4L2_HEVC_SLICE_TYPE_I;
}

static void avd_hevc_adjust_decoded_fmt(struct avd_ctx *ctx,
					struct v4l2_pix_format_mplane *pix_mp)
{
	/* Keep the MV table outside 128-byte-aligned compressed storage. */
	pix_mp->plane_fmt[0].sizeimage =
		ALIGN(pix_mp->plane_fmt[0].sizeimage, AVD_ALIGN) +
		mv_color_size(pix_mp->width, pix_mp->height);
}

static enum avd_image_fmt avd_hevc_get_image_fmt(struct avd_ctx *ctx,
						 struct v4l2_ctrl *ctrl)
{
	const struct v4l2_ctrl_hevc_sps *sps = ctrl->p_new.p_hevc_sps;

	if (ctrl->id != V4L2_CID_STATELESS_HEVC_SPS)
		return AVD_IMG_FMT_ANY;

	/*
	 * TODO: we can do up to 4:4:4 12 bit
	 * not sure if v4l2 supports RExt
	 */

	if (sps->bit_depth_luma_minus8 == 0) {
		if (sps->chroma_format_idc == 2)
			return AVD_IMG_FMT_422_8BIT;
		else
			return AVD_IMG_FMT_420_8BIT;
	} else if (sps->bit_depth_luma_minus8 == 2) {
		if (sps->chroma_format_idc == 2)
			return AVD_IMG_FMT_422_10BIT;
		else
			return AVD_IMG_FMT_420_10BIT;
	}

	return AVD_IMG_FMT_ANY;
}

static int avd_hevc_validate_sps(struct avd_ctx *ctx,
				 const struct v4l2_ctrl_hevc_sps *sps)
{
	if (sps->pic_width_in_luma_samples > ctx->coded_fmt.fmt.pix_mp.width ||
	    sps->pic_height_in_luma_samples > ctx->coded_fmt.fmt.pix_mp.height)
		return -EINVAL;
	if (sps->bit_depth_luma_minus8 != sps->bit_depth_chroma_minus8)
		/* Luma and chroma bit depth mismatch: the firmware faults (H0 error) */
		return -EINVAL;
	if (sps->bit_depth_luma_minus8 != 0 && sps->bit_depth_luma_minus8 != 2)
		/* Only 8 and 10 bit have capture formats */
		return -EINVAL;
	if (sps->chroma_format_idc == 2 && sps->bit_depth_luma_minus8 == 2)
		/* 4:2:2 10 bit would need P210, which is not offered */
		return -EINVAL;

	return 0;
}

static void avd_hevc_stop(struct avd_ctx *ctx)
{
	struct avd_hevc_ctx *hevc_ctx = ctx->priv;
	unsigned int i;

	if (!hevc_ctx)
		return;
	for (i = 0; i < AVD_MAX_FRAMES; i++) {
		struct avd_hevc_bufs *bufs = &hevc_ctx->slots[i];

		avd_buf_free(ctx->dev, &bufs->pipe_state);
		avd_buf_free(ctx->dev, &bufs->mv_above_info);
		avd_buf_free(ctx->dev, &bufs->az_above);
		avd_buf_free(ctx->dev, &bufs->ip_above);
		avd_buf_free(ctx->dev, &bufs->lf_above);
		avd_buf_free(ctx->dev, &bufs->lf_above_info);
		avd_buf_free(ctx->dev, &bufs->lf_left);
		avd_buf_free(ctx->dev, &bufs->lf_left_info);
		avd_buf_free(ctx->dev, &bufs->sw_left);
	}
	kfree(hevc_ctx->slots);
	kfree(hevc_ctx);
	ctx->priv = NULL;
}

static int avd_hevc_start(struct avd_ctx *ctx)
{
	struct avd_hevc_ctx *hevc_ctx;

	hevc_ctx = kzalloc(sizeof(*hevc_ctx), GFP_KERNEL);
	if (!hevc_ctx)
		return -ENOMEM;
	hevc_ctx->slots = kcalloc(AVD_MAX_FRAMES, sizeof(*hevc_ctx->slots), GFP_KERNEL);
	if (!hevc_ctx->slots) {
		kfree(hevc_ctx);
		return -ENOMEM;
	}
	ctx->priv = hevc_ctx;
	return 0;
}

static int avd_hevc_alloc_scratch(struct avd_ctx *ctx, struct avd_hevc_run *run)
{
	struct avd_dev *avd = ctx->dev;
	const struct v4l2_ctrl_hevc_sps *sps = run->sps;
	const struct v4l2_ctrl_hevc_pps *pps = run->pps;
	struct avd_hevc_tile_info *tile_info = &run->tile_info;
	int i, ret, w, bit_depth, max_col = 0, max_row = 0, cols, rows;
	int log2_min_cb_size, max_cu_width;
	cols = pps->num_tile_columns_minus1 + 1;
	rows = pps->num_tile_rows_minus1 + 1;
	ret = avd_buf_alloc(avd, &run->bufs->pipe_state, 0x200);
	if (ret)
		return ret;
	memset(run->bufs->pipe_state.cpu, 0, run->bufs->pipe_state.size);

	if (sps->bit_depth_chroma_minus8 > sps->bit_depth_luma_minus8)
		bit_depth = sps->bit_depth_chroma_minus8;
	else
		bit_depth = sps->bit_depth_luma_minus8;

	bit_depth += 8;

	w = sps->pic_width_in_luma_samples;

	log2_min_cb_size = sps->log2_min_luma_coding_block_size_minus3 + 3;
	max_cu_width = 1 << (sps->log2_diff_max_min_luma_coding_block_size +
			     log2_min_cb_size);

	for (i = 0; i < cols; i++)
		max_col = tile_info->col_width[i] > max_col ?
				  tile_info->col_width[i] :
				  max_col;

	for (i = 0; i < rows; i++)
		max_row = tile_info->row_height[i] > max_row ?
				  tile_info->row_height[i] :
				  max_row;

	ret = avd_buf_alloc(avd, &run->bufs->ip_above,
			    ((max_col * max_cu_width) / 4) * bit_depth);
	if (ret)
		return ret;

	ret = avd_buf_alloc(avd, &run->bufs->mv_above_info,
			    ((max_col * max_cu_width) / 16) * 20);
	if (ret)
		return ret;

	ret = avd_buf_alloc(avd, &run->bufs->lf_above,
			    DIV_ROUND_UP(w, 16) * bit_depth * (10 + 6) +
				    (cols - 1) * 256);
	if (ret)
		return ret;

	ret = avd_buf_alloc(avd, &run->bufs->lf_above_info,
			    DIV_ROUND_UP(w + 7, 16) * 36 + cols * 128);
	if (ret)
		return ret;

	if (pps->flags & V4L2_HEVC_PPS_FLAG_TILES_ENABLED) {
		ret = avd_buf_alloc(avd, &run->bufs->lf_left,
				    ((max_row * max_cu_width) / 4) * 36 +
					    144 /* why? */);
		if (ret)
			return ret;
		ret = avd_buf_alloc(avd, &run->bufs->lf_left_info,
				    ((max_row * max_cu_width) / 4) * 9);
		if (ret)
			return ret;

		ret = avd_buf_alloc(avd, &run->bufs->az_above,
				    /* not sure if its cols or rows */
				    DIV_ROUND_UP(w, 64) * 144 +
					    (cols - 1) * 128);
		if (ret)
			return ret;

		ret = avd_buf_alloc(avd, &run->bufs->sw_left,
				    ((max_row * max_cu_width) / 4) * 216);
		if (ret)
			return ret;
	} else {
		ret = avd_buf_alloc(avd, &run->bufs->az_above,
				    DIV_ROUND_UP(w + 7, 16) * 4 * bit_depth);
		if (ret)
			return ret;
	}

	return 0;
}

static int avd_hevc_compute_tiles(struct avd_ctx *ctx, struct avd_hevc_run *run)
{
	const struct v4l2_ctrl_hevc_sps *sps = run->sps;
	const struct v4l2_ctrl_hevc_pps *pps = run->pps;
	bool tiles_enabled;
	struct avd_hevc_tile_info *tile_info = &run->tile_info;
	u16 log2_min_cb_size, width, height;
	s32 max_cu_width, pic_in_ctbs_width, pic_in_ctbs_height,
		pic_in_ctbs_size;

	width = sps->pic_width_in_luma_samples;
	height = sps->pic_height_in_luma_samples;

	log2_min_cb_size = sps->log2_min_luma_coding_block_size_minus3 + 3;

	max_cu_width = 1 << (sps->log2_diff_max_min_luma_coding_block_size +
			     log2_min_cb_size);
	pic_in_ctbs_width = (width + max_cu_width - 1) / max_cu_width;
	pic_in_ctbs_height = (height + max_cu_width - 1) / max_cu_width;
	pic_in_ctbs_size = pic_in_ctbs_height * pic_in_ctbs_width;

	tiles_enabled = !!(pps->flags & V4L2_HEVC_PPS_FLAG_TILES_ENABLED);

	tile_info->ctb_addr_rs_to_ts = kzalloc(
		sizeof(*tile_info->ctb_addr_rs_to_ts) * pic_in_ctbs_size,
		GFP_KERNEL);
	if (!tile_info->ctb_addr_rs_to_ts)
		return -ENOMEM;

	tile_info->tile_ids = kzalloc(
		sizeof(*tile_info->tile_ids) * pic_in_ctbs_size, GFP_KERNEL);
	if (!tile_info->tile_ids)
		return -ENOMEM;

	if (tiles_enabled) {
		if (pps->flags & V4L2_HEVC_PPS_FLAG_UNIFORM_SPACING) {
			compute_tiles_uniform(run, log2_min_cb_size, width,
					      height, pic_in_ctbs_width,
					      pic_in_ctbs_height,
					      tile_info->col_width,
					      tile_info->row_height);
		} else {
			compute_tiles_non_uniform(run, log2_min_cb_size, width,
						  height, pic_in_ctbs_width,
						  pic_in_ctbs_height,
						  tile_info->col_width,
						  tile_info->row_height);
		}

		compute_bd(run, tile_info->col_bd, tile_info->row_bd,
			   tile_info->col_width, tile_info->row_height);

		/*
		 * 6.5.1 CTB raster and tile scanning conversion process
		 * (6-7) and (6-9)
		 *
		 * we really only need to know if tileidx != last tileidx
		 * so doing all this is stupid
		 */
		compute_rs_to_ts(run, pic_in_ctbs_size, pic_in_ctbs_width,
				 tile_info->col_bd, tile_info->row_bd,
				 tile_info->col_width, tile_info->row_height,
				 tile_info->ctb_addr_rs_to_ts);
		compute_tile_ids(run, pic_in_ctbs_width, tile_info->col_bd,
				 tile_info->row_bd,
				 tile_info->ctb_addr_rs_to_ts,
				 tile_info->tile_ids);
	} else {
		tile_info->col_width[0] =
			(width + max_cu_width - 1) / max_cu_width;
		tile_info->row_height[0] =
			(height + max_cu_width - 1) / max_cu_width;
	}

	return 0;
}

static void avd_hevc_retire(struct avd_ctx *ctx, struct avd_frame *f,
			    enum vb2_buffer_state state)
{
	struct avd_hevc_run *run = f->codec_priv;

	if (!run)
		return;
	kvfree(run->tile_info.ctb_addr_rs_to_ts);
	kvfree(run->tile_info.tile_ids);
	kvfree(run->ranges);
	kfree(run->segment_ends);
	kvfree(run->sl);
	kvfree(run->entry_point_offsets);
	kfree(run->sps);
	kfree(run->pps);
	kfree(run->decode);
	kfree(run->scaling_matrix);
	kfree(run);
	f->codec_priv = NULL;
}

/* Core applies the request before prepare and completes it after prepare. */
static int avd_hevc_copy_controls(struct avd_ctx *ctx, struct avd_hevc_run *run)
{
	struct v4l2_ctrl *sl, *ep, *sps, *pps, *decode, *scaling;

	sl = v4l2_ctrl_find(&ctx->ctrl_hdl, V4L2_CID_STATELESS_HEVC_SLICE_PARAMS);
	ep = v4l2_ctrl_find(&ctx->ctrl_hdl, V4L2_CID_STATELESS_HEVC_ENTRY_POINT_OFFSETS);
	sps = v4l2_ctrl_find(&ctx->ctrl_hdl, V4L2_CID_STATELESS_HEVC_SPS);
	pps = v4l2_ctrl_find(&ctx->ctrl_hdl, V4L2_CID_STATELESS_HEVC_PPS);
	decode = v4l2_ctrl_find(&ctx->ctrl_hdl, V4L2_CID_STATELESS_HEVC_DECODE_PARAMS);
	scaling = v4l2_ctrl_find(&ctx->ctrl_hdl, V4L2_CID_STATELESS_HEVC_SCALING_MATRIX);
	if (!sl || !ep || !sps || !pps || !decode || !scaling)
		return -EINVAL;

	/* All six controls share this handler lock, including dynamic storage. */
	v4l2_ctrl_lock(sl);
	if (!sl->elems || sl->elems >= 0x258 || ep->elems > 0x4000) {
		v4l2_ctrl_unlock(sl);
		return -EINVAL;
	}
	run->num_slices = sl->elems;
	run->num_entry_point_offsets = ep->elems;
	run->sl = kvmalloc_array(sl->elems, sl->elem_size, GFP_KERNEL);
	run->entry_point_offsets = kvmalloc_array(ep->elems, ep->elem_size, GFP_KERNEL);
	if (run->sl)
		memcpy((void *)run->sl, sl->p_cur.p, sl->elems * sl->elem_size);
	if (run->entry_point_offsets)
		memcpy((void *)run->entry_point_offsets, ep->p_cur.p, ep->elems * ep->elem_size);
	run->sps = kmemdup(sps->p_cur.p, sizeof(*run->sps), GFP_KERNEL);
	run->pps = kmemdup(pps->p_cur.p, sizeof(*run->pps), GFP_KERNEL);
	run->decode = kmemdup(decode->p_cur.p, sizeof(*run->decode), GFP_KERNEL);
	run->scaling_matrix = kmemdup(scaling->p_cur.p, sizeof(*run->scaling_matrix), GFP_KERNEL);
	v4l2_ctrl_unlock(sl);
	if (!run->sl || !run->entry_point_offsets || !run->sps || !run->pps ||
	    !run->decode || !run->scaling_matrix)
		return -ENOMEM;
	return 0;
}

static int avd_hevc_validate_run(struct avd_ctx *ctx, struct avd_hevc_run *run)
{
	const struct v4l2_ctrl_hevc_sps *sps = run->sps;
	const struct v4l2_ctrl_hevc_pps *pps = run->pps;
	u32 log2_ctb, width, height, cols, rows, sum, i, j, ep = 0;
	int ret;

	ret = avd_hevc_validate_sps(ctx, sps);
	if (ret)
		return ret;
	if (!sps->pic_width_in_luma_samples || !sps->pic_height_in_luma_samples ||
	    sps->log2_min_luma_coding_block_size_minus3 > 3 ||
	    sps->log2_diff_max_min_luma_coding_block_size > 3)
		return -EINVAL;
	log2_ctb = sps->log2_min_luma_coding_block_size_minus3 + 3 +
		   sps->log2_diff_max_min_luma_coding_block_size;
	if (log2_ctb > 6 || (sps->chroma_format_idc != 1 && sps->chroma_format_idc != 2))
		return -EINVAL;
	width = DIV_ROUND_UP(sps->pic_width_in_luma_samples, 1U << log2_ctb);
	height = DIV_ROUND_UP(sps->pic_height_in_luma_samples, 1U << log2_ctb);
	cols = pps->num_tile_columns_minus1 + 1;
	rows = pps->num_tile_rows_minus1 + 1;
	if (cols > 16 || rows > 22 || cols > width || rows > height ||
	    (!(pps->flags & V4L2_HEVC_PPS_FLAG_TILES_ENABLED) && (cols != 1 || rows != 1)))
		return -EINVAL;
	if (!(pps->flags & V4L2_HEVC_PPS_FLAG_UNIFORM_SPACING)) {
		for (i = 0, sum = 0; i + 1 < cols; i++)
			sum += pps->column_width_minus1[i] + 1;
		if (sum >= width)
			return -EINVAL;
		for (i = 0, sum = 0; i + 1 < rows; i++)
			sum += pps->row_height_minus1[i] + 1;
		if (sum >= height)
			return -EINVAL;
	}
	if (run->decode->num_active_dpb_entries > ARRAY_SIZE(run->decode->dpb))
		return -EINVAL;
	for (i = 0; i < run->decode->num_active_dpb_entries; i++) {
		struct avd_decoded_buffer *ref = avd_get_ref_buf(ctx, run->frame->dst,
							     run->decode->dpb[i].timestamp);

		if (!ref || &ref->base.vb == run->frame->dst)
			return -EINVAL;
	}
	for (i = 0; i < run->num_slices; i++) {
		const struct v4l2_ctrl_hevc_slice_params *sl = &run->sl[i];
		u64 bytes = 0;
		unsigned int l0 = sl->num_ref_idx_l0_active_minus1 + 1;
		unsigned int l1 = sl->num_ref_idx_l1_active_minus1 + 1;
		int chroma_denom = sl->pred_weight_table.luma_log2_weight_denom +
				   sl->pred_weight_table.delta_chroma_log2_weight_denom;

		if (sl->slice_type > V4L2_HEVC_SLICE_TYPE_I || sl->slice_segment_addr >= width * height ||
		    (!i && sl->flags & V4L2_HEVC_SLICE_PARAMS_FLAG_DEPENDENT_SLICE_SEGMENT) ||
		    sl->five_minus_max_num_merge_cand > 4 ||
		    sl->num_entry_point_offsets > run->num_entry_point_offsets - ep ||
		    sl->pred_weight_table.luma_log2_weight_denom > 7 || chroma_denom < 0 || chroma_denom > 7)
			return -EINVAL;
		if (sl->slice_type != V4L2_HEVC_SLICE_TYPE_I) {
			if (l0 > ARRAY_SIZE(sl->ref_idx_l0) || l1 > ARRAY_SIZE(sl->ref_idx_l1) ||
			    sl->collocated_ref_idx >= ((sl->slice_type == V4L2_HEVC_SLICE_TYPE_P ||
			    sl->flags & V4L2_HEVC_SLICE_PARAMS_FLAG_COLLOCATED_FROM_L0) ? l0 : l1))
				return -EINVAL;
			for (j = 0; j < l0; j++)
				if (sl->ref_idx_l0[j] >= run->decode->num_active_dpb_entries)
					return -EINVAL;
			if (sl->slice_type == V4L2_HEVC_SLICE_TYPE_B)
				for (j = 0; j < l1; j++)
					if (sl->ref_idx_l1[j] >= run->decode->num_active_dpb_entries)
						return -EINVAL;
		}
		for (j = 0; j < sl->num_entry_point_offsets; j++) {
			u32 step = run->entry_point_offsets[ep++];

			if (!step)
				return -EINVAL;
			bytes += step;
		}
		if (bytes >= run->ranges[i].size)
			return -EINVAL;
	}
	return 0;
}

static int avd_hevc_prepare(struct avd_ctx *ctx, struct avd_frame *f)
{
	struct avd_hevc_ctx *hevc_ctx = ctx->priv;
	struct avd_decoded_buffer *dst;
	struct avd_hevc_run *run;
	u32 payload, data_offset, dst_len, mv_len;
	int ret;

	if (!hevc_ctx || f->idx >= AVD_MAX_FRAMES || !f->src || !f->dst ||
	    !f->prog || !f->prog->cpu || f->codec_priv)
		return -EINVAL;
	f->dep = AVD_DEP_NONE;
	f->prog_words = 0;
	run = kzalloc(sizeof(*run), GFP_KERNEL);
	if (!run)
		return -ENOMEM;
	f->codec_priv = run;
	run->frame = f;
	run->bufs = &hevc_ctx->slots[f->idx];
	run->base.bufs.src = f->src;
	run->base.bufs.dst = f->dst;
	data_offset = f->src->vb2_buf.planes[0].data_offset;
	payload = vb2_get_plane_payload(&f->src->vb2_buf, 0);
	if (payload <= data_offset) {
		ret = -EINVAL;
		goto fail;
	}
	payload -= data_offset;
	run->base.coded_in = vb2_dma_contig_plane_dma_addr(&f->src->vb2_buf, 0) + data_offset;
	run->base.y_out = vb2_dma_contig_plane_dma_addr(&f->dst->vb2_buf, 0);
	run->base.uv_out = run->base.y_out +
		ctx->decoded_fmt.fmt.pix_mp.plane_fmt[0].bytesperline *
		ctx->decoded_fmt.fmt.pix_mp.height;
	run->base.comp_out = run->base.y_out + ctx->comp.start_offset;
	dst_len = vb2_plane_size(&f->dst->vb2_buf, 0);
	mv_len = mv_color_size(fmt_width(ctx), fmt_height(ctx));
	if (dst_len < mv_len || round_down(dst_len - mv_len, AVD_ALIGN) <
	    ctx->comp.start_offset + ctx->comp.size) {
		ret = -EINVAL;
		goto fail;
	}
	run->addresses.mv_color = run->base.y_out + round_down(dst_len - mv_len, AVD_ALIGN);
	ret = avd_hevc_copy_controls(ctx, run);
	if (ret)
		goto fail;
	run->ranges = kvmalloc_array(run->num_slices, sizeof(*run->ranges), GFP_KERNEL);
	run->segment_ends = kcalloc(run->num_slices + run->num_entry_point_offsets + 1,
				    sizeof(*run->segment_ends), GFP_KERNEL);
	if (!run->ranges || !run->segment_ends) {
		ret = -ENOMEM;
		goto fail;
	}
	ret = avd_hevc_slice_ranges(run->sl, run->num_slices, payload, run->ranges);
	if (ret)
		goto fail;
	ret = avd_hevc_validate_run(ctx, run);
	if (ret)
		goto fail;
	ret = avd_hevc_compute_tiles(ctx, run);
	if (ret)
		goto fail;
	ret = avd_hevc_alloc_scratch(ctx, run);
	if (ret)
		goto fail;
	if (f->fp) {
		ret = avd_hevc_fp_begin(&run->fp, ctx->dev->variant->hevc_fp_layout,
				f->fp->cpu, f->fp->size, f->frame_num, run->num_slices);
		if (ret)
			goto fail;
	}
	dst = vb2_to_avd_decoded_buf(&f->dst->vb2_buf);
	dst->comp = ctx->comp;
	ctx->decomp = true;
	update_dec_buf_info(dst, &run->sl[0]);
	set_header(ctx, run);
	run->segment_ends[0] = f->prog_words;
	ret = stream_slices(ctx, run);
	if (!ret)
		ret = run->error;
	if (!ret)
		return 0;
fail:
	f->prog_words = 0;
	avd_hevc_retire(ctx, f, VB2_BUF_STATE_ERROR);
	return ret;
}

static int avd_hevc_pack_cmd(struct avd_ctx *ctx, struct avd_frame *f)
{
	struct avd_hevc_run *run = f->codec_priv;

	if (!run || !f->fp)
		return -EINVAL;
	return avd_hevc_fp_pack_cmd(&run->fp, f->cmd, (f->cmd[1] >> 16) & 3,
				    ctx->dev->variant->vp_slots[AVD_CODEC_HEVC]);
}

static int avd_hevc_try_ctrl(struct avd_ctx *ctx, struct v4l2_ctrl *ctrl)
{
	if (ctrl->id == V4L2_CID_STATELESS_HEVC_SPS)
		return avd_hevc_validate_sps(ctx, ctrl->p_new.p_hevc_sps);

	return 0;
}

const struct avd_codec_frame_ops avd_hevc_frame_ops = {
	.prepare = avd_hevc_prepare,
	.pack_cmd = avd_hevc_pack_cmd,
	.retire = avd_hevc_retire,
};

/* CPU-only conversion for the old core's temporary dispatch bridge. */
int avd_hevc_legacy_job(struct avd_ctx *ctx, struct avd_frame *f)
{
	struct avd_hevc_run *run = f->codec_priv;
	size_t start = 0;
	unsigned int i;
	int ret;

	if (!run)
		return -EINVAL;
	ret = avd_init_job(ctx, AVD_CODEC_HEVC, run->segments + 1);
	if (ret)
		return ret;
	for (i = 0; i <= run->segments; i++) {
		struct avd_segment *seg = &ctx->job.segments[i];
		size_t end = run->segment_ends[i], j;

		if (end - start > AVD_MAX_INST)
			return -E2BIG;
		for (j = start; j < end; j++)
			seg->instructions[seg->num++] = le32_to_cpu(((__le32 *)f->prog->cpu)[j]);
		start = end;
	}
	ctx->job.num = run->segments;
	ctx->hevc_legacy_submit_num = run->submit_num;
	return 0;
}

const struct avd_coded_fmt_ops avd_hevc_fmt_ops = {
	.adjust_decoded_fmt = avd_hevc_adjust_decoded_fmt,
	.start = avd_hevc_start,
	.stop = avd_hevc_stop,
	.run = avd_hevc_legacy_run,
	.submit = avd_hevc_legacy_submit,
	.try_ctrl = avd_hevc_try_ctrl,
	.get_image_fmt = avd_hevc_get_image_fmt,
};
