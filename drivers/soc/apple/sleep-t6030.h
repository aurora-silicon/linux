/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/*
 * T6030 (M3 Pro, J516S). Included by sleep.c. UNTESTED: compile-tested on integration/aurora
 * only. A sleep stops before the commit unless apple_sleep.untested_commit=1.
 *
 * The shape follows T6040 (#2215), the other Pro die without region tables: early_context (the
 * whole entry runs inside cpu_suspend() on the saved context), the SoC's own register map,
 * commit_regs and wake, the secondaries released at the start of the commit and their power
 * words polled off, WD0 off, the AON AP deadline 0, the complex words' low nibbles to 0. Where
 * T6040 uses M4-only blocks (SOC-East, MasterMini, AOP-global wake, axi2af bridges, keep-alive
 * tables) T6030 has no counterpart. The wake block has T8140's layout (mask +0x8, status
 * +0x24, 7 words). The commit's steps are fixed: there is no runtime switch for them.
 *
 * Every address is from the J516S ADT. The register sequence was run on a J516S from an
 * out-of-tree 7.2.11 port, where a separate provider driver did the device side (display off,
 * SMC keys, PMP idle, SEP, memory cache). No provider is carried here: what it did either has
 * a core mechanism on integration/aurora (noted at each op) or is marked TODO(t6030).
 *
 * Measured on J516S (aurora-silicon/linux#2687), kept as data or comments below:
 *  - The CPU-start block's per-cluster stride is 6.
 *  - A core's wake needs CPM_PWRDN_CTL (S3_5_C15_C6_2, the core's SYS_APL_DEEP_SLEEP) bits 0
 *    and 63 cleared again.
 *  - Reading EDPRCR hangs the SoC. This profile never touches the external debug block.
 *  - The SMC's AP idle notice must be AP-only SET_AP_PWR_STATE 0x201. An IOP 0x201 word makes
 *    the SMC power-cycle the SoC 5-9 s later.
 *  - A sleep that came back after MBSE = slpa had lost the SSD (EIO) until a reboot, read as
 *    the SMC cutting ANS's NAND link. That was the port's SMC and NVMe code. Integration's
 *    macsmc and NVMe resume were never run on T6030, so here it is expected, not measured.
 *  - The GPU's AGX vote must be withdrawn before the PMP goes idle, or the PMP firmware crashes.
 *  - WD2 does not arm. A hang after the commit word ends only with a long power-button press.
 */

#define T6030_PS_CPU(cl, core)		(0x30 * (cl) + 8 * (core))	/* ECPU0..5, PCPU0..5 */
#define T6030_PS_GPIO			0x120
#define T6030_PS_AP_TMM			0x1a8
#define T6030_PS_SIO_CPU		0x1d0
#define T6030_PS_APCIE_GP		0x500
#define T6030_PS_ANS			0x510
#define T6030_PS_APCIE_SYS_GP		0x558
#define T6030_PS_APCIE_ST		0x568
#define T6030_PS_APCIE_SYS_ST		0x5b0
#define T6030_PS_APCIE_PHY_SW		0x5b8
#define T6030_PS_SEP			0xc00

/* "cluster" (0x350780000): the E and P complex words, T8140's layout */
#define T6030_CLUSTER_E			0x0
#define T6030_CLUSTER_P			0x4
#define T6030_CLUSTER_NIBBLE		GENMASK(3, 0)

/* "pwrgate" (0x350724000): the wake pair and the CPM pair */
#define T6030_PWRGATE_WAKE0		0x0
#define T6030_PWRGATE_WAKE1		0x60
#define T6030_PWRGATE_CPM0		0xc0
#define T6030_PWRGATE_CPM1		0xc8

/* The PMU (ADT "chapel" on nub-spmi0) */
#define T6030_PMU_USID			0xb

/* nub-spmi0: IRQ enable word 8 (+0x40), T6040's and T6050's sleep value */
#define T6030_SPMI_NOTIFY_SLEEP		0x2000001

/*
 * macOS's early wake (_earlyWakeHacks): the SEP's PS word, DEV_DISABLE (bit 10) cleared, then
 * bit 11 (sleep.c's PS_BUSY) polled to 0. macOS has no timeout. The ADT's pmgr sep-ps-timeout
 * is 5 (ms here).
 */
#define T6030_SEP_WAIT_US		5000

/* The macOS valid mask of the wake block's last word (kAPWakeupSourcesValidMasks) */
#define T6030_WAKE_LAST_WORD		0x01ffffff

/*
 * The last WFI's controls. CYC_OVRD is writable on M1 to M3 (cpuidle-apple's list). Without
 * WFI mode 3 a core keeps cpuidle's resting mode 2 and never power-gates in its last WFI.
 */
#define SYS_APL_CYC_OVRD_T6030		sys_reg(3, 5, 15, 5, 0)
#define T6030_CYC_OVRD_WFI_MODE		GENMASK_ULL(25, 24)
#define T6030_CYC_OVRD_DISABLE_WFI_RET	BIT_ULL(0)

struct t6030_sleep {
	struct device *dev;
	/*
	 * nub-spmi0 is kept here, not in apple_sleep.spmi: the core's PMU notes for an untested
	 * SoC go to PMU 0x8308.., which is not known to be free scratch on T6030's PMU.
	 */
	void __iomem *spmi;
	void __iomem *aon_deadline;
	/* What the commit found and the wake read back, for the report */
	u32 cluster_saved[2];
	u32 cluster_woke[2];
	u32 pwrgate_woke[4];
	u32 sec_left;
	bool returned;
	bool dry_ran;
	u32 dry_words[8];
	/* WD2 as the wake found it: CTL, count, alarm */
	u32 wd2_found[3];
	/* The SEP word before, after the bit 11 poll, and the poll's result */
	u32 sep_found, sep_after;
	int sep_ret;
};

static struct t6030_sleep *t6030_sleep;

/* Each core's CYC_OVRD before its last WFI, put back when it starts again */
static DEFINE_PER_CPU(u64, t6030_cyc_saved);
static DEFINE_PER_CPU(bool, t6030_cyc_valid);

static bool t6030_sleep_dry(const struct apple_sleep *c)
{
	return c->soc->untested && !READ_ONCE(sleep_untested_commit);
}

static void t6030_iounmap(void *p)
{
	iounmap((void __iomem *)p);
}

/* A named block of the sleep node, mapped non-posted and shared with its owner: no claim. */
static void __iomem *t6030_sleep_map(struct device *dev, const char *name, size_t min)
{
	struct device_node *np = dev->of_node;
	struct resource res;
	void __iomem *p;
	int index;

	index = of_property_match_string(np, "reg-names", name);
	if (index < 0 || of_address_to_resource(np, index, &res) || resource_size(&res) < min)
		return NULL;
	p = ioremap_np(res.start, resource_size(&res));
	if (!p || devm_add_action_or_reset(dev, t6030_iounmap, (void __force *)p))
		return NULL;
	return p;
}

/*
 * The common map's blocks that T6030 has, and the AON AP deadline. ps_res and driver_down stay
 * unset, so apple_sleep_domain_set() answers -EINVAL and no driver lowers a "ps" word through
 * this driver (as the out-of-tree port ran).
 */
static int t6030_sleep_map_regs(struct apple_sleep *c)
{
	struct device *dev = c->dev;
	struct t6030_sleep *t;

	t = devm_kzalloc(dev, sizeof(*t), GFP_KERNEL);
	if (!t)
		return -ENOMEM;
	t->dev = dev;
	c->ps = t6030_sleep_map(dev, "ps", 0x1000);
	c->pwrgate = t6030_sleep_map(dev, "pwrgate", T6030_PWRGATE_CPM1 + 4);
	c->cpu_start = t6030_sleep_map(dev, "cpu-start", CPU_START_CLUSTER(1) + 4);
	c->cluster = t6030_sleep_map(dev, "cluster", T6030_CLUSTER_P + 4);
	c->commit = t6030_sleep_map(dev, "commit", 4);
	c->wdt = t6030_sleep_map(dev, "wdt", WDT_WD2_CTL + 4);
	c->wake = t6030_sleep_map(dev, "wake", 0x24 + 7 * 4);
	t->spmi = t6030_sleep_map(dev, "spmi", SPMI_NOTIFY_ACK + 4);
	if (!c->ps || !c->pwrgate || !c->cpu_start || !c->cluster || !c->commit || !c->wdt ||
	    !c->wake || !t->spmi)
		return dev_err_probe(dev, -ENODEV, "a register block is missing or not mapped\n");
	/* Optional: without it the deadline is left as it is. */
	t->aon_deadline = t6030_sleep_map(dev, "aon-deadline", 4);
	t6030_sleep = t;

	/* The common wake words (own_wake is off): apple,wake-enable from the ADT. */
	return apple_sleep_read_wake_sources(c);
}

/*
 * .begin, before any device suspends. With no smc_policy, macsmc sends MBSE = slpw at its
 * .prepare and slpa with the AP idle notice at its noirq suspend, before this driver's .enter
 * can refuse. On the out-of-tree port, a sleep that came back after slpa had lost the SSD, and
 * the same is expected here. A dry run would pay that for nothing, so it is refused here,
 * before pm_suspend_via_firmware() is set and before the SMC hears anything.
 *
 * TODO(t6030): move slpa behind the dry-run point (a T6030 smc_policy, or a macsmc hook), then
 * let the dry run reach final_check.
 */
static int t6030_sleep_begin(struct apple_sleep *c)
{
	struct t6030_sleep *t = t6030_sleep;

	if (t6030_sleep_dry(c)) {
		dev_warn(c->dev, "dry run refused at .begin: the SMC's slpa cost the SSD on the out-of-tree port (apple_sleep.untested_commit=1 goes on)\n");
		return -EBUSY;
	}
	t->returned = false;
	t->dry_ran = false;
	t->sec_left = 0;
	return 0;
}

/*
 * TODO(t6030): the display pipes. The out-of-tree port lowered them from its provider before
 * this op. ndisp is 0, so nothing in the core waits on a DCP either.
 */
static int t6030_sleep_display_off(struct apple_sleep *c)
{
	return 0;
}

/*
 * The last returning check, after the refusals, the held cores and the coprocessor checks. The
 * early_context entry has no dry run of its own (only apple_sleep_enter()'s, which it skips), so
 * this is T6030's: it reads the words the commit would write and stops. .begin refuses a dry
 * run already, so this is reached only if apple_sleep.untested_commit was cleared since.
 *
 * TODO(t6030): the memory controller's cache off and error enables (AMCC/DCS) while a refusal
 * can still undo them, staged here with undo_regs to put them back.
 */
static int t6030_sleep_final_check(struct apple_sleep *c)
{
	struct t6030_sleep *t = t6030_sleep;
	int i;

	if (!t6030_sleep_dry(c))
		return 0;
	t->dry_words[0] = readl(c->cluster + T6030_CLUSTER_E);
	t->dry_words[1] = readl(c->cluster + T6030_CLUSTER_P);
	t->dry_words[2] = readl(c->pwrgate + T6030_PWRGATE_WAKE0);
	t->dry_words[3] = readl(c->pwrgate + T6030_PWRGATE_WAKE1);
	t->dry_words[4] = readl(c->pwrgate + T6030_PWRGATE_CPM0);
	t->dry_words[5] = readl(c->pwrgate + T6030_PWRGATE_CPM1);
	t->dry_words[6] = readl(c->commit);
	t->dry_words[7] = 0;
	for (i = 0; i < c->soc->wake_words; i++)
		t->dry_words[7] |= readl(c->wake + c->soc->wake_status + 4 * i) ? BIT(i) : 0;
	t->dry_ran = true;
	return -EAGAIN;
}

/* Reversible, after every refusal: nothing of T6030's own (the core arms the wake masks). */
static void t6030_sleep_prepare_regs(struct apple_sleep *c)
{
}

static void t6030_sleep_undo_regs(struct apple_sleep *c)
{
}

static int t6030_sleep_cpu_prepare(u64 mpidr)
{
	return 0;
}

/*
 * On the core itself as it starts (the spin table's postboot and hold paths) and on the boot
 * core after the entry: CPM_PWRDN_CTL's deep bits cleared, without which the next wake fails,
 * and CYC_OVRD as it was before this core's last WFI, if it took one.
 */
static void t6030_sleep_cpu_restore(u64 mpidr)
{
	sysreg_clear_set_s(SYS_APL_DEEP_SLEEP, DEEP_SLEEP_ENABLE, 0);
	if (this_cpu_read(t6030_cyc_valid)) {
		write_sysreg_s(this_cpu_read(t6030_cyc_saved), SYS_APL_CYC_OVRD_T6030);
		this_cpu_write(t6030_cyc_valid, false);
	}
	isb();
}

/*
 * Every core's last WFI, the timers masked by the core: S3_5_C15_C6_2 bits 0 and 63 as on
 * T8140, and CYC_OVRD WFI mode 3 with retention off. T6040 also releases each core's debug
 * power request through EDPRCR. Reading EDPRCR hangs T6030, so nothing here touches the
 * external debug block.
 */
static void t6030_deep_wfi_arm(void)
{
	u64 cyc = read_sysreg_s(SYS_APL_CYC_OVRD_T6030);

	this_cpu_write(t6030_cyc_saved, cyc);
	this_cpu_write(t6030_cyc_valid, true);
	sysreg_clear_set_s(SYS_APL_DEEP_SLEEP, 0, DEEP_SLEEP_ENABLE);
	write_sysreg_s((cyc & ~(T6030_CYC_OVRD_WFI_MODE | T6030_CYC_OVRD_DISABLE_WFI_RET)) |
		       FIELD_PREP(T6030_CYC_OVRD_WFI_MODE, 3) | T6030_CYC_OVRD_DISABLE_WFI_RET,
		       SYS_APL_CYC_OVRD_T6030);
	isb();
}

/* The secondaries' power words, ps + 8 * (6 * cluster + core), read to ACTUAL 0 (T6040: 10 ms each) */
static void t6030_secondaries_off(struct apple_sleep *c, struct t6030_sleep *t)
{
	u64 self = read_cpuid_mpidr() & MPIDR_HWID_BITMASK;
	unsigned int cpu;
	u32 v;

	for_each_present_cpu(cpu) {
		u64 mpidr = cpu_logical_map(cpu);
		unsigned int cl = MPIDR_AFFINITY_LEVEL(mpidr, 1), core = MPIDR_AFFINITY_LEVEL(mpidr, 0);

		if (mpidr == self || cl > 1 || core > 5)
			continue;
		if (readl_poll_timeout_atomic(c->ps + T6030_PS_CPU(cl, core), v,
					      !FIELD_GET(PS_ACTUAL, v), 1, 10000))
			t->sec_left |= BIT(6 * cl + core);
	}
}

/*
 * The irreversible steps from the stopped cores to the mailbox, in T6040's order where T6030
 * has the block. The core then arms the mailbox, writes the commit word, masks the AIC and takes
 * the boot core's deep WFI.
 *
 * On J516S this sequence, with the provider's device side before it, wrote the commit word with
 * every secondary off and then hung: no wake, and no WD2 reset. Expect the same here.
 *
 * TODO(t6030): lower SIO's word (0x1b8) and the PMP's and PMS SRAM's (0x4c8, 0x4d0) once their
 * coprocessors have stopped. On the port, lowering SIO's did not change the hang. A diff of the
 * power words against macOS at its commit (as #2075 found VENC0_SYS left on for T6050) is the
 * open lead.
 *
 * Not carried, as the port's runs had them off: T6040's PMU boot stage 0x50 and nub-spmi-a0
 * notify enable, and T8140's SoC perf state, clock-mux parking and CLLT clear. Each would come
 * back only if a macOS commit trace shows T6030 needs it.
 */
static void t6030_sleep_commit_regs(struct apple_sleep *c)
{
	struct t6030_sleep *t = t6030_sleep;

	t8132_clean_ram();

	/* T6040: every other core in its last WFI before the complex words, polled off */
	spin_table_release_stopped();
	t6030_secondaries_off(c, t);

	/* T6040: WD0 off; T6050 also zeroes the counts. Then the AON AP deadline. */
	writel(0, c->wdt + WDT_WD0_CTL);
	writel(0, c->wdt + WDT_WD0_COUNT);
	writel(0, c->wdt + WDT_WD1_COUNT);
	if (t->aon_deadline)
		writel(0, t->aon_deadline);

	/* T6040: nub-spmi0's NOTIFY_ENABLE at its sleep value. No SPMI traffic after this. */
	writel(T6030_SPMI_NOTIFY_SLEEP, t->spmi + SPMI_NOTIFY_ENABLE);

	/* T6040: each complex word's low nibble to 0, E then P */
	t->cluster_saved[0] = readl(c->cluster + T6030_CLUSTER_E);
	t->cluster_saved[1] = readl(c->cluster + T6030_CLUSTER_P);
	writel(t->cluster_saved[0] & ~T6030_CLUSTER_NIBBLE, c->cluster + T6030_CLUSTER_E);
	writel(t->cluster_saved[1] & ~T6030_CLUSTER_NIBBLE, c->cluster + T6030_CLUSTER_P);
}

/* T8132's: what the commit dirtied since the clean, this stack and the driver's state */
static void t6030_sleep_pre_commit(struct apple_sleep *c)
{
	dcache_clean_poc((unsigned long)task_stack_page(current),
			 (unsigned long)task_stack_page(current) + THREAD_SIZE);
	dcache_clean_poc((unsigned long)c, (unsigned long)(c + 1));
	dcache_clean_poc((unsigned long)t6030_sleep, (unsigned long)(t6030_sleep + 1));
}

/* Back on the boot core through the reset vector, the AIC restored: T6040's wake order */
static void t6030_sleep_wake(struct apple_sleep *c)
{
	struct t6030_sleep *t = t6030_sleep;
	u32 v;

	/* This core's deep bits before anything can idle it (the core's cpu_restore comes later) */
	t6030_sleep_cpu_restore(0);

	/* WD2 does not arm on T6030 (measured): recorded for the report, and set as .begin did */
	t->wd2_found[0] = readl(c->wdt + WDT_WD2_CTL);
	t->wd2_found[1] = readl(c->wdt + WDT_WD2_COUNT);
	t->wd2_found[2] = readl(c->wdt + WDT_WD2_ALARM);

	v = readl(c->cluster + T6030_CLUSTER_E);
	writel((v & ~T6030_CLUSTER_NIBBLE) | CLUSTER_ON, c->cluster + T6030_CLUSTER_E);
	v = readl(c->cluster + T6030_CLUSTER_P);
	writel((v & ~T6030_CLUSTER_NIBBLE) | CLUSTER_ON, c->cluster + T6030_CLUSTER_P);
	dsb(sy);
	t->cluster_woke[0] = readl(c->cluster + T6030_CLUSTER_E);
	t->cluster_woke[1] = readl(c->cluster + T6030_CLUSTER_P);

	writel(readl(c->pwrgate + T6030_PWRGATE_WAKE0) | PWRGATE_ON, c->pwrgate + T6030_PWRGATE_WAKE0);
	writel(readl(c->pwrgate + T6030_PWRGATE_WAKE1) | PWRGATE_ON, c->pwrgate + T6030_PWRGATE_WAKE1);
	writel(readl(c->pwrgate + T6030_PWRGATE_CPM0) | PWRGATE_ON, c->pwrgate + T6030_PWRGATE_CPM0);
	writel(readl(c->pwrgate + T6030_PWRGATE_CPM1) | PWRGATE_ON, c->pwrgate + T6030_PWRGATE_CPM1);
	dsb(sy);
	t->pwrgate_woke[0] = readl(c->pwrgate + T6030_PWRGATE_WAKE0);
	t->pwrgate_woke[1] = readl(c->pwrgate + T6030_PWRGATE_WAKE1);
	t->pwrgate_woke[2] = readl(c->pwrgate + T6030_PWRGATE_CPM0);
	t->pwrgate_woke[3] = readl(c->pwrgate + T6030_PWRGATE_CPM1);

	/* macOS's early wake, after the complexes and the wake power gates */
	t->sep_found = readl(c->ps + T6030_PS_SEP);
	if (t->sep_found & PS_DEV_DISABLE)
		writel(t->sep_found & ~PS_DEV_DISABLE, c->ps + T6030_PS_SEP);
	t->sep_ret = readl_poll_timeout_atomic(c->ps + T6030_PS_SEP, v, !(v & PS_BUSY), 1,
					       T6030_SEP_WAIT_US);
	t->sep_after = v;

	/* Only C2S turns AP_TMM on (T8140): this wake came down the deep path. */
	c->deep_ran = FIELD_GET(PS_ACTUAL, readl(c->ps + T6030_PS_AP_TMM)) != 0;
	c->pins_lost = c->pins_saved_valid;
	apple_sleep_restore_pins();

	apple_sleep_set_wd2(c, WDT_SUSPEND_SECS);
	t->returned = true;
}

/* .wake (apple_sleep_report()) and .end: what this sleep found, once */
static void t6030_sleep_finish(struct apple_sleep *c)
{
	struct t6030_sleep *t = t6030_sleep;

	if (t->dry_ran) {
		t->dry_ran = false;
		dev_info(c->dev, "dry run: every check passed, refused before the commit (apple_sleep.untested_commit=1 commits); complex E %#x P %#x, pwrgate wake %#x %#x cpm %#x %#x, commit %#x, wake status words raised %#x\n",
			 t->dry_words[0], t->dry_words[1], t->dry_words[2], t->dry_words[3],
			 t->dry_words[4], t->dry_words[5], t->dry_words[6], t->dry_words[7]);
	}
	if (!t->returned)
		return;
	t->returned = false;
	dev_info(c->dev, "complex E %#x P %#x at the commit, %#x %#x after the wake; pwrgate %#x %#x %#x %#x; secondaries not off %#x\n",
		 t->cluster_saved[0], t->cluster_saved[1], t->cluster_woke[0],
		 t->cluster_woke[1], t->pwrgate_woke[0], t->pwrgate_woke[1], t->pwrgate_woke[2],
		 t->pwrgate_woke[3], t->sec_left);
	dev_info(c->dev, "wake: WD2 CTL %#x count %#x alarm %#x; SEP PS %#x -> %#x, bit 11 %s (%d)\n",
		 t->wd2_found[0], t->wd2_found[1], t->wd2_found[2], t->sep_found, t->sep_after,
		 t->sep_ret ? "still set after 5 ms" : "clear", t->sep_ret);
}

/* E: 0.0.0-5, P: 1.1.0-5 (t6030.dtsi cpu reg) */
static bool t6030_sleep_cpu_valid(u64 mpidr)
{
	unsigned int cluster = MPIDR_AFFINITY_LEVEL(mpidr, 1), core = MPIDR_AFFINITY_LEVEL(mpidr, 0);

	return cluster < 2 && core < 6 && MPIDR_AFFINITY_LEVEL(mpidr, 2) == cluster;
}

/*
 * No commit_notice: with no smc_policy, macsmc's noirq suspend sends MBSE = slpa and then
 * apple_rtkit_ap_power(false), which is SET_AP_PWR_STATE 0x201 for the AP alone, with no IOP
 * word. That is the notice T6030 needs (an IOP 0x201 power-cycles the SoC 5-9 s later), so a
 * T6030 notice must stay AP-only if it ever moves here.
 *
 * TODO(t6030), each before a commit can be expected to wake:
 *  - ANS: the T6030 NVMe ("apple,t6030-nvme-ans3") matches t8103's data, which registers no
 *    sleep stop, so every sleep is refused at "ANS or PCIe is on with no driver to stop it".
 *    It also needs the apple,apcie tunables for ANS's PCIe block.
 *  - GPU: G15 holds its PMP vote for the whole session. It needs a first-pass stop
 *    (apple_sleep_coproc_register_first()) that withdraws it before pmp-export's stop idles
 *    the PMP, or the PMP firmware crashes.
 *  - SEP: no sleep request was ever sent on T6030. The out-of-tree port left it running.
 *  - A watchdog net: WD2 does not arm.
 */
static const struct apple_sleep_ops t6030_sleep_ops = {
	.deep_wfi_arm = t6030_deep_wfi_arm,
	.resume = cpu_resume,
	.aic_mask_all = aic_sleep_mask_all,
	.aic_ack = aic_sleep_ack,
	.aic_restore = aic_sleep_restore,
	.map_regs = t6030_sleep_map_regs,
	.begin = t6030_sleep_begin,
	.display_off = t6030_sleep_display_off,
	.final_check = t6030_sleep_final_check,
	.prepare_regs = t6030_sleep_prepare_regs,
	.undo_regs = t6030_sleep_undo_regs,
	.commit_regs = t6030_sleep_commit_regs,
	.pre_commit = t6030_sleep_pre_commit,
	.wake = t6030_sleep_wake,
	.finish = t6030_sleep_finish,
	.cpu_prepare = t6030_sleep_cpu_prepare,
	.cpu_restore = t6030_sleep_cpu_restore,
};

static const struct apple_sleep_soc t6030_sleep_soc = {
	.ps = {
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
	.early_context = true,
	.untested = true,
	.ops = &t6030_sleep_ops,
	/* No machine: m1n1 may drop absent cores, and the binned J516S has 11 */
	.boot_mpidr = 0x10100,
	.cpu_cluster_affinity = 1,
	.cpu_clusters = 2,
	.cpu_cores_per_cluster = 6,
	/* Measured (#2687): core n of cluster m is CPU-start bit 6 * m + n. */
	.cpu_start_stride = 6,
	.cpu_valid = t6030_sleep_cpu_valid,
	.pmu_usid = T6030_PMU_USID,
	.wake_mask = 0x8,
	.wake_status = 0x24,
	.wake_words = 7,
	.wake_last_word = T6030_WAKE_LAST_WORD,
	.pwrgate_wake = { T6030_PWRGATE_WAKE0, T6030_PWRGATE_WAKE1 },
	.pwrgate_cpm = { T6030_PWRGATE_CPM0, T6030_PWRGATE_CPM1 },
	.cluster_cpu = { T6030_CLUSTER_E, T6030_CLUSTER_P },
	.ncluster_cpu = 2,
};
