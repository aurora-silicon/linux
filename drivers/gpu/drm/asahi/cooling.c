// SPDX-License-Identifier: GPL-2.0-only OR MIT

#include <linux/device.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/thermal.h>
#include <linux/string.h>

#include "cooling.h"
#include "cooling_policy.h"

struct asahi_gpu_cooling {
	struct device *dev;
	struct mutex lock;
	struct asahi_gpu_cap cap;
	u32 *mirrors[3];
	struct thermal_cooling_device *cdev;
	struct device_attribute telemetry;
};

/* No mailbox or ACK exists. The caller validated all destinations before any
 * write. The dedicated WC words are never modified by any other host path.
 * One mutex covers all three stores, independently of the GPU device lock.
 */
static int asahi_gpu_publish_cap(void *context, unsigned int percent)
{
	struct asahi_gpu_cooling *cooling = context;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(cooling->mirrors); i++)
		WRITE_ONCE(*cooling->mirrors[i], percent);
	/* Complete all three WC stores before reporting host publication. */
	wmb();
	return 0;
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
	return asahi_gpu_cap_update(&cooling->cap, state, 100,
				   asahi_gpu_publish_cap, cooling);
}

static const struct thermal_cooling_device_ops asahi_gpu_cooling_ops = {
	.get_max_state = asahi_gpu_cooling_max,
	.get_cur_state = asahi_gpu_cooling_get,
	.set_cur_state = asahi_gpu_cooling_set,
};

static ssize_t gpu_power_ceiling_show(struct device *dev,
				    struct device_attribute *attr, char *buf)
{
	struct asahi_gpu_cooling *cooling =
		container_of(attr, struct asahi_gpu_cooling, telemetry);

	guard(mutex)(&cooling->lock);
	return sysfs_emit(buf, "requested_percent %u\npublished_percent %u\n"
			  "applied_percent unsupported\nachieved_percent unsupported\n"
			  "qualification_backstop_percent %u\n",
			  100 - cooling->cap.requested_state,
			  cooling->cap.published_percent, ASAHI_GPU_PREARM_PERCENT);
}

struct asahi_gpu_cooling *asahi_gpu_cooling_register(struct device *dev,
						  u32 * const mirrors[3])
{
	struct asahi_gpu_cooling *cooling;
	int ret, i;

	if (!dev || !dev->of_node || !mirrors)
		return ERR_PTR(-EINVAL);
	for (i = 0; i < 3; i++) {
		if (!mirrors[i] || !IS_ALIGNED((unsigned long)mirrors[i], sizeof(u32)))
			return ERR_PTR(-EINVAL);
		if (mirrors[i] == mirrors[(i + 1) % 3])
			return ERR_PTR(-EINVAL);
	}
	cooling = kzalloc_obj(*cooling);
	if (!cooling)
		return ERR_PTR(-ENOMEM);
	cooling->dev = dev;
	mutex_init(&cooling->lock);
	memcpy(cooling->mirrors, mirrors, sizeof(cooling->mirrors));
	asahi_gpu_cap_init(&cooling->cap);
	ret = asahi_gpu_cap_update(&cooling->cap, 0, 100,
				   asahi_gpu_publish_cap, cooling);
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
	return cooling;
free:
	kfree(cooling);
	return ERR_PTR(ret);
}

void asahi_gpu_cooling_rearm(struct asahi_gpu_cooling *cooling)
{
	guard(mutex)(&cooling->lock);
	/* Keep a stronger governor request across recovery. */
	asahi_gpu_cap_update(&cooling->cap, cooling->cap.requested_state, 100,
			     asahi_gpu_publish_cap, cooling);
}

void asahi_gpu_cooling_unregister(struct asahi_gpu_cooling *cooling)
{
	device_remove_file(cooling->dev, &cooling->telemetry);
	/* Join all callbacks before Rust can release the firmware mapping. */
	thermal_cooling_device_unregister(cooling->cdev);
	kfree(cooling);
}
