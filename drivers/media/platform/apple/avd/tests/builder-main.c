/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
int main(void)
{
 static const u8 data[] = { 0x65, 0, 0, 3, 0x80, 0x42, 0, 0, 3, 4, 5, 6, 7, 8 };
 for (int quirks = 0; quirks < 2; quirks++)
 for (int revision = 3; revision <= 4; revision++)
 for (int type = 0; type < 3; type++)
 for (int slices = 1; slices <= 4; slices += 3)
 for (int custom = 0; custom < 2; custom++)
 for (int cabac = 0; cabac < 2; cabac++) {
  struct avd_variant variant = { .quirks = quirks, .revision = revision };
  struct avd_dev dev = { .variant = &variant };
  struct avd_h264_ctx *picture = calloc(1, sizeof(*picture));
  struct avd_segment segments[5] = {};
  struct avd_ctx ctx = { .dev = &dev, .priv = picture, .decomp = true,
   .comp = { .offsets = { 0, 0x100, 0x200, 0x300 } },
   .job = { .segments = segments },
  };
  struct avd_h264_run r = {};
  struct v4l2_ctrl_h264_sps sps = { .pic_width_in_mbs_minus1 = 3,
   .pic_height_in_map_units_minus1 = 3, .chroma_format_idc = 1,
   .flags = V4L2_H264_SPS_FLAG_FRAME_MBS_ONLY | V4L2_H264_SPS_FLAG_DIRECT_8X8_INFERENCE };
  struct v4l2_ctrl_h264_pps pps = { .flags = V4L2_H264_PPS_FLAG_SCALING_MATRIX_PRESENT |
    (custom ? V4L2_H264_PPS_FLAG_TRANSFORM_8X8_MODE : 0),
   .weighted_bipred_idc = custom ? 1 : 2 };
  struct v4l2_ctrl_h264_slice_params sl = { .slice_type = type,
   .header_bit_size = 33, .num_ref_idx_l0_active_minus1 = 1,
   .num_ref_idx_l1_active_minus1 = 1 };
  struct v4l2_ctrl_h264_decode_params decode = { .flags = type == 2 ? V4L2_H264_DECODE_PARAM_FLAG_IDR_PIC : 0 };
  struct v4l2_ctrl_h264_scaling_matrix scaling;
  struct v4l2_ctrl_h264_pred_weights weights = { .luma_log2_weight_denom = 2, .chroma_log2_weight_denom = 2 };
  struct vb2_v4l2_buffer dst = {};
  u32 output[AVD_MAX_INST * 5]; size_t words = 0;
#ifdef TEST_FRAME
  struct avd_buf program = { .cpu = output, .size = sizeof(output) };
  struct avd_frame frame = { .prog = &program };
  struct avd_buf fp = { .cpu = calloc(1, 0x8b4e0), .size = 0x8b4e0 };
  if (slices == 1 && quirks) assert(!avd_h264_fp_begin(&r.fp_cursor, &fp, AVD_H264_FP_V3));
  r.frame = &frame; r.picture = picture;
#endif
  if (cabac) pps.flags |= V4L2_H264_PPS_FLAG_ENTROPY_CODING_MODE;
  if (custom) pps.flags |= V4L2_H264_PPS_FLAG_WEIGHTED_PRED;
  memset(&scaling, 16, sizeof(scaling));
  for (int l = 0; l < 2; l++) for (int i = 0; i < 32; i++) {
   weights.weight_factors[l].luma_weight[i] = custom ? i+1 : 4;
   weights.weight_factors[l].luma_offset[i] = custom ? -i : 0;
   for (int c = 0; c < 2; c++) {
    weights.weight_factors[l].chroma_weight[i][c] = custom ? i+c+1 : 4;
    weights.weight_factors[l].chroma_offset[i][c] = custom ? -i-c : 0;
   }
  }
  for (int i = 0; i < 2; i++) {
   decode.dpb[i].flags = V4L2_H264_DPB_ENTRY_FLAG_VALID;
   decode.dpb[i].reference_ts = i;
   decode.dpb[i].top_field_order_cnt = i*2;
   sl.ref_pic_list0[i].index = i; sl.ref_pic_list1[i].index = i;
   refs[i].base.vb.vb2_buf.dma = 0x600000 + i*0x10000;
   refs[i].base.vb.vb2_buf.planes[0].length = 0x10000;
   refs[i].comp.start_offset = 0x1000;
  }
  ctx.decoded_fmt.fmt.pix_mp.width = ctx.decoded_fmt.fmt.pix_mp.height = 64;
  ctx.decoded_fmt.fmt.pix_mp.plane_fmt[0].bytesperline = 64;
  picture->slice_num = slices;
#ifdef TEST_FRAME
  picture->slice = (struct avd_buf) { .cpu = (void *)data, .addr = 0x123450000ULL, .size = sizeof(data) };
  picture->active_slice = &picture->slice;
#else
  picture->slices = calloc(4, sizeof(*picture->slices));
  picture->slices[0] = (struct avd_buf) { .cpu = (void *)data, .addr = 0x123450000ULL, .size = sizeof(data) };
  picture->active_slice = &picture->slices[0];
#endif
  picture->bufs.above_info.addr = 0x100000;
  picture->bufs.ip_above.addr = 0x110000;
  picture->bufs.lf_above.addr = 0x120000;
  picture->bufs.lf_above_info.addr = 0x130000;
  picture->bufs.mv_above_info.addr = 0x140000;
  picture->bufs.pipe_state.addr = 0x150000;
  r.base.bufs.dst = &dst;
  r.base.y_out = 0x200000; r.base.uv_out = 0x210000; r.base.comp_out = 0x220000;
  r.addresses.mv_color = 0x300000;
  r.sps = &sps; r.pps = &pps; r.slice_params = &sl; r.decode_params = &decode;
  r.scaling_matrix = &scaling; r.pred_weights = &weights;
  r.num_valid = 2; r.cur_poc = 6;
  assert(avd_h264_find_slice_data(data, sizeof(data), avd_h264_header_len(&r), &r.slice_data_off));
  stream_hdr(&ctx, &r);
  for (int i = 0; i < slices; i++) {
   ctx.job.num = i+1; sl.first_mb_in_slice = i*4;
   stream_slice(&ctx, &r);
  }
#ifdef TEST_FRAME
  assert(!r.error); words = frame.prog_words;
  if (r.fp_cursor.buf) {
   const u8 *f = fp.cpu;
   assert(get_unaligned_le32(f+0x3c) == 0x003f003f);
   assert(get_unaligned_le32(f+0x210) == (r.base.y_out >> 8));
   assert(get_unaligned_le32(f+0xc0) == (r.base.comp_out >> 7));
   assert(get_unaligned_le32(f+0xa84) == (u32)(picture->slice.addr + r.slice_data_off));
   assert(get_unaligned_le32(f+0xa88) == sizeof(data)-r.slice_data_off);
   assert(get_unaligned_le32(f+0x4cc) == (custom ? 0x10101010 : 0x060a0d10));
   if (type != 2) assert(get_unaligned_le32(f+0x110) == ((refs[0].base.vb.vb2_buf.dma + 0x1000) >> 7));
  }
  free(fp.cpu);
#else
  for (int i = 0; i <= slices; i++) {
   memcpy(output + words, segments[i].instructions, segments[i].num*4);
   words += segments[i].num;
  }
#endif
  fwrite(&words, sizeof(words), 1, stdout); fwrite(output, 4, words, stdout);
#ifdef TEST_FRAME
  r.fp_cursor.buf = NULL; r.error = 0; frame.prog_words = 0;
  program.size = sizeof(u32); output[1] = 0xdeadbeef;
  stream_hdr(&ctx, &r);
  assert(r.error == -ENOSPC && frame.prog_words == 1);
  assert(output[1] == 0xdeadbeef);
#endif
#ifndef TEST_FRAME
  free(picture->slices);
#endif
  free(picture);
 }
 return 0;
}
