/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
int main(void)
{
 struct avd_variant variant = { .quirks = AVD_QUIRK_LSR, .revision = 3,
  .h264_fp_layout = AVD_H264_FP_V3 };
 struct avd_dev dev = { .variant = &variant };
 struct avd_h264_ctx h264 = {};
 struct avd_ctx ctx = { .dev = &dev, .priv = &h264 };
 struct v4l2_ctrl_h264_sps sps = { .chroma_format_idc = 1,
  .pic_width_in_mbs_minus1 = 3, .pic_height_in_map_units_minus1 = 6,
  .flags = V4L2_H264_SPS_FLAG_FRAME_MBS_ONLY };
 struct v4l2_ctrl_h264_pps pps = {};
 struct v4l2_ctrl_h264_decode_params decode = { .flags = V4L2_H264_DECODE_PARAM_FLAG_IDR_PIC };
 struct v4l2_ctrl_h264_slice_params sl = { .slice_type = V4L2_H264_SLICE_TYPE_I, .header_bit_size = 33 };
 struct v4l2_ctrl_h264_scaling_matrix scaling;
 struct v4l2_ctrl_h264_pred_weights weights = {};
 struct avd_decoded_buffer dst[17] = {};
 struct vb2_v4l2_buffer src[17][4] = {};
 struct avd_buf program[17] = {}, fp = { .cpu = calloc(1, 0x8b4e0), .size = 0x8b4e0 };
 struct avd_frame f[17] = {};
 u8 data[40] = { 0, 0, 0, 0x65, 0, 0, 3, 0x80, 0x42 };
 unsigned int ids[] = { V4L2_CID_STATELESS_H264_SPS, V4L2_CID_STATELESS_H264_PPS,
  V4L2_CID_STATELESS_H264_DECODE_PARAMS, V4L2_CID_STATELESS_H264_SLICE_PARAMS,
  V4L2_CID_STATELESS_H264_SCALING_MATRIX, V4L2_CID_STATELESS_H264_PRED_WEIGHTS };
 void *controls[] = { &sps, &pps, &decode, &sl, &scaling, &weights };
 memset(&scaling, 16, sizeof(scaling));
 ctx.decoded_fmt.fmt.pix_mp.width = 64;
 ctx.decoded_fmt.fmt.pix_mp.height = 112;
 ctx.decoded_fmt.fmt.pix_mp.plane_fmt[0].bytesperline = 64;
 ctx.decoded_fmt.fmt.pix_mp.plane_fmt[0].sizeimage = 0x10000;
 for (int i = 0; i < 6; i++) ctx.ctrl_hdl.ctrls[i] = (struct v4l2_ctrl) { .id = ids[i], .p_cur.p = controls[i] };
 for (int n = 0; n < 17; n++) {
  program[n] = (struct avd_buf) { .cpu = calloc(1024, 4), .size = 4096 };
  dst[n].base.vb.vb2_buf.dma = 0x200000 + n * 0x10000;
  dst[n].base.vb.vb2_buf.planes[0].length = 0x10000;
  f[n] = (struct avd_frame) { .idx = n % 16, .dst = &dst[n].base.vb, .prog = &program[n] };
  for (int i = 0; i < 4; i++) {
   src[n][i].vb2_buf = (struct vb2_buffer) { .dma = 0x10000000 + n * 0x10000 + i * 0x1000,
    .data = data, .payload = sizeof(data), .planes = { { .length = sizeof(data), .data_offset = 3 } } };
  }
 }
 for (int i=0; i<2; i++) {
  decode.dpb[i].flags=V4L2_H264_DPB_ENTRY_FLAG_VALID;
  decode.dpb[i].reference_ts=i;
  refs[i].base.vb.vb2_buf.dma=0x600000+i*0x10000;
  refs[i].base.vb.vb2_buf.planes[0].length=0x10000;
 }
 sl.ref_pic_list1[0].index=1;
 for (int n = 0; n < 16; n++) {
  sl.slice_type=n%3;
  decode.flags=sl.slice_type==V4L2_H264_SLICE_TYPE_I ? V4L2_H264_DECODE_PARAM_FLAG_IDR_PIC : 0;
  int count = n & 1 ? 1 : 4;
  decode.frame_num = n;
  for (int i = 0; i < count; i++) {
   sl.first_mb_in_slice = i * 4;
   f[n].src = &src[n][i];
   src[n][i].flags = i + 1 < count ? V4L2_BUF_FLAG_M2M_HOLD_CAPTURE_BUF : 0;
   /* Intermediate slots and programs may be reused by unrelated contexts. */
   f[n].idx = i + 1 < count ? 15 : n;
   assert(!avd_h264_prepare(&ctx, &f[n]));
   assert(f[n].dep == AVD_DEP_NONE);
   if (i + 1 < count) {
    assert(!f[n].prog_words && h264.pending);
    avd_h264_retire(&ctx, &f[n], VB2_BUF_STATE_DONE);
    memset(program[n].cpu, 0xcd, program[n].size);
   }
  }
  assert(!h264.pending && f[n].codec_priv && f[n].prog_words);
  u32 *program_words=program[n].cpu;
  assert(program_words[0] == (AVD_OP_EXEC | AVD_OP_EXEC_FLAG_START_REV3(1) | AVD_OP_EXEC_FIFO_IDX(n)));
  assert(program_words[f[n].prog_words-1] == (AVD_OP_EXEC | AVD_OP_EXEC_FLAG_END(1)));
  unsigned int exec_count=0;
  for (size_t i=0;i<f[n].prog_words;i++) if ((program_words[i]&0xff000000)==AVD_OP_EXEC) exec_count++;
  assert(exec_count == (unsigned int)count+1);
  assert(f[n].frame_num == (u32)n && f[n].cmd[0] == AVD_CODEC_H264);
  struct avd_h264_ctx *picture = f[n].codec_priv;
  assert(picture->busy && picture->slice_num == (size_t)count);
  for (int i = 0; i < count; i++) {
   assert(!picture->slices[i].data.cpu);
   assert(picture->slices[i].data.addr == src[n][i].vb2_buf.dma + 3);
   bool found = false;
   u32 *words = program[n].cpu;
   for (size_t j = 0; j + 2 < f[n].prog_words; j++)
    if ((words[j] & 0xfff00000) == AVD_OP_CODED_DATA &&
        words[j+1] == (u32)(picture->slices[i].data.addr + picture->slices[i].data_offset)) found = true;
   assert(found);
  }
  for (int i = 0; i < n; i++) {
   assert(f[i].codec_priv != f[n].codec_priv);
   assert(f[i].fifo_iova != f[n].fifo_iova);
  }
 }
 sl.first_mb_in_slice = 0; f[16].src = &src[16][0];
 assert(avd_h264_prepare(&ctx, &f[16]) == -EBUSY);
 for (int n = 15; n >= 0; n--) avd_h264_retire(&ctx, &f[n], n & 1 ? VB2_BUF_STATE_DONE : VB2_BUF_STATE_ERROR);
 for (int n = 0; n < 16; n++) assert(!h264.slots[n]->busy);
 /* Corrupt continuation releases the partial picture, including a held request. */
 f[0].src = &src[0][0]; src[0][0].flags = V4L2_BUF_FLAG_M2M_HOLD_CAPTURE_BUF;
 assert(!avd_h264_prepare(&ctx, &f[0]));
 sl.first_mb_in_slice = 0;
 assert(avd_h264_prepare(&ctx, &f[0]) == -EINVAL);
 avd_h264_retire(&ctx, &f[0], VB2_BUF_STATE_ERROR);
 assert(!h264.pending && !h264.slots[0]->busy);
 /* Streamoff retires the last held request with ERROR. */
 assert(!avd_h264_prepare(&ctx, &f[0]));
 avd_h264_retire(&ctx, &f[0], VB2_BUF_STATE_ERROR);
 assert(!h264.pending && !h264.slots[0]->busy);
 /* Capacity failure is bounded and fully retireable. */
 src[0][0].flags = 0; program[0].size = 4;
 ((u32 *)program[0].cpu)[1] = 0xdeadbeef;
 assert(avd_h264_prepare(&ctx, &f[0]) == -ENOSPC);
 assert(((u32 *)program[0].cpu)[1] == 0xdeadbeef);
 avd_h264_retire(&ctx, &f[0], VB2_BUF_STATE_ERROR);
 program[0].size = 4096;
 /* Apple V3 uses an independent record for every accumulated slice. */
 apple_backend = true; f[0].fp = &fp;
 for (int i = 0; i < 4; i++) {
  sl.first_mb_in_slice = i * 4;
  f[0].src = &src[0][i]; src[0][i].flags = i < 3 ? V4L2_BUF_FLAG_M2M_HOLD_CAPTURE_BUF : 0;
  assert(!avd_h264_prepare(&ctx, &f[0]));
 }
 assert(get_unaligned_le32((u8 *)fp.cpu + 0x1c) == 4);
 for (int i = 0; i < 4; i++) {
  struct avd_h264_ctx *picture = f[0].codec_priv;
  assert(get_unaligned_le32((u8 *)fp.cpu + 0xa84 + 0x3b4*i) ==
   (u32)(src[0][i].vb2_buf.dma + 3 + picture->slices[i].data_offset));
 }
 avd_h264_retire(&ctx, &f[0], VB2_BUF_STATE_DONE);
 assert(!avd_h264_slice_fits(&sps, 0, SIZE_MAX, 1, 0x10000));
 for (int n = 0; n < 17; n++) free(program[n].cpu);
 for (int n = 0; n < 16; n++) avd_h264_free_picture(&ctx, h264.slots[n]);
 free(fp.cpu);
 puts("PASS: actual prepare/retire, held 1/4-slice pictures, 16 independent scratch owners, OUTPUT DMA offsets, V3 records, abort and capacity errors");
 return 0;
}
