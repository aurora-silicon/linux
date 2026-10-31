/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#define AVD_ALIGN 256
#define AVD_MAX_INST 512
#define AVD_QUIRK_LSR BIT(0)
enum avd_codec {AVD_CODEC_HEVC,AVD_CODEC_H264,AVD_CODEC_VP9,AVD_CODEC_AV1};
enum avd_image_fmt {AVD_IMG_FMT_ANY,AVD_IMG_FMT_420_8BIT,AVD_IMG_FMT_420_10BIT,AVD_IMG_FMT_422_8BIT,AVD_IMG_FMT_422_10BIT};
enum vb2_buffer_state {VB2_BUF_STATE_DONE, VB2_BUF_STATE_ERROR};
struct vb2_buffer {unsigned int index; u64 timestamp; struct {size_t length;} planes[1];dma_addr_t dma;size_t payload;};
struct vb2_v4l2_buffer {struct vb2_buffer vb2_buf;};
struct avd_comp {u32 size;size_t start_offset;u32 offsets[4];};
struct avd_decoded_buffer {struct {struct vb2_v4l2_buffer vb;} base;struct avd_comp comp;struct {u16 width,height,upscaled_width;u8 bit_depth;enum v4l2_av1_frame_type frame_type;u32 order_hints[8];u8 ref_frame_idx[7];bool intrabc;size_t color_size,seg_size;} av1;};
struct avd_run {struct {struct vb2_v4l2_buffer *src,*dst;} bufs;dma_addr_t coded_in,y_out,uv_out,comp_out;};
struct avd_buf {void *cpu;dma_addr_t addr;size_t size;};
struct avd_segment {size_t num;u32 instructions[AVD_MAX_INST];};
struct avd_job {size_t num;struct avd_segment *segments;};
struct v4l2_pix_format_mplane {u32 width,height;struct {u32 bytesperline,sizeimage;} plane_fmt[1];};
struct v4l2_ctrl {u32 id;unsigned int elems;union {void *p;} p_cur;union {struct v4l2_ctrl_av1_sequence *p_av1_sequence;} p_new;};
struct avd_variant {unsigned int quirks,revision;enum avd_av1_fp_layout av1_fp_layout;};
struct avd_dev {struct avd_variant *variant;bool apple;};
struct avd_ctx {struct avd_dev *dev;const struct avd_coded_fmt_desc *coded_fmt_desc;void *priv;struct {struct {struct v4l2_pix_format_mplane pix_mp;} fmt;} decoded_fmt;struct avd_comp comp;bool decomp;unsigned int max_frames;struct v4l2_ctrl ctrl_hdl[4];struct avd_job job;struct vb2_v4l2_buffer *next_src,*next_dst;};
struct avd_coded_fmt_ops {int (*run)(struct avd_ctx *);void (*done)(struct avd_ctx *,struct vb2_v4l2_buffer *,struct vb2_v4l2_buffer *,enum vb2_buffer_state);void (*submit)(struct avd_ctx *);unsigned int (*pp_kicks)(struct avd_ctx *);void (*adjust_decoded_fmt)(struct avd_ctx *,struct v4l2_pix_format_mplane *);int (*start)(struct avd_ctx *);void (*stop)(struct avd_ctx *);int (*try_ctrl)(struct avd_ctx *,struct v4l2_ctrl *);enum avd_image_fmt (*get_image_fmt)(struct avd_ctx *,struct v4l2_ctrl *);};
static int allocations, fail_after=-1;
static dma_addr_t next_dma=0x10000000;
static int avd_buf_alloc(struct avd_dev *d,struct avd_buf *b,size_t n) {if(fail_after==0)return -ENOMEM;if(fail_after>0)fail_after--;b->cpu=calloc(1,n);if(!b->cpu)return -ENOMEM;b->size=n;b->addr=next_dma;next_dma+=ALIGN(n,256);allocations++;return 0;}
static void avd_buf_free(struct avd_dev *d,struct avd_buf *b) {if(b->cpu){free(b->cpu);allocations--;}memset(b,0,sizeof(*b));}
static struct avd_decoded_buffer buffers[64];
static struct vb2_v4l2_buffer sources[64];
static struct avd_decoded_buffer *vb2_to_avd_decoded_buf(struct vb2_buffer *b) {return container_of(b,struct avd_decoded_buffer,base.vb.vb2_buf);}
static struct avd_decoded_buffer *avd_get_ref_buf(struct avd_ctx *c,struct vb2_v4l2_buffer *dst,u64 ts) {for(int i=0;i<64;i++)if(buffers[i].base.vb.vb2_buf.timestamp==ts)return &buffers[i];return vb2_to_avd_decoded_buf(&dst->vb2_buf);}
static dma_addr_t vb2_dma_contig_plane_dma_addr(struct vb2_buffer *b,int n) {return b->dma;}
static size_t vb2_get_plane_payload(struct vb2_buffer *b,int n) {return b->payload;}
static struct v4l2_ctrl *v4l2_ctrl_find(void *h,u32 id) {struct v4l2_ctrl *p=h;for(int i=0;i<4;i++)if(p[i].id==id)return p+i;return NULL;}
struct avd_av1_cdfs {u8 data[16384];};
static void avd_av1_default_coeff_probs(u32 q,void *p) {memset(p,q,sizeof(struct avd_av1_cdfs));}
static void avd_av1_set_default_cdfs(void *p) {}

static int legacy_submit_error, legacy_controls_setup, legacy_controls_complete;
static unsigned int legacy_kicks;
static u32 legacy_sent[32768];
static size_t legacy_sent_words;
static void avd_run_preamble(struct avd_ctx *c,struct avd_run *r) {
 memset(r,0,sizeof(*r));r->bufs.src=c->next_src;r->bufs.dst=c->next_dst;c->decomp=true;legacy_controls_setup++;
}
static void avd_run_postamble(struct avd_ctx *c,struct avd_run *r) {legacy_controls_complete++;}
static int avd_init_job(struct avd_ctx *c,enum avd_codec codec,unsigned int n) {
 free(c->job.segments);c->job.segments=calloc(n,sizeof(*c->job.segments));c->job.num=0;
 return c->job.segments?0:-ENOMEM;
}
static int avd_submit_job(struct avd_ctx *c);
static void avd_open_av1_submit(struct avd_ctx *c,unsigned int count) {legacy_kicks=count;}

struct avd_coded_fmt_desc {const struct avd_coded_fmt_ops *ops;const struct avd_codec_frame_ops *frame_ops;};
bool avd_backend_is_apple(const struct avd_dev *d) {return d->apple;}
