/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _APPLE_AVE_LAYOUT_H_
#define _APPLE_AVE_LAYOUT_H_

struct ave_ctx;
struct ave_dev;
struct ave_fw_layout;
struct ave_fw_abi;

struct ave_command {
	enum ave_opcode op;
	enum ave_codec codec;
	u64 token;
	/* Zero unit and arg1 mean the encoder's values. */
	u32 client, subtype, unit, arg0, arg1, arg2;
};

/* Status is normalized to zero for success by the ABI parser. */
struct ave_reply {
	enum ave_opcode op;
	u64 client;
	u32 subtype, status, raw_status, bytes;
};

struct ave_fw_transport {
	u32 ipc_bytes;
	u32 command_offset, command_bytes, scalar_offset;
	u32 process_offset, process_bytes;
	/* Zero channel/heap sizes mean bounded negotiation on 9003. */
	u32 hello[4];
	u32 descriptors;
	u32 io_index, t2h_index;
	u32 slots[2], doorbells[2], offsets[2]; /* zero: discovered */
	bool boot_config;
	bool sync_host_clock;
	bool scratch_halt;
};

struct ave_fw_abi {
	const struct ave_fw_transport *transport;
	u32 wire_id[2][AVE_OP_COUNT];
	u32 reply_id[AVE_OP_COUNT];
	u32 scalar_bytes[AVE_OP_COUNT];
	u32 semantic[AVE_OP_COUNT];
	u32 header_bytes, timeout_offset, created_offset;
	bool padded_surfaces;
	void (*write_header)(u8 *cmd, const struct ave_fw_abi *abi,
			     const struct ave_command *header);
	void (*write_config)(struct ave_dev *ave, u8 *cmd);
	int (*parse_reply)(const struct ave_fw_abi *abi, const u8 *msg, u32 len,
			   struct ave_reply *reply);
};

/* Separate address/size offsets and strides also represent split arrays. */
struct ave_surface_layout {
	u32 address, size;
	u32 address_stride, size_stride;
};

#define AVE_SURFACE(off, stride)      \
	{ .address = (off),           \
	  .size = (off) + 8,          \
	  .address_stride = (stride), \
	  .size_stride = (stride) }

/* Zero offsets denote fields absent from a firmware's wire format. */
struct ave_start_layout {
	struct ave_surface_layout header, secondary;
	struct ave_surface_layout ref_a, ref_b, ref_c, ref_c_alias, ref_d;
	struct ave_surface_layout ref_e, ref_f;
	struct ave_surface_layout mb, coded, meta, row_a, row_b, fixed,
		hevc_extra;
	struct ave_surface_layout tiles[8];
	u32 refs_count, work_count, lfs_count, slots_count, meta_count;
	u32 mb_count, row_a_count, row_b_count, fixed_count, tiles_count;
	u32 codec;
	u32 flag_8bit;
	u32 const_214;
	u32 const_218;
	u32 fps;
	u32 rc_mode;
	u32 bitrate;
	u32 init_qp;
	u32 limits;
	u32 limit_bytes;
	u32 limit_secs;
	u32 f64_288;
	u32 f64_290;
	u32 min_qp;
	u32 max_qp;
	u32 gop_flags;
	u32 p_frames;
	u32 max_b;
	u32 ref_depth;
	u32 idr_interval;
	u32 f64_2c0;
	u32 f64_2c8;
	u32 rc_flags;
	u32 min_qp2;
	u32 max_qp2;
	u32 rc_310;
	u32 size_class;
	u32 coded_w;
	u32 coded_h;
	u32 codec_flags;
	u32 h264_380;
	u32 h264_384;
	u32 codec_2588;
	u32 bit_depth;
	u32 coded_h2;
	u32 h264_profile;
	u32 h264_level;
	u32 h264_max_refs;
	u32 h264_width_mbs;
	u32 h264_height_mbs;
	u32 h264_crop;
	u32 h264_crop_left;
	u32 h264_crop_right;
	u32 h264_crop_top;
	u32 h264_crop_bottom;
	u32 h264_cabac;
	u32 h264_8x8;
	u32 hevc_pad;
	u32 hevc_rc_270c;
	u32 hevc_rc_9604;
	u32 hevc_rc_9608;
	u32 hevc_profile_a;
	u32 hevc_compat_a;
	u32 hevc_8bit_a;
	u32 hevc_level_a;
	u32 hevc_profile_b;
	u32 hevc_compat_b;
	u32 hevc_8bit_b;
	u32 hevc_level_b;
	u32 hevc_width;
	u32 hevc_height;
	u32 hevc_width_ctb;
	u32 hevc_height_ctb;
	u32 hevc_conf;
	u32 hevc_conf_left;
	u32 hevc_conf_right;
	u32 hevc_conf_top;
	u32 hevc_conf_bottom;
	u32 hevc_depth_luma;
	u32 hevc_depth_chroma;
	u32 hevc_refs;
	u32 hevc_reorder_copy;
	u32 hevc_dpb_minus1;
	u32 hevc_reorder;
	u32 hevc_reorder_on;
	u32 geometry_mode, geometry_depth;
	u32 profile_idc, level_idc, max_refs_copy, mb_size, mb_height;
	u32 crop_copy, crop_right_copy, crop_bottom_copy;
	u32 codec_byte, cabac_copy, transform_copy, hevc_cq, hevc_cq_copy;
	u32 hevc_cq_codec, hevc_cq_picture;
};

struct ave_proc_layout {
	u32 first_frame;
	u32 y, uv; /* {u64 address, u32 size, u32 stride} */
	u32 in_w, in_h;
	u32 coded_cap;
	u32 fps_int;
	u32 bitrate;
	u32 limits, limit_bytes, limit_secs;
	u32 const5;
	u32 fps; /* f64 */
	u32 timescale; /* u64 */
	u32 slot;
	u32 coded, meta; /* u64 addresses */
	u32 ordinal;
	u32 pts; /* u64 */
	u32 force_key; /* 1 on a forced key frame */
	u32 force_type; /* 2 on a forced key frame */
	u32 frame_buf; /* H.264 only */
	u32 shdr_table; /* HEVC only */
	u32 crop; /* HEVC only */
	u32 meta_size, extra, frame_size;
	u32 low_delay; /* HEVC only: 3 with reordering off */
};

struct ave_fw_layout {
	const char *name;
	const struct ave_fw_abi *abi;
	bool encode_ready;
	size_t bytes[2][AVE_IMAGE_COUNT];
	struct ave_start_layout start[2];
	struct ave_proc_layout process[2];
	u32 hdr_bytes, meta_bytes;
	u32 process_timeout_us;
	u8 process_subtype, max_slots, mcc_dsid;
	bool counted;
};

extern const struct ave_fw_abi ave_abi_9003, ave_abi_6070;
extern const struct ave_fw_layout ave_layout_9003_78_0;
extern const struct ave_fw_layout ave_layout_6070_11_1;
/* Not selectable until its loader TEXT CRC is measured and qualified. */
extern const struct ave_fw_layout ave_layout_26a428;
const struct ave_fw_layout *ave_layout_for_crc(u32 crc);

#endif
