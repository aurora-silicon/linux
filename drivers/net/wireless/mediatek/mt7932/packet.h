/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef MT7932_PACKET_H
#define MT7932_PACKET_H

#include "protocol.h"
#include "channels.h"

struct mt7932_rx_frame {
	const u8 *data;
	size_t length;
	u32 timestamp, rcpi, rxv;
	u16 frame_control, sequence_control, qos_control, wtbl;
	u8 channel, amsdu;
	bool translated, group4, signal_valid, rate_valid, unicast;
};

/* NEO_PACKET_RX_GROUPS_AND_DATA_TXP_CONTRACT. Strict non-monitor subset;
 * descriptor groups are ordered4,1,2,3,5, not by numerical group identifier.
 */
static inline int mt7932_rx_frame(const u8 *packet, size_t size, struct mt7932_rx_frame *out)
{
	u32 w0, w1, w2, w3, w4, kind;
	size_t length, offset = 24, padding;
	unsigned int i;
	static const u8 groups[] = {4, 1, 2, 3, 5};
	static const u8 sizes[] = {16, 16, 8, 8, 72};

	if (size < 24)
		return -EMSGSIZE;
	w0 = get_unaligned_le32(packet);
	length = w0 & 0xffff;
	kind = w0 >> 27;
	if (kind == 7 && ((w0 >> 16) & 15) == 1)
		kind = 8;
	if (kind != 2 && kind != 8)
		return -ENOMSG;
	if (length < 24 || length > size)
		return -EMSGSIZE;
	w1 = get_unaligned_le32(packet + 4);
	w2 = get_unaligned_le32(packet + 8);
	w3 = get_unaligned_le32(packet + 12);
	w4 = get_unaligned_le32(packet + 16);
	/* Word2 bit25 is HDR_TRANS_ERROR, not an integrity error. The source
	 * still parses the original 802.11 frame when translation was not done.
	 */
	if ((w1 & (0x1eU << 24)) || (w2 & (3U << 23)) ||
	    ((w2 & (1U << 13)) && (w1 & (1U << 23))))
		return -EBADMSG;
	if ((w1 & (1U << 15)) && !(w1 & (1U << 13)))
		return -EPROTO;
	memset(out, 0, sizeof(*out));
	out->translated = !!(w2 & (1U << 13));
	out->channel = (w3 >> 8) & 255;
	out->wtbl = w1 & 0x3ff;
	out->unicast = ((w3 >> 16) & 3) == 1;
	out->amsdu = w4 & 3;
	if (!mt7932_channel_band(out->channel))
		return -EOPNOTSUPP;
	if ((out->translated && (w2 & (1U << 27))) || (!out->translated && out->amsdu))
		return -EOPNOTSUPP;
	for (i = 0; i < 5; i++) {
		unsigned int group = groups[i];

		if (!(w1 & (1U << (10 + group))))
			continue;
		if (sizes[i] > length - offset)
			return -EMSGSIZE;
		if (group == 4) {
			out->frame_control = get_unaligned_le16(packet + offset);
			out->sequence_control = get_unaligned_le16(packet + offset + 8);
			out->qos_control = get_unaligned_le16(packet + offset + 10);
			out->group4 = true;
		} else if (group == 2) {
			out->timestamp = get_unaligned_le32(packet + offset);
		} else if (group == 3 || group == 5) {
			if (group == 3) {
				out->rxv = get_unaligned_le32(packet + offset);
				out->rate_valid = true;
			}
			out->rcpi = get_unaligned_le32(packet + offset + (group == 3 ? 4 : 24));
			out->signal_valid = true;
		}
		offset += sizes[i];
	}
	/* Translated fragments need header reconstruction, which this bounded
	 * Ethernet path does not implement. RXD word2 bit27 is not equivalent
	 * to the saved group4 frame-control More Fragments bit.
	 */
	if (out->translated && out->group4 && (out->frame_control & 0x0400))
		return -EOPNOTSUPP;
	padding = 2 * ((w2 >> 14) & 3);
	if (padding > length - offset)
		return -EMSGSIZE;
	offset += padding;
	if (length - offset < (out->translated ? 14 : 24))
		return -EMSGSIZE;
	out->data = packet + offset;
	out->length = length - offset;
	if (!out->translated) {
		out->frame_control = get_unaligned_le16(out->data);
		out->sequence_control = get_unaligned_le16(out->data + 22);
	}
	return 0;
}

static inline int mt7932_rx_signal(const struct mt7932_rx_frame *frame, u8 antenna_mask)
{
	int signal = -128;
	unsigned int i;

	if (!frame->signal_valid || !antenna_mask || (antenna_mask & ~15))
		return -128;
	for (i = 0; i < 4; i++) {
		int value = ((frame->rcpi >> (8 * i)) & 255) / 2 - 110;

		if ((antenna_mask & (1U << i)) && value < 0 && value > signal)
			signal = value;
	}
	return signal;
}

static inline bool mt7932_rx_beacon(const struct mt7932_rx_frame *frame)
{
	size_t offset = 36;
	u16 type = frame->frame_control & 0x00fc;

	if (frame->translated || (type != 0x80 && type != 0x50) || frame->length < 36)
		return false;
	while (offset < frame->length) {
		if (frame->length - offset < 2 || frame->data[offset + 1] > frame->length - offset - 2)
			return false;
		offset += 2 + frame->data[offset + 1];
	}
	return true;
}

/* One linear, protected Ethernet packet. Caller owns token, WTBL and buffers. */
static inline int mt7932_data_header(u8 out[64], const u8 *ethernet, size_t length,
				     u64 dma, unsigned int token, unsigned int wtbl,
				     unsigned int own_mac, unsigned int tid, bool protected_frame)
{
	u32 word1;

	if (length < 14 || length > 4095 || dma >> 35 || token > 0x7fff ||
	    wtbl > 0x3ff || own_mac > 63 || tid > 7)
		return -EINVAL;
	memset(out, 0, 64);
	put_unaligned_le32((16U << 25) | (10U << 16) | (length + 32), out);
	word1 = (1U << 31) | (own_mac << 24) | (tid << 20) | wtbl;
	if (get_unaligned_be16(ethernet + 12) >= 0x600)
		word1 |= 1U << 15;
	put_unaligned_le32(word1, out + 4);
	put_unaligned_le32((30U << 11) | (protected_frame ? 2 : 0), out + 12);
	put_unaligned_le16(token | 0x8000, out + 32);
	put_unaligned_le32(dma, out + 40);
	put_unaligned_le16(length | ((dma >> 32) << 12) | 0x8000, out + 44);
	return 0;
}

struct mt7932_duplicate {
	u16 sequence;
	bool valid, retry_amsdu;
};

/* RX consumer alone owns this association-local cache. */
static inline bool mt7932_rx_duplicate(struct mt7932_duplicate cache[17],
				       const struct mt7932_rx_frame *frame)
{
	struct mt7932_duplicate *entry;
	unsigned int index = 16;
	u16 sequence;
	bool retry;

	if (!frame->group4) {
		cache[16] = (struct mt7932_duplicate) { .valid = true };
		return false;
	}
	if (frame->unicast && (frame->frame_control & 0xfc) == 0x88)
		index = frame->qos_control & 15;
	entry = &cache[index];
	sequence = frame->sequence_control >> 4;
	retry = frame->frame_control & (1U << 11);
	if (retry && entry->valid && entry->sequence == sequence) {
		if (!entry->retry_amsdu)
			return true;
		if (frame->amsdu == 1)
			entry->retry_amsdu = false;
		return false;
	}
	entry->sequence = sequence;
	entry->valid = true;
	entry->retry_amsdu = retry && frame->amsdu == 3;
	return false;
}

/* Validate the complete variable walk before returning any completion IDs. */
static inline int mt7932_tx_free(const u8 *packet, size_t size, u16 *tokens,
				 unsigned int capacity)
{
	size_t length, offset = 8;
	unsigned int version, count, i = 0;
	u32 word;

	if (size < 8)
		return -EMSGSIZE;
	length = get_unaligned_le16(packet);
	if (length < 8 || length > size)
		return -EMSGSIZE;
	word = get_unaligned_le32(packet + 4);
	version = (word >> 16) & 7;
	if (version > 3)
		return -EOPNOTSUPP;
	count = get_unaligned_le16(packet + 2) & (version == 3 ? 0x3ff : 0x7f);
	if (count > capacity)
		return -ENOSPC;
	while (i < count) {
		if (length - offset < (version ? 4 : 2))
			return -EMSGSIZE;
		word = version ? get_unaligned_le32(packet + offset) : get_unaligned_le16(packet + offset);
		offset += version ? 4 : 2;
		if (version == 3 && (word & (1U << 31)))
			continue;
		tokens[i++] = version == 3 ? (word >> 16) & 0x7fff : word & (version == 2 ? 0x7fff : 0xffff);
	}
	return count;
}

#endif
