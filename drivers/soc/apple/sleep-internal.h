/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/*
 * The coprocessor and enclave registry, and what the Apple SoC sleep driver
 * (sleep.c) shares with a SoC's own part of it (sleep-t6040.c for T6040).
 * Drivers register through <linux/soc/apple/sleep.h>; the controller bound on
 * this SoC walks the table at its commit.
 */
#ifndef _APPLE_SLEEP_INTERNAL_H_
#define _APPLE_SLEEP_INTERNAL_H_

#include <linux/bits.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/mutex.h>
#include <linux/soc/apple/tunable.h>
#include <linux/types.h>

#include <asm/sysreg.h>

struct device;
struct device_node;
struct platform_device;
struct seq_file;

/* The coprocessors the last mile stops. Each driver idles its own first. */
#define APPLE_SLEEP_COPROCS	6

struct apple_sleep_coproc {
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
	unsigned int order;	/* among those registered first, lowest runs first */
	u32 ps;
};

/* Irreversible enclave stop; nonzero is legal only if SLEEP was not sent. */
struct apple_sleep_enclave {
	struct device *dev;
	int (*stop)(void *data);
	void *data;
};

extern struct apple_sleep_coproc apple_sleep_coprocs[APPLE_SLEEP_COPROCS];
extern struct apple_sleep_enclave apple_sleep_enclave;
extern struct mutex apple_sleep_coprocs_lock;

struct apple_sleep_coproc *apple_sleep_coproc_find(struct device *dev);

/*
 * The sleep controller bound on this SoC. Each binds only to its own SoC's
 * compatible, so at most one is bound. Bound, it tells the registry when its
 * probe is complete: from then on a suspend through firmware that is not a dry
 * run is one whose commit stops the registered coprocessors.
 *
 * APPLE_SLEEP_REG_ENCLAVE_ANY: take every enclave registration. Without it one
 * is taken only while such a commit is the case.
 */
#define APPLE_SLEEP_REG_ENCLAVE_ANY	BIT(0)

int apple_sleep_registry_bind(unsigned int flags);
void apple_sleep_registry_unbind(void);
void apple_sleep_registry_ready(void);

/* Must match aura's struct aura_s2r_mailbox. */
#define AURA_S2R_MAGIC		0x3152325341525541ULL	/* "AURAS2R1" */

struct aura_s2r_mailbox {
	u64 magic;		/* aura, before the kernel starts */
	u64 boot_mpidr;		/* aura: the only core that may take resume_pa */
	u64 resume_pa;		/* kernel, armed before a commit; aura clears it */
	u64 resumes;		/* aura: resume_pa taken this many times */
};

/*
 * Registers that are the same on every SoC this driver knows.
 */

/* PS word */
#define PS_AUTO_ENABLE		BIT(28)
#define PS_MIN			GENMASK(19, 16)
#define PS_UNLATCH		BIT(12)
#define PS_BUSY			BIT(11)
#define PS_DEV_DISABLE		BIT(10)
#define PS_FLAGS		GENMASK(9, 8)
#define PS_ACTUAL		GENMASK(7, 4)
#define PS_TARGET		GENMASK(3, 0)
#define PS_ACTIVE		0xf
#define PS_OFF_TIMEOUT_US	5000
#define PS_BUSY_SETTLE_US	1000

/* CPU start block, as aura's smp_start_cpu() uses it. A core's bit is the SoC's. */
#define CPU_START_STOP		0x0
#define CPU_START_SYS		0x4
#define CPU_START_CLUSTER(n)	(0x8 + (n) * 4)

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

/* The AP queue of the PMU's SPMI controller, and its response vector's enable and ack */
#define SPMI_STATUS		0x00
#define SPMI_STATUS_RX_EMPTY	BIT(24)
#define SPMI_CMD		0x04
#define SPMI_REPLY		0x08
#define SPMI_NOTIFY_ENABLE	0x40
#define SPMI_NOTIFY_REPLY	BIT(0)
#define SPMI_NOTIFY_ACK		0x80
#define SPMI_CMD_ACTIVE		BIT(15)
#define SPMI_OP_EXT_WRITE(n)	((n) - 1)
#define SPMI_OP_EXT_READ(n)	(0x20 | ((n) - 1))
#define SPMI_OP_ZERO_WRITE	0x80
#define SPMI_OP_SLEEP		0x11
#define SPMI_OP_WAKEUP		0x13
#define SPMI_POLL_US		20000

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
#define APCIE_PORT_STATUS	0x804
#define APCIE_PORT_STATUS_READY	BIT(0)
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

/*
 * Written with bits 0 and 63 by every core right before its last WFI. This is not
 * CYC_OVRD, which is undefined on this SoC.
 */
#define SYS_APL_DEEP_SLEEP	sys_reg(3, 5, 15, 6, 2)
#define DEEP_SLEEP_ENABLE	(BIT_ULL(0) | BIT_ULL(63))

#define SYS_APL_IPI_SR_EL1	sys_reg(3, 5, 15, 1, 1)
#define IPI_SR_PENDING		BIT(0)
#define SYS_APL_SIQ_CFG_EL1	sys_reg(3, 4, 15, 10, 4)

/* A one-byte register write on the sleep's SPMI bus, to the PMU's USID */
#define SPMI_CMD_WRITE		(BIT(15) | 0x30)
#define SPMI_CMD_USID		GENMASK(11, 8)

/* A PMU write: the reply's vector masked, then acknowledged, so none is left raised. */
static inline void apple_sleep_pmu_write(void __iomem *spmi, u8 usid, u16 reg, u8 val)
{
	u32 status, enable;

	enable = readl(spmi + SPMI_NOTIFY_ENABLE);
	writel(enable & ~SPMI_NOTIFY_REPLY, spmi + SPMI_NOTIFY_ENABLE);
	writel(((u32)reg << 16) | SPMI_CMD_WRITE | FIELD_PREP(SPMI_CMD_USID, usid), spmi + SPMI_CMD);
	writel(val, spmi + SPMI_CMD);
	if (!readl_poll_timeout_atomic(spmi + SPMI_STATUS, status,
				       !(status & SPMI_STATUS_RX_EMPTY), 1, 1000))
		readl(spmi + SPMI_REPLY);
	writel(1, spmi + SPMI_NOTIFY_ACK);
	writel(enable, spmi + SPMI_NOTIFY_ENABLE);
}

/*
 * A domain is raised with the target, then AUTO_ENABLE once it reads on. Measured
 * byte for byte on ANS and PCIe, for instance 0x0f002300, 0x0f00200f, 0x1f00200f.
 */
static inline int apple_sleep_ps_raise_start(void __iomem *reg)
{
	u32 now;

	writel((readl(reg) & ~(PS_AUTO_ENABLE | GENMASK(11, 0))) | PS_TARGET, reg);

	return readl_poll_timeout_atomic(reg, now, (now & PS_ACTUAL) == PS_ACTUAL, 1,
					 PS_OFF_TIMEOUT_US);
}

static inline void apple_sleep_ps_raise_finish(void __iomem *reg)
{
	writel((readl(reg) & ~(PS_AUTO_ENABLE | GENMASK(11, 0))) | PS_TARGET | PS_AUTO_ENABLE,
	       reg);
}

/* Read-modify-write in table order, with no write where nothing changes. */
static inline void apple_sleep_tunable_apply_changed(void __iomem *regs,
						     const struct apple_tunable *t)
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

/* WD2 of the AON watchdog, the sleep's safety net, armed for @secs from now. */
static inline void apple_sleep_wd2_arm(void __iomem *wdt, unsigned int secs)
{
	writel(0, wdt + WDT_WD2_CTL);
	writel(0, wdt + WDT_WD2_COUNT);
	writel(secs * WDT_TICKS_PER_S, wdt + WDT_WD2_ALARM);
	writel(1, wdt + WDT_WD2_CTL);
}

/*
 * A SoC whose sleep doesn't fit the lists of struct apple_sleep_soc brings its own sequences.
 * @probe then runs in place of the driver's own, and installs the part's suspend operations.
 * @restore, if set, runs when the system comes back from a hibernation image.
 * @registry_flags: how the part binds to the registry, which the driver does around @probe.
 */
struct apple_sleep_part {
	int (*probe)(struct platform_device *pdev);
	int (*restore)(struct device *dev);
	unsigned int registry_flags;
};

/* sleep-t6040.c */
extern const struct apple_sleep_part t6040_sleep_part;

/*
 * For a part with dry runs, from its .begin to its .end: 1 runs the firmware-driven device
 * suspend short of what only a power cycle restores, 2 also the last steps that can still be
 * taken back. apple_sleep_power_loss() and apple_sleep_last_steps() answer by it.
 */
void apple_sleep_set_dry(unsigned int level);

/*
 * The storage PCIe block's windows and tunables are on a node a loader fills. A tunable table
 * @prop for the window @name, or NULL; and the window itself mapped, with its tunable if @prop
 * names one, or NULL where either is missing.
 */
struct apple_tunable *apple_sleep_apcie_tunable(struct device *dev, struct device_node *np,
						const char *name, const char *prop);
void __iomem *apple_sleep_apcie_block(struct device *dev, struct device_node *np,
				      const char *name, const char *prop,
				      struct apple_tunable **tunable);

/*
 * A part whose sleep takes neither the lid's state at the wake nor the USB wake ports says so
 * at its probe. apple_sleep_lid_closed_at_wake() and apple_sleep_usb_port_can_wake() then
 * answer as they do with no sleep driver, whatever the SMC's and the USB host's drivers told.
 */
void apple_sleep_set_part_plain_wake(void);

/* For a debugfs file: who is registered, and how far each is. */
void apple_sleep_registry_show(struct seq_file *s);

/*
 * The rest is for the commit point, in this order: interrupts off and one CPU up, so nothing
 * takes the registry's lock. @dev is the sleep driver's, for the log.
 *
 * Every registered coprocessor has to be ready: returns how many aren't, each logged, and sets
 * @pcie_ans_idled if ANS is among the ready ones.
 */
unsigned int apple_sleep_coprocs_refuse(struct device *dev, bool *pcie_ans_idled);
/* Each ready coprocessor's last check: the first refusal, or 0. */
int apple_sleep_coprocs_check(struct device *dev);
bool apple_sleep_enclave_registered(void);
/* A power word that a ready coprocessor's last stop lowers: on until the commit. */
bool apple_sleep_coproc_lowers(u32 ps);
/* The enclave's stop, last of all refusals: nonzero only if no request was sent. */
int apple_sleep_enclave_stop(struct device *dev);
/* The last stops, those registered first with @first and then the rest. */
void apple_sleep_coprocs_stop(bool first, bool *pcie_ans);

#endif
