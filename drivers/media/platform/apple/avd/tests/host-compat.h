/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <linux/v4l2-controls.h>
#include <linux/videodev2.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef int32_t s32;
typedef uint64_t u64;
typedef uint32_t __le32;
typedef uint64_t dma_addr_t;
#define BIT(n) (1U << (n))
#define GENMASK(h,l) ((UINT32_MAX >> (31-(h))) & (UINT32_MAX << (l)))
#define FIELD_PREP(mask,v) (((u32)(v) << __builtin_ctz(mask)) & (mask))
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#define DIV_ROUND_UP(n,d) (((n)+(d)-1)/(d))
#define cpu_to_le32(x) (x)
#define lower_32_bits(x) ((u32)(x))
#define upper_32_bits(x) ((u32)((x)>>32))
#define U32_MAX UINT32_MAX
#define AVD_QUIRK_LSR BIT(0)
#define AVD_QUIRK_NO_PIPE_STATE BIT(1)
#define AVD_MAX_INST 512
#define AVD_MAX_FRAMES 16
#define AVD_H_ 1
#define AVD_QUEUE_H_ 1
#define VISIBLE_IF_KUNIT
#define dev_info(...) ((void)0)
struct avd_buf { void *cpu; dma_addr_t addr; size_t size; };
struct avd_variant { unsigned int quirks; unsigned char revision; unsigned int h264_fp_layout; };
struct avd_dev { const struct avd_variant *variant; };
struct vb2_buffer { struct { size_t length, data_offset; } planes[1]; dma_addr_t dma; void *data; size_t payload; };
struct vb2_v4l2_buffer { struct vb2_buffer vb2_buf; u32 flags; };
struct avd_comp { u32 size; size_t start_offset; u32 offsets[4]; };
struct avd_decoded_buffer { struct { struct vb2_v4l2_buffer vb; } base; struct avd_comp comp; size_t grey_chroma_offset; bool grey_chroma_10bit; u8 bank; };
struct avd_segment { size_t num; u32 instructions[AVD_MAX_INST]; };
struct avd_job { size_t num; struct avd_segment *segments; };
struct v4l2_ctrl { u32 id; struct { void *p; } p_cur; };
struct v4l2_ctrl_handler { struct v4l2_ctrl ctrls[6]; };
struct avd_ctx {
 struct v4l2_ctrl_handler ctrl_hdl; unsigned int max_frames, image_fmt; struct avd_dev *dev; void *priv; bool decomp; struct avd_comp comp; struct avd_job job;
 struct { struct { struct { int q; } cap_q_ctx; } *m2m_ctx; } fh;
 struct v4l2_format decoded_fmt;
};
struct avd_run {
 struct { struct vb2_v4l2_buffer *src, *dst; } bufs;
 dma_addr_t coded_in, y_out, uv_out, comp_out;
};
struct avd_frame {
 unsigned int idx; struct avd_buf *prog, *fp; size_t prog_words; u32 cmd[24];
 struct vb2_v4l2_buffer *src, *dst; void *codec_priv; u32 frame_num; int dep; dma_addr_t fifo_iova;
};
enum { AVD_CODEC_H264 = 1 };
static struct avd_decoded_buffer refs[16];
#define vb2_to_avd_decoded_buf(vb) ((struct avd_decoded_buffer *)(vb))
#define vb2_dma_contig_plane_dma_addr(vb, p) ((vb)->dma)
#define avd_get_ref_buf(ctx,dst,ts) ((void)(ctx), (void)(dst), &refs[(ts)])
#define vb2_find_buffer(q,ts) (&refs[(ts)].base.vb.vb2_buf)
#define fmt_width(ctx) ((ctx)->decoded_fmt.fmt.pix_mp.width)
#define fmt_height(ctx) ((ctx)->decoded_fmt.fmt.pix_mp.height)
static inline void put_unaligned_le32(u32 x, void *p) { memcpy(p, &x, 4); }
static inline u32 get_unaligned_le32(const void *p) { u32 x; memcpy(&x,p,4);return x; }
static inline u16 get_unaligned_le16(const void *p) { u16 x; memcpy(&x,p,2);return x; }

#define min_t(t,a,b) ((t)(a) < (t)(b) ? (t)(a) : (t)(b))
#define max_t(t,a,b) ((t)(a) > (t)(b) ? (t)(a) : (t)(b))
#define ALIGN(x,a) (((x)+(a)-1)&~((a)-1))
#define PAGE_SIZE 4096
#define PAGE_ALIGN(x) ALIGN(x, PAGE_SIZE)
#define GFP_KERNEL 0
#define kzalloc(s,g) calloc(1,s)
#define kfree(p) free(p)
#define kvfree(p) free(p)
#define kvmalloc_array(n,s,g) malloc((n)*(s))
#define dev_err(...) ((void)0)
#define vb2_plane_size(vb,p) ((vb)->planes[p].length)
#define vb2_plane_vaddr(vb,p) ((vb)->data)
#define vb2_get_plane_payload(vb,p) ((vb)->payload)
#define AVD_DEP_NONE 0
#define AVD_IMG_FMT_420_10BIT 2
#define AVD_IMG_FMT_422_10BIT 4
enum vb2_buffer_state { VB2_BUF_STATE_DONE, VB2_BUF_STATE_ERROR };
struct v4l2_h264_reflist_builder { unsigned int num_valid; s32 cur_pic_order_count; };
static inline void v4l2_h264_init_reflist_builder(struct v4l2_h264_reflist_builder *b,
 const struct v4l2_ctrl_h264_decode_params *d, const struct v4l2_ctrl_h264_sps *s,
 const struct v4l2_h264_dpb_entry *dpb)
{
 (void)s; b->num_valid=0; b->cur_pic_order_count=d->top_field_order_cnt;
 for (int i=0;i<16;i++) b->num_valid += !!(dpb[i].flags & V4L2_H264_DPB_ENTRY_FLAG_VALID);
}
static inline struct v4l2_ctrl *v4l2_ctrl_find(struct v4l2_ctrl_handler *h, u32 id)
{
 for (int i=0;i<6;i++) if (h->ctrls[i].id==id) return &h->ctrls[i];
 return NULL;
}
static bool apple_backend;
static inline bool avd_backend_is_apple(struct avd_dev *d) { (void)d; return apple_backend; }
static inline int avd_init_job(struct avd_ctx *ctx, int codec, size_t segments)
{
 (void)codec; free(ctx->job.segments); ctx->job.num=0;
 ctx->job.segments=calloc(segments,sizeof(*ctx->job.segments));
 return ctx->job.segments ? 0 : -ENOMEM;
}
