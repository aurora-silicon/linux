// SPDX-License-Identifier: GPL-2.0-only
/*
 * The actual dcp-dp-audio late sleep callback and dev_pm_ops, and the DCP's
 * dcp_sleep_keeps_firmware(), against an OF graph and driver-core model that
 * counts every node and device reference and checks the device lock.
 */
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

/* The harness builds with -Wno-unused-function. */
#define __maybe_unused
#define pm_sleep_ptr(fn) (fn)
#define EXPORT_SYMBOL_GPL(sym)

/* ACTUAL_PM_MACROS */

struct device_node {
	int refs;
	bool available;
};

struct device_driver {
	int unused;
};

struct device {
	struct device_node *of_node;
	int refs;
	const struct device_driver *driver;
	void *drvdata;
	bool locked, set_wakeup_path;
};

/* What the DCP's suspend left: see the sites setting it in dcp.c. */
struct apple_dcp {
	bool sleep_fw_runs;
};

struct platform_device {
	struct device dev;
};

struct dev_pm_ops {
	int (*prepare)(struct device *dev);
	int (*suspend)(struct device *dev);
	int (*resume)(struct device *dev);
	int (*freeze)(struct device *dev);
	int (*thaw)(struct device *dev);
	int (*poweroff)(struct device *dev);
	int (*restore)(struct device *dev);
	int (*suspend_late)(struct device *dev);
	int (*resume_early)(struct device *dev);
	int (*freeze_late)(struct device *dev);
	int (*thaw_early)(struct device *dev);
	int (*poweroff_late)(struct device *dev);
	int (*restore_early)(struct device *dev);
	int (*runtime_suspend)(struct device *dev);
	int (*runtime_resume)(struct device *dev);
	int (*runtime_idle)(struct device *dev);
};

/* The model: the dpaudio node, its endpoint, the DCP node and device. */
static struct device_node audio_node, endpoint_node, dcp_node;
static struct platform_device dcp_pdev;
static const struct device_driver dcp_driver;
static struct apple_dcp dcp_state;
static struct device audio_dev = { .of_node = &audio_node };
static bool have_endpoint, have_remote, have_pdev;
static int force_suspend_ret, force_suspend_calls, find_calls;

static void get_node(struct device_node *np)
{
	assert(np->refs >= 0);
	np->refs++;
}

static void of_node_put(struct device_node *np)
{
	if (!np)
		return;
	assert(np == &endpoint_node || np == &dcp_node);
	assert(np->refs > 0);
	np->refs--;
}

static struct device_node *of_graph_get_endpoint_by_regs(const struct device_node *parent,
							  int port, int reg)
{
	assert(parent == &audio_node && port == 0 && reg == 0);
	if (!have_endpoint)
		return NULL;
	get_node(&endpoint_node);
	return &endpoint_node;
}

static struct device_node *of_graph_get_remote_port_parent(const struct device_node *np)
{
	assert(np == &endpoint_node && endpoint_node.refs > 0);
	if (!have_remote)
		return NULL;
	get_node(&dcp_node);
	return &dcp_node;
}

static bool of_device_is_available(const struct device_node *np)
{
	assert(np == &dcp_node && dcp_node.refs > 0);
	return np->available;
}

static struct platform_device *of_find_device_by_node(struct device_node *np)
{
	assert(np == &dcp_node && dcp_node.refs > 0);
	find_calls++;
	if (!have_pdev)
		return NULL;
	dcp_pdev.dev.refs++;
	return &dcp_pdev;
}

static void put_device(struct device *dev)
{
	assert(dev == &dcp_pdev.dev && dev->refs > 0);
	dev->refs--;
}

/* The driver core's contract: driver and driver data are read under the lock. */
static void device_lock(struct device *dev)
{
	assert(dev == &dcp_pdev.dev && dev->refs > 0 && !dev->locked);
	dev->locked = true;
}

static void device_unlock(struct device *dev)
{
	assert(dev == &dcp_pdev.dev && dev->locked);
	dev->locked = false;
}

static void *dev_get_drvdata(const struct device *dev)
{
	assert(dev == &dcp_pdev.dev && dev->locked && dev->driver);
	return dev->drvdata;
}

static void device_set_wakeup_path(struct device *dev)
{
	assert(dev == &audio_dev);
	dev->set_wakeup_path = true;
}

static int pm_runtime_force_suspend(struct device *dev)
{
	assert(dev == &audio_dev);
	force_suspend_calls++;
	return force_suspend_ret;
}

static int pm_runtime_force_resume(struct device *dev)
{
	(void)dev;
	return 0;
}

/* The rest of the driver's callbacks only have to be distinct symbols. */
static int dcpaud_pm_prepare(struct device *dev)
{
	(void)dev;
	return 1;
}

static int dcpaud_system_suspend(struct device *dev)
{
	(void)dev;
	return 2;
}

static int dcpaud_suspend(struct device *dev)
{
	(void)dev;
	return 3;
}

static int dcpaud_resume(struct device *dev)
{
	(void)dev;
	return 4;
}

/* ACTUAL_DCP_FUNCTION */

/* ACTUAL_FUNCTION */

/* ACTUAL_PM_OPS */

int main(void)
{
	unsigned int cases = 0, held = 0;

	for (unsigned int bits = 0; bits < 128; bits++) {
		bool expect;
		int ret;

		have_endpoint = bits & 1;
		have_remote = bits & 2;
		dcp_node.available = bits & 4;
		have_pdev = bits & 8;
		dcp_pdev.dev.driver = bits & 16 ? &dcp_driver : NULL;
		dcp_pdev.dev.drvdata = bits & 16 ? &dcp_state : NULL;
		dcp_state.sleep_fw_runs = bits & 32;
		dcp_pdev.dev.locked = false;
		force_suspend_ret = bits & 64 ? -EBUSY : 0;
		endpoint_node.refs = dcp_node.refs = dcp_pdev.dev.refs = 0;
		audio_dev.set_wakeup_path = false;
		force_suspend_calls = find_calls = 0;

		/* The PM core calls the late slot for every system sleep. */
		ret = dcpaud_pm_ops.suspend_late(&audio_dev);

		expect = have_endpoint && have_remote && dcp_node.available && have_pdev &&
			 (!dcp_pdev.dev.driver || dcp_state.sleep_fw_runs);
		if (audio_dev.set_wakeup_path != expect)
			fprintf(stderr, "case %#x: wakeup path %d, expected %d\n", bits,
				audio_dev.set_wakeup_path, expect);
		assert(audio_dev.set_wakeup_path == expect);
		/* Every reference taken is dropped, whatever the topology. */
		assert(!endpoint_node.refs && !dcp_node.refs && !dcp_pdev.dev.refs);
		assert(!dcp_pdev.dev.locked);
		/* An unavailable or missing DCP node is never looked up. */
		if (!(have_endpoint && have_remote && dcp_node.available))
			assert(!find_calls);
		/* The runtime PM force-suspend always runs and its result is returned. */
		assert(force_suspend_calls == 1);
		assert(ret == force_suspend_ret);
		held += expect;
		cases++;
	}

	/* The wiring: late suspend and its siblings, then the rest unchanged. */
#ifdef HAVE_SUSPEND_LATE
	assert(dcpaud_pm_ops.suspend_late == dcpaud_suspend_late);
	assert(dcpaud_pm_ops.freeze_late == dcpaud_suspend_late);
	assert(dcpaud_pm_ops.poweroff_late == dcpaud_suspend_late);
#endif
	assert(dcpaud_pm_ops.resume_early == pm_runtime_force_resume);
	assert(dcpaud_pm_ops.thaw_early == pm_runtime_force_resume);
	assert(dcpaud_pm_ops.restore_early == pm_runtime_force_resume);
	assert(dcpaud_pm_ops.prepare == dcpaud_pm_prepare);
	assert(dcpaud_pm_ops.suspend == dcpaud_system_suspend);
	assert(!dcpaud_pm_ops.resume);
	assert(dcpaud_pm_ops.runtime_suspend == dcpaud_suspend);
	assert(dcpaud_pm_ops.runtime_resume == dcpaud_resume);

	printf("%u late-suspend cases passed, %u hold the dp-audio domain; pm ops wired\n",
	       cases, held);
	return 0;
}
