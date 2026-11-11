// SPDX-License-Identifier: GPL-2.0-only
/*
 * Apple AVE video encoder: firmware commands
 *
 * Start and Process are large command images. Each is built in a zeroed
 * buffer: the header, the constant words (ave-image.c), then every field
 * that depends on the session or the frame.
 *
 * Copyright The Aurora Silicon Contributors
 */
#include <linux/device.h>
#include <linux/math64.h>
#include <linux/sizes.h>
#include <linux/string.h>
#include <linux/unaligned.h>

#include <media/videobuf2-v4l2.h>

#include "ave.h"
#include "ave-regs.h"

/*
 * IEEE 754 double for the positive value @num / @den (0 for 0), built with
 * integer arithmetic; the firmware takes rates and limits as doubles.
 */
static u64 ave_f64(u64 num, u64 den)
{
	u64 mant = 0, rem;
	int exp = 0;

	if (!num || !den)
		return 0;
	while (num >= den << 1) {
		den <<= 1;
		exp++;
	}
	while (num < den) {
		num <<= 1;
		exp--;
	}
	/* 1 <= num/den < 2: take 53 significant bits by long division. */
	rem = num;
	for (unsigned int i = 0; i < 53; i++) {
		mant <<= 1;
		if (rem >= den) {
			mant |= 1;
			rem -= den;
		}
		rem <<= 1;
	}
	return (u64)(exp + 1023) << 52 | (mant & GENMASK_ULL(51, 0));
}

#define AVE_F64_NEG BIT_ULL(63)

static void ave_put32(u8 *cmd, u32 off, u32 val)
{
	put_unaligned_le32(val, cmd + off);
}

static void ave_put16(u8 *cmd, u32 off, u16 val)
{
	put_unaligned_le16(val, cmd + off);
}

static void ave_put64(u8 *cmd, u32 off, u64 val)
{
	put_unaligned_le64(val, cmd + off);
}

void ave_write_surface(const struct ave_fw_layout *fw, u8 *cmd,
		       const struct ave_surface_layout *l, unsigned int index,
		       dma_addr_t addr, u32 size)
{
	u32 address_off = l->address + l->address_stride * index;
	u32 size_off;

	ave_put64(cmd, address_off, addr);
	if (l->size == AVE_OFFSET_ABSENT)
		return;
	size_off = l->size + l->size_stride * index;
	ave_put32(cmd, size_off, size);
	if (fw->abi->padded_surfaces)
		ave_put32(cmd, size_off + 4, 0);
}

static void ave_put_buf(const struct ave_fw_layout *fw, u8 *cmd,
			const struct ave_surface_layout *l, unsigned int index,
			const struct ave_buf *b, u32 size)
{
	ave_write_surface(fw, cmd, l, index, b->dma, size);
}

/* Is another session open in the firmware besides @ctx's? */
static bool ave_cmd_others_open(struct ave_ctx *ctx)
{
	struct ave_dev *ave = ctx->ave;

	for (unsigned int i = 0; i < AVE_MAX_SESSIONS; i++)
		if (ave->sessions[i] && ave->sessions[i] != ctx &&
		    ave->sessions[i]->sess->fw_open)
			return true;
	return false;
}

/*
 * Header word +0x2c depends on the set of open sessions: 0x1e0 on Open and
 * Start (0x140 when another session is open), the codec value on every
 * other command of a lone session, and 0 while another one is open.
 */
u32 ave_session_arg2(struct ave_ctx *ctx)
{
	if (ctx->ave->layout->counted && ctx->sess->p.codec == AVE_CODEC_H264)
		return ctx->sess->p.bframes ? AVE_SESSION_ARG2_H264 : 0x140;
	if (ave_cmd_others_open(ctx))
		return 0;
	return ctx->sess->p.codec == AVE_CODEC_HEVC ? AVE_SESSION_ARG2_HEVC :
						      AVE_SESSION_ARG2_H264;
}

u32 ave_session_setup_arg2(struct ave_ctx *ctx)
{
	return ave_cmd_others_open(ctx) ? AVE_SESSION_ARG2_SETUP_SHARED :
					  AVE_SESSION_ARG2_SETUP;
}

void ave_write_header(struct ave_ctx *ctx, u8 *cmd, enum ave_opcode op,
		      u32 echo, u32 arg0, u32 arg2)
{
	const struct ave_fw_abi *abi = ctx->ave->layout->abi;
	struct ave_command header = {
		.op = op,
		.codec = ctx->sess->p.codec,
		.client = ctx->sess->client,
		.subtype = echo,
		.unit = ctx->sess->unit,
		.arg0 = arg0,
		.arg1 = ctx->sess->arg1,
		.arg2 = arg2,
	};

	ctx->sess->token += AVE_TOKEN_STEP;
	header.token = ctx->sess->token;
	abi->write_header(cmd, abi, &header);
}

void ave_write_global(struct ave_dev *ave, u8 *cmd, enum ave_opcode op)
{
	const struct ave_fw_abi *abi = ave->layout->abi;
	struct ave_command header = {
		.op = op,
		.codec = AVE_CODEC_H264,
		.subtype = U32_MAX,
	};

	abi->write_header(cmd, abi, &header);
}

void ave_stamp_command(const struct ave_fw_abi *abi, u8 *cmd, u64 timeout,
		       u64 created)
{
	ave_put64(cmd, abi->timeout_offset, timeout);
	ave_put64(cmd, abi->created_offset, created);
}

/* Throughput modes: 3 codes for quality, 5 prioritizes speed (as macOS) */
#define AVE_THROUGHPUT_QUALITY 3
#define AVE_THROUGHPUT_SPEED 5

#define AVE_START_ARRAY_LEN 30
#define AVE_START_INIT_QP_VALUE 26

static void ave_write_address_bank(u8 *cmd,
				   const struct ave_address_bank_layout *l,
				   unsigned int index, dma_addr_t first,
				   dma_addr_t second)
{
	for (unsigned int bank = 0; bank < l->banks; bank++)
		for (unsigned int value = 0; value < l->values; value++)
			ave_put64(cmd, l->address + bank * l->bank_stride +
					 index * l->entry_stride + value * sizeof(u64),
				  value ? second : first);
}

static int ave_6070_check_buf(const struct ave_buf *b)
{
	return b->dma && IS_ALIGNED(b->dma, SZ_16K) ? 0 : -EINVAL;
}

static int ave_start_validate_6070(const struct ave_session *sess)
{
	const struct ave_params *p = &sess->p;
	bool hevc = p->codec == AVE_CODEC_HEVC;
	unsigned int tiles = hevc ? 4 : AVE_6070_AVC_ENTROPY_TILES;

	if (ave_6070_check_buf(&sess->hdr))
		return -EINVAL;
	if (ave_6070_check_buf(&sess->client_mem) ||
	    sess->client_mem.size != AVE_6070_CLIENT_MEM_BYTES)
		return -EINVAL;
	if (ave_6070_check_buf(&sess->param_sets) || !sess->param_sets.size)
		return -EINVAL;
	for (unsigned int i = 0; i < p->refs; i++)
		if (ave_6070_check_buf(&sess->ref_a[i]) ||
		    ave_6070_check_buf(&sess->ref_b[i]) ||
		    ave_6070_check_buf(&sess->ref_c[i]))
			return -EINVAL;
	for (unsigned int i = 0; i < AVE_6070_LOWRES_RESULTS; i++)
		if (ave_6070_check_buf(&sess->ref_d[i]))
			return -EINVAL;
	for (unsigned int slot = 0; slot < p->slots; slot++)
		if (ave_6070_check_buf(&sess->slot[slot].coded) ||
		    ave_6070_check_buf(&sess->slot[slot].meta))
			return -EINVAL;
	for (unsigned int set = 0; set < AVE_NEIGHBOUR_SETS; set++)
		if (ave_6070_check_buf(&sess->row_a[set]) ||
		    ave_6070_check_buf(&sess->row_b[set]) ||
		    ave_6070_check_buf(&sess->fixed[set]) ||
		    ave_6070_check_buf(&sess->neighbour_fw[set]))
			return -EINVAL;
	for (unsigned int set = 0; set < AVE_ENTROPY_SETS; set++)
		for (unsigned int tile = 0; tile < tiles; tile++)
			if (ave_6070_check_buf(&sess->tile[set][tile]) ||
			    ave_6070_check_buf(&sess->tile_aux[set][tile]))
				return -EINVAL;
	if (hevc)
		for (unsigned int i = 0; i < 2; i++)
			if (ave_6070_check_buf(&sess->hevc_extra[i]) ||
			    ave_6070_check_buf(&sess->hevc_transcoded[i]))
				return -EINVAL;
	return 0;
}

static int ave_start_surfaces_6070(struct ave_session *sess, u8 *cmd,
				   const struct ave_fw_layout *fw)
{
	const struct ave_params *p = &sess->p;
	const struct ave_start_layout *l = &fw->start[p->codec - 1];
	bool hevc = p->codec == AVE_CODEC_HEVC;
	unsigned int tiles = hevc ? 4 : AVE_6070_AVC_ENTROPY_TILES;
	struct ave_6070_avc_recon_layout recon =
		ave_6070_recon_layout(p->width, p->height, p->bit_depth);
	int ret = ave_start_validate_6070(sess);

	if (ret)
		return ret;

	ave_put_buf(fw, cmd, &l->header, 0, &sess->hdr, fw->hdr_bytes);
	ave_put_buf(fw, cmd, &l->secondary, 0, &sess->client_mem,
		    sess->client_mem.size);
	ave_put_buf(fw, cmd, &l->param_sets, 0, &sess->param_sets,
		    sess->param_sets.size);

	for (unsigned int i = 0; i < p->refs; i++) {
		ave_write_address_bank(cmd, &l->recon, i,
				       sess->ref_a[i].dma + recon.luma_data_offset,
				       sess->ref_a[i].dma + recon.luma_metadata_offset);
		ave_write_address_bank(cmd, &l->colocated, i,
				       sess->ref_b[i].dma, 0);
		ave_write_address_bank(cmd, &l->lfs, i, sess->ref_c[i].dma, 0);
	}
	for (unsigned int i = 0; i < AVE_6070_LOWRES_RESULTS; i++)
		ave_write_address_bank(cmd, &l->lfs_result, i,
				       sess->ref_d[i].dma, 0);

	for (unsigned int slot = 0; slot < p->slots; slot++) {
		ave_put_buf(fw, cmd, &l->coded, slot, &sess->slot[slot].coded,
			    p->coded_cap);
		ave_put_buf(fw, cmd, &l->meta, slot, &sess->slot[slot].meta,
			    sess->slot[slot].meta.size);
	}

	for (unsigned int set = 0; set < AVE_NEIGHBOUR_SETS; set++) {
		ave_put_buf(fw, cmd, &l->row_a, set, &sess->row_a[set],
			    sess->row_a[set].size);
		ave_put_buf(fw, cmd, &l->row_b, set, &sess->row_b[set],
			    sess->row_b[set].size);
		ave_put_buf(fw, cmd, &l->fixed, set, &sess->fixed[set],
			    sess->fixed[set].size);
		ave_put_buf(fw, cmd, &l->neighbour_fw, set,
			    &sess->neighbour_fw[set], sess->neighbour_fw[set].size);
	}

	for (unsigned int set = 0; set < l->entropy.sets; set++)
		for (unsigned int tile = 0; tile < tiles; tile++) {
			unsigned int main = 2 * tile, aux = main + 1;

			ave_put64(cmd, l->entropy.address +
					 main * l->entropy.address_entry_stride +
					 set * l->entropy.address_set_stride,
				  sess->tile[set][tile].dma);
			ave_put32(cmd, l->entropy.size +
					 main * l->entropy.size_entry_stride +
					 set * l->entropy.size_set_stride,
				  sess->tile[set][tile].size);
			ave_put64(cmd, l->entropy.address +
					 aux * l->entropy.address_entry_stride +
					 set * l->entropy.address_set_stride,
				  sess->tile_aux[set][tile].dma);
			ave_put32(cmd, l->entropy.size +
					 aux * l->entropy.size_entry_stride +
					 set * l->entropy.size_set_stride,
				  sess->tile_aux[set][tile].size);
		}

	if (hevc)
		for (unsigned int i = 0; i < 2; i++) {
			ave_put_buf(fw, cmd, &l->hevc_extra, i,
				    &sess->hevc_extra[i], sess->hevc_extra[i].size);
			ave_put_buf(fw, cmd, &l->hevc_transcoded, i,
				    &sess->hevc_transcoded[i],
				    sess->hevc_transcoded[i].size);
		}
	return 0;
}

static void ave_write_recon_selection(u8 *cmd,
				      const struct ave_recon_selection_layout *wire,
				      const struct ave_buf *buf,
				      const struct ave_6070_avc_recon_layout *layout)
{
	ave_put64(cmd, wire->luma_data, buf->dma + layout->luma_data_offset);
	ave_put64(cmd, wire->luma_metadata, buf->dma + layout->luma_metadata_offset);
	ave_put64(cmd, wire->chroma_data, buf->dma + layout->chroma_data_offset);
	ave_put64(cmd, wire->chroma_metadata,
		  buf->dma + layout->chroma_metadata_offset);
}

static int ave_start_surfaces(struct ave_session *sess, u8 *cmd,
			      const struct ave_fw_layout *fw)
{
	const struct ave_params *p = &sess->p;
	bool hevc = p->codec == AVE_CODEC_HEVC;
	unsigned int tiles = hevc ? 4 : AVE_TILE_BUFS;
	const struct ave_start_layout *l = &fw->start[p->codec - 1];

	if (fw->kind == AVE_LAYOUT_6070)
		return ave_start_surfaces_6070(sess, cmd, fw);

	/* Clear every descriptor the image may carry, then fill ours. */
	for (unsigned int i = 0; i < AVE_MAX_REFS; i++) {
		ave_write_surface(fw, cmd, &l->ref_a, i, 0, 0);
		ave_write_surface(fw, cmd, &l->ref_b, i, 0, 0);
		ave_write_surface(fw, cmd, &l->ref_c, i, 0, 0);
		ave_write_surface(fw, cmd, &l->ref_c_alias, i, 0, 0);
		ave_write_surface(fw, cmd, &l->ref_d, i, 0, 0);
	}
	for (unsigned int s = 0; s < AVE_START_ARRAY_LEN; s++) {
		ave_write_surface(fw, cmd, &l->coded, s, 0, 0);
		ave_write_surface(fw, cmd, &l->meta, s, 0, 0);
	}

	ave_put_buf(fw, cmd, &l->header, 0, &sess->hdr, fw->hdr_bytes);
	ave_write_surface(fw, cmd, &l->secondary, 0, 0, 0);
	for (unsigned int i = 0; i < p->refs; i++) {
		ave_put_buf(fw, cmd, &l->ref_a, i, &sess->ref_a[i],
			    sess->ref_a[i].size);
		ave_put_buf(fw, cmd, &l->ref_b, i, &sess->ref_b[i],
			    sess->ref_b[i].size);
		ave_put_buf(fw, cmd, &l->ref_c, i, &sess->ref_c[i],
			    sess->ref_c[i].size);
		ave_write_surface(fw, cmd, &l->ref_c_alias, i,
				  sess->ref_c[i].dma, 0);
		ave_put_buf(fw, cmd, &l->ref_d, i, &sess->ref_d[i],
			    sess->ref_d[i].size);
	}
	if (fw->counted) {
		ave_put32(cmd, l->refs_count, p->refs);
		ave_put32(cmd, l->work_count, p->refs);
		ave_put32(cmd, l->lfs_count, p->refs);
		ave_put32(cmd, l->slots_count, p->slots);
		ave_put32(cmd, l->meta_count, p->slots);
		ave_put32(cmd, l->mb_count, p->rc_mode == 3 ? 3 : 1);
		if (p->rc_mode == 3)
			for (unsigned int i = 0; i < 2; i++)
				ave_put_buf(fw, cmd, &l->mb, i + 1,
					    &sess->mb_extra[i],
					    sess->mb_extra[i].size);
		ave_put32(cmd, l->tiles_count, tiles);
		for (unsigned int i = 0; i < p->refs; i++) {
			ave_put_buf(fw, cmd, &l->ref_e, i, &sess->ref_e[i],
				    sess->ref_e[i].size);
			ave_put_buf(fw, cmd, &l->ref_f, i, &sess->ref_f[i],
				    sess->ref_f[i].size);
		}
	}
	ave_put_buf(fw, cmd, &l->mb, 0, &sess->mb, sess->mb.size);
	for (unsigned int s = 0; s < p->slots; s++) {
		ave_put_buf(fw, cmd, &l->coded, s, &sess->slot[s].coded,
			    p->coded_cap);
		ave_put_buf(fw, cmd, &l->meta, s, &sess->slot[s].meta,
			    sess->slot[s].meta.size);
	}
	ave_put_buf(fw, cmd, &l->row_a, 0, &sess->row_a[0], sess->row_a[0].size);
	ave_put_buf(fw, cmd, &l->row_b, 0, &sess->row_b[0], sess->row_b[0].size);
	ave_put_buf(fw, cmd, &l->fixed, 0, &sess->fixed[0], SZ_16K);
	for (unsigned int i = 0; i < AVE_TILE_BUFS; i++) {
		if (i < tiles) {
			ave_put_buf(fw, cmd, &l->tiles[i], 0, &sess->tile[0][i],
				    sess->tile[0][i].size);
			ave_put_buf(fw, cmd, &l->tiles[i], 1,
				    &sess->tile_aux[0][i], sess->tile_aux[0][i].size);
		} else {
			ave_write_surface(fw, cmd, &l->tiles[i], 0, 0, 0);
			ave_write_surface(fw, cmd, &l->tiles[i], 1, 0, 0);
		}
	}
	if (hevc)
		for (unsigned int i = 0; i < 2; i++)
			ave_put_buf(fw, cmd, &l->hevc_extra, i,
				    &sess->hevc_extra[i],
				    sess->hevc_extra[i].size);
	return 0;
}

static void ave_start_put32(u8 *cmd, u32 off, u32 val)
{
	if (off)
		ave_put32(cmd, off, val);
}

static void ave_start_put16(u8 *cmd, u32 off, u16 val)
{
	if (off)
		ave_put16(cmd, off, val);
}

static void ave_start_put64(u8 *cmd, u32 off, u64 val)
{
	if (off)
		ave_put64(cmd, off, val);
}

static void ave_start_put8(u8 *cmd, u32 off, u8 val)
{
	if (off)
		cmd[off] = val;
}

/* 6070 Process state copied from the session's codec object. */
#define AVE_6070_AVC_STATE_BYTES	0x984
#define AVE_6070_HEVC_SLICE_BYTES	0x5570
#define AVE_6070_HEVC_STATE_BYTES	0x5884

/*
 * Per-picture fields of the retained AVC slice object, named after the
 * H.264 slice-header syntax they carry (7.3.3).
 */
#define AVE_6070_AVC_NAL_REF_IDC	0x004
#define AVE_6070_AVC_NAL_UNIT_TYPE	0x008
#define AVE_6070_AVC_SLICE_TYPE		0x010
#define AVE_6070_AVC_FRAME_NUM		0x01c
#define AVE_6070_AVC_IDR_PIC		0x022	/* u8: IDR picture */
#define AVE_6070_AVC_IDR_PIC_ID		0x024
#define AVE_6070_AVC_POC_LSB		0x028
#define AVE_6070_AVC_DIRECT_SPATIAL	0x03c	/* u8: direct_spatial_mv_pred_flag */
#define AVE_6070_AVC_REF_IDX_OVERRIDE	0x040	/* num_ref_idx_active_override_flag */
#define AVE_6070_AVC_L0_ACTIVE_MINUS1	0x044
#define AVE_6070_AVC_L1_ACTIVE_MINUS1	0x048
#define AVE_6070_AVC_SLICE_QP_DELTA	0x050
/* Reference marking and list-modification storage, cleared per picture. */
#define AVE_6070_AVC_REF_SYNTAX		0x074
#define AVE_6070_AVC_REF_SYNTAX_BYTES	0x500

/*
 * The 6070 reordered form is IbP: one B between two P pictures, which
 * keeps two references (the past and the next P) and reorders one picture.
 */
#define AVE_6070_REORDER_DEPTH		1
#define AVE_6070_REORDER_REFS		2
/* log2_max_mv_length_{horizontal,vertical} of the reordered stream (E.2.1). */
#define AVE_H264_VUI_LOG2_MAX_MV	15

/* H.264 Table 7-1 NAL unit types and Table 7-6 slice types. */
#define AVE_H264_NAL_SLICE		1
#define AVE_H264_NAL_IDR_SLICE		5
#define AVE_H264_SLICE_P		0
#define AVE_H264_SLICE_B		1
#define AVE_H264_SLICE_I		2

/*
 * Start sets log2_max_frame_num_minus4 = 0 and
 * log2_max_pic_order_cnt_lsb_minus4 = 4 for the 6070.
 */
#define AVE_6070_AVC_FRAME_NUM_MASK	GENMASK(3, 0)
#define AVE_6070_POC_LSB_MASK		GENMASK(7, 0)

/* Retained HEVC slice-segment fields (H.265 7.3.6.1). */
#define AVE_6070_HEVC_POC_LSB		0x028
#define AVE_6070_HEVC_SAO_LUMA		0x11d	/* u8: slice_sao_luma_flag */
#define AVE_6070_HEVC_SAO_CHROMA	0x11e	/* u8: slice_sao_chroma_flag */
/* Second retained partition: the slice's short-term RPS selection. */
#define AVE_6070_HEVC_ST_RPS_SPS_FLAG	0x000	/* u8: short_term_ref_pic_set_sps_flag */
#define AVE_6070_HEVC_ST_RPS_IDX	0x004	/* short_term_ref_pic_set_idx */

/*
 * Start short-term RPS list: a u32 count, then one entry per set holding
 * the H.265 7.3.7 syntax followed by its 7.4.8 derived values. Arrays have
 * one element per reference; only element 0 is used here.
 */
#define AVE_6070_HEVC_RPS_LIST_HEADER	4
#define AVE_6070_HEVC_RPS_STRIDE	0x164
#define AVE_6070_HEVC_RPS_NUM_NEG	0x030	/* num_negative_pics */
#define AVE_6070_HEVC_RPS_NUM_POS	0x034	/* num_positive_pics */
#define AVE_6070_HEVC_RPS_S0_MINUS1	0x038	/* u16 delta_poc_s0_minus1[] */
#define AVE_6070_HEVC_RPS_S0_USED	0x058	/* u8 used_by_curr_pic_s0_flag[] */
#define AVE_6070_HEVC_RPS_S1_MINUS1	0x068	/* delta_poc_s1_minus1[] */
#define AVE_6070_HEVC_RPS_S1_USED	0x0a8	/* u8 used_by_curr_pic_s1_flag[] */
#define AVE_6070_HEVC_RPS_NUM_NEG_D	0x0b8	/* NumNegativePics */
#define AVE_6070_HEVC_RPS_NUM_POS_D	0x0bc	/* NumPositivePics */
#define AVE_6070_HEVC_RPS_S0_USED_D	0x0c0	/* u8 UsedByCurrPicS0[] */
#define AVE_6070_HEVC_RPS_S1_USED_D	0x0d0	/* u8 UsedByCurrPicS1[] */
#define AVE_6070_HEVC_RPS_S0_DELTA	0x0e0	/* s32 DeltaPocS0[] */
#define AVE_6070_HEVC_RPS_S1_DELTA	0x120	/* s32 DeltaPocS1[] */
#define AVE_6070_HEVC_RPS_NUM_DELTA	0x160	/* NumDeltaPocs */
/* Low delay: four one-reference sets; one B: alternating P {-2} / B {-1, +1}. */
#define AVE_6070_HEVC_RPS_SETS_LOW_DELAY	4
#define AVE_6070_HEVC_RPS_SETS_REORDER	8

/* HEVC Start constants (H.265 A.3 and 7.4.3.2). */
#define AVE_6070_HEVC_INPUT_420_8BIT	16	/* firmware input-format code: 4:2:0 Main */
#define AVE_6070_HEVC_SAO_ALL		GENMASK(15, 0)	/* SAO on for every CTB class */

enum ave_6070_picture_type {
	AVE_6070_PICTURE_I = 0,
	AVE_6070_PICTURE_P = 1,
	AVE_6070_PICTURE_B = 2,
	AVE_6070_PICTURE_IDR = 3,
};

struct ave_6070_picture {
	u32 type;
	u32 frame_num;
	u32 poc_lsb;
	u32 current_ref;
	u32 list0;
	u32 list0_second;
	u32 list1;
};

static struct ave_6070_picture
ave_6070_picture(const struct ave_params *p, u32 ordinal, bool force_key,
		 bool last)
{
	struct ave_6070_picture pic = {
		.current_ref = ordinal % p->refs,
		.list0 = U32_MAX,
		.list0_second = U32_MAX,
		.list1 = U32_MAX,
	};
	u32 period = max(p->gop, 1U);
	u32 pos = ordinal % period;
	u32 back;

	if (!ordinal || force_key || !pos) {
		pic.type = AVE_6070_PICTURE_IDR;
		return pic;
	}

	if (p->bframes && (pos & 1) && !last && pos + 1 < period) {
		pic.type = AVE_6070_PICTURE_B;
		pic.frame_num = (pos + 3) / 2;
		pic.poc_lsb = pos;
		pic.list0 = (ordinal - 1) % p->refs;
		pic.list1 = (ordinal + 1) % p->refs;
		return pic;
	}

	pic.type = AVE_6070_PICTURE_P;
	if (!p->bframes) {
		pic.frame_num = pos;
		pic.poc_lsb = 2 * pos;
		pic.list0 = (ordinal - 1) % p->refs;
		return pic;
	}

	back = pos & 1 ? 1 : 2;
	pic.frame_num = DIV_ROUND_UP(pos, 2);
	pic.poc_lsb = pos;
	pic.list0 = (ordinal - back) % p->refs;
	if (pos >= back + 2)
		pic.list0_second = (ordinal - back - 2) % p->refs;
	return pic;
}

static void ave_6070_avc_slice(u8 *state, const struct ave_params *p,
			       const struct ave_6070_picture *pic, u32 ordinal)
{
	u32 qp = p->rc_mode == AVE_RC_CQ ? p->initial_qp : AVE_START_INIT_QP_VALUE;
	bool idr = pic->type == AVE_6070_PICTURE_IDR;
	bool b = pic->type == AVE_6070_PICTURE_B;

	/* Rebuild every field whose syntax depends on the current picture. */
	memset(state + AVE_6070_AVC_REF_SYNTAX, 0, AVE_6070_AVC_REF_SYNTAX_BYTES);
	/* B pictures are not used for reference. */
	ave_put32(state, AVE_6070_AVC_NAL_REF_IDC, !b);
	ave_put32(state, AVE_6070_AVC_NAL_UNIT_TYPE,
		  idr ? AVE_H264_NAL_IDR_SLICE : AVE_H264_NAL_SLICE);
	ave_put32(state, AVE_6070_AVC_SLICE_TYPE,
		  b ? AVE_H264_SLICE_B :
		  pic->type == AVE_6070_PICTURE_P ? AVE_H264_SLICE_P : AVE_H264_SLICE_I);
	ave_put32(state, AVE_6070_AVC_FRAME_NUM,
		  pic->frame_num & AVE_6070_AVC_FRAME_NUM_MASK);
	state[AVE_6070_AVC_IDR_PIC] = idr;
	/* Consecutive IDR pictures must differ in idr_pic_id. */
	ave_put32(state, AVE_6070_AVC_IDR_PIC_ID,
		  1 ^ ((ordinal / max(p->gop, 1U)) & 1));
	ave_put32(state, AVE_6070_AVC_POC_LSB,
		  pic->poc_lsb & AVE_6070_POC_LSB_MASK);
	state[AVE_6070_AVC_DIRECT_SPATIAL] = 1;
	ave_put32(state, AVE_6070_AVC_REF_IDX_OVERRIDE,
		  pic->type == AVE_6070_PICTURE_P || b);
	ave_put32(state, AVE_6070_AVC_L0_ACTIVE_MINUS1,
		  pic->list0_second == U32_MAX ? 0 : 1);
	ave_put32(state, AVE_6070_AVC_L1_ACTIVE_MINUS1, 0);
	/* The firmware takes the slice QP relative to the H.264 default. */
	ave_put32(state, AVE_6070_AVC_SLICE_QP_DELTA, qp - AVE_START_INIT_QP_VALUE);
}

static void ave_6070_hevc_rps(u8 *cmd, const struct ave_start_layout *l,
			      bool bframes)
{
	u32 sets = bframes ? AVE_6070_HEVC_RPS_SETS_REORDER :
			     AVE_6070_HEVC_RPS_SETS_LOW_DELAY;

	ave_put32(cmd, l->hevc_rps, sets);
	for (u32 i = 0; i < sets; i++) {
		u32 entry = l->hevc_rps + AVE_6070_HEVC_RPS_LIST_HEADER +
			    i * AVE_6070_HEVC_RPS_STRIDE;
		/* Odd sets of a reordered session describe the B picture. */
		bool b_set = bframes && (i & 1);
		/* One past picture: POC -1 (low delay, B) or -2 (P after a B). */
		u32 negative = bframes && !b_set ? 2 : 1;
		/* The B picture also references the next P: POC +1. */
		u32 positive = b_set ? 1 : 0;

		ave_put32(cmd, entry + AVE_6070_HEVC_RPS_NUM_NEG, 1);
		ave_put32(cmd, entry + AVE_6070_HEVC_RPS_NUM_POS, positive);
		ave_put16(cmd, entry + AVE_6070_HEVC_RPS_S0_MINUS1, negative - 1);
		cmd[entry + AVE_6070_HEVC_RPS_S0_USED] = 1;
		ave_put32(cmd, entry + AVE_6070_HEVC_RPS_NUM_NEG_D, 1);
		ave_put32(cmd, entry + AVE_6070_HEVC_RPS_NUM_POS_D, positive);
		cmd[entry + AVE_6070_HEVC_RPS_S0_USED_D] = 1;
		ave_put32(cmd, entry + AVE_6070_HEVC_RPS_S0_DELTA, -negative);
		ave_put32(cmd, entry + AVE_6070_HEVC_RPS_NUM_DELTA, 1 + positive);
		if (!positive)
			continue;
		ave_put32(cmd, entry + AVE_6070_HEVC_RPS_S1_MINUS1, 0);
		cmd[entry + AVE_6070_HEVC_RPS_S1_USED] = 1;
		cmd[entry + AVE_6070_HEVC_RPS_S1_USED_D] = 1;
		ave_put32(cmd, entry + AVE_6070_HEVC_RPS_S1_DELTA, 1);
	}
}

int ave_init_codec_state(struct ave_session *sess)
{
	u8 *state = sess->codec_state;

	if (!state)
		return -EINVAL;

	memset(state, 0, sess->codec_state_bytes);
	if (sess->p.codec == AVE_CODEC_H264) {
		if (sess->codec_state_bytes != AVE_6070_AVC_STATE_BYTES)
			return -EINVAL;

		/*
		 * The AVC object contains slice syntax, not SPS or PPS state. These
		 * are its first-picture defaults and retained control values.
		 */
		ave_put32(state, 0x000, AVE_6070_AVC_STATE_BYTES);
		ave_put32(state, AVE_6070_AVC_NAL_REF_IDC, 1);
		ave_put32(state, AVE_6070_AVC_SLICE_TYPE, AVE_H264_SLICE_I);
		ave_put32(state, AVE_6070_AVC_IDR_PIC_ID, 1);
		state[AVE_6070_AVC_DIRECT_SPATIAL] = 1;
		ave_put32(state, 0x980, U32_MAX); /* automatic variance */
		return 0;
	}

	if (sess->p.codec != AVE_CODEC_HEVC ||
	    sess->codec_state_bytes != AVE_6070_HEVC_STATE_BYTES)
		return -EINVAL;

	/*
	 * HEVC keeps a slice object followed by short- and long-term RPS
	 * tails. The first IDR uses no RPS; the session's VPS/SPS owns the
	 * reference count.  Header destinations are slot-specific and are
	 * added while building Process.
	 */
	ave_put32(state, 0x000, AVE_6070_HEVC_SLICE_BYTES);
	state[0x00c] = 1; /* first slice segment in picture */
	state[0x1aa] = 1; /* collocated picture comes from list 0 */
	ave_put32(state, 0x3d4, 3); /* five minus maximum merge candidates */
	ave_put32(state, 0x54c, U32_MAX); /* no slice-segment limit */
	ave_put32(state, 0x550, U32_MAX); /* no dependent-segment limit */
	return 0;
}

/*
 * general_level_idc is 30 times the level number (H.265 A.4). The firmware
 * level enum counts 1, 2, 2.1, 3, 3.1, 4, 4.1, 5 and 5.1 from one.
 */
static u32 ave_hevc_level_idc(u32 level)
{
	switch (level) {
	case 1:
		return 30;
	case 2:
		return 60;
	case 3:
		return 63;
	case 4:
		return 90;
	case 5:
		return 93;
	case 6:
		return 120;
	case 7:
		return 123;
	case 8:
		return 150;
	case 9:
		return 153;
	default:
		return 0;
	}
}

static void ave_start_config(const struct ave_params *p, u8 *cmd,
			     const struct ave_fw_layout *fw)
{
	static const u8 h264_level_idc[] = {
		10, 9, 11, 12, 13, 20, 21, 22, 30,
		31, 32, 40, 41, 42, 50, 51, 52,
	};
	bool hevc = p->codec == AVE_CODEC_HEVC;
	bool cq = p->rc_mode == 3;
	u32 rc_mode;
	s32 min_qp = p->min_qp;
	u32 max_qp = p->max_qp;
	u32 depth = p->bit_depth ?: 8;
	u32 crop_r = (p->cw - p->width) / 2, crop_b = (p->ch - p->height) / 2;
	bool crop = crop_r || crop_b;
	const struct ave_start_layout *l = &fw->start[p->codec - 1];

	ave_start_put32(cmd, l->codec, p->codec);
	ave_start_put32(
		cmd, l->flag_8bit,
		fw->counted ?
			0x10000 | (hevc ? (depth == 8 ? 0x41 : 0x40) : 0) |
				(cq ? 0x10 : 0) :
		hevc ? depth == 8 :
		       0);
	ave_start_put32(cmd, l->geometry_mode,
			(hevc ? 0x40 : 0) | (cq ? 0 : 2));
	ave_start_put32(cmd, l->geometry_depth, depth);
	ave_start_put32(cmd, l->const_214, 0x39d);
	ave_start_put32(cmd, l->const_218, 0x18);
	if (l->fps.denominator) {
		ave_put32(cmd, l->fps.denominator, p->fps_den);
		ave_put32(cmd, l->fps.numerator, p->fps_num);
	} else {
		ave_start_put32(cmd, l->fps.numerator, p->fps_int);
	}

	/* Rate control */
	/*
	 * Constant QP is mode 3 on 9003 and mode 2 on 6070. With 3 the 6070
	 * firmware accepts Start but has no rate control for the session and
	 * stops on the first Process.
	 */
	rc_mode = cq && l->rc_mode_fixed_qp ? l->rc_mode_fixed_qp : p->rc_mode;
	ave_start_put32(cmd, l->rc_mode, rc_mode);
	ave_start_put32(cmd, l->bitrate,
			cq && fw->kind == AVE_LAYOUT_6070 ? 0 : p->bitrate);
	for (unsigned int i = 0; i < (unsigned int)(l->init_qp_words ?: 3); i++)
		ave_put32(cmd, (l->init_qp + 4 * i),
			  cq ? p->initial_qp : AVE_START_INIT_QP_VALUE);
	ave_start_put32(cmd, l->limits, p->limit_bytes ? 1 : 0);
	if (l->limit_bytes && p->limit_bytes) {
		ave_put64(cmd, l->limit_bytes,
			  ave_f64((u64)p->limit_bytes * (fw->counted ? 8 : 1),
				  1));
		ave_put64(cmd, l->limit_secs, ave_f64(1, 1));
	}
	ave_start_put64(cmd, l->f64_288, ave_f64(1, 1));
	ave_start_put64(cmd, l->f64_290, ave_f64(1, 2));
	ave_start_put64(cmd, l->f64_2c0, AVE_F64_NEG | ave_f64(1, 1));
	ave_start_put64(cmd, l->f64_2c8, AVE_F64_NEG | ave_f64(1, 1));
	if (cq && fw->kind == AVE_LAYOUT_6070) {
		min_qp = 0;
		max_qp = 51;
	}
	ave_start_put32(cmd, l->min_qp, min_qp);
	ave_start_put32(cmd, l->max_qp, max_qp);
	ave_start_put32(cmd, l->min_qp2, min_qp);
	ave_start_put32(cmd, l->max_qp2, max_qp);
	ave_start_put32(cmd, l->rc_flags, cq ? 0 : hevc ? 0x736 : 0x8202);
	ave_start_put32(cmd, l->rc_310, cq ? 0 : 30);
	if (!hevc)
		ave_start_put32(cmd, l->h264_380, 1);

	/* GOP structure: with B-frames as captured, or P-only, or intra-only. */
	ave_start_put32(
		cmd, l->gop_flags,
		(p->bframes ? 0x600 : 0x200) |
			(fw->counted && cq ? 0x10000 | (hevc ? 0x100 : 0) : 0));
	ave_start_put32(cmd, l->p_frames, p->intra_only ? 0 : 1);
	ave_start_put32(cmd, l->max_b,
			p->bframes ? (fw->kind == AVE_LAYOUT_6070 ? 1 : 3) : 0);
	ave_start_put32(cmd, l->ref_depth, p->bframes ? 3 : 1);
	ave_start_put32(cmd, l->idr_interval, p->gop);

	/* Picture size class, which is also the throughput mode */
	ave_start_put32(cmd, l->size_class,
			p->speed ? AVE_THROUGHPUT_SPEED :
			p->width * p->height > 1280 * 720 ? AVE_THROUGHPUT_QUALITY :
							    1);
	ave_start_put32(cmd, l->coded_w, p->cw);
	ave_start_put32(cmd, l->coded_h, p->ch);
	ave_start_put32(cmd, l->coded_h2, p->ch);
	ave_start_put32(cmd, l->codec_flags, hevc ? 0x2000002 : 2);
	ave_start_put32(cmd, l->bit_depth, depth);
	ave_start_put8(cmd, l->ref_compression, 1);

	if (!hevc) {
		u32 profile_idc = p->high ? 100 : (p->profile == 2 ? 66 : 77);
		u32 level_idc = h264_level_idc[p->level - 1];
		u32 max_mvs = level_idc >= 31 ? 16 : level_idc == 30 ? 32 : 64;

		ave_start_put32(cmd, l->h264_384,
				p->width * p->height <= 320 * 240 ? 0x40 :
								    0x10);
		ave_start_put32(cmd, l->codec_2588, p->high ? 2 : 0);
		ave_start_put32(cmd, l->h264_max_mvs_per_2mb, max_mvs);
		/* Explicit P/B types make the adaptive type selector unnecessary. */
		ave_start_put8(cmd, l->h264_adapt_b,
			       fw->kind == AVE_LAYOUT_6070 ? 0 : p->bframes);
		ave_start_put8(cmd, l->h264_pixel_packing, 1);
		ave_start_put32(cmd, l->h264_transform_mode, p->high ? 2 : 0);
		if (l->h264_skip_mode)
			ave_put16(cmd, l->h264_skip_mode, AVE_AVC_RECON_SKIP_MODE);
		ave_start_put32(cmd, l->h264_input_bit_depth, depth);
		ave_start_put32(cmd, l->h264_parameter_set_count, 1);
		ave_start_put32(cmd, l->h264_slice_map_count, 1);
		ave_start_put32(cmd, l->h264_slice_map_end, p->height);
		/* The input format follows the SPS chroma and depth fields. */
		ave_start_put32(cmd, l->h264_input_format, 16);
		if (l->h264_multipass_inactive)
			for (unsigned int i = 0; i < 4; i++)
				ave_put32(cmd, l->h264_multipass_inactive + 4 * i,
					  U32_MAX);
		for (unsigned int i = 0; i < ARRAY_SIZE(l->h264_num_views); i++)
			ave_start_put32(cmd, l->h264_num_views[i], 1);
		ave_start_put8(cmd, l->h264_allow_reordering, p->bframes);
		for (unsigned int i = 0; i < ARRAY_SIZE(l->h264_ref_spacing); i++)
			ave_start_put32(cmd, l->h264_ref_spacing[i], p->bframes);
		if (l->h264_scaling_4x4)
			for (unsigned int i = 0; i < 6 * 16; i++)
				ave_put16(cmd, l->h264_scaling_4x4 + 2 * i,
					  AVE_AVC_FLAT_SCALING_WEIGHT);
		if (l->h264_scaling_8x8)
			for (unsigned int i = 0; i < 6 * 64; i++)
				ave_put16(cmd, l->h264_scaling_8x8 + 2 * i,
					  AVE_AVC_FLAT_SCALING_WEIGHT);
		ave_start_put32(cmd, l->h264_sve_count, 1);
		ave_start_put16(cmd, l->h264_multi_me, p->bframes ? 1 : 0);
		ave_start_put32(cmd, l->h264_profile,
				fw->kind == AVE_LAYOUT_6070 ? profile_idc : p->profile);
		ave_start_put32(cmd, l->h264_level,
				fw->kind == AVE_LAYOUT_6070 ? level_idc : p->level);
		ave_start_put32(cmd, l->h264_max_refs,
				(p->bframes && fw->kind == AVE_LAYOUT_6070 ?
					 AVE_6070_REORDER_REFS : p->refs - 1) |
					(fw->counted ? (p->cw / 16) << 16 : 0));
		if (fw->counted) {
			ave_put32(cmd, l->profile_idc, profile_idc);
			ave_put32(cmd, l->level_idc, level_idc);
			put_unaligned_le16(p->refs - 1, cmd + l->max_refs_copy);
			ave_put32(cmd, l->mb_size,
				  ((p->ch / 16 - 1) << 16) | (p->cw / 16 - 1));
			cmd[l->mb_height] = p->ch / 16;
			cmd[l->mb_height + 2] = 1;
			cmd[l->mb_height + 3] = 1;
			cmd[l->crop_copy] = crop;
			put_unaligned_le16(crop_r, cmd + l->crop_right_copy);
			put_unaligned_le16(crop_b, cmd + l->crop_bottom_copy);
			cmd[l->codec_byte] = !cq;
			/* CABAC flag in the second, packed picture structure. */
			cmd[l->cabac_copy] = p->profile != 2;
			cmd[l->transform_copy] = p->high;
		}
		ave_start_put32(cmd, l->h264_sps_id, 0);
		ave_start_put32(cmd, l->h264_chroma, 1);
		ave_start_put32(cmd, l->h264_depth_luma, 0);
		ave_start_put32(cmd, l->h264_depth_chroma, 0);
		ave_start_put32(cmd, l->h264_frame_num_bits, 0);
		ave_start_put32(cmd, l->h264_poc_type, p->bframes ? 0 : 2);
		ave_start_put32(cmd, l->h264_poc_bits,
				p->bframes ? 4 : 0);
		ave_start_put8(cmd, l->h264_gaps, 0);
		ave_start_put32(cmd, l->h264_width_mbs, p->cw / 16 - 1);
		ave_start_put32(cmd, l->h264_height_mbs, p->ch / 16 - 1);
		ave_start_put32(cmd, l->h264_frame_only, 1);
		ave_start_put32(cmd, l->h264_direct, 1);
		ave_start_put8(cmd, l->h264_vui,
			       fw->kind == AVE_LAYOUT_6070 && p->bframes);
		/*
		 * Reordered streams publish bitstream_restriction (E.2.1): one
		 * reordered picture, two decoded pictures buffered, and no motion
		 * vector bound beyond the level limits.
		 */
		ave_start_put8(cmd, l->h264_vui_restriction, p->bframes);
		ave_start_put8(cmd, l->h264_vui_mv_over_boundaries, p->bframes);
		ave_start_put32(cmd, l->h264_vui_max_bytes_denom, 0);
		ave_start_put32(cmd, l->h264_vui_max_bits_denom, 0);
		ave_start_put32(cmd, l->h264_vui_max_mv_h,
				p->bframes ? AVE_H264_VUI_LOG2_MAX_MV : 0);
		ave_start_put32(cmd, l->h264_vui_max_mv_v,
				p->bframes ? AVE_H264_VUI_LOG2_MAX_MV : 0);
		ave_start_put32(cmd, l->h264_vui_max_reorder,
				p->bframes ? AVE_6070_REORDER_DEPTH : 0);
		ave_start_put32(cmd, l->h264_vui_max_dec_buffering,
				p->bframes ? AVE_6070_REORDER_REFS : 0);
		ave_start_put8(cmd, l->h264_crop, crop);
		ave_start_put32(cmd, l->h264_crop_left, 0);
		ave_start_put32(cmd, l->h264_crop_right, crop_r);
		ave_start_put32(cmd, l->h264_crop_top, 0);
		ave_start_put32(cmd, l->h264_crop_bottom, crop_b);
		ave_start_put8(cmd, l->h264_fw_header, 1);
		ave_start_put32(cmd, l->h264_header_len, 0);
		ave_start_put32(cmd, l->h264_pps_id, 0);
		ave_start_put32(cmd, l->h264_pps_sps_id, 0);
		ave_start_put32(cmd, l->h264_cabac, p->profile != 2);
		ave_start_put32(cmd, l->h264_slice_groups, 0);
		ave_start_put32(cmd, l->h264_pic_init_qp, 0);
		ave_start_put8(cmd, l->h264_deblock, 1);
		ave_start_put8(cmd, l->h264_constrained, 0);
		ave_start_put8(cmd, l->h264_redundant, 0);
		ave_start_put8(cmd, l->h264_8x8, p->high);
		ave_start_put8(cmd, l->h264_pps_fw_header, 1);
		return;
	}

	ave_start_put32(cmd, l->hevc_pad,
			1 | (p->cw > p->width ? BIT(16) | BIT(18) : 0) |
				(p->ch > p->height ? BIT(17) | BIT(19) : 0));
	ave_start_put32(cmd, l->hevc_profile_a, p->profile);
	ave_start_put32(cmd, l->hevc_profile_b, p->profile);
	if (fw->kind == AVE_LAYOUT_6070) {
		cmd[l->hevc_compat_a + 1] = 1;
		cmd[l->hevc_compat_a + 2] = 1;
		cmd[l->hevc_compat_b + 1] = 1;
		cmd[l->hevc_compat_b + 2] = 1;
		cmd[l->hevc_8bit_a] = 1;
		cmd[l->hevc_8bit_a + 2] = 1;
		cmd[l->hevc_8bit_a + 3] = 1;
		cmd[l->hevc_8bit_b] = 1;
		cmd[l->hevc_8bit_b + 2] = 1;
		cmd[l->hevc_8bit_b + 3] = 1;
	} else {
		cmd[l->hevc_compat_a] = depth == 8;
		cmd[l->hevc_compat_a + 1] = 1;
		cmd[l->hevc_compat_b] = depth == 8;
		cmd[l->hevc_compat_b + 1] = 1;
		cmd[l->hevc_8bit_a] = depth == 8;
		cmd[l->hevc_8bit_b] = depth == 8;
	}
	/* Rate-control words that only change in constant-quality mode */
	ave_start_put32(cmd, l->hevc_cq, cq);
	ave_start_put32(cmd, l->hevc_cq_copy, cq);
	ave_start_put32(cmd, l->hevc_cq_codec, cq);
	ave_start_put32(cmd, l->hevc_cq_picture, cq ? 0x100 : 0);
	ave_start_put32(cmd, l->hevc_rc_270c, 2);
	ave_start_put32(cmd, l->hevc_rc_9604, cq ? 2 : 0);
	ave_start_put32(cmd, l->hevc_rc_9608, cq ? 0 : 0x10000);
	ave_start_put32(cmd, l->hevc_level_a,
			fw->kind == AVE_LAYOUT_6070 ? ave_hevc_level_idc(p->level) :
							 p->level);
	ave_start_put32(cmd, l->hevc_level_b,
			fw->kind == AVE_LAYOUT_6070 ? ave_hevc_level_idc(p->level) :
							 p->level);
	ave_start_put32(cmd, l->hevc_width, p->cw);
	ave_start_put32(cmd, l->hevc_height, p->ch);
	ave_start_put32(cmd, l->hevc_width_ctb, DIV_ROUND_UP(p->cw, 32));
	ave_start_put32(cmd, l->hevc_height_ctb, DIV_ROUND_UP(p->ch, 32));
	ave_start_put32(cmd, l->hevc_conf, crop);
	ave_start_put32(cmd, l->hevc_conf_left, 0);
	ave_start_put32(cmd, l->hevc_conf_right, crop_r);
	ave_start_put32(cmd, l->hevc_conf_top, 0);
	ave_start_put32(cmd, l->hevc_conf_bottom, crop_b);
	ave_start_put32(cmd, l->hevc_depth_luma, depth - 8);
	ave_start_put32(cmd, l->hevc_depth_chroma, depth - 8);
	/* Reference structure: B-frame pyramid or low-delay P (ave-image.c) */
	ave_start_put32(cmd, l->hevc_refs,
			fw->kind == AVE_LAYOUT_6070 ? p->refs - 1 :
						       p->bframes ? 4 : 1);
	ave_start_put32(cmd, l->hevc_dpb_minus1,
			fw->kind == AVE_LAYOUT_6070 ? p->refs - 1 : 4);
	ave_start_put32(cmd, l->hevc_reorder,
			p->bframes ? fw->kind == AVE_LAYOUT_6070 ? 1 : 2 : 0);
	ave_start_put32(cmd, l->hevc_reorder_copy,
			p->bframes ? fw->kind == AVE_LAYOUT_6070 ? 1 : 2 : 0);
	ave_start_put32(cmd, l->hevc_reorder_on, p->bframes);
	ave_start_put32(cmd, l->hevc_input_format, AVE_6070_HEVC_INPUT_420_8BIT);
	ave_start_put32(cmd, l->hevc_pps_count, 1);
	ave_start_put32(cmd, l->hevc_slice_map_count, 1);
	ave_start_put32(cmd, l->hevc_slice_map_end, p->ch);
	ave_start_put32(cmd, l->hevc_sao_config, AVE_6070_HEVC_SAO_ALL);
	ave_start_put32(cmd, l->hevc_sao_offsets, U32_MAX);
	ave_start_put32(cmd, l->hevc_input_depth, depth);
	ave_start_put32(cmd, l->hevc_num_views[0], 1);
	ave_start_put32(cmd, l->hevc_num_views[1], 1);
	ave_start_put32(cmd, l->hevc_sve_count, 1);
	ave_start_put32(cmd, l->hevc_sve_index, 0);
	ave_start_put8(cmd, l->hevc_vps_base_internal, 1);
	ave_start_put8(cmd, l->hevc_vps_base_available, 1);
	ave_start_put8(cmd, l->hevc_vps_temporal_nesting, 1);
	ave_start_put8(cmd, l->hevc_vps_sublayer_info, 1);
	ave_start_put32(cmd, l->hevc_vps_max_dec_buffering, p->refs - 1);
	ave_start_put32(cmd, l->hevc_vps_num_reorder, p->bframes ? 1 : 0);
	ave_start_put32(cmd, l->hevc_vps_max_latency, 0);
	ave_start_put8(cmd, l->hevc_sps_temporal_nesting, 1);
	ave_start_put8(cmd, l->hevc_sps_sublayer_info, 1);
	ave_start_put32(cmd, l->hevc_sps_max_latency, 0);
	ave_start_put32(cmd, l->hevc_sps_log2_min_cb, 0);
	ave_start_put32(cmd, l->hevc_sps_log2_diff_cb, 2);
	ave_start_put32(cmd, l->hevc_sps_log2_min_tb, 0);
	ave_start_put32(cmd, l->hevc_sps_log2_diff_tb, 3);
	ave_start_put32(cmd, l->hevc_sps_depth_inter, 1);
	ave_start_put32(cmd, l->hevc_sps_depth_intra, 0);
	ave_start_put8(cmd, l->hevc_sps_scaling_enabled, 0);
	ave_start_put8(cmd, l->hevc_sps_sao, 1);
	ave_start_put8(cmd, l->hevc_sps_tmvp, 0);
	ave_start_put8(cmd, l->hevc_sps_fw_header, 1);
	ave_start_put32(cmd, l->hevc_sps_header_len, 0);
	ave_start_put32(cmd, l->hevc_pps_extra_slice_bits, 0);
	ave_start_put32(cmd, l->hevc_pps_init_qp, 0);
	ave_start_put8(cmd, l->hevc_pps_cu_qp_delta, 0);
	ave_start_put32(cmd, l->hevc_pps_cu_qp_depth, 0);
	ave_start_put8(cmd, l->hevc_pps_wpp, 1);
	ave_start_put8(cmd, l->hevc_pps_deblock_present, 1);
	ave_start_put8(cmd, l->hevc_pps_fw_header, 1);
	ave_start_put32(cmd, l->hevc_pps_header_len, 0);
	if (fw->kind == AVE_LAYOUT_6070)
		ave_6070_hevc_rps(cmd, l, p->bframes);
}

int ave_build_start(struct ave_ctx *ctx, u8 *cmd, size_t len)
{
	struct ave_dev *ave = ctx->ave;
	struct ave_session *sess = ctx->sess;
	const struct ave_params *p = &sess->p;
	const struct ave_fw_layout *fw = ave->layout;

	if (!fw->encode_ready)
		return -EOPNOTSUPP;
	if (len != fw->bytes[p->codec - 1][AVE_IMAGE_START] || !p->refs ||
	    p->refs > AVE_MAX_REFS || !p->slots || p->slots > fw->max_slots)
		return -EINVAL;
	if (fw->kind == AVE_LAYOUT_6070 && ave_start_validate_6070(sess))
		return -EINVAL;
	ave_image_start(cmd, len, fw, p->codec, p->reorder_off,
			p->codec == AVE_CODEC_HEVC && !p->bframes);
	ave_write_header(ctx, cmd, AVE_OP_START, AVE_OP_START, AVE_SESSION_ARG0,
			 ave_session_setup_arg2(ctx));
	if (ave_start_surfaces(sess, cmd, fw))
		return -EINVAL;
	ave_start_config(p, cmd, fw);
	return 0;
}

#define AVE_PROC_TIMESCALE 14400000ULL
#define AVE_PROC_SHDR_ENTRIES 256
#define AVE_PROC_SHDR_STRIDE 0x400
#define AVE_PROC_FORCED_TYPE 2
#define AVE_PROC_LOW_DELAY 3

int ave_build_process(struct ave_ctx *ctx, u8 *cmd, size_t len,
		      unsigned int slot, dma_addr_t input, u32 bitrate,
		      bool force_key, bool last)
{
	struct ave_session *sess = ctx->sess;
	const struct ave_params *p = &sess->p;
	const struct ave_fw_layout *fw = ctx->ave->layout;
	const struct ave_proc_layout *l = &fw->process[p->codec - 1];
	struct ave_slot *s;
	struct ave_6070_avc_recon_layout recon;
	struct ave_6070_picture pic = { };
	dma_addr_t y = input;
	u64 pts;

	if (!fw->encode_ready)
		return -EOPNOTSUPP;
	if (len != fw->bytes[p->codec - 1][AVE_IMAGE_PROCESS] ||
	    slot >= p->slots || p->slots > fw->max_slots)
		return -EINVAL;
	if ((!l->force_key && force_key) ||
	    (!l->bitrate && bitrate != p->bitrate))
		return -EOPNOTSUPP;
	s = &sess->slot[slot];
	if (fw->kind == AVE_LAYOUT_6070 &&
	    (!IS_ALIGNED(input, SZ_16K) || ave_6070_check_buf(&s->coded) ||
	     ave_6070_check_buf(&s->meta) || ave_6070_check_buf(&s->aux) ||
	     (l->retained_count && !sess->codec_state)))
		return -EINVAL;
	ave_image_process(cmd, len, fw, p->codec);
	if (fw->kind == AVE_LAYOUT_6070) {
		for (unsigned int i = 0; i < l->retained_count; i++) {
			const struct ave_retained_partition *part = &l->retained[i];

			if (part->state_offset + part->bytes > sess->codec_state_bytes)
				return -EINVAL;
			memcpy(cmd + part->command_offset,
			       sess->codec_state + part->state_offset, part->bytes);
		}
		pic = ave_6070_picture(p, sess->frames, force_key, last);
		if (p->codec == AVE_CODEC_H264) {
			ave_6070_avc_slice(cmd + l->retained[0].command_offset, p,
					   &pic, sess->frames);
		} else {
			u8 *slice = cmd + l->retained[0].command_offset;

			u8 *st_rps = cmd + l->retained[1].command_offset;

			ave_put32(slice, AVE_6070_HEVC_POC_LSB,
				  pic.poc_lsb & AVE_6070_POC_LSB_MASK);
			slice[AVE_6070_HEVC_SAO_LUMA] = 1;
			slice[AVE_6070_HEVC_SAO_CHROMA] = 1;
			/* Inter pictures select a Start RPS; the IDR uses none. */
			if (pic.type == AVE_6070_PICTURE_P ||
			    pic.type == AVE_6070_PICTURE_B) {
				st_rps[AVE_6070_HEVC_ST_RPS_SPS_FLAG] = 1;
				ave_put32(st_rps, AVE_6070_HEVC_ST_RPS_IDX, 0);
			}
		}
		ave_put32(cmd, l->pic_size, 0xf68);
		if (p->bit_depth == 8) {
			recon = ave_6070_recon_layout(p->width, p->height,
						      p->bit_depth);
			ave_write_recon_selection(cmd, &l->current_recon,
						  &sess->ref_a[pic.current_ref], &recon);
			ave_put64(cmd, l->current_colocated,
				  sess->ref_b[pic.current_ref].dma);
			ave_put64(cmd, l->current_lowres,
				  sess->ref_c[pic.current_ref].dma);
			for (unsigned int i = 0; i < AVE_6070_LOWRES_RESULTS; i++)
				ave_put64(cmd, l->lowres_results + i * sizeof(u64),
					  sess->ref_d[i].dma);
			if (pic.list0 != U32_MAX) {
				ave_write_recon_selection(cmd, &l->list0_recon,
							  &sess->ref_a[pic.list0], &recon);
				ave_put64(cmd, l->list0_lowres,
					  sess->ref_c[pic.list0].dma);
			}
			if (pic.list0_second != U32_MAX) {
				ave_write_recon_selection(cmd, &l->list0_recon_second,
							  &sess->ref_a[pic.list0_second],
							  &recon);
				ave_put64(cmd, l->list0_lowres_second,
					  sess->ref_c[pic.list0_second].dma);
			}
			if (pic.list1 != U32_MAX) {
				ave_write_recon_selection(cmd, &l->list1_recon,
							  &sess->ref_a[pic.list1], &recon);
				ave_put64(cmd, l->list1_lowres,
					  sess->ref_c[pic.list1].dma);
				ave_put64(cmd, l->list1_colocated,
					  sess->ref_b[pic.list1].dma);
			}
		}
	}
	ave_write_header(ctx, cmd, AVE_OP_PROCESS, fw->process_subtype + slot,
			 0, ave_session_arg2(ctx));
	/* Input picture: NV12, chroma right after p->height luma lines. */
	ave_put64(cmd, l->y, y);
	ave_put32(cmd, l->y + 8,
		  fw->kind == AVE_LAYOUT_6070 ? p->stride : p->stride * p->height);
	ave_put32(cmd, l->y + 12,
		  fw->kind == AVE_LAYOUT_6070 ? 0 : p->stride);
	ave_put64(cmd, l->uv, y + p->stride * p->height);
	ave_put32(cmd, l->uv + 8,
		  fw->kind == AVE_LAYOUT_6070 ? p->stride : p->stride * p->height / 2);
	ave_put32(cmd, l->uv + 12,
		  fw->kind == AVE_LAYOUT_6070 ? 0 : p->stride);
	ave_put32(cmd, l->in_w, p->width);
	ave_put32(cmd, l->in_h, p->height);
	if (l->src_neighbor_data) {
		for (unsigned int set = 0; set < AVE_NEIGHBOUR_SETS; set++) {
			ave_put64(cmd, l->src_neighbor_info + set * sizeof(u64),
				  sess->row_a[set].dma);
			ave_put64(cmd, l->src_neighbor_pixel + set * sizeof(u64),
				  sess->row_b[set].dma);
			ave_put64(cmd, l->src_neighbor_data + set * sizeof(u64),
				  sess->fixed[set].dma);
			ave_put64(cmd, l->src_neighbor_fw + set * sizeof(u64),
				  sess->neighbour_fw[set].dma);
		}
	}

	/* Session values repeated in every frame */
	ave_put32(cmd, l->coded_cap, p->coded_cap);
	ave_put32(cmd, l->fps_int, p->fps_int);
	/* The bit rate may change while streaming; it takes effect here. */
	if (l->bitrate)
		ave_put32(cmd, l->bitrate,
			  p->rc_mode == 3 && fw->kind == AVE_LAYOUT_6070 ? 0 : bitrate);
	if (l->limits)
		ave_put32(cmd, l->limits, p->limit_bytes ? 1 : 0);
	if (l->limit_bytes && p->limit_bytes) {
		ave_put64(cmd, l->limit_bytes, ave_f64(p->limit_bytes, 1));
		ave_put64(cmd, l->limit_secs, ave_f64(1, 1));
	}
	if (l->frame_type)
		ave_put32(cmd, l->frame_type, pic.type);
	else
		ave_put32(cmd, l->const5, 5);
	ave_put64(cmd, l->fps, ave_f64(p->fps_num, p->fps_den));
	ave_put64(cmd, l->timescale, AVE_PROC_TIMESCALE);
	if (l->crop)
		ave_put32(cmd, l->crop,
			  p->cw != p->width || p->ch != p->height);

	/* This frame */
	if (l->first_frame)
		ave_put32(cmd, l->first_frame, sess->frames == 0);
	if (l->first_process)
		cmd[l->first_process] = sess->frames == 0;
	if (l->avg_variance)
		ave_put32(cmd, l->avg_variance, U32_MAX);
	if (l->static_area_mask)
		ave_put32(cmd, l->static_area_mask, AVE_AVC_STATIC_AREA_MASK_ALL);
	if (l->static_area_threshold)
		ave_put32(cmd, l->static_area_threshold, 100);
	ave_put32(cmd, l->slot, slot);
	ave_put64(cmd, l->coded, s->coded.dma);
	ave_put64(cmd, l->meta, s->meta.dma);
	if (l->meta_size)
		ave_put32(cmd, l->meta_size, s->meta.size);
	if (l->extra)
		ave_write_surface(
			fw, cmd,
			&(struct ave_surface_layout)AVE_SURFACE(l->extra, 0), 0,
			s->extra.dma, SZ_16K);
	if (l->frame_size)
		ave_put32(cmd, l->frame_size,
			  p->rc_mode == 3 ? 0 : s->aux.size);
	if (l->frame_buf && fw->kind != AVE_LAYOUT_6070)
		ave_put64(cmd, l->frame_buf,
			  fw->counted && p->rc_mode == 3 ? 0 : s->aux.dma);
	if (l->shdr_table)
		for (unsigned int i = 0; i < AVE_PROC_SHDR_ENTRIES; i++)
			ave_put64(cmd, l->shdr_table + 8 * i,
				  s->aux.dma + i * AVE_PROC_SHDR_STRIDE);
	if (l->lrme_stats)
		ave_put64(cmd, l->lrme_stats, s->aux.dma);
	ave_put32(cmd, l->ordinal, sess->frames);
	pts = mul_u64_u64_div_u64((u64)sess->frames * p->fps_den,
				  AVE_PROC_TIMESCALE, p->fps_num);
	ave_put64(cmd, l->pts, pts);
	if (l->force_key)
		ave_put32(cmd, l->force_key, force_key);
	if (l->force_type)
		ave_put32(cmd, l->force_type,
			  force_key ? AVE_PROC_FORCED_TYPE : 0);
	if (l->low_delay)
		ave_put32(cmd, l->low_delay,
			  p->bframes ? 0 : AVE_PROC_LOW_DELAY);
	return 0;
}
