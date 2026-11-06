// SPDX-License-Identifier: GPL-2.0-only OR MIT

#include <linux/debugfs.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/thermal.h>
#include <asm/cacheflush.h>

#include "cooling.h"

struct asahi_gpu_cooling {
	struct device *dev;
	struct mutex lock;
	struct asahi_gpu_cap cap;
	struct asahi_gpu_power_words words;
	u32 ppm_budget_mw;
	struct thermal_cooling_device *cdev;
	struct device_attribute telemetry;
	struct dentry *debugfs;
};

static void clean_word(u32 *word)
{
	dcache_clean_poc((unsigned long)word, (unsigned long)(word + 1));
}

/* No mailbox or ACK exists. All pointers are checked before registration.
 * The cooling mutex owns every ceiling requester and both power publications;
 * it never takes the GPU mutex or waits for GPU work. RegionC is write-back,
 * so a WC store barrier alone would not publish this word to firmware.
 */
static int asahi_gpu_publish_cap(void *context, unsigned int mw)
{
	struct asahi_gpu_cooling *cooling = context;
	struct asahi_gpu_power_words *words = &cooling->words;
	u32 ppm = min(mw, cooling->ppm_budget_mw);

	lockdep_assert_held(&cooling->lock);
	/* Independent absolute-power inputs. PPM retains its own budget, bounded
	 * by the composed ceiling; neither word is a percent or an effort floor.
	 */
	WRITE_ONCE(*words->ppm_target_mw, ppm);
	WRITE_ONCE(*words->ppm_enabled, 1);
	WRITE_ONCE(*words->max_power_mw, mw);
	clean_word(words->max_power_mw);
	wmb();
	return 0;
}

int asahi_gpu_cooling_request(struct asahi_gpu_cooling *cooling,
			     enum asahi_gpu_cap_source source, u32 mw)
{
	/* Thermal requests also need a state; only the cooling callback sets it. */
	if (source <= ASAHI_GPU_CAP_THERMAL || source >= ASAHI_GPU_CAP_SOURCES)
		return -EINVAL;
	guard(mutex)(&cooling->lock);
	if (mw < cooling->cap.floor_mw || mw > cooling->cap.max_mw)
		return -ERANGE;
	return asahi_gpu_cap_request(&cooling->cap, source, mw,
				     asahi_gpu_publish_cap, cooling);
}

static int asahi_gpu_cooling_max(struct thermal_cooling_device *cdev,
				 unsigned long *state)
{
	*state = ASAHI_GPU_COOLING_MAX;
	return 0;
}

static int asahi_gpu_cooling_get(struct thermal_cooling_device *cdev,
				 unsigned long *state)
{
	struct asahi_gpu_cooling *cooling = cdev->devdata;

	guard(mutex)(&cooling->lock);
	*state = cooling->cap.requested_state;
	return 0;
}

static int asahi_gpu_cooling_set(struct thermal_cooling_device *cdev,
				 unsigned long state)
{
	struct asahi_gpu_cooling *cooling = cdev->devdata;

	if (asahi_gpu_cap_percent(state) < 0)
		return -EINVAL;
	guard(mutex)(&cooling->lock);
	return asahi_gpu_cap_update(&cooling->cap, state, asahi_gpu_publish_cap, cooling);
}

static const struct thermal_cooling_device_ops asahi_gpu_cooling_ops = {
	.get_max_state = asahi_gpu_cooling_max,
	.get_cur_state = asahi_gpu_cooling_get,
	.set_cur_state = asahi_gpu_cooling_set,
};

static int ceiling_show(struct seq_file *m, void *unused)
{
	struct asahi_gpu_cooling *cooling = m->private;
	unsigned int i;

	guard(mutex)(&cooling->lock);
	seq_printf(m, "requested_state %u\nrequested_ceiling_mw %u\napplied_ceiling_mw %u\n"
		   "region_c_readback_mw %u\nppm_target_readback_mw %u\nppm_enabled %u\n"
		   "initialized_ppm_budget_mw %u\n"
		   "initialized_max_mw %u\nprovisional_floor_mw %u\n"
		   "achieved_ceiling_mw unsupported\n",
		   cooling->cap.requested_state, asahi_gpu_cap_effective(&cooling->cap),
		   cooling->cap.applied_mw, READ_ONCE(*cooling->words.max_power_mw),
		   READ_ONCE(*cooling->words.ppm_target_mw),
		   READ_ONCE(*cooling->words.ppm_enabled), cooling->ppm_budget_mw,
		   cooling->cap.max_mw,
		   cooling->cap.floor_mw);
	for (i = 0; i < ASAHI_GPU_CAP_SOURCES; i++)
		seq_printf(m, "request_%u_mw %u\n", i, cooling->cap.requests[i]);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(ceiling);

static ssize_t gpu_power_ceiling_show(struct device *dev,
				    struct device_attribute *attr, char *buf)
{
	struct asahi_gpu_cooling *cooling =
		container_of(attr, struct asahi_gpu_cooling, telemetry);

	guard(mutex)(&cooling->lock);
	return sysfs_emit(buf, "requested_state %u\nrequested_ceiling_mw %u\n"
			  "applied_ceiling_mw %u\nprovisional_floor_mw %u\n"
			  "achieved_ceiling_mw unsupported\n",
			  cooling->cap.requested_state,
			  asahi_gpu_cap_effective(&cooling->cap), cooling->cap.applied_mw,
			  cooling->cap.floor_mw);
}

struct asahi_gpu_cooling *asahi_gpu_cooling_register(struct device *dev,
					const struct asahi_gpu_power_words *words)
{
	struct asahi_gpu_cooling *cooling;
	char *name;
	int ret;

	if (!dev || !dev->of_node || !words || !words->max_power_mw ||
	    !words->ppm_enabled || !words->ppm_target_mw ||
	    !IS_ALIGNED((unsigned long)words->max_power_mw, sizeof(u32)) ||
	    !IS_ALIGNED((unsigned long)words->ppm_enabled, sizeof(u32)) ||
	    !IS_ALIGNED((unsigned long)words->ppm_target_mw, sizeof(u32)) ||
	    words->max_power_mw == words->ppm_enabled ||
	    words->max_power_mw == words->ppm_target_mw ||
	    words->ppm_enabled == words->ppm_target_mw)
		return ERR_PTR(-EINVAL);
	cooling = kzalloc_obj(*cooling);
	if (!cooling)
		return ERR_PTR(-ENOMEM);
	cooling->dev = dev;
	cooling->words = *words;
	mutex_init(&cooling->lock);
	ret = asahi_gpu_cap_init(&cooling->cap, READ_ONCE(*words->max_power_mw));
	if (ret) {
		ret = -EINVAL;
		goto free;
	}
	cooling->ppm_budget_mw = cooling->cap.max_mw;
	if (READ_ONCE(*words->ppm_enabled) && READ_ONCE(*words->ppm_target_mw))
		cooling->ppm_budget_mw = READ_ONCE(*words->ppm_target_mw);
	if (cooling->ppm_budget_mw < cooling->cap.floor_mw) {
		ret = -ERANGE;
		goto free;
	}
	cooling->cap.requests[ASAHI_GPU_CAP_SYSTEM] =
		min(cooling->cap.max_mw, cooling->ppm_budget_mw);
	mutex_lock(&cooling->lock);
	ret = asahi_gpu_cap_update(&cooling->cap, ASAHI_GPU_COOLING_MAX,
				   asahi_gpu_publish_cap, cooling);
	mutex_unlock(&cooling->lock);
	if (ret)
		goto free;
	cooling->cdev = thermal_of_cooling_device_register(dev->of_node, 0,
				"aurora-gpu-power", cooling, &asahi_gpu_cooling_ops);
	if (IS_ERR(cooling->cdev)) {
		ret = PTR_ERR(cooling->cdev);
		goto free;
	}
	sysfs_attr_init(&cooling->telemetry.attr);
	cooling->telemetry.attr.name = "gpu_power_ceiling";
	cooling->telemetry.attr.mode = 0444;
	cooling->telemetry.show = gpu_power_ceiling_show;
	ret = device_create_file(dev, &cooling->telemetry);
	if (ret) {
		thermal_cooling_device_unregister(cooling->cdev);
		goto free;
	}
	name = kasprintf(GFP_KERNEL, "asahi-%s-power", dev_name(dev));
	if (name) {
		cooling->debugfs = debugfs_create_dir(name, NULL);
		debugfs_create_file("ceiling", 0444, cooling->debugfs, cooling, &ceiling_fops);
		kfree(name);
	}
	return cooling;
free:
	kfree(cooling);
	return ERR_PTR(ret);
}

void asahi_gpu_cooling_rearm(struct asahi_gpu_cooling *cooling)
{
	guard(mutex)(&cooling->lock);
	/* Preserve every requester across recovery, before consumption resumes. */
	asahi_gpu_cap_rearm(&cooling->cap, asahi_gpu_publish_cap, cooling);
}

void asahi_gpu_cooling_unregister(struct asahi_gpu_cooling *cooling)
{
	debugfs_remove_recursive(cooling->debugfs);
	device_remove_file(cooling->dev, &cooling->telemetry);
	thermal_cooling_device_unregister(cooling->cdev);
	kfree(cooling);
}
