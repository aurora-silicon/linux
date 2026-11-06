/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * AVE motion-compensated temporal filter (MCTF): the camera's temporal noise
 * filter runs as a separate firmware client (work type 3) on the selector-2
 * Start/Process carriers of the encoder firmware. Layouts are keyed by the
 * firmware TEXT CRC. Until a layout is qualified against complete command
 * images, the driver builds nothing and refuses MCTF sessions.
 *
 * Copyright The Aurora Silicon Contributors
 */
#ifndef _APPLE_AVE_MCTF_H_
#define _APPLE_AVE_MCTF_H_

#include <linux/types.h>

#define AVE_MCTF_WORK_TYPE	3
#define AVE_MCTF_STRENGTH_MAX	100

/* A linear NV12 plane pair in a four-plane descriptor: Y at +0x00, UV at +0x20. */
struct ave_mctf_nv12 {
	u32 y, uv;
};

struct ave_mctf_layout {
	u32 text_crc;
	u32 start_bytes, process_bytes;
	u32 work_type, selector;	/* header offsets */
	u32 subtype;			/* Process subtype base, plus the output slot */
	struct ave_mctf_nv12 input, output, reference;
	u32 width, height;		/* input geometry */
	u32 first_frame, output_slot, frame, pts, timescale, strength;
	struct ave_mctf_nv12 history[2];	/* Start: firmware-owned outputs */
	u32 history_y_bytes, history_uv_bytes;	/* at 1920x1080 */
	/* Complete Start/Process constant runs verified against captures. */
	bool qualified;
};

struct ave_mctf_frame {
	u64 in_y, in_uv, out_y, out_uv, ref_y, ref_uv;
	u32 in_y_bytes, in_uv_bytes, out_y_bytes, out_uv_bytes, stride;
	u32 width, height;
	u64 frame, pts, timescale;
	u32 slot;
	s32 strength;	/* 0..100; automatic strength is resolved by the caller */
	bool first;
};

const struct ave_mctf_layout *ave_mctf_layout_for_crc(u32 text_crc);
int ave_mctf_write_process(const struct ave_mctf_layout *l, u8 *cmd,
			   size_t len, const struct ave_mctf_frame *f);

#endif
