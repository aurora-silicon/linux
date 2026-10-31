// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * AV1 Apple frame parameters. Source: Apple AVD protocol SPEC (2026-10-31),
 * sections 5, 6.1, 6.5, 6.6, 10. No AV1 layout exists in the public V3 code.
 * Keep unproved fields unnamed and refuse submission until they are settled.
 */
#include <linux/unaligned.h>
#include "avd.h"
#include "avd-av1-fp.h"

#define BORAGE_FP_SIZE 0x1130d0
#define BORAGE_WORK_CAP 0x1002

/*
 * [C] table: source                         destination / unresolved rule
 * SPEC 6.1 fp+0x18,+0x19                   cmd+0x00 bits 0,1; meaning
 * SPEC 6.5 fp+0x4f4                        cmd+0x00 bits 2:6,18:19;
 *                                         selected source bits unspecified
 * SPEC 6.5 fp+0x4f4                        cmd+0x00 bits 7:11,13:17;
 *                                         source shifts unspecified
 * SPEC 6.5 work array at fp+0x1c           cmd+0x1c; array encoding
 * SPEC 6.5 fp+0x4fc,+0x5e0,0x01481780      cmd+0x3c,+0x44; packed addresses
 * SPEC 6.5 fp+0x4f8                        cmd+0x4c; source meaning
 * SPEC 6.6 sequence/frame/tile/grain       syntax/DVA offsets still open
 * SPEC 10  T8140                          TODO: identify selected packer;
 *                                         Daybreak per-SoC ledger task
 *                                         t-3862-daybreak-avd-per-soc-ledger-
 *                                         t8140-t6000-t8112-pe
 */

int avd_av1_validate_tiles(const struct v4l2_ctrl_av1_frame *frame,
			   const struct v4l2_ctrl_av1_tile_group_entry *tiles,
			   unsigned int count, size_t payload)
{
	const struct v4l2_av1_tile_info *info = &frame->tile_info;
	unsigned int i, n;

	if (!info->tile_cols || !info->tile_rows ||
	    info->tile_cols > V4L2_AV1_MAX_TILE_COLS ||
	    info->tile_rows > V4L2_AV1_MAX_TILE_ROWS)
		return -EINVAL;
	n = info->tile_cols * info->tile_rows;
	if (n > V4L2_AV1_MAX_TILE_COUNT || n > BORAGE_WORK_CAP ||
	    count < n || count > V4L2_AV1_MAX_TILE_COUNT ||
	    info->context_update_tile_id >= n)
		return -EINVAL;
	for (i = 0; i < n; i++) {
		if (tiles[i].tile_row != i / info->tile_cols ||
		    tiles[i].tile_col != i % info->tile_cols ||
		    !tiles[i].tile_size || tiles[i].tile_offset > payload ||
		    tiles[i].tile_size > payload - tiles[i].tile_offset)
			return -EINVAL;
		if (i && tiles[i].tile_offset <
		    (u64)tiles[i - 1].tile_offset + tiles[i - 1].tile_size)
			return -EINVAL;
	}
	return 0;
}

int avd_av1_pack_fp(struct avd_ctx *ctx, struct avd_frame *f,
		    const struct v4l2_ctrl_av1_sequence *seq,
		    const struct v4l2_ctrl_av1_frame *frame,
		    const struct v4l2_ctrl_av1_tile_group_entry *tiles,
		    const struct v4l2_ctrl_av1_film_grain *grain)
{
	u8 *fp;

	if (ctx->dev->variant->av1_fp_layout != AVD_AV1_FP_BORAGE)
		return -EOPNOTSUPP;
	if (!f->fp || !f->fp->cpu || f->fp->size < BORAGE_FP_SIZE)
		return -ENOSPC;
	fp = f->fp->cpu;
	memset(fp, 0, f->fp->size);
	put_unaligned_le32(f->frame_num, fp + 0x10); /* SPEC 5.2 */
	put_unaligned_le16(frame->tile_info.tile_cols, fp + 0x3e4);
	put_unaligned_le16(frame->tile_info.tile_rows, fp + 0x3e6);
	/* SPEC 6.6 does not locate seq/frame/tiles/grain syntax or surface DVAs.
	 * A native V4L2 struct or Linux instruction stream is not an Apple fp. */
	return -EOPNOTSUPP;
}

int avd_av1_pack_cmd(struct avd_ctx *ctx, struct avd_frame *f)
{
	u8 *cmd = (u8 *)f->cmd;
	const u8 *fp;
	u32 work, base;

	if (ctx->dev->variant->av1_fp_layout != AVD_AV1_FP_BORAGE)
		return -EOPNOTSUPP;
	if (!f->fp || !f->fp->cpu || f->fp->size < BORAGE_FP_SIZE)
		return -ENOSPC;
	fp = f->fp->cpu;
	/* Work index is the modulo-four sequence from the core prefix. */
	work = get_unaligned_le16(cmd + 6);
	if (work >= 4 || !get_unaligned_le16(fp + 0x1a) ||
	    get_unaligned_le16(fp + 0x1a) > BORAGE_WORK_CAP)
		return -EINVAL;
	base = 0x01483118 + 0x1814 * work;
	put_unaligned_le16(get_unaligned_le16(fp + 0x1a), cmd + 0x04);
	put_unaligned_le32(base, cmd + 0x0c);
	put_unaligned_le32(base + 8, cmd + 0x14);
	put_unaligned_le32(0x014832c0 + 0x1814 * work, cmd + 0x24);
	put_unaligned_le32(0x014834dc + 0x1814 * work, cmd + 0x28);
	put_unaligned_le32(base + 0x4d0, cmd + 0x2c);
	/* Slot-relative offsets; their packed DVA encoding is still [C]. */
	put_unaligned_le32(0x5f8, cmd + 0x30);
	put_unaligned_le32(0x4fc, cmd + 0x34);
	put_unaligned_le32(0x5e0, cmd + 0x38);
	put_unaligned_le32(get_unaligned_le32(fp + 0x4f8), cmd + 0x4c);
	return -EOPNOTSUPP;
}
