/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/*
 * T8103 (M1): suspend-to-RAM through the PMGR's S2R timer. Included by sleep.c after the
 * other profiles, so every shared helper is in scope.
 *
 * At .prepare (0x87, after SEP's sleep and before the SMC's last keys) the coprocessors no
 * driver stops are idled and stopped, and the domains Linux leaves on that the sleep needs
 * off are lowered. The commit then steps the memory clock down, quiesces the memory
 * controller and cache, arms the AOP wake mask, parks the SoC clock muxes, quiesces the CPU
 * complexes, turns CPM power gating on, takes both CPU clusters' targets off, AUTO_ENABLE off
 * on three always-on words, clears SEP's power-gate wait, arms the watchdog and the PMU RTC
 * alarm's SPMI vector, and writes the S2R timer. The cores power down without retention
 * through CYC_OVRD and ACC_OVRD, after writing back their caches, as on the other
 * Firestorm/Icestorm SoCs. Based on the t6000 suspend-to-RAM reference by Andreas Fernandez.
 *
 * A timed wake is the PMU RTC's alarm (wake source 54), set by its RTC driver.
 */

/* "wake": five mask words, then five status words */
#define T8103_WAKE_MASK		0x08
#define T8103_WAKE_STATUS	0x1c
#define T8103_WAKE_WORDS	5

/* "clk-mux": the SoC clock muxes, parked in this order */
#define T8103_MUX_PARK_KEEP	0xf0ffffff
#define T8103_MUX_PARK_SET	0x80000000
#define T8103_MUX_BUSY		BIT(30)
#define T8103_MUX_TIMEOUT_US	192000
/* The SoC clock muxes: 40 words from here in the clock block */
#define T8103_MUX_SOC		0x40100

/* "ps": the CPM power gating, the CPU clusters and the SEP power-gate wait */
#define T8103_CPM_GATING	BIT(31)
#define T8103_CLUSTER_E		0x18000
#define T8103_CLUSTER_P		0x18004
#define T8103_CLUSTER_BUSY	BIT(4)
#define T8103_SEP_PG		0x4c200
#define T8103_SEP_PG_WAIT	BIT(2)
/* SEP's own power state word, and how long it gets to gate after its sleep's answer */
#define T8103_SEP_PS		0xc00
#define T8103_SEP_GATE_US	500000

/* "ps-aon": the S2R timer, the commit */
#define T8103_S2R_TIMER		0x44000

/* "spmi" (nub, gen 1): vector enable words from +0x20, the PMU RTC alarm is vector 2 */
#define T8103_SPMI_VEC_ENABLE	0x20
#define T8103_SPMI_VEC_RTC	BIT(2)
#define T8103_SPMI_VEC_ACK	0x60
/*
 * The reply notification is vector 192 here, as on t6000: its enable and ack words are the
 * seventh, not gen 1's ninth.
 */
#define T8103_SPMI_NOTIFY_ENABLE	(0x20 + 4 * (192 / 32))
#define T8103_SPMI_NOTIFY_ACK		(0x60 + 4 * (192 / 32))
#define T8103_SPMI_EXT_WRITEL	0x30

/* The PMU's boot-stage byte: 0x50 while the AP sleeps */
#define T8103_PMU_USID		0xf
#define T8103_PMU_BOOT_STAGE	0x9f01
#define T8103_PMU_STAGE_SLEEP	0x50
/* The watchdog from the commit to the wake, and from the wake to .end */
#define T8103_WDT_COMMIT_SECS	60
#define T8103_WDT_RESUME_SECS	35

/* "wake": word 0 bit 0 keeps DRAM up through the sleep (SLP_DDR) */
#define T8103_WAKE_SLP_DDR	0x00
#define T8103_WAKE_SLP_DDR_ON	BIT(0)

/* Each core's external debug block: 1 MiB apart, the clusters 16 MiB apart */
#define T8103_ED		0x210010000ULL
#define T8103_ED_CORE		0x100000ULL
#define T8103_ED_CLUSTER	0x1000000ULL

/* Per-core power-down controls */
#define T8103_CYC_OVRD			sys_reg(3, 5, 15, 5, 0)
#define T8103_CYC_DISABLE_WFI_RET	BIT_ULL(0)
#define T8103_CYC_FIQ_MODE		GENMASK_ULL(21, 20)
#define T8103_CYC_IRQ_MODE		GENMASK_ULL(23, 22)
#define T8103_CYC_WFI_MODE		GENMASK_ULL(25, 24)
#define T8103_ACC_OVRD			sys_reg(3, 5, 15, 6, 0)
#define T8103_PMCR0			sys_reg(3, 1, 15, 0, 0)
#define T8103_ACC_PWR_DN_SRM		GENMASK_ULL(14, 13)
#define T8103_ACC_DIS_L2_FLUSH		GENMASK_ULL(16, 15)
#define T8103_ACC_TRAIN_DOWN_LINK	GENMASK_ULL(18, 17)
#define T8103_ACC_POWER_DOWN_CPM	GENMASK_ULL(26, 25)
#define T8103_ACC_DISABLE_PIO_ON_WFI	BIT_ULL(32)
#define T8103_ACC_DEEP_SLEEP		BIT_ULL(34)

/* The valid sources in each wake mask word */
static const u32 t8103_wake_valid[T8103_WAKE_WORDS] = {
	0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0x03ffffff,
};

/*
 * The mask the platform keeps at every sleep, as macOS sets it (a set bit masks): the AOP, NUB
 * (the wake timer, the watchdog, SPMI0 SW3), SMC and ATC port sources left on.
 */
static const u32 t8103_wake_platform[T8103_WAKE_WORDS] = {
	0xf3ffafff, 0xffb97eff, 0xfffdeffe, 0xffffffff, 0x03ffff87,
};

static const u32 t8103_mux_park[] = { 0x40140, 0x40144, 0x40148, 0x4014c, 0x40154 };

/* The always-on words whose AUTO_ENABLE goes off for the sleep, in this order */
static const u32 t8103_auto_off[] = { 0x1c8, 0x1c0, 0x1d0 };

static const u32 t8103_cpm[] = { 0x1c080, 0x1c088 };

static const u32 t8103_cluster[] = { T8103_CLUSTER_E, T8103_CLUSTER_P };

/*
 * The CPU complexes' and voltage monitors' quiesce, as macOS runs it between the mux park and
 * CPM gating: per cluster the energy accumulator, the
 * fixed-frequency PLL relock and three throttlers off; the DCS, GFX, DISP and SOC voltage
 * monitors off, each waited idle; then each cluster's APSC off, waited. ACC blocks at
 * 0x210e20000 (E) and 0x211e20000 (P).
 */
#define T8103_ACC_E		0x210e20000ULL
#define T8103_ACC_P		0x211e20000ULL
#define T8103_ACC_LEN		0x30000
#define T8103_ACC_DVFS		0x00020
#define T8103_ACC_DVFS_RELOCK	BIT_ULL(42)
#define T8103_ACC_DVFS_APSC_OFF	BIT_ULL(22)
#define T8103_ACC_DVFS_PENDING	BIT_ULL(7)
#define T8103_ACC_AMX_THRTL	0x20250
#define T8103_ACC_LLC_THRTL	0x20240
#define T8103_ACC_PPT_THRTL	0x28400
#define T8103_ACC_ENERGY	0x28080
#define T8103_ACC_ENERGY_EN	(BIT_ULL(63) | BIT_ULL(61))
#define T8103_THRTL_EN		BIT_ULL(63)
/* In "ps": DCS, GFX, DISP, SOC, in macOS's order; idle at +0x3e0 bit 1 */
static const u32 t8103_tvm[] = { 0x48400, 0x48800, 0x48c00, 0x48000 };
#define T8103_TVM_EN		BIT(0)
#define T8103_TVM_IDLE_OFF	0x3e0
#define T8103_TVM_BUSY		BIT(1)
#define T8103_QUIESCE_US	10000
static const u32 t8103_acc_thrtl[] = { T8103_ACC_AMX_THRTL, T8103_ACC_LLC_THRTL,
				       T8103_ACC_PPT_THRTL };

/*
 * The PMP: the boot loader starts its firmware and no Linux driver owns it here, and a commit
 * with it running resets the SoC (as T8140 found). Idled over its RTKit management endpoint,
 * then its CPU stopped; its domains stay on. The SIO and the display coprocessors likewise.
 */
#define T8103_PMP_ASC		0x23bc00000ULL
#define T8103_PMP_MBOX		0x23bc08000ULL
#define T8103_SIO_ASC		0x236400000ULL
#define T8103_SIO_MBOX		0x236408000ULL
#define T8103_DCP_MBOX		0x231c08000ULL
#define T8103_DCPEXT_ASC	0x271c00000ULL
#define T8103_DCPEXT_MBOX	0x271c08000ULL
/* The display coprocessors' CPU power words, in "ps" */
#define T8103_DISP0_CPU_PS	0x10018
#define T8103_DISPEXT_CPU_PS	0x378
#define T8103_ASC_CPU_CONTROL	0x44
#define T8103_ASC_CPU_RUN	BIT(4)
#define T8103_ASC_CPU_STATUS	0x48
#define T8103_ASC_CPU_IDLE	GENMASK(1, 0)
#define T8103_MBOX_A2I_CONTROL	0x110
#define T8103_MBOX_I2A_CONTROL	0x114
#define T8103_MBOX_FULL		BIT(16)
#define T8103_MBOX_EMPTY	BIT(17)
#define T8103_MBOX_RECV_ENABLE	BIT(0)
#define T8103_MBOX_A2I_SEND0	0x800
#define T8103_MBOX_A2I_SEND1	0x808
#define T8103_MBOX_I2A_RECV0	0x830
#define T8103_MBOX_I2A_RECV1	0x838
#define T8103_RTKIT_TYPE	GENMASK_ULL(59, 52)
#define T8103_RTKIT_SET_IOP_PWR	6
#define T8103_RTKIT_IOP_PWR_ACK	7
#define T8103_RTKIT_PWR		GENMASK_ULL(15, 0)
#define T8103_RTKIT_PWR_IDLE	0x201
#define T8103_PMP_ACK_US	200000
#define T8103_PMP_IDLE_US	10000

/* AGX fabric tunables programmed by the boot loader; lost when ps_gfx powers off. */
#define T8103_AGX_FABRIC	0x205000000ULL
static const u32 t8103_agx_regs[] = { 0x30, 0x34, 0x400, 0x600, 0x738, 0x798, 0x800 };

/*
 * CLK_PERF_DCS (T8103_DCS_PERF) holds the memory clock's performance state: target in bits 3:0,
 * busy in bit 31. Moving it by more than one state in a single write leaves memory unusable on
 * this SoC, while single steps don't. The SoC's own sleep entry moves it to T8103_DCS_SLEEP in
 * one write and its wake leaves it at T8103_DCS_WAKE, so it is stepped down to the sleep state
 * before the commit and back up after the wake, one state at a time.
 */
#define T8103_DCS_PERF		0x23b05c000ULL
#define T8103_DCS_SLEEP		4
#define T8103_DCS_WAKE		5

u32 apple_sleep_perf_step(void __iomem *reg, u32 state);

/* Single steps to @target, each finished before the next. Returns false if one didn't finish. */
static bool t8103_dcs_step_to(void __iomem *reg, unsigned int target, u32 *last)
{
	unsigned int cur = readl(reg) & 0xf;
	u32 v = 0;

	while (cur != target) {
		cur = cur < target ? cur + 1 : cur - 1;
		v = apple_sleep_perf_step(reg, cur);
		if (v & BIT(31)) {
			*last = v;
			return false;
		}
	}
	*last = readl(reg);
	return true;
}

/*
 * Domains genpd keeps on for an unbound consumer (sync_state) or stopped display/USB/PCIe blocks
 * that macOS has off at its commit. Lowered children-before-parents after every stop.
 */
static const u32 t8103_commit_off[] = {
	0x3f8,		/* 0: gfx */
	0x10018,	/* 1: disp0_cpu0 */
	0x360,		/* 2: disp0_fe */
	0x378,		/* 3: dispext_cpu0 */
	0x368,		/* 4: dispext_fe */
	0x450,		/* 5: atc1_pcie */
	0x448,		/* 6: atc1_common */
	0x428,		/* 7: atc0_pcie */
	0x420,		/* 8: atc0_common */
	0x350,		/* 9: rmx (parent of disp0_fe, dispext_fe) */
	0x3d8,		/* 10: pmp (parent of gfx, disp0_fe, dispext_fe) */
	0x3e8,		/* 11: apcie_gp */
	0x348,		/* 12: apcie (parent of apcie_gp) */
	0x1a0,		/* 13: pcie_ref (parent of apcie) */
	0x130,		/* 14: gpio (kept on through 0x85 for SEP, lowered after) */
};

#define T8103_COMMIT_OFF_APCIE_START	11
/*
 * The display front ends' parents (pmp, rmx): genpd still counts them on after the wake, so
 * nothing else raises them, and the DCP driver's power-up of DISP0 hangs without them. Put
 * back as the commit found them, pmp first.
 */
#define T8103_COMMIT_OFF_GFX		0
#define T8103_COMMIT_OFF_PMP		10
#define T8103_COMMIT_OFF_RMX		9

/* The PMGR's fabric bridge blocks: +0x14=1, +0x2bc=1,2,4 before their domain powers off */
static const struct {
	u64 pa;
	u32 ps_off;
} t8103_bridges[] = {
	{ 0x205008000ULL, 0x3f8 },	/* reg[40]: gfx */
	{ 0x232008000ULL, 0x360 },	/* reg[39]: disp0_fe */
	{ 0x272008000ULL, 0x368 },	/* reg[44]: dispext_fe */
	{ 0x68c008000ULL, 0x348 },	/* reg[41]: apcie */
};

#define T8103_BRIDGE_8014	0x014
#define T8103_BRIDGE_82BC	0x2bc

/* pmgr_mini (ps_aon) domains left on by USB/debug/msg that must go off */
static const u32 t8103_aon_commit_off[] = {
	0x98,		/* 0: atc0_usb */
	0xa0,		/* 1: atc1_usb */
	0x88,		/* 2: atc0_usb_aon */
	0x90,		/* 3: atc1_usb_aon */
	0xb8,		/* 4: debug_usb */
	0xc0,		/* 5: debug_auth */
	0x58,		/* 6: debug */
	0x78,		/* 7: msg */
};

/* Clock-gated ps domains (0x244) Linux leaves at actual 4, lowered to 0 as macOS has them */
static const u32 t8103_cg_commit_off[] = {
	0x118,		/* 0: soc_spmi0 */
	0x120,		/* 1: soc_spmi1 */
	0x128,		/* 2: soc_spmi2 */
	0x148,		/* 3: pms_fpwm0 */
	0x150,		/* 4: pms_fpwm1 */
	0x158,		/* 5: pms_fpwm2 */
	0x160,		/* 6: pms_fpwm3 */
	0x168,		/* 7: pms_fpwm4 */
	0x1a8,		/* 8: aft0 */
	0x1b0,		/* 9: devc0_ivdmc */
	0x1d8,		/* 10: fpwm0 */
	0x1e8,		/* 11: fpwm2 */
};

/*
 * The memory cache (SLC): its ways off before the CPU complexes' quiesce, as macOS has it,
 * and back on at the wake. Broadcast control at 0x200100000, the four planes
 * at 0x200000000 + 0x40000 * p; each plane reports its ways at +0x1c04 bits 14:10 and 9:5.
 */
#define T8103_MCC_BCAST		0x200100000ULL
#define T8103_MCC_BCAST_LP	0x04
#define T8103_MCC_BCAST_LP_BITS	0x30000000U
#define T8103_MCC_BCAST_A	0x0c
#define T8103_MCC_BCAST_B	0x14
#define T8103_MCC_AMCNP_ERR	0x55000
#define T8103_MCC_PLANES	0x200000000ULL
#define T8103_MCC_NPLANES	4
#define T8103_MCC_STRIDE	0x40000
#define T8103_MCC_WAYS		0x1c00
#define T8103_MCC_WAYS_STATUS	0x1c04
#define T8103_MCC_WAYS_A	GENMASK(14, 10)
#define T8103_MCC_WAYS_B	GENMASK(9, 5)
#define T8103_MCC_ERR_EN0	0x9000
#define T8103_MCC_ERR_EN1	0x9008
#define T8103_MCC_TIMEOUT_US	100000

/*
 * DCS channels, 8 x 0x40000 from 0x200200000: dynamic clock and power gating (+0xc), error
 * reporting (+0x7a0), write queue flush (+0xa30)
 */
#define T8103_DCS_BASE		0x200200000ULL
#define T8103_DCS_NCHAN		8
#define T8103_DCS_STRIDE	0x40000
#define T8103_DCS_CTL		0x00c
#define T8103_DCS_CTL_LP	0x1100
#define T8103_DCS_ERR_EN	0x7a0
#define T8103_DCS_WQ_FLUSH	0xa30
#define T8103_DCS_FLUSH_US	330

/*
 * NCO channels 0..4 clock the externally-clocked MCA PMGR domains (ps_mca0..5). The SoC sleep
 * entry sequence writes and polls those domains; if a channel's enable bit was cleared after
 * an audio stream closed, the PMGR transition hangs busy and resets the SoC. Enable any
 * disabled channel across the commit and turn it back off on wake.
 */
#define T8103_NCO		0x23b044000ULL
#define T8103_NCO_CHANS		5
#define T8103_NCO_STRIDE	0x4000
#define T8103_NCO_CTRL_EN	BIT(31)

/* AIC: each CPU's IPI mask set at +0x5024 and its status at +0x5004, 0x80 per CPU */
#define T8103_AIC_CPU		0x23b105000ULL
#define T8103_AIC_CPU_STRIDE	0x80
#define T8103_AIC_CPU_STATUS	0x04
#define T8103_AIC_CPU_MASK_SET	0x24
#define T8103_AIC_CPU_MASK_CLR	0x28
#define T8103_AIC_CPU_PENDING	GENMASK(18, 16)
#define T8103_AIC_IPI_MASK	0x80000001
#define T8103_AIC_CPUS		8

/* Always-on domains meant to run with AUTO_ENABLE that the boot loader leaves without it */
static const u32 t8103_ps_boot_auto[] = {
	0x100, /* sbr */
	0x138, /* pms_busif */
	0x140, /* pms */
	0x1b8, /* imx */
	0x2f8, /* mcc */
	0x300, /* dcs0 */
	0x308, /* dcs2 */
	0x310, /* dcs1 */
	0x318, /* dcs3 */
	0x320, /* dcs4 */
	0x328, /* dcs6 */
	0x330, /* dcs5 */
	0x338, /* dcs7 */
	0x340, /* smx */
	0x3e0, /* pms_sram */
};

static const u32 t8103_aon_boot_auto[] = {
	0x60,  /* nub_spmi0 */
	0x68,  /* nub_spmi1 */
	0x70,  /* nub_aon */
	0xa8,  /* nub_fabric */
	0xb0,  /* nub_sram */
};

static struct t8103_sleep {
	void __iomem *mcc_bcast, *mcc_planes, *dcs, *aic_cpu, *nco;
	void __iomem *bridges[ARRAY_SIZE(t8103_bridges)];
	u8 nco_forced;
	/* The SoC clock muxes from T8103_MUX_SOC at the commit, and those the wake put back */
	u32 mux_snap[40];
	u64 mux_fixed;
	u32 slc_ways, aic_ipi_mask[T8103_AIC_CPUS];
	u32 mcc_lp_saved, amcnp_err_saved;
	u32 amcc_err0_saved[T8103_MCC_NPLANES], amcc_err1_saved[T8103_MCC_NPLANES];
	u32 dcs_ctl_saved[T8103_DCS_NCHAN], dcs_wq_saved[T8103_DCS_NCHAN];
	u32 dcs_err_saved[T8103_DCS_NCHAN];
	bool dcs_quiesced;
	u8 slc_result, aic_result;
	void __iomem *pmp_asc, *pmp_mbox, *sio_asc, *sio_mbox;
	void __iomem *dcp_mbox, *dcpext_asc, *dcpext_mbox;
	void __iomem *agx_fabric;
	u32 agx_saved[ARRAY_SIZE(t8103_agx_regs)];
	bool agx_saved_valid;
	int gfx_wake_err;
	/* Each coprocessor's t8103_asc_stop() result */
	u8 pmp_result, sio_result, dcp_result, dcpext_result;
	/* The t8103_commit_off and t8103_cg_commit_off words found or taken off */
	u16 commit_off, cg_commit_off;
	/* The t8103_commit_off words this commit lowered, as it found them */
	u16 commit_lowered;
	u32 commit_saved[ARRAY_SIZE(t8103_commit_off)];
	u8 aon_commit_off;
	u32 cg_saved[ARRAY_SIZE(t8103_cg_commit_off)];
	/* The quiesce's blocks, what it found there, and whether it ran */
	void __iomem *acc[2];
	void __iomem *ed[2][4];
	u64 acc_dvfs[2], acc_energy[2], acc_thrtl[2][ARRAY_SIZE(t8103_acc_thrtl)];
	u32 tvm[ARRAY_SIZE(t8103_tvm)];
	bool quiesced;
	unsigned int quiesce_timeouts;
	/* The sources apple,wake-enable leaves on, by mask word */
	u32 wanted[T8103_WAKE_WORDS];
	struct platform_device *gpu;

	/* As prepare_regs found them, for the wake */
	bool saved;
	u32 wake_mask[T8103_WAKE_WORDS];
	u32 wake_slp_ddr;
	bool wake_slp_ddr_changed;
	/* The AOP's driver is bound: DRAM stays up for it through the sleep (SLP_DDR) */
	bool aop_slp_ddr;
	u32 mux[ARRAY_SIZE(t8103_mux_park)];
	void __iomem *dcs_perf;
	u32 dcs_perf_saved;
	u32 dcs_perf_down_val, dcs_perf_wake_val, dcs_perf_up_val;
	bool dcs_perf_down, dcs_perf_down_ok, dcs_perf_up_ok;
	u32 auto_word[ARRAY_SIZE(t8103_auto_off)];
	u32 cpm[ARRAY_SIZE(t8103_cpm)];
	u32 cluster[ARRAY_SIZE(t8103_cluster)];
	u32 sep_pg;
	u32 spmi_vec;

	/* This attempt: the commit's results, read back at the wake and reported once */
	bool attempt, committed, sep_pg_cleared;
	int sep_ps_gated;
	u32 sep_ps;
	u32 wake_entry[T8103_WAKE_WORDS];
	u8 mux_timeouts, cluster_timeouts;
	int cluster_wake_ret[ARRAY_SIZE(t8103_cluster)];
	u8 mux_wake_timeouts;
	/* Every interrupt line masked once for this commit (E14 before the timer) */
	bool aic_masked;
} t8103_sleep;

/*
 * A one-byte PMU write on nub-spmi0 by hand, in the commit, where nothing else talks SPMI: its
 * reply notification masked, a stale reply read first, the command and its data, the reply
 * checked, nothing left in the FIFO, the notification acknowledged and its enable put back.
 */
static int t8103_pmu_write(struct apple_sleep *s, u16 address, u8 val)
{
	u32 enable, status, reply, command = T8103_SPMI_EXT_WRITEL;
	unsigned int i;
	int ret = 0;

	enable = readl(s->spmi + T8103_SPMI_NOTIFY_ENABLE);
	writel(enable & ~SPMI_NOTIFY_REPLY, s->spmi + T8103_SPMI_NOTIFY_ENABLE);
	for (i = 0; i < SPMI_DRAIN_MAX && !(readl(s->spmi + SPMI_STATUS) & SPMI_STATUS_RX_EMPTY);
	     i++)
		readl(s->spmi + SPMI_REPLY);

	writel(command | FIELD_PREP(SPMI_CMD_USID, T8103_PMU_USID) | (u32)address << 16 |
	       SPMI_CMD_ACTIVE, s->spmi + SPMI_CMD);
	writel(val, s->spmi + SPMI_CMD);

	if (readl_poll_timeout_atomic(s->spmi + SPMI_STATUS, status,
				      !(status & SPMI_STATUS_RX_EMPTY), 1, 1000)) {
		ret = -ETIMEDOUT;
		goto out;
	}
	reply = readl(s->spmi + SPMI_REPLY);
	if ((reply & 0xff) != command || ((reply >> 8) & 0x7f) != T8103_PMU_USID ||
	    !(reply & SPMI_CMD_ACTIVE))
		ret = -EIO;
out:
	if (ret)
		udelay(SPMI_POLL_US / 10);
	for (i = 0; i < SPMI_DRAIN_MAX && !(readl(s->spmi + SPMI_STATUS) & SPMI_STATUS_RX_EMPTY);
	     i++)
		readl(s->spmi + SPMI_REPLY);
	writel(1, s->spmi + T8103_SPMI_NOTIFY_ACK);
	writel(enable, s->spmi + T8103_SPMI_NOTIFY_ENABLE);
	return ret;
}

static bool t8103_sleep_cpu_valid(u64 mpidr)
{
	/* E cores 0.0.n, P cores 1.1.n */
	return !(mpidr & ~MPIDR_HWID_BITMASK) && !MPIDR_AFFINITY_LEVEL(mpidr, 3) &&
	       MPIDR_AFFINITY_LEVEL(mpidr, 1) == MPIDR_AFFINITY_LEVEL(mpidr, 2);
}

/* Each core's power-down controls as Linux has them, back after a refusal or a reset */
struct t8103_cpu_control {
	u64 cyc, acc;
	bool saved;
};

static DEFINE_PER_CPU(struct t8103_cpu_control, t8103_cpu_control);

static int t8103_sleep_cpu_prepare(u64 mpidr)
{
	struct t8103_cpu_control *c = this_cpu_ptr(&t8103_cpu_control);

	if (!is_kernel_in_hyp_mode())
		return -EOPNOTSUPP;
	if (!c->saved) {
		c->cyc = read_sysreg_s(T8103_CYC_OVRD);
		c->acc = read_sysreg_s(T8103_ACC_OVRD);
		c->saved = true;
	}
	return 0;
}

static void t8103_sleep_cpu_restore(u64 mpidr)
{
	struct t8103_cpu_control *c = this_cpu_ptr(&t8103_cpu_control);

	if (!c->saved)
		return;
	if (read_sysreg_s(T8103_ACC_OVRD) != c->acc)
		write_sysreg_s(c->acc, T8103_ACC_OVRD);
	if (read_sysreg_s(T8103_CYC_OVRD) != c->cyc)
		write_sysreg_s(c->cyc, T8103_CYC_OVRD);
	isb();
	c->saved = false;
}

/*
 * void t8103_cpu_off(u64 cyc_ovrd, u64 acc_ovrd): a core's last instructions. The data cache
 * off, L1 written back and invalidated by set and way, then L2 written back by set and way (a
 * cluster's L2 is not written back when it powers down), the timers off, the power-down
 * controls written, then WFI until the power goes. Never returns.
 */
asm(
"	.pushsection .text, \"ax\"\n"
"	.type	t8103_cpu_off, %function\n"
"t8103_cpu_off:\n"
"	msr	daifset, #0xf\n"
"	mrs	x3, sctlr_el1\n"
"	bic	x3, x3, #(1 << 2)\n"
"	msr	sctlr_el1, x3\n"
"	isb\n"
"	msr	csselr_el1, xzr\n"
"	isb\n"
"	mrs	x3, ccsidr_el1\n"
"	and	x4, x3, #7\n"
"	add	x4, x4, #4\n"
"	ubfx	x5, x3, #3, #10\n"
"	ubfx	x6, x3, #13, #15\n"
"	clz	w7, w5\n"
"1:	mov	x8, x6\n"
"2:	lsl	x9, x5, x7\n"
"	lsl	x10, x8, x4\n"
"	orr	x9, x9, x10\n"
"	dc	cisw, x9\n"
"	subs	x8, x8, #1\n"
"	b.ge	2b\n"
"	subs	x5, x5, #1\n"
"	b.ge	1b\n"
"	dsb	sy\n"
"	isb\n"
/* L2 too: written back (clean only, others in the cluster may still run), set by way */
"	mov	x3, #2\n"
"	msr	csselr_el1, x3\n"
"	isb\n"
"	mrs	x3, ccsidr_el1\n"
"	and	x4, x3, #7\n"
"	add	x4, x4, #4\n"
"	ubfx	x5, x3, #3, #10\n"
"	ubfx	x6, x3, #13, #15\n"
"	clz	w7, w5\n"
"	mov	x11, #2\n"
"11:	mov	x8, x6\n"
"12:	lsl	x9, x5, x7\n"
"	lsl	x10, x8, x4\n"
"	orr	x9, x9, x10\n"
"	orr	x9, x9, x11\n"
"	dc	csw, x9\n"
"	subs	x8, x8, #1\n"
"	b.ge	12b\n"
"	subs	x5, x5, #1\n"
"	b.ge	11b\n"
"	dsb	sy\n"
"	isb\n"
"	msr	cntp_ctl_el0, xzr\n"
"	msr	cntv_ctl_el0, xzr\n"
"	mrs	x3, CurrentEL\n"
"	cmp	x3, #(2 << 2)\n"
"	b.ne	3f\n"
"	msr	s3_5_c14_c2_1, xzr\n"	/* CNTP_CTL_EL02 */
"	msr	s3_5_c14_c3_1, xzr\n"	/* CNTV_CTL_EL02 */
"3:	msr	s3_5_c15_c6_0, x1\n"	/* ACC_OVRD */
"	msr	s3_5_c15_c5_0, x0\n"	/* CYC_OVRD */
"	isb\n"
"	mov	x9, #1\n"
"4:	msr	s3_5_c15_c1_1, x9\n"	/* IPI_SR: a fast IPI ends WFI */
"	dsb	sy\n"
"	isb\n"
"	wfi\n"
"	b	4b\n"
"	.size	t8103_cpu_off, . - t8103_cpu_off\n"
"	.popsection\n"
);
void t8103_cpu_off(u64 cyc_ovrd, u64 acc_ovrd);

/* E10, on every core before its last WFI: power down without retention, woken by a reset. */
static void t8103_deep_wfi_arm(void)
{
	u64 mpidr = read_cpuid_mpidr() & MPIDR_HWID_BITMASK;
	unsigned int cluster = MPIDR_AFFINITY_LEVEL(mpidr, 2);
	unsigned int core = MPIDR_AFFINITY_LEVEL(mpidr, 0);
	u64 cyc = read_sysreg_s(T8103_CYC_OVRD), acc = read_sysreg_s(T8103_ACC_OVRD);

	/*
	 * The core's external debug block: unlocked, then its power-down request bits cleared,
	 * so a debugger's request doesn't hold the core's power on.
	 */
	if (cluster < 2 && core < 4 && t8103_sleep.ed[cluster][core]) {
		writel_relaxed(0xc5acce55U, t8103_sleep.ed[cluster][core] + 0xfb0);
		writel_relaxed(0, t8103_sleep.ed[cluster][core] + 0x310);
		dsb(sy);
	}

	/* The WFI mode and no retention, with the IRQ and FIQ modes cleared as t6000 has them */
	cyc &= ~(T8103_CYC_WFI_MODE | T8103_CYC_IRQ_MODE | T8103_CYC_FIQ_MODE);
	cyc |= FIELD_PREP(T8103_CYC_WFI_MODE, 3) | T8103_CYC_DISABLE_WFI_RET;
	/* The PMU counters and their interrupt off */
	write_sysreg_s(0, T8103_PMCR0);
	acc &= ~(T8103_ACC_PWR_DN_SRM | T8103_ACC_DIS_L2_FLUSH | T8103_ACC_TRAIN_DOWN_LINK |
		 T8103_ACC_POWER_DOWN_CPM);
	acc |= FIELD_PREP(T8103_ACC_PWR_DN_SRM, 3) | FIELD_PREP(T8103_ACC_DIS_L2_FLUSH, 2) |
	       FIELD_PREP(T8103_ACC_TRAIN_DOWN_LINK, 3) | FIELD_PREP(T8103_ACC_POWER_DOWN_CPM, 3) |
	       T8103_ACC_DISABLE_PIO_ON_WFI | T8103_ACC_DEEP_SLEEP;
	t8103_cpu_off(cyc, acc);
	unreachable();
}

/*
 * The first thing Linux runs after a wake, MMU off and no stack: the PAC controls put back,
 * then cpu_resume.
 */
asm(
"	.pushsection .text, \"ax\"\n"
"	.type	t8103_resume_stub, %function\n"
"t8103_resume_stub:\n"
"	adrp	x0, t8103_pac_saved\n"
"	add	x0, x0, :lo12:t8103_pac_saved\n"
/* The PAC controls are back at their reset values: put back what the kernel signed with */
"	ldr	x1, [x0, #16]\n"
"	cbz	x1, 8f\n"
"	ldr	x1, [x0]\n"
"	msr	s3_4_c15_c0_4, x1\n"
"	ldr	x1, [x0, #8]\n"
"	msr	s3_6_c15_c12_2, x1\n"
"	isb\n"
"8:\n"
"	b	cpu_resume\n"
"	.size	t8103_resume_stub, . - t8103_resume_stub\n"
"	.popsection\n"
);
void t8103_resume_stub(void);

/*
 * The cores' PAC control registers (APCTL_EL1, APCTL_EL2) come back from the core reset of a
 * wake at their reset values, not the ones the kernel's return addresses were signed with.
 * Saved here right before the commit; t8103_resume_stub puts them back, MMU off, before
 * cpu_resume. [2] says they're valid.
 */
u64 t8103_pac_saved[3] __aligned(64);

/* Right before the timer: the PAC controls, and this stack to the point of coherency */
static void t8103_pre_commit(struct apple_sleep *s)
{
	t8103_pac_saved[0] = read_sysreg_s(sys_reg(3, 4, 15, 0, 4));
	t8103_pac_saved[1] = read_sysreg_s(sys_reg(3, 6, 15, 12, 2));
	t8103_pac_saved[2] = 1;
	dcache_clean_poc((unsigned long)t8103_pac_saved,
			 (unsigned long)t8103_pac_saved + sizeof(t8103_pac_saved));
	dcache_clean_poc((unsigned long)task_stack_page(current),
			 (unsigned long)task_stack_page(current) + THREAD_SIZE);
}

/* E14 once, before the timer: the shared commit's call after it then changes nothing. */
static void t8103_aic_mask_all(void)
{
	if (t8103_sleep.aic_masked)
		return;
	aic_sleep_mask_all();
	t8103_sleep.aic_masked = true;
}

static void t8103_aic_restore(void)
{
	if (!t8103_sleep.aic_masked)
		return;
	aic_sleep_restore();
	t8103_sleep.aic_masked = false;
}

static void __iomem *t8103_map(struct apple_sleep *s, const char *name, resource_size_t min,
			       struct resource *res)
{
	struct device_node *np = s->dev->of_node;
	int index = of_property_match_string(np, "reg-names", name);

	if (index < 0 || of_address_to_resource(np, index, res))
		return IOMEM_ERR_PTR(-ENODEV);
	if (resource_size(res) < min)
		return IOMEM_ERR_PTR(-EINVAL);
	/* Shared with the drivers of these blocks, so not claimed. */
	return apple_sleep_map(s->dev, name);
}

/* Mapped for good, non-posted (a posted write to these blocks is an SError), or NULL */
static void __iomem *t8103_ioremap(struct apple_sleep *s, phys_addr_t pa, size_t len)
{
	void __iomem *base = ioremap_np(pa, len);

	if (base && devm_add_action_or_reset(s->dev, apple_sleep_iounmap, (void __force *)base))
		return NULL;
	return base;
}

static int t8103_sleep_map_regs(struct apple_sleep *s)
{
	struct t8103_sleep *t = &t8103_sleep;
	struct resource res;
	struct device_node *np;
	int i, c, ret;

	s->ps = t8103_map(s, "ps", T8103_SEP_PG + 4, &res);
	s->ps_aon = t8103_map(s, "ps-aon", T8103_S2R_TIMER + 4, &res);
	s->clk_mux = t8103_map(s, "clk-mux", T8103_MUX_SOC + 4 * ARRAY_SIZE(t->mux_snap), &res);
	s->wake = t8103_map(s, "wake", T8103_WAKE_STATUS + 4 * T8103_WAKE_WORDS, &res);
	s->cpu_start = t8103_map(s, "cpu-start", CPU_START_CLUSTER(1) + 4, &res);
	s->wdt = t8103_map(s, "wdt", WDT_WD1_CTL + 4, &res);
	s->spmi = t8103_map(s, "spmi", SPMI_NOTIFY_ACK + 4, &res);
	if (IS_ERR(s->ps) || IS_ERR(s->ps_aon) || IS_ERR(s->clk_mux) || IS_ERR(s->wake) ||
	    IS_ERR(s->cpu_start) || IS_ERR(s->wdt) || IS_ERR(s->spmi))
		return dev_err_probe(s->dev, -EINVAL,
				     "ps, ps-aon, clk-mux, wake, cpu-start, wdt and spmi must all be mapped\n");
	/*
	 * The CPU complexes' blocks and the cores' external debug blocks, the memory controller
	 * and cache, the AIC's per-CPU words, the memory clock's performance state, the
	 * coprocessors stopped at the commit and the fabric bridges: none of them in the sleep
	 * node's reg, each shared with its own driver where it has one, so not claimed.
	 */
	for (i = 0; i < 2; i++) {
		t->acc[i] = t8103_ioremap(s, i ? T8103_ACC_P : T8103_ACC_E, T8103_ACC_LEN);
		if (!t->acc[i])
			return dev_err_probe(s->dev, -ENOMEM, "ACC blocks not mapped\n");
		for (c = 0; c < 4; c++)
			t->ed[i][c] = t8103_ioremap(s, T8103_ED + i * T8103_ED_CLUSTER +
						       c * T8103_ED_CORE, 0x1000);
	}
	t->mcc_bcast = t8103_ioremap(s, T8103_MCC_BCAST, 0x60000);
	t->mcc_planes = t8103_ioremap(s, T8103_MCC_PLANES, T8103_MCC_NPLANES * T8103_MCC_STRIDE);
	t->dcs = t8103_ioremap(s, T8103_DCS_BASE, T8103_DCS_NCHAN * T8103_DCS_STRIDE);
	t->aic_cpu = t8103_ioremap(s, T8103_AIC_CPU, T8103_AIC_CPUS * T8103_AIC_CPU_STRIDE);
	t->nco = t8103_ioremap(s, T8103_NCO, T8103_NCO_CHANS * T8103_NCO_STRIDE);
	if (!t->mcc_bcast || !t->mcc_planes || !t->dcs || !t->aic_cpu || !t->nco)
		return dev_err_probe(s->dev, -ENOMEM,
				     "MCC, DCS, AIC CPU or NCO block not mapped\n");
	t->dcs_perf = t8103_ioremap(s, T8103_DCS_PERF, 4);
	t->pmp_asc = t8103_ioremap(s, T8103_PMP_ASC, 0x100);
	t->pmp_mbox = t8103_ioremap(s, T8103_PMP_MBOX, 0x1000);
	t->sio_asc = t8103_ioremap(s, T8103_SIO_ASC, 0x100);
	t->sio_mbox = t8103_ioremap(s, T8103_SIO_MBOX, 0x1000);
	t->dcp_mbox = t8103_ioremap(s, T8103_DCP_MBOX, 0x1000);
	t->dcpext_asc = t8103_ioremap(s, T8103_DCPEXT_ASC, 0x100);
	t->dcpext_mbox = t8103_ioremap(s, T8103_DCPEXT_MBOX, 0x1000);
	for (i = 0; i < ARRAY_SIZE(t8103_bridges); i++)
		t->bridges[i] = t8103_ioremap(s, t8103_bridges[i].pa, 0x1000);
	t->agx_fabric = t8103_ioremap(s, T8103_AGX_FABRIC, 0x1000);
	/* The commit word is the S2R timer, the clusters' words are in "ps". */
	s->commit = s->ps_aon + T8103_S2R_TIMER;
	s->cluster = s->ps;

	ret = apple_sleep_read_wake_sources(s);
	if (ret)
		return ret;
	for (i = 0; i < s->wake_enable_count; i++) {
		u32 id = s->wake_enable[i];

		if (id / 32 >= T8103_WAKE_WORDS || !(t8103_wake_valid[id / 32] & BIT(id % 32)))
			return dev_err_probe(s->dev, -EINVAL, "wake source %u is not valid\n", id);
		t->wanted[id / 32] |= BIT(id % 32);
	}

	np = of_find_compatible_node(NULL, NULL, "apple,agx-t8103");
	if (np) {
		t->gpu = of_find_device_by_node(np);
		of_node_put(np);
	}
	if (t->gpu &&
	    devm_add_action_or_reset(s->dev, apple_sleep_put_device, &t->gpu->dev))
		return -ENOMEM;

	/*
	 * The AP pins' controller stays powered through the noirq phase: SEP answers its sleep
	 * request at 0x85 only with the GPIO domain on, and the commit lowers it.
	 */
	np = of_parse_phandle(s->dev->of_node, "apple,pinctrl", 0);
	if (np) {
		struct platform_device *pctl = of_find_device_by_node(np);

		if (pctl) {
			dev_pm_syscore_device(&pctl->dev, true);
			put_device(&pctl->dev);
		}
		of_node_put(np);
	}

	dev_info(s->dev, "T8103: wake sources %08x %08x %08x %08x %08x\n", t->wanted[0],
		 t->wanted[1], t->wanted[2], t->wanted[3], t->wanted[4]);
	return 0;
}

/*
 * The AOP runs through the sleep and reaches DRAM: with its driver bound, the sleep keeps DRAM
 * up for it (SLP_DDR), as for the USB host's vote. With it unbound, or after a coprocessor
 * crash, or when this sleep was asked to go deep, DRAM goes down with the rest.
 */
static bool t8103_aop_bound(void)
{
	struct device_node *np = of_find_compatible_node(NULL, NULL, "apple,t8103-aop");
	struct platform_device *pdev = np ? of_find_device_by_node(np) : NULL;
	bool bound = pdev && device_is_bound(&pdev->dev);

	if (pdev)
		put_device(&pdev->dev);
	of_node_put(np);
	return bound;
}

static int t8103_sleep_begin(struct apple_sleep *s)
{
	struct t8103_sleep *t = &t8103_sleep;

	t->attempt = true;
	t->committed = false;
	t->saved = false;
	t->aic_masked = false;
	t->sep_pg_cleared = false;
	t->mux_timeouts = 0;
	t->cluster_timeouts = 0;
	t->mux_wake_timeouts = 0;
	t->gfx_wake_err = 0;
	t->nco_forced = 0;
	memset(t->cluster_wake_ret, 0, sizeof(t->cluster_wake_ret));
	t->aop_slp_ddr = t8103_aop_bound();
	if (t->aop_slp_ddr)
		dev_info(s->dev, "T8103: AOP bound: DRAM stays up for it\n");
	return 0;
}

/* The SoC's own refusals, before any other: nothing has been written yet. */
static int t8103_sleep_check(struct apple_sleep *s)
{
	struct t8103_sleep *t = &t8103_sleep;
	unsigned int i, wanted = 0;

	/* Refuse a bound GPU driver unless it registered a commit stop and registers are mapped. */
	if (t->gpu && device_is_bound(&t->gpu->dev)) {
		if (!apple_sleep_coproc_ready_node(t->gpu->dev.of_node)) {
			dev_err(s->dev, "refused: the GPU's driver is bound\n");
			return -EBUSY;
		}
		if (!t->agx_fabric || !t->bridges[0]) {
			dev_err(s->dev, "refused: GPU fabric or bridge registers not mapped\n");
			return -ENODEV;
		}
	}

	for (i = 0; i < T8103_WAKE_WORDS; i++)
		wanted += hweight32(t->wanted[i] & t8103_wake_valid[i]);
	if (!wanted) {
		dev_err(s->dev, "refused: no wake source enabled\n");
		return -EINVAL;
	}
	return 0;
}

/*
 * A coprocessor CPU's registers answer while its domain is at full power, or asked for full
 * power with AUTO_ENABLE, which brings an auto clock-gated one back on access (a running
 * DCP reads actual 4 this way). Not once its driver has lowered it: target 0, no AUTO_ENABLE.
 */
static bool t8103_cpu_answers(struct apple_sleep *s, u32 ps)
{
	u32 v = readl(s->ps + ps);

	return FIELD_GET(PS_ACTUAL, v) == 0xf ||
	       (FIELD_GET(PS_TARGET, v) == 0xf && (v & PS_AUTO_ENABLE));
}

/*
 * Nothing here talks to the display coprocessor or lowers its domains: its driver stops it
 * for the sleep where it can, and its state is only reported.
 */
static int t8103_sleep_display_off(struct apple_sleep *s)
{
	void __iomem *asc = s->display[0].asc;
	bool answers = s->display[0].bound && asc && t8103_cpu_answers(s, T8103_DISP0_CPU_PS);

	dev_info(s->dev, "T8103: display coprocessor %s, CPU power %#x, control %#x; left as it is\n",
		 s->display[0].bound ? "bound" : "unbound", readl(s->ps + T8103_DISP0_CPU_PS),
		 answers ? readl(asc + ASC_CPU_CONTROL) : 0);
	return 0;
}

static void t8103_pre_smc_down(struct apple_sleep *s);

/* Reversible: every word the commit and the wake write, saved. Nothing is written. */
static void t8103_sleep_prepare_regs(struct apple_sleep *s)
{
	struct t8103_sleep *t = &t8103_sleep;
	unsigned int i;

	for (i = 0; i < T8103_WAKE_WORDS; i++)
		t->wake_mask[i] = readl(s->wake + T8103_WAKE_MASK + 4 * i);
	for (i = 0; i < ARRAY_SIZE(t8103_mux_park); i++)
		t->mux[i] = readl(s->clk_mux + t8103_mux_park[i]);
	for (i = 0; i < ARRAY_SIZE(t8103_auto_off); i++)
		t->auto_word[i] = readl(s->ps + t8103_auto_off[i]);
	for (i = 0; i < ARRAY_SIZE(t8103_cpm); i++)
		t->cpm[i] = readl(s->ps + t8103_cpm[i]);
	for (i = 0; i < ARRAY_SIZE(t8103_cluster); i++)
		t->cluster[i] = readl(s->ps + t8103_cluster[i]);
	t->sep_pg = readl(s->ps + T8103_SEP_PG);
	t->spmi_vec = readl(s->spmi + T8103_SPMI_VEC_ENABLE);
	t->saved = true;
	if (s->entry_committed)
		t8103_pre_smc_down(s);
}

/* prepare_regs writes nothing, so neither does this. */
static void t8103_sleep_undo_regs(struct apple_sleep *s)
{
	t8103_sleep.saved = false;
}

/*
 * A coprocessor's RTKit idled over its management endpoint, then its CPU stopped. In the
 * commit, interrupts off, the boot core alone: nothing waits on it after.
 * Returns bit 0 acked, 1 CPU idle, 2 stopped, 4 already stopped, 5 not mapped.
 */
static u8 t8103_asc_stop(void __iomem *asc, void __iomem *m)
{
	unsigned int us, n;
	u8 result = 0;
	u64 msg0;
	u32 v;

	if (!asc || !m)
		return BIT(5);
	if (!(readl(asc + T8103_ASC_CPU_CONTROL) & T8103_ASC_CPU_RUN))
		return BIT(4);
	for (n = 0; n < 64 && !(readl(m + T8103_MBOX_I2A_CONTROL) & T8103_MBOX_EMPTY); n++) {
		readq(m + T8103_MBOX_I2A_RECV0);
		readq(m + T8103_MBOX_I2A_RECV1);
	}
	for (us = 0; us < T8103_PMP_ACK_US && (readl(m + T8103_MBOX_A2I_CONTROL) & T8103_MBOX_FULL);
	     us += 10)
		udelay(10);
	writeq(FIELD_PREP(T8103_RTKIT_TYPE, T8103_RTKIT_SET_IOP_PWR) |
	       FIELD_PREP(T8103_RTKIT_PWR, T8103_RTKIT_PWR_IDLE), m + T8103_MBOX_A2I_SEND0);
	writeq(0, m + T8103_MBOX_A2I_SEND1);
	for (us = 0; us < T8103_PMP_ACK_US; us += 10) {
		if (readl(m + T8103_MBOX_I2A_CONTROL) & T8103_MBOX_EMPTY) {
			udelay(10);
			continue;
		}
		msg0 = readq(m + T8103_MBOX_I2A_RECV0);
		readq(m + T8103_MBOX_I2A_RECV1);
		if (FIELD_GET(T8103_RTKIT_TYPE, msg0) == T8103_RTKIT_IOP_PWR_ACK &&
		    FIELD_GET(T8103_RTKIT_PWR, msg0) == T8103_RTKIT_PWR_IDLE) {
			result |= BIT(0);
			break;
		}
	}
	/* Its outbox off, then RUN cleared once it idles */
	writel(readl(m + T8103_MBOX_I2A_CONTROL) & ~T8103_MBOX_RECV_ENABLE,
	       m + T8103_MBOX_I2A_CONTROL);
	if (!readl_poll_timeout_atomic(asc + T8103_ASC_CPU_STATUS, v,
				       (v & T8103_ASC_CPU_IDLE) == T8103_ASC_CPU_IDLE, 1,
				       T8103_PMP_IDLE_US))
		result |= BIT(1);
	writel(readl(asc + T8103_ASC_CPU_CONTROL) & ~T8103_ASC_CPU_RUN,
	       asc + T8103_ASC_CPU_CONTROL);
	return result | BIT(2);
}

static void t8103_pmp_stop(struct apple_sleep *s)
{
	struct t8103_sleep *t = &t8103_sleep;

	t->pmp_result = t8103_asc_stop(t->pmp_asc, t->pmp_mbox);
	t->sio_result = 0;
	if (FIELD_GET(PS_ACTUAL, readl(s->ps + 0x1d0)))
		t->sio_result = t8103_asc_stop(t->sio_asc, t->sio_mbox);
	/*
	 * Both display coprocessors: bound and running; drivers restart them at the wake. Not
	 * once a driver has lowered the CPU's domain itself: its registers don't answer then.
	 */
	t->dcp_result = 0;
	if (s->display[0].bound && t8103_cpu_answers(s, T8103_DISP0_CPU_PS))
		t->dcp_result = t8103_asc_stop(s->display[0].asc, t->dcp_mbox);
	t->dcpext_result = 0;
	if (t8103_cpu_answers(s, T8103_DISPEXT_CPU_PS))
		t->dcpext_result = t8103_asc_stop(t->dcpext_asc, t->dcpext_mbox);
}

/*
 * The memory controller's platform error reporting off, its and the DCS channels' dynamic
 * clock and power gating on, and the DCS write queues flushed, each as it was put back at the
 * wake.
 */
static void t8103_dcs_quiesce(struct apple_sleep *s)
{
	struct t8103_sleep *t = &t8103_sleep;
	unsigned int ch, p;

	t->amcnp_err_saved = readl(t->mcc_bcast + T8103_MCC_AMCNP_ERR);
	writel(0, t->mcc_bcast + T8103_MCC_AMCNP_ERR);
	for (p = 0; p < T8103_MCC_NPLANES; p++) {
		void __iomem *base = t->mcc_planes + p * T8103_MCC_STRIDE;

		t->amcc_err0_saved[p] = readl(base + T8103_MCC_ERR_EN0);
		t->amcc_err1_saved[p] = readl(base + T8103_MCC_ERR_EN1);
		writel(0, base + T8103_MCC_ERR_EN1);
		writel(0, base + T8103_MCC_ERR_EN0);
	}
	for (ch = 0; ch < T8103_DCS_NCHAN; ch++) {
		void __iomem *reg = t->dcs + ch * T8103_DCS_STRIDE + T8103_DCS_ERR_EN;

		t->dcs_err_saved[ch] = readl(reg);
		writel(0, reg);
	}
	t->mcc_lp_saved = readl(t->mcc_bcast + T8103_MCC_BCAST_LP);
	writel(t->mcc_lp_saved | T8103_MCC_BCAST_LP_BITS, t->mcc_bcast + T8103_MCC_BCAST_LP);
	for (ch = 0; ch < T8103_DCS_NCHAN; ch++) {
		void __iomem *reg = t->dcs + ch * T8103_DCS_STRIDE + T8103_DCS_CTL;

		t->dcs_ctl_saved[ch] = readl(reg);
		writel(t->dcs_ctl_saved[ch] | T8103_DCS_CTL_LP, reg);
	}
	for (ch = 0; ch < T8103_DCS_NCHAN; ch++) {
		void __iomem *reg = t->dcs + ch * T8103_DCS_STRIDE + T8103_DCS_WQ_FLUSH;

		t->dcs_wq_saved[ch] = readl(reg);
		writel(t->dcs_wq_saved[ch] & 0xffff0000U, reg);
	}
	/* The flush's time: the queues have no status to poll */
	udelay(T8103_DCS_FLUSH_US);
	t->dcs_quiesced = true;
}

static void t8103_dcs_restore(struct apple_sleep *s)
{
	struct t8103_sleep *t = &t8103_sleep;
	unsigned int ch, p;

	if (!t->dcs_quiesced)
		return;
	t->dcs_quiesced = false;
	for (ch = 0; ch < T8103_DCS_NCHAN; ch++)
		writel(t->dcs_wq_saved[ch], t->dcs + ch * T8103_DCS_STRIDE + T8103_DCS_WQ_FLUSH);
	for (ch = 0; ch < T8103_DCS_NCHAN; ch++)
		writel(t->dcs_ctl_saved[ch], t->dcs + ch * T8103_DCS_STRIDE + T8103_DCS_CTL);
	writel(t->mcc_lp_saved, t->mcc_bcast + T8103_MCC_BCAST_LP);
	for (ch = 0; ch < T8103_DCS_NCHAN; ch++)
		writel(t->dcs_err_saved[ch], t->dcs + ch * T8103_DCS_STRIDE + T8103_DCS_ERR_EN);
	for (p = 0; p < T8103_MCC_NPLANES; p++) {
		void __iomem *base = t->mcc_planes + p * T8103_MCC_STRIDE;

		writel(t->amcc_err1_saved[p], base + T8103_MCC_ERR_EN1);
		writel(t->amcc_err0_saved[p], base + T8103_MCC_ERR_EN0);
	}
	writel(t->amcnp_err_saved, t->mcc_bcast + T8103_MCC_AMCNP_ERR);
}

/* The ways set to @ways on every plane at once, then each plane waited for: 0 on success */
static int t8103_slc_set(struct apple_sleep *s, u32 ways)
{
	struct t8103_sleep *t = &t8103_sleep;
	unsigned int p, bad = 0;
	u32 v;

	writel(1, t->mcc_bcast + T8103_MCC_BCAST_A);
	writel(1, t->mcc_bcast + T8103_MCC_BCAST_B);
	writel(ways, t->mcc_planes + T8103_MCC_WAYS);
	writel(0, t->mcc_bcast + T8103_MCC_BCAST_A);
	writel(0, t->mcc_bcast + T8103_MCC_BCAST_B);
	for (p = 0; p < T8103_MCC_NPLANES; p++) {
		void __iomem *reg = t->mcc_planes + p * T8103_MCC_STRIDE + T8103_MCC_WAYS_STATUS;

		if (readl_poll_timeout_atomic(reg, v, FIELD_GET(T8103_MCC_WAYS_A, v) == ways, 1,
					      T8103_MCC_TIMEOUT_US) ||
		    readl_poll_timeout_atomic(reg, v, FIELD_GET(T8103_MCC_WAYS_B, v) == ways, 1,
					      T8103_MCC_TIMEOUT_US))
			bad |= BIT(p);
	}
	return bad;
}

/* The memory cache's ways off before the CPU complexes' quiesce, as macOS has it */
static void t8103_slc_off(struct apple_sleep *s)
{
	struct t8103_sleep *t = &t8103_sleep;
	u32 v = readl(t->mcc_planes + T8103_MCC_WAYS_STATUS);

	t->slc_result = 0;
	t->slc_ways = FIELD_GET(T8103_MCC_WAYS_B, v);
	if (!t->slc_ways)
		return;
	t->slc_result = BIT(7) | t8103_slc_set(s, 0);
}

static void t8103_slc_on(struct apple_sleep *s)
{
	struct t8103_sleep *t = &t8103_sleep;

	if (!(t->slc_result & BIT(7)))
		return;
	t->slc_result = 0;
	t8103_slc_set(s, t->slc_ways);
}

/* After the AIC's mask: each CPU's IPIs masked and drained, as macOS has them */
static void t8103_aic_ipi_quiesce(struct apple_sleep *s)
{
	struct t8103_sleep *t = &t8103_sleep;
	unsigned int i;
	u32 v;

	t->aic_result = 0;
	for (i = 0; i < T8103_AIC_CPUS; i++) {
		void __iomem *c = t->aic_cpu + i * T8103_AIC_CPU_STRIDE;

		t->aic_ipi_mask[i] = readl(c + T8103_AIC_CPU_MASK_CLR);
		writel(T8103_AIC_IPI_MASK, c + T8103_AIC_CPU_MASK_SET);
		if (readl_poll_timeout_atomic(c + T8103_AIC_CPU_STATUS, v,
					      !(v & T8103_AIC_CPU_PENDING), 1, 10000))
			t->aic_result |= BIT(i);
	}
}

static void t8103_aic_ipi_restore(struct apple_sleep *s)
{
	struct t8103_sleep *t = &t8103_sleep;
	unsigned int i;

	for (i = 0; i < T8103_AIC_CPUS; i++) {
		void __iomem *c = t->aic_cpu + i * T8103_AIC_CPU_STRIDE;

		writel(t->aic_ipi_mask[i], c + T8103_AIC_CPU_MASK_SET);
		writel(~t->aic_ipi_mask[i], c + T8103_AIC_CPU_MASK_CLR);
	}
}

/* The quiesce, irreversible: in the commit, interrupts off, the boot core alone */
static void t8103_quiesce(struct apple_sleep *s)
{
	struct t8103_sleep *t = &t8103_sleep;
	unsigned int c, i;
	u64 q;
	u32 v;

	t->quiesced = false;
	t->quiesce_timeouts = 0;
	for (c = 0; c < 2; c++) {
		t->acc_energy[c] = readq(t->acc[c] + T8103_ACC_ENERGY);
		writeq(t->acc_energy[c] & ~T8103_ACC_ENERGY_EN, t->acc[c] + T8103_ACC_ENERGY);
	}
	for (c = 0; c < 2; c++) {
		t->acc_dvfs[c] = readq(t->acc[c] + T8103_ACC_DVFS);
		writeq(t->acc_dvfs[c] & ~T8103_ACC_DVFS_RELOCK, t->acc[c] + T8103_ACC_DVFS);
	}
	for (i = 0; i < ARRAY_SIZE(t8103_acc_thrtl); i++)
		for (c = 0; c < 2; c++) {
			void __iomem *reg = t->acc[c] + t8103_acc_thrtl[i];

			t->acc_thrtl[c][i] = readq(reg);
			writeq(t->acc_thrtl[c][i] & ~T8103_THRTL_EN, reg);
		}
	for (i = 0; i < ARRAY_SIZE(t8103_tvm); i++) {
		t->tvm[i] = readl(s->ps + t8103_tvm[i]);
		writel(t->tvm[i] & ~T8103_TVM_EN, s->ps + t8103_tvm[i]);
		if (readl_poll_timeout_atomic(s->ps + t8103_tvm[i] + T8103_TVM_IDLE_OFF, v,
					      !(v & T8103_TVM_BUSY), 1, T8103_QUIESCE_US))
			t->quiesce_timeouts++;
	}
	for (c = 0; c < 2; c++) {
		void __iomem *reg = t->acc[c] + T8103_ACC_DVFS;

		writeq(readq(reg) | T8103_ACC_DVFS_APSC_OFF, reg);
		if (readq_poll_timeout_atomic(reg, q, !(q & T8103_ACC_DVFS_PENDING), 1,
					      T8103_QUIESCE_US))
			t->quiesce_timeouts++;
	}
	t->quiesced = true;
}

/* At the wake, in reverse: only the bits the quiesce changed */
static void t8103_unquiesce(struct apple_sleep *s)
{
	struct t8103_sleep *t = &t8103_sleep;
	unsigned int c, i;

	if (!t->quiesced)
		return;
	t->quiesced = false;
	for (c = 0; c < 2; c++) {
		void __iomem *reg = t->acc[c] + T8103_ACC_DVFS;
		u64 q;

		if (t->cluster_wake_ret[c])
			continue;
		q = readq(reg);
		q = (q & ~(T8103_ACC_DVFS_APSC_OFF | T8103_ACC_DVFS_RELOCK)) |
		    (t->acc_dvfs[c] & (T8103_ACC_DVFS_APSC_OFF | T8103_ACC_DVFS_RELOCK));
		writeq(q, reg);
	}
	for (i = 0; i < ARRAY_SIZE(t8103_tvm); i++)
		if (t->tvm[i] & T8103_TVM_EN)
			writel(readl(s->ps + t8103_tvm[i]) | T8103_TVM_EN, s->ps + t8103_tvm[i]);
	for (i = 0; i < ARRAY_SIZE(t8103_acc_thrtl); i++)
		for (c = 0; c < 2; c++) {
			void __iomem *reg = t->acc[c] + t8103_acc_thrtl[i];

			if (t->cluster_wake_ret[c])
				continue;
			writeq(readq(reg) | (t->acc_thrtl[c][i] & T8103_THRTL_EN), reg);
		}
	for (c = 0; c < 2; c++) {
		void __iomem *reg = t->acc[c] + T8103_ACC_ENERGY;

		if (!t->cluster_wake_ret[c])
			writeq(readq(reg) | (t->acc_energy[c] & T8103_ACC_ENERGY_EN), reg);
	}
}

/*
 * At 0x87, after SEP's sleep and before the SMC's last keys, as t6000 has it: SEP's domain
 * forced off (FORCE_NOACCESS, no minimum) and waited for, the PMP, SIO and both display
 * coprocessors stopped, then the always-on PMGR's USB and debug domains and the main PMGR's
 * stopped blocks lowered (gpio last, at the commit), with the fabric bridges' handshake
 * first. The always-on domains that should come back by themselves get AUTO_ENABLE.
 */
static void t8103_pre_smc_down(struct apple_sleep *s)
{
	struct t8103_sleep *t = &t8103_sleep;
	unsigned int i;
	u32 v;

	t->sep_ps_gated = -ENODATA;
	if (apple_sleep_enclave.stop) {
		v = readl(s->ps + T8103_SEP_PS);
		writel((v & ~0x0f000700U) | BIT(10), s->ps + T8103_SEP_PS);
		t->sep_ps_gated = readl_poll_timeout_atomic(s->ps + T8103_SEP_PS, v,
							    !FIELD_GET(PS_ACTUAL, v), 10,
							    T8103_SEP_GATE_US);
		t->sep_ps = v;
	}
	t->commit_off = 0;
	t->commit_lowered = 0;
	t->agx_saved_valid = false;
	if (t->gpu && device_is_bound(&t->gpu->dev)) {
		struct apple_sleep_coproc *gc = apple_sleep_coproc_find(&t->gpu->dev);

		if (gc && gc->stop_ret)
			panic("T8103 committed GPU stop failed: %d", gc->stop_ret);
	}
	/*
	 * Lower GFX (and save its fabric tunables) before stopping PMP, matching macOS and
	 * newer SoCs where GFX powers off while PMP is still running.
	 */
	if (!sleep_ps_on(s, t8103_commit_off[T8103_COMMIT_OFF_GFX])) {
		t->commit_off |= BIT(T8103_COMMIT_OFF_GFX);
	} else {
		if (t->agx_fabric) {
			for (i = 0; i < ARRAY_SIZE(t8103_agx_regs); i++)
				t->agx_saved[i] = readl(t->agx_fabric + t8103_agx_regs[i]);
			t->agx_saved_valid = true;
		}
		if (t->bridges[0]) {
			writel(1, t->bridges[0] + T8103_BRIDGE_8014);
			writel(1, t->bridges[0] + T8103_BRIDGE_82BC);
			writel(2, t->bridges[0] + T8103_BRIDGE_82BC);
			writel(4, t->bridges[0] + T8103_BRIDGE_82BC);
		}
		t->commit_saved[T8103_COMMIT_OFF_GFX] =
			readl(s->ps + t8103_commit_off[T8103_COMMIT_OFF_GFX]);
		t->commit_lowered |= BIT(T8103_COMMIT_OFF_GFX);
		sleep_ps_lower(s, t8103_commit_off[T8103_COMMIT_OFF_GFX]);
		if (sleep_ps_wait_off(s, t8103_commit_off[T8103_COMMIT_OFF_GFX]))
			panic("T8103 committed GFX power-off timed out");
		t->commit_off |= BIT(T8103_COMMIT_OFF_GFX);
	}
	t8103_pmp_stop(s);
	t->aon_commit_off = 0;
	for (i = 0; i < ARRAY_SIZE(t8103_aon_commit_off); i++) {
		void __iomem *reg = s->ps_aon + t8103_aon_commit_off[i];
		u32 av = readl(reg);

		if (!FIELD_GET(PS_ACTUAL, av)) {
			t->aon_commit_off |= BIT(i);
			continue;
		}
		writel((av & ~(PS_TARGET | PS_AUTO_ENABLE)) | BIT(8) | BIT(9), reg);
		if (!readl_poll_timeout_atomic(reg, av, !FIELD_GET(PS_ACTUAL, av), 1, 10000))
			t->aon_commit_off |= BIT(i);
	}
	for (i = 1; i < ARRAY_SIZE(t8103_commit_off) - 1; i++) {
		unsigned int b;

		if (!sleep_ps_on(s, t8103_commit_off[i])) {
			t->commit_off |= BIT(i);
			continue;
		}
		for (b = 0; b < ARRAY_SIZE(t8103_bridges); b++) {
			if (t8103_bridges[b].ps_off != t8103_commit_off[i] || !t->bridges[b])
				continue;
			writel(1, t->bridges[b] + T8103_BRIDGE_8014);
			writel(1, t->bridges[b] + T8103_BRIDGE_82BC);
			writel(2, t->bridges[b] + T8103_BRIDGE_82BC);
			writel(4, t->bridges[b] + T8103_BRIDGE_82BC);
		}
		t->commit_saved[i] = readl(s->ps + t8103_commit_off[i]);
		t->commit_lowered |= BIT(i);
		sleep_ps_lower(s, t8103_commit_off[i]);
		if (!sleep_ps_wait_off(s, t8103_commit_off[i]))
			t->commit_off |= BIT(i);
	}
	t->cg_commit_off = 0;
	for (i = 0; i < ARRAY_SIZE(t8103_cg_commit_off); i++) {
		void __iomem *reg = s->ps + t8103_cg_commit_off[i];
		u32 cv = readl(reg);

		t->cg_saved[i] = cv;
		if (!FIELD_GET(PS_ACTUAL, cv)) {
			t->cg_commit_off |= BIT(i);
			continue;
		}
		sleep_ps_lower(s, t8103_cg_commit_off[i]);
		if (!sleep_ps_wait_off(s, t8103_cg_commit_off[i]))
			t->cg_commit_off |= BIT(i);
	}
	for (i = 0; i < ARRAY_SIZE(t8103_ps_boot_auto); i++) {
		void __iomem *reg = s->ps + t8103_ps_boot_auto[i];
		u32 bv = readl(reg);

		if ((bv & PS_AUTO_ENABLE) || !FIELD_GET(PS_TARGET, bv) || !FIELD_GET(PS_ACTUAL, bv))
			continue;
		writel(bv & ~PS_FLAGS, reg);
		readl(reg);
		writel((bv & ~PS_FLAGS) | PS_AUTO_ENABLE, reg);
	}
	for (i = 0; i < ARRAY_SIZE(t8103_aon_boot_auto); i++) {
		void __iomem *reg = s->ps_aon + t8103_aon_boot_auto[i];
		u32 bv = readl(reg);

		if ((bv & PS_AUTO_ENABLE) || !FIELD_GET(PS_TARGET, bv) || !FIELD_GET(PS_ACTUAL, bv))
			continue;
		writel(bv & ~PS_FLAGS, reg);
		readl(reg);
		writel((bv & ~PS_FLAGS) | PS_AUTO_ENABLE, reg);
	}
}

/*
 * The commit: the memory clock stepped down, the memory controller and cache quiesced, then
 * the wake mask, the clock muxes, the CPU complexes' quiesce, CPM gating, the clusters,
 * AUTO_ENABLE and SEP's power-gate wait in the order the hardware takes them, then the
 * watchdog, the alarm's vector, the PMU's boot stage and the AIC. The shared commit then arms
 * the mailbox and writes the S2R timer. Nothing printed, nothing can fail: a timeout is kept
 * for the report.
 */
static void t8103_sleep_commit_regs(struct apple_sleep *s)
{
	struct t8103_sleep *t = &t8103_sleep;
	unsigned int i;
	u32 v;

	t->committed = true;
	/* A hang in the writes below still ends in a reset. */
	apple_sleep_set_wd2(s, T8103_WDT_COMMIT_SECS);
	t->dcs_perf_down = false;
	if (t->dcs_perf) {
		/* The other cores are parked and the coprocessors that use memory are stopped. */
		t->dcs_perf_saved = readl(t->dcs_perf);
		t->dcs_perf_down_ok = t8103_dcs_step_to(t->dcs_perf, T8103_DCS_SLEEP,
							&t->dcs_perf_down_val);
		t->dcs_perf_down = true;
	}
	/* gpio, the last of t8103_commit_off, once SEP and the SMC are done with it */
	i = ARRAY_SIZE(t8103_commit_off) - 1;
	if (!sleep_ps_on(s, t8103_commit_off[i])) {
		t->commit_off |= BIT(i);
	} else {
		sleep_ps_lower(s, t8103_commit_off[i]);
		if (!sleep_ps_wait_off(s, t8103_commit_off[i]))
			t->commit_off |= BIT(i);
	}
	t8103_dcs_quiesce(s);
	t8103_slc_off(s);

	/*
	 * E1: DRAM kept up through the sleep for the AOP (SLP_DDR), unless it crashed or this
	 * sleep goes deep; else its vote cleared. Then every valid source masked but the wanted
	 * ones and those the platform always leaves on.
	 */
	t->wake_slp_ddr = readl(s->wake + T8103_WAKE_SLP_DDR);
	t->wake_slp_ddr_changed = false;
	if (t->aop_slp_ddr && !sleep_slp_ddr_crashed && !sleep_slp_ddr_deep_once) {
		writel(t->wake_slp_ddr | T8103_WAKE_SLP_DDR_ON, s->wake + T8103_WAKE_SLP_DDR);
		t->wake_slp_ddr_changed = true;
	} else if (t->wake_slp_ddr & T8103_WAKE_SLP_DDR_ON) {
		writel(t->wake_slp_ddr & ~T8103_WAKE_SLP_DDR_ON, s->wake + T8103_WAKE_SLP_DDR);
		t->wake_slp_ddr_changed = true;
	}
	for (i = 0; i < T8103_WAKE_WORDS; i++) {
		u32 mask = t8103_wake_platform[i] & ~t->wanted[i];

		writel(mask, s->wake + T8103_WAKE_MASK + 4 * i);
		s->wake_armed[i] = t8103_wake_valid[i] & ~mask;
	}
	/* E2: the latched sources cleared */
	for (i = 0; i < T8103_WAKE_WORDS; i++) {
		v = readl(s->wake + T8103_WAKE_STATUS + 4 * i);
		t->wake_entry[i] = v;
		writel(v, s->wake + T8103_WAKE_STATUS + 4 * i);
	}
	/* E3: the SoC clock muxes parked, each waited for */
	for (i = 0; i < ARRAY_SIZE(t8103_mux_park); i++) {
		void __iomem *reg = s->clk_mux + t8103_mux_park[i];

		writel((readl(reg) & T8103_MUX_PARK_KEEP) | T8103_MUX_PARK_SET, reg);
		if (readl_poll_timeout_atomic(reg, v, !(v & T8103_MUX_BUSY), 1,
					      T8103_MUX_TIMEOUT_US))
			t->mux_timeouts++;
	}
	/*
	 * Externally-clocked MCA power domains (ps_mca0..5) hang BUSY in the sleep sequencer
	 * if their NCO channel is disabled. Enable any disabled channel for the commit.
	 */
	t->nco_forced = 0;
	for (i = 0; i < T8103_NCO_CHANS; i++) {
		void __iomem *reg = t->nco + i * T8103_NCO_STRIDE;

		v = readl(reg);
		if (!(v & T8103_NCO_CTRL_EN)) {
			writel(v | T8103_NCO_CTRL_EN, reg);
			t->nco_forced |= BIT(i);
		}
	}
	for (i = 0; i < ARRAY_SIZE(t->mux_snap); i++)
		t->mux_snap[i] = readl(s->clk_mux + T8103_MUX_SOC + i * 4);
	/* E4: the CPU complexes' and voltage monitors' quiesce */
	t8103_quiesce(s);
	/* E5: CPM power gating on */
	for (i = 0; i < ARRAY_SIZE(t8103_cpm); i++)
		writel(readl(s->ps + t8103_cpm[i]) | T8103_CPM_GATING, s->ps + t8103_cpm[i]);
	/* E6, E7: E cluster to 0 first, then P cluster to 0 */
	writel(readl(s->ps + T8103_CLUSTER_E) & ~PS_TARGET, s->ps + T8103_CLUSTER_E);
	writel(readl(s->ps + T8103_CLUSTER_P) & ~PS_TARGET, s->ps + T8103_CLUSTER_P);
	if (readl_poll_timeout_atomic(s->ps + T8103_CLUSTER_P, v, !(v & T8103_CLUSTER_BUSY), 1,
				      CLUSTER_TIMEOUT_US))
		t->cluster_timeouts++;
	/* E8: AUTO_ENABLE off on the always-on words */
	for (i = 0; i < ARRAY_SIZE(t8103_auto_off); i++)
		writel(readl(s->ps + t8103_auto_off[i]) & ~PS_AUTO_ENABLE,
		       s->ps + t8103_auto_off[i]);
	/*
	 * E9: the S2R engine doesn't wait for SEP's power gate unless SEP was asked to sleep and
	 * gated, as the reference waits for before its last SMC step: an engine left waiting for
	 * a gate that never comes would stall with the watchdog standing still.
	 */
	t->sep_ps_gated = -ENODATA;
	if (apple_sleep_enclave.stop) {
		t->sep_ps_gated = readl_poll_timeout_atomic(s->ps + T8103_SEP_PS, v,
							    !FIELD_GET(PS_ACTUAL, v), 10,
							    T8103_SEP_GATE_US);
		t->sep_ps = v;
	}
	if (!apple_sleep_enclave.stop || t->sep_ps_gated) {
		writel(readl(s->ps + T8103_SEP_PG) & ~T8103_SEP_PG_WAIT, s->ps + T8103_SEP_PG);
		t->sep_pg_cleared = true;
	}
	dsb(sy);
	/* E11: the watchdog from here to the wake */
	apple_sleep_set_wd2(s, T8103_WDT_COMMIT_SECS);
	/*
	 * E12: the PMU RTC alarm's SPMI vector acknowledged, then on, so an alarm its RTC driver
	 * set reaches the wake logic as source 54.
	 */
	writel(T8103_SPMI_VEC_RTC, s->spmi + T8103_SPMI_VEC_ACK);
	writel(readl(s->spmi + T8103_SPMI_VEC_ENABLE) | T8103_SPMI_VEC_RTC,
	       s->spmi + T8103_SPMI_VEC_ENABLE);
	/* E13: the PMU told the AP sleeps */
	t8103_pmu_write(s, T8103_PMU_BOOT_STAGE, T8103_PMU_STAGE_SLEEP);
	/* E14: every interrupt line masked, then one already raised taken */
	t8103_aic_mask_all();
	aic_sleep_ack();
	t8103_aic_ipi_quiesce(s);
	dsb(sy);
}

/* After the timer and the AIC's mask, the timer once more, as macOS writes it again then */
static void t8103_post_timer(struct apple_sleep *s)
{
	dsb(sy);
	writel(1, s->commit);
}

/* W3 to W7, on the boot core with the AIC restored, before any driver resumes */
static void t8103_sleep_wake(struct apple_sleep *s)
{
	struct t8103_sleep *t = &t8103_sleep;
	unsigned int i;
	int pmp_ret = 0;
	u32 v;

	t8103_aic_ipi_restore(s);
	t8103_slc_on(s);
	t8103_dcs_restore(s);
	if (t->dcs_perf_down && t->dcs_perf) {
		/* The wake left it at T8103_DCS_WAKE, or the sleep never happened. */
		t->dcs_perf_wake_val = readl(t->dcs_perf);
		t->dcs_perf_up_ok = t8103_dcs_step_to(t->dcs_perf, t->dcs_perf_saved & 0xf,
						      &t->dcs_perf_up_val);
		t->dcs_perf_down = false;
	}

	/* W3: the clusters back, E first */
	for (i = 0; i < ARRAY_SIZE(t8103_cluster); i++) {
		void __iomem *reg = s->ps + t8103_cluster[i];

		writel((readl(reg) & ~PS_TARGET) | CLUSTER_ON, reg);
		t->cluster_wake_ret[i] = readl_poll_timeout_atomic(reg, v,
								   !(v & T8103_CLUSTER_BUSY), 1,
								   CLUSTER_TIMEOUT_US);
	}
	s->cluster_e_ret = t->cluster_wake_ret[0];
	s->cluster_p_ret = t->cluster_wake_ret[1];

	/* W4: AUTO_ENABLE back where it was */
	for (i = 0; i < ARRAY_SIZE(t8103_auto_off); i++)
		if (t->auto_word[i] & PS_AUTO_ENABLE)
			writel(readl(s->ps + t8103_auto_off[i]) | PS_AUTO_ENABLE,
			       s->ps + t8103_auto_off[i]);
	/* W5: CPM gating as it was */
	for (i = 0; i < ARRAY_SIZE(t8103_cpm); i++)
		writel(t->cpm[i], s->ps + t8103_cpm[i]);
	/*
	 * W6: the clock muxes as they were. The sleep's own sequence leaves some muxes the
	 * commit had enabled parked or off (storage's among them): every one that differs from
	 * the commit goes back, then the parked ones.
	 */
	t->mux_fixed = 0;
	for (i = 0; i < ARRAY_SIZE(t->mux_snap); i++) {
		void __iomem *reg = s->clk_mux + T8103_MUX_SOC + i * 4;

		if (readl(reg) == t->mux_snap[i] || !(t->mux_snap[i] & BIT(31)))
			continue;
		writel(t->mux_snap[i], reg);
		if (readl_poll_timeout_atomic(reg, v, !(v & T8103_MUX_BUSY), 1,
					      T8103_MUX_TIMEOUT_US))
			t->mux_wake_timeouts++;
		t->mux_fixed |= BIT_ULL(i);
	}
	for (i = 0; i < ARRAY_SIZE(t8103_mux_park); i++) {
		void __iomem *reg = s->clk_mux + t8103_mux_park[i];

		writel(t->mux[i], reg);
		if (readl_poll_timeout_atomic(reg, v, !(v & T8103_MUX_BUSY), 1,
					      T8103_MUX_TIMEOUT_US))
			t->mux_wake_timeouts++;
	}
	/* Turn back off any NCO channel the commit enabled so clk-apple-nco state matches. */
	for (i = 0; i < T8103_NCO_CHANS; i++) {
		void __iomem *reg = t->nco + i * T8103_NCO_STRIDE;

		if (t->nco_forced & BIT(i))
			writel(readl(reg) & ~T8103_NCO_CTRL_EN, reg);
	}
	t->nco_forced = 0;
	/*
	 * A cluster's ACC answers only while the cluster is powered: after a real sleep both
	 * were off until W3, and an access before that is an external abort.
	 */
	t8103_unquiesce(s);
	/* SEP's power-gate wait, the alarm's vector and the wake mask as they were */
	if (t->sep_pg_cleared)
		writel(t->sep_pg, s->ps + T8103_SEP_PG);
	if (apple_sleep_enclave.stop) {
		v = readl(s->ps + T8103_SEP_PS);
		writel((v & ~0x0f000700U) | (0xfU << 24) | BIT(9), s->ps + T8103_SEP_PS);
	}
	writel(t->spmi_vec, s->spmi + T8103_SPMI_VEC_ENABLE);

	/* The clock-gated domains the commit lowered */
	for (i = 0; i < ARRAY_SIZE(t8103_cg_commit_off); i++) {
		if (t->cg_commit_off & BIT(i))
			sleep_ps_restore(s->ps + t8103_cg_commit_off[i], t->cg_saved[i]);
	}
	/*
	 * The apcie and gpio domains the commit found off or couldn't lower, parents first; the
	 * PMGR driver brings back the ones it lowered.
	 */
	for (i = ARRAY_SIZE(t8103_commit_off); i > T8103_COMMIT_OFF_APCIE_START; i--) {
		unsigned int idx = i - 1;

		if (t->commit_off & BIT(idx))
			sleep_ps_restore(s->ps + t8103_commit_off[idx], 0xf);
	}
	if (t->commit_lowered & BIT(T8103_COMMIT_OFF_PMP))
		pmp_ret = sleep_ps_restore(s->ps + t8103_commit_off[T8103_COMMIT_OFF_PMP],
					   t->commit_saved[T8103_COMMIT_OFF_PMP]);
	if (t->commit_lowered & BIT(T8103_COMMIT_OFF_RMX))
		sleep_ps_restore(s->ps + t8103_commit_off[T8103_COMMIT_OFF_RMX],
				 t->commit_saved[T8103_COMMIT_OFF_RMX]);
	if (t->commit_lowered & BIT(T8103_COMMIT_OFF_GFX)) {
		int gfx_ret = pmp_ret ? pmp_ret :
			sleep_ps_restore(s->ps + t8103_commit_off[T8103_COMMIT_OFF_GFX],
					 t->commit_saved[T8103_COMMIT_OFF_GFX]);

		if (!gfx_ret && t->agx_saved_valid && t->agx_fabric) {
			for (i = 0; i < ARRAY_SIZE(t8103_agx_regs); i++)
				writel(t->agx_saved[i], t->agx_fabric + t8103_agx_regs[i]);
		}
		t->gfx_wake_err = gfx_ret;
		if (gfx_ret && t->gpu) {
			struct apple_sleep_coproc *gc = apple_sleep_coproc_find(&t->gpu->dev);

			if (gc)
				gc->wake_err = gfx_ret;
		}
	}
	t->commit_lowered = 0;
	for (i = ARRAY_SIZE(t8103_aon_commit_off); i > 0; i--) {
		unsigned int idx = i - 1;

		if (t->aon_commit_off & BIT(idx))
			sleep_ps_restore(s->ps_aon + t8103_aon_commit_off[idx], 0xf);
	}
	/*
	 * The sleep reset every AP pin with the gpio domain, and the pin controller's driver
	 * puts back only the ones it configured: all of them back now, before any device
	 * resumes (storage's link doesn't come up without them).
	 */
	s->pins_lost = s->pins_saved_valid;
	apple_sleep_restore_pins();
	/* W7: what woke the SoC, for the report */
	for (i = 0; i < T8103_WAKE_WORDS; i++)
		s->wake_status[i] = readl(s->wake + T8103_WAKE_STATUS + 4 * i);
	for (i = 0; i < T8103_WAKE_WORDS; i++)
		writel(t->wake_mask[i], s->wake + T8103_WAKE_MASK + 4 * i);
	if (t->wake_slp_ddr_changed) {
		writel(t->wake_slp_ddr, s->wake + T8103_WAKE_SLP_DDR);
		t->wake_slp_ddr_changed = false;
	}

	/* Device resume can take a while: the watchdog until .end puts apple_wdt's back. */
	apple_sleep_set_wd2(s, T8103_WDT_RESUME_SECS);
}

/* At .wake and again at .end: the attempt's results, once */
static void t8103_sleep_finish(struct apple_sleep *s)
{
	struct t8103_sleep *t = &t8103_sleep;

	if (!t->attempt)
		return;
	t->attempt = false;
	if (!t->committed)
		return;
	dev_info(s->dev, "T8103: commit: wake status at entry %08x %08x %08x %08x %08x, mux timeouts %u, cluster timeouts %u, SEP wait %s\n",
		 t->wake_entry[0], t->wake_entry[1], t->wake_entry[2], t->wake_entry[3],
		 t->wake_entry[4], t->mux_timeouts, t->cluster_timeouts,
		 t->sep_pg_cleared ? "cleared" : "kept");
	dev_info(s->dev, "T8103: SEP power state %#x at the commit (gate wait %d)\n", t->sep_ps,
		 t->sep_ps_gated);
	dev_dbg(s->dev, "T8103: stopped PMP %#x SIO %#x DCP %#x DCPEXT %#x, memory cache %#x, quiesce timeouts %u, IPIs pending %#x\n",
		t->pmp_result, t->sio_result, t->dcp_result, t->dcpext_result, t->slc_result,
		t->quiesce_timeouts, t->aic_result);
	if (t->dcs_perf && (!t->dcs_perf_down_ok || !t->dcs_perf_up_ok))
		dev_warn(s->dev, "T8103: DRAM perf state %#x, down to %#x (%s), %#x after the wake, back to %#x (%s)\n",
			 t->dcs_perf_saved, t->dcs_perf_down_val,
			 t->dcs_perf_down_ok ? "ok" : "stuck", t->dcs_perf_wake_val,
			 t->dcs_perf_up_val, t->dcs_perf_up_ok ? "ok" : "stuck");
	if (t->mux_fixed)
		dev_info(s->dev, "T8103: clock muxes put back after the wake: %#llx\n",
			 t->mux_fixed);
	if (t->gfx_wake_err)
		dev_err(s->dev, "T8103: GFX power restore failed after wake: %d\n",
			t->gfx_wake_err);
	dev_info(s->dev, "T8103: wake status %08x %08x %08x %08x %08x, clusters %d %d, mux timeouts %u\n",
		 s->wake_status[0], s->wake_status[1], s->wake_status[2], s->wake_status[3],
		 s->wake_status[4], t->cluster_wake_ret[0], t->cluster_wake_ret[1],
		 t->mux_wake_timeouts);
}

static const struct apple_sleep_wake_name t8103_wake_names[] = {
	{ 12, "AOP.GPIO.IRQ4" }, { 14, "AOP.GPIO.IRQ6" },
	{ 54, "NUB.SPMI0Sw3IRQ" },
	{ 76, "SMC.CPUWakeupAP" }, { 81, "SMC.OutboxNotEmpty" },
	{ 133, "ATC0.USBWakeup" }, { 134, "ATC1.USBWakeup" },
};

static const struct apple_sleep_ops t8103_sleep_ops = {
	.deep_wfi_arm = t8103_deep_wfi_arm,
	.resume = t8103_resume_stub,
	.aic_mask_all = t8103_aic_mask_all,
	.aic_ack = aic_sleep_ack,
	.aic_restore = t8103_aic_restore,
	.map_regs = t8103_sleep_map_regs,
	.begin = t8103_sleep_begin,
	.pre_commit = t8103_pre_commit,
	.post_timer = t8103_post_timer,
	.check = t8103_sleep_check,
	.display_off = t8103_sleep_display_off,
	.prepare_regs = t8103_sleep_prepare_regs,
	.undo_regs = t8103_sleep_undo_regs,
	.commit_regs = t8103_sleep_commit_regs,
	.wake = t8103_sleep_wake,
	.finish = t8103_sleep_finish,
	.cpu_prepare = t8103_sleep_cpu_prepare,
	.cpu_restore = t8103_sleep_cpu_restore,
};

static const struct apple_sleep_soc t8103_sleep_soc = {
	.ops = &t8103_sleep_ops,
	.early_context = true,
	.early_cpu_wfi = true,
	.boot_wfi_delay_ms = 3,
	/* The wake mask is the AOP's global block, armed and read by the SoC's operations. */
	.own_wake = true,
	.wdt_wd1 = true,
	.smc_gen1_keys = true,
	.boot_mpidr = 0x0,
	.cpu_count = 8,
	/* MPIDR affinity 2 is the cluster: E 0.0.n, P 1.1.n; start bits 4 * cluster + core */
	.cpu_cluster_affinity = 0,
	.cpu_clusters = 2,
	.cpu_cores_per_cluster = 4,
	.cpu_start_stride = 4,
	.cpu_valid = t8103_sleep_cpu_valid,
	.cluster_cpu = { T8103_CLUSTER_E, T8103_CLUSTER_P },
	.ncluster_cpu = 2,
	.ndisp = 1,
	.pmu_usid = T8103_PMU_USID,
	.wake_mask = T8103_WAKE_MASK,
	.wake_status = T8103_WAKE_STATUS,
	.wake_words = T8103_WAKE_WORDS,
	.wake_last_word = 0x03ffffff,
	.wake_names = t8103_wake_names,
	.nwake_names = ARRAY_SIZE(t8103_wake_names),
};
