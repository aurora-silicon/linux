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
		out->word[PTD_POWER_10MS] = src->reads;
	src->reads++;
}

static void apple_ptd_test_stable_read(struct kunit *test)
{
	struct apple_ptd_test_src src = { .torn = 3 };
	struct apple_ptd_sample s = {};

	KUNIT_EXPECT_EQ(test, apple_ptd_stable_read(apple_ptd_test_read, &src, &s), 0);
	KUNIT_EXPECT_EQ(test, s.word[PTD_POWER_10MS], 0x104);
	KUNIT_EXPECT_EQ(test, s.word[PTD_VOLT_10MS], 0x100);
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

/* Signed Q16.16 from a count of quarter units (no floating point). */
#define Q16Q(q)	((u32)(s32)((q) * 16384))

static void apple_ptd_test_convert(struct kunit *test)
{
	struct apple_ptd_sample s = {
		.word = {
			[PTD_VOLT_10MS] = Q16Q(50),
			[PTD_CURR_100MS] = 1234,
			[PTD_CURR_10MS] = Q16Q(-5),
			[PTD_POWER_100MS] = Q16Q(63),
			[PTD_POWER_10MS] = Q16Q(-82),
			[PTD_VOLT_100MS] = Q16Q(49),
		},
	};
	long v;

	KUNIT_EXPECT_EQ(test, apple_ptd_convert(&s, hwmon_in, 0, &v), 0);
	KUNIT_EXPECT_EQ(test, v, 12500);
	KUNIT_EXPECT_EQ(test, apple_ptd_convert(&s, hwmon_in, 1, &v), 0);
	KUNIT_EXPECT_EQ(test, v, 12250);
	KUNIT_EXPECT_EQ(test, apple_ptd_convert(&s, hwmon_curr, 0, &v), 0);
	KUNIT_EXPECT_EQ(test, v, -1250);
	KUNIT_EXPECT_EQ(test, apple_ptd_convert(&s, hwmon_curr, 1, &v), 0);
	KUNIT_EXPECT_EQ(test, v, 1234);
	KUNIT_EXPECT_EQ(test, apple_ptd_convert(&s, hwmon_power, 0, &v), 0);
	KUNIT_EXPECT_EQ(test, v, -20500000);
	KUNIT_EXPECT_EQ(test, apple_ptd_convert(&s, hwmon_power, 1, &v), 0);
	KUNIT_EXPECT_EQ(test, v, 15750000);
	KUNIT_EXPECT_EQ(test, apple_ptd_convert(&s, hwmon_temp, 0, &v), -EOPNOTSUPP);
}

static void apple_ptd_test_q16_truncates(struct kunit *test)
{
	long v;

	/* One LSB is 15.26 uV: truncated toward zero on both signs. */
	KUNIT_EXPECT_EQ(test, apple_ptd_q16(1, MICRO, &v), 0);
	KUNIT_EXPECT_EQ(test, v, 15);
	KUNIT_EXPECT_EQ(test, apple_ptd_q16((u32)-1, MICRO, &v), 0);
	KUNIT_EXPECT_EQ(test, v, -15);
	/* Full-scale values keep their sign; nothing is read as unsigned. */
	KUNIT_EXPECT_EQ(test, apple_ptd_q16(0x80000000, MILLI, &v), 0);
	KUNIT_EXPECT_EQ(test, v, -32768000);
	KUNIT_EXPECT_EQ(test, apple_ptd_q16(0x7fffffff, MILLI, &v), 0);
	KUNIT_EXPECT_EQ(test, v, 32767999);
}

static struct kunit_case apple_ptd_test_cases[] = {
	KUNIT_CASE(apple_ptd_test_record_offset),
	KUNIT_CASE(apple_ptd_test_record_rejects),
	KUNIT_CASE(apple_ptd_test_stable_read),
	KUNIT_CASE(apple_ptd_test_stable_read_gives_up),
	KUNIT_CASE(apple_ptd_test_convert),
	KUNIT_CASE(apple_ptd_test_q16_truncates),
	{}
};

static struct kunit_suite apple_ptd_test_suite = {
	.name = "apple-aon-ptd-hwmon",
	.test_cases = apple_ptd_test_cases,
};

kunit_test_suite(apple_ptd_test_suite);
