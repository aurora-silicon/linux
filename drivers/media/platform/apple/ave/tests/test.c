// SPDX-License-Identifier: GPL-2.0-only
static const struct ave_coproc_regs test_regs;
static const struct ave_variant test_variant = {
	.lfs_ref_scale = 2,
#ifndef TEST_BASELINE
	.boot_qualified = true,
	.regs = &test_regs,
#endif
};

static void params(struct ave_params *p, unsigned int codec, u32 w, u32 h)
{
	memset(p, 0, sizeof(*p));
	p->codec = codec;
	p->width = w;
	p->height = h;
	p->cw = ALIGN(w, 16);
	p->ch = ALIGN(h, 16);
	p->stride = ALIGN(w, 64);
	p->mbs = p->cw / 16 * (p->ch / 16);
	p->fps_num = 30;
	p->fps_int = 30;
	p->fps_den = 1;
	p->bframes = true;
	p->refs = 5;
	p->slots = 5;
	p->gop = 30;
	p->rc_mode = 4;
	p->bitrate = 4000000;
	p->limit_bytes = 1000000;
	p->max_qp = 51;
	p->profile = codec == 1 ? 4 : 1;
	p->level = codec == 1 ? 12 : 6;
	p->coded_cap =
		max(ALIGN(3 * (codec == 2 ? ALIGN(w, 32) : p->cw) * p->ch / 2,
			  SZ_16K),
		    0x3c000U);
}

static void start(struct ave_ctx *ctx)
{
	assert(ave_sess_alloc(ctx->ave, ctx->sess) == 0);
	assert(ave_cmd_open(ctx) == 0);
	assert(ave_cmd_start(ctx) == 0);
}

static void frame(struct ave_ctx *ctx, unsigned int slot, bool force)
{
	struct vb2_v4l2_buffer src;

	assert(ave_cmd_process(ctx, slot, &src, 0x10012340000ULL,
			       ctx->sess->p.bitrate, force) == 0);
}

#ifndef TEST_BASELINE
static void facts(void)
{
	struct ave_dev dev = { .variant = &test_variant,
			       .layout = &ave_layout_26a428 };
	struct ave_session session = {};
	struct ave_ctx ctx = { .ave = &dev, .sess = &session };
	u8 reply[0x50] = {};

	assert(!ave_layout_for_crc(0));
	assert(!ave_layout_for_crc(0xffffffff));
	assert(!ave_layout_for_crc(0x2ce7359e));
	assert(ave_layout_for_crc(0x69c54454) == &ave_layout_9003_78_0);
	assert(ave_fw_check_build(&dev) == -EOPNOTSUPP);
	dev.text_crc = 0x69c54454;
	assert(ave_fw_check_build(&dev) == -EOPNOTSUPP);
	dev.layout = ave_layout_for_crc(dev.text_crc);
	assert(ave_fw_check_build(&dev) == 0);
	struct ave_variant probe_only = test_variant;

	probe_only.boot_qualified = false;
	probe_only.regs = NULL;
	dev.variant = &probe_only;
	assert(ave_fw_check_build(&dev) == -EOPNOTSUPP);
	dev.variant = &test_variant;
	dev.layout = &ave_layout_26a428;

	for (unsigned int codec = 1; codec <= 2; codec++) {
		params(&session.p, codec, 1920, 1080);
		start(&ctx);
		assert(wire_len == (codec == 1 ? 0x3b40U : 0x13ee0U));
		assert(get_unaligned_le32(wire + 0x50) == 0xec000);
		assert(get_unaligned_le32(wire + 0x94) == 5);
		assert(get_unaligned_le32(wire + 0x918) == 5);
		assert(get_unaligned_le32(wire + 0xb00) == 5);
		assert(get_unaligned_le32(wire + 0xb10) == 0x10000);
		assert(get_unaligned_le32(wire + 0xdbc) == 5);
		assert(get_unaligned_le32(wire + 0xfe4) == 5);
		assert(get_unaligned_le32(wire + 0xc0) == 1920 * 1120 / 2);
		assert(get_unaligned_le32(wire + 0xd0) == 2048 * 2048 / 128);
		assert(get_unaligned_le32(wire + 0x322c) == 1920);
		assert(get_unaligned_le32(wire + 0x3230) == 1088);
		assert(get_unaligned_le32(wire + 0x3618) == 4000000);
		assert(get_unaligned_le64(wire + 0x3638) ==
		       0x415e848000000000ULL); /* 8e6 */
		if (codec == 1) {
			assert(get_unaligned_le32(wire + 0x37d0) ==
			       (120U << 16 | 4));
			assert(wire[0x37d4] == 68 && wire[0x37d6] == 1);
			assert(get_unaligned_le32(wire + 0x3a98) == 77);
			assert(get_unaligned_le32(wire + 0x3aa0) == 40);
			assert(get_unaligned_le32(wire + 0x3ac0) ==
			       (67U << 16 | 119));
			assert(wire[0x3ac7] == 1);
			assert(get_unaligned_le32(wire + 0x3acc) >> 16 == 4);
		} else {
			assert(get_unaligned_le32(wire + 0x68) == 2);
			assert(get_unaligned_le32(wire + 0x66b8) == 1920);
			assert(get_unaligned_le32(wire + 0x66bc) == 1088);
			assert(get_unaligned_le32(wire + 0x6708) == 2);
		}
		session.frames = 0;
		frame(&ctx, 0, false);
		assert(wire_len == (codec == 1 ? 0x6df8U : 0x78b8U));
		assert(get_unaligned_le32(wire + 0x20) == 0x13);
		assert(get_unaligned_le64(wire + 0x30) == 46400000);
		assert(get_unaligned_le32(wire +
					  (codec == 1 ? 0xdc0 : 0x1568)) == 1);
		assert(get_unaligned_le32(wire +
					  (codec == 1 ? 0x58b8 : 0x6060)) ==
		       0x10000);
		assert(get_unaligned_le32(wire +
					  (codec == 1 ? 0x58c8 : 0x6070)) ==
		       0x4000);
		if (codec == 1)
			assert(get_unaligned_le32(wire + 0x58e0) == 0x360000);
		frame(&ctx, 4, false);
		assert(get_unaligned_le32(wire + 0x20) == 0x17);
		assert(get_unaligned_le32(wire +
					  (codec == 1 ? 0xdc0 : 0x1568)) == 0);
		assert(get_unaligned_le64(wire +
					  (codec == 1 ? 0x5760 : 0x5f08)) ==
		       480000);
		unsigned int before = submissions;

		assert(ave_cmd_process(&ctx, session.p.slots, NULL, 0,
				       session.p.bitrate, false) == -EINVAL);

		assert(ave_cmd_process(&ctx, 0, NULL, 0, session.p.bitrate,
				       true) == -EOPNOTSUPP);
		assert(ave_cmd_process(&ctx, 0, NULL, 0, 12000000, false) ==
		       -EOPNOTSUPP);
		assert(submissions == before);
		dev.sessions[0] = &ctx;
		put_unaligned_le64(session.client, reply + 0x10);
		put_unaligned_le32(0x17, reply + 0x20);
		struct ave_reply decoded;

		assert(dev.layout->abi->parse_reply(dev.layout->abi, reply,
						    sizeof(reply),
						    &decoded) == 0);
		ave_sess_reply(&dev, &decoded);
		assert(!session.slot[4].busy && session.slot[4].done);
		assert(ctx.ready == (int)codec);
		dev.sessions[0] = NULL;
		ave_sess_free_bufs(&dev, &session);
		memset(&session, 0, sizeof(session));
	}
	for (unsigned int codec = 1; codec <= 2; codec++) {
		params(&session.p, codec, 1920, 1080);
		session.p.rc_mode = 3;
		session.p.slots = 7;
		session.p.initial_qp = 28;
		session.p.max_qp = 48;
		session.p.bitrate = codec == 1 ? 14100000 : 7050000;
		session.p.limit_bytes = 0;
		session.p.coded_cap = 0x458000;
		start(&ctx);
		assert(get_unaligned_le32(wire + 0x918) == 7);
		assert(get_unaligned_le32(wire + 0xb00) == 7);
		assert(get_unaligned_le32(wire + 0x1868) == 3);
		assert(get_unaligned_le32(wire + 0x1888) == 0xa4000);
		assert(get_unaligned_le32(wire + 0x1898) == 0xa4000);
		assert(get_unaligned_le32(wire + 0x988) == 0x458000);
		assert(get_unaligned_le32(wire + 0x3620) == 28);
		assert(get_unaligned_le32(wire + 0x3734) == 0);
		assert(get_unaligned_le32(wire + 0x36d0) ==
		       (codec == 1 ? 0x10600U : 0x10700U));
		if (codec == 1) {
			assert(wire[0x3830] == 0);
		} else {
			assert(get_unaligned_le32(wire + 0x3708) == 1);
			assert(get_unaligned_le32(wire + 0x37c8) == 1);
			assert(get_unaligned_le32(wire + 0x9e38) == 0x100);
		}
		frame(&ctx, 6, false);
		assert(get_unaligned_le32(wire + 0x20) == 0x19);
		if (codec == 1) {
			assert(get_unaligned_le64(wire + 0x58d8) == 0);
			assert(get_unaligned_le32(wire + 0x58e0) == 0);
		}
		ave_sess_free_bufs(&dev, &session);
		memset(&session, 0, sizeof(session));
	}
	params(&session.p, 2, 1920, 1080);
	session.p.bit_depth = 10;
	session.p.profile = 2;
	session.p.min_qp = -12;
	session.p.stride *= 2;
	session.p.coded_cap = session.p.coded_cap * 5 / 4;
	start(&ctx);
	assert(get_unaligned_le32(wire + 0xa0) == 0x290400);
	assert(get_unaligned_le32(wire + 0xc0) == 0x148200);
	assert(get_unaligned_le32(wire + 0x1bc0) == 0x47b800);
	assert(get_unaligned_le32(wire + 0x35e8) == 0x10040);
	assert(get_unaligned_le32(wire + 0x3a60) == 0x01000101);
	assert(get_unaligned_le32(wire + 0x6530) == 0x01000000);
	assert(get_unaligned_le32(wire + 0x66dc) == 2);
	ave_sess_free_bufs(&dev, &session);
	memset(&session, 0, sizeof(session));

	params(&session.p, 1, 1366, 768);
	session.p.profile = 6;
	session.p.high = true;
	start(&ctx);
	assert(get_unaligned_le32(wire + 0x3a98) == 100);
	assert(wire[0x3b0d] == 1 && wire[0x3823] == 1 && wire[0x3820] == 1);
	assert(get_unaligned_le32(wire + 0x3ac8) >> 16 == 5);
	ave_sess_free_bufs(&dev, &session);
	params(&session.p, 1, 1920, 1080);
	session.p.bframes = false;
	session.p.profile = 2;
	session.p.refs = 2;
	session.p.slots = 3;
	start(&ctx);
	frame(&ctx, 2, false);
	assert(get_unaligned_le32(wire + 0x20) == 0x15);
	assert(get_unaligned_le32(wire + 0x2c) == 0x140);
	ave_sess_free_bufs(&dev, &session);

	assert(alloc_count == free_count);
	puts("ok: CRC/boot gates, T6040/CQ/Main10, bounds, replies, refusal, allocations");
}
#endif

#if defined(TEST_PER_ABI) && !defined(TEST_BASELINE)
static void facts_6070(void)
{
	struct ave_variant variant = test_variant;
	struct ave_fw_layout fw = ave_layout_6070_11_1;
	struct ave_dev dev = { .layout = &fw, .variant = &variant };
	struct ave_session session = {};
	struct ave_ctx ctx = { .ave = &dev, .sess = &session };
	u8 *actual = malloc(0x32dc8 + 32), *expected = malloc(0x32dc8);
	u8 msg[0x48] = {};
	struct ave_reply reply;

	assert(actual && expected);
	variant.lfs_ref_scale = 1;
	for (unsigned int codec = 1; codec <= 2; codec++) {
		dev.text_crc = codec == 1 ? 0x460c7a7c : 0x25bfd039;
		dev.layout = ave_layout_for_crc(dev.text_crc);
		assert(ave_fw_check_build(&dev) == -EOPNOTSUPP);
		dev.layout = &fw;
		fw.encode_ready =
			true; /* private copy for software tests only */
		for (unsigned int b = 0; b < 2; b++) {
			size_t len = fw.bytes[codec - 1][AVE_IMAGE_START];

			params(&session.p, codec, 1920, 1080);
			session.p.bframes = b;
			session.p.refs = b ? 5 : 2;
			session.p.slots = b ? 5 : 3;
			session.token = 400;
			session.client = 10;
			assert(ave_sess_alloc(&dev, &session) == 0);
			assert(!session.mb.cpu && !session.mb.size);
			for (unsigned int rate = 0; rate < 2; rate++) {
				session.p.fps_num = rate ? 30000 : 30;
				session.p.fps_den = rate ? 1001 : 1;
				memset(actual, 0xa5, len + 32);
				ave_6070_reference_start(&session, expected,
							 len);
				assert(ave_build_start(&ctx, actual, len) == 0);
				assert(!memcmp(actual, expected, len));
				for (unsigned int i = 0; i < 32; i++)
					assert(actual[len + i] == 0xa5);
			}
			session.p.fps_num = 30;
			session.p.fps_den = 1;
			len = fw.bytes[codec - 1][AVE_IMAGE_PROCESS];
			assert(ave_build_process(&ctx, actual, len, 0,
						 0x10012340000ULL, 6000000,
						 false) == -EOPNOTSUPP);
			session.process_state = malloc(len);
			assert(session.process_state);
			session.process_state_bytes = len;
			for (unsigned int i = 0; i < len; i++)
				session.process_state[i] = i ^ 0xa5;
			for (unsigned int frame = 0; frame < 2; frame++)
				for (unsigned int slot = 0;
				     slot < session.p.slots; slot++) {
					int ret;

					session.frames = frame ? 7 : 0;
					ave_6070_reference_process(&session,
								   expected,
								   len, slot,
								   frame);
					memset(actual, 0xa5, len + 32);
					ret = ave_build_process(&ctx, actual, len, slot,
								0x10012340000ULL, 6000000, frame);
					assert(ret == 0);
					assert(!memcmp(actual, expected, len));
					for (unsigned int i = 0; i < 32; i++)
						assert(actual[len + i] == 0xa5);
				}
			ave_sess_free_bufs(&dev, &session);
			memset(&session, 0, sizeof(session));
		}
	}
	for (unsigned int id = 0xe01; id <= 0xe0b; id++) {
		u32 len = id == 0xe06 ? 0x48 : 0x40;

		put_unaligned_le16(id, msg);
		put_unaligned_le32(10, msg + 0x10);
		put_unaligned_le32(0x19, msg + 0x1c);
		put_unaligned_le32(0x00ee0000, msg + 0x38);
		put_unaligned_le32(0x12345678, msg + 0x40);
		if (id == 0xe07 || id == 0xe08) {
			assert(ave_abi_6070.parse_reply(&ave_abi_6070, msg, len,
							&reply) == -EOPNOTSUPP);
			continue;
		}
		assert(ave_abi_6070.parse_reply(&ave_abi_6070, msg, len,
						&reply) == 0);
		assert(reply.done_id == id && reply.client == 10 &&
		       reply.subtype == 0x19);
		assert(!reply.status && reply.raw_status == 0x00ee0000);
		assert(reply.process_extra == (id == 0xe06 ? 0x12345678U : 0));
		assert(ave_abi_6070.parse_reply(&ave_abi_6070, msg, len - 1,
						&reply) == -EPROTO);
		put_unaligned_le32(1, msg + 0x38);
		assert(ave_abi_6070.parse_reply(&ave_abi_6070, msg, len,
						&reply) == 0);
		assert(reply.status == 1);
	}
	/* Complete drains with barriers, never with the queue-purging Flush. */
	unsigned int sent = submissions, settled = settlements;

	params(&session.p, 2, 1920, 1080);
	assert(ave_cmd_complete(&ctx, 3) == 0);
	assert(submissions == sent + 3 && settlements == settled + 3);
	assert(wire_len == 0x40 && get_unaligned_le16(wire) == 13);
	assert(get_unaligned_le32(wire + 0x1c) == 9);
	/* EncodeDone routing must not free another session or slot. */
	params(&session.p, 2, 1920, 1080);
	session.client = 10;
	session.busy = 1;
	session.slot[4].busy = true;
	dev.sessions[0] = &ctx;
	put_unaligned_le16(0xe06, msg);
	put_unaligned_le32(11, msg + 0x10);
	put_unaligned_le32(0x19, msg + 0x1c);
	put_unaligned_le32(0x00ee0000, msg + 0x38);
	assert(ave_abi_6070.parse_reply(&ave_abi_6070, msg, 0x48, &reply) == 0);
	ave_sess_reply(&dev, &reply);
	assert(session.busy == 1 && !session.done_count);
	put_unaligned_le32(10, msg + 0x10);
	assert(ave_abi_6070.parse_reply(&ave_abi_6070, msg, 0x48, &reply) == 0);
	ave_sess_reply(&dev, &reply);
	assert(!session.busy && session.done_count == 1 &&
	       session.done_fifo[0] == 4);
	assert(session.slot[4].done && !session.slot[4].status &&
	       ctx.ready == 1);
	ave_sess_reply(&dev, &reply);
	assert(session.done_count == 1 && ctx.ready == 1);
	assert(alloc_count == free_count);
	free(expected);
	free(actual);
	puts("ok: 6070 named fields, retained partitions, bounds, parser, CRC refusal");
}
#endif

int main(int argc, char **argv)
{
	struct ave_dev dev = {
		.variant = &test_variant,
#ifdef TEST_LAYOUT
		.layout = &ave_layout_9003_78_0,
#endif
	};
	struct ave_session session = {};
	struct ave_ctx ctx = { .ave = &dev, .sess = &session };

	if (argc > 1 && !strcmp(argv[1], "--matrix")) {
		/* Byte-for-byte regression input, including addresses and headers. */
		static const u32 geometry[][2] = {
			{ 320, 240 },	{ 1280, 720 },	{ 1366, 768 },
			{ 1920, 1080 }, { 2408, 1506 }, { 3840, 2160 },
		};
		static const u32 rates[] = { 1000000, 4000000, 12000000 };

		for (unsigned int i = 0; i < 288; i++) {
			u32 codec = i < 216 ? 1 : 2;
			u32 n = codec == 1 ? i : i - 216;
			u32 mode = n % 2 ? 4 : 2;

			n /= 2;
			u32 rate = rates[n % 3];

			n /= 3;
			u32 b = n % 2;

			n /= 2;
			u32 profile = codec == 1 ? 2 + 2 * (n % 3) : 1;

			if (codec == 1)
				n /= 3;
			params(&session.p, codec, geometry[n][0],
			       geometry[n][1]);
			session.p.profile = profile;
			session.p.high = codec == 1 && profile == 6;
			session.p.bframes = b;
			session.p.refs = b ? 5 : 2;
			session.p.slots = b ? 5 : 3;
			session.p.reorder_off = !b;
			session.p.bitrate = rate;
			session.p.rc_mode = mode;
			session.p.limit_bytes = mode == 4 ? rate / 4 : 0;
			start(&ctx);
			assert(fwrite(wire, 1, wire_len, stdout) == wire_len);
			for (unsigned int slot = 0; slot < session.p.slots;
			     slot++) {
				frame(&ctx, slot, slot == 1);
				assert(fwrite(wire, 1, wire_len, stdout) ==
				       wire_len);
			}
			ave_sess_free_bufs(&dev, &session);
			memset(&session, 0, sizeof(session));
		}
		return 0;
	}

	if (argc > 1 && !strcmp(argv[1], "--case")) {
		/* An optional synthetic case for offline, private capture comparison. */
		assert(argc == 20);
#ifndef TEST_BASELINE
		if (atoi(argv[2]))
			dev.layout = &ave_layout_26a428;
#endif
		params(&session.p, atoi(argv[3]), atoi(argv[4]), atoi(argv[5]));
		session.p.profile = atoi(argv[6]);
		session.p.high = session.p.profile == 6;
		session.p.level = atoi(argv[7]);
		session.p.bitrate = atoi(argv[8]);
		session.p.rc_mode = atoi(argv[9]);
		session.p.bframes = atoi(argv[10]);
		session.p.refs = atoi(argv[11]);
		session.p.slots = atoi(argv[12]);
		session.p.gop = atoi(argv[13]);
		session.p.fps_int = atoi(argv[14]);
		session.p.fps_num = session.p.fps_int;
		session.p.stride = atoi(argv[15]);
		session.p.reorder_off = atoi(argv[16]);
		session.p.intra_only = session.p.gop == 1 &&
				       session.p.codec == 1;
		session.p.limit_bytes =
			session.p.rc_mode == 4 ? session.p.bitrate / 4 : 0;
#ifndef TEST_BASELINE
		session.p.initial_qp = atoi(argv[17]);
		session.p.bit_depth = atoi(argv[18]);
		if (session.p.bit_depth == 10)
			session.p.min_qp = -12;
		if (session.p.bit_depth == 10)
			session.p.coded_cap = session.p.coded_cap * 5 / 4;
		if (session.p.rc_mode == 3) {
			session.p.max_qp = 48;
			session.p.coded_cap =
				session.p.initial_qp == 40 ? 0x3a4000 :
				session.p.initial_qp == 28 ? 0x458000 :
							     0x5fc000;
		}
#endif
		start(&ctx);
		if (atoi(argv[19])) {
			session.frames = 0;
			frame(&ctx, 0, false);
		}
		assert(fwrite(wire, 1, wire_len, stdout) == wire_len);
		return 0;
	}
#ifndef TEST_BASELINE
	facts();
#ifdef TEST_PER_ABI
	facts_6070();
#endif
#else
	(void)test_regs;
#endif
	/* Exercise alignment, profile/GOP/rate combinations under sanitizers. */
	static const u32 sizes[][2] = {
		{ 320, 240 },	{ 640, 480 },	{ 1280, 720 },	{ 1366, 768 },
		{ 1920, 1080 }, { 2408, 1506 }, { 3840, 2160 },
	};
	for (unsigned int c = 1; c <= 2; c++)
		for (unsigned int i = 0; i < ARRAY_SIZE(sizes); i++) {
			params(&session.p, c, sizes[i][0], sizes[i][1]);
			start(&ctx);
			for (unsigned int s = 0; s < session.p.slots; s++)
				frame(&ctx, s, true);
			ave_sess_free_bufs(&dev, &session);
			memset(&session, 0, sizeof(session));
		}
	assert(alloc_count == free_count);
	puts("ok: J700 builder and allocation alignment matrix");
	return 0;
}
