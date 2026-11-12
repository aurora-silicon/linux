// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * KUnit tests for the buffer size checks of DRM_IOCTL_ANE_SUBMIT.
 */

#include <kunit/test.h>
#include <linux/sizes.h>

#include "ane.h"

struct ane_submit_size_case {
	const char *desc;
	u64 tsk_size;
	u32 td_count;
	u32 td_size;
	size_t cmd_size;
	size_t btsp_size;
	int ret;
};

static const struct ane_submit_size_case ane_submit_size_cases[] = {
	{
		/* libane: the program in handles[0], one tile of bootstrap */
		.desc = "100 descriptors of 0x274 bytes",
		.tsk_size = 100 * 0x274, .td_count = 100, .td_size = 0x274,
		.cmd_size = SZ_64K + SZ_16K, .btsp_size = SZ_16K,
	},
	{
		.desc = "descriptors fill the task region",
		.tsk_size = 0x1000, .td_count = 0x400, .td_size = 4,
		.cmd_size = SZ_16K, .btsp_size = SZ_16K,
	},
	{
		.desc = "one descriptor past the task region",
		.tsk_size = 0x1000, .td_count = 0x401, .td_size = 4,
		.cmd_size = SZ_16K, .btsp_size = SZ_16K, .ret = -EINVAL,
	},
	{
		.desc = "65535 descriptors in one page",
		.tsk_size = 0x100, .td_count = 0xffff, .td_size = 4,
		.cmd_size = SZ_16K, .btsp_size = SZ_16K, .ret = -EINVAL,
	},
	{
		.desc = "task region one byte short of the buffer",
		.tsk_size = SZ_16K - 1, .td_count = 1, .td_size = 4,
		.cmd_size = SZ_16K, .btsp_size = SZ_16K,
	},
	{
		.desc = "task region as large as the buffer",
		.tsk_size = SZ_16K, .td_count = 1, .td_size = 4,
		.cmd_size = SZ_16K, .btsp_size = SZ_16K, .ret = -EINVAL,
	},
	{
		.desc = "first descriptor fills the bootstrap buffer",
		.tsk_size = SZ_16K, .td_count = 1, .td_size = SZ_16K,
		.cmd_size = SZ_32K, .btsp_size = SZ_16K,
	},
	{
		.desc = "first descriptor one word past the bootstrap buffer",
		.tsk_size = SZ_16K + 4, .td_count = 1, .td_size = SZ_16K + 4,
		.cmd_size = SZ_32K, .btsp_size = SZ_16K, .ret = -EINVAL,
	},
	{
		.desc = "largest descriptor array below 4 GiB",
		.tsk_size = 0x3fffULL * 0x40000, .td_count = 0x3fff,
		.td_size = 0x40000, .cmd_size = SZ_4G, .btsp_size = 0x40000,
	},
	{
		.desc = "descriptor array of 4 GiB",
		.tsk_size = SZ_4G, .td_count = 0x4000, .td_size = 0x40000,
		.cmd_size = SZ_4G + SZ_16K, .btsp_size = 0x40000,
		.ret = -EINVAL,
	},
};

KUNIT_ARRAY_PARAM_DESC(ane_submit_size, ane_submit_size_cases, desc);

static void ane_submit_sizes_test(struct kunit *test)
{
	const struct ane_submit_size_case *c = test->param_value;
	struct drm_ane_submit args = {
		.tsk_size = c->tsk_size,
		.td_count = c->td_count,
		.td_size = c->td_size,
	};

	KUNIT_EXPECT_EQ(test, ane_submit_check_sizes(&args, c->cmd_size,
						     c->btsp_size), c->ret);
}

static struct kunit_case ane_submit_test_cases[] = {
	KUNIT_CASE_PARAM(ane_submit_sizes_test, ane_submit_size_gen_params),
	{}
};

static struct kunit_suite ane_submit_test_suite = {
	.name = "ane-submit",
	.test_cases = ane_submit_test_cases,
};

struct ane_recoverable_case {
	const char *desc;
	bool tm_retention;
	unsigned int unproven;
	int err;
	u32 finished;
	bool aborted;
	bool recoverable;
};

static const struct ane_recoverable_case ane_recoverable_cases[] = {
	{ "T8103 timeout without events", false, 0, -ETIMEDOUT, 0, false,
	  true },
	{ "T8103 timeout without events after a recovery", false, 1,
	  -ETIMEDOUT, 0, false, false },
	{ "T600x timeout without events", true, 0, -ETIMEDOUT, 0, false,
	  false },
	{ "T600x timeout after a recovery", true, 1, -ETIMEDOUT, 0, false,
	  false },
	{ "T600x timeout with one finish line", true, 0, -ETIMEDOUT, 1, false,
	  true },
	{ "T600x task error with one finish line", true, 0, -EIO, 1, false,
	  true },
	{ "T600x bad event count", true, 0, -EIO, 0, false, true },
	{ "T600x task error after a recovery", true, 1, -EIO, 2, false, true },
	{ "T600x aborted timeout without events", true, 0, -ETIMEDOUT, 0,
	  true, true },
	{ "T600x aborted timeout after a recovery", true, 1, -ETIMEDOUT, 0,
	  true, true },
	{ "T600x aborted timeout, last allowed", true,
	  ANE_TM_ABORT_RECOVERIES_MAX - 1, -ETIMEDOUT, 0, true, true },
	{ "T600x aborted timeout, too many in a row", true,
	  ANE_TM_ABORT_RECOVERIES_MAX, -ETIMEDOUT, 0, true, false },
};

KUNIT_ARRAY_PARAM_DESC(ane_recoverable, ane_recoverable_cases, desc);

static void ane_recoverable_test(struct kunit *test)
{
	const struct ane_recoverable_case *c = test->param_value;

	KUNIT_EXPECT_EQ(test, ane_tm_failure_recoverable(c->tm_retention,
							 c->unproven, c->err,
							 c->finished,
							 c->aborted),
			c->recoverable);
}

static struct kunit_case ane_recovery_test_cases[] = {
	KUNIT_CASE_PARAM(ane_recoverable_test, ane_recoverable_gen_params),
	{}
};

static struct kunit_suite ane_recovery_test_suite = {
	.name = "ane-tm-recovery",
	.test_cases = ane_recovery_test_cases,
};

kunit_test_suites(&ane_submit_test_suite, &ane_recovery_test_suite);

MODULE_DESCRIPTION("KUnit tests for the Apple Neural Engine SUBMIT checks");
MODULE_LICENSE("Dual MIT/GPL");
MODULE_IMPORT_NS("EXPORTED_FOR_KUNIT_TESTING");
