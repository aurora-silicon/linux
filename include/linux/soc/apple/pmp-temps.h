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

static inline int apple_pmp_temp_sample(void __iomem *sram, unsigned int index,
					long *value)
{
	const struct apple_pmp_temp_record *record = &apple_pmp_temp_records[index];
	u8 first[PMP_TEMP_RECORD_SIZE], second[PMP_TEMP_RECORD_SIZE];
	unsigned int attempt;

	for (attempt = 0; attempt < PMP_TEMP_READ_ATTEMPTS; attempt++) {
		memcpy_fromio(first, sram + record->offset, sizeof(first));
		memcpy_fromio(second, sram + record->offset, sizeof(second));
		if (!memcmp(first, second, sizeof(first)))
			return apple_pmp_temp_decode(first, record->name, value);
	}

	return -EAGAIN;
}

static inline int apple_pmp_temp_hotspot(void __iomem *sram, long *value)
{
	bool found = false;
	long hottest = 0, sample;
	unsigned int i;
	int ret;

	for (i = 0; i < ARRAY_SIZE(apple_pmp_temp_records); i++) {
		ret = apple_pmp_temp_sample(sram, i, &sample);
		if (ret == -ENODATA)
			continue;
		if (ret)
			return ret;
		if (!found || sample > hottest)
			hottest = sample;
		found = true;
	}

	if (!found)
		return -ENODATA;
	*value = hottest;
	return 0;
}

#endif
