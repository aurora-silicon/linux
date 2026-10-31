/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef AVD_OPEN_PROTOCOL_H_
#define AVD_OPEN_PROTOCOL_H_

#include <linux/types.h>
#include <linux/errno.h>

#define AVD_OPEN_MAGIC 0x41565131U
#define AVD_OPEN_VERSION 2U
#define AVD_OPEN_INFO 0x7fc0U
#define AVD_OPEN_RING 0x8000U
#define AVD_OPEN_DESC_BYTES 64U
#define AVD_OPEN_PACKET_WORDS 256U
#define AVD_OPEN_PACKET_BYTES 1028U
#define AVD_OPEN_NOTIFY 0x54530000U
#define AVD_OPEN_FATAL 0x5452ffffU

#define AVD_OPEN_MULTI_INFO 0x7f80U
#define AVD_OPEN_MULTI_MAGIC 0x41564d31U
#define AVD_OPEN_MULTI_VERSION 1U
#define AVD_OPEN_CAP_MULTI_VP (1U << 31)
#define AVD_OPEN_FLAG_MULTI_VP (1U << 31)
#define AVD_OPEN_CONTEXT_MASK 0x3ffU
#define AVD_OPEN_CONTEXT_DEP (1U << 31)
#define AVD_OPEN_FLAG_SRAM 1U
#define AVD_OPEN_FEAT_SRAM_RING 1U
#define AVD_OPEN_FEAT_MULTI_KICK 2U

/* ABI 1 wire fields stay unchanged; the extension uses reserved words. */
struct avd_open_desc {
	__le32 magic, tag, codec, vp_slot;
	__le32 program_lo, program_hi, program_words, pp_kick;
	__le32 state, status, completion_tag;
	union {
		__le32 reserved[5];
		struct { __le32 flags, pp_extra, pp_cont, tail[2]; };
	};
};

/* The root capability prevents stale extension SRAM from an old image. */
static inline bool avd_open_has_multi(u32 capabilities, const u32 multi[7])
{
	return (capabilities & AVD_OPEN_CAP_MULTI_VP) &&
		multi[0] == AVD_OPEN_MULTI_MAGIC && multi[1] == AVD_OPEN_MULTI_VERSION &&
		(multi[6] & AVD_OPEN_FLAG_MULTI_VP);
}

/* A power-of-two ring keeps count-to-slot mapping valid across u32 wrap. */
static inline int avd_open_ring_chunk(u32 wp, u32 rp, u32 ring, u32 remaining, u32 *chunk)
{
	u32 used = wp - rp, space, pos;

	if (!ring || (ring & (ring - 1)) || used > ring)
		return -EIO;
	space = ring - used;
	pos = wp & (ring - 1);
	*chunk = space < remaining ? space : remaining;
	if (*chunk > ring - pos)
		*chunk = ring - pos;
	return 0;
}

static inline bool avd_open_completion(u32 word, u32 tag)
{
	return (word & 0xffff0000U) == AVD_OPEN_NOTIFY &&
		(word & 0xffffU) == (tag & 0xffffU);
}
#endif
