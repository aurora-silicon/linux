/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef _LINUX_SOC_APPLE_PMP_TEMPS_H
#define _LINUX_SOC_APPLE_PMP_TEMPS_H

#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/unaligned.h>

/* Read-only T8132 firmware records; shared by hwmon and thermal clients. */
#define PMP_TEMP_SRAM_BASE	0x380500000ULL
#define PMP_TEMP_SRAM_SIZE	0xc0000
#define PMP_TEMP_RECORD_SIZE	0x44
#define PMP_TEMP_SAMPLE_OFFSET	0x34
#define PMP_TEMP_VALID_OFFSET	0x40
#define PMP_TEMP_READ_ATTEMPTS	20
#define PMP_TEMP_CHANNELS	6

/* Firmware identifiers do not establish the physical placement of sensors. */
static const struct apple_pmp_temp_record {
	const char *name;
	const char *label;
	u32 offset;
} apple_pmp_temp_records[] = {
	{ "ta000m", "Ta000m", 0x68624 },
	{ "te000m", "Te000m", 0x686c8 },
	{ "te001m", "Te001m", 0x6876c },
	{ "tp000m", "Tp000m", 0x68810 },
	{ "tp001m", "Tp001m", 0x688b4 },
	/* No individual channel: include only valid samples in the hotspot. */
	{ "tp002m", "Tp002m", 0x68958 },
};

static inline int apple_pmp_temp_decode(const u8 *record, const char *name, long *value)
{
	s32 sample;

	if (memcmp(record, name, strlen(name) + 1))
		return -EIO;
	if (get_unaligned_le32(record + PMP_TEMP_VALID_OFFSET) != 1)
		return -ENODATA;

	sample = (s32)get_unaligned_le32(record + PMP_TEMP_SAMPLE_OFFSET);
	if (sample < -2560 || sample > 9600)
		return -ERANGE;

	*value = DIV_ROUND_CLOSEST((s64)sample * 1000, 64);
	return 0;
}

/*
 * The records live in the firmware's heap, so where they are depends on the
 * firmware build: the offsets above are those of the 26.6.2 (25G83) PMP
 * firmware. A map holds each record's offset for the running firmware.
 */
struct apple_pmp_temp_map {
	u32 offset[PMP_TEMP_CHANNELS];
};

static inline void apple_pmp_temp_map_default(struct apple_pmp_temp_map *map)
{
	unsigned int i;

	for (i = 0; i < PMP_TEMP_CHANNELS; i++)
		map->offset[i] = apple_pmp_temp_records[i].offset;
}

/* Distance between consecutive records in the 25G83 firmware's array. */
#define PMP_TEMP_RECORD_STRIDE	0xa4

/* A record with this name and a validity word of 0 or 1. */
static inline bool apple_pmp_temp_record_plausible(const u8 *record, const char *name)
{
	return !memcmp(record, name, strlen(name) + 1) &&
	       get_unaligned_le32(record + PMP_TEMP_VALID_OFFSET) <= 1;
}

/**
 * apple_pmp_temp_locate() - find the temperature records of the running firmware
 * @sram: the PMP SRAM
 * @start: first byte to search, 4-byte aligned
 * @end: end of the search, at most the size of the SRAM mapping
 * @map: filled with the offsets found
 *
 * Every record starts with its NUL-terminated name. Each name must appear at
 * exactly one plausible record; where it appears at several, the one in step
 * with a unique first record (PMP_TEMP_RECORD_STRIDE apart) is taken.
 *
 * Context: process context; reads up to @end - @start bytes of SRAM.
 * Return: 0 with @map filled, -ENOENT if a record is missing, -EEXIST if one
 * is ambiguous, -EINVAL for a bad range. @map is written only on success.
 */
static inline int apple_pmp_temp_locate(void __iomem *sram, size_t start, size_t end,
					struct apple_pmp_temp_map *map)
{
	u32 found[PMP_TEMP_CHANNELS][4];
	unsigned int count[PMP_TEMP_CHANNELS] = {};
	struct apple_pmp_temp_map result;
	u8 record[PMP_TEMP_RECORD_SIZE];
	unsigned int i, j;
	size_t off;

	if ((start & 3) || end < PMP_TEMP_RECORD_SIZE || start > end - PMP_TEMP_RECORD_SIZE)
		return -EINVAL;
	for (off = start; off <= end - PMP_TEMP_RECORD_SIZE; off += 4) {
		u32 word = readl_relaxed(sram + off);

		for (i = 0; i < PMP_TEMP_CHANNELS; i++) {
			const char *name = apple_pmp_temp_records[i].name;

			if (word != get_unaligned_le32(name))
				continue;
			memcpy_fromio(record, sram + off, sizeof(record));
			if (apple_pmp_temp_record_plausible(record, name) &&
			    count[i] < ARRAY_SIZE(found[i]))
				found[i][count[i]++] = off;
		}
	}
	for (i = 0; i < PMP_TEMP_CHANNELS; i++) {
		if (!count[i])
			return -ENOENT;
		if (count[i] == 1) {
			result.offset[i] = found[i][0];
			continue;
		}
		if (!i || count[0] != 1)
			return -EEXIST;
		for (j = 0; j < count[i]; j++)
			if (found[i][j] == result.offset[0] + i * PMP_TEMP_RECORD_STRIDE)
				break;
		if (j == count[i])
			return -EEXIST;
		result.offset[i] = found[i][j];
	}
	*map = result;
	return 0;
}

static inline int apple_pmp_temp_sample_map(void __iomem *sram,
					    const struct apple_pmp_temp_map *map,
					    unsigned int index, long *value)
{
	const struct apple_pmp_temp_record *record = &apple_pmp_temp_records[index];
	u32 offset = READ_ONCE(map->offset[index]);
	u8 first[PMP_TEMP_RECORD_SIZE], second[PMP_TEMP_RECORD_SIZE];
	unsigned int attempt;

	for (attempt = 0; attempt < PMP_TEMP_READ_ATTEMPTS; attempt++) {
		memcpy_fromio(first, sram + offset, sizeof(first));
		memcpy_fromio(second, sram + offset, sizeof(second));
		if (!memcmp(first, second, sizeof(first)))
			return apple_pmp_temp_decode(first, record->name, value);
	}

	return -EAGAIN;
}

static inline int apple_pmp_temp_sample(void __iomem *sram, unsigned int index,
					long *value)
{
	struct apple_pmp_temp_map map;

	apple_pmp_temp_map_default(&map);
	return apple_pmp_temp_sample_map(sram, &map, index, value);
}

static inline int apple_pmp_temp_hotspot_channel_map(void __iomem *sram,
						     const struct apple_pmp_temp_map *map,
						     long *value, int *channel)
{
	bool found = false;
	long hottest = 0, sample;
	unsigned int i;
	int ret;

	*channel = -1;
	for (i = 0; i < ARRAY_SIZE(apple_pmp_temp_records); i++) {
		ret = apple_pmp_temp_sample_map(sram, map, i, &sample);
		if (ret == -ENODATA)
			continue;
		if (ret)
			return ret;
		if (!found || sample > hottest) {
			hottest = sample;
			*channel = i;
		}
		found = true;
	}

	if (!found)
		return -ENODATA;
	*value = hottest;
	return 0;
}

static inline int apple_pmp_temp_hotspot_channel(void __iomem *sram, long *value,
					       int *channel)
{
	struct apple_pmp_temp_map map;

	apple_pmp_temp_map_default(&map);
	return apple_pmp_temp_hotspot_channel_map(sram, &map, value, channel);
}

static inline int apple_pmp_temp_hotspot_map(void __iomem *sram,
					     const struct apple_pmp_temp_map *map,
					     long *value)
{
	int channel;

	return apple_pmp_temp_hotspot_channel_map(sram, map, value, &channel);
}

static inline int apple_pmp_temp_hotspot(void __iomem *sram, long *value)
{
	int channel;

	return apple_pmp_temp_hotspot_channel(sram, value, &channel);
}

#endif
