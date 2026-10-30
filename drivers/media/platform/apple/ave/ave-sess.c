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
#include <linux/dma-mapping.h>
#include <linux/log2.h>
#include <linux/printk.h>
#include <linux/slab.h>
#include <linux/unaligned.h>

#include <media/v4l2-ctrls.h>
#include <media/videobuf2-v4l2.h>

#include "ave.h"
#include "ave-regs.h"

#define AVE_RC_VBR			4
#define AVE_RC_CBR			2

#define AVE_H264_BASELINE		2
#define AVE_H264_MAIN			4
#define AVE_H264_HIGH			6
#define AVE_HEVC_MAIN			1

#define AVE_META_BYTES			0xc000
#define AVE_META_HEVC_PAYLOAD		0x120
#define AVE_META_HEVC_SHDR		0x128
#define AVE_META_HEVC_PAYLOAD2		0x12c
#define AVE_META_HEVC_PARAMS		0xb2a4
#define AVE_SHDR_BYTES			(256 * 0x400)
#define AVE_SHDR_ENTRY_BYTES		0x400
#define AVE_CODED_MIN			0x3c000
#define AVE_HDR_BYTES			0x12c000

/* Spacing of the H.264 per-frame buffer seen at 1080p; scaled with area. */
#define AVE_H264_FRAME_BUF_1080P	0x364000
#define AVE_MBS_1080P			(120 * 68)

/* Level limits: max frame size, max rate, max bit rate (kbit/s, Main). */
struct ave_level {
	u32 max_fs;
	u64 max_rate;
	u32 max_kbps;
};

/* H.264 Table A-1, in V4L2 menu order (1, 1b, 1.1 ... 5.2); macroblocks. */
static const struct ave_level ave_h264_levels[] = {
	{ 99, 1485, 64 }, { 99, 1485, 128 }, { 396, 3000, 192 }, { 396, 6000, 384 },
	{ 396, 11880, 768 }, { 396, 11880, 2000 }, { 792, 19800, 4000 },
	{ 1620, 20250, 4000 }, { 1620, 40500, 10000 }, { 3600, 108000, 14000 },
	{ 5120, 216000, 20000 }, { 8192, 245760, 20000 }, { 8192, 245760, 50000 },
	{ 8704, 522240, 50000 }, { 22080, 589824, 135000 }, { 36864, 983040, 240000 },
	{ 36864, 2073600, 240000 },
};

/* HEVC Table A-8 (Main tier), in V4L2 menu order (1, 2, 2.1 ... 5.1); samples. */
static const struct ave_level ave_hevc_levels[] = {
	{ 36864, 552960, 128 }, { 122880, 3686400, 1500 }, { 245760, 7372800, 3000 },
	{ 552960, 16588800, 6000 }, { 983040, 33177600, 10000 },
	{ 2228224, 66846720, 12000 }, { 2228224, 133693440, 20000 },
	{ 8912896, 267386880, 25000 }, { 8912896, 534773760, 40000 },
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
	 * Reference structures as macOS sets them up. H.264 has all three
	 * (B-frames, P only, intra only). HEVC without B-frames needs a
	 * reference-set table whose contents are not known yet, so HEVC
	 * always uses B-frames.
	 */
	p->intra_only = p->codec == AVE_CODEC_H264 && ctx->gop_size == 1;
	p->bframes = p->codec == AVE_CODEC_HEVC ||
		     (!baseline && !p->intra_only && ctx->b_frames);
	p->refs = p->intra_only ? 1 : p->bframes ? 5 : 2;
	p->slots = p->bframes ? 5 : 3;
	p->gop = ctx->gop_size ?: 120;

	p->bitrate = ctx->bitrate;
	if (ctx->bitrate_mode == V4L2_MPEG_VIDEO_BITRATE_MODE_CBR) {
		p->rc_mode = AVE_RC_CBR;
		p->limit_bytes = 0;
	} else {
		p->rc_mode = AVE_RC_VBR;
		p->limit_bytes = max(ctx->bitrate_peak, ctx->bitrate) / 8;
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
	return 0;
}

/* Working buffer sizes, all from the requested picture size. */
static int ave_sess_alloc(struct ave_dev *ave, struct ave_session *sess)
{
	const struct ave_params *p = &sess->p;
	bool hevc = p->codec == AVE_CODEC_HEVC;
	u32 w = p->width, h = p->height;
	u32 w16 = DIV_ROUND_UP(w, 16), w32 = DIV_ROUND_UP(w, 32);
	u32 h32 = DIV_ROUND_UP(h, 32), w64 = DIV_ROUND_UP(w, 64), h64 = DIV_ROUND_UP(h, 64);
	const unsigned long nomap = DMA_ATTR_NO_KERNEL_MAPPING;
	size_t ref_a, ref_b, ref_c, ref_d, tile, frame_buf;
	int ret = 0;

	ref_a = ALIGN(w, 32) * ALIGN(h + 16, 32);
	ref_b = roundup_pow_of_two(w) * roundup_pow_of_two(h) / 32;
	ref_c = hevc ? 512 * w64 * h64 : ALIGN(w, 64) * (ALIGN(h, 16) / 2 + 24);
	ref_d = ALIGN(hevc ? 64 * w32 * h32 : 128 * p->mbs, SZ_16K);
	tile = hevc ? 2304 * w32 * h64 : ALIGN(w, 16) * ALIGN(h, 64);

	ret |= ave_alloc(ave, &sess->hdr, AVE_HDR_BYTES, nomap);
	for (unsigned int i = 0; i < p->refs; i++) {
		ret |= ave_alloc(ave, &sess->ref_a[i], ref_a, nomap);
		ret |= ave_alloc(ave, &sess->ref_b[i], ref_b, nomap);
		ret |= ave_alloc(ave, &sess->ref_c[i], ref_c, nomap);
		ret |= ave_alloc(ave, &sess->ref_d[i], ref_d, nomap);
	}
	ret |= ave_alloc(ave, &sess->mb, ALIGN(82 * p->mbs, SZ_16K), nomap);
	ret |= ave_alloc(ave, &sess->row_a, hevc ? SZ_16K : ALIGN(256 * w16, SZ_16K), nomap);
	ret |= ave_alloc(ave, &sess->row_b, ALIGN(hevc ? 768 * w32 : 1024 * w16, SZ_16K), nomap);
	ret |= ave_alloc(ave, &sess->fixed, SZ_16K, nomap);
	for (unsigned int i = 0; i < (hevc ? 4 : AVE_TILE_BUFS); i++) {
		ret |= ave_alloc(ave, &sess->tile[i], tile, nomap);
		ret |= ave_alloc(ave, &sess->tile_aux[i], ALIGN(h, 128), nomap);
	}
	if (hevc)
		for (unsigned int i = 0; i < 2; i++)
			ret |= ave_alloc(ave, &sess->hevc_extra[i], ALIGN(w * h32, SZ_16K), nomap);

	/* Per-slot output; the size of the H.264 frame buffer is not known. */
	frame_buf = ALIGN(DIV_ROUND_UP_ULL((u64)AVE_H264_FRAME_BUF_1080P * p->mbs, AVE_MBS_1080P),
			  SZ_16K);
	frame_buf = max_t(size_t, frame_buf, AVE_H264_FRAME_BUF_1080P);
	for (unsigned int s = 0; s < p->slots; s++) {
		ret |= ave_alloc(ave, &sess->slot[s].coded, p->coded_cap, 0);
		ret |= ave_alloc(ave, &sess->slot[s].meta, AVE_META_BYTES, 0);
		ret |= ave_alloc(ave, &sess->slot[s].aux, hevc ? AVE_SHDR_BYTES : frame_buf,
				 hevc ? 0 : nomap);
	}
	return ret ? -ENOMEM : 0;
}

static void ave_sess_free_bufs(struct ave_dev *ave, struct ave_session *sess)
{
	ave_free(ave, &sess->hdr);
	for (unsigned int i = 0; i < AVE_MAX_REFS; i++) {
		ave_free(ave, &sess->ref_a[i]);
		ave_free(ave, &sess->ref_b[i]);
		ave_free(ave, &sess->ref_c[i]);
		ave_free(ave, &sess->ref_d[i]);
	}
	ave_free(ave, &sess->mb);
	ave_free(ave, &sess->row_a);
	ave_free(ave, &sess->row_b);
	ave_free(ave, &sess->fixed);
	for (unsigned int i = 0; i < AVE_TILE_BUFS; i++) {
		ave_free(ave, &sess->tile[i]);
		ave_free(ave, &sess->tile_aux[i]);
	}
	for (unsigned int i = 0; i < 2; i++)
		ave_free(ave, &sess->hevc_extra[i]);
	for (unsigned int s = 0; s < AVE_MAX_SLOTS; s++) {
		ave_free(ave, &sess->slot[s].coded);
		ave_free(ave, &sess->slot[s].meta);
		ave_free(ave, &sess->slot[s].aux);
	}
}

/* Work out the parameters and allocate the buffers; no firmware access. */
struct ave_session *ave_sess_create(struct ave_ctx *ctx)
{
	struct ave_session *sess;
	int ret;

	sess = kzalloc(sizeof(*sess), GFP_KERNEL);
	if (!sess)
		return ERR_PTR(-ENOMEM);
	ret = ave_sess_params(ctx, &sess->p);
	if (!ret)
		ret = ave_sess_alloc(ctx->ave, sess);
	if (ret) {
		ave_sess_free_bufs(ctx->ave, sess);
		kfree(sess);
		return ERR_PTR(ret);
	}
	for (unsigned int s = 0; s < sess->p.slots; s++)
		ave_sess_prepare_slot(sess, s);
	dev_dbg(ctx->ave->dev, "session %ux%u codec %u profile %u level %u, %u slots, %u refs\n",
		sess->p.width, sess->p.height, sess->p.codec, sess->p.profile, sess->p.level,
		sess->p.slots, sess->p.refs);
	return sess;
}

/*
 * Free the buffers. A firmware that stopped answering may still write to
 * them, and a freed DART range is soon handed out again, so in that case
 * they stay allocated (until reboot, like the firmware itself).
 */
void ave_sess_destroy(struct ave_ctx *ctx)
{
	struct ave_session *sess = ctx->sess;

	if (!sess)
		return;
	if (ctx->ave->fw_state == AVE_FW_BROKEN)
		dev_warn(ctx->ave->dev, "keeping the buffers of a failed session\n");
	else
		ave_sess_free_bufs(ctx->ave, sess);
	kfree(sess);
	ctx->sess = NULL;
}

int ave_sess_free_slot(struct ave_session *sess)
{
	for (unsigned int s = 0; s < sess->p.slots; s++)
		if (!sess->slot[s].busy && !sess->slot[s].done)
			return s;
	return -EBUSY;
}

/*
 * Clear what the firmware writes, so the output can be measured: the whole
 * metadata buffer, the first slice header and (H.264) the part of the coded
 * buffer the previous frame used.
 */
void ave_sess_prepare_slot(struct ave_session *sess, unsigned int slot)
{
	struct ave_slot *s = &sess->slot[slot];

	memset(s->meta.cpu, 0, s->meta.size);
	if (sess->p.codec == AVE_CODEC_HEVC) {
		memset(s->aux.cpu, 0, AVE_SHDR_ENTRY_BYTES);
	} else if (s->coded_dirty) {
		memset(s->coded.cpu, 0, min(s->coded_dirty, s->coded.size));
		s->coded_dirty = 0;
	}
}

/* A frame reply: +0x10 our client, +0x20 the slot's subtype, +0x40 status. */
void ave_sess_reply(struct ave_ctx *ctx, const u8 *msg, u32 len)
{
	struct ave_session *sess = ctx->sess;
	u64 client = get_unaligned_le64(msg + AVE_REPLY_CLIENT);
	u32 subtype = get_unaligned_le32(msg + AVE_REPLY_SUBTYPE);
	u32 slot = subtype - AVE_PROC_SUBTYPE;
	struct ave_slot *s;

	if (!sess || len < AVE_REPLY_PROCESS_BYTES || client != sess->client ||
	    slot >= sess->p.slots || !sess->slot[slot].busy) {
		dev_warn(ctx->ave->dev, "stray frame reply (client %llu subtype %#x len %u)\n",
			 client, subtype, len);
		return;
	}
	s = &sess->slot[slot];
	s->status = get_unaligned_le32(msg + AVE_REPLY_STATUS);
	if (s->status)
		dev_dbg(ctx->ave->dev, "frame in slot %u: status %#x\n", slot, s->status);
	s->busy = false;
	s->done = true;
	sess->busy--;
	sess->done_fifo[(sess->done_head + sess->done_count) % AVE_MAX_SLOTS] = slot;
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

/* NAL unit types in an Annex B buffer: is there a key frame / parameter set? */
static void ave_scan_nals(const u8 *p, size_t len, bool hevc, bool *key, bool *params)
{
	for (size_t i = 0; i + 3 < len; i++) {
		u32 type;

		if (p[i] || p[i + 1] || p[i + 2] != 1)
			continue;
		type = hevc ? (p[i + 3] >> 1) & 0x3f : p[i + 3] & 0x1f;
		if (hevc) {
			*key |= type >= 16 && type <= 21;
			*params |= type >= 32 && type <= 34;
		} else {
			*key |= type == 5;
			*params |= type == 7;
		}
		i += 2;
	}
}

static void ave_sess_dump(struct ave_ctx *ctx, struct ave_slot *s, const char *why)
{
	struct ave_session *sess = ctx->sess;

	if (sess->warned)
		return;
	sess->warned = true;
	dev_warn(ctx->ave->dev, "frame %u output: %s (enable dynamic debug for a dump)\n",
		 ctx->dst_sequence, why);
	print_hex_dump_debug("ave coded: ", DUMP_PREFIX_OFFSET, 16, 1, s->coded.cpu, 128, false);
	print_hex_dump_debug("ave meta: ", DUMP_PREFIX_OFFSET, 16, 4, s->meta.cpu, 0x200, false);
	if (sess->p.codec == AVE_CODEC_HEVC)
		print_hex_dump_debug("ave meta params: ", DUMP_PREFIX_OFFSET, 16, 1,
				     s->meta.cpu + AVE_META_HEVC_PARAMS, 128, false);
}

/*
 * Assemble the access unit of @slot into @dst.
 *
 * HEVC: the firmware puts the parameter sets in the metadata buffer, the
 * slice header in the first slice-header entry and the slice data in the
 * coded buffer; metadata words give the two lengths.
 *
 * H.264: where the length is reported is not known yet, so the coded buffer
 * is read as Annex B until the data ends.
 */
int ave_sess_output(struct ave_ctx *ctx, unsigned int slot, struct vb2_v4l2_buffer *dst,
		    bool *keyframe)
{
	struct ave_session *sess = ctx->sess;
	struct ave_slot *s = &sess->slot[slot];
	bool hevc = sess->p.codec == AVE_CODEC_HEVC;
	u8 *out = vb2_plane_vaddr(&dst->vb2_buf, 0);
	size_t room = vb2_plane_size(&dst->vb2_buf, 0);
	bool params = false;
	ssize_t len = 0;

	*keyframe = false;
	if (!out)
		return -EFAULT;
	if (s->status)
		return -EIO;

	if (hevc) {
		const u8 *meta = s->meta.cpu;
		u32 shdr = get_unaligned_le32(meta + AVE_META_HEVC_SHDR);
		u32 data = get_unaligned_le32(meta + AVE_META_HEVC_PAYLOAD);
		ssize_t ps = 0;

		if (ave_starts_annexb(meta + AVE_META_HEVC_PARAMS,
				      AVE_META_BYTES - AVE_META_HEVC_PARAMS)) {
			ps = ave_copy_annexb(out, room, meta + AVE_META_HEVC_PARAMS,
					     AVE_META_BYTES - AVE_META_HEVC_PARAMS, 16);
			if (ps < 0)
				return ps;
		}
		if (shdr > AVE_SHDR_ENTRY_BYTES || data > sess->p.coded_cap ||
		    ps + shdr + data > room) {
			dev_err(ctx->ave->dev, "bad HEVC output sizes %u/%u\n", shdr, data);
			ave_sess_dump(ctx, s, "bad sizes");
			return -EPROTO;
		}
		if (get_unaligned_le32(meta + AVE_META_HEVC_PAYLOAD2) != data)
			dev_dbg(ctx->ave->dev, "HEVC payload words differ: %u/%u\n", data,
				get_unaligned_le32(meta + AVE_META_HEVC_PAYLOAD2));
		memcpy(out + ps, s->aux.cpu, shdr);
		memcpy(out + ps + shdr, s->coded.cpu, data);
		len = ps + shdr + data;
	} else {
		len = ave_copy_annexb(out, room, s->coded.cpu, s->coded.size, 256);
		if (len < 0) {
			dev_err(ctx->ave->dev, "coded frame larger than the CAPTURE buffer\n");
			return len;
		}
		s->coded_dirty = len + 256;
	}

	if (!len) {
		ave_sess_dump(ctx, s, "empty");
		return -ENODATA;
	}
	if (!ave_starts_annexb(out, len))
		ave_sess_dump(ctx, s, "not Annex B");
	ave_scan_nals(out, len, hevc, keyframe, &params);
	if (!ctx->dst_sequence && !params)
		ave_sess_dump(ctx, s, "first frame has no parameter sets");
	vb2_set_plane_payload(&dst->vb2_buf, 0, len);
	return 0;
}
