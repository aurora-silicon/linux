// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* H.264 frame-parameter facts. No firmware or captured frame images. */
#include <linux/errno.h>
#include <linux/limits.h>
#include <linux/string.h>
#include <linux/unaligned.h>

#include "avd.h"
#include "avd-queue.h"
#include "avd-h264-fp.h"

/*
 * Each generation has an independent provenance. Register revisions or firmware
 * tiers do not imply a frame layout. Select this enum from OF match data only.
 *
 * V3: https://github.com/eiln/avd/blob/main/avid/h264/fp.py (MIT), with
 *     avid/h264/halv3.py for the address shifts and instruction fields.
 * Borage: clean-room protocol SPEC (2026-10-31), sections 4.1, 6.1, 6.2.
 * T6000 selects LilyD: V3 is comparison evidence, not its selected Apple ABI.
 * TODO rows: Daybreak per-SoC ledger task t-3862; no V3/Borage fallback.
 */
struct avd_h264_fp_layout_info {
	enum avd_h264_fp_layout layout;
	size_t size;
	const char *source;
};

static const struct avd_h264_fp_layout_info avd_h264_fp_layouts[] = {
	{ AVD_H264_FP_V3, 0x8b4e0, "MIT eiln/avd avid/h264/fp.py: Viola/V3 comparison" },
	{ AVD_H264_FP_BORAGE, 0x1130d0, "SPEC 6.2: T6040/Borage" },
	{ AVD_H264_FP_T8112_TODO, 0, "TODO t-3862: T8112 per-SoC ledger" },
	{ AVD_H264_FP_T8140_TODO, 0, "TODO t-3862: T8140 per-SoC ledger" },
};

/* Offsets and array capacities from the public V3 layout, not Borage. */
static const struct {
	u16 offset;
	u16 count;
} v3_fields[AVD_H264_FP_NONE] = {
	[AVD_H264_FP_MV_ABOVE_INFO] = { 0xb8, 1 },
	[AVD_H264_FP_MV_COLOR] = { 0xbc, 1 },
	[AVD_H264_FP_HDR_D0_REF_HDR] = { 0xd0, 16 },
	[AVD_H264_FP_HDR_4C_PIC_SCALING_LIST_DIMS] = { 0x4c, 1 },
	[AVD_H264_FP_SCL_46C_PIC_SCALING_MATRIX_4X4] = { 0x46c, 24 },
	[AVD_H264_FP_SCL_4CC_PIC_SCALING_MATRIX_8X8] = { 0x4cc, 96 },
	[AVD_H264_FP_HDR_34_START_HDR] = { 0x34, 1 },
	[AVD_H264_FP_HDR_38_MODE] = { 0x38, 1 },
	[AVD_H264_FP_HDR_3C_HEIGHT_WIDTH] = { 0x3c, 1 },
	[AVD_H264_FP_HDR_40_ZERO] = { 0x40, 1 },
	[AVD_H264_FP_HDR_28_HEIGHT_WIDTH_SHIFT3] = { 0x28, 1 },
	[AVD_H264_FP_HDR_2C_SPS_PARAM] = { 0x2c, 1 },
	[AVD_H264_FP_HDR_44_FLAGS] = { 0x44, 1 },
	[AVD_H264_FP_HDR_48_CHROMA_QP_INDEX_OFFSET] = { 0x48, 1 },
	[AVD_H264_FP_HDR_58_CONST_3A] = { 0x58, 1 },
	[AVD_H264_FP_HDR_9C_PPS_TILE_ADDR_LSB8] = { 0x9c, 1 },
	[AVD_H264_FP_IP_ABOVE] = { 0xa0, 1 },
	[AVD_H264_FP_LF_ABOVE] = { 0xa4, 1 },
	[AVD_H264_FP_LF_ABOVE_INFO] = { 0xa8, 1 },
	[AVD_H264_FP_HDR_210_Y_ADDR_LSB8] = { 0x210, 1 },
	[AVD_H264_FP_HDR_218_WIDTH_ALIGN] = { 0x218, 1 },
	[AVD_H264_FP_HDR_214_UV_ADDR_LSB8] = { 0x214, 1 },
	[AVD_H264_FP_HDR_21C_WIDTH_ALIGN] = { 0x21c, 1 },
	[AVD_H264_FP_HDR_54_HEIGHT_WIDTH] = { 0x54, 1 },
	[AVD_H264_FP_SLC_76C_CMD_WEIGHTS_DENOM] = { 0x76c, 1 },
	[AVD_H264_FP_SLC_LUMA_WEIGHTS] = { 0x770, 96 },
	[AVD_H264_FP_SLC_LUMA_OFFSETS] = { 0x8f0, 96 },
	[AVD_H264_FP_SLC_CHROMA_WEIGHTS_0_] = { 0x770, 96 },
	[AVD_H264_FP_SLC_CHROMA_OFFSETS_0_] = { 0x8f0, 96 },
	[AVD_H264_FP_SLC_CHROMA_WEIGHTS_1_] = { 0x770, 96 },
	[AVD_H264_FP_SLC_CHROMA_OFFSETS_1_] = { 0x8f0, 96 },
	[AVD_H264_FP_SLC_A7C_CMD_SET_CODED_SLICE] = { 0xa7c, 1 },
	[AVD_H264_FP_SLC_A84_SLICE_ADDR_LOW] = { 0xa84, 1 },
	[AVD_H264_FP_SLC_A88_SLICE_HDR_SIZE] = { 0xa88, 1 },
	[AVD_H264_FP_SLC_A70_CMD_QUANT_PARAM] = { 0xa70, 1 },
	[AVD_H264_FP_SLC_A74_CMD_DEBLOCKING_FILTER] = { 0xa74, 1 },
	[AVD_H264_FP_SLC_6E8_CMD_REF_LIST_0] = { 0x6e8, 32 },
	[AVD_H264_FP_SLC_6E4_CMD_REF_TYPE] = { 0x6e4, 1 },
	[AVD_H264_FP_SLC_A78_SPS_TILE_ADDR2_LSB8] = { 0xa78, 1 },
};

static int fp_word(struct avd_buf *buf, size_t offset, u32 word)
{
	if (!buf || !buf->cpu || buf->size < sizeof(u32) ||
	    offset > buf->size - sizeof(u32))
		return -ENOSPC;
	put_unaligned_le32(word, (u8 *)buf->cpu + offset);
	return 0;
}

int avd_h264_fp_begin(struct avd_h264_fp_cursor *cursor, struct avd_buf *buf,
		      enum avd_h264_fp_layout layout)
{
	unsigned int i;
	size_t size = 0;

	memset(cursor, 0, sizeof(*cursor));
	for (i = 0; i < ARRAY_SIZE(avd_h264_fp_layouts); i++)
		if (avd_h264_fp_layouts[i].layout == layout)
			size = avd_h264_fp_layouts[i].size;
	if (layout != AVD_H264_FP_V3 || !size)
		return -EOPNOTSUPP;
	if (!buf || !buf->cpu || buf->size < size)
		return -ENOSPC;
	memset(buf->cpu, 0, size);
	cursor->buf = buf;
	/* V3 piodma words are transport-owned; do not invent their encodings. */
	fp_word(buf, 0x04, 1);
	fp_word(buf, 0x14, 0xdeadcafe);
	fp_word(buf, 0x18, 0x101);
	fp_word(buf, 0x1c, 1);
	fp_word(buf, 0x20, 0x8b4c0);
	/* Public DFW metadata in AVDH264V3DFWScalingList. */
	fp_word(buf, 0x284, 0x1de28b5);
	fp_word(buf, 0x468, 0x1de28b5);
	fp_word(buf, 0x64c, 0x764099);
	fp_word(buf, 0x6c8, 0x124111);
	return 0;
}

/* MIT fp.py: 600 slice records, 0x3b4 bytes each, before input +0x8b4c0. */
int avd_h264_fp_slice(struct avd_h264_fp_cursor *cursor, unsigned int index,
		       unsigned int count)
{
	unsigned int i;

	if (!cursor->buf || !count || count > 600 || index >= count)
		return -EINVAL;
	cursor->slice = index;
	cursor->weights = 0;
	cursor->offsets = 0;
	for (i = AVD_H264_FP_SLC_76C_CMD_WEIGHTS_DENOM; i < AVD_H264_FP_NONE; i++)
		cursor->count[i] = 0;
	return fp_word(cursor->buf, 0x1c, count);
}

int avd_h264_fp_emit(struct avd_h264_fp_cursor *cursor,
		     enum avd_h264_fp_field field, u32 word)
{
	unsigned int index;

	if (field == AVD_H264_FP_NONE)
		return 0;
	if ((unsigned int)field >= AVD_H264_FP_NONE)
		return -EINVAL;
	/* Luma and both chroma components share one ordered weights array. */
	if (v3_fields[field].offset == 0x770)
		index = cursor->weights++;
	else if (v3_fields[field].offset == 0x8f0)
		index = cursor->offsets++;
	else
		index = cursor->count[field]++;
	if (index >= v3_fields[field].count)
		return -E2BIG;
	return fp_word(cursor->buf, v3_fields[field].offset + index * 4 +
		       (v3_fields[field].offset >= 0x6e0 ? cursor->slice * 0x3b4 : 0),
		       word);
}

int avd_h264_fp_address(struct avd_h264_fp_cursor *cursor,
			enum avd_h264_fp_field field, unsigned int index,
			dma_addr_t addr)
{
	if (field == AVD_H264_FP_NONE)
		return 0;
	/* Builder scratch annotations already identify distinct fields. */
	if (field != AVD_H264_FP_HDR_9C_PPS_TILE_ADDR_LSB8)
		index = 0;
	if ((unsigned int)field >= AVD_H264_FP_NONE || index > 7 ||
	    (addr & 0xff) || (addr >> 8) > U32_MAX)
		return -ERANGE;
	return fp_word(cursor->buf, v3_fields[field].offset + index * 4 +
		       (v3_fields[field].offset >= 0x6e0 ? cursor->slice * 0x3b4 : 0),
		       addr >> 8);
}

int avd_h264_fp_ref(struct avd_h264_fp_cursor *cursor, bool reference,
		    unsigned int index, unsigned int plane, dma_addr_t addr)
{
	if (plane >= 4 || index >= 16 || (addr & 0x7f) || (addr >> 7) > U32_MAX)
		return -ERANGE;
	return fp_word(cursor->buf,
		reference ? 0x110 + plane * 0x40 + index * 4 : 0xc0 + plane * 4,
		addr >> 7);
}

/*
 * [C] Borage fp+0x3d8 bits 0:6 -> cmd+0x00 bits 2:8 (SPEC 6.2).
 * [C] Borage fp+0x18/+0x19 -> cmd+0x00 bits 0/1 (SPEC 6.1).
 * [C] cmd+0x3c/+0x44 combine fp+0x3dc/+0x5c0/+0x7a4 and
 *     MMIO 0x01482220; the shift/mask rules are absent from SPEC 6.2.
 * The two +0x1c work counts' exact bounds are also not specified there.
 * Keep these fields unnamed; no guessed command may reach firmware.
 */
int avd_h264_borage_cmd_fields(struct avd_frame *f)
{
	const u8 *fp;
	u32 work, slot, flags;
	u16 count;

	if (!f || !f->fp || !f->fp->cpu || f->fp->size < 0x1130d0 ||
	    f->idx >= AVD_MAX_FRAMES)
		return -EINVAL;
	fp = f->fp->cpu;
	count = get_unaligned_le16(fp + 0x1a);
	/* validateAvcFields' independently established slice-count bound. */
	if (count >= 0x385)
		return -EINVAL;
	work = (f->cmd[1] >> 16) & 0xffff;
	if (work >= 4)
		return -EINVAL;
	work = 0x01483120 + 0x1814 * work;
	slot = 0x1130d0 * f->idx;
	flags = (fp[0x18] & 1) | ((fp[0x19] & 1) << 1) |
		((get_unaligned_le32(fp + 0x3d8) & 0x7f) << 2) | ((u32)count << 16);
	/* Preserve the common prefix outside the codec's documented masks. */
	f->cmd[0] = (f->cmd[0] & ~0xffff01ffU) | flags;
	f->cmd[1] = (f->cmd[1] & 0xffff0000U) | get_unaligned_le16(fp + 0x1c);
	f->cmd[3] = work;
	f->cmd[4] = slot + 0x3dc;
	f->cmd[5] = work + 0x0c;
	f->cmd[6] = slot + 0x5c0;
	f->cmd[7] = slot + 0x8a0;
	f->cmd[10] = work + 0x34;
	f->cmd[11] = work + 0x350;
	f->cmd[12] = work + 0x3ac;
	/* Projection is reviewable/testable, but not a publishable descriptor. */
	return -EOPNOTSUPP;
}

int avd_h264_fp_pack_cmd(enum avd_h264_fp_layout layout, struct avd_frame *f)
{
	/* V3 command transport is not the Borage 0x60 command (SPEC 6.2). */
	if (layout != AVD_H264_FP_BORAGE)
		return -EOPNOTSUPP;
	return avd_h264_borage_cmd_fields(f);
}
