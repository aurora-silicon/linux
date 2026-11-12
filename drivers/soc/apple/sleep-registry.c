// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * Apple SoC sleep: the coprocessor and enclave registry
 *
 * A driver whose coprocessor can only be stopped once a sleep is certain
 * registers it here, readies it in its own suspend and leaves the last stop to
 * the commit of whichever sleep controller is bound on this SoC (sleep.c,
 * t6040-sleep.c). The table lives apart from the controllers so that one
 * kernel can carry all of them; each binds only to its own SoC.
 */

#include <linux/cleanup.h>
#include <linux/device.h>
#include <linux/errno.h>
#include <linux/export.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/soc/apple/sleep.h>
#include <linux/string.h>

#include <asm/virt.h>

#include "sleep-internal.h"

struct apple_sleep_coproc apple_sleep_coprocs[APPLE_SLEEP_COPROCS];
struct apple_sleep_enclave apple_sleep_enclave;
/* Guards the table, the enclave slot and the bound controller. */
DEFINE_MUTEX(apple_sleep_coprocs_lock);

static const struct apple_sleep_ctl *apple_sleep_ctl;

int apple_sleep_registry_bind(const struct apple_sleep_ctl *ctl)
{
	guard(mutex)(&apple_sleep_coprocs_lock);
	if (apple_sleep_ctl)
		return -EBUSY;
	WRITE_ONCE(apple_sleep_ctl, ctl);

	return 0;
}

void apple_sleep_registry_unbind(const struct apple_sleep_ctl *ctl)
{
	guard(mutex)(&apple_sleep_coprocs_lock);
	if (apple_sleep_ctl == ctl)
		WRITE_ONCE(apple_sleep_ctl, NULL);
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
	c->ps = 0;
	c->dev = dev;

	return 0;
}
EXPORT_SYMBOL_GPL(apple_sleep_coproc_register);

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
	const struct apple_sleep_ctl *ctl;
	int ret;

	if (!is_kernel_in_hyp_mode())
		return -ENODEV;

	if (!stop)
		return -EINVAL;
	ctl = READ_ONCE(apple_sleep_ctl);
	if (!ctl)
		return -EOPNOTSUPP;
	if (ctl->enclave_admit) {
		ret = ctl->enclave_admit();
		if (ret)
			return ret;
	}

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
	const struct apple_sleep_ctl *ctl = READ_ONCE(apple_sleep_ctl);

	return ctl && ctl->commit_stops();
}
EXPORT_SYMBOL_GPL(apple_sleep_commit_stops_coprocs);
