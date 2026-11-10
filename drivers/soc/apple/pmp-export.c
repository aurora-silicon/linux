// SPDX-License-Identifier: GPL-2.0-only OR MIT
#include <linux/atomic.h>
#include <linux/device.h>
#include <linux/export.h>
#include <linux/io.h>
#include <linux/limits.h>
#include <linux/mutex.h>
#include <linux/notifier.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/pm.h>
#include <linux/soc/apple/pmp.h>
#include <linux/soc/apple/pmp-display.h>
#include <linux/soc/apple/pmp-report.h>
#include <linux/soc/apple/rtkit.h>
#include <linux/soc/apple/sleep.h>
#include <linux/suspend.h>

/* Join removal and serialize the firmware's untagged power reply stream. */
static DEFINE_MUTEX(pmp_lock);
static const void *pmp_data;
static struct device *pmp_device;
/* The firmware is stopped for a system sleep. */
static bool pmp_asleep;

/* Resumes from disk whose restore found the PMP halted, see below. */
static atomic_t pmp_cold_restarts = ATOMIC_INIT(0);

void apple_pmp_note_cold_restart(void)
{
	atomic_inc(&pmp_cold_restarts);
}

/*
 * A co-processor that iBoot restarts on a cold boot but that has no run bit
 * the AP can read (the SEP) learns from this that the resume from disk it is
 * being notified of was a cold boot: compare the count across the
 * hibernation, from a PM notifier of lower priority than the PMP's.
 */
int apple_pmp_cold_restarts(void)
{
	/* No bound PMP: no restore runs, so a count would mean nothing. */
	if (!READ_ONCE(pmp_data))
		return -ENODEV;
	return atomic_read(&pmp_cold_restarts) & INT_MAX;
}
EXPORT_SYMBOL_GPL(apple_pmp_cold_restarts);

/*
 * After a hibernation restore the boot loader has cold-restarted the PMP
 * (image resident in PMS_SRAM, ASC CPU halted) and nothing replays the
 * probe-time bring-up, so the firmware stays down (the display gate and
 * cpufreq both wait on a PMP that never reports running). The module cannot be
 * reloaded (the power-vote bridge is retained until reboot), so rebuild the
 * firmware session in place.
 *
 * The restored kernel resumes inside hibernate() with in_suspend == 0 and
 * sends PM_POST_HIBERNATION, not PM_POST_RESTORE (that one only reaches the
 * boot kernel when it fails to restore). PM_POST_HIBERNATION also ends an
 * aborted hibernation, where the firmware never stopped: restore() reads the
 * ASC CPU run bit and leaves a running firmware alone.
 */
static int apple_pmp_pm_notify(struct notifier_block *nb, unsigned long action,
			       void *unused)
{
	if (action != PM_POST_HIBERNATION && action != PM_POST_RESTORE)
		return NOTIFY_DONE;
	/* Serialize against unregister in Drop, as the power-command path does. */
	guard(mutex)(&pmp_lock);
	if (pmp_data)
		apple_pmp_restore(pmp_data);
	return NOTIFY_DONE;
}

static struct notifier_block apple_pmp_pm_nb = {
	.notifier_call = apple_pmp_pm_notify,
	/* Ahead of every consumer's restore at priority 0 or below (ANE, DCP, GPU). */
	.priority = 10,
};

int apple_pmp_register(struct device *dev, const void *data)
{
	scoped_guard(mutex, &pmp_lock) {
		if (pmp_data)
			return -EBUSY;
		WRITE_ONCE(pmp_data, data);
		pmp_device = dev;
	}
	/*
	 * Outside pmp_lock: the notifier chain's lock is taken around the
	 * callback, which takes pmp_lock, so nesting them here would invert the
	 * order. The callback re-checks pmp_data under the lock.
	 */
	register_pm_notifier(&apple_pmp_pm_nb);
	return 0;
}

void apple_pmp_unregister(const void *data)
{
	bool was_registered = false;

	scoped_guard(mutex, &pmp_lock) {
		if (pmp_data == data) {
			WRITE_ONCE(pmp_data, NULL);
			pmp_device = NULL;
			was_registered = true;
		}
	}
	if (was_registered)
		unregister_pm_notifier(&apple_pmp_pm_nb);
}

int apple_pmp_link_device(struct device *consumer)
{
	struct device_node *node;
	struct device *supplier = NULL;
	struct device_link *link;

	node = of_parse_phandle(consumer->of_node, "apple,pmp", 0);
	if (!node)
		return -EINVAL;
	scoped_guard(mutex, &pmp_lock) {
		if (pmp_device && pmp_device->of_node == node && device_is_bound(pmp_device))
			supplier = get_device(pmp_device);
	}
	of_node_put(node);
	if (!supplier)
		return -EPROBE_DEFER;
	link = device_link_add(consumer, supplier, DL_FLAG_AUTOREMOVE_CONSUMER);
	put_device(supplier);
	return link ? 0 : -EINVAL;
}
EXPORT_SYMBOL_GPL(apple_pmp_link_device);

int apple_pmp_set_device_power(u8 command, u16 device_id, u32 enabled)
{
	bool report_gfx;
	int ret;

	if ((command != 0x0e && command != 0x0f) || device_id > 15 || enabled > 1)
		return -EINVAL;
	guard(mutex)(&pmp_lock);
	if (!pmp_data)
		return -ENODEV;
	if (pmp_asleep)
		return -EAGAIN;
	/* The matched native report must be ready; never use an elapsed-time fallback. */
	ret = apple_pmp_report_wait_supplier_ready(pmp_device, 5000);
	if (ret)
		return ret;
	/* T6030 needs both the RTKit power vote and the PTD bandwidth report. */
	report_gfx = IS_ENABLED(CONFIG_APPLE_T6030_GPU_PMP_LINK) &&
		     command == 0x0f && device_id == 5 &&
		     of_machine_is_compatible("apple,t6030") &&
		     of_device_is_compatible(pmp_device->of_node, "apple,t6030-pmp-v2");
	if (report_gfx) {
		ret = apple_pmp_report_check_t6030_gfx(pmp_device);
		if (ret)
			return ret;
	}
	if (report_gfx && !enabled) {
		ret = apple_pmp_report_set_t6030_gfx(pmp_device, false);
		if (ret) {
			/* Clearing was written before the ACK timed out. Restore it. */
			apple_pmp_report_set_t6030_gfx(pmp_device, true);
			return ret;
		}
	}
	ret = apple_pmp_send_power_command(pmp_data, command, device_id, enabled);
	/* Power is unknown after a lost reply; assert the report conservatively. */
	if (report_gfx && !enabled && ret)
		apple_pmp_report_set_t6030_gfx(pmp_device, true);
	return ret;
}
EXPORT_SYMBOL_GPL(apple_pmp_set_device_power);

/* Reassert after startup, including a report left set by an earlier failure. */
int apple_pmp_t6030_gpu_ready(void)
{
	int ret;

	guard(mutex)(&pmp_lock);
	if (!pmp_data)
		return -ENODEV;
	if (pmp_asleep)
		return -EAGAIN;
	ret = apple_pmp_report_set_t6030_gfx(pmp_device, false);
	if (ret)
		return ret;
	return apple_pmp_report_set_t6030_gfx(pmp_device, true);
}
EXPORT_SYMBOL_GPL(apple_pmp_t6030_gpu_ready);

/*
 * The PMP's idle is a point of no return: an idled PMP doesn't come back with its power kept,
 * and only a real sleep restarts it. Where the sleep driver can take them, its idle
 * and stop move to the sleep's last step, after every refusal: a refused sleep then finds it
 * still running, and the GPU's request, withdrawn at the commit, reaches a running PMP.
 */
static struct apple_pmp_commit_stop pmp_commit;
static bool pmp_commit_registered, pmp_commit_used;

/* Interrupts off, one CPU: what apple_pmp_sleep() does, through pointers taken before. */
static int apple_pmp_commit_stop_fn(void *unused)
{
	struct apple_pmp_commit_stop *c = &pmp_commit;
	unsigned int us;
	size_t i;
	int ret;

	ret = apple_rtkit_sleep_idle_polled(c->rtk, c->control, &us);
	apple_rtkit_mailbox_recv_off(c->rtk);
	for (i = 0; c->sram && i < c->count; i++)
		c->words[i] = readl_relaxed(c->sram + 4 * i);
	/* Told to idle, it can't carry on whatever it answered: the sleep goes on. */
	return ret ? 1 : 0;
}

/*
 * Retained J713 power votes have no teardown. On T8140 an unbind leaves the firmware running,
 * and a commit with it running reset the SoC: its sleep registration is what refuses
 * then, every sleep until reboot, since a rebind can't register again.
 */
bool apple_pmp_keep_bound(void)
{
	return apple_pmp_display_retained_board() || of_machine_is_compatible("apple,t8140");
}

/* Whether the sleep's last step takes the PMP: registered with the sleep driver, once. */
static bool apple_pmp_commit_available(struct device *dev)
{
	void __iomem *asc;
	int idx;

	if (pmp_commit_registered)
		return true;
	idx = of_property_match_string(dev->of_node, "reg-names", "asc");
	asc = idx >= 0 ? of_iomap(dev->of_node, idx) : NULL;
	if (!asc)
		return false;
	if (apple_sleep_coproc_register(dev, asc, false, apple_pmp_commit_stop_fn, NULL)) {
		iounmap(asc);
		return false;
	}
	pmp_commit_registered = true;

	return true;
}

static int apple_pmp_commit_prepare(struct device *dev)
{
	int ret;

	ret = apple_pmp_sleep_prepare(pmp_data, &pmp_commit);
	if (ret)
		return ret;
	apple_sleep_coproc_set_ready(dev, true);
	pmp_commit_used = true;
	dev_dbg(dev, "idle and stop left for the sleep's commit\n");

	return 0;
}

static int apple_pmp_suspend_late(struct device *dev)
{
	bool at_commit;
	int ret;

	if (!pm_suspend_via_firmware())
		return 0;
	/* Retained power votes have no teardown. */
	if (apple_pmp_display_retained_board())
		return -EBUSY;

	guard(mutex)(&pmp_lock);
	if (pmp_device != dev || pmp_asleep)
		return 0;
	at_commit = apple_pmp_commit_available(dev);
	/* Idled any earlier, a refusal after it would leave it stopped. */
	if (!at_commit && apple_sleep_commit_stops_coprocs()) {
		dev_err(dev, "no stop at the sleep's commit: refusing the sleep\n");
		return -EBUSY;
	}
	/* An idled PMP comes back only through a real sleep, which a dry run never reaches. */
	if (!at_commit && !apple_sleep_power_loss()) {
		dev_warn(dev, "dry run: not idled, the sleep controller can't take it at the commit\n");
		return 0;
	}
	apple_pmp_report_sleep(dev, at_commit);
	ret = at_commit ? apple_pmp_commit_prepare(dev) : apple_pmp_sleep(pmp_data);
	if (ret) {
		apple_pmp_report_wake(dev, false);
		return dev_err_probe(dev, ret, "firmware did not go idle for sleep\n");
	}
	pmp_asleep = true;

	return 0;
}

static int apple_pmp_resume_early(struct device *dev)
{
	int ret;

	guard(mutex)(&pmp_lock);
	if (!pmp_asleep)
		return 0;
	/* Left for the commit: if the sleep was refused before it, the firmware never idled. */
	if (pmp_commit_used) {
		bool stopped = apple_sleep_coproc_stopped(dev);

		pmp_commit_used = false;
		apple_sleep_coproc_set_ready(dev, false);
		if (!pm_resume_via_firmware() && !stopped) {
			pmp_asleep = false;
			apple_pmp_report_wake(dev, true);
			dev_info(dev, "sleep refused before the commit: firmware never stopped\n");
			return 0;
		}
	}
	/* A stopped firmware doesn't restart with its power kept: the next real sleep does. */
	if (!pm_resume_via_firmware()) {
		dev_warn(dev, "sleep refused: firmware stays stopped until a real sleep\n");
		return 0;
	}
	pmp_asleep = false;
	ret = apple_pmp_wake(pmp_data);
	if (ret)
		dev_err(dev, "firmware did not come back from sleep: %d\n", ret);
	else
		dev_dbg(dev, "firmware back from sleep\n");
	/* Requests go back either way: consumers read them from the report too. */
	apple_pmp_report_wake(dev, true);

	return 0;
}

const struct dev_pm_ops apple_pmp_pm_ops = {
	LATE_SYSTEM_SLEEP_PM_OPS(apple_pmp_suspend_late, apple_pmp_resume_early)
};
