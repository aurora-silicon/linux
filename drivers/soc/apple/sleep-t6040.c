// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * Apple SoC system sleep: the T6040 (M4 Pro) part
 *
 * The T6040 sleeps through the same boot object and mailbox as the SoCs sleep.c takes from
 * its lists, and nearly every step in between differs: three CPU complexes, five display
 * pipes whose power states have to read off first, two storage PCIe lanes, the wake block
 * and the order of the commit. So this file is a part of that driver (sleep-internal.h) with
 * its own probe, state and suspend operations. The commit follows the T6040 entry as
 * measured on the J616s, with what the SoC's sleep engine needs from the OS (see
 * t6040_sleep_commit); the wake return mirrors the PMGR wake restore
 * (see t6040_sleep_return). The registry of coprocessors and of the enclave
 * (linux/soc/apple/sleep.h), the dry run level and the mailbox layout are the generic
 * driver's.
 *
 * It refuses first:
 *
 *  - It binds below EL2 and without the boot object's mailbox as well, and shows both as
 *    blockers in debugfs t6040-sleep/preflight, next to the CPUs that can park and the
 *    display power states the ADT lists to check before a sleep (device-check-ps-b4-sleep).
 *  - It offers suspend-to-RAM only to a system booted with t6040_sleep.allow_entry=1.
 *    Without it suspend-to-idle stays the default sleep, and it passes through here
 *    untouched either way.
 *  - Its suspend_ops .begin computes the blocker list of a suspend-to-RAM. With a blocker
 *    left it prints every one and returns an error before any device suspends.
 *  - Dry runs (t6040_sleep.dry_run, also writable at runtime):
 *      1: pm_suspend_via_firmware() is set, so every driver's firmware-driven
 *         sleep step that can be taken back runs (apple_sleep_power_loss() is
 *         false, so what only a power cycle restores is left out), the CPUs
 *         park and answer the commit's hold, the commit point's checks run,
 *         and the sleep is refused there and resumed warm.
 *      2: 1, plus the last steps that can still be taken back (the SMC's sleep
 *         commit, each coprocessor's last check, such as ANS's check of the
 *         shutdown notification it sent at its suspend).
 *    The AON watchdog's WD2 is armed from .begin to .end of a dry run
 *    (t6040_sleep.wd2_secs, 0 to leave it off).
 *  - The real commit (t6040_sleep_commit) runs only when no blocker remains,
 *    t6040_sleep.allow_entry=1 is given and no dry run is asked for. The AON
 *    watchdog's WD2 is its net (t6040_sleep.sleep_wd_secs), and over the device
 *    suspend and resume around it.
 *
 * The lid's state at the wake and the USB wake ports, which the generic driver keeps for the
 * display and the port controllers, are declined: this sleep was tested without them.
 */

#include <linux/bitfield.h>
#include <linux/bits.h>
#include <linux/cpu.h>
#include <linux/debugfs.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/irqchip/irq-apple-aic.h>
#include <linux/mfd/syscon.h>
#include <linux/minmax.h>
#include <linux/moduleparam.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_platform.h>
#include <linux/mfd/macsmc.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/smp.h>
#include <linux/soc/apple/aura-hibernate.h>
#include <linux/soc/apple/sleep.h>
#include <linux/soc/apple/tunable.h>
#include <linux/suspend.h>

#include <asm/barrier.h>
#include <asm/cacheflush.h>
#include <asm/cpu_ops.h>
#include <asm/memory.h>
#include <asm/cputype.h>
#include <asm/smp_plat.h>
#include <asm/suspend.h>
#include <asm/sysreg.h>
#include <asm/virt.h>
#include <clocksource/arm_arch_timer.h>
#include <asm/arch_timer.h>

#include "sleep-internal.h"

/* The parameters keep the names they had when this was a driver of its own. */
#undef MODULE_PARAM_PREFIX
#define MODULE_PARAM_PREFIX "t6040_sleep."

#define PS_STRIDE		8
#define PS_MAX_PER_GROUP	16
#define PS_SIO_FIRST		0x150	/* SOC-East: I2C0, the first SIO peripheral word */
#define PS_SIO_LAST		0x228	/* SOC-East: UART6, the last */

/*
 * ANS's storage PCIe block: the TWO ST lanes (apcie_st0/apcie_st1) and their
 * shared PHY (T8140 has a single lane). Taken down at the commit
 * and raised on wake. The power words sit in SOC-East (t604x-pmgr.dtsi
 * offsets, same bank as ps_ans); the control windows (axi2af/cio3pll/clkgen/
 * phy-block/phy-ip/rc) and the PLL/lane tunables are NOT in this SoC's static
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
#define PS_BUSY			BIT(11)	/* a request is in progress */

/*
 * cluster-ctl (pmgr reg0 + cluster-ctl-offset 0x20000 = 0x502820000). Measured
 * on the J616s: the three CPU complexes are at E +0x0, P0 +0x4,
 * P1 +0x8; the platform writes the low nibble 0 to power one down
 * and raises it to 0xf to power one up, with +0xc the status/poll word. Three
 * complexes on J616s (T8140 had E/P/AVE).
 */
#define CLUSTER_CTL(n)		((n) * 4)
#define CLUSTER_CTL_NR		5	/* E, P0, P1 and two more complexes (+0xc, +0x10) */
#define CLUSTER_CTL_FIRST_WAIT	3	/* the platform waits past the third: the last two are polled */
#define CLUSTER_CTL_BUSY	BIT(4)
#define CLUSTER_CTL_NIBBLE	GENMASK(3, 0)
#define CLUSTER_CTL_ON		0xf
#define CLUSTER_TIMEOUT_US	50000

/*
 * SOC-PwrGate (pmgr reg0x26 = 0x50282c000). Retention
 * bits at +0x0/+0x40 and the three CPM voters at +0x100/+0x108/+0x110, each
 * BIT31. The wake restore re-asserts them; no pwrgate write was seen
 * in the entry (the voters read back 0x80000000, a no-op), so entry
 * leaves them alone.
 */
#define PWRGATE_ON		0x80000000
#define PWRGATE_WAKE_0		0x000
#define PWRGATE_WAKE_1		0x040
#define PWRGATE_CPM_0		0x100
#define PWRGATE_CPM_1		0x108
#define PWRGATE_CPM_2		0x110

/*
 * SOC-East (pmgr reg2 = 0x502280000). The SEP/retention word at +0xc00: on
 * wake the platform clears DEV_DISABLE (BIT10) and then polls PARENT_MISSING (BIT11)
 * clear before going on. Also holds several
 * of the axi2af domains' power words.
 */
#define SOC_EAST_RETENTION	0xc00
#define SOC_EAST_DEV_DISABLE	BIT(10)
#define SOC_EAST_PARENT_MISSING	BIT(11)
#define SOC_EAST_TIMEOUT_US	5000

/*
 * ClkGen SOC (pmgr reg3 = 0x500000000). The clk-mux park words
 * (0x500038050/054/060/064) have bits [27:24] cleared at quiesce (observed
 * 0xa5100000 / 0xa7100000 -> 0xa0100000), with BIT31 set, each polled until
 * BIT30 clears. This is the clock quiesce, NOT measured on the J616s:
 * the one measured entry did not cover the clkgen block. Wake restores
 * the saved words (the clock restore is otherwise a NOP).
 */
#define CLKGEN_MUX_SET		0x80000000
#define CLKGEN_MUX_MASK		0xf0ffffff
#define CLKGEN_MUX_WORDS	4
#define CLKGEN_MUX_PENDING	BIT(30)
#define CLK_QUIESCE_TIMEOUT_US	1000

/*
 * The clock quiesce, whole: two SOC-East
 * words lose BIT28 and are polled until [7:4] reads their [3:0], then the four
 * clk-mux words, in this order, get [27:24] := 0 with BIT31 and are polled until
 * BIT30 clears. Nothing else (no +0x70000 write).
 */
#define SOC_EAST_CLK_A		0x120
#define SOC_EAST_CLK_B		0x130
#define SOC_EAST_CLK_AUTO	BIT(28)

/*
 * The keep-alive enables: 32 words in PMGR SOC
 * from +0x1dc000 and 32 in SOC-East from +0x50000, stride 0x44, BIT0 the keep-alive.
 * The quiesce clears them right before the S2R timer (unless the boot-arg
 * pmgr-disable-ka); its wake restore sets them again.
 */
#define T6040_KA_SOC_PA		0x5029dc000ULL
#define T6040_KA_EAST_PA	0x5022d0000ULL
#define KA_WORDS		32
#define KA_STRIDE		0x44
#define KA_SIZE			(KA_WORDS * KA_STRIDE)

/*
 * The SMC's AP watchdog deadline (the watchdog timer's reg[2] + 8):
 * The platform writes 0 when it enables the AON watchdog for a reconfig,
 * right after arming the AON WD2 (measured on the J616s: 0x508330008 := 0).
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

/*
 * The hardware steps of the generic PMGR quiesce that run on J616s before
 * its T6041 part (measured and the ADT). In order:
 *  - the ACC quiesce of c = E, P0, P1: APSC off, ACC +0xe20020 BIT23 := 1 (64-bit),
 *    poll BIT7 clear (CPM gating after it reads back already set: not written);
 *  - features off: cpu-fixed-freq-pll-relock, BIT42 := 0 in the E and P0
 *    ACC words; then the TVMs disp2, afr, dcs (two), gfx (two), disp, soc: control
 *    BIT0 := 0, status polled until BIT1 clears.
 * The platform then drops the AP voter's DCS floor vote in the PMC. That step is left out: the
 * first access to that word hung the bus on the J616s.
 * The platform polls the APSC and TVM words without a timeout; here every poll is bounded.
 */
static const u64 t6040_acc_apsc_pa[] = { 0x210e20020ULL, 0x211e20020ULL, 0x212e20020ULL };
#define ACC_APSC_DISABLE	BIT_ULL(23)
#define ACC_APSC_BUSY		BIT_ULL(7)
#define ACC_PLL_RELOCK		BIT_ULL(42)
#define ACC_PLL_RELOCK_COMPLEXES	2

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
 * The PMP's PTD (pmp-report 0x5003c0000, T6041 layout as drivers/pmdomain/apple/
 * pmp-report.c): SOC-DEV-PS-REQ read at +0x1e00, SOC-DEV-PS-ACK at +0x1e80, 64-bit.
 * Read only, for the preflight.
 */
#define T6040_PTD_PA		0x5003c0000ULL
#define PTD_REQ_READ		0x1e00
#define PTD_ACK			0x1e80
#define QUIESCE_TIMEOUT_US	2000

#define AGX_GATE_RESIDENT	GENMASK(19, 18)
#define T6040_AFR_PS_PA		0x288e80100ULL
#define AFR_LEVEL		GENMASK(19, 16)
#define AGX_TIMEOUT_US		10000

/*
 * The PMU over SPMI (spmi0 = 0x509014000, spmi-a0 = 0x509188000), written as on the T8140.
 * Measured on the J616s: the boot stage is set to 0x50 by an extended write whose
 * command word is (0xf801 << 16) | 0x8e30 = 0xf8018e30, and NOTIFY_ENABLE is 0x2000001 on
 * both controllers' +0x40.
 */
/* The PMU's USID on the sleep's SPMI bus, and the boot stage that announces a sleep */
#define PMU_USID		0xe
#define PMU_BOOT_STAGE		0xf801		/* T6040 slave/reg; cmd 0xf8018e30 */
#define PMU_BOOT_STAGE_SLEEP	0x50
/* The response vector's enable for the sleep */
#define SPMI_NOTIFY_ENABLE_SLEEP	0x2000001

/*
 * MasterMini (NUB_MPMGR_AON, RegMap 4 = pmgr reg[0x37] = 0x508900000).
 * The S2R timer at +0x4000 (physical 0x508904000) is the commit: writing 1 arms
 * the SoC sleep engine. Measured on the J616s: exactly this write.
 */
#define MASTERMINI_S2R_TIMER	0x4000

/*
 * AOP-global (AP controls, RegMap 0x42, window 0x508000000). Mapped from the
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
#define AXI2AF_DISP_SYS		4	/* t6040_axi2af[] index */
#define DISP_PS_TIMEOUT_US	8000

/* The GP PCIe (t602x port layout, as T8140's radio port): power words in soc_east. */
#define PS_APCIE_GP		0x100
#define PS_APCIE_SYS_GP		0x390
#define AXI2AF_ANS		9
#define AXI2AF_APCIE_SYS_GP	14
#define GP_PORT_L2		0x88
#define GP_PORT_L2_ENTER	0x11
#define GP_PORT_L2_BUSY		BIT(0)
#define GP_PORT_INTSTAT		0x100
#define GP_PORT_INTMSK		0x104
#define GP_PORT_INT_LINK	GENMASK(7, 4)
#define GP_PORT_PERST_GUARD	0x13c
#define GP_PORT_LINKSTS		0x208
#define GP_PORT_LINKSTS_UP	BIT(0)
#define GP_PORT_APPCLK		0x800
#define GP_PORT_APPCLK_EN	BIT(0)
#define GP_PORT_APPCLK_CGDIS	BIT(8)
#define GP_PORT_PERST		0x82c
#define GP_PHY_LINK		BIT(31)
#define GP_PHY_EN0		BIT(9)
#define GP_PHY_EN1		BIT(10)
#define GP_PHY_RESET		BIT(4)
#define GP_PHY_CLK0REQ		BIT(0)
#define GP_PHY_CLK1REQ		BIT(1)
#define GP_PHY_CLK0ACK		BIT(2)
#define GP_PIN_DATA		BIT(0)
#define GP_STOP_US		5000
#define T6040_PMC_DCP_PA	0x5048a9c14ULL	/* count of DCPs powered (+0), second voter (+4); the platform ends at 0/0 */
#define T6040_AXI2AF_DOMAINS	15

/*
 * The AON watchdog's WD2, the sleep safety net: ADT wdt reg[0] = 0x50836c000, with the
 * T8140's registers. Linux's own watchdog driver runs WD1 (+0x10..+0x1c), not this one.
 * ALARM is 32 bits at 24 MHz.
 */
#define WDT_MAX_SECS		178U

struct t6040_sleep_group {
	const char *name;
	struct regmap *regmap;
	u32 first;	/* the pipe's CPU */
	u32 last;	/* its last sub-block */
};

/* A device the commit point looks at: bound or not, and its own power word. */
struct t6040_sleep_watch {
	const char *compatible;
	const char *name;
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
	/* The SOC and SOC-East power word pages at their fixed addresses. */
	void __iomem *pmgr_soc;		/* 0x502800000: fabrics, ATC commons, DCS */
	void __iomem *pmgr_east;	/* 0x502280000: fabrics, DCS, SIO */
	u32 sio_ps_saved[(PS_SIO_LAST - PS_SIO_FIRST) / PS_STRIDE + 1];
	bool sio_ps_saved_valid;
	void __iomem *soc_east;
	void __iomem *soc_ps;
	void __iomem *aop_global;
	void __iomem *axi2af_bridge[T6040_AXI2AF_DOMAINS];
	struct aura_s2r_mailbox *mbox;
	u64 mbox_pa;
	bool power_ops;
	bool wd2_armed;
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
	void __iomem *ptd;
	void __iomem *gapf;
	void __iomem *afr_ps;
	u32 cluster_saved[CLUSTER_CTL_NR];
	u32 wake_mask_saved[AOP_WAKE_WORDS];
	bool wake_mask_saved_valid;
	u32 wake_status[AOP_WAKE_WORDS];
	bool returned;
	/* ANS's storage ST lanes: windows + whether a commit stopped ANS. */
	struct t6040_apcie_st apcie_st;
	bool pcie_ans_stopped;
	bool ans_bridge_quiesced;	/* the commit found ANS's fabric bridge armed */
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

/*
 * 0: no dry run. 1: the firmware-driven device suspend short of what only a power
 * cycle restores, parking and the commit's hold, refused at the commit point. 2: 1 and
 * the last steps that can still be taken back. Read at each suspend's .begin.
 */
static unsigned int dry_run;
module_param(dry_run, uint, 0644);
MODULE_PARM_DESC(dry_run,
		 "Dry run level: 1 firmware-driven device sleep refused at the commit point, 2 also the last steps");

static bool allow_entry;
module_param(allow_entry, bool, 0444);
MODULE_PARM_DESC(allow_entry,
		 "Reach the real commit if no blocker remains");

/*
 * The AON WD2 net armed at the commit. 5 s covers the reconfig; a longer
 * net tells "the SoC never slept" (reset at the net) from "it slept and the wake
 * failed" (reset at the wake source). Never off: 1 to 178.
 */
static unsigned int sleep_wd_secs = 30;
module_param(sleep_wd_secs, uint, 0644);
MODULE_PARM_DESC(sleep_wd_secs,
		 "AON WD2 net armed at the S2R commit, in seconds (default 30, clamped to 1..178)");

static unsigned int wd2_secs = WDT_SUSPEND_SECS;
module_param(wd2_secs, uint, 0644);
MODULE_PARM_DESC(wd2_secs,
		 "Arm the AON WD2 for this many seconds from .begin to .end of a dry run (0: off, at most 178)");

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

/* The generic driver answers apple_sleep_power_loss() and apple_sleep_last_steps() by it. */
static void t6040_sleep_set_dry(struct t6040_sleep *s, unsigned int dry)
{
	s->dry = dry;
	apple_sleep_set_dry(dry);
}

/* A real entry: its commit stops the registered coprocessors; a dry run's never does. */
static bool t6040_sleep_reg_commit_stops(void)
{
	return READ_ONCE(t6040_sleep) && is_kernel_in_hyp_mode() && apple_sleep_power_loss();
}

/*
 * In the power-state check, a sub-block is on while its TARGET is not 0. A
 * pipe's CPU word never counts: it stays under automatic power gating (taking it out
 * of automatic mode raised an SError on T6040) and the SoC power-down drops it.
 */
static bool t6040_sleep_ps_on(u32 val, bool cpu)
{
	return !cpu && FIELD_GET(PS_TARGET, val);
}

static unsigned int t6040_sleep_show_group(struct seq_file *s, const struct t6040_sleep_group *g)
{
	unsigned int on = 0;
	u32 off;

	for (off = g->first; off <= g->last; off += PS_STRIDE) {
		bool cpu = off == g->first;
		u32 val;
		int ret;

		ret = regmap_read(g->regmap, off, &val);
		if (ret) {
			/* Unreadable counts as on: the refusal fails closed. */
			if (s)
				seq_printf(s, "  %-12s +%#05x  read failed (%d)\n", g->name, off,
					   ret);
			on++;
			continue;
		}
		if (t6040_sleep_ps_on(val, cpu))
			on++;
		if (s)
			seq_printf(s, "  %-12s +%#05x %s %#010x target %lu actual %lu%s\n", g->name,
				   off, cpu ? "cpu  " : "child", val, FIELD_GET(PS_TARGET, val),
				   FIELD_GET(PS_ACTUAL, val),
				   t6040_sleep_ps_on(val, cpu) ? "  ON" : "");
	}

	return on;
}

/* How many secondaries can leave the kernel (park), and the boot ops' name. */
static unsigned int t6040_sleep_parkable(const char **name)
{
	unsigned int cpu, parkable = 0;

	for_each_possible_cpu(cpu) {
		const struct cpu_operations *ops = get_cpu_ops(cpu);

		if (cpu && ops && ops->cpu_die && ops->cpu_disable)
			parkable++;
	}
	if (name)
		*name = get_cpu_ops(0) ? get_cpu_ops(0)->name : "?";

	return parkable;
}

/*
 * The live blockers, recomputed each time: not at EL2, no boot-object return
 * path, a CPU that cannot park, or a display pipe still on. Returns the count,
 * and lists each when @s is given. @disp_out, if not NULL, gets the subtotal of
 * display-pipe blockers -- these clear during device suspend (apple-dcp's
 * .suspend stops the DCP and lowers its domains), so .begin excludes them and
 * only .enter, after the device suspend, enforces them.
 */
static unsigned int t6040_sleep_live_blockers(struct t6040_sleep *sleep, struct seq_file *s,
					      unsigned int *disp_out)
{
	/* Mapped and aura's: after a resume from disk, this boot's (t6040_sleep_restore()). */
	bool mailbox = sleep->mbox;
	bool el2 = is_kernel_in_hyp_mode();
	unsigned int i, blockers = 0, disp = 0, parkable;
	const char *ops_name;

	parkable = t6040_sleep_parkable(&ops_name);

	if (s) {
		seq_printf(s, "kernel at EL2: %s\n", el2 ? "yes" : "no");
		seq_printf(s, "boot object return path (/chosen/aurora,s2r-mailbox): %s\n",
			   mailbox ? "yes" : "no");
		seq_printf(s, "secondaries that can leave the kernel: %u of %u (%s)\n", parkable,
			   num_possible_cpus() - 1, ops_name);
	}
	blockers += !el2 + !mailbox + (parkable != num_possible_cpus() - 1);

	if (s)
		seq_puts(s, "power states checked before sleep (device-check-ps-b4-sleep):\n");
	for (i = 0; i < sleep->ngroups; i++) {
		unsigned int on = t6040_sleep_show_group(s, &sleep->groups[i]);

		if (s)
			seq_printf(s, "  %-12s %u on\n", sleep->groups[i].name, on);
		disp += !!on;
	}

	if (disp_out)
		*disp_out = disp;
	return blockers + disp;
}

/* The total, at the commit point: everything the live state still holds against the commit. */
static unsigned int t6040_sleep_total_blockers(struct t6040_sleep *sleep)
{
	return t6040_sleep_live_blockers(sleep, NULL, NULL);
}

static void t6040_axi2af_show(struct t6040_sleep *sleep, struct seq_file *s);

static int t6040_sleep_preflight_show(struct seq_file *s, void *unused)
{
	struct t6040_sleep *sleep = s->private;
	unsigned int live = t6040_sleep_live_blockers(sleep, s, NULL);
	unsigned int i;

	apple_sleep_registry_show(s);
	t6040_axi2af_show(sleep, s);
	seq_printf(s, "verdict: %u live blocker(s)\n", live);
	seq_printf(s, "dry_run %u, wd2_secs %u, allow_entry %d, parking ops %s\n",
		   READ_ONCE(dry_run), READ_ONCE(wd2_secs), allow_entry,
		   sleep->power_ops ? "registered" : "not registered");
	if (sleep->cluster_ctl) {
		static const char * const nm[] = { "EACC", "PACC0", "PACC1_ANE", "DISP_INT", "DISP_EXT" };

		for (i = 0; i < CLUSTER_CTL_NR; i++)
			seq_printf(s, "cluster-ctl +%#x %-9s %#x\n", CLUSTER_CTL(i), nm[i],
				   readl(sleep->cluster_ctl + CLUSTER_CTL(i)));
	}
	if (sleep->gapf)
		seq_printf(s, "gapf: base %#x cfg %#x lock %#x, slices dcp(17) %#x dcpext0(29) %#x dcpext1(48) %#x\n",
			   readl(sleep->gapf), readl(sleep->gapf + 0x20), readl(sleep->gapf + 0x24),
			   readl(sleep->gapf + 0x540), readl(sleep->gapf + 0x840),
			   readl(sleep->gapf + 0xd00));
	if (sleep->ptd)
		seq_printf(s, "pmp ptd: SOC-DEV-PS-REQ %#llx, SOC-DEV-PS-ACK %#llx, status %#llx\n",
			   readq(sleep->ptd + PTD_REQ_READ), readq(sleep->ptd + PTD_ACK),
			   readq(sleep->ptd + 0x10));

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(t6040_sleep_preflight);

/* The display groups' on-state, one line per group, no allocation. For the log at the commit point. */
static unsigned int t6040_sleep_log_groups(struct t6040_sleep *sleep)
{
	unsigned int i, blockers = 0;

	for (i = 0; i < sleep->ngroups; i++) {
		const struct t6040_sleep_group *g = &sleep->groups[i];
		u32 off, on_mask = 0, val;
		unsigned int bit = 0;

		for (off = g->first; off <= g->last; off += PS_STRIDE, bit++) {
			if (regmap_read(g->regmap, off, &val) ||
			    t6040_sleep_ps_on(val, off == g->first)) {
				on_mask |= BIT(bit);
				pr_info("t6040-sleep:   %s +%#x = %#x\n", g->name, off, val);
			}
		}
		if (on_mask)
			pr_info("t6040-sleep: %-8s ON (on mask %#x, cpu first)\n", g->name,
				on_mask);
		blockers += !!on_mask;
	}

	return blockers;
}

/* Log the full blocker list (interrupts may be off: no allocation, pr_* only). */
static void t6040_sleep_log_blockers(struct t6040_sleep *sleep)
{
	const char *ops_name;

	pr_info("t6040-sleep: EL2 %s, s2r-mailbox %s, parkable %u/%u (%s)\n",
		is_kernel_in_hyp_mode() ? "yes" : "no",
		of_property_present(of_chosen, "aurora,s2r-mailbox") ? "yes" : "no",
		t6040_sleep_parkable(&ops_name), num_possible_cpus() - 1, ops_name);
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
#define T6040_ED_CLUSTERS	3
#define T6040_ED_CORES		6

static void __iomem *t6040_ed[T6040_ED_CLUSTERS][T6040_ED_CORES];

static void t6040_ed_map(struct device *dev)
{
	unsigned int cpu, n = 0;

	for_each_possible_cpu(cpu) {
		u64 mpidr = cpu_logical_map(cpu);
		unsigned int c = MPIDR_AFFINITY_LEVEL(mpidr, 1), k = MPIDR_AFFINITY_LEVEL(mpidr, 0);

		if (c >= T6040_ED_CLUSTERS || k >= T6040_ED_CORES || t6040_ed[c][k])
			continue;
		t6040_ed[c][k] = t6040_ioremap_np(dev, T6040_ED_BASE_PA + c * T6040_ED_CLUSTER_STRIDE +
						 k * T6040_ED_CORE_STRIDE, 0x1000);
		n += !!t6040_ed[c][k];
	}
	dev_dbg(dev, "external debug windows mapped for %u cores\n", n);
}

static void t6040_ed_release(void)
{
	u64 mpidr = read_cpuid_mpidr();
	unsigned int c = MPIDR_AFFINITY_LEVEL(mpidr, 1), k = MPIDR_AFFINITY_LEVEL(mpidr, 0);
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

/* A core's last instructions: nothing brings it back but a reset. */
static void __noreturn t6040_deep_wfi(void)
{
	t6040_deep_wfi_arm();

	while (true) {
		dsb(sy);
		isb();
		wfi();
		write_sysreg_s(IPI_SR_PENDING, SYS_APL_IPI_SR_EL1);
	}
}

static void t6040_cpu_stop(u64 mpidr)
{
	u32 bit = t6040_cpu_start_bit(mpidr);

	if (t6040_sleep && t6040_sleep->cpu_start && bit)
		writel(bit, t6040_sleep->cpu_start + CPU_START_STOP);
	dsb(sy);
}

static void t6040_cpu_off(u64 mpidr)
{
	t6040_deep_wfi();
}

static int t6040_cpu_on(u64 mpidr)
{
	struct t6040_sleep *s = t6040_sleep;
	unsigned int cluster = t6040_cpu_cluster(mpidr);
	u32 bit = t6040_cpu_start_bit(mpidr);

	if (!s || !s->cpu_start)
		return -ENODEV;
	if (!bit)
		return -EINVAL;
	writel(bit, s->cpu_start + CPU_START_SYS);
	writel(BIT(t6040_cpu_core(mpidr)), s->cpu_start + CPU_START_CLUSTER(cluster));

	return 0;
}

static const struct spin_table_power_ops t6040_cpu_power_ops = {
	.cpu_stop = t6040_cpu_stop,
	.cpu_off = t6040_cpu_off,
	.cpu_on = t6040_cpu_on,
};

/*
 * The commit's hold needs them. Registered, they also let kexec take parking
 * CPUs as able to leave the kernel, for the rest of this boot. Nothing calls
 * @cpu_stop or @cpu_off but a commit, and @cpu_on starts only a CPU a commit
 * stopped.
 */
static void t6040_sleep_power_ops(struct t6040_sleep *s)
{
	if (s->power_ops || !s->cpu_start)
		return;
	if (!spin_table_set_power_ops(&t6040_cpu_power_ops)) {
		s->power_ops = true;
		dev_dbg(s->dev, "CPU parking power ops registered\n");
	}
}

static void t6040_sleep_set_wd2(struct t6040_sleep *s, unsigned int secs)
{
	if (!s->wdt)
		return;
	apple_sleep_wd2_arm(s->wdt, secs);
	s->wd2_armed = true;
}

static void t6040_sleep_clear_wd2(struct t6040_sleep *s)
{
	if (!s->wdt || !s->wd2_armed)
		return;
	writel(0, s->wdt + WDT_WD2_CTL);
	s->wd2_armed = false;
}

/*
 * In process context, after the device suspend: which watched devices are
 * bound. The commit point runs with interrupts off and only reads power words.
 */
static void t6040_sleep_watch_bound(struct t6040_sleep *s)
{
	int i;

	for (i = 0; i < T6040_WATCH_COUNT; i++) {
		struct t6040_sleep_watch *w = &s->watch[i];
		struct device_node *np = of_find_compatible_node(NULL, NULL, w->compatible);
		struct platform_device *pdev = np ? of_find_device_by_node(np) : NULL;

		w->bound = pdev && device_is_bound(&pdev->dev);
		if (pdev)
			put_device(&pdev->dev);
		of_node_put(np);
	}
}

/* Its power word's actual state is on. Unknown counts as on. */
static bool t6040_sleep_watch_on(struct t6040_sleep_watch *w, u32 *val)
{
	*val = 0;
	if (!w->has_ps || regmap_read(w->ps_regmap, w->ps_offset, val))
		return true;
	return FIELD_GET(PS_ACTUAL, *val);
}

/*
 * The checks a real commit makes, read-only: every registered coprocessor
 * quiesced by its driver, one CPU up and it the boot object's, the SEP bound
 * with its sleep request left here, ANS stopped only by its driver, and the
 * display pipes off. Logs each failure and returns the count.
 */
static unsigned int t6040_sleep_refuse(struct t6040_sleep *s)
{
	struct t6040_sleep_watch *sep = &s->watch[T6040_WATCH_SEP];
	struct t6040_sleep_watch *ans = &s->watch[T6040_WATCH_ANS];
	bool ans_idled = false;
	unsigned int refused;
	u32 val;

	refused = apple_sleep_coprocs_refuse(s->dev, &ans_idled);

	if (num_online_cpus() != 1 || !s->mbox ||
	    (read_cpuid_mpidr() & MPIDR_HWID_BITMASK) != s->mbox->boot_mpidr) {
		pr_err("t6040-sleep: refused: wrong CPU, other CPUs online, or no mailbox\n");
		refused++;
	}

	/*
	 * Refuse rather than half-sleep if any register block the commit or the
	 * wake must write is missing from the device tree. The deep WFI would
	 * never arm, and the wake could not restore the SoC.
	 */
	if (!s->cluster_ctl || !s->pwrgate || !s->mastermini || !s->spmi0 ||
	    !s->soc_east || !s->aop_global) {
		pr_err("t6040-sleep: refused: a commit/wake register base is not mapped (cluster-ctl %d pwrgate %d mastermini %d spmi0 %d soc-east %d aop-global %d)\n",
		       !!s->cluster_ctl, !!s->pwrgate, !!s->mastermini, !!s->spmi0,
		       !!s->soc_east, !!s->aop_global);
		refused++;
	}

	if (!sep->bound) {
		/* Nobody told it of the sleep: a commit with it running never returns. */
		t6040_sleep_watch_on(sep, &val);
		pr_err("t6040-sleep: refused: sep isn't bound (state %#x)\n", val);
		refused++;
	} else if (!apple_sleep_enclave_registered()) {
		pr_err("t6040-sleep: refused: sep's sleep request isn't left for the last refusal\n");
		refused++;
	}

	/* Only ANS's driver can stop it, and it doesn't come back without a real sleep. */
	if (!ans_idled && t6040_sleep_watch_on(ans, &val)) {
		pr_err("t6040-sleep: refused: ANS is on (%#x) with no driver to stop it\n", val);
		refused++;
	}

	/*
	 * ANS would be stopped, but its ST lanes have no way back without the
	 * apple,apcie control windows + PLL/lane tunables: refused.
	 */
	if (ans_idled && !s->apcie_st.valid) {
		pr_err("t6040-sleep: refused: ANS would stop but the storage-apcie windows/tunables are absent (apple,apcie)\n");
		refused++;
	}

	/* The engine waits for the display CPUs with no timeout. */
	if (t6040_sleep_log_groups(s))
		refused++;

	return refused;
}

/*
 * The AOP and audio island's power words (MiniPMGR 0x508304000 + 0x000..0x158). The island
 * is off after a sleep, and the AOP's resume, which reads its ASC first, then faulted on the
 * J616s. The words are read in .begin and put back at the wake, parents first: a word that
 * was on goes to a manual target 0xf (in automatic mode it gated again before the AOP's
 * driver got to its ASC), any other gets its saved value if that asks for power. The table
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
		s->aop_ps_saved[i] = readl(s->aop_ps + 8 * i);
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
			if (FIELD_GET(PS_ACTUAL, now) == 0xf && !(now & PS_AUTO_ENABLE))
				continue;
			writel((now & ~(PS_AUTO_ENABLE | PS_TARGET)) | PS_TARGET, r);
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
	if (!s->pmgr_east)
		return;
	for (i = 0; i < ARRAY_SIZE(s->sio_ps_saved); i++)
		s->sio_ps_saved[i] = readl(s->pmgr_east + PS_SIO_FIRST + i * PS_STRIDE);
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
		void __iomem *w = s->pmgr_east + off;
		u32 saved = s->sio_ps_saved[i];

		if (FIELD_GET(PS_TARGET, saved) != 0xf || FIELD_GET(PS_ACTUAL, saved) != 0xf)
			continue;
		val = readl(w);
		if (FIELD_GET(PS_TARGET, val) == 0xf)
			continue;
		writel((val & ~PS_TARGET) | FIELD_PREP(PS_TARGET, 0xf), w);
		/* Timekeeping is still suspended here: count the wait, no ktime. */
		for (n = 0; n < 1000; n++) {
			cur = readl(w);
			if (FIELD_GET(PS_ACTUAL, cur) == 0xf)
				break;
			udelay(10);
		}
		if (n == 1000)
			failed++;
		raised++;
		pr_debug("t6040-sleep: wake: SOC-East power word +%#x: %#x at the commit, %#x after the wake, now %#x\n",
			 off, saved, val, cur);
	}
	if (failed)
		pr_warn("t6040-sleep: wake: %u of %u SIO power word(s) raised again did not come on\n",
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
 * The hardware S2R-lo entry counter: the count is PMGR
 * regmap 1 + 0x70970 shifted right by 3; the platform reads
 * 0x508370970 at quiesce. It tells "the SoC entered S2R" from "the
 * engine never finished". Read-only, with the AOP-global S2R control, wake masks and
 * wake status, at debug level: at probe, for the boot after a failed sleep, and in a dry run.
 * Mapped once at probe: the dry run reads it with interrupts off.
 */
#define T6040_S2R_LO_COUNT_PA	0x508370970ULL

static void t6040_sleep_log_s2r_state(struct t6040_sleep *s, const char *when)
{
	u32 raw = s->s2r_lo_count ? readl(s->s2r_lo_count) : 0;
	u32 m[AOP_WAKE_WORDS] = {}, st[AOP_WAKE_WORDS] = {}, ctl = 0;
	int i;

	if (s->aop_global) {
		ctl = readl(s->aop_global + AOP_S2R_CTRL);
		for (i = 0; i < AOP_WAKE_WORDS; i++) {
			m[i] = readl(s->aop_global + AOP_WAKE_MASK(i));
			st[i] = readl(s->aop_global + AOP_WAKE_STATUS(i));
		}
	}
	dev_dbg(s->dev, "S2R state at %s: S2R-lo count %u (raw %#x), AOP-global ctl %#x\n",
		when, raw >> 3, raw, ctl);
	dev_dbg(s->dev, "S2R state at %s: wake masks %*ph\n", when,
		(int)sizeof(m), m);
	dev_dbg(s->dev, "S2R state at %s: wake status %*ph\n", when,
		(int)sizeof(st), st);
}

/*
 * The 15 axi2af fabric-bridge retention domains (the
 * table for the axi2af workaround). Each domain's power word is in
 * SOC (soc_ps, base 0x502800000) or SOC-East (soc_east, base 0x502280000); its
 * fabric bridge is a separate window. On wake every domain that is powered up
 * (PS TARGET == 0xf) gets bridge +0x104/+0x108 bit0 set, or the bridge drops
 * its traffic (the ANS/Wi-Fi/display-dead signature).
 */
static const struct t6040_axi2af_domain {
	const char *name;
	bool east;		/* power word in soc_east (true) or soc_ps (false) */
	u32 ps_off;
	phys_addr_t bridge_pa;
} t6040_axi2af[T6040_AXI2AF_DOMAINS] = {
	{ "atc0_common",  false, 0x390, 0x380000000 },
	{ "atc1_common",  false, 0x3b8, 0x388000000 },
	{ "atc2_common",  false, 0x3e0, 0x390000000 },
	{ "atc3_common",  false, 0x408, 0x398000000 },
	{ "disp_sys",     true,  0x258, 0x403000000 },
	{ "dispext0_sys", false, 0x340, 0x3a3000000 },
	{ "dispext1_sys", false, 0x490, 0x603000000 },
	{ "dispext2_sys", false, 0x4e0, 0x607000000 },
	{ "dispext3_sys", false, 0x2d8, 0x307000000 },
	{ "ans",          true,  0x370, 0x40b000000 },
	{ "avd_sys",      false, 0x170, 0x280000000 },
	{ "venc0_sys",    false, 0x278, 0x282000000 },
	{ "venc1_sys",    false, 0x2a0, 0x284000000 },
	{ "jpg",          true,  0x3a0, 0x486000000 },
	{ "apcie_sys_gp", true,  0x390, 0x416000000 },
};

/* Only with the PMU's bus mapped. */
static void t6040_pmu_write(struct t6040_sleep *s, u16 reg, u8 val)
{
	if (s->spmi0)
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
 * The five complexes off, as the quiesce loop turns cluster c off and waits if c > 3,
 * for c = 1..5 (measured on the J616s): each slot's low nibble -> 0, in order +0x0 .. +0x10,
 * and the last two (+0xc, +0x10) polled until BIT4 clears (+0xc read 0x2f ->
 * 0x20 -> 0x10 -> 0x0 there). The CPU complexes (E, P0, P1) go down at the WFI.
 */
static void t6040_clusters_off(struct t6040_sleep *s)
{
	u32 val;
	int i;

	if (!s->cluster_ctl)
		return;
	for (i = 0; i < CLUSTER_CTL_NR; i++) {
		val = readl(s->cluster_ctl + CLUSTER_CTL(i));
		s->cluster_saved[i] = val;
		writel(val & ~CLUSTER_CTL_NIBBLE, s->cluster_ctl + CLUSTER_CTL(i));
		if (i >= CLUSTER_CTL_FIRST_WAIT)
			readl_poll_timeout_atomic(s->cluster_ctl + CLUSTER_CTL(i), val,
						  !(val & CLUSTER_CTL_BUSY), 1, CLUSTER_TIMEOUT_US);
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

	if (s->soc_east) {
		t6040_east_clk_quiesce(s, SOC_EAST_CLK_A);
		t6040_east_clk_quiesce(s, SOC_EAST_CLK_B);
	}
	if (!s->clkgen)
		return;
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

	if (!s->clkgen || !s->clkgen_saved)
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

	if (!s->soc_ps)
		return;
	for_each_present_cpu(cpu) {
		u32 bit = t6040_cpu_start_bit(cpu_logical_map(cpu));

		if (!bit || bit == self)
			continue;
		readl_poll_timeout_atomic(s->soc_ps + 8 * __ffs(bit), v,
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
	writel((val & ~AFR_LEVEL) | FIELD_PREP(AFR_LEVEL, 4), s->afr_ps);
	if (readl_poll_timeout_atomic(s->afr_ps, cur, FIELD_GET(AFR_LEVEL, cur) == 4, 1,
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

		was |= (u64)(va & 1) << i | (u64)(vb & 1) << (i + KA_WORDS);
		writel((va & ~1U) | (on && (mask & BIT_ULL(i))), a);
		writel((vb & ~1U) | (on && (mask & BIT_ULL(i + KA_WORDS))), b);
	}
	return was;
}

/* A domain's power word, from whichever window holds it. */
static void __iomem *t6040_axi2af_ps(struct t6040_sleep *s, const struct t6040_axi2af_domain *d)
{
	void __iomem *base = d->east ? s->soc_east : s->soc_ps;

	return base ? base + d->ps_off : NULL;
}

/* The preflight's list of the axi2af domains' power words. */
static void t6040_axi2af_show(struct t6040_sleep *sleep, struct seq_file *s)
{
	int i;

	seq_puts(s, "axi2af domains (the quiesce gate wants every one off):\n");
	for (i = 0; i < T6040_AXI2AF_DOMAINS; i++) {
		void __iomem *ps = t6040_axi2af_ps(sleep, &t6040_axi2af[i]);
		u32 val = ps ? readl(ps) : 0;

		seq_printf(s, "  %-13s %#010x%s\n", t6040_axi2af[i].name, val,
			   ps ? (FIELD_GET(PS_ACTUAL, val) ? "  ON" : "") : "  (unmapped)");
	}
}

/*
 * Read-only axi2af gate: on entry the platform panics unless every reachable domain's
 * power word reads [7:4]==0. Here the ones still on are only named, at debug level.
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
			pr_debug("t6040-sleep: axi2af %s still on (%#x) at commit\n",
				 t6040_axi2af[i].name, val);
			on++;
		}
	}
	if (on)
		pr_debug("t6040-sleep: %d axi2af domain(s) still powered at commit\n", on);
}

/*
 * The wake-side axi2af re-arm: every retained domain powered back up (PS TARGET 0xf and
 * ACTUAL on) gets its fabric bridge +0x104/+0x108 bit0 set, or the bridge drops its
 * traffic. A word in automatic mode can read TARGET 0xf with its domain gated; the bridge
 * is then off too (the first resume on the J616s took an SError in this step), so such a
 * domain is left alone.
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
		if (FIELD_GET(PS_TARGET, val) != 0xf)
			continue;
		if (!FIELD_GET(PS_ACTUAL, val)) {
			pr_debug("t6040-sleep: wake: axi2af %s not re-armed, PS %#x (gated)\n",
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

	if (!s->aop_global)
		return;
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

	if (!s->aop_global || !s->wake_mask_saved_valid)
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
	return apple_sleep_ps_raise_start(base + offset);
}

static void t6040_ps_raise_finish(void __iomem *base, u32 offset)
{
	apple_sleep_ps_raise_finish(base + offset);
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

/* Read-modify-write a tunable in table order, writing only where it changes. */
static void t6040_tunable_apply_changed(void __iomem *regs, const struct apple_tunable *t)
{
	if (t)
		apple_sleep_tunable_apply_changed(regs, t);
}

/*
 * ENTRY: quiesce ANS's storage link, once ANS has been stopped by its driver. The PHY, the
 * clock generator and the AXI enables are treated as shared by the two ST lanes.
 *
 * The power words of this group are left to the SoC's sleep engine, which powers it down
 * itself and in its own order: APCIE_PHY_SW to clock-gated, APCIE_SYS_ST0/1 off, APCIE_ST0/1
 * clock-gated, ANS off, APCIE_SYS_GP off, APCIE_GP clock-gated. It polls APCIE_PHY_SW for
 * its parents, which are both SYS_ST lanes: with them lowered here, as the T8140 controller
 * does, it waited there for good on the J616s. So the commit raises APCIE_SYS_ST1, which
 * Linux never uses, and does only what the engine needs before it runs: the shared PHY put
 * to sleep, APCIE_PHY_SW clock-gated while APCIE_GP is still on, the PCIe PLL shut down.
 */

/* The shared PHY and PHY-IP to sleep (the root complex disable; it polls unbounded). */
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

/* APCIE_PHY_SW to clock-gated (0x0f000244), its busy bit polled clear. */
static void t6040_apcie_phy_sw_gate(struct t6040_sleep *s)
{
	void __iomem *reg = s->soc_east + PS_APCIE_PHY_SW;
	u32 val;

	writel(0x0f000244, reg);
	readl_poll_timeout_atomic(reg, val, !(val & PS_BUSY), 1, PS_OFF_TIMEOUT_US);
}

/* The PCIe PLL shutdown: the clock generator's control word. */
static void t6040_apcie_pll_off(struct t6040_sleep *s)
{
	void __iomem *clk = s->apcie_st.clkgen + APCIE_CLK_CTRL;
	u32 val;

	writel((readl(clk) & ~GENMASK(5, 3)) | BIT(4) | BIT(3), clk);
	writel(readl(clk) | BIT(1), clk);
	writel(readl(clk) & ~BIT(0), clk);
	readl_poll_timeout_atomic(clk, val, !(val & APCIE_CLK_BUSY), 1, APCIE_POLL_US);
	writel(readl(clk) | BIT(2), clk);
}

/*
 * The sleep engine hands each ATC port to its always-on side and then powers the port's
 * common domain off itself: it needs the four ATC commons on at that
 * point. With them off it stalled on the J616s raising ATC0_COMMON, whose parent FAB2_SOC
 * was off as well. FAB2_SOC is raised by hand, then ATC0..3_COMMON to a manual target 0xf,
 * then FAB2_SOC gets its automatic mode back: the powered children hold it up.
 */
#define PS_FAB2_SOC		0x338

static const u16 t6040_atc_common[4] = { 0x390, 0x3b8, 0x3e0, 0x408 };

static void t6040_atc_commons_on(struct t6040_sleep *s)
{
	void __iomem *b = s->pmgr_soc;
	int i;

	if (!b)
		return;
	t6040_ps_raise_start(b, PS_FAB2_SOC);
	for (i = 0; i < 4; i++)
		t6040_ps_raise_start(b, t6040_atc_common[i]);
	t6040_ps_raise_finish(b, PS_FAB2_SOC);
}

/*
 * The CFG writes of the PHY sleep entry, run on every ATC port before a sleep:
 * ATCPHY_CFG +0x2c bit 0 (S2R_AON_SYNC_RST_N), +0x08 bit 0 (CFG_S2R_FSM_EN) and +0x28 bit 0
 * (AHB_CLK_REQ_N) set, then +0x04 bit 8 (the clock's ack) polled. Without them the engine
 * stalled on the J616s on a port that carries DisplayPort lanes: port 3 (the HDMI output)
 * always, any other port once a display runs on it in DP alternate mode. A parked port, and
 * one its PHY driver has put to sleep, already has +0x28 bit 0 set and is left alone.
 */
static void t6040_atc_s2r_entry(struct t6040_sleep *s)
{
	u32 v;
	int n;

	for (n = 0; n < 4; n++) {
		void __iomem *c = s->atc_cfg[n];

		if (!c || (readl(c + 0x28) & BIT(0)))
			continue;
		writel(readl(c + 0x2c) | BIT(0), c + 0x2c);
		udelay(1);
		writel(readl(c + 0x08) | BIT(0), c + 0x08);
		udelay(5);
		writel(readl(c + 0x28) | BIT(0), c + 0x28);
		readl_poll_timeout_atomic(c + 0x04, v, v & BIT(8), 1, 1000);
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
		void __iomem *b = t6040_fabrics[i].east ? s->pmgr_east : s->pmgr_soc;
		u32 off = t6040_fabrics[i].off, v;

		if (!b)
			continue;
		v = readl(b + off);
		if (!(v & PS_AUTO_ENABLE) || FIELD_GET(PS_TARGET, v) != 0xf)
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
 * the ps_dcs_* domains disabled: target 0xf, no automatic mode), and the engine then waited
 * for the first DCS pair's power gate for good on the J616s. Both are applied in each .begin.
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
	void __iomem *b = t6040_dcs_ps[i].east ? s->pmgr_east : s->pmgr_soc;

	return b ? b + t6040_dcs_ps[i].off : NULL;
}

/*
 * In order: the minimum state cleared, target 0xf, ACTUAL 0xf polled, then automatic
 * mode. Returns a mask of the DCS whose ACTUAL did not reach 0xf.
 */
static u32 t6040_dcs_ps_auto_on(struct t6040_sleep *s)
{
	u32 to = 0, v;
	int i;

	for (i = 0; i < 16; i++) {
		void __iomem *r = t6040_dcs_ps_reg(s, i);

		if (!r)
			continue;
		v = readl(r);
		if ((v & PS_AUTO_ENABLE) && !(v & GENMASK(19, 16)))
			continue;
		writel((v & ~(GENMASK(19, 16) | PS_FLAGS | PS_TARGET)) | 0xf, r);
		if (readl_poll_timeout_atomic(r, v, FIELD_GET(PS_ACTUAL, v) == 0xf, 1,
					      PS_OFF_TIMEOUT_US))
			to |= BIT(i);
		writel(readl(r) | PS_AUTO_ENABLE, r);
	}
	return to;
}

static void t6040_dcs_dyn_pg(struct t6040_sleep *s)
{
	int i;

	for (i = 0; i < 16; i++) {
		u32 v;

		if (!s->dcs[i])
			continue;
		v = readl(s->dcs[i] + 0x20);
		if ((v & 0x1100) != 0x1100)
			writel(v | 0x1100, s->dcs[i] + 0x20);
	}
}

/*
 * ANS's own fabric bridge (0x4_0b00_0000 +0x104/+0x108 bit 0). The bit is cleared before a
 * retained domain goes off and set again once it is back up; here the wake sets it
 * (t6040_sleep_raise_apcie_st()) and the SoC's sleep engine, not this driver, takes ANS down.
 * The boot loader leaves the bit clear, so a boot's first sleep goes down with it clear;
 * without this the next one meets the bridge as the first wake armed it. Only while ANS is
 * on: the bridge sits in it. Returns whether the bit was set.
 */
static bool t6040_ans_bridge_quiesce(struct t6040_sleep *s)
{
	void __iomem *bridge = s->axi2af_bridge[AXI2AF_ANS];
	u32 a, b;

	if (!bridge || !s->soc_east ||
	    FIELD_GET(PS_ACTUAL, readl(s->soc_east + PS_ANS)) != 0xf)
		return false;
	a = readl(bridge + AXI2AF_BRIDGE_104);
	b = readl(bridge + AXI2AF_BRIDGE_108);
	if (!((a | b) & BIT(0)))
		return false;
	writel(a & ~BIT(0), bridge + AXI2AF_BRIDGE_104);
	writel(b & ~BIT(0), bridge + AXI2AF_BRIDGE_108);

	return true;
}

static void t6040_sleep_lower_apcie_st(struct t6040_sleep *s)
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
	s->ans_bridge_quiesced = t6040_ans_bridge_quiesce(s);
}

static void t6040_bridge_rearm(void __iomem *bridge)
{
	if (!bridge)
		return;
	writel(readl(bridge + AXI2AF_BRIDGE_104) | BIT(0), bridge + AXI2AF_BRIDGE_104);
	writel(readl(bridge + AXI2AF_BRIDGE_108) | BIT(0), bridge + AXI2AF_BRIDGE_108);
}

/*
 * RESUME: raise ANS's storage link from power-off, the two-lane adaptation of
 * sleep.c's apple_sleep_raise_apcie(). Runs atomic on the waking core in
 * t6040_sleep_return(), AFTER t6040_axi2af_rearm() and BEFORE control returns to
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
static int t6040_sleep_raise_apcie_st(struct t6040_sleep *s)
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
	 * target 0xf: in automatic mode it gated again on the J616s, and the DART behind it
	 * then faulted in its resume.
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
	t6040_tunable_apply_changed(a->phy_ip, a->phy_ip_pll);
	t6040_tunable_apply_changed(a->phy_ip, a->phy_ip_lanes);
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
 * the T6040 AON WDT (0x50836c000), used only when the DT's wdt block is that one.
 */
#define T6040_WDT_PA		0x50836c000ULL
asm(
"	.pushsection .text, \"ax\"\n"
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
extern char t6040_resume_stub[];

/*
 * The four ATC complexes' power words in SOC (soc_ps, 0x502800000), deepest first: cio_usb,
 * cio_pcie, cio, pcie, common (ADT: ATCn_CIO_USB .. ATCn_COMMON). The quiesce gate wants
 * them off; their drivers don't lower atcN_common.
 *
 * Their common_dp, usb, phymxwrap and usb_aon words are MiniPMGR words (0x508300000 + 0xa0..,
 * ADT ATCn_COMMON_DP/USB/PHYMXWRAP/USB_AON), not SOC ones: applied to soc_ps,
 * those offsets lower SOC AIC (0x118), PMC (0x120), PMS_FPWM0-4 and
 * FAB0_SOC (0x168) right before the S2R request; the dead PMC then blocks the sleep engine's
 * PMC writes (an AP read of the PMC hangs from that step on).
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

static void t6040_sleep_lower_atc(struct t6040_sleep *s)
{
	int p, k;

	if (!s->soc_ps)
		return;
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
	s->gp_appclk_saved[p] = readl(port + GP_PORT_APPCLK);
	s->gp_perst_saved[p] = readl(port + GP_PORT_PERST);
	s->gp_saved[p] = true;
	writel(readl(port + GP_PORT_INTMSK) | GP_PORT_INT_LINK, port + GP_PORT_INTMSK);
	writel(GP_PORT_INT_LINK, port + GP_PORT_INTSTAT);
	writel(readl(port + GP_PORT_APPCLK) | GP_PORT_APPCLK_CGDIS, port + GP_PORT_APPCLK);
	if (readl(port + GP_PORT_LINKSTS) & GP_PORT_LINKSTS_UP) {
		writel(GP_PORT_L2_ENTER, port + GP_PORT_L2);
		readl_poll_timeout_atomic(port + GP_PORT_L2, val, !(val & GP_PORT_L2_BUSY), 10,
					  GP_STOP_US);
	}
	if (s->gp_pins && s->gp_perst[p] >= 0) {
		void __iomem *reg = s->gp_pins + 4 * s->gp_perst[p];

		val = readl(reg);
		writel(s->gp_perst_low[p] ? val & ~GP_PIN_DATA : val | GP_PIN_DATA, reg);
	}
	writel(readl(phy) & ~GP_PHY_LINK, phy);
	udelay(1000);
	/* What this bit does isn't known. It is set around PERST# (T8140). */
	writel(readl(port + GP_PORT_PERST_GUARD) | BIT(8), port + GP_PORT_PERST_GUARD);
	writel(0, port + GP_PORT_PERST);
	writel(readl(port + GP_PORT_PERST_GUARD) & ~BIT(8), port + GP_PORT_PERST_GUARD);
	writel(readl(phy) & ~GP_PHY_EN1, phy);
	writel(readl(phy) & ~GP_PHY_EN0, phy);
	writel(readl(phy) | GP_PHY_RESET, phy);
	writel(readl(phy) & ~GP_PHY_CLK0REQ, phy);
	readl_poll_timeout_atomic(phy, val, !(val & GP_PHY_CLK0ACK), 1, GP_STOP_US);
	writel(readl(phy) & ~GP_PHY_CLK1REQ, phy);
	writel(readl(port + GP_PORT_APPCLK) & ~GP_PORT_APPCLK_EN, port + GP_PORT_APPCLK);
	writel(readl(port + GP_PORT_INTSTAT), port + GP_PORT_INTSTAT);
}

/* The GP host's two ports, while the block's domains are all still on. */
static void t6040_sleep_stop_gp_ports(struct t6040_sleep *s)
{
	int p;

	if (!s->soc_east || !FIELD_GET(PS_ACTUAL, readl(s->soc_east + PS_APCIE_SYS_GP)))
		return;
	for (p = 0; p < 2; p++)
		t6040_gp_port_stop(s, p);
}

/*
 * apcie_sys_gp (AUTO off, axi2af bit0s, target 0), then apcie_gp. After ANS's ST lanes: the
 * storage lanes' cio3pll/clkgen/PHY windows sit in apcie_gp, and touching them with it off
 * hung the bus. T8140's order: SYS_ST, ST, SYS_GP, GP. APCIE_GP goes to clock-gated
 * (0x244), the state the sleep engine itself asks for, not to off.
 */
static void t6040_sleep_lower_apcie_gp(struct t6040_sleep *s)
{
	void __iomem *sys, *gp, *bridge = s->axi2af_bridge[AXI2AF_APCIE_SYS_GP];
	u32 val;

	if (!s->soc_east)
		return;
	sys = s->soc_east + PS_APCIE_SYS_GP;
	gp = s->soc_east + PS_APCIE_GP;
	if (!FIELD_GET(PS_ACTUAL, readl(sys)))
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

	writel(0x244, gp);
	readl_poll_timeout_atomic(gp, val, FIELD_GET(PS_ACTUAL, val) == 4, 1, PS_OFF_TIMEOUT_US);
}

/*
 * The real commit (t6040_sleep_commit, reached through cpu_suspend): t6040_sleep_enter
 * calls it only when no blocker remains and allow_entry is set. In order: the stopped CPUs
 * released to their last WFI, the ATC complexes and the GP PCIe ports lowered, ANS's storage
 * link quiesced, WD0 off and the AON WD2 net armed, the PMU's boot stage 0x50 and
 * NOTIFY_ENABLE over spmi0/spmi-a0, the steps of the PMGR quiesce (APSC, PLL relock,
 * TVMs, the AGX gate, the five complexes, the axi2af read-check, the clock quiesce, the
 * keep-alive bits), aura's mailbox armed with the resume entry, the fabrics and the ATC
 * ports readied for the sleep engine, the MasterMini S2R timer (0x508904000 := 1), the AIC
 * masked and the deep WFI. The watchdog, PMU, cluster, axi2af and timer writes are as
 * measured on the J616s. A hang here ends at WD2.
 */
static int t6040_sleep_commit(unsigned long unused)
{
	struct t6040_sleep *s = t6040_sleep;

	s->siq_cfg = read_sysreg_s(SYS_APL_SIQ_CFG_EL1);
	spin_table_stop_parked();
	/*
	 * On the platform every other core is off, in its last WFI, before the boot core turns the
	 * complexes off. With the stopped cores still spinning the sleep engine stalled on the
	 * J616s: release them now, and wait for their power words to read off further down.
	 */
	spin_table_release_stopped();
	t6040_sleep_lower_atc(s);
	t6040_sleep_stop_gp_ports(s);

	/*
	 * ANS's storage link, once ANS has been stopped by its driver (see
	 * t6040_apcie_phy_off). It follows every refusal, so a refused sleep finds
	 * the link up. No-op unless the apple,apcie windows are present (apcie_st.valid).
	 */
	if (s->soc_east)
		t6040_ps_raise_start(s->soc_east, PS_APCIE_SYS_ST1);
	if (s->pcie_ans_stopped)
		t6040_sleep_lower_apcie_st(s);
	t6040_sleep_lower_apcie_gp(s);

	/* WD0 off, then the AON WD2 net (5 s covers the reconfig). */
	if (s->wdt)
		writel(0, s->wdt + WDT_WD0_CTL);
	t6040_sleep_set_wd2(s, clamp(READ_ONCE(sleep_wd_secs), 1U, WDT_MAX_SECS));
	/* And the SMC's AP watchdog deadline off, right after the WD2 (measured). */
	if (s->smc_wdt_deadline)
		writel(0, s->smc_wdt_deadline);

	/* PMU boot stage 0x50 and NOTIFY_ENABLE on both SPMI controllers (measured). */
	t6040_pmu_write(s, PMU_BOOT_STAGE, PMU_BOOT_STAGE_SLEEP);
	if (s->spmi0)
		writel(SPMI_NOTIFY_ENABLE_SLEEP, s->spmi0 + SPMI_NOTIFY_ENABLE);
	if (s->spmi_a0)
		writel(SPMI_NOTIFY_ENABLE_SLEEP, s->spmi_a0 + SPMI_NOTIFY_ENABLE);

	/*
	 * The order of the PMGR quiesce (the cluster and axi2af
	 * parts measured on the J616s): the five complexes off, the axi2af gate read,
	 * the clock quiesce, the keep-alive bits off, then the S2R timer. The CPM
	 * pwrgate voters are not written on entry: they read back 0x80000000
	 * already; the wake side re-asserts them.
	 */
	t6040_secondaries_off(s);
	t6040_pmgr_quiesce(s);
	t6040_agx_gate(s);
	t6040_clusters_off(s);

	/* Read-only: the axi2af panic gate checked before the engine goes. */
	t6040_axi2af_readcheck(s);
	t6040_clkgen_park(s);
	s->ka_on_before = t6040_keep_alive(s, false, 0);

	/* Arm aura's AURAS2R1 mailbox with this boot's resume entry. */
	if (s->mbox) {
		WRITE_ONCE(s->mbox->resume_pa,
			   s->wdt_stub_ok ?
			   __pa_symbol(t6040_resume_stub) : __pa_symbol(cpu_resume));
		dcache_clean_poc((unsigned long)s->mbox, (unsigned long)(s->mbox + 1));
		dsb(sy);
	}

	t6040_fabrics_on(s);
	t6040_sio_ps_save(s);

	/* The commit: the MasterMini S2R timer arms the SoC sleep engine. */
	if (s->mastermini)
		writel(1, s->mastermini + MASTERMINI_S2R_TIMER);
	dsb(sy);

	/* A raised line would end the WFI the engine waits for. */
	aic_sleep_mask_all();
	spin_table_release_stopped();

	t6040_deep_wfi();
}

/*
 * The GP PCIe host's two ports (Wi-Fi/BT, SD): the boot loader set their clocks up, and
 * nobody does after a sleep. The DART in port 0's block then faulted in its noirq resume on
 * the J616s. t6040_gp_port_stop() backwards, short of the link: the lane reference clocks
 * requested and acknowledged, the PHY's enables and the port's application clock as saved,
 * the controller's PERST register as saved, then the port's ready bit polled. The link
 * itself stays down for the host driver.
 */
#define GP_PHY_CLK1ACK		BIT(3)
#define GP_PORT_STATUS		0x804
#define GP_PORT_STATUS_READY	BIT(0)

static void t6040_gp_pcie_clk(struct t6040_sleep *s)
{
	unsigned int timeouts = 0;
	u32 val;
	int i;

	for (i = 0; i < 2; i++) {
		void __iomem *phy = s->gp_phy[i], *port = s->gp_port[i];
		u32 cfg, app;

		if (!phy || !port || !s->gp_saved[i])
			continue;
		cfg = s->gp_phy_saved[i];
		app = s->gp_appclk_saved[i];
		writel(readl(phy) | GP_PHY_CLK0REQ, phy);
		if (readl_poll_timeout_atomic(phy, val, val & GP_PHY_CLK0ACK, 1, 50000))
			timeouts++;
		writel(readl(phy) | GP_PHY_CLK1REQ, phy);
		if (readl_poll_timeout_atomic(phy, val, val & GP_PHY_CLK1ACK, 1, 50000))
			timeouts++;
		if (!(cfg & GP_PHY_RESET)) {
			writel(readl(phy) & ~GP_PHY_RESET, phy);
			udelay(1);
		}
		writel(readl(phy) | (cfg & (GP_PHY_EN0 | GP_PHY_EN1)), phy);
		writel(readl(phy) | (cfg & GP_PHY_LINK), phy);
		writel((readl(port + GP_PORT_APPCLK) & ~(GP_PORT_APPCLK_EN | GP_PORT_APPCLK_CGDIS)) |
		       (app & (GP_PORT_APPCLK_EN | GP_PORT_APPCLK_CGDIS)), port + GP_PORT_APPCLK);
		/*
		 * The stop also wrote the controller's PERST register 0, and without it the
		 * port does not report ready. Put it back as it was (the endpoint's PERST# pin
		 * stays asserted), with the guard bit set around it.
		 */
		writel(readl(port + GP_PORT_PERST_GUARD) | BIT(8), port + GP_PORT_PERST_GUARD);
		writel(s->gp_perst_saved[i], port + GP_PORT_PERST);
		writel(readl(port + GP_PORT_PERST_GUARD) & ~BIT(8), port + GP_PORT_PERST_GUARD);
		if (readl_poll_timeout_atomic(port + GP_PORT_STATUS, val, val & GP_PORT_STATUS_READY,
					      1, 50000))
			timeouts++;
	}
	s->gp_clk_timeouts = timeouts;
}

/*
 * The wake return (t6040_sleep_return), reached on the boot core after aura's
 * AURAS2R1 mailbox hands it back to cpu_resume. aura restores NO SoC register;
 * every write here is the OS driver's job. It is the
 * mirror of the PMGR wake restore: the three CPU complexes on, the two wake
 * pwrgates, SOC-East retention off (with the PARENT_MISSING poll), the axi2af
 * fabric-bridge re-arm, the CPM voters re-asserted, the clk mux restored, the
 * AP-global S2R control cleared, and the wake-status words read. Between them,
 * what a Linux sleep needs on top: ANS's storage link, the GP PCIe ports' clocks
 * and the AOP island's power words.
 */
static void t6040_sleep_return(struct t6040_sleep *s)
{
	u32 val;
	int i, ret;

	/* The reset left the timer and IRQ routing at their EL2 defaults. */
	write_sysreg(s->cntkctl, cntkctl_el1);
	isb();
	write_sysreg_s(s->siq_cfg, SYS_APL_SIQ_CFG_EL1);
	t6040_wake_sync();

	spin_table_stop_finished();
	t6040_wake_sync();
	aic_sleep_restore();
	t6040_wake_sync();

	/*
	 * The three CPU complexes back on (E, P0, P1), nibble 0xf, poll +0xc for
	 * the ready bit. The platform then also writes 1 to register 0xe440f8 of each
	 * complex. That is not done here: the secondaries come up through the
	 * normal per-CPU start afterwards, and all 14 CPUs of the J616s are
	 * online after a wake without it.
	 */
	if (s->cluster_ctl)
		for (i = 0; i < CLUSTER_CTL_NR; i++) {
			u32 on = i < CLUSTER_CTL_FIRST_WAIT ? CLUSTER_CTL_ON :
				 s->cluster_saved[i] & CLUSTER_CTL_NIBBLE;

			val = readl(s->cluster_ctl + CLUSTER_CTL(i));
			writel((val & ~CLUSTER_CTL_NIBBLE) | on, s->cluster_ctl + CLUSTER_CTL(i));
		}
	t6040_wake_sync();

	/* Wake pwrgates and CPM voters: read, set BIT31. */
	if (s->pwrgate) {
		writel(readl(s->pwrgate + PWRGATE_WAKE_0) | PWRGATE_ON, s->pwrgate + PWRGATE_WAKE_0);
		writel(readl(s->pwrgate + PWRGATE_WAKE_1) | PWRGATE_ON, s->pwrgate + PWRGATE_WAKE_1);
	}
	t6040_wake_sync();

	/* SOC-East retention off: clear DEV_DISABLE, then poll PARENT_MISSING clear. */
	if (s->soc_east) {
		val = readl(s->soc_east + SOC_EAST_RETENTION);
		writel(val & ~SOC_EAST_DEV_DISABLE, s->soc_east + SOC_EAST_RETENTION);
		ret = readl_poll_timeout_atomic(s->soc_east + SOC_EAST_RETENTION, val,
						!(val & SOC_EAST_PARENT_MISSING), 1,
						SOC_EAST_TIMEOUT_US);
		if (ret)
			pr_warn("t6040-sleep: SOC-East PARENT_MISSING stuck on wake (%#x)\n", val);
	}
	t6040_wake_sync();

	/*
	 * Bit 0 of the SoC's 14 energy accumulators could be set again here. They
	 * only account energy and nothing in this kernel reads them, so they are
	 * left as the power loss left them.
	 */

	/* The fabric-bridge retention re-arm (the ANS/Wi-Fi/display-dead guard). */
	t6040_axi2af_rearm(s);
	t6040_wake_sync();

	/*
	 * Raise ANS's two ST lanes + shared PHY from power-off, AFTER the fabric
	 * bridges are re-armed and BEFORE control returns to the PM core, so
	 * nvme's resume_early sets the ANS run bit onto a live link.
	 * No-op unless the apple,apcie windows are present.
	 */
	s->apcie_wake_step = 0;
	s->pcie_ans_on_ret = 0;
	if (s->pcie_ans_stopped)
		s->pcie_ans_on_ret = t6040_sleep_raise_apcie_st(s);
	t6040_gp_pcie_clk(s);
	t6040_wake_sync();
	t6040_aop_ps_restore(s);
	t6040_wake_sync();

	if (s->pwrgate) {
		writel((readl(s->pwrgate + PWRGATE_CPM_0) & ~PWRGATE_ON) | PWRGATE_ON,
		       s->pwrgate + PWRGATE_CPM_0);
		writel((readl(s->pwrgate + PWRGATE_CPM_1) & ~PWRGATE_ON) | PWRGATE_ON,
		       s->pwrgate + PWRGATE_CPM_1);
		writel((readl(s->pwrgate + PWRGATE_CPM_2) & ~PWRGATE_ON) | PWRGATE_ON,
		       s->pwrgate + PWRGATE_CPM_2);
	}
	t6040_wake_sync();

	/* Undo the entry clk-mux park; the clock restore is otherwise a NOP. */
	t6040_clkgen_restore(s);
	t6040_wake_sync();

	/* Software ack of the armed S2R request (the S2R timer has no stop). */
	if (s->aop_global)
		writel(0, s->aop_global + AOP_S2R_CTRL);
	t6040_wake_sync();

	/* The keep-alive bits the commit cleared, back (the wake restore enables them). */
	if (s->ka_on_before)
		t6040_keep_alive(s, true, s->ka_on_before);
	t6040_wake_sync();

	/* A longer WD2 over the device resume, until .end. */
	t6040_sleep_set_wd2(s, WDT_SUSPEND_SECS);
	t6040_wake_sync();

	if (s->mbox) {
		dcache_inval_poc((unsigned long)s->mbox, (unsigned long)(s->mbox + 1));
		pr_debug("t6040-sleep: wake: resumes now %llu\n", READ_ONCE(s->mbox->resumes));
	}
	if (s->aop_global)
		for (i = 0; i < AOP_WAKE_WORDS; i++)
			s->wake_status[i] = readl(s->aop_global + AOP_WAKE_STATUS(i));
	s->returned = true;
}

/*
 * Offer "deep" only when the system was booted with allow_entry: without it
 * every suspend to RAM is refused, so mem_sleep keeps s2idle as its default.
 */
static int t6040_sleep_valid(suspend_state_t state)
{
	return state == PM_SUSPEND_MEM && READ_ONCE(allow_entry);
}

/* Set by .begin for a suspend to RAM; .end only undoes what .begin did. */
static bool t6040_sleep_mem_begun;

/*
 * SMC key pmVC, the request for the VDD_CIO rail: the platform writes its enable mask | 0x100
 * (0x101 on the J616s) while an ATC port's always-on side is powered, also at sleep. Linux
 * never asks for the rail, which is power-gated otherwise (ADT vdd-cio-rail-pg), and a rail
 * the SMC drops during the sleep transition races the sleep engine's hand-over of the ATC
 * ports. Written in each .begin.
 */
#define T6040_PMVC_REQUEST	0x101

/* The SMC node is apple,t6041-smc + apple,t8103-smc here (macsmc's match list). */
static const struct of_device_id t6040_smc_match[] = {
	{ .compatible = "apple,t8103-smc" },
	{ .compatible = "apple,smc" },
	{}
};

static void t6040_sleep_pmvc(struct t6040_sleep *s)
{
	struct platform_device *pdev;
	struct device_node *np;
	struct apple_smc *smc;
	int ret = -ENODEV;

	np = of_find_matching_node(NULL, t6040_smc_match);
	pdev = np ? of_find_device_by_node(np) : NULL;
	of_node_put(np);
	smc = pdev ? platform_get_drvdata(pdev) : NULL;
	if (smc)
		ret = apple_smc_write_u32(smc, SMC_KEY(pmVC), T6040_PMVC_REQUEST);
	if (pdev)
		put_device(&pdev->dev);
	if (ret < 0)
		dev_warn(s->dev, "pmVC: write %#x failed: %d\n", T6040_PMVC_REQUEST, ret);
}

/*
 * The refusal-first gate. Default: print the blocker list and return an error
 * here, before any device suspends, so nothing is entered. A dry run proceeds
 * and refuses at .enter. allow_entry with no blocker and no dry run: the
 * path that reaches the commit.
 */
static int t6040_sleep_begin(suspend_state_t state)
{
	struct t6040_sleep *s = t6040_sleep;
	unsigned int disp = 0;
	unsigned int live = t6040_sleep_live_blockers(s, NULL, &disp);
	/* The display pipes are lowered during device suspend (apple-dcp .suspend),
	 * so .begin only gates on the non-display live blockers; .enter re-checks the
	 * full set at the commit point, by which time the display is off. */
	unsigned int begin_total = live - disp;
	unsigned int dry = READ_ONCE(dry_run), secs;
	u32 dcs_to;

	/*
	 * The PM core calls .begin and .end for suspend-to-idle too when there
	 * are no s2idle_ops. s2idle needs nothing from this controller.
	 */
	t6040_sleep_mem_begun = false;
	if (state != PM_SUSPEND_MEM)
		return 0;
	t6040_sleep_mem_begun = true;

	t6040_sleep_set_dry(s, 0);
	s->returned = false;
	s->gp_saved[0] = s->gp_saved[1] = false;
	t6040_aop_ps_save(s);
	t6040_sleep_pmvc(s);
	t6040_dcs_dyn_pg(s);
	dcs_to = t6040_dcs_ps_auto_on(s);
	if (dcs_to)
		pr_warn("t6040-sleep: DCS power words %#x did not read on before automatic mode\n",
			dcs_to);
	if (!begin_total && allow_entry && !dry) {
		/* The non-display blockers are clear and no dry run is asked for: proceed
		 * to device suspend; .enter holds the real commit until the display is
		 * also off. The AON WD2 is the net from here. */
		pr_debug("t6040-sleep: entering: %u display pipe(s) on, to be lowered before the commit\n",
			 disp);
		t6040_sleep_power_ops(s);
		t6040_sleep_set_wd2(s, WDT_SUSPEND_SECS);
		pm_set_suspend_via_firmware();
		return 0;
	}

	pr_info("t6040-sleep: suspend requested: %u live blocker(s) (%u display, cleared during device suspend)\n",
		live, disp);
	t6040_sleep_log_blockers(s);

	if (dry) {
		dry = min(dry, 2U);
		t6040_sleep_power_ops(s);
		if (!s->power_ops)
			pr_warn("t6040-sleep: dry run: no CPU parking power ops, so no hold at the commit point\n");
		t6040_sleep_set_dry(s, dry);
		pm_set_suspend_via_firmware();
		secs = min(READ_ONCE(wd2_secs), WDT_MAX_SECS);
		if (secs && s->wdt)
			t6040_sleep_set_wd2(s, secs);
		else
			secs = 0;
		pr_info("t6040-sleep: dry run %u: firmware-driven device sleep short of %s, refused at the commit point, WD2 %u s (0: off)\n",
			dry, dry >= 2 ? "the stops only a power cycle undoes" :
					"the last steps", secs);
		return 0;
	}

	pr_info("t6040-sleep: refusing the suspend; nothing entered\n");
	return -EOPNOTSUPP;
}

/* After the device suspend, in process context. */
static int t6040_sleep_prepare(void)
{
	t6040_sleep_watch_bound(t6040_sleep);

	return 0;
}

/*
 * A dry run at the commit point, interrupts off and one CPU up: every
 * check a real commit makes, the parked CPUs held and let go, at level 2 each
 * coprocessor's last check, then the refusal. Never reached: the enclave's
 * sleep request, each coprocessor's last stop, the wake masks, the SoC
 * power-down.
 */
static int t6040_sleep_dry_enter(struct t6040_sleep *s, unsigned int dry)
{
	unsigned int refused;
	int ret;

	pr_info("t6040-sleep: dry run %u at the commit point: %u CPU(s) online\n", dry,
		num_online_cpus());
	t6040_sleep_log_s2r_state(s, "dry-run commit point");
	/*
	 * A wake source pending and left unmasked by the masks a real commit arms would keep the
	 * AP partition from powering down, and the sleep engine then stalls. Read only.
	 */
	if (s->aop_global) {
		u32 p[AOP_WAKE_WORDS];
		int i;

		for (i = 0; i < AOP_WAKE_WORDS; i++)
			p[i] = readl(s->aop_global + AOP_WAKE_STATUS(i)) & ~t6040_wake_mask[i];
		pr_info("t6040-sleep: dry-run commit point: wake status left unmasked by the commit's masks %*ph\n",
			(int)sizeof(p), p);
	}
	refused = t6040_sleep_refuse(s);
	pr_info("t6040-sleep: a real commit would %s here (%u check(s) failed)\n",
		refused ? "refuse" : "go on", refused);

	ret = s->power_ops ? spin_table_hold_parked() : -EOPNOTSUPP;
	if (ret)
		pr_err("t6040-sleep: the parked CPUs didn't answer the hold: %d\n", ret);
	else
		pr_info("t6040-sleep: %u parked CPU(s) answered the hold\n",
			num_present_cpus() - num_online_cpus());

	if (dry >= 2 && !ret)
		pr_info("t6040-sleep: the coprocessors' last checks: %d\n",
			apple_sleep_coprocs_check(s->dev));
	if (!ret)
		spin_table_unhold_parked();

	pr_info("t6040-sleep: dry run: refused before the enclave's sleep request (%s), the coprocessors' last stops and the SoC power-down\n",
		apple_sleep_enclave_registered() ? "left by sep" : "none left");

	return -EBUSY;
}

#define PS_RESET_REQ		BIT(12)

#define DISP_CPU_GATE_US	50000

/*
 * regmap_read_poll_timeout_atomic() reads ktime_get(), which warns while timekeeping is
 * suspended (12 warnings from .enter on the J616s). Counted 1 us steps instead.
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
 * AUTO_ENABLE set, take it out of automatic mode with the target kept (v & 0xeffffcff) and
 * wait for ACTUAL 15; then @pre (the SYS hooks); then TARGET 0 with bits 9:8 written 1 and
 * bits 10-12, 24-31 kept; then poll bit11 clear and ACTUAL 0.
 */
static int t6040_psw_off(struct t6040_sleep *s, const struct t6040_psw *w,
			      void (*pre)(struct t6040_sleep *s))
{
	u32 v = t6040_psw_read(w);

	if (v & PS_AUTO_ENABLE) {
		t6040_psw_write(w, v & 0xeffffcff);
		t6040_psw_poll(w, PS_ACTUAL, PS_ACTUAL);
		v = t6040_psw_read(w);
	}
	if (pre)
		pre(s);
	t6040_psw_write(w, (v & 0xfffffcf0) | 0x300);
	t6040_psw_poll(w, PS_BUSY, 0);
	return t6040_psw_poll(w, PS_ACTUAL, 0);
}

/* DISP_SYS's hooks before its PS write: the axi2af bridge bit0s, then the retention pair. */
static void t6040_disp_sys_pre(struct t6040_sleep *s)
{
	void __iomem *bridge = s->axi2af_bridge[AXI2AF_DISP_SYS];

	if (bridge) {
		writel(readl(bridge + AXI2AF_BRIDGE_104) & ~BIT(0), bridge + AXI2AF_BRIDGE_104);
		writel(readl(bridge + AXI2AF_BRIDGE_108) & ~BIT(0), bridge + AXI2AF_BRIDGE_108);
	}
	if (s->disp_pwrgate) {
		writel(3, s->disp_pwrgate + DISP_PWRGATE_A);
		writel(0, s->disp_pwrgate + DISP_PWRGATE_B);
	}
}

/* dart-dcp's outstanding-transaction counters ([20:16]) at 0 before its domain goes. */
static int t6040_dcp_dart_idle(struct t6040_sleep *s)
{
	static const u16 regs[] = { 0x240, 0x244, 0x250, 0x254, 0x260, 0x270 };
	unsigned int i, us = 0;

	if (!s->dcp_dart)
		return 0;
	for (i = 0; i < ARRAY_SIZE(regs); i++)
		while (readl(s->dcp_dart + regs[i]) & GENMASK(20, 16)) {
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

	if (!s->soc_east)
		return -ENODEV;
	s->disp_down = true;
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
	if (FIELD_GET(PS_ACTUAL, t6040_psw_read(&cpu)))
		t6040_dcp_dart_idle(s);
	t6040_psw_poll_long(&cpu, PS_ACTUAL, 0, DISP_CPU_GATE_US);

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

	t6040_psw_write(w, (v & ~(PS_TARGET | PS_AUTO_ENABLE | PS_FLAGS)) | PS_TARGET);
	ret = t6040_psw_poll(w, PS_ACTUAL, PS_ACTUAL);
	t6040_psw_poll(w, PS_BUSY, 0);
	if (sys) {
		void __iomem *bridge = s->axi2af_bridge[AXI2AF_DISP_SYS];

		if (s->disp_pwrgate) {
			writel(0, s->disp_pwrgate + DISP_PWRGATE_A);
			writel(1, s->disp_pwrgate + DISP_PWRGATE_B);
		}
		if (bridge && !ret) {
			writel(readl(bridge + AXI2AF_BRIDGE_104) | BIT(0), bridge + AXI2AF_BRIDGE_104);
			writel(readl(bridge + AXI2AF_BRIDGE_108) | BIT(0), bridge + AXI2AF_BRIDGE_108);
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
	if (t6040_psw_on(s, &sys, true, true))
		return;
	if (t6040_psw_on(s, &fe, false, false))
		return;
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
			pr_debug("t6040-sleep: wake: PMC display interfaces BWR %#x boost %#x, put back to %#x %#x\n",
				 bwr, boost, readl(s->pmc_bwr), readl(s->pmc_bwr + PMC_BOOST));
	}
}

/*
 * The display power words the ADT wants off before a sleep (apple,check-ps-b4-sleep), lowered
 * here: after the device suspend, interrupts off, one CPU up, so no driver, interrupt handler
 * or DART touches the pipes any more. The DCPs only stop their firmware: lowering the domains
 * from their suspend panicked on T6040. Each group from its last sub-block up to, and not
 * including, its pipe CPU, which the PMGR gates by itself: each word as the pmgr-pwrstate
 * driver powers a domain off (TARGET 0, the one-shot bits clear), polled to ACTUAL 0. The
 * internal display's FE and SYS domains follow. Returns the number of words left on.
 */
static unsigned int t6040_sleep_lower_display(struct t6040_sleep *s)
{
	unsigned int i, left = 0;

	for (i = 0; i < s->ngroups; i++) {
		const struct t6040_sleep_group *g = &s->groups[i];
		u32 off = g->last;

		for (;;) {
			bool cpu = off == g->first;
			u32 val, cur;

			/* An automatically gated CPU still powered: wait for the PMGR to gate it. */
			if (cpu && !regmap_read(g->regmap, off, &val) && (val & PS_AUTO_ENABLE) &&
			    FIELD_GET(PS_ACTUAL, val))
				t6040_regmap_poll_off(g->regmap, off, &cur);
			if (!cpu && !regmap_read(g->regmap, off, &val) &&
			    t6040_sleep_ps_on(val, cpu)) {
				val &= ~(PS_TARGET | PS_AUTO_ENABLE | PS_FLAGS | PS_DEV_DISABLE |
					 PS_BUSY | PS_RESET_REQ);
				regmap_write(g->regmap, off, val);
				if (t6040_regmap_poll_off(g->regmap, off, &cur)) {
					pr_warn("t6040-sleep: %s +%#x still on: %#x\n", g->name,
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

static int t6040_sleep_enter(suspend_state_t state)
{
	struct t6040_sleep *s = t6040_sleep;
	unsigned int dry = s->dry, left;
	int ret;

	if (dry)
		return t6040_sleep_dry_enter(s, dry);

	left = t6040_sleep_lower_display(s);
	if (left)
		pr_warn("t6040-sleep: %u display power word(s) did not go off\n", left);
	if (t6040_sleep_total_blockers(s) || !allow_entry || !apple_sleep_power_loss()) {
		unsigned int on = t6040_sleep_log_groups(s);

		pr_info("t6040-sleep: at the commit point: %u CPU(s) online, %u display group(s) on; refusing\n",
			num_online_cpus(), on);
		t6040_disp_back(s);
		return -EBUSY;
	}

	/* The refusals, the hold and the stops in the generic driver's order. */
	if (t6040_sleep_refuse(s)) {
		t6040_disp_back(s);
		return -EBUSY;
	}
	ret = spin_table_hold_parked();
	if (ret) {
		t6040_disp_back(s);
		return ret;
	}
	ret = apple_sleep_coprocs_check(s->dev);
	if (!ret)
		ret = apple_sleep_enclave_stop(s->dev);
	if (ret) {
		spin_table_unhold_parked();
		t6040_disp_back(s);
		return ret;
	}
	s->pcie_ans_stopped = false;
	s->ans_bridge_quiesced = false;
	apple_sleep_coprocs_stop(true, &s->pcie_ans_stopped);
	apple_sleep_coprocs_stop(false, &s->pcie_ans_stopped);
	t6040_wake_arm(s);

	s->cntkctl = read_sysreg(cntkctl_el1);
	pm_set_resume_via_firmware();
	ret = cpu_suspend(0, t6040_sleep_commit);
	if (!ret)
		t6040_sleep_return(s);
	if (s->ans_bridge_quiesced)
		pr_debug("t6040-sleep: ANS's fabric bridge was armed at the commit: quiesced\n");

	if (s->returned)
		t6040_wake_sync();
	t6040_wake_restore(s);
	if (s->returned) {
		t6040_wake_sync();
		t6040_sio_ps_restore(s);
	}
	t6040_disp_back(s);
	if (s->returned) {
		t6040_wake_sync();
		if (s->pcie_ans_on_ret)
			pr_warn("t6040-sleep: wake: the storage PCIe raise failed after step %u: %d\n",
				s->apcie_wake_step, s->pcie_ans_on_ret);
		if (s->gp_clk_timeouts)
			pr_warn("t6040-sleep: wake: %u GP PCIe clock or ready poll(s) timed out\n",
				s->gp_clk_timeouts);
		if (s->aop_ps_timeouts)
			pr_warn("t6040-sleep: wake: %u AOP island power word(s) did not come on\n",
				s->aop_ps_timeouts);
	}
	return ret;
}

static void t6040_sleep_end(void)
{
	struct t6040_sleep *s = t6040_sleep;
	unsigned int dry = s->dry;

	if (!t6040_sleep_mem_begun)
		return;
	t6040_sleep_mem_begun = false;
	t6040_sleep_clear_wd2(s);
	if (dry)
		pr_info("t6040-sleep: dry run %u over, WD2 off\n", dry);
	t6040_sleep_set_dry(s, 0);
}

static const struct platform_suspend_ops t6040_sleep_ops = {
	.valid = t6040_sleep_valid,
	.begin = t6040_sleep_begin,
	.prepare = t6040_sleep_prepare,
	.enter = t6040_sleep_enter,
	.end = t6040_sleep_end,
};

static int t6040_sleep_parse_groups(struct t6040_sleep *sleep)
{
	struct device_node *np = sleep->dev->of_node;
	const char *prop = "apple,check-ps-b4-sleep";
	int n, i;

	/* Each group: the PMGR syscon, the CPU's offset and the last sub-block's. */
	n = of_property_count_u32_elems(np, prop);
	if (n <= 0 || n % 3)
		return dev_err_probe(sleep->dev, -EINVAL, "no valid %s\n", prop);
	n /= 3;
	if (of_property_count_strings(np, "apple,check-ps-b4-sleep-names") != n)
		return dev_err_probe(sleep->dev, -EINVAL, "one name per %s group\n", prop);

	sleep->groups = devm_kcalloc(sleep->dev, n, sizeof(*sleep->groups), GFP_KERNEL);
	if (!sleep->groups)
		return -ENOMEM;

	for (i = 0; i < n; i++) {
		struct t6040_sleep_group *g = &sleep->groups[i];
		struct of_phandle_args args;
		int ret;

		ret = of_parse_phandle_with_fixed_args(np, prop, 2, i, &args);
		if (ret)
			return dev_err_probe(sleep->dev, ret, "%s[%d]\n", prop, i);
		g->regmap = syscon_node_to_regmap(args.np);
		of_node_put(args.np);
		if (IS_ERR(g->regmap))
			return dev_err_probe(sleep->dev, PTR_ERR(g->regmap), "%s[%d] regmap\n",
					     prop, i);
		g->first = args.args[0];
		g->last = args.args[1];
		if (g->first > g->last || (g->last - g->first) % PS_STRIDE ||
		    (g->last - g->first) / PS_STRIDE >= PS_MAX_PER_GROUP)
			return dev_err_probe(sleep->dev, -EINVAL, "%s[%d]: bad range %#x..%#x\n",
					     prop, i, g->first, g->last);
		of_property_read_string_index(np, "apple,check-ps-b4-sleep-names", i, &g->name);
	}
	sleep->ngroups = n;

	return 0;
}

/*
 * A watched device's own power word: its first power domain, a PMGR power-state
 * node whose reg is the offset in its parent's syscon. Optional.
 */
static void t6040_sleep_watch_init(struct t6040_sleep *s, unsigned int i,
				   const char *compatible, const char *name)
{
	struct t6040_sleep_watch *w = &s->watch[i];
	struct device_node *np, *pd, *pmgr;
	struct regmap *regmap;

	w->compatible = compatible;
	w->name = name;
	np = of_find_compatible_node(NULL, NULL, compatible);
	if (!np)
		return;
	pd = of_parse_phandle(np, "power-domains", 0);
	of_node_put(np);
	if (!pd)
		return;
	pmgr = of_get_parent(pd);
	if (pmgr && !of_property_read_u32(pd, "reg", &w->ps_offset)) {
		regmap = syscon_node_to_regmap(pmgr);
		if (!IS_ERR(regmap)) {
			w->ps_regmap = regmap;
			w->has_ps = true;
		}
	}
	of_node_put(pmgr);
	of_node_put(pd);
	if (!w->has_ps)
		dev_warn(s->dev, "%s: no power word to watch\n", name);
}

/* A named register block, shared with the owning driver: mapped, not claimed. Optional. */
static void __iomem *t6040_sleep_map(struct device *dev, const char *name)
{
	struct device_node *np = dev->of_node;
	struct resource res;
	int index;

	index = of_property_match_string(np, "reg-names", name);
	if (index < 0 || of_address_to_resource(np, index, &res))
		return NULL;

	return t6040_ioremap_np(dev, res.start, resource_size(&res));
}

/*
 * ANS's storage-apcie windows + PLL/lane tunables, from the apple,apcie node
 * the boot object publishes from the ADT. Our static tree has none
 * of these, so until aura fills the node @valid stays false and a real sleep
 * with ANS stopped refuses (t6040_sleep_refuse). The reg-names and tunable
 * property names match the T8140's apple,apcie shape exactly, so the same
 * loader fills both SoCs. The GP PCIe host (pcie@1cb0000000, Wi-Fi + SD) is a
 * different node and is left to cold-recover.
 */
static void t6040_sleep_map_apcie(struct t6040_sleep *s)
{
	struct t6040_apcie_st *a = &s->apcie_st;
	struct device_node *np = of_parse_phandle(s->dev->of_node, "apple,apcie", 0);

	if (!np)
		return;
	a->axi = apple_sleep_apcie_block(s->dev, np, "axi2af", "apple,tunable-axi2af",
					 &a->axi_tunable);
	a->cio3pll = apple_sleep_apcie_block(s->dev, np, "cio3pll", "apple,tunable-cio3pll",
					     &a->cio3pll_tunable);
	a->clkgen = apple_sleep_apcie_block(s->dev, np, "clkgen", "apple,tunable-clkgen",
					    &a->clkgen_tunable);
	a->phy = apple_sleep_apcie_block(s->dev, np, "phy-block", "apple,tunable-phy",
					 &a->phy_tunable);
	a->rc = apple_sleep_apcie_block(s->dev, np, "rc", NULL, NULL);
	a->phy_ip = apple_sleep_apcie_block(s->dev, np, "phy-ip", "apple,tunable-phy-ip-pll",
					    &a->phy_ip_pll);
	a->phy_ip_lanes = apple_sleep_apcie_tunable(s->dev, np, "phy-ip",
						    "apple,tunable-phy-ip-auspma");
	of_node_put(np);

	a->valid = a->axi && a->cio3pll && a->clkgen && a->phy && a->rc && a->phy_ip &&
		   a->phy_ip_lanes;
	if (!a->valid)
		dev_info(s->dev,
			 "no storage-apcie windows/tunables (apple,apcie): a real sleep with ANS stopped refuses\n");
}

/* Maps the mailbox at @pa if it is aura's, or returns NULL. */
static struct aura_s2r_mailbox *t6040_sleep_mailbox_at(struct t6040_sleep *s, u64 pa)
{
	struct aura_s2r_mailbox *mbox;

	mbox = devm_memremap(s->dev, pa, sizeof(*mbox), MEMREMAP_WB);
	if (IS_ERR(mbox))
		return NULL;
	if (mbox->magic != AURA_S2R_MAGIC) {
		dev_warn(s->dev, "s2r-mailbox magic %#llx is not AURAS2R1\n", mbox->magic);
		devm_memunmap(s->dev, mbox);
		return NULL;
	}
	return mbox;
}

/* The boot object's mailbox, if it published one. Optional (a blocker if absent). */
static void t6040_sleep_map_mailbox(struct t6040_sleep *s)
{
	struct device_node *chosen = of_find_node_by_path("/chosen");
	u64 pa;
	int ret;

	if (!chosen)
		return;
	ret = of_property_read_u64(chosen, "aurora,s2r-mailbox", &pa);
	of_node_put(chosen);
	if (ret)
		return;

	s->mbox = t6040_sleep_mailbox_at(s, pa);
	if (s->mbox)
		s->mbox_pa = pa;
}

#ifdef CONFIG_HIBERNATION
/*
 * Restored from a hibernation image, this kernel runs under the aura of the boot that resumed it.
 * That aura moved with the boot object, and so did its mailbox: the one mapped at probe was the
 * hibernating boot's, and the boot core would come back from a sleep through this boot's. Take this
 * boot's address from the device tree aura keeps at its fixed slot. Without it, drop the mailbox, so
 * that a suspend refuses for it instead of arming memory that is no longer aura's.
 */
static int t6040_sleep_restore(struct device *dev)
{
	struct t6040_sleep *s = dev_get_drvdata(dev);
	struct aura_s2r_mailbox *mbox = NULL;
	u64 pa = 0;
	int ret;

	ret = aura_hibernate_boot_chosen_u64("aurora,s2r-mailbox", &pa);
	if (!ret && s->mbox && pa == s->mbox_pa)
		return 0;
	if (!ret)
		mbox = t6040_sleep_mailbox_at(s, pa);
	if (s->mbox)
		devm_memunmap(dev, s->mbox);
	if (mbox)
		dev_info(dev, "restored: s2r-mailbox %#llx, was %#llx on the boot that hibernated\n",
			 pa, s->mbox_pa);
	else
		dev_warn(dev, "restored: this boot's s2r-mailbox is unknown (%d, %#llx): dropped\n",
			 ret, pa);
	s->mbox = mbox;
	s->mbox_pa = mbox ? pa : 0;
	return 0;
}
#endif

/* The GP PCIe host's port and PHY windows and its ports' PERST# pins, for the commit. */
static void t6040_sleep_map_gp(struct t6040_sleep *s)
{
	static const char *const ports[] = { "port0", "port1" };
	static const char *const phys[] = { "phy0", "phy1" };
	struct device_node *host, *child;
	struct device_node *pins = NULL;
	struct resource res;
	int p, idx;

	s->gp_perst[0] = s->gp_perst[1] = -1;
	host = of_find_compatible_node(NULL, NULL, "apple,t6041-pcie");
	if (!host)
		return;
	if (!of_device_is_available(host))
		goto out;
	for (p = 0; p < 2; p++) {
		idx = of_property_match_string(host, "reg-names", ports[p]);
		if (idx >= 0 && !of_address_to_resource(host, idx, &res))
			s->gp_port[p] = t6040_ioremap_np(s->dev, res.start, resource_size(&res));
		idx = of_property_match_string(host, "reg-names", phys[p]);
		if (idx >= 0 && !of_address_to_resource(host, idx, &res))
			s->gp_phy[p] = t6040_ioremap_np(s->dev, res.start, resource_size(&res));
	}
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

static int t6040_sleep_probe(struct platform_device *pdev)
{
	struct t6040_sleep *sleep;
	struct dentry *dir;
	int ret, i;

	/* This sleep was tested with neither the lid's state at the wake nor USB wake ports. */
	apple_sleep_set_part_plain_wake();

	sleep = devm_kzalloc(&pdev->dev, sizeof(*sleep), GFP_KERNEL);
	if (!sleep)
		return -ENOMEM;
	sleep->dev = &pdev->dev;

	ret = t6040_sleep_parse_groups(sleep);
	if (ret)
		return ret;

	/*
	 * Optional: the commit's and the wake's hardware (all NULL on absence;
	 * every write is guarded and the gate refuses if a required one is
	 * missing). The axi2af fabric bridges are fixed PAs, not DT.
	 */
	BUILD_BUG_ON(ARRAY_SIZE(t6040_axi2af) != T6040_AXI2AF_DOMAINS);
	BUILD_BUG_ON(ARRAY_SIZE(t6040_tvm) != ARRAY_SIZE(((struct t6040_sleep *)0)->tvm_ctl));
	BUILD_BUG_ON(ARRAY_SIZE(t6040_acc_apsc_pa) != ARRAY_SIZE(((struct t6040_sleep *)0)->acc_apsc));
	sleep->cpu_start = t6040_sleep_map(&pdev->dev, "cpu-start");
	sleep->wdt = t6040_sleep_map(&pdev->dev, "wdt");
	{
		struct resource wres;
		int widx = of_property_match_string(pdev->dev.of_node, "reg-names", "wdt");

		sleep->wdt_stub_ok = widx >= 0 &&
			!of_address_to_resource(pdev->dev.of_node, widx, &wres) &&
			wres.start == T6040_WDT_PA;
		if (!sleep->wdt_stub_ok)
			dev_warn(&pdev->dev,
				 "the wdt block is not the AON watchdog: no WD2 restart at the wake\n");
	}
	sleep->cluster_ctl = t6040_sleep_map(&pdev->dev, "cluster-ctl");
	sleep->pwrgate = t6040_sleep_map(&pdev->dev, "soc-pwrgate");
	sleep->clkgen = t6040_sleep_map(&pdev->dev, "clkgen");
	sleep->spmi0 = t6040_sleep_map(&pdev->dev, "spmi0");
	sleep->spmi_a0 = t6040_sleep_map(&pdev->dev, "spmi-a0");
	sleep->mastermini = t6040_sleep_map(&pdev->dev, "mastermini");
	sleep->aop_ps = t6040_ioremap_np(&pdev->dev, T6040_AOP_PS_PA, 0x160);
	sleep->pmgr_soc = t6040_ioremap_np(&pdev->dev, 0x502800000ULL, 0x9000);
	for (i = 0; i < 16; i++)
		sleep->dcs[i] = t6040_ioremap_np(&pdev->dev, 0x220200000ULL +
						 (i / 4) * 0x2000000ULL + (i % 4) * 0x200000ULL,
						 0x2000);
	for (i = 0; i < 4; i++)
		sleep->atc_cfg[i] = t6040_ioremap_np(&pdev->dev,
						     0x383200000ULL + i * 0x8000000ULL, 0x100);
	sleep->pmgr_east = t6040_ioremap_np(&pdev->dev, 0x502280000ULL, 0xd000);
	sleep->soc_east = t6040_sleep_map(&pdev->dev, "soc-east");
	sleep->soc_ps = t6040_sleep_map(&pdev->dev, "soc-ps");
	sleep->aop_global = t6040_sleep_map(&pdev->dev, "aop-global");
	for (i = 0; i < T6040_AXI2AF_DOMAINS; i++)
		sleep->axi2af_bridge[i] = t6040_ioremap_np(&pdev->dev,
						       t6040_axi2af[i].bridge_pa, 0x200);
	t6040_sleep_map_apcie(sleep);
	t6040_sleep_map_mailbox(sleep);
	t6040_sleep_watch_init(sleep, T6040_WATCH_SEP, "apple,sep", "sep");
	t6040_sleep_watch_init(sleep, T6040_WATCH_ANS, "apple,t6041-nvme-ans2", "ans");
	/* Fixed PAs: dart-dcp's counters and the internal display's retention pair. */
	sleep->dcp_dart = t6040_ioremap_np(&pdev->dev, T6040_DCP_DART_PA, 0x1000);
	sleep->disp_pwrgate = t6040_ioremap_np(&pdev->dev, T6040_DISP_PWRGATE_PA, 0x100);
	sleep->pmc_dcp = t6040_ioremap_np(&pdev->dev, T6040_PMC_DCP_PA, 12);
	sleep->pmc_bwr = t6040_ioremap_np(&pdev->dev, T6040_PMC_BWR_PA, 0x10);
	sleep->ptd = t6040_ioremap_np(&pdev->dev, T6040_PTD_PA, 0x11000);
	/* The DART clock-protection (GAPF) block, read-only here. */
	sleep->gapf = t6040_ioremap_np(&pdev->dev, 0x500578000ULL, 0x1000);
	sleep->ka_soc = t6040_ioremap_np(&pdev->dev, T6040_KA_SOC_PA, KA_SIZE);
	sleep->ka_east = t6040_ioremap_np(&pdev->dev, T6040_KA_EAST_PA, KA_SIZE);
	sleep->smc_wdt_deadline = t6040_ioremap_np(&pdev->dev, T6040_SMC_WDT_DEADLINE_PA, 4);
	sleep->agx_gate = t6040_ioremap_np(&pdev->dev, T6040_AGX_GATE_PA, 4);
	sleep->s2r_lo_count = t6040_ioremap_np(&pdev->dev, T6040_S2R_LO_COUNT_PA, 4);
	for (i = 0; i < ARRAY_SIZE(t6040_acc_apsc_pa); i++)
		sleep->acc_apsc[i] = t6040_ioremap_np(&pdev->dev, t6040_acc_apsc_pa[i], 8);
	for (i = 0; i < ARRAY_SIZE(t6040_tvm); i++) {
		sleep->tvm_ctl[i] = t6040_ioremap_np(&pdev->dev, t6040_tvm[i].ctl, 4);
		sleep->tvm_status[i] = t6040_ioremap_np(&pdev->dev, t6040_tvm[i].status, 4);
	}
	sleep->afr_ps = t6040_ioremap_np(&pdev->dev, T6040_AFR_PS_PA, 4);
	t6040_sleep_map_gp(sleep);
	t6040_ed_map(&pdev->dev);

	platform_set_drvdata(pdev, sleep);
	t6040_sleep = sleep;

	dir = debugfs_create_dir("t6040-sleep", NULL);
	debugfs_create_file("preflight", 0400, dir, sleep, &t6040_sleep_preflight_fops);

	/*
	 * The CPU power ops act only when a commit stops the parked secondaries.
	 * A real entry needs allow_entry; a dry run registers them at its .begin,
	 * for the hold. Until then kexec keeps refusing parking CPUs as before.
	 * Parking itself needs no power ops.
	 */
	if (allow_entry)
		t6040_sleep_power_ops(sleep);

	suspend_set_ops(&t6040_sleep_ops);
	dev_info(&pdev->dev,
		 "refusal-first: %u display groups, cpu-start %s, wdt %s, commit/wake blocks %s, storage-apcie %s, s2r-mailbox %s, parking ops %s, dry run %u\n",
		 sleep->ngroups, sleep->cpu_start ? "mapped" : "absent",
		 sleep->wdt ? "mapped" : "absent",
		 (sleep->cluster_ctl && sleep->pwrgate && sleep->mastermini && sleep->spmi0 &&
		  sleep->soc_east && sleep->aop_global) ? "mapped" : "incomplete",
		 sleep->apcie_st.valid ? "mapped" : "absent",
		 sleep->mbox ? "present" : "absent",
		 sleep->power_ops ? "registered" : "not registered", READ_ONCE(dry_run));
	t6040_sleep_log_s2r_state(sleep, "probe");

	return 0;
}

const struct apple_sleep_part t6040_sleep_part = {
	.probe = t6040_sleep_probe,
#ifdef CONFIG_HIBERNATION
	.restore = t6040_sleep_restore,
#endif
	/* No enclave_admit: every enclave is taken, in a dry run as in a real sleep. */
	.registry = {
		.commit_stops = t6040_sleep_reg_commit_stops,
	},
};
