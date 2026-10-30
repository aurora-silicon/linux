// SPDX-License-Identifier: GPL-2.0-only
/*
 * Apple AVE video encoder: firmware commands
 *
 * Start and Process are large command images. Most of their words never
 * change between sessions of a firmware build; the driver takes those from
 * a per-build image file (see ave_fw_templates()) and writes every field
 * that depends on the session or the frame itself.
 *
 * Copyright The Aurora Silicon Contributors
 */
#include <linux/device.h>
#include <linux/firmware.h>
#include <linux/math64.h>
#include <linux/sizes.h>
#include <linux/string.h>
#include <linux/unaligned.h>

#include <media/videobuf2-dma-contig.h>
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

static u32 ave_session_arg2(enum ave_codec codec)
{
	return codec == AVE_CODEC_HEVC ? AVE_SESSION_ARG2_HEVC : AVE_SESSION_ARG2_H264;
}

static void ave_cmd_header(struct ave_ctx *ctx, u8 *cmd, enum ave_opcode op, u32 echo,
			   u32 arg0, u32 arg2)
{
	struct ave_dev *ave = ctx->ave;

	ave->next_token += AVE_TOKEN_STEP;
	ave_put32(cmd, AVE_CMD_OPCODE, op);
	ave_put64(cmd, AVE_CMD_TOKEN, ave->next_token);
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
	ave_put32(cmd, AVE_CONFIG_MCC_DSID, AVE_CONFIG_MCC_DSID_T8140);
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
	ret = ave_cmd_scalar(ctx, AVE_OP_OPEN, AVE_SESSION_ARG0, AVE_SESSION_ARG2_SETUP);
	if (!ret)
		ctx->sess->fw_open = true;
	return ret ?: ave_ipc_settle(ave, AVE_CMD_WAIT_MS);
}

/* Drain barrier: answered once every Process sent before it is answered. */
int ave_cmd_complete(struct ave_ctx *ctx, unsigned int count)
{
	int ret = 0;

	for (unsigned int i = 0; i < count && !ret; i++)
		ret = ave_cmd_scalar(ctx, AVE_OP_COMPLETE, 0, ave_session_arg2(ctx->sess->p.codec));
	return ret;
}

/* Priority and Stop go out together, Close after Stop is answered. */
int ave_cmd_close(struct ave_ctx *ctx)
{
	u32 arg2 = ave_session_arg2(ctx->sess->p.codec);
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

#define AVE_START_H264_BYTES		0x3180
#define AVE_START_HEVC_BYTES		0x13f28

#define AVE_START_SURFACE		0x48	/* and an unused second one at 0x58 */
#define AVE_START_SURFACE_BYTES		0x12c000
#define AVE_START_SURFACE2		0x58
#define AVE_START_REF_A(i)		(0x390 + 0x20 * (i))
#define AVE_START_REF_B(i)		(0x3a0 + 0x20 * (i))
#define AVE_START_REF_C(i)		(0x7d0 + 0x20 * (i))
#define AVE_START_REF_C_ALIAS(i)	(0x7e0 + 0x20 * (i))
#define AVE_START_MB			0xc10
#define AVE_START_CODED(s)		(0xd50 + 0x10 * (s))
#define AVE_START_META(s)		(0xf50 + 0x10 * (s))
#define AVE_START_ARRAY_LEN		30
#define AVE_START_REF_D(i)		(0x1130 + 0x10 * (i))
#define AVE_START_ROW_A			0x1350
#define AVE_START_ROW_B			0x1390
#define AVE_START_FIXED			0x13d0
#define AVE_START_FIXED_BYTES		0x4000
#define AVE_START_HEVC_EXTRA(i)		(0x2520 + 0x10 * (i))

static const u16 ave_start_tile_off[AVE_TILE_BUFS] = {
	0x1410, 0x1430, 0x1490, 0x14b0,		/* both codecs */
	0x1510, 0x1530, 0x1590, 0x15b0,		/* H.264 only */
};

/* Session configuration words */
#define AVE_START_CODEC			0xc0
#define AVE_START_8BIT			0x208
#define AVE_START_CONST_214		0x214
#define AVE_START_CONST_218		0x218
#define AVE_START_FPS			0x220
#define AVE_START_RC_MODE		0x234
#define AVE_START_BITRATE		0x238
#define AVE_START_INIT_QP(i)		(0x240 + 4 * (i))
#define AVE_START_INIT_QP_VALUE		26
#define AVE_START_LIMITS		0x250
#define AVE_START_LIMIT_BYTES		0x258
#define AVE_START_LIMIT_SECS		0x260
#define AVE_START_F64_288		0x288
#define AVE_START_F64_290		0x290
#define AVE_START_MIN_QP		0x298
#define AVE_START_MAX_QP		0x29c
#define AVE_START_GOP_FLAGS		0x2a0
#define AVE_START_P_FRAMES		0x2ac
#define AVE_START_MAX_B			0x2b0
#define AVE_START_REF_DEPTH		0x2b4
#define AVE_START_IDR_INTERVAL		0x2b8
#define AVE_START_F64_2C0		0x2c0
#define AVE_START_F64_2C8		0x2c8
#define AVE_START_RC_FLAGS		0x300
#define AVE_START_MIN_QP2		0x308
#define AVE_START_MAX_QP2		0x30c
#define AVE_START_RC_310		0x310
#define AVE_START_SIZE_CLASS		0x320
#define AVE_START_CODED_W		0x368
#define AVE_START_CODED_H		0x36c
#define AVE_START_CODEC_FLAGS		0x370
#define AVE_START_H264_380		0x380
#define AVE_START_H264_384		0x384
#define AVE_START_H264_HIGH		0x2588
#define AVE_START_BIT_DEPTH		0x2598
#define AVE_START_CODED_H2		0x25dc

/* H.264 parameter-set words */
#define AVE_START_H264_PROFILE		0x291c
#define AVE_START_H264_LEVEL		0x2938
#define AVE_START_H264_MAX_REFS		0x2d44
#define AVE_START_H264_WIDTH_MBS	0x2d4c
#define AVE_START_H264_HEIGHT_MBS	0x2d50
#define AVE_START_H264_CROP		0x2db4
#define AVE_START_H264_CROP_LEFT	0x2db8
#define AVE_START_H264_CROP_RIGHT	0x2dbc
#define AVE_START_H264_CROP_TOP		0x2dc0
#define AVE_START_H264_CROP_BOTTOM	0x2dc4
#define AVE_START_H264_CABAC		0x2fd8
#define AVE_START_H264_8X8		0x3023	/* byte */

/* HEVC parameter-set words; most appear twice */
#define AVE_START_HEVC_PAD		0x26e0
#define AVE_START_HEVC_PROFILE_A	0x2938
#define AVE_START_HEVC_COMPAT_A		0x293d	/* two bytes */
#define AVE_START_HEVC_8BIT_A		0x2962	/* byte */
#define AVE_START_HEVC_LEVEL_A		0x296c
#define AVE_START_HEVC_PROFILE_B	0x5904
#define AVE_START_HEVC_COMPAT_B		0x5909
#define AVE_START_HEVC_8BIT_B		0x592e
#define AVE_START_HEVC_LEVEL_B		0x5938
#define AVE_START_HEVC_WIDTH		0x5ab4
#define AVE_START_HEVC_HEIGHT		0x5ab8
#define AVE_START_HEVC_WIDTH_CTB	0x5abc
#define AVE_START_HEVC_HEIGHT_CTB	0x5ac0
#define AVE_START_HEVC_CONF		0x5ac4
#define AVE_START_HEVC_CONF_LEFT	0x5ac8
#define AVE_START_HEVC_CONF_RIGHT	0x5acc
#define AVE_START_HEVC_CONF_TOP		0x5ad0
#define AVE_START_HEVC_CONF_BOTTOM	0x5ad4
#define AVE_START_HEVC_DEPTH_LUMA	0x5ad8
#define AVE_START_HEVC_DEPTH_CHROMA	0x5adc
/* HEVC reference structure with B-frames (reordering on) */
#define AVE_START_HEVC_REFS		0x259c
#define AVE_START_HEVC_REORDER_COPY	0x2e54
#define AVE_START_HEVC_DPB_MINUS1	0x5b00
#define AVE_START_HEVC_REORDER		0x5b04
#define AVE_START_HEVC_REORDER_ON	0x95fc

static void ave_start_surfaces(struct ave_session *sess, u8 *cmd)
{
	const struct ave_params *p = &sess->p;
	bool hevc = p->codec == AVE_CODEC_HEVC;
	unsigned int tiles = hevc ? 4 : AVE_TILE_BUFS;

	/* Clear every descriptor the image may carry, then fill ours. */
	for (unsigned int i = 0; i < AVE_MAX_REFS; i++) {
		ave_put_surface(cmd, AVE_START_REF_A(i), 0, 0);
		ave_put_surface(cmd, AVE_START_REF_B(i), 0, 0);
		ave_put_surface(cmd, AVE_START_REF_C(i), 0, 0);
		ave_put_surface(cmd, AVE_START_REF_C_ALIAS(i), 0, 0);
		ave_put_surface(cmd, AVE_START_REF_D(i), 0, 0);
	}
	for (unsigned int s = 0; s < AVE_START_ARRAY_LEN; s++) {
		ave_put_surface(cmd, AVE_START_CODED(s), 0, 0);
		ave_put_surface(cmd, AVE_START_META(s), 0, 0);
	}

	ave_put_buf(cmd, AVE_START_SURFACE, &sess->hdr, AVE_START_SURFACE_BYTES);
	ave_put_surface(cmd, AVE_START_SURFACE2, 0, 0);
	for (unsigned int i = 0; i < p->refs; i++) {
		ave_put_buf(cmd, AVE_START_REF_A(i), &sess->ref_a[i], sess->ref_a[i].size);
		ave_put_buf(cmd, AVE_START_REF_B(i), &sess->ref_b[i], sess->ref_b[i].size);
		ave_put_buf(cmd, AVE_START_REF_C(i), &sess->ref_c[i], sess->ref_c[i].size);
		ave_put_surface(cmd, AVE_START_REF_C_ALIAS(i), sess->ref_c[i].dma, 0);
		ave_put_buf(cmd, AVE_START_REF_D(i), &sess->ref_d[i], sess->ref_d[i].size);
	}
	ave_put_buf(cmd, AVE_START_MB, &sess->mb, sess->mb.size);
	for (unsigned int s = 0; s < p->slots; s++) {
		ave_put_buf(cmd, AVE_START_CODED(s), &sess->slot[s].coded, p->coded_cap);
		ave_put_buf(cmd, AVE_START_META(s), &sess->slot[s].meta, sess->slot[s].meta.size);
	}
	ave_put_buf(cmd, AVE_START_ROW_A, &sess->row_a, sess->row_a.size);
	ave_put_buf(cmd, AVE_START_ROW_B, &sess->row_b, sess->row_b.size);
	ave_put_buf(cmd, AVE_START_FIXED, &sess->fixed, AVE_START_FIXED_BYTES);
	for (unsigned int i = 0; i < AVE_TILE_BUFS; i++) {
		u32 off = ave_start_tile_off[i];

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
			ave_put_buf(cmd, AVE_START_HEVC_EXTRA(i), &sess->hevc_extra[i],
				    sess->hevc_extra[i].size);
}

static void ave_start_config(const struct ave_params *p, u8 *cmd)
{
	bool hevc = p->codec == AVE_CODEC_HEVC;
	u32 crop_r = (p->cw - p->width) / 2, crop_b = (p->ch - p->height) / 2;
	bool crop = crop_r || crop_b;

	ave_put32(cmd, AVE_START_CODEC, p->codec);
	ave_put32(cmd, AVE_START_8BIT, hevc ? 1 : 0);
	ave_put32(cmd, AVE_START_CONST_214, 0x39d);
	ave_put32(cmd, AVE_START_CONST_218, 0x18);
	ave_put32(cmd, AVE_START_FPS, p->fps_int);

	/* Rate control */
	ave_put32(cmd, AVE_START_RC_MODE, p->rc_mode);
	ave_put32(cmd, AVE_START_BITRATE, p->bitrate);
	for (unsigned int i = 0; i < 3; i++)
		ave_put32(cmd, AVE_START_INIT_QP(i), AVE_START_INIT_QP_VALUE);
	ave_put32(cmd, AVE_START_LIMITS, p->limit_bytes ? 1 : 0);
	if (p->limit_bytes) {
		ave_put64(cmd, AVE_START_LIMIT_BYTES, ave_f64(p->limit_bytes, 1));
		ave_put64(cmd, AVE_START_LIMIT_SECS, ave_f64(1, 1));
	}
	ave_put64(cmd, AVE_START_F64_288, ave_f64(1, 1));
	ave_put64(cmd, AVE_START_F64_290, ave_f64(1, 2));
	ave_put64(cmd, AVE_START_F64_2C0, AVE_F64_NEG | ave_f64(1, 1));
	ave_put64(cmd, AVE_START_F64_2C8, AVE_F64_NEG | ave_f64(1, 1));
	ave_put32(cmd, AVE_START_MIN_QP, p->min_qp);
	ave_put32(cmd, AVE_START_MAX_QP, p->max_qp);
	ave_put32(cmd, AVE_START_MIN_QP2, p->min_qp);
	ave_put32(cmd, AVE_START_MAX_QP2, p->max_qp);
	ave_put32(cmd, AVE_START_RC_FLAGS, hevc ? 0x736 : 0x8202);
	ave_put32(cmd, AVE_START_RC_310, 30);
	if (!hevc)
		ave_put32(cmd, AVE_START_H264_380, 1);

	/* GOP structure: with B-frames as captured, or P-only, or intra-only. */
	ave_put32(cmd, AVE_START_GOP_FLAGS, p->bframes ? 0x600 : 0x200);
	ave_put32(cmd, AVE_START_P_FRAMES, p->intra_only ? 0 : 1);
	ave_put32(cmd, AVE_START_MAX_B, p->bframes ? 3 : 0);
	ave_put32(cmd, AVE_START_REF_DEPTH, p->bframes ? 3 : 1);
	ave_put32(cmd, AVE_START_IDR_INTERVAL, p->gop);

	/* Picture size */
	ave_put32(cmd, AVE_START_SIZE_CLASS, p->width * p->height > 1280 * 720 ? 3 : 1);
	ave_put32(cmd, AVE_START_CODED_W, p->cw);
	ave_put32(cmd, AVE_START_CODED_H, p->ch);
	ave_put32(cmd, AVE_START_CODED_H2, p->ch);
	ave_put32(cmd, AVE_START_CODEC_FLAGS, hevc ? 0x2000002 : 2);
	ave_put32(cmd, AVE_START_BIT_DEPTH, 8);

	if (!hevc) {
		ave_put32(cmd, AVE_START_H264_384, p->width * p->height <= 320 * 240 ? 0x40 : 0x10);
		ave_put32(cmd, AVE_START_H264_HIGH, p->high ? 2 : 0);
		ave_put32(cmd, AVE_START_H264_PROFILE, p->profile);
		ave_put32(cmd, AVE_START_H264_LEVEL, p->level);
		ave_put32(cmd, AVE_START_H264_MAX_REFS, p->refs - 1);
		ave_put32(cmd, AVE_START_H264_WIDTH_MBS, p->cw / 16 - 1);
		ave_put32(cmd, AVE_START_H264_HEIGHT_MBS, p->ch / 16 - 1);
		ave_put32(cmd, AVE_START_H264_CROP, crop);
		ave_put32(cmd, AVE_START_H264_CROP_LEFT, 0);
		ave_put32(cmd, AVE_START_H264_CROP_RIGHT, crop_r);
		ave_put32(cmd, AVE_START_H264_CROP_TOP, 0);
		ave_put32(cmd, AVE_START_H264_CROP_BOTTOM, crop_b);
		ave_put32(cmd, AVE_START_H264_CABAC, p->profile != 2);
		cmd[AVE_START_H264_8X8] = p->high;
		return;
	}

	ave_put32(cmd, AVE_START_HEVC_PAD, 1 |
		  (p->cw > p->width ? BIT(16) | BIT(18) : 0) |
		  (p->ch > p->height ? BIT(17) | BIT(19) : 0));
	ave_put32(cmd, AVE_START_HEVC_PROFILE_A, p->profile);
	ave_put32(cmd, AVE_START_HEVC_PROFILE_B, p->profile);
	cmd[AVE_START_HEVC_COMPAT_A] = 1;
	cmd[AVE_START_HEVC_COMPAT_A + 1] = 1;
	cmd[AVE_START_HEVC_COMPAT_B] = 1;
	cmd[AVE_START_HEVC_COMPAT_B + 1] = 1;
	cmd[AVE_START_HEVC_8BIT_A] = 1;
	cmd[AVE_START_HEVC_8BIT_B] = 1;
	ave_put32(cmd, AVE_START_HEVC_LEVEL_A, p->level);
	ave_put32(cmd, AVE_START_HEVC_LEVEL_B, p->level);
	ave_put32(cmd, AVE_START_HEVC_WIDTH, p->cw);
	ave_put32(cmd, AVE_START_HEVC_HEIGHT, p->ch);
	ave_put32(cmd, AVE_START_HEVC_WIDTH_CTB, DIV_ROUND_UP(p->cw, 32));
	ave_put32(cmd, AVE_START_HEVC_HEIGHT_CTB, DIV_ROUND_UP(p->ch, 32));
	ave_put32(cmd, AVE_START_HEVC_CONF, crop);
	ave_put32(cmd, AVE_START_HEVC_CONF_LEFT, 0);
	ave_put32(cmd, AVE_START_HEVC_CONF_RIGHT, crop_r);
	ave_put32(cmd, AVE_START_HEVC_CONF_TOP, 0);
	ave_put32(cmd, AVE_START_HEVC_CONF_BOTTOM, crop_b);
	ave_put32(cmd, AVE_START_HEVC_DEPTH_LUMA, 0);
	ave_put32(cmd, AVE_START_HEVC_DEPTH_CHROMA, 0);
	/* Only the B-frame structure is described for HEVC (see ave-sess.c). */
	ave_put32(cmd, AVE_START_HEVC_REFS, 4);
	ave_put32(cmd, AVE_START_HEVC_DPB_MINUS1, 4);
	ave_put32(cmd, AVE_START_HEVC_REORDER, 2);
	ave_put32(cmd, AVE_START_HEVC_REORDER_COPY, 2);
	ave_put32(cmd, AVE_START_HEVC_REORDER_ON, 1);
}

int ave_cmd_start(struct ave_ctx *ctx)
{
	struct ave_dev *ave = ctx->ave;
	struct ave_session *sess = ctx->sess;
	const struct firmware *tmpl = ave->tmpl[sess->p.codec - 1][AVE_TMPL_START];
	u8 *cmd;
	int ret;

	cmd = ave_ipc_cmd(ave, AVE_BUF_CMD, tmpl->size);
	if (!cmd)
		return -EBUSY;
	memcpy(cmd, tmpl->data, tmpl->size);
	ave_cmd_header(ctx, cmd, AVE_OP_START, AVE_OP_START, AVE_SESSION_ARG0,
		       AVE_SESSION_ARG2_SETUP);
	ave_start_surfaces(sess, cmd);
	ave_start_config(&sess->p, cmd);
	ave_sess_check_addrs(ctx, cmd, tmpl->size, AVE_TMPL_START, 0);

	ret = ave_ipc_submit(ave, AVE_BUF_CMD, tmpl->size, AVE_CMD_SESSION_TIMEOUT_US, true);
	return ret ?: ave_ipc_settle(ave, AVE_START_WAIT_MS);
}

/* Process (id 8) */

struct ave_proc_layout {
	u32 first_frame;
	u32 y, uv;		/* {u64 address, u32 size, u32 stride} */
	u32 in_w, in_h;
	u32 coded_cap;
	u32 fps_int;
	u32 bitrate;
	u32 limits, limit_bytes, limit_secs;
	u32 const5;
	u32 fps;		/* f64 */
	u32 timescale;		/* u64 */
	u32 slot;
	u32 coded, meta;	/* u64 addresses */
	u32 ordinal;
	u32 pts;		/* u64 */
	u32 force_key;		/* 1 on a forced key frame */
	u32 force_type;		/* 2 on a forced key frame */
	u32 frame_buf;		/* H.264 only */
	u32 shdr_table;		/* HEVC only */
	u32 crop;		/* HEVC only */
};

static const struct ave_proc_layout ave_proc_h264 = {
	.first_frame = 0x12c0,
	.y = 0x5830, .uv = 0x5850,
	.in_w = 0x58e8, .in_h = 0x58ec,
	.coded_cap = 0x61c8,
	.fps_int = 0x1610,
	.bitrate = 0x1620,
	.limits = 0x1628, .limit_bytes = 0x1630, .limit_secs = 0x1638,
	.const5 = 0x6238,
	.fps = 0x6268, .timescale = 0x6278,
	.slot = 0x61b4, .coded = 0x61b8, .meta = 0x61c0,
	.ordinal = 0x6228, .pts = 0x6270,
	.force_key = 0x1658, .force_type = 0x29f8,
	.frame_buf = 0x58c8,
};

static const struct ave_proc_layout ave_proc_hevc = {
	.first_frame = 0x5d90,
	.y = 0xa300, .uv = 0xa320,
	.in_w = 0xa3b8, .in_h = 0xa3bc,
	.coded_cap = 0xac98,
	.fps_int = 0x60e0,
	.bitrate = 0x60f0,
	.limits = 0x60f8, .limit_bytes = 0x6100, .limit_secs = 0x6108,
	.const5 = 0xad08,
	.fps = 0xad38, .timescale = 0xad48,
	.slot = 0xac84, .coded = 0xac88, .meta = 0xac90,
	.ordinal = 0xacf8, .pts = 0xad40,
	.force_key = 0x6128, .force_type = 0x74c8,
	.shdr_table = 0xd88,
	.crop = 0x74dc,
};

#define AVE_PROC_TIMESCALE		14400000ULL
#define AVE_PROC_SHDR_ENTRIES		256
#define AVE_PROC_SHDR_STRIDE		0x400
#define AVE_PROC_FORCED_TYPE		2

int ave_cmd_process(struct ave_ctx *ctx, unsigned int slot, struct vb2_v4l2_buffer *src,
		    u32 bitrate, bool force_key)
{
	struct ave_dev *ave = ctx->ave;
	struct ave_session *sess = ctx->sess;
	const struct ave_params *p = &sess->p;
	const struct ave_proc_layout *l = p->codec == AVE_CODEC_HEVC ? &ave_proc_hevc :
								       &ave_proc_h264;
	const struct firmware *tmpl = ave->tmpl[p->codec - 1][AVE_TMPL_PROCESS];
	struct ave_slot *s = &sess->slot[slot];
	unsigned int buf = AVE_BUF_PROC(slot);
	dma_addr_t y;
	u64 pts;
	u8 *cmd;
	int ret;

	y = vb2_dma_contig_plane_dma_addr(&src->vb2_buf, 0) + src->vb2_buf.planes[0].data_offset;
	if (!IS_ALIGNED(y, 64))
		return -EINVAL;

	ret = ave_ipc_wait_ack(ave, buf);
	if (ret)
		return ret;
	cmd = ave_ipc_cmd(ave, buf, tmpl->size);
	if (!cmd)
		return -EBUSY;
	memcpy(cmd, tmpl->data, tmpl->size);
	ave_cmd_header(ctx, cmd, AVE_OP_PROCESS, AVE_PROC_SUBTYPE + slot, 0,
		       ave_session_arg2(p->codec));

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
	ave_put32(cmd, l->bitrate, bitrate);
	ave_put32(cmd, l->limits, p->limit_bytes ? 1 : 0);
	if (p->limit_bytes) {
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
	if (l->frame_buf)
		ave_put64(cmd, l->frame_buf, s->aux.dma);
	if (l->shdr_table)
		for (unsigned int i = 0; i < AVE_PROC_SHDR_ENTRIES; i++)
			ave_put64(cmd, l->shdr_table + 8 * i,
				  s->aux.dma + i * AVE_PROC_SHDR_STRIDE);
	ave_put32(cmd, l->ordinal, sess->frames);
	pts = mul_u64_u64_div_u64((u64)sess->frames * p->fps_den, AVE_PROC_TIMESCALE, p->fps_num);
	ave_put64(cmd, l->pts, pts);
	ave_put32(cmd, l->force_key, force_key);
	ave_put32(cmd, l->force_type, force_key ? AVE_PROC_FORCED_TYPE : 0);
	ave_sess_check_addrs(ctx, cmd, tmpl->size, AVE_TMPL_PROCESS, y);

	ret = ave_ipc_submit(ave, buf, tmpl->size, AVE_CMD_SESSION_TIMEOUT_US, false);
	if (ret)
		return ret;
	s->src = src;
	s->busy = true;
	s->status = 0;
	sess->progress = jiffies;
	sess->busy++;
	sess->frames++;
	return 0;
}
