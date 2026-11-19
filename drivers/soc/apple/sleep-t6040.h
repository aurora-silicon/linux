/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/*
 * T6040 (M4 Pro) suspend to RAM. Included by sleep.c after the T6050 part, as that one is.
 * See the profile at the end for what it matches and when it refuses.
 */
#include <linux/mfd/macsmc.h>
#include <linux/mfd/syscon.h>
#include <linux/regmap.h>

#define PS_ACTIVE		0xf
#define PS_MIN			GENMASK(19, 16)
#define APCIE_PORT_STATUS	0x804
#define APCIE_PORT_STATUS_READY	BIT(0)

#define PS_STRIDE		8
#define PS_MAX_PER_GROUP	16
#define PS_SIO_FIRST		0x150	/* SOC-East: I2C0, the first SIO peripheral word */
/* The extent of the two power word pages the driver accesses (the DT window sizes). */
#define SOC_EAST_SIZE		0xd000
#define SOC_PS_SIZE		0x9000
#define PS_SIO_LAST		0x228	/* SOC-East: UART6, the last */

/*
 * ANS's storage PCIe block: the two ST lanes (apcie_st0/apcie_st1) and their
 * shared PHY (T8140 has a single lane). Taken down at the commit
 * and raised on wake. The power words sit in SOC-East (t604x-pmgr.dtsi
 * offsets, same bank as ps_ans); the control windows (axi2af/cio3pll/clkgen/
 * phy-block/phy-ip/rc) and the PLL/lane tunables are not in this SoC's static
 * description and are published by the boot object (aura) under the apple,apcie
 * node, as on the T8140. The block-internal offsets are the T8140's
 * (sleep-internal.h): the same Apple PCIe IP.
 */
#define PS_ANS			0x370	/* SOC-East: ps_ans */
#define PS_APCIE_ST0		0x108	/* SOC-East: ps_apcie_st0 */
#define PS_APCIE_ST1		0x110	/* SOC-East: ps_apcie_st1 */
#define PS_APCIE_SYS_ST0	0x380	/* SOC-East: ps_apcie_sys_st0 */
#define PS_APCIE_SYS_ST1	0x388	/* SOC-East: ps_apcie_sys_st1 */
#define PS_APCIE_PHY_SW		0x398	/* SOC-East: ps_apcie_phy_sw (the shared PHY) */
#define PS_FAB2_SOC		0x338	/* SOC: the ATC commons' parent */
/* The clock-gated words (TARGET 4) the commit writes, as the sleep engine asks for them. */
#define PS_APCIE_PHY_SW_CLKGATED	0x0f000244
#define PS_APCIE_GP_CLKGATED	0x00000244

/*
 * cluster-ctl (ADT pmgr reg[0] + cluster-ctl-offset 0x20000 = 0x502820000), one word per
 * complex, measured on the J616s: the CPU complexes E +0x0, P0 +0x4 and P1 +0x8, then the
 * internal and the external display's complexes +0xc and +0x10. The low nibble 0 powers a
 * complex down and 0xf powers it up; BIT4 reads set until a power-down has finished.
 */
#define CLUSTER_CTL(n)		((n) * 4)
#define CLUSTER_CTL_NR		5	/* E, P0, P1, internal and external display */
#define CLUSTER_CTL_FIRST_WAIT	3	/* the two display complexes are polled */
#define CLUSTER_CTL_BUSY	BIT(4)
#define CLUSTER_CTL_NIBBLE	GENMASK(3, 0)
#define CLUSTER_CTL_ON		0xf
#define T6040_CLUSTER_TIMEOUT_US	50000

/*
 * SOC-PwrGate (ADT pmgr reg[0x26] = 0x50282c000): the retention bits at +0x0/+0x40 and
 * the three CPM voters at +0x100/+0x108/+0x110, each BIT31. The wake restore re-asserts
 * them. The entry leaves them alone: the voters already read 0x80000000 there.
 */
#define PWRGATE_ON		0x80000000
#define PWRGATE_WAKE_0		0x000
#define PWRGATE_WAKE_1		0x040
#define PWRGATE_CPM_0		0x100
#define PWRGATE_CPM_1		0x108
#define PWRGATE_CPM_2		0x110

/*
 * SOC-East (ADT pmgr reg[2] = 0x502280000). The SEP/retention word at +0xc00: the wake
 * clears PS_DEV_DISABLE and then polls PS_BUSY clear before going on. Also holds several
 * of the axi2af domains' power words.
 */
#define SOC_EAST_RETENTION	0xc00
#define SOC_EAST_TIMEOUT_US	5000

/*
 * The clock quiesce, whole and nothing else: two SOC-East words lose BIT28 and are
 * polled until [7:4] reads their [3:0]; then the four clk-mux park words of ClkGen SOC (ADT
 * pmgr reg[3] = 0x500000000, +0x38050/054/060/064), in t6040_clkgen_mux[] order, get [27:24]
 * := 0 with BIT31 set, each polled until BIT30 clears. The wake writes the saved clk-mux
 * words back; there is nothing else to restore. Not measured on the J616s.
 */
#define SOC_EAST_CLK_A		0x120
#define SOC_EAST_CLK_B		0x130
#define SOC_EAST_CLK_AUTO	BIT(28)
#define CLKGEN_MUX_SET		0x80000000
#define CLKGEN_MUX_MASK		0xf0ffffff
#define CLKGEN_MUX_WORDS	4
#define CLKGEN_MUX_PENDING	BIT(30)
#define CLK_QUIESCE_TIMEOUT_US	1000

/*
 * The keep-alive enables: 32 words in PMGR SOC from +0x1dc000 and 32 in SOC-East from
 * +0x50000, stride 0x44, BIT0 the keep-alive. Cleared right before the S2R timer, set
 * again at the wake.
 */
#define T6040_KA_SOC_PA		0x5029dc000ULL
#define T6040_KA_EAST_PA	0x5022d0000ULL
#define KA_WORDS		32
#define KA_STRIDE		0x44
#define KA_SIZE			(KA_WORDS * KA_STRIDE)
#define KA_ENABLE		BIT(0)

/*
 * The SMC's AP watchdog deadline (the watchdog timer's reg[2] + 8): written 0 right after
 * the AON WD2 is armed for the sleep.
 */
#define T6040_SMC_WDT_DEADLINE_PA	0x508330008ULL

/*
 * The AGX residency gate at the top of the PMGR quiesce: unless
 * MiniPMGR +0x75010 has both [19:18] set (it read 0 on the J616s), the GPU's AFR power
 * word (pmgr_gfx 0x288e80000 + 0x100, ADT power-device AFR) is lowered when its
 * target is not 0: [19:16] := 4 and polled, [7:4] polled to 4, then the word := 0
 * and [7:4] polled to 0.
 */
#define T6040_AGX_GATE_PA	0x508375010ULL
#define AGX_GATE_RESIDENT	GENMASK(19, 18)
#define T6040_AFR_PS_PA		0x288e80100ULL
#define AGX_TIMEOUT_US		10000

/*
 * Before the SoC-specific steps of the PMGR quiesce, in order:
 *  - the ACC quiesce of c = E, P0, P1: APSC off, ACC +0xe20020 BIT23 := 1 (64-bit),
 *    poll BIT7 clear (CPM gating after it reads back already set: not written);
 *  - features off: cpu-fixed-freq-pll-relock, BIT42 := 0 in the E and P0
 *    ACC words; then the TVMs disp2, afr, dcs (two), gfx (two), disp, soc: control
 *    BIT0 := 0, status polled until BIT1 clears.
 * The PMC's DCS floor vote is not touched: an access to that word hangs the bus. Every poll
 * here is bounded.
 */
static const u64 t6040_acc_apsc_pa[] = { 0x210e20020ULL, 0x211e20020ULL, 0x212e20020ULL };
#define ACC_APSC_DISABLE	BIT_ULL(23)
#define ACC_APSC_BUSY		BIT_ULL(7)
#define ACC_PLL_RELOCK		BIT_ULL(42)
#define ACC_PLL_RELOCK_COMPLEXES	2
#define QUIESCE_TIMEOUT_US	2000

static const struct {
	u64 ctl;
	u64 status;
} t6040_tvm[] = {
	{ 0x502920000ULL, 0x502920250ULL },	/* disp2-tvm */
	{ 0x288e95000ULL, 0x288e953e0ULL },	/* afr-tvm */
	{ 0x502908000ULL, 0x502908250ULL },	/* dcs-tvm */
	{ 0x502910000ULL, 0x502910250ULL },
	{ 0x288e94000ULL, 0x288e943e0ULL },	/* gfx-tvm */
	{ 0x288e94400ULL, 0x288e947e0ULL },
	{ 0x502918000ULL, 0x502918250ULL },	/* disp-tvm */
	{ 0x502900000ULL, 0x502900250ULL },	/* soc-tvm */
};

#define TVM_ENABLE		BIT(0)
#define TVM_BUSY		BIT(1)

/*
 * The PMC's bandwidth-request and boost interface enables of the display pipes
 * (0x5048ac020 and +0x8), one bit per pipe.
 */
#define T6040_PMC_BWR_PA	0x5048ac020ULL
#define PMC_BOOST		0x8

/*
 * The PMU over SPMI (spmi = 0x509014000, spmi-a0 = 0x509188000), written as on the T8140.
 * Measured on the J616s: the boot stage is set to 0x50 by an extended write to
 * PMU_BOOT_STAGE, and NOTIFY_ENABLE is 0x2000001 on both controllers' +0x40.
 */
/* The PMU's USID on the sleep's SPMI bus, and the boot stage that announces a sleep */
#define PMU_USID		0xe
#define PMU_BOOT_STAGE		0xf801		/* T6040 slave/reg */
#define PMU_BOOT_STAGE_SLEEP	0x50
/* The response vector's enable for the sleep */
#define SPMI_NOTIFY_ENABLE_SLEEP	0x2000001

/*
 * MasterMini, the block of the always-on PMGR (ADT pmgr reg[0x37] = 0x508900000).
 * The S2R timer at +0x4000 (physical 0x508904000) is the commit: writing 1 arms
 * the SoC sleep engine. Measured on the J616s: exactly this write.
 */
#define MASTERMINI_S2R_TIMER	0x4000

/*
 * AOP-global (the AP controls, window 0x508000000). Mapped from the
 * S2R-control word 0x508004000: +0x0 the S2R control (cleared to 0 on wake),
 * +0xc..+0x2c the 9 wake-source masks (armed before
 * sleep), +0x30..+0x50 the 9 wake-source status words (read on wake).
 */
#define AOP_S2R_CTRL		0x00
#define AOP_WAKE_MASK(w)	(0x0c + (w) * 4)
#define AOP_WAKE_STATUS(w)	(0x30 + (w) * 4)
#define AOP_WAKE_WORDS		9

/* axi2af fabric-bridge retention re-arm. */
#define AXI2AF_BRIDGE_104	0x104
#define AXI2AF_BRIDGE_108	0x108

/* The internal display: its PMGR words and its DART. */
#define T6040_DCP_DART_PA	0x402340000ULL
#define T6040_DISP_PWRGATE_PA	0x5022a1000ULL
#define DISP_PWRGATE_A		0x1c	/* := 3 before DISP_SYS goes down, 0 after it is up */
#define DISP_PWRGATE_B		0x60	/* := 0 before DISP_SYS goes down, 1 after it is up */
#define PS_DISP_FE		0x368	/* soc_east */
#define PS_DISP_SYS		0x258	/* soc_east */
#define DISP_PS_TIMEOUT_US	8000
#define DISP_CPU_GATE_US	50000

/* The GP PCIe (t602x port layout, as T8140's radio port): power words in soc_east. */
#define PS_APCIE_GP		0x100
#define PS_APCIE_SYS_GP		0x390
#define GP_PIN_DATA		BIT(0)
/* The PMC's DCP voters: the count of DCPs powered (+0) and a second voter (+4), both 0 at sleep */
#define T6040_PMC_DCP_PA	0x5048a9c14ULL

/* The axi2af fabric-bridge retention domains, t6040_axi2af[] */
enum {
	AXI2AF_ATC0_COMMON,
	AXI2AF_ATC1_COMMON,
	AXI2AF_ATC2_COMMON,
	AXI2AF_ATC3_COMMON,
	AXI2AF_DISP_SYS,
	AXI2AF_DISPEXT0_SYS,
	AXI2AF_DISPEXT1_SYS,
	AXI2AF_DISPEXT2_SYS,
	AXI2AF_DISPEXT3_SYS,
	AXI2AF_ANS,
	AXI2AF_AVD_SYS,
	AXI2AF_VENC0_SYS,
	AXI2AF_VENC1_SYS,
	AXI2AF_JPG,
	AXI2AF_APCIE_SYS_GP,
	T6040_AXI2AF_DOMAINS
};

/*
 * The AON watchdog's WD2, the sleep safety net: ADT wdt reg[0] = 0x50836c000, with the
 * T8140's registers. Linux's own watchdog driver runs WD1 (+0x10..+0x1c), not this one.
 * ALARM is 32 bits at 24 MHz.
 */
#define WDT_MAX_SECS		178U
#define T6040_WDT_PA		0x50836c000ULL	/* the address t6040_resume_stub writes */

struct t6040_sleep_group {
	const char *name;
	struct regmap *regmap;
	u32 first;	/* the pipe's CPU */
	u32 last;	/* its last sub-block */
};

/* A device the commit point looks at: bound or not, and its own power word. */
struct t6040_sleep_watch {
	const char *compatible;
	struct regmap *ps_regmap;
	u32 ps_offset;
	bool has_ps;
	bool bound;
};

enum {
	T6040_WATCH_SEP,
	T6040_WATCH_ANS,
	T6040_WATCH_COUNT,
};

/*
 * ANS's storage-apcie control windows and PLL/lane tunables, mapped from the
 * apple,apcie node the boot object publishes (t6040_sleep_map_apcie). Every
 * pointer is NULL until a loader fills the node: @valid is false then, and a
 * real sleep with ANS stopped refuses. The two ST lanes share this one PHY.
 */
struct t6040_apcie_st {
	void __iomem *axi;		/* reg-name "axi2af" (shared) */
	void __iomem *cio3pll;		/* reg-name "cio3pll" (shared) */
	void __iomem *clkgen;		/* reg-name "clkgen"  (shared) */
	void __iomem *phy;		/* reg-name "phy-block" (shared PHY) */
	void __iomem *phy_ip;		/* reg-name "phy-ip" */
	void __iomem *rc;		/* reg-name "rc" */
	struct apple_tunable *axi_tunable;	/* apple,tunable-axi2af */
	struct apple_tunable *cio3pll_tunable;	/* apple,tunable-cio3pll */
	struct apple_tunable *clkgen_tunable;	/* apple,tunable-clkgen */
	struct apple_tunable *phy_tunable;	/* apple,tunable-phy */
	struct apple_tunable *phy_ip_pll;	/* apple,tunable-phy-ip-pll */
	struct apple_tunable *phy_ip_lanes;	/* apple,tunable-phy-ip-auspma */
	bool valid;
};

struct t6040_sleep {
	struct device *dev;
	struct apple_sleep *core;
	struct t6040_sleep_group *groups;
	unsigned int ngroups;
	/* The dry run level of the suspend in progress, from .begin to .end; 0 otherwise. */
	unsigned int dry;
	void __iomem *cpu_start;
	void __iomem *wdt;
	/* The DT's wdt block is the one t6040_resume_stub hardcodes. */
	bool wdt_stub_ok;
	/* The commit's and the wake's SoC blocks (see the #defines). Optional. */
	void __iomem *cluster_ctl;
	void __iomem *pwrgate;
	void __iomem *dcp_dart;		/* dart-dcp, for its transaction idle wait */
	void __iomem *disp_pwrgate;	/* internal display retention window 0x5022a1000 */
	bool disp_down;			/* t6040_disp_off() lowered the internal display */
	bool disp_by_driver;		/* the display driver had lowered DISP_SYS itself */
	void __iomem *pmc_dcp;		/* PMS pmc-dcp voter pair 0x5048a9c14/c18 */
	u32 pmc_dcp_saved[2];
	u32 pmc_if_saved[2];		/* BWR and boost interface enables at the commit */
	bool pmc_if_valid;
	/* The GP PCIe host's two ports (Wi-Fi/BT, SD) for the commit's port stop (T8140's). */
	void __iomem *gp_port[2];
	void __iomem *gp_phy[2];
	void __iomem *gp_pins;
	int gp_perst[2];
	bool gp_perst_low[2];
	void __iomem *clkgen;
	void __iomem *spmi0;
	void __iomem *spmi_a0;
	void __iomem *mastermini;
	void __iomem *dcs[16];	/* DCS(n,k): 0x220200000 + n * 0x2000000 + k * 0x200000 */
	void __iomem *atc_cfg[4];	/* ATCPHY_CFG(n) 0x383200000 + n * 0x8000000 */
	void __iomem *aop_ps;	/* MiniPMGR AOP + audio island PS words 0x508304000..0x158 */
	u32 sio_ps_saved[(PS_SIO_LAST - PS_SIO_FIRST) / PS_STRIDE + 1];
	bool sio_ps_saved_valid;
	/* The SOC-East and SOC power word pages: fabrics, ATC commons, DCS, SIO. */
	void __iomem *soc_east;		/* 0x502280000 */
	void __iomem *soc_ps;		/* 0x502800000 */
	void __iomem *aop_global;
	void __iomem *axi2af_bridge[T6040_AXI2AF_DOMAINS];
	struct aura_s2r_mailbox *mbox;
	u64 mbox_pa;
	bool power_ops;
	bool wd2_armed;
	/* Set by .begin for a suspend to RAM; .end only undoes what .begin did. */
	bool mem_begun;
	u64 siq_cfg;
	u64 cntkctl;
	/* Saved across the commit, restored on wake. */
	u32 clkgen_mux_saved[CLKGEN_MUX_WORDS];
	bool clkgen_saved;
	void __iomem *ka_soc;
	void __iomem *ka_east;
	u64 ka_on_before;		/* keep-alive bits set before the commit cleared them */
	void __iomem *smc_wdt_deadline;
	void __iomem *agx_gate;
	void __iomem *s2r_lo_count;
	void __iomem *acc_apsc[3];
	void __iomem *tvm_ctl[8];
	void __iomem *tvm_status[8];
	void __iomem *pmc_bwr;
	void __iomem *afr_ps;
	u32 cluster_saved[CLUSTER_CTL_NR];
	u32 wake_mask_saved[AOP_WAKE_WORDS];
	bool wake_mask_saved_valid;
	u32 wake_status[AOP_WAKE_WORDS];
	bool returned;
	/* ANS's storage ST lanes: windows + whether a commit stopped ANS. */
	struct t6040_apcie_st apcie_st;
	int pcie_ans_on_ret;
	u32 apcie_wake_step;
	u32 gp_phy_saved[2], gp_appclk_saved[2], gp_perst_saved[2];
	bool gp_saved[2];
	unsigned int gp_clk_timeouts;
	unsigned int aop_ps_timeouts;
	u32 aop_ps_saved[44];
	bool aop_ps_saved_valid;
	struct t6040_sleep_watch watch[T6040_WATCH_COUNT];
};

static struct t6040_sleep *t6040_sleep;

static void t6040_iounmap(void *p)
{
	iounmap((void __iomem __force *)p);
}

/*
 * Every window here non-posted (nGnRnE), as the PMGR, PMC and coprocessor blocks
 * require. No region request: these overlap other drivers.
 */
static void __iomem *t6040_ioremap_np(struct device *dev, phys_addr_t pa, size_t size)
{
	void __iomem *p = ioremap_np(pa, size);

	if (!p || devm_add_action_or_reset(dev, t6040_iounmap, (void __force *)p))
		return NULL;
	return p;
}

/*
 * In the power-state check, a sub-block is on while its TARGET is not 0. A
 * pipe's CPU word never counts: it stays under automatic power gating (taking it out
 * of automatic mode raises an SError) and the SoC power-down drops it.
 */
static bool t6040_sleep_ps_on(u32 val, bool cpu)
{
	return !cpu && FIELD_GET(PS_TARGET, val);
}

/*
 * CPU parking power ops. MPIDR Aff1 is the cluster (E=0, P0=1,
 * P1=2), Aff0 the core. The start and stop bit is the core's bit in the ADT
 * function-enable_core masks, which is its cpu-id: the E cluster has four
 * slots (bits 0-3), each P cluster six (P0 bits 4-9, P1 bits 10-15; one slot
 * of each is harvested on J616s). Four-core banks (4 * cluster + core) would
 * give P1 core 0 the bit of P0 core 4.
 */
static const u8 t6040_cluster_first_bit[] = { 0, 4, 10 };
static const u8 t6040_cluster_slots[] = { 4, 6, 6 };

static unsigned int t6040_cpu_cluster(u64 mpidr)
{
	return MPIDR_AFFINITY_LEVEL(mpidr, 1);
}

static unsigned int t6040_cpu_core(u64 mpidr)
{
	return MPIDR_AFFINITY_LEVEL(mpidr, 0);
}

static u32 t6040_cpu_start_bit(u64 mpidr)
{
	unsigned int cluster = t6040_cpu_cluster(mpidr), core = t6040_cpu_core(mpidr);

	if (WARN_ON_ONCE(cluster >= ARRAY_SIZE(t6040_cluster_first_bit) ||
			 core >= t6040_cluster_slots[cluster]))
		return 0;
	return BIT(t6040_cluster_first_bit[cluster] + core);
}

/*
 * Each core's CoreSight external debug block (ADT cpuN coresight-reg: 0x210010000 +
 * cluster * 0x1000000 + core * 0x100000). Every CPU, boot and secondary,
 * unlocks it (EDLAR) and writes EDPRCR := 0 right before its deep WFI, so that
 * the debug logic does not prevent power gating. With
 * EDPRCR.CORENPDRQ left set, a core only emulates its power-down.
 */
#define T6040_ED_BASE_PA	0x210010000ULL
#define T6040_ED_CLUSTER_STRIDE	0x1000000ULL
#define T6040_ED_CORE_STRIDE	0x100000ULL
#define ED_EDPRCR		0x310
#define ED_EDLAR		0xfb0
#define ED_LOCK_ACCESS_KEY	0xc5acce55
#define T6040_ED_CLUSTERS	ARRAY_SIZE(t6040_cluster_slots)
#define T6040_ED_CORES		6	/* the most slots of a cluster */

static void __iomem *t6040_ed[T6040_ED_CLUSTERS][T6040_ED_CORES];

static void t6040_ed_map(struct device *dev)
{
	unsigned int cpu, n = 0;

	for_each_possible_cpu(cpu) {
		u64 mpidr = cpu_logical_map(cpu);
		unsigned int c = t6040_cpu_cluster(mpidr), k = t6040_cpu_core(mpidr);

		if (c >= T6040_ED_CLUSTERS || k >= T6040_ED_CORES || t6040_ed[c][k])
			continue;
		t6040_ed[c][k] = t6040_ioremap_np(dev, T6040_ED_BASE_PA +
						 c * T6040_ED_CLUSTER_STRIDE +
						 k * T6040_ED_CORE_STRIDE, 0x1000);
		n += !!t6040_ed[c][k];
	}
	dev_dbg(dev, "external debug windows mapped for %u cores\n", n);
}

static void t6040_ed_release(void)
{
	u64 mpidr = read_cpuid_mpidr();
	unsigned int c = t6040_cpu_cluster(mpidr), k = t6040_cpu_core(mpidr);
	void __iomem *ed;

	if (c >= T6040_ED_CLUSTERS || k >= T6040_ED_CORES)
		return;
	ed = t6040_ed[c][k];
	if (!ed)
		return;
	writel_relaxed(ED_LOCK_ACCESS_KEY, ed + ED_EDLAR);
	writel_relaxed(0, ed + ED_EDPRCR);
}

static void t6040_deep_wfi_arm(void)
{
	/* A timer left running ends the WFI and costs power until reset. */
	write_sysreg(ARCH_TIMER_CTRL_IT_MASK, cntp_ctl_el0);
	write_sysreg(ARCH_TIMER_CTRL_IT_MASK, cntv_ctl_el0);
	t6040_ed_release();
	sysreg_clear_set_s(SYS_APL_DEEP_SLEEP, 0, DEEP_SLEEP_ENABLE);
}

static void t6040_sleep_set_wd2(struct t6040_sleep *s, unsigned int secs)
{
	writel(0, s->wdt + WDT_WD2_CTL);
	writel(0, s->wdt + WDT_WD2_COUNT);
	writel(secs * WDT_TICKS_PER_S, s->wdt + WDT_WD2_ALARM);
	writel(1, s->wdt + WDT_WD2_CTL);
	s->wd2_armed = true;
}

/*
 * The AOP and audio island's power words (MiniPMGR 0x508304000 + 0x000..0x158). The island
 * is off after a sleep, and the AOP's resume, which reads its ASC first, faults on it. The
 * words are read in .begin and put back at the wake, parents first: a word that was on goes
 * to a manual target 0xf (in automatic mode it gates again before the AOP's driver gets to
 * its ASC), any other gets its saved value if that asks for power. The table
 * is the power-down order, children first.
 */
#define T6040_AOP_PS_PA		0x508304000ULL
static const u16 t6040_aop_ps_order[] = {
	0x0b0, 0x128, 0x120, 0x158, 0x150, 0x148, 0x140, 0x138, 0x130, 0x118, 0x0c0, 0x0c8,
	0x0e8, 0x0f0, 0x0f8, 0x100, 0x108, 0x110, 0x118, 0x0e0, 0x0d8, 0x0d0, 0x0b8, 0x090,
	0x020, 0x098, 0x0a0, 0x0a8, 0x028, 0x030, 0x068, 0x070, 0x078, 0x080, 0x038, 0x040,
	0x048, 0x058, 0x060, 0x088, 0x050, 0x010, 0x018, 0x008, 0x000,
};

#define T6040_AOP_PS_WORDS	44

static void t6040_aop_ps_save(struct t6040_sleep *s)
{
	int i;

	s->aop_ps_saved_valid = false;
	if (!s->aop_ps)
		return;
	for (i = 0; i < T6040_AOP_PS_WORDS; i++)
		s->aop_ps_saved[i] = readl(s->aop_ps + PS_STRIDE * i);
	s->aop_ps_saved_valid = true;
}

static void t6040_aop_ps_restore(struct t6040_sleep *s)
{
	int k;

	s->aop_ps_timeouts = 0;
	if (!s->aop_ps || !s->aop_ps_saved_valid)
		return;
	for (k = ARRAY_SIZE(t6040_aop_ps_order) - 1; k >= 0; k--) {
		u16 off = t6040_aop_ps_order[k];
		void __iomem *r = s->aop_ps + off;
		u32 v, want = s->aop_ps_saved[off / 8], now = readl(r);

		if (FIELD_GET(PS_ACTUAL, want)) {
			if (FIELD_GET(PS_ACTUAL, now) == PS_ACTIVE && !(now & PS_AUTO_ENABLE))
				continue;
			writel((now & ~PS_AUTO_ENABLE) | PS_TARGET, r);
		} else {
			if (FIELD_GET(PS_ACTUAL, now) || !FIELD_GET(PS_TARGET, want))
				continue;
			writel(want, r);
		}
		if (readl_poll_timeout_atomic(r, v, FIELD_GET(PS_ACTUAL, v), 1, 2000))
			s->aop_ps_timeouts++;
	}
}

/*
 * The power loss resets every power word, and only the domains a Linux driver owns are
 * raised again when their devices resume. Here the SIO peripheral words (I2C0 .. UART6 in
 * SOC-East) that were fully on at the commit
 * and came back lower are raised again, parents first. On J616s those are what the boot
 * loader left on and no driver claims: I2C0, SPI0/1/3/4/5 (SPI4 goes to the internal panel),
 * the UARTs, AES. Nothing else: every other word is its owner's to raise.
 */
static void t6040_sio_ps_save(struct t6040_sleep *s)
{
	unsigned int i;

	s->sio_ps_saved_valid = false;
	for (i = 0; i < ARRAY_SIZE(s->sio_ps_saved); i++)
		s->sio_ps_saved[i] = readl(s->soc_east + PS_SIO_FIRST + i * PS_STRIDE);
	s->sio_ps_saved_valid = true;
}

static void t6040_sio_ps_restore(struct t6040_sleep *s)
{
	unsigned int i, n, raised = 0, failed = 0;
	u32 val, cur = 0;

	if (!s->sio_ps_saved_valid)
		return;
	s->sio_ps_saved_valid = false;
	for (i = 0; i < ARRAY_SIZE(s->sio_ps_saved); i++) {
		u32 off = PS_SIO_FIRST + i * PS_STRIDE;
		void __iomem *w = s->soc_east + off;
		u32 saved = s->sio_ps_saved[i];

		if (FIELD_GET(PS_TARGET, saved) != PS_ACTIVE ||
		    FIELD_GET(PS_ACTUAL, saved) != PS_ACTIVE)
			continue;
		val = readl(w);
		if (FIELD_GET(PS_TARGET, val) == PS_ACTIVE)
			continue;
		writel(val | PS_TARGET, w);
		/* Timekeeping is still suspended here: count the wait, no ktime. */
		for (n = 0; n < 1000; n++) {
			cur = readl(w);
			if (FIELD_GET(PS_ACTUAL, cur) == PS_ACTIVE)
				break;
			udelay(10);
		}
		if (n == 1000)
			failed++;
		raised++;
		dev_dbg(s->dev, "wake: SOC-East power word +%#x: %#x at the commit, %#x after the wake, now %#x\n",
			off, saved, val, cur);
	}
	if (failed)
		dev_warn(s->dev, "wake: %u of %u SIO power word(s) raised again did not come on\n",
			 failed, raised);
}

/*
 * The 4 clk-mux park words (ClkGen SOC + these offsets), from
 * the clock quiesce; the offsets skip 0x58/0x5c.
 */
static const u32 t6040_clkgen_mux[CLKGEN_MUX_WORDS] = {
	0x38060, 0x38064, 0x38054, 0x38050,
};

/*
 * The 9 AOP-global wake-source masks armed before an S2R
 * (the values are decoded from the ADT's ap-wake-sources).
 * valid & ~enabled per word.
 */
static const u32 t6040_wake_mask[AOP_WAKE_WORDS] = {
	0xe5ff5fff, 0xff42edff, 0xffffffff, 0x3ffffbdf, 0xf7ffffc0,
	0xffffff7f, 0xffffffff, 0xffffffff, 0x00003fff,
};

/*
 * The hardware S2R-lo entry counter: bits 31:3 of MiniPMGR + 0x70970. It tells "the SoC
 * entered S2R" from "the engine never finished". Read-only, with the AOP-global S2R
 * control, wake masks and wake status, at debug level: at probe, for the boot after a
 * failed sleep, and in a dry run. Mapped once at probe: the dry run reads it with
 * interrupts off.
 */
#define T6040_S2R_LO_COUNT_PA	0x508370970ULL

/*
 * The 15 domains with an axi2af fabric bridge to re-arm. Each domain's power word is in
 * SOC (soc_ps, base 0x502800000) or SOC-East (soc_east, base 0x502280000); its
 * fabric bridge is a separate window. On wake every domain that is powered up
 * (PS TARGET == 0xf) gets bridge +0x104/+0x108 bit0 set, or the bridge drops
 * its traffic.
 */
static const struct t6040_axi2af_domain {
	const char *name;
	bool east;		/* power word in soc_east (true) or soc_ps (false) */
	u32 ps_off;
	phys_addr_t bridge_pa;
} t6040_axi2af[] = {
	[AXI2AF_ATC0_COMMON]  = { "atc0_common",  false, 0x390, 0x380000000 },
	[AXI2AF_ATC1_COMMON]  = { "atc1_common",  false, 0x3b8, 0x388000000 },
	[AXI2AF_ATC2_COMMON]  = { "atc2_common",  false, 0x3e0, 0x390000000 },
	[AXI2AF_ATC3_COMMON]  = { "atc3_common",  false, 0x408, 0x398000000 },
	[AXI2AF_DISP_SYS]     = { "disp_sys",     true,  0x258, 0x403000000 },
	[AXI2AF_DISPEXT0_SYS] = { "dispext0_sys", false, 0x340, 0x3a3000000 },
	[AXI2AF_DISPEXT1_SYS] = { "dispext1_sys", false, 0x490, 0x603000000 },
	[AXI2AF_DISPEXT2_SYS] = { "dispext2_sys", false, 0x4e0, 0x607000000 },
	[AXI2AF_DISPEXT3_SYS] = { "dispext3_sys", false, 0x2d8, 0x307000000 },
	[AXI2AF_ANS]          = { "ans",          true,  0x370, 0x40b000000 },
	[AXI2AF_AVD_SYS]      = { "avd_sys",      false, 0x170, 0x280000000 },
	[AXI2AF_VENC0_SYS]    = { "venc0_sys",    false, 0x278, 0x282000000 },
	[AXI2AF_VENC1_SYS]    = { "venc1_sys",    false, 0x2a0, 0x284000000 },
	[AXI2AF_JPG]          = { "jpg",          true,  0x3a0, 0x486000000 },
	[AXI2AF_APCIE_SYS_GP] = { "apcie_sys_gp", true,  0x390, 0x416000000 },
};

static void t6040_pmu_write(struct t6040_sleep *s, u16 reg, u8 val)
{
	apple_sleep_pmu_write(s->spmi0, PMU_USID, reg, val);
}

/*
 * Between the steps of the wake return: every access of a step is complete, and a fault it
 * raised is taken, before the next step starts.
 */
static void t6040_wake_sync(void)
{
	dsb(sy);
	isb();
}

/*
 * The five complexes off, measured on the J616s: each slot's low nibble to 0, in order
 * +0x0 .. +0x10, and the two display complexes (+0xc, +0x10) polled until BIT4 clears.
 * The CPU complexes (E, P0, P1) go down at the WFI.
 */
static void t6040_clusters_off(struct t6040_sleep *s)
{
	u32 val;
	int i;

	for (i = 0; i < CLUSTER_CTL_NR; i++) {
		val = readl(s->cluster_ctl + CLUSTER_CTL(i));
		s->cluster_saved[i] = val;
		writel(val & ~CLUSTER_CTL_NIBBLE, s->cluster_ctl + CLUSTER_CTL(i));
		if (i >= CLUSTER_CTL_FIRST_WAIT)
			readl_poll_timeout_atomic(s->cluster_ctl + CLUSTER_CTL(i), val,
						  !(val & CLUSTER_CTL_BUSY), 1, T6040_CLUSTER_TIMEOUT_US);
	}
}

static void t6040_east_clk_quiesce(struct t6040_sleep *s, u32 off)
{
	u32 val = readl(s->soc_east + off), cur;

	writel(val & ~SOC_EAST_CLK_AUTO, s->soc_east + off);
	readl_poll_timeout_atomic(s->soc_east + off, cur,
				  FIELD_GET(PS_ACTUAL, cur) == FIELD_GET(PS_TARGET, val), 1,
				  CLK_QUIESCE_TIMEOUT_US);
}

/* The clock quiesce, saving the clk-mux words for wake. */
static void t6040_clkgen_park(struct t6040_sleep *s)
{
	u32 cur;
	int i;

	t6040_east_clk_quiesce(s, SOC_EAST_CLK_A);
	t6040_east_clk_quiesce(s, SOC_EAST_CLK_B);
	for (i = 0; i < CLKGEN_MUX_WORDS; i++) {
		void __iomem *reg = s->clkgen + t6040_clkgen_mux[i];
		u32 val = readl(reg);

		s->clkgen_mux_saved[i] = val;
		writel((val & CLKGEN_MUX_MASK) | CLKGEN_MUX_SET, reg);
		readl_poll_timeout_atomic(reg, cur, !(cur & CLKGEN_MUX_PENDING), 1,
					  CLK_QUIESCE_TIMEOUT_US);
	}
	s->clkgen_saved = true;
}

/* The mirror on wake: the saved clk-mux words back, last parked first. */
static void t6040_clkgen_restore(struct t6040_sleep *s)
{
	int i;

	if (!s->clkgen_saved)
		return;
	for (i = CLKGEN_MUX_WORDS - 1; i >= 0; i--)
		writel(s->clkgen_mux_saved[i], s->clkgen + t6040_clkgen_mux[i]);
	s->clkgen_saved = false;
}

/*
 * Wait for every other core's power word (SOC + 8 * its cpu-start bit index,
 * ECPU_0.. PCPU1_5 in the ADT) to read ACTUAL 0, each for at most 10 ms.
 */
static void t6040_secondaries_off(struct t6040_sleep *s)
{
	u32 self = t6040_cpu_start_bit(read_cpuid_mpidr() & MPIDR_HWID_BITMASK);
	unsigned int cpu;
	u32 v;

	for_each_present_cpu(cpu) {
		u32 bit = t6040_cpu_start_bit(cpu_logical_map(cpu));

		if (!bit || bit == self)
			continue;
		readl_poll_timeout_atomic(s->soc_ps + PS_STRIDE * __ffs(bit), v,
					  !FIELD_GET(PS_ACTUAL, v), 1, 10000);
	}
}

/* The hardware steps of the generic PMGR quiesce, in order (see t6040_acc_apsc_pa). */
static void t6040_pmgr_quiesce(struct t6040_sleep *s)
{
	u64 q;
	u32 v;
	int i;

	for (i = 0; i < ARRAY_SIZE(t6040_acc_apsc_pa); i++) {
		void __iomem *acc = s->acc_apsc[i];

		if (!acc)
			continue;
		writeq(readq(acc) | ACC_APSC_DISABLE, acc);
		readq_poll_timeout_atomic(acc, q, !(q & ACC_APSC_BUSY), 1, QUIESCE_TIMEOUT_US);
	}
	for (i = 0; i < ACC_PLL_RELOCK_COMPLEXES; i++)
		if (s->acc_apsc[i])
			writeq(readq(s->acc_apsc[i]) & ~ACC_PLL_RELOCK, s->acc_apsc[i]);
	for (i = 0; i < ARRAY_SIZE(t6040_tvm); i++) {
		if (!s->tvm_ctl[i] || !s->tvm_status[i])
			continue;
		writel(readl(s->tvm_ctl[i]) & ~TVM_ENABLE, s->tvm_ctl[i]);
		readl_poll_timeout_atomic(s->tvm_status[i], v, !(v & TVM_BUSY), 1,
					  QUIESCE_TIMEOUT_US);
	}
}

/* The AGX residency gate of the PMGR quiesce (see T6040_AGX_GATE_PA). */
static void t6040_agx_gate(struct t6040_sleep *s)
{
	u32 gate, val, cur;

	if (!s->agx_gate || !s->afr_ps)
		return;
	gate = readl(s->agx_gate);
	if (!(~gate & AGX_GATE_RESIDENT))
		return;
	val = readl(s->afr_ps);
	if (!FIELD_GET(PS_TARGET, val))
		return;
	writel((val & ~PS_MIN) | FIELD_PREP(PS_MIN, 4), s->afr_ps);
	if (readl_poll_timeout_atomic(s->afr_ps, cur, FIELD_GET(PS_MIN, cur) == 4, 1,
				      AGX_TIMEOUT_US))
		return;
	if (readl_poll_timeout_atomic(s->afr_ps, cur, FIELD_GET(PS_ACTUAL, cur) == 4, 1,
				      AGX_TIMEOUT_US))
		return;
	writel(0, s->afr_ps);
	readl_poll_timeout_atomic(s->afr_ps, cur, !FIELD_GET(PS_ACTUAL, cur), 1, AGX_TIMEOUT_US);
}

/*
 * Keep-alive: BIT0 of the 64 keep-alive words, PMGR SOC then SOC-East per
 * index. Clears them all (on == false) or sets those in @mask; returns the
 * mask of the bits that were set (SOC in the low half, SOC-East in the high).
 */
static u64 t6040_keep_alive(struct t6040_sleep *s, bool on, u64 mask)
{
	u64 was = 0;
	int i;

	if (!s->ka_soc || !s->ka_east)
		return 0;
	for (i = 0; i < KA_WORDS; i++) {
		void __iomem *a = s->ka_soc + i * KA_STRIDE;
		void __iomem *b = s->ka_east + i * KA_STRIDE;
		u32 va = readl(a), vb = readl(b);

		was |= (u64)(va & KA_ENABLE) << i | (u64)(vb & KA_ENABLE) << (i + KA_WORDS);
		writel((va & ~KA_ENABLE) | (on && (mask & BIT_ULL(i))), a);
		writel((vb & ~KA_ENABLE) | (on && (mask & BIT_ULL(i + KA_WORDS))), b);
	}
	return was;
}

/* A domain's power word, from whichever window holds it. */
static void __iomem *t6040_axi2af_ps(struct t6040_sleep *s, const struct t6040_axi2af_domain *d)
{
	void __iomem *base = d->east ? s->soc_east : s->soc_ps;

	return base ? base + d->ps_off : NULL;
}


/*
 * Read-only axi2af gate: every reachable domain's power word is expected to read
 * [7:4] == 0 here. The ones still on are only named, at debug level.
 */
static void t6040_axi2af_readcheck(struct t6040_sleep *s)
{
	int i, on = 0;

	for (i = 0; i < T6040_AXI2AF_DOMAINS; i++) {
		void __iomem *ps = t6040_axi2af_ps(s, &t6040_axi2af[i]);
		u32 val;

		if (!ps)
			continue;
		val = readl(ps);
		if (FIELD_GET(PS_ACTUAL, val)) {
			dev_dbg(s->dev, "axi2af %s still on (%#x) at commit\n",
				t6040_axi2af[i].name, val);
			on++;
		}
	}
	if (on)
		dev_dbg(s->dev, "%d axi2af domain(s) still powered at commit\n", on);
}

/*
 * The wake-side axi2af re-arm: every retained domain powered back up (PS TARGET 0xf and
 * ACTUAL on) gets its fabric bridge +0x104/+0x108 bit0 set, or the bridge drops its
 * traffic. A word in automatic mode can read TARGET 0xf with its domain gated; the bridge
 * is then off too, and an access to it raises an SError, so such a domain is left alone.
 */
static void t6040_axi2af_rearm(struct t6040_sleep *s)
{
	int i;

	for (i = 0; i < T6040_AXI2AF_DOMAINS; i++) {
		void __iomem *ps = t6040_axi2af_ps(s, &t6040_axi2af[i]);
		void __iomem *bridge = s->axi2af_bridge[i];
		u32 val;

		if (!ps || !bridge)
			continue;
		val = readl(ps);
		if (FIELD_GET(PS_TARGET, val) != PS_ACTIVE)
			continue;
		if (!FIELD_GET(PS_ACTUAL, val)) {
			dev_dbg(s->dev, "wake: axi2af %s not re-armed, PS %#x (gated)\n",
				t6040_axi2af[i].name, val);
			continue;
		}
		t6040_wake_sync();
		writel(readl(bridge + AXI2AF_BRIDGE_104) | BIT(0), bridge + AXI2AF_BRIDGE_104);
		writel(readl(bridge + AXI2AF_BRIDGE_108) | BIT(0), bridge + AXI2AF_BRIDGE_108);
		t6040_wake_sync();
	}
}

/* Arm the 9 AOP-global wake masks before the commit, saving the old words. */
static void t6040_wake_arm(struct t6040_sleep *s)
{
	int i;

	for (i = 0; i < AOP_WAKE_WORDS; i++) {
		s->wake_mask_saved[i] = readl(s->aop_global + AOP_WAKE_MASK(i));
		writel(t6040_wake_mask[i], s->aop_global + AOP_WAKE_MASK(i));
	}
	s->wake_mask_saved_valid = true;
}

/* Put the wake masks back at .end of a returned (or refused) sleep. */
static void t6040_wake_restore(struct t6040_sleep *s)
{
	int i;

	if (!s->wake_mask_saved_valid)
		return;
	for (i = 0; i < AOP_WAKE_WORDS; i++)
		writel(s->wake_mask_saved[i], s->aop_global + AOP_WAKE_MASK(i));
	s->wake_mask_saved_valid = false;
}

/*
 * One power word raised, as sleep.c's sleep_ps_raise() does: target 0xf with automatic
 * mode off and the flag bits cleared, ACTUAL polled, then AUTO_ENABLE. The two halves
 * are separate for a word that has to stay at a manual target for a while or for good.
 */
static int t6040_ps_raise_start(void __iomem *base, u32 offset)
{
	void __iomem *reg = base + offset;
	u32 now;

	writel((readl(reg) & ~(PS_AUTO_ENABLE | GENMASK(11, 0))) | PS_TARGET, reg);

	return readl_poll_timeout_atomic(reg, now, (now & PS_ACTUAL) == PS_ACTUAL, 1,
					 PS_OFF_TIMEOUT_US);
}

static void t6040_ps_raise_finish(void __iomem *base, u32 offset)
{
	void __iomem *reg = base + offset;

	writel((readl(reg) & ~(PS_AUTO_ENABLE | GENMASK(11, 0))) | PS_TARGET | PS_AUTO_ENABLE,
	       reg);
}

static int t6040_ps_raise(void __iomem *base, u32 offset)
{
	int ret = t6040_ps_raise_start(base, offset);

	t6040_ps_raise_finish(base, offset);

	return ret;
}

static bool t6040_ps_actual_on(void __iomem *base, u32 offset)
{
	return FIELD_GET(PS_ACTUAL, readl(base + offset));
}

/* The shared PHY and PHY-IP to sleep: the PCIe root complex disable sequence. */
static void t6040_apcie_phy_off(struct t6040_sleep *s)
{
	struct t6040_apcie_st *a = &s->apcie_st;
	void __iomem *ctrl = a->phy + APCIE_PHY_CTRL;
	void __iomem *run = a->phy + APCIE_PHY_RUN;
	u32 val;

	writel(readl(ctrl) | APCIE_PHY_CTRL_UP, ctrl);
	udelay(10);
	writel(readl(a->phy_ip + 0x38) & ~GENMASK(27, 26), a->phy_ip + 0x38);
	writel(readl(a->phy_ip + 0x8c) | GENMASK(27, 24), a->phy_ip + 0x8c);
	udelay(5);
	writel(readl(ctrl) | APCIE_PHY_CTRL_RESET, ctrl);
	udelay(1);
	writel(readl(ctrl) & ~APCIE_PHY_CTRL_CLK1REQ, ctrl);
	readl_poll_timeout_atomic(ctrl, val, !(val & APCIE_PHY_CTRL_CLK1ACK), 1, APCIE_POLL_US);
	udelay(1);
	writel(readl(ctrl) & ~APCIE_PHY_CTRL_CLK0REQ, ctrl);
	readl_poll_timeout_atomic(ctrl, val, !(val & APCIE_PHY_CTRL_CLK0ACK), 1, APCIE_POLL_US);
	udelay(1);
	writel(readl(ctrl) & ~APCIE_PHY_CTRL_UP, ctrl);
	udelay(10);
	writel(readl(run) & ~APCIE_PHY_RUN_EN2, run);
	writel(readl(run) & ~APCIE_PHY_RUN_EN, run);
}

/* APCIE_PHY_SW to clock-gated, its busy bit polled clear. */
static void t6040_apcie_phy_sw_gate(struct t6040_sleep *s)
{
	void __iomem *reg = s->soc_east + PS_APCIE_PHY_SW;
	u32 val;

	writel(PS_APCIE_PHY_SW_CLKGATED, reg);
	readl_poll_timeout_atomic(reg, val, !(val & PS_BUSY), 1, PS_OFF_TIMEOUT_US);
}

/* The PCIe PLL shutdown: the clock generator's control word. */
static void t6040_apcie_pll_off(struct t6040_sleep *s)
{
	void __iomem *clk = s->apcie_st.clkgen + APCIE_CLK_CTRL;
	u32 val;

	writel((readl(clk) & ~(APCIE_CLK_RUN | GENMASK(4, 3))) | GENMASK(4, 3), clk);
	writel(readl(clk) | APCIE_CLK_OFF, clk);
	writel(readl(clk) & ~BIT(0), clk);
	readl_poll_timeout_atomic(clk, val, !(val & APCIE_CLK_BUSY), 1, APCIE_POLL_US);
	writel(readl(clk) | APCIE_CLK_OFF2, clk);
}

/*
 * The sleep engine hands each ATC port to its always-on side and then powers the port's
 * common domain off itself: it needs the four ATC commons on at that
 * point. With them off it waits for good raising ATC0_COMMON, whose parent FAB2_SOC is
 * off as well. FAB2_SOC is raised by hand, then ATC0..3_COMMON to a manual target 0xf,
 * then FAB2_SOC gets its automatic mode back: the powered children hold it up.
 */
static const u16 t6040_atc_common[4] = { 0x390, 0x3b8, 0x3e0, 0x408 };

static void t6040_atc_commons_on(struct t6040_sleep *s)
{
	void __iomem *b = s->soc_ps;
	int i;

	t6040_ps_raise_start(b, PS_FAB2_SOC);
	for (i = 0; i < ARRAY_SIZE(t6040_atc_common); i++)
		t6040_ps_raise_start(b, t6040_atc_common[i]);
	t6040_ps_raise_finish(b, PS_FAB2_SOC);
}

/*
 * The CFG writes of the PHY sleep entry, run on every ATC port before a sleep:
 * ATCPHY_CFG +0x2c bit 0 (S2R_AON_SYNC_RST_N), +0x08 bit 0 (CFG_S2R_FSM_EN) and +0x28 bit 0
 * (AHB_CLK_REQ_N) set, then +0x04 bit 8 (the clock's ack) polled. Without them the engine
 * waits for good on a port that carries DisplayPort lanes: port 3 (the HDMI output) always,
 * any other port once a display runs on it in DP alternate mode. A parked port, and
 * one its PHY driver has put to sleep, already has +0x28 bit 0 set and is left alone.
 */
#define ATC_CFG_CLK_STATUS		0x04
#define ATC_CFG_CLK_ACK			BIT(8)
#define ATC_CFG_S2R_FSM			0x08
#define ATC_CFG_S2R_FSM_EN		BIT(0)
#define ATC_CFG_AHB_CLK			0x28
#define ATC_CFG_AHB_CLK_REQ_N		BIT(0)
#define ATC_CFG_S2R_AON			0x2c
#define ATC_CFG_S2R_AON_SYNC_RST_N	BIT(0)

static void t6040_atc_s2r_entry(struct t6040_sleep *s)
{
	u32 v;
	int n;

	for (n = 0; n < ARRAY_SIZE(s->atc_cfg); n++) {
		void __iomem *c = s->atc_cfg[n];

		if (!c || (readl(c + ATC_CFG_AHB_CLK) & ATC_CFG_AHB_CLK_REQ_N))
			continue;
		writel(readl(c + ATC_CFG_S2R_AON) | ATC_CFG_S2R_AON_SYNC_RST_N,
		       c + ATC_CFG_S2R_AON);
		udelay(1);
		writel(readl(c + ATC_CFG_S2R_FSM) | ATC_CFG_S2R_FSM_EN, c + ATC_CFG_S2R_FSM);
		udelay(5);
		writel(readl(c + ATC_CFG_AHB_CLK) | ATC_CFG_AHB_CLK_REQ_N, c + ATC_CFG_AHB_CLK);
		readl_poll_timeout_atomic(c + ATC_CFG_CLK_STATUS, v, v & ATC_CFG_CLK_ACK, 1, 1000);
	}
}

/*
 * The 13 fabric power words. The fabrics are expected up through the entry, and the wake
 * polls these words for automatic mode with target 0xf. Under Linux genpd has powered some
 * of them off with their last consumer by now (FAB2_SOC with the ATC commons at
 * suspend_noirq). Right before the S2R timer each one not in that state is raised: target
 * 0xf, ACTUAL 0xf polled, then automatic mode. The ATC commons and the ports' PHY sleep
 * entry follow, in this order.
 */
static const struct {
	bool east;
	u16 off;
} t6040_fabrics[13] = {
	{ false, 0x110 } /* AFI */,      { false, 0x178 } /* AFC */,
	{ false, 0x448 } /* SBR */,      { false, 0x100 } /* PMS */,
	{ false, 0x168 } /* FAB0_SOC */, { false, 0x2d0 } /* FAB1_SOC */,
	{ false, 0x338 } /* FAB2_SOC */, { false, 0x440 } /* FAB5_SOC */,
	{ false, 0x480 } /* FAB6_SOC */, { false, 0x488 } /* FAB7_SOC */,
	{ true, 0x118 } /* FAB3_SOC */,  { true, 0x128 } /* FAB4_SOC */,
	{ false, 0x298 } /* FAB_AFR */,
};

static void t6040_fabrics_on(struct t6040_sleep *s)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(t6040_fabrics); i++) {
		void __iomem *b = t6040_fabrics[i].east ? s->soc_east : s->soc_ps;
		u32 off = t6040_fabrics[i].off, v;

		v = readl(b + off);
		if (!(v & PS_AUTO_ENABLE) || FIELD_GET(PS_TARGET, v) != PS_ACTIVE)
			t6040_ps_raise(b, off);
	}
	t6040_atc_commons_on(s);
	t6040_atc_s2r_entry(s);
}

/*
 * The memory channels' power gating. The sleep engine does not write the DCS power words: it
 * expects each DCS to gate itself after self-refresh. That takes the DCS's dynamic power-gate
 * mode (DCS +0x20 bits 12 and 8) and its power word in automatic mode with no minimum state.
 * Both are expected at boot and again after each wake. Nothing sets them under Linux (the DT leaves
 * the ps_dcs_* domains disabled: target 0xf, no automatic mode), and the engine then waits
 * for the first DCS pair's power gate for good. Both are applied in each .begin.
 */
static const struct {
	bool east;
	u16 off;
} t6040_dcs_ps[16] = {
	{ false, 0x188 }, { false, 0x1a0 }, { false, 0x1b0 }, { false, 0x1c0 },
	{ true, 0x2e0 },  { true, 0x2e8 },  { true, 0x2f0 },  { true, 0x2f8 },
	{ false, 0x1d0 }, { false, 0x1e0 }, { false, 0x1f0 }, { false, 0x200 },
	{ true, 0x300 },  { true, 0x308 },  { true, 0x310 },  { true, 0x318 },
};

static void __iomem *t6040_dcs_ps_reg(struct t6040_sleep *s, int i)
{
	void __iomem *b = t6040_dcs_ps[i].east ? s->soc_east : s->soc_ps;

	return b + t6040_dcs_ps[i].off;
}

/*
 * In order: the minimum state cleared, target 0xf, ACTUAL 0xf polled, then automatic
 * mode. Returns a mask of the DCS whose ACTUAL did not reach 0xf.
 */
static u32 t6040_dcs_ps_auto_on(struct t6040_sleep *s)
{
	u32 to = 0, v;
	int i;

	for (i = 0; i < ARRAY_SIZE(t6040_dcs_ps); i++) {
		void __iomem *r = t6040_dcs_ps_reg(s, i);

		v = readl(r);
		if ((v & PS_AUTO_ENABLE) && !(v & PS_MIN))
			continue;
		writel((v & ~(PS_MIN | PS_FLAGS | PS_TARGET)) |
		       FIELD_PREP(PS_TARGET, PS_ACTIVE), r);
		if (readl_poll_timeout_atomic(r, v, FIELD_GET(PS_ACTUAL, v) == PS_ACTIVE, 1,
					      PS_OFF_TIMEOUT_US))
			to |= BIT(i);
		writel(readl(r) | PS_AUTO_ENABLE, r);
	}
	return to;
}

#define DCS_PG_CTL		0x20
#define DCS_PG_DYN		(BIT(12) | BIT(8))

static void t6040_dcs_dyn_pg(struct t6040_sleep *s)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(s->dcs); i++) {
		u32 v;

		if (!s->dcs[i])
			continue;
		v = readl(s->dcs[i] + DCS_PG_CTL);
		if ((v & DCS_PG_DYN) != DCS_PG_DYN)
			writel(v | DCS_PG_DYN, s->dcs[i] + DCS_PG_CTL);
	}
}

/*
 * ANS's own fabric bridge (0x4_0b00_0000 +0x104/+0x108 bit 0). The bit is cleared before a
 * retained domain goes off and set again once it is back up; here the wake sets it
 * (t6040_apcie_st_raise()) and the SoC's sleep engine, not this driver, takes ANS down.
 * The boot loader leaves the bit clear, so a boot's first sleep goes down with it clear;
 * without this the next one meets the bridge as the first wake armed it. Only while ANS is
 * on: the bridge sits in it.
 */
static void t6040_ans_bridge_quiesce(struct t6040_sleep *s)
{
	void __iomem *bridge = s->axi2af_bridge[AXI2AF_ANS];
	u32 a, b;

	if (!bridge ||
	    FIELD_GET(PS_ACTUAL, readl(s->soc_east + PS_ANS)) != PS_ACTIVE)
		return;
	a = readl(bridge + AXI2AF_BRIDGE_104);
	b = readl(bridge + AXI2AF_BRIDGE_108);
	if (!((a | b) & BIT(0)))
		return;
	writel(a & ~BIT(0), bridge + AXI2AF_BRIDGE_104);
	writel(b & ~BIT(0), bridge + AXI2AF_BRIDGE_108);
}

/*
 * Quiesce ANS's storage link, once ANS has been stopped by its driver. The PHY, the
 * clock generator and the AXI enables are treated as shared by the two ST lanes.
 *
 * The power words of this group are left to the SoC's sleep engine, which powers it down
 * itself and in its own order: APCIE_PHY_SW to clock-gated, APCIE_SYS_ST0/1 off, APCIE_ST0/1
 * clock-gated, ANS off, APCIE_SYS_GP off, APCIE_GP clock-gated. It polls APCIE_PHY_SW for
 * its parents, which are both SYS_ST lanes: with them lowered here, as the T8140 controller
 * does, it waits there for good. So the commit raises APCIE_SYS_ST1, which
 * Linux never uses, and does only what the engine needs before it runs: the shared PHY put
 * to sleep, APCIE_PHY_SW clock-gated while APCIE_GP is still on, the PCIe PLL shut down.
 */
static void t6040_apcie_st_lower(struct t6040_sleep *s)
{
	struct t6040_apcie_st *a = &s->apcie_st;
	void __iomem *ps = s->soc_east;

	if (!a->valid || !ps)
		return;

	/* The PHY's registers need a lane domain on; quiet it first (shared). */
	if (a->phy && t6040_ps_actual_on(ps, PS_APCIE_SYS_ST0))
		t6040_apcie_phy_off(s);
	t6040_apcie_phy_sw_gate(s);
	if (a->clkgen && t6040_ps_actual_on(ps, PS_APCIE_SYS_ST0))
		t6040_apcie_pll_off(s);

	/* AXI enables off (shared). */
	if (a->axi) {
		writel(readl(a->axi + APCIE_AXI_EN0) & ~BIT(0), a->axi + APCIE_AXI_EN0);
		writel(readl(a->axi + APCIE_AXI_EN1) & ~BIT(0), a->axi + APCIE_AXI_EN1);
	}
	t6040_ans_bridge_quiesce(s);
}

static void t6040_bridge_rearm(void __iomem *bridge)
{
	if (!bridge)
		return;
	writel(readl(bridge + AXI2AF_BRIDGE_104) | BIT(0), bridge + AXI2AF_BRIDGE_104);
	writel(readl(bridge + AXI2AF_BRIDGE_108) | BIT(0), bridge + AXI2AF_BRIDGE_108);
}

/*
 * Raise ANS's storage link from power-off, the two-lane adaptation of
 * sleep.c's apple_sleep_raise_apcie(). Runs atomic on the waking core in
 * t6040_sleep_return(), after t6040_axi2af_rearm() and before control returns to
 * the PM core, so nvme's resume_early sets the ANS run bit onto a live link.
 * Both lanes come up; the PLL/clkgen/PHY tables run once (shared). The two
 * apcie_sys_st domains are not in the 15-domain axi2af table, so their bridges
 * get no re-arm.
 *
 * The AXI enables (+0x104/+0x108 of 0x4_1600_0000) are the fabric bridge of
 * APCIE_SYS_GP, which the commit lowered together with APCIE_GP: both are raised
 * here before the enables are written. APCIE_PHY_SW, which the commit gated,
 * comes back before the PHY, and the PHY's reference PLL (common +0 bit 31) is
 * waited for before the PHY's clocks are requested. @apcie_wake_step tells a
 * failed raise's stage.
 */
static int t6040_apcie_st_raise(struct t6040_sleep *s)
{
	struct t6040_apcie_st *a = &s->apcie_st;
	void __iomem *ps = s->soc_east;
	void __iomem *ctrl, *run;
	u32 val;
	int ret;

	if (!a->valid || !ps)
		return 0;
	ctrl = a->phy + APCIE_PHY_CTRL;
	run = a->phy + APCIE_PHY_RUN;

	/*
	 * In order, each domain to ACTUAL 0xf: ANS, GP, SYS_GP, ST0, SYS_ST0, ST1, SYS_ST1,
	 * the fabric-bridge bit0s set after ANS and after SYS_GP. SYS_GP stays at a manual
	 * target 0xf: in automatic mode it gates again, and the DART behind it then faults in
	 * its resume.
	 */
	ret = t6040_ps_raise(ps, PS_ANS);
	if (!ret)
		t6040_bridge_rearm(s->axi2af_bridge[AXI2AF_ANS]);
	ret |= t6040_ps_raise(ps, PS_APCIE_GP);
	if (!ret)
		ret = t6040_ps_raise_start(ps, PS_APCIE_SYS_GP);
	if (!ret) {
		writel(readl(a->axi + APCIE_AXI_EN0) | BIT(0), a->axi + APCIE_AXI_EN0);
		writel(readl(a->axi + APCIE_AXI_EN1) | BIT(0), a->axi + APCIE_AXI_EN1);
	}
	ret |= t6040_ps_raise(ps, PS_APCIE_ST0);
	ret |= t6040_ps_raise(ps, PS_APCIE_SYS_ST0);
	ret |= t6040_ps_raise(ps, PS_APCIE_ST1);
	ret |= t6040_ps_raise(ps, PS_APCIE_SYS_ST1);
	t6040_wake_sync();
	s->apcie_wake_step = 1;
	if (ret)
		return ret;

	/* Tunables + clkgen (shared). Both OFF bits clear; only a power-off cleared them. */
	apple_tunable_apply(a->axi, a->axi_tunable);
	apple_tunable_apply(a->cio3pll, a->cio3pll_tunable);
	apple_tunable_apply(a->clkgen, a->clkgen_tunable);
	writel(readl(a->clkgen + APCIE_CLK_START) | APCIE_CLK_START_GO,
	       a->clkgen + APCIE_CLK_START);
	writel(readl(a->clkgen + APCIE_CLK_CTRL) & ~(APCIE_CLK_OFF2 | APCIE_CLK_OFF),
	       a->clkgen + APCIE_CLK_CTRL);
	ret = readl_poll_timeout_atomic(a->clkgen + APCIE_CLK_CTRL, val,
					!(val & APCIE_CLK_BUSY), 1, APCIE_POLL_US);
	t6040_wake_sync();
	s->apcie_wake_step = 2;
	if (ret)
		return ret;

	ret = t6040_ps_raise(ps, PS_APCIE_PHY_SW);
	s->apcie_wake_step = 3;
	if (ret)
		return ret;

	/* Shared PHY bring-up + PLL/lane tables (as sleep.c's sleep_apcie_phy_ip_apply()). */
	apple_tunable_apply(a->phy, a->phy_tunable);
	ret = readl_poll_timeout_atomic(a->phy + APCIE_PHY_COMMON, val, val & BIT(31), 1,
					APCIE_POLL_US);
	t6040_wake_sync();
	s->apcie_wake_step = 4;
	if (ret)
		return ret;
	writel(readl(ctrl) | APCIE_PHY_CTRL_CLK0REQ, ctrl);
	ret = readl_poll_timeout_atomic(ctrl, val, val & APCIE_PHY_CTRL_CLK0ACK, 1, APCIE_POLL_US);
	if (ret)
		return ret;
	writel(readl(ctrl) | APCIE_PHY_CTRL_CLK1REQ, ctrl);
	ret = readl_poll_timeout_atomic(ctrl, val, val & APCIE_PHY_CTRL_CLK1ACK, 1, APCIE_POLL_US);
	if (ret)
		return ret;
	writel(readl(ctrl) & ~APCIE_PHY_CTRL_RESET, ctrl);
	t6040_wake_sync();
	s->apcie_wake_step = 5;

	writel(readl(run) | APCIE_PHY_RUN_EN, run);
	sleep_tunable_apply_changed(a->phy_ip, a->phy_ip_pll);
	sleep_tunable_apply_changed(a->phy_ip, a->phy_ip_lanes);
	writel(readl(a->phy_ip + APCIE_PHY_IP_PLL_CTL) & ~APCIE_PHY_IP_PLL_HOLD,
	       a->phy_ip + APCIE_PHY_IP_PLL_CTL);
	writel(readl(run) | APCIE_PHY_RUN_EN2, run);
	ret = readl_poll_timeout_atomic(a->phy + APCIE_PHY_READY, val, val & BIT(0), 1,
					APCIE_POLL_US);
	if (ret)
		return ret;
	t6040_wake_sync();
	s->apcie_wake_step = 6;
	writel(readl(a->phy + APCIE_PHY_COMMON) | BIT(0), a->phy + APCIE_PHY_COMMON);
	writel(readl(ctrl) | APCIE_PHY_CTRL_UP, ctrl);
	t6040_wake_sync();
	s->apcie_wake_step = 7;

	/* RC ready handshake (as T8140's). */
	writel(0, a->rc + APCIE_RC_ZERO);
	writel(0x140, a->rc + APCIE_RC_READY_CFG);
	writel(1, a->rc + APCIE_RC_READY_START);
	return readl_poll_timeout_atomic(a->rc + APCIE_RC_READY_STAT, val, val & BIT(0), 1,
					 APCIE_POLL_US);
}

/*
 * The first thing Linux runs after a wake, MMU off and no stack (as the T8132 stub does). The
 * last wake program leaves the AON WD2 running for 5 s, less than the device resume takes:
 * restart it for 60 s, in t6040_sleep_set_wd2()'s order, then take cpu_resume. The PA is
 * the T6040 AON WDT (T6040_WDT_PA), used only when the DT's wdt block is that one.
 */
asm("	.pushsection .text, \"ax\"\n"
"	.type	t6040_resume_stub, %function\n"
"t6040_resume_stub:\n"
"	movz	x9, #0xc000\n"
"	movk	x9, #0x0836, lsl #16\n"
"	movk	x9, #0x5, lsl #32\n"
"	str	wzr, [x9, #0x2c]\n"
"	str	wzr, [x9, #0x20]\n"
"	movz	w10, #0xa800\n"
"	movk	w10, #0x55d4, lsl #16\n"
"	str	w10, [x9, #0x28]\n"
"	mov	w10, #1\n"
"	str	w10, [x9, #0x2c]\n"
"	b	cpu_resume\n"
"	.size	t6040_resume_stub, . - t6040_resume_stub\n"
"	.popsection\n"
);
extern void t6040_resume_stub(void);

/*
 * The four ATC complexes' power words in SOC (soc_ps, 0x502800000), deepest first: cio_usb,
 * cio_pcie, cio, pcie, common (ADT: ATCn_CIO_USB .. ATCn_COMMON). The quiesce gate wants
 * them off; their drivers don't lower atcN_common.
 *
 * Their common_dp, usb, phymxwrap and usb_aon words are MiniPMGR words (0x508300000 + 0xa0..,
 * ADT ATCn_COMMON_DP/USB/PHYMXWRAP/USB_AON) and not in this table: the same offsets in SOC
 * are AIC, PMC, PMS_FPWM0-4 and FAB0_SOC.
 */
static const u16 t6040_atc_words[4][5] = {
	{ 0x3b0, 0x3a8, 0x398, 0x3a0, 0x390 },
	{ 0x3d8, 0x3d0, 0x3c0, 0x3c8, 0x3b8 },
	{ 0x400, 0x3f8, 0x3e8, 0x3f0, 0x3e0 },
	{ 0x438, 0x430, 0x420, 0x428, 0x408 },
};

/* A domain lowered (T8140 measured): AUTO_ENABLE off first, then target 0, poll. */
static int t6040_ps_lower_word(void __iomem *reg)
{
	u32 val = readl(reg);

	if (val & PS_AUTO_ENABLE) {
		val &= ~(PS_AUTO_ENABLE | PS_FLAGS);
		writel(val, reg);
	}
	writel((val & ~PS_TARGET) | PS_FLAGS, reg);
	return readl_poll_timeout_atomic(reg, val, !FIELD_GET(PS_ACTUAL, val), 1,
					 PS_OFF_TIMEOUT_US);
}

static void t6040_atc_lower(struct t6040_sleep *s)
{
	int p, k;

	for (p = 0; p < ARRAY_SIZE(t6040_atc_words); p++)
		for (k = 0; k < ARRAY_SIZE(t6040_atc_words[0]); k++) {
			void __iomem *reg = s->soc_ps + t6040_atc_words[p][k];
			u32 val = readl(reg);

			if (!FIELD_GET(PS_TARGET, val) && !FIELD_GET(PS_ACTUAL, val))
				continue;
			t6040_ps_lower_word(reg);
		}
}

/*
 * T8140's radio port stop, on the same t602x port block: the link into L2, the endpoint's
 * PERST# asserted, the PHY's clocks off. The endpoint keeps its power and comes back by its
 * host after the sleep.
 */
static void t6040_gp_port_stop(struct t6040_sleep *s, int p)
{
	void __iomem *port = s->gp_port[p];
	void __iomem *phy = s->gp_phy[p];
	u32 val;

	if (!port || !phy)
		return;
	s->gp_phy_saved[p] = readl(phy);
	s->gp_appclk_saved[p] = readl(port + APCIE_PORT_APPCLK);
	s->gp_perst_saved[p] = readl(port + APCIE_PORT_PERST);
	s->gp_saved[p] = true;
	writel(readl(port + APCIE_PORT_INTMSK) | APCIE_PORT_INT_LINK, port + APCIE_PORT_INTMSK);
	writel(APCIE_PORT_INT_LINK, port + APCIE_PORT_INTSTAT);
	writel(readl(port + APCIE_PORT_APPCLK) | APCIE_PORT_APPCLK_CGDIS, port + APCIE_PORT_APPCLK);
	if (readl(port + APCIE_PORT_LINKSTS) & APCIE_PORT_LINKSTS_UP) {
		writel(APCIE_PORT_L2_ENTER, port + APCIE_PORT_L2);
		readl_poll_timeout_atomic(port + APCIE_PORT_L2, val, !(val & APCIE_PORT_L2_BUSY),
					  10, APCIE_PORT_STOP_US);
	}
	if (s->gp_pins && s->gp_perst[p] >= 0) {
		void __iomem *reg = s->gp_pins + 4 * s->gp_perst[p];

		val = readl(reg);
		writel(s->gp_perst_low[p] ? val & ~GP_PIN_DATA : val | GP_PIN_DATA, reg);
	}
	writel(readl(phy) & ~APCIE_PORT_PHY_LINK, phy);
	udelay(1000);
	/* What this bit does isn't known. It is set around PERST# (T8140). */
	writel(readl(port + APCIE_PORT_PERST_GUARD) | BIT(8), port + APCIE_PORT_PERST_GUARD);
	writel(0, port + APCIE_PORT_PERST);
	writel(readl(port + APCIE_PORT_PERST_GUARD) & ~BIT(8), port + APCIE_PORT_PERST_GUARD);
	writel(readl(phy) & ~APCIE_PORT_PHY_EN1, phy);
	writel(readl(phy) & ~APCIE_PORT_PHY_EN0, phy);
	writel(readl(phy) | APCIE_PHY_CTRL_RESET, phy);
	writel(readl(phy) & ~APCIE_PHY_CTRL_CLK0REQ, phy);
	readl_poll_timeout_atomic(phy, val, !(val & APCIE_PHY_CTRL_CLK0ACK), 1, APCIE_PORT_STOP_US);
	writel(readl(phy) & ~APCIE_PHY_CTRL_CLK1REQ, phy);
	writel(readl(port + APCIE_PORT_APPCLK) & ~APCIE_PORT_APPCLK_EN, port + APCIE_PORT_APPCLK);
	writel(readl(port + APCIE_PORT_INTSTAT), port + APCIE_PORT_INTSTAT);
}

/* Whether the GP host's system domain is still on; if not, there is nothing left to stop. */
static __always_inline bool t6040_apcie_sys_gp_on(struct t6040_sleep *s)
{
	return FIELD_GET(PS_ACTUAL, readl(s->soc_east + PS_APCIE_SYS_GP));
}

/* The GP host's two ports, while the block's domains are all still on. */
static void t6040_gp_ports_stop(struct t6040_sleep *s)
{
	int p;

	if (!t6040_apcie_sys_gp_on(s))
		return;
	for (p = 0; p < ARRAY_SIZE(s->gp_port); p++)
		t6040_gp_port_stop(s, p);
}

/*
 * apcie_sys_gp (AUTO off, axi2af bit0s, target 0), then apcie_gp. After ANS's ST lanes: the
 * storage lanes' cio3pll/clkgen/PHY windows sit in apcie_gp, and an access to them with it
 * off hangs the bus. T8140's order: SYS_ST, ST, SYS_GP, GP. APCIE_GP goes to clock-gated
 * (TARGET 4), the state the sleep engine itself asks for, not to off.
 */
static void t6040_apcie_gp_lower(struct t6040_sleep *s)
{
	void __iomem *sys, *gp, *bridge = s->axi2af_bridge[AXI2AF_APCIE_SYS_GP];
	u32 val;

	sys = s->soc_east + PS_APCIE_SYS_GP;
	gp = s->soc_east + PS_APCIE_GP;
	if (!t6040_apcie_sys_gp_on(s))
		return;

	val = readl(sys);
	if (val & PS_AUTO_ENABLE) {
		val &= ~(PS_AUTO_ENABLE | PS_FLAGS);
		writel(val, sys);
	}
	if (bridge) {
		writel(readl(bridge + AXI2AF_BRIDGE_104) & ~BIT(0), bridge + AXI2AF_BRIDGE_104);
		writel(readl(bridge + AXI2AF_BRIDGE_108) & ~BIT(0), bridge + AXI2AF_BRIDGE_108);
	}
	writel((val & ~PS_TARGET) | PS_FLAGS, sys);
	readl_poll_timeout_atomic(sys, val, !FIELD_GET(PS_ACTUAL, val), 1, PS_OFF_TIMEOUT_US);

	writel(PS_APCIE_GP_CLKGATED, gp);
	readl_poll_timeout_atomic(gp, val, FIELD_GET(PS_ACTUAL, val) == 4, 1, PS_OFF_TIMEOUT_US);
}

/*
 * The GP PCIe host's two ports (Wi-Fi/BT, SD): the boot loader set their clocks up, and
 * nobody does after a sleep. Without them the DART in port 0's block faults in its noirq
 * resume. t6040_gp_port_stop() backwards, short of the link: the lane reference clocks
 * requested and acknowledged, the PHY's enables and the port's application clock as saved,
 * the controller's PERST register as saved, then the port's ready bit polled. The link
 * itself stays down for the host driver.
 */
static void t6040_gp_pcie_clk(struct t6040_sleep *s)
{
	const u32 appclk = APCIE_PORT_APPCLK_EN | APCIE_PORT_APPCLK_CGDIS;
	unsigned int timeouts = 0;
	u32 val;
	int i;

	s->gp_clk_timeouts = 0;
	/* The port blocks answer only with GP and SYS_GP on. */
	if (FIELD_GET(PS_ACTUAL, readl(s->soc_east + PS_APCIE_GP)) != PS_ACTIVE ||
	    FIELD_GET(PS_ACTUAL, readl(s->soc_east + PS_APCIE_SYS_GP)) != PS_ACTIVE)
		return;
	for (i = 0; i < ARRAY_SIZE(s->gp_port); i++) {
		void __iomem *phy = s->gp_phy[i], *port = s->gp_port[i];
		u32 cfg, app;

		if (!phy || !port || !s->gp_saved[i])
			continue;
		cfg = s->gp_phy_saved[i];
		app = s->gp_appclk_saved[i];
		writel(readl(phy) | APCIE_PHY_CTRL_CLK0REQ, phy);
		if (readl_poll_timeout_atomic(phy, val, val & APCIE_PHY_CTRL_CLK0ACK, 1, 50000))
			timeouts++;
		writel(readl(phy) | APCIE_PHY_CTRL_CLK1REQ, phy);
		if (readl_poll_timeout_atomic(phy, val, val & APCIE_PHY_CTRL_CLK1ACK, 1, 50000))
			timeouts++;
		if (!(cfg & APCIE_PHY_CTRL_RESET)) {
			writel(readl(phy) & ~APCIE_PHY_CTRL_RESET, phy);
			udelay(1);
		}
		writel(readl(phy) | (cfg & (APCIE_PORT_PHY_EN0 | APCIE_PORT_PHY_EN1)), phy);
		writel(readl(phy) | (cfg & APCIE_PORT_PHY_LINK), phy);
		writel((readl(port + APCIE_PORT_APPCLK) & ~appclk) | (app & appclk),
		       port + APCIE_PORT_APPCLK);
		/*
		 * The stop also wrote the controller's PERST register 0, and without it the
		 * port does not report ready. Put it back as it was (the endpoint's PERST# pin
		 * stays asserted), with the guard bit set around it.
		 */
		writel(readl(port + APCIE_PORT_PERST_GUARD) | BIT(8),
		       port + APCIE_PORT_PERST_GUARD);
		writel(s->gp_perst_saved[i], port + APCIE_PORT_PERST);
		writel(readl(port + APCIE_PORT_PERST_GUARD) & ~BIT(8),
		       port + APCIE_PORT_PERST_GUARD);
		if (readl_poll_timeout_atomic(port + APCIE_PORT_STATUS, val,
					      val & APCIE_PORT_STATUS_READY, 1, 50000))
			timeouts++;
	}
	s->gp_clk_timeouts = timeouts;
}

/*
 * The VDD_CIO rail request (SMC key pmVC) is written in each .begin; its key and value
 * belong to the SMC driver (apple_smc_request_pmvc()).
 */

/* The SMC node is apple,t6041-smc + apple,t8103-smc here (macsmc's match list). */
static const struct of_device_id t6040_smc_match[] = {
	{ .compatible = "apple,t8103-smc" },
	{ .compatible = "apple,smc" },
	{}
};

/*
 * regmap_read_poll_timeout_atomic() reads ktime_get(), which warns while timekeeping is
 * suspended. Counted 1 us steps instead.
 */
static int t6040_regmap_poll_off(struct regmap *map, u32 off, u32 *cur)
{
	unsigned int us;

	for (us = 0; ; us++) {
		if (regmap_read(map, off, cur))
			return -EIO;
		if (!FIELD_GET(PS_ACTUAL, *cur))
			return 0;
		if (us >= PS_OFF_TIMEOUT_US)
			return -ETIMEDOUT;
		udelay(1);
	}
}

/* A power word behind a regmap (pmgr_disp) or a plain window (soc_east). */
struct t6040_psw {
	struct regmap *map;
	void __iomem *io;
	u32 off;
};

static u32 t6040_psw_read(const struct t6040_psw *w)
{
	u32 v = 0;

	if (w->map)
		regmap_read(w->map, w->off, &v);
	else
		v = readl(w->io + w->off);
	return v;
}

static void t6040_psw_write(const struct t6040_psw *w, u32 v)
{
	if (w->map)
		regmap_write(w->map, w->off, v);
	else
		writel(v, w->io + w->off);
}

static int t6040_psw_poll_long(const struct t6040_psw *w, u32 mask, u32 want, unsigned int max_us)
{
	unsigned int us;

	for (us = 0; us < max_us; us++) {
		if ((t6040_psw_read(w) & mask) == want)
			return 0;
		udelay(1);
	}
	return -ETIMEDOUT;
}

static int t6040_psw_poll(const struct t6040_psw *w, u32 mask, u32 want)
{
	unsigned int us;

	for (us = 0; us < DISP_PS_TIMEOUT_US; us++) {
		if ((t6040_psw_read(w) & mask) == want)
			return 0;
		udelay(1);
	}
	return -ETIMEDOUT;
}

/*
 * The power-down of a PS word: with
 * AUTO_ENABLE set, take it out of automatic mode with the target kept and the flags cleared, and
 * wait for ACTUAL 15; then @pre (the SYS hooks); then TARGET 0 with bits 9:8 written 1 and
 * bits 10-12, 24-31 kept; then poll bit11 clear and ACTUAL 0.
 */
static int t6040_psw_off(struct t6040_sleep *s, const struct t6040_psw *w,
			 void (*pre)(struct t6040_sleep *s))
{
	u32 v = t6040_psw_read(w);

	if (v & PS_AUTO_ENABLE) {
		t6040_psw_write(w, v & ~(PS_AUTO_ENABLE | PS_FLAGS));
		t6040_psw_poll(w, PS_ACTUAL, PS_ACTUAL);
		v = t6040_psw_read(w);
	}
	if (pre)
		pre(s);
	t6040_psw_write(w, (v & ~(PS_FLAGS | PS_TARGET)) | PS_FLAGS);
	t6040_psw_poll(w, PS_BUSY, 0);
	return t6040_psw_poll(w, PS_ACTUAL, 0);
}

/* DISP_SYS's hooks before its PS write: the axi2af bridge bit0s, then the retention pair. */
static void t6040_disp_sys_pre(struct t6040_sleep *s)
{
	void __iomem *bridge = s->axi2af_bridge[AXI2AF_DISP_SYS];

	/* Behind DISP_SYS: an access with the domain already down never completes. */
	if (bridge && FIELD_GET(PS_ACTUAL, readl(s->soc_east + PS_DISP_SYS))) {
		writel(readl(bridge + AXI2AF_BRIDGE_104) & ~BIT(0), bridge + AXI2AF_BRIDGE_104);
		writel(readl(bridge + AXI2AF_BRIDGE_108) & ~BIT(0), bridge + AXI2AF_BRIDGE_108);
	}
	if (s->disp_pwrgate) {
		writel(3, s->disp_pwrgate + DISP_PWRGATE_A);
		writel(0, s->disp_pwrgate + DISP_PWRGATE_B);
	}
}

/* dart-dcp's outstanding-transaction counters at 0 before its domain goes. */
#define DCP_DART_TXN_COUNT	GENMASK(20, 16)

static int t6040_dcp_dart_idle(struct t6040_sleep *s)
{
	static const u16 regs[] = { 0x240, 0x244, 0x250, 0x254, 0x260, 0x270 };
	unsigned int i, us = 0;

	if (!s->dcp_dart)
		return 0;
	for (i = 0; i < ARRAY_SIZE(regs); i++)
		while (readl(s->dcp_dart + regs[i]) & DCP_DART_TXN_COUNT) {
			if (++us > DISP_PS_TIMEOUT_US)
				return -ETIMEDOUT;
			udelay(1);
		}
	return 0;
}

/* The internal display's FE and SYS domains, child first, after its sub-blocks. */
static int t6040_disp_off(struct t6040_sleep *s, const struct t6040_sleep_group *g)
{
	struct t6040_psw cpu = { .map = g->regmap, .off = g->first };
	struct t6040_psw fe = { .io = s->soc_east, .off = PS_DISP_FE };
	struct t6040_psw sys = { .io = s->soc_east, .off = PS_DISP_SYS };
	int ret;

	s->disp_down = true;
	s->disp_by_driver = !FIELD_GET(PS_ACTUAL, t6040_psw_read(&sys));
	/*
	 * The PMC's bandwidth-request and boost interface enables of the display pipes
	 * (0x5048ac020 / 0x5048ac028, one bit per pipe; they belong set while a pipe's SYS
	 * domain is powered). The power loss clears them and nothing sets them
	 * again: without its boost bit a pipe's backlight data stream never runs.
	 */
	s->pmc_if_valid = false;
	if (s->pmc_bwr) {
		s->pmc_if_saved[0] = readl(s->pmc_bwr);
		s->pmc_if_saved[1] = readl(s->pmc_bwr + PMC_BOOST);
		s->pmc_if_valid = true;
	}
	/* The PMS DCP voters to 0 first, as T8140's driver ends. */
	if (s->pmc_dcp) {
		s->pmc_dcp_saved[0] = readl(s->pmc_dcp);
		s->pmc_dcp_saved[1] = readl(s->pmc_dcp + 4);
		writel(0, s->pmc_dcp);
		writel(0, s->pmc_dcp + 4);
	}
	/*
	 * DISP_CPU's word refuses any write from here: rewriting its own value at runtime reset
	 * the SoC. Its CPU is left in automatic mode and the PMGR gates it once the firmware
	 * idles in WFI with the run bit kept: wait for ACTUAL 0, after dart-dcp's transactions
	 * have drained, and go on either way.
	 */
	if (FIELD_GET(PS_ACTUAL, t6040_psw_read(&cpu)) && !s->disp_by_driver)
		t6040_dcp_dart_idle(s);
	t6040_psw_poll_long(&cpu, PS_ACTUAL, 0, DISP_CPU_GATE_US);
	/* Lowered by the display driver, which also raises it again after the wake. */
	if (s->disp_by_driver)
		return 0;

	ret = t6040_psw_off(s, &fe, NULL);
	if (!ret)
		ret = t6040_psw_off(s, &sys, t6040_disp_sys_pre);
	return ret;
}

/*
 * The raise (the mirror): TARGET 15, poll ACTUAL 15, poll bit11; for DISP_SYS the
 * retention pair (A := 0, B := 1) and the axi2af bridge bit0s set; then bits 9:8 cleared and
 * AUTO_ENABLE back where it belongs (DISP_CPU, DISP_SYS).
 */
static int t6040_psw_on(struct t6040_sleep *s, const struct t6040_psw *w, bool sys,
			bool autoen)
{
	u32 v = t6040_psw_read(w);
	int ret;

	t6040_psw_write(w, (v & ~(PS_AUTO_ENABLE | PS_FLAGS)) | PS_TARGET);
	ret = t6040_psw_poll(w, PS_ACTUAL, PS_ACTUAL);
	t6040_psw_poll(w, PS_BUSY, 0);
	if (sys) {
		void __iomem *bridge = s->axi2af_bridge[AXI2AF_DISP_SYS];

		if (s->disp_pwrgate) {
			writel(0, s->disp_pwrgate + DISP_PWRGATE_A);
			writel(1, s->disp_pwrgate + DISP_PWRGATE_B);
		}
		if (bridge && !ret) {
			writel(readl(bridge + AXI2AF_BRIDGE_104) | BIT(0),
			       bridge + AXI2AF_BRIDGE_104);
			writel(readl(bridge + AXI2AF_BRIDGE_108) | BIT(0),
			       bridge + AXI2AF_BRIDGE_108);
		}
	}
	v = t6040_psw_read(w);
	t6040_psw_write(w, (v & ~PS_AUTO_ENABLE) | PS_FLAGS | (autoen ? PS_AUTO_ENABLE : 0));
	return ret;
}

/* The internal display back on, parent first, whatever brought .enter back. */
static void t6040_disp_back(struct t6040_sleep *s)
{
	struct t6040_psw sys = { .io = s->soc_east, .off = PS_DISP_SYS };
	struct t6040_psw fe = { .io = s->soc_east, .off = PS_DISP_FE };

	if (!s->disp_down)
		return;
	s->disp_down = false;
	/* DISP_CPU's word can't be written from here: automatic mode raises it. */
	if (!s->disp_by_driver) {
		if (t6040_psw_on(s, &sys, true, true))
			return;
		if (t6040_psw_on(s, &fe, false, false))
			return;
	}
	if (s->pmc_dcp) {
		writel(s->pmc_dcp_saved[1], s->pmc_dcp + 4);
		writel(s->pmc_dcp_saved[0], s->pmc_dcp);
	}
	if (s->pmc_if_valid) {
		u32 bwr = readl(s->pmc_bwr), boost = readl(s->pmc_bwr + PMC_BOOST);

		s->pmc_if_valid = false;
		if (bwr != s->pmc_if_saved[0])
			writel(bwr | s->pmc_if_saved[0], s->pmc_bwr);
		if (boost != s->pmc_if_saved[1])
			writel(boost | s->pmc_if_saved[1], s->pmc_bwr + PMC_BOOST);
		if (bwr != s->pmc_if_saved[0] || boost != s->pmc_if_saved[1])
			dev_dbg(s->dev, "wake: PMC display interfaces BWR %#x boost %#x, put back to %#x %#x\n",
				bwr, boost, readl(s->pmc_bwr), readl(s->pmc_bwr + PMC_BOOST));
	}
}

/*
 * The display power words the ADT wants off before a sleep (apple,check-ps-b4-sleep), lowered
 * here: after the device suspend, interrupts off, one CPU up, so no driver, interrupt handler
 * or DART touches the pipes any more. The DCPs only stop their firmware: lowering the domains
 * from their suspend makes the SoC panic. Each group from its last sub-block up to, and not
 * including, its pipe CPU, which the PMGR gates by itself: each word as the pmgr-pwrstate
 * driver powers a domain off (TARGET 0, the one-shot bits clear), polled to ACTUAL 0. The
 * internal display's FE and SYS domains follow. Returns the number of words left on.
 */
static unsigned int t6040_disp_lower(struct t6040_sleep *s)
{
	unsigned int i, left = 0;

	for (i = 0; i < s->ngroups; i++) {
		const struct t6040_sleep_group *g = &s->groups[i];
		u32 off = g->last;

		for (;;) {
			bool cpu = off == g->first;
			u32 val, cur = 0;

			/* An automatically gated CPU still on: wait for the PMGR to gate it. */
			if (cpu && !regmap_read(g->regmap, off, &val) && (val & PS_AUTO_ENABLE) &&
			    FIELD_GET(PS_ACTUAL, val))
				t6040_regmap_poll_off(g->regmap, off, &cur);
			if (!cpu && !regmap_read(g->regmap, off, &val) &&
			    t6040_sleep_ps_on(val, cpu)) {
				val &= ~(PS_TARGET | PS_AUTO_ENABLE | PS_FLAGS | PS_DEV_DISABLE |
					 PS_BUSY | PS_UNLATCH);
				regmap_write(g->regmap, off, val);
				if (t6040_regmap_poll_off(g->regmap, off, &cur)) {
					dev_warn(s->dev, "%s +%#x still on: %#x\n", g->name,
						 off, cur);
					left++;
				}
			}
			if (off == g->first)
				break;
			off -= PS_STRIDE;
		}
		if (!i && !strcmp(g->name, "disp") && t6040_disp_off(s, g))
			left++;
	}
	return left;
}


static int t6040_sleep_parse_groups(struct t6040_sleep *s)
{
	struct device_node *np = s->dev->of_node;
	const char *prop = "apple,check-ps-b4-sleep";
	int n, i;

	/* Each group: the PMGR syscon, the CPU's offset and the last sub-block's. */
	n = of_property_count_u32_elems(np, prop);
	if (n <= 0 || n % 3)
		return dev_err_probe(s->dev, -EINVAL, "no valid %s\n", prop);
	n /= 3;
	if (of_property_count_strings(np, "apple,check-ps-b4-sleep-names") != n)
		return dev_err_probe(s->dev, -EINVAL, "one name per %s group\n", prop);

	s->groups = devm_kcalloc(s->dev, n, sizeof(*s->groups), GFP_KERNEL);
	if (!s->groups)
		return -ENOMEM;

	for (i = 0; i < n; i++) {
		struct t6040_sleep_group *g = &s->groups[i];
		struct of_phandle_args args;
		int ret;

		ret = of_parse_phandle_with_fixed_args(np, prop, 2, i, &args);
		if (ret)
			return dev_err_probe(s->dev, ret, "%s[%d]\n", prop, i);
		g->regmap = syscon_node_to_regmap(args.np);
		of_node_put(args.np);
		if (IS_ERR(g->regmap))
			return dev_err_probe(s->dev, PTR_ERR(g->regmap), "%s[%d] regmap\n",
					     prop, i);
		g->first = args.args[0];
		g->last = args.args[1];
		if (g->first > g->last || (g->last - g->first) % PS_STRIDE ||
		    (g->last - g->first) / PS_STRIDE >= PS_MAX_PER_GROUP)
			return dev_err_probe(s->dev, -EINVAL, "%s[%d]: bad range %#x..%#x\n",
					     prop, i, g->first, g->last);
		of_property_read_string_index(np, "apple,check-ps-b4-sleep-names", i, &g->name);
	}
	s->ngroups = n;

	return 0;
}

/*
 * A named register block, shared with the owning driver: mapped, not claimed. NULL when it is
 * missing from the node or smaller than @min, the extent the driver accesses.
 */
static void __iomem *t6040_sleep_map_min(struct device *dev, const char *name, size_t min)
{
	struct device_node *np = dev->of_node;
	struct resource res;
	int index;

	index = of_property_match_string(np, "reg-names", name);
	if (index < 0 || of_address_to_resource(np, index, &res) || resource_size(&res) < min)
		return NULL;

	return t6040_ioremap_np(dev, res.start, resource_size(&res));
}

static void __iomem *t6040_sleep_map(struct device *dev, const char *name)
{
	return t6040_sleep_map_min(dev, name, 0);
}

/*
 * ANS's storage-apcie windows + PLL/lane tunables, from the apple,apcie node
 * the boot object publishes from the ADT. Our static tree has none
 * of these, so until aura fills the node @valid stays false and a real sleep
 * with ANS stopped refuses (t6040_sleep_refuse). The reg-names and tunable
 * property names match the T8140's apple,apcie shape exactly, so the same
 * loader fills both SoCs. The GP PCIe host (pcie@1cb0000000, Wi-Fi and SD) is a
 * different node, mapped by t6040_sleep_map_gp().
 */
static void t6040_sleep_map_apcie(struct t6040_sleep *s)
{
	struct t6040_apcie_st *a = &s->apcie_st;
	struct device_node *np = of_parse_phandle(s->dev->of_node, "apple,apcie", 0);

	if (!np)
		return;
	a->axi = apple_sleep_apcie_block(s->core, np, "axi2af", "apple,tunable-axi2af",
					 &a->axi_tunable);
	a->cio3pll = apple_sleep_apcie_block(s->core, np, "cio3pll", "apple,tunable-cio3pll",
					     &a->cio3pll_tunable);
	a->clkgen = apple_sleep_apcie_block(s->core, np, "clkgen", "apple,tunable-clkgen",
					    &a->clkgen_tunable);
	a->phy = apple_sleep_apcie_block(s->core, np, "phy-block", "apple,tunable-phy",
					 &a->phy_tunable);
	a->rc = apple_sleep_apcie_block(s->core, np, "rc", NULL, NULL);
	a->phy_ip = apple_sleep_apcie_block(s->core, np, "phy-ip", "apple,tunable-phy-ip-pll",
					    &a->phy_ip_pll);
	a->phy_ip_lanes = apple_sleep_apcie_tunable(s->core, np, "phy-ip",
						    "apple,tunable-phy-ip-auspma");
	of_node_put(np);

	a->valid = a->axi && a->cio3pll && a->clkgen && a->phy && a->rc && a->phy_ip &&
		   a->phy_ip_lanes;
	if (!a->valid)
		dev_info(s->dev,
			 "no storage-apcie windows/tunables (apple,apcie): a real sleep with ANS stopped refuses\n");
}

/* The GP PCIe host's port and PHY windows and its ports' PERST# pins, for the commit. */
static void t6040_sleep_map_gp(struct t6040_sleep *s)
{
	static const char *const ports[] = { "port0", "port1" };
	static const char *const phys[] = { "phy0", "phy1" };
	struct device_node *host, *child;
	struct device_node *pins = NULL;
	struct resource res;
	int p, idx;

	s->gp_perst[0] = -1;
	s->gp_perst[1] = -1;
	host = of_find_compatible_node(NULL, NULL, "apple,t6041-pcie");
	if (!host)
		return;
	/* Also with the host disabled: its port blocks hold DARTs that stay bound. */
	for (p = 0; p < ARRAY_SIZE(s->gp_port); p++) {
		idx = of_property_match_string(host, "reg-names", ports[p]);
		if (idx >= 0 && !of_address_to_resource(host, idx, &res))
			s->gp_port[p] = t6040_ioremap_np(s->dev, res.start, resource_size(&res));
		idx = of_property_match_string(host, "reg-names", phys[p]);
		if (idx >= 0 && !of_address_to_resource(host, idx, &res))
			s->gp_phy[p] = t6040_ioremap_np(s->dev, res.start, resource_size(&res));
	}
	/* Its endpoints' PERST# pins only for a host Linux runs. */
	if (!of_device_is_available(host))
		goto out;
	for_each_available_child_of_node(host, child) {
		struct of_phandle_args args;
		u32 reg[1];

		if (of_property_read_u32_array(child, "reg", reg, 1))
			continue;
		p = (reg[0] >> 11) & 0x1f;	/* the device number of pci@N,0 */
		if (p > 1 || of_parse_phandle_with_args(child, "reset-gpios", "#gpio-cells", 0,
							&args))
			continue;
		if (args.args_count >= 2 && (!pins || pins == args.np)) {
			if (!pins)
				pins = of_node_get(args.np);
			s->gp_perst[p] = args.args[0];
			s->gp_perst_low[p] = args.args[1] & 1;	/* GPIO_ACTIVE_LOW */
		}
		of_node_put(args.np);
	}
	if (pins) {
		if (!of_address_to_resource(pins, 0, &res))
			s->gp_pins = t6040_ioremap_np(s->dev, res.start, resource_size(&res));
		of_node_put(pins);
	}
	dev_dbg(s->dev, "GP PCIe: ports %d/%d phys %d/%d, PERST# pins %d/%d\n",
		!!s->gp_port[0], !!s->gp_port[1], !!s->gp_phy[0], !!s->gp_phy[1],
		s->gp_perst[0], s->gp_perst[1]);
out:
	of_node_put(host);
}


static int t6040_sleep_map_regs(struct apple_sleep *c)
{
	struct device *dev = c->dev;
	struct t6040_sleep *s;
	struct resource wres;
	int ret, i, widx;

	s = devm_kzalloc(dev, sizeof(*s), GFP_KERNEL);
	if (!s)
		return -ENOMEM;
	s->dev = dev;
	s->core = c;
	ret = t6040_sleep_parse_groups(s);
	if (ret)
		return ret;

	BUILD_BUG_ON(ARRAY_SIZE(t6040_axi2af) != T6040_AXI2AF_DOMAINS);
	BUILD_BUG_ON(ARRAY_SIZE(t6040_tvm) != ARRAY_SIZE(((struct t6040_sleep *)0)->tvm_ctl));
	BUILD_BUG_ON(ARRAY_SIZE(t6040_acc_apsc_pa) !=
		     ARRAY_SIZE(((struct t6040_sleep *)0)->acc_apsc));
	s->cpu_start = t6040_sleep_map(dev, "cpu-start");
	s->wdt = t6040_sleep_map(dev, "wdt");
	s->cluster_ctl = t6040_sleep_map(dev, "cluster");
	s->pwrgate = t6040_sleep_map(dev, "pwrgate");
	s->clkgen = t6040_sleep_map(dev, "clkgen");
	s->spmi0 = t6040_sleep_map(dev, "spmi");
	s->spmi_a0 = t6040_sleep_map(dev, "spmi-a0");
	s->mastermini = t6040_sleep_map(dev, "mastermini");
	s->soc_east = t6040_sleep_map_min(dev, "soc-east", SOC_EAST_SIZE);
	s->soc_ps = t6040_sleep_map_min(dev, "soc-ps", SOC_PS_SIZE);
	s->aop_global = t6040_sleep_map(dev, "wake");
	if (!s->cpu_start || !s->wdt || !s->cluster_ctl || !s->pwrgate || !s->clkgen ||
	    !s->spmi0 || !s->spmi_a0 || !s->mastermini || !s->soc_east || !s->soc_ps ||
	    !s->aop_global)
		return dev_err_probe(dev, -ENODEV, "a register block is missing or not mapped\n");

	widx = of_property_match_string(dev->of_node, "reg-names", "wdt");
	s->wdt_stub_ok = widx >= 0 && !of_address_to_resource(dev->of_node, widx, &wres) &&
			 wres.start == T6040_WDT_PA;
	if (!s->wdt_stub_ok)
		dev_warn(dev, "the wdt block is not the AON watchdog: no WD2 restart at the wake\n");

	s->aop_ps = t6040_ioremap_np(dev, T6040_AOP_PS_PA, 0x160);
	for (i = 0; i < ARRAY_SIZE(s->dcs); i++)
		s->dcs[i] = t6040_ioremap_np(dev, 0x220200000ULL + (i / 4) * 0x2000000ULL +
					     (i % 4) * 0x200000ULL, 0x2000);
	for (i = 0; i < ARRAY_SIZE(s->atc_cfg); i++)
		s->atc_cfg[i] = t6040_ioremap_np(dev, 0x383200000ULL + i * 0x8000000ULL, 0x100);
	for (i = 0; i < T6040_AXI2AF_DOMAINS; i++)
		s->axi2af_bridge[i] = t6040_ioremap_np(dev, t6040_axi2af[i].bridge_pa, 0x200);
	t6040_sleep_map_apcie(s);
	s->dcp_dart = t6040_ioremap_np(dev, T6040_DCP_DART_PA, 0x1000);
	s->disp_pwrgate = t6040_ioremap_np(dev, T6040_DISP_PWRGATE_PA, 0x100);
	s->pmc_dcp = t6040_ioremap_np(dev, T6040_PMC_DCP_PA, 12);
	s->pmc_bwr = t6040_ioremap_np(dev, T6040_PMC_BWR_PA, 0x10);
	s->ka_soc = t6040_ioremap_np(dev, T6040_KA_SOC_PA, KA_SIZE);
	s->ka_east = t6040_ioremap_np(dev, T6040_KA_EAST_PA, KA_SIZE);
	s->smc_wdt_deadline = t6040_ioremap_np(dev, T6040_SMC_WDT_DEADLINE_PA, 4);
	s->agx_gate = t6040_ioremap_np(dev, T6040_AGX_GATE_PA, 4);
	s->s2r_lo_count = t6040_ioremap_np(dev, T6040_S2R_LO_COUNT_PA, 4);
	for (i = 0; i < ARRAY_SIZE(t6040_acc_apsc_pa); i++)
		s->acc_apsc[i] = t6040_ioremap_np(dev, t6040_acc_apsc_pa[i], 8);
	for (i = 0; i < ARRAY_SIZE(t6040_tvm); i++) {
		s->tvm_ctl[i] = t6040_ioremap_np(dev, t6040_tvm[i].ctl, 4);
		s->tvm_status[i] = t6040_ioremap_np(dev, t6040_tvm[i].status, 4);
	}
	s->afr_ps = t6040_ioremap_np(dev, T6040_AFR_PS_PA, 4);
	t6040_sleep_map_gp(s);
	t6040_ed_map(dev);

	s->mbox = c->mbox;
	c->ps = s->soc_east;
	c->wdt = s->wdt;
	c->cpu_start = s->cpu_start;
	c->commit = s->mastermini + MASTERMINI_S2R_TIMER;
	t6040_sleep = s;

	return 0;
}

/* The VDD_CIO rail request (SMC key pmVC), written in each .begin. */
static void t6040_sleep_pmvc(struct t6040_sleep *s)
{
	struct platform_device *pdev;
	struct device_node *np;
	struct apple_smc *smc;

	np = of_find_matching_node(NULL, t6040_smc_match);
	pdev = np ? of_find_device_by_node(np) : NULL;
	of_node_put(np);
	smc = pdev ? platform_get_drvdata(pdev) : NULL;
	if (smc)
		apple_smc_request_pmvc(smc);
	else
		dev_warn(s->dev, "pmVC: no SMC to write to\n");
	if (pdev)
		put_device(&pdev->dev);
}

/* Before device suspend: the AOP islands as they are, the DCS power words to automatic mode. */
static int t6040_sleep_begin(struct apple_sleep *c)
{
	struct t6040_sleep *s = t6040_sleep;
	u32 timeouts;

	s->returned = false;
	s->gp_saved[0] = false;
	s->gp_saved[1] = false;
	t6040_aop_ps_save(s);
	t6040_sleep_pmvc(s);
	t6040_dcs_dyn_pg(s);
	timeouts = t6040_dcs_ps_auto_on(s);
	if (timeouts)
		dev_warn(s->dev, "DCS power words %#x did not read on before automatic mode\n",
			 timeouts);
	return 0;
}

/* ANS is always stopped by a real sleep, and its storage link can't be brought back untuned. */
static int t6040_sleep_check(struct apple_sleep *c)
{
	struct t6040_sleep *s = t6040_sleep;

	if (!s->apcie_st.valid)
		return dev_err_probe(s->dev, -EOPNOTSUPP,
				     "no storage-apcie windows/tunables (apple,apcie)\n");
	return 0;
}

/* The display pipes' power words down, after the device suspend and with one CPU up. */
static int t6040_sleep_display_off(struct apple_sleep *c)
{
	struct t6040_sleep *s = t6040_sleep;
	unsigned int left = t6040_disp_lower(s);

	if (left) {
		dev_warn(s->dev, "%u display power word(s) did not go off\n", left);
		t6040_disp_back(s);
		return -EBUSY;
	}
	return 0;
}

/* Nothing to stage: marks the display and the wake masks as to be undone from here on. */
static int t6040_sleep_stage(struct apple_sleep *c)
{
	return 0;
}

static void t6040_sleep_prepare_regs(struct apple_sleep *c)
{
	t6040_wake_arm(t6040_sleep);
}

/* The reverse of the entry, on a refusal and after the wake alike. */
static void t6040_sleep_undo_regs(struct apple_sleep *c)
{
	struct t6040_sleep *s = t6040_sleep;

	t6040_wake_restore(s);
	if (s->returned) {
		t6040_wake_sync();
		t6040_sio_ps_restore(s);
	}
	t6040_disp_back(s);
	if (!s->returned)
		return;
	t6040_wake_sync();
	if (s->pcie_ans_on_ret)
		dev_warn(s->dev, "wake: the storage PCIe raise failed after step %u: %d\n",
			 s->apcie_wake_step, s->pcie_ans_on_ret);
	if (s->gp_clk_timeouts)
		dev_warn(s->dev, "wake: %u GP PCIe clock or ready poll(s) timed out\n",
			 s->gp_clk_timeouts);
	if (s->aop_ps_timeouts)
		dev_warn(s->dev, "wake: %u AOP island power word(s) did not come on\n",
			 s->aop_ps_timeouts);
}

static int t6040_sleep_cpu_prepare(u64 mpidr)
{
	return 0;
}

static void t6040_sleep_cpu_restore(u64 mpidr)
{
}

/*
 * The irreversible steps from the stopped cores to the mailbox. The caller has stopped the
 * parked CPUs; the engine's mailbox word and the commit write follow it.
 */
static void t6040_sleep_commit_regs(struct apple_sleep *c)
{
	struct t6040_sleep *s = t6040_sleep;

	/*
	 * Every other core has to be off, in its last WFI, before the boot core turns the
	 * complexes off, or the sleep engine waits for them: release them now, and wait for
	 * their power words to read off further down.
	 */
	spin_table_release_stopped();
	t6040_atc_lower(s);
	t6040_gp_ports_stop(s);

	/* ANS's storage link, once ANS has been stopped by its driver; a refusal finds it up. */
	t6040_ps_raise_start(s->soc_east, PS_APCIE_SYS_ST1);
	if (s->core->pcie_ans_stopped)
		t6040_apcie_st_lower(s);
	t6040_apcie_gp_lower(s);

	/* WD0 off, then the AON WD2 net, and the SMC's AP watchdog deadline off (measured). */
	writel(0, s->wdt + WDT_WD0_CTL);
	t6040_sleep_set_wd2(s, 30);
	if (s->smc_wdt_deadline)
		writel(0, s->smc_wdt_deadline);

	/* PMU boot stage 0x50 and NOTIFY_ENABLE on both SPMI controllers (measured). */
	t6040_pmu_write(s, PMU_BOOT_STAGE, PMU_BOOT_STAGE_SLEEP);
	writel(SPMI_NOTIFY_ENABLE_SLEEP, s->spmi0 + SPMI_NOTIFY_ENABLE);
	writel(SPMI_NOTIFY_ENABLE_SLEEP, s->spmi_a0 + SPMI_NOTIFY_ENABLE);

	/* The PMGR quiesce: the complexes off, the axi2af gate read, the clock quiesce, keep-alive off. */
	t6040_secondaries_off(s);
	t6040_pmgr_quiesce(s);
	t6040_agx_gate(s);
	t6040_clusters_off(s);
	t6040_axi2af_readcheck(s);
	t6040_clkgen_park(s);
	s->ka_on_before = t6040_keep_alive(s, false, 0);
}

/* Armed mailbox, last step before the commit word. */
static void t6040_sleep_pre_commit(struct apple_sleep *c)
{
	struct t6040_sleep *s = t6040_sleep;

	t6040_fabrics_on(s);
	t6040_sio_ps_save(s);
}

/* Back on the boot core with the AIC restored. */
static void t6040_sleep_wake(struct apple_sleep *c)
{
	struct t6040_sleep *s = t6040_sleep;
	u32 val;
	int i, ret;

	t6040_wake_sync();

	/*
	 * The complexes back: E, P0 and P1 to 0xf, the two display complexes to the nibble the
	 * commit found, with no poll. The secondaries come up through the normal per-CPU start.
	 */
	for (i = 0; i < CLUSTER_CTL_NR; i++) {
		u32 on = i < CLUSTER_CTL_FIRST_WAIT ? CLUSTER_CTL_ON :
			 s->cluster_saved[i] & CLUSTER_CTL_NIBBLE;

		val = readl(s->cluster_ctl + CLUSTER_CTL(i));
		writel((val & ~CLUSTER_CTL_NIBBLE) | on, s->cluster_ctl + CLUSTER_CTL(i));
	}
	t6040_wake_sync();

	/* Wake pwrgates and CPM voters: read, set BIT31. */
	writel(readl(s->pwrgate + PWRGATE_WAKE_0) | PWRGATE_ON, s->pwrgate + PWRGATE_WAKE_0);
	writel(readl(s->pwrgate + PWRGATE_WAKE_1) | PWRGATE_ON, s->pwrgate + PWRGATE_WAKE_1);
	t6040_wake_sync();

	/* SOC-East retention off: clear DEV_DISABLE, then poll BUSY clear. */
	val = readl(s->soc_east + SOC_EAST_RETENTION);
	writel(val & ~PS_DEV_DISABLE, s->soc_east + SOC_EAST_RETENTION);
	ret = readl_poll_timeout_atomic(s->soc_east + SOC_EAST_RETENTION, val,
					!(val & PS_BUSY), 1, SOC_EAST_TIMEOUT_US);
	if (ret)
		dev_warn(s->dev, "SOC-East retention word still busy on wake (%#x)\n", val);
	t6040_wake_sync();

	/* The fabric-bridge retention re-arm, or the bridges drop their traffic. */
	t6040_axi2af_rearm(s);
	t6040_wake_sync();

	/* ANS's storage link, after the bridges and before nvme's resume_early. */
	s->apcie_wake_step = 0;
	s->pcie_ans_on_ret = 0;
	if (s->core->pcie_ans_stopped)
		s->pcie_ans_on_ret = t6040_apcie_st_raise(s);
	t6040_gp_pcie_clk(s);
	t6040_wake_sync();
	t6040_aop_ps_restore(s);
	t6040_wake_sync();

	/* Every AP pin but the skipped ones reset in the sleep: all put back */
	c->pins_lost = c->pins_saved_valid;
	apple_sleep_restore_pins();

	writel(readl(s->pwrgate + PWRGATE_CPM_0) | PWRGATE_ON, s->pwrgate + PWRGATE_CPM_0);
	writel(readl(s->pwrgate + PWRGATE_CPM_1) | PWRGATE_ON, s->pwrgate + PWRGATE_CPM_1);
	writel(readl(s->pwrgate + PWRGATE_CPM_2) | PWRGATE_ON, s->pwrgate + PWRGATE_CPM_2);
	t6040_wake_sync();

	/* Undo the entry clk-mux park. */
	t6040_clkgen_restore(s);
	t6040_wake_sync();

	/* Software ack of the armed S2R request (the S2R timer has no stop). */
	writel(0, s->aop_global + AOP_S2R_CTRL);
	t6040_wake_sync();

	/* The keep-alive bits the commit cleared, back. */
	if (s->ka_on_before)
		t6040_keep_alive(s, true, s->ka_on_before);
	t6040_wake_sync();

	/* A longer WD2 over the device resume, until .end. */
	t6040_sleep_set_wd2(s, WDT_SUSPEND_SECS);
	t6040_wake_sync();

	for (i = 0; i < AOP_WAKE_WORDS; i++)
		s->wake_status[i] = readl(s->aop_global + AOP_WAKE_STATUS(i));
	s->returned = true;
}

/* The CPU-start bits of the clusters E, P0 and P1: four, six and six slots, in cpu-id order. */
static const u8 t6040_cpu_start_first_bit[] = { 0, 4, 10 };

/* The slots of each cluster: E has four, a P cluster six (the ADT enables five of them on a J616s). */
static bool t6040_sleep_cpu_valid(u64 mpidr)
{
	static const u8 slots[] = { 4, 6, 6 };
	unsigned int cluster = MPIDR_AFFINITY_LEVEL(mpidr, 1), core = MPIDR_AFFINITY_LEVEL(mpidr, 0);

	return cluster < ARRAY_SIZE(slots) && core < slots[cluster] &&
	       MPIDR_AFFINITY_LEVEL(mpidr, 2) == !!cluster;
}

static const struct apple_sleep_ops t6040_sleep_ops = {
	.deep_wfi_arm = t6040_deep_wfi_arm,
	.resume = t6040_resume_stub,
	.aic_mask_all = aic_sleep_mask_all,
	.aic_ack = aic_sleep_ack,
	.aic_restore = aic_sleep_restore,
	.map_regs = t6040_sleep_map_regs,
	.begin = t6040_sleep_begin,
	.check = t6040_sleep_check,
	.display_off = t6040_sleep_display_off,
	.stage = t6040_sleep_stage,
	.prepare_regs = t6040_sleep_prepare_regs,
	.undo_regs = t6040_sleep_undo_regs,
	.commit_regs = t6040_sleep_commit_regs,
	.pre_commit = t6040_sleep_pre_commit,
	.wake = t6040_sleep_wake,
	.cpu_prepare = t6040_sleep_cpu_prepare,
	.cpu_restore = t6040_sleep_cpu_restore,
};

static const struct apple_sleep_soc t6040_sleep_soc = {
	.ps = {
		.ans = PS_ANS,
		.apcie_gp = PS_APCIE_GP,
		.apcie_sys_gp = PS_APCIE_SYS_GP,
		.apcie_st = PS_APCIE_ST0,
		.apcie_sys_st = PS_APCIE_SYS_ST0,
		.apcie_phy_sw = PS_APCIE_PHY_SW,
	},
	.early_context = true,
	.own_wake = true,
	.ops = &t6040_sleep_ops,
	.machine = "apple,j616s",
	.boot_mpidr = 0x10100,
	.cpu_count = 14,
	.cpu_cluster_affinity = 1,
	.cpu_clusters = 3,
	.cpu_cores_per_cluster = 6,
	.cpu_start_first_bit = t6040_cpu_start_first_bit,
	.cpu_valid = t6040_sleep_cpu_valid,
};
