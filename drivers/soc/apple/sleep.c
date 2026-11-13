// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * Apple SoC system sleep
 *
 * On some Apple SoCs a kernel can run directly at EL2 behind the CTXR execute
 * window (Documentation/arch/arm64/ctxr-execmem.rst), with no monitor below it.
 * So no PSCI answers a request to power a CPU off or to suspend the system, and
 * the kernel does both itself.
 *
 * iBoot locks every core's reset vector to the boot object. A core that comes
 * back from a power-off therefore restarts inside the boot object (aura), and
 * that has to send it back here. The boot object opts in by publishing a mailbox
 * in /chosen/aurora,s2r-mailbox, which also gives its spin-table CPUs the parking
 * spin table (arch/arm64/kernel/cpu_ops.c). Without it this driver does nothing:
 *
 *  - A secondary core goes back through its spin-table release address, which
 *    the boot object keeps, into secondary_holding_pen.
 *  - The boot core takes cpu_resume from the mailbox: EL2, MMU off.
 *
 * The register addresses are in the device tree. The ADT has every one of them,
 * as /arm-io/pmgr's or /arm-io/wdt's reg entries. Register offsets and order not
 * otherwise explained here were measured from a macOS sleep under aura.
 *
 * What the kernel does in suspend-to-RAM is what macOS does in its last 14 ms,
 * with the order kept. What differs between SoCs, their power words and the
 * lists macOS was measured with, is each SoC's match data. Only SoCs measured
 * and tested that way are matched: on the others this driver doesn't bind, and
 * s2idle stays the only sleep.
 */

#include <linux/bitfield.h>
#include <linux/bitmap.h>
#include <linux/bits.h>
#include <linux/cacheflush.h>
#include <linux/percpu.h>
#include <asm/kvm_arm.h>
#include <linux/math64.h>
#include <linux/unaligned.h>
#include <linux/soc/apple/pmp-report.h>
#include <linux/cpu.h>
#include <linux/cpumask.h>
#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/irqchip/irq-apple-aic.h>
#include <linux/ktime.h>
#include <linux/moduleparam.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/panic.h>
#include <linux/pm_domain.h>
#include <linux/smp.h>
#include <linux/soc/apple/dart.h>
#include <linux/soc/apple/sleep.h>
#include <linux/soc/apple/dcp-v27-sleep.h>
#include <linux/soc/apple/sleep-ans.h>
#include <linux/soc/apple/tunable.h>
#include <linux/spinlock.h>
#include <linux/suspend.h>
#include <linux/workqueue.h>
#include <clocksource/arm_arch_timer.h>
#include <asm/apple-idle.h>
#include <dt-bindings/gpio/gpio.h>
#include <asm/arch_timer.h>
#include <asm/barrier.h>
#include <asm/cpu_ops.h>
#include <asm/cputype.h>
#include <asm/memory.h>
#include <asm/smp_plat.h>
#include <asm/suspend.h>
#include <asm/sysreg.h>
#include <asm/virt.h>

/* Must match aura's struct aura_s2r_mailbox. */
#define AURA_S2R_MAGIC		0x3152325341525541ULL	/* "AURAS2R1" */

struct aura_s2r_mailbox {
	u64 magic;		/* aura, before the kernel starts */
	u64 boot_mpidr;		/* aura: the only core that may take resume_pa */
	u64 resume_pa;		/* kernel, armed before a commit; aura clears it */
	u64 resumes;		/* aura: resume_pa taken this many times */
};

/* PS word */
#define PS_AUTO_ENABLE		BIT(28)
#define PS_UNLATCH		BIT(12)
#define PS_DEV_DISABLE		BIT(10)
#define PS_FLAGS		GENMASK(9, 8)
#define PS_ACTUAL		GENMASK(7, 4)
#define PS_TARGET		GENMASK(3, 0)
#define PS_OFF_TIMEOUT_US	5000
#define PS_BUSY_SETTLE_US	1000

/* CPU start block, as aura's smp_start_cpu() uses it */
#define CPU_START_STOP		0x0
#define CPU_START_SYS		0x4
#define CPU_START_CLUSTER(n)	(0x8 + (n) * 4)

/* Power-gate page: reads back this value already. Purpose unknown. */
#define PWRGATE_ON		0x80000000

/* Three clock muxes macOS writes before the commit. */
#define CLK_MUX_PARKED		0x80100000
#define CLK_MUX_34		0x0
#define CLK_MUX_38		0x4
#define CLK_MUX_3C		0x8

/* Cluster control: only these three words, the next one is locked. */
#define CLUSTER_E		0x0
#define CLUSTER_P		0x4
#define CLUSTER_AVE		0x8
#define CLUSTER_ON		0xf
/* AVE: a transition still in progress, as when a lower waits for a domain it powers */
#define CLUSTER_PENDING		BIT(4)
/* Set when a complex's up sequence ends, and cleared by writing 1. */
#define CLUSTER_UP_DONE		BIT(5)
#define CLUSTER_E_READY		BIT(5)
#define CLUSTER_TIMEOUT_US	5000

/* CLLT: names from nothing. Bits measured cleared by macOS before the commit. */
#define CLLT_0			0x30
#define CLLT_1			0x34
#define CLLT_2			0x3c
#define CLLT_0_CLEAR		GENMASK(7, 0)
#define CLLT_1_CLEAR		GENMASK(7, 0)
#define CLLT_2_CLEAR		GENMASK(17, 16)

/* Watchdog block, ADT /arm-io/wdt reg[0]. WD2 is the sleep safety net. */
#define WDT_WD0_COUNT		0x00
#define WDT_WD0_CTL		0x0c
#define WDT_WD1_COUNT		0x10
#define WDT_WD2_COUNT		0x20
#define WDT_WD2_ALARM		0x28
#define WDT_WD2_CTL		0x2c
#define WDT_TICKS_PER_S		24000000
/* The sleep's own safety net: ADT /arm-io/wdt simple-reconfig-wdog-icc-time. */
#define WDT_SLEEP_SECS		5
/* Device suspend and resume can take a while, and nothing else recovers a hang in them. */
#define WDT_SUSPEND_SECS	120

#define AON_AP_DEADLINE		0x08

/* Wake sources: a mask, so 0 enables. Status words are sticky. */
#define WAKE_MASK(word)		(0x08 + (word) * 4)
#define WAKE_STATUS(word)	(0x28 + (word) * 4)
#define WAKE_WORDS		8
#define WAKE_SOURCES_MAX	32

/* The AP queue of the PMU's SPMI controller, and its response vector's enable and ack */
#define SPMI_STATUS		0x00
#define SPMI_STATUS_RX_EMPTY	BIT(24)
#define SPMI_CMD		0x04
#define SPMI_REPLY		0x08
#define SPMI_NOTIFY_ENABLE	0x40
#define SPMI_NOTIFY_ENABLE_SLEEP	0x2000001
#define SPMI_NOTIFY_REPLY	BIT(0)
#define SPMI_NOTIFY_ACK		0x80
#define SPMI_CMD_ACTIVE		BIT(15)
#define SPMI_OP_EXT_WRITE(n)	((n) - 1)
#define SPMI_OP_EXT_READ(n)	(0x20 | ((n) - 1))
#define SPMI_OP_ZERO_WRITE	0x80
#define SPMI_OP_SLEEP		0x11
#define SPMI_OP_WAKEUP		0x13
#define SPMI_POLL_US		20000

/* A port controller on SPMI: registers go through a page select, then a data window. */
#define HPM_REG_SELECT		0x00
#define HPM_REG_DATA		0x20
#define HPM_REG_DATA_CMD	0xa0
#define HPM_PAGE_CMD1		0x08
#define HPM_PAGE_DATA1		0x09
#define HPM_CMD_SSPS		0x53505353
#define HPM_CMD_COLD_RESET	0x64696147
#define HPM_SSPS_S3		3
/* The first look after the reset, and how long a contract that never seemed to go is trusted to have gone. */
#define HPM_RESET_FIRST_MS	50
#define HPM_RESET_FLOOR_MS	1000
#define HPM_PAGE_POWER_STATUS	0x3f
#define HPM_POWER_CONNECTED	BIT(0)
#define HPM_POWER_TYPEC_MODE	GENMASK(3, 2)
#define HPM_POWER_TYPEC_PD	3
/* A contract that comes back does by 0.7 s; one that doesn't is back after another reset. */
#define HPM_CONTRACT_MS		1500
#define HPM_RESET_TRIES		3

/* A PMU register write on the sleep's SPMI bus */
#define SPMI_CMD_WRITE		(BIT(15) | (0xe << 8) | 0x30)
#define PMU_BOOT_STAGE		0xf701
#define PMU_BOOT_STAGE_SLEEP	0x50

/* Three of the memory controller's channels, and what macOS steps down on each. */
#define MCC_CHANNELS		3
#define MCC_ENABLES		0x4
#define MCC_LANES		0x8
#define MCC_LANES_MASK		0xc
#define MCC_LANE_STROBE(n)	BIT(16 + 4 * (n))
#define MCC_LANE(n)		BIT(4 * (n))

/* Two words macOS zeroes just before the power gates. */
#define PTD_A			0x0
#define PTD_B			0x8

/* Perf-state words, each like DCS's. The SoC's is first. */
#define CLK_PERF_SOC		0x0
#define SOC_SLEEP_STATE		3

/* The PMC's voter agents, each with a word holding its floor for every rail. */
#define PMC_VOTER_AGENTS	4
#define PMC_VOTER_STRIDE	0x4000

/* DCS: the low nibble asks for a state, and [11:8] is the state it has reached. */
#define DCS_STATE		GENMASK(11, 8)
#define DCS_BUSY		BIT(31)
#define DCS_SLEEP_STATE		4
#define DCS_STEP_STATE		6
#define DCS_TIMEOUT_US		5000

/*
 * Word 0 of the wake block. Bit 0 lets the sleep keep DRAM for the USB host's always-on side.
 * macOS sets it from the host's start to its stop, not at the commit.
 */
#define WAKE_SLP_DDR		0x00
#define WAKE_SLP_DDR_USB	BIT(0)

/*
 * AMCC error enables. Any error enabled and pending at the commit turns the
 * sleep around within 20 ms with no error handler to service it. Linux has none.
 * Measured: with macOS's handler hidden, as Linux's position is, 6 of 6 sleeps
 * held with both of these off, and bit 25 alone still bounced. Their status
 * stays set; only the enables matter. A deep sleep sets them again, so this has
 * to be done before every commit.
 */
#define AMCC_WORD0_ENABLE	0x0
#define AMCC_WORD1_ENABLE	0x8
#define AMCC_WORD0_DSID_AGE_OUT	BIT(25)
#define AMCC_WORD1_UNEXP_RT_HIT	BIT(8)

/* A coprocessor's ASC */
#define ASC_CPU_CONTROL		0x44
#define ASC_CPU_CONTROL_RUN	BIT(4)
#define ASC_CPU_STATUS		0x48

/* The part of the PCIe block that ANS shares, whether or not its host is bound. */
#define APCIE_AXI_EN0		0x104
#define APCIE_AXI_EN1		0x108
#define APCIE_CLK_CTRL		0x0
#define APCIE_CLK_BUSY		BIT(31)
#define APCIE_CLK_RUN		BIT(5)
#define APCIE_CLK_OFF		BIT(1)
#define APCIE_CLK_OFF2		BIT(2)
#define APCIE_CLK_START		0x4
#define APCIE_CLK_START_GO	BIT(31)
#define APCIE_PHY_COMMON	0x4000
#define APCIE_PHY_CTRL		0x8000
#define APCIE_PHY_CTRL_CLK0REQ	BIT(0)
#define APCIE_PHY_CTRL_CLK1REQ	BIT(1)
#define APCIE_PHY_CTRL_CLK0ACK	BIT(2)
#define APCIE_PHY_CTRL_CLK1ACK	BIT(3)
#define APCIE_PHY_CTRL_RESET	BIT(4)
#define APCIE_PHY_CTRL_UP	BIT(27)
#define APCIE_PHY_RUN		0x8004
#define APCIE_PHY_RUN_EN	BIT(0)
#define APCIE_PHY_RUN_EN2	BIT(4)
#define APCIE_PHY_READY		0x8008
#define APCIE_PHY_IP_PLL_CTL	0x90
#define APCIE_PHY_IP_PLL_HOLD	BIT(16)
#define APCIE_PORT_INTSTAT	0x100
#define APCIE_PORT_INTMSK	0x104
#define APCIE_PORT_INT_LINK	GENMASK(7, 4)
#define APCIE_PORT_L2		0x88
#define APCIE_PORT_L2_ENTER	0x11
#define APCIE_PORT_L2_BUSY	BIT(0)
#define APCIE_PORT_PERST_GUARD	0x13c
#define APCIE_PORT_LINKSTS	0x208
#define APCIE_PORT_LINKSTS_UP	BIT(0)
#define APCIE_PORT_APPCLK	0x800
#define APCIE_PORT_APPCLK_EN	BIT(0)
#define APCIE_PORT_APPCLK_CGDIS	BIT(8)
#define APCIE_PORT_PERST	0x82c
#define APCIE_PORT_PHY_LINK	BIT(31)
#define APCIE_PORT_PHY_EN0	BIT(9)
#define APCIE_PORT_PHY_EN1	BIT(10)
#define APCIE_ENGINE_CTRL	0x0
#define APCIE_ENGINE_IRQ_MASK	0x8
#define APCIE_ENGINE_STOP	BIT(0)
#define APCIE_FABRIC_STOP	0x4
#define APCIE_PORT_STOP_US	5000
#define APCIE_RC_ZERO		0x04
#define APCIE_RC_READY_START	0x50
#define APCIE_RC_READY_CFG	0x54
#define APCIE_RC_READY_STAT	0x58
#define APCIE_POLL_US		50000

/* The coprocessors the last mile stops. Each driver idles its own first. */
#define SLEEP_COPROCS		16

static struct apple_sleep_coproc {
	struct device *dev;
	void __iomem *asc;
	bool pcie_ans;
	int (*last_check)(void *data);
	int (*last_stop)(void *data);
	void *data;
	bool ready;
	bool stopped;
	u32 status;
	int stop_ret;
	/* No ASC: its last stop takes it whole, power word @ps included, before the others. */
	bool first;
	/* Between ANS and the others, whatever order they registered in. */
	bool early;
	u32 ps;
	struct device_node *domain;
} apple_sleep_coprocs[SLEEP_COPROCS];
static DEFINE_MUTEX(apple_sleep_coprocs_lock);

/* Irreversible enclave stop; nonzero is legal only if SLEEP was not sent. */
static struct {
	struct device *dev;
	int (*stop)(void *data);
	void *data;
} apple_sleep_enclave;

/*
 * Written with bits 0 and 63 by every core right before its last WFI, measured
 * from macOS. This is not CYC_OVRD, which is undefined on this SoC.
 */
#define SYS_APL_DEEP_SLEEP	sys_reg(3, 5, 15, 6, 2)
#define DEEP_SLEEP_ENABLE	(BIT_ULL(0) | BIT_ULL(63))

#define SYS_APL_IPI_SR_EL1	sys_reg(3, 5, 15, 1, 1)
#define IPI_SR_PENDING		BIT(0)
#define SYS_APL_SIQ_CFG_EL1	sys_reg(3, 4, 15, 10, 4)

#define SLEEP_MAX_DISPLAYS	5

/* Allocation ceiling; active geometry and DVFM names come from the SoC. */
#define SLEEP_MAX_CLUSTERS	3

/* cpm-dvfm */
#define DVFM_CTL		0x178
#define DVFM_WORD_0		0x140
#define DVFM_WORD_1		0x160
#define DVFM_WORD_2		0x190
#define DVFM_WORD_3		0x198
#define DVFM_ENABLE		BIT_ULL(63)

/* In macOS's order, on the way down. */
static const u32 sleep_dvfm_words[] = {
	DVFM_WORD_0, DVFM_WORD_1, DVFM_WORD_2, DVFM_WORD_3,
};

/* A power word by its offset, named for the log */
struct apple_sleep_word {
	const char *name;
	u32 offset;
};

/* Power words by their offsets */
struct apple_sleep_words {
	const u32 *offset;
	unsigned int count;
};

#define SLEEP_WORDS(a)		{ .offset = (a), .count = ARRAY_SIZE(a) }

/*
 * A coprocessor that must be bound for a sleep, by its path. @must_stop: it must also have
 * stopped by the last step. @ps: its CPU's own domain, where a driver may power it off.
 */
struct apple_sleep_gate_coproc {
	const char *path;
	bool must_stop;
	u32 ps;
	/* Only with a proven-readable ASC RUN control, never an arbitrary status. */
	bool allow_unstarted;
	bool strict_unbound;
	const char *compatible;
	u64 match_start;
	int run_index;
	bool explicit_run_index;
};

/* A source in the ADT's pmgr/ap-wake-sources: for the log, and to tell the SMC's apart */
struct apple_sleep_wake_name {
	u32 id;
	const char *name;
};

/* The power words in "ps" that the sleep drives itself */
struct apple_sleep_domains {
	/* CPU, front end and system, of the DCP and then of the external DCP */
	u32 disp[SLEEP_MAX_DISPLAYS][3];
	u32 sio_cpu;
	u32 sep;
	/* The USB host's complex: always-on, common and the controller */
	u32 usb_aon;
	u32 usb_common;
	u32 usb;
	/* Only the deep path's wake turns it on. */
	u32 ap_tmm;
	u32 gpio;
	u32 ans;
	/* ANS's PCIe block */
	u32 apcie_gp;
	u32 apcie_sys_gp;
	u32 apcie_st;
	u32 apcie_sys_st;
	u32 apcie_phy_sw;
};

/*
 * What differs between SoCs. Power words are offsets in "ps", or in "ps-aon" where named
 * so, and the lists keep the order macOS was measured with.
 */
struct apple_sleep_region_state;
struct apple_sleep_region_desc {
	const char *name;
	u32 size;
	bool power;
};

struct apple_sleep_region_table {
	u32 region, stride, count;
};

struct apple_sleep_region_profile {
	const struct apple_sleep_region_desc *region;
	unsigned int count;
	struct apple_sleep_words power, replay, entry_clk, entry_perf;
	const struct apple_sleep_region_table *table;
	unsigned int ntables;
};

struct apple_sleep;

static int sleep_v27_scope_begin(void);
static void sleep_v27_scope_end(void);
static int sleep_v27_final_check(unsigned int i);
static bool sleep_v27_gate_prepared(const struct device_node *np);
static int sleep_v27_final_stop(struct apple_sleep *s);

struct apple_sleep_soc_ops {
	int (*map_regs)(struct apple_sleep *s);
	int (*begin)(struct apple_sleep *s);
	int (*stage)(struct apple_sleep *s);
	int (*final_check)(struct apple_sleep *s);
	/* After final peer stops: errors are fatal, never a veto. */
	int (*commit_notice)(struct apple_sleep *s);
	void (*finish)(struct apple_sleep *s);
	int (*cpu_prepare)(u64 mpidr);
	void (*cpu_restore)(u64 mpidr);
	/* A core's runtime power-down WFI, after its stop bit (never returns) */
	void (*idle_wfi)(void);
	/* Optional: a stopped core's own last steps and WFI at the commit (never returns) */
	void (*cpu_down)(struct apple_sleep *s, u64 mpidr);
	/* Optional: past the point of no return, between ANS's stop and SIO's */
	void (*after_ans)(struct apple_sleep *s);

	/* Pure bounded planning only; no register read/write or entry authorization. */
	int (*plan_complex_request)(u32 observed, u32 *request);
	/* Reversible register preparation, after all refusal checks. */
	void (*prepare_regs)(struct apple_sleep *s);
	void (*undo_regs)(struct apple_sleep *s);
	int (*display_off)(struct apple_sleep *s);
	int (*display_stop)(struct apple_sleep *s);
	/* Once, with the other CPUs held: the boot cluster's request cycle, as macOS's runtime */
	void (*boot_cluster_cycle)(struct apple_sleep *s);
	/* After the always-on GPU stop, right before SEP's sleep request. */
	void (*before_enclave)(struct apple_sleep *s);
	/* After cpu_suspend saved context, before any stopped core loses power. */
	void (*pre_commit)(struct apple_sleep *s);
	int (*check)(struct apple_sleep *s);
	/* Irreversible SoC registers; no protocol/lifecycle duplication here. */
	void (*commit_regs)(struct apple_sleep *s);
	/* Optional traffic strictly after timer, before AIC/release/DEEP. */
	void (*post_timer)(struct apple_sleep *s);
	void (*wake_regs)(struct apple_sleep *s);
};

struct apple_sleep_soc {
	const struct apple_sleep_soc_ops *ops;
	const struct apple_sleep_region_profile *regions;
	const struct apple_sleep_smc_policy *smc_policy;
	bool early_context;
	bool own_wake_masks;
	bool strict_gate_inventory;

	/* MPIDR cluster affinity and the CPU-start block's per-cluster bit bank. */
	unsigned int cpu_cluster_affinity;
	unsigned int cpu_clusters;
	unsigned int cpu_cores_per_cluster;
	unsigned int cpu_start_stride;
	bool (*cpu_valid)(u64 mpidr);
	const char *machine;
	u64 boot_mpidr;
	unsigned int cpu_count;
	unsigned int display_count;
	const char *dvfm_names[SLEEP_MAX_CLUSTERS];
	struct apple_sleep_domains ps;
	/* In "ps-aon": held up by a live debug link, and off on macOS */
	u32 ps_aon_debug_switch;
	/* In "pwrgate": each pair written as macOS does */
	u32 pwrgate_wake[2];
	u32 pwrgate_cpm[2];
	u32 pwrgate_disp[2];
	/* Every power word, saved at the commit and checked at the wake */
	struct apple_sleep_words ps_all;
	struct apple_sleep_words ps_aon_all;
	/* On at macOS's commit and left off by Linux */
	struct apple_sleep_words ps_macos_on;
	/* Off at macOS's commit, in the order it lowers them */
	struct apple_sleep_words ps_macos_off;
	struct apple_sleep_words ps_aon_macos_off;
	/* In "ps-aon": on at macOS's commit, with AUTO_ENABLE off */
	struct apple_sleep_words ps_aon_macos_no_auto;
	/* Written back on by macOS's wake before any driver runs, in its order */
	struct apple_sleep_words ps_wake_order;
	struct apple_sleep_words ps_aon_wake_order;
	/* Polled off just before the complex words */
	struct apple_sleep_words ps_still_on;
	/* The wake's timed power-gate polls: off with BUSY clear at the commit, on macOS */
	struct apple_sleep_words ps_engine_polled;
	/* Lowered at the last step if still on, with no user left */
	const struct apple_sleep_word *stray;
	unsigned int nstray;
	/* Off on macOS by the last step: one still on only warns. */
	const struct apple_sleep_word *gate_off;
	unsigned int ngate_off;
	const struct apple_sleep_gate_coproc *gate_coprocs;
	unsigned int ngate_coprocs;
	const struct apple_sleep_wake_name *wake_names;
	unsigned int nwake_names;
	/* The sources in the wake block's last word */
	u32 wake_last_word;
	/* The PCIe host whose port the radio is on, for its PERST#, or NULL */
	const char *radio_pcie;
	/* The encoders' system power words: their domains need the AVE complex up */
	const u32 *ave_domains;
	unsigned int nave_domains;
	/* The PMC whose cluster vote for the encoders needs the complex up, and its bit */
	const char *ave_pmc;
	u32 ave_pmc_cluster;
};

struct apple_sleep {
	struct device *dev;
	const struct apple_sleep_soc *soc;
	struct apple_sleep_region_state *region;
	void __iomem *ps;
	void __iomem *pwrgate;
	void __iomem *cpu_start;
	void __iomem *cllt;
	void __iomem *cluster;
	void __iomem *commit;
	void __iomem *wdt;
	void __iomem *aon_misc;
	void __iomem *wake;
	void __iomem *amcc[4];
	void __iomem *pmc_dcp;
	void __iomem *spmi;
	void __iomem *pmc_voters;
	void __iomem *dcs;
	void __iomem *ps_aon;
	void __iomem *spmi_a0;
	void __iomem *hpm_spmi;
	void __iomem *wdt_aux, *disp_gate, *pmc_rails;
	/* The port controller whose cold reset ends the debug link. */
	struct device_node *hpm_np;
	u64 siq_cfg;
	u32 hpm_sid;
	int usb_down_ret;
	/* Cut, or found already down, for this sleep. */
	bool usb_down_ran;
	/* The PMGR block "ps" maps, for the power controllers' nodes. */
	struct resource ps_res;
	/* Its words a driver took down with apple_sleep_domain_set() and hasn't raised. */
	unsigned long *driver_down;
	void __iomem *clk_mux;
	void __iomem *mcc[MCC_CHANNELS];
	void __iomem *ptd;
	void __iomem *clk_perf;
	void __iomem *cluster_dcs;
	/* As the match found them: one for each word of the SoC's lists */
	u32 *ps_on_saved;
	u32 *ps_off_saved;
	u32 *ps_aon_saved;
	u32 *ps_aon_auto_saved;
	/* For each word of ps_all and ps_aon_all: at the commit, as the wake found it, late */
	u32 *ps_commit;
	u32 *ps_aon_commit;
	u32 *ps_woke;
	u32 *ps_aon_woke;
	bool *ps_late;
	bool *ps_aon_late;
	/* The rewake's own books */
	bool *ps_done;
	bool *ps_aon_done;
	u32 spmi_saved[2];
	bool matched;
	void __iomem *dvfm[SLEEP_MAX_CLUSTERS];
	struct aura_s2r_mailbox *mbox;

	u32 wake_enable[WAKE_SOURCES_MAX];
	int wake_enable_count;
	u32 wake_saved[WAKE_WORDS];
	/* The sources this sleep left enabled. */
	u32 wake_armed[WAKE_WORDS];

	struct {
		struct device_node *np;
		void __iomem *asc;
		bool bound;
		bool quiesced;
	} display[SLEEP_MAX_DISPLAYS];

	/* Filled in for the report after a return. */
	u32 wake_status[WAKE_WORDS];
	u64 dvfm_ctl[SLEEP_MAX_CLUSTERS];
	u64 dvfm_saved[SLEEP_MAX_CLUSTERS][ARRAY_SIZE(sleep_dvfm_words)];
	bool dvfm_parked;
	u64 resumes_before;
	u64 resumes_after;
	int cluster_e_ret;
	int cluster_ave_ret;
	/* The AVE complex, as the parent domain of the encoders' enabled domains */
	struct generic_pm_domain ave_pd;
	bool ave_pd_owned;
	/* Which of the SoC's encoder domains are its subdomains, one bit each */
	unsigned long ave_linked;
	/* Its subdomains: the encoders' system domains, then the PMC's cluster vote */
	struct device_node *ave_children[4];
	unsigned int ave_nchildren;
	int cluster_ave_commit_ret;
	unsigned int ps_rewoken;
	unsigned int ps_rewake_timeouts;
	u64 cntkctl;
	int cpus_stop_ret;
	int ans_off_ret;
	/* Per-attempt M5 ANS receipt; survives platform .wake reporting. */
	int ans_m5_wake_error;
	int pcie_ans_on_ret;
	void __iomem *apcie_axi;
	void __iomem *apcie_cio3pll;
	void __iomem *apcie_clkgen;
	void __iomem *apcie_phy;
	void __iomem *apcie_rc;
	bool apcie_port_stopped;
	/* A real sleep left the radio's port down, until its host sets it up again */
	bool apcie_port_down;
	void __iomem *apcie_port;
	void __iomem *apcie_port_phy;
	void __iomem *apcie_engine;
	void __iomem *apcie_fabric;
	struct apple_tunable *apcie_axi_tunable;
	struct apple_tunable *apcie_cio3pll_tunable;
	struct apple_tunable *apcie_clkgen_tunable;
	struct apple_tunable *apcie_phy_tunable;
	void __iomem *apcie_phy_ip;
	struct apple_tunable *apcie_phy_ip_pll;
	struct apple_tunable *apcie_phy_ip_lanes;
	bool pcie_ans_stopped;
	/* The words apple_sleep_lower_stray() lowered, one bit each */
	unsigned long stray_lowered;
	/* What must be bound for a sleep, and what must have stopped by the last step */
	struct apple_sleep_gate {
		const char *name;
		/* Compared only, never dereferenced: nodes outlive the gate. */
		const struct device_node *np;
		void __iomem *asc;
		bool bound;
		bool must_stop;
		bool sep;
		u32 ps;
		bool allow_unstarted;
		bool strict_unbound;
	} gate[16];
	unsigned int gate_count;
	/* The AP pins, which a deep sleep resets */
	void __iomem *pins;
	u32 *pins_saved;
	unsigned long *pins_skip;
	unsigned int npins;
	unsigned int pin_irq_groups;
	bool pins_saved_valid;
	bool pins_lost;
	/* The radio port's PERST# among them, or -1 */
	int radio_perst;
	bool radio_perst_low;
	bool returned;
	/* This wake came back down the deep path, not the DRAM-kept one. */
	bool deep_ran;
	bool regs_staged, wake_masks_saved, entry_prepared, entry_committed;
	u64 smc_idle_sent, smc_idle_ack, timer_at, committed_at;
	struct {
		struct device *dev;
		const struct apple_sleep_smc_saved_ops *saved_ops;
		bool held;
		int (*late_notice)(void *data, u64 *sent, u64 *ack);
		void *data;
	} smc;
	/* This sleep's wake came from the SMC's sources alone. */
	bool smc_woke_alone;
	/* A power word the DRAM-kept wake polls stayed latched: this sleep went deep. */
	bool deep_once;
	/* Each latched word the commit cleared, as it found it */
	struct {
		u32 offset;
		u32 val;
	} ps_unlatched[48];
	unsigned int ps_unlatched_count;
};

static struct apple_sleep *apple_sleep;
/* Publication/removal only; never acquired in the saved-context callback. */
static DEFINE_MUTEX(apple_sleep_smc_lock);
static int apple_sleep_profile_check(struct apple_sleep *s);
static void __iomem *sleep_region_reg(struct apple_sleep *s, u32 word);
static int sleep_ps_offset(struct apple_sleep *s, struct device_node *pd);
static int sleep_region_domain_offset(struct apple_sleep *s, struct device_node *pd);
static unsigned long *sleep_region_domain_bitmap(struct apple_sleep *s, u32 word, unsigned int *bit);
static void __iomem *sleep_power_reg(struct apple_sleep *s, u32 word)
{
	return s->soc->regions ? sleep_region_reg(s, word) : s->ps + word;
}

static unsigned long *sleep_domain_bitmap(struct apple_sleep *s, u32 word, unsigned int *bit)
{
	if (s->soc->regions)
		return sleep_region_domain_bitmap(s, word, bit);
	*bit = word / 4;
	return s->driver_down;
}


static struct apple_sleep_coproc *apple_sleep_coproc_find(struct device *dev)
{
	int i;

	for (i = 0; i < SLEEP_COPROCS; i++)
		if (apple_sleep_coprocs[i].dev == dev)
			return &apple_sleep_coprocs[i];

	return NULL;
}

/* A registered coprocessor its driver has readied, whose run bit the last step clears. */
static bool apple_sleep_coproc_ready_node(const struct device_node *np)
{
	int i;

	for (i = 0; np && i < SLEEP_COPROCS; i++)
		if (apple_sleep_coprocs[i].dev && apple_sleep_coprocs[i].ready &&
		    apple_sleep_coprocs[i].dev->of_node == np)
			return true;
	return false;
}

static bool sleep_usb_asleep;

void apple_sleep_usb_set_asleep(bool asleep)
{
	WRITE_ONCE(sleep_usb_asleep, asleep);
}
EXPORT_SYMBOL_GPL(apple_sleep_usb_set_asleep);

static unsigned long sleep_usb_wake_ports;

void apple_sleep_usb_set_wake_ports(unsigned long ports)
{
	WRITE_ONCE(sleep_usb_wake_ports, ports);
}
EXPORT_SYMBOL_GPL(apple_sleep_usb_set_wake_ports);

bool apple_sleep_usb_port_can_wake(unsigned int port)
{
	return port < BITS_PER_LONG && (READ_ONCE(sleep_usb_wake_ports) & BIT(port));
}
EXPORT_SYMBOL_GPL(apple_sleep_usb_port_can_wake);

/* macOS has every port's always-on USB domain on at its commit with both ports in use. */

static unsigned long sleep_usb_aon_kept;

bool apple_sleep_usb_aon_keep(unsigned int port, bool keep)
{
	if (port >= BITS_PER_LONG)
		return false;
	if (!keep) {
		clear_bit(port, &sleep_usb_aon_kept);
		return false;
	}
	set_bit(port, &sleep_usb_aon_kept);
	return true;
}
EXPORT_SYMBOL_GPL(apple_sleep_usb_aon_keep);

/* The USB host's vote, kept until the word is mapped. Only bit 0 is touched, as on macOS. */
static DEFINE_SPINLOCK(sleep_slp_ddr_lock);
static bool sleep_slp_ddr_usb;
static void __iomem *sleep_slp_ddr_word;

/*
 * A coprocessor that runs through a sleep crashed. The AOP is the always-on master SLP_DDR keeps
 * DRAM up for: after its firmware crashed, the next SLP_DDR wake stalled in the sleep engine,
 * while the deep path doesn't need it. Nothing restarts it short of a reboot.
 */
static bool sleep_slp_ddr_crashed;

/* This sleep only: the DRAM-kept wake would stall on something this commit leaves wrong. */
static bool sleep_slp_ddr_deep_once;

static void sleep_slp_ddr_write(void)
{
	u32 val;

	lockdep_assert_held(&sleep_slp_ddr_lock);
	if (!sleep_slp_ddr_word)
		return;
	val = readl(sleep_slp_ddr_word) & ~WAKE_SLP_DDR_USB;
	if (sleep_slp_ddr_usb && !sleep_slp_ddr_crashed && !sleep_slp_ddr_deep_once)
		val |= WAKE_SLP_DDR_USB;
	writel(val, sleep_slp_ddr_word);
}

/* A driver found something the DRAM-kept wake would stall on: this sleep goes deep. */
static bool sleep_deep_requested;

bool apple_sleep_request_deep_once(void)
{
	if (!READ_ONCE(apple_sleep))
		return false;
	WRITE_ONCE(sleep_deep_requested, true);

	return true;
}
EXPORT_SYMBOL_GPL(apple_sleep_request_deep_once);

static void sleep_slp_ddr_set_deep_once(bool deep)
{
	guard(spinlock_irqsave)(&sleep_slp_ddr_lock);
	sleep_slp_ddr_deep_once = deep;
	sleep_slp_ddr_write();
}

void apple_sleep_usb_slp_ddr(bool up)
{
	guard(spinlock_irqsave)(&sleep_slp_ddr_lock);
	sleep_slp_ddr_usb = up;
	sleep_slp_ddr_write();
}
EXPORT_SYMBOL_GPL(apple_sleep_usb_slp_ddr);

/*
 * From RTKit, for every firmware crash. One this driver stops and restarts around each sleep
 * doesn't run through it; for any other, every later sleep takes the deep path. It may come
 * from a poll with interrupts off, so the table is read without its lock: entries only change
 * at registration.
 */
void apple_sleep_coproc_crashed(struct device *dev)
{
	if (!apple_sleep || apple_sleep_coproc_find(dev))
		return;
	scoped_guard(spinlock_irqsave, &sleep_slp_ddr_lock) {
		if (sleep_slp_ddr_crashed)
			return;
		sleep_slp_ddr_crashed = true;
		sleep_slp_ddr_write();
	}
	dev_warn(apple_sleep->dev, "%s crashed: later sleeps take the deep path\n", dev_name(dev));
}
EXPORT_SYMBOL_GPL(apple_sleep_coproc_crashed);

static bool sleep_debug_link_cut;

/* Only the debug port's controller took the cold reset. */
static bool sleep_debug_port_is(struct device *dev)
{
	return apple_sleep && apple_sleep->hpm_np && dev->of_node == apple_sleep->hpm_np;
}

bool apple_sleep_debug_link_was_cut(struct device *dev)
{
	return sleep_debug_port_is(dev) && xchg(&sleep_debug_link_cut, false);
}
EXPORT_SYMBOL_GPL(apple_sleep_debug_link_was_cut);

bool apple_sleep_debug_link_cut_peek(struct device *dev)
{
	return sleep_debug_port_is(dev) && READ_ONCE(sleep_debug_link_cut);
}
EXPORT_SYMBOL_GPL(apple_sleep_debug_link_cut_peek);

/* The radio's host set its port up again after a real sleep: the next commit may stop it. */
void apple_sleep_radio_port_up(void)
{
	if (apple_sleep)
		WRITE_ONCE(apple_sleep->apcie_port_down, false);
}
EXPORT_SYMBOL_GPL(apple_sleep_radio_port_up);

#define PIN_REG(n)		(4 * (n))
#define PIN_DATA		BIT(0)
#define PIN_LOCK		BIT(21)
#define PIN_CTL			0xc48
#define PIN_IRQ(g, w)		(0x800 + 0x40 * (g) + 4 * (w))

/* Before any device suspends, with the pins' domain on. Skipped pins aren't read. */
static void sleep_pins_save(struct apple_sleep *s)
{
	unsigned int pin;

	s->pins_lost = false;
	s->pins_saved_valid = false;
	if (!s->pins)
		return;
	for (pin = 0; pin < s->npins; pin++)
		if (!test_bit(pin, s->pins_skip))
			s->pins_saved[pin] = readl(s->pins + PIN_REG(pin));
	s->pins_saved_valid = true;
}

/*
 * A deep sleep resets every pin, and the pin controller's driver puts back only the
 * ones it configured. macOS puts all of them back right before ANS's run bit, and
 * ANS's link doesn't come up without that: 1 written at 0xc48, every interrupt
 * group's pending bits cleared, then each pin written back as it reads, then as it
 * was, its lock bit last.
 */
void apple_sleep_restore_pins(void)
{
	struct apple_sleep *s = apple_sleep;
	unsigned int pin, grp, w, changed = 0;

	if (!s || !s->pins_lost)
		return;
	s->pins_lost = false;

	writel(1, s->pins + PIN_CTL);
	for (grp = 0; grp < s->pin_irq_groups; grp++)
		for (w = 0; w < DIV_ROUND_UP(s->npins, 32); w++)
			writel(U32_MAX, s->pins + PIN_IRQ(grp, w));
	for (pin = 0; pin < s->npins; pin++) {
		void __iomem *reg = s->pins + PIN_REG(pin);
		u32 want = s->pins_saved[pin], cur;

		if (test_bit(pin, s->pins_skip))
			continue;
		cur = readl(reg);
		changed += cur != want;
		writel(cur, reg);
		if (want & PIN_LOCK)
			writel(want & ~PIN_LOCK, reg);
		writel(want, reg);
	}
	dev_info(s->dev, "%u of %u AP pins changed by the sleep, all put back\n", changed,
		 s->npins - bitmap_weight(s->pins_skip, s->npins));
}
EXPORT_SYMBOL_GPL(apple_sleep_restore_pins);

/*
 * T6050's ANS owner: the sleep stops ANS itself in its last steps. One
 * attempt runs from .begin to .end: NVMe's own .suspend registers before
 * .prepare closes registration, .prepare_late (after device noirq suspend)
 * reserves the one ready owner, and .finish or .end releases it on the suspend
 * task, after early resume has taken its receipt. The manager never takes the
 * owner's device lock: no late or noirq PM phase does either, so an unregister
 * can wait for the release.
 */
static struct {
	struct task_struct *task;
	struct device *dev;
	struct module *module;
	bool open, reserved, leaving;
} sleep_ans_late;
static DECLARE_COMPLETION(sleep_ans_late_released);

static bool sleep_ans_late_selected(void)
{
	return apple_sleep_v27_late();
}

bool apple_sleep_ans_late(void)
{
	return sleep_ans_late_selected();
}
EXPORT_SYMBOL_GPL(apple_sleep_ans_late);

static void sleep_ans_late_open(void)
{
	if (!sleep_ans_late_selected())
		return;
	guard(mutex)(&apple_sleep_coprocs_lock);
	sleep_ans_late.task = current;
	sleep_ans_late.open = true;
}

static void sleep_ans_late_close(void)
{
	if (!sleep_ans_late_selected())
		return;
	guard(mutex)(&apple_sleep_coprocs_lock);
	sleep_ans_late.open = false;
}

/* Exactly one bound, registered, ready ANS owner. Pins it; takes no device lock. */
static int sleep_ans_late_reserve(void)
{
	struct apple_sleep_coproc *c = NULL;
	struct device *dev;
	int i, n = 0;

	if (!sleep_ans_late_selected())
		return 0;
	guard(mutex)(&apple_sleep_coprocs_lock);
	dev = sleep_ans_late.dev;
	if (sleep_ans_late.task != current || sleep_ans_late.reserved || sleep_ans_late.leaving ||
	    !dev)
		return -EBUSY;
	for (i = 0; i < SLEEP_COPROCS; i++)
		if (apple_sleep_coprocs[i].dev && apple_sleep_coprocs[i].pcie_ans) {
			c = &apple_sleep_coprocs[i];
			n++;
		}
	if (n != 1 || c->dev != dev || !c->ready || c->stopped || !c->last_check ||
	    !c->last_stop || !device_is_bound(dev) || !dev->driver)
		return -EBUSY;
	if (!try_module_get(dev->driver->owner))
		return -ENODEV;
	sleep_ans_late.module = dev->driver->owner;
	get_device(dev);
	reinit_completion(&sleep_ans_late_released);
	sleep_ans_late.reserved = true;
	return 0;
}

/* .finish, then .end: both on the suspend task, both after early resume. */
static void sleep_ans_late_release(bool end)
{
	struct module *module = NULL;
	struct device *dev = NULL;

	if (!sleep_ans_late_selected())
		return;
	scoped_guard(mutex, &apple_sleep_coprocs_lock) {
		WARN_ON_ONCE(sleep_ans_late.task && sleep_ans_late.task != current);
		if (sleep_ans_late.reserved) {
			dev = sleep_ans_late.dev;
			module = sleep_ans_late.module;
			sleep_ans_late.module = NULL;
			sleep_ans_late.reserved = false;
			complete_all(&sleep_ans_late_released);
		}
		if (end) {
			sleep_ans_late.open = false;
			sleep_ans_late.task = NULL;
		}
	}
	if (dev) {
		put_device(dev);
		module_put(module);
	}
}

/* Unregister waits out a reservation; no new one starts once it is leaving. */
static void sleep_ans_late_leave(struct device *dev)
{
	for (;;) {
		scoped_guard(mutex, &apple_sleep_coprocs_lock) {
			if (sleep_ans_late.dev != dev)
				return;
			sleep_ans_late.leaving = true;
			if (!sleep_ans_late.reserved)
				return;
		}
		wait_for_completion(&sleep_ans_late_released);
	}
}

/* Below EL2 a monitor owns the last mile, and this code doesn't run it. */
int apple_sleep_coproc_register(struct device *dev, void __iomem *asc, bool pcie_ans,
				int (*last_stop)(void *data), void *data)
{
	struct apple_sleep_coproc *c;

	if (!is_kernel_in_hyp_mode())
		return -ENODEV;

	guard(mutex)(&apple_sleep_coprocs_lock);
	if (apple_sleep_coproc_find(dev))
		return -EEXIST;
	c = apple_sleep_coproc_find(NULL);
	if (!c)
		return -ENOSPC;
	c->asc = asc;
	c->pcie_ans = pcie_ans;
	c->last_check = NULL;
	c->last_stop = last_stop;
	c->data = data;
	c->ready = false;
	c->stopped = false;
	c->first = false;
	c->early = false;
	c->ps = 0;
	c->dev = dev;

	return 0;
}
EXPORT_SYMBOL_GPL(apple_sleep_coproc_register);

void apple_sleep_coproc_set_early(struct device *dev)
{
	struct apple_sleep_coproc *c;

	guard(mutex)(&apple_sleep_coprocs_lock);
	c = apple_sleep_coproc_find(dev);
	if (c)
		c->early = true;
}
EXPORT_SYMBOL_GPL(apple_sleep_coproc_set_early);

int apple_sleep_coproc_register_first(struct device *dev, int (*last_stop)(void *data),
				      void *data)
{
	struct apple_sleep_coproc *c;
	struct device_node *np;
	u32 ps = 0;
	int ret;

	if (!last_stop)
		return -EINVAL;
	np = of_parse_phandle(dev->of_node, "power-domains", 0);
	if (np) {
		of_property_read_u32(np, "reg", &ps);
		of_node_put(np);
	}
	ret = apple_sleep_coproc_register(dev, NULL, false, last_stop, data);
	if (ret)
		return ret;

	guard(mutex)(&apple_sleep_coprocs_lock);
	c = apple_sleep_coproc_find(dev);
	if (c) {
		c->first = true;
		c->ps = ps;
		c->domain = of_parse_phandle(dev->of_node, "power-domains", 0);
	}

	return 0;
}
EXPORT_SYMBOL_GPL(apple_sleep_coproc_register_first);

int apple_sleep_coproc_set_last_check(struct device *dev, int (*last_check)(void *data))
{
	struct apple_sleep_coproc *c;

	guard(mutex)(&apple_sleep_coprocs_lock);
	c = apple_sleep_coproc_find(dev);
	if (!c)
		return -ENOENT;
	c->last_check = last_check;

	return 0;
}
EXPORT_SYMBOL_GPL(apple_sleep_coproc_set_last_check);

int apple_sleep_enclave_register(struct device *dev, int (*stop)(void *data), void *data)
{
	if (!is_kernel_in_hyp_mode())
		return -ENODEV;

	if (!stop)
		return -EINVAL;
	if (!apple_sleep_commit_stops_coprocs())
		return -EOPNOTSUPP;

	guard(mutex)(&apple_sleep_coprocs_lock);
	if (apple_sleep_enclave.dev)
		return -EBUSY;
	apple_sleep_enclave.stop = stop;
	apple_sleep_enclave.data = data;
	apple_sleep_enclave.dev = dev;

	return 0;
}
EXPORT_SYMBOL_GPL(apple_sleep_enclave_register);

void apple_sleep_enclave_unregister(struct device *dev)
{
	guard(mutex)(&apple_sleep_coprocs_lock);
	if (apple_sleep_enclave.dev == dev)
		memset(&apple_sleep_enclave, 0, sizeof(apple_sleep_enclave));
}
EXPORT_SYMBOL_GPL(apple_sleep_enclave_unregister);

void apple_sleep_coproc_unregister(struct device *dev)
{
	struct apple_sleep_coproc *c;

	if (!dev)
		return;
	sleep_ans_late_leave(dev);
	guard(mutex)(&apple_sleep_coprocs_lock);
	c = apple_sleep_coproc_find(dev);
	if (c) {
		of_node_put(c->domain);
		memset(c, 0, sizeof(*c));
	}
	if (sleep_ans_late.dev == dev) {
		sleep_ans_late.dev = NULL;
		sleep_ans_late.leaving = false;
	}
}
EXPORT_SYMBOL_GPL(apple_sleep_coproc_unregister);

void apple_sleep_coproc_set_ready(struct device *dev, bool ready)
{
	struct apple_sleep_coproc *c;

	guard(mutex)(&apple_sleep_coprocs_lock);
	c = apple_sleep_coproc_find(dev);
	if (!c)
		return;
	c->ready = ready;
	c->stopped = false;
}
EXPORT_SYMBOL_GPL(apple_sleep_coproc_set_ready);

bool apple_sleep_coproc_stopped(struct device *dev)
{
	struct apple_sleep_coproc *c;

	guard(mutex)(&apple_sleep_coprocs_lock);
	c = apple_sleep_coproc_find(dev);
	return c && c->stopped;
}
EXPORT_SYMBOL_GPL(apple_sleep_coproc_stopped);

bool apple_sleep_commit_stops_coprocs(void)
{
	return READ_ONCE(apple_sleep) && pm_suspend_via_firmware();
}
EXPORT_SYMBOL_GPL(apple_sleep_commit_stops_coprocs);

/*
 * The last thing that can fail on the way to the commit: no CPU but this one is up and
 * interrupts are off, so no lock can be held. A coprocessor stopped here comes back only
 * through a real sleep, so every refusal has to be behind this, and nothing after it may
 * go back. In the order measured on macOS, on ANS itself: whatever its driver does last,
 * status and control read, then 0 written twice.
 */
static void apple_sleep_coproc_run_off(struct apple_sleep_coproc *c)
{
	/* Its own last stop cleared its run bits. */
	if (!c->asc) {
		c->stopped = true;
		return;
	}
	if (!apple_sleep->soc->regions)
		c->status = readl(c->asc + ASC_CPU_STATUS);
	writel(readl(c->asc + ASC_CPU_CONTROL) & ~ASC_CPU_CONTROL_RUN, c->asc + ASC_CPU_CONTROL);
	writel(readl(c->asc + ASC_CPU_CONTROL) & ~ASC_CPU_CONTROL_RUN, c->asc + ASC_CPU_CONTROL);
	c->stopped = true;
}

/* A power word that a ready coprocessor's last stop lowers: on until the commit. */
static bool apple_sleep_coproc_lowers(u32 ps)
{
	int i;

	for (i = 0; i < SLEEP_COPROCS; i++)
		if (apple_sleep_coprocs[i].dev && apple_sleep_coprocs[i].ready &&
		    apple_sleep_coprocs[i].first &&
		    ((apple_sleep->soc->regions && apple_sleep_coprocs[i].domain) ?
		     sleep_ps_offset(apple_sleep, apple_sleep_coprocs[i].domain) == ps :
		     apple_sleep_coprocs[i].ps == ps))
			return true;
	return false;
}

/* The last refusals, each coprocessor's own: nothing has stopped yet. */
static int apple_sleep_coprocs_check(struct apple_sleep *s)
{
	int i, ret;

	for (i = 0; i < SLEEP_COPROCS; i++) {
		struct apple_sleep_coproc *c = &apple_sleep_coprocs[i];

		if (!c->dev || !c->ready || c->stopped || !c->last_check)
			continue;
		ret = c->last_check(c->data);
		if (ret) {
			dev_err(s->dev, "refused: %s: %d\n", dev_name(c->dev), ret);
			return ret;
		}
	}

	return 0;
}

/*
 * The other cores stop at the commit, and one left awake there keeps the engine waiting
 * until WD2 resets the SoC. Each answers here, before the display goes off, and waits for the
 * stop or a refusal.
 */
static int apple_sleep_cpus_hold(struct apple_sleep *s)
{
	int ret = spin_table_hold_parked();

	if (ret)
		dev_err(s->dev, "refused: a CPU didn't answer the hold: %d\n", ret);

	return ret;
}

/* A nonzero result must mean no SLEEP was sent; post-send errors are fatal. */
static int apple_sleep_enclave_stop(struct apple_sleep *s)
{
	int ret;

	if (!apple_sleep_enclave.stop)
		return 0;
	ret = apple_sleep_enclave.stop(apple_sleep_enclave.data);
	if (ret)
		dev_err(s->dev, "refused before SEP SLEEP was sent: %d\n", ret);

	return ret;
}

/*
 * In two passes, after the last refusal: from the first stop only a real sleep restarts the
 * GPU, so a failure here goes on to the commit. Those registered first stop whole first,
 * before macOS's off words drop under them: the GPU withdraws its PMP request there, and the
 * PMP has to see that before its own stop. The rest after the match.
 */
static void apple_sleep_coproc_stop(struct apple_sleep *s, struct apple_sleep_coproc *c)
{
	s->pcie_ans_stopped |= c->pcie_ans;
	if (c->stopped)
		return;
	c->stop_ret = c->last_stop ? c->last_stop(c->data) : 0;
	if (s->soc->regions && c->pcie_ans && c->stop_ret)
		s->ans_m5_wake_error = -EIO;
	apple_sleep_coproc_run_off(c);
}

static void apple_sleep_coprocs_stop(struct apple_sleep *s, bool first)
{
	/*
	 * Slot order, except that T6050's second pass stops ANS before the rest, as macOS
	 * does on the M5.
	 * The ANS owner registers at its .suspend, after the PMP's probe-time slot.
	 */
	bool ans_first = s->soc->regions && !first;
	int pass, i;

	if (first)
		s->pcie_ans_stopped = false;
	/* macOS's order: ANS, SIO (early), then the rest, PMP among them. */
	for (pass = ans_first ? 0 : 2; pass < 3; pass++) {
		if (ans_first && pass == 1 && s->soc->ops->after_ans)
			s->soc->ops->after_ans(s);
		for (i = 0; i < SLEEP_COPROCS; i++) {
			struct apple_sleep_coproc *c = &apple_sleep_coprocs[i];

			if (!c->dev || !c->ready || c->first != first ||
			    (ans_first && pass != (c->pcie_ans ? 0 : c->early ? 1 : 2)))
				continue;
			apple_sleep_coproc_stop(s, c);
		}
	}
	dsb(sy);
}

const struct apple_sleep_smc_policy *apple_sleep_smc_policy(void)
{
	return apple_sleep && pm_suspend_via_firmware() ? apple_sleep->soc->smc_policy : NULL;
}
EXPORT_SYMBOL_GPL(apple_sleep_smc_policy);

int apple_sleep_smc_register(struct device *dev,
		int (*late_notice)(void *data, u64 *sent, u64 *ack), void *data)
{
	struct apple_sleep *s = apple_sleep;
	guard(mutex)(&apple_sleep_smc_lock);

	if (!s || !s->soc->smc_policy || !pm_suspend_via_firmware())
		return -EOPNOTSUPP;
	if (!late_notice || s->smc.saved_ops || (s->smc.dev && s->smc.dev != dev))
		return -EBUSY;
	if (!s->smc.dev)
		s->smc.dev = get_device(dev);
	s->smc.late_notice = late_notice;
	s->smc.data = data;
	return 0;
}
EXPORT_SYMBOL_GPL(apple_sleep_smc_register);

int apple_sleep_smc_register_saved(struct device *dev,
		const struct apple_sleep_smc_saved_ops *ops, void *data)
{
	struct apple_sleep *s = apple_sleep;
	guard(mutex)(&apple_sleep_smc_lock);

	if (!s || !s->soc->early_context || !s->soc->smc_policy ||
	    !pm_suspend_via_firmware())
		return -EOPNOTSUPP;
	if (!ops || !ops->begin || !ops->late_notice || !ops->end || s->smc.dev)
		return -EBUSY;
	if (s->soc->ops->commit_notice && (!ops->check || !ops->commit_notice))
		return -EINVAL;
	s->smc.dev = get_device(dev);
	s->smc.saved_ops = ops;
	s->smc.late_notice = ops->late_notice;
	s->smc.data = data;
	return 0;
}
EXPORT_SYMBOL_GPL(apple_sleep_smc_register_saved);

void apple_sleep_smc_unregister(struct device *dev)
{
	guard(mutex)(&apple_sleep_smc_lock);

	/* Callers hold dev's lock (.complete or .remove), so a held scope
	 * cannot be torn down underneath the IRQ-disabled callback.
	 */
	if (apple_sleep && apple_sleep->smc.dev == dev) {
		put_device(apple_sleep->smc.dev);
		memset(&apple_sleep->smc, 0, sizeof(apple_sleep->smc));
	}
}
EXPORT_SYMBOL_GPL(apple_sleep_smc_unregister);

void apple_sleep_display_quiesced(struct device *dev, bool quiesced)
{
	struct apple_sleep *s = apple_sleep;
	unsigned int i;

	if (!s || !s->soc->regions)
		return;
	for (i = 0; i < s->soc->display_count; i++)
		if (s->display[i].np == dev->of_node)
			WRITE_ONCE(s->display[i].quiesced, quiesced);
}
EXPORT_SYMBOL_GPL(apple_sleep_display_quiesced);

static unsigned int sleep_cpu_cluster(const struct apple_sleep *s, u64 mpidr)
{
	return MPIDR_AFFINITY_LEVEL(mpidr, s->soc->cpu_cluster_affinity);
}

static unsigned int sleep_cpu_core(u64 mpidr)
{
	return MPIDR_AFFINITY_LEVEL(mpidr, 0);
}

static u32 sleep_cpu_start_bit(const struct apple_sleep *s, u64 mpidr)
{
	return BIT(s->soc->cpu_start_stride * sleep_cpu_cluster(s, mpidr) +
		   sleep_cpu_core(mpidr));
}

/* Validate every possible hotplug target before any power callback is exposed. */
static int apple_sleep_validate_cpu_topology(struct apple_sleep *s)
{
	const struct apple_sleep_soc *soc = s->soc;
	struct device_node *np = s->dev->of_node;
	struct resource res;
	u32 used = 0;
	unsigned int cpu;
	int index, ret;

	if (soc->cpu_cluster_affinity > 3 || !soc->cpu_clusters ||
	    soc->cpu_clusters > SLEEP_MAX_CLUSTERS || !soc->cpu_cores_per_cluster ||
	    soc->cpu_cores_per_cluster > 32 || soc->cpu_start_stride > 32 ||
	    soc->cpu_start_stride < soc->cpu_cores_per_cluster ||
	    soc->cpu_start_stride * (soc->cpu_clusters - 1) +
		    soc->cpu_cores_per_cluster > 32)
		return dev_err_probe(s->dev, -EINVAL, "invalid CPU-start geometry\n");

	if (soc->machine && (!of_machine_is_compatible(soc->machine) ||
	    cpu_logical_map(0) != soc->boot_mpidr ||
	    num_possible_cpus() != soc->cpu_count))
		return dev_err_probe(s->dev, -EINVAL, "unsupported board/CPU inventory\n");

	index = of_property_match_string(np, "reg-names", "cpu-start");
	if (index < 0)
		return index;
	ret = of_address_to_resource(np, index, &res);
	if (ret)
		return ret;
	if (resource_size(&res) < max_t(unsigned int, CPU_START_STOP,
		CPU_START_CLUSTER(soc->cpu_clusters - 1)) + sizeof(u32))
		return dev_err_probe(s->dev, -EINVAL, "CPU-start resource too small\n");

	for_each_possible_cpu(cpu) {
		u64 mpidr = cpu_logical_map(cpu);
		unsigned int cluster = sleep_cpu_cluster(s, mpidr);
		unsigned int core = sleep_cpu_core(mpidr);
		u32 bit;

		if (cluster >= soc->cpu_clusters || core >= soc->cpu_cores_per_cluster ||
		    (soc->cpu_valid && !soc->cpu_valid(mpidr)))
			return dev_err_probe(s->dev, -EINVAL,
					     "unsupported CPU%u MPIDR %#llx\n", cpu, mpidr);
		bit = sleep_cpu_start_bit(s, mpidr);
		if (used & bit)
			return dev_err_probe(s->dev, -EINVAL,
					     "duplicate CPU-start bit for CPU%u\n", cpu);
		used |= bit;
	}

	return 0;
}

static void sleep_deep_wfi_arm(void)
{
	/* A timer left running ends the WFI, and costs power until reset. */
	write_sysreg(ARCH_TIMER_CTRL_IT_MASK, cntp_ctl_el0);
	write_sysreg(ARCH_TIMER_CTRL_IT_MASK, cntv_ctl_el0);
	sysreg_clear_set_s(SYS_APL_DEEP_SLEEP, 0, DEEP_SLEEP_ENABLE);
}

/* A core's last instructions: nothing brings it back but a reset. */
static void __noreturn sleep_deep_wfi(void)
{
	sleep_deep_wfi_arm();

	while (true) {
		dsb(sy);
		isb();
		wfi();
		write_sysreg_s(IPI_SR_PENDING, SYS_APL_IPI_SR_EL1);
	}
}

static void sleep_cpu_stop(u64 mpidr)
{
	struct apple_sleep *s = apple_sleep;

	writel(sleep_cpu_start_bit(s, mpidr), s->cpu_start + CPU_START_STOP);
	dsb(sy);
}

static void sleep_cpu_off(u64 mpidr)
{
	if (apple_sleep && apple_sleep->soc->ops->cpu_down)
		apple_sleep->soc->ops->cpu_down(apple_sleep, mpidr);
	sleep_deep_wfi();
}

static int sleep_cpu_on(u64 mpidr)
{
	struct apple_sleep *s = apple_sleep;
	unsigned int cluster = sleep_cpu_cluster(s, mpidr);

	writel(sleep_cpu_start_bit(s, mpidr), s->cpu_start + CPU_START_SYS);
	writel(BIT(sleep_cpu_core(mpidr)), s->cpu_start + CPU_START_CLUSTER(cluster));

	return 0;
}

static int sleep_cpu_hold(u64 mpidr)
{
	return apple_sleep->soc->ops->cpu_prepare ?
		apple_sleep->soc->ops->cpu_prepare(mpidr) : 0;
}

static void sleep_cpu_restore(u64 mpidr)
{
	if (apple_sleep->soc->ops->cpu_restore)
		apple_sleep->soc->ops->cpu_restore(mpidr);
}

static void sleep_cpu_idle_off(u64 mpidr)
{
	if (apple_sleep->soc->ops->idle_wfi)
		apple_sleep->soc->ops->idle_wfi();
}

static const struct spin_table_power_ops sleep_cpu_power_ops = {
	.cpu_hold = sleep_cpu_hold,
	.cpu_idle_off = sleep_cpu_idle_off,
	.cpu_restore = sleep_cpu_restore,
	.cpu_stop = sleep_cpu_stop,
	.cpu_off = sleep_cpu_off,
	.cpu_on = sleep_cpu_on,
};

/*
 * As macOS lowers a domain: AUTO_ENABLE off first if it's set, then target 0.
 * Writing the flags clears them. Values measured, for instance ANS 0x1f0020ff,
 * 0x0f0020ff, 0x0f0023f0.
 */
static u32 sleep_ps_lower_start(struct apple_sleep *s, u32 offset)
{
	void __iomem *reg = sleep_power_reg(s, offset);
	u32 val = readl(reg);

	if (val & PS_AUTO_ENABLE) {
		val &= ~(PS_AUTO_ENABLE | PS_FLAGS);
		writel(val, reg);
	}

	return val;
}

static void sleep_ps_lower_finish(struct apple_sleep *s, u32 offset, u32 val)
{
	writel((val & ~PS_TARGET) | PS_FLAGS, sleep_power_reg(s, offset));
}

static void sleep_ps_lower(struct apple_sleep *s, u32 offset)
{
	sleep_ps_lower_finish(s, offset, sleep_ps_lower_start(s, offset));
}

/*
 * As macOS raises a domain: the target, then AUTO_ENABLE once it reads on. Measured
 * byte for byte on ANS and PCIe, for instance 0x0f002300, 0x0f00200f, 0x1f00200f.
 */
static int sleep_ps_raise_start(struct apple_sleep *s, u32 offset)
{
	void __iomem *reg = sleep_power_reg(s, offset);
	u32 now;

	writel((readl(reg) & ~(PS_AUTO_ENABLE | GENMASK(11, 0))) | PS_TARGET, reg);

	return readl_poll_timeout_atomic(reg, now, (now & PS_ACTUAL) == PS_ACTUAL, 1,
					 PS_OFF_TIMEOUT_US);
}

static void sleep_ps_raise_finish(struct apple_sleep *s, u32 offset)
{
	void __iomem *reg = sleep_power_reg(s, offset);

	writel((readl(reg) & ~(PS_AUTO_ENABLE | GENMASK(11, 0))) | PS_TARGET | PS_AUTO_ENABLE,
	       reg);
}

static int sleep_ps_raise(struct apple_sleep *s, u32 offset)
{
	int ret = sleep_ps_raise_start(s, offset);

	sleep_ps_raise_finish(s, offset);

	return ret;
}

/* A domain the hardware idled under AUTO_ENABLE comes back once that's cleared. */
static bool sleep_ps_wait_on(struct apple_sleep *s, u32 offset, u32 val)
{
	if (!FIELD_GET(PS_TARGET, val))
		return false;

	return !readl_poll_timeout_atomic(sleep_power_reg(s, offset), val, FIELD_GET(PS_ACTUAL, val), 1,
					  PS_OFF_TIMEOUT_US);
}

static int sleep_ps_wait_off(struct apple_sleep *s, u32 offset)
{
	u32 val;

	return readl_poll_timeout_atomic(sleep_power_reg(s, offset), val, !FIELD_GET(PS_ACTUAL, val),
					 10, PS_OFF_TIMEOUT_US);
}

static bool sleep_ps_on(struct apple_sleep *s, u32 offset)
{
	return FIELD_GET(PS_ACTUAL, readl(sleep_power_reg(s, offset)));
}

/* Below full power the hardware has idled it, and its registers may not answer. */
static bool sleep_ps_full(struct apple_sleep *s, u32 offset)
{
	return FIELD_GET(PS_ACTUAL, readl(s->ps + offset)) == FIELD_MAX(PS_ACTUAL);
}

/* @pd's word in "ps": a power controller node, child of the PMGR that "ps" maps. */
static int sleep_ps_offset(struct apple_sleep *s, struct device_node *pd)
{
	struct device_node *pmgr __free(device_node) = NULL;
	struct resource res;
	u32 off;

	if (s->soc->regions)
		return sleep_region_domain_offset(s, pd);
	pmgr = of_get_parent(pd);
	if (!pmgr || of_address_to_resource(pmgr, 0, &res) || res.start != s->ps_res.start ||
	    of_property_read_u32(pd, "reg", &off) || off % 4 ||
	    off + 4 > resource_size(&s->ps_res))
		return -EINVAL;

	return off;
}

int apple_sleep_domain_check(struct device_node *pd)
{
	int off;

	if (!apple_sleep)
		return -ENODEV;
	off = sleep_ps_offset(apple_sleep, pd);
	return off < 0 ? off : 0;
}
EXPORT_SYMBOL_GPL(apple_sleep_domain_check);

/*
 * A power domain lowered or raised by its own driver for a sleep, before the commit or after
 * a refused one, as macOS's drivers do through its PMGR. A lower waits for off. Until the
 * driver raises it, the PMGR's driver leaves it alone at the wake.
 */
int apple_sleep_domain_set(struct device_node *pd, bool on)
{
	struct apple_sleep *s = apple_sleep;
	int off, ret = 0;
	u32 val;
	unsigned int bit;
	unsigned long *down;

	if (!s)
		return -ENODEV;
	off = sleep_ps_offset(s, pd);
	if (off < 0)
		return off;

	down = sleep_domain_bitmap(s, off, &bit);
	if (!down)
		return -EINVAL;

	val = readl(sleep_power_reg(s, off));
	if (on) {
		if (FIELD_GET(PS_TARGET, val) != 0xf || FIELD_GET(PS_ACTUAL, val) != 0xf)
			ret = sleep_ps_raise(s, off);
		else if (!(val & PS_AUTO_ENABLE))
			sleep_ps_raise_finish(s, off);
		if (!ret)
			clear_bit(bit, down);
		return ret;
	}
	set_bit(bit, down);
	if (FIELD_GET(PS_TARGET, val))
		sleep_ps_lower(s, off);
	return sleep_ps_wait_off(s, off);
}
EXPORT_SYMBOL_GPL(apple_sleep_domain_set);

bool apple_sleep_domain_is_down(struct device_node *pd)
{
	struct apple_sleep *s = apple_sleep;
	unsigned long *down;
	unsigned int bit;
	int off;

	if (!s)
		return false;
	off = sleep_ps_offset(s, pd);
	if (off < 0)
		return false;
	down = sleep_domain_bitmap(s, off, &bit);
	return down && test_bit(bit, down);
}
EXPORT_SYMBOL_GPL(apple_sleep_domain_is_down);

/*
 * After a refused sleep, the sleep engine's own restart of a firmware's CPU whose domain shares
 * a power switch and can't gate alone, as SIO's: each of the device's own domains reset through
 * target 4 and 0, released with the domain off, then raised again, its DARTs' state kept. The
 * caller has cleared the run bit, and starts the firmware after.
 */
#define PS_BUSY			BIT(11)
#define PS_RESET		BIT(31)
#define PS_ENGINE_RESET_4	0x8f000244
#define PS_ENGINE_RESET_0	0x80000000
#define ASC_CPU_STATUS_READY	BIT(1)
#define ENGINE_RESET_PS		4
#define ENGINE_RESET_DARTS	2

static int sleep_engine_reset_wait(struct apple_sleep *s, u32 offset, u32 actual)
{
	u32 val;

	return readl_poll_timeout_atomic(sleep_power_reg(s, offset), val,
					 FIELD_GET(PS_ACTUAL, val) == actual && !(val & PS_BUSY),
					 10, PS_OFF_TIMEOUT_US);
}

/* A domain of the PMGR block mapped here as "ps", and its offset in it. */
static bool sleep_ps_domain(struct apple_sleep *s, struct device_node *pd, u32 *offset)
{
	int idx = of_property_match_string(s->dev->of_node, "reg-names", "ps");
	struct device_node *parent = of_get_parent(pd);
	struct resource ps, res;
	bool ours;

	ours = idx >= 0 && parent && !of_address_to_resource(s->dev->of_node, idx, &ps) &&
	       !of_address_to_resource(parent, 0, &res) && res.start == ps.start &&
	       !of_property_read_u32(pd, "reg", offset);
	of_node_put(parent);

	return ours;
}

/* The @index-th DART in @np's iommus, a reference held, or NULL past the last. */
static struct device *sleep_iommus_dart(struct device_node *np, int index)
{
	struct of_phandle_args args;
	struct platform_device *pdev;

	if (of_parse_phandle_with_args(np, "iommus", "#iommu-cells", index, &args))
		return NULL;
	pdev = of_find_device_by_node(args.np);
	of_node_put(args.np);

	return pdev ? &pdev->dev : NULL;
}

int apple_sleep_engine_reset(struct device *dev)
{
	struct apple_sleep *s = apple_sleep;
	struct device *darts[ENGINE_RESET_DARTS] = {};
	u32 ps[ENGINE_RESET_PS], saved[ENGINE_RESET_PS], val;
	struct of_phandle_iterator it;
	void __iomem *asc;
	int i, n = 0, idx, err, ret = 0;

	if (!s)
		return -ENODEV;
	if (s->soc->regions)
		return -EOPNOTSUPP; /* No measured M5 standalone engine-reset request. */
	of_for_each_phandle(&it, err, dev->of_node, "power-domains", NULL, 0) {
		if (!sleep_ps_domain(s, it.node, &val))
			continue;
		if (n == ENGINE_RESET_PS) {
			of_node_put(it.node);
			return -E2BIG;
		}
		ps[n++] = val;
	}
	if (!n)
		return -ENODEV;

	idx = of_property_match_string(dev->of_node, "reg-names", "coproc");
	if (idx < 0)
		idx = of_property_match_string(dev->of_node, "reg-names", "asc");
	asc = idx >= 0 ? of_iomap(dev->of_node, idx) : NULL;
	if (!asc)
		return -ENODEV;

	for (i = 0; i < ENGINE_RESET_DARTS; i++) {
		darts[i] = sleep_iommus_dart(dev->of_node, i);
		if (!darts[i])
			break;
		apple_dart_state_save(darts[i]);
	}

	for (i = 0; i < n && !ret; i++) {
		saved[i] = readl(sleep_power_reg(s, ps[i]));
		writel(PS_ENGINE_RESET_4, sleep_power_reg(s, ps[i]));
		ret = sleep_engine_reset_wait(s, ps[i], 4);
		if (!ret) {
			writel(PS_ENGINE_RESET_0, sleep_power_reg(s, ps[i]));
			ret = sleep_engine_reset_wait(s, ps[i], 0);
		}
	}
	if (!ret) {
		usleep_range(1000, 2000);
		for (i = 0; i < n; i++)
			writel(0, sleep_power_reg(s, ps[i]));
		for (i = n - 1; i >= 0 && !ret; i--) {
			writel(PS_TARGET, sleep_power_reg(s, ps[i]));
			ret = sleep_engine_reset_wait(s, ps[i], FIELD_MAX(PS_ACTUAL));
			if (!ret)
				writel(saved[i] & ~(PS_FLAGS | PS_RESET | PS_ACTUAL),
				       sleep_power_reg(s, ps[i]));
		}
	}
	if (!ret)
		ret = readl_poll_timeout(asc + ASC_CPU_STATUS, val, val & ASC_CPU_STATUS_READY, 10,
					 100 * USEC_PER_MSEC);

	for (i = 0; i < ENGINE_RESET_DARTS && darts[i]; i++) {
		apple_dart_state_restore(darts[i]);
		put_device(darts[i]);
	}
	iounmap(asc);

	return ret;
}
EXPORT_SYMBOL_GPL(apple_sleep_engine_reset);

/* The order macOS does it in, across clusters. */
static void sleep_dvfm_park(struct apple_sleep *s)
{
	int cluster, word;

	for (cluster = 0; cluster < s->soc->cpu_clusters; cluster++) {
		s->dvfm_ctl[cluster] = readq(s->dvfm[cluster] + DVFM_CTL);
		for (word = 0; word < ARRAY_SIZE(sleep_dvfm_words); word++)
			s->dvfm_saved[cluster][word] =
				readq(s->dvfm[cluster] + sleep_dvfm_words[word]);
	}

	for (cluster = 0; cluster < s->soc->cpu_clusters; cluster++)
		writeq(0, s->dvfm[cluster] + DVFM_CTL);
	for (word = 0; word < 2; word++)
		for (cluster = 0; cluster < s->soc->cpu_clusters; cluster++)
			writeq(s->dvfm_saved[cluster][word] & ~DVFM_ENABLE,
			       s->dvfm[cluster] + sleep_dvfm_words[word]);
	for (cluster = 0; cluster < s->soc->cpu_clusters; cluster++)
		for (word = 2; word < 4; word++)
			writeq(s->dvfm_saved[cluster][word] & ~DVFM_ENABLE,
			       s->dvfm[cluster] + sleep_dvfm_words[word]);
	s->dvfm_parked = true;
}

static void sleep_dvfm_unpark(struct apple_sleep *s)
{
	int cluster, word;

	if (!s->dvfm_parked)
		return;
	s->dvfm_parked = false;

	for (cluster = 0; cluster < s->soc->cpu_clusters; cluster++)
		writeq(s->dvfm_ctl[cluster], s->dvfm[cluster] + DVFM_CTL);
	for (cluster = 0; cluster < s->soc->cpu_clusters; cluster++)
		for (word = 2; word < 4; word++)
			writeq(s->dvfm_saved[cluster][word],
			       s->dvfm[cluster] + sleep_dvfm_words[word]);
	for (word = 1; word >= 0; word--)
		for (cluster = 0; cluster < s->soc->cpu_clusters; cluster++)
			writeq(s->dvfm_saved[cluster][word],
			       s->dvfm[cluster] + sleep_dvfm_words[word]);
}

/*
 * Every device has suspended by the last step, so a stray word still on has no user: it's
 * lowered there, and a sleep with one that won't go off is refused. They come back on after
 * the sleep or a refusal, for genpd's books.
 */
static void apple_sleep_stray_restore(struct apple_sleep *s)
{
	const struct apple_sleep_word *stray = s->soc->stray;
	int i;

	for (i = s->soc->nstray - 1; i >= 0; i--)
		if (s->stray_lowered & BIT(i))
			sleep_ps_raise(s, stray[i].offset);
	s->stray_lowered = 0;
}

static int apple_sleep_lower_stray(struct apple_sleep *s)
{
	const struct apple_sleep_word *stray = s->soc->stray;
	int i, ret = 0;

	s->stray_lowered = 0;
	for (i = 0; i < s->soc->nstray; i++) {
		u32 off = stray[i].offset;

		if (!sleep_ps_on(s, off))
			continue;
		/* Its driver's own last stop lowers it, at the commit. */
		if (apple_sleep_coproc_lowers(off)) {
			dev_dbg(s->dev, "%s: left on for its driver's last stop\n",
				stray[i].name);
			continue;
		}
		sleep_ps_lower(s, off);
		s->stray_lowered |= BIT(i);
		if (sleep_ps_wait_off(s, off)) {
			dev_err(s->dev, "refused: %s won't go off: %#x\n",
				stray[i].name, readl(sleep_power_reg(s, off)));
			ret = -EBUSY;
		} else {
			dev_info(s->dev, "%s was on at the last step, lowered\n",
				 stray[i].name);
		}
	}
	if (ret)
		apple_sleep_stray_restore(s);

	return ret;
}

/*
 * The SoC's coprocessors that must be bound for a sleep, the ones that must also have stopped
 * by the last step, and the domains macOS has off by then. Taken in process context, since
 * the checks run with interrupts off. A driver whose stop failed has already failed the
 * suspend, naming itself, so what's left is whether it's bound at all, and what the hardware
 * shows.
 */
static struct device_node *sleep_gate_node(const struct apple_sleep_gate_coproc *g)
{
	struct device_node *np;
	struct resource res;
	unsigned int i;

	if (g->path)
		return of_find_node_by_path(g->path);
	for_each_compatible_node(np, NULL, g->compatible) {
		if (!g->match_start)
			return np;
		for (i = 0; !of_address_to_resource(np, i, &res); i++)
			if (res.start == g->match_start)
				return np;
	}
	return NULL;
}

static void apple_sleep_gate_prepare(struct apple_sleep *s)
{
	const struct apple_sleep_gate_coproc *coprocs = s->soc->gate_coprocs;
	int i;

	s->gate_count = 0;
	for (i = 0; i < s->soc->ngate_coprocs && s->gate_count < ARRAY_SIZE(s->gate); i++) {
		struct device_node *c = sleep_gate_node(&coprocs[i]);
		struct platform_device *pdev = c ? of_find_device_by_node(c) : NULL;
		struct apple_sleep_gate *g;

		if (!c) {
			dev_warn(s->dev, "%s isn't in this tree\n",
				 coprocs[i].path ?: coprocs[i].compatible);
			if (s->soc->strict_gate_inventory) {
				g = &s->gate[s->gate_count++];
				g->name = coprocs[i].compatible;
				g->bound = false;
				g->strict_unbound = true;
			}
			continue;
		}
		g = &s->gate[s->gate_count++];
		g->name = c->name;
		g->np = c;
		g->bound = pdev && device_is_bound(&pdev->dev);
		g->sep = of_node_name_eq(c, "sep");
		g->must_stop = coprocs[i].must_stop;
		g->ps = coprocs[i].ps;
		g->allow_unstarted = coprocs[i].allow_unstarted;
		g->strict_unbound = coprocs[i].strict_unbound;
		if ((g->must_stop || g->allow_unstarted) && !g->asc) {
			int idx = coprocs[i].explicit_run_index ? coprocs[i].run_index :
				of_property_match_string(c, "reg-names", "coproc");

			if (idx < 0)
				idx = of_property_match_string(c, "reg-names", "asc");
			if (idx >= 0 && (g->allow_unstarted || coprocs[i].explicit_run_index)) {
				struct resource res;

				if (of_address_to_resource(c, idx, &res) ||
				    resource_size(&res) < ASC_CPU_CONTROL + sizeof(u32))
					idx = -EINVAL;
			}
			if (idx >= 0)
				g->asc = of_iomap(c, idx);
		}
		if (pdev)
			put_device(&pdev->dev);
		of_node_put(c);
	}
}

/*
 * Names everything wrong, so one boot shows the whole list. A coprocessor that must stop and
 * still runs refuses the sleep; the rest only warns, as a real sleep is the only full test.
 */
static bool apple_sleep_gate_failed(struct apple_sleep *s)
{
	int i, bad = 0, running = 0;

	for (i = 0; i < s->gate_count; i++) {
		struct apple_sleep_gate *g = &s->gate[i];

		/* Even an always-on domain may read actual0: require a prepared protocol. */
		if (g->sep && (!g->bound || !apple_sleep_enclave.stop)) {
			dev_err(s->dev, "refused: SEP has no prepared irreversible sleep request\n");
			bad++;
			running++;
			continue;
		}
		/* A RUN-clear image loaded by iBoot is not running firmware. Unknown
		 * mappings refuse; never probe the SMC's unsafe ASC status aperture.
		 */
		if (!g->bound && g->allow_unstarted) {
			if (g->ps && !sleep_ps_on(s, g->ps)) {
				dev_err(s->dev, "%s RUN state inaccessible; refusing unknown state\n", g->name);
				bad++;
				running++;
				continue;
			}
			if (g->asc && !(readl(g->asc + ASC_CPU_CONTROL) & ASC_CPU_CONTROL_RUN))
				continue;
			dev_err(s->dev, "%s running or RUN control unavailable without driver\n", g->name);
			bad++;
			running++;
			continue;
		}
		if (!g->bound) {
			dev_err(s->dev, "%s isn't bound\n", g->name);
			bad++;
			/* Nothing will stop it, and a commit over a running CPU resets the SoC. */
			if (g->must_stop && g->asc && g->ps && sleep_ps_full(s, g->ps) &&
			    (readl(g->asc + ASC_CPU_CONTROL) & ASC_CPU_CONTROL_RUN)) {
				dev_err(s->dev, "%s runs unbound: control %#x status %#x\n", g->name,
					readl(g->asc + ASC_CPU_CONTROL),
					readl(g->asc + ASC_CPU_STATUS));
				running++;
			}
			if (g->strict_unbound)
				running++;
			continue;
		}
		/* A CPU whose domain is off isn't running, and its registers aren't there. */
		if (g->ps && !sleep_ps_on(s, g->ps))
			continue;
		if (g->must_stop && (!g->asc ||
				     ((readl(g->asc + ASC_CPU_CONTROL) & ASC_CPU_CONTROL_RUN) &&
				      !apple_sleep_coproc_ready_node(g->np) &&
				      !sleep_v27_gate_prepared(g->np)))) {
			if (g->allow_unstarted || s->soc->regions)
				dev_err(s->dev, "%s didn't stop: control %#x\n", g->name,
					g->asc ? readl(g->asc + ASC_CPU_CONTROL) : 0);
			else
				dev_err(s->dev, "%s didn't stop: control %#x status %#x\n", g->name,
					g->asc ? readl(g->asc + ASC_CPU_CONTROL) : 0,
					g->asc ? readl(g->asc + ASC_CPU_STATUS) : 0);
			bad++;
			running++;
		}
	}

	for (i = 0; i < s->soc->ngate_off; i++) {
		const struct apple_sleep_word *w = &s->soc->gate_off[i];

		if (sleep_ps_on(s, w->offset)) {
			dev_err(s->dev, "%s is on: %#x\n", w->name, readl(sleep_power_reg(s, w->offset)));
			bad++;
		}
	}

	if (running) {
		dev_err(s->dev, "refused: %d coprocessors that must stop or gate are still running\n",
			running);
		return true;
	}
	if (bad && s->soc->strict_gate_inventory)
		return true;
	if (bad)
		dev_warn(s->dev, "going ahead with %d problems\n", bad);
	else
		dev_info(s->dev, "%u coprocessors up and stopped\n", s->gate_count);

	return false;
}

static void sleep_wake_unmask(u32 *val, u32 id)
{
	if (id / 32 < WAKE_WORDS)
		val[id / 32] &= ~BIT(id % 32);
}

/*
 * Mask every source but the ones asked for. These words read all zero until
 * something writes them, which leaves every source enabled, listed in the ADT or
 * not. macOS reads all ones outside the ADT's ap-wake-sources (measured). The AOP's
 * source 0 is masked with the rest: that held under macOS with its handler hidden,
 * as did leaving it enabled with the AMCC enables off. The two together are unmeasured.
 */
static void sleep_wake_masks(struct apple_sleep *s, u32 *val)
{
	int i;

	for (i = 0; i < WAKE_WORDS; i++)
		val[i] = i == WAKE_WORDS - 1 ? s->soc->wake_last_word : U32_MAX;
	for (i = 0; i < s->wake_enable_count; i++)
		sleep_wake_unmask(val, s->wake_enable[i]);
}

/* The last checks, then the stray words lowered: what has stopped may not return until a wake. */
static int apple_sleep_refuse(struct apple_sleep *s)
{
	const struct apple_sleep_domains *d = &s->soc->ps;
	bool refused = false, idled = false;
	int i;

	for (i = 0; i < SLEEP_COPROCS; i++) {
		struct apple_sleep_coproc *c = &apple_sleep_coprocs[i];

		if (!c->dev)
			continue;
		if (c->ready) {
			idled |= c->pcie_ans;
		} else {
			/* Nothing may lower a coprocessor's domain under its driver. */
			dev_err(s->dev, "refused: %s is running and its driver has not quiesced it\n",
				dev_name(c->dev));
			refused = true;
		}
	}

	if (num_online_cpus() != 1 ||
	    (read_cpuid_mpidr() & MPIDR_HWID_BITMASK) != s->mbox->boot_mpidr) {
		dev_err(s->dev, "refused: wrong CPU, or other CPUs online\n");
		refused = true;
	}

	/* Only a driver takes the display to where its domain can be lowered. */
	if (!s->display[0].bound) {
		dev_err(s->dev, "refused: no driver bound to the display coprocessor\n");
		refused = true;
	}

	/* ANS's link needs these after a real power loss. */
	if (idled && !s->soc->regions && !s->apcie_phy_tunable) {
		dev_err(s->dev, "refused: no tunables to bring ANS's PCIe block back\n");
		refused = true;
	}

	/* Only ANS's driver can stop it, and it doesn't come back without a real sleep. */
	if (!idled && (sleep_ps_on(s, d->ans) || sleep_ps_on(s, d->apcie_sys_gp))) {
		dev_err(s->dev, "refused: ANS or PCIe is on with no driver to stop it\n");
		refused = true;
	}

	/* The engine waits for DISP_CPU with no timeout, so this must never slip. */
	for (i = 0; i < s->soc->display_count; i++) {
		u32 ps = d->disp[i][0];

		if (s->display[i].bound && sleep_ps_on(s, ps) &&
		    (readl(s->display[i].asc + ASC_CPU_CONTROL) & ASC_CPU_CONTROL_RUN) &&
		    (!apple_sleep_v27_late() || sleep_v27_final_check(i))) {
			dev_err(s->dev, "refused: a bound display coprocessor is still running\n");
			refused = true;
		}
	}

	if (apple_sleep_gate_failed(s))
		refused = true;

	if (refused)
		return -EBUSY;

	return apple_sleep_lower_stray(s);
}

static void sleep_wake_arm(struct apple_sleep *s)
{
	u32 val[WAKE_WORDS];
	int i;

	for (i = 0; i < WAKE_WORDS; i++)
		s->wake_saved[i] = readl(s->wake + WAKE_MASK(i));
	sleep_wake_masks(s, val);
	for (i = 0; i < WAKE_WORDS; i++) {
		writel(val[i], s->wake + WAKE_MASK(i));
		s->wake_armed[i] = ~val[i];
	}
}

static void sleep_wake_restore(struct apple_sleep *s)
{
	int i;

	for (i = 0; i < WAKE_WORDS; i++)
		writel(s->wake_saved[i], s->wake + WAKE_MASK(i));
}

/*
 * The sleep engine waits on DISP_CPU with no timeout, so the display domains
 * must be off. A coprocessor's driver stops its firmware first, and nothing here
 * talks to the firmware. Otherwise no driver is bound, and the bootloader left the
 * firmware running: on the DCP, stop its CPU cold. That's unproven. The coprocessor
 * restarts from its reset vector after any sleep, and the engine itself clears the
 * DCP's run bit on the way down.
 *
 * Domain order and values as macOS's: DCPEXT then DCP, CPU, front end, system.
 * The rest of the display's domains go off with these, by nothing Linux or macOS
 * writes. The coprocessors' secure mailboxes are not used here. Whether that
 * holds is open.
 */
static int sleep_display_off(struct apple_sleep *s)
{
	const u32 (*domains)[3] = s->soc->ps.disp;
	int i, ret = 0;

	for (i = s->soc->display_count - 1; i >= 0; i--) {
		void __iomem *asc = s->display[i].asc;

		/* The engine doesn't wait for DISPEXT0. */
		if (i == 1 && !s->display[i].bound)
			continue;

		if (sleep_ps_on(s, domains[i][0]) && asc &&
		    (readl(asc + ASC_CPU_CONTROL) & ASC_CPU_CONTROL_RUN)) {
			writel(0, asc + ASC_CPU_CONTROL);
			writel(0, asc + ASC_CPU_CONTROL);
		}

		/* Reads as a count of the DCPs powered, and macOS ends at 0. */
		writel(i, s->pmc_dcp);
		if (i == 0) {
			if (readl(s->ps + domains[0][0]) & PS_AUTO_ENABLE)
				writel(readl(s->ps + domains[0][0]) &
				       ~(PS_AUTO_ENABLE | PS_FLAGS), s->ps + domains[0][0]);
			writel(3, s->pwrgate + s->soc->pwrgate_disp[0]);
			writel(0, s->pwrgate + s->soc->pwrgate_disp[1]);
		}
		sleep_ps_lower(s, domains[i][0]);
		sleep_ps_lower(s, domains[i][1]);
		sleep_ps_lower(s, domains[i][2]);
	}

	for (i = 0; i < s->soc->display_count; i++) {
		int err;

		if (i == 1 && !s->display[i].bound)
			continue;

		err = sleep_ps_wait_off(s, domains[i][0]);

		if (err) {
			dev_err(s->dev, "refused: display domain %#x stayed on: %#x\n",
				domains[i][0], readl(s->ps + domains[i][0]));
			ret = err;
		}
	}

	return ret;
}

/* One command on the port controller's bus, polled: no other CPU, and interrupts are off. */
static int sleep_hpm_cmd(struct apple_sleep *s, u32 op, u32 extra, const u32 *in, u32 *out)
{
	void __iomem *b = s->hpm_spmi;
	u32 status, reply;
	int ret;

	while (!(readl(b + SPMI_STATUS) & SPMI_STATUS_RX_EMPTY))
		readl(b + SPMI_REPLY);

	writel(extra << 16 | SPMI_CMD_ACTIVE | s->hpm_sid << 8 | op, b + SPMI_CMD);
	if (in)
		writel(*in, b + SPMI_CMD);
	ret = readl_poll_timeout_atomic(b + SPMI_STATUS, status,
					!(status & SPMI_STATUS_RX_EMPTY), 10, SPMI_POLL_US);
	if (ret)
		return ret;
	reply = readl(b + SPMI_REPLY);
	writel(1, b + SPMI_NOTIFY_ACK);
	if (out) {
		ret = readl_poll_timeout_atomic(b + SPMI_STATUS, status,
						!(status & SPMI_STATUS_RX_EMPTY), 10,
						SPMI_POLL_US);
		if (ret)
			return ret;
		*out = readl(b + SPMI_REPLY);
	}

	/* A zero write's reply carries the value written. */
	return op == SPMI_OP_ZERO_WRITE || (reply & 0xff) == op ? 0 : -EIO;
}

static int sleep_hpm_select(struct apple_sleep *s, u32 page)
{
	u32 val = ~0U;
	int i, ret;

	ret = sleep_hpm_cmd(s, SPMI_OP_ZERO_WRITE, page << 8, NULL, NULL);
	for (i = 0; !ret && i < 2000; i++) {
		ret = sleep_hpm_cmd(s, SPMI_OP_EXT_READ(1), HPM_REG_SELECT, NULL, &val);
		if (!ret && (val & 0xff) == page)
			return 0;
		/* A select it dropped, waking, is sent again. page | 0x80 is one in progress. */
		if (!ret && (val & 0xff) != (page | 0x80) && i % 20 == 19) {
			ret = sleep_hpm_cmd(s, SPMI_OP_WAKEUP, 0, NULL, NULL);
			if (!ret)
				ret = sleep_hpm_cmd(s, SPMI_OP_ZERO_WRITE, page << 8, NULL, NULL);
		}
		udelay(10);
	}

	return ret ?: -ETIMEDOUT;
}

/*
 * What macOS sends the other port controller about a second before a sleep, measured
 * in three runs: system power state 3, then the bus's sleep command.
 */
static int sleep_hpm_sleep(struct apple_sleep *s)
{
	u32 data = HPM_SSPS_S3, cmd = HPM_CMD_SSPS, val = ~0U;
	int i, ret;

	ret = sleep_hpm_cmd(s, SPMI_OP_WAKEUP, 0, NULL, NULL);
	if (!ret)
		ret = sleep_hpm_select(s, HPM_PAGE_DATA1);
	if (!ret)
		ret = sleep_hpm_cmd(s, SPMI_OP_EXT_WRITE(1), HPM_REG_DATA_CMD, &data, NULL);
	if (!ret)
		ret = sleep_hpm_select(s, HPM_PAGE_CMD1);
	if (!ret)
		ret = sleep_hpm_cmd(s, SPMI_OP_EXT_WRITE(4), HPM_REG_DATA_CMD, &cmd, NULL);
	/* Selected afresh for each read, as macOS does. */
	for (i = 0; !ret && val && i < 100; i++) {
		udelay(1000);
		ret = sleep_hpm_select(s, HPM_PAGE_CMD1);
		if (!ret)
			ret = sleep_hpm_cmd(s, SPMI_OP_EXT_READ(4), HPM_REG_DATA, NULL, &val);
	}
	if (!ret && val)
		ret = -ETIMEDOUT;
	/* Asleep either way: a failure here only means it wasn't told first. */
	sleep_hpm_cmd(s, SPMI_OP_SLEEP, 0, NULL, NULL);

	return ret;
}

/*
 * The partner takes the reset as an unplug, attaches again and renegotiates its contract.
 * Left to finish across the commit, that sometimes failed: neither side had a contract or
 * saw the other, and no VDM got through until a replug. Returns the ms it took.
 */
static int sleep_hpm_wait_contract(struct apple_sleep *s, int *gone_ms)
{
	ktime_t start = ktime_get();
	bool wake = true, up;
	u32 val = 0;
	int ms, ret;

	/*
	 * It reads back as before for a moment after the reset. Count it as back once it was
	 * seen gone (no contract, or no answer while the controller restarts), or after the
	 * floor if it never seemed to go.
	 */
	*gone_ms = -1;
	mdelay(HPM_RESET_FIRST_MS);
	for (;;) {
		ms = ktime_ms_delta(ktime_get(), start);
		if (ms >= HPM_CONTRACT_MS)
			return -ETIMEDOUT;
		ret = wake ? sleep_hpm_cmd(s, SPMI_OP_WAKEUP, 0, NULL, NULL) : 0;
		if (!ret)
			ret = sleep_hpm_select(s, HPM_PAGE_POWER_STATUS);
		if (!ret)
			ret = sleep_hpm_cmd(s, SPMI_OP_EXT_READ(4), HPM_REG_DATA, NULL, &val);
		up = !ret && (val & HPM_POWER_CONNECTED) &&
		     FIELD_GET(HPM_POWER_TYPEC_MODE, val) == HPM_POWER_TYPEC_PD;
		if (!up && *gone_ms < 0)
			*gone_ms = ms;
		if (up && (*gone_ms >= 0 || ms >= HPM_RESET_FLOOR_MS))
			return ms;
		wake = ret;
		mdelay(10);
	}
}

/* A live debug link holds the debug switch domain on. */
static bool sleep_debug_link_up(struct apple_sleep *s)
{
	return s->hpm_spmi &&
	       FIELD_GET(PS_ACTUAL, readl(s->ps_aon + s->soc->ps_aon_debug_switch));
}

/*
 * The debug link stays up with the ATC0 domains off, and it holds the debug domains
 * active, which macOS has idle. The only known way to end it is the port controller's
 * cold reset. To the SMC that's a cable pulled: it records the port's events as wake
 * reasons once it has been warned of the sleep, and wakes the AP right after the commit.
 * So the SMC's driver warns it only after this. tipd rearms the link after a refusal.
 */
static bool apple_sleep_usb_down(struct apple_sleep *s)
{
	u32 cmd = HPM_CMD_COLD_RESET;
	u32 enable;
	int i, ret, ms, gone;

	if (s->usb_down_ran)
		return false;
	s->usb_down_ran = true;

	if (!sleep_debug_link_up(s))
		return false;

	dev_info(s->dev, "debug link: cutting it for the sleep\n");
	enable = readl(s->hpm_spmi + SPMI_NOTIFY_ENABLE);
	writel(enable & ~SPMI_NOTIFY_REPLY, s->hpm_spmi + SPMI_NOTIFY_ENABLE);
	/* A reset that the partner doesn't come back from is done again. */
	for (i = 1; i <= HPM_RESET_TRIES; i++) {
		ret = sleep_hpm_cmd(s, SPMI_OP_WAKEUP, 0, NULL, NULL);
		if (!ret)
			ret = sleep_hpm_select(s, HPM_PAGE_CMD1);
		if (!ret)
			ret = sleep_hpm_cmd(s, SPMI_OP_EXT_WRITE(4), HPM_REG_DATA, &cmd, NULL);
		if (ret)
			break;
		WRITE_ONCE(sleep_debug_link_cut, true);
		ms = sleep_hpm_wait_contract(s, &gone);
		if (ms >= 0) {
			dev_info(s->dev, "debug link: contract back %d ms after reset %d (gone at %d ms)\n",
				 ms, i, gone);
			break;
		}
		dev_warn(s->dev, "debug link: no contract with the partner after reset %d\n", i);
	}
	if (!ret)
		ret = sleep_hpm_sleep(s);
	writel(1, s->hpm_spmi + SPMI_NOTIFY_ACK);
	writel(enable, s->hpm_spmi + SPMI_NOTIFY_ENABLE);
	s->usb_down_ret = ret;

	return true;
}

bool apple_sleep_debug_link_cut(void)
{
	return apple_sleep ? apple_sleep_usb_down(apple_sleep) : false;
}
EXPORT_SYMBOL_GPL(apple_sleep_debug_link_cut);

bool apple_sleep_debug_link_up(void)
{
	struct apple_sleep *s = apple_sleep;

	return s && !s->usb_down_ran && sleep_debug_link_up(s);
}
EXPORT_SYMBOL_GPL(apple_sleep_debug_link_up);

/*
 * macOS takes the wake for a lid closing during the sleep as a dark one: every driver wakes,
 * the display stays off, and the root domain sleeps again as soon as it sees the lid closed.
 */
void apple_sleep_lid_closed_wake(void)
{
	struct apple_sleep *s = apple_sleep;

	if (!s || !s->smc_woke_alone)
		return;
	dev_info(s->dev, "the lid closed during the sleep: dark wake\n");
	pm_set_dark_wake();
}
EXPORT_SYMBOL_GPL(apple_sleep_lid_closed_wake);

/* Read in the SMC's noirq resume. Forgotten as each sleep begins. */
static int sleep_lid_closed = -ENODATA;

void apple_sleep_lid_at_wake(bool closed)
{
	WRITE_ONCE(sleep_lid_closed, closed);
}
EXPORT_SYMBOL_GPL(apple_sleep_lid_at_wake);

int apple_sleep_lid_closed_at_wake(void)
{
	return READ_ONCE(sleep_lid_closed);
}
EXPORT_SYMBOL_GPL(apple_sleep_lid_closed_at_wake);

/* Set in the SMC's noirq resume. Forgotten as each sleep begins. */
static int sleep_radio_woke = -ENODATA;

void apple_sleep_radio_at_wake(bool woke)
{
	WRITE_ONCE(sleep_radio_woke, woke);
}
EXPORT_SYMBOL_GPL(apple_sleep_radio_at_wake);

int apple_sleep_radio_woke(void)
{
	return READ_ONCE(sleep_radio_woke);
}
EXPORT_SYMBOL_GPL(apple_sleep_radio_woke);

/*
 * The SMC woke the AP for the port alone, and this sleep cut the debug link: the cut's own
 * events. Sleep again. The link stays down until tipd rearms it at a full wake, so the next
 * entry doesn't cut it.
 */
void apple_sleep_debug_link_cut_wake(void)
{
	struct apple_sleep *s = apple_sleep;

	if (!s || !s->smc_woke_alone || !READ_ONCE(sleep_debug_link_cut))
		return;
	dev_info(s->dev, "debug link: its cut woke the sleep: dark wake\n");
	pm_set_dark_wake();
}
EXPORT_SYMBOL_GPL(apple_sleep_debug_link_cut_wake);

/*
 * The radio's link and the engine that starts it, in macOS's order, measured on one
 * sleep: the link into L2, the engine stopped, PERST# asserted, the PHY's clocks off.
 * macOS does it 40 ms before ANS's idle. Here it follows, so that a refused sleep finds
 * the link untouched. A link that's down or a radio that's gone only makes the poll time
 * out, and the power goes next either way.
 */
static void sleep_apcie_port_stop(struct apple_sleep *s)
{
	void __iomem *port = s->apcie_port;
	void __iomem *phy = s->apcie_port_phy;
	u32 val;

	writel(readl(port + APCIE_PORT_INTMSK) | APCIE_PORT_INT_LINK, port + APCIE_PORT_INTMSK);
	writel(APCIE_PORT_INT_LINK, port + APCIE_PORT_INTSTAT);

	writel(readl(port + APCIE_PORT_APPCLK) | APCIE_PORT_APPCLK_CGDIS, port + APCIE_PORT_APPCLK);
	/* A loader can leave the port ready with the link never trained. */
	if (readl(port + APCIE_PORT_LINKSTS) & APCIE_PORT_LINKSTS_UP) {
		writel(APCIE_PORT_L2_ENTER, port + APCIE_PORT_L2);
		readl_poll_timeout_atomic(port + APCIE_PORT_L2, val, !(val & APCIE_PORT_L2_BUSY),
					  10, APCIE_PORT_STOP_US);
	}
	/* The radio keeps its power and firmware, in reset until its host releases it. */
	if (s->radio_perst >= 0 && sleep_ps_on(s, s->soc->ps.gpio)) {
		void __iomem *reg = s->pins + PIN_REG(s->radio_perst);

		val = readl(reg);
		writel(s->radio_perst_low ? val & ~PIN_DATA : val | PIN_DATA, reg);
	}
	writel(readl(phy) & ~APCIE_PORT_PHY_LINK, phy);

	if (s->apcie_engine && s->apcie_fabric) {
		writel(0, s->apcie_engine + APCIE_ENGINE_IRQ_MASK);
		writel(3, s->apcie_fabric + APCIE_FABRIC_STOP);
		writel(readl(s->apcie_engine + APCIE_ENGINE_CTRL) | APCIE_ENGINE_STOP,
		       s->apcie_engine + APCIE_ENGINE_CTRL);
	}
	udelay(1000);

	/* What this bit does isn't known. macOS sets it around PERST#. */
	writel(readl(port + APCIE_PORT_PERST_GUARD) | BIT(8), port + APCIE_PORT_PERST_GUARD);
	writel(0, port + APCIE_PORT_PERST);
	writel(readl(port + APCIE_PORT_PERST_GUARD) & ~BIT(8), port + APCIE_PORT_PERST_GUARD);

	writel(readl(phy) & ~APCIE_PORT_PHY_EN1, phy);
	writel(readl(phy) & ~APCIE_PORT_PHY_EN0, phy);
	writel(readl(phy) | APCIE_PHY_CTRL_RESET, phy);
	writel(readl(phy) & ~APCIE_PHY_CTRL_CLK0REQ, phy);
	readl_poll_timeout_atomic(phy, val, !(val & APCIE_PHY_CTRL_CLK0ACK), 1, APCIE_POLL_US);
	writel(readl(phy) & ~APCIE_PHY_CTRL_CLK1REQ, phy);

	writel(readl(port + APCIE_PORT_APPCLK) & ~APCIE_PORT_APPCLK_EN, port + APCIE_PORT_APPCLK);
	writel(readl(port + APCIE_PORT_INTSTAT), port + APCIE_PORT_INTSTAT);
}

static void sleep_apcie_port_stop_once(struct apple_sleep *s)
{
	/* Only with the port set up, by the loader or by its host after a wake, or it aborts. */
	if (s->apcie_port_stopped || s->apcie_port_down || !s->apcie_port || !s->apcie_port_phy ||
	    !sleep_ps_on(s, s->soc->ps.apcie_gp) || !sleep_ps_on(s, s->soc->ps.apcie_phy_sw))
		return;
	s->apcie_port_stopped = true;
	sleep_apcie_port_stop(s);
}

/* A poll here only paces the steps: the power goes next whatever it reads. */
static void sleep_apcie_phy_off(struct apple_sleep *s)
{
	void __iomem *ctrl = s->apcie_phy + APCIE_PHY_CTRL;
	void __iomem *run = s->apcie_phy + APCIE_PHY_RUN;
	u32 val;

	writel(readl(ctrl) | APCIE_PHY_CTRL_RESET, ctrl);
	writel(readl(ctrl) & ~APCIE_PHY_CTRL_CLK1REQ, ctrl);
	readl_poll_timeout_atomic(ctrl, val, !(val & APCIE_PHY_CTRL_CLK1ACK), 1, APCIE_POLL_US);
	writel(readl(ctrl) & ~APCIE_PHY_CTRL_CLK0REQ, ctrl);
	readl_poll_timeout_atomic(ctrl, val, !(val & APCIE_PHY_CTRL_CLK0ACK), 1, APCIE_POLL_US);
	writel(readl(ctrl) & ~APCIE_PHY_CTRL_UP, ctrl);
	writel(readl(run) & ~APCIE_PHY_RUN_EN2, run);
	writel(readl(run) & ~APCIE_PHY_RUN_EN, run);
}

static void sleep_apcie_clk_off(struct apple_sleep *s)
{
	void __iomem *clk = s->apcie_clkgen + APCIE_CLK_CTRL;
	u32 val;

	writel(readl(clk) & ~APCIE_CLK_RUN, clk);
	writel(readl(clk) | APCIE_CLK_OFF, clk);
	readl_poll_timeout_atomic(clk, val, !(val & APCIE_CLK_BUSY), 1, APCIE_POLL_US);
	writel(readl(clk) | APCIE_CLK_OFF2, clk);
}

/*
 * ANS's own link runs on this block, and macOS takes it down with the domains. Its
 * order, measured on one sleep, in the same microseconds as the power words. A block's
 * registers fault with its domain off, and a loader may have left any of these off.
 */
static void apple_sleep_lower_apcie(struct apple_sleep *s)
{
	const struct apple_sleep_domains *d = &s->soc->ps;
	u32 val;

	sleep_apcie_port_stop_once(s);
	/* The PHY's registers need both domains on. */
	if (s->apcie_phy && sleep_ps_on(s, d->apcie_phy_sw) && sleep_ps_on(s, d->apcie_sys_st))
		sleep_apcie_phy_off(s);
	sleep_ps_lower(s, d->apcie_phy_sw);

	if (s->apcie_clkgen && sleep_ps_on(s, d->apcie_sys_st))
		sleep_apcie_clk_off(s);
	sleep_ps_lower(s, d->apcie_sys_st);
	sleep_ps_lower(s, d->apcie_st);

	/*
	 * The AXI enables are the block's link into the fabric. macOS reads the domain on
	 * before clearing them: powered off with them set, SoC NI 1 latches a PCIe receive
	 * link error once the block's last domain goes off.
	 */
	val = sleep_ps_lower_start(s, d->apcie_sys_gp);
	if (s->apcie_axi && sleep_ps_wait_on(s, d->apcie_sys_gp, val)) {
		writel(readl(s->apcie_axi + APCIE_AXI_EN0) & ~BIT(0), s->apcie_axi + APCIE_AXI_EN0);
		writel(readl(s->apcie_axi + APCIE_AXI_EN1) & ~BIT(0), s->apcie_axi + APCIE_AXI_EN1);
	}
	sleep_ps_lower_finish(s, d->apcie_sys_gp, val);

	sleep_ps_lower(s, d->apcie_gp);
	sleep_ps_lower(s, d->ans);
}

/* Read-modify-write in table order, with no write where nothing changes. */
static void sleep_tunable_apply_changed(void __iomem *regs, const struct apple_tunable *t)
{
	size_t i;

	for (i = 0; i < t->sz; i++) {
		void __iomem *reg = regs + t->values[i].offset;
		u32 old = readl(reg);
		u32 val = (old & ~t->values[i].mask) | (t->values[i].value & t->values[i].mask);

		if (val != old)
			writel(val, reg);
	}
}

/*
 * The power loss takes the PHY's PLL and lane settings with it, and ANS's link can't
 * train without them. Applied as macOS does: the PLL table, then the lanes, then the
 * PLL's hold released. That takes about the settling time the PHY needs here.
 */
static void sleep_apcie_phy_ip_apply(struct apple_sleep *s)
{
	void __iomem *ctl = s->apcie_phy_ip + APCIE_PHY_IP_PLL_CTL;

	sleep_tunable_apply_changed(s->apcie_phy_ip, s->apcie_phy_ip_pll);
	sleep_tunable_apply_changed(s->apcie_phy_ip, s->apcie_phy_ip_lanes);
	writel(readl(ctl) & ~APCIE_PHY_IP_PLL_HOLD, ctl);
}

/*
 * Brought up from power-off in macOS's order, measured on one sleep: all of it follows
 * the domains on the waking core, well before ANS runs. Whether ANS needs each step is
 * inferred. Nothing else does this if no host driver is bound.
 */
static int apple_sleep_raise_apcie(struct apple_sleep *s)
{
	const struct apple_sleep_domains *d = &s->soc->ps;
	void __iomem *ctrl = s->apcie_phy + APCIE_PHY_CTRL;
	void __iomem *run = s->apcie_phy + APCIE_PHY_RUN;
	u32 val;
	int ret;

	ret = sleep_ps_raise(s, d->ans);
	ret |= sleep_ps_raise(s, d->apcie_gp);
	ret |= sleep_ps_raise_start(s, d->apcie_sys_gp);
	if (!ret) {
		writel(readl(s->apcie_axi + APCIE_AXI_EN0) | BIT(0), s->apcie_axi + APCIE_AXI_EN0);
		writel(readl(s->apcie_axi + APCIE_AXI_EN1) | BIT(0), s->apcie_axi + APCIE_AXI_EN1);
	}
	sleep_ps_raise_finish(s, d->apcie_sys_gp);
	ret |= sleep_ps_raise(s, d->apcie_st);
	ret |= sleep_ps_raise(s, d->apcie_sys_st);
	if (ret)
		return ret;

	apple_tunable_apply(s->apcie_axi, s->apcie_axi_tunable);
	apple_tunable_apply(s->apcie_cio3pll, s->apcie_cio3pll_tunable);
	apple_tunable_apply(s->apcie_clkgen, s->apcie_clkgen_tunable);
	writel(readl(s->apcie_clkgen + APCIE_CLK_START) | APCIE_CLK_START_GO,
	       s->apcie_clkgen + APCIE_CLK_START);
	/* Both off bits: the lowering sets them, and only a power-off clears OFF. */
	writel(readl(s->apcie_clkgen + APCIE_CLK_CTRL) & ~(APCIE_CLK_OFF2 | APCIE_CLK_OFF),
	       s->apcie_clkgen + APCIE_CLK_CTRL);
	ret = readl_poll_timeout_atomic(s->apcie_clkgen + APCIE_CLK_CTRL, val,
					!(val & APCIE_CLK_BUSY), 1, APCIE_POLL_US);
	if (ret)
		return ret;

	ret = sleep_ps_raise(s, d->apcie_phy_sw);
	if (ret)
		return ret;

	apple_tunable_apply(s->apcie_phy, s->apcie_phy_tunable);
	writel(readl(ctrl) | APCIE_PHY_CTRL_CLK0REQ, ctrl);
	ret = readl_poll_timeout_atomic(ctrl, val, val & APCIE_PHY_CTRL_CLK0ACK, 1, APCIE_POLL_US);
	if (ret)
		return ret;
	writel(readl(ctrl) | APCIE_PHY_CTRL_CLK1REQ, ctrl);
	ret = readl_poll_timeout_atomic(ctrl, val, val & APCIE_PHY_CTRL_CLK1ACK, 1, APCIE_POLL_US);
	if (ret)
		return ret;
	writel(readl(ctrl) & ~APCIE_PHY_CTRL_RESET, ctrl);

	writel(readl(run) | APCIE_PHY_RUN_EN, run);
	sleep_apcie_phy_ip_apply(s);
	writel(readl(run) | APCIE_PHY_RUN_EN2, run);
	ret = readl_poll_timeout_atomic(s->apcie_phy + APCIE_PHY_READY, val, val & BIT(0), 1,
					APCIE_POLL_US);
	if (ret)
		return ret;
	writel(readl(s->apcie_phy + APCIE_PHY_COMMON) | BIT(0), s->apcie_phy + APCIE_PHY_COMMON);
	writel(readl(ctrl) | APCIE_PHY_CTRL_UP, ctrl);

	writel(0, s->apcie_rc + APCIE_RC_ZERO);
	writel(0x140, s->apcie_rc + APCIE_RC_READY_CFG);
	writel(1, s->apcie_rc + APCIE_RC_READY_START);

	return readl_poll_timeout_atomic(s->apcie_rc + APCIE_RC_READY_STAT, val, val & BIT(0), 1,
					 APCIE_POLL_US);
}

/* About 4 ms after the other cores stop on macOS: deepest first, ANS last. */
static void apple_sleep_lower_io(struct apple_sleep *s)
{
	const struct apple_sleep_domains *d = &s->soc->ps;

	/*
	 * USB's complex, deepest first. macOS's host lowers the controller's and the common
	 * domain in its own suspend, so here they're only lowered for a host that didn't. A host
	 * that sleeps in place keeps the always-on one, as macOS's does.
	 */
	if (FIELD_GET(PS_TARGET, readl(sleep_power_reg(s, d->usb))))
		sleep_ps_lower(s, d->usb);
	if (FIELD_GET(PS_TARGET, readl(sleep_power_reg(s, d->usb_common))))
		sleep_ps_lower(s, d->usb_common);
	if (!READ_ONCE(sleep_usb_asleep))
		sleep_ps_lower(s, d->usb_aon);
	if (s->pcie_ans_stopped) {
		apple_sleep_lower_apcie(s);
		s->ans_off_ret = sleep_ps_wait_off(s, d->ans);
	}
}

static void apple_sleep_set_wd2(struct apple_sleep *s, unsigned int secs)
{
	writel(0, s->wdt + WDT_WD2_CTL);
	writel(0, s->wdt + WDT_WD2_COUNT);
	writel(secs * WDT_TICKS_PER_S, s->wdt + WDT_WD2_ALARM);
	writel(1, s->wdt + WDT_WD2_CTL);
}

#define PS_PARENT_MISSING	BIT(11)
#define PS_AUTO_STATE		GENMASK(27, 24)
#define PS_REWAKE_PASSES	2

static void sleep_ps_save(struct apple_sleep *s)
{
	const struct apple_sleep_words *all = &s->soc->ps_all, *aon = &s->soc->ps_aon_all;
	int i;

	for (i = 0; i < all->count; i++)
		s->ps_commit[i] = readl(s->ps + all->offset[i]);
	for (i = 0; i < aon->count; i++)
		s->ps_aon_commit[i] = readl(s->ps_aon + aon->offset[i]);
}

static int sleep_ps_find(const u32 *all, int n, u32 offset)
{
	int i;

	for (i = 0; i < n; i++)
		if (all[i] == offset)
			return i;
	return -1;
}

/*
 * As macOS raises a domain: the target, then AUTO_ENABLE once it reads on. Never
 * DEV_DISABLE, which keeps it off: the SEP's is set at the commit, and macOS's wake writes
 * that word without it. The hardware owns the auto state field, so it stays as read.
 */
static int sleep_ps_restore(void __iomem *reg, u32 saved)
{
	u32 keep = (saved & ~(PS_AUTO_ENABLE | PS_AUTO_STATE | PS_PARENT_MISSING | PS_DEV_DISABLE |
			      PS_FLAGS | PS_ACTUAL)) | (readl(reg) & PS_AUTO_STATE);
	u32 now;
	int ret;

	/*
	 * Off under AUTO_ENABLE at the commit, as the SEP's is: the hardware powers it when
	 * it's wanted, and forcing it on doesn't. macOS writes that word once, AUTO set.
	 */
	if ((saved & PS_AUTO_ENABLE) && !FIELD_GET(PS_ACTUAL, saved)) {
		writel(keep | PS_AUTO_ENABLE, reg);
		return 0;
	}

	writel(keep, reg);
	ret = readl_poll_timeout_atomic(reg, now,
					FIELD_GET(PS_ACTUAL, now) == FIELD_GET(PS_TARGET, saved), 1,
					PS_OFF_TIMEOUT_US);
	if (saved & PS_AUTO_ENABLE)
		writel(keep | PS_AUTO_ENABLE, reg);

	return ret;
}

/* Whether the domain at @offset was on as the wake found it. */
static bool sleep_ps_woke_on(struct apple_sleep *s, u32 offset)
{
	const struct apple_sleep_words *all = &s->soc->ps_all;
	int i = sleep_ps_find(all->offset, all->count, offset);

	return i >= 0 && FIELD_GET(PS_ACTUAL, s->ps_woke[i]);
}

/* Linux had it on and the wake left it otherwise. With AUTO_ENABLE, off is allowed. */
static bool sleep_ps_dropped(u32 saved, u32 woke)
{
	u32 control = PS_AUTO_ENABLE | PS_DEV_DISABLE | PS_TARGET;

	if (!FIELD_GET(PS_TARGET, saved))
		return false;
	if ((saved ^ woke) & control)
		return true;
	return !(saved & PS_AUTO_ENABLE) &&
	       FIELD_GET(PS_ACTUAL, woke) != FIELD_GET(PS_TARGET, saved);
}

/*
 * Every domain back as it was at the commit, before any driver's resume reads its block:
 * macOS's wake words first, in its order, whatever they read, if Linux had them on. Then
 * any other domain the wake left otherwise, a second time for one whose parent came later.
 */
static void sleep_ps_rewake(struct apple_sleep *s)
{
	const struct apple_sleep_soc *soc = s->soc;
	const u32 *all = soc->ps_all.offset, *aon = soc->ps_aon_all.offset;
	unsigned int n = soc->ps_all.count, naon = soc->ps_aon_all.count;
	bool *redo = s->ps_late, *redo_aon = s->ps_aon_late;
	bool *done = s->ps_done, *done_aon = s->ps_aon_done;
	int i, j, pass;

	memset(done, 0, n * sizeof(*done));
	memset(done_aon, 0, naon * sizeof(*done_aon));
	memset(s->ps_late, 0, n * sizeof(*s->ps_late));
	memset(s->ps_aon_late, 0, naon * sizeof(*s->ps_aon_late));
	s->ps_rewoken = 0;
	s->ps_rewake_timeouts = 0;
	for (i = 0; i < n; i++)
		s->ps_woke[i] = readl(sleep_power_reg(s, all[i]));
	for (i = 0; i < naon; i++)
		s->ps_aon_woke[i] = readl(s->ps_aon + aon[i]);

	for (j = 0; j < soc->ps_wake_order.count; j++) {
		i = sleep_ps_find(all, n, soc->ps_wake_order.offset[j]);
		if (i < 0 || !FIELD_GET(PS_TARGET, s->ps_commit[i]))
			continue;
		done[i] = true;
		s->ps_rewoken++;
		if (sleep_ps_restore(sleep_power_reg(s, all[i]), s->ps_commit[i]))
			redo[i] = true;
	}
	for (j = 0; j < soc->ps_aon_wake_order.count; j++) {
		i = sleep_ps_find(aon, naon, soc->ps_aon_wake_order.offset[j]);
		if (i < 0 || !FIELD_GET(PS_TARGET, s->ps_aon_commit[i]))
			continue;
		done_aon[i] = true;
		s->ps_rewoken++;
		if (sleep_ps_restore(s->ps_aon + aon[i], s->ps_aon_commit[i]))
			redo_aon[i] = true;
	}

	for (pass = 0; pass < PS_REWAKE_PASSES; pass++) {
		for (i = 0; i < n; i++) {
			if (!redo[i] &&
			    (done[i] || !sleep_ps_dropped(s->ps_commit[i], s->ps_woke[i])))
				continue;
			if (!done[i]) {
				done[i] = true;
				s->ps_rewoken++;
			}
			redo[i] = sleep_ps_restore(sleep_power_reg(s, all[i]), s->ps_commit[i]);
		}
		for (i = 0; i < naon; i++) {
			if (!redo_aon[i] &&
			    (done_aon[i] ||
			     !sleep_ps_dropped(s->ps_aon_commit[i], s->ps_aon_woke[i])))
				continue;
			if (!done_aon[i]) {
				done_aon[i] = true;
				s->ps_rewoken++;
			}
			redo_aon[i] = sleep_ps_restore(s->ps_aon + aon[i], s->ps_aon_commit[i]);
		}
	}

	for (i = 0; i < n; i++)
		s->ps_rewake_timeouts += redo[i];
	for (i = 0; i < naon; i++)
		s->ps_rewake_timeouts += redo_aon[i];
}

static void sleep_ps_rewake_report(struct apple_sleep *s)
{
	const struct apple_sleep_words *all = &s->soc->ps_all, *aon = &s->soc->ps_aon_all;
	int i;

	dev_dbg(s->dev, "power words: %u written back, %u not on in time\n", s->ps_rewoken,
		s->ps_rewake_timeouts);
	for (i = 0; i < all->count; i++)
		if (s->ps_late[i])
			dev_err(s->dev, "power word %#x not on in time: %#x at the commit, %#x now\n",
				all->offset[i], s->ps_commit[i],
				readl(s->ps + all->offset[i]));
	for (i = 0; i < aon->count; i++)
		if (s->ps_aon_late[i])
			dev_err(s->dev, "power word aon %#x not on in time: %#x at the commit, %#x now\n",
				aon->offset[i], s->ps_aon_commit[i],
				readl(s->ps_aon + aon->offset[i]));
}

/* As macOS writes the PMU: the reply's vector masked, then acknowledged, so none is left raised. */
static void sleep_pmu_write(struct apple_sleep *s, u16 reg, u8 val)
{
	u32 status, enable;

	enable = readl(s->spmi + SPMI_NOTIFY_ENABLE);
	writel(enable & ~SPMI_NOTIFY_REPLY, s->spmi + SPMI_NOTIFY_ENABLE);
	writel(((u32)reg << 16) | SPMI_CMD_WRITE, s->spmi + SPMI_CMD);
	writel(val, s->spmi + SPMI_CMD);
	if (!readl_poll_timeout_atomic(s->spmi + SPMI_STATUS, status,
				       !(status & SPMI_STATUS_RX_EMPTY), 1, 1000))
		readl(s->spmi + SPMI_REPLY);
	writel(1, s->spmi + SPMI_NOTIFY_ACK);
	writel(enable, s->spmi + SPMI_NOTIFY_ENABLE);
}

/*
 * The DRAM-kept wake writes each of these words and gives its switch 171 us to report closed.
 * A domain that went off with BUSY latched holds that poll until WD2 resets the SoC. The PMGR
 * driver doesn't clear the latch in a system sleep, so do it here. Returns whether one stayed
 * latched; the deep path then cuts their power instead.
 */
static bool apple_sleep_ps_unlatch(struct apple_sleep *s)
{
	const struct apple_sleep_words *words = &s->soc->ps_engine_polled;
	bool stuck = false;
	unsigned int i;

	s->ps_unlatched_count = 0;
	for (i = 0; i < words->count && i < ARRAY_SIZE(s->ps_unlatched); i++) {
		void __iomem *reg = s->ps + words->offset[i];
		u32 val = readl(reg), off;

		if (!(val & PS_BUSY) || (val & (PS_ACTUAL | PS_TARGET)))
			continue;
		/* A switch still finishing reads BUSY for a moment: only one that stays is a latch. */
		if (!readl_poll_timeout_atomic(reg, val, !(val & PS_BUSY), 1, PS_BUSY_SETTLE_US))
			continue;
		s->ps_unlatched[s->ps_unlatched_count].offset = words->offset[i];
		s->ps_unlatched[s->ps_unlatched_count++].val = val;
		off = val & ~(PS_AUTO_ENABLE | PS_FLAGS | PS_BUSY | PS_UNLATCH | PS_DEV_DISABLE);
		writel(off | PS_UNLATCH, reg);
		readl_poll_timeout_atomic(reg, val, !(val & PS_BUSY), 1, PS_OFF_TIMEOUT_US);
		writel(off, reg);
		if (readl(reg) & PS_BUSY)
			stuck = true;
	}

	return stuck;
}

static void apple_sleep_match(struct apple_sleep *s)
{
	const struct apple_sleep_soc *soc = s->soc;
	int i, raised = 0, lowered = 0, failed = 0;

	writel(0, s->wdt + WDT_WD0_COUNT);
	writel(0, s->wdt + WDT_WD1_COUNT);
	sleep_pmu_write(s, PMU_BOOT_STAGE, PMU_BOOT_STAGE_SLEEP);

	s->spmi_saved[0] = readl(s->spmi + SPMI_NOTIFY_ENABLE);
	s->spmi_saved[1] = readl(s->spmi_a0);
	writel(SPMI_NOTIFY_ENABLE_SLEEP, s->spmi + SPMI_NOTIFY_ENABLE);
	writel(SPMI_NOTIFY_ENABLE_SLEEP, s->spmi_a0);

	for (i = 0; i < soc->ps_macos_on.count; i++) {
		u32 offset = soc->ps_macos_on.offset[i];

		s->ps_on_saved[i] = readl(sleep_power_reg(s, offset));
		/* State 4 reads as on, and macOS has these at f. */
		if (FIELD_GET(PS_ACTUAL, s->ps_on_saved[i]) == 0xf)
			continue;
		/* Measured at the commit with AUTO_ENABLE clear, unlike after a wake. */
		if (sleep_ps_raise_start(s, offset))
			failed++;
		raised++;
	}
	for (i = 0; i < soc->ps_macos_off.count; i++) {
		u32 offset = soc->ps_macos_off.offset[i];

		s->ps_off_saved[i] = readl(sleep_power_reg(s, offset));
		if (!sleep_ps_on(s, offset))
			continue;
		sleep_ps_lower(s, offset);
		lowered++;
	}
	for (i = 0; i < soc->ps_aon_macos_off.count; i++) {
		void __iomem *reg = s->ps_aon + soc->ps_aon_macos_off.offset[i];
		u32 val = readl(reg);

		s->ps_aon_saved[i] = val;
		if (!FIELD_GET(PS_ACTUAL, val))
			continue;
		if (val & PS_AUTO_ENABLE) {
			val &= ~(PS_AUTO_ENABLE | PS_FLAGS);
			writel(val, reg);
		}
		writel((val & ~PS_TARGET) | PS_FLAGS, reg);
		lowered++;
	}
	for (i = 0; i < soc->ps_aon_macos_no_auto.count; i++) {
		void __iomem *reg = s->ps_aon + soc->ps_aon_macos_no_auto.offset[i];
		u32 val = readl(reg);

		s->ps_aon_auto_saved[i] = val;
		writel(val & ~(PS_AUTO_ENABLE | PS_FLAGS), reg);
	}
	s->matched = true;

	for (i = 0; i < soc->ps_macos_off.count; i++) {
		u32 offset = soc->ps_macos_off.offset[i];

		if (sleep_ps_wait_off(s, offset))
			dev_dbg(s->dev, "still on, macOS has it off: %x = %x\n", offset,
				readl(sleep_power_reg(s, offset)));
	}
	dev_dbg(s->dev, "macOS's power words: raised %d, %d of them failed, lowered %d\n",
		raised, failed, lowered);
}

/* Back as they were, after a refusal past the match or a real wake. */
static void apple_sleep_match_undo(struct apple_sleep *s)
{
	const struct apple_sleep_soc *soc = s->soc;
	int i;

	if (!s->matched)
		return;
	s->matched = false;

	for (i = soc->ps_aon_macos_no_auto.count - 1; i >= 0; i--)
		writel(s->ps_aon_auto_saved[i] & ~PS_FLAGS,
		       s->ps_aon + soc->ps_aon_macos_no_auto.offset[i]);
	for (i = soc->ps_aon_macos_off.count - 1; i >= 0; i--)
		if (FIELD_GET(PS_ACTUAL, s->ps_aon_saved[i]))
			writel(s->ps_aon_saved[i] & ~PS_FLAGS,
			       s->ps_aon + soc->ps_aon_macos_off.offset[i]);
	for (i = soc->ps_macos_off.count - 1; i >= 0; i--)
		if (FIELD_GET(PS_ACTUAL, s->ps_off_saved[i]))
			sleep_ps_raise(s, soc->ps_macos_off.offset[i]);
	for (i = soc->ps_macos_on.count - 1; i >= 0; i--) {
		u32 val = s->ps_on_saved[i];

		if (!FIELD_GET(PS_ACTUAL, val))
			sleep_ps_lower(s, soc->ps_macos_on.offset[i]);
		else if (FIELD_GET(PS_ACTUAL, val) != 0xf)
			writel(val & ~PS_FLAGS, s->ps + soc->ps_macos_on.offset[i]);
	}
	writel(s->spmi_saved[0], s->spmi + SPMI_NOTIFY_ENABLE);
	writel(s->spmi_saved[1], s->spmi_a0);
}

/* The safety net, as macOS arms it. */
static void apple_sleep_arm_wd2(struct apple_sleep *s)
{
	writel(0, s->wdt + WDT_WD0_CTL);
	apple_sleep_set_wd2(s, WDT_SLEEP_SECS);
	writel(0, s->aon_misc + AON_AP_DEADLINE);
}

static int sleep_perf_wait(void __iomem *reg, unsigned int state)
{
	u32 val;

	return readl_poll_timeout_atomic(reg, val,
					 (val & (DCS_BUSY | DCS_STATE)) ==
					 FIELD_PREP(DCS_STATE, state),
					 1, DCS_TIMEOUT_US);
}

static int sleep_dcs_wait(struct apple_sleep *s, unsigned int state)
{
	return sleep_perf_wait(s->dcs, state);
}

/*
 * As macOS does at about -18 ms, on each channel in turn: the enables down a bit at a time,
 * then each of four lanes strobed and cleared, two lanes on every channel before the
 * next two. Each write is built from a fresh read, as the trace has it.
 */
static void apple_sleep_mcc_stop(struct apple_sleep *s)
{
	int ch, pass, n;
	u32 val;

	for (ch = 0; ch < MCC_CHANNELS; ch++) {
		val = readl(s->mcc[ch] + MCC_ENABLES);
		for (n = 0; val && n < 32; n++) {
			val &= val - 1;
			writel(val, s->mcc[ch] + MCC_ENABLES);
			val = readl(s->mcc[ch] + MCC_ENABLES);
		}
	}

	for (pass = 0; pass < 2; pass++) {
		for (ch = 0; ch < MCC_CHANNELS; ch++) {
			void __iomem *mcc = s->mcc[ch];

			for (n = 2 * pass; n < 2 * pass + 2; n++) {
				writel(readl(mcc + MCC_LANES_MASK), mcc + MCC_LANES_MASK);
				writel(readl(mcc + MCC_LANES) | MCC_LANE_STROBE(n),
				       mcc + MCC_LANES);
				writel(readl(mcc + MCC_LANES) & ~MCC_LANE(n), mcc + MCC_LANES);
			}
		}
	}
}

/*
 * The engine's first step polls for DCS at this state and has no way to get it there.
 * macOS has every voter at 0 by now, which is how it lands there, and steps it down itself
 * on the path without the PMC. Both, here, in that order. Nothing can fail by now.
 */
static void apple_sleep_dcs_down(struct apple_sleep *s)
{
	unsigned int state;
	int agent;

	for (agent = 0; agent < PMC_VOTER_AGENTS; agent++)
		if (readl(s->pmc_voters + agent * PMC_VOTER_STRIDE))
			writel(0, s->pmc_voters + agent * PMC_VOTER_STRIDE);
	if (!sleep_dcs_wait(s, DCS_SLEEP_STATE))
		return;

	state = FIELD_GET(DCS_STATE, readl(s->dcs));
	if (state > DCS_STEP_STATE) {
		writel(DCS_STEP_STATE, s->dcs);
		sleep_dcs_wait(s, DCS_STEP_STATE);
	}
	if (state > DCS_SLEEP_STATE) {
		writel(DCS_SLEEP_STATE, s->dcs);
		sleep_dcs_wait(s, DCS_SLEEP_STATE);
	}
}

/* The boot core's last WFI. An event that ends it is taken, and the WFI taken again. */
static void __noreturn sleep_boot_wfi(struct apple_sleep *s)
{
	sleep_deep_wfi_arm();

	while (true) {
		dsb(sy);
		isb();
		wfi();
		aic_sleep_ack();
		write_sysreg_s(IPI_SR_PENDING, SYS_APL_IPI_SR_EL1);
	}
}

/* XNU polls these off just before its complex words. The sleep goes on whatever they read. */
static void apple_sleep_wait_still_on(struct apple_sleep *s)
{
	const struct apple_sleep_words *words = &s->soc->ps_still_on;
	int i;

	for (i = 0; i < words->count; i++)
		sleep_ps_wait_off(s, words->offset[i]);
}

/*
 * The last mile, in macOS's order. It starts after every refusal: this can't be
 * undone, and returning now would leave the clusters armed off. No printing, and
 * under 33 ms from the complex words to the WFI (the engine's bound for the
 * cores to go idle). A hang from here ends at the watchdog.
 *
 * What's left out on purpose:
 *  - The two writes of 0 to 0x302800000: reads 0, changes nothing.
 *  - The RTC alarm: macOS arms it after the commit. rtcwake has armed it long
 *    before this, with no SPMI traffic near the engine.
 */
static void t8140_sleep_commit_regs(struct apple_sleep *s)
{
	int plane, agent;
	u32 val;

	apple_sleep_lower_io(s);
	apple_sleep_arm_wd2(s);
	apple_sleep_wait_still_on(s);

	/* Everything the wake polls is down by now, the words just lowered included. */
	if (apple_sleep_ps_unlatch(s)) {
		s->deep_once = true;
		sleep_slp_ddr_set_deep_once(true);
	}

	/* The SoC comes down before memory does, as on macOS. */
	if (sleep_perf_wait(s->clk_perf + CLK_PERF_SOC, SOC_SLEEP_STATE)) {
		writel(SOC_SLEEP_STATE, s->clk_perf + CLK_PERF_SOC);
		sleep_perf_wait(s->clk_perf + CLK_PERF_SOC, SOC_SLEEP_STATE);
	}
	apple_sleep_dcs_down(s);
	apple_sleep_mcc_stop(s);

	/* Gates, PTD, the first voter twice, gates: macOS's order, measured once. */
	for (agent = 0; agent < 2; agent++) {
		writel(PWRGATE_ON, s->pwrgate + s->soc->pwrgate_cpm[0]);
		writel(PWRGATE_ON, s->pwrgate + s->soc->pwrgate_cpm[1]);
	}
	writeq(0, s->ptd + PTD_A);
	writeq(0, s->ptd + PTD_B);
	for (agent = 0; agent < 2; agent++) {
		readl(s->pmc_voters);
		writel(0, s->pmc_voters);
		readl(s->pmc_voters + 4);
	}
	writel(PWRGATE_ON, s->pwrgate + s->soc->pwrgate_cpm[0]);
	writel(PWRGATE_ON, s->pwrgate + s->soc->pwrgate_cpm[1]);

	/* Already off since .prepare, or since genpd turned the encoders off, as on macOS. */
	writel(0, s->cluster + CLUSTER_AVE);
	s->cluster_ave_commit_ret = readl_poll_timeout_atomic(s->cluster + CLUSTER_AVE, val, !val,
							      1, 5000);
	writel(0, s->cluster + CLUSTER_E);
	writel(0, s->cluster + CLUSTER_P);

	writel(readl(s->ps + s->soc->ps.sio_cpu) & ~PS_AUTO_ENABLE, s->ps + s->soc->ps.sio_cpu);

	/* The SoC refuses the first, on macOS too. */
	writel(CLK_MUX_PARKED, s->clk_mux + CLK_MUX_38);
	writel(CLK_MUX_PARKED, s->clk_mux + CLK_MUX_3C);
	writel(CLK_MUX_PARKED, s->clk_mux + CLK_MUX_34);

	writel(readl(s->cllt + CLLT_0) & ~CLLT_0_CLEAR, s->cllt + CLLT_0);
	writel(readl(s->cllt + CLLT_1) & ~CLLT_1_CLEAR, s->cllt + CLLT_1);
	writel(readl(s->cllt + CLLT_2) & ~CLLT_2_CLEAR, s->cllt + CLLT_2);

	/*
	 * Two runs put these writes after the CLLT's: 5 to 6 ms before the commit
	 * in one, and the CLLT at 5.75 ms in the other. No run has hooked both.
	 */
	sleep_dvfm_park(s);

	sleep_ps_save(s);
	for (plane = 0; plane < ARRAY_SIZE(s->amcc); plane++) {
		void __iomem *amcc = s->amcc[plane];

		writel(readl(amcc + AMCC_WORD0_ENABLE) & ~AMCC_WORD0_DSID_AGE_OUT,
		       amcc + AMCC_WORD0_ENABLE);
		writel(readl(amcc + AMCC_WORD1_ENABLE) & ~AMCC_WORD1_UNEXP_RT_HIT,
		       amcc + AMCC_WORD1_ENABLE);
	}

}

static bool sleep_v27_selected_for(struct apple_sleep *s);

static int apple_sleep_commit(unsigned long unused)
{
	struct apple_sleep *s = apple_sleep;

	if (s->soc->ops->pre_commit)
		s->soc->ops->pre_commit(s);

	/*
	 * The other cores stop first, as on macOS. Each answered the hold in .enter, and they
	 * stay coherent until the commit.
	 */
	s->siq_cfg = read_sysreg_s(SYS_APL_SIQ_CFG_EL1);
	s->cpus_stop_ret = spin_table_stop_parked();
	/*
	 * The stopped cores wait, powered, until after the timer: at macOS's commit every core and CPM
	 * reads 0x1f0, 113 ms after the stop bits. Released into their WFI at this point
	 * instead, cores 1-17 read 0x100 at the timer.
	 */

	s->soc->ops->commit_regs(s);

	/* aura reads this with the MMU off. */
	s->resumes_before = READ_ONCE(s->mbox->resumes);
	WRITE_ONCE(s->mbox->resume_pa, __pa_symbol(cpu_resume));
	dcache_clean_poc((unsigned long)s->mbox, (unsigned long)(s->mbox + 1));
	dsb(sy);

	writel(1, s->commit);
	s->committed_at = __arch_counter_get_cntpct();
	/*
	 * As on macOS: a raised line would end the last WFI, and the engine waits for it. Masked
	 * before the SoC's post-timer PMU commands, as XNU does.
	 */
	aic_sleep_mask_all();
	if (s->soc->ops->post_timer)
		s->soc->ops->post_timer(s);

	/* Each core's deep WFI comes after the commit, this one last. Any SoC post-timer traffic is finished. */
	spin_table_release_stopped();
	sleep_boot_wfi(s);
}

/* Back on the boot core through the reset vector, MMU on, in macOS's order. */
static void t8140_sleep_wake_regs(struct apple_sleep *s)
{
	u32 val;

	writel(CLUSTER_ON, s->cluster + CLUSTER_E);
	s->cluster_e_ret = readl_poll_timeout_atomic(s->cluster + CLUSTER_E, val,
						     val & CLUSTER_E_READY, 1,
						     CLUSTER_TIMEOUT_US);
	writel(CLUSTER_ON, s->cluster + CLUSTER_P);

	writel(PWRGATE_ON, s->pwrgate + s->soc->pwrgate_wake[0]);
	writel(PWRGATE_ON, s->pwrgate + s->soc->pwrgate_wake[1]);

	sleep_ps_rewake(s);

	/* Only C2S turns AP_TMM on. */
	s->deep_ran = sleep_ps_woke_on(s, s->soc->ps.ap_tmm);

	val = readl(s->ps + s->soc->ps.sep);
	if (val & PS_DEV_DISABLE)
		writel(val & ~PS_DEV_DISABLE, s->ps + s->soc->ps.sep);

	/* The complex comes back on, and macOS turns it off. */
	writel(0, s->cluster + CLUSTER_AVE);
	s->cluster_ave_ret = readl_poll_timeout_atomic(s->cluster + CLUSTER_AVE, val, !val,
						       1, CLUSTER_TIMEOUT_US);

	writel(PWRGATE_ON, s->pwrgate + s->soc->pwrgate_cpm[0]);
	writel(PWRGATE_ON, s->pwrgate + s->soc->pwrgate_cpm[1]);

	sleep_dvfm_unpark(s);

	/* macOS stops it here. Keep a longer one over device resume, until .end. */
	apple_sleep_set_wd2(s, WDT_SUSPEND_SECS);

	/*
	 * What the commit lowered for a driver comes back before anything uses it, in
	 * macOS's order, measured about 14 ms after this point. With no driver, genpd's
	 * books decide, as they do for every other domain.
	 */
	if (s->pcie_ans_stopped)
		s->pcie_ans_on_ret = apple_sleep_raise_apcie(s);
	s->apcie_port_down = true;
	s->pins_lost = s->pins_saved_valid;

}

static void apple_sleep_return(struct apple_sleep *s)
{
	int i;

	/*
	 * The reset leaves the timer control at its EL2 default, without the event stream
	 * or EL0's counter. The timer driver's CPU PM exit puts it back too, later.
	 */
	write_sysreg(s->cntkctl, cntkctl_el1);
	isb();

	/* This core takes the AIC's IRQs, and a reset may have changed that. */
	write_sysreg_s(s->siq_cfg, SYS_APL_SIQ_CFG_EL1);

	spin_table_stop_finished();
	aic_sleep_restore();

	s->soc->ops->wake_regs(s);

	dcache_inval_poc((unsigned long)s->mbox, (unsigned long)(s->mbox + 1));
	s->resumes_after = READ_ONCE(s->mbox->resumes);

	if (s->soc->own_wake_masks)
		for (i = 0; i < WAKE_WORDS; i++)
			s->wake_status[i] = readl(s->wake + WAKE_STATUS(i));
	s->returned = true;
}

static void t8140_sleep_prepare_regs(struct apple_sleep *s)
{
	apple_sleep_match(s);

	/* Acknowledge a leftover done flag, absent on macOS. */
	if (readl(s->cluster_dcs) & CLUSTER_UP_DONE)
		writel(readl(s->cluster_dcs), s->cluster_dcs);
}

static int apple_sleep_smc_late_notice(struct apple_sleep *s)
{
	if (!s->soc->smc_policy)
		return 0;
	if (!s->smc.dev || !s->smc.late_notice)
		return -ENODEV;
	if (s->soc->early_context && (!s->smc.saved_ops || !s->smc.held))
		return -EBUSY;
	return s->smc.late_notice(s->smc.data, &s->smc_idle_sent, &s->smc_idle_ack);
}

/* Past the point of no return: one pmVC write alone, as macOS's after ANS. */
static int apple_sleep_smc_pmvc(struct apple_sleep *s)
{
	if (!s->smc.dev || !s->smc.held || !s->smc.saved_ops || !s->smc.saved_ops->pmvc)
		return -ENODEV;
	return s->smc.saved_ops->pmvc(s->smc.data);
}

static int apple_sleep_smc_commit_notice(struct apple_sleep *s)
{
	if (!s->smc.dev || !s->smc.held || !s->smc.saved_ops ||
	    !s->smc.saved_ops->commit_notice)
		return -ENODEV;
	return s->smc.saved_ops->commit_notice(s->smc.data,
		&s->smc_idle_sent, &s->smc_idle_ack);
}

static void apple_sleep_enter_cleanup(struct apple_sleep *s)
{
	if (s->regs_staged)
		s->soc->ops->undo_regs(s);
	s->regs_staged = false;
	apple_sleep_stray_restore(s);
	if (s->wake_masks_saved)
		sleep_wake_restore(s);
	s->wake_masks_saved = false;
	s->entry_prepared = false;
}

/*
 * T6050, in macOS's order on the M5 Pro: the display sets, then GFX and GFX1, the first always-on stop and so the
 * point of no return, then SEP; ANS, the PMP and the SMC follow as before. Always-on last, as
 * on the Neo, where the display also goes off before SEP.
 */
static void apple_sleep_display_then_gpu(struct apple_sleep *s)
{
	int ret;

	s->entry_committed = true;
	ret = s->soc->ops->display_stop(s);
	if (ret)
		panic("Apple committed display stop failed: %d", ret);
	apple_sleep_coprocs_stop(s, true);
}

/* One shared lifecycle; all returning checks precede enclave/final-stop. */
static int apple_sleep_enter_prepare(struct apple_sleep *s)
{
	const char *step = "SoC check";
	int ret;

	if (s->soc->ops->check) {
		ret = s->soc->ops->check(s);
		if (ret) {
			if (s->soc->regions)
				dev_info(s->dev, "refused at .enter: %s: %d\n", step, ret);
			return ret;
		}
	}
	ret = apple_sleep_refuse(s);
	if (ret)
		return ret;
	ret = apple_sleep_cpus_hold(s);
	if (!ret) {
		step = "display prepare";
		if (s->soc->regions)
			s->regs_staged = true;
		ret = s->soc->ops->display_off(s);
	}
	if (!ret && s->soc->ops->stage) {
		step = "stage";
		s->regs_staged = true; /* Partial stage must also be undone on failure. */
		ret = s->soc->ops->stage(s);
	}
	if (!ret) {
		step = "coprocessor checks";
		ret = apple_sleep_coprocs_check(s);
	}
	if (!ret && !s->soc->ops->commit_notice) {
		step = "SMC notice";
		ret = apple_sleep_smc_late_notice(s);
	}
	if (!ret && s->soc->ops->final_check) {
		step = "final check";
		ret = s->soc->ops->final_check(s);
	}
	if (!ret && pm_wakeup_pending()) {
		step = "wakeup pending";
		ret = -EBUSY;
	}
	if (ret && s->soc->regions)
		dev_info(s->dev, "refused at .enter: %s: %d\n", step, ret);
	if (!ret && s->soc->ops->display_stop) {
		apple_sleep_display_then_gpu(s);
		if (s->soc->ops->before_enclave)
			s->soc->ops->before_enclave(s);
		ret = apple_sleep_enclave_stop(s);
		if (ret)
			panic("Apple SEP refused after the always-on GPU stop: %d", ret);
	} else if (!ret) {
		ret = apple_sleep_enclave_stop(s);
	}
	if (ret) {
		dev_dbg(s->dev, "refused before any commit coprocessor stop: %d\n", ret);
		spin_table_unhold_parked();
		apple_sleep_enter_cleanup(s);
		return ret;
	}
	/* SEP success belongs to irreversible commitment, never cancellation. */
	s->entry_committed = true;
	/* No returning error path below this point. */
	if (!s->soc->ops->display_stop)
		apple_sleep_coprocs_stop(s, true);
	if (s->soc->own_wake_masks) {
		sleep_wake_arm(s);
		s->wake_masks_saved = true;
	}
	s->soc->ops->prepare_regs(s);
	s->regs_staged = true;
	apple_sleep_coprocs_stop(s, false);
	if (s->soc->ops->commit_notice) {
		ret = s->soc->ops->commit_notice(s);
		if (ret)
			panic("Apple committed SMC notice failed after peer stop: %d", ret);
	}
	if (!s->soc->regions)
		apple_sleep_usb_down(s);
	s->deep_once = READ_ONCE(sleep_deep_requested);
	if (s->deep_once)
		sleep_slp_ddr_set_deep_once(true);
	s->entry_prepared = true;
	return 0;
}

static int apple_sleep_enter_saved(unsigned long unused)
{
	struct apple_sleep *s = apple_sleep;
	int ret = s->soc->ops->cpu_prepare(s->mbox->boot_mpidr);

	if (ret)
		return ret;
	ret = apple_sleep_enter_prepare(s);
	if (ret)
		return ret;
	pm_set_resume_via_firmware();
	/* cpu_suspend has already saved boot context. Commit never returns. */
	return apple_sleep_commit(0);
}

static int apple_sleep_enter(suspend_state_t state)
{
	struct apple_sleep *s = apple_sleep;
	int ret;

	s->entry_committed = false;
	/* Its own cpu_suspend() can't nest in the early-context one below. */
	if (s->soc->ops->boot_cluster_cycle)
		s->soc->ops->boot_cluster_cycle(s);
	if (s->soc->early_context) {
		s->cntkctl = read_sysreg(cntkctl_el1);
		ret = cpu_suspend(0, apple_sleep_enter_saved);
	} else {
		ret = apple_sleep_enter_prepare(s);
		if (ret)
			return ret;
		s->cntkctl = read_sysreg(cntkctl_el1);
		pm_set_resume_via_firmware();
		ret = cpu_suspend(0, apple_sleep_commit);
	}
	if (ret && s->entry_committed)
		panic("Apple committed sleep returned without physical wake: %d", ret);
	if (!ret) {
		apple_sleep_return(s);
		if (s->deep_once)
			sleep_slp_ddr_set_deep_once(false);
	}
	if (s->entry_prepared)
		apple_sleep_enter_cleanup(s);
	if (s->soc->ops->cpu_restore)
		s->soc->ops->cpu_restore(s->mbox->boot_mpidr);
	return ret;
}

/* From here to .end a hang resets the machine. Nobody has to be there to recover it. */
static int apple_sleep_begin(suspend_state_t state)
{
	int ret;
	/* The last sleep's cut is its port controller's to take, by its .complete. */
	WRITE_ONCE(sleep_debug_link_cut, false);
	WRITE_ONCE(sleep_lid_closed, -ENODATA);
	WRITE_ONCE(sleep_radio_woke, -ENODATA);
	/* The PM core also calls .begin for s2idle when no s2idle ops are installed. */
	if (state != PM_SUSPEND_MEM)
		return 0;

	ret = apple_sleep_profile_check(apple_sleep);
	if (ret)
		return ret;

	if (apple_sleep->soc->ops->begin) {
		ret = apple_sleep->soc->ops->begin(apple_sleep);
		if (ret)
			return ret;
	}

	if (apple_sleep) {
		apple_sleep->apcie_port_stopped = false;
		apple_sleep->usb_down_ran = false;
		apple_sleep->smc_woke_alone = false;
		sleep_pins_save(apple_sleep);
	}
	apple_sleep_set_wd2(apple_sleep, WDT_SUSPEND_SECS);
	pm_set_suspend_via_firmware();
	sleep_ans_late_open();

	return 0;
}

static void apple_sleep_end(void)
{
	WRITE_ONCE(sleep_deep_requested, false);
	sleep_ans_late_release(true);
	sleep_v27_scope_end();
	if (pm_suspend_via_firmware() && apple_sleep->soc->ops->finish)
		apple_sleep->soc->ops->finish(apple_sleep);
	if (pm_suspend_via_firmware())
		writel(0, apple_sleep->wdt + WDT_WD2_CTL);
}

/*
 * macOS turns this complex off at boot and again at the end of each wake, so at the commit
 * its word is already 0 and the engine has no sequence to run for it. Turning it off at the
 * commit instead puts that sequence, with its untimed wait, right before the CPU complexes'.
 */
static void apple_sleep_ave_off(struct apple_sleep *s)
{
	u32 val;
	int ret;

	/* Target 0, and the leftover done flag from iBoot's up, as XNU's first write does. */
	writel(CLUSTER_UP_DONE, s->cluster + CLUSTER_AVE);
	ret = readl_poll_timeout_atomic(s->cluster + CLUSTER_AVE, val, !val, 1, CLUSTER_TIMEOUT_US);
	if (ret)
		dev_warn(s->dev, "AVE complex didn't go off: %#x\n", val);
}

/*
 * The encoders' system power domains (VENC0_SYS, VENC1_SYS) only power on with the complex
 * up: with it off, VENC0_SYS refuses its target and the complex is left pending, and with
 * one of them on the complex doesn't go off. genpd ignores a failed power-on in the noirq
 * resume, so without the parent link the first wake raised VENC0_SYS under a complex that
 * .prepare and the wake had turned off, and apple-dart's restore of the encoder's DARTs in
 * that unpowered block reset the SoC. As their parent, the complex comes on before either
 * of them and goes off after both: off at boot and after each wake, on while an encoder is
 * in use, as macOS has it. The PMC's cluster vote for the encoders (VENC-CLSTR-V) is the
 * same: raised with the complex off, the complex's next power-up never completes, and with
 * the complex off first the vote doesn't drop. So it's a subdomain too.
 */
static int apple_sleep_ave_pd_on(struct generic_pm_domain *pd)
{
	struct apple_sleep *s = container_of(pd, struct apple_sleep, ave_pd);
	u32 val;
	int ret;

	/* A raise over a lower still in progress never completes. */
	ret = readl_poll_timeout_atomic(s->cluster + CLUSTER_AVE, val, !(val & CLUSTER_PENDING),
					1, CLUSTER_TIMEOUT_US);
	if (ret) {
		dev_err(s->dev, "AVE complex still busy: %#x\n", val);
		return ret;
	}

	writel(CLUSTER_ON, s->cluster + CLUSTER_AVE);
	ret = readl_poll_timeout_atomic(s->cluster + CLUSTER_AVE, val, val & CLUSTER_UP_DONE, 1,
					CLUSTER_TIMEOUT_US);
	if (ret)
		dev_err(s->dev, "AVE complex didn't come on: %#x\n", val);

	return ret;
}

static int apple_sleep_ave_pd_off(struct generic_pm_domain *pd)
{
	struct apple_sleep *s = container_of(pd, struct apple_sleep, ave_pd);
	unsigned int i;
	u32 val;
	int ret;

	/*
	 * Each encoder domain reads actual 0 a moment before its switch is done. Lowered
	 * before that, the complex stays pending and its next raise never completes, so wait
	 * for BUSY to clear too, as macOS waits for a lowered word to read 0 in its low bits.
	 */
	for (i = 0; i < s->soc->nave_domains; i++) {
		if (!(s->ave_linked & BIT(i)))
			continue;
		ret = readl_poll_timeout_atomic(s->ps + s->soc->ave_domains[i], val,
						!(val & (PS_BUSY | PS_ACTUAL)), 1,
						PS_OFF_TIMEOUT_US);
		if (ret) {
			dev_err(s->dev, "encoder domain %#x not off: %#x\n",
				s->soc->ave_domains[i], val);
			return -EBUSY;
		}
	}

	/* Target 0, and the done flag of its last power-up. */
	writel(CLUSTER_UP_DONE, s->cluster + CLUSTER_AVE);
	ret = readl_poll_timeout_atomic(s->cluster + CLUSTER_AVE, val, !val, 1,
					CLUSTER_TIMEOUT_US);
	if (ret)
		dev_err(s->dev, "AVE complex didn't go off: %#x\n", val);

	return ret;
}

/* The PMGR node "ps" maps */
static struct device_node *apple_sleep_ps_pmgr(struct apple_sleep *s)
{
	struct device_node *np;
	struct resource res;

	for_each_compatible_node(np, NULL, "apple,t8103-pmgr")
		if (!of_address_to_resource(np, 0, &res) && res.start == s->ps_res.start)
			return np;

	return NULL;
}

static void apple_sleep_ave_pd_remove(void *data)
{
	struct apple_sleep *s = data;
	struct of_phandle_args parent = { .np = s->dev->of_node }, child = {};
	unsigned int i;

	for (i = 0; i < s->ave_nchildren; i++) {
		child.np = s->ave_children[i];
		of_genpd_remove_subdomain(&parent, &child);
		of_node_put(s->ave_children[i]);
	}
	s->ave_nchildren = 0;
	of_genpd_del_provider(s->dev->of_node);
	pm_genpd_remove(&s->ave_pd);
	s->ave_pd_owned = false;
}

/* The enabled encoder domains, and with any of them the PMC's encoder cluster vote. */
static void apple_sleep_ave_find(struct apple_sleep *s)
{
	const struct apple_sleep_soc *soc = s->soc;
	struct device_node *pmgr, *pmc, *np;
	unsigned int i;
	u32 val;

	pmgr = apple_sleep_ps_pmgr(s);
	if (!pmgr)
		return;
	for_each_available_child_of_node(pmgr, np) {
		for (i = 0; i < soc->nave_domains; i++) {
			if (of_property_read_u32(np, "reg", &val) || val != soc->ave_domains[i] ||
			    s->ave_nchildren == ARRAY_SIZE(s->ave_children))
				continue;
			s->ave_linked |= BIT(i);
			s->ave_children[s->ave_nchildren++] = of_node_get(np);
		}
	}
	of_node_put(pmgr);
	if (!s->ave_nchildren || !soc->ave_pmc)
		return;

	for_each_compatible_node(pmc, NULL, soc->ave_pmc) {
		if (!of_device_is_available(pmc))
			continue;
		for_each_available_child_of_node(pmc, np) {
			if (of_property_read_u32(np, "apple,cluster-bit", &val) ||
			    val != soc->ave_pmc_cluster ||
			    s->ave_nchildren == ARRAY_SIZE(s->ave_children))
				continue;
			s->ave_children[s->ave_nchildren++] = of_node_get(np);
		}
	}
}

/* The complex as the parent of the encoders' domains, or left to .prepare if none is enabled. */
static int apple_sleep_ave_pd_init(struct apple_sleep *s)
{
	struct of_phandle_args parent = { .np = s->dev->of_node }, child = {};
	unsigned int i;
	int ret;

	apple_sleep_ave_find(s);
	if (!s->ave_nchildren)
		return 0;

	s->ave_pd.name = "ave_complex";
	s->ave_pd.power_on = apple_sleep_ave_pd_on;
	s->ave_pd.power_off = apple_sleep_ave_pd_off;
	s->ave_pd.flags = GENPD_FLAG_IRQ_SAFE | GENPD_FLAG_NO_STAY_ON | GENPD_FLAG_NO_SYNC_STATE;
	ret = pm_genpd_init(&s->ave_pd, NULL,
			    !(readl(s->cluster + CLUSTER_AVE) & CLUSTER_ON));
	if (!ret) {
		ret = of_genpd_add_provider_simple(s->dev->of_node, &s->ave_pd);
		if (ret)
			pm_genpd_remove(&s->ave_pd);
	}
	if (ret) {
		for (i = 0; i < s->ave_nchildren; i++)
			of_node_put(s->ave_children[i]);
		s->ave_nchildren = 0;
		s->ave_linked = 0;
		return ret;
	}
	s->ave_pd_owned = true;
	ret = devm_add_action_or_reset(s->dev, apple_sleep_ave_pd_remove, s);
	if (ret)
		return ret;

	/* -EPROBE_DEFER until the PMGR's and the PMC's drivers have their domains */
	for (i = 0; i < s->ave_nchildren; i++) {
		child.np = s->ave_children[i];
		ret = of_genpd_add_subdomain(&parent, &child);
		if (ret)
			return ret;
	}

	return 0;
}

static int apple_sleep_smc_scope_begin(void)
{
	struct apple_sleep *s = apple_sleep;
	int ret = 0;

	if (!s->soc->early_context || !s->soc->smc_policy)
		return 0;
	mutex_lock(&apple_sleep_smc_lock);
	if (!s->smc.saved_ops || !s->smc.dev) {
		ret = -ENODEV;
		goto out;
	}
	/* Unregister runs with device_lock held. Never block on the inverse
	 * order: refusal is safe here, before CPUs/IRQs/parked cores go down.
	 */
	if (!device_trylock(s->smc.dev)) {
		ret = -EBUSY;
		goto out;
	}
	s->smc.held = true;
	mutex_unlock(&apple_sleep_smc_lock);
	ret = s->smc.saved_ops->begin(s->smc.data);
	if (ret) {
		s->smc.held = false;
		device_unlock(s->smc.dev);
	}
	return ret;
out:
	mutex_unlock(&apple_sleep_smc_lock);
	return ret;
}

static void apple_sleep_smc_scope_end(void)
{
	struct apple_sleep *s = apple_sleep;
	int ret;

	if (!s->smc.held)
		return;
	ret = s->smc.saved_ops->end(s->smc.data);
	if (ret)
		dev_err(s->smc.dev, "SMC state not restored after the sleep: %d\n", ret);
	s->smc.held = false;
	device_unlock(s->smc.dev);
}

static int apple_sleep_late_scope_begin(void)
{
	const char *step = "V27 display scope";
	int ret = sleep_v27_scope_begin();

	if (!ret) {
		step = "SMC scope";
		ret = apple_sleep_smc_scope_begin();
	}
	if (!ret) {
		step = "ANS reservation";
		ret = sleep_ans_late_reserve();
	}
	if (ret && apple_sleep->soc->regions)
		dev_info(apple_sleep->dev, "refused at .prepare_late: %s: %d\n", step, ret);
	if (ret) {
		apple_sleep_smc_scope_end();
		sleep_v27_scope_end();
	}
	return ret;
}

static void apple_sleep_late_scope_finish(void)
{
	/* .finish also runs when noirq suspend fails before platform .wake. */
	apple_sleep_smc_scope_end();
	sleep_v27_scope_end();
	/* After device early resume, which consumed the ANS receipt. */
	sleep_ans_late_release(false);
}

static int apple_sleep_prepare(void)
{
	struct apple_sleep *s = apple_sleep;
	int i;

	/* Every device .suspend has run: no owner registers after this. */
	sleep_ans_late_close();
	/* With an encoder's domain linked, genpd has it off once the encoders are. */
	if (!s->soc->regions && !s->ave_pd_owned)
		apple_sleep_ave_off(s);

	for (i = 0; i < s->soc->display_count; i++) {
		struct platform_device *pdev;

		s->display[i].bound = false;
		if (!s->display[i].np)
			continue;
		pdev = of_find_device_by_node(s->display[i].np);
		if (pdev) {
			s->display[i].bound = device_is_bound(&pdev->dev);
			put_device(&pdev->dev);
		}
	}
	apple_sleep_gate_prepare(s);

	return 0;
}

/* Only the SMC's sources woke the SoC. */
static bool apple_sleep_smc_woke_alone(struct apple_sleep *s)
{
	const struct apple_sleep_wake_name *names = s->soc->wake_names;
	u32 smc[WAKE_WORDS] = {};
	bool any = false;
	int i;

	/* SMC.CPUWakeupAP and SMC.OutboxNotEmpty, by their names. */
	for (i = 0; i < s->soc->nwake_names; i++) {
		u32 id = names[i].id;

		if (strstarts(names[i].name, "SMC.") && id / 32 < WAKE_WORDS)
			smc[id / 32] |= BIT(id % 32);
	}
	for (i = 0; i < WAKE_WORDS; i++) {
		u32 woke = s->wake_status[i] & s->wake_armed[i];

		if (woke & ~smc[i])
			return false;
		any |= !!woke;
	}

	return any;
}

static void apple_sleep_report(void)
{
	struct apple_sleep *s = apple_sleep;
	const struct apple_sleep_wake_name *names = s->soc->wake_names;
	int i;

	if (s->soc->ops->finish)
		s->soc->ops->finish(s);
	apple_sleep_smc_scope_end();
	sleep_v27_scope_end();
	s->smc_woke_alone = false;
	if (!s->returned)
		return;
	s->returned = false;
	s->smc_woke_alone = apple_sleep_smc_woke_alone(s);

	dev_info(s->dev, "back from sleep, boot object resumes %llu -> %llu\n",
		 s->resumes_before, s->resumes_after);
	for (i = 0; i < s->ps_unlatched_count; i++)
		dev_warn(s->dev, "power word %#x was off with BUSY latched at the commit: %#x\n",
			 s->ps_unlatched[i].offset, s->ps_unlatched[i].val);
	if (s->deep_once)
		dev_info(s->dev, "took the deep path: a power word stayed latched or stuck\n");
	if (s->deep_ran)
		dev_info(s->dev, "the deep path ran\n");
	for (i = 0; i < s->soc->nwake_names; i++) {
		u32 id = names[i].id;

		if (s->wake_status[id / 32] & BIT(id % 32))
			dev_info(s->dev, "wake source: %s\n", names[i].name);
	}
	if (!s->soc->regions)
		sleep_ps_rewake_report(s);

	for (i = 0; i < SLEEP_COPROCS; i++) {
		struct apple_sleep_coproc *c = &apple_sleep_coprocs[i];

		if (c->dev && c->stopped && c->stop_ret)
			dev_err(s->dev, "%s stop reported %d; physical sleep completed\n",
				dev_name(c->dev), c->stop_ret);
	}
	if (s->cpus_stop_ret)
		dev_err(s->dev, "other CPUs did not stop: %d\n", s->cpus_stop_ret);
	if (s->cluster_e_ret || s->cluster_ave_ret || s->cluster_ave_commit_ret)
		dev_err(s->dev, "cluster power timed out: E %d AVE %d, AVE at the commit %d\n",
			s->cluster_e_ret, s->cluster_ave_ret, s->cluster_ave_commit_ret);
	if (s->ans_off_ret)
		dev_err(s->dev, "ANS did not read off at the commit: %d\n", s->ans_off_ret);
	if (s->usb_down_ret)
		dev_err(s->dev, "debug link: the stop failed: %d\n", s->usb_down_ret);
	if (s->pcie_ans_stopped && s->pcie_ans_on_ret)
		dev_err(s->dev, "ANS's PCIe block did not come up after the wake: %d\n",
			s->pcie_ans_on_ret);
}

static const struct platform_suspend_ops apple_sleep_ops = {
	.valid = suspend_valid_only_mem,
	.begin = apple_sleep_begin,
	.prepare = apple_sleep_prepare,
	.prepare_late = apple_sleep_late_scope_begin,
	.finish = apple_sleep_late_scope_finish,
	.enter = apple_sleep_enter,
	.wake = apple_sleep_report,
	.end = apple_sleep_end,
};

static int apple_sleep_read_wake_sources(struct apple_sleep *s)
{
	struct device_node *np = s->dev->of_node;
	int count;

	count = of_property_read_variable_u32_array(np, "apple,wake-enable", s->wake_enable,
						    1, WAKE_SOURCES_MAX);
	if (count < 0)
		return dev_err_probe(s->dev, count, "no wake source enabled\n");
	s->wake_enable_count = count;

	return 0;
}

static int apple_sleep_map_displays(struct apple_sleep *s)
{
	int i;

	for (i = 0; i < s->soc->display_count; i++) {
		struct device_node *np;
		int index;

		np = of_parse_phandle(s->dev->of_node, "apple,display-coprocessors", i);
		if (!np)
			return dev_err_probe(s->dev, -ENODEV, "no display coprocessor %d\n", i);
		s->display[i].np = np;

		index = of_property_match_string(np, "reg-names", "coproc");
		if (index < 0)
			return dev_err_probe(s->dev, index, "%pOF has no coproc registers\n", np);
		s->display[i].asc = of_iomap(np, index);
		if (!s->display[i].asc)
			return dev_err_probe(s->dev, -ENOMEM, "cannot map %pOF\n", np);
	}

	return 0;
}

static void __iomem *apple_sleep_map(struct device *dev, const char *name)
{
	struct device_node *np = dev->of_node;
	struct resource res;
	void __iomem *base;
	int index;

	index = of_property_match_string(np, "reg-names", name);
	if (index < 0)
		return IOMEM_ERR_PTR(index);
	if (of_address_to_resource(np, index, &res))
		return IOMEM_ERR_PTR(-EINVAL);

	/* Shared with the drivers of these blocks, so not claimed. */
	base = devm_ioremap(dev, res.start, resource_size(&res));
	if (!base)
		return IOMEM_ERR_PTR(-ENOMEM);

	return base;
}

/* The register blocks every sleep writes, by their reg-names. */
static const struct {
	const char *name;
	size_t offset;
} apple_sleep_regs[] = {
	{ "ps", offsetof(struct apple_sleep, ps) },
	{ "pwrgate", offsetof(struct apple_sleep, pwrgate) },
	{ "cpu-start", offsetof(struct apple_sleep, cpu_start) },
	{ "cllt", offsetof(struct apple_sleep, cllt) },
	{ "cluster", offsetof(struct apple_sleep, cluster) },
	{ "commit", offsetof(struct apple_sleep, commit) },
	{ "wdt", offsetof(struct apple_sleep, wdt) },
	{ "aon-misc", offsetof(struct apple_sleep, aon_misc) },
	{ "wake", offsetof(struct apple_sleep, wake) },
	{ "pmc-dcp", offsetof(struct apple_sleep, pmc_dcp) },
	{ "spmi", offsetof(struct apple_sleep, spmi) },
	{ "pmc-voters", offsetof(struct apple_sleep, pmc_voters) },
	{ "dcs", offsetof(struct apple_sleep, dcs) },
	{ "ps-aon", offsetof(struct apple_sleep, ps_aon) },
	{ "spmi-a0", offsetof(struct apple_sleep, spmi_a0) },
	{ "clk-mux", offsetof(struct apple_sleep, clk_mux) },
	{ "mcc0", offsetof(struct apple_sleep, mcc[0]) },
	{ "mcc1", offsetof(struct apple_sleep, mcc[1]) },
	{ "mcc2", offsetof(struct apple_sleep, mcc[2]) },
	{ "ptd", offsetof(struct apple_sleep, ptd) },
	{ "clk-perf", offsetof(struct apple_sleep, clk_perf) },
	{ "cluster-dcs", offsetof(struct apple_sleep, cluster_dcs) },
	{ "amcc0", offsetof(struct apple_sleep, amcc[0]) },
	{ "amcc1", offsetof(struct apple_sleep, amcc[1]) },
	{ "amcc2", offsetof(struct apple_sleep, amcc[2]) },
	{ "amcc3", offsetof(struct apple_sleep, amcc[3]) },
};

static int apple_sleep_map_regs(struct apple_sleep *s)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(apple_sleep_regs); i++) {
		void __iomem **field = (void __iomem **)((u8 *)s + apple_sleep_regs[i].offset);

		*field = apple_sleep_map(s->dev, apple_sleep_regs[i].name);
		if (IS_ERR(*field))
			return dev_err_probe(s->dev, PTR_ERR(*field), "no %s registers\n",
					     apple_sleep_regs[i].name);
	}

	for (i = 0; i < s->soc->cpu_clusters; i++) {
		s->dvfm[i] = apple_sleep_map(s->dev, s->soc->dvfm_names[i]);
		if (IS_ERR(s->dvfm[i]))
			return PTR_ERR(s->dvfm[i]);
	}

	return 0;
}

static int apple_sleep_map_mailbox(struct apple_sleep *s)
{
	struct device_node *chosen;
	u64 pa;
	int ret;

	chosen = of_find_node_by_path("/chosen");
	if (!chosen)
		return -ENODEV;
	ret = of_property_read_u64(chosen, "aurora,s2r-mailbox", &pa);
	of_node_put(chosen);
	if (ret) {
		dev_info(s->dev, "no aurora,s2r-mailbox from the boot object: no deep sleep\n");
		return -ENODEV;
	}

	s->mbox = devm_memremap(s->dev, pa, sizeof(*s->mbox), MEMREMAP_WB);
	if (IS_ERR(s->mbox))
		return PTR_ERR(s->mbox);

	/* aura wrote it with the MMU off, so nothing of it can be in a cache. */
	if (s->mbox->magic != AURA_S2R_MAGIC)
		return dev_err_probe(s->dev, -EINVAL, "bad mailbox magic %llx\n",
				     s->mbox->magic);
	/* Probe can run on any CPU. Logical CPU 0 is always the boot CPU. */
	if (s->mbox->boot_mpidr != cpu_logical_map(0))
		return dev_err_probe(s->dev, -EINVAL,
				     "mailbox is for CPU %llx, not this boot CPU\n",
				     s->mbox->boot_mpidr);

	return 0;
}

static struct apple_tunable *apple_sleep_apcie_tunable(struct apple_sleep *s,
						       struct device_node *np,
						       const char *name, const char *prop)
{
	int idx = of_property_match_string(np, "reg-names", name);
	struct apple_tunable *t;
	struct resource res;

	if (idx < 0 || of_address_to_resource(np, idx, &res))
		return NULL;
	t = devm_apple_tunable_parse(s->dev, np, prop, &res);

	return IS_ERR(t) ? NULL : t;
}

static void __iomem *apple_sleep_apcie_block(struct apple_sleep *s, struct device_node *np,
					     const char *name, const char *prop,
					     struct apple_tunable **tunable)
{
	int idx = of_property_match_string(np, "reg-names", name);

	if (idx < 0)
		return NULL;
	if (prop) {
		*tunable = apple_sleep_apcie_tunable(s, np, name, prop);
		if (!*tunable)
			return NULL;
	}

	return of_iomap(np, idx);
}

/*
 * Its registers go with PCIe's node, which a test tree may have disabled, and a loader
 * publishes the tunables there. Missing ones only refuse a real sleep with ANS stopped.
 */
static void apple_sleep_map_apcie(struct apple_sleep *s)
{
	struct device_node *np = of_parse_phandle(s->dev->of_node, "apple,apcie", 0);
	struct device_node *engine;

	if (!np)
		return;
	s->apcie_axi = apple_sleep_apcie_block(s, np, "axi2af", "apple,tunable-axi2af",
					       &s->apcie_axi_tunable);
	s->apcie_cio3pll = apple_sleep_apcie_block(s, np, "cio3pll", "apple,tunable-cio3pll",
						   &s->apcie_cio3pll_tunable);
	s->apcie_clkgen = apple_sleep_apcie_block(s, np, "clkgen", "apple,tunable-clkgen",
						  &s->apcie_clkgen_tunable);
	s->apcie_phy = apple_sleep_apcie_block(s, np, "phy-block", "apple,tunable-phy",
					       &s->apcie_phy_tunable);
	s->apcie_rc = apple_sleep_apcie_block(s, np, "rc", NULL, NULL);
	s->apcie_phy_ip = apple_sleep_apcie_block(s, np, "phy-ip", "apple,tunable-phy-ip-pll",
						  &s->apcie_phy_ip_pll);
	s->apcie_phy_ip_lanes = apple_sleep_apcie_tunable(s, np, "phy-ip",
							  "apple,tunable-phy-ip-auspma");
	if (!s->apcie_axi || !s->apcie_cio3pll || !s->apcie_clkgen || !s->apcie_phy ||
	    !s->apcie_rc || !s->apcie_phy_ip || !s->apcie_phy_ip_lanes) {
		dev_info(s->dev,
			 "no PCIe tunables for ANS: suspend-to-RAM with it stopped will be refused\n");
		s->apcie_phy_tunable = NULL;
		s->apcie_phy = NULL;
		s->apcie_clkgen = NULL;
		s->apcie_axi = NULL;
	}

	s->apcie_port = apple_sleep_apcie_block(s, np, "port0", NULL, NULL);
	s->apcie_port_phy = apple_sleep_apcie_block(s, np, "phy0", NULL, NULL);
	engine = of_parse_phandle(np, "apple,piodma", 0);
	if (engine) {
		s->apcie_engine = apple_sleep_apcie_block(s, engine, "engine", NULL, NULL);
		s->apcie_fabric = apple_sleep_apcie_block(s, engine, "fabric", NULL, NULL);
		of_node_put(engine);
	}
	of_node_put(np);
}

/* The radio port's PERST# on the AP pins @np, from the ports of the SoC's enabled PCIe host. */
static void apple_sleep_find_radio_perst(struct apple_sleep *s, struct device_node *np)
{
	struct device_node *host, *port;
	struct of_phandle_args args;

	if (!s->soc->radio_pcie)
		return;
	host = of_find_compatible_node(NULL, NULL, s->soc->radio_pcie);
	if (!host)
		return;
	if (of_device_is_available(host)) {
		for_each_available_child_of_node(host, port) {
			if (of_parse_phandle_with_args(port, "reset-gpios", "#gpio-cells", 0,
						       &args))
				continue;
			if (args.np == np && args.args_count >= 2 && args.args[0] < s->npins &&
			    !test_bit(args.args[0], s->pins_skip)) {
				s->radio_perst = args.args[0];
				s->radio_perst_low = args.args[1] & GPIO_ACTIVE_LOW;
			}
			of_node_put(args.np);
		}
	}
	of_node_put(host);
}

/* macOS leaves the skipped pins alone, and reading one may fault. */
static void apple_sleep_map_pins(struct apple_sleep *s)
{
	struct device_node *np = of_parse_phandle(s->dev->of_node, "apple,pinctrl", 0);
	u32 npins, pin;
	int i, n;

	s->radio_perst = -1;
	if (!np)
		return;
	if (!of_device_is_available(np) || of_property_read_u32(np, "apple,npins", &npins) ||
	    !npins || npins > 512)
		goto out;
	/* Without the pins to leave alone none is saved: reading some of them faults. */
	if (s->soc->regions && !of_property_present(s->dev->of_node, "apple,pinctrl-skip-pins"))
		goto out;
	s->pins_saved = devm_kcalloc(s->dev, npins, sizeof(*s->pins_saved), GFP_KERNEL);
	s->pins_skip = devm_bitmap_zalloc(s->dev, npins, GFP_KERNEL);
	if (!s->pins_saved || !s->pins_skip)
		goto out;
	n = of_property_count_u32_elems(s->dev->of_node, "apple,pinctrl-skip-pins");
	for (i = 0; i < n; i++)
		if (!of_property_read_u32_index(s->dev->of_node, "apple,pinctrl-skip-pins", i,
						&pin) && pin < npins)
			set_bit(pin, s->pins_skip);
	s->pin_irq_groups = of_irq_count(np);
	s->npins = npins;
	s->pins = of_iomap(np, 0);
	if (s->pins)
		apple_sleep_find_radio_perst(s, np);
out:
	of_node_put(np);
}

static void apple_sleep_map_debug_port(struct apple_sleep *s)
{
	struct device_node *np = of_parse_phandle(s->dev->of_node, "apple,debug-port", 0);
	struct device_node *bus;

	if (!np)
		return;
	bus = of_get_parent(np);
	if (bus && !of_property_read_u32_index(np, "reg", 0, &s->hpm_sid))
		s->hpm_spmi = of_iomap(bus, 0);
	of_node_put(bus);
	/* Kept, for as long as the driver: no unbind. */
	s->hpm_np = np;
}

/* The books kept for each word of the SoC's lists */
static int apple_sleep_alloc_books(struct apple_sleep *s)
{
	const struct apple_sleep_soc *soc = s->soc;
	unsigned int n = soc->ps_all.count, naon = soc->ps_aon_all.count;
	struct device *dev = s->dev;

	/* One bit, and one gate, for each */
	if (soc->nstray > BITS_PER_LONG || soc->ngate_coprocs > ARRAY_SIZE(s->gate))
		return dev_err_probe(dev, -EINVAL, "the SoC's lists are too long\n");

	s->ps_on_saved = devm_kcalloc(dev, soc->ps_macos_on.count, sizeof(u32), GFP_KERNEL);
	s->ps_off_saved = devm_kcalloc(dev, soc->ps_macos_off.count, sizeof(u32), GFP_KERNEL);
	s->ps_aon_saved = devm_kcalloc(dev, soc->ps_aon_macos_off.count, sizeof(u32),
				       GFP_KERNEL);
	s->ps_aon_auto_saved = devm_kcalloc(dev, soc->ps_aon_macos_no_auto.count, sizeof(u32),
					    GFP_KERNEL);
	s->ps_commit = devm_kcalloc(dev, n, sizeof(u32), GFP_KERNEL);
	s->ps_woke = devm_kcalloc(dev, n, sizeof(u32), GFP_KERNEL);
	s->ps_late = devm_kcalloc(dev, n, sizeof(bool), GFP_KERNEL);
	s->ps_done = devm_kcalloc(dev, n, sizeof(bool), GFP_KERNEL);
	s->ps_aon_commit = devm_kcalloc(dev, naon, sizeof(u32), GFP_KERNEL);
	s->ps_aon_woke = devm_kcalloc(dev, naon, sizeof(u32), GFP_KERNEL);
	s->ps_aon_late = devm_kcalloc(dev, naon, sizeof(bool), GFP_KERNEL);
	s->ps_aon_done = devm_kcalloc(dev, naon, sizeof(bool), GFP_KERNEL);
	if (!s->ps_on_saved || !s->ps_off_saved || !s->ps_aon_saved || !s->ps_aon_auto_saved ||
	    !s->ps_commit || !s->ps_woke || !s->ps_late || !s->ps_done || !s->ps_aon_commit ||
	    !s->ps_aon_woke || !s->ps_aon_late || !s->ps_aon_done)
		return -ENOMEM;

	return 0;
}

#include "sleep-t6050.h"

/* ANS's sleep owner on T6050: registered at its .suspend, its final stop in the last steps. */
int apple_sleep_ans_m5_register(struct device *dev, void __iomem *asc,
	int (*check)(void *), int (*stop)(void *), void *data)
{
	struct apple_sleep *s = apple_sleep;
	bool late = sleep_ans_late_selected();
	int ret;

	if (!late || !s || s->soc != &t6050_sleep_soc)
		return -EOPNOTSUPP;
	if (!check || !stop || !asc ||
	    !of_device_is_compatible(dev->of_node, "apple,t6050-nvme-ans2"))
		return -EINVAL;
	if (late) {
		guard(mutex)(&apple_sleep_coprocs_lock);
		/* Only from an open attempt's own .suspend, never cold probe. */
		if (!sleep_ans_late.open || sleep_ans_late.leaving || sleep_ans_late.dev)
			return -EBUSY;
		sleep_ans_late.dev = dev;
	}
	ret = apple_sleep_coproc_register(dev, asc, true, stop, data);
	if (!ret) {
		ret = apple_sleep_coproc_set_last_check(dev, check);
		if (ret)
			apple_sleep_coproc_unregister(dev);
	}
	if (ret && late) {
		guard(mutex)(&apple_sleep_coprocs_lock);
		if (sleep_ans_late.dev == dev)
			sleep_ans_late.dev = NULL;
	}
	return ret;
}
EXPORT_SYMBOL_GPL(apple_sleep_ans_m5_register);

int apple_sleep_ans_m5_preflight(struct device *dev)
{
	struct apple_sleep *s = apple_sleep;
	struct apple_sleep_coproc *c;
	unsigned int i;

	/* Called noirq: PM owns registration/lifetimes; acquire no sleeping lock. */
	if (!sleep_ans_late_selected() || !s || s->soc != &t6050_sleep_soc || !s->region ||
	    !s->region->snapshot_valid)
		return -EOPNOTSUPP;
	/* The entry boundary: the attempt's own task, its reserved owner, late scope armed. */
	if (sleep_ans_late_selected() &&
	    (READ_ONCE(sleep_ans_late.task) != current || !READ_ONCE(sleep_ans_late.reserved) ||
	     READ_ONCE(sleep_ans_late.dev) != dev || !s->smc.held))
		return -EBUSY;
	c = apple_sleep_coproc_find(dev);
	if (!c || !c->pcie_ans || !c->ready || c->stopped)
		return -EBUSY;
	for (i = 0; i < ARRAY_SIZE(t6050_ans_storage_ps); i++) {
		u32 word = SLEEP_REGION_WORD(T6050_PS1, t6050_ans_storage_ps[i]);
		int n = t6050_ans_storage_index(s, word);
		u32 value;

		if (n < 0 || test_bit(n, s->region->driver_down) ||
		    test_bit(n, s->region->manager_down))
			return -EBUSY;
		value = readl(sleep_region_reg(s, word));
		/*
		 * On, or under auto power at any actual: the NVMe node has no power domains on
		 * T6050, so the idle storage PHY gates itself, as on macOS until its late lowering.
		 */
		if ((value & (PS_TARGET | PS_ACTUAL)) == (PS_TARGET | PS_ACTUAL))
			continue;
		if (FIELD_GET(PS_TARGET, value) == 0xf && !(value & PS_DEV_DISABLE) &&
		    (value & PS_AUTO_ENABLE))
			continue;
		dev_info(s->dev, "ANS storage word %#x not on: %#x\n", t6050_ans_storage_ps[i], value);
		return -EIO;
	}
	return 0;
}
EXPORT_SYMBOL_GPL(apple_sleep_ans_m5_preflight);

int apple_sleep_ans_m5_domains_ready(struct device *dev)
{
	struct apple_sleep *s = apple_sleep;
	struct apple_sleep_coproc *c;

	if (!sleep_ans_late_selected() || !s || s->soc != &t6050_sleep_soc ||
	    !pm_resume_via_firmware() ||
	    s->resumes_after <= s->resumes_before)
		return -EOPNOTSUPP;
	/* The reserved owner's receipt, valid until .finish releases it. */
	if (sleep_ans_late_selected() &&
	    (!READ_ONCE(sleep_ans_late.reserved) || READ_ONCE(sleep_ans_late.dev) != dev))
		return -EIO;
	c = apple_sleep_coproc_find(dev);
	if (!c || !c->pcie_ans || !c->stopped || !s->pcie_ans_stopped)
		return -EIO;
	return s->ans_m5_wake_error;
}
EXPORT_SYMBOL_GPL(apple_sleep_ans_m5_domains_ready);


/* The display sleep's read-only power-provider handoff. The region
 * manager alone restores saved SYS/FE/CPU and DISP gate values. No consumer
 * borrows a raw PMGR base or calls T8140's standalone reset on T6050.
 */
int apple_sleep_display_v27_power_ready(struct device *dev)
{
	struct apple_sleep *s = apple_sleep;
	unsigned int d, j;

	if (!s || !s->soc->regions || !s->region ||
	    !of_machine_is_compatible("apple,j714s") || s->soc->display_count != 5)
		return -EOPNOTSUPP;
	if (s->region->late_errors || s->region->display_lowered)
		return -EIO;
	for (d = 0; d < s->soc->display_count; d++) {
		if (s->display[d].np != dev->of_node)
			continue;
		if (!s->display[d].asc || !device_is_bound(dev))
			return -ENODEV;
		for (j = 0; j < 3; j++) {
			u32 value = readl(sleep_region_reg(s, s->soc->ps.disp[d][j]));

			if (!t6050_display_word_ready(d, value)) {
				dev_info(s->dev, "display %u: power word %u reads %#x\n", d, j, value);
				return -EAGAIN;
			}
		}
		return 0;
	}
	return -ENODEV;
}
EXPORT_SYMBOL_GPL(apple_sleep_display_v27_power_ready);



#include "sleep-v27-late.inc"

static int apple_sleep_profile_check(struct apple_sleep *s)
{
	const struct apple_sleep_soc *soc;
	unsigned int i;

	if (!s)
		return -ENODEV;
	soc = s->soc;

	if (soc->ngate_coprocs > ARRAY_SIZE(s->gate) || soc->nstray > BITS_PER_LONG)
		return -EINVAL;
	if (soc->early_context && (!soc->ops || !soc->ops->cpu_prepare || !soc->ops->cpu_restore))
		return -EINVAL;
	if (!soc->ops || !soc->ops->prepare_regs || !soc->ops->undo_regs ||
	    !soc->ops->display_off || !soc->ops->commit_regs || !soc->ops->wake_regs ||
	    !soc->display_count || soc->display_count > SLEEP_MAX_DISPLAYS ||
	    !soc->cpu_clusters || soc->cpu_clusters > SLEEP_MAX_CLUSTERS)
		return dev_err_probe(s->dev, -EINVAL, "incomplete sleep profile\n");
	for (i = 0; !soc->regions && i < soc->cpu_clusters; i++)
		if (!soc->dvfm_names[i])
			return -EINVAL;
	return 0;
}

static int apple_sleep_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct apple_sleep *s;
	int ret;

	/* Below EL2 a monitor owns all of this. */
	if (!is_kernel_in_hyp_mode())
		return -ENODEV;

	s = devm_kzalloc(dev, sizeof(*s), GFP_KERNEL);
	if (!s)
		return -ENOMEM;
	s->dev = dev;
	s->soc = of_device_get_match_data(dev);
	if (!s->soc)
		return -ENODEV;

	ret = apple_sleep_profile_check(s);
	if (ret)
		return ret;

	/* Without the boot object's half, a wake would rerun its boot flow. */
	ret = apple_sleep_map_mailbox(s);
	if (ret)
		return ret;

	if (s->soc->ops->map_regs) {
		ret = s->soc->ops->map_regs(s);
		if (ret)
			return ret;
	} else {
		ret = apple_sleep_map_regs(s);
		if (ret)
			return ret;
		ret = of_address_to_resource(dev->of_node,
					     of_property_match_string(dev->of_node, "reg-names", "ps"),
					     &s->ps_res);
		if (ret)
			return ret;
		ret = apple_sleep_ave_pd_init(s);
		if (ret)
			return dev_err_probe(dev, ret, "cannot link the AVE complex\n");
		s->driver_down = devm_bitmap_zalloc(dev, resource_size(&s->ps_res) / 4, GFP_KERNEL);
		if (!s->driver_down)
			return -ENOMEM;
		ret = apple_sleep_alloc_books(s);
		if (ret)
			return ret;
		scoped_guard(spinlock_irqsave, &sleep_slp_ddr_lock) {
			sleep_slp_ddr_word = s->wake + WAKE_SLP_DDR;
			sleep_slp_ddr_write();
		}

		ret = apple_sleep_read_wake_sources(s);
		if (ret)
			return ret;
	}
	ret = apple_sleep_validate_cpu_topology(s);
	if (ret)
		return ret;

	ret = apple_sleep_map_displays(s);
	if (ret)
		return ret;
	if (!s->soc->regions)
		apple_sleep_map_apcie(s);
	apple_sleep_map_pins(s);
	apple_sleep_map_debug_port(s);

	platform_set_drvdata(pdev, s);

	ret = spin_table_set_power_ops(&sleep_cpu_power_ops);
	if (ret)
		return dev_err_probe(dev, ret, "cannot control CPU power\n");

	/* Last: the exported helpers take it as a probed driver, and a failure frees s. */
	apple_sleep = s;
	/* Deep becomes the default sleep state, unless mem_sleep_default= chose another. */
	suspend_set_ops(&apple_sleep_ops);

	dev_info(dev, "deep sleep %s: boot object resumes %llu\n",
		 mem_sleep_current == PM_SUSPEND_MEM ? "is the default" :
		 "available, not the default", READ_ONCE(s->mbox->resumes));

	return 0;
}

/*
 * T8140 (MacBook Neo). Power words from the ADT's /arm-io/pmgr, measured from macOS's sleep
 * under aura. CPU clusters: 0 is E, 1 is P, ADT /arm-io/pmgr reg[5], [7], [9] and [14], [16],
 * [18].
 */
#define T8140_PS_DISP_CPU	0x10000
#define T8140_PS_DISP_FE	0x318
#define T8140_PS_DISP_SYS	0x280
#define T8140_PS_DISPEXT0_CPU	0x328
#define T8140_PS_DISPEXT0_FE	0x320
#define T8140_PS_DISPEXT0_SYS	0x288
#define T8140_PS_SIO_CPU	0x2a8
#define T8140_PS_SEP		0xc00
#define T8140_PS_ATC0_USB_AON	0x3e0
#define T8140_PS_ATC0_COMMON	0x3e8
#define T8140_PS_ATC0_USB	0x3f0
#define T8140_PS_AP_TMM		0x1a0
#define T8140_PS_GPIO		0x120
#define T8140_PS_ANS		0x278
#define T8140_PS_APCIE_GP	0x258
#define T8140_PS_APCIE_SYS_GP	0x2a0
#define T8140_PS_APCIE_ST	0x310
#define T8140_PS_APCIE_SYS_ST	0x3a0
#define T8140_PS_APCIE_PHY_SW	0x3c0

/*
 * Every power word in the ADT, read just before ANS's stop so a run shows the whole chip.
 */
static const u32 t8140_ps_all[] = {
	0x0, 0x8, 0x10, 0x18, 0x30, 0x38, 0x50, 0x58, 0x60, 0x100, 0x108, 0x110, 0x118,
	0x120, 0x128, 0x130, 0x138, 0x140, 0x148, 0x150, 0x158, 0x160, 0x168, 0x170, 0x178,
	0x180, 0x188, 0x190, 0x198, 0x1a0, 0x1a8, 0x1b0, 0x1b8, 0x1c0, 0x1c8, 0x1d0, 0x1d8,
	0x1e0, 0x1e8, 0x1f0, 0x1f8, 0x200, 0x208, 0x210, 0x218, 0x220, 0x228, 0x230, 0x238,
	0x240, 0x248, 0x250, 0x258, 0x260, 0x268, 0x270, 0x278, 0x280, 0x288, 0x290, 0x298,
	0x2a0, 0x2a8, 0x2b0, 0x2b8, 0x2c0, 0x2c8, 0x2d0, 0x2d8, 0x2e0, 0x2e8, 0x2f0, 0x2f8,
	0x300, 0x308, 0x310, 0x318, 0x320, 0x328, 0x330, 0x338, 0x340, 0x348, 0x350, 0x358,
	0x360, 0x368, 0x370, 0x378, 0x380, 0x388, 0x390, 0x398, 0x3a0, 0x3a8, 0x3b0, 0x3b8,
	0x3c0, 0x3c8, 0x3d0, 0x3d8, 0x3e0, 0x3e8, 0x3f0, 0xc00, 0x4000, 0x4008, 0x4010,
	0x4018, 0x4020, 0x4028, 0x4030, 0x4038, 0x4040, 0x4048, 0x4050, 0x4058, 0x4060,
	0x8000, 0x8008, 0x8010, 0x8018, 0x8020, 0x8028, 0x8030, 0x8038, 0x8040, 0x8048,
	0xc000, 0xc008, 0xc010, 0x10000, 0x10008, 0x10010, 0x10018, 0x10020, 0x10028,
	0x10030, 0x10038, 0x10040,
};

static const u32 t8140_ps_aon_all[] = {
	0x0, 0x58, 0x60, 0x68, 0x70, 0x78, 0x80, 0x88, 0x90, 0x98, 0xa0, 0xa8, 0xb0,
	0x4000, 0x4008, 0x4010, 0x4018, 0x4020, 0x4028, 0x4030, 0x4038, 0x4040, 0x4048,
	0x4050, 0x4058, 0x4060, 0x4068, 0x4070, 0x4078, 0x4080, 0x4088, 0x4090, 0x4098,
	0x40a0, 0x40a8, 0x40b0, 0x40b8, 0x40c0, 0x40c8, 0x40d0, 0x40d8, 0x40e0, 0x40e8,
	0x40f0, 0x40f8, 0x4100, 0x4108, 0x4110, 0x4118, 0x4120, 0x4128, 0x4130, 0x4138,
	0x4140, 0x4148, 0x4150, 0x4158, 0x4160, 0x4168, 0x4170, 0x4178, 0x4180, 0x8000,
	0x8008, 0x8010, 0x8018, 0x8020, 0x8028, 0x8030, 0x8038, 0x8040, 0x8048, 0x8050,
	0x8058, 0x8060, 0x8068, 0x8070, 0x8078,
};

/* What macOS has on at the commit and Linux leaves off, in its order: nothing uses them here. */
static const u32 t8140_ps_macos_on[] = {
	0x2f0, 0x2f8, 0x358, 0x360, 0x368, 0x370, 0x378, 0x380, 0x388, 0x118,
};

/*
 * The power words macOS's wake writes back on before any driver runs, in its order
 * (logs/wakerec-20261105-154137Z). The engine leaves domains like GPIO dark, and genpd still
 * has them on, so nothing else would.
 */
static const u32 t8140_ps_wake_order[] = {
	0xc00, 0x110, 0x118, 0x120, 0x168, 0x178, 0x2a8, 0x2f0, 0x2f8, 0x300, 0x358, 0x360,
	0x368, 0x370, 0x378, 0x380, 0x388, 0x3d0, 0x3d8,
};

static const u32 t8140_ps_aon_wake_order[] = { 0x78, 0x88, 0x8030, 0x8038 };

/* What macOS has off, highest first, as it lowers them. Domains with their own stop aren't here. */
static const u32 t8140_ps_macos_off[] = {
	0x4060, 0x4058, 0x4050, 0x4048, 0x4040, 0x4038, 0x4030, 0x4028, 0x4020, 0x4018,
	0x4010, 0x4008, 0x4000, 0x398, 0x390, 0x350, 0x348, 0x340, 0x338, 0x330,
	0x308, 0x2e8, 0x2e0, 0x2d8, 0x2d0, 0x2c8, 0x2c0, 0x2b8, 0x2b0, 0x298,
	0x290, 0x270, 0x268, 0x250, 0x248, 0x240, 0x238, 0x230, 0x1a0, 0x190,
	0x188, 0x180, 0x170, 0x160, 0x158, 0x150, 0x148, 0x140, 0x108,
};

static const u32 t8140_ps_aon_macos_off[] = {
	0x8028, 0x4140, 0x40f8, 0x80,
};

/* The AOP's SPMI words: on, with AUTO_ENABLE off on macOS. */
static const u32 t8140_ps_aon_macos_no_auto[] = {
	0x4078, 0x4080,
};

/* M2A_1B's power-gate polls, less the SEP, PMP and SIO switch, which macOS doesn't have off. */
static const u32 t8140_ps_engine_polled[] = {
	0xc008, 0xc010, 0xc000, 0x290, 0x270, 0x8038, 0x8030, 0x8048, 0x8040, 0x8028, 0x248,
	0x8010, 0x8008, 0x8020, 0x8018, 0x8000, 0x240, 0x4038, 0x4030, 0x4028, 0x4040, 0x4060,
	0x4058, 0x4050, 0x4048, 0x4018, 0x4010, 0x4008, 0x4000, 0x268, 0x1f0, 0x298, 0x238,
	0x3b8, 0x3b0, 0x328, 0x288, 0x308, 0x3a0, 0x2a0, 0x278,
};

static const u32 t8140_ps_still_on[] = {
	T8140_PS_ATC0_COMMON, T8140_PS_DISP_SYS, T8140_PS_DISPEXT0_SYS, T8140_PS_ANS,
	0x270 /* AVD_SYS */, 0x240 /* VENC0_SYS */, 0x248 /* VENC1_SYS */, 0x1f0 /* GFX */,
	0x230 /* JPG */, T8140_PS_APCIE_SYS_GP,
};

/*
 * What XNU polls just before its complex words, and panics on if one is on, besides the
 * display's, ANS's and ATC0's, which the commit lowers itself.
 */
static const struct apple_sleep_word t8140_stray_words[] = {
	{ "AVD_SYS", 0x270 }, { "VENC0_SYS", 0x240 }, { "VENC1_SYS", 0x248 },
	{ "GFX", 0x1f0 }, { "JPG", 0x230 },
};

/*
 * A commit with the PMP and SIO still running reset the SoC 5.5 s later. The DCPs' drivers
 * power their CPUs off at DPMS off.
 */
static const struct apple_sleep_gate_coproc t8140_gate_coprocs[] = {
	{ "/soc/dcp@412e00000", true, T8140_PS_DISP_CPU },
	{ "/soc/dcp@416e00000", true, T8140_PS_DISPEXT0_CPU },
	{ "/soc/pmp@300500000", true },	{ "/soc/dma-controller@385e00000", true, T8140_PS_SIO_CPU },
	{ "/soc/nvme@3cdcc0000" },	{ "/soc/mtp@314600000" },
	{ "/soc/aop@310c00000" },	{ "/soc/smc@30c600000" },
	{ "/soc/sep@282600000" },	{ "/soc/gpu@482600000" },
};

static const struct apple_sleep_word t8140_gate_off[] = {
	{ "ane_sys", 0x290 }, { "isp_sys", 0x268 }, { "prores", 0x250 },
	{ "avd_sys", 0x270 }, { "jpg", 0x230 }, { "venc0_sys", 0x240 },
};

/* VENC0_SYS, VENC1_SYS */
static const u32 t8140_ave_domains[] = { 0x240, 0x248 };

static const struct apple_sleep_wake_name t8140_wake_names[] = {
	{ 0, "AOP.AICWakeupAP" }, { 5, "AOP.CPUWakeupAP" },
	{ 42, "AOP.OutboxNotEmpty" }, { 62, "AOP.SPMI0.SW3" },
	{ 66, "AOP.SPMI1.SW3" }, { 70, "ATC0_PMGR_USB_WAKEUP" },
	{ 80, "MTP.DOCK.CHANNELS.AP0.IRQ" }, { 99, "MTP.OUTBOX.N.EMPTY_IRQ" },
	{ 109, "NUB.APWakeupTime" }, { 110, "NUB.APWatchdog" },
	{ 125, "NUB.GPIO.IRQ6" }, { 131, "NUB.SPMI0.SW3" },
	{ 136, "NUB.SPMI1.SW3" }, { 140, "NUB.WakeupAP" },
	{ 145, "SMC.CPUWakeupAP" }, { 165, "SMC.OutboxNotEmpty" },
	{ 211, "AOP.TrustedOutboxNotEmpty" }, { 227, "AOP2.OutboxNotEmpty" },
	{ 231, "AOP2.TrustedOutboxNotEmpty" }, { 236, "AOP2.CPUWakeupAP" },
};

static const struct apple_sleep_soc_ops t8140_sleep_ops = {
	.prepare_regs = t8140_sleep_prepare_regs,
	.undo_regs = apple_sleep_match_undo,
	.display_off = sleep_display_off,
	.commit_regs = t8140_sleep_commit_regs,
	.wake_regs = t8140_sleep_wake_regs,
};

static const struct apple_sleep_soc t8140_sleep_soc = {
	.ops = &t8140_sleep_ops,
	.own_wake_masks = true,
	.cpu_cluster_affinity = 2,
	.cpu_clusters = 2,
	.cpu_cores_per_cluster = 4,
	.cpu_start_stride = 4,
	.display_count = 2,
	.dvfm_names = { "dvfm-e", "dvfm-p" },
	.ps = {
		.disp = {
			{ T8140_PS_DISP_CPU, T8140_PS_DISP_FE, T8140_PS_DISP_SYS },
			{ T8140_PS_DISPEXT0_CPU, T8140_PS_DISPEXT0_FE, T8140_PS_DISPEXT0_SYS },
		},
		.sio_cpu = T8140_PS_SIO_CPU,
		.sep = T8140_PS_SEP,
		.usb_aon = T8140_PS_ATC0_USB_AON,
		.usb_common = T8140_PS_ATC0_COMMON,
		.usb = T8140_PS_ATC0_USB,
		.ap_tmm = T8140_PS_AP_TMM,
		.gpio = T8140_PS_GPIO,
		.ans = T8140_PS_ANS,
		.apcie_gp = T8140_PS_APCIE_GP,
		.apcie_sys_gp = T8140_PS_APCIE_SYS_GP,
		.apcie_st = T8140_PS_APCIE_ST,
		.apcie_sys_st = T8140_PS_APCIE_SYS_ST,
		.apcie_phy_sw = T8140_PS_APCIE_PHY_SW,
	},
	.ps_aon_debug_switch = 0xb0,
	.pwrgate_wake = { 0x000, 0x040 },
	.pwrgate_cpm = { 0x060, 0x068 },
	.pwrgate_disp = { 0xc8c, 0xcd0 },
	.ps_all = SLEEP_WORDS(t8140_ps_all),
	.ps_aon_all = SLEEP_WORDS(t8140_ps_aon_all),
	.ps_macos_on = SLEEP_WORDS(t8140_ps_macos_on),
	.ps_macos_off = SLEEP_WORDS(t8140_ps_macos_off),
	.ps_aon_macos_off = SLEEP_WORDS(t8140_ps_aon_macos_off),
	.ps_aon_macos_no_auto = SLEEP_WORDS(t8140_ps_aon_macos_no_auto),
	.ps_wake_order = SLEEP_WORDS(t8140_ps_wake_order),
	.ps_aon_wake_order = SLEEP_WORDS(t8140_ps_aon_wake_order),
	.ps_still_on = SLEEP_WORDS(t8140_ps_still_on),
	.ps_engine_polled = SLEEP_WORDS(t8140_ps_engine_polled),
	.stray = t8140_stray_words,
	.nstray = ARRAY_SIZE(t8140_stray_words),
	.gate_off = t8140_gate_off,
	.ngate_off = ARRAY_SIZE(t8140_gate_off),
	.gate_coprocs = t8140_gate_coprocs,
	.ngate_coprocs = ARRAY_SIZE(t8140_gate_coprocs),
	.wake_names = t8140_wake_names,
	.nwake_names = ARRAY_SIZE(t8140_wake_names),
	/* 21 sources. Measured: macOS reads 0x001fef77 there. */
	.wake_last_word = GENMASK(20, 0),
	.radio_pcie = "apple,t8140-pcie",
	.ave_domains = t8140_ave_domains,
	.nave_domains = ARRAY_SIZE(t8140_ave_domains),
	.ave_pmc = "apple,t8140-pmc",
	.ave_pmc_cluster = 3,
};

static const struct of_device_id apple_sleep_of_match[] = {
	{ .compatible = "apple,t8140-sleep", .data = &t8140_sleep_soc },
	{ .compatible = "apple,t6050-sleep", .data = &t6050_sleep_soc },
	{ }
};

static struct platform_driver apple_sleep_driver = {
	.driver = {
		.name = "apple-sleep",
		.of_match_table = apple_sleep_of_match,
		.suppress_bind_attrs = true,
	},
	.probe = apple_sleep_probe,
};
builtin_platform_driver(apple_sleep_driver);

#if IS_ENABLED(CONFIG_APPLE_SOC_SLEEP_KUNIT_TEST)
#include "sleep-test.c"
#endif
