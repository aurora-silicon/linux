/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef AVD_VP9_HOST_H_
#define AVD_VP9_HOST_H_
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef uint64_t dma_addr_t;
#define U32_MAX UINT32_MAX
#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
#define V4L2_VP9_FRAME_FLAG_KEY_FRAME 1
#define V4L2_VP9_FRAME_FLAG_INTRA_ONLY 8
#define V4L2_VP9_FRAME_FLAG_REFRESH_FRAME_CTX 0x020
#define V4L2_VP9_FRAME_FLAG_PARALLEL_DEC_MODE 0x040
#define AVD_MAX_FRAMES 16
#define AVD_VP9_PROBS_SIZE 0x774
/* Kept in the same order as the public production enum. */
enum avd_vp9_fp_generation {
	AVD_VP9_FP_UNKNOWN, AVD_VP9_FP_V3, AVD_VP9_FP_BORAGE,
	AVD_VP9_FP_T8112, AVD_VP9_FP_T8140, AVD_VP9_FP_T6000,
};
struct avd_buf { void *cpu; dma_addr_t addr; size_t size; };
struct avd_frame {
	unsigned int idx;
	struct avd_buf *fp, *prog;
	size_t prog_words;
	u32 cmd[24];
};
struct avd_variant { unsigned int vp9_fp_generation; };
struct avd_dev { const struct avd_variant *variant; };
struct avd_ctx { struct avd_dev *dev; };
struct v4l2_ctrl_vp9_frame { u32 flags; u8 tile_cols_log2, tile_rows_log2; };
struct v4l2_ctrl_vp9_compressed_hdr { u8 tx_mode; };
static inline u32 get_unaligned_le32(const void *v)
{
	const u8 *p = v;
	return p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24;
}
static inline u16 get_unaligned_le16(const void *v)
{
	const u8 *p = v;
	return p[0] | (u16)p[1] << 8;
}
static inline void put_unaligned_le32(u32 n, void *v)
{
	u8 *p = v;
	for (unsigned int i = 0; i < 4; i++) p[i] = n >> (i * 8);
}
static inline void put_unaligned_le16(u16 n, void *v)
{
	u8 *p = v;
	p[0] = n;
	p[1] = n >> 8;
}
#endif
