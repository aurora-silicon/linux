// SPDX-License-Identifier: GPL-2.0

#include <linux/device.h>

__rust_helper int rust_helper_devm_add_action(struct device *dev,
					      void (*action)(void *),
					      void *data)
{
	return devm_add_action(dev, action, data);
}

__rust_helper int rust_helper_devm_add_action_or_reset(struct device *dev,
						       void (*action)(void *),
						       void *data)
{
	return devm_add_action_or_reset(dev, action, data);
}

__rust_helper void *rust_helper_dev_get_drvdata(const struct device *dev)
{
	return dev_get_drvdata(dev);
}

__rust_helper void rust_helper_dev_set_drvdata(struct device *dev, void *data)
{
	dev_set_drvdata(dev, data);
}

__rust_helper const char *rust_helper_dev_name(const struct device *dev)
{
	return dev_name(dev);
}

/* Nonblocking publication check: a worker must not block its parent's unbind. */
__rust_helper bool rust_helper_device_probe_data_ready(struct device *dev)
{
	bool ready;

	if (!device_trylock(dev))
		return false;
	ready = device_is_bound(dev) && dev_get_drvdata(dev);
	device_unlock(dev);
	return ready;
}

/* Keep a registered DMA owner from binding after its transport is removed. */
__rust_helper void rust_helper_device_quarantine(struct device *dev)
{
	device_lock(dev);
	dev_clear_ready_to_probe(dev);
	device_unlock(dev);
}

/* Probe/unbind callbacks already hold the parent device lock. */
__rust_helper void rust_helper_device_quarantine_locked(struct device *dev)
{
	lockdep_assert_held(&dev->mutex);
	dev_clear_ready_to_probe(dev);
}
