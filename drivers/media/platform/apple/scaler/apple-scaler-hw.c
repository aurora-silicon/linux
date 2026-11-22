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
#include <linux/log2.h>
#include <linux/math.h>
#include <linux/minmax.h>
#include <linux/export.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/string.h>
#include <linux/wordpart.h>

#include <kunit/visibility.h>

#include "apple-scaler-hw.h"

#define ASC_SWIZZLE_IDENTITY	0x03020100
#define ASC_SWIZZLE_CBCR	0x03000102
/* Compression word value for the lossless formats */
#define ASC_LOSSLESS		0x40

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
		.bpp = { 2, 4 }, .hsub = 2, .vsub = 2, .yuv = true, .wide = true,
	}, {
		/* The AVE temporal filter's picture form; one buffer, four regions. */
		.fourcc = V4L2_PIX_FMT_APPLE_L8V0, .layout = 0xc, .code = 0x78,
		.sampling = 0x46, .swizzle = ASC_SWIZZLE_CBCR, .planes = 2,
		.bpp = { 1, 2 }, .hsub = 2, .vsub = 2, .yuv = true, .lossless = true,
	},
};

void asc_lossless_geom(u32 width, u32 height, struct asc_lossless_geom *g)
{
	u32 tx = DIV_ROUND_UP(width, 32), ty = DIV_ROUND_UP(height, 32);
	u32 grid = roundup_pow_of_two(max3(tx, ty, 64u));

	g->tiles_x = tx;
	g->y_pitch = tx * 1024;
	g->y_meta = ty * g->y_pitch;
	g->c = g->y_meta + grid * grid * 32;
	g->c_pitch = tx * 512;
	g->c_meta = g->c + ty * g->c_pitch;
	g->bytes = g->c_meta + grid * grid * 8;
}
EXPORT_SYMBOL_IF_KUNIT(asc_lossless_geom);

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

/*
 * Job programming. The engine takes the whole pipeline state for every
 * job; nothing carries over from the previous one. The order below follows
 * the pipeline (source, destination, chroma, scaler, CSC, memory system)
 * and ends with the pre-start tag and START.
 */
struct asc_emit {
	struct asc_reg *w;
	unsigned int n, max;
};

static void asc_emit(struct asc_emit *e, u32 off, u32 val)
{
	if (e->n < e->max)
		e->w[e->n] = (struct asc_reg){ off, val };
	e->n++;
}

/* Source (read DMA) and destination (write DMA) register banks. */
struct asc_dma_regs {
	u32 layout, code, sampling, plane, stride, offset, swizzle, size;
	u32 plane_width, plane_height;
	u32 meta;	/* first metadata address slot */
	u32 flag;	/* or-ed into layout-adjacent words on the write side */
};

static const struct asc_dma_regs asc_rdma = {
	.layout = 0x180, .code = 0x184, .sampling = 0x188, .plane = 0x198,
	.stride = 0x1d8, .offset = 0x1e8, .swizzle = 0x1f8, .size = 0x1fc,
	.plane_width = 0x234, .plane_height = 0x244, .meta = 0x1b8, .flag = 0,
};

static const struct asc_dma_regs asc_wdma = {
	.layout = 0x280, .code = 0x284, .sampling = 0x288, .plane = 0x290,
	.stride = 0x2c0, .offset = 0x2cc, .swizzle = 0x2d8, .size = 0x2dc,
	.plane_width = 0x308, .plane_height = 0x314, .meta = 0x2a8, .flag = BIT(15),
};

static void asc_emit_image(struct asc_emit *e, const struct asc_dma_regs *r,
			   const struct asc_image *img)
{
	const struct asc_format *f = img->fmt;
	/* Plane 2 is never used; the engine still takes it at luma size. */
	u32 pw[3] = { img->width, img->width / f->hsub, img->width };
	u32 ph[3] = { img->height, img->height / f->vsub, img->height };
	unsigned int i, nplanes = f->planes == 1 ? 3 : f->planes;

	asc_emit(e, r->layout, f->layout);
	asc_emit(e, r->code, r->flag | f->code);
	asc_emit(e, r->sampling, r->flag | f->sampling);
	/* Lossless compression on: one word per side, two on the read side. */
	if (r->flag)
		asc_emit(e, r->sampling + 4, r->flag | (f->lossless ? ASC_LOSSLESS : 0));
	else {
		asc_emit(e, r->sampling + 4, f->lossless ? ASC_LOSSLESS : 0);
		asc_emit(e, r->sampling + 8, f->lossless ? ASC_LOSSLESS : 0);
	}
	for (i = 0; i < nplanes; i++) {
		u64 dva = i < f->planes ? img->dva[i] : 0;

		asc_emit(e, r->plane + 8 * i, lower_32_bits(dva));
		asc_emit(e, r->plane + 8 * i + 4, upper_32_bits(dva));
		asc_emit(e, r->stride + 4 * i, i < f->planes ? img->stride[i] : 0);
	}
	/* Metadata addresses follow the plane address slots. */
	for (i = 0; f->lossless && i < f->planes; i++) {
		asc_emit(e, r->meta + 8 * i, lower_32_bits(img->meta[i]));
		asc_emit(e, r->meta + 8 * i + 4, upper_32_bits(img->meta[i]));
	}
	for (i = 0; i < 3; i++)
		asc_emit(e, r->offset + 4 * i, 0);
	asc_emit(e, r->swizzle, f->swizzle);
	asc_emit(e, r->size, img->width);
	asc_emit(e, r->size + 4, img->height);
	for (i = 0; i < 3; i++) {
		asc_emit(e, r->plane_width + 4 * i, f->planes == 1 ? img->width : pw[i]);
		asc_emit(e, r->plane_height + 4 * i, f->planes == 1 ? img->height : ph[i]);
	}
}

/* Per-plane memory request words, as the engine is driven on macOS. */
static void asc_emit_memory(struct asc_emit *e, const struct asc_job *job)
{
	const struct asc_format *s = job->src.fmt, *d = job->dst.fmt;
	u32 wide = s->wide ? BIT(1) : 0;
	unsigned int i;

	for (i = 0; i < 4; i++) {
		asc_emit(e, 0x204 + 4 * i, 0x0007d311);
		asc_emit(e, 0x214 + 4 * i, 0x00000707);
		asc_emit(e, 0x224 + 4 * i, 0x00000020);
	}
	for (i = 0; i < 3; i++) {
		asc_emit(e, 0x2e4 + 4 * i, 0x000ac211);
		asc_emit(e, 0x2f0 + 4 * i, 0x0c0ac207);
		asc_emit(e, 0x2fc + 4 * i, 0x04000620);
	}
	asc_emit(e, 0x254, 1);

	for (i = 0; i < 4; i++)
		asc_emit(e, 0x13800 + 4 * i, 0);
	asc_emit(e, 0x13810, 0x8 | wide);
	asc_emit(e, 0x13814, (s->yuv ? 0x28 : 0x8) | wide);
	asc_emit(e, 0x13818, (s->yuv ? 0x28 : 0x8) | wide);
	asc_emit(e, 0x1381c, s->yuv ? 0x20 : 0x8);
	asc_emit(e, 0x13c04, 0);
	asc_emit(e, 0x13c10, 0x8);
	asc_emit(e, 0x13c14, d->yuv ? 0x28 : 0x8);
	asc_emit(e, 0x13c18, d->yuv ? 0x28 : 0x8);
	asc_emit(e, 0x13c1c, d->yuv ? 0x20 : 0x8);
	asc_emit(e, 0x13c00, 1);
}

static void asc_emit_chroma(struct asc_emit *e, const struct asc_job *job)
{
	/* Upsampler interpolation weights, 0x800 = one half. */
	static const u32 up[8] = { 0, 0x800, 0x800, 0, 0, 0x800, 0x800, 0 };
	/* 2x2 ordered-dither index pattern, repeated over 4x4. */
	static const u32 dither[16] = { 0, 2, 0, 2, 3, 1, 3, 1, 0, 2, 0, 2, 3, 1, 3, 1 };
	const struct asc_format *s = job->src.fmt, *d = job->dst.fmt;
	/* Same subsampling on both sides: chroma passes through untouched. */
	bool same = s->yuv && d->yuv && s->hsub == d->hsub && s->vsub == d->vsub;
	bool up_on = s->yuv && s->hsub > 1 && !same;
	unsigned int i;

	asc_emit(e, 0x800, up_on ? 0xd : 0xc);
	/* The weights are loaded for any subsampled source, used only when on. */
	if (s->yuv && s->hsub > 1)
		for (i = 0; i < ARRAY_SIZE(up); i++)
			asc_emit(e, 0x804 + 4 * i, up[i]);
	asc_emit(e, 0x824, 0xc);
	asc_emit(e, 0x900, d->yuv && d->hsub > 1 && !same ? 0x00010103 : 0);
	if (s->wide && !d->wide) {
		asc_emit(e, 0xa00, 0x02020200);
		for (i = 0; i < ARRAY_SIZE(dither); i++)
			asc_emit(e, 0xa80 + 4 * i, dither[i]);
	} else {
		asc_emit(e, 0xa00, 0);
	}
}

/* Scaler at 1:1: polyphase filters off, unit ratios, zero initial phase. */
static void asc_emit_unscaled(struct asc_emit *e, const struct asc_job *job)
{
	static const u32 base[2] = { 0x1000, 0x2000 };
	u32 flags = job->src.fmt->yuv && job->src.fmt->hsub > 1 ? BIT(1) : 0;
	u32 one = 1u << ASC_RATIO_SHIFT;
	unsigned int i;

	for (i = 0; i < 2; i++) {
		asc_emit(e, base[i], flags);
		asc_emit(e, base[i] + 0x04, 0);
		asc_emit(e, base[i] + 0x08, 0);
		asc_emit(e, base[i] + 0x0c, one);
		asc_emit(e, base[i] + 0x10, one);
		asc_emit(e, base[i] + 0x14, one);
		asc_emit(e, base[i] + 0x18, one);
		asc_emit(e, base[i] + 0x1c, 0);
		asc_emit(e, base[i] + 0x20, one);
		asc_emit(e, base[i] + 0x24, one);
	}
	asc_emit(e, 0x480, 0);
}

static void asc_emit_csc(struct asc_emit *e, const struct asc_csc *csc)
{
	unsigned int r, c;

	asc_emit(e, ASC_CSC_MODE, ASC_CSC_MODE_DEFAULT);
	for (c = 0; c < 3; c++)
		asc_emit(e, ASC_CSC_PRE_OFFSET(c), asc_csc_offset_reg(csc->pre[c]));
	for (r = 0; r < 3; r++)
		for (c = 0; c < 3; c++)
			asc_emit(e, ASC_CSC_COEF(r, c), (u16)csc->coef[r][c]);
	for (c = 0; c < 3; c++)
		asc_emit(e, ASC_CSC_POST_OFFSET(c), asc_csc_offset_reg(csc->post[c]));
	/* Output clamps: full signed range on every channel. */
	for (c = 0; c < 3; c++) {
		asc_emit(e, 0xa98c + 4 * c, 0x80000001);
		asc_emit(e, 0xa998 + 4 * c, 0x7fffffff);
		asc_emit(e, 0xa9a4 + 4 * c, 0x80000001);
		asc_emit(e, 0xa9b0 + 4 * c, 0x7fffffff);
	}
	asc_emit(e, ASC_CSC_CTRL, ASC_CSC_CTRL_ENABLE);
}

/* Control words the engine is driven with for every job on macOS. */
static const struct asc_reg asc_job_fixed[] = {
	{ 0x000e4, 0x00000000 }, { 0x000e8, 0x00000038 }, { 0x00124, 0x00000000 },
	{ 0x00380, 0x00000000 }, { 0x03000, 0x00000000 }, { 0x07418, 0xffffffff },
	{ 0x16000, 0x00000000 }, { 0x16004, 0x244024c0 }, { 0x16080, 0x00000000 },
	{ 0x16084, 0x0b710367 }, { 0x16088, 0x00000128 }, { 0x16504, 0x02000500 },
	{ 0x16508, 0x00003200 }, { 0x1651c, 0x00851400 }, { 0x16534, 0x00000008 },
	{ 0x16568, 0x00250500 }, { 0x16588, 0x00496513 }, { 0x16600, 0x00000015 },
	{ 0x16800, 0x00000000 }, { 0x25080, 0x00000000 },
};

/*
 * The MSR20 generation (M4, M4 Pro/Max, A18 Pro) has a control block at
 * 0x2e300 beyond the 0x28000 aperture of older engines. Engines without
 * it run the same jobs with only the common words above.
 */
static const struct asc_reg asc_job_fixed_msr20[] = {
	{ 0x2e300, 0x03804038 }, { 0x2e304, 0xa0a0a0a0 }, { 0x2e308, 0x02550000 },
	{ 0x2e30c, 0x02550255 }, { 0x2e310, 0x00000000 }, { 0x2e314, 0x00100008 },
	{ 0x2e318, 0x02550000 },
};

/**
 * asc_job_build - register writes for one job, in order
 * @job: the conversion; source and destination must be the same size
 * @w: output array
 * @max: capacity of @w
 *
 * Return: number of writes, -EINVAL for a job this path cannot run, or
 * -ENOSPC if @w is too small.
 */
int asc_job_build(const struct asc_job *job, struct asc_reg *w, unsigned int max)
{
	const struct asc_format *s = job->src.fmt, *d = job->dst.fmt;
	struct asc_emit e = { .w = w, .max = max };
	unsigned int i;

	if (!s || !d || job->src.width != job->dst.width ||
	    job->src.height != job->dst.height)
		return -EINVAL;
	/* Y'CbCr <-> R'G'B' needs a matrix; same-family jobs must not have one. */
	if ((s->yuv != d->yuv) != !!job->csc)
		return -EINVAL;

	asc_emit(&e, ASC_CSC_CTRL, ASC_CSC_CTRL_BYPASS);
	asc_emit_image(&e, &asc_rdma, &job->src);
	asc_emit_image(&e, &asc_wdma, &job->dst);
	asc_emit_memory(&e, job);
	/* Alpha fill: constant opaque alpha when the source has none. */
	asc_emit(&e, 0x8c, (d->alpha && !s->alpha ? BIT(31) : 0) | 0xffff);
	asc_emit_chroma(&e, job);
	asc_emit_unscaled(&e, job);
	if (job->csc)
		asc_emit_csc(&e, job->csc);
	for (i = 0; i < ARRAY_SIZE(asc_job_fixed); i++)
		asc_emit(&e, asc_job_fixed[i].off, asc_job_fixed[i].val);
	if (job->msr20)
		for (i = 0; i < ARRAY_SIZE(asc_job_fixed_msr20); i++)
			asc_emit(&e, asc_job_fixed_msr20[i].off, asc_job_fixed_msr20[i].val);
	asc_emit(&e, 0x110, job->transform_id);
	asc_emit(&e, 0x098, 0xfffffffe);
	asc_emit(&e, 0x080, 1);

	return e.n <= max ? e.n : -ENOSPC;
}
EXPORT_SYMBOL_IF_KUNIT(asc_job_build);
