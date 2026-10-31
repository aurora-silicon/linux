// SPDX-License-Identifier: GPL-2.0-only OR MIT
#include <kunit/test.h>
#include <media/v4l2-ctrls.h>

#include "avd-validate.h"

static void avd_h264_capabilities(struct kunit *test)
{
	struct v4l2_ctrl_h264_sps sps = {
		.flags = V4L2_H264_SPS_FLAG_FRAME_MBS_ONLY,
		.pic_width_in_mbs_minus1 = 255,
		.pic_height_in_map_units_minus1 = 255,
		.chroma_format_idc = 1,
	};
	struct v4l2_ctrl_h264_pps pps = {};
	struct v4l2_ctrl_h264_decode_params decode = {};

	KUNIT_EXPECT_NULL(test, avd_validate_h264_sps(&sps));
	sps.pic_width_in_mbs_minus1++;
	KUNIT_EXPECT_NOT_NULL(test, avd_validate_h264_sps(&sps));
	sps.pic_width_in_mbs_minus1--;
	sps.pic_height_in_map_units_minus1++;
	KUNIT_EXPECT_NOT_NULL(test, avd_validate_h264_sps(&sps));
	sps.pic_height_in_map_units_minus1--;
	sps.flags = 0; /* PAFF, even if this particular picture is a frame. */
	KUNIT_EXPECT_NOT_NULL(test, avd_validate_h264_sps(&sps));
	sps.flags = V4L2_H264_SPS_FLAG_FRAME_MBS_ONLY |
		    V4L2_H264_SPS_FLAG_MB_ADAPTIVE_FRAME_FIELD;
	KUNIT_EXPECT_NOT_NULL(test, avd_validate_h264_sps(&sps));
	sps.flags = V4L2_H264_SPS_FLAG_FRAME_MBS_ONLY;
	sps.chroma_format_idc = 3;
	KUNIT_EXPECT_NOT_NULL(test, avd_validate_h264_sps(&sps));
	sps.chroma_format_idc = 2;
	sps.bit_depth_luma_minus8 = 2;
	sps.bit_depth_chroma_minus8 = 2;
	KUNIT_EXPECT_NOT_NULL(test, avd_validate_h264_sps(&sps));
	sps.chroma_format_idc = 1;
	KUNIT_EXPECT_NULL(test, avd_validate_h264_sps(&sps));
	sps.bit_depth_chroma_minus8 = 0;
	KUNIT_EXPECT_NOT_NULL(test, avd_validate_h264_sps(&sps));
	sps.bit_depth_luma_minus8 = 4;
	sps.bit_depth_chroma_minus8 = 4;
	KUNIT_EXPECT_NOT_NULL(test, avd_validate_h264_sps(&sps));
	KUNIT_EXPECT_NULL(test, avd_validate_h264_pps(&pps));
	pps.num_slice_groups_minus1 = 1;
	KUNIT_EXPECT_NOT_NULL(test, avd_validate_h264_pps(&pps));
	KUNIT_EXPECT_NULL(test, avd_validate_h264_decode(&decode));
	decode.flags = V4L2_H264_DECODE_PARAM_FLAG_FIELD_PIC;
	KUNIT_EXPECT_NOT_NULL(test, avd_validate_h264_decode(&decode));
	decode.flags = V4L2_H264_DECODE_PARAM_FLAG_BOTTOM_FIELD;
	KUNIT_EXPECT_NOT_NULL(test, avd_validate_h264_decode(&decode));
}

static void avd_component_formats(struct kunit *test)
{
	struct v4l2_ctrl_hevc_sps hevc = { .chroma_format_idc = 1 };
	struct v4l2_ctrl_av1_sequence av1 = {
		.flags = V4L2_AV1_SEQUENCE_FLAG_SUBSAMPLING_X |
			 V4L2_AV1_SEQUENCE_FLAG_SUBSAMPLING_Y,
		.bit_depth = 8,
	};
	struct v4l2_ctrl_vp9_frame vp9 = {
		.flags = V4L2_VP9_FRAME_FLAG_X_SUBSAMPLING |
			 V4L2_VP9_FRAME_FLAG_Y_SUBSAMPLING,
		.bit_depth = 8,
	};

	KUNIT_EXPECT_NULL(test, avd_validate_hevc_sps(&hevc));
	hevc.chroma_format_idc = 3;
	KUNIT_EXPECT_NOT_NULL(test, avd_validate_hevc_sps(&hevc));
	hevc.chroma_format_idc = 1;
	hevc.bit_depth_luma_minus8 = 4;
	hevc.bit_depth_chroma_minus8 = 4;
	KUNIT_EXPECT_NOT_NULL(test, avd_validate_hevc_sps(&hevc));
	KUNIT_EXPECT_NULL(test, avd_validate_av1_sequence(&av1));
	av1.bit_depth = 10;
	KUNIT_EXPECT_NULL(test, avd_validate_av1_sequence(&av1));
	av1.bit_depth = 12;
	KUNIT_EXPECT_NOT_NULL(test, avd_validate_av1_sequence(&av1));
	av1.bit_depth = 8;
	av1.flags = 0;
	KUNIT_EXPECT_NOT_NULL(test, avd_validate_av1_sequence(&av1));
	KUNIT_EXPECT_NULL(test, avd_validate_vp9(&vp9));
	vp9.bit_depth = 12;
	KUNIT_EXPECT_NOT_NULL(test, avd_validate_vp9(&vp9));
	vp9.bit_depth = 10;
	KUNIT_EXPECT_NULL(test, avd_validate_vp9(&vp9));
	vp9.flags = 0;
	KUNIT_EXPECT_NOT_NULL(test, avd_validate_vp9(&vp9));
}

static void avd_av1_tile_payload(struct kunit *test)
{
	struct v4l2_av1_tile_info ti = { .tile_cols = 2, .tile_rows = 1 };
	struct v4l2_ctrl_av1_tile_group_entry tiles[] = {
		{ .tile_size = 8 },
		{ .tile_col = 1, .tile_offset = 8, .tile_size = 8 },
	};

	KUNIT_EXPECT_NULL(test, avd_validate_av1_tiles(&ti, tiles, 2, 16));
	KUNIT_EXPECT_NOT_NULL(test, avd_validate_av1_tiles(&ti, tiles, 1, 16));
	KUNIT_EXPECT_NOT_NULL(test, avd_validate_av1_tiles(&ti, tiles, 2, 15));
	tiles[1].tile_offset = U32_MAX;
	KUNIT_EXPECT_NOT_NULL(test, avd_validate_av1_tiles(&ti, tiles, 2, 16));
	tiles[1].tile_offset = 8;
	tiles[1].tile_row = V4L2_AV1_MAX_TILE_ROWS;
	KUNIT_EXPECT_NOT_NULL(test, avd_validate_av1_tiles(&ti, tiles, 2, 16));
	tiles[1].tile_row = 0;
	ti.context_update_tile_id = 2;
	KUNIT_EXPECT_NOT_NULL(test, avd_validate_av1_tiles(&ti, tiles, 2, 16));
	ti.context_update_tile_id = 0;
	ti.tile_cols = 0;
	KUNIT_EXPECT_NOT_NULL(test, avd_validate_av1_tiles(&ti, tiles, 2, 16));
}

/* Exercise the public V4L2 type validator, before any driver callback. */
static void avd_v4l2_tile_controls(struct kunit *test)
{
	struct v4l2_ctrl ctrl = { .type = V4L2_CTRL_TYPE_AV1_FRAME, .new_elems = 1 };
	struct v4l2_ctrl_av1_frame *av1 = kunit_kzalloc(test, sizeof(*av1), GFP_KERNEL);
	struct v4l2_ctrl_vp9_frame vp9 = {
		.bit_depth = 8,
		.flags = V4L2_VP9_FRAME_FLAG_X_SUBSAMPLING |
			 V4L2_VP9_FRAME_FLAG_Y_SUBSAMPLING,
	};
	unsigned int cols, rows;
	int ret;

	KUNIT_ASSERT_NOT_NULL(test, av1);
	for (cols = 0; cols <= V4L2_AV1_MAX_TILE_COLS + 1; cols++) {
		for (rows = 0; rows <= V4L2_AV1_MAX_TILE_ROWS + 1; rows++) {
			bool valid = cols && rows && cols <= V4L2_AV1_MAX_TILE_COLS &&
				     rows <= V4L2_AV1_MAX_TILE_ROWS &&
				     cols * rows <= V4L2_AV1_MAX_TILE_COUNT;

			av1->tile_info.tile_cols = cols;
			av1->tile_info.tile_rows = rows;
			av1->tile_info.context_update_tile_id = 0;
			ret = v4l2_ctrl_type_op_validate(&ctrl, v4l2_ctrl_ptr_create(av1));
			KUNIT_EXPECT_EQ(test, ret, valid ? 0 : -EINVAL);
		}
	}
	av1->tile_info.tile_cols = 2;
	av1->tile_info.tile_rows = 2;
	av1->tile_info.context_update_tile_id = 3;
	KUNIT_EXPECT_EQ(test, v4l2_ctrl_type_op_validate(&ctrl, v4l2_ctrl_ptr_create(av1)), 0);
	av1->tile_info.context_update_tile_id = 4;
	ret = v4l2_ctrl_type_op_validate(&ctrl, v4l2_ctrl_ptr_create(av1));
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);
	ctrl.type = V4L2_CTRL_TYPE_VP9_FRAME;
	for (rows = 0; rows <= 255; rows++) {
		vp9.tile_rows_log2 = rows;
		KUNIT_EXPECT_EQ(test, v4l2_ctrl_type_op_validate(&ctrl, v4l2_ctrl_ptr_create(&vp9)),
				rows <= 2 ? 0 : -EINVAL);
	}
}

static struct kunit_case avd_validation_cases[] = {
	KUNIT_CASE(avd_h264_capabilities),
	KUNIT_CASE(avd_component_formats),
	KUNIT_CASE(avd_av1_tile_payload),
	KUNIT_CASE(avd_v4l2_tile_controls),
	{}
};

static struct kunit_suite avd_validation_suite = {
	.name = "apple-avd-validation",
	.test_cases = avd_validation_cases,
};

kunit_test_suite(avd_validation_suite);
