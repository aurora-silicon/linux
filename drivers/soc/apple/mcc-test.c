// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* Included by mcc.c to exercise allocation failure and cache handoff. */
#include <kunit/device.h>
#include <kunit/test.h>
#include <linux/sizes.h>

struct apple_mcc_test {
	struct apple_mcc mcc;
	unsigned int reject_plane;
	unsigned long attempted;
	unsigned long disabled;
	unsigned long flushed;
	unsigned long deprioritized;
};

static void apple_mcc_test_write(struct apple_mcc *mcc, unsigned int plane,
				 u32 val, u32 offset)
{
	struct kunit *test = kunit_get_current_test();
	struct apple_mcc_test *ctx = test->priv;

	if (offset == MCC_ALLOC_DISABLE) {
		if (val) {
			ctx->disabled |= BIT(plane);
		} else {
			ctx->attempted |= BIT(plane);
			if (plane == ctx->reject_plane)
				return;
		}
	}
	if (offset == MCC_CACHE_FLUSH) {
		ctx->flushed |= BIT(plane);
		/* Model a cache whose flush completes immediately. */
		val = 0;
	}
	if (offset == MCC_DSID_DEPRIO_ACTION_BASE +
		      mcc->release_dsid * MCC_DSID_DEPRIO_ACTION_STRIDE &&
	    val == MCC_DSID_DEPRIO_ACTION_DEPRIORITIZE) {
		ctx->deprioritized |= BIT(plane);
		return;
	}
	writel(val, mcc->planes[plane] + offset);
}

static struct apple_mcc_test *apple_mcc_test_alloc(struct kunit *test)
{
	struct apple_mcc_test *ctx;
	struct apple_mcc *mcc;
	unsigned int i;

	ctx = kunit_kzalloc(test, sizeof(*ctx), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, ctx);
	mcc = &ctx->mcc;
	mcc->dev = kunit_device_register(test, "apple-mcc-test");
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, mcc->dev);
	mcc->nr_planes = 3;
	mutex_init(&mcc->stream_lock);
	ctx->reject_plane = MCC_MAX_PLANES;
	for (i = 0; i < mcc->nr_planes; i++) {
		void *plane = kunit_kzalloc(test, SZ_16K, GFP_KERNEL);

		mcc->planes[i] = (__force void __iomem *)plane;
		KUNIT_ASSERT_NOT_NULL(test, mcc->planes[i]);
		writel(1, mcc->planes[i] + MCC_ALLOC_DISABLE);
		writel(1, mcc->planes[i] + MCC_CACHE_WAYS);
		writel(FIELD_PREP(MCC_CACHE_WAYS_POWERED, 1), mcc->planes[i] + MCC_CACHE_POWER);
	}
	test->priv = ctx;
	kunit_activate_static_stub(test, apple_mcc_writel, apple_mcc_test_write);
	return ctx;
}

static void apple_mcc_test_lifecycle_release(struct kunit *test)
{
	struct apple_mcc_test *ctx = apple_mcc_test_alloc(test);
	struct apple_mcc *mcc = &ctx->mcc;
	u32 stream_id = APPLE_MCC_STREAM_GPU_PARAMETER_BUFFER_WRITE;
	int ret;

	mcc->action_table = true;
	mcc->release_stream_id = stream_id;
	mcc->release_dsid = 21;
	mcc->release_stream_error = 0;

	ret = apple_mcc_release_stream_from_provider(mcc, stream_id);
	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, ctx->deprioritized, GENMASK(mcc->nr_planes - 1, 0));
	KUNIT_EXPECT_EQ(test, mcc->release_count, 1U);

	ctx->deprioritized = 0;
	KUNIT_EXPECT_EQ(test, apple_mcc_release_stream_from_provider(mcc, 1), -ENOENT);
	KUNIT_EXPECT_EQ(test, ctx->deprioritized, 0UL);
}

static void apple_mcc_test_set_stream_field(u8 *record, size_t field, u32 value)
{
	put_unaligned_le32(value, record + field);
}

#define MCC_TEST_SET_FIELD(record, field, value)				\
	apple_mcc_test_set_stream_field((record),				\
		offsetof(struct apple_mcc_mcache_stream_record, field), (value))

static void apple_mcc_test_valid_stream(u8 *record, u32 dsid)
{
	MCC_TEST_SET_FIELD(record, stream_id, MCC_GPU_PARAMETER_BUFFER_STREAM_ID);
	MCC_TEST_SET_FIELD(record, first_quota_group, 1);
	MCC_TEST_SET_FIELD(record, last_quota_group, 1);
	MCC_TEST_SET_FIELD(record, first_dsid, dsid);
	MCC_TEST_SET_FIELD(record, last_dsid, dsid);
	MCC_TEST_SET_FIELD(record, allocation_policy,
			   MCC_MCACHE_POLICY_PMP_DYNAMIC_QUOTA);
	MCC_TEST_SET_FIELD(record, default_requested_bytes,
			   MCC_MCACHE_DEFAULT_REQUESTED_BYTES);
}

static void apple_mcc_test_stream_selection(struct kunit *test)
{
	u8 records[2 * sizeof(struct apple_mcc_mcache_stream_record)] = {};
	u8 *target = records + sizeof(struct apple_mcc_mcache_stream_record);
	u32 dsid = U32_MAX;

	MCC_TEST_SET_FIELD(records, stream_id, 1);
	apple_mcc_test_valid_stream(target, 7);

	KUNIT_EXPECT_EQ(test,
			apple_mcc_parse_stream_dsid(records, sizeof(records), &dsid), 0);
	KUNIT_EXPECT_EQ(test, dsid, 7U);
}

static void apple_mcc_test_stream_refusal(struct kunit *test)
{
	u8 records[2 * sizeof(struct apple_mcc_mcache_stream_record)] = {};
	u8 *duplicate = records + sizeof(struct apple_mcc_mcache_stream_record);
	size_t record_size = sizeof(struct apple_mcc_mcache_stream_record);
	u32 dsid;

	apple_mcc_test_valid_stream(records, 7);
	MCC_TEST_SET_FIELD(records, last_quota_group, 2);
	KUNIT_EXPECT_EQ(test,
			apple_mcc_parse_stream_dsid(records, record_size, &dsid),
			-EINVAL);

	apple_mcc_test_valid_stream(records, 7);
	MCC_TEST_SET_FIELD(records, default_requested_bytes, 1);
	KUNIT_EXPECT_EQ(test,
			apple_mcc_parse_stream_dsid(records, record_size, &dsid),
			-EINVAL);

	apple_mcc_test_valid_stream(records, 7);
	apple_mcc_test_valid_stream(duplicate, 7);
	KUNIT_EXPECT_EQ(test,
			apple_mcc_parse_stream_dsid(records, sizeof(records), &dsid),
			-EINVAL);
}

static void apple_mcc_test_partial_enable(struct kunit *test)
{
	struct apple_mcc_test *ctx = apple_mcc_test_alloc(test);
	struct apple_mcc *mcc = &ctx->mcc;
	unsigned int i;

	ctx->reject_plane = 1;
	KUNIT_EXPECT_EQ(test, apple_mcc_enable(mcc), -EIO);
	KUNIT_EXPECT_EQ(test, ctx->attempted, BIT(0) | BIT(1));
	/* The failed plane's state is uncertain, so include it in cleanup. */
	KUNIT_EXPECT_EQ(test, ctx->disabled, BIT(0) | BIT(1));
	KUNIT_EXPECT_EQ(test, ctx->flushed, BIT(0) | BIT(1));
	KUNIT_EXPECT_FALSE(test, mcc->enabled);
	for (i = 0; i < mcc->nr_planes; i++)
		KUNIT_EXPECT_EQ(test, readl(mcc->planes[i] + MCC_ALLOC_DISABLE), 1U);
}

static void apple_mcc_test_quiesce(struct kunit *test)
{
	struct apple_mcc_test *ctx = apple_mcc_test_alloc(test);
	struct apple_mcc *mcc = &ctx->mcc;

	KUNIT_ASSERT_EQ(test, apple_mcc_enable(mcc), 0);
	mcc->enabled = true;
	apple_mcc_quiesce(mcc);
	KUNIT_EXPECT_EQ(test, ctx->attempted, 7UL);
	KUNIT_EXPECT_EQ(test, ctx->disabled, 7UL);
	KUNIT_EXPECT_EQ(test, ctx->flushed, 7UL);
	KUNIT_EXPECT_FALSE(test, mcc->enabled);
}

static void apple_mcc_test_not_ready(struct kunit *test)
{
	struct apple_mcc_test *ctx = apple_mcc_test_alloc(test);
	struct apple_mcc *mcc = &ctx->mcc;

	writel(0, mcc->planes[1] + MCC_CACHE_WAYS);
	KUNIT_EXPECT_EQ(test, apple_mcc_enable(mcc), -EBUSY);
	KUNIT_EXPECT_EQ(test, ctx->attempted, 0UL);
	KUNIT_EXPECT_EQ(test, ctx->disabled, 0UL);
	KUNIT_EXPECT_EQ(test, ctx->flushed, 0UL);
}

static struct kunit_case apple_mcc_test_cases[] = {
	KUNIT_CASE(apple_mcc_test_stream_selection),
	KUNIT_CASE(apple_mcc_test_stream_refusal),
	KUNIT_CASE(apple_mcc_test_lifecycle_release),
	KUNIT_CASE(apple_mcc_test_partial_enable),
	KUNIT_CASE(apple_mcc_test_quiesce),
	KUNIT_CASE(apple_mcc_test_not_ready),
	{}
};

static struct kunit_suite apple_mcc_test_suite = {
	.name = "apple-mcc",
	.test_cases = apple_mcc_test_cases,
};

kunit_test_suite(apple_mcc_test_suite);
