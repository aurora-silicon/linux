// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * V3 offsets: eiln/avd, avid/h265/fp.py and halv3.py (MIT),
 * Copyright 2023 Eileen Yoon <eyn@gmx.com>.
 * Borage offsets/constants: Apple AVD protocol specification, section 6.3.
 * No Borage syntax offset is inferred by translating the V3 structure.
 */
#include "avd-hevc-fp.h"
#ifndef AVD_HEVC_HOST_TEST
#include <linux/errno.h>
#include <linux/string.h>
#endif

struct avd_hevc_fp_layout {
	size_t size;
	u32 fields[HEVC_FP_FIELD_COUNT];
	/* [C] candidate offsets; no unproved syntax bit definitions. */
	u32 c_offsets[4];
};

static const struct avd_hevc_fp_layout layouts[] = {
	[AVD_HEVC_FP_V3] = {
		.size = 0x34d0c,
		.fields = {
			[HEVC_FP_DIMS8] = 0x28, [HEVC_FP_TXFM] = 0x2c,
			[HEVC_FP_PCM] = 0x30, [HEVC_FP_FLAGS] = 0x34,
			[HEVC_FP_START] = 0x4c, [HEVC_FP_MODE] = 0x50,
			[HEVC_FP_DIMS] = 0x54, [HEVC_FP_PIXFMT] = 0x58,
			[HEVC_FP_PPS_FLAGS] = 0x5c, [HEVC_FP_PPS_QP] = 0x60,
			[HEVC_FP_SCALING_DIMS] = 0x7c,
			[HEVC_FP_DC16] = 0x80, [HEVC_FP_DC32] = 0x88,
			[HEVC_FP_SCALING4] = 0x610,
			[HEVC_FP_SCALING8] = 0x670,
			[HEVC_FP_SCALING16] = 0x7f0,
			[HEVC_FP_SCALING32] = 0x970,
			[HEVC_FP_FEATURES] = 0x98,
			[HEVC_FP_SCRATCH] = 0xdc,
			[HEVC_FP_CURRENT] = 0x104,
			[HEVC_FP_REF_HDR] = 0x114,
			[HEVC_FP_Y] = 0x1b4, [HEVC_FP_UV] = 0x1b8,
			[HEVC_FP_Y_STRIDE] = 0x1bc,
			[HEVC_FP_UV_STRIDE] = 0x1c0,
			/* Actual Struct offsets, including the leading word at a84. */
			[HEVC_FP_REF_TYPE] = 0xa88,
			[HEVC_FP_REF_LIST] = 0xa8c,
			[HEVC_FP_WEIGHT_DENOM] = 0xb08,
			[HEVC_FP_WEIGHTS] = 0xb0c,
			[HEVC_FP_WEIGHT_OFFSETS] = 0xb6c,
			[HEVC_FP_QUANT] = 0xbcc,
			[HEVC_FP_DEBLOCK] = 0xbd0,
			[HEVC_FP_MV] = 0xbd4,
			[HEVC_FP_SLICE_ADDR] = 0xbd8,
			[HEVC_FP_SLICE_SIZE] = 0xbdc,
		},
		/* [C] Main10 differential capture; V3 header candidates only. */
		.c_offsets = { 0x2c, 0x58 },
	},
	[AVD_HEVC_FP_BORAGE] = {
		.size = 0x1130d0,
		/* [C] Header candidates from the known PIO region, not V3 aliases. */
		.c_offsets = { 0x314, 0x318, 0x6fc, 0xbdc },
	},
	/* TODO t-3862: identify the Apple class before assigning any offsets. */
	[AVD_HEVC_FP_T8112_TODO] = {},
	[AVD_HEVC_FP_T8140_TODO] = {},
};

static u16 read16(const void *cpu, u32 off)
{
	const u8 *p = (const u8 *)cpu + off;

	return p[0] | (u16)p[1] << 8;
}

static u32 read32(const void *cpu, u32 off)
{
	const u8 *p = (const u8 *)cpu + off;

	return p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24;
}

static void write32(void *cpu, u32 off, u32 value)
{
	u8 *p = (u8 *)cpu + off;

	p[0] = value;
	p[1] = value >> 8;
	p[2] = value >> 16;
	p[3] = value >> 24;
}

static void store(struct avd_hevc_fp_builder *fp, u32 off, u32 value)
{
	if (off > fp->size || fp->size - off < 4) {
		fp->error = -ENOSPC;
		return;
	}
	write32(fp->cpu, off, value);
}

int avd_hevc_fp_begin(struct avd_hevc_fp_builder *fp,
		      enum avd_hevc_fp_layout_id layout, void *cpu, size_t size,
		      u32 frame_num, unsigned int slices)
{
	if ((unsigned int)layout >= sizeof(layouts) / sizeof(layouts[0]) || !cpu)
		return -EINVAL;
	memset(fp, 0, sizeof(*fp));
	fp->cpu = cpu;
	fp->size = size;
	fp->layout = layout;
	if (size < layouts[layout].size)
		return -ENOSPC;
	memset(cpu, 0, size);
	if (layout == AVD_HEVC_FP_BORAGE) {
		if (!slices || slices >= 0x258)
			return -EINVAL;
		store(fp, 0x10, frame_num);
		((u8 *)cpu)[0x1a] = slices;
		((u8 *)cpu)[0x1b] = slices >> 8;
	}
	return 0;
}

void avd_hevc_fp_emit(struct avd_hevc_fp_builder *fp,
		      enum avd_hevc_fp_field field, u32 value)
{
	u32 off, count;

	if (!fp->cpu || fp->layout != AVD_HEVC_FP_V3 || !field ||
	    field >= HEVC_FP_FIELD_COUNT)
		return;
	/* [C] Linux stream bits 15:23 at V3 fp+0x2c need an Apple capture. */
	if (field == HEVC_FP_TXFM && (value & 0x00ff8000)) {
		fp->error = -EOPNOTSUPP;
		return;
	}
	/* fp.py describes one slice record. Multi-record stride is unproved. */
	if (fp->segment > 1 && field >= HEVC_FP_REF_LIST)
		return;
	off = layouts[fp->layout].fields[field];
	if (!off)
		return;
	switch (field) {
	case HEVC_FP_DC16: case HEVC_FP_DC32:
		count = 2;
		break;
	case HEVC_FP_SCALING4:
		count = 24;
		break;
	case HEVC_FP_SCALING8: case HEVC_FP_SCALING16:
		count = 96;
		break;
	case HEVC_FP_SCALING32:
		count = 32;
		break;
	case HEVC_FP_REF_HDR:
		count = 8;
		break;
	case HEVC_FP_REF_LIST:
		count = 30;
		break;
	case HEVC_FP_WEIGHTS: case HEVC_FP_WEIGHT_OFFSETS:
		count = 24;
		break;
	default:
		count = 1;
		break;
	}
	if (count == 1) {
		store(fp, off, value);
	} else if (fp->cursor[field] < count) {
		store(fp, off + 4 * fp->cursor[field]++, value);
	} else {
		fp->error = -EOPNOTSUPP;
	}
}

void avd_hevc_fp_address(struct avd_hevc_fp_builder *fp,
			 enum avd_hevc_fp_field field, u64 addr,
			 unsigned int index)
{
	u32 off;

	if (!fp->cpu || fp->layout != AVD_HEVC_FP_V3)
		return;
	if (field == HEVC_FP_MV && fp->segment > 1)
		return;
	if (field == HEVC_FP_SCRATCH) {
		if (index >= 10) {
			fp->error = -EINVAL;
			return;
		}
		off = 0xdc + 4 * index;
	} else {
		if (field != HEVC_FP_Y && field != HEVC_FP_UV &&
		    field != HEVC_FP_MV)
			return;
		off = layouts[fp->layout].fields[field];
	}
	if (addr >> 40) {
		fp->error = -ERANGE;
		return;
	}
	store(fp, off, addr >> 8);
}

void avd_hevc_fp_reference(struct avd_hevc_fp_builder *fp,
			  unsigned int index, u64 addr, const u32 offsets[4])
{
	unsigned int i;

	if (!fp->cpu || fp->layout != AVD_HEVC_FP_V3)
		return;
	if (index > 8) {
		fp->error = -EOPNOTSUPP;
		return;
	}
	for (i = 0; i < 4; i++) {
		u32 off = index ? 0x134 + 0x20 * i + 4 * (index - 1) :
				  0x104 + 4 * i;

		if ((addr + offsets[i]) >> 39) {
			fp->error = -ERANGE;
			return;
		}
		store(fp, off, (addr + offsets[i]) >> 7);
	}
}

int avd_hevc_fp_validate_work(const void *cpu, size_t size,
			     unsigned int active_vps)
{
	u32 previous = 0;
	unsigned int i;

	if (!cpu || size < 0x34 || !active_vps || active_vps > 4 ||
	    !read16(cpu, 0x1a) || read16(cpu, 0x1a) >= 0x258)
		return -EINVAL;
	/* Four count fields are proved. The firmware traversal count is [C]. */
	for (i = 0; i < active_vps; i++) {
		u32 count = read16(cpu, 0x1c + 2 * i);
		u32 off = read32(cpu, 0x24 + 4 * i);

		if (!count || count > 0x1000 / active_vps || off < 0x34 ||
		    (off & 3) || (i && off <= previous) || off >= size)
			return -EINVAL;
		previous = off;
	}
	return 0;
}

int avd_hevc_fp_pack_cmd(const struct avd_hevc_fp_builder *fp, u32 cmd[24],
			 unsigned int work_index, unsigned int active_vps)
{
	u32 base, stride;
	int ret;

	if (fp->layout != AVD_HEVC_FP_BORAGE)
		return -EOPNOTSUPP;
	if (fp->error)
		return fp->error;
	if (work_index > 3 || fp->size < layouts[fp->layout].size)
		return -EINVAL;
	ret = avd_hevc_fp_validate_work(fp->cpu, fp->size, active_vps);
	if (ret)
		return ret;
	stride = 0x1814 * work_index;
	base = 0x0148312c + stride;
	/* [C] fp+0x314 bits 0:6 -> cmd+0x00 bits 2:8; no bit names. */
	cmd[0] = (cmd[0] & 0x0000fe00) |
		 (!!((const u8 *)fp->cpu)[0x18]) |
		 (!!((const u8 *)fp->cpu)[0x19] << 1) |
		 ((read32(fp->cpu, 0x314) & 0x7f) << 2) |
		 ((u32)read16(fp->cpu, 0x1a) << 16);
	cmd[1] = read16(fp->cpu, 0x1c) | (u32)read16(fp->cpu, 0x1e) << 16;
	cmd[2] = read16(fp->cpu, 0x20) | (u32)read16(fp->cpu, 0x22) << 16;
	cmd[3] = base;
	cmd[4] = 0x318;
	cmd[5] = base + 0x24;
	cmd[6] = 0x6fc;
	cmd[7] = 0xbdc;
	cmd[10] = 0x014831a0 + stride;
	cmd[11] = 0x014833ac + stride;
	cmd[12] = base + 0x2dc;
	/*
	 * [C] cmd+0x3c/+0x44: packed addresses from fp+0x318/+0x6fc/
	 * +0xae0 and 0x01482f10. Exact shifts and masks are not specified.
	 * Do not submit a partially packed command. The caller must propagate
	 * this error; the fields above are useful to differential host tests.
	 */
	return -EOPNOTSUPP;
}
