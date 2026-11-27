// SPDX-License-Identifier: GPL-2.0-only
/*
 * Included by dcp.c: verify DPMS power-on retry behavior when PMC interface
 * restoration or post-start boot/display-on fails on the first unblank and
 * succeeds on a subsequent unblank.
 */
#include <kunit/device.h>
#include <kunit/test.h>

#define DCP_S2R_PMC_COPROC_SIZE	(APPLE_DCP_COPROC_CPU_STATUS + sizeof(u32))

struct dcp_s2r_pmc_test_ctx {
	struct apple_dcp dcp;
	u32 ps_regs[4];
	u32 coproc_regs[DCP_S2R_PMC_COPROC_SIZE / sizeof(u32)];
	unsigned int pmc_on_calls;
	unsigned int pmc_off_calls;
	unsigned int boot_calls;
	unsigned int display_on_calls;
	unsigned int fail_pmc_until;
	unsigned int fail_boot_until;
	unsigned int fail_display_until;
	u32 boot_cpu_control;
	u32 display_cpu_control;
};

static const struct dcp_s2r_layout dcp_s2r_pmc_test_layout = {
	.disp_sys = 0x4,
	.disp_fe = 0x8,
	.disp_cpu = 0xc,
};

static void dcp_s2r_pmc_test_noop(struct work_struct *work)
{
}

static int dcp_s2r_pmc_test_map_power(struct apple_dcp *dcp, void __iomem **ps,
				      void __iomem **pwrgate, void __iomem **pmc)
{
	struct dcp_s2r_pmc_test_ctx *ctx = container_of(dcp, struct dcp_s2r_pmc_test_ctx, dcp);

	*ps = (void __iomem __force *)ctx->ps_regs;
	*pwrgate = NULL;
	*pmc = NULL;
	return 0;
}

static void dcp_s2r_pmc_test_ps_sync(struct apple_dcp *dcp, void __iomem *reg)
{
	u32 val = readl(reg);

	val = (val & ~GENMASK(7, 4)) | FIELD_PREP(GENMASK(7, 4), val & GENMASK(3, 0));
	writel(val, reg);
}

static int dcp_s2r_pmc_test_pmc_check(struct apple_dcp *dcp)
{
	return 0;
}

static int dcp_s2r_pmc_test_pmc_off(struct apple_dcp *dcp)
{
	struct dcp_s2r_pmc_test_ctx *ctx = container_of(dcp, struct dcp_s2r_pmc_test_ctx, dcp);

	ctx->pmc_off_calls++;
	return 0;
}

static int dcp_s2r_pmc_test_pmc_on(struct apple_dcp *dcp)
{
	struct dcp_s2r_pmc_test_ctx *ctx = container_of(dcp, struct dcp_s2r_pmc_test_ctx, dcp);

	ctx->pmc_on_calls++;
	if (ctx->pmc_on_calls <= ctx->fail_pmc_until)
		return -EIO;
	return 0;
}

static int dcp_s2r_pmc_test_boot(struct apple_dcp *dcp)
{
	struct dcp_s2r_pmc_test_ctx *ctx = container_of(dcp, struct dcp_s2r_pmc_test_ctx, dcp);

	ctx->boot_calls++;
	ctx->boot_cpu_control = ctx->coproc_regs[APPLE_DCP_COPROC_CPU_CONTROL / 4];
	if (ctx->boot_calls <= ctx->fail_boot_until)
		return -ETIMEDOUT;
	return 0;
}

static int dcp_s2r_pmc_test_display_on(struct apple_dcp *dcp)
{
	struct dcp_s2r_pmc_test_ctx *ctx = container_of(dcp, struct dcp_s2r_pmc_test_ctx, dcp);

	ctx->display_on_calls++;
	ctx->display_cpu_control = ctx->coproc_regs[APPLE_DCP_COPROC_CPU_CONTROL / 4];
	if (ctx->display_on_calls <= ctx->fail_display_until)
		return -ETIMEDOUT;
	return 0;
}

static const struct dcp_s2r_test_ops dcp_s2r_pmc_test_ops = {
	.map_power = dcp_s2r_pmc_test_map_power,
	.ps_sync = dcp_s2r_pmc_test_ps_sync,
	.pmc_check = dcp_s2r_pmc_test_pmc_check,
	.pmc_off = dcp_s2r_pmc_test_pmc_off,
	.pmc_on = dcp_s2r_pmc_test_pmc_on,
	.boot = dcp_s2r_pmc_test_boot,
	.display_on = dcp_s2r_pmc_test_display_on,
};

static void dcp_s2r_pmc_test_teardown(void *data)
{
	struct dcp_s2r_pmc_test_ctx *ctx = data;

	iomfb_queue_stop(&ctx->dcp);
	cancel_work_sync(&ctx->dcp.s2r_prewarm_work);
	cancel_delayed_work_sync(&ctx->dcp.s2r_prewarm_off_wq);
}

static struct dcp_s2r_pmc_test_ctx *
dcp_s2r_pmc_test_init(struct kunit *test, unsigned int fail_pmc_until,
		      unsigned int fail_boot_until, unsigned int fail_display_until)
{
	struct dcp_s2r_pmc_test_ctx *ctx;
	struct device *dev;

	ctx = kunit_kzalloc(test, sizeof(*ctx), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, ctx);
	dev = kunit_device_register(test, "dcp-s2r-pmc");
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, dev);

	ctx->dcp.dev = dev;
	ctx->dcp.fw_compat = DCP_FIRMWARE_V_27_0;
	ctx->dcp.hw.s2r = true;
	ctx->dcp.hw.s2r_layout = &dcp_s2r_pmc_test_layout;
	ctx->dcp.coproc_reg = (void __iomem __force *)ctx->coproc_regs;
	ctx->dcp.s2r_test_ops = &dcp_s2r_pmc_test_ops;
	ctx->fail_pmc_until = fail_pmc_until;
	ctx->fail_boot_until = fail_boot_until;
	ctx->fail_display_until = fail_display_until;

	iomfb_queue_init(&ctx->dcp);
	INIT_WORK(&ctx->dcp.s2r_prewarm_work, dcp_s2r_pmc_test_noop);
	INIT_DELAYED_WORK(&ctx->dcp.s2r_prewarm_off_wq, dcp_s2r_pmc_test_noop);
	mutex_init(&ctx->dcp.s2r_prewarm_lock);
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, dcp_s2r_pmc_test_teardown, ctx), 0);

	return ctx;
}

static void dcp_s2r_pmc_dpms_retry_after_failure(struct kunit *test)
{
	struct dcp_s2r_pmc_test_ctx *ctx = dcp_s2r_pmc_test_init(test, 1, 0, 0);
	struct apple_dcp *dcp = &ctx->dcp;

	dcp->dpms_powered_off = true;
	dcp->s2r_fw_down = true;
	dcp->s2r_pmc_off = true;
	dcp->iomfb.presentation_paused = true;

	/* First unblank: PMC restore fails, so DPMS-off and S2R flags stay armed. */
	dcp_dpms_power_on(dcp);
	KUNIT_EXPECT_EQ(test, ctx->pmc_on_calls, 1U);
	KUNIT_EXPECT_EQ(test, ctx->pmc_off_calls, 0U);
	KUNIT_EXPECT_EQ(test, ctx->boot_calls, 0U);
	KUNIT_EXPECT_EQ(test, ctx->display_on_calls, 0U);
	KUNIT_EXPECT_TRUE(test, dcp->dpms_powered_off);
	KUNIT_EXPECT_TRUE(test, dcp->s2r_fw_down);
	KUNIT_EXPECT_TRUE(test, dcp->s2r_pmc_off);
	KUNIT_EXPECT_FALSE(test, dcp->s2r_domains_up);
	KUNIT_EXPECT_TRUE(test, dcp->crashed);
	KUNIT_EXPECT_TRUE(test, dcp->iomfb.presentation_paused);
	KUNIT_EXPECT_EQ(test, ctx->ps_regs[dcp_s2r_pmc_test_layout.disp_fe / 4], 0U);
	KUNIT_EXPECT_EQ(test, ctx->ps_regs[dcp_s2r_pmc_test_layout.disp_cpu / 4], 0U);
	KUNIT_EXPECT_EQ(test, ctx->coproc_regs[APPLE_DCP_COPROC_CPU_CONTROL / 4], 0U);

	/* Second unblank: retries PMC restore, raises FE/CPU, boots, and clears state. */
	dcp_dpms_power_on(dcp);
	KUNIT_EXPECT_EQ(test, ctx->pmc_on_calls, 2U);
	KUNIT_EXPECT_EQ(test, ctx->pmc_off_calls, 0U);
	KUNIT_EXPECT_EQ(test, ctx->boot_calls, 1U);
	KUNIT_EXPECT_EQ(test, ctx->display_on_calls, 1U);
	KUNIT_EXPECT_FALSE(test, dcp->dpms_powered_off);
	KUNIT_EXPECT_FALSE(test, dcp->s2r_fw_down);
	KUNIT_EXPECT_FALSE(test, dcp->s2r_pmc_off);
	KUNIT_EXPECT_TRUE(test, dcp->s2r_domains_up);
	KUNIT_EXPECT_FALSE(test, dcp->crashed);
	KUNIT_EXPECT_FALSE(test, dcp->iomfb.presentation_paused);
	KUNIT_EXPECT_EQ(test, ctx->ps_regs[dcp_s2r_pmc_test_layout.disp_fe / 4], 0xffU);
	KUNIT_EXPECT_EQ(test, ctx->ps_regs[dcp_s2r_pmc_test_layout.disp_cpu / 4],
			(u32)(0xffU | BIT(28)));
	KUNIT_EXPECT_EQ(test, ctx->coproc_regs[APPLE_DCP_COPROC_CPU_CONTROL / 4],
			(u32)APPLE_DCP_COPROC_CPU_CONTROL_RUN);

	/* Third unblank: already powered on, does not re-run restore. */
	dcp_dpms_power_on(dcp);
	KUNIT_EXPECT_EQ(test, ctx->pmc_on_calls, 2U);
}

static void dcp_s2r_pmc_dpms_retry_after_boot_failure(struct kunit *test)
{
	struct dcp_s2r_pmc_test_ctx *ctx = dcp_s2r_pmc_test_init(test, 0, 1, 0);
	struct apple_dcp *dcp = &ctx->dcp;

	dcp->dpms_powered_off = true;
	dcp->s2r_fw_down = true;
	dcp->s2r_pmc_off = true;
	dcp->iomfb.presentation_paused = true;

	/* First unblank: boot handshake fails after CPU RUN; unwind restores stopped state. */
	dcp_dpms_power_on(dcp);
	KUNIT_EXPECT_EQ(test, ctx->pmc_on_calls, 1U);
	KUNIT_EXPECT_EQ(test, ctx->boot_calls, 1U);
	KUNIT_EXPECT_EQ(test, ctx->display_on_calls, 0U);
	KUNIT_EXPECT_EQ(test, ctx->pmc_off_calls, 1U);
	KUNIT_EXPECT_EQ(test, ctx->boot_cpu_control, (u32)APPLE_DCP_COPROC_CPU_CONTROL_RUN);
	KUNIT_EXPECT_EQ(test, ctx->coproc_regs[APPLE_DCP_COPROC_CPU_CONTROL / 4], 0U);
	KUNIT_EXPECT_TRUE(test, dcp->dpms_powered_off);
	KUNIT_EXPECT_TRUE(test, dcp->s2r_fw_down);
	KUNIT_EXPECT_TRUE(test, dcp->s2r_pmc_off);
	KUNIT_EXPECT_FALSE(test, dcp->s2r_domains_up);
	KUNIT_EXPECT_TRUE(test, dcp->crashed);
	KUNIT_EXPECT_TRUE(test, dcp->iomfb.presentation_paused);
	KUNIT_EXPECT_EQ(test, ctx->ps_regs[dcp_s2r_pmc_test_layout.disp_sys / 4],
			(u32)GENMASK(9, 8));
	KUNIT_EXPECT_EQ(test, ctx->ps_regs[dcp_s2r_pmc_test_layout.disp_fe / 4],
			(u32)GENMASK(9, 8));
	KUNIT_EXPECT_EQ(test, ctx->ps_regs[dcp_s2r_pmc_test_layout.disp_cpu / 4],
			(u32)GENMASK(9, 8));

	/* Second unblank: starts cleanly from stopped/off state and succeeds. */
	dcp_dpms_power_on(dcp);
	KUNIT_EXPECT_EQ(test, ctx->pmc_on_calls, 2U);
	KUNIT_EXPECT_EQ(test, ctx->boot_calls, 2U);
	KUNIT_EXPECT_EQ(test, ctx->display_on_calls, 1U);
	KUNIT_EXPECT_EQ(test, ctx->pmc_off_calls, 1U);
	KUNIT_EXPECT_EQ(test, ctx->boot_cpu_control, (u32)APPLE_DCP_COPROC_CPU_CONTROL_RUN);
	KUNIT_EXPECT_EQ(test, ctx->coproc_regs[APPLE_DCP_COPROC_CPU_CONTROL / 4],
			(u32)APPLE_DCP_COPROC_CPU_CONTROL_RUN);
	KUNIT_EXPECT_FALSE(test, dcp->dpms_powered_off);
	KUNIT_EXPECT_FALSE(test, dcp->s2r_fw_down);
	KUNIT_EXPECT_FALSE(test, dcp->s2r_pmc_off);
	KUNIT_EXPECT_TRUE(test, dcp->s2r_domains_up);
	KUNIT_EXPECT_FALSE(test, dcp->crashed);
	KUNIT_EXPECT_FALSE(test, dcp->iomfb.presentation_paused);
}

static void dcp_s2r_pmc_dpms_retry_after_display_on_failure(struct kunit *test)
{
	struct dcp_s2r_pmc_test_ctx *ctx = dcp_s2r_pmc_test_init(test, 0, 0, 1);
	struct apple_dcp *dcp = &ctx->dcp;

	dcp->dpms_powered_off = true;
	dcp->s2r_fw_down = true;
	dcp->s2r_pmc_off = true;
	dcp->iomfb.presentation_paused = true;

	/* First unblank: display-on fails after boot; unwind stops CPU and domains. */
	dcp_dpms_power_on(dcp);
	KUNIT_EXPECT_EQ(test, ctx->pmc_on_calls, 1U);
	KUNIT_EXPECT_EQ(test, ctx->boot_calls, 1U);
	KUNIT_EXPECT_EQ(test, ctx->display_on_calls, 1U);
	KUNIT_EXPECT_EQ(test, ctx->pmc_off_calls, 1U);
	KUNIT_EXPECT_EQ(test, ctx->display_cpu_control, (u32)APPLE_DCP_COPROC_CPU_CONTROL_RUN);
	KUNIT_EXPECT_EQ(test, ctx->coproc_regs[APPLE_DCP_COPROC_CPU_CONTROL / 4], 0U);
	KUNIT_EXPECT_TRUE(test, dcp->dpms_powered_off);
	KUNIT_EXPECT_TRUE(test, dcp->s2r_fw_down);
	KUNIT_EXPECT_TRUE(test, dcp->s2r_pmc_off);
	KUNIT_EXPECT_FALSE(test, dcp->s2r_domains_up);
	KUNIT_EXPECT_TRUE(test, dcp->crashed);
	KUNIT_EXPECT_TRUE(test, dcp->iomfb.presentation_paused);

	/* Second unblank: starts cleanly from stopped/off state and succeeds. */
	dcp_dpms_power_on(dcp);
	KUNIT_EXPECT_EQ(test, ctx->pmc_on_calls, 2U);
	KUNIT_EXPECT_EQ(test, ctx->boot_calls, 2U);
	KUNIT_EXPECT_EQ(test, ctx->display_on_calls, 2U);
	KUNIT_EXPECT_EQ(test, ctx->pmc_off_calls, 1U);
	KUNIT_EXPECT_EQ(test, ctx->coproc_regs[APPLE_DCP_COPROC_CPU_CONTROL / 4],
			(u32)APPLE_DCP_COPROC_CPU_CONTROL_RUN);
	KUNIT_EXPECT_FALSE(test, dcp->dpms_powered_off);
	KUNIT_EXPECT_FALSE(test, dcp->s2r_fw_down);
	KUNIT_EXPECT_FALSE(test, dcp->s2r_pmc_off);
	KUNIT_EXPECT_TRUE(test, dcp->s2r_domains_up);
	KUNIT_EXPECT_FALSE(test, dcp->crashed);
	KUNIT_EXPECT_FALSE(test, dcp->iomfb.presentation_paused);
}

static void dcp_s2r_pmc_dpms_fw_already_up(struct kunit *test)
{
	struct dcp_s2r_pmc_test_ctx *ctx = dcp_s2r_pmc_test_init(test, 0, 0, 0);
	struct apple_dcp *dcp = &ctx->dcp;

	dcp->dpms_powered_off = true;
	dcp->s2r_fw_down = false;
	dcp->s2r_pmc_off = true;

	dcp_dpms_power_on(dcp);
	KUNIT_EXPECT_EQ(test, ctx->pmc_on_calls, 0U);
	KUNIT_EXPECT_FALSE(test, dcp->dpms_powered_off);
	KUNIT_EXPECT_TRUE(test, dcp->s2r_pmc_off);
}

static struct kunit_case dcp_s2r_pmc_cases[] = {
	KUNIT_CASE(dcp_s2r_pmc_dpms_retry_after_failure),
	KUNIT_CASE(dcp_s2r_pmc_dpms_retry_after_boot_failure),
	KUNIT_CASE(dcp_s2r_pmc_dpms_retry_after_display_on_failure),
	KUNIT_CASE(dcp_s2r_pmc_dpms_fw_already_up),
	{}
};

static struct kunit_suite dcp_s2r_pmc_suite = {
	.name = "apple-dcp-s2r-pmc",
	.test_cases = dcp_s2r_pmc_cases,
};

kunit_test_suite(dcp_s2r_pmc_suite);
