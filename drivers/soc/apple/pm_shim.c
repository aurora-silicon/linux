// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * System sleep for the SEP driver: the notifier forwards the sleep events to
 * Rust, which ends any Touch ID capture before the system suspends (see
 * pm.rs); after a resume from disk on a cold boot it probes the device again;
 * and the enclave's power domains are kept on across sleep. The device's
 * suspend and resume callbacks are the firmware suspend's: the enclave is
 * told, and gates its own domain, whose state word is read here. On T8140 at
 * EL2 the sleep driver sends the request at the irreversible commit boundary.
 * Only errors before send may refuse.
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
#include <linux/panic.h>
#include <linux/pm_domain.h>
#include <linux/regmap.h>
#include <linux/soc/apple/dart.h>
#include <linux/soc/apple/mailbox.h>
#include <linux/soc/apple/pmp.h>
#include <linux/soc/apple/sleep.h>
#include <linux/spinlock.h>
#include <linux/suspend.h>
#include <linux/workqueue.h>

#include <asm/virt.h>

#include "shim.h"

/* The state word of a PMGR power domain, as pmgr-pwrstate has it. */
#define SEP_PS_DEV_DISABLE	BIT(10)
#define SEP_PS_ACTUAL		GENMASK(7, 4)

/* One-shot precommit test: no SLEEP request or commit coprocessor stop. */
static bool sleep_refuse_before_send;
module_param(sleep_refuse_before_send, bool, 0644);
MODULE_PARM_DESC(sleep_refuse_before_send,
		 "Test only: refuse the next prepared deep sleep before sending SEP SLEEP");

static int (*sep_pm_event)(int event);
static struct device *sep_pm_dev;
static int (*sep_pm_sleep)(void);
static void (*sep_pm_wake)(void);
static bool (*sep_pm_told)(void);
/* In the device's .suspend, the only caller that can leave the request to the sleep. */
static bool sep_pm_suspending;

/*
 * Whether the boot loader restarted the SoC under this hibernation. iBoot
 * boots sepOS anew on every boot, and the SEP has no run bit the AP can read,
 * so the PMP's restore (priority 10, ahead of this notifier) is asked: it
 * counts the resumes from disk that found the PMP halted. The count is taken
 * at registration and again at each hibernation's prepare and end, so a
 * prepare this notifier missed (registered during it) still compares with
 * the last count. Negative: no bound PMP, cold or warm cannot be told.
 */
static int sep_pm_cold_base = -ENODEV;

/* The marker dt.rs leaves on the SEP node once the registration is sent. */
#define SEP_REGISTERED_PROP "apple,sep-shmem-registered-iova"

/*
 * A resume from disk on a cold boot: the enclave runs a freshly booted sepOS
 * that has never seen this kernel's registration, so the restored session is
 * dead. The device is probed again from a work item, as at boot: probe sends
 * a new registration (the marker is gone with the session it guarded), opens
 * the xART store and loads the host state from disk again (the boot that
 * resumed this image may have changed both), and recreates /dev/sep-bio.
 */
static struct device *sep_reprobe_dev;
static DEFINE_SPINLOCK(sep_reprobe_lock);

static void sep_reprobe_fn(struct work_struct *work)
{
	struct device *dev;
	int ret;

	spin_lock(&sep_reprobe_lock);
	dev = sep_reprobe_dev;
	sep_reprobe_dev = NULL;
	spin_unlock(&sep_reprobe_lock);
	if (!dev)
		return;

	dev_info(dev, "resume from disk: attaching the restarted enclave again\n");
	ret = device_reprobe(dev);
	/* A failed probe is not reported by device_reprobe() itself. */
	if (ret || !READ_ONCE(dev->driver))
		dev_err(dev, "resume from disk: probing the SEP again failed (%d); Touch ID stays down until a reboot\n",
			ret);
	put_device(dev);
}

static DECLARE_WORK(sep_reprobe_work, sep_reprobe_fn);

static void sep_pm_schedule_reprobe(struct device *dev)
{
	struct device_node *np = dev->of_node;
	struct property *prop;

	/*
	 * Only here, with the cold boot established: removing the marker on a
	 * live enclave would let a second registration park it.
	 */
	prop = np ? of_find_property(np, SEP_REGISTERED_PROP, NULL) : NULL;
	if (prop && of_remove_property(np, prop)) {
		dev_err(dev, "resume from disk: the registration marker could not be removed; Touch ID stays down until a reboot\n");
		return;
	}

	spin_lock(&sep_reprobe_lock);
	if (!sep_reprobe_dev) {
		sep_reprobe_dev = get_device(dev);
		queue_work(system_dfl_wq, &sep_reprobe_work);
	}
	spin_unlock(&sep_reprobe_lock);
}

/* The module goes away: no probe may run from the work item after it. */
void sep_pm_exit(void)
{
	struct device *dev;

	cancel_work_sync(&sep_reprobe_work);
	spin_lock(&sep_reprobe_lock);
	dev = sep_reprobe_dev;
	sep_reprobe_dev = NULL;
	spin_unlock(&sep_reprobe_lock);
	if (dev)
		put_device(dev);
}

static int sep_pm_notify(struct notifier_block *nb, unsigned long action,
			 void *data)
{
	int (*event)(int event) = READ_ONCE(sep_pm_event);
	struct device *dev = READ_ONCE(sep_pm_dev);
	int base, now;

	if (!event)
		return NOTIFY_DONE;

	switch (action) {
	case PM_SUSPEND_PREPARE:
		sep_diag_sleep(true);
		event(SEP_PM_SLEEP);
		return NOTIFY_OK;
	case PM_POST_SUSPEND:
		event(SEP_PM_WAKE);
		sep_diag_sleep(false);
		return NOTIFY_OK;
	case PM_HIBERNATION_PREPARE:
		sep_diag_sleep(true);
		sep_pm_cold_base = apple_pmp_cold_restarts();
		event(SEP_PM_HIBERNATE);
		return NOTIFY_OK;
	case PM_RESTORE_PREPARE:
		sep_diag_sleep(true);
		/*
		 * This kernel is about to load an image. If it registered with the
		 * enclave, the enclave keeps that session and may DMA into memory
		 * the image overwrites, and the restored kernel would register a
		 * second time: refuse the restore (this boot goes on instead).
		 */
		if (event(SEP_PM_RESTORE) > 0) {
			pr_err("apple_sep: this kernel attached the SEP; refusing to restore a hibernation image over it\n");
			return NOTIFY_BAD;
		}
		return NOTIFY_OK;
	case PM_POST_RESTORE:
		event(SEP_PM_HIBERNATE_WARM);
		sep_diag_sleep(false);
		return NOTIFY_OK;
	case PM_POST_HIBERNATION:
		sep_diag_sleep(false);
		/*
		 * Ends an aborted hibernation, the wake of a hybrid sleep and a
		 * resume from the image alike. Only a resume from the image can
		 * have gone through a boot, and only the PMP knows whether it did
		 * (a test resume does not): a hybrid sleep's wake is never taken
		 * for a cold boot, even when the PMP did not come back from its
		 * suspend-to-RAM and the PMP's restore finds it halted.
		 */
		base = sep_pm_cold_base;
		now = apple_pmp_cold_restarts();
		sep_pm_cold_base = now;
		if (!pm_hibernate_is_restored()) {
			event(SEP_PM_HIBERNATE_WARM);
			return NOTIFY_OK;
		}
		if (base < 0 || now < 0) {
			/*
			 * Cold or warm cannot be told. Sending to a freshly booted
			 * enclave, or registering again with a live one, are both
			 * worse than losing Touch ID until a reboot.
			 */
			if (dev)
				dev_err(dev, "hibernation: no PMP count, cannot tell a cold boot; the SEP session is dropped until a reboot\n");
			event(SEP_PM_HIBERNATE_COLD);
			return NOTIFY_OK;
		}
		if (now != base) {
			if (event(SEP_PM_HIBERNATE_COLD) > 0 && dev)
				sep_pm_schedule_reprobe(dev);
			return NOTIFY_OK;
		}
		event(SEP_PM_HIBERNATE_WARM);
		return NOTIFY_OK;
	default:
		return NOTIFY_DONE;
	}
}

static struct notifier_block sep_pm_nb = {
	.notifier_call = sep_pm_notify,
	/* Below the PMP's 10, so that its restore has counted a cold boot. */
	.priority = 0,
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
 * Returns 0 once the enclave has gated itself, 1 if the deadline expired, or a
 * negative observation error (including -ENOENT if there is no domain to read).
 * Keep a read's -ETIMEDOUT distinct from the polling deadline. DEV_DISABLE is
 * the evidence: a running enclave idles at actual state 0 with auto-enable on.
 */
int sep_pm_wait_gated(void *ptr, unsigned int timeout_ms)
{
	struct regmap *regmap;
	u32 offset, val;
	int ret, poll_ret;

	ret = sep_pm_domain(ptr, &regmap, &offset);
	if (ret)
		return ret;

	poll_ret = read_poll_timeout(regmap_read, ret, ret || (val & SEP_PS_DEV_DISABLE),
				     1000, timeout_ms * USEC_PER_MSEC, false,
				     regmap, offset, &val);
	if (ret)
		return ret;

	return !!poll_ret;
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
 * .suspend only stages this request and holds RX. The platform sends it after
 * the last legitimate refusal. Once sent, only physical sleep/wake is supported;
 * a failed/aborted SLEEP is fatal as on the native path, not an in-place restart.
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
	bool registered, sent, answered, gate_valid;
	u8 reply_result;
	unsigned int answer_us;
	int gate_ret;
	u32 state;
} sep_pm_commit;

/* The answer stops here. Anything else goes to the driver, which reads it after the wake. */
static void sep_pm_commit_rx(struct apple_mbox *mbox, struct apple_mbox_msg msg, void *cookie)
{
	struct sep_pm_commit *c = cookie;

	if (c->sent && (msg.msg0 & c->reply_mask) == c->reply) {
		/* Control reply result is byte3; the match mask is below it. */
		c->reply_result = (msg.msg0 >> 24) & 0xff;
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

/* Nonzero returns are permitted only before the request was sent. */
static int sep_pm_commit_stop(void *data)
{
	struct sep_pm_commit *c = data;
	unsigned int us;
	bool prepared = false;
	int ret = 0;

	c->sent = false;
	c->answered = false;
	c->answer_us = 0;
	c->reply_result = 0xff;
	c->gate_ret = -EINVAL;
	c->gate_valid = false;
	c->state = 0;

	sep_pm_commit_take_rx(c, true);
	/* No SLEEP sent yet: drain what was already waiting. */
	ret = apple_mbox_poll(c->mbox);
	if (ret < 0)
		goto out;
	for (us = 0; !apple_mbox_send_ready(c->mbox); us += SEP_PM_POLL_US) {
		if (us >= SEP_PM_TIMEOUT_US) {
			ret = -ETIMEDOUT;
			goto out;
		}
		udelay(SEP_PM_POLL_US);
	}
	ret = apple_dart_sleep_prepare(c->dart);
	if (ret)
		goto out;
	prepared = true;
	/* The PM core's earlier check precedes the potentially long preparation. */
	if (pm_wakeup_pending()) {
		ret = -EBUSY;
		goto out;
	}
	if (xchg(&sleep_refuse_before_send, false)) {
		dev_info(c->dev, "test: precommit refusal before SEP SLEEP; request not sent\n");
		ret = -ECANCELED;
		goto out;
	}
	/* Commit decision: a new wake from here must finish physical sleep/wake. */
	ret = apple_mbox_send(c->mbox, c->request, true);
	if (ret)
		goto out;
	c->sent = true;
	while (!c->answered && !((c->state = readl(c->ps)) & SEP_PS_DEV_DISABLE) &&
	       c->answer_us < SEP_PM_TIMEOUT_US) {
		ret = apple_mbox_poll_one(c->mbox);
		if (ret < 0)
			break;
		if (!c->answered) {
			udelay(SEP_PM_POLL_US);
			c->answer_us += SEP_PM_POLL_US;
		}
	}
out:
	/* Callback ownership is RAM only; RX remains held until a real wake. */
	sep_pm_commit_take_rx(c, false);
	if (!c->sent) {
		if (prepared)
			apple_dart_sleep_restore(c->dart);
		return ret;
	}
	/* Preserve the failure reason without another hardware query. */
	if (ret >= 0 && !c->answered)
		ret = c->state & SEP_PS_DEV_DISABLE ? -EHOSTDOWN : -ETIMEDOUT;
	if (c->answered && c->reply_result)
		ret = -EREMOTEIO;
	/* Native cmsgSLEEP errors/abort/timeout are fatal, never cancellation. */
	if (ret < 0)
		panic("SEP SLEEP failed after send: ret %d reply_valid %d result %#x state %#x",
		      ret, c->answered, c->reply_result, c->state);
	/*
	 * Native success-only receive close. No mailbox access follows it. The
	 * held RX IRQ adapts native workloop exclusion; access-until-close is
	 * the native contract, not proved atomic by the preceding PMGR sample.
	 */
	apple_mbox_set_recv_enabled(c->mbox, false);
	c->gate_ret = readl_poll_timeout_atomic(c->ps, c->state, c->state & SEP_PS_DEV_DISABLE,
						SEP_PM_POLL_US, SEP_PM_TIMEOUT_US);
	c->gate_valid = true;
	if (c->gate_ret)
		panic("SEP SLEEP did not gate after send: %d state %#x", c->gate_ret, c->state);

	return 0;
}

/*
 * From .suspend, with the enclave drained and its receive interrupt held: leaves the
 * request in @msg0 and @msg1 to the irreversible commit. Its answer is the message
 * whose @mask bits read @reply. Unsupported paths must refuse without sending.
 */
int sep_pm_commit_prepare(void *ptr, void *mbox, u64 msg0, u32 msg1, u64 reply, u64 mask)
{
	struct sep_pm_commit *c = &sep_pm_commit;
	struct device *dev = ptr;
	struct device_node *np;
	struct resource res;
	u32 offset;
	int ret;

	/* Check capability before resources that legacy providers need not have. */
	if (!READ_ONCE(sep_pm_suspending) || !IS_ENABLED(CONFIG_APPLE_SOC_SLEEP) ||
	    !is_kernel_in_hyp_mode())
		return -EOPNOTSUPP;
	if (c->registered)
		return -EBUSY;
	/* Reject ACK/counting/PSD transports before any request or MMIO. */
	if (!apple_mbox_poll_one_supported(mbox))
		return -EOPNOTSUPP;
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
	/* Resume may follow a pre-tell abort: observations belong to this attempt. */
	c->answered = false;
	c->answer_us = 0;
	c->reply_result = 0xff;
	c->gate_ret = -EINVAL;
	c->gate_valid = false;
	c->state = 0;
	ret = apple_sleep_enclave_register(dev, sep_pm_commit_stop, c);
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
	if (c->sent && !pm_resume_via_firmware())
		panic("SEP SLEEP was sent without a physical sleep/wake");
	apple_sleep_enclave_unregister(c->dev);
	c->registered = false;
	dev_dbg(c->dev,
		"sleep commit finished: sent=%d reply_valid=%d result=%#x answer_us=%u gate_valid=%d gate_ret=%d state=%#x\n",
		c->sent, c->answered, c->reply_result, c->answer_us,
		c->gate_valid, c->gate_ret, c->state);
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
 * what it did if its own step fails. No SLEEP is sent during device suspend.
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
	if (ret > 0)
		ret = -ETIMEDOUT;
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

int sep_pm_register(void *ptr, int (*event)(int event), int (*sleep)(void),
		    void (*wake)(void), bool (*told)(void))
{
	int ret;

	WRITE_ONCE(sep_pm_dev, (struct device *)ptr);
	WRITE_ONCE(sep_pm_event, event);
	WRITE_ONCE(sep_pm_sleep, sleep);
	WRITE_ONCE(sep_pm_wake, wake);
	WRITE_ONCE(sep_pm_told, told);
	sep_pm_cold_base = apple_pmp_cold_restarts();
	ret = register_pm_notifier(&sep_pm_nb);
	if (ret) {
		WRITE_ONCE(sep_pm_event, NULL);
		WRITE_ONCE(sep_pm_sleep, NULL);
		WRITE_ONCE(sep_pm_wake, NULL);
		WRITE_ONCE(sep_pm_told, NULL);
		WRITE_ONCE(sep_pm_dev, NULL);
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
	WRITE_ONCE(sep_pm_dev, NULL);
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
