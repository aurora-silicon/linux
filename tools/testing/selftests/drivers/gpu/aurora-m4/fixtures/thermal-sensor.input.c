// SPDX-License-Identifier: GPL-2.0-only OR MIT

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <linux/soc/apple/pmp-temps.h>
static void put32(u8 *p, u32 value) {
    for (unsigned i = 0; i < 4; i++) p[i] = value >> (8 * i);
}
int main(void) {
    int32_t raw;
    unsigned valid, actual_name, expected_name;
    while (scanf("%d %u %u %u", &raw, &valid, &actual_name, &expected_name) == 4) {
        if (actual_name >= 6 || expected_name >= 6) return 2;
        u8 record[PMP_TEMP_RECORD_SIZE] = {0};
        memcpy(record, apple_pmp_temp_records[actual_name].name, 7);
        put32(record + PMP_TEMP_SAMPLE_OFFSET, raw);
        put32(record + PMP_TEMP_VALID_OFFSET, valid);
        long value = 0;
        int error = apple_pmp_temp_decode(record, apple_pmp_temp_records[expected_name].name, &value);
        printf("%d %ld\n", error, error ? 0 : value);
    }
    return ferror(stdin) || ferror(stdout);
}
