// SPDX-License-Identifier: GPL-2.0-only
#include <kunit/test.h>
#include "apple-admac-payload.h"

struct payload_test {
	struct admac_payload payload;
	u32 source[16];
	u32 copies[ADMAC_PAYLOAD_SLOTS][4];
	u64 issued;
	u64 retired;
	bool fail;
	bool fault;
};

static int payload_process(void *context, void *samples, size_t bytes, u64 *ticket)
{
	struct payload_test *t = context;
	u32 *words = samples;
	unsigned int k;

	if (t->fail)
		return -EIO;
	for (k = 0; k < bytes / sizeof(*words); k++)
		words[k] /= 2;
	*ticket = ++t->issued;
	return 0;
}

static int payload_retire(void *context, u64 ticket)
{
	struct payload_test *t = context;

	if (ticket != t->retired + 1)
		return -ESTALE;
	t->retired = ticket;
	return 0;
}

static void payload_fault(void *context)
{
	struct payload_test *t = context;

	t->fault = true;
}

static struct payload_test *payload_new(struct kunit *test)
{
	struct payload_test *t = kunit_kzalloc(test, sizeof(*t), GFP_KERNEL);
	unsigned int k;

	KUNIT_ASSERT_NOT_NULL(test, t);
	for (k = 0; k < ARRAY_SIZE(t->source); k++)
		t->source[k] = 100 + k;
	t->payload.area = t->copies;
	t->payload.dma = 0x100000;
	t->payload.filter = (struct apple_admac_playback_filter) {
		.source = t->source,
		.buffer_bytes = sizeof(t->source),
		.period_bytes = sizeof(t->copies[0]),
		.context = t,
		.process = payload_process,
		.retire = payload_retire,
		.fault = payload_fault,
	};
	return t;
}

static void immutable_copy_and_repeat(struct kunit *test)
{
	struct payload_test *t = payload_new(test);
	dma_addr_t first, second;

	KUNIT_ASSERT_EQ(test, admac_payload_prepare(&t->payload, 0, &first), 0);
	KUNIT_EXPECT_EQ(test, first, (dma_addr_t)0x100000);
	KUNIT_EXPECT_EQ(test, t->copies[0][0], 50U);
	t->source[0] = 1000; /* mmap changes the raw ring after submission */
	KUNIT_EXPECT_EQ(test, t->copies[0][0], 50U);
	KUNIT_ASSERT_EQ(test, admac_payload_prepare(&t->payload, 0, &second), 0);
	KUNIT_EXPECT_NE(test, first, second);
	KUNIT_EXPECT_EQ(test, t->copies[1][0], 500U);
	KUNIT_EXPECT_EQ(test, t->issued, 2ULL);
	KUNIT_ASSERT_EQ(test, admac_payload_retire(&t->payload), 0);
	KUNIT_ASSERT_EQ(test, admac_payload_retire(&t->payload), 0);
	KUNIT_EXPECT_EQ(test, admac_payload_retire(&t->payload), -ESTALE);
}

static void active_fifo_and_completion_lag_never_reuse(struct kunit *test)
{
	struct payload_test *t = payload_new(test);
	dma_addr_t address;
	u32 saved[ADMAC_PAYLOAD_SLOTS][4];
	unsigned int k;

	for (k = 0; k < ADMAC_PAYLOAD_SLOTS; k++) {
		t->source[0] = 100 + k * 10;
		KUNIT_ASSERT_EQ(test, admac_payload_prepare(&t->payload, 0, &address), 0);
		KUNIT_EXPECT_EQ(test, address, t->payload.dma + k * sizeof(t->copies[0]));
	}
	memcpy(saved, t->copies, sizeof(saved));
	KUNIT_EXPECT_EQ(test, admac_payload_prepare(&t->payload, 0, &address), -ENOSPC);
	KUNIT_EXPECT_EQ(test, memcmp(saved, t->copies, sizeof(saved)), 0);
	KUNIT_ASSERT_EQ(test, admac_payload_retire(&t->payload), 0);
	t->source[0] = 2000;
	KUNIT_ASSERT_EQ(test, admac_payload_prepare(&t->payload, 0, &address), 0);
	KUNIT_EXPECT_EQ(test, address, t->payload.dma);
	KUNIT_EXPECT_EQ(test, t->copies[0][0], 1000U);
	KUNIT_EXPECT_EQ(test, memcmp(saved[1], t->copies[1], sizeof(saved) - sizeof(saved[0])), 0);
}

static void failed_process_never_publishes(struct kunit *test)
{
	struct payload_test *t = payload_new(test);
	dma_addr_t address = 123;

	t->fail = true;
	KUNIT_EXPECT_EQ(test, admac_payload_prepare(&t->payload, 0, &address), -EIO);
	KUNIT_EXPECT_EQ(test, address, (dma_addr_t)123);
	KUNIT_EXPECT_EQ(test, t->payload.pending, 0);
	KUNIT_EXPECT_EQ(test, t->issued, 0ULL);
	KUNIT_EXPECT_EQ(test, admac_payload_prepare(&t->payload, SIZE_MAX, &address), -EINVAL);
}

static void failed_stop_retains_storage_and_debt(struct kunit *test)
{
	struct payload_test *t = payload_new(test);
	dma_addr_t address;
	u32 saved[4];

	KUNIT_ASSERT_EQ(test, admac_payload_prepare(&t->payload, 0, &address), 0);
	memcpy(saved, t->copies[0], sizeof(saved));
	admac_payload_quarantine(&t->payload);
	KUNIT_EXPECT_TRUE(test, t->fault);
	KUNIT_EXPECT_EQ(test, admac_payload_retire(&t->payload), -EIO);
	KUNIT_EXPECT_EQ(test, admac_payload_prepare(&t->payload, 0, &address), -EIO);
	KUNIT_EXPECT_EQ(test, t->retired, 0ULL);
	KUNIT_EXPECT_EQ(test, t->payload.pending, 1);
	KUNIT_EXPECT_EQ(test, memcmp(saved, t->copies[0], sizeof(saved)), 0);
	admac_payload_quarantine(&t->payload);
}

static struct kunit_case payload_cases[] = {
	KUNIT_CASE(immutable_copy_and_repeat),
	KUNIT_CASE(active_fifo_and_completion_lag_never_reuse),
	KUNIT_CASE(failed_process_never_publishes),
	KUNIT_CASE(failed_stop_retains_storage_and_debt),
	{}
};

static struct kunit_suite payload_suite = {
	.name = "apple-admac-payload",
	.test_cases = payload_cases,
};

kunit_test_suite(payload_suite);
MODULE_LICENSE("GPL");
