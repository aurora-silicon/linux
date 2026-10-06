// SPDX-License-Identifier: GPL-2.0-only OR MIT
#include <kunit/test.h>
#include <linux/module.h>

#include "epic/dpavservep-edid.h"

static int dcpavserv_edid_test_init(struct kunit *test)
{
	struct dpavserv_copy_edid_resp *resp;

	resp = kunit_kzalloc(test, sizeof(*resp) + DPAVSERV_EDID_BUF_SIZE,
			     GFP_KERNEL);
	if (!resp)
		return -ENOMEM;
	resp->max_size = cpu_to_le64(DPAVSERV_EDID_BUF_SIZE);
	resp->used_size = cpu_to_le64(DPAVSERV_EDID_LEADING_SIZE +
				    DPAVSERV_EDID_BLOCK_SIZE);
	test->priv = resp;
	return 0;
}

static void dcpavserv_edid_short_reply_test(struct kunit *test)
{
	struct dpavserv_copy_edid_resp *resp = test->priv;
	size_t edid_size = SIZE_MAX;
	size_t reply_size;

	for (reply_size = 0; reply_size < sizeof(*resp); reply_size++) {
		KUNIT_EXPECT_EQ(test, dcpavserv_edid_size(resp, reply_size, &edid_size),
				-EIO);
		KUNIT_EXPECT_EQ(test, edid_size, SIZE_MAX);
	}
	reply_size = sizeof(*resp) + le64_to_cpu(resp->used_size);
	KUNIT_EXPECT_EQ(test, dcpavserv_edid_size(resp, sizeof(*resp), &edid_size),
			-EIO);
	KUNIT_EXPECT_EQ(test, dcpavserv_edid_size(resp, reply_size - 1, &edid_size),
			-EIO);
	KUNIT_EXPECT_EQ(test, edid_size, SIZE_MAX);
	KUNIT_EXPECT_EQ(test, dcpavserv_edid_size(resp, reply_size, &edid_size), 0);
	KUNIT_EXPECT_EQ(test, edid_size, (size_t)DPAVSERV_EDID_BLOCK_SIZE);
}

static void dcpavserv_edid_declared_size_test(struct kunit *test)
{
	struct dpavserv_copy_edid_resp *resp = test->priv;
	const u64 invalid_sizes[] = {
		0,
		DPAVSERV_EDID_LEADING_SIZE + DPAVSERV_EDID_BLOCK_SIZE - 1,
		DPAVSERV_EDID_BUF_SIZE + 1,
		U64_MAX,
	};
	size_t edid_size = SIZE_MAX;
	size_t reply_size = sizeof(*resp) + DPAVSERV_EDID_BUF_SIZE;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(invalid_sizes); i++) {
		resp->used_size = cpu_to_le64(invalid_sizes[i]);
		KUNIT_EXPECT_EQ(test, dcpavserv_edid_size(resp, reply_size, &edid_size),
				-EIO);
		KUNIT_EXPECT_EQ(test, edid_size, SIZE_MAX);
	}
	resp->used_size = cpu_to_le64(DPAVSERV_EDID_LEADING_SIZE +
				    DPAVSERV_EDID_BLOCK_SIZE);
	resp->max_size = cpu_to_le64(DPAVSERV_EDID_BUF_SIZE - 1);
	KUNIT_EXPECT_EQ(test, dcpavserv_edid_size(resp, reply_size, &edid_size), -EIO);
	KUNIT_EXPECT_EQ(test, edid_size, SIZE_MAX);
}

static void dcpavserv_edid_whole_blocks_test(struct kunit *test)
{
	struct dpavserv_copy_edid_resp *resp = test->priv;
	size_t edid_size = SIZE_MAX;
	size_t data_size = DPAVSERV_EDID_LEADING_SIZE + 2 * DPAVSERV_EDID_BLOCK_SIZE;
	size_t reply_size = sizeof(*resp) + data_size;
	u8 *extensions = &resp->data[DPAVSERV_EDID_LEADING_SIZE +
				     DPAVSERV_EDID_EXT_COUNT_OFFSET];

	resp->used_size = cpu_to_le64(data_size);
	*extensions = 1;
	KUNIT_EXPECT_EQ(test, dcpavserv_edid_size(resp, reply_size, &edid_size), 0);
	KUNIT_EXPECT_EQ(test, edid_size, (size_t)2 * DPAVSERV_EDID_BLOCK_SIZE);
	edid_size = SIZE_MAX;
	*extensions = 2;
	KUNIT_EXPECT_EQ(test, dcpavserv_edid_size(resp, reply_size, &edid_size), -EIO);
	KUNIT_EXPECT_EQ(test, edid_size, SIZE_MAX);
	*extensions = 0;
	resp->used_size = cpu_to_le64(data_size - 1);
	KUNIT_EXPECT_EQ(test, dcpavserv_edid_size(resp, reply_size, &edid_size), -EIO);
	KUNIT_EXPECT_EQ(test, edid_size, SIZE_MAX);
}

static void dcpavserv_edid_extended_count_test(struct kunit *test)
{
	struct dpavserv_copy_edid_resp *resp = test->priv;
	size_t edid_size = SIZE_MAX;
	size_t data_size = DPAVSERV_EDID_LEADING_SIZE + 3 * DPAVSERV_EDID_BLOCK_SIZE;
	u8 *edid = resp->data + DPAVSERV_EDID_LEADING_SIZE;

	resp->used_size = cpu_to_le64(data_size);
	/* The base count remains one when CTA supplies an extended count. */
	edid[DPAVSERV_EDID_EXT_COUNT_OFFSET] = 1;
	edid[128] = 0x02;
	edid[129] = 0x03;
	edid[130] = 7;
	edid[132] = 0xe2;
	edid[133] = 0x78;
	edid[134] = 2;
	KUNIT_EXPECT_EQ(test, dcpavserv_edid_size(resp, sizeof(*resp) + data_size,
						  &edid_size), 0);
	KUNIT_EXPECT_EQ(test, edid_size, (size_t)3 * DPAVSERV_EDID_BLOCK_SIZE);
}

static void dcpavserv_edid_maximum_test(struct kunit *test)
{
	struct dpavserv_copy_edid_resp *resp = test->priv;
	size_t edid_size = SIZE_MAX;
	size_t reply_size = sizeof(*resp) + DPAVSERV_EDID_BUF_SIZE;

	resp->used_size = cpu_to_le64(DPAVSERV_EDID_BUF_SIZE);
	resp->data[DPAVSERV_EDID_LEADING_SIZE + DPAVSERV_EDID_EXT_COUNT_OFFSET] = 255;
	KUNIT_EXPECT_EQ(test, dcpavserv_edid_size(resp, reply_size, &edid_size), 0);
	KUNIT_EXPECT_EQ(test, edid_size, (size_t)DPAVSERV_EDID_MAX_SIZE);
	edid_size = SIZE_MAX;
	KUNIT_EXPECT_EQ(test, dcpavserv_edid_size(resp, reply_size - 1, &edid_size),
			-EIO);
	KUNIT_EXPECT_EQ(test, edid_size, SIZE_MAX);
}

static struct kunit_case dcpavserv_edid_cases[] = {
	KUNIT_CASE(dcpavserv_edid_short_reply_test),
	KUNIT_CASE(dcpavserv_edid_declared_size_test),
	KUNIT_CASE(dcpavserv_edid_whole_blocks_test),
	KUNIT_CASE(dcpavserv_edid_extended_count_test),
	KUNIT_CASE(dcpavserv_edid_maximum_test),
	{ }
};

static struct kunit_suite dcpavserv_edid_suite = {
	.name = "apple-display-edid-bounds",
	.init = dcpavserv_edid_test_init,
	.test_cases = dcpavserv_edid_cases,
};

kunit_test_suite(dcpavserv_edid_suite);
MODULE_LICENSE("Dual MIT/GPL");
