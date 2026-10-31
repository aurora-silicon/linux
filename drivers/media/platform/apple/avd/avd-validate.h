/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef AVD_VALIDATE_H_
#define AVD_VALIDATE_H_

#include <linux/stddef.h>
#include <linux/v4l2-controls.h>

/* Return a diagnostic so control rejection and request rejection agree. */
static inline const char *avd_validate_h264_sps(const struct v4l2_ctrl_h264_sps *sps)
{
	if (!(sps->flags & V4L2_H264_SPS_FLAG_FRAME_MBS_ONLY) ||
	    (sps->flags & V4L2_H264_SPS_FLAG_MB_ADAPTIVE_FRAME_FIELD))
		return "H.264 MBAFF/PAFF is unsupported";
	if (sps->pic_width_in_mbs_minus1 >= 256 ||
	    sps->pic_height_in_map_units_minus1 >= 256)
		return "H.264 dimensions exceed 4096x4096";
	if (sps->chroma_format_idc > 2)
		return "H.264 4:4:4 is unsupported";
	if (sps->bit_depth_luma_minus8 != sps->bit_depth_chroma_minus8 ||
	    (sps->bit_depth_luma_minus8 != 0 && sps->bit_depth_luma_minus8 != 2))
		return "H.264 requires matching 8-bit or 10-bit components";
	if (sps->chroma_format_idc == 2 && sps->bit_depth_luma_minus8 == 2)
		return "H.264 10-bit 4:2:2 requires unavailable P210 capture";
	return NULL;
}

static inline const char *avd_validate_h264_pps(const struct v4l2_ctrl_h264_pps *pps)
{
	return pps->num_slice_groups_minus1 ? "H.264 multiple slice groups are unsupported" : NULL;
}

static inline const char *avd_validate_h264_decode(const struct v4l2_ctrl_h264_decode_params *p)
{
	return p->flags & (V4L2_H264_DECODE_PARAM_FLAG_FIELD_PIC |
			   V4L2_H264_DECODE_PARAM_FLAG_BOTTOM_FIELD) ?
		"H.264 field pictures are unsupported" : NULL;
}

static inline const char *avd_validate_hevc_sps(const struct v4l2_ctrl_hevc_sps *sps)
{
	if (sps->chroma_format_idc > 2)
		return "HEVC 4:4:4 is unsupported";
	if (sps->bit_depth_luma_minus8 != sps->bit_depth_chroma_minus8 ||
	    (sps->bit_depth_luma_minus8 != 0 && sps->bit_depth_luma_minus8 != 2))
		return "HEVC requires matching 8-bit or 10-bit components";
	if (sps->chroma_format_idc == 2 && sps->bit_depth_luma_minus8 == 2)
		return "HEVC 10-bit 4:2:2 requires unavailable P210 capture";
	return NULL;
}

static inline const char *avd_validate_vp9(const struct v4l2_ctrl_vp9_frame *f)
{
	if (f->bit_depth != 8 && f->bit_depth != 10)
		return "VP9 requires 8-bit or 10-bit components";
	if (!(f->flags & V4L2_VP9_FRAME_FLAG_X_SUBSAMPLING))
		return "VP9 4:4:4 is unsupported";
	if (!(f->flags & V4L2_VP9_FRAME_FLAG_Y_SUBSAMPLING) && f->bit_depth == 10)
		return "VP9 10-bit 4:2:2 requires unavailable P210 capture";
	if (f->tile_cols_log2 > 6 || f->tile_rows_log2 > 2)
		return "VP9 tile log2 dimensions are invalid";
	return NULL;
}

static inline const char *avd_validate_av1_sequence(const struct v4l2_ctrl_av1_sequence *s)
{
	if (s->bit_depth != 8 && s->bit_depth != 10)
		return "AV1 requires 8-bit or 10-bit components";
	if (s->flags & V4L2_AV1_SEQUENCE_FLAG_MONO_CHROME)
		return "AV1 monochrome is unsupported";
	if (!(s->flags & V4L2_AV1_SEQUENCE_FLAG_SUBSAMPLING_X))
		return "AV1 4:4:4 is unsupported";
	if (!(s->flags & V4L2_AV1_SEQUENCE_FLAG_SUBSAMPLING_Y) && s->bit_depth == 10)
		return "AV1 10-bit 4:2:2 requires unavailable P210 capture";
	return NULL;
}

/* Validate every tile before set_tiles() indexes arrays or emits DMA ranges. */
static inline const char *
avd_validate_av1_tiles(const struct v4l2_av1_tile_info *ti,
		       const struct v4l2_ctrl_av1_tile_group_entry *tiles,
		       unsigned int entries, unsigned int payload)
{
	unsigned int i, count = ti->tile_cols * ti->tile_rows;

	if (!ti->tile_cols || !ti->tile_rows ||
	    ti->tile_cols > V4L2_AV1_MAX_TILE_COLS ||
	    ti->tile_rows > V4L2_AV1_MAX_TILE_ROWS || count > entries ||
	    ti->context_update_tile_id >= count)
		return "AV1 tile grid is invalid";
	for (i = 0; i < count; i++) {
		if (tiles[i].tile_row != i / ti->tile_cols ||
		    tiles[i].tile_col != i % ti->tile_cols)
			return "AV1 tile coordinates do not match the grid";
		if (!tiles[i].tile_size || tiles[i].tile_offset > payload ||
		    tiles[i].tile_size > payload - tiles[i].tile_offset)
			return "AV1 tile data exceeds the OUTPUT payload";
	}
	return NULL;
}

#endif /* AVD_VALIDATE_H_ */
