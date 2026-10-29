// SPDX-License-Identifier: GPL-2.0

#include <kunit/visibility.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/iopoll.h>
#include <linux/of_device.h>
#include <linux/of_irq.h>
#include <linux/property.h>
#include <linux/regmap.h>
#include <linux/spmi.h>
#include <linux/suspend.h>
#include <linux/usb/role.h>

#include "tps6598x.h"

#define tps_to_sn(tps) container_of_const((tps), struct sn201202x, cd.tps)

/* A sleeping controller does not answer a select; give up on it sooner. */
#define SN201202X_ATOMIC_SELECT_MS	10

static int regmap_sn201202x_select_reg(struct spmi_device *sdev, u8 reg)
{
	int err;
	u8 val;
	bool warned = false;
	int attempts = 5;
	struct tps6598x *tps = spmi_device_get_drvdata(sdev);
	struct sn201202x *sn = tps_to_sn(tps);

	if (reg >= 0x80) {
		WARN_ON(1);
		return -EINVAL;
	}
	reinit_completion(&sn->select_completion);
	err = spmi_register_zero_write(sdev, reg);
	if (err)
		return err;

	if (sn->polled || sn->select_polled) {
		int poll_err;

		if (sn->atomic)
			poll_err = read_poll_timeout_atomic(spmi_register_read, err,
							    err || val == reg, 100,
							    SN201202X_ATOMIC_SELECT_MS * 1000,
							    false, sdev, 0, &val);
		else
			poll_err = read_poll_timeout(spmi_register_read, err,
						     err || val == reg, 100, 100000,
						     false, sdev, 0, &val);
		return err ?: poll_err;
	}

	if (!wait_for_completion_timeout(&sn->select_completion, msecs_to_jiffies(100)))
		return -ETIMEDOUT;

	while (--attempts) {
		err = spmi_register_read(sdev, 0, &val);
		if (err)
			return err;
		if (val == (reg | 0x80)) {
			if (!warned) {
				dev_warn(tps->dev,
					 "Got interrupt but selection not complete?\n");
				warned = true;
			}
			msleep(20);
			continue;
		}
		if (val == reg)
			return 0;
		return -EIO;
	}

	return -EIO;
}

static int regmap_sn201202x_read(void *context,
				 const void *reg, size_t reg_size,
				 void *val, size_t val_size)
{
	struct spmi_device *sdev = context;
	int err;
	unsigned int offset = 0x20;
	size_t len;
	u8 addr;

	if (reg_size != 1) {
		WARN_ON(1);
		return -EINVAL;
	}
	if (val_size > 0x40) {
		WARN_ON(1);
		return -EINVAL;
	}

	addr = *(u8 *)reg;

	err = regmap_sn201202x_select_reg(sdev, addr);
	if (err)
		return err;

	if (of_device_is_compatible(sdev->dev.of_node, "apple,t8132-sn201202x")) {
		u8 size;

		/* Selection completion does not establish the payload's length. */
		err = spmi_register_read(sdev, 0x1f, &size);
		if (err)
			return err;
		if (size < val_size || size > TPS_MAX_LEN) {
			dev_err_ratelimited(&sdev->dev,
					    "Register %#x reports %u bytes for a %zu-byte read\n",
					    addr, size, val_size);
			return -EIO;
		}
	}

	while (val_size) {
		len = min_t(size_t, val_size, 16);
		err = spmi_ext_register_read(context, offset, val, len);
		if (err)
			return err;
		offset += len;
		val += len;
		val_size -= len;
	}

	return 0;
}

static int regmap_sn201202x_write(void *context, const void *data,
				  size_t count)
{
	int err = 0;
	unsigned int offset = 0xa0;
	size_t len;
	u8 addr;

	if (count < 1) {
		WARN_ON(1);
		return -EINVAL;
	}

	addr = *(u8 *)data;
	data += 1;
	count -= 1;

	if (count > 0x40) {
		WARN_ON(1);
		return -EINVAL;
	}

	err = regmap_sn201202x_select_reg(context, addr);
	if (err)
		return err;

	while (count) {
		len = min_t(size_t, count, 16);
		err = spmi_ext_register_write(context, offset, data, len);
		if (err)
			return err;
		offset += len;
		data += len;
		count -= len;
	}

	return err;
}

static irqreturn_t sn201202x_irq(int irq, void *data)
{
	struct completion *c = data;

	complete(c);
	return IRQ_HANDLED;
}

static const struct regmap_bus regmap_sn201202x = {
	.read				= regmap_sn201202x_read,
	.write				= regmap_sn201202x_write,
	.reg_format_endian_default	= REGMAP_ENDIAN_NATIVE,
	.val_format_endian_default	= REGMAP_ENDIAN_NATIVE,
};

static struct regmap *__devm_regmap_init_sn201202x(struct spmi_device *sdev,
						   const struct regmap_config *config,
						   struct lock_class_key *lock_key,
						   const char *lock_name)
{
	return __devm_regmap_init(&sdev->dev, &regmap_sn201202x, sdev, config,
				  lock_key, lock_name);
}

#define devm_regmap_init_sn201202x(dev, config)				\
	__regmap_lockdep_wrapper(__devm_regmap_init_sn201202x, #config,	\
				dev, config)

static const struct of_device_id sn201202x_of_match[] = {
	{ .compatible = "apple,sn201202x", (void *)TIPD_VARIANT_SN201202X},
	{}
};

/*
 * In system sleep the wake interrupt is suspended, so the controller is
 * polled as without interrupts.
 */
static int sn201202x_wake_poll(struct sn201202x *sn, bool poll)
{
	/* Busy-waiting has no clock to go by: each step is charged instead */
	int budget_ms = 500;
	unsigned int tries = 0;
	unsigned long deadline;
	u8 size;
	int ret;

	reinit_completion(&sn->wake_completion);
	ret = spmi_command_wakeup(sn->sdev);
	if (ret)
		return ret;
	/* A WAKEUP sent while it was still going to sleep may go unanswered */
	if (!poll && wait_for_completion_timeout(&sn->wake_completion,
						 msecs_to_jiffies(100)))
		return 0;

	/* A sleeping ACE3 ACKs commands but returns zeros until it is awake. */
	deadline = jiffies + msecs_to_jiffies(500);
	do {
		if (++tries % 20 == 0) {
			ret = spmi_command_wakeup(sn->sdev);
			if (ret)
				return ret;
		}
		ret = regmap_sn201202x_select_reg(sn->sdev, TPS_REG_MODE);
		if (ret == -ETIMEDOUT) {
			budget_ms -= SN201202X_ATOMIC_SELECT_MS;
			continue;
		}
		if (ret)
			return ret;
		ret = spmi_register_read(sn->sdev, 0x1f, &size);
		if (ret)
			return ret;
		if (size)
			return 0;
		if (sn->atomic)
			mdelay(1);
		else
			usleep_range(1000, 2000);
		budget_ms--;
	} while (sn->atomic ? budget_ms > 0 : time_before(jiffies, deadline));

	return -ETIMEDOUT;
}

static int sn201202x_wake(struct sn201202x *sn)
{
	return sn201202x_wake_poll(sn, sn->polled);
}

/*
 * The links that wake the system when they appear in system sleep. A port
 * attaches as a USB device before any data role swap, as it does to a
 * charger, and a charger may still show a USB link for a moment after one:
 * a USB link counts only with this port as the host, and from a power source
 * only if the source is capable of USB communication.
 */
VISIBLE_IF_KUNIT u32 sn201202x_sleep_links(const struct cd321x_sleep_state *st)
{
	u32 links = st->data_status & (TPS_DATA_STATUS_DP_CONNECTION |
				       TPS_DATA_STATUS_TBT_CONNECTION |
				       CD321X_DATA_STATUS_USB4_CONNECTION);

	if (st->data_status & TPS_DATA_STATUS_USB_DATA_ROLE)
		return links;
	if (!(st->status & TPS_STATUS_PORTROLE) && !st->usb_comm)
		return links;
	return links | (st->data_status & (TPS_DATA_STATUS_USB2_CONNECTION |
					   TPS_DATA_STATUS_USB3_CONNECTION));
}
EXPORT_SYMBOL_IF_KUNIT(sn201202x_sleep_links);

/*
 * The event interrupt stays live in system sleep (IRQF_NO_SUSPEND). Wake the
 * controller to acknowledge the event, and wake the system only for a new
 * data connection, or if the controller cannot be read. It goes back to sleep
 * either way, as the resume expects.
 */
static bool sn201202x_sleep_event(struct tps6598x *tps)
{
	struct sn201202x *sn = tps_to_sn(tps);
	struct cd321x_sleep_state st;
	u32 links = 0;
	bool wake;
	int ret;

	guard(mutex)(&tps->lock);
	if (!READ_ONCE(sn->sleep_watch))
		return false;

	sn->atomic = true;
	ret = sn201202x_wake_poll(sn, true);
	if (!ret)
		ret = cd321x_sleep_events(tps, &st);
	if (!ret)
		links = sn201202x_sleep_links(&st);
	wake = ret || (links & ~sn->sleep_links);
	if (!ret)
		sn->sleep_links = links;
	spmi_command_sleep(sn->sdev);
	sn->atomic = false;
	if (wake)
		pm_wakeup_dev_event(tps->dev, 0, true);
	return true;
}

static int sn201202x_probe(struct spmi_device *device)
{
	const struct of_device_id *match;
	const struct tipd_data *data;
	struct usb_role_switch *sw;
	struct fwnode_handle *fwnode;
	struct sn201202x *sn;
	struct tps6598x *tps;
	int irq_select, irq_sleep, irq_wake;
	int ret;

	match = of_match_device(sn201202x_of_match, &device->dev);
	if (!match)
		return -EINVAL;
	data = tipd_hw_variants[(enum tipd_hw_variant)match->data];

	/*
	 * tipd_init() defers until the connector's USB role switch (the dwc3)
	 * is registered, but only after it has woken the chip, switched it to
	 * S0 and set its interrupt mask, and the deferral then puts the chip
	 * back to sleep. With both drivers loaded by udev at boot that cycle
	 * could repeat several times. Defer here instead, before the chip is
	 * touched. A connector without a role switch, or one whose remote node
	 * is disabled, gives NULL, and the probe continues as before.
	 */
	fwnode = device_get_named_child_node(&device->dev, "connector");
	sw = fwnode_usb_role_switch_get(fwnode);
	fwnode_handle_put(fwnode);
	if (IS_ERR(sw))
		return dev_err_probe(&device->dev, PTR_ERR(sw), "waiting for the USB role switch\n");
	usb_role_switch_put(sw);

	sn = devm_kzalloc(&device->dev, data->tps_struct_size, GFP_KERNEL);
	if (!sn)
		return -ENOMEM;
	sn->sdev = device;
	tps = &sn->cd.tps;

	mutex_init(&tps->lock);
	tps->dev = &device->dev;
	tps->data = data;

	init_completion(&sn->select_completion);
	init_completion(&sn->sleep_completion);
	init_completion(&sn->wake_completion);

	sn->polled = !of_property_present(device->dev.of_node, "interrupts") &&
		     !of_property_present(device->dev.of_node, "interrupts-extended");
	if (!sn->polled) {
		tps->irq = of_irq_get_byname(device->dev.of_node, "irq");
		if (tps->irq < 0)
			return tps->irq;
		irq_select = of_irq_get_byname(device->dev.of_node, "select");
		/* T8132 has no select notification, but retains event/sleep/wake IRQs. */
		if (irq_select == -EINVAL || irq_select == -ENODATA) {
			if (!of_device_is_compatible(device->dev.of_node,
						     "apple,t8132-sn201202x"))
				return irq_select;
			sn->select_polled = true;
		} else if (irq_select < 0) {
			return irq_select;
		}
		irq_sleep = of_irq_get_byname(device->dev.of_node, "sleep");
		if (irq_sleep < 0)
			return irq_sleep;
		irq_wake = of_irq_get_byname(device->dev.of_node, "wake");
		if (irq_wake < 0)
			return irq_wake;

		if (!sn->select_polled) {
			ret = devm_request_irq(&device->dev, irq_select, sn201202x_irq,
					       0, NULL, &sn->select_completion);
			if (ret)
				return ret;
		}
		ret = devm_request_irq(&device->dev, irq_sleep, sn201202x_irq,
				       0, NULL, &sn->sleep_completion);
		if (ret)
			return ret;
		ret = devm_request_irq(&device->dev, irq_wake, sn201202x_irq,
				       0, NULL, &sn->wake_completion);
		if (ret)
			return ret;
	}

	/*
	 * In system sleep only the event interrupt stays live. Register access
	 * then has to poll, which a select notification would not.
	 */
	if (!sn->polled && sn->select_polled)
		tps->sleep_event = sn201202x_sleep_event;

	spmi_device_set_drvdata(device, tps);
	tps->regmap = devm_regmap_init_sn201202x(device, &tps6598x_regmap_config);
	if (IS_ERR(tps->regmap))
		return PTR_ERR(tps->regmap);

	ret = sn201202x_wake(sn);
	if (ret)
		return ret;

	ret = tipd_init(tps);
	if (ret) {
		spmi_command_sleep(device);
		return ret;
	}

	/* Events can latch before the IRQ is registered. Consume that state now. */
	tps->data->irq_handler(tps->irq, tps);
	return 0;
}

static void sn201202x_remove(struct spmi_device *device)
{
	struct tps6598x *tps = spmi_device_get_drvdata(device);
	struct sn201202x *sn = tps_to_sn(tps);

	tipd_remove(tps);
	spmi_command_sleep(sn->sdev);
}

VISIBLE_IF_KUNIT int __maybe_unused sn201202x_resume(struct device *dev)
{
	struct tps6598x *tps = dev_get_drvdata(dev);
	struct sn201202x *sn = tps_to_sn(tps);
	int err;

	if (READ_ONCE(sn->sleep_watch)) {
		/* Waits for a running sleep_event() */
		disable_irq(tps->irq);
		WRITE_ONCE(sn->sleep_watch, false);
	}
	err = sn201202x_wake(sn);
	if (!err)
		err = tipd_resume(tps);
	/* The resume revalidation acknowledges events latched while asleep. */
	if (tps->irq)
		enable_irq(tps->irq);
	return err;
}
EXPORT_SYMBOL_IF_KUNIT(sn201202x_resume);

VISIBLE_IF_KUNIT int __maybe_unused sn201202x_suspend(struct device *dev)
{
	struct tps6598x *tps = dev_get_drvdata(dev);
	struct sn201202x *sn = tps_to_sn(tps);
	bool watch;
	int err, ret;

	/*
	 * A sleeping controller cannot be read, so its events must not be
	 * handled until it is awake again. An interrupt arriving meanwhile is
	 * replayed when it is enabled.
	 */
	if (tps->irq)
		disable_irq(tps->irq);
	err = tipd_suspend(tps);
	if (err)
		goto out_enable;
	watch = tps->sleep_event && device_may_wakeup(dev) &&
		pm_suspend_target_state == PM_SUSPEND_TO_IDLE;
	if (watch) {
		struct cd321x_sleep_state st;

		guard(mutex)(&tps->lock);
		/* Without a baseline, a plug-in in sleep cannot be told from a charger */
		watch = !cd321x_sleep_events(tps, &st);
		if (watch)
			sn->sleep_links = sn201202x_sleep_links(&st);
		else
			dev_warn(dev, "port state unreadable, no wakeup for an attach\n");
	}
	reinit_completion(&sn->sleep_completion);
	err = spmi_command_sleep(sn->sdev);
	if (err)
		goto out_resume;
	if (!sn->polled &&
	    !wait_for_completion_timeout(&sn->sleep_completion, msecs_to_jiffies(100))) {
		err = -ETIMEDOUT;
		goto out_wake;
	}
	if (watch) {
		WRITE_ONCE(sn->sleep_watch, true);
		enable_irq(tps->irq);
	}
	return 0;

out_wake:
	/* It accepted SLEEP, so it may be asleep without having confirmed it. */
	ret = sn201202x_wake(sn);
	if (ret)
		dev_err(dev, "failed to wake up after failed suspend: %d\n", ret);
out_resume:
	tipd_resume(tps);
out_enable:
	if (tps->irq)
		enable_irq(tps->irq);
	return err;
}
EXPORT_SYMBOL_IF_KUNIT(sn201202x_suspend);

MODULE_DEVICE_TABLE(of, sn201202x_of_match);

static const struct dev_pm_ops sn201202x_pm_ops = {
	.prepare = pm_sleep_ptr(tipd_prepare),
	.complete = pm_sleep_ptr(tipd_complete),
	SET_SYSTEM_SLEEP_PM_OPS(sn201202x_suspend, sn201202x_resume)
};

static struct spmi_driver sn201202x_driver = {
	.driver = {
		.name = "sn201202x",
		.pm = &sn201202x_pm_ops,
		.of_match_table = sn201202x_of_match,
	},
	.probe = sn201202x_probe,
	.remove = sn201202x_remove,
};
module_spmi_driver(sn201202x_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("TI SN201202x USB Power Delivery Controller Driver");
