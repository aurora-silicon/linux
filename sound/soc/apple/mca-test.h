/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * The capture sync of the cacheable buffers: which ranges are invalidated
 * for an advance of the DMA, laps included.
 */
#include <kunit/test.h>

#define MCA_TEST_RATE		48000
#define MCA_TEST_PERIOD		512
#define MCA_TEST_BUFFER		(4 * MCA_TEST_PERIOD)
/* 12 channels of S16 */
#define MCA_TEST_FRAME		24
#define MCA_TEST_BYTES		(MCA_TEST_BUFFER * MCA_TEST_FRAME)

static s64 mca_test_frames_ns(u64 frames)
{
	return div_u64(frames * NSEC_PER_SEC, MCA_TEST_RATE);
}

static bool mca_test_may_lap(s64 elapsed_ns)
{
	return mca_capture_may_lap(MCA_TEST_RATE, MCA_TEST_BUFFER, MCA_TEST_PERIOD, elapsed_ns);
}

static void mca_capture_lap_by_time(struct kunit *test)
{
	/* The usual cadence: a period, or a little more under IRQ latency */
	KUNIT_EXPECT_FALSE(test, mca_test_may_lap(0));
	KUNIT_EXPECT_FALSE(test, mca_test_may_lap(mca_test_frames_ns(MCA_TEST_PERIOD)));
	KUNIT_EXPECT_FALSE(test, mca_test_may_lap(mca_test_frames_ns(2 * MCA_TEST_PERIOD)));
	/* From a buffer less a period on, the position may hide a lap */
	KUNIT_EXPECT_TRUE(test,
			  mca_test_may_lap(mca_test_frames_ns(MCA_TEST_BUFFER - MCA_TEST_PERIOD)));
	KUNIT_EXPECT_TRUE(test, mca_test_may_lap(mca_test_frames_ns(MCA_TEST_BUFFER)));
	KUNIT_EXPECT_TRUE(test, mca_test_may_lap(mca_test_frames_ns(3 * MCA_TEST_BUFFER)));
	/* No rate, or a clock that went back: assume the worst */
	KUNIT_EXPECT_TRUE(test, mca_capture_may_lap(0, MCA_TEST_BUFFER, MCA_TEST_PERIOD, 0));
	KUNIT_EXPECT_TRUE(test, mca_test_may_lap(-1));
}

static void mca_capture_ranges_follow_the_dma(struct kunit *test)
{
	size_t off[2], len[2];
	const size_t p = MCA_TEST_PERIOD * MCA_TEST_FRAME;

	/* No advance and no lap: nothing */
	KUNIT_EXPECT_EQ(test, mca_capture_ranges(p, p, MCA_TEST_BYTES, false, off, len), 0U);

	/* Forward */
	KUNIT_ASSERT_EQ(test, mca_capture_ranges(p, 2 * p, MCA_TEST_BYTES, false, off, len), 1U);
	KUNIT_EXPECT_EQ(test, off[0], p);
	KUNIT_EXPECT_EQ(test, len[0], p);

	/* Across the end */
	KUNIT_ASSERT_EQ(test, mca_capture_ranges(3 * p, p, MCA_TEST_BYTES, false, off, len), 2U);
	KUNIT_EXPECT_EQ(test, off[0], 3 * p);
	KUNIT_EXPECT_EQ(test, len[0], (size_t)MCA_TEST_BYTES - 3 * p);
	KUNIT_EXPECT_EQ(test, off[1], (size_t)0);
	KUNIT_EXPECT_EQ(test, len[1], p);

	/* Onto the end exactly */
	KUNIT_ASSERT_EQ(test, mca_capture_ranges(3 * p, 0, MCA_TEST_BYTES, false, off, len), 1U);
	KUNIT_EXPECT_EQ(test, off[0], 3 * p);
	KUNIT_EXPECT_EQ(test, len[0], p);
}

/* An exact lap, or more than one, brings the same or a nearby position back */
static void mca_capture_lap_syncs_everything(struct kunit *test)
{
	size_t off[2], len[2];
	const size_t p = MCA_TEST_PERIOD * MCA_TEST_FRAME;
	bool lap;

	/* Exactly one lap: the same position */
	lap = mca_test_may_lap(mca_test_frames_ns(MCA_TEST_BUFFER));
	KUNIT_ASSERT_EQ(test, mca_capture_ranges(p, p, MCA_TEST_BYTES, lap, off, len), 1U);
	KUNIT_EXPECT_EQ(test, off[0], (size_t)0);
	KUNIT_EXPECT_EQ(test, len[0], (size_t)MCA_TEST_BYTES);

	/* Two laps and a period: what the position shows is only the period */
	lap = mca_test_may_lap(mca_test_frames_ns(2 * MCA_TEST_BUFFER + MCA_TEST_PERIOD));
	KUNIT_ASSERT_EQ(test, mca_capture_ranges(p, 2 * p, MCA_TEST_BYTES, lap, off, len), 1U);
	KUNIT_EXPECT_EQ(test, off[0], (size_t)0);
	KUNIT_EXPECT_EQ(test, len[0], (size_t)MCA_TEST_BYTES);
}

static struct kunit_case mca_capture_sync_cases[] = {
	KUNIT_CASE(mca_capture_lap_by_time),
	KUNIT_CASE(mca_capture_ranges_follow_the_dma),
	KUNIT_CASE(mca_capture_lap_syncs_everything),
	{}
};

static struct kunit_suite mca_capture_sync_suite = {
	.name = "apple-mca-capture-sync",
	.test_cases = mca_capture_sync_cases,
};
kunit_test_suite(mca_capture_sync_suite);

/*
 * The registers kept for a system sleep: every write, the last value of each,
 * and the write-back after the sleep with the enables last and the SERDES
 * stopped. The "hardware" is a buffer here.
 */
struct mca_test_regs {
	struct mca_cluster cl;
	u32 hw[CLUSTER_NREGS];
};

static struct mca_test_regs *mca_test_regs_new(struct kunit *test)
{
	struct mca_test_regs *t = kunit_kzalloc(test, sizeof(*t), GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, t);
	t->cl.base = (void __iomem *)t->hw;
	return t;
}

static void mca_sleep_regs_follow_writes(struct kunit *test)
{
	struct mca_test_regs *t = mca_test_regs_new(test);

	mca_write(&t->cl, REG_SYNCGEN_HI_PERIOD, 31);
	mca_write(&t->cl, REG_SYNCGEN_HI_PERIOD, 15);
	mca_modify(&t->cl, REG_STATUS, STATUS_MCLK_EN, STATUS_MCLK_EN);
	mca_modify(&t->cl, CLUSTER_TX_OFF + REG_TX_SERDES_CONF, SERDES_CONF_NCHANS, 5);

	KUNIT_EXPECT_EQ(test, t->cl.regs[REG_SYNCGEN_HI_PERIOD / 4], 15U);
	KUNIT_EXPECT_EQ(test, t->hw[REG_SYNCGEN_HI_PERIOD / 4], 15U);
	KUNIT_EXPECT_EQ(test, t->cl.regs[REG_STATUS / 4], (u32)STATUS_MCLK_EN);
	KUNIT_EXPECT_EQ(test, t->cl.regs[(CLUSTER_TX_OFF + REG_TX_SERDES_CONF) / 4], 5U);
	KUNIT_EXPECT_TRUE(test, test_bit(REG_SYNCGEN_HI_PERIOD / 4, t->cl.regs_written));
	KUNIT_EXPECT_FALSE(test, test_bit(REG_SYNCGEN_LO_PERIOD / 4, t->cl.regs_written));
}

static void mca_sleep_regs_restore_in_order(struct kunit *test)
{
	struct mca_test_regs *t = mca_test_regs_new(test);
	unsigned int txs = CLUSTER_TX_OFF + REG_SERDES_STATUS;

	mca_write(&t->cl, REG_MCLK_CONF, 0x100);
	mca_write(&t->cl, REG_SYNCGEN_STATUS, SYNCGEN_STATUS_EN);
	mca_write(&t->cl, REG_PORT_ENABLES, PORT_ENABLES_CLOCKS | PORT_ENABLES_TX_DATA);
	mca_write(&t->cl, txs, SERDES_STATUS_EN | SERDES_STATUS_RST | BIT(4));

	/* The sleep: the hardware is back at its reset values */
	memset(t->hw, 0, sizeof(t->hw));

	mca_restore_cluster(&t->cl, true, false);
	KUNIT_EXPECT_EQ(test, t->hw[REG_MCLK_CONF / 4], 0x100U);
	/* No enable before every configuration register is back */
	KUNIT_EXPECT_EQ(test, t->hw[REG_SYNCGEN_STATUS / 4], 0U);
	KUNIT_EXPECT_EQ(test, t->hw[REG_PORT_ENABLES / 4], 0U);
	/* The SERDES comes back stopped */
	KUNIT_EXPECT_EQ(test, t->hw[txs / 4], (u32)BIT(4));

	mca_restore_cluster(&t->cl, true, true);
	KUNIT_EXPECT_EQ(test, t->hw[REG_SYNCGEN_STATUS / 4], (u32)SYNCGEN_STATUS_EN);
	KUNIT_EXPECT_EQ(test, t->hw[REG_PORT_ENABLES / 4],
			(u32)(PORT_ENABLES_CLOCKS | PORT_ENABLES_TX_DATA));
	/* Never written: left alone */
	KUNIT_EXPECT_EQ(test, t->hw[REG_SYNCGEN_LO_PERIOD / 4], 0U);
}

/* A cluster whose domain is not held gets only its port back */
static void mca_sleep_regs_port_only(struct kunit *test)
{
	struct mca_test_regs *t = mca_test_regs_new(test);

	mca_write(&t->cl, REG_MCLK_CONF, 0x100);
	mca_write(&t->cl, REG_PORT_CLOCK_SEL, FIELD_PREP(PORT_CLOCK_SEL, 2));
	mca_write(&t->cl, REG_PORT_ENABLES, PORT_ENABLES_CLOCKS);
	memset(t->hw, 0, sizeof(t->hw));

	mca_restore_cluster(&t->cl, false, false);
	mca_restore_cluster(&t->cl, false, true);
	KUNIT_EXPECT_EQ(test, t->hw[REG_MCLK_CONF / 4], 0U);
	KUNIT_EXPECT_EQ(test, t->hw[REG_PORT_CLOCK_SEL / 4], (u32)FIELD_PREP(PORT_CLOCK_SEL, 2));
	KUNIT_EXPECT_EQ(test, t->hw[REG_PORT_ENABLES / 4], (u32)PORT_ENABLES_CLOCKS);
}

static struct kunit_case mca_sleep_regs_cases[] = {
	KUNIT_CASE(mca_sleep_regs_follow_writes),
	KUNIT_CASE(mca_sleep_regs_restore_in_order),
	KUNIT_CASE(mca_sleep_regs_port_only),
	{}
};

static struct kunit_suite mca_sleep_regs_suite = {
	.name = "apple-mca-sleep-regs",
	.test_cases = mca_sleep_regs_cases,
};

kunit_test_suite(mca_sleep_regs_suite);
