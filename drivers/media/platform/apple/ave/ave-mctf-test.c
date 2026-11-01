// SPDX-License-Identifier: GPL-2.0-only
/*
 * MCTF Start and Process images against the MacBook Neo captures: every
 * constant word macOS set in six sessions (header words included), the
 * surface addresses, and the per-frame words over the first seven frames.
 * The expected tables here are kept apart from the driver's own tables.
 *
 * Copyright The Aurora Silicon Contributors
 */
#include <kunit/test.h>
#include <linux/module.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/unaligned.h>

#include "ave.h"
#include "ave-regs.h"
#include "ave-mctf.h"

#define T_START_BYTES	0x13f28
#define T_PROCESS_BYTES	0xb1c0
#define T_CREATED	0x5a5a5a5aULL
#define T_CLIENT	10

/* Every non-zero constant u32 word of the captured Start */
static const struct ave_mctf_word t_start_const[] = {
	{ 0x0, 0x6 }, { 0x8, 0x1f4 }, { 0x18, 0x3 },
	{ 0x1c, 0x2 }, { 0x20, 0x6 }, { 0x24, 0xc8 },
	{ 0x28, 0x3c0 }, { 0x2c, 0x1e0 }, { 0x30, 0x5a06e00 },
	{ 0x4c, 0x100 }, { 0x78, 0x200 }, { 0x7c, 0x1 },
	{ 0x80, 0x50 }, { 0x94, 0x3c0 }, { 0x220, 0x1e },
	{ 0x338, 0x1 }, { 0x33c, 0xffffffff }, { 0x368, 0x780 },
	{ 0x36c, 0x438 }, { 0x370, 0x2000002 }, { 0x394, 0x100 },
	{ 0x398, 0x1fe000 }, { 0x3a4, 0x100 }, { 0x3a8, 0x20000 },
	{ 0x3b4, 0x100 }, { 0x3b8, 0x1fe000 }, { 0x3c4, 0x100 },
	{ 0x3c8, 0x20000 }, { 0x3d4, 0x100 }, { 0x3d8, 0x1fe000 },
	{ 0x3e4, 0x100 }, { 0x3e8, 0x20000 }, { 0x7d4, 0x100 },
	{ 0x7d8, 0x3fc00 }, { 0x7e4, 0x100 }, { 0x7f4, 0x100 },
	{ 0x7f8, 0x3fc00 }, { 0x804, 0x100 }, { 0x814, 0x100 },
	{ 0x818, 0x3fc00 }, { 0x824, 0x100 }, { 0x834, 0x100 },
	{ 0x838, 0x3fc00 }, { 0x844, 0x100 }, { 0x854, 0x100 },
	{ 0x858, 0x3fc00 }, { 0x864, 0x100 }, { 0xc14, 0x100 },
	{ 0xc18, 0xa4000 }, { 0xc24, 0x100 }, { 0xc28, 0xa4000 },
	{ 0xc34, 0x100 }, { 0xc38, 0xa4000 }, { 0xcb4, 0x100 },
	{ 0xcb8, 0x4c000 }, { 0xcc4, 0x100 }, { 0xcc8, 0x4c000 },
	{ 0xcd4, 0x100 }, { 0xcd8, 0x4c000 }, { 0xd54, 0x100 },
	{ 0xd58, 0x2f8000 }, { 0xd64, 0x100 }, { 0xd68, 0x2f8000 },
	{ 0xf54, 0x100 }, { 0xf58, 0xc000 }, { 0xf64, 0x100 },
	{ 0xf68, 0xc000 }, { 0x1134, 0x100 }, { 0x1138, 0x20000 },
	{ 0x1144, 0x100 }, { 0x1148, 0x20000 }, { 0x1154, 0x100 },
	{ 0x1158, 0x20000 }, { 0x1354, 0x100 }, { 0x1358, 0x78000 },
	{ 0x1394, 0x100 }, { 0x1398, 0xc000 }, { 0x13d4, 0x100 },
	{ 0x13d8, 0x4000 }, { 0x1414, 0x100 }, { 0x1418, 0x23dc00 },
	{ 0x1424, 0x100 }, { 0x1428, 0x480 }, { 0x1434, 0x100 },
	{ 0x1438, 0x23dc00 }, { 0x1444, 0x100 }, { 0x1448, 0x480 },
	{ 0x1494, 0x100 }, { 0x1498, 0x23dc00 }, { 0x14a4, 0x100 },
	{ 0x14a8, 0x480 }, { 0x14b4, 0x100 }, { 0x14b8, 0x23dc00 },
	{ 0x14c4, 0x100 }, { 0x14c8, 0x480 }, { 0x1e54, 0x100 },
	{ 0x1e58, 0x3fc00 }, { 0x1e64, 0x100 }, { 0x1e74, 0x100 },
	{ 0x1e78, 0x3fc00 }, { 0x1e84, 0x100 }, { 0x1e94, 0x100 },
	{ 0x1e98, 0x3fc00 }, { 0x1ea4, 0x100 }, { 0x1eb4, 0x100 },
	{ 0x1eb8, 0x3fc00 }, { 0x1ec4, 0x100 }, { 0x1ed4, 0x100 },
	{ 0x1ed8, 0x3fc00 }, { 0x1ee4, 0x100 }, { 0x1fd4, 0x100 },
	{ 0x1fd8, 0xa4000 }, { 0x1fe4, 0x100 }, { 0x1fe8, 0xa4000 },
	{ 0x1ff4, 0x100 }, { 0x1ff8, 0xa4000 }, { 0x2074, 0x100 },
	{ 0x2078, 0x4c000 }, { 0x2084, 0x100 }, { 0x2088, 0x4c000 },
	{ 0x2094, 0x100 }, { 0x2098, 0x4c000 }, { 0x2124, 0x100 },
	{ 0x2128, 0x27d800 }, { 0x212c, 0xf000 }, { 0x2134, 0x100 },
	{ 0x2138, 0x20000 }, { 0x2144, 0x100 }, { 0x2148, 0x13ec00 },
	{ 0x214c, 0x7800 }, { 0x2154, 0x100 }, { 0x2158, 0x8000 },
	{ 0x2164, 0x100 }, { 0x2168, 0x27d800 }, { 0x216c, 0xf000 },
	{ 0x2174, 0x100 }, { 0x2178, 0x20000 }, { 0x2184, 0x100 },
	{ 0x2188, 0x13ec00 }, { 0x218c, 0x7800 }, { 0x2194, 0x100 },
	{ 0x2198, 0x8000 }, { 0x2580, 0x1000000 }, { 0x2584, 0x101 },
	{ 0x2594, 0x1 }, { 0x2598, 0x8 }, { 0x259c, 0x2 },
	{ 0x25a4, 0x101 }, { 0x25d4, 0x1 }, { 0x26e8, 0x3000000 },
	{ 0x26f8, 0x10 }, { 0x26fc, 0xffffffff }, { 0x2700, 0x1 },
	{ 0x2704, 0x1 }, { 0x2738, 0x10 }, { 0x5aac, 0x1 },
	{ 0x7558, 0x100 }, { 0xb774, 0x1 }, { 0xe120, 0x4 },
	{ 0xe154, 0x4 }, { 0xe17c, 0x1010101 }, { 0xe1dc, 0x4 },
	{ 0xe1e4, 0x1010101 }, { 0xe204, 0xffffffff }, { 0xe208, 0xfffffffe },
	{ 0xe20c, 0xfffffffd }, { 0xe210, 0xfffffffc }, { 0xe244, 0x1 },
	{ 0xe248, 0x2 }, { 0xe24c, 0x3 }, { 0xe250, 0x4 },
	{ 0xe284, 0x4 }, { 0xe2b8, 0x1 }, { 0xe2e0, 0x1 },
	{ 0xe340, 0x1 }, { 0xe348, 0x1 }, { 0xe368, 0xffffffff },
	{ 0xe3a8, 0x1 }, { 0xe3e8, 0x1 }, { 0xe41c, 0x2 },
	{ 0xe444, 0x101 }, { 0xe4a4, 0x2 }, { 0xe4ac, 0x101 },
	{ 0xe4cc, 0xffffffff }, { 0xe4d0, 0xfffffffe }, { 0xe50c, 0x1 },
	{ 0xe510, 0x2 }, { 0xe54c, 0x2 }, { 0xe580, 0x3 },
	{ 0xe5a8, 0x10101 }, { 0xe608, 0x3 }, { 0xe610, 0x10101 },
	{ 0xe630, 0xffffffff }, { 0xe634, 0xfffffffe }, { 0xe638, 0xfffffffd },
	{ 0xe670, 0x1 }, { 0xe674, 0x2 }, { 0xe678, 0x3 },
	{ 0xe6b0, 0x3 }, { 0x13ef4, 0x1 }, { 0x13ef8, 0x1 },
	{ 0x13f00, 0x1 },
};

/* Captured Start words that held a surface address (u64) */
static const u32 t_start_addr[] = {
	0x48, 0x390, 0x3a0, 0x3b0, 0x3c0, 0x3d0, 0x3e0, 0x7d0,
	0x7e0, 0x7f0, 0x800, 0x810, 0x820, 0x830, 0x840, 0x850,
	0x860, 0xc10, 0xc20, 0xc30, 0xcb0, 0xcc0, 0xcd0, 0xd50,
	0xd60, 0xf50, 0xf60, 0x1130, 0x1140, 0x1150, 0x1350, 0x1390,
	0x13d0, 0x1410, 0x1420, 0x1430, 0x1440, 0x1490, 0x14a0, 0x14b0,
	0x14c0, 0x1e50, 0x1e60, 0x1e70, 0x1e80, 0x1e90, 0x1ea0, 0x1eb0,
	0x1ec0, 0x1ed0, 0x1ee0, 0x1fd0, 0x1fe0, 0x1ff0, 0x2070, 0x2080,
	0x2090, 0x2120, 0x2130, 0x2140, 0x2150, 0x2160, 0x2170, 0x2180,
	0x2190,
};

/* Address words that repeated another address in every session */
static const struct {
	u32 off, same_as;
} t_start_alias[] = {
	{ 0x7e0, 0x7d0 },
	{ 0x800, 0x7f0 },
	{ 0x820, 0x810 },
	{ 0x840, 0x830 },
	{ 0x860, 0x850 },
	{ 0x1e60, 0x1e50 },
	{ 0x1e80, 0x1e70 },
	{ 0x1ea0, 0x1e90 },
	{ 0x1ec0, 0x1eb0 },
	{ 0x1ee0, 0x1ed0 },
};

/* Every non-zero constant u32 word of the captured Process */
static const struct ave_mctf_word t_process_const[] = {
	{ 0x0, 0x8 }, { 0x18, 0x3 }, { 0x1c, 0x2 },
	{ 0x24, 0x50 }, { 0x28, 0x3c0 }, { 0x2c, 0x20 },
	{ 0x30, 0x2c40200 }, { 0xb4c, 0x5 }, { 0xb50, 0x5 },
	{ 0xd7c, 0x1 }, { 0x6158, 0x1 }, { 0x74cc, 0xffffffff },
	{ 0x74dc, 0x10000 }, { 0xa304, 0x100 }, { 0xa308, 0x1fe000 },
	{ 0xa30c, 0xf000 }, { 0xa314, 0x100 }, { 0xa318, 0x20000 },
	{ 0xa324, 0x780 }, { 0xa328, 0x438 }, { 0xa334, 0xf000 },
	{ 0xa344, 0x100 }, { 0xa348, 0xff000 }, { 0xa34c, 0x7800 },
	{ 0xa354, 0x100 }, { 0xa358, 0x8000 }, { 0xa364, 0x3c0 },
	{ 0xa368, 0x21c }, { 0xa374, 0x7800 }, { 0xa3b8, 0x780 },
	{ 0xa3bc, 0x438 }, { 0xac8c, 0x100 }, { 0xac94, 0x100 },
	{ 0xac98, 0x2f8000 }, { 0xad08, 0x5 }, { 0xad3c, 0x403e0000 },
	{ 0xad60, 0x1 }, { 0xad74, 0x3ff00000 }, { 0xad84, 0x100 },
	{ 0xad88, 0x1fe000 }, { 0xad8c, 0xf000 }, { 0xad94, 0x100 },
	{ 0xad98, 0x20000 }, { 0xada4, 0x780 }, { 0xada8, 0x438 },
	{ 0xadb4, 0xf000 }, { 0xadc4, 0x100 }, { 0xadc8, 0xff000 },
	{ 0xadcc, 0x7800 }, { 0xadd4, 0x100 }, { 0xadd8, 0x8000 },
	{ 0xade4, 0x3c0 }, { 0xade8, 0x21c }, { 0xadf4, 0x7800 },
};

/* Captured picture words, rewritten for linear NV12 (width/height included) */
static const u32 t_process_planes[] = {
	0xa308, 0xa30c, 0xa318, 0xa324, 0xa328, 0xa334, 0xa348, 0xa34c,
	0xa358, 0xa364, 0xa368, 0xa374, 0xad88, 0xad8c, 0xad98, 0xada4,
	0xada8, 0xadb4, 0xadc8, 0xadcc, 0xadd8, 0xade4, 0xade8, 0xadf4,
};

/* Captured per-frame address words (their high words are constants) */
static const u32 t_process_addr[] = {
	0xa300, 0xa310, 0xa340, 0xa350, 0xac88, 0xac90, 0xad80, 0xad90,
	0xadc0, 0xadd0,
};

struct t_mctf {
	struct ave_dev *ave;
	struct ave_ctx *ctx;
	struct ave_session *sess;
	const struct ave_mctf_layout *l;
};

static void t_mctf_init(struct kunit *test, struct t_mctf *t)
{
	t->ave = kunit_kzalloc(test, sizeof(*t->ave), GFP_KERNEL);
	t->ctx = kunit_kzalloc(test, sizeof(*t->ctx), GFP_KERNEL);
	t->sess = kunit_kzalloc(test, sizeof(*t->sess), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, t->ave);
	KUNIT_ASSERT_NOT_NULL(test, t->ctx);
	KUNIT_ASSERT_NOT_NULL(test, t->sess);
	t->l = ave_mctf_qualified(AVE_FW_TEXT_CRC_9003_78_0);
	KUNIT_ASSERT_NOT_NULL(test, t->l);
	KUNIT_ASSERT_LE(test, t->l->n_surfaces, (unsigned int)AVE_MCTF_BUFS);

	t->ave->layout = &ave_layout_9003_78_0;
	t->ave->text_crc = AVE_FW_TEXT_CRC_9003_78_0;
	t->ctx->ave = t->ave;
	t->ctx->sess = t->sess;
	t->ctx->mctf = true;
	t->sess->p = (struct ave_params){
		.codec = AVE_CODEC_HEVC, .width = 1920, .height = 1080,
		.stride = 1920, .slots = 2,
	};
	t->sess->mctf = t->l;
	t->sess->unit = t->l->unit;
	t->sess->arg1 = t->l->arg1;
	t->sess->frame_arg0 = t->l->frame_arg0;
	t->sess->client = T_CLIENT;
	t->sess->token = 400;	/* Open went out */
	/* Synthetic, 16 KiB aligned DART addresses, one per surface row */
	for (unsigned int i = 0; i < t->l->n_surfaces; i++)
		t->sess->mctf_iova[i] = 0x840000000ULL + (u64)i * 0x1000000;
}

static bool t_listed(const u32 *list, unsigned int n, u32 off)
{
	for (unsigned int i = 0; i < n; i++)
		if (list[i] == off)
			return true;
	return false;
}

static void t_build_start(struct kunit *test, struct t_mctf *t, u8 *cmd)
{
	KUNIT_ASSERT_EQ(test, ave_mctf_build_start(t->l, cmd, T_START_BYTES,
						   t->sess->mctf_iova), 0);
	ave_write_header(t->ctx, cmd, AVE_OP_START, AVE_OP_START,
			 AVE_SESSION_ARG0, ave_session_setup_arg2(t->ctx));
	ave_stamp_command(t->ave->layout->abi, cmd, 94400000, T_CREATED);
}

/* Start: byte for byte the capture, with this session's addresses. */
static void ave_mctf_start_image(struct kunit *test)
{
	u8 *cmd = kunit_kzalloc(test, T_START_BYTES, GFP_KERNEL);
	u8 *exp = kunit_kzalloc(test, T_START_BYTES, GFP_KERNEL);
	struct t_mctf t;

	KUNIT_ASSERT_NOT_NULL(test, cmd);
	KUNIT_ASSERT_NOT_NULL(test, exp);
	t_mctf_init(test, &t);
	KUNIT_EXPECT_EQ(test, t.l->start_bytes, (u32)T_START_BYTES);
	t_build_start(test, &t, cmd);

	for (unsigned int i = 0; i < ARRAY_SIZE(t_start_const); i++)
		put_unaligned_le32(t_start_const[i].val, exp + t_start_const[i].off);
	/* Session words: client, creation time, header surface size */
	put_unaligned_le64(T_CLIENT, exp + 0x10);
	put_unaligned_le64(T_CREATED, exp + 0x38);
	put_unaligned_le32(0x12c000, exp + 0x50);

	for (unsigned int i = 0; i < ARRAY_SIZE(t_start_addr); i++) {
		u32 off = t_start_addr[i];
		u64 addr = get_unaligned_le64(cmd + off);

		KUNIT_EXPECT_TRUE_MSG(test, addr >= 0x840000000ULL &&
				      IS_ALIGNED(addr, SZ_16K),
				      "Start +%#x: %#llx", off, addr);
		put_unaligned_le64(addr, exp + off);
	}
	/* Distinct surfaces stay distinct; the aliases repeat their surface. */
	for (unsigned int i = 0; i < ARRAY_SIZE(t_start_addr); i++)
		for (unsigned int j = i + 1; j < ARRAY_SIZE(t_start_addr); j++) {
			u32 a = t_start_addr[i], b = t_start_addr[j];
			bool same = false;

			for (unsigned int k = 0; k < ARRAY_SIZE(t_start_alias); k++)
				same |= t_start_alias[k].off == b &&
					t_start_alias[k].same_as == a;
			KUNIT_EXPECT_EQ_MSG(test, get_unaligned_le64(cmd + a) ==
					    get_unaligned_le64(cmd + b), same,
					    "Start +%#x vs +%#x", a, b);
		}
	KUNIT_EXPECT_MEMEQ(test, cmd, exp, T_START_BYTES);

	/* Every surface is mapped at least as large as its size word. */
	for (unsigned int i = 0; i < t.l->n_surfaces; i++) {
		const struct ave_mctf_surface *s = &t.l->surfaces[i];

		if (s->alloc && s->off != 0x48)
			KUNIT_EXPECT_GE_MSG(test, s->alloc,
					    get_unaligned_le32(cmd + s->off + 8),
					    "surface +%#x", s->off);
	}
	KUNIT_EXPECT_GE(test, t.l->surfaces[0].alloc, 0x12c000U);
}

static void t_picture(u8 *exp, u32 off, u64 addr, u32 bytes, u32 w, u32 h)
{
	put_unaligned_le64(addr, exp + off);
	put_unaligned_le32(bytes, exp + off + 0x08);
	put_unaligned_le32(1920, exp + off + 0x0c);
	put_unaligned_le32(w, exp + off + 0x24);
	put_unaligned_le32(h, exp + off + 0x28);
	put_unaligned_le32(1920, exp + off + 0x34);
}

/* Process over frames 0..6, as macOS sent them, byte for byte. */
static void ave_mctf_process_frames(struct kunit *test)
{
	static const u8 levels[] = { 16, 16, 16, 12, 12, 12, 9 };
	u8 *start = kunit_kzalloc(test, T_START_BYTES, GFP_KERNEL);
	u8 *cmd = kunit_kzalloc(test, T_PROCESS_BYTES, GFP_KERNEL);
	u8 *exp = kunit_kzalloc(test, T_PROCESS_BYTES, GFP_KERNEL);
	const u32 luma = 1920 * 1080;
	struct t_mctf t;

	KUNIT_ASSERT_NOT_NULL(test, start);
	KUNIT_ASSERT_NOT_NULL(test, cmd);
	KUNIT_ASSERT_NOT_NULL(test, exp);
	t_mctf_init(test, &t);
	KUNIT_EXPECT_EQ(test, t.l->process_bytes, (u32)T_PROCESS_BYTES);
	t_build_start(test, &t, start);

	for (u32 n = 0; n < ARRAY_SIZE(levels); n++) {
		/* A five-picture input ring, as the camera hands them over */
		u64 in = 0x900000000ULL + (n % 5) * 0x400000;
		u64 out = 0x980000000ULL + (n % 3) * 0x400000;
		u32 slot = n == 1;	/* macOS: the second frame goes in slot 1 */
		struct ave_mctf_frame f = {
			.in = in, .out = out, .stride = 1920, .width = 1920,
			.height = 1080, .slot = slot, .frame = n,
			.level = levels[n],
		};

		KUNIT_ASSERT_EQ(test, ave_mctf_build_process(t.l, cmd, T_PROCESS_BYTES,
							     t.sess->mctf_iova, &f), 0);
		ave_write_header(t.ctx, cmd, AVE_OP_PROCESS, t.l->subtype + slot,
				 t.sess->frame_arg0, ave_session_arg2(t.ctx));
		ave_stamp_command(t.ave->layout->abi, cmd, 46400000, T_CREATED + n);

		memset(exp, 0, T_PROCESS_BYTES);
		for (unsigned int i = 0; i < ARRAY_SIZE(t_process_const); i++) {
			u32 off = t_process_const[i].off;

			if (t_listed(t_process_planes, ARRAY_SIZE(t_process_planes), off) ||
			    t_listed(t_process_addr, ARRAY_SIZE(t_process_addr), off - 4))
				continue;
			put_unaligned_le32(t_process_const[i].val, exp + off);
		}
		/* Header: the nominal frame clock (+100 per frame), slot subtype */
		put_unaligned_le64(600 + 100 * n, exp + 0x8);
		put_unaligned_le64(T_CLIENT, exp + 0x10);
		put_unaligned_le32(0x15 + slot, exp + 0x20);
		put_unaligned_le64(T_CREATED + n, exp + 0x38);
		/* First-frame word, slot, its two Start surfaces, index, level */
		put_unaligned_le32(n ? 4 : 5, exp + 0x5d90);
		put_unaligned_le32(slot, exp + 0xac84);
		put_unaligned_le64(get_unaligned_le64(start + 0xd50 + 0x10 * slot),
				   exp + 0xac88);
		put_unaligned_le64(get_unaligned_le64(start + 0xf50 + 0x10 * slot),
				   exp + 0xac90);
		put_unaligned_le32(n, exp + 0xacf8);
		put_unaligned_le32(levels[n], exp + 0xad68);
		/*
		 * Linear NV12 in the captured picture entries: no header
		 * planes, linear bytes and row bytes, the captured sizes.
		 */
		t_picture(exp, 0xa300, in, luma, 1920, 1080);
		t_picture(exp, 0xa340, in + luma, luma / 2, 960, 540);
		t_picture(exp, 0xad80, out, luma, 1920, 1080);
		t_picture(exp, 0xadc0, out + luma, luma / 2, 960, 540);
		KUNIT_EXPECT_MEMEQ_MSG(test, cmd, exp, T_PROCESS_BYTES, "frame %u", n);
	}
	/* The two slots use different surfaces. */
	KUNIT_EXPECT_NE(test, get_unaligned_le64(start + 0xd50),
			get_unaligned_le64(start + 0xd60));
	KUNIT_EXPECT_NE(test, get_unaligned_le64(start + 0xf50),
			get_unaligned_le64(start + 0xf60));
}

/* Other builds and bad requests are refused. */
static void ave_mctf_refusal(struct kunit *test)
{
	const struct ave_mctf_layout *m4 =
		ave_mctf_layout_for_crc(AVE_FW_TEXT_CRC_T6040_25G83);
	u8 *cmd = kunit_kzalloc(test, T_START_BYTES, GFP_KERNEL);
	struct ave_mctf_frame f = {
		.in = 0x900000000ULL, .out = 0x980000000ULL, .stride = 1920,
		.width = 1920, .height = 1080, .level = 16,
	};
	struct ave_mctf_frame bad;
	struct t_mctf t;

	KUNIT_ASSERT_NOT_NULL(test, cmd);
	t_mctf_init(test, &t);
	KUNIT_EXPECT_NULL(test, ave_mctf_layout_for_crc(0x2ce7359e));
	KUNIT_EXPECT_NULL(test, ave_mctf_qualified(0x2ce7359e));
	KUNIT_EXPECT_NULL(test, ave_mctf_qualified(0));
	KUNIT_ASSERT_NOT_NULL(test, m4);
	KUNIT_EXPECT_FALSE(test, m4->qualified);
	KUNIT_EXPECT_NULL(test, ave_mctf_qualified(AVE_FW_TEXT_CRC_T6040_25G83));
	KUNIT_EXPECT_EQ(test, ave_mctf_build_start(m4, cmd, T_START_BYTES,
						   t.sess->mctf_iova), -EOPNOTSUPP);
	KUNIT_EXPECT_EQ(test, ave_mctf_build_process(m4, cmd, T_PROCESS_BYTES,
						     t.sess->mctf_iova, &f),
			-EOPNOTSUPP);
	KUNIT_EXPECT_EQ(test, ave_mctf_build_start(NULL, cmd, T_START_BYTES,
						   t.sess->mctf_iova), -EOPNOTSUPP);

	KUNIT_EXPECT_EQ(test, ave_mctf_build_start(t.l, cmd, T_START_BYTES - 4,
						   t.sess->mctf_iova), -EINVAL);
	KUNIT_EXPECT_EQ(test, ave_mctf_build_process(t.l, cmd, T_PROCESS_BYTES,
						     t.sess->mctf_iova, &f), 0);
	KUNIT_EXPECT_EQ(test, ave_mctf_build_process(t.l, cmd, T_PROCESS_BYTES - 4,
						     t.sess->mctf_iova, &f), -EINVAL);
	bad = f;
	bad.level = AVE_MCTF_LEVEL_MAX + 1;
	KUNIT_EXPECT_EQ(test, ave_mctf_build_process(t.l, cmd, T_PROCESS_BYTES,
						     t.sess->mctf_iova, &bad), -EINVAL);
	bad = f;
	bad.width = 1280;
	bad.height = 720;
	KUNIT_EXPECT_EQ(test, ave_mctf_build_process(t.l, cmd, T_PROCESS_BYTES,
						     t.sess->mctf_iova, &bad), -EINVAL);
	bad = f;
	bad.slot = AVE_MCTF_SLOTS;
	KUNIT_EXPECT_EQ(test, ave_mctf_build_process(t.l, cmd, T_PROCESS_BYTES,
						     t.sess->mctf_iova, &bad), -EINVAL);
	bad = f;
	bad.stride = 1928;	/* not a multiple of 64 */
	KUNIT_EXPECT_EQ(test, ave_mctf_build_process(t.l, cmd, T_PROCESS_BYTES,
						     t.sess->mctf_iova, &bad), -EINVAL);
	/* A missing surface address is refused, not sent as zero. */
	t.sess->mctf_iova[0] = 0;
	KUNIT_EXPECT_EQ(test, ave_mctf_build_start(t.l, cmd, T_START_BYTES,
						   t.sess->mctf_iova), -EINVAL);
}

static struct kunit_case ave_mctf_cases[] = {
	KUNIT_CASE(ave_mctf_start_image),
	KUNIT_CASE(ave_mctf_process_frames),
	KUNIT_CASE(ave_mctf_refusal),
	{}
};

static struct kunit_suite ave_mctf_suite = {
	.name = "apple-ave-mctf",
	.test_cases = ave_mctf_cases,
};
kunit_test_suite(ave_mctf_suite);

MODULE_DESCRIPTION("Apple AVE MCTF command image tests");
MODULE_LICENSE("GPL");
