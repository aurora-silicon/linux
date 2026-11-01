// SPDX-License-Identifier: GPL-2.0-only
/* Byte equality against the pre-ABI-refactor 9003 command builders. */
#include <kunit/test.h>
#include <linux/module.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/unaligned.h>

#include "ave.h"
#include "ave-regs.h"
#include "tests/ave-9003-reference.h"
#include "tests/ave-6070-reference.h"

static void ave_test_buf(struct ave_buf *b, unsigned int *index)
{
	b->dma = 0x820000000ULL + (u64)(*index) * 0x2000000;
	b->size = SZ_16K * (1 + (*index)++);
}

static void ave_test_session(struct ave_session *sess, enum ave_codec codec,
			     u32 width, u32 height, bool bframes)
{
	struct ave_params *p = &sess->p;
	unsigned int index = 0;

	*p = (struct ave_params){
		.codec = codec,
		.width = width,
		.height = height,
		.cw = ALIGN(width, 16),
		.ch = ALIGN(height, 16),
		.stride = ALIGN(width, 64),
		.fps_num = 30,
		.fps_den = 1,
		.fps_int = 30,
		.bframes = bframes,
		.reorder_off = !bframes,
		.slots = bframes ? 5 : 3,
		.refs = bframes ? 5 : 2,
		.gop = 120,
		.rc_mode = 4,
		.bitrate = 8000000,
		.limit_bytes = 2000000,
		.min_qp = -1,
		.max_qp = 51,
		.profile = codec == AVE_CODEC_H264 ? 6 : 1,
		.level = codec == AVE_CODEC_H264 ? 13 : 7,
		.high = codec == AVE_CODEC_H264,
	};
	p->mbs = p->cw / 16 * (p->ch / 16);
	p->coded_cap = ALIGN(3 * p->cw * p->ch / 2, SZ_16K);
	sess->token = 400;
	sess->client = 10;
	ave_test_buf(&sess->hdr, &index);
	for (unsigned int i = 0; i < AVE_MAX_REFS; i++) {
		ave_test_buf(&sess->ref_a[i], &index);
		ave_test_buf(&sess->ref_b[i], &index);
		ave_test_buf(&sess->ref_c[i], &index);
		ave_test_buf(&sess->ref_d[i], &index);
	}
	ave_test_buf(&sess->mb, &index);
	ave_test_buf(&sess->row_a, &index);
	ave_test_buf(&sess->row_b, &index);
	ave_test_buf(&sess->fixed, &index);
	for (unsigned int i = 0; i < AVE_TILE_BUFS; i++) {
		ave_test_buf(&sess->tile[i], &index);
		ave_test_buf(&sess->tile_aux[i], &index);
	}
	for (unsigned int i = 0; i < 2; i++)
		ave_test_buf(&sess->hevc_extra[i], &index);
	for (unsigned int i = 0; i < p->slots; i++) {
		ave_test_buf(&sess->slot[i].coded, &index);
		ave_test_buf(&sess->slot[i].meta, &index);
		ave_test_buf(&sess->slot[i].aux, &index);
	}
}

/* These are synthetic DART addresses, never addresses from a device capture. */
static void ave_mask_dart(u8 *cmd, size_t len)
{
	for (size_t off = 0; off + 8 <= len; off += 8) {
		u64 val = get_unaligned_le64(cmd + off);

		if (val >= 0x800000000ULL && val < 0x1000000000ULL)
			put_unaligned_le64(0, cmd + off);
	}
}

static void ave_9003_byte_equality(struct kunit *test)
{
	struct ave_dev *ave = kunit_kzalloc(test, sizeof(*ave), GFP_KERNEL);
	struct ave_ctx *ctx = kunit_kzalloc(test, sizeof(*ctx), GFP_KERNEL);
	struct ave_session *sess =
		kunit_kzalloc(test, sizeof(*sess), GFP_KERNEL);
	u8 *actual = kunit_kmalloc(test, 0x14000, GFP_KERNEL);
	u8 *expected = kunit_kmalloc(test, 0x14000, GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, ave);
	KUNIT_ASSERT_NOT_NULL(test, ctx);
	KUNIT_ASSERT_NOT_NULL(test, sess);
	KUNIT_ASSERT_NOT_NULL(test, actual);
	KUNIT_ASSERT_NOT_NULL(test, expected);
	ave->layout = &ave_layout_9003_78_0;
	ctx->ave = ave;
	ctx->sess = sess;
	for (unsigned int codec = AVE_CODEC_H264; codec <= AVE_CODEC_HEVC;
	     codec++) {
		for (unsigned int setting = 0; setting < 3; setting++) {
			size_t len =
				ave->layout->bytes[codec - 1][AVE_IMAGE_START];

			ave_test_session(sess, codec,
					 setting == 2 ? 3840 : 1920,
					 setting == 2 ? 2160 : 1080,
					 setting != 1);
			memset(actual, 0, len);
			memset(expected, 0, len);
			KUNIT_ASSERT_EQ(test, ave_build_start(ctx, actual, len),
					0);
			sess->token = 400;
			ave_reference_start(ctx, expected, len);
			ave_stamp_command(ave->layout->abi, actual, 94400000,
					  0x12345678);
			put_unaligned_le64(94400000, expected + 0x30);
			put_unaligned_le64(0x12345678, expected + 0x38);
			KUNIT_EXPECT_MEMEQ(test, actual, expected, len);
			ave_mask_dart(actual, len);
			ave_mask_dart(expected, len);
			KUNIT_EXPECT_MEMEQ(test, actual, expected, len);
			for (unsigned int frame = 0; frame < 2; frame++) {
				unsigned int slot = frame ? sess->p.slots - 1 :
							    0;

				len = ave->layout->bytes[codec - 1]
							[AVE_IMAGE_PROCESS];
				sess->frames = frame ? 7 : 0;
				sess->token = 500;
				memset(actual, 0, len);
				memset(expected, 0, len);
				KUNIT_ASSERT_EQ(
					test,
					ave_build_process(ctx, actual, len,
							  slot, 0x8f0000000ULL,
							  9000000, !!frame),
					0);
				sess->token = 500;
				ave_reference_process(ctx, expected, len, slot,
						      0x8f0000000ULL, 9000000,
						      !!frame);
				ave_stamp_command(ave->layout->abi, actual,
						  94400000, 0x12345678);
				put_unaligned_le64(94400000, expected + 0x30);
				put_unaligned_le64(0x12345678, expected + 0x38);
				KUNIT_EXPECT_MEMEQ(test, actual, expected, len);
				ave_mask_dart(actual, len);
				ave_mask_dart(expected, len);
				KUNIT_EXPECT_MEMEQ(test, actual, expected, len);
			}
		}
	}
}

static void ave_6070_payload_fields(struct kunit *test)
{
	struct ave_dev *ave = kunit_kzalloc(test, sizeof(*ave), GFP_KERNEL);
	struct ave_ctx *ctx = kunit_kzalloc(test, sizeof(*ctx), GFP_KERNEL);
	struct ave_session *sess =
		kunit_kzalloc(test, sizeof(*sess), GFP_KERNEL);
	struct ave_fw_layout *fw = kunit_kmalloc(test, sizeof(*fw), GFP_KERNEL);
	u8 *actual = kunit_kmalloc(test, 0x32dc8 + 32, GFP_KERNEL);
	u8 *expected = kunit_kmalloc(test, 0x32dc8, GFP_KERNEL);
	u8 *state = kunit_kmalloc(test, 0x6838, GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, ave);
	KUNIT_ASSERT_NOT_NULL(test, ctx);
	KUNIT_ASSERT_NOT_NULL(test, sess);
	KUNIT_ASSERT_NOT_NULL(test, fw);
	KUNIT_ASSERT_NOT_NULL(test, actual);
	KUNIT_ASSERT_NOT_NULL(test, expected);
	KUNIT_ASSERT_NOT_NULL(test, state);
	*fw = ave_layout_6070_11_1;
	/* Test a private layout; the selectable CRCs remain refused. */
	fw->encode_ready = true;
	ave->layout = fw;
	ctx->ave = ave;
	ctx->sess = sess;
	for (unsigned int i = 0; i < 0x6838; i++)
		state[i] = i ^ 0xa5;
	for (unsigned int codec = AVE_CODEC_H264; codec <= AVE_CODEC_HEVC;
	     codec++) {
		for (unsigned int b = 0; b < 2; b++) {
			size_t len = fw->bytes[codec - 1][AVE_IMAGE_START];
			unsigned int index = 100;

			memset(sess, 0, sizeof(*sess));
			ave_test_session(sess, codec, 1920, 1080, b);
			sess->p.gop = 30;
			sess->p.bitrate = 4000000;
			sess->p.limit_bytes = 1000000;
			sess->p.min_qp = 0;
			sess->p.profile = codec == AVE_CODEC_H264 ? 4 : 1;
			sess->p.high = false;
			sess->p.level = codec == AVE_CODEC_H264 ? 12 : 6;
			for (unsigned int set = 0; set < 3; set++) {
				ave_test_buf(&sess->row_a_set[set], &index);
				ave_test_buf(&sess->row_b_set[set], &index);
				ave_test_buf(&sess->fixed_set[set], &index);
				for (unsigned int k = 0; k < 8; k++) {
					ave_test_buf(&sess->tile_set[set][k],
						     &index);
					ave_test_buf(&sess->tile_aux_set[set][k],
						     &index);
				}
			}
			memset(actual, 0xa5, len + 32);
			ave_6070_reference_start(sess, expected, len);
			KUNIT_ASSERT_EQ(test, ave_build_start(ctx, actual, len),
					0);
			KUNIT_EXPECT_MEMEQ(test, actual, expected, len);
			for (unsigned int i = 0; i < 32; i++)
				KUNIT_EXPECT_EQ(test, actual[len + i],
						(u8)0xa5);
			/* Check a rational frame rate independently of integer fps. */
			sess->p.fps_num = 30000;
			sess->p.fps_den = 1001;
			ave_6070_reference_start(sess, expected, len);
			KUNIT_ASSERT_EQ(test, ave_build_start(ctx, actual, len),
					0);
			KUNIT_EXPECT_MEMEQ(test, actual, expected, len);
			sess->p.fps_num = 30;
			sess->p.fps_den = 1;
			len = fw->bytes[codec - 1][AVE_IMAGE_PROCESS];
			/* Missing retained state must fail before writing the command. */
			memset(actual, 0xa5, len);
			memset(expected, 0xa5, len);
			KUNIT_EXPECT_EQ(test,
					ave_build_process(ctx, actual, len, 0,
							  0x10012340000ULL,
							  6000000, false),
					-EOPNOTSUPP);
			KUNIT_EXPECT_MEMEQ(test, actual, expected, len);
			sess->process_state = state;
			sess->process_state_bytes = len;
			for (unsigned int frame = 0; frame < 2; frame++)
				for (unsigned int slot = 0;
				     slot < sess->p.slots; slot++) {
					int ret;

					sess->frames = frame ? 7 : 0;
					memset(actual, 0xa5, len + 32);
					ave_6070_reference_process(sess,
								   expected,
								   len, slot,
								   frame);
					ret = ave_build_process(ctx, actual, len, slot,
								0x10012340000ULL, 6000000, frame);
					KUNIT_ASSERT_EQ(test, ret, 0);
					KUNIT_EXPECT_MEMEQ(test, actual,
							   expected, len);
					for (unsigned int i = 0; i < 32; i++)
						KUNIT_EXPECT_EQ(test,
								actual[len + i],
								(u8)0xa5);
				}
		}
	}
}

static void ave_6070_images(struct kunit *test)
{
	const struct ave_fw_abi *abi = &ave_abi_6070;
	struct ave_dev *ave = kunit_kzalloc(test, sizeof(*ave), GFP_KERNEL);
	struct ave_ctx *ctx = kunit_kzalloc(test, sizeof(*ctx), GFP_KERNEL);
	struct ave_session *sess =
		kunit_kzalloc(test, sizeof(*sess), GFP_KERNEL);
	u8 actual[0x70] = {}, expected[0x70] = {};

	KUNIT_ASSERT_NOT_NULL(test, ave);
	KUNIT_ASSERT_NOT_NULL(test, ctx);
	KUNIT_ASSERT_NOT_NULL(test, sess);
	ave->layout = &ave_layout_6070_11_1;
	ave->extra0_dma = 0x876540000ULL;
	ctx->ave = ave;
	ctx->sess = sess;
	abi->write_config(ave, actual);
	ave_stamp_command(abi, actual, 3000000, 0x123456);
	put_unaligned_le16(1, expected);
	put_unaligned_le32(U32_MAX, expected + 0x1c);
	put_unaligned_le64(3000000, expected + 0x28);
	put_unaligned_le64(0x123456, expected + 0x30);
	expected[0x41] = 1;
	put_unaligned_le64(ave->extra0_dma, expected + 0x48);
	KUNIT_EXPECT_MEMEQ(test, actual, expected, sizeof(actual));
	for (unsigned int codec = AVE_CODEC_H264; codec <= AVE_CODEC_HEVC;
	     codec++) {
		static const enum ave_opcode ops[] = { AVE_OP_OPEN,
						       AVE_OP_CLOSE,
						       AVE_OP_HALT };
		static const u16 wire[] = { 2, 12, 14 };
		static const u32 semantic[] = { 3, 4, U32_MAX };

		for (unsigned int i = 0; i < ARRAY_SIZE(ops); i++) {
			memset(actual, 0, sizeof(actual));
			memset(expected, 0, sizeof(expected));
			sess->p.codec = codec;
			sess->client = 10;
			sess->token = 300;
			if (ops[i] == AVE_OP_HALT)
				ave_write_global(ave, actual, ops[i]);
			else
				ave_write_header(ctx, actual, ops[i], ops[i],
						 0xc8, 0x1e0);
			ave_stamp_command(abi, actual, 3000000, 0x123456);
			put_unaligned_le16(wire[i], expected);
			if (ops[i] != AVE_OP_HALT) {
				put_unaligned_le64(400, expected + 0x08);
				put_unaligned_le32(10, expected + 0x10);
				put_unaligned_le32(codec - 1, expected + 0x18);
				put_unaligned_le32(0xc8, expected + 0x20);
			}
			put_unaligned_le32(semantic[i], expected + 0x1c);
			put_unaligned_le64(3000000, expected + 0x28);
			put_unaligned_le64(0x123456, expected + 0x30);
			KUNIT_EXPECT_MEMEQ(test, actual, expected,
					   abi->scalar_bytes[ops[i]]);
		}
	}
	KUNIT_EXPECT_EQ(test, abi->wire_id[0][AVE_OP_PRIORITY], 0U);
	KUNIT_EXPECT_EQ(test, abi->scalar_bytes[AVE_OP_RESET], 0U);
	KUNIT_EXPECT_EQ(test, abi->reply_id[AVE_OP_HALT], 0U);
}

static void ave_6070_replies(struct kunit *test)
{
	const struct ave_fw_abi *abi = &ave_abi_6070;
	struct ave_reply reply;
	u8 msg[0x48] = {};

	for (unsigned int op = 1; op < AVE_OP_COUNT; op++) {
		u32 len = op == AVE_OP_PROCESS ? 0x48 : 0x40;

		if (!abi->reply_id[op])
			continue;
		put_unaligned_le16(abi->reply_id[op], msg);
		put_unaligned_le32(10, msg + 0x10);
		put_unaligned_le32(abi->semantic[op], msg + 0x1c);
		put_unaligned_le32(0x00ee0000, msg + 0x38);
		put_unaligned_le32(0x12345678, msg + 0x40);
		KUNIT_ASSERT_EQ(test, abi->parse_reply(abi, msg, len, &reply),
				0);
		KUNIT_EXPECT_EQ(test, reply.op, (enum ave_opcode)op);
		KUNIT_EXPECT_EQ(test, reply.client, 10ULL);
		KUNIT_EXPECT_EQ(test, reply.subtype, abi->semantic[op]);
		KUNIT_EXPECT_EQ(test, reply.status, 0U);
		KUNIT_EXPECT_EQ(test, reply.done_id, abi->reply_id[op]);
		KUNIT_EXPECT_EQ(test, reply.raw_status, 0x00ee0000U);
		KUNIT_EXPECT_EQ(test, reply.process_extra,
				op == AVE_OP_PROCESS ? 0x12345678U : 0U);
		KUNIT_EXPECT_EQ(test,
				abi->parse_reply(abi, msg, len - 1, &reply),
				-EPROTO);
		KUNIT_EXPECT_EQ(test,
				abi->parse_reply(abi, msg, len + 1, &reply),
				-EPROTO);
		put_unaligned_le32(0, msg + 0x38);
		KUNIT_ASSERT_EQ(test, abi->parse_reply(abi, msg, len, &reply),
				0);
		KUNIT_EXPECT_NE(test, reply.status, 0U);
	}
	for (unsigned int id = 0xe07; id <= 0xe08; id++) {
		put_unaligned_le16(id, msg);
		KUNIT_EXPECT_EQ(test, abi->parse_reply(abi, msg, 0x40, &reply),
				-EOPNOTSUPP);
	}
	put_unaligned_le16(2, msg); /* Request IDs are not completion IDs. */
	KUNIT_EXPECT_EQ(test, abi->parse_reply(abi, msg, 0x40, &reply),
			-EPROTO);
}

static void ave_6070_reply_routing(struct kunit *test)
{
	struct ave_dev *ave = kunit_kzalloc(test, sizeof(*ave), GFP_KERNEL);
	struct ave_ctx *ctx = kunit_kzalloc(test, sizeof(*ctx), GFP_KERNEL);
	struct ave_session *sess =
		kunit_kzalloc(test, sizeof(*sess), GFP_KERNEL);
	struct ave_reply reply;
	u8 msg[0x48] = {};

	KUNIT_ASSERT_NOT_NULL(test, ave);
	KUNIT_ASSERT_NOT_NULL(test, ctx);
	KUNIT_ASSERT_NOT_NULL(test, sess);
	ave->layout = &ave_layout_6070_11_1;
	ctx->ave = ave;
	ctx->sess = sess;
	ave->sessions[0] = ctx;
	sess->p.slots = 5;
	sess->client = 10;
	sess->busy = 1;
	sess->slot[4].busy = true;
	put_unaligned_le16(0xe06, msg);
	put_unaligned_le32(10, msg + 0x10);
	put_unaligned_le32(0x19, msg + 0x1c);
	put_unaligned_le32(0x00ee0000, msg + 0x38);
	KUNIT_ASSERT_EQ(test,
			ave_abi_6070.parse_reply(&ave_abi_6070, msg,
						 sizeof(msg), &reply),
			0);
	ave_sess_reply(ave, &reply);
	KUNIT_EXPECT_EQ(test, sess->busy, 0U);
	KUNIT_EXPECT_EQ(test, sess->done_count, 1U);
	KUNIT_EXPECT_EQ(test, sess->done_fifo[0], (u8)4);
	KUNIT_EXPECT_EQ(test, sess->slot[4].status, 0U);
	KUNIT_EXPECT_TRUE(test, sess->slot[4].done);
	KUNIT_EXPECT_EQ(test, atomic_read(&ctx->ready), 1);
}

static void ave_6070_split_surfaces(struct kunit *test)
{
	const struct ave_surface_layout bank = {
		.address = 0x4b8,
		.size = 0x558,
		.address_stride = 8,
		.size_stride = 4,
	};
	u8 *cmd = kunit_kzalloc(test, 0x600, GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, cmd);
	for (unsigned int i = 0; i < 20; i++)
		ave_write_surface(&ave_layout_6070_11_1, cmd, &bank, i,
				  0x800000000ULL + i * SZ_16K,
				  (i + 1) * SZ_16K);
	for (unsigned int i = 0; i < 20; i++) {
		KUNIT_EXPECT_EQ(test, get_unaligned_le64(cmd + 0x4b8 + i * 8),
				0x800000000ULL + i * SZ_16K);
		KUNIT_EXPECT_EQ(test, get_unaligned_le32(cmd + 0x558 + i * 4),
				(i + 1) * SZ_16K);
	}
}

static void ave_6070_gate(struct kunit *test)
{
	struct ave_dev *ave = kunit_kzalloc(test, sizeof(*ave), GFP_KERNEL);
	struct ave_ctx *ctx = kunit_kzalloc(test, sizeof(*ctx), GFP_KERNEL);
	struct ave_session *sess =
		kunit_kzalloc(test, sizeof(*sess), GFP_KERNEL);
	u8 cmd[0x70], before[0x70];

	KUNIT_ASSERT_NOT_NULL(test, ave);
	KUNIT_ASSERT_NOT_NULL(test, ctx);
	KUNIT_ASSERT_NOT_NULL(test, sess);
	ave->layout = &ave_layout_6070_11_1;
	ctx->ave = ave;
	ctx->sess = sess;
	sess->p.codec = AVE_CODEC_H264;
	memset(cmd, 0xa5, sizeof(cmd));
	memcpy(before, cmd, sizeof(cmd));
	KUNIT_EXPECT_PTR_EQ(test, ave_layout_for_crc(0x460c7a7c),
			    &ave_layout_6070_11_1);
	KUNIT_EXPECT_PTR_EQ(test, ave_layout_for_crc(0x25bfd039),
			    &ave_layout_6070_11_1);
	KUNIT_EXPECT_PTR_EQ(test, ave_layout_for_crc(0), NULL);
	KUNIT_EXPECT_EQ(test, ave_build_start(ctx, cmd, sizeof(cmd)),
			-EOPNOTSUPP);
	KUNIT_EXPECT_EQ(
		test, ave_build_process(ctx, cmd, sizeof(cmd), 0, 0, 0, false),
		-EOPNOTSUPP);
	KUNIT_EXPECT_MEMEQ(test, cmd, before, sizeof(cmd));
	KUNIT_EXPECT_EQ(test, sess->token, 0ULL);
}

static void ave_6070_channels(struct kunit *test)
{
	const struct ave_fw_transport *t = ave_abi_6070.transport;
	struct ave_dev *ave = kunit_kzalloc(test, sizeof(*ave), GFP_KERNEL);
	u32 hello[4] = { 7, 0x9bc0, 0x100, 0 };
	u8 *desc;

	KUNIT_ASSERT_NOT_NULL(test, ave);
	ave->layout = &ave_layout_6070_11_1;
	ave->ipc.cpu = kunit_kzalloc(test, SZ_64K, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, ave->ipc.cpu);
	ave->ipc.size = t->ipc_bytes;
	ave->ipc_fw_base = 0x100000000ULL;
	ave->channel_bytes = hello[1];
	KUNIT_EXPECT_EQ(test, ave_ipc_check_hello(t, hello), 0);
	for (unsigned int word = 0; word < 4; word++) {
		hello[word]++;
		KUNIT_EXPECT_EQ(test, ave_ipc_check_hello(t, hello), -EPROTO);
		hello[word]--;
	}
	for (unsigned int i = 0; i < 7; i++) {
		int id = i == 1 ? 0 : 1;
		bool io = i == 1 || i == 5;
		u64 address = ave->ipc_fw_base + AVE_IPC_CHANNEL_OFF +
			      (io ? t->offsets[id] : 0x1000 + i * 0x400);

		desc = ave->ipc.cpu + AVE_IPC_CHANNEL_OFF + i * 0x100;
		strscpy(desc, io ? (id ? "IO_T2H" : "IO") : "UNUSED", 0x40);
		put_unaligned_le32(io ? id : U32_MAX, desc + 0x40);
		put_unaligned_le32(io ? t->doorbells[id] : U32_MAX,
				   desc + 0x44);
		put_unaligned_le32(io ? t->slots[id] : 4, desc + 0x48);
		put_unaligned_le64(address, desc + 0x4c);
	}
	KUNIT_ASSERT_EQ(test, ave_ipc_parse_channels(ave), 0);
	KUNIT_EXPECT_EQ(test, ave->ring[0].slots, 32U);
	KUNIT_EXPECT_EQ(test, ave->ring[1].slots, 8U);
	KUNIT_EXPECT_EQ(test, ave->ring[0].doorbell, 1U);
	KUNIT_EXPECT_EQ(test, ave->ring[1].doorbell, 3U);
	desc = ave->ipc.cpu + AVE_IPC_CHANNEL_OFF + 0x100;
	put_unaligned_le32(33, desc + 0x48);
	KUNIT_EXPECT_EQ(test, ave_ipc_parse_channels(ave), -EPROTO);
	put_unaligned_le32(32, desc + 0x48);
	desc = ave->ipc.cpu + AVE_IPC_CHANNEL_OFF;
	put_unaligned_le64(ave->ipc_fw_base + ave->ipc.size, desc + 0x4c);
	KUNIT_EXPECT_EQ(test, ave_ipc_parse_channels(ave), -EPROTO);
}

static void ave_6070_variants(struct kunit *test)
{
	const struct ave_fw_transport *t = ave_abi_6070.transport;

	KUNIT_EXPECT_EQ(test, ave_t6000.boot_device_id, 11U);
	KUNIT_EXPECT_EQ(test, ave_t6000.device_type, 9U);
	KUNIT_EXPECT_EQ(test, ave_t6000.boot_arg_value, 0x400U);
	KUNIT_EXPECT_EQ(test, ave_t6000.timebase_multiplier, 1000000U);
	KUNIT_EXPECT_EQ(test, ave_t6000.text_crc, 0x460c7a7cU);
	KUNIT_EXPECT_EQ(test, ave_t8112.boot_device_id, 15U);
	KUNIT_EXPECT_EQ(test, ave_t8112.device_type, 13U);
	KUNIT_EXPECT_EQ(test, ave_t8112.boot_arg_value, 0x10000U);
	KUNIT_EXPECT_EQ(test, ave_t8112.timebase_multiplier, 1U);
	KUNIT_EXPECT_EQ(test, ave_t8112.text_crc, 0x25bfd039U);
	KUNIT_EXPECT_EQ(test, ave_fw_vcbar(&ave_t6000, 0x10000b28000ULL),
			0x0102000000b28000ULL);
	KUNIT_EXPECT_EQ(test, ave_fw_vcbar(&ave_t8112, 0x800000000ULL),
			0x0102000800000000ULL);
	KUNIT_EXPECT_EQ(test, ave_t6000.regs->ctrl, 0x400044U);
	KUNIT_EXPECT_EQ(test, ave_t8112.regs->timebase, 0x178000U);
	KUNIT_EXPECT_GE(test, t->command_bytes, 0x32dc8U);
	KUNIT_EXPECT_LE(test, t->command_offset + t->command_bytes,
			t->scalar_offset);
	KUNIT_EXPECT_LE(test,
			t->scalar_offset +
				AVE_IPC_SCALAR_BUFS * AVE_IPC_SCALAR_BYTES,
			t->process_offset);
	KUNIT_EXPECT_LE(
		test, t->process_offset + AVE_IPC_PROC_BUFS * t->process_bytes,
		t->ipc_bytes);
}

static struct kunit_case ave_abi_cases[] = {
	KUNIT_CASE(ave_9003_byte_equality),
	KUNIT_CASE(ave_6070_images),
	KUNIT_CASE(ave_6070_payload_fields),
	KUNIT_CASE(ave_6070_replies),
	KUNIT_CASE(ave_6070_reply_routing),
	KUNIT_CASE(ave_6070_split_surfaces),
	KUNIT_CASE(ave_6070_gate),
	KUNIT_CASE(ave_6070_channels),
	KUNIT_CASE(ave_6070_variants),
	{}
};

static struct kunit_suite ave_abi_suite = {
	.name = "apple-ave-abi",
	.test_cases = ave_abi_cases,
};
kunit_test_suite(ave_abi_suite);

MODULE_LICENSE("GPL");
