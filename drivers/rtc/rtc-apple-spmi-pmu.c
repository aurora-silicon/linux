// SPDX-License-Identifier: GPL-2.0-only
/*
 * Apple SPMI PMU RTC with a 48-bit counter and signed wall-clock offset.
 * Counter and alarm accesses use named NVMEM cells supplied by the boot DT.
 */

#include <linux/bitops.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/nvmem-consumer.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_wakeirq.h>
#include <linux/rtc.h>
#include <linux/slab.h>
#include <linux/unaligned.h>

#define RTC_BYTES	6
#define RTC_BITS	(8 * RTC_BYTES)
/* Wall time and offset are in 1/32768 s; the counter runs twice as fast. */
#define RTC_SEC_SHIFT	15

/* Abbey PMU alarm control bits; match this layout explicitly. */
#define ALARM_EVENT	BIT(1)
#define ALARM_MONITOR	BIT(0)
#define ALARM_ENABLE	BIT(6)
#define ALARM_IRQ_MASK	BIT(0)

struct apple_pmu_rtc {
	struct device *dev;
	struct rtc_device *rtc;
	struct nvmem_cell *counter, *offset, *alarm, *ctrl, *irq_mask;
	struct mutex lock; /* Serializes counter, offset and alarm transactions. */
	bool alarm_fault;
};

static int apple_pmu_rtc_read_cell(struct nvmem_cell *cell, void *val, size_t len)
{
	size_t got;
	void *buf = nvmem_cell_read(cell, &got);

	if (IS_ERR(buf))
		return PTR_ERR(buf);
	if (got != len) {
		kfree(buf);
		return -EIO;
	}
	memcpy(val, buf, len);
	kfree(buf);
	return 0;
}

static int apple_pmu_rtc_write_cell(struct nvmem_cell *cell, void *val, size_t len)
{
	int ret = nvmem_cell_write(cell, val, len);

	return ret < 0 ? ret : ret == len ? 0 : -EIO;
}

static int apple_pmu_rtc_write_u8(struct nvmem_cell *cell, u8 val)
{
	return apple_pmu_rtc_write_cell(cell, &val, sizeof(val));
}

static int apple_pmu_rtc_read_u48(struct nvmem_cell *cell, u64 *val)
{
	u8 bytes[8] = {};
	int ret = apple_pmu_rtc_read_cell(cell, bytes, RTC_BYTES);

	if (!ret)
		*val = get_unaligned_le64(bytes);
	return ret;
}

/* Wall time in 1/32768 s: counter / 2 + offset. */
static int apple_pmu_rtc_ticks(struct apple_pmu_rtc *prtc, u64 *ticks, u64 *offset)
{
	u64 a = 0, b = 0, off = 0;
	int ret, tries = 3;

	do {
		a = b;
		ret = apple_pmu_rtc_read_u48(prtc->counter, &b);
		if (ret)
			return ret;
	} while (a >> 16 != b >> 16 && --tries);

	if (!tries)
		return -EIO;

	/* Do not use an inconsistent offset to construct wall time. */
	for (tries = 3; tries; tries--) {
		u64 check = 0;

		ret = apple_pmu_rtc_read_u48(prtc->offset, &off);
		if (!ret)
			ret = apple_pmu_rtc_read_u48(prtc->offset, &check);
		if (ret)
			return ret;
		if (off == check)
			break;
	}
	if (!tries)
		return -EIO;
	*ticks = (b >> 1) + off;
	if (offset)
		*offset = off;
	return 0;
}

static int apple_pmu_rtc_read_time(struct device *dev, struct rtc_time *tm)
{
	struct apple_pmu_rtc *prtc = dev_get_drvdata(dev);
	u64 ticks;
	int ret;

	guard(mutex)(&prtc->lock);
	ret = apple_pmu_rtc_ticks(prtc, &ticks, NULL);

	if (ret)
		return ret;
	rtc_time64_to_tm(sign_extend64(ticks, RTC_BITS - 1) >> RTC_SEC_SHIFT, tm);
	return 0;
}

static int apple_pmu_rtc_set_time(struct device *dev, struct rtc_time *tm)
{
	struct apple_pmu_rtc *prtc = dev_get_drvdata(dev);
	u64 ctr = 0, off;
	u8 bytes[8];
	int ret;

	guard(mutex)(&prtc->lock);
	ret = apple_pmu_rtc_read_u48(prtc->counter, &ctr);

	if (ret)
		return ret;
	/* The set second begins now. */
	off = ((u64)rtc_tm_to_time64(tm) << RTC_SEC_SHIFT) - (ctr >> 1);
	put_unaligned_le64(off, bytes);
	return apple_pmu_rtc_write_cell(prtc->offset, bytes, RTC_BYTES);
}

/* Arm or disarm the programmed alarm; caller holds prtc->lock. */
static int apple_pmu_rtc_program(struct apple_pmu_rtc *prtc, bool enable)
{
	u8 ctrl, mask;
	int ret;

	if (prtc->alarm_fault)
		return -EIO;
	ret = apple_pmu_rtc_read_cell(prtc->ctrl, &ctrl, 1);
	if (ret)
		return ret;
	ret = apple_pmu_rtc_read_cell(prtc->irq_mask, &mask, 1);
	if (ret)
		return ret;
	ret = apple_pmu_rtc_write_u8(prtc->irq_mask, mask | ALARM_IRQ_MASK);
	if (ret)
		return ret;
	/* Disarm and acknowledge before changing the comparator or unmasking. */
	ret = apple_pmu_rtc_write_u8(prtc->ctrl, ctrl & ~ALARM_ENABLE);
	if (ret)
		return ret;
	ctrl &= ~(ALARM_EVENT | ALARM_ENABLE);
	ret = apple_pmu_rtc_write_u8(prtc->ctrl,
				     ctrl | ALARM_MONITOR | (enable ? ALARM_ENABLE : 0));
	if (ret || !enable)
		return ret;
	return apple_pmu_rtc_write_u8(prtc->irq_mask, mask & ~ALARM_IRQ_MASK);
}

static int apple_pmu_rtc_read_alarm(struct device *dev, struct rtc_wkalrm *wkalrm)
{
	struct apple_pmu_rtc *prtc = dev_get_drvdata(dev);
	u64 ticks, off;
	__le32 alarm = 0;
	u8 ctrl = 0;
	int ret;

	mutex_lock(&prtc->lock);
	ret = apple_pmu_rtc_ticks(prtc, &ticks, &off);
	if (!ret)
		ret = apple_pmu_rtc_read_cell(prtc->alarm, &alarm, sizeof(alarm));
	if (!ret)
		ret = apple_pmu_rtc_read_cell(prtc->ctrl, &ctrl, 1);
	mutex_unlock(&prtc->lock);
	if (ret)
		return ret;

	wkalrm->enabled = !!(ctrl & ALARM_ENABLE);
	wkalrm->pending = !!(ctrl & ALARM_EVENT);
	/* The alarm is in counter seconds; one counter second is 1 s. */
	ticks = ((u64)le32_to_cpu(alarm) << RTC_SEC_SHIFT) + off;
	rtc_time64_to_tm(sign_extend64(ticks, RTC_BITS - 1) >> RTC_SEC_SHIFT,
			 &wkalrm->time);
	return 0;
}

static int apple_pmu_rtc_set_alarm(struct device *dev, struct rtc_wkalrm *wkalrm)
{
	struct apple_pmu_rtc *prtc = dev_get_drvdata(dev);
	u64 off = 0, ticks;
	int ret;

	__le32 alarm;

	mutex_lock(&prtc->lock);
	ret = apple_pmu_rtc_program(prtc, false);
	if (!ret)
		ret = apple_pmu_rtc_read_u48(prtc->offset, &off);
	if (!ret) {
		/* Counter seconds; round up so the alarm never fires early. */
		ticks = ((u64)rtc_tm_to_time64(&wkalrm->time) << RTC_SEC_SHIFT) - off;
		ticks &= GENMASK_ULL(RTC_BITS - 1, 0);
		alarm = cpu_to_le32(DIV_ROUND_UP_ULL(ticks, 1ULL << RTC_SEC_SHIFT));
		ret = apple_pmu_rtc_write_cell(prtc->alarm, &alarm, sizeof(alarm));
	}
	if (!ret)
		ret = apple_pmu_rtc_program(prtc, wkalrm->enabled);
	mutex_unlock(&prtc->lock);
	return ret;
}

static int apple_pmu_rtc_alarm_irq_enable(struct device *dev, unsigned int enabled)
{
	struct apple_pmu_rtc *prtc = dev_get_drvdata(dev);
	int ret;

	mutex_lock(&prtc->lock);
	ret = apple_pmu_rtc_program(prtc, enabled);
	mutex_unlock(&prtc->lock);
	return ret;
}

static irqreturn_t apple_pmu_rtc_irq(int irq, void *data)
{
	struct apple_pmu_rtc *prtc = data;
	u8 ctrl = 0;
	int ret;

	mutex_lock(&prtc->lock);
	ret = apple_pmu_rtc_read_cell(prtc->ctrl, &ctrl, 1);
	if (ret) {
		prtc->alarm_fault = true;
		disable_irq_nosync(irq);
		mutex_unlock(&prtc->lock);
		dev_err_ratelimited(prtc->dev, "alarm status read failed: %d\n", ret);
		return IRQ_HANDLED;
	}
	if (!(ctrl & ALARM_EVENT)) {
		mutex_unlock(&prtc->lock);
		return IRQ_NONE;
	}
	/* One-shot, as the RTC core expects: clear the event and disarm. */
	ret = apple_pmu_rtc_program(prtc, false);
	if (ret) {
		prtc->alarm_fault = true;
		disable_irq_nosync(irq);
		dev_err_ratelimited(prtc->dev, "alarm acknowledgment failed: %d\n", ret);
	}
	mutex_unlock(&prtc->lock);
	rtc_update_irq(prtc->rtc, 1, RTC_AF | RTC_IRQF);
	return IRQ_HANDLED;
}

static const struct rtc_class_ops apple_pmu_rtc_ops = {
	.read_time = apple_pmu_rtc_read_time,
	.set_time = apple_pmu_rtc_set_time,
	.read_alarm = apple_pmu_rtc_read_alarm,
	.set_alarm = apple_pmu_rtc_set_alarm,
	.alarm_irq_enable = apple_pmu_rtc_alarm_irq_enable,
};

static void apple_pmu_rtc_clear_wake(void *data)
{
	struct device *dev = data;

	dev_pm_clear_wake_irq(dev);
	device_init_wakeup(dev, false);
}

static void apple_pmu_rtc_stop(void *data)
{
	struct apple_pmu_rtc *prtc = data;

	guard(mutex)(&prtc->lock);
	if (apple_pmu_rtc_program(prtc, false))
		dev_err(prtc->dev, "failed to disarm RTC alarm\n");
}

static int apple_pmu_rtc_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct apple_pmu_rtc *prtc;
	int i, irq, nirq, ret;

	prtc = devm_kzalloc(dev, sizeof(*prtc), GFP_KERNEL);
	if (!prtc)
		return -ENOMEM;
	prtc->dev = dev;
	mutex_init(&prtc->lock);
	platform_set_drvdata(pdev, prtc);

	struct {
		struct nvmem_cell **cell;
		const char *name;
	} cells[] = {
		{ &prtc->counter, "counter" },
		{ &prtc->offset, "rtc_offset" },
		{ &prtc->alarm, "alarm" },
		{ &prtc->ctrl, "alarm_ctrl" },
		{ &prtc->irq_mask, "irq_mask" },
	};
	for (i = 0; i < ARRAY_SIZE(cells); i++) {
		*cells[i].cell = devm_nvmem_cell_get(dev, cells[i].name);
		if (IS_ERR(*cells[i].cell))
			return dev_err_probe(dev, PTR_ERR(*cells[i].cell),
					     "missing NVMEM cell %s\n", cells[i].name);
	}

	prtc->rtc = devm_rtc_allocate_device(dev);
	if (IS_ERR(prtc->rtc))
		return PTR_ERR(prtc->rtc);
	prtc->rtc->ops = &apple_pmu_rtc_ops;
	prtc->rtc->range_min = S64_MIN >> (RTC_SEC_SHIFT + (64 - RTC_BITS));
	prtc->rtc->range_max = S64_MAX >> (RTC_SEC_SHIFT + (64 - RTC_BITS));

	nirq = platform_irq_count(pdev);
	if (nirq < 0)
		return nirq;
	if (nirq > 1)
		return -EINVAL;
	if (!nirq) {
		clear_bit(RTC_FEATURE_ALARM, prtc->rtc->features);
	} else {
		irq = platform_get_irq(pdev, 0);
		if (irq < 0)
			return irq;
		ret = apple_pmu_rtc_program(prtc, false);
		if (ret)
			return ret;
		ret = devm_request_threaded_irq(dev, irq, NULL, apple_pmu_rtc_irq,
						IRQF_ONESHOT, dev_name(dev), prtc);
		if (ret)
			return ret;
		ret = device_init_wakeup(dev, true);
		if (ret)
			return ret;
		ret = devm_add_action_or_reset(dev, apple_pmu_rtc_clear_wake, dev);
		if (ret)
			return ret;
		ret = dev_pm_set_wake_irq(dev, irq);
		if (ret)
			return ret;
		ret = devm_add_action_or_reset(dev, apple_pmu_rtc_stop, prtc);
		if (ret)
			return ret;
	}

	return devm_rtc_register_device(prtc->rtc);
}

static const struct of_device_id apple_pmu_rtc_of_match[] = {
	{ .compatible = "apple,abbey-pmu-rtc" },
	{}
};
MODULE_DEVICE_TABLE(of, apple_pmu_rtc_of_match);

static struct platform_driver apple_pmu_rtc_driver = {
	.driver = {
		.name = "apple-spmi-pmu-rtc",
		.of_match_table = apple_pmu_rtc_of_match,
	},
	.probe = apple_pmu_rtc_probe,
};
module_platform_driver(apple_pmu_rtc_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Apple SPMI PMU RTC with wake alarm");
