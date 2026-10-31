// SPDX-License-Identifier: GPL-2.0-only OR MIT
#define AVD_VP9_HOST_TEST
#include "../avd-vp9-fp.c"

static void test_dependency(void)
{
	for (unsigned int flags = 0; flags < 0x1000; flags++)
		assert(avd_vp9_needs_counts(flags) ==
		       (!!(flags & V4L2_VP9_FRAME_FLAG_REFRESH_FRAME_CTX) &&
			!(flags & V4L2_VP9_FRAME_FLAG_PARALLEL_DEC_MODE)));
}

static void test_slots_and_layouts(void)
{
	struct avd_variant variant = { .vp9_fp_generation = AVD_VP9_FP_BORAGE };
	struct avd_dev dev = { .variant = &variant };
	struct avd_ctx ctx = { .dev = &dev };
	struct avd_buf bufs[AVD_MAX_FRAMES] = {};
	struct avd_frame frames[AVD_MAX_FRAMES] = {};
	struct v4l2_ctrl_vp9_frame ctrl = {};

	for (unsigned int i = 0; i < AVD_MAX_FRAMES; i++) {
		bufs[i].size = 0x1130d0;
		bufs[i].cpu = calloc(1, bufs[i].size);
		assert(bufs[i].cpu);
		frames[i].fp = &bufs[i];
		ctrl.tile_cols_log2 = i % 7;
		ctrl.tile_rows_log2 = i % 3;
		assert(avd_vp9_pack_fp(&ctx, &frames[i], &ctrl, NULL, NULL, 0) ==
		       -EOPNOTSUPP);
	}
	for (unsigned int i = 0; i < AVD_MAX_FRAMES; i++) {
		u8 *fp = bufs[i].cpu;
		assert(get_unaligned_le16(fp + 0x218) == (1U << (i % 7)));
		assert(get_unaligned_le16(fp + 0x21a) == (1U << (i % 3)));
	}
	ctrl.tile_cols_log2 = 6;
	ctrl.tile_rows_log2 = 2; /* exactly 256 tiles */
	assert(pack_borage(&frames[0], &ctrl) == -EOPNOTSUPP);
	ctrl.tile_cols_log2 = 7;
	assert(pack_borage(&frames[0], &ctrl) == -EINVAL);
	ctrl.tile_cols_log2 = 0;
	ctrl.tile_rows_log2 = 255; /* reject before shift */
	assert(pack_borage(&frames[0], &ctrl) == -EINVAL);
	for (unsigned int gen = AVD_VP9_FP_T8112; gen <= AVD_VP9_FP_T6000; gen++) {
		variant.vp9_fp_generation = gen;
		assert(avd_vp9_pack_fp(&ctx, &frames[0], &ctrl, NULL, NULL, 0) ==
		       -EOPNOTSUPP);
	}
	for (unsigned int i = 0; i < AVD_MAX_FRAMES; i++) free(bufs[i].cpu);
}

static void test_v3(void)
{
	u8 bytes[0x24aa4] = {};
	u8 prog[128 * 4] = {};
	struct avd_buf fp = { .cpu = bytes, .size = sizeof(bytes) };
	struct avd_buf instructions = { .cpu = prog, .size = sizeof(prog) };
	struct avd_frame f = { .fp = &fp, .prog = &instructions, .prog_words = 54 };
	struct v4l2_ctrl_vp9_frame ctrl = { .flags = V4L2_VP9_FRAME_FLAG_KEY_FRAME };

	for (unsigned int i = 0; i < 49; i++)
		put_unaligned_le32(0x10203000 + i, prog + i * 4);
	put_unaligned_le32(0x2d800000, prog + 49 * 4);
	put_unaligned_le32(0x123400, prog + 50 * 4);
	put_unaligned_le32(17, prog + 51 * 4);
	put_unaligned_le32(0x2a000000, prog + 52 * 4);
	put_unaligned_le32(0x70003, prog + 53 * 4);
	assert(pack_v3(&f, &ctrl, 0x120000) == -EOPNOTSUPP);
	/* Independent checks against fp.py offsets, not the production map. */
	assert(get_unaligned_le32(bytes + 0x28) == 0x10203002);
	assert(get_unaligned_le32(bytes + 0x2c) == 0x10203005);
	assert(get_unaligned_le32(bytes + 0x30) == 0x10203000);
	assert(get_unaligned_le32(bytes + 0x4c) == 0x10203019);
	assert(get_unaligned_le32(bytes + 0x104) == 0x1200);
	assert(get_unaligned_le32(bytes + 0x168) == 0x1020302b);
	assert(get_unaligned_le32(bytes + 0xab8) == 0x123400);
	assert(get_unaligned_le32(bytes + 0xabc) == 17);
	assert(get_unaligned_le32(bytes + 0xab4) == 0);
	assert(get_unaligned_le32(bytes + 0xab0) == 0x40005);
	assert(get_unaligned_le32(bytes + 0xe0) == 0x240);
	f.prog_words = 53;
	assert(pack_v3(&f, &ctrl, 0) == -EINVAL);
}

static void test_command(void)
{
	struct avd_variant variant = { .vp9_fp_generation = AVD_VP9_FP_BORAGE };
	struct avd_dev dev = { .variant = &variant };
	struct avd_ctx ctx = { .dev = &dev };
	struct avd_buf fp = { .cpu = calloc(1, 0x1130d0),
		.addr = 0x200000, .size = 0x1130d0 };
	struct avd_frame f = { .fp = &fp };
	u8 *cmd = (u8 *)f.cmd, *bytes = fp.cpu;

	bytes[0x18] = 1;
	bytes[0x19] = 0;
	put_unaligned_le32(0x55, bytes + 0x2ac);
	put_unaligned_le16(12, bytes + 0x1a);
	for (unsigned int work = 0; work < 4; work++) {
		memset(cmd, 0, sizeof(f.cmd));
		put_unaligned_le32(0xa5a5fe00, cmd);
		put_unaligned_le16(work, cmd + 6);
		assert(avd_vp9_pack_cmd(&ctx, &f) == -EOPNOTSUPP);
		assert(get_unaligned_le32(cmd) == (0xa5a5fe00 | 0x155));
		assert(get_unaligned_le32(cmd + 4) == 0x01483120 + work * 0x1814);
		assert(get_unaligned_le32(cmd + 0x14) == 0x014831a0 + work * 0x1814);
		assert(get_unaligned_le16(cmd + 0xc) == 12);
		assert(get_unaligned_le32(cmd + 0x24) == fp.addr + 0x2b0);
		assert(get_unaligned_le32(cmd + 0x38) == 0);
	}
	put_unaligned_le16(4, cmd + 6);
	assert(avd_vp9_pack_cmd(&ctx, &f) == -EINVAL);
	put_unaligned_le16(0, cmd + 6);
	fp.addr = 1ULL << 40;
	assert(avd_vp9_pack_cmd(&ctx, &f) == -ERANGE);
	free(fp.cpu);
}

int main(void)
{
	test_dependency();
	test_slots_and_layouts();
	test_v3();
	test_command();
	puts("VP9 fp: dependency table, 16 slots, V3 fields, Borage fields and bounds PASS");
	return 0;
}
