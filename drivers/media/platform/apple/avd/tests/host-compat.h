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
struct avd_variant { unsigned int quirks; unsigned char revision; };
struct avd_dev { const struct avd_variant *variant; };
struct vb2_buffer { struct { size_t length; } planes[1]; dma_addr_t dma; };
struct vb2_v4l2_buffer { struct vb2_buffer vb2_buf; };
struct avd_comp { u32 size; size_t start_offset; u32 offsets[4]; };
struct avd_decoded_buffer { struct { struct vb2_v4l2_buffer vb; } base; struct avd_comp comp; };
struct avd_segment { size_t num; u32 instructions[AVD_MAX_INST]; };
struct avd_job { size_t num; struct avd_segment *segments; };
struct avd_ctx {
 struct avd_dev *dev; void *priv; bool decomp; struct avd_comp comp; struct avd_job job;
 struct { struct { struct { int q; } cap_q_ctx; } *m2m_ctx; } fh;
 struct { struct { struct { u32 width,height; struct { u32 bytesperline; } plane_fmt[1]; } pix_mp; } fmt; } decoded_fmt;
};
struct avd_run {
 struct { struct vb2_v4l2_buffer *src, *dst; } bufs;
 dma_addr_t coded_in, y_out, uv_out, comp_out;
};
struct avd_frame {
 unsigned int idx; struct avd_buf *prog, *fp; size_t prog_words; u32 cmd[24];
};
enum { AVD_CODEC_H264 = 1 };
static struct avd_decoded_buffer refs[16];
#define vb2_to_avd_decoded_buf(vb) ((struct avd_decoded_buffer *)(vb))
#define vb2_dma_contig_plane_dma_addr(vb, p) ((vb)->dma)
#define avd_get_ref_buf(ctx,dst,ts) (&refs[(ts)])
#define vb2_find_buffer(q,ts) (&refs[(ts)].base.vb.vb2_buf)
#define fmt_width(ctx) ((ctx)->decoded_fmt.fmt.pix_mp.width)
#define fmt_height(ctx) ((ctx)->decoded_fmt.fmt.pix_mp.height)
static inline void put_unaligned_le32(u32 x, void *p) { memcpy(p, &x, 4); }
static inline u32 get_unaligned_le32(const void *p) { u32 x; memcpy(&x,p,4);return x; }
static inline u16 get_unaligned_le16(const void *p) { u16 x; memcpy(&x,p,2);return x; }
