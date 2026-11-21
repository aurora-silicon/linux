/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef MT7932_FIRMWARE_H
#define MT7932_FIRMWARE_H

#include "protocol.h"
#include "bandwidth.h"

struct mt7932_region {
	u32 offset, length, target, mode;
	u8 features;
};

/* Source-selected defaults, not regulatory limits or permission to transmit. */
static inline void mt7932_basic_config(u8 body[24])
{
	memset(body, 0, 24);
	body[3] = MT7932_MAX_5G_BW;
	body[12] = 1;
	body[14] = 2;
	body[15] = 3;
	put_unaligned_le16(0x2000, body + 16);
	body[20] = 2;
}

static inline void mt7932_mlme_config(u8 body[4])
{
	body[0] = 7;
	body[1] = 4;
	body[2] = 2;
	body[3] = 0;
}

/* Normal band0 MAC initialization, legacy ED/3e SET with a reply waiter. */
static inline void mt7932_rts_config(u8 body[12])
{
	memset(body, 0, 12);
	body[0] = 1;
	put_unaligned_le32(2347, body + 4);
	put_unaligned_le32(2, body + 8);
}

static inline int mt7932_patch_region(const u8 *data, size_t size, unsigned int index,
				      struct mt7932_region *region)
{
	u32 count, kind, bytes, security;
	const u8 *record;

	if (size < 96)
		return -EINVAL;
	count = get_unaligned_be32(data + 44);
	if (!count || count > 4096 || count > (size - 96) / 64 || index >= count)
		return -EINVAL;
	record = data + 96 + index * 64;
	kind = get_unaligned_be32(record);
	region->offset = get_unaligned_be32(record + 4);
	bytes = get_unaligned_be32(record + 8);
	region->target = get_unaligned_be32(record + 12);
	region->length = get_unaligned_be32(record + 16);
	security = get_unaligned_be32(record + 20);
	if ((kind & 0xffff) != 2 || region->offset < 96 + count * 64 ||
	    region->offset > size || bytes > size - region->offset ||
	    !region->length || region->length > bytes ||
	    region->length - 1 > (u32)~0U - region->target)
		return -EINVAL;
	region->features = 0;
	region->mode = 1U << 31;
	if (security != (u32)~0U) {
		switch (security >> 24) {
		case 0:
			break;
		case 1:
			if ((security & 0xff) > 3)
				return -EINVAL;
			region->mode |= 9 | ((security & 3) << 1);
			break;
		case 2:
			region->mode |= 0x49;
			break;
		default:
			return -EOPNOTSUPP;
		}
	}
	return count;
}

static inline int mt7932_ram_region(const u8 *data, size_t size, unsigned int index,
				    struct mt7932_region *region)
{
	size_t metadata, cursor = 0;
	unsigned int count, i;
	const u8 *record;
	u32 length;

	if (size < 36)
		return -EINVAL;
	count = data[size - 34];
	if (!count || count > (size - 36) / 40 || index >= count)
		return -EINVAL;
	metadata = size - 36 - count * 40;
	for (i = 0; i <= index; i++) {
		record = data + metadata + i * 40;
		length = get_unaligned_le32(record + 20);
		if (!length || cursor > metadata || length > metadata - cursor)
			return -EINVAL;
		if (i != index)
			cursor += length;
	}
	if (cursor > (u32)~0U)
		return -EINVAL;
	region->offset = cursor;
	region->length = length;
	region->target = get_unaligned_le32(record + 16);
	region->features = record[24];
	if (length - 1 > (u32)~0U - region->target)
		return -EINVAL;
	region->mode = 1U << 31;
	if (region->features & 1)
		region->mode |= 9 | (region->features & 6);
	if (region->features & 16)
		region->mode |= 64;
	return count;
}

/*
 * The station connect request grew by twelve bytes in RAM images built from
 * July 2026: a 32-bit secure-roaming group bitmap and eight reserved bytes
 * follow the earlier 856-byte body, whose fields did not move. Earlier images
 * do not start an association for the longer request, nor later ones for the
 * shorter. The connac trailer's build date tells them apart before download:
 * chip, ECO, region count, format version and flags, two reserved bytes, a
 * ten-byte label, then the fifteen-byte build date and the CRC.
 */
#define MT7932_JOIN_BODY		856
#define MT7932_JOIN_BODY_SECROAM	868
#define MT7932_JOIN_BODY_MAX		MT7932_JOIN_BODY_SECROAM

static inline int mt7932_join_length(const u8 *data, size_t size)
{
	static const char secroam[] = "20260701";
	const u8 *date;
	unsigned int i;

	if (size < 36)
		return -EINVAL;
	date = data + size - 19;
	for (i = 0; i < 14; i++)
		if (!isdigit(date[i]))
			return -EINVAL;
	if (date[14])
		return -EINVAL;
	return memcmp(date, secroam, 8) < 0 ? MT7932_JOIN_BODY : MT7932_JOIN_BODY_SECROAM;
}

/* Validate the entire capability body before consumers use any individual TLV. */
static inline int mt7932_capabilities(const u8 *body, size_t length)
{
	unsigned int count, i;
	size_t offset = 4;

	if (length < 4)
		return -EMSGSIZE;
	count = get_unaligned_le16(body);
	for (i = 0; i < count; i++) {
		u32 type, size, minimum = 0;

		if (length - offset < 8)
			return -EMSGSIZE;
		type = get_unaligned_le32(body + offset);
		size = get_unaligned_le32(body + offset + 4);
		offset += 8;
		if (size > length - offset)
			return -EMSGSIZE;
		switch (type) {
		case 7: minimum = 6; break;
		case 8: minimum = 12; break;
		case 0x18: minimum = 4; break;
		/* Stock W7932_2 returns four bytes; keep this capability opaque. */
		case 0x20: minimum = 4; break;
		}
		if (size < minimum)
			return -EMSGSIZE;
		if (type == 8 && (!body[offset + 4] || body[offset + 4] > 4))
			return -EINVAL;
		offset += size;
	}
	return offset == length ? (int)count : -EPROTO;
}
#endif
