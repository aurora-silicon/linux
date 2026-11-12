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
#include <linux/overflow.h>
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
/* Rate control: coefficient budgets in bytes per macroblock, Q3 */
#define D_RC_TARGET		0x110
#define D_RC_MAX		0x112
#define D_RC_THRESHOLDS		0x114	/* six 7-bit macroblock class thresholds */
#define D_RC_ALPHA		0x11c
#define D_RC_ALPHA0		0x11d
#define D_RC_GAMMA0		0x11e
#define D_RC_BETA		0x120	/* four words */
#define D_RC_QUANT		0x130	/* sixteen words */
#define D_QTABLE_SEL		0x170	/* matrix bank in 7:4, CRC control in 1:0 */

/* Slices of eight macroblocks: log2 width 3 in the high nibble. */
#define APR_SLICE_LOG2		3

/*
 * Per-profile policy, as macOS uses it: the size multiplier relative to
 * 422 HQ (a fraction), and the starting quantiser. The engine refines the
 * slice quantisers around the starting one to meet the byte budget.
 */
static const struct {
	u8 num, den;
	u8 quant_index;
} apr_profile_policy[APR_PROFILES] = {
	[APR_PROFILE_PROXY] = { 13, 63, 8 },
	[APR_PROFILE_LT] = { 13, 28, 3 },
	[APR_PROFILE_STANDARD] = { 2, 3, 2 },
	[APR_PROFILE_HQ] = { 1, 1, 1 },
	[APR_PROFILE_4444] = { 3, 2, 1 },
	[APR_PROFILE_4444_XQ] = { 9, 4, 1 },
};

/*
 * Fixed rate-control settings: macroblock class thresholds (with quantiser
 * refinement off in bit 63), the alpha, alpha0 and gamma0 weights, four
 * betas and sixteen correlation/quantiser thresholds. They do not depend on
 * profile, size or input format.
 */
#define APR_RC_THRESHOLDS	0x8000402015100c0cULL
#define APR_RC_ALPHA		0x80
#define APR_RC_ALPHA0		0x80
#define APR_RC_GAMMA0		0x2c
static const u32 apr_rc_beta[4] = { 0x880080, 0x4e00c5, 0x9000d0, 0x200122 };
#define APR_RC_QUANT		0x400200

/*
 * 422 HQ frame size for a picture of @pixels luma samples: fixed steps up
 * to 2.36 Mpixel, then half a byte per pixel. Other profiles scale it.
 */
static u64 apr_hq_budget(u64 pixels)
{
	static const struct { u32 pixels, bytes; } steps[] = {
		{ 349920, 294912 }, { 414720, 344064 }, { 691200, 442368 },
		{ 921600, 516096 }, { 1555200, 884736 }, { 2073600, 1032192 },
		{ 2359297, 1179648 },
	};

	for (unsigned int i = 0; i < ARRAY_SIZE(steps); i++)
		if (pixels <= steps[i].pixels)
			return steps[i].bytes;
	return pixels / 2;
}

/* Round n / 8 to a single-precision float, then down to an integer. */
static u64 apr_eighths_f32_floor(u64 n)
{
	int shift;
	u64 unit, q, r;

	if (!n)
		return 0;
	/* 24 significant bits: the float's unit, in eighths, is 2^(fls - 24). */
	shift = fls64(n) - 24;
	if (shift <= 0)
		return n / 8;
	unit = 1ULL << shift;
	q = n >> shift;
	r = n & (unit - 1);
	if (r > unit / 2 || (r == unit / 2 && (q & 1)))
		q++;
	return (q << shift) / 8;
}

/*
 * Target and maximum frame sizes in bytes for a profile: the HQ budget
 * scaled by the profile, in whole 4 KiB units, eight ninths of that as the
 * target and nine eighths of the target as the maximum.
 */
void apr_frame_budget(enum apr_profile profile, u32 width, u32 height,
		      u32 *target, u32 *max)
{
	u64 m = apr_hq_budget((u64)width * height) * apr_profile_policy[profile].num /
		apr_profile_policy[profile].den;

	m = round_down(max_t(u64, m, SZ_4K), SZ_4K);
	*target = 8 * m / 9;
	*max = apr_eighths_f32_floor(9ULL * *target);
}
EXPORT_SYMBOL_IF_KUNIT(apr_frame_budget);

/* A frame budget as Q3 bytes per macroblock, after header and slice-table bytes. */
static u16 apr_mb_budget(u32 bytes, u32 slices, u32 mbs)
{
	u32 overhead = 164 + 8 * slices;
	u32 left = bytes > overhead ? bytes - overhead : 1;

	return min(16383U, 8 * (left / mbs));
}

/* Slices per picture: full slices per row, then the remainder in powers of two. */
u32 apr_slice_count(u32 width, u32 height, u32 log2_slice)
{
	u32 mbw = DIV_ROUND_UP(width, 16), mbh = DIV_ROUND_UP(height, 16);

	return (hweight32(mbw & (BIT(log2_slice) - 1)) + (mbw >> log2_slice)) * mbh;
}
EXPORT_SYMBOL_IF_KUNIT(apr_slice_count);

static int apr_build_desc_common(const struct apr_frame *f, u8 *desc)
{
	bool is444 = apr_input_is_444(f->input);
	bool deep = f->input == APR_INPUT_422_16 || f->input == APR_INPUT_444_16;
	u32 min_luma, min_chroma, cmd, slices;

	if ((unsigned int)f->input > APR_INPUT_444_16 ||
	    (unsigned int)f->profile >= APR_PROFILES ||
	    is444 != apr_profile_is_444(f->profile))
		return -EINVAL;
	if (!f->width || !f->height || f->width > U16_MAX || f->height > U16_MAX ||
	    (!is444 && f->width & 1))
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

	return 0;
}

int apr_build_desc(const struct apr_frame *f, u8 *desc)
{
	bool is444 = apr_input_is_444(f->input);
	enum apr_qtable table = apr_profile_qtable(f->profile);
	u32 slices, mbs, target, max;
	unsigned int i;
	int ret;

	ret = apr_build_desc_common(f, desc);
	if (ret)
		return ret;
	slices = apr_slice_count(f->width, f->height, APR_SLICE_LOG2);
	mbs = DIV_ROUND_UP(f->width, 16) * DIV_ROUND_UP(f->height, 16);
	apr_frame_budget(f->profile, f->width, f->height, &target, &max);

	put_unaligned_be16(slices, desc + D_SLICES);
	desc[D_SLICE_LOG2] = APR_SLICE_LOG2 << 4;
	desc[D_QUANT_INDEX] = apr_profile_policy[f->profile].quant_index;

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

	put_unaligned_le16(apr_mb_budget(target, slices, mbs), desc + D_RC_TARGET);
	put_unaligned_le16(apr_mb_budget(max, slices, mbs), desc + D_RC_MAX);
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
 * T6000 constants: Copyright The Asahi Linux Contributors.
 * MIT license: see m1n1-LICENSE.
 * Descriptor v0 (APR a0). Source: MIT m1n1 f3ac615eb137,
 * proxyclient/m1n1/hw/prores.py:7-111 and
 * proxyclient/experiments/prores.py:592-644. Only the input/output prefix
 * is shared with v1. The rate-control block is v1's, 0x20 bytes earlier:
 * the target and maximum Q3 bytes per macroblock at 0xf0/0xf2 take the same
 * per-profile budgets, so every profile lands on its data rate. (The public
 * experiment's 0xffff limits leave the engine unbudgeted.)
 */
int apr_build_desc_v0(const struct apr_frame *f, u8 *desc)
{
	bool is444 = apr_input_is_444(f->input);
	enum apr_qtable table = apr_profile_qtable(f->profile);
	u32 slices, mbs, target, max;
	unsigned int i;
	int ret;

	ret = apr_build_desc_common(f, desc);
	if (ret)
		return ret;
	slices = apr_slice_count(f->width, f->height, APR_SLICE_LOG2);
	mbs = DIV_ROUND_UP(f->width, 16) * DIV_ROUND_UP(f->height, 16);
	apr_frame_budget(f->profile, f->width, f->height, &target, &max);

	/* Frame header, including the legacy aspect/rate byte at 0x065. */
	put_unaligned_be16(D_FH_BYTES, desc + 0x058);
	desc[0x05b] = is444 ? 1 : 0;
	memcpy(desc + 0x05c, "aurr", 4);
	put_unaligned_be16(f->width, desc + 0x060);
	put_unaligned_be16(f->height, desc + 0x062);
	desc[0x064] = (is444 ? 3 : 2) << 6;
	desc[0x066] = f->primaries;
	desc[0x067] = f->transfer;
	desc[0x068] = f->matrix;
	/* No alpha. Both quantisation matrices are included in the header. */
	desc[0x06b] = 3;
	/* Deprecated slice count at 0x0ec stays zero, as in m1n1. */
	desc[0x0ee] = APR_SLICE_LOG2 << 4;
	desc[0x0ef] = apr_profile_policy[f->profile].quant_index;
	put_unaligned_le16(apr_mb_budget(target, slices, mbs), desc + 0x0f0);
	put_unaligned_le16(apr_mb_budget(max, slices, mbs), desc + 0x0f2);
	put_unaligned_le64(APR_RC_THRESHOLDS, desc + 0x0f4);
	put_unaligned_le32(0x2c8080, desc + 0x0fc);
	for (i = 0; i < ARRAY_SIZE(apr_rc_beta); i++)
		put_unaligned_le32(apr_rc_beta[i], desc + 0x100 + 4 * i);
	for (i = 0; i < 16; i++)
		put_unaligned_le32(APR_RC_QUANT, desc + 0x110 + 4 * i);
	put_unaligned_le32(table << 4 | 3, desc + 0x150);
	return 0;
}
EXPORT_SYMBOL_IF_KUNIT(apr_build_desc_v0);

/* Non-RAW decode descriptor version 0 (T6000 and T8140). */
#define DD_COMMAND		0x000
#define  DD_COMMAND_BFF		BIT(14)
#define  DD_COMMAND_IRQ		BIT(12)
#define  DD_COMMAND_COLOR	(2 << 8)
#define  DD_COMMAND_CHROMA_422	(2 << 2)
#define  DD_COMMAND_CHROMA_444	(3 << 2)
#define  DD_COMMAND_DEPTH_16	3
#define DD_COMMAND_EXT		0x004
#define DD_INPUT		0x008
#define DD_INPUT_X		0x010
#define DD_INPUT_Y		0x012
#define DD_INPUT_WIDTH		0x014
#define DD_INPUT_HEIGHT		0x016
#define DD_OUTPUT_X		0x018
#define DD_OUTPUT_Y		0x01a
#define DD_OUTPUT_WIDTH		0x01c
#define DD_OUTPUT_HEIGHT	0x01e
#define DD_LUMA_STRIDE		0x020
#define DD_CHROMA_STRIDE	0x022
#define DD_ALPHA_STRIDE		0x024
#define DD_LUMA			0x028
#define DD_LUMA_METADATA	0x030
#define DD_CHROMA		0x038
#define DD_CHROMA_METADATA	0x040
#define DD_ALPHA		0x048
#define DD_ALPHA_METADATA	0x050
#define DD_CRC_CONTROL		0x090

int apr_build_decode_desc(const struct apr_decode_frame *f, u8 *desc)
{
	bool is444 = apr_input_is_444(f->output);
	bool deep = f->output == APR_INPUT_422_16 ||
		    f->output == APR_INPUT_444_16;
	u32 min_luma, min_chroma, command;

	if ((unsigned int)f->output > APR_INPUT_444_16 || !f->width ||
	    !f->height || f->width > U16_MAX || f->height > U16_MAX ||
	    (!is444 && f->width & 1))
		return -EINVAL;
	min_luma = f->width * (deep ? 2 : 1);
	min_chroma = min_luma * (is444 ? 2 : 1);
	if (f->luma_stride % 64 || f->chroma_stride % 64 ||
	    f->luma_stride < min_luma || f->chroma_stride < min_chroma ||
	    f->luma_stride / 64 > U16_MAX ||
	    f->chroma_stride / 64 > U16_MAX || !f->input || !f->luma ||
	    !f->chroma)
		return -EINVAL;

	memset(desc, 0, APR_DECODE_DESC_BYTES);
	command = (u32)f->job_id << 16 | DD_COMMAND_IRQ | DD_COMMAND_COLOR |
		  (is444 ? DD_COMMAND_CHROMA_444 : DD_COMMAND_CHROMA_422);
	if (deep)
		command |= DD_COMMAND_DEPTH_16;
	if (f->bottom_field_first)
		command |= DD_COMMAND_BFF;
	put_unaligned_le32(command, desc + DD_COMMAND);
	put_unaligned_le32(0, desc + DD_COMMAND_EXT);
	put_unaligned_le64(f->input, desc + DD_INPUT);
	put_unaligned_le16(0, desc + DD_INPUT_X);
	put_unaligned_le16(0, desc + DD_INPUT_Y);
	put_unaligned_le16(f->width, desc + DD_INPUT_WIDTH);
	put_unaligned_le16(f->height, desc + DD_INPUT_HEIGHT);
	put_unaligned_le16(0, desc + DD_OUTPUT_X);
	put_unaligned_le16(0, desc + DD_OUTPUT_Y);
	put_unaligned_le16(f->width, desc + DD_OUTPUT_WIDTH);
	put_unaligned_le16(f->height, desc + DD_OUTPUT_HEIGHT);
	put_unaligned_le16(f->luma_stride / 64, desc + DD_LUMA_STRIDE);
	put_unaligned_le16(f->chroma_stride / 64, desc + DD_CHROMA_STRIDE);
	put_unaligned_le16(0, desc + DD_ALPHA_STRIDE);
	put_unaligned_le64(f->luma, desc + DD_LUMA);
	put_unaligned_le64(0, desc + DD_LUMA_METADATA);
	put_unaligned_le64(f->chroma, desc + DD_CHROMA);
	put_unaligned_le64(0, desc + DD_CHROMA_METADATA);
	put_unaligned_le64(0, desc + DD_ALPHA);
	put_unaligned_le64(0, desc + DD_ALPHA_METADATA);
	put_unaligned_le32(3, desc + DD_CRC_CONTROL);
	return 0;
}
EXPORT_SYMBOL_IF_KUNIT(apr_build_decode_desc);

static int apr_parse_picture(const u8 *data, u32 bytes, u32 width,
			     u32 height, bool interlaced, u32 max_slices,
			     u32 *picture_size, u32 *slices)
{
	u32 header, size, table_bytes, data_bytes = 0, min_size;
	u32 log2_slice, field_height, i;

	if (bytes < 8)
		return -EINVAL;
	header = data[0] >> 3;
	size = get_unaligned_be32(data + 1);
	log2_slice = data[7] >> 4;
	if (header < 8 || header > size || size > bytes || log2_slice > 3 ||
	    (data[7] & 0xf))
		return -EINVAL;
	field_height = interlaced ? DIV_ROUND_UP(height, 2) : height;
	*slices = apr_slice_count(width, field_height, log2_slice);
	if (*slices < 9 || *slices > max_slices ||
	    check_mul_overflow(*slices, 2U, &table_bytes) ||
	    check_add_overflow(header, table_bytes, &min_size) ||
	    min_size > size)
		return -EINVAL;
	for (i = 0; i < *slices; i++) {
		u32 slice = get_unaligned_be16(data + header + 2 * i);

		if (slice < 6 || check_add_overflow(data_bytes, slice, &data_bytes))
			return -EINVAL;
	}
	if (data_bytes > size - min_size)
		return -EINVAL;
	*picture_size = size;
	return 0;
}

int apr_parse_frame(const u8 *data, size_t bytes, struct apr_prores_info *info)
{
	const u8 *header;
	u32 frame_size, header_size, min_header, offset;
	u64 pixels;
	unsigned int i;
	int ret;

	if (!data || !info || bytes < 28 || bytes > U32_MAX)
		return -EINVAL;
	frame_size = get_unaligned_be32(data);
	if (frame_size != bytes || memcmp(data + 4, "icpf", 4))
		return -EINVAL;
	header = data + 8;
	header_size = get_unaligned_be16(header);
	if (header_size < 20 || header_size > frame_size - 8 ||
	    get_unaligned_be16(header + 2) > 1)
		return -EINVAL;
	min_header = 20 + 64 * hweight8(header[19] & 3);
	if (header_size < min_header)
		return -EINVAL;

	memset(info, 0, sizeof(*info));
	info->frame_size = frame_size;
	info->width = get_unaligned_be16(header + 8);
	info->height = get_unaligned_be16(header + 10);
	info->chroma = header[12] >> 6;
	info->frame_type = (header[12] >> 2) & 3;
	info->alpha = header[17] & 0xf;
	info->primaries = header[14];
	info->transfer = header[15];
	info->matrix = header[16];
	pixels = (u64)info->width * info->height;
	if (info->width < 16 || info->height < 16 || pixels > 0x10000000 ||
	    (info->chroma != 2 && info->chroma != 3) || info->frame_type > 2 ||
	    (info->chroma == 2 && info->width & 1) || info->alpha > 2 ||
	    (info->alpha && info->chroma != 3))
		return -EINVAL;
	info->pictures = info->frame_type ? 2 : 1;
	offset = 8 + header_size;
	for (i = 0; i < info->pictures; i++) {
		if (offset > frame_size)
			return -EINVAL;
		info->picture_offset[i] = offset;
		ret = apr_parse_picture(data + offset, frame_size - offset,
					info->width, info->height,
					info->pictures == 2,
					i ? 16384 : 262144,
					&info->picture_size[i], &info->slices[i]);
		if (ret)
			return ret;
		if (check_add_overflow(offset, info->picture_size[i], &offset))
			return -EINVAL;
	}
	return offset == frame_size ? 0 : -EINVAL;
}
EXPORT_SYMBOL_IF_KUNIT(apr_parse_frame);

u32 apr_ring_advance(u32 ptr, u32 bytes, u32 ring_bytes)
{
	u32 offset = ptr & APR_RING_OFFSET_MASK;
	u32 generation = ptr & APR_RING_GENERATION;

	offset += bytes;
	if (offset >= ring_bytes) {
		offset -= ring_bytes;
		generation ^= APR_RING_GENERATION;
	}
	return generation | offset;
}
EXPORT_SYMBOL_IF_KUNIT(apr_ring_advance);

void apr_ring_write(u8 *ring, u32 ring_bytes, u32 ptr, const u8 *desc,
		    u32 desc_bytes)
{
	u32 offset = ptr & APR_RING_OFFSET_MASK;
	u32 first = min(desc_bytes, ring_bytes - offset);

	memcpy(ring + offset, desc, first);
	if (first != desc_bytes)
		memcpy(ring, desc + first, desc_bytes - first);
}
EXPORT_SYMBOL_IF_KUNIT(apr_ring_write);

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
