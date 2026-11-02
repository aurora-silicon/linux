// SPDX-License-Identifier: GPL-2.0-only
/*
 * Apple ProRes (APR) encoder: quantisation tables and job descriptors.
 *
 * Nothing here touches hardware, so all of it is covered by KUnit.
 */
#include <linux/bitops.h>
#include <linux/errno.h>
#include <linux/export.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/sizes.h>
#include <linux/string.h>
#include <linux/unaligned.h>

#include <kunit/visibility.h>

#include "apple-prores.h"

#define Z APR_QM_ZERO

/*
 * Weight matrices per engine table. Standard, LT and HQ are the published
 * ProRes matrices. XQ luma is the HQ shape at half the weight. The engine's
 * Proxy table drops the highest frequencies outright instead of weighting
 * them by 63.
 */
static const u8 apr_qm_xq[64] = {
	2, 2, 2, 2, 2, 2, 2, 2,
	2, 2, 2, 2, 2, 2, 2, 2,
	2, 2, 2, 2, 2, 2, 2, 2,
	2, 2, 2, 2, 2, 2, 2, 3,
	2, 2, 2, 2, 2, 2, 3, 3,
	2, 2, 2, 2, 2, 3, 3, 3,
	2, 2, 2, 2, 3, 3, 3, 4,
	2, 2, 2, 2, 3, 3, 4, 4,
};

static const u8 apr_qm_hq[64] = {
	4, 4, 4, 4, 4, 4, 4, 4,
	4, 4, 4, 4, 4, 4, 4, 4,
	4, 4, 4, 4, 4, 4, 4, 4,
	4, 4, 4, 4, 4, 4, 4, 5,
	4, 4, 4, 4, 4, 4, 5, 5,
	4, 4, 4, 4, 4, 5, 5, 6,
	4, 4, 4, 4, 5, 5, 6, 7,
	4, 4, 4, 4, 5, 6, 7, 7,
};

static const u8 apr_qm_standard[64] = {
	4, 4, 5, 5, 6, 7, 7, 9,
	4, 4, 5, 6, 7, 7, 9, 9,
	5, 5, 6, 7, 7, 9, 9, 10,
	5, 5, 6, 7, 7, 9, 9, 10,
	5, 6, 7, 7, 8, 9, 10, 12,
	6, 7, 7, 8, 9, 10, 12, 15,
	6, 7, 7, 9, 10, 11, 14, 17,
	7, 7, 9, 10, 11, 14, 17, 21,
};

static const u8 apr_qm_lt[64] = {
	4, 5, 6, 7, 9, 11, 13, 15,
	5, 5, 7, 8, 11, 13, 15, 17,
	6, 7, 9, 11, 13, 15, 15, 17,
	7, 7, 9, 11, 13, 15, 17, 19,
	7, 9, 11, 13, 14, 16, 19, 23,
	9, 11, 13, 14, 16, 19, 23, 29,
	9, 11, 13, 15, 17, 21, 28, 35,
	11, 13, 16, 17, 21, 28, 35, 41,
};

static const u8 apr_qm_proxy[64] = {
	4, 7, 9, 11, 13, 14, 63, Z,
	7, 7, 11, 12, 14, 63, Z, Z,
	9, 11, 13, 14, 63, Z, Z, Z,
	11, 11, 13, 14, Z, Z, Z, Z,
	11, 13, 14, Z, Z, Z, Z, Z,
	13, 14, Z, Z, Z, Z, Z, Z,
	13, Z, Z, Z, Z, Z, Z, Z,
	Z, Z, Z, Z, Z, Z, Z, Z,
};

const u8 *const apr_qmat_luma[APR_QTABLES] = {
	[APR_QTABLE_XQ] = apr_qm_xq,
	[APR_QTABLE_HQ] = apr_qm_hq,
	[APR_QTABLE_STANDARD] = apr_qm_standard,
	[APR_QTABLE_LT] = apr_qm_lt,
	[APR_QTABLE_PROXY] = apr_qm_proxy,
};
EXPORT_SYMBOL_IF_KUNIT(apr_qmat_luma);

/* XQ keeps the HQ weights for chroma; the other tables share one matrix. */
const u8 *const apr_qmat_chroma[APR_QTABLES] = {
	[APR_QTABLE_XQ] = apr_qm_hq,
	[APR_QTABLE_HQ] = apr_qm_hq,
	[APR_QTABLE_STANDARD] = apr_qm_standard,
	[APR_QTABLE_LT] = apr_qm_lt,
	[APR_QTABLE_PROXY] = apr_qm_proxy,
};
EXPORT_SYMBOL_IF_KUNIT(apr_qmat_chroma);

bool apr_profile_is_444(enum apr_profile profile)
{
	return profile == APR_PROFILE_4444 || profile == APR_PROFILE_4444_XQ;
}
EXPORT_SYMBOL_IF_KUNIT(apr_profile_is_444);

bool apr_input_is_444(enum apr_input input)
{
	return input == APR_INPUT_444_8 || input == APR_INPUT_444_16;
}
EXPORT_SYMBOL_IF_KUNIT(apr_input_is_444);

enum apr_qtable apr_profile_qtable(enum apr_profile profile)
{
	switch (profile) {
	case APR_PROFILE_PROXY:
		return APR_QTABLE_PROXY;
	case APR_PROFILE_LT:
		return APR_QTABLE_LT;
	case APR_PROFILE_STANDARD:
		return APR_QTABLE_STANDARD;
	case APR_PROFILE_4444_XQ:
		return APR_QTABLE_XQ;
	case APR_PROFILE_HQ:
	case APR_PROFILE_4444:
	default:
		return APR_QTABLE_HQ;
	}
}
EXPORT_SYMBOL_IF_KUNIT(apr_profile_qtable);

/*
 * Each matrix register holds two neighbouring weights of one row, the lower
 * column in bits 11:0 and the next in bits 23:12. A weight field is the
 * weight with bit 11 set; a dropped coefficient is all ones.
 */
static u32 apr_qm_field(u8 weight)
{
	return weight == APR_QM_ZERO ? 0xfff : BIT(11) | weight;
}

u32 apr_qmat_reg(const u8 *qmat, unsigned int index)
{
	if (index >= 32)
		return 0;
	return apr_qm_field(qmat[2 * index + 1]) << 12 |
	       apr_qm_field(qmat[2 * index]);
}
EXPORT_SYMBOL_IF_KUNIT(apr_qmat_reg);

/*
 * The DC scale table maps the engine's quantiser index to a ProRes qscale:
 * 1 to 64 in steps of one, then up to 256 in steps of four. Each entry
 * holds 2 * qscale in bits 17:9 (wrapping at 512 for the last entry) and a
 * rounding bias in bits 8:0, 2 * qscale - 1 up to qscale 64 and
 * 2 * qscale - 4 above.
 */
u32 apr_dc_scale_reg(unsigned int index)
{
	u32 qs, bias;

	if (index >= APR_DC_SCALE_ENTRIES)
		return 0;
	qs = index < 64 ? index + 1 : 64 + 4 * (index - 63);
	bias = qs <= 64 ? 2 * qs - 1 : 2 * qs - 4;
	return ((2 * qs) & 0x1ff) << 9 | (bias & 0x1ff);
}
EXPORT_SYMBOL_IF_KUNIT(apr_dc_scale_reg);

/* Descriptor version 1 (T8140). Little-endian unless noted. */
#define D_CMD			0x000
#define  D_CMD_BASE		0x3630	/* job controls, encode mode, pass count */
#define  D_CMD_SUB_422		(2 << 2)
#define  D_CMD_SUB_444		(3 << 2)
#define  D_CMD_DEPTH_16		3
#define D_OUTPUT		0x008
#define D_OUTPUT_SIZE		0x010
#define D_OFFSET_X		0x014
#define D_OFFSET_Y		0x016
#define D_WIDTH			0x018
#define D_HEIGHT		0x01a
#define D_CODED_W		0x01c
#define D_CODED_H		0x01e
#define D_LUMA_STRIDE		0x020	/* in units of 64 bytes */
#define D_CHROMA_STRIDE		0x022
#define D_ALPHA_STRIDE		0x024
#define D_LUMA			0x028
#define D_CHROMA		0x038
#define D_ALPHA			0x048
/* 0x058-0x070: statistics outputs, unused. */
#define D_SLICES		0x078	/* big-endian */
#define D_SLICE_LOG2		0x07a	/* high nibble */
#define D_QUANT_INDEX		0x07b
/* Frame header fields the engine writes into the bitstream; big-endian. */
#define D_FH_SIZE		0x07c
#define D_FH_VERSION		0x07f
#define D_FH_VENDOR		0x080
#define D_FH_WIDTH		0x084
#define D_FH_HEIGHT		0x086
#define D_FH_FORMAT		0x088
#define D_FH_PRIMARIES		0x089
#define D_FH_TRANSFER		0x08a
#define D_FH_MATRIX		0x08b
#define D_FH_ALPHA		0x08c
#define D_FH_FLAGS		0x08f
#define D_FH_BYTES		0x94
/* Coded window and rate control */
#define D_MB_W			0x110
#define D_MB_H			0x112
#define D_RC_THRESHOLDS		0x114	/* six 7-bit macroblock class thresholds */
#define D_RC_ALPHA		0x11c
#define D_RC_ALPHA0		0x11d
#define D_RC_GAMMA0		0x11e
#define D_RC_BETA		0x120	/* four words */
#define D_RC_QUANT		0x130	/* sixteen words */
#define D_QTABLE_SEL		0x170	/* matrix bank in 7:4, CRC control in 1:0 */

/* Slices of eight macroblocks: log2 width 3 in the high nibble. */
#define APR_SLICE_LOG2		3
#define APR_QUANT_INDEX		2

/*
 * Rate-control settings the engine is known to work with (m1n1). Their
 * per-profile tuning is not known yet.
 */
#define APR_RC_THRESHOLDS	0x8000402015100c0cULL
#define APR_RC_ALPHA		0x80
#define APR_RC_ALPHA0		0x80
#define APR_RC_GAMMA0		0x2c
static const u32 apr_rc_beta[4] = { 0x880080, 0x4e00c5, 0x9000d0, 0x200122 };
#define APR_RC_QUANT		0x400200

/* Slices per picture: full slices per row, then the remainder in powers of two. */
u32 apr_slice_count(u32 width, u32 height, u32 log2_slice)
{
	u32 mbw = DIV_ROUND_UP(width, 16), mbh = DIV_ROUND_UP(height, 16);

	return (hweight32(mbw & (BIT(log2_slice) - 1)) + (mbw >> log2_slice)) * mbh;
}
EXPORT_SYMBOL_IF_KUNIT(apr_slice_count);

int apr_build_desc(const struct apr_frame *f, u8 *desc)
{
	bool is444 = apr_input_is_444(f->input);
	bool deep = f->input == APR_INPUT_422_16 || f->input == APR_INPUT_444_16;
	enum apr_qtable table = apr_profile_qtable(f->profile);
	u32 min_luma, min_chroma, cmd, slices;
	unsigned int i;

	if (f->profile >= APR_PROFILES || is444 != apr_profile_is_444(f->profile))
		return -EINVAL;
	if (!f->width || !f->height || f->width > U16_MAX || f->height > U16_MAX ||
	    f->width & 1)
		return -EINVAL;
	min_luma = f->width * (deep ? 2 : 1);
	min_chroma = min_luma * (is444 ? 2 : 1);
	if (f->luma_stride % 64 || f->chroma_stride % 64 ||
	    f->luma_stride < min_luma || f->chroma_stride < min_chroma ||
	    f->luma_stride / 64 > U16_MAX || f->chroma_stride / 64 > U16_MAX)
		return -EINVAL;
	if (!f->luma || !f->chroma || !f->output || f->output_size < SZ_4K)
		return -EINVAL;
	slices = apr_slice_count(f->width, f->height, APR_SLICE_LOG2);
	if (slices > U16_MAX)
		return -EINVAL;

	memset(desc, 0, APR_DESC_BYTES);

	cmd = D_CMD_BASE | (is444 ? D_CMD_SUB_444 : D_CMD_SUB_422);
	if (deep)
		cmd |= D_CMD_DEPTH_16;
	put_unaligned_le32(cmd, desc + D_CMD);
	put_unaligned_le64(f->output, desc + D_OUTPUT);
	put_unaligned_le32(f->output_size, desc + D_OUTPUT_SIZE);
	put_unaligned_le16(0, desc + D_OFFSET_X);
	put_unaligned_le16(0, desc + D_OFFSET_Y);
	put_unaligned_le16(f->width, desc + D_WIDTH);
	put_unaligned_le16(f->height, desc + D_HEIGHT);
	put_unaligned_le16(f->width, desc + D_CODED_W);
	put_unaligned_le16(f->height, desc + D_CODED_H);
	put_unaligned_le16(f->luma_stride / 64, desc + D_LUMA_STRIDE);
	put_unaligned_le16(f->chroma_stride / 64, desc + D_CHROMA_STRIDE);
	put_unaligned_le16(0, desc + D_ALPHA_STRIDE);
	put_unaligned_le64(f->luma, desc + D_LUMA);
	put_unaligned_le64(f->chroma, desc + D_CHROMA);
	put_unaligned_le64(0, desc + D_ALPHA);

	put_unaligned_be16(slices, desc + D_SLICES);
	desc[D_SLICE_LOG2] = APR_SLICE_LOG2 << 4;
	desc[D_QUANT_INDEX] = APR_QUANT_INDEX;

	put_unaligned_be16(D_FH_BYTES, desc + D_FH_SIZE);
	desc[D_FH_VERSION] = is444 ? 1 : 0;
	memcpy(desc + D_FH_VENDOR, "aurr", 4);
	put_unaligned_be16(f->width, desc + D_FH_WIDTH);
	put_unaligned_be16(f->height, desc + D_FH_HEIGHT);
	/* chroma_format in bits 7:6, progressive frame */
	desc[D_FH_FORMAT] = (is444 ? 3 : 2) << 6;
	desc[D_FH_PRIMARIES] = f->primaries;
	desc[D_FH_TRANSFER] = f->transfer;
	desc[D_FH_MATRIX] = f->matrix;
	desc[D_FH_ALPHA] = 0;
	/* Both quantisation matrices are carried in the frame header. */
	desc[D_FH_FLAGS] = 0x03;

	put_unaligned_le16(DIV_ROUND_UP(f->width, 16), desc + D_MB_W);
	put_unaligned_le16(DIV_ROUND_UP(f->height, 16), desc + D_MB_H);
	put_unaligned_le64(APR_RC_THRESHOLDS, desc + D_RC_THRESHOLDS);
	desc[D_RC_ALPHA] = APR_RC_ALPHA;
	desc[D_RC_ALPHA0] = APR_RC_ALPHA0;
	desc[D_RC_GAMMA0] = APR_RC_GAMMA0;
	for (i = 0; i < ARRAY_SIZE(apr_rc_beta); i++)
		put_unaligned_le32(apr_rc_beta[i], desc + D_RC_BETA + 4 * i);
	for (i = 0; i < 16; i++)
		put_unaligned_le32(APR_RC_QUANT, desc + D_RC_QUANT + 4 * i);
	put_unaligned_le32(table << 4 | 3, desc + D_QTABLE_SEL);
	return 0;
}
EXPORT_SYMBOL_IF_KUNIT(apr_build_desc);

/*
 * A finished frame starts with its big-endian length, which counts the whole
 * frame, followed by the 'icpf' frame identifier.
 */
int apr_check_output(const u8 *out, size_t capacity, u32 *frame_bytes)
{
	u32 size;

	if (capacity < 8)
		return -EINVAL;
	size = get_unaligned_be32(out);
	if (memcmp(out + 4, "icpf", 4) || size < 8 + D_FH_BYTES || size > capacity)
		return -EIO;
	*frame_bytes = size;
	return 0;
}
EXPORT_SYMBOL_IF_KUNIT(apr_check_output);
