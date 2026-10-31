/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
int main(int argc, char **argv)
{
	struct avd_variant variant = { .revision = 4 };
	struct avd_dev dev = { .variant = &variant };
	struct avd_ctx ctx_value = { .dev = &dev, .decomp = true };
	struct avd_ctx *ctx = &ctx_value;
	struct avd_hevc_run run_value = {};
	struct avd_hevc_run *run = &run_value;
	struct avd_hevc_ctx hevc = {};
	struct avd_decoded_buffer dst = {};
	struct v4l2_ctrl_hevc_sps sps = {};
	struct v4l2_ctrl_hevc_pps pps = {};
	struct v4l2_ctrl_hevc_decode_params decode = {};
	struct v4l2_ctrl_hevc_scaling_matrix scaling = {};
	struct v4l2_ctrl_hevc_slice_params sl[4] = {};
	u32 entry[3] = { 100, 100, 100 };
	u32 rs_to_ts[64], tile_ids[64];
	unsigned int i, j, bit_depth, ctu, tiles, type, hw, layout;
	int ret;
#ifndef HEVC_LEGACY
	struct avd_buf prog = { .size = 65536 };
	struct avd_frame f = { .prog = &prog };
	struct avd_hevc_bufs bufs = {};
	struct avd_hevc_slice_range ranges[4];
	size_t segment_ends[8];

	prog.cpu = calloc(1, prog.size);
	run->frame = &f;
	run->bufs = &bufs;
	run->ranges = ranges;
	run->segment_ends = segment_ends;
#endif
	if (argc != 7)
		return 2;
	bit_depth = atoi(argv[1]); ctu = atoi(argv[2]); tiles = atoi(argv[3]);
	type = atoi(argv[4]); hw = atoi(argv[5]); layout = atoi(argv[6]);
	variant.revision = hw == 0 ? 3 : 4;
	variant.quirks = hw < 2 ? AVD_QUIRK_LSR : 0;
	if (hw == 0)
		variant.quirks |= AVD_QUIRK_NO_PIPE_STATE;
	if (hw == 3)
		variant.quirks |= AVD_QUIRK_HEVC_V5_HEADER;
	ctx->priv = &hevc;
	ctx->decoded_fmt.fmt.pix_mp.width = 256;
	ctx->decoded_fmt.fmt.pix_mp.height = 256;
	ctx->decoded_fmt.fmt.pix_mp.plane_fmt[0].bytesperline = bit_depth == 10 ? 512 : 256;
	ctx->comp.offsets[0] = 0x1000;
	ctx->comp.offsets[1] = 0;
	ctx->comp.offsets[2] = 0x2000;
	ctx->comp.offsets[3] = 0x3000;
	test_reference.base.vb.vb2_buf.addr = 0x200000;
	test_reference.base.vb.planes[0].length = 0x40000;
	test_reference.comp = ctx->comp;
	ctx->job.segments = calloc(8, sizeof(*ctx->job.segments));
	sps.pic_width_in_luma_samples = 256;
	sps.pic_height_in_luma_samples = 256;
	sps.chroma_format_idc = 1;
	sps.bit_depth_luma_minus8 = sps.bit_depth_chroma_minus8 = bit_depth - 8;
	sps.log2_min_luma_coding_block_size_minus3 = 0;
	sps.log2_diff_max_min_luma_coding_block_size = ctu == 64 ? 3 : 2;
	sps.log2_diff_max_min_luma_transform_block_size = 3;
	sps.max_transform_hierarchy_depth_inter = 2;
	sps.max_transform_hierarchy_depth_intra = 2;
	sps.flags = V4L2_HEVC_SPS_FLAG_SCALING_LIST_ENABLED |
		V4L2_HEVC_SPS_FLAG_STRONG_INTRA_SMOOTHING_ENABLED |
		V4L2_HEVC_SPS_FLAG_SPS_TEMPORAL_MVP_ENABLED;
	for (i = 0; i < sizeof(scaling); i++)
		((u8 *)&scaling)[i] = (i % 255) + 1;
	pps.flags = V4L2_HEVC_PPS_FLAG_UNIFORM_SPACING |
		V4L2_HEVC_PPS_FLAG_WEIGHTED_PRED | V4L2_HEVC_PPS_FLAG_WEIGHTED_BIPRED;
	if (tiles)
		pps.flags |= V4L2_HEVC_PPS_FLAG_TILES_ENABLED;
	pps.num_tile_columns_minus1 = pps.num_tile_rows_minus1 = tiles ? 1 : 0;
	decode.num_active_dpb_entries = type == V4L2_HEVC_SLICE_TYPE_I ? 0 : 2;
	decode.dpb[0].pic_order_cnt_val = 1;
	decode.dpb[1].pic_order_cnt_val = 3;
	decode.dpb[1].flags = V4L2_HEVC_DPB_ENTRY_LONG_TERM_REFERENCE;
	for (i = 0; i < 2; i++) {
		sl[i].bit_size = 420 * 8;
		sl[i].data_byte_offset = 20;
		sl[i].slice_type = type;
		sl[i].slice_pic_order_cnt = 2;
		sl[i].slice_segment_addr = i * (256 / ctu) * (tiles ? 1 : 2);
		sl[i].num_ref_idx_l0_active_minus1 = 1;
		sl[i].num_ref_idx_l1_active_minus1 = 1;
		sl[i].ref_idx_l0[1] = 1;
		sl[i].ref_idx_l1[0] = 1;
		sl[i].flags = V4L2_HEVC_SLICE_PARAMS_FLAG_SLICE_TEMPORAL_MVP_ENABLED |
			V4L2_HEVC_SLICE_PARAMS_FLAG_COLLOCATED_FROM_L0 |
			V4L2_HEVC_SLICE_PARAMS_FLAG_SLICE_SAO_LUMA;
		sl[i].pred_weight_table.luma_log2_weight_denom = 2;
		sl[i].pred_weight_table.delta_luma_weight_l0[0] = 1;
		sl[i].pred_weight_table.delta_luma_weight_l1[1] = -1;
		sl[i].pred_weight_table.delta_chroma_weight_l0[0][0] = 2;
		sl[i].pred_weight_table.chroma_offset_l0[0][1] = -2;
	}
	run->sps = &sps; run->pps = &pps; run->decode = &decode;
	run->scaling_matrix = &scaling;
	run->sl = sl; run->num_slices = tiles ? 1 : 2;
	if (tiles) {
		sl[0].num_entry_point_offsets = 3;
		run->entry_point_offsets = entry;
		run->num_entry_point_offsets = 3;
	}
	run->base.bufs.dst = &dst.base.vb;
	run->base.coded_in = 0x100000;
	run->base.y_out = 0x300000;
	run->base.uv_out = 0x310000;
	run->base.comp_out = 0x320000;
	run->addresses.mv_color = 0x400000;
	for (i = 0; i < 64; i++) {
		rs_to_ts[i] = i;
		tile_ids[i] = i >= 32 ? 1 : 0;
	}
	run->tile_info.ctb_addr_rs_to_ts = rs_to_ts;
	run->tile_info.tile_ids = tile_ids;
	run->tile_info.col_bd[1] = 128 / ctu;
	run->tile_info.col_bd[2] = 256 / ctu;
	run->tile_info.row_bd[1] = 128 / ctu;
	run->tile_info.row_bd[2] = 256 / ctu;
#ifdef HEVC_LEGACY
#define TEST_BUF(name, value) hevc.bufs.name.addr = value
#else
#define TEST_BUF(name, value) bufs.name.addr = value
	if (layout) {
		for (i = 0; i < (unsigned int)run->num_slices; i++) {
			sl[i].data_byte_offset = i * 420 + 20;
			sl[i].bit_size = 400 * 8;
		}
	}
	ret = avd_hevc_slice_ranges(sl, run->num_slices, run->num_slices * 420, ranges);
	if (ret)
		return 3;
#endif
	TEST_BUF(pipe_state, 0x410000);
	TEST_BUF(ip_above, 0x420000);
	TEST_BUF(lf_above, 0x430000);
	TEST_BUF(lf_above_info, 0x440000);
	TEST_BUF(lf_left, 0x450000);
	TEST_BUF(lf_left_info, 0x460000);
	TEST_BUF(az_above, 0x470000);
	TEST_BUF(sw_left, 0x480000);
	TEST_BUF(mv_above_info, 0x490000);
	set_header(ctx, run);
	ret = stream_slices(ctx, run);
	if (ret)
		return 4;
#ifdef HEVC_LEGACY
	for (i = 0; i <= ctx->job.num; i++)
		for (j = 0; j < ctx->job.segments[i].num; j++)
			printf("%08x\n", ctx->job.segments[i].instructions[j]);
#else
	(void)j;
	if (run->error)
		return 5;
	for (i = 0; i < f.prog_words; i++)
		printf("%08x\n", ((u32 *)prog.cpu)[i]);
	free(prog.cpu);
#endif
	(void)layout;
	free(ctx->job.segments);
	return 0;
}
