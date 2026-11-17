// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * KUnit tests for the SEP character device shim: an ioctl that sleeps on a
 * concurrent device must not hold up open() or a second caller's ioctl (the
 * cancel that ends a touch-gated wait), and unregistering waits for it.
 *
 * Included from bio_shim.c.
 */

#include <kunit/test.h>
#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/kthread.h>

#define BIO_TEST_WAIT 1		/* sleeps until BIO_TEST_CANCEL or 5 s */
#define BIO_TEST_CANCEL 2

struct bio_test_ctx {
	struct completion cancelled;
	struct completion waiting;
};

static int bio_test_open(void *ctx) { return 0; }
static void bio_test_release(void *ctx) { }
static int bio_test_ready(void *ctx) { return 0; }

static long bio_test_ioctl(void *ctx, unsigned int cmd, unsigned long arg)
{
	struct bio_test_ctx *c = ctx;

	switch (cmd) {
	case BIO_TEST_WAIT:
		complete(&c->waiting);
		return wait_for_completion_timeout(&c->cancelled, 5 * HZ) ?
		       -ECANCELED : -ETIMEDOUT;
	case BIO_TEST_CANCEL:
		complete(&c->cancelled);
		return 0;
	}
	return -ENOTTY;
}

struct bio_test_call {
	struct file *file;
	unsigned int cmd;
	long ret;
	struct completion done;
};

static int bio_test_call_fn(void *data)
{
	struct bio_test_call *call = data;

	call->ret = sep_bio_fop_ioctl(call->file, call->cmd, 0);
	complete(&call->done);
	return 0;
}

static void bio_test_start(struct kunit *test, struct bio_test_call *call,
			   struct file *file, unsigned int cmd)
{
	struct task_struct *t;

	call->file = file;
	call->cmd = cmd;
	call->ret = 1;
	init_completion(&call->done);
	t = kthread_run(bio_test_call_fn, call, "sep-bio-test");
	KUNIT_ASSERT_FALSE(test, IS_ERR(t));
}

struct bio_test_dev {
	struct bio_test_ctx ctx;
	struct sep_bio_chardev *d;
	struct file file;
	struct inode inode;
};

static void bio_test_setup(struct kunit *test, struct bio_test_dev *t)
{
	init_completion(&t->ctx.cancelled);
	init_completion(&t->ctx.waiting);
	t->d = sep_bio_register_concurrent("sep-bio-kunit", 0600, &t->ctx,
					   bio_test_open, bio_test_release,
					   bio_test_ioctl, bio_test_ready);
	KUNIT_ASSERT_NOT_NULL(test, t->d);
	t->file.private_data = &t->d->misc;
	KUNIT_ASSERT_EQ(test, sep_bio_fop_open(&t->inode, &t->file), 0);
}

/* A cancel from another caller reaches the driver while a wait is running. */
static void bio_shim_cancel_during_wait(struct kunit *test)
{
	struct bio_test_dev *t = kunit_kzalloc(test, sizeof(*t), GFP_KERNEL);
	struct bio_test_call wait;
	struct file second = {};
	struct inode inode2 = {};

	KUNIT_ASSERT_NOT_NULL(test, t);
	bio_test_setup(test, t);
	bio_test_start(test, &wait, &t->file, BIO_TEST_WAIT);
	KUNIT_ASSERT_TRUE(test, wait_for_completion_timeout(&t->ctx.waiting, 2 * HZ));

	/* open() and the cancel run while the wait sleeps inside f_ioctl. */
	second.private_data = &t->d->misc;
	KUNIT_EXPECT_EQ(test, sep_bio_fop_open(&inode2, &second), 0);
	KUNIT_EXPECT_EQ(test, sep_bio_fop_ioctl(&second, BIO_TEST_CANCEL, 0), 0L);
	KUNIT_EXPECT_TRUE(test, wait_for_completion_timeout(&wait.done, 2 * HZ));
	KUNIT_EXPECT_EQ(test, wait.ret, (long)-ECANCELED);

	sep_bio_fop_release(&inode2, &second);
	sep_bio_unregister(t->d);
	sep_bio_fop_release(&t->inode, &t->file);
}

struct bio_test_unreg {
	struct sep_bio_chardev *d;
	struct completion done;
};

static int bio_test_unreg_fn(void *data)
{
	struct bio_test_unreg *u = data;

	sep_bio_unregister(u->d);
	complete(&u->done);
	return 0;
}

/* Unregistering waits for a running ioctl; later ioctls get -ENODEV. */
static void bio_shim_unregister_waits(struct kunit *test)
{
	struct bio_test_dev *t = kunit_kzalloc(test, sizeof(*t), GFP_KERNEL);
	struct bio_test_call wait;
	struct bio_test_unreg u;
	struct task_struct *k;

	KUNIT_ASSERT_NOT_NULL(test, t);
	bio_test_setup(test, t);
	bio_test_start(test, &wait, &t->file, BIO_TEST_WAIT);
	KUNIT_ASSERT_TRUE(test, wait_for_completion_timeout(&t->ctx.waiting, 2 * HZ));

	u.d = t->d;
	init_completion(&u.done);
	k = kthread_run(bio_test_unreg_fn, &u, "sep-bio-unreg");
	KUNIT_ASSERT_FALSE(test, IS_ERR(k));
	KUNIT_EXPECT_FALSE(test, wait_for_completion_timeout(&u.done, HZ / 5));

	/* End the wait directly: a new ioctl would queue behind the writer. */
	complete(&t->ctx.cancelled);
	KUNIT_EXPECT_TRUE(test, wait_for_completion_timeout(&wait.done, 2 * HZ));
	KUNIT_EXPECT_TRUE(test, wait_for_completion_timeout(&u.done, 2 * HZ));
	KUNIT_EXPECT_EQ(test, sep_bio_fop_ioctl(&t->file, BIO_TEST_CANCEL, 0), (long)-ENODEV);
	sep_bio_fop_release(&t->inode, &t->file);
}

static struct kunit_case bio_shim_test_cases[] = {
	KUNIT_CASE(bio_shim_cancel_during_wait),
	KUNIT_CASE(bio_shim_unregister_waits),
	{}
};

static struct kunit_suite bio_shim_test_suite = {
	.name = "apple_sep_bio_shim",
	.test_cases = bio_shim_test_cases,
};
kunit_test_suite(bio_shim_test_suite);
