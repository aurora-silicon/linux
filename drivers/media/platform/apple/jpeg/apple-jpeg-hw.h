/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Apple Silicon JPEG engine: register map and encode job programming.
 *
 * Offsets follow the public m1n1 map (proxyclient/m1n1/hw/jpeg.py, MIT) for
 * the control and 0x1000 blocks. T8140 widens every DMA address to a 64-bit
 * low/high pair, which moves the colour, format and identity registers up.
 */
#ifndef APPLE_JPEG_HW_H
#define APPLE_JPEG_HW_H

#include <linux/align.h>
#include <linux/bits.h>
#include <linux/limits.h>
#include <linux/math.h>
#include <linux/minmax.h>
#include <linux/mm.h>
#include <linux/sizes.h>
#include <linux/types.h>

#define APPLE_JPEG_REG_SIZE		0x4000

#define AJ_MAIN_RUN			0x000
#define AJ_MODE				0x008
#define  AJ_MODE_RESET			0x100
#define  AJ_MODE_SETUP			0x13e
#define  AJ_MODE_IDENTIFY		0x147
#define  AJ_MODE_OPEN			0x17f
#define  AJ_MODE_IDLE			0x743
#define  AJ_MODE_ENCODE			0x74f
#define AJ_DEBUG_STATUS			0x00c
#define AJ_IRQ_MASK			0x020
#define AJ_STATUS			0x024
#define  AJ_STATUS_DONE			BIT(0)
#define  AJ_STATUS_ERRORS		GENMASK(7, 1)
#define AJ_CODEC			0x028
#define  AJ_CODEC_422			1
#define  AJ_CODEC_420			3
#define AJ_CODEC_MODE			0x02c	/* 1 encode */
#define AJ_DECODE_ENABLE		0x034
#define AJ_ENCODE_ENABLE		0x038
#define AJ_PX_USE_PLANE1		0x058
#define AJ_PX_MCUS_W			0x05c
#define AJ_PX_MCUS_H			0x060
#define AJ_PX_PLANE0_WIDTH_M1		0x064
#define AJ_PX_PLANE0_HEIGHT_M1		0x068
#define AJ_PX_PLANE0_TILE_H		0x06c
#define AJ_PX_PLANE0_TILE_V		0x070
#define AJ_PX_PLANE0_STRIDE		0x074
#define AJ_PX_PLANE1_WIDTH_M1		0x078
#define AJ_PX_PLANE1_HEIGHT_M1		0x07c
#define AJ_PX_PLANE1_TILE_H		0x080
#define AJ_PX_PLANE1_TILE_V		0x084
#define AJ_PX_PLANE1_STRIDE		0x088
#define AJ_PIXEL_FORMAT_YUV_PLANES	4
#define AJ_JPEG_IO_FLAGS		0x1000
#define  AJ_IO_FLAGS_ORDER		(BIT(4) | BIT(5))
#define  AJ_IO_FLAGS_422		1
#define  AJ_IO_FLAGS_420		2
#define AJ_COMMAND			0x1004
#define AJ_QTBL_SEL			0x100c
#define  AJ_QTBL_SEL_LUMA_CHROMA	0x14	/* Y: table 0, Cb and Cr: table 1 */
#define AJ_HUFFMAN_TABLE		0x1010
#define  AJ_HUFFMAN_BASELINE		0x3c
#define AJ_RST_INTERVAL			0x1014
#define AJ_JPEG_HEIGHT			0x1018
#define AJ_JPEG_WIDTH			0x101c
#define AJ_COMPRESSED_BYTES		0x1020
#define AJ_OUTPUT_FLAGS			0x1024
#define AJ_INT_STATUS			0x102c
#define  AJ_INT_STATUS_DONE		BIT(31)
#define AJ_BITSTREAM_CORRUPTION		0x1030
#define AJ_QTBL(t)			(0x1100 + 0x40 * (t))

#define AJ_HWREV_MASK			0xffff0fff
/* ADT hw-type 0x101008; its stepping nibble is masked off like T8140's. */
#define AJ_HWREV_T6041			0x100008
#define AJ_HWREV_T8140			0x110000

struct apple_jpeg_write {
	u32 offset;
	u32 value;
};

/* Only the control/geometry and JPEG blocks are common to both layouts. */
struct apple_jpeg_reg_layout {
	u16 input_plane0, input_plane1, input_end;
	u16 pipe_limit0, pipe_limit1;
	u16 output_start0, output_start1, output_end;
	u16 matrix_base;
	u16 encode_format, component_pos_base, convert_cs;
	u16 prefetch_read, prefetch_write, tiling;
	u16 timeout, hwrev, image_width, image_height;
	u16 decode_block_base;
	u8 dma_addr_bits;
	bool reset_complete;
	const struct apple_jpeg_write *defaults;
	u16 num_defaults;
};

struct apple_jpeg_variant {
	const struct apple_jpeg_reg_layout *layout;
	u32 hwrev_mask;
	u32 hwrev;
	u32 int_done_mask;
};

extern const struct apple_jpeg_variant apple_jpeg_t6000;
extern const struct apple_jpeg_variant apple_jpeg_t6041;
extern const struct apple_jpeg_variant apple_jpeg_t8112;
extern const struct apple_jpeg_variant apple_jpeg_t8140;

static inline bool
apple_jpeg_hwrev_supported(const struct apple_jpeg_variant *variant, u32 hwrev)
{
	if (!variant || !variant->hwrev || !hwrev || hwrev == U32_MAX)
		return false;
	return (hwrev & variant->hwrev_mask) == variant->hwrev;
}

struct apple_jpeg_job {
	u32 width;
	u32 height;
	bool is422;		/* NV16: 4:2:2; NV12: 4:2:0 */
	u32 luma_stride;
	u32 chroma_stride;
	u64 luma;
	u64 chroma;
	u64 input_end;		/* inclusive */
	u64 output;
	u32 output_size;
	u32 quality;		/* 1 to 100 */
	u32 restart_interval;
};

/*
 * Raw input: one buffer, the luma rows, then the interleaved CbCr rows at
 * the same stride (V4L2 NV12/NV16). The engine prefetches its input up to
 * AJ_INPUT_END, so both planes must sit in one buffer that ends there: with
 * separate plane buffers it read past the end of whichever plane lies lower
 * and faulted. It also fetches whole MCU rows (16 for 4:2:0, 8 for 4:2:2);
 * the luma overrun lands in the chroma rows, and the buffer carries the
 * chroma rows up to the MCU boundary after the visible ones.
 */
struct apple_jpeg_layout {
	u32 bytesperline;
	u32 chroma_offset;
	u32 sizeimage;
};

static inline void apple_jpeg_raw_layout(u32 width, u32 height, bool is422,
					 struct apple_jpeg_layout *l)
{
	u32 rows = ALIGN(height, is422 ? 8 : 16);

	l->bytesperline = ALIGN(width, 16);
	l->chroma_offset = l->bytesperline * height;
	l->sizeimage = l->chroma_offset + l->bytesperline * (is422 ? rows : rows / 2);
}

/* A kept CAPTURE size above the worst case is honoured up to this. */
#define APPLE_JPEG_CODED_LIMIT		SZ_64M

/*
 * JPEG buffer size: the worst case at quality 100 plus headers, or @keep, a
 * size already agreed with the client, when that is larger (bounded).
 */
static inline u32 apple_jpeg_coded_size(u32 width, u32 height, u32 keep)
{
	u64 need = PAGE_ALIGN((u64)width * height * 3 + SZ_16K);
	u64 limit = max_t(u64, need, APPLE_JPEG_CODED_LIMIT);

	if (keep > need)
		need = min_t(u64, PAGE_ALIGN((u64)keep), limit);
	return need;
}

/* Enough for the default image, the job and both quantisation tables. */
#define APPLE_JPEG_MAX_WRITES		256

void apple_jpeg_quant_tables(u32 quality, u8 luma[64], u8 chroma[64]);
u32 apple_jpeg_timeout(u32 width, u32 height);
int apple_jpeg_defaults(const struct apple_jpeg_reg_layout *layout, bool encode,
			struct apple_jpeg_write *w, unsigned int max);
int apple_jpeg_build_job(const struct apple_jpeg_reg_layout *layout,
			 const struct apple_jpeg_job *job, struct apple_jpeg_write *w,
			 unsigned int max);

#endif /* APPLE_JPEG_HW_H */
