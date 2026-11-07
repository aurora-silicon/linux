// SPDX-License-Identifier: GPL-2.0-only
/*
 * Apple silicon scaler/CSC (MSR): format table, colour matrices and
 * scale ratios in register encoding.
 *
 * The colour matrices are derived here from the luma coefficients the
 * standards define (Kr, Kb), not looked up, so every encoding/range pair
 * comes from one formula. The format codes and component orders are the
 * values the engine is driven with for those layouts.
 */
#include <linux/array_size.h>
#include <linux/bits.h>
#include <linux/errno.h>
#include <linux/export.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/string.h>

#include <kunit/visibility.h>

#include "apple-scaler-hw.h"

#define ASC_SWIZZLE_IDENTITY	0x03020100
#define ASC_SWIZZLE_CBCR	0x03000102

static const struct asc_format asc_formats[] = {
	{
		/* Bytes B, G, R, A in memory. */
		.fourcc = V4L2_PIX_FMT_ARGB32, .layout = 0x1, .code = 0x1e,
		.sampling = 0x0, .swizzle = ASC_SWIZZLE_IDENTITY, .planes = 1,
		.bpp = { 4 }, .hsub = 1, .vsub = 1, .alpha = true,
	}, {
		.fourcc = V4L2_PIX_FMT_XRGB32, .layout = 0x1, .code = 0x1e,
		.sampling = 0x0, .swizzle = ASC_SWIZZLE_IDENTITY, .planes = 1,
		.bpp = { 4 }, .hsub = 1, .vsub = 1,
	}, {
		.fourcc = V4L2_PIX_FMT_NV12M, .layout = 0xc, .code = 0x1,
		.sampling = 0x6, .swizzle = ASC_SWIZZLE_CBCR, .planes = 2,
		.bpp = { 1, 2 }, .hsub = 2, .vsub = 2, .yuv = true,
	}, {
		.fourcc = V4L2_PIX_FMT_NV12, .layout = 0xc, .code = 0x1,
		.sampling = 0x6, .swizzle = ASC_SWIZZLE_CBCR, .planes = 2,
		.bpp = { 1, 2 }, .hsub = 2, .vsub = 2, .yuv = true,
	}, {
		/* 10 bits in the top of each 16-bit sample. */
		.fourcc = V4L2_PIX_FMT_P010, .layout = 0xc, .code = 0x2,
		.sampling = 0x12, .swizzle = ASC_SWIZZLE_CBCR, .planes = 2,
		.bpp = { 2, 4 }, .hsub = 2, .vsub = 2, .yuv = true,
	},
};

const struct asc_format *asc_format_by_index(unsigned int index)
{
	return index < ARRAY_SIZE(asc_formats) ? &asc_formats[index] : NULL;
}
EXPORT_SYMBOL_IF_KUNIT(asc_format_by_index);

const struct asc_format *asc_find_format(u32 fourcc)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(asc_formats); i++)
		if (asc_formats[i].fourcc == fourcc)
			return &asc_formats[i];
	return NULL;
}
EXPORT_SYMBOL_IF_KUNIT(asc_find_format);

/* Luma coefficients Kr and Kb in units of 1/10000. */
struct asc_luma {
	s64 kr, kb;
};

static int asc_luma_coefs(enum v4l2_ycbcr_encoding enc, struct asc_luma *l)
{
	switch (enc) {
	case V4L2_YCBCR_ENC_601:
	case V4L2_YCBCR_ENC_XV601:
		*l = (struct asc_luma){ 2990, 1140 };
		return 0;
	case V4L2_YCBCR_ENC_709:
	case V4L2_YCBCR_ENC_XV709:
		*l = (struct asc_luma){ 2126, 722 };
		return 0;
	case V4L2_YCBCR_ENC_BT2020:
		*l = (struct asc_luma){ 2627, 593 };
		return 0;
	default:
		return -EINVAL;
	}
}

/* round(num / den) for signed num and positive den */
static s64 asc_div_round(s64 num, s64 den)
{
	return num >= 0 ? div64_s64(num + den / 2, den) : -div64_s64(-num + den / 2, den);
}

/* A coefficient num/den (with the range scale sn/sd applied) in Q3.12. */
static s16 asc_q12(s64 num, s64 den, s64 sn, s64 sd)
{
	return (s16)asc_div_round(num * sn << ASC_CSC_COEF_SHIFT, den * sd);
}

/*
 * Offsets are in units of 2^-24 of full scale. The engine works on a
 * 16-bit internal scale, so the limited-range black level (8-bit code 16)
 * is 0x1010 there; the chroma midpoint is 0x8020 (10-bit code 512). These
 * are the levels the engine is driven with on macOS for every encoding
 * and depth.
 */
#define ASC_LUMA_BLACK	(0x1010 << 8)
#define ASC_CHROMA_MID	(0x8020 << 8)

/**
 * asc_csc_compute - colour conversion between Y'CbCr and R'G'B'
 * @enc: Y'CbCr encoding (luma coefficients)
 * @quant: range of the Y'CbCr side (the R'G'B' side is full range)
 * @yuv_to_rgb: direction
 * @csc: result in register encoding
 *
 * Return: 0, or -EINVAL for an encoding the matrix cannot express.
 */
int asc_csc_compute(enum v4l2_ycbcr_encoding enc,
		    enum v4l2_quantization quant, bool yuv_to_rgb,
		    struct asc_csc *csc)
{
	const s64 one = 10000;
	bool limited = quant != V4L2_QUANTIZATION_FULL_RANGE;
	/* Range scales: luma 219/255 and chroma 224/255 for limited range. */
	s64 yn = limited ? 219 : 1, yd = limited ? 255 : 1;
	s64 cn = limited ? 224 : 1, cd = limited ? 255 : 1;
	struct asc_luma l;
	s64 kg;
	int i;

	if (asc_luma_coefs(enc, &l))
		return -EINVAL;
	kg = one - l.kr - l.kb;
	memset(csc, 0, sizeof(*csc));

	if (yuv_to_rgb) {
		/* R = Y + 2(1-Kr) Cr, B = Y + 2(1-Kb) Cb, G from Y = Kr R + Kg G + Kb B */
		for (i = 0; i < 3; i++)
			csc->coef[i][0] = asc_q12(1, 1, yd, yn);
		csc->coef[0][2] = asc_q12(2 * (one - l.kr), one, cd, cn);
		csc->coef[1][1] = asc_q12(-2 * l.kb * (one - l.kb), one * kg, cd, cn);
		csc->coef[1][2] = asc_q12(-2 * l.kr * (one - l.kr), one * kg, cd, cn);
		csc->coef[2][1] = asc_q12(2 * (one - l.kb), one, cd, cn);
		csc->pre[0] = limited ? -ASC_LUMA_BLACK : 0;
		csc->pre[1] = -ASC_CHROMA_MID;
		csc->pre[2] = -ASC_CHROMA_MID;
	} else {
		/* Y = Kr R + Kg G + Kb B, Cb = (B - Y) / 2(1-Kb), Cr = (R - Y) / 2(1-Kr) */
		csc->coef[0][0] = asc_q12(l.kr, one, yn, yd);
		csc->coef[0][1] = asc_q12(kg, one, yn, yd);
		csc->coef[0][2] = asc_q12(l.kb, one, yn, yd);
		csc->coef[1][0] = asc_q12(-l.kr, 2 * (one - l.kb), cn, cd);
		csc->coef[1][1] = asc_q12(-kg, 2 * (one - l.kb), cn, cd);
		csc->coef[1][2] = asc_q12(1, 2, cn, cd);
		csc->coef[2][0] = asc_q12(1, 2, cn, cd);
		csc->coef[2][1] = asc_q12(-kg, 2 * (one - l.kr), cn, cd);
		csc->coef[2][2] = asc_q12(-l.kb, 2 * (one - l.kr), cn, cd);
		csc->post[0] = limited ? ASC_LUMA_BLACK : 0;
		csc->post[1] = ASC_CHROMA_MID;
		csc->post[2] = ASC_CHROMA_MID;
	}
	return 0;
}
EXPORT_SYMBOL_IF_KUNIT(asc_csc_compute);

u32 asc_csc_offset_reg(s32 offset)
{
	return (u32)offset & ASC_CSC_OFFSET_MASK;
}
EXPORT_SYMBOL_IF_KUNIT(asc_csc_offset_reg);

/**
 * asc_scale_ratio - one scaler ratio register value
 * @in: input extent in pixels
 * @out: output extent in pixels
 *
 * Return: in/out in Q22 with the two lowest bits clear, as the engine
 * takes it; 0 if @out is 0.
 */
u32 asc_scale_ratio(u32 in, u32 out)
{
	if (!out)
		return 0;
	return (u32)div_u64((u64)in << ASC_RATIO_SHIFT, out) & ~3u;
}
EXPORT_SYMBOL_IF_KUNIT(asc_scale_ratio);
