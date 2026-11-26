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
 * aura's init_cpu() sets HID4 (P) or EHID4 (E) DISABLE_DC_MVA and DISABLE_DC_SW_L2 on every
 * M3 core, as XNU's start does. With them set the by-VA cleans and the L2 set/way ones do
 * nothing: dcache_clean_poc() is a no-op here, so a deep sleep that drops the caches lost
 * whatever the clean was for, aura's resume_pa among it. XNU lifts both around its sleep's
 * clean, and so does this (T8122's, aurora-silicon/linux#3140), on the core that cleans.
 */
#define T6030_HID4			sys_reg(3, 0, 15, 4, 0)
#define T6030_EHID4			sys_reg(3, 0, 15, 4, 1)
#define T6030_HID4_DISABLE_DC_MVA	BIT_ULL(11)
#define T6030_HID4_DISABLE_DC_SW_L2	BIT_ULL(44)
#define T6030_HID4_DC_MASK		(T6030_HID4_DISABLE_DC_MVA | T6030_HID4_DISABLE_DC_SW_L2)
/* Aff2: the P cores are 0x101xx */
#define T6030_MPIDR_PCORE		BIT_ULL(16)

/* This core's cache maintenance unblocked; returns its HID4 or EHID4 as it was */
static __always_inline u64 t6030_dc_unblock(void)
{
	u64 old;

	if (read_cpuid_mpidr() & T6030_MPIDR_PCORE) {
		old = read_sysreg_s(T6030_HID4);
		write_sysreg_s(old & ~T6030_HID4_DC_MASK, T6030_HID4);
	} else {
		old = read_sysreg_s(T6030_EHID4);
		write_sysreg_s(old & ~T6030_HID4_DC_MASK, T6030_EHID4);
	}
	isb();
	return old;
}

static __always_inline void t6030_dc_reblock(u64 old)
{
	dsb(sy);
	if (read_cpuid_mpidr() & T6030_MPIDR_PCORE)
		write_sysreg_s(old, T6030_HID4);
	else
		write_sysreg_s(old, T6030_EHID4);
	isb();
}

/* [start, end) cleaned past L2 by VA: dc cvadp (FEAT_DPB2), dc cvap, or dc civac. Unblocked. */
static void t6030_clean_range(unsigned long start, unsigned long end)
{
	unsigned long line = 4UL << ((read_cpuid_cachetype() >> CTR_EL0_DminLine_SHIFT) & 0xf);
	unsigned int dpb = cpuid_feature_extract_unsigned_field(read_sysreg(id_aa64isar1_el1),
								 ID_AA64ISAR1_EL1_DPB_SHIFT);
	unsigned long addr = start & ~(line - 1);

	if (dpb >= 2) {
		for (; addr < end; addr += line)
			asm volatile("sys #3, c7, c13, #1, %0" : : "r"(addr) : "memory");
	} else if (dpb >= 1) {
		for (; addr < end; addr += line)
			asm volatile("sys #3, c7, c12, #1, %0" : : "r"(addr) : "memory");
	} else {
		for (; addr < end; addr += line)
			asm volatile("dc civac, %0" : : "r"(addr) : "memory");
	}
	dsb(sy);
}

/*
 * Every data or unified cache level this core sees (L1 and its cluster's L2), cleaned by set
 * and way, in registers only: no store between the first dc csw and the last dsb. Unblocked.
 */
static __always_inline void t6030_clean_all_levels(void)
{
	u64 clidr, ccidx, lvl, ctype, ccsidr, lshift, way, mset, wshift, set, tmp;

	asm volatile(
		"dsb sy\n\t"
		"mrs %[clidr], clidr_el1\n\t"
		"mrs %[ccidx], id_aa64mmfr2_el1\n\t"
		"ubfx %[ccidx], %[ccidx], #20, #4\n\t"
		"mov %[lvl], #0\n\t"
		"1:\n\t"
		"cmp %[lvl], #7\n\t"
		"b.hs 5f\n\t"
		"add %[tmp], %[lvl], %[lvl], lsl #1\n\t"
		"lsr %[ctype], %[clidr], %[tmp]\n\t"
		"and %[ctype], %[ctype], #7\n\t"
		"cbz %[ctype], 5f\n\t"
		"cmp %[ctype], #2\n\t"
		"b.lo 4f\n\t"
		"cmp %[ctype], #4\n\t"
		"b.hi 4f\n\t"
		"lsl %[tmp], %[lvl], #1\n\t"
		"msr csselr_el1, %[tmp]\n\t"
		"isb\n\t"
		"mrs %[ccsidr], ccsidr_el1\n\t"
		"and %[lshift], %[ccsidr], #7\n\t"
		"add %[lshift], %[lshift], #4\n\t"
		"cbnz %[ccidx], 20f\n\t"
		"ubfx %[way], %[ccsidr], #3, #10\n\t"
		"ubfx %[mset], %[ccsidr], #13, #15\n\t"
		"b 21f\n\t"
		"20:\n\t"
		"ubfx %[way], %[ccsidr], #3, #21\n\t"
		"ubfx %[mset], %[ccsidr], #32, #24\n\t"
		"21:\n\t"
		"clz %w[wshift], %w[way]\n\t"
		"2:\n\t"
		"mov %[set], %[mset]\n\t"
		"lsl %[ctype], %[way], %[wshift]\n\t"
		"orr %[ctype], %[ctype], %[lvl], lsl #1\n\t"
		"3:\n\t"
		"lsl %[tmp], %[set], %[lshift]\n\t"
		"orr %[tmp], %[ctype], %[tmp]\n\t"
		"dc csw, %[tmp]\n\t"
		"subs %[set], %[set], #1\n\t"
		"b.ge 3b\n\t"
		"subs %[way], %[way], #1\n\t"
		"b.ge 2b\n\t"
		"dsb sy\n\t"
		"4:\n\t"
		"add %[lvl], %[lvl], #1\n\t"
		"b 1b\n\t"
		"5:\n\t"
		"msr csselr_el1, xzr\n\t"
		"dsb sy\n\t"
		"isb\n\t"
		: [clidr] "=&r"(clidr), [ccidx] "=&r"(ccidx), [lvl] "=&r"(lvl),
		  [ctype] "=&r"(ctype), [ccsidr] "=&r"(ccsidr), [lshift] "=&r"(lshift),
		  [way] "=&r"(way), [mset] "=&r"(mset), [wshift] "=&r"(wshift),
		  [set] "=&r"(set), [tmp] "=&r"(tmp)
		:
		: "cc", "memory");
}

/*
 * Lowered in this order, and raised again in reverse at the wake: SIO's CPU by .after_early,
 * the PMP and its SRAM by .commit_notice, the storage PHY by .commit_regs.
 */
static const u32 t6030_lowered[] = {
	T6030_PS_SIO_CPU, T6030_PS_PMP, T6030_PS_PMS_SRAM, T6030_PS_APCIE_PHY_SW,
};

enum { T6030_LOWER_SIO_CPU, T6030_LOWER_PMP, T6030_LOWER_PMS_SRAM, T6030_LOWER_PHY };

/*
 * On under Linux at the commit, off at macOS's (PLAN §4, runs/t1c-stop/linux-diff.tsv): every
 * USB-C port's PHY common, then debug port's fabric (on under the HV only because aura holds
 * it), then the words macOS leaves to their auto state (0x200; the ADT calls them critical).
 * T8122 (#2746) found its SoC powers down only with the port words off. Lowered by
 * .commit_regs, each back as it was at the wake.
 */
static const u32 t6030_macos_down[] = {
	0x4e0, 0x4e8, 0x4f0, 0x4f8,		/* ATC0-3_COMMON */
	0x4d8, 0x4c0, 0x4b8, 0x4b0,		/* LW31, LW30, LS3, IOA3 */
	0x108, 0x2e8, 0x2f0, 0x2f8, 0x300,	/* MSG, DPA1-4 */
	0x1e0, 0x1e8, 0x138, 0x140, 0x148, 0x150, 0x158, /* FPWM1/2, PMS_FPWM0-4 */
	0x190, 0x218, 0x178,			/* AFT0, I2C5, PMGR_SOC_OCLA */
};

/*
 * The other way: off under Linux, on at macOS's commit (PLAN §4, linux-diff.tsv), the shared
 * core's ps_macos_on pattern: GPIO, I2C0/4/7/8, SPI0 and UART0-6. Raised by .commit_regs,
 * AUTO_ENABLE clear, each left as it was before the entry at the wake.
 */
static const u32 t6030_macos_up[] = {
	0x120, 0x1f0, 0x210, 0x228, 0x230, 0x260,
	0x2a0, 0x2a8, 0x2b0, 0x2b8, 0x2c0, 0x2c8, 0x2d0,
};

/* And in "ps-aon": NUB_SPI0 and NUB_OCLA (0x200 at macOS's commit) */
static const u32 t6030_macos_down_aon[] = { 0x88, 0x90 };

/* SIO's and its CPU's words: apple,seq-commit clears their AUTO_ENABLE, the wake puts them back */
static const u32 t6030_auto_off[] = { T6030_PS_SIO, T6030_PS_SIO_CPU };

/* Per probe, in s->soc_state */
struct t6030_sleep {
	void __iomem *blk[T6030_BLOCKS];
	u32 blk_size[T6030_BLOCKS];
	struct sleep_seq prepare, notice, commit, wake_early, wake;
	u32 notify_saved[2];
	bool notify_staged;
	u32 wake_saved[7];
	u32 wake_status[7];
	u32 lowered_saved[ARRAY_SIZE(t6030_lowered)];
	unsigned int nlowered;
	u32 auto_saved[ARRAY_SIZE(t6030_auto_off)];
	bool auto_staged;
	u32 down_saved[ARRAY_SIZE(t6030_macos_down)];
	u32 up_saved[ARRAY_SIZE(t6030_macos_up)];
	u32 down_aon_saved[ARRAY_SIZE(t6030_macos_down_aon)];
	bool down_staged;
	u8 down_stuck;
	/* The GP PCIe host's (apple,pcie-host) PHY and clock blocks, shared with its driver */
	void __iomem *gp_phy, *gp_phy_ip, *gp_clkgen, *gp_axi2af;
	int gp_park_ret;
	/* The PMU's RTC alarm as the entry found it, for the rearm after the commit word */
	u8 alarm_compare[4];
	bool alarm_valid;
	/* The PMP's mailbox and the MTP's DART, for macOS's last steps on them */
	void __iomem *pmp_mbox, *mtp_dart;
	int pmp_idle_ret;
	u64 pmp_ack[2];
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
	/*
	 * An entry's alignment is checked against the block: the block's own must hold too.
	 * Its size bounds every entry's offset, as a u32.
	 */
	if (of_address_to_resource(s->dev->of_node, b, &res) ||
	    !IS_ALIGNED(res.start, sizeof(u64)) || !resource_size(&res) ||
	    resource_size(&res) > U32_MAX)
		return 0;
	/* Shared with the drivers of those blocks: not claimed */
	t->blk[b] = apple_sleep_ioremap(s->dev, &res);
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

/* One of the GP PCIe host's reg entries by name, mapped once (not claimed), or NULL */
static void __iomem *t6030_gp_block(struct apple_sleep *s, struct device_node *host,
				    const char *name)
{
	struct resource res;
	int i = of_property_match_string(host, "reg-names", name);

	if (i < 0 || of_address_to_resource(host, i, &res))
		return NULL;
	return apple_sleep_ioremap(s->dev, &res);
}

/* Without all four the park is left out: the host's driver keeps them as they are. */
static void t6030_gp_map(struct apple_sleep *s)
{
	struct t6030_sleep *t = t6030_state(s);
	struct device_node *host = of_parse_phandle(s->dev->of_node, "apple,pcie-host", 0);

	if (!host)
		return;
	t->gp_phy = t6030_gp_block(s, host, "phy-shared");
	t->gp_phy_ip = t6030_gp_block(s, host, "phy-ip");
	t->gp_clkgen = t6030_gp_block(s, host, "clkgen");
	t->gp_axi2af = t6030_gp_block(s, host, "axi2af");
	of_node_put(host);
	if (!t->gp_phy || !t->gp_phy_ip || !t->gp_clkgen || !t->gp_axi2af) {
		dev_warn(s->dev, "the GP PCIe host's PHY and clock blocks: not all mapped, no park\n");
		t->gp_phy = NULL;
	}
}

/*
 * Read-only: a core's power-down controls as they stand before its last WFI arms them (at probe,
 * on whichever core probes, and at the dry run, on the boot core).
 * macOS 27.2's boot core at its commit word (lane A's HV markers, p20-mac-stop-1/2): ACC_OVRD
 * 0x180010900004020e, CYC_OVRD 0x2000000 (WFI mode 2), S3_5_C15_C6_2 0. XNU ORs the deep-sleep
 * fields into what it finds, so any other bit that differs here differs at the sleep too.
 */
static void t6030_ovrd_report(struct apple_sleep *s, const char *when)
{
	dev_info(s->dev, "%s: cpu %#llx ACC_OVRD %#llx CYC_OVRD %#llx ACC_CFG %#llx\n", when,
		 read_cpuid_mpidr() & MPIDR_HWID_BITMASK, read_sysreg_s(sys_reg(3, 5, 15, 6, 0)),
		 read_sysreg_s(sys_reg(3, 5, 15, 5, 0)), read_sysreg_s(sys_reg(3, 5, 15, 4, 0)));
}

static void t6030_late_map(struct apple_sleep *s);

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

	t6030_gp_map(s);
	t6030_late_map(s);
	t6030_ovrd_report(s, "probe");

	return apple_sleep_read_wake_sources(s);
}

static bool t6030_macos_off_at_commit(u32 offset);

/*
 * Read-only, at a dry run's end (every device suspended): each "ps" word on (offset=value; * =
 * macOS has it off at its commit), and each "ps-aon" word on without AUTO_ENABLE
 */
static void t6030_dry_ps_on(struct apple_sleep *s)
{
	const struct apple_sleep_words *all = &s->soc->ps_all, *aon = &s->soc->ps_aon_all;
	/* Interrupts are off here: no allocation. */
	static char line[512];
	int i, n = 0, on = 0;
	u32 v;

	line[0] = 0;
	for (i = 0; i < all->count; i++) {
		v = readl(s->ps + all->offset[i]);
		if (!FIELD_GET(PS_ACTUAL, v))
			continue;
		on++;
		n += scnprintf(line + n, sizeof(line) - n, " %x=%x%s", all->offset[i], v,
			       t6030_macos_off_at_commit(all->offset[i]) ? "*" : "");
		if (n > 400) {
			dev_info(s->dev, "dry run: ps on (* = off at macOS's commit):%s\n", line);
			n = 0;
			line[0] = 0;
		}
	}
	dev_info(s->dev, "dry run: ps on (* = off at macOS's commit):%s; %d on\n", line, on);
	for (i = 0, n = 0, line[0] = 0; i < aon->count; i++) {
		v = readl(s->ps_aon + aon->offset[i]);
		if (FIELD_GET(PS_ACTUAL, v))
			n += scnprintf(line + n, sizeof(line) - n, " %x=%x", aon->offset[i], v);
	}
	dev_info(s->dev, "dry run: ps-aon on:%s\n", line);
}

static void t6030_sleep_clean_ram(void);
static void t6030_sleep_clean(struct apple_sleep *s);

/*
 * Read-only but for the clean itself: whether this core's HID4 blocks the cleans, and how long
 * the commit's clean takes, against the SMC's AP idle to timer spacing it runs in.
 */
static void t6030_dry_clean(struct apple_sleep *s)
{
	bool pcore = read_cpuid_mpidr() & T6030_MPIDR_PCORE;
	u64 hid4 = pcore ? read_sysreg_s(T6030_HID4) : read_sysreg_s(T6030_EHID4);
	u64 t0 = __arch_counter_get_cntpct(), t1, t2, hz = arch_timer_get_cntfrq();

	t6030_sleep_clean_ram();
	t1 = __arch_counter_get_cntpct();
	t6030_sleep_clean(s);
	t2 = __arch_counter_get_cntpct();
	dev_info(s->dev, "dry run: %sHID4 %#llx (DISABLE_DC_MVA %d, DISABLE_DC_SW_L2 %d); RAM clean %llu us before the AP idle, last clean %llu us in its %u us spacing\n",
		 pcore ? "" : "E", hid4, !!(hid4 & T6030_HID4_DISABLE_DC_MVA),
		 !!(hid4 & T6030_HID4_DISABLE_DC_SW_L2), div64_u64((t1 - t0) * USEC_PER_SEC, hz),
		 div64_u64((t2 - t1) * USEC_PER_SEC, hz), s->soc->smc_policy->idle_to_timer_us);
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
		t6030_dry_ps_on(s);
		t6030_dry_clean(s);
		t6030_ovrd_report(s, "dry run");
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
 * A PMU command on gen-1 nub-spmi0 by hand, as macOS 27.2 sends it after the commit word on
 * J516s (t5): status, the notify mask, the command and its data, the done bit polled and
 * acked, then the reply's header and one data word at a time, each only once the status says
 * the FIFO holds one.
 */
#define T6030_SPMI_RX_EMPTY		BIT(24)
#define T6030_SPMI_DONE			BIT(0)
#define T6030_SPMI_POLL_US		1000

static int t6030_pmu_xfer(struct apple_sleep *s, u32 mask, u8 opcode, u16 address,
			  const u8 *wr, u8 *rd, unsigned int len)
{
	u8 command = opcode | (len - 1), usid = s->soc->pmu_usid;
	unsigned int i, n;
	u32 value, data;
	int ret = 0;

	if (!len || len > 8 || !!wr == !!rd)
		return -EINVAL;
	readl(s->spmi + SPMI_STATUS);
	writel(mask, s->spmi + SPMI_NOTIFY_ENABLE);
	writel((u32)address << 16 | SPMI_CMD_ACTIVE | (u32)usid << 8 | command,
	       s->spmi + SPMI_CMD);
	for (i = 0; wr && i < len; i += 4) {
		data = 0;
		for (n = 0; n < min(4U, len - i); n++)
			data |= (u32)wr[i + n] << (8 * n);
		writel(data, s->spmi + SPMI_CMD);
	}
	if (!readl_poll_timeout_atomic(s->spmi + SPMI_NOTIFY_ACK, value,
				       value & T6030_SPMI_DONE, 1, T6030_SPMI_POLL_US))
		writel(T6030_SPMI_DONE, s->spmi + SPMI_NOTIFY_ACK);
	if (readl_poll_timeout_atomic(s->spmi + SPMI_STATUS, value,
				      !(value & T6030_SPMI_RX_EMPTY), 1, T6030_SPMI_POLL_US))
		return -ETIMEDOUT;
	value = readl(s->spmi + SPMI_REPLY);
	if ((value & 0xff) != command || ((value >> 8) & 0x7f) != usid ||
	    (wr && !(value & SPMI_CMD_ACTIVE)))
		ret = -EIO;
	for (i = 0; !ret && rd && i < len; i += 4) {
		if (readl(s->spmi + SPMI_STATUS) & T6030_SPMI_RX_EMPTY) {
			ret = -EIO;
			break;
		}
		data = readl(s->spmi + SPMI_REPLY);
		for (n = 0; n < min(4U, len - i); n++)
			rd[i + n] = data >> (8 * n);
	}
	/* Nothing left behind for the SPMI driver: the FIFO read empty, at most its 16 entries */
	for (i = 0; i < SPMI_DRAIN_MAX && !(readl(s->spmi + SPMI_STATUS) & T6030_SPMI_RX_EMPTY);
	     i++)
		readl(s->spmi + SPMI_REPLY);
	return ret;
}

/*
 * The RTC alarm the OS armed (rtcwake), read before anything of the entry touches the PMU:
 * the control, the mask, the 4-byte compare and the seconds counter. Only an armed, unmasked
 * alarm still ahead of the counter is rearmed after the commit word.
 */
static void t6030_alarm_snapshot(struct apple_sleep *s, u32 runtime_mask)
{
	struct t6030_sleep *t = t6030_state(s);
	u32 mask = runtime_mask & ~T6030_SPMI_DONE;
	u8 ctl = 0, rmask = 0, ticks[6] = {};
	u32 compare, now;

	t->alarm_valid = false;
	if (t6030_pmu_xfer(s, mask, T6050_SPMI_EXT_READL, T6050_RTC_CTL, NULL, &ctl, 1) ||
	    t6030_pmu_xfer(s, mask, T6050_SPMI_EXT_READL, T6050_RTC_MASK, NULL, &rmask, 1) ||
	    t6030_pmu_xfer(s, mask, T6050_SPMI_EXT_READL, T6050_RTC_COMPARE, NULL,
			   t->alarm_compare, 4) ||
	    t6030_pmu_xfer(s, mask, T6050_SPMI_EXT_READL, T6050_RTC_COUNTER, NULL, ticks, 6))
		goto out;
	compare = get_unaligned_le32(t->alarm_compare);
	/* The counter's 48 bits are 1/65536 s: its top 32 are the compare's seconds */
	now = get_unaligned_le32(ticks + 2);
	t->alarm_valid = (ctl & T6050_RTC_CTL_ARMED) && !(rmask & BIT(0)) &&
			 (s32)(compare - now) > 0;
out:
	writel(runtime_mask, s->spmi + SPMI_NOTIFY_ENABLE);
}

/* PMU scratch +3: bit 5, the GPU's GFX word (block 13, "ps-gpu"), on */
#define T6030_BLK_PS_GPU		13
#define T6030_PS_GPU_GFX		0x110

/*
 * +@at the domains on that macOS has off at its commit (bit 0 APCIE_GP, 1 APCIE_SYS_GP, 2 ANS,
 * 3 DISPEXT0_CPU, 4 DISPEXT1_CPU, 5 GFX, 6 DISP_CPU, 7 SEP), +@at+1 how many "ps" words are on
 */
static void t6030_domains_note(struct apple_sleep *s, u8 at)
{
	static const u32 words[] = {
		T6030_PS_APCIE_GP, T6030_PS_APCIE_SYS_GP, T6030_PS_ANS, T6030_PS_DISPEXT0_CPU,
		T6030_PS_DISPEXT1_CPU, 0, T6030_PS_DISP_CPU, T6030_PS_SEP,
	};
	struct t6030_sleep *t = t6030_state(s);
	const struct apple_sleep_words *all = &s->soc->ps_all;
	unsigned int i, on = 0;
	u8 bits = 0;

	for (i = 0; i < ARRAY_SIZE(words); i++)
		if (words[i] && sleep_ps_on(s, words[i]))
			bits |= BIT(i);
	if (t->blk[T6030_BLK_PS_GPU] &&
	    FIELD_GET(PS_ACTUAL, readl(t->blk[T6030_BLK_PS_GPU] + T6030_PS_GPU_GFX)))
		bits |= BIT(5);
	for (i = 0; i < all->count; i++)
		on += sleep_ps_on(s, all->offset[i]);
	sleep_note_at(s, at, bits);
	sleep_note_at(s, at + 1, min(on, 0xffU));
}

/* The sleep's CLLT word: macOS writes it back before each commit word (0x1c17 on J516s) */
#define T6030_CLLT_SLEEP		0x24

/* PMU scratch +3, +4 after the commit word: the RTC control the rearm read, and how it went */
#define T6030_NOTE_POST_NO_ALARM	0xd0
#define T6030_NOTE_POST_FAILED		0xe0

/*
 * After the commit word, every line masked, as macOS 27.2 on J516s (t5): the PMU's RTC alarm
 * rearmed (its control reads 0x01 there, the alarm no longer armed: the entry's PMU and SMC
 * steps disarm it), each command under the sleep's notify mask without its done bit, then the
 * CLLT word and the commit word once more.
 */
static void t6030_sleep_post_timer(struct apple_sleep *s)
{
	struct t6030_sleep *t = t6030_state(s);
	u32 mask = s->soc->spmi_notify_sleep & ~T6030_SPMI_DONE;
	u8 ctl = 0, v, step = 0;
	int ret;

	if (!t->alarm_valid) {
		sleep_note_at(s, 4, T6030_NOTE_POST_NO_ALARM);
		goto recommit;
	}
	ret = t6030_pmu_xfer(s, mask, T6050_SPMI_EXT_READL, T6050_RTC_CTL, NULL, &ctl, 1);
	if (!ret && (ctl & T6050_RTC_CTL_FIRED)) {
		step = 1;
		v = 0x43;
		ret = t6030_pmu_xfer(s, mask, T6050_SPMI_EXT_WRITEL, T6050_RTC_CTL, &v, NULL, 1);
	}
	if (!ret) {
		step = 2;
		ret = t6030_pmu_xfer(s, mask, T6050_SPMI_EXT_WRITEL, T6050_RTC_COMPARE,
				     t->alarm_compare, NULL, 4);
	}
	if (!ret) {
		step = 3;
		v = 0;
		ret = t6030_pmu_xfer(s, mask, T6050_SPMI_EXT_WRITEL, T6050_RTC_MASK, &v, NULL, 1);
	}
	if (!ret) {
		step = 4;
		v = 0x41;
		ret = t6030_pmu_xfer(s, mask, T6050_SPMI_EXT_WRITEL, T6050_RTC_CTL, &v, NULL, 1);
	}
	/* +3/+4 keep the commit's digest unless the rearm failed */
	if (ret) {
		sleep_note_at(s, 3, ctl);
		sleep_note_at(s, 4, T6030_NOTE_POST_FAILED | step);
	}
	/* The notes put the done bit back in the mask: macOS's is without it from here on */
	writel(mask, s->spmi + SPMI_NOTIFY_ENABLE);
recommit:
	writel(readl(s->cllt + T6030_CLLT_SLEEP), s->cllt + T6030_CLLT_SLEEP);
	writel(1, s->commit);
}

/* The boot core's WFI came back: the CLLT word and the commit word again, as macOS's loop */
static void t6030_sleep_wfi_again(struct apple_sleep *s)
{
	writel(readl(s->cllt + T6030_CLLT_SLEEP), s->cllt + T6030_CLLT_SLEEP);
	writel(1, s->commit);
}

/*
 * The wake block (nub-ap-ctrl) as macOS 27.2 on J516s has it: a mask word per 32 sources from
 * +0x08 (1 masks), the sticky status words from +0x24, seven of each (the ADT's sources end at
 * 214). macOS writes the masks once at boot, all set but its ap-wake-sources (its first
 * access: +0x08 = 0xfffffff7, source 3 alone), and reads the status words at its wake: +0x34
 * bit 31 there, source 159, NUB.SPMI0.SW3.IRQ, the PMU's RTC alarm. iBoot leaves every word 0.
 */
#define T6030_WAKE_MASK			0x08
#define T6030_WAKE_STATUS		0x24
#define T6030_WAKE_WORDS		7

static void t6030_wake_masks_set(struct apple_sleep *s)
{
	struct t6030_sleep *t = t6030_state(s);
	u32 val[T6030_WAKE_WORDS];
	unsigned int i;

	for (i = 0; i < T6030_WAKE_WORDS; i++) {
		t->wake_saved[i] = readl(s->wake + T6030_WAKE_MASK + 4 * i);
		val[i] = U32_MAX;
	}
	for (i = 0; i < s->wake_enable_count; i++)
		if (s->wake_enable[i] / 32 < T6030_WAKE_WORDS)
			val[s->wake_enable[i] / 32] &= ~BIT(s->wake_enable[i] % 32);
	for (i = 0; i < T6030_WAKE_WORDS; i++)
		writel(val[i], s->wake + T6030_WAKE_MASK + 4 * i);
}

static void t6030_wake_masks_restore(struct apple_sleep *s)
{
	struct t6030_sleep *t = t6030_state(s);
	unsigned int i;

	for (i = 0; i < T6030_WAKE_WORDS; i++)
		writel(t->wake_saved[i], s->wake + T6030_WAKE_MASK + 4 * i);
}

/*
 * After the enclave, before the coprocessors stop: the PMU's stage, the notify masks, then the
 * votes. macOS's nub-spmi-a0 word comes before the first vote in every run.
 */
/*
 * macOS 27.2's last 30 ms on J516s that Linux didn't have (t5 sleep.tsv against Linux under the
 * HV, runs/p20-hv-stop-1j/sleep-diff.txt). The PMP is told the AP and then the IOP go idle over
 * RTKit's management endpoint, each answered within 0.2 ms; then its mailbox's I2A control bit
 * 0 goes off, and its power word comes down (-23.9 to -23.5 ms). 10.7 ms of nothing later, its
 * SRAM's word comes down (-12.8 ms). After the SMC's AP idle, the MTP's DART has its errors
 * masked and acknowledged (-11.0 ms).
 *
 * The mailbox is our DTS's pmp_mbox (0x350c08000, apple,asc-mailbox-v4, the registers of
 * drivers/mailbox/apple-mailbox.c); it's mapped from that address because the PMP's nodes are
 * added at run time, and only under APPLE_USE_PMP. The management messages are RTKit's
 * (drivers/soc/apple/rtkit.c). The DART's registers are apple-dart.c's T8110 ERROR and
 * ERROR_MASK, plus +0x704, written as traced.
 */
#define T6030_PMP_MBOX_PA		0x350c08000ULL
#define T6030_MBOX_A2I_CONTROL		0x110
#define T6030_MBOX_I2A_CONTROL		0x114
#define T6030_MBOX_CONTROL_FULL		BIT(16)
#define T6030_MBOX_CONTROL_EMPTY	BIT(17)
#define T6030_MBOX_I2A_IRQ		BIT(0)
#define T6030_MBOX_A2I_SEND0		0x800
#define T6030_MBOX_A2I_SEND1		0x808
#define T6030_MBOX_I2A_RECV0		0x830
#define T6030_MBOX_I2A_RECV1		0x838
#define T6030_RTKIT_MGMT_TYPE		GENMASK_ULL(59, 52)
#define T6030_RTKIT_SET_IOP_PWR		0x6
#define T6030_RTKIT_SET_IOP_PWR_ACK	0x7
#define T6030_RTKIT_SET_AP_PWR		0xb
#define T6030_RTKIT_PWR_IDLE		0x201
#define T6030_PMP_ACK_US		10000
#define T6030_PMP_DRAIN			32
#define T6030_PMP_TO_SRAM_US		10680
#define T6030_DART_ERROR		0x100
#define T6030_DART_ERROR_MASK		0x104
#define T6030_DART_704			0x704

static void t6030_late_map(struct apple_sleep *s)
{
	struct t6030_sleep *t = t6030_state(s);
	struct device_node *np = of_find_node_by_path("/soc/iommu@37a808000");

	t->pmp_mbox = ioremap_np(T6030_PMP_MBOX_PA, 0x1000);
	if (np && of_device_is_compatible(np, "apple,t6030-dart"))
		t->mtp_dart = of_iomap(np, 0);
	of_node_put(np);
	t->pmp_idle_ret = -ENODEV;
}

/* One management message to the PMP, then its answer: 0, or why none came */
static int t6030_pmp_mgmt(void __iomem *mb, u64 type, u64 ack, u64 *got)
{
	unsigned int us, n = 0;
	u64 m0, m1;

	if (readl(mb + T6030_MBOX_A2I_CONTROL) & T6030_MBOX_CONTROL_FULL)
		return -EBUSY;
	writeq(FIELD_PREP(T6030_RTKIT_MGMT_TYPE, type) | T6030_RTKIT_PWR_IDLE,
	       mb + T6030_MBOX_A2I_SEND0);
	writeq(0, mb + T6030_MBOX_A2I_SEND1);
	readl(mb + T6030_MBOX_A2I_CONTROL);
	for (us = 0; us < T6030_PMP_ACK_US; us++) {
		if (readl(mb + T6030_MBOX_I2A_CONTROL) & T6030_MBOX_CONTROL_EMPTY) {
			udelay(1);
			continue;
		}
		m0 = readq(mb + T6030_MBOX_I2A_RECV0);
		m1 = readq(mb + T6030_MBOX_I2A_RECV1);
		*got = m0;
		if (!(m1 & 0xff) && FIELD_GET(T6030_RTKIT_MGMT_TYPE, m0) == ack)
			return 0;
		if (++n >= T6030_PMP_DRAIN)
			return -EPROTO;
	}
	return -ETIMEDOUT;
}

static void t6030_pmp_idle(struct apple_sleep *s)
{
	struct t6030_sleep *t = t6030_state(s);
	void __iomem *mb = t->pmp_mbox;
	int ret;

	t->pmp_idle_ret = -ENODEV;
	if (!mb || !sleep_ps_on(s, T6030_PS_PMP))
		return;
	ret = t6030_pmp_mgmt(mb, T6030_RTKIT_SET_AP_PWR, T6030_RTKIT_SET_AP_PWR, &t->pmp_ack[0]);
	if (!ret)
		ret = t6030_pmp_mgmt(mb, T6030_RTKIT_SET_IOP_PWR, T6030_RTKIT_SET_IOP_PWR_ACK,
				     &t->pmp_ack[1]);
	writel(readl(mb + T6030_MBOX_I2A_CONTROL) & ~T6030_MBOX_I2A_IRQ,
	       mb + T6030_MBOX_I2A_CONTROL);
	t->pmp_idle_ret = ret;
}

static void t6030_mtp_dart_quiet(struct apple_sleep *s)
{
	void __iomem *d = t6030_state(s)->mtp_dart;

	if (!d)
		return;
	writel(0xffffffff, d + T6030_DART_ERROR_MASK);
	readl(d + T6030_DART_ERROR_MASK);
	writel(readl(d + T6030_DART_ERROR), d + T6030_DART_ERROR);
	writel(0xffffffff, d + T6030_DART_704);
	readl(d + T6030_DART_704);
}

static void t6030_sleep_prepare_regs(struct apple_sleep *s)
{
	struct t6030_sleep *t = t6030_state(s);

	/*
	 * The RAM walk first: what follows, up to the commit, is macOS's last 30 ms (t5), and the
	 * commit's 1.8 ms tail still cleans what the steps below write.
	 */
	t6030_sleep_clean_ram();
	t6030_alarm_snapshot(s, readl(s->spmi + SPMI_NOTIFY_ENABLE));
	/* t5: WD0's and WD1's counts to 0 at -28.3 ms, after the RTC read, before PMU 0xf801 */
	writel(0, s->wdt + WDT_WD0_COUNT);
	writel(0, s->wdt + WDT_WD1_COUNT);
	sleep_pmu_write(s, s->soc->pmu_sleep_reg, s->soc->pmu_sleep_val);
	t->notify_saved[0] = readl(s->spmi_a0 + SPMI_NOTIFY_ENABLE);
	t->notify_saved[1] = readl(s->spmi + SPMI_NOTIFY_ENABLE);
	t->notify_staged = true;
	t6030_wake_masks_set(s);
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
	t6030_wake_masks_restore(s);
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
	t6030_pmp_idle(s);
	ret = t6030_sleep_lower(s, T6030_LOWER_PMP, T6030_LOWER_PMS_SRAM);
	if (ret)
		return ret;
	udelay(T6030_PMP_TO_SRAM_US);
	ret = t6030_sleep_lower(s, T6030_LOWER_PMS_SRAM, T6030_LOWER_PHY);
	if (ret)
		return ret;
	ret = apple_sleep_smc_commit_notice(s);
	if (ret)
		return ret;
	if (!s->smc_idle_sent || s->smc_idle_ack < s->smc_idle_sent)
		return -EPROTO;
	t6030_mtp_dart_quiet(s);
	return 0;
}

/*
 * The GP PCIe host's PHY parked and its clock generator off, as macOS 27.2 on J516s does them
 * on the boot core after the other cores stop (t5): the PHY in two steps around APCIE_PHY_SW's
 * lowering, then its AXI bridge and the clock generator, waited on. pcie0's own sleep stop
 * holds its domains and leaves all of this running, so that a refused sleep finds the host
 * untouched; past the commit nothing comes back but a cold start.
 */
#define T6030_GP_PHY_CTRL		0x0
#define T6030_GP_PHY_CFG		0x4
#define T6030_GP_PHYIP_6C		0x6c
#define T6030_GP_PHYIP_7C		0x7c
#define T6030_GP_AXI2AF_600		0x600
#define T6030_GP_CLK_CTRL		0x0
#define T6030_GP_CLK_BUSY		BIT(31)
#define T6030_GP_POLL_US		1000

static bool t6030_gp_up(struct apple_sleep *s)
{
	return t6030_state(s)->gp_phy && sleep_ps_on(s, T6030_PS_APCIE_SYS_GP) &&
	       sleep_ps_on(s, T6030_PS_APCIE_PHY_SW);
}

static void t6030_gp_phy_park(struct apple_sleep *s)
{
	struct t6030_sleep *t = t6030_state(s);
	void __iomem *phy = t->gp_phy;
	int ret = 0;
	u32 v;

	t->gp_park_ret = -ENODEV;
	if (!t6030_gp_up(s))
		return;
	writel(readl(phy + T6030_GP_PHY_CTRL), phy + T6030_GP_PHY_CTRL);
	writel(readl(t->gp_phy_ip + T6030_GP_PHYIP_6C) & ~GENMASK(3, 2),
	       t->gp_phy_ip + T6030_GP_PHYIP_6C);
	readl(t->gp_phy_ip + T6030_GP_PHYIP_7C);
	writel(0xf000000, t->gp_phy_ip + T6030_GP_PHYIP_7C);
	writel(readl(phy + T6030_GP_PHY_CTRL) | BIT(7), phy + T6030_GP_PHY_CTRL);
	writel(readl(phy + T6030_GP_PHY_CTRL) & ~BIT(1), phy + T6030_GP_PHY_CTRL);
	ret |= readl_poll_timeout_atomic(phy + T6030_GP_PHY_CTRL, v, !(v & BIT(3)), 1,
					 T6030_GP_POLL_US);
	writel(readl(phy + T6030_GP_PHY_CTRL) & ~BIT(0), phy + T6030_GP_PHY_CTRL);
	ret |= readl_poll_timeout_atomic(phy + T6030_GP_PHY_CTRL, v, !(v & BIT(2)), 1,
					 T6030_GP_POLL_US);
	writel(readl(phy + T6030_GP_PHY_CTRL) & ~BIT(9), phy + T6030_GP_PHY_CTRL);
	writel(readl(phy + T6030_GP_PHY_CFG) & ~BIT(4), phy + T6030_GP_PHY_CFG);
	writel(readl(phy + T6030_GP_PHY_CFG) & ~BIT(0), phy + T6030_GP_PHY_CFG);
	t->gp_park_ret = ret ? -ETIMEDOUT : 0;
}

/* After APCIE_PHY_SW is off: the AXI bridge's bit 16, then the clock generator off */
static void t6030_gp_clk_off(struct apple_sleep *s)
{
	struct t6030_sleep *t = t6030_state(s);
	void __iomem *clk = t->gp_clkgen;
	u32 v;

	if (t->gp_park_ret == -ENODEV)
		return;
	writel(readl(t->gp_axi2af + T6030_GP_AXI2AF_600) & ~BIT(16),
	       t->gp_axi2af + T6030_GP_AXI2AF_600);
	writel(readl(clk + T6030_GP_CLK_CTRL) & ~BIT(5), clk + T6030_GP_CLK_CTRL);
	writel(readl(clk + T6030_GP_CLK_CTRL) | BIT(1), clk + T6030_GP_CLK_CTRL);
	readl_poll_timeout_atomic(clk + T6030_GP_CLK_CTRL, v, v & T6030_GP_CLK_BUSY, 1,
				  T6030_GP_POLL_US);
	writel(readl(clk + T6030_GP_CLK_CTRL) & ~BIT(0), clk + T6030_GP_CLK_CTRL);
	if (readl_poll_timeout_atomic(clk + T6030_GP_CLK_CTRL, v, !(v & T6030_GP_CLK_BUSY), 1,
				      T6030_GP_POLL_US))
		t->gp_park_ret = -ETIMEDOUT;
	writel(readl(clk + T6030_GP_CLK_CTRL) | BIT(2), clk + T6030_GP_CLK_CTRL);
}

/*
 * t6030_macos_down[] and _aon[] off, each saved first; one that doesn't go off is counted
 * (t->down_stuck), not fatal: macOS has them off long before its commit, with no order of its
 * own in the window.
 */
static void t6030_sleep_unstage(struct apple_sleep *s, u32 offset, u32 before);

static void t6030_sleep_down(struct apple_sleep *s)
{
	struct t6030_sleep *t = t6030_state(s);
	unsigned int i;
	u32 v, now;

	t->down_stuck = 0;
	for (i = 0; i < ARRAY_SIZE(t6030_macos_down); i++) {
		t->down_saved[i] = readl(s->ps + t6030_macos_down[i]);
		if (!FIELD_GET(PS_ACTUAL, t->down_saved[i]))
			continue;
		sleep_ps_lower(s, t6030_macos_down[i]);
		if (sleep_ps_wait_off(s, t6030_macos_down[i]))
			t->down_stuck++;
	}
	for (i = 0; i < ARRAY_SIZE(t6030_macos_up); i++) {
		t->up_saved[i] = readl(s->ps + t6030_macos_up[i]);
		if (FIELD_GET(PS_ACTUAL, t->up_saved[i]) == 0xf)
			continue;
		if (sleep_ps_raise_start(s, t6030_macos_up[i]))
			t->down_stuck++;
	}
	for (i = 0; i < ARRAY_SIZE(t6030_macos_down_aon); i++) {
		void __iomem *reg = s->ps_aon + t6030_macos_down_aon[i];

		v = readl(reg);
		t->down_aon_saved[i] = v;
		if (!FIELD_GET(PS_ACTUAL, v))
			continue;
		writel(v & ~(PS_AUTO_ENABLE | PS_FLAGS), reg);
		writel((v & ~(PS_AUTO_ENABLE | PS_FLAGS | PS_TARGET)) | PS_FLAGS, reg);
		if (readl_poll_timeout_atomic(reg, now, !FIELD_GET(PS_ACTUAL, now), 1,
					      PS_OFF_TIMEOUT_US))
			t->down_stuck++;
	}
	t->down_staged = true;
}

/* At the wake: the "ps" words back through the rewake (as before the entry), the aon ones here */
static void t6030_sleep_down_unstage(struct apple_sleep *s)
{
	struct t6030_sleep *t = t6030_state(s);
	unsigned int i;

	if (!t->down_staged)
		return;
	for (i = 0; i < ARRAY_SIZE(t6030_macos_down); i++)
		t6030_sleep_unstage(s, t6030_macos_down[i], t->down_saved[i]);
	for (i = 0; i < ARRAY_SIZE(t6030_macos_up); i++)
		t6030_sleep_unstage(s, t6030_macos_up[i], t->up_saved[i]);
}

static void t6030_sleep_down_aon_back(struct apple_sleep *s)
{
	struct t6030_sleep *t = t6030_state(s);
	unsigned int i;
	u32 now;

	if (!t->down_staged)
		return;
	t->down_staged = false;
	for (i = 0; i < ARRAY_SIZE(t6030_macos_down_aon); i++) {
		void __iomem *reg = s->ps_aon + t6030_macos_down_aon[i];
		u32 v = t->down_aon_saved[i];

		if (!FIELD_GET(PS_ACTUAL, v))
			continue;
		writel(v & ~PS_FLAGS, reg);
		readl_poll_timeout_atomic(reg, now, FIELD_GET(PS_ACTUAL, now), 1, PS_OFF_TIMEOUT_US);
	}
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
	t6030_gp_phy_park(s);
	if (t6030_sleep_lower(s, T6030_LOWER_PHY, ARRAY_SIZE(t6030_lowered)))
		t6030_sleep_failed("storage PHY power word");
	t6030_gp_clk_off(s);
	for (i = 0; i < ARRAY_SIZE(t6030_auto_off); i++)
		t->auto_saved[i] = readl(s->ps + t6030_auto_off[i]);
	t->auto_staged = true;
	t6030_sleep_down(s);
	sleep_seq_save(t->blk, &t->wake_early);
	sleep_seq_save(t->blk, &t->wake);
	if (sleep_seq_run_strict(t->blk, &t->commit))
		t6030_sleep_failed("apple,seq-commit");
	/* Every listed word as the commit leaves it, for the wake's report */
	sleep_ps_save(s);
}

/* Whether the kernel's tables map @addr: KFENCE's guard pages, for one, are left out */
static bool t6030_va_mapped(unsigned long addr)
{
	pgd_t *pgdp = pgd_offset_k(addr);
	p4d_t *p4dp;
	pud_t *pudp, pud;
	pmd_t *pmdp, pmd;

	if (pgd_none(READ_ONCE(*pgdp)) || pgd_bad(READ_ONCE(*pgdp)))
		return false;
	p4dp = p4d_offset(pgdp, addr);
	if (p4d_none(READ_ONCE(*p4dp)) || p4d_bad(READ_ONCE(*p4dp)))
		return false;
	pudp = pud_offset(p4dp, addr);
	pud = READ_ONCE(*pudp);
	if (pud_none(pud))
		return false;
	if (pud_leaf(pud))
		return pud_valid(pud);
	if (pud_bad(pud))
		return false;
	pmdp = pmd_offset(pudp, addr);
	pmd = READ_ONCE(*pmdp);
	if (pmd_none(pmd))
		return false;
	if (pmd_leaf(pmd))
		return pmd_valid(pmd);
	if (pmd_bad(pmd))
		return false;
	return pte_valid(READ_ONCE(*pte_offset_kernel(pmdp, addr)));
}

/* A run of mapped pages, cleaned once it ends */
struct t6030_run {
	unsigned long start, end;
};

static void t6030_run_add(struct t6030_run *r, unsigned long va)
{
	if (r->start && va == r->end) {
		r->end += PAGE_SIZE;
		return;
	}
	if (r->start)
		t6030_clean_range(r->start, r->end);
	r->start = va;
	r->end = va + PAGE_SIZE;
}

static void t6030_run_end(struct t6030_run *r)
{
	if (r->start)
		t6030_clean_range(r->start, r->end);
	r->start = 0;
}

/*
 * Every page of RAM in use, through the linear map: free pages hold nothing the wake reads,
 * and are left. Then the kernel image through its own mapping. Unblocked.
 */
static void t6030_clean_ram(void)
{
	struct t6030_run run = {};
	struct memblock_region *reg;
	unsigned long va;

	for_each_mem_region(reg) {
		unsigned long pfn = PFN_UP(reg->base), end = PFN_DOWN(reg->base + reg->size);

		if (memblock_is_nomap(reg))
			continue;
		while (pfn < end) {
			struct page *page;
			unsigned long nr = 1;

			if (!pfn_valid(pfn)) {
				t6030_run_end(&run);
				pfn++;
				continue;
			}
			page = pfn_to_page(pfn);
			if (PageBuddy(page)) {
				unsigned long order = page_private(page);

				if (order <= MAX_PAGE_ORDER && IS_ALIGNED(pfn, 1UL << order))
					nr = min(1UL << order, end - pfn);
				t6030_run_end(&run);
				pfn += nr;
				continue;
			}
			va = (unsigned long)page_address(page);
			if (!PageOffline(page) && va && t6030_va_mapped(va))
				t6030_run_add(&run, va);
			else
				t6030_run_end(&run);
			pfn++;
		}
		t6030_run_end(&run);
	}
	for (va = (unsigned long)_text & PAGE_MASK; va < PAGE_ALIGN((unsigned long)_end);
	     va += PAGE_SIZE) {
		if (t6030_va_mapped(va))
			t6030_run_add(&run, va);
		else
			t6030_run_end(&run);
	}
	t6030_run_end(&run);
}

static void t6030_clean_obj(const void *p, size_t size)
{
	if (p && size)
		t6030_clean_range((unsigned long)p, (unsigned long)p + size);
}

/*
 * Every page of RAM in use and the kernel image, past L2, while the other cores are held and
 * their caches still on: before the SMC's AP idle, the ~250 ms it takes kept out of the
 * spacing from there to the commit word. What is written after it is in a core's L1 or L2,
 * which t6030_sleep_clean() and each core's last WFI clean, or was evicted to memory.
 */
static void t6030_sleep_clean_ram(void)
{
	u64 hid4 = t6030_dc_unblock();

	t6030_clean_ram();
	t6030_dc_reblock(hid4);
}

/*
 * Then, right before the commit word: the kernel's data and each CPU's, the resume context,
 * this stack, the sleep state and its books, the wake sequences with the words read for them,
 * this profile's state, aura's mailbox (resume_pa) last, past L2 by VA; then this core's L1
 * and L2 by set and way. The other cores clean their own at their last WFI.
 */
static void t6030_sleep_clean(struct apple_sleep *s)
{
	const struct apple_sleep_soc *soc = s->soc;
	struct t6030_sleep *t = t6030_state(s);
	unsigned int i, stash_n = mpidr_hash_size();
	size_t pcpu = __per_cpu_end - __per_cpu_start;
	u64 hid4 = t6030_dc_unblock();

	t6030_clean_range((unsigned long)_sdata, (unsigned long)_end);
	for_each_possible_cpu(i)
		t6030_clean_obj(__per_cpu_start + per_cpu_offset(i), pcpu);
	t6030_clean_obj(task_stack_page(current), THREAD_SIZE);
	t6030_clean_obj(current, sizeof(*current));
	t6030_clean_obj(s, sizeof(*s));
	t6030_clean_obj(s->ps_commit, soc->ps_all.count * sizeof(*s->ps_commit));
	t6030_clean_obj(s->ps_aon_commit, soc->ps_aon_all.count * sizeof(*s->ps_aon_commit));
	for (i = 0; i < 2; i++) {
		const struct sleep_seq *seq = i ? &t->wake : &t->wake_early;

		t6030_clean_obj(seq->op, 4 * seq->n * sizeof(*seq->op));
		if (seq->saved)
			t6030_clean_obj(seq->saved, seq->n * sizeof(*seq->saved));
	}
	t6030_clean_obj(t, sizeof(*t));
	if (sleep_save_stash) {
		t6030_clean_obj(sleep_save_stash, stash_n * sizeof(*sleep_save_stash));
		for (i = 0; i < stash_n; i++)
			t6030_clean_obj((void *)READ_ONCE(sleep_save_stash[i]),
					sizeof(struct sleep_stack_data));
	}
	t6030_clean_obj(&sleep_save_stash, sizeof(sleep_save_stash));
	t6030_clean_obj(&mpidr_hash, sizeof(mpidr_hash));
	t6030_clean_obj(s->mbox, sizeof(*s->mbox));
	t6030_clean_all_levels();
	t6030_dc_reblock(hid4);
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
	/*
	 * The last PMU notes before the commit word: +3 the domains on that macOS has off, +4
	 * how many "ps" words are on (see t6030_domains_note()). Past them, the engine has the
	 * sleep.
	 */
	t6030_domains_note(s, 3);
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

	sleep_note(s, 0x70);
	for (i = 0; i < T6030_WAKE_WORDS; i++)
		t->wake_status[i] = readl(s->wake + T6030_WAKE_STATUS + 4 * i);
	t6030_sleep_wake_seq(s, &t->wake_early, "apple,seq-wake-early");
	sleep_note(s, 0x71);
	/* SIO_CPU is in both lists: its value before the lowering is the one before the entry */
	if (t->auto_staged)
		for (i = 0; i < ARRAY_SIZE(t6030_auto_off); i++)
			t6030_sleep_unstage(s, t6030_auto_off[i], t->auto_saved[i]);
	for (i = 0; i < t->nlowered; i++)
		t6030_sleep_unstage(s, t6030_lowered[i], t->lowered_saved[i]);
	t6030_sleep_down_unstage(s);
	t->auto_staged = false;
	t->nlowered = 0;
	sleep_ps_rewake_begin(s);
	sleep_ps_rewake_words(s, &t6030_wake_early_words, false);
	/* None of the memory side's registers before every early word is on */
	sleep_ps_rewake_settle(s);
	t6030_sleep_wake_check_ps(s, "before the memory side");
	sleep_note(s, 0x72);
	t6030_sleep_wake_seq(s, &t->wake, "apple,seq-wake");
	sleep_note(s, 0x73);
	sleep_ps_rewake_words(s, &t6030_wake_mid_words, false);
	t6030_sleep_undo_regs(s);
	sleep_note(s, 0x74);
	sleep_ps_rewake_words(s, &t6030_wake_late_words, false);
	sleep_ps_rewake_rest(s);
	t6030_sleep_down_aon_back(s);
	t6030_sleep_wake_check_ps(s, "at the end");
	sleep_note(s, 0x75);
}

/*
 * The cores' power-down controls, A7 to M3 (aura's cpu_regs.h; XNU's arm64_prepare_for_sleep
 * on its "global sleep" cores). CYC_OVRD is not locked before M4, and aura's init_cpu() leaves
 * each core's WFI in mode 2 with retention: a WFI that never powers the core down, so its
 * cluster never goes off and the sleep sequencer waits. The deep sleep wants mode 3 without
 * retention, and ACC_OVRD's deep-sleep settings.
 *
 * Every core's last steps are XNU's on H15 (pexpert/arm64/H15.h: __ARM_GLOBAL_SLEEP_BIT__,
 * HAS_CLUSTER, HAS_RETENTION_STATE, no NO_CPU_OVRD), and nothing else.
 * arm64_prepare_for_sleep(true) ORs ACC_OVRD's deep-sleep fields and CYC_OVRD's
 * ok2pwrdn_force_down (WFI mode 3) and disWfiRetn in, and loops on dsb, isb, wfi. That is at
 * the kernel's own EL, as aura's cpu_sleep(true) at EL2. S3_5_C15_C6_2 is T6041's and T6050's
 * deep-sleep register; H15 has no write to it.
 *
 * No external-debug release. ml_arm_sleep() writes EDLAR and EDPRCR only through
 * coresight_base[CORESIGHT_ED], and configure_coresight_registers() maps that only on a
 * debug-fused chip; J516s's /chosen effective-production-status-ap is 1. On J516s a plain read
 * of a core's coresight-reg window (0x210010310, EDPRCR) hangs the SoC.
 */
#define T6030_CYC_OVRD			sys_reg(3, 5, 15, 5, 0)
#define T6030_CYC_OVRD_WFI_MODE		GENMASK_ULL(25, 24)
#define T6030_CYC_OVRD_DISABLE_WFI_RET	BIT_ULL(0)
#define T6030_ACC_OVRD			sys_reg(3, 5, 15, 6, 0)
#define T6030_ACC_OVRD_PWR_DN_SRM	GENMASK_ULL(14, 13)
#define T6030_ACC_OVRD_DIS_L2_FLUSH	GENMASK_ULL(16, 15)
#define T6030_ACC_OVRD_TRAIN_DOWN_LINK	GENMASK_ULL(18, 17)
#define T6030_ACC_OVRD_POWER_DOWN_CPM	GENMASK_ULL(26, 25)
#define T6030_ACC_OVRD_NO_PIO_ON_WFI	BIT_ULL(32)
#define T6030_ACC_OVRD_DEEP_SLEEP	BIT_ULL(34)

static void t6030_deep_wfi_arm(void)
{
	u64 hid4 = t6030_dc_unblock();

	/*
	 * This core's last stores are in by now: its L1 and its cluster's L2 back to memory, as
	 * XNU's CleanPoC_Dcache before its sleep. A cluster's last core in cleans the L2 last.
	 */
	t6030_clean_all_levels();
	t6030_dc_reblock(hid4);

	/* As aura's cpu_sleep(true): only the L2 flush field is cleared first, the rest ORed in */
	sysreg_clear_set_s(T6030_ACC_OVRD, T6030_ACC_OVRD_DIS_L2_FLUSH,
			   T6030_ACC_OVRD_PWR_DN_SRM |
			   FIELD_PREP(T6030_ACC_OVRD_DIS_L2_FLUSH, 2) |
			   T6030_ACC_OVRD_TRAIN_DOWN_LINK | T6030_ACC_OVRD_POWER_DOWN_CPM |
			   T6030_ACC_OVRD_NO_PIO_ON_WFI | T6030_ACC_OVRD_DEEP_SLEEP);
	sysreg_clear_set_s(T6030_CYC_OVRD, T6030_CYC_OVRD_WFI_MODE,
			   FIELD_PREP(T6030_CYC_OVRD_WFI_MODE, 3) | T6030_CYC_OVRD_DISABLE_WFI_RET);
	isb();
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

/*
 * The ps words macOS 27.2 has OFF (actual state 0) at its commit on J516s, as the last value
 * each had in the t1, t1b, t1c, t3 and t4 traces (ps-at-commit.tsv). The others weren't read in
 * any window, so their state there is unknown: the dry run's list marks only these.
 */
static const u32 t6030_macos_off[] = {
	0x180, 0x1c0, 0x1d0, 0x1f8, 0x200, 0x208, 0x220, 0x250, 0x258, 0x270,
	0x3b8, 0x3c0, 0x3c8, 0x3d8, 0x3e0, 0x3f0, 0x3f8, 0x498, 0x4c8, 0x4d0,
	0x5b8, 0xc00, 0x4000, 0x4008, 0x4010, 0x4018, 0x4020, 0x4028, 0x10000,
};

static bool t6030_macos_off_at_commit(u32 offset)
{
	return sleep_ps_find(t6030_macos_off, ARRAY_SIZE(t6030_macos_off), offset) >= 0;
}

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
	.post_timer = t6030_sleep_post_timer,
	.wfi_again = t6030_sleep_wfi_again,
	.wake = t6030_sleep_wake,
	.cpu_prepare = t6030_sleep_cpu_prepare,
	.cpu_restore = t6030_sleep_cpu_restore,
	.macos_off_at_commit = t6030_macos_off_at_commit,
};

static const struct apple_sleep_soc t6030_sleep_soc = {
	.untested = true,
	.ops = &t6030_sleep_ops,
	.smc_policy = &t6030_smc_policy,
	.early_context = true,
	.own_wake = true,
	/*
	 * No early_cpu_wfi. H15 has __ARM_GLOBAL_SLEEP_BIT__, so XNU's ml_arm_sleep() holds
	 * each other core in WFE after its stop bit until the boot core clears arm64_stall_sleep.
	 * The boot core does that in its own ml_arm_sleep(), after its quiesce actions (the PMGR's
	 * last mile and the commit, IOCPUSleepKernel()). ACC_OVRD is one register for every core:
	 * armed early, "a CPU [that] executes WFI outside of the deep sleep context ... will hang".
	 * So the stopped cores wait in WFE and take their deep WFI after the commit.
	 */
	/*
	 * A core the sequencer restarts after the commit takes it again in the pen, with the
	 * A7-M3 power-down controls (aura's reset path puts CYC_OVRD back in retention)
	 */
	.pen_deep = true,
	.observer = true,
	/* No write to WD2's registers in any of macOS's entries */
	.no_wd2 = true,
	/* pcie0 (Wi-Fi, the card reader) holds APCIE_SYS_GP; ANS is on APCIE_SYS_ST */
	.gp_host_apart = true,
	/* .after_early and .commit_notice panic on a failed stop (t6030_sleep_check_stops()) */
	.failed_stop_ends_sleep = true,
	/* The ADT's spmi-chapel on nub-spmi0, and the sleep stage macOS writes to it */
	.pmu_usid = 0xb,
	.pmu_sleep_reg = 0xf801,
	.pmu_sleep_val = 0x50,
	.spmi_notify_sleep = 0x2000001,
	.cpu_cluster_affinity = 1,
	.cpu_clusters = 2,
	.cpu_cores_per_cluster = 6,
	.cpu_start_stride = 6,
	/*
	 * MEAS t5 (wake.tsv +20.87..21.04 ms): macOS starts each core with three writes, its
	 * stop-bit-numbered bit at +0x4, then +0x8 and +0xc, its own bit in its cluster's, 0 in
	 * the other's
	 */
	.cpu_start_every_cluster = true,
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
