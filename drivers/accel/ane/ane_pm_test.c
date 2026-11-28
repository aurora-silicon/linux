// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* Exercise recovery against the real runtime-PM and device-link core. */
#include <kunit/test.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/mutex.h>
#include <linux/slab.h>

#include "ane_fw_sleep.h"
#include "ane_pm.h"

struct ane_pm_test_dev {
	struct device dev;
	int suspend_error;
	int resume_error;
	unsigned int suspends;
	unsigned int resumes;
};

struct ane_pm_test_ctx {
	struct kunit *test;
	struct device *consumer;
	struct device *domains[2];
	struct device_link *links[2];
	unsigned int verified;
	int verify_error;
};

static int ane_pm_test_suspend(struct device *dev)
{
	struct ane_pm_test_dev *d = container_of(dev, struct ane_pm_test_dev, dev);

	d->suspends++;
	return d->suspend_error;
}

static int ane_pm_test_resume(struct device *dev)
{
	struct ane_pm_test_dev *d = container_of(dev, struct ane_pm_test_dev, dev);

	d->resumes++;
	return d->resume_error;
}

static const struct dev_pm_ops ane_pm_test_ops = {
	.runtime_suspend = ane_pm_test_suspend,
	.runtime_resume = ane_pm_test_resume,
};

static const struct device_type ane_pm_test_type = {
	.pm = &ane_pm_test_ops,
};

static void ane_pm_test_release(struct device *dev)
{
	kfree(container_of(dev, struct ane_pm_test_dev, dev));
}

static void ane_pm_test_unregister(void *data)
{
	struct device *dev = data;

	pm_runtime_barrier(dev);
	pm_runtime_disable(dev);
	/* Also releases supplier votes after an injected resume failure. */
	pm_runtime_set_suspended(dev);
	device_unregister(dev);
}

static struct device *ane_pm_test_register(struct kunit *test, const char *name)
{
	struct ane_pm_test_dev *d;
	int ret;

	d = kzalloc_obj(*d);
	if (!d)
		return NULL;
	device_initialize(&d->dev);
	d->dev.release = ane_pm_test_release;
	d->dev.type = &ane_pm_test_type;
	ret = dev_set_name(&d->dev, "%s", name);
	if (!ret)
		ret = device_add(&d->dev);
	if (ret) {
		put_device(&d->dev);
		return NULL;
	}
	pm_runtime_enable(&d->dev);
	ret = kunit_add_action_or_reset(test, ane_pm_test_unregister, &d->dev);
	return ret ? NULL : &d->dev;
}

static void ane_pm_test_unlink(void *data)
{
	device_link_del(data);
}

static int ane_pm_test_verify(void *data)
{
	struct ane_pm_test_ctx *ctx = data;

	ctx->verified++;
	KUNIT_EXPECT_TRUE(ctx->test, pm_runtime_suspended(ctx->consumer));
	KUNIT_EXPECT_EQ(ctx->test, atomic_read(&ctx->consumer->power.usage_count), 0);
	for (int i = 0; i < ARRAY_SIZE(ctx->domains); i++) {
		if (!ctx->domains[i])
			continue;
		KUNIT_EXPECT_TRUE(ctx->test, pm_runtime_suspended(ctx->domains[i]));
		KUNIT_EXPECT_EQ(ctx->test, atomic_read(&ctx->domains[i]->power.usage_count), 0);
	}
	return ctx->verify_error;
}

static int ane_pm_test_setup(struct kunit *test, struct ane_pm_test_ctx *ctx,
			     bool multiple)
{
	static const char * const names[] = { "ane-pm-domain0", "ane-pm-domain1" };

	ctx->test = test;
	ctx->consumer = ane_pm_test_register(test, "ane-pm-consumer");
	if (!ctx->consumer)
		return -ENOMEM;
	if (multiple) {
		for (int i = 0; i < ARRAY_SIZE(ctx->domains); i++) {
			ctx->domains[i] = ane_pm_test_register(test, names[i]);
			if (!ctx->domains[i])
				return -ENOMEM;
			ctx->links[i] = device_link_add(ctx->consumer, ctx->domains[i],
							DL_FLAG_STATELESS | DL_FLAG_PM_RUNTIME |
						   DL_FLAG_RPM_ACTIVE);
			if (!ctx->links[i])
				return -ENOMEM;
			if (kunit_add_action_or_reset(test, ane_pm_test_unlink,
						      ctx->links[i]))
				return -ENOMEM;
		}
	}
	return pm_runtime_resume_and_get(ctx->consumer);
}

static void ane_pm_test_finish(struct kunit *test, struct ane_pm_test_ctx *ctx)
{
	KUNIT_EXPECT_EQ(test, atomic_read(&ctx->consumer->power.usage_count), 1);
	KUNIT_EXPECT_GE(test, pm_runtime_put_sync_suspend(ctx->consumer), 0);
}

static void ane_pm_cycle_domains_test(struct kunit *test)
{
	struct ane_pm_test_ctx ctx = {};

	KUNIT_ASSERT_GE(test, ane_pm_test_setup(test, &ctx, true), 0);
	for (int repeat = 0; repeat < 3; repeat++) {
		KUNIT_EXPECT_EQ(test, ane_pm_cycle(ctx.consumer, ctx.domains, 2,
						   ane_pm_test_verify, &ctx), 0);
		KUNIT_EXPECT_TRUE(test, pm_runtime_active(ctx.consumer));
		for (int i = 0; i < ARRAY_SIZE(ctx.domains); i++) {
			KUNIT_EXPECT_TRUE(test, pm_runtime_active(ctx.domains[i]));
			KUNIT_EXPECT_EQ(test, atomic_read(&ctx.domains[i]->power.usage_count), 1);
		}
	}
	KUNIT_EXPECT_EQ(test, ctx.verified, 3U);
	ane_pm_test_finish(test, &ctx);
}

static void ane_pm_cycle_single_test(struct kunit *test)
{
	struct ane_pm_test_ctx ctx = {};

	KUNIT_ASSERT_GE(test, ane_pm_test_setup(test, &ctx, false), 0);
	KUNIT_EXPECT_EQ(test, ane_pm_cycle(ctx.consumer, NULL, 1,
					   ane_pm_test_verify, &ctx), 0);
	KUNIT_EXPECT_EQ(test, ctx.verified, 1U);
	ane_pm_test_finish(test, &ctx);
}

static void ane_pm_cycle_suspend_error_test(struct kunit *test)
{
	struct ane_pm_test_ctx ctx = {};
	struct ane_pm_test_dev *d;

	KUNIT_ASSERT_GE(test, ane_pm_test_setup(test, &ctx, true), 0);
	d = container_of(ctx.consumer, struct ane_pm_test_dev, dev);
	d->suspend_error = -EBUSY;
	KUNIT_EXPECT_EQ(test, ane_pm_cycle(ctx.consumer, ctx.domains, 2,
					   ane_pm_test_verify, &ctx), -EBUSY);
	KUNIT_EXPECT_EQ(test, ctx.verified, 0U);
	d->suspend_error = 0;
	ane_pm_test_finish(test, &ctx);
}

static void ane_pm_cycle_supplier_error_test(struct kunit *test)
{
	struct ane_pm_test_ctx ctx = {};
	struct ane_pm_test_dev *d;

	KUNIT_ASSERT_GE(test, ane_pm_test_setup(test, &ctx, true), 0);
	d = container_of(ctx.domains[1], struct ane_pm_test_dev, dev);
	d->suspend_error = -EBUSY;
	KUNIT_EXPECT_EQ(test, ane_pm_cycle(ctx.consumer, ctx.domains, 2,
					   ane_pm_test_verify, &ctx), -EBUSY);
	KUNIT_EXPECT_EQ(test, ctx.verified, 0U);
	d->suspend_error = 0;
	ane_pm_test_finish(test, &ctx);
}

static void ane_pm_cycle_verify_error_test(struct kunit *test)
{
	struct ane_pm_test_ctx ctx = { .verify_error = -ETIMEDOUT };

	KUNIT_ASSERT_GE(test, ane_pm_test_setup(test, &ctx, true), 0);
	KUNIT_EXPECT_EQ(test, ane_pm_cycle(ctx.consumer, ctx.domains, 2,
					   ane_pm_test_verify, &ctx), -ETIMEDOUT);
	KUNIT_EXPECT_EQ(test, ctx.verified, 1U);
	KUNIT_EXPECT_TRUE(test, pm_runtime_active(ctx.consumer));
	ane_pm_test_finish(test, &ctx);
}

static void ane_pm_cycle_resume_error_test(struct kunit *test)
{
	struct ane_pm_test_ctx ctx = {};
	struct ane_pm_test_dev *d;

	KUNIT_ASSERT_GE(test, ane_pm_test_setup(test, &ctx, false), 0);
	d = container_of(ctx.consumer, struct ane_pm_test_dev, dev);
	d->resume_error = -EIO;
	KUNIT_EXPECT_EQ(test, ane_pm_cycle(ctx.consumer, NULL, 1,
					   ane_pm_test_verify, &ctx), -EIO);
	KUNIT_EXPECT_EQ(test, ctx.verified, 1U);
	/* get_sync must leave exactly the reference the caller will put. */
	KUNIT_EXPECT_EQ(test, atomic_read(&ctx.consumer->power.usage_count), 1);
	pm_runtime_put_noidle(ctx.consumer);
}

/*
 * Exercise the H16 runtime-PM idle-park, wake, system-sleep and unbind model
 * against the real Linux runtime-PM and device-link core: one consumer device
 * linked to three DART suppliers (DL_FLAG_PM_RUNTIME | DL_FLAG_RPM_ACTIVE)
 * plus five attached power-domain devices.
 */
struct ane_h16_rpm_sim {
	struct device dev;
	struct device *power[5];
	struct device *dart[3];
	struct device_link *links[3];
	/* Serializes simulated power-state and park transitions. */
	struct mutex lock;
	bool engine_on;
	bool program_ready;
	bool parked;
	bool slept;
	bool in_sleep;
	bool pinned;
	bool rpm_enabled;
	bool rpm_held;
	bool rpm_busy;
	unsigned int power_downs;
	unsigned int power_ups;
	unsigned int parks;
	unsigned int unparks;
	bool dart_active_at_resume;
};

static void h16_sim_power_down(struct ane_h16_rpm_sim *s)
{
	int i;

	for (i = ARRAY_SIZE(s->dart); i-- > 0; )
		pm_runtime_put_sync(s->dart[i]);
	for (i = ARRAY_SIZE(s->power); i-- > 0; )
		pm_runtime_put_sync(s->power[i]);
	WRITE_ONCE(s->engine_on, false);
	s->power_downs++;
}

static int h16_sim_power_up(struct ane_h16_rpm_sim *s)
{
	int i, j, ret;

	for (i = 0; i < ARRAY_SIZE(s->power); i++) {
		ret = pm_runtime_resume_and_get(s->power[i]);
		if (ret < 0)
			goto err_power;
	}
	for (j = 0; j < ARRAY_SIZE(s->dart); j++) {
		ret = pm_runtime_resume_and_get(s->dart[j]);
		if (ret < 0)
			goto err_dart;
	}
	WRITE_ONCE(s->engine_on, true);
	s->power_ups++;
	return 0;
err_dart:
	while (j--)
		pm_runtime_put_sync(s->dart[j]);
err_power:
	while (i--)
		pm_runtime_put_sync(s->power[i]);
	return ret;
}

static void h16_sim_unpark(struct ane_h16_rpm_sim *s)
{
	lockdep_assert_held(&s->lock);
	if (!s->parked || READ_ONCE(s->in_sleep))
		return;
	if (!h16_sim_power_up(s))
		s->unparks++;
	WRITE_ONCE(s->parked, false);
}

static int h16_sim_runtime_suspend(struct device *dev)
{
	struct ane_h16_rpm_sim *s = container_of(dev, struct ane_h16_rpm_sim, dev);

	if (!READ_ONCE(s->rpm_enabled) || !READ_ONCE(s->engine_on))
		return 0;
	mutex_lock(&s->lock);
	if (!s->engine_on) {
		mutex_unlock(&s->lock);
		return 0;
	}
	if (!s->program_ready || READ_ONCE(s->in_sleep)) {
		mutex_unlock(&s->lock);
		return -EBUSY;
	}
	if (pm_runtime_autosuspend_expiration(dev)) {
		mutex_unlock(&s->lock);
		return -EBUSY;
	}
	h16_sim_power_down(s);
	WRITE_ONCE(s->parked, true);
	s->parks++;
	mutex_unlock(&s->lock);
	return 0;
}

static int h16_sim_runtime_resume(struct device *dev)
{
	struct ane_h16_rpm_sim *s = container_of(dev, struct ane_h16_rpm_sim, dev);
	bool all_darts_active = true;
	int i;

	for (i = 0; i < ARRAY_SIZE(s->dart); i++)
		all_darts_active &= pm_runtime_active(s->dart[i]);
	s->dart_active_at_resume = all_darts_active;

	if (!READ_ONCE(s->rpm_enabled) || !READ_ONCE(s->program_ready) ||
	    !READ_ONCE(s->parked))
		return 0;
	mutex_lock(&s->lock);
	h16_sim_unpark(s);
	mutex_unlock(&s->lock);
	return 0;
}

static const struct dev_pm_ops h16_sim_pm_ops = {
	.runtime_suspend = h16_sim_runtime_suspend,
	.runtime_resume = h16_sim_runtime_resume,
};

static const struct device_type h16_sim_type = {
	.pm = &h16_sim_pm_ops,
};

static void h16_sim_release(struct device *dev)
{
	kfree(container_of(dev, struct ane_h16_rpm_sim, dev));
}

static void h16_sim_unregister(void *data)
{
	struct ane_h16_rpm_sim *s = data;

	device_unregister(&s->dev);
}

static void h16_sim_power_cleanup(void *data)
{
	struct ane_h16_rpm_sim *s = data;

	if (READ_ONCE(s->rpm_enabled)) {
		pm_runtime_disable(&s->dev);
		pm_runtime_dont_use_autosuspend(&s->dev);
		if (s->rpm_held) {
			s->rpm_held = false;
			pm_runtime_put_noidle(&s->dev);
		}
		if (s->rpm_busy) {
			s->rpm_busy = false;
			pm_runtime_put_noidle(&s->dev);
		}
		WRITE_ONCE(s->rpm_enabled, false);
	}
	if (READ_ONCE(s->engine_on))
		h16_sim_power_down(s);
	pm_runtime_set_suspended(&s->dev);
}

static struct ane_h16_rpm_sim *h16_sim_create(struct kunit *test, int delay_ms)
{
	static const char * const pd_names[] = {
		"h16-ane-sys", "h16-ane-cpu", "h16-ane-mpm",
		"h16-ane-td", "h16-ane-base",
	};
	static const char * const dart_names[] = {
		"h16-ane-dart0", "h16-ane-dart1", "h16-ane-dart2",
	};
	struct ane_h16_rpm_sim *s;
	int i, ret;

	s = kzalloc_obj(*s);
	if (!s)
		return NULL;
	mutex_init(&s->lock);
	device_initialize(&s->dev);
	s->dev.release = h16_sim_release;
	s->dev.type = &h16_sim_type;
	ret = dev_set_name(&s->dev, "h16-ane-rpm-consumer");
	if (!ret)
		ret = device_add(&s->dev);
	if (ret) {
		put_device(&s->dev);
		return NULL;
	}
	if (kunit_add_action_or_reset(test, h16_sim_unregister, s))
		return NULL;

	for (i = 0; i < ARRAY_SIZE(s->power); i++) {
		s->power[i] = ane_pm_test_register(test, pd_names[i]);
		if (!s->power[i])
			return NULL;
	}
	for (i = 0; i < ARRAY_SIZE(s->dart); i++) {
		s->dart[i] = ane_pm_test_register(test, dart_names[i]);
		if (!s->dart[i])
			return NULL;
		/* Matches apple_dart_probe_device() supplier links. */
		s->links[i] = device_link_add(&s->dev, s->dart[i],
					      DL_FLAG_STATELESS |
					      DL_FLAG_PM_RUNTIME |
					      DL_FLAG_RPM_ACTIVE);
		if (!s->links[i])
			return NULL;
		if (kunit_add_action_or_reset(test, ane_pm_test_unlink, s->links[i]))
			return NULL;
	}
	/* LIFO: drop power and disable runtime PM before links/suppliers go. */
	if (kunit_add_action_or_reset(test, h16_sim_power_cleanup, s))
		return NULL;
	KUNIT_ASSERT_EQ(test, pm_runtime_set_active(&s->dev), 0);
	pm_runtime_get_noresume(&s->dev);
	s->rpm_held = true;
	pm_runtime_set_autosuspend_delay(&s->dev, delay_ms);
	pm_runtime_use_autosuspend(&s->dev);
	WRITE_ONCE(s->rpm_enabled, true);
	pm_runtime_enable(&s->dev);

	KUNIT_ASSERT_EQ(test, h16_sim_power_up(s), 0);
	/* First boot power-up is not counted as an unpark/wake power-up. */
	s->power_ups = 0;
	s->pinned = true;
	WRITE_ONCE(s->program_ready, true);
	return s;
}

static void h16_sim_wait_darts_suspended(struct kunit *test, struct ane_h16_rpm_sim *s)
{
	int i, tries;

	for (i = 0; i < ARRAY_SIZE(s->dart); i++) {
		for (tries = 0; tries < 40 && !pm_runtime_suspended(s->dart[i]); tries++)
			usleep_range(10000, 15000);
	}
}

static void h16_sim_wait_suspended(struct kunit *test, struct ane_h16_rpm_sim *s)
{
	int i;

	for (i = 0; i < 40 && !pm_runtime_suspended(&s->dev); i++)
		usleep_range(10000, 15000);
	pm_runtime_barrier(&s->dev);
	KUNIT_ASSERT_TRUE(test, pm_runtime_suspended(&s->dev));
	h16_sim_wait_darts_suspended(test, s);
}

static void h16_sim_expect_all_off(struct kunit *test, struct ane_h16_rpm_sim *s)
{
	int i;

	KUNIT_EXPECT_TRUE(test, pm_runtime_status_suspended(&s->dev));
	KUNIT_EXPECT_EQ(test, atomic_read(&s->dev.power.usage_count), 0);
	for (i = 0; i < ARRAY_SIZE(s->dart); i++) {
		KUNIT_EXPECT_TRUE(test, pm_runtime_suspended(s->dart[i]));
		KUNIT_EXPECT_EQ(test, atomic_read(&s->dart[i]->power.usage_count), 0);
	}
	for (i = 0; i < ARRAY_SIZE(s->power); i++) {
		KUNIT_EXPECT_TRUE(test, pm_runtime_suspended(s->power[i]));
		KUNIT_EXPECT_EQ(test, atomic_read(&s->power[i]->power.usage_count), 0);
	}
}

static int h16_sim_session_wake(struct ane_h16_rpm_sim *s)
{
	int ret;

	if (!READ_ONCE(s->parked))
		return 0;
	if (READ_ONCE(s->in_sleep))
		return -EAGAIN;
	ret = pm_runtime_resume_and_get(&s->dev);
	if (ret < 0)
		return ret;
	if (READ_ONCE(s->parked)) {
		mutex_lock(&s->lock);
		h16_sim_unpark(s);
		mutex_unlock(&s->lock);
	}
	if (READ_ONCE(s->parked)) {
		pm_runtime_put_sync_suspend(&s->dev);
		return -EAGAIN;
	}
	pm_runtime_mark_last_busy(&s->dev);
	pm_runtime_put_autosuspend(&s->dev);
	return 0;
}

/*
 * Idle autosuspend parks H16: all five power domains and all three DART
 * suppliers drop to suspended with usage_count == 0. Waking resumes the
 * DART suppliers before the consumer callback powers the five domains up.
 */
static void ane_pm_h16_idle_park_wake_test(struct kunit *test)
{
	struct ane_h16_rpm_sim *s = h16_sim_create(test, 80);
	int i;

	KUNIT_ASSERT_NOT_NULL(test, s);

	/* Hand off boot hold to autosuspend. */
	s->rpm_held = false;
	pm_runtime_mark_last_busy(&s->dev);
	pm_runtime_put_autosuspend(&s->dev);

	/* Simulate an EXECUTE busy hold blocking suspend while in flight. */
	pm_runtime_get_noresume(&s->dev);
	s->rpm_busy = true;
	msleep(120);
	KUNIT_EXPECT_TRUE(test, pm_runtime_active(&s->dev));
	KUNIT_EXPECT_FALSE(test, s->parked);

	/* End EXECUTE: autosuspend parks consumer, 5 domains and 3 DARTs. */
	s->rpm_busy = false;
	pm_runtime_mark_last_busy(&s->dev);
	pm_runtime_put_autosuspend(&s->dev);
	h16_sim_wait_suspended(test, s);
	KUNIT_EXPECT_TRUE(test, s->parked);
	KUNIT_EXPECT_FALSE(test, s->engine_on);
	KUNIT_EXPECT_EQ(test, s->parks, 1U);
	KUNIT_EXPECT_EQ(test, s->power_downs, 1U);
	h16_sim_expect_all_off(test, s);

	/* Wake for a request: DARTs resume first, then 5 domains power up. */
	KUNIT_ASSERT_EQ(test, h16_sim_session_wake(s), 0);
	KUNIT_EXPECT_TRUE(test, s->dart_active_at_resume);
	KUNIT_EXPECT_FALSE(test, s->parked);
	KUNIT_EXPECT_TRUE(test, s->engine_on);
	KUNIT_EXPECT_EQ(test, s->unparks, 1U);
	KUNIT_EXPECT_EQ(test, s->power_ups, 1U);
	for (i = 0; i < ARRAY_SIZE(s->power); i++)
		KUNIT_EXPECT_TRUE(test, pm_runtime_active(s->power[i]));
	for (i = 0; i < ARRAY_SIZE(s->dart); i++)
		KUNIT_EXPECT_TRUE(test, pm_runtime_active(s->dart[i]));

	/* Second idle park + wake cycle stays clean and balanced. */
	h16_sim_wait_suspended(test, s);
	KUNIT_EXPECT_EQ(test, s->parks, 2U);
	KUNIT_EXPECT_EQ(test, s->power_downs, 2U);
	h16_sim_expect_all_off(test, s);

	KUNIT_ASSERT_EQ(test, h16_sim_session_wake(s), 0);
	KUNIT_EXPECT_EQ(test, s->unparks, 2U);
	KUNIT_EXPECT_EQ(test, s->power_ups, 2U);
	KUNIT_EXPECT_TRUE(test, s->engine_on);
}

/*
 * System S2R while already parked does no double power_down or double
 * power_up and passes the device-suspend backstop. S2R while active powers
 * down once, suspends the DARTs, and stays parked on resume (including the
 * pm_runtime_forbid fallback wake path).
 */
static void ane_pm_h16_s2r_while_parked_test(struct kunit *test)
{
	struct ane_h16_rpm_sim *s = h16_sim_create(test, 80);
	bool running;

	KUNIT_ASSERT_NOT_NULL(test, s);
	s->rpm_held = false;
	pm_runtime_mark_last_busy(&s->dev);
	pm_runtime_put_autosuspend(&s->dev);
	h16_sim_wait_suspended(test, s);
	KUNIT_ASSERT_EQ(test, s->power_downs, 1U);

	/* S2R prepare while already parked: no second power_down. */
	WRITE_ONCE(s->in_sleep, true);
	pm_runtime_get_noresume(&s->dev);
	mutex_lock(&s->lock);
	if (!s->parked) {
		h16_sim_power_down(s);
		WRITE_ONCE(s->slept, true);
	}
	mutex_unlock(&s->lock);
	pm_runtime_put_sync_suspend(&s->dev);

	/* Device-suspend backstop must allow sleep while parked. */
	running = s->pinned && READ_ONCE(s->engine_on) && !READ_ONCE(s->slept);
	KUNIT_EXPECT_EQ(test, ane_fw_sleep_backstop(running), 0);
	KUNIT_EXPECT_EQ(test, s->power_downs, 1U);
	KUNIT_EXPECT_FALSE(test, s->slept);
	/* Requests during S2R get -EAGAIN without booting. */
	KUNIT_EXPECT_EQ(test, h16_sim_session_wake(s), -EAGAIN);

	/* S2R resume while parked: no power_up, stays parked. */
	WRITE_ONCE(s->in_sleep, false);
	KUNIT_EXPECT_EQ(test, s->power_ups, 0U);
	KUNIT_EXPECT_TRUE(test, s->parked);
	h16_sim_expect_all_off(test, s);

	/* First request after S2R wakes cleanly. */
	KUNIT_ASSERT_EQ(test, h16_sim_session_wake(s), 0);
	KUNIT_EXPECT_EQ(test, s->unparks, 1U);
	KUNIT_EXPECT_EQ(test, s->power_ups, 1U);

	/* Now S2R while active with runtime PM forbidden (power/control=on). */
	pm_runtime_forbid(&s->dev);
	WRITE_ONCE(s->in_sleep, true);
	pm_runtime_get_noresume(&s->dev);
	mutex_lock(&s->lock);
	h16_sim_power_down(s);
	WRITE_ONCE(s->slept, true);
	mutex_unlock(&s->lock);
	pm_runtime_put_sync_suspend(&s->dev);
	running = s->pinned && READ_ONCE(s->engine_on) && !READ_ONCE(s->slept);
	KUNIT_EXPECT_EQ(test, ane_fw_sleep_backstop(running), 0);
	KUNIT_EXPECT_EQ(test, s->power_downs, 2U);

	/* PM_POST_SUSPEND parks the engine; consumer stayed RPM_ACTIVE (forbidden). */
	mutex_lock(&s->lock);
	WRITE_ONCE(s->slept, false);
	WRITE_ONCE(s->parked, true);
	mutex_unlock(&s->lock);
	WRITE_ONCE(s->in_sleep, false);
	KUNIT_EXPECT_TRUE(test, pm_runtime_active(&s->dev));
	KUNIT_EXPECT_TRUE(test, s->parked);

	/* Fallback unpark path in session_wake boots when RPM never suspended. */
	KUNIT_ASSERT_EQ(test, h16_sim_session_wake(s), 0);
	KUNIT_EXPECT_FALSE(test, s->parked);
	KUNIT_EXPECT_TRUE(test, s->engine_on);
	KUNIT_EXPECT_EQ(test, s->unparks, 2U);
	KUNIT_EXPECT_EQ(test, s->power_ups, 2U);
	pm_runtime_allow(&s->dev);
}

/*
 * Unbind/remove while parked disables runtime PM without waking the engine or
 * double-powering down.
 */
static void ane_pm_h16_unbind_while_parked_test(struct kunit *test)
{
	struct ane_h16_rpm_sim *s = h16_sim_create(test, 20);

	KUNIT_ASSERT_NOT_NULL(test, s);
	s->rpm_held = false;
	pm_runtime_mark_last_busy(&s->dev);
	pm_runtime_put_autosuspend(&s->dev);
	h16_sim_wait_suspended(test, s);
	KUNIT_ASSERT_EQ(test, s->power_downs, 1U);
	KUNIT_ASSERT_EQ(test, s->unparks, 0U);

	/* Remove while parked: stays suspended, no wake or double power-down. */
	pm_runtime_disable(&s->dev);
	pm_runtime_dont_use_autosuspend(&s->dev);
	WRITE_ONCE(s->rpm_enabled, false);
	KUNIT_EXPECT_EQ(test, s->power_downs, 1U);
	KUNIT_EXPECT_EQ(test, s->power_ups, 0U);
	KUNIT_EXPECT_EQ(test, s->unparks, 0U);
	h16_sim_expect_all_off(test, s);
}

/*
 * Pre-RUN unbind/rollback while active drops the device hold, powers down
 * the five domains and three DARTs, and releases the supplier device links.
 */
static void ane_pm_h16_unbind_rollback_test(struct kunit *test)
{
	struct ane_h16_rpm_sim *s = h16_sim_create(test, 20);

	KUNIT_ASSERT_NOT_NULL(test, s);
	pm_runtime_disable(&s->dev);
	pm_runtime_dont_use_autosuspend(&s->dev);
	s->rpm_held = false;
	pm_runtime_put_noidle(&s->dev);
	WRITE_ONCE(s->rpm_enabled, false);
	h16_sim_power_down(s);
	KUNIT_EXPECT_EQ(test, pm_runtime_set_suspended(&s->dev), 0);
	h16_sim_wait_darts_suspended(test, s);
	h16_sim_expect_all_off(test, s);
}

static struct kunit_case ane_pm_test_cases[] = {
	KUNIT_CASE(ane_pm_cycle_domains_test),
	KUNIT_CASE(ane_pm_cycle_single_test),
	KUNIT_CASE(ane_pm_cycle_suspend_error_test),
	KUNIT_CASE(ane_pm_cycle_supplier_error_test),
	KUNIT_CASE(ane_pm_cycle_verify_error_test),
	KUNIT_CASE(ane_pm_cycle_resume_error_test),
	KUNIT_CASE(ane_pm_h16_idle_park_wake_test),
	KUNIT_CASE(ane_pm_h16_s2r_while_parked_test),
	KUNIT_CASE(ane_pm_h16_unbind_while_parked_test),
	KUNIT_CASE(ane_pm_h16_unbind_rollback_test),
	{}
};

static struct kunit_suite ane_pm_test_suite = {
	.name = "ane-pm-recovery",
	.test_cases = ane_pm_test_cases,
};

kunit_test_suite(ane_pm_test_suite);

MODULE_LICENSE("Dual MIT/GPL");
MODULE_DESCRIPTION("ANE recovery runtime-PM transaction tests");
