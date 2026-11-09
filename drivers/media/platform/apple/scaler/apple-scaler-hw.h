/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Apple silicon scaler/CSC (MSR): pipeline field encodings.
 *
 * These helpers compute register values from V4L2 state. They touch no
 * hardware, so KUnit checks them against the values the engine is known
 * to accept.
 */
#ifndef APPLE_SCALER_HW_H
#define APPLE_SCALER_HW_H

#include <linux/types.h>
#include <linux/videodev2.h>

/* Colour-space converter block. */
#define ASC_CSC_CTRL			0x3800
#define   ASC_CSC_CTRL_BYPASS		0x000e0000
#define   ASC_CSC_CTRL_ENABLE		0x000e0003
#define ASC_CSC_MODE			0x3804
#define   ASC_CSC_MODE_DEFAULT		0xc
#define ASC_CSC_PRE_OFFSET(i)		(0x3808 + 4 * (i))
#define ASC_CSC_COEF(row, col)		(0x3814 + 4 * (3 * (row) + (col)))
#define ASC_CSC_POST_OFFSET(i)		(0x3838 + 4 * (i))

/* Matrix coefficients are signed Q3.12. */
#define ASC_CSC_COEF_SHIFT		12
/* Offsets are signed fractions of full scale: 1.0 = 1 << 24, 26-bit field. */
#define ASC_CSC_OFFSET_SHIFT		24
#define ASC_CSC_OFFSET_MASK		GENMASK(25, 0)

/* Scale ratios are unsigned Q22. */
#define ASC_RATIO_SHIFT			22

/**
 * struct asc_csc - one colour conversion, in register encoding
 * @coef: 3x3 matrix, row = output channel, column = input channel
 * @pre: offsets added to the input channels before the matrix
 * @post: offsets added to the output channels after the matrix
 */
struct asc_csc {
	s16 coef[3][3];
	s32 pre[3];
	s32 post[3];
};

/**
 * struct asc_format - a pixel format the engine reads and writes
 * @fourcc: V4L2 pixel format
 * @layout: layout class (source 0x180 / destination 0x280)
 * @code: format code (source 0x184; destination 0x284 without bit 15)
 * @sampling: subsampling descriptor (source 0x188 / destination 0x288 without bit 15)
 * @swizzle: component order (source 0x1f8 / destination 0x2d8)
 * @planes: colour planes (each needs its own DMA address)
 * @bpp: bytes per sample position of each plane, at that plane's resolution
 * @hsub: horizontal chroma subsampling of plane 1
 * @vsub: vertical chroma subsampling of plane 1
 * @yuv: Y'CbCr format (needs the CSC to pair with RGB)
 * @alpha: carries an alpha channel
 * @wide: samples are 16 bits in memory (more than 8 significant bits)
 */
struct asc_format {
	u32 fourcc;
	u32 layout;
	u32 code;
	u32 sampling;
	u32 swizzle;
	u8 planes;
	u8 bpp[2];
	u8 hsub;
	u8 vsub;
	bool yuv;
	bool alpha;
	bool wide;
};

/* One register write: byte offset in the MSR aperture and value. */
struct asc_reg {
	u32 off;
	u32 val;
};

#define ASC_MAX_PLANES		2
#define ASC_JOB_MAX_WRITES	224

/**
 * struct asc_image - one side of a job
 * @fmt: pixel format
 * @width: width in pixels
 * @height: height in pixels
 * @dva: DMA address of each colour plane
 * @stride: bytes per line of each colour plane
 */
struct asc_image {
	const struct asc_format *fmt;
	u32 width;
	u32 height;
	u64 dva[ASC_MAX_PLANES];
	u32 stride[ASC_MAX_PLANES];
};

/**
 * struct asc_job - a complete conversion
 * @src: image read
 * @dst: image written
 * @csc: colour conversion, or NULL to pass components through
 * @transform_id: tag the engine carries for this job
 */
struct asc_job {
	struct asc_image src;
	struct asc_image dst;
	const struct asc_csc *csc;
	u32 transform_id;
	bool msr20;	/* engine has the MSR20-generation control block */
};

int asc_job_build(const struct asc_job *job, struct asc_reg *w, unsigned int max);

const struct asc_format *asc_find_format(u32 fourcc);
const struct asc_format *asc_format_by_index(unsigned int index);

int asc_csc_compute(enum v4l2_ycbcr_encoding enc,
		    enum v4l2_quantization quant, bool yuv_to_rgb,
		    struct asc_csc *csc);
u32 asc_csc_offset_reg(s32 offset);

u32 asc_scale_ratio(u32 in, u32 out);

#endif /* APPLE_SCALER_HW_H */
