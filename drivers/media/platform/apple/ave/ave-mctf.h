/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * AVE motion-compensated temporal filter (MCTF): the camera's temporal noise
 * filter runs as a separate firmware client (work type 3) on the selector-2
 * Start/Process carriers of the encoder firmware. Layouts are keyed by the
 * firmware TEXT CRC; only a layout qualified against captured command images
 * is ever sent, every other build refuses MCTF sessions.
 *
 * Copyright The Aurora Silicon Contributors
 */
#ifndef _APPLE_AVE_MCTF_H_
#define _APPLE_AVE_MCTF_H_

#include <linux/types.h>
#include <linux/videodev2.h>

#define AVE_MCTF_WORK_TYPE	3
#define AVE_MCTF_WIDTH		1920
#define AVE_MCTF_HEIGHT		1080
#define AVE_MCTF_SLOTS		2

/*
 * Filter level, written raw to every Process command. macOS uses 16 for the
 * first frames of a session, then 12 in room light and 9 in the dark.
 */
#define V4L2_CID_APPLE_AVE_MCTF_LEVEL	(V4L2_CID_USER_BASE + 0x1240)
#define AVE_MCTF_LEVEL_MAX	31
#define AVE_MCTF_LEVEL_DEFAULT	16

/* One constant u32 word of a command image */
struct ave_mctf_word {
	u32 off, val;
};

/*
 * A Start surface: a driver-allocated, zeroed, device-only buffer of @alloc
 * bytes whose u64 address goes to @off. Its size word, when the firmware
 * has one, is a constant word. An alias row (@alloc zero) repeats the
 * address of the surface at Start offset @alias.
 */
struct ave_mctf_surface {
	u32 off, alloc, alias;
};

/* Process u64 address taken from the Start surface at @start + @stride * slot */
struct ave_mctf_slot_ref {
	u32 off, start, stride;
};

/* Linear NV12 plane descriptor: u64 address, u32 bytes, u32 line stride */
struct ave_mctf_nv12 {
	u32 y, uv;
};

struct ave_mctf_layout {
	u32 text_crc;
	u32 start_bytes, process_bytes;
	/* Header: work type, then the +0x24 and +0x28 arguments */
	u32 unit, arg1, frame_arg0;
	u32 subtype;	/* Process subtype base, plus the slot */
	u32 slots;
	const struct ave_mctf_word *start_words, *process_words;
	unsigned int n_start_words, n_process_words;
	const struct ave_mctf_surface *surfaces;
	unsigned int n_surfaces;
	struct ave_mctf_slot_ref slot_refs[2];
	struct ave_mctf_nv12 input, output;
	u32 width, height;	/* input geometry */
	u32 first_frame, first_value, next_value;
	u32 slot, frame, level;
	/* Start and Process images match captures of this build at 1080p. */
	bool qualified;
};

struct ave_mctf_frame {
	dma_addr_t in, out;	/* NV12, chroma right after the luma lines */
	u32 stride, width, height;
	u32 slot, frame, level;
};

const struct ave_mctf_layout *ave_mctf_layout_for_crc(u32 text_crc);
const struct ave_mctf_layout *ave_mctf_qualified(u32 text_crc);
int ave_mctf_build_start(const struct ave_mctf_layout *l, u8 *cmd, size_t len,
			 const dma_addr_t *surf);
int ave_mctf_build_process(const struct ave_mctf_layout *l, u8 *cmd,
			   size_t len, const dma_addr_t *surf,
			   const struct ave_mctf_frame *f);

#endif
