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

#define AVE_F64_NEG		BIT_ULL(63)

static void ave_put32(u8 *cmd, u32 off, u32 val)
{
	put_unaligned_le32(val, cmd + off);
}

static void ave_put64(u8 *cmd, u32 off, u64 val)
{
	put_unaligned_le64(val, cmd + off);
}

/* A surface descriptor: {u64 DART address, u32 size, u32 0}. */
static void ave_put_surface(u8 *cmd, u32 off, dma_addr_t addr, u32 size)
{
	ave_put64(cmd, off, addr);
	ave_put32(cmd, off + 8, size);
	ave_put32(cmd, off + 12, 0);
}

static void ave_put_buf(u8 *cmd, u32 off, const struct ave_buf *b, u32 size)
{
	ave_put_surface(cmd, off, b->dma, size);
}

/* Is another session open in the firmware besides @ctx's? */
static bool ave_cmd_others_open(struct ave_ctx *ctx)
{
	struct ave_dev *ave = ctx->ave;

	for (unsigned int i = 0; i < AVE_MAX_SESSIONS; i++)
		if (ave->sessions[i] && ave->sessions[i] != ctx && ave->sessions[i]->sess->fw_open)
			return true;
	return false;
}

/*
 * Header word +0x2c depends on the set of open sessions: 0x1e0 on Open and
 * Start (0x140 when another session is open), the codec value on every
 * other command of a lone session, and 0 while another one is open.
 */
static u32 ave_session_arg2(struct ave_ctx *ctx)
{
	if (ctx->ave->layout->counted && ctx->sess->p.codec == AVE_CODEC_H264)
		return ctx->sess->p.bframes ? AVE_SESSION_ARG2_H264 : 0x140;
	if (ave_cmd_others_open(ctx))
		return 0;
	return ctx->sess->p.codec == AVE_CODEC_HEVC ? AVE_SESSION_ARG2_HEVC :
						      AVE_SESSION_ARG2_H264;
}

static u32 ave_session_setup_arg2(struct ave_ctx *ctx)
{
	return ave_cmd_others_open(ctx) ? AVE_SESSION_ARG2_SETUP_SHARED : AVE_SESSION_ARG2_SETUP;
}

static void ave_cmd_header(struct ave_ctx *ctx, u8 *cmd, enum ave_opcode op, u32 echo,
			   u32 arg0, u32 arg2)
{
	ctx->sess->token += AVE_TOKEN_STEP;
	ave_put32(cmd, AVE_CMD_OPCODE, op);
	ave_put64(cmd, AVE_CMD_TOKEN, ctx->sess->token);
	ave_put64(cmd, AVE_CMD_CLIENT, ctx->sess->client);
	ave_put32(cmd, AVE_CMD_UNIT, AVE_SESSION_UNIT);
	ave_put32(cmd, AVE_CMD_CODEC, ctx->sess->p.codec);
	ave_put32(cmd, AVE_CMD_OPCODE_ECHO, echo);
	ave_put32(cmd, AVE_CMD_ARG0, arg0);
	ave_put32(cmd, AVE_CMD_ARG1, AVE_SESSION_ARG1);
	ave_put32(cmd, AVE_CMD_ARG2, arg2);
}

/* Firmware-wide configuration, sent once after every firmware boot. */
int ave_cmd_config(struct ave_dev *ave)
{
	int buf = ave_ipc_scalar_buf(ave);
	u8 *cmd;
	int ret;

	if (buf < 0)
		return buf;
	cmd = ave_ipc_cmd(ave, buf, AVE_CONFIG_BYTES);
	ave_put32(cmd, AVE_CMD_OPCODE, AVE_OP_CONFIG);
	ave_put32(cmd, AVE_CONFIG_ALL_UNITS, U32_MAX);
	cmd[AVE_CONFIG_OPT_A] = 1;
	cmd[AVE_CONFIG_OPT_B] = 1;
	ave_put32(cmd, AVE_CONFIG_MCC_DSID, ave->layout->mcc_dsid);
	ret = ave_ipc_submit(ave, buf, AVE_CONFIG_BYTES, AVE_CMD_GLOBAL_TIMEOUT_US, true);
	return ret ?: ave_ipc_settle(ave, AVE_CMD_WAIT_MS);
}

/*
 * Firmware-wide stop, sent after every session has closed. The firmware
 * neither acknowledges nor answers it.
 */
int ave_cmd_halt(struct ave_dev *ave)
{
	int buf = ave_ipc_scalar_buf(ave);
	u8 *cmd;

	if (buf < 0)
		return buf;
	cmd = ave_ipc_cmd(ave, buf, AVE_CMD_HDR_BYTES);
	ave_put32(cmd, AVE_CMD_OPCODE, AVE_OP_HALT);
	ave_put32(cmd, AVE_CMD_OPCODE_ECHO, AVE_HALT_ALL);
	return ave_ipc_submit(ave, buf, AVE_CMD_HDR_BYTES, AVE_CMD_GLOBAL_TIMEOUT_US, false);
}

static int ave_cmd_scalar(struct ave_ctx *ctx, enum ave_opcode op, u32 arg0, u32 arg2)
{
	struct ave_dev *ave = ctx->ave;
	int buf = ave_ipc_scalar_buf(ave);
	u8 *cmd;

	if (buf < 0)
		return buf;
	cmd = ave_ipc_cmd(ave, buf, AVE_CMD_HDR_BYTES);
	ave_cmd_header(ctx, cmd, op, op, arg0, arg2);
	return ave_ipc_submit(ave, buf, AVE_CMD_HDR_BYTES, AVE_CMD_SESSION_TIMEOUT_US, true);
}

int ave_cmd_open(struct ave_ctx *ctx)
{
	struct ave_dev *ave = ctx->ave;
	int ret;

	ave->next_client += AVE_CLIENT_STEP;
	ctx->sess->client = ave->next_client;
	ctx->sess->token = AVE_TOKEN_BASE;
	ret = ave_cmd_scalar(ctx, AVE_OP_OPEN, AVE_SESSION_ARG0, ave_session_setup_arg2(ctx));
	if (!ret)
		ctx->sess->fw_open = true;
	return ret ?: ave_ipc_settle(ave, AVE_CMD_WAIT_MS);
}

/* Drain barrier: answered once every Process sent before it is answered. */
int ave_cmd_complete(struct ave_ctx *ctx, unsigned int count)
{
	int ret = 0;

	for (unsigned int i = 0; i < count && !ret; i++)
		ret = ave_cmd_scalar(ctx, AVE_OP_COMPLETE, 0, ave_session_arg2(ctx));
	return ret;
}

/* Priority and Stop go out together, Close after Stop is answered. */
int ave_cmd_close(struct ave_ctx *ctx)
{
	u32 arg2 = ave_session_arg2(ctx);
	int ret, err;

	ret = ave_cmd_scalar(ctx, AVE_OP_PRIORITY, AVE_SESSION_ARG0, arg2);
	if (!ret)
		ret = ave_cmd_scalar(ctx, AVE_OP_STOP, AVE_SESSION_ARG0, arg2);
	err = ave_ipc_settle(ctx->ave, AVE_CMD_WAIT_MS);
	ret = ret ?: err;
	if (ctx->ave->fw_state != AVE_FW_RUNNING)
		return ret;
	err = ave_cmd_scalar(ctx, AVE_OP_CLOSE, AVE_SESSION_ARG0, arg2);
	if (!err)
		err = ave_ipc_settle(ctx->ave, AVE_CMD_WAIT_MS);
	ctx->sess->fw_open = false;
	return ret ?: err;
}

/* Start (id 6) */

#define AVE_START_ARRAY_LEN		30
#define AVE_START_INIT_QP_VALUE		26

static void ave_start_surfaces(struct ave_session *sess, u8 *cmd,
			       const struct ave_fw_layout *fw)
{
	const struct ave_params *p = &sess->p;
	bool hevc = p->codec == AVE_CODEC_HEVC;
	unsigned int tiles = hevc ? 4 : AVE_TILE_BUFS;
	const struct ave_start_layout *l = &fw->start[p->codec - 1];

	/* Clear every descriptor the image may carry, then fill ours. */
	for (unsigned int i = 0; i < AVE_MAX_REFS; i++) {
		ave_put_surface(cmd, l->ref_a + l->ref_stride * i, 0, 0);
		ave_put_surface(cmd, l->ref_b + l->ref_stride * i, 0, 0);
		ave_put_surface(cmd, l->ref_c + l->ref_c_stride * i, 0, 0);
		ave_put_surface(cmd, l->ref_c_alias + l->ref_c_stride * i, 0, 0);
		ave_put_surface(cmd, l->ref_d + 0x10 * i, 0, 0);
	}
	for (unsigned int s = 0; s < AVE_START_ARRAY_LEN; s++) {
		ave_put_surface(cmd, l->coded + 0x10 * s, 0, 0);
		ave_put_surface(cmd, l->meta + 0x10 * s, 0, 0);
	}

	ave_put_buf(cmd, 0x48, &sess->hdr, fw->hdr_bytes);
	ave_put_surface(cmd, 0x58, 0, 0);
	for (unsigned int i = 0; i < p->refs; i++) {
		ave_put_buf(cmd, l->ref_a + l->ref_stride * i,
			    &sess->ref_a[i], sess->ref_a[i].size);
		ave_put_buf(cmd, l->ref_b + l->ref_stride * i,
			    &sess->ref_b[i], sess->ref_b[i].size);
		ave_put_buf(cmd, l->ref_c + l->ref_c_stride * i,
			    &sess->ref_c[i], sess->ref_c[i].size);
		ave_put_surface(cmd, l->ref_c_alias + l->ref_c_stride * i, sess->ref_c[i].dma, 0);
		ave_put_buf(cmd, l->ref_d + 0x10 * i, &sess->ref_d[i], sess->ref_d[i].size);
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
				ave_put_buf(cmd, l->mb + 0x10 * (i + 1),
					    &sess->mb_extra[i], sess->mb_extra[i].size);
		ave_put32(cmd, l->tiles_count, tiles);
		for (unsigned int i = 0; i < p->refs; i++) {
			ave_put_buf(cmd, l->ref_e + l->ref_stride * i,
				    &sess->ref_e[i], sess->ref_e[i].size);
			ave_put_buf(cmd, l->ref_f + l->ref_stride * i,
				    &sess->ref_f[i], sess->ref_f[i].size);
		}
	}
	ave_put_buf(cmd, l->mb, &sess->mb, sess->mb.size);
	for (unsigned int s = 0; s < p->slots; s++) {
		ave_put_buf(cmd, l->coded + 0x10 * s, &sess->slot[s].coded, p->coded_cap);
		ave_put_buf(cmd, l->meta + 0x10 * s, &sess->slot[s].meta, sess->slot[s].meta.size);
	}
	ave_put_buf(cmd, l->row_a, &sess->row_a, sess->row_a.size);
	ave_put_buf(cmd, l->row_b, &sess->row_b, sess->row_b.size);
	ave_put_buf(cmd, l->fixed, &sess->fixed, SZ_16K);
	for (unsigned int i = 0; i < AVE_TILE_BUFS; i++) {
		u32 off = l->tiles[i];

		if (i < tiles) {
			ave_put_buf(cmd, off, &sess->tile[i], sess->tile[i].size);
			ave_put_buf(cmd, off + 0x10, &sess->tile_aux[i], sess->tile_aux[i].size);
		} else {
			ave_put_surface(cmd, off, 0, 0);
			ave_put_surface(cmd, off + 0x10, 0, 0);
		}
	}
	if (hevc)
		for (unsigned int i = 0; i < 2; i++)
			ave_put_buf(cmd, l->hevc_extra + 0x10 * i, &sess->hevc_extra[i],
				    sess->hevc_extra[i].size);
}

static void ave_start_put32(u8 *cmd, u32 off, u32 val)
{
	if (off)
		ave_put32(cmd, off, val);
}

static void ave_start_config(const struct ave_params *p, u8 *cmd,
			     const struct ave_fw_layout *fw)
{
	bool hevc = p->codec == AVE_CODEC_HEVC;
	bool cq = p->rc_mode == 3;
	u32 depth = p->bit_depth ?: 8;
	u32 crop_r = (p->cw - p->width) / 2, crop_b = (p->ch - p->height) / 2;
	bool crop = crop_r || crop_b;
	const struct ave_start_layout *l = &fw->start[p->codec - 1];

	ave_start_put32(cmd, l->codec, p->codec);
	ave_start_put32(cmd, l->flag_8bit,
			fw->counted ? 0x10000 | (hevc ? (depth == 8 ? 0x41 : 0x40) : 0) |
			(cq ? 0x10 : 0) : hevc ? 1 : 0);
	ave_start_put32(cmd, l->geometry_mode, (hevc ? 0x40 : 0) | (cq ? 0 : 2));
	ave_start_put32(cmd, l->geometry_depth, depth);
	ave_start_put32(cmd, l->const_214, 0x39d);
	ave_start_put32(cmd, l->const_218, 0x18);
	ave_start_put32(cmd, l->fps, p->fps_int);

	/* Rate control */
	ave_start_put32(cmd, l->rc_mode, p->rc_mode);
	ave_start_put32(cmd, l->bitrate, p->bitrate);
	for (unsigned int i = 0; i < 3; i++)
		ave_put32(cmd, (l->init_qp + 4 * i), cq ? p->initial_qp : AVE_START_INIT_QP_VALUE);
	ave_start_put32(cmd, l->limits, p->limit_bytes ? 1 : 0);
	if (l->limit_bytes && p->limit_bytes) {
		ave_put64(cmd, l->limit_bytes,
			  ave_f64((u64)p->limit_bytes * (fw->counted ? 8 : 1), 1));
		ave_put64(cmd, l->limit_secs, ave_f64(1, 1));
	}
	ave_put64(cmd, l->f64_288, ave_f64(1, 1));
	ave_put64(cmd, l->f64_290, ave_f64(1, 2));
	ave_put64(cmd, l->f64_2c0, AVE_F64_NEG | ave_f64(1, 1));
	ave_put64(cmd, l->f64_2c8, AVE_F64_NEG | ave_f64(1, 1));
	ave_start_put32(cmd, l->min_qp, p->min_qp);
	ave_start_put32(cmd, l->max_qp, p->max_qp);
	ave_start_put32(cmd, l->min_qp2, p->min_qp);
	ave_start_put32(cmd, l->max_qp2, p->max_qp);
	ave_start_put32(cmd, l->rc_flags, cq ? 0 : hevc ? 0x736 : 0x8202);
	ave_start_put32(cmd, l->rc_310, cq ? 0 : 30);
	if (!hevc)
		ave_start_put32(cmd, l->h264_380, 1);

	/* GOP structure: with B-frames as captured, or P-only, or intra-only. */
	ave_start_put32(cmd, l->gop_flags, (p->bframes ? 0x600 : 0x200) |
			(fw->counted && cq ? 0x10000 | (hevc ? 0x100 : 0) : 0));
	ave_start_put32(cmd, l->p_frames, p->intra_only ? 0 : 1);
	ave_start_put32(cmd, l->max_b, p->bframes ? 3 : 0);
	ave_start_put32(cmd, l->ref_depth, p->bframes ? 3 : 1);
	ave_start_put32(cmd, l->idr_interval, p->gop);

	/* Picture size */
	ave_start_put32(cmd, l->size_class, p->width * p->height > 1280 * 720 ? 3 : 1);
	ave_start_put32(cmd, l->coded_w, p->cw);
	ave_start_put32(cmd, l->coded_h, p->ch);
	ave_start_put32(cmd, l->coded_h2, p->ch);
	ave_start_put32(cmd, l->codec_flags, hevc ? 0x2000002 : 2);
	ave_start_put32(cmd, l->bit_depth, depth);

	if (!hevc) {
		ave_start_put32(cmd, l->h264_384, p->width * p->height <= 320 * 240 ? 0x40 : 0x10);
		ave_start_put32(cmd, l->codec_2588, p->high ? 2 : 0);
		ave_start_put32(cmd, l->h264_profile, p->profile);
		ave_start_put32(cmd, l->h264_level, p->level);
		ave_start_put32(cmd, l->h264_max_refs,
				(p->refs - 1) | (fw->counted ? (p->cw / 16) << 16 : 0));
		if (fw->counted) {
			static const u8 level_idc[] = {
				10, 9, 11, 12, 13, 20, 21, 22, 30,
				31, 32, 40, 41, 42, 50, 51, 52,
			};

			ave_put32(cmd, l->profile_idc, p->high ? 100 : p->profile == 2 ? 66 : 77);
			ave_put32(cmd, l->level_idc, level_idc[p->level - 1]);
			put_unaligned_le16(p->refs - 1, cmd + l->max_refs_copy);
			ave_put32(cmd, l->mb_size, ((p->ch / 16 - 1) << 16) | (p->cw / 16 - 1));
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
		ave_start_put32(cmd, l->h264_width_mbs, p->cw / 16 - 1);
		ave_start_put32(cmd, l->h264_height_mbs, p->ch / 16 - 1);
		ave_start_put32(cmd, l->h264_crop, crop);
		ave_start_put32(cmd, l->h264_crop_left, 0);
		ave_start_put32(cmd, l->h264_crop_right, crop_r);
		ave_start_put32(cmd, l->h264_crop_top, 0);
		ave_start_put32(cmd, l->h264_crop_bottom, crop_b);
		ave_start_put32(cmd, l->h264_cabac, p->profile != 2);
		cmd[l->h264_8x8] = p->high;
		return;
	}

	ave_start_put32(cmd, l->hevc_pad, 1 |
		  (p->cw > p->width ? BIT(16) | BIT(18) : 0) |
		  (p->ch > p->height ? BIT(17) | BIT(19) : 0));
	ave_start_put32(cmd, l->hevc_profile_a, p->profile);
	ave_start_put32(cmd, l->hevc_profile_b, p->profile);
	cmd[l->hevc_compat_a] = depth == 8;
	cmd[l->hevc_compat_a + 1] = 1;
	cmd[l->hevc_compat_b] = depth == 8;
	cmd[l->hevc_compat_b + 1] = 1;
	cmd[l->hevc_8bit_a] = depth == 8;
	cmd[l->hevc_8bit_b] = depth == 8;
	/* Rate-control words that only change in constant-quality mode */
	ave_start_put32(cmd, l->hevc_cq, cq);
	ave_start_put32(cmd, l->hevc_cq_copy, cq);
	ave_start_put32(cmd, l->hevc_cq_codec, cq);
	ave_start_put32(cmd, l->hevc_cq_picture, cq ? 0x100 : 0);
	ave_start_put32(cmd, l->hevc_rc_270c, 2);
	ave_start_put32(cmd, l->hevc_rc_9604, cq ? 2 : 0);
	ave_start_put32(cmd, l->hevc_rc_9608, cq ? 0 : 0x10000);
	ave_start_put32(cmd, l->hevc_level_a, p->level);
	ave_start_put32(cmd, l->hevc_level_b, p->level);
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
	ave_start_put32(cmd, l->hevc_refs, p->bframes ? 4 : 1);
	ave_start_put32(cmd, l->hevc_dpb_minus1, 4);
	ave_start_put32(cmd, l->hevc_reorder, p->bframes ? 2 : 0);
	ave_start_put32(cmd, l->hevc_reorder_copy, p->bframes ? 2 : 0);
	ave_start_put32(cmd, l->hevc_reorder_on, p->bframes);
}

int ave_cmd_start(struct ave_ctx *ctx)
{
	struct ave_dev *ave = ctx->ave;
	struct ave_session *sess = ctx->sess;
	const struct ave_params *p = &sess->p;
	const struct ave_fw_layout *fw = ave->layout;
	size_t len = fw->bytes[p->codec - 1][AVE_IMAGE_START];
	u8 *cmd;
	int ret;

	if (!p->refs || p->refs > AVE_MAX_REFS || !p->slots || p->slots > fw->max_slots)
		return -EINVAL;
	cmd = ave_ipc_cmd(ave, AVE_BUF_CMD, len);
	if (!cmd)
		return -EBUSY;
	ave_image_start(cmd, len, fw, p->codec, p->reorder_off,
			p->codec == AVE_CODEC_HEVC && !p->bframes);
	ave_cmd_header(ctx, cmd, AVE_OP_START, AVE_OP_START, AVE_SESSION_ARG0,
		       ave_session_setup_arg2(ctx));
	ave_start_surfaces(sess, cmd, fw);
	ave_start_config(&sess->p, cmd, fw);
	ave_debugfs_save(ave, p->codec, AVE_IMAGE_START, cmd, len);

	ret = ave_ipc_submit(ave, AVE_BUF_CMD, len, AVE_CMD_SESSION_TIMEOUT_US, true);
	if (ret)
		return ret;
	sess->handed = true;
	return ave_ipc_settle(ave, AVE_START_WAIT_MS);
}

/* Process (id 8) */

#define AVE_PROC_TIMESCALE		14400000ULL
#define AVE_PROC_SHDR_ENTRIES		256
#define AVE_PROC_SHDR_STRIDE		0x400
#define AVE_PROC_FORCED_TYPE		2
#define AVE_PROC_LOW_DELAY		3

int ave_cmd_process(struct ave_ctx *ctx, unsigned int slot, struct vb2_v4l2_buffer *src,
		    dma_addr_t input, u32 bitrate, bool force_key)
{
	struct ave_dev *ave = ctx->ave;
	struct ave_session *sess = ctx->sess;
	const struct ave_params *p = &sess->p;
	const struct ave_fw_layout *fw = ave->layout;
	const struct ave_proc_layout *l = &fw->process[p->codec - 1];
	size_t len = fw->bytes[p->codec - 1][AVE_IMAGE_PROCESS];
	struct ave_slot *s;
	unsigned int buf = AVE_BUF_PROC(sess->index * AVE_MAX_SLOTS + slot);
	dma_addr_t y = input;
	u64 pts;
	u8 *cmd;
	int ret;

	if (slot >= p->slots || p->slots > fw->max_slots)
		return -EINVAL;
	s = &sess->slot[slot];
	if ((!l->force_key && force_key) || (!l->bitrate && bitrate != p->bitrate))
		return -EOPNOTSUPP;
	ret = ave_ipc_wait_ack(ave, buf);
	if (ret)
		return ret;
	cmd = ave_ipc_cmd(ave, buf, len);
	if (!cmd)
		return -EBUSY;
	ave_image_process(cmd, len, fw, p->codec);
	ave_cmd_header(ctx, cmd, AVE_OP_PROCESS, fw->process_subtype + slot, 0,
		       ave_session_arg2(ctx));

	/* Input picture: NV12, chroma right after p->height luma lines. */
	ave_put64(cmd, l->y, y);
	ave_put32(cmd, l->y + 8, p->stride * p->height);
	ave_put32(cmd, l->y + 12, p->stride);
	ave_put64(cmd, l->uv, y + p->stride * p->height);
	ave_put32(cmd, l->uv + 8, p->stride * p->height / 2);
	ave_put32(cmd, l->uv + 12, p->stride);
	ave_put32(cmd, l->in_w, p->width);
	ave_put32(cmd, l->in_h, p->height);

	/* Session values repeated in every frame */
	ave_put32(cmd, l->coded_cap, p->coded_cap);
	ave_put32(cmd, l->fps_int, p->fps_int);
	/* The bit rate may change while streaming; it takes effect here. */
	if (l->bitrate)
		ave_put32(cmd, l->bitrate, bitrate);
	if (l->limits)
		ave_put32(cmd, l->limits, p->limit_bytes ? 1 : 0);
	if (l->limit_bytes && p->limit_bytes) {
		ave_put64(cmd, l->limit_bytes, ave_f64(p->limit_bytes, 1));
		ave_put64(cmd, l->limit_secs, ave_f64(1, 1));
	}
	ave_put32(cmd, l->const5, 5);
	ave_put64(cmd, l->fps, ave_f64(p->fps_num, p->fps_den));
	ave_put64(cmd, l->timescale, AVE_PROC_TIMESCALE);
	if (l->crop)
		ave_put32(cmd, l->crop, p->cw != p->width || p->ch != p->height);

	/* This frame */
	ave_put32(cmd, l->first_frame, sess->frames == 0);
	ave_put32(cmd, l->slot, slot);
	ave_put64(cmd, l->coded, s->coded.dma);
	ave_put64(cmd, l->meta, s->meta.dma);
	if (l->meta_size)
		ave_put32(cmd, l->meta_size, s->meta.size);
	if (l->extra)
		ave_put_buf(cmd, l->extra, &s->extra, SZ_16K);
	if (l->frame_size)
		ave_put32(cmd, l->frame_size, p->rc_mode == 3 ? 0 : s->aux.size);
	if (l->frame_buf)
		ave_put64(cmd, l->frame_buf, fw->counted && p->rc_mode == 3 ? 0 : s->aux.dma);
	if (l->shdr_table)
		for (unsigned int i = 0; i < AVE_PROC_SHDR_ENTRIES; i++)
			ave_put64(cmd, l->shdr_table + 8 * i,
				  s->aux.dma + i * AVE_PROC_SHDR_STRIDE);
	ave_put32(cmd, l->ordinal, sess->frames);
	pts = mul_u64_u64_div_u64((u64)sess->frames * p->fps_den, AVE_PROC_TIMESCALE, p->fps_num);
	ave_put64(cmd, l->pts, pts);
	if (l->force_key)
		ave_put32(cmd, l->force_key, force_key);
	if (l->force_type)
		ave_put32(cmd, l->force_type, force_key ? AVE_PROC_FORCED_TYPE : 0);
	if (l->low_delay)
		ave_put32(cmd, l->low_delay, p->bframes ? 0 : AVE_PROC_LOW_DELAY);
	if (!sess->frames)
		ave_debugfs_save(ave, p->codec, AVE_IMAGE_PROCESS, cmd, len);

	ret = ave_ipc_submit(ave, buf, len, fw->process_timeout_us, false);
	if (ret)
		return ret;
	s->src = src;
	s->busy = true;
	s->status = 0;
	sess->progress = jiffies;
	sess->last_sent = jiffies;
	sess->nudged = false;
	sess->busy++;
	sess->frames++;
	return 0;
}
