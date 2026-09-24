/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef APPLE_PMP_V2_RESIDENT_H
#define APPLE_PMP_V2_RESIDENT_H

#include <linux/overflow.h>
#include <linux/sizes.h>
#include <linux/unaligned.h>

struct pmp_v2_segment {
	u64 physical, virtual, device;
	u32 size, flags;
};

struct pmp_v2_patch {
	u32 key, value;
	size_t offset;
	bool present;
};

static int pmp_v2_translate(const struct pmp_v2_segment *segments, unsigned int count,
			    u64 address, size_t size, bool writable, u64 *physical)
{
	u64 end;
	unsigned int i;

	if (!size || check_add_overflow(address, (u64)size, &end))
		return -EINVAL;
	for (i = 0; i < count; i++) {
		const struct pmp_v2_segment *s = &segments[i];

		if (s->virtual <= address && end <= s->virtual + s->size) {
			if (writable && (s->flags & 1))
				return -EPERM;
			*physical = s->physical + address - s->virtual;
			return 0;
		}
	}
	return -ERANGE;
}

static int pmp_v2_segments(const void *raw, size_t size, struct pmp_v2_segment *out,
			   unsigned int capacity)
{
	unsigned int i, j, count = size / 32;
	u64 end;

	if (!size || size % 32 || count > capacity)
		return -EINVAL;
	for (i = 0; i < count; i++) {
		const u8 *d = raw + 32 * i;
		struct pmp_v2_segment *s = &out[i];

		s->physical = get_unaligned_le64(d);
		s->virtual = get_unaligned_le64(d + 8);
		s->device = get_unaligned_le64(d + 16);
		s->size = get_unaligned_le32(d + 24);
		s->flags = get_unaligned_le32(d + 28);
		/* Mapping resident segments is not part of this stopped profile. */
		if (!s->size || !(s->flags & BIT(1)) ||
		    check_add_overflow(s->physical, (u64)s->size, &end) ||
		    check_add_overflow(s->virtual, (u64)s->size, &end) ||
		    check_add_overflow(s->device, (u64)s->size, &end))
			return -EINVAL;
		for (j = 0; j < i; j++)
			if ((s->virtual < out[j].virtual + out[j].size &&
			     out[j].virtual < s->virtual + s->size) ||
			    (s->physical < out[j].physical + out[j].size &&
			     out[j].physical < s->physical + s->size))
				return -EINVAL;
	}
	return count;
}

static int pmp_v2_locate(const struct pmp_v2_segment *segments, unsigned int count,
			 int (*read)(void *, u64, void *, size_t), void *cookie,
			 u64 *physical, size_t *size)
{
	static const u32 offsets[] = { 0x20, 0xc0, 0x204, 0xc00, 0x1020, 0x1204, 0x4020, 0x4204 };
	u64 address, base;
	u8 header[64];
	u32 version, field;
	unsigned int i;
	int ret;

	if (!count)
		return -EINVAL;
	base = segments[0].virtual;
	for (i = 0; i < ARRAY_SIZE(offsets); i++) {
		if (check_add_overflow(base, (u64)offsets[i], &address))
			return -EOVERFLOW;
		ret = pmp_v2_translate(segments, count, address, sizeof(header), false, &address);
		if (ret)
			return ret;
		ret = read(cookie, address, header, sizeof(header));
		if (ret)
			return ret;
		version = get_unaligned_le32(header + 4);
		if (get_unaligned_le32(header) != 0x64697575 || (version != 4 && version != 5))
			continue;
		field = version == 4 ? 0x20 : 0x28;
		*size = get_unaligned_le32(header + field + 4);
		if (*size < 8 || *size > SZ_1M ||
		    check_add_overflow(base, (u64)get_unaligned_le32(header + field), &address))
			return -EINVAL;
		return pmp_v2_translate(segments, count, address, *size, true, physical);
	}
	return -ENOENT;
}

/* Populate the complete ledger before allowing any resident write. */
static int pmp_v2_patch_plan(const void *data, size_t size,
			     struct pmp_v2_patch *patches, unsigned int count)
{
	size_t cursor = 0;
	u32 key, length;
	unsigned int i;

	for (i = 0; i < count; i++)
		patches[i].present = false;
	while (cursor < size) {
		if (size - cursor < 8)
			return -EINVAL;
		key = get_unaligned_le32(data + cursor);
		length = get_unaligned_le32(data + cursor + 4);
		cursor += 8;
		if (length > size - cursor)
			return -EINVAL;
		for (i = 0; i < count; i++) {
			if (patches[i].key != key)
				continue;
			if (length != 4 || patches[i].present)
				return -EINVAL;
			patches[i].present = true;
			patches[i].offset = cursor;
		}
		cursor += length;
	}
	return 0;
}

#endif
