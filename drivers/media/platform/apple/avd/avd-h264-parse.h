/* SPDX-License-Identifier: GPL-2.0 */
/*
 * H.264 Annex-B NALU scanner and slice header parser for Apple AVD.
 */
#ifndef AVD_H264_PARSE_H
#define AVD_H264_PARSE_H

#include <linux/types.h>
#include <linux/v4l2-controls.h>

struct avd_h264_nalu {
	u32 off;
	u32 len;
	u8 nal_ref_idc;
	u8 nal_unit_type;
};

struct avd_h264_br {
	const u8 *data;
	u32 len;
	u32 pos;
	u32 rbsp_bits;
	u8 bit;
	u8 zeros;
	bool err;
};

bool avd_h264_mode_supported(s32 decode_mode, s32 start_code);
int avd_h264_next_nalu(const u8 *buf, u32 len, u32 *cursor,
		       struct avd_h264_nalu *nalu);
int avd_h264_next_frame_nalu(const u8 *buf, u32 len, s32 start_code,
			     u32 *cursor, struct avd_h264_nalu *nalu);

int avd_h264_parse_slice_header(const u8 *nal, u32 nal_len,
				u8 nal_ref_idc, u8 nal_unit_type,
				const struct v4l2_ctrl_h264_sps *sps,
				const struct v4l2_ctrl_h264_pps *pps,
				const struct v4l2_ctrl_h264_decode_params *decode,
				u8 num_init_refs,
				const struct v4l2_h264_reference *init_p,
				const struct v4l2_h264_reference *init_b0,
				const struct v4l2_h264_reference *init_b1,
				struct v4l2_ctrl_h264_slice_params *sl,
				struct v4l2_ctrl_h264_pred_weights *weights,
				u32 *slice_data_off, u8 *slice_bit_off);

#if IS_ENABLED(CONFIG_KUNIT)
void avd_h264_br_init(struct avd_h264_br *br, const u8 *data, u32 len, u32 pos);
u32 avd_h264_br_read_bits(struct avd_h264_br *br, unsigned int n);
u32 avd_h264_br_read_ue(struct avd_h264_br *br);
s32 avd_h264_br_read_se(struct avd_h264_br *br);
void avd_h264_br_skip_epb(struct avd_h264_br *br);
#endif

#endif /* AVD_H264_PARSE_H */
