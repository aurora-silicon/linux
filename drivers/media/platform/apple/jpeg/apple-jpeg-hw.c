// SPDX-License-Identifier: GPL-2.0-only
/*
 * Apple Silicon JPEG engine: encode job programming, without hardware access
 * so that KUnit covers all of it.
 *
 * Quantisation tables are the ITU-T T.81 Annex K tables scaled with the
 * libjpeg quality formula.
 */
#include <linux/dma-mapping.h>
#include <linux/errno.h>
#include <linux/export.h>
#include <linux/kernel.h>
#include <linux/minmax.h>
#include <linux/module.h>
#include <linux/overflow.h>

#include <kunit/visibility.h>

#include "apple-jpeg-hw.h"

/* T.81 Annex K, tables K.1 and K.2, in natural (row-major) order. */
static const u8 apple_jpeg_annex_k_luma[64] = {
	16, 11, 10, 16, 24, 40, 51, 61,
	12, 12, 14, 19, 26, 58, 60, 55,
	14, 13, 16, 24, 40, 57, 69, 56,
	14, 17, 22, 29, 51, 87, 80, 62,
	18, 22, 37, 56, 68, 109, 103, 77,
	24, 35, 55, 64, 81, 104, 113, 92,
	49, 64, 78, 87, 103, 121, 120, 101,
	72, 92, 95, 98, 112, 100, 103, 99,
};

static const u8 apple_jpeg_annex_k_chroma[64] = {
	17, 18, 24, 47, 99, 99, 99, 99,
	18, 21, 26, 66, 99, 99, 99, 99,
	24, 26, 56, 99, 99, 99, 99, 99,
	47, 66, 99, 99, 99, 99, 99, 99,
	99, 99, 99, 99, 99, 99, 99, 99,
	99, 99, 99, 99, 99, 99, 99, 99,
	99, 99, 99, 99, 99, 99, 99, 99,
	99, 99, 99, 99, 99, 99, 99, 99,
};

void apple_jpeg_quant_tables(u32 quality, u8 luma[64], u8 chroma[64])
{
	u32 q = clamp_t(u32, quality, 1, 100);
	u32 scale = q < 50 ? 5000 / q : 200 - 2 * q;

	for (unsigned int i = 0; i < 64; i++) {
		luma[i] = clamp_t(u32, (apple_jpeg_annex_k_luma[i] * scale + 50) / 100, 1, 255);
		chroma[i] = clamp_t(u32, (apple_jpeg_annex_k_chroma[i] * scale + 50) / 100, 1, 255);
	}
}
EXPORT_SYMBOL_IF_KUNIT(apple_jpeg_quant_tables);

/* Engine cycles allowed per job: three per input byte, within fixed bounds. */
u32 apple_jpeg_timeout(u32 width, u32 height)
{
	u64 t = 3ULL * width * height;

	return clamp_t(u64, t, 0x03000000, 0x7ed6b400);
}
EXPORT_SYMBOL_IF_KUNIT(apple_jpeg_timeout);

static const struct apple_jpeg_write aj_wide64_defaults[] = {
	{ 0x000, 0x0 },
	{ 0x004, 0x0 },
	{ 0x028, 0x0 },
	{ 0x02c, 0x0 },
	{ 0x030, 0x0 },
	{ 0x090, 0x0 },
	{ 0x094, 0x0 },
	{ 0x0a0, 0x0 },
	{ 0x0a4, 0x0 },
	{ 0x0c0, 0x0 },
	{ 0x0c4, 0x0 },
	{ 0x0d0, 0x0 },
	{ 0x0d4, 0x0 },
	{ 0x1c0, 0x0 },
	{ 0x1dc, 0x0 },
	{ 0x288, 0x0 },
	{ 0x28c, 0x0 },
	{ 0x290, 0x0 },
	{ 0x2a4, 0x0 },
	{ 0x2a8, 0x0 },
	{ 0x2ac, 0x0 },
	{ 0x03c, 0x0 },
	{ 0x040, 0x0 },
	{ 0x044, 0x0 },
	{ 0x048, 0x0 },
	{ 0x04c, 0x0 },
	{ 0x050, 0x0 },
	{ 0x054, 0x0 },
	{ 0x058, 0x0 },
	{ 0x0e8, 0x0 },
	{ 0x0ec, 0x0 },
	{ 0x0f0, 0x0 },
	{ 0x0f4, 0x0 },
	{ 0x0f8, 0x0 },
	{ 0x0fc, 0x0 },
	{ 0x100, 0x0 },
	{ 0x104, 0x0 },
	{ 0x108, 0x0 },
	{ 0x10c, 0x0 },
	{ 0x110, 0x0 },
	{ 0x13c, 0x0 },
	{ 0x140, 0x0 },
	{ 0x144, 0x0 },
	{ 0x148, 0x0 },
	{ 0x14c, 0x0 },
	{ 0x150, 0x0 },
	{ 0x154, 0x0 },
	{ 0x158, 0x0 },
	{ 0x16c, 0x0 },
	{ 0x170, 0x0 },
	{ 0x174, 0x0 },
	{ 0x178, 0x0 },
	{ 0x17c, 0x0 },
	{ 0x180, 0x0 },
	{ 0x184, 0x0 },
	{ 0x188, 0x0 },
	{ 0x18c, 0x0 },
	{ 0x190, 0x0 },
	{ 0x194, 0x0 },
	{ 0x198, 0x0 },
	{ 0x1e4, 0x0 },
	{ 0x1e8, 0x0 },
	{ 0x1ec, 0x0 },
	{ 0x1f0, 0x0 },
	{ 0x1f8, 0x0 },
	{ 0x1fc, 0x0 },
	{ 0x200, 0x0 },
	{ 0x204, 0x0 },
	{ 0x208, 0x0 },
	{ 0x20c, 0x0 },
	{ 0x210, 0x0 },
	{ 0x218, 0x0 },
	{ 0x21c, 0x0 },
	{ 0x220, 0x0 },
	{ 0x224, 0x0 },
	{ 0x228, 0x0 },
	{ 0x22c, 0x0 },
	{ 0x230, 0x0 },
	{ 0x234, 0x0 },
	{ 0x238, 0x0 },
	{ 0x23c, 0x0 },
	{ 0x240, 0x0 },
	{ 0x244, 0x0 },
	{ 0x248, 0x0 },
	{ 0x24c, 0x0 },
	{ 0x250, 0x0 },
	{ 0x254, 0x0 },
	{ 0x260, 0x0 },
	{ 0x264, 0x0 },
	{ 0x268, 0x0 },
	{ 0x26c, 0x0 },
	{ 0x270, 0x0 },
	{ 0x274, 0x0 },
	{ 0x278, 0x0 },
	{ 0x27c, 0x0 },
	{ 0x034, 0x1 },
	{ 0x038, 0x1 },
	{ 0x05c, 0x1 },
	{ 0x060, 0x1 },
	{ 0x064, 0x1 },
	{ 0x068, 0x1 },
	{ 0x06c, 0x1 },
	{ 0x070, 0x1 },
	{ 0x074, 0x1 },
	{ 0x078, 0x1 },
	{ 0x07c, 0x1 },
	{ 0x080, 0x1 },
	{ 0x084, 0x1 },
	{ 0x088, 0x1 },
	{ 0x0b0, 0xffffffff },
	{ 0x0b4, 0xf },
	{ 0x0e0, 0xffffffff },
	{ 0x0e4, 0xf },
	{ 0x0b8, 0x1 },
	{ 0x0bc, 0x1 },
	{ 0x280, 0x1f40 },
	{ 0x294, 0x7 },
	{ 0x2b0, 0x7 },
	{ 0x020, 0xff },
};

/* Complete legacy32 reset image; the layout helper appends tiling. */
static const struct apple_jpeg_write aj_legacy32_defaults[] = {
	{ 0x000, 0x0 },
	{ 0x004, 0x0 },
	{ 0x028, 0x0 },
	{ 0x02c, 0x0 },
	{ 0x030, 0x0 },
	{ 0x08c, 0x0 },
	{ 0x090, 0x0 },
	{ 0x0a0, 0x0 },
	{ 0x0a4, 0x0 },
	{ 0x184, 0x0 },
	{ 0x03c, 0x0 },
	{ 0x040, 0x0 },
	{ 0x044, 0x0 },
	{ 0x048, 0x0 },
	{ 0x04c, 0x0 },
	{ 0x050, 0x0 },
	{ 0x054, 0x0 },
	{ 0x058, 0x0 },
	{ 0x0ac, 0x0 },
	{ 0x0b0, 0x0 },
	{ 0x0b4, 0x0 },
	{ 0x0b8, 0x0 },
	{ 0x0bc, 0x0 },
	{ 0x0c0, 0x0 },
	{ 0x0c4, 0x0 },
	{ 0x0c8, 0x0 },
	{ 0x0cc, 0x0 },
	{ 0x0d0, 0x0 },
	{ 0x0d4, 0x0 },
	{ 0x0d8, 0xff },
	{ 0x0dc, 0xff },
	{ 0x0e0, 0xff },
	{ 0x0e4, 0xff },
	{ 0x0e8, 0xff },
	{ 0x0ec, 0xff },
	{ 0x0f0, 0xff },
	{ 0x0f4, 0xff },
	{ 0x0f8, 0xff },
	{ 0x0fc, 0xff },
	{ 0x100, 0x0 },
	{ 0x104, 0x0 },
	{ 0x108, 0x0 },
	{ 0x10c, 0x0 },
	{ 0x110, 0x0 },
	{ 0x114, 0x0 },
	{ 0x118, 0x0 },
	{ 0x11c, 0x0 },
	{ 0x130, 0x0 },
	{ 0x134, 0x0 },
	{ 0x138, 0x0 },
	{ 0x13c, 0x0 },
	{ 0x140, 0x0 },
	{ 0x144, 0x0 },
	{ 0x148, 0x0 },
	{ 0x14c, 0x0 },
	{ 0x150, 0x0 },
	{ 0x154, 0x0 },
	{ 0x158, 0x0 },
	{ 0x15c, 0x0 },
	{ 0x05c, 0x1 },
	{ 0x060, 0x1 },
	{ 0x064, 0x1 },
	{ 0x068, 0x1 },
	{ 0x06c, 0x1 },
	{ 0x070, 0x1 },
	{ 0x074, 0x1 },
	{ 0x078, 0x1 },
	{ 0x07c, 0x1 },
	{ 0x080, 0x1 },
	{ 0x084, 0x1 },
	{ 0x088, 0x1 },
	{ 0x034, 0x1 },
	{ 0x038, 0x1 },
	{ 0x09c, 0xffffffff },
	{ 0x0a8, 0xffffffff },
	{ 0x094, 0x1 },
	{ 0x098, 0x1 },
	{ 0x1fc, 0x0 },
	{ 0x200, 0x0 },
	{ 0x204, 0x0 },
	{ 0x208, 0x0 },
	{ 0x214, 0x0 },
	{ 0x218, 0x0 },
	{ 0x21c, 0x0 },
	{ 0x220, 0x0 },
	{ 0x224, 0x0 },
	{ 0x228, 0x0 },
	{ 0x22c, 0x0 },
	{ 0x230, 0x0 },
	{ 0x234, 0x1f40 },
	{ 0x244, 0x0 },
	{ 0x248, 0x0 },
	{ 0x258, 0x0 },
	{ 0x25c, 0x0 },
	{ 0x23c, 0x0 },
	{ 0x240, 0x0 },
	{ 0x250, 0x0 },
	{ 0x254, 0x0 },
	{ 0x1a0, 0x0 },
	{ 0x1a8, 0x0 },
	{ 0x1ac, 0x0 },
	{ 0x1b0, 0x0 },
	{ 0x1b4, 0x0 },
	{ 0x1bc, 0x0 },
	{ 0x1c0, 0x0 },
	{ 0x1c4, 0x0 },
	{ 0x1c8, 0x0 },
	{ 0x1cc, 0x0 },
	{ 0x1d0, 0x0 },
	{ 0x1d4, 0x0 },
	{ 0x020, 0xff },
};

static const struct apple_jpeg_reg_layout aj_wide64 = {
	.input_plane0 = 0x090,
	.input_plane1 = 0x0a0,
	.input_end = 0x0b0,
	.pipe_limit0 = 0x0b8,
	.pipe_limit1 = 0x0bc,
	.output_start0 = 0x0c0,
	.output_start1 = 0x0d0,
	.output_end = 0x0e0,
	.matrix_base = 0x0e8,
	.encode_format = 0x13c,
	.component_pos_base = 0x140,
	.convert_cs = 0x150,
	.prefetch_read = 0x154,
	.prefetch_write = 0x158,
	.tiling = 0x19c,
	.timeout = 0x1c0,
	.hwrev = 0x1c4,
	.image_width = 0x258,
	.image_height = 0x25c,
	.decode_block_base = 0x16c,
	.dma_addr_bits = 64,
	.reset_complete = true,
	.defaults = aj_wide64_defaults,
	.num_defaults = ARRAY_SIZE(aj_wide64_defaults),
};

/*
 * T6050's reset image, as AppleJPEGDriver writes it between MODE setup and
 * open: T8140's register map with its own default set.
 */
static const struct apple_jpeg_write aj_t6050_defaults[] = {
	{ 0x000, 0x0 },
	{ 0x004, 0x0 },
	{ 0x028, 0x0 },
	{ 0x02c, 0x0 },
	{ 0x034, 0x1 },
	{ 0x038, 0x1 },
	{ 0x1d8, 0x0 },
	{ 0x03c, 0x0 },
	{ 0x040, 0x0 },
	{ 0x044, 0x0 },
	{ 0x048, 0x0 },
	{ 0x04c, 0x0 },
	{ 0x050, 0x0 },
	{ 0x054, 0x0 },
	{ 0x198, 0x0 },
	{ 0x08c, 0x0 },
	{ 0x058, 0x0 },
	{ 0x05c, 0x1 },
	{ 0x060, 0x1 },
	{ 0x06c, 0x1 },
	{ 0x070, 0x1 },
	{ 0x074, 0x1 },
	{ 0x080, 0x1 },
	{ 0x084, 0x1 },
	{ 0x088, 0x1 },
	{ 0x1c8, 0x0 },
	{ 0x1cc, 0x0 },
	{ 0x090, 0x0 },
	{ 0x0a0, 0x0 },
	{ 0x094, 0x0 },
	{ 0x0a4, 0x0 },
	{ 0x0b8, 0x7f },
	{ 0x0bc, 0x7f },
	{ 0x0b0, 0xffffffff },
	{ 0x0b4, 0xf },
	{ 0x0c0, 0x0 },
	{ 0x0d0, 0x0 },
	{ 0x0c4, 0x0 },
	{ 0x0d4, 0x0 },
	{ 0x0e0, 0xffffffff },
	{ 0x0e4, 0xf },
	{ 0x410, 0x0 },
	{ 0x414, 0x0 },
	{ 0x444, 0x0 },
	{ 0x420, 0x0 },
	{ 0x424, 0x0 },
	{ 0x428, 0x0 },
	{ 0x418, 0x0 },
	{ 0x448, 0x0 },
	{ 0x42c, 0x0 },
	{ 0x430, 0x0 },
	{ 0x434, 0x0 },
	{ 0x41c, 0x0 },
	{ 0x44c, 0x0 },
	{ 0x438, 0x0 },
	{ 0x43c, 0x0 },
	{ 0x440, 0x0 },
	{ 0x13c, 0x0 },
	{ 0x400, 0x0 },
	{ 0x404, 0x0 },
	{ 0x140, 0x0 },
	{ 0x144, 0x0 },
	{ 0x148, 0x0 },
	{ 0x14c, 0x0 },
	{ 0x154, 0x0 },
	{ 0x158, 0x0 },
	{ 0x16c, 0x0 },
	{ 0x170, 0x0 },
	{ 0x184, 0x0 },
	{ 0x188, 0x0 },
	{ 0x408, 0x0 },
	{ 0x40c, 0x0 },
	{ 0x18c, 0x0 },
	{ 0x190, 0x0 },
	{ 0x194, 0x0 },
	{ 0x174, 0x0 },
	{ 0x178, 0x0 },
	{ 0x17c, 0x0 },
	{ 0x180, 0x0 },
	{ 0x238, 0x0 },
	{ 0x240, 0x0 },
	{ 0x248, 0x0 },
	{ 0x250, 0x0 },
	{ 0x24c, 0x0 },
	{ 0x254, 0x0 },
	{ 0x23c, 0x0 },
	{ 0x244, 0x0 },
	{ 0x260, 0x0 },
	{ 0x264, 0x0 },
	{ 0x268, 0x0 },
	{ 0x26c, 0x0 },
	{ 0x270, 0x0 },
	{ 0x274, 0x0 },
	{ 0x278, 0x0 },
	{ 0x27c, 0x0 },
	{ 0x280, 0x1eaa },
	{ 0x288, 0x0 },
	{ 0x28c, 0x0 },
	{ 0x2a4, 0x0 },
	{ 0x2a8, 0x0 },
	{ 0x218, 0x0 },
	{ 0x220, 0x0 },
	{ 0x224, 0x0 },
	{ 0x21c, 0x0 },
	{ 0x228, 0x0 },
	{ 0x230, 0x0 },
	{ 0x234, 0x0 },
	{ 0x22c, 0x0 },
	{ 0x1dc, 0x0 },
	{ 0x1e4, 0x0 },
	{ 0x1e8, 0x0 },
	{ 0x1ec, 0x0 },
	{ 0x1f0, 0x0 },
	{ 0x1f8, 0x0 },
	{ 0x1fc, 0x0 },
	{ 0x200, 0x0 },
	{ 0x204, 0x0 },
	{ 0x208, 0x0 },
	{ 0x20c, 0x0 },
	{ 0x210, 0x0 },
	{ 0x1d0, 0x2 },
	{ 0x19c, 0x0 },
	{ 0x1c0, 0x0 },
	{ 0x020, 0xff },
};

/* After the handshake, in AppleJPEGDriver's order; jpeg0 gets 1-8, jpeg1 9-16. */
static const struct apple_jpeg_dma_id aj_t6050_dma_ids[] = {
	{ 0x098, 1, true },
	{ 0x0a8, 2, true },
	{ 0x0b8, 0x7f, false },
	{ 0x0bc, 0x7f, false },
	{ 0x0c8, 3, true },
	{ 0x0d8, 4, true },
	{ 0x294, 8, false },
	{ 0x298, 5, true },
	{ 0x29c, 6, true },
	{ 0x2b0, 8, false },
	{ 0x2b4, 7, true },
	{ 0x2b8, 8, true },
};

static const struct apple_jpeg_reg_layout aj_wide64_t6050 = {
	.input_plane0 = 0x090,
	.input_plane1 = 0x0a0,
	.input_end = 0x0b0,
	.pipe_limit0 = 0x0b8,
	.pipe_limit1 = 0x0bc,
	.output_start0 = 0x0c0,
	.output_start1 = 0x0d0,
	.output_end = 0x0e0,
	.matrix_base = 0x0e8,
	.encode_format = 0x13c,
	.component_pos_base = 0x140,
	.convert_cs = 0x150,
	.prefetch_read = 0x154,
	.prefetch_write = 0x158,
	.tiling = 0x19c,
	.timeout = 0x1c0,
	.hwrev = 0x1c4,
	.image_width = 0x258,
	.image_height = 0x25c,
	.decode_block_base = 0x16c,
	.dma_addr_bits = 64,
	.reset_complete = true,
	.defaults = aj_t6050_defaults,
	.num_defaults = ARRAY_SIZE(aj_t6050_defaults),
	.dma_ids = aj_t6050_dma_ids,
	.num_dma_ids = ARRAY_SIZE(aj_t6050_dma_ids),
	.t6050_job = true,
};

static const struct apple_jpeg_reg_layout aj_legacy32 = {
	.input_plane0 = 0x08c,
	.input_plane1 = 0x090,
	.input_end = 0x09c,
	.pipe_limit0 = 0x094,
	.pipe_limit1 = 0x098,
	.output_start0 = 0x0a0,
	.output_start1 = 0x0a4,
	.output_end = 0x0a8,
	.matrix_base = 0x0ac,
	.encode_format = 0x100,
	.component_pos_base = 0x104,
	.convert_cs = 0x114,
	.prefetch_read = 0x118,
	.prefetch_write = 0x11c,
	.tiling = 0x160,
	.timeout = 0x184,
	.hwrev = 0x188,
	.image_width = 0x20c,
	.image_height = 0x210,
	.decode_block_base = 0x130,
	.dma_addr_bits = 32,
	.reset_complete = true,
	.defaults = aj_legacy32_defaults,
	.num_defaults = ARRAY_SIZE(aj_legacy32_defaults),
};

const struct apple_jpeg_variant apple_jpeg_t6000 = {
	.layout = &aj_legacy32,
	.hwrev_mask = AJ_HWREV_MASK,
	.hwrev = 0x0d0013,
	.int_done_mask = BIT(31),
};
EXPORT_SYMBOL_IF_KUNIT(apple_jpeg_t6000);

const struct apple_jpeg_variant apple_jpeg_t6041 = {
	.layout = &aj_wide64,
	.hwrev_mask = AJ_HWREV_MASK,
	.hwrev = AJ_HWREV_T6041,
	.int_done_mask = BIT(31) | BIT(30),
};
EXPORT_SYMBOL_IF_KUNIT(apple_jpeg_t6041);

const struct apple_jpeg_variant apple_jpeg_t6050 = {
	.layout = &aj_wide64_t6050,
	.hwrev_mask = AJ_HWREV_MASK,
	.hwrev = AJ_HWREV_T6050,
	.int_done_mask = AJ_INT_STATUS_DONE,
};
EXPORT_SYMBOL_IF_KUNIT(apple_jpeg_t6050);

const struct apple_jpeg_variant apple_jpeg_t8112 = {
	.layout = &aj_wide64,
	.hwrev_mask = AJ_HWREV_MASK,
	.hwrev = 0x0e0008,
	.int_done_mask = BIT(31) | BIT(30),
};
EXPORT_SYMBOL_IF_KUNIT(apple_jpeg_t8112);

const struct apple_jpeg_variant apple_jpeg_t8122 = {
	.layout = &aj_wide64,
	.hwrev_mask = AJ_HWREV_MASK,
	.hwrev = AJ_HWREV_T8122,
	.int_done_mask = BIT(31) | BIT(30),
};
EXPORT_SYMBOL_IF_KUNIT(apple_jpeg_t8122);

const struct apple_jpeg_variant apple_jpeg_t8140 = {
	.layout = &aj_wide64,
	.hwrev_mask = AJ_HWREV_MASK,
	.hwrev = AJ_HWREV_T8140,
	.int_done_mask = AJ_INT_STATUS_DONE,
};
EXPORT_SYMBOL_IF_KUNIT(apple_jpeg_t8140);

struct aj_list {
	struct apple_jpeg_write *w;
	unsigned int n, max;
	int error;
};

static void aj_put(struct aj_list *l, u32 offset, u32 value)
{
	if (l->n < l->max)
		l->w[l->n] = (struct apple_jpeg_write){ offset, value };
	l->n++;
}

static void aj_put_addr(struct aj_list *l, const struct apple_jpeg_reg_layout *layout,
			u32 offset, u64 value)
{
	if (layout->dma_addr_bits == 32 && upper_32_bits(value)) {
		l->error = -ERANGE;
		return;
	}
	aj_put(l, offset, lower_32_bits(value));
	if (layout->dma_addr_bits == 64)
		aj_put(l, offset + 4, upper_32_bits(value));
}

/* Tiling is set for a job and clear for a reset: in place, or appended. */
static void aj_defaults(struct aj_list *l, const struct apple_jpeg_reg_layout *layout,
			bool encode)
{
	bool tiling = false;

	for (unsigned int i = 0; i < layout->num_defaults; i++) {
		const struct apple_jpeg_write *d = &layout->defaults[i];

		if (d->offset == layout->tiling) {
			aj_put(l, d->offset, encode);
			tiling = true;
		} else {
			aj_put(l, d->offset, d->value);
		}
	}
	if (!tiling)
		aj_put(l, layout->tiling, encode);
}

/* The reset image, as written between the mode changes of a reset. */
int apple_jpeg_dma_ids(const struct apple_jpeg_reg_layout *layout, u32 stream,
		       struct apple_jpeg_write *w, unsigned int max)
{
	struct aj_list l = { .w = w, .max = max };

	for (unsigned int i = 0; i < layout->num_dma_ids; i++) {
		const struct apple_jpeg_dma_id *id = &layout->dma_ids[i];

		aj_put(&l, id->offset, id->value + (id->per_stream ? 8 * stream : 0));
	}
	return l.error ?: (l.n <= max ? l.n : -ENOSPC);
}
EXPORT_SYMBOL_IF_KUNIT(apple_jpeg_dma_ids);

int apple_jpeg_defaults(const struct apple_jpeg_reg_layout *layout, bool encode,
			struct apple_jpeg_write *w, unsigned int max)
{
	struct aj_list l = { .w = w, .max = max };

	aj_defaults(&l, layout, encode);
	return l.error ?: (l.n <= max ? l.n : -ENOSPC);
}
EXPORT_SYMBOL_IF_KUNIT(apple_jpeg_defaults);

/*
 * The T6050 sequence, in AppleJPEGDriver's order (per the macOS register
 * trace). Unlike the earlier revisions it has no plane size or pipe limit
 * registers, takes 16-bit quantisation tables and two-bit Huffman table
 * selectors, and needs 0x08c, 0x334 and IO flag bit 7.
 *
 * 4:2:0 is the traced sequence. The other subsamplings come from running
 * AppleJPEGDriver's own encode setup (27.2.1, AppleJPEGWrapperControl::
 * encoderHWSetup with the V14 wrapper the J815's hw-type 0x110010 selects)
 * under an emulator on a register page: it reproduces the traced 4:2:0
 * sequence in order and value for every non-table word, and for the kext's
 * 444v/422v/L008 inputs it differs only in the words in this table and in
 * plane 1 use, which the kext clears for grey inputs. The codec and the IO
 * flags' subsampling code come as one pair from AppleJPEGDriver::startEncoder,
 * which maps the request's subsampling to (codec, code): 4:2:0 (3, 2),
 * 4:2:2 (1, 1), 4:4:4 (0, 0), 4:1:1 (2, 6), grey (4, 3).
 */
static const struct {
	u8 codec;	/* AJ_CODEC: the kext's 0 4:4:4, 1 4:2:2, 3 4:2:0, 4 grey */
	u8 source;	/* AJ_T6050_PX_MODE: the input's chroma (L008 counts as 4:4:4) */
	u8 io;		/* JPEG subsampling in AJ_JPEG_IO_FLAGS */
	u8 mcu_w, mcu_h;
	u8 tile0_h, tile0_v, tile1_h;	/* plane 1 tile height is 8 when used */
	u8 enc_ctrl;
} aj_t6050_chroma[] = {
	[AJ_CHROMA_420] = { 3, 3, 2, 16, 16, 2, 16, 2, AJ_T6050_ENC_CTRL_ENCODE },
	[AJ_CHROMA_422] = { 1, 1, 1, 16, 8, 2, 8, 2, 0x28 },
	[AJ_CHROMA_444] = { 0, 0, 0, 8, 8, 1, 8, 2, 0x20 },
	[AJ_CHROMA_GRAY] = { 4, 0, 3, 8, 8, 1, 8, 0, 0x20 },
};

static int aj_build_t6050(struct aj_list *l, const struct apple_jpeg_reg_layout *layout,
			  const struct apple_jpeg_job *job, u64 output_end)
{
	const struct apple_jpeg_write addr[] = {
		{ layout->input_plane0, lower_32_bits(job->luma) },
		{ layout->input_plane1, lower_32_bits(job->chroma) },
		{ layout->input_end, lower_32_bits(job->input_end | 7) },
		{ layout->output_start0, lower_32_bits(job->output) },
		{ layout->output_start1, lower_32_bits(job->output) },
		{ layout->output_end, lower_32_bits(output_end) },
	};
	const u64 high[] = {
		job->luma, job->chroma, job->input_end, job->output, job->output, output_end,
	};
	u32 width = job->width, height = job->height;
	const typeof(aj_t6050_chroma[0]) *c;
	u8 q[2][64];

	if (job->subsampling >= ARRAY_SIZE(aj_t6050_chroma))
		return -EINVAL;
	c = &aj_t6050_chroma[job->subsampling];

	aj_put(l, AJ_MODE, AJ_MODE_SETUP);
	aj_put(l, AJ_MAIN_RUN, 0);
	aj_defaults(l, layout, true);
	aj_put(l, AJ_MODE, AJ_MODE_OPEN);

	aj_put(l, AJ_ENCODE_ENABLE, 1);
	aj_put(l, AJ_CODEC_MODE, 1);
	aj_put(l, AJ_DECODE_ENABLE, 0);
	aj_put(l, AJ_CODEC, c->codec);
	aj_put(l, layout->image_width, width);
	aj_put(l, layout->image_height, height);
	aj_put(l, AJ_T6050_PX_MODE, c->source);
	/* Grey has no plane 1 (the kext clears it for L008/gray inputs). */
	aj_put(l, AJ_PX_USE_PLANE1, job->subsampling != AJ_CHROMA_GRAY);
	aj_put(l, layout->timeout, apple_jpeg_timeout(width, height));
	aj_put(l, AJ_PX_MCUS_W, DIV_ROUND_UP(width, c->mcu_w));
	aj_put(l, AJ_PX_MCUS_H, DIV_ROUND_UP(height, c->mcu_h));
	aj_put(l, AJ_PX_PLANE0_TILE_H, c->tile0_h);
	aj_put(l, AJ_PX_PLANE0_TILE_V, c->tile0_v);
	if (c->tile1_h) {
		aj_put(l, AJ_PX_PLANE1_TILE_H, c->tile1_h);
		aj_put(l, AJ_PX_PLANE1_TILE_V, 8);
	}
	aj_put(l, AJ_T6050_ENC_CTRL, c->enc_ctrl);

	/* Two planes, Y then interleaved Cb, Cr. */
	aj_put(l, layout->encode_format, AJ_PIXEL_FORMAT_YUV_PLANES);
	aj_put(l, layout->component_pos_base, 0);
	aj_put(l, layout->component_pos_base + 4, 0);
	aj_put(l, layout->component_pos_base + 8, 1);

	/* Every low address word, then every high one. */
	for (unsigned int i = 0; i < ARRAY_SIZE(addr); i++)
		aj_put(l, addr[i].offset, addr[i].value);
	for (unsigned int i = 0; i < ARRAY_SIZE(addr); i++)
		aj_put(l, addr[i].offset + 4, upper_32_bits(high[i]));
	aj_put(l, AJ_PX_PLANE0_STRIDE, job->luma_stride);
	aj_put(l, AJ_PX_PLANE1_STRIDE, job->chroma_stride);
	aj_put(l, layout->prefetch_read, 1);
	aj_put(l, layout->prefetch_write, 0);
	aj_put(l, AJ_T6050_PRE_ENCODE, 0);

	aj_put(l, AJ_MODE, AJ_MODE_ENCODE);
	aj_put(l, AJ_JPEG_IO_FLAGS, AJ_IO_FLAGS_T6050 | AJ_IO_FLAGS_ORDER | c->io);
	aj_put(l, AJ_JPEG_WIDTH, width);
	aj_put(l, AJ_JPEG_HEIGHT, height);
	aj_put(l, AJ_RST_INTERVAL, job->restart_interval);
	aj_put(l, AJ_OUTPUT_FLAGS, 0);

	apple_jpeg_quant_tables(job->quality, q[0], q[1]);
	for (unsigned int t = 0; t < 2; t++)
		for (unsigned int i = 0; i < 32; i++)
			aj_put(l, AJ_QTBL16(t) + 4 * i, q[t][2 * i] << 16 | q[t][2 * i + 1]);
	aj_put(l, AJ_HUFFMAN_TABLE, AJ_HUFFMAN_T6050);
	aj_put(l, AJ_QTBL_SEL, AJ_QTBL_SEL_LUMA_CHROMA);

	aj_put(l, AJ_MAIN_RUN, 1);
	aj_put(l, AJ_COMMAND, 1);
	return 0;
}

/*
 * Every register write of one baseline encode of two-plane (NV12 or NV16;
 * on T6050 also NV24) or one-plane (GREY, T6050)
 * full-range input, in order, ending with the start command.
 */
int apple_jpeg_build_job(const struct apple_jpeg_reg_layout *layout,
			 const struct apple_jpeg_job *job, struct apple_jpeg_write *w,
			 unsigned int max)
{
	u32 width = job->width, height = job->height;
	bool is422 = job->subsampling == AJ_CHROMA_422;
	u32 mcu_h = is422 ? 8 : 16;
	u32 crows = is422 ? height : DIV_ROUND_UP(height, 2);
	struct aj_list l = { .w = w, .max = max };
	u8 luma[64], chroma[64];
	u64 output_end, addr_mask = DMA_BIT_MASK(layout->dma_addr_bits);
	int ret;

	if (!width || !height || width > U16_MAX || height > U16_MAX)
		return -EINVAL;
	if (job->luma_stride < width ||
	    job->chroma_stride < (job->subsampling == AJ_CHROMA_444 ? 2 * width :
				  job->subsampling == AJ_CHROMA_GRAY ? 0 : ALIGN(width, 2)))
		return -EINVAL;
	if ((job->luma | job->chroma) & 3 || job->output & 31 || !job->output_size)
		return -EINVAL;
	if (job->input_end < job->luma || job->input_end < job->chroma ||
	    job->restart_interval > U16_MAX)
		return -EINVAL;

	/* Validate every inclusive boundary before emitting any address word. */
	if (check_add_overflow(job->output, (u64)job->output_size - 1, &output_end) ||
	    ((job->luma | job->chroma | (job->input_end | 7) | job->output |
	      output_end) & ~addr_mask))
		return -ERANGE;

	if (layout->t6050_job) {
		ret = aj_build_t6050(&l, layout, job, output_end);
		if (ret)
			return ret;
		return l.error ?: (l.n <= max ? l.n : -ENOSPC);
	}

	if (job->subsampling != AJ_CHROMA_420 && job->subsampling != AJ_CHROMA_422)
		return -EOPNOTSUPP;

	aj_put(&l, AJ_MODE, AJ_MODE_SETUP);
	aj_put(&l, AJ_MAIN_RUN, 0);
	aj_defaults(&l, layout, true);
	aj_put(&l, AJ_MODE, AJ_MODE_OPEN);

	aj_put(&l, AJ_ENCODE_ENABLE, 1);
	aj_put(&l, AJ_CODEC_MODE, 1);
	aj_put(&l, AJ_DECODE_ENABLE, 0);
	aj_put(&l, AJ_CODEC, is422 ? AJ_CODEC_422 : AJ_CODEC_420);

	aj_put(&l, AJ_PX_USE_PLANE1, 1);
	aj_put(&l, AJ_PX_MCUS_W, DIV_ROUND_UP(width, 16));
	aj_put(&l, AJ_PX_MCUS_H, DIV_ROUND_UP(height, mcu_h));
	aj_put(&l, AJ_PX_PLANE0_WIDTH_M1, width - 1);
	aj_put(&l, AJ_PX_PLANE0_HEIGHT_M1, height - 1);
	aj_put(&l, AJ_PX_PLANE0_TILE_H, 2);
	aj_put(&l, AJ_PX_PLANE0_TILE_V, mcu_h);
	aj_put(&l, AJ_PX_PLANE0_STRIDE, job->luma_stride);
	aj_put(&l, AJ_PX_PLANE1_WIDTH_M1, ALIGN(width, 2) - 1);
	aj_put(&l, AJ_PX_PLANE1_HEIGHT_M1, crows - 1);
	aj_put(&l, AJ_PX_PLANE1_TILE_H, 2);
	aj_put(&l, AJ_PX_PLANE1_TILE_V, 8);
	aj_put(&l, AJ_PX_PLANE1_STRIDE, job->chroma_stride);
	aj_put(&l, layout->image_width, width);
	aj_put(&l, layout->image_height, height);

	aj_put_addr(&l, layout, layout->input_plane0, job->luma);
	aj_put_addr(&l, layout, layout->input_plane1, job->chroma);
	/* The engine treats the end as inclusive of a whole 8-byte word. */
	aj_put_addr(&l, layout, layout->input_end, job->input_end | 7);
	aj_put_addr(&l, layout, layout->output_start0, job->output);
	aj_put_addr(&l, layout, layout->output_start1, job->output);
	aj_put_addr(&l, layout, layout->output_end, output_end);
	aj_put(&l, layout->pipe_limit0, is422 ? 8 : 3);
	aj_put(&l, layout->pipe_limit1, is422 ? 2 : 1);

	/* Two planes, Y then interleaved Cb, Cr; no colour conversion. */
	aj_put(&l, layout->encode_format, AJ_PIXEL_FORMAT_YUV_PLANES);
	aj_put(&l, layout->component_pos_base, 0);
	aj_put(&l, layout->component_pos_base + 4, 0);
	aj_put(&l, layout->component_pos_base + 8, 1);
	aj_put(&l, layout->component_pos_base + 12, 0);
	aj_put(&l, layout->convert_cs, 0);

	aj_put(&l, layout->timeout, apple_jpeg_timeout(width, height));
	aj_put(&l, layout->prefetch_read, 1);
	aj_put(&l, layout->prefetch_write, 0);

	aj_put(&l, AJ_MODE, AJ_MODE_ENCODE);
	aj_put(&l, AJ_JPEG_IO_FLAGS, AJ_IO_FLAGS_ORDER |
	       (is422 ? AJ_IO_FLAGS_422 : AJ_IO_FLAGS_420));
	aj_put(&l, AJ_JPEG_WIDTH, width);
	aj_put(&l, AJ_JPEG_HEIGHT, height);
	aj_put(&l, AJ_RST_INTERVAL, job->restart_interval);
	aj_put(&l, AJ_OUTPUT_FLAGS, 0);

	apple_jpeg_quant_tables(job->quality, luma, chroma);
	for (unsigned int i = 0; i < 16; i++) {
		aj_put(&l, AJ_QTBL(0) + 4 * i, luma[4 * i] << 24 | luma[4 * i + 1] << 16 |
		       luma[4 * i + 2] << 8 | luma[4 * i + 3]);
		aj_put(&l, AJ_QTBL(1) + 4 * i, chroma[4 * i] << 24 | chroma[4 * i + 1] << 16 |
		       chroma[4 * i + 2] << 8 | chroma[4 * i + 3]);
	}
	aj_put(&l, AJ_QTBL_SEL, AJ_QTBL_SEL_LUMA_CHROMA);
	aj_put(&l, AJ_HUFFMAN_TABLE, AJ_HUFFMAN_BASELINE);

	aj_put(&l, AJ_MAIN_RUN, 1);
	aj_put(&l, AJ_COMMAND, 1);
	return l.error ?: (l.n <= max ? l.n : -ENOSPC);
}
EXPORT_SYMBOL_IF_KUNIT(apple_jpeg_build_job);
