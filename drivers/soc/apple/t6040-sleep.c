// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * Apple T6040 (M4 Pro) system sleep: refusal-first controller.
 *
 * This is the T6040 port of aurora-silicon/linux #1208 (T8140 deep sleep at
 * EL2). It does not take the SoC down. #1208's T8140 driver follows the "last
 * 14 ms" power-down micro-sequence measured on a J700; that
 * measurement has not been made for the T6040, so the exact commit sequence (DCS
 * ladder, MCC lanes, PMC voters, CLLT, DVFM, the the platform on/off power-word lists,
 * the wake order) is UNKNOWN here.
 *
 * What it does:
 *
 *  - It keeps the debugfs preflight of the display power states the platform checks
 *    before it sleeps (device-check-ps-b4-sleep).
 *  - It keeps #1208's coprocessor and enclave registry (sleep.h) for the T6040:
 *    a driver that can only stop its coprocessor once the sleep is certain
 *    (ANS, the PMP, the SEP's sleep request) registers here, readies it in its
 *    own suspend and leaves the last stop to the commit, after every refusal.
 *    A refused sleep then finds it running.
 *  - Its suspend_ops .begin computes the blocker list. By default a
 *    `echo mem > /sys/power/state` prints every blocker and returns an error
 *    before any device suspends.
 *  - Dry runs (t6040_sleep.dry_run, also writable at runtime):
 *      1: the ordinary device suspend and CPU parking, the firmware never told.
 *      2: pm_suspend_via_firmware() is set, so every driver's firmware-driven
 *         sleep step that can be taken back runs (apple_sleep_power_loss() is
 *         false, so what only a power cycle restores is left out), the CPUs
 *         park and answer the commit's hold, the commit point's checks run,
 *         and the sleep is refused there and resumed warm.
 *      3: 2, plus the last steps that can still be taken back (the SMC's sleep
 *         commit, each coprocessor's last check, such as ANS's check of the
 *         shutdown notification it sent at its suspend).
 *    The AON watchdog's WD2 is armed from .begin to .end of a dry run 2 or 3
 *    (t6040_sleep.wd2_secs, 0 to leave it off).
 *  - The real commit (t6040_sleep_commit) is compiled but unreachable: it runs
 *    only when no blocker remains and t6040_sleep.allow_entry=1, and the
 *    missing-data blockers never clear in this kernel.
 *
 * Why this file and not a generalisation of t8140-sleep.c: the port keeps
 * #1208's T8140 behaviour byte-identical, and the per-SoC match data should be
 * built once the T6040 commit and wake have been measured. Until then the
 * registry lives here as well, with the same contract, and a kernel builds one
 * of the two controllers.
 */

#include <linux/bitfield.h>
#include <linux/bits.h>
#include <linux/cleanup.h>
#include <linux/cpu.h>
#include <linux/debugfs.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/irqchip/irq-apple-aic.h>
#include <linux/mfd/syscon.h>
#include <linux/minmax.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_platform.h>
#include <linux/mfd/macsmc.h>
#include <linux/panic_notifier.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/smp.h>
#include <linux/soc/apple/aura-hibernate.h>
#include <linux/soc/apple/sleep.h>
#include <linux/soc/apple/tunable.h>
#include <linux/suspend.h>
#include <linux/syscore_ops.h>

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

#define PS_TARGET		GENMASK(3, 0)
#define PS_ACTUAL		GENMASK(7, 4)
#define PS_FLAGS		GENMASK(9, 8)
#define PS_AUTO_ENABLE		BIT(28)
#define PS_OFF_TIMEOUT_US	5000
#define PS_STRIDE		8
#define PS_MAX_PER_GROUP	16

/*
 * ANS's storage PCIe block: the TWO ST lanes (apcie_st0/apcie_st1) and their
 * shared PHY (T8140 has a single lane). Taken down at the commit
 * and raised on wake. The power words sit in SOC-East (t604x-pmgr.dtsi
 * offsets, same bank as ps_ans); the control windows (axi2af/cio3pll/clkgen/
 * phy-block/phy-ip/rc) and the PLL/lane tunables are NOT in this SoC's static
 * description and are published by the boot object (aura) under the apple,apcie
 * node, matching aurora #1208. The block-internal offsets below are T8140's:
 * the same Apple PCIe IP, expected identical.
 */
#define PS_ANS			0x370	/* SOC-East: ps_ans */
#define PS_APCIE_ST0		0x108	/* SOC-East: ps_apcie_st0 */
#define PS_APCIE_ST1		0x110	/* SOC-East: ps_apcie_st1 */
#define PS_APCIE_SYS_ST0	0x380	/* SOC-East: ps_apcie_sys_st0 */
#define PS_APCIE_SYS_ST1	0x388	/* SOC-East: ps_apcie_sys_st1 */
#define PS_APCIE_PHY_SW		0x398	/* SOC-East: ps_apcie_phy_sw (the shared PHY) */
#define PS_BUSY			BIT(11)	/* polled clear by the A2D engine after each request */

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
#define APCIE_RC_ZERO		0x04
#define APCIE_RC_READY_START	0x50
#define APCIE_RC_READY_CFG	0x54
#define APCIE_RC_READY_STAT	0x58
#define APCIE_POLL_US		50000

/*
 * CPU start block (misc-cores-offset 0x88000, the offset aura uses for
 * T6031). cpu-start = pmgr reg0 (0x502800000) +
 * 0x88000 = 0x502888000. Register semantics are the same as T8140's.
 */
#define CPU_START_STOP		0x0
#define CPU_START_SYS		0x4
#define CPU_START_CLUSTER(n)	(0x8 + (n) * 4)

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
 *    BIT0 := 0, status polled until BIT1 clears;
 *  - the PMC performance state quiesce: the AP voter agent (3) drops its
 *    DCS floor vote, PMC_DVFM_IF + 3 * 0x4000 [3:0] := 0, pending +4 BIT3 polled
 *    (the SOC domain has no AP voter map entry here: no write).
 * The platform polls the APSC and TVM words without a timeout; here every poll is bounded
 * and a timeout sets a bit of the commit's 0x5d mark.
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
#define T6040_PMC_AP_FLOOR_PA	0x50480c000ULL

/*
 * As the platform does (measured on the J616s where marked), a display pipe's PMC votes go
 * before any of its power words: boost 0x5048ac028 BIT(p), DVMR 0x5048a4000 BIT(p) with
 * the ack polled at +0x20 BIT0 and 0x5048a9c14 := popcount(DVMR) (measured), BWR 0x5048ac020
 * BIT(p ? p + 2 : 0); the cluster rail 0x5048a9c18 BIT2 (DISP_INT) or BIT3 (DISP_EXT, last
 * pipe only) with the ack at 0x5048a9c1c (measured). p: 0 internal, 1 DCPEXT0, 2 DCPEXT1.
 */
#define T6040_PMC_DVMR_PA	0x5048a4000ULL
#define PMC_DVMR_ACK		0x20
#define T6040_PMC_BWR_PA	0x5048ac020ULL
#define PMC_BOOST		0x8
#define PMC_DCP_COUNT		0x0	/* 0x5048a9c14 */
#define PMC_RAIL		0x4	/* 0x5048a9c18 */
#define PMC_RAIL_ACK		0x8	/* 0x5048a9c1c */
#define PMC_RAIL_INT		BIT(2)
#define PMC_RAIL_EXT		BIT(3)
#define PMC_TIMEOUT_US		8000

/*
 * The DCPEXT power words in SOC (soc_ps), CPU then FE then SYS, the values
 * (measured: CPU 0x1f0003ff -> 0x0f0000ff -> 0x0f0003f0, FE 0x3ff -> 0x3f0, SYS -> 0x3f0); the
 * SYS word after its axi2af bridge bits and its pwrgate pair (A := 3, B := 0) in the
 * SOC pwrgate block's second page.
 */
#define T6040_DISPEXT_PWRGATE_PA	0x50282d000ULL

/*
 * The display coprocessors' ASC CPU_CONTROL (+0x44): DCP, DCPEXT0, DCPEXT1. Upstream
 * T8140 (#1208, working S2R) writes it 0 twice right before the display CPU word goes
 * down; RUN and bit5 alone, cleared at the device suspend, leave the word refusing writes.
 */
static const u64 t6040_dcp_asc_ctl_pa[3] = { 0x402e00044ULL, 0x3a2e00044ULL, 0x602e00044ULL };

/*
 * The PMP's PTD (pmp-report 0x5003c0000, T6041 layout as drivers/pmdomain/apple/
 * pmp-report.c): SOC-DEV-PS-REQ read +0x1e00, written +0x10f00, SOC-DEV-PS-ACK +0x1e80,
 * 64-bit. DCPEXT0/1 are requests 17/18 (DT report@11/@12, withdrawn by that driver at
 * suspend_noirq); the internal display is 16, assumed from the pattern. The platform has
 * the PMP release a display's request before its power words change.
 */
#define T6040_PTD_PA		0x5003c0000ULL
#define PTD_REQ_READ		0x1e00
#define PTD_REQ_WRITE		0x10f00
#define PTD_ACK			0x1e80
#define PTD_DISP_INT		16
static const struct {
	u32 sys, fe, cpu, pg_a, pg_b;
	int axi2af;
} t6040_dispext[2] = {
	{ 0x340, 0x348, 0x350, 0x234, 0x258, 5 },
	{ 0x490, 0x498, 0x4a0, 0x6b4, 0x6d8, 6 },
};
#define PMC_FLOOR_DCS		GENMASK(3, 0)
#define PMC_PENDING		0x4
#define PMC_PENDING_AP		BIT(3)
#define QUIESCE_TIMEOUT_US	2000

/* The 0x5d mark's timeout bits. */
#define QTO_APSC(c)		BIT(c)
#define QTO_TVM(i)		BIT(3 + (i))
#define QTO_PMC			BIT(11)
#define QTO_CLUSTER(i)		BIT(12 + (i) - CLUSTER_CTL_FIRST_WAIT)
#define QTO_EAST_CLK(i)		BIT(14 + (i))
#define QTO_MUX(i)		BIT(16 + (i))
#define AGX_GATE_RESIDENT	GENMASK(19, 18)
#define T6040_AFR_PS_PA		0x288e80100ULL
#define AFR_LEVEL		GENMASK(19, 16)
#define AGX_TIMEOUT_US		10000

/*
 * The PMU over SPMI (spmi0 = 0x509014000, spmi-a0 = 0x509188000). Same shape as
 * #1208's t8140_pmu_write: the reply vector is masked, the command and its data
 * are written to +0x4, the reply polled and acknowledged. Measured on the J616s
 *: the boot stage is set to 0x50 by an extended write whose command
 * word is (0xf801 << 16) | 0x8e30 = 0xf8018e30, and NOTIFY_ENABLE is 0x2000001
 * on both controllers' +0x40.
 */
#define SPMI_STATUS		0x00
#define SPMI_STATUS_RX_EMPTY	BIT(24)
#define SPMI_CMD		0x04
#define SPMI_REPLY		0x08
#define SPMI_NOTIFY_ENABLE	0x40
#define SPMI_NOTIFY_ACK		0x80
#define SPMI_NOTIFY_REPLY	BIT(0)
#define SPMI_CMD_PMU(reg, op)	(((u32)(reg) << 16) | BIT(15) | (0xe << 8) | (op))
#define SPMI_OP_WRITE4		0x33
#define SPMI_OP_READ4		0x3b
#define PMU_RTC_COMPARE		0x2008
#define SPMI_CMD_WRITE		(BIT(15) | (0xe << 8) | 0x30)	/* low 16 bits 0x8e30 */
#define SPMI_NOTIFY_ENABLE_SLEEP	0x2000001
#define PMU_BOOT_STAGE		0xf801		/* T6040 slave/reg; cmd 0xf8018e30 */
#define PMU_BOOT_STAGE_SLEEP	0x50
/* The post-timer PMU "go hib (0x7)" replay framing. */
#define SPMI_GO_HIB_NOTIFY	0x2000000

/*
 * MasterMini (NUB_MPMGR_AON, RegMap 4 = pmgr reg[0x37] = 0x508900000).
 * The S2R timer at +0x4000 (physical 0x508904000) is the commit: writing 1 arms
 * the SoC sleep engine. Measured on the J616s: the platform makes exactly this write.
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

/* The internal display, as the platform drives the PMGR and the DART. */
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
 * The AON watchdog's WD2, the sleep safety net. ADT wdt reg[0] = 0x50836c000.
 * Offsets and the 24 MHz tick follow the platform (its AON watchdog for a reconfig)
 * and the ADT (simple-reconfig-wdog-icc-time = 5). Linux's own
 * watchdog driver runs WD1 (+0x10..+0x1c), not this one.
 */
#define WDT_WD0_COUNT		0x00
#define WDT_WD0_BITE		0x04
#define WDT_WD0_CTL		0x0c
#define WDT_WD1_COUNT		0x10
#define WDT_WD1_BITE		0x14
#define WDT_WD1_CTL		0x1c
#define WDT_CTL_RESET_EN	BIT(2)
#define WDT_WD2_COUNT		0x20
#define WDT_WD2_ALARM		0x28
#define WDT_WD2_CTL		0x2c
#define WDT_TICKS_PER_S		24000000
/* At the commit, as the reconfig. */
#define WDT_SLEEP_SECS		5
/* From .begin to .end: the device suspend and resume, as #1208 does. */
#define WDT_SUSPEND_SECS	120
/* ALARM is 32 bits at 24 MHz. */
#define WDT_MAX_SECS		178U

/* ASC CPU control, the same in every Apple coprocessor wrapper. */
#define ASC_CPU_CONTROL		0x44
#define ASC_CPU_CONTROL_RUN	BIT(4)
#define ASC_CPU_STATUS		0x48

/*
 * The deep-sleep IMPDEF sysreg. The platform on T6040 prepares a core for
 * sleep with the same register and bits as
 * T8140. Written by every core right before its last WFI; only a reset clears
 * it.
 */
#define SYS_APL_DEEP_SLEEP	sys_reg(3, 5, 15, 6, 2)
#define DEEP_SLEEP_ENABLE	(BIT_ULL(0) | BIT_ULL(63))
#define SYS_APL_IPI_SR_EL1	sys_reg(3, 5, 15, 1, 1)
#define SYS_APL_SIQ_CFG_EL1	sys_reg(3, 4, 15, 10, 4)
#define IPI_SR_PENDING		BIT(0)

/* Must match aura's struct aura_s2r_mailbox (aura #117). */
#define T6040_S2R_MAGIC		0x3152325341525541ULL	/* "AURAS2R1" */

struct t6040_s2r_mailbox {
	u64 magic;
	u64 boot_mpidr;
	u64 resume_pa;
	u64 resumes;
};

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
	struct dentry *debugfs;
	void __iomem *cpu_start;
	void __iomem *wdt;
	/* The DT's wdt block is the one t6040_resume_stub hardcodes. */
	bool wdt_stub_ok;
	/* Bring-up marks: the PMU RTC comparator saved at .enter, put back (+1 s) before the commit. */
	u32 alarm_saved, alarm_hdr;
	bool alarm_ok;
	/* The commit's and the wake's SoC blocks (see the #defines). Optional. */
	void __iomem *cluster_ctl;
	void __iomem *pwrgate;
	u32 commit_on_mask;	/* axi2af domains still on at the commit (0: none) */
	void __iomem *dcp_dart;		/* dart-dcp, for its transaction idle wait */
	void __iomem *disp_pwrgate;	/* internal display retention window 0x5022a1000 */
	bool disp_down;			/* t6040_disp_off() lowered the internal display */
	void __iomem *pmc_dcp;		/* PMS pmc-dcp voter pair 0x5048a9c14/c18 */
	u32 pmc_dcp_saved[2];
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
	/* Bring-up A2D watch windows (read-only, MCPS/MPMGR A2D steps, iBoot rcfg seq 8/24). */
	void __iomem *a2d_mini, *a2d_mini4, *a2d_mm8, *a2d_pmc, *a2d_blk;
	void __iomem *a2d_mm12;	/* MasterMini 0x50890c000 (IB2 engine + handshake +0x14) */
	void __iomem *aop_req;	/* AOP island power requests 0x5108b8100 (bit 0 DRAM, 1 AP, 2 CPU) */
	void __iomem *mtp_ps;	/* MiniPMGR MTP island PS words 0x50830c000..0x060 (bring-up) */
	u32 mtp_left;		/* MTP PS words that did not reach ACTUAL 0 at this commit */
	u32 apcie_diag[16];	/* apcie_seq bit 2: 'APCS' + flags + PHY/PS words, PMU 0xa200 */
	void __iomem *atc_aon;	/* 0x508320c00: the 4 ATC port blocks of A2D seq 25 */
	void __iomem *dcs[16];	/* DCS(n,k): 0x220200000 + n * 0x2000000 + k * 0x200000 */
	void __iomem *amcc_sem[4];	/* 0x2201c0000 + n * 0x2000000 (+0x28 MCU handshake) */
	void __iomem *pg_main, *pg_east;	/* pwrgate 0x50282c000, SOC-East 0x5022a0000 */
	void __iomem *aop_asc;	/* AOP ASC 0x510600000 (CPU_CONTROL +0x44) */
	void __iomem *atc_cfg[4];	/* ATCPHY_CFG(n) 0x383200000 + n * 0x8000000 */
	void __iomem *mini_ps;	/* 0x508300000: MiniPMGR PS words */
	void __iomem *aop_ps;	/* MiniPMGR AOP + audio island PS words 0x508304000..0x158 */
	void __iomem *snap_soc, *snap_east;	/* PS snapshot pages 0x502800000 / 0x502280000 */
	void __iomem *pmc_probe_io[9];	/* bring-up PMC probe words */
	u32 aop_left;		/* AOP words left on << 8 | first index left on */
	void __iomem *cpm_cfg[3];	/* cluster + 0xe50000 (iBoot idle power-down, bring-up) */
	void __iomem *cpm_impl[3];	/* cluster + 0xe40000 (cpm-impl-reg) */
	void __iomem *cpu4_impl110;	/* boot CPU cpu-impl-reg + 0x110 (0x211050110) */
	u32 cpm_to;			/* clusters whose CPM state poll timed out at this commit */
	void __iomem *soc_east;
	void __iomem *soc_ps;
	void __iomem *aop_global;
	void __iomem *axi2af_bridge[T6040_AXI2AF_DOMAINS];
	struct t6040_s2r_mailbox *mbox;
	u64 mbox_pa;
	bool power_ops;
	bool wd2_armed;
	bool timer_written;	/* bring-up observer: the S2R timer is armed */
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
	void __iomem *ane_sys_ps;	/* pmgr +0x3c0 */
	void __iomem *ane_ps;		/* pmgr_ane: mpm, cpu, td, base */
	void __iomem *acc_apsc[3];
	void __iomem *tvm_ctl[8];
	void __iomem *tvm_status[8];
	void __iomem *pmc_ap_floor;
	void __iomem *pmc_dvmr;
	void __iomem *pmc_bwr;
	void __iomem *dispext_pwrgate;
	void __iomem *dcp_asc_ctl[3];
	void __iomem *ptd;
	void __iomem *gapf;
	u32 quiesce_to;			/* timeout bits of this commit's quiesce polls */
	void __iomem *afr_ps;
	u32 afr_saved;
	u32 cluster_saved[CLUSTER_CTL_NR];
	u32 cluster_after[CLUSTER_CTL_NR];
	u32 wake_mask_saved[AOP_WAKE_WORDS];
	bool wake_mask_saved_valid;
	u32 wake_status[AOP_WAKE_WORDS];
	bool returned;
	/* ANS's storage ST lanes: windows + whether a commit stopped ANS. */
	struct t6040_apcie_st apcie_st;
	bool pcie_ans_stopped;
	int ans_off_ret;
	int pcie_ans_on_ret;
	u32 apcie_wake_step;
	u32 gp_phy_saved[2], gp_appclk_saved[2], gp_perst_saved[2];
	bool gp_saved[2];
	u32 gp_port_res;
	u32 aop_ps_saved[44];
	bool aop_ps_saved_valid;
	struct t6040_sleep_watch watch[T6040_WATCH_COUNT];
};

/*
 * 0: refuse at .begin. 1: device suspend and parking, the firmware never told.
 * 2: the firmware-driven device suspend short of what only a power cycle
 * restores, parking and the commit's hold, refused at the commit point. 3: 2 and
 * the last steps that can still be taken back. Read at each suspend's .begin.
 */
static unsigned int dry_run;
module_param(dry_run, uint, 0644);
MODULE_PARM_DESC(dry_run,
		 "Dry run level: 1 parking only, 2 firmware-driven device sleep refused at the commit point, 3 also the last steps");

static bool allow_entry;
module_param(allow_entry, bool, 0444);
MODULE_PARM_DESC(allow_entry,
		 "Reach the real commit if no blocker remains (none clears with the current unknowns)");

/*
 * Explicit, deliberate override of the missing-data blockers (t6040_sleep_unknowns)
 * for the hardware S2R bring-up: the entry/wake/apcie/dcp code has run on the J616s
 * only with this override set, and a real commit can hang or lose devices on
 * resume (the AON WD2 is the net). Writable at runtime so it is armed only as the
 * last step after the system is quiesced; refuses still apply to the live blockers
 * (display/CPU/mailbox) and to allow_entry. Leave 0 for normal operation.
 */
static bool i_know_untested;
module_param(i_know_untested, bool, 0644);
MODULE_PARM_DESC(i_know_untested,
		 "DANGER: ignore the missing-data blockers for a first HW S2R bring-up (needs allow_entry too)");

/*
 * The AON WD2 net armed at the commit. The platform arms 5 s for its reconfig; a longer
 * net tells "the SoC never slept" (reset at the net) from "it slept and the wake
 * failed" (reset at the wake source) on a first bring-up. Never off: 1 to 178.
 */
static unsigned int sleep_wd_secs = 30;
module_param(sleep_wd_secs, uint, 0644);
MODULE_PARM_DESC(sleep_wd_secs,
		 "AON WD2 net armed at the S2R commit, in seconds (default 5, clamped to 1..178)");

static unsigned int wd2_secs = WDT_SUSPEND_SECS;
module_param(wd2_secs, uint, 0644);
MODULE_PARM_DESC(wd2_secs,
		 "Arm the AON WD2 for this many seconds from .begin to .end of a dry run 2 or 3 (0: off, at most 178)");

/*
 * After the S2R timer, the platform issues a PMU "go hib (0x7)" SPMI sequence.
 * Whether a Linux-at-EL2 driver must issue it, or the MasterMini timer alone
 * arms the engine and the PMU sees it over the hardware SPMI link, is unproven.
 * Off by default. The commit is unreachable regardless of this.
 */
static bool pmgr_quiesce = true;
module_param(pmgr_quiesce, bool, 0644);
MODULE_PARM_DESC(pmgr_quiesce,
		 "Run the steps of the generic PMGR quiesce in the commit: APSC off, PLL relock off, TVMs off, PMC DCS floor");

/*
 * On the J616s the first access to 0x50480c000 (the AP voter agent's floor) hung the
 * bus (mark 0x330, reset by the 30 s WD2). The address is unverified on
 * hardware; off until it is.
 */
/*
 * On the J616s, with the secondaries still spinning (stopped, coherent) until after
 * the timer, the complexes +0xc/+0x10 never dropped BIT4 and the engine stalled. On
 * The platform every other core is already off (self-disable + deep WFI) when the master
 * turns the complexes off. Release the stopped cores to their last WFI at the top
 * of the commit and wait for their core PS words to read off.
 */
static bool release_early = true;
module_param(release_early, bool, 0644);
MODULE_PARM_DESC(release_early,
		 "Release the stopped CPUs to their last WFI at the commit's start and wait until they are off");

static bool pmc_floor;
module_param(pmc_floor, bool, 0644);
MODULE_PARM_DESC(pmc_floor, "With pmgr_quiesce: the PMC AP DCS floor vote (its address hung the bus on the J616s)");

static bool tvm_gpu = true;
module_param(tvm_gpu, bool, 0644);
MODULE_PARM_DESC(tvm_gpu, "With pmgr_quiesce: also the GPU PMGR's afr/gfx TVMs (0x288e94000..)");

/*
 * cluster-ctl DISP_INT (+0xc) / DISP_EXT (+0x10): 0 leaves them as they are, 1 writes the
 * nibble 0 and waits for BIT4 (the platform), 2 writes without waiting. Their CPU words can't be
 * lowered from here, so the slots never finish under 1.
 */
static unsigned int disp_clusters = 1;
module_param(disp_clusters, uint, 0644);
MODULE_PARM_DESC(disp_clusters,
		 "DISP_INT/DISP_EXT cluster-ctl slots at the commit: 0 untouched, 1 off + wait, 2 off, no wait");

/*
 * At runtime the PTD request word reads 0x63c00: EACC0/PACC0/PACC1/GFX (10-13) and the
 * two DCPEXTs (17, 18, withdrawn by pmp-report at suspend_noirq). Bits set here are
 * withdrawn at the commit right before the S2R timer (no ack wait). Bring-up knob.
 */
static unsigned long ptd_withdraw;
module_param(ptd_withdraw, ulong, 0644);
MODULE_PARM_DESC(ptd_withdraw, "PTD SOC-DEV-PS-REQ bits to withdraw right before the S2R timer");

static bool post_timer_mask;
module_param(post_timer_mask, bool, 0644);
MODULE_PARM_DESC(post_timer_mask, "Bring-up: the post-timer counter probe's mark carries which cores and ANE words are on");

static bool post_timer_ane;
module_param(post_timer_ane, bool, 0644);
MODULE_PARM_DESC(post_timer_ane, "Bring-up: the post-timer counter probe's mark carries the ANE power words instead");

static bool post_timer_counter_probe;
module_param(post_timer_counter_probe, bool, 0644);
MODULE_PARM_DESC(post_timer_counter_probe,
		 "Bring-up: after the S2R timer, test whether CNTPCT still moves (answer by WD2 timing + mark)");

static unsigned int post_timer_wd_ms;
module_param(post_timer_wd_ms, uint, 0644);
MODULE_PARM_DESC(post_timer_wd_ms,
		 "Bring-up: re-arm the AON WD2 to this many ms (max 178000) right after the S2R timer");

static unsigned int post_timer_probe_ms;
module_param(post_timer_probe_ms, uint, 0644);
MODULE_PARM_DESC(post_timer_probe_ms,
		 "Bring-up: after the S2R timer spin this long (max 4 s), then mark 0x50EEPPQQ = cluster-ctl E/P0/P1");

static unsigned int timer_delay_ms;
module_param(timer_delay_ms, uint, 0644);
MODULE_PARM_DESC(timer_delay_ms,
		 "Bring-up: hold this long (max 10 s) right before the S2R timer, mark 0x3e0 first");

static bool agx_gate = true;
module_param(agx_gate, bool, 0644);
MODULE_PARM_DESC(agx_gate,
		 "Lower the GPU's AFR power word in the commit, as the PMGR quiesce does behind its AGX residency gate");

static bool keep_alive_off = true;
module_param(keep_alive_off, bool, 0644);
MODULE_PARM_DESC(keep_alive_off,
		 "Clear the 64 PMGR keep-alive bits right before the S2R timer, as the PMGR quiesce does");

static bool replay_pmu_go_hib;
module_param(replay_pmu_go_hib, bool, 0644);
MODULE_PARM_DESC(replay_pmu_go_hib,
		 "Replay the PMU \"go hib\" SPMI pairs after the S2R timer (necessity unconfirmed)");

static struct t6040_sleep *t6040_sleep;

static void t6040_iounmap(void *p)
{
	iounmap((void __iomem __force *)p);
}

/*
 * Every window here non-posted (nGnRnE), as the platform and aura map the PMGR, PMC and
 * coprocessor blocks. No region request: these overlap other drivers.
 */
static void __iomem *t6040_ioremap_np(struct device *dev, phys_addr_t pa, size_t size)
{
	void __iomem *p = ioremap_np(pa, size);

	if (!p || devm_add_action_or_reset(dev, t6040_iounmap, (void __force *)p))
		return NULL;
	return p;
}

/* The dry run level of the suspend in progress, from .begin to .end; 0 otherwise. */
static unsigned int t6040_sleep_dry;

bool apple_sleep_power_loss(void)
{
	return pm_suspend_via_firmware() && !READ_ONCE(t6040_sleep_dry);
}
EXPORT_SYMBOL_GPL(apple_sleep_power_loss);

bool apple_sleep_last_steps(void)
{
	unsigned int dry = READ_ONCE(t6040_sleep_dry);

	return pm_suspend_via_firmware() && (!dry || dry >= 3);
}
EXPORT_SYMBOL_GPL(apple_sleep_last_steps);

/*
 * The coprocessor and enclave registry, with #1208's contract (sleep.h). A
 * registered coprocessor that isn't ready refuses the sleep; a ready one keeps
 * running until the commit's last stop, after every refusal, so a refused sleep
 * finds it as it was.
 */
#define SLEEP_COPROCS		6

static struct t6040_sleep_coproc {
	struct device *dev;
	void __iomem *asc;
	bool pcie_ans;
	int (*last_check)(void *data);
	int (*last_stop)(void *data);
	void *data;
	bool ready;
	bool stopped;
	u32 status;
	/* No ASC: its last stop takes it whole, power word @ps included, before the others. */
	bool first;
	u32 ps;
} t6040_sleep_coprocs[SLEEP_COPROCS];
static DEFINE_MUTEX(t6040_sleep_coprocs_lock);

/* The enclave's sleep request, left to the last refusal. Under t6040_sleep_coprocs_lock. */
static struct {
	struct device *dev;
	int (*tell)(void *data);
	void *data;
} t6040_sleep_enclave;

static struct t6040_sleep_coproc *t6040_sleep_coproc_find(struct device *dev)
{
	int i;

	for (i = 0; i < SLEEP_COPROCS; i++)
		if (t6040_sleep_coprocs[i].dev == dev)
			return &t6040_sleep_coprocs[i];

	return NULL;
}

/* Below EL2 a monitor owns the last mile, and this code doesn't run it. */
int apple_sleep_coproc_register(struct device *dev, void __iomem *asc, bool pcie_ans,
				int (*last_stop)(void *data), void *data)
{
	struct t6040_sleep_coproc *c;

	if (!is_kernel_in_hyp_mode())
		return -ENODEV;

	guard(mutex)(&t6040_sleep_coprocs_lock);
	if (t6040_sleep_coproc_find(dev))
		return -EEXIST;
	c = t6040_sleep_coproc_find(NULL);
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
	c->ps = 0;
	c->dev = dev;

	return 0;
}
EXPORT_SYMBOL_GPL(apple_sleep_coproc_register);

int apple_sleep_coproc_register_first(struct device *dev, int (*last_stop)(void *data),
				      void *data)
{
	struct t6040_sleep_coproc *c;
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

	guard(mutex)(&t6040_sleep_coprocs_lock);
	c = t6040_sleep_coproc_find(dev);
	if (c) {
		c->first = true;
		c->ps = ps;
	}

	return 0;
}
EXPORT_SYMBOL_GPL(apple_sleep_coproc_register_first);

int apple_sleep_coproc_set_last_check(struct device *dev, int (*last_check)(void *data))
{
	struct t6040_sleep_coproc *c;

	guard(mutex)(&t6040_sleep_coprocs_lock);
	c = t6040_sleep_coproc_find(dev);
	if (!c)
		return -ENOENT;
	c->last_check = last_check;

	return 0;
}
EXPORT_SYMBOL_GPL(apple_sleep_coproc_set_last_check);

int apple_sleep_enclave_register(struct device *dev, int (*tell)(void *data), void *data)
{
	if (!is_kernel_in_hyp_mode())
		return -ENODEV;

	guard(mutex)(&t6040_sleep_coprocs_lock);
	if (t6040_sleep_enclave.dev)
		return -EBUSY;
	t6040_sleep_enclave.tell = tell;
	t6040_sleep_enclave.data = data;
	t6040_sleep_enclave.dev = dev;

	return 0;
}
EXPORT_SYMBOL_GPL(apple_sleep_enclave_register);

void apple_sleep_enclave_unregister(struct device *dev)
{
	guard(mutex)(&t6040_sleep_coprocs_lock);
	if (t6040_sleep_enclave.dev == dev)
		memset(&t6040_sleep_enclave, 0, sizeof(t6040_sleep_enclave));
}
EXPORT_SYMBOL_GPL(apple_sleep_enclave_unregister);

void apple_sleep_coproc_unregister(struct device *dev)
{
	struct t6040_sleep_coproc *c;

	guard(mutex)(&t6040_sleep_coprocs_lock);
	c = t6040_sleep_coproc_find(dev);
	if (c)
		memset(c, 0, sizeof(*c));
}
EXPORT_SYMBOL_GPL(apple_sleep_coproc_unregister);

void apple_sleep_coproc_set_ready(struct device *dev, bool ready)
{
	struct t6040_sleep_coproc *c;

	guard(mutex)(&t6040_sleep_coprocs_lock);
	c = t6040_sleep_coproc_find(dev);
	if (!c)
		return;
	c->ready = ready;
	c->stopped = false;
}
EXPORT_SYMBOL_GPL(apple_sleep_coproc_set_ready);

bool apple_sleep_coproc_stopped(struct device *dev)
{
	struct t6040_sleep_coproc *c;

	guard(mutex)(&t6040_sleep_coprocs_lock);
	c = t6040_sleep_coproc_find(dev);
	return c && c->stopped;
}
EXPORT_SYMBOL_GPL(apple_sleep_coproc_stopped);

/*
 * What still keeps the commit unreachable. The SoC power-down and the
 * wake return are IMPLEMENTED from the entry as measured on the J616s and
 * from its wake restore, which was not measured (t6040_sleep_commit,
 * t6040_sleep_return), but no step
 * of a T6040 Linux sleep has run on hardware, and two subsystems still have no
 * way back from a real power loss. These are facts about this kernel, not
 * runtime state: they never clear here, so the commit stays unreachable until
 * tests on hardware retire them. The enclave's
 * sleep request and the AIC save/restore are built.
 */
static const char *const t6040_sleep_unknowns[] = {
	"U1/U2: the T6040 SoC power-down is implemented from the held entry trace (clusters off, clk-mux park, PMU boot-stage, MasterMini S2R timer) but has never been run on hardware; the PMU go-hib replay's necessity is unconfirmed",
	"U3: the T6040 wake return is implemented after the wake restore (complexes on, wake/CPM pwrgates, SOC-East retention, axi2af bridge re-arm, clkgen restore, S2R-ctrl clear) but has never been run; the aura mailbox hand-back is unproven on T6040",
	"U4: ANS's two ST-lane PCIe link + shared PHY lower/raise is implemented (t6040_sleep_lower/raise_apcie_st, a two-lane port of #1208) but needs the apple,apcie control windows + PLL/lane tunables published by the boot object; absent here, so a real sleep with ANS stopped still refuses; the GP host's Wi-Fi/SD are left to cold-recover and no HW S2R has run",
	"U5: the DCP cold restart after a real power loss is implemented (dcp_desktop_cold_restart, factored from the S4 hibernation path, wired into dcp_t6041_s2r_resume) but the display-domain lower/raise needs the apple,display-power window (absent here), DCPEXT is deferred to ext_dead, and no HW S2R has run",
};

#define T6040_SLEEP_DATA_BLOCKERS	ARRAY_SIZE(t6040_sleep_unknowns)

/*
 * As in the power-state check, a sub-block is on while its TARGET is not 0. The
 * pipe's CPU counts as on while its ACTUAL is not 0: the engine waits for the
 * CPU's actual state. A CPU under automatic power gating (AUTO_ENABLE, target 15)
 * that reads ACTUAL 0 is gated off by the PMGR and counts as off; taking it out
 * of automatic mode raised an SError on T6040, so it is left so.
 */
static bool ignore_display_cpu = true;
module_param(ignore_display_cpu, bool, 0644);
MODULE_PARM_DESC(ignore_display_cpu,
		 "Let the commit go on with the display pipes' CPU words on (the SoC power-down drops them)");

static bool t6040_sleep_ps_on(u32 val, bool cpu)
{
	if (cpu && READ_ONCE(ignore_display_cpu))
		return false;
	if (cpu)
		return FIELD_GET(PS_ACTUAL, val) ||
		       (FIELD_GET(PS_TARGET, val) && !(val & PS_AUTO_ENABLE));
	return FIELD_GET(PS_TARGET, val);
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
		seq_puts(s, "power states the platform checks before sleep (device-check-ps-b4-sleep):\n");
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

/* The total: live state plus the data this kernel is missing. Zero only if the port is complete. */
static unsigned int t6040_sleep_total_blockers(struct t6040_sleep *sleep)
{
	unsigned int data = READ_ONCE(i_know_untested) ? 0 : T6040_SLEEP_DATA_BLOCKERS;

	if (!data && T6040_SLEEP_DATA_BLOCKERS)
		pr_warn_once("t6040-sleep: i_know_untested set: IGNORING %zu never-run data blockers (U1-U5); a real commit may hang or lose devices on resume\n",
			     T6040_SLEEP_DATA_BLOCKERS);
	return t6040_sleep_live_blockers(sleep, NULL, NULL) + data;
}

static void t6040_sleep_show_registry(struct seq_file *s)
{
	int i;

	guard(mutex)(&t6040_sleep_coprocs_lock);
	seq_puts(s, "coprocessors registered for the commit's last stop:\n");
	for (i = 0; i < SLEEP_COPROCS; i++) {
		struct t6040_sleep_coproc *c = &t6040_sleep_coprocs[i];

		if (!c->dev)
			continue;
		seq_printf(s, "  %s%s%s%s%s\n", dev_name(c->dev), c->pcie_ans ? " (ANS)" : "",
			   c->first ? " (first)" : "", c->last_check ? ", last check" : "",
			   c->ready ? ", ready" : "");
	}
	seq_printf(s, "enclave sleep request left for the last refusal: %s\n",
		   t6040_sleep_enclave.dev ? dev_name(t6040_sleep_enclave.dev) : "none");
}

static void t6040_axi2af_show(struct t6040_sleep *sleep, struct seq_file *s);

static int t6040_sleep_preflight_show(struct seq_file *s, void *unused)
{
	struct t6040_sleep *sleep = s->private;
	unsigned int live = t6040_sleep_live_blockers(sleep, s, NULL);
	unsigned int i;

	t6040_sleep_show_registry(s);
	t6040_axi2af_show(sleep, s);
	seq_puts(s, "data this kernel is missing for a T6040 commit:\n");
	for (i = 0; i < T6040_SLEEP_DATA_BLOCKERS; i++)
		seq_printf(s, "  %s\n", t6040_sleep_unknowns[i]);

	seq_printf(s,
		   "verdict: %u live blocker(s) + %zu missing-data blocker(s); no entry path in this kernel\n",
		   live, T6040_SLEEP_DATA_BLOCKERS);
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
		pr_info("t6040-sleep: %-8s %s (on mask %#x, cpu first)\n", g->name,
			on_mask ? "ON" : "off", on_mask);
		blockers += !!on_mask;
	}

	return blockers;
}

/* Log the full blocker list (interrupts may be off: no allocation, pr_* only). */
static void t6040_sleep_log_blockers(struct t6040_sleep *sleep)
{
	const char *ops_name;
	unsigned int i;

	pr_info("t6040-sleep: EL2 %s, s2r-mailbox %s, parkable %u/%u (%s)\n",
		is_kernel_in_hyp_mode() ? "yes" : "no",
		of_property_present(of_chosen, "aurora,s2r-mailbox") ? "yes" : "no",
		t6040_sleep_parkable(&ops_name), num_possible_cpus() - 1, ops_name);
	for (i = 0; i < T6040_SLEEP_DATA_BLOCKERS; i++)
		pr_info("t6040-sleep: blocker: %s\n", t6040_sleep_unknowns[i]);
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
 * cluster * 0x1000000 + core * 0x100000). The platform has every CPU, boot and
 * secondary, unlock it (EDLAR) and write EDPRCR := 0 right before its deep WFI, so that
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

static bool ed_power_release = true;
module_param(ed_power_release, bool, 0644);
MODULE_PARM_DESC(ed_power_release,
		 "Before each core's deep WFI, unlock its external debug and clear EDPRCR");

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
	dev_info(dev, "external debug windows mapped for %u cores\n", n);
}

static void t6040_ed_release(void)
{
	u64 mpidr = read_cpuid_mpidr();
	unsigned int c = MPIDR_AFFINITY_LEVEL(mpidr, 1), k = MPIDR_AFFINITY_LEVEL(mpidr, 0);
	void __iomem *ed;

	if (!READ_ONCE(ed_power_release) || c >= T6040_ED_CLUSTERS || k >= T6040_ED_CORES)
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

/*
 * Bring-up observer (on the J616s, after the timer only the master core is on, P1 still 0x10;
 * the master never WFIs while it probes). One parked core is kept spinning instead of its
 * deep WFI; after the master writes the timer and takes its own deep WFI, it samples the
 * complexes and the master's core word and keeps the latest in the PMU comparator:
 *   0x3PPLLEEM  [27:22] P0, [21:16] P1, [15:10] E cluster-ctl bits [5:0],
 *               [9:6] the master core's ACTUAL, [5:0] sample number (saturates at 63)
 * Its own complex can't drop while it spins: put it in P1 to watch E and P0.
 */
static unsigned long observer_mpidr = ULONG_MAX;
module_param(observer_mpidr, ulong, 0644);
MODULE_PARM_DESC(observer_mpidr,
		 "Bring-up: MPIDR of a parked core that observes the complexes after the S2R timer (ULONG_MAX: none)");

static void t6040_mark_nodelay(struct t6040_sleep *s, u32 m)
{
	void __iomem *b = s->spmi0;
	u32 i;

	if (!b)
		return;
	for (i = 0; i < 16 && !(readl(b + SPMI_STATUS) & SPMI_STATUS_RX_EMPTY); i++)
		readl(b + SPMI_REPLY);
	writel(SPMI_CMD_PMU(PMU_RTC_COMPARE, SPMI_OP_WRITE4), b + SPMI_CMD);
	writel(m, b + SPMI_CMD);
	for (i = 0; i < 1000000; i++)
		if (readl(b + SPMI_NOTIFY_ACK) & SPMI_NOTIFY_REPLY)
			break;
	writel(SPMI_NOTIFY_REPLY, b + SPMI_NOTIFY_ACK);
	if (!(readl(b + SPMI_STATUS) & SPMI_STATUS_RX_EMPTY))
		readl(b + SPMI_REPLY);
}

static u32 t6040_a2d_state(struct t6040_sleep *s);
static u32 t6040_aop_state(struct t6040_sleep *s);

static bool observer_aop;
module_param(observer_aop, bool, 0644);
MODULE_PARM_DESC(observer_aop, "Bring-up: the observer core marks the AOP power requests (0x1 tag, t6040_aop_state)");

static bool observer_a2d;
module_param(observer_a2d, bool, 0644);
MODULE_PARM_DESC(observer_a2d,
		 "Bring-up: the observer core marks the A2D state (0x2 tag, t6040_a2d_state) instead of the complexes");

static void __noreturn t6040_observe(struct t6040_sleep *s)
{
	u32 n = 0, i, e, p0, p1, mc;

	while (!READ_ONCE(s->timer_written))
		cpu_relax();
	for (;;) {
		if (READ_ONCE(observer_aop)) {
			t6040_mark_nodelay(s, 0x10000000 | t6040_aop_state(s) | (n & 3));
			n++;
			for (i = 0; i < 500000; i++)
				(void)readl_relaxed(s->cluster_ctl);
			continue;
		}
		if (READ_ONCE(observer_a2d)) {
			t6040_mark_nodelay(s, 0x20000000 | t6040_a2d_state(s) | (n & 3));
			n++;
			for (i = 0; i < 500000; i++)
				(void)readl_relaxed(s->cluster_ctl);
			continue;
		}
		e = readl(s->cluster_ctl + CLUSTER_CTL(0)) & 0x3f;
		p0 = readl(s->cluster_ctl + CLUSTER_CTL(1)) & 0x3f;
		p1 = readl(s->cluster_ctl + CLUSTER_CTL(2)) & 0x3f;
		mc = s->soc_ps ? FIELD_GET(PS_ACTUAL, readl(s->soc_ps + 8 * 4)) : 0xf;
		t6040_mark_nodelay(s, 0x30000000 | p0 << 22 | p1 << 16 | e << 10 | mc << 6 |
				      min(n, 63U));
		n++;
		for (i = 0; i < 500000; i++)
			(void)readl_relaxed(s->cluster_ctl);
	}
}

static void t6040_cpu_off(u64 mpidr)
{
	struct t6040_sleep *s = t6040_sleep;

	if (s && s->cluster_ctl && READ_ONCE(observer_mpidr) == (mpidr & MPIDR_HWID_BITMASK))
		t6040_observe(s);
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
		dev_info(s->dev, "CPU parking power ops registered\n");
	}
}

static void t6040_sleep_set_wd2(struct t6040_sleep *s, unsigned int secs)
{
	if (!s->wdt)
		return;
	writel(0, s->wdt + WDT_WD2_CTL);
	writel(0, s->wdt + WDT_WD2_COUNT);
	writel(secs * WDT_TICKS_PER_S, s->wdt + WDT_WD2_ALARM);
	writel(1, s->wdt + WDT_WD2_CTL);
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
 * The PACC1_ANE complex (cluster-ctl +0x8) holds P1 and the ANE: after the S2R timer
 * E reads 0x00 and P1 stays 0x10. The ANE's power words, ACTUAL nibbles:
 * sys (pmgr +0x3c0), then mpm, cpu, td, base (pmgr_ane +0x0..+0x18). Returns them
 * packed sys<<16 | mpm<<12 | cpu<<8 | td<<4 | base; 0xfffff without windows.
 */
#define T6040_ANE_SYS_PS_PA	0x5022803c0ULL
#define T6040_ANE_PS_PA		0x50228c000ULL

static u32 t6040_ane_actual(struct t6040_sleep *s)
{
	u32 v, i;

	if (!s->ane_sys_ps || !s->ane_ps)
		return 0xfffff;
	v = FIELD_GET(GENMASK(7, 4), readl(s->ane_sys_ps)) << 16;
	for (i = 0; i < 4; i++)
		v |= FIELD_GET(GENMASK(7, 4), readl(s->ane_ps + i * 8)) << (12 - 4 * i);
	return v;
}

/*
 * Bring-up: how far the hardware A2D got, from registers its programs write or poll
 * (iBoot's reconfig programs, MPMGR seq 8 and MCPS seq 24):
 *   [27:20] progress code 0x508360114 (MPMGR: 4, 0x10, then 5, 7)
 *   [19:18] MasterMini+0x8214 bits 25:24 (MPMGR handshake acks)
 *   [17]    PMC request 0x5048aa13c bit 0 (MCPS := 1), [16] ack 0x5048aa140 bit 31
 *   [15:14] MiniPMGR 0x508364004 bits 1:0 (MCPS handshake)
 *   [13]    0x502850020 bit 0 (MCPS writes it after progress == 0x10)
 *   [12:9]  GPIO PS 0x502800450 ACTUAL (MCPS powers it on first)
 *   [8:5]   SIO_CPU PS 0x502280130 ACTUAL, [4] its BIT31 (MCPS := 0x80000000 after
 *           the SIO poll 0x429e08000 bit 1)
 *   [3]     DPA4 PS 0x502280250 TARGET 0 (last of the group-1 domains MCPS turns off)
 *   [2]     PS 0x502280120 TARGET 0 (turned off right after group 1)
 * Bits [1:0] are left for a sample count. Every window is optional (0 bits).
 */
static unsigned int a2d_read_mask = 0x3f;
module_param(a2d_read_mask, uint, 0644);
MODULE_PARM_DESC(a2d_read_mask,
		 "Bring-up: A2D state windows read (bit 0 MiniPMGR 0x114, 1 MasterMini 0x8214, 2 PMC 0x5048aa13c/140, 3 0x508364004, 4 0x502850020, 5 PS words)");

static u32 t6040_a2d_state(struct t6040_sleep *s)
{
	u32 v = 0, w, m = READ_ONCE(a2d_read_mask);

	if (s->a2d_mini && (m & BIT(0)))
		v |= (readl(s->a2d_mini + 0x114) & 0xff) << 20;
	if (s->a2d_mm8 && (m & BIT(1)))
		v |= ((readl(s->a2d_mm8 + 0x214) >> 24) & 3) << 18;
	if (s->a2d_pmc && (m & BIT(2))) {
		v |= (readl(s->a2d_pmc + 0x13c) & 1) << 17;
		v |= (readl(s->a2d_pmc + 0x140) >> 31) << 16;
	}
	if (s->a2d_mini4 && (m & BIT(3)))
		v |= (readl(s->a2d_mini4 + 0x4) & 3) << 14;
	if (s->a2d_blk && (m & BIT(4)))
		v |= (readl(s->a2d_blk + 0x20) & 1) << 13;
	if (s->soc_ps && (m & BIT(5)))
		v |= FIELD_GET(PS_ACTUAL, readl(s->soc_ps + 0x450)) << 9;
	if (s->soc_east && (m & BIT(5))) {
		w = readl(s->soc_east + 0x130);
		v |= FIELD_GET(PS_ACTUAL, w) << 5 | (w >> 31) << 4;
		v |= !FIELD_GET(PS_TARGET, readl(s->soc_east + 0x250)) << 3;
		v |= !FIELD_GET(PS_TARGET, readl(s->soc_east + 0x120)) << 2;
	}
	return v;
}

/*
 * Bring-up: the AOP's power requests against the MPMGR's first A2D handshake (AOP firmware:
 * AOP-local 0x5108b8100 bit 0 DRAM (status MasterMini+0x14 bit 0), bit 1 AP
 * and bit 2 AOP CPU (status MiniPMGR 0x508360108)). Mark tag 1:
 *   [27:20] progress 0x508360114, [19:18] MasterMini+0x8214 acks 25:24,
 *   [17:15] AOP requests 0x5108b8100 [2:0], [14] MasterMini+0x14 bit 0,
 *   [13:10] MasterMini+0x8214 bits 31 (REQ), 16, and MasterMini+0xc014 (IB2) bits 31, 24;
 *   [9:6] MasterMini+0xc014 bit 25, MiniPMGR +0x104 [2:0]; [5:2] MiniPMGR +0x108 [3:0];
 *   [1:0] sample count
 */
static u32 t6040_aop_state(struct t6040_sleep *s)
{
	u32 v = 0;

	if (s->a2d_mini) {
		v |= (readl(s->a2d_mini + 0x114) & 0xff) << 20;
		v |= (readl(s->a2d_mini + 0x104) & 7) << 6;
		v |= (readl(s->a2d_mini + 0x108) & 0xf) << 2;
	}
	if (s->a2d_mm8) {
		u32 h1 = readl(s->a2d_mm8 + 0x214);

		v |= ((h1 >> 24) & 3) << 18;
		v |= (h1 >> 31) << 13 | ((h1 >> 16) & 1) << 12;
	}
	if (s->a2d_mm12) {
		u32 h2 = readl(s->a2d_mm12 + 0x14);

		v |= (h2 >> 31) << 11 | ((h2 >> 24) & 1) << 10 | ((h2 >> 25) & 1) << 9;
	}
	if (s->aop_req)
		v |= (readl(s->aop_req + 0x100) & 7) << 15;
	if (s->mastermini)
		v |= (readl(s->mastermini + 0x14) & 1) << 14;
	return v;
}

static void t6040_log_a2d(struct t6040_sleep *s, const char *when)
{
	pr_info("t6040-sleep: %s: A2D state %#010x (0x2 tag in marks), AOP state %#010x (0x1 tag), raw AOP req %#x MiniPMGR+0x100/104/108 %#x/%#x/%#x MasterMini+0x14 %#x +0x8214 %#x +0xc014 %#x\n",
		when, t6040_a2d_state(s), t6040_aop_state(s),
		s->aop_req ? readl(s->aop_req + 0x100) : 0,
		s->a2d_mini ? readl(s->a2d_mini + 0x100) : 0, s->a2d_mini ? readl(s->a2d_mini + 0x104) : 0,
		s->a2d_mini ? readl(s->a2d_mini + 0x108) : 0,
		s->mastermini ? readl(s->mastermini + 0x14) : 0,
		s->a2d_mm8 ? readl(s->a2d_mm8 + 0x214) : 0, s->a2d_mm12 ? readl(s->a2d_mm12 + 0x14) : 0);
	/*
	 * The MiniPMGR handshake words the reconfig programs use (iBoot rcfg: MCPS A2D waits
	 * on +0x44 bit 2 / bit 3 and +0x40 bit 8, D2A raises +0x40/+0x3c/+0x34, D2S +0x48/+0x4c),
	 * read only, and the five cluster-ctl slots (EACC, PACC0, PACC1_ANE, DISP_INT, DISP_EXT).
	 */
	if (s->a2d_mini) {
		static const u16 hs[] = { 0x08, 0x1c, 0x24, 0x34, 0x38, 0x3c, 0x40, 0x44, 0x48, 0x4c, 0x5c, 0x114, 0x200 };
		u32 v[ARRAY_SIZE(hs)], c[CLUSTER_CTL_NR] = {};
		int i;

		for (i = 0; i < ARRAY_SIZE(hs); i++)
			v[i] = readl(s->a2d_mini + hs[i]);
		if (s->cluster_ctl)
			for (i = 0; i < CLUSTER_CTL_NR; i++)
				c[i] = readl(s->cluster_ctl + CLUSTER_CTL(i));
		pr_info("t6040-sleep: %s: MiniPMGR +08 %#x +1c %#x +24 %#x +34 %#x +38 %#x +3c %#x +40 %#x +44 %#x +48 %#x +4c %#x +5c %#x +114 %#x +200 %#x; cluster-ctl %#x %#x %#x %#x %#x\n",
			when, v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8], v[9], v[10], v[11], v[12],
			c[0], c[1], c[2], c[3], c[4]);
	}
}

static void t6040_log_ane(struct t6040_sleep *s, const char *when)
{
	u32 c[3] = {}, i;

	if (s->cluster_ctl)
		for (i = 0; i < 3; i++)
			c[i] = readl(s->cluster_ctl + CLUSTER_CTL(i));
	pr_info("t6040-sleep: %s: ANE ACTUAL sys/mpm/cpu/td/base %05x, cluster-ctl E %#x P0 %#x P1 %#x\n",
		when, t6040_ane_actual(s), c[0], c[1], c[2]);
}

/* Bring-up: the WD2 re-armed for @ms from now, writes only (usable with the counter stopped). */
static void t6040_wd2_raw(struct t6040_sleep *s, u32 ms)
{
	writel(0, s->wdt + WDT_WD2_CTL);
	writel(0, s->wdt + WDT_WD2_COUNT);
	writel(ms * (WDT_TICKS_PER_S / 1000), s->wdt + WDT_WD2_ALARM);
	writel(1, s->wdt + WDT_WD2_CTL);
	dsb(sy);
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
/* Why the last t6040_sleep_refuse() refused, one bit per check (the refusal mark). */
static u32 t6040_refuse_why;
static u32 t6040_sleep_groups_on(struct t6040_sleep *s);

static unsigned int t6040_sleep_refuse(struct t6040_sleep *s)
{
	struct t6040_sleep_watch *sep = &s->watch[T6040_WATCH_SEP];
	struct t6040_sleep_watch *ans = &s->watch[T6040_WATCH_ANS];
	unsigned int refused = 0, ready = 0, registered = 0;
	u32 why = 0;
	bool ans_idled = false;
	u32 val;
	int i;

	for (i = 0; i < SLEEP_COPROCS; i++) {
		struct t6040_sleep_coproc *c = &t6040_sleep_coprocs[i];

		if (!c->dev)
			continue;
		registered++;
		if (c->ready) {
			ready++;
			ans_idled |= c->pcie_ans;
		} else {
			/* Nothing may lower a coprocessor's domain under its driver. */
			pr_err("t6040-sleep: refused: %s is running and its driver has not quiesced it\n",
			       dev_name(c->dev));
			refused++;
			why |= BIT(0);
		}
	}
	pr_info("t6040-sleep: %u of %u registered coprocessors ready for the commit\n", ready,
		registered);

	if (num_online_cpus() != 1 || !s->mbox ||
	    (read_cpuid_mpidr() & MPIDR_HWID_BITMASK) != s->mbox->boot_mpidr) {
		pr_err("t6040-sleep: refused: wrong CPU, other CPUs online, or no mailbox\n");
		refused++;
		why |= BIT(1);
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
		why |= BIT(2);
	}

	if (!sep->bound) {
		/* Nobody told it of the sleep: a commit with it running never returns. */
		t6040_sleep_watch_on(sep, &val);
		pr_err("t6040-sleep: refused: sep isn't bound (state %#x)\n", val);
		refused++;
		why |= BIT(3);
	} else if (!t6040_sleep_enclave.tell) {
		pr_err("t6040-sleep: refused: sep's sleep request isn't left for the last refusal\n");
		refused++;
		why |= BIT(3);
	}

	/* Only ANS's driver can stop it, and it doesn't come back without a real sleep. */
	if (!ans_idled && t6040_sleep_watch_on(ans, &val)) {
		pr_err("t6040-sleep: refused: ANS is on (%#x) with no driver to stop it\n", val);
		refused++;
		why |= BIT(4);
	}

	/*
	 * ANS would be stopped, but its ST lanes have no way back without the
	 * apple,apcie control windows + PLL/lane tunables. #1208 keeps
	 * suspend-to-idle the default in this state; here the deep sleep refuses.
	 */
	if (ans_idled && !s->apcie_st.valid) {
		pr_err("t6040-sleep: refused: ANS would stop but the storage-apcie windows/tunables are absent (apple,apcie)\n");
		refused++;
		why |= BIT(5);
	}

	/* The engine waits for the display CPUs with no timeout. */
	if (t6040_sleep_log_groups(s)) {
		refused++;
		why |= BIT(6) | t6040_sleep_groups_on(s) << 8;
	}
	t6040_refuse_why = why;

	return refused;
}

/* The last refusals, each coprocessor's own: nothing has stopped yet. */
static int t6040_sleep_coprocs_check(void)
{
	int i, ret;

	for (i = 0; i < SLEEP_COPROCS; i++) {
		struct t6040_sleep_coproc *c = &t6040_sleep_coprocs[i];

		if (!c->dev || !c->ready || c->stopped || !c->last_check)
			continue;
		ret = c->last_check(c->data);
		if (ret) {
			pr_err("t6040-sleep: refused: %s: %d\n", dev_name(c->dev), ret);
			return ret;
		}
		pr_info("t6040-sleep: last check of %s passed\n", dev_name(c->dev));
	}

	return 0;
}

/* Last of all the refusals: an enclave that gated comes back only through a real sleep. */
static int t6040_sleep_enclave_tell(void)
{
	int ret;

	if (!t6040_sleep_enclave.tell)
		return 0;
	ret = t6040_sleep_enclave.tell(t6040_sleep_enclave.data);
	if (ret)
		pr_err("t6040-sleep: refused: the enclave didn't take the sleep request: %d\n",
		       ret);

	return ret;
}

/*
 * Bring-up: the platform idles the PMP (AP and IOP power state 0x201) right before the cores
 * sleep (PMP mailbox 0x500e08800) and no stop was seen after that, so the
 * coprocessor may have to stay runnable for the engine. Bit 0 keeps the PMP's run bit
 * after its idle, bit 1 ANS's.
 */
static unsigned int coproc_keep_run;
module_param(coproc_keep_run, uint, 0644);
MODULE_PARM_DESC(coproc_keep_run,
		 "Bring-up: keep the run bit after the idle at the commit (bit 0 PMP, bit 1 ANS)");

/*
 * Bring-up: A2D stalls at MiniPMGR +0x40, the AP partition that does not power down.
 * LLB's power-down runs the same A2D to completion after powering the MTP (and AOP+audio)
 * islands off; the MTP is the only coprocessor whose DAPF reaches the MiniPMGR request words
 * +0x100..+0x10b. Bit 0: power the 13 MTP island PS words (MiniPMGR 0x50830c000..0x060,
 * MTP_FABRIC..MTP_DMA) off right before the S2R timer, children first, each polled to
 * ACTUAL 0 (mark 0x57 + the words left on). Bit 1: the same at the dry-run commit point,
 * logged, then the saved words written back (the MTP firmware does not survive it).
 */
static unsigned int mtp_island_off;
module_param(mtp_island_off, uint, 0644);
MODULE_PARM_DESC(mtp_island_off,
		 "Bring-up: MTP island PS off as LLB before A2D (bit 0 real commit, bit 1 dry-run point)");

#define T6040_MTP_PS_PA		0x50830c000ULL
#define T6040_MTP_PS_WORDS	13

/* Children first. Returns the words (bit n = +8 * n) that did not reach ACTUAL 0. */
static u32 t6040_mtp_island_off(struct t6040_sleep *s, u32 *saved)
{
	u32 left = 0, v;
	int n;

	if (!s->mtp_ps)
		return GENMASK(T6040_MTP_PS_WORDS - 1, 0);
	for (n = T6040_MTP_PS_WORDS - 1; n >= 0; n--) {
		void __iomem *r = s->mtp_ps + 8 * n;

		v = readl(r);
		if (saved)
			saved[n] = v;
		/* As LLB: automatic mode off first, then the target. */
		if (v & PS_AUTO_ENABLE) {
			v &= ~PS_AUTO_ENABLE;
			writel(v, r);
		}
		if (FIELD_GET(PS_TARGET, v))
			writel(v & ~PS_TARGET, r);
		if (readl_poll_timeout_atomic(r, v, !FIELD_GET(PS_ACTUAL, v), 1, 2000))
			left |= BIT(n);
	}
	return left;
}

/*
 * Bring-up: iBoot's idle power-down (the same A2D as LLB's,
 * which completes) configures the CPU clusters' CPM right before the cluster-ctl writes and
 * the S2R request: cluster+0xe50000
 * |= 0x1c000000, poll +0x90 [19:16] in {5, 6, 10}, RMW (& 0xffffffef9c7e1fff) | 0x61816000,
 * cpm-impl-reg cluster+0xe40000 |= bit 63 | bit 0, clear 0x1c000000 again; then the boot
 * CPU's cpu-impl-reg 0x211050110 |= 0x3000000. Linux and aura never write these.
 * Bit 0: clusters 0 and 1 as iBoot, bit 1: cluster 2 too, bit 2: the boot CPU's +0x110.
 * A CPM state poll that times out leaves that cluster with 0x1c000000 cleared (mark 0x56).
 */
static unsigned int cpm_iboot;
module_param(cpm_iboot, uint, 0644);
MODULE_PARM_DESC(cpm_iboot,
		 "Bring-up: iBoot's CPM cluster config before the S2R request (bit 0 clusters 0+1, bit 1 cluster 2, bit 2 boot CPU +0x110)");

#define T6040_CPM_CFG_PA(c)	(0x210e50000ULL + (c) * 0x1000000ULL)
#define T6040_CPM_IMPL_PA(c)	(0x210e40000ULL + (c) * 0x1000000ULL)
#define T6040_CPU4_IMPL110_PA	0x211050110ULL
#define CPM_CFG_LOCK		0x1c000000ULL
#define CPM_CFG_STATE		0x90
#define CPM_CFG_KEEP		0xffffffef9c7e1fffULL
#define CPM_CFG_SET		0x61816000ULL
#define CPM_IMPL_SET		0x8000000000000001ULL

static bool t6040_cpm_state_ok(u64 v)
{
	unsigned int st = (v >> 16) & 0xf;

	return st <= 10 && (BIT(st) & 0x460);
}

static u32 t6040_cpm_iboot(struct t6040_sleep *s, unsigned int mask)
{
	u32 to = 0;
	u64 v;
	int c;

	for (c = 0; c < 3; c++) {
		void __iomem *cfg = s->cpm_cfg[c], *impl = s->cpm_impl[c];

		if (!(c < 2 ? mask & BIT(0) : mask & BIT(1)) || !cfg || !impl)
			continue;
		writeq(readq(cfg) | CPM_CFG_LOCK, cfg);
		if (readq_poll_timeout_atomic(cfg + CPM_CFG_STATE, v, t6040_cpm_state_ok(v), 1, 2000)) {
			to |= BIT(c);
			writeq(readq(cfg) & ~CPM_CFG_LOCK, cfg);
			continue;
		}
		writeq((readq(cfg) & CPM_CFG_KEEP) | CPM_CFG_SET, cfg);
		writeq(readq(impl) | CPM_IMPL_SET, impl);
		writeq(readq(cfg) & ~CPM_CFG_LOCK, cfg);
	}
	if ((mask & BIT(2)) && s->cpu4_impl110)
		writeq(readq(s->cpu4_impl110) | 0x3000000ULL, s->cpu4_impl110);
	return to;
}

static void t6040_log_cpm(struct t6040_sleep *s, const char *when)
{
	u64 v[3][3] = {};
	int c;

	for (c = 0; c < 3; c++) {
		if (s->cpm_cfg[c]) {
			v[c][0] = readq(s->cpm_cfg[c]);
			v[c][1] = readq(s->cpm_cfg[c] + CPM_CFG_STATE);
		}
		if (s->cpm_impl[c])
			v[c][2] = readq(s->cpm_impl[c]);
	}
	pr_info("t6040-sleep: %s: CPM cfg/state/impl c0 %#llx/%#llx/%#llx c1 %#llx/%#llx/%#llx c2 %#llx/%#llx/%#llx cpu4 +0x110 %#llx\n",
		when, v[0][0], v[0][1], v[0][2], v[1][0], v[1][1], v[1][2], v[2][0], v[2][1], v[2][2],
		s->cpu4_impl110 ? readq(s->cpu4_impl110) : 0);
}

/*
 * Bring-up: the AOP + audio island off as iBoot's idle power-down (PS ids
 * 0x49..0x31 in this order, LLB's id table gives each one's offset from MiniPMGR 0x508304000;
 * 0x56 appears twice there). Bit 0: right before the S2R timer (mark 0x55, words left on
 * << 8 | first index left on). Wake through the AOP may not work with it.
 */
static unsigned int aop_island_off;
module_param(aop_island_off, uint, 0644);
MODULE_PARM_DESC(aop_island_off,
		 "Bring-up: AOP + audio island PS off as iBoot before A2D (bit 0 real commit)");

#define T6040_AOP_PS_PA		0x508304000ULL
static const u16 t6040_aop_ps_order[] = {
	0x0b0, 0x128, 0x120, 0x158, 0x150, 0x148, 0x140, 0x138, 0x130, 0x118, 0x0c0, 0x0c8,
	0x0e8, 0x0f0, 0x0f8, 0x100, 0x108, 0x110, 0x118, 0x0e0, 0x0d8, 0x0d0, 0x0b8, 0x090,
	0x020, 0x098, 0x0a0, 0x0a8, 0x028, 0x030, 0x068, 0x070, 0x078, 0x080, 0x038, 0x040,
	0x048, 0x058, 0x060, 0x088, 0x050, 0x010, 0x018, 0x008, 0x000,
};


/*
 * Bring-up: which SOC / SOC-East power words are still on right before the S2R timer.
 * The reset that follows a stalled A2D loses everything but the PMU, so the bitmap (bit i =
 * ACTUAL != 0 of t6040_ps_snap_pa[i], ADT order) goes into PMU SRAM at the unused tail of
 * the SOCD container: 0xa2c0 'PSNP' + count, then the bitmap words. m4-s2r-psnap decodes it.
 */
static int t6040_pmu_cmd(struct t6040_sleep *s, u32 cmd, const u32 *tx, u32 *rx,
			 unsigned int nrx);

static bool ps_snap = true;
module_param(ps_snap, bool, 0644);
MODULE_PARM_DESC(ps_snap, "Bring-up: store the on-bitmap of the SOC/SOC-East PS words in PMU SRAM 0xa2c0 right before the S2R timer");

/* Generated from the live J616s ADT: SOC + SOC-East PS words, display CPU words left out. */
static const u32 t6040_ps_snap_pa[] = {
	0x2280000, 0x2280100, 0x2280108, 0x2280110, 0x2280118, 0x2280120,
	0x2280128, 0x2280130, 0x2280138, 0x2280140, 0x2280148, 0x2280150,
	0x2280158, 0x2280160, 0x2280168, 0x2280170, 0x2280178, 0x2280180,
	0x2280188, 0x2280190, 0x2280198, 0x22801a0, 0x22801a8, 0x22801b0,
	0x22801b8, 0x22801c0, 0x22801c8, 0x22801d0, 0x22801d8, 0x22801e0,
	0x22801e8, 0x22801f0, 0x22801f8, 0x2280200, 0x2280208, 0x2280210,
	0x2280218, 0x2280220, 0x2280228, 0x2280230, 0x2280238, 0x2280240,
	0x2280248, 0x2280250, 0x2280258, 0x2280260, 0x2280268, 0x2280270,
	0x2280278, 0x2280280, 0x2280288, 0x2280290, 0x2280298, 0x22802e0,
	0x22802e8, 0x22802f0, 0x22802f8, 0x2280300, 0x2280308, 0x2280310,
	0x2280318, 0x2280368, 0x2280370, 0x2280378, 0x2280380, 0x2280388,
	0x2280390, 0x2280398, 0x22803a0, 0x22803c0, 0x2280c00, 0x228c000,
	0x228c008, 0x228c010, 0x228c018, 0x2800000, 0x2800008, 0x2800010,
	0x2800018, 0x2800020, 0x2800028, 0x2800030, 0x2800038, 0x2800040,
	0x2800048, 0x2800050, 0x2800058, 0x2800060, 0x2800068, 0x2800070,
	0x2800078, 0x2800080, 0x2800088, 0x2800090, 0x2800100, 0x2800108,
	0x2800110, 0x2800118, 0x2800120, 0x2800128, 0x2800130, 0x2800138,
	0x2800140, 0x2800148, 0x2800150, 0x2800158, 0x2800160, 0x2800168,
	0x2800170, 0x2800178, 0x2800180, 0x2800188, 0x2800190, 0x2800198,
	0x28001a0, 0x28001a8, 0x28001b0, 0x28001b8, 0x28001c0, 0x28001c8,
	0x28001d0, 0x28001d8, 0x28001e0, 0x28001e8, 0x28001f0, 0x28001f8,
	0x2800200, 0x2800278, 0x2800288, 0x2800290, 0x2800298, 0x28002d0,
	0x2800328, 0x2800330, 0x2800338, 0x2800340, 0x2800348, 0x2800390,
	0x2800398, 0x28003a0, 0x28003a8, 0x28003b0, 0x28003b8, 0x28003c0,
	0x28003c8, 0x28003d0, 0x28003d8, 0x28003e0, 0x28003e8, 0x28003f0,
	0x28003f8, 0x2800400, 0x2800408, 0x2800410, 0x2800418, 0x2800420,
	0x2800428, 0x2800430, 0x2800438, 0x2800440, 0x2800448, 0x2800450,
	0x2800458, 0x2800468, 0x2800470, 0x2800478, 0x2800480, 0x2800488,
	0x2800490, 0x2800498, 0x2808000, 0x2808008, 0x2808010, 0x2808018,
	0x2808020,
};

#define T6040_PS_SNAP_WORDS	DIV_ROUND_UP(ARRAY_SIZE(t6040_ps_snap_pa), 32)
#define T6040_PS_SNAP_PMU	0xa2c0
#define T6040_PS_SNAP_MAGIC	0x504e5350	/* "PSNP" in PMU byte order */

static void t6040_ps_snap_take(struct t6040_sleep *s, u32 *map)
{
	int i;

	memset(map, 0, T6040_PS_SNAP_WORDS * sizeof(u32));
	for (i = 0; i < ARRAY_SIZE(t6040_ps_snap_pa); i++) {
		u32 pa = t6040_ps_snap_pa[i];
		void __iomem *b = (pa >> 16) == 0x0280 ? s->snap_soc : s->snap_east;
		u32 off = pa - ((pa >> 16) == 0x0280 ? 0x02800000 : 0x02280000);

		if (b && FIELD_GET(PS_ACTUAL, readl(b + off)))
			map[i / 32] |= BIT(i % 32);
	}
}

static void t6040_ps_snap_store(struct t6040_sleep *s)
{
	u32 map[T6040_PS_SNAP_WORDS], hdr = T6040_PS_SNAP_MAGIC;
	u32 cnt = ARRAY_SIZE(t6040_ps_snap_pa);
	int i;

	t6040_ps_snap_take(s, map);
	t6040_pmu_cmd(s, SPMI_CMD_PMU(T6040_PS_SNAP_PMU, SPMI_OP_WRITE4), &hdr, NULL, 0);
	t6040_pmu_cmd(s, SPMI_CMD_PMU(T6040_PS_SNAP_PMU + 4, SPMI_OP_WRITE4), &cnt, NULL, 0);
	for (i = 0; i < T6040_PS_SNAP_WORDS; i++)
		t6040_pmu_cmd(s, SPMI_CMD_PMU(T6040_PS_SNAP_PMU + 8 + 4 * i, SPMI_OP_WRITE4),
			      &map[i], NULL, 0);
}


/*
 * Bring-up: the stalled MCPS sits in its PMC write block (static voters 0x5048ac000..,
 * rail words 0x504820000 + rail * 0x4000, agent floors 0x504800000 + agent * 0x4000), and an
 * AP access to the agent-3 floor hung the bus on the J616s. Read the words one by one, a mark
 * (0x3d0 + index) before each so a hang names the word. Bit 0: at the dry-run commit point
 * (WD2 armed, values logged). Bit 1: in the real commit right before the S2R timer, after the
 * clock and keep-alive steps; the values go to PMU SRAM 0xa280 ('PMCP' + the 9 words).
 */
static unsigned int pmc_probe;
module_param(pmc_probe, uint, 0644);
MODULE_PARM_DESC(pmc_probe, "Bring-up: read the PMC words the A2D engine writes (bit 0 dry-run point, bit 1 real commit before the timer)");

static const u64 t6040_pmc_probe_pa[9] = {
	0x5048aa140ULL, 0x5048ac000ULL, 0x5048ac00cULL, 0x5048ac020ULL, 0x504800000ULL,
	0x50480c000ULL, 0x504820000ULL, 0x504824004ULL, 0x504828004ULL,
};
#define T6040_PMC_PROBE_PMU	0xa280
#define T6040_PMC_PROBE_MAGIC	0x50434d50	/* "PMCP" in PMU byte order */

static void t6040_mark(struct t6040_sleep *s, u32 n);

static void t6040_pmc_probe_read(struct t6040_sleep *s, u32 *v)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(t6040_pmc_probe_pa); i++) {
		t6040_mark(s, 0x3d0 + i);
		v[i] = s->pmc_probe_io[i] ? readl(s->pmc_probe_io[i]) : 0xdeadbeef;
	}
}

/*
 * Bring-up (the PMC stopped answering between the dry-run point and the timer): one read
 * of PMC 0x5048aa140 after each .enter / commit step, mark 0x380 + checkpoint before it, so a
 * hang names the first step after which the PMC is gone.
 */
static bool pmc_bisect;
module_param(pmc_bisect, bool, 0644);
MODULE_PARM_DESC(pmc_bisect, "Bring-up: read PMC 0x5048aa140 after each sleep step (mark 0x380 + step)");

static void t6040_pmc_cp(struct t6040_sleep *s, unsigned int k)
{
	if (!READ_ONCE(pmc_bisect) || !s->pmc_probe_io[0])
		return;
	t6040_mark(s, 0x380 + k);
	(void)readl(s->pmc_probe_io[0]);
}

static void t6040_pmc_probe_store(struct t6040_sleep *s, const u32 *v)
{
	u32 m = T6040_PMC_PROBE_MAGIC;
	int i;

	t6040_pmu_cmd(s, SPMI_CMD_PMU(T6040_PMC_PROBE_PMU, SPMI_OP_WRITE4), &m, NULL, 0);
	for (i = 0; i < ARRAY_SIZE(t6040_pmc_probe_pa); i++)
		t6040_pmu_cmd(s, SPMI_CMD_PMU(T6040_PMC_PROBE_PMU + 4 + 4 * i, SPMI_OP_WRITE4),
			      &v[i], NULL, 0);
}

/* As LLB per word: automatic mode off, target 0, ACTUAL polled (bounded here). */
static u32 t6040_aop_island_off(struct t6040_sleep *s)
{
	u32 left = 0, first = 0xff, v;
	int i;

	if (!s->aop_ps)
		return 0xffff;
	for (i = 0; i < ARRAY_SIZE(t6040_aop_ps_order); i++) {
		void __iomem *r = s->aop_ps + t6040_aop_ps_order[i];

		v = readl(r);
		if (v & PS_AUTO_ENABLE) {
			v &= ~PS_AUTO_ENABLE;
			writel(v, r);
		}
		if (FIELD_GET(PS_TARGET, v))
			writel(v & ~PS_TARGET, r);
		if (readl_poll_timeout_atomic(r, v, !FIELD_GET(PS_ACTUAL, v), 1, 2000)) {
			left++;
			if (first == 0xff)
				first = i;
		}
	}
	return left ? (left << 8 | first) : 0;
}

/*
 * On the J616s the device resume reached the AOP (510c10000.aop) and took an SError in its resume,
 * whose first act after a firmware-driven sleep is reading its ASC's run bit: the AOP + audio
 * island was not powered after the wake. aop_wake bit0 records 'AOPW' at PMU 0xa2c0 right
 * after the wake return's APCIe step (w1 = words on before | words on after << 8 | raise
 * timeouts << 16, w2..w7 = ACTUAL nibbles of the 44 words 0x000..0x158, 8 per word, before any
 * raise); bit1 writes back the words saved in .begin, parents first (iBoot's power-down order
 * reversed), waiting for ACTUAL where the saved target was on. The ASC stays halted: the AOP
 * driver then takes it over cold (unbind + probe after thaw).
 */
static void t6040_ret_mark(struct t6040_sleep *s, u32 n);

#define T6040_AOP_PS_WORDS	44
#define T6040_AOPW_PMU		0xa2c0
#define T6040_AOPW_MAGIC	0x57504f41	/* "AOPW" in PMU byte order */

static unsigned int aop_wake = 15;
module_param(aop_wake, uint, 0644);
MODULE_PARM_DESC(aop_wake, "Bring-up wake: bit0 record the AOP island ('AOPW' at PMU 0xa2c0), bit1 power it back as saved in .begin, bit2 words on before the sleep to manual target 0xf, bit3 read the ASC CPU_CONTROL (marks 0x58/0x59)");

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

static unsigned int t6040_aop_ps_count_on(struct t6040_sleep *s, u32 *nib)
{
	unsigned int on = 0;
	int i;

	for (i = 0; i < T6040_AOP_PS_WORDS; i++) {
		u32 a = FIELD_GET(PS_ACTUAL, readl(s->aop_ps + 8 * i));

		if (a)
			on++;
		if (nib)
			nib[i / 8] |= a << (4 * (i % 8));
	}
	return on;
}

static void t6040_aop_wake(struct t6040_sleep *s)
{
	unsigned int mode = READ_ONCE(aop_wake), before, after, to = 0;
	u32 w[10] = { T6040_AOPW_MAGIC };
	int k, i;

	if (!mode || !s->aop_ps)
		return;
	before = t6040_aop_ps_count_on(s, &w[2]);
	w[8] = w[9] = 0;
	if ((mode & BIT(1)) && s->aop_ps_saved_valid) {
		for (k = ARRAY_SIZE(t6040_aop_ps_order) - 1; k >= 0; k--) {
			u16 off = t6040_aop_ps_order[k];
			void __iomem *r = s->aop_ps + off;
			u32 v, want = s->aop_ps_saved[off / 8], now = readl(r);
			/*
			 * AOP's resume faulted again after a 1 h 43 min sleep (it had passed
			 * before): a word in automatic mode raised here gates again before the AOP
			 * driver reads its ASC. bit2: every word that was on before the sleep goes to
			 * manual target 0xf (AUTO off) and stays up through the resume.
			 */
			bool manual = (mode & BIT(2)) && FIELD_GET(PS_ACTUAL, want);

			if (manual) {
				if (FIELD_GET(PS_ACTUAL, now) == 0xf && !(now & PS_AUTO_ENABLE))
					continue;
				writel((now & ~(PS_AUTO_ENABLE | PS_TARGET)) | PS_TARGET, r);
			} else {
				if (FIELD_GET(PS_ACTUAL, now) || !FIELD_GET(PS_TARGET, want))
					continue;
				writel(want, r);
			}
			if (off / 8 < 32)
				w[8] |= BIT(off / 8);
			else
				w[9] |= BIT(off / 8 - 32);
			if (readl_poll_timeout_atomic(r, v, FIELD_GET(PS_ACTUAL, v), 1, 2000))
				to++;
		}
	}
	after = t6040_aop_ps_count_on(s, NULL);
	w[1] = before | after << 8 | min(to, 0xffffU) << 16;
	if ((mode & BIT(0)) && s->spmi0)
		for (i = 0; i < 10; i++)
			t6040_pmu_cmd(s, SPMI_CMD_PMU(T6040_AOPW_PMU + 4 * i, SPMI_OP_WRITE4),
				      &w[i], NULL, 0);

	/*
	 * With every AOP island word that was on before the sleep at manual on, the
	 * AOP's resume still faulted on its ASC: bit3 reads the ASC CPU_CONTROL here, marks
	 * 0x58 before and 0x59 after, so an SError at 0x58 says the ASC is dead right at the
	 * wake (nothing later gated it). AOPW w8 then becomes CPU_CONTROL, w9 the AOP
	 * requests 0x5108b8100 [15:0] | MasterMini+0x14 << 16.
	 */
	if ((mode & BIT(3)) && s->aop_asc && s->spmi0) {
		u32 ctl, rq;

		rq = (s->aop_req ? readl(s->aop_req + 0x100) & 0xffff : 0xffff) |
		     (s->mastermini ? (readl(s->mastermini + 0x14) & 0xffff) << 16 : 0);
		t6040_pmu_cmd(s, SPMI_CMD_PMU(T6040_AOPW_PMU + 36, SPMI_OP_WRITE4), &rq, NULL, 0);
		t6040_ret_mark(s, 0x58);
		ctl = readl(s->aop_asc + ASC_CPU_CONTROL);
		dsb(sy);
		isb();
		t6040_pmu_cmd(s, SPMI_CMD_PMU(T6040_AOPW_PMU + 32, SPMI_OP_WRITE4), &ctl, NULL, 0);
		t6040_ret_mark(s, 0x59);
	}
}

/* As read: parents first, the saved targets back. */
static void t6040_mtp_island_back(struct t6040_sleep *s, const u32 *saved)
{
	int n;

	for (n = 0; n < T6040_MTP_PS_WORDS; n++)
		writel(saved[n], s->mtp_ps + 8 * n);
}

/* As #1208's, measured on ANS: status and control read, then the run bit cleared twice. */
static void t6040_sleep_coproc_run_off(struct t6040_sleep_coproc *c)
{
	unsigned int keep = READ_ONCE(coproc_keep_run);

	if (!c->asc) {
		c->stopped = true;
		return;
	}
	c->status = readl(c->asc + ASC_CPU_STATUS);
	if ((c->pcie_ans && (keep & BIT(1))) ||
	    (!c->pcie_ans && c->last_stop && (keep & BIT(0)))) {
		c->stopped = true;
		return;
	}
	writel(readl(c->asc + ASC_CPU_CONTROL) & ~ASC_CPU_CONTROL_RUN, c->asc + ASC_CPU_CONTROL);
	writel(readl(c->asc + ASC_CPU_CONTROL) & ~ASC_CPU_CONTROL_RUN, c->asc + ASC_CPU_CONTROL);
	c->stopped = true;
}

static u32 t6040_mark_hash(const char *name);
static void t6040_mark_raw(struct t6040_sleep *s, u32 v);
static void t6040_ret_mark(struct t6040_sleep *s, u32 n);

/* After the last refusal: those registered first stop whole first, then the rest. */
static void t6040_sleep_coprocs_stop(bool first)
{
	u32 h;
	int i;

	for (i = 0; i < SLEEP_COPROCS; i++) {
		struct t6040_sleep_coproc *c = &t6040_sleep_coprocs[i];

		if (!c->dev || !c->ready || c->first != first || c->stopped)
			continue;
		/* 0x5fSFHHHH: S 1 its last stop, 2 its run bit, 3 done; F first; H its name's hash. */
		h = t6040_mark_hash(dev_name(c->dev)) | first << 16;
		t6040_mark_raw(t6040_sleep, 0x5f100000 | h);
		if (c->last_stop)
			c->last_stop(c->data);
		t6040_mark_raw(t6040_sleep, 0x5f200000 | h);
		t6040_sleep_coproc_run_off(c);
		t6040_mark_raw(t6040_sleep, 0x5f300000 | h);
		/* ANS stopped: the commit lowers its ST lanes, the wake raises them. */
		if (c->pcie_ans && t6040_sleep)
			t6040_sleep->pcie_ans_stopped = true;
	}
	dsb(sy);
}

/*
 * The 4 clk-mux park words (ClkGen SOC + these offsets), from
 * The clock quiesce; the offsets skip 0x58/0x5c.
 */
static const u32 t6040_clkgen_mux[CLKGEN_MUX_WORDS] = {
	0x38060, 0x38064, 0x38054, 0x38050,
};

/*
 * The 9 AOP-global wake-source masks the platform arms before an S2R
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
 * wake status, logged at probe so the boot after a failed sleep shows them.
 */
#define T6040_S2R_LO_COUNT_PA	0x508370970ULL

static void t6040_sleep_log_s2r_state(struct t6040_sleep *s, const char *when)
{
	void __iomem *c = ioremap_np(T6040_S2R_LO_COUNT_PA, 4);
	u32 raw = c ? readl(c) : 0, m[AOP_WAKE_WORDS] = {}, st[AOP_WAKE_WORDS] = {}, ctl = 0;
	int i;

	if (c)
		iounmap(c);
	if (s->aop_global) {
		ctl = readl(s->aop_global + AOP_S2R_CTRL);
		for (i = 0; i < AOP_WAKE_WORDS; i++) {
			m[i] = readl(s->aop_global + AOP_WAKE_MASK(i));
			st[i] = readl(s->aop_global + AOP_WAKE_STATUS(i));
		}
	}
	dev_info(s->dev, "S2R state at %s: S2R-lo count %u (raw %#x), AOP-global ctl %#x\n",
		 when, raw >> 3, raw, ctl);
	dev_info(s->dev, "S2R state at %s: wake masks %*ph\n", when,
		 (int)sizeof(m), m);
	dev_info(s->dev, "S2R state at %s: wake status %*ph\n", when,
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

/*
 * The post-timer PMU "go hib (0x7)" SPMI pairs, as the platform sends them. Opaque
 * (the abbey PMU opcode map is not decoded); replayed only if replay_pmu_go_hib
 * is set. Each is framed as the platform frames it: notify, command, optional data, poll
 * +0x80 bit0, acknowledge.
 */
static const struct {
	u32 cmd;
	u32 data;
	bool has_data;
} t6040_pmu_go_hib_seq[] = {
	{ 0x20008e38, 0,        false },
	{ 0x20088e33, 0x1e9238, true  },
	{ 0x200e8e30, 0x0,      true  },
	{ 0x20008e30, 0x41,     true  },
};

/*
 * As the platform writes the PMU over SPMI (mirror of #1208's t8140_pmu_write): the
 * reply vector masked, the command and value written to +0x4, the reply polled
 * and acknowledged, so none is left raised.
 */
static void t6040_pmu_write(struct t6040_sleep *s, u16 reg, u8 val)
{
	u32 status, enable;

	if (!s->spmi0)
		return;
	enable = readl(s->spmi0 + SPMI_NOTIFY_ENABLE);
	writel(enable & ~SPMI_NOTIFY_REPLY, s->spmi0 + SPMI_NOTIFY_ENABLE);
	writel(((u32)reg << 16) | SPMI_CMD_WRITE, s->spmi0 + SPMI_CMD);
	writel(val, s->spmi0 + SPMI_CMD);
	if (!readl_poll_timeout_atomic(s->spmi0 + SPMI_STATUS, status,
				       !(status & SPMI_STATUS_RX_EMPTY), 1, 1000))
		readl(s->spmi0 + SPMI_REPLY);
	writel(1, s->spmi0 + SPMI_NOTIFY_ACK);
	writel(enable, s->spmi0 + SPMI_NOTIFY_ENABLE);
}

/*
 * A PMU (SID 0xe) command with an extended register over spmi0, as #1314 frames T8132's:
 * drain stale replies, mask the reply vector, the command and its data, poll and acknowledge
 * the reply, then read @nrx words (a header, then the data).
 */
#define T6040_MARK(n)		(0x53340000 | (n))	/* "S4" + step */

static int t6040_pmu_cmd(struct t6040_sleep *s, u32 cmd, const u32 *tx, u32 *rx,
			 unsigned int nrx)
{
	void __iomem *b = s->spmi0;
	unsigned int i;
	u32 val, enable;
	int ret;

	if (!b)
		return -ENODEV;
	for (i = 0; i < 16 && !(readl(b + SPMI_STATUS) & SPMI_STATUS_RX_EMPTY); i++)
		readl(b + SPMI_REPLY);
	enable = readl(b + SPMI_NOTIFY_ENABLE);
	writel(enable & ~SPMI_NOTIFY_REPLY, b + SPMI_NOTIFY_ENABLE);
	writel(cmd, b + SPMI_CMD);
	if (tx)
		writel(*tx, b + SPMI_CMD);
	ret = readl_poll_timeout_atomic(b + SPMI_NOTIFY_ACK, val, val & SPMI_NOTIFY_REPLY, 1,
					1000);
	writel(SPMI_NOTIFY_REPLY, b + SPMI_NOTIFY_ACK);
	for (i = 0; !ret && i < nrx; i++) {
		ret = readl_poll_timeout_atomic(b + SPMI_STATUS, val,
						!(val & SPMI_STATUS_RX_EMPTY), 1, 1000);
		if (!ret)
			rx[i] = readl(b + SPMI_REPLY);
	}
	/* A write's reply word, if the PMU sent one. */
	if (!nrx && !(readl(b + SPMI_STATUS) & SPMI_STATUS_RX_EMPTY))
		readl(b + SPMI_REPLY);
	writel(enable, b + SPMI_NOTIFY_ENABLE);

	return ret;
}

/*
 * Bring-up progress through a reset (#1314's way on T8132): the PMU keeps its RTC comparator
 * across one, and userspace reads it back on any kernel (spmi_nvmem0 offset 0x2008). A value
 * 0x5334xxxx is the last step reached; the saved alarm + 1 s means the commit itself ran.
 */
static bool marks = true;
module_param(marks, bool, 0644);
MODULE_PARM_DESC(marks, "Write S2R progress marks into the PMU RTC comparator (bring-up)");

static void t6040_mark_raw(struct t6040_sleep *s, u32 v);

/* The furthest driver step this sleep reached, carried in the device marks' byte 2. */
static u8 t6040_furthest;

static void t6040_mark(struct t6040_sleep *s, u32 n)
{
	u32 v = T6040_MARK(n);

	if (n < 0x40 && n > READ_ONCE(t6040_furthest))
		WRITE_ONCE(t6040_furthest, n);

	t6040_mark_raw(s, v);
}

static void t6040_mark_save(struct t6040_sleep *s)
{
	u32 rx[2];

	s->alarm_ok = false;
	WRITE_ONCE(t6040_furthest, 0);
	if (!READ_ONCE(marks) || !s->spmi0)
		return;
	if (t6040_pmu_cmd(s, SPMI_CMD_PMU(PMU_RTC_COMPARE, SPMI_OP_READ4), NULL, rx, 2)) {
		pr_warn("t6040-sleep: marks: can't read the PMU comparator, no marks\n");
		return;
	}
	s->alarm_hdr = rx[0];
	s->alarm_saved = rx[1];
	s->alarm_ok = true;
}

/*
 * Per-device marks from the PM core (#1314's hook), between .begin and .end: 0x4P00HHHH, P the
 * phase (1 prepare, 2 suspend, 3 late, 4 noirq, 8-10 resume), H the FNV-1a hash of the device's
 * name folded to 16 bits. A panic keeps the low 28 bits of the last mark and sets the top nibble
 * to its kind: 0xe an SError, 0xd a fatal exception (oops), 0x6 any other panic. A bare hardware
 * reset keeps the mark as it was. The driver's own families are chosen so that a panic over them
 * never reads as a device mark: 0x5334 steps, 0x5b refusal, 0x5c display word, 0x5e bridge.
 */
static void (*pm_bringup_dev_mark)(struct device *dev, unsigned int phase);
static DEFINE_RAW_SPINLOCK(t6040_mark_lock);
static u32 t6040_last_mark;

static u32 t6040_mark_hash(const char *name)
{
	u32 h = 0x811c9dc5;

	for (; *name; name++) {
		h ^= (u8)*name;
		h *= 0x01000193;
	}
	return (h ^ (h >> 16)) & 0xffff;
}

static void t6040_mark_raw(struct t6040_sleep *s, u32 v)
{
	unsigned long flags;

	if (!s || !READ_ONCE(marks) || !s->alarm_ok)
		return;
	raw_spin_lock_irqsave(&t6040_mark_lock, flags);
	t6040_last_mark = v;
	t6040_pmu_cmd(s, SPMI_CMD_PMU(PMU_RTC_COMPARE, SPMI_OP_WRITE4), &v, NULL, 0);
	raw_spin_unlock_irqrestore(&t6040_mark_lock, flags);
}

/*
 * On the J616s a device's suspend failed before .prepare (furthest step 0x01), the PM core rolled
 * back and the resume's SError mark overwrote which one it was. susp_rec keeps the last
 * suspend-phase device mark (phases 1-4) at PMU 0xa204 too, under 'SUSF' at 0xa200 written in
 * .begin (it takes DCSR's place: use with dcs_rec=0).
 */
#define T6040_SUSF_PMU		0xa200
#define T6040_SUSF_MAGIC	0x46535553	/* "SUSF" in PMU byte order */

static bool susp_rec = true;
module_param(susp_rec, bool, 0644);
MODULE_PARM_DESC(susp_rec, "Bring-up: keep the last suspend-phase device mark at PMU 0xa204 ('SUSF' 0xa200)");

static void t6040_dev_mark(struct device *dev, unsigned int phase)
{
	struct t6040_sleep *s = t6040_sleep;
	u32 v = 0x40000000 | (phase & 0xf) << 24 | READ_ONCE(t6040_furthest) << 16 |
		(dev ? t6040_mark_hash(dev_name(dev)) : 0);

	t6040_mark_raw(s, v);
	if (READ_ONCE(susp_rec) && phase >= 1 && phase <= 4 && s && s->spmi0 && s->alarm_ok)
		t6040_pmu_cmd(s, SPMI_CMD_PMU(T6040_SUSF_PMU + 4, SPMI_OP_WRITE4), &v, NULL, 0);
}

static void t6040_susf_begin(struct t6040_sleep *s)
{
	u32 w[2] = { T6040_SUSF_MAGIC, 0 };

	if (!READ_ONCE(susp_rec) || !s->spmi0)
		return;
	t6040_pmu_cmd(s, SPMI_CMD_PMU(T6040_SUSF_PMU, SPMI_OP_WRITE4), &w[0], NULL, 0);
	t6040_pmu_cmd(s, SPMI_CMD_PMU(T6040_SUSF_PMU + 4, SPMI_OP_WRITE4), &w[1], NULL, 0);
}

void apple_sleep_bringup_mark(u32 v)
{
	if (READ_ONCE(pm_bringup_dev_mark))
		t6040_mark_raw(t6040_sleep, v);
}
EXPORT_SYMBOL_GPL(apple_sleep_bringup_mark);

static bool serror_esr;
module_param(serror_esr, bool, 0644);
MODULE_PARM_DESC(serror_esr, "An SError panic mark carries ESR[15:0] instead of the mark's low half");

static unsigned int panic_wd_secs = 15;
module_param(panic_wd_secs, uint, 0644);
MODULE_PARM_DESC(panic_wd_secs,
		 "Bring-up: a panic during a sleep cycle arms the WD0 and WD1 resets this many seconds out (0: off)");

static bool panic_msg_hash;
module_param(panic_msg_hash, bool, 0644);
MODULE_PARM_DESC(panic_msg_hash,
		 "Bring-up: a panic mark carries the CPU and a 24-bit hash of the message, not the last mark");

static int t6040_panic_mark(struct notifier_block *nb, unsigned long action, void *data)
{
	struct t6040_sleep *s = t6040_sleep;

	if (s && READ_ONCE(pm_bringup_dev_mark) && s->alarm_ok) {
		const char *msg = data;
		u32 kind = 0x60000000;
		u32 v;

		if (msg && strstr(msg, "SError"))
			kind = 0xe0000000;
		else if (msg && strstr(msg, "Fatal exception"))
			kind = 0xd0000000;
		v = (READ_ONCE(t6040_last_mark) & 0x0fffffff) | kind;
		/* An SError keeps the mark's family and step, and its syndrome's low half. */
		if (kind == 0xe0000000 && READ_ONCE(serror_esr))
			v = (v & 0xffff0000) | (read_sysreg(esr_el1) & 0xffff);
		/*
		 * Bring-up: which CPU and which message instead of the last mark:
		 * kind | CPU << 24 | FNV-1a of the first 32 message bytes folded to 24 bits.
		 */
		if (READ_ONCE(panic_msg_hash)) {
			u32 h = 0x811c9dc5;
			int i;

			for (i = 0; msg && msg[i] && i < 32; i++)
				h = (h ^ (u8)msg[i]) * 0x01000193;
			v = kind | (raw_smp_processor_id() & 0xf) << 24 | ((h ^ (h >> 24)) & 0xffffff);
		}

		t6040_pmu_cmd(s, SPMI_CMD_PMU(PMU_RTC_COMPARE, SPMI_OP_WRITE4), &v, NULL, 0);

		/*
		 * After an S2R wake the SMC has had no 'waka' yet and the 'phra' restart can
		 * hang (on the J616s an SError panic right after the resume left the Mac dead for
		 * 12 min, f803 0x50). Arm the WD0 and WD1 resets as a backstop behind the
		 * normal panic restart; WD0 is the block the SMC logger bites.
		 */
		if (s->wdt && READ_ONCE(panic_wd_secs)) {
			u32 t = min(READ_ONCE(panic_wd_secs), WDT_MAX_SECS) * WDT_TICKS_PER_S;

			writel(0, s->wdt + WDT_WD0_CTL);
			writel(0, s->wdt + WDT_WD0_COUNT);
			writel(t, s->wdt + WDT_WD0_BITE);
			writel(WDT_CTL_RESET_EN, s->wdt + WDT_WD0_CTL);
			writel(0, s->wdt + WDT_WD1_CTL);
			writel(0, s->wdt + WDT_WD1_COUNT);
			writel(t, s->wdt + WDT_WD1_BITE);
			writel(WDT_CTL_RESET_EN, s->wdt + WDT_WD1_CTL);
			dsb(sy);
		}
	}
	return NOTIFY_DONE;
}

static struct notifier_block t6040_panic_nb = {
	.notifier_call = t6040_panic_mark,
};

/* The wake alarm back, 1 s later, so a reset after this point reads as "the commit ran". */
static void t6040_mark_restore(struct t6040_sleep *s)
{
	u32 v = s->alarm_saved + 1;

	if (s->alarm_ok)
		t6040_pmu_cmd(s, SPMI_CMD_PMU(PMU_RTC_COMPARE, SPMI_OP_WRITE4), &v, NULL, 0);
}

/*
 * The optional "go hib" replay. Necessity from Linux is unconfirmed. Its second
 * pair writes the PMU RTC compare (reg 0x2008); the measured value 0x1e9238 was the
 * alarm of that moment and lies in the past now, which would fire the alarm at once. The live alarm
 * goes there instead (the one t6040_mark_restore puts back), or nothing is replayed.
 */
#define PMU_GO_HIB_COMPARE_CMD	0x20088e33

static void t6040_pmu_go_hib(struct t6040_sleep *s)
{
	u32 status, data;
	int i;

	if (!s->spmi0 || !s->alarm_ok)
		return;
	for (i = 0; i < ARRAY_SIZE(t6040_pmu_go_hib_seq); i++) {
		data = t6040_pmu_go_hib_seq[i].cmd == PMU_GO_HIB_COMPARE_CMD ?
		       s->alarm_saved + 1 : t6040_pmu_go_hib_seq[i].data;
		readl(s->spmi0 + SPMI_STATUS);
		writel(SPMI_GO_HIB_NOTIFY, s->spmi0 + SPMI_NOTIFY_ENABLE);
		writel(t6040_pmu_go_hib_seq[i].cmd, s->spmi0 + SPMI_CMD);
		if (t6040_pmu_go_hib_seq[i].has_data)
			writel(data, s->spmi0 + SPMI_CMD);
		readl_poll_timeout_atomic(s->spmi0 + SPMI_NOTIFY_ACK, status,
					  status & SPMI_NOTIFY_REPLY, 1, 1000);
		writel(1, s->spmi0 + SPMI_NOTIFY_ACK);
	}
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
		unsigned int dc = READ_ONCE(disp_clusters);

		val = readl(s->cluster_ctl + CLUSTER_CTL(i));
		s->cluster_saved[i] = val;
		s->cluster_after[i] = val;
		if (i >= CLUSTER_CTL_FIRST_WAIT && !dc)
			continue;
		writel(val & ~CLUSTER_CTL_NIBBLE, s->cluster_ctl + CLUSTER_CTL(i));
		if (i >= CLUSTER_CTL_FIRST_WAIT && dc == 1 &&
		    readl_poll_timeout_atomic(s->cluster_ctl + CLUSTER_CTL(i), val,
					      !(val & CLUSTER_CTL_BUSY), 1, CLUSTER_TIMEOUT_US))
			s->quiesce_to |= QTO_CLUSTER(i);
		s->cluster_after[i] = readl(s->cluster_ctl + CLUSTER_CTL(i));
	}
}

static void t6040_east_clk_quiesce(struct t6040_sleep *s, u32 off, int bit)
{
	u32 val = readl(s->soc_east + off), cur;

	writel(val & ~SOC_EAST_CLK_AUTO, s->soc_east + off);
	if (readl_poll_timeout_atomic(s->soc_east + off, cur,
				      FIELD_GET(PS_ACTUAL, cur) == FIELD_GET(PS_TARGET, val), 1,
				      CLK_QUIESCE_TIMEOUT_US))
		s->quiesce_to |= QTO_EAST_CLK(bit);
}

/* The clock quiesce, saving the clk-mux words for wake. */
static void t6040_clkgen_park(struct t6040_sleep *s)
{
	u32 cur;
	int i;

	if (s->soc_east) {
		t6040_east_clk_quiesce(s, SOC_EAST_CLK_A, 0);
		t6040_east_clk_quiesce(s, SOC_EAST_CLK_B, 1);
	}
	if (!s->clkgen)
		return;
	for (i = 0; i < CLKGEN_MUX_WORDS; i++) {
		void __iomem *reg = s->clkgen + t6040_clkgen_mux[i];
		u32 val = readl(reg);

		s->clkgen_mux_saved[i] = val;
		writel((val & CLKGEN_MUX_MASK) | CLKGEN_MUX_SET, reg);
		if (readl_poll_timeout_atomic(reg, cur, !(cur & CLKGEN_MUX_PENDING), 1,
					      CLK_QUIESCE_TIMEOUT_US))
			s->quiesce_to |= QTO_MUX(i);
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
 * ECPU_0.. PCPU1_5 in the ADT) to read ACTUAL 0. Returns how many stayed on.
 */
static unsigned int t6040_secondaries_off(struct t6040_sleep *s)
{
	u32 self = t6040_cpu_start_bit(read_cpuid_mpidr() & MPIDR_HWID_BITMASK);
	unsigned int cpu, on = 0;
	u32 v;

	if (!s->soc_ps)
		return 0;
	for_each_present_cpu(cpu) {
		u32 bit = t6040_cpu_start_bit(cpu_logical_map(cpu));

		if (!bit || bit == self)
			continue;
		if (readl_poll_timeout_atomic(s->soc_ps + 8 * __ffs(bit), v,
					      !FIELD_GET(PS_ACTUAL, v), 1, 10000))
			on++;
	}
	return on;
}

/* The hardware steps of the generic PMGR quiesce, in the order (see t6040_acc_apsc_pa). */
static void t6040_pmgr_quiesce(struct t6040_sleep *s)
{
	u64 q;
	u32 v;
	int i;

	for (i = 0; i < ARRAY_SIZE(t6040_acc_apsc_pa); i++) {
		void __iomem *acc = s->acc_apsc[i];

		if (!acc)
			continue;
		t6040_mark(s, 0x300 | i);
		writeq(readq(acc) | ACC_APSC_DISABLE, acc);
		if (readq_poll_timeout_atomic(acc, q, !(q & ACC_APSC_BUSY), 1, QUIESCE_TIMEOUT_US))
			s->quiesce_to |= QTO_APSC(i);
	}
	for (i = 0; i < ACC_PLL_RELOCK_COMPLEXES; i++)
		if (s->acc_apsc[i]) {
			t6040_mark(s, 0x310 | i);
			writeq(readq(s->acc_apsc[i]) & ~ACC_PLL_RELOCK, s->acc_apsc[i]);
		}
	for (i = 0; i < ARRAY_SIZE(t6040_tvm); i++) {
		if (!s->tvm_ctl[i] || !s->tvm_status[i])
			continue;
		if (t6040_tvm[i].ctl >> 32 == 0x2 && !READ_ONCE(tvm_gpu))
			continue;
		t6040_mark(s, 0x320 | i);
		writel(readl(s->tvm_ctl[i]) & ~TVM_ENABLE, s->tvm_ctl[i]);
		if (readl_poll_timeout_atomic(s->tvm_status[i], v, !(v & TVM_BUSY), 1,
					      QUIESCE_TIMEOUT_US))
			s->quiesce_to |= QTO_TVM(i);
	}
	if (s->pmc_ap_floor && READ_ONCE(pmc_floor)) {
		t6040_mark(s, 0x330);
		writel(readl(s->pmc_ap_floor) & ~PMC_FLOOR_DCS, s->pmc_ap_floor);
		if (readl_poll_timeout_atomic(s->pmc_ap_floor + PMC_PENDING, v,
					      !(v & PMC_PENDING_AP), 1, 8000))
			s->quiesce_to |= QTO_PMC;
	}
}

/* The AGX residency gate of the PMGR quiesce. Returns a mark step for how far it got. */
static u32 t6040_agx_gate(struct t6040_sleep *s)
{
	u32 gate, val, cur;

	s->afr_saved = 0;
	if (!s->agx_gate || !s->afr_ps)
		return 0;
	t6040_mark(s, 0x340);
	gate = readl(s->agx_gate);
	if (!(~gate & AGX_GATE_RESIDENT))
		return 1;
	t6040_mark(s, 0x341);
	val = readl(s->afr_ps);
	if (!FIELD_GET(PS_TARGET, val))
		return 2;
	s->afr_saved = val;
	writel((val & ~AFR_LEVEL) | FIELD_PREP(AFR_LEVEL, 4), s->afr_ps);
	if (readl_poll_timeout_atomic(s->afr_ps, cur, FIELD_GET(AFR_LEVEL, cur) == 4, 1,
				      AGX_TIMEOUT_US))
		return 3;
	if (readl_poll_timeout_atomic(s->afr_ps, cur, FIELD_GET(PS_ACTUAL, cur) == 4, 1,
				      AGX_TIMEOUT_US))
		return 4;
	writel(0, s->afr_ps);
	if (readl_poll_timeout_atomic(s->afr_ps, cur, !FIELD_GET(PS_ACTUAL, cur), 1,
				      AGX_TIMEOUT_US))
		return 5;
	return 6;
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
 * power word reads [7:4]==0. We only log the ones still on, as a safety check.
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
			pr_info("t6040-sleep: axi2af %s still on (%#x) at commit\n",
				t6040_axi2af[i].name, val);
			on++;
		}
	}
	if (on)
		pr_info("t6040-sleep: %d axi2af domain(s) still powered at commit\n", on);
}

/*
 * The wake-side axi2af re-arm: every retained domain powered back up (PS
 * TARGET == 0xf) gets its fabric bridge +0x104/+0x108 bit0 set, or the bridge
 * drops its traffic. The highest-risk wake write.
 */
/*
 * On the J616s the first real resume took an asynchronous SError in here (mark 0x36 landed, 0x37
 * did not). Mode 1 re-arms every domain with TARGET 0xf, as the wake restore does; mode 2 also
 * wants ACTUAL on, since an AUTO word can read TARGET 0xf with the domain gated, and its
 * bridge is then off too; 0 skips the re-arm. Each domain is marked before (0x5eII01VV) and
 * after (0x5eII09VV) its bridge writes, VV the PS word's low byte (ACTUAL << 4 | TARGET).
 */
static unsigned int axi2af_rearm_mode = 2;
module_param(axi2af_rearm_mode, uint, 0644);
MODULE_PARM_DESC(axi2af_rearm_mode,
		 "Bring-up wake: axi2af bridge re-arm 0 off, 1 TARGET 0xf (as the platform), 2 TARGET 0xf and ACTUAL on");

static void t6040_axi2af_rearm(struct t6040_sleep *s)
{
	unsigned int mode = READ_ONCE(axi2af_rearm_mode);
	int i;

	for (i = 0; i < T6040_AXI2AF_DOMAINS; i++) {
		void __iomem *ps = t6040_axi2af_ps(s, &t6040_axi2af[i]);
		void __iomem *bridge = s->axi2af_bridge[i];
		u32 val;

		if (!ps || !bridge)
			continue;
		val = readl(ps);
		if (!mode || FIELD_GET(PS_TARGET, val) != 0xf)
			continue;
		if (mode >= 2 && !FIELD_GET(PS_ACTUAL, val)) {
			pr_info("t6040-sleep: wake: axi2af %s not re-armed, PS %#x (gated)\n",
				t6040_axi2af[i].name, val);
			continue;
		}
		dsb(sy);
		isb();
		t6040_mark_raw(s, 0x5e000100 | i << 16 | (val & 0xff));
		writel(readl(bridge + AXI2AF_BRIDGE_104) | BIT(0), bridge + AXI2AF_BRIDGE_104);
		writel(readl(bridge + AXI2AF_BRIDGE_108) | BIT(0), bridge + AXI2AF_BRIDGE_108);
		dsb(sy);
		isb();
		t6040_mark_raw(s, 0x5e000900 | i << 16 | (val & 0xff));
	}
}

/*
 * Bring-up: every AP wake source masked, as LLB and iBoot do before their power-down (the
 * same A2D, which completes there). Nothing but a cold boot (the power key) ends that sleep.
 */
static bool wake_mask_all;
module_param(wake_mask_all, bool, 0644);
MODULE_PARM_DESC(wake_mask_all,
		 "Bring-up: mask all 9 AOP-global wake words as LLB (no wake source: power key only)");

/* Arm the 9 AOP-global wake masks before the commit, saving the old words. */
static void t6040_wake_arm(struct t6040_sleep *s)
{
	bool all = READ_ONCE(wake_mask_all);
	int i;

	if (!s->aop_global)
		return;
	for (i = 0; i < AOP_WAKE_WORDS; i++) {
		s->wake_mask_saved[i] = readl(s->aop_global + AOP_WAKE_MASK(i));
		writel(all ? 0xffffffff : t6040_wake_mask[i], s->aop_global + AOP_WAKE_MASK(i));
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
 * ANS's storage-apcie power words (the two ST lanes + ANS) live in SOC-East.
 * These helpers drive one power word in a given bank, as #1208's t8140_ps_*
 * do in its single PMGR bank: lower is AUTO_ENABLE off then target 0; raise is
 * the target, then AUTO_ENABLE once ACTUAL reads on. Writing PS_FLAGS clears
 * them. Separate from t6040_sleep_ps_on(), which reads a cached value.
 */
static u32 t6040_ps_lower_start(void __iomem *base, u32 offset)
{
	void __iomem *reg = base + offset;
	u32 val = readl(reg);

	if (val & PS_AUTO_ENABLE) {
		val &= ~(PS_AUTO_ENABLE | PS_FLAGS);
		writel(val, reg);
	}

	return val;
}

static void t6040_ps_lower_finish(void __iomem *base, u32 offset, u32 val)
{
	writel((val & ~PS_TARGET) | PS_FLAGS, base + offset);
}

static void t6040_ps_lower(void __iomem *base, u32 offset)
{
	t6040_ps_lower_finish(base, offset, t6040_ps_lower_start(base, offset));
}

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

static int t6040_ps_wait_off(void __iomem *base, u32 offset)
{
	u32 val;

	return readl_poll_timeout_atomic(base + offset, val, !FIELD_GET(PS_ACTUAL, val), 10,
					 PS_OFF_TIMEOUT_US);
}

static bool t6040_ps_actual_on(void __iomem *base, u32 offset)
{
	return FIELD_GET(PS_ACTUAL, readl(base + offset));
}

/* Read-modify-write a tunable in table order, writing only where it changes. */
static void t6040_tunable_apply_changed(void __iomem *regs, const struct apple_tunable *t)
{
	size_t i;

	if (!t)
		return;
	for (i = 0; i < t->sz; i++) {
		void __iomem *reg = regs + t->values[i].offset;
		u32 old = readl(reg);
		u32 val = (old & ~t->values[i].mask) | (t->values[i].value & t->values[i].mask);

		if (val != old)
			writel(val, reg);
	}
}

/* The shared PHY quieted (as T8140's t8140_apcie_phy_off): both clocks off, run bits down. */
static void t6040_apcie_phy_off(struct t6040_apcie_st *a)
{
	void __iomem *ctrl = a->phy + APCIE_PHY_CTRL;
	void __iomem *run = a->phy + APCIE_PHY_RUN;
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

/* The shared clkgen off (as T8140's t8140_apcie_clk_off). */
static void t6040_apcie_clk_off(struct t6040_apcie_st *a)
{
	void __iomem *clk = a->clkgen + APCIE_CLK_CTRL;
	u32 val;

	writel(readl(clk) & ~APCIE_CLK_RUN, clk);
	writel(readl(clk) | APCIE_CLK_OFF, clk);
	readl_poll_timeout_atomic(clk, val, !(val & APCIE_CLK_BUSY), 1, APCIE_POLL_US);
	writel(readl(clk) | APCIE_CLK_OFF2, clk);
}

/*
 * ENTRY: lower ANS's storage link, the two-lane adaptation of #1208's
 * t8140_sleep_lower_apcie(). Called from the commit's lower_io only once ANS
 * has been stopped by its driver (pcie_ans_stopped). The PHY/clkgen/axi are
 * shared across the lanes; only the two sys_st/st domains are per-lane.
 *
 * Not known (needs the first S2R on hardware): whether T6040 truly shares the PHY across
 * st0/st1 (assumed here) or gates a per-lane PHY, and whether a separate
 * ps_apcie_phy_sw domain exists (none in the DT, so none is driven). A wrong
 * shared/per-lane split only times out the raise's ready poll, which names the
 * failing stage; the power goes next either way.
 */
/*
 * The A2D engine (iBoot MCPS seq 24) powers this group down itself, in its own order:
 * APCIE_PHY_SW (0x398) to clock-gated, polled for PARENT_MISSING (bit 11) clear, then
 * APCIE_SYS_ST0/1 off, APCIE_ST0/1 clock-gated, ANS off, APCIE_SYS_GP off, APCIE_GP
 * clock-gated. APCIE_PHY_SW's parents are APCIE_SYS_ST0/1 (ADT): with them already off
 * the engine waits on PARENT_MISSING for good (on the J616s: MCPS stuck at 0x502280398).
 * apcie_st_ps=0 leaves the power words to the engine (the PHY, clock and AXI quiesce
 * still run); 1 lowers them here as #1208 does on T8140.
 */
static bool apcie_st_ps;
module_param(apcie_st_ps, bool, 0644);
MODULE_PARM_DESC(apcie_st_ps, "Lower ANS's ST lanes and ANS in the commit (0: leave them to the A2D engine, which needs APCIE_PHY_SW's parents on)");

/*
 * With apcie_st_ps=0 the engine still waits on APCIE_PHY_SW: right before the timer
 * APCIE_ST0/1, ANS and APCIE_SYS_ST0 are on but APCIE_SYS_ST1 is off (Linux never uses the
 * second lane), and APCIE_PHY_SW's parents are both SYS_ST lanes. apcie_st1_on raises
 * APCIE_SYS_ST1 (target 0xf, no automatic mode) before the request; the engine lowers it.
 */
static bool apcie_st1_on = true;
module_param(apcie_st1_on, bool, 0644);
MODULE_PARM_DESC(apcie_st1_on, "Bring-up: power APCIE_SYS_ST1 on before the S2R request (APCIE_PHY_SW's second parent)");

/*
 * With both SYS_ST lanes on the engine still waits on APCIE_PHY_SW bit 11. The platform never
 * leaves that gate to the engine: its PCIe quiesce puts the shared PHY to sleep (the root
 * complex disable), gates APCIE_PHY_SW itself while APCIE_GP is still on (the clock quiesce),
 * then shuts the PCIe PLLs down and gates the other seven.
 * apcie_seq: bit 0 S1 as the platform; bit 1 APCIE_PHY_SW to clock-gated (the engine's 0x0f000244,
 * bit 11 polled clear) then S3 as the platform; bit 2 record 'APCS' + flags + PHY/PS words in PMU SRAM
 * 0xa200; bit 3 APCIE_GP to clock-gated (the engine's 0x244) instead of off.
 * Flags (word 1): 0 S1 ran, 1 PHY_SW gate ran, 2 S3 ran, 3 ANS stopped, 4 windows valid,
 * 5 GP clock-gated; timeouts 8 CLK1ACK, 9 CLK0ACK, 16 PHY_SW bit 11, 17 clkgen busy, 18 GP.
 */
static unsigned int apcie_seq = 11;
module_param(apcie_seq, uint, 0644);
MODULE_PARM_DESC(apcie_seq, "Bring-up: storage PCIe quiesce as the platform (bit 0 PHY sleep, 1 APCIE_PHY_SW gate + PLL off, 2 record at PMU 0xa200, 3 APCIE_GP clock-gated)");

#define T6040_APCIE_DIAG_PMU	0xa200
#define T6040_APCIE_DIAG_MAGIC	0x53435041	/* "APCS" in PMU byte order */

/* S1, the root complex disable: the shared PHY and PHY-IP to sleep; the platform spins unbounded. */
static void t6040_apcie_phy_off(struct t6040_sleep *s)
{
	struct t6040_apcie_st *a = &s->apcie_st;
	void __iomem *ctrl = a->phy + APCIE_PHY_CTRL;
	void __iomem *run = a->phy + APCIE_PHY_RUN;
	u32 val;

	s->apcie_diag[2] = readl(ctrl);
	writel(readl(ctrl) | APCIE_PHY_CTRL_UP, ctrl);
	udelay(10);
	writel(readl(a->phy_ip + 0x38) & ~GENMASK(27, 26), a->phy_ip + 0x38);
	writel(readl(a->phy_ip + 0x8c) | GENMASK(27, 24), a->phy_ip + 0x8c);
	udelay(5);
	writel(readl(ctrl) | APCIE_PHY_CTRL_RESET, ctrl);
	udelay(1);
	writel(readl(ctrl) & ~APCIE_PHY_CTRL_CLK1REQ, ctrl);
	if (readl_poll_timeout_atomic(ctrl, val, !(val & APCIE_PHY_CTRL_CLK1ACK), 1, APCIE_POLL_US))
		s->apcie_diag[1] |= BIT(8);
	udelay(1);
	writel(readl(ctrl) & ~APCIE_PHY_CTRL_CLK0REQ, ctrl);
	if (readl_poll_timeout_atomic(ctrl, val, !(val & APCIE_PHY_CTRL_CLK0ACK), 1, APCIE_POLL_US))
		s->apcie_diag[1] |= BIT(9);
	udelay(1);
	writel(readl(ctrl) & ~APCIE_PHY_CTRL_UP, ctrl);
	udelay(10);
	writel(readl(run) & ~APCIE_PHY_RUN_EN2, run);
	writel(readl(run) & ~APCIE_PHY_RUN_EN, run);
	s->apcie_diag[3] = readl(ctrl);
	s->apcie_diag[4] = readl(run);
	s->apcie_diag[1] |= BIT(0);
}

/* S2 step 1, gate idx 7 off: APCIE_PHY_SW gated while APCIE_GP is still on. */
static void t6040_apcie_phy_sw_gate(struct t6040_sleep *s)
{
	void __iomem *reg = s->soc_east + PS_APCIE_PHY_SW;
	u32 val;

	s->apcie_diag[5] = readl(reg);
	writel(0x0f000244, reg);
	if (readl_poll_timeout_atomic(reg, val, !(val & PS_BUSY), 1, PS_OFF_TIMEOUT_US))
		s->apcie_diag[1] |= BIT(16);
	s->apcie_diag[6] = readl(reg);
	s->apcie_diag[1] |= BIT(1);
}

/* S3, the PCIe PLL shutdown: PCIECLKGEN +0 as the platform. */
static void t6040_apcie_pll_off(struct t6040_sleep *s)
{
	void __iomem *clk = s->apcie_st.clkgen + APCIE_CLK_CTRL;
	u32 val;

	writel((readl(clk) & ~GENMASK(5, 3)) | BIT(4) | BIT(3), clk);
	writel(readl(clk) | BIT(1), clk);
	writel(readl(clk) & ~BIT(0), clk);
	if (readl_poll_timeout_atomic(clk, val, !(val & APCIE_CLK_BUSY), 1, APCIE_POLL_US))
		s->apcie_diag[1] |= BIT(17);
	writel(readl(clk) | BIT(2), clk);
	s->apcie_diag[7] = readl(clk);
	s->apcie_diag[1] |= BIT(2);
}

/*
 * With APCIE_PHY_SW gated the MCPS finished A2D phase 1 (seq 24, marks 0x16/0x17) and
 * stalls in seq 25 raising ATC0_COMMON (0x30f, ACTUAL f polled): its parent FAB2_SOC is off at
 * the timer, as are FAB1/4/7_SOC (genpd powers fab2_soc off with the ATC commons at
 * suspend_noirq). On the platform the fabrics stay up in D, and the D2A polls these 13 words for AUTO 1
 * and target f. fab_on raises each one not in that state (target f, ACTUAL f polled, then
 * AUTO) right before the timer and records 'FABS' + raised mask + timeout mask + the 13 words
 * in PMU SRAM 0xa240; a timeout also marks 0x54d0MMMM.
 */
/*
 * All 13 were already AUTO + target f except SBR and FAB_AFR, but FAB1/2/4/7_SOC read
 * 0x1000030f: automatic level 0, so they power-gate when idle (ACTUAL 0) and the engine still
 * waits for ATC0_COMMON. Bit 1 sets their automatic level to f (as FAB0/FAB3, 0x1f0000ff) and
 * polls ACTUAL f; record word 1/2 bits 16+ name those words.
 */
/*
 * The level write does not stick (FAB1/2/4 read back 0x1000030f), but the MCPS got
 * through ATC0 (ATC0_PHYMXWRAP on) and now waits for ATC1_COMMON. On the platform the ATC commons are
 * still on when the engine runs: seq 25 hands each port to its AON side and then powers the
 * common off itself, and the powered children hold FAB2_SOC up. Bit 2 raises FAB2_SOC by hand
 * (AUTO off, target f), then ATC0..3_COMMON (target f, no automatic mode), then gives FAB2_SOC
 * its AUTO back; 'ATCC' + FAB2 before/after + the 4 commons before/after go to PMU 0xa280,
 * timeouts to record word 2 bit 13 (FAB2) and bits 28-31 (ATC0-3).
 */
/*
 * With ATC1's dwc3 unbound the MCPS stalled at ATC0 again. Over five runs ATC0's
 * hand-over passed exactly when FAB7_SOC (child DISPEXT1_SYS; the PHY mux wrapper carries the
 * DISPEXT DP outputs into the ATC PHYs) happened to be up (0x1f0003ff) and stalled when it was
 * auto-gated (0x1000030f); bits 27:24 read back the automatic state, they are not a setting.
 * Bit 3 holds FAB1/2/4/7_SOC on by hand (AUTO off, target f, ACTUAL f polled) through the
 * timer; record word 1/2 bits 16+i name them (bit 1 is not used with it).
 */
static unsigned int fab_on = 5;
module_param(fab_on, uint, 0644);
MODULE_PARM_DESC(fab_on, "Bring-up: fabric PS words before the S2R timer (bit 0 AUTO + target f as the D2A expects, bit 1 automatic level f, bit 2 ATC commons on under FAB2; records at PMU 0xa240/0xa280)");

#define T6040_ATC_PMU		0xa280
#define T6040_ATC_MAGIC		0x43435441	/* "ATCC" in PMU byte order */
#define PS_FAB2_SOC		0x338

static const u16 t6040_atc_common[4] = { 0x390, 0x3b8, 0x3e0, 0x408 };

static void t6040_atc_commons_on(struct t6040_sleep *s, u32 *timeouts)
{
	void __iomem *b = s->snap_soc;
	u32 rec[11] = { T6040_ATC_MAGIC };
	int i;

	if (!b)
		return;
	rec[1] = readl(b + PS_FAB2_SOC);
	for (i = 0; i < 4; i++)
		rec[3 + i] = readl(b + t6040_atc_common[i]);
	if (t6040_ps_raise_start(b, PS_FAB2_SOC))
		*timeouts |= BIT(13);
	for (i = 0; i < 4; i++)
		if (t6040_ps_raise_start(b, t6040_atc_common[i]))
			*timeouts |= BIT(28 + i);
	t6040_ps_raise_finish(b, PS_FAB2_SOC);
	rec[2] = readl(b + PS_FAB2_SOC);
	for (i = 0; i < 4; i++)
		rec[7 + i] = readl(b + t6040_atc_common[i]);
	for (i = 0; i < ARRAY_SIZE(rec); i++)
		t6040_pmu_cmd(s, SPMI_CMD_PMU(T6040_ATC_PMU + 4 * i, SPMI_OP_WRITE4), &rec[i],
			      NULL, 0);
}

/*
 * On the first real S2R entry the wake program (seq 21) powers the ATC ports back up and
 * stalls at port 3: ATCPHY_CFG(3) +0x2c := 1, then +0x04 bit 8 (ACIO80PHY_AHB_CLK_ACK) never
 * rises. Ports 0..2 pass the same step. The PHY S2R entry, run on every port before
 * sleep, sets +0x2c bit 0 (S2R_AON_SYNC_RST_N), +0x08 bit 0 (CFG_S2R_FSM_EN) and +0x28 bit 0
 * (AHB_CLK_REQ_N) and waits for that ack. atc_cfg_dump logs +0x00/04/08/24/
 * 28/2c of the 4 ports now; atc_s2r (port mask) does the three writes + the ack poll right
 * before the timer, after the commons are up (fab_on bit 2). The 'ATCF' record at PMU 0xa240
 * (instead of 'FABS') keeps, per port, +0x04 and a word of +0x08/+0x24/+0x28/+0x2c low bytes.
 */
static unsigned int atc_s2r = 8;
module_param(atc_s2r, uint, 0644);
MODULE_PARM_DESC(atc_s2r, "Bring-up: port mask; the platform aciophy_s2r_entry's CFG writes (+0x2c, +0x08, +0x28 |= 1, poll +0x04 bit 8) right before the S2R timer");
static bool atc_rec = true;
module_param(atc_rec, bool, 0644);
MODULE_PARM_DESC(atc_rec, "Bring-up: record the 4 ATCPHY_CFG blocks at PMU 0xa240 right before the S2R timer (replaces 'FABS')");

#define T6040_ATCF_MAGIC	0x46435441	/* "ATCF" in PMU byte order */

static u32 t6040_atc_s2r_entry(struct t6040_sleep *s, unsigned int mask)
{
	u32 to = 0, v;
	int n;

	for (n = 0; n < 4; n++) {
		void __iomem *c = s->atc_cfg[n];

		if (!(mask & BIT(n)) || !c)
			continue;
		writel(readl(c + 0x2c) | BIT(0), c + 0x2c);
		udelay(1);
		writel(readl(c + 0x08) | BIT(0), c + 0x08);
		udelay(5);
		writel(readl(c + 0x28) | BIT(0), c + 0x28);
		if (readl_poll_timeout_atomic(c + 0x04, v, v & BIT(8), 1, 1000))
			to |= BIT(n);
	}
	return to;
}

static void t6040_atc_rec_store(struct t6040_sleep *s, u32 to)
{
	u32 w[16] = { T6040_ATCF_MAGIC };
	int n, i;

	for (n = 0; n < 4; n++) {
		void __iomem *c = s->atc_cfg[n];

		if (!c)
			continue;
		w[1 + 2 * n] = readl(c + 0x04);
		w[2 + 2 * n] = (readl(c + 0x08) & 0xff) | (readl(c + 0x24) & 0xff) << 8 |
			       (readl(c + 0x28) & 0xff) << 16 | (readl(c + 0x2c) & 0xff) << 24;
	}
	w[9] = to;
	for (i = 0; i < 16; i++)
		t6040_pmu_cmd(s, SPMI_CMD_PMU(0xa240 + 4 * i, SPMI_OP_WRITE4), &w[i], NULL, 0);
}

static int t6040_atc_cfg_dump_set(const char *val, const struct kernel_param *kp)
{
	struct t6040_sleep *s = t6040_sleep;
	int n;

	if (!s)
		return -ENODEV;
	for (n = 0; n < 4; n++) {
		void __iomem *c = s->atc_cfg[n];

		if (c)
			dev_info(s->dev, "atc%d cfg: +0x00 %08x +0x04 %08x +0x08 %08x +0x24 %08x +0x28 %08x +0x2c %08x\n",
				 n, readl(c), readl(c + 0x04), readl(c + 0x08), readl(c + 0x24),
				 readl(c + 0x28), readl(c + 0x2c));
	}
	return 0;
}

static const struct kernel_param_ops t6040_atc_cfg_dump_ops = {
	.set = t6040_atc_cfg_dump_set,
};
module_param_cb(atc_cfg_dump, &t6040_atc_cfg_dump_ops, NULL, 0200);
MODULE_PARM_DESC(atc_cfg_dump, "Bring-up: write anything to log the 4 ATCPHY_CFG blocks now");

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

#define PS_AUTO_LEVEL		GENMASK(27, 24)
#define T6040_FAB_PMU		0xa240
#define T6040_FAB_MAGIC		0x53424146	/* "FABS" in PMU byte order */

static void t6040_fabrics_on(struct t6040_sleep *s)
{
	u32 rec[16] = { T6040_FAB_MAGIC }, atc_to = 0;
	int i;

	for (i = 0; i < ARRAY_SIZE(t6040_fabrics); i++) {
		void __iomem *b = t6040_fabrics[i].east ? s->snap_east : s->snap_soc;
		u32 off = t6040_fabrics[i].off, v;

		if (!b) {
			rec[3 + i] = 0xdeadbeef;
			continue;
		}
		v = readl(b + off);
		if (!(v & PS_AUTO_ENABLE) || FIELD_GET(PS_TARGET, v) != 0xf) {
			rec[1] |= BIT(i);
			if (t6040_ps_raise(b, off))
				rec[2] |= BIT(i);
		}
		v = readl(b + off);
		if ((READ_ONCE(fab_on) & BIT(1)) && FIELD_GET(PS_AUTO_LEVEL, v) != 0xf) {
			rec[1] |= BIT(16 + i);
			writel((v & ~(PS_AUTO_LEVEL | PS_FLAGS)) | FIELD_PREP(PS_AUTO_LEVEL, 0xf),
			       b + off);
			if (readl_poll_timeout_atomic(b + off, v, FIELD_GET(PS_ACTUAL, v) == 0xf, 1,
						      PS_OFF_TIMEOUT_US))
				rec[2] |= BIT(16 + i);
		}
		rec[3 + i] = readl(b + off);
	}
	if (READ_ONCE(fab_on) & BIT(2))
		t6040_atc_commons_on(s, &rec[2]);
	if (READ_ONCE(atc_s2r) & 0xf)
		atc_to = t6040_atc_s2r_entry(s, READ_ONCE(atc_s2r) & 0xf);
	if (READ_ONCE(fab_on) & BIT(3)) {
		for (i = 0; i < ARRAY_SIZE(t6040_fabrics); i++) {
			void __iomem *b = t6040_fabrics[i].east ? s->snap_east : s->snap_soc;
			u32 off = t6040_fabrics[i].off;

			if (!b || (off != 0x2d0 && off != 0x338 && off != 0x488 &&
				   !(t6040_fabrics[i].east && off == 0x128)))
				continue;
			rec[1] |= BIT(16 + i);
			if (t6040_ps_raise_start(b, off))
				rec[2] |= BIT(16 + i);
		}
	}
	for (i = 0; i < ARRAY_SIZE(t6040_fabrics); i++) {
		void __iomem *b = t6040_fabrics[i].east ? s->snap_east : s->snap_soc;

		rec[3 + i] = b ? readl(b + t6040_fabrics[i].off) : 0xdeadbeef;
	}
	if (READ_ONCE(atc_rec))
		t6040_atc_rec_store(s, atc_to);
	else
		for (i = 0; i < ARRAY_SIZE(rec); i++)
			t6040_pmu_cmd(s, SPMI_CMD_PMU(T6040_FAB_PMU + 4 * i, SPMI_OP_WRITE4),
				      &rec[i], NULL, 0);
	if (rec[2])
		t6040_mark_raw(s, 0x54d00000 | (rec[2] & 0xffff));
}

/*
 * The MCPS stalls in seq 25's ATC block of port 0 or port 1 at random. Each port n has a
 * block at 0x508320c00 + 0x80n (seq 23 writes +0x00..+0x14, seq 21 +0x1c/+0x40/+0x60, seq 25
 * writes +0x60 := 2 and polls +0x00 bit 30). aon_rec stores, right before the S2R timer (instead
 * of 'APCS'), 'AONB' + port 0/1 {+0x00, +0x1c, +0x40, +0x60} + port 2/3 {+0x00, +0x60} +
 * ATC0/1_PHYMXWRAP + ATC1_USB_AON at PMU 0xa200; only the offsets those programs use are
 * read. Writing aon_dump logs the same words now.
 */
static bool aon_rec;
module_param(aon_rec, bool, 0644);
MODULE_PARM_DESC(aon_rec, "Bring-up: record the ATC A2D hand-over blocks at PMU 0xa200 right before the S2R timer (replaces 'APCS')");

#define T6040_AON_MAGIC		0x424e4f41	/* "AONB" in PMU byte order */

static void t6040_aon_take(struct t6040_sleep *s, u32 *w)
{
	static const u16 off[12] = { 0x000, 0x01c, 0x040, 0x060, 0x080, 0x09c, 0x0c0, 0x0e0,
				     0x100, 0x160, 0x180, 0x1e0 };
	int i;

	w[0] = T6040_AON_MAGIC;
	for (i = 0; i < 12; i++)
		w[1 + i] = s->atc_aon ? readl(s->atc_aon + off[i]) : 0xdeadbeef;
	w[13] = s->mini_ps ? readl(s->mini_ps + 0x150) : 0xdeadbeef;
	w[14] = s->mini_ps ? readl(s->mini_ps + 0x158) : 0xdeadbeef;
	w[15] = s->mini_ps ? readl(s->mini_ps + 0x0d0) : 0xdeadbeef;
}

static void t6040_aon_store(struct t6040_sleep *s)
{
	u32 w[16];
	int i;

	t6040_aon_take(s, w);
	for (i = 0; i < 16; i++)
		t6040_pmu_cmd(s, SPMI_CMD_PMU(T6040_APCIE_DIAG_PMU + 4 * i, SPMI_OP_WRITE4), &w[i],
			      NULL, 0);
}

static int t6040_aon_dump_set(const char *val, const struct kernel_param *kp)
{
	u32 w[16];

	if (!t6040_sleep)
		return -ENODEV;
	t6040_aon_take(t6040_sleep, w);
	dev_info(t6040_sleep->dev,
		 "aon: p0 %08x %08x %08x %08x p1 %08x %08x %08x %08x p2 %08x %08x p3 %08x %08x mxwrap0 %08x mxwrap1 %08x usb_aon1 %08x\n",
		 w[1], w[2], w[3], w[4], w[5], w[6], w[7], w[8], w[9], w[10], w[11], w[12], w[13],
		 w[14], w[15]);
	return 0;
}

static const struct kernel_param_ops t6040_aon_dump_ops = {
	.set = t6040_aon_dump_set,
};
module_param_cb(aon_dump, &t6040_aon_dump_ops, NULL, 0200);
MODULE_PARM_DESC(aon_dump, "Bring-up: write anything to log the ATC A2D hand-over blocks now");

/*
 * A2D completes; the engine stalls in D2S (seq 28) polling the AMCC0 DCS pair's power
 * gate (0x50282c918 bit 30). On this revision the program never writes the DCS power words: the
 * DCS gate themselves after self-refresh, only in dynamic power-gate mode (DCS+0x20 bit 12, bit 8
 * enable). The platform keeps that mode on at runtime and re-applies it after each wake
 * (DCS dynamic power-gate mode 1: RMW +0x20 mask 0x1100 := 0x1100 on
 * all 16 DCS); Linux never sets it. Only the offsets the platform and the engine use are read.
 * dcs_dump logs them now; dcs_pg bit 0 applies the the platform mode now (on write) and again in
 * .begin; dcs_rec stores 'DCSR' + the DCS +0x20 low halves + the 4 AMCC handshake words + the
 * 8 pwrgate CFG0 top bytes + the DCS +0x500 bits [1:0] at PMU 0xa200 right before the timer.
 */
#define T6040_DCS_MAGIC		0x52534344	/* "DCSR" in PMU byte order */
static const u16 t6040_pg_main_off[4] = { 0x918, 0x998, 0xa18, 0xa98 };
static const u16 t6040_pg_east_off[4] = { 0xa00, 0xa80, 0xb00, 0xb80 };

static bool dcs_rec;
module_param(dcs_rec, bool, 0644);
MODULE_PARM_DESC(dcs_rec, "Bring-up: record the DCS power-gate state at PMU 0xa200 right before the S2R timer (replaces 'AONB'/'APCS')");

/*
 * D2S still stalls with dynamic power gating on and the AOP/PMP/MTP ruled out.
 * #1208's T8140 commit stops each memory channel right before its gates at
 * about -18 ms (t8140_sleep_mcc_stop): at DCS +0x580 ("mcc0..2" = 0x2_2040_0580 +
 * ch * 0x20_0000, the DCS slices' spacing) the enables word +0x4 cleared one bit at a time,
 * then lanes 0..3 strobed and cleared on +0x8 (mask word +0xc), two lanes per channel per
 * pass. The T6040 commit never did it (the measurement behind "the platform writes none" did not
 * cover these pages). dcs_mcc_stop bit 0 runs that sequence on all 16 DCS right before the S2R
 * timer; 'DCSR' words 10/11 then hold DCS 0.0 +0x584/+0x588 after it.
 */
static unsigned int dcs_mcc_stop;
module_param(dcs_mcc_stop, uint, 0644);
MODULE_PARM_DESC(dcs_mcc_stop, "Bring-up: bit 0 = T8140 mcc_stop (DCS +0x584 enables, +0x588 lanes) on all 16 DCS right before the S2R timer");

static void t6040_dcs_mcc_stop(struct t6040_sleep *s)
{
	int i, pass, n;
	u32 val;

	for (i = 0; i < 16; i++) {
		void __iomem *m = s->dcs[i] ? s->dcs[i] + 0x580 : NULL;

		if (!m)
			continue;
		val = readl(m + 0x4);
		for (n = 0; val && n < 32; n++) {
			val &= val - 1;
			writel(val, m + 0x4);
			val = readl(m + 0x4);
		}
	}
	for (pass = 0; pass < 2; pass++) {
		for (i = 0; i < 16; i++) {
			void __iomem *m = s->dcs[i] ? s->dcs[i] + 0x580 : NULL;

			if (!m)
				continue;
			for (n = 2 * pass; n < 2 * pass + 2; n++) {
				writel(readl(m + 0xc), m + 0xc);
				writel(readl(m + 0x8) | BIT(16 + 4 * n), m + 0x8);
				writel(readl(m + 0x8) & ~BIT(4 * n), m + 0x8);
			}
		}
	}
}

/*
 * On this revision the D2S program does not write the DCS power-state words; it expects
 * them in automatic mode (bit 28) with PS_MIN 0, so each DCS domain drops to 0 after
 * self-refresh and its pwrgate block reports bit 30. The platform sets that at boot
 * (0x0f0000ff -> 0x1f0000ff) on all 16 DCS PS words; the Linux
 * DT leaves the ps_dcs_* domains disabled, so they stay at target f with AUTO off. dcs_dump
 * logs them and the 8 AMCC PS words; dcs_ps_auto bit 0 sets AUTO now (the platform order: PS_MIN
 * cleared, target f, ACTUAL f polled, then AUTO) and again in .begin.
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
static const u16 t6040_amcc_ps[8] = { 0x180, 0x198, 0x1a8, 0x1b8, 0x1c8, 0x1d8, 0x1e8, 0x1f8 };

static void __iomem *t6040_dcs_ps_reg(struct t6040_sleep *s, int i)
{
	void __iomem *b = t6040_dcs_ps[i].east ? s->snap_east : s->snap_soc;

	return b ? b + t6040_dcs_ps[i].off : NULL;
}

static unsigned int dcs_ps_auto = 1;

/* Returns a mask of the DCS whose ACTUAL did not reach f before AUTO was set. */
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

static int t6040_dcs_ps_auto_set(const char *val, const struct kernel_param *kp)
{
	int ret = param_set_uint(val, kp);

	if (!ret && (dcs_ps_auto & BIT(0)) && t6040_sleep)
		dev_info(t6040_sleep->dev, "DCS PS automatic mode on (the platform boot parity), timeouts %#x\n",
			 t6040_dcs_ps_auto_on(t6040_sleep));
	return ret;
}

static const struct kernel_param_ops t6040_dcs_ps_auto_ops = {
	.set = t6040_dcs_ps_auto_set,
	.get = param_get_uint,
};
module_param_cb(dcs_ps_auto, &t6040_dcs_ps_auto_ops, &dcs_ps_auto, 0644);
MODULE_PARM_DESC(dcs_ps_auto, "Bring-up: bit 0 = DCS power-state words to automatic mode (PS_MIN 0) now and in .begin, as the platform at boot");

static void t6040_dcs_take(struct t6040_sleep *s, u32 *w)
{
	int i;

	memset(w, 0, 16 * sizeof(u32));
	w[0] = T6040_DCS_MAGIC;
	for (i = 0; i < 16; i++) {
		u32 v = s->dcs[i] ? readl(s->dcs[i] + 0x20) : 0xdead;
		u32 b = s->dcs[i] ? readl(s->dcs[i] + 0x500) : 0;

		w[1 + i / 2] |= (v & 0xffff) << (16 * (i % 2));
		w[15] |= (b & 3) << (2 * i);
	}
	for (i = 0; i < 4; i++) {
		if (s->amcc_sem[i])
			w[9] |= (readl(s->amcc_sem[i] + 0x28) & 1) << i;
		if (s->pg_main)
			w[13] |= (readl(s->pg_main + t6040_pg_main_off[i]) >> 24) << (8 * i);
		if (s->pg_east)
			w[14] |= (readl(s->pg_east + t6040_pg_east_off[i]) >> 24) << (8 * i);
	}
	/* The AOP island's DRAM request (0x5108b8100 bit 0) and MasterMini+0x14. */
	if (s->aop_req)
		w[9] |= (readl(s->aop_req + 0x100) & 0xff) << 8;
	if (s->mastermini)
		w[9] |= (readl(s->mastermini + 0x14) & 0xffff) << 16;
	/* DCS_00 and DCS_04 PS words, AMCC0 PS word (instead of the AOP ASC). */
	w[10] = t6040_dcs_ps_reg(s, 0) ? readl(t6040_dcs_ps_reg(s, 0)) : 0xdeadbeef;
	w[11] = t6040_dcs_ps_reg(s, 4) ? readl(t6040_dcs_ps_reg(s, 4)) : 0xdeadbeef;
	w[12] = s->snap_soc ? readl(s->snap_soc + t6040_amcc_ps[0]) : 0xdeadbeef;
}

/*
 * With dynamic power gating on (0x1100 on all 16 DCS) and the DRAM MCU handshake done
 * for all 4 AMCC, the DCS still do not gate: something keeps using DRAM. The AOP runs through
 * A2D/D2S and has a DRAM request bit (0x5108b8100 bit 0). aop_halt bit 0 clears the AOP ASC RUN
 * bit right after the 'DCSR' record, just before the S2R timer (diagnostic: the AOP island stays
 * on; a wake then has no AOP). Record word 12 = CPU_CONTROL after the halt.
 */
static unsigned int aop_halt;
module_param(aop_halt, uint, 0644);
MODULE_PARM_DESC(aop_halt, "Bring-up: bit 0 = stop the AOP CPU right before the S2R timer (diagnostic)");

static void t6040_dcs_store(struct t6040_sleep *s)
{
	u32 w[16];
	int i;

	t6040_dcs_take(s, w);
	if ((READ_ONCE(dcs_mcc_stop) & BIT(0)) && s->dcs[0]) {
		t6040_dcs_mcc_stop(s);
		w[10] = readl(s->dcs[0] + 0x584);
		w[11] = readl(s->dcs[0] + 0x588);
	}
	if ((READ_ONCE(aop_halt) & BIT(0)) && s->aop_asc) {
		writel(readl(s->aop_asc + ASC_CPU_CONTROL) & ~ASC_CPU_CONTROL_RUN,
		       s->aop_asc + ASC_CPU_CONTROL);
		udelay(10);
		w[12] = readl(s->aop_asc + ASC_CPU_CONTROL) | (readl(s->aop_req + 0x100) & 0xff) << 24;
	}
	for (i = 0; i < 16; i++)
		t6040_pmu_cmd(s, SPMI_CMD_PMU(T6040_APCIE_DIAG_PMU + 4 * i, SPMI_OP_WRITE4), &w[i],
			      NULL, 0);
}

static int t6040_dcs_dump_set(const char *val, const struct kernel_param *kp)
{
	struct t6040_sleep *s = t6040_sleep;
	int i;

	if (!s)
		return -ENODEV;
	for (i = 0; i < 16; i++)
		if (s->dcs[i])
			dev_info(s->dev, "dcs %d.%d: +0x20 %08x +0x500 %08x +0x1d48 %08x +0x580 %08x %08x %08x %08x\n",
				 i / 4, i % 4, readl(s->dcs[i] + 0x20), readl(s->dcs[i] + 0x500),
				 readl(s->dcs[i] + 0x1d48), readl(s->dcs[i] + 0x580),
				 readl(s->dcs[i] + 0x584), readl(s->dcs[i] + 0x588),
				 readl(s->dcs[i] + 0x58c));
	for (i = 0; i < 4; i++)
		dev_info(s->dev, "amcc %d: +0x1c0028 %08x pg main %08x east %08x\n", i,
			 s->amcc_sem[i] ? readl(s->amcc_sem[i] + 0x28) : 0xdeadbeef,
			 s->pg_main ? readl(s->pg_main + t6040_pg_main_off[i]) : 0xdeadbeef,
			 s->pg_east ? readl(s->pg_east + t6040_pg_east_off[i]) : 0xdeadbeef);
	for (i = 0; i < 16; i++)
		dev_info(s->dev, "dcs ps %d: %08x\n", i,
			 t6040_dcs_ps_reg(s, i) ? readl(t6040_dcs_ps_reg(s, i)) : 0xdeadbeef);
	for (i = 0; i < 8; i++)
		dev_info(s->dev, "amcc ps %d: %08x\n", i,
			 s->snap_soc ? readl(s->snap_soc + t6040_amcc_ps[i]) : 0xdeadbeef);
	dev_info(s->dev, "aop: req %08x mastermini+0x14 %08x asc ctl %08x status %08x\n",
		 s->aop_req ? readl(s->aop_req + 0x100) : 0xdeadbeef,
		 s->mastermini ? readl(s->mastermini + 0x14) : 0xdeadbeef,
		 s->aop_asc ? readl(s->aop_asc + ASC_CPU_CONTROL) : 0xdeadbeef,
		 s->aop_asc ? readl(s->aop_asc + ASC_CPU_STATUS) : 0xdeadbeef);
	return 0;
}

static const struct kernel_param_ops t6040_dcs_dump_ops = {
	.set = t6040_dcs_dump_set,
};
module_param_cb(dcs_dump, &t6040_dcs_dump_ops, NULL, 0200);
MODULE_PARM_DESC(dcs_dump, "Bring-up: write anything to log the DCS power-gate state now");

static unsigned int dcs_pg = 1;

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

static int t6040_dcs_pg_set(const char *val, const struct kernel_param *kp)
{
	int ret = param_set_uint(val, kp);

	if (!ret && (dcs_pg & BIT(0)) && t6040_sleep) {
		t6040_dcs_dyn_pg(t6040_sleep);
		dev_info(t6040_sleep->dev, "DCS dynamic power-gate mode on (the platform parity)\n");
	}
	return ret;
}

static const struct kernel_param_ops t6040_dcs_pg_ops = {
	.set = t6040_dcs_pg_set,
	.get = param_get_uint,
};
module_param_cb(dcs_pg, &t6040_dcs_pg_ops, &dcs_pg, 0644);
MODULE_PARM_DESC(dcs_pg, "Bring-up: bit 0 = DCS dynamic power-gate mode on (+0x20 |= 0x1100 on all 16 DCS) now and in .begin");

/* Words 8-15 right before the S2R timer, then the whole record to PMU SRAM 0xa200. */
static void t6040_apcie_diag_store(struct t6040_sleep *s)
{
	static const u16 w[8] = { PS_APCIE_PHY_SW, PS_APCIE_SYS_ST0, PS_APCIE_SYS_ST1,
				  PS_APCIE_ST0, PS_APCIE_ST1, PS_ANS, PS_APCIE_SYS_GP,
				  PS_APCIE_GP };
	int i;

	s->apcie_diag[0] = T6040_APCIE_DIAG_MAGIC;
	for (i = 0; i < 8; i++)
		s->apcie_diag[8 + i] = s->soc_east ? readl(s->soc_east + w[i]) : 0xdeadbeef;
	for (i = 0; i < ARRAY_SIZE(s->apcie_diag); i++)
		t6040_pmu_cmd(s, SPMI_CMD_PMU(T6040_APCIE_DIAG_PMU + 4 * i, SPMI_OP_WRITE4),
			      &s->apcie_diag[i], NULL, 0);
}

static void t6040_sleep_lower_apcie_st(struct t6040_sleep *s)
{
	struct t6040_apcie_st *a = &s->apcie_st;
	void __iomem *ps = s->soc_east;
	unsigned int mac = READ_ONCE(apcie_seq);

	if (!a->valid || !ps)
		return;

	/* The PHY's registers need a lane domain on; quiet it first (shared). */
	if (a->phy && t6040_ps_actual_on(ps, PS_APCIE_SYS_ST0)) {
		if (mac & BIT(0))
			t6040_apcie_phy_off(s);
		else
			t6040_apcie_phy_off(a);
	}
	if (mac & BIT(1))
		t6040_apcie_phy_sw_gate(s);
	if (a->clkgen && t6040_ps_actual_on(ps, PS_APCIE_SYS_ST0)) {
		if (mac & BIT(1))
			t6040_apcie_pll_off(s);
		else
			t6040_apcie_clk_off(a);
	}

	/* Per-lane: both sys_st then both st, deepest first. */
	if (READ_ONCE(apcie_st_ps)) {
		t6040_ps_lower(ps, PS_APCIE_SYS_ST1);
		t6040_ps_lower(ps, PS_APCIE_SYS_ST0);
		t6040_ps_lower(ps, PS_APCIE_ST1);
		t6040_ps_lower(ps, PS_APCIE_ST0);
	}

	/* AXI enables off (shared), then ANS last. */
	if (a->axi) {
		writel(readl(a->axi + APCIE_AXI_EN0) & ~BIT(0), a->axi + APCIE_AXI_EN0);
		writel(readl(a->axi + APCIE_AXI_EN1) & ~BIT(0), a->axi + APCIE_AXI_EN1);
	}
	if (READ_ONCE(apcie_st_ps))
		t6040_ps_lower(ps, PS_ANS);
	/* The caller polls PS_ANS off, as #1208 does. */
}

/*
 * RESUME: raise ANS's storage link from power-off, the two-lane adaptation of
 * #1208's t8140_sleep_raise_apcie(). Runs atomic on the waking core in
 * t6040_sleep_return(), AFTER t6040_axi2af_rearm() (the ANS/apcie_sys fabric
 * bridges must be re-armed before link traffic) and BEFORE control returns to
 * the PM core, so nvme's resume_early sets the ANS run bit onto a live link.
 * The two lanes come up together; the PLL/clkgen/PHY tables run once (shared).
 *
 * Not known (first S2R on hardware): whether ANS trains on one lane or needs both, and whether
 * the two apcie_sys_st domains need their own axi2af bridge re-arm (they are NOT
 * in the 15-domain axi2af workaround table, so none is done here).
 */
/*
 * On the J616s the resume took an SError between marks 0x37 and 0x38, in here: the AXI enables
 * (+0x104/+0x108 of 0x4_1600_0000) are the axi2af bridge of APCIE_SYS_GP, which the commit
 * (apcie_seq) left off together with GP, and nothing raised it. The root complex enable:
 * gates idx 0..6 = ANS, GP, SYS_GP, ST0, SYS_ST0, ST1, SYS_ST1 in that
 * order, each to ACTUAL 0xf, setting the axi2af bit0 after ANS and SYS_GP; the PLLs; then gate
 * idx 7 = APCIE_PHY_SW (which the commit gated) before the PHY, and "refpll_clk_good" (PHY
 * common +0 bit31) before the PHY init. apcie_wake bit0: that domain order and both bridges;
 * bit1: APCIE_PHY_SW on and the refpll wait. Marks 0x50..0x55 follow the steps.
 */
static unsigned int apcie_wake = 7;
module_param(apcie_wake, uint, 0644);
MODULE_PARM_DESC(apcie_wake,
		 "Bring-up wake: bit0 the platform gate order ANS,GP,SYS_GP,ST0,SYS_ST0,ST1,SYS_ST1 + axi2af bits; bit1 PHY_SW on + refpll wait; bit2 SYS_GP without AUTO");

static void t6040_bridge_rearm(void __iomem *bridge)
{
	if (!bridge)
		return;
	writel(readl(bridge + AXI2AF_BRIDGE_104) | BIT(0), bridge + AXI2AF_BRIDGE_104);
	writel(readl(bridge + AXI2AF_BRIDGE_108) | BIT(0), bridge + AXI2AF_BRIDGE_108);
}

static int t6040_sleep_raise_apcie_st(struct t6040_sleep *s)
{
	struct t6040_apcie_st *a = &s->apcie_st;
	void __iomem *ps = s->soc_east;
	void __iomem *ctrl, *run;
	unsigned int wake = READ_ONCE(apcie_wake);
	u32 val;
	int ret;

	if (!a->valid || !ps)
		return 0;
	ctrl = a->phy + APCIE_PHY_CTRL;
	run = a->phy + APCIE_PHY_RUN;

	if (wake & BIT(0)) {
		ret = t6040_ps_raise(ps, PS_ANS);
		if (!ret)
			t6040_bridge_rearm(s->axi2af_bridge[AXI2AF_ANS]);
		ret |= t6040_ps_raise(ps, PS_APCIE_GP);
		/*
		 * The 'APCW' record: after the raise SYS_GP read 0x1000030f (AUTO on, auto state 0,
		 * ACTUAL 0): AUTO gated it again and the ANS apcie DART (0x4_1000_0000) behind it
		 * took the SError in its noirq resume. bit2 leaves SYS_GP in manual target 0xf,
		 * as GP/ST0/ST1 read (0x2ff).
		 */
		if (!ret)
			ret = (wake & BIT(2)) ? t6040_ps_raise_start(ps, PS_APCIE_SYS_GP) :
						t6040_ps_raise(ps, PS_APCIE_SYS_GP);
		if (!ret) {
			writel(readl(a->axi + APCIE_AXI_EN0) | BIT(0), a->axi + APCIE_AXI_EN0);
			writel(readl(a->axi + APCIE_AXI_EN1) | BIT(0), a->axi + APCIE_AXI_EN1);
		}
		ret |= t6040_ps_raise(ps, PS_APCIE_ST0);
		ret |= t6040_ps_raise(ps, PS_APCIE_SYS_ST0);
		ret |= t6040_ps_raise(ps, PS_APCIE_ST1);
		ret |= t6040_ps_raise(ps, PS_APCIE_SYS_ST1);
		t6040_ret_mark(s, 0x50);
		s->apcie_wake_step = 1;
		if (ret)
			return ret;
	} else {
	/* Domains up: ANS first, then both lanes (st then sys_st), AXI enables. */
	ret  = t6040_ps_raise(ps, PS_ANS);
	ret |= t6040_ps_raise(ps, PS_APCIE_ST0);
	ret |= t6040_ps_raise(ps, PS_APCIE_ST1);
	ret |= t6040_ps_raise_start(ps, PS_APCIE_SYS_ST0);
	ret |= t6040_ps_raise_start(ps, PS_APCIE_SYS_ST1);
	if (!ret) {
		writel(readl(a->axi + APCIE_AXI_EN0) | BIT(0), a->axi + APCIE_AXI_EN0);
		writel(readl(a->axi + APCIE_AXI_EN1) | BIT(0), a->axi + APCIE_AXI_EN1);
	}
	t6040_ps_raise_finish(ps, PS_APCIE_SYS_ST0);
	t6040_ps_raise_finish(ps, PS_APCIE_SYS_ST1);
	if (ret)
		return ret;
	}

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
	if (wake)
		t6040_ret_mark(s, 0x51);
	s->apcie_wake_step = 2;
	if (ret)
		return ret;

	if (wake & BIT(1)) {
		ret = t6040_ps_raise(ps, PS_APCIE_PHY_SW);
		s->apcie_wake_step = 3;
		if (ret)
			return ret;
	}

	/* Shared PHY bring-up + PLL/lane tables (as T8140's t8140_apcie_phy_ip_apply). */
	apple_tunable_apply(a->phy, a->phy_tunable);
	if (wake & BIT(1)) {
		ret = readl_poll_timeout_atomic(a->phy + APCIE_PHY_COMMON, val, val & BIT(31), 1,
						APCIE_POLL_US);
		t6040_ret_mark(s, 0x52);
		s->apcie_wake_step = 4;
		if (ret)
			return ret;
	}
	writel(readl(ctrl) | APCIE_PHY_CTRL_CLK0REQ, ctrl);
	ret = readl_poll_timeout_atomic(ctrl, val, val & APCIE_PHY_CTRL_CLK0ACK, 1, APCIE_POLL_US);
	if (ret)
		return ret;
	writel(readl(ctrl) | APCIE_PHY_CTRL_CLK1REQ, ctrl);
	ret = readl_poll_timeout_atomic(ctrl, val, val & APCIE_PHY_CTRL_CLK1ACK, 1, APCIE_POLL_US);
	if (ret)
		return ret;
	writel(readl(ctrl) & ~APCIE_PHY_CTRL_RESET, ctrl);
	if (wake)
		t6040_ret_mark(s, 0x53);
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
	if (wake)
		t6040_ret_mark(s, 0x54);
	s->apcie_wake_step = 6;
	writel(readl(a->phy + APCIE_PHY_COMMON) | BIT(0), a->phy + APCIE_PHY_COMMON);
	writel(readl(ctrl) | APCIE_PHY_CTRL_UP, ctrl);
	if (wake)
		t6040_ret_mark(s, 0x55);
	s->apcie_wake_step = 7;

	/* RC ready handshake (as T8140's). */
	writel(0, a->rc + APCIE_RC_ZERO);
	writel(0x140, a->rc + APCIE_RC_READY_CFG);
	writel(1, a->rc + APCIE_RC_READY_START);
	return readl_poll_timeout_atomic(a->rc + APCIE_RC_READY_STAT, val, val & BIT(0), 1,
					 APCIE_POLL_US);
}

/*
 * The real commit (t6040_sleep_commit, reached through cpu_suspend). COMPILED
 * BUT UNREACHABLE: t6040_sleep_enter calls it only when no blocker remains and
 * allow_entry is set, and the missing-data blockers (t6040_sleep_unknowns)
 * never clear in this kernel. The SoC power-down here is the T6040 entry as
 * measured on the J616s, which is SHORT: unlike #1208's T8140 commit there
 * is NO dcs_down / mcc_stop / pwrgate / PTD / clk-mux-per-lane / CLLT / dvfm /
 * AMCC / ps_seq list (the platform writes none of them). The
 * order is the, as measured: WD0 off, PMU boot-stage 0x50
 * + NOTIFY over spmi0/spmi-a0, the AON 5 s net, the three clusters off, the
 * clk-mux park, the axi2af read-check, the AURAS2R1 mailbox, the MasterMini S2R
 * timer (0x508904000 := 1), the AIC masked, the parked CPUs released, the
 * optional PMU "go hib" replay, and the deep WFI. All of it is measured except the
 * clk-mux park (not observed) and the go-hib replay (measured bytes, necessity unproven).
 * A hang here ends at WD2.
 */
/*
 * The first thing Linux runs after a wake, MMU off and no stack (#1314's T8132 stub). The
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

static bool resume_stub = true;
module_param(resume_stub, bool, 0644);
MODULE_PARM_DESC(resume_stub,
		 "Wake into a stub that restarts the AON WD2 for 60 s before cpu_resume (default on)");

static u32 t6040_axi2af_on_mask(struct t6040_sleep *s);

static bool lower_atc = true;
module_param(lower_atc, bool, 0644);
MODULE_PARM_DESC(lower_atc, "Lower the four USB-C (ATC) complexes in the commit off");

/*
 * The four ATC complexes' power words in SOC (soc_ps, 0x502800000), deepest first: cio_usb,
 * cio_pcie, cio, pcie, common (ADT: ATCn_CIO_USB .. ATCn_COMMON). The platform has them off at its
 * quiesce gate; their drivers don't lower atcN_common.
 *
 * Their common_dp, usb, phymxwrap and usb_aon words are MiniPMGR words (0x508300000 + 0xa0..,
 * ADT ATCn_COMMON_DP/USB/PHYMXWRAP/USB_AON), not SOC ones: applied to soc_ps,
 * those offsets lower SOC AIC (0x118), PMC (0x120), PMS_FPWM0-4 and
 * FAB0_SOC (0x168) right before the S2R request; the dead PMC then blocks the A2D engine's
 * PMC writes (an AP read of the PMC hangs from that step on).
 */
static const u16 t6040_atc_words[4][5] = {
	{ 0x3b0, 0x3a8, 0x398, 0x3a0, 0x390 },
	{ 0x3d8, 0x3d0, 0x3c0, 0x3c8, 0x3b8 },
	{ 0x400, 0x3f8, 0x3e8, 0x3f0, 0x3e0 },
	{ 0x438, 0x430, 0x420, 0x428, 0x408 },
};

/* As the platform lowers a domain (T8140 measured): AUTO_ENABLE off first, then target 0, poll. */
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

/* Marks 0x57PKSOOO: ATC P, word K, S 0 written / 8 off / 4 still on, OOO its offset. */
static void t6040_sleep_lower_atc(struct t6040_sleep *s)
{
	int p, k;

	if (!s->soc_ps)
		return;
	for (p = 0; p < ARRAY_SIZE(t6040_atc_words); p++)
		for (k = 0; k < ARRAY_SIZE(t6040_atc_words[0]); k++) {
			u32 off = t6040_atc_words[p][k];
			void __iomem *reg = s->soc_ps + off;
			u32 val = readl(reg);
			u32 m = 0x57000000 | p << 20 | k << 16 | off;

			if (!FIELD_GET(PS_TARGET, val) && !FIELD_GET(PS_ACTUAL, val))
				continue;
			t6040_mark_raw(s, m);
			t6040_mark_raw(s, m | (t6040_ps_lower_word(reg) ? 0x4000 : 0x8000));
		}
}

static bool lower_apcie_gp = true;
module_param(lower_apcie_gp, bool, 0644);
MODULE_PARM_DESC(lower_apcie_gp, "Stop the GP PCIe ports and lower apcie_sys_gp in the commit (T8140's radio port stop)");

/*
 * T8140's radio port stop, on the same t602x port block: the link into L2, the endpoint's
 * PERST# asserted, the PHY's clocks off. The endpoint keeps its power and comes back by its
 * host after the sleep. Marks 0x541P00RR: port P, RR 1 started, 2 done.
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
	t6040_mark_raw(s, 0x54100001 | p << 16);
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
	/* What this bit does isn't known. The platform sets it around PERST# (T8140). */
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
	t6040_mark_raw(s, 0x54100002 | p << 16);
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
 * hung the bus. T8140's order: SYS_ST, ST, SYS_GP, GP. Marks 0x543x/0x544x.
 */
static void t6040_sleep_lower_apcie_gp(struct t6040_sleep *s)
{
	void __iomem *sys, *gp, *bridge = s->axi2af_bridge[AXI2AF_APCIE_SYS_GP];
	u32 val;
	int ret;

	if (!s->soc_east)
		return;
	sys = s->soc_east + PS_APCIE_SYS_GP;
	gp = s->soc_east + PS_APCIE_GP;
	if (!FIELD_GET(PS_ACTUAL, readl(sys)))
		return;

	t6040_mark_raw(s, 0x54300001);
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
	ret = readl_poll_timeout_atomic(sys, val, !FIELD_GET(PS_ACTUAL, val), 1, PS_OFF_TIMEOUT_US);
	t6040_mark_raw(s, 0x54300002 | (ret ? 0x80 : 0));

	t6040_mark_raw(s, 0x54400001);
	if (READ_ONCE(apcie_seq) & BIT(3)) {
		writel(0x244, gp);
		ret = readl_poll_timeout_atomic(gp, val, FIELD_GET(PS_ACTUAL, val) == 4, 1,
						PS_OFF_TIMEOUT_US);
		s->apcie_diag[1] |= BIT(5) | (ret ? BIT(18) : 0);
	} else {
		ret = t6040_ps_lower_word(gp);
	}
	t6040_mark_raw(s, 0x54400002 | (ret ? 0x80 : 0));
}

static int t6040_sleep_commit(unsigned long unused)
{
	struct t6040_sleep *s = t6040_sleep;

	t6040_mark(s, 0x20);
	s->siq_cfg = read_sysreg_s(SYS_APL_SIQ_CFG_EL1);
	spin_table_stop_parked();
	if (READ_ONCE(release_early))
		spin_table_release_stopped();
	t6040_mark(s, 0x21);
	t6040_pmc_cp(s, 4);
	if (READ_ONCE(lower_atc))
		t6040_sleep_lower_atc(s);
	t6040_pmc_cp(s, 11);
	if (READ_ONCE(lower_apcie_gp))
		t6040_sleep_stop_gp_ports(s);
	t6040_pmc_cp(s, 12);

	/*
	 * lower_io: ANS's two ST lanes + shared PHY down, once ANS has been
	 * stopped by its driver. The platform does this ~40 ms before ANS idle;
	 * here it follows every refusal, so a refused sleep finds the link up.
	 * No-op unless the apple,apcie windows are present (apcie_st.valid).
	 */
	memset(s->apcie_diag, 0, sizeof(s->apcie_diag));
	s->apcie_diag[1] = (s->pcie_ans_stopped ? BIT(3) : 0) | (s->apcie_st.valid ? BIT(4) : 0);
	if (READ_ONCE(apcie_st1_on) && s->soc_east &&
	    t6040_ps_raise_start(s->soc_east, PS_APCIE_SYS_ST1))
		t6040_mark_raw(s, 0x54c00001);
	if (s->pcie_ans_stopped) {
		t6040_sleep_lower_apcie_st(s);
		t6040_pmc_cp(s, 13);
		if (s->soc_east && READ_ONCE(apcie_st_ps))
			s->ans_off_ret = t6040_ps_wait_off(s->soc_east, PS_ANS);
	}
	t6040_pmc_cp(s, 14);
	if (READ_ONCE(lower_apcie_gp))
		t6040_sleep_lower_apcie_gp(s);

	/*
	 * What the quiesce gate would panic on, after everything here has lowered what it
	 * lowers: 0x58 then the mask of the axi2af domains still on. The S2R timer then resets
	 * the SoC without a sleep, so the mark is kept: the wake alarm is not put back.
	 */
	s->commit_on_mask = t6040_axi2af_on_mask(s);
	t6040_pmc_cp(s, 5);

	t6040_mark(s, 0x22);
	/* WD0 off, then the AON 5 s reconfig net (as the platform arms it for a reconfig). */
	if (s->wdt)
		writel(0, s->wdt + WDT_WD0_CTL);
	t6040_sleep_set_wd2(s, clamp(READ_ONCE(sleep_wd_secs), 1U, WDT_MAX_SECS));
	/* And the SMC's AP watchdog deadline off, as the platform right after its WD2 (measured). */
	if (s->smc_wdt_deadline)
		writel(0, s->smc_wdt_deadline);

	t6040_mark(s, 0x23);
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
	s->quiesce_to = 0;
	WRITE_ONCE(s->timer_written, false);
	if (READ_ONCE(release_early)) {
		t6040_mark(s, 0x2c);
		s->quiesce_to |= min(t6040_secondaries_off(s), 15U) << 20;
	}
	if (READ_ONCE(pmgr_quiesce)) {
		t6040_mark(s, 0x2b);
		t6040_pmgr_quiesce(s);
	}
	t6040_pmc_cp(s, 6);
	if (READ_ONCE(agx_gate))
		t6040_mark(s, 0x2a0 | t6040_agx_gate(s));
	s->cpm_to = 0;
	if (READ_ONCE(cpm_iboot)) {
		t6040_mark(s, 0x2e);
		s->cpm_to = t6040_cpm_iboot(s, READ_ONCE(cpm_iboot));
	}
	t6040_pmc_cp(s, 7);
	t6040_mark(s, 0x24);
	t6040_clusters_off(s);
	t6040_pmc_cp(s, 8);

	/* Read-only: the axi2af panic gate the platform checks before the engine goes. */
	t6040_axi2af_readcheck(s);
	t6040_mark(s, 0x350);
	t6040_clkgen_park(s);
	t6040_mark(s, 0x351);
	t6040_pmc_cp(s, 9);
	if (READ_ONCE(keep_alive_off))
		s->ka_on_before = t6040_keep_alive(s, false, 0);
	t6040_pmc_cp(s, 10);
	t6040_mark(s, 0x25);

	/* Arm aura's AURAS2R1 mailbox with this boot's resume entry. */
	if (s->mbox) {
		WRITE_ONCE(s->mbox->resume_pa,
			   READ_ONCE(resume_stub) && s->wdt_stub_ok ?
			   __pa_symbol(t6040_resume_stub) : __pa_symbol(cpu_resume));
		dcache_clean_poc((unsigned long)s->mbox, (unsigned long)(s->mbox + 1));
		dsb(sy);
	}

	if (s->ptd && READ_ONCE(ptd_withdraw))
		writeq(readq(s->ptd + PTD_REQ_READ) & ~(u64)READ_ONCE(ptd_withdraw),
		       s->ptd + PTD_REQ_WRITE);

	/*
	 * Discriminator: hold everything right before the timer. A reset that lands in the
	 * hold keeps mark 0x3e0 (its source started before the timer: the SMC/PMU after
	 * 'slpa', or a net); one that moves later by the hold follows the timer (the engine).
	 */
	if (READ_ONCE(timer_delay_ms)) {
		t6040_mark(s, 0x3e0);
		mdelay(min(READ_ONCE(timer_delay_ms), 10000U));
	}

	/* iBoot's order: the AOP + audio island, then the MTP island. */
	s->aop_left = 0;
	if (READ_ONCE(aop_island_off) & BIT(0))
		s->aop_left = t6040_aop_island_off(s);
	s->mtp_left = 0;
	if (READ_ONCE(mtp_island_off) & BIT(0))
		s->mtp_left = t6040_mtp_island_off(s, NULL);

	if (READ_ONCE(fab_on))
		t6040_fabrics_on(s);
	if (READ_ONCE(ps_snap))
		t6040_ps_snap_store(s);
	if (READ_ONCE(dcs_rec))
		t6040_dcs_store(s);
	else if (READ_ONCE(aon_rec))
		t6040_aon_store(s);
	else if (READ_ONCE(apcie_seq) & BIT(2))
		t6040_apcie_diag_store(s);
	if (READ_ONCE(pmc_probe) & BIT(1)) {
		u32 pv[ARRAY_SIZE(t6040_pmc_probe_pa)];

		t6040_pmc_probe_read(s, pv);
		t6040_pmc_probe_store(s, pv);
	}

	/* Last before the S2R timer: what is still on, re-read now, kept through the reset. */
	s->commit_on_mask |= t6040_axi2af_on_mask(s);
	if (s->aop_left) {
		t6040_mark_raw(s, 0x55000000 | s->aop_left);
	} else if (s->mtp_left) {
		t6040_mark_raw(s, 0x57000000 | s->mtp_left);
	} else if (s->cpm_to) {
		t6040_mark_raw(s, 0x56000000 | s->cpm_to);
	} else if (s->commit_on_mask) {
		t6040_mark_raw(s, 0x58000000 | s->commit_on_mask);
	} else if (s->quiesce_to & (QTO_CLUSTER(3) | QTO_CLUSTER(4))) {
		/*
		 * DISP_INT (+0xc) / DISP_EXT (+0x10) kept BIT4: 0x51, then +0xc and +0x10 as
		 * read after the wait, then +0xc's low 6 bits and +0x10's [5:4] from before.
		 */
		t6040_mark_raw(s, 0x51000000 | (s->cluster_after[3] & 0xff) << 16 |
				  (s->cluster_after[4] & 0xff) << 8 |
				  (s->cluster_saved[3] & 0x3f) |
				  ((s->cluster_saved[4] >> 4) & 3) << 6);
	} else if (s->quiesce_to) {
		/* The commit ran, but a quiesce poll timed out: which ones, kept. */
		t6040_mark_raw(s, 0x5d000000 | s->quiesce_to);
	} else {
		/* One SPMI write: the alarm back + 1 s reads as "the commit ran". */
		t6040_mark_restore(s);
	}
	/* The commit: the MasterMini S2R timer arms the SoC sleep engine. */
	if (s->mastermini)
		writel(1, s->mastermini + MASTERMINI_S2R_TIMER);
	dsb(sy);
	WRITE_ONCE(s->timer_written, true);
	dsb(ish);

	/*
	 * Bring-up: an SPMI mark right after the timer never lands. Re-arm the AON
	 * WD2 here, no SPMI: a reset at timer + this many ms means this core still runs
	 * after the timer; one at the usual timer + ~4.5 s means it does not (measured: it runs).
	 * Long values test whether A2D only needs longer than ~5 s: the reset follows the
	 * timer by that much although the commit armed WD2 for sleep_wd_secs (ADT
	 * simple-reconfig-wdog-icc-time = 5).
	 */
	if (READ_ONCE(post_timer_wd_ms) && s->wdt) {
		writel(0, s->wdt + WDT_WD2_CTL);
		writel(0, s->wdt + WDT_WD2_COUNT);
		writel(min(READ_ONCE(post_timer_wd_ms), WDT_MAX_SECS * 1000U) * (WDT_TICKS_PER_S / 1000),
		       s->wdt + WDT_WD2_ALARM);
		writel(1, s->wdt + WDT_WD2_CTL);
		dsb(sy);
	}

	/*
	 * Bring-up (this core does run after the timer: the WD2 above fired). Does
	 * the system counter stop at the timer (every udelay() would then hang)? Safety WD2
	 * 4 s, 1M cluster-ctl reads timed by CNTPCT, an SPMI mark polled by count (no udelay):
	 * 0x50EEPPQQ, E bit 7 set, bit 6 = counter stopped. Then the WD2 by the answer:
	 * stopped -> 100 ms, moving -> 2.5 s; a loop that hangs leaves the 4 s.
	 */
	if (READ_ONCE(post_timer_counter_probe) && s->wdt && s->cluster_ctl) {
		u32 i, v[3], st;
		u64 c0, c1;
		bool stopped;

		t6040_wd2_raw(s, 4000);
		isb();
		c0 = read_sysreg(cntpct_el0);
		for (i = 0; i < 1000000; i++)
			(void)readl_relaxed(s->cluster_ctl);
		isb();
		c1 = read_sysreg(cntpct_el0);
		stopped = c1 == c0;
		for (i = 0; i < 3; i++)
			v[i] = readl(s->cluster_ctl + CLUSTER_CTL(i)) & 0x3f;
		if (s->spmi0 && READ_ONCE(marks) && s->alarm_ok) {
			void __iomem *b = s->spmi0;
			u32 m = 0x50000000 | (v[0] | 0x80 | (stopped ? 0x40 : 0)) << 16 |
				v[1] << 8 | v[2];

			/* 0x50EAAAAA: tag E, then the ANE ACTUAL nibbles (t6040_ane_actual). */
			if (READ_ONCE(post_timer_ane))
				m = 0x50e00000 | t6040_ane_actual(s);
			/*
			 * 0x50CMMMMM: tag C, ACTUAL != 0 as bits: 0-3 E cores, 4-8 P0 cores,
			 * 9-13 P1 cores (cpu-start bits 0-8, 10-14), 14-18 ANE sys/mpm/cpu/td/base.
			 */
			if (READ_ONCE(post_timer_mask)) {
				u32 a = t6040_ane_actual(s), mask = 0, b;

				for (b = 0; b < 15 && s->soc_ps; b++) {
					if (b == 9)
						continue;
					if (FIELD_GET(PS_ACTUAL, readl(s->soc_ps + 8 * b)))
						mask |= BIT(b < 9 ? b : b - 1);
				}
				for (b = 0; b < 5; b++)
					if ((a >> (16 - 4 * b)) & 0xf)
						mask |= BIT(14 + b);
				m = 0x50c00000 | mask;
			}

			for (i = 0; i < 16 && !(readl(b + SPMI_STATUS) & SPMI_STATUS_RX_EMPTY); i++)
				readl(b + SPMI_REPLY);
			writel(SPMI_CMD_PMU(PMU_RTC_COMPARE, SPMI_OP_WRITE4), b + SPMI_CMD);
			writel(m, b + SPMI_CMD);
			for (i = 0; i < 1000000; i++) {
				st = readl(b + SPMI_NOTIFY_ACK);
				if (st & SPMI_NOTIFY_REPLY)
					break;
			}
			writel(SPMI_NOTIFY_REPLY, b + SPMI_NOTIFY_ACK);
			if (!(readl(b + SPMI_STATUS) & SPMI_STATUS_RX_EMPTY))
				readl(b + SPMI_REPLY);
		}
		t6040_wd2_raw(s, stopped ? 100 : 2500);
	}

	/* A raised line would end the WFI the engine waits for. */
	aic_sleep_mask_all();
	spin_table_release_stopped();

	/* Optional, the bytes, necessity from Linux unproven (off by default). */
	if (replay_pmu_go_hib)
		t6040_pmu_go_hib(s);

	/*
	 * Bring-up probe: the SoC resets ~5 s after the timer (a hold before the timer moves it),
	 * and a probe that waited with udelay() never marked: nothing that waits on
	 * time or on a reply may finish after the timer. Staged, no timer-based waits; the
	 * last mark that lands tells how far it got (the wake alarm is not put back):
	 *   0x50ffffff  A: an SPMI mark right after the timer landed
	 *   0x50EEPPQQ  B: cluster-ctl E/P0/P1 bytes [5:0] read once
	 *   0x50EEPPQQ  C: after N * 1000 cluster-ctl reads: E bit 7 set, E bit 6 = the
	 *               counter (CNTPCT) did not move over the reads
	 */
	if (READ_ONCE(post_timer_probe_ms) && s->cluster_ctl) {
		u32 n = min(READ_ONCE(post_timer_probe_ms), 4000U) * 1000, i, v[3];
		u64 c0, c1;

		t6040_mark_raw(s, 0x50ffffff);
		for (i = 0; i < 3; i++)
			v[i] = readl(s->cluster_ctl + CLUSTER_CTL(i)) & 0x3f;
		t6040_mark_raw(s, 0x50000000 | v[0] << 16 | v[1] << 8 | v[2]);
		isb();
		c0 = read_sysreg(cntpct_el0);
		for (i = 0; i < n; i++)
			(void)readl_relaxed(s->cluster_ctl);
		isb();
		c1 = read_sysreg(cntpct_el0);
		for (i = 0; i < 3; i++)
			v[i] = readl(s->cluster_ctl + CLUSTER_CTL(i)) & 0x3f;
		t6040_mark_raw(s, 0x50000000 | (v[0] | 0x80 | (c1 == c0 ? 0x40 : 0)) << 16 |
				  v[1] << 8 | v[2]);
	}

	t6040_deep_wfi();
}

/*
 * The wake return (t6040_sleep_return), reached on the boot core after aura's
 * AURAS2R1 mailbox hands it back to cpu_resume. aura restores NO SoC register;
 * every write here is the OS driver's job. It is the
 * mirror of the PMGR wake restore: the three CPU complexes on, the two wake
 * pwrgates, SOC-East retention off (with the PARENT_MISSING poll), the axi2af
 * fabric-bridge re-arm, the CPM voters re-asserted, the clk mux restored, the
 * AP-global S2R control cleared, and the wake-status words read. It runs on
 * the J616s once the data blockers are overridden (i_know_untested).
 */
/*
 * Wake-return marks (seen on the J616s: an asynchronous SError on CPU 0 after the wake, last
 * mark a driver step): complete every access of the step before the mark, so a pending SError is
 * taken before the next mark lands and the comparator names the step that caused it.
 */
/*
 * On the J616s a wake reached the device resume and took an SError in the noirq resume of the ANS
 * apcie DART (0x4_1000_0000, no power domain in the DT). Record what the ANS/APCIe raise did,
 * right after it: 'APCW' at PMU 0xa280 (over the ATCC record; PMVC at 0xa2ac stays), w1 =
 * stopped(bit31) | step reached << 16 | ret & 0xffff, w2..w9 = the SOC-East PS words of ANS,
 * GP, SYS_GP, ST0, SYS_ST0, ST1, SYS_ST1, PHY_SW (PMGR, always readable).
 */
#define T6040_APCW_PMU		0xa280
#define T6040_APCW_MAGIC	0x57435041	/* "APCW" in PMU byte order */

static bool apcie_wake_rec = true;
module_param(apcie_wake_rec, bool, 0644);
MODULE_PARM_DESC(apcie_wake_rec, "Bring-up: record the wake-side ANS/APCIe raise at PMU 0xa280 ('APCW')");

/*
 * With the apcie fabric up, the noirq resume of 410000000.iommu still took the SError:
 * that DART sits in the block of port 0 of the GP PCIe host (pcie@1cb0000000, Wi-Fi/BT; port 1
 * SD behind 411000000.iommu), whose clocks the bootloader set up at boot and nobody after an
 * S2R. pcie_port_clk (mask of ports) undoes t6040_gp_port_stop()'s PHY/app clock part right after
 * the apcie raise: lane REFCLK0/1 request + ack, REFCLKEN, PORT_APPCLK EN, PORT_STATUS ready.
 * The link itself stays down. Result per port in a byte of APCW w10: bit0 refclk acked,
 * bit1 port ready, bit4 refclk timeout, bit5 ready timeout, bit7 not mapped.
 */
#define GP_PHY_CLK1ACK		BIT(3)
#define GP_PORT_STATUS		0x804
#define GP_PORT_STATUS_READY	BIT(0)

static unsigned int pcie_port_clk = 3;
module_param(pcie_port_clk, uint, 0644);
MODULE_PARM_DESC(pcie_port_clk,
		 "Bring-up wake: GP PCIe ports (mask) whose refclk + app clock are brought back after the apcie raise");

static void t6040_gp_pcie_clk(struct t6040_sleep *s)
{
	unsigned int mask = READ_ONCE(pcie_port_clk);
	u32 res = 0, val;
	int i;

	for (i = 0; i < 2; i++) {
		void __iomem *phy = s->gp_phy[i], *port = s->gp_port[i];
		u32 r = 0, cfg, app;

		if (!(mask & BIT(i)))
			continue;
		if (!phy || !port || !s->gp_saved[i]) {
			res |= BIT(7) << (8 * i);
			continue;
		}
		cfg = s->gp_phy_saved[i];
		app = s->gp_appclk_saved[i];
		/* t6040_gp_port_stop() backwards: clock requests, reference buffer, enables, link. */
		writel(readl(phy) | GP_PHY_CLK0REQ, phy);
		if (readl_poll_timeout_atomic(phy, val, val & GP_PHY_CLK0ACK, 1, 50000))
			r |= BIT(4);
		writel(readl(phy) | GP_PHY_CLK1REQ, phy);
		if (readl_poll_timeout_atomic(phy, val, val & GP_PHY_CLK1ACK, 1, 50000))
			r |= BIT(4);
		if (!(r & BIT(4)))
			r |= BIT(0);
		if (!(cfg & GP_PHY_RESET)) {
			writel(readl(phy) & ~GP_PHY_RESET, phy);
			udelay(1);
		}
		writel(readl(phy) | (cfg & (GP_PHY_EN0 | GP_PHY_EN1)), phy);
		writel(readl(phy) | (cfg & GP_PHY_LINK), phy);
		writel((readl(port + GP_PORT_APPCLK) & ~(GP_PORT_APPCLK_EN | GP_PORT_APPCLK_CGDIS)) |
		       (app & (GP_PORT_APPCLK_EN | GP_PORT_APPCLK_CGDIS)), port + GP_PORT_APPCLK);
		/*
		 * Clocks back (refclk acked) but no PORT_STATUS ready: the stop also wrote
		 * the controller's PERST register 0. Put it back as it was (the endpoint's PERST#
		 * pin stays asserted), with the guard bit the platform sets around it.
		 */
		writel(readl(port + GP_PORT_PERST_GUARD) | BIT(8), port + GP_PORT_PERST_GUARD);
		writel(s->gp_perst_saved[i], port + GP_PORT_PERST);
		writel(readl(port + GP_PORT_PERST_GUARD) & ~BIT(8), port + GP_PORT_PERST_GUARD);
		if (readl_poll_timeout_atomic(port + GP_PORT_STATUS, val, val & GP_PORT_STATUS_READY,
					      1, 50000))
			r |= BIT(5);
		else
			r |= BIT(1);
		res |= r << (8 * i);
	}
	s->gp_port_res = res;
}

static void t6040_apcie_wake_rec(struct t6040_sleep *s)
{
	static const u16 off[8] = { PS_ANS, PS_APCIE_GP, PS_APCIE_SYS_GP, PS_APCIE_ST0,
				    PS_APCIE_SYS_ST0, PS_APCIE_ST1, PS_APCIE_SYS_ST1,
				    PS_APCIE_PHY_SW };
	u32 w[11];
	int i;

	if (!READ_ONCE(apcie_wake_rec) || !s->soc_east || !s->spmi0)
		return;
	w[10] = s->gp_port_res;
	w[0] = T6040_APCW_MAGIC;
	w[1] = (s->pcie_ans_stopped ? BIT(31) : 0) | (s->apcie_wake_step & 0xff) << 16 |
	       ((u32)s->pcie_ans_on_ret & 0xffff);
	for (i = 0; i < 8; i++)
		w[2 + i] = readl(s->soc_east + off[i]);
	for (i = 0; i < 11; i++)
		t6040_pmu_cmd(s, SPMI_CMD_PMU(T6040_APCW_PMU + 4 * i, SPMI_OP_WRITE4), &w[i],
			      NULL, 0);
}

static void t6040_ret_mark(struct t6040_sleep *s, u32 n)
{
	dsb(sy);
	isb();
	t6040_mark(s, n);
}

static void t6040_sleep_return(struct t6040_sleep *s)
{
	u32 val;
	int i, ret;

	t6040_mark(s, 0x30);
	/* The reset left the timer and IRQ routing at their EL2 defaults. */
	write_sysreg(s->cntkctl, cntkctl_el1);
	isb();
	write_sysreg_s(s->siq_cfg, SYS_APL_SIQ_CFG_EL1);
	t6040_ret_mark(s, 0x31);

	spin_table_stop_finished();
	t6040_ret_mark(s, 0x32);
	aic_sleep_restore();
	t6040_ret_mark(s, 0x33);

	/*
	 * The three CPU complexes back on (E, P0, P1), nibble 0xf, poll +0xc for
	 * the ready bit. The ACC restore then writes ACC register 0xe440f8
	 * := 1 per complex; that per-complex ACC window is not mapped here (TODO)
	 * and is left to the normal per-CPU bring-up.
	 */
	if (s->cluster_ctl)
		for (i = 0; i < CLUSTER_CTL_NR; i++) {
			u32 on = i < CLUSTER_CTL_FIRST_WAIT ? CLUSTER_CTL_ON :
				 s->cluster_saved[i] & CLUSTER_CTL_NIBBLE;

			val = readl(s->cluster_ctl + CLUSTER_CTL(i));
			writel((val & ~CLUSTER_CTL_NIBBLE) | on, s->cluster_ctl + CLUSTER_CTL(i));
		}
	t6040_ret_mark(s, 0x34);

	/* Wake pwrgates and CPM voters: read, set BIT31. */
	if (s->pwrgate) {
		writel(readl(s->pwrgate + PWRGATE_WAKE_0) | PWRGATE_ON, s->pwrgate + PWRGATE_WAKE_0);
		writel(readl(s->pwrgate + PWRGATE_WAKE_1) | PWRGATE_ON, s->pwrgate + PWRGATE_WAKE_1);
	}
	t6040_ret_mark(s, 0x35);

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
	t6040_ret_mark(s, 0x36);

	/*
	 * The SOC DPE restore (the 14 DPE energy accumulators |= 1) is skipped:
	 * the DPE RegMap base and its offset list are not resolved here, and it
	 * is energy accounting only, benign to skip. TODO.
	 */

	/* The fabric-bridge retention re-arm (the ANS/Wi-Fi/display-dead guard). */
	t6040_axi2af_rearm(s);
	t6040_ret_mark(s, 0x37);

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
	s->gp_port_res = 0;
	if (READ_ONCE(pcie_port_clk)) {
		t6040_gp_pcie_clk(s);
		t6040_ret_mark(s, 0x56);
	}
	t6040_apcie_wake_rec(s);
	if (READ_ONCE(aop_wake)) {
		t6040_aop_wake(s);
		t6040_ret_mark(s, 0x57);
	}
	t6040_ret_mark(s, 0x38);

	if (s->pwrgate) {
		writel((readl(s->pwrgate + PWRGATE_CPM_0) & ~PWRGATE_ON) | PWRGATE_ON,
		       s->pwrgate + PWRGATE_CPM_0);
		writel((readl(s->pwrgate + PWRGATE_CPM_1) & ~PWRGATE_ON) | PWRGATE_ON,
		       s->pwrgate + PWRGATE_CPM_1);
		writel((readl(s->pwrgate + PWRGATE_CPM_2) & ~PWRGATE_ON) | PWRGATE_ON,
		       s->pwrgate + PWRGATE_CPM_2);
	}
	t6040_ret_mark(s, 0x39);

	/* Undo the entry clk-mux park; the clock restore is otherwise a NOP. */
	t6040_clkgen_restore(s);
	t6040_ret_mark(s, 0x3a);

	/* Software ack of the armed S2R request (the platform has no stop of the S2R timer). */
	if (s->aop_global)
		writel(0, s->aop_global + AOP_S2R_CTRL);
	t6040_ret_mark(s, 0x3b);

	/* The keep-alive bits the commit cleared, back (as the wake restore enables them). */
	if (s->ka_on_before)
		t6040_keep_alive(s, true, s->ka_on_before);
	t6040_ret_mark(s, 0x3c);

	/* A longer WD2 over the device resume, until .end. */
	t6040_sleep_set_wd2(s, WDT_SUSPEND_SECS);
	t6040_ret_mark(s, 0x3f);

	if (s->mbox) {
		dcache_inval_poc((unsigned long)s->mbox, (unsigned long)(s->mbox + 1));
		pr_info("t6040-sleep: wake: resumes now %llu\n", READ_ONCE(s->mbox->resumes));
	}
	if (s->aop_global)
		for (i = 0; i < AOP_WAKE_WORDS; i++)
			s->wake_status[i] = readl(s->aop_global + AOP_WAKE_STATUS(i));
	s->returned = true;
}

static int t6040_sleep_valid(suspend_state_t state)
{
	return state == PM_SUSPEND_MEM;
}

/*
 * The refusal-first gate. Default: print the blocker list and return an error
 * here, before any device suspends, so nothing is entered. A dry run proceeds
 * and refuses at .enter. allow_entry with no blocker: the only path that would
 * reach the commit, and it cannot, because the missing-data blockers never
 * clear.
 */
/*
 * Seq 25 stalls because ATC_n_COMMON does not come back up (port 0 or port 1 at random).
 * The platform writes SMC key pmVC (the VDD_CIO rail request,
 * its enable mask | 0x100) = 0x101 while any ATC USB_AON is on, also at sleep; Linux never
 * does, and the ADT marks the rail power-gated (vdd-cio-rail-pg). A rail the SMC drops during
 * the sleep transition would race the engine. pmvc != 0 writes that value in .begin and logs
 * the key before and after.
 */
static unsigned int pmvc = 0x101;
module_param(pmvc, uint, 0644);
MODULE_PARM_DESC(pmvc, "Bring-up: value for SMC key pmVC (VDD_CIO request, the platform 0x101) written in .begin; 0 = leave it");

/* The .begin log does not reach the journal; keep the result in PMU SRAM 0xa2ac. */
#define T6040_PMVC_PMU		0xa2ac
#define T6040_PMVC_MAGIC	0x43564d50	/* "PMVC" in PMU byte order */

/* The SMC node is apple,t6041-smc + apple,t8103-smc here (macsmc's match list). */
static const struct of_device_id t6040_smc_match[] = {
	{ .compatible = "apple,t8103-smc" },
	{ .compatible = "apple,smc" },
	{}
};

static int t6040_sleep_pmvc_write(struct t6040_sleep *s, u32 v, bool rec)
{
	struct platform_device *pdev;
	struct device_node *np;
	struct apple_smc *smc;
	u32 before = 0, after = 0, w[5];
	int rb = -ENODEV, ret = -ENODEV, ra = -ENODEV, i;

	np = of_find_matching_node(NULL, t6040_smc_match);
	pdev = np ? of_find_device_by_node(np) : NULL;
	of_node_put(np);
	smc = pdev ? platform_get_drvdata(pdev) : NULL;
	if (smc) {
		rb = apple_smc_read_u32(smc, SMC_KEY(pmVC), &before);
		ret = apple_smc_write_u32(smc, SMC_KEY(pmVC), v);
		ra = apple_smc_read_u32(smc, SMC_KEY(pmVC), &after);
	}
	if (pdev)
		put_device(&pdev->dev);
	dev_info(s->dev, "pmVC: read %d 0x%08x, write 0x%08x %d, read %d 0x%08x\n",
		 rb, before, v, ret, ra, after);
	if (rec) {
		w[0] = T6040_PMVC_MAGIC;
		w[1] = (u16)rb | (u32)(u16)ret << 16;
		w[2] = before;
		w[3] = (u16)ra;
		w[4] = after;
		for (i = 0; i < 5; i++)
			t6040_pmu_cmd(s, SPMI_CMD_PMU(T6040_PMVC_PMU + 4 * i, SPMI_OP_WRITE4),
				      &w[i], NULL, 0);
	}
	return ret;
}

static int t6040_pmvc_now_set(const char *val, const struct kernel_param *kp)
{
	u32 v;
	int ret = kstrtou32(val, 0, &v);

	if (ret)
		return ret;
	if (!t6040_sleep)
		return -ENODEV;
	ret = t6040_sleep_pmvc_write(t6040_sleep, v, false);
	return ret < 0 ? ret : 0;
}

static const struct kernel_param_ops t6040_pmvc_now_ops = {
	.set = t6040_pmvc_now_set,
};
module_param_cb(pmvc_now, &t6040_pmvc_now_ops, NULL, 0200);
MODULE_PARM_DESC(pmvc_now, "Bring-up: write this value to SMC key pmVC now and log the key before/after");

static void t6040_sleep_pmvc(struct t6040_sleep *s)
{
	u32 v = READ_ONCE(pmvc);

	if (v)
		t6040_sleep_pmvc_write(s, v, true);
}

static int t6040_sleep_begin(suspend_state_t state)
{
	struct t6040_sleep *s = t6040_sleep;
	unsigned int disp = 0;
	unsigned int live = t6040_sleep_live_blockers(s, NULL, &disp);
	unsigned int data = READ_ONCE(i_know_untested) ? 0 : T6040_SLEEP_DATA_BLOCKERS;
	/* The display pipes are lowered during device suspend (apple-dcp .suspend),
	 * so .begin only gates on the non-display live blockers; .enter re-checks the
	 * full set at the commit point, by which time the display is off. */
	unsigned int begin_total = (live - disp) + data;
	unsigned int dry = READ_ONCE(dry_run), secs;

	WRITE_ONCE(t6040_sleep_dry, 0);
	s->returned = false;
	s->gp_saved[0] = s->gp_saved[1] = false;
	t6040_aop_ps_save(s);
	t6040_susf_begin(s);
	t6040_sleep_pmvc(s);
	if (READ_ONCE(dcs_pg) & BIT(0))
		t6040_dcs_dyn_pg(s);
	if (READ_ONCE(dcs_ps_auto) & BIT(0))
		t6040_dcs_ps_auto_on(s);
	pr_info("t6040-sleep: suspend requested: %u live blocker(s) (%u display, cleared during device suspend) + %u missing-data blocker(s)\n",
		live, disp, data);
	t6040_sleep_log_blockers(s);

	if (!begin_total && allow_entry) {
		/* Non-display blockers clear and the override/unknowns allow it: proceed
		 * to device suspend; .enter holds the real commit until the display is
		 * also off. The AON WD2 is the net from here. */
		if (disp)
			pr_warn("t6040-sleep: entering: %u display pipe(s) still on, relying on device suspend to lower them before the commit\n",
				disp);
		t6040_sleep_power_ops(s);
		t6040_sleep_set_wd2(s, WDT_SUSPEND_SECS);
		pm_set_suspend_via_firmware();
		t6040_mark_save(s);
		t6040_mark(s, 0x01);
		if (s->alarm_ok)
			WRITE_ONCE(pm_bringup_dev_mark, t6040_dev_mark);
		return 0;
	}

	if (dry >= 2) {
		dry = min(dry, 3U);
		t6040_sleep_power_ops(s);
		if (!s->power_ops)
			pr_warn("t6040-sleep: dry run: no CPU parking power ops, so no hold at the commit point\n");
		WRITE_ONCE(t6040_sleep_dry, dry);
		pm_set_suspend_via_firmware();
		secs = min(READ_ONCE(wd2_secs), WDT_MAX_SECS);
		if (secs && s->wdt)
			t6040_sleep_set_wd2(s, secs);
		else
			secs = 0;
		pr_info("t6040-sleep: dry run %u: firmware-driven device sleep short of %s, refused at the commit point, WD2 %u s (0: off)\n",
			dry, dry >= 3 ? "the stops only a power cycle undoes" :
					"the last steps", secs);
		return 0;
	}

	if (dry) {
		pr_info("t6040-sleep: dry run 1: parking CPUs and refusing at the commit point; the firmware is never told\n");
		return 0;
	}

	pr_info("t6040-sleep: refusing the suspend; nothing entered\n");
	return -EOPNOTSUPP;
}

/* After the device suspend, in process context. */
static int t6040_sleep_prepare(void)
{
	t6040_mark(t6040_sleep, 0x02);
	t6040_sleep_watch_bound(t6040_sleep);

	return 0;
}

/*
 * A dry run 2 or 3 at the commit point, interrupts off and one CPU up: every
 * check a real commit makes, the parked CPUs held and let go, at level 3 each
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
	t6040_log_ane(s, "dry-run commit point");
	t6040_log_a2d(s, "dry-run commit point");
	t6040_sleep_log_s2r_state(s, "dry-run commit point");
	t6040_log_cpm(s, "dry-run commit point");
	{
		u32 map[T6040_PS_SNAP_WORDS];

		t6040_ps_snap_take(s, map);
		pr_info("t6040-sleep: dry-run commit point: PS snapshot %*ph\n", (int)sizeof(map), map);
		if (READ_ONCE(ps_snap))
			t6040_ps_snap_store(s);
	}
	if (READ_ONCE(pmc_probe) & BIT(0)) {
		u32 pv[ARRAY_SIZE(t6040_pmc_probe_pa)];

		t6040_pmc_probe_read(s, pv);
		pr_info("t6040-sleep: dry-run commit point: PMC probe aa140 %#x ac000 %#x ac00c %#x ac020 %#x 800000 %#x 80c000 %#x 820000 %#x 824004 %#x 828004 %#x\n",
			pv[0], pv[1], pv[2], pv[3], pv[4], pv[5], pv[6], pv[7], pv[8]);
		t6040_pmc_probe_store(s, pv);
	}
	/*
	 * A wake source pending and left unmasked by the masks a real commit arms would keep the
	 * AP partition from powering down (A2D then stalls at MiniPMGR +0x40). Read only.
	 */
	if (s->aop_global) {
		u32 p[AOP_WAKE_WORDS];
		int i;

		for (i = 0; i < AOP_WAKE_WORDS; i++)
			p[i] = readl(s->aop_global + AOP_WAKE_STATUS(i)) & ~t6040_wake_mask[i];
		pr_info("t6040-sleep: dry-run commit point: wake status left unmasked by the commit's masks %*ph\n",
			(int)sizeof(p), p);
	}
	if ((READ_ONCE(mtp_island_off) & BIT(1)) && s->mtp_ps) {
		u32 saved[T6040_MTP_PS_WORDS], after[T6040_MTP_PS_WORDS], left;
		int i;

		left = t6040_mtp_island_off(s, saved);
		for (i = 0; i < T6040_MTP_PS_WORDS; i++)
			after[i] = readl(s->mtp_ps + 8 * i);
		pr_info("t6040-sleep: dry-run MTP island off: left on %#x, PS before %*ph\n",
			left, (int)sizeof(saved), saved);
		pr_info("t6040-sleep: dry-run MTP island off: PS after %*ph\n",
			(int)sizeof(after), after);
		t6040_log_a2d(s, "dry-run MTP island off");
		t6040_mtp_island_back(s, saved);
		t6040_log_a2d(s, "dry-run MTP island back");
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

	if (dry >= 3 && !ret)
		pr_info("t6040-sleep: the coprocessors' last checks: %d\n",
			t6040_sleep_coprocs_check());
	if (!ret)
		spin_table_unhold_parked();

	pr_info("t6040-sleep: dry run: refused before the enclave's sleep request (%s), the coprocessors' last stops and the SoC power-down\n",
		t6040_sleep_enclave.dev ? "left by sep" : "none left");

	return -EBUSY;
}

/*
 * The display power words the platform has off before its sleep (apple,check-ps-b4-sleep), lowered
 * here: after the device suspend, interrupts off, one CPU up, so no driver, interrupt handler
 * or DART touches the pipes any more. The DCPs only stop their firmware: lowering the domains
 * from their suspend panicked on T6040. Each group from its last sub-block to its pipe CPU,
 * each word as the pmgr-pwrstate driver powers a domain off (TARGET 0, the one-shot bits
 * clear), polled to ACTUAL 0. Returns the number of words left on.
 */
static bool lower_display = true;
module_param(lower_display, bool, 0644);
MODULE_PARM_DESC(lower_display, "Lower the display power words at the S2R commit point");

static bool lower_cpu;
module_param(lower_cpu, bool, 0644);
MODULE_PARM_DESC(lower_cpu, "Also lower each display group's pipe CPU word by hand (raised an SError on T6040)");

static bool axi2af_quiesce;
module_param(axi2af_quiesce, bool, 0644);
MODULE_PARM_DESC(axi2af_quiesce, "Clear a display group's axi2af fabric-bridge bit0 before lowering it (off: its SYS domain stays on)");

#define PS_DEV_DISABLE		BIT(10)
#define PS_BUSY			BIT(11)
#define PS_RESET_REQ		BIT(12)

/*
 * The display mark: 0x5cGGSOOO, GG the group, S 0 before the write, 8 off, 4 still on;
 * for a gated CPU 1 waiting for its gate, 2 gated, 3 not gated in time.
 */
#define T6040_DISP_MARK(g, st, off)	(0x5c000000 | ((g) & 0xff) << 16 | (st) << 12 | ((off) & 0xfff))

/* The axi2af domain that carries a display group ("disp" -> "disp_sys"), or -1. */
static int t6040_axi2af_of_group(const char *group)
{
	size_t n = strlen(group);
	int i;

	for (i = 0; i < T6040_AXI2AF_DOMAINS; i++)
		if (!strncmp(t6040_axi2af[i].name, group, n) &&
		    !strcmp(t6040_axi2af[i].name + n, "_sys"))
			return i;
	return -1;
}

/*
 * The entry side of the axi2af workaround (the quiesce step for it and the PMGR
 * power-down hook): before a retained domain goes off, its fabric bridge
 * +0x104/+0x108 bit0 is cleared; t6040_axi2af_rearm() sets it again on wake. Only while
 * the domain is powered: the bridge sits in it. Mark 0x5eII0000, II the domain.
 */
static void t6040_axi2af_quiesce(struct t6040_sleep *s, int i)
{
	void __iomem *ps, *bridge;

	if (i < 0)
		return;
	ps = t6040_axi2af_ps(s, &t6040_axi2af[i]);
	bridge = s->axi2af_bridge[i];
	if (!ps || !bridge || !FIELD_GET(PS_ACTUAL, readl(ps)))
		return;
	t6040_mark_raw(s, 0x5e000000 | (i & 0xff) << 16);
	writel(readl(bridge + AXI2AF_BRIDGE_104) & ~BIT(0), bridge + AXI2AF_BRIDGE_104);
	writel(readl(bridge + AXI2AF_BRIDGE_108) & ~BIT(0), bridge + AXI2AF_BRIDGE_108);
	t6040_mark_raw(s, 0x5e008000 | (i & 0xff) << 16);
}

static bool disp_seq_off = true;

/*
 * Under Linux cluster-ctl DISP_INT went 0x2f -> 0x10 and stuck there; on the platform it reached 0,
 * DISP_CPU having gone down at the DCP stop (RUN, bit5, dart idle, then the PS word
 * in two steps). Rewriting the word under a running DCP resets the SoC. With
 * appledrm.s2r_keep_run=0 this tries the exact sequence on it.
 */
static bool pmc_seq;
module_param(pmc_seq, bool, 0644);
MODULE_PARM_DESC(pmc_seq,
		 "Release a display pipe's PMC votes as the platform (boost, DVMR + ack, BWR, rail + ack) instead of zeroing c14/c18");

static bool dispext_off;
module_param(dispext_off, bool, 0644);
MODULE_PARM_DESC(dispext_off,
		 "Power the stopped DCPEXT pipes down as the platform (PMC votes, CPU, FE, SYS) so DISP_EXT can go off");

static bool pmp_disp_release;
module_param(pmp_disp_release, bool, 0644);
MODULE_PARM_DESC(pmp_disp_release,
		 "With disp_cpu_seq: withdraw the internal display's PMP request (PTD bit 16) and wait for the ack first");

static bool disp_cpu_seq;
module_param(disp_cpu_seq, bool, 0644);
MODULE_PARM_DESC(disp_cpu_seq,
		 "Lower DISP_CPU in the two steps after the DCP stop (needs appledrm.s2r_keep_run=0)");

static bool disp_need_cpu_gated;
module_param(disp_need_cpu_gated, bool, 0644);
MODULE_PARM_DESC(disp_need_cpu_gated, "Leave DISP_FE/SYS on unless the PMGR has gated DISP_CPU");

#define DISP_CPU_GATE_US	50000

static bool gated_cpu_target_only = true;
module_param(gated_cpu_target_only, bool, 0644);
MODULE_PARM_DESC(gated_cpu_target_only, "A DISP_CPU already gated (ACTUAL 0) only gets TARGET 0, keeping AUTO_ENABLE, and dart-dcp is left unread");

static bool pmc_dcp_off = true;
module_param(pmc_dcp_off, bool, 0644);
MODULE_PARM_DESC(pmc_dcp_off, "Drop the PMS DCP voters to 0 before the internal display goes down");
module_param(disp_seq_off, bool, 0644);
MODULE_PARM_DESC(disp_seq_off, "Lower DISP_CPU, DISP_FE and DISP_SYS at the commit point as the PMGR does");

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

/* Marks 0x56SS00RR: SS the word's step, RR what it reached (bit 7: a poll timed out). */
static void t6040_disp_mark(struct t6040_sleep *s, u32 step, u32 rr)
{
	t6040_mark_raw(s, 0x56000000 | (step & 0xff) << 16 | (rr & 0xff));
}

/*
 * The power-down of a PS word: with
 * AUTO_ENABLE set, take it out of automatic mode with the target kept (v & 0xeffffcff) and
 * wait for ACTUAL 15; then @pre (the SYS hooks); then TARGET 0 with bits 9:8 written 1 and
 * bits 10-12, 24-31 kept; then poll bit11 clear and ACTUAL 0.
 */
static int t6040_psw_off(struct t6040_sleep *s, const struct t6040_psw *w, u32 step,
			      void (*pre)(struct t6040_sleep *s))
{
	u32 v = t6040_psw_read(w);
	int ret;

	if (v & PS_AUTO_ENABLE) {
		t6040_disp_mark(s, step, 0x01);
		t6040_psw_write(w, v & 0xeffffcff);
		ret = t6040_psw_poll(w, PS_ACTUAL, PS_ACTUAL);
		t6040_disp_mark(s, step, 0x02 | (ret ? 0x80 : 0));
		v = t6040_psw_read(w);
	}
	if (pre)
		pre(s);
	t6040_disp_mark(s, step, 0x03);
	t6040_psw_write(w, (v & 0xfffffcf0) | 0x300);
	ret = t6040_psw_poll(w, PS_BUSY, 0);
	t6040_disp_mark(s, step, 0x04 | (ret ? 0x80 : 0));
	ret = t6040_psw_poll(w, PS_ACTUAL, 0);
	t6040_disp_mark(s, step, 0x05 | (ret ? 0x80 : 0));
	return ret;
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
	t6040_disp_mark(s, 0x30, 0x06);
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
			if (++us > DISP_PS_TIMEOUT_US) {
				t6040_disp_mark(s, 0x08, 0x80 | i);
				return -ETIMEDOUT;
			}
			udelay(1);
		}
	t6040_disp_mark(s, 0x08, 0x00);
	return 0;
}

/* The internal display's three domains, child first, after its sub-blocks (the order). */
/* A display pipe's PMC votes, released in the order; @rail the cluster rail bit or 0. */
static int t6040_pmc_disp_release(struct t6040_sleep *s, unsigned int p, u32 rail)
{
	int ret = 0;
	u32 v;

	if (s->pmc_bwr)
		writel(readl(s->pmc_bwr + PMC_BOOST) & ~BIT(p), s->pmc_bwr + PMC_BOOST);
	if (s->pmc_dvmr) {
		writel(readl(s->pmc_dvmr) & ~BIT(p), s->pmc_dvmr);
		if (readl_poll_timeout_atomic(s->pmc_dvmr + PMC_DVMR_ACK, v, !(v & BIT(0)), 1,
					      PMC_TIMEOUT_US))
			ret |= 1;
		if (s->pmc_dcp)
			writel(hweight32(readl(s->pmc_dvmr)), s->pmc_dcp + PMC_DCP_COUNT);
	}
	if (s->pmc_bwr)
		writel(readl(s->pmc_bwr) & ~BIT(p ? p + 2 : 0), s->pmc_bwr);
	if (rail && s->pmc_dcp) {
		writel(readl(s->pmc_dcp + PMC_RAIL) & ~rail, s->pmc_dcp + PMC_RAIL);
		if (readl_poll_timeout_atomic(s->pmc_dcp + PMC_RAIL_ACK, v, !(v & rail), 1,
					      PMC_TIMEOUT_US))
			ret |= 2;
	}
	return ret;
}

/* As T8140: the coprocessor's CPU_CONTROL to 0, twice, right before its CPU word goes. */
static void t6040_dcp_cold_stop(struct t6040_sleep *s, unsigned int p)
{
	if (!s->dcp_asc_ctl[p])
		return;
	writel(0, s->dcp_asc_ctl[p]);
	writel(0, s->dcp_asc_ctl[p]);
}

/* One power word down in the two steps (any AUTO first). Returns 0x80 on a timeout. */
static u32 t6040_soc_ps_off(void __iomem *w)
{
	u32 v = readl(w), cur;
	u32 to = 0;

	if (!FIELD_GET(PS_ACTUAL, v) && !FIELD_GET(PS_TARGET, v))
		return 0;
	if (v & PS_AUTO_ENABLE) {
		writel(v & 0xeffffcff, w);
		if (readl_poll_timeout_atomic(w, cur, FIELD_GET(PS_ACTUAL, cur) == 0xf, 1,
					      PMC_TIMEOUT_US))
			to = 0x80;
	}
	writel((readl(w) & 0xfffffcf0) | 0x300, w);
	if (readl_poll_timeout_atomic(w, cur, !(cur & PS_BUSY), 1, PMC_TIMEOUT_US) ||
	    readl_poll_timeout_atomic(w, cur, !FIELD_GET(PS_ACTUAL, cur), 1, PMC_TIMEOUT_US))
		to = 0x80;
	return to;
}

/*
 * The stopped DCPEXT pipes down it, DCPEXT1 first. Marks 0x56SS00RR,
 * SS 0x80 | pipe << 4 | step (1 PMC, 2 CPU, 3 FE, 4 SYS), RR 0x80 on a timeout.
 */
static void t6040_dispext_off(struct t6040_sleep *s)
{
	int n;

	if (!s->soc_ps)
		return;
	for (n = 1; n >= 0; n--) {
		u32 base = 0x80 | n << 4;
		void __iomem *bridge = s->axi2af_bridge[t6040_dispext[n].axi2af];

		t6040_disp_mark(s, base | 1, 0);
		if (READ_ONCE(pmc_seq))
			t6040_disp_mark(s, base | 1,
					0x1 | t6040_pmc_disp_release(s, n + 1, n ? 0 : PMC_RAIL_EXT) << 4);
		t6040_disp_mark(s, base | 2, 0);
		t6040_dcp_cold_stop(s, n + 1);
		t6040_disp_mark(s, base | 2, 1);
		t6040_disp_mark(s, base | 2,
				2 | t6040_soc_ps_off(s->soc_ps + t6040_dispext[n].cpu));
		t6040_disp_mark(s, base | 3,
				t6040_soc_ps_off(s->soc_ps + t6040_dispext[n].fe));
		if (bridge) {
			writel(readl(bridge + AXI2AF_BRIDGE_104) & ~BIT(0), bridge + AXI2AF_BRIDGE_104);
			writel(readl(bridge + AXI2AF_BRIDGE_108) & ~BIT(0), bridge + AXI2AF_BRIDGE_108);
		}
		if (s->dispext_pwrgate) {
			writel(3, s->dispext_pwrgate + t6040_dispext[n].pg_a);
			writel(0, s->dispext_pwrgate + t6040_dispext[n].pg_b);
		}
		t6040_disp_mark(s, base | 4,
				t6040_soc_ps_off(s->soc_ps + t6040_dispext[n].sys));
	}
}

static int t6040_disp_off(struct t6040_sleep *s, const struct t6040_sleep_group *g)
{
	struct t6040_psw cpu = { .map = g->regmap, .off = g->first };
	struct t6040_psw fe = { .io = s->soc_east, .off = PS_DISP_FE };
	struct t6040_psw sys = { .io = s->soc_east, .off = PS_DISP_SYS };
	int ret;

	if (!s->soc_east)
		return -ENODEV;
	s->disp_down = true;
	/* The PMS DCP voters to 0 first, as the the platform entry trace and T8140's driver end. */
	if (s->pmc_dcp && READ_ONCE(pmc_dcp_off)) {
		s->pmc_dcp_saved[0] = readl(s->pmc_dcp);
		s->pmc_dcp_saved[1] = readl(s->pmc_dcp + 4);
		if (READ_ONCE(pmc_seq)) {
			t6040_disp_mark(s, 0x0a, 0);
			t6040_disp_mark(s, 0x0a, t6040_pmc_disp_release(s, 0, PMC_RAIL_INT) << 4 |
						 (readl(s->pmc_dcp + PMC_RAIL) & 0xf));
		} else {
			writel(0, s->pmc_dcp);
			writel(0, s->pmc_dcp + 4);
		}
		t6040_disp_mark(s, 0x09, (s->pmc_dcp_saved[0] & 0xf) | (s->pmc_dcp_saved[1] & 0xf) << 4);
	}
	/*
	 * DISP_CPU's word refuses any write from here: rewriting its own value at runtime reset
	 * the SoC (the platform reaches it through its "DISP-CPU-V" PMGR device). Its CPU is
	 * left in automatic mode and the PMGR gates it once the firmware idles in WFI with the
	 * run bit kept: wait for ACTUAL 0. Mark 0x560f00RR first: RR ACTUAL | AUTO_ENABLE << 4 |
	 * (TARGET != 0) << 5; then 0x561000RR, RR 0x10 gated, 0x90 not gated in time.
	 */
	{
		u32 v = t6040_psw_read(&cpu);

		t6040_disp_mark(s, 0x0f, FIELD_GET(PS_ACTUAL, v) | !!(v & PS_AUTO_ENABLE) << 4 |
					 !!FIELD_GET(PS_TARGET, v) << 5);
		if (FIELD_GET(PS_ACTUAL, v))
			t6040_dcp_dart_idle(s);
		if (READ_ONCE(disp_cpu_seq) && FIELD_GET(PS_ACTUAL, v)) {
			if (s->ptd && READ_ONCE(pmp_disp_release)) {
				u64 req = readq(s->ptd + PTD_REQ_READ), ack;
				int to;

				t6040_disp_mark(s, 0x0d, (req >> PTD_DISP_INT) & 1);
				writeq(req & ~BIT_ULL(PTD_DISP_INT), s->ptd + PTD_REQ_WRITE);
				to = readq_poll_timeout_atomic(s->ptd + PTD_ACK, ack,
							       !(ack & BIT_ULL(PTD_DISP_INT)), 10,
							       50000);
				t6040_disp_mark(s, 0x0d, 0x10 | (to ? 0x80 : 0) |
							 ((ack >> PTD_DISP_INT) & 1));
			}
			t6040_disp_mark(s, 0x0b, 0);
			t6040_dcp_cold_stop(s, 0);
			if (s->disp_pwrgate) {
				writel(3, s->disp_pwrgate + DISP_PWRGATE_A);
				writel(0, s->disp_pwrgate + DISP_PWRGATE_B);
			}
			t6040_disp_mark(s, 0x0b, 1);
			t6040_psw_off(s, &cpu, 0x0c, NULL);
		}
		ret = t6040_psw_poll_long(&cpu, PS_ACTUAL, 0, DISP_CPU_GATE_US);
		t6040_disp_mark(s, 0x10, ret ? 0x90 : 0x10);
		if (ret && READ_ONCE(disp_need_cpu_gated))
			return ret;
		ret = 0;
	}
	if (!ret)
		ret = t6040_psw_off(s, &fe, 0x20, NULL);
	if (!ret)
		ret = t6040_psw_off(s, &sys, 0x30, t6040_disp_sys_pre);
	return ret;
}

/*
 * The raise (the mirror): TARGET 15, poll ACTUAL 15, poll bit11; for DISP_SYS the
 * retention pair (A := 0, B := 1) and the axi2af bridge bit0s set; then bits 9:8 cleared and
 * AUTO_ENABLE back where the platform sets it (DISP_CPU, DISP_SYS). Marks 0x56 step 0x4x/0x5x/0x6x.
 */
static int t6040_psw_on(struct t6040_sleep *s, const struct t6040_psw *w, u32 step,
			     bool sys, bool autoen)
{
	u32 v = t6040_psw_read(w);
	int ret, ret2;

	t6040_disp_mark(s, step, 0x01);
	t6040_psw_write(w, (v & ~(PS_TARGET | PS_AUTO_ENABLE | PS_FLAGS)) | PS_TARGET);
	ret = t6040_psw_poll(w, PS_ACTUAL, PS_ACTUAL);
	ret2 = t6040_psw_poll(w, PS_BUSY, 0);
	t6040_disp_mark(s, step, 0x02 | (ret || ret2 ? 0x80 : 0));
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
		t6040_disp_mark(s, step, 0x06);
	}
	v = t6040_psw_read(w);
	t6040_psw_write(w, (v & ~PS_AUTO_ENABLE) | PS_FLAGS | (autoen ? PS_AUTO_ENABLE : 0));
	t6040_disp_mark(s, step, 0x05);
	return ret;
}

/* The internal display back on, parent first, whatever brought .enter back. */
static void t6040_disp_back(struct t6040_sleep *s)
{
	struct t6040_sleep_group *g = NULL;
	unsigned int i;

	if (!s->disp_down)
		return;
	s->disp_down = false;
	for (i = 0; i < s->ngroups; i++)
		if (!strcmp(s->groups[i].name, "disp"))
			g = &s->groups[i];
	{
		struct t6040_psw sys = { .io = s->soc_east, .off = PS_DISP_SYS };
		struct t6040_psw fe = { .io = s->soc_east, .off = PS_DISP_FE };

		if (t6040_psw_on(s, &sys, 0x40, true, true))
			return;
		if (t6040_psw_on(s, &fe, 0x50, false, false))
			return;
		/* DISP_CPU's word can't be written from here: automatic mode raises it. */
		(void)g;
	}
	if (s->pmc_dcp && READ_ONCE(pmc_dcp_off)) {
		writel(s->pmc_dcp_saved[1], s->pmc_dcp + 4);
		writel(s->pmc_dcp_saved[0], s->pmc_dcp);
	}
}

static bool disp_probe;
module_param(disp_probe, bool, 0644);
MODULE_PARM_DESC(disp_probe, "Bring-up: at the commit point record the internal display's words and refuse");

/*
 * The internal display as the DCP's stop left it: bits 0-10 the sub-blocks +0x8..+0x58 with
 * TARGET != 0, 11-14 DISP_CPU ACTUAL, 15 its AUTO_ENABLE, 16-19 DISP_FE ACTUAL, 20-23
 * DISP_SYS ACTUAL.
 */
static u32 t6040_disp_probe_word(struct t6040_sleep *s)
{
	u32 w = 0, val, off;
	unsigned int i;

	for (i = 0; i < s->ngroups; i++) {
		const struct t6040_sleep_group *g = &s->groups[i];

		if (strcmp(g->name, "disp"))
			continue;
		for (off = g->first + PS_STRIDE; off <= g->last && off - g->first <= 11 * PS_STRIDE;
		     off += PS_STRIDE)
			if (!regmap_read(g->regmap, off, &val) && FIELD_GET(PS_TARGET, val))
				w |= BIT((off - g->first) / PS_STRIDE - 1);
		if (!regmap_read(g->regmap, g->first, &val))
			w |= FIELD_GET(PS_ACTUAL, val) << 11 | !!(val & PS_AUTO_ENABLE) << 15;
	}
	if (s->soc_east) {
		w |= FIELD_GET(PS_ACTUAL, readl(s->soc_east + PS_DISP_FE)) << 16;
		w |= FIELD_GET(PS_ACTUAL, readl(s->soc_east + PS_DISP_SYS)) << 20;
	}
	return w;
}

static unsigned int t6040_sleep_lower_display(struct t6040_sleep *s)
{
	bool quiesce = READ_ONCE(axi2af_quiesce), with_cpu = READ_ONCE(lower_cpu);
	unsigned int i, left = 0;

	/*
	 * The platform takes a DCPEXT pipe down CPU, FE, SYS (child first). The group loop below
	 * lowers FE and SYS and leaves the CPU, whose word then refuses the write:
	 * the DCPEXT pipes go first.
	 */
	if (READ_ONCE(dispext_off))
		t6040_dispext_off(s);

	for (i = 0; i < s->ngroups; i++) {
		const struct t6040_sleep_group *g = &s->groups[i];
		u32 off = g->last;

		if (quiesce)
			t6040_axi2af_quiesce(s, t6040_axi2af_of_group(g->name));
		for (;;) {
			bool cpu = off == g->first;
			u32 val, cur;

			/* An automatically gated CPU still powered: wait for the PMGR to gate it. */
			if (cpu && !regmap_read(g->regmap, off, &val) && (val & PS_AUTO_ENABLE) &&
			    FIELD_GET(PS_ACTUAL, val)) {
				t6040_mark_raw(s, T6040_DISP_MARK(i, 1, off));
				t6040_mark_raw(s, T6040_DISP_MARK(i,
					t6040_regmap_poll_off(g->regmap, off, &cur) ? 3 : 2, off));
			}
			if ((!cpu || with_cpu) && !regmap_read(g->regmap, off, &val) &&
			    t6040_sleep_ps_on(val, cpu)) {
				t6040_mark_raw(s, T6040_DISP_MARK(i, 0, off));
				val &= ~(PS_TARGET | PS_AUTO_ENABLE | PS_FLAGS | PS_DEV_DISABLE |
					 PS_BUSY | PS_RESET_REQ);
				regmap_write(g->regmap, off, val);
				if (t6040_regmap_poll_off(g->regmap, off, &cur)) {
					t6040_mark_raw(s, T6040_DISP_MARK(i, 4, off));
					pr_warn("t6040-sleep: %s +%#x still on: %#x\n", g->name,
						off, cur);
					left++;
				} else {
					t6040_mark_raw(s, T6040_DISP_MARK(i, 8, off));
				}
			}
			if (off == g->first)
				break;
			off -= PS_STRIDE;
		}
		if (!i && !strcmp(g->name, "disp") && READ_ONCE(disp_seq_off) &&
		    t6040_disp_off(s, g))
			left++;
	}
	return left;
}

/* Which display groups are on, one bit each (the refusal mark). */
static u32 t6040_sleep_groups_on(struct t6040_sleep *s)
{
	unsigned int i;
	u32 mask = 0;

	for (i = 0; i < s->ngroups; i++) {
		const struct t6040_sleep_group *g = &s->groups[i];
		u32 off, val;

		for (off = g->first; off <= g->last; off += PS_STRIDE)
			if (regmap_read(g->regmap, off, &val) ||
			    t6040_sleep_ps_on(val, off == g->first))
				mask |= BIT(i);
	}
	return mask;
}

/* The first display word on, for the refusal mark: index, flag bits, ACTUAL and TARGET. */
static u32 t6040_sleep_first_on(struct t6040_sleep *s)
{
	unsigned int i;

	for (i = 0; i < s->ngroups; i++) {
		const struct t6040_sleep_group *g = &s->groups[i];
		u32 off, val;

		for (off = g->first; off <= g->last; off += PS_STRIDE) {
			if (regmap_read(g->regmap, off, &val) ||
			    !t6040_sleep_ps_on(val, off == g->first))
				continue;
			return (((off - g->first) / PS_STRIDE) & 0xf) << 12 |
			       !!(val & PS_AUTO_ENABLE) << 8 | !!(val & PS_DEV_DISABLE) << 9 |
			       !!(val & PS_BUSY) << 10 | !!(val & PS_RESET_REQ) << 11 |
			       (val & (PS_ACTUAL | PS_TARGET));
		}
	}
	return 0;
}

static bool axi2af_gate;
module_param(axi2af_gate, bool, 0644);
MODULE_PARM_DESC(axi2af_gate, "Refuse the commit while any axi2af domain is still powered (the panic gate)");

/* Which of the 15 axi2af domains read ACTUAL != 0, one bit each. */
static u32 t6040_axi2af_on_mask(struct t6040_sleep *s)
{
	u32 mask = 0;
	int i;

	for (i = 0; i < T6040_AXI2AF_DOMAINS; i++) {
		void __iomem *ps = t6040_axi2af_ps(s, &t6040_axi2af[i]);

		if (ps && FIELD_GET(PS_ACTUAL, readl(ps)))
			mask |= BIT(i);
	}
	return mask;
}

/* After the late and noirq device suspend, before the CPUs go offline: a bring-up mark. */
static int t6040_sleep_prepare_late(void)
{
	t6040_mark(t6040_sleep, 0x03);
	return 0;
}

static int t6040_sleep_enter(suspend_state_t state)
{
	struct t6040_sleep *s = t6040_sleep;
	unsigned int dry = READ_ONCE(t6040_sleep_dry);
	int ret;

	if (dry)
		return t6040_sleep_dry_enter(s, dry);

	t6040_mark(s, 0x05);
	t6040_pmc_cp(s, 0);
	if (READ_ONCE(disp_probe)) {
		/* Bring-up: what the DCP's stop left, then refuse with it kept (0x55). */
		t6040_mark_raw(s, 0x55000000 | t6040_disp_probe_word(s));
		WRITE_ONCE(pm_bringup_dev_mark, NULL);
		pr_info("t6040-sleep: display probe at the commit point; refusing\n");
		return -EBUSY;
	}
	if (READ_ONCE(lower_display)) {
		unsigned int left = t6040_sleep_lower_display(s);

		t6040_mark(s, 0x06);
		if (left)
			pr_warn("t6040-sleep: %u display power word(s) did not go off\n", left);
	}
	/*
	 * The PMGR quiesce panics unless all 15 axi2af domains read ACTUAL 0, and a commit with
	 * some on reset the SoC about a second after the S2R timer was armed. Refuse
	 * with the domains still on: 0x59 then a 15-bit mask, bit i = t6040_axi2af[i].
	 */
	t6040_pmc_cp(s, 1);
	if (READ_ONCE(axi2af_gate)) {
		/* The commit lowers the ATC complexes (bits 0-3) and ANS (bit 9) itself. */
		u32 on = t6040_axi2af_on_mask(s) &
			 ~((READ_ONCE(lower_atc) ? GENMASK(3, 0) : 0) | BIT(9));

		if (on) {
			t6040_mark_raw(s, 0x59000000 | on);
			WRITE_ONCE(pm_bringup_dev_mark, NULL);
			pr_info("t6040-sleep: at the commit point: axi2af domains still on %#x; refusing\n",
				on);
			t6040_disp_back(s);
			return -EBUSY;
		}
	}
	if (t6040_sleep_total_blockers(s) || !allow_entry) {
		unsigned int on = t6040_sleep_log_groups(s);

		/*
		 * The reason, kept through what may follow: 0x5bGGWRVV, GG the groups on, then the
		 * first word on: W its index in its group (0 the CPU), R bit 0 AUTO_ENABLE, bit 1
		 * DEV_DISABLE, bit 2 BUSY, bit 3 RESET, VV its ACTUAL and TARGET.
		 */
		t6040_mark_raw(s, 0x5b000000 | (t6040_sleep_groups_on(s) & 0xff) << 16 |
				    t6040_sleep_first_on(s));
		WRITE_ONCE(pm_bringup_dev_mark, NULL);

		pr_info("t6040-sleep: at the commit point: %u CPU(s) online, %u display group(s) on; refusing\n",
			num_online_cpus(), on);
		t6040_disp_back(s);
		return -EBUSY;
	}

	/* Unreachable with the current unknowns. #1208's order. */
	if (t6040_sleep_refuse(s)) {
		/* 0x5aGGWWWW: why, as t6040_sleep_refuse() recorded it (GG display groups on). */
		t6040_mark_raw(s, 0x5a000000 | (t6040_refuse_why & 0xffffff));
		WRITE_ONCE(pm_bringup_dev_mark, NULL);
		t6040_disp_back(s);
		return -EBUSY;
	}
	t6040_mark(s, 0x10);
	ret = spin_table_hold_parked();
	if (ret) {
		t6040_mark_restore(s);
		t6040_disp_back(s);
		return ret;
	}
	t6040_mark(s, 0x11);
	ret = t6040_sleep_coprocs_check();
	if (!ret)
		ret = t6040_sleep_enclave_tell();
	if (ret) {
		spin_table_unhold_parked();
		t6040_mark_restore(s);
		t6040_disp_back(s);
		return ret;
	}
	t6040_mark(s, 0x12);
	t6040_pmc_cp(s, 2);
	s->pcie_ans_stopped = false;
	t6040_sleep_coprocs_stop(true);
	t6040_sleep_coprocs_stop(false);
	t6040_mark(s, 0x13);
	t6040_pmc_cp(s, 3);
	t6040_wake_arm(s);
	t6040_mark(s, 0x14);

	s->cntkctl = read_sysreg(cntkctl_el1);
	pm_set_resume_via_firmware();
	t6040_mark(s, 0x15);
	ret = cpu_suspend(0, t6040_sleep_commit);
	if (!ret)
		t6040_sleep_return(s);
	else
		t6040_mark_restore(s);
	if (s->alarm_ok)
		pr_info("t6040-sleep: marks: PMU comparator was %#x (reply %#x), cpu_suspend %d\n",
			s->alarm_saved, s->alarm_hdr, ret);

	if (s->returned)
		t6040_ret_mark(s, 0x41);
	t6040_wake_restore(s);
	if (s->returned)
		t6040_ret_mark(s, 0x42);
	t6040_disp_back(s);
	if (s->returned)
		t6040_ret_mark(s, 0x43);
	return ret;
}

/*
 * After .enter: 0x44 at the end of syscore_resume (IRQs still off, ours is registered last),
 * 0x45 in .wake (IRQs on, the secondary CPUs back, before the noirq device resume).
 */
static void t6040_sleep_syscore_resume(void *data)
{
	struct t6040_sleep *s = t6040_sleep;

	if (s && s->returned && READ_ONCE(pm_bringup_dev_mark))
		t6040_ret_mark(s, 0x44);
}

static const struct syscore_ops t6040_sleep_syscore_ops = {
	.resume = t6040_sleep_syscore_resume,
};

static struct syscore t6040_sleep_syscore = {
	.ops = &t6040_sleep_syscore_ops,
};

static void t6040_sleep_wake(void)
{
	struct t6040_sleep *s = t6040_sleep;

	if (s && s->returned && READ_ONCE(pm_bringup_dev_mark))
		t6040_ret_mark(s, 0x45);
}

static void t6040_sleep_end(void)
{
	struct t6040_sleep *s = t6040_sleep;
	unsigned int dry = READ_ONCE(t6040_sleep_dry);

	t6040_sleep_clear_wd2(s);
	WRITE_ONCE(pm_bringup_dev_mark, NULL);
	/* Any return or refusal after .begin: the wake alarm back where the RTC driver set it. */
	if (s->alarm_ok) {
		u32 v = s->alarm_saved;

		t6040_pmu_cmd(s, SPMI_CMD_PMU(PMU_RTC_COMPARE, SPMI_OP_WRITE4), &v, NULL, 0);
		s->alarm_ok = false;
	}
	if (dry)
		pr_info("t6040-sleep: dry run %u over, WD2 off\n", dry);
	WRITE_ONCE(t6040_sleep_dry, 0);
}

static const struct platform_suspend_ops t6040_sleep_ops = {
	.valid = t6040_sleep_valid,
	.begin = t6040_sleep_begin,
	.prepare = t6040_sleep_prepare,
	.prepare_late = t6040_sleep_prepare_late,
	.enter = t6040_sleep_enter,
	.wake = t6040_sleep_wake,
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

/* A tunable table on the apple,apcie node, named by its matching reg-name. */
static struct apple_tunable *t6040_apcie_tunable(struct t6040_sleep *s, struct device_node *np,
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

/* A control window on the apple,apcie node, with its optional tunable. */
static void __iomem *t6040_apcie_block(struct t6040_sleep *s, struct device_node *np,
				       const char *name, const char *prop,
				       struct apple_tunable **tunable)
{
	int idx = of_property_match_string(np, "reg-names", name);

	if (idx < 0)
		return NULL;
	if (prop) {
		*tunable = t6040_apcie_tunable(s, np, name, prop);
		if (!*tunable)
			return NULL;
	}

	return of_iomap(np, idx);
}

/*
 * ANS's storage-apcie windows + PLL/lane tunables, from the apple,apcie node
 * the boot object publishes from the ADT. Our static tree has none
 * of these, so until aura fills the node @valid stays false and a real sleep
 * with ANS stopped refuses (t6040_sleep_refuse). The reg-names and tunable
 * property names match aurora #1208's apple,apcie shape exactly, so the same
 * loader fills both SoCs. The GP PCIe host (pcie@1cb0000000, Wi-Fi + SD) is a
 * different node and is left to cold-recover.
 */
static void t6040_sleep_map_apcie(struct t6040_sleep *s)
{
	struct t6040_apcie_st *a = &s->apcie_st;
	struct device_node *np = of_parse_phandle(s->dev->of_node, "apple,apcie", 0);

	if (!np)
		return;
	a->axi = t6040_apcie_block(s, np, "axi2af", "apple,tunable-axi2af", &a->axi_tunable);
	a->cio3pll = t6040_apcie_block(s, np, "cio3pll", "apple,tunable-cio3pll",
				       &a->cio3pll_tunable);
	a->clkgen = t6040_apcie_block(s, np, "clkgen", "apple,tunable-clkgen", &a->clkgen_tunable);
	a->phy = t6040_apcie_block(s, np, "phy-block", "apple,tunable-phy", &a->phy_tunable);
	a->rc = t6040_apcie_block(s, np, "rc", NULL, NULL);
	a->phy_ip = t6040_apcie_block(s, np, "phy-ip", "apple,tunable-phy-ip-pll", &a->phy_ip_pll);
	a->phy_ip_lanes = t6040_apcie_tunable(s, np, "phy-ip", "apple,tunable-phy-ip-auspma");
	of_node_put(np);

	a->valid = a->axi && a->cio3pll && a->clkgen && a->phy && a->rc && a->phy_ip &&
		   a->phy_ip_lanes;
	if (!a->valid)
		dev_info(s->dev,
			 "no storage-apcie windows/tunables (apple,apcie): a real sleep with ANS stopped refuses\n");
}

/* Maps the mailbox at @pa if it is aura's, or returns NULL. */
static struct t6040_s2r_mailbox *t6040_sleep_mailbox_at(struct t6040_sleep *s, u64 pa)
{
	struct t6040_s2r_mailbox *mbox;

	mbox = devm_memremap(s->dev, pa, sizeof(*mbox), MEMREMAP_WB);
	if (IS_ERR(mbox))
		return NULL;
	if (mbox->magic != T6040_S2R_MAGIC) {
		dev_warn(s->dev, "s2r-mailbox magic %#llx is not AURAS2R1\n", mbox->magic);
		devm_memunmap(s->dev, mbox);
		return NULL;
	}
	return mbox;
}

/* The aura #117 mailbox, if the boot object published one. Optional (a blocker if absent). */
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
	struct t6040_s2r_mailbox *mbox = NULL;
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

static const struct dev_pm_ops t6040_sleep_pm_ops = {
#ifdef CONFIG_HIBERNATION
	.restore = t6040_sleep_restore,
#endif
};

/*
 * Bring-up, root only: write one value to the internal display's CPU (pmgr_disp +0) or front
 * end (soc_east +0x368) power word at runtime, to tell a write that is refused from a state
 * change that is. "cpu [value]" or "fe [value]", no value: the word's own. Marks 0x567S00RR around the write.
 */
static ssize_t t6040_ps_write_dbg(struct file *f, const char __user *ubuf, size_t len,
				  loff_t *ppos)
{
	struct t6040_sleep *s = file_inode(f)->i_private;
	char buf[48], which[8];
	u32 val = 0, before = 0, after = 0;
	unsigned int i;
	int n;

	if (len >= sizeof(buf))
		return -EINVAL;
	if (copy_from_user(buf, ubuf, len))
		return -EFAULT;
	buf[len] = 0;
	n = sscanf(buf, "%7s %x", which, &val);
	if (n < 1)
		return -EINVAL;
	if (!strcmp(which, "cpu")) {
		for (i = 0; i < s->ngroups; i++) {
			const struct t6040_sleep_group *g = &s->groups[i];

			if (strcmp(g->name, "disp"))
				continue;
			regmap_read(g->regmap, g->first, &before);
			if (n < 2)
				val = before;
			if (s->spmi0)
				t6040_mark_save(s);
			t6040_disp_mark(s, 0x70, 0x01);
			regmap_write(g->regmap, g->first, val);
			udelay(50);
			regmap_read(g->regmap, g->first, &after);
			t6040_disp_mark(s, 0x70, 0x02);
		}
	} else if (!strcmp(which, "fe") && s->soc_east) {
		before = readl(s->soc_east + PS_DISP_FE);
		if (n < 2)
			val = before;
		if (s->spmi0)
			t6040_mark_save(s);
		t6040_disp_mark(s, 0x71, 0x01);
		writel(val, s->soc_east + PS_DISP_FE);
		udelay(50);
		after = readl(s->soc_east + PS_DISP_FE);
		t6040_disp_mark(s, 0x71, 0x02);
	} else {
		return -EINVAL;
	}
	pr_info("t6040-sleep: ps test %s: %#x written over %#x, now %#x\n", which, val, before, after);
	return len;
}

static const struct file_operations t6040_ps_write_fops = {
	.owner = THIS_MODULE,
	.write = t6040_ps_write_dbg,
	.open = simple_open,
	.llseek = noop_llseek,
};

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
	dev_info(s->dev, "GP PCIe: ports %d/%d phys %d/%d, PERST# pins %d/%d\n",
		 !!s->gp_port[0], !!s->gp_port[1], !!s->gp_phy[0], !!s->gp_phy[1],
		 s->gp_perst[0], s->gp_perst[1]);
out:
	of_node_put(host);
}

static int t6040_sleep_probe(struct platform_device *pdev)
{
	struct t6040_sleep *sleep;
	int ret, i;

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
		dev_info(&pdev->dev, "wake stub (WD2 60 s before cpu_resume): %s\n",
			 sleep->wdt_stub_ok ? "ready" : "no, wdt block is not the AON WDT");
	}
	sleep->cluster_ctl = t6040_sleep_map(&pdev->dev, "cluster-ctl");
	sleep->pwrgate = t6040_sleep_map(&pdev->dev, "soc-pwrgate");
	sleep->clkgen = t6040_sleep_map(&pdev->dev, "clkgen");
	sleep->spmi0 = t6040_sleep_map(&pdev->dev, "spmi0");
	if (sleep->spmi0) {
		u32 rx[2];

		if (!t6040_pmu_cmd(sleep, SPMI_CMD_PMU(PMU_RTC_COMPARE, SPMI_OP_READ4), NULL, rx, 2))
			dev_info(&pdev->dev, "PMU comparator at probe: %#x (reply %#x)%s\n", rx[1], rx[0],
				 (rx[1] & 0xffff0000) == T6040_MARK(0) ?
				 ": the previous boot's last S2R mark" : "");
	}
	sleep->spmi_a0 = t6040_sleep_map(&pdev->dev, "spmi-a0");
	sleep->mastermini = t6040_sleep_map(&pdev->dev, "mastermini");
	sleep->a2d_mini = t6040_ioremap_np(&pdev->dev, 0x508360000ULL, 0x1000);
	sleep->a2d_mini4 = t6040_ioremap_np(&pdev->dev, 0x508364000ULL, 0x1000);
	sleep->a2d_mm8 = t6040_ioremap_np(&pdev->dev, 0x508908000ULL, 0x1000);
	sleep->a2d_pmc = t6040_ioremap_np(&pdev->dev, 0x5048aa000ULL, 0x1000);
	sleep->a2d_blk = t6040_ioremap_np(&pdev->dev, 0x502850000ULL, 0x1000);
	sleep->aop_req = t6040_ioremap_np(&pdev->dev, 0x5108b8000ULL, 0x1000);
	sleep->a2d_mm12 = t6040_ioremap_np(&pdev->dev, 0x50890c000ULL, 0x1000);
	sleep->mtp_ps = t6040_ioremap_np(&pdev->dev, T6040_MTP_PS_PA, 8 * T6040_MTP_PS_WORDS);
	sleep->aop_ps = t6040_ioremap_np(&pdev->dev, T6040_AOP_PS_PA, 0x160);
	sleep->snap_soc = t6040_ioremap_np(&pdev->dev, 0x502800000ULL, 0x9000);
	sleep->atc_aon = t6040_ioremap_np(&pdev->dev, 0x508320c00ULL, 0x200);
	for (i = 0; i < 16; i++)
		sleep->dcs[i] = t6040_ioremap_np(&pdev->dev, 0x220200000ULL +
						 (i / 4) * 0x2000000ULL + (i % 4) * 0x200000ULL,
						 0x2000);
	for (i = 0; i < 4; i++)
		sleep->amcc_sem[i] = t6040_ioremap_np(&pdev->dev,
						      0x2201c0000ULL + i * 0x2000000ULL, 0x1000);
	sleep->pg_main = t6040_ioremap_np(&pdev->dev, 0x50282c000ULL, 0x1000);
	sleep->aop_asc = t6040_ioremap_np(&pdev->dev, 0x510600000ULL, 0x100);
	for (i = 0; i < 4; i++)
		sleep->atc_cfg[i] = t6040_ioremap_np(&pdev->dev,
						     0x383200000ULL + i * 0x8000000ULL, 0x100);
	sleep->pg_east = t6040_ioremap_np(&pdev->dev, 0x5022a0000ULL, 0x1000);
	sleep->mini_ps = t6040_ioremap_np(&pdev->dev, 0x508300000ULL, 0x200);
	sleep->snap_east = t6040_ioremap_np(&pdev->dev, 0x502280000ULL, 0xd000);
	for (i = 0; i < ARRAY_SIZE(t6040_pmc_probe_pa); i++)
		sleep->pmc_probe_io[i] = t6040_ioremap_np(&pdev->dev, t6040_pmc_probe_pa[i], 4);
	for (i = 0; i < 3; i++) {
		sleep->cpm_cfg[i] = t6040_ioremap_np(&pdev->dev, T6040_CPM_CFG_PA(i), CPM_CFG_STATE + 8);
		sleep->cpm_impl[i] = t6040_ioremap_np(&pdev->dev, T6040_CPM_IMPL_PA(i), 8);
	}
	sleep->cpu4_impl110 = t6040_ioremap_np(&pdev->dev, T6040_CPU4_IMPL110_PA, 8);
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
	sleep->pmc_dvmr = t6040_ioremap_np(&pdev->dev, T6040_PMC_DVMR_PA, 0x40);
	sleep->pmc_bwr = t6040_ioremap_np(&pdev->dev, T6040_PMC_BWR_PA, 0x10);
	sleep->dispext_pwrgate = t6040_ioremap_np(&pdev->dev, T6040_DISPEXT_PWRGATE_PA, 0x1000);
	for (i = 0; i < ARRAY_SIZE(t6040_dcp_asc_ctl_pa); i++)
		sleep->dcp_asc_ctl[i] = t6040_ioremap_np(&pdev->dev, t6040_dcp_asc_ctl_pa[i], 4);
	sleep->ptd = t6040_ioremap_np(&pdev->dev, T6040_PTD_PA, 0x11000);
	/* The DART clock-protection (GAPF) block, read-only here. */
	sleep->gapf = t6040_ioremap_np(&pdev->dev, 0x500578000ULL, 0x1000);
	sleep->ka_soc = t6040_ioremap_np(&pdev->dev, T6040_KA_SOC_PA, KA_SIZE);
	sleep->ka_east = t6040_ioremap_np(&pdev->dev, T6040_KA_EAST_PA, KA_SIZE);
	sleep->smc_wdt_deadline = t6040_ioremap_np(&pdev->dev, T6040_SMC_WDT_DEADLINE_PA, 4);
	sleep->agx_gate = t6040_ioremap_np(&pdev->dev, T6040_AGX_GATE_PA, 4);
	sleep->ane_sys_ps = t6040_ioremap_np(&pdev->dev, T6040_ANE_SYS_PS_PA, 4);
	sleep->ane_ps = t6040_ioremap_np(&pdev->dev, T6040_ANE_PS_PA, 0x20);
	for (i = 0; i < ARRAY_SIZE(t6040_acc_apsc_pa); i++)
		sleep->acc_apsc[i] = t6040_ioremap_np(&pdev->dev, t6040_acc_apsc_pa[i], 8);
	for (i = 0; i < ARRAY_SIZE(t6040_tvm); i++) {
		sleep->tvm_ctl[i] = t6040_ioremap_np(&pdev->dev, t6040_tvm[i].ctl, 4);
		sleep->tvm_status[i] = t6040_ioremap_np(&pdev->dev, t6040_tvm[i].status, 4);
	}
	sleep->pmc_ap_floor = t6040_ioremap_np(&pdev->dev, T6040_PMC_AP_FLOOR_PA, 8);
	sleep->afr_ps = t6040_ioremap_np(&pdev->dev, T6040_AFR_PS_PA, 4);
	if (sleep->ka_soc && sleep->ka_east) {
		u32 soc = 0, east = 0;

		for (i = 0; i < KA_WORDS; i++) {
			soc |= (readl(sleep->ka_soc + i * KA_STRIDE) & 1) << i;
			east |= (readl(sleep->ka_east + i * KA_STRIDE) & 1) << i;
		}
		dev_info(&pdev->dev, "keep-alive bits at probe: SOC %#010x, SOC-East %#010x\n",
			 soc, east);
	}
	t6040_sleep_map_gp(sleep);
	t6040_ed_map(&pdev->dev);

	platform_set_drvdata(pdev, sleep);
	t6040_sleep = sleep;

	sleep->debugfs = debugfs_create_dir("t6040-sleep", NULL);
	debugfs_create_file("preflight", 0400, sleep->debugfs, sleep, &t6040_sleep_preflight_fops);
	debugfs_create_file("ps_write", 0200, sleep->debugfs, sleep, &t6040_ps_write_fops);

	/*
	 * The CPU power ops act only when a commit stops the parked secondaries.
	 * A real entry needs allow_entry; a dry run registers them at its .begin,
	 * for the hold. Until then kexec keeps refusing parking CPUs as before.
	 * Parking itself needs no power ops.
	 */
	if (allow_entry)
		t6040_sleep_power_ops(sleep);

	register_syscore(&t6040_sleep_syscore);
	suspend_set_ops(&t6040_sleep_ops);
	atomic_notifier_chain_register(&panic_notifier_list, &t6040_panic_nb);
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
	t6040_log_ane(sleep, "probe");
	t6040_log_a2d(sleep, "probe");

	return 0;
}

static void t6040_sleep_remove(struct platform_device *pdev)
{
	struct t6040_sleep *sleep = platform_get_drvdata(pdev);

	unregister_syscore(&t6040_sleep_syscore);
	debugfs_remove_recursive(sleep->debugfs);
}

static const struct of_device_id t6040_sleep_of_match[] = {
	{ .compatible = "apple,t6040-sleep" },
	{}
};
MODULE_DEVICE_TABLE(of, t6040_sleep_of_match);

static struct platform_driver t6040_sleep_driver = {
	.driver = {
		.name = "t6040-sleep",
		.of_match_table = t6040_sleep_of_match,
		.pm = pm_sleep_ptr(&t6040_sleep_pm_ops),
		/* The suspend ops can't be taken back. */
		.suppress_bind_attrs = true,
	},
	.probe = t6040_sleep_probe,
	.remove = t6040_sleep_remove,
};
module_platform_driver(t6040_sleep_driver);

MODULE_DESCRIPTION("Apple T6040 system sleep (refusal-first controller)");
MODULE_LICENSE("Dual MIT/GPL");
