/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef AVD_H264_FP_H_
#define AVD_H264_FP_H_

#include <linux/types.h>

struct avd_buf;
struct avd_frame;

enum avd_h264_fp_layout {
	AVD_H264_FP_UNKNOWN,
	AVD_H264_FP_V3,
	AVD_H264_FP_BORAGE,
	AVD_H264_FP_T8112_TODO,
	AVD_H264_FP_T8140_TODO,
};

enum avd_h264_fp_field {
	AVD_H264_FP_MV_ABOVE_INFO,
	AVD_H264_FP_MV_COLOR,
	AVD_H264_FP_HDR_D0_REF_HDR,
	AVD_H264_FP_HDR_4C_PIC_SCALING_LIST_DIMS,
	AVD_H264_FP_SCL_46C_PIC_SCALING_MATRIX_4X4,
	AVD_H264_FP_SCL_4CC_PIC_SCALING_MATRIX_8X8,
	AVD_H264_FP_HDR_34_START_HDR,
	AVD_H264_FP_HDR_38_MODE,
	AVD_H264_FP_HDR_3C_HEIGHT_WIDTH,
	AVD_H264_FP_HDR_40_ZERO,
	AVD_H264_FP_HDR_28_HEIGHT_WIDTH_SHIFT3,
	AVD_H264_FP_HDR_2C_SPS_PARAM,
	AVD_H264_FP_HDR_44_FLAGS,
	AVD_H264_FP_HDR_48_CHROMA_QP_INDEX_OFFSET,
	AVD_H264_FP_HDR_58_CONST_3A,
	AVD_H264_FP_HDR_9C_PPS_TILE_ADDR_LSB8,
	AVD_H264_FP_IP_ABOVE,
	AVD_H264_FP_LF_ABOVE,
	AVD_H264_FP_LF_ABOVE_INFO,
	AVD_H264_FP_HDR_210_Y_ADDR_LSB8,
	AVD_H264_FP_HDR_218_WIDTH_ALIGN,
	AVD_H264_FP_HDR_214_UV_ADDR_LSB8,
	AVD_H264_FP_HDR_21C_WIDTH_ALIGN,
	AVD_H264_FP_HDR_54_HEIGHT_WIDTH,
	AVD_H264_FP_SLC_76C_CMD_WEIGHTS_DENOM,
	AVD_H264_FP_SLC_LUMA_WEIGHTS,
	AVD_H264_FP_SLC_LUMA_OFFSETS,
	AVD_H264_FP_SLC_CHROMA_WEIGHTS_0_,
	AVD_H264_FP_SLC_CHROMA_OFFSETS_0_,
	AVD_H264_FP_SLC_CHROMA_WEIGHTS_1_,
	AVD_H264_FP_SLC_CHROMA_OFFSETS_1_,
	AVD_H264_FP_SLC_A7C_CMD_SET_CODED_SLICE,
	AVD_H264_FP_SLC_A84_SLICE_ADDR_LOW,
	AVD_H264_FP_SLC_A88_SLICE_HDR_SIZE,
	AVD_H264_FP_SLC_A70_CMD_QUANT_PARAM,
	AVD_H264_FP_SLC_A74_CMD_DEBLOCKING_FILTER,
	AVD_H264_FP_SLC_6E8_CMD_REF_LIST_0,
	AVD_H264_FP_SLC_6E4_CMD_REF_TYPE,
	AVD_H264_FP_SLC_A78_SPS_TILE_ADDR2_LSB8,
	AVD_H264_FP_NONE,
};

struct avd_h264_fp_cursor {
	struct avd_buf *buf;
	u16 count[AVD_H264_FP_NONE];
	u16 weights;
	u16 offsets;
	u16 slice;
};

int avd_h264_fp_begin(struct avd_h264_fp_cursor *cursor, struct avd_buf *buf,
		      enum avd_h264_fp_layout layout);
int avd_h264_fp_slice(struct avd_h264_fp_cursor *cursor, unsigned int index,
		       unsigned int count);
int avd_h264_fp_emit(struct avd_h264_fp_cursor *cursor,
		     enum avd_h264_fp_field field, u32 word);
int avd_h264_fp_address(struct avd_h264_fp_cursor *cursor,
			enum avd_h264_fp_field field, unsigned int index,
			dma_addr_t addr);
int avd_h264_fp_ref(struct avd_h264_fp_cursor *cursor, bool reference,
		    unsigned int index, unsigned int plane, dma_addr_t addr);
int avd_h264_fp_pack_cmd(enum avd_h264_fp_layout layout, struct avd_frame *f);
/* Known Borage projection only; returns -EOPNOTSUPP for unresolved fields. */
int avd_h264_borage_cmd_fields(struct avd_frame *f);

#endif /* AVD_H264_FP_H_ */
