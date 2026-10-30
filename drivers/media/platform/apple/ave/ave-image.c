// SPDX-License-Identifier: GPL-2.0-only
/*
 * Apple AVE video encoder: constant words of the session commands
 *
 * macOS builds Start and Process in code, from the session settings and
 * its own defaults; nothing is stored. Besides the fields the driver
 * computes (sizes, geometry, profile, level, rate control, GOP, buffer
 * addresses, per-frame values), every captured command on firmware
 * AppleAVE2FW 9003.78.0 holds the same small set of constant words. They
 * are listed here as facts, grouped by where they sit; for most of them
 * the meaning is not known, only that macOS always sets that value. The
 * commands are built in a zeroed buffer: these words first, then the
 * computed fields.
 *
 * Copyright The Aurora Silicon Contributors
 */
#include <linux/kernel.h>
#include <linux/unaligned.h>

#include "ave.h"
#include "ave-regs.h"

/* @count consecutive u32 words at @off, each set to @val. */
struct ave_word {
	u32 off;
	u16 count;
	u32 val;
};

#define W(o, v)		{ .off = (o), .count = 1, .val = (v) }
#define RUN(o, n, v)	{ .off = (o), .count = (n), .val = (v) }

/* IEEE 754 doubles, high words only (the low words are zero) */
#define F64_HI_1_0	0x3ff00000	/* 1.0 */
#define F64_HI_0_5	0x3fe00000	/* 0.5 */
#define F64_HI_M1_0	0xbff00000	/* -1.0 */

/* Session configuration words common to both codecs (Start +0x68..) */
static const struct ave_word ave_start_common[] = {
	/* First surface size: always the 0x12c000 header surface */
	W(0x50, 0x12c000),
	W(0x94, 0x36),
	W(0x214, 0x39d),
	W(0x218, 0x18),
	/* Rate-control doubles: 1.0, 0.5 (the 0.5 s VBV buffer), -1, -1 */
	W(0x28c, F64_HI_1_0),
	W(0x294, F64_HI_0_5),
	W(0x2c4, F64_HI_M1_0),
	W(0x2cc, F64_HI_M1_0),
	W(0x2d0, 1),
	W(0x2e4, 1),
	RUN(0x2f4, 3, 1),
	RUN(0x338, 2, 0xffffffff),
	/* Buffer set: metadata sizes of slots 0..2, +0x13d0 surface size */
	W(0xf58, 0xc000),
	W(0xf68, 0xc000),
	W(0xf78, 0xc000),
	W(0x13d8, 0x4000),
};

static const struct ave_word ave_h264_start[] = {
	/* Codec (1) and four flags after it */
	RUN(0xc0, 5, 1),
	W(0x370, 2),
	/* Codec settings block (+0x2580..): see also ave_start_codec_bytes() */
	W(0x258c, 3),
	W(0x2594, 1),
	W(0x2598, 8),		/* bit depth */
	W(0x25a4, 0x100),
	W(0x25cc, 1),
	W(0x25d4, 1),
	W(0x26dc, 4),
	W(0x26f8, 0x15),
	RUN(0x2704, 2, 1),
	W(0x2734, 1),
	W(0x2738, 0x10),
	W(0x27c0, 8),
	/* Sequence and picture parameter block (+0x2900..) */
	W(0x2940, 1),
	W(0x2970, 0x100000),
	RUN(0x2974, 239, 0x100010),
	W(0x2d30, 0x10),
	W(0x2d34, 1),
	W(0x2d3c, 2),
	RUN(0x2d54, 2, 1),
	W(0x2d5c, 0x1000000),
	W(0x2d60, 5),
	W(0x2d68, 1),
	RUN(0x2d6c, 3, 2),
	W(0x2dc8, 1),
	/* Two runs of -13 */
	RUN(0x3004, 7, 0xfffffff3),
	RUN(0x302c, 7, 0xfffffff3),
	W(0x3048, 1),
	/* Execution-unit info */
	W(0x3150, 1),
	W(0x3158, 1),
};

static const struct ave_word ave_hevc_start[] = {
	/* Codec (2) and four flags after it */
	W(0xc0, 2),
	RUN(0xc4, 4, 1),
	W(0x370, 0x2000002),
	W(0x1358, 0x4000),	/* +0x1350 surface size, fixed for HEVC */
	/* Codec settings block (+0x2580..): see also ave_start_codec_bytes() */
	W(0x2588, 2),
	W(0x2594, 1),
	W(0x2598, 8),		/* bit depth */
	W(0x25a4, 0x100),
	W(0x25cc, 1),
	W(0x25d4, 1),
	W(0x26dc, 4),
	RUN(0x26f8, 2, 0x15),
	W(0x2700, 2),
	RUN(0x2704, 2, 1),
	W(0x2710, 1),
	W(0x2734, 1),
	W(0x2738, 0x10),
	W(0x27c0, 8),
	/* Profile, tier and level block, first copy (+0x2920..) */
	W(0x2920, 0x101),
	W(0x292c, 1),
	W(0x2934, 1),
	W(0x295c, 0x1010001),
	W(0x2964, 1),
	RUN(0x2a8c, 7, 0x1010101),
	W(0x2aa8, 0x101),
	W(0x2abc, 0x1010101),
	W(0x2ac0, 0x101),
	RUN(0x2e38, 7, 4),
	/* Timing pair 1001 / 24000, also at +0x74f0 */
	W(0x3298, 1001),
	W(0x329c, 24000),
	/* Sequence parameter block, second copy (+0x58f0..) */
	W(0x58f8, 1),
	W(0x5900, 1),
	W(0x5928, 0x1010001),
	W(0x5930, 1),
	W(0x5aac, 1),
	W(0x5ae0, 7),		/* log2_max_pic_order_cnt_lsb_minus4: 11-bit POC */
	RUN(0x5ae8, 7, 4),	/* ends with sps_max_dec_pic_buffering_minus1 */
	W(0x5b40, 2),
	W(0x5b48, 3),
	W(0x5b4c, 1),
	W(0x5b54, 1),
	/* Picture parameter and slice defaults (+0x7400..) */
	W(0x7400, 0x100),
	RUN(0x7404, 2, 7),
	W(0x740c, 1),
	W(0x7414, 1),
	W(0x749c, 1),
	W(0x74b0, 0x100),
	W(0x74b4, 5),
	W(0x74bc, 1),
	RUN(0x74c0, 3, 2),
	W(0x74f0, 1001),
	W(0x74f4, 24000),
	W(0x7500, 0x101),
	W(0x7504, 1),
	W(0x7508, 3),
	W(0x750c, 5),
	RUN(0x7510, 3, 0x17),
	RUN(0x7524, 2, 0xbebb),
	W(0x7558, 0x100),
	/* Rate-control block (+0x9600..), with two runs of -13 */
	W(0x960c, 2),
	RUN(0x9614, 7, 0xfffffff3),
	RUN(0x9634, 7, 0xfffffff3),
	W(0x9654, 0x100),
	W(0x9e64, 0x10000),
	W(0xb734, 2),
	W(0xb774, 1),
	/* Execution-unit info */
	RUN(0x13ef4, 2, 1),
	W(0x13f00, 1),
};

/*
 * HEVC with frame reordering off: a low-delay P reference structure (the
 * reference picture sets hold the delta POCs -1 .. -4). All zero with
 * B-frames.
 */
static const struct ave_word ave_hevc_start_low_delay[] = {
	W(0xe120, 4), W(0xe154, 4), W(0xe17c, 0x01010101),
	W(0xe1dc, 4), W(0xe1e4, 0x01010101),
	W(0xe204, -1), W(0xe208, -2), W(0xe20c, -3), W(0xe210, -4),
	W(0xe244, 1), W(0xe248, 2), W(0xe24c, 3), W(0xe250, 4),
	W(0xe284, 4), W(0xe2b8, 1), W(0xe2e0, 1), W(0xe340, 1), W(0xe348, 1),
	W(0xe368, -1), W(0xe3a8, 1), W(0xe3e8, 1),
	W(0xe41c, 2), W(0xe444, 0x101), W(0xe4a4, 2), W(0xe4ac, 0x101),
	W(0xe4cc, -1), W(0xe4d0, -2),
	W(0xe50c, 1), W(0xe510, 2), W(0xe54c, 2),
	W(0xe580, 3), W(0xe5a8, 0x10101), W(0xe608, 3), W(0xe610, 0x10101),
	W(0xe630, -1), W(0xe634, -2), W(0xe638, -3),
	W(0xe670, 1), W(0xe674, 2), W(0xe678, 3), W(0xe6b0, 3),
};

/* Process: the first-frame flag and per-frame values are computed. */
static const struct ave_word ave_h264_process[] = {
	/* Codec block */
	W(0x99c, 1),
	W(0x9a8, 2),
	W(0x9bc, 1),
	W(0x9d4, 1),
	W(0xeb4, 1),
	W(0x12bc, 0xffffffff),
	/* Picture-management block */
	W(0x29fc, 0xffffffff),
	W(0x6238, 5),
	W(0x6278, 14400000),	/* presentation timescale, low word */
};

static const struct ave_word ave_hevc_process[] = {
	/* Codec block */
	W(0x9a8, 1),
	W(0x9b8, 2),
	W(0xab8, 0x10100),
	W(0xb44, 0x10000),
	RUN(0xb4c, 2, 5),
	RUN(0xd70, 2, 0xffffffff),
	W(0xd7c, 1),
	/* Picture-management block */
	W(0x74cc, 0xffffffff),
	W(0x74d0, 0x100),
	W(0x74e0, 1),
	W(0xad08, 5),
	W(0xad3c, 0x403e0000),	/* frame rate 30.0, rewritten per session */
	W(0xad48, 14400000),	/* presentation timescale, low word */
	W(0xad68, 0xffffffff),
};

static void ave_put_words(u8 *cmd, size_t len, const struct ave_word *w, size_t n)
{
	for (size_t i = 0; i < n; i++)
		for (unsigned int j = 0; j < w[i].count; j++) {
			size_t off = w[i].off + 4 * j;

			if (WARN_ON_ONCE(off + 4 > len))
				return;
			put_unaligned_le32(w[i].val, cmd + off);
		}
}

/*
 * The 8 bytes at Start +0x2580 look like a DART address but are separate
 * settings: byte 0 is 4 for HEVC, byte 3 is 1, byte 5 is 1 unless the
 * client turned frame reordering off (H.264 only; HEVC keeps 1).
 */
static void ave_start_codec_bytes(u8 *cmd, enum ave_codec codec, bool reorder_off)
{
	cmd[0x2580] = codec == AVE_CODEC_HEVC ? 4 : 0;
	cmd[0x2583] = 1;
	cmd[0x2585] = codec == AVE_CODEC_H264 && reorder_off ? 0 : 1;
}

void ave_image_start(u8 *cmd, size_t len, enum ave_codec codec, bool reorder_off,
		     bool hevc_low_delay)
{
	ave_put_words(cmd, len, ave_start_common, ARRAY_SIZE(ave_start_common));
	if (codec == AVE_CODEC_HEVC) {
		ave_put_words(cmd, len, ave_hevc_start, ARRAY_SIZE(ave_hevc_start));
		if (hevc_low_delay)
			ave_put_words(cmd, len, ave_hevc_start_low_delay,
				      ARRAY_SIZE(ave_hevc_start_low_delay));
	} else {
		ave_put_words(cmd, len, ave_h264_start, ARRAY_SIZE(ave_h264_start));
	}
	ave_start_codec_bytes(cmd, codec, reorder_off);
}

void ave_image_process(u8 *cmd, size_t len, enum ave_codec codec)
{
	if (codec == AVE_CODEC_HEVC)
		ave_put_words(cmd, len, ave_hevc_process, ARRAY_SIZE(ave_hevc_process));
	else
		ave_put_words(cmd, len, ave_h264_process, ARRAY_SIZE(ave_h264_process));
}
