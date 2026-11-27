/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Apple silicon scaler (MSR23, M5): job builder.
 *
 * A MSR23 job is a register image the engine's firmware streams into the
 * hardware, described by a list of blocks. This builds the image for a
 * conversion from its parameters. The pointers (surfaces, spill buffer) and
 * the frame id are left to the caller.
 */
#ifndef _APPLE_SCALER_MSR23_H_
#define _APPLE_SCALER_MSR23_H_

#include <linux/types.h>
#include <linux/videodev2.h>

/* The image: every block of a job lives in it. */
#define MSR23_IMG_LEN		0x14000

/* Block list record word 2: the firmware tiles the job itself (no tile array). */
#define MSR23_HDR2_FW_TILING	0x72000000

/* Offsets in the image of the blocks the caller writes into. */
#define MSR23_B0		0x80
#define MSR23_B1		0x200
#define MSR23_B14		0x380

/* 64-bit pointers in the image (low word, then high word). */
#define MSR23_PTR_NC0		(MSR23_B0 + 0x50)	/* spill buffer, NC0 pages */
#define MSR23_PTR_NC1		(MSR23_B0 + 0x58)	/* spill buffer after NC0 */
#define MSR23_PTR_SRC(p)	(MSR23_B1 + 0x18 + 8 * (p))
#define MSR23_PTR_DST(p)	(MSR23_B14 + 0x10 + 8 * (p))
/* Block 0 +0x60: the frame id, as in the record's word 1. */
#define MSR23_FRAME_ID		(MSR23_B0 + 0x60)

/*
 * The sizes the builder was checked over on hardware: even, 64 pixels to
 * 4096 wide and 4K UHD in area, at most 4x down or up per axis. The same
 * holds for the crop and compose rectangles.
 */
#define MSR23_MIN_SIZE		64
#define MSR23_MAX_WIDTH		4096
#define MSR23_MAX_PIXELS	(3840 * 2160)
#define MSR23_MAX_RATIO		4

/* NC buffer pages. */
#define MSR23_NC_PAGE		0x4000

/**
 * struct msr23_block - one block of a job
 * @id: block (register group) id
 * @off: offset in the image
 * @size: size word of the block list (size in 32-byte units at bit 20)
 */
struct msr23_block {
	u8 id;
	u32 off;
	u32 size;
};

#define MSR23_NBLOCKS		10
extern const struct msr23_block msr23_blocks[MSR23_NBLOCKS];

/**
 * struct msr23_pic - one side of a job
 * @fourcc: V4L2 pixel format
 * @width: width in pixels
 * @height: height in pixels
 * @stride: bytes per line of plane 0 and of the chroma plane (0 if none)
 */
struct msr23_pic {
	u32 fourcc;
	u32 width, height;
	u32 stride[2];
};

/**
 * struct msr23_job - a conversion
 * @src: source picture
 * @dst: destination picture
 * @crop: the part of the source read (a zero width: all of it)
 * @compose: the part of the destination written (a zero width: all of it);
 *	the rest of the destination is left as it is. The caller adds
 *	&msr23_built.dst_off to the destination pointers.
 * @rotate: clockwise rotation in degrees: 0, 90, 180 or 270
 * @hflip: mirror the result left to right, after the rotation
 * @vflip: mirror the result top to bottom, after the rotation
 * @enc: Y'CbCr encoding of the Y'CbCr side (both sides: the same)
 * @full: that side is full range
 *
 * The rectangles are in pixels, with even origins and sizes.
 */
struct msr23_job {
	struct msr23_pic src, dst;
	struct v4l2_rect crop, compose;
	u16 rotate;
	bool hflip, vflip;
	enum v4l2_ycbcr_encoding enc;
	bool full;
};

/**
 * struct msr23_built - what the builder computed besides the image
 * @nc0: NC0 spill pages (16 KiB each)
 * @nc1: NC1 spill pages
 * @planes: number of planes of the source and of the destination
 * @dst_off: bytes to add to each destination plane's pointer (the compose
 *	rectangle's origin)
 */
struct msr23_built {
	u32 nc0, nc1;
	u8 planes[2];
	u32 dst_off[2];
};

bool msr23_format_ok(u32 fourcc);
int msr23_check(const struct msr23_job *job);
int msr23_build(const struct msr23_job *job, u32 *img, struct msr23_built *out);

#endif
