/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _APPLE_AVE_LAYOUT_H_
#define _APPLE_AVE_LAYOUT_H_

/* Zero offsets denote fields absent from a firmware's wire format. */
struct ave_start_layout {
	u16 ref_a, ref_b, ref_c, ref_c_alias, ref_d;
	u16 ref_e, ref_f, ref_stride, ref_c_stride;
	u16 mb, coded, meta, row_a, row_b, fixed, hevc_extra;
	u16 tiles[8];
	u16 refs_count, work_count, lfs_count, slots_count, meta_count;
	u16 mb_count, row_a_count, row_b_count, fixed_count, tiles_count;
	u16 codec;
	u16 flag_8bit;
	u16 const_214;
	u16 const_218;
	u16 fps;
	u16 rc_mode;
	u16 bitrate;
	u16 init_qp;
	u16 limits;
	u16 limit_bytes;
	u16 limit_secs;
	u16 f64_288;
	u16 f64_290;
	u16 min_qp;
	u16 max_qp;
	u16 gop_flags;
	u16 p_frames;
	u16 max_b;
	u16 ref_depth;
	u16 idr_interval;
	u16 f64_2c0;
	u16 f64_2c8;
	u16 rc_flags;
	u16 min_qp2;
	u16 max_qp2;
	u16 rc_310;
	u16 size_class;
	u16 coded_w;
	u16 coded_h;
	u16 codec_flags;
	u16 h264_380;
	u16 h264_384;
	u16 codec_2588;
	u16 bit_depth;
	u16 coded_h2;
	u16 h264_profile;
	u16 h264_level;
	u16 h264_max_refs;
	u16 h264_width_mbs;
	u16 h264_height_mbs;
	u16 h264_crop;
	u16 h264_crop_left;
	u16 h264_crop_right;
	u16 h264_crop_top;
	u16 h264_crop_bottom;
	u16 h264_cabac;
	u16 h264_8x8;
	u16 hevc_pad;
	u16 hevc_rc_270c;
	u16 hevc_rc_9604;
	u16 hevc_rc_9608;
	u16 hevc_profile_a;
	u16 hevc_compat_a;
	u16 hevc_8bit_a;
	u16 hevc_level_a;
	u16 hevc_profile_b;
	u16 hevc_compat_b;
	u16 hevc_8bit_b;
	u16 hevc_level_b;
	u16 hevc_width;
	u16 hevc_height;
	u16 hevc_width_ctb;
	u16 hevc_height_ctb;
	u16 hevc_conf;
	u16 hevc_conf_left;
	u16 hevc_conf_right;
	u16 hevc_conf_top;
	u16 hevc_conf_bottom;
	u16 hevc_depth_luma;
	u16 hevc_depth_chroma;
	u16 hevc_refs;
	u16 hevc_reorder_copy;
	u16 hevc_dpb_minus1;
	u16 hevc_reorder;
	u16 hevc_reorder_on;
	u16 geometry_mode, geometry_depth;
	u16 profile_idc, level_idc, max_refs_copy, mb_size, mb_height;
	u16 crop_copy, crop_right_copy, crop_bottom_copy;
	u16 codec_byte, cabac_copy, transform_copy, hevc_cq, hevc_cq_copy;
	u16 hevc_cq_codec, hevc_cq_picture;
};

struct ave_proc_layout {
	u16 first_frame;
	u16 y, uv; /* {u64 address, u32 size, u32 stride} */
	u16 in_w, in_h;
	u16 coded_cap;
	u16 fps_int;
	u16 bitrate;
	u16 limits, limit_bytes, limit_secs;
	u16 const5;
	u16 fps; /* f64 */
	u16 timescale; /* u64 */
	u16 slot;
	u16 coded, meta; /* u64 addresses */
	u16 ordinal;
	u16 pts; /* u64 */
	u16 force_key; /* 1 on a forced key frame */
	u16 force_type; /* 2 on a forced key frame */
	u16 frame_buf; /* H.264 only */
	u16 shdr_table; /* HEVC only */
	u16 crop; /* HEVC only */
	u16 meta_size, extra, frame_size;
	u16 low_delay; /* HEVC only: 3 with reordering off */
};

struct ave_fw_layout {
	const char *name;
	size_t bytes[2][AVE_IMAGE_COUNT];
	struct ave_start_layout start[2];
	struct ave_proc_layout process[2];
	u32 hdr_bytes, meta_bytes;
	u32 process_timeout_us;
	u8 process_subtype, max_slots, mcc_dsid;
	bool counted;
};

extern const struct ave_fw_layout ave_layout_9003_78_0;
/* Not selectable until its loader TEXT CRC is measured and qualified. */
extern const struct ave_fw_layout ave_layout_26a428;
const struct ave_fw_layout *ave_layout_for_crc(u32 crc);

#endif
