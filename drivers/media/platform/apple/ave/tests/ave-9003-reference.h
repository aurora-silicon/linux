/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Frozen 9003 builder from 23e431a98962, for byte-regression tests only.
 * Copyright The Aurora Silicon Contributors
 */
struct ave_ref_start_layout {
	u16 ref_a, ref_b, ref_c, ref_c_alias, ref_d;
	u16 ref_e, ref_f, ref_stride, ref_c_stride;
	u16 mb, coded, meta, row_a, row_b, fixed, hevc_extra;
	u16 tiles[8];
	u16 refs_count, work_count, lfs_count, slots_count, meta_count;
	u16 mb_count, row_a_count, row_b_count, fixed_count, tiles_count;
	u16 codec;
	u16 flag_8bit;
	u16 const_214;
	u16 const_218;
	u16 fps;
	u16 rc_mode;
	u16 bitrate;
	u16 init_qp;
	u16 limits;
	u16 limit_bytes;
	u16 limit_secs;
	u16 f64_288;
	u16 f64_290;
	u16 min_qp;
	u16 max_qp;
	u16 gop_flags;
	u16 p_frames;
	u16 max_b;
	u16 ref_depth;
	u16 idr_interval;
	u16 f64_2c0;
	u16 f64_2c8;
	u16 rc_flags;
	u16 min_qp2;
	u16 max_qp2;
	u16 rc_310;
	u16 size_class;
	u16 coded_w;
	u16 coded_h;
	u16 codec_flags;
	u16 h264_380;
	u16 h264_384;
	u16 codec_2588;
	u16 bit_depth;
	u16 coded_h2;
	u16 h264_profile;
	u16 h264_level;
	u16 h264_max_refs;
	u16 h264_width_mbs;
	u16 h264_height_mbs;
	u16 h264_crop;
	u16 h264_crop_left;
	u16 h264_crop_right;
	u16 h264_crop_top;
	u16 h264_crop_bottom;
	u16 h264_cabac;
	u16 h264_8x8;
	u16 hevc_pad;
	u16 hevc_rc_270c;
	u16 hevc_rc_9604;
	u16 hevc_rc_9608;
	u16 hevc_profile_a;
	u16 hevc_compat_a;
	u16 hevc_8bit_a;
	u16 hevc_level_a;
	u16 hevc_profile_b;
	u16 hevc_compat_b;
	u16 hevc_8bit_b;
	u16 hevc_level_b;
	u16 hevc_width;
	u16 hevc_height;
	u16 hevc_width_ctb;
	u16 hevc_height_ctb;
	u16 hevc_conf;
	u16 hevc_conf_left;
	u16 hevc_conf_right;
	u16 hevc_conf_top;
	u16 hevc_conf_bottom;
	u16 hevc_depth_luma;
	u16 hevc_depth_chroma;
	u16 hevc_refs;
	u16 hevc_reorder_copy;
	u16 hevc_dpb_minus1;
	u16 hevc_reorder;
	u16 hevc_reorder_on;
	u16 geometry_mode, geometry_depth;
	u16 profile_idc, level_idc, max_refs_copy, mb_size, mb_height;
	u16 crop_copy, crop_right_copy, crop_bottom_copy;
	u16 codec_byte, cabac_copy, transform_copy, hevc_cq, hevc_cq_copy;
	u16 hevc_cq_codec, hevc_cq_picture;
};

struct ave_ref_proc_layout {
	u16 first_frame;
	u16 y, uv; /* {u64 address, u32 size, u32 stride} */
	u16 in_w, in_h;
	u16 coded_cap;
	u16 fps_int;
	u16 bitrate;
	u16 limits, limit_bytes, limit_secs;
	u16 const5;
	u16 fps; /* f64 */
	u16 timescale; /* u64 */
	u16 slot;
	u16 coded, meta; /* u64 addresses */
	u16 ordinal;
	u16 pts; /* u64 */
	u16 force_key; /* 1 on a forced key frame */
	u16 force_type; /* 2 on a forced key frame */
	u16 frame_buf; /* H.264 only */
	u16 shdr_table; /* HEVC only */
	u16 crop; /* HEVC only */
	u16 meta_size, extra, frame_size;
	u16 low_delay; /* HEVC only: 3 with reordering off */
};

struct ave_ref_fw_layout {
	const char *name;
	size_t bytes[2][AVE_IMAGE_COUNT];
	struct ave_ref_start_layout start[2];
	struct ave_ref_proc_layout process[2];
	u32 hdr_bytes, meta_bytes;
	u32 process_timeout_us;
	u8 process_subtype, max_slots, mcc_dsid;
	bool counted;
};

static const struct ave_ref_fw_layout ave_reference_layout = {
	.name = "9003.78.0 / 25G83",
	.bytes = { { 0x3180, 0x63d8 }, { 0x13f28, 0xb1c0 } },
	.hdr_bytes = 0x12c000,
	.meta_bytes = 0xc000,
	.process_timeout_us = 94400000,
	.process_subtype = 0x15,
	.max_slots = 5,
	.mcc_dsid = 0x12,
	.start = {
		[0] = {
			.ref_a = 0x390,
			.ref_b = 0x3a0,
			.ref_c = 0x7d0,
			.ref_c_alias = 0x7e0,
			.ref_d = 0x1130,
			.ref_stride = 0x20,
			.ref_c_stride = 0x20,
			.mb = 0xc10,
			.coded = 0xd50,
			.meta = 0xf50,
			.row_a = 0x1350,
			.row_b = 0x1390,
			.fixed = 0x13d0,
			.hevc_extra = 0x2520,
			.tiles = { 0x1410, 0x1430, 0x1490, 0x14b0, 0x1510,
				0x1530, 0x1590, 0x15b0 },
			.codec = 0xc0,
			.flag_8bit = 0x208,
			.const_214 = 0x214,
			.const_218 = 0x218,
			.fps = 0x220,
			.rc_mode = 0x234,
			.bitrate = 0x238,
			.init_qp = 0x240,
			.limits = 0x250,
			.limit_bytes = 0x258,
			.limit_secs = 0x260,
			.f64_288 = 0x288,
			.f64_290 = 0x290,
			.min_qp = 0x298,
			.max_qp = 0x29c,
			.gop_flags = 0x2a0,
			.p_frames = 0x2ac,
			.max_b = 0x2b0,
			.ref_depth = 0x2b4,
			.idr_interval = 0x2b8,
			.f64_2c0 = 0x2c0,
			.f64_2c8 = 0x2c8,
			.rc_flags = 0x300,
			.min_qp2 = 0x308,
			.max_qp2 = 0x30c,
			.rc_310 = 0x310,
			.size_class = 0x320,
			.coded_w = 0x368,
			.coded_h = 0x36c,
			.codec_flags = 0x370,
			.h264_380 = 0x380,
			.h264_384 = 0x384,
			.codec_2588 = 0x2588,
			.bit_depth = 0x2598,
			.coded_h2 = 0x25dc,
			.h264_profile = 0x291c,
			.h264_level = 0x2938,
			.h264_max_refs = 0x2d44,
			.h264_width_mbs = 0x2d4c,
			.h264_height_mbs = 0x2d50,
			.h264_crop = 0x2db4,
			.h264_crop_left = 0x2db8,
			.h264_crop_right = 0x2dbc,
			.h264_crop_top = 0x2dc0,
			.h264_crop_bottom = 0x2dc4,
			.h264_cabac = 0x2fd8,
			.h264_8x8 = 0x3023,
			.hevc_pad = 0x26e0,
			.hevc_rc_270c = 0x270c,
			.hevc_rc_9604 = 0x9604,
			.hevc_rc_9608 = 0x9608,
			.hevc_profile_a = 0x2938,
			.hevc_compat_a = 0x293d,
			.hevc_8bit_a = 0x2962,
			.hevc_level_a = 0x296c,
			.hevc_profile_b = 0x5904,
			.hevc_compat_b = 0x5909,
			.hevc_8bit_b = 0x592e,
			.hevc_level_b = 0x5938,
			.hevc_width = 0x5ab4,
			.hevc_height = 0x5ab8,
			.hevc_width_ctb = 0x5abc,
			.hevc_height_ctb = 0x5ac0,
			.hevc_conf = 0x5ac4,
			.hevc_conf_left = 0x5ac8,
			.hevc_conf_right = 0x5acc,
			.hevc_conf_top = 0x5ad0,
			.hevc_conf_bottom = 0x5ad4,
			.hevc_depth_luma = 0x5ad8,
			.hevc_depth_chroma = 0x5adc,
			.hevc_refs = 0x259c,
			.hevc_reorder_copy = 0x2e54,
			.hevc_dpb_minus1 = 0x5b00,
			.hevc_reorder = 0x5b04,
			.hevc_reorder_on = 0x95fc,
		},
		[1] = {
			.ref_a = 0x390,
			.ref_b = 0x3a0,
			.ref_c = 0x7d0,
			.ref_c_alias = 0x7e0,
			.ref_d = 0x1130,
			.ref_stride = 0x20,
			.ref_c_stride = 0x20,
			.mb = 0xc10,
			.coded = 0xd50,
			.meta = 0xf50,
			.row_a = 0x1350,
			.row_b = 0x1390,
			.fixed = 0x13d0,
			.hevc_extra = 0x2520,
			.tiles = { 0x1410, 0x1430, 0x1490, 0x14b0, 0x1510,
				0x1530, 0x1590, 0x15b0 },
			.codec = 0xc0,
			.flag_8bit = 0x208,
			.const_214 = 0x214,
			.const_218 = 0x218,
			.fps = 0x220,
			.rc_mode = 0x234,
			.bitrate = 0x238,
			.init_qp = 0x240,
			.limits = 0x250,
			.limit_bytes = 0x258,
			.limit_secs = 0x260,
			.f64_288 = 0x288,
			.f64_290 = 0x290,
			.min_qp = 0x298,
			.max_qp = 0x29c,
			.gop_flags = 0x2a0,
			.p_frames = 0x2ac,
			.max_b = 0x2b0,
			.ref_depth = 0x2b4,
			.idr_interval = 0x2b8,
			.f64_2c0 = 0x2c0,
			.f64_2c8 = 0x2c8,
			.rc_flags = 0x300,
			.min_qp2 = 0x308,
			.max_qp2 = 0x30c,
			.rc_310 = 0x310,
			.size_class = 0x320,
			.coded_w = 0x368,
			.coded_h = 0x36c,
			.codec_flags = 0x370,
			.h264_380 = 0x380,
			.h264_384 = 0x384,
			.codec_2588 = 0x2588,
			.bit_depth = 0x2598,
			.coded_h2 = 0x25dc,
			.h264_profile = 0x291c,
			.h264_level = 0x2938,
			.h264_max_refs = 0x2d44,
			.h264_width_mbs = 0x2d4c,
			.h264_height_mbs = 0x2d50,
			.h264_crop = 0x2db4,
			.h264_crop_left = 0x2db8,
			.h264_crop_right = 0x2dbc,
			.h264_crop_top = 0x2dc0,
			.h264_crop_bottom = 0x2dc4,
			.h264_cabac = 0x2fd8,
			.h264_8x8 = 0x3023,
			.hevc_pad = 0x26e0,
			.hevc_rc_270c = 0x270c,
			.hevc_rc_9604 = 0x9604,
			.hevc_rc_9608 = 0x9608,
			.hevc_profile_a = 0x2938,
			.hevc_compat_a = 0x293d,
			.hevc_8bit_a = 0x2962,
			.hevc_level_a = 0x296c,
			.hevc_profile_b = 0x5904,
			.hevc_compat_b = 0x5909,
			.hevc_8bit_b = 0x592e,
			.hevc_level_b = 0x5938,
			.hevc_width = 0x5ab4,
			.hevc_height = 0x5ab8,
			.hevc_width_ctb = 0x5abc,
			.hevc_height_ctb = 0x5ac0,
			.hevc_conf = 0x5ac4,
			.hevc_conf_left = 0x5ac8,
			.hevc_conf_right = 0x5acc,
			.hevc_conf_top = 0x5ad0,
			.hevc_conf_bottom = 0x5ad4,
			.hevc_depth_luma = 0x5ad8,
			.hevc_depth_chroma = 0x5adc,
			.hevc_refs = 0x259c,
			.hevc_reorder_copy = 0x2e54,
			.hevc_dpb_minus1 = 0x5b00,
			.hevc_reorder = 0x5b04,
			.hevc_reorder_on = 0x95fc,
		},
	},
	.process = {
		[0] = {
			.first_frame = 0x12c0,
			.y = 0x5830,
			.uv = 0x5850,
			.in_w = 0x58e8,
			.in_h = 0x58ec,
			.coded_cap = 0x61c8,
			.fps_int = 0x1610,
			.bitrate = 0x1620,
			.limits = 0x1628,
			.limit_bytes = 0x1630,
			.limit_secs = 0x1638,
			.const5 = 0x6238,
			.fps = 0x6268,
			.timescale = 0x6278,
			.slot = 0x61b4,
			.coded = 0x61b8,
			.meta = 0x61c0,
			.ordinal = 0x6228,
			.pts = 0x6270,
			.force_key = 0x1658,
			.force_type = 0x29f8,
			.frame_buf = 0x58c8,
		},
		[1] = {
			.first_frame = 0x5d90,
			.y = 0xa300,
			.uv = 0xa320,
			.in_w = 0xa3b8,
			.in_h = 0xa3bc,
			.coded_cap = 0xac98,
			.fps_int = 0x60e0,
			.bitrate = 0x60f0,
			.limits = 0x60f8,
			.limit_bytes = 0x6100,
			.limit_secs = 0x6108,
			.const5 = 0xad08,
			.fps = 0xad38,
			.timescale = 0xad48,
			.slot = 0xac84,
			.coded = 0xac88,
			.meta = 0xac90,
			.ordinal = 0xacf8,
			.pts = 0xad40,
			.force_key = 0x6128,
			.force_type = 0x74c8,
			.shdr_table = 0xd88,
			.crop = 0x74dc,
			.low_delay = 0xcf8,
		},
	},
};

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
static u32 ave_ref_session_arg2(struct ave_ctx *ctx)
{
	if (ctx->ave->layout->counted && ctx->sess->p.codec == AVE_CODEC_H264)
		return ctx->sess->p.bframes ? AVE_SESSION_ARG2_H264 : 0x140;
	if (ave_cmd_others_open(ctx))
		return 0;
	return ctx->sess->p.codec == AVE_CODEC_HEVC ? AVE_SESSION_ARG2_HEVC :
						      AVE_SESSION_ARG2_H264;
}

static u32 ave_ref_session_setup_arg2(struct ave_ctx *ctx)
{
	return ave_cmd_others_open(ctx) ? AVE_SESSION_ARG2_SETUP_SHARED :
					  AVE_SESSION_ARG2_SETUP;
}

static void ave_cmd_header(struct ave_ctx *ctx, u8 *cmd, enum ave_opcode op,
			   u32 echo, u32 arg0, u32 arg2)
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

#define AVE_START_ARRAY_LEN 30
#define AVE_START_INIT_QP_VALUE 26

static void ave_start_surfaces(struct ave_session *sess, u8 *cmd,
			       const struct ave_ref_fw_layout *fw)
{
	const struct ave_params *p = &sess->p;
	bool hevc = p->codec == AVE_CODEC_HEVC;
	unsigned int tiles = hevc ? 4 : AVE_TILE_BUFS;
	const struct ave_ref_start_layout *l = &fw->start[p->codec - 1];

	/* Clear every descriptor the image may carry, then fill ours. */
	for (unsigned int i = 0; i < AVE_MAX_REFS; i++) {
		ave_put_surface(cmd, l->ref_a + l->ref_stride * i, 0, 0);
		ave_put_surface(cmd, l->ref_b + l->ref_stride * i, 0, 0);
		ave_put_surface(cmd, l->ref_c + l->ref_c_stride * i, 0, 0);
		ave_put_surface(cmd, l->ref_c_alias + l->ref_c_stride * i, 0,
				0);
		ave_put_surface(cmd, l->ref_d + 0x10 * i, 0, 0);
	}
	for (unsigned int s = 0; s < AVE_START_ARRAY_LEN; s++) {
		ave_put_surface(cmd, l->coded + 0x10 * s, 0, 0);
		ave_put_surface(cmd, l->meta + 0x10 * s, 0, 0);
	}

	ave_put_buf(cmd, 0x48, &sess->hdr, fw->hdr_bytes);
	ave_put_surface(cmd, 0x58, 0, 0);
	for (unsigned int i = 0; i < p->refs; i++) {
		ave_put_buf(cmd, l->ref_a + l->ref_stride * i, &sess->ref_a[i],
			    sess->ref_a[i].size);
		ave_put_buf(cmd, l->ref_b + l->ref_stride * i, &sess->ref_b[i],
			    sess->ref_b[i].size);
		ave_put_buf(cmd, l->ref_c + l->ref_c_stride * i,
			    &sess->ref_c[i], sess->ref_c[i].size);
		ave_put_surface(cmd, l->ref_c_alias + l->ref_c_stride * i,
				sess->ref_c[i].dma, 0);
		ave_put_buf(cmd, l->ref_d + 0x10 * i, &sess->ref_d[i],
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
				ave_put_buf(cmd, l->mb + 0x10 * (i + 1),
					    &sess->mb_extra[i],
					    sess->mb_extra[i].size);
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
		ave_put_buf(cmd, l->coded + 0x10 * s, &sess->slot[s].coded,
			    p->coded_cap);
		ave_put_buf(cmd, l->meta + 0x10 * s, &sess->slot[s].meta,
			    sess->slot[s].meta.size);
	}
	ave_put_buf(cmd, l->row_a, &sess->row_a, sess->row_a.size);
	ave_put_buf(cmd, l->row_b, &sess->row_b, sess->row_b.size);
	ave_put_buf(cmd, l->fixed, &sess->fixed, SZ_16K);
	for (unsigned int i = 0; i < AVE_TILE_BUFS; i++) {
		u32 off = l->tiles[i];

		if (i < tiles) {
			ave_put_buf(cmd, off, &sess->tile[i],
				    sess->tile[i].size);
			ave_put_buf(cmd, off + 0x10, &sess->tile_aux[i],
				    sess->tile_aux[i].size);
		} else {
			ave_put_surface(cmd, off, 0, 0);
			ave_put_surface(cmd, off + 0x10, 0, 0);
		}
	}
	if (hevc)
		for (unsigned int i = 0; i < 2; i++)
			ave_put_buf(cmd, l->hevc_extra + 0x10 * i,
				    &sess->hevc_extra[i],
				    sess->hevc_extra[i].size);
}

static void ave_start_put32(u8 *cmd, u32 off, u32 val)
{
	if (off)
		ave_put32(cmd, off, val);
}

static void ave_start_config(const struct ave_params *p, u8 *cmd,
			     const struct ave_ref_fw_layout *fw)
{
	bool hevc = p->codec == AVE_CODEC_HEVC;
	bool cq = p->rc_mode == 3;
	u32 depth = p->bit_depth ?: 8;
	u32 crop_r = (p->cw - p->width) / 2, crop_b = (p->ch - p->height) / 2;
	bool crop = crop_r || crop_b;
	const struct ave_ref_start_layout *l = &fw->start[p->codec - 1];

	ave_start_put32(cmd, l->codec, p->codec);
	ave_start_put32(
		cmd, l->flag_8bit,
		fw->counted ?
			0x10000 | (hevc ? (depth == 8 ? 0x41 : 0x40) : 0) |
				(cq ? 0x10 : 0) :
		hevc ? 1 :
		       0);
	ave_start_put32(cmd, l->geometry_mode,
			(hevc ? 0x40 : 0) | (cq ? 0 : 2));
	ave_start_put32(cmd, l->geometry_depth, depth);
	ave_start_put32(cmd, l->const_214, 0x39d);
	ave_start_put32(cmd, l->const_218, 0x18);
	ave_start_put32(cmd, l->fps, p->fps_int);

	/* Rate control */
	ave_start_put32(cmd, l->rc_mode, p->rc_mode);
	ave_start_put32(cmd, l->bitrate, p->bitrate);
	for (unsigned int i = 0; i < 3; i++)
		ave_put32(cmd, (l->init_qp + 4 * i),
			  cq ? p->initial_qp : AVE_START_INIT_QP_VALUE);
	ave_start_put32(cmd, l->limits, p->limit_bytes ? 1 : 0);
	if (l->limit_bytes && p->limit_bytes) {
		ave_put64(cmd, l->limit_bytes,
			  ave_f64((u64)p->limit_bytes * (fw->counted ? 8 : 1),
				  1));
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
	ave_start_put32(
		cmd, l->gop_flags,
		(p->bframes ? 0x600 : 0x200) |
			(fw->counted && cq ? 0x10000 | (hevc ? 0x100 : 0) : 0));
	ave_start_put32(cmd, l->p_frames, p->intra_only ? 0 : 1);
	ave_start_put32(cmd, l->max_b, p->bframes ? 3 : 0);
	ave_start_put32(cmd, l->ref_depth, p->bframes ? 3 : 1);
	ave_start_put32(cmd, l->idr_interval, p->gop);

	/* Picture size */
	ave_start_put32(cmd, l->size_class,
			p->width * p->height > 1280 * 720 ? 3 : 1);
	ave_start_put32(cmd, l->coded_w, p->cw);
	ave_start_put32(cmd, l->coded_h, p->ch);
	ave_start_put32(cmd, l->coded_h2, p->ch);
	ave_start_put32(cmd, l->codec_flags, hevc ? 0x2000002 : 2);
	ave_start_put32(cmd, l->bit_depth, depth);

	if (!hevc) {
		ave_start_put32(cmd, l->h264_384,
				p->width * p->height <= 320 * 240 ? 0x40 :
								    0x10);
		ave_start_put32(cmd, l->codec_2588, p->high ? 2 : 0);
		ave_start_put32(cmd, l->h264_profile, p->profile);
		ave_start_put32(cmd, l->h264_level, p->level);
		ave_start_put32(cmd, l->h264_max_refs,
				(p->refs - 1) |
					(fw->counted ? (p->cw / 16) << 16 : 0));
		if (fw->counted) {
			static const u8 level_idc[] = {
				10, 9,	11, 12, 13, 20, 21, 22, 30,
				31, 32, 40, 41, 42, 50, 51, 52,
			};

			ave_put32(cmd, l->profile_idc,
				  p->high	  ? 100 :
				  p->profile == 2 ? 66 :
						    77);
			ave_put32(cmd, l->level_idc, level_idc[p->level - 1]);
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

	ave_start_put32(cmd, l->hevc_pad,
			1 | (p->cw > p->width ? BIT(16) | BIT(18) : 0) |
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

static void ave_reference_start(struct ave_ctx *ctx, u8 *cmd, size_t len)
{
	const struct ave_ref_fw_layout *fw = &ave_reference_layout;
	const struct ave_params *p = &ctx->sess->p;

	ave_image_start(cmd, len, ctx->ave->layout, p->codec, p->reorder_off,
			p->codec == AVE_CODEC_HEVC && !p->bframes);
	ave_cmd_header(ctx, cmd, AVE_OP_START, AVE_OP_START, AVE_SESSION_ARG0,
		       ave_ref_session_setup_arg2(ctx));
	ave_start_surfaces(ctx->sess, cmd, fw);
	ave_start_config(p, cmd, fw);
}

#define AVE_PROC_TIMESCALE 14400000ULL
#define AVE_PROC_SHDR_ENTRIES 256
#define AVE_PROC_SHDR_STRIDE 0x400
#define AVE_PROC_FORCED_TYPE 2
#define AVE_PROC_LOW_DELAY 3

static void ave_reference_process(struct ave_ctx *ctx, u8 *cmd, size_t len,
				  unsigned int slot, dma_addr_t input,
				  u32 bitrate, bool force_key)
{
	struct ave_session *sess = ctx->sess;
	const struct ave_params *p = &sess->p;
	const struct ave_ref_fw_layout *fw = &ave_reference_layout;
	const struct ave_ref_proc_layout *l = &fw->process[p->codec - 1];
	struct ave_slot *s = &sess->slot[slot];
	dma_addr_t y = input;
	u64 pts;

	ave_image_process(cmd, len, ctx->ave->layout, p->codec);
	ave_cmd_header(ctx, cmd, AVE_OP_PROCESS, fw->process_subtype + slot, 0,
		       ave_ref_session_arg2(ctx));
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
		ave_put32(cmd, l->crop,
			  p->cw != p->width || p->ch != p->height);

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
		ave_put32(cmd, l->frame_size,
			  p->rc_mode == 3 ? 0 : s->aux.size);
	if (l->frame_buf)
		ave_put64(cmd, l->frame_buf,
			  fw->counted && p->rc_mode == 3 ? 0 : s->aux.dma);
	if (l->shdr_table)
		for (unsigned int i = 0; i < AVE_PROC_SHDR_ENTRIES; i++)
			ave_put64(cmd, l->shdr_table + 8 * i,
				  s->aux.dma + i * AVE_PROC_SHDR_STRIDE);
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
}
