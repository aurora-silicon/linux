// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * Apple SoC sleep: the coprocessor and enclave registry
 *
 * A driver whose coprocessor can only be stopped once a sleep is certain
 * registers it here, readies it in its own suspend and leaves the last stop to
 * the commit of whichever sleep controller is bound on this SoC (sleep.c,
 * sleep-t6040.c). The table lives apart from the controllers so that one
 * kernel can carry all of them; each binds only to its own SoC.
 */

#include <linux/cleanup.h>
#include <linux/device.h>
#include <linux/errno.h>
#include <linux/export.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/seq_file.h>
#include <linux/soc/apple/sleep.h>
#include <linux/string.h>

#include <asm/barrier.h>
#include <asm/virt.h>

#include "sleep-internal.h"

struct apple_sleep_coproc apple_sleep_coprocs[APPLE_SLEEP_COPROCS];
struct apple_sleep_enclave apple_sleep_enclave;
/* Guards the table, the enclave slot and the bound controller. */
DEFINE_MUTEX(apple_sleep_coprocs_lock);

#define APPLE_SLEEP_REG_BOUND	BIT(8)
#define APPLE_SLEEP_REG_READY	BIT(9)

/* The bound controller's flags and state. */
static unsigned int apple_sleep_reg;

int apple_sleep_registry_bind(unsigned int flags)
{
	guard(mutex)(&apple_sleep_coprocs_lock);
	if (apple_sleep_reg & APPLE_SLEEP_REG_BOUND)
		return -EBUSY;
	WRITE_ONCE(apple_sleep_reg, flags | APPLE_SLEEP_REG_BOUND);

	return 0;
}

void apple_sleep_registry_unbind(void)
{
	guard(mutex)(&apple_sleep_coprocs_lock);
	WRITE_ONCE(apple_sleep_reg, 0);
}

void apple_sleep_registry_ready(void)
{
	guard(mutex)(&apple_sleep_coprocs_lock);
	WRITE_ONCE(apple_sleep_reg, apple_sleep_reg | APPLE_SLEEP_REG_READY);
}

struct apple_sleep_coproc *apple_sleep_coproc_find(struct device *dev)
{
	int i;

	for (i = 0; i < APPLE_SLEEP_COPROCS; i++)
		if (apple_sleep_coprocs[i].dev == dev)
			return &apple_sleep_coprocs[i];

	return NULL;
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
	c->order = 0;
	c->ps = 0;
	c->dev = dev;

	return 0;
}
EXPORT_SYMBOL_GPL(apple_sleep_coproc_register);

int apple_sleep_coproc_register_first(struct device *dev, unsigned int order,
				      int (*last_stop)(void *data), void *data)
{
	struct apple_sleep_coproc *c;
	struct device_node *np;
	u32 ps = 0;
	int ret;

	if (!last_stop || order >= APPLE_SLEEP_FIRST_ORDERS)
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
		c->order = order;
		c->ps = ps;
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

/* With no sleep controller bound, nothing would ever send the request: refuse it. */
int apple_sleep_enclave_register(struct device *dev, int (*stop)(void *data), void *data)
{
	unsigned int reg;

	if (!is_kernel_in_hyp_mode())
		return -ENODEV;

	if (!stop)
		return -EINVAL;
	reg = READ_ONCE(apple_sleep_reg);
	if (!(reg & APPLE_SLEEP_REG_BOUND))
		return -EOPNOTSUPP;
	if (!(reg & APPLE_SLEEP_REG_ENCLAVE_ANY) && !apple_sleep_commit_stops_coprocs())
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

	guard(mutex)(&apple_sleep_coprocs_lock);
	c = apple_sleep_coproc_find(dev);
	if (c)
		memset(c, 0, sizeof(*c));
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
	return (READ_ONCE(apple_sleep_reg) & APPLE_SLEEP_REG_READY) && is_kernel_in_hyp_mode() &&
	       apple_sleep_power_loss();
}
EXPORT_SYMBOL_GPL(apple_sleep_commit_stops_coprocs);

/*
 * The last thing that can fail on the way to the commit: no CPU but this one is up and
 * interrupts are off, so no lock can be held. A coprocessor stopped here comes back only
 * through a real sleep, so every refusal has to be behind this, and nothing after it may
 * go back. In this order, on ANS itself: whatever its driver does last,
 * status and control read, then 0 written twice.
 */
static void apple_sleep_coproc_run_off(struct apple_sleep_coproc *c)
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
bool apple_sleep_coproc_lowers(u32 ps)
{
	int i;

	for (i = 0; i < APPLE_SLEEP_COPROCS; i++)
		if (apple_sleep_coprocs[i].dev && apple_sleep_coprocs[i].ready &&
		    apple_sleep_coprocs[i].first && apple_sleep_coprocs[i].ps == ps)
			return true;
	return false;
}

bool apple_sleep_enclave_registered(void)
{
	return apple_sleep_enclave.dev;
}

void apple_sleep_registry_show(struct seq_file *s)
{
	int i;

	guard(mutex)(&apple_sleep_coprocs_lock);
	seq_puts(s, "coprocessors registered for the commit's last stop:\n");
	for (i = 0; i < APPLE_SLEEP_COPROCS; i++) {
		struct apple_sleep_coproc *c = &apple_sleep_coprocs[i];

		if (!c->dev)
			continue;
		seq_printf(s, "  %s%s%s%s%s\n", dev_name(c->dev), c->pcie_ans ? " (ANS)" : "",
			   c->first ? " (first)" : "", c->last_check ? ", last check" : "",
			   c->ready ? ", ready" : "");
	}
	seq_printf(s, "enclave sleep request left for the last refusal: %s\n",
		   apple_sleep_enclave.dev ? dev_name(apple_sleep_enclave.dev) : "none");
}

/* A registered coprocessor that isn't ready refuses the sleep. */
unsigned int apple_sleep_coprocs_refuse(struct device *dev, bool *pcie_ans_idled)
{
	unsigned int refused = 0;
	int i;

	for (i = 0; i < APPLE_SLEEP_COPROCS; i++) {
		struct apple_sleep_coproc *c = &apple_sleep_coprocs[i];

		if (!c->dev)
			continue;
		if (c->ready) {
			*pcie_ans_idled |= c->pcie_ans;
		} else {
			/* Nothing may lower a coprocessor's domain under its driver. */
			dev_err(dev, "refused: %s is running and its driver has not quiesced it\n",
				dev_name(c->dev));
			refused++;
		}
	}

	return refused;
}

/* The last refusals, each coprocessor's own: nothing has stopped yet. */
int apple_sleep_coprocs_check(struct device *dev)
{
	int i, ret;

	for (i = 0; i < APPLE_SLEEP_COPROCS; i++) {
		struct apple_sleep_coproc *c = &apple_sleep_coprocs[i];

		if (!c->dev || !c->ready || c->stopped || !c->last_check)
			continue;
		ret = c->last_check(c->data);
		if (ret) {
			dev_err(dev, "refused: %s: %d\n", dev_name(c->dev), ret);
			return ret;
		}
	}

	return 0;
}

/* A nonzero result must mean no SLEEP was sent; post-send errors are fatal. */
int apple_sleep_enclave_stop(struct device *dev)
{
	int ret;

	if (!apple_sleep_enclave.stop)
		return 0;
	ret = apple_sleep_enclave.stop(apple_sleep_enclave.data);
	if (ret)
		dev_err(dev, "refused before SEP SLEEP was sent: %d\n", ret);

	return ret;
}

/*
 * In two passes, after the last refusal: from the first stop only a real sleep restarts the
 * GPU, so a failure here goes on to the commit. Those registered first stop whole first,
 * before the off words drop under them: the GPU withdraws its PMP request there, and the
 * PMP has to see that before its own stop. The rest after the match. With ANS among the ready
 * ones, @pcie_ans.
 */
void apple_sleep_coprocs_stop(bool first, bool *pcie_ans)
{
	unsigned int order;
	int i;

	if (first)
		*pcie_ans = false;
	/* Those registered first run by their order, the others in one pass over the table. */
	for (order = 0; order < (first ? APPLE_SLEEP_FIRST_ORDERS : 1); order++) {
		for (i = 0; i < APPLE_SLEEP_COPROCS; i++) {
			struct apple_sleep_coproc *c = &apple_sleep_coprocs[i];

			if (!c->dev || !c->ready || c->first != first)
				continue;
			if (first && c->order != order)
				continue;
			*pcie_ans |= c->pcie_ans;
			if (c->stopped)
				continue;
			c->stop_ret = c->last_stop ? c->last_stop(c->data) : 0;
			apple_sleep_coproc_run_off(c);
		}
	}
	dsb(sy);
}

/*
 * A controller that was tested with neither the lid's state at the wake nor the USB wake ports
 * binds without the capability: the queries then answer as they do with no sleep driver.
 */
static bool apple_sleep_declines(unsigned int cap)
{
	unsigned int reg = READ_ONCE(apple_sleep_reg);

	return (reg & APPLE_SLEEP_REG_BOUND) && !(reg & cap);
}

static unsigned long sleep_usb_wake_ports;

void apple_sleep_usb_set_wake_ports(unsigned long ports)
{
	WRITE_ONCE(sleep_usb_wake_ports, ports);
}
EXPORT_SYMBOL_GPL(apple_sleep_usb_set_wake_ports);

bool apple_sleep_usb_port_can_wake(unsigned int port)
{
	if (apple_sleep_declines(APPLE_SLEEP_REG_USB_WAKE_PORTS))
		return false;

	return port < BITS_PER_LONG && (READ_ONCE(sleep_usb_wake_ports) & BIT(port));
}
EXPORT_SYMBOL_GPL(apple_sleep_usb_port_can_wake);

/* Read in the SMC's noirq resume. Forgotten as each sleep begins. */
static int sleep_lid_closed = -ENODATA;

void apple_sleep_lid_at_wake(bool closed)
{
	WRITE_ONCE(sleep_lid_closed, closed);
}
EXPORT_SYMBOL_GPL(apple_sleep_lid_at_wake);

int apple_sleep_lid_closed_at_wake(void)
{
	if (apple_sleep_declines(APPLE_SLEEP_REG_LID_AT_WAKE))
		return -ENODATA;

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

/* Each sleep begins with no answer about the wake before it. */
void apple_sleep_wake_info_forget(void)
{
	WRITE_ONCE(sleep_lid_closed, -ENODATA);
	WRITE_ONCE(sleep_radio_woke, -ENODATA);
}
