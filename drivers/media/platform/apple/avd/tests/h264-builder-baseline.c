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

/* Frozen Linux builder, copyright Asahi Linux Contributors and Eileen Yoon.
 * Baseline aurora-wip fbc4e79, before frame_ops; synthetic regression only. */
#define H264_SCL_DIMS	(0x1000000 | ((64 / 4) << 5) | (((16 / 4) << 5) - 1))

#define h264_FLAG_CI_PRED(v)			FIELD_PREP(BIT(19), !!(v))
#define H264_FLAG_ENTROPY_CODING_MODE(v)	FIELD_PREP(BIT(20), !!(v))
#define H264_FLAG_NOT_IDR(v)			FIELD_PREP(BIT(21), !!(v))

#define H264_TRANSFORM_8X8_MODE(v)		FIELD_PREP(BIT(7), !!(v))

/*
 * Most slices held for one picture. This bounds the slice table, the job
 * table and the memory that a client can pin.
 */
#define MAX_SLICES	4096

struct avd_h264_run {
	struct avd_run base;

	const struct v4l2_ctrl_h264_decode_params *decode_params;
	const struct v4l2_ctrl_h264_sps *sps;
	const struct v4l2_ctrl_h264_pps *pps;
	const struct v4l2_ctrl_h264_slice_params *slice_params;

	const struct v4l2_ctrl_h264_scaling_matrix *scaling_matrix;
	const struct v4l2_ctrl_h264_pred_weights *pred_weights;

	struct run_addr {
		dma_addr_t mv_color;
	} addresses;

	s32 cur_poc;
	u8 num_valid;
	u32 slice_data_off;
};

/* state */
struct avd_h264_ctx {
	bool monochrome;
	struct avd_h264_reflists {
		struct v4l2_h264_reference p[V4L2_H264_REF_LIST_LEN];
		struct v4l2_h264_reference b0[V4L2_H264_REF_LIST_LEN];
		struct v4l2_h264_reference b1[V4L2_H264_REF_LIST_LEN];
	} reflists;

	struct avd_buf *slices;
	struct avd_buf *active_slice;
	size_t slice_num;
	size_t alloc_slice_num;
	/* allocated size of the held slices */
	size_t slice_bytes;

	struct avd_h264_bufs {
		struct avd_buf inst;
		struct avd_buf pipe_state;
		struct avd_buf above_info;
		struct avd_buf lf_above_info;
		struct avd_buf lf_above;
		struct avd_buf ip_above;
		struct avd_buf mv_above_info;
	} bufs;
};

/* ffmpeg submits wrong timestamp, use first_mb_in_slice as a workaround */
#define is_new_frame(sl) \
	(sl->first_mb_in_slice == 0) /* (ctx->fh.m2m_ctx->new_frame) */

/* scaling matrix */
static const u32 default_8x8_intra[] = {
	0x060a0d10, 0x0a0b1012, 0x0d101217, 0x10121719, 0x1217191b, 0x17191b1d,
	0x191b1d1f, 0x1b1d1f21, 0x1217191b, 0x17191b1d, 0x191b1d1f, 0x1b1d1f21,
	0x1d1f2124, 0x1f212426, 0x21242628, 0x2426282a,
};
static const u32 default_8x8_inter[] = {
	0x090d0f11, 0x0d0d1113, 0x0f111315, 0x11131516, 0x13151618, 0x15161819,
	0x1618191b, 0x18191b1c, 0x13151618, 0x15161819, 0x1618191b, 0x18191b1c,
	0x191b1c1e, 0x1b1c1e20, 0x1c1e2021, 0x1e202123,
};

static inline u32 mv_color_size(u32 w, u32 h)
{
	return (DIV_ROUND_UP(w, 16) + 1) * (DIV_ROUND_UP(h, 16) + 1) * 64;
}

/* sorry for the formatting */

static void stream_refs(struct avd_ctx *ctx, struct avd_h264_run *run)
{
	const struct v4l2_ctrl_h264_decode_params *decode = run->decode_params;
	const struct v4l2_h264_dpb_entry *dpb = decode->dpb;
	struct avd_h264_ctx *h264_ctx = ctx->priv;
	struct avd_decoded_buffer *dst, *ref;
	dma_addr_t addr;

	dst = vb2_to_avd_decoded_buf(&run->base.bufs.dst->vb2_buf);

	push(0, "");
	pusha(h264_ctx->bufs.mv_above_info.addr, "mv_above_info", 7);
	pusha(run->addresses.mv_color, "mv_color", 0);

	push(0, "");
	push(0, "");
	push(0, "");
	push(0, "");

	for (int i = 0; i < ARRAY_SIZE(decode->dpb); i++) {
		if (!(dpb[i].flags & V4L2_H264_DPB_ENTRY_FLAG_VALID))
			continue;

		ref = avd_get_ref_buf(ctx, &dst->base.vb, dpb[i].reference_ts);

		addr = vb2_dma_contig_plane_dma_addr(&ref->base.vb.vb2_buf, 0) +
		       ref->comp.start_offset;

		push(AVD_REF_NUM(run->num_valid - 1) | AVD_REF_FLAG_CONST |
			     AVD_REF_FLAG_LONG(
				     dpb[i].flags &
				     V4L2_H264_DPB_ENTRY_FLAG_LONG_TERM) |
			     AVD_REF_DELTA_POC(run->cur_poc -
					       dpb[i].top_field_order_cnt),
		     "hdr_d0_ref_hdr");

		push_comp(ctx, addr, ctx->comp.offsets);
	}
}

static void stream_scaling(struct avd_ctx *ctx, struct avd_h264_run *run)
{
	const struct v4l2_ctrl_h264_pps *pps = run->pps;
	const struct v4l2_ctrl_h264_scaling_matrix *scaling =
		run->scaling_matrix;

	push(H264_SCL_DIMS, "hdr_4c_pic_scaling_list_dims");

	for (int i = 0; i < 6; i++)
		for (int j = 0; j < 16; j += 4)
			push(AVD_SCALING_I3(
				     scaling->scaling_list_4x4[i][j + 0]) |
				     AVD_SCALING_I2(
					     scaling->scaling_list_4x4[i][j + 1]) |
				     AVD_SCALING_I1(
					     scaling->scaling_list_4x4[i][j + 2]) |
				     AVD_SCALING_I0(
					     scaling->scaling_list_4x4[i][j + 3]),
			     "scl_46c_pic_scaling_matrix_4x4");

	/* Instead of 8x8 raster scan order avd expects 4 4x4 subblocks */
	static const u8 map[16] = {
		0,  8,	16, 24, /* top left */
		4,  12, 20, 28, /* top right */
		32, 40, 48, 56, /* bottom left */
		36, 44, 52, 60, /* bottom right */
	};

	/* 7.3.2.2, only matrix 0 and 1 are used if chroma_format_idc < 3 */
	if (pps->flags & V4L2_H264_PPS_FLAG_TRANSFORM_8X8_MODE) {
		for (int i = 0; i < 2; i++)
			for (int j = 0; j < 16; j++)
				push(AVD_SCALING_I3(scaling->scaling_list_8x8
							    [i][map[j] + 0]) |
					     AVD_SCALING_I2(
						     scaling->scaling_list_8x8
							     [i][map[j] + 1]) |
					     AVD_SCALING_I1(
						     scaling->scaling_list_8x8
							     [i][map[j] + 2]) |
					     AVD_SCALING_I0(
						     scaling->scaling_list_8x8
							     [i][map[j] + 3]),
				     "scl_4cc_pic_scaling_matrix_8x8");

	} else {
		for (int i = 0; i < ARRAY_SIZE(default_8x8_intra); i++)
			push(default_8x8_intra[i],
			     "scl_4cc_pic_scaling_matrix_8x8");
		for (int i = 0; i < ARRAY_SIZE(default_8x8_inter); i++)
			push(default_8x8_inter[i],
			     "scl_4cc_pic_scaling_matrix_8x8");
	}
}

static void stream_hdr(struct avd_ctx *ctx, struct avd_h264_run *run)
{
	const struct v4l2_ctrl_h264_decode_params *decode = run->decode_params;
	const struct v4l2_ctrl_h264_sps *sps = run->sps;
	const struct v4l2_ctrl_h264_pps *pps = run->pps;
	struct avd_dev *avd = ctx->dev;
	struct avd_h264_ctx *h264_ctx = ctx->priv;
	u32 bytesperline;
	u32 width = (sps->pic_width_in_mbs_minus1 + 1) * 16;
	u32 height = (sps->pic_height_in_map_units_minus1 + 1) * 16;

	push(AVD_OP_HDR | AVD_OP_HDR_FLAG_DECOMP(ctx->decomp) |
		     AVD_OP_HDR_FLAG_INTRA(
			     decode->flags &
			     V4L2_H264_DECODE_PARAM_FLAG_IDR_PIC) |
		     AVD_OP_HDR_CONST |
		     AVD_OP_HDR_FLAG_PIPE_STATE(
			     !(avd->variant->quirks & AVD_QUIRK_NO_PIPE_STATE)),
	     "hdr_34_start_hdr");

	push(AVD_HDR_CODEC_MODE(AVD_CODEC_H264), "hdr_38_mode");

	push(AVD_HDR_HEIGHT(height - 1) | AVD_HDR_WIDTH(width - 1),
	     "hdr_3c_height_width");

	push(0, "hdr_40_zero");

	push(AVD_HDR_HEIGHT((height - 1) >> 3) |
		     AVD_HDR_WIDTH((width - 1) >> 3),
	     "hdr_28_height_width_shift3");

	/* TODO: did i mix up luma/chroma in hevc or here? */
	push(AVD_HDR_COMMON_CHROMA_FORMAT(sps->chroma_format_idc) |
		     AVD_HDR_COMMON_BIT_DEPTH_L(sps->bit_depth_luma_minus8) |
		     AVD_HDR_COMMON_BIT_DEPTH_C(sps->bit_depth_chroma_minus8) |
		     AVD_HDR_COMMON_MIN_LUMA_CBS(1) |
		     AVD_HDR_COMMON_LUMA_CBS(1) |
		     H264_TRANSFORM_8X8_MODE(
			     pps->flags &
			     V4L2_H264_PPS_FLAG_TRANSFORM_8X8_MODE) |
		     AVD_HDR_COMMON_FLAG0(
			     sps->flags &
			     V4L2_H264_SPS_FLAG_DIRECT_8X8_INFERENCE),
	     "hdr_2c_sps_param");

	push(H264_FLAG_ENTROPY_CODING_MODE(
		     pps->flags & V4L2_H264_PPS_FLAG_ENTROPY_CODING_MODE) |
		     H264_FLAG_NOT_IDR(!(decode->flags &
					 V4L2_H264_DECODE_PARAM_FLAG_IDR_PIC)) |
		     h264_FLAG_CI_PRED(
			     pps->flags &
			     V4L2_H264_PPS_FLAG_CONSTRAINED_INTRA_PRED),
	     "hdr_44_flags");

	push(AVD_HDR_H26X_QP_OFFSET_CB(pps->chroma_qp_index_offset) |
		     AVD_HDR_H26X_QP_OFFSET_CR(
			     pps->second_chroma_qp_index_offset),
	     "hdr_48_chroma_qp_index_offset");

	push(AVD_HDR_FEAT_H26X | AVD_HDR_FEAT_COMMON | AVD_HDR_FEAT_H264 |
		     AVD_HDR_FEAT_PIPE_STATE_EN(
			     !(avd->variant->quirks & AVD_QUIRK_NO_PIPE_STATE)),
	     "hdr_58_const_3a");

	push(0, "");
	push(0, "");

	if (avd->variant->revision == 3)
		push(0, "zero");

	pusha(h264_ctx->bufs.above_info.addr, "hdr_9c_pps_tile_addr_lsb8", 0);

	push(0, "");
	push(0, "");

	if (avd->variant->revision == 3)
		push(0, "zero");
	else if (!(avd->variant->quirks & AVD_QUIRK_NO_PIPE_STATE))
		pusha(h264_ctx->bufs.pipe_state.addr, "pipe_state", 0);

	pusha(h264_ctx->bufs.ip_above.addr, "ip_above", 1);
	pusha(h264_ctx->bufs.lf_above.addr, "lf_above", 2);
	pusha(h264_ctx->bufs.lf_above_info.addr, "lf_above_info", 3);
	push(0, "");

	push_comp(ctx, run->base.comp_out, ctx->comp.offsets);

	bytesperline = ctx->decoded_fmt.fmt.pix_mp.plane_fmt[0].bytesperline;
	if (avd->variant->quirks & AVD_QUIRK_LSR)
		bytesperline = bytesperline >> 4;

	pusha(run->base.y_out, "hdr_210_y_addr_lsb8", 0);
	push(bytesperline, "hdr_218_width_align");
	pusha(run->base.uv_out, "hdr_214_uv_addr_lsb8", 0);
	push(bytesperline, "hdr_21c_width_align");

	push(0, "cm3_mark_end_section");
	push(((height - 1) << 16) | (width - 1), "hdr_54_height_width");

	if (!(decode->flags & V4L2_H264_DECODE_PARAM_FLAG_IDR_PIC))
		stream_refs(ctx, run);

	if (pps->flags & V4L2_H264_PPS_FLAG_SCALING_MATRIX_PRESENT)
		stream_scaling(ctx, run);
	else
		push(0, "cm3_mark_end_section_scl");
}

#define DEFAULT_WEIGHT_DENOM \
	(AVD_OP_WEIGHTS_HDR_LUMA(5) | AVD_OP_WEIGHTS_HDR_CHROMA(5))

static void stream_weights(struct avd_ctx *ctx, struct avd_h264_run *run)
{
	int luma_denom, chroma_denom;
	struct v4l2_h264_weight_factors factors;
	const struct v4l2_ctrl_h264_pred_weights *weights = run->pred_weights;
	const struct v4l2_ctrl_h264_pps *pps = run->pps;
	const struct v4l2_ctrl_h264_slice_params *sl = run->slice_params;

	bool pred_weight_req = V4L2_H264_CTRL_PRED_WEIGHTS_REQUIRED(pps, sl);
	bool default_weights = pps->weighted_bipred_idc == 2 &&
			       !pred_weight_req;

	push(AVD_OP_WEIGHTS_HDR | AVD_OP_WEIGHTS_HDR_FLAG1(default_weights) |
		     AVD_OP_WEIGHTS_HDR_FLAG0(pred_weight_req) |
		     AVD_OP_WEIGHTS_HDR_LUMA(
			     !default_weights ?
				     weights->luma_log2_weight_denom :
				     0) |
		     AVD_OP_WEIGHTS_HDR_CHROMA(
			     !default_weights ?
				     weights->chroma_log2_weight_denom :
				     0) |
		     (default_weights ? DEFAULT_WEIGHT_DENOM : 0),
	     "slc_76c_cmd_weights_denom");

	if (!pred_weight_req)
		return;

	luma_denom = 1 << weights->luma_log2_weight_denom;
	chroma_denom = 1 << weights->chroma_log2_weight_denom;

	for (int y = 0; y < 2; y++) {
		if (y == 1 && sl->slice_type != V4L2_H264_SLICE_TYPE_B)
			break;

		factors = weights->weight_factors[y];
		int to = y == 0 ? sl->num_ref_idx_l0_active_minus1 :
				  sl->num_ref_idx_l1_active_minus1;
		for (int i = 0; i < to + 1; i++) {
			/* Avd only expects offsets/weights if they are not the default
			 * ones, otherwise we get artifacts */
			if ((factors.luma_weight[i] != luma_denom) ||
			    (factors.luma_offset[i] != 0)) {
				push(AVD_OP_WEIGHTS | AVD_OP_WEIGHTS_IDENT(1) |
					     AVD_OP_WEIGHTS_LIST_IDX(y) |
					     AVD_OP_WEIGHTS_INDEX(i) |
					     AVD_OP_WEIGHTS_WEIGHT(
						     factors.luma_weight[i]),
				     "slc_luma_weights");
				push(AVD_OP_OFFSETS |
					     AVD_OP_OFFSETS_OFFSET(
						     factors.luma_offset[i]),
				     "slc_luma_offsets");
			}

			if ((factors.chroma_weight[i][0] != chroma_denom) ||
			    (factors.chroma_offset[i][0] != 0) ||
			    (factors.chroma_weight[i][1] != chroma_denom) ||
			    (factors.chroma_offset[i][1] != 0)) {
				push(AVD_OP_WEIGHTS | AVD_OP_WEIGHTS_IDENT(2) |
					     AVD_OP_WEIGHTS_LIST_IDX(y) |
					     AVD_OP_WEIGHTS_INDEX(i) |
					     AVD_OP_WEIGHTS_WEIGHT(
						     factors.chroma_weight[i][0]),
				     "slc_chroma_weights[0]");
				push(AVD_OP_OFFSETS |
					     AVD_OP_OFFSETS_OFFSET(
						     factors.chroma_offset[i][0]),
				     "slc_chroma_offsets[0]");
				push(AVD_OP_WEIGHTS | AVD_OP_WEIGHTS_IDENT(3) |
					     AVD_OP_WEIGHTS_LIST_IDX(y) |
					     AVD_OP_WEIGHTS_INDEX(i) |
					     AVD_OP_WEIGHTS_WEIGHT(
						     factors.chroma_weight[i][1]),
				     "slc_chroma_weights[1]");
				push(AVD_OP_OFFSETS |
					     AVD_OP_OFFSETS_OFFSET(
						     factors.chroma_offset[i][1]),
				     "slc_chroma_offsets[1]");
			}
		}
	}
}

/*
 * Find the slice data in a slice: skip the two bytes in front of the slice
 * header, then the header_len bytes of the header itself, which do not count
 * its emulation prevention bytes. At least one byte of slice data must follow.
 */
VISIBLE_IF_KUNIT bool avd_h264_find_slice_data(const u8 *data, u32 len,
					       u32 header_len, u32 *off)
{
	u32 pos = 2, bytes_read = 2;

	while (bytes_read < header_len && pos < len) {
		if (data[pos - 2] != 0x00 || data[pos - 1] != 0x00 ||
		    data[pos] != 0x03)
			bytes_read++;
		pos++;
	}

	if (pos >= len)
		return false;

	*off = pos;
	return true;
}

/* Bytes of slice header in front of the slice data. */
static u32 avd_h264_header_len(const struct avd_h264_run *run)
{
	/* CABAC slice data starts on a byte boundary. */
	bool cabac = run->pps->flags & V4L2_H264_PPS_FLAG_ENTROPY_CODING_MODE;

	return (run->slice_params->header_bit_size + (cabac ? 7 : 0)) / 8;
}

static void stream_slice(struct avd_ctx *ctx, struct avd_h264_run *run)
{
	const struct v4l2_ctrl_h264_decode_params *decode = run->decode_params;
	const struct v4l2_ctrl_h264_pps *pps = run->pps;
	const struct v4l2_ctrl_h264_sps *sps = run->sps;
	const struct v4l2_ctrl_h264_slice_params *sl = run->slice_params;
	struct avd_h264_ctx *h264_ctx = ctx->priv;
	u32 payload_len = h264_ctx->active_slice->size;
	bool en_mode = (pps->flags & V4L2_H264_PPS_FLAG_ENTROPY_CODING_MODE) ==
		       0;
	u32 off = run->slice_data_off;

	dma_addr_t slc_a84 = h264_ctx->active_slice->addr + off;

	push(AVD_OP_CODED_DATA |
		     AVD_OP_CODED_DATA_BIT_OFF(
			     en_mode ? (sl->header_bit_size % 8) : 0) |
		     AVD_OP_CODED_DATA_ADDR(slc_a84 >> 32),
	     "slc_a7c_cmd_set_coded_slice");
	push((u32)(slc_a84 & 0xffffffff), "slc_a84_slice_addr_low");

	/* should not include trailing 0? */
	push(payload_len - off, "slc_a88_slice_hdr_size");

	push(AVD_OP_SL_LOC |
		     AVD_OP_SL_LOC_Y(sl->first_mb_in_slice /
				     (sps->pic_width_in_mbs_minus1 + 1)) |
		     AVD_OP_SL_LOC_X(sl->first_mb_in_slice %
				     (sps->pic_width_in_mbs_minus1 + 1)),
	     "cm3_cmd_exec_mb_vp");

	push(AVD_OP_QP | AVD_OP_QP_VAL(26 + pps->pic_init_qp_minus26 +
				       sl->slice_qp_delta),
	     "slc_a70_cmd_quant_param");

	push(AVD_OP_DBLK |
		     AVD_OP_DBLK_FLAG_FULL_EN(
			     sl->disable_deblocking_filter_idc == 0) |
		     AVD_OP_DBLK_FLAG_EN(sl->disable_deblocking_filter_idc !=
					 1) |
		     AVD_OP_DBLK_OFF1(sl->slice_beta_offset_div2) |
		     AVD_OP_DBLK_OFF0(sl->slice_alpha_c0_offset_div2),
	     "slc_a74_cmd_deblocking_filter");

	if (sl->slice_type == V4L2_H264_SLICE_TYPE_P ||
	    sl->slice_type == V4L2_H264_SLICE_TYPE_B) {
		u32 num_ref_idx_active = sl->num_ref_idx_l0_active_minus1 + 1;
		for (u32 i = 0; i < num_ref_idx_active; i++)
			push(AVD_OP_REF | AVD_OP_REF_LIST_IDX(0) |
				     AVD_OP_REF_LOOP_IDX(i) |
				     AVD_OP_REF_DBP_IDX(
					     sl->ref_pic_list0[i].index),
			     "slc_6e8_cmd_ref_list_0");

		if (sl->slice_type == V4L2_H264_SLICE_TYPE_B) {
			u32 num_ref_idx_active =
				sl->num_ref_idx_l1_active_minus1 + 1;
			for (u32 i = 0; i < num_ref_idx_active; i++)
				push(AVD_OP_REF | AVD_OP_REF_LIST_IDX(1) |
					     AVD_OP_REF_LOOP_IDX(i) |
					     AVD_OP_REF_DBP_IDX(
						     sl->ref_pic_list1[i].index),
				     "slc_6e8_cmd_ref_list_0");
		}
		stream_weights(ctx, run);
	}

	if (sl->first_mb_in_slice == 0) {
		push(AVD_OP_SL_DIM_START, "cm3_cmd_set_mb_dims");
		push(AVD_SL_DIM_END_Y(sps->pic_height_in_map_units_minus1) |
			     AVD_SL_DIM_END_X(sps->pic_width_in_mbs_minus1),
		     "cm3_set_mb_dims");
	}

	push(AVD_OP_SL_REF | AVD_OP_SL_REF_FLAG_CABAC(sl->cabac_init_idc == 1) |
		     AVD_OP_SL_REF_FLAG1(sl->cabac_init_idc == 2) |
		     AVD_OP_SL_REF_FLAG2(
			     !(sl->flags &
			       V4L2_H264_SLICE_FLAG_DIRECT_SPATIAL_MV_PRED)) |
		     AVD_OP_SL_REF_NUM_L0(sl->num_ref_idx_l0_active_minus1) |
		     AVD_OP_SL_REF_NUM_L1(sl->num_ref_idx_l1_active_minus1) |
		     AVD_OP_SL_REF_SLICE_P(sl->slice_type ==
					   V4L2_H264_SLICE_TYPE_P) |
		     AVD_OP_SL_REF_SLICE_I(sl->slice_type ==
					   V4L2_H264_SLICE_TYPE_I) |
		     AVD_OP_SL_REF_SLICE_B(sl->slice_type ==
					   V4L2_H264_SLICE_TYPE_B)

		     ,
	     "slc_6e4_cmd_ref_type");

	if (sl->slice_type == V4L2_H264_SLICE_TYPE_B) {
		/* bidirectional reference of previous mv */
		struct vb2_buffer *vb = vb2_find_buffer(
			&ctx->fh.m2m_ctx->cap_q_ctx.q,
			decode->dpb[sl->ref_pic_list1[0].index].reference_ts);

		dma_addr_t mv_color_addr =
			vb ? vb2_dma_contig_plane_dma_addr(vb, 0) +
					(vb->planes[0].length -
					 mv_color_size(fmt_width(ctx),
						       fmt_height(ctx))) :
			     run->addresses.mv_color;

		pusha(mv_color_addr, "slc_a78_sps_tile_addr2_lsb8", 0);
	}
}


static int avd_h264_alloc_bufs; /* extraction end marker */
