// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * System sleep for the SEP driver: the notifier only forwards "about to sleep"
 * and "back from sleep" to Rust, which ends any Touch ID capture before the
 * system suspends (see pm.rs), and the enclave's power domains are kept on
 * across sleep. The device's suspend and resume callbacks are the firmware
 * suspend's: the enclave is told, and gates its own domain, whose state word is
 * read here. On T8140 at EL2 the sleep driver sends the request, as its last
 * refusal.
 */
#include <linux/compiler.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/mfd/syscon.h>
#include <linux/notifier.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/pm.h>
#include <linux/pm_domain.h>
#include <linux/regmap.h>
#include <linux/soc/apple/dart.h>
#include <linux/soc/apple/mailbox.h>
#include <linux/soc/apple/sleep.h>
#include <linux/suspend.h>

#include "shim.h"

/* The state word of a PMGR power domain, as pmgr-pwrstate has it. */
#define SEP_PS_DEV_DISABLE	BIT(10)

static void (*sep_pm_event)(bool entering);
static int (*sep_pm_sleep)(void);
static void (*sep_pm_wake)(void);
static bool (*sep_pm_told)(void);
/* In the device's .suspend, the only caller that can leave the request to the sleep. */
static bool sep_pm_suspending;

static int sep_pm_notify(struct notifier_block *nb, unsigned long action,
			 void *data)
{
	void (*event)(bool entering) = READ_ONCE(sep_pm_event);

	if (!event)
		return NOTIFY_DONE;

	switch (action) {
	case PM_SUSPEND_PREPARE:
	case PM_HIBERNATION_PREPARE:
	case PM_RESTORE_PREPARE:
		event(true);
		return NOTIFY_OK;
	case PM_POST_SUSPEND:
	case PM_POST_HIBERNATION:
	case PM_POST_RESTORE:
		event(false);
		return NOTIFY_OK;
	default:
		return NOTIFY_DONE;
	}
}

static struct notifier_block sep_pm_nb = {
	.notifier_call = sep_pm_notify,
};

/* The enclave's own domain is the first one its node lists. */
static int sep_pm_domain(struct device *dev, struct regmap **regmap, u32 *offset)
{
	struct device_node *np;
	int ret;

	np = of_parse_phandle(dev->of_node, "power-domains", 0);
	if (!np)
		return -ENOENT;

	*regmap = syscon_node_to_regmap(np->parent);
	ret = PTR_ERR_OR_ZERO(*regmap);
	if (!ret)
		ret = of_property_read_u32(np, "reg", offset);
	of_node_put(np);

	return ret;
}

/*
 * Returns 0 once the enclave has gated itself, which shows as DEV_DISABLE, and
 * -ENOENT if there is no domain to read. The actual state is no guide: a running
 * enclave idles at 0 with auto-enable on.
 */
int sep_pm_wait_gated(void *ptr, unsigned int timeout_ms)
{
	struct regmap *regmap;
	u32 offset, val;
	int ret;

	ret = sep_pm_domain(ptr, &regmap, &offset);
	if (ret)
		return ret;

	return regmap_read_poll_timeout(regmap, offset, val,
					val & SEP_PS_DEV_DISABLE,
					1000, timeout_ms * USEC_PER_MSEC);
}

int sep_pm_clear_dev_disable(void *ptr)
{
	struct regmap *regmap;
	u32 offset, val;
	int ret;

	ret = sep_pm_domain(ptr, &regmap, &offset);
	if (ret)
		return ret;

	ret = regmap_read(regmap, offset, &val);
	if (ret || !(val & SEP_PS_DEV_DISABLE))
		return ret;

	return regmap_write(regmap, offset, val & ~SEP_PS_DEV_DISABLE);
}

/* The enclave's DART, the first one its node lists, with a reference held. */
static struct device *sep_pm_dart(struct device *dev)
{
	struct platform_device *pdev;
	struct device_node *np;

	np = of_parse_phandle(dev->of_node, "iommus", 0);
	if (!np)
		return NULL;
	pdev = of_find_device_by_node(np);
	of_node_put(np);

	return pdev ? &pdev->dev : NULL;
}

int sep_pm_dart_prepare(void *ptr)
{
	struct device *dart = sep_pm_dart(ptr);
	int ret;

	if (!dart)
		return -ENODEV;
	ret = apple_dart_sleep_prepare(dart);
	put_device(dart);

	return ret;
}

void sep_pm_dart_restore(void *ptr)
{
	struct device *dart = sep_pm_dart(ptr);

	if (dart) {
		apple_dart_sleep_restore(dart);
		put_device(dart);
	}
}

#define SEP_PM_POLL_US		20
#define SEP_PM_TIMEOUT_US	(5000 * USEC_PER_MSEC)

/*
 * The sleep request, sent by the sleep driver as its very last refusal, with interrupts off:
 * an enclave that gated doesn't come back with its power kept, so a sleep refused anywhere
 * before must find it never told. The driver hands over the request and its answer's match
 * at .suspend, with its receive interrupt held.
 */
static struct sep_pm_commit {
	struct device *dev;
	struct device *dart;
	struct apple_mbox *mbox;
	void __iomem *ps;
	struct apple_mbox_msg request;
	u64 reply, reply_mask;
	void (*rx)(struct apple_mbox *mbox, struct apple_mbox_msg msg, void *cookie);
	void *cookie;
	bool registered, sent, answered;
	unsigned int answer_us;
	int gate_ret;
	u32 state;
} sep_pm_commit;

/* The answer stops here. Anything else goes to the driver, which reads it after the wake. */
static void sep_pm_commit_rx(struct apple_mbox *mbox, struct apple_mbox_msg msg, void *cookie)
{
	struct sep_pm_commit *c = cookie;

	if (c->sent && (msg.msg0 & c->reply_mask) == c->reply) {
		c->answered = true;
		return;
	}
	if (c->rx)
		c->rx(mbox, msg, c->cookie);
}

static void sep_pm_commit_take_rx(struct sep_pm_commit *c, bool take)
{
	unsigned long flags;

	spin_lock_irqsave(&c->mbox->rx_lock, flags);
	if (take) {
		c->rx = c->mbox->rx;
		c->cookie = c->mbox->cookie;
		c->mbox->rx = sep_pm_commit_rx;
		c->mbox->cookie = c;
	} else {
		c->mbox->rx = c->rx;
		c->mbox->cookie = c->cookie;
	}
	spin_unlock_irqrestore(&c->mbox->rx_lock, flags);
}

/* 0 once the enclave has gated. A sleep refused after it was sent finds it told. */
static int sep_pm_commit_tell(void *data)
{
	struct sep_pm_commit *c = data;
	unsigned int us;
	int ret = 0;

	c->sent = false;
	c->answered = false;
	c->answer_us = 0;
	c->gate_ret = -EINVAL;

	sep_pm_commit_take_rx(c, true);
	/* What was already waiting first. */
	apple_mbox_poll(c->mbox);
	for (us = 0; !apple_mbox_send_ready(c->mbox); us += SEP_PM_POLL_US) {
		if (us >= SEP_PM_TIMEOUT_US) {
			ret = -ETIMEDOUT;
			goto out;
		}
		udelay(SEP_PM_POLL_US);
	}
	/* Its DART is saved, with macOS's writes, right before the request, as at .suspend. */
	ret = apple_dart_sleep_prepare(c->dart);
	if (ret)
		goto out;
	ret = apple_mbox_send(c->mbox, c->request, true);
	if (ret) {
		/* Not a real sleep: this only drops the saved copy. */
		apple_dart_sleep_restore(c->dart);
		goto out;
	}
	c->sent = true;
	/* Never read a gated enclave's mailbox: that stalls the CPU for good. */
	while (!c->answered && !(readl(c->ps) & SEP_PS_DEV_DISABLE) &&
	       c->answer_us < SEP_PM_TIMEOUT_US) {
		apple_mbox_poll(c->mbox);
		if (!c->answered) {
			udelay(SEP_PM_POLL_US);
			c->answer_us += SEP_PM_POLL_US;
		}
	}
out:
	sep_pm_commit_take_rx(c, false);
	if (!c->sent)
		return ret;
	if (c->answered)
		apple_mbox_set_recv_enabled(c->mbox, false);
	c->gate_ret = readl_poll_timeout_atomic(c->ps, c->state, c->state & SEP_PS_DEV_DISABLE,
						SEP_PM_POLL_US, SEP_PM_TIMEOUT_US);

	return c->gate_ret;
}

/*
 * From .suspend, with the enclave drained and its receive interrupt held: leaves the
 * request in @msg0 and @msg1 to the sleep driver's last refusal. Its answer is the message
 * whose @mask bits read @reply. -EOPNOTSUPP where there's no such step.
 */
int sep_pm_commit_prepare(void *ptr, void *mbox, u64 msg0, u32 msg1, u64 reply, u64 mask)
{
	struct sep_pm_commit *c = &sep_pm_commit;
	struct device *dev = ptr;
	struct device_node *np;
	struct resource res;
	u32 offset;
	int ret;

	if (!READ_ONCE(sep_pm_suspending))
		return -EOPNOTSUPP;
	if (c->registered)
		return -EBUSY;
	np = of_parse_phandle(dev->of_node, "power-domains", 0);
	if (!np)
		return -ENOENT;
	ret = of_property_read_u32(np, "reg", &offset);
	if (!ret)
		ret = np->parent ? of_address_to_resource(np->parent, 0, &res) : -EINVAL;
	of_node_put(np);
	if (ret)
		return ret;
	if (offset + 4 > resource_size(&res))
		return -EINVAL;

	c->dart = sep_pm_dart(dev);
	if (!c->dart || !dev_get_drvdata(c->dart)) {
		ret = -ENODEV;
		goto err;
	}
	c->ps = ioremap(res.start + offset, 4);
	if (!c->ps) {
		ret = -ENOMEM;
		goto err;
	}
	c->dev = dev;
	c->mbox = mbox;
	c->request.msg0 = msg0;
	c->request.msg1 = msg1;
	c->reply = reply;
	c->reply_mask = mask;
	c->sent = false;
	ret = apple_sleep_enclave_register(dev, sep_pm_commit_tell, c);
	if (ret)
		goto err;
	c->registered = true;

	return 0;

err:
	if (c->ps)
		iounmap(c->ps);
	c->ps = NULL;
	if (c->dart)
		put_device(c->dart);
	c->dart = NULL;
	return ret;
}

/* After the sleep, refused or not: 1 if the request went out, 0 if not, <0 if none was left. */
int sep_pm_commit_finish(void *ptr)
{
	struct sep_pm_commit *c = &sep_pm_commit;

	if (!c->registered || c->dev != ptr)
		return -ENOENT;
	apple_sleep_enclave_unregister(c->dev);
	c->registered = false;
	if (c->sent)
		dev_dbg(c->dev, "sleep request at the last refusal: %s in %u us, gate %d, state %#x\n",
			c->answered ? "answered" : "no answer", c->answer_us, c->gate_ret,
			c->state);
	iounmap(c->ps);
	c->ps = NULL;
	put_device(c->dart);
	c->dart = NULL;

	return c->sent;
}

/*
 * Only a firmware-driven suspend tells the enclave anything, so every other sleep
 * stays as it was. Refusing is safe: the PM core runs the resume callback of a
 * device whose suspend had passed when a later one failed, and sleep_enter undoes
 * what it did if its own step fails.
 */
static int __maybe_unused sep_pm_suspend(struct device *dev)
{
	int (*sleep)(void) = READ_ONCE(sep_pm_sleep);
	int ret;

	if (!pm_suspend_via_firmware() || !sleep)
		return 0;

	WRITE_ONCE(sep_pm_suspending, true);
	ret = sleep();
	WRITE_ONCE(sep_pm_suspending, false);

	return ret;
}

/* The last step before the platform's own: the domain has to be down by now. */
static int __maybe_unused sep_pm_suspend_noirq(struct device *dev)
{
	bool (*told)(void) = READ_ONCE(sep_pm_told);
	int ret;

	if (!pm_suspend_via_firmware() || !told || !told())
		return 0;

	ret = sep_pm_wait_gated(dev, 5000);
	if (ret == -ENOENT)
		return 0;
	if (ret) {
		dev_err(dev, "the enclave did not gate its power domain: %d\n", ret);
	} else {
		struct regmap *regmap;
		u32 offset, val = 0;

		if (!sep_pm_domain(dev, &regmap, &offset))
			regmap_read(regmap, offset, &val);
		dev_dbg(dev, "the enclave gated its power domain, state %#x\n", val);
	}

	return ret;
}

static int __maybe_unused sep_pm_resume(struct device *dev)
{
	void (*wake)(void) = READ_ONCE(sep_pm_wake);

	if (wake)
		wake();

	return 0;
}

const struct dev_pm_ops sep_pm_ops = {
	.suspend = pm_sleep_ptr(sep_pm_suspend),
	.resume = pm_sleep_ptr(sep_pm_resume),
	.suspend_noirq = pm_sleep_ptr(sep_pm_suspend_noirq),
};

int sep_pm_register(void *ptr, void (*event)(bool entering), int (*sleep)(void),
		    void (*wake)(void), bool (*told)(void))
{
	int ret;

	WRITE_ONCE(sep_pm_event, event);
	WRITE_ONCE(sep_pm_sleep, sleep);
	WRITE_ONCE(sep_pm_wake, wake);
	WRITE_ONCE(sep_pm_told, told);
	ret = register_pm_notifier(&sep_pm_nb);
	if (ret) {
		WRITE_ONCE(sep_pm_event, NULL);
		WRITE_ONCE(sep_pm_sleep, NULL);
		WRITE_ONCE(sep_pm_wake, NULL);
		WRITE_ONCE(sep_pm_told, NULL);
		return ret;
	}

	return 0;
}

/* Returns only once no notifier call is still running. */
void sep_pm_unregister(void)
{
	unregister_pm_notifier(&sep_pm_nb);
	WRITE_ONCE(sep_pm_event, NULL);
	WRITE_ONCE(sep_pm_sleep, NULL);
	WRITE_ONCE(sep_pm_wake, NULL);
	WRITE_ONCE(sep_pm_told, NULL);
}

static void sep_pm_release_domains(void *data)
{
	struct dev_pm_domain_list *list = data;
	u32 i;

	for (i = 0; i < list->num_pds; i++)
		dev_pm_syscore_device(list->pd_devs[i], false);
}

/*
 * The enclave runs from boot to shutdown, and on some SoCs it needs power
 * domains besides its own: once one of them is switched off, it stops
 * reading its mailbox for good. A node that lists more than one domain gets
 * all of them attached here, powered on through device links for as long as
 * the driver is bound, and taken out of system sleep, which would otherwise
 * switch a domain off in the noirq phase once its other users had suspended.
 * A node with a single domain is attached by the platform core as before.
 */
int sep_pm_keep_domains(void *ptr)
{
	struct device *dev = ptr;
	struct dev_pm_domain_attach_data data = {
		.pd_flags = PD_FLAG_DEV_LINK_ON,
	};
	struct dev_pm_domain_list *list;
	int count, ret;
	u32 i;

	count = of_count_phandle_with_args(dev->of_node, "power-domains",
					   "#power-domain-cells");
	if (count < 0 && count != -ENOENT)
		return count;
	if (count <= 1)
		return 0;

	ret = devm_pm_domain_attach_list(dev, &data, &list);
	if (ret < 0)
		return ret;

	for (i = 0; i < list->num_pds; i++)
		dev_pm_syscore_device(list->pd_devs[i], true);

	return devm_add_action_or_reset(dev, sep_pm_release_domains, list);
}
