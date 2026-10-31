/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#include "avd-hevc-fp.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

static u32 get32(const u8 *p)
{
	return p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24;
}

static void put32(u8 *p, u32 v)
{
	p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24;
}

static void test_ranges(void)
{
	struct v4l2_ctrl_hevc_slice_params sl[2] = {};
	struct avd_hevc_slice_range ranges[2];

	sl[0].bit_size = 800; sl[0].data_byte_offset = 10;
	sl[1].bit_size = 1600; sl[1].data_byte_offset = 20;
	assert(!avd_hevc_slice_ranges(sl, 2, 300, ranges));
	assert(ranges[0].offset == 10 && ranges[0].size == 90);
	assert(ranges[1].offset == 120 && ranges[1].size == 180);
	sl[0].bit_size = 720;
	sl[1].bit_size = 1440; sl[1].data_byte_offset = 120;
	assert(!avd_hevc_slice_ranges(sl, 2, 300, ranges));
	assert(ranges[0].offset == 10 && ranges[0].size == 90);
	assert(ranges[1].offset == 120 && ranges[1].size == 180);
	sl[1].data_byte_offset = 99;
	assert(avd_hevc_slice_ranges(sl, 2, 300, ranges) == -EINVAL);
	sl[1].data_byte_offset = UINT32_MAX;
	assert(avd_hevc_slice_ranges(sl, 2, 300, ranges) == -EINVAL);
	sl[1].data_byte_offset = 120; sl[1].bit_size = 1600;
	assert(avd_hevc_slice_ranges(sl, 2, 300, ranges) == -EINVAL);
	assert(avd_hevc_slice_ranges(sl, 0, 300, ranges) == -EINVAL);
	sl[0].bit_size = 1; sl[0].data_byte_offset = 0;
	assert(!avd_hevc_slice_ranges(sl, 1, 1, ranges));
	assert(ranges[0].size == 1);
	sl[0].bit_size = UINT32_MAX;
	assert(avd_hevc_slice_ranges(sl, 1, 1, ranges) == -EINVAL);
}

static void test_v3(u8 *cpu, size_t size)
{
	struct avd_hevc_fp_builder fp;
	u32 offsets[4] = { 0x100, 0, 0x300, 0x200 };
	u32 cmd[24] = {};
	unsigned int i;

	assert(!avd_hevc_fp_begin(&fp, AVD_HEVC_FP_V3, cpu, size, 9, 1));
	avd_hevc_fp_emit(&fp, HEVC_FP_DIMS8, 0x001d0027);
	avd_hevc_fp_emit(&fp, HEVC_FP_PPS_FLAGS, 0x123456);
	assert(get32(cpu + 0x28) == 0x001d0027);
	assert(get32(cpu + 0x5c) == 0x123456);
	avd_hevc_fp_emit(&fp, HEVC_FP_TXFM, 0x01010000);
	assert(fp.error == -EOPNOTSUPP && get32(cpu + 0x2c) == 0);
	fp.error = 0;
	for (i = 0; i < 24; i++)
		avd_hevc_fp_emit(&fp, HEVC_FP_SCALING4, 0x01020304 + i);
	assert(get32(cpu + 0x610) == 0x01020304);
	assert(get32(cpu + 0x66c) == 0x01020304 + 23);
	avd_hevc_fp_address(&fp, HEVC_FP_Y, 0x123400, 0);
	assert(get32(cpu + 0x1b4) == 0x1234);
	avd_hevc_fp_reference(&fp, 0, 0x400000, offsets);
	avd_hevc_fp_reference(&fp, 2, 0x500000, offsets);
	assert(get32(cpu + 0x104) == 0x400100 >> 7);
	assert(get32(cpu + 0x138) == 0x500100 >> 7);
	assert(get32(cpu + 0x198) == 0x500200 >> 7);
	/* Do not write beyond the licensed array into the next section. */
	avd_hevc_fp_emit(&fp, HEVC_FP_SCALING4, 0xffffffff);
	assert(fp.error == -EOPNOTSUPP && get32(cpu + 0x670) == 0);
	assert(avd_hevc_fp_pack_cmd(&fp, cmd, 0, 4) == -EOPNOTSUPP);
	assert(avd_hevc_fp_begin(&fp, AVD_HEVC_FP_V3, cpu, 32, 9, 1) == -ENOSPC);
	assert(avd_hevc_fp_begin(&fp, (enum avd_hevc_fp_layout_id)-1, cpu, size, 9, 1) == -EINVAL);
}

static void test_borage(u8 *cpu, size_t size)
{
	struct avd_hevc_fp_builder fp;
	u32 cmd[24] = {};
	unsigned int i;

	assert(!avd_hevc_fp_begin(&fp, AVD_HEVC_FP_BORAGE, cpu, size, 42, 2));
	assert(get32(cpu + 0x10) == 42);
	for (i = 0; i < 4; i++) {
		cpu[0x1c + 2 * i] = 1;
		put32(cpu + 0x24 + 4 * i, 0x1000 + 4 * i);
	}
	assert(!avd_hevc_fp_validate_work(cpu, size, 4));
	cpu[0x18] = 1; cpu[0x19] = 1;
	put32(cpu + 0x314, 0x55);
	cmd[0] = 1 | 1 << 9;
	assert(avd_hevc_fp_pack_cmd(&fp, cmd, 3, 4) == -EOPNOTSUPP);
	assert(cmd[0] == (2U << 16 | 1U << 9 | 0x55 << 2 | 3));
	assert(cmd[1] == 0x10001 && cmd[2] == 0x10001);
	assert(cmd[3] == 0x0148312c + 3 * 0x1814);
	assert(cmd[4] == 0x318 && cmd[5] == cmd[3] + 0x24);
	assert(cmd[6] == 0x6fc && cmd[7] == 0xbdc);
	assert(cmd[10] == 0x014831a0 + 3 * 0x1814);
	assert(cmd[11] == 0x014833ac + 3 * 0x1814);
	assert(cmd[12] == cmd[3] + 0x2dc);
	assert(cmd[15] == 0 && cmd[17] == 0);
	/* Work limit: 0x1000 / 4, then one over it. */
	cpu[0x1c] = 0; cpu[0x1d] = 4;
	for (i = 1; i < 4; i++)
		put32(cpu + 0x24 + 4 * i, 0x3000 + 4 * i);
	assert(!avd_hevc_fp_validate_work(cpu, size, 4));
	cpu[0x1c] = 1;
	assert(avd_hevc_fp_validate_work(cpu, size, 4) == -EINVAL);
	cpu[0x1c] = 1; cpu[0x1d] = 0;
	put32(cpu + 0x28, 0x1000);
	assert(avd_hevc_fp_validate_work(cpu, size, 4) == -EINVAL);
	put32(cpu + 0x28, size);
	assert(avd_hevc_fp_validate_work(cpu, size, 4) == -EINVAL);
	cpu[0x1a] = 0x58; cpu[0x1b] = 2;
	assert(avd_hevc_fp_validate_work(cpu, size, 4) == -EINVAL);
	assert(avd_hevc_fp_validate_work(cpu, 16, 4) == -EINVAL);
	assert(avd_hevc_fp_validate_work(cpu, size, 0) == -EINVAL);
	assert(avd_hevc_fp_validate_work(cpu, size, 5) == -EINVAL);
}

int main(void)
{
	size_t size = 0x1130d0;
	u8 *cpu = malloc(size);

	assert(cpu);
	test_ranges();
	test_v3(cpu, size);
	test_borage(cpu, size);
	free(cpu);
	puts("PASS: slice ranges, V3 offsets and bounds, Borage command fields/work validation");
	return 0;
}
