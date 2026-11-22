// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * Included by dcp.c: the boot loader handoff state the driver reads before
 * it starts a display coprocessor. dcp_external_is_idle() runs on a fake
 * coprocessor register window; boot_show_notch() on /chosen, with the
 * property applied and reverted through an OF changeset.
 */
#include <kunit/test.h>
#include <linux/of.h>

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

	/* Other bits in either register do not change the answer. */
	KUNIT_EXPECT_TRUE(test, dcp_external_is_idle(dcp_handoff_coproc(test, ~0u, ~0u)));
	KUNIT_EXPECT_FALSE(test, dcp_external_is_idle(dcp_handoff_coproc(test, ~run, ~0u)));
	KUNIT_EXPECT_FALSE(test, dcp_external_is_idle(dcp_handoff_coproc(test, ~0u, ~idle)));
	KUNIT_EXPECT_FALSE(test, dcp_external_is_idle(dcp_handoff_coproc(test, ~0u,
									   ~stopped)));
}

/* boot_show_notch() with /chosen/asahi,show-notch set by @add, or absent. */
static bool dcp_handoff_notch(struct kunit *test,
			      int (*add)(struct of_changeset *cs, struct device_node *np))
{
	struct of_changeset cs;
	bool ret;

	if (!add)
		return boot_show_notch();
	of_changeset_init(&cs);
	KUNIT_ASSERT_EQ(test, add(&cs, of_chosen), 0);
	KUNIT_ASSERT_EQ(test, of_changeset_apply(&cs), 0);
	ret = boot_show_notch();
	KUNIT_EXPECT_EQ(test, of_changeset_revert(&cs), 0);
	of_changeset_destroy(&cs);
	return ret;
}

#define DCP_HANDOFF_NOTCH_STRING(tag, value)						\
static int dcp_handoff_notch_##tag(struct of_changeset *cs, struct device_node *np)	\
{											\
	return of_changeset_add_prop_string(cs, np, "asahi,show-notch", value);		\
}

DCP_HANDOFF_NOTCH_STRING(one, "1")
DCP_HANDOFF_NOTCH_STRING(zero, "0")
DCP_HANDOFF_NOTCH_STRING(empty, "")
DCP_HANDOFF_NOTCH_STRING(ten, "10")
DCP_HANDOFF_NOTCH_STRING(yes, "yes")

static int dcp_handoff_notch_u32(struct of_changeset *cs, struct device_node *np)
{
	return of_changeset_add_prop_u32(cs, np, "asahi,show-notch", 1);
}

static int dcp_handoff_notch_bool(struct of_changeset *cs, struct device_node *np)
{
	return of_changeset_add_prop_bool(cs, np, "asahi,show-notch");
}

static void dcp_handoff_boot_notch(struct kunit *test)
{
	if (!of_chosen)
		kunit_skip(test, "no /chosen node");
	if (of_property_present(of_chosen, "asahi,show-notch"))
		kunit_skip(test, "/chosen already has asahi,show-notch");

	/* Absent: the driver keeps the notch rows out of the mode. */
	KUNIT_EXPECT_FALSE(test, dcp_handoff_notch(test, NULL));
	/* Only the string "1" the boot loader writes shows the notch. */
	KUNIT_EXPECT_TRUE(test, dcp_handoff_notch(test, dcp_handoff_notch_one));
	KUNIT_EXPECT_FALSE(test, dcp_handoff_notch(test, dcp_handoff_notch_zero));
	KUNIT_EXPECT_FALSE(test, dcp_handoff_notch(test, dcp_handoff_notch_empty));
	KUNIT_EXPECT_FALSE(test, dcp_handoff_notch(test, dcp_handoff_notch_ten));
	KUNIT_EXPECT_FALSE(test, dcp_handoff_notch(test, dcp_handoff_notch_yes));
	/* A cell or an empty property is not the string "1". */
	KUNIT_EXPECT_FALSE(test, dcp_handoff_notch(test, dcp_handoff_notch_u32));
	KUNIT_EXPECT_FALSE(test, dcp_handoff_notch(test, dcp_handoff_notch_bool));
	/* Reverted: absent again. */
	KUNIT_EXPECT_FALSE(test, dcp_handoff_notch(test, NULL));
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
