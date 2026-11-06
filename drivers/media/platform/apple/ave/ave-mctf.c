// SPDX-License-Identifier: GPL-2.0-only
/*
 * AVE MCTF command fields (see ave-mctf.h). Offsets are the derived facts in
 * the selector-2 MCTF ledger; the two 25G83 encoder firmware builds share
 * them but stay separate entries.
 *
 * Copyright The Aurora Silicon Contributors
 */
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/unaligned.h>

#include "ave.h"
#include "ave-mctf.h"

#define AVE_MCTF_25G83(crc)						\
	{								\
		.text_crc = (crc),					\
		.start_bytes = 0x13f28, .process_bytes = 0xb1c0,	\
		.work_type = 0x18, .selector = 0x1c, .subtype = 0x15,	\
		.input = { 0xa300, 0xa320 },				\
		.output = { 0xad80, 0xada0 },				\
		.reference = { 0xae00, 0xae20 },			\
		.width = 0xa3b8, .height = 0xa3bc,			\
		.first_frame = 0x5d90, .output_slot = 0xac84,		\
		.frame = 0xacf8, .pts = 0xad40, .timescale = 0xad48,	\
		.strength = 0xad54,					\
		.history = { { 0x20f8, 0x2118 }, { 0x2138, 0x2158 } },	\
		.history_y_bytes = 0x1fe000,				\
		.history_uv_bytes = 0x0ff000,				\
		.qualified = false,					\
	}

static const struct ave_mctf_layout ave_mctf_layouts[] = {
	AVE_MCTF_25G83(0x69c54454),			/* J700, AppleAVE2FW 9003.78.0 */
	AVE_MCTF_25G83(AVE_FW_TEXT_CRC_T6040_25G83),	/* T6040 */
};

const struct ave_mctf_layout *ave_mctf_layout_for_crc(u32 text_crc)
{
	for (unsigned int i = 0; i < ARRAY_SIZE(ave_mctf_layouts); i++)
		if (ave_mctf_layouts[i].text_crc == text_crc)
			return &ave_mctf_layouts[i];
	return NULL;
}

static void ave_mctf_plane(u8 *cmd, u32 off, u64 addr, u32 bytes, u32 stride)
{
	put_unaligned_le64(addr, cmd + off);
	put_unaligned_le32(bytes, cmd + off + 8);
	put_unaligned_le32(stride, cmd + off + 12);
}

/*
 * Write the per-frame fields into a Process image whose constant runs the
 * caller already filled. Unqualified layouts are refused; the KUnit test
 * exercises the field writer through a qualified copy of a layout.
 */
int ave_mctf_write_process(const struct ave_mctf_layout *l, u8 *cmd,
			   size_t len, const struct ave_mctf_frame *f)
{
	if (!l || !l->qualified)
		return -EOPNOTSUPP;
	if (len != l->process_bytes)
		return -EINVAL;
	if (f->strength < 0 || f->strength > AVE_MCTF_STRENGTH_MAX ||
	    !f->stride || f->stride % 64 || f->stride < f->width ||
	    f->in_y_bytes < (u64)f->stride * f->height ||
	    f->in_uv_bytes < (u64)f->stride * (f->height / 2) ||
	    f->out_y_bytes < f->in_y_bytes || f->out_uv_bytes < f->in_uv_bytes)
		return -EINVAL;

	put_unaligned_le32(AVE_MCTF_WORK_TYPE, cmd + l->work_type);
	put_unaligned_le32(2, cmd + l->selector);
	put_unaligned_le32(l->subtype + f->slot, cmd + 0x20);
	ave_mctf_plane(cmd, l->input.y, f->in_y, f->in_y_bytes, f->stride);
	ave_mctf_plane(cmd, l->input.uv, f->in_uv, f->in_uv_bytes, f->stride);
	ave_mctf_plane(cmd, l->output.y, f->out_y, f->out_y_bytes, f->stride);
	ave_mctf_plane(cmd, l->output.uv, f->out_uv, f->out_uv_bytes, f->stride);
	if (f->ref_y) {
		ave_mctf_plane(cmd, l->reference.y, f->ref_y, f->in_y_bytes, f->stride);
		ave_mctf_plane(cmd, l->reference.uv, f->ref_uv, f->in_uv_bytes, f->stride);
	}
	put_unaligned_le32(f->width, cmd + l->width);
	put_unaligned_le32(f->height, cmd + l->height);
	put_unaligned_le32(f->first, cmd + l->first_frame);
	put_unaligned_le32(f->slot, cmd + l->output_slot);
	put_unaligned_le64(f->frame, cmd + l->frame);
	put_unaligned_le64(f->pts, cmd + l->pts);
	put_unaligned_le64(f->timescale, cmd + l->timescale);
	put_unaligned_le32(f->strength, cmd + l->strength);
	return 0;
}
