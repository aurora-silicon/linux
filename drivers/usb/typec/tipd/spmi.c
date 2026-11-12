// SPDX-License-Identifier: GPL-2.0

#include <kunit/visibility.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/iopoll.h>
#include <linux/of_device.h>
#include <linux/of_irq.h>
#include <linux/pm_runtime.h>
#include <linux/property.h>
#include <linux/regmap.h>
#include <linux/spmi.h>
#include <linux/suspend.h>
#include <linux/usb/role.h>

#include "tps6598x.h"

#define tps_to_sn(tps) container_of_const((tps), struct sn201202x, cd.tps)

/* A sleeping controller does not answer a select; give up on it sooner. */
#define SN201202X_ATOMIC_SELECT_MS	10

/* An idle controller sleeps after this long without register access. */
#define SN201202X_AUTOSUSPEND_MS	1000
/* Failed tries to sleep before it waits for the next access */
#define SN201202X_SLEEP_TRIES		3

#define SN201202X_REG_LDCM_STATUS	0x74
#define SN201202X_LDCM_MITIGATE		BIT(6)
#define SN201202X_LDCM_DETECTED		BIT(2)
#define SN201202X_LDCM_STATE		GENMASK(3, 1)
#define SN201202X_LDCM_STATE_IDLE	0
#define SN201202X_LDCM_RESULT		GENMASK(5, 4)
#define SN201202X_LDCM_RETRY_MS		500

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

static int __regmap_sn201202x_read(void *context,
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

static int __regmap_sn201202x_write(void *context, const void *data,
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

/*
 * A register access wakes a controller that sleeps idle, and lets it sleep
 * again SN201202X_AUTOSUSPEND_MS after the last one, with its tries to sleep
 * counted afresh. In system sleep the event path wakes it and puts it back
 * to sleep itself.
 */
static int sn201202x_access_get(struct tps6598x *tps, bool *rpm)
{
	int err;

	*rpm = !READ_ONCE(tps_to_sn(tps)->atomic);
	if (!*rpm)
		return 0;
	err = pm_runtime_resume_and_get(tps->dev);
	if (err)
		return err;
	/* Held awake, so no runtime suspend runs meanwhile */
	tps_to_sn(tps)->sleep_tries = 0;
	return 0;
}

static void sn201202x_access_put(struct tps6598x *tps, bool rpm)
{
	if (rpm)
		pm_runtime_put_autosuspend(tps->dev);
}

static int regmap_sn201202x_read(void *context,
				 const void *reg, size_t reg_size,
				 void *val, size_t val_size)
{
	struct tps6598x *tps = spmi_device_get_drvdata(context);
	bool rpm;
	int err;

	err = sn201202x_access_get(tps, &rpm);
	if (err)
		return err;
	err = __regmap_sn201202x_read(context, reg, reg_size, val, val_size);
	sn201202x_access_put(tps, rpm);
	return err;
}

static int regmap_sn201202x_write(void *context, const void *data,
				  size_t count)
{
	struct tps6598x *tps = spmi_device_get_drvdata(context);
	bool rpm;
	int err;

	err = sn201202x_access_get(tps, &rpm);
	if (err)
		return err;
	err = __regmap_sn201202x_write(context, data, count);
	sn201202x_access_put(tps, rpm);
	return err;
}

static irqreturn_t sn201202x_irq(int irq, void *data)
{
	struct completion *c = data;

	complete(c);
	return IRQ_HANDLED;
}

VISIBLE_IF_KUNIT const struct regmap_bus regmap_sn201202x = {
	.read				= regmap_sn201202x_read,
	.write				= regmap_sn201202x_write,
	.reg_format_endian_default	= REGMAP_ENDIAN_NATIVE,
	.val_format_endian_default	= REGMAP_ENDIAN_NATIVE,
};
EXPORT_SYMBOL_IF_KUNIT(regmap_sn201202x);

VISIBLE_IF_KUNIT enum typec_contaminant
sn201202x_ldcm_decode(const u8 status[SN201202X_LDCM_STATUS_LEN])
{
	if (FIELD_GET(SN201202X_LDCM_STATE, status[1]) !=
	    SN201202X_LDCM_STATE_IDLE)
		return TYPEC_CONTAMINANT_UNKNOWN;

	if ((status[0] & SN201202X_LDCM_DETECTED) &&
	    (status[1] & SN201202X_LDCM_RESULT))
		return TYPEC_CONTAMINANT_LIQUID;

	return TYPEC_CONTAMINANT_NONE;
}
EXPORT_SYMBOL_IF_KUNIT(sn201202x_ldcm_decode);

static int sn201202x_ldcm_read(void *ctx,
			      enum typec_contaminant *contaminant)
{
	struct tps6598x *tps = ctx;
	u8 status[SN201202X_LDCM_STATUS_LEN];
	int ret;

	ret = tps6598x_block_read(tps, SN201202X_REG_LDCM_STATUS, status,
				  sizeof(status));
	*contaminant = ret ? TYPEC_CONTAMINANT_UNKNOWN :
			     sn201202x_ldcm_decode(status);
	return ret;
}

static int sn201202x_ldcm_set_mitigation(void *ctx, bool enable)
{
	struct tps6598x *tps = ctx;
	struct sn201202x *sn = tps_to_sn(tps);
	u8 command = enable ? SN201202X_LDCM_MITIGATE : 0;
	int ret;

	/*
	 * The controller keeps its mitigation until told otherwise or reset.
	 * Repeating the current state would only disturb the port, so send a
	 * command only for a change or after a reset or failure.
	 */
	if (sn->ldcm_mitigation_known && sn->ldcm_mitigated == enable)
		return 0;

	/*
	 * As macOS, which reads a dry port's status but sends it no LDCM
	 * command: only a mitigation this driver requested since the
	 * controller's last reset needs removing.
	 */
	if (!enable && !sn->ldcm_requested) {
		sn->ldcm_mitigation_known = true;
		sn->ldcm_mitigated = false;
		return 0;
	}
	if (enable)
		sn->ldcm_requested = true;

	/*
	 * Successful task completion is the controller's mitigation ACK.
	 * Startup commands can take more than a second on SN201202x. Wait for
	 * completion before a retry can request the opposite mitigation state.
	 */
	ret = tps6598x_exec_cmd_tmo(tps, "LDCM", sizeof(command), &command,
				   0, NULL, 5000, 0);
	sn->ldcm_mitigation_known = !ret;
	if (!ret)
		sn->ldcm_mitigated = enable;
	if (!ret && !enable)
		sn->ldcm_requested = false;
	return ret;
}

static void sn201202x_ldcm_publish(void *ctx,
				   enum typec_contaminant contaminant)
{
	struct tps6598x *tps = ctx;

	typec_set_contaminant(tps->port, contaminant);
	tps->contaminant = contaminant;
}

VISIBLE_IF_KUNIT int
sn201202x_ldcm_update_state(void *ctx, const struct sn201202x_ldcm_ops *ops,
			    enum typec_contaminant last)
{
	enum typec_contaminant contaminant;
	int read_ret, ret;

	read_ret = ops->read(ctx, &contaminant);
	/*
	 * A measurement in progress is not a result. Keep the last result and
	 * its mitigation; the controller reports the new one when it is done.
	 */
	if (!read_ret && contaminant == TYPEC_CONTAMINANT_UNKNOWN &&
	    last != TYPEC_CONTAMINANT_UNKNOWN)
		return -EINPROGRESS;
	if (read_ret || contaminant != TYPEC_CONTAMINANT_NONE) {
		ret = ops->set_mitigation(ctx, true);
		if (ret) {
			ops->publish(ctx, TYPEC_CONTAMINANT_UNKNOWN);
			return ret;
		}

		ops->publish(ctx, contaminant);
		if (read_ret)
			return read_ret;
		return contaminant == TYPEC_CONTAMINANT_UNKNOWN ? -EAGAIN : 0;
	}

	ret = ops->set_mitigation(ctx, false);
	if (ret) {
		ops->publish(ctx, TYPEC_CONTAMINANT_UNKNOWN);
		return ret;
	}

	/* Do not report dry if liquid appeared while mitigation was removed. */
	read_ret = ops->read(ctx, &contaminant);
	if (!read_ret && contaminant == TYPEC_CONTAMINANT_UNKNOWN) {
		/*
		 * Removing mitigation can start another measurement. The dry
		 * snapshot above is still the latest result; preserve it just
		 * as we preserve a previously published result at entry. Forcing
		 * mitigation back on here can prevent the measurement finishing.
		 */
		ops->publish(ctx, TYPEC_CONTAMINANT_NONE);
		return -EINPROGRESS;
	}
	if (read_ret || contaminant != TYPEC_CONTAMINANT_NONE) {
		ret = ops->set_mitigation(ctx, true);
		if (ret) {
			ops->publish(ctx, TYPEC_CONTAMINANT_UNKNOWN);
			return ret;
		}
	}

	ops->publish(ctx, contaminant);
	if (read_ret)
		return read_ret;
	return contaminant == TYPEC_CONTAMINANT_UNKNOWN ? -EAGAIN : 0;
}
EXPORT_SYMBOL_IF_KUNIT(sn201202x_ldcm_update_state);

static const struct sn201202x_ldcm_ops sn201202x_ldcm_ops = {
	.read = sn201202x_ldcm_read,
	.set_mitigation = sn201202x_ldcm_set_mitigation,
	.publish = sn201202x_ldcm_publish,
};

static int sn201202x_ldcm_update(struct tps6598x *tps)
{
	if (!tps->contaminant_ops)
		return 0;

	return sn201202x_ldcm_update_state(tps, &sn201202x_ldcm_ops,
					   tps->contaminant);
}

/* Measuring, or mitigated without a result yet: wait for the controller. */
static bool sn201202x_ldcm_pending(int ret)
{
	return ret == -EINPROGRESS || ret == -EAGAIN;
}

static void sn201202x_ldcm_event(struct tps6598x *tps)
{
	struct sn201202x *sn = tps_to_sn(tps);

	mod_delayed_work(system_freezable_wq, &sn->ldcm_work, 0);
}

static void sn201202x_ldcm_retry(struct tps6598x *tps, int ret)
{
	struct sn201202x *sn = tps_to_sn(tps);

	if (!sn201202x_ldcm_pending(ret))
		dev_err_ratelimited(tps->dev,
				    "liquid-detection status unavailable: %d\n", ret);
	mod_delayed_work(system_freezable_wq, &sn->ldcm_work,
			 msecs_to_jiffies(SN201202X_LDCM_RETRY_MS));
}

static void sn201202x_ldcm_work(struct work_struct *work)
{
	struct sn201202x *sn = container_of(to_delayed_work(work),
					  struct sn201202x, ldcm_work);
	struct tps6598x *tps = &sn->cd.tps;
	enum typec_contaminant last;
	int ret;

	mutex_lock(&tps->lock);
	last = tps->contaminant;
	ret = sn201202x_ldcm_update(tps);
	/* Apply the cable state again only if the published state changed. */
	if (tps->contaminant != last)
		mod_delayed_work(system_freezable_wq, &sn->cd.resume_work, 0);
	mutex_unlock(&tps->lock);

	if (ret)
		sn201202x_ldcm_retry(tps, ret);
}

/*
 * Refresh the snapshot in the caller's context, before it applies the cable
 * state, so that a port is never dropped for a stale "unknown". Context:
 * tps->lock held.
 */
static void sn201202x_ldcm_refresh(struct tps6598x *tps)
{
	int ret;

	lockdep_assert_held(&tps->lock);
	ret = sn201202x_ldcm_update(tps);
	if (ret)
		sn201202x_ldcm_retry(tps, ret);
}

/* The controller was reset, which also removed its mitigation. */
static void sn201202x_ldcm_invalidate(struct tps6598x *tps)
{
	struct sn201202x *sn = tps_to_sn(tps);

	tps->contaminant = TYPEC_CONTAMINANT_UNKNOWN;
	sn->ldcm_mitigated = false;
	sn->ldcm_mitigation_known = false;
	sn->ldcm_requested = false;
	sn201202x_ldcm_event(tps);
}

static void sn201202x_ldcm_stop(struct tps6598x *tps)
{
	/* Reset recovery is drained later and may still try to queue an update. */
	disable_delayed_work_sync(&tps_to_sn(tps)->ldcm_work);
}

VISIBLE_IF_KUNIT const struct tipd_contaminant_ops sn201202x_contaminant_ops = {
	.update = sn201202x_ldcm_update,
	.refresh = sn201202x_ldcm_refresh,
	.event = sn201202x_ldcm_event,
	.invalidate = sn201202x_ldcm_invalidate,
	.stop = sn201202x_ldcm_stop,
};
EXPORT_SYMBOL_IF_KUNIT(sn201202x_contaminant_ops);

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

VISIBLE_IF_KUNIT int __maybe_unused sn201202x_runtime_suspend(struct device *dev)
{
	struct tps6598x *tps = dev_get_drvdata(dev);
	struct sn201202x *sn = tps_to_sn(tps);

	/*
	 * A display, Thunderbolt or USB4 link keeps it awake: its events are
	 * untested with the controller asleep. The next event, such as the
	 * unplug, lets it sleep again. The data status is not read again
	 * without a plug, so the plug status says when the link is gone.
	 */
	if ((READ_ONCE(tps->status) & TPS_STATUS_PLUG_PRESENT) &&
	    (READ_ONCE(tps->data_status) & (TPS_DATA_STATUS_DP_CONNECTION |
					    TPS_DATA_STATUS_TBT_CONNECTION |
					    CD321X_DATA_STATUS_USB4_CONNECTION)))
		return -EBUSY;
	/* So does Apple's "DBMa" debug mode, for the routes it carries */
	if (tps->dbma_at_probe || tipd_debugfs_dbma_held(tps))
		return -EBUSY;

	reinit_completion(&sn->sleep_completion);
	if (spmi_command_sleep(sn->sdev))
		goto retry;
	if (!sn->polled &&
	    !wait_for_completion_timeout(&sn->sleep_completion, msecs_to_jiffies(100))) {
		/*
		 * It accepted SLEEP, so it may be asleep without having
		 * confirmed it. Woken, it tries again later; if it does not
		 * wake, it is taken as asleep, and the next access wakes it.
		 */
		if (sn201202x_wake(sn))
			goto asleep;
		goto retry;
	}
asleep:
	sn->sleep_tries = 0;
	return 0;

retry:
	/*
	 * With a new expiry, runtime PM tries again after the delay. Without
	 * a delay it does not, and the controller waits for the next access.
	 */
	if (++sn->sleep_tries < SN201202X_SLEEP_TRIES) {
		pm_runtime_mark_last_busy(dev);
		return -EAGAIN;
	}
	sn->sleep_tries = 0;
	dev_warn_once(dev, "cannot sleep while idle, awake until the next access\n");
	return -EBUSY;
}
EXPORT_SYMBOL_IF_KUNIT(sn201202x_runtime_suspend);

VISIBLE_IF_KUNIT int __maybe_unused sn201202x_runtime_resume(struct device *dev)
{
	struct tps6598x *tps = dev_get_drvdata(dev);

	return sn201202x_wake(tps_to_sn(tps));
}
EXPORT_SYMBOL_IF_KUNIT(sn201202x_runtime_resume);

/*
 * The SPMI bus holds a runtime PM reference while the driver is bound.
 * Probe gives it to idle sleep only where the event interrupt stays live
 * while the controller sleeps, as the system sleep event filter has it;
 * elsewhere the controller stays awake.
 */
VISIBLE_IF_KUNIT void sn201202x_idle_sleep_start(struct tps6598x *tps)
{
	if (!tps->sleep_event)
		return;
	pm_runtime_set_autosuspend_delay(tps->dev, SN201202X_AUTOSUSPEND_MS);
	pm_runtime_use_autosuspend(tps->dev);
	pm_runtime_put_autosuspend(tps->dev);
}
EXPORT_SYMBOL_IF_KUNIT(sn201202x_idle_sleep_start);

/* On remove, once the bus has woken it: take back the reference */
VISIBLE_IF_KUNIT void sn201202x_idle_sleep_stop(struct tps6598x *tps)
{
	if (!tps->sleep_event)
		return;
	pm_runtime_get_noresume(tps->dev);
	pm_runtime_dont_use_autosuspend(tps->dev);
}
EXPORT_SYMBOL_IF_KUNIT(sn201202x_idle_sleep_stop);

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
	struct cd321x_sleep_state st = {};
	u32 links = 0;
	bool wake, ldcm_changed = false;
	int ldcm_ret = 0;
	int ret;

	guard(mutex)(&tps->lock);
	if (!READ_ONCE(sn->sleep_watch))
		return false;

	WRITE_ONCE(sn->atomic, true);
	ret = sn201202x_wake_poll(sn, true);
	if (!ret)
		ret = cd321x_sleep_events(tps, &st);
	if (!ret)
		links = sn201202x_sleep_links(&st);
	if (!ret && st.contaminant_event) {
		enum typec_contaminant last = tps->contaminant;

		ldcm_ret = sn201202x_ldcm_update(tps);
		/* Wake for a new result or a failure, not for a measurement. */
		if (ldcm_ret == -EINPROGRESS)
			ldcm_ret = 0;
		ldcm_changed = tps->contaminant != last;
	}
	wake = ret || ldcm_ret || ldcm_changed || (links & ~sn->sleep_links);
	if (!ret)
		sn->sleep_links = links;
	spmi_command_sleep(sn->sdev);
	WRITE_ONCE(sn->atomic, false);
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
	data = tipd_hw_variants[(enum tipd_hw_variant)(uintptr_t)match->data];

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
	/* A port without liquid detection (MagSafe 3) gets no LDCM commands */
	if (!of_property_read_bool(device->dev.of_node, "apple,no-liquid-detection"))
		tps->contaminant_ops = &sn201202x_contaminant_ops;
	INIT_DELAYED_WORK(&sn->ldcm_work, sn201202x_ldcm_work);

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
		cancel_delayed_work_sync(&sn->ldcm_work);
		spmi_command_sleep(device);
		return ret;
	}
	sn201202x_ldcm_event(tps);

	/* Events can latch before the IRQ is registered. Consume that state now. */
	tps->data->irq_handler(tps->irq, tps);

	sn201202x_idle_sleep_start(tps);
	return 0;
}

static void sn201202x_remove(struct spmi_device *device)
{
	struct tps6598x *tps = spmi_device_get_drvdata(device);
	struct sn201202x *sn = tps_to_sn(tps);

	sn201202x_idle_sleep_stop(tps);
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
	/* One that slept on from idle sleep wakes through runtime PM */
	if (pm_runtime_status_suspended(dev)) {
		err = pm_runtime_resume(dev);
		if (err > 0)
			err = 0;
	} else {
		err = sn201202x_wake(sn);
	}
	if (!err) {
		/* Registered before xHCI, so this is before the root port resumes. */
		tipd_usb_wake(tps, false);
		err = tipd_resume(tps);
	}
	/* The resume revalidation acknowledges events latched while asleep. */
	if (tps->irq)
		enable_irq(tps->irq);
	pm_runtime_put_autosuspend(dev);
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
	/* Awake, also from idle sleep, until resume */
	err = pm_runtime_resume_and_get(dev);
	if (err) {
		/*
		 * It sleeps idle and does not wake: let it sleep on without
		 * the wakeup watch, and let resume wake it.
		 */
		dev_warn(dev, "asleep and not waking: %d, no wakeup for an attach\n", err);
		pm_runtime_get_noresume(dev);
		return 0;
	}
	err = tipd_suspend(tps);
	if (err)
		goto out_put;
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
	/* Registered before xHCI, so its root port has suspended by now. */
	tipd_usb_wake(tps, true);
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
out_put:
	pm_runtime_put_autosuspend(dev);
	if (tps->irq)
		enable_irq(tps->irq);
	return err;
}
EXPORT_SYMBOL_IF_KUNIT(sn201202x_suspend);

static int sn201202x_prepare(struct device *dev)
{
	struct tps6598x *tps = dev_get_drvdata(dev);
	struct sn201202x *sn = tps_to_sn(tps);
	int ret;

	cancel_delayed_work_sync(&sn->ldcm_work);
	mutex_lock(&tps->lock);
	ret = sn201202x_ldcm_update(tps);
	mutex_unlock(&tps->lock);
	/* A measurement in progress keeps the last result; its event wakes us. */
	if (ret == -EINPROGRESS)
		ret = 0;
	if (ret && !sn->ldcm_mitigated) {
		/* Do not enter sleep without a dry snapshot or mitigation ACK. */
		sn201202x_ldcm_event(tps);
		return ret;
	}
	if (ret)
		dev_warn(tps->dev,
			 "connector state unavailable; entering sleep mitigated: %d\n",
			 ret);
	return tipd_prepare(dev);
}

static void sn201202x_complete(struct device *dev)
{
	struct tps6598x *tps = dev_get_drvdata(dev);

	/* Refresh the detector snapshot before cable revalidation is thawed. */
	scoped_guard(mutex, &tps->lock)
		sn201202x_ldcm_refresh(tps);
	tipd_complete(dev);
}

MODULE_DEVICE_TABLE(of, sn201202x_of_match);

static const struct dev_pm_ops sn201202x_pm_ops = {
	.prepare = pm_sleep_ptr(sn201202x_prepare),
	.complete = pm_sleep_ptr(sn201202x_complete),
	SET_SYSTEM_SLEEP_PM_OPS(sn201202x_suspend, sn201202x_resume)
	SET_RUNTIME_PM_OPS(sn201202x_runtime_suspend, sn201202x_runtime_resume, NULL)
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
