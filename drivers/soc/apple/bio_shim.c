/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/* Copyright 2026 Dj */

#include <linux/capability.h>
#include <linux/fs.h>
#include <linux/mutex.h>
#include <linux/ktime.h>
#include <linux/miscdevice.h>
#include <linux/poll.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/wait.h>

#include "shim.h"

struct sep_bio_chardev {
	struct miscdevice misc;
	wait_queue_head_t wq;
	struct mutex lock;
	void *ctx;			/* NULL once detached */
	int (*f_open)(void *ctx);
	void (*f_release)(void *ctx);
	long (*f_ioctl)(void *ctx, unsigned int cmd, unsigned long arg);
	int (*f_ready)(void *ctx);
	bool registered;
	int open_count;
};

static struct sep_bio_chardev *from_file(struct file *file)
{
	return container_of(file->private_data, struct sep_bio_chardev, misc);
}

static int sep_bio_fop_open(struct inode *inode, struct file *file)
{
	struct sep_bio_chardev *d = from_file(file);
	int ret;

	mutex_lock(&d->lock);
	if (!d->ctx) {
		mutex_unlock(&d->lock);
		return -ENODEV;
	}
	ret = d->f_open(d->ctx);
	if (!ret)
		d->open_count++;
	mutex_unlock(&d->lock);

	return ret;
}

static int sep_bio_fop_release(struct inode *inode, struct file *file)
{
	struct sep_bio_chardev *d = from_file(file);
	bool last;

	mutex_lock(&d->lock);
	if (d->ctx)
		d->f_release(d->ctx);
	d->open_count--;
	last = (d->open_count == 0) && !d->ctx;
	mutex_unlock(&d->lock);

	/* Detached while this file was open: nobody else can reach it now. */
	if (last) {
		mutex_destroy(&d->lock);
		kfree(d);
	}

	return 0;
}

static long sep_bio_fop_ioctl(struct file *file, unsigned int cmd,
				 unsigned long arg)
{
	struct sep_bio_chardev *d = from_file(file);
	long ret;

	mutex_lock(&d->lock);
	ret = d->ctx ? d->f_ioctl(d->ctx, cmd, arg) : -ENODEV;
	mutex_unlock(&d->lock);

	return ret;
}

/*
 * Readable when a *_POLL ioctl has something the caller has not seen yet; quiet
 * once that ioctl consumes it.
 */
static __poll_t sep_bio_fop_poll(struct file *file,
				    struct poll_table_struct *wait)
{
	struct sep_bio_chardev *d = from_file(file);

	__poll_t mask;

	poll_wait(file, &d->wq, wait);

	mutex_lock(&d->lock);
	if (!d->ctx)
		mask = EPOLLERR | EPOLLHUP;
	else
		mask = d->f_ready(d->ctx) ? (EPOLLIN | EPOLLRDNORM) : 0;
	mutex_unlock(&d->lock);

	return mask;
}

static const struct file_operations sep_bio_fops = {
	.owner = THIS_MODULE,
	.open = sep_bio_fop_open,
	.release = sep_bio_fop_release,
	.unlocked_ioctl = sep_bio_fop_ioctl,
	.compat_ioctl = compat_ptr_ioctl,
	.poll = sep_bio_fop_poll,
};

/* @name must outlive the registration; the Rust side passes a &'static CStr. */
void *sep_bio_register(const char *name, unsigned short mode, void *ctx,
			  int (*f_open)(void *),
			  void (*f_release)(void *),
			  long (*f_ioctl)(void *, unsigned int, unsigned long),
			  int (*f_ready)(void *))
{
	struct sep_bio_chardev *d;
	int ret;

	d = kzalloc(sizeof(*d), GFP_KERNEL);
	if (!d)
		return NULL;

	init_waitqueue_head(&d->wq);
	mutex_init(&d->lock);
	d->ctx = ctx;
	d->f_open = f_open;
	d->f_release = f_release;
	d->f_ioctl = f_ioctl;
	d->f_ready = f_ready;

	d->misc.minor = MISC_DYNAMIC_MINOR;
	d->misc.name = name;
	d->misc.fops = &sep_bio_fops;
	d->misc.mode = mode;

	ret = misc_register(&d->misc);
	if (ret) {
		mutex_destroy(&d->lock);
		kfree(d);
		return NULL;
	}
	d->registered = true;

	return d;
}

void sep_bio_unregister(void *dev)
{
	struct sep_bio_chardev *d = dev;
	bool free_now;

	if (!d)
		return;

	if (d->registered)
		misc_deregister(&d->misc);

	mutex_lock(&d->lock);
	d->ctx = NULL;
	free_now = (d->open_count == 0);
	mutex_unlock(&d->lock);

	wake_up_interruptible(&d->wq);

	if (free_now) {
		mutex_destroy(&d->lock);
		kfree(d);
	}
}

void sep_bio_wake(void *dev)
{
	struct sep_bio_chardev *d = dev;

	if (d)
		wake_up_interruptible(&d->wq);
}

/* In C so the CAP_SYS_ADMIN number stays a kernel header constant, not a Rust literal. */
int sep_bio_capable_admin(void)
{
	return capable(CAP_SYS_ADMIN) ? 1 : 0;
}

__u64 sep_bio_monotonic_ns(void)
{
	return ktime_get_ns();
}

__u64 sep_bio_boottime_ns(void)
{
	return ktime_get_boottime_ns();
}
