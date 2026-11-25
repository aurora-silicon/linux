// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * Included by dcp.c: the boot loader handoff state the driver reads before
 * it starts a display coprocessor. dcp_external_is_idle() runs on a fake
 * coprocessor register window; dcp_boot_show_notch(), which boot_show_notch()
 * applies to /chosen, on nodes of the test's own. Nothing here reads or
 * writes the device tree, so the suite is safe next to a probing display.
 */
#include <kunit/test.h>
#include <linux/of.h>

#include "dcp-lifecycle.h"

#define DCP_HANDOFF_COPROC_SIZE	(APPLE_DCP_COPROC_CPU_STATUS + sizeof(u32))

static struct apple_dcp *dcp_handoff_coproc(struct kunit *test, u32 control, u32 status)
{
	struct apple_dcp *dcp = kunit_kzalloc(test, sizeof(*dcp), GFP_KERNEL);
	void *regs = kunit_kzalloc(test, DCP_HANDOFF_COPROC_SIZE, GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, dcp);
	KUNIT_ASSERT_NOT_NULL(test, regs);
	*(u32 *)(regs + APPLE_DCP_COPROC_CPU_CONTROL) = control;
	*(u32 *)(regs + APPLE_DCP_COPROC_CPU_STATUS) = status;
	dcp->coproc_reg = (void __iomem __force *)regs;
	return dcp;
}

static void dcp_handoff_external_idle(struct kunit *test)
{
	const u32 run = APPLE_DCP_COPROC_CPU_CONTROL_RUN;
	const u32 stopped = APPLE_DCP_COPROC_CPU_STATUS_STOPPED;
	const u32 idle = APPLE_DCP_COPROC_CPU_STATUS_IDLE;
	const u32 others = ~(u32)APPLE_DCP_COPROC_CPU_STATUS_RUNNING;
	unsigned int bits;

	/* Inherited idle: the CPU enabled, and stopped and idle in its status. */
	KUNIT_EXPECT_TRUE(test, dcp_external_is_idle(dcp_handoff_coproc(test, run,
									  stopped | idle)));

	/* Every other combination of the three bits is not. */
	for (bits = 0; bits < 7; bits++) {
		u32 control = bits & 1 ? run : 0;
		u32 status = (bits & 2 ? stopped : 0) | (bits & 4 ? idle : 0);

		KUNIT_EXPECT_FALSE_MSG(test,
				       dcp_external_is_idle(dcp_handoff_coproc(test, control,
									       status)),
				       "control %#x status %#x", control, status);
	}

	/*
	 * Other bits in either register do not change the answer. RUNNING is
	 * left out of them: apple_dcp_coprocessor_is_active() reads it as an
	 * active processor, and whether the check should refuse it next to
	 * STOPPED and IDLE is not this test's to pin.
	 */
	KUNIT_EXPECT_TRUE(test, dcp_external_is_idle(dcp_handoff_coproc(test, ~0u, others)));
	KUNIT_EXPECT_FALSE(test, dcp_external_is_idle(dcp_handoff_coproc(test, ~run, others)));
	KUNIT_EXPECT_FALSE(test, dcp_external_is_idle(dcp_handoff_coproc(test, ~0u,
									   others & ~idle)));
	KUNIT_EXPECT_FALSE(test, dcp_external_is_idle(dcp_handoff_coproc(test, ~0u,
									   others & ~stopped)));
}

/*
 * dcp_boot_show_notch() on a node of the test's own, never part of the
 * device tree, with asahi,show-notch holding @length bytes at @value.
 */
static bool dcp_handoff_notch(const void *value, int length)
{
	struct property prop = {
		.name = "asahi,show-notch",
		.length = length,
		.value = (void *)value,
	};
	struct device_node chosen = { .properties = &prop };

	return dcp_boot_show_notch(&chosen);
}

#define dcp_handoff_notch_string(s)	dcp_handoff_notch(s, sizeof(s))

static void dcp_handoff_boot_notch(struct kunit *test)
{
	static const char unterminated[] = { '1' };
	static const __be32 cell = cpu_to_be32(1);
	struct property other = {
		.name = "bootargs",
		.length = sizeof("1"),
		.value = "1",
	};
	struct device_node chosen = {};

	/* Absent, or no /chosen at all: the notch rows stay out of the mode. */
	KUNIT_EXPECT_FALSE(test, dcp_boot_show_notch(NULL));
	KUNIT_EXPECT_FALSE(test, dcp_boot_show_notch(&chosen));
	chosen.properties = &other;
	KUNIT_EXPECT_FALSE(test, dcp_boot_show_notch(&chosen));
	/* Only the string "1" the boot loader writes shows the notch. */
	KUNIT_EXPECT_TRUE(test, dcp_handoff_notch_string("1"));
	KUNIT_EXPECT_FALSE(test, dcp_handoff_notch_string("0"));
	KUNIT_EXPECT_FALSE(test, dcp_handoff_notch_string(""));
	KUNIT_EXPECT_FALSE(test, dcp_handoff_notch_string("10"));
	KUNIT_EXPECT_FALSE(test, dcp_handoff_notch_string("yes"));
	/* A cell, an empty property or an unterminated "1" is not that string. */
	KUNIT_EXPECT_FALSE(test, dcp_handoff_notch(&cell, sizeof(cell)));
	KUNIT_EXPECT_FALSE(test, dcp_handoff_notch(NULL, 0));
	KUNIT_EXPECT_FALSE(test, dcp_handoff_notch(unterminated, sizeof(unterminated)));
}

static struct kunit_case dcp_handoff_cases[] = {
	KUNIT_CASE(dcp_handoff_external_idle),
	KUNIT_CASE(dcp_handoff_boot_notch),
	{}
};

static struct kunit_suite dcp_handoff_suite = {
	.name = "apple-dcp-handoff",
	.test_cases = dcp_handoff_cases,
};

kunit_test_suite(dcp_handoff_suite);
