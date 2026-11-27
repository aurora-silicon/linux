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
 * The two writes macOS 27.2 lowers each of them with, every run (entry -25.4, -24.7, -13.7 and
 * -8.5 ms): AUTO_ENABLE off with the target kept, then target 0. The flags differ from what
 * sleep_ps_lower() writes, and some stay: PMP keeps 0xf in bits 19:16 at the commit
 * (0x0f0f0300), the storage PHY reads 0x0f000200.
 */
static const u32 t6030_lowered_macos[][2] = {
	{ 0x400004f, 0x4000340 },	/* SIO_CPU */
	{ 0xf0f00ff, 0xf0f03f0 },	/* PMP */
	{ 0xf, 0x300 },			/* PMS_SRAM */
	{ 0x400004f, 0x4000340 },	/* APCIE_PHY_SW */
};

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
	/* The DISPEXT1 power gate as the entry found it */
	u32 gate_dispext1_saved;
	bool gate_dispext1_staged;
	/* The memory side brought to macOS's configured state (apple_sleep.mem_config) */
	bool mem_configured;
	void __iomem *mem_va[24];
	u64 mem_page[24];
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
static bool t6030_last_writes_on(void);
static void t6030_last_writes_filter(struct apple_sleep *s, struct sleep_seq *seq);
static void t6030_last_pmu_boot(struct apple_sleep *s);
static void t6030_spmi1_probe(struct apple_sleep *s);
static void t6030_pmu_boot_record(struct apple_sleep *s);
static void t6030_pwrgate_boot(struct apple_sleep *s);
static void t6030_cpm_probe(struct apple_sleep *s);
static void t6030_cpm_entry(void);
static void t6030_eng_probe(struct apple_sleep *s);
static void t6030_eng_boot_record(struct apple_sleep *s);
static void t6030_dbgprcr_report(struct apple_sleep *s);

static int t6030_mem_config_apply(struct apple_sleep *s);
static bool t6030_mem_config_boot_on(void);

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
	t6030_last_writes_filter(s, &t->commit);

	t6030_gp_map(s);
	t6030_late_map(s);
	t6030_ovrd_report(s, "probe");
	t6030_eng_probe(s);
	t6030_eng_boot_record(s);
	t6030_dbgprcr_report(s);
	t6030_pmu_boot_record(s);
	t6030_pwrgate_boot(s);
	t6030_cpm_probe(s);
	if (t6030_mem_config_boot_on())
		t6030_mem_config_apply(s);
	if (t6030_last_writes_on())
		t6030_last_pmu_boot(s);
	t6030_spmi1_probe(s);

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

	if (!c)
		return;
	/* With notes on, +2 already names the slot: +4 gets why, as -errno. */
	sleep_note_at(apple_sleep, 4, min(-c->stop_ret, 0xff));
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
/* The nub SPMI controllers' gen-1 interrupt words: masks from +0x20, acks from +0x60 */
#define T6030_SPMI_MASK(n)		(0x20 + 4 * (n))
#define T6030_SPMI_ACK(n)		(0x60 + 4 * (n))
#define T6030_SPMI_POLL_US		1000

static int t6030_spmi_xfer(void __iomem *spmi, u8 usid, u32 mask, u8 opcode, u16 address,
			   const u8 *wr, u8 *rd, unsigned int len)
{
	u8 command = opcode | (len - 1);
	unsigned int i, n;
	u32 value, data;
	int ret = 0;

	if (!len || len > 8 || !!wr == !!rd)
		return -EINVAL;
	readl(spmi + SPMI_STATUS);
	writel(mask, spmi + SPMI_NOTIFY_ENABLE);
	writel((u32)address << 16 | SPMI_CMD_ACTIVE | (u32)usid << 8 | command,
	       spmi + SPMI_CMD);
	for (i = 0; wr && i < len; i += 4) {
		data = 0;
		for (n = 0; n < min(4U, len - i); n++)
			data |= (u32)wr[i + n] << (8 * n);
		writel(data, spmi + SPMI_CMD);
	}
	if (!readl_poll_timeout_atomic(spmi + SPMI_NOTIFY_ACK, value,
				       value & T6030_SPMI_DONE, 1, T6030_SPMI_POLL_US))
		writel(T6030_SPMI_DONE, spmi + SPMI_NOTIFY_ACK);
	if (readl_poll_timeout_atomic(spmi + SPMI_STATUS, value,
				      !(value & T6030_SPMI_RX_EMPTY), 1, T6030_SPMI_POLL_US))
		return -ETIMEDOUT;
	value = readl(spmi + SPMI_REPLY);
	if ((value & 0xff) != command || ((value >> 8) & 0x7f) != usid ||
	    (wr && !(value & SPMI_CMD_ACTIVE)))
		ret = -EIO;
	for (i = 0; !ret && rd && i < len; i += 4) {
		if (readl(spmi + SPMI_STATUS) & T6030_SPMI_RX_EMPTY) {
			ret = -EIO;
			break;
		}
		data = readl(spmi + SPMI_REPLY);
		for (n = 0; n < min(4U, len - i); n++)
			rd[i + n] = data >> (8 * n);
	}
	/* Nothing left behind for the SPMI driver: the FIFO read empty, at most its 16 entries */
	for (i = 0; i < SPMI_DRAIN_MAX && !(readl(spmi + SPMI_STATUS) & T6030_SPMI_RX_EMPTY);
	     i++)
		readl(spmi + SPMI_REPLY);
	return ret;
}

static int t6030_pmu_xfer(struct apple_sleep *s, u32 mask, u8 opcode, u16 address,
			  const u8 *wr, u8 *rd, unsigned int len)
{
	return t6030_spmi_xfer(s->spmi, s->soc->pmu_usid, mask, opcode, address, wr, rd, len);
}

/*
 * DIAGNOSTIC (T6030 sleep bring-up): the main PMU's alarm and stage words as this boot found
 * them, read at probe before Linux writes any (rtc-macsmc probes later; last_writes' probe
 * clears 0xf803): 0x2000 bit 1 set means the alarm fired before the reset that started this
 * boot, and the scratch notes are what the last sleep left. Reads only.
 */
#define T6030_PMU_OFF2WAKE		0x281c
/* The register the sleep program's PMU commands write: D2C 1, C2S 2; at the wake S2C 1, C2D 0 */
#define T6030_PMU_PSTATE		0x200

/*
 * DIAGNOSTIC (T6030 sleep bring-up): the two clusters' CPMs (cpm-impl, E 0x210e40000, P
 * 0x211e40000, 0xc000 each), which macOS 27.2 configures and Linux never touched (lane A,
 * mac-cpm-1: every AP access to both across a boot, a sleep and a wake). About 0.3 s into XNU,
 * and again after the wake: +0x8000/+0x8008 = 0, +0x8080 written back, +0x40f8 = 1, bit 63 set
 * on +0x8400/+0x8408/+0x0270/+0x0250, an 11-entry table at +0x4018..+0x40b8, and
 * +0x4180..+0x41b8 (+0x4190 = 0xffffffff, +0x41b0 = 0x6665, +0x4180 = 3 last); iBoot leaves
 * those 0. 3.5 ms before the commit: +0x8080 = 0, then bit 63 off again on the four. 64-bit
 * registers. The probe logs every one of them; apple_sleep.cpm_macos=1 makes macOS's writes, in
 * its order, at probe and before apple,seq-commit (whose pwrgate +0xc0/+0xc8 writes macOS
 * interleaves them with). Only these offsets are touched; never the coresight or impl windows.
 */
#define T6030_CPM_E_PA			0x210e40000ULL
#define T6030_CPM_P_PA			0x211e40000ULL
#define T6030_CPM_SIZE			0xc000
#define T6030_CPM_CNT0			0x8000
#define T6030_CPM_CNT1			0x8008
#define T6030_CPM_CTL			0x8080
#define T6030_CPM_ENA			0x40f8
#define T6030_CPM_B63_0			0x8400
#define T6030_CPM_B63_1			0x8408
#define T6030_CPM_B63_2			0x0270
#define T6030_CPM_B63_3			0x0250
#define T6030_CPM_TABLE			0x4018
#define T6030_CPM_TABLE_STRIDE		0x10
#define T6030_CPM_CFG			0x4180
#define T6030_CPM_B63			BIT_ULL(63)

static bool t6030_cpm_macos;
module_param_named(cpm_macos, t6030_cpm_macos, bool, 0444);
MODULE_PARM_DESC(cpm_macos, "T6030 DIAGNOSTIC: macOS's CPM writes at boot and before the commit");

static void __iomem *t6030_cpm[2];

static const u16 t6030_cpm_table[] = {
	0x8a0f, 0x800f, 0x8c0f, 0x9c0f, 0x9e0f, 0x810f, 0xb90f, 0xbe0f, 0xbf0f, 0xd40f, 0x980f,
};
/* +0x4180..+0x41b8 in macOS's order, then +0x4180 = 3 */
static const u32 t6030_cpm_cfg[] = { 0, 0, 0xffffffff, 0, 0, 0, 0x6665, 0 };
#define T6030_CPM_CFG_ON		3

static void t6030_cpm_log(struct apple_sleep *s, const char *when)
{
	static const u16 regs[] = {
		T6030_CPM_B63_3, T6030_CPM_B63_2, T6030_CPM_ENA, T6030_CPM_CNT0, T6030_CPM_CNT1,
		T6030_CPM_CTL, T6030_CPM_B63_0, T6030_CPM_B63_1,
	};
	u64 v[ARRAY_SIZE(regs)], tbl[ARRAY_SIZE(t6030_cpm_table)], cfg[ARRAY_SIZE(t6030_cpm_cfg)];
	unsigned int c, i;

	for (c = 0; c < 2; c++) {
		if (!t6030_cpm[c])
			continue;
		for (i = 0; i < ARRAY_SIZE(regs); i++)
			v[i] = readq(t6030_cpm[c] + regs[i]);
		for (i = 0; i < ARRAY_SIZE(tbl); i++)
			tbl[i] = readq(t6030_cpm[c] + T6030_CPM_TABLE + T6030_CPM_TABLE_STRIDE * i);
		for (i = 0; i < ARRAY_SIZE(cfg); i++)
			cfg[i] = readq(t6030_cpm[c] + T6030_CPM_CFG + 8 * i);
		dev_info(s->dev, "CPM %c at %s: 0250 %#llx 0270 %#llx 40f8 %#llx 8000 %#llx 8008 %#llx 8080 %#llx 8400 %#llx 8408 %#llx\n",
			 c ? 'P' : 'E', when, v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7]);
		dev_info(s->dev, "CPM %c at %s: 4018.. %llx %llx %llx %llx %llx %llx %llx %llx %llx %llx %llx | 4180.. %llx %llx %llx %llx %llx %llx %llx %llx\n",
			 c ? 'P' : 'E', when, tbl[0], tbl[1], tbl[2], tbl[3], tbl[4], tbl[5], tbl[6],
			 tbl[7], tbl[8], tbl[9], tbl[10], cfg[0], cfg[1], cfg[2], cfg[3], cfg[4],
			 cfg[5], cfg[6], cfg[7]);
	}
}

static void t6030_cpm_b63(unsigned int c, u16 off, bool on)
{
	u64 v = readq(t6030_cpm[c] + off);

	writeq(on ? v | T6030_CPM_B63 : v & ~T6030_CPM_B63, t6030_cpm[c] + off);
}

/* macOS's boot writes, both clusters, in its order */
static void t6030_cpm_boot_writes(void)
{
	unsigned int c, i;

	for (c = 0; c < 2; c++) {
		writeq(0, t6030_cpm[c] + T6030_CPM_CNT0);
		writeq(0, t6030_cpm[c] + T6030_CPM_CNT1);
		writeq(readq(t6030_cpm[c] + T6030_CPM_CTL), t6030_cpm[c] + T6030_CPM_CTL);
		writeq(1, t6030_cpm[c] + T6030_CPM_ENA);
	}
	for (c = 0; c < 2; c++) {
		t6030_cpm_b63(c, T6030_CPM_B63_0, true);
		t6030_cpm_b63(c, T6030_CPM_B63_1, true);
	}
	for (c = 0; c < 2; c++)
		t6030_cpm_b63(c, T6030_CPM_B63_2, true);
	for (c = 0; c < 2; c++)
		t6030_cpm_b63(c, T6030_CPM_B63_3, true);
	for (c = 0; c < 2; c++) {
		writeq(1, t6030_cpm[c] + T6030_CPM_ENA);
		for (i = 0; i < ARRAY_SIZE(t6030_cpm_table); i++)
			writeq(t6030_cpm_table[i],
			       t6030_cpm[c] + T6030_CPM_TABLE + T6030_CPM_TABLE_STRIDE * i);
	}
	for (c = 0; c < 2; c++) {
		for (i = 0; i < ARRAY_SIZE(t6030_cpm_cfg); i++)
			writeq(t6030_cpm_cfg[i], t6030_cpm[c] + T6030_CPM_CFG + 8 * i);
		writeq(T6030_CPM_CFG_ON, t6030_cpm[c] + T6030_CPM_CFG);
	}
}

static void t6030_cpm_probe(struct apple_sleep *s)
{
	struct resource res;
	static const u64 pa[2] = { T6030_CPM_E_PA, T6030_CPM_P_PA };
	unsigned int c;

	for (c = 0; c < 2; c++) {
		res = (struct resource)DEFINE_RES_MEM(pa[c], T6030_CPM_SIZE);
		res.flags |= IORESOURCE_MEM_NONPOSTED;
		t6030_cpm[c] = apple_sleep_ioremap(s->dev, &res);
		if (!t6030_cpm[c])
			return;
	}
	t6030_cpm_log(s, "probe");
	if (!READ_ONCE(t6030_cpm_macos))
		return;
	t6030_cpm_boot_writes();
	t6030_cpm_log(s, "probe, after macOS's writes");
}

/* macOS's entry writes, 3.5 ms before its commit: +0x8080 = 0, then bit 63 off on the four */
static void t6030_cpm_entry(void)
{
	unsigned int c;

	if (!READ_ONCE(t6030_cpm_macos) || !t6030_cpm[0] || !t6030_cpm[1])
		return;
	for (c = 0; c < 2; c++)
		writeq(0, t6030_cpm[c] + T6030_CPM_CTL);
	for (c = 0; c < 2; c++)
		t6030_cpm_b63(c, T6030_CPM_B63_3, false);
	for (c = 0; c < 2; c++)
		t6030_cpm_b63(c, T6030_CPM_B63_2, false);
	for (c = 0; c < 2; c++) {
		t6030_cpm_b63(c, T6030_CPM_B63_0, false);
		t6030_cpm_b63(c, T6030_CPM_B63_1, false);
	}
}

/*
 * DIAGNOSTIC (T6030 sleep bring-up): the cluster power-gate enables in pmgr[28] ("pwrgate").
 * iBoot leaves both 0. About 1 s into XNU, macOS's PMGR driver reads +0x0 (the E cluster's) and
 * writes 0x80000000, then reads +0x60 (the P cluster's) and writes 0x80000000; at each wake +0x60
 * reads 0 again and is set at +3.7 ms. Linux committed with both 0. On the Neo the P cluster's
 * enable left P0 clock-gated, and the sleep engine waited at P_DOWN forever. The probe logs
 * +0x0..+0xfc; apple_sleep.pcluster_pg=1 also makes macOS's two boot writes.
 */
#define T6030_PWRGATE_E_CLUSTER		0x0
#define T6030_PWRGATE_P_CLUSTER		0x60
#define T6030_PWRGATE_ENABLE		BIT(31)
#define T6030_PWRGATE_WORDS		64

static bool t6030_pcluster_pg;
module_param_named(pcluster_pg, t6030_pcluster_pg, bool, 0444);
MODULE_PARM_DESC(pcluster_pg, "T6030 DIAGNOSTIC: macOS's cluster power-gate enables (pwrgate +0x0 and +0x60 = 0x80000000) at probe");

static void t6030_pwrgate_boot(struct apple_sleep *s)
{
	u32 w[T6030_PWRGATE_WORDS], e, p;
	int i;

	for (i = 0; i < T6030_PWRGATE_WORDS; i++)
		w[i] = readl(s->pwrgate + 4 * i);
	print_hex_dump(KERN_INFO, "apple-sleep pwrgate: ", DUMP_PREFIX_OFFSET, 32, 4, w, sizeof(w),
		       false);
	if (!READ_ONCE(t6030_pcluster_pg))
		return;
	e = readl(s->pwrgate + T6030_PWRGATE_E_CLUSTER);
	writel(T6030_PWRGATE_ENABLE, s->pwrgate + T6030_PWRGATE_E_CLUSTER);
	p = readl(s->pwrgate + T6030_PWRGATE_P_CLUSTER);
	writel(T6030_PWRGATE_ENABLE, s->pwrgate + T6030_PWRGATE_P_CLUSTER);
	dev_info(s->dev, "pcluster_pg: cluster power gates E %#x -> %#x, P %#x -> %#x\n", e,
		 readl(s->pwrgate + T6030_PWRGATE_E_CLUSTER), p,
		 readl(s->pwrgate + T6030_PWRGATE_P_CLUSTER));
}

static void t6030_pmu_boot_record(struct apple_sleep *s)
{
	u32 runtime = readl(s->spmi + SPMI_NOTIFY_ENABLE), mask = runtime & ~T6030_SPMI_DONE;
	u8 ctl = 0, rmask = 0, compare[4] = {}, ticks[6] = {}, stage[4] = {}, wake = 0, notes[5] = {};
	u8 pst[2] = {};
	u32 cmp, now;
	int ret;

	ret = t6030_pmu_xfer(s, mask, T6050_SPMI_EXT_READL, T6050_RTC_CTL, NULL, &ctl, 1) ?:
	      t6030_pmu_xfer(s, mask, T6050_SPMI_EXT_READL, T6050_RTC_MASK, NULL, &rmask, 1) ?:
	      t6030_pmu_xfer(s, mask, T6050_SPMI_EXT_READL, T6050_RTC_COMPARE, NULL, compare, 4) ?:
	      t6030_pmu_xfer(s, mask, T6050_SPMI_EXT_READL, T6050_RTC_COUNTER, NULL, ticks, 6) ?:
	      t6030_pmu_xfer(s, mask, T6050_SPMI_EXT_READL, 0xf800, NULL, stage, 4) ?:
	      t6030_pmu_xfer(s, mask, T6050_SPMI_EXT_READL, T6030_PMU_OFF2WAKE, NULL, &wake, 1) ?:
	      t6030_pmu_xfer(s, mask, T6050_SPMI_EXT_READL, PMU_NOTE_BASE, NULL, notes, 5) ?:
	      t6030_pmu_xfer(s, mask, T6050_SPMI_EXT_READL, T6030_PMU_PSTATE, NULL, pst, 2);
	writel(runtime, s->spmi + SPMI_NOTIFY_ENABLE);
	cmp = get_unaligned_le32(compare);
	now = get_unaligned_le32(ticks + 2);
	dev_info(s->dev, "boot PMU%s: 0x2000 %#04x (armed %d event %d) 0x200e %#x compare-counter %+d s, f801 %#04x f802 %#04x f803 %#04x, 0x281c %#04x, 0x200/0x201 %#04x %#04x, notes %5ph\n",
		 ret ? " (a read failed)" : "", ctl, !!(ctl & T6050_RTC_CTL_ARMED),
		 !!(ctl & T6050_RTC_CTL_FIRED), rmask, (s32)(cmp - now), stage[1], stage[2], stage[3],
		 wake, pst[0], pst[1], notes);
}

/*
 * DIAGNOSTIC (T6030 sleep bring-up): the second Chapel PMU (ADT spmi-chapelF1, SID 0xb on
 * nub-spmi1, with its own RTC block and off2wake latch), which the DT leaves disabled and no
 * driver touches. apple_sleep.spmi1=1 maps nub-spmi1 at probe, once its always-on power word
 * reads on, and logs one line of that PMU's RTC words, its 0xf800 block and 0x281c; writing
 * spmi1_dump logs another, and the observer's obs_timeline=4 samples it. Read commands only:
 * the controller's notify mask is put back as found after each. Boot parameter, off by default.
 */
#define T6030_SPMI1_BASE		0x364794000ULL
#define T6030_SPMI1_SIZE		0x100
#define T6030_AON_NUB_SPMI1		0x60
#define T6030_PMU2_USID			0xb
#define T6030_PS_ACTIVE			0xf

static bool t6030_spmi1;
module_param_named(spmi1, t6030_spmi1, bool, 0444);
MODULE_PARM_DESC(spmi1, "T6030 DIAGNOSTIC: read the second PMU on nub-spmi1 (probe line, spmi1_dump, obs_timeline=4)");

static void __iomem *t6030_spmi1_base;

static int t6030_pmu2_read(u16 reg, u8 *buf, unsigned int len)
{
	void __iomem *spmi = t6030_spmi1_base;
	unsigned int done, n;
	u32 runtime;
	int ret = 0;

	if (!spmi)
		return -ENODEV;
	runtime = readl(spmi + SPMI_NOTIFY_ENABLE);
	for (done = 0; !ret && done < len; done += n) {
		n = min(len - done, 8U);
		ret = t6030_spmi_xfer(spmi, T6030_PMU2_USID, runtime & ~T6030_SPMI_DONE,
				      T6050_SPMI_EXT_READL, reg + done, NULL, buf + done, n);
	}
	writel(runtime, spmi + SPMI_NOTIFY_ENABLE);
	return ret;
}

static void t6030_pmu2_report(struct apple_sleep *s, const char *when)
{
	u8 ctl = 0, mask = 0, wake = 0, ticks[6] = {}, compare[4] = {}, stage[32] = {};
	int ret;

	ret = t6030_pmu2_read(T6050_RTC_CTL, &ctl, 1) ?:
	      t6030_pmu2_read(T6050_RTC_COUNTER, ticks, 6) ?:
	      t6030_pmu2_read(T6050_RTC_COMPARE, compare, 4) ?:
	      t6030_pmu2_read(T6050_RTC_MASK, &mask, 1) ?:
	      t6030_pmu2_read(T6030_PMU_OFF2WAKE, &wake, 1) ?:
	      t6030_pmu2_read(0xf800, stage, sizeof(stage));
	if (ret) {
		dev_info(s->dev, "spmi1 PMU at %s: read failed (%d)\n", when, ret);
		return;
	}
	dev_info(s->dev, "spmi1 PMU at %s: ctl %#04x counter %u s compare %u s mask %#x 0x281c %#04x | f800: %*ph\n",
		 when, ctl, get_unaligned_le32(ticks + 2), get_unaligned_le32(compare), mask, wake,
		 (int)sizeof(stage), stage);
}

static int t6030_pmu2_write(u16 reg, const u8 *buf, unsigned int len)
{
	void __iomem *spmi = t6030_spmi1_base;
	u32 runtime;
	int ret;

	if (!spmi || !len || len > 8)
		return -EINVAL;
	runtime = readl(spmi + SPMI_NOTIFY_ENABLE);
	ret = t6030_spmi_xfer(spmi, T6030_PMU2_USID, runtime & ~T6030_SPMI_DONE,
			      T6050_SPMI_EXT_WRITEL, reg, buf, NULL, len);
	writel(runtime, spmi + SPMI_NOTIFY_ENABLE);
	return ret;
}

/*
 * DIAGNOSTIC (T6030 sleep bring-up), apple_sleep.spmi1_boot=1 (with spmi1): the second PMU's
 * housekeeping macOS 27.2's boot does and Linux never did (lane A, mac-spmi1-1), literally:
 * nub-spmi1's notify mask raised to its steady 0x3f833ff1; 0xf801 = 0x30 and the status reads;
 * 0xf803 read and cleared, 0xf808 cleared; the fault log 0x2200 (7 bytes, 0x9000 there) and the
 * wake latch 0x281c (2 bytes, 4 there) read and written back to clear them; 0x8b00 read; then
 * 0xf802 = 0 and the reads again. Every value is logged before it is cleared. At the entry,
 * 26.6 ms before its commit, macOS sets the notify mask to 0x2000001 (t6030_spmi1_entry).
 */
#define T6030_SPMI1_NOTIFY_RUN		0x3f833ff1
#define T6030_SPMI1_NOTIFY_SLEEP	0x2000001

static bool t6030_spmi1_boot;
module_param_named(spmi1_boot, t6030_spmi1_boot, bool, 0444);
MODULE_PARM_DESC(spmi1_boot, "T6030 DIAGNOSTIC: with spmi1, macOS's second-PMU boot housekeeping and entry mask");

static void t6030_pmu2_status_reads(u8 *st)
{
	static const struct { u16 reg; u8 len; } reads[] = {
		{ 0xf801, 1 }, { 0xf808, 4 }, { 0xf802, 1 }, { 0xf818, 1 }, { 0xf819, 2 },
		{ 0xf800, 1 }, { 0xf81f, 1 },
	};
	unsigned int i, o = 0;

	for (i = 0; i < ARRAY_SIZE(reads); i++) {
		t6030_pmu2_read(reads[i].reg, st + o, reads[i].len);
		o += reads[i].len;
	}
}

static void t6030_spmi1_housekeeping(struct apple_sleep *s)
{
	u8 v30 = 0x30, zero[4] = {}, st[11] = {}, st2[11] = {}, f803 = 0, fault[7] = {};
	u8 wake[2] = {}, id[5] = {};
	u32 mask = readl(t6030_spmi1_base + SPMI_NOTIFY_ENABLE);

	writel(T6030_SPMI1_NOTIFY_RUN, t6030_spmi1_base + SPMI_NOTIFY_ENABLE);
	t6030_pmu2_write(0xf801, &v30, 1);
	t6030_pmu2_status_reads(st);
	t6030_pmu2_read(0xf803, &f803, 1);
	t6030_pmu2_write(0xf803, zero, 1);
	t6030_pmu2_write(0xf808, zero, 4);
	t6030_pmu2_read(0x2200, fault, sizeof(fault));
	t6030_pmu2_write(0x2200, fault, sizeof(fault));
	t6030_pmu2_read(T6030_PMU_OFF2WAKE, wake, sizeof(wake));
	t6030_pmu2_write(T6030_PMU_OFF2WAKE, wake, sizeof(wake));
	t6030_pmu2_read(0x8b00, id, sizeof(id));
	t6030_pmu2_write(0xf802, zero, 1);
	t6030_pmu2_status_reads(st2);
	dev_info(s->dev, "spmi1_boot: notify %#x -> %#x; status %*ph; f803 %#04x (cleared); fault 0x2200 %*ph; wake 0x281c %*ph (written back); 0x8b00 %*ph; after %*ph\n",
		 mask, readl(t6030_spmi1_base + SPMI_NOTIFY_ENABLE), (int)sizeof(st), st, f803,
		 (int)sizeof(fault), fault, (int)sizeof(wake), wake, (int)sizeof(id), id,
		 (int)sizeof(st2), st2);
}

static void t6030_spmi1_entry(struct apple_sleep *s)
{
	if (READ_ONCE(t6030_spmi1_boot) && t6030_spmi1_base)
		writel(T6030_SPMI1_NOTIFY_SLEEP, t6030_spmi1_base + SPMI_NOTIFY_ENABLE);
}

static void t6030_spmi1_probe(struct apple_sleep *s)
{
	struct resource res;
	u32 ps;

	if (!READ_ONCE(t6030_spmi1) || t6030_spmi1_base)
		return;
	ps = readl(s->ps_aon + T6030_AON_NUB_SPMI1);
	if (FIELD_GET(PS_ACTUAL, ps) != T6030_PS_ACTIVE) {
		dev_info(s->dev, "spmi1 PMU: nub-spmi1's power word reads %#x, not mapped\n", ps);
		return;
	}
	/* Nonposted, as the bus it sits on (nonposted-mmio): a posted write there resets the SoC */
	res = (struct resource)DEFINE_RES_MEM(T6030_SPMI1_BASE, T6030_SPMI1_SIZE);
	res.flags |= IORESOURCE_MEM_NONPOSTED;
	t6030_spmi1_base = apple_sleep_ioremap(s->dev, &res);
	if (!t6030_spmi1_base)
		return;
	dev_info(s->dev, "spmi1: power %#x, status %#x, mask0 %#x ack0 %#x, notify %#x\n", ps,
		 readl(t6030_spmi1_base + SPMI_STATUS), readl(t6030_spmi1_base + T6030_SPMI_MASK(0)),
		 readl(t6030_spmi1_base + T6030_SPMI_ACK(0)),
		 readl(t6030_spmi1_base + SPMI_NOTIFY_ENABLE));
	t6030_pmu2_report(s, "probe");
	if (READ_ONCE(t6030_spmi1_boot))
		t6030_spmi1_housekeeping(s);
}

static int t6030_spmi1_dump_set(const char *val, const struct kernel_param *kp)
{
	struct apple_sleep *s = apple_sleep;

	if (!s || !t6030_spmi1_base)
		return -ENODEV;
	t6030_pmu2_report(s, "dump");
	return 0;
}

static const struct kernel_param_ops t6030_spmi1_dump_ops = {
	.set = t6030_spmi1_dump_set,
};
module_param_cb(spmi1_dump, &t6030_spmi1_dump_ops, NULL, 0200);
MODULE_PARM_DESC(spmi1_dump, "T6030 DIAGNOSTIC: with spmi1, log the second PMU's words now");

/*
 * DIAGNOSTIC (T6030 sleep bring-up): the last-second differences lane A's 0x1000 stops of macOS
 * 27.2 and Linux 313ee6fb show on J516S (t6030-linux-313ee6fb-stop-1-diff-summary):
 *  - the PMU's RTC alarm: after the commit word macOS finds it disarmed (control 0x01) and arms
 *    it again (0x41), an edge; Linux's is still armed from rtcwake. Disarm it (0x01) first;
 *  - SOC-DEV-DVFS[0] (PTD +0x900): macOS writes bit 61 | 1 there 4.2 ms before the late group's
 *    votes, which then drop it to bit 61 alone;
 *  - AFR (ps-gpu +0x100): apple,seq-commit writes it 0, macOS doesn't touch it in its window.
 * Boot parameter, off by default.
 */
static bool t6030_last_writes;
module_param_named(last_writes, t6030_last_writes, bool, 0444);
MODULE_PARM_DESC(last_writes, "T6030 DIAGNOSTIC: macOS's last-second PMU, RTC alarm, DVFS[0] and AFR writes");

static bool t6030_last_writes_on(void)
{
	return READ_ONCE(t6030_last_writes);
}

#define T6030_LAST_DVFS0		0x900
#define T6030_LAST_DVFS0_VOTE		(BIT_ULL(61) | 1)
#define T6030_LAST_DVFS0_TO_SLPA_US	(15200 + 4200)
#define T6030_BLK_PS_GPU_AFR		0x100

/* apple,seq-commit without its AFR write (block 13, +0x100), for last_writes */
static void t6030_last_writes_filter(struct apple_sleep *s, struct sleep_seq *seq)
{
	unsigned int i, n = 0;
	u32 *op;

	if (!READ_ONCE(t6030_last_writes) || seq->saved)
		return;
	op = devm_kcalloc(s->dev, seq->n, 4 * sizeof(u32), GFP_KERNEL);
	if (!op)
		return;
	for (i = 0; i < seq->n; i++) {
		const u32 *e = seq->op + 4 * i;

		if (e[0] == 0 && e[1] == 13 && e[2] == T6030_BLK_PS_GPU_AFR)
			continue;
		memcpy(op + 4 * n++, e, 4 * sizeof(u32));
	}
	dev_info(s->dev, "last_writes: apple,seq-commit without its AFR write (%u of %u entries)\n",
		 n, seq->n);
	seq->op = op;
	seq->n = n;
}

/*
 * last_writes: after the PMU's sleep stage write (0xf801), macOS reads 0xf801, 0xf808 (4 bytes),
 * 0xf802, 0xf818, 0xf819 (2), 0xf800 and 0xf81f, all 0 there: latched event status, perhaps
 * cleared by the read. Its I2C6 word is one write of 0x3f0; the last wake-mask word has only
 * its 25 low bits set (0x01bfffff at macOS's boot), and NUB_FABRIC (ps-aon +0xa0) is under
 * AUTO_ENABLE at its final WFI.
 */
#define T6030_LAST_I2C6			0x3f0
#define T6030_LAST_WAKE_LAST_BITS	GENMASK(24, 0)
#define T6030_AON_NUB_FABRIC		0xa0

static void t6030_last_pmu_reads(struct apple_sleep *s);

/*
 * last_writes, at the driver's probe: macOS reads the PMU's 0xf803 (0x30 at its boot, after the
 * boot stage 0xf801 = 0x30), writes it 0 and clears 0xf808, 1.27 s before its PMP starts. Linux's
 * loader sets the same stage and never clears them.
 */
static void t6030_last_pmu_boot(struct apple_sleep *s)
{
	u32 runtime = readl(s->spmi + SPMI_NOTIFY_ENABLE);
	u8 v = 0, zero[4] = {};

	t6030_pmu_xfer(s, runtime & ~T6030_SPMI_DONE, T6050_SPMI_EXT_READL, 0xf803, NULL, &v, 1);
	t6030_pmu_xfer(s, runtime & ~T6030_SPMI_DONE, T6050_SPMI_EXT_WRITEL, 0xf803, zero, NULL, 1);
	t6030_pmu_xfer(s, runtime & ~T6030_SPMI_DONE, T6050_SPMI_EXT_WRITEL, 0xf808, zero, NULL, 4);
	writel(runtime, s->spmi + SPMI_NOTIFY_ENABLE);
	dev_info(s->dev, "last_writes: PMU 0xf803 was %#x, cleared with 0xf808\n", v);
}

/*
 * last_writes, at the entry: the alarm disarmed (control 0x01, as macOS has it until the commit)
 * before the PMU's sleep stage write, so that the rearm after the commit word arms it with an
 * edge, after that write, as on macOS.
 */
static void t6030_last_alarm_disarm(struct apple_sleep *s)
{
	u32 runtime = readl(s->spmi + SPMI_NOTIFY_ENABLE);
	u8 v = 0x01;

	t6030_pmu_xfer(s, runtime & ~T6030_SPMI_DONE, T6050_SPMI_EXT_WRITEL, T6050_RTC_CTL, &v,
		       NULL, 1);
	writel(runtime, s->spmi + SPMI_NOTIFY_ENABLE);
}

/* last_writes, at the wake: macOS's stage 0xf801 = 0xfe, then the same status reads */
static void t6030_last_pmu_wake(struct apple_sleep *s)
{
	u32 runtime = readl(s->spmi + SPMI_NOTIFY_ENABLE);
	u8 v = 0xfe;

	t6030_pmu_xfer(s, runtime & ~T6030_SPMI_DONE, T6050_SPMI_EXT_WRITEL, 0xf801, &v, NULL, 1);
	writel(runtime, s->spmi + SPMI_NOTIFY_ENABLE);
	t6030_last_pmu_reads(s);
}

static void t6030_last_pmu_reads(struct apple_sleep *s)
{
	static const struct { u16 reg; u8 len; } reads[] = {
		{ 0xf801, 1 }, { 0xf808, 4 }, { 0xf802, 1 }, { 0xf818, 1 }, { 0xf819, 2 },
		{ 0xf800, 1 }, { 0xf81f, 1 },
	};
	u32 runtime = readl(s->spmi + SPMI_NOTIFY_ENABLE);
	u8 buf[4];
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(reads); i++)
		t6030_pmu_xfer(s, runtime & ~T6030_SPMI_DONE, T6050_SPMI_EXT_READL, reads[i].reg,
			       NULL, buf, reads[i].len);
	writel(runtime, s->spmi + SPMI_NOTIFY_ENABLE);
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
/* t5: the commit word's second and third writes, +764.6 and +922.0 us after the first */
#define T6030_RECOMMIT_US		157

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
	/* t5: the CLLT word and the commit word a third time, 157 us after the second */
	udelay(T6030_RECOMMIT_US);
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
	if (READ_ONCE(t6030_last_writes))
		val[T6030_WAKE_WORDS - 1] &= T6030_LAST_WAKE_LAST_BITS;
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
/* The value macOS writes to ERROR after reading it, in every run (an acknowledgment) */
#define T6030_DART_ERROR_MACOS		0x10100000

/*
 * macOS 27.2's PTD request lifecycle on J516s across sleep and wake:
 *  - before pmVC (-4.71 s): 0x80000 to pmgr[40]+0x18328;
 *  - at pmVC (-4.44 s): 0x10040 to 0x3503d08c0, polled on 0x3503c11c0 until acknowledged;
 *  - 1.05 s before the commit word: 0x10000 to 0x3503d08c0 (no ack poll), kept across commit;
 *  - on wake (+9.16 ms, before PMS_SRAM and PMP power on): read 0x3503c1180 and seed 0x10000.
 */
#define T6030_PTD_READ_PA		0x3503c1000ULL
#define T6030_PTD_TGT_READ		0x180
#define T6030_PTD_ACTUAL		0x1c0
#define T6030_PTD_UPDATE_PA		0x3503d0000ULL
#define T6030_PTD_REQUEST		0x8c0
#define T6030_PTD_CTRL_PA		0x3503d8000ULL
#define T6030_PTD_SLEEP_CTRL		0x328
#define T6030_PTD_SLEEP_CTRL_ENTRY	0x80000
#define T6030_MASK_AT_PMVC		0x10040ULL
#define T6030_MASK_LATE			0x10000ULL
#define T6030_PTD_ACK_US		50000
/* 1.05 s before the commit word, which comes 13.4 ms after slpa */
#define T6030_MASK_LATE_TO_SLPA_US	1036600

static bool t6030_diag_mask_steps = true;
module_param_named(diag_mask_steps, t6030_diag_mask_steps, bool, 0644);
MODULE_PARM_DESC(diag_mask_steps, "T6030: macOS's PTD request steps across sleep and wake (default: on)");
static void __iomem *t6030_ptd_read;
static void __iomem *t6030_ptd_update;
static void __iomem *t6030_ptd_ctrl;

static void t6030_late_map(struct apple_sleep *s)
{
	struct t6030_sleep *t = t6030_state(s);
	struct device_node *np = of_find_node_by_path("/soc/iommu@37a808000");

	t6030_ptd_read = ioremap_np(T6030_PTD_READ_PA, 0x1000);
	t6030_ptd_update = ioremap_np(T6030_PTD_UPDATE_PA, 0x1000);
	t6030_ptd_ctrl = ioremap_np(T6030_PTD_CTRL_PA, 0x1000);
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

/* The running PMP's driver, registered for the commit, or NULL */
static struct apple_sleep_coproc *t6030_pmp_coproc(void)
{
	int i;

	for (i = 0; i < SLEEP_COPROCS; i++) {
		struct apple_sleep_coproc *c = &apple_sleep_coprocs[i];

		if (c->dev && c->ready &&
		    of_device_is_compatible(c->dev->of_node, "apple,t6030-pmp-v2"))
			return c;
	}
	return NULL;
}

static void t6030_pmp_idle(struct apple_sleep *s)
{
	struct t6030_sleep *t = t6030_state(s);
	struct apple_sleep_coproc *c = t6030_pmp_coproc();
	void __iomem *mb = t->pmp_mbox;
	int ret;

	/* A running PMP was idled by its own driver's stop, with macOS's two messages. */
	if (c) {
		t->pmp_idle_ret = c->stopped ? c->stop_ret : -ESRCH;
		return;
	}
	t->pmp_idle_ret = -ENODEV;
	if (!mb || !sleep_ps_on(s, T6030_PS_PMP))
		return;
	ret = t6030_pmp_mgmt(mb, T6030_RTKIT_SET_AP_PWR, T6030_RTKIT_SET_AP_PWR, &t->pmp_ack[0]);
	if (!ret)
		ret = t6030_pmp_mgmt(mb, T6030_RTKIT_SET_IOP_PWR, T6030_RTKIT_SET_IOP_PWR_ACK,
				     &t->pmp_ack[1]);
	writel(readl(mb + T6030_MBOX_I2A_CONTROL) & ~T6030_MBOX_I2A_IRQ,
	       mb + T6030_MBOX_I2A_CONTROL);
	readl(mb);
	t->pmp_idle_ret = ret;
}

static void t6030_mtp_dart_quiet(struct apple_sleep *s)
{
	void __iomem *d = t6030_state(s)->mtp_dart;

	if (!d)
		return;
	writel(0xffffffff, d + T6030_DART_ERROR_MASK);
	readl(d + T6030_DART_ERROR_MASK);
	readl(d + T6030_DART_ERROR);
	writel(T6030_DART_ERROR_MACOS, d + T6030_DART_ERROR);
	writel(0xffffffff, d + T6030_DART_704);
	readl(d + T6030_DART_704);
}

/*
 * macOS 27.2's entry on J516s, every run (ref/t6030-macos-s2r-fixed-sequence.tsv), before its last
 * 30 ms: at -4.44 s nub-spmi-a0's fourth ack word gets bit 1; at -4.43 s, with DISPEXT1's words
 * going off, the power gate at +0x16ec goes from 0 to 3 (back to 0 at 59.6 ms into the wake); at
 * -3.44 s SPI2's word is lowered. In its last 30 ms, after the PMU's 0xf801, I2C6's word is
 * lowered (-27.4 ms). Under Linux DISPEXT1, SPI2 and I2C6 are already off; the gate isn't set.
 */
#define T6030_PWRGATE_DISPEXT1		0x16ec
#define T6030_PWRGATE_DISPEXT1_OFF	3
#define T6030_PS_SPI2			0x270
#define T6030_PS_I2C6			0x220
#define T6030_SPMI_A0_ENTRY_ACK3	BIT(1)

static void t6030_sleep_down(struct apple_sleep *s);

static void t6030_sleep_late_entry(struct apple_sleep *s)
{
	struct t6030_sleep *t = t6030_state(s);

	writel(T6030_SPMI_A0_ENTRY_ACK3, s->spmi_a0 + T6030_SPMI_ACK(3));
	t->gate_dispext1_saved = readl(s->pwrgate + T6030_PWRGATE_DISPEXT1);
	t->gate_dispext1_staged = true;
	writel(T6030_PWRGATE_DISPEXT1_OFF, s->pwrgate + T6030_PWRGATE_DISPEXT1);
	sleep_ps_lower(s, T6030_PS_SPI2);
}

/*
 * DIAGNOSTIC: the memory side in macOS's configured state. Under Linux on J516s the AMCC's SLC way
 * words, way masks and plane stage words, the DCS's +0x580..0x63c words, the AMCC planes' +0x27000
 * words read 0, and the DCS channels' +0x18 reads 0x70a0100: iBoot leaves them so. macOS 27.2 runs
 * with them configured: its entry clears them (apple,seq-commit copies that) and its wake sets
 * them again. These are its wake's writes, in its order, every run alike, 9.36 to 10.90 ms after
 * the reset (ref/t6030-macos-s2r-fixed-sequence.tsv): each AMCC0 plane's way words then that
 * way's mask bit; each AMCC plane's stage words, 0 to 1 to 3; the DCS's words, +0x580 = 0x71 and
 * +0x584's bits up to 0x1ff, then +0x588 = 0x100, 0x1, 0x1001, 0x11; the planes' +0x27000 words;
 * and each DCS channel's +0x18 = 0x70a1100, the value macOS reads back and writes at its wake
 * (wake-full-1: all twelve). The SoC perf word gets bits 15:12 = 4, as macOS's reads hold it
 * (0x4222 against Linux's 0x1222), read-modify-write as macOS's -2.68 ms step.
 *
 * apple_sleep.mem_config=1 does all of it once, at run time, and reports what reads back. Its
 * tail, from the +0x27000 words, is done again at the wake.
 */
struct t6030_mem_write {
	u64 pa;
	u8 width;
	u64 val;
};

static const struct t6030_mem_write t6030_mem_config[] = {
	{ 0x220024010ULL, 64, 0x110000100501000ULL },
	{ 0x220024018ULL, 64, 0xff000000000ULL },
	{ 0x220024000ULL, 64, 0x1ULL },
	{ 0x220024020ULL, 64, 0x210000100501000ULL },
	{ 0x220024028ULL, 64, 0xff000000000ULL },
	{ 0x220024000ULL, 64, 0x3ULL },
	{ 0x220024030ULL, 64, 0x110000000500000ULL },
	{ 0x220024038ULL, 64, 0xff000000000ULL },
	{ 0x220024000ULL, 64, 0x7ULL },
	{ 0x220024040ULL, 64, 0x210000000500000ULL },
	{ 0x220024048ULL, 64, 0xff000000000ULL },
	{ 0x220024000ULL, 64, 0xfULL },
	{ 0x220024050ULL, 64, 0x11000311492a100ULL },
	{ 0x220024058ULL, 64, 0xff000000000ULL },
	{ 0x220024000ULL, 64, 0x1fULL },
	{ 0x220024060ULL, 64, 0x21000311492a100ULL },
	{ 0x220024068ULL, 64, 0xff000000000ULL },
	{ 0x220024000ULL, 64, 0x3fULL },
	{ 0x220024070ULL, 64, 0x11000121c4121c0ULL },
	{ 0x220024078ULL, 64, 0xff000000000ULL },
	{ 0x220024000ULL, 64, 0x7fULL },
	{ 0x220024080ULL, 64, 0x21000121c4121c0ULL },
	{ 0x220024088ULL, 64, 0xff000000000ULL },
	{ 0x220024000ULL, 64, 0xffULL },
	{ 0x220024090ULL, 64, 0x110001129b11280ULL },
	{ 0x220024098ULL, 64, 0xff000000000ULL },
	{ 0x220024000ULL, 64, 0x1ffULL },
	{ 0x2200240a0ULL, 64, 0x210001129b11280ULL },
	{ 0x2200240a8ULL, 64, 0xff000000000ULL },
	{ 0x220024000ULL, 64, 0x3ffULL },
	{ 0x2200240b0ULL, 64, 0x110001048310480ULL },
	{ 0x2200240b8ULL, 64, 0xff000000000ULL },
	{ 0x220024000ULL, 64, 0x7ffULL },
	{ 0x2200240c0ULL, 64, 0x210001048310480ULL },
	{ 0x2200240c8ULL, 64, 0xff000000000ULL },
	{ 0x220024000ULL, 64, 0xfffULL },
	{ 0x2200240d0ULL, 64, 0x110001263c12600ULL },
	{ 0x2200240d8ULL, 64, 0xff000000000ULL },
	{ 0x220024000ULL, 64, 0x1fffULL },
	{ 0x2200240e0ULL, 64, 0x210001263c12600ULL },
	{ 0x2200240e8ULL, 64, 0xff000000000ULL },
	{ 0x220024000ULL, 64, 0x3fffULL },
	{ 0x2200240f0ULL, 64, 0x11000104c5104c0ULL },
	{ 0x2200240f8ULL, 64, 0xff000000000ULL },
	{ 0x220024000ULL, 64, 0x7fffULL },
	{ 0x220024100ULL, 64, 0x21000104c5104c0ULL },
	{ 0x220024108ULL, 64, 0xff000000000ULL },
	{ 0x220024000ULL, 64, 0xffffULL },
	{ 0x220064010ULL, 64, 0x11000106c9106c0ULL },
	{ 0x220064018ULL, 64, 0xff000000000ULL },
	{ 0x220064000ULL, 64, 0x1ULL },
	{ 0x220064020ULL, 64, 0x21000106c9106c0ULL },
	{ 0x220064028ULL, 64, 0xff000000000ULL },
	{ 0x220064000ULL, 64, 0x3ULL },
	{ 0x220064030ULL, 64, 0x110001018410180ULL },
	{ 0x220064038ULL, 64, 0xff000000000ULL },
	{ 0x220064000ULL, 64, 0x7ULL },
	{ 0x220064040ULL, 64, 0x210001018410180ULL },
	{ 0x220064048ULL, 64, 0xff000000000ULL },
	{ 0x220064000ULL, 64, 0xfULL },
	{ 0x220064050ULL, 64, 0x110001131411300ULL },
	{ 0x220064058ULL, 64, 0xff000000000ULL },
	{ 0x220064000ULL, 64, 0x1fULL },
	{ 0x220064060ULL, 64, 0x210001131411300ULL },
	{ 0x220064068ULL, 64, 0xff000000000ULL },
	{ 0x220064000ULL, 64, 0x3fULL },
	{ 0x220064070ULL, 64, 0x110001135411340ULL },
	{ 0x220064078ULL, 64, 0xff000000000ULL },
	{ 0x220064000ULL, 64, 0x7fULL },
	{ 0x220064080ULL, 64, 0x210001135411340ULL },
	{ 0x220064088ULL, 64, 0xff000000000ULL },
	{ 0x220064000ULL, 64, 0xffULL },
	{ 0x220064090ULL, 64, 0x110001371e13700ULL },
	{ 0x220064098ULL, 64, 0xff000000000ULL },
	{ 0x220064000ULL, 64, 0x1ffULL },
	{ 0x2200640a0ULL, 64, 0x210001371e13700ULL },
	{ 0x2200640a8ULL, 64, 0xff000000000ULL },
	{ 0x220064000ULL, 64, 0x3ffULL },
	{ 0x2200640b0ULL, 64, 0x110001375e13740ULL },
	{ 0x2200640b8ULL, 64, 0xff000000000ULL },
	{ 0x220064000ULL, 64, 0x7ffULL },
	{ 0x2200640c0ULL, 64, 0x210001375e13740ULL },
	{ 0x2200640c8ULL, 64, 0xff000000000ULL },
	{ 0x220064000ULL, 64, 0xfffULL },
	{ 0x2200640d0ULL, 64, 0x110001379e13780ULL },
	{ 0x2200640d8ULL, 64, 0xff000000000ULL },
	{ 0x220064000ULL, 64, 0x1fffULL },
	{ 0x2200640e0ULL, 64, 0x210001379e13780ULL },
	{ 0x2200640e8ULL, 64, 0xff000000000ULL },
	{ 0x220064000ULL, 64, 0x3fffULL },
	{ 0x2200640f0ULL, 64, 0x11000137de137c0ULL },
	{ 0x2200640f8ULL, 64, 0xff000000000ULL },
	{ 0x220064000ULL, 64, 0x7fffULL },
	{ 0x220064100ULL, 64, 0x21000137de137c0ULL },
	{ 0x220064108ULL, 64, 0xff000000000ULL },
	{ 0x220064000ULL, 64, 0xffffULL },
	{ 0x2200a4010ULL, 64, 0x110001359713580ULL },
	{ 0x2200a4018ULL, 64, 0xff000000000ULL },
	{ 0x2200a4000ULL, 64, 0x1ULL },
	{ 0x2200a4020ULL, 64, 0x210001359713580ULL },
	{ 0x2200a4028ULL, 64, 0xff000000000ULL },
	{ 0x2200a4000ULL, 64, 0x3ULL },
	{ 0x2200a4030ULL, 64, 0x110001364713640ULL },
	{ 0x2200a4038ULL, 64, 0xff000000000ULL },
	{ 0x2200a4000ULL, 64, 0x7ULL },
	{ 0x2200a4040ULL, 64, 0x210001364713640ULL },
	{ 0x2200a4048ULL, 64, 0xff000000000ULL },
	{ 0x2200a4000ULL, 64, 0xfULL },
	{ 0x2200a4050ULL, 64, 0x11000102e9102c0ULL },
	{ 0x2200a4058ULL, 64, 0xff000000000ULL },
	{ 0x2200a4000ULL, 64, 0x1fULL },
	{ 0x2200a4060ULL, 64, 0x21000102e9102c0ULL },
	{ 0x2200a4068ULL, 64, 0xff000000000ULL },
	{ 0x2200a4000ULL, 64, 0x3fULL },
	{ 0x2200a4070ULL, 64, 0x110001340413400ULL },
	{ 0x2200a4078ULL, 64, 0xff000000000ULL },
	{ 0x2200a4000ULL, 64, 0x7fULL },
	{ 0x2200a4080ULL, 64, 0x210001340413400ULL },
	{ 0x2200a4088ULL, 64, 0xff000000000ULL },
	{ 0x2200a4000ULL, 64, 0xffULL },
	{ 0x2200a4090ULL, 64, 0x110001251012500ULL },
	{ 0x2200a4098ULL, 64, 0xff000000000ULL },
	{ 0x2200a4000ULL, 64, 0x1ffULL },
	{ 0x2200a40a0ULL, 64, 0x210001251012500ULL },
	{ 0x2200a40a8ULL, 64, 0xff000000000ULL },
	{ 0x2200a4000ULL, 64, 0x3ffULL },
	{ 0x2200a40b0ULL, 64, 0x110001120811200ULL },
	{ 0x2200a40b8ULL, 64, 0xff000000000ULL },
	{ 0x2200a4000ULL, 64, 0x7ffULL },
	{ 0x2200a40c0ULL, 64, 0x210001120811200ULL },
	{ 0x2200a40c8ULL, 64, 0xff000000000ULL },
	{ 0x2200a4000ULL, 64, 0xfffULL },
	{ 0x2200a40d0ULL, 64, 0x11000126bf12680ULL },
	{ 0x2200a40d8ULL, 64, 0xff000000000ULL },
	{ 0x2200a4000ULL, 64, 0x1fffULL },
	{ 0x2200a40e0ULL, 64, 0x21000126bf12680ULL },
	{ 0x2200a40e8ULL, 64, 0xff000000000ULL },
	{ 0x2200a4000ULL, 64, 0x3fffULL },
	{ 0x220024858ULL, 64, 0x100000000500000ULL },
	{ 0x220024810ULL, 64, 0x103fff00000000ULL },
	{ 0x220024860ULL, 64, 0xff000000000ULL },
	{ 0x220024800ULL, 64, 0x1ULL },
	{ 0x220024868ULL, 64, 0x200000000500000ULL },
	{ 0x220024870ULL, 64, 0xff000000000ULL },
	{ 0x220024800ULL, 64, 0x3ULL },
	{ 0x220064858ULL, 64, 0x10000311492a100ULL },
	{ 0x220064810ULL, 64, 0x103fff00000000ULL },
	{ 0x220064860ULL, 64, 0xff000000000ULL },
	{ 0x220064800ULL, 64, 0x1ULL },
	{ 0x220064868ULL, 64, 0x20000311492a100ULL },
	{ 0x220064870ULL, 64, 0xff000000000ULL },
	{ 0x220064800ULL, 64, 0x3ULL },
	{ 0x2200a4858ULL, 64, 0x100003ffff00000ULL },
	{ 0x2200a4810ULL, 64, 0x103fff00000000ULL },
	{ 0x2200a4860ULL, 64, 0xff000000000ULL },
	{ 0x2200a4800ULL, 64, 0x1ULL },
	{ 0x2200a4868ULL, 64, 0x200003ffff00000ULL },
	{ 0x2200a4870ULL, 64, 0xff000000000ULL },
	{ 0x2200a4800ULL, 64, 0x3ULL },
	{ 0x222024858ULL, 64, 0x100001129b11280ULL },
	{ 0x222024810ULL, 64, 0x103fff00000000ULL },
	{ 0x222024860ULL, 64, 0xff000000000ULL },
	{ 0x222024800ULL, 64, 0x1ULL },
	{ 0x222024868ULL, 64, 0x200001129b11280ULL },
	{ 0x222024870ULL, 64, 0xff000000000ULL },
	{ 0x222024800ULL, 64, 0x3ULL },
	{ 0x222064858ULL, 64, 0x100001251012500ULL },
	{ 0x222064810ULL, 64, 0x103fff00000000ULL },
	{ 0x222064860ULL, 64, 0xff000000000ULL },
	{ 0x222064800ULL, 64, 0x1ULL },
	{ 0x222064868ULL, 64, 0x200001251012500ULL },
	{ 0x222064870ULL, 64, 0xff000000000ULL },
	{ 0x222064800ULL, 64, 0x3ULL },
	{ 0x2220a4858ULL, 64, 0x100001120811200ULL },
	{ 0x2220a4810ULL, 64, 0x103fff00000000ULL },
	{ 0x2220a4860ULL, 64, 0xff000000000ULL },
	{ 0x2220a4800ULL, 64, 0x1ULL },
	{ 0x2220a4868ULL, 64, 0x200001120811200ULL },
	{ 0x2220a4870ULL, 64, 0xff000000000ULL },
	{ 0x2220a4800ULL, 64, 0x3ULL },
	{ 0x22040058cULL, 32, 0x3ff001aULL },
	{ 0x220400590ULL, 32, 0x0ULL },
	{ 0x220400594ULL, 32, 0x33ULL },
	{ 0x220400598ULL, 32, 0x0ULL },
	{ 0x22040059cULL, 32, 0x0ULL },
	{ 0x220400580ULL, 32, 0x71ULL },
	{ 0x220400584ULL, 32, 0x1ULL },
	{ 0x2204005a0ULL, 32, 0x3ff0039ULL },
	{ 0x2204005a4ULL, 32, 0x0ULL },
	{ 0x2204005a8ULL, 32, 0x33ULL },
	{ 0x2204005acULL, 32, 0x0ULL },
	{ 0x2204005b0ULL, 32, 0x0ULL },
	{ 0x220400580ULL, 32, 0x71ULL },
	{ 0x220400584ULL, 32, 0x3ULL },
	{ 0x2204005b4ULL, 32, 0x3ff003aULL },
	{ 0x2204005b8ULL, 32, 0x0ULL },
	{ 0x2204005bcULL, 32, 0x33ULL },
	{ 0x2204005c0ULL, 32, 0x0ULL },
	{ 0x2204005c4ULL, 32, 0x0ULL },
	{ 0x220400580ULL, 32, 0x71ULL },
	{ 0x220400584ULL, 32, 0x7ULL },
	{ 0x2204005c8ULL, 32, 0x3ff0013ULL },
	{ 0x2204005ccULL, 32, 0x0ULL },
	{ 0x2204005d0ULL, 32, 0x0ULL },
	{ 0x2204005d4ULL, 32, 0x0ULL },
	{ 0x2204005d8ULL, 32, 0x0ULL },
	{ 0x220400580ULL, 32, 0x71ULL },
	{ 0x220400584ULL, 32, 0xfULL },
	{ 0x2204005dcULL, 32, 0x3ff0016ULL },
	{ 0x2204005e0ULL, 32, 0x0ULL },
	{ 0x2204005e4ULL, 32, 0x0ULL },
	{ 0x2204005e8ULL, 32, 0x0ULL },
	{ 0x2204005ecULL, 32, 0x0ULL },
	{ 0x220400580ULL, 32, 0x71ULL },
	{ 0x220400584ULL, 32, 0x1fULL },
	{ 0x2204005f0ULL, 32, 0x3ff001aULL },
	{ 0x2204005f4ULL, 32, 0x0ULL },
	{ 0x2204005f8ULL, 32, 0x31ULL },
	{ 0x2204005fcULL, 32, 0x0ULL },
	{ 0x220400600ULL, 32, 0x0ULL },
	{ 0x220400580ULL, 32, 0x71ULL },
	{ 0x220400584ULL, 32, 0x3fULL },
	{ 0x220400604ULL, 32, 0x3ff001aULL },
	{ 0x220400608ULL, 32, 0x0ULL },
	{ 0x22040060cULL, 32, 0x32ULL },
	{ 0x220400610ULL, 32, 0x0ULL },
	{ 0x220400614ULL, 32, 0x0ULL },
	{ 0x220400580ULL, 32, 0x71ULL },
	{ 0x220400584ULL, 32, 0x7fULL },
	{ 0x220400618ULL, 32, 0x3ff0012ULL },
	{ 0x22040061cULL, 32, 0x0ULL },
	{ 0x220400620ULL, 32, 0x33ULL },
	{ 0x220400624ULL, 32, 0x0ULL },
	{ 0x220400628ULL, 32, 0x0ULL },
	{ 0x220400580ULL, 32, 0x71ULL },
	{ 0x220400584ULL, 32, 0xffULL },
	{ 0x22040062cULL, 32, 0x3ff0014ULL },
	{ 0x220400630ULL, 32, 0x0ULL },
	{ 0x220400634ULL, 32, 0x33ULL },
	{ 0x220400638ULL, 32, 0x0ULL },
	{ 0x22040063cULL, 32, 0x0ULL },
	{ 0x220400580ULL, 32, 0x71ULL },
	{ 0x220400584ULL, 32, 0x1ffULL },
	{ 0x220400588ULL, 32, 0x100ULL },
	{ 0x220400588ULL, 32, 0x1ULL },
	{ 0x220400588ULL, 32, 0x1001ULL },
	{ 0x220400588ULL, 32, 0x11ULL },
	{ 0x220027018ULL, 64, 0x10000000ULL },
	{ 0x220027008ULL, 64, 0x700ULL },
	{ 0x220027000ULL, 64, 0x11ULL },
	{ 0x220067008ULL, 64, 0x708ULL },
	{ 0x220067000ULL, 64, 0x1ULL },
	{ 0x2200a7008ULL, 64, 0x710ULL },
	{ 0x2200a7000ULL, 64, 0x1ULL },
	{ 0x222027008ULL, 64, 0x718ULL },
	{ 0x222027000ULL, 64, 0x1ULL },
	{ 0x220400018ULL, 32, 0x70a1100ULL },
	{ 0x220600018ULL, 32, 0x70a1100ULL },
	{ 0x220800018ULL, 32, 0x70a1100ULL },
	{ 0x220a00018ULL, 32, 0x70a1100ULL },
	{ 0x220c00018ULL, 32, 0x70a1100ULL },
	{ 0x220e00018ULL, 32, 0x70a1100ULL },
	{ 0x222400018ULL, 32, 0x70a1100ULL },
	{ 0x222600018ULL, 32, 0x70a1100ULL },
	{ 0x222800018ULL, 32, 0x70a1100ULL },
	{ 0x222a00018ULL, 32, 0x70a1100ULL },
	{ 0x222c00018ULL, 32, 0x70a1100ULL },
	{ 0x222e00018ULL, 32, 0x70a1100ULL },
};

/* The +0x27000 words and the DCS channels' +0x18, which the wake's apple,seq-wake lacks */
#define T6030_MEM_CONFIG_TAIL		247
#define T6030_MEM_PAGE			SZ_16K
#define T6030_MEM_PAGES			24
#define T6030_PERF_SOC_HI		GENMASK(15, 12)
#define T6030_PERF_SOC_HI_MACOS		4

/* A register of the table: its page mapped once, in process context (@map), kept for the wake */
static void __iomem *t6030_mem_reg(struct t6030_sleep *t, u64 pa, bool map)
{
	u64 p = pa & ~(u64)(T6030_MEM_PAGE - 1);
	int i;

	for (i = 0; i < T6030_MEM_PAGES && t->mem_va[i]; i++)
		if (t->mem_page[i] == p)
			return t->mem_va[i] + (pa - p);
	if (!map || i == T6030_MEM_PAGES)
		return NULL;
	t->mem_va[i] = ioremap_np(p, T6030_MEM_PAGE);
	t->mem_page[i] = p;
	return t->mem_va[i] ? t->mem_va[i] + (pa - p) : NULL;
}

/* Every page the table writes, mapped up front: 0, or -ENOMEM with nothing written */
static int t6030_mem_config_map(struct t6030_sleep *t)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(t6030_mem_config); i++)
		if (!t6030_mem_reg(t, t6030_mem_config[i].pa, true))
			return -ENOMEM;
	return 0;
}

static void t6030_mem_config_run(struct t6030_sleep *t, unsigned int from, unsigned int to)
{
	unsigned int i;
	void __iomem *r;

	for (i = from; i < to; i++) {
		const struct t6030_mem_write *w = &t6030_mem_config[i];

		r = t6030_mem_reg(t, w->pa, false);
		if (!r)
			continue;
		if (w->width == 64)
			writeq(w->val, r);
		else
			writel(w->val, r);
	}
}

static void t6030_mem_perf(struct apple_sleep *s)
{
	u32 v = readl(s->clk_perf + CLK_PERF_SOC);

	writel((v & ~T6030_PERF_SOC_HI) | FIELD_PREP(T6030_PERF_SOC_HI, T6030_PERF_SOC_HI_MACOS),
	       s->clk_perf + CLK_PERF_SOC);
	readl(s->clk_perf + CLK_PERF_SOC);
}

static u64 t6030_mem_read(u64 pa, u8 width)
{
	void __iomem *r = ioremap_np(pa & ~(u64)(T6030_MEM_PAGE - 1), T6030_MEM_PAGE);
	u64 v;

	if (!r)
		return ~0ULL;
	v = width == 64 ? readq(r + (pa & (T6030_MEM_PAGE - 1))) :
			  readl(r + (pa & (T6030_MEM_PAGE - 1)));
	iounmap(r);
	return v;
}

static void t6030_deep_wfi_arm(void);

static int t6030_mem_config_apply(struct apple_sleep *s)
{
	struct t6030_sleep *t = t6030_state(s);
	u32 perf0;
	int ret;

	perf0 = readl(s->clk_perf + CLK_PERF_SOC);
	ret = t6030_mem_config_map(t);
	if (!ret) {
		t6030_mem_config_run(t, 0, ARRAY_SIZE(t6030_mem_config));
		t6030_mem_perf(s);
		t->mem_configured = true;
	}
	dev_info(s->dev, "mem config: %d; ways %#llx %#llx %#llx; stages %#llx %#llx %#llx / %#llx %#llx %#llx; DCS +0x584 %#llx +0x588 %#llx; +0x27000 %#llx %#llx %#llx %#llx; ch +0x18 %#llx %#llx; perf %#x -> %#x\n",
		 ret, t6030_mem_read(0x220024000, 64), t6030_mem_read(0x220064000, 64),
		 t6030_mem_read(0x2200a4000, 64), t6030_mem_read(0x220024800, 64),
		 t6030_mem_read(0x220064800, 64), t6030_mem_read(0x2200a4800, 64),
		 t6030_mem_read(0x222024800, 64), t6030_mem_read(0x222064800, 64),
		 t6030_mem_read(0x2220a4800, 64), t6030_mem_read(0x220400584, 32),
		 t6030_mem_read(0x220400588, 32), t6030_mem_read(0x220027000, 64),
		 t6030_mem_read(0x220067000, 64), t6030_mem_read(0x2200a7000, 64),
		 t6030_mem_read(0x222027000, 64), t6030_mem_read(0x220400018, 32),
		 t6030_mem_read(0x222e00018, 32), perf0, readl(s->clk_perf + CLK_PERF_SOC));
	return ret;
}

static int t6030_mem_config_set(const char *val, const struct kernel_param *kp)
{
	struct apple_sleep *s = apple_sleep;
	bool on;
	int ret = kstrtobool(val, &on);

	if (ret || !on)
		return ret;
	if (!s || s->soc->ops->deep_wfi_arm != t6030_deep_wfi_arm)
		return -ENODEV;
	return t6030_mem_config_apply(s);
}

/*
 * DIAGNOSTIC: the same at the driver's probe, before the PMP report starts the PMP, as XNU
 * programs the AMCC ways and the DCS tables 215-216 ms into its boot (macos-boot-memside-5),
 * long before its PMP runs. The PMP's DCS/fabric states may need them to switch at all.
 */
static bool t6030_mem_config_boot;
module_param_named(mem_config_boot, t6030_mem_config_boot, bool, 0444);
MODULE_PARM_DESC(mem_config_boot, "T6030 DIAGNOSTIC: mem_config at probe, before the PMP starts");

static bool t6030_mem_config_boot_on(void)
{
	return READ_ONCE(t6030_mem_config_boot);
}

static const struct kernel_param_ops t6030_mem_config_ops = {
	.set = t6030_mem_config_set,
};
module_param_cb(mem_config, &t6030_mem_config_ops, NULL, 0200);
MODULE_PARM_DESC(mem_config, "T6030 DIAGNOSTIC: bring the AMCC, DCS and SoC perf word to macOS's configured state (traced wake writes)");

static void t6030_sleep_prepare_regs(struct apple_sleep *s)
{
	if (READ_ONCE(t6030_diag_mask_steps) && t6030_ptd_ctrl)
		writel(T6030_PTD_SLEEP_CTRL_ENTRY, t6030_ptd_ctrl + T6030_PTD_SLEEP_CTRL);
	t6030_sleep_late_entry(s);
	/*
	 * The RAM walk here, seconds before the commit: the steps from .after_ans on are macOS's
	 * last 30 ms, and the commit's 1.8 ms tail still cleans what they write.
	 */
	t6030_sleep_clean_ram();
}

/*
 * macOS 27.2's late group on J516s, after ANS's stop and the 4.1 s from pmVC, 28.6 to 23.9 ms
 * before its commit and 15.2 ms before slpa (run 3 and mac-stop-1000-1, PLAN 29.13-29.14): the
 * PTD's no-demand votes (pmgr[40]); the PMU burst with the RTC read and 0xf801; WD0's and WD1's
 * counts to 0; I2C6's word; the notify masks; then SIO's and the PMP's AP idle (the next
 * coprocessor stops and .commit_notice). Linux took it right after pmVC, 3.8 s before its commit,
 * so the coprocessors heard AP idle seconds early. Wait for macOS's slot first.
 */
#define T6030_LATE_TO_SLPA_US		15200

static void t6030_sleep_after_ans(struct apple_sleep *s)
{
	struct t6030_sleep *t = t6030_state(s);

	if (READ_ONCE(t6030_diag_mask_steps) && t6030_ptd_update) {
		u64 ack = 0;

		writeq(T6030_MASK_AT_PMVC, t6030_ptd_update + T6030_PTD_REQUEST);
		if (t6030_ptd_read &&
		    readq_poll_timeout_atomic(t6030_ptd_read + T6030_PTD_ACTUAL, ack,
					      ack == T6030_MASK_AT_PMVC, 100,
					      T6030_PTD_ACK_US))
			dev_warn(s->dev, "PTD request %#llx not acknowledged (ack %#llx)\n",
				 T6030_MASK_AT_PMVC, ack);
		apple_sleep_smc_until_before_slpa(s, T6030_MASK_LATE_TO_SLPA_US);
		writeq(T6030_MASK_LATE, t6030_ptd_update + T6030_PTD_REQUEST);
	}
	if (READ_ONCE(t6030_last_writes) && t6030_ptd_update) {
		apple_sleep_smc_until_before_slpa(s, T6030_LAST_DVFS0_TO_SLPA_US);
		writeq(T6030_LAST_DVFS0_VOTE, t6030_ptd_update + T6030_LAST_DVFS0);
	}
	apple_sleep_smc_until_before_slpa(s, T6030_LATE_TO_SLPA_US);
	if (sleep_seq_run_strict(t->blk, &t->prepare))
		t6030_sleep_failed("apple,seq-prepare");
	t6030_alarm_snapshot(s, readl(s->spmi + SPMI_NOTIFY_ENABLE));
	if (READ_ONCE(t6030_last_writes) && t->alarm_valid)
		t6030_last_alarm_disarm(s);
	writel(0, s->wdt + WDT_WD0_COUNT);
	writel(0, s->wdt + WDT_WD1_COUNT);
	sleep_pmu_write(s, s->soc->pmu_sleep_reg, s->soc->pmu_sleep_val);
	if (READ_ONCE(t6030_last_writes)) {
		t6030_last_pmu_reads(s);
		/* macOS's one write, read back as off (0x200) */
		writel(T6030_LAST_I2C6, s->ps + T6030_PS_I2C6);
		readl(s->ps + T6030_PS_I2C6);
	} else {
		sleep_ps_lower(s, T6030_PS_I2C6);
	}
	t->notify_saved[0] = readl(s->spmi_a0 + SPMI_NOTIFY_ENABLE);
	t->notify_saved[1] = readl(s->spmi + SPMI_NOTIFY_ENABLE);
	t->notify_staged = true;
	t6030_wake_masks_set(s);
	writel(s->soc->spmi_notify_sleep, s->spmi_a0 + SPMI_NOTIFY_ENABLE);
	writel(s->soc->spmi_notify_sleep, s->spmi + SPMI_NOTIFY_ENABLE);
	t6030_spmi1_entry(s);
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
		writel(t6030_lowered_macos[i][0], s->ps + t6030_lowered[i]);
		writel(t6030_lowered_macos[i][1], s->ps + t6030_lowered[i]);
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
 * Lower the 23 "ps" and 2 "ps-aon" domains that macOS already has off long before its -30 ms
 * sleep window (ATC0-3_COMMON, LW31/LW30/LS3/IOA3, MSG, DPA1-4, FPWM1/2, PMS_FPWM0-4, AFT0,
 * I2C5, PMGR_SOC_OCLA, NUB_SPI0, NUB_OCLA), so the commit sees the same off state as macOS (25
 * words on instead of 48), and restore every word on wake. Run from .commit_regs, after the
 * coprocessors' stops.
 */
/*
 * Off by default (MEAS J516S, 11-26): from the late group the PMP's commit stop timed out, and
 * from .commit_regs the entry stopped there, before the commit word. One of the 25 words is to blame.
 */
static bool t6030_lower_macos_off;
module_param_named(lower_macos_off, t6030_lower_macos_off, bool, 0644);
MODULE_PARM_DESC(lower_macos_off, "T6030: lower the macOS-off power domains (ATC0-3_COMMON, fabric, DPA1-4) from .commit_regs (default: off)");

static void t6030_sleep_unstage(struct apple_sleep *s, u32 offset, u32 before);

static void t6030_sleep_down(struct apple_sleep *s)
{
	struct t6030_sleep *t = t6030_state(s);
	unsigned int i;
	u32 v, now;

	if (!READ_ONCE(t6030_lower_macos_off))
		return;
	t->down_stuck = 0;
	for (i = 0; i < ARRAY_SIZE(t6030_macos_down); i++) {
		t->down_saved[i] = readl(s->ps + t6030_macos_down[i]);
		if (!FIELD_GET(PS_ACTUAL, t->down_saved[i]))
			continue;
		sleep_ps_lower(s, t6030_macos_down[i]);
		if (sleep_ps_wait_off(s, t6030_macos_down[i]))
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
/*
 * macOS 27.2 halts ANS's CPU but keeps ANS and the storage PCIe domains powered through the commit
 * on J516S (both final-WFI snapshots: ANS 0x1f0023ff, APCIE_SYS_ST 0x1f0003ff, APCIE_ST 0x2ff);
 * Linux's stop lowers all three. Raised again here, after every coprocessor's stop, parents
 * first; ANS's CPU comes up halted with its domain. Off: Linux's state at the commit as before.
 */
static bool t6030_storage_on;
module_param_named(storage_on, t6030_storage_on, bool, 0644);
MODULE_PARM_DESC(storage_on, "T6030: ANS and the storage PCIe domains powered at the commit, as macOS (ANS's CPU halted)");

/*
 * DIAGNOSTIC (T6030 sleep bring-up): the "ps" words lane A's final-WFI comparison of macOS 27.2
 * with Linux 313ee6fb (t6030-ps-at-final-wfi-macos-vs-313ee6fb) found in another state, set as
 * macOS has them, from .commit_regs after the coprocessors' stops:
 *  - off on macOS, on in Linux: MSG, DPA1-4, ATC1-3_COMMON (and NUB_SPI0, NUB_OCLA in ps-aon);
 *  - off on macOS, at target 4 in Linux: PMS_FPWM0-4, PMGR_SOC_OCLA, AFT0, FPWM1-2, I2C5, MCA3;
 *  - powered at macOS's final WFI (no AUTO_ENABLE), off or at 4 in Linux: DWI, GPIO, I2C0/4/7/8,
 *    SPI0, UART1-6.
 * LW31/LW30/LS3/IOA3, which Neon's list lowered too, are on at macOS's final WFI and left alone.
 * Each wait is bounded; a word that doesn't get there is counted in PMU scratch +2 (0x80 | n).
 */
static bool t6030_wfi_match;
module_param_named(wfi_match, t6030_wfi_match, bool, 0644);
MODULE_PARM_DESC(wfi_match, "T6030 DIAGNOSTIC: set the power words lane A's final-WFI diff found apart to macOS's state at the commit");

static const u32 t6030_wfi_off[] = {
	0x108,					/* MSG */
	0x2e8, 0x2f0, 0x2f8, 0x300,		/* DPA1-4 */
	0x4e8, 0x4f0, 0x4f8,			/* ATC1-3_COMMON */
	0x138, 0x140, 0x148, 0x150, 0x158,	/* PMS_FPWM0-4 */
	0x178, 0x190,				/* PMGR_SOC_OCLA, AFT0 */
	0x1e0, 0x1e8, 0x218, 0x398,		/* FPWM1-2, I2C5, MCA3 */
};

static const u32 t6030_wfi_off_aon[] = { 0x88, 0x90 };	/* NUB_SPI0, NUB_OCLA */

static const u32 t6030_wfi_on[] = {
	0x118, 0x120,				/* DWI, GPIO */
	0x1f0, 0x210, 0x228, 0x230,		/* I2C0, I2C4, I2C7, I2C8 */
	0x260,					/* SPI0 */
	0x2a8, 0x2b0, 0x2b8, 0x2c0, 0x2c8, 0x2d0, /* UART1-6 */
};

#define T6030_PS_GPU_GPX		0x0
#define T6030_PS_MIN			GENMASK(19, 16)
#define T6030_GPX_MIN_MACOS		4

static void t6030_wfi_match_at_commit(struct apple_sleep *s)
{
	struct t6030_sleep *t = t6030_state(s);
	unsigned int i, stuck = 0;
	u32 v, now;

	if (!READ_ONCE(t6030_wfi_match))
		return;
	for (i = 0; i < ARRAY_SIZE(t6030_wfi_off); i++) {
		if (!(readl(s->ps + t6030_wfi_off[i]) & (PS_ACTUAL | PS_TARGET)))
			continue;
		sleep_ps_lower(s, t6030_wfi_off[i]);
		if (sleep_ps_wait_off(s, t6030_wfi_off[i]))
			stuck++;
	}
	for (i = 0; i < ARRAY_SIZE(t6030_wfi_off_aon); i++) {
		void __iomem *reg = s->ps_aon + t6030_wfi_off_aon[i];

		v = readl(reg);
		if (!FIELD_GET(PS_ACTUAL, v))
			continue;
		writel(v & ~(PS_AUTO_ENABLE | PS_FLAGS), reg);
		writel((v & ~(PS_AUTO_ENABLE | PS_FLAGS | PS_TARGET)) | PS_FLAGS, reg);
		if (readl_poll_timeout_atomic(reg, now, !FIELD_GET(PS_ACTUAL, now), 1,
					      PS_OFF_TIMEOUT_US))
			stuck++;
	}
	for (i = 0; i < ARRAY_SIZE(t6030_wfi_on); i++) {
		if (FIELD_GET(PS_ACTUAL, readl(s->ps + t6030_wfi_on[i])) == 0xf)
			continue;
		if (sleep_ps_raise_start(s, t6030_wfi_on[i]))
			stuck++;
	}
	/* NUB_FABRIC: on, under AUTO_ENABLE (0x1f0003ff at macOS's final WFI; Linux 0x0f0000ff) */
	v = readl(s->ps_aon + T6030_AON_NUB_FABRIC);
	if (FIELD_GET(PS_ACTUAL, v) == 0xf && !(v & PS_AUTO_ENABLE))
		writel((v & ~PS_FLAGS) | PS_AUTO_ENABLE, s->ps_aon + T6030_AON_NUB_FABRIC);
	/*
	 * GPX (ps-gpu +0x0): its minimum state 4 (bits 19:16), the one real value left between
	 * Linux's words and macOS's at the final WFI (0x1f0402ff there; Linux 0x1f0002ff).
	 */
	if (t->blk[T6030_BLK_PS_GPU]) {
		v = readl(t->blk[T6030_BLK_PS_GPU] + T6030_PS_GPU_GPX);
		if (FIELD_GET(T6030_PS_MIN, v) != T6030_GPX_MIN_MACOS)
			writel((v & ~(PS_FLAGS | T6030_PS_MIN)) |
			       FIELD_PREP(T6030_PS_MIN, T6030_GPX_MIN_MACOS),
			       t->blk[T6030_BLK_PS_GPU] + T6030_PS_GPU_GPX);
	}
	sleep_note_at(s, 2, 0x80 | min(stuck, 0x7fU));
}

/* PMU scratch +4 before each raise (0x11-0x13) and after (0x14); +3 the word's state if one fails */
static void t6030_storage_on_at_commit(struct apple_sleep *s)
{
	static const u32 words[] = { T6030_PS_APCIE_SYS_ST, T6030_PS_APCIE_ST, T6030_PS_ANS };
	unsigned int i;
	int ret;

	if (!READ_ONCE(t6030_storage_on))
		return;
	for (i = 0; i < ARRAY_SIZE(words); i++) {
		sleep_note_at(s, 4, 0x11 + i);
		ret = words[i] == T6030_PS_APCIE_ST ? sleep_ps_raise_start(s, words[i]) :
						      sleep_ps_raise(s, words[i]);
		if (ret) {
			sleep_note_at(s, 3, readl(s->ps + words[i]) & 0xff);
			t6030_sleep_failed("storage power words on");
		}
	}
	sleep_note_at(s, 4, 0x14);
}

static void t6030_sleep_commit_regs(struct apple_sleep *s)
{
	struct t6030_sleep *t = t6030_state(s);
	unsigned int i;

	/* A core that didn't take its stop would keep the engine waiting at the commit */
	if (s->cpus_stop_ret)
		t6030_sleep_failed("the other cores' stop");
	/*
	 * After the PMP's and SIO's stops: lowered in the late group, with the PMP still running,
	 * these words made the PMP's commit stop time out (-ETIMEDOUT, J516S).
	 */
	t6030_sleep_down(s);
	t6030_storage_on_at_commit(s);
	t6030_wfi_match_at_commit(s);
	t6030_gp_phy_park(s);
	if (t6030_sleep_lower(s, T6030_LOWER_PHY, ARRAY_SIZE(t6030_lowered)))
		t6030_sleep_failed("storage PHY power word");
	t6030_gp_clk_off(s);
	for (i = 0; i < ARRAY_SIZE(t6030_auto_off); i++)
		t->auto_saved[i] = readl(s->ps + t6030_auto_off[i]);
	t->auto_staged = true;
	sleep_seq_save(t->blk, &t->wake_early);
	sleep_seq_save(t->blk, &t->wake);
	t6030_cpm_entry();
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
/*
 * macOS 27.2's wake on J516s, 12.97-13.13 ms (ref/t6030-macos-wake-full.tsv), after the memory
 * side and before the cores' starts: on nub-spmi0 the latched RTC alarm, source 2, acked and the
 * first mask word set to 0x1000; on nub-spmi-a0 bit 25 of its third ack word acked and its third
 * mask word set to 0x80000000.
 */
#define T6030_SPMI_WAKE_ACK0		BIT(2)
#define T6030_SPMI_WAKE_MASK0		BIT(12)
#define T6030_SPMI_A0_WAKE_ACK2		BIT(25)
#define T6030_SPMI_A0_WAKE_MASK2	BIT(31)

static void t6030_spmi_wake_irqs(struct apple_sleep *s)
{
	writel(T6030_SPMI_WAKE_ACK0, s->spmi + T6030_SPMI_ACK(0));
	writel(T6030_SPMI_WAKE_MASK0, s->spmi + T6030_SPMI_MASK(0));
	writel(T6030_SPMI_A0_WAKE_ACK2, s->spmi_a0 + T6030_SPMI_ACK(2));
	writel(T6030_SPMI_A0_WAKE_MASK2, s->spmi_a0 + T6030_SPMI_MASK(2));
}

/*
 * macOS 27.2's wake on J516s, every run alike (ref/t6030-macos-s2r-gaps-sleepdriver.tsv), between
 * its power words going back on and its memory side, which Linux lacked:
 * - 4.97 ms: the GPU PMGR's AFR word, the target and then AUTO_ENABLE;
 * - 5.08 to 6.01 ms: the words the reset left on that were off at the commit, lowered with
 *   macOS's values, each read back (the storage PHY, the PMP and SIO's CPU among them come back
 *   later, as before the sleep);
 * - 6.29 to 9.11 ms: the GPU PMGR's +0x10000 block, which reads 0 after the reset, written
 *   word by word as macOS writes it.
 * The values are macOS's; the offsets are the sleep controller's "ps", "ps-aon" and "ps-gpu".
 */
struct t6030_wake_write {
	u32 offset;
	u32 val;
};

#define T6030_PS_GPU_AFR		0x100
#define T6030_PS_GPU_AFR_ON		0xf00000f
#define T6030_PS_GPU_AFR_AUTO		0x1f00000f

static void t6030_wake_afr_on(struct apple_sleep *s)
{
	void __iomem *gpu = t6030_state(s)->blk[T6030_BLK_PS_GPU];
	u32 val;

	if (!gpu)
		return;
	writel(T6030_PS_GPU_AFR_ON, gpu + T6030_PS_GPU_AFR);
	if (readl_poll_timeout_atomic(gpu + T6030_PS_GPU_AFR, val,
				      FIELD_GET(PS_ACTUAL, val) == 0xf, 1, 2000))
		dev_warn(s->dev, "T6030 wake: AFR power word not on: %#x\n", val);
	writel(T6030_PS_GPU_AFR_AUTO, gpu + T6030_PS_GPU_AFR);
	readl(gpu + T6030_PS_GPU_AFR);
}

static const struct t6030_wake_write t6030_wake_lowered[] = {
	{ 0x5b8, 0xf000340 },
	{ 0x4c8, 0xf0003f0 },
	{ 0x398, 0x340 },
	{ 0x390, 0x340 },
	{ 0x388, 0x340 },
	{ 0x308, 0x340 },
	{ 0x2d8, 0x340 },
	{ 0x298, 0x340 },
	{ 0x290, 0x340 },
	{ 0x288, 0x340 },
	{ 0x280, 0x340 },
	{ 0x278, 0x340 },
	{ 0x270, 0x340 },
	{ 0x268, 0x340 },
	{ 0x250, 0x340 },
	{ 0x220, 0x340 },
	{ 0x208, 0x340 },
	{ 0x200, 0x340 },
	{ 0x1f8, 0x340 },
	{ 0x1d8, 0x340 },
	{ 0x1d0, 0xf0003f0 },
	{ 0x180, 0x4000340 },
	{ 0x168, 0x3f0 },
	{ 0x160, 0x3f0 },
	{ T6030_AON(0xc0), 0x3f0 },
	{ T6030_AON(0xb8), 0x3f0 },
};

static const struct t6030_wake_write t6030_wake_gpu[] = {
	{ 0x10834, 0x0 },
	{ 0x10820, 0x4 },
	{ 0x10874, 0x1 },
	{ 0x10860, 0x4 },
	{ 0x10854, 0x1 },
	{ 0x10840, 0x4 },
	{ 0x10110, 0x0 },
	{ 0x10114, 0x0 },
	{ 0x10100, 0x4 },
	{ 0x10130, 0x0 },
	{ 0x10134, 0x0 },
	{ 0x10120, 0x4 },
	{ 0x10154, 0x1 },
	{ 0x10140, 0x4 },
	{ 0x10170, 0x0 },
	{ 0x10174, 0x0 },
	{ 0x10160, 0x4 },
	{ 0x10190, 0x0 },
	{ 0x10194, 0x0 },
	{ 0x10180, 0x4 },
	{ 0x101b0, 0x0 },
	{ 0x101b4, 0x0 },
	{ 0x101a0, 0x4 },
	{ 0x101d0, 0x0 },
	{ 0x101d4, 0x0 },
	{ 0x101c0, 0x4 },
	{ 0x106f4, 0x0 },
	{ 0x106e0, 0x4 },
	{ 0x10714, 0x0 },
	{ 0x10700, 0x4 },
	{ 0x10734, 0x0 },
	{ 0x10720, 0x4 },
	{ 0x10754, 0x0 },
	{ 0x10740, 0x4 },
	{ 0x10774, 0x1 },
	{ 0x10760, 0x4 },
	{ 0x10794, 0x0 },
	{ 0x10780, 0x4 },
	{ 0x107b4, 0x0 },
	{ 0x107a0, 0x4 },
	{ 0x107d4, 0x1 },
	{ 0x107c0, 0x4 },
	{ 0x107f4, 0x0 },
	{ 0x107e0, 0x4 },
	{ 0x10814, 0x0 },
	{ 0x10800, 0x4 },
	{ 0x101f4, 0x1 },
	{ 0x101e0, 0x4 },
	{ 0x10214, 0x0 },
	{ 0x10200, 0x4 },
	{ 0x10234, 0x0 },
	{ 0x10220, 0x4 },
	{ 0x10274, 0x1 },
	{ 0x10250, 0x0 },
	{ 0x10254, 0x0 },
	{ 0x10260, 0x4 },
	{ 0x10240, 0x5 },
	{ 0x102b0, 0x0 },
	{ 0x102b4, 0x0 },
	{ 0x10290, 0x0 },
	{ 0x10294, 0x0 },
	{ 0x102a0, 0x4 },
	{ 0x10280, 0x5 },
	{ 0x102f0, 0x0 },
	{ 0x102f4, 0x0 },
	{ 0x102d0, 0x0 },
	{ 0x102d4, 0x0 },
	{ 0x102e0, 0x4 },
	{ 0x102c0, 0x5 },
	{ 0x10330, 0x0 },
	{ 0x10334, 0x0 },
	{ 0x10310, 0x0 },
	{ 0x10314, 0x0 },
	{ 0x10320, 0x4 },
	{ 0x10300, 0x5 },
	{ 0x10370, 0x0 },
	{ 0x10374, 0x0 },
	{ 0x10350, 0x0 },
	{ 0x10354, 0x0 },
	{ 0x10360, 0x4 },
	{ 0x10340, 0x5 },
	{ 0x103b0, 0x0 },
	{ 0x103b4, 0x0 },
	{ 0x10390, 0x0 },
	{ 0x10394, 0x0 },
	{ 0x103a0, 0x4 },
	{ 0x10380, 0x5 },
	{ 0x103f0, 0x0 },
	{ 0x103f4, 0x0 },
	{ 0x103d0, 0x0 },
	{ 0x103d4, 0x0 },
	{ 0x103e0, 0x4 },
	{ 0x103c0, 0x5 },
	{ 0x10430, 0x0 },
	{ 0x10434, 0x0 },
	{ 0x10410, 0x0 },
	{ 0x10414, 0x0 },
	{ 0x10420, 0x4 },
	{ 0x10400, 0x5 },
	{ 0x10450, 0x0 },
	{ 0x10454, 0x0 },
	{ 0x10440, 0x4 },
	{ 0x10470, 0x0 },
	{ 0x10474, 0x0 },
	{ 0x10460, 0x4 },
	{ 0x10490, 0x0 },
	{ 0x10494, 0x0 },
	{ 0x10480, 0x4 },
	{ 0x104b0, 0x0 },
	{ 0x104b4, 0x0 },
	{ 0x104a0, 0x4 },
	{ 0x104d0, 0x0 },
	{ 0x104d4, 0x0 },
	{ 0x104c0, 0x4 },
	{ 0x104f0, 0x0 },
	{ 0x104f4, 0x0 },
	{ 0x104e0, 0x4 },
	{ 0x10510, 0x0 },
	{ 0x10514, 0x0 },
	{ 0x10500, 0x4 },
	{ 0x10530, 0x0 },
	{ 0x10534, 0x0 },
	{ 0x10520, 0x4 },
	{ 0x10550, 0x0 },
	{ 0x10554, 0x0 },
	{ 0x10540, 0x4 },
	{ 0x10570, 0x0 },
	{ 0x10574, 0x0 },
	{ 0x10560, 0x4 },
	{ 0x10590, 0x0 },
	{ 0x10594, 0x0 },
	{ 0x10580, 0x4 },
	{ 0x105b0, 0x0 },
	{ 0x105b4, 0x0 },
	{ 0x105a0, 0x4 },
	{ 0x105d0, 0x0 },
	{ 0x105d4, 0x0 },
	{ 0x105c0, 0x4 },
	{ 0x105f0, 0x0 },
	{ 0x105f4, 0x0 },
	{ 0x105e0, 0x4 },
	{ 0x10610, 0x0 },
	{ 0x10614, 0x0 },
	{ 0x10600, 0x4 },
	{ 0x10630, 0x0 },
	{ 0x10634, 0x0 },
	{ 0x10620, 0x4 },
	{ 0x10650, 0x0 },
	{ 0x10654, 0x0 },
	{ 0x10640, 0x4 },
	{ 0x10670, 0x0 },
	{ 0x10674, 0x0 },
	{ 0x10660, 0x4 },
	{ 0x10694, 0x1 },
	{ 0x10680, 0x4 },
	{ 0x106b0, 0x0 },
	{ 0x106b4, 0x0 },
	{ 0x106a0, 0x4 },
	{ 0x106d4, 0x1 },
	{ 0x106c0, 0x4 },
	{ 0x10890, 0x0 },
	{ 0x10894, 0x0 },
	{ 0x10880, 0x4 },
	{ 0x108b4, 0x1 },
	{ 0x108a0, 0x4 },
	{ 0x108d4, 0x1 },
	{ 0x108c0, 0x4 },
	{ 0x108f4, 0x1 },
	{ 0x108e0, 0x4 },
	{ 0x10910, 0x0 },
	{ 0x10914, 0x0 },
	{ 0x10900, 0x4 },
	{ 0x10000, 0x30001 },
	{ 0x14000, 0x1 },
	{ 0x14400, 0x1 },
	{ 0x14800, 0x1 },
	{ 0x14c00, 0x1 },
	{ 0x15000, 0x1 },
};

static void t6030_wake_gpu_writes(struct apple_sleep *s, const struct t6030_wake_write *w,
				  unsigned int n)
{
	void __iomem *gpu = t6030_state(s)->blk[T6030_BLK_PS_GPU];
	unsigned int i;

	for (i = 0; gpu && i < n; i++) {
		writel(w[i].val, gpu + w[i].offset);
		readl(gpu + w[i].offset);
	}
}

static void t6030_wake_lower(struct apple_sleep *s)
{
	unsigned int i;
	void __iomem *r;

	for (i = 0; i < ARRAY_SIZE(t6030_wake_lowered); i++) {
		u32 off = t6030_wake_lowered[i].offset;

		r = (off & SLEEP_PS_WAKE_AON) ? s->ps_aon + (off & ~SLEEP_PS_WAKE_AON) :
						 s->ps + off;
		writel(t6030_wake_lowered[i].val, r);
		readl(r);
	}
}

static void t6030_sleep_wake(struct apple_sleep *s)
{
	struct t6030_sleep *t = t6030_state(s);
	unsigned int i;

	sleep_note(s, 0x70);
	for (i = 0; i < T6030_WAKE_WORDS; i++) {
		t->wake_status[i] = readl(s->wake + T6030_WAKE_STATUS + 4 * i);
		if (i < ARRAY_SIZE(s->wake_status))
			s->wake_status[i] = t->wake_status[i];
	}
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
	t6030_wake_afr_on(s);
	t6030_wake_lower(s);
	t6030_wake_gpu_writes(s, t6030_wake_gpu, ARRAY_SIZE(t6030_wake_gpu));
	if (READ_ONCE(t6030_diag_mask_steps) && t6030_ptd_update) {
		if (t6030_ptd_read)
			readq(t6030_ptd_read + T6030_PTD_TGT_READ);
		writeq(T6030_MASK_LATE, t6030_ptd_update + T6030_PTD_REQUEST);
	}
	t6030_sleep_wake_check_ps(s, "before the memory side");
	sleep_note(s, 0x72);
	t6030_sleep_wake_seq(s, &t->wake, "apple,seq-wake");
	if (t->mem_configured) {
		t6030_mem_config_run(t, T6030_MEM_CONFIG_TAIL, ARRAY_SIZE(t6030_mem_config));
		t6030_mem_perf(s);
	}
	sleep_note(s, 0x73);
	sleep_ps_rewake_words(s, &t6030_wake_mid_words, false);
	t6030_spmi_wake_irqs(s);
	t6030_sleep_undo_regs(s);
	sleep_note(s, 0x74);
	sleep_ps_rewake_words(s, &t6030_wake_late_words, false);
	sleep_ps_rewake_rest(s);
	t6030_sleep_down_aon_back(s);
	if (t->gate_dispext1_staged) {
		writel(t->gate_dispext1_saved, s->pwrgate + T6030_PWRGATE_DISPEXT1);
		t->gate_dispext1_staged = false;
	}
	t6030_sleep_wake_check_ps(s, "at the end");
	if (READ_ONCE(t6030_last_writes))
		t6030_last_pmu_wake(s);
	sleep_note(s, 0x75);
}

/*
 * The cores' power-down controls, A7 to M3 (aura's cpu_regs.h). CYC_OVRD is not locked before
 * M4, and aura's init_cpu() leaves each core's WFI in mode 2 with retention: a WFI that never
 * powers the core down, so its cluster never goes off and the sleep sequencer waits.
 *
 * Every core's last steps are macOS 27.2's on J516s, measured at each core's final WFI under
 * aura's HV (PLAN 29.14): CYC_OVRD in WFI mode 3, ACC_OVRD at its boot value (no deep-sleep
 * fields; P cores 0x180010900004020e, E cores 0x180010100004020e), and S3_5_C15_C6_2 =
 * 0x8000000000000001. The last is 0 at the commit words and set after them.
 *
 * No external-debug release. ml_arm_sleep() writes EDLAR and EDPRCR only through
 * coresight_base[CORESIGHT_ED], and configure_coresight_registers() maps that only on a
 * debug-fused chip; J516s's /chosen effective-production-status-ap is 1. On J516s a plain read
 * of a core's coresight-reg window (0x210010310, EDPRCR) hangs the SoC.
 */
#define T6030_CYC_OVRD			sys_reg(3, 5, 15, 5, 0)
#define T6030_CYC_OVRD_WFI_MODE		GENMASK_ULL(25, 24)
#define T6030_CYC_OVRD_DISABLE_WFI_RET	BIT_ULL(0)

/*
 * DIAGNOSTIC (T6030 sleep bring-up): each core's own FIQ sources quiet before its final WFI, as
 * T8142's m5_quiet. A pending FIQ ends a WFI, masked or not, and a stopped core's loop only
 * clears IPI_SR. With VHE, sleep_deep_wfi_arm masks the EL2 timers only; this also masks the
 * EL1 pair (macOS's per-core init leaves cntp_ctl = 2) and stops the core PMU (PMCR0 = 0), as
 * XNU's core-offline path does before its deep WFI. Boot parameter, off by default (A/B).
 */
/*
 * DIAGNOSTIC (T6030 sleep bring-up): the core's debug power-down request. Before its final WFI,
 * macOS 27.2 clears each core's EDPRCR through the external-debug window (EDLAR unlocked, then
 * EDPRCR = 0, twice), which clears CORENPDRQ: a request that keeps the core from powering down.
 * That window hangs this SoC, but the same bit reads through DBGPRCR_EL1. The probe logs every
 * core's; apple_sleep.dbg_pdrq=1 writes it 0 on every core before its final WFI.
 */
#define T6030_DBGPRCR_EL1		sys_reg(2, 0, 1, 4, 4)
#define T6030_DBGPRCR_CORENPDRQ		BIT(0)

static bool t6030_dbg_pdrq;
module_param_named(dbg_pdrq, t6030_dbg_pdrq, bool, 0444);
MODULE_PARM_DESC(dbg_pdrq, "T6030 DIAGNOSTIC: DBGPRCR_EL1 = 0 (CORENPDRQ clear) before each core's final WFI");

static u64 t6030_dbgprcr[NR_CPUS];

static void t6030_dbgprcr_read(void *unused)
{
	unsigned int cpu = smp_processor_id();

	if (cpu < NR_CPUS)
		t6030_dbgprcr[cpu] = read_sysreg_s(T6030_DBGPRCR_EL1);
}

static void t6030_dbgprcr_report(struct apple_sleep *s)
{
	unsigned int cpu, req = 0;

	on_each_cpu(t6030_dbgprcr_read, NULL, 1);
	for_each_online_cpu(cpu)
		if (cpu < 32 && (t6030_dbgprcr[cpu] & T6030_DBGPRCR_CORENPDRQ))
			req |= BIT(cpu);
	dev_info(s->dev, "DBGPRCR_EL1 at probe: CORENPDRQ set on cpus %#x (cpu0 %#llx, cpu6 %#llx, %u online)\n",
		 req, t6030_dbgprcr[0], t6030_dbgprcr[min(6U, NR_CPUS - 1)], num_online_cpus());
}

static bool t6030_no_wfi_clean;
module_param_named(no_wfi_clean, t6030_no_wfi_clean, bool, 0444);
MODULE_PARM_DESC(no_wfi_clean, "T6030 DIAGNOSTIC: no set/way clean or HID4 toggles before each core's final WFI");

static bool t6030_quiet;
module_param_named(quiet, t6030_quiet, bool, 0444);
MODULE_PARM_DESC(quiet, "T6030 DIAGNOSTIC: mask the EL1 timers and stop the core PMU before each core's final WFI");

static void t6030_deep_wfi_arm(void)
{
	u64 hid4;

	if (READ_ONCE(t6030_dbg_pdrq)) {
		write_sysreg_s(0, T6030_DBGPRCR_EL1);
		isb();
	}
	if (READ_ONCE(t6030_quiet)) {
		if (is_kernel_in_hyp_mode()) {
			write_sysreg_s(ARCH_TIMER_CTRL_IT_MASK, SYS_CNTP_CTL_EL02);
			write_sysreg_s(ARCH_TIMER_CTRL_IT_MASK, SYS_CNTV_CTL_EL02);
		}
		write_sysreg_s(0, T8142_PMCR0);
		isb();
	}
	/*
	 * This core's last stores are in by now: its L1 and its cluster's L2 back to memory, as
	 * XNU's CleanPoC_Dcache before its sleep. A cluster's last core in cleans the L2 last.
	 * DIAGNOSTIC: apple_sleep.no_wfi_clean=1 leaves out the clean and the HID4/EHID4 toggles,
	 * which no working SoC's last WFI does (A/B).
	 */
	if (!READ_ONCE(t6030_no_wfi_clean)) {
		hid4 = t6030_dc_unblock();
		t6030_clean_all_levels();
		t6030_dc_reblock(hid4);
	}

	/*
	 * macOS 27.2 at every core's final WFI on J516s, under aura's HV with option 0x1000
	 * (PLAN 29.14): CYC_OVRD in WFI mode 3, ACC_OVRD left at its boot value, and
	 * S3_5_C15_C6_2 = 0x8000000000000001, which is 0 at the commit words and set after them.
	 */
	sysreg_clear_set_s(T6030_CYC_OVRD, T6030_CYC_OVRD_WFI_MODE,
			   FIELD_PREP(T6030_CYC_OVRD_WFI_MODE, 3) | T6030_CYC_OVRD_DISABLE_WFI_RET);
	sysreg_clear_set_s(SYS_APL_DEEP_SLEEP, 0, DEEP_SLEEP_ENABLE);
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
 * DIAGNOSTIC (T6030 sleep bring-up): where the RTC alarm's event stops after a real commit.
 * With observer_ms, the observer core (the boot core's P neighbour, kept out of the sleep)
 * samples every 100 ms for observer_ms, from the boot core's last WFI:
 *  - the PMU's alarm control, 0x2000 (one SPMI read a sample, after the MMIO reads);
 *  - nub-ap-ctrl's seven wake status words, +0x24..+0x3c (macOS's wake status: +0x34 bit 31);
 *  - nub-spmi0's first ack and mask words, +0x60 (macOS reads 4 at its wake: source 2) and
 *    +0x20; nub-spmi-a0's third, +0x68 (macOS 0x4f000000) and +0x28;
 *  - the boot core's PCPU word, both cluster words, the commit word and the CLLT sleep word.
 * Only the PMU scratch's 5 bytes survive the VDM, so obs_timeline picks what goes there, each
 * byte written when it changes:
 *  1: the sample at which each first changed (0xff: not yet; 0xfe at the window's end: never):
 *     +0 the alarm event (0x2000 bit 1), +1 any wake status word nonzero, +2 nub-spmi0 +0x60
 *     nonzero, +3 nub-spmi-a0 +0x68 nonzero, +4 the boot core's side moved: its WFI came
 *     back, its PCPU word on again after it went off, or a cluster, commit or CLLT word changed
 *     after the first 200 ms;
 *  2: the latest values: +0 the alarm control, +1 the wake status words nonzero (bits 0-6) and
 *     +0x34 bit 31 (bit 7), +2 nub-spmi0 +0x60 bits 7:0, +3 nub-spmi0 +0x20 bits 3:0 and 15:12
 *     (bits 3:0 and 7:4), +4 nub-spmi-a0 +0x68 bits 31:24.
 *  3: as 1 for +0 (the alarm event), +1 (a wake status word nonzero) and +4 (nub-spmi0 +0x60
 *     nonzero); +2 and +3 the first nonzero status word at +1's sample: +2 its index (bits 7:5)
 *     and lowest set bit (4:0), +3 its set bits, at most 6 (7:5), and highest set bit (4:0).
 *  4: with apple_sleep.spmi1, the second PMU's latest values (one SPMI read set on nub-spmi1 a
 *     sample): +0 its alarm control (0xff: unread), +1 its 0xf801, +2 its 0x281c, +3 its compare
 *     minus its counter in seconds (signed, clamped), +4 the main PMU's alarm control.
 *  5: the cores: +0/+1 a bit per core (ECPU0-5 bits 0-5, PCPU0-5 bits 6-11) whose power word
 *     never read off in the window, +2/+3 the E and P cluster words' low bytes, +4 every core's
 *     FIQ sources still firing at its last instruction before WFI, ORed (t8142_fiq_sources()).
 *  6: what else a sleep program would wait on, at the latest sample: a bit per power word not
 *     off (actual state nonzero), in the order of t6030_obs_ps_words (ECPM, PCPM, AMCC0,
 *     DCS0-5, AMCC1, DCS6-11, PMP: bits 0-16) then t6030_obs_aon_words (the nub words
 *     ps-aon +0x58..+0xa8: bits 17-27), in +0..+3 (+3 bits 3:0); +3 bit 4 the PTD request not
 *     equal to its ack, bit 5 the PTD unread; +4 ECPM's actual state (7:4) and PCPM's (3:0).
 *  7: the sleep program's own progress (lane A's decode of the NUB SRAM program), the latest:
 *     +0 the step code 0x3642b8040, +1 E_ST 0x350782000 and +2 P_ST 0x350782004 (bits 7:0),
 *     +3 bit 0 the sequence flag 0x3642d010c, 1/2 E_ST bits 9/10, 3/4 P_ST bits 9/10,
 *     5 0x3642b801c = 1 (a sequence ended), 6 = 0xa03 (one started), 7 any AMCC +0x40c bit 0;
 *     +4 the engine's SPMI queue: 0x36471c000 bits 18:16 (2:0), 0x36471c060 bits 2:0 (5:3),
 *     WD2 armed (6).
 * The sample bytes count 100 ms each up to a 25.3 s window, steps of 200 ms beyond.
 * Reads only, beyond the notes. 0 (default): the plain observer.
 */
#define T6030_OBS_PERIOD_MS		100
#define T6030_OBS_SETTLE		2
#define T6030_OBS_NOT_YET		0xff
#define T6030_OBS_NEVER			0xfe
#define T6030_OBS_LAST			0xfd
#define T6030_OBS_STATUS_WORDS		7
#define T6030_OBS_MACOS_WAKE_WORD	4
#define T6030_OBS_CTL_UNREAD		0xff
#define T6030_PS_PCPU0			0x30
#define T6030_PS_ACTUAL			GENMASK(7, 4)

static unsigned int t6030_obs_timeline;
module_param_named(obs_timeline, t6030_obs_timeline, uint, 0644);
MODULE_PARM_DESC(obs_timeline, "T6030 DIAGNOSTIC: with observer_ms, the observer's 100 ms samples in PMU scratch: 1 when each first changed, 2 the latest values, 3 when, with the first wake status word (0 = off)");

/* obs_pmu=0: MMIO reads only, no SPMI read of the PMU per sample (+0 then reads never) */
static bool t6030_obs_pmu = true;
module_param_named(obs_pmu, t6030_obs_pmu, bool, 0644);
MODULE_PARM_DESC(obs_pmu, "T6030 DIAGNOSTIC: with obs_timeline, read the PMU's alarm control each sample (default Y)");

static bool t6030_obs_timeline_on(void)
{
	unsigned int mode = READ_ONCE(t6030_obs_timeline);

	return mode >= 1 && mode <= 7;
}

struct t6030_obs {
	u32 status[T6030_OBS_STATUS_WORDS];
	u32 ack0, mask0, a0_ack2, a0_mask2;
	u32 pcpu, cluster[2], commit, cllt;
	u8 ctl;
	bool ctl_read;
	/* obs_timeline=4: the second PMU */
	u8 p2_ctl, p2_stage, p2_wake;
	s8 p2_left;
	bool p2_read;
};

static void t6030_obs_read_pmu2(struct t6030_obs *o)
{
	u8 ticks[6] = {}, compare[4] = {};
	s64 left;

	o->p2_read = !t6030_pmu2_read(T6050_RTC_CTL, &o->p2_ctl, 1) &&
		     !t6030_pmu2_read(0xf801, &o->p2_stage, 1) &&
		     !t6030_pmu2_read(T6030_PMU_OFF2WAKE, &o->p2_wake, 1) &&
		     !t6030_pmu2_read(T6050_RTC_COUNTER, ticks, 6) &&
		     !t6030_pmu2_read(T6050_RTC_COMPARE, compare, 4);
	left = (s64)get_unaligned_le32(compare) - get_unaligned_le32(ticks + 2);
	o->p2_left = clamp_t(s64, left, S8_MIN, S8_MAX);
}

static void t6030_obs_read(struct apple_sleep *s, struct t6030_obs *o)
{
	u32 runtime;
	int i;

	for (i = 0; i < T6030_OBS_STATUS_WORDS; i++)
		o->status[i] = readl(s->wake + T6030_WAKE_STATUS + 4 * i);
	o->ack0 = readl(s->spmi + T6030_SPMI_ACK(0));
	o->mask0 = readl(s->spmi + T6030_SPMI_MASK(0));
	o->a0_ack2 = readl(s->spmi_a0 + T6030_SPMI_ACK(2));
	o->a0_mask2 = readl(s->spmi_a0 + T6030_SPMI_MASK(2));
	o->pcpu = readl(s->ps + T6030_PS_PCPU0);
	o->cluster[0] = readl(s->cluster + T6030_CLUSTER_E);
	o->cluster[1] = readl(s->cluster + T6030_CLUSTER_P);
	o->commit = readl(s->commit);
	o->cllt = readl(s->cllt + T6030_CLLT_SLEEP);
	o->ctl = T6030_OBS_CTL_UNREAD;
	o->ctl_read = false;
	if (!READ_ONCE(t6030_obs_pmu))
		return;
	/* The notify mask as the commit left it, without the done bit during the read */
	runtime = readl(s->spmi + SPMI_NOTIFY_ENABLE);
	o->ctl_read = !t6030_pmu_xfer(s, runtime & ~T6030_SPMI_DONE, T6050_SPMI_EXT_READL,
				      T6050_RTC_CTL, NULL, &o->ctl, 1);
	writel(runtime, s->spmi + SPMI_NOTIFY_ENABLE);
}

/* obs_timeline=6: ECPM, PCPM, AMCC0, DCS0-5, AMCC1, DCS6-11, PMP ("ps"); the nub words ("ps-aon") */
static const u16 t6030_obs_ps_words[] = {
	0x60, 0x68, 0x318, 0x320, 0x328, 0x330, 0x338, 0x340, 0x348, 0x350, 0x358, 0x360,
	0x368, 0x370, 0x378, 0x380, 0x4c8,
};
static const u8 t6030_obs_aon_words[] = {
	0x58, 0x60, 0x68, 0x70, 0x78, 0x80, 0x88, 0x90, 0x98, 0xa0, 0xa8,
};
#define T6030_OBS_BLK_PTD		17
#define T6030_OBS_PTD_REQ		0x1180
#define T6030_OBS_PTD_ACK		0x11c0

/*
 * obs_timeline=7: the sleep program's progress and poll targets, mapped nonposted at probe (no
 * access there). Lane A's decode of the J516s program: every sequence writes the flag 0 at its
 * start and 1 at its end, the step code around each PMU handshake, E_DOWN and P_DOWN poll E_ST
 * and P_ST (& 0x20 == 0 with no timeout), the PMU handshake polls the engine's own SPMI queue
 * (+0x60 with no timeout), and the DRAM step polls each AMCC +0x40c bit 0.
 */
#define T6030_ENG_FLAG_PA		0x3642d0000ULL
#define T6030_ENG_FLAG			0x10c
#define T6030_ENG_STEP_PA		0x3642b8000ULL
#define T6030_ENG_STEP			0x40
#define T6030_ENG_SEQ			0x1c
#define T6030_ENG_SEQ_START		0xa03
#define T6030_ENG_CST_PA		0x350782000ULL
#define T6030_ENG_E_ST			0x0
#define T6030_ENG_P_ST			0x4
#define T6030_ENG_SPMI_PA		0x36471c000ULL
#define T6030_ENG_SPMI_ACK		0x60
#define T6030_ENG_AMCC_CH		0x40c
#define T6030_WDT_WD2_CTL		0x2c
#define T6030_WDT_WD2_ARMED		BIT(2)
#define T6030_OBS_BLK_AMCC0		14
#define T6030_OBS_BLK_AMCC1		15

static void __iomem *t6030_eng_flag, *t6030_eng_step, *t6030_eng_cst, *t6030_eng_spmi;
static void __iomem *t6030_eng_smc;

/* The DRAM step's DCS self-refresh words (A:10, POLL bit 11), 6 channels per AMCC */
#define T6030_ENG_DCS_CHANNELS		12
#define T6030_ENG_DCS_SR		0x5b500
static void __iomem *t6030_eng_dcs[T6030_ENG_DCS_CHANNELS];

static u64 t6030_eng_dcs_pa(unsigned int i)
{
	return (i < 6 ? 0x220000000ULL : 0x222000000ULL) + 0x400000 + 0x200000 * (i % 6) +
	       (T6030_ENG_DCS_SR & ~0xfffULL);
}

/* The SMC word each sequence writes at its start and end */
#define T6030_ENG_SMC_PA		0x36c400000ULL
#define T6030_ENG_SMC_NOTIFY		0x910

static void __iomem *t6030_eng_map(struct apple_sleep *s, u64 pa)
{
	struct resource res = DEFINE_RES_MEM(pa, 0x1000);

	res.flags |= IORESOURCE_MEM_NONPOSTED;
	return apple_sleep_ioremap(s->dev, &res);
}

static void t6030_eng_probe(struct apple_sleep *s)
{
	unsigned int i;

	t6030_eng_flag = t6030_eng_map(s, T6030_ENG_FLAG_PA);
	t6030_eng_step = t6030_eng_map(s, T6030_ENG_STEP_PA);
	t6030_eng_cst = t6030_eng_map(s, T6030_ENG_CST_PA);
	t6030_eng_spmi = t6030_eng_map(s, T6030_ENG_SPMI_PA);
	t6030_eng_smc = t6030_eng_map(s, T6030_ENG_SMC_PA);
	for (i = 0; i < T6030_ENG_DCS_CHANNELS; i++)
		t6030_eng_dcs[i] = t6030_eng_map(s, t6030_eng_dcs_pa(i));
	t6030_block(s, T6030_OBS_BLK_AMCC0);
	t6030_block(s, T6030_OBS_BLK_AMCC1);
}

/*
 * The sleep program's markers as this boot found them, before anything writes them: if they
 * survive the reset that ended a failed sleep, they say which sequence and step it stopped in.
 * Reads only.
 */
static void t6030_eng_boot_record(struct apple_sleep *s)
{
	u32 wd[12];
	int i;

	for (i = 0; i < ARRAY_SIZE(wd); i++)
		wd[i] = readl(s->wdt + 4 * i);
	/* Never the queue's +0x8: reading it drains the engine's reply */
	dev_info(s->dev, "boot engine: flag 0x3642d010c %#x, step 0x3642b8040 %#x, seq 0x3642b801c %#x, SPMI queue 0x36471c000 %#x +0x4 %#x +0x60 %#x, SMC 0x36c400910 %#x, E_ST %#x P_ST %#x\n",
		 t6030_eng_flag ? readl(t6030_eng_flag + T6030_ENG_FLAG) : ~0U,
		 t6030_eng_step ? readl(t6030_eng_step + T6030_ENG_STEP) : ~0U,
		 t6030_eng_step ? readl(t6030_eng_step + T6030_ENG_SEQ) : ~0U,
		 t6030_eng_spmi ? readl(t6030_eng_spmi) : ~0U,
		 t6030_eng_spmi ? readl(t6030_eng_spmi + 0x4) : ~0U,
		 t6030_eng_spmi ? readl(t6030_eng_spmi + T6030_ENG_SPMI_ACK) : ~0U,
		 t6030_eng_smc ? readl(t6030_eng_smc + T6030_ENG_SMC_NOTIFY) : ~0U,
		 t6030_eng_cst ? readl(t6030_eng_cst + T6030_ENG_E_ST) : ~0U,
		 t6030_eng_cst ? readl(t6030_eng_cst + T6030_ENG_P_ST) : ~0U);
	dev_info(s->dev, "boot engine: WD 0x3642b0000: %*ph\n", (int)sizeof(wd), wd);
	{
		struct t6030_sleep *t = t6030_state(s);
		u32 amcc[6] = {}, dcs[T6030_ENG_DCS_CHANNELS] = {};
		unsigned int b, c;

		for (b = 0; b < 2; b++)
			for (c = 0; c < 3; c++)
				if (t->blk[T6030_OBS_BLK_AMCC0 + b])
					amcc[3 * b + c] = readl(t->blk[T6030_OBS_BLK_AMCC0 + b] +
								0x40000 * c + T6030_ENG_AMCC_CH);
		for (c = 0; c < T6030_ENG_DCS_CHANNELS; c++)
			if (t6030_eng_dcs[c])
				dcs[c] = readl(t6030_eng_dcs[c] + (T6030_ENG_DCS_SR & 0xfff));
		dev_info(s->dev, "boot engine: AMCC +0x40c %08x %08x %08x %08x %08x %08x\n", amcc[0],
			 amcc[1], amcc[2], amcc[3], amcc[4], amcc[5]);
		dev_info(s->dev, "boot engine: DCS +0x5b500 %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x\n",
			 dcs[0], dcs[1], dcs[2], dcs[3], dcs[4], dcs[5], dcs[6], dcs[7], dcs[8],
			 dcs[9], dcs[10], dcs[11]);
	}
}

static void t6030_obs_engine(struct apple_sleep *s, u8 *v)
{
	struct t6030_sleep *t = t6030_state(s);
	u32 e = 0, p = 0, seq = 0, q = 0, ack = 0, ch;
	unsigned int b, c;

	if (t6030_eng_cst) {
		e = readl(t6030_eng_cst + T6030_ENG_E_ST);
		p = readl(t6030_eng_cst + T6030_ENG_P_ST);
	}
	v[0] = t6030_eng_step ? readl(t6030_eng_step + T6030_ENG_STEP) : 0xff;
	v[1] = e;
	v[2] = p;
	v[3] = 0;
	if (t6030_eng_flag && (readl(t6030_eng_flag + T6030_ENG_FLAG) & 1))
		v[3] |= BIT(0);
	v[3] |= !!(e & BIT(9)) << 1 | !!(e & BIT(10)) << 2 | !!(p & BIT(9)) << 3 |
		!!(p & BIT(10)) << 4;
	if (t6030_eng_step)
		seq = readl(t6030_eng_step + T6030_ENG_SEQ);
	if (seq == 1)
		v[3] |= BIT(5);
	else if (seq == T6030_ENG_SEQ_START)
		v[3] |= BIT(6);
	for (b = T6030_OBS_BLK_AMCC0; b <= T6030_OBS_BLK_AMCC1; b++)
		for (c = 0; t->blk[b] && c < 3; c++) {
			ch = readl(t->blk[b] + 0x40000 * c + T6030_ENG_AMCC_CH);
			if (ch & BIT(0))
				v[3] |= BIT(7);
		}
	if (t6030_eng_spmi) {
		q = readl(t6030_eng_spmi);
		ack = readl(t6030_eng_spmi + T6030_ENG_SPMI_ACK);
	}
	v[4] = ((q >> 16) & 7) | (ack & 7) << 3;
	if (readl(s->wdt + T6030_WDT_WD2_CTL) & T6030_WDT_WD2_ARMED)
		v[4] |= BIT(6);
}

static void t6030_obs_waits(struct apple_sleep *s, u8 *v)
{
	void __iomem *ptd = t6030_state(s)->blk[T6030_OBS_BLK_PTD];
	unsigned int i, n = ARRAY_SIZE(t6030_obs_ps_words);
	u32 on = 0;

	for (i = 0; i < n; i++)
		if (FIELD_GET(PS_ACTUAL, readl(s->ps + t6030_obs_ps_words[i])))
			on |= BIT(i);
	for (i = 0; i < ARRAY_SIZE(t6030_obs_aon_words); i++)
		if (FIELD_GET(PS_ACTUAL, readl(s->ps_aon + t6030_obs_aon_words[i])))
			on |= BIT(n + i);
	v[0] = on;
	v[1] = on >> 8;
	v[2] = on >> 16;
	v[3] = (on >> 24) & 0xf;
	if (!ptd)
		v[3] |= BIT(5);
	else if (readl(ptd + T6030_OBS_PTD_REQ) != readl(ptd + T6030_OBS_PTD_ACK))
		v[3] |= BIT(4);
	v[4] = FIELD_GET(PS_ACTUAL, readl(s->ps + t6030_obs_ps_words[0])) << 4 |
	       FIELD_GET(PS_ACTUAL, readl(s->ps + t6030_obs_ps_words[1]));
}

static void t6030_obs_mark(struct apple_sleep *s, u8 *at, unsigned int off, bool hit,
			   unsigned int k)
{
	if (!hit || at[off] != T6030_OBS_NOT_YET)
		return;
	at[off] = min(k, (unsigned int)T6030_OBS_LAST);
	sleep_note_at(s, off, at[off]);
}

static void t6030_obs_values(const struct t6030_obs *o, u8 *v, bool pmu2)
{
	int i;

	if (pmu2) {
		v[0] = o->p2_read ? o->p2_ctl : T6030_OBS_CTL_UNREAD;
		v[1] = o->p2_read ? o->p2_stage : T6030_OBS_CTL_UNREAD;
		v[2] = o->p2_read ? o->p2_wake : T6030_OBS_CTL_UNREAD;
		v[3] = o->p2_read ? (u8)o->p2_left : T6030_OBS_CTL_UNREAD;
		v[4] = o->ctl_read ? o->ctl : T6030_OBS_CTL_UNREAD;
		return;
	}

	v[0] = o->ctl_read ? o->ctl : T6030_OBS_CTL_UNREAD;
	v[1] = 0;
	for (i = 0; i < T6030_OBS_STATUS_WORDS; i++)
		if (o->status[i])
			v[1] |= BIT(i);
	if (o->status[T6030_OBS_MACOS_WAKE_WORD] & BIT(31))
		v[1] |= BIT(7);
	v[2] = o->ack0 & 0xff;
	v[3] = (o->mask0 & 0xf) | ((o->mask0 >> 8) & 0xf0);
	v[4] = o->a0_ack2 >> 24;
}

static void t6030_observe_timeline(struct apple_sleep *s, unsigned int ms, u64 t0)
{
	u64 period = arch_timer_get_cntfrq() * T6030_OBS_PERIOD_MS / 1000;
	unsigned int n = max(ms / T6030_OBS_PERIOD_MS, 1U), k, i, w;
	unsigned int mode = READ_ONCE(t6030_obs_timeline);
	unsigned int scale = DIV_ROUND_UP(n, T6030_OBS_LAST + 1);
	bool pmu2 = mode == 4, values = mode == 2 || pmu2, word = mode == 3, cores = mode >= 5;
	u16 never_off = GENMASK(11, 0);
	bool off_seen = false, hit, moved;
	struct t6030_obs o, ref = {};
	u8 at[5], v[5], last[5] = {};

	memset(at, T6030_OBS_NOT_YET, sizeof(at));
	for (i = 0; !values && !cores && i < ARRAY_SIZE(at); i++)
		sleep_note_at(s, i, T6030_OBS_NOT_YET);
	for (k = 0; k < n; k++) {
		while (read_sysreg(cntpct_el0) - t0 < k * period)
			cpu_relax();
		t6030_obs_read(s, &o);
		if (pmu2)
			t6030_obs_read_pmu2(&o);
		if (k == T6030_OBS_SETTLE)
			ref = o;
		if (cores) {
			if (mode == 7) {
				t6030_obs_engine(s, v);
			} else if (mode == 6) {
				t6030_obs_waits(s, v);
			} else {
				for (i = 0; i < 12; i++)
					if (!FIELD_GET(T6030_PS_ACTUAL, readl(s->ps + 8 * i)))
						never_off &= ~BIT(i);
				v[0] = never_off & 0xff;
				v[1] = never_off >> 8;
				v[2] = o.cluster[0] & 0xff;
				v[3] = o.cluster[1] & 0xff;
				v[4] = atomic_read(&sleep_fiq_or);
			}
			for (i = 0; i < ARRAY_SIZE(v); i++) {
				if (k && v[i] == last[i])
					continue;
				sleep_note_at(s, i, v[i]);
				last[i] = v[i];
			}
			continue;
		}
		if (values) {
			t6030_obs_values(&o, v, pmu2);
			for (i = 0; i < ARRAY_SIZE(v); i++) {
				if (k && v[i] == last[i])
					continue;
				sleep_note_at(s, i, v[i]);
				last[i] = v[i];
			}
			continue;
		}
		t6030_obs_mark(s, at, 0, o.ctl_read && (o.ctl & T6050_RTC_CTL_FIRED), k / scale);
		for (hit = false, i = 0; i < T6030_OBS_STATUS_WORDS; i++)
			hit |= !!o.status[i];
		if (word) {
			if (hit && at[1] == T6030_OBS_NOT_YET) {
				for (i = 0; !o.status[i]; i++)
					;
				w = o.status[i];
				at[2] = i << 5 | __ffs(w);
				at[3] = min(hweight32(w), 6U) << 5 | __fls(w);
				sleep_note_at(s, 2, at[2]);
				sleep_note_at(s, 3, at[3]);
			}
			t6030_obs_mark(s, at, 1, hit, k / scale);
			t6030_obs_mark(s, at, 4, !!o.ack0, k / scale);
			continue;
		}
		t6030_obs_mark(s, at, 1, hit, k / scale);
		t6030_obs_mark(s, at, 2, !!o.ack0, k / scale);
		t6030_obs_mark(s, at, 3, !!o.a0_ack2, k / scale);
		if (!FIELD_GET(T6030_PS_ACTUAL, o.pcpu))
			off_seen = true;
		moved = READ_ONCE(t8142_obs_wfi_back) ||
			(off_seen && FIELD_GET(T6030_PS_ACTUAL, o.pcpu)) ||
			(k > T6030_OBS_SETTLE &&
			 (o.cluster[0] != ref.cluster[0] || o.cluster[1] != ref.cluster[1] ||
			  o.commit != ref.commit || o.cllt != ref.cllt));
		t6030_obs_mark(s, at, 4, moved, k / scale);
	}
	for (i = 0; !values && !cores && i < ARRAY_SIZE(at); i++)
		if (at[i] == T6030_OBS_NOT_YET)
			sleep_note_at(s, i, T6030_OBS_NEVER);
}

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
	.after_ans = t6030_sleep_after_ans,
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
