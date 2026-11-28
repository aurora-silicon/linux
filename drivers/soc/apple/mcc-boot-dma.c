// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * A display controller can keep scanning out the boot loader's framebuffer
 * after it binds, or with its node disabled. Its driver reserves a record
 * while it tracks that boot scanout and marks it once a displayed swap of
 * its own has retired it.
 */
#include <linux/bug.h>
#include <linux/device.h>
#include <linux/errno.h>
#include <linux/export.h>
#include <linux/init.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/notifier.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/soc/apple/mcc.h>
#include <linux/workqueue.h>

struct apple_mcc_scanout {
	struct list_head link;
	struct device_node *supplier;
	bool retired;
};

struct apple_mcc_scanout_waiter {
	struct list_head link;
	struct device_node *consumer;
	struct device_node *supplier;
};

/*
 * Retirement survives either driver's unbind and reprobe; records and
 * waiters go when a node they name is detached.
 */
static DEFINE_MUTEX(boot_scanout_lock);
static LIST_HEAD(boot_scanouts);
static LIST_HEAD(waiting_consumers);
static LIST_HEAD(retry_consumers);
static LIST_HEAD(enabled_consumers);

/* A node detached already: the notifier has dropped, or will drop, its entries. */
static bool node_detached(struct device_node *np)
{
	return of_node_check_flag(np, OF_DETACHED);
}

static struct apple_mcc_scanout *find_scanout_locked(struct device_node *supplier)
{
	struct apple_mcc_scanout *scanout;

	list_for_each_entry(scanout, &boot_scanouts, link) {
		if (scanout->supplier == supplier)
			return scanout;
	}
	return NULL;
}

int apple_mcc_boot_scanout_reserve(struct device_node *supplier)
{
	struct apple_mcc_scanout *scanout;
	int ret = 0;

	mutex_lock(&boot_scanout_lock);
	if (node_detached(supplier)) {
		ret = -ENODEV;
		goto out_unlock;
	}
	/* A scanout retired before the supplier was rebound stays retired. */
	if (find_scanout_locked(supplier))
		goto out_unlock;
	scanout = kzalloc_obj(*scanout);
	if (!scanout) {
		ret = -ENOMEM;
		goto out_unlock;
	}
	scanout->supplier = of_node_get(supplier);
	list_add_tail(&scanout->link, &boot_scanouts);

out_unlock:
	mutex_unlock(&boot_scanout_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(apple_mcc_boot_scanout_reserve);

/*
 * Held across a cache controller's final check of its nodes and its
 * allocation enable, and by the detach notifier. OF flags a node detached
 * before it calls the notifier, so probe cannot hold a detach off; but the
 * notifier always runs after the flag is set. Either probe sees the flag,
 * or it enabled allocation and registered first, and the notifier then
 * turns allocation off (apple_mcc_boot_dma_lost()).
 */
void apple_mcc_boot_dma_lock(void)
{
	mutex_lock(&boot_scanout_lock);
}
EXPORT_SYMBOL_GPL(apple_mcc_boot_dma_lock);

void apple_mcc_boot_dma_unlock(void)
{
	mutex_unlock(&boot_scanout_lock);
}
EXPORT_SYMBOL_GPL(apple_mcc_boot_dma_unlock);

/* Under apple_mcc_boot_dma_lock(), once allocation is enabled. */
void apple_mcc_boot_dma_register_locked(struct apple_mcc_boot_dma_consumer *consumer)
{
	list_add_tail(&consumer->link, &enabled_consumers);
}
EXPORT_SYMBOL_GPL(apple_mcc_boot_dma_register_locked);

/* Before the consumer turns allocation off and goes. */
void apple_mcc_boot_dma_unregister(struct apple_mcc_boot_dma_consumer *consumer)
{
	mutex_lock(&boot_scanout_lock);
	list_del_init(&consumer->link);
	mutex_unlock(&boot_scanout_lock);
}
EXPORT_SYMBOL_GPL(apple_mcc_boot_dma_unregister);

/*
 * Whether detaching @np leaves @consumer's boot DMA unconfirmed: its own
 * node, or a boot DMA supplier whose boot scanout has not retired. The
 * detached node no longer resolves, so match its phandle.
 */
static bool consumer_loses_boot_dma_locked(struct apple_mcc_boot_dma_consumer *consumer,
					   struct device_node *np)
{
	struct device_node *node = consumer->dev->of_node;
	struct apple_mcc_scanout *scanout;
	u32 phandle;
	int i, n;

	if (node == np)
		return true;
	if (!np->phandle)
		return false;
	n = of_property_count_u32_elems(node, "apple,boot-dma-devices");
	for (i = 0; i < n; i++) {
		if (of_property_read_u32_index(node, "apple,boot-dma-devices", i, &phandle) ||
		    phandle != np->phandle)
			continue;
		scanout = find_scanout_locked(np);
		return !scanout || !scanout->retired;
	}
	return false;
}

static void waiter_free(struct apple_mcc_scanout_waiter *waiter)
{
	of_node_put(waiter->consumer);
	of_node_put(waiter->supplier);
	kfree(waiter);
}

int apple_mcc_boot_scanout_ready(struct device *dev, struct device_node *supplier)
{
	struct apple_mcc_scanout_waiter *waiter;
	struct apple_mcc_scanout *scanout;
	struct device_node *consumer = dev->of_node;
	int ret = -EPROBE_DEFER;

	mutex_lock(&boot_scanout_lock);
	if (node_detached(consumer) || node_detached(supplier)) {
		ret = -ENODEV;
		goto out_unlock;
	}
	scanout = find_scanout_locked(supplier);
	if (scanout && scanout->retired) {
		ret = 0;
		goto out_unlock;
	}

	list_for_each_entry(waiter, &waiting_consumers, link) {
		if (waiter->consumer == consumer && waiter->supplier == supplier)
			goto out_unlock;
	}
	waiter = kzalloc_obj(*waiter);
	if (!waiter) {
		ret = -ENOMEM;
		goto out_unlock;
	}
	waiter->consumer = of_node_get(consumer);
	waiter->supplier = of_node_get(supplier);
	list_add_tail(&waiter->link, &waiting_consumers);

out_unlock:
	mutex_unlock(&boot_scanout_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(apple_mcc_boot_scanout_ready);

static void apple_mcc_retry(struct work_struct *work)
{
	struct apple_mcc_scanout_waiter *waiter, *tmp;
	LIST_HEAD(pending);

	mutex_lock(&boot_scanout_lock);
	list_splice_tail_init(&retry_consumers, &pending);
	mutex_unlock(&boot_scanout_lock);

	list_for_each_entry_safe(waiter, tmp, &pending, link) {
		struct platform_device *pdev = NULL;

		list_del(&waiter->link);
		mutex_lock(&boot_scanout_lock);
		if (!node_detached(waiter->consumer) && !node_detached(waiter->supplier))
			pdev = of_find_device_by_node(waiter->consumer);
		mutex_unlock(&boot_scanout_lock);
		if (pdev) {
			device_initial_probe(&pdev->dev);
			put_device(&pdev->dev);
		}
		waiter_free(waiter);
	}
}

static DECLARE_WORK(retry_work, apple_mcc_retry);

void apple_mcc_boot_scanout_retired(struct device_node *supplier)
{
	struct apple_mcc_scanout_waiter *waiter, *tmp;
	struct apple_mcc_scanout *scanout;
	bool schedule = false;

	mutex_lock(&boot_scanout_lock);
	/* Reserved by apple_mcc_boot_scanout_reserve(): marking it cannot fail. */
	scanout = find_scanout_locked(supplier);
	if (scanout)
		scanout->retired = true;
	else
		WARN_ON_ONCE(!node_detached(supplier));

	list_for_each_entry_safe(waiter, tmp, &waiting_consumers, link) {
		if (waiter->supplier == supplier) {
			list_move_tail(&waiter->link, &retry_consumers);
			schedule = true;
		}
	}
	mutex_unlock(&boot_scanout_lock);

	if (schedule)
		schedule_work(&retry_work);
}
EXPORT_SYMBOL_GPL(apple_mcc_boot_scanout_retired);

static void drop_waiters_locked(struct list_head *list, struct device_node *np)
{
	struct apple_mcc_scanout_waiter *waiter, *tmp;

	list_for_each_entry_safe(waiter, tmp, list, link) {
		if (waiter->consumer == np || waiter->supplier == np) {
			list_del(&waiter->link);
			waiter_free(waiter);
		}
	}
}

static int apple_mcc_of_notify(struct notifier_block *nb, unsigned long action, void *arg)
{
	struct apple_mcc_boot_dma_consumer *consumer, *next;
	struct of_reconfig_data *rd = arg;
	struct apple_mcc_scanout *scanout, *tmp;

	if (action != OF_RECONFIG_DETACH_NODE)
		return NOTIFY_DONE;

	mutex_lock(&boot_scanout_lock);
	/* Before the records go: a retired supplier may detach. */
	list_for_each_entry_safe(consumer, next, &enabled_consumers, link) {
		if (consumer_loses_boot_dma_locked(consumer, rd->dn)) {
			list_del_init(&consumer->link);
			consumer->lost(consumer);
		}
	}
	list_for_each_entry_safe(scanout, tmp, &boot_scanouts, link) {
		if (scanout->supplier == rd->dn) {
			list_del(&scanout->link);
			of_node_put(scanout->supplier);
			kfree(scanout);
		}
	}
	drop_waiters_locked(&waiting_consumers, rd->dn);
	drop_waiters_locked(&retry_consumers, rd->dn);
	mutex_unlock(&boot_scanout_lock);
	return NOTIFY_OK;
}

static struct notifier_block apple_mcc_of_nb = {
	.notifier_call = apple_mcc_of_notify,
};

static int __init apple_mcc_boot_dma_init(void)
{
	/* Without CONFIG_OF_DYNAMIC no node is ever detached. */
	if (IS_ENABLED(CONFIG_OF_DYNAMIC))
		return of_reconfig_notifier_register(&apple_mcc_of_nb);
	return 0;
}
core_initcall(apple_mcc_boot_dma_init);
