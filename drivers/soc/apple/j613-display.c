// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* Allocation-free, built-in arbitration of J613 coldplug and PD fallback. */
#include <linux/device.h>
#include <linux/device/driver.h>
#include <linux/export.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/property.h>
#include <linux/workqueue.h>
#include <linux/soc/apple/j613-display.h>
#include <video/nomodeset.h>

enum j613_display_state {
	J613_DISPLAY_PENDING,
	J613_DISPLAY_HANDOFF,
	J613_DISPLAY_FALLBACK,
};

static DEFINE_MUTEX(j613_display_lock);
static enum j613_display_state j613_display_state;
static bool j613_display_expired;

/* Two J613 PD controllers; pin only until the one-shot retry has run. */
static struct device *j613_display_consumers[2];
static bool j613_display_timer_started;

static void j613_display_timeout(struct work_struct *work)
{
	struct device *consumers[2];
	unsigned int i;

	mutex_lock(&j613_display_lock);
	j613_display_expired = true;
	if (j613_display_state == J613_DISPLAY_PENDING)
		j613_display_state = J613_DISPLAY_FALLBACK;
	for (i = 0; i < ARRAY_SIZE(consumers); i++) {
		consumers[i] = j613_display_consumers[i];
		j613_display_consumers[i] = NULL;
	}
	mutex_unlock(&j613_display_lock);
	for (i = 0; i < ARRAY_SIZE(consumers); i++) {
		if (!consumers[i])
			continue;
		/* Bound devices stay bound; an unbound controller retries fallback. */
		if (device_is_registered(consumers[i])) {
			int ret = device_attach(consumers[i]);

			if (ret < 0 && ret != -EPROBE_DEFER)
				dev_dbg(consumers[i], "PD fallback retry: %d\n", ret);
		}
		put_device(consumers[i]);
	}
}

static DECLARE_DELAYED_WORK(j613_display_timeout_work, j613_display_timeout);

/* Held through the changeset: a late module must not reopen fallback routes. */
bool apple_j613_display_begin(void)
{
	if (!of_machine_is_compatible("apple,j613"))
		return true;

	mutex_lock(&j613_display_lock);
	if (j613_display_state == J613_DISPLAY_FALLBACK) {
		mutex_unlock(&j613_display_lock);
		return false;
	}
	return true;
}
EXPORT_SYMBOL_GPL(apple_j613_display_begin);

void apple_j613_display_end(int ret)
{
	if (!of_machine_is_compatible("apple,j613"))
		return;

	j613_display_state = ret ? J613_DISPLAY_FALLBACK : J613_DISPLAY_HANDOFF;
	mutex_unlock(&j613_display_lock);
}
EXPORT_SYMBOL_GPL(apple_j613_display_end);

void apple_j613_display_fail(void)
{
	if (!of_machine_is_compatible("apple,j613"))
		return;

	mutex_lock(&j613_display_lock);
	j613_display_state = J613_DISPLAY_FALLBACK;
	mutex_unlock(&j613_display_lock);
}
EXPORT_SYMBOL_GPL(apple_j613_display_fail);

bool apple_j613_display_pending(struct device *dev)
{
	struct device_node *dcp;
	bool pending;
	unsigned int i;

	if (!of_machine_is_compatible("apple,j613"))
		return false;
	dcp = of_find_node_by_path("dcp");
	pending = dcp && of_property_present(dcp, "apple,j613-25g83-profile");
	of_node_put(dcp);
	if (!pending)
		return false;

	mutex_lock(&j613_display_lock);
	/* Also bound module absence when the global probe timeout is disabled. */
	if (!j613_display_expired) {
		for (i = 0; i < ARRAY_SIZE(j613_display_consumers); i++) {
			if (j613_display_consumers[i] == dev)
				break;
			if (!j613_display_consumers[i]) {
				j613_display_consumers[i] = get_device(dev);
				break;
			}
		}
		if (!j613_display_timer_started) {
			j613_display_timer_started = true;
			schedule_delayed_work(&j613_display_timeout_work, 10 * HZ);
		}
	}
	j613_display_expired |= driver_deferred_probe_check_state(dev) != -EPROBE_DEFER;
	if (!IS_ENABLED(CONFIG_DRM_APPLE) || video_firmware_drivers_only() ||
	    (j613_display_expired && j613_display_state == J613_DISPLAY_PENDING))
		j613_display_state = J613_DISPLAY_FALLBACK;
	pending = j613_display_state == J613_DISPLAY_PENDING;
	mutex_unlock(&j613_display_lock);
	return pending;
}
EXPORT_SYMBOL_GPL(apple_j613_display_pending);

/* Ignore only this profile's DCP subtree; PHY providers remain mandatory. */
bool apple_j613_display_skip(const struct fwnode_handle *node, bool missing)
{
	struct device_node *dcp, *ext;
	struct fwnode_handle *parent;
	bool fallback, skip = false;

	if (!of_machine_is_compatible("apple,j613"))
		return false;
	mutex_lock(&j613_display_lock);
	fallback = j613_display_state == J613_DISPLAY_FALLBACK ||
		   (missing && j613_display_expired);
	mutex_unlock(&j613_display_lock);
	if (!fallback)
		return false;

	dcp = of_find_node_by_path("dcp");
	if (!dcp || !of_device_is_compatible(dcp, "apple,t8122-dcp") ||
	    !of_property_present(dcp, "apple,j613-25g83-profile")) {
		of_node_put(dcp);
		return false;
	}

	/* The Type-C routes belong to this sibling, not to the marked panel. */
	ext = of_find_node_by_path("dcpext");
	if (!ext)
		ext = of_find_node_by_path("dcpext0");
	if (ext && !of_device_is_compatible(ext, "apple,t8122-dcpext")) {
		of_node_put(ext);
		ext = NULL;
	}

	fwnode_for_each_parent_node(node, parent) {
		if (parent == of_fwnode_handle(dcp) ||
		    parent == of_fwnode_handle(ext)) {
			fwnode_handle_put(parent);
			skip = true;
			break;
		}
	}
	of_node_put(ext);
	of_node_put(dcp);
	if (skip) {
		mutex_lock(&j613_display_lock);
		j613_display_state = J613_DISPLAY_FALLBACK;
		mutex_unlock(&j613_display_lock);
	}
	return skip;
}
EXPORT_SYMBOL_GPL(apple_j613_display_skip);
