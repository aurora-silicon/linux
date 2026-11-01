// SPDX-License-Identifier: GPL-2.0-only
/*
 * Apple AVE video encoder: encoding sessions
 *
 * A session owns the encoder's working buffers (sized from the picture
 * size), a ring of up to five Process slots and the coded output of each
 * slot. The firmware answers frames in bitstream order, so the N-th answer
 * is the N-th access unit; with B-frames that is not the input order.
 *
 * Copyright The Aurora Silicon Contributors
 */
#include <linux/device.h>
#include <linux/jiffies.h>
#include <linux/dma-mapping.h>
#include <linux/log2.h>
#include <linux/printk.h>
#include <linux/slab.h>
#include <linux/sort.h>
#include <linux/unaligned.h>

#include <media/v4l2-ctrls.h>
#include <media/videobuf2-dma-contig.h>
#include <media/videobuf2-v4l2.h>

#include "ave.h"
#include "ave-regs.h"
#include "ave-mctf.h"

#define AVE_RC_VBR 4
#define AVE_RC_CBR 2

/* Filler-data NAL unit types */
#define AVE_H264_NAL_FILLER 12
#define AVE_HEVC_NAL_FILLER 38

#define AVE_H264_BASELINE 2
#define AVE_H264_MAIN 4
#define AVE_H264_HIGH 6
#define AVE_HEVC_MAIN 1

#define AVE_META_BYTES 0xc000
/* Metadata words: coded bytes (twice), HEVC slice-header bytes, parameter sets */
#define AVE_META_PAYLOAD 0x120
#define AVE_META_HEVC_SHDR 0x128
#define AVE_META_PAYLOAD2 0x12c
#define AVE_META_PARAMS 0xb2a4
#define AVE_META_SCAN_FROM 0x200
#define AVE_SHDR_BYTES (256 * 0x400)
#define AVE_SHDR_ENTRY_BYTES 0x400
#define AVE_CODED_MIN 0x3c000

/* Level limits: max frame size, max rate, max bit rate (kbit/s, Main). */
struct ave_level {
	u32 max_fs;
	u64 max_rate;
	u32 max_kbps;
};

/* H.264 Table A-1, in V4L2 menu order (1, 1b, 1.1 ... 5.2); macroblocks. */
static const struct ave_level ave_h264_levels[] = {
	{ 99, 1485, 64 },	    { 99, 1485, 128 },
	{ 396, 3000, 192 },	    { 396, 6000, 384 },
	{ 396, 11880, 768 },	    { 396, 11880, 2000 },
	{ 792, 19800, 4000 },	    { 1620, 20250, 4000 },
	{ 1620, 40500, 10000 },	    { 3600, 108000, 14000 },
	{ 5120, 216000, 20000 },    { 8192, 245760, 20000 },
	{ 8192, 245760, 50000 },    { 8704, 522240, 50000 },
	{ 22080, 589824, 135000 },  { 36864, 983040, 240000 },
	{ 36864, 2073600, 240000 },
};

/* HEVC Table A-8 (Main tier), in V4L2 menu order (1, 2, 2.1 ... 5.1); samples. */
static const struct ave_level ave_hevc_levels[] = {
	{ 36864, 552960, 128 },	       { 122880, 3686400, 1500 },
	{ 245760, 7372800, 3000 },     { 552960, 16588800, 6000 },
	{ 983040, 33177600, 10000 },   { 2228224, 66846720, 12000 },
	{ 2228224, 133693440, 20000 }, { 8912896, 267386880, 25000 },
	{ 8912896, 534773760, 40000 },
};

/*
 * The lowest level that fits the picture, rate and bit rate, or the
 * requested one if that is higher. Returned as the firmware's 1-based enum.
 */
static int ave_pick_level(const struct ave_params *p, u32 requested)
{
	bool hevc = p->codec == AVE_CODEC_HEVC;
	const struct ave_level *t = hevc ? ave_hevc_levels : ave_h264_levels;
	unsigned int n = hevc ? ARRAY_SIZE(ave_hevc_levels) : ARRAY_SIZE(ave_h264_levels);
	u64 size = hevc ? (u64)p->cw * p->ch : p->mbs;
	u64 side = hevc ? max(p->cw, p->ch) : p->cw / 16;
	u64 rate = DIV_ROUND_UP_ULL(size * p->fps_num, p->fps_den);
	u32 kbps = DIV_ROUND_UP(p->bitrate, 1000);

	for (unsigned int i = 0; i < n; i++) {
		u32 max_kbps = t[i].max_kbps;

		if (p->high)
			max_kbps += max_kbps / 4;
		if (size <= t[i].max_fs && side * side <= 8ULL * t[i].max_fs &&
		    rate <= t[i].max_rate && kbps <= max_kbps)
			return max(i, requested) + 1;
	}
	return -EINVAL;
}

static int ave_sess_params(struct ave_ctx *ctx, struct ave_params *p)
{
	bool baseline = false;
	int level;

	p->codec = ave_ctx_codec(ctx);
	p->width = ctx->src_fmt.width;
	p->height = ctx->src_fmt.height;
	p->cw = ALIGN(p->width, 16);
	p->ch = ALIGN(p->height, 16);
	p->stride = ctx->src_fmt.plane_fmt[0].bytesperline;
	p->mbs = (p->cw / 16) * (p->ch / 16);
	p->fps_num = ctx->timeperframe.denominator;
	p->fps_den = ctx->timeperframe.numerator;
	p->fps_int = max(DIV_ROUND_CLOSEST(p->fps_num, p->fps_den), 1U);

	if (p->codec == AVE_CODEC_H264) {
		switch (ctx->h264_profile) {
		case V4L2_MPEG_VIDEO_H264_PROFILE_BASELINE:
		case V4L2_MPEG_VIDEO_H264_PROFILE_CONSTRAINED_BASELINE:
			p->profile = AVE_H264_BASELINE;
			baseline = true;
			break;
		case V4L2_MPEG_VIDEO_H264_PROFILE_HIGH:
			p->profile = AVE_H264_HIGH;
			p->high = true;
			break;
		default:
			p->profile = AVE_H264_MAIN;
			break;
		}
		p->min_qp = ctx->h264_min_qp;
		p->max_qp = ctx->h264_max_qp;
	} else {
		p->profile = AVE_HEVC_MAIN;
		p->min_qp = ctx->hevc_min_qp;
		p->max_qp = ctx->hevc_max_qp;
	}
	if (p->min_qp > p->max_qp)
		p->min_qp = p->max_qp;

	/*
	 * Reference structures as macOS sets them up: a B-frame pyramid,
	 * P frames only (low delay), or (H.264) intra only.
	 */
	p->intra_only = p->codec == AVE_CODEC_H264 && ctx->gop_size == 1;
	p->bframes = !baseline && !p->intra_only && ctx->b_frames;
	p->reorder_off = !ctx->b_frames;
	p->refs = p->intra_only ? 1 : p->bframes ? 5 : 2;
	p->slots = p->bframes ? 5 : 3;
	p->gop = ctx->gop_size ?: 120;

	p->bitrate = ctx->bitrate;
	if (ctx->bitrate_mode == V4L2_MPEG_VIDEO_BITRATE_MODE_CBR) {
		p->rc_mode = AVE_RC_CBR;
		p->limit_bytes = 0;
	} else {
		/*
		 * The data-rate limit macOS sets is twice the average rate over
		 * one second; a peak equal to the average starved the rate
		 * control on hardware. A peak of 0 means that default.
		 */
		p->rc_mode = AVE_RC_VBR;
		p->limit_bytes = (ctx->bitrate_peak ? max(ctx->bitrate_peak, ctx->bitrate) :
				  2ULL * ctx->bitrate) / 8;
	}

	level = ave_pick_level(p, p->codec == AVE_CODEC_H264 ? ctx->h264_level :
							       ctx->hevc_level);
	if (level < 0)
		return level;
	p->level = level;

	/* Coded-data capacity: 1.5 bytes per (aligned) pixel, 16 KiB pages. */
	p->coded_cap = ALIGN(3 * (p->codec == AVE_CODEC_HEVC ? ALIGN(p->width, 32) :
							       ALIGN(p->width, 16)) *
			     ALIGN(p->height, 16) / 2, SZ_16K);
	p->coded_cap = max_t(u32, p->coded_cap, AVE_CODED_MIN);
	if (ctx->ave->layout->counted &&
	    (p->fps_num != 30ULL * p->fps_den ||
	     (!p->bframes && (p->codec == AVE_CODEC_HEVC || (!baseline && !p->intra_only)))))
		return -EOPNOTSUPP;
	return 0;
}

/*
 * Two groups hold more than their size fields say, measured on hardware
 * at 1080p and 2408x1506: the +0x390 group is a 4:2:0 picture whose size
 * covers luma only (the firmware writes up to 1.5 times it), and the
 * +0x3a0 group is written up to 1.2 times its size (mapped at 1.25 times,
 * as macOS spaces them). Every other buffer stays within its size; the
 * mapping is only rounded up to whole pages.
 */

/* One buffer to allocate: what the firmware is told and what is mapped. */
struct ave_buf_req {
	struct ave_buf *b;
	size_t size;
	size_t alloc;
	unsigned long attrs;
};

#define AVE_MAX_BUF_REQS \
	(1 + 6 * AVE_MAX_REFS + 6 + 2 * AVE_TILE_BUFS + 2 + 4 * AVE_MAX_SLOTS)

static int ave_buf_req_cmp(const void *a, const void *b)
{
	const struct ave_buf_req *x = a, *y = b;

	return x->alloc < y->alloc ? 1 : x->alloc > y->alloc ? -1 : 0;
}

static void ave_req(struct ave_buf_req *r, unsigned int *n, struct ave_buf *b, size_t size,
		    size_t alloc, unsigned long attrs)
{
	r[*n] = (struct ave_buf_req){
		.b = b, .size = size, .alloc = max(size, alloc), .attrs = attrs,
	};
	(*n)++;
}

/*
 * Working buffer sizes, all from the requested picture size. The DMA layer
 * aligns each mapping to a power of two in the encoder's 256 MiB window, so
 * the buffers are mapped largest first: the smaller ones then fill the
 * alignment gaps the large ones leave.
 */
static int ave_sess_alloc(struct ave_dev *ave, struct ave_session *sess)
{
	const struct ave_params *p = &sess->p;
	const struct ave_fw_layout *fw = ave->layout;
	bool hevc = p->codec == AVE_CODEC_HEVC;
	u32 w = p->width, h = p->height;
	u32 w16 = DIV_ROUND_UP(w, 16), w32 = DIV_ROUND_UP(w, 32);
	u32 h32 = DIV_ROUND_UP(h, 32), h64 = DIV_ROUND_UP(h, 64);
	const unsigned long nomap = DMA_ATTR_NO_KERNEL_MAPPING;
	size_t ref_a, ref_b, ref_c, ref_d, tile, mapped = 0;
	struct ave_buf_req *r;
	unsigned int n = 0;
	int ret = 0;

	r = kcalloc(AVE_MAX_BUF_REQS, sizeof(*r), GFP_KERNEL);
	if (!r)
		return -ENOMEM;

	ref_a = ALIGN(w, 32) * ALIGN(h + 16, 32);
	if (p->bit_depth == 10)
		ref_a = ref_a * 5 / 4;
	ref_b = roundup_pow_of_two(w) * roundup_pow_of_two(h) / 32;
	ref_c = ave_lfs_ref_size(w, h, hevc, ave->variant->lfs_ref_scale);
	ref_d = ALIGN(hevc ? 64 * w32 * h32 : 128 * p->mbs, SZ_16K);
	tile = hevc ? 2304 * w32 * h64 : ALIGN(w, 16) * ALIGN(h, 64);
	if (p->bit_depth == 10)
		tile *= 2;

	ave_req(r, &n, &sess->hdr, fw->hdr_bytes, 0, nomap);
	for (unsigned int i = 0; i < p->refs; i++) {
		ave_req(r, &n, &sess->ref_a[i], ref_a, ref_a * 3 / 2, nomap);
		ave_req(r, &n, &sess->ref_b[i], ref_b, ref_b * 5 / 4, nomap);
		ave_req(r, &n, &sess->ref_c[i], ref_c, 0, nomap);
		ave_req(r, &n, &sess->ref_d[i], ref_d, 0, nomap);
		if (fw->counted) {
			ave_req(r, &n, &sess->ref_e[i], ref_a / 2, 0, nomap);
			ave_req(r, &n, &sess->ref_f[i], ref_b / 4, 0, nomap);
		}
	}
	ave_req(r, &n, &sess->mb, ALIGN(82 * p->mbs, SZ_16K), 0, nomap);
	if (fw->counted && p->rc_mode == 3)
		for (unsigned int i = 0; i < 2; i++)
			ave_req(r, &n, &sess->mb_extra[i], ALIGN(82 * p->mbs, SZ_16K), 0, nomap);
	/* HEVC: one 0x180-byte neighbour record per 64 pixels, at least 16 KiB. */
	ave_req(r, &n, &sess->row_a,
		ALIGN(hevc ? max_t(u32, 0x180 * DIV_ROUND_UP(w, 64), SZ_16K) : 256 * w16, SZ_16K),
		0, nomap);
	ave_req(r, &n, &sess->row_b, ALIGN(hevc ? 768 * w32 : 1024 * w16, SZ_16K), 0, nomap);
	ave_req(r, &n, &sess->fixed, SZ_16K, 0, nomap);
	for (unsigned int i = 0; i < (hevc ? 4 : AVE_TILE_BUFS); i++) {
		ave_req(r, &n, &sess->tile[i], tile, 0, nomap);
		ave_req(r, &n, &sess->tile_aux[i], ALIGN(h, 128), 0, nomap);
	}
	if (hevc)
		for (unsigned int i = 0; i < 2; i++)
			ave_req(r, &n, &sess->hevc_extra[i], ALIGN(w * h32, SZ_16K), 0, nomap);
	for (unsigned int s = 0; s < p->slots; s++) {
		ave_req(r, &n, &sess->slot[s].coded, p->coded_cap, 0, 0);
		ave_req(r, &n, &sess->slot[s].meta, fw->meta_bytes, 0, 0);
		/* J700 has HEVC slice headers; 26A428 has a new small slot buffer. */
		if (fw->counted) {
			ave_req(r, &n, &sess->slot[s].extra, SZ_16K, 0, 0);
			if (!hevc && p->rc_mode != 3)
				ave_req(r, &n, &sess->slot[s].aux,
					ALIGN(432 * p->mbs, SZ_16K), 0, nomap);
		} else {
			ave_req(r, &n, &sess->slot[s].aux,
				hevc ? AVE_SHDR_BYTES : ALIGN(432 * p->mbs, SZ_16K), 0,
				hevc ? 0 : nomap);
		}
	}

	sort(r, n, sizeof(*r), ave_buf_req_cmp, NULL);
	for (unsigned int i = 0; i < n && !ret; i++) {
		ret = ave_alloc_padded(ave, r[i].b, r[i].size, r[i].alloc, r[i].attrs);
		mapped += r[i].b->alloc;
	}
	kfree(r);
	if (ret) {
		dev_err(ave->dev, "out of encoder DMA space after %zu MiB of session buffers\n",
			mapped >> 20);
		return -ENOMEM;
	}
	dev_dbg(ave->dev, "session buffers: %u, %zu KiB mapped\n", n, mapped >> 10);
	return 0;
}

/*
 * An MCTF session: fixed 1080p NV12 on the selector-2 carriers, the
 * firmware-owned surfaces of the layout (zeroed by the DMA allocator and
 * never mapped for the CPU), largest first like the encoder's buffers.
 */
int ave_sess_mctf_init(struct ave_ctx *ctx, struct ave_session *sess)
{
	struct ave_dev *ave = ctx->ave;
	const struct ave_mctf_layout *l = ave_mctf_qualified(ave->text_crc);
	struct ave_params *p = &sess->p;
	struct ave_buf_req *r;
	unsigned int n = 0;
	size_t mapped = 0;
	int ret = 0;

	if (!l)
		return -EOPNOTSUPP;
	if (l->n_surfaces > AVE_MCTF_BUFS || l->slots > AVE_MAX_SLOTS ||
	    l->slots > AVE_MCTF_OUT_BUFS || l->inputs > AVE_MCTF_IN_BUFS)
		return -EINVAL;
	*p = (struct ave_params){
		.codec = AVE_CODEC_HEVC,	/* the selector-2 carriers */
		.width = AVE_MCTF_WIDTH,
		.height = AVE_MCTF_HEIGHT,
		.cw = AVE_MCTF_WIDTH,
		.ch = ALIGN(AVE_MCTF_HEIGHT, 16),
		.stride = ctx->src_fmt.plane_fmt[0].bytesperline,
		.fps_num = 30,
		.fps_den = 1,
		.fps_int = 30,
		.slots = l->slots,
	};
	sess->mctf = l;
	sess->unit = l->unit;
	sess->arg1 = l->arg1;
	sess->frame_arg0 = l->frame_arg0;

	r = kcalloc(l->n_surfaces, sizeof(*r), GFP_KERNEL);
	if (!r)
		return -ENOMEM;
	for (unsigned int i = 0; i < l->n_surfaces; i++)
		if (l->surfaces[i].alloc)
			ave_req(r, &n, &sess->mctf_buf[i], l->surfaces[i].alloc, 0,
				DMA_ATTR_NO_KERNEL_MAPPING);
	sort(r, n, sizeof(*r), ave_buf_req_cmp, NULL);
	for (unsigned int i = 0; i < n && !ret; i++) {
		ret = ave_alloc(ave, r[i].b, r[i].size, r[i].attrs);
		mapped += r[i].b->alloc;
	}
	kfree(r);
	if (ret) {
		dev_err(ave->dev, "out of encoder DMA space after %zu MiB of MCTF surfaces\n",
			mapped >> 20);
		return -ENOMEM;
	}
	for (unsigned int i = 0; i < l->n_surfaces; i++)
		sess->mctf_iova[i] = sess->mctf_buf[i].dma;
	/* Picture surfaces: the CPU moves pictures in and out of them. */
	for (unsigned int i = 0; i < l->inputs && !ret; i++)
		ret = ave_alloc(ave, &sess->mctf_in[i], l->form.bytes, 0);
	for (unsigned int i = 0; i < l->slots && !ret; i++)
		ret = ave_alloc(ave, &sess->mctf_out[i], l->form.bytes, 0);
	if (ret) {
		dev_err(ave->dev, "no DMA space for the MCTF picture surfaces\n");
		return -ENOMEM;
	}
	dev_dbg(ave->dev, "MCTF session: %u surfaces, %zu KiB mapped\n", n, mapped >> 10);
	return 0;
}

static void ave_sess_free_bufs(struct ave_dev *ave, struct ave_session *sess)
{
	ave_free(ave, &sess->hdr);
	for (unsigned int i = 0; i < AVE_MAX_REFS; i++) {
		ave_free(ave, &sess->ref_a[i]);
		ave_free(ave, &sess->ref_b[i]);
		ave_free(ave, &sess->ref_c[i]);
		ave_free(ave, &sess->ref_d[i]);
		ave_free(ave, &sess->ref_e[i]);
		ave_free(ave, &sess->ref_f[i]);
	}
	ave_free(ave, &sess->mb);
	for (unsigned int i = 0; i < 2; i++)
		ave_free(ave, &sess->mb_extra[i]);
	ave_free(ave, &sess->row_a);
	ave_free(ave, &sess->row_b);
	ave_free(ave, &sess->fixed);
	for (unsigned int i = 0; i < AVE_TILE_BUFS; i++) {
		ave_free(ave, &sess->tile[i]);
		ave_free(ave, &sess->tile_aux[i]);
	}
	for (unsigned int i = 0; i < 2; i++)
		ave_free(ave, &sess->hevc_extra[i]);
	for (unsigned int i = 0; i < AVE_MCTF_BUFS; i++)
		ave_free(ave, &sess->mctf_buf[i]);
	for (unsigned int i = 0; i < AVE_MCTF_IN_BUFS; i++)
		ave_free(ave, &sess->mctf_in[i]);
	for (unsigned int i = 0; i < AVE_MCTF_OUT_BUFS; i++)
		ave_free(ave, &sess->mctf_out[i]);
	for (unsigned int s = 0; s < AVE_MAX_SLOTS; s++) {
		ave_free(ave, &sess->slot[s].coded);
		ave_free(ave, &sess->slot[s].meta);
		ave_free(ave, &sess->slot[s].aux);
		ave_free(ave, &sess->slot[s].extra);
		ave_free(ave, &sess->slot[s].bounce);
		ave_vb2_put(sess->slot[s].input_mem);
		sess->slot[s].input_mem = NULL;
		ave_vb2_put(sess->slot[s].output_mem);
		sess->slot[s].output_mem = NULL;
	}
}

static void ave_note_copy(struct ave_ctx *ctx, u32 stride, const char *how)
{
	if (ctx->sess->copy_noted)
		return;
	ctx->sess->copy_noted = true;
	dev_dbg(ctx->ave->dev, "input has a %u byte stride; moving lines to %u %s\n", stride,
		ctx->sess->p.stride, how);
}

/*
 * The input picture for @slot, as a DART address. The encoder reads NV12
 * with the 64-byte aligned stride S_FMT reported and chroma right after
 * height luma lines. Some clients (ffmpeg's v4l2m2m encoder) fill the
 * buffer with their own, smaller line size instead, which shows in the
 * payload size; such frames are copied to a per-slot buffer in the
 * encoder's layout.
 */
int ave_sess_input(struct ave_ctx *ctx, unsigned int slot, struct vb2_v4l2_buffer *src,
		   dma_addr_t *input)
{
	struct ave_session *sess = ctx->sess;
	const struct ave_params *p = &sess->p;
	struct vb2_buffer *vb = &src->vb2_buf;
	u32 off = vb->planes[0].data_offset;
	u32 used = vb2_get_plane_payload(vb, 0) - min(off, vb2_get_plane_payload(vb, 0));
	u32 crows = DIV_ROUND_UP(p->height, 2), rows = p->height + crows;
	u32 width = ALIGN(p->width, 2), stride;
	struct ave_buf *b = &sess->slot[slot].bounce;
	const u8 *from;
	u8 *to;
	int ret;

	*input = vb2_dma_contig_plane_dma_addr(vb, 0) + off;
	if (!used || used >= p->stride * rows || used % rows || used / rows < width) {
		if (IS_ALIGNED(*input, 64))
			return 0;
		dev_err(ctx->ave->dev, "OUTPUT plane at %pad is not 64-byte aligned\n", input);
		return -EINVAL;
	}
	stride = used / rows;

	from = vb2_plane_vaddr(vb, 0);
	if (!from) {
		dev_err(ctx->ave->dev, "cannot map an OUTPUT buffer to copy its %u byte stride\n",
			stride);
		return -EFAULT;
	}
	from += off;

	/*
	 * Buffers the driver allocated (MMAP) are large enough for the
	 * encoder's layout: spread the lines out in place, last line first,
	 * so no extra DMA space is needed. Imported buffers may be shared
	 * with other users and get a copy instead.
	 */
	if (vb->memory == VB2_MEMORY_MMAP &&
	    vb2_plane_size(vb, 0) >= off + (size_t)p->stride * rows) {
		to = (u8 *)from;
		for (u32 y = rows; y-- > 0;)
			memmove(to + y * p->stride, from + y * stride, width);
		ave_note_copy(ctx, stride, "in place");
		return 0;
	}

	ret = ave_alloc(ctx->ave, b, p->stride * rows, 0);
	if (ret) {
		dev_err(ctx->ave->dev, "no room to copy %u byte stride input\n", stride);
		return ret;
	}
	to = b->cpu;
	for (u32 y = 0; y < rows; y++)
		memcpy(to + y * p->stride, from + y * stride, width);
	ave_note_copy(ctx, stride, "to a copy");
	*input = b->dma;
	return 0;
}

/* Debug map of the session buffers, to match DART faults to a buffer. */
static void ave_log_buf(struct ave_dev *ave, const char *name, int i, const struct ave_buf *b)
{
	if (b->cpu)
		dev_dbg(ave->dev, "buf %s%d %pad +%#zx (mapped %#zx)\n", name, i, &b->dma,
			b->size, b->alloc);
}

static void ave_sess_log_map(struct ave_dev *ave, struct ave_session *sess)
{
	ave_log_buf(ave, "hdr", 0, &sess->hdr);
	ave_log_buf(ave, "mb", 0, &sess->mb);
	ave_log_buf(ave, "row_a", 0, &sess->row_a);
	ave_log_buf(ave, "row_b", 0, &sess->row_b);
	ave_log_buf(ave, "fixed", 0, &sess->fixed);
	for (int i = 0; i < 2; i++)
		ave_log_buf(ave, "hevc_extra", i, &sess->hevc_extra[i]);
	for (int i = 0; i < AVE_MAX_REFS; i++) {
		ave_log_buf(ave, "ref_a", i, &sess->ref_a[i]);
		ave_log_buf(ave, "ref_b", i, &sess->ref_b[i]);
		ave_log_buf(ave, "ref_c", i, &sess->ref_c[i]);
		ave_log_buf(ave, "ref_d", i, &sess->ref_d[i]);
	}
	for (int i = 0; i < AVE_TILE_BUFS; i++) {
		ave_log_buf(ave, "tile", i, &sess->tile[i]);
		ave_log_buf(ave, "tile_aux", i, &sess->tile_aux[i]);
	}
	for (int i = 0; i < AVE_MCTF_BUFS; i++)
		ave_log_buf(ave, "mctf", i, &sess->mctf_buf[i]);
	for (int i = 0; i < AVE_MAX_SLOTS; i++) {
		ave_log_buf(ave, "coded", i, &sess->slot[i].coded);
		ave_log_buf(ave, "meta", i, &sess->slot[i].meta);
		ave_log_buf(ave, "aux", i, &sess->slot[i].aux);
	}
}

/* Work out the parameters and allocate the buffers; no firmware access. */
struct ave_session *ave_sess_create(struct ave_ctx *ctx)
{
	struct ave_session *sess;
	int ret;

	sess = kzalloc_obj(*sess);
	if (!sess)
		return ERR_PTR(-ENOMEM);
	if (ctx->mctf) {
		ret = ave_sess_mctf_init(ctx, sess);
	} else {
		ret = ave_sess_params(ctx, &sess->p);
		if (!ret)
			ret = ave_sess_alloc(ctx->ave, sess);
	}
	if (ret) {
		ave_sess_free_bufs(ctx->ave, sess);
		kfree(sess);
		return ERR_PTR(ret);
	}
	for (unsigned int s = 0; s < sess->p.slots; s++)
		ave_sess_prepare_slot(sess, s);
	ave_sess_log_map(ctx->ave, sess);
	dev_dbg(ctx->ave->dev, "session %ux%u codec %u profile %u level %u, %u slots, %u refs\n",
		sess->p.width, sess->p.height, sess->p.codec, sess->p.profile, sess->p.level,
		sess->p.slots, sess->p.refs);
	dev_dbg(ctx->ave->dev, "rate control mode %u, %u bit/s, limit %u bytes/s, gop %u, %u/%u fps\n",
		sess->p.rc_mode, sess->p.bitrate, sess->p.limit_bytes, sess->p.gop,
		sess->p.fps_num, sess->p.fps_den);
	return sess;
}

/* Keep every firmware-visible allocation mapped until a confirmed power cycle. */
void ave_sess_destroy(struct ave_ctx *ctx)
{
	struct ave_dev *ave = ctx->ave;
	struct ave_session *sess = ctx->sess;

	if (!sess)
		return;
	mutex_lock(&ave->hw_lock);
	if (sess->handed && (ave->fw_state == AVE_FW_BROKEN ||
			     ave->fw_state == AVE_FW_RECOVERING)) {
		sess->failed_next = ave->failed_sessions;
		ave->failed_sessions = sess;
	} else {
		ave_sess_free_bufs(ave, sess);
		kfree(sess);
	}
	ctx->sess = NULL;
	mutex_unlock(&ave->hw_lock);
}

/* Caller holds hw_lock and has verified that the coprocessor lost power. */
void ave_sess_reap(struct ave_dev *ave)
{
	struct ave_session *sess;

	lockdep_assert_held(&ave->hw_lock);
	while ((sess = ave->failed_sessions)) {
		ave->failed_sessions = sess->failed_next;
		ave_sess_free_bufs(ave, sess);
		kfree(sess);
	}
}

int ave_sess_free_slot(struct ave_session *sess)
{
	for (unsigned int s = 0; s < sess->p.slots; s++)
		if (!sess->slot[s].busy && !sess->slot[s].done)
			return s;
	return -EBUSY;
}

/*
 * Clear what the firmware reports through, so a stale value is never taken
 * for a new one: the whole metadata buffer and the first slice header.
 */
void ave_sess_prepare_slot(struct ave_session *sess, unsigned int slot)
{
	struct ave_slot *s = &sess->slot[slot];

	if (s->meta.cpu)
		memset(s->meta.cpu, 0, s->meta.size);
	if (sess->p.codec == AVE_CODEC_HEVC && s->aux.cpu)
		memset(s->aux.cpu, 0, AVE_SHDR_ENTRY_BYTES);
}

/*
 * A frame reply: +0x10 the session's client token, +0x20 the slot's
 * subtype, +0x40 status. Caller holds hw_lock.
 */
void ave_sess_reply(struct ave_dev *ave, const struct ave_reply *reply)
{
	u64 client = reply->client;
	u32 subtype = reply->subtype;
	u32 slot = subtype - ave->layout->process_subtype;
	struct ave_session *sess = NULL;
	struct ave_ctx *ctx = NULL;
	struct ave_slot *s;

	for (unsigned int i = 0; i < AVE_MAX_SESSIONS; i++)
		if (ave->sessions[i] &&
		    ave->sessions[i]->sess->client == client)
			ctx = ave->sessions[i];
	if (ctx)
		sess = ctx->sess;
	if (!sess ||
	    reply->bytes <
		    (ave->layout->abi->transport->boot_config ? 0x50 : 0x48) ||
	    slot >= sess->p.slots || !sess->slot[slot].busy) {
		dev_warn(ave->dev,
			 "stray frame reply (client %llu subtype %#x len %u)\n",
			 client, subtype, reply->bytes);
		return;
	}
	s = &sess->slot[slot];
	sess->progress = jiffies;
	s->status = reply->status;
	if (s->status)
		dev_dbg(ctx->ave->dev, "frame in slot %u: status %#x\n", slot,
			s->status);
	s->busy = false;
	s->done = true;
	sess->busy--;
	sess->done_fifo[(sess->done_head + sess->done_count) % AVE_MAX_SLOTS] =
		slot;
	sess->done_count++;
	atomic_inc(&ctx->ready);
}

/*
 * Copy Annex B data from @src to @dst until the data ends: NAL units never
 * hold three zero bytes in a row, and the buffer was cleared before the
 * firmware wrote it, so a run of @zero_run zero bytes marks the end.
 * Returns the length or -ENOSPC.
 */
static ssize_t ave_copy_annexb(u8 *dst, size_t room, const u8 *src, size_t cap,
			       unsigned int zero_run)
{
	size_t pos = 0, end = 0;
	unsigned int zeros = 0;

	while (pos < cap) {
		size_t chunk = min3((size_t)SZ_16K, cap - pos, room - pos);

		if (!chunk)
			return -ENOSPC;
		memcpy(dst + pos, src + pos, chunk);
		for (size_t i = pos; i < pos + chunk; i++) {
			if (dst[i]) {
				end = i + 1;
				zeros = 0;
			} else if (end && ++zeros >= zero_run) {
				return end;
			}
		}
		pos += chunk;
	}
	return end;
}

static bool ave_starts_annexb(const u8 *p, size_t len)
{
	return (len >= 3 && !p[0] && !p[1] && p[2] == 1) ||
	       (len >= 4 && !p[0] && !p[1] && !p[2] && p[3] == 1);
}

/* NAL unit types in an Annex B buffer: key frame, parameter sets, filler? */
static void ave_scan_nals(const u8 *p, size_t len, bool hevc, bool *key, bool *params,
			  bool *filler)
{
	for (size_t i = 0; i + 3 < len; i++) {
		u32 type;

		if (p[i] || p[i + 1] || p[i + 2] != 1)
			continue;
		type = hevc ? (p[i + 3] >> 1) & 0x3f : p[i + 3] & 0x1f;
		if (hevc) {
			*key |= type >= 16 && type <= 21;
			*params |= type >= 32 && type <= 34;
			*filler |= type == AVE_HEVC_NAL_FILLER;
		} else {
			*key |= type == 5;
			*params |= type == 7;
			*filler |= type == AVE_H264_NAL_FILLER;
		}
		i += 2;
	}
}

/* Where in the metadata buffer do the parameter sets start, if anywhere? */
static int ave_find_params(const u8 *meta, bool hevc)
{
	if (ave_starts_annexb(meta + AVE_META_PARAMS, AVE_META_BYTES - AVE_META_PARAMS))
		return AVE_META_PARAMS;
	/* Not at the HEVC offset: look for an SPS (H.264) or VPS (HEVC). */
	for (int i = AVE_META_SCAN_FROM; i + 4 < AVE_META_BYTES; i++) {
		u8 type;

		if (meta[i] || meta[i + 1] || meta[i + 2] != 1)
			continue;
		type = hevc ? (meta[i + 3] >> 1) & 0x3f : meta[i + 3] & 0x1f;
		if (type == (hevc ? 32 : 7))
			return i > AVE_META_SCAN_FROM && !meta[i - 1] ? i - 1 : i;
	}
	return -ENOENT;
}

static void ave_sess_dump(struct ave_ctx *ctx, struct ave_slot *s, const char *why)
{
	struct ave_session *sess = ctx->sess;
	const u8 *meta = s->meta.cpu;
	int ps;

	if (sess->warned)
		return;
	sess->warned = true;
	dev_warn(ctx->ave->dev, "frame %u output: %s (enable dynamic debug for a dump)\n",
		 ctx->dst_sequence, why);
	print_hex_dump_debug("ave coded: ", DUMP_PREFIX_OFFSET, 16, 1, s->coded.cpu, 128, false);
	print_hex_dump_debug("ave meta: ", DUMP_PREFIX_OFFSET, 16, 4, meta, 0x200, false);
	print_hex_dump_debug("ave meta b200: ", DUMP_PREFIX_OFFSET, 16, 1, meta + 0xb200,
			     0x200, false);
	ps = ave_find_params(meta, sess->p.codec == AVE_CODEC_HEVC);
	if (ps >= 0) {
		dev_dbg(ctx->ave->dev, "parameter sets at metadata +%#x\n", ps);
		print_hex_dump_debug("ave meta params: ", DUMP_PREFIX_OFFSET, 16, 1, meta + ps,
				     min(128, AVE_META_BYTES - ps), false);
	}
	/* Anything else the firmware wrote past the header words. */
	for (int i = AVE_META_SCAN_FROM; i < AVE_META_BYTES; i += 16)
		if (memchr_inv(meta + i, 0, 16))
			dev_dbg(ctx->ave->dev, "metadata +%#x: %16ph\n", i, meta + i);
}

/*
 * CBR padding. macOS streams in CBR mode follow a buffer model: each frame
 * has a budget of bitrate / (8 x fps) bytes, and once the bytes left unused
 * add up to more than 0.5 s of bit rate (the 0.5 s VBV buffer of Start
 * +0x290), every frame is padded with a filler-data NAL unit so the surplus
 * stays there. Whether the firmware or macOS user space writes that filler
 * is not known; if the firmware's output never carries filler, the driver
 * adds it here.
 */
#define AVE_FILLER_OVERHEAD 7 /* start code, NAL header, trailing bits */

static size_t ave_sess_cbr_fill(struct ave_ctx *ctx, u8 *out, size_t len, size_t room,
				bool fw_filler)
{
	struct ave_session *sess = ctx->sess;
	const struct ave_params *p = &sess->p;
	bool hevc = p->codec == AVE_CODEC_HEVC;
	s64 budget = div_u64((u64)p->bitrate * p->fps_den, 8ULL * p->fps_num);
	s64 vbv = p->bitrate / 16;	/* 0.5 s of bit rate, in bytes */
	s64 excess, pad;
	u8 *f;

	if (p->rc_mode != AVE_RC_CBR)
		return len;
	if (fw_filler && !sess->fw_fills) {
		sess->fw_fills = true;
		dev_dbg(ctx->ave->dev, "CBR: the firmware writes filler data\n");
	}
	excess = sess->cbr_surplus + budget - (s64)len - vbv;
	if (!sess->fw_fills && excess > AVE_FILLER_OVERHEAD) {
		pad = min_t(s64, excess, room - len);
		if (pad > AVE_FILLER_OVERHEAD) {
			f = out + len;
			f[0] = 0;
			f[1] = 0;
			f[2] = 0;
			f[3] = 1;
			if (hevc) {
				/* nal_unit_type 38, layer 0, temporal id 0 + 1 */
				f[4] = AVE_HEVC_NAL_FILLER << 1;
				f[5] = 1;
				memset(f + 6, 0xff, pad - AVE_FILLER_OVERHEAD);
			} else {
				f[4] = AVE_H264_NAL_FILLER;
				memset(f + 5, 0xff, pad - AVE_FILLER_OVERHEAD + 1);
			}
			f[pad - 1] = 0x80;	/* rbsp_stop_one_bit */
			len += pad;
			if (!sess->host_fill_noted) {
				sess->host_fill_noted = true;
				dev_dbg(ctx->ave->dev,
					"CBR: no filler from the firmware, the driver adds it\n");
			}
		}
	}
	sess->cbr_surplus += budget - (s64)len;
	return len;
}

/*
 * Assemble the access unit of @slot into @dst: the parameter sets the
 * firmware leaves in the metadata buffer (when it emits them), for HEVC the
 * slice header from the first slice-header entry, then the coded data.
 * Metadata +0x120 (repeated at +0x12c) holds the coded length for both
 * codecs, +0x128 the HEVC slice-header length.
 */
int ave_sess_output(struct ave_ctx *ctx, unsigned int slot, struct vb2_v4l2_buffer *dst,
		    bool *keyframe)
{
	struct ave_session *sess = ctx->sess;
	struct ave_slot *s = &sess->slot[slot];
	bool hevc = sess->p.codec == AVE_CODEC_HEVC;
	u8 *out = vb2_plane_vaddr(&dst->vb2_buf, 0);
	size_t room = vb2_plane_size(&dst->vb2_buf, 0);
	const u8 *meta = s->meta.cpu;
	u32 data, data2, shdr = 0;
	bool params = false, filler = false;
	ssize_t ps = 0;
	size_t len;
	int at;

	*keyframe = false;
	if (!out)
		return -EFAULT;
	if (s->status) {
		dev_warn_ratelimited(ctx->ave->dev, "frame %u: firmware status %#x\n",
				     ctx->dst_sequence, s->status);
		return -EIO;
	}

	data = get_unaligned_le32(meta + AVE_META_PAYLOAD);
	data2 = get_unaligned_le32(meta + AVE_META_PAYLOAD2);
	if (hevc)
		shdr = get_unaligned_le32(meta + AVE_META_HEVC_SHDR);
	if (data != data2)
		dev_dbg(ctx->ave->dev, "coded length words differ: %u/%u\n", data, data2);

	at = ave_find_params(meta, hevc);
	if (at >= 0) {
		ps = ave_copy_annexb(out, room, meta + at, AVE_META_BYTES - at, 16);
		if (ps < 0)
			return ps;
	}
	if (!data || data > sess->p.coded_cap || shdr > AVE_SHDR_ENTRY_BYTES ||
	    ps + shdr + data > room) {
		dev_err(ctx->ave->dev, "bad output sizes %u/%u (capacity %u, room %zu)\n",
			shdr, data, sess->p.coded_cap, room);
		ave_sess_dump(ctx, s, "bad sizes");
		return -EPROTO;
	}
	if (hevc)
		memcpy(out + ps, s->aux.cpu, shdr);
	memcpy(out + ps + shdr, s->coded.cpu, data);
	len = ps + shdr + data;

	if (!ave_starts_annexb(out, len))
		ave_sess_dump(ctx, s, "not Annex B");
	ave_scan_nals(out, len, hevc, keyframe, &params, &filler);
	if (!ctx->dst_sequence && !params)
		ave_sess_dump(ctx, s, "first frame has no parameter sets");
	len = ave_sess_cbr_fill(ctx, out, len, room, filler);
	vb2_set_plane_payload(&dst->vb2_buf, 0, len);
	return 0;
}

/*
 * Frames are outstanding, nothing came back for a while, and the firmware
 * has enough frames that it should have answered: without B-frames every
 * frame is answered on its own; with B-frames, once four are inside (one
 * reference plus three B) one of them must come out.
 */
bool ave_sess_stalled(struct ave_session *sess)
{
	unsigned int need = sess->p.bframes ? 4 : 1;

	return sess->busy && (sess->busy >= need || sess->nudged) &&
	       time_after(jiffies, sess->progress + msecs_to_jiffies(AVE_FRAME_WAIT_MS));
}

/*
 * With B-frames the firmware keeps frames until later ones arrive. A client
 * that waits for each coded frame before sending the next (ffmpeg's
 * v4l2m2m encoder does) would wait forever. When frames are held and no
 * input came for a while, a Complete makes the firmware code what it holds,
 * as macOS does after the first frame.
 */
#define AVE_STARVE_MS 50

bool ave_sess_starved(struct ave_session *sess)
{
	return sess->busy && !sess->nudged && sess->handed &&
	       time_after(jiffies, sess->last_sent + msecs_to_jiffies(AVE_STARVE_MS));
}
