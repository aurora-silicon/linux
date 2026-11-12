// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * KUnit tests for the Apple AON PTD battery readout: record placement, the
 * stable two-read sample and the unit conversions. No hardware is needed.
 *
 * Included by apple-aon-ptd-hwmon.c.
 */

#include <kunit/test.h>

static void apple_ptd_test_record_offset(struct kunit *test)
{
	u32 off = 0;

	KUNIT_EXPECT_EQ(test, apple_ptd_record_offset(PTD_BATT_ID, 1, 0x12, 4,
						      0x4000, &off), 0);
	KUNIT_EXPECT_EQ(test, off, 0x120);

	/* The record may end exactly at the end of the window. */
	KUNIT_EXPECT_EQ(test, apple_ptd_record_offset(PTD_BATT_ID, 1, 0x3fc, 4,
						      0x4000, &off), 0);
	KUNIT_EXPECT_EQ(test, off, 0x3fc0);
}

static void apple_ptd_test_record_rejects(struct kunit *test)
{
	u32 off = 0xdead;

	/* Another record or another layout version: not ours. */
	KUNIT_EXPECT_EQ(test, apple_ptd_record_offset(PTD_BATT_ID + 1, 1, 0x12, 4,
						      0x4000, &off), -ENODEV);
	KUNIT_EXPECT_EQ(test, apple_ptd_record_offset(PTD_BATT_ID, 2, 0x12, 4,
						      0x4000, &off), -ENODEV);
	/* Too short for the words we read. */
	KUNIT_EXPECT_EQ(test, apple_ptd_record_offset(PTD_BATT_ID, 1, 0x12, 3,
						      0x4000, &off), -EINVAL);
	/* Past the end of the peek window, including u32 wrap-around. */
	KUNIT_EXPECT_EQ(test, apple_ptd_record_offset(PTD_BATT_ID, 1, 0x3fd, 4,
						      0x4000, &off), -EINVAL);
	KUNIT_EXPECT_EQ(test, apple_ptd_record_offset(PTD_BATT_ID, 1, 0xfffffffe, 4,
						      0x4000, &off), -EINVAL);
	KUNIT_EXPECT_EQ(test, apple_ptd_record_offset(PTD_BATT_ID, 1, 0x12, 0xffffffff,
						      0x4000, &off), -EINVAL);
	KUNIT_EXPECT_EQ(test, off, 0xdead);
}

/* A record the producer rewrites on the first @torn reads. */
struct apple_ptd_test_src {
	unsigned int reads;
	unsigned int torn;
};

static void apple_ptd_test_read(void *ctx, struct apple_ptd_sample *out)
{
	struct apple_ptd_test_src *src = ctx;
	unsigned int i;

	for (i = 0; i < PTD_BATT_WORDS; i++)
		out->word[i] = 0x100 + i;
	if (src->reads < src->torn)
		out->word[PTD_SYS_POWER] = src->reads;
	src->reads++;
}

static void apple_ptd_test_stable_read(struct kunit *test)
{
	struct apple_ptd_test_src src = { .torn = 3 };
	struct apple_ptd_sample s = {};

	KUNIT_EXPECT_EQ(test, apple_ptd_stable_read(apple_ptd_test_read, &src, &s), 0);
	KUNIT_EXPECT_EQ(test, s.word[PTD_SYS_POWER], 0x101);
	KUNIT_EXPECT_EQ(test, s.word[PTD_BATT_VOLT], 0x100);
	/* Pairs (0,1) and (2,3) disagree, the third pair agrees. */
	KUNIT_EXPECT_EQ(test, src.reads, 6);
}

static void apple_ptd_test_stable_read_gives_up(struct kunit *test)
{
	struct apple_ptd_test_src src = { .torn = UINT_MAX };
	struct apple_ptd_sample s = { .word = { 7 } };

	KUNIT_EXPECT_EQ(test, apple_ptd_stable_read(apple_ptd_test_read, &src, &s),
			-EAGAIN);
	KUNIT_EXPECT_EQ(test, src.reads, 2 * PTD_READ_TRIES);
	/* A failed read leaves the caller's sample alone. */
	KUNIT_EXPECT_EQ(test, s.word[0], 7);
}

static void apple_ptd_test_convert(struct kunit *test)
{
	/* Values seen on a J616s: 13095.4 mV battery, 0.4335 W system. */
	struct apple_ptd_sample s = {
		.word = {
			[PTD_BATT_VOLT] = 858218725,
			[PTD_SYS_POWER] = 28410,
		},
	};
	long v;

	KUNIT_EXPECT_EQ(test, apple_ptd_convert(&s, hwmon_in, 0, &v), 0);
	KUNIT_EXPECT_EQ(test, v, 13095);
	KUNIT_EXPECT_EQ(test, apple_ptd_convert(&s, hwmon_power, 0, &v), 0);
	KUNIT_EXPECT_EQ(test, v, 433502);
	KUNIT_EXPECT_EQ(test, apple_ptd_convert(&s, hwmon_in, 1, &v), -EOPNOTSUPP);
	KUNIT_EXPECT_EQ(test, apple_ptd_convert(&s, hwmon_curr, 0, &v), -EOPNOTSUPP);
	KUNIT_EXPECT_EQ(test, apple_ptd_convert(&s, hwmon_temp, 0, &v), -EOPNOTSUPP);
}

static void apple_ptd_test_uq16(struct kunit *test)
{
	long v;

	/* Truncated: one LSB is 15.26 uW, 1/65536 mV. */
	KUNIT_EXPECT_EQ(test, apple_ptd_uq16(1, MICRO, &v), 0);
	KUNIT_EXPECT_EQ(test, v, 15);
	KUNIT_EXPECT_EQ(test, apple_ptd_uq16(65535, 1, &v), 0);
	KUNIT_EXPECT_EQ(test, v, 0);
	/* The top bit is magnitude, not sign. */
	KUNIT_EXPECT_EQ(test, apple_ptd_uq16(0x80000000, 1, &v), 0);
	KUNIT_EXPECT_EQ(test, v, 32768);
	KUNIT_EXPECT_EQ(test, apple_ptd_uq16(0xffffffff, 1, &v), 0);
	KUNIT_EXPECT_EQ(test, v, 65535);
#if BITS_PER_LONG == 64
	KUNIT_EXPECT_EQ(test, apple_ptd_uq16(0xffffffff, MICRO, &v), 0);
	KUNIT_EXPECT_EQ(test, v, 65535999984L);
#else
	KUNIT_EXPECT_EQ(test, apple_ptd_uq16(0xffffffff, MICRO, &v), -EOVERFLOW);
#endif
}

static struct kunit_case apple_ptd_test_cases[] = {
	KUNIT_CASE(apple_ptd_test_record_offset),
	KUNIT_CASE(apple_ptd_test_record_rejects),
	KUNIT_CASE(apple_ptd_test_stable_read),
	KUNIT_CASE(apple_ptd_test_stable_read_gives_up),
	KUNIT_CASE(apple_ptd_test_convert),
	KUNIT_CASE(apple_ptd_test_uq16),
	{}
};

static struct kunit_suite apple_ptd_test_suite = {
	.name = "apple-aon-ptd-hwmon",
	.test_cases = apple_ptd_test_cases,
};

kunit_test_suite(apple_ptd_test_suite);
