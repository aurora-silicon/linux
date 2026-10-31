/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Apple AVD VPU codec driver
 *
 * Copyright The Asahi Linux Contributors
 * Copyright 2023 Eileen Yoon <eyn@gmx.com>
 *
 * Copyright (c) 2023, Collabora
 *	Benjamin Gaignard <benjamin.gaignard@collabora.com>
 *
 * Copyright (c) 2014 Rockchip Electronics Co., Ltd.
 *	Hertz Wong <hertz.wong@rock-chips.com>
 *	Herman Chen <herman.chen@rock-chips.com>
 *
 * Copyright (C) 2014 Google, Inc.
 *	Tomasz Figa <tfiga@chromium.org>
 */

#include <linux/refcount.h>
#include <linux/unaligned.h>
#include <media/videobuf2-dma-contig.h>

#include "avd.h"
#include "avd-av1-entropymode.h"
#include "avd-inst.h"
#include "avd-av1-fp.h"

/* The AV1 builder writes only the current frame's DMA program. */
#undef push
#undef pusha
#define push(inst, name) avd_av1_push(run, (inst))
#define pusha(addr, name, i) avd_av1_push_address(ctx, run, (addr))

/*
 * This comes from
 * verisilicon/rockchip_vpu981_hw_av1_dec.c
 *
 * These 3 values aren't defined enum v4l2_av1_segment_feature because
 * they are not part of the specification
 */
#define V4L2_AV1_SEG_LVL_ALT_LF_Y_H	2
#define V4L2_AV1_SEG_LVL_ALT_LF_U	3
#define V4L2_AV1_SEG_LVL_ALT_LF_V	4

#define DIV_LUT_BITS		8
#define DIV_LUT_PREC_BITS	14
#define WARPEDMODEL_PREC_BITS	16
#define WARP_PARAM_REDUCE_BITS	6

#define DIV_LUT_NUM	BIT(DIV_LUT_BITS)

#define AV1_DIV_ROUND_UP_POW2(value, n)			\
({							\
	typeof(n) _n  = n;				\
	typeof(value) _value = value;			\
	(_value + (BIT(_n) >> 1)) >> _n;		\
})

#define AV1_DIV_ROUND_UP_POW2_SIGNED(value, n)				\
({									\
	typeof(n) _n_  = n;						\
	typeof(value) _value_ = value;					\
	(((_value_) < 0) ? -AV1_DIV_ROUND_UP_POW2(-(_value_), (_n_))	\
		: AV1_DIV_ROUND_UP_POW2((_value_), (_n_)));		\
})

#define AV1_REF_SCALE_SHIFT	14
#define SUPERRES_SCALE_BITS	14
#define SUPERRES_EXTRA_BITS	8
#define SUPERRES_SCALE_MASK	((1 << 14) - 1)

#define AV1_CODEC_MODE_INTRABC(v)	FIELD_PREP(BIT(28), !!(v))

#define AV1_HDR_JNT_COMP(v)		FIELD_PREP(BIT(0), !!(v))
#define AV1_HDR_ORDER_HINT_BITS(v)	FIELD_PREP(GENMASK(3, 1), v)
#define AV1_HDR_ORDER_HINT(v)		FIELD_PREP(BIT(4), !!(v))
#define AV1_HDR_DUAL_FILTER(v)		FIELD_PREP(BIT(5), !!(v))
#define AV1_HDR_MASKED_COMPOUND(v)	FIELD_PREP(BIT(6), !!(v))
#define AV1_HDR_INTERINTRA_COMPOUND(v)	FIELD_PREP(BIT(7), !!(v))
#define AV1_HDR_INTRA_EDGE_FILTER(v)	FIELD_PREP(BIT(8), !!(v))
#define AV1_HDR_FILTER_INTRA(v)		FIELD_PREP(BIT(9), !!(v))
#define AV1_HDR_128X128_SUPERBLOCK(v)	FIELD_PREP(BIT(10), !!(v))
#define AV1_HDR_SCREEN_CONTENT_TOOLS(v)	FIELD_PREP(BIT(11), !!(v))

#define AV1_FLAGS_TX_MODE_LARGEST(v)		FIELD_PREP(BIT(0), !!(v))
#define AV1_FLAGS_TX_MODE_SELECT(v)		FIELD_PREP(BIT(1), !!(v))
#define AV1_FLAGS_TX_REDUCED_SET(v)		FIELD_PREP(BIT(2), !!(v))
#define AV1_FLAGS_ALLOW_WARPED_MOTION(v)	FIELD_PREP(BIT(3), !!(v))
#define AV1_FLAGS_SKIP_MODE(v)			FIELD_PREP(BIT(4), !!(v))
#define AV1_FLAGS_MOTION_IS_SWITCHABLE(v)	FIELD_PREP(BIT(5), !!(v))
#define AV1_FLAGS_INTRABC(v)			FIELD_PREP(BIT(6), !!(v))
#define AV1_FLAGS_INTEGER_MV(v)			FIELD_PREP(BIT(7), !!(v))
#define AV1_FLAGS_DISABLE_CDF_UPDATE(v)		FIELD_PREP(BIT(8), !!(v))
#define AV1_FLAGS_TX_MODE_ONLY_4X4(v)		FIELD_PREP(BIT(9), !!(v))
#define AV1_FLAGS_HIGH_PRECISION_MV(v)		FIELD_PREP(BIT(10), !!(v))
#define AV1_FLAGS_REF_FRAME_MVS(v)		FIELD_PREP(BIT(12), !!(v))
#define AV1_FLAGS_SEG_TEMPORAL_UPDATE(v)	FIELD_PREP(BIT(13), !!(v))
#define AV1_FLAGS_SEG_UPDATE_MAP(v)		FIELD_PREP(BIT(14), !!(v))
#define AV1_FLAGS_SEG_ENABLED(v)		FIELD_PREP(BIT(15), !!(v))
#define AV1_FLAGS_SEG_UPDATE_DATA(v)		FIELD_PREP(BIT(16), !!(v))
#define AV1_FLAGS_INTRA(v)			FIELD_PREP(BIT(31), !!(v))
#define AV1_FLAGS_SKIP_MODE1(v)			FIELD_PREP(GENMASK(19, 17), v)
#define AV1_FLAGS_SKIP_MODE0(v)			FIELD_PREP(GENMASK(22, 20), v)
#define AV1_FLAGS_ORDER_HINT(v)			FIELD_PREP(GENMASK(30, 23), v)

#define AV1_FLAGS_REFERENCE_SELECT(v)		FIELD_PREP(BIT(28), !!(v))
#define AV1_FLAGS_INTERPOLATION_FILTER(v)	FIELD_PREP(GENMASK(31, 29), v)

#define AV1_SIGN_BIAS(v, i)	(!!(v) << (i))
#define AV1_REF_SLOT(v, i)	(((v) & 7) << (7 + ((i) * 3)))

#define AV1_SEG_ALT_Q(v)	FIELD_PREP(GENMASK(8, 0), v)
#define AV1_SEG_ALT_Q_EN(v)	FIELD_PREP(BIT(9), !!(v))
#define AV1_SEG_REF_FRAME(v)	FIELD_PREP(GENMASK(12, 10), v)
#define AV1_SEG_REF_FRAME_EN(v)	FIELD_PREP(BIT(13), !!(v))
#define AV1_SEG_REF_SKIP_EN(v)	FIELD_PREP(BIT(14), !!(v))
#define AV1_SEG_REF_GMV_EN(v)	FIELD_PREP(BIT(15), !!(v))

#define AV1_SEG_LF_V(v)		FIELD_PREP(GENMASK(6, 0), v)
#define AV1_SEG_LF_V_EN(v)	FIELD_PREP(BIT(7), !!(v))
#define AV1_SEG_LF_U(v)		FIELD_PREP(GENMASK(14, 8), v)
#define AV1_SEG_LF_U_EN(v)	FIELD_PREP(BIT(15), !!(v))
#define AV1_SEG_LF_Y_H(v)	FIELD_PREP(GENMASK(22, 16), v)
#define AV1_SEG_LF_Y_H_EN(v)	FIELD_PREP(BIT(23), !!(v))
#define AV1_SEG_LF_Y_V(v)	FIELD_PREP(GENMASK(30, 24), v)
#define AV1_SEG_LF_Y_V_EN(v)	FIELD_PREP(BIT(31), !!(v))

#define AV1_SEG_UNK0(v)		FIELD_PREP(BIT(16), !!(v))

#define AV1_QP_U_AC(v)		FIELD_PREP(GENMASK(6, 0), v)
#define AV1_QP_U_DC(v)		FIELD_PREP(GENMASK(13, 7), v)
#define AV1_QP_Y_DC(v)		FIELD_PREP(GENMASK(20, 14), v)
#define AV1_QP_BASE_IDX(v)	FIELD_PREP(GENMASK(28, 21), v)
#define AV1_QP_DELTA_RES(v)	FIELD_PREP(GENMASK(30, 29), v)
#define AV1_QP_PRESENT(v)	FIELD_PREP(BIT(31), !!(v))
#define AV1_QP_V_DC(v)		FIELD_PREP(GENMASK(26, 20), v)
#define AV1_QP_V_AC(v)		FIELD_PREP(GENMASK(19, 13), v)
#define AV1_QP_QMATRIX(v)	FIELD_PREP(BIT(12), !!(v))
#define AV1_QP_QM_Y(v)		FIELD_PREP(GENMASK(11, 8), v)
#define AV1_QP_QM_U(v)		FIELD_PREP(GENMASK(7, 4), v)
#define AV1_QP_QM_V(v)		FIELD_PREP(GENMASK(3, 0), v)

#define AV1_GM_VALID(v)		FIELD_PREP(BIT(30), !!(v))
#define AV1_GM_TYPE(v)		FIELD_PREP(GENMASK(31, 30), v)
#define AV1_GM_PARAM0(v)	FIELD_PREP(GENMASK(29, 15), v)
#define AV1_GM_PARAM1(v)	FIELD_PREP(GENMASK(14, 0), v)

#define AV1_GM_PARAM_SHEAR0(v)	FIELD_PREP(GENMASK(15, 0), v)
#define AV1_GM_PARAM_SHEAR1(v)	FIELD_PREP(GENMASK(31, 16), v)

#define AV1_ORDER_HINT0(v)	FIELD_PREP(GENMASK(7, 0), v)
#define AV1_ORDER_HINT1(v)	FIELD_PREP(GENMASK(15, 8), v)
#define AV1_ORDER_HINT2(v)	FIELD_PREP(GENMASK(23, 16), v)
#define AV1_ORDER_HINT3(v)	FIELD_PREP(GENMASK(31, 24), v)
#define AV1_ORDER_HINT_REF(v)	FIELD_PREP(GENMASK(31, 29), v)

#define AV1_LF_DELTA_ENABLED(v)	FIELD_PREP(BIT(27), !!(v))
#define AV1_LF_DELTA_MULTI(v)	FIELD_PREP(BIT(28), !!(v))
#define AV1_LF_DELTA_PRESENT(v)	FIELD_PREP(BIT(31), !!(v))
#define AV1_LF_DELTA_RES(v)	FIELD_PREP(GENMASK(30, 29), v)
#define AV1_LF_LV3(v)		FIELD_PREP(GENMASK(5, 0), v)
#define AV1_LF_LV2(v)		FIELD_PREP(GENMASK(11, 6), v)
#define AV1_LF_LV1(v)		FIELD_PREP(GENMASK(17, 12), v)
#define AV1_LF_LV0(v)		FIELD_PREP(GENMASK(23, 18), v)

#define AV1_LF_SHARPNESS(v)	FIELD_PREP(GENMASK(31, 28), v)
#define AV1_LF_REF0(v)		FIELD_PREP(GENMASK(27, 21), v)
#define AV1_LF_REF1(v)		FIELD_PREP(GENMASK(20, 14), v)
#define AV1_LF_REF2(v)		FIELD_PREP(GENMASK(13, 7), v)
#define AV1_LF_REF3(v)		FIELD_PREP(GENMASK(6, 0), v)

#define AV1_LF_MODE0(v)		FIELD_PREP(GENMASK(13, 7), v)
#define AV1_LF_MODE1(v)		FIELD_PREP(GENMASK(6, 0), v)

#define AV1_CDEF_EN(v)		FIELD_PREP(BIT(28), !!(v))
#define AV1_CDEF_BITS(v)	FIELD_PREP(GENMASK(25, 24), v)
#define AV1_CDEF_DAMPING(v)	FIELD_PREP(GENMASK(27, 26), v)

#define AV1_CDEF_Y_PRI(v)	FIELD_PREP(GENMASK(11, 8), v)
#define AV1_CDEF_Y_SEC(v)	FIELD_PREP(GENMASK(7, 6), v)
#define AV1_CDEF_UV_PRI(v)	FIELD_PREP(GENMASK(5, 2), v)
#define AV1_CDEF_UV_SEC(v)	FIELD_PREP(GENMASK(1, 0), v)

#define AV1_CDEF_LO(v)	FIELD_PREP(GENMASK(11, 0), v)
#define AV1_CDEF_HI(v)	FIELD_PREP(GENMASK(23, 12), v)

#define AV1_LR_TYPE0(v)	FIELD_PREP(GENMASK(11, 10), v)
#define AV1_LR_TYPE1(v)	FIELD_PREP(GENMASK(9, 8), v)
#define AV1_LR_TYPE2(v)	FIELD_PREP(GENMASK(7, 6), v)

#define AV1_LR_UNIT2(v)	FIELD_PREP(GENMASK(1, 0), v)
#define AV1_LR_UNIT1(v)	FIELD_PREP(GENMASK(3, 2), v)
#define AV1_LR_UNIT0(v)	FIELD_PREP(GENMASK(5, 4), v)

#define AV1_REF_ORDER_HINT(v)	FIELD_PREP(GENMASK(23, 0), v)
#define AV1_REF_SCALE_X(v)	FIELD_PREP(GENMASK(31, 16), v)
#define AV1_REF_SCALE_Y(v)	FIELD_PREP(GENMASK(15, 0), v)

#define AV1_SUPERRES_USE(v)	FIELD_PREP(BIT(31), !!(v))
#define AV1_SUPERRES_DENOM(v)	FIELD_PREP(GENMASK(30, 28), v)
/* im gonna asume 16 like the others */
#define AV1_SUPERRES_UPSCALE(v)	FIELD_PREP(GENMASK(16, 0), v)

#define AV1_UPSCALE_STEPX(v)	FIELD_PREP(GENMASK(13, 0), v)
#define AV1_UPSCALE_UPSCX(v)	FIELD_PREP(GENMASK(31, 14), v)

#define AVD_CDFS_SIZE	sizeof(struct avd_av1_cdfs)

/* Imported planes can have an unaligned length; AVD aligns both tail addresses. */
#define AVD_AV1_COLOR_OFFSET(dst, cl) \
	(ALIGN_DOWN((dst), AVD_ALIGN) - ALIGN(cl, AVD_ALIGN))
#define AVD_AV1_CDFS_OFFSET(dst, cl) \
	(AVD_AV1_COLOR_OFFSET(dst, cl) - ALIGN(AVD_CDFS_SIZE, AVD_ALIGN))
/* The frame's segmentation map, saved with it like the CDFs (AV1 7.20) */
#define AVD_AV1_SEG_OFFSET(dst, cl, sl) \
	(AVD_AV1_CDFS_OFFSET(dst, cl) - (sl))

#define AV1_PRIMARY_REF_NONE	7

/* A capture reference and every in-flight reader hold their own references. */
struct avd_av1_aux {
	refcount_t refs;
	struct avd_buf buf;
	u64 producer;
	bool done;
	int status;
};

struct avd_av1_ref {
	struct list_head node;
	unsigned int index;
	u64 timestamp;
	u16 width, height;
	struct avd_av1_aux *cdf;
	struct avd_av1_aux *seg;
};

struct avd_av1_scratch {
	struct avd_buf pipe_state;
	struct avd_buf above_info;
	struct avd_buf rf_above_info;
	struct avd_buf ip_above;
	struct avd_buf lf_above;
	struct avd_buf lf_above_info;
	struct avd_buf lf_left;
	struct avd_buf lf_left_info;
	struct avd_buf sr_left;
	struct avd_buf rf_left;
	struct avd_buf rf_left_info;
	struct avd_buf mv_above_info;
};

struct avd_av1_run {
	struct avd_run base;
	struct avd_frame *slot;
	struct avd_av1_scratch bufs;
	struct avd_av1_aux *cdf_in, *cdf_out;
	struct avd_av1_aux *seg_in, *seg_out;
	bool cdf_written, seg_written;
	int error;

	struct {
		dma_addr_t probs_out;
		dma_addr_t probs_in;
		dma_addr_t color;
		dma_addr_t seg;
		dma_addr_t prev_seg;
	} addresses;

	/* Used only while prepare() owns the request controls. */
	const struct v4l2_ctrl_av1_sequence *seq;
	const struct v4l2_ctrl_av1_frame *frame;
	const struct v4l2_ctrl_av1_tile_group_entry *tile_group;
	const struct v4l2_ctrl_av1_film_grain *grain;
	unsigned int num_tiles;
	/* Ends exclude EXEC words; used only by the legacy transport adapter. */
	size_t segment_end[V4L2_AV1_MAX_TILE_COUNT + 1];
};

struct avd_av1_ctx {
	struct list_head refs;
	u64 prev_seq;
	u64 prev_timestamp;
	bool prev_updates_cdf;
	bool have_prev;
	unsigned long active_slots;
	struct avd_frame legacy_frame;
	struct avd_buf legacy_prog;
};

static void avd_av1_push(struct avd_av1_run *run, u32 inst)
{
	struct avd_frame *f = run->slot;

	if (f->prog_words >= f->prog->size / sizeof(__le32)) {
		run->error = -ENOSPC;
		return;
	}
	((__le32 *)f->prog->cpu)[f->prog_words++] = cpu_to_le32(inst);
}

static void avd_av1_push_address(struct avd_ctx *ctx, struct avd_av1_run *run,
			       dma_addr_t addr)
{
	if (ctx->dev->variant->quirks & AVD_QUIRK_LSR) {
		avd_av1_push(run, addr >> 8);
	} else {
		avd_av1_push(run, lower_32_bits(addr));
		avd_av1_push(run, upper_32_bits(addr));
	}
}

static void avd_av1_push_comp(struct avd_ctx *ctx, struct avd_av1_run *run,
			     dma_addr_t addr, const u32 offsets[4])
{
	unsigned int i;

	for (i = 0; i < 4; i++) {
		if (ctx->dev->variant->quirks & AVD_QUIRK_LSR)
			avd_av1_push(run, (addr + offsets[i]) >> 7);
		else
			avd_av1_push_address(ctx, run, addr + offsets[i]);
	}
}

static struct avd_av1_aux *avd_av1_aux_get(struct avd_av1_aux *aux)
{
	if (aux)
		refcount_inc(&aux->refs);
	return aux;
}

static void avd_av1_aux_put(struct avd_ctx *ctx, struct avd_av1_aux *aux)
{
	if (aux && refcount_dec_and_test(&aux->refs)) {
		avd_buf_free(ctx->dev, &aux->buf);
		kfree(aux);
	}
}

static struct avd_av1_aux *avd_av1_aux_alloc(struct avd_ctx *ctx, size_t size,
					   u64 producer, bool done)
{
	struct avd_av1_aux *aux;

	aux = kzalloc(sizeof(*aux), GFP_KERNEL);
	if (!aux)
		return NULL;
	if (avd_buf_alloc(ctx->dev, &aux->buf, size)) {
		kfree(aux);
		return NULL;
	}
	refcount_set(&aux->refs, 1);
	aux->producer = producer;
	aux->done = done;
	memset(aux->buf.cpu, 0, size);
	return aux;
}

static int avd_av1_aux_ready(struct avd_av1_aux *aux, enum avd_frame_dep dep)
{
	if (smp_load_acquire(&aux->done))
		return READ_ONCE(aux->status);
	return dep == AVD_DEP_PREV_DONE ? 0 : -EAGAIN;
}

static struct avd_av1_ref *avd_av1_find_ref(struct avd_ctx *ctx,
					 unsigned int index, u64 timestamp)
{
	struct avd_av1_ctx *av1 = ctx->priv;
	struct avd_av1_ref *ref;

	list_for_each_entry(ref, &av1->refs, node)
		if (ref->index == index && ref->timestamp == timestamp)
			return ref;
	return NULL;
}

/*
 * from verisilicon/rockchip_vpu981_hw_av1_dec.c
 */
static const short div_lut[DIV_LUT_NUM + 1] = {
	16384, 16320, 16257, 16194, 16132, 16070, 16009, 15948, 15888, 15828, 15768,
	15709, 15650, 15592, 15534, 15477, 15420, 15364, 15308, 15252, 15197, 15142,
	15087, 15033, 14980, 14926, 14873, 14821, 14769, 14717, 14665, 14614, 14564,
	14513, 14463, 14413, 14364, 14315, 14266, 14218, 14170, 14122, 14075, 14028,
	13981, 13935, 13888, 13843, 13797, 13752, 13707, 13662, 13618, 13574, 13530,
	13487, 13443, 13400, 13358, 13315, 13273, 13231, 13190, 13148, 13107, 13066,
	13026, 12985, 12945, 12906, 12866, 12827, 12788, 12749, 12710, 12672, 12633,
	12596, 12558, 12520, 12483, 12446, 12409, 12373, 12336, 12300, 12264, 12228,
	12193, 12157, 12122, 12087, 12053, 12018, 11984, 11950, 11916, 11882, 11848,
	11815, 11782, 11749, 11716, 11683, 11651, 11619, 11586, 11555, 11523, 11491,
	11460, 11429, 11398, 11367, 11336, 11305, 11275, 11245, 11215, 11185, 11155,
	11125, 11096, 11067, 11038, 11009, 10980, 10951, 10923, 10894, 10866, 10838,
	10810, 10782, 10755, 10727, 10700, 10673, 10645, 10618, 10592, 10565, 10538,
	10512, 10486, 10460, 10434, 10408, 10382, 10356, 10331, 10305, 10280, 10255,
	10230, 10205, 10180, 10156, 10131, 10107, 10082, 10058, 10034, 10010, 9986,
	9963,  9939,  9916,  9892,  9869,  9846,  9823,  9800,  9777,  9754,  9732,
	9709,  9687,  9664,  9642,  9620,  9598,  9576,  9554,  9533,  9511,  9489,
	9468,  9447,  9425,  9404,  9383,  9362,  9341,  9321,  9300,  9279,  9259,
	9239,  9218,  9198,  9178,  9158,  9138,  9118,  9098,  9079,  9059,  9039,
	9020,  9001,  8981,  8962,  8943,  8924,  8905,  8886,  8867,  8849,  8830,
	8812,  8793,  8775,  8756,  8738,  8720,  8702,  8684,  8666,  8648,  8630,
	8613,  8595,  8577,  8560,  8542,  8525,  8508,  8490,  8473,  8456,  8439,
	8422,  8405,  8389,  8372,  8355,  8339,  8322,  8306,  8289,  8273,  8257,
	8240,  8224,  8208,  8192,
};

static inline int avd_av1_dec_get_msb(u32 n)
{
	if (n == 0)
		return 0;
	return 31 ^ __builtin_clz(n);
}

static short avd_av1_dec_resolve_divisor_32(u32 d, short *shift)
{
	int f;
	u64 e;

	*shift = avd_av1_dec_get_msb(d);
	/* e is obtained from D after resetting the most significant 1 bit. */
	e = d - ((u32)1 << *shift);
	/* Get the most significant DIV_LUT_BITS (8) bits of e into f */
	if (*shift > DIV_LUT_BITS)
		f = AV1_DIV_ROUND_UP_POW2(e, *shift - DIV_LUT_BITS);
	else
		f = e << (DIV_LUT_BITS - *shift);
	if (f > DIV_LUT_NUM)
		return -1;
	*shift += DIV_LUT_PREC_BITS;
	/* Use f as lookup into the precomputed table of multipliers */
	return div_lut[f];
}

static void
avd_av1_dec_get_shear_params(const s32 *params, s64 *alpha,
					 s64 *beta, s64 *gamma, s64 *delta)
{
	const int *mat = params;
	short shift;
	short y;
	long long gv, dv;

	if (mat[2] <= 0)
		return;

	*alpha = clamp_val(mat[2] - (1 << WARPEDMODEL_PREC_BITS), S16_MIN, S16_MAX);
	*beta = clamp_val(mat[3], S16_MIN, S16_MAX);

	y = avd_av1_dec_resolve_divisor_32(abs(mat[2]), &shift) * (mat[2] < 0 ? -1 : 1);

	gv = ((long long)mat[4] * (1 << WARPEDMODEL_PREC_BITS)) * y;

	*gamma = clamp_val((int)AV1_DIV_ROUND_UP_POW2_SIGNED(gv, shift), S16_MIN, S16_MAX);

	dv = ((long long)mat[3] * mat[4]) * y;
	*delta = clamp_val(mat[5] -
		(int)AV1_DIV_ROUND_UP_POW2_SIGNED(dv, shift) - (1 << WARPEDMODEL_PREC_BITS),
		S16_MIN, S16_MAX);

	*alpha = AV1_DIV_ROUND_UP_POW2_SIGNED(*alpha, WARP_PARAM_REDUCE_BITS)
		 * (1 << WARP_PARAM_REDUCE_BITS);
	*beta = AV1_DIV_ROUND_UP_POW2_SIGNED(*beta, WARP_PARAM_REDUCE_BITS)
		* (1 << WARP_PARAM_REDUCE_BITS);
	*gamma = AV1_DIV_ROUND_UP_POW2_SIGNED(*gamma, WARP_PARAM_REDUCE_BITS)
		 * (1 << WARP_PARAM_REDUCE_BITS);
	*delta = AV1_DIV_ROUND_UP_POW2_SIGNED(*delta, WARP_PARAM_REDUCE_BITS)
		* (1 << WARP_PARAM_REDUCE_BITS);
}

static void set_refs(struct avd_ctx *ctx, struct avd_av1_run *run)
{
	const struct v4l2_ctrl_av1_frame *frame = run->frame;
	const struct v4l2_av1_global_motion *gm = &frame->global_motion;
	int i, ref_idx;
	struct avd_decoded_buffer *dst, *ref;
	bool intrabc = !!(frame->flags & V4L2_AV1_FRAME_FLAG_ALLOW_INTRABC);
	int height, width, x_scale, y_scale;
	height = frame->frame_height_minus_1 + 1;
	width = frame->frame_width_minus_1 + 1;
	dma_addr_t addr;
	s64 alpha = 0, beta = 0, gamma = 0, delta = 0;
	dst = vb2_to_avd_decoded_buf(&run->base.bufs.dst->vb2_buf);

	push(0, "ref_cnst0");
	pusha(run->bufs.mv_above_info.addr, "mv_above_info", 0);

	for (i = 0; i < 4; i++)
		push(0, "ref_cnst1");

	for (i = 0; i < (intrabc ? 1 : V4L2_AV1_REFS_PER_FRAME); i++) {
		if (intrabc) {
			ref = dst;
		} else {
			ref_idx = i + V4L2_AV1_REF_LAST_FRAME;
			ref = avd_get_ref_buf(
				ctx, &dst->base.vb,
				frame->reference_frame_ts
					[frame->ref_frame_idx[i]]);
		}
		addr = vb2_dma_contig_plane_dma_addr(&ref->base.vb.vb2_buf, 0) +
		       ref->comp.start_offset;

		if (!intrabc) {
			int shift =
				(gm->flags[ref_idx] &
				 V4L2_AV1_GLOBAL_MOTION_FLAG_IS_TRANSLATION) ?
					(WARPEDMODEL_PREC_BITS -
					 2) - (frame->flags & V4L2_AV1_FRAME_FLAG_ALLOW_HIGH_PRECISION_MV ?
						       1 :
						       0) :
					10;
			push(AV1_GM_TYPE(gm->type[ref_idx]) |
				     AV1_GM_PARAM0(gm->params[ref_idx][0] >>
						   shift) |
				     AV1_GM_PARAM1(gm->params[ref_idx][1] >>
						   shift),
			     "ref_gm_mv");
			push(AV1_GM_VALID(!(V4L2_AV1_GLOBAL_MOTION_IS_INVALID(
						    ref_idx) &
					    gm->invalid)) |
				     AV1_GM_PARAM0(AV1_DIV_ROUND_UP_POW2_SIGNED(
					     gm->params[ref_idx][2], 1)) |
				     AV1_GM_PARAM1(AV1_DIV_ROUND_UP_POW2_SIGNED(
					     gm->params[ref_idx][3], 1)),
			     "ref_gm_param");
			push(AV1_GM_PARAM1(gm->params[ref_idx][5] >> 1) |
				     AV1_GM_PARAM0(gm->params[ref_idx][4] >> 1),
			     "ref_gm_unk2");
			avd_av1_dec_get_shear_params(&gm->params[ref_idx][0],
						     &alpha, &beta, &gamma,
						     &delta);
			push(AV1_GM_PARAM_SHEAR0(beta) |
				     AV1_GM_PARAM_SHEAR1(alpha),
			     "ref_gm_beta_alpha");

			push(AV1_GM_PARAM_SHEAR0(delta) |
				     AV1_GM_PARAM_SHEAR1(gamma),
			     "ref_gm_delta_gamma");
		}
		push(AVD_REF_FLAG_CONST |
			     AV1_REF_ORDER_HINT(
				     intrabc ? 0 : frame->order_hints[ref_idx]),
		     "ref_flag");
		push(AVD_HDR_HEIGHT(ref->av1.height) |
			     AVD_HDR_WIDTH(ref->av1.width),
		     "ref_height_width");

		x_scale = ((ref->av1.upscaled_width << AV1_REF_SCALE_SHIFT) +
			   (width / 2)) /
			  width;
		y_scale = (((ref->av1.height + 1) << AV1_REF_SCALE_SHIFT) +
			   (height / 2)) /
			  height;
		push(AV1_REF_SCALE_X(x_scale) | AV1_REF_SCALE_Y(y_scale),
		     "ref_scale");

		avd_av1_push_comp(ctx, run, addr, ref->comp.offsets);
	}
}

static int avd_av1_get_dist(struct avd_av1_run *run, int a, int b)
{
	const struct v4l2_ctrl_av1_sequence *seq = run->seq;
	int bits = seq->order_hint_bits - 1;
	int diff, m;

	if (!seq->order_hint_bits)
		return 0;

	diff = a - b;
	m = 1 << bits;
	diff = (diff & (m - 1)) - (diff & m);

	return diff;
}

static bool is_valid_ref_frame(struct avd_ctx *ctx, struct avd_av1_run *run,
			       struct avd_decoded_buffer *dst, int mi_cols,
			       int mi_rows,
			       enum v4l2_av1_reference_frame ref_frame)
{
	const struct v4l2_ctrl_av1_frame *frame = run->frame;
	struct avd_decoded_buffer *ref = avd_get_ref_buf(
		ctx, &dst->base.vb,
		frame->reference_frame_ts
			[frame->ref_frame_idx[ref_frame -
					      V4L2_AV1_REF_LAST_FRAME]]);
	int ref_mi_cols = DIV_ROUND_UP(ref->av1.width + 1, 8);
	int ref_mi_rows = DIV_ROUND_UP(ref->av1.height + 1, 8);
	bool intra_only = ((ref->av1.frame_type == V4L2_AV1_KEY_FRAME) ||
			   (ref->av1.frame_type == V4L2_AV1_INTRA_ONLY_FRAME));

	return ref != dst && ref_mi_cols == mi_cols && ref_mi_rows == mi_rows &&
	       !intra_only;
}

static int avd_select_refs(struct avd_ctx *ctx, struct avd_av1_run *run,
			   u8 *selected_refs)
{
	const struct v4l2_ctrl_av1_frame *frame = run->frame;
	const struct v4l2_ctrl_av1_sequence *seq = run->seq;
	int ref_stamp = 2;
	struct avd_decoded_buffer *dst, *ref;
	bool use_last;
	int ref_num = 0;
	int order_hint = frame->order_hint;
	int mi_cols = DIV_ROUND_UP(frame->frame_width_minus_1 + 1, 8);
	int mi_rows = DIV_ROUND_UP(frame->frame_height_minus_1 + 1, 8);
	dst = vb2_to_avd_decoded_buf(&run->base.bufs.dst->vb2_buf);

	int last_ref_idx = frame->ref_frame_idx[V4L2_AV1_REF_INTRA_FRAME];
	if (seq->flags & (V4L2_AV1_SEQUENCE_FLAG_ENABLE_ORDER_HINT |
			  V4L2_AV1_SEQUENCE_FLAG_ENABLE_REF_FRAME_MVS)) {
		ref = avd_get_ref_buf(ctx, &dst->base.vb,
				      frame->reference_frame_ts[last_ref_idx]);
		if (dst != ref) {
			use_last =
				(ref->av1.order_hints[V4L2_AV1_REF_ALTREF_FRAME] !=
				 frame->order_hints[V4L2_AV1_REF_GOLDEN_FRAME]);
			if (use_last &&
			    is_valid_ref_frame(ctx, run, dst, mi_cols, mi_rows,
					       V4L2_AV1_REF_LAST_FRAME))
				selected_refs[ref_num++] =
					V4L2_AV1_REF_LAST_FRAME;
			ref_stamp--;
		}

		if (avd_av1_get_dist(
			    run, frame->order_hints[V4L2_AV1_REF_BWDREF_FRAME],
			    order_hint) > 0 &&
		    is_valid_ref_frame(ctx, run, dst, mi_cols, mi_rows,
				       V4L2_AV1_REF_BWDREF_FRAME)) {
			selected_refs[ref_num++] = V4L2_AV1_REF_BWDREF_FRAME;
			ref_stamp--;
		}

		if (avd_av1_get_dist(
			    run, frame->order_hints[V4L2_AV1_REF_ALTREF2_FRAME],
			    order_hint) > 0 &&
		    is_valid_ref_frame(ctx, run, dst, mi_cols, mi_rows,
				       V4L2_AV1_REF_ALTREF2_FRAME)) {
			selected_refs[ref_num++] = V4L2_AV1_REF_ALTREF2_FRAME;
			ref_stamp--;
		}

		if (ref_stamp >= 0 &&
		    avd_av1_get_dist(
			    run, frame->order_hints[V4L2_AV1_REF_ALTREF_FRAME],
			    order_hint) > 0 &&
		    is_valid_ref_frame(ctx, run, dst, mi_cols, mi_rows,
				       V4L2_AV1_REF_ALTREF_FRAME)) {
			selected_refs[ref_num++] = V4L2_AV1_REF_ALTREF_FRAME;
			ref_stamp--;
		}

		if (ref_stamp >= 0 &&
		    is_valid_ref_frame(ctx, run, dst, mi_cols, mi_rows,
				       V4L2_AV1_REF_LAST2_FRAME)) {
			selected_refs[ref_num++] = V4L2_AV1_REF_LAST2_FRAME;
			ref_stamp--;
		}
	}

	return ref_num;
}

static void set_ref_hints(struct avd_ctx *ctx, struct avd_av1_run *run,
			  u8 *selected_refs)
{
	const struct v4l2_ctrl_av1_frame *frame = run->frame;
	int i, ref_idx;
	struct avd_decoded_buffer *dst, *ref;
	dst = vb2_to_avd_decoded_buf(&run->base.bufs.dst->vb2_buf);

	for (i = 0; i < 3; i++) {
		if (selected_refs[i] < V4L2_AV1_REF_LAST_FRAME) {
			push(0, "");
			push(0, "");
		} else {
			ref_idx = frame->ref_frame_idx[selected_refs[i] -
						       V4L2_AV1_REF_LAST_FRAME];
			ref = avd_get_ref_buf(
				ctx, &dst->base.vb,
				frame->reference_frame_ts[ref_idx]);

			push(AV1_ORDER_HINT0(ref->av1.order_hints[3]) |
				     AV1_ORDER_HINT1(ref->av1.order_hints[2]) |
				     AV1_ORDER_HINT2(ref->av1.order_hints[1]) |
				     AV1_ORDER_HINT_REF(
					     (frame->flags &
					      V4L2_AV1_FRAME_FLAG_USE_REF_FRAME_MVS) ?
						     selected_refs[i] :
						     0),
			     "order_hints0");

			push(AV1_ORDER_HINT0(ref->av1.order_hints[7]) |
				     AV1_ORDER_HINT1(ref->av1.order_hints[6]) |
				     AV1_ORDER_HINT2(ref->av1.order_hints[5]) |
				     AV1_ORDER_HINT3(ref->av1.order_hints[4]),
			     "order_hints1");
		}
	}
}

static void set_ref_hdr(struct avd_ctx *ctx, struct avd_av1_run *run)
{
	const struct v4l2_ctrl_av1_frame *frame = run->frame;
	u8 ref_slots[7] = { 1, 2, 3, 4, 5, 6, 7 };
	int sign_bias = 0;
	int ref_slot = 0;
	int i, j;
	/* point to the first index where the frames equal */
	for (i = 0; i < 6; i++) {
		for (j = i + 1; j < 7; j++) {
			if (frame->reference_frame_ts[frame->ref_frame_idx[i]] ==
			    frame->reference_frame_ts[frame->ref_frame_idx[j]]) {
				ref_slots[j] = ref_slots[i];
				break;
			}
		}
	}
	for (i = 0; i < 7; i++) {
		sign_bias |= AV1_SIGN_BIAS(
			avd_av1_get_dist(
				run,
				frame->order_hints[i + V4L2_AV1_REF_LAST_FRAME],
				frame->order_hint) > 0,
			6 - i);

		ref_slot |= AV1_REF_SLOT(ref_slots[i], 6 - i);
	}
	push(ref_slot | sign_bias |
		     AV1_FLAGS_REFERENCE_SELECT(
			     frame->flags &
			     V4L2_AV1_FRAME_FLAG_REFERENCE_SELECT) |
		     AV1_FLAGS_INTERPOLATION_FILTER(
			     frame->interpolation_filter),
	     "reference_select");
}

/* 7.16. Upscaling process */
static void calc_upscale(int frame_width, int upscaled_width, int sub_x,
			 int *step_x, int *initial_subpel_x)
{
	int downscaled_plane_w, upscaled_plane_w, err;

	downscaled_plane_w = AV1_DIV_ROUND_UP_POW2(frame_width, sub_x);
	upscaled_plane_w = AV1_DIV_ROUND_UP_POW2(upscaled_width, sub_x);
	*step_x = ((downscaled_plane_w << SUPERRES_SCALE_BITS) +
		   (upscaled_plane_w / 2)) /
		  upscaled_plane_w;
	err = (upscaled_plane_w * *step_x) -
	      (downscaled_plane_w << SUPERRES_SCALE_BITS);
	*initial_subpel_x = (-((upscaled_plane_w - downscaled_plane_w)
			       << (SUPERRES_SCALE_BITS - 1)) +
			     upscaled_plane_w / 2) /
				    upscaled_plane_w +
			    (1 << (SUPERRES_EXTRA_BITS - 1)) - err / 2;
	*initial_subpel_x &= SUPERRES_SCALE_MASK;
}

static void set_header(struct avd_ctx *ctx, struct avd_av1_run *run)
{
	const struct v4l2_ctrl_av1_frame *frame = run->frame;
	const struct v4l2_ctrl_av1_sequence *seq = run->seq;
	const struct v4l2_av1_loop_restoration *lr = &frame->loop_restoration;
	const struct v4l2_av1_cdef *cdef = &frame->cdef;
	const struct v4l2_av1_segmentation *seg = &frame->segmentation;
	u32 bytesperline;
	int i, segid, segval, ref_idx;
	int step_x, initial_subpel_x;
	u8 selected_refs[3] = { 0, 0, 0 };
	bool coded_lossless = frame->tx_mode == V4L2_AV1_TX_MODE_ONLY_4X4;
	bool intrabc = !!(frame->flags & V4L2_AV1_FRAME_FLAG_ALLOW_INTRABC);
	bool intra_only = !!((frame->frame_type == V4L2_AV1_KEY_FRAME ||
			      frame->frame_type == V4L2_AV1_INTRA_ONLY_FRAME));
	bool cdef_enabled =
		!(!(seq->flags & V4L2_AV1_SEQUENCE_FLAG_ENABLE_CDEF) ||
		  frame->flags & V4L2_AV1_FRAME_FLAG_ALLOW_INTRABC ||
		  coded_lossless);

	struct avd_decoded_buffer *dst, *ref;
	dma_addr_t ref_addr;
	dst = vb2_to_avd_decoded_buf(&run->base.bufs.dst->vb2_buf);

	push(AVD_OP_HDR | AVD_OP_HDR_FLAG_DECOMP(1) |
		     AVD_OP_HDR_FLAG_INTRA(intra_only && !intrabc) |
		     AVD_OP_HDR_CONST | AVD_OP_HDR_FLAG_PIPE_STATE(1),
	     "op_hdr");

	push(AVD_HDR_CODEC_MODE(AVD_CODEC_AV1) |
		     AV1_CODEC_MODE_INTRABC(intrabc),
	     "codec");

	/*
	 * TODO: height_width_* are kinda weird
	 * on most samples this is true
	 */
	push(AVD_HDR_HEIGHT(frame->frame_height_minus_1) |
		     AVD_HDR_WIDTH(frame->frame_width_minus_1),
	     "height_width_1");
	push(0, "");
	push(AVD_HDR_HEIGHT(frame->frame_height_minus_1) |
		     AVD_HDR_WIDTH(frame->frame_width_minus_1),
	     "height_width_2");

	push(AVD_HDR_COMMON_CHROMA_FORMAT(1) |
		     AVD_HDR_COMMON_BIT_DEPTH_C(seq->bit_depth - 8) |
		     AVD_HDR_COMMON_BIT_DEPTH_L(seq->bit_depth - 8) |
		     AV1_HDR_JNT_COMP(seq->flags &
				      V4L2_AV1_SEQUENCE_FLAG_ENABLE_JNT_COMP) |
		     AV1_HDR_DUAL_FILTER(
			     seq->flags &
			     V4L2_AV1_SEQUENCE_FLAG_ENABLE_DUAL_FILTER) |
		     AV1_HDR_ORDER_HINT_BITS((seq->order_hint_bits - 1)) |
		     AV1_HDR_ORDER_HINT(
			     seq->flags &
			     V4L2_AV1_SEQUENCE_FLAG_ENABLE_ORDER_HINT) |
		     AV1_HDR_MASKED_COMPOUND(
			     seq->flags &
			     V4L2_AV1_SEQUENCE_FLAG_ENABLE_MASKED_COMPOUND) |
		     AV1_HDR_INTERINTRA_COMPOUND(
			     seq->flags &
			     V4L2_AV1_SEQUENCE_FLAG_ENABLE_INTERINTRA_COMPOUND) |
		     AV1_HDR_INTRA_EDGE_FILTER(
			     seq->flags &
			     V4L2_AV1_SEQUENCE_FLAG_ENABLE_INTRA_EDGE_FILTER) |
		     AV1_HDR_FILTER_INTRA(
			     seq->flags &
			     V4L2_AV1_SEQUENCE_FLAG_ENABLE_FILTER_INTRA) |
		     AV1_HDR_128X128_SUPERBLOCK(
			     seq->flags &
			     V4L2_AV1_SEQUENCE_FLAG_USE_128X128_SUPERBLOCK) |
		     AV1_HDR_SCREEN_CONTENT_TOOLS(
			     frame->flags &
			     V4L2_AV1_FRAME_FLAG_ALLOW_SCREEN_CONTENT_TOOLS),
	     "hdr_common");

	push(AV1_FLAGS_TX_MODE_LARGEST(frame->tx_mode ==
				       V4L2_AV1_TX_MODE_LARGEST) |
		     AV1_FLAGS_TX_MODE_SELECT(frame->tx_mode ==
					      V4L2_AV1_TX_MODE_SELECT) |
		     AV1_FLAGS_TX_REDUCED_SET(
			     frame->flags &
			     V4L2_AV1_FRAME_FLAG_REDUCED_TX_SET) |
		     AV1_FLAGS_ALLOW_WARPED_MOTION(
			     frame->flags &
			     V4L2_AV1_FRAME_FLAG_ALLOW_WARPED_MOTION) |
		     AV1_FLAGS_SKIP_MODE(
			     frame->flags &
			     V4L2_AV1_FRAME_FLAG_SKIP_MODE_PRESENT) |
		     AV1_FLAGS_MOTION_IS_SWITCHABLE(
			     frame->flags &
			     V4L2_AV1_FRAME_FLAG_IS_MOTION_MODE_SWITCHABLE) |
		     AV1_FLAGS_INTRABC(frame->flags &
				       V4L2_AV1_FRAME_FLAG_ALLOW_INTRABC) |
		     AV1_FLAGS_INTEGER_MV(
			     frame->flags &
			     V4L2_AV1_FRAME_FLAG_FORCE_INTEGER_MV) |
		     AV1_FLAGS_DISABLE_CDF_UPDATE(
			     frame->flags &
			     V4L2_AV1_FRAME_FLAG_DISABLE_CDF_UPDATE) |
		     AV1_FLAGS_TX_MODE_ONLY_4X4(frame->tx_mode ==
						V4L2_AV1_TX_MODE_ONLY_4X4) |
		     AV1_FLAGS_HIGH_PRECISION_MV(
			     frame->flags &
			     V4L2_AV1_FRAME_FLAG_ALLOW_HIGH_PRECISION_MV) |
		     AV1_FLAGS_REF_FRAME_MVS(
			     frame->flags &
			     V4L2_AV1_FRAME_FLAG_USE_REF_FRAME_MVS) |
		     AV1_FLAGS_SEG_TEMPORAL_UPDATE(
			     frame->segmentation.flags &
			     V4L2_AV1_SEGMENTATION_FLAG_TEMPORAL_UPDATE) |
		     AV1_FLAGS_SEG_UPDATE_MAP(
			     frame->segmentation.flags &
			     V4L2_AV1_SEGMENTATION_FLAG_UPDATE_MAP) |
		     AV1_FLAGS_SEG_ENABLED(frame->segmentation.flags &
					   V4L2_AV1_SEGMENTATION_FLAG_ENABLED) |
		     /* im not sure this is correct */
		     AV1_FLAGS_SEG_UPDATE_DATA(
			     frame->segmentation.flags &
			     V4L2_AV1_SEGMENTATION_FLAG_UPDATE_DATA) |
		     AV1_FLAGS_SKIP_MODE1(
			     frame->flags & V4L2_AV1_FRAME_FLAG_SKIP_MODE_ALLOWED ?
				     frame->skip_mode_frame[1] - 1 :
				     7) |
		     AV1_FLAGS_SKIP_MODE0(
			     frame->flags & V4L2_AV1_FRAME_FLAG_SKIP_MODE_ALLOWED ?
				     frame->skip_mode_frame[0] - 1 :
				     7) |
		     AV1_FLAGS_ORDER_HINT(frame->order_hint) |
		     AV1_FLAGS_INTRA(intra_only),
	     "av1_flags0");

	set_ref_hdr(ctx, run);

	for (segid = 0; segid < V4L2_AV1_MAX_SEGMENTS; segid++) {
#define SEG_FEAT_EN(feat) \
	(seg->feature_enabled[segid] & V4L2_AV1_SEGMENT_FEATURE_ENABLED(feat))
		/* what? why? */
		segval = AV1_SEG_UNK0(frame->tx_mode ==
				      V4L2_AV1_TX_MODE_ONLY_4X4);

		segval |= AV1_SEG_ALT_Q(
			seg->feature_data[segid][V4L2_AV1_SEG_LVL_ALT_Q]);
		segval |= AV1_SEG_ALT_Q_EN(SEG_FEAT_EN(V4L2_AV1_SEG_LVL_ALT_Q));
		segval |= AV1_SEG_REF_FRAME(
			seg->feature_data[segid][V4L2_AV1_SEG_LVL_REF_FRAME]);
		segval |= AV1_SEG_REF_FRAME_EN(
			SEG_FEAT_EN(V4L2_AV1_SEG_LVL_REF_FRAME));
		segval |= AV1_SEG_REF_SKIP_EN(
			SEG_FEAT_EN(V4L2_AV1_SEG_LVL_REF_SKIP));
		segval |= AV1_SEG_REF_GMV_EN(
			SEG_FEAT_EN(V4L2_AV1_SEG_LVL_REF_GLOBALMV));
		push(segval, "seg_0");
		segval = 0;
		segval |= AV1_SEG_LF_V(
			seg->feature_data[segid][V4L2_AV1_SEG_LVL_ALT_LF_V]);
		segval |=
			AV1_SEG_LF_V_EN(SEG_FEAT_EN(V4L2_AV1_SEG_LVL_ALT_LF_V));
		segval |= AV1_SEG_LF_U(
			seg->feature_data[segid][V4L2_AV1_SEG_LVL_ALT_LF_U]);
		segval |=
			AV1_SEG_LF_U_EN(SEG_FEAT_EN(V4L2_AV1_SEG_LVL_ALT_LF_U));
		segval |= AV1_SEG_LF_Y_H(
			seg->feature_data[segid][V4L2_AV1_SEG_LVL_ALT_LF_Y_H]);
		segval |= AV1_SEG_LF_Y_H_EN(
			SEG_FEAT_EN(V4L2_AV1_SEG_LVL_ALT_LF_Y_H));
		segval |= AV1_SEG_LF_Y_V(
			seg->feature_data[segid][V4L2_AV1_SEG_LVL_ALT_LF_Y_V]);
		segval |= AV1_SEG_LF_Y_V_EN(
			SEG_FEAT_EN(V4L2_AV1_SEG_LVL_ALT_LF_Y_V));
		push(segval, "seg_1");
#undef SEG_FEAT_EN
	}

	push(AVD_HDR_FEAT_VP9, "feat_en");

	avd_select_refs(ctx, run, selected_refs);
	set_ref_hints(ctx, run, selected_refs);

	push(0, "");
	push(0, "");

	pusha(run->addresses.probs_out, "probs_out", 0);
	pusha(run->addresses.probs_in, "probs", 1);
	pusha(run->bufs.above_info.addr, "col", 0);
	pusha(run->addresses.seg, "seg", 0);
	pusha(run->addresses.prev_seg, "prev_seg", 1);
	pusha(run->addresses.color, "w_color", 0);

	for (i = 0; i < 3; i++) {
		if (selected_refs[i] < V4L2_AV1_REF_LAST_FRAME) {
			pusha((dma_addr_t)0, "", 0);
		} else {
			ref_idx = frame->ref_frame_idx[selected_refs[i] -
						       V4L2_AV1_REF_LAST_FRAME];
			ref = avd_get_ref_buf(
				ctx, &dst->base.vb,
				frame->reference_frame_ts[ref_idx]);
			ref_addr =
				vb2_dma_contig_plane_dma_addr(
					&ref->base.vb.vb2_buf, 0) +
				AVD_AV1_COLOR_OFFSET(
					ref->base.vb.vb2_buf.planes[0].length,
					ref->av1.color_size);

			pusha(ref_addr, "r_color", ref_idx);
		}
	}

	push(AV1_QP_PRESENT(frame->quantization.flags &
			    V4L2_AV1_QUANTIZATION_FLAG_DELTA_Q_PRESENT) |
		     AV1_QP_DELTA_RES(frame->quantization.delta_q_res) |
		     AV1_QP_BASE_IDX(frame->quantization.base_q_idx) |
		     AV1_QP_Y_DC(frame->quantization.delta_q_y_dc) |
		     AV1_QP_U_DC(frame->quantization.delta_q_u_dc) |
		     AV1_QP_U_AC(frame->quantization.delta_q_u_ac),
	     "qp_base_q_idx");

	push(AV1_QP_V_DC(frame->quantization.delta_q_v_dc) |
		     AV1_QP_V_AC(frame->quantization.delta_q_v_ac) |
		     AV1_QP_QMATRIX(frame->quantization.flags &
				    V4L2_AV1_QUANTIZATION_FLAG_USING_QMATRIX) |
		     AV1_QP_QM_Y(frame->quantization.qm_y) |
		     AV1_QP_QM_U(frame->quantization.qm_u) |
		     AV1_QP_QM_V(frame->quantization.qm_v),
	     "qp_qm");

	push(AV1_LF_DELTA_ENABLED(frame->loop_filter.flags &
				  V4L2_AV1_LOOP_FILTER_FLAG_DELTA_ENABLED) |
		     AV1_LF_DELTA_PRESENT(
			     frame->loop_filter.flags &
			     V4L2_AV1_LOOP_FILTER_FLAG_DELTA_LF_PRESENT) |
		     AV1_LF_DELTA_MULTI(
			     frame->loop_filter.flags &
			     V4L2_AV1_LOOP_FILTER_FLAG_DELTA_LF_MULTI) |
		     AV1_LF_DELTA_RES(frame->loop_filter.delta_lf_res) |
		     AV1_LF_SHARPNESS(frame->loop_filter.sharpness) |
		     AV1_LF_LV0(frame->loop_filter.level[0]) |
		     AV1_LF_LV1(frame->loop_filter.level[1]) |
		     AV1_LF_LV2(frame->loop_filter.level[2]) |
		     AV1_LF_LV3(frame->loop_filter.level[3]),
	     "lf_update");

	push(AV1_LF_REF0(frame->loop_filter.ref_deltas[0]) |
		     AV1_LF_REF1(frame->loop_filter.ref_deltas[1]) |
		     AV1_LF_REF2(frame->loop_filter.ref_deltas[2]) |
		     AV1_LF_REF3(frame->loop_filter.ref_deltas[3]),
	     "lf_sharp_ref_deltas");

	push(AV1_LF_REF0(frame->loop_filter.ref_deltas[4]) |
		     AV1_LF_REF1(frame->loop_filter.ref_deltas[5]) |
		     AV1_LF_REF2(frame->loop_filter.ref_deltas[6]) |
		     AV1_LF_REF3(frame->loop_filter.ref_deltas[7]),
	     "lf_ref_deltas");

	push(AV1_LF_MODE0(frame->loop_filter.mode_deltas[0]) |
		     AV1_LF_MODE1(frame->loop_filter.mode_deltas[1]),
	     "lf_mode");

#define AVD_AV1_SEC_FIXUP(i) ((i) == 4 ? 3 : i)

#define AV1_CDEF_PACK_ONE(i)                                          \
	(AV1_CDEF_Y_PRI(cdef->y_pri_strength[i]) |                    \
	 AV1_CDEF_Y_SEC(AVD_AV1_SEC_FIXUP(cdef->y_sec_strength[i])) | \
	 AV1_CDEF_UV_PRI(cdef->uv_pri_strength[i]) |                  \
	 AV1_CDEF_UV_SEC(AVD_AV1_SEC_FIXUP(cdef->uv_sec_strength[i])))

#define AV1_CDEF_PACK(i)                            \
	AV1_CDEF_HI(AV1_CDEF_PACK_ONE(((i) * 2))) | \
		AV1_CDEF_LO(AV1_CDEF_PACK_ONE(((i) * 2) + 1))
	push(AV1_CDEF_EN(cdef_enabled) | AV1_CDEF_BITS(frame->cdef.bits) |
		     AV1_CDEF_DAMPING(frame->cdef.damping_minus_3) |
		     AV1_CDEF_PACK(0),
	     "cdef0");
	push(AV1_CDEF_PACK(1), "cdef1");
	push(AV1_CDEF_PACK(2), "cdef2");
	push(AV1_CDEF_PACK(3), "cdef3");
#undef AV1_CDEF_PACK_ONE
#undef AV1_CDEF_PACK

	push(AV1_SUPERRES_USE(frame->flags & V4L2_AV1_FRAME_FLAG_USE_SUPERRES) |
		     AV1_SUPERRES_DENOM(
			     frame->flags & V4L2_AV1_FRAME_FLAG_USE_SUPERRES ?
				     (frame->superres_denom - 1) :
				     0) |
		     AV1_SUPERRES_UPSCALE(frame->upscaled_width - 1),
	     "superres");

	calc_upscale(frame->frame_width_minus_1 + 1, frame->upscaled_width, 0,
		     &step_x, &initial_subpel_x);
	push(AV1_UPSCALE_UPSCX(initial_subpel_x) | AV1_UPSCALE_STEPX(step_x),
	     "upscale_p0");
	calc_upscale(frame->frame_width_minus_1 + 1, frame->upscaled_width,
		     !!(seq->flags & V4L2_AV1_SEQUENCE_FLAG_SUBSAMPLING_X),
		     &step_x, &initial_subpel_x);
	push(AV1_UPSCALE_UPSCX(initial_subpel_x) | AV1_UPSCALE_STEPX(step_x),
	     "upscale_p1");

	u8 restoration_unit_size[V4L2_AV1_NUM_PLANES_MAX] = { 3, 3, 3 };

	if (lr->flags & V4L2_AV1_LOOP_RESTORATION_FLAG_USES_LR) {
		restoration_unit_size[0] = 1 + lr->lr_unit_shift;
		restoration_unit_size[1] =
			1 + lr->lr_unit_shift - lr->lr_uv_shift;
		restoration_unit_size[2] =
			1 + lr->lr_unit_shift - lr->lr_uv_shift;
	}

	push(AV1_LR_TYPE0(frame->loop_restoration.frame_restoration_type[0]) |
		     AV1_LR_TYPE1(
			     frame->loop_restoration.frame_restoration_type[1]) |
		     AV1_LR_TYPE2(
			     frame->loop_restoration.frame_restoration_type[2]) |
		     AV1_LR_UNIT0(restoration_unit_size[0]) |
		     AV1_LR_UNIT1(restoration_unit_size[1]) |
		     AV1_LR_UNIT2(restoration_unit_size[2]),
	     "lr");

	push(0, "");
	push(0, "");
	pusha(run->bufs.pipe_state.addr, "pipe_state", 0);
	pusha(run->bufs.ip_above.addr, "ip_above", 0);
	pusha(run->bufs.lf_above.addr, "lf_above", 2);
	pusha(run->bufs.lf_above_info.addr, "lf_above_info", 3);
	pusha(run->bufs.lf_left.addr, "lf_left", 0);
	pusha(run->bufs.lf_left_info.addr, "lf_left_info", 1);
	/* make it obvious if its used */
	pusha((dma_addr_t)0xa0a0a0a0a0a0, "", 0);
	pusha(run->bufs.rf_above_info.addr, "rf_above_info", 0);
	pusha(run->bufs.sr_left.addr, "sr_left", 2);
	pusha(run->bufs.rf_left_info.addr, "rf_left_info", 1);
	pusha(run->bufs.rf_left.addr, "rf_left", 3);

	push(0, "mark_section");

	avd_av1_push_comp(ctx, run, run->base.comp_out, ctx->comp.offsets);

	pusha((u64)0, "packed_fmt_scratch", 0);

	/* ignored if decomp is disabled */
	bytesperline = ctx->decoded_fmt.fmt.pix_mp.plane_fmt[0].bytesperline;
	pusha(run->base.y_out, "y", 0);
	push(bytesperline, "bytesperline_y");
	pusha(run->base.uv_out, "uv", 0);
	push(bytesperline, "bytesperline_uv");

	push(0, "mark_section");

	push(AVD_HDR_HEIGHT(frame->render_height_minus_1 + 1) |
		     AVD_HDR_WIDTH(frame->render_width_minus_1 + 1),
	     "height_width_3");
	if (!intra_only || intrabc)
		set_refs(ctx, run);
}

static void set_tiles(struct avd_ctx *ctx, struct avd_av1_run *run)
{
	const struct v4l2_ctrl_av1_frame *frame = run->frame;
	const struct v4l2_ctrl_av1_sequence *seq = run->seq;
	const struct v4l2_ctrl_av1_tile_group_entry *tile_group;
	const struct v4l2_av1_tile_info *tile_info = &frame->tile_info;
	int row, col, sb_row, sb_col, tile_id;
	int sb_shift =
		seq->flags & V4L2_AV1_SEQUENCE_FLAG_USE_128X128_SUPERBLOCK ? 5 :
									     4;


	for (row = 0; row < tile_info->tile_rows; row++) {
		for (col = 0; col < tile_info->tile_cols; col++) {
			tile_id = row * tile_info->tile_cols + col;
			tile_group = &run->tile_group[tile_id];

			push(AVD_OP_CODED_DATA |
				     AVD_OP_CODED_DATA_ADDR(
					     (run->base.coded_in +
					      tile_group->tile_offset) >>
					     32),
			     "tile_start");
			push((u32)((run->base.coded_in +
				    tile_group->tile_offset) &
				   0xffffffff),
			     "tile_addr");
			push(tile_group->tile_size, "tile_size");

			sb_row =
				tile_info->mi_row_starts[tile_group->tile_row] >>
				sb_shift;
			sb_col =
				tile_info->mi_col_starts[tile_group->tile_col] >>
				sb_shift;
			push(AVD_OP_SL_DIM_START |
				     AVD_OP_SL_DIM_START_Y(sb_row) |
				     AVD_OP_SL_DIM_START_X(sb_col),
			     "tile_op_start");

			push(AVD_SL_DIM_END_ROW(
				     !(frame->flags &
				       V4L2_AV1_FRAME_FLAG_DISABLE_FRAME_END_UPDATE_CDF) &&
						     tile_id ==
							     tile_info->context_update_tile_id ?
					     8 :
					     0) |
				     AVD_SL_DIM_END_COL(col) |
				     AVD_SL_DIM_END_Y(
					     sb_row +
					     tile_info->height_in_sbs_minus_1
						     [tile_group->tile_row]) |
				     AVD_SL_DIM_END_X(
					     sb_col +
					     tile_info->width_in_sbs_minus_1
						     [tile_group->tile_col]),
			     "tile_op_end");
			run->segment_end[tile_id + 1] = run->slot->prog_words;
			push(AVD_OP_EXEC | AVD_OP_EXEC_FLAG_END(
				tile_id + 1 == run->num_tiles), "tile_exec");
		}
	}
}

/* No CPU reads from hardware-owned capture tails during prepare(). */
static int avd_av1_set_aux(struct avd_ctx *ctx, struct avd_av1_run *run)
{
	struct avd_av1_ctx *av1 = ctx->priv;
	const struct v4l2_ctrl_av1_frame *frame = run->frame;
	struct avd_av1_ref *ref = NULL;
	struct avd_decoded_buffer *dst, *primary;
	bool defaults = frame->primary_ref_frame == AV1_PRIMARY_REF_NONE ||
		frame->frame_type == V4L2_AV1_KEY_FRAME ||
		frame->frame_type == V4L2_AV1_INTRA_ONLY_FRAME ||
		(frame->flags & V4L2_AV1_FRAME_FLAG_ERROR_RESILIENT_MODE);
	bool seg_enabled = frame->segmentation.flags &
		V4L2_AV1_SEGMENTATION_FLAG_ENABLED;
	u64 timestamp = 0;
	int ret;

	dst = vb2_to_avd_decoded_buf(&run->base.bufs.dst->vb2_buf);
	if (!defaults) {
		timestamp = frame->reference_frame_ts[
			frame->ref_frame_idx[frame->primary_ref_frame]];
		primary = avd_get_ref_buf(ctx, &dst->base.vb, timestamp);
		if (primary == dst)
			return -EINVAL;
		ref = avd_av1_find_ref(ctx, primary->base.vb.vb2_buf.index,
				       timestamp);
		if (!ref)
			return -EINVAL;
		run->cdf_in = avd_av1_aux_get(ref->cdf);
	} else {
		run->cdf_in = avd_av1_aux_alloc(ctx, AVD_CDFS_SIZE, 0, true);
		if (!run->cdf_in)
			return -ENOMEM;
		avd_av1_default_coeff_probs(frame->quantization.base_q_idx,
					    run->cdf_in->buf.cpu);
		avd_av1_set_default_cdfs(run->cdf_in->buf.cpu);
	}

	run->slot->dep = avd_av1_cdf_dep(!defaults, timestamp,
		av1->have_prev, av1->prev_timestamp, av1->prev_updates_cdf) ?
		AVD_DEP_PREV_DONE : AVD_DEP_NONE;
	/* PREV_DONE drains earlier frames too, but do not invent a dependency
	 * for a different, unfinished CDF producer. The queue must retry it. */
	ret = avd_av1_aux_ready(run->cdf_in, run->slot->dep);
	if (ret)
		return ret;

	run->cdf_written = !(frame->flags &
		V4L2_AV1_FRAME_FLAG_DISABLE_FRAME_END_UPDATE_CDF);
	if (run->cdf_written)
		run->cdf_out = avd_av1_aux_alloc(ctx, AVD_CDFS_SIZE,
						 run->slot->seq, false);
	else
		run->cdf_out = avd_av1_aux_get(run->cdf_in);
	if (!run->cdf_out)
		return -ENOMEM;

	if (ref && seg_enabled &&
	    DIV_ROUND_UP(ref->width + 1, 8) ==
		DIV_ROUND_UP(frame->frame_width_minus_1 + 1, 8) &&
	    DIV_ROUND_UP(ref->height + 1, 8) ==
		DIV_ROUND_UP(frame->frame_height_minus_1 + 1, 8)) {
		run->seg_in = avd_av1_aux_get(ref->seg);
		/* The two segmentation addresses let hardware read the previous
		 * map directly. No speculative CPU copy of an unfinished map. */
		ret = avd_av1_aux_ready(run->seg_in, run->slot->dep);
		if (ret)
			return ret;
	} else {
		run->seg_in = avd_av1_aux_alloc(ctx, dst->av1.seg_size, 0, true);
		if (!run->seg_in)
			return -ENOMEM;
	}
	run->seg_written = seg_enabled && (frame->segmentation.flags &
		V4L2_AV1_SEGMENTATION_FLAG_UPDATE_MAP);
	if (run->seg_written)
		run->seg_out = avd_av1_aux_alloc(ctx, dst->av1.seg_size,
						 run->slot->seq, false);
	else
		run->seg_out = avd_av1_aux_get(run->seg_in);
	if (!run->seg_out)
		return -ENOMEM;

	run->addresses.probs_in = run->cdf_in->buf.addr;
	run->addresses.probs_out = run->cdf_out->buf.addr;
	run->addresses.prev_seg = run->seg_in->buf.addr;
	run->addresses.seg = run->seg_out->buf.addr;
	return 0;
}

static int avd_av1_seg_size(int w, int h)
{
	/* 3 bits per 4x4 block, in rows of 128x128 superblocks (64x64 fit too) */
	return ALIGN(ALIGN(w, 128) * ALIGN(h, 128) / 128 * 3, AVD_ALIGN);
}

static int avd_color_size(int h, int w)
{
	/*
	 * in reality its dependent on quality
	 * 64 with lower quality
	 */
	return ALIGN(w, 128) * ALIGN(h, 128) / 16;
}

static void update_dec_buf_info(struct avd_decoded_buffer *buf,
				const struct v4l2_ctrl_av1_sequence *seq,
				const struct v4l2_ctrl_av1_frame *frame)
{
	int i;
	buf->av1.width = frame->frame_width_minus_1;
	buf->av1.height = frame->frame_height_minus_1;
	buf->av1.upscaled_width = frame->upscaled_width;
	buf->av1.bit_depth = seq->bit_depth;
	buf->av1.frame_type = frame->frame_type;
	buf->av1.intrabc = frame->flags & V4L2_AV1_FRAME_FLAG_ALLOW_INTRABC;

	buf->av1.color_size = avd_color_size(frame->frame_width_minus_1 + 1,
					     frame->frame_height_minus_1 + 1);
	buf->av1.seg_size = avd_av1_seg_size(frame->frame_width_minus_1 + 1,
					     frame->frame_height_minus_1 + 1);

	for (i = 0; i < V4L2_AV1_TOTAL_REFS_PER_FRAME; i++)
		buf->av1.order_hints[i] = frame->order_hints[i];

	for (i = 0; i < V4L2_AV1_REFS_PER_FRAME; i++)
		buf->av1.ref_frame_idx[i] = frame->ref_frame_idx[i];
}

static int avd_av1_controls(struct avd_ctx *ctx, struct avd_av1_run *run)
{
	struct v4l2_ctrl *ctrl;
	int dst_len, tlb_len;

	ctrl = v4l2_ctrl_find(&ctx->ctrl_hdl, V4L2_CID_STATELESS_AV1_SEQUENCE);
	if (WARN_ON(!ctrl))
		return -EINVAL;
	run->seq = ctrl->p_cur.p;

	ctrl = v4l2_ctrl_find(&ctx->ctrl_hdl, V4L2_CID_STATELESS_AV1_FRAME);
	if (WARN_ON(!ctrl))
		return -EINVAL;
	run->frame = ctrl->p_cur.p;

	/* The map, CDFs and color data sit at the buffer's end, sized by the frame. */
	if (run->frame->frame_width_minus_1 + 1 > ctx->decoded_fmt.fmt.pix_mp.width ||
	    run->frame->frame_height_minus_1 + 1 > ctx->decoded_fmt.fmt.pix_mp.height)
		return -EINVAL;

	/*
	 * The CDF and segmentation map loads index the reference arrays with
	 * these, and the control framework does not range-check them.
	 */
	if (run->frame->primary_ref_frame > AV1_PRIMARY_REF_NONE)
		return -EINVAL;
	if (run->frame->primary_ref_frame != AV1_PRIMARY_REF_NONE) {
		s8 idx = run->frame->ref_frame_idx[run->frame->primary_ref_frame];

		if (idx < 0 || idx >= V4L2_AV1_TOTAL_REFS_PER_FRAME)
			return -EINVAL;
	}

	ctrl = v4l2_ctrl_find(&ctx->ctrl_hdl,
			      V4L2_CID_STATELESS_AV1_TILE_GROUP_ENTRY);
	if (WARN_ON(!ctrl))
		return -EINVAL;
	run->tile_group = ctrl->p_cur.p;
	run->num_tiles = ctrl->elems;

	ctrl = v4l2_ctrl_find(&ctx->ctrl_hdl,
			      V4L2_CID_STATELESS_AV1_FILM_GRAIN);
	if (WARN_ON(!ctrl))
		return -EINVAL;
	run->grain = ctrl->p_cur.p;

	dst_len = run->base.bufs.dst->vb2_buf.planes[0].length;
	tlb_len = avd_color_size(run->frame->frame_width_minus_1 + 1,
				 run->frame->frame_height_minus_1 + 1);

	run->addresses.color =
		run->base.y_out + AVD_AV1_COLOR_OFFSET(dst_len, tlb_len);

	run->addresses.probs_out =
		run->base.y_out + AVD_AV1_CDFS_OFFSET(dst_len, tlb_len);

	run->addresses.seg =
		run->base.y_out +
		AVD_AV1_SEG_OFFSET(dst_len, tlb_len,
				   avd_av1_seg_size(run->frame->frame_width_minus_1 + 1,
						    run->frame->frame_height_minus_1 + 1));
	return 0;
}

static int avd_av1_alloc_work_bufs(struct avd_ctx *ctx, struct avd_av1_run *run)
{
	struct avd_dev *avd = ctx->dev;
	const struct v4l2_ctrl_av1_frame *frame = run->frame;
	const struct v4l2_ctrl_av1_sequence *seq = run->seq;
	const struct v4l2_av1_tile_info *tile_info = &frame->tile_info;
	int ret, max_sb_col = 0, max_sb_row = 0, i, sb_cols1 = 0, sb_cols2 = 0,
		 sb;
	int sb_shift =
		seq->flags & V4L2_AV1_SEQUENCE_FLAG_USE_128X128_SUPERBLOCK ? 1 :
									     0;
	int w = frame->frame_width_minus_1 + 1;
	int bit_depth = seq->bit_depth;

	for (i = 0; i < tile_info->tile_cols; i++) {
		sb = tile_info->width_in_sbs_minus_1[i] + 1;
		max_sb_col = sb > max_sb_col ? sb : max_sb_col;
		sb_cols1 += ALIGN((sb << sb_shift) * 44, 128);
		/* wrong alignment? */
		sb_cols2 += ALIGN((sb << sb_shift) * 12, 128);
	}

	max_sb_col <<= sb_shift;

	ret = avd_buf_alloc(avd, &run->bufs.mv_above_info, max_sb_col * 80);
	if (ret)
		return ret;

	/* first frame this is always much smaller */
	ret = avd_buf_alloc(avd, &run->bufs.above_info, max_sb_col * 400);
	if (ret)
		return ret;

	ret = avd_buf_alloc(avd, &run->bufs.lf_above_info, sb_cols1);
	if (ret)
		return ret;

	ret = avd_buf_alloc(avd, &run->bufs.ip_above,
			    max_sb_col * bit_depth * 22);
	if (ret)
		return ret;

	ret = avd_buf_alloc(avd, &run->bufs.lf_above,
			    ALIGN(ALIGN(w, 8) * bit_depth * 2 +
					  tile_info->tile_cols * bit_depth * 16,
				  128));
	if (ret)
		return ret;

	ret = avd_buf_alloc(avd, &run->bufs.rf_above_info, sb_cols2);
	if (ret)
		return ret;

	/* this is not a mistake, but im not sure why */
	if (tile_info->tile_cols > 1) {
		for (i = 0; i < tile_info->tile_rows; i++) {
			sb = tile_info->height_in_sbs_minus_1[i] + 1;
			max_sb_row = sb > max_sb_row ? sb : max_sb_row;
		}

		max_sb_row = max_sb_row << sb_shift;

		ret = avd_buf_alloc(avd, &run->bufs.lf_left_info,
				    max_sb_row * 72);
		if (ret)
			return ret;

		/*
		 * they all share the pattern of max_sb_row * something where
		 * something is a block of luma and chroma aligned to
		 * something. Depending on the height, the last
		 * (or second last??) block has a different size from the
		 * rest.
		 *
		 * I counted blocks as luma or chroma with padding until it
		 * changes to the other
		 */
		ret = avd_buf_alloc(avd, &run->bufs.lf_left,
				    (max_sb_row * 72 + 1) * 2 * bit_depth);
		if (ret)
			return ret;

		ret = avd_buf_alloc(avd, &run->bufs.sr_left,
				    max_sb_row * bit_depth * (53 + 52));
		if (ret)
			return ret;

		ret = avd_buf_alloc(
			avd, &run->bufs.rf_left,
			max_sb_row * bit_depth *
				(192 + 96 + (bit_depth > 8 ? 12 : 0)));
		if (ret)
			return ret;

		ret = avd_buf_alloc(avd, &run->bufs.rf_left_info,
				    max_sb_row * 24);
		if (ret)
			return ret;
	}

	return 0;
}

static void avd_av1_release_run(struct avd_ctx *ctx, struct avd_av1_run *run)
{
	avd_buf_free(ctx->dev, &run->bufs.pipe_state);
	avd_buf_free(ctx->dev, &run->bufs.above_info);
	avd_buf_free(ctx->dev, &run->bufs.rf_above_info);
	avd_buf_free(ctx->dev, &run->bufs.ip_above);
	avd_buf_free(ctx->dev, &run->bufs.lf_above);
	avd_buf_free(ctx->dev, &run->bufs.lf_above_info);
	avd_buf_free(ctx->dev, &run->bufs.lf_left);
	avd_buf_free(ctx->dev, &run->bufs.lf_left_info);
	avd_buf_free(ctx->dev, &run->bufs.sr_left);
	avd_buf_free(ctx->dev, &run->bufs.rf_left);
	avd_buf_free(ctx->dev, &run->bufs.rf_left_info);
	avd_buf_free(ctx->dev, &run->bufs.mv_above_info);
	avd_av1_aux_put(ctx, run->cdf_in);
	avd_av1_aux_put(ctx, run->cdf_out);
	avd_av1_aux_put(ctx, run->seg_in);
	avd_av1_aux_put(ctx, run->seg_out);
	kfree(run);
}

static int avd_av1_publish_ref(struct avd_ctx *ctx, struct avd_av1_run *run)
{
	struct avd_av1_ctx *av1 = ctx->priv;
	struct avd_av1_ref *ref, *old = NULL;
	unsigned int index = run->base.bufs.dst->vb2_buf.index;

	list_for_each_entry(ref, &av1->refs, node) {
		if (ref->index == index) {
			old = ref;
			break;
		}
	}
	ref = old;
	if (!ref) {
		ref = kzalloc(sizeof(*ref), GFP_KERNEL);
		if (!ref)
			return -ENOMEM;
		ref->index = index;
		list_add_tail(&ref->node, &av1->refs);
	}
	avd_av1_aux_put(ctx, ref->cdf);
	avd_av1_aux_put(ctx, ref->seg);
	ref->cdf = avd_av1_aux_get(run->cdf_out);
	ref->seg = avd_av1_aux_get(run->seg_out);
	/* Core copies source metadata to CAPTURE after prepare returns. */
	ref->timestamp = run->base.bufs.src->vb2_buf.timestamp;
	ref->width = run->frame->frame_width_minus_1;
	ref->height = run->frame->frame_height_minus_1;
	av1->prev_timestamp = ref->timestamp;
	av1->prev_seq = run->slot->seq;
	av1->prev_updates_cdf = run->cdf_written;
	av1->have_prev = true;
	return 0;
}

static int avd_av1_prepare(struct avd_ctx *ctx, struct avd_frame *f)
{
	struct avd_av1_ctx *av1 = ctx->priv;
	struct avd_av1_run *run;
	struct avd_decoded_buffer *dst;
	unsigned int i;
	int ret;

	if (!f->src || !f->dst || !f->prog || !f->prog->cpu ||
	    f->idx >= AVD_MAX_FRAMES || f->codec_priv)
		return -EINVAL;
	if (test_and_set_bit(f->idx, &av1->active_slots))
		return -EBUSY;
	run = kzalloc(sizeof(*run), GFP_KERNEL);
	if (!run) {
		clear_bit(f->idx, &av1->active_slots);
		return -ENOMEM;
	}
	run->slot = f;
	run->base.bufs.src = f->src;
	run->base.bufs.dst = f->dst;
	run->base.coded_in = vb2_dma_contig_plane_dma_addr(&f->src->vb2_buf, 0);
	run->base.y_out = vb2_dma_contig_plane_dma_addr(&f->dst->vb2_buf, 0);
	run->base.uv_out = run->base.y_out +
		ctx->decoded_fmt.fmt.pix_mp.plane_fmt[0].bytesperline *
		ctx->decoded_fmt.fmt.pix_mp.height;
	run->base.comp_out = run->base.y_out + ctx->comp.start_offset;
	f->prog_words = 0;
	f->dep = AVD_DEP_NONE;
	ret = avd_av1_controls(ctx, run);
	if (ret)
		goto fail;
	ret = avd_av1_validate_tiles(run->frame, run->tile_group,
				     run->num_tiles,
				     vb2_get_plane_payload(&f->src->vb2_buf, 0));
	if (ret)
		goto fail;
	/* The control is a fixed-capacity array; only the tile grid is work. */
	run->num_tiles = run->frame->tile_info.tile_cols *
		run->frame->tile_info.tile_rows;
	/* Preserve the existing open decoder's grain-less output fallback. */
	for (i = 0; i < V4L2_AV1_REFS_PER_FRAME; i++) {
		if (run->frame->ref_frame_idx[i] < 0 ||
		    run->frame->ref_frame_idx[i] >= V4L2_AV1_TOTAL_REFS_PER_FRAME) {
			ret = -EINVAL;
			goto fail;
		}
	}
	/* Borage syntax/DVA mappings remain [C]; never submit invented bytes. */
	if (f->fp) {
		ret = avd_av1_pack_fp(ctx, f, run->seq, run->frame,
				      run->tile_group, run->grain);
		if (ret)
			goto fail;
	}
	ret = avd_av1_alloc_work_bufs(ctx, run);
	if (ret)
		goto fail;
	ret = avd_buf_alloc(ctx->dev, &run->bufs.pipe_state, 0x200);
	if (ret)
		goto fail;
	dst = vb2_to_avd_decoded_buf(&f->dst->vb2_buf);
	update_dec_buf_info(dst, run->seq, run->frame);
	dst->comp = ctx->comp;
	ret = avd_av1_set_aux(ctx, run);
	if (ret)
		goto fail;
	avd_av1_push(run, AVD_OP_EXEC |
		AVD_OP_EXEC_FLAG_START_REV4(ctx->dev->variant->revision == 4));
	set_header(ctx, run);
	run->segment_end[0] = f->prog_words;
	set_tiles(ctx, run);
	if (run->error) {
		ret = run->error;
		goto fail;
	}
	ret = avd_av1_publish_ref(ctx, run);
	if (ret)
		goto fail;
	/* The request controls can change as soon as prepare returns. */
	run->seq = NULL;
	run->frame = NULL;
	run->tile_group = NULL;
	run->grain = NULL;
	f->codec_priv = run;
	return 0;
fail:
	avd_av1_release_run(ctx, run);
	clear_bit(f->idx, &av1->active_slots);
	f->prog_words = 0;
	return ret;
}

static void avd_av1_retire(struct avd_ctx *ctx, struct avd_frame *f,
			    enum vb2_buffer_state state)
{
	struct avd_av1_ctx *av1 = ctx->priv;
	struct avd_av1_run *run = f->codec_priv;
	int status = state == VB2_BUF_STATE_DONE ? 0 : -EIO;

	if (!run)
		return;
	if (run->cdf_written) {
		WRITE_ONCE(run->cdf_out->status, status);
		smp_store_release(&run->cdf_out->done, true);
	}
	if (run->seg_written) {
		WRITE_ONCE(run->seg_out->status, status);
		smp_store_release(&run->seg_out->done, true);
	}
	avd_av1_release_run(ctx, run);
	f->codec_priv = NULL;
	clear_bit(f->idx, &av1->active_slots);
}

/*
 * The open transports still consume legacy segments and PP kick counts. Keep
 * that interface until they accept native frame_ops. All packing and auxiliary
 * ownership stay in prepare/retire; only this adapter accesses the legacy job.
 */
static int avd_av1_run(struct avd_ctx *ctx)
{
	struct avd_av1_ctx *av1 = ctx->priv;
	struct avd_frame *f = &av1->legacy_frame;
	struct avd_av1_run *run;
	struct avd_run base;
	const __le32 *words;
	size_t pos = 1, end, i, j;
	int ret;

	avd_run_preamble(ctx, &base);
	if (!av1->legacy_prog.cpu) {
		ret = avd_buf_alloc(ctx->dev, &av1->legacy_prog,
			(AVD_MAX_INST + 1) * (V4L2_AV1_MAX_TILE_COUNT + 1) * sizeof(u32));
		if (ret)
			goto complete_controls;
	}
	f->src = base.bufs.src;
	f->dst = base.bufs.dst;
	f->prog = &av1->legacy_prog;
	f->seq++;
	ret = avd_av1_prepare(ctx, f);
	if (ret)
		goto complete_controls;
	run = f->codec_priv;
	ret = avd_init_job(ctx, AVD_CODEC_AV1, run->num_tiles + 1);
	if (ret)
		goto retire;
	words = f->prog->cpu;
	for (i = 0; i <= run->num_tiles; i++) {
		struct avd_segment *seg = &ctx->job.segments[i];

		end = run->segment_end[i];
		if (end < pos || end - pos > AVD_MAX_INST) {
			ret = -EOVERFLOW;
			goto retire;
		}
		seg->num = end - pos;
		for (j = 0; j < seg->num; j++)
			seg->instructions[j] = le32_to_cpu(words[pos + j]);
		pos = end + !!i;
	}
	ctx->job.num = run->num_tiles;
	avd_run_postamble(ctx, &base);
	ret = avd_submit_job(ctx);
	if (!ret)
		return 0;
	avd_av1_retire(ctx, f, VB2_BUF_STATE_ERROR);
	return ret;
retire:
	avd_av1_retire(ctx, f, VB2_BUF_STATE_ERROR);
complete_controls:
	avd_run_postamble(ctx, &base);
	return ret;
}

static void avd_av1_done(struct avd_ctx *ctx, struct vb2_v4l2_buffer *src,
			 struct vb2_v4l2_buffer *dst, enum vb2_buffer_state state)
{
	struct avd_av1_ctx *av1 = ctx->priv;

	avd_av1_retire(ctx, &av1->legacy_frame, state);
}

static unsigned int avd_av1_pp_kicks(struct avd_ctx *ctx)
{
	struct avd_av1_ctx *av1 = ctx->priv;
	struct avd_av1_run *run = av1->legacy_frame.codec_priv;

	return run ? run->num_tiles : 1;
}

static void avd_av1_submit(struct avd_ctx *ctx)
{
	avd_open_av1_submit(ctx, avd_av1_pp_kicks(ctx));
}

static void avd_av1_stop(struct avd_ctx *ctx)
{
	struct avd_av1_ctx *av1 = ctx->priv;
	struct avd_av1_ref *ref, *tmp;

	/* Core drains and retires every slot before stopping the codec. */
	if (WARN_ON(av1->active_slots))
		return;
	list_for_each_entry_safe(ref, tmp, &av1->refs, node) {
		avd_av1_aux_put(ctx, ref->cdf);
		avd_av1_aux_put(ctx, ref->seg);
		list_del(&ref->node);
		kfree(ref);
	}
	avd_buf_free(ctx->dev, &av1->legacy_prog);
	kfree(av1);
	ctx->priv = NULL;
}

static int avd_av1_start(struct avd_ctx *ctx)
{
	struct avd_av1_ctx *av1;

	av1 = kzalloc(sizeof(*av1), GFP_KERNEL);
	if (!av1)
		return -ENOMEM;
	INIT_LIST_HEAD(&av1->refs);
	/* The embedded legacy frame must retire before this ctx runs again. */
	ctx->max_frames = 1;
	ctx->priv = av1;
	return 0;
}

static enum avd_image_fmt avd_av1_get_image_fmt(struct avd_ctx *ctx,
						struct v4l2_ctrl *ctrl)
{
#define BIT_DEPTH(chroma)                                    \
	(seq->bit_depth == 8 ? AVD_IMG_FMT_##chroma##_8BIT : \
			       AVD_IMG_FMT_##chroma##_10BIT)

	const struct v4l2_ctrl_av1_sequence *seq = ctrl->p_new.p_av1_sequence;

	if (ctrl->id != V4L2_CID_STATELESS_AV1_SEQUENCE)
		return AVD_IMG_FMT_ANY;

	if (!(seq->flags & V4L2_AV1_SEQUENCE_FLAG_SUBSAMPLING_X)) /* 4:4:4 */
		return AVD_IMG_FMT_ANY;
	if (!(seq->flags & V4L2_AV1_SEQUENCE_FLAG_SUBSAMPLING_Y))
		return BIT_DEPTH(422);
	if (seq->flags & V4L2_AV1_SEQUENCE_FLAG_MONO_CHROME)
		return AVD_IMG_FMT_ANY; /* 4:0:0 (not supported?) */

	return BIT_DEPTH(420);
#undef BIT_DEPTH
}

static void avd_av1_adjust_decoded_fmt(struct avd_ctx *ctx,
				       struct v4l2_pix_format_mplane *pix_mp)
{
	/* Keep both auxiliary regions aligned after compressed storage. */
	pix_mp->plane_fmt[0].sizeimage =
		ALIGN(pix_mp->plane_fmt[0].sizeimage, AVD_ALIGN);
	pix_mp->plane_fmt[0].sizeimage += ALIGN(AVD_CDFS_SIZE, AVD_ALIGN);
	pix_mp->plane_fmt[0].sizeimage +=
		ALIGN(avd_color_size(pix_mp->width, pix_mp->height), AVD_ALIGN);
	pix_mp->plane_fmt[0].sizeimage +=
		avd_av1_seg_size(pix_mp->width, pix_mp->height);
}

static int avd_av1_validate_sequence(struct avd_ctx *ctx,
				     struct v4l2_ctrl_av1_sequence *seq)
{
	if (seq->flags & V4L2_AV1_SEQUENCE_FLAG_MONO_CHROME)
		return -EINVAL; /* 4:0:0 (not supported?) */

	return 0;
}

static int avd_av1_try_ctrl(struct avd_ctx *ctx, struct v4l2_ctrl *ctrl)
{
	if (ctrl->id == V4L2_CID_STATELESS_AV1_SEQUENCE)
		return avd_av1_validate_sequence(ctx,
						 ctrl->p_new.p_av1_sequence);
	/* width should also be > 8 */
	return 0;
}

const struct avd_coded_fmt_ops avd_av1_fmt_ops = {
	.adjust_decoded_fmt = avd_av1_adjust_decoded_fmt,
	.start = avd_av1_start,
	.stop = avd_av1_stop,
	.run = avd_av1_run,
	.done = avd_av1_done,
	.submit = avd_av1_submit,
	.pp_kicks = avd_av1_pp_kicks,
	.try_ctrl = avd_av1_try_ctrl,
	.get_image_fmt = avd_av1_get_image_fmt,
};

const struct avd_codec_frame_ops avd_av1_frame_ops = {
	.prepare = avd_av1_prepare,
	.pack_cmd = avd_av1_pack_cmd,
	.retire = avd_av1_retire,
};
