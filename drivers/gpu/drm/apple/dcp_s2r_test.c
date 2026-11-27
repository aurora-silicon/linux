// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* Included by dcp.c to verify DART wake ordering on s2r_disp_dart_with_dcp pipelines. */
#include <kunit/static_stub.h>
#include <kunit/test.h>

enum dcp_s2r_test_op {
	DCP_S2R_TEST_GAPF = 1,
	DCP_S2R_TEST_EXT_DOMAINS_ON,
	DCP_S2R_TEST_DOMAINS_ON,
	DCP_S2R_TEST_POWER_DARK_OFF,
	DCP_S2R_TEST_DCP_STREAMS_ON,
	DCP_S2R_TEST_DISP_STREAMS_ON,
	DCP_S2R_TEST_EXT_RTKIT_CYCLED,
	DCP_S2R_TEST_EXT_RTKIT_SLEEP,
	DCP_S2R_TEST_WARM_RTKIT_BOOT,
};

struct dcp_s2r_test_ctx {
	struct apple_dcp *dcp;
	struct device dev;
	struct device dcp_dart;
	struct device disp_dart;
	struct device_node node;
	enum dcp_s2r_test_op ops[8];
	u32 dcp_streams;
	u32 disp_streams;
	unsigned int nops;
};

static struct dcp_s2r_test_ctx *dcp_s2r_test_current(struct kunit **out_test)
{
	struct kunit *test = kunit_get_current_test();

	if (out_test)
		*out_test = test;
	return test ? test->priv : NULL;
}

static void dcp_s2r_test_push(struct dcp_s2r_test_ctx *ctx, enum dcp_s2r_test_op op)
{
	struct kunit *test = kunit_get_current_test();

	KUNIT_ASSERT_LT(test, ctx->nops, (unsigned int)ARRAY_SIZE(ctx->ops));
	ctx->ops[ctx->nops++] = op;
}

static int dcp_s2r_test_gapf_ready(struct device *dev)
{
	struct kunit *test;
	struct dcp_s2r_test_ctx *ctx = dcp_s2r_test_current(&test);

	KUNIT_EXPECT_PTR_EQ(test, dev, &ctx->dcp_dart);
	dcp_s2r_test_push(ctx, DCP_S2R_TEST_GAPF);
	return 0;
}

static int dcp_s2r_test_ext_domains_on(struct apple_dcp *dcp)
{
	struct kunit *test;
	struct dcp_s2r_test_ctx *ctx = dcp_s2r_test_current(&test);

	KUNIT_EXPECT_PTR_EQ(test, dcp, ctx->dcp);
	dcp_s2r_test_push(ctx, DCP_S2R_TEST_EXT_DOMAINS_ON);
	return 0;
}

static int dcp_s2r_test_domains_on(struct apple_dcp *dcp)
{
	struct kunit *test;
	struct dcp_s2r_test_ctx *ctx = dcp_s2r_test_current(&test);

	KUNIT_EXPECT_PTR_EQ(test, dcp, ctx->dcp);
	dcp_s2r_test_push(ctx, DCP_S2R_TEST_DOMAINS_ON);
	return 0;
}

static int dcp_s2r_test_power_dark(struct device *dev, bool dark)
{
	struct kunit *test;
	struct dcp_s2r_test_ctx *ctx = dcp_s2r_test_current(&test);

	KUNIT_EXPECT_PTR_EQ(test, dev, &ctx->disp_dart);
	KUNIT_EXPECT_FALSE(test, dark);
	dcp_s2r_test_push(ctx, DCP_S2R_TEST_POWER_DARK_OFF);
	return 0;
}

static int dcp_s2r_test_display_streams(struct device *dev, u32 streams, bool on, bool flush)
{
	struct kunit *test;
	struct dcp_s2r_test_ctx *ctx = dcp_s2r_test_current(&test);

	KUNIT_EXPECT_TRUE(test, on);
	KUNIT_EXPECT_FALSE(test, flush);
	if (dev == &ctx->dcp_dart) {
		ctx->dcp_streams = streams;
		dcp_s2r_test_push(ctx, DCP_S2R_TEST_DCP_STREAMS_ON);
	} else {
		KUNIT_EXPECT_PTR_EQ(test, dev, &ctx->disp_dart);
		ctx->disp_streams = streams;
		dcp_s2r_test_push(ctx, DCP_S2R_TEST_DISP_STREAMS_ON);
	}
	return 0;
}

static int dcp_s2r_test_ext_rtkit_wake(struct apple_dcp *dcp, void __iomem *control, bool cycled)
{
	struct kunit *test;
	struct dcp_s2r_test_ctx *ctx = dcp_s2r_test_current(&test);

	KUNIT_EXPECT_PTR_EQ(test, dcp, ctx->dcp);
	dcp_s2r_test_push(ctx, cycled ? DCP_S2R_TEST_EXT_RTKIT_CYCLED :
					DCP_S2R_TEST_EXT_RTKIT_SLEEP);
	return 0;
}

static int dcp_s2r_test_warm_rtkit_boot(struct apple_dcp *dcp)
{
	struct kunit *test;
	struct dcp_s2r_test_ctx *ctx = dcp_s2r_test_current(&test);

	KUNIT_EXPECT_PTR_EQ(test, dcp, ctx->dcp);
	dcp_s2r_test_push(ctx, DCP_S2R_TEST_WARM_RTKIT_BOOT);
	return 0;
}

static struct dcp_s2r_test_ctx *dcp_s2r_test_setup(struct kunit *test,
						   const struct apple_dcp_hw_data *hw)
{
	struct dcp_s2r_test_ctx *ctx = kunit_kzalloc(test, sizeof(*ctx), GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, ctx);
	ctx->dcp = kunit_kzalloc(test, sizeof(*ctx->dcp), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, ctx->dcp);

	ctx->node.name = "dcp";
	ctx->node.full_name = "dcp";
	ctx->dev.init_name = "dcp-s2r-test";
	ctx->dev.of_node = &ctx->node;
	ctx->dcp_dart.init_name = "dcp-s2r-dcp-dart";
	ctx->disp_dart.init_name = "dcp-s2r-disp-dart";

	ctx->dcp->dev = &ctx->dev;
	ctx->dcp->hw = *hw;
	ctx->dcp->rtk = (struct apple_rtkit *)1;
	ctx->dcp->s2r_dcp_dart = &ctx->dcp_dart;
	ctx->dcp->s2r_disp_dart = &ctx->disp_dart;
	test->priv = ctx;

	kunit_activate_static_stub(test, dcp_s2r_dart_gapf_ready, dcp_s2r_test_gapf_ready);
	kunit_activate_static_stub(test, dcp_s2r_ext_domains_on, dcp_s2r_test_ext_domains_on);
	kunit_activate_static_stub(test, dcp_s2r_domains_on, dcp_s2r_test_domains_on);
	kunit_activate_static_stub(test, dcp_s2r_dart_power_dark, dcp_s2r_test_power_dark);
	kunit_activate_static_stub(test, dcp_s2r_dart_display_streams,
				   dcp_s2r_test_display_streams);
	kunit_activate_static_stub(test, dcp_s2r_ext_rtkit_wake, dcp_s2r_test_ext_rtkit_wake);
	kunit_activate_static_stub(test, dcp_s2r_warm_rtkit_boot, dcp_s2r_test_warm_rtkit_boot);

	return ctx;
}

/*
 * T8103 DCPExt: dcp_s2r_ext_start() must not clear display_dark before
 * apple_dart_display_streams(s2r_disp_dart, ..., true, false) restores TTBR/TCR
 * and runs the post-restore TLB invalidation on dark exit.
 */
static void dcp_s2r_t8103_dcpext_wake_order(struct kunit *test)
{
	struct dcp_s2r_test_ctx *ctx = dcp_s2r_test_setup(test, &apple_dcp_hw_t8103_dcpext);

	ctx->dcp->ext_idle_domains_off = true;
	KUNIT_ASSERT_EQ(test, dcp_s2r_ext_start(ctx->dcp, true), 0);
	KUNIT_EXPECT_FALSE(test, ctx->dcp->ext_idle_domains_off);
	KUNIT_EXPECT_EQ(test, ctx->nops, 5U);
	KUNIT_EXPECT_EQ(test, ctx->ops[0], DCP_S2R_TEST_GAPF);
	KUNIT_EXPECT_EQ(test, ctx->ops[1], DCP_S2R_TEST_EXT_DOMAINS_ON);
	KUNIT_EXPECT_EQ(test, ctx->ops[2], DCP_S2R_TEST_DCP_STREAMS_ON);
	KUNIT_EXPECT_EQ(test, ctx->ops[3], DCP_S2R_TEST_DISP_STREAMS_ON);
	KUNIT_EXPECT_EQ(test, ctx->ops[4], DCP_S2R_TEST_EXT_RTKIT_CYCLED);
	KUNIT_EXPECT_EQ(test, ctx->dcp_streams, dcp_s2r_t8103_ext.dcp_streams);
	KUNIT_EXPECT_EQ(test, ctx->disp_streams, dcp_s2r_t8103_ext.disp_streams);

	ctx->nops = 0;
	ctx->dcp->ext_idle_domains_off = true;
	KUNIT_ASSERT_EQ(test, dcp_s2r_ext_start(ctx->dcp, false), 0);
	KUNIT_EXPECT_FALSE(test, ctx->dcp->ext_idle_domains_off);
	KUNIT_EXPECT_EQ(test, ctx->nops, 5U);
	KUNIT_EXPECT_EQ(test, ctx->ops[2], DCP_S2R_TEST_DCP_STREAMS_ON);
	KUNIT_EXPECT_EQ(test, ctx->ops[3], DCP_S2R_TEST_DISP_STREAMS_ON);
	KUNIT_EXPECT_EQ(test, ctx->ops[4], DCP_S2R_TEST_EXT_RTKIT_SLEEP);
}

/* External DCPs without s2r_disp_dart_with_dcp clear dark mode after domain power-on. */
static void dcp_s2r_t8140_dcpext_wake_order(struct kunit *test)
{
	struct dcp_s2r_test_ctx *ctx = dcp_s2r_test_setup(test, &apple_dcp_hw_t8140_external);

	KUNIT_ASSERT_EQ(test, dcp_s2r_ext_start(ctx->dcp, true), 0);
	KUNIT_EXPECT_EQ(test, ctx->nops, 5U);
	KUNIT_EXPECT_EQ(test, ctx->ops[0], DCP_S2R_TEST_GAPF);
	KUNIT_EXPECT_EQ(test, ctx->ops[1], DCP_S2R_TEST_EXT_DOMAINS_ON);
	KUNIT_EXPECT_EQ(test, ctx->ops[2], DCP_S2R_TEST_POWER_DARK_OFF);
	KUNIT_EXPECT_EQ(test, ctx->ops[3], DCP_S2R_TEST_DCP_STREAMS_ON);
	KUNIT_EXPECT_EQ(test, ctx->ops[4], DCP_S2R_TEST_EXT_RTKIT_CYCLED);
	KUNIT_EXPECT_EQ(test, ctx->dcp_streams, dcp_s2r_t8140.dcp_streams);
}

/* T8103 internal DCP warm restart keeps the same wake ordering. */
static void dcp_s2r_t8103_warm_restart_wake_order(struct kunit *test)
{
	struct dcp_s2r_test_ctx *ctx = dcp_s2r_test_setup(test, &apple_dcp_hw_t8103);

	KUNIT_ASSERT_EQ(test, dcp_s2r_warm_restart(ctx->dcp), 0);
	KUNIT_EXPECT_EQ(test, ctx->nops, 4U);
	KUNIT_EXPECT_EQ(test, ctx->ops[0], DCP_S2R_TEST_DOMAINS_ON);
	KUNIT_EXPECT_EQ(test, ctx->ops[1], DCP_S2R_TEST_DCP_STREAMS_ON);
	KUNIT_EXPECT_EQ(test, ctx->ops[2], DCP_S2R_TEST_DISP_STREAMS_ON);
	KUNIT_EXPECT_EQ(test, ctx->ops[3], DCP_S2R_TEST_WARM_RTKIT_BOOT);
	KUNIT_EXPECT_EQ(test, ctx->dcp_streams, dcp_s2r_t8103.dcp_streams);
	KUNIT_EXPECT_EQ(test, ctx->disp_streams, dcp_s2r_t8103.disp_streams);
}

static struct kunit_case dcp_s2r_cases[] = {
	KUNIT_CASE(dcp_s2r_t8103_dcpext_wake_order),
	KUNIT_CASE(dcp_s2r_t8140_dcpext_wake_order),
	KUNIT_CASE(dcp_s2r_t8103_warm_restart_wake_order),
	{}
};

static struct kunit_suite dcp_s2r_suite = {
	.name = "apple-dcp-s2r",
	.test_cases = dcp_s2r_cases,
};

kunit_test_suite(dcp_s2r_suite);
