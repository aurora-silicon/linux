/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * T6030 (M3 Pro). Included by sleep.c.
 *
 * The entry is macOS 27.2's on J516s, traced under aura's hypervisor: three runs to the
 * commit agree write for write, and two more hooked the memory side, the coprocessors'
 * mailboxes and the SMC's shared buffer. In macOS's order:
 *
 *  - .final_check: the SMC's last returning check, then, untested, the end of a dry run;
 *  - .prepare_regs, before the coprocessors stop: the PMU's sleep stage, the sleep's notify
 *    mask on nub-spmi-a0 and nub-spmi0, then the DT's apple,seq-prepare (the PTD's
 *    no-demand votes);
 *  - SIO's stop, then .after_early: the SIO CPU's power word, then the DT's
 *    apple,seq-notice (the votes again);
 *  - the PMP's stop, then .commit_notice: the PMP's and its SRAM's power words, then the
 *    SMC's slpa and AP idle;
 *  - .commit_regs, the other cores stopped: the storage PHY's power word, a read of each word
 *    the wake writes back, then the DT's apple,seq-commit (WD0 off, the AMCCs' ways and
 *    planes, the DCS, the GPU's PMGR, the CPU power gates, the perf word, AFR, the cluster
 *    words, SIO, the clock muxes and CLLT);
 *  - .pre_commit: no sooner after the SMC's idle than the policy's spacing. The core then
 *    writes the commit word.
 *
 * Each power word goes down as macOS's does, AUTO_ENABLE and the flags off first, then the
 * target, and is polled off. Past the point of no return there is no going back: a word that
 * doesn't go off ends the sleep in a panic before slpa or the commit, never in a commit with
 * it on. The register sequences are struct sleep_seq lists on this node's reg entries, by
 * index. The SMC's pmVC and spacings are the DT's apple,smc-sleep-policy.
 *
 * Untested: no sleep has been committed from Linux on T6030. Deep sleep is offered only with
 * apple_sleep.deep=1, and .final_check stops a sleep before the enclave and the coprocessors
 * stop unless apple_sleep.untested_commit=1. With the SMC policy macsmc sends nothing past
 * its warning, pmVC and wake type before that point: no slpa on a dry run.
 *
 * The wake is macOS 27.2's on J516s, as aura's wake record has it, step for step:
 *  - the DT's apple,seq-wake-early: each complex's word, waited on, then two CPU power gates;
 *  - the power words macOS turns on before its memory side, in its order, as Linux had them,
 *    each on before the memory side starts;
 *  - the DT's apple,seq-wake: the other two gates, then the AMCCs' ways and planes and the DCS,
 *    the reverse of the commit's, only where a bit was on at the entry, each after its words
 *    as read then;
 *  - the words macOS turns on after its memory side (APCIE_PHY_SW to SIO_CPU), the notify
 *    masks, the display complexes, I2C and SPI, then any other word the wake left otherwise.
 * Past the commit nothing goes back: a wake sequence without its snapshot or with an entry that
 * fails, or a power word not on in the end, ends the wake in a panic that names it. What the
 * wake reads first is cleaned to the point of coherency before the commit.
 */

/* Power words in "ps", from the J516s ADT's pmgr devices */
#define T6030_PS_GPIO			0x120
#define T6030_PS_AP_TMM			0x1a8
#define T6030_PS_SIO			0x1b8
#define T6030_PS_DISP_SYS		0x1c0
#define T6030_PS_SIO_CPU		0x1d0
#define T6030_PS_DISP_FE		0x258
#define T6030_PS_DISPEXT0_SYS		0x3b8
#define T6030_PS_DISPEXT1_SYS		0x3c0
#define T6030_PS_DISPEXT0_FE		0x3d8
#define T6030_PS_DISPEXT0_CPU		0x3e0
#define T6030_PS_DISPEXT1_FE		0x3f0
#define T6030_PS_DISPEXT1_CPU		0x3f8
#define T6030_PS_PMP			0x4c8
#define T6030_PS_PMS_SRAM		0x4d0
#define T6030_PS_APCIE_GP		0x500
#define T6030_PS_ANS			0x510
#define T6030_PS_APCIE_SYS_GP		0x558
#define T6030_PS_APCIE_ST		0x568
#define T6030_PS_APCIE_SYS_ST		0x5b0
#define T6030_PS_APCIE_PHY_SW		0x5b8
#define T6030_PS_SEP			0xc00
#define T6030_PS_DISP_CPU		0x10000

/* "cluster": the E and P complex words */
#define T6030_CLUSTER_E			0x0
#define T6030_CLUSTER_P			0x4

/* The register sequences' blocks: this node's reg entries, at most this many */
#define T6030_BLOCKS			24

/* PMU scratch note: the dry run ended at .final_check, as the shared path's (0x31) */
#define T6030_NOTE_DRY_RUN		0x31

/*
 * Lowered in this order, and raised again in reverse at the wake: SIO's CPU by .after_early,
 * the PMP and its SRAM by .commit_notice, the storage PHY by .commit_regs.
 */
static const u32 t6030_lowered[] = {
	T6030_PS_SIO_CPU, T6030_PS_PMP, T6030_PS_PMS_SRAM, T6030_PS_APCIE_PHY_SW,
};

enum { T6030_LOWER_SIO_CPU, T6030_LOWER_PMP, T6030_LOWER_PHY = 3 };

/* SIO's and its CPU's words: apple,seq-commit clears their AUTO_ENABLE, the wake puts them back */
static const u32 t6030_auto_off[] = { T6030_PS_SIO, T6030_PS_SIO_CPU };

/* Per probe, in s->soc_state */
struct t6030_sleep {
	void __iomem *blk[T6030_BLOCKS];
	u32 blk_size[T6030_BLOCKS];
	struct sleep_seq prepare, notice, commit, wake_early, wake;
	u32 notify_saved[2];
	bool notify_staged;
	u32 lowered_saved[ARRAY_SIZE(t6030_lowered)];
	unsigned int nlowered;
	u32 auto_saved[ARRAY_SIZE(t6030_auto_off)];
	bool auto_staged;
};

static struct apple_sleep_smc_policy t6030_smc_policy;

static struct t6030_sleep *t6030_state(struct apple_sleep *s)
{
	return s->soc_state;
}

/* A sequence's block: this node's reg entry @b, mapped once per probe, or 0 */
static u32 t6030_block(struct apple_sleep *s, u32 b)
{
	struct t6030_sleep *t = t6030_state(s);
	struct resource res;

	if (b >= T6030_BLOCKS)
		return 0;
	if (t->blk[b])
		return t->blk_size[b];
	/* An entry's alignment is checked against the block: the block's own must hold too */
	if (of_address_to_resource(s->dev->of_node, b, &res) || !IS_ALIGNED(res.start, sizeof(u64)))
		return 0;
	/* Shared with the drivers of those blocks: not claimed */
	t->blk[b] = devm_ioremap(s->dev, res.start, resource_size(&res));
	t->blk_size[b] = t->blk[b] ? resource_size(&res) : 0;
	return t->blk_size[b];
}

/*
 * All of them, each whole: one missing, empty or with an entry that doesn't fit fails the probe.
 * Only the wake's, read at .commit_regs, may restore or test what was read.
 */
static int t6030_sleep_seq_get(struct apple_sleep *s, const char *name, struct sleep_seq *seq,
			       bool saves)
{
	int ret = sleep_seq_get(s, name, seq, t6030_block, saves);

	if (ret)
		return dev_err_probe(s->dev, ret, "%s unusable\n", name);
	if (!seq->n)
		return dev_err_probe(s->dev, -ENODATA, "no %s\n", name);
	return 0;
}

static int t6030_sleep_map_regs(struct apple_sleep *s)
{
	static const struct {
		const char *name;
		size_t offset;
	} regs[] = {
		{ "ps", offsetof(struct apple_sleep, ps) },
		{ "pwrgate", offsetof(struct apple_sleep, pwrgate) },
		{ "cpu-start", offsetof(struct apple_sleep, cpu_start) },
		{ "cllt", offsetof(struct apple_sleep, cllt) },
		{ "cluster", offsetof(struct apple_sleep, cluster) },
		{ "commit", offsetof(struct apple_sleep, commit) },
		{ "wdt", offsetof(struct apple_sleep, wdt) },
		{ "wake", offsetof(struct apple_sleep, wake) },
		{ "spmi", offsetof(struct apple_sleep, spmi) },
		{ "ps-aon", offsetof(struct apple_sleep, ps_aon) },
		{ "spmi-a0", offsetof(struct apple_sleep, spmi_a0) },
		{ "clk-mux", offsetof(struct apple_sleep, clk_mux) },
		{ "clk-perf", offsetof(struct apple_sleep, clk_perf) },
	};
	struct device_node *np = s->dev->of_node;
	struct t6030_sleep *t;
	u32 v[6];
	unsigned int i;
	int ret;

	t = devm_kzalloc(s->dev, sizeof(*t), GFP_KERNEL);
	if (!t)
		return -ENOMEM;
	s->soc_state = t;

	for (i = 0; i < ARRAY_SIZE(regs); i++) {
		void __iomem **field = (void __iomem **)((u8 *)s + regs[i].offset);

		*field = apple_sleep_map(s->dev, regs[i].name);
		if (IS_ERR(*field))
			return dev_err_probe(s->dev, PTR_ERR(*field), "no %s registers\n",
					     regs[i].name);
	}
	ret = of_address_to_resource(np, of_property_match_string(np, "reg-names", "ps"),
				     &s->ps_res);
	if (ret)
		return ret;
	/* As the common map keeps them: domains drivers lowered, and each listed word's books */
	s->driver_down = devm_bitmap_zalloc(s->dev, resource_size(&s->ps_res) / 4, GFP_KERNEL);
	if (!s->driver_down)
		return -ENOMEM;
	ret = apple_sleep_alloc_books(s);
	if (ret)
		return ret;

	if (of_property_read_u32_array(np, "apple,smc-sleep-policy", v, ARRAY_SIZE(v)))
		return dev_err_probe(s->dev, -ENODATA, "no SMC sleep spacings in the DT\n");
	t6030_smc_policy = (struct apple_sleep_smc_policy) {
		.pmvc = v[0], .warn_to_pmvc_us = v[1], .pmvc_to_slpa_us = v[2],
		.slpa_to_idle_us = v[3], .idle_to_timer_us = v[4], .waka_to_pmvc_us = v[5],
	};

	ret = t6030_sleep_seq_get(s, "apple,seq-prepare", &t->prepare, false) ?:
	      t6030_sleep_seq_get(s, "apple,seq-notice", &t->notice, false) ?:
	      t6030_sleep_seq_get(s, "apple,seq-commit", &t->commit, false) ?:
	      t6030_sleep_seq_get(s, "apple,seq-wake-early", &t->wake_early, true) ?:
	      t6030_sleep_seq_get(s, "apple,seq-wake", &t->wake, true);
	if (ret)
		return ret;

	return apple_sleep_read_wake_sources(s);
}

/*
 * The last returning check: the SMC's, with its scope held. An untested profile's dry run
 * ends here, before the enclave or any coprocessor stops, and notes so in PMU scratch.
 */
static int t6030_sleep_final_check(struct apple_sleep *s)
{
	int ret;

	if (!s->smc.held || !s->smc.saved_ops || !s->smc.saved_ops->check) {
		dev_info(s->dev, "final check: SMC scope held %d\n", s->smc.held);
		return -EBUSY;
	}
	ret = s->smc.saved_ops->check(s->smc.data);
	if (ret) {
		dev_info(s->dev, "final check: SMC check %d\n", ret);
		return ret;
	}
	if (s->soc->untested && !READ_ONCE(sleep_untested_commit)) {
		sleep_note(s, T6030_NOTE_DRY_RUN);
		return -EAGAIN;
	}
	return 0;
}

static int t6030_sleep_display_off(struct apple_sleep *s)
{
	/* The display processors' drivers stop them at their own suspend. */
	return 0;
}

static int t6030_sleep_cpu_prepare(u64 mpidr)
{
	return 0;
}

static void t6030_sleep_cpu_restore(u64 mpidr)
{
}

/* Past the point of no return a step that fails ends the sleep here, never in a commit. */
static void t6030_sleep_failed(const char *what)
{
	panic("T6030 sleep: %s failed, not committing", what);
}

/* The same for a coprocessor's last stop that failed so far in this sleep */
static void t6030_sleep_check_stops(void)
{
	struct apple_sleep_coproc *c = apple_sleep_coprocs_stop_failed();

	if (c)
		panic("T6030 sleep: %s's stop failed (%d), not committing", dev_name(c->dev),
		      c->stop_ret);
}

/*
 * After the enclave, before the coprocessors stop: the PMU's stage, the notify masks, then the
 * votes. macOS's nub-spmi-a0 word comes before the first vote in every run.
 */
static void t6030_sleep_prepare_regs(struct apple_sleep *s)
{
	struct t6030_sleep *t = t6030_state(s);

	sleep_pmu_write(s, s->soc->pmu_sleep_reg, s->soc->pmu_sleep_val);
	t->notify_saved[0] = readl(s->spmi_a0 + SPMI_NOTIFY_ENABLE);
	t->notify_saved[1] = readl(s->spmi + SPMI_NOTIFY_ENABLE);
	t->notify_staged = true;
	writel(s->soc->spmi_notify_sleep, s->spmi_a0 + SPMI_NOTIFY_ENABLE);
	writel(s->soc->spmi_notify_sleep, s->spmi + SPMI_NOTIFY_ENABLE);
	if (sleep_seq_run_strict(t->blk, &t->prepare))
		t6030_sleep_failed("apple,seq-prepare");
}

static void t6030_sleep_undo_regs(struct apple_sleep *s)
{
	struct t6030_sleep *t = t6030_state(s);

	if (!t->notify_staged)
		return;
	/* nub-spmi0 first, as macOS's wake */
	writel(t->notify_saved[1], s->spmi + SPMI_NOTIFY_ENABLE);
	writel(t->notify_saved[0], s->spmi_a0 + SPMI_NOTIFY_ENABLE);
	t->notify_staged = false;
}

/* Lowers t6030_lowered[@from] up to @to, saving each word, and polls each off */
static int t6030_sleep_lower(struct apple_sleep *s, unsigned int from, unsigned int to)
{
	struct t6030_sleep *t = t6030_state(s);
	unsigned int i;

	for (i = from; i < to; i++) {
		t->lowered_saved[i] = readl(s->ps + t6030_lowered[i]);
		t->nlowered = i + 1;
		sleep_ps_lower(s, t6030_lowered[i]);
		if (sleep_ps_wait_off(s, t6030_lowered[i])) {
			dev_err(s->dev, "power word %#x not off: %#x\n", t6030_lowered[i],
				readl(s->ps + t6030_lowered[i]));
			return -ETIMEDOUT;
		}
	}
	return 0;
}

/* SIO has stopped: its CPU's word, then the votes again, before the PMP stops */
static void t6030_sleep_after_early(struct apple_sleep *s)
{
	struct t6030_sleep *t = t6030_state(s);

	t6030_sleep_check_stops();
	if (t6030_sleep_lower(s, T6030_LOWER_SIO_CPU, T6030_LOWER_PMP))
		t6030_sleep_failed("SIO CPU power word");
	if (sleep_seq_run_strict(t->blk, &t->notice))
		t6030_sleep_failed("apple,seq-notice");
}

/*
 * The PMP has stopped: its word and its SRAM's, then the SMC's slpa and AP idle. An error
 * ends the sleep in the core's panic, before slpa where a word didn't go off.
 */
static int t6030_sleep_commit_notice(struct apple_sleep *s)
{
	int ret;

	t6030_sleep_check_stops();
	ret = t6030_sleep_lower(s, T6030_LOWER_PMP, T6030_LOWER_PHY);
	if (ret)
		return ret;
	ret = apple_sleep_smc_commit_notice(s);
	if (ret)
		return ret;
	if (!s->smc_idle_sent || s->smc_idle_ack < s->smc_idle_sent)
		return -EPROTO;
	return 0;
}

/*
 * The other cores stopped: the storage PHY's word, then what the wake sequences write back,
 * read before the DT's sequence to the commit word changes any of it
 */
static void t6030_sleep_commit_regs(struct apple_sleep *s)
{
	struct t6030_sleep *t = t6030_state(s);
	unsigned int i;

	/* A core that didn't take its stop would keep the engine waiting at the commit */
	if (s->cpus_stop_ret)
		t6030_sleep_failed("the other cores' stop");
	if (t6030_sleep_lower(s, T6030_LOWER_PHY, ARRAY_SIZE(t6030_lowered)))
		t6030_sleep_failed("storage PHY power word");
	for (i = 0; i < ARRAY_SIZE(t6030_auto_off); i++)
		t->auto_saved[i] = readl(s->ps + t6030_auto_off[i]);
	t->auto_staged = true;
	sleep_seq_save(t->blk, &t->wake_early);
	sleep_seq_save(t->blk, &t->wake);
	if (sleep_seq_run_strict(t->blk, &t->commit))
		t6030_sleep_failed("apple,seq-commit");
	/* Every listed word as the commit leaves it, for the wake's report */
	sleep_ps_save(s);
}

/*
 * What the wake reads before the caches are on again, cleaned to the point of coherency: the
 * kernel's data and resume context, this stack, the sleep state and its power word books, the
 * wake sequences with the words read for them, and this profile's state, its flags included.
 */
static void t6030_sleep_clean(struct apple_sleep *s)
{
	const struct apple_sleep_soc *soc = s->soc;
	struct t6030_sleep *t = t6030_state(s);

	t8132_clean_ram();
	dcache_clean_poc((unsigned long)task_stack_page(current),
			 (unsigned long)task_stack_page(current) + THREAD_SIZE);
	dcache_clean_poc((unsigned long)s, (unsigned long)(s + 1));
	dcache_clean_poc((unsigned long)s->ps_commit,
			 (unsigned long)(s->ps_commit + soc->ps_all.count));
	dcache_clean_poc((unsigned long)s->ps_aon_commit,
			 (unsigned long)(s->ps_aon_commit + soc->ps_aon_all.count));
	sleep_seq_clean(&t->wake_early);
	sleep_seq_clean(&t->wake);
	dcache_clean_poc((unsigned long)t, (unsigned long)(t + 1));
	dsb(sy);
}

/*
 * The last step before the commit word: the clean, then no sooner after the SMC's AP idle than
 * the policy's spacing, as on macOS
 */
static void t6030_sleep_pre_commit(struct apple_sleep *s)
{
	u64 at = s->smc_idle_sent + (u64)arch_timer_get_cntfrq() *
		 s->soc->smc_policy->idle_to_timer_us / USEC_PER_SEC;

	t6030_sleep_clean(s);
	while (__arch_counter_get_cntpct() < at)
		cpu_relax();
}

/* The commit's record of @offset, which the rewake puts back, as it was before the entry */
static void t6030_sleep_unstage(struct apple_sleep *s, u32 offset, u32 before)
{
	const struct apple_sleep_words *all = &s->soc->ps_all;
	int i = sleep_ps_find(all->offset, all->count, offset);

	if (i >= 0)
		s->ps_commit[i] = before;
}

/* A "ps-aon" word in a wake order */
#define T6030_AON(offset)		(SLEEP_PS_WAKE_AON | (offset))

/*
 * The power words macOS 27.2's wake on J516s turns on, in the order it first raises their
 * target (aura's whole wake record), in three steps around the rest of its wake. Words it
 * writes earlier with the target off, or never raises during its wake, are left to the end.
 *  - before the second pair of CPU power gates and the memory side, by 2.4 ms;
 */
static const u32 t6030_ps_wake_early[] = {
	0xc00, T6030_AON(0x98), 0x110, 0x118, 0x120, 0x170, 0x1b8, 0x1f0, 0x210, 0x228, 0x230,
	0x238, 0x240, 0x248, 0x260, 0x2a0, 0x2a8, 0x2b0, 0x2b8, 0x2c0, 0x2c8, 0x2d0, 0x2e0,
	0x408, 0x500, 0x510, 0x558, 0x568, 0x5b0,
};

/*
 *  - after the memory side, before the notify masks: APCIE_PHY_SW, SIO_ADMA, MCA0-3, PMS_SRAM,
 *    PMP, SIO_CPU (12 to 41 ms);
 */
static const u32 t6030_ps_wake_mid[] = {
	0x5b8, 0x2d8, 0x308, 0x388, 0x390, 0x398, 0x4d0, 0x4c8, 0x1d0,
};

/*  - after the notify masks: the display complexes, then I2C1-3 and SPI2 (57 to 70 ms). */
static const u32 t6030_ps_wake_late[] = {
	0x3b8, 0x3d8, 0x3e0, 0x3c0, 0x3f0, 0x3f8, 0x1c0, 0x258, 0x10000, 0x1f8, 0x208, 0x200,
	0x270,
};

static const struct apple_sleep_words t6030_wake_early_words = SLEEP_WORDS(t6030_ps_wake_early);
static const struct apple_sleep_words t6030_wake_mid_words = SLEEP_WORDS(t6030_ps_wake_mid);
static const struct apple_sleep_words t6030_wake_late_words = SLEEP_WORDS(t6030_ps_wake_late);

/*
 * A wake sequence, once, with what .commit_regs read for it. Past the commit there is no
 * going back: a sequence without its snapshot or with an entry that fails ends here, never in
 * a wake with the memory side half restored.
 */
static void t6030_sleep_wake_seq(struct apple_sleep *s, struct sleep_seq *seq, const char *name)
{
	struct t6030_sleep *t = t6030_state(s);
	int ret = sleep_seq_run_strict(t->blk, seq);

	seq->have_saved = false;
	if (ret)
		panic("T6030 wake: %s failed: %d", name, ret);
}

/*
 * Each power word written so far on again, after the retries, or the wake ends here, naming
 * the first that isn't, before anything that needs it runs
 */
static void t6030_sleep_wake_check_ps(struct apple_sleep *s, const char *step)
{
	const u32 *all = s->soc->ps_all.offset, *aon = s->soc->ps_aon_all.offset;
	int i;

	for (i = 0; i < s->soc->ps_all.count; i++)
		if (s->ps_late[i])
			panic("T6030 wake, %s: power word %#x not on: %#x", step, all[i],
			      readl(s->ps + all[i]));
	for (i = 0; i < s->soc->ps_aon_all.count; i++)
		if (s->ps_aon_late[i])
			panic("T6030 wake, %s: ps-aon word %#x not on: %#x", step, aon[i],
			      readl(s->ps_aon + aon[i]));
}

/*
 * Back on the boot core, in macOS's order: the DT's apple,seq-wake-early (each complex's word,
 * waited on, then two CPU power gates); the power words macOS turns on before its memory side,
 * as Linux had them, all on before going further; apple,seq-wake (the other two gates, then the
 * memory side, only what Linux had on); the words macOS turns on next; the notify masks; the
 * display complexes, I2C and SPI; any other word the wake left otherwise. The words the entry
 * lowered or took AUTO_ENABLE from go back as they were before it.
 */
static void t6030_sleep_wake(struct apple_sleep *s)
{
	struct t6030_sleep *t = t6030_state(s);
	unsigned int i;

	t6030_sleep_wake_seq(s, &t->wake_early, "apple,seq-wake-early");
	/* SIO_CPU is in both lists: its value before the lowering is the one before the entry */
	if (t->auto_staged)
		for (i = 0; i < ARRAY_SIZE(t6030_auto_off); i++)
			t6030_sleep_unstage(s, t6030_auto_off[i], t->auto_saved[i]);
	for (i = 0; i < t->nlowered; i++)
		t6030_sleep_unstage(s, t6030_lowered[i], t->lowered_saved[i]);
	t->auto_staged = false;
	t->nlowered = 0;
	sleep_ps_rewake_begin(s);
	sleep_ps_rewake_words(s, &t6030_wake_early_words, false);
	/* None of the memory side's registers before every early word is on */
	sleep_ps_rewake_settle(s);
	t6030_sleep_wake_check_ps(s, "before the memory side");
	t6030_sleep_wake_seq(s, &t->wake, "apple,seq-wake");
	sleep_ps_rewake_words(s, &t6030_wake_mid_words, false);
	t6030_sleep_undo_regs(s);
	sleep_ps_rewake_words(s, &t6030_wake_late_words, false);
	sleep_ps_rewake_rest(s);
	t6030_sleep_wake_check_ps(s, "at the end");
}

static void t6030_deep_wfi_arm(void)
{
	sysreg_clear_set_s(SYS_APL_DEEP_SLEEP, 0, DEEP_SLEEP_ENABLE);
}

/* Every power word in "ps" that the J516s ADT's pmgr devices name */
static const u32 t6030_ps_all[] = {
	0x0, 0x8, 0x10, 0x18, 0x20, 0x28, 0x30, 0x38, 0x40, 0x48,
	0x50, 0x58, 0x60, 0x68, 0x108, 0x110, 0x118, 0x120, 0x138, 0x140,
	0x148, 0x150, 0x158, 0x160, 0x168, 0x170, 0x178, 0x180, 0x190, 0x1a8,
	0x1b8, 0x1c0, 0x1c8, 0x1d0, 0x1d8, 0x1e0, 0x1e8, 0x1f0, 0x1f8, 0x200,
	0x208, 0x210, 0x218, 0x220, 0x228, 0x230, 0x238, 0x240, 0x248, 0x250,
	0x258, 0x260, 0x268, 0x270, 0x278, 0x280, 0x288, 0x290, 0x298, 0x2a0,
	0x2a8, 0x2b0, 0x2b8, 0x2c0, 0x2c8, 0x2d0, 0x2d8, 0x2e0, 0x2e8, 0x2f0,
	0x2f8, 0x300, 0x308, 0x388, 0x390, 0x398, 0x3a0, 0x3a8, 0x3b0, 0x3b8,
	0x3c0, 0x3c8, 0x3d0, 0x3d8, 0x3e0, 0x3e8, 0x3f0, 0x3f8, 0x408, 0x490,
	0x498, 0x4a0, 0x4a8, 0x4b0, 0x4b8, 0x4c0, 0x4c8, 0x4d0, 0x4d8, 0x4e0,
	0x4e8, 0x4f0, 0x4f8, 0x500, 0x508, 0x510, 0x518, 0x520, 0x528, 0x530,
	0x538, 0x540, 0x548, 0x550, 0x558, 0x560, 0x568, 0x570, 0x578, 0x580,
	0x588, 0x590, 0x598, 0x5a0, 0x5a8, 0x5b0, 0x5b8, 0xc00, 0x4000, 0x4008,
	0x4010, 0x4018, 0x4020, 0x4028, 0x8000, 0x8008, 0x8010, 0x8018, 0x8020, 0xc000,
	0xc008, 0xc010, 0xc018, 0x10000,
};

/* And in "ps-aon" */
static const u32 t6030_ps_aon_all[] = {
	0x0, 0x58, 0x60, 0x68, 0x70, 0x78, 0x88, 0x90, 0x98, 0xa0,
	0xa8, 0xb0, 0xb8, 0xc0, 0xc8, 0xd0, 0xd8, 0xe0, 0xe8, 0xf0,
};

/*
 * What macOS has stopped by its commit, from t6030.dtsi, failing closed (gate_fail_closed): an
 * entry missing from the tree refuses. So does one that must stop and still runs, a DCP, bound
 * or not, SIO, or ANS's CPU with its domain on, or one unbound whose run state can't be read.
 * SEP refuses without its prepared SLEEP request. The others are named when unbound.
 */
static const struct apple_sleep_gate_coproc t6030_gate_coprocs[] = {
	{ .path = "/soc/dcp@28ec00000", .must_stop = true, .ps = T6030_PS_DISP_CPU },
	{ .path = "/soc/dcp@2d2c00000", .must_stop = true, .ps = T6030_PS_DISPEXT0_CPU },
	{ .path = "/soc/dcp@2d6c00000", .must_stop = true, .ps = T6030_PS_DISPEXT1_CPU },
	/* Their CPU controls: SIO's only reg entry, and ANS's "ans" one */
	{ .path = "/soc/dma-controller@289c00000", .must_stop = true, .ps = T6030_PS_SIO_CPU,
	  .explicit_run_index = true, .run_index = 0 },
	{ .path = "/soc/nvme@38dcc0000", .must_stop = true, .ps = T6030_PS_ANS,
	  .explicit_run_index = true, .run_index = 1 },
	{ .path = "/soc/sep@29e400000" },
	{ .path = "/soc/mtp@37a400000" },
	{ .path = "/soc/aop@374c00000" },
	{ .path = "/soc/smc@36c400000" },
	{ .path = "/soc/gpu@292400000" },
};

static const struct apple_sleep_ops t6030_sleep_ops = {
	.deep_wfi_arm = t6030_deep_wfi_arm,
	.resume = cpu_resume,
	.aic_mask_all = aic_sleep_mask_all,
	.aic_ack = aic_sleep_ack,
	.aic_restore = aic_sleep_restore,
	.map_regs = t6030_sleep_map_regs,
	.display_off = t6030_sleep_display_off,
	.final_check = t6030_sleep_final_check,
	.prepare_regs = t6030_sleep_prepare_regs,
	.undo_regs = t6030_sleep_undo_regs,
	.after_early = t6030_sleep_after_early,
	.commit_notice = t6030_sleep_commit_notice,
	.commit_regs = t6030_sleep_commit_regs,
	.pre_commit = t6030_sleep_pre_commit,
	.wake = t6030_sleep_wake,
	.cpu_prepare = t6030_sleep_cpu_prepare,
	.cpu_restore = t6030_sleep_cpu_restore,
};

static const struct apple_sleep_soc t6030_sleep_soc = {
	.untested = true,
	.ops = &t6030_sleep_ops,
	.smc_policy = &t6030_smc_policy,
	.early_context = true,
	.own_wake = true,
	/* Each core takes its deep WFI from its stop bit, before the commit, as on macOS */
	.early_cpu_wfi = true,
	/* No write to WD2's registers in any of macOS's entries */
	.no_wd2 = true,
	/* The ADT's spmi-chapel on nub-spmi0, and the sleep stage macOS writes to it */
	.pmu_usid = 0xb,
	.pmu_sleep_reg = 0xf801,
	.pmu_sleep_val = 0x50,
	.spmi_notify_sleep = 0x2000001,
	.cpu_cluster_affinity = 1,
	.cpu_clusters = 2,
	.cpu_cores_per_cluster = 6,
	.cpu_start_stride = 6,
	.cluster_cpu = { T6030_CLUSTER_E, T6030_CLUSTER_P },
	.ncluster_cpu = 2,
	/* DCP, then the external DCPs; macOS lowers the internal one first, then 1, then 0 */
	.ndisp = 3,
	.display_order = { 0, 2, 1 },
	.ps = {
		.disp = {
			{ T6030_PS_DISP_CPU, T6030_PS_DISP_FE, T6030_PS_DISP_SYS },
			{ T6030_PS_DISPEXT0_CPU, T6030_PS_DISPEXT0_FE, T6030_PS_DISPEXT0_SYS },
			{ T6030_PS_DISPEXT1_CPU, T6030_PS_DISPEXT1_FE, T6030_PS_DISPEXT1_SYS },
		},
		.sio_cpu = T6030_PS_SIO_CPU,
		.sep = T6030_PS_SEP,
		.ap_tmm = T6030_PS_AP_TMM,
		.gpio = T6030_PS_GPIO,
		.ans = T6030_PS_ANS,
		.apcie_gp = T6030_PS_APCIE_GP,
		.apcie_sys_gp = T6030_PS_APCIE_SYS_GP,
		.apcie_st = T6030_PS_APCIE_ST,
		.apcie_sys_st = T6030_PS_APCIE_SYS_ST,
		.apcie_phy_sw = T6030_PS_APCIE_PHY_SW,
	},
	.gate_coprocs = t6030_gate_coprocs,
	.ngate_coprocs = ARRAY_SIZE(t6030_gate_coprocs),
	.gate_fail_closed = true,
	/* The wake block's layout isn't traced on T6030: no source is armed or read back. */
	.ps_all = SLEEP_WORDS(t6030_ps_all),
	.ps_aon_all = SLEEP_WORDS(t6030_ps_aon_all),
};
