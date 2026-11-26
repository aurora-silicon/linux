/* SPDX-License-Identifier: GPL-2.0 */
#ifndef AVD_H264_PARAMS_H
#define AVD_H264_PARAMS_H

#include <linux/v4l2-controls.h>

/* The reference headers contain only VALID entries, in DPB array order. */
static inline unsigned int
avd_h264_ref_slot(const struct v4l2_ctrl_h264_decode_params *decode,
		  unsigned int index)
{
	unsigned int slot = 0;

	for (unsigned int i = 0; i < index; i++)
		if (decode->dpb[i].flags & V4L2_H264_DPB_ENTRY_FLAG_VALID)
			slot++;

	return slot;
}

static inline unsigned int
avd_h264_num_refs(const struct v4l2_ctrl_h264_decode_params *decode)
{
	return avd_h264_ref_slot(decode, V4L2_H264_NUM_DPB_ENTRIES);
}

/* SP/SI require switching-picture state which the instruction builder lacks. */
static inline bool
avd_h264_slice_type_supported(const struct v4l2_ctrl_h264_slice_params *sl)
{
	return sl->slice_type == V4L2_H264_SLICE_TYPE_I ||
	       sl->slice_type == V4L2_H264_SLICE_TYPE_P ||
	       sl->slice_type == V4L2_H264_SLICE_TYPE_B;
}

/*
 * The picture a slice belongs to, in macroblocks (H.264 7.4.2.1.1 and the
 * field_pic_flag semantics in 7.4.3): PicWidthInMbs, and PicSizeInMbs =
 * PicWidthInMbs * FrameHeightInMbs / (1 + field_pic_flag), with
 * FrameHeightInMbs = (2 - frame_mbs_only_flag) * PicHeightInMapUnits.
 */
struct avd_h264_pic_size {
	u32 width_mbs;
	u64 mbs;
};

static inline struct avd_h264_pic_size
avd_h264_pic_size(const struct v4l2_ctrl_h264_sps *sps,
		  const struct v4l2_ctrl_h264_decode_params *decode)
{
	bool field = decode->flags & V4L2_H264_DECODE_PARAM_FLAG_FIELD_PIC;
	u32 frame_height = (sps->flags & V4L2_H264_SPS_FLAG_FRAME_MBS_ONLY ? 1 : 2) *
			   ((u32)sps->pic_height_in_map_units_minus1 + 1);
	struct avd_h264_pic_size pic = {
		.width_mbs = (u32)sps->pic_width_in_mbs_minus1 + 1,
	};

	pic.mbs = (u64)pic.width_mbs * (frame_height / (1 + field));
	return pic;
}

static inline bool avd_h264_same_pic_size(const struct avd_h264_pic_size *a,
					  const struct avd_h264_pic_size *b)
{
	return a->width_mbs == b->width_mbs && a->mbs == b->mbs;
}

/*
 * H.264 7.4.3: first_mb_in_slice * (1 + MbaffFrameFlag) is less than
 * PicSizeInMbs. The slice start is programmed as a macroblock column and
 * row of @pic, so a slice outside it would send the decoder outside the
 * picture's buffers.
 */
static inline bool
avd_h264_slice_in_picture(const struct avd_h264_pic_size *pic,
			  const struct v4l2_ctrl_h264_sps *sps,
			  const struct v4l2_ctrl_h264_decode_params *decode,
			  u32 first_mb)
{
	bool mbaff = (sps->flags & V4L2_H264_SPS_FLAG_MB_ADAPTIVE_FRAME_FIELD) &&
		     !(decode->flags & V4L2_H264_DECODE_PARAM_FLAG_FIELD_PIC);

	return ((u64)first_mb << mbaff) < pic->mbs;
}

/* Pictures are held until their final slice; ASO is not implemented. */
static inline bool avd_h264_slice_order_supported(unsigned int held,
						  unsigned int last_mb,
						  unsigned int first_mb)
{
	return held ? first_mb > last_mb : first_mb == 0;
}

#endif /* AVD_H264_PARAMS_H */
