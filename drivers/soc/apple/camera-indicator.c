// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * Apple silicon camera-in-use indicator broker
 *
 * Where the camera has no indicator light on its sensor supply (T8140,
 * T8142, T6050), its image processor blanks every frame unless a capture
 * watchdog is serviced, and it expects that only while a camera-in-use
 * indicator is on screen. The display driver draws that indicator on the
 * internal panel and reports when a frame carrying it has been scanned out;
 * the camera driver services the watchdog only while it is reported shown.
 *
 * This is a software indicator kept by the kernel, not a hardware privacy
 * guarantee: a kernel without it, or code running in it, can defeat it.
 *
 * There is one internal panel per machine, so there is one provider.
 */

#include <linux/debugfs.h>
#include <linux/device.h>
#include <linux/export.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/seq_file.h>
#include <linux/sched.h>
#include <linux/soc/apple/camera-indicator.h>
#include <linux/wait.h>

struct apple_cam_indicator {
	struct mutex lock;	/* provider, ops, priv, users */
	struct device *dev;
	const struct apple_cam_indicator_ops *ops;
	void *priv;
	unsigned int users;
	bool shown;
	atomic_long_t shows;
	atomic_long_t losses;
	wait_queue_head_t wait;
	struct dentry *debugfs;
};

static struct apple_cam_indicator cami = {
	.lock = __MUTEX_INITIALIZER(cami.lock),
	.wait = __WAIT_QUEUE_HEAD_INITIALIZER(cami.wait),
};

int apple_cam_indicator_register(struct device *dev,
				 const struct apple_cam_indicator_ops *ops,
				 void *priv)
{
	guard(mutex)(&cami.lock);
	if (cami.dev)
		return -EBUSY;
	cami.dev = dev;
	cami.ops = ops;
	cami.priv = priv;
	WRITE_ONCE(cami.shown, false);
	if (cami.users)
		ops->set(priv, true);
	dev_info(dev, "camera-in-use indicator: software, drawn by this display\n");
	return 0;
}
EXPORT_SYMBOL_GPL(apple_cam_indicator_register);

void apple_cam_indicator_unregister(struct device *dev)
{
	guard(mutex)(&cami.lock);
	if (cami.dev != dev)
		return;
	apple_cam_indicator_set_shown(false);
	cami.dev = NULL;
	cami.ops = NULL;
	cami.priv = NULL;
}
EXPORT_SYMBOL_GPL(apple_cam_indicator_unregister);

void apple_cam_indicator_set_shown(bool shown)
{
	if (READ_ONCE(cami.shown) == shown)
		return;
	WRITE_ONCE(cami.shown, shown);
	atomic_long_inc(shown ? &cami.shows : &cami.losses);
	wake_up_all(&cami.wait);
}
EXPORT_SYMBOL_GPL(apple_cam_indicator_set_shown);

bool apple_cam_indicator_shown(void)
{
	return READ_ONCE(cami.shown);
}
EXPORT_SYMBOL_GPL(apple_cam_indicator_shown);

void apple_cam_indicator_release(void)
{
	guard(mutex)(&cami.lock);
	if (WARN_ON_ONCE(!cami.users))
		return;
	if (--cami.users)
		return;
	/* Gone before the last frame without it is out: say so at once. */
	apple_cam_indicator_set_shown(false);
	if (cami.ops)
		cami.ops->set(cami.priv, false);
}
EXPORT_SYMBOL_GPL(apple_cam_indicator_release);

/*
 * Ask for the indicator and wait until the display reports it on screen.
 * Returns 0 with the indicator held (release it with
 * apple_cam_indicator_release()), -ENODEV without a display that draws it,
 * -ETIMEDOUT if it was not shown in time, or -ERESTARTSYS.
 */
int apple_cam_indicator_acquire(unsigned int timeout_ms)
{
	long left;

	scoped_guard(mutex, &cami.lock) {
		if (!cami.ops)
			return -ENODEV;
		if (!cami.users++)
			cami.ops->set(cami.priv, true);
	}

	left = wait_event_interruptible_timeout(cami.wait,
						READ_ONCE(cami.shown),
						msecs_to_jiffies(timeout_ms));
	if (left > 0)
		return 0;

	apple_cam_indicator_release();
	return left ? left : -ETIMEDOUT;
}
EXPORT_SYMBOL_GPL(apple_cam_indicator_acquire);

static int apple_cam_indicator_show(struct seq_file *s, void *unused)
{
	guard(mutex)(&cami.lock);
	seq_printf(s, "provider: %s\n", cami.dev ? dev_name(cami.dev) : "none");
	seq_printf(s, "users: %u\n", cami.users);
	seq_printf(s, "shown: %d\n", READ_ONCE(cami.shown));
	seq_printf(s, "shows: %ld\n", atomic_long_read(&cami.shows));
	seq_printf(s, "losses: %ld\n", atomic_long_read(&cami.losses));
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(apple_cam_indicator);

static int __init apple_cam_indicator_init(void)
{
	cami.debugfs = debugfs_create_file("apple-camera-indicator", 0444, NULL,
					   NULL, &apple_cam_indicator_fops);
	return 0;
}
module_init(apple_cam_indicator_init);

static void __exit apple_cam_indicator_exit(void)
{
	debugfs_remove(cami.debugfs);
}
module_exit(apple_cam_indicator_exit);

MODULE_DESCRIPTION("Apple silicon camera-in-use indicator broker");
MODULE_LICENSE("Dual MIT/GPL");
