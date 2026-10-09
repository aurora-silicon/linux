// SPDX-License-Identifier: GPL-2.0-only
/* Apple Baku PMU clock and retained-power alarm. */

#include <linux/bitops.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/nvmem-consumer.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/rtc.h>
#include <linux/slab.h>
#include <linux/suspend.h>
#include <linux/unaligned.h>

#define BAKU_RTC_CONTROL	0xf800
#define BAKU_RTC_COUNTER	0xf802
#define BAKU_RTC_COMPARE	0xf808
#define BAKU_RTC_EVENT		0xf80c
#define BAKU_RTC_MASK		0xf80e
#define BAKU_RTC_ARM		BIT(6)
#define BAKU_RTC_ALARM		BIT(0)
#define BAKU_RTC_HZ_SHIFT	16
#define BAKU_RTC_MAX_DELAY	3600

static bool experimental;
module_param(experimental, bool, 0444);
MODULE_PARM_DESC(experimental, "Enable alarms with retained SPMI/AIC power in s2idle");

struct baku_rtc {
	struct regmap *map;
	struct nvmem_cell *offset;
	struct rtc_device *clock;
	/* Serialize comparator updates and interrupt acknowledgment. */
	struct mutex transaction;
	int interrupt;
	bool io_failed;
};

static int baku_rtc_seconds(struct baku_rtc *baku, u64 *seconds)
{
	u8 count[8] = {};
	int error;

	error = regmap_bulk_read(baku->map, BAKU_RTC_COUNTER, count, 6);
	if (error)
		return error;
	*seconds = get_unaligned_le64(count) >> BAKU_RTC_HZ_SHIFT;
	return 0;
}

static int baku_rtc_offset(struct baku_rtc *baku, s64 *seconds)
{
	size_t bytes;
	void *value;

	/* The SMC RTC can update this cell, so do not cache it at probe. */
	value = nvmem_cell_read(baku->offset, &bytes);
	if (IS_ERR(value))
		return PTR_ERR(value);
	if (bytes != sizeof(__le64)) {
		kfree(value);
		return -EINVAL;
	}
	*seconds = get_unaligned_le64(value);
	kfree(value);
	if (*seconds < 0 || *seconds > RTC_TIMESTAMP_END_9999 - U32_MAX)
		return -ERANGE;
	return 0;
}

static int baku_rtc_read_time(struct device *dev, struct rtc_time *time)
{
	struct baku_rtc *baku = dev_get_drvdata(dev);
	u64 counter;
	s64 epoch;
	int error;

	guard(mutex)(&baku->transaction);
	error = baku_rtc_offset(baku, &epoch);
	if (error)
		return error;
	error = baku_rtc_seconds(baku, &counter);
	if (error)
		return error;
	rtc_time64_to_tm(epoch + counter, time);
	return 0;
}

/* All comparator/control accesses are serialized with the IRQ thread. */
static int baku_rtc_stop_alarm(struct baku_rtc *baku)
{
	int error;

	error = regmap_update_bits(baku->map, BAKU_RTC_MASK,
				   BAKU_RTC_ALARM, BAKU_RTC_ALARM);
	if (error)
		return error;
	error = regmap_update_bits(baku->map, BAKU_RTC_CONTROL, BAKU_RTC_ARM, 0);
	if (error)
		return error;
	return regmap_write(baku->map, BAKU_RTC_EVENT, BAKU_RTC_ALARM);
}

static int baku_rtc_start_alarm(struct baku_rtc *baku)
{
	int error;

	error = regmap_write(baku->map, BAKU_RTC_EVENT, BAKU_RTC_ALARM);
	if (!error)
		error = regmap_update_bits(baku->map, BAKU_RTC_MASK, BAKU_RTC_ALARM, 0);
	if (!error)
		error = regmap_update_bits(baku->map, BAKU_RTC_CONTROL,
					   BAKU_RTC_ARM, BAKU_RTC_ARM);
	if (error)
		baku_rtc_stop_alarm(baku);
	return error;
}

static int baku_rtc_validate_alarm(struct baku_rtc *baku, u64 comparator)
{
	u64 counter;
	int error;

	error = baku_rtc_seconds(baku, &counter);
	if (error)
		return error;
	if (comparator <= counter)
		return -ETIME;
	if (comparator - counter > BAKU_RTC_MAX_DELAY)
		return -ERANGE;
	return 0;
}

static int baku_rtc_read_alarm(struct device *dev, struct rtc_wkalrm *alarm)
{
	struct baku_rtc *baku = dev_get_drvdata(dev);
	unsigned int control, event, mask;
	__le32 comparator;
	s64 epoch;
	int error;

	guard(mutex)(&baku->transaction);
	error = baku_rtc_offset(baku, &epoch);
	if (error)
		return error;
	error = regmap_bulk_read(baku->map, BAKU_RTC_COMPARE,
				 &comparator, sizeof(comparator));
	if (!error)
		error = regmap_read(baku->map, BAKU_RTC_CONTROL, &control);
	if (!error)
		error = regmap_read(baku->map, BAKU_RTC_EVENT, &event);
	if (!error)
		error = regmap_read(baku->map, BAKU_RTC_MASK, &mask);
	if (error)
		return error;
	rtc_time64_to_tm(epoch + le32_to_cpu(comparator), &alarm->time);
	alarm->enabled = (control & BAKU_RTC_ARM) && !(mask & BAKU_RTC_ALARM);
	alarm->pending = !!(event & BAKU_RTC_ALARM);
	return 0;
}

static int baku_rtc_set_alarm(struct device *dev, struct rtc_wkalrm *alarm)
{
	struct baku_rtc *baku = dev_get_drvdata(dev);
	__le32 comparator, readback;
	s64 epoch, deadline;
	int error;

	guard(mutex)(&baku->transaction);
	if (baku->io_failed)
		return -EIO;
	error = baku_rtc_offset(baku, &epoch);
	if (error)
		return error;
	deadline = rtc_tm_to_time64(&alarm->time) - epoch;
	if (deadline < 0 || deadline > U32_MAX)
		return -ERANGE;
	error = baku_rtc_validate_alarm(baku, deadline);
	if (error)
		return error;
	error = baku_rtc_stop_alarm(baku);
	if (error)
		return error;
	comparator = cpu_to_le32(deadline);
	error = regmap_bulk_write(baku->map, BAKU_RTC_COMPARE,
				  &comparator, sizeof(comparator));
	if (error)
		return error;
	error = regmap_bulk_read(baku->map, BAKU_RTC_COMPARE,
				 &readback, sizeof(readback));
	if (error)
		return error;
	if (readback != comparator)
		return -EIO;
	if (alarm->enabled)
		return baku_rtc_start_alarm(baku);
	return 0;
}

static int baku_rtc_alarm_enable(struct device *dev, unsigned int enable)
{
	struct baku_rtc *baku = dev_get_drvdata(dev);
	__le32 comparator;
	int error;

	guard(mutex)(&baku->transaction);
	if (!enable)
		return baku_rtc_stop_alarm(baku);
	if (baku->io_failed)
		return -EIO;
	error = regmap_bulk_read(baku->map, BAKU_RTC_COMPARE,
				 &comparator, sizeof(comparator));
	if (error)
		return error;
	error = baku_rtc_validate_alarm(baku, le32_to_cpu(comparator));
	return error ?: baku_rtc_start_alarm(baku);
}

static irqreturn_t baku_rtc_interrupt(int irq, void *context)
{
	struct device *dev = context;
	struct baku_rtc *baku = dev_get_drvdata(dev);
	unsigned int event;
	int error;

	mutex_lock(&baku->transaction);
	error = regmap_read(baku->map, BAKU_RTC_EVENT, &event);
	if (!error && (event & BAKU_RTC_ALARM))
		error = baku_rtc_stop_alarm(baku);
	if (error) {
		baku->io_failed = true;
		disable_irq_nosync(irq);
	}
	mutex_unlock(&baku->transaction);
	if (error) {
		dev_err(dev, "cannot acknowledge RTC interrupt: %d\n", error);
		pm_wakeup_dev_event(dev, 0, true);
		return IRQ_HANDLED;
	}
	if (!(event & BAKU_RTC_ALARM))
		return IRQ_NONE;
	if (device_may_wakeup(dev))
		pm_wakeup_dev_event(dev, 0, true);
	rtc_update_irq(baku->clock, 1, RTC_IRQF | RTC_AF);
	return IRQ_HANDLED;
}

/*
 * Firmware leaves the alarm armed even when it has no wake scheduled, with
 * the comparator set close to the end of 32-bit time. Keep an alarm that is
 * still pending within the supported range, so that the RTC core picks it
 * up at registration, and disarm anything else.
 */
static int baku_rtc_init_alarm(struct baku_rtc *baku)
{
	unsigned int control, mask;
	__le32 comparator;
	int error;

	error = regmap_read(baku->map, BAKU_RTC_CONTROL, &control);
	if (!error)
		error = regmap_read(baku->map, BAKU_RTC_MASK, &mask);
	if (!error)
		error = regmap_bulk_read(baku->map, BAKU_RTC_COMPARE,
					 &comparator, sizeof(comparator));
	if (error)
		return error;
	if ((control & BAKU_RTC_ARM) && !(mask & BAKU_RTC_ALARM) &&
	    !baku_rtc_validate_alarm(baku, le32_to_cpu(comparator)))
		return 0;
	return baku_rtc_stop_alarm(baku);
}

static const struct rtc_class_ops baku_rtc_ops = {
	.read_time = baku_rtc_read_time,
	.read_alarm = baku_rtc_read_alarm,
	.set_alarm = baku_rtc_set_alarm,
	.alarm_irq_enable = baku_rtc_alarm_enable,
};

static int baku_rtc_prepare(struct device *dev)
{
	struct baku_rtc *baku = dev_get_drvdata(dev);

	if (pm_suspend_target_state != PM_SUSPEND_TO_IDLE || baku->io_failed)
		return -EBUSY;
	return 0;
}

static const struct dev_pm_ops baku_rtc_pm = {
	.prepare = baku_rtc_prepare,
};

static void baku_rtc_release(void *context)
{
	struct device *dev = context;
	struct baku_rtc *baku = dev_get_drvdata(dev);

	disable_irq(baku->interrupt);
	mutex_lock(&baku->transaction);
	if (baku_rtc_stop_alarm(baku))
		dev_err(dev, "cannot disable RTC alarm on removal\n");
	mutex_unlock(&baku->transaction);
	device_init_wakeup(dev, false);
}

static int baku_rtc_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct baku_rtc *baku;
	int error;

	/* Retention has only been measured on J700 with the explicit opt-in. */
	if (!experimental)
		return -ENODEV;
	baku = devm_kzalloc(dev, sizeof(*baku), GFP_KERNEL);
	if (!baku)
		return -ENOMEM;
	baku->map = dev_get_regmap(dev->parent, NULL);
	if (!baku->map)
		return dev_err_probe(dev, -EPROBE_DEFER, "PMIC regmap unavailable\n");
	baku->offset = devm_nvmem_cell_get(dev, "rtc_offset");
	if (IS_ERR(baku->offset))
		return dev_err_probe(dev, PTR_ERR(baku->offset), "RTC offset unavailable\n");
	baku->interrupt = platform_get_irq(pdev, 0);
	if (baku->interrupt < 0)
		return baku->interrupt;
	mutex_init(&baku->transaction);
	platform_set_drvdata(pdev, baku);
	baku->clock = devm_rtc_allocate_device(dev);
	if (IS_ERR(baku->clock))
		return PTR_ERR(baku->clock);
	baku->clock->ops = &baku_rtc_ops;
	baku->clock->range_min = 0;
	baku->clock->range_max = RTC_TIMESTAMP_END_9999;
	baku->clock->alarm_offset_max = BAKU_RTC_MAX_DELAY;
	clear_bit(RTC_FEATURE_UPDATE_INTERRUPT, baku->clock->features);
	error = device_init_wakeup(dev, true);
	if (error)
		return error;
	/*
	 * This interrupt remains live during retained-power s2idle. The
	 * thread uses the parent PMIC's SPMI regmap to acknowledge the alarm.
	 * Off-domain wake routing is not established by this driver.
	 */
	error = devm_request_threaded_irq(dev, baku->interrupt, NULL,
					  baku_rtc_interrupt,
					  IRQF_NO_AUTOEN | IRQF_NO_SUSPEND | IRQF_ONESHOT,
					  dev_name(dev), dev);
	if (error)
		goto disable_wakeup;
	error = baku_rtc_init_alarm(baku);
	if (error)
		goto disable_wakeup;
	error = devm_rtc_register_device(baku->clock);
	if (error)
		goto disable_wakeup;
	error = devm_add_action_or_reset(dev, baku_rtc_release, dev);
	if (error)
		return error;
	enable_irq(baku->interrupt);
	return 0;

disable_wakeup:
	device_init_wakeup(dev, false);
	return error;
}

static const struct of_device_id baku_rtc_match[] = {
	{ .compatible = "apple,j700-pmu-rtc" },
	{ }
};
MODULE_DEVICE_TABLE(of, baku_rtc_match);

static struct platform_driver baku_rtc_driver = {
	.probe = baku_rtc_probe,
	.driver = {
		.name = "apple-pmu-rtc",
		.of_match_table = baku_rtc_match,
		.pm = &baku_rtc_pm,
	},
};
module_platform_driver(baku_rtc_driver);

MODULE_AUTHOR("Eryk Wieliczko <github@wieliczko.ninja>");
MODULE_DESCRIPTION("Apple Baku PMU RTC alarm with retained-power s2idle");
MODULE_LICENSE("GPL");
