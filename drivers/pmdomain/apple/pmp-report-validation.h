/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef _APPLE_PMP_REPORT_VALIDATION_H
#define _APPLE_PMP_REPORT_VALIDATION_H

#include <linux/bitops.h>
#include <linux/errno.h>
#include <linux/types.h>
#include <linux/unaligned.h>

#define PMP_DVFS_VALID BIT_ULL(61)
/* Main (default) request level: bits 4:0. */
#define PMP_DVFS_MAIN_LEVEL_MAX		0x1f
/* Request classes 0x21..0x25: four-bit lanes from bit 32 upwards. */
#define PMP_DVFS_CLASS_FIRST		0x21
#define PMP_DVFS_CLASS_LAST		0x25
#define PMP_DVFS_CLASS_COUNT		(PMP_DVFS_CLASS_LAST - PMP_DVFS_CLASS_FIRST + 1)
#define PMP_DVFS_CLASS_LEVEL_MAX	0xf
#define PMP_DVFS_CLASS_SHIFT		32
#define PMP_DVFS_CLASS_BITS		4
/* <class level> pairs, each class at most once */
#define PMP_DVFS_CLASS_CELLS_MAX	(2 * PMP_DVFS_CLASS_COUNT)

#define PMP_SOC_DEVICE_SIZE	0x7c
#define PMP_SOC_DEVICE_ACK	BIT(1)
#define PMP_GPU_EFFORT_MAX	100
#define PMP_PTD_RANGE_SIZE	32
#define PMP_PTD_RANGE_REQUEST	10
#define PMP_PTD_RANGE_ACK	11

/*
 * The request word for a PMP that is starting: requests made through entries,
 * and every other bit as it was found.
 */
static inline u64 apple_pmp_request_word(u64 found, u64 known, u64 shadow)
{
	return (found & ~known) | shadow;
}

/*
 * A complete SOC-DEV-DVFS request word: the valid bit, an optional main level
 * in bits 4:0, and zero or more <request-class level> pairs. Classes 0x21
 * through 0x25 occupy the four-bit lanes at bits 35:32 through 51:48.
 */
static inline bool apple_pmp_dvfs_word(u32 main_level, const u32 *class_levels,
				       size_t cells, u64 *word)
{
	u32 classes = 0;
	u64 value;
	size_t i;

	if (!word || main_level > PMP_DVFS_MAIN_LEVEL_MAX || cells % 2 ||
	    (cells && !class_levels) || cells > PMP_DVFS_CLASS_CELLS_MAX)
		return false;
	value = PMP_DVFS_VALID | main_level;
	for (i = 0; i < cells; i += 2) {
		u32 request_class = class_levels[i];
		u32 level = class_levels[i + 1];
		u32 index, shift;

		if (request_class < PMP_DVFS_CLASS_FIRST ||
		    request_class > PMP_DVFS_CLASS_LAST ||
		    level > PMP_DVFS_CLASS_LEVEL_MAX)
			return false;
		index = request_class - PMP_DVFS_CLASS_FIRST;
		if (classes & BIT(index))
			return false;
		classes |= BIT(index);
		shift = PMP_DVFS_CLASS_SHIFT + PMP_DVFS_CLASS_BITS * index;
		value |= (u64)level << shift;
	}
	*word = value;
	return true;
}

/* The GPU lifetime-control effort entry holds an integer percent. */
static inline int apple_pmp_gpu_effort_percent(u64 word)
{
	if (word > PMP_GPU_EFFORT_MAX)
		return -EPROTO;
	return word;
}

/* The PTD request read, request update and acknowledgment apertures. */
static inline bool apple_pmp_ranges_valid(const u8 *table, size_t len,
					  u32 request_read, u32 request_write,
					 u32 actual)
{
	unsigned int found = 0;
	size_t pos;

	if (!table || !len || len % PMP_PTD_RANGE_SIZE)
		return false;
	for (pos = 0; pos < len; pos += PMP_PTD_RANGE_SIZE) {
		u32 id = get_unaligned_le32(table + pos);
		u64 base = get_unaligned_le32(table + pos + 4);
		u32 count = get_unaligned_le32(table + pos + 8);

		if (id == PMP_PTD_RANGE_REQUEST) {
			if ((found & BIT(0)) || !count || base * 16 != request_read ||
			    0x10000 + base * 8 != request_write)
				return false;
			found |= BIT(0);
		} else if (id == PMP_PTD_RANGE_ACK) {
			if ((found & BIT(1)) || !count || base * 16 != actual)
				return false;
			found |= BIT(1);
		}
	}
	return found == (BIT(0) | BIT(1));
}

/* Request bit N names device ID N + 1, independent of table order. */
static inline bool apple_pmp_devices_valid(const u8 *table, size_t len,
					   u64 seed, u64 ack, bool ordered)
{
	u64 found = 0;
	size_t pos;

	if (!table || !len || len % PMP_SOC_DEVICE_SIZE || (ack & ~seed))
		return false;
	for (pos = 0; pos < len; pos += PMP_SOC_DEVICE_SIZE) {
		const u8 *dev = table + pos;
		u32 id = get_unaligned_le32(dev);
		u64 bit;

		if (!id || id > 64)
			continue;
		bit = BIT_ULL(id - 1);
		if (!(seed & bit))
			continue;
		if ((found & bit) || (ordered && id != pos / PMP_SOC_DEVICE_SIZE + 1) ||
		    !!(get_unaligned_le32(dev + 8) & PMP_SOC_DEVICE_ACK) !=
		    !!(ack & bit))
			return false;
		found |= bit;
	}
	return found == seed;
}

/*
 * The index of the soc-device record that holds device ID @id, or -ENOENT if
 * no record or more than one does. Where the PTD request bits are indexed by
 * soc-device record (T8122), the device's bit is that index, not ID - 1.
 */
static inline int apple_pmp_record_of_id(const u8 *table, size_t len, u32 id)
{
	int found = -ENOENT;
	size_t pos;

	if (!table || !len || len % PMP_SOC_DEVICE_SIZE || !id)
		return -ENOENT;
	for (pos = 0; pos < len; pos += PMP_SOC_DEVICE_SIZE) {
		if (get_unaligned_le32(table + pos) != id)
			continue;
		if (found >= 0)
			return -ENOENT;
		found = pos / PMP_SOC_DEVICE_SIZE;
	}
	return found;
}

/*
 * Request bit N names soc-device record N. Each seeded record must exist and
 * be acknowledged exactly when its flags say so.
 */
static inline bool apple_pmp_records_valid(const u8 *table, size_t len,
					   u64 seed, u64 ack)
{
	u64 bits = seed;

	if (!table || !len || len % PMP_SOC_DEVICE_SIZE || (ack & ~seed))
		return false;
	while (bits) {
		unsigned int n = __ffs64(bits);
		const u8 *dev;

		bits &= bits - 1;
		if ((size_t)(n + 1) * PMP_SOC_DEVICE_SIZE > len)
			return false;
		dev = table + n * PMP_SOC_DEVICE_SIZE;
		if (!get_unaligned_le32(dev) ||
		    !!(get_unaligned_le32(dev + 8) & PMP_SOC_DEVICE_ACK) !=
		    !!(ack & BIT_ULL(n)))
			return false;
	}
	return true;
}

/* Packed integer arguments are validated before the PMP driver patches them. */
static inline bool apple_pmp_bootargs_valid(const u8 *args, size_t len)
{
	unsigned int found = 0;
	size_t pos = 0;

	if (!args || !len)
		return false;
	while (pos < len) {
		u32 key, size;

		if (len - pos < 8)
			return false;
		key = get_unaligned_le32(args + pos);
		size = get_unaligned_le32(args + pos + 4);
		pos += 8;
		if (size > len - pos)
			return false;
		if (key == 0x42444944 || key == 0x44564944) { /* BDID or DVID */
			if (!size || size > 8)
				return false;
			found |= key == 0x42444944 ? BIT(0) : BIT(1);
		}
		pos += size;
	}
	return found == (BIT(0) | BIT(1));
}

#endif
