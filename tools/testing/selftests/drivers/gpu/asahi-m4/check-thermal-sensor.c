// SPDX-License-Identifier: GPL-2.0-only OR MIT
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <linux/soc/apple/pmp-temps.h>

static void store32(u8 *p, u32 word)
{
	for (unsigned int i = 0; i < 4; i++)
		p[i] = word >> (8 * i);
}

static void selftest(void)
{
	u8 *sram = calloc(1, PMP_TEMP_SRAM_SIZE);
	long mc = 0;
	int channel = -1;

	assert(sram);
	for (unsigned int i = 0; i < PMP_TEMP_CHANNELS; i++) {
		u8 *record = sram + apple_pmp_temp_records[i].offset;

		memcpy(record, apple_pmp_temp_records[i].name, 7);
		store32(record + PMP_TEMP_SAMPLE_OFFSET, (90 + i) * 64);
		store32(record + PMP_TEMP_VALID_OFFSET, 1);
	}
	assert(!apple_pmp_temp_hotspot_channel(sram, &mc, &channel));
	assert(mc == 95000 && channel == 5);
	store32(sram + apple_pmp_temp_records[5].offset + PMP_TEMP_VALID_OFFSET, 0);
	assert(!apple_pmp_temp_hotspot(sram, &mc) && mc == 94000);
	for (unsigned int i = 0; i < PMP_TEMP_CHANNELS; i++)
		store32(sram + apple_pmp_temp_records[i].offset + PMP_TEMP_VALID_OFFSET, 0);
	assert(apple_pmp_temp_hotspot(sram, &mc) == -ENODATA);
	sram[apple_pmp_temp_records[0].offset] = '?';
	assert(apple_pmp_temp_hotspot(sram, &mc) == -EIO);
	free(sram);
}

/* Records moved in a later firmware are found by name. */
static void locate_selftest(void)
{
	u8 *sram = calloc(1, PMP_TEMP_SRAM_SIZE);
	struct apple_pmp_temp_map map, old;
	const u32 shift = 0x1140;
	long mc = 0;
	int channel = -1;

	assert(sram);
	apple_pmp_temp_map_default(&map);
	assert(apple_pmp_temp_locate(sram, 0x3d000, PMP_TEMP_SRAM_SIZE, &map) == -ENOENT);
	for (unsigned int i = 0; i < PMP_TEMP_CHANNELS; i++) {
		u8 *record = sram + apple_pmp_temp_records[i].offset + shift;

		memcpy(record, apple_pmp_temp_records[i].name, 7);
		store32(record + PMP_TEMP_SAMPLE_OFFSET, (80 + i) * 64);
		store32(record + PMP_TEMP_VALID_OFFSET, 1);
	}
	/* The 25G83 offsets no longer hold the records. */
	assert(apple_pmp_temp_hotspot(sram, &mc) == -EIO);
	assert(!apple_pmp_temp_locate(sram, 0x3d000, PMP_TEMP_SRAM_SIZE, &map));
	for (unsigned int i = 0; i < PMP_TEMP_CHANNELS; i++)
		assert(map.offset[i] == apple_pmp_temp_records[i].offset + shift);
	assert(!apple_pmp_temp_hotspot_channel_map(sram, &map, &mc, &channel));
	assert(mc == 85000 && channel == 5);
	/* A name outside a plausible record (validity word > 1) is no candidate. */
	memcpy(sram + 0x40000, "te001m", 7);
	store32(sram + 0x40000 + PMP_TEMP_VALID_OFFSET, 7);
	old = map;
	assert(!apple_pmp_temp_locate(sram, 0x3d000, PMP_TEMP_SRAM_SIZE, &map));
	assert(!memcmp(&map, &old, sizeof(map)));
	/* A second plausible copy: the one in step with the first record wins. */
	store32(sram + 0x40000 + PMP_TEMP_VALID_OFFSET, 0);
	assert(!apple_pmp_temp_locate(sram, 0x3d000, PMP_TEMP_SRAM_SIZE, &map));
	assert(map.offset[2] == apple_pmp_temp_records[2].offset + shift);
	/* Two copies of the first record: ambiguous, and the map is kept. */
	memcpy(sram + 0x41000, "ta000m", 7);
	assert(apple_pmp_temp_locate(sram, 0x3d000, PMP_TEMP_SRAM_SIZE, &map) == -EEXIST);
	assert(map.offset[2] == apple_pmp_temp_records[2].offset + shift);
	/* The search stays inside the range it is given. */
	assert(apple_pmp_temp_locate(sram, 0x69800, PMP_TEMP_SRAM_SIZE, &map) == -ENOENT);
	assert(apple_pmp_temp_locate(sram, 0x3d001, PMP_TEMP_SRAM_SIZE, &map) == -EINVAL);
	free(sram);
}

int main(int argc, char **argv)
{
	int raw;
	u32 valid;
	unsigned int actual, expected;

	(void)argv;
	if (argc > 1) {
		selftest();
		locate_selftest();
		puts("Production PMP hotspot, missing-record and record-location checks passed");
		return 0;
	}
	while (scanf("%d %u %u %u", &raw, &valid, &actual, &expected) == 4) {
		u8 record[PMP_TEMP_RECORD_SIZE] = { 0 };
		long value = 0;
		int ret;

		if (actual >= PMP_TEMP_CHANNELS || expected >= PMP_TEMP_CHANNELS)
			return 1;
		memcpy(record, apple_pmp_temp_records[actual].name, 7);
		store32(record + PMP_TEMP_SAMPLE_OFFSET, raw);
		store32(record + PMP_TEMP_VALID_OFFSET, valid);
		ret = apple_pmp_temp_decode(record, apple_pmp_temp_records[expected].name, &value);
		printf("%d %ld\n", ret, value);
	}
	return ferror(stdin) ? 1 : 0;
}
