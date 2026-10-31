/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef AVD_HEVC_FP_H_
#define AVD_HEVC_FP_H_

#ifdef AVD_HEVC_HOST_TEST
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <errno.h>
#include <string.h>
#include <linux/v4l2-controls.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
#else
#include <linux/types.h>
#include <linux/v4l2-controls.h>
#endif

/* Selected by OF match data, never inferred from the firmware file name. */
enum avd_hevc_fp_layout_id {
	AVD_HEVC_FP_UNKNOWN,
	AVD_HEVC_FP_V3,
	AVD_HEVC_FP_BORAGE,
	AVD_HEVC_FP_T8112_TODO,
	AVD_HEVC_FP_T8140_TODO,
};

struct avd_hevc_slice_range {
	u32 offset;
	u32 size;
};

int avd_hevc_slice_ranges(const struct v4l2_ctrl_hevc_slice_params *sl,
			 unsigned int count, u32 payload,
			 struct avd_hevc_slice_range *ranges);

/* These names describe the licensed V3 layout only. */
enum avd_hevc_fp_field {
	HEVC_FP_NONE,
	HEVC_FP_DIMS8, HEVC_FP_TXFM, HEVC_FP_PCM, HEVC_FP_FLAGS,
	HEVC_FP_START, HEVC_FP_MODE, HEVC_FP_DIMS, HEVC_FP_PIXFMT,
	HEVC_FP_PPS_FLAGS, HEVC_FP_PPS_QP, HEVC_FP_SCALING_DIMS,
	HEVC_FP_DC16, HEVC_FP_DC32,
	HEVC_FP_SCALING4, HEVC_FP_SCALING8,
	HEVC_FP_SCALING16, HEVC_FP_SCALING32,
	HEVC_FP_FEATURES, HEVC_FP_SCRATCH,
	HEVC_FP_CURRENT, HEVC_FP_REF_HDR,
	HEVC_FP_Y, HEVC_FP_UV, HEVC_FP_Y_STRIDE, HEVC_FP_UV_STRIDE,
	HEVC_FP_REF_LIST, HEVC_FP_WEIGHT_DENOM, HEVC_FP_WEIGHTS,
	HEVC_FP_WEIGHT_OFFSETS, HEVC_FP_QUANT, HEVC_FP_DEBLOCK,
	HEVC_FP_MV, HEVC_FP_SLICE_ADDR, HEVC_FP_SLICE_SIZE,
	HEVC_FP_REF_TYPE,
	HEVC_FP_FIELD_COUNT,
};

struct avd_hevc_fp_builder {
	void *cpu;
	size_t size;
	enum avd_hevc_fp_layout_id layout;
	u16 cursor[HEVC_FP_FIELD_COUNT];
	unsigned int segment;
	int error;
};

void avd_hevc_fp_emit(struct avd_hevc_fp_builder *fp,
		      enum avd_hevc_fp_field field, u32 value);
void avd_hevc_fp_address(struct avd_hevc_fp_builder *fp,
			 enum avd_hevc_fp_field field, u64 addr,
			 unsigned int index);
void avd_hevc_fp_reference(struct avd_hevc_fp_builder *fp,
			  unsigned int index, u64 addr, const u32 offsets[4]);
int avd_hevc_fp_begin(struct avd_hevc_fp_builder *fp,
		      enum avd_hevc_fp_layout_id layout, void *cpu, size_t size,
		      u32 frame_num, unsigned int slices);
int avd_hevc_fp_validate_work(const void *cpu, size_t size,
			     unsigned int active_vps);
int avd_hevc_fp_pack_cmd(const struct avd_hevc_fp_builder *fp, u32 cmd[24],
			 unsigned int work_index, unsigned int active_vps);
#endif
