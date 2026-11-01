// SPDX-License-Identifier: GPL-2.0-only OR MIT
#include <kunit/test.h>

#include "ane_fw_ids.h"

static void ane_fw_ids_restart_test(struct kunit *test)
{
	struct ane_fw_ids ids = {};
	u32 old, fresh;

	KUNIT_ASSERT_EQ(test, ane_fw_id_alloc(&ids, &old), 0);
	/* Firmware may reuse wire ID zero after reset; the kernel namespace
	 * is deliberately independent and survives cache invalidation.
	 */
	KUNIT_ASSERT_EQ(test, ane_fw_id_alloc(&ids, &fresh), 0);
	KUNIT_EXPECT_NE(test, old, fresh);
	KUNIT_EXPECT_NE(test, old, 0U);
	KUNIT_EXPECT_NE(test, fresh, U32_MAX);
}

static void ane_fw_ids_exhaustion_test(struct kunit *test)
{
	struct ane_fw_ids ids = { .next = U32_MAX - 1ULL };
	u32 key = 0;

	KUNIT_ASSERT_EQ(test, ane_fw_id_alloc(&ids, &key), 0);
	KUNIT_EXPECT_EQ(test, key, U32_MAX - 1);
	key = 123;
	KUNIT_EXPECT_EQ(test, ane_fw_id_alloc(&ids, &key), -ENOSPC);
	KUNIT_EXPECT_EQ(test, key, 123U);
	KUNIT_EXPECT_EQ(test, ane_fw_id_alloc(&ids, &key), -ENOSPC);
}

static void ane_fw_ids_overflow_test(struct kunit *test)
{
	struct ane_fw_ids ids = { .next = U64_MAX };
	u32 key = 123;

	KUNIT_EXPECT_EQ(test, ane_fw_id_alloc(&ids, &key), -ENOSPC);
	KUNIT_EXPECT_EQ(test, key, 123U);
	KUNIT_EXPECT_EQ(test, ids.next, U64_MAX);
}

static struct kunit_case ane_fw_ids_cases[] = {
	KUNIT_CASE(ane_fw_ids_restart_test),
	KUNIT_CASE(ane_fw_ids_exhaustion_test),
	KUNIT_CASE(ane_fw_ids_overflow_test),
	{}
};

static struct kunit_suite ane_fw_ids_suite = {
	.name = "ane-fw-ids",
	.test_cases = ane_fw_ids_cases,
};

kunit_test_suite(ane_fw_ids_suite);

MODULE_LICENSE("Dual MIT/GPL");
MODULE_DESCRIPTION("ANE firmware recovery identifier tests");
