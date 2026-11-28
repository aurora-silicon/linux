// SPDX-License-Identifier: GPL-2.0-only
/*
 * KUnit tests for the checks the 6070 Start and Process builders make on
 * session parameters and buffers before they write a command: counts that
 * index the session arrays, every surface the firmware is told about, and
 * the codec state copied into Process.
 *
 * Copyright The Aurora Silicon Contributors
 */
#include <kunit/test.h>
#include <linux/module.h>
#include <linux/sizes.h>
#include <linux/string.h>
#include <linux/unaligned.h>

#include "ave.h"
#include "ave-layout.h"

#define T_FILL 0xa5

struct t_wr {
	struct ave_dev *ave;
	struct ave_ctx *ctx;
	struct ave_session *sess;
	struct ave_fw_layout *fw;
	u8 *start, *process;
	size_t start_len, process_len;
};

/* Synthetic, 16 KiB aligned DMA addresses, one window per buffer. */
static void t_buf(struct ave_buf *b, unsigned int *index)
{
	b->dma = 0x820000000ULL + (u64)(*index) * 0x2000000;
	b->size = SZ_16K * (1 + (*index)++);
}

static void t_wr_init(struct kunit *test, struct t_wr *t, enum ave_codec codec)
{
	struct ave_session *sess;
	struct ave_params *p;
	unsigned int index = 0;

	t->ave = kunit_kzalloc(test, sizeof(*t->ave), GFP_KERNEL);
	t->ctx = kunit_kzalloc(test, sizeof(*t->ctx), GFP_KERNEL);
	t->sess = kunit_kzalloc(test, sizeof(*t->sess), GFP_KERNEL);
	t->fw = kunit_kmalloc(test, sizeof(*t->fw), GFP_KERNEL);
	KUNIT_ASSERT_TRUE(test, t->ave && t->ctx && t->sess && t->fw);
	*t->fw = ave_layout_6070_11_1;
	t->fw->encode_ready = true;
	t->ave->layout = t->fw;
	t->ctx->ave = t->ave;
	t->ctx->sess = t->sess;
	sess = t->sess;
	p = &sess->p;

	*p = (struct ave_params){
		.codec = codec,
		.width = 1920,
		.height = 1080,
		.cw = 1920,
		.ch = ALIGN(1080, 16),
		.stride = ALIGN(1920, 64),
		.fps_num = 30,
		.fps_den = 1,
		.fps_int = 30,
		.bframes = true,
		.slots = 5,
		.refs = 5,
		.gop = 30,
		.rc_mode = 3,
		.bitrate = 4000000,
		.initial_qp = 33,
		.min_qp = 0,
		.max_qp = 48,
		.profile = codec == AVE_CODEC_H264 ? 4 : 1,
		.level = codec == AVE_CODEC_H264 ? 13 : 7,
		.bit_depth = 8,
	};
	p->mbs = p->cw / 16 * (p->ch / 16);
	p->coded_cap = ALIGN(3 * p->cw * p->ch, SZ_16K);
	sess->token = 400;
	sess->client = 10;
	t_buf(&sess->hdr, &index);
	t_buf(&sess->client_mem, &index);
	sess->client_mem.size = AVE_6070_CLIENT_MEM_BYTES;
	t_buf(&sess->param_sets, &index);
	for (unsigned int i = 0; i < AVE_MAX_REFS; i++) {
		t_buf(&sess->ref_a[i], &index);
		t_buf(&sess->ref_b[i], &index);
		t_buf(&sess->ref_c[i], &index);
		t_buf(&sess->ref_d[i], &index);
	}
	t_buf(&sess->mb, &index);
	for (unsigned int set = 0; set < AVE_NEIGHBOUR_SETS; set++) {
		t_buf(&sess->row_a[set], &index);
		t_buf(&sess->row_b[set], &index);
		t_buf(&sess->fixed[set], &index);
		t_buf(&sess->neighbour_fw[set], &index);
	}
	for (unsigned int set = 0; set < AVE_ENTROPY_SETS; set++)
		for (unsigned int i = 0; i < AVE_TILE_BUFS; i++) {
			t_buf(&sess->tile[set][i], &index);
			t_buf(&sess->tile_aux[set][i], &index);
		}
	for (unsigned int i = 0; i < 2; i++) {
		t_buf(&sess->hevc_extra[i], &index);
		t_buf(&sess->hevc_transcoded[i], &index);
	}
	sess->hevc_transcoded[1].size = sess->hevc_transcoded[0].size;
	for (unsigned int i = 0; i < AVE_MAX_SLOTS; i++) {
		t_buf(&sess->slot[i].coded, &index);
		t_buf(&sess->slot[i].meta, &index);
		t_buf(&sess->slot[i].aux, &index);
	}
	sess->codec_state_bytes = t->fw->codec_state_bytes[codec - 1];
	sess->codec_state = kunit_kzalloc(test, sess->codec_state_bytes, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, sess->codec_state);
	KUNIT_ASSERT_EQ(test, ave_init_codec_state(sess), 0);

	t->start_len = t->fw->bytes[codec - 1][AVE_IMAGE_START];
	t->process_len = t->fw->bytes[codec - 1][AVE_IMAGE_PROCESS];
	t->start = kunit_kmalloc(test, t->start_len + 1, GFP_KERNEL);
	t->process = kunit_kmalloc(test, t->process_len + 1, GFP_KERNEL);
	KUNIT_ASSERT_TRUE(test, t->start && t->process);
}

/* Build Start into a filled buffer; a refusal must leave it untouched. */
static int t_start(struct kunit *test, struct t_wr *t)
{
	int ret;

	memset(t->start, T_FILL, t->start_len + 1);
	ret = ave_build_start(t->ctx, t->start, t->start_len);
	if (ret)
		KUNIT_EXPECT_NULL(test, memchr_inv(t->start, T_FILL, t->start_len));
	KUNIT_EXPECT_EQ(test, t->start[t->start_len], T_FILL);
	return ret;
}

static int t_process(struct kunit *test, struct t_wr *t, unsigned int slot,
		     dma_addr_t input)
{
	memset(t->process, T_FILL, t->process_len + 1);
	return ave_build_process(t->ctx, t->process, t->process_len, slot, input,
				 t->sess->p.bitrate, false, false);
}

#define T_INPUT 0x8f0000000ULL

/* Counts that index the session's arrays are bounded before any is read. */
static void ave_writer_test_start_counts(struct kunit *test)
{
	struct ave_params *p;
	struct t_wr t;

	t_wr_init(test, &t, AVE_CODEC_H264);
	p = &t.sess->p;
	KUNIT_ASSERT_EQ(test, t_start(test, &t), 0);

	p->refs = 0;
	KUNIT_EXPECT_EQ(test, t_start(test, &t), -EINVAL);
	p->refs = AVE_MAX_REFS + 1;
	KUNIT_EXPECT_EQ(test, t_start(test, &t), -EINVAL);
	p->refs = U32_MAX;
	KUNIT_EXPECT_EQ(test, t_start(test, &t), -EINVAL);
	p->refs = AVE_MAX_REFS;
	p->slots = 0;
	KUNIT_EXPECT_EQ(test, t_start(test, &t), -EINVAL);
	p->slots = t.fw->max_slots + 1;
	KUNIT_EXPECT_EQ(test, t_start(test, &t), -EINVAL);
	p->slots = U32_MAX;
	KUNIT_EXPECT_EQ(test, t_start(test, &t), -EINVAL);
	p->slots = t.fw->max_slots;
	KUNIT_EXPECT_EQ(test, t_start(test, &t), 0);

	/* A command buffer of any other size. */
	memset(t.start, T_FILL, t.start_len + 1);
	KUNIT_EXPECT_EQ(test, ave_build_start(t.ctx, t.start, t.start_len - 4), -EINVAL);
	KUNIT_EXPECT_EQ(test, ave_build_start(t.ctx, t.start, t.start_len + 1), -EINVAL);
	KUNIT_EXPECT_NULL(test, memchr_inv(t.start, T_FILL, t.start_len + 1));
	/* A layout that cannot encode builds nothing. */
	t.fw->encode_ready = false;
	KUNIT_EXPECT_EQ(test, t_start(test, &t), -EOPNOTSUPP);
}

/*
 * Each buffer the firmware gets must be mapped and 16 KiB aligned. Flip
 * the alignment of one at a time: the last one of each array counts too.
 */
static void ave_writer_test_start_buffers(struct kunit *test)
{
	struct ave_session *s;
	struct t_wr t;

	t_wr_init(test, &t, AVE_CODEC_H264);
	s = t.sess;
#define T_BAD(field)                                                    \
	do {                                                            \
		dma_addr_t keep = (field).dma;                          \
		(field).dma = keep + SZ_4K;                             \
		KUNIT_EXPECT_EQ_MSG(test, t_start(test, &t), -EINVAL,   \
				    "misaligned " #field);               \
		(field).dma = 0;                                        \
		KUNIT_EXPECT_EQ_MSG(test, t_start(test, &t), -EINVAL,   \
				    "unmapped " #field);                 \
		(field).dma = keep;                                     \
	} while (0)
	T_BAD(s->hdr);
	T_BAD(s->client_mem);
	T_BAD(s->param_sets);
	T_BAD(s->ref_a[0]);
	T_BAD(s->ref_a[AVE_MAX_REFS - 1]);
	T_BAD(s->ref_b[AVE_MAX_REFS - 1]);
	T_BAD(s->ref_c[AVE_MAX_REFS - 1]);
	T_BAD(s->ref_d[AVE_6070_LOWRES_RESULTS - 1]);
	T_BAD(s->slot[0].coded);
	T_BAD(s->slot[4].coded);
	T_BAD(s->slot[4].meta);
	T_BAD(s->row_a[AVE_NEIGHBOUR_SETS - 1]);
	T_BAD(s->row_b[AVE_NEIGHBOUR_SETS - 1]);
	T_BAD(s->fixed[AVE_NEIGHBOUR_SETS - 1]);
	T_BAD(s->neighbour_fw[AVE_NEIGHBOUR_SETS - 1]);
	T_BAD(s->tile[AVE_ENTROPY_SETS - 1][AVE_6070_AVC_ENTROPY_TILES - 1]);
	T_BAD(s->tile_aux[AVE_ENTROPY_SETS - 1][AVE_6070_AVC_ENTROPY_TILES - 1]);
#undef T_BAD
	KUNIT_EXPECT_EQ(test, t_start(test, &t), 0);

	/* The client memory has the size the firmware expects, no other. */
	s->client_mem.size = AVE_6070_CLIENT_MEM_BYTES - SZ_16K;
	KUNIT_EXPECT_EQ(test, t_start(test, &t), -EINVAL);
	s->client_mem.size = AVE_6070_CLIENT_MEM_BYTES;
	s->param_sets.size = 0;
	KUNIT_EXPECT_EQ(test, t_start(test, &t), -EINVAL);
	s->param_sets.size = SZ_16K;

	/* Buffers this session does not use are not looked at. */
	s->slot[5].coded.dma = 0;
	s->tile[0][AVE_6070_AVC_ENTROPY_TILES].dma = 1;
	s->hevc_extra[1].dma = 1;
	s->hevc_transcoded[0].dma = 0;
	KUNIT_EXPECT_EQ(test, t_start(test, &t), 0);
}

/* HEVC uses four entropy tiles and the transcode buffers too. */
static void ave_writer_test_start_buffers_hevc(struct kunit *test)
{
	struct ave_session *s;
	struct t_wr t;

	t_wr_init(test, &t, AVE_CODEC_HEVC);
	s = t.sess;
	KUNIT_ASSERT_EQ(test, t_start(test, &t), 0);
	s->tile[AVE_ENTROPY_SETS - 1][3].dma += SZ_4K;
	KUNIT_EXPECT_EQ(test, t_start(test, &t), -EINVAL);
	s->tile[AVE_ENTROPY_SETS - 1][3].dma -= SZ_4K;
	s->tile_aux[0][3].dma = 0;
	KUNIT_EXPECT_EQ(test, t_start(test, &t), -EINVAL);
	s->tile_aux[0][3].dma = 0x8e0000000ULL;
	s->hevc_extra[1].dma += SZ_4K;
	KUNIT_EXPECT_EQ(test, t_start(test, &t), -EINVAL);
	s->hevc_extra[1].dma -= SZ_4K;
	s->hevc_transcoded[1].dma = 0;
	KUNIT_EXPECT_EQ(test, t_start(test, &t), -EINVAL);
	s->hevc_transcoded[1].dma = 0x8e0004000ULL;
	/* The fifth tile is not used. */
	s->tile[0][4].dma = 1;
	KUNIT_EXPECT_EQ(test, t_start(test, &t), 0);
}

/* general_level_idc is 30 times the level (H.265 A.4); other levels are 0. */
static void ave_writer_test_hevc_level_idc(struct kunit *test)
{
	static const u32 idc[] = { 0, 30, 60, 63, 90, 93, 120, 123, 150, 153, 0 };
	const struct ave_start_layout *l;
	struct t_wr t;

	t_wr_init(test, &t, AVE_CODEC_HEVC);
	l = &t.fw->start[AVE_CODEC_HEVC - 1];
	KUNIT_ASSERT_NE(test, l->hevc_level_a, 0U);
	KUNIT_ASSERT_NE(test, l->hevc_level_b, 0U);
	KUNIT_ASSERT_LE(test, l->hevc_level_a + 4, (u32)t.start_len);
	KUNIT_ASSERT_LE(test, l->hevc_level_b + 4, (u32)t.start_len);
	for (unsigned int level = 0; level < ARRAY_SIZE(idc); level++) {
		t.sess->p.level = level;
		KUNIT_ASSERT_EQ(test, t_start(test, &t), 0);
		KUNIT_EXPECT_EQ_MSG(test, get_unaligned_le32(t.start + l->hevc_level_a),
				    idc[level], "level %u", level);
		KUNIT_EXPECT_EQ_MSG(test, get_unaligned_le32(t.start + l->hevc_level_b),
				    idc[level], "level %u", level);
	}
	t.sess->p.level = U32_MAX;
	KUNIT_ASSERT_EQ(test, t_start(test, &t), 0);
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(t.start + l->hevc_level_a), 0U);
}

/* H.264 level_idc (Table A-1) for every level the controls can select. */
static void ave_writer_test_h264_level_idc(struct kunit *test)
{
	static const u32 idc[] = {
		10, 9, 11, 12, 13, 20, 21, 22, 30, 31, 32, 40, 41, 42, 50, 51, 52,
	};
	const struct ave_start_layout *l;
	struct t_wr t;

	t_wr_init(test, &t, AVE_CODEC_H264);
	l = &t.fw->start[AVE_CODEC_H264 - 1];
	KUNIT_ASSERT_NE(test, l->h264_level, 0U);
	for (unsigned int i = 0; i < ARRAY_SIZE(idc); i++) {
		t.sess->p.level = i + 1;
		KUNIT_ASSERT_EQ(test, t_start(test, &t), 0);
		KUNIT_EXPECT_EQ_MSG(test, get_unaligned_le32(t.start + l->h264_level),
				    idc[i], "level %u", i + 1);
	}
}

/* Process: the slot, the input and the slot's buffers are checked first. */
static void ave_writer_test_process_refusal(struct kunit *test)
{
	struct ave_session *s;
	struct ave_params *p;
	struct t_wr t;

	t_wr_init(test, &t, AVE_CODEC_H264);
	s = t.sess;
	p = &s->p;
	KUNIT_ASSERT_EQ(test, t_process(test, &t, 0, T_INPUT), 0);
	KUNIT_EXPECT_EQ(test, t.process[t.process_len], T_FILL);
	KUNIT_ASSERT_EQ(test, t_process(test, &t, p->slots - 1, T_INPUT), 0);

#define T_REFUSED(expr)                                                         \
	do {                                                                    \
		KUNIT_EXPECT_EQ_MSG(test, (expr), -EINVAL, #expr);              \
		KUNIT_EXPECT_NULL_MSG(test,                                     \
				      memchr_inv(t.process, T_FILL, t.process_len + 1), \
				      #expr " wrote the command");              \
	} while (0)
	T_REFUSED(t_process(test, &t, p->slots, T_INPUT));
	T_REFUSED(t_process(test, &t, AVE_MAX_SLOTS, T_INPUT));
	T_REFUSED(t_process(test, &t, U32_MAX, T_INPUT));
	T_REFUSED(t_process(test, &t, 0, T_INPUT + SZ_4K));
	T_REFUSED(t_process(test, &t, 0, T_INPUT + 1));
	p->slots = t.fw->max_slots + 1;
	T_REFUSED(t_process(test, &t, 0, T_INPUT));
	p->slots = 5;
	s->slot[2].coded.dma += SZ_4K;
	T_REFUSED(t_process(test, &t, 2, T_INPUT));
	s->slot[2].coded.dma -= SZ_4K;
	s->slot[2].meta.dma = 0;
	T_REFUSED(t_process(test, &t, 2, T_INPUT));
	s->slot[2].meta.dma = 0x8e0000000ULL;
	s->slot[2].aux.dma += 4;
	T_REFUSED(t_process(test, &t, 2, T_INPUT));
	s->slot[2].aux.dma -= 4;
	/* No codec state to copy into the command. */
	s->codec_state = NULL;
	T_REFUSED(t_process(test, &t, 2, T_INPUT));
#undef T_REFUSED
	memset(t.process, T_FILL, t.process_len + 1);
	KUNIT_EXPECT_EQ(test,
			ave_build_process(t.ctx, t.process, t.process_len - 4, 0,
					  T_INPUT, p->bitrate, false, false),
			-EINVAL);
	KUNIT_EXPECT_NULL(test, memchr_inv(t.process, T_FILL, t.process_len + 1));
}

/* The codec state copied into Process may not be read past its end. */
static void ave_writer_test_process_state_bounds(struct kunit *test)
{
	const enum ave_codec codecs[] = { AVE_CODEC_H264, AVE_CODEC_HEVC };

	for (unsigned int c = 0; c < ARRAY_SIZE(codecs); c++) {
		struct ave_session *s;
		struct t_wr t;
		size_t full;

		t_wr_init(test, &t, codecs[c]);
		s = t.sess;
		full = s->codec_state_bytes;
		KUNIT_ASSERT_EQ(test, t_process(test, &t, 0, T_INPUT), 0);
		/* The same state in a buffer one byte short. */
		s->codec_state_bytes = full - 1;
		s->codec_state = kunit_kzalloc(test, full - 1, GFP_KERNEL);
		KUNIT_ASSERT_NOT_NULL(test, s->codec_state);
		KUNIT_EXPECT_EQ(test, t_process(test, &t, 0, T_INPUT), -EINVAL);
		s->codec_state_bytes = 1;
		KUNIT_EXPECT_EQ(test, t_process(test, &t, 0, T_INPUT), -EINVAL);
		s->codec_state_bytes = 0;
		KUNIT_EXPECT_EQ(test, t_process(test, &t, 0, T_INPUT), -EINVAL);
	}
}

/* The codec state is only set up at the size this firmware uses. */
static void ave_writer_test_codec_state(struct kunit *test)
{
	struct ave_session *s;
	struct t_wr t;
	u8 *state;

	t_wr_init(test, &t, AVE_CODEC_H264);
	s = t.sess;
	state = s->codec_state;
	s->codec_state = NULL;
	KUNIT_EXPECT_EQ(test, ave_init_codec_state(s), -EINVAL);
	s->codec_state = state;
	s->codec_state_bytes = t.fw->codec_state_bytes[0] - 4;
	KUNIT_EXPECT_EQ(test, ave_init_codec_state(s), -EINVAL);
	/* An H.264 state where HEVC is set up, and the other way round. */
	s->codec_state_bytes = t.fw->codec_state_bytes[0];
	s->p.codec = AVE_CODEC_HEVC;
	KUNIT_EXPECT_EQ(test, ave_init_codec_state(s), -EINVAL);
	s->p.codec = 0;
	KUNIT_EXPECT_EQ(test, ave_init_codec_state(s), -EINVAL);
	s->p.codec = AVE_CODEC_H264;
	KUNIT_EXPECT_EQ(test, ave_init_codec_state(s), 0);
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(state), (u32)s->codec_state_bytes);
}

static struct kunit_case ave_writer_test_cases[] = {
	KUNIT_CASE(ave_writer_test_start_counts),
	KUNIT_CASE(ave_writer_test_start_buffers),
	KUNIT_CASE(ave_writer_test_start_buffers_hevc),
	KUNIT_CASE(ave_writer_test_hevc_level_idc),
	KUNIT_CASE(ave_writer_test_h264_level_idc),
	KUNIT_CASE(ave_writer_test_process_refusal),
	KUNIT_CASE(ave_writer_test_process_state_bounds),
	KUNIT_CASE(ave_writer_test_codec_state),
	{}
};

static struct kunit_suite ave_writer_test_suite = {
	.name = "apple-ave-writer",
	.test_cases = ave_writer_test_cases,
};

kunit_test_suite(ave_writer_test_suite);

MODULE_DESCRIPTION("KUnit tests for Apple AVE command builder input checks");
MODULE_LICENSE("GPL");
