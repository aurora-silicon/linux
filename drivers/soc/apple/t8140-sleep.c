// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * Apple T8140 system sleep
 *
 * On T8140 a kernel can run directly at EL2 behind the CTXR execute window
 * (Documentation/arch/arm64/ctxr-execmem.rst), with no monitor below it. So no
 * PSCI answers a request to power a CPU off or to suspend the system, and the
 * kernel does both itself.
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
 * with the order kept.
 */

#include <linux/bitfield.h>
#include <linux/bits.h>
#include <linux/cacheflush.h>
#include <linux/cpu.h>
#include <linux/cpumask.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/irqchip/irq-apple-aic.h>
#include <linux/moduleparam.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/smp.h>
#include <linux/soc/apple/dart.h>
#include <linux/soc/apple/sleep.h>
#include <linux/suspend.h>
#include <clocksource/arm_arch_timer.h>
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
#define T8140_S2R_MAGIC		0x3152325341525541ULL	/* "AURAS2R1" */

struct t8140_s2r_mailbox {
	u64 magic;		/* aura, before the kernel starts */
	u64 boot_mpidr;		/* aura: the only core that may take resume_pa */
	u64 resume_pa;		/* kernel, armed before a commit; aura clears it */
	u64 resumes;		/* aura: resume_pa taken this many times */
};

/* PS word */
#define PS_AUTO_ENABLE		BIT(28)
#define PS_DEV_DISABLE		BIT(10)
#define PS_FLAGS		GENMASK(9, 8)
#define PS_ACTUAL		GENMASK(7, 4)
#define PS_TARGET		GENMASK(3, 0)
#define PS_OFF_TIMEOUT_US	5000

#define PS_DISP_CPU		0x10000
#define PS_DISP_FE		0x318
#define PS_DISP_SYS		0x280
#define PS_DISPEXT0_CPU		0x328
#define PS_DISPEXT0_FE		0x320
#define PS_DISPEXT0_SYS		0x288
#define PS_SIO_CPU		0x2a8
#define PS_SEP			0xc00
#define PS_ATC0_USB_AON		0x3e0
#define PS_ATC0_COMMON		0x3e8
#define PS_ATC0_USB		0x3f0

/* CPU start block, as aura's smp_start_cpu() uses it */
#define CPU_START_STOP		0x0
#define CPU_START_SYS		0x4
#define CPU_START_CLUSTER(n)	(0x8 + (n) * 4)

/* Power-gate page: reads back this value already. Purpose unknown. */
#define PWRGATE_ON		0x80000000
#define PWRGATE_WAKE_0		0x000
#define PWRGATE_WAKE_1		0x040
#define PWRGATE_CPM_0		0x060
#define PWRGATE_CPM_1		0x068
#define PWRGATE_DISP_0		0xc8c
#define PWRGATE_DISP_1		0xcd0

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
/* Word 7 has 21 sources. Measured: macOS reads 0x001fef77 there. */
#define WAKE_WORD7_ALL		GENMASK(20, 0)
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
#define HPM_RESET_MS		1000

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

/* An always-on word that a live debug link holds up. macOS has it off. */
#define PS_AON_DEBUG_SWITCH	0xb0

/* The PMC's voter agents, each with a word holding its floor for every rail. */
#define PMC_VOTER_AGENTS	4
#define PMC_VOTER_STRIDE	0x4000

/* DCS: the low nibble asks for a state, and [11:8] is the state it has reached. */
#define DCS_STATE		GENMASK(11, 8)
#define DCS_BUSY		BIT(31)
#define DCS_SLEEP_STATE		4
#define DCS_STEP_STATE		6
#define DCS_TIMEOUT_US		5000

/* Word 0 of the wake block. macOS clears it at boot, and sets it only for a device's DDR use. */
#define WAKE_SLP_DDR		0x00

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

#define PS_ANS			0x278
#define PS_APCIE_SYS_GP		0x2a0

/* The coprocessors the last mile stops. Each driver idles its own first. */
#define SLEEP_COPROCS		4

static struct t8140_sleep_coproc {
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
} t8140_sleep_coprocs[SLEEP_COPROCS];
static DEFINE_MUTEX(t8140_sleep_coprocs_lock);

/* The enclave's sleep request, left to the last refusal. Under t8140_sleep_coprocs_lock. */
static struct {
	struct device *dev;
	int (*tell)(void *data);
	void *data;
} t8140_sleep_enclave;

/*
 * Written with bits 0 and 63 by every core right before its last WFI, measured
 * from macOS. This is not CYC_OVRD, which is undefined on this SoC.
 */
#define SYS_APL_DEEP_SLEEP	sys_reg(3, 5, 15, 6, 2)
#define DEEP_SLEEP_ENABLE	(BIT_ULL(0) | BIT_ULL(63))

#define SYS_APL_IPI_SR_EL1	sys_reg(3, 5, 15, 1, 1)
#define IPI_SR_PENDING		BIT(0)
#define SYS_APL_SIQ_CFG_EL1	sys_reg(3, 4, 15, 10, 4)

/* The names of ADT pmgr/ap-wake-sources, for the log only */
static const struct {
	u32 id;
	const char *name;
} t8140_wake_names[] = {
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

#define T8140_DISPLAY_COUNT	2

/* CPU clusters: 0 is E, 1 is P. ADT /arm-io/pmgr reg[5], [7], [9] and [14], [16], [18]. */
#define T8140_CLUSTERS		2

/* cpm-dvfm */
#define DVFM_CTL		0x178
#define DVFM_WORD_0		0x140
#define DVFM_WORD_1		0x160
#define DVFM_WORD_2		0x190
#define DVFM_WORD_3		0x198
#define DVFM_ENABLE		BIT_ULL(63)

/* In macOS's order, on the way down. */
static const u32 t8140_dvfm_words[] = {
	DVFM_WORD_0, DVFM_WORD_1, DVFM_WORD_2, DVFM_WORD_3,
};

struct t8140_sleep {
	struct device *dev;
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
	/* The port controller whose cold reset ends the debug link. */
	struct device_node *hpm_np;
	u64 siq_cfg;
	u32 hpm_sid;
	int usb_down_ret;
	/* Cut, or found already down, for this sleep. */
	bool usb_down_ran;
	void __iomem *clk_mux;
	void __iomem *mcc[MCC_CHANNELS];
	void __iomem *ptd;
	void __iomem *clk_perf;
	void __iomem *cluster_dcs;
	u32 ps_on_saved[10];
	u32 ps_off_saved[64];
	u32 ps_aon_saved[4];
	u32 ps_aon_auto_saved[2];
	u32 spmi_saved[2];
	bool matched;
	void __iomem *dvfm[T8140_CLUSTERS];
	struct t8140_s2r_mailbox *mbox;

	u32 wake_enable[WAKE_SOURCES_MAX];
	int wake_enable_count;
	u32 wake_saved[WAKE_WORDS];

	struct {
		struct device_node *np;
		void __iomem *asc;
		bool bound;
	} display[T8140_DISPLAY_COUNT];

	/* Filled in for the report after a return. */
	u32 wake_status[WAKE_WORDS];
	u64 dvfm_ctl[T8140_CLUSTERS];
	u64 dvfm_saved[T8140_CLUSTERS][ARRAY_SIZE(t8140_dvfm_words)];
	bool dvfm_parked;
	u64 resumes_before;
	u64 resumes_after;
	int cluster_e_ret;
	int cluster_ave_ret;
	int cluster_ave_commit_ret;
	unsigned int ps_rewoken;
	unsigned int ps_rewake_timeouts;
	u64 cntkctl;
	int cpus_stop_ret;
	/* The words t8140_sleep_lower_stray() lowered, one bit each */
	u8 stray_lowered;
	/* What must be bound for a sleep, and what must have stopped by the last step */
	struct t8140_sleep_gate {
		const char *name;
		/* Compared only, never dereferenced: nodes outlive the gate. */
		const struct device_node *np;
		void __iomem *asc;
		bool bound;
		bool must_stop;
		bool sep;
		u32 ps;
	} gate[16];
	unsigned int gate_count;
	bool returned;
};

static struct t8140_sleep *t8140_sleep;

static struct t8140_sleep_coproc *t8140_sleep_coproc_find(struct device *dev)
{
	int i;

	for (i = 0; i < SLEEP_COPROCS; i++)
		if (t8140_sleep_coprocs[i].dev == dev)
			return &t8140_sleep_coprocs[i];

	return NULL;
}

/* A registered coprocessor its driver has readied, whose run bit the last step clears. */
static bool t8140_sleep_coproc_ready_node(const struct device_node *np)
{
	int i;

	for (i = 0; np && i < SLEEP_COPROCS; i++)
		if (t8140_sleep_coprocs[i].dev && t8140_sleep_coprocs[i].ready &&
		    t8140_sleep_coprocs[i].dev->of_node == np)
			return true;
	return false;
}

static bool t8140_usb_asleep;

void apple_sleep_usb_set_asleep(bool asleep)
{
	WRITE_ONCE(t8140_usb_asleep, asleep);
}
EXPORT_SYMBOL_GPL(apple_sleep_usb_set_asleep);

static unsigned long t8140_usb_wake_ports;

void apple_sleep_usb_set_wake_ports(unsigned long ports)
{
	WRITE_ONCE(t8140_usb_wake_ports, ports);
}
EXPORT_SYMBOL_GPL(apple_sleep_usb_set_wake_ports);

bool apple_sleep_usb_port_can_wake(unsigned int port)
{
	return port < BITS_PER_LONG && (READ_ONCE(t8140_usb_wake_ports) & BIT(port));
}
EXPORT_SYMBOL_GPL(apple_sleep_usb_port_can_wake);

static bool t8140_debug_link_cut;

/* Only the debug port's controller took the cold reset. */
static bool t8140_debug_port_is(struct device *dev)
{
	return t8140_sleep && t8140_sleep->hpm_np && dev->of_node == t8140_sleep->hpm_np;
}

bool apple_sleep_debug_link_was_cut(struct device *dev)
{
	return t8140_debug_port_is(dev) && xchg(&t8140_debug_link_cut, false);
}
EXPORT_SYMBOL_GPL(apple_sleep_debug_link_was_cut);

bool apple_sleep_debug_link_cut_peek(struct device *dev)
{
	return t8140_debug_port_is(dev) && READ_ONCE(t8140_debug_link_cut);
}
EXPORT_SYMBOL_GPL(apple_sleep_debug_link_cut_peek);

/* Below EL2 a monitor owns the last mile, and this code doesn't run it. */
int apple_sleep_coproc_register(struct device *dev, void __iomem *asc, bool pcie_ans,
				int (*last_stop)(void *data), void *data)
{
	struct t8140_sleep_coproc *c;

	if (!is_kernel_in_hyp_mode())
		return -ENODEV;

	guard(mutex)(&t8140_sleep_coprocs_lock);
	if (t8140_sleep_coproc_find(dev))
		return -EEXIST;
	c = t8140_sleep_coproc_find(NULL);
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
	struct t8140_sleep_coproc *c;
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

	guard(mutex)(&t8140_sleep_coprocs_lock);
	c = t8140_sleep_coproc_find(dev);
	if (c) {
		c->first = true;
		c->ps = ps;
	}

	return 0;
}
EXPORT_SYMBOL_GPL(apple_sleep_coproc_register_first);

int apple_sleep_coproc_set_last_check(struct device *dev, int (*last_check)(void *data))
{
	struct t8140_sleep_coproc *c;

	guard(mutex)(&t8140_sleep_coprocs_lock);
	c = t8140_sleep_coproc_find(dev);
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

	guard(mutex)(&t8140_sleep_coprocs_lock);
	if (t8140_sleep_enclave.dev)
		return -EBUSY;
	t8140_sleep_enclave.tell = tell;
	t8140_sleep_enclave.data = data;
	t8140_sleep_enclave.dev = dev;

	return 0;
}
EXPORT_SYMBOL_GPL(apple_sleep_enclave_register);

void apple_sleep_enclave_unregister(struct device *dev)
{
	guard(mutex)(&t8140_sleep_coprocs_lock);
	if (t8140_sleep_enclave.dev == dev)
		memset(&t8140_sleep_enclave, 0, sizeof(t8140_sleep_enclave));
}
EXPORT_SYMBOL_GPL(apple_sleep_enclave_unregister);

void apple_sleep_coproc_unregister(struct device *dev)
{
	struct t8140_sleep_coproc *c;

	guard(mutex)(&t8140_sleep_coprocs_lock);
	c = t8140_sleep_coproc_find(dev);
	if (c)
		memset(c, 0, sizeof(*c));
}
EXPORT_SYMBOL_GPL(apple_sleep_coproc_unregister);

void apple_sleep_coproc_set_ready(struct device *dev, bool ready)
{
	struct t8140_sleep_coproc *c;

	guard(mutex)(&t8140_sleep_coprocs_lock);
	c = t8140_sleep_coproc_find(dev);
	if (!c)
		return;
	c->ready = ready;
	c->stopped = false;
}
EXPORT_SYMBOL_GPL(apple_sleep_coproc_set_ready);

bool apple_sleep_coproc_stopped(struct device *dev)
{
	struct t8140_sleep_coproc *c;

	guard(mutex)(&t8140_sleep_coprocs_lock);
	c = t8140_sleep_coproc_find(dev);
	return c && c->stopped;
}
EXPORT_SYMBOL_GPL(apple_sleep_coproc_stopped);

/*
 * The last thing that can fail on the way to the commit: no CPU but this one is up and
 * interrupts are off, so no lock can be held. A coprocessor stopped here comes back only
 * through a real sleep, so every refusal has to be behind this, and nothing after it may
 * go back. In the order measured on macOS, on ANS itself: whatever its driver does last,
 * status and control read, then 0 written twice.
 */
static void t8140_sleep_coproc_run_off(struct t8140_sleep_coproc *c)
{
	/* Its own last stop cleared its run bits. */
	if (!c->asc) {
		c->stopped = true;
		return;
	}
	c->status = readl(c->asc + ASC_CPU_STATUS);
	writel(readl(c->asc + ASC_CPU_CONTROL) & ~ASC_CPU_CONTROL_RUN, c->asc + ASC_CPU_CONTROL);
	writel(readl(c->asc + ASC_CPU_CONTROL) & ~ASC_CPU_CONTROL_RUN, c->asc + ASC_CPU_CONTROL);
	c->stopped = true;
}

/* A power word that a ready coprocessor's last stop lowers: on until the commit. */
static bool t8140_sleep_coproc_lowers(u32 ps)
{
	int i;

	for (i = 0; i < SLEEP_COPROCS; i++)
		if (t8140_sleep_coprocs[i].dev && t8140_sleep_coprocs[i].ready &&
		    t8140_sleep_coprocs[i].first && t8140_sleep_coprocs[i].ps == ps)
			return true;
	return false;
}

/* The last refusals, each coprocessor's own: nothing has stopped yet. */
static int t8140_sleep_coprocs_check(struct t8140_sleep *s)
{
	int i, ret;

	for (i = 0; i < SLEEP_COPROCS; i++) {
		struct t8140_sleep_coproc *c = &t8140_sleep_coprocs[i];

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
static int t8140_sleep_cpus_hold(struct t8140_sleep *s)
{
	int ret = spin_table_hold_parked();

	if (ret)
		dev_err(s->dev, "refused: a CPU didn't answer the hold: %d\n", ret);

	return ret;
}

/* Last of all the refusals: an enclave that gated comes back only through a real sleep. */
static int t8140_sleep_enclave_tell(struct t8140_sleep *s)
{
	int ret;

	if (!t8140_sleep_enclave.tell)
		return 0;
	ret = t8140_sleep_enclave.tell(t8140_sleep_enclave.data);
	if (ret)
		dev_err(s->dev, "refused: the enclave didn't take the sleep request: %d\n", ret);

	return ret;
}

/*
 * In two passes, after the last refusal: from the first stop only a real sleep restarts the
 * GPU, so a failure here goes on to the commit. Those registered first stop whole first,
 * before macOS's off words drop under them: the GPU withdraws its PMP request there, and the
 * PMP has to see that before its own stop. The rest after the match.
 */
static void t8140_sleep_coprocs_stop(struct t8140_sleep *s, bool first)
{
	int i;

	for (i = 0; i < SLEEP_COPROCS; i++) {
		struct t8140_sleep_coproc *c = &t8140_sleep_coprocs[i];

		if (!c->dev || !c->ready || c->first != first)
			continue;
		if (c->stopped)
			continue;
		if (c->last_stop)
			c->last_stop(c->data);
		t8140_sleep_coproc_run_off(c);
	}
	dsb(sy);
}

/* MPIDR is cluster 0 cores 0-3 at 0.0.n and cluster 1 cores 0-1 at 1.1.n. */
static unsigned int t8140_cpu_cluster(u64 mpidr)
{
	return MPIDR_AFFINITY_LEVEL(mpidr, 2);
}

static unsigned int t8140_cpu_core(u64 mpidr)
{
	return MPIDR_AFFINITY_LEVEL(mpidr, 0);
}

static u32 t8140_cpu_start_bit(u64 mpidr)
{
	return BIT(4 * t8140_cpu_cluster(mpidr) + t8140_cpu_core(mpidr));
}

static void t8140_deep_wfi_arm(void)
{
	/* A timer left running ends the WFI, and costs power until reset. */
	write_sysreg(ARCH_TIMER_CTRL_IT_MASK, cntp_ctl_el0);
	write_sysreg(ARCH_TIMER_CTRL_IT_MASK, cntv_ctl_el0);
	sysreg_clear_set_s(SYS_APL_DEEP_SLEEP, 0, DEEP_SLEEP_ENABLE);
}

/* A core's last instructions: nothing brings it back but a reset. */
static void __noreturn t8140_deep_wfi(void)
{
	t8140_deep_wfi_arm();

	while (true) {
		dsb(sy);
		isb();
		wfi();
		write_sysreg_s(IPI_SR_PENDING, SYS_APL_IPI_SR_EL1);
	}
}

static void t8140_cpu_stop(u64 mpidr)
{
	writel(t8140_cpu_start_bit(mpidr), t8140_sleep->cpu_start + CPU_START_STOP);
	dsb(sy);
}

static void t8140_cpu_off(u64 mpidr)
{
	t8140_deep_wfi();
}

static int t8140_cpu_on(u64 mpidr)
{
	struct t8140_sleep *s = t8140_sleep;
	unsigned int cluster = t8140_cpu_cluster(mpidr);

	writel(t8140_cpu_start_bit(mpidr), s->cpu_start + CPU_START_SYS);
	writel(BIT(t8140_cpu_core(mpidr)), s->cpu_start + CPU_START_CLUSTER(cluster));

	return 0;
}

static const struct spin_table_power_ops t8140_cpu_power_ops = {
	.cpu_stop = t8140_cpu_stop,
	.cpu_off = t8140_cpu_off,
	.cpu_on = t8140_cpu_on,
};

/*
 * As macOS lowers a domain: AUTO_ENABLE off first if it's set, then target 0.
 * Writing the flags clears them. Values measured, for instance ANS 0x1f0020ff,
 * 0x0f0020ff, 0x0f0023f0.
 */
static u32 t8140_ps_lower_start(struct t8140_sleep *s, u32 offset)
{
	void __iomem *reg = s->ps + offset;
	u32 val = readl(reg);

	if (val & PS_AUTO_ENABLE) {
		val &= ~(PS_AUTO_ENABLE | PS_FLAGS);
		writel(val, reg);
	}

	return val;
}

static void t8140_ps_lower_finish(struct t8140_sleep *s, u32 offset, u32 val)
{
	writel((val & ~PS_TARGET) | PS_FLAGS, s->ps + offset);
}

static void t8140_ps_lower(struct t8140_sleep *s, u32 offset)
{
	t8140_ps_lower_finish(s, offset, t8140_ps_lower_start(s, offset));
}

/*
 * As macOS raises a domain: the target, then AUTO_ENABLE once it reads on. Measured
 * byte for byte on ANS and PCIe, for instance 0x0f002300, 0x0f00200f, 0x1f00200f.
 */
static int t8140_ps_raise_start(struct t8140_sleep *s, u32 offset)
{
	void __iomem *reg = s->ps + offset;
	u32 now;

	writel((readl(reg) & ~(PS_AUTO_ENABLE | GENMASK(11, 0))) | PS_TARGET, reg);

	return readl_poll_timeout_atomic(reg, now, (now & PS_ACTUAL) == PS_ACTUAL, 1,
					 PS_OFF_TIMEOUT_US);
}

static void t8140_ps_raise_finish(struct t8140_sleep *s, u32 offset)
{
	void __iomem *reg = s->ps + offset;

	writel((readl(reg) & ~(PS_AUTO_ENABLE | GENMASK(11, 0))) | PS_TARGET | PS_AUTO_ENABLE,
	       reg);
}

static int t8140_ps_raise(struct t8140_sleep *s, u32 offset)
{
	int ret = t8140_ps_raise_start(s, offset);

	t8140_ps_raise_finish(s, offset);

	return ret;
}

static int t8140_ps_wait_off(struct t8140_sleep *s, u32 offset)
{
	u32 val;

	return readl_poll_timeout_atomic(s->ps + offset, val, !FIELD_GET(PS_ACTUAL, val),
					 10, PS_OFF_TIMEOUT_US);
}

static bool t8140_ps_on(struct t8140_sleep *s, u32 offset)
{
	return FIELD_GET(PS_ACTUAL, readl(s->ps + offset));
}

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

static int t8140_engine_reset_wait(struct t8140_sleep *s, u32 offset, u32 actual)
{
	u32 val;

	return readl_poll_timeout_atomic(s->ps + offset, val,
					 FIELD_GET(PS_ACTUAL, val) == actual && !(val & PS_BUSY),
					 10, PS_OFF_TIMEOUT_US);
}

/* A domain of the PMGR block mapped here as "ps", and its offset in it. */
static bool t8140_ps_domain(struct t8140_sleep *s, struct device_node *pd, u32 *offset)
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
static struct device *t8140_iommus_dart(struct device_node *np, int index)
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
	struct t8140_sleep *s = t8140_sleep;
	struct device *darts[ENGINE_RESET_DARTS] = {};
	u32 ps[ENGINE_RESET_PS], saved[ENGINE_RESET_PS], val;
	struct of_phandle_iterator it;
	void __iomem *asc;
	int i, n = 0, idx, err, ret = 0;

	if (!s)
		return -ENODEV;
	of_for_each_phandle(&it, err, dev->of_node, "power-domains", NULL, 0) {
		if (!t8140_ps_domain(s, it.node, &val))
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
		darts[i] = t8140_iommus_dart(dev->of_node, i);
		if (!darts[i])
			break;
		apple_dart_state_save(darts[i]);
	}

	for (i = 0; i < n && !ret; i++) {
		saved[i] = readl(s->ps + ps[i]);
		writel(PS_ENGINE_RESET_4, s->ps + ps[i]);
		ret = t8140_engine_reset_wait(s, ps[i], 4);
		if (!ret) {
			writel(PS_ENGINE_RESET_0, s->ps + ps[i]);
			ret = t8140_engine_reset_wait(s, ps[i], 0);
		}
	}
	if (!ret) {
		usleep_range(1000, 2000);
		for (i = 0; i < n; i++)
			writel(0, s->ps + ps[i]);
		for (i = n - 1; i >= 0 && !ret; i--) {
			writel(PS_TARGET, s->ps + ps[i]);
			ret = t8140_engine_reset_wait(s, ps[i], FIELD_MAX(PS_ACTUAL));
			if (!ret)
				writel(saved[i] & ~(PS_FLAGS | PS_RESET | PS_ACTUAL),
				       s->ps + ps[i]);
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
static void t8140_dvfm_park(struct t8140_sleep *s)
{
	int cluster, word;

	for (cluster = 0; cluster < T8140_CLUSTERS; cluster++) {
		s->dvfm_ctl[cluster] = readq(s->dvfm[cluster] + DVFM_CTL);
		for (word = 0; word < ARRAY_SIZE(t8140_dvfm_words); word++)
			s->dvfm_saved[cluster][word] =
				readq(s->dvfm[cluster] + t8140_dvfm_words[word]);
	}

	writeq(0, s->dvfm[0] + DVFM_CTL);
	writeq(0, s->dvfm[1] + DVFM_CTL);
	for (word = 0; word < 2; word++)
		for (cluster = 0; cluster < T8140_CLUSTERS; cluster++)
			writeq(s->dvfm_saved[cluster][word] & ~DVFM_ENABLE,
			       s->dvfm[cluster] + t8140_dvfm_words[word]);
	for (cluster = 0; cluster < T8140_CLUSTERS; cluster++)
		for (word = 2; word < 4; word++)
			writeq(s->dvfm_saved[cluster][word] & ~DVFM_ENABLE,
			       s->dvfm[cluster] + t8140_dvfm_words[word]);
	s->dvfm_parked = true;
}

static void t8140_dvfm_unpark(struct t8140_sleep *s)
{
	int cluster, word;

	if (!s->dvfm_parked)
		return;
	s->dvfm_parked = false;

	writeq(s->dvfm_ctl[0], s->dvfm[0] + DVFM_CTL);
	writeq(s->dvfm_ctl[1], s->dvfm[1] + DVFM_CTL);
	for (cluster = 0; cluster < T8140_CLUSTERS; cluster++)
		for (word = 2; word < 4; word++)
			writeq(s->dvfm_saved[cluster][word],
			       s->dvfm[cluster] + t8140_dvfm_words[word]);
	for (word = 1; word >= 0; word--)
		for (cluster = 0; cluster < T8140_CLUSTERS; cluster++)
			writeq(s->dvfm_saved[cluster][word],
			       s->dvfm[cluster] + t8140_dvfm_words[word]);
}

/*
 * What XNU polls just before its complex words, and panics on if one is on, besides the
 * display's, ANS's and ATC0's, which the commit lowers itself. Every device has suspended by
 * the last step, so one still on has no user: it's lowered there, and a sleep with one that
 * won't go off is refused. They come back on after the sleep or a refusal, for genpd's books.
 */
static const struct {
	const char *name;
	u32 offset;
} t8140_stray_words[] = {
	{ "AVD_SYS", 0x270 }, { "VENC0_SYS", 0x240 }, { "VENC1_SYS", 0x248 },
	{ "GFX", 0x1f0 }, { "JPG", 0x230 },
};

static void t8140_sleep_stray_restore(struct t8140_sleep *s)
{
	int i;

	for (i = ARRAY_SIZE(t8140_stray_words) - 1; i >= 0; i--)
		if (s->stray_lowered & BIT(i))
			t8140_ps_raise(s, t8140_stray_words[i].offset);
	s->stray_lowered = 0;
}

static int t8140_sleep_lower_stray(struct t8140_sleep *s)
{
	int i, ret = 0;

	s->stray_lowered = 0;
	for (i = 0; i < ARRAY_SIZE(t8140_stray_words); i++) {
		u32 off = t8140_stray_words[i].offset;

		if (!t8140_ps_on(s, off))
			continue;
		/* Its driver's own last stop lowers it, at the commit. */
		if (t8140_sleep_coproc_lowers(off)) {
			dev_dbg(s->dev, "%s: left on for its driver's last stop\n",
				t8140_stray_words[i].name);
			continue;
		}
		t8140_ps_lower(s, off);
		s->stray_lowered |= BIT(i);
		if (t8140_ps_wait_off(s, off)) {
			dev_err(s->dev, "refused: %s won't go off: %#x\n",
				t8140_stray_words[i].name, readl(s->ps + off));
			ret = -EBUSY;
		} else {
			dev_info(s->dev, "%s was on at the last step, lowered\n",
				 t8140_stray_words[i].name);
		}
	}
	if (ret)
		t8140_sleep_stray_restore(s);

	return ret;
}

/*
 * T8140's coprocessors that must be bound for a sleep, the ones that must also have stopped
 * by the last step (a commit with the PMP and SIO still running reset the SoC 5.5 s later),
 * and the domains macOS has off by then. Taken in process context, since the checks run with
 * interrupts off. A driver whose stop failed has already failed the suspend, naming itself,
 * so what's left is whether it's bound at all, and what the hardware shows. @ps is the CPU's
 * own domain where a driver may power it off, as the DCPs' at DPMS off.
 */
static const struct {
	const char *path;
	bool must_stop;
	u32 ps;
} t8140_gate_coprocs[] = {
	{ "/soc/dcp@412e00000", true, PS_DISP_CPU },
	{ "/soc/dcp@416e00000", true, PS_DISPEXT0_CPU },
	{ "/soc/pmp@300500000", true },	{ "/soc/dma-controller@385e00000", true },
	{ "/soc/nvme@3cdcc0000" },	{ "/soc/mtp@314600000" },
	{ "/soc/aop@310c00000" },	{ "/soc/smc@30c600000" },
	{ "/soc/sep@282600000" },	{ "/soc/gpu@482600000" },
};

static const struct {
	const char *name;
	u32 offset;
} t8140_gate_off[] = {
	{ "ane_sys", 0x290 }, { "isp_sys", 0x268 }, { "prores", 0x250 },
	{ "avd_sys", 0x270 }, { "jpg", 0x230 }, { "venc0_sys", 0x240 },
};

static void t8140_sleep_gate_prepare(struct t8140_sleep *s)
{
	int i;

	s->gate_count = 0;
	for (i = 0; i < ARRAY_SIZE(t8140_gate_coprocs) && s->gate_count < ARRAY_SIZE(s->gate);
	     i++) {
		struct device_node *c = of_find_node_by_path(t8140_gate_coprocs[i].path);
		struct platform_device *pdev = c ? of_find_device_by_node(c) : NULL;
		struct t8140_sleep_gate *g;

		if (!c) {
			dev_warn(s->dev, "%s isn't in this tree\n", t8140_gate_coprocs[i].path);
			continue;
		}
		g = &s->gate[s->gate_count++];
		g->name = c->name;
		g->np = c;
		g->bound = pdev && device_is_bound(&pdev->dev);
		g->sep = of_node_name_eq(c, "sep");
		g->must_stop = t8140_gate_coprocs[i].must_stop;
		g->ps = t8140_gate_coprocs[i].ps;
		if (g->must_stop && !g->asc) {
			int idx = of_property_match_string(c, "reg-names", "coproc");

			if (idx < 0)
				idx = of_property_match_string(c, "reg-names", "asc");
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
static bool t8140_sleep_gate_failed(struct t8140_sleep *s)
{
	int i, bad = 0, running = 0;

	for (i = 0; i < s->gate_count; i++) {
		struct t8140_sleep_gate *g = &s->gate[i];

		if (!g->bound) {
			dev_err(s->dev, "%s isn't bound\n", g->name);
			bad++;
			/* Nobody told it of the sleep; a commit with it running never returns. */
			if (g->sep && t8140_ps_on(s, PS_SEP)) {
				dev_err(s->dev, "sep runs with no driver bound: %#x\n",
					readl(s->ps + PS_SEP));
				running++;
			}
			continue;
		}
		/* A CPU whose domain is off isn't running, and its registers aren't there. */
		if (g->ps && !t8140_ps_on(s, g->ps))
			continue;
		if (g->must_stop && (!g->asc ||
				     ((readl(g->asc + ASC_CPU_CONTROL) & ASC_CPU_CONTROL_RUN) &&
				      !t8140_sleep_coproc_ready_node(g->np)))) {
			dev_err(s->dev, "%s didn't stop: control %#x status %#x\n", g->name,
				g->asc ? readl(g->asc + ASC_CPU_CONTROL) : 0,
				g->asc ? readl(g->asc + ASC_CPU_STATUS) : 0);
			bad++;
			running++;
		}
		/* Told later, as the last refusal: see t8140_sleep_enclave_tell(). */
		if (g->sep && t8140_ps_on(s, PS_SEP) && !t8140_sleep_enclave.tell) {
			dev_err(s->dev, "sep hasn't gated itself: %#x\n", readl(s->ps + PS_SEP));
			bad++;
			running++;
		}
	}

	for (i = 0; i < ARRAY_SIZE(t8140_gate_off); i++) {
		if (t8140_ps_on(s, t8140_gate_off[i].offset)) {
			dev_err(s->dev, "%s is on: %#x\n", t8140_gate_off[i].name,
				readl(s->ps + t8140_gate_off[i].offset));
			bad++;
		}
	}

	if (running) {
		dev_err(s->dev, "refused: %d coprocessors that must stop or gate are still running\n",
			running);
		return true;
	}
	if (bad)
		dev_warn(s->dev, "going ahead with %d problems\n", bad);
	else
		dev_info(s->dev, "%u coprocessors up and stopped\n", s->gate_count);

	return false;
}

static void t8140_wake_unmask(u32 *val, u32 id)
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
static void t8140_wake_masks(struct t8140_sleep *s, u32 *val)
{
	int i;

	for (i = 0; i < WAKE_WORDS; i++)
		val[i] = i == WAKE_WORDS - 1 ? WAKE_WORD7_ALL : U32_MAX;
	for (i = 0; i < s->wake_enable_count; i++)
		t8140_wake_unmask(val, s->wake_enable[i]);
}

/* The last checks, then the stray words lowered: what has stopped may not return until a wake. */
static int t8140_sleep_refuse(struct t8140_sleep *s)
{
	bool refused = false, idled = false;
	int i;

	for (i = 0; i < SLEEP_COPROCS; i++) {
		struct t8140_sleep_coproc *c = &t8140_sleep_coprocs[i];

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

	/* ANS lowers its PCIe block with it, which this can't bring back yet. */
	if (idled || t8140_ps_on(s, PS_ANS) || t8140_ps_on(s, PS_APCIE_SYS_GP)) {
		dev_err(s->dev, "refused: ANS or its PCIe block is on\n");
		refused = true;
	}

	/* The engine waits for DISP_CPU with no timeout, so this must never slip. */
	for (i = 0; i < T8140_DISPLAY_COUNT; i++) {
		u32 ps = i ? PS_DISPEXT0_CPU : PS_DISP_CPU;

		if (s->display[i].bound && t8140_ps_on(s, ps) &&
		    (readl(s->display[i].asc + ASC_CPU_CONTROL) & ASC_CPU_CONTROL_RUN)) {
			dev_err(s->dev, "refused: a bound display coprocessor is still running\n");
			refused = true;
		}
	}

	if (t8140_sleep_gate_failed(s))
		refused = true;

	if (refused)
		return -EBUSY;

	return t8140_sleep_lower_stray(s);
}

static void t8140_wake_arm(struct t8140_sleep *s)
{
	u32 val[WAKE_WORDS];
	int i;

	for (i = 0; i < WAKE_WORDS; i++)
		s->wake_saved[i] = readl(s->wake + WAKE_MASK(i));
	t8140_wake_masks(s, val);
	for (i = 0; i < WAKE_WORDS; i++)
		writel(val[i], s->wake + WAKE_MASK(i));
}

static void t8140_wake_restore(struct t8140_sleep *s)
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
static int t8140_display_off(struct t8140_sleep *s)
{
	static const u32 domains[T8140_DISPLAY_COUNT][3] = {
		{ PS_DISP_CPU, PS_DISP_FE, PS_DISP_SYS },
		{ PS_DISPEXT0_CPU, PS_DISPEXT0_FE, PS_DISPEXT0_SYS },
	};
	int i, ret = 0;

	for (i = T8140_DISPLAY_COUNT - 1; i >= 0; i--) {
		void __iomem *asc = s->display[i].asc;

		/* The engine doesn't wait for DISPEXT0. */
		if (i == 1 && !s->display[i].bound)
			continue;

		if (t8140_ps_on(s, domains[i][0]) && asc &&
		    (readl(asc + ASC_CPU_CONTROL) & ASC_CPU_CONTROL_RUN)) {
			writel(0, asc + ASC_CPU_CONTROL);
			writel(0, asc + ASC_CPU_CONTROL);
		}

		/* Reads as a count of the DCPs powered, and macOS ends at 0. */
		writel(i, s->pmc_dcp);
		if (i == 0) {
			if (readl(s->ps + PS_DISP_CPU) & PS_AUTO_ENABLE)
				writel(readl(s->ps + PS_DISP_CPU) &
				       ~(PS_AUTO_ENABLE | PS_FLAGS), s->ps + PS_DISP_CPU);
			writel(3, s->pwrgate + PWRGATE_DISP_0);
			writel(0, s->pwrgate + PWRGATE_DISP_1);
		}
		t8140_ps_lower(s, domains[i][0]);
		t8140_ps_lower(s, domains[i][1]);
		t8140_ps_lower(s, domains[i][2]);
	}

	for (i = 0; i < T8140_DISPLAY_COUNT; i++) {
		int err;

		if (i == 1 && !s->display[i].bound)
			continue;

		err = t8140_ps_wait_off(s, domains[i][0]);

		if (err) {
			dev_err(s->dev, "refused: display domain %#x stayed on: %#x\n",
				domains[i][0], readl(s->ps + domains[i][0]));
			ret = err;
		}
	}

	return ret;
}

/* One command on the port controller's bus, polled: no other CPU, and interrupts are off. */
static int t8140_hpm_cmd(struct t8140_sleep *s, u32 op, u32 extra, const u32 *in, u32 *out)
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

static int t8140_hpm_select(struct t8140_sleep *s, u32 page)
{
	u32 val = ~0U;
	int i, ret;

	ret = t8140_hpm_cmd(s, SPMI_OP_ZERO_WRITE, page << 8, NULL, NULL);
	for (i = 0; !ret && i < 2000; i++) {
		ret = t8140_hpm_cmd(s, SPMI_OP_EXT_READ(1), HPM_REG_SELECT, NULL, &val);
		if (!ret && (val & 0xff) == page)
			return 0;
		udelay(10);
	}

	return ret ?: -ETIMEDOUT;
}

/*
 * What macOS sends the other port controller about a second before a sleep, measured
 * in three runs: system power state 3, then the bus's sleep command.
 */
static int t8140_hpm_sleep(struct t8140_sleep *s)
{
	u32 data = HPM_SSPS_S3, cmd = HPM_CMD_SSPS, val = ~0U;
	int i, ret;

	ret = t8140_hpm_cmd(s, SPMI_OP_WAKEUP, 0, NULL, NULL);
	if (!ret)
		ret = t8140_hpm_select(s, HPM_PAGE_DATA1);
	if (!ret)
		ret = t8140_hpm_cmd(s, SPMI_OP_EXT_WRITE(1), HPM_REG_DATA_CMD, &data, NULL);
	if (!ret)
		ret = t8140_hpm_select(s, HPM_PAGE_CMD1);
	if (!ret)
		ret = t8140_hpm_cmd(s, SPMI_OP_EXT_WRITE(4), HPM_REG_DATA_CMD, &cmd, NULL);
	/* Selected afresh for each read, as macOS does. */
	for (i = 0; !ret && val && i < 100; i++) {
		udelay(1000);
		ret = t8140_hpm_select(s, HPM_PAGE_CMD1);
		if (!ret)
			ret = t8140_hpm_cmd(s, SPMI_OP_EXT_READ(4), HPM_REG_DATA, NULL, &val);
	}
	if (!ret && val)
		ret = -ETIMEDOUT;
	/* Asleep either way: a failure here only means it wasn't told first. */
	t8140_hpm_cmd(s, SPMI_OP_SLEEP, 0, NULL, NULL);

	return ret;
}

/*
 * The debug link stays up with the ATC0 domains off, and it holds the debug domains
 * active, which macOS has idle. The only known way to end it is the port controller's
 * cold reset. To the SMC that's a cable pulled: it queues the port's events and records
 * them as wake reasons. So it has to come before the SMC's AP idle notice, not after, or
 * SMC.OutboxNotEmpty is up at the commit. tipd rearms the link after a refusal.
 */
static bool t8140_sleep_usb_down(struct t8140_sleep *s)
{
	u32 cmd = HPM_CMD_COLD_RESET;
	u32 enable;
	int ret;

	if (s->usb_down_ran)
		return false;
	s->usb_down_ran = true;

	if (!s->hpm_spmi || !FIELD_GET(PS_ACTUAL, readl(s->ps_aon + PS_AON_DEBUG_SWITCH)))
		return false;

	dev_info(s->dev, "debug link: cutting it for the sleep\n");
	enable = readl(s->hpm_spmi + SPMI_NOTIFY_ENABLE);
	writel(enable & ~SPMI_NOTIFY_REPLY, s->hpm_spmi + SPMI_NOTIFY_ENABLE);
	ret = t8140_hpm_cmd(s, SPMI_OP_WAKEUP, 0, NULL, NULL);
	if (!ret)
		ret = t8140_hpm_select(s, HPM_PAGE_CMD1);
	if (!ret)
		ret = t8140_hpm_cmd(s, SPMI_OP_EXT_WRITE(4), HPM_REG_DATA, &cmd, NULL);
	if (!ret) {
		WRITE_ONCE(t8140_debug_link_cut, true);
		mdelay(HPM_RESET_MS);
		ret = t8140_hpm_sleep(s);
	}
	writel(1, s->hpm_spmi + SPMI_NOTIFY_ACK);
	writel(enable, s->hpm_spmi + SPMI_NOTIFY_ENABLE);
	s->usb_down_ret = ret;

	return true;
}

bool apple_sleep_debug_link_cut(void)
{
	return t8140_sleep ? t8140_sleep_usb_down(t8140_sleep) : false;
}
EXPORT_SYMBOL_GPL(apple_sleep_debug_link_cut);

/* About 4 ms after the other cores stop on macOS: deepest first. */
static void t8140_sleep_lower_io(struct t8140_sleep *s)
{
	/*
	 * USB's complex, deepest first: macOS has it off here, and its drivers don't lower it.
	 * A host that sleeps in place keeps ATC0_USB_AON, as macOS's does.
	 */
	t8140_ps_lower(s, PS_ATC0_USB);
	t8140_ps_lower(s, PS_ATC0_COMMON);
	if (!READ_ONCE(t8140_usb_asleep))
		t8140_ps_lower(s, PS_ATC0_USB_AON);
}

static void t8140_sleep_set_wd2(struct t8140_sleep *s, unsigned int secs)
{
	writel(0, s->wdt + WDT_WD2_CTL);
	writel(0, s->wdt + WDT_WD2_COUNT);
	writel(secs * WDT_TICKS_PER_S, s->wdt + WDT_WD2_ALARM);
	writel(1, s->wdt + WDT_WD2_CTL);
}

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

/* Each word at the commit, and as the wake found it. */
static u32 t8140_ps_commit[ARRAY_SIZE(t8140_ps_all)];
static u32 t8140_ps_aon_commit[ARRAY_SIZE(t8140_ps_aon_all)];
static u32 t8140_ps_woke[ARRAY_SIZE(t8140_ps_all)];
static u32 t8140_ps_aon_woke[ARRAY_SIZE(t8140_ps_aon_all)];

#define PS_PARENT_MISSING	BIT(11)
#define PS_AUTO_STATE		GENMASK(27, 24)
#define PS_REWAKE_PASSES	2

static void t8140_ps_save(struct t8140_sleep *s)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(t8140_ps_all); i++)
		t8140_ps_commit[i] = readl(s->ps + t8140_ps_all[i]);
	for (i = 0; i < ARRAY_SIZE(t8140_ps_aon_all); i++)
		t8140_ps_aon_commit[i] = readl(s->ps_aon + t8140_ps_aon_all[i]);
}

static int t8140_ps_find(const u32 *all, int n, u32 offset)
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
static int t8140_ps_restore(void __iomem *reg, u32 saved)
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

/* Linux had it on and the wake left it otherwise. With AUTO_ENABLE, off is allowed. */
static bool t8140_ps_dropped(u32 saved, u32 woke)
{
	u32 control = PS_AUTO_ENABLE | PS_DEV_DISABLE | PS_TARGET;

	if (!FIELD_GET(PS_TARGET, saved))
		return false;
	if ((saved ^ woke) & control)
		return true;
	return !(saved & PS_AUTO_ENABLE) &&
	       FIELD_GET(PS_ACTUAL, woke) != FIELD_GET(PS_TARGET, saved);
}

/* Words that didn't read on in time, for the report. */
static bool t8140_ps_late[ARRAY_SIZE(t8140_ps_all)];
static bool t8140_ps_aon_late[ARRAY_SIZE(t8140_ps_aon_all)];

/*
 * Every domain back as it was at the commit, before any driver's resume reads its block:
 * macOS's wake words first, in its order, whatever they read, if Linux had them on. Then
 * any other domain the wake left otherwise, a second time for one whose parent came later.
 */
static void t8140_ps_rewake(struct t8140_sleep *s)
{
	bool *redo = t8140_ps_late, *redo_aon = t8140_ps_aon_late;
	bool done[ARRAY_SIZE(t8140_ps_all)] = {}, done_aon[ARRAY_SIZE(t8140_ps_aon_all)] = {};
	int i, j, pass;

	memset(t8140_ps_late, 0, sizeof(t8140_ps_late));
	memset(t8140_ps_aon_late, 0, sizeof(t8140_ps_aon_late));
	s->ps_rewoken = 0;
	s->ps_rewake_timeouts = 0;
	for (i = 0; i < ARRAY_SIZE(t8140_ps_all); i++)
		t8140_ps_woke[i] = readl(s->ps + t8140_ps_all[i]);
	for (i = 0; i < ARRAY_SIZE(t8140_ps_aon_all); i++)
		t8140_ps_aon_woke[i] = readl(s->ps_aon + t8140_ps_aon_all[i]);

	for (j = 0; j < ARRAY_SIZE(t8140_ps_wake_order); j++) {
		i = t8140_ps_find(t8140_ps_all, ARRAY_SIZE(t8140_ps_all), t8140_ps_wake_order[j]);
		if (i < 0 || !FIELD_GET(PS_TARGET, t8140_ps_commit[i]))
			continue;
		done[i] = true;
		s->ps_rewoken++;
		if (t8140_ps_restore(s->ps + t8140_ps_all[i], t8140_ps_commit[i]))
			redo[i] = true;
	}
	for (j = 0; j < ARRAY_SIZE(t8140_ps_aon_wake_order); j++) {
		i = t8140_ps_find(t8140_ps_aon_all, ARRAY_SIZE(t8140_ps_aon_all),
				  t8140_ps_aon_wake_order[j]);
		if (i < 0 || !FIELD_GET(PS_TARGET, t8140_ps_aon_commit[i]))
			continue;
		done_aon[i] = true;
		s->ps_rewoken++;
		if (t8140_ps_restore(s->ps_aon + t8140_ps_aon_all[i], t8140_ps_aon_commit[i]))
			redo_aon[i] = true;
	}

	for (pass = 0; pass < PS_REWAKE_PASSES; pass++) {
		for (i = 0; i < ARRAY_SIZE(t8140_ps_all); i++) {
			if (!redo[i] &&
			    (done[i] || !t8140_ps_dropped(t8140_ps_commit[i], t8140_ps_woke[i])))
				continue;
			if (!done[i]) {
				done[i] = true;
				s->ps_rewoken++;
			}
			redo[i] = t8140_ps_restore(s->ps + t8140_ps_all[i], t8140_ps_commit[i]);
		}
		for (i = 0; i < ARRAY_SIZE(t8140_ps_aon_all); i++) {
			if (!redo_aon[i] &&
			    (done_aon[i] ||
			     !t8140_ps_dropped(t8140_ps_aon_commit[i], t8140_ps_aon_woke[i])))
				continue;
			if (!done_aon[i]) {
				done_aon[i] = true;
				s->ps_rewoken++;
			}
			redo_aon[i] = t8140_ps_restore(s->ps_aon + t8140_ps_aon_all[i],
						       t8140_ps_aon_commit[i]);
		}
	}

	for (i = 0; i < ARRAY_SIZE(t8140_ps_all); i++)
		s->ps_rewake_timeouts += redo[i];
	for (i = 0; i < ARRAY_SIZE(t8140_ps_aon_all); i++)
		s->ps_rewake_timeouts += redo_aon[i];
}

static void t8140_ps_rewake_report(struct t8140_sleep *s)
{
	int i;

	dev_dbg(s->dev, "power words: %u written back, %u not on in time\n", s->ps_rewoken,
		s->ps_rewake_timeouts);
	for (i = 0; i < ARRAY_SIZE(t8140_ps_all); i++)
		if (t8140_ps_late[i])
			dev_err(s->dev, "power word %#x not on in time: %#x at the commit, %#x now\n",
				t8140_ps_all[i], t8140_ps_commit[i],
				readl(s->ps + t8140_ps_all[i]));
	for (i = 0; i < ARRAY_SIZE(t8140_ps_aon_all); i++)
		if (t8140_ps_aon_late[i])
			dev_err(s->dev, "power word aon %#x not on in time: %#x at the commit, %#x now\n",
				t8140_ps_aon_all[i], t8140_ps_aon_commit[i],
				readl(s->ps_aon + t8140_ps_aon_all[i]));
}

/* What it has off, highest first, as it lowers them. Domains with their own stop aren't here. */
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

/* As macOS writes the PMU: the reply's vector masked, then acknowledged, so none is left raised. */
static void t8140_pmu_write(struct t8140_sleep *s, u16 reg, u8 val)
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

static void t8140_sleep_match(struct t8140_sleep *s)
{
	int i, raised = 0, lowered = 0, failed = 0;

	writel(0, s->wdt + WDT_WD0_COUNT);
	writel(0, s->wdt + WDT_WD1_COUNT);
	t8140_pmu_write(s, PMU_BOOT_STAGE, PMU_BOOT_STAGE_SLEEP);

	s->spmi_saved[0] = readl(s->spmi + SPMI_NOTIFY_ENABLE);
	s->spmi_saved[1] = readl(s->spmi_a0);
	writel(SPMI_NOTIFY_ENABLE_SLEEP, s->spmi + SPMI_NOTIFY_ENABLE);
	writel(SPMI_NOTIFY_ENABLE_SLEEP, s->spmi_a0);

	for (i = 0; i < ARRAY_SIZE(t8140_ps_macos_on); i++) {
		u32 offset = t8140_ps_macos_on[i];

		s->ps_on_saved[i] = readl(s->ps + offset);
		/* State 4 reads as on, and macOS has these at f. */
		if (FIELD_GET(PS_ACTUAL, s->ps_on_saved[i]) == 0xf)
			continue;
		/* Measured at the commit with AUTO_ENABLE clear, unlike after a wake. */
		if (t8140_ps_raise_start(s, offset))
			failed++;
		raised++;
	}
	for (i = 0; i < ARRAY_SIZE(t8140_ps_macos_off); i++) {
		u32 offset = t8140_ps_macos_off[i];

		s->ps_off_saved[i] = readl(s->ps + offset);
		if (!t8140_ps_on(s, offset))
			continue;
		t8140_ps_lower(s, offset);
		lowered++;
	}
	for (i = 0; i < ARRAY_SIZE(t8140_ps_aon_macos_off); i++) {
		void __iomem *reg = s->ps_aon + t8140_ps_aon_macos_off[i];
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
	for (i = 0; i < ARRAY_SIZE(t8140_ps_aon_macos_no_auto); i++) {
		void __iomem *reg = s->ps_aon + t8140_ps_aon_macos_no_auto[i];
		u32 val = readl(reg);

		s->ps_aon_auto_saved[i] = val;
		writel(val & ~(PS_AUTO_ENABLE | PS_FLAGS), reg);
	}
	s->matched = true;

	for (i = 0; i < ARRAY_SIZE(t8140_ps_macos_off); i++)
		if (t8140_ps_wait_off(s, t8140_ps_macos_off[i]))
			dev_dbg(s->dev, "still on, macOS has it off: %x = %x\n",
				t8140_ps_macos_off[i], readl(s->ps + t8140_ps_macos_off[i]));
	dev_dbg(s->dev, "macOS's power words: raised %d, %d of them failed, lowered %d\n",
		raised, failed, lowered);
}

/* Back as they were, after a refusal past the match or a real wake. */
static void t8140_sleep_match_undo(struct t8140_sleep *s)
{
	int i;

	if (!s->matched)
		return;
	s->matched = false;

	for (i = ARRAY_SIZE(t8140_ps_aon_macos_no_auto) - 1; i >= 0; i--)
		writel(s->ps_aon_auto_saved[i] & ~PS_FLAGS,
		       s->ps_aon + t8140_ps_aon_macos_no_auto[i]);
	for (i = ARRAY_SIZE(t8140_ps_aon_macos_off) - 1; i >= 0; i--)
		if (FIELD_GET(PS_ACTUAL, s->ps_aon_saved[i]))
			writel(s->ps_aon_saved[i] & ~PS_FLAGS,
			       s->ps_aon + t8140_ps_aon_macos_off[i]);
	for (i = ARRAY_SIZE(t8140_ps_macos_off) - 1; i >= 0; i--)
		if (FIELD_GET(PS_ACTUAL, s->ps_off_saved[i]))
			t8140_ps_raise(s, t8140_ps_macos_off[i]);
	for (i = ARRAY_SIZE(t8140_ps_macos_on) - 1; i >= 0; i--) {
		u32 val = s->ps_on_saved[i];

		if (!FIELD_GET(PS_ACTUAL, val))
			t8140_ps_lower(s, t8140_ps_macos_on[i]);
		else if (FIELD_GET(PS_ACTUAL, val) != 0xf)
			writel(val & ~PS_FLAGS, s->ps + t8140_ps_macos_on[i]);
	}
	writel(s->spmi_saved[0], s->spmi + SPMI_NOTIFY_ENABLE);
	writel(s->spmi_saved[1], s->spmi_a0);
}

/* The safety net, as macOS arms it. */
static void t8140_sleep_arm_wd2(struct t8140_sleep *s)
{
	writel(0, s->wdt + WDT_WD0_CTL);
	t8140_sleep_set_wd2(s, WDT_SLEEP_SECS);
	writel(0, s->aon_misc + AON_AP_DEADLINE);
}

static int t8140_perf_wait(void __iomem *reg, unsigned int state)
{
	u32 val;

	return readl_poll_timeout_atomic(reg, val,
					 (val & (DCS_BUSY | DCS_STATE)) ==
					 FIELD_PREP(DCS_STATE, state),
					 1, DCS_TIMEOUT_US);
}

static int t8140_dcs_wait(struct t8140_sleep *s, unsigned int state)
{
	return t8140_perf_wait(s->dcs, state);
}

/*
 * As macOS does at about -18 ms, on each channel in turn: the enables down a bit at a time,
 * then each of four lanes strobed and cleared, two lanes on every channel before the
 * next two. Each write is built from a fresh read, as the trace has it.
 */
static void t8140_sleep_mcc_stop(struct t8140_sleep *s)
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
static void t8140_sleep_dcs_down(struct t8140_sleep *s)
{
	unsigned int state;
	int agent;

	for (agent = 0; agent < PMC_VOTER_AGENTS; agent++)
		if (readl(s->pmc_voters + agent * PMC_VOTER_STRIDE))
			writel(0, s->pmc_voters + agent * PMC_VOTER_STRIDE);
	if (!t8140_dcs_wait(s, DCS_SLEEP_STATE))
		return;

	state = FIELD_GET(DCS_STATE, readl(s->dcs));
	if (state > DCS_STEP_STATE) {
		writel(DCS_STEP_STATE, s->dcs);
		t8140_dcs_wait(s, DCS_STEP_STATE);
	}
	if (state > DCS_SLEEP_STATE) {
		writel(DCS_SLEEP_STATE, s->dcs);
		t8140_dcs_wait(s, DCS_SLEEP_STATE);
	}
}

/* The boot core's last WFI. An event that ends it is taken, and the WFI taken again. */
static void __noreturn t8140_boot_wfi(struct t8140_sleep *s)
{
	t8140_deep_wfi_arm();

	while (true) {
		dsb(sy);
		isb();
		wfi();
		aic_sleep_ack();
		write_sysreg_s(IPI_SR_PENDING, SYS_APL_IPI_SR_EL1);
	}
}

/* XNU polls these off just before its complex words. The sleep goes on whatever they read. */
static void t8140_sleep_wait_still_on(struct t8140_sleep *s)
{
	static const u32 words[] = {
		PS_ATC0_COMMON, PS_DISP_SYS, PS_DISPEXT0_SYS, PS_ANS, 0x270 /* AVD_SYS */,
		0x240 /* VENC0_SYS */, 0x248 /* VENC1_SYS */, 0x1f0 /* GFX */, 0x230 /* JPG */,
		PS_APCIE_SYS_GP,
	};
	int i;

	for (i = 0; i < ARRAY_SIZE(words); i++)
		t8140_ps_wait_off(s, words[i]);
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
static int t8140_sleep_commit(unsigned long unused)
{
	struct t8140_sleep *s = t8140_sleep;
	int plane, agent;
	u32 val;

	/*
	 * The other cores stop first, as on macOS. Each answered the hold in .enter, and they
	 * stay coherent until the commit.
	 */
	s->siq_cfg = read_sysreg_s(SYS_APL_SIQ_CFG_EL1);
	s->cpus_stop_ret = spin_table_stop_parked();

	t8140_sleep_arm_wd2(s);
	t8140_sleep_wait_still_on(s);

	/* The SoC comes down before memory does, as on macOS. */
	if (t8140_perf_wait(s->clk_perf + CLK_PERF_SOC, SOC_SLEEP_STATE)) {
		writel(SOC_SLEEP_STATE, s->clk_perf + CLK_PERF_SOC);
		t8140_perf_wait(s->clk_perf + CLK_PERF_SOC, SOC_SLEEP_STATE);
	}
	t8140_sleep_dcs_down(s);
	t8140_sleep_mcc_stop(s);

	/* Gates, PTD, the first voter twice, gates: macOS's order, measured once. */
	for (agent = 0; agent < 2; agent++) {
		writel(PWRGATE_ON, s->pwrgate + PWRGATE_CPM_0);
		writel(PWRGATE_ON, s->pwrgate + PWRGATE_CPM_1);
	}
	writeq(0, s->ptd + PTD_A);
	writeq(0, s->ptd + PTD_B);
	for (agent = 0; agent < 2; agent++) {
		readl(s->pmc_voters);
		writel(0, s->pmc_voters);
		readl(s->pmc_voters + 4);
	}
	writel(PWRGATE_ON, s->pwrgate + PWRGATE_CPM_0);
	writel(PWRGATE_ON, s->pwrgate + PWRGATE_CPM_1);

	/* Already off since .prepare, as on macOS. */
	writel(0, s->cluster + CLUSTER_AVE);
	s->cluster_ave_commit_ret = readl_poll_timeout_atomic(s->cluster + CLUSTER_AVE, val, !val,
							      1, 5000);
	writel(0, s->cluster + CLUSTER_E);
	writel(0, s->cluster + CLUSTER_P);

	writel(readl(s->ps + PS_SIO_CPU) & ~PS_AUTO_ENABLE, s->ps + PS_SIO_CPU);

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
	t8140_dvfm_park(s);

	t8140_ps_save(s);
	for (plane = 0; plane < ARRAY_SIZE(s->amcc); plane++) {
		void __iomem *amcc = s->amcc[plane];

		writel(readl(amcc + AMCC_WORD0_ENABLE) & ~AMCC_WORD0_DSID_AGE_OUT,
		       amcc + AMCC_WORD0_ENABLE);
		writel(readl(amcc + AMCC_WORD1_ENABLE) & ~AMCC_WORD1_UNEXP_RT_HIT,
		       amcc + AMCC_WORD1_ENABLE);
	}

	/* aura reads this with the MMU off. */
	s->resumes_before = READ_ONCE(s->mbox->resumes);
	WRITE_ONCE(s->mbox->resume_pa, __pa_symbol(cpu_resume));
	dcache_clean_poc((unsigned long)s->mbox, (unsigned long)(s->mbox + 1));
	dsb(sy);

	writel(1, s->commit);

	/* As on macOS: a raised line would end the last WFI, and the engine waits for it. */
	aic_sleep_mask_all();

	/* Each core's deep WFI comes after the commit, this one last. No SPMI from here on. */
	spin_table_release_stopped();
	t8140_boot_wfi(s);
}

/* Back on the boot core through the reset vector, MMU on, in macOS's order. */
static void t8140_sleep_return(struct t8140_sleep *s)
{
	u32 val;
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

	writel(CLUSTER_ON, s->cluster + CLUSTER_E);
	s->cluster_e_ret = readl_poll_timeout_atomic(s->cluster + CLUSTER_E, val,
						     val & CLUSTER_E_READY, 1,
						     CLUSTER_TIMEOUT_US);
	writel(CLUSTER_ON, s->cluster + CLUSTER_P);

	writel(PWRGATE_ON, s->pwrgate + PWRGATE_WAKE_0);
	writel(PWRGATE_ON, s->pwrgate + PWRGATE_WAKE_1);

	t8140_ps_rewake(s);

	val = readl(s->ps + PS_SEP);
	if (val & PS_DEV_DISABLE)
		writel(val & ~PS_DEV_DISABLE, s->ps + PS_SEP);

	/* The complex comes back on, and macOS turns it off. */
	writel(0, s->cluster + CLUSTER_AVE);
	s->cluster_ave_ret = readl_poll_timeout_atomic(s->cluster + CLUSTER_AVE, val, !val,
						       1, CLUSTER_TIMEOUT_US);

	writel(PWRGATE_ON, s->pwrgate + PWRGATE_CPM_0);
	writel(PWRGATE_ON, s->pwrgate + PWRGATE_CPM_1);

	t8140_dvfm_unpark(s);

	/* macOS stops it here. Keep a longer one over device resume, until .end. */
	t8140_sleep_set_wd2(s, WDT_SUSPEND_SECS);

	dcache_inval_poc((unsigned long)s->mbox, (unsigned long)(s->mbox + 1));
	s->resumes_after = READ_ONCE(s->mbox->resumes);

	for (i = 0; i < WAKE_WORDS; i++)
		s->wake_status[i] = readl(s->wake + WAKE_STATUS(i));
	s->returned = true;
}

static int t8140_sleep_enter(suspend_state_t state)
{
	struct t8140_sleep *s = t8140_sleep;
	int ret;

	ret = t8140_sleep_refuse(s);
	if (ret)
		return ret;
	ret = t8140_sleep_cpus_hold(s);
	/* As on macOS, the display goes off before the GPU. */
	if (!ret)
		ret = t8140_display_off(s);
	if (!ret)
		ret = t8140_sleep_coprocs_check(s);
	if (!ret)
		ret = t8140_sleep_enclave_tell(s);
	if (ret) {
		spin_table_unhold_parked();
		t8140_sleep_stray_restore(s);
		return ret;
	}
	t8140_sleep_coprocs_stop(s, true);

	t8140_wake_arm(s);
	t8140_sleep_match(s);

	if (readl(s->wake + WAKE_SLP_DDR))
		writel(0, s->wake + WAKE_SLP_DDR);
	/* Acknowledge a leftover done flag, as the DCS word has none on macOS. */
	if (readl(s->cluster_dcs) & CLUSTER_UP_DONE)
		writel(readl(s->cluster_dcs), s->cluster_dcs);
	t8140_sleep_coprocs_stop(s, false);
	/* Done by now unless no SMC driver is bound. */
	t8140_sleep_usb_down(s);

	s->cntkctl = read_sysreg(cntkctl_el1);
	pm_set_resume_via_firmware();
	ret = cpu_suspend(0, t8140_sleep_commit);
	if (!ret)
		t8140_sleep_return(s);

	t8140_sleep_match_undo(s);
	t8140_sleep_stray_restore(s);
	t8140_wake_restore(s);

	return ret;
}

/* From here to .end a hang resets the machine. Nobody has to be there to recover it. */
static int t8140_sleep_begin(suspend_state_t state)
{
	/* The last sleep's cut is its port controller's to take, by its .complete. */
	WRITE_ONCE(t8140_debug_link_cut, false);
	if (t8140_sleep)
		t8140_sleep->usb_down_ran = false;
	t8140_sleep_set_wd2(t8140_sleep, WDT_SUSPEND_SECS);
	pm_set_suspend_via_firmware();

	return 0;
}

static void t8140_sleep_end(void)
{
	writel(0, t8140_sleep->wdt + WDT_WD2_CTL);
}

/*
 * macOS turns this complex off at boot and again at the end of each wake, so at the commit
 * its word is already 0 and the engine has no sequence to run for it. Turning it off at the
 * commit instead puts that sequence, with its untimed wait, right before the CPU complexes'.
 */
static void t8140_sleep_ave_off(struct t8140_sleep *s)
{
	u32 val;
	int ret;

	/* Target 0, and the leftover done flag from iBoot's up, as XNU's first write does. */
	writel(CLUSTER_UP_DONE, s->cluster + CLUSTER_AVE);
	ret = readl_poll_timeout_atomic(s->cluster + CLUSTER_AVE, val, !val, 1, CLUSTER_TIMEOUT_US);
	if (ret)
		dev_warn(s->dev, "AVE complex didn't go off: %#x\n", val);
}

static int t8140_sleep_prepare(void)
{
	struct t8140_sleep *s = t8140_sleep;
	int i;

	t8140_sleep_ave_off(s);

	for (i = 0; i < T8140_DISPLAY_COUNT; i++) {
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
	t8140_sleep_gate_prepare(s);

	return 0;
}

static void t8140_sleep_report(void)
{
	struct t8140_sleep *s = t8140_sleep;
	int i;

	if (!s->returned)
		return;
	s->returned = false;

	dev_info(s->dev, "back from sleep, boot object resumes %llu -> %llu\n",
		 s->resumes_before, s->resumes_after);
	for (i = 0; i < ARRAY_SIZE(t8140_wake_names); i++) {
		u32 id = t8140_wake_names[i].id;

		if (s->wake_status[id / 32] & BIT(id % 32))
			dev_info(s->dev, "wake source: %s\n", t8140_wake_names[i].name);
	}
	t8140_ps_rewake_report(s);

	if (s->cpus_stop_ret)
		dev_err(s->dev, "other CPUs did not stop: %d\n", s->cpus_stop_ret);
	if (s->cluster_e_ret || s->cluster_ave_ret || s->cluster_ave_commit_ret)
		dev_err(s->dev, "cluster power timed out: E %d AVE %d, AVE at the commit %d\n",
			s->cluster_e_ret, s->cluster_ave_ret, s->cluster_ave_commit_ret);
	if (s->usb_down_ret)
		dev_err(s->dev, "debug link: the stop failed: %d\n", s->usb_down_ret);
}

static const struct platform_suspend_ops t8140_sleep_ops = {
	.valid = suspend_valid_only_mem,
	.begin = t8140_sleep_begin,
	.prepare = t8140_sleep_prepare,
	.enter = t8140_sleep_enter,
	.wake = t8140_sleep_report,
	.end = t8140_sleep_end,
};

static int t8140_sleep_read_wake_sources(struct t8140_sleep *s)
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

static int t8140_sleep_map_displays(struct t8140_sleep *s)
{
	int i;

	for (i = 0; i < T8140_DISPLAY_COUNT; i++) {
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

static void __iomem *t8140_sleep_map(struct device *dev, const char *name)
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
} t8140_sleep_regs[] = {
	{ "ps", offsetof(struct t8140_sleep, ps) },
	{ "pwrgate", offsetof(struct t8140_sleep, pwrgate) },
	{ "cpu-start", offsetof(struct t8140_sleep, cpu_start) },
	{ "cllt", offsetof(struct t8140_sleep, cllt) },
	{ "cluster", offsetof(struct t8140_sleep, cluster) },
	{ "commit", offsetof(struct t8140_sleep, commit) },
	{ "wdt", offsetof(struct t8140_sleep, wdt) },
	{ "aon-misc", offsetof(struct t8140_sleep, aon_misc) },
	{ "wake", offsetof(struct t8140_sleep, wake) },
	{ "pmc-dcp", offsetof(struct t8140_sleep, pmc_dcp) },
	{ "spmi", offsetof(struct t8140_sleep, spmi) },
	{ "pmc-voters", offsetof(struct t8140_sleep, pmc_voters) },
	{ "dcs", offsetof(struct t8140_sleep, dcs) },
	{ "ps-aon", offsetof(struct t8140_sleep, ps_aon) },
	{ "spmi-a0", offsetof(struct t8140_sleep, spmi_a0) },
	{ "clk-mux", offsetof(struct t8140_sleep, clk_mux) },
	{ "mcc0", offsetof(struct t8140_sleep, mcc[0]) },
	{ "mcc1", offsetof(struct t8140_sleep, mcc[1]) },
	{ "mcc2", offsetof(struct t8140_sleep, mcc[2]) },
	{ "ptd", offsetof(struct t8140_sleep, ptd) },
	{ "clk-perf", offsetof(struct t8140_sleep, clk_perf) },
	{ "cluster-dcs", offsetof(struct t8140_sleep, cluster_dcs) },
	{ "dvfm-e", offsetof(struct t8140_sleep, dvfm[0]) },
	{ "dvfm-p", offsetof(struct t8140_sleep, dvfm[1]) },
	{ "amcc0", offsetof(struct t8140_sleep, amcc[0]) },
	{ "amcc1", offsetof(struct t8140_sleep, amcc[1]) },
	{ "amcc2", offsetof(struct t8140_sleep, amcc[2]) },
	{ "amcc3", offsetof(struct t8140_sleep, amcc[3]) },
};

static int t8140_sleep_map_regs(struct t8140_sleep *s)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(t8140_sleep_regs); i++) {
		void __iomem **field = (void __iomem **)((u8 *)s + t8140_sleep_regs[i].offset);

		*field = t8140_sleep_map(s->dev, t8140_sleep_regs[i].name);
		if (IS_ERR(*field))
			return dev_err_probe(s->dev, PTR_ERR(*field), "no %s registers\n",
					     t8140_sleep_regs[i].name);
	}

	return 0;
}

static int t8140_sleep_map_mailbox(struct t8140_sleep *s)
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
	if (s->mbox->magic != T8140_S2R_MAGIC)
		return dev_err_probe(s->dev, -EINVAL, "bad mailbox magic %llx\n",
				     s->mbox->magic);
	/* Probe can run on any CPU. Logical CPU 0 is always the boot CPU. */
	if (s->mbox->boot_mpidr != cpu_logical_map(0))
		return dev_err_probe(s->dev, -EINVAL,
				     "mailbox is for CPU %llx, not this boot CPU\n",
				     s->mbox->boot_mpidr);

	return 0;
}

static void t8140_sleep_map_debug_port(struct t8140_sleep *s)
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

static int t8140_sleep_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct t8140_sleep *s;
	int ret;

	/* Below EL2 a monitor owns all of this. */
	if (!is_kernel_in_hyp_mode())
		return -ENODEV;

	s = devm_kzalloc(dev, sizeof(*s), GFP_KERNEL);
	if (!s)
		return -ENOMEM;
	s->dev = dev;

	/* Without the boot object's half, a wake would rerun its boot flow. */
	ret = t8140_sleep_map_mailbox(s);
	if (ret)
		return ret;

	ret = t8140_sleep_map_regs(s);
	if (ret)
		return ret;

	ret = t8140_sleep_read_wake_sources(s);
	if (ret)
		return ret;
	ret = t8140_sleep_map_displays(s);
	if (ret)
		return ret;
	t8140_sleep_map_debug_port(s);

	platform_set_drvdata(pdev, s);

	ret = spin_table_set_power_ops(&t8140_cpu_power_ops);
	if (ret)
		return dev_err_probe(dev, ret, "cannot control CPU power\n");

	/* Last: the exported helpers take it as a probed driver, and a failure frees s. */
	t8140_sleep = s;
	/* Deep becomes the default sleep state, unless mem_sleep_default= chose another. */
	suspend_set_ops(&t8140_sleep_ops);

	dev_info(dev, "deep sleep %s: boot object resumes %llu\n",
		 mem_sleep_current == PM_SUSPEND_MEM ? "is the default" :
		 "available, not the default", READ_ONCE(s->mbox->resumes));

	return 0;
}

static const struct of_device_id t8140_sleep_of_match[] = {
	{ .compatible = "apple,t8140-sleep" },
	{ }
};

static struct platform_driver t8140_sleep_driver = {
	.driver = {
		.name = "t8140-sleep",
		.of_match_table = t8140_sleep_of_match,
		.suppress_bind_attrs = true,
	},
	.probe = t8140_sleep_probe,
};
builtin_platform_driver(t8140_sleep_driver);
