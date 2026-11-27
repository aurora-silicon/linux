// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * Apple T6041 (M4 Max) system sleep, at EL2 with no monitor below.
 *
 * It runs macOS 26.6.2's last steps before the S2R commit, in macOS's order and
 * with its values, as traced from macOS sleeping under aura's hypervisor, and
 * commits; aura's S2R mailbox brings the boot core back to cpu_resume on the
 * wake, and the driver restores what the sleep took.
 *
 * Needs the kernel at EL2 (CTXR window) and the "aurora,rvbar-spin-table" CPU
 * enable method, so the other cores can leave the kernel.
 */

#include <clocksource/arm_arch_timer.h>
#include <linux/bitfield.h>
#include <linux/bitmap.h>
#include <linux/build_bug.h>
#include <linux/cacheflush.h>
#include <linux/cpumask.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/irqchip/irq-apple-aic.h>
#include <linux/math.h>
#include <linux/minmax.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/processor.h>
#include <linux/slab.h>
#include <linux/smp.h>
#include <linux/soc/apple/sleep.h>
#include <linux/soc/apple/tunable.h>
#include <linux/string.h>
#include <linux/suspend.h>
#include <linux/time64.h>
#include <asm/arch_timer.h>
#include <asm/cpu_ops.h>
#include <asm/cputype.h>
#include <asm/smp_plat.h>
#include <asm/suspend.h>
#include <asm/sysreg.h>
#include <asm/virt.h>

#define SYS_APL_IPI_SR_EL1	sys_reg(3, 5, 15, 1, 1)
#define IPI_SR_PENDING		BIT(0)
#define SYS_APL_SIQ_CFG_EL1	sys_reg(3, 4, 15, 10, 4)
#define SYS_APL_DEEP_SLEEP	sys_reg(3, 5, 15, 6, 2)
#define DEEP_SLEEP_BITS		(BIT(0) | BIT(63))

#define KA_WORDS		32
#define KA_STRIDE		0x44

#define PS_AUTO_ENABLE		BIT(28)
#define PS_AUTO_STATE		GENMASK(27, 24)
#define PS_FLAGS		GENMASK(9, 8)
#define PS_ACTUAL		GENMASK(7, 4)
#define PS_TARGET		GENMASK(3, 0)

#define SPMI_STATUS		0x00
#define SPMI_CMD		0x04
#define SPMI_REPLY		0x08
#define SPMI_RX_EMPTY		BIT(24)
#define SPMI_TX_EMPTY		BIT(8)
#define SPMI_IDLE		(SPMI_RX_EMPTY | SPMI_TX_EMPTY)
#define SPMI_DRAIN_MAX		16
#define SPMI_NOTIFY_MASK	0x40
#define SPMI_IRQ_ACK		0x80
#define PMU_SID			0xb

#define PERF_BUSY		BIT(31)
#define PERF_ACTUAL		GENMASK(11, 8)

#define AMCC_MAX_GROUPS		8
#define AMCC_ERR_OFF		0x25000
#define AMCC_PLANE_STRIDE	0x40000
#define AMCC_MIN_SIZE		0x1c2848

#define DCS_CH_STRIDE		0x200000UL
#define DCS_CH_SIZE		0x40

#define PTD_SOC_DEV_REQ_READ	0x01e00
#define PTD_SOC_DEV_REQ_WRITE	0x10f00
#define PTD_CLUSTER_WRITE_0	0x10f80
#define PTD_CLUSTER_WRITE_1	0x10f88
#define PTD_CLUSTER_WRITE_2	0x10f90
#define PTD_CLUSTER_WRITE_3	0x10fa0
#define PTD_MIN_SIZE		0x10fa8

#define AOP_PS_WORDS		44

#define T6041_GP_PORTS		4

#define WAKE_WORDS		9
#define WAKE_LAST_MASK		0x00003fffU
#define WAKE_MAX_ID		(32 * (WAKE_WORDS - 1) + 14)
#define WAKE_SOURCES_MAX	64

#define AXI2AF_ATC_COUNT	4
#define AXI2AF_DISP_FIRST	4
#define AXI2AF_DISP_COUNT	5
#define AXI2AF_REG_COUNT	10
#define AXI2AF_COUNT		15
#define AXI2AF_ANS		9
#define AXI2AF_APCIE		14

#define T6041_CPU_COUNT		16
#define T6041_CPM_COUNT		3
#define T6041_COMMIT_SIO_COUNT	2
#define T6041_COMMIT_PERIPH_COUNT 5
#define T6041_FULL_DETECT_COUNT	2
#define T6041_MAX_PMGR_BANKS	16
#define T6041_PS_MAX_DEPTH	16

enum t6041_apcie_ps {
	T6041_APCIE_ANS,
	T6041_APCIE_GP,
	T6041_APCIE_SYS_GP,
	T6041_APCIE_ST0,
	T6041_APCIE_SYS_ST0,
	T6041_APCIE_ST1,
	T6041_APCIE_SYS_ST1,
	T6041_APCIE_PHY_SW,
	T6041_APCIE_PS_COUNT,
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
	/* PMGR power-state words resolved from DT phandles. */
	void __iomem **ps_off;
	unsigned int n_off;
	void __iomem **ps_atc_off;
	unsigned int n_atc_off;
	void __iomem **ps_auto;
	unsigned long *ps_auto_mem;
	unsigned int n_auto;
	void __iomem **ps_auto_clkgate;
	unsigned long *ps_auto_clkgate_mem;
	unsigned int n_auto_clkgate;
	void __iomem **ps_auto_active;
	unsigned int n_auto_active;
	void __iomem **ps_keep_on;
	unsigned int n_keep_on;
	void __iomem **ps_restore;
	unsigned int n_restore;
	void __iomem *ps_cpu[T6041_CPU_COUNT];
	void __iomem *ps_cpm[T6041_CPM_COUNT];
	void __iomem *ps_sep;
	void __iomem *ps_spmi_a0;
	void __iomem *ps_atc_common[AXI2AF_ATC_COUNT];
	void __iomem *ps_atc_fabric;
	void __iomem *ps_atc_afi;
	void __iomem *ps_apcie[T6041_APCIE_PS_COUNT];
	void __iomem *ps_commit_sio[T6041_COMMIT_SIO_COUNT];
	void __iomem *ps_commit_periph[T6041_COMMIT_PERIPH_COUNT];
	void __iomem *ps_axi2af[AXI2AF_COUNT];
	void __iomem *ps_full_detect[T6041_FULL_DETECT_COUNT];
	void __iomem *cluster;
	void __iomem *pwrgate;
	void __iomem *misc;
	void __iomem *wdt;
	void __iomem *aon;
	void __iomem *commit;
	void __iomem *clkgen;
	void __iomem *spmi;
	void __iomem *spmi_a0;
	void __iomem *pmc;
	void __iomem *ka[2];	/* keep-alive tables, 1 from iBoot, 0 at macOS's commit */
	unsigned int ngroups;	/* AMCCs of four planes: 8 on full T6041, 6 on binned */
	void __iomem *dcs[4 * AMCC_MAX_GROUPS];
	void __iomem *amccb[AMCC_MAX_GROUPS];
	void __iomem *wake;	/* SLP_DDR +0, wake masks +0x0c..+0x2c */
	void __iomem *perf;	/* SoC perf +0, DCS perf +0x400 */
	void __iomem *axi2af[AXI2AF_COUNT];
	void __iomem *ptd;	/* PTD read (+0x0) and write (+0x10000) pages */
	u32 wake_masks[WAKE_WORDS];
	int stop_ret;
	int coproc_ret;
	int apcie_ret;
	unsigned int atc_late;
	unsigned int off_late;
	unsigned int dcs_late;
	u32 cpu_off_late;
	u32 cpm_off_late;
	unsigned int spmi_late;
	unsigned int ps_late;
	unsigned int gp_late;
	/* Saved as the commit starts, put back by t6041_sleep_return(). */
	u64 cntkctl;
	u64 siq_cfg;
	u32 *ps_pre;
	u32 pre_perf[2];
	u32 pre_pmc[5];
	u32 pre_clk[4];
	u32 pre_mask[2];
	u32 pre_wake[WAKE_WORDS];
	u32 pre_votes[5];
	u32 pre_disp_cluster[2];
	/* Each AMCC plane's cache ways (+0x2800) and status (+0x2804). */
	u32 pre_ways[4 * AMCC_MAX_GROUPS];
	u32 pre_cache[4 * AMCC_MAX_GROUPS];
	bool pre_a0;
	/*
	 * GP PCIe ports: port block, PHY word, and saved state. Ports 0/1 are
	 * required; 2/3 are restored when described and set up.
	 */
	void __iomem *gp_port[T6041_GP_PORTS];
	void __iomem *gp_phy[T6041_GP_PORTS];
	u32 gp_phy_pre[T6041_GP_PORTS];
	u32 gp_app_pre[T6041_GP_PORTS];
	u32 gp_perst_pre[T6041_GP_PORTS];
	unsigned int gp_up;
	bool gp_saved;
	/* ANS's storage PCIe block and bootloader-supplied tunables (apple,apcie). */
	void __iomem *ap_rc;
	void __iomem *ap_phy;
	void __iomem *ap_phy_ip;
	void __iomem *ap_axi;
	void __iomem *ap_cio3pll;
	void __iomem *ap_clkgen;
	struct apple_tunable *tun_axi2af;
	struct apple_tunable *tun_cio3pll;
	struct apple_tunable *tun_clkgen;
	struct apple_tunable *tun_phy;
	struct apple_tunable *tun_phy_ip_pll;
	struct apple_tunable *tun_phy_ip_lanes;
	bool apcie_ready;
	bool ans_stopped;
	/* The AOP island's 44 MiniPMGR power words (ps-aop). */
	void __iomem *aop_ps;
	u32 aop_pre[AOP_PS_WORDS];
	/* AP GPIO block (apple,pinctrl), saved/restored across deep sleep. */
	void __iomem *gpio;
	u32 npins;
	u32 *gpio_pre;
	unsigned long *gpio_skip;
};

static struct t6041_sleep *t6041_sleep;

/* ADT cpu number: E 0-3, P0 4-9, P1 10-15. */
static unsigned int t6041_cpu_id(u64 mpidr)
{
	unsigned int cl = MPIDR_AFFINITY_LEVEL(mpidr, 1);
	unsigned int core = MPIDR_AFFINITY_LEVEL(mpidr, 0);

	return cl == 0 ? core : cl == 1 ? 4 + core : 10 + core;
}

/*
 * Pure counter + cpu_relax() busy-spin helpers for the sleep commit path.
 * Never call udelay()/mdelay()/readl_poll_timeout_atomic() once CPU/CPM
 * PS_TARGET = 0 or cluster power-down is armed: arm64's delay_loop uses
 * WFIT/WFET, which lets the sleep hardware power-gate the boot core early.
 */
static void t6041_spin_us(u64 us)
{
	u64 t0 = arch_timer_read_counter();
	u64 ticks = DIV_ROUND_UP_ULL(us * arch_timer_get_cntfrq(), USEC_PER_SEC);

	while (arch_timer_read_counter() - t0 < ticks)
		cpu_relax();
}

#define t6041_poll_until_us(addr, val, cond, timeout_us) ({			\
	u64 __t0 = arch_timer_read_counter();					\
	u64 __ticks = DIV_ROUND_UP_ULL((u64)(timeout_us) *			\
				       arch_timer_get_cntfrq(), USEC_PER_SEC);	\
	bool __timed_out = false;						\
	int __ret = 0;								\
										\
	for (;;) {								\
		(val) = readl(addr);						\
		if (cond)							\
			break;							\
		if (__timed_out) {						\
			__ret = -ETIMEDOUT;					\
			break;							\
		}								\
		if (arch_timer_read_counter() - __t0 >= __ticks)		\
			__timed_out = true;					\
		else								\
			cpu_relax();						\
	}									\
	__ret;									\
})

/*
 * A core's last WFI, as the Neo's and the Air's t8140_deep_wfi() and macOS's
 * last-WFI routine with x0 = 1: timers masked, the deep-sleep bits (C6_2 bit 0
 * and 63) set, then WFI for good. (Not the runtime offline branch, which leaves
 * C6_2 clear.)
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

/* One byte to a PMU register over nub-spmi0: command, then data. */
static void t6041_pmu_write(struct t6041_sleep *s, u16 reg, u8 val)
{
	unsigned int i;
	u32 status;

	/* Drain any stale reply before issuing our command. */
	for (i = 0; i < SPMI_DRAIN_MAX && !(readl(s->spmi + SPMI_STATUS) & SPMI_RX_EMPTY); i++)
		readl(s->spmi + SPMI_REPLY);
	writel((u32)reg << 16 | BIT(15) | PMU_SID << 8 | 0x30, s->spmi + SPMI_CMD);
	writel(val, s->spmi + SPMI_CMD);
	if (t6041_poll_until_us(s->spmi + SPMI_STATUS, status,
				!(status & SPMI_RX_EMPTY), 1000))
		s->spmi_late++;
	for (i = 0; i < SPMI_DRAIN_MAX && !(readl(s->spmi + SPMI_STATUS) & SPMI_RX_EMPTY); i++)
		readl(s->spmi + SPMI_REPLY);
	writel(1, s->spmi + SPMI_IRQ_ACK);
}

static void cpm_gates(struct t6041_sleep *s)
{
	writel(0x80000000, s->pwrgate + 0x100);
	writel(0x80000000, s->pwrgate + 0x108);
	writel(0x80000000, s->pwrgate + 0x110);
}

static void t6041_ps_match_list(void __iomem *const *regs,
				const unsigned long *may_be_fused,
				unsigned int count, u32 mask, u32 want)
{
	unsigned int i;

	for (i = 0; i < count; i++) {
		u32 now = readl(regs[i]);

		if (may_be_fused && test_bit(i, may_be_fused) &&
		    !(now & (PS_TARGET | PS_ACTUAL)))
			continue;
		if ((now & mask) != want)
			writel((now & ~mask) | want, regs[i]);
	}
}

/*
 * Put every matched PS word into its macOS commit mode: hardware AUTO (off,
 * clock-gated, or active) or manual active. Only fused-off AMCC/DCS channel
 * slices on 24-channel binned parts (reading 0 for both target and actual)
 * are skipped; idle non-channel domains such as fab6_soc must still enter
 * hardware AUTO.
 */
static void t6041_ps_match_all(struct t6041_sleep *s)
{
	const u32 auto_mask = PS_TARGET | PS_AUTO_ENABLE | PS_AUTO_STATE;

	t6041_ps_match_list(s->ps_auto, s->ps_auto_mem, s->n_auto, auto_mask,
			    PS_TARGET | PS_AUTO_ENABLE | FIELD_PREP(PS_AUTO_STATE, 0));
	t6041_ps_match_list(s->ps_auto_clkgate, s->ps_auto_clkgate_mem,
			    s->n_auto_clkgate, auto_mask,
			    PS_TARGET | PS_AUTO_ENABLE | FIELD_PREP(PS_AUTO_STATE, 4));
	t6041_ps_match_list(s->ps_auto_active, NULL, s->n_auto_active, auto_mask,
			    PS_TARGET | PS_AUTO_ENABLE | FIELD_PREP(PS_AUTO_STATE, 0xf));
	t6041_ps_match_list(s->ps_keep_on, NULL, s->n_keep_on,
			    PS_TARGET | PS_AUTO_ENABLE, PS_TARGET);
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

/*
 * Raise each present core and all three CPM PS words to on, then drop their
 * targets to 0 so the hardware drops PS_ACTUAL when each core and cluster
 * enters deep WFI. Clear any stray target/auto bits on absent/fused-off cores
 * (e.g. pcpu0_5 / pcpu1_5 on 14-core binned parts) without raising them.
 */
static void t6041_cpu_ps_match(struct t6041_sleep *s)
{
	const u32 cpu_clear = PS_TARGET | PS_AUTO_ENABLE | PS_AUTO_STATE;
	DECLARE_BITMAP(present, T6041_CPU_COUNT);
	unsigned int cpu, i;
	u32 v;

	bitmap_zero(present, T6041_CPU_COUNT);
	for_each_present_cpu(cpu) {
		unsigned int id = t6041_cpu_id(cpu_logical_map(cpu));
		void __iomem *reg = s->ps_cpu[id];

		__set_bit(id, present);
		v = readl(reg);
		writel((v & ~cpu_clear) | PS_TARGET, reg);
		t6041_poll_until_us(reg, v, FIELD_GET(PS_ACTUAL, v) == 0xf, 2000);
	}
	for (i = 0; i < T6041_CPM_COUNT; i++) {
		void __iomem *reg = s->ps_cpm[i];

		v = readl(reg);
		writel((v & ~cpu_clear) | PS_TARGET, reg);
		t6041_poll_until_us(reg, v, FIELD_GET(PS_ACTUAL, v) == 0xf, 2000);
	}
	for (i = 0; i < T6041_CPU_COUNT; i++) {
		void __iomem *reg = s->ps_cpu[i];

		if (test_bit(i, present)) {
			writel(readl(reg) & ~PS_TARGET, reg);
		} else {
			v = readl(reg);
			if (v & cpu_clear)
				writel(v & ~cpu_clear, reg);
		}
	}
	for (i = 0; i < T6041_CPM_COUNT; i++) {
		void __iomem *reg = s->ps_cpm[i];

		writel(readl(reg) & ~PS_TARGET, reg);
	}
}

/*
 * Lower every USB-C port's power domains in macOS's deepest-first order
 * (phymxwrap -> usb -> usb_aon -> common_dp, then cio/pcie -> common) and
 * wait for each domain to reach PS_ACTUAL == 0, then hand fab2_soc and afi
 * to hardware AUTO (forcing them off hangs T6041).
 */
static void t6041_atc_down(struct t6041_sleep *s)
{
	unsigned int i;
	u32 v;

	for (i = 0; i < s->n_atc_off; i++)
		t6041_ps_lower(s->ps_atc_off[i]);
	for (i = 0; i < s->n_atc_off; i++) {
		if (t6041_poll_until_us(s->ps_atc_off[i], v,
					!FIELD_GET(PS_ACTUAL, v), 10000))
			s->atc_late++;
	}
	writel(readl(s->ps_atc_fabric) | PS_AUTO_ENABLE, s->ps_atc_fabric);
	writel(readl(s->ps_atc_afi) | PS_AUTO_ENABLE, s->ps_atc_afi);
}

/*
 * Raise fab2_soc manually, raise all four atcN_common domains, then re-enable
 * AUTO on fab2_soc so the sleep engine's ATC step never stalls on a gated
 * parent fabric.
 */
static int t6041_ps_raise_start(void __iomem *reg)
{
	u32 v;

	writel((readl(reg) & ~(PS_AUTO_ENABLE | GENMASK(11, 0))) | PS_TARGET, reg);
	return t6041_poll_until_us(reg, v, (v & PS_ACTUAL) == PS_ACTUAL, 5000);
}

static void t6041_atc_commons_on(struct t6041_sleep *s)
{
	void __iomem *fab2 = s->ps_atc_fabric;
	unsigned int i;

	t6041_ps_raise_start(fab2);
	for (i = 0; i < AXI2AF_ATC_COUNT; i++)
		t6041_ps_raise_start(s->ps_atc_common[i]);
	writel((readl(fab2) & ~(PS_AUTO_ENABLE | GENMASK(11, 0))) | PS_TARGET | PS_AUTO_ENABLE,
	       fab2);
}

/*
 * Disable DSID age-out (word 0 bit 25) and unexpected RT hit (word 1 bit 8)
 * error enables on every AMCC plane and clear any latched error status words.
 */
static void t6041_amcc_quiesce(struct t6041_sleep *s)
{
	unsigned int g, p, w;

	for (g = 0; g < s->ngroups; g++) {
		for (p = 0; p < 4; p++) {
			void __iomem *a = s->amccb[g] + AMCC_ERR_OFF + AMCC_PLANE_STRIDE * p;

			writel(readl(a + 0x0) & ~BIT(25), a + 0x0);
			writel(readl(a + 0x8) & ~BIT(8), a + 0x8);
			for (w = 0; w < 7; w++) {
				u32 v = readl(a + 4 + 8 * w);

				if (v)
					writel(v, a + 4 + 8 * w);
			}
		}
	}
	dsb(sy);
}

/*
 * Flush AMCC caches and enable DCS dynamic power gating on every enabled
 * channel while the AMCC slices are still forced on.
 */
static void t6041_mcc_macos(struct t6041_sleep *s)
{
	unsigned int i, p;
	u32 v;

	for (i = 0; i < s->ngroups; i++) {
		/* +0x1c0000 is the AMCC all-planes write window; poll per plane. */
		writel(1, s->amccb[i] + 0x1c082c);
		writel(1, s->amccb[i] + 0x1c2844);
		for (p = 0; p < 4; p++)
			t6041_poll_until_us(s->amccb[i] + AMCC_PLANE_STRIDE * p + 0x2844,
					    v, !(v & BIT(0)), 100000);
	}
	dsb(sy);
	for (i = 0; i < 4 * s->ngroups; i++)
		writel(readl(s->dcs[i] + 0x20) | 0x1100, s->dcs[i] + 0x20);
	dsb(sy);
}

static const char *const t6041_axi2af_names[AXI2AF_REG_COUNT] = {
	"axi2af-disp",
	"axi2af-dispext0",
	"axi2af-dispext1",
	"axi2af-dispext2",
	"axi2af-dispext3",
	"axi2af-ans",
	"axi2af-avd",
	"axi2af-venc0",
	"axi2af-venc1",
	"axi2af-jpg",
};

static void t6041_wake_sep(struct t6041_sleep *s)
{
	unsigned int i;
	u32 v;

	if (readl(s->wake + 0x0))
		writel(0, s->wake + 0x0);
	for (i = 0; i < WAKE_WORDS; i++)
		writel(s->wake_masks[i], s->wake + 0xc + 4 * i);

	v = readl(s->ps_sep);
	if (!(v & BIT(10)))
		writel(v | BIT(10), s->ps_sep);
	dsb(sy);
}

/* A perf word: the low nibble asks, [11:8] is the state reached, bit 31 busy. */
static bool t6041_perf_set(void __iomem *reg, u32 state)
{
	u32 v;

	if (FIELD_GET(PERF_ACTUAL, readl(reg)) == state)
		return true;
	writel((readl(reg) & ~0xf) | state, reg);
	return !t6041_poll_until_us(reg, v,
				    !(v & PERF_BUSY) && FIELD_GET(PERF_ACTUAL, v) == state,
				    5000);
}

/* SoC to 3, voters 0, DCS to 6: macOS commits at DCS 6 (0x7666); the engine takes it to 4. */
static void t6041_mem_down(struct t6041_sleep *s)
{
	unsigned int i;

	t6041_perf_set(s->perf + 0x0, 3);
	for (i = 0; i < 4; i++)
		writel(0, s->pmc + i * 0x4000);
	t6041_perf_set(s->perf + 0x400, 6);
}

/*
 * Cluster power-control words: +0 E, +4 P0, +8 P1, +0xc and +0x10 the two AVE
 * complexes. Clear bit 5 (CLUSTER_UP_DONE) and take both AVE complexes to 0.
 */
static void t6041_ave_off(struct t6041_sleep *s)
{
	static const u16 ave[] = { 0xc, 0x10 };
	unsigned int i;
	u32 v;

	for (i = 0; i < ARRAY_SIZE(ave); i++) {
		writel(BIT(5), s->cluster + ave[i]);
		t6041_poll_until_us(s->cluster + ave[i], v, !v, 5000);
	}
	v = readl(s->cluster + 0x8);
	if (v & BIT(5))
		writel((v & 0xf) | BIT(5), s->cluster + 0x8);
}

static void t6041_save(struct t6041_sleep *s);

/*
 * Lower a domain with two writes (AUTO off with target still f, then
 * target 0), only when it is currently on so bit 11 does not latch.
 */
static void t6041_ps_lower_pair(void __iomem *reg, u32 first, u32 second)
{
	u32 v = readl(reg);

	if (!FIELD_GET(PS_TARGET, v) && !FIELD_GET(PS_ACTUAL, v))
		return;
	writel(first, reg);
	writel(second, reg);
}

/*
 * Pulse bit 33 of SOC-DEV-PS-REQ (PTD write +0x10f00) after ANS lowers and
 * before arming WD2, matching macOS's traced -10 ms to -4 ms pulse (~6 ms).
 * PMP is already halted by apple_sleep_coprocs_stop_all() before commit, so
 * its PTD report page provides no completion/ACK bit to poll; read back
 * SOC-DEV-PS-REQ (+0x1e00) to post the write before holding the 5 ms pulse.
 */
static void t6041_ptd_soc_dev_pulse(struct t6041_sleep *s)
{
	writeq(0x200000800ULL, s->ptd + PTD_SOC_DEV_REQ_WRITE);
	readq(s->ptd + PTD_SOC_DEV_REQ_READ);
	t6041_spin_us(5000);
	writeq(0x800ULL, s->ptd + PTD_SOC_DEV_REQ_WRITE);
}

static int t6041_sleep_commit(unsigned long unused)
{
	struct t6041_sleep *s = t6041_sleep;
	unsigned int i, cpu;
	u32 v;

	t6041_save(s);
	s->stop_ret = spin_table_stop_parked();
	t6041_ave_off(s);
	/* While every AMCC slice is still on: a clock-gated plane faults. */
	t6041_amcc_quiesce(s);
	t6041_mcc_macos(s);
	/*
	 * Disarm AXI2AF bridges on powered blocks before lowering them so the
	 * power domain does not latch bit 11 on subsequent sleeps.
	 */
	for (i = 0; i < AXI2AF_COUNT; i++) {
		if (!s->axi2af[i])
			continue;
		if (FIELD_GET(PS_ACTUAL, readl(s->ps_axi2af[i])) != 0xf)
			continue;
		v = readl(s->axi2af[i] + 0x4);
		if (!(v & 1) && !(readl(s->axi2af[i] + 0x8) & 1))
			continue;
		writel(v & ~1U, s->axi2af[i] + 0x4);
		writel(readl(s->axi2af[i] + 0x8) & ~1U, s->axi2af[i] + 0x8);
	}
	t6041_cpu_ps_match(s);
	t6041_mem_down(s);

	/*
	 * Lower child-before-parent sorted domains and wait for each domain to
	 * reach PS_ACTUAL == 0 before moving on so no parent is ever lowered
	 * while a child is still powering off (which latches bit 11).
	 */
	for (i = 0; i < s->n_off; i++) {
		t6041_ps_lower(s->ps_off[i]);
		if (t6041_poll_until_us(s->ps_off[i], v,
					!FIELD_GET(PS_ACTUAL, v), 5000))
			s->off_late++;
	}

	t6041_ps_match_all(s);
	/* Every PMC voter at 0, and the PMS word macOS holds at 0. */
	for (i = 0; i < 4; i++)
		writel(0, s->pmc + i * 0x4000);
	writel(0, s->pmc + 0xa9c18);
	readl(s->pmc + 0xa9c18);
	if (t6041_poll_until_us(s->perf + 0x400, v,
				!(v & PERF_BUSY) && FIELD_GET(PERF_ACTUAL, v) <= 6, 20000) ||
	    t6041_poll_until_us(s->perf + 0x0, v,
				!(v & PERF_BUSY) && FIELD_GET(PERF_ACTUAL, v) <= 3, 20000))
		s->dcs_late++;
	/* Cluster PTD votes, no demand. */
	writeq(0x2000000000000000ULL, s->ptd + PTD_CLUSTER_WRITE_0);
	writeq(0x2000000000000000ULL, s->ptd + PTD_CLUSTER_WRITE_1);
	writeq(0x2000000000000000ULL, s->ptd + PTD_CLUSTER_WRITE_2);
	writeq(0x2000000000000000ULL, s->ptd + PTD_CLUSTER_WRITE_3);
	t6041_atc_down(s);
	t6041_wake_sep(s);

	writel(0xffef, s->misc + 0x0);
	/*
	 * Mask AIC before releasing parked secondary cores into their final
	 * deep WFI, then poll each non-boot core and secondary CPM PS word
	 * until hardware drops PS_ACTUAL to 0.
	 */
	aic_sleep_mask_all();
	spin_table_release_stopped();
	for_each_present_cpu(cpu) {
		unsigned int id;

		if (cpu == smp_processor_id())
			continue;
		id = t6041_cpu_id(cpu_logical_map(cpu));
		if (t6041_poll_until_us(s->ps_cpu[id], v,
					!FIELD_GET(PS_ACTUAL, v), 50000))
			s->cpu_off_late |= BIT(id);
	}
	for (i = 0; i < T6041_CPM_COUNT; i++) {
		if (i == MPIDR_AFFINITY_LEVEL(cpu_logical_map(smp_processor_id()), 1))
			continue;
		if (t6041_poll_until_us(s->ps_cpm[i], v,
					!FIELD_GET(PS_ACTUAL, v), 50000))
			s->cpm_off_late |= BIT(i);
	}

	writel(0x3f833ff1, s->spmi + SPMI_NOTIFY_MASK);
	/* PMU boot stage to 0x50 (asleep). */
	t6041_pmu_write(s, 0xf801, 0x50);
	writel(0x3f0, s->ps_commit_periph[0]);
	writel(0x3f0, s->ps_commit_periph[1]);
	writel(0x3f0, s->ps_commit_periph[2]);
	writel(0x3f0, s->ps_commit_periph[3]);
	writel(0x2000001, s->spmi + SPMI_NOTIFY_MASK);
	if (FIELD_GET(PS_ACTUAL, readl(s->ps_spmi_a0)))
		writel(0x2000001, s->spmi_a0 + SPMI_NOTIFY_MASK);
	writel(0x3f0, s->ps_commit_periph[4]);
	cpm_gates(s);
	writel(0, s->cluster + 0x0);
	cpm_gates(s);
	writel(0, s->cluster + 0x8);
	t6041_ps_lower_pair(s->ps_apcie[T6041_APCIE_PHY_SW], 0x400004f, 0x4000340);
	t6041_ps_lower_pair(s->ps_apcie[T6041_APCIE_SYS_ST1], 0xf, 0x300);
	writel(0x3f0, s->ps_apcie[T6041_APCIE_ST1]);
	t6041_ps_lower_pair(s->ps_apcie[T6041_APCIE_SYS_ST0], 0xf0000ff, 0xf0003f0);
	writel(0x3f0, s->ps_apcie[T6041_APCIE_ST0]);
	t6041_ps_lower_pair(s->ps_apcie[T6041_APCIE_SYS_GP], 0xf, 0x300);
	writel(0x3f0, s->ps_apcie[T6041_APCIE_GP]);
	t6041_ps_lower_pair(s->ps_apcie[T6041_APCIE_ANS], 0xf0000ff, 0xf0003f0);
	/* macOS -10 ms to -4 ms before arming WD2: pulse PTD SOC-DEV-PS-REQ bit 33. */
	t6041_ptd_soc_dev_pulse(s);
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
	writel(0x400024f, s->ps_commit_sio[0]);
	writel(0x400024f, s->ps_commit_sio[1]);
	writel(0xa0100000, s->clkgen + 0x60);
	writel(0xa0100000, s->clkgen + 0x64);
	writel(0xa0100000, s->clkgen + 0x54);
	writel(0xa0100000, s->clkgen + 0x50);
	/* aura reads this with the MMU off on the way back. */
	WRITE_ONCE(s->mbox->resume_pa, __pa_symbol(cpu_resume));
	dcache_clean_poc((unsigned long)s->mbox, (unsigned long)(s->mbox + 1));
	t6041_atc_commons_on(s);
	/*
	 * Confirm the SPMI controller's TX and RX FIFOs are idle after the
	 * PMU boot-stage write before the sleep engine takes over SPMI.
	 */
	if (t6041_poll_until_us(s->spmi + SPMI_STATUS, v,
				(v & SPMI_IDLE) == SPMI_IDLE, 5000))
		s->spmi_late++;
	/*
	 * Keep SPMI quiet before clearing keep-alive tables and committing
	 * sleep, matching macOS and the sleep engine's bus handoff window.
	 */
	t6041_spin_us(20000);
	/* Keep-alive tables off right before commit. */
	for (i = 0; i < KA_WORDS; i++) {
		writel(0, s->ka[0] + i * KA_STRIDE);
		writel(0, s->ka[1] + i * KA_STRIDE);
	}
	dsb(sy);
	writel(1, s->commit);
	dsb(sy);
	/*
	 * Secondary cores and non-boot CPMs are already polled off in deep WFI.
	 * Mask AIC again and hold the 3 ms commit-to-boot-WFI gap (matching
	 * sleep.c's boot_wfi_delay_ms = 3 on T8132/T8142; no hardware status
	 * bit reports commit-armed, and pure counter spin avoids WFET early
	 * gating) before entering the boot core's final deep WFI.
	 */
	aic_sleep_mask_all();
	t6041_spin_us(3000);
	t6041_boot_wfi(s);
}

/*
 * Display pipe PMC votes saved before commit and restored on wake (offsets into
 * the PMC register block).
 */
static const u32 t6041_pmc_vote_off[5] = { 0xac028, 0xa4000, 0xac020, 0xa9c10, 0xa9c20 };

/*
 * Restore AMCC cache mode after wake: write way count through the all-planes
 * window, then poll each plane's status word.
 */
static void t6041_mcc_active(struct t6041_sleep *s)
{
	unsigned int i, p;
	u32 v;

	for (i = 0; i < s->ngroups; i++) {
		writel(s->pre_ways[4 * i], s->amccb[i] + 0x1c2800);
		for (p = 0; p < 4; p++)
			readl_poll_timeout_atomic(s->amccb[i] + AMCC_PLANE_STRIDE * p + 0x2804, v,
						  v == s->pre_cache[4 * i + p], 10, 100000);
	}
}

/* Restore display PMC votes; wait for the DVMR ack (+0x20). */
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

static const u16 t6041_clk_mux[4] = { 0x50, 0x54, 0x64, 0x60 };

static void t6041_save(struct t6041_sleep *s)
{
	unsigned int i;

	for (i = 0; i < 4 * s->ngroups; i++) {
		void __iomem *plane = s->amccb[i / 4] + AMCC_PLANE_STRIDE * (i % 4);

		s->pre_ways[i] = readl(plane + 0x2800);
		s->pre_cache[i] = readl(plane + 0x2804);
	}
	for (i = 0; i < ARRAY_SIZE(t6041_pmc_vote_off); i++)
		s->pre_votes[i] = readl(s->pmc + t6041_pmc_vote_off[i]);
	s->pre_disp_cluster[0] = readl(s->cluster + 0x0c);
	s->pre_disp_cluster[1] = readl(s->cluster + 0x10);
	s->cntkctl = read_sysreg(cntkctl_el1);
	s->siq_cfg = read_sysreg_s(SYS_APL_SIQ_CFG_EL1);
	for (i = 0; i < s->n_restore; i++)
		s->ps_pre[i] = readl(s->ps_restore[i]);
	s->pre_perf[0] = readl(s->perf + 0x0);
	s->pre_perf[1] = readl(s->perf + 0x400);
	for (i = 0; i < 4; i++)
		s->pre_pmc[i] = readl(s->pmc + i * 0x4000);
	s->pre_pmc[4] = readl(s->pmc + 0xa9c18);
	for (i = 0; i < 4; i++)
		s->pre_clk[i] = readl(s->clkgen + t6041_clk_mux[i]);
	s->pre_mask[0] = readl(s->spmi + SPMI_NOTIFY_MASK);
	s->pre_a0 = FIELD_GET(PS_ACTUAL, readl(s->ps_spmi_a0)) != 0;
	if (s->pre_a0)
		s->pre_mask[1] = readl(s->spmi_a0 + SPMI_NOTIFY_MASK);
	for (i = 0; i < WAKE_WORDS; i++)
		s->pre_wake[i] = readl(s->wake + 0xc + 4 * i);
	for (i = 0; i < AOP_PS_WORDS; i++)
		s->aop_pre[i] = readl(s->aop_ps + 8 * i);
	for (i = 0; i < s->npins; i++) {
		if (test_bit(i, s->gpio_skip))
			continue;
		s->gpio_pre[i] = readl(s->gpio + 4 * i);
	}
	/* Only with the GP PCIe block on: apcie_gp and apcie_sys_gp. */
	s->gp_saved = s->gp_port[0] && s->gp_port[1] && s->gp_phy[0] && s->gp_phy[1] &&
		      FIELD_GET(PS_ACTUAL, readl(s->ps_apcie[T6041_APCIE_GP])) == 0xf &&
		      FIELD_GET(PS_ACTUAL, readl(s->ps_apcie[T6041_APCIE_SYS_GP])) == 0xf;
	s->gp_up = 0;
	for (i = 0; i < T6041_GP_PORTS && s->gp_saved; i++) {
		if (!s->gp_port[i] || !s->gp_phy[i])
			continue;
		s->gp_phy_pre[i] = readl(s->gp_phy[i]);
		s->gp_app_pre[i] = readl(s->gp_port[i] + 0x800);
		s->gp_perst_pre[i] = readl(s->gp_port[i] + 0x82c);
		/* The PCIe driver requests a port's refclk when it sets the port up. */
		if (s->gp_phy_pre[i] & BIT(0))
			s->gp_up |= BIT(i);
	}
}

/*
 * Restore all AP GPIO pin configurations lost across deep sleep, skipping pins
 * listed in apple,pinctrl-skip-pins.
 */
static void t6041_gpio_restore(struct t6041_sleep *s)
{
	unsigned int i;

	for (i = 0; i < s->npins; i++) {
		if (test_bit(i, s->gpio_skip))
			continue;
		if (readl(s->gpio + 4 * i) != s->gpio_pre[i])
			writel(s->gpio_pre[i], s->gpio + 4 * i);
	}
}

/*
 * AOP island MiniPMGR power words (ps-aop). Walked backwards (parents first)
 * on wake so every previously-on domain returns to manual target f.
 */
static const u16 t6041_aop_ps_order[] = {
	0x0b0, 0x128, 0x120, 0x158, 0x150, 0x148, 0x140, 0x138, 0x130, 0x0c0, 0x0c8,
	0x0e8, 0x0f0, 0x0f8, 0x100, 0x108, 0x110, 0x118, 0x0e0, 0x0d8, 0x0d0, 0x0b8, 0x090,
	0x020, 0x098, 0x0a0, 0x0a8, 0x028, 0x030, 0x068, 0x070, 0x078, 0x080, 0x038, 0x040,
	0x048, 0x058, 0x060, 0x088, 0x050, 0x010, 0x018, 0x008, 0x000,
};

static_assert(ARRAY_SIZE(t6041_aop_ps_order) == AOP_PS_WORDS);

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

static int t6041_ps_raise(void __iomem *reg)
{
	int ret = t6041_ps_raise_start(reg);

	writel((readl(reg) & ~(PS_AUTO_ENABLE | GENMASK(11, 0))) | PS_TARGET | PS_AUTO_ENABLE, reg);
	return ret;
}

/*
 * Bring ANS's storage PCIe link back from power-off using the DT-supplied
 * apple,tunable-* tables from apple,apcie. Returns 0, or the step (1-8) that
 * timed out. Runs only when the NVMe driver stopped ANS for this sleep.
 */
static int t6041_apcie_raise(struct t6041_sleep *s)
{
	void __iomem *ctrl = s->ap_phy + 0x8000;
	void __iomem *run = s->ap_phy + 0x8004;
	u32 v;
	int ret;

	ret = t6041_ps_raise(s->ps_apcie[T6041_APCIE_ANS]);
	if (!ret) {
		writel(readl(s->axi2af[AXI2AF_ANS] + 0x4) | 1, s->axi2af[AXI2AF_ANS] + 0x4);
		writel(readl(s->axi2af[AXI2AF_ANS] + 0x8) | 1, s->axi2af[AXI2AF_ANS] + 0x8);
	}
	ret |= t6041_ps_raise(s->ps_apcie[T6041_APCIE_GP]);
	if (!ret)
		ret = t6041_ps_raise_start(s->ps_apcie[T6041_APCIE_SYS_GP]);
	if (!ret) {
		writel(readl(s->ap_axi + 0x104) | 1, s->ap_axi + 0x104);
		writel(readl(s->ap_axi + 0x108) | 1, s->ap_axi + 0x108);
	}
	ret |= t6041_ps_raise(s->ps_apcie[T6041_APCIE_ST0]);
	ret |= t6041_ps_raise(s->ps_apcie[T6041_APCIE_SYS_ST0]);
	ret |= t6041_ps_raise(s->ps_apcie[T6041_APCIE_ST1]);
	ret |= t6041_ps_raise(s->ps_apcie[T6041_APCIE_SYS_ST1]);
	if (ret)
		return 1;

	apple_tunable_apply(s->ap_axi, s->tun_axi2af);
	apple_tunable_apply(s->ap_cio3pll, s->tun_cio3pll);
	apple_tunable_apply(s->ap_clkgen, s->tun_clkgen);
	writel(readl(s->ap_clkgen + 0x4) | BIT(31), s->ap_clkgen + 0x4);
	writel(readl(s->ap_clkgen + 0x0) & ~(BIT(2) | BIT(1)), s->ap_clkgen + 0x0);
	if (readl_poll_timeout_atomic(s->ap_clkgen + 0x0, v, !(v & BIT(31)), 1, 50000))
		return 2;
	if (t6041_ps_raise(s->ps_apcie[T6041_APCIE_PHY_SW]))
		return 3;
	apple_tunable_apply(s->ap_phy, s->tun_phy);
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
	apple_tunable_apply(s->ap_phy_ip, s->tun_phy_ip_pll);
	apple_tunable_apply(s->ap_phy_ip, s->tun_phy_ip_lanes);
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
 * Restore the reference and app clocks of the GP PCIe ports that were set up
 * before the sleep, so their DART blocks do not raise SErrors on resume.
 * Returns a mask of late ports.
 */
static unsigned int t6041_gp_pcie_clk(struct t6041_sleep *s)
{
	unsigned int i, late = 0;
	u32 v;

	for (i = 0; i < T6041_GP_PORTS; i++) {
		void __iomem *phy = s->gp_phy[i];
		void __iomem *port = s->gp_port[i];
		u32 cfg = s->gp_phy_pre[i];
		u32 app = s->gp_app_pre[i];

		if (!s->gp_saved || !(s->gp_up & BIT(i)) ||
		    FIELD_GET(PS_ACTUAL, readl(s->ps_apcie[T6041_APCIE_GP])) != 0xf ||
		    FIELD_GET(PS_ACTUAL, readl(s->ps_apcie[T6041_APCIE_SYS_GP])) != 0xf)
			continue;
		writel(readl(phy) | BIT(0), phy);
		if (readl_poll_timeout_atomic(phy, v, v & BIT(2), 1, 50000))
			late |= BIT(i);
		writel(readl(phy) | BIT(1), phy);
		if (readl_poll_timeout_atomic(phy, v, v & BIT(3), 1, 50000))
			late |= BIT(i);
		if (!(cfg & BIT(4))) {
			writel(readl(phy) & ~BIT(4), phy);
			/* T6020+ PHY refclk buffer settle before REFCLKEN (as in pcie-apple.c). */
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
 * Restore every non-CPU/CPM PS word to its pre-commit target and AUTO state:
 * raises parents first (0..n_restore-1), then lowers children first.
 */
static unsigned int t6041_ps_restore(struct t6041_sleep *s)
{
	const u32 keep = ~(u32)(PS_AUTO_ENABLE | PS_FLAGS | PS_TARGET);
	unsigned int i, late = 0;
	u32 v;

	for (i = 0; i < s->n_restore; i++) {
		void __iomem *reg = s->ps_restore[i];
		u32 pre = s->ps_pre[i];
		u32 cur = readl(reg);
		u32 pt = pre & PS_TARGET;

		if (!(pre & (PS_TARGET | PS_ACTUAL)) && !(cur & (PS_TARGET | PS_ACTUAL)))
			continue;
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
	for (i = s->n_restore; i-- > 0;) {
		void __iomem *reg = s->ps_restore[i];
		u32 pre = s->ps_pre[i];
		u32 cur = readl(reg);
		u32 pt = pre & PS_TARGET;

		if (pt >= (cur & PS_TARGET))
			continue;
		if (cur & PS_AUTO_ENABLE) {
			cur &= ~(PS_AUTO_ENABLE | PS_FLAGS);
			writel(cur, reg);
		}
		writel((cur & keep) | pt | PS_FLAGS, reg);
		if (readl_poll_timeout_atomic(reg, v, FIELD_GET(PS_ACTUAL, v) == pt, 1, 3000))
			late++;
		if (pre & PS_AUTO_ENABLE)
			writel((readl(reg) & ~PS_FLAGS) | PS_AUTO_ENABLE, reg);
	}
	return late;
}

static u64 t6041_ret_t0, t6041_ret_t1;

/*
 * Back on the boot core through aura's mailbox: clusters on, masks back, WD2 off.
 * Hardware wake reset restores the boot CPU power state before return, so
 * udelay()/readl_poll_timeout_atomic() are safe here and in its helpers.
 */
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
	/* Restore DISP_INT (+0x0c) and DISP_EXT (+0x10, DISPEXT_RAIL_V) complexes. */
	for (i = 0; i < ARRAY_SIZE(s->pre_disp_cluster); i++) {
		u32 on = s->pre_disp_cluster[i] & 0xf;

		if (!on)
			continue;
		v = readl(s->cluster + 0x0c + 4 * i);
		writel((v & ~0xfU) | on, s->cluster + 0x0c + 4 * i);
	}
	/* The two wake gates, then the CPM voters. */
	writel(readl(s->pwrgate + 0x000) | BIT(31), s->pwrgate + 0x000);
	writel(readl(s->pwrgate + 0x040) | BIT(31), s->pwrgate + 0x040);
	cpm_gates(s);
	/* SEP DEV_DISABLE off again, bit 11 polled clear; S2R request word; wake masks. */
	v = readl(s->ps_sep);
	if (v & BIT(10))
		writel(v & ~BIT(10), s->ps_sep);
	readl_poll_timeout_atomic(s->ps_sep, v, !(v & BIT(11)), 1, 5000);
	writel(0, s->wake + 0x0);
	for (i = 0; i < WAKE_WORDS; i++)
		writel(s->pre_wake[i], s->wake + 0xc + 4 * i);
	/* Before t6041_mcc_active() touches the AMCC/DCS planes. */
	s->ps_late = t6041_ps_restore(s);
	t6041_mcc_active(s);
	t6041_aop_wake(s);
	t6041_gpio_restore(s);
	/* Re-arm AXI2AF bridges whose blocks are back on. */
	for (i = 0; i < AXI2AF_COUNT; i++) {
		if (!s->axi2af[i])
			continue;
		v = readl(s->ps_axi2af[i]);
		if ((v & PS_TARGET) != PS_TARGET || FIELD_GET(PS_ACTUAL, v) != 0xf)
			continue;
		writel(readl(s->axi2af[i] + 0x4) | 1, s->axi2af[i] + 0x4);
		writel(readl(s->axi2af[i] + 0x8) | 1, s->axi2af[i] + 0x8);
	}
	s->apcie_ret = s->ans_stopped ? t6041_apcie_raise(s) : 0;
	s->gp_late = t6041_gp_pcie_clk(s);
	/* Restore parked clock muxes (last parked first), perf states, and PMC voters. */
	for (i = 4; i-- > 0;)
		writel(s->pre_clk[i], s->clkgen + t6041_clk_mux[i]);
	t6041_perf_set(s->perf + 0x400, s->pre_perf[1] & 0xf);
	t6041_perf_set(s->perf + 0x0, s->pre_perf[0] & 0xf);
	for (i = 0; i < 4; i++)
		writel(s->pre_pmc[i], s->pmc + i * 0x4000);
	writel(s->pre_pmc[4], s->pmc + 0xa9c18);
	t6041_pmc_votes(s);
	writel(s->pre_mask[0], s->spmi + SPMI_NOTIFY_MASK);
	if (s->pre_a0 && FIELD_GET(PS_ACTUAL, readl(s->ps_spmi_a0)))
		writel(s->pre_mask[1], s->spmi_a0 + SPMI_NOTIFY_MASK);
	writel(0, s->wdt + 0x2c);
	/* PMU boot stage back from 0x50 (asleep) to 0x30 (OS running). */
	t6041_pmu_write(s, 0xf801, 0x30);
	t6041_ret_t1 = read_sysreg(cntpct_el0);
}

static int t6041_sleep_enter(suspend_state_t state)
{
	struct t6041_sleep *s = t6041_sleep;
	unsigned int i;
	int ret;

	/* With no way back, a sleep would end in aura's boot flow over this kernel. */
	if (!s->mbox)
		return -EOPNOTSUPP;
	if (WARN_ON_ONCE(!s->apcie_ready))
		return -EOPNOTSUPP;
	/*
	 * Display power chains require GAPF unprotect, child-first lowering, and
	 * fab6_soc hold (handled by apple-dcp); refuse sleep if any display block
	 * is still active (e.g., apple-drm unbound or nomodeset).
	 */
	for (i = 0; i < AXI2AF_DISP_COUNT; i++) {
		u32 v = readl(s->ps_axi2af[AXI2AF_DISP_FIRST + i]);

		if (v & (PS_TARGET | PS_ACTUAL)) {
			dev_err(s->dev, "refused: display domain %u still active (%#x)\n", i, v);
			return -EBUSY;
		}
	}
	/*
	 * Every parked CPU answers the hold while a refusal is still possible: at the commit
	 * only a held CPU stops, and one left awake there keeps the engine waiting until the
	 * watchdog resets the SoC.
	 */
	ret = spin_table_hold_parked();
	if (ret) {
		dev_err(s->dev, "refused: a parked CPU didn't answer the hold: %d\n", ret);
		return ret;
	}
	ret = apple_sleep_coprocs_check_all();
	if (ret) {
		spin_table_unhold_parked();
		return ret;
	}
	if (pm_wakeup_pending()) {
		spin_table_unhold_parked();
		return -EBUSY;
	}
	/*
	 * Send SEP SLEEP only after every refusal check has passed: a nonzero
	 * return here guarantees nothing was sent, while success gates the
	 * enclave and commits this sleep through physical wake.
	 */
	ret = apple_sleep_enclave_stop_all();
	if (ret) {
		spin_table_unhold_parked();
		return ret;
	}
	s->stop_ret = 0;
	s->apcie_ret = 0;
	s->atc_late = 0;
	s->off_late = 0;
	s->dcs_late = 0;
	s->cpu_off_late = 0;
	s->cpm_off_late = 0;
	s->spmi_late = 0;
	s->ps_late = 0;
	s->gp_late = 0;
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
	if (s->atc_late)
		dev_warn(s->dev, "%u ATC power domains did not reach off before commit\n",
			 s->atc_late);
	if (s->off_late)
		dev_warn(s->dev, "%u power domains did not reach off before commit\n",
			 s->off_late);
	if (s->dcs_late)
		dev_warn(s->dev, "DCS/SoC perf did not settle after clearing PMC voters\n");
	if (s->cpu_off_late)
		dev_warn(s->dev, "secondary CPUs %#x did not reach off before commit\n",
			 s->cpu_off_late);
	if (s->cpm_off_late)
		dev_warn(s->dev, "secondary CPMs %#x did not reach off before commit\n",
			 s->cpm_off_late);
	if (s->spmi_late)
		dev_warn(s->dev, "%u SPMI waits timed out around sleep\n", s->spmi_late);
	if (s->ps_late)
		dev_dbg(s->dev, "%u power domains took more than 3 ms to come back\n", s->ps_late);
	if (s->apcie_ret)
		dev_warn(s->dev, "storage link raise timed out at step %d\n", s->apcie_ret);
	if (s->gp_late)
		dev_warn(s->dev, "GP PCIe ports %#x didn't come back\n", s->gp_late);
}

static int t6041_sleep_begin(suspend_state_t state)
{
	/* The PM core also calls .begin for s2idle when no s2idle ops are installed. */
	if (state != PM_SUSPEND_MEM)
		return 0;
	pm_set_suspend_via_firmware();
	return 0;
}

static const struct platform_suspend_ops t6041_sleep_ops = {
	.valid = suspend_valid_only_mem,
	.begin = t6041_sleep_begin,
	.end = t6041_sleep_end,
	.enter = t6041_sleep_enter,
};

static void t6041_iounmap(void *base)
{
	iounmap((void __iomem *)base);
}

static void __iomem *t6041_map_idx(struct device *dev, struct device_node *np,
				   int idx, resource_size_t min_size,
				   resource_size_t *out_size)
{
	struct resource res;
	void __iomem *base;
	int ret;

	if (idx < 0 || of_address_to_resource(np, idx, &res))
		return IOMEM_ERR_PTR(-EINVAL);
	if (resource_size(&res) < min_size)
		return IOMEM_ERR_PTR(-EINVAL);
	/* Shared with other drivers, so map without claiming the region. */
	base = of_iomap(np, idx);
	if (!base)
		return IOMEM_ERR_PTR(-ENOMEM);
	ret = devm_add_action_or_reset(dev, t6041_iounmap, (void __force *)base);
	if (ret)
		return IOMEM_ERR_PTR(ret);
	if (out_size)
		*out_size = resource_size(&res);
	return base;
}

static void __iomem *t6041_map_name(struct device *dev, struct device_node *np,
				    const char *name, resource_size_t min_size,
				    resource_size_t *out_size)
{
	int idx = of_property_match_string(np, "reg-names", name);

	if (idx < 0)
		return IOMEM_ERR_PTR(idx);
	return t6041_map_idx(dev, np, idx, min_size, out_size);
}

struct t6041_pmgr_bank {
	struct device_node *np;
	void __iomem *base;
	resource_size_t size;
};

struct t6041_ps_resolver {
	struct device *dev;
	struct t6041_pmgr_bank banks[T6041_MAX_PMGR_BANKS];
	unsigned int nbanks;
};

static void t6041_ps_resolver_cleanup(struct t6041_ps_resolver *r)
{
	unsigned int i;

	for (i = 0; i < r->nbanks; i++)
		of_node_put(r->banks[i].np);
}

static void __iomem *t6041_resolve_ps_node(struct t6041_ps_resolver *r,
					   struct device_node *ps_np)
{
	struct t6041_pmgr_bank *bank = NULL;
	struct device_node *parent;
	unsigned int i;
	u32 off;

	if (of_property_read_u32(ps_np, "reg", &off) || (off & 3)) {
		dev_err(r->dev, "%pOF: invalid PMGR PS reg offset\n", ps_np);
		return IOMEM_ERR_PTR(-EINVAL);
	}

	parent = of_get_parent(ps_np);
	if (!parent)
		return IOMEM_ERR_PTR(-EINVAL);

	for (i = 0; i < r->nbanks; i++) {
		if (r->banks[i].np == parent) {
			bank = &r->banks[i];
			break;
		}
	}

	if (!bank) {
		resource_size_t size;
		void __iomem *base;

		if (r->nbanks >= T6041_MAX_PMGR_BANKS) {
			of_node_put(parent);
			return IOMEM_ERR_PTR(-ENOSPC);
		}
		base = t6041_map_idx(r->dev, parent, 0, 4, &size);
		if (IS_ERR(base)) {
			of_node_put(parent);
			return base;
		}
		bank = &r->banks[r->nbanks++];
		bank->np = of_node_get(parent);
		bank->base = base;
		bank->size = size;
	}
	of_node_put(parent);

	if (off > bank->size || bank->size - off < 4) {
		dev_err(r->dev, "%pOF: offset %#x exceeds parent size %pa\n",
			ps_np, off, &bank->size);
		return IOMEM_ERR_PTR(-EINVAL);
	}

	return bank->base + off;
}

static int t6041_parse_ps_fixed(struct t6041_ps_resolver *r, const char *prop,
				void __iomem **out_regs,
				struct device_node **out_nodes,
				unsigned int expected)
{
	struct device_node *np = r->dev->of_node;
	unsigned int i;
	int count;

	count = of_count_phandle_with_args(np, prop, NULL);
	if (count != expected)
		return dev_err_probe(r->dev, -EINVAL,
				     "%s: expected %u phandles, got %d\n",
				     prop, expected, count);

	for (i = 0; i < expected; i++) {
		struct device_node *ps_np;
		void __iomem *reg;

		ps_np = of_parse_phandle(np, prop, i);
		if (!ps_np)
			return dev_err_probe(r->dev, -EINVAL,
					     "%s[%u]: unresolved phandle\n",
					     prop, i);
		reg = t6041_resolve_ps_node(r, ps_np);
		if (IS_ERR(reg)) {
			of_node_put(ps_np);
			return dev_err_probe(r->dev, PTR_ERR(reg),
					     "%s[%u]: cannot map PS register\n",
					     prop, i);
		}
		out_regs[i] = reg;
		if (out_nodes)
			out_nodes[i] = ps_np;
		else
			of_node_put(ps_np);
	}
	return 0;
}

struct t6041_topo_ctx {
	struct device *dev;
	struct device_node *const *nodes;
	unsigned int n;
	unsigned int stride;
	unsigned long *adj;
	unsigned int *indeg;
};

static int t6041_topo_find(const struct t6041_topo_ctx *ctx,
			   const struct device_node *np)
{
	unsigned int i;

	for (i = 0; i < ctx->n; i++)
		if (ctx->nodes[i] == np)
			return i;
	return -1;
}

static int t6041_ps_parent(struct device_node *np, int index,
			   struct device_node **out)
{
	struct of_phandle_args args;
	int ret;

	ret = of_parse_phandle_with_args(np, "power-domains",
					 "#power-domain-cells", index, &args);
	if (ret) {
		*out = NULL;
		return ret;
	}
	*out = args.np;
	return 0;
}

static int t6041_topo_add_ancestors(struct t6041_topo_ctx *ctx, unsigned int src,
				    struct device_node *np, unsigned int depth)
{
	struct device_node *parent;
	int k, ret;

	if (depth >= T6041_PS_MAX_DEPTH) {
		dev_err(ctx->dev, "PMGR power-domain hierarchy too deep or cyclic at %pOF\n", np);
		return -EINVAL;
	}

	for (k = 0; ; k++) {
		int idx;

		ret = t6041_ps_parent(np, k, &parent);
		if (ret == -ENOENT)
			break;
		if (ret)
			return ret;

		idx = t6041_topo_find(ctx, parent);
		if (idx == (int)src) {
			dev_err(ctx->dev, "PMGR power-domain cycle involving %pOF\n", parent);
			of_node_put(parent);
			return -EINVAL;
		}
		if (idx >= 0) {
			if (!__test_and_set_bit(idx, ctx->adj + (size_t)src * ctx->stride))
				ctx->indeg[idx]++;
		} else {
			ret = t6041_topo_add_ancestors(ctx, src, parent, depth + 1);
			if (ret) {
				of_node_put(parent);
				return ret;
			}
		}
		of_node_put(parent);
	}
	return 0;
}

/*
 * Stable (input-order-preserving) Kahn's topological sort on DT power-domains
 * edges (child -> parent). For child-first (shutdown), pick the lowest-index
 * ready node forward; for parent-first (restore), run the same child-first
 * pass in reverse index order and reverse the output so edge-free nodes later
 * in the list (such as auto-managed DCS domains after AMCCs) stay after
 * earlier nodes whose ancestors are resolved later.
 */
static int t6041_topo_sort(struct device *dev, struct device_node *const *nodes,
			   unsigned int n, unsigned int *order, bool child_first)
{
	struct t6041_topo_ctx ctx = {
		.dev = dev,
		.nodes = nodes,
		.n = n,
		.stride = BITS_TO_LONGS(n),
	};
	unsigned long *emitted = NULL;
	unsigned int i, step;
	int ret = 0;

	if (!n)
		return 0;

	ctx.adj = kcalloc((size_t)n * ctx.stride, sizeof(*ctx.adj), GFP_KERNEL);
	ctx.indeg = kcalloc(n, sizeof(*ctx.indeg), GFP_KERNEL);
	emitted = bitmap_zalloc(n, GFP_KERNEL);
	if (!ctx.adj || !ctx.indeg || !emitted) {
		ret = -ENOMEM;
		goto out;
	}

	for (i = 0; i < n; i++) {
		ret = t6041_topo_add_ancestors(&ctx, i, nodes[i], 0);
		if (ret)
			goto out;
	}

	for (step = 0; step < n; step++) {
		unsigned int pick, next;
		bool found = false;

		if (child_first) {
			for (pick = 0; pick < n; pick++) {
				if (!test_bit(pick, emitted) && !ctx.indeg[pick]) {
					found = true;
					break;
				}
			}
		} else {
			for (pick = n; pick-- > 0; ) {
				if (!test_bit(pick, emitted) && !ctx.indeg[pick]) {
					found = true;
					break;
				}
			}
		}
		if (!found) {
			dev_err(dev, "PMGR power-domain cycle detected\n");
			ret = -EINVAL;
			goto out;
		}
		__set_bit(pick, emitted);
		order[child_first ? step : n - 1 - step] = pick;
		for_each_set_bit(next, ctx.adj + (size_t)pick * ctx.stride, n)
			ctx.indeg[next]--;
	}

out:
	bitmap_free(emitted);
	kfree(ctx.indeg);
	kfree(ctx.adj);
	return ret;
}

static bool t6041_node_in_list(struct device_node *const *list, unsigned int n,
			       const struct device_node *np)
{
	unsigned int i;

	for (i = 0; i < n; i++)
		if (list[i] == np)
			return true;
	return false;
}

struct t6041_restore_acc {
	struct device_node **nodes;
	void __iomem **regs;
	unsigned int count;
	struct device_node *const *exclude;
	unsigned int n_exclude;
};

static bool t6041_ps_is_mem_channel(const struct device_node *np)
{
	const char *label;

	if (of_property_read_string(np, "label", &label))
		return false;
	return str_has_prefix(label, "amcc") || str_has_prefix(label, "dcs_");
}

static int t6041_parse_ps_list(struct t6041_ps_resolver *r, const char *prop,
			       bool topo_child_first,
			       void __iomem ***out_regs,
			       unsigned int *out_count,
			       unsigned long **out_mem_mask,
			       struct t6041_restore_acc *acc)
{
	struct device *dev = r->dev;
	struct device_node *np = dev->of_node;
	struct device_node **nodes;
	void __iomem **regs;
	void __iomem **dst;
	unsigned long *mem_mask = NULL;
	unsigned int *order = NULL;
	unsigned int i, uniq = 0;
	int count, ret = 0;

	count = of_count_phandle_with_args(np, prop, NULL);
	if (count <= 0)
		return dev_err_probe(dev, -EINVAL, "%s: missing or empty phandle list\n", prop);

	nodes = kcalloc(count, sizeof(*nodes), GFP_KERNEL);
	regs = kcalloc(count, sizeof(*regs), GFP_KERNEL);
	if (!nodes || !regs) {
		ret = -ENOMEM;
		goto out_free;
	}

	for (i = 0; i < count; i++) {
		struct device_node *ps_np;
		void __iomem *reg;

		ps_np = of_parse_phandle(np, prop, i);
		if (!ps_np) {
			ret = dev_err_probe(dev, -EINVAL, "%s[%u]: unresolved phandle\n",
					    prop, i);
			goto out_put;
		}
		if (t6041_node_in_list(nodes, uniq, ps_np)) {
			of_node_put(ps_np);
			continue;
		}
		reg = t6041_resolve_ps_node(r, ps_np);
		if (IS_ERR(reg)) {
			of_node_put(ps_np);
			ret = dev_err_probe(dev, PTR_ERR(reg),
					    "%s[%u]: cannot map PS register\n", prop, i);
			goto out_put;
		}
		nodes[uniq] = ps_np;
		regs[uniq] = reg;
		uniq++;
	}

	dst = devm_kcalloc(dev, uniq, sizeof(*dst), GFP_KERNEL);
	if (!dst) {
		ret = -ENOMEM;
		goto out_put;
	}

	if (topo_child_first) {
		order = kcalloc(uniq, sizeof(*order), GFP_KERNEL);
		if (!order) {
			ret = -ENOMEM;
			goto out_put;
		}
		ret = t6041_topo_sort(dev, nodes, uniq, order, true);
		if (ret)
			goto out_put;
		for (i = 0; i < uniq; i++)
			dst[i] = regs[order[i]];
	} else {
		memcpy(dst, regs, uniq * sizeof(*dst));
	}

	if (out_mem_mask) {
		mem_mask = devm_bitmap_zalloc(dev, uniq, GFP_KERNEL);
		if (!mem_mask) {
			ret = -ENOMEM;
			goto out_put;
		}
		for (i = 0; i < uniq; i++) {
			struct device_node *entry = topo_child_first ? nodes[order[i]] : nodes[i];

			if (t6041_ps_is_mem_channel(entry))
				__set_bit(i, mem_mask);
		}
	}

	for (i = 0; i < uniq; i++) {
		if (t6041_node_in_list(acc->exclude, acc->n_exclude, nodes[i]))
			continue;
		if (t6041_node_in_list(acc->nodes, acc->count, nodes[i]))
			continue;
		acc->nodes[acc->count] = of_node_get(nodes[i]);
		acc->regs[acc->count] = regs[i];
		acc->count++;
	}

	*out_regs = dst;
	*out_count = uniq;
	if (out_mem_mask)
		*out_mem_mask = mem_mask;

out_put:
	kfree(order);
	for (i = 0; i < uniq; i++)
		of_node_put(nodes[i]);
out_free:
	kfree(regs);
	kfree(nodes);
	return ret;
}

static int t6041_map_ps_domains(struct t6041_sleep *s)
{
	static const char *const sleep_props[] = {
		"apple,sleep-off-domains",
		"apple,atc-off-domains",
		"apple,sleep-auto-domains",
		"apple,sleep-auto-clkgate-domains",
		"apple,sleep-auto-active-domains",
		"apple,sleep-keep-on-domains",
	};
	struct device_node *cpu_cpm_nodes[T6041_CPU_COUNT + T6041_CPM_COUNT] = {};
	struct t6041_ps_resolver r = { .dev = s->dev };
	struct t6041_restore_acc acc = {
		.exclude = cpu_cpm_nodes,
		.n_exclude = ARRAY_SIZE(cpu_cpm_nodes),
	};
	struct device *dev = s->dev;
	struct device_node *np = dev->of_node;
	unsigned int *order = NULL;
	unsigned int max_restore = 0;
	unsigned int i, cpu;
	int ret;

	ret = t6041_parse_ps_fixed(&r, "apple,cpu-domains", s->ps_cpu,
				   cpu_cpm_nodes, T6041_CPU_COUNT);
	if (ret)
		goto out;
	ret = t6041_parse_ps_fixed(&r, "apple,cpm-domains", s->ps_cpm,
				   cpu_cpm_nodes + T6041_CPU_COUNT, T6041_CPM_COUNT);
	if (ret)
		goto out;

	for_each_present_cpu(cpu) {
		if (t6041_cpu_id(cpu_logical_map(cpu)) >= T6041_CPU_COUNT) {
			ret = dev_err_probe(dev, -EINVAL, "CPU %u ID out of range\n", cpu);
			goto out;
		}
	}

	ret = t6041_parse_ps_fixed(&r, "apple,sep-domain", &s->ps_sep, NULL, 1);
	if (ret)
		goto out;
	ret = t6041_parse_ps_fixed(&r, "apple,spmi-a0-domain", &s->ps_spmi_a0, NULL, 1);
	if (ret)
		goto out;
	ret = t6041_parse_ps_fixed(&r, "apple,atc-common-domains", s->ps_atc_common,
				   NULL, AXI2AF_ATC_COUNT);
	if (ret)
		goto out;
	ret = t6041_parse_ps_fixed(&r, "apple,atc-fabric-domain", &s->ps_atc_fabric, NULL, 1);
	if (ret)
		goto out;
	ret = t6041_parse_ps_fixed(&r, "apple,atc-afi-domain", &s->ps_atc_afi, NULL, 1);
	if (ret)
		goto out;
	ret = t6041_parse_ps_fixed(&r, "apple,apcie-domains", s->ps_apcie,
				   NULL, T6041_APCIE_PS_COUNT);
	if (ret)
		goto out;
	ret = t6041_parse_ps_fixed(&r, "apple,commit-sio-domains", s->ps_commit_sio,
				   NULL, T6041_COMMIT_SIO_COUNT);
	if (ret)
		goto out;
	ret = t6041_parse_ps_fixed(&r, "apple,commit-periph-domains", s->ps_commit_periph,
				   NULL, T6041_COMMIT_PERIPH_COUNT);
	if (ret)
		goto out;
	ret = t6041_parse_ps_fixed(&r, "apple,axi2af-domains", s->ps_axi2af,
				   NULL, AXI2AF_COUNT);
	if (ret)
		goto out;
	ret = t6041_parse_ps_fixed(&r, "apple,full-part-detect-domains", s->ps_full_detect,
				   NULL, T6041_FULL_DETECT_COUNT);
	if (ret)
		goto out;

	for (i = 0; i < ARRAY_SIZE(sleep_props); i++) {
		int n = of_count_phandle_with_args(np, sleep_props[i], NULL);

		if (n <= 0) {
			ret = dev_err_probe(dev, -EINVAL,
					    "%s: missing or empty phandle list\n",
					    sleep_props[i]);
			goto out;
		}
		max_restore += n;
	}

	acc.nodes = kcalloc(max_restore, sizeof(*acc.nodes), GFP_KERNEL);
	acc.regs = kcalloc(max_restore, sizeof(*acc.regs), GFP_KERNEL);
	if (!acc.nodes || !acc.regs) {
		ret = -ENOMEM;
		goto out;
	}

	/*
	 * Accumulate restore domains fabric- and controller-first before leaf
	 * and peripheral lists so hardware parents without DT power-domains
	 * edges (e.g. disabled AMCCs above auto-managed DCS nodes, or fab*
	 * fabrics above ATC/media blocks) are still ordered parent-first in
	 * ps_restore.
	 */
	ret = t6041_parse_ps_list(&r, "apple,sleep-auto-domains", false,
				  &s->ps_auto, &s->n_auto, &s->ps_auto_mem, &acc);
	if (ret)
		goto out;
	ret = t6041_parse_ps_list(&r, "apple,sleep-auto-active-domains", false,
				  &s->ps_auto_active, &s->n_auto_active, NULL, &acc);
	if (ret)
		goto out;
	ret = t6041_parse_ps_list(&r, "apple,sleep-auto-clkgate-domains", false,
				  &s->ps_auto_clkgate, &s->n_auto_clkgate,
				  &s->ps_auto_clkgate_mem, &acc);
	if (ret)
		goto out;
	ret = t6041_parse_ps_list(&r, "apple,sleep-keep-on-domains", false,
				  &s->ps_keep_on, &s->n_keep_on, NULL, &acc);
	if (ret)
		goto out;
	ret = t6041_parse_ps_list(&r, "apple,atc-off-domains", false,
				  &s->ps_atc_off, &s->n_atc_off, NULL, &acc);
	if (ret)
		goto out;
	ret = t6041_parse_ps_list(&r, "apple,sleep-off-domains", true,
				  &s->ps_off, &s->n_off, NULL, &acc);
	if (ret)
		goto out;

	order = kcalloc(acc.count, sizeof(*order), GFP_KERNEL);
	s->ps_restore = devm_kcalloc(dev, acc.count, sizeof(*s->ps_restore), GFP_KERNEL);
	s->ps_pre = devm_kcalloc(dev, acc.count, sizeof(*s->ps_pre), GFP_KERNEL);
	if (!order || !s->ps_restore || !s->ps_pre) {
		ret = -ENOMEM;
		goto out;
	}

	ret = t6041_topo_sort(dev, acc.nodes, acc.count, order, false);
	if (ret)
		goto out;

	for (i = 0; i < acc.count; i++)
		s->ps_restore[i] = acc.regs[order[i]];
	s->n_restore = acc.count;

out:
	kfree(order);
	for (i = 0; i < acc.count; i++)
		of_node_put(acc.nodes[i]);
	kfree(acc.regs);
	kfree(acc.nodes);
	for (i = 0; i < ARRAY_SIZE(cpu_cpm_nodes); i++)
		of_node_put(cpu_cpm_nodes[i]);
	t6041_ps_resolver_cleanup(&r);
	return ret;
}

static struct apple_tunable *t6041_apcie_tunable(struct device *dev,
						 struct device_node *np,
						 const char *reg_name,
						 const char *prop)
{
	int idx = of_property_match_string(np, "reg-names", reg_name);
	struct apple_tunable *t;
	struct resource res;

	if (idx < 0 || of_address_to_resource(np, idx, &res))
		return NULL;
	t = devm_apple_tunable_parse(dev, np, prop, &res);
	return IS_ERR(t) ? NULL : t;
}

static void __iomem *t6041_apcie_block(struct device *dev, struct device_node *np,
				       const char *reg_name, resource_size_t min_size,
				       const char *prop, struct apple_tunable **tunable)
{
	void __iomem *base;

	if (prop) {
		*tunable = t6041_apcie_tunable(dev, np, reg_name, prop);
		if (!*tunable)
			return NULL;
	}
	base = t6041_map_name(dev, np, reg_name, min_size, NULL);
	return IS_ERR(base) ? NULL : base;
}

static void t6041_map_apcie(struct t6041_sleep *s)
{
	struct device *dev = s->dev;
	struct device_node *np;
	unsigned int i;

	np = of_parse_phandle(dev->of_node, "apple,apcie", 0);
	if (!np)
		return;

	s->ap_rc = t6041_apcie_block(dev, np, "rc", 0x5c, NULL, NULL);
	s->ap_axi = t6041_apcie_block(dev, np, "axi2af", 0x110,
				      "apple,tunable-axi2af", &s->tun_axi2af);
	s->ap_cio3pll = t6041_apcie_block(dev, np, "cio3pll", 0x4,
					  "apple,tunable-cio3pll", &s->tun_cio3pll);
	s->ap_clkgen = t6041_apcie_block(dev, np, "clkgen", 0x8,
					 "apple,tunable-clkgen", &s->tun_clkgen);
	s->ap_phy = t6041_apcie_block(dev, np, "phy-block", 0x800c,
				      "apple,tunable-phy", &s->tun_phy);
	s->ap_phy_ip = t6041_apcie_block(dev, np, "phy-ip", 0x94,
					 "apple,tunable-phy-ip-pll", &s->tun_phy_ip_pll);
	s->tun_phy_ip_lanes = t6041_apcie_tunable(dev, np, "phy-ip",
						  "apple,tunable-phy-ip-auspma");
	for (i = 0; i < T6041_GP_PORTS; i++) {
		char name[8];

		snprintf(name, sizeof(name), "port%u", i);
		s->gp_port[i] = t6041_apcie_block(dev, np, name, 0x830, NULL, NULL);
		snprintf(name, sizeof(name), "phy%u", i);
		s->gp_phy[i] = t6041_apcie_block(dev, np, name, 0x4, NULL, NULL);
	}
	of_node_put(np);

	if (s->ap_axi)
		s->axi2af[AXI2AF_APCIE] = s->ap_axi + 0x100;

	if (s->ap_rc && s->ap_axi && s->ap_cio3pll && s->ap_clkgen &&
	    s->ap_phy && s->ap_phy_ip && s->tun_phy_ip_lanes &&
	    s->gp_port[0] && s->gp_port[1] && s->gp_phy[0] && s->gp_phy[1])
		s->apcie_ready = true;
}

static int t6041_map_pinctrl(struct t6041_sleep *s)
{
	struct device *dev = s->dev;
	struct device_node *np = dev->of_node;
	struct device_node *pin_np;
	void __iomem *base;
	u32 npins, pin;
	int i, n;

	pin_np = of_parse_phandle(np, "apple,pinctrl", 0);
	if (!pin_np)
		return dev_err_probe(dev, -EINVAL, "missing apple,pinctrl\n");
	if (of_property_read_u32(pin_np, "apple,npins", &npins) || !npins || npins > 512) {
		of_node_put(pin_np);
		return dev_err_probe(dev, -EINVAL, "invalid apple,npins on pinctrl\n");
	}
	base = t6041_map_idx(dev, pin_np, 0, (resource_size_t)npins * 4, NULL);
	of_node_put(pin_np);
	if (IS_ERR(base))
		return dev_err_probe(dev, PTR_ERR(base), "cannot map apple,pinctrl\n");

	s->gpio_pre = devm_kcalloc(dev, npins, sizeof(*s->gpio_pre), GFP_KERNEL);
	s->gpio_skip = devm_bitmap_zalloc(dev, npins, GFP_KERNEL);
	if (!s->gpio_pre || !s->gpio_skip)
		return -ENOMEM;

	n = of_property_count_u32_elems(np, "apple,pinctrl-skip-pins");
	for (i = 0; i < n; i++)
		if (!of_property_read_u32_index(np, "apple,pinctrl-skip-pins", i, &pin) &&
		    pin < npins)
			set_bit(pin, s->gpio_skip);

	s->gpio = base;
	s->npins = npins;
	return 0;
}

static int t6041_map_spmi(struct t6041_sleep *s)
{
	struct device *dev = s->dev;
	struct device_node *np = dev->of_node;
	struct device_node *bus_np;
	void __iomem *base;

	bus_np = of_parse_phandle(np, "apple,spmi-buses", 0);
	if (!bus_np)
		return dev_err_probe(dev, -EINVAL, "missing apple,spmi-buses[0]\n");
	base = t6041_map_idx(dev, bus_np, 0, 0x84, NULL);
	of_node_put(bus_np);
	if (IS_ERR(base))
		return dev_err_probe(dev, PTR_ERR(base), "cannot map apple,spmi-buses[0]\n");
	s->spmi = base;

	bus_np = of_parse_phandle(np, "apple,spmi-buses", 1);
	if (!bus_np)
		return dev_err_probe(dev, -EINVAL, "missing apple,spmi-buses[1]\n");
	base = t6041_map_idx(dev, bus_np, 0, 0x84, NULL);
	of_node_put(bus_np);
	if (IS_ERR(base))
		return dev_err_probe(dev, PTR_ERR(base), "cannot map apple,spmi-buses[1]\n");
	s->spmi_a0 = base;
	return 0;
}

static int t6041_map_axi2af(struct t6041_sleep *s)
{
	struct device *dev = s->dev;
	struct device_node *np = dev->of_node;
	unsigned int i;

	for (i = 0; i < AXI2AF_ATC_COUNT; i++) {
		struct device_node *phy_np;
		void __iomem *base;

		phy_np = of_parse_phandle(np, "apple,atc-phys", i);
		if (!phy_np)
			return dev_err_probe(dev, -EINVAL, "missing apple,atc-phys[%u]\n", i);
		base = t6041_map_name(dev, phy_np, "axi2af", 0x110, NULL);
		of_node_put(phy_np);
		if (IS_ERR(base))
			return dev_err_probe(dev, PTR_ERR(base),
					     "cannot map axi2af on apple,atc-phys[%u]\n", i);
		s->axi2af[i] = base + 0x100;
	}

	for (i = 0; i < AXI2AF_REG_COUNT; i++) {
		void __iomem *base = t6041_map_name(dev, np, t6041_axi2af_names[i], 0xc, NULL);

		if (IS_ERR(base))
			return dev_err_probe(dev, PTR_ERR(base), "cannot map %s\n",
					     t6041_axi2af_names[i]);
		s->axi2af[AXI2AF_ATC_COUNT + i] = base;
	}
	return 0;
}

static int t6041_map_mcc(struct t6041_sleep *s)
{
	struct device *dev = s->dev;
	struct device_node *np = dev->of_node;
	unsigned int g, p;

	for (g = 0; g < s->ngroups; g++) {
		char name[16];
		struct resource res;
		void __iomem *base;
		int idx;

		snprintf(name, sizeof(name), "amcc%u", g);
		base = t6041_map_name(dev, np, name, AMCC_MIN_SIZE, NULL);
		if (IS_ERR(base))
			return dev_err_probe(dev, PTR_ERR(base), "cannot map %s\n", name);
		s->amccb[g] = base;

		snprintf(name, sizeof(name), "dcs%u", g);
		idx = of_property_match_string(np, "reg-names", name);
		if (idx < 0 || of_address_to_resource(np, idx, &res))
			return dev_err_probe(dev, -EINVAL, "missing %s\n", name);
		for (p = 0; p < 4; p++) {
			resource_size_t off = (resource_size_t)p * DCS_CH_STRIDE;

			if (off + DCS_CH_SIZE > resource_size(&res))
				return dev_err_probe(dev, -EINVAL, "%s too small\n", name);
			s->dcs[4 * g + p] = devm_ioremap(dev, res.start + off, DCS_CH_SIZE);
			if (!s->dcs[4 * g + p])
				return -ENOMEM;
		}
	}
	return 0;
}

static int t6041_parse_wake_sources(struct t6041_sleep *s)
{
	struct device *dev = s->dev;
	u32 ids[WAKE_SOURCES_MAX];
	int i, n;

	n = of_property_read_variable_u32_array(dev->of_node, "apple,wake-enable",
						ids, 1, WAKE_SOURCES_MAX);
	if (n < 0)
		return dev_err_probe(dev, n, "missing apple,wake-enable\n");

	for (i = 0; i < WAKE_WORDS - 1; i++)
		s->wake_masks[i] = U32_MAX;
	s->wake_masks[WAKE_WORDS - 1] = WAKE_LAST_MASK;

	for (i = 0; i < n; i++) {
		if (ids[i] >= WAKE_MAX_ID)
			return dev_err_probe(dev, -EINVAL,
					     "wake source %u out of range\n", ids[i]);
		s->wake_masks[ids[i] / 32] &= ~BIT(ids[i] % 32);
	}
	return 0;
}

static const struct {
	const char *name;
	size_t field;
	resource_size_t min_size;
} t6041_core_regs[] = {
	{ "ps-aop",    offsetof(struct t6041_sleep, aop_ps),  8 * AOP_PS_WORDS },
	{ "cluster",   offsetof(struct t6041_sleep, cluster), 0x14 },
	{ "pwrgate",   offsetof(struct t6041_sleep, pwrgate), 0x114 },
	{ "cpu-start", offsetof(struct t6041_sleep, misc),    0x14 },
	{ "wdt",       offsetof(struct t6041_sleep, wdt),     0x30 },
	{ "aon-misc",  offsetof(struct t6041_sleep, aon),     0xc },
	{ "commit",    offsetof(struct t6041_sleep, commit),  0x4 },
	{ "clk-mux",   offsetof(struct t6041_sleep, clkgen),  0x68 },
	{ "ka-pmgr",   offsetof(struct t6041_sleep, ka[0]),   KA_WORDS * KA_STRIDE },
	{ "ka-east",   offsetof(struct t6041_sleep, ka[1]),   KA_WORDS * KA_STRIDE },
	{ "pmc",       offsetof(struct t6041_sleep, pmc),     0xac02c },
	{ "ptd",       offsetof(struct t6041_sleep, ptd),     PTD_MIN_SIZE },
	{ "perf",      offsetof(struct t6041_sleep, perf),    0x404 },
	{ "wake",      offsetof(struct t6041_sleep, wake),    0xc + 4 * WAKE_WORDS },
};

static int t6041_sleep_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	struct t6041_sleep *s;
	unsigned int i;
	u64 pa;
	int ret;

	if (!is_kernel_in_hyp_mode())
		return -ENODEV;

	s = devm_kzalloc(dev, sizeof(*s), GFP_KERNEL);
	if (!s)
		return -ENOMEM;
	s->dev = dev;

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

	for (i = 0; i < ARRAY_SIZE(t6041_core_regs); i++) {
		void __iomem *base = t6041_map_name(dev, np, t6041_core_regs[i].name,
						    t6041_core_regs[i].min_size, NULL);

		if (IS_ERR(base))
			return dev_err_probe(dev, PTR_ERR(base), "cannot map %s\n",
					     t6041_core_regs[i].name);
		*(void __iomem **)((char *)s + t6041_core_regs[i].field) = base;
	}

	ret = t6041_map_ps_domains(s);
	if (ret)
		return ret;
	ret = t6041_parse_wake_sources(s);
	if (ret)
		return ret;
	ret = t6041_map_spmi(s);
	if (ret)
		return ret;
	ret = t6041_map_axi2af(s);
	if (ret)
		return ret;
	ret = t6041_map_pinctrl(s);
	if (ret)
		return ret;

	/*
	 * The full part: the two full-part detect words (amcc24, amcc28) are
	 * not both target and actual 0 as on the binned part, where those
	 * channels are fused off.
	 */
	s->ngroups = (readl(s->ps_full_detect[0]) & (PS_TARGET | PS_ACTUAL)) &&
		     (readl(s->ps_full_detect[1]) & (PS_TARGET | PS_ACTUAL)) ? 8 : 6;
	dev_info(dev, "%u memory channels\n", 4 * s->ngroups);

	ret = t6041_map_mcc(s);
	if (ret)
		return ret;

	t6041_map_apcie(s);
	if (!s->apcie_ready) {
		dev_info(dev, "no PCIe tunables for ANS: no deep sleep\n");
		return -ENODEV;
	}

	t6041_sleep = s;
	ret = spin_table_set_power_ops(&t6041_cpu_power_ops);
	if (ret) {
		t6041_sleep = NULL;
		return dev_err_probe(dev, ret, "cannot control CPU power\n");
	}
	apple_sleep_set_commit_stops_coprocs(true);
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
