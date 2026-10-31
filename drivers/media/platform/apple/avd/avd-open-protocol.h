/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef AVD_OPEN_PROTOCOL_H_
#define AVD_OPEN_PROTOCOL_H_

#include <linux/types.h>

#define AVD_OPEN_MAGIC 0x41565131U
#define AVD_OPEN_VERSION 2U
#define AVD_OPEN_INFO 0x7fc0U
#define AVD_OPEN_RING 0x8000U
#define AVD_OPEN_DESC_BYTES 64U
#define AVD_OPEN_PACKET_WORDS 256U
#define AVD_OPEN_PACKET_BYTES 1028U
#define AVD_OPEN_NOTIFY 0x54530000U
#define AVD_OPEN_FATAL 0x5452ffffU

/* Little endian, kept in sync with avd-fw src/queue.h. */
struct avd_open_desc {
	__le32 magic, tag, codec, fifo;
	__le32 program_lo, program_hi, program_words, num_segments;
	__le32 state, status, completion_tag, context, dependency;
	__le32 fifo_lo, fifo_hi, reserved;
};

static inline bool avd_open_completion(u32 word, u32 tag)
{
	return (word & 0xffff0000U) == AVD_OPEN_NOTIFY &&
		(word & 0xffffU) == (tag & 0xffffU);
}
#endif
