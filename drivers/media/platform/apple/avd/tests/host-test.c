/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
static int avd_submit_job(struct avd_ctx *c)
{
 legacy_sent_words=0;
 legacy_sent[legacy_sent_words++]=AVD_OP_EXEC|AVD_OP_EXEC_FLAG_START_REV4(c->dev->variant->revision==4);
 for(size_t i=0;i<=c->job.num;i++) {
  for(size_t j=0;j<c->job.segments[i].num;j++)legacy_sent[legacy_sent_words++]=c->job.segments[i].instructions[j];
  if(i)legacy_sent[legacy_sent_words++]=AVD_OP_EXEC|AVD_OP_EXEC_FLAG_END(i==c->job.num);
 }
 free(c->job.segments);c->job.segments=NULL;
 return legacy_submit_error;
}

static struct v4l2_ctrl_av1_sequence sequence;
static struct v4l2_ctrl_av1_frame params;
static struct v4l2_ctrl_av1_tile_group_entry tiles[512];
static struct v4l2_ctrl_av1_film_grain grain;
static struct avd_variant variant = {.revision=4};
static struct avd_dev device = {.variant=&variant};

static void init_ctx(struct avd_ctx *ctx)
{
	memset(ctx,0,sizeof(*ctx));
	ctx->dev=&device;
	ctx->decoded_fmt.fmt.pix_mp.width=256;
	ctx->decoded_fmt.fmt.pix_mp.height=256;
	ctx->decoded_fmt.fmt.pix_mp.plane_fmt[0].bytesperline=256;
	ctx->comp.start_offset=65536;
	ctx->comp.offsets[0]=256;
	ctx->comp.offsets[1]=512;
	ctx->comp.offsets[2]=768;
	ctx->comp.offsets[3]=1024;
	ctx->decomp=true;
	ctx->ctrl_hdl[0]=(struct v4l2_ctrl){.id=V4L2_CID_STATELESS_AV1_SEQUENCE,.p_cur.p=&sequence};
	ctx->ctrl_hdl[1]=(struct v4l2_ctrl){.id=V4L2_CID_STATELESS_AV1_FRAME,.p_cur.p=&params};
	ctx->ctrl_hdl[2]=(struct v4l2_ctrl){.id=V4L2_CID_STATELESS_AV1_TILE_GROUP_ENTRY,.p_cur.p=tiles,.elems=V4L2_AV1_MAX_TILE_COUNT};
	ctx->ctrl_hdl[3]=(struct v4l2_ctrl){.id=V4L2_CID_STATELESS_AV1_FILM_GRAIN,.p_cur.p=&grain};
	assert(avd_av1_start(ctx)==0);
	assert(ctx->max_frames==1);
}

static void init_params(void)
{
	memset(&sequence,0,sizeof(sequence));
	memset(&params,0,sizeof(params));
	memset(&grain,0,sizeof(grain));
	memset(tiles,0,sizeof(tiles));
	sequence.bit_depth=8;
	sequence.order_hint_bits=8;
	sequence.flags=V4L2_AV1_SEQUENCE_FLAG_SUBSAMPLING_X|V4L2_AV1_SEQUENCE_FLAG_SUBSAMPLING_Y;
	params.frame_type=V4L2_AV1_KEY_FRAME;
	params.primary_ref_frame=7;
	params.frame_width_minus_1=127;
	params.frame_height_minus_1=127;
	params.upscaled_width=128;
	params.superres_denom=8;
	params.tx_mode=V4L2_AV1_TX_MODE_LARGEST;
	params.tile_info.tile_cols=1;
	params.tile_info.tile_rows=1;
	params.tile_info.width_in_sbs_minus_1[0]=1;
	params.tile_info.height_in_sbs_minus_1[0]=1;
	tiles[0].tile_size=128;
	for(int i=0;i<64;i++) {
		buffers[i].base.vb.vb2_buf=(struct vb2_buffer){.index=i,.timestamp=1000+i,.planes={{.length=1048576}},.dma=0x200000000ULL+i*0x200000,.payload=4096};
		sources[i].vb2_buf=(struct vb2_buffer){.index=i,.timestamp=1000+i,.planes={{.length=4096}},.dma=0x10000000ULL+i*0x200000,.payload=4096};
		buffers[i].av1.width=127;
		buffers[i].av1.height=127;
		buffers[i].av1.upscaled_width=128;
	}
}

static struct avd_frame make_frame(unsigned int idx,unsigned int dst,struct avd_buf *prog)
{
	return (struct avd_frame){.idx=idx,.seq=dst+1,.frame_num=dst+1,
		.src=&sources[dst],.dst=&buffers[dst].base.vb,.prog=prog};
}

static void dependency_table(void)
{
	for(int uses=0;uses<2;uses++)for(int prev=0;prev<2;prev++)
	for(int update=0;update<2;update++)for(int same=0;same<2;same++)
		assert(avd_av1_cdf_dep(uses,123,prev,same?123:456,update)==
			(uses&&prev&&update&&same));
}

static void sixteen_slots(void)
{
	struct avd_ctx ctx;
	struct avd_frame f[17];
	struct avd_buf prog[17]={0};
	u8 snapshots[16][4096];
	init_params();init_ctx(&ctx);
	for(int i=0;i<17;i++) {
		assert(!avd_buf_alloc(&device,&prog[i],4096));
		f[i]=make_frame(i,i,&prog[i]);
	}
	for(int i=0;i<16;i++) {
		assert(!avd_av1_prepare(&ctx,&f[i]));
		assert(f[i].dep==AVD_DEP_NONE);
		assert(f[i].prog_words>100);
		memcpy(snapshots[i],prog[i].cpu,4096);
		struct avd_av1_run *r=f[i].codec_priv;
		assert(!r->seq&&!r->frame&&!r->tile_group&&!r->grain);
		for(int j=0;j<i;j++) {
			struct avd_av1_run *s=f[j].codec_priv;
			assert(r->bufs.above_info.cpu!=s->bufs.above_info.cpu);
			assert(r->bufs.pipe_state.cpu!=s->bufs.pipe_state.cpu);
			assert(r->cdf_in!=s->cdf_in&&r->cdf_out!=s->cdf_out);
			assert(r->seg_in!=s->seg_in);
			assert(!memcmp(snapshots[j],prog[j].cpu,4096));
		}
	}
	assert(avd_av1_prepare(&ctx,&f[16])==-EINVAL);
	for(int i=0;i<16;i++)avd_av1_retire(&ctx,&f[i],VB2_BUF_STATE_DONE);
	avd_av1_stop(&ctx);
	for(int i=0;i<17;i++)avd_buf_free(&device,&prog[i]);
	assert(!allocations);
}

static void cdf_chain(void)
{
	struct avd_ctx ctx;
	struct avd_frame f[3];
	struct avd_buf prog[3]={0};
	init_params();init_ctx(&ctx);
	for(int i=0;i<3;i++) {assert(!avd_buf_alloc(&device,&prog[i],4096));f[i]=make_frame(i,i,&prog[i]);}
	assert(!avd_av1_prepare(&ctx,&f[0]));
	params.frame_type=V4L2_AV1_INTER_FRAME;
	params.primary_ref_frame=0;
	params.reference_frame_ts[0]=1000;
	params.flags=V4L2_AV1_FRAME_FLAG_DISABLE_FRAME_END_UPDATE_CDF;
	assert(!avd_av1_prepare(&ctx,&f[1]));
	assert(f[1].dep==AVD_DEP_PREV_DONE);
	struct avd_av1_run *a=f[0].codec_priv,*b=f[1].codec_priv;
	assert(b->cdf_in==a->cdf_out&&b->cdf_out==a->cdf_out);
	params.reference_frame_ts[0]=1001;
	int saved=allocations;
	assert(avd_av1_prepare(&ctx,&f[2])==-EAGAIN);
	assert(f[2].dep==AVD_DEP_NONE&&saved==allocations);
	avd_av1_retire(&ctx,&f[0],VB2_BUF_STATE_DONE);
	assert(!avd_av1_prepare(&ctx,&f[2]));
	assert(f[2].dep==AVD_DEP_NONE);
	assert(((struct avd_av1_run *)f[2].codec_priv)->cdf_in==b->cdf_out);
	avd_av1_retire(&ctx,&f[1],VB2_BUF_STATE_DONE);
	avd_av1_retire(&ctx,&f[2],VB2_BUF_STATE_ERROR);
	avd_av1_stop(&ctx);
	for(int i=0;i<3;i++)avd_buf_free(&device,&prog[i]);
	assert(!allocations);
}

static void reference_reuse_and_segmentation(void)
{
	struct avd_ctx ctx;struct avd_buf prog[3]={0};struct avd_frame f[3];
	init_params();init_ctx(&ctx);
	for(int i=0;i<3;i++){assert(!avd_buf_alloc(&device,&prog[i],4096));f[i]=make_frame(i,i,&prog[i]);}
	params.segmentation.flags=V4L2_AV1_SEGMENTATION_FLAG_ENABLED|V4L2_AV1_SEGMENTATION_FLAG_UPDATE_MAP;
	assert(!avd_av1_prepare(&ctx,&f[0]));
	struct avd_av1_run *a=f[0].codec_priv;
	struct avd_av1_aux *cdf=a->cdf_out,*seg=a->seg_out;
	params.frame_type=V4L2_AV1_INTER_FRAME;params.primary_ref_frame=0;
	params.reference_frame_ts[0]=1000;
	params.segmentation.flags=V4L2_AV1_SEGMENTATION_FLAG_ENABLED;
	params.flags=V4L2_AV1_FRAME_FLAG_DISABLE_FRAME_END_UPDATE_CDF;
	assert(!avd_av1_prepare(&ctx,&f[1]));
	struct avd_av1_run *b=f[1].codec_priv;
	assert(b->cdf_out==cdf&&b->seg_out==seg);
	avd_av1_retire(&ctx,&f[0],VB2_BUF_STATE_DONE);
	/* Reuse the original capture: inherited state belongs to buffer 1 too. */
	params.primary_ref_frame=7;params.frame_type=V4L2_AV1_KEY_FRAME;
	params.flags=0;params.segmentation.flags=0;
	buffers[0].base.vb.vb2_buf.timestamp=2000;
	f[2].dst=&buffers[0].base.vb;
	f[2].src->vb2_buf.timestamp=2000;
	assert(!avd_av1_prepare(&ctx,&f[2]));
	assert(cdf->refs.n>=2&&seg->refs.n>=2);
	assert(b->cdf_in==cdf&&b->seg_in==seg&&cdf->done&&seg->done);
	avd_av1_retire(&ctx,&f[1],VB2_BUF_STATE_DONE);
	avd_av1_retire(&ctx,&f[2],VB2_BUF_STATE_ERROR);
	avd_av1_stop(&ctx);
	for(int i=0;i<3;i++)avd_buf_free(&device,&prog[i]);
	assert(!allocations);
}

static void allocation_failures(void)
{
	for(int fail=0;fail<16;fail++) {
		struct avd_ctx ctx;struct avd_buf prog={0};
		init_params();init_ctx(&ctx);
		assert(!avd_buf_alloc(&device,&prog,4096));
		struct avd_frame f=make_frame(0,0,&prog);
		fail_after=fail;
		int rc=avd_av1_prepare(&ctx,&f);
		fail_after=-1;
		if(!rc)avd_av1_retire(&ctx,&f,VB2_BUF_STATE_ERROR);
		else {assert(rc==-ENOMEM);assert(!f.codec_priv&&!f.prog_words);}
		avd_av1_stop(&ctx);avd_buf_free(&device,&prog);
		assert(!allocations);
	}
}

static void bounds_and_fp(void)
{
	struct avd_ctx ctx;struct avd_buf fp={0},prog={0};
	init_params();init_ctx(&ctx);
	assert(!avd_av1_validate_tiles(&params,tiles,1,128));
	assert(!avd_av1_validate_tiles(&params,tiles,V4L2_AV1_MAX_TILE_COUNT,128));
	assert(avd_av1_validate_tiles(&params,tiles,0,128)==-EINVAL);
	tiles[0].tile_offset=UINT32_MAX;
	assert(avd_av1_validate_tiles(&params,tiles,1,128)==-EINVAL);
	tiles[0].tile_offset=0;
	assert(!avd_buf_alloc(&device,&fp,0x1130d0));
	assert(!avd_buf_alloc(&device,&prog,4096));
	struct avd_frame f=make_frame(0,0,&prog);f.fp=&fp;
	variant.av1_fp_layout=AVD_AV1_FP_BORAGE;
	assert(avd_av1_prepare(&ctx,&f)==-EOPNOTSUPP);
	assert(get_unaligned_le16((u8 *)fp.cpu+0x3e4)==1);
	assert(get_unaligned_le16((u8 *)fp.cpu+0x3e6)==1);
	put_unaligned_le16(0x1002,(u8 *)fp.cpu+0x1a);
	put_unaligned_le32(0xdeadbeef,(u8 *)fp.cpu+0x4f8);
	put_unaligned_le16(3,(u8 *)f.cmd+6);
	assert(avd_av1_pack_cmd(&ctx,&f)==-EOPNOTSUPP);
	assert(get_unaligned_le32((u8 *)f.cmd+0x4c)==0xdeadbeef);
	assert(get_unaligned_le32((u8 *)f.cmd+0xc)==0x01483118+0x1814*3);
	put_unaligned_le16(0x1003,(u8 *)fp.cpu+0x1a);
	assert(avd_av1_pack_cmd(&ctx,&f)==-EINVAL);
	variant.av1_fp_layout=AVD_AV1_FP_T8140_TODO;
	assert(avd_av1_pack_cmd(&ctx,&f)==-EOPNOTSUPP);
	variant.av1_fp_layout=AVD_AV1_FP_UNKNOWN;
	f.fp=NULL;grain.flags=V4L2_AV1_FILM_GRAIN_FLAG_APPLY_GRAIN;
	assert(!avd_av1_prepare(&ctx,&f));
	avd_av1_retire(&ctx,&f,VB2_BUF_STATE_DONE);
	grain.flags=0;prog.size=4;
	assert(avd_av1_prepare(&ctx,&f)==-ENOSPC);
	avd_av1_stop(&ctx);avd_buf_free(&device,&fp);avd_buf_free(&device,&prog);
	assert(!allocations);
}

static void program_parity(void)
{
	for(int test=0;test<64;test++) {
		struct avd_ctx ctx;struct avd_buf prog={0};
		init_params();init_ctx(&ctx);
		if(test&1)sequence.bit_depth=10;
		if(test&32)grain.flags=V4L2_AV1_FILM_GRAIN_FLAG_APPLY_GRAIN;
		if(test&16) {
			params.frame_type=V4L2_AV1_INTER_FRAME;
			sequence.flags|=V4L2_AV1_SEQUENCE_FLAG_ENABLE_ORDER_HINT|V4L2_AV1_SEQUENCE_FLAG_ENABLE_REF_FRAME_MVS;
			params.flags|=V4L2_AV1_FRAME_FLAG_USE_REF_FRAME_MVS;
			params.order_hint=4;
			for(int i=0;i<7;i++){params.ref_frame_idx[i]=i;params.reference_frame_ts[i]=1001+i;}
		}
		if(test&2) {
			sequence.flags|=V4L2_AV1_SEQUENCE_FLAG_ENABLE_CDEF;
			params.cdef.bits=2;params.cdef.damping_minus_3=1;
			for(int i=0;i<4;i++){params.cdef.y_pri_strength[i]=i;params.cdef.uv_pri_strength[i]=3-i;}
		}
		if(test&4) {
			params.upscaled_width=160;params.superres_denom=10;
			params.loop_restoration.frame_restoration_type[0]=V4L2_AV1_FRAME_RESTORE_WIENER;
			params.loop_restoration.loop_restoration_size[0]=64;
		}
		if(test&8) {
			params.tile_info.tile_cols=2;params.tile_info.tile_rows=2;
			for(int i=0;i<2;i++) {
				params.tile_info.width_in_sbs_minus_1[i]=0;
				params.tile_info.height_in_sbs_minus_1[i]=0;
				params.tile_info.mi_col_starts[i]=i*16;
				params.tile_info.mi_row_starts[i]=i*16;
			}
			for(int i=0;i<4;i++)tiles[i]=(struct v4l2_ctrl_av1_tile_group_entry){.tile_offset=i*128,.tile_size=128,.tile_col=i%2,.tile_row=i/2};
			ctx.ctrl_hdl[2].elems=4;
		}
		assert(!avd_buf_alloc(&device,&prog,8192));
		struct avd_frame f=make_frame(0,0,&prog);
		assert(!avd_av1_prepare(&ctx,&f));
		struct avd_av1_run *r=f.codec_priv;
		r->seq=&sequence;r->frame=&params;r->tile_group=tiles;r->grain=&grain;
		ctx.job.segments=calloc(r->num_tiles+1,sizeof(struct avd_segment));
		legacy_set_header(&ctx,r);legacy_set_tiles(&ctx,r);
		u32 expected[2048];unsigned int n=0;
		expected[n++]=AVD_OP_EXEC|AVD_OP_EXEC_FLAG_START_REV4(1);
		for(unsigned int i=0;i<=r->num_tiles;i++) {
			struct avd_segment *seg=&ctx.job.segments[i];
			for(unsigned int j=0;j<seg->num;j++)expected[n++]=seg->instructions[j];
			if(i)expected[n++]=AVD_OP_EXEC|AVD_OP_EXEC_FLAG_END(i==r->num_tiles);
		}
		assert(n==f.prog_words&&!memcmp(expected,prog.cpu,n*sizeof(u32)));
		free(ctx.job.segments);
		avd_av1_retire(&ctx,&f,VB2_BUF_STATE_DONE);avd_av1_stop(&ctx);
		avd_buf_free(&device,&prog);assert(!allocations);
	}
	puts("AV1 legacy program parity: 64 synthetic control vectors (including grain-less fallback): PASS");
}

static void legacy_transport(void)
{
 for(int error=0;error<2;error++) {
  struct avd_ctx ctx;
  init_params();init_ctx(&ctx);
  params.tile_info.tile_cols=2;params.tile_info.tile_rows=2;
  for(int i=0;i<2;i++) {
   params.tile_info.width_in_sbs_minus_1[i]=0;
   params.tile_info.height_in_sbs_minus_1[i]=0;
  }
  for(int i=0;i<4;i++)tiles[i]=(struct v4l2_ctrl_av1_tile_group_entry){.tile_offset=i*128,.tile_size=128,.tile_col=i%2,.tile_row=i/2};
  grain.flags=V4L2_AV1_FILM_GRAIN_FLAG_APPLY_GRAIN;
  ctx.next_src=&sources[0];ctx.next_dst=&buffers[0].base.vb;
  legacy_controls_setup=legacy_controls_complete=0;
  legacy_submit_error=error?-EIO:0;
  assert(avd_av1_fmt_ops.run&&avd_av1_fmt_ops.done&&avd_av1_fmt_ops.submit);
  assert(avd_av1_fmt_ops.run(&ctx)==legacy_submit_error);
  assert(legacy_controls_setup==1&&legacy_controls_complete==1);
  struct avd_av1_ctx *av1=ctx.priv;
  assert(legacy_sent_words==av1->legacy_frame.prog_words);
  assert(!memcmp(legacy_sent,av1->legacy_prog.cpu,legacy_sent_words*sizeof(u32)));
  if(!error) {
   assert(av1->legacy_frame.codec_priv&&av1->active_slots==1);
   assert(avd_av1_fmt_ops.pp_kicks(&ctx)==4);
   avd_av1_fmt_ops.submit(&ctx);assert(legacy_kicks==4);
  }
  avd_av1_fmt_ops.done(&ctx,ctx.next_src,ctx.next_dst,error?VB2_BUF_STATE_ERROR:VB2_BUF_STATE_DONE);
  assert(!av1->legacy_frame.codec_priv&&!av1->active_slots);
  /* Serial decode keeps reference auxiliary state through completion. */
  if(!error) {
   params.frame_type=V4L2_AV1_INTER_FRAME;params.primary_ref_frame=0;
   params.reference_frame_ts[0]=1000;ctx.next_src=&sources[1];ctx.next_dst=&buffers[1].base.vb;
   assert(!avd_av1_fmt_ops.run(&ctx));
   assert(av1->legacy_frame.dep==AVD_DEP_PREV_DONE);
   avd_av1_fmt_ops.done(&ctx,ctx.next_src,ctx.next_dst,VB2_BUF_STATE_DONE);
  }
  avd_av1_stop(&ctx);assert(!allocations);
 }
 legacy_submit_error=0;
 puts("AV1 legacy transport: multi-tile grain-less decode, PP kicks, CDF reuse, submit failure cleanup: PASS");
}

static void source_timestamp_and_failed_producer(void)
{
 for(int failed=0;failed<2;failed++) {
  struct avd_ctx ctx;struct avd_frame f[2];struct avd_buf prog[2]={0};
  init_params();init_ctx(&ctx);
  for(int i=0;i<2;i++){assert(!avd_buf_alloc(&device,&prog[i],4096));f[i]=make_frame(i,i,&prog[i]);}
  buffers[0].base.vb.vb2_buf.timestamp=0;
  assert(!avd_av1_prepare(&ctx,&f[0]));
  struct avd_av1_ctx *av1=ctx.priv;
  assert(av1->prev_timestamp==1000);
  assert(avd_av1_find_ref(&ctx,0,1000));
  /* Model core's metadata copy after prepare, before the next request. */
  buffers[0].base.vb.vb2_buf.timestamp=1000;
  params.frame_type=V4L2_AV1_INTER_FRAME;params.primary_ref_frame=0;
  params.reference_frame_ts[0]=1000;
  if(failed)avd_av1_retire(&ctx,&f[0],VB2_BUF_STATE_ERROR);
  int rc=avd_av1_prepare(&ctx,&f[1]);
  assert(rc==(failed?-EIO:0));
  if(!failed) {
   assert(f[1].dep==AVD_DEP_PREV_DONE);
   avd_av1_retire(&ctx,&f[0],VB2_BUF_STATE_DONE);
   avd_av1_retire(&ctx,&f[1],VB2_BUF_STATE_DONE);
  }
  avd_av1_stop(&ctx);
  for(int i=0;i<2;i++)avd_buf_free(&device,&prog[i]);
  assert(!allocations);
 }
 puts("AV1 reference publication: source timestamp and failed producer: PASS");
}

static void backend_selection(void)
{
 struct avd_ctx ctx={.dev=&device};
 struct avd_coded_fmt_ops ops=avd_av1_fmt_ops;
 struct avd_coded_fmt_desc desc={.ops=&ops,.frame_ops=&avd_av1_frame_ops};
 ctx.coded_fmt_desc=&desc;
 for(int apple=0;apple<2;apple++)for(int legacy=0;legacy<2;legacy++) {
  device.apple=apple;ops.run=legacy?avd_av1_run:NULL;
  assert(avd_get_frame_ops(&ctx)==((apple||!legacy)?&avd_av1_frame_ops:NULL));
  assert(desc.frame_ops==&avd_av1_frame_ops);
 }
 device.apple=false;
 puts("AV1 backend selection: Apple frame_ops and open legacy fallback: PASS");
}

static void segmentation_mi_size(void)
{
 const unsigned int widths[]={126,120,119};
 for(unsigned int w=0;w<3;w++) {
  struct avd_ctx ctx;struct avd_frame f[2];struct avd_buf prog[2]={0};
  init_params();init_ctx(&ctx);
  for(int i=0;i<2;i++){assert(!avd_buf_alloc(&device,&prog[i],4096));f[i]=make_frame(i,i,&prog[i]);}
  params.segmentation.flags=V4L2_AV1_SEGMENTATION_FLAG_ENABLED|V4L2_AV1_SEGMENTATION_FLAG_UPDATE_MAP;
  assert(!avd_av1_prepare(&ctx,&f[0]));
  struct avd_av1_run *a=f[0].codec_priv;
  params.frame_type=V4L2_AV1_INTER_FRAME;params.primary_ref_frame=0;
  params.reference_frame_ts[0]=1000;
  params.flags=V4L2_AV1_FRAME_FLAG_DISABLE_FRAME_END_UPDATE_CDF;
  params.segmentation.flags=V4L2_AV1_SEGMENTATION_FLAG_ENABLED;
  params.frame_width_minus_1=widths[w];params.upscaled_width=widths[w]+1;
  assert(!avd_av1_prepare(&ctx,&f[1]));
  struct avd_av1_run *b=f[1].codec_priv;
  assert((b->seg_in==a->seg_out)==(widths[w]>=120));
  avd_av1_retire(&ctx,&f[0],VB2_BUF_STATE_DONE);
  avd_av1_retire(&ctx,&f[1],VB2_BUF_STATE_DONE);
  avd_av1_stop(&ctx);
  for(int i=0;i<2;i++)avd_buf_free(&device,&prog[i]);
  assert(!allocations);
 }
 puts("AV1 segmentation: inherit only for equal MiCols/MiRows: PASS");
}

int main(void)
{
	segmentation_mi_size();backend_selection();source_timestamp_and_failed_producer();legacy_transport();program_parity();dependency_table();sixteen_slots();cdf_chain();reference_reuse_and_segmentation();allocation_failures();bounds_and_fp();
	puts("AV1 host tests: dependency table, 16 slots, CDF inheritance, allocation failures, bounds, Apple refusal: PASS");
	return 0;
}
