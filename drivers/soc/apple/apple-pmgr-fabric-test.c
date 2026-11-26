// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * KUnit tests for the PMGR misc fabric controls: suspend/resume p-states
 * with and without PMP, the confirmed fabric raise, holds, the idle and
 * tiered active fabric/DCS governors, GPU duty-cycle tracking, and the
 * device tree lookup of bandwidth voters. The fabric and DCS controls are
 * words in kernel memory and the voters a regmap on a buffer; nothing
 * touches MMIO.
 */

#include <kunit/device.h>
#include <kunit/static_stub.h>
#include <kunit/test.h>

#define PF_DEFAULT	0xa5000077	/* desired 7, actual 7, unknown upper bits */
#define PF_VOTE_IDLE	0x0f		/* target on, hardware gated: idle */
#define PF_VOTE_BUSY	0xff
#define PF_MS(n)	((u64)(n) * NSEC_PER_MSEC)

struct pf_test {
	struct apple_pmgr_misc misc;	/* first: test->priv is both */
	u32 *fabric, *dcs;
	u32 *votes;
	bool cpus_min;
	bool published;
	struct apple_pmgr_gov_sample sample;
	bool sample_primed;
	unsigned int saved_idle_pstate;
	unsigned int saved_idle_dcs_pstate;
	bool saved_idle_pstate_explicit;
	bool saved_idle_dcs_pstate_explicit;
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

static bool pf_fake_sample_load(struct apple_pmgr_misc *misc, u64 now,
				struct apple_pmgr_gov_sample *sample)
{
	struct pf_test *f = kunit_get_current_test()->priv;

	*sample = f->sample;
	return f->sample_primed;
}

static void pf_gpu_reset(void)
{
	unsigned long flags;

	atomic_set(&apple_pmgr_gpu_tracked, 0);
	spin_lock_irqsave(&apple_pmgr_gpu.lock, flags);
	apple_pmgr_gpu.busy_count = 0;
	apple_pmgr_gpu.busy_since_ns = 0;
	apple_pmgr_gpu.busy_ns = 0;
	apple_pmgr_gpu.wake = NULL;
	spin_unlock_irqrestore(&apple_pmgr_gpu.lock, flags);
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
	f->saved_idle_dcs_pstate = READ_ONCE(idle_dcs_pstate);
	f->saved_idle_pstate_explicit = READ_ONCE(idle_fabric_pstate_explicit);
	f->saved_idle_dcs_pstate_explicit = READ_ONCE(idle_dcs_pstate_explicit);
	WRITE_ONCE(idle_fabric_pstate, 0);
	WRITE_ONCE(idle_dcs_pstate, 0);
	WRITE_ONCE(idle_fabric_pstate_explicit, false);
	WRITE_ONCE(idle_dcs_pstate_explicit, false);
	pf_gpu_reset();

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
	misc->gov_cfg = apple_pmgr_gov_cfg_default;
	misc->gov.tier = APPLE_PMGR_TIER_FULL;
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
	f->sample_primed = true;
	test->priv = f;
	kunit_activate_static_stub(test, apple_pmgr_cpus_at_min, pf_fake_cpus_at_min);
	return 0;
}

static void pf_test_exit(struct kunit *test)
{
	struct pf_test *f = test->priv;

	if (!f)
		return;
	apple_pmgr_gpu_wake_set(NULL);
	cancel_delayed_work_sync(&f->misc.idle_work);
	cancel_delayed_work_sync(&f->misc.idle_work_prompt);
	pf_gpu_reset();
	WRITE_ONCE(idle_fabric_pstate, f->saved_idle_pstate);
	WRITE_ONCE(idle_dcs_pstate, f->saved_idle_dcs_pstate);
	WRITE_ONCE(idle_fabric_pstate_explicit, f->saved_idle_pstate_explicit);
	WRITE_ONCE(idle_dcs_pstate_explicit, f->saved_idle_dcs_pstate_explicit);
	if (f->published) {
		/* Pairs with the acquire in the driver's readers. */
		smp_store_release(&apple_pmgr_misc_inst, NULL);
	}
}

static void pf_publish(struct pf_test *f)
{
	f->published = true;
	apple_pmgr_gpu_wake_set(&f->misc);
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
	/* Absent optional voter (venc_sys) is skipped cleanly. */
	misc->votes[3].regmap = NULL;
	KUNIT_EXPECT_FALSE(test, apple_pmgr_voters_active(misc));

	/* An unreadable required voter counts as busy. */
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
	/* At the lowest idle state, deferrable work gives 0 Hz idle wakeups. */
	KUNIT_EXPECT_TRUE(test, delayed_work_pending(&misc->idle_work));
	KUNIT_EXPECT_FALSE(test, delayed_work_pending(&misc->idle_work_prompt));
	cancel_delayed_work_sync(&misc->idle_work);

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
		bool static_blocker = b == PF_PMP || b == PF_NO_VOTERS || b == PF_IDLE_HIGH;

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
		if (static_blocker) {
			KUNIT_EXPECT_TRUE_MSG(test, delayed_work_pending(&misc->idle_work),
					      "blocker %u", b);
			KUNIT_EXPECT_FALSE_MSG(test, delayed_work_pending(&misc->idle_work_prompt),
					       "blocker %u", b);
			cancel_delayed_work_sync(&misc->idle_work);
		} else {
			KUNIT_EXPECT_TRUE_MSG(test, delayed_work_pending(&misc->idle_work_prompt),
					      "blocker %u", b);
			KUNIT_EXPECT_FALSE_MSG(test, delayed_work_pending(&misc->idle_work),
					       "blocker %u", b);
			cancel_delayed_work_sync(&misc->idle_work_prompt);
		}
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
	KUNIT_EXPECT_EQ(test, t8103->dev_min_ps[DEV_FABRIC], 1U);
	KUNIT_EXPECT_EQ(test, t8103->dev_min_ps[DEV_DCS], 4U);
	KUNIT_EXPECT_EQ(test, t8103->dcs_valid_states, (u32)DCS_VALID_STATES_T8103);
	KUNIT_EXPECT_TRUE(test, t8103->single_step);
	KUNIT_EXPECT_TRUE(test, t8103->tiered_scaling);
	KUNIT_EXPECT_EQ(test, t8103->default_idle_fabric, 1U);
	KUNIT_EXPECT_EQ(test, t8103->default_idle_dcs, 4U);
	KUNIT_EXPECT_EQ(test, t8103->light_fabric, 2U);
	KUNIT_EXPECT_EQ(test, t8103->light_dcs, 4U);
	KUNIT_EXPECT_EQ(test, t600x->dev_min_ps[DEV_FABRIC], 1U);
	KUNIT_EXPECT_EQ(test, t600x->dev_min_ps[DEV_DCS], 7U);
	KUNIT_EXPECT_EQ(test, t600x->dcs_valid_states, (u32)DCS_VALID_STATES_T600X);
	KUNIT_EXPECT_FALSE(test, t600x->tiered_scaling);
	KUNIT_EXPECT_EQ(test, t602x->dev_min_ps[DEV_FABRIC], 1U);
	KUNIT_EXPECT_EQ(test, t602x->dev_min_ps[DEV_DCS], 1U);
	/* No DCS state list: suspend uses the plain read-modify-write. */
	KUNIT_EXPECT_EQ(test, t602x->dcs_valid_states, 0U);
	KUNIT_EXPECT_FALSE(test, t602x->tiered_scaling);
}

static u32 pf_step_log[16];
static unsigned int pf_step_cnt;
static bool pf_step_inject_busy;

static u32 pf_fake_perf_step(void __iomem *reg, u32 state)
{
	u32 val = readl_relaxed(reg);

	if (pf_step_cnt < ARRAY_SIZE(pf_step_log))
		pf_step_log[pf_step_cnt++] = state;
	FIELD_MODIFY(APPLE_CLKGEN_PSTATE_DESIRED, &val, state);
	if (pf_step_inject_busy) {
		val |= APPLE_CLKGEN_PSTATE_BUSY;
	} else {
		val &= ~APPLE_CLKGEN_PSTATE_BUSY;
		FIELD_MODIFY(APPLE_CLKGEN_PSTATE_ACTUAL, &val, state);
	}
	writel_relaxed(val, reg);
	return val;
}

static void pf_setup_t8103(struct kunit *test, struct pf_test *f, bool tiered)
{
	struct apple_pmgr_misc *misc = &f->misc;

	pf_step_cnt = 0;
	pf_step_inject_busy = false;
	kunit_activate_static_stub(test, apple_pmgr_perf_step, pf_fake_perf_step);

	misc->single_step = true;
	misc->tiered_scaling = tiered;
	misc->dcs_valid_states = DCS_VALID_STATES_T8103;
	misc->light_fabric = 2;
	misc->light_dcs = 4;
	misc->devices[DEV_FABRIC].suspend_state = 1;
	misc->devices[DEV_DCS].suspend_state = 4;
	misc->devices[DEV_FABRIC].active_state = 4;
	misc->devices[DEV_DCS].active_state = 6;
	misc->fabric_now = 4;
	misc->dcs_now = 6;
	misc->gov.tier = APPLE_PMGR_TIER_FULL;
	writel(0x00000444, f->fabric);
	writel(0x00000666, f->dcs);
}

static void pf_t8103_gpu_tracking_and_dcs_valid_test(struct kunit *test)
{
	struct pf_test *f = test->priv;
	struct apple_pmgr_misc *misc = &f->misc;
	u32 last = 0;

	pf_setup_t8103(test, f, false);
	pf_publish(f);

	/* T8103 DCS rejects unsafe states (1, 2, 3) and accepts 4..6. */
	KUNIT_EXPECT_EQ(test, idle_dcs_pstate_set("1", NULL), -EINVAL);
	KUNIT_EXPECT_EQ(test, idle_dcs_pstate_set("2", NULL), -EINVAL);
	KUNIT_EXPECT_EQ(test, idle_dcs_pstate_set("3", NULL), -EINVAL);
	KUNIT_EXPECT_EQ(test, idle_dcs_pstate_set("4", NULL), 0);
	KUNIT_EXPECT_EQ(test, idle_dcs_pstate, 4U);
	cancel_delayed_work_sync(&misc->idle_work);
	cancel_delayed_work_sync(&misc->idle_work_prompt);

	/*
	 * With GPU tracking active, an always-on gfx PMGR voter does not
	 * block idle while gpu_busy == 0, and single-stepping demotes DCS
	 * first (6->5->4) and then fabric (4->3->2->1).
	 */
	f->votes[0] = PF_VOTE_BUSY;
	KUNIT_EXPECT_TRUE(test, apple_pmgr_voters_active(misc));

	apple_pmgr_gpu_tracking_start();
	KUNIT_EXPECT_FALSE(test, apple_pmgr_voters_active(misc));

	WRITE_ONCE(idle_fabric_pstate, 1);
	WRITE_ONCE(idle_dcs_pstate, 4);
	pf_step_cnt = 0;
	apple_pmgr_idle_eval(misc);
	KUNIT_EXPECT_EQ(test, misc->fabric_now, 1U);
	KUNIT_EXPECT_EQ(test, misc->dcs_now, 4U);
	KUNIT_EXPECT_EQ(test, pf_step_cnt, 5U);
	KUNIT_EXPECT_EQ(test, pf_step_log[0], 5U);
	KUNIT_EXPECT_EQ(test, pf_step_log[1], 4U);
	KUNIT_EXPECT_EQ(test, pf_step_log[2], 3U);
	KUNIT_EXPECT_EQ(test, pf_step_log[3], 2U);
	KUNIT_EXPECT_EQ(test, pf_step_log[4], 1U);
	KUNIT_EXPECT_TRUE(test, delayed_work_pending(&misc->idle_work));
	KUNIT_EXPECT_FALSE(test, delayed_work_pending(&misc->idle_work_prompt));

	/*
	 * GPU busy 0 -> 1 edge never takes misc->lock or slams (4, 6)
	 * synchronously; it cancels deferrable idle_work and kicks
	 * idle_work_prompt while recording duty cycle.
	 */
	scoped_guard(mutex, &misc->lock) {
		KUNIT_EXPECT_EQ(test, apple_pmgr_gpu_busy_get(), 0);
		KUNIT_EXPECT_EQ(test, apple_pmgr_gpu_busy_count(), 1U);
		KUNIT_EXPECT_EQ(test, misc->fabric_now, 1U);
		KUNIT_EXPECT_EQ(test, misc->dcs_now, 4U);
		KUNIT_EXPECT_FALSE(test, delayed_work_pending(&misc->idle_work));
		KUNIT_EXPECT_NE(test, work_busy(&misc->idle_work_prompt.work), 0U);
		KUNIT_EXPECT_TRUE(test, apple_pmgr_voters_active(misc));
		apple_pmgr_gpu_busy_put();
		KUNIT_EXPECT_EQ(test, apple_pmgr_gpu_busy_count(), 0U);
	}
	cancel_delayed_work_sync(&misc->idle_work_prompt);

	/* Busy timeout on step_to returns -ETIMEDOUT and leaves BUSY set. */
	pf_step_inject_busy = true;
	KUNIT_EXPECT_EQ(test, apple_pmgr_step_to(f->fabric, 4, 0, &last), -ETIMEDOUT);
	/* Subsequent write while BUSY is still set fails -EBUSY without stepping. */
	KUNIT_EXPECT_EQ(test, apple_pmgr_step_to(f->fabric, 4, 0, &last), -EBUSY);
	pf_step_inject_busy = false;
	writel(0x00000111, f->fabric);
	writel(0x00000444, f->dcs);
	misc->fabric_now = 1;
	misc->dcs_now = 4;

	/* Missing required voter regmap counts as busy on tracking stop. */
	misc->votes[2].regmap = NULL;
	apple_pmgr_gpu_tracking_stop();
	KUNIT_EXPECT_TRUE(test, apple_pmgr_voters_active(misc));
	KUNIT_EXPECT_EQ(test, misc->fabric_now, 4U);
	KUNIT_EXPECT_EQ(test, misc->dcs_now, 6U);

	/* Path validation rejects stepping across an invalid intermediate DCS state. */
	writel(0x00000666, f->dcs);
	KUNIT_EXPECT_EQ(test, apple_pmgr_step_to(f->dcs, 4, BIT(4) | BIT(6), &last), -EINVAL);
}

static void pf_gov_tiers_and_hysteresis_test(struct kunit *test)
{
	struct apple_pmgr_gov gov = { .tier = APPLE_PMGR_TIER_FULL };
	const struct apple_pmgr_gov_cfg *cfg = &apple_pmgr_gov_cfg_default;
	struct apple_pmgr_gov_sample idle_s = {
		.e_max = 5, .e_sum = 12, .p_max = 5, .p_sum = 8, .gpu_pct = 4,
	};
	struct apple_pmgr_gov_sample light_s = { .e_max = 15, .e_sum = 30 };
	struct apple_pmgr_gov_sample near_full_s = {
		.e_max = 64, .e_sum = 249, .p_max = 64, .p_sum = 84,
		.gpu_pct = 34, .gpu_inflight = true, .gpu_inflight_ms = 9,
	};
	struct apple_pmgr_gov_sample full_pmax = { .p_max = 65 };
	struct apple_pmgr_gov_sample full_psum = { .p_max = 40, .p_sum = 85 };
	struct apple_pmgr_gov_sample full_esum = { .e_max = 63, .e_sum = 250 };
	struct apple_pmgr_gov_sample full_gpu = { .gpu_pct = 35 };

	/* Prime at t=0 in FULL; 500 ms hold required for FULL -> LIGHT. */
	KUNIT_EXPECT_EQ(test, apple_pmgr_gov_step(&gov, cfg, PF_MS(0), &idle_s),
			APPLE_PMGR_TIER_FULL);
	KUNIT_EXPECT_EQ(test, apple_pmgr_gov_step(&gov, cfg, PF_MS(460), &idle_s),
			APPLE_PMGR_TIER_FULL);
	KUNIT_EXPECT_EQ(test, apple_pmgr_gov_step(&gov, cfg, PF_MS(500), &idle_s),
			APPLE_PMGR_TIER_LIGHT);

	/* Another 500 ms hold in LIGHT required for LIGHT -> IDLE. */
	KUNIT_EXPECT_EQ(test, apple_pmgr_gov_step(&gov, cfg, PF_MS(960), &idle_s),
			APPLE_PMGR_TIER_LIGHT);
	KUNIT_EXPECT_EQ(test, apple_pmgr_gov_step(&gov, cfg, PF_MS(1000), &idle_s),
			APPLE_PMGR_TIER_IDLE);

	/* Boundary idle sample keeps IDLE. */
	KUNIT_EXPECT_EQ(test, apple_pmgr_gov_step(&gov, cfg, PF_MS(1200), &idle_s),
			APPLE_PMGR_TIER_IDLE);

	/* Fast raise: 1 sample promotes IDLE -> LIGHT. */
	KUNIT_EXPECT_EQ(test, apple_pmgr_gov_step(&gov, cfg, PF_MS(1400), &light_s),
			APPLE_PMGR_TIER_LIGHT);

	/* Just below every FULL threshold stays at LIGHT indefinitely. */
	KUNIT_EXPECT_EQ(test, apple_pmgr_gov_step(&gov, cfg, PF_MS(1600), &near_full_s),
			APPLE_PMGR_TIER_LIGHT);
	KUNIT_EXPECT_EQ(test, apple_pmgr_gov_step(&gov, cfg, PF_MS(1640), &near_full_s),
			APPLE_PMGR_TIER_LIGHT);

	/* Single-sample spike to FULL demand resets when followed by light load. */
	KUNIT_EXPECT_EQ(test, apple_pmgr_gov_step(&gov, cfg, PF_MS(1800), &full_pmax),
			APPLE_PMGR_TIER_LIGHT);
	KUNIT_EXPECT_EQ(test, apple_pmgr_gov_step(&gov, cfg, PF_MS(1840), &light_s),
			APPLE_PMGR_TIER_LIGHT);
	KUNIT_EXPECT_EQ(test, gov.full_streak, 0U);

	/* Two consecutive samples promote LIGHT -> FULL (p_max >= 65). */
	KUNIT_EXPECT_EQ(test, apple_pmgr_gov_step(&gov, cfg, PF_MS(2000), &full_pmax),
			APPLE_PMGR_TIER_LIGHT);
	KUNIT_EXPECT_EQ(test, apple_pmgr_gov_step(&gov, cfg, PF_MS(2040), &full_pmax),
			APPLE_PMGR_TIER_FULL);

	/* Verify p_sum >= 85, e_sum >= 250, and gpu_pct >= 35 each promote in 2 samples. */
	KUNIT_EXPECT_EQ(test, apple_pmgr_gov_step(&gov, cfg, PF_MS(2600), &light_s),
			APPLE_PMGR_TIER_LIGHT);
	KUNIT_EXPECT_EQ(test, apple_pmgr_gov_step(&gov, cfg, PF_MS(2800), &full_psum),
			APPLE_PMGR_TIER_LIGHT);
	KUNIT_EXPECT_EQ(test, apple_pmgr_gov_step(&gov, cfg, PF_MS(2840), &full_psum),
			APPLE_PMGR_TIER_FULL);

	KUNIT_EXPECT_EQ(test, apple_pmgr_gov_step(&gov, cfg, PF_MS(3400), &light_s),
			APPLE_PMGR_TIER_LIGHT);
	KUNIT_EXPECT_EQ(test, apple_pmgr_gov_step(&gov, cfg, PF_MS(3600), &full_esum),
			APPLE_PMGR_TIER_LIGHT);
	KUNIT_EXPECT_EQ(test, apple_pmgr_gov_step(&gov, cfg, PF_MS(3640), &full_esum),
			APPLE_PMGR_TIER_FULL);

	KUNIT_EXPECT_EQ(test, apple_pmgr_gov_step(&gov, cfg, PF_MS(4200), &light_s),
			APPLE_PMGR_TIER_LIGHT);
	KUNIT_EXPECT_EQ(test, apple_pmgr_gov_step(&gov, cfg, PF_MS(4400), &full_gpu),
			APPLE_PMGR_TIER_LIGHT);
	KUNIT_EXPECT_EQ(test, apple_pmgr_gov_step(&gov, cfg, PF_MS(4440), &full_gpu),
			APPLE_PMGR_TIER_FULL);

	/* Residency sums match elapsed time across all tiers. */
	KUNIT_EXPECT_EQ(test, gov.residency_ns[APPLE_PMGR_TIER_IDLE], PF_MS(400));
	KUNIT_EXPECT_EQ(test, gov.residency_ns[APPLE_PMGR_TIER_LIGHT], PF_MS(1860));
	KUNIT_EXPECT_EQ(test, gov.residency_ns[APPLE_PMGR_TIER_FULL], PF_MS(2180));
	KUNIT_EXPECT_EQ(test, gov.transitions, 10ULL);
}

static void pf_gov_gpu_inflight_and_ratelimit_test(struct kunit *test)
{
	struct apple_pmgr_gov gov = { .tier = APPLE_PMGR_TIER_IDLE };
	const struct apple_pmgr_gov_cfg *cfg = &apple_pmgr_gov_cfg_default;
	struct apple_pmgr_gov_sample frame_1ms = {
		.gpu_pct = 6, .gpu_inflight = true, .gpu_inflight_ms = 1,
	};
	struct apple_pmgr_gov_sample gpu_long = {
		.gpu_pct = 25, .gpu_inflight = true, .gpu_inflight_ms = 10,
	};
	struct apple_pmgr_gov_sample override_s = { .hard_override = true };

	/* 1 ms compositor frame raises IDLE -> LIGHT in 1 sample and stays LIGHT. */
	KUNIT_EXPECT_EQ(test, apple_pmgr_gov_step(&gov, cfg, PF_MS(100), &frame_1ms),
			APPLE_PMGR_TIER_LIGHT);
	KUNIT_EXPECT_EQ(test, apple_pmgr_gov_step(&gov, cfg, PF_MS(140), &frame_1ms),
			APPLE_PMGR_TIER_LIGHT);

	/* Continuous GPU job >= 10 ms within 100 ms min_interval is rate-limited. */
	KUNIT_EXPECT_EQ(test, apple_pmgr_gov_step(&gov, cfg, PF_MS(180), &gpu_long),
			APPLE_PMGR_TIER_LIGHT);
	KUNIT_EXPECT_EQ(test, gov.rate_limited, 1ULL);

	/* Once min_interval (100 ms) elapses, GPU >= 10 ms promotes in 1 sample. */
	KUNIT_EXPECT_EQ(test, apple_pmgr_gov_step(&gov, cfg, PF_MS(200), &gpu_long),
			APPLE_PMGR_TIER_FULL);

	/* Hard override from IDLE bypasses both 2-sample streak and rate limit. */
	gov = (struct apple_pmgr_gov){
		.tier = APPLE_PMGR_TIER_IDLE,
		.initialized = true,
		.transitioned = true,
		.last_transition_ns = PF_MS(1000),
		.tier_since_ns = PF_MS(1000),
	};
	KUNIT_EXPECT_EQ(test, apple_pmgr_gov_step(&gov, cfg, PF_MS(1020), &override_s),
			APPLE_PMGR_TIER_FULL);
	KUNIT_EXPECT_EQ(test, gov.rate_limited, 0ULL);
	KUNIT_EXPECT_EQ(test, gov.transitions, 1ULL);
}

static void pf_tiered_eval_order_and_timers_test(struct kunit *test)
{
	struct pf_test *f = test->priv;
	struct apple_pmgr_misc *misc = &f->misc;

	pf_setup_t8103(test, f, true);
	kunit_activate_static_stub(test, apple_pmgr_sample_load, pf_fake_sample_load);
	WRITE_ONCE(idle_fabric_pstate, 1);
	WRITE_ONCE(idle_dcs_pstate, 4);

	/* Prime at t=0 in FULL; quiet for 500 ms -> demote FULL (4,6) -> LIGHT (2,4). */
	f->sample = (struct apple_pmgr_gov_sample){};
	apple_pmgr_tiered_eval(misc, PF_MS(0));
	KUNIT_EXPECT_EQ(test, misc->gov.tier, APPLE_PMGR_TIER_FULL);
	KUNIT_EXPECT_TRUE(test, delayed_work_pending(&misc->idle_work_prompt));
	KUNIT_EXPECT_FALSE(test, delayed_work_pending(&misc->idle_work));

	/* FULL -> LIGHT demotes DCS first (5, 4), then fabric (3, 2). */
	pf_step_cnt = 0;
	apple_pmgr_tiered_eval(misc, PF_MS(500));
	KUNIT_EXPECT_EQ(test, misc->gov.tier, APPLE_PMGR_TIER_LIGHT);
	KUNIT_EXPECT_EQ(test, misc->fabric_now, 2U);
	KUNIT_EXPECT_EQ(test, misc->dcs_now, 4U);
	KUNIT_EXPECT_EQ(test, pf_step_cnt, 4U);
	KUNIT_EXPECT_EQ(test, pf_step_log[0], 5U);
	KUNIT_EXPECT_EQ(test, pf_step_log[1], 4U);
	KUNIT_EXPECT_EQ(test, pf_step_log[2], 3U);
	KUNIT_EXPECT_EQ(test, pf_step_log[3], 2U);
	KUNIT_EXPECT_TRUE(test, delayed_work_pending(&misc->idle_work_prompt));
	KUNIT_EXPECT_FALSE(test, delayed_work_pending(&misc->idle_work));

	/* Quiet another 500 ms -> demote LIGHT (2,4) -> IDLE (1,4); deferrable timer armed. */
	pf_step_cnt = 0;
	apple_pmgr_tiered_eval(misc, PF_MS(1000));
	KUNIT_EXPECT_EQ(test, misc->gov.tier, APPLE_PMGR_TIER_IDLE);
	KUNIT_EXPECT_EQ(test, misc->fabric_now, 1U);
	KUNIT_EXPECT_EQ(test, misc->dcs_now, 4U);
	KUNIT_EXPECT_EQ(test, pf_step_cnt, 1U);
	KUNIT_EXPECT_EQ(test, pf_step_log[0], 1U);
	KUNIT_EXPECT_TRUE(test, delayed_work_pending(&misc->idle_work));
	KUNIT_EXPECT_FALSE(test, delayed_work_pending(&misc->idle_work_prompt));

	/* Light E-core activity promotes IDLE (1,4) -> LIGHT (2,4) without touching DCS. */
	f->sample = (struct apple_pmgr_gov_sample){ .e_max = 20, .e_sum = 35 };
	pf_step_cnt = 0;
	apple_pmgr_tiered_eval(misc, PF_MS(1200));
	KUNIT_EXPECT_EQ(test, misc->gov.tier, APPLE_PMGR_TIER_LIGHT);
	KUNIT_EXPECT_EQ(test, misc->fabric_now, 2U);
	KUNIT_EXPECT_EQ(test, misc->dcs_now, 4U);
	KUNIT_EXPECT_EQ(test, pf_step_cnt, 1U);
	KUNIT_EXPECT_EQ(test, pf_step_log[0], 2U);
	KUNIT_EXPECT_TRUE(test, delayed_work_pending(&misc->idle_work_prompt));
	KUNIT_EXPECT_FALSE(test, delayed_work_pending(&misc->idle_work));

	/* Heavy P-core load promotes LIGHT -> FULL: fabric first (3,4), then DCS (5,6). */
	f->sample = (struct apple_pmgr_gov_sample){ .p_max = 75, .p_sum = 90 };
	apple_pmgr_tiered_eval(misc, PF_MS(1400));
	KUNIT_EXPECT_EQ(test, misc->gov.tier, APPLE_PMGR_TIER_LIGHT);
	pf_step_cnt = 0;
	apple_pmgr_tiered_eval(misc, PF_MS(1440));
	KUNIT_EXPECT_EQ(test, misc->gov.tier, APPLE_PMGR_TIER_FULL);
	KUNIT_EXPECT_EQ(test, misc->fabric_now, 4U);
	KUNIT_EXPECT_EQ(test, misc->dcs_now, 6U);
	KUNIT_EXPECT_EQ(test, pf_step_cnt, 4U);
	KUNIT_EXPECT_EQ(test, pf_step_log[0], 3U);
	KUNIT_EXPECT_EQ(test, pf_step_log[1], 4U);
	KUNIT_EXPECT_EQ(test, pf_step_log[2], 5U);
	KUNIT_EXPECT_EQ(test, pf_step_log[3], 6U);

	/* Return to IDLE; venc_sys override promotes immediately (0-sample latency). */
	f->sample = (struct apple_pmgr_gov_sample){};
	apple_pmgr_tiered_eval(misc, PF_MS(2000));
	apple_pmgr_tiered_eval(misc, PF_MS(2500));
	KUNIT_EXPECT_EQ(test, misc->gov.tier, APPLE_PMGR_TIER_IDLE);

	f->votes[3] = PF_VOTE_BUSY;	/* venc_sys */
	pf_step_cnt = 0;
	apple_pmgr_tiered_eval(misc, PF_MS(2520));
	KUNIT_EXPECT_EQ(test, misc->gov.tier, APPLE_PMGR_TIER_FULL);
	KUNIT_EXPECT_EQ(test, misc->fabric_now, 4U);
	KUNIT_EXPECT_EQ(test, misc->dcs_now, 6U);
	KUNIT_EXPECT_EQ(test, pf_step_cnt, 5U);
	KUNIT_EXPECT_EQ(test, pf_step_log[0], 2U);
	KUNIT_EXPECT_EQ(test, pf_step_log[1], 3U);
	KUNIT_EXPECT_EQ(test, pf_step_log[2], 4U);
	KUNIT_EXPECT_EQ(test, pf_step_log[3], 5U);
	KUNIT_EXPECT_EQ(test, pf_step_log[4], 6U);
	f->votes[3] = PF_VOTE_IDLE;

	/* Verify avd_sys and holds overrides also promote immediately from IDLE. */
	apple_pmgr_tiered_eval(misc, PF_MS(3100));
	apple_pmgr_tiered_eval(misc, PF_MS(3600));
	KUNIT_EXPECT_EQ(test, misc->gov.tier, APPLE_PMGR_TIER_IDLE);
	f->votes[1] = PF_VOTE_BUSY;	/* avd_sys */
	apple_pmgr_tiered_eval(misc, PF_MS(3610));
	KUNIT_EXPECT_EQ(test, misc->gov.tier, APPLE_PMGR_TIER_FULL);
	f->votes[1] = PF_VOTE_IDLE;

	/* Deep sleep prepare() from IDLE raises to (4, 6), and complete() resumes cleanly. */
	apple_pmgr_tiered_eval(misc, PF_MS(4200));
	apple_pmgr_tiered_eval(misc, PF_MS(4700));
	KUNIT_EXPECT_EQ(test, misc->gov.tier, APPLE_PMGR_TIER_IDLE);
	KUNIT_EXPECT_EQ(test, misc->fabric_now, 1U);
	KUNIT_EXPECT_EQ(test, misc->dcs_now, 4U);

	KUNIT_ASSERT_EQ(test, apple_pmgr_misc_prepare(misc->dev), 0);
	KUNIT_EXPECT_TRUE(test, misc->sleeping);
	KUNIT_EXPECT_EQ(test, misc->fabric_now, 4U);
	KUNIT_EXPECT_EQ(test, misc->dcs_now, 6U);
	KUNIT_EXPECT_EQ(test, misc->gov.tier, APPLE_PMGR_TIER_FULL);
	KUNIT_EXPECT_FALSE(test, delayed_work_pending(&misc->idle_work));
	KUNIT_EXPECT_FALSE(test, delayed_work_pending(&misc->idle_work_prompt));

	misc->gov_primed = true;
	apple_pmgr_misc_complete(misc->dev);
	KUNIT_EXPECT_FALSE(test, misc->sleeping);
	KUNIT_EXPECT_TRUE(test, flush_delayed_work(&misc->idle_work));
	cancel_delayed_work_sync(&misc->idle_work);
	cancel_delayed_work_sync(&misc->idle_work_prompt);
}

static void pf_tiered_optout_and_dcs_floor_test(struct kunit *test)
{
	struct pf_test *f = test->priv;
	struct apple_pmgr_misc *misc = &f->misc;
	u32 fab = 0, dcs = 0;

	pf_setup_t8103(test, f, true);
	kunit_activate_static_stub(test, apple_pmgr_sample_load, pf_fake_sample_load);

	/* idle_fabric_pstate=0 keeps fabric at 4 while DCS scales 4..6. */
	apple_pmgr_tier_targets(misc, APPLE_PMGR_TIER_IDLE, 0, 4, &fab, &dcs);
	KUNIT_EXPECT_EQ(test, fab, 4U);
	KUNIT_EXPECT_EQ(test, dcs, 4U);
	apple_pmgr_tier_targets(misc, APPLE_PMGR_TIER_LIGHT, 0, 4, &fab, &dcs);
	KUNIT_EXPECT_EQ(test, fab, 4U);
	KUNIT_EXPECT_EQ(test, dcs, 4U);
	apple_pmgr_tier_targets(misc, APPLE_PMGR_TIER_FULL, 0, 4, &fab, &dcs);
	KUNIT_EXPECT_EQ(test, fab, 4U);
	KUNIT_EXPECT_EQ(test, dcs, 6U);

	/* idle_dcs_pstate=0 keeps DCS at 6 while fabric scales 1..2..4. */
	apple_pmgr_tier_targets(misc, APPLE_PMGR_TIER_IDLE, 1, 0, &fab, &dcs);
	KUNIT_EXPECT_EQ(test, fab, 1U);
	KUNIT_EXPECT_EQ(test, dcs, 6U);
	apple_pmgr_tier_targets(misc, APPLE_PMGR_TIER_LIGHT, 1, 0, &fab, &dcs);
	KUNIT_EXPECT_EQ(test, fab, 2U);
	KUNIT_EXPECT_EQ(test, dcs, 6U);
	apple_pmgr_tier_targets(misc, APPLE_PMGR_TIER_FULL, 1, 0, &fab, &dcs);
	KUNIT_EXPECT_EQ(test, fab, 4U);
	KUNIT_EXPECT_EQ(test, dcs, 6U);

	/* DCS >= 4 invariant: invalid DCS target falls back to active_state (6). */
	apple_pmgr_tier_targets(misc, APPLE_PMGR_TIER_IDLE, 1, 3, &fab, &dcs);
	KUNIT_EXPECT_EQ(test, fab, 1U);
	KUNIT_EXPECT_EQ(test, dcs, 6U);
	misc->light_dcs = 3;
	apple_pmgr_tier_targets(misc, APPLE_PMGR_TIER_LIGHT, 1, 4, &fab, &dcs);
	KUNIT_EXPECT_EQ(test, fab, 2U);
	KUNIT_EXPECT_EQ(test, dcs, 4U);
	misc->dcs_valid_states = BIT(4) | BIT(6);
	misc->light_dcs = 5;
	apple_pmgr_tier_targets(misc, APPLE_PMGR_TIER_LIGHT, 1, 4, &fab, &dcs);
	KUNIT_EXPECT_EQ(test, fab, 2U);
	KUNIT_EXPECT_EQ(test, dcs, 6U);
	misc->dcs_valid_states = DCS_VALID_STATES_T8103;
	misc->light_dcs = 4;

	/* Both 0 disables scaling, raises to (4, 6), and arms no work. */
	misc->fabric_now = 1;
	misc->dcs_now = 4;
	writel(0x00000111, f->fabric);
	writel(0x00000444, f->dcs);
	WRITE_ONCE(idle_fabric_pstate, 0);
	WRITE_ONCE(idle_dcs_pstate, 0);
	apple_pmgr_tiered_eval(misc, PF_MS(100));
	KUNIT_EXPECT_EQ(test, misc->fabric_now, 4U);
	KUNIT_EXPECT_EQ(test, misc->dcs_now, 6U);
	KUNIT_EXPECT_FALSE(test, delayed_work_pending(&misc->idle_work));
	KUNIT_EXPECT_FALSE(test, delayed_work_pending(&misc->idle_work_prompt));
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
	    pf_dt_foreign("apple,t6000-pmp-v2"))
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
	static const struct pf_dt_child c_venc[] = {
		{ "power-controller@100", "gfx", 0x100 },
		{ "power-controller@110", "avd_sys", 0x110 },
		{ "power-controller@118", "isp_sys", 0x118 },
		{ "power-controller@128", "venc_sys", 0x128 },
	};
	struct pf_test *f = test->priv;
	struct apple_pmgr_misc *misc = &f->misc;
	struct regmap *map = syscon_node_to_regmap(pf_dt_pmgr);

	pf_dt_require_fixture_only(test);

	KUNIT_ASSERT_FALSE(test, IS_ERR(map));
	/* Optional venc_sys absent: voters_ok is still true. */
	KUNIT_EXPECT_TRUE(test, pf_dt_find(test, misc, c, ARRAY_SIZE(c)));
	/* Matched by label, in the order of apple_pmgr_fabric_voters. */
	KUNIT_EXPECT_EQ(test, misc->votes[0].reg, 0x100U);
	KUNIT_EXPECT_EQ(test, misc->votes[1].reg, 0x110U);
	KUNIT_EXPECT_EQ(test, misc->votes[2].reg, 0x118U);
	KUNIT_EXPECT_PTR_EQ(test, misc->votes[0].regmap, map);
	KUNIT_EXPECT_PTR_EQ(test, misc->votes[1].regmap, map);
	KUNIT_EXPECT_PTR_EQ(test, misc->votes[2].regmap, map);
	KUNIT_EXPECT_NULL(test, misc->votes[3].regmap);

	/* The voters read through the parent's register map. */
	pf_dt_regs[0x110 / 4] = PF_VOTE_IDLE;
	pf_dt_regs[0x118 / 4] = PF_VOTE_IDLE;
	pf_dt_regs[0x100 / 4] = PF_VOTE_BUSY;
	KUNIT_EXPECT_TRUE(test, apple_pmgr_voters_active(misc));
	pf_dt_regs[0x100 / 4] = PF_VOTE_IDLE;
	KUNIT_EXPECT_FALSE(test, apple_pmgr_voters_active(misc));

	/* When venc_sys is present, it is bound and active checks include it. */
	KUNIT_EXPECT_TRUE(test, pf_dt_find(test, misc, c_venc, ARRAY_SIZE(c_venc)));
	KUNIT_EXPECT_EQ(test, misc->votes[3].reg, 0x128U);
	KUNIT_EXPECT_PTR_EQ(test, misc->votes[3].regmap, map);
	pf_dt_regs[0x128 / 4] = PF_VOTE_BUSY;
	KUNIT_EXPECT_TRUE(test, apple_pmgr_voters_active(misc));
	pf_dt_regs[0x128 / 4] = PF_VOTE_IDLE;
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
	KUNIT_CASE(pf_t8103_gpu_tracking_and_dcs_valid_test),
	KUNIT_CASE(pf_gov_tiers_and_hysteresis_test),
	KUNIT_CASE(pf_gov_gpu_inflight_and_ratelimit_test),
	KUNIT_CASE(pf_tiered_eval_order_and_timers_test),
	KUNIT_CASE(pf_tiered_optout_and_dcs_floor_test),
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
