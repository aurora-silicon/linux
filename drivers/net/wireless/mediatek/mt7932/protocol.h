/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef MT7932_PROTOCOL_H
#define MT7932_PROTOCOL_H

#include <linux/types.h>
#include <linux/string.h>
#include <linux/errno.h>
#include <linux/unaligned.h>

#define MT7932_ENVELOPE 64
#define MT7932_RX_CAPACITY 0x930

/* Byte construction also supports host tests without packed-structure casts. */
static inline int mt7932_envelope(u8 *out, size_t capacity, u8 cid, u8 ext,
				 u8 seq, bool runtime, bool set, bool reply,
				 const void *payload, size_t length)
{
	size_t total = MT7932_ENVELOPE + length;

	if (!seq || length > 8192 - MT7932_ENVELOPE || total > capacity)
		return -EINVAL;
	memset(out, 0, MT7932_ENVELOPE);
	put_unaligned_le32(total | ((cid ? 2U : 3U) << 23), out);
	put_unaligned_le32(0x10000, out + 4);
	put_unaligned_le16(total - 32, out + 32);
	out[36] = cid;
	out[37] = 0xa0;
	out[38] = cid == 0 || (runtime && set);
	out[39] = seq;
	out[41] = ext;
	out[43] = runtime && reply;
	if (length)
		memcpy(out + MT7932_ENVELOPE, payload, length);
	return total;
}

struct mt7932_event {
	u8 eid, seq, option;
	const u8 *packet;
	size_t length;
};

static inline int mt7932_event_parse(const u8 *packet, size_t length,
				      struct mt7932_event *event)
{
	u32 word;
	size_t outer, inner;

	if (length < 32 || length > MT7932_RX_CAPACITY)
		return -EMSGSIZE;
	word = get_unaligned_le32(packet);
	outer = word & 0xffff;
	if (outer < 32 || outer > length)
		return -EMSGSIZE;
	if ((word >> 27) != 7 || ((word >> 16) & 15) == 1)
		return -ENOMSG;
	inner = get_unaligned_le16(packet + 24);
	if (inner < 8 || inner > outer - 24)
		return -EMSGSIZE;
	event->eid = packet[28];
	event->seq = packet[29];
	event->option = packet[30];
	event->packet = packet;
	event->length = inner + 24;
	return 0;
}

static inline bool mt7932_event_matches(const struct mt7932_event *e, u8 seq)
{
	return seq && e->seq == seq && !(e->option & 4);
}

#endif
