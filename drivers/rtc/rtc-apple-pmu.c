// SPDX-License-Identifier: GPL-2.0-only
/* Apple PMU clock and retained-power alarm. */

#include <linux/bitops.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/nvmem-consumer.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/rtc.h>
#include <linux/slab.h>
#include <linux/suspend.h>
#include <linux/unaligned.h>

#define APPLE_PMU_RTC_CONTROL	0x0
#define APPLE_PMU_RTC_COUNTER	0x2
#define APPLE_PMU_RTC_COMPARE	0x8
#define APPLE_PMU_RTC_MASK	0xe
#define APPLE_PMU_RTC_ARM		BIT(6)
#define APPLE_PMU_RTC_ALARM		BIT(0)
#define APPLE_PMU_RTC_HZ_SHIFT	16
#define APPLE_PMU_RTC_MAX_DELAY	3600

struct apple_pmu_rtc_hw {
	u16 base;
	u16 event;
	u8 event_bit;
	u8 monitor_bit;
	bool offset_in_seconds;
};

static const struct apple_pmu_rtc_hw apple_pmu_rtc_baku = {
	.base = 0xf800,
	.event = 0xf80c,
	.event_bit = BIT(0),
	.offset_in_seconds = true,
};

static const struct apple_pmu_rtc_hw apple_pmu_rtc_abbey = {
	.base = 0x2000,
	.event = 0x2000,
	.event_bit = BIT(1),
	.monitor_bit = BIT(0),
};

struct apple_pmu_rtc {
	const struct apple_pmu_rtc_hw *hw;
	struct regmap *map;
	struct nvmem_cell *offset;
	struct rtc_device *clock;
	/* Serialize comparator updates and interrupt acknowledgment. */
	struct mutex transaction;
	int interrupt;
	bool io_failed;
};

static int apple_pmu_rtc_counter(struct apple_pmu_rtc *rtc, u64 *ticks)
{
	u8 count[8] = {};
	int error;

	error = regmap_bulk_read(rtc->map, rtc->hw->base + APPLE_PMU_RTC_COUNTER,
				 count, 6);
	if (error)
		return error;
	*ticks = get_unaligned_le64(count);
	return 0;
}

/* Return the offset in 1/65536 second units, matching the PMU counter. */
static int apple_pmu_rtc_offset(struct apple_pmu_rtc *rtc, s64 *ticks)
{
	u64 offset = 0;
	size_t bytes;
	void *value;

	/* The SMC RTC can update this cell, so do not cache it at probe. */
	value = nvmem_cell_read(rtc->offset, &bytes);
	if (IS_ERR(value))
		return PTR_ERR(value);
	if (bytes != (rtc->hw->offset_in_seconds ? 8 : 6)) {
		kfree(value);
		return -EINVAL;
	}
	memcpy(&offset, value, bytes);
	kfree(value);
	offset = le64_to_cpu(offset);
	if (rtc->hw->offset_in_seconds) {
		if (offset > RTC_TIMESTAMP_END_9999 - U32_MAX)
			return -ERANGE;
		*ticks = offset << APPLE_PMU_RTC_HZ_SHIFT;
	} else {
		/* Abbey stores a signed 48-bit offset in 1/32768 seconds. */
		*ticks = sign_extend64(offset, 47) * 2;
	}
	return 0;
}

static int apple_pmu_rtc_read_time(struct device *dev, struct rtc_time *time)
{
	struct apple_pmu_rtc *rtc = dev_get_drvdata(dev);
	u64 counter;
	s64 epoch;
	int error;

	guard(mutex)(&rtc->transaction);
	error = apple_pmu_rtc_offset(rtc, &epoch);
	if (error)
		return error;
	error = apple_pmu_rtc_counter(rtc, &counter);
	if (error)
		return error;
	rtc_time64_to_tm(((s64)counter + epoch) >> APPLE_PMU_RTC_HZ_SHIFT, time);
	return 0;
}

/* All comparator/control accesses are serialized with the IRQ thread. */
static int apple_pmu_rtc_ack_alarm(struct apple_pmu_rtc *rtc)
{
	unsigned int control = 0;
	int error;

	/*
	 * The event is W1C. Abbey shares the byte with ARM and MONITOR,
	 * unlike Baku's dedicated event register. Preserve the control bits
	 * when acknowledging, rather than writing only the event bit.
	 */
	if (rtc->hw->event == rtc->hw->base + APPLE_PMU_RTC_CONTROL) {
		error = regmap_read(rtc->map, rtc->hw->event, &control);
		if (error)
			return error;
	}
	return regmap_write(rtc->map, rtc->hw->event, control | rtc->hw->event_bit);
}

static int apple_pmu_rtc_arm(struct apple_pmu_rtc *rtc, bool enable)
{
	unsigned int control;
	int error;

	error = regmap_read(rtc->map, rtc->hw->base + APPLE_PMU_RTC_CONTROL, &control);
	if (error)
		return error;
	/* Do not acknowledge a shared W1C event as a side effect of RMW. */
	if (rtc->hw->event == rtc->hw->base + APPLE_PMU_RTC_CONTROL)
		control &= ~rtc->hw->event_bit;
	control &= ~APPLE_PMU_RTC_ARM;
	if (enable)
		control |= APPLE_PMU_RTC_ARM;
	control |= rtc->hw->monitor_bit;
	return regmap_write(rtc->map, rtc->hw->base + APPLE_PMU_RTC_CONTROL, control);
}

static int apple_pmu_rtc_stop_alarm(struct apple_pmu_rtc *rtc)
{
	int error;

	error = regmap_update_bits(rtc->map, rtc->hw->base + APPLE_PMU_RTC_MASK,
				   APPLE_PMU_RTC_ALARM, APPLE_PMU_RTC_ALARM);
	if (error)
		return error;
	error = apple_pmu_rtc_arm(rtc, false);
	if (error)
		return error;
	return apple_pmu_rtc_ack_alarm(rtc);
}

static int apple_pmu_rtc_start_alarm(struct apple_pmu_rtc *rtc)
{
	int error;

	error = apple_pmu_rtc_ack_alarm(rtc);
	if (!error)
		error = regmap_update_bits(rtc->map, rtc->hw->base + APPLE_PMU_RTC_MASK,
					   APPLE_PMU_RTC_ALARM, 0);
	if (!error)
		error = apple_pmu_rtc_arm(rtc, true);
	if (error)
		apple_pmu_rtc_stop_alarm(rtc);
	return error;
}

static int apple_pmu_rtc_validate_alarm(struct apple_pmu_rtc *rtc, u64 comparator)
{
	u64 counter;
	int error;

	error = apple_pmu_rtc_counter(rtc, &counter);
	if (error)
		return error;
	counter >>= APPLE_PMU_RTC_HZ_SHIFT;
	if (comparator <= counter)
		return -ETIME;
	if (comparator - counter > APPLE_PMU_RTC_MAX_DELAY)
		return -ERANGE;
	return 0;
}

static int apple_pmu_rtc_read_alarm(struct device *dev, struct rtc_wkalrm *alarm)
{
	struct apple_pmu_rtc *rtc = dev_get_drvdata(dev);
	unsigned int control, event, mask;
	__le32 comparator;
	s64 epoch;
	int error;

	guard(mutex)(&rtc->transaction);
	error = apple_pmu_rtc_offset(rtc, &epoch);
	if (error)
		return error;
	error = regmap_bulk_read(rtc->map, rtc->hw->base + APPLE_PMU_RTC_COMPARE,
				 &comparator, sizeof(comparator));
	if (!error)
		error = regmap_read(rtc->map, rtc->hw->base + APPLE_PMU_RTC_CONTROL, &control);
	if (!error)
		error = regmap_read(rtc->map, rtc->hw->event, &event);
	if (!error)
		error = regmap_read(rtc->map, rtc->hw->base + APPLE_PMU_RTC_MASK, &mask);
	if (error)
		return error;
	rtc_time64_to_tm((epoch >> APPLE_PMU_RTC_HZ_SHIFT) +
			le32_to_cpu(comparator), &alarm->time);
	alarm->enabled = (control & APPLE_PMU_RTC_ARM) && !(mask & APPLE_PMU_RTC_ALARM);
	alarm->pending = !!(event & rtc->hw->event_bit);
	return 0;
}

static int apple_pmu_rtc_set_alarm(struct device *dev, struct rtc_wkalrm *alarm)
{
	struct apple_pmu_rtc *rtc = dev_get_drvdata(dev);
	__le32 comparator, readback;
	s64 epoch, deadline;
	int error;

	guard(mutex)(&rtc->transaction);
	if (rtc->io_failed)
		return -EIO;
	error = apple_pmu_rtc_offset(rtc, &epoch);
	if (error)
		return error;
	/* Rounding the offset down lets fractional offsets fire up to 1 s late. */
	deadline = rtc_tm_to_time64(&alarm->time) - (epoch >> APPLE_PMU_RTC_HZ_SHIFT);
	if (deadline < 0 || deadline > U32_MAX)
		return -ERANGE;
	error = apple_pmu_rtc_validate_alarm(rtc, deadline);
	if (error)
		return error;
	error = apple_pmu_rtc_stop_alarm(rtc);
	if (error)
		return error;
	comparator = cpu_to_le32(deadline);
	error = regmap_bulk_write(rtc->map, rtc->hw->base + APPLE_PMU_RTC_COMPARE,
				  &comparator, sizeof(comparator));
	if (error)
		return error;
	error = regmap_bulk_read(rtc->map, rtc->hw->base + APPLE_PMU_RTC_COMPARE,
				 &readback, sizeof(readback));
	if (error)
		return error;
	if (readback != comparator)
		return -EIO;
	if (alarm->enabled)
		return apple_pmu_rtc_start_alarm(rtc);
	return 0;
}

static int apple_pmu_rtc_alarm_enable(struct device *dev, unsigned int enable)
{
	struct apple_pmu_rtc *rtc = dev_get_drvdata(dev);
	__le32 comparator;
	int error;

	guard(mutex)(&rtc->transaction);
	if (!enable)
		return apple_pmu_rtc_stop_alarm(rtc);
	if (rtc->io_failed)
		return -EIO;
	error = regmap_bulk_read(rtc->map, rtc->hw->base + APPLE_PMU_RTC_COMPARE,
				 &comparator, sizeof(comparator));
	if (error)
		return error;
	error = apple_pmu_rtc_validate_alarm(rtc, le32_to_cpu(comparator));
	return error ?: apple_pmu_rtc_start_alarm(rtc);
}

static irqreturn_t apple_pmu_rtc_interrupt(int irq, void *context)
{
	struct device *dev = context;
	struct apple_pmu_rtc *rtc = dev_get_drvdata(dev);
	unsigned int event;
	int error;

	mutex_lock(&rtc->transaction);
	error = regmap_read(rtc->map, rtc->hw->event, &event);
	if (!error && (event & rtc->hw->event_bit))
		error = apple_pmu_rtc_stop_alarm(rtc);
	if (error) {
		rtc->io_failed = true;
		disable_irq_nosync(irq);
	}
	mutex_unlock(&rtc->transaction);
	if (error) {
		dev_err(dev, "cannot acknowledge RTC interrupt: %d\n", error);
		pm_wakeup_dev_event(dev, 0, true);
		return IRQ_HANDLED;
	}
	if (!(event & rtc->hw->event_bit))
		return IRQ_NONE;
	if (device_may_wakeup(dev))
		pm_wakeup_dev_event(dev, 0, true);
	rtc_update_irq(rtc->clock, 1, RTC_IRQF | RTC_AF);
	return IRQ_HANDLED;
}

/*
 * Firmware leaves the alarm armed even when it has no wake scheduled, with
 * the comparator set close to the end of 32-bit time. Keep an alarm that is
 * still pending within the supported range, so that the RTC core picks it
 * up at registration, and disarm anything else.
 */
static int apple_pmu_rtc_init_alarm(struct apple_pmu_rtc *rtc)
{
	unsigned int control, mask;
	__le32 comparator;
	int error;

	error = regmap_read(rtc->map, rtc->hw->base + APPLE_PMU_RTC_CONTROL, &control);
	if (!error)
		error = regmap_read(rtc->map, rtc->hw->base + APPLE_PMU_RTC_MASK, &mask);
	if (!error)
		error = regmap_bulk_read(rtc->map, rtc->hw->base + APPLE_PMU_RTC_COMPARE,
					 &comparator, sizeof(comparator));
	if (error)
		return error;
	if ((control & APPLE_PMU_RTC_ARM) && !(mask & APPLE_PMU_RTC_ALARM) &&
	    !apple_pmu_rtc_validate_alarm(rtc, le32_to_cpu(comparator)))
		return 0;
	return apple_pmu_rtc_stop_alarm(rtc);
}

static const struct rtc_class_ops apple_pmu_rtc_ops = {
	.read_time = apple_pmu_rtc_read_time,
	.read_alarm = apple_pmu_rtc_read_alarm,
	.set_alarm = apple_pmu_rtc_set_alarm,
	.alarm_irq_enable = apple_pmu_rtc_alarm_enable,
};

static int apple_pmu_rtc_prepare(struct device *dev)
{
	struct apple_pmu_rtc *rtc = dev_get_drvdata(dev);

	if (pm_suspend_target_state != PM_SUSPEND_TO_IDLE || rtc->io_failed)
		return -EBUSY;
	return 0;
}

static const struct dev_pm_ops apple_pmu_rtc_pm = {
	.prepare = apple_pmu_rtc_prepare,
};

static void apple_pmu_rtc_release(void *context)
{
	struct device *dev = context;
	struct apple_pmu_rtc *rtc = dev_get_drvdata(dev);

	disable_irq(rtc->interrupt);
	mutex_lock(&rtc->transaction);
	if (apple_pmu_rtc_stop_alarm(rtc))
		dev_err(dev, "cannot disable RTC alarm on removal\n");
	mutex_unlock(&rtc->transaction);
	device_init_wakeup(dev, false);
}

static int apple_pmu_rtc_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct apple_pmu_rtc *rtc;
	int error;

	rtc = devm_kzalloc(dev, sizeof(*rtc), GFP_KERNEL);
	if (!rtc)
		return -ENOMEM;
	rtc->hw = of_device_get_match_data(dev);
	if (!rtc->hw)
		return -EINVAL;
	rtc->map = dev_get_regmap(dev->parent, NULL);
	if (!rtc->map)
		return dev_err_probe(dev, -EPROBE_DEFER, "PMIC regmap unavailable\n");
	rtc->offset = devm_nvmem_cell_get(dev, "rtc_offset");
	if (IS_ERR(rtc->offset))
		return dev_err_probe(dev, PTR_ERR(rtc->offset), "RTC offset unavailable\n");
	rtc->interrupt = platform_get_irq(pdev, 0);
	if (rtc->interrupt < 0)
		return rtc->interrupt;
	mutex_init(&rtc->transaction);
	platform_set_drvdata(pdev, rtc);
	rtc->clock = devm_rtc_allocate_device(dev);
	if (IS_ERR(rtc->clock))
		return PTR_ERR(rtc->clock);
	rtc->clock->ops = &apple_pmu_rtc_ops;
	rtc->clock->range_min = 0;
	rtc->clock->range_max = RTC_TIMESTAMP_END_9999;
	rtc->clock->alarm_offset_max = APPLE_PMU_RTC_MAX_DELAY;
	clear_bit(RTC_FEATURE_UPDATE_INTERRUPT, rtc->clock->features);
	error = device_init_wakeup(dev, true);
	if (error)
		return error;
	/*
	 * This interrupt remains live during retained-power s2idle. The
	 * thread uses the parent PMIC's SPMI regmap to acknowledge the alarm.
	 * Off-domain wake routing is not established by this driver.
	 */
	error = devm_request_threaded_irq(dev, rtc->interrupt, NULL,
					  apple_pmu_rtc_interrupt,
					  IRQF_NO_AUTOEN | IRQF_NO_SUSPEND | IRQF_ONESHOT,
					  dev_name(dev), dev);
	if (error)
		goto disable_wakeup;
	error = apple_pmu_rtc_init_alarm(rtc);
	if (error)
		goto disable_wakeup;
	error = devm_rtc_register_device(rtc->clock);
	if (error)
		goto disable_wakeup;
	error = devm_add_action_or_reset(dev, apple_pmu_rtc_release, dev);
	if (error)
		return error;
	enable_irq(rtc->interrupt);
	return 0;

disable_wakeup:
	device_init_wakeup(dev, false);
	return error;
}

static const struct of_device_id apple_pmu_rtc_match[] = {
	{ .compatible = "apple,j700-pmu-rtc", .data = &apple_pmu_rtc_baku },
	{ .compatible = "apple,t6041-pmu-rtc", .data = &apple_pmu_rtc_abbey },
	{ }
};
MODULE_DEVICE_TABLE(of, apple_pmu_rtc_match);

static struct platform_driver apple_pmu_rtc_driver = {
	.probe = apple_pmu_rtc_probe,
	.driver = {
		.name = "apple-pmu-rtc",
		.of_match_table = apple_pmu_rtc_match,
		.pm = &apple_pmu_rtc_pm,
	},
};
module_platform_driver(apple_pmu_rtc_driver);

MODULE_AUTHOR("Eryk Wieliczko <github@wieliczko.ninja>");
MODULE_DESCRIPTION("Apple PMU RTC alarm with retained-power s2idle");
MODULE_LICENSE("GPL");
