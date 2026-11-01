// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* Workload-scoped ANE clock and memory bandwidth requests. */
#include <linux/interconnect.h>
#include <linux/jiffies.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/pm_opp.h>
#include <linux/pm_runtime.h>
#include <linux/soc/apple/pmp-report.h>
#include <linux/sysfs.h>

#include "ane.h"

/* Amortize clock transitions across a stream of short submissions. */
#define ANE_PERF_IDLE_MS 32

static int ane_perf_drop(struct ane_device *ane)
{
	struct ane_perf *p = &ane->perf;
	int ret;

	if (p->clock_held) {
		ret = apple_pmp_report_ane_perf(p->report, ane->dev, 0, &p->clock_held);
		p->level = 0;
		if (ret)
			return ret;
	}
	if (p->voted) {
		ret = icc_set_bw(p->memory, 0, 0);
		if (ret)
			return ret;
		p->voted = false;
	}
	return 0;
}

static void ane_perf_idle(struct work_struct *work)
{
	struct ane_device *ane = container_of(to_delayed_work(work),
					    struct ane_device, perf.idle);
	struct ane_perf *p = &ane->perf;

	mutex_lock(&p->lock);
	if (!p->busy && !atomic_read(&ane->wedged)) {
		p->error = ane_perf_drop(ane);
		if (!p->error && p->pinned) {
			p->pinned = false;
			pm_runtime_put_autosuspend(ane->dev);
		}
		if (p->error && ++p->release_retries < 3)
			mod_delayed_work(system_dfl_wq, &p->idle,
					 msecs_to_jiffies(ANE_PERF_IDLE_MS));
	}
	mutex_unlock(&p->lock);
}

int ane_perf_begin(struct ane_device *ane)
{
	struct ane_perf *p = &ane->perf;
	int ret = 0;

	if (p->cpu_boost)
		ane_boost_begin(ane);

	/* The submit ioctl already holds a runtime PM reference. */
	mutex_lock(&p->lock);
	p->busy = true;
	if (p->error) {
		ret = ane_perf_drop(ane);
		if (ret)
			goto out;
	}
	if (p->memory && !p->voted) {
		ret = icc_set_bw(p->memory, p->avg_bw, p->peak_bw);
		if (ret)
			goto out;
		p->voted = true;
		WRITE_ONCE(p->raises, p->raises + 1);
	}
	if (p->report && !p->clock_held) {
		ret = apple_pmp_report_ane_perf(p->report, ane->dev, p->high_level,
						&p->clock_held);
		if (ret)
			goto out;
		p->level = p->high_level;
		WRITE_ONCE(p->raises, p->raises + 1);
	}
out:
	p->error = ret;
	/* Keep the windows powered until delayed release, including errors. */
	if (!p->pinned && (p->clock_held || p->voted)) {
		pm_runtime_get_noresume(ane->dev);
		p->pinned = true;
	}
	mutex_unlock(&p->lock);
	return ret;
}

void ane_perf_end(struct ane_device *ane)
{
	struct ane_perf *p = &ane->perf;

	if (p->cpu_boost)
		ane_boost_end(ane);

	mutex_lock(&p->lock);
	p->busy = false;
	p->release_retries = 0;
	mod_delayed_work(system_dfl_wq, &p->idle, msecs_to_jiffies(ANE_PERF_IDLE_MS));
	mutex_unlock(&p->lock);
}

static ssize_t performance_level_show(struct device *dev,
				      struct device_attribute *attr, char *buf)
{
	struct ane_device *ane = dev_get_drvdata(dev);
	struct ane_perf *p = &ane->perf;
	ssize_t ret;

	mutex_lock(&p->lock);
	ret = p->report ? sysfs_emit(buf, "%u\n", p->level) :
			  sysfs_emit(buf, "unsupported\n");
	mutex_unlock(&p->lock);
	return ret;
}
static DEVICE_ATTR_RO(performance_level);

static ssize_t memory_vote_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct ane_device *ane = dev_get_drvdata(dev);
	struct ane_perf *p = &ane->perf;
	ssize_t ret;

	mutex_lock(&p->lock);
	ret = p->memory ? sysfs_emit(buf, "%u %u\n",
				    p->voted ? p->avg_bw : 0,
				    p->voted ? p->peak_bw : 0) :
			 sysfs_emit(buf, "unsupported\n");
	mutex_unlock(&p->lock);
	return ret;
}
static DEVICE_ATTR_RO(memory_vote);

static ssize_t performance_error_show(struct device *dev,
				      struct device_attribute *attr, char *buf)
{
	struct ane_device *ane = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%d\n", READ_ONCE(ane->perf.error));
}
static DEVICE_ATTR_RO(performance_error);

static struct attribute *ane_perf_attrs[] = {
	&dev_attr_performance_level.attr,
	&dev_attr_memory_vote.attr,
	&dev_attr_performance_error.attr,
	NULL,
};

static const struct attribute_group ane_perf_group = { .attrs = ane_perf_attrs };

int ane_perf_init(struct ane_device *ane, unsigned int high_level)
{
	struct ane_perf *p = &ane->perf;
	struct platform_device *supplier;
	struct device_link *link;
	struct dev_pm_opp *opp;
	int ret;

	mutex_init(&p->lock);
	p->initialized = true;
	INIT_DELAYED_WORK(&p->idle, ane_perf_idle);
	p->high_level = high_level;
	p->report = of_parse_phandle(ane->dev->of_node, "apple,performance-controller", 0);
	if (p->report) {
		if (!high_level)
			return -EINVAL;
		supplier = of_find_device_by_node(p->report);
		if (!supplier)
			return -EPROBE_DEFER;
		link = device_link_add(ane->dev, &supplier->dev,
				       DL_FLAG_AUTOREMOVE_CONSUMER);
		put_device(&supplier->dev);
		if (!link)
			return -ENOMEM;
		ret = apple_pmp_report_ane_available(p->report);
		if (ret)
			return ret;
	}

	if (of_property_present(ane->dev->of_node, "interconnects")) {
		p->memory = devm_of_icc_get(ane->dev, "memory");
		if (IS_ERR(p->memory))
			return PTR_ERR(p->memory);
		if (!p->memory)
			return -ENODEV;
		ret = devm_pm_opp_of_add_table(ane->dev);
		if (ret)
			return ret;
		opp = dev_pm_opp_find_level_exact(ane->dev, high_level);
		if (IS_ERR(opp))
			return PTR_ERR(opp);
		p->avg_bw = dev_pm_opp_get_bw(opp, false, 0);
		p->peak_bw = dev_pm_opp_get_bw(opp, true, 0);
		dev_pm_opp_put(opp);
		if (!p->peak_bw || p->avg_bw > p->peak_bw)
			return -EINVAL;
	}

	ret = devm_device_add_group(ane->dev, &ane_perf_group);
	if (ret)
		return ret;

	/*
	 * Without a performance controller the engine runs at its boot
	 * clock, and the CPU cluster boost is what keeps bandwidth-bound
	 * programs and the submit tail fast. Keep it until the DT binds a
	 * controller.
	 */
	if (!p->report) {
		ret = ane_boost_init(ane);
		if (ret)
			return ret;
		p->cpu_boost = true;
		dev_info(ane->dev, "no performance controller; keeping the CPU frequency boost\n");
	}
	return 0;
}

int ane_perf_exit(struct ane_device *ane)
{
	struct ane_perf *p = &ane->perf;
	int ret;

	if (!p->initialized)
		return 0;
	if (p->cpu_boost) {
		ane_boost_exit(ane);
		p->cpu_boost = false;
	}
	cancel_delayed_work_sync(&p->idle);
	mutex_lock(&p->lock);
	p->error = ane_perf_drop(ane);
	if (p->error)
		dev_err(ane->dev, "performance release failed during removal: %d\n",
			p->error);
	if (p->pinned && !p->error) {
		pm_runtime_put_noidle(ane->dev);
		p->pinned = false;
	}
	ret = p->error;
	mutex_unlock(&p->lock);
	if (ret)
		return ret;
	of_node_put(p->report);
	p->report = NULL;
	return 0;
}

int ane_perf_suspend(struct ane_device *ane)
{
	struct ane_perf *p = &ane->perf;
	int ret;

	cancel_delayed_work_sync(&p->idle);
	mutex_lock(&p->lock);
	if (p->busy && !ane->recovering) {
		ret = -EBUSY;
		goto out;
	}
	ret = ane_perf_drop(ane);
	p->error = ret;
	if (!ret && p->pinned) {
		pm_runtime_put_noidle(ane->dev);
		p->pinned = false;
	}
out:
	mutex_unlock(&p->lock);
	return ret;
}
