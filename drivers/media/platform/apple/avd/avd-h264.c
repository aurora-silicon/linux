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

#include <kunit/visibility.h>
#include <linux/dev_printk.h>
#include <linux/mm.h>
#include <linux/slab.h>

#include <media/v4l2-h264.h>
#include <media/videobuf2-dma-contig.h>

#include "avd.h"
#include "avd-inst.h"
#include "avd-queue.h"
#include "avd-h264-fp.h"

#define H264_SCL_DIMS	(0x1000000 | ((64 / 4) << 5) | (((16 / 4) << 5) - 1))

#define h264_FLAG_CI_PRED(v)			FIELD_PREP(BIT(19), !!(v))
#define H264_FLAG_ENTROPY_CODING_MODE(v)	FIELD_PREP(BIT(20), !!(v))
#define H264_FLAG_NOT_IDR(v)			FIELD_PREP(BIT(21), !!(v))

#define H264_TRANSFORM_8X8_MODE(v)		FIELD_PREP(BIT(7), !!(v))

/* not a hardware constraint */
#define MAX_SLICES	4096

struct avd_h264_ctx;

/* Only metadata is copied. The core owns each OUTPUT until picture retire. */
struct avd_h264_slice {
	struct avd_buf data;
	struct v4l2_ctrl_h264_slice_params params;
	struct v4l2_ctrl_h264_pred_weights weights;
	u32 data_offset;
};

struct avd_h264_run {
	struct avd_frame *frame;
	struct avd_h264_ctx *picture;
	struct avd_h264_fp_cursor fp_cursor;
	unsigned int ref_index;
	int error;
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

	/* CPU view and DMA address of OUTPUT; never owns coded-data memory. */
	struct avd_buf slice;
	struct avd_h264_ctx *slots[AVD_MAX_FRAMES];
	struct avd_h264_ctx *pending;
	bool busy;
	struct avd_buf *active_slice;
	size_t slice_num;
	size_t slice_bytes;
	size_t alloc_slices;
	struct avd_h264_slice *slices;
	struct vb2_v4l2_buffer *dst;
	struct v4l2_ctrl_h264_sps sps;
	struct v4l2_ctrl_h264_pps pps;
	struct v4l2_ctrl_h264_decode_params decode;
	struct v4l2_ctrl_h264_scaling_matrix scaling;

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

static void avd_h264_emit(struct avd_ctx *ctx, struct avd_h264_run *run,
			  u32 word, enum avd_h264_fp_field field)
{
	struct avd_frame *f = run->frame;

	if (!f) {
		(push)(ctx, word);
		return;
	}
	if (run->error)
		return;
	if (f->prog && f->prog->cpu) {
		if (f->prog_words >= f->prog->size / sizeof(__le32)) {
			run->error = -ENOSPC;
			return;
		}
		((__le32 *)f->prog->cpu)[f->prog_words++] = cpu_to_le32(word);
	} else if (!run->fp_cursor.buf) {
		run->error = -EINVAL;
		return;
	}
	if (run->fp_cursor.buf)
		run->error = avd_h264_fp_emit(&run->fp_cursor, field, word);
}

static void avd_h264_emit_addr(struct avd_ctx *ctx, struct avd_h264_run *run,
			       dma_addr_t addr, enum avd_h264_fp_field field,
			       unsigned int index)
{
	/* Open instruction addresses retain the existing per-variant encoding. */
	if (ctx->dev->variant->quirks & AVD_QUIRK_LSR) {
		avd_h264_emit(ctx, run, addr >> 8, AVD_H264_FP_NONE);
	} else {
		avd_h264_emit(ctx, run, lower_32_bits(addr), AVD_H264_FP_NONE);
		avd_h264_emit(ctx, run, upper_32_bits(addr), AVD_H264_FP_NONE);
	}
	if (run->fp_cursor.buf && !run->error)
		/* V3 stores the same shifted word as the LSR instruction. */
		run->error = avd_h264_fp_address(&run->fp_cursor, field, index,
					 addr & ~(dma_addr_t)0xff);
}

static void avd_h264_emit_comp(struct avd_ctx *ctx, struct avd_h264_run *run,
			       dma_addr_t addr, bool reference)
{
	unsigned int i;

	for (i = 0; i < 4; i++) {
		dma_addr_t a = addr + ctx->comp.offsets[i];

		if (ctx->dev->variant->quirks & AVD_QUIRK_LSR)
			avd_h264_emit(ctx, run, a >> 7, AVD_H264_FP_NONE);
		else
			avd_h264_emit_addr(ctx, run, a, AVD_H264_FP_NONE, 0);
		if (run->fp_cursor.buf && !run->error)
			run->error = avd_h264_fp_ref(&run->fp_cursor, reference,
						 run->ref_index, i, a);
	}
}

#undef push
#undef pusha
#define push(word, field) avd_h264_emit(ctx, run, (word), (field))
#define pusha(addr, field, index) \
	avd_h264_emit_addr(ctx, run, (addr), (field), (index))

static void stream_refs(struct avd_ctx *ctx, struct avd_h264_run *run)
{
	const struct v4l2_ctrl_h264_decode_params *decode = run->decode_params;
	const struct v4l2_h264_dpb_entry *dpb = decode->dpb;
	struct avd_h264_ctx *h264_ctx = run->picture;
	struct avd_decoded_buffer *dst, *ref;
	dma_addr_t addr;

	dst = vb2_to_avd_decoded_buf(&run->base.bufs.dst->vb2_buf);

	push(0, AVD_H264_FP_NONE);
	pusha(h264_ctx->bufs.mv_above_info.addr, AVD_H264_FP_MV_ABOVE_INFO, 7);
	pusha(run->addresses.mv_color, AVD_H264_FP_MV_COLOR, 0);

	push(0, AVD_H264_FP_NONE);
	push(0, AVD_H264_FP_NONE);
	push(0, AVD_H264_FP_NONE);
	push(0, AVD_H264_FP_NONE);

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
		     AVD_H264_FP_HDR_D0_REF_HDR);

		avd_h264_emit_comp(ctx, run, addr, true);
		run->ref_index++;
	}
}

static void stream_scaling(struct avd_ctx *ctx, struct avd_h264_run *run)
{
	const struct v4l2_ctrl_h264_pps *pps = run->pps;
	const struct v4l2_ctrl_h264_scaling_matrix *scaling =
		run->scaling_matrix;

	push(H264_SCL_DIMS, AVD_H264_FP_HDR_4C_PIC_SCALING_LIST_DIMS);

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
			     AVD_H264_FP_SCL_46C_PIC_SCALING_MATRIX_4X4);

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
				     AVD_H264_FP_SCL_4CC_PIC_SCALING_MATRIX_8X8);

	} else {
		for (int i = 0; i < ARRAY_SIZE(default_8x8_intra); i++)
			push(default_8x8_intra[i],
			     AVD_H264_FP_SCL_4CC_PIC_SCALING_MATRIX_8X8);
		for (int i = 0; i < ARRAY_SIZE(default_8x8_inter); i++)
			push(default_8x8_inter[i],
			     AVD_H264_FP_SCL_4CC_PIC_SCALING_MATRIX_8X8);
	}
}

static void stream_hdr(struct avd_ctx *ctx, struct avd_h264_run *run)
{
	const struct v4l2_ctrl_h264_decode_params *decode = run->decode_params;
	const struct v4l2_ctrl_h264_sps *sps = run->sps;
	const struct v4l2_ctrl_h264_pps *pps = run->pps;
	struct avd_dev *avd = ctx->dev;
	struct avd_h264_ctx *h264_ctx = run->picture;
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
	     AVD_H264_FP_HDR_34_START_HDR);

	push(AVD_HDR_CODEC_MODE(AVD_CODEC_H264), AVD_H264_FP_HDR_38_MODE);

	push(AVD_HDR_HEIGHT(height - 1) | AVD_HDR_WIDTH(width - 1),
	     AVD_H264_FP_HDR_3C_HEIGHT_WIDTH);

	push(0, AVD_H264_FP_HDR_40_ZERO);

	push(AVD_HDR_HEIGHT((height - 1) >> 3) |
		     AVD_HDR_WIDTH((width - 1) >> 3),
	     AVD_H264_FP_HDR_28_HEIGHT_WIDTH_SHIFT3);

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
	     AVD_H264_FP_HDR_2C_SPS_PARAM);

	push(H264_FLAG_ENTROPY_CODING_MODE(
		     pps->flags & V4L2_H264_PPS_FLAG_ENTROPY_CODING_MODE) |
		     H264_FLAG_NOT_IDR(!(decode->flags &
					 V4L2_H264_DECODE_PARAM_FLAG_IDR_PIC)) |
		     h264_FLAG_CI_PRED(
			     pps->flags &
			     V4L2_H264_PPS_FLAG_CONSTRAINED_INTRA_PRED),
	     AVD_H264_FP_HDR_44_FLAGS);

	push(AVD_HDR_H26X_QP_OFFSET_CB(pps->chroma_qp_index_offset) |
		     AVD_HDR_H26X_QP_OFFSET_CR(
			     pps->second_chroma_qp_index_offset),
	     AVD_H264_FP_HDR_48_CHROMA_QP_INDEX_OFFSET);

	push(AVD_HDR_FEAT_H26X | AVD_HDR_FEAT_COMMON | AVD_HDR_FEAT_H264 |
		     AVD_HDR_FEAT_PIPE_STATE_EN(
			     !(avd->variant->quirks & AVD_QUIRK_NO_PIPE_STATE)),
	     AVD_H264_FP_HDR_58_CONST_3A);

	push(0, AVD_H264_FP_NONE);
	push(0, AVD_H264_FP_NONE);

	if (avd->variant->revision == 3)
		push(0, AVD_H264_FP_NONE);

	pusha(h264_ctx->bufs.above_info.addr, AVD_H264_FP_HDR_9C_PPS_TILE_ADDR_LSB8, 0);

	push(0, AVD_H264_FP_NONE);
	push(0, AVD_H264_FP_NONE);

	if (avd->variant->revision == 3)
		push(0, AVD_H264_FP_NONE);
	else if (!(avd->variant->quirks & AVD_QUIRK_NO_PIPE_STATE))
		pusha(h264_ctx->bufs.pipe_state.addr, AVD_H264_FP_NONE, 0);

	pusha(h264_ctx->bufs.ip_above.addr, AVD_H264_FP_IP_ABOVE, 1);
	pusha(h264_ctx->bufs.lf_above.addr, AVD_H264_FP_LF_ABOVE, 2);
	pusha(h264_ctx->bufs.lf_above_info.addr, AVD_H264_FP_LF_ABOVE_INFO, 3);
	push(0, AVD_H264_FP_NONE);

	avd_h264_emit_comp(ctx, run, run->base.comp_out, false);

	bytesperline = ctx->decoded_fmt.fmt.pix_mp.plane_fmt[0].bytesperline;
	if (avd->variant->quirks & AVD_QUIRK_LSR)
		bytesperline = bytesperline >> 4;

	pusha(run->base.y_out, AVD_H264_FP_HDR_210_Y_ADDR_LSB8, 0);
	push(bytesperline, AVD_H264_FP_HDR_218_WIDTH_ALIGN);
	pusha(run->base.uv_out, AVD_H264_FP_HDR_214_UV_ADDR_LSB8, 0);
	push(bytesperline, AVD_H264_FP_HDR_21C_WIDTH_ALIGN);

	push(0, AVD_H264_FP_NONE);
	push(((height - 1) << 16) | (width - 1), AVD_H264_FP_HDR_54_HEIGHT_WIDTH);

	if (!(decode->flags & V4L2_H264_DECODE_PARAM_FLAG_IDR_PIC))
		stream_refs(ctx, run);

	if (pps->flags & V4L2_H264_PPS_FLAG_SCALING_MATRIX_PRESENT)
		stream_scaling(ctx, run);
	else
		push(0, AVD_H264_FP_NONE);
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
	     AVD_H264_FP_SLC_76C_CMD_WEIGHTS_DENOM);

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
				     AVD_H264_FP_SLC_LUMA_WEIGHTS);
				push(AVD_OP_OFFSETS |
					     AVD_OP_OFFSETS_OFFSET(
						     factors.luma_offset[i]),
				     AVD_H264_FP_SLC_LUMA_OFFSETS);
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
				     AVD_H264_FP_SLC_CHROMA_WEIGHTS_0_);
				push(AVD_OP_OFFSETS |
					     AVD_OP_OFFSETS_OFFSET(
						     factors.chroma_offset[i][0]),
				     AVD_H264_FP_SLC_CHROMA_OFFSETS_0_);
				push(AVD_OP_WEIGHTS | AVD_OP_WEIGHTS_IDENT(3) |
					     AVD_OP_WEIGHTS_LIST_IDX(y) |
					     AVD_OP_WEIGHTS_INDEX(i) |
					     AVD_OP_WEIGHTS_WEIGHT(
						     factors.chroma_weight[i][1]),
				     AVD_H264_FP_SLC_CHROMA_WEIGHTS_1_);
				push(AVD_OP_OFFSETS |
					     AVD_OP_OFFSETS_OFFSET(
						     factors.chroma_offset[i][1]),
				     AVD_H264_FP_SLC_CHROMA_OFFSETS_1_);
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
	struct avd_h264_ctx *h264_ctx = run->picture;
	u32 payload_len = h264_ctx->active_slice->size;
	bool en_mode = (pps->flags & V4L2_H264_PPS_FLAG_ENTROPY_CODING_MODE) ==
		       0;
	u32 off = run->slice_data_off;

	dma_addr_t slc_a84 = h264_ctx->active_slice->addr + off;

	push(AVD_OP_CODED_DATA |
		     AVD_OP_CODED_DATA_BIT_OFF(
			     en_mode ? (sl->header_bit_size % 8) : 0) |
		     AVD_OP_CODED_DATA_ADDR(slc_a84 >> 32),
	     AVD_H264_FP_SLC_A7C_CMD_SET_CODED_SLICE);
	push((u32)(slc_a84 & 0xffffffff), AVD_H264_FP_SLC_A84_SLICE_ADDR_LOW);

	/* should not include trailing 0? */
	push(payload_len - off, AVD_H264_FP_SLC_A88_SLICE_HDR_SIZE);

	push(AVD_OP_SL_LOC |
		     AVD_OP_SL_LOC_Y(sl->first_mb_in_slice /
				     (sps->pic_width_in_mbs_minus1 + 1)) |
		     AVD_OP_SL_LOC_X(sl->first_mb_in_slice %
				     (sps->pic_width_in_mbs_minus1 + 1)),
	     AVD_H264_FP_NONE);

	push(AVD_OP_QP | AVD_OP_QP_VAL(26 + pps->pic_init_qp_minus26 +
				       sl->slice_qp_delta),
	     AVD_H264_FP_SLC_A70_CMD_QUANT_PARAM);

	push(AVD_OP_DBLK |
		     AVD_OP_DBLK_FLAG_FULL_EN(
			     sl->disable_deblocking_filter_idc == 0) |
		     AVD_OP_DBLK_FLAG_EN(sl->disable_deblocking_filter_idc !=
					 1) |
		     AVD_OP_DBLK_OFF1(sl->slice_beta_offset_div2) |
		     AVD_OP_DBLK_OFF0(sl->slice_alpha_c0_offset_div2),
	     AVD_H264_FP_SLC_A74_CMD_DEBLOCKING_FILTER);

	if (sl->slice_type == V4L2_H264_SLICE_TYPE_P ||
	    sl->slice_type == V4L2_H264_SLICE_TYPE_B) {
		u32 num_ref_idx_active = sl->num_ref_idx_l0_active_minus1 + 1;
		for (u32 i = 0; i < num_ref_idx_active; i++)
			push(AVD_OP_REF | AVD_OP_REF_LIST_IDX(0) |
				     AVD_OP_REF_LOOP_IDX(i) |
				     AVD_OP_REF_DBP_IDX(
					     sl->ref_pic_list0[i].index),
			     AVD_H264_FP_SLC_6E8_CMD_REF_LIST_0);

		if (sl->slice_type == V4L2_H264_SLICE_TYPE_B) {
			u32 num_ref_idx_active =
				sl->num_ref_idx_l1_active_minus1 + 1;
			for (u32 i = 0; i < num_ref_idx_active; i++)
				push(AVD_OP_REF | AVD_OP_REF_LIST_IDX(1) |
					     AVD_OP_REF_LOOP_IDX(i) |
					     AVD_OP_REF_DBP_IDX(
						     sl->ref_pic_list1[i].index),
				     AVD_H264_FP_SLC_6E8_CMD_REF_LIST_0);
		}
		stream_weights(ctx, run);
	}

	if (sl->first_mb_in_slice == 0) {
		push(AVD_OP_SL_DIM_START, AVD_H264_FP_NONE);
		push(AVD_SL_DIM_END_Y(sps->pic_height_in_map_units_minus1) |
			     AVD_SL_DIM_END_X(sps->pic_width_in_mbs_minus1),
		     AVD_H264_FP_NONE);
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
	     AVD_H264_FP_SLC_6E4_CMD_REF_TYPE);

	if (sl->slice_type == V4L2_H264_SLICE_TYPE_B) {
		/* bidirectional reference of previous mv */
		struct avd_decoded_buffer *ref = avd_get_ref_buf(
			ctx, run->base.bufs.dst,
			decode->dpb[sl->ref_pic_list1[0].index].reference_ts);
		struct vb2_buffer *vb = &ref->base.vb.vb2_buf;

		dma_addr_t mv_color_addr =
			vb ? vb2_dma_contig_plane_dma_addr(vb, 0) +
					(vb->planes[0].length -
					 mv_color_size(fmt_width(ctx),
						       fmt_height(ctx))) :
			     run->addresses.mv_color;

		pusha(mv_color_addr, AVD_H264_FP_SLC_A78_SPS_TILE_ADDR2_LSB8, 0);
	}
}

static int avd_h264_alloc_bufs(struct avd_ctx *ctx,
			       struct avd_h264_ctx *h264_ctx)
{
	struct avd_dev *dev = ctx->dev;
	int ret, w, bit_depth, mb;
	w = fmt_width(ctx);
	bit_depth = (ctx->image_fmt == AVD_IMG_FMT_420_10BIT ||
		     ctx->image_fmt == AVD_IMG_FMT_422_10BIT) ?
			    10 :
			    8;

	if (!(dev->variant->quirks & AVD_QUIRK_NO_PIPE_STATE)) {
		ret = avd_buf_alloc(dev, &h264_ctx->bufs.pipe_state, 0x200);
		if (ret) {
			return ret;
		}
	}

	ret = avd_buf_alloc(dev, &h264_ctx->bufs.inst, fifo_size());
	if (ret)
		return ret;

	mb = DIV_ROUND_UP(w, 16);

	ret = avd_buf_alloc(dev, &h264_ctx->bufs.above_info, mb * 20);
	if (ret)
		return ret;

	ret = avd_buf_alloc(dev, &h264_ctx->bufs.ip_above, bit_depth * 4 * mb);
	if (ret)
		return ret;

	ret = avd_buf_alloc(dev, &h264_ctx->bufs.lf_above,
			    bit_depth * 4 * 4 * mb);
	if (ret)
		return ret;

	ret = avd_buf_alloc(dev, &h264_ctx->bufs.lf_above_info, 32 * mb);
	if (ret)
		return ret;

	ret = avd_buf_alloc(dev, &h264_ctx->bufs.mv_above_info, 32 * mb);
	if (ret)
		return ret;

	return 0;
}

static int avd_h264_validate_pps(struct avd_ctx *ctx,
				 const struct v4l2_ctrl_h264_pps *pps)
{
	if (pps->num_slice_groups_minus1 != 0) {
		dev_err(ctx->dev->dev, "pps->num_slice_groups_minus1 != 0");
		return -EINVAL;
	}

	return 0;
}

static int avd_h264_validate_sps(struct avd_ctx *ctx,
				 const struct v4l2_ctrl_h264_sps *sps)
{
	if (sps->chroma_format_idc > 2)
		/* Only 4:0:0, 4:2:0 and 4:2:2 are supported */
		return -EINVAL;
	if (sps->bit_depth_luma_minus8 != sps->bit_depth_chroma_minus8)
		/* Luma and chroma bit depth mismatch */
		return -EINVAL;
	if (sps->bit_depth_luma_minus8 != 0 && sps->bit_depth_luma_minus8 != 2)
		/* Only 8 and 10 bit have capture formats */
		return -EINVAL;
	if (sps->chroma_format_idc == 2 && sps->bit_depth_luma_minus8 == 2)
		/* 4:2:2 10 bit would need P210; P010 is too small for its chroma */
		return -EINVAL;
	if (!(sps->flags & V4L2_H264_SPS_FLAG_FRAME_MBS_ONLY))
		/* no interlaced support */
		return -EINVAL;

	return 0;
}

static void avd_h264_free_picture(struct avd_ctx *ctx,
				  struct avd_h264_ctx *h264_ctx)
{
	struct avd_dev *dev = ctx->dev;

	if (!h264_ctx)
		return;
	kvfree(h264_ctx->slices);
	avd_buf_free(dev, &h264_ctx->bufs.inst);
	avd_buf_free(dev, &h264_ctx->bufs.pipe_state);
	avd_buf_free(dev, &h264_ctx->bufs.above_info);
	avd_buf_free(dev, &h264_ctx->bufs.lf_above_info);
	avd_buf_free(dev, &h264_ctx->bufs.lf_above);
	avd_buf_free(dev, &h264_ctx->bufs.ip_above);
	avd_buf_free(dev, &h264_ctx->bufs.mv_above_info);
	kfree(h264_ctx);
}

static void avd_h264_stop(struct avd_ctx *ctx)
{
	struct avd_h264_ctx *h264_ctx = ctx->priv;
	unsigned int i;

	if (!h264_ctx)
		return;
	/* Core drains every frame before stop; no slot can still be DMA-owned. */
	for (i = 0; i < AVD_MAX_FRAMES; i++)
		avd_h264_free_picture(ctx, h264_ctx->slots[i]);
	avd_h264_free_picture(ctx, h264_ctx);
	ctx->priv = NULL;
}

static int avd_h264_start(struct avd_ctx *ctx)
{
	struct avd_h264_ctx *h264_ctx;
	struct v4l2_ctrl *ctrl;
	int ret;

	ctrl = v4l2_ctrl_find(&ctx->ctrl_hdl, V4L2_CID_STATELESS_H264_SPS);
	if (!ctrl)
		return -EINVAL;

	ret = avd_h264_validate_sps(ctx, ctrl->p_new.p_h264_sps);
	if (ret)
		return ret;

	h264_ctx = kzalloc(sizeof(*h264_ctx), GFP_KERNEL);
	if (!h264_ctx)
		return -ENOMEM;

	ctx->priv = h264_ctx;

	/* Scratch is allocated lazily and stays owned until picture retire. */
	ctx->max_frames = AVD_MAX_FRAMES;
	return 0;
}

static void avd_h264_run_preamble(struct avd_ctx *ctx, struct avd_h264_run *run)
{
	struct v4l2_ctrl *ctrl;

	avd_run_preamble(ctx, &run->base);
	run->picture = ctx->priv;

	ctrl = v4l2_ctrl_find(&ctx->ctrl_hdl,
			      V4L2_CID_STATELESS_H264_DECODE_PARAMS);
	run->decode_params = ctrl ? ctrl->p_cur.p : NULL;
	ctrl = v4l2_ctrl_find(&ctx->ctrl_hdl, V4L2_CID_STATELESS_H264_SPS);
	run->sps = ctrl ? ctrl->p_cur.p : NULL;
	ctrl = v4l2_ctrl_find(&ctx->ctrl_hdl, V4L2_CID_STATELESS_H264_PPS);
	run->pps = ctrl ? ctrl->p_cur.p : NULL;
	ctrl = v4l2_ctrl_find(&ctx->ctrl_hdl,
			      V4L2_CID_STATELESS_H264_SLICE_PARAMS);
	run->slice_params = ctrl ? ctrl->p_cur.p : NULL;
	ctrl = v4l2_ctrl_find(&ctx->ctrl_hdl,
			      V4L2_CID_STATELESS_H264_SCALING_MATRIX);
	run->scaling_matrix = ctrl ? ctrl->p_cur.p : NULL;
	ctrl = v4l2_ctrl_find(&ctx->ctrl_hdl,
			      V4L2_CID_STATELESS_H264_PRED_WEIGHTS);
	run->pred_weights = ctrl ? ctrl->p_cur.p : NULL;
}

/*
 * Check that every reference in the slice's ref lists points at a DPB entry
 * that is VALID. stream_refs() only pushes a reference header (and its RVRA
 * addresses) for VALID entries and the slice command indexes those by DPB
 * index, so a reference to any other entry makes the VP dereference an
 * uninitialised reference slot: that ends in a DART translation fault, a
 * frame that never completes and a watchdog reset of the VP, which also
 * disturbs the other contexts. It happens whenever a client starts decoding
 * on a non-IDR frame (joining a live RTSP stream mid-GOP): ffmpeg's
 * v4l2request hwaccel leaves the index of a missing reference at 0 and the
 * DPB is empty. Refuse such slices before anything is pushed; the client gets
 * the capture buffer back with V4L2_BUF_FLAG_ERROR, as for corrupt input.
 */
static bool avd_h264_refs_valid(const struct avd_h264_run *run)
{
	const struct v4l2_ctrl_h264_slice_params *sl = run->slice_params;
	const struct v4l2_h264_dpb_entry *dpb = run->decode_params->dpb;
	const struct v4l2_h264_reference *lists[2] = {
		sl->ref_pic_list0, sl->ref_pic_list1
	};
	u32 num[2] = {
		sl->num_ref_idx_l0_active_minus1 + 1,
		sl->num_ref_idx_l1_active_minus1 + 1
	};
	int nlists;

	if (sl->slice_type == V4L2_H264_SLICE_TYPE_P)
		nlists = 1;
	else if (sl->slice_type == V4L2_H264_SLICE_TYPE_B)
		nlists = 2;
	else
		return true;

	for (int l = 0; l < nlists; l++) {
		if (num[l] > V4L2_H264_REF_LIST_LEN)
			return false;
		for (u32 i = 0; i < num[l]; i++) {
			u8 idx = lists[l][i].index;

			if (idx >= V4L2_H264_NUM_DPB_ENTRIES ||
			    !(dpb[idx].flags & V4L2_H264_DPB_ENTRY_FLAG_VALID))
				return false;
		}
	}

	return true;
}

/*
 * Every slice codes at least one macroblock, so a picture has no more slices
 * than macroblocks, counting both fields of a frame.
 */
static size_t avd_h264_max_slices(const struct v4l2_ctrl_h264_sps *sps)
{
	size_t mbs = (size_t)(sps->pic_width_in_mbs_minus1 + 1) *
		     (sps->pic_height_in_map_units_minus1 + 1);

	if (!(sps->flags & V4L2_H264_SPS_FLAG_FRAME_MBS_ONLY))
		mbs *= 2;

	return min_t(size_t, mbs, MAX_SLICES);
}

/*
 * Coded data held for one picture. A picture of PCM macroblocks is about the
 * size of the decoded one and emulation prevention adds at most one byte for
 * every two, so twice the decoded buffer is enough; MAX_SLICES pages leave
 * room for small pictures cut into many slices.
 */
static size_t avd_h264_slice_budget(size_t decoded_size)
{
	return max_t(size_t, 2 * decoded_size, MAX_SLICES * PAGE_SIZE);
}

/* Check that one more slice of len bytes fits in the current picture. */
VISIBLE_IF_KUNIT bool avd_h264_slice_fits(const struct v4l2_ctrl_h264_sps *sps,
					  size_t held, size_t held_bytes,
					  u32 len, size_t decoded_size)
{
	return held < avd_h264_max_slices(sps) &&
	       PAGE_ALIGN((size_t)len) <= avd_h264_slice_budget(decoded_size) &&
	       held_bytes <= avd_h264_slice_budget(decoded_size) - PAGE_ALIGN((size_t)len);
}

/* Reset metadata only: OUTPUT memory belongs to the queue, never the codec. */
static void avd_h264_release_picture(struct avd_h264_ctx *h264,
				     struct avd_h264_ctx *picture)
{
	if (h264->pending == picture)
		h264->pending = NULL;
	if (picture->dst)
		vb2_to_avd_decoded_buf(&picture->dst->vb2_buf)->bank = 0;
	picture->slice_num = 0;
	picture->slice_bytes = 0;
	picture->dst = NULL;
	picture->busy = false;
}

static int avd_h264_collect(struct avd_ctx *ctx, struct avd_h264_run *run)
{
	struct avd_h264_ctx *h264 = ctx->priv, *picture = h264->pending;
	struct vb2_v4l2_buffer *src = run->base.bufs.src;
	struct vb2_buffer *vb = &src->vb2_buf;
	const struct v4l2_ctrl_h264_slice_params *sl = run->slice_params;
	struct avd_h264_slice *slice, *slices;
	const u8 *data = vb2_plane_vaddr(vb, 0);
	size_t payload = vb2_get_plane_payload(vb, 0);
	size_t offset = vb->planes[0].data_offset;
	size_t capacity, total_mbs;
	unsigned int i;
	u32 data_offset;
	int ret;

	/* Attach existing state even on validation failure, for error retirement. */
	run->picture = picture;
	if (!run->sps || !run->pps || !run->decode_params || !sl ||
	    !run->scaling_matrix || !run->pred_weights)
		return -EINVAL;
	ret = avd_h264_validate_sps(ctx, run->sps);
	if (!ret)
		ret = avd_h264_validate_pps(ctx, run->pps);
	if (ret || !avd_h264_refs_valid(run))
		return ret ?: -EINVAL;
	total_mbs = (size_t)(run->sps->pic_width_in_mbs_minus1 + 1) *
		    (run->sps->pic_height_in_map_units_minus1 + 1);
	if (sl->first_mb_in_slice >= total_mbs ||
	    sl->slice_type > V4L2_H264_SLICE_TYPE_I ||
	    run->pred_weights->luma_log2_weight_denom > 7 ||
	    run->pred_weights->chroma_log2_weight_denom > 7 ||
	    (run->sps->pic_width_in_mbs_minus1 + 1) * 16 != ALIGN(fmt_width(ctx), 16) ||
	    (run->sps->pic_height_in_map_units_minus1 + 1) * 16 != ALIGN(fmt_height(ctx), 16))
		return -EINVAL;
	if (!data || offset > payload || payload > vb2_plane_size(vb, 0) ||
	    payload - offset > U32_MAX || payload - offset < 3 ||
	    sl->header_bit_size > (payload - offset) * 8 ||
	    !avd_h264_find_slice_data(data + offset, payload - offset,
				    avd_h264_header_len(run), &data_offset))
		return -EINVAL;
	if (!sl->first_mb_in_slice) {
		/* A new picture cannot silently reuse a partially collected picture. */
		if (picture)
			return -EINVAL;
		for (i = 0; i < AVD_MAX_FRAMES; i++) {
			picture = h264->slots[i];
			if (!picture) {
				picture = kzalloc(sizeof(*picture), GFP_KERNEL);
				if (!picture)
					return -ENOMEM;
				ret = avd_h264_alloc_bufs(ctx, picture);
				if (ret) {
					avd_h264_free_picture(ctx, picture);
					return ret;
				}
				h264->slots[i] = picture;
			}
			if (!picture->busy)
				break;
		}
		if (i == AVD_MAX_FRAMES)
			return -EBUSY;
		picture->busy = true;
		picture->dst = run->base.bufs.dst;
		picture->sps = *run->sps;
		picture->pps = *run->pps;
		picture->decode = *run->decode_params;
		picture->scaling = *run->scaling_matrix;
		picture->monochrome = run->sps->chroma_format_idc == 0;
		h264->pending = picture;
		run->picture = picture;
		vb2_to_avd_decoded_buf(&picture->dst->vb2_buf)->bank = i + 1;
	} else if (!picture || picture->dst != run->base.bufs.dst ||
		   !picture->slice_num ||
		   run->decode_params->frame_num != picture->decode.frame_num ||
		   run->decode_params->top_field_order_cnt != picture->decode.top_field_order_cnt ||
		   run->decode_params->bottom_field_order_cnt !=
			picture->decode.bottom_field_order_cnt ||
		   memcmp(run->decode_params->dpb, picture->decode.dpb,
			  sizeof(picture->decode.dpb)) ||
		   memcmp(run->sps, &picture->sps, sizeof(picture->sps)) ||
		   memcmp(run->pps, &picture->pps, sizeof(picture->pps)) ||
		   memcmp(run->scaling_matrix, &picture->scaling, sizeof(picture->scaling))) {
		return -EINVAL;
	}
	if (!avd_h264_slice_fits(run->sps, picture->slice_num,
				 picture->slice_bytes, payload - offset,
				 ctx->decoded_fmt.fmt.pix_mp.plane_fmt[0].sizeimage))
		return -E2BIG;
	if (picture->slice_num == picture->alloc_slices) {
		capacity = min_t(size_t, avd_h264_max_slices(run->sps),
				 max_t(size_t, 4, picture->alloc_slices * 2));
		slices = kvmalloc_array(capacity, sizeof(*slices), GFP_KERNEL);
		if (!slices)
			return -ENOMEM;
		if (picture->slice_num)
			memcpy(slices, picture->slices,
			       picture->slice_num * sizeof(*slices));
		kvfree(picture->slices);
		picture->slices = slices;
		picture->alloc_slices = capacity;
	}
	slice = &picture->slices[picture->slice_num++];
	slice->data = (struct avd_buf) {
		.addr = vb2_dma_contig_plane_dma_addr(vb, 0) + offset,
		.size = payload - offset,
	};
	slice->params = *sl;
	slice->weights = *run->pred_weights;
	slice->data_offset = data_offset;
	picture->slice_bytes += PAGE_ALIGN(payload - offset);
	return 0;
}

static int avd_h264_build_picture(struct avd_ctx *ctx, struct avd_h264_run *run)
{
	struct avd_h264_ctx *picture = run->picture;
	struct avd_frame *f = run->frame;
	struct vb2_buffer *dst = &picture->dst->vb2_buf;
	struct v4l2_h264_reflist_builder builder;
	size_t i, mv_len = mv_color_size(fmt_width(ctx), fmt_height(ctx));
	int ret;

	if (mv_len > vb2_plane_size(dst, 0))
		return -EINVAL;
	run->sps = &picture->sps;
	run->pps = &picture->pps;
	run->decode_params = &picture->decode;
	run->scaling_matrix = &picture->scaling;
	run->base.y_out = vb2_dma_contig_plane_dma_addr(dst, 0);
	run->base.uv_out = run->base.y_out +
		(size_t)ctx->decoded_fmt.fmt.pix_mp.plane_fmt[0].bytesperline *
		ctx->decoded_fmt.fmt.pix_mp.height;
	run->base.comp_out = run->base.y_out + ctx->comp.start_offset;
	run->addresses.mv_color = run->base.y_out + vb2_plane_size(dst, 0) - mv_len;
	vb2_to_avd_decoded_buf(dst)->comp = ctx->comp;
	v4l2_h264_init_reflist_builder(&builder, run->decode_params, run->sps,
				       run->decode_params->dpb);
	run->num_valid = builder.num_valid;
	run->cur_poc = builder.cur_pic_order_count;
	if (f) {
		f->frame_num = picture->decode.frame_num;
		f->fifo_iova = picture->bufs.inst.addr;
		if (run->fp_cursor.buf) {
			ret = avd_h264_fp_slice(&run->fp_cursor, 0, picture->slice_num);
			if (ret)
				return ret;
		}
		avd_h264_emit(ctx, run, AVD_OP_EXEC |
			AVD_OP_EXEC_FLAG_START_REV4(ctx->dev->variant->revision == 4) |
			AVD_OP_EXEC_FLAG_START_REV3(ctx->dev->variant->revision == 3) |
			AVD_OP_EXEC_FIFO_IDX(f->idx), AVD_H264_FP_NONE);
	} else {
		ret = avd_init_job(ctx, AVD_CODEC_H264, picture->slice_num + 1);
		if (ret)
			return ret;
	}
	ctx->decomp = true;
	stream_hdr(ctx, run);
	for (i = 0; i < picture->slice_num; i++) {
		struct avd_h264_slice *slice = &picture->slices[i];

		picture->active_slice = &slice->data;
		run->slice_params = &slice->params;
		run->pred_weights = &slice->weights;
		run->slice_data_off = slice->data_offset;
		if (f && run->fp_cursor.buf) {
			ret = avd_h264_fp_slice(&run->fp_cursor, i, picture->slice_num);
			if (ret)
				return ret;
		}
		if (!f)
			ctx->job.num++;
		stream_slice(ctx, run);
		if (f)
			avd_h264_emit(ctx, run, AVD_OP_EXEC |
				AVD_OP_EXEC_FLAG_END(i == picture->slice_num - 1),
				AVD_H264_FP_NONE);
		if (run->error)
			return run->error;
	}
	return 0;
}

static int avd_h264_run(struct avd_ctx *ctx)
{
	struct avd_h264_ctx *h264 = ctx->priv;
	struct avd_h264_run run = {};
	int ret;

	avd_h264_run_preamble(ctx, &run);
	ret = avd_h264_collect(ctx, &run);
	avd_run_postamble(ctx, &run.base);
	if (ret)
		goto fail;
	if (run.base.bufs.src->flags & V4L2_BUF_FLAG_M2M_HOLD_CAPTURE_BUF) {
		avd_job_finish(ctx, VB2_BUF_STATE_DONE);
		return 0;
	}
	h264->pending = NULL;
	ret = avd_h264_build_picture(ctx, &run);
	if (ret)
		goto fail;
	ctx->job_fifo = run.picture->bufs.inst.addr;
	ret = avd_submit_job(ctx);
	if (!ret)
		return 0;
 fail:
	kvfree(ctx->job.segments);
	ctx->job.segments = NULL;
	if (run.picture)
		avd_h264_release_picture(h264, run.picture);
	return ret;
}

static void avd_h264_done(struct avd_ctx *ctx, struct vb2_v4l2_buffer *src_buf,
			  struct vb2_v4l2_buffer *dst_buf,
			  enum vb2_buffer_state result)
{
	struct avd_h264_ctx *h264 = ctx->priv, *picture;
	struct avd_decoded_buffer *dst;

	if (!dst_buf || ((src_buf->flags & V4L2_BUF_FLAG_M2M_HOLD_CAPTURE_BUF) &&
			 result == VB2_BUF_STATE_DONE))
		return;
	dst = vb2_to_avd_decoded_buf(&dst_buf->vb2_buf);
	if (!dst->bank || dst->bank > AVD_MAX_FRAMES)
		return;
	picture = h264->slots[dst->bank - 1];
	if (!picture || picture->dst != dst_buf)
		return;
	if (picture->monochrome && result == VB2_BUF_STATE_DONE) {
		const struct v4l2_pix_format_mplane *pix = &ctx->decoded_fmt.fmt.pix_mp;

		dst->grey_chroma_offset = (size_t)pix->plane_fmt[0].bytesperline * pix->height;
		dst->grey_chroma_10bit = ctx->image_fmt == AVD_IMG_FMT_420_10BIT;
	}
	dst->bank = 0;
	avd_h264_release_picture(h264, picture);
}

static enum avd_image_fmt avd_h264_get_image_fmt(struct avd_ctx *ctx,
						 struct v4l2_ctrl *ctrl)
{
	const struct v4l2_ctrl_h264_sps *sps = ctrl->p_new.p_h264_sps;

	if (ctrl->id != V4L2_CID_STATELESS_H264_SPS)
		return AVD_IMG_FMT_ANY;

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

static void avd_h264_adjust_decoded_fmt(struct avd_ctx *ctx,
					struct v4l2_pix_format_mplane *pix_mp)
{
	pix_mp->plane_fmt[0].sizeimage +=
		mv_color_size(pix_mp->width, pix_mp->height);
}

static int avd_h264_try_ctrl(struct avd_ctx *ctx, struct v4l2_ctrl *ctrl)
{
	if (ctrl->id == V4L2_CID_STATELESS_H264_SPS)
		return avd_h264_validate_sps(ctx, ctrl->p_new.p_h264_sps);
	if (ctrl->id == V4L2_CID_STATELESS_H264_PPS)
		return avd_h264_validate_pps(ctx, ctrl->p_new.p_h264_pps);

	return 0;
}

static void avd_h264_submit(struct avd_ctx *ctx)
{
	writel_relaxed(
		AVD_OP_EXEC |
			AVD_OP_EXEC_FLAG_START_REV4(
				ctx->dev->variant->revision == 4) |
			AVD_OP_EXEC_FLAG_START_REV3(
				ctx->dev->variant->revision == 3) |
			AVD_OP_EXEC_FIFO_IDX(ctx->fifo_idx) |
			AVD_OP_EXEC_FIFO_MASK(ctx->dev->variant->fifo_slots),
		ctx->dev->ctrl + ctx->dev->variant->submit_offset);
}

static void avd_h264_frame_controls(struct avd_ctx *ctx, struct avd_h264_run *run)
{
	struct v4l2_ctrl *ctrl;

#define H264_CTRL(member, id) do { \
	ctrl = v4l2_ctrl_find(&ctx->ctrl_hdl, id); \
	run->member = ctrl ? ctrl->p_cur.p : NULL; \
} while (0)
	H264_CTRL(decode_params, V4L2_CID_STATELESS_H264_DECODE_PARAMS);
	H264_CTRL(sps, V4L2_CID_STATELESS_H264_SPS);
	H264_CTRL(pps, V4L2_CID_STATELESS_H264_PPS);
	H264_CTRL(slice_params, V4L2_CID_STATELESS_H264_SLICE_PARAMS);
	H264_CTRL(scaling_matrix, V4L2_CID_STATELESS_H264_SCALING_MATRIX);
	H264_CTRL(pred_weights, V4L2_CID_STATELESS_H264_PRED_WEIGHTS);
#undef H264_CTRL
}

static int avd_h264_prepare(struct avd_ctx *ctx, struct avd_frame *f)
{
	struct avd_h264_ctx *h264 = ctx->priv;
	struct avd_h264_run run = { .frame = f };
	bool apple = avd_backend_is_apple(ctx->dev);
	int ret;

	f->dep = AVD_DEP_NONE;
	f->prog_words = 0;
	if (!h264 || f->idx >= AVD_MAX_FRAMES || !f->src || !f->dst ||
	    (!apple && (!f->prog || !f->prog->cpu)))
		return -EINVAL;
	f->codec_priv = h264->pending;
	avd_h264_frame_controls(ctx, &run);
	run.base.bufs.src = f->src;
	run.base.bufs.dst = f->dst;
	ret = avd_h264_collect(ctx, &run);
	f->codec_priv = run.picture;
	if (ret)
		return ret;
	if (f->src->flags & V4L2_BUF_FLAG_M2M_HOLD_CAPTURE_BUF)
		return 0;
	h264->pending = NULL;
	if (apple) {
		ret = avd_h264_fp_begin(&run.fp_cursor, f->fp,
				       ctx->dev->variant->h264_fp_layout);
		if (ret)
			return ret;
	} else {
		/* Native queued-open descriptor; Apple prefix remains core-owned. */
		f->cmd[0] = AVD_CODEC_H264;
	}
	return avd_h264_build_picture(ctx, &run);
}

static int avd_h264_pack_cmd(struct avd_ctx *ctx, struct avd_frame *f)
{
	return avd_h264_fp_pack_cmd(ctx->dev->variant->h264_fp_layout, f);
}

static void avd_h264_retire(struct avd_ctx *ctx, struct avd_frame *f,
			     enum vb2_buffer_state state)
{
	struct avd_h264_ctx *h264 = ctx->priv, *picture = f->codec_priv;

	if (!picture)
		return;
	if ((f->src->flags & V4L2_BUF_FLAG_M2M_HOLD_CAPTURE_BUF) &&
	    state == VB2_BUF_STATE_DONE)
		return;
	if (picture->dst == f->dst)
		avd_h264_done(ctx, f->src, f->dst, state);
	else
		avd_h264_release_picture(h264, picture);
	f->codec_priv = NULL;
}

const struct avd_codec_frame_ops avd_h264_frame_ops = {
	.prepare = avd_h264_prepare,
	.pack_cmd = avd_h264_pack_cmd,
	.retire = avd_h264_retire,
};

const struct avd_coded_fmt_ops avd_h264_fmt_ops = {
	.adjust_decoded_fmt = avd_h264_adjust_decoded_fmt,
	.start = avd_h264_start,
	.stop = avd_h264_stop,
	.done = avd_h264_done,
	.run = avd_h264_run,
	.submit = avd_h264_submit,
	.try_ctrl = avd_h264_try_ctrl,
	.get_image_fmt = avd_h264_get_image_fmt,
};
