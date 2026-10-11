// SPDX-License-Identifier: GPL-2.0-only
/* Copyright 2026 Aurora Silicon */

#ifndef APPLE_AFK_V2_H
#define APPLE_AFK_V2_H

#include <linux/errno.h>
#include <linux/types.h>
#include <linux/unaligned.h>

struct epic_hdr_v2 {
	u8 seq;
	u8 flags;
	__le16 channel;
	__le32 length; /* bytes after the first eight transport bytes */
	__le64 timestamp;
	u8 type;
	u8 category; /* report=0, command=1, reply=2 */
	__le16 interface_flags;
	__le32 reserved;
} __packed;

struct epic_cmd_v2 {
	u8 flags; /* bit 0: out-of-band buffer descriptors follow */
	u8 tag;
	__le16 reserved;
	__le32 length; /* command: response capacity; reply: status */
	__le64 rxbuf;
	__le64 txbuf;
	__le32 rxlen;
	__le32 txlen;
} __packed;

/* length excludes the first eight transport bytes. Ignore ring padding. */
static inline int afk_v2_payload_size(const struct epic_hdr_v2 *header,
				      size_t record_size, size_t *payload_size)
{
	u32 length;

	if (record_size < sizeof(*header) || header->category > 2)
		return -EINVAL;
	length = le32_to_cpu(header->length);
	if (length < sizeof(*header) - 8 || length > record_size - 8)
		return -EINVAL;
	*payload_size = length - (sizeof(*header) - 8);
	return 0;
}

static inline u16 afk_command_tag(bool v2, u8 generation, unsigned int slot)
{
	return v2 ? (((generation & 0xf) << 4) | slot) :
		     (((u16)generation << 8) | slot);
}

static inline unsigned int afk_command_slot(bool v2, u16 tag)
{
	return tag & (v2 ? 0xf : 0xff);
}

#endif
