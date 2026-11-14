// SPDX-License-Identifier: GPL-2.0-only
/* Execute production stop/routing/recovery with a strict DMA/link model. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>
typedef uint32_t u32;
typedef uint16_t u16;
typedef uint64_t u64;
#define __iomem
#define GENMASK(h, l) ((~0U << (l)) & (~0U >> (31 - (h))))
#define FIELD_PREP(m, v) (((v) << __builtin_ctz(m)) & (m))
#define BIT(n) (1U << (n))
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define READ_ONCE(x) (x)
#define PORT_STATUS 0x804
#define PORT_STATUS_READY BIT(0)
#define PORT_LINKSTS_BUSY BIT(2)
#define RADIO_DBI_RO_WR 0x8bc
#define RADIO_DBI_LINK_CONTROL 0x710
#define RADIO_DBI_WIDTH_SPEED 0x80c
#define PCI_EXP_LNKCAP_SLS 0xf
#define PCI_EXP_LNKCTL2_TLS 0xf
#define PCI_EXP_LNKCAP_MLW GENMASK(9, 4)
#define PORT_INTMSK 0x104
#define PORT_INTSTAT 0x100
#define PORT_APPCLK 0x800
#define PORT_APPCLK_EN BIT(0)
#define PORT_APPCLK_CGDIS BIT(8)
#define PORT_LINKSTS 0x208
#define PORT_LINKSTS_UP BIT(0)
#define PORT_LTSSMCTL 0x80
#define PORT_LTSSMCTL_START BIT(0)
#define PORT_MSICFG 0x124
#define PORT_PERST_OFF BIT(0)
#define PORT_MSICFG_EN BIT(0)
#define PORT_MSIBASE 0x168
#define PHY_LANE_CFG_REFCLK0REQ BIT(0)
#define PHY_LANE_CFG_REFCLK1REQ BIT(1)
#define PHY_LANE_CFG_REFCLK0ACK BIT(2)
#define PHY_LANE_CFG_REFCLK1ACK BIT(3)
#define PHY_LANE_CFG_REFCLKEN (BIT(9) | BIT(10))
#define RADIO_PORT_L2 0x88
#define RADIO_PORT_L2_ENTER 0x11
#define RADIO_PORT_L2_BUSY BIT(0)
#define RADIO_PORT_PERST_GUARD 0x13c
#define RADIO_PHY_RESET BIT(4)
#define RADIO_PHY_LINK BIT(31)
#define RADIO_PORT_TIMEOUT_US 50000
#define PCI_VENDOR_ID 0
#define PCI_COMMAND 4
#define PCI_COMMAND_MASTER 4
struct device {
	int unused;
};
struct gpio_desc {
	int asserted;
};
struct pci_dev {
	u32 identity;
	u16 command;
	bool state_saved;
};
struct hw_info {
	u32 port_perst, port_msimap, port_msiaddr, port_msiaddr_hi;
};
struct pci_config_window {
	void *win;
};
struct apple_tunable {
	unsigned int id;
};
struct apple_pcie {
	const struct hw_info *hw;
	struct pci_config_window *cfg;
};
struct apple_pcie_port {
	struct apple_pcie *pcie;
	void *base, *phy, *np;
};
struct apple_dart_sleep {
	bool off, restored;
};
struct apple_radio_function {
	struct pci_dev *pdev;
};
struct apple_radio_sleep {
	struct apple_pcie *pcie;
	struct apple_pcie_port *port;
	struct apple_dart_sleep dart;
	struct gpio_desc *perst, *clkreq;
	struct pci_dev *root;
	struct apple_radio_function fn[2];
	const struct apple_tunable *tables[4];
	u32 appclk;
	bool config_ready;
	u32 rid[16], msi[32], msicfg, msibase, msiaddr, msiaddr_hi, intmask;
};
static u32 portregs[0x8000 / 4], phyregs[0x100 / 4], dbiregs[0x1000 / 4];
static unsigned int poll_fail, poll_count, table_count, table_at[4],
	dbi_open_at, dbi_close_at;
static bool hold_ack;
static bool timeout, drop_route, fail_dart_off, fail_dart_restore, fail_gpio;
static unsigned int ops, gpio_asserted, dart_off_at, clocks_off_at,
	dart_restore_at, link_at, route_at;
static unsigned int offset(void *p)
{
	return (char *)p - (char *)portregs;
}
static u32 readl(void *p)
{
	return *(u32 *)p;
}
static void writel(u32 value, void *p)
{
	ops++;
	if (p == (void *)phyregs) {
		if (!hold_ack)
			value = (value & ~(BIT(2) | BIT(3))) |
				((value & (BIT(0) | BIT(1))) << 2);
		*(u32 *)p = value;
		return;
	}
	if ((uintptr_t)p >= (uintptr_t)dbiregs &&
	    (uintptr_t)p < (uintptr_t)(dbiregs + ARRAY_SIZE(dbiregs))) {
		if (p == (void *)(dbiregs + RADIO_DBI_RO_WR / 4)) {
			if (value & 1)
				dbi_open_at = ops;
			else
				dbi_close_at = ops;
		}
		*(u32 *)p = value;
		return;
	}
	unsigned int off = offset(p);

	assert(off < sizeof(portregs));
	if (drop_route && off == 0x3008)
		return;
	if (off >= 0x3000)
		route_at = ops;
	if (off == RADIO_PORT_L2 && !timeout)
		value &= ~1U;
	if (off == 0x82c && (value & 1))
		portregs[PORT_STATUS / 4] = PORT_STATUS_READY;
	if (off == PORT_LTSSMCTL && value == 1) {
		assert(dart_restore_at);
		link_at = ops;
		portregs[PORT_LINKSTS / 4] = 1;
	}
	if (off == PORT_APPCLK && !(value & PORT_APPCLK_EN))
		clocks_off_at = ops;
	*(u32 *)p = value;
}
#define readl_poll_timeout_atomic(p, v, cond, delay, t)           \
	({                                                        \
		(void)(delay);                                    \
		(void)(t);                                        \
		v = readl(p);                                     \
		timeout ? -ETIMEDOUT : ((cond) ? 0 : -ETIMEDOUT); \
	})
#define readl_poll_timeout(p, v, cond, delay, t)                     \
	({                                                           \
		(void)(delay);                                       \
		(void)(t);                                           \
		v = readl(p);                                        \
		poll_count++;                                        \
		poll_fail == poll_count ? -ETIMEDOUT :               \
					  ((cond) ? 0 : -ETIMEDOUT); \
	})
#define read_poll_timeout(op, v, cond, delay, t, sleep, p, off)   \
	({                                                        \
		(void)(delay);                                    \
		(void)(t);                                        \
		(void)(sleep);                                    \
		v = op(p, off);                                   \
		timeout ? -ETIMEDOUT : ((cond) ? 0 : -ETIMEDOUT); \
	})
static u32 apple_pcie_port_readl(struct apple_pcie_port *p, u32 off)
{
	return readl(p->base + off);
}
static void apple_pcie_port_writel(struct apple_pcie_port *p, u32 v, u32 off)
{
	writel(v, p->base + off);
}
static void apple_pcie_port_rmw_set(struct apple_pcie_port *p, u32 bits,
				    u32 off)
{
	apple_pcie_port_writel(p, apple_pcie_port_readl(p, off) | bits, off);
}
static void apple_pcie_port_rmw_clear(struct apple_pcie_port *p, u32 bits,
				      u32 off)
{
	apple_pcie_port_writel(p, apple_pcie_port_readl(p, off) & ~bits, off);
}
static u32 port_rid2sid_offset(struct apple_pcie_port *p, int i)
{
	(void)p;
	return 0x3000 + 4 * i;
}
static int gpiod_set_value(struct gpio_desc *g, int v)
{
	if (fail_gpio)
		return -EIO;
	g->asserted = v;
	ops++;
	if (v) {
		gpio_asserted = ops;
		portregs[PORT_LINKSTS / 4] = 0;
	}
	return 0;
}
static int gpiod_direction_output(struct gpio_desc *g, int v)
{
	return gpiod_set_value(g, v);
}
static int gpiod_direction_input(struct gpio_desc *g)
{
	(void)g;
	return fail_gpio ? -EIO : 0;
}
static int gpiod_get_value(struct gpio_desc *g)
{
	return g->asserted;
}
static int apple_dart_sleep_power_off(struct apple_dart_sleep *d)
{
	assert(gpio_asserted);
	if (fail_dart_off)
		return -EIO;
	d->off = true;
	dart_off_at = ++ops;
	return 0;
}
static int apple_dart_sleep_power_restore(struct apple_dart_sleep *d)
{
	assert(route_at);
	if (fail_dart_restore)
		return -EIO;
	d->restored = true;
	dart_restore_at = ++ops;
	return 0;
}
static int of_pci_get_max_link_speed(void *np)
{
	(void)np;
	return 3;
}
static void apple_tunable_apply(void *base, const struct apple_tunable *t)
{
	assert(t->id == table_count++);
	table_at[t->id] = ++ops;
	if (t->id) {
		assert(base == dbiregs && dbi_open_at && !dbi_close_at);
		assert(dbiregs[RADIO_DBI_RO_WR / 4] & 1);
	} else
		assert(base == portregs);
}
static u16 readw(void *p)
{
	return *(u16 *)p;
}
static void writew(u16 v, void *p)
{
	*(u16 *)p = v;
	ops++;
}
static void pci_restore_state(struct pci_dev *p)
{
	p->state_saved = false;
}
static int pci_read_config_dword(struct pci_dev *p, int off, u32 *v)
{
	assert(off == PCI_VENDOR_ID);
	*v = p->identity;
	return 0;
}
static int pci_read_config_word(struct pci_dev *p, int off, u16 *v)
{
	assert(off == PCI_COMMAND);
	*v = p->command;
	return 0;
}
static void msleep(unsigned int v)
{
	(void)v;
}
#define CONFIG_ARM64 1
#define USEC_PER_SEC 1000000
static u64 tick;
static bool stopped_counter, backward_counter;
static u64 arch_timer_read_counter(void)
{
	if (backward_counter)
		return tick--;
	return stopped_counter ? tick : (tick += 1000);
}
static u32 arch_timer_get_cntfrq(void)
{
	return 1000000;
}
static void cpu_relax(void)
{
}
/* ACTUAL_SPIN */
/* ACTUAL_FUNCTIONS */
static struct hw_info hw = { .port_perst = 0x82c,
			     .port_msimap = 0x3800,
			     .port_msiaddr = 0x168,
			     .port_msiaddr_hi = 0x16c };
static struct pci_config_window cfg = { .win = dbiregs };
static struct apple_tunable tables[4] = { { 0 }, { 1 }, { 2 }, { 3 } };
static struct apple_pcie pcie = { .hw = &hw, .cfg = &cfg };
static struct apple_pcie_port port = { .pcie = &pcie,
				       .base = portregs,
				       .phy = phyregs };
static struct gpio_desc reset, clkreq;
static struct pci_dev root, wifi, bt;
static struct apple_radio_sleep s;
static void fresh(void)
{
	memset(portregs, 0, sizeof(portregs));
	memset(phyregs, 0, sizeof(phyregs));
	memset(&s, 0, sizeof(s));
	memset(dbiregs, 0, sizeof(dbiregs));
	poll_fail = poll_count = table_count = dbi_open_at = dbi_close_at = 0;
	memset(table_at, 0, sizeof(table_at));
	hold_ack = stopped_counter = backward_counter = false;
	tick = 0;
	for (unsigned int i = 0; i < 4; i++)
		s.tables[i] = &tables[i];
	s.appclk = PORT_APPCLK_CGDIS;
	s.pcie = &pcie;
	s.port = &port;
	s.perst = &reset;
	s.clkreq = &clkreq;
	s.root = &root;
	s.fn[0].pdev = &wifi;
	s.fn[1].pdev = &bt;
	wifi.identity = 0x443414e4;
	bt.identity = 0x5f7214e4;
	wifi.command = bt.command = 0;
	root.state_saved = true;
	s.msicfg = 1;
	s.msibase = s.msiaddr = 0xfffff000;
	s.intmask = 0x1234;
	for (unsigned int i = 0; i < 16; i++)
		s.rid[i] = 0x80000100 + i;
	for (unsigned int i = 0; i < 32; i++)
		s.msi[i] = 0x80000000 + i;
	portregs[PORT_APPCLK / 4] = PORT_APPCLK_EN;
	portregs[PORT_LINKSTS / 4] = 1;
	phyregs[0] = ~0U;
	timeout = drop_route = fail_dart_off = fail_dart_restore = fail_gpio =
		false;
	ops = gpio_asserted = dart_off_at = clocks_off_at = dart_restore_at =
		link_at = route_at = 0;
}
int main(void)
{
	fresh();
	assert(!apple_radio_recover(&s));
	assert(dart_restore_at < link_at && !reset.asserted);
	assert(portregs[PORT_INTMSK / 4] == s.intmask && wifi.command == 0 &&
	       bt.command == 0);
	for (unsigned int i = 0; i < 16; i++)
		assert(portregs[(0x3000 + 4 * i) / 4] == s.rid[i]);
	fresh();
	drop_route = true;
	assert(apple_radio_recover(&s) == -EIO);
	assert(!dart_restore_at && !link_at && reset.asserted);
	fresh();
	fail_dart_restore = true;
	assert(apple_radio_recover(&s) == -EIO);
	assert(!link_at && reset.asserted);
	fresh();
	fail_gpio = true;
	assert(apple_radio_recover(&s) == -EIO);
	assert(!dart_restore_at && !link_at);
	for (unsigned int i = 1; i <= 4; i++) {
		fresh();
		poll_fail = i;
		assert(apple_radio_recover(&s) == -ETIMEDOUT);
		assert(!dart_restore_at && !link_at && reset.asserted &&
		       !dbi_open_at);
	}
	fresh();
	assert(!apple_radio_recover(&s));
	assert(table_count == 4 && table_at[0] < dbi_open_at &&
	       table_at[3] < dbi_close_at && dbi_close_at < route_at);
	assert((dbiregs[0x7c / 4] & PCI_EXP_LNKCAP_SLS) == 3);
	assert((dbiregs[0x7c / 4] & PCI_EXP_LNKCAP_MLW) == BIT(4));
	assert((dbiregs[0x9c / 4] & GENMASK(6, 1)) == 14);
	assert((*(u16 *)((char *)dbiregs + 0xa0) & PCI_EXP_LNKCTL2_TLS) == 3);
	assert(portregs[PORT_APPCLK / 4] & PORT_APPCLK_CGDIS);
	fresh();
	tick = 10;
	backward_counter = true;
	assert(apple_sleep_spin(NULL, 0, 0, 1000) == -EIO);
	fresh();
	stopped_counter = true;
	assert(apple_sleep_spin(NULL, 0, 0, 1000) == -ETIMEDOUT);
	fresh();
	wifi.identity = 0xffffffff;
	assert(apple_radio_recover(&s) == -ENODEV);
	fresh();
	wifi.command = PCI_COMMAND_MASTER;
	assert(apple_radio_recover(&s) == -EUCLEAN);
	puts("PASS: port recovery, routing, DART/link ordering and failures");
}
