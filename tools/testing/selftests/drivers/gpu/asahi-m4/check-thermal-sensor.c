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

int main(int argc, char **argv)
{
	int raw;
	u32 valid;
	unsigned int actual, expected;

	(void)argv;
	if (argc > 1) {
		selftest();
		puts("Production PMP hotspot and missing-record checks passed");
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
