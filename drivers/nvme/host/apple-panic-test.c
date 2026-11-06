// SPDX-License-Identifier: GPL-2.0-only

#include <kunit/test.h>
#include <linux/crc32.h>
#include <linux/module.h>
#include <linux/string.h>
#include <linux/unaligned.h>

#include "apple-panic.h"

#define TEST_RECORD_SIZE 256

enum test_header_offset {
	TEST_MAGIC_OFFSET = 0,
	TEST_CRC_OFFSET = 4,
	TEST_VERSION_OFFSET = 8,
	TEST_PANIC_OFFSET = 20,
	TEST_PANIC_LENGTH = 24,
	TEST_STACKSHOT_OFFSET = 28,
	TEST_STACKSHOT_LENGTH = 32,
	TEST_OTHER_OFFSET = 36,
	TEST_OTHER_LENGTH = 40,
	TEST_EXTENSIBLE_OFFSET = 161,
	TEST_EXTENSIBLE_LENGTH = 165,
	TEST_INITIATOR_OFFSET = 169,
	TEST_INITIATOR_LENGTH = 173,
};

static void apple_panic_test_set_crc(u8 *data, size_t size)
{
	put_unaligned_le32(crc32_le(0, data + TEST_VERSION_OFFSET,
				    size - TEST_VERSION_OFFSET),
			   data + TEST_CRC_OFFSET);
}

static u8 *apple_panic_test_record(struct kunit *test)
{
	u8 *data = kunit_kzalloc(test, TEST_RECORD_SIZE, GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, data);
	put_unaligned_le32(APPLE_PANIC_FUNK_MAGIC, data + TEST_MAGIC_OFFSET);
	put_unaligned_le32(APPLE_PANIC_FUNK_VERSION, data + TEST_VERSION_OFFSET);
	put_unaligned_le32(APPLE_PANIC_FUNK_V6_HDR_SIZE,
			   data + TEST_PANIC_OFFSET);
	put_unaligned_le32(5, data + TEST_PANIC_LENGTH);
	memcpy(data + APPLE_PANIC_FUNK_V6_HDR_SIZE, "panic", 5);
	put_unaligned_le32(200, data + TEST_OTHER_OFFSET);
	put_unaligned_le32(5, data + TEST_OTHER_LENGTH);
	memcpy(data + 200, "other", 5);
	apple_panic_test_set_crc(data, TEST_RECORD_SIZE);
	return data;
}

static void apple_panic_valid_record_test(struct kunit *test)
{
	struct apple_panic_record record;
	u8 *data = apple_panic_test_record(test);

	KUNIT_EXPECT_EQ(test, apple_panic_parse_funk_v6(data, TEST_RECORD_SIZE,
							&record), 0);
	KUNIT_EXPECT_EQ(test, record.panic_text.size, (size_t)5);
	KUNIT_EXPECT_MEMEQ(test, record.panic_text.data, "panic", 5);
	KUNIT_EXPECT_EQ(test, record.other_log.size, (size_t)5);
	KUNIT_EXPECT_MEMEQ(test, record.other_log.data, "other", 5);
	KUNIT_EXPECT_EQ(test, record.stackshot.size, (size_t)0);
}

static void apple_panic_short_record_test(struct kunit *test)
{
	struct apple_panic_record record;
	u8 *data = apple_panic_test_record(test);

	KUNIT_EXPECT_EQ(test, apple_panic_parse_funk_v6(
		data, APPLE_PANIC_FUNK_V6_HDR_SIZE - 1, &record), -EINVAL);
}

static void apple_panic_bad_magic_test(struct kunit *test)
{
	struct apple_panic_record record;
	u8 *data = apple_panic_test_record(test);

	data[TEST_MAGIC_OFFSET] ^= 1;
	KUNIT_EXPECT_EQ(test, apple_panic_parse_funk_v6(data, TEST_RECORD_SIZE,
							&record), -EBADMSG);
}

static void apple_panic_bad_version_test(struct kunit *test)
{
	struct apple_panic_record record;
	u8 *data = apple_panic_test_record(test);

	put_unaligned_le32(5, data + TEST_VERSION_OFFSET);
	apple_panic_test_set_crc(data, TEST_RECORD_SIZE);
	KUNIT_EXPECT_EQ(test, apple_panic_parse_funk_v6(data, TEST_RECORD_SIZE,
							&record),
			-EPROTONOSUPPORT);
}

static void apple_panic_crc_covers_padding_test(struct kunit *test)
{
	struct apple_panic_record record;
	u8 *data = apple_panic_test_record(test);

	data[TEST_RECORD_SIZE - 1] ^= 1;
	KUNIT_EXPECT_EQ(test, apple_panic_parse_funk_v6(data, TEST_RECORD_SIZE,
							&record), -EBADMSG);
}

static void apple_panic_segment_in_header_test(struct kunit *test)
{
	struct apple_panic_record record;
	u8 *data = apple_panic_test_record(test);

	put_unaligned_le32(16, data + TEST_PANIC_OFFSET);
	apple_panic_test_set_crc(data, TEST_RECORD_SIZE);
	KUNIT_EXPECT_EQ(test, apple_panic_parse_funk_v6(data, TEST_RECORD_SIZE,
							&record), -EINVAL);
}

static void apple_panic_segment_out_of_bounds_test(struct kunit *test)
{
	struct apple_panic_record record;
	u8 *data = apple_panic_test_record(test);

	put_unaligned_le32(TEST_RECORD_SIZE - 2, data + TEST_PANIC_OFFSET);
	put_unaligned_le32(4, data + TEST_PANIC_LENGTH);
	apple_panic_test_set_crc(data, TEST_RECORD_SIZE);
	KUNIT_EXPECT_EQ(test, apple_panic_parse_funk_v6(data, TEST_RECORD_SIZE,
							&record), -EINVAL);
}

static void apple_panic_segment_huge_length_test(struct kunit *test)
{
	struct apple_panic_record record;
	u8 *data = apple_panic_test_record(test);

	put_unaligned_le32(U32_MAX, data + TEST_PANIC_LENGTH);
	apple_panic_test_set_crc(data, TEST_RECORD_SIZE);
	KUNIT_EXPECT_EQ(test, apple_panic_parse_funk_v6(data, TEST_RECORD_SIZE,
							&record), -EINVAL);
}

static void apple_panic_overlapping_segments_test(struct kunit *test)
{
	struct apple_panic_record record;
	u8 *data = apple_panic_test_record(test);

	put_unaligned_le32(179, data + TEST_OTHER_OFFSET);
	put_unaligned_le32(5, data + TEST_OTHER_LENGTH);
	apple_panic_test_set_crc(data, TEST_RECORD_SIZE);
	KUNIT_EXPECT_EQ(test, apple_panic_parse_funk_v6(data, TEST_RECORD_SIZE,
							&record), -EINVAL);
}

static void apple_panic_disordered_segments_test(struct kunit *test)
{
	struct apple_panic_record record;
	u8 *data = apple_panic_test_record(test);

	put_unaligned_le32(220, data + TEST_STACKSHOT_OFFSET);
	put_unaligned_le32(4, data + TEST_STACKSHOT_LENGTH);
	memcpy(data + 220, "shot", 4);
	apple_panic_test_set_crc(data, TEST_RECORD_SIZE);
	KUNIT_EXPECT_EQ(test, apple_panic_parse_funk_v6(data, TEST_RECORD_SIZE,
							&record), 0);
	KUNIT_EXPECT_MEMEQ(test, record.stackshot.data, "shot", 4);
}

static void apple_panic_empty_segment_ignores_offset_test(struct kunit *test)
{
	struct apple_panic_record record;
	u8 *data = apple_panic_test_record(test);

	put_unaligned_le32(U32_MAX, data + TEST_EXTENSIBLE_OFFSET);
	apple_panic_test_set_crc(data, TEST_RECORD_SIZE);
	KUNIT_EXPECT_EQ(test, apple_panic_parse_funk_v6(data, TEST_RECORD_SIZE,
							&record), 0);
	KUNIT_EXPECT_EQ(test, record.extensible_log.size, (size_t)0);
}

static struct kunit_case apple_panic_test_cases[] = {
	KUNIT_CASE(apple_panic_valid_record_test),
	KUNIT_CASE(apple_panic_short_record_test),
	KUNIT_CASE(apple_panic_bad_magic_test),
	KUNIT_CASE(apple_panic_bad_version_test),
	KUNIT_CASE(apple_panic_crc_covers_padding_test),
	KUNIT_CASE(apple_panic_segment_in_header_test),
	KUNIT_CASE(apple_panic_segment_out_of_bounds_test),
	KUNIT_CASE(apple_panic_segment_huge_length_test),
	KUNIT_CASE(apple_panic_overlapping_segments_test),
	KUNIT_CASE(apple_panic_disordered_segments_test),
	KUNIT_CASE(apple_panic_empty_segment_ignores_offset_test),
	{}
};

static struct kunit_suite apple_panic_test_suite = {
	.name = "apple-panic-record",
	.test_cases = apple_panic_test_cases,
};

kunit_test_suite(apple_panic_test_suite);

MODULE_LICENSE("GPL");
