/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _APPLE_AVE_SIZES_H_
#define _APPLE_AVE_SIZES_H_

#include <linux/align.h>
#include <linux/log2.h>
#include <linux/math.h>
#include <linux/sizes.h>
#include <linux/types.h>

struct ave_6070_avc_recon_layout {
	u32 luma_data;
	u32 chroma_data;
	u32 luma_metadata;
	u32 chroma_metadata;
	u32 luma_data_offset;
	u32 chroma_data_offset;
	u32 luma_metadata_offset;
	u32 chroma_metadata_offset;
	u32 bytes;
};

/*
 * Contiguous 4:2:0 reconstruction surface, compressed in
 * 32-pixel luma and 16-pixel chroma tile rows over the macroblock-aligned
 * picture: the luma and chroma data pair first, then the luma and chroma
 * metadata pair. AVC and HEVC use the same geometry; ten-bit HEVC scales
 * only the two data regions.
 */
static inline struct ave_6070_avc_recon_layout
ave_6070_recon_layout(u32 width, u32 height, u32 bit_depth)
{
	struct ave_6070_avc_recon_layout l = { };
	u32 coded_w = ALIGN(width, 16), coded_h = ALIGN(height, 16);
	u32 luma_cols = DIV_ROUND_UP(coded_w, 32);
	u32 luma_rows = DIV_ROUND_UP(coded_h + 4, 32);
	u32 chroma_cols = DIV_ROUND_UP(coded_w / 2, 16);
	u32 chroma_rows = DIV_ROUND_UP(coded_h / 2 + 4, 16);
	u32 data_span;

	if (bit_depth != 10)
		bit_depth = 8;
	l.luma_data = 1024 * luma_cols * luma_rows * bit_depth / 8;
	l.chroma_data = ALIGN(512 * chroma_cols * chroma_rows * bit_depth / 8,
			      128);
	l.luma_metadata = ALIGN(32 * roundup_pow_of_two(luma_cols) *
				roundup_pow_of_two(luma_rows), 128);
	l.chroma_metadata = ALIGN(8 * roundup_pow_of_two(chroma_cols) *
				  roundup_pow_of_two(chroma_rows), 128);

	data_span = ALIGN(l.luma_data + l.chroma_data, 128);
	l.luma_data_offset = 0;
	l.chroma_data_offset = l.luma_data;
	l.luma_metadata_offset = data_span;
	l.chroma_metadata_offset = data_span + l.luma_metadata;
	l.bytes = data_span + ALIGN(l.luma_metadata + l.chroma_metadata, 128);
	return l;
}

static inline struct ave_6070_avc_recon_layout
ave_6070_avc_recon_layout(u32 width, u32 height)
{
	return ave_6070_recon_layout(width, height, 8);
}

static inline u32 ave_6070_transcoded_size(u32 coded_bytes)
{
	return ALIGN(DIV_ROUND_UP(coded_bytes, 2), 4096);
}

/* Start +0x7d0 LFSRef: exact descriptor bytes, before DMA page rounding. */
static inline u32 ave_lfs_ref_size(u32 width, u32 height, bool hevc, u32 scale)
{
	u32 size;

	if (hevc)
		size = 256 * DIV_ROUND_UP(width, 64) * DIV_ROUND_UP(height, 64);
	else
		size = ALIGN(width, 64) * (ALIGN(height, 16) / 4 + 12);
	return size * scale;
}

/* Working-buffer rules used by the split-bank 6070 command layout. */
static inline u32 ave_6070_colocated_size(u32 width, u32 height, bool hevc)
{
	if (hevc)
		return ALIGN(2 * width, 64) *
		       ALIGN(DIV_ROUND_UP(height, 32), 2);

	return ALIGN(8 * width, 128) * DIV_ROUND_UP(height, 16);
}

static inline u32 ave_6070_lowres_result_size(u32 width, u32 height)
{
	return ALIGN(4 * width, 128) * DIV_ROUND_UP(height, 64) + 1024;
}

static inline u32 ave_6070_src_neighbor_info_size(u32 width, u32 height,
						  bool hevc)
{
	u32 raw;

	if (hevc) {
		u32 rows = DIV_ROUND_UP(DIV_ROUND_UP(height, 32), 2);

		raw = 192 * DIV_ROUND_UP(width, 32) * (rows ? rows - 1 : 0);
	} else {
		raw = ALIGN(16 * width, 256);
	}

	return ALIGN(max_t(u32, raw, SZ_16K), SZ_16K);
}

static inline u32 ave_6070_src_neighbor_pixel_size(u32 width, u32 height,
						   bool hevc)
{
	u32 raw;

	if (hevc) {
		u32 rows = DIV_ROUND_UP(DIV_ROUND_UP(height, 32), 2);

		raw = 768 * DIV_ROUND_UP(width, 32) * (rows ? rows - 1 : 0);
	} else {
		raw = ALIGN(64 * width, 1024);
	}

	return ALIGN(max_t(u32, raw, SZ_16K), SZ_16K);
}

/* Active bytes in the transcode neighbour-data channel, before page rounding. */
static inline u32 ave_6070_src_neighbor_data_channel_size(u32 width, bool hevc)
{
	if (hevc)
		return ALIGN(ALIGN(width, 32) / 8, 128);

	return ALIGN(56 * DIV_ROUND_UP(width, 16), 64);
}

static inline u32 ave_6070_src_neighbor_data_size(u32 width, bool hevc)
{
	u32 raw = ave_6070_src_neighbor_data_channel_size(width, hevc);

	return ALIGN(max_t(u32, raw, SZ_16K), SZ_16K);
}

static inline u32 ave_6070_src_neighbor_fw_size(u32 width, bool hevc)
{
	u32 raw = ALIGN((hevc ? 2 : 4) * width, 128);

	return ALIGN(max_t(u32, raw, SZ_16K), SZ_16K);
}

/* Every channel in a 6070 entropy set has the same backing capacity. */
static inline u32 ave_6070_entropy_channel_size(u32 width, u32 height, bool hevc)
{
	if (hevc)
		return 2304 * DIV_ROUND_UP(width, 32) * DIV_ROUND_UP(height, 64);

	return ALIGN(64 * width, 1024) *
	       max_t(u32, 8, DIV_ROUND_UP(height, 64));
}

static inline u32 ave_6070_entropy_aux_size(u32 width, u32 height, bool hevc)
{
	return ave_6070_entropy_channel_size(width, height, hevc);
}

static inline u32 ave_6070_mb_input_size(u32 width, u32 height)
{
	return ALIGN(ALIGN(width, 32) * DIV_ROUND_UP(height, 32), 4096);
}

#endif
