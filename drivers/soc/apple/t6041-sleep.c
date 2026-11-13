// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * Apple T6041 (M4 Max) system sleep, at EL2 with no monitor below.
 *
 * It runs macOS 26.6.2's last steps before the S2R commit, in macOS's order and with its
 * values, as traced from macOS sleeping under aura's hypervisor, and commits; aura's S2R
 * mailbox brings the boot core back to cpu_resume on the wake, and the driver restores what
 * the sleep took.
 *
 * Needs the kernel at EL2 (CTXR window) and the "aurora,rvbar-spin-table" CPU enable method,
 * so the other cores can leave the kernel.
 */

#include <clocksource/arm_arch_timer.h>
#include <linux/bitfield.h>
#include <linux/cacheflush.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/irqchip/irq-apple-aic.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/soc/apple/sleep.h>
#include <linux/suspend.h>
#include <asm/arch_timer.h>
#include <asm/cpu_ops.h>
#include <asm/cputype.h>
#include <asm/suspend.h>
#include <asm/sysreg.h>
#include <asm/virt.h>

#define SYS_APL_IPI_SR_EL1	sys_reg(3, 5, 15, 1, 1)
#define IPI_SR_PENDING		BIT(0)
#define SYS_APL_SIQ_CFG_EL1	sys_reg(3, 4, 15, 10, 4)
#define SYS_APL_DEEP_SLEEP	sys_reg(3, 5, 15, 6, 2)
#define DEEP_SLEEP_BITS		(BIT(0) | BIT(63))

/* Physical blocks (ADT /arm-io/pmgr reg entries on J616C). */
#define PA_PS		0x502800000UL	/* pmgr[37] and on: PS words */
#define PA_EAST		0x502280000UL	/* pmgr[2]: East PS words */
#define PA_CLUSTER	0x502820000UL	/* pmgr[44] */
#define PA_PWRGATE	0x50282c000UL	/* pmgr[38] */
#define PA_MISC_CORES	0x502888000UL	/* pmgr[40] + 0x8000 */
#define PA_WDT		0x50836c000UL
#define PA_AON_MISC	0x508330000UL
#define PA_COMMIT	0x508904000UL	/* pmgr[55] + 0x4000 */
#define PA_CLKGEN	0x500038000UL
#define PA_SPMI0	0x509014000UL	/* nub-spmi0 */
#define PA_KA_PMGR	0x5029dc000UL	/* pmgr[0] + 0x1dc000: keep-alive table */
#define PA_KA_EAST	0x5022d0000UL	/* pmgr[2] + 0x50000: keep-alive table */
#define KA_WORDS	32
#define KA_STRIDE	0x44

#define PS_AUTO_ENABLE	BIT(28)
#define PS_FLAGS	GENMASK(9, 8)
#define PS_ACTUAL	GENMASK(7, 4)
#define PS_TARGET	GENMASK(3, 0)

#define SPMI_STATUS		0x0
#define SPMI_CMD		0x4
#define SPMI_RX_EMPTY		BIT(24)
#define SPMI_NOTIFY_MASK	0x40
#define SPMI_IRQ_ACK		0x80
#define PMU_SID			0xb

static const u64 t6041_macos_off[] = {
	0x508300168,
	0x508300160,
	0x508300158,
	0x508300148,
	0x508300140,
	0x508300120,
	0x5083000e0,
	0x5083000d8,
	0x5083000d0,
	0x5083000b8,
	0x5083000b0,
	0x5083000a8,
	0x508300098,
	0x508300080,
	0x502808020,
	0x502808018,
	0x502808010,
	0x502808008,
	0x502808000,
	0x502800478,
	0x502800470,
	0x502800468,
	0x502800458,
	0x502800438,
	0x502800430,
	0x502800428,
	0x502800420,
	0x502800408,
	0x502800400,
	0x5028003f8,
	0x5028003f0,
	0x5028003e8,
	0x5028003e0,
	0x5028003d8,
	0x5028003d0,
	0x5028003c8,
	0x5028003c0,
	0x5028003b8,
	0x5028003b0,
	0x5028003a8,
	0x5028003a0,
	0x502800398,
	0x502800330,
	0x502800328,
	0x5028002c8,
	0x5028002c0,
	0x5028002b8,
	0x5028002b0,
	0x5028002a8,
	0x5028002a0,
	0x502800278,
	0x502800190,
	0x502800170,
	0x502800160,
	0x502800158,
	0x502800150,
	0x502800148,
	0x502800140,
	0x502800138,
	0x50228c018,
	0x50228c010,
	0x50228c008,
	0x50228c000,
	0x5022803c0,
	0x5022803b8,
	0x5022803b0,
	0x5022803a8,
	0x5022803a0,
	0x502280398,
	0x502280390,
	0x502280388,
	0x502280380,
	0x502280378,
	0x502280370,
	0x502280250,
	0x502280248,
	0x502280240,
	0x502280238,
	0x502280230,
	0x5022801e8,
	0x5022801e0,
	0x5022801d8,
	0x5022801d0,
	0x5022801c8,
	0x5022801c0,
	0x5022801b8,
	0x502280190,
	0x502280188,
	0x502280180,
	0x502280178,
	0x502280170,
	0x502280168,
	0x502280160,
	0x502280158,
	0x502280150,
	0x502280148,
	0x502280140,
	0x502280138,
	0x502280110,
	0x502280108,
	0x502280100,
	0x502280000,
	0x288e80110,
	0x288e80100,
};

/* aura's struct aura_s2r_mailbox (aura PR 117). */
#define T6041_S2R_MAGIC		0x3152325341525541ULL	/* "AURAS2R1" */
struct t6041_s2r_mailbox {
	u64 magic;
	u64 boot_mpidr;
	u64 resume_pa;
	u64 resumes;
};

struct t6041_sleep {
	struct device *dev;
	struct t6041_s2r_mailbox *mbox;
	void __iomem *ps, *east, *cluster, *pwrgate, *misc, *wdt, *aon, *commit, *clkgen, *spmi;
	void __iomem *off[ARRAY_SIZE(t6041_macos_off)];
	void __iomem *ps_aon, *pmc;
	void __iomem *ka[2];	/* keep-alive tables, 1 from iBoot, 0 at macOS's commit */
	void __iomem *spmi_a0;	/* 0x509188000: nub-spmi-a0, notify mask written by macOS at -113 ms */
	unsigned int ngroups;	/* AMCCs of four planes: 8 on the full T6041, 6 on the binned one */
	void __iomem *amcc[32];
	/* DCS channel blocks (ADT mcc reg 20+): 0x220200000 + 0x2000000 * g + 0x200000 * c */
	void __iomem *dcs[32];
	/* AMCC blocks (ADT mcc reg 12+): 0x220000000 + 0x2000000 * g, planes at + 0x40000 * p */
	void __iomem *amccb[8];
	void __iomem *wake;	/* 0x508004000: SLP_DDR +0, wake masks +0x0c..+0x2c */
	void __iomem *perf;	/* 0x50020c000: SoC perf +0, DCS perf +0x400 */
	void __iomem *axi2af[15];
	void __iomem *ptd;	/* 0x5003d0000: the PMP's PTD update page (ADT mcc[11]) */
	int stop_ret;
	int coproc_ret;
	int apcie_ret;
	unsigned int ps_late, gp_late;
	/* Saved as the commit starts, put back by t6041_sleep_return(). */
	u64 cntkctl, siq_cfg;
	u32 pre_perf[2], pre_pmc[5], pre_clk[4], pre_mask[2], pre_wake[9], pre_votes[5];
	/* Each AMCC plane's cache ways (+0x2800) and status (+0x2804). */
	u32 pre_ways[32], pre_cache[32];
	u32 full_pre[15];	/* t6041_full_ps as the commit starts */
	bool pre_a0;
	/* GP PCIe ports 0/1: port block, PHY word, and what they held as the commit started. */
	void __iomem *gp_port[2], *gp_phy[2];
	u32 gp_phy_pre[2], gp_app_pre[2], gp_perst_pre[2];
	bool gp_saved;
	/* ANS's storage PCIe block (ADT /arm-io/apcie0 reg 1-6). */
	void __iomem *ap_rc, *ap_phy, *ap_phy_ip, *ap_axi, *ap_cio3pll, *ap_clkgen;
	bool ans_stopped;
	/* The AOP island's 44 MiniPMGR power words, 0x508304000 + 8 n. */
	void __iomem *aop_ps;
	u32 aop_pre[44];
	/* The AP GPIO block's 200 pin words (ADT gpio0, #gpio-pins 0xc8), as the commit starts. */
	void __iomem *gpio;
	u32 gpio_pre[200];
};

static struct t6041_sleep *t6041_sleep;

#include "t6041-macos-regs.h"
#include "t6041-ps-order.h"
#include "t6041-apcie-tunables.h"
/* Each register of the macOS commit-state table, mapped. */
static void __iomem *t6041_reg_map[ARRAY_SIZE(t6041_macos_regs)];

/*
 * The full T6041 has eight AMCCs and 32 memory channels, the binned one six and 24, and the
 * macOS commit table above is from a binned J616c. These are the PS words of the other eight
 * channels (named from a full J614c's ADT; on the binned part they read target and actual 0),
 * each with the value macOS has for the same kind of word: an AMCC slice 0x1000030f (AUTO,
 * auto state 0), a DCS channel 0x1400034f (auto state 4). Left forced on, they keep fab3_soc
 * (East +0x118) on at the commit. dcs_27 isn't among the words the J614c has on there.
 */
static const struct { u8 east; u16 off; u32 val; } t6041_full_ps[15] = {
	{ 0, 0x248, 0x1000030f },	/* amcc24 */
	{ 0, 0x250, 0x1400034f },	/* dcs_24 */
	{ 0, 0x258, 0x1000030f },	/* amcc25 */
	{ 0, 0x260, 0x1400034f },	/* dcs_25 */
	{ 0, 0x268, 0x1000030f },	/* amcc26 */
	{ 0, 0x270, 0x1400034f },	/* dcs_26 */
	{ 0, 0x280, 0x1000030f },	/* amcc27 */
	{ 1, 0x2c0, 0x1000030f },	/* amcc28 */
	{ 1, 0x2c8, 0x1000030f },	/* amcc29 */
	{ 1, 0x2d0, 0x1000030f },	/* amcc30 */
	{ 1, 0x2d8, 0x1000030f },	/* amcc31 */
	{ 1, 0x340, 0x1400034f },	/* dcs_28 */
	{ 1, 0x348, 0x1400034f },	/* dcs_29 */
	{ 1, 0x350, 0x1400034f },	/* dcs_30 */
	{ 1, 0x358, 0x1400034f },	/* dcs_31 */
};

static void __iomem *t6041_full_reg(struct t6041_sleep *s, unsigned int i)
{
	return (t6041_full_ps[i].east ? s->east : s->ps) + t6041_full_ps[i].off;
}

/* On the full part only: the extra channels' words to their macOS kind, as t6041_ps_match_all(). */
static void t6041_full_ps_match(struct t6041_sleep *s)
{
	const u32 mask = PS_TARGET | PS_AUTO_ENABLE | GENMASK(27, 24);
	unsigned int i;

	if (s->ngroups != 8)
		return;
	for (i = 0; i < ARRAY_SIZE(t6041_full_ps); i++) {
		void __iomem *reg = t6041_full_reg(s, i);
		u32 now = readl(reg), mac = t6041_full_ps[i].val;

		if ((now & mask) != (mac & mask))
			writel((now & ~mask) | (mac & mask), reg);
	}
}

/* ADT cpu number: E 0-3, P0 4-8, P1 10-14. */
static unsigned int t6041_cpu_id(u64 mpidr)
{
	unsigned int cl = MPIDR_AFFINITY_LEVEL(mpidr, 1), core = MPIDR_AFFINITY_LEVEL(mpidr, 0);

	return cl == 0 ? core : cl == 1 ? 4 + core : 10 + core;
}

/*
 * A core's last WFI, as the Neo's and the Air's t8140_deep_wfi() and macOS's last-WFI routine
 * with x0 = 1: timers masked, the deep-sleep bits (C6_2 bit 0 and 63) set, then WFI for good.
 * (Not the runtime offline branch, which leaves C6_2 clear.)
 */
static void __noreturn t6041_offline_wfi(void)
{
	write_sysreg(ARCH_TIMER_CTRL_IT_MASK, cntp_ctl_el0);
	write_sysreg(ARCH_TIMER_CTRL_IT_MASK, cntv_ctl_el0);
	sysreg_clear_set_s(SYS_APL_DEEP_SLEEP, 0, DEEP_SLEEP_BITS);
	isb();
	while (true) {
		dsb(sy);
		isb();
		wfi();
		aic_sleep_ack();
		write_sysreg_s(IPI_SR_PENDING, SYS_APL_IPI_SR_EL1);
	}
}

static void t6041_cpu_stop(u64 mpidr)
{
	writel(BIT(t6041_cpu_id(mpidr)), t6041_sleep->misc + 0x0);
	dsb(sy);
}

static int t6041_cpu_on(u64 mpidr)
{
	void __iomem *misc = t6041_sleep->misc;
	unsigned int cl = MPIDR_AFFINITY_LEVEL(mpidr, 1);

	writel(BIT(t6041_cpu_id(mpidr)), misc + 0x4);
	writel(BIT(MPIDR_AFFINITY_LEVEL(mpidr, 0)), misc + 0x8 + 4 * cl);
	return 0;
}


static void t6041_ps_lower(void __iomem *reg)
{
	u32 val = readl(reg);

	if (val & PS_AUTO_ENABLE) {
		val &= ~(PS_AUTO_ENABLE | PS_FLAGS);
		writel(val, reg);
	}
	writel((val & ~PS_TARGET) | PS_FLAGS, reg);
}

/* One byte to a PMU register over nub-spmi0, as XNU's writes: command, then the data. */
static void t6041_pmu_write(struct t6041_sleep *s, u16 reg, u8 val)
{
	unsigned int us;

	/* A reply someone else left (aura's own writes at the wake), so the next one is ours. */
	while (!(readl(s->spmi + SPMI_STATUS) & SPMI_RX_EMPTY))
		readl(s->spmi + 0x8);
	writel((u32)reg << 16 | BIT(15) | PMU_SID << 8 | 0x30, s->spmi + SPMI_CMD);
	writel(val, s->spmi + SPMI_CMD);
	/*
	 * Take the reply once it is there. The controller stays powered through the sleep, so a
	 * reply that comes in after we stop looking becomes the reply to the SPMI driver's next
	 * command after the wake, which then fails as an unexpected response.
	 */
	for (us = 0; us < 1000 && (readl(s->spmi + SPMI_STATUS) & SPMI_RX_EMPTY); us++)
		udelay(1);
	while (!(readl(s->spmi + SPMI_STATUS) & SPMI_RX_EMPTY))
		readl(s->spmi + 0x8);
	/* macOS acknowledges every command here (its interrupt handler writes 1 to +0x80). */
	writel(1, s->spmi + SPMI_IRQ_ACK);
}

static void east(struct t6041_sleep *s, u32 off, u32 v)
{
	writel(v, s->east + off);
}

static void cpm_gates(struct t6041_sleep *s)
{
	writel(0x80000000, s->pwrgate + 0x100);
	writel(0x80000000, s->pwrgate + 0x108);
	writel(0x80000000, s->pwrgate + 0x110);
}

/*
 * The core and CPM words macOS has on at its commit (0x1f0: on, target 0, so they drop when
 * the core idles). Linux runs the cores with them reading off, so nothing ever drops.
 */
static const u16 t6041_cpu_ps[] = {
	0x00, 0x08, 0x10, 0x18,				/* ecpu 0-3 */
	0x20, 0x28, 0x30, 0x38, 0x40,			/* pcpu0 0-4 */
	0x50, 0x58, 0x60, 0x68, 0x70,			/* pcpu1 0-4 */
	0x80, 0x88,					/* ecpm, pcpm0 */
};

/*
 * Every named PS word to macOS's target, AUTO_ENABLE and auto state at its commit, as the Neo's match
 * step does: macOS leaves half the DCS channels and their AMCC slices to the hardware.
 * Returns how many words changed.
 */
static int t6041_ps_match_all(void)
{
	unsigned int i;
	int n = 0;

	for (i = 0; i < ARRAY_SIZE(t6041_macos_regs); i++) {
		/*
		 * With the auto state [27:24], the state AUTO_ENABLE drops an idle domain to:
		 * macOS has 0 (off) for half the AMCC slices and dcs_23, 4 for fab3_soc, where
		 * Linux kept iBoot's 4 and f (AVE test, 9 Nov 19:24).
		 */
		u32 mask = PS_TARGET | PS_AUTO_ENABLE | GENMASK(27, 24), mac = t6041_macos_regs[i].val, now;

		if (!t6041_macos_regs[i].ps)
			continue;
		now = readl(t6041_reg_map[i]);
		if ((now & mask) == (mac & mask))
			continue;
		writel((now & ~mask) | (mac & mask), t6041_reg_map[i]);
		n++;
	}
	return n;
}

static void t6041_spin_s(u64 ms)
{
	u64 t0 = __arch_counter_get_cntpct(), f = arch_timer_get_cntfrq();

	while (__arch_counter_get_cntpct() - t0 < ms * f / 1000)
		cpu_relax();
}

static void t6041_cpu_off(u64 mpidr)
{
	t6041_offline_wfi();
}

static const struct spin_table_power_ops t6041_cpu_power_ops = {
	.cpu_stop = t6041_cpu_stop,
	.cpu_off = t6041_cpu_off,
	.cpu_on = t6041_cpu_on,
};

static void __noreturn t6041_boot_wfi(struct t6041_sleep *s)
{
	write_sysreg(ARCH_TIMER_CTRL_IT_MASK, cntp_ctl_el0);
	write_sysreg(ARCH_TIMER_CTRL_IT_MASK, cntv_ctl_el0);
	sysreg_clear_set_s(SYS_APL_DEEP_SLEEP, 0, DEEP_SLEEP_BITS);
	while (aic_sleep_ack())
		;
	while (true) {
		dsb(sy);
		isb();
		wfi();
		aic_sleep_ack();
		write_sysreg_s(IPI_SR_PENDING, SYS_APL_IPI_SR_EL1);
	}
}


/* Raise each to on, then drop its target as macOS has it. Returns how many read on. */
static int t6041_cpu_ps_match(struct t6041_sleep *s)
{
	unsigned int i;
	int up = 0;
	u32 v;

	for (i = 0; i < ARRAY_SIZE(t6041_cpu_ps); i++) {
		void __iomem *reg = s->ps + t6041_cpu_ps[i];

		v = readl(reg);
		writel((v & ~(PS_TARGET | PS_AUTO_ENABLE)) | 0xf, reg);
		if (!readl_poll_timeout_atomic(reg, v, FIELD_GET(PS_ACTUAL, v) == 0xf, 1, 2000))
			up++;
	}
	for (i = 0; i < ARRAY_SIZE(t6041_cpu_ps); i++) {
		void __iomem *reg = s->ps + t6041_cpu_ps[i];

		writel(readl(reg) & ~PS_TARGET, reg);
	}
	return up;
}

/*
 * Every USB-C port's power domains, deepest first, then the fabric above them. The macOS
 * trace these states were matched to had aura holding ATC0's chain on for its debug link
 * (dj: on the Neo, macOS only slept once aura dropped that link). PS offsets: pmgr2
 * (0x502800000) for the commons and CIO/PCIe, pmgr3 AON (0x508300000) for the rest.
 */
static const u16 t6041_atc_aon[] = {
	0x150, 0x158, 0x160, 0x168,	/* atcN_phymxwrap */
	0x118, 0x120, 0x140, 0x148,	/* atcN_usb */
	0x0c8, 0x0d0, 0x0d8, 0x0e0,	/* atcN_usb_aon */
	0x0a0, 0x0a8, 0x0b0, 0x0b8,	/* atcN_common_dp */
};
static const u16 t6041_atc_soc[] = {
	0x3b0, 0x3a8, 0x3a0, 0x398,	/* atc0 cio_usb, cio_pcie, pcie, cio */
	0x3d8, 0x3d0, 0x3c8, 0x3c0,	/* atc1 */
	0x400, 0x3f8, 0x3f0, 0x3e8,	/* atc2 */
	0x438, 0x430, 0x428, 0x420,	/* atc3 */
	0x390, 0x3b8, 0x3e0, 0x408,	/* atcN_common */
};
/* fab2_soc and afi: forcing them off hung the Max (8 Nov). Left to the hardware instead. */
static const u16 t6041_fab_auto[] = { 0x338, 0x110 };

static void t6041_atc_down(struct t6041_sleep *s)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(t6041_atc_aon); i++)
		t6041_ps_lower(s->ps_aon + t6041_atc_aon[i]);
	for (i = 0; i < ARRAY_SIZE(t6041_atc_soc); i++)
		t6041_ps_lower(s->ps + t6041_atc_soc[i]);
	udelay(2000);
	for (i = 0; i < ARRAY_SIZE(t6041_fab_auto); i++) {
		void __iomem *reg = s->ps + t6041_fab_auto[i];

		writel(readl(reg) | PS_AUTO_ENABLE, reg);
	}
}

/*
 * The M4 Pro's fix for the same stall (linux#1477, t6040-sleep fab_on bit 2): the engine's ATC
 * step raises each atcN_common and polls ACTUAL f; with every common off, their parent fab2_soc
 * is auto-gated and the common never comes up. So right before the commit: fab2_soc on by hand
 * (AUTO off, target f), the four commons on (target f, no AUTO), then fab2_soc's AUTO back; the
 * powered commons hold it up.
 */
static int t6041_ps_raise_start(void __iomem *reg)
{
	u32 v;

	writel((readl(reg) & ~(PS_AUTO_ENABLE | GENMASK(11, 0))) | PS_TARGET, reg);
	return readl_poll_timeout_atomic(reg, v, (v & PS_ACTUAL) == PS_ACTUAL, 1, 5000);
}

static void t6041_atc_commons_on(struct t6041_sleep *s)
{
	static const u16 common[4] = { 0x390, 0x3b8, 0x3e0, 0x408 };
	void __iomem *fab2 = s->ps + 0x338;
	int i;

	t6041_ps_raise_start(fab2);
	for (i = 0; i < 4; i++)
		t6041_ps_raise_start(s->ps + common[i]);
	writel((readl(fab2) & ~(PS_AUTO_ENABLE | GENMASK(11, 0))) | PS_TARGET | PS_AUTO_ENABLE, fab2);
}

/*
 * The Neo's step just before its commit (dj: "the last stall"): on every AMCC plane, the
 * DSID age-out (word 0 bit 25) and unexpected RT hit (word 1 bit 8) error enables off. aura's
 * boot leaves an unexpected RT hit latched in INTSTS1 (0x100) on the Max's planes; those are
 * written back to clear them too.
 */
static void t6041_amcc_quiesce(struct t6041_sleep *s)
{
	int p, w;

	for (p = 0; p < 4 * s->ngroups; p++) {
		void __iomem *a = s->amcc[p];

		writel(readl(a + 0x0) & ~BIT(25), a + 0x0);
		writel(readl(a + 0x8) & ~BIT(8), a + 0x8);
		for (w = 0; w < 7; w++) {
			u32 v = readl(a + 4 + 8 * w);

			if (v)
				writel(v, a + 4 + 8 * w);
		}
	}
	dsb(sy);
}

/*
 * What macOS's memory controller driver does and the EL2 path never did (kernelcache 26.6.2,
 * AppleT6041MemCacheController and AppleT6041SOCTuner; neither is in a trace, the first is
 * done at boot and the second in the quiesce actions after the watchdog arm):
 *  - mccSelectDCSDynamicPowerGateMode(1), from the tuner at boot (ADT mcc-power-gating = 1):
 *    +0x20 bits 8 and 12 set on every enabled DCS channel block.
 *  - cacheFlush, the driver's quiesce action: per AMCC +0x82c = 1 (allocation off) and
 *    +0x2844 = 1 (flush), then +0x2844 bit 0 polled clear on each of its four planes.
 * macOS has every amcc slice power-gated at its commit (0x1000030f); here they only
 * clock-gated (0x1400024f), and the engine stalls in its memory cache program.
 * Run while the slices are still forced on.
 */
static void t6041_mcc_macos(struct t6041_sleep *s)
{
	unsigned int i, p;
	u32 v;

	for (i = 0; i < s->ngroups; i++) {
		/* +0x1c0000 is the AMCC's all-planes window: macOS writes there, polls per plane. */
		writel(1, s->amccb[i] + 0x1c082c);
		writel(1, s->amccb[i] + 0x1c2844);
		for (p = 0; p < 4; p++)
			readl_poll_timeout_atomic(s->amccb[i] + 0x40000 * p + 0x2844, v,
						  !(v & BIT(0)), 10, 100000);
	}
	dsb(sy);
	for (i = 0; i < 4 * s->ngroups; i++)
		writel(readl(s->dcs[i] + 0x20) | 0x1100, s->dcs[i] + 0x20);
	dsb(sy);
}

/*
 * AppleT6041PMGR's AXI2AF workaround (macOS 26.6.2, the table at the kernelcache's _DATA and the
 * routine that writes it): 15 fabric bridges, each with the PS word of its block. Before a sleep,
 * every bridge whose block is on gets bit 0 set in its CPG node (+0x104) and link teardown
 * (+0x108) words, so the fabric sees the link down; macOS asserts all 15 blocks are off by then.
 * Linux powers these blocks off as plain PS words, never tearing the link down.
 * pmgr: 0 = 0x502800000, 2 = 0x502280000.
 */
static const struct { u64 base; u8 pmgr; u16 ps; } t6041_axi2af[15] = {
	{ 0x380000000, 0, 0x390 },	/* atc0_common */
	{ 0x388000000, 0, 0x3b8 },	/* atc1_common */
	{ 0x390000000, 0, 0x3e0 },	/* atc2_common */
	{ 0x398000000, 0, 0x408 },	/* atc3_common */
	{ 0x403000000, 2, 0x258 },	/* disp_sys */
	{ 0x3a3000000, 0, 0x340 },	/* dispext0_sys */
	{ 0x603000000, 0, 0x490 },	/* dispext1_sys */
	{ 0x607000000, 0, 0x4e0 },	/* dispext2_sys */
	{ 0x307000000, 0, 0x2d8 },	/* dispext3_sys */
	{ 0x40b000000, 2, 0x370 },	/* ans */
	{ 0x280000000, 0, 0x170 },	/* avd_sys */
	{ 0x282000000, 0, 0x278 },	/* venc0_sys */
	{ 0x284000000, 0, 0x2a0 },	/* venc1_sys */
	{ 0x486000000, 2, 0x3a0 },	/* jpg */
	{ 0x416000000, 2, 0x390 },	/* apcie_sys_gp */
};

/*
 * iBoot's own S2R entry ends with the wake masks, a SEPROM command, then SEP PS |= bit 10
 * (DEV_DISABLE) and the commit. macOS at its commit (HV stop, 9 Nov) has SLP_DDR 0, exactly
 * the 25 ADT ap-wake-sources unmasked in the 9 mask words at +0x0c, and SEP PS 0x1000070f.
 */
static const u32 t6041_wake_masks[9] = {
	0xe5ff5fff, 0xff42edff, 0xffffffff, 0x3ffffbdf, 0xf7ffffc0,
	0xffffff7f, 0xffffffff, 0xffffffff, 0x00003fff,
};

static void t6041_wake_sep(struct t6041_sleep *s)
{
	unsigned int i;
	u32 v;

	if (readl(s->wake + 0x0))
		writel(0, s->wake + 0x0);
	for (i = 0; i < ARRAY_SIZE(t6041_wake_masks); i++)
		writel(t6041_wake_masks[i], s->wake + 0xc + 4 * i);

	v = readl(s->east + 0xc00);
	if (!(v & BIT(10)))
		writel(v | BIT(10), s->east + 0xc00);
	dsb(sy);
}

/* A perf word: the low nibble asks, [11:8] is the state reached, bit 31 busy. */
static bool t6041_perf_set(void __iomem *reg, u32 state)
{
	u32 v;

	if (FIELD_GET(GENMASK(11, 8), readl(reg)) == state)
		return true;
	writel((readl(reg) & ~0xf) | state, reg);
	return !readl_poll_timeout_atomic(reg, v,
			!(v & BIT(31)) && FIELD_GET(GENMASK(11, 8), v) == state, 1, 5000);
}

/* SoC to 3, voters 0, DCS to 6: macOS commits at DCS 6 (0x7666); the engine takes it to 4. */
static void t6041_mem_down(struct t6041_sleep *s)
{
	int i;

	t6041_perf_set(s->perf + 0x0, 3);
	for (i = 0; i < 4; i++)
		writel(0, s->pmc + i * 0x4000);
	t6041_perf_set(s->perf + 0x400, 6);
}

/*
 * The cluster words at 0x502820000: +0 E, +4 P0, +8 P1 (macOS's runtime writes and its offline
 * order), +0xc and +0x10 the two AVE complexes. Bit 5 is the up-done flag (CLUSTER_UP_DONE in
 * the Neo's driver), left set by iBoot's up and cleared by writing it. XNU clears it with its
 * first write and turns the AVE complexes off at boot, so at macOS's commit +0xc/+0x10 read 0
 * and the engine has no sequence to run for them; Linux had them on (0x2f), then 0x10/0x30 at
 * the commit. As the Neo's t8140_sleep_ave_off(): target 0 with the flag, then wait for 0.
 */
static void t6041_ave_off(struct t6041_sleep *s)
{
	static const u16 ave[] = { 0xc, 0x10 };
	unsigned int i;
	u32 v;

	for (i = 0; i < ARRAY_SIZE(ave); i++) {
		writel(BIT(5), s->cluster + ave[i]);
		readl_poll_timeout_atomic(s->cluster + ave[i], v, !v, 1, 5000);
	}
	v = readl(s->cluster + 0x8);
	if (v & BIT(5))
		writel((v & 0xf) | BIT(5), s->cluster + 0x8);
}

static void t6041_save(struct t6041_sleep *s);

/*
 * macOS lowers an East domain with two writes: AUTO off with the target still f, then target 0.
 * On a domain Linux already has off, the first one turns it on under a parent that is off, and
 * bit 11 latches. So only when it is on.
 */
static void east_lower(struct t6041_sleep *s, u32 off, u32 first, u32 second)
{
	u32 v = readl(s->east + off);

	if (!FIELD_GET(PS_TARGET, v) && !FIELD_GET(PS_ACTUAL, v))
		return;
	writel(first, s->east + off);
	writel(second, s->east + off);
}

static int t6041_sleep_commit(unsigned long unused)
{
	struct t6041_sleep *s = t6041_sleep;
	unsigned int i;

	t6041_save(s);
	s->stop_ret = spin_table_stop_parked();
	t6041_ave_off(s);
	/* While every AMCC slice is still on: a clock-gated plane faults. */
	t6041_amcc_quiesce(s);
	t6041_mcc_macos(s);
	/*
	 * The wake arms bit 0 of both words of every bridge whose block is on (and of ANS's in
	 * the storage raise), and boot has them clear (bridge 4: 0x10000 / 0x101330). Lowered with
	 * its bridge still armed, a block latches bit 11: on the second sleep of a boot ans and
	 * venc1_sys did, the engine waited and the chip reset. So only that bit is cleared again
	 * here, on blocks that are on (a bridge behind an off block can't be read), as linux#1477
	 * does before it lowers APCIE_SYS_GP.
	 */
	for (i = 0; i < 15; i++) {
		u32 v = readl((t6041_axi2af[i].pmgr ? s->east : s->ps) + t6041_axi2af[i].ps);

		if (FIELD_GET(PS_ACTUAL, v) != 0xf)
			continue;
		v = readl(s->axi2af[i] + 0x4);
		if (!(v & 1) && !(readl(s->axi2af[i] + 0x8) & 1))
			continue;
		writel(v & ~1U, s->axi2af[i] + 0x4);
		writel(readl(s->axi2af[i] + 0x8) & ~1U, s->axi2af[i] + 0x8);
	}
	t6041_cpu_ps_match(s);
	t6041_mem_down(s);

	for (i = 0; i < ARRAY_SIZE(t6041_macos_off); i++) {
		t6041_ps_lower(s->off[i]);
		udelay(100);
	}
	udelay(5000);
	t6041_full_ps_match(s);
	t6041_ps_match_all();
	/* As macOS at its commit (dj): every PMC voter at 0, and the PMS word it has at 0. */
	for (i = 0; i < 4; i++)
		writel(0, s->pmc + i * 0x4000);
	writel(0, s->pmc + 0xa9c18);
	udelay(5000);
	/* macOS 82 ms before its commit (trace, 8 Nov): the four cluster PTD votes, no demand. */
	writeq(0x2000000000000000ULL, s->ptd + 0xf80);
	writeq(0x2000000000000000ULL, s->ptd + 0xf88);
	writeq(0x2000000000000000ULL, s->ptd + 0xf90);
	writeq(0x2000000000000000ULL, s->ptd + 0xfa0);
	t6041_atc_down(s);
	t6041_wake_sep(s);

	writel(0xffef, s->misc + 0x0);
	/*
	 * macOS's order: its other cores are in their last WFI, deep-sleep bits set, 37 ms
	 * before the commit, and everything below runs with only the boot core up. AIC masked
	 * first so nothing ends that WFI.
	 */
	aic_sleep_mask_all();
	spin_table_release_stopped();
	t6041_spin_s(30);

	writel(0x3f833ff1, s->spmi + SPMI_NOTIFY_MASK);
	/* The PMU boot stage to 0x50 (asleep). */
	t6041_pmu_write(s, 0xf801, 0x50);
	east(s, 0x180, 0x3f0);
	east(s, 0x168, 0x3f0);
	east(s, 0x160, 0x3f0);
	east(s, 0x158, 0x3f0);
	writel(0x2000001, s->spmi + SPMI_NOTIFY_MASK);
	/* macOS writes nub-spmi-a0's mask too (-113 ms). Only with its domain on, which macOS keeps. */
	if (FIELD_GET(PS_ACTUAL, readl(s->ps_aon + 0x68)))
		writel(0x2000001, s->spmi_a0 + SPMI_NOTIFY_MASK);
	east(s, 0x1c8, 0x3f0);
	cpm_gates(s);
	writel(0, s->cluster + 0x0);
	cpm_gates(s);
	writel(0, s->cluster + 0x8);
	east_lower(s, 0x398, 0x400004f, 0x4000340);
	east_lower(s, 0x388, 0xf, 0x300);
	east(s, 0x110, 0x3f0);
	east_lower(s, 0x380, 0xf0000ff, 0xf0003f0);
	east(s, 0x108, 0x3f0);
	east_lower(s, 0x390, 0xf, 0x300);
	east(s, 0x100, 0x3f0);
	east_lower(s, 0x370, 0xf0000ff, 0xf0003f0);
	/* macOS 10 and 4 ms before arming WD2: the SoC PTD word, then without bit 33. */
	writeq(0x200000800ULL, s->ptd + 0xf00);
	udelay(5000);
	writeq(0x800ULL, s->ptd + 0xf00);
	writel(0, s->wdt + 0x0c);
	writel(0, s->wdt + 0x2c);
	writel(0, s->wdt + 0x20);
	writel(0x7270e00, s->wdt + 0x28);
	writel(1, s->wdt + 0x2c);
	writel(0, s->aon + 0x8);
	cpm_gates(s);
	cpm_gates(s);
	cpm_gates(s);
	writel(0x10, s->cluster + 0x00);
	writel(0, s->cluster + 0x04);
	writel(0x10, s->cluster + 0x08);
	writel(0x20, s->cluster + 0x0c);
	writel(0, s->cluster + 0x10);
	east(s, 0x120, 0x400024f);
	east(s, 0x130, 0x400024f);
	writel(0xa0100000, s->clkgen + 0x60);
	writel(0xa0100000, s->clkgen + 0x64);
	writel(0xa0100000, s->clkgen + 0x54);
	writel(0xa0100000, s->clkgen + 0x50);
	/* aura reads this with the MMU off on the way back. */
	WRITE_ONCE(s->mbox->resume_pa, __pa_symbol(cpu_resume));
	dcache_clean_poc((unsigned long)s->mbox, (unsigned long)(s->mbox + 1));
	t6041_atc_commons_on(s);
	/*
	 * No SPMI near the commit or after it, as on macOS (its last command is 113 ms before)
	 * and on the Air (nothing to the PMU after the commit): the engine drives this bus.
	 */
	t6041_spin_s(20);
	/*
	 * macOS's last writes before its commit (ApplePMGR's keep-alive off, -0.6 ms): every
	 * keep-alive word to 0, the two tables in turn.
	 */
	for (i = 0; i < KA_WORDS; i++) {
		writel(0, s->ka[0] + i * KA_STRIDE);
		writel(0, s->ka[1] + i * KA_STRIDE);
	}
	dsb(sy);
	writel(1, s->commit);
	dsb(sy);
	/* The other cores are in their deep WFI already. AIC masked again, 3 ms, then this core's. */
	aic_sleep_mask_all();
	t6041_spin_s(3);
	t6041_boot_wfi(s);
}

/*
 * The display pipes' PMC votes (linux#1477 "G41": macOS sets them before a pipe's power words:
 * boost 0x5048ac028, DVMR 0x5048a4000 with its ack at +0x20, BWR 0x5048ac020, the cluster rail
 * 0x5048a9c18). iBoot leaves them set (boost 0x1f, rail 0xc) and Linux never touched them; after
 * a wake they read 0, and the display then runs through every step and shows black. Saved as
 * the commit starts and put back after the wake, offsets into the PMC block at 0x504800000.
 */
static const u32 t6041_pmc_vote_off[5] = { 0xac028, 0xa4000, 0xac020, 0xa9c10, 0xa9c20 };

/*
 * macOS's memory-cache Active action (AppleT6041MemCacheController::mccEnableCacheMode(true)):
 * the way count back through the all-planes window, then each plane's status polled. After a
 * wake every plane reads +0x2800 = 0x80000000 and +0x2804 = 0 (cache off), where Linux had
 * 0x8000000c / 0x0c000c00. The allocation-disable word +0x82c is left as it was.
 */
static void t6041_mcc_active(struct t6041_sleep *s)
{
	unsigned int i, p;
	u32 v;

	for (i = 0; i < s->ngroups; i++) {
		writel(s->pre_ways[4 * i], s->amccb[i] + 0x1c2800);
		for (p = 0; p < 4; p++)
			readl_poll_timeout_atomic(s->amccb[i] + 0x40000 * p + 0x2804, v,
						  v == s->pre_cache[4 * i + p], 10, 100000);
	}
}

/* Puts back the display's PMC votes; the DVMR one with its ack. */
static void t6041_pmc_votes(struct t6041_sleep *s)
{
	unsigned int i;
	u32 v;

	for (i = 0; i < ARRAY_SIZE(t6041_pmc_vote_off); i++) {
		void __iomem *reg = s->pmc + t6041_pmc_vote_off[i];

		if (readl(reg) != s->pre_votes[i])
			writel(s->pre_votes[i], reg);
		if (t6041_pmc_vote_off[i] == 0xa4000 && s->pre_votes[i])
			readl_poll_timeout_atomic(reg + 0x20, v, v & BIT(0), 1, 8000);
	}
}

/* What the commit is about to change, as Linux left it. */
static u32 t6041_pre[ARRAY_SIZE(t6041_macos_regs)];
static const u16 t6041_clk_mux[4] = { 0x50, 0x54, 0x64, 0x60 };

static void t6041_save(struct t6041_sleep *s)
{
	unsigned int i;

	for (i = 0; i < 4 * s->ngroups; i++) {
		void __iomem *plane = s->amccb[i / 4] + 0x40000 * (i % 4);

		s->pre_ways[i] = readl(plane + 0x2800);
		s->pre_cache[i] = readl(plane + 0x2804);
	}
	for (i = 0; i < ARRAY_SIZE(t6041_pmc_vote_off); i++)
		s->pre_votes[i] = readl(s->pmc + t6041_pmc_vote_off[i]);
	s->cntkctl = read_sysreg(cntkctl_el1);
	s->siq_cfg = read_sysreg_s(SYS_APL_SIQ_CFG_EL1);
	for (i = 0; i < ARRAY_SIZE(t6041_macos_regs); i++)
		if (t6041_macos_regs[i].ps)
			t6041_pre[i] = readl(t6041_reg_map[i]);
	for (i = 0; i < ARRAY_SIZE(t6041_full_ps) && s->ngroups == 8; i++)
		s->full_pre[i] = readl(t6041_full_reg(s, i));
	s->pre_perf[0] = readl(s->perf + 0x0);
	s->pre_perf[1] = readl(s->perf + 0x400);
	for (i = 0; i < 4; i++)
		s->pre_pmc[i] = readl(s->pmc + i * 0x4000);
	s->pre_pmc[4] = readl(s->pmc + 0xa9c18);
	for (i = 0; i < 4; i++)
		s->pre_clk[i] = readl(s->clkgen + t6041_clk_mux[i]);
	s->pre_mask[0] = readl(s->spmi + SPMI_NOTIFY_MASK);
	s->pre_a0 = FIELD_GET(PS_ACTUAL, readl(s->ps_aon + 0x68)) != 0;
	if (s->pre_a0)
		s->pre_mask[1] = readl(s->spmi_a0 + SPMI_NOTIFY_MASK);
	for (i = 0; i < 9; i++)
		s->pre_wake[i] = readl(s->wake + 0xc + 4 * i);
	for (i = 0; i < 44; i++)
		s->aop_pre[i] = readl(s->aop_ps + 8 * i);
	for (i = 0; i < 200; i++)
		s->gpio_pre[i] = readl(s->gpio + 4 * i);
	/* Only with the GP PCIe block on: apcie_gp (East +0x100) and apcie_sys_gp (+0x390). */
	s->gp_saved = FIELD_GET(PS_ACTUAL, readl(s->east + 0x100)) == 0xf &&
		      FIELD_GET(PS_ACTUAL, readl(s->east + 0x390)) == 0xf;
	for (i = 0; i < 2 && s->gp_saved; i++) {
		s->gp_phy_pre[i] = readl(s->gp_phy[i]);
		s->gp_app_pre[i] = readl(s->gp_port[i] + 0x800);
		s->gp_perst_pre[i] = readl(s->gp_port[i] + 0x82c);
	}
}

/*
 * The AP GPIO block loses every pin's configuration in the sleep. Linux's pinctrl driver only
 * writes back the pins it configured itself; the rest (pin functions iBoot set up for other
 * users, the display's among them) stayed at their reset values, and after a wake the DCP's
 * power-on ended in "No device added after powering on the rails. HPD=0". macOS restores all
 * of them: its ADT lists only three exceptions (gpio0 no-restore-gpio-pins = 190, 191, 192).
 */
static void t6041_gpio_restore(struct t6041_sleep *s)
{
	unsigned int i;

	for (i = 0; i < 200; i++) {
		if (i >= 190 && i <= 192)
			continue;
		if (readl(s->gpio + 4 * i) != s->gpio_pre[i])
			writel(s->gpio_pre[i], s->gpio + 4 * i);
	}
}

/*
 * linux#1477 t6040_aop_wake (aop_wake bits 1 and 2): the AOP island's power words are not in
 * our PS table. Parents first (their order table backwards), every word that was on before the
 * sleep goes to manual target f, AUTO off, so it can't gate again before the AOP driver reads
 * its ASC.
 */
static const u16 t6041_aop_ps_order[] = {
	0x0b0, 0x128, 0x120, 0x158, 0x150, 0x148, 0x140, 0x138, 0x130, 0x118, 0x0c0, 0x0c8,
	0x0e8, 0x0f0, 0x0f8, 0x100, 0x108, 0x110, 0x118, 0x0e0, 0x0d8, 0x0d0, 0x0b8, 0x090,
	0x020, 0x098, 0x0a0, 0x0a8, 0x028, 0x030, 0x068, 0x070, 0x078, 0x080, 0x038, 0x040,
	0x048, 0x058, 0x060, 0x088, 0x050, 0x010, 0x018, 0x008, 0x000,
};

static void t6041_aop_wake(struct t6041_sleep *s)
{
	int k;
	u32 v;

	for (k = ARRAY_SIZE(t6041_aop_ps_order) - 1; k >= 0; k--) {
		u16 off = t6041_aop_ps_order[k];
		void __iomem *r = s->aop_ps + off;
		u32 now = readl(r);

		if (!FIELD_GET(PS_ACTUAL, s->aop_pre[off / 8]))
			continue;
		if (FIELD_GET(PS_ACTUAL, now) == 0xf && !(now & PS_AUTO_ENABLE))
			continue;
		writel((now & ~(PS_AUTO_ENABLE | PS_TARGET)) | PS_TARGET, r);
		readl_poll_timeout_atomic(r, v, FIELD_GET(PS_ACTUAL, v), 1, 2000);
	}
}

static void t6041_tun_apply(void __iomem *regs, const struct t6041_tun *t, unsigned int n,
			    bool only_changed)
{
	unsigned int i;

	for (i = 0; i < n; i++) {
		u32 old = readl(regs + t[i].off);
		u32 val = (old & ~t[i].mask) | (t[i].val & t[i].mask);

		if (!only_changed || val != old)
			writel(val, regs + t[i].off);
	}
}

static int t6041_ps_raise(void __iomem *reg)
{
	int ret = t6041_ps_raise_start(reg);

	writel((readl(reg) & ~(PS_AUTO_ENABLE | GENMASK(11, 0))) | PS_TARGET | PS_AUTO_ENABLE, reg);
	return ret;
}

/*
 * ANS's storage link from power-off, a port of linux#1477 t6040_sleep_raise_apcie_st() with its
 * defaults (apcie_wake = 7): the domains in macOS's order with the two bridges armed and SYS_GP
 * left in manual on, the axi2af / cio3pll / clkgen tunables and the PLL start, APCIE_PHY_SW,
 * the PHY tunables and its two clock requests, the PHY-IP PLL and lane tables, then the root
 * complex's ready handshake. Returns 0, or the step (1-8) that timed out. Runs only when the
 * NVMe driver stopped ANS for this sleep.
 */
#define TUN(reg, tbl, chg)	t6041_tun_apply(reg, tbl, ARRAY_SIZE(tbl), chg)

static int t6041_apcie_raise(struct t6041_sleep *s)
{
	void __iomem *ps = s->east, *ctrl = s->ap_phy + 0x8000, *run = s->ap_phy + 0x8004;
	u32 v;
	int ret;

	ret = t6041_ps_raise(ps + 0x370);
	if (!ret) {
		writel(readl(s->axi2af[9] + 0x4) | 1, s->axi2af[9] + 0x4);
		writel(readl(s->axi2af[9] + 0x8) | 1, s->axi2af[9] + 0x8);
	}
	ret |= t6041_ps_raise(ps + 0x100);
	if (!ret)
		ret = t6041_ps_raise_start(ps + 0x390);
	if (!ret) {
		writel(readl(s->ap_axi + 0x104) | 1, s->ap_axi + 0x104);
		writel(readl(s->ap_axi + 0x108) | 1, s->ap_axi + 0x108);
	}
	ret |= t6041_ps_raise(ps + 0x108);
	ret |= t6041_ps_raise(ps + 0x380);
	ret |= t6041_ps_raise(ps + 0x110);
	ret |= t6041_ps_raise(ps + 0x388);
	if (ret)
		return 1;

	TUN(s->ap_axi, t6041_tun_axi2af, false);
	TUN(s->ap_cio3pll, t6041_tun_cio3pll, false);
	TUN(s->ap_clkgen, t6041_tun_clkgen, false);
	writel(readl(s->ap_clkgen + 0x4) | BIT(31), s->ap_clkgen + 0x4);
	writel(readl(s->ap_clkgen + 0x0) & ~(BIT(2) | BIT(1)), s->ap_clkgen + 0x0);
	if (readl_poll_timeout_atomic(s->ap_clkgen + 0x0, v, !(v & BIT(31)), 1, 50000))
		return 2;
	if (t6041_ps_raise(ps + 0x398))
		return 3;
	TUN(s->ap_phy, t6041_tun_phy, false);
	if (readl_poll_timeout_atomic(s->ap_phy + 0x4000, v, v & BIT(31), 1, 50000))
		return 4;
	writel(readl(ctrl) | BIT(0), ctrl);
	if (readl_poll_timeout_atomic(ctrl, v, v & BIT(2), 1, 50000))
		return 5;
	writel(readl(ctrl) | BIT(1), ctrl);
	if (readl_poll_timeout_atomic(ctrl, v, v & BIT(3), 1, 50000))
		return 6;
	writel(readl(ctrl) & ~BIT(4), ctrl);

	writel(readl(run) | BIT(0), run);
	TUN(s->ap_phy_ip, t6041_tun_phy_ip_pll, true);
	TUN(s->ap_phy_ip, t6041_tun_phy_ip_lanes, true);
	writel(readl(s->ap_phy_ip + 0x90) & ~BIT(16), s->ap_phy_ip + 0x90);
	writel(readl(run) | BIT(4), run);
	if (readl_poll_timeout_atomic(s->ap_phy + 0x8008, v, v & BIT(0), 1, 50000))
		return 7;
	writel(readl(s->ap_phy + 0x4000) | BIT(0), s->ap_phy + 0x4000);
	writel(readl(ctrl) | BIT(27), ctrl);

	writel(0, s->ap_rc + 0x04);
	writel(0x140, s->ap_rc + 0x54);
	writel(1, s->ap_rc + 0x50);
	if (readl_poll_timeout_atomic(s->ap_rc + 0x58, v, v & BIT(0), 1, 50000))
		return 8;
	return 0;
}

/*
 * linux#1477 t6040_gp_pcie_clk (its el2.123/124): the DARTs at 0x410000000 and 0x411000000 sit
 * in the blocks of GP PCIe ports 0 and 1, whose clocks the bootloader set up and nothing does
 * after a real sleep; reading them then raises an SError. Per port: both reference clock
 * requests with their acks, reset off, the enables and link bit as saved, the app clock, the
 * controller's PERST word under its guard bit, then PORT_STATUS ready. Returns a mask of the
 * ports that didn't come back.
 */
static unsigned int t6041_gp_pcie_clk(struct t6041_sleep *s)
{
	unsigned int i, late = 0;
	u32 v;

	for (i = 0; i < 2; i++) {
		void __iomem *phy = s->gp_phy[i], *port = s->gp_port[i];
		u32 cfg = s->gp_phy_pre[i], app = s->gp_app_pre[i];

		if (!s->gp_saved || FIELD_GET(PS_ACTUAL, readl(s->east + 0x100)) != 0xf ||
		    FIELD_GET(PS_ACTUAL, readl(s->east + 0x390)) != 0xf)
			continue;
		writel(readl(phy) | BIT(0), phy);
		if (readl_poll_timeout_atomic(phy, v, v & BIT(2), 1, 50000))
			late |= BIT(i);
		writel(readl(phy) | BIT(1), phy);
		if (readl_poll_timeout_atomic(phy, v, v & BIT(3), 1, 50000))
			late |= BIT(i);
		if (!(cfg & BIT(4))) {
			writel(readl(phy) & ~BIT(4), phy);
			udelay(1);
		}
		writel(readl(phy) | (cfg & (BIT(9) | BIT(10))), phy);
		writel(readl(phy) | (cfg & BIT(31)), phy);
		writel((readl(port + 0x800) & ~(BIT(0) | BIT(8))) | (app & (BIT(0) | BIT(8))),
		       port + 0x800);
		writel(readl(port + 0x13c) | BIT(8), port + 0x13c);
		writel(s->gp_perst_pre[i], port + 0x82c);
		writel(readl(port + 0x13c) & ~BIT(8), port + 0x13c);
		if (readl_poll_timeout_atomic(port + 0x804, v, v & BIT(0), 1, 50000))
			late |= BIT(i);
	}
	return late;
}

/*
 * Every PS word back to the target and AUTO it had as the commit started: raises parents
 * first, then lowers children first, so no word is switched under a parent that is off (that
 * latches bit 11). Returns how many raises didn't reach their target in 3 ms.
 */
static unsigned int t6041_ps_restore(struct t6041_sleep *s)
{
	const u32 keep = ~(u32)(PS_AUTO_ENABLE | PS_FLAGS | PS_TARGET);
	unsigned int late = 0;
	int k;
	u32 v;

	for (k = 0; k < ARRAY_SIZE(t6041_ps_order); k++) {
		void __iomem *reg = t6041_reg_map[t6041_ps_order[k]];
		u32 pre = t6041_pre[t6041_ps_order[k]], cur = readl(reg);
		u32 pt = pre & PS_TARGET;

		if (pt > (cur & PS_TARGET)) {
			writel((cur & keep) | pt, reg);
			if (readl_poll_timeout_atomic(reg, v, FIELD_GET(PS_ACTUAL, v) == pt, 1, 3000))
				late++;
			cur = readl(reg);
		}
		if (pt == (cur & PS_TARGET) && ((pre ^ cur) & PS_AUTO_ENABLE))
			writel((cur & ~(PS_AUTO_ENABLE | PS_FLAGS)) | (pre & PS_AUTO_ENABLE), reg);
	}
	for (k = ARRAY_SIZE(t6041_ps_order) - 1; k >= 0; k--) {
		void __iomem *reg = t6041_reg_map[t6041_ps_order[k]];
		u32 pre = t6041_pre[t6041_ps_order[k]], cur = readl(reg);
		u32 pt = pre & PS_TARGET;

		if (pt >= (cur & PS_TARGET))
			continue;
		if (cur & PS_AUTO_ENABLE) {
			cur &= ~(PS_AUTO_ENABLE | PS_FLAGS);
			writel(cur, reg);
		}
		writel((cur & keep) | pt | PS_FLAGS, reg);
		udelay(50);
		if (pre & PS_AUTO_ENABLE)
			writel((readl(reg) & ~PS_FLAGS) | PS_AUTO_ENABLE, reg);
	}
	return late;
}

/* The full part's extra channel words back to their target and AUTO, as t6041_ps_restore(). */
static unsigned int t6041_full_ps_restore(struct t6041_sleep *s)
{
	const u32 keep = ~(u32)(PS_AUTO_ENABLE | PS_FLAGS | PS_TARGET);
	unsigned int i, late = 0;
	u32 v;

	for (i = 0; i < ARRAY_SIZE(t6041_full_ps) && s->ngroups == 8; i++) {
		void __iomem *reg = t6041_full_reg(s, i);
		u32 pre = s->full_pre[i], cur = readl(reg), pt = pre & PS_TARGET;

		if (pt > (cur & PS_TARGET)) {
			writel((cur & keep) | pt, reg);
			if (readl_poll_timeout_atomic(reg, v, FIELD_GET(PS_ACTUAL, v) == pt,
						      1, 3000))
				late++;
			cur = readl(reg);
		}
		if (pt == (cur & PS_TARGET) && ((pre ^ cur) & PS_AUTO_ENABLE))
			writel((cur & ~(PS_AUTO_ENABLE | PS_FLAGS)) | (pre & PS_AUTO_ENABLE), reg);
	}
	return late;
}

static u64 t6041_ret_t0, t6041_ret_t1;

/* Back on the boot core through aura's mailbox: the clusters on, the masks back, WD2 off. */
static void t6041_sleep_return(struct t6041_sleep *s)
{
	unsigned int i;
	u32 v;

	t6041_ret_t0 = read_sysreg(cntpct_el0);
	/* The reset left the timer control and the AIC's IRQ routing at their EL2 defaults. */
	write_sysreg(s->cntkctl, cntkctl_el1);
	isb();
	write_sysreg_s(s->siq_cfg, SYS_APL_SIQ_CFG_EL1);
	writel(0, s->wdt + 0x2c);
	/* Keep-alive back on, as macOS on its way back and as iBoot leaves it. */
	for (i = 0; i < KA_WORDS; i++) {
		writel(1, s->ka[0] + i * KA_STRIDE);
		writel(1, s->ka[1] + i * KA_STRIDE);
	}
	spin_table_stop_finished();
	aic_sleep_restore();
	writel(0xf, s->cluster + 0x0);
	writel(0xf, s->cluster + 0x4);
	writel(0xf, s->cluster + 0x8);
	/* As restoreHW (linux#1477): the two wake gates, then the CPM voters. */
	writel(readl(s->pwrgate + 0x000) | BIT(31), s->pwrgate + 0x000);
	writel(readl(s->pwrgate + 0x040) | BIT(31), s->pwrgate + 0x040);
	cpm_gates(s);
	/* SEP's DEV_DISABLE off again, bit 11 polled clear; the S2R request word; the wake masks. */
	v = readl(s->east + 0xc00);
	if (v & BIT(10))
		writel(v & ~BIT(10), s->east + 0xc00);
	readl_poll_timeout_atomic(s->east + 0xc00, v, !(v & BIT(11)), 1, 5000);
	writel(0, s->wake + 0x0);
	for (i = 0; i < 9; i++)
		writel(s->pre_wake[i], s->wake + 0xc + 4 * i);
	s->ps_late = t6041_ps_restore(s);
	/* Before t6041_mcc_active() touches their planes. */
	s->ps_late += t6041_full_ps_restore(s);
	t6041_mcc_active(s);
	t6041_aop_wake(s);
	t6041_gpio_restore(s);
	/* A bridge whose block is back on is armed again, or it drops its traffic (linux#1477). */
	for (i = 0; i < 15; i++) {
		v = readl((t6041_axi2af[i].pmgr ? s->east : s->ps) + t6041_axi2af[i].ps);
		if ((v & PS_TARGET) != PS_TARGET || FIELD_GET(PS_ACTUAL, v) != 0xf)
			continue;
		writel(readl(s->axi2af[i] + 0x4) | 1, s->axi2af[i] + 0x4);
		writel(readl(s->axi2af[i] + 0x8) | 1, s->axi2af[i] + 0x8);
	}
	s->apcie_ret = s->ans_stopped ? t6041_apcie_raise(s) : 0;
	s->gp_late = t6041_gp_pcie_clk(s);
	/* The clock muxes the commit parked, last parked first; perf states; PMC voters. */
	for (i = 4; i-- > 0;)
		writel(s->pre_clk[i], s->clkgen + t6041_clk_mux[i]);
	t6041_perf_set(s->perf + 0x400, s->pre_perf[1] & 0xf);
	t6041_perf_set(s->perf + 0x0, s->pre_perf[0] & 0xf);
	for (i = 0; i < 4; i++)
		writel(s->pre_pmc[i], s->pmc + i * 0x4000);
	writel(s->pre_pmc[4], s->pmc + 0xa9c18);
	t6041_pmc_votes(s);
	writel(s->pre_mask[0], s->spmi + SPMI_NOTIFY_MASK);
	if (s->pre_a0 && FIELD_GET(PS_ACTUAL, readl(s->ps_aon + 0x68)))
		writel(s->pre_mask[1], s->spmi_a0 + SPMI_NOTIFY_MASK);
	writel(0, s->wdt + 0x2c);
	/* The PMU boot stage back from 0x50 (asleep) to 0x30 (OS running), as macsmc-reboot's probe. */
	t6041_pmu_write(s, 0xf801, 0x30);
	t6041_ret_t1 = read_sysreg(cntpct_el0);
}

static int t6041_sleep_enter(suspend_state_t state)
{
	struct t6041_sleep *s = t6041_sleep;
	int ret;

	/* With no way back, a sleep would end in aura's boot flow over this kernel. */
	if (!s->mbox)
		return -EOPNOTSUPP;
	/*
	 * Every parked CPU answers the hold while a refusal is still possible: at the commit
	 * only a held CPU stops, and one left awake there keeps the engine waiting until the
	 * watchdog resets the SoC.
	 */
	ret = spin_table_hold_parked();
	if (ret) {
		pr_err("t6041-sleep: refused: a parked CPU didn't answer the hold: %d\n", ret);
		return ret;
	}
	/* macOS stops the PMP before its last steps; past this a refusal can't go back. */
	ret = apple_sleep_coprocs_stop_all();
	s->coproc_ret = ret;
	s->ans_stopped = apple_sleep_coprocs_ans_stopped();
	pm_set_resume_via_firmware();
	ret = cpu_suspend(0, t6041_sleep_commit);
	if (!ret)
		t6041_sleep_return(s);
	return ret;
}

static void t6041_sleep_end(void)
{
	struct t6041_sleep *s = t6041_sleep;

	if (!s || !t6041_ret_t1)
		return;
	dev_dbg(s->dev, "back from deep sleep, return path %llu ms\n",
		div64_u64((t6041_ret_t1 - t6041_ret_t0) * 1000, arch_timer_get_cntfrq()));
	t6041_ret_t1 = 0;
	if (s->coproc_ret < 0)
		dev_warn(s->dev, "a coprocessor didn't stop for the sleep: %d\n", s->coproc_ret);
	if (s->stop_ret)
		dev_warn(s->dev, "the parked CPUs didn't all stop: %d\n", s->stop_ret);
	/* One of them takes longer than 3 ms on every wake here, and comes up all the same. */
	if (s->ps_late)
		dev_dbg(s->dev, "%u power domains took more than 3 ms to come back\n", s->ps_late);
	if (s->apcie_ret)
		dev_warn(s->dev, "storage link raise timed out at step %d\n", s->apcie_ret);
	if (s->gp_late)
		dev_warn(s->dev, "GP PCIe ports %#x didn't come back\n", s->gp_late);
}

static int t6041_sleep_begin(suspend_state_t state)
{
	pm_set_suspend_via_firmware();
	return 0;
}

static const struct platform_suspend_ops t6041_sleep_ops = {
	.valid = suspend_valid_only_mem,
	.begin = t6041_sleep_begin,
	.end = t6041_sleep_end,
	.enter = t6041_sleep_enter,
};

static int t6041_sleep_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct t6041_sleep *s;
	unsigned int i;
	int ret;

	if (!is_kernel_in_hyp_mode())
		return -ENODEV;

	s = devm_kzalloc(dev, sizeof(*s), GFP_KERNEL);
	if (!s)
		return -ENOMEM;
	s->dev = dev;
	s->ps = devm_ioremap(dev, PA_PS, 0x18000);
	s->east = devm_ioremap(dev, PA_EAST, 0x30000);
	s->cluster = devm_ioremap(dev, PA_CLUSTER, 0x4000);
	s->pwrgate = devm_ioremap(dev, PA_PWRGATE, 0x4000);
	s->misc = devm_ioremap(dev, PA_MISC_CORES, 0x4000);
	s->wdt = devm_ioremap(dev, PA_WDT, 0x4000);
	s->aon = devm_ioremap(dev, PA_AON_MISC, 0x4000);
	s->commit = devm_ioremap(dev, PA_COMMIT, 0x4);
	s->clkgen = devm_ioremap(dev, PA_CLKGEN, 0x4000);
	s->spmi = devm_ioremap(dev, PA_SPMI0, 0x4000);
	s->spmi_a0 = devm_ioremap(dev, 0x509188000UL, 0x4000);
	if (!s->spmi_a0)
		return -ENOMEM;
	s->ka[0] = devm_ioremap(dev, PA_KA_PMGR, KA_WORDS * KA_STRIDE);
	s->ka[1] = devm_ioremap(dev, PA_KA_EAST, KA_WORDS * KA_STRIDE);
	if (!s->ka[0] || !s->ka[1])
		return -ENOMEM;
	s->ps_aon = devm_ioremap(dev, 0x508300000UL, 0x4000);
	s->pmc = devm_ioremap(dev, 0x504800000UL, 0xb0000);
	for (i = 0; i < 15; i++) {
		s->axi2af[i] = devm_ioremap(dev, t6041_axi2af[i].base + 0x100, 0x10);
		if (!s->axi2af[i])
			return -ENOMEM;
	}
	s->ptd = devm_ioremap(dev, 0x5003d0000UL, 0x4000);
	if (!s->ptd)
		return -ENOMEM;
	s->perf = devm_ioremap(dev, 0x50020c000UL, 0x800);
	if (!s->perf)
		return -ENOMEM;
	s->wake = devm_ioremap(dev, 0x508004000UL, 0x58);
	if (!s->wake)
		return -ENOMEM;
	/*
	 * The full part: amcc24's word (+0x248) and amcc28's (East +0x2c0) aren't both target and
	 * actual 0 as on the binned one, where those channels are fused off.
	 */
	if (!s->ps || !s->east)
		return -ENOMEM;
	s->ngroups = (readl(s->ps + 0x248) & (PS_TARGET | PS_ACTUAL)) &&
		     (readl(s->east + 0x2c0) & (PS_TARGET | PS_ACTUAL)) ? 8 : 6;
	dev_info(dev, "%u memory channels\n", 4 * s->ngroups);
	/* AMCC error words: groups of 4 planes, 0x220025000 + 0x2000000 * g + 0x40000 * p. */
	for (i = 0; i < 4 * s->ngroups; i++) {
		s->amcc[i] = devm_ioremap(dev, 0x220025000UL + 0x2000000UL * (i / 4) +
					  0x40000UL * (i % 4), 0x40);
		if (!s->amcc[i])
			return -ENOMEM;
	}
	for (i = 0; i < 4 * s->ngroups; i++) {
		s->dcs[i] = devm_ioremap(dev, 0x220200000UL + 0x2000000UL * (i / 4) +
					 0x200000UL * (i % 4), 0x40);
		if (!s->dcs[i])
			return -ENOMEM;
	}
	for (i = 0; i < s->ngroups; i++) {
		s->amccb[i] = devm_ioremap(dev, 0x220000000UL + 0x2000000UL * i, 0x200000);
		if (!s->amccb[i])
			return -ENOMEM;
	}
	if (!s->ps || !s->east || !s->cluster || !s->pwrgate || !s->misc || !s->wdt || !s->aon ||
	    !s->commit || !s->clkgen || !s->spmi || !s->ps_aon || !s->pmc)
		return -ENOMEM;
	for (i = 0; i < ARRAY_SIZE(t6041_macos_regs); i++) {
		t6041_reg_map[i] = devm_ioremap(dev, t6041_macos_regs[i].pa, 4);
		if (!t6041_reg_map[i])
			return -ENOMEM;
	}
	for (i = 0; i < ARRAY_SIZE(t6041_macos_off); i++) {
		s->off[i] = devm_ioremap(dev, t6041_macos_off[i], 4);
		if (!s->off[i])
			return -ENOMEM;
	}

	{
		u64 pa;

		if (!of_property_read_u64(of_chosen, "aurora,s2r-mailbox", &pa)) {
			s->mbox = devm_memremap(dev, pa, sizeof(*s->mbox), MEMREMAP_WB);
			if (IS_ERR(s->mbox) || READ_ONCE(s->mbox->magic) != T6041_S2R_MAGIC)
				s->mbox = NULL;
		}
		/* With no way back, a sleep would end in aura's boot flow over this kernel. */
		if (!s->mbox) {
			dev_info(dev, "no aura S2R mailbox: no deep sleep\n");
			return -ENODEV;
		}
	}
	s->gpio = devm_ioremap(dev, 0x51c000000UL, 0x400);
	s->aop_ps = devm_ioremap(dev, 0x508304000UL, 0x160);
	if (!s->aop_ps || !s->gpio)
		return -ENOMEM;
	s->ap_rc = devm_ioremap(dev, 0x414000000UL, 0x4000);
	s->ap_phy = devm_ioremap(dev, 0x417000000UL, 0x40000);
	s->ap_phy_ip = devm_ioremap(dev, 0x417040000UL, 0x28000);
	s->ap_axi = devm_ioremap(dev, 0x416000000UL, 0x21000);
	s->ap_cio3pll = devm_ioremap(dev, 0x415046200UL, 0x4000);
	s->ap_clkgen = devm_ioremap(dev, 0x415044000UL, 0x4000);
	if (!s->ap_rc || !s->ap_phy || !s->ap_phy_ip || !s->ap_axi || !s->ap_cio3pll || !s->ap_clkgen)
		return -ENOMEM;
	s->gp_port[0] = devm_ioremap(dev, 0x410028000UL, 0x1000);
	s->gp_port[1] = devm_ioremap(dev, 0x411028000UL, 0x1000);
	s->gp_phy[0] = devm_ioremap(dev, 0x417020000UL, 0x10);
	s->gp_phy[1] = devm_ioremap(dev, 0x417024000UL, 0x10);
	if (!s->gp_port[0] || !s->gp_port[1] || !s->gp_phy[0] || !s->gp_phy[1])
		return -ENOMEM;
	t6041_sleep = s;
	ret = spin_table_set_power_ops(&t6041_cpu_power_ops);
	if (ret)
		return dev_err_probe(dev, ret, "cannot control CPU power\n");
	suspend_set_ops(&t6041_sleep_ops);
	dev_info(dev, "T6041 deep sleep at EL2 through aura's S2R mailbox\n");
	return 0;
}

static const struct of_device_id t6041_sleep_of_match[] = {
	{ .compatible = "apple,t6041-sleep" },
	{ }
};

static struct platform_driver t6041_sleep_driver = {
	.driver = {
		.name = "t6041-sleep",
		.of_match_table = t6041_sleep_of_match,
		.suppress_bind_attrs = true,
	},
	.probe = t6041_sleep_probe,
};
builtin_platform_driver(t6041_sleep_driver);
