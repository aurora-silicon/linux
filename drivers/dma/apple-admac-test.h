/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * The position of a cyclic transfer from the report ring and the residue,
 * at a period's end in particular.
 */
#include <kunit/test.h>

/* 512 frames of 6 channels of S24 in 32 bits, a 64-period buffer */
#define ADMAC_TEST_PERIOD	12288
#define ADMAC_TEST_BUFFER	(64 * ADMAC_TEST_PERIOD)

/* A report ring with @n reports, read slot @rd */
static u32 admac_test_ring(unsigned int rd, unsigned int n)
{
	u32 v = FIELD_PREP(RING_READ_SLOT, rd) | FIELD_PREP(RING_WRITE_SLOT, (rd + n) % 4);

	if (!n)
		v |= RING_EMPTY;
	else if (n == 4)
		v |= RING_FULL;
	return v;
}

static struct admac_tx admac_test_tx(size_t reclaimed, size_t reported)
{
	return (struct admac_tx) {
		.cyclic = true,
		.buf_len = ADMAC_TEST_BUFFER,
		.period_len = ADMAC_TEST_PERIOD,
		.reclaimed_pos = reclaimed,
		.reported_pos = reported,
	};
}

static struct admac_residue_read admac_test_read(u32 ring, u32 residue)
{
	return (struct admac_residue_read) { ring, residue, ring, residue };
}

static void admac_pos_mid_period(struct kunit *test)
{
	struct admac_tx tx = admac_test_tx(21 * ADMAC_TEST_PERIOD, 0);
	struct admac_residue_read r = admac_test_read(admac_test_ring(1, 0), 4224);
	size_t pos;

	KUNIT_ASSERT_TRUE(test, admac_cyclic_pos(&tx, &r, &pos));
	KUNIT_EXPECT_EQ(test, pos, 22 * ADMAC_TEST_PERIOD - 4224);

	/* A report the interrupt has not drained yet, the next period loaded */
	r = admac_test_read(admac_test_ring(3, 1), ADMAC_TEST_PERIOD);
	KUNIT_ASSERT_TRUE(test, admac_cyclic_pos(&tx, &r, &pos));
	KUNIT_EXPECT_EQ(test, pos, 22 * ADMAC_TEST_PERIOD);
	r = admac_test_read(admac_test_ring(3, 1), 128);
	KUNIT_ASSERT_TRUE(test, admac_cyclic_pos(&tx, &r, &pos));
	KUNIT_EXPECT_EQ(test, pos, 23 * ADMAC_TEST_PERIOD - 128);
}

/*
 * The J316s underrun: the period's report posted, the next descriptor not
 * loaded yet. The old reading put the transfer a whole period ahead.
 */
static void admac_pos_report_before_load(struct kunit *test)
{
	struct admac_tx tx = admac_test_tx(21 * ADMAC_TEST_PERIOD, 22 * ADMAC_TEST_PERIOD - 432);
	struct admac_residue_read r = admac_test_read(admac_test_ring(3, 1), 0);
	size_t pos;

	KUNIT_EXPECT_FALSE(test, admac_cyclic_pos(&tx, &r, &pos));
	KUNIT_EXPECT_EQ(test, admac_cyclic_pos_at_period_end(&tx, &r), 22 * ADMAC_TEST_PERIOD);
}

/* The period run out, its report not posted: the end of the period, or the last report */
static void admac_pos_residue_out(struct kunit *test)
{
	struct admac_tx tx = admac_test_tx(21 * ADMAC_TEST_PERIOD, 22 * ADMAC_TEST_PERIOD - 432);
	struct admac_residue_read r = admac_test_read(admac_test_ring(1, 0), 0);
	size_t pos;

	KUNIT_EXPECT_FALSE(test, admac_cyclic_pos(&tx, &r, &pos));
	/* Never behind what was reported, never ahead of the transfer */
	KUNIT_EXPECT_EQ(test, admac_cyclic_pos_at_period_end(&tx, &r),
			22 * ADMAC_TEST_PERIOD - 432);
}

/* The ring moved, or the next descriptor loaded, between the reads */
static void admac_pos_torn_read(struct kunit *test)
{
	struct admac_tx tx = admac_test_tx(21 * ADMAC_TEST_PERIOD, 0);
	struct admac_residue_read r;
	size_t pos;

	r = (struct admac_residue_read) { admac_test_ring(1, 0), 128, admac_test_ring(1, 1), 128 };
	KUNIT_EXPECT_FALSE(test, admac_cyclic_pos(&tx, &r, &pos));
	r = (struct admac_residue_read) { admac_test_ring(1, 1), 0, admac_test_ring(1, 1),
					  ADMAC_TEST_PERIOD };
	KUNIT_EXPECT_FALSE(test, admac_cyclic_pos(&tx, &r, &pos));
	r = (struct admac_residue_read) { admac_test_ring(1, 0), 128, admac_test_ring(1, 0),
					  ADMAC_TEST_PERIOD };
	KUNIT_EXPECT_FALSE(test, admac_cyclic_pos(&tx, &r, &pos));
	/* A residue larger than a period is no read of this transfer */
	r = admac_test_read(admac_test_ring(1, 0), ADMAC_TEST_PERIOD + 128);
	KUNIT_EXPECT_FALSE(test, admac_cyclic_pos(&tx, &r, &pos));
}

/* Positions run over two buffers, then wrap */
static void admac_pos_wraps(struct kunit *test)
{
	size_t last = 2 * ADMAC_TEST_BUFFER - ADMAC_TEST_PERIOD;
	struct admac_tx tx = admac_test_tx(last, last + 4224);
	struct admac_residue_read r = admac_test_read(admac_test_ring(0, 1), 128);
	size_t pos;

	KUNIT_ASSERT_TRUE(test, admac_cyclic_pos(&tx, &r, &pos));
	KUNIT_EXPECT_EQ(test, pos, ADMAC_TEST_PERIOD - 128);

	/* At the period's end across the wrap: the report counts */
	r = admac_test_read(admac_test_ring(0, 1), 0);
	KUNIT_EXPECT_EQ(test, admac_cyclic_pos_at_period_end(&tx, &r), 0);
	KUNIT_EXPECT_TRUE(test, admac_cyclic_pos_after(&tx, last + 4224, 0));
	KUNIT_EXPECT_FALSE(test, admac_cyclic_pos_after(&tx, 0, last + 4224));
	KUNIT_EXPECT_FALSE(test, admac_cyclic_pos_after(&tx, 0, 0));
}

static struct kunit_case admac_pos_cases[] = {
	KUNIT_CASE(admac_pos_mid_period),
	KUNIT_CASE(admac_pos_report_before_load),
	KUNIT_CASE(admac_pos_residue_out),
	KUNIT_CASE(admac_pos_torn_read),
	KUNIT_CASE(admac_pos_wraps),
	{}
};

static struct kunit_suite admac_pos_suite = {
	.name = "apple-admac-position",
	.test_cases = admac_pos_cases,
};

kunit_test_suite(admac_pos_suite);
