// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * KUnit tests for the PMGR misc fabric controls: suspend/resume p-states
 * with and without PMP, the confirmed fabric raise, holds, the idle fabric
 * governor and its parameter, and the device tree lookup of its bandwidth
 * voters. The fabric and DCS controls are words in kernel memory and the
 * voters a regmap on a buffer; nothing touches MMIO.
 */

#include <kunit/device.h>
#include <kunit/static_stub.h>
#include <kunit/test.h>

#define PF_DEFAULT	0xa5000077	/* desired 7, actual 7, unknown upper bits */
#define PF_VOTE_IDLE	0x0f		/* target on, hardware gated: idle */
#define PF_VOTE_BUSY	0xff

struct pf_test {
	struct apple_pmgr_misc misc;	/* first: test->priv is both */
	u32 *fabric, *dcs;
	u32 *votes;
	bool cpus_min;
	bool published;
	unsigned int saved_idle_pstate;
	bool saved_idle_pstate_explicit;
};

static const struct regmap_config pf_vote_regmap_config = {
	.reg_bits = 32,
	.reg_stride = 4,
	.val_bits = 32,
	.max_register = 0x1c,
};

static bool pf_fake_cpus_at_min(void)
{
	struct pf_test *f = kunit_get_current_test()->priv;

	return f->cpus_min;
}

static int pf_test_init(struct kunit *test)
{
	struct apple_pmgr_misc *misc;
	struct regmap *map;
	struct pf_test *f;
	unsigned int i;

	/*
	 * The cases drive the driver's single instance and its parameter.
	 * Where the driver runs on real hardware they would take both over.
	 */
	if (smp_load_acquire(&apple_pmgr_misc_inst))
		kunit_skip(test, "apple-pmgr-misc is bound on this system");

	f = kunit_kzalloc(test, sizeof(*f), GFP_KERNEL);
	if (!f)
		return -ENOMEM;
	misc = &f->misc;
	f->saved_idle_pstate = READ_ONCE(idle_fabric_pstate);
	f->saved_idle_pstate_explicit = READ_ONCE(idle_fabric_pstate_explicit);
	WRITE_ONCE(idle_fabric_pstate, 0);
	WRITE_ONCE(idle_fabric_pstate_explicit, false);
	f->fabric = kunit_kzalloc(test, sizeof(u32), GFP_KERNEL);
	f->dcs = kunit_kzalloc(test, sizeof(u32), GFP_KERNEL);
	f->votes = kunit_kzalloc(test, 8 * sizeof(u32), GFP_KERNEL);
	if (!f->fabric || !f->dcs || !f->votes)
		return -ENOMEM;
	misc->dev = kunit_device_register(test, "pmgr-fabric-test");
	if (IS_ERR(misc->dev))
		return PTR_ERR(misc->dev);
	map = devm_regmap_init_mmio(misc->dev, (void __iomem *)f->votes,
				    &pf_vote_regmap_config);
	if (IS_ERR(map))
		return PTR_ERR(map);

	/* t602x layout: plain read-modify-write of both desired fields. */
	writel(PF_DEFAULT, f->fabric);
	writel(0xa5000099, f->dcs);
	misc->devices[DEV_FABRIC].base = (void __iomem *)f->fabric;
	misc->devices[DEV_FABRIC].active_state = 7;
	misc->devices[DEV_FABRIC].suspend_state = SYS_DEV_PSTATE_SUSPEND;
	misc->devices[DEV_DCS].base = (void __iomem *)f->dcs;
	misc->devices[DEV_DCS].active_state = 9;
	misc->devices[DEV_DCS].suspend_state = SYS_DEV_PSTATE_SUSPEND;
	mutex_init(&misc->lock);
	misc->fabric_now = 7;
	misc->dcs_now = 9;
	INIT_DEFERRABLE_WORK(&misc->idle_work, apple_pmgr_idle_work);
	INIT_DELAYED_WORK(&misc->idle_work_prompt, apple_pmgr_idle_work_prompt);
	for (i = 0; i < ARRAY_SIZE(misc->votes); i++) {
		f->votes[i] = PF_VOTE_IDLE;
		misc->votes[i].regmap = map;
		misc->votes[i].reg = i * sizeof(u32);
	}
	misc->voters_ok = true;
	dev_set_drvdata(misc->dev, misc);
	f->cpus_min = true;
	test->priv = f;
	kunit_activate_static_stub(test, apple_pmgr_cpus_at_min, pf_fake_cpus_at_min);
	return 0;
}

static void pf_test_exit(struct kunit *test)
{
	struct pf_test *f = test->priv;

	if (!f)
		return;
	cancel_delayed_work_sync(&f->misc.idle_work);
	cancel_delayed_work_sync(&f->misc.idle_work_prompt);
	WRITE_ONCE(idle_fabric_pstate, f->saved_idle_pstate);
	WRITE_ONCE(idle_fabric_pstate_explicit, f->saved_idle_pstate_explicit);
	if (f->published) {
		/* Pairs with the acquire in the driver's readers. */
		smp_store_release(&apple_pmgr_misc_inst, NULL);
	}
}

static void pf_publish(struct pf_test *f)
{
	f->published = true;
	/* Pairs with the acquire in the driver's readers, as probe's release. */
	smp_store_release(&apple_pmgr_misc_inst, &f->misc);
}

/* Fabric reported low by both fields, as after the governor lowered it. */
static void pf_set_low(struct pf_test *f, u32 reg)
{
	writel(reg, f->fabric);
	f->misc.fabric_now = 1;
}

static void pf_suspend_resume_test(struct kunit *test)
{
	struct pf_test *f = test->priv;
	struct apple_pmgr_misc *misc = &f->misc;

	KUNIT_ASSERT_EQ(test, apple_pmgr_misc_suspend_noirq(misc->dev), 0);
	/* Only the desired field changes; readback and unknown bits stay. */
	KUNIT_EXPECT_EQ(test, readl(f->fabric), 0xa5000071U);
	KUNIT_EXPECT_EQ(test, readl(f->dcs), 0xa5000091U);
	misc->fabric_now = 1;
	KUNIT_ASSERT_EQ(test, apple_pmgr_misc_resume_noirq(misc->dev), 0);
	KUNIT_EXPECT_EQ(test, readl(f->fabric), PF_DEFAULT);
	KUNIT_EXPECT_EQ(test, readl(f->dcs), 0xa5000099U);
	KUNIT_EXPECT_EQ(test, misc->fabric_now, 7U);
}

static void pf_suspend_absent_control_test(struct kunit *test)
{
	struct pf_test *f = test->priv;
	struct apple_pmgr_misc *misc = &f->misc;

	/* A control the device tree does not describe is skipped. */
	misc->devices[DEV_DCS].base = NULL;
	KUNIT_ASSERT_EQ(test, apple_pmgr_misc_suspend_noirq(misc->dev), 0);
	KUNIT_EXPECT_EQ(test, readl(f->fabric), 0xa5000071U);
	KUNIT_EXPECT_EQ(test, readl(f->dcs), 0xa5000099U);
	KUNIT_ASSERT_EQ(test, apple_pmgr_misc_resume_noirq(misc->dev), 0);
	KUNIT_EXPECT_EQ(test, readl(f->fabric), PF_DEFAULT);
}

static void pf_pmp_suspend_test(struct kunit *test)
{
	struct pf_test *f = test->priv;
	struct apple_pmgr_misc *misc = &f->misc;

	misc->pmp_owned = true;
	misc->pmp_sleep = true;
	writel(0xa5000035, f->fabric);	/* PMP chose fabric p-state 5 */
	KUNIT_ASSERT_EQ(test, apple_pmgr_misc_suspend_noirq(misc->dev), 0);
	KUNIT_EXPECT_EQ(test, misc->pmp_fabric, 5U);
	KUNIT_EXPECT_EQ(test, readl(f->fabric), 0xa5000031U);
	/* DCS is PMP's: not lowered. */
	KUNIT_EXPECT_EQ(test, readl(f->dcs), 0xa5000099U);

	KUNIT_ASSERT_EQ(test, apple_pmgr_misc_resume_noirq(misc->dev), 0);
	KUNIT_EXPECT_EQ(test, readl(f->fabric), 0xa5000035U);
	KUNIT_EXPECT_EQ(test, readl(f->dcs), 0xa5000099U);
	KUNIT_EXPECT_EQ(test, misc->fabric_now, 7U);
}

static void pf_pmp_resume_new_choice_test(struct kunit *test)
{
	struct pf_test *f = test->priv;
	struct apple_pmgr_misc *misc = &f->misc;

	misc->pmp_owned = true;
	misc->pmp_sleep = true;
	misc->pmp_fabric = 5;
	/* PMP moved the fabric since suspend: its choice is kept. */
	writel(0xa5000033, f->fabric);
	KUNIT_ASSERT_EQ(test, apple_pmgr_misc_resume_noirq(misc->dev), 0);
	KUNIT_EXPECT_EQ(test, readl(f->fabric), 0xa5000033U);

	/* Without a fabric control only the bookkeeping happens. */
	misc->devices[DEV_FABRIC].base = NULL;
	KUNIT_EXPECT_EQ(test, apple_pmgr_misc_suspend_noirq(misc->dev), 0);
	KUNIT_EXPECT_EQ(test, apple_pmgr_misc_resume_noirq(misc->dev), 0);
	KUNIT_EXPECT_EQ(test, readl(f->dcs), 0xa5000099U);
}

static void pf_raise_test(struct kunit *test)
{
	struct pf_test *f = test->priv;
	struct apple_pmgr_misc *misc = &f->misc;

	/* Hardware already reports 7: the confirming poll succeeds at once. */
	pf_set_low(f, 0xa5000071);
	scoped_guard(mutex, &misc->lock)
		KUNIT_EXPECT_EQ(test, apple_pmgr_raise_fabric(misc), 0);
	KUNIT_EXPECT_EQ(test, readl(f->fabric), PF_DEFAULT);
	KUNIT_EXPECT_EQ(test, misc->fabric_now, 7U);
}

static void pf_raise_timeout_test(struct kunit *test)
{
	struct pf_test *f = test->priv;
	struct apple_pmgr_misc *misc = &f->misc;

	/* Actual never follows: report it and leave the state unconfirmed. */
	pf_set_low(f, 0xa5000011);
	scoped_guard(mutex, &misc->lock)
		KUNIT_EXPECT_EQ(test, apple_pmgr_raise_fabric(misc), -ETIMEDOUT);
	KUNIT_EXPECT_EQ(test, readl(f->fabric), 0xa5000017U);
	KUNIT_EXPECT_EQ(test, misc->fabric_now, U32_MAX);

	/* The next caller tries again rather than trusting fabric_now. */
	writel(0xa5000077, f->fabric);
	scoped_guard(mutex, &misc->lock)
		KUNIT_EXPECT_EQ(test, apple_pmgr_raise_fabric(misc), 0);
	KUNIT_EXPECT_EQ(test, misc->fabric_now, 7U);
}

static void pf_raise_skip_test(struct kunit *test)
{
	struct pf_test *f = test->priv;
	struct apple_pmgr_misc *misc = &f->misc;

	/* Already at the boot p-state: no write, no poll. */
	writel(0xa5000013, f->fabric);
	scoped_guard(mutex, &misc->lock)
		KUNIT_EXPECT_EQ(test, apple_pmgr_raise_fabric(misc), 0);
	KUNIT_EXPECT_EQ(test, readl(f->fabric), 0xa5000013U);

	/* PMP owns the fabric: delegate, never write. */
	pf_set_low(f, 0xa5000011);
	misc->pmp_owned = true;
	scoped_guard(mutex, &misc->lock)
		KUNIT_EXPECT_EQ(test, apple_pmgr_raise_fabric(misc), 0);
	KUNIT_EXPECT_EQ(test, readl(f->fabric), 0xa5000011U);
	KUNIT_EXPECT_EQ(test, misc->fabric_now, 1U);
}

static void pf_hold_release_test(struct kunit *test)
{
	struct pf_test *f = test->priv;
	struct apple_pmgr_misc *misc = &f->misc;

	/* No instance yet: holds succeed and change nothing. */
	pf_set_low(f, 0xa5000071);
	KUNIT_EXPECT_EQ(test, apple_pmgr_fabric_hold(), 0);
	apple_pmgr_fabric_release();
	KUNIT_EXPECT_EQ(test, misc->holds, 0U);
	KUNIT_EXPECT_EQ(test, readl(f->fabric), 0xa5000071U);

	pf_publish(f);
	KUNIT_EXPECT_EQ(test, apple_pmgr_fabric_hold(), 0);
	KUNIT_EXPECT_EQ(test, misc->holds, 1U);
	KUNIT_EXPECT_EQ(test, readl(f->fabric), PF_DEFAULT);
	KUNIT_EXPECT_EQ(test, apple_pmgr_fabric_hold(), 0);
	KUNIT_EXPECT_EQ(test, misc->holds, 2U);
	apple_pmgr_fabric_release();
	apple_pmgr_fabric_release();
	KUNIT_EXPECT_EQ(test, misc->holds, 0U);

	/* An unconfirmed raise is reported, but the hold stands. */
	pf_set_low(f, 0xa5000011);
	KUNIT_EXPECT_EQ(test, apple_pmgr_fabric_hold(), -ETIMEDOUT);
	KUNIT_EXPECT_EQ(test, misc->holds, 1U);
	apple_pmgr_fabric_release();
	KUNIT_EXPECT_EQ(test, misc->holds, 0U);
}

static void pf_voters_active_test(struct kunit *test)
{
	struct pf_test *f = test->priv;
	struct apple_pmgr_misc *misc = &f->misc;
	unsigned int i;

	/* Target on but gated by hardware is idle: only actual counts. */
	KUNIT_EXPECT_FALSE(test, apple_pmgr_voters_active(misc));
	for (i = 0; i < ARRAY_SIZE(misc->votes); i++) {
		f->votes[i] = PF_VOTE_BUSY;
		KUNIT_EXPECT_TRUE_MSG(test, apple_pmgr_voters_active(misc), "voter %u", i);
		f->votes[i] = 0xf0;
		KUNIT_EXPECT_TRUE_MSG(test, apple_pmgr_voters_active(misc), "voter %u", i);
		f->votes[i] = 0xe0 | PF_VOTE_IDLE;
		KUNIT_EXPECT_FALSE_MSG(test, apple_pmgr_voters_active(misc), "voter %u", i);
		f->votes[i] = PF_VOTE_IDLE;
	}
	/* An unreadable voter counts as busy. */
	misc->votes[2].reg = 0x100;
	KUNIT_EXPECT_TRUE(test, apple_pmgr_voters_active(misc));
}

static void pf_idle_lowers_test(struct kunit *test)
{
	struct pf_test *f = test->priv;
	struct apple_pmgr_misc *misc = &f->misc;

	WRITE_ONCE(idle_fabric_pstate, 1);
	apple_pmgr_idle_eval(misc);
	KUNIT_EXPECT_EQ(test, readl(f->fabric), 0xa5000071U);
	KUNIT_EXPECT_EQ(test, misc->fabric_now, 1U);
	/* While low, the prompt timer watches for new load. */
	KUNIT_EXPECT_TRUE(test, delayed_work_pending(&misc->idle_work_prompt));
	KUNIT_EXPECT_FALSE(test, delayed_work_pending(&misc->idle_work));
	cancel_delayed_work_sync(&misc->idle_work_prompt);

	/* Already low: no second write. */
	writel(0xa5000011, f->fabric);
	apple_pmgr_idle_eval(misc);
	KUNIT_EXPECT_EQ(test, readl(f->fabric), 0xa5000011U);
}

enum pf_blocker {
	PF_HOLD, PF_DCS_BW, PF_VOTER, PF_CPU, PF_PMP, PF_NO_VOTERS, PF_IDLE_HIGH,
	PF_BLOCKERS
};

static void pf_idle_blockers_test(struct kunit *test)
{
	struct pf_test *f = test->priv;
	struct apple_pmgr_misc *misc = &f->misc;
	unsigned int b;

	for (b = 0; b < PF_BLOCKERS; b++) {
		WRITE_ONCE(idle_fabric_pstate, 1);
		misc->holds = b == PF_HOLD;
		misc->dcs_bw_active = b == PF_DCS_BW;
		f->votes[1] = b == PF_VOTER ? PF_VOTE_BUSY : PF_VOTE_IDLE;
		f->cpus_min = b != PF_CPU;
		misc->pmp_owned = b == PF_PMP;
		misc->voters_ok = b != PF_NO_VOTERS;
		if (b == PF_IDLE_HIGH)
			WRITE_ONCE(idle_fabric_pstate, 7);
		pf_set_low(f, 0xa5000071);

		apple_pmgr_idle_eval(misc);
		if (b == PF_PMP) {
			/* PMP decides: neither lowered nor raised by us. */
			KUNIT_EXPECT_EQ(test, readl(f->fabric), 0xa5000071U);
		} else {
			KUNIT_EXPECT_EQ_MSG(test, readl(f->fabric), PF_DEFAULT, "blocker %u", b);
			KUNIT_EXPECT_EQ_MSG(test, misc->fabric_now, 7U, "blocker %u", b);
		}
		KUNIT_EXPECT_TRUE_MSG(test, delayed_work_pending(&misc->idle_work),
				      "blocker %u", b);
		KUNIT_EXPECT_FALSE_MSG(test, delayed_work_pending(&misc->idle_work_prompt),
				       "blocker %u", b);
		cancel_delayed_work_sync(&misc->idle_work);
	}
}

static void pf_idle_off_sleeping_test(struct kunit *test)
{
	struct pf_test *f = test->priv;
	struct apple_pmgr_misc *misc = &f->misc;

	/* Governor off: raise to the boot state, no timer. */
	pf_set_low(f, 0xa5000071);
	apple_pmgr_idle_eval(misc);
	KUNIT_EXPECT_EQ(test, readl(f->fabric), PF_DEFAULT);
	KUNIT_EXPECT_FALSE(test, delayed_work_pending(&misc->idle_work));
	KUNIT_EXPECT_FALSE(test, delayed_work_pending(&misc->idle_work_prompt));

	/* Sleeping: the evaluation stops without touching the fabric. */
	WRITE_ONCE(idle_fabric_pstate, 1);
	misc->sleeping = true;
	apple_pmgr_idle_eval(misc);
	KUNIT_EXPECT_EQ(test, readl(f->fabric), PF_DEFAULT);
	pf_set_low(f, 0xa5000071);
	apple_pmgr_idle_eval(misc);
	KUNIT_EXPECT_EQ(test, readl(f->fabric), 0xa5000071U);
	KUNIT_EXPECT_FALSE(test, delayed_work_pending(&misc->idle_work));
	KUNIT_EXPECT_FALSE(test, delayed_work_pending(&misc->idle_work_prompt));
}

static void pf_param_test(struct kunit *test)
{
	struct pf_test *f = test->priv;
	struct apple_pmgr_misc *misc = &f->misc;

	KUNIT_EXPECT_EQ(test, idle_fabric_pstate_set("16", NULL), -EINVAL);
	KUNIT_EXPECT_EQ(test, idle_fabric_pstate_set("one", NULL), -EINVAL);
	KUNIT_EXPECT_EQ(test, idle_fabric_pstate, 0U);
	/* Before probe the value is only stored. */
	KUNIT_EXPECT_EQ(test, idle_fabric_pstate_set("15", NULL), 0);
	KUNIT_EXPECT_EQ(test, idle_fabric_pstate, 15U);
	KUNIT_EXPECT_EQ(test, idle_fabric_pstate_set("0", NULL), 0);

	pf_publish(f);
	/*
	 * Setting it runs the governor at once. The worker runs outside the
	 * test, without the CPU stub, so only that it ran is checked.
	 */
	KUNIT_EXPECT_EQ(test, idle_fabric_pstate_set("1", NULL), 0);
	KUNIT_EXPECT_EQ(test, idle_fabric_pstate, 1U);
	KUNIT_EXPECT_TRUE(test, flush_delayed_work(&misc->idle_work));
	cancel_delayed_work_sync(&misc->idle_work);
	cancel_delayed_work_sync(&misc->idle_work_prompt);

	/* Clearing it stops the governor and raises the fabric. */
	pf_set_low(f, 0xa5000071);
	KUNIT_EXPECT_EQ(test, idle_fabric_pstate_set("0", NULL), 0);
	KUNIT_EXPECT_EQ(test, readl(f->fabric), PF_DEFAULT);
	KUNIT_EXPECT_FALSE(test, delayed_work_pending(&misc->idle_work));
	KUNIT_EXPECT_FALSE(test, delayed_work_pending(&misc->idle_work_prompt));

	/* With PMP owning the fabric the governor cannot be enabled. */
	misc->pmp_owned = true;
	KUNIT_EXPECT_EQ(test, idle_fabric_pstate_set("1", NULL), -EBUSY);
	KUNIT_EXPECT_EQ(test, idle_fabric_pstate, 0U);
	KUNIT_EXPECT_EQ(test, idle_fabric_pstate_set("0", NULL), 0);
}

static void pf_prepare_complete_test(struct kunit *test)
{
	struct pf_test *f = test->priv;
	struct apple_pmgr_misc *misc = &f->misc;

	WRITE_ONCE(idle_fabric_pstate, 1);
	pf_set_low(f, 0xa5000071);
	KUNIT_ASSERT_EQ(test, apple_pmgr_misc_prepare(misc->dev), 0);
	/* Raised before any device suspends, governor parked. */
	KUNIT_EXPECT_TRUE(test, misc->sleeping);
	KUNIT_EXPECT_FALSE(test, misc->pmp_sleep);
	KUNIT_EXPECT_EQ(test, readl(f->fabric), PF_DEFAULT);
	KUNIT_EXPECT_FALSE(test, delayed_work_pending(&misc->idle_work));

	apple_pmgr_misc_complete(misc->dev);
	KUNIT_EXPECT_FALSE(test, misc->sleeping);
	/* The governor restarts at once. */
	KUNIT_EXPECT_TRUE(test, flush_delayed_work(&misc->idle_work));
	cancel_delayed_work_sync(&misc->idle_work);
	cancel_delayed_work_sync(&misc->idle_work_prompt);

	/* No fabric control: nothing to gate. */
	misc->devices[DEV_FABRIC].base = NULL;
	KUNIT_ASSERT_EQ(test, apple_pmgr_misc_prepare(misc->dev), 0);
	KUNIT_EXPECT_FALSE(test, misc->sleeping);
	apple_pmgr_misc_complete(misc->dev);
	KUNIT_EXPECT_FALSE(test, delayed_work_pending(&misc->idle_work));
}

static void pf_match_data_test(struct kunit *test)
{
	const struct apple_pmgr_misc_hw *t8103 = NULL, *t600x = NULL, *t602x = NULL;
	const struct of_device_id *id;

	for (id = apple_pmgr_misc_of_match; id->compatible[0]; id++) {
		if (!strcmp(id->compatible, "apple,t8103-pmgr-misc"))
			t8103 = id->data;
		else if (!strcmp(id->compatible, "apple,t6000-pmgr-misc"))
			t600x = id->data;
		else if (!strcmp(id->compatible, "apple,t6020-pmgr-misc"))
			t602x = id->data;
	}
	KUNIT_ASSERT_NOT_NULL(test, t8103);
	KUNIT_ASSERT_NOT_NULL(test, t600x);
	KUNIT_ASSERT_NOT_NULL(test, t602x);
	KUNIT_EXPECT_EQ(test, t8103->dev_min_ps[DEV_FABRIC], 0U);
	KUNIT_EXPECT_EQ(test, t8103->dev_min_ps[DEV_DCS], 0U);
	KUNIT_EXPECT_EQ(test, t8103->dcs_valid_states, (u32)DCS_VALID_STATES_T8103);
	KUNIT_EXPECT_TRUE(test, t8103->single_step);
	KUNIT_EXPECT_EQ(test, t600x->dev_min_ps[DEV_FABRIC], 1U);
	KUNIT_EXPECT_EQ(test, t600x->dev_min_ps[DEV_DCS], 7U);
	KUNIT_EXPECT_EQ(test, t600x->dcs_valid_states, (u32)DCS_VALID_STATES_T600X);
	KUNIT_EXPECT_EQ(test, t602x->dev_min_ps[DEV_FABRIC], 1U);
	KUNIT_EXPECT_EQ(test, t602x->dev_min_ps[DEV_DCS], 1U);
	/* No DCS state list: suspend uses the plain read-modify-write. */
	KUNIT_EXPECT_EQ(test, t602x->dcs_valid_states, 0U);
}

static u32 pf_step_log[16];
static unsigned int pf_step_cnt;
static bool pf_step_inject_busy;
static bool pf_step_inject_bad_desired;

static u32 pf_fake_perf_step(void __iomem *reg, u32 state)
{
	u32 val = readl_relaxed(reg);

	if (pf_step_cnt < ARRAY_SIZE(pf_step_log))
		pf_step_log[pf_step_cnt++] = state;
	FIELD_MODIFY(APPLE_CLKGEN_PSTATE_DESIRED, &val,
		     pf_step_inject_bad_desired ? (state ^ 1U) : state);
	if (pf_step_inject_busy) {
		val |= APPLE_CLKGEN_PSTATE_BUSY;
	} else {
		val &= ~APPLE_CLKGEN_PSTATE_BUSY;
		FIELD_MODIFY(APPLE_CLKGEN_PSTATE_ACTUAL, &val, state);
	}
	writel_relaxed(val, reg);
	return val;
}

static void pf_t8103_gpu_tracking_and_step_test(struct kunit *test)
{
	struct pf_test *f = test->priv;
	struct apple_pmgr_misc *misc = &f->misc;
	u32 last = 0;

	pf_step_cnt = 0;
	pf_step_inject_busy = false;
	pf_step_inject_bad_desired = false;
	kunit_activate_static_stub(test, apple_pmgr_perf_step, pf_fake_perf_step);

	misc->single_step = true;
	misc->dcs_valid_states = DCS_VALID_STATES_T8103;
	misc->devices[DEV_FABRIC].active_state = 4;
	misc->devices[DEV_DCS].active_state = 6;
	/* Without DT floors, both fabric and DCS retain boot states untouched. */
	misc->devices[DEV_FABRIC].suspend_state = 4;
	misc->devices[DEV_DCS].suspend_state = 6;
	misc->idle_dcs_pstate = 0;
	misc->fabric_now = 4;
	misc->dcs_now = 6;
	writel(0x00000444, f->fabric);
	writel(0x00000666, f->dcs);
	pf_publish(f);

	/* Parameter writes below the board fabric floor are rejected, and eval enforces it too. */
	KUNIT_EXPECT_EQ(test, idle_fabric_pstate_set("1", NULL), -EINVAL);
	WRITE_ONCE(idle_fabric_pstate, 1);
	pf_step_cnt = 0;
	apple_pmgr_idle_eval(misc);
	KUNIT_EXPECT_EQ(test, pf_step_cnt, 0U);
	KUNIT_EXPECT_EQ(test, readl(f->fabric), 0x00000444U);
	cancel_delayed_work_sync(&misc->idle_work);
	WRITE_ONCE(idle_fabric_pstate, 0);

	/* Live probe/resume verification rejects BUSY or ACTUAL!=DESIRED as unknown (U32_MAX). */
	writel(0x00000656, f->dcs);
	KUNIT_EXPECT_EQ(test, apple_pmgr_verify_active(misc, DEV_DCS), U32_MAX);
	misc->dcs_now = U32_MAX;
	KUNIT_EXPECT_EQ(test, apple_pmgr_gpu_busy_get(), -ETIMEDOUT);
	KUNIT_EXPECT_EQ(test, atomic_read(&apple_pmgr_gpu_busy), 0);
	writel(0x00000666, f->dcs);
	KUNIT_EXPECT_EQ(test, apple_pmgr_verify_active(misc, DEV_DCS), 6U);
	KUNIT_EXPECT_EQ(test, apple_pmgr_gpu_busy_get(), 0);
	KUNIT_EXPECT_EQ(test, misc->dcs_now, 6U);
	apple_pmgr_gpu_busy_put();
	writel(APPLE_CLKGEN_PSTATE_BUSY | 0x00000666, f->dcs);
	KUNIT_EXPECT_EQ(test, apple_pmgr_verify_active(misc, DEV_DCS), U32_MAX);
	writel(0x00000666, f->dcs);
	pf_step_cnt = 0;
	KUNIT_ASSERT_EQ(test, apple_pmgr_misc_suspend_noirq_target(misc, PM_SUSPEND_TO_IDLE), 0);
	KUNIT_EXPECT_EQ(test, pf_step_cnt, 0U);
	KUNIT_EXPECT_EQ(test, readl(f->fabric), 0x00000444U);
	KUNIT_EXPECT_EQ(test, readl(f->dcs), 0x00000666U);
	KUNIT_ASSERT_EQ(test, apple_pmgr_misc_resume_noirq_target(misc, PM_SUSPEND_TO_IDLE), 0);

	/* With a fabric floor of 1 and no DCS floor, only fabric scales. */
	misc->devices[DEV_FABRIC].suspend_state = 1;
	WRITE_ONCE(idle_fabric_pstate, 1);
	pf_step_cnt = 0;
	apple_pmgr_idle_eval(misc);
	KUNIT_EXPECT_EQ(test, misc->fabric_now, 1U);
	KUNIT_EXPECT_EQ(test, misc->dcs_now, 6U);
	KUNIT_EXPECT_EQ(test, readl(f->dcs), 0x00000666U);
	KUNIT_EXPECT_EQ(test, pf_step_cnt, 3U);
	cancel_delayed_work_sync(&misc->idle_work_prompt);

	/* Prepare propagates a failed raise and clears sleeping before aborting sleep. */
	pf_step_inject_busy = true;
	KUNIT_EXPECT_EQ(test, apple_pmgr_misc_prepare(misc->dev), -ETIMEDOUT);
	KUNIT_EXPECT_FALSE(test, misc->sleeping);
	cancel_delayed_work_sync(&misc->idle_work);
	pf_step_inject_busy = false;
	writel(0x00000411, f->fabric);
	misc->fabric_now = 1;

	/* Restore fabric and verify s2idle with no DCS floor leaves dcs-ps untouched. */
	scoped_guard(mutex, &misc->lock)
		KUNIT_EXPECT_EQ(test, apple_pmgr_raise_fabric(misc), 0);
	pf_step_cnt = 0;
	KUNIT_ASSERT_EQ(test, apple_pmgr_misc_suspend_noirq_target(misc, PM_SUSPEND_TO_IDLE), 0);
	KUNIT_EXPECT_EQ(test, readl(f->fabric), 0x00000411U);
	KUNIT_EXPECT_EQ(test, readl(f->dcs), 0x00000666U);
	KUNIT_EXPECT_EQ(test, pf_step_cnt, 3U);
	/* Even when s2idle resume skips MMIO on an unfloored rail, it verifies live state. */
	writel(0x00000656, f->dcs);
	KUNIT_ASSERT_EQ(test, apple_pmgr_misc_resume_noirq_target(misc, PM_SUSPEND_TO_IDLE), 0);
	KUNIT_EXPECT_EQ(test, readl(f->fabric), 0x00000444U);
	KUNIT_EXPECT_EQ(test, misc->fabric_now, 4U);
	KUNIT_EXPECT_EQ(test, misc->dcs_now, U32_MAX);
	writel(0x00000666, f->dcs);
	KUNIT_EXPECT_EQ(test, apple_pmgr_gpu_busy_get(), 0);
	KUNIT_EXPECT_EQ(test, misc->dcs_now, 6U);
	apple_pmgr_gpu_busy_put();

	/* Rollback on a failed DCS step also verifies an unfloored, unsettled fabric rail. */
	misc->devices[DEV_FABRIC].suspend_state = 4;
	misc->devices[DEV_DCS].suspend_state = 4;
	misc->dcs_valid_states = BIT(4) | BIT(5) | BIT(6);
	writel(0x00000434, f->fabric);
	writel(0x00000666, f->dcs);
	misc->fabric_now = 4;
	misc->dcs_now = 6;
	pf_step_inject_busy = true;
	KUNIT_EXPECT_EQ(test, apple_pmgr_misc_suspend_noirq_target(misc, PM_SUSPEND_TO_IDLE),
			-ETIMEDOUT);
	KUNIT_EXPECT_EQ(test, misc->fabric_now, U32_MAX);
	KUNIT_EXPECT_EQ(test, misc->dcs_now, U32_MAX);
	pf_step_inject_busy = false;
	writel(0x00000444, f->fabric);
	writel(0x00000666, f->dcs);
	misc->fabric_now = 4;
	misc->dcs_now = 6;
	misc->devices[DEV_FABRIC].suspend_state = 1;
	misc->devices[DEV_DCS].suspend_state = 6;
	misc->dcs_valid_states = DCS_VALID_STATES_T8103;

	/* Deep sleep leaves stepping to the sleep engine and detects post-wake mismatch. */
	pf_step_cnt = 0;
	KUNIT_ASSERT_EQ(test, apple_pmgr_misc_suspend_noirq_target(misc, PM_SUSPEND_MEM), 0);
	KUNIT_EXPECT_EQ(test, pf_step_cnt, 0U);
	writel(0x00000411, f->fabric);
	KUNIT_ASSERT_EQ(test, apple_pmgr_misc_resume_noirq_target(misc, PM_SUSPEND_MEM), 0);
	KUNIT_EXPECT_EQ(test, misc->fabric_now, U32_MAX);
	/* While PMGR owns the sleep transition, GPU busy acquisition defers (-EAGAIN). */
	misc->sleeping = true;
	KUNIT_EXPECT_EQ(test, apple_pmgr_gpu_busy_get(), -EAGAIN);
	KUNIT_EXPECT_EQ(test, atomic_read(&apple_pmgr_gpu_busy), 0);
	/* Once complete clears sleeping, GPU acquisition raises and confirms fabric. */
	misc->sleeping = false;
	KUNIT_EXPECT_EQ(test, apple_pmgr_gpu_busy_get(), 0);
	KUNIT_EXPECT_EQ(test, misc->fabric_now, 4U);
	KUNIT_EXPECT_EQ(test, readl(f->fabric), 0x00000444U);
	apple_pmgr_gpu_busy_put();

	/*
	 * With a board floor of 4 and GPU tracking active, an always-on gfx
	 * PMGR voter does not block idle while gpu_busy == 0, and single-step
	 * walks 4->3->2->1 on fabric and 6->5->4 on DCS.
	 */
	misc->devices[DEV_DCS].suspend_state = 4;
	misc->idle_dcs_pstate = 4;
	misc->dcs_valid_states = BIT(4) | BIT(5) | BIT(6);
	f->votes[0] = PF_VOTE_BUSY;
	KUNIT_EXPECT_TRUE(test, apple_pmgr_voters_active(misc));

	apple_pmgr_gpu_tracking_start();
	KUNIT_EXPECT_FALSE(test, apple_pmgr_voters_active(misc));

	pf_step_cnt = 0;
	apple_pmgr_idle_eval(misc);
	KUNIT_EXPECT_EQ(test, misc->fabric_now, 1U);
	KUNIT_EXPECT_EQ(test, misc->dcs_now, 4U);
	KUNIT_EXPECT_EQ(test, pf_step_cnt, 5U);
	KUNIT_EXPECT_EQ(test, pf_step_log[0], 3U);
	KUNIT_EXPECT_EQ(test, pf_step_log[2], 1U);
	KUNIT_EXPECT_EQ(test, pf_step_log[3], 5U);
	KUNIT_EXPECT_EQ(test, pf_step_log[4], 4U);
	cancel_delayed_work_sync(&misc->idle_work_prompt);

	/* Busy timeout on raise unwinds gpu_busy and returns error. */
	pf_step_inject_busy = true;
	KUNIT_EXPECT_EQ(test, apple_pmgr_gpu_busy_get(), -ETIMEDOUT);
	KUNIT_EXPECT_EQ(test, atomic_read(&apple_pmgr_gpu_busy), 0);
	/* Subsequent write while BUSY is still set fails -EBUSY without stepping. */
	KUNIT_EXPECT_EQ(test, apple_pmgr_step_to(f->fabric, 4, 0, &last), -EBUSY);
	/* Clear injected busy and verify mismatched DESIRED after step fails -ETIMEDOUT. */
	pf_step_inject_busy = false;
	writel(0x00000111, f->fabric);
	writel(0x00000444, f->dcs);
	misc->fabric_now = 1;
	misc->dcs_now = 4;
	pf_step_inject_bad_desired = true;
	KUNIT_EXPECT_EQ(test, apple_pmgr_step_to(f->fabric, 2, 0, &last), -ETIMEDOUT);
	pf_step_inject_bad_desired = false;
	writel(0x00000111, f->fabric);

	/* Normal raise succeeds and lifts both fabric and DCS synchronously. */
	KUNIT_EXPECT_EQ(test, apple_pmgr_gpu_busy_get(), 0);
	KUNIT_EXPECT_TRUE(test, apple_pmgr_voters_active(misc));
	KUNIT_EXPECT_EQ(test, misc->fabric_now, 4U);
	KUNIT_EXPECT_EQ(test, misc->dcs_now, 6U);

	/* Missing voter regmap counts as busy (no NULL deref on tracking stop). */
	misc->votes[2].regmap = NULL;
	apple_pmgr_gpu_busy_put();
	apple_pmgr_gpu_tracking_stop();
	KUNIT_EXPECT_TRUE(test, apple_pmgr_voters_active(misc));

	/* Path validation rejects unsettled ACTUAL!=DESIRED, off-range start, and bad steps. */
	pf_step_cnt = 0;
	writel(0x00000046, f->dcs);
	KUNIT_EXPECT_EQ(test, apple_pmgr_step_to(f->dcs, 4, BIT(4) | BIT(5) | BIT(6), &last),
			-EBUSY);
	KUNIT_EXPECT_EQ(test, pf_step_cnt, 0U);
	writel(0x00000777, f->dcs);
	KUNIT_EXPECT_EQ(test, apple_pmgr_step_to(f->dcs, 6, BIT(4) | BIT(5) | BIT(6), &last),
			-EINVAL);
	KUNIT_EXPECT_EQ(test, pf_step_cnt, 0U);
	writel(0x00000666, f->dcs);
	KUNIT_EXPECT_EQ(test, apple_pmgr_step_to(f->dcs, 4, BIT(4) | BIT(6), &last), -EINVAL);
	KUNIT_EXPECT_EQ(test, pf_step_cnt, 0U);
}

/*
 * Device tree voters. The pmgr node is created once and keeps a syscon
 * entry for the whole boot (syscon entries cannot be removed), so it is
 * never freed; each case adds and removes its own power-state children.
 */
static struct device_node *pf_dt_pmgr;

/*
 * The driver looks its voters and the PMP up in the whole tree. Nodes of
 * the system's own devicetree would join the fixture's, so the devicetree
 * cases only run where the fixture's are the only ones.
 */
static bool pf_dt_foreign(const char *compatible)
{
	struct device_node *np, *box = pf_dt_pmgr ? pf_dt_pmgr->parent : NULL;

	for_each_compatible_node(np, NULL, compatible) {
		struct device_node *p;
		bool ours = false;

		for (p = np->parent; p && box; p = p->parent)
			if (p == box) {
				ours = true;
				break;
			}
		if (!ours) {
			of_node_put(np);
			return true;
		}
	}
	return false;
}

static void pf_dt_require_fixture_only(struct kunit *test)
{
	if (pf_dt_foreign("apple,pmgr-pwrstate") ||
	    pf_dt_foreign("apple,t6000-pmp-v2") ||
	    pf_dt_foreign("apple,t8103-pmp"))
		kunit_skip(test, "the system devicetree describes PMGR or PMP nodes");
}
static u32 pf_dt_regs[0x50];

static const struct regmap_config pf_dt_regmap_config = {
	.reg_bits = 32,
	.reg_stride = 4,
	.val_bits = 32,
	.max_register = sizeof(pf_dt_regs) - sizeof(u32),
};

struct pf_dt_child {
	const char *name;
	const char *label;	/* NULL: no label property */
	u32 reg;		/* 0: no reg property */
	bool disabled;
	const char *compatible;	/* NULL: apple,pmgr-pwrstate */
};

static void pf_dt_apply(struct kunit *test, struct of_changeset *cs,
			const struct pf_dt_child *c, unsigned int n)
{
	unsigned int i;

	of_changeset_init(cs);
	for (i = 0; i < n; i++) {
		const char *compatible = c[i].compatible ?: "apple,pmgr-pwrstate";
		struct device_node *np = of_changeset_create_node(cs, pf_dt_pmgr, c[i].name);

		KUNIT_ASSERT_NOT_NULL(test, np);
		KUNIT_ASSERT_EQ(test, of_changeset_add_prop_string(cs, np, "compatible",
								   compatible), 0);
		if (c[i].label)
			KUNIT_ASSERT_EQ(test, of_changeset_add_prop_string(cs, np, "label",
									   c[i].label), 0);
		if (c[i].reg)
			KUNIT_ASSERT_EQ(test, of_changeset_add_prop_u32(cs, np, "reg",
									c[i].reg), 0);
		if (c[i].disabled)
			KUNIT_ASSERT_EQ(test, of_changeset_add_prop_string(cs, np, "status",
									   "disabled"), 0);
	}
	KUNIT_ASSERT_EQ(test, of_changeset_apply(cs), 0);
}

static void pf_dt_revert(struct of_changeset *cs)
{
	of_changeset_revert(cs);
	of_changeset_destroy(cs);
}

static bool pf_dt_find(struct kunit *test, struct apple_pmgr_misc *misc,
		       const struct pf_dt_child *c, unsigned int n)
{
	struct of_changeset cs;

	memset(misc->votes, 0, sizeof(misc->votes));
	misc->voters_ok = false;
	pf_dt_apply(test, &cs, c, n);
	apple_pmgr_find_voters(misc);
	pf_dt_revert(&cs);
	return misc->voters_ok;
}

static void pf_dt_voters_test(struct kunit *test)
{
	static const struct pf_dt_child c[] = {
		{ "power-controller@100", "gfx", 0x100 },
		{ "power-controller@108", "disp0_sys", 0x108 },
		{ "power-controller@110", "avd_sys", 0x110 },
		{ "power-controller@118", "isp_sys", 0x118 },
		{ "clock@120", "isp_sys", 0x120, false, "apple,t6000-clkgen" },
	};
	struct pf_test *f = test->priv;
	struct apple_pmgr_misc *misc = &f->misc;
	struct regmap *map = syscon_node_to_regmap(pf_dt_pmgr);

	pf_dt_require_fixture_only(test);

	KUNIT_ASSERT_FALSE(test, IS_ERR(map));
	KUNIT_EXPECT_TRUE(test, pf_dt_find(test, misc, c, ARRAY_SIZE(c)));
	/* Matched by label, in the order of apple_pmgr_fabric_voters. */
	KUNIT_EXPECT_EQ(test, misc->votes[0].reg, 0x100U);
	KUNIT_EXPECT_EQ(test, misc->votes[1].reg, 0x110U);
	KUNIT_EXPECT_EQ(test, misc->votes[2].reg, 0x118U);
	KUNIT_EXPECT_PTR_EQ(test, misc->votes[0].regmap, map);
	KUNIT_EXPECT_PTR_EQ(test, misc->votes[1].regmap, map);
	KUNIT_EXPECT_PTR_EQ(test, misc->votes[2].regmap, map);

	/* The voters read through the parent's register map. */
	pf_dt_regs[0x110 / 4] = PF_VOTE_IDLE;
	pf_dt_regs[0x118 / 4] = PF_VOTE_IDLE;
	pf_dt_regs[0x100 / 4] = PF_VOTE_BUSY;
	KUNIT_EXPECT_TRUE(test, apple_pmgr_voters_active(misc));
	pf_dt_regs[0x100 / 4] = PF_VOTE_IDLE;
	KUNIT_EXPECT_FALSE(test, apple_pmgr_voters_active(misc));
}

static void pf_dt_voters_missing_test(struct kunit *test)
{
	struct pf_dt_child c[] = {
		{ "power-controller@100", "gfx", 0x100 },
		{ "power-controller@110", "avd_sys", 0x110 },
		{ "power-controller@118", "isp_sys", 0x118 },
	};
	struct pf_test *f = test->priv;
	struct apple_pmgr_misc *misc = &f->misc;

	pf_dt_require_fixture_only(test);

	/* Desktop without ISP: the governor must never lower. */
	KUNIT_EXPECT_FALSE(test, pf_dt_find(test, misc, c, 2));
	KUNIT_EXPECT_NULL(test, misc->votes[2].regmap);

	c[2].disabled = true;
	KUNIT_EXPECT_FALSE(test, pf_dt_find(test, misc, c, 3));
	c[2].disabled = false;

	c[2].reg = 0;
	KUNIT_EXPECT_FALSE(test, pf_dt_find(test, misc, c, 3));
	c[2].reg = 0x118;

	c[2].label = NULL;
	KUNIT_EXPECT_FALSE(test, pf_dt_find(test, misc, c, 3));
	c[2].label = "isp";
	KUNIT_EXPECT_FALSE(test, pf_dt_find(test, misc, c, 3));
	c[2].label = "isp_sys";

	KUNIT_EXPECT_TRUE(test, pf_dt_find(test, misc, c, 3));

	/* With the voters missing the governor stays high. */
	misc->voters_ok = false;
	WRITE_ONCE(idle_fabric_pstate, 1);
	apple_pmgr_idle_eval(misc);
	KUNIT_EXPECT_EQ(test, readl(f->fabric), PF_DEFAULT);
}

static void pf_dt_voters_duplicate_test(struct kunit *test)
{
	static const struct pf_dt_child c[] = {
		{ "power-controller@100", "gfx", 0x100 },
		{ "power-controller@108", "gfx", 0x108 },
		{ "power-controller@110", "avd_sys", 0x110 },
	};
	struct pf_test *f = test->priv;
	struct apple_pmgr_misc *misc = &f->misc;

	pf_dt_require_fixture_only(test);

	/* A second gfx node must not stand in for the missing isp_sys. */
	KUNIT_EXPECT_FALSE(test, pf_dt_find(test, misc, c, ARRAY_SIZE(c)));
	KUNIT_EXPECT_NOT_NULL(test, misc->votes[0].regmap);
	KUNIT_EXPECT_NULL(test, misc->votes[2].regmap);
}

static void pf_dt_pmp_unbound_test(struct kunit *test)
{
	static const struct pf_dt_child c[] = {
		{ "pmp", NULL, 0, false, "apple,t6000-pmp-v2" },
	};
	struct pf_test *f = test->priv;
	struct apple_pmgr_misc *misc = &f->misc;
	struct of_changeset cs;

	pf_dt_require_fixture_only(test);

	/* PMP described but no driver bound: sleep takes the path without it. */
	misc->pmp_owned = true;
	pf_dt_apply(test, &cs, c, ARRAY_SIZE(c));
	KUNIT_EXPECT_EQ(test, apple_pmgr_misc_prepare(misc->dev), 0);
	pf_dt_revert(&cs);
	KUNIT_EXPECT_FALSE(test, misc->pmp_sleep);
	KUNIT_ASSERT_EQ(test, apple_pmgr_misc_suspend_noirq(misc->dev), 0);
	KUNIT_EXPECT_EQ(test, readl(f->dcs), 0xa5000091U);
	KUNIT_ASSERT_EQ(test, apple_pmgr_misc_resume_noirq(misc->dev), 0);
	apple_pmgr_misc_complete(misc->dev);
	KUNIT_EXPECT_EQ(test, readl(f->dcs), 0xa5000099U);
}

static int pf_dt_floor_run(struct kunit *test, struct apple_pmgr_misc *misc,
			   const char *prop, const u32 *vals, size_t nvals,
			   enum sys_device dev)
{
	struct of_changeset cs;
	struct device_node *np;
	struct device_node *saved = misc->dev->of_node;
	int ret;

	of_changeset_init(&cs);
	np = of_changeset_create_node(&cs, pf_dt_pmgr, "pmgr-misc-floor");
	KUNIT_ASSERT_NOT_NULL(test, np);
	if (vals)
		KUNIT_ASSERT_EQ(test,
				of_changeset_add_prop_u32_array(&cs, np, prop, vals, nvals),
				0);
	KUNIT_ASSERT_EQ(test, of_changeset_apply(&cs), 0);
	misc->dev->of_node = np;
	if (dev == DEV_FABRIC)
		ret = apple_pmgr_fabric_floor_init(misc);
	else if (dev == DEV_DCS)
		ret = apple_pmgr_dcs_floor_init(misc);
	else
		ret = apple_pmgr_disp_floor_init(misc);
	misc->dev->of_node = saved;
	pf_dt_revert(&cs);
	return ret;
}

static int pf_dt_fab_floor(struct kunit *test, struct apple_pmgr_misc *misc,
			   const u32 *vals, size_t nvals)
{
	return pf_dt_floor_run(test, misc, "apple,fabric-min-pstate", vals, nvals, DEV_FABRIC);
}

static int pf_dt_dcs_floor(struct kunit *test, struct apple_pmgr_misc *misc,
			   const u32 *vals, size_t nvals)
{
	return pf_dt_floor_run(test, misc, "apple,dcs-min-pstate", vals, nvals, DEV_DCS);
}

static int pf_dt_disp_floor(struct kunit *test, struct apple_pmgr_misc *misc,
			    const u32 *vals, size_t nvals)
{
	return pf_dt_floor_run(test, misc, "apple,disp-min-pstate", vals, nvals, DEV_DISP);
}

static void pf_dt_floors_test(struct kunit *test)
{
	struct pf_test *f = test->priv;
	struct apple_pmgr_misc *misc = &f->misc;
	static const u32 v0[] = { 0 };
	static const u32 v1[] = { 1 };
	static const u32 v2[] = { 2 };
	static const u32 v4[] = { 4 };
	static const u32 v5[] = { 5 };
	static const u32 v6[] = { 6 };
	static const u32 v7[] = { 7 };
	static const u32 v16[] = { 16 };
	static const u32 vpair[] = { 4, 5 };

	misc->single_step = true;
	misc->dcs_valid_states = DCS_VALID_STATES_T8103;
	misc->devices[DEV_FABRIC].active_state = 4;
	misc->devices[DEV_FABRIC].suspend_state = 4;
	misc->idle_fabric_default = 0;
	misc->devices[DEV_DCS].active_state = 6;
	misc->devices[DEV_DCS].suspend_state = 6;
	misc->idle_dcs_pstate = 0;

	/* Absent fabric/DCS properties retain boot states for idle and s2idle. */
	KUNIT_EXPECT_EQ(test, pf_dt_fab_floor(test, misc, NULL, 0), 0);
	KUNIT_EXPECT_EQ(test, misc->devices[DEV_FABRIC].suspend_state, 4U);
	KUNIT_EXPECT_EQ(test, misc->idle_fabric_default, 0U);
	KUNIT_EXPECT_EQ(test, pf_dt_dcs_floor(test, misc, NULL, 0), 0);
	KUNIT_EXPECT_EQ(test, misc->devices[DEV_DCS].suspend_state, 6U);
	KUNIT_EXPECT_EQ(test, misc->idle_dcs_pstate, 0U);

	/* Invalid fabric floor values fail; valid floor enables idle and s2idle fabric scaling. */
	KUNIT_EXPECT_EQ(test, pf_dt_fab_floor(test, misc, v0, 1), -EINVAL);
	KUNIT_EXPECT_EQ(test, pf_dt_fab_floor(test, misc, v5, 1), -EINVAL);
	KUNIT_EXPECT_EQ(test, pf_dt_fab_floor(test, misc, v16, 1), -EINVAL);
	KUNIT_EXPECT_EQ(test, pf_dt_fab_floor(test, misc, vpair, 2), -EINVAL);
	misc->pmp_owned = true;
	KUNIT_EXPECT_EQ(test, pf_dt_fab_floor(test, misc, v1, 1), 0);
	KUNIT_EXPECT_EQ(test, misc->devices[DEV_FABRIC].suspend_state, 4U);
	KUNIT_EXPECT_EQ(test, misc->idle_fabric_default, 0U);
	misc->pmp_owned = false;
	KUNIT_EXPECT_EQ(test, pf_dt_fab_floor(test, misc, v4, 1), 0);
	KUNIT_EXPECT_EQ(test, misc->devices[DEV_FABRIC].suspend_state, 4U);
	KUNIT_EXPECT_EQ(test, misc->idle_fabric_default, 0U);
	KUNIT_EXPECT_EQ(test, pf_dt_fab_floor(test, misc, v1, 1), 0);
	KUNIT_EXPECT_EQ(test, misc->devices[DEV_FABRIC].suspend_state, 1U);
	KUNIT_EXPECT_EQ(test, misc->idle_fabric_default, 1U);

	/* Out-of-range, invalid-mask, above-boot, or multi-cell DCS properties fail. */
	KUNIT_EXPECT_EQ(test, pf_dt_dcs_floor(test, misc, v0, 1), -EINVAL);
	KUNIT_EXPECT_EQ(test, pf_dt_dcs_floor(test, misc, v2, 1), -EINVAL);
	KUNIT_EXPECT_EQ(test, pf_dt_dcs_floor(test, misc, v7, 1), -EINVAL);
	KUNIT_EXPECT_EQ(test, pf_dt_dcs_floor(test, misc, v16, 1), -EINVAL);
	KUNIT_EXPECT_EQ(test, pf_dt_dcs_floor(test, misc, vpair, 2), -EINVAL);
	KUNIT_EXPECT_EQ(test, misc->idle_dcs_pstate, 0U);

	/* Non-contiguous intermediate states and OPP-table conflicts fail. */
	misc->dcs_valid_states = BIT(4) | BIT(6);
	KUNIT_EXPECT_EQ(test, pf_dt_dcs_floor(test, misc, v4, 1), -EINVAL);
	misc->dcs_valid_states = DCS_VALID_STATES_T8103;
	misc->dcs_data = (struct icc_onecell_data *)1;
	KUNIT_EXPECT_EQ(test, pf_dt_dcs_floor(test, misc, v4, 1), -EINVAL);
	misc->dcs_data = NULL;

	/* Floor equal to boot state is valid and keeps idle DCS scaling off. */
	KUNIT_EXPECT_EQ(test, pf_dt_dcs_floor(test, misc, v6, 1), 0);
	KUNIT_EXPECT_EQ(test, misc->devices[DEV_DCS].suspend_state, 6U);
	KUNIT_EXPECT_EQ(test, misc->idle_dcs_pstate, 0U);

	/* PMP-owned boards ignore the property without enabling host DCS scaling. */
	misc->dcs_valid_states = DCS_VALID_STATES_T8103;
	misc->pmp_owned = true;
	KUNIT_EXPECT_EQ(test, pf_dt_dcs_floor(test, misc, v4, 1), 0);
	KUNIT_EXPECT_EQ(test, misc->devices[DEV_DCS].suspend_state, 6U);
	KUNIT_EXPECT_EQ(test, misc->idle_dcs_pstate, 0U);
	misc->pmp_owned = false;

	/* Valid floor enables idle and s2idle DCS scaling and masks lower states. */
	misc->dcs_valid_states = DCS_VALID_STATES_T8103;
	KUNIT_EXPECT_EQ(test, pf_dt_dcs_floor(test, misc, v4, 1), 0);
	KUNIT_EXPECT_EQ(test, misc->devices[DEV_DCS].suspend_state, 4U);
	KUNIT_EXPECT_EQ(test, misc->idle_dcs_pstate, 4U);
	KUNIT_EXPECT_FALSE(test, apple_pmgr_dcs_state_valid(misc, 3));
	KUNIT_EXPECT_TRUE(test, apple_pmgr_dcs_state_valid(misc, 4));
	KUNIT_EXPECT_TRUE(test, apple_pmgr_dcs_state_valid(misc, 6));
}

static void pf_t8103_disp_scaling_test(struct kunit *test)
{
	struct pf_test *f = test->priv;
	struct apple_pmgr_misc *misc = &f->misc;
	u32 *disp = kunit_kzalloc(test, sizeof(u32), GFP_KERNEL);
	static const u32 v0[] = { 0 };
	static const u32 v2[] = { 2 };
	static const u32 v3[] = { 3 };
	static const u32 v4[] = { 4 };
	static const u32 v16[] = { 16 };
	static const u32 vpair[] = { 2, 3 };

	KUNIT_ASSERT_NOT_NULL(test, disp);
	pf_step_cnt = 0;
	pf_step_inject_busy = false;
	pf_step_inject_bad_desired = false;
	kunit_activate_static_stub(test, apple_pmgr_perf_step, pf_fake_perf_step);

	misc->single_step = true;
	misc->devices[DEV_FABRIC].active_state = 4;
	misc->devices[DEV_FABRIC].suspend_state = 4;
	misc->devices[DEV_DCS].active_state = 6;
	misc->devices[DEV_DCS].suspend_state = 6;
	misc->fabric_now = 4;
	misc->dcs_now = 6;
	writel(0x00000444, f->fabric);
	writel(0x00000666, f->dcs);

	/* Property without disp-ps reg fails cleanly. */
	KUNIT_EXPECT_EQ(test, pf_dt_disp_floor(test, misc, v2, 1), -EINVAL);

	writel(0x00000333, disp);
	misc->devices[DEV_DISP].base = (void __iomem *)disp;
	misc->devices[DEV_DISP].active_state = 3;
	misc->devices[DEV_DISP].suspend_state = 3;
	misc->disp_now = 3;

	/* Validate absent, out-of-range, above-boot, equal-to-boot, and valid floors. */
	KUNIT_EXPECT_EQ(test, pf_dt_disp_floor(test, misc, NULL, 0), 0);
	KUNIT_EXPECT_EQ(test, misc->idle_disp_pstate, 0U);
	KUNIT_EXPECT_EQ(test, pf_dt_disp_floor(test, misc, v0, 1), -EINVAL);
	KUNIT_EXPECT_EQ(test, pf_dt_disp_floor(test, misc, v4, 1), -EINVAL);
	KUNIT_EXPECT_EQ(test, pf_dt_disp_floor(test, misc, v16, 1), -EINVAL);
	KUNIT_EXPECT_EQ(test, pf_dt_disp_floor(test, misc, vpair, 2), -EINVAL);
	KUNIT_EXPECT_EQ(test, pf_dt_disp_floor(test, misc, v3, 1), 0);
	KUNIT_EXPECT_EQ(test, misc->idle_disp_pstate, 0U);
	KUNIT_EXPECT_EQ(test, pf_dt_disp_floor(test, misc, v2, 1), 0);
	KUNIT_EXPECT_EQ(test, misc->devices[DEV_DISP].suspend_state, 2U);
	KUNIT_EXPECT_EQ(test, misc->idle_disp_pstate, 2U);

	/* Idle lowers disp 3 -> 2; hold and GPU busy raise it back to 3. */
	pf_publish(f);
	f->cpus_min = true;
	apple_pmgr_idle_eval(misc);
	KUNIT_EXPECT_EQ(test, misc->disp_now, 2U);
	KUNIT_EXPECT_EQ(test, readl(disp), 0x00000322U);
	cancel_delayed_work_sync(&misc->idle_work_prompt);

	KUNIT_EXPECT_EQ(test, apple_pmgr_fabric_hold(), 0);
	KUNIT_EXPECT_EQ(test, misc->disp_now, 3U);
	KUNIT_EXPECT_EQ(test, readl(disp), 0x00000333U);
	apple_pmgr_fabric_release();

	apple_pmgr_idle_eval(misc);
	KUNIT_EXPECT_EQ(test, misc->disp_now, 2U);
	cancel_delayed_work_sync(&misc->idle_work_prompt);

	KUNIT_EXPECT_EQ(test, apple_pmgr_gpu_busy_get(), 0);
	KUNIT_EXPECT_EQ(test, misc->disp_now, 3U);
	apple_pmgr_gpu_busy_put();

	apple_pmgr_idle_eval(misc);
	KUNIT_EXPECT_EQ(test, misc->disp_now, 2U);
	cancel_delayed_work_sync(&misc->idle_work_prompt);

	/* Prepare raises disp back to boot state; s2idle steps 3 -> 2 -> 3. */
	KUNIT_EXPECT_EQ(test, apple_pmgr_misc_prepare(misc->dev), 0);
	KUNIT_EXPECT_EQ(test, misc->disp_now, 3U);
	KUNIT_EXPECT_EQ(test, apple_pmgr_misc_suspend_noirq_target(misc, PM_SUSPEND_TO_IDLE), 0);
	KUNIT_EXPECT_EQ(test, readl(disp), 0x00000322U);
	KUNIT_EXPECT_EQ(test, apple_pmgr_misc_resume_noirq_target(misc, PM_SUSPEND_TO_IDLE), 0);
	KUNIT_EXPECT_EQ(test, misc->disp_now, 3U);
	KUNIT_EXPECT_EQ(test, readl(disp), 0x00000333U);
	apple_pmgr_misc_complete(misc->dev);
	cancel_delayed_work_sync(&misc->idle_work);
}

static int pf_suite_init(struct kunit_suite *suite)
{
	struct of_changeset cs;
	struct device_node *box;
	struct regmap *map;
	int ret;

	if (!of_root)
		return -ENODEV;
	map = regmap_init_mmio(NULL, (void __iomem *)pf_dt_regs, &pf_dt_regmap_config);
	if (IS_ERR(map))
		return PTR_ERR(map);

	/*
	 * Disabled container: nothing below it is populated as a device. The
	 * changeset is never reverted, see above.
	 */
	of_changeset_init(&cs);
	box = of_changeset_create_node(&cs, of_root, "kunit-apple-pmgr-fabric");
	if (!box || of_changeset_add_prop_string(&cs, box, "status", "disabled"))
		goto nomem;
	pf_dt_pmgr = of_changeset_create_node(&cs, box, "power-management");
	if (!pf_dt_pmgr)
		goto nomem;
	ret = of_changeset_apply(&cs);
	if (ret)
		goto destroy;
	return of_syscon_register_regmap(pf_dt_pmgr, map);

nomem:
	ret = -ENOMEM;
destroy:
	of_changeset_destroy(&cs);
	regmap_exit(map);
	pf_dt_pmgr = NULL;
	return ret;
}

static struct kunit_case apple_pmgr_fabric_test_cases[] = {
	KUNIT_CASE(pf_suspend_resume_test),
	KUNIT_CASE(pf_suspend_absent_control_test),
	KUNIT_CASE(pf_pmp_suspend_test),
	KUNIT_CASE(pf_pmp_resume_new_choice_test),
	KUNIT_CASE(pf_raise_test),
	KUNIT_CASE(pf_raise_timeout_test),
	KUNIT_CASE(pf_raise_skip_test),
	KUNIT_CASE(pf_hold_release_test),
	KUNIT_CASE(pf_voters_active_test),
	KUNIT_CASE(pf_idle_lowers_test),
	KUNIT_CASE(pf_idle_blockers_test),
	KUNIT_CASE(pf_idle_off_sleeping_test),
	KUNIT_CASE(pf_param_test),
	KUNIT_CASE(pf_prepare_complete_test),
	KUNIT_CASE(pf_match_data_test),
	KUNIT_CASE(pf_t8103_gpu_tracking_and_step_test),
	KUNIT_CASE(pf_t8103_disp_scaling_test),
	KUNIT_CASE(pf_dt_floors_test),
	KUNIT_CASE(pf_dt_voters_test),
	KUNIT_CASE(pf_dt_voters_missing_test),
	KUNIT_CASE(pf_dt_voters_duplicate_test),
	KUNIT_CASE(pf_dt_pmp_unbound_test),
	{}
};

static struct kunit_suite apple_pmgr_fabric_test_suite = {
	.name = "apple-pmgr-fabric",
	.suite_init = pf_suite_init,
	.init = pf_test_init,
	.exit = pf_test_exit,
	.test_cases = apple_pmgr_fabric_test_cases,
};

kunit_test_suite(apple_pmgr_fabric_test_suite);
