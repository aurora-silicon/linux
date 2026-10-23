// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * Included by av.c. Real workqueue lifetime for audiosrv_data: quiesce drains
 * owner work before the owner is marked dead, an ERR_PTR endpoint is not
 * dereferenced, and skipping the drain lets the callback observe the dead
 * owner. The deliberate no-drain control validates the test boundary;
 * production-body mutation runs separately verify the real drain.
 */
#include <linux/atomic.h>
#include <linux/completion.h>
#include <linux/delay.h>
#include <kunit/device.h>
#include <kunit/test.h>

struct av_life_test {
	struct device *dev;
	void *group;
	struct apple_dcp *dcp;
	struct audiosrv_data *asrv;
	struct apple_dcp_afkep *ep;
	struct workqueue_struct *wq;
	struct work_struct ep_work;
	struct work_struct stop_work;
	struct workqueue_struct *stop_wq;
	atomic_t owner_runs;
	atomic_t owner_saw;
	int ep_runs;
	int ep_saw;
	int dead;
	struct completion entered;
	struct completion release;
	struct completion owner_done;
	struct completion stop_entered;
	struct completion stop_done;
	bool stop_saw_owner_done;
	bool stop_unbind;
	bool stop_release_group;
	struct audiosrv_data *release_owner;
	struct apple_dcp_afkep *release_ep;
	bool endpoint_released;
	bool release_saw_stopped;
	bool release_saw_quiesced;
	bool group_released;
};

static struct av_life_test *av_life_current;
static void av_life_release_group(struct av_life_test *t);

static void av_life_owner_fn(struct work_struct *work)
{
	struct av_life_test *t = av_life_current;

	atomic_set(&t->owner_saw, READ_ONCE(t->dead));
	atomic_inc(&t->owner_runs);
	complete(&t->owner_done);
}

static void av_life_stop_fn(struct work_struct *work)
{
	struct av_life_test *t = container_of(work, struct av_life_test, stop_work);

	complete(&t->stop_entered);
	if (t->stop_release_group)
		av_life_release_group(t);
	else if (t->stop_unbind)
		dcp_comp_unbind(t->dcp->dev, NULL, NULL);
	else
		av_audiosrv_quiesce(t->dcp);
	t->stop_saw_owner_done = completion_done(&t->owner_done);
	complete(&t->stop_done);
}

static void av_life_cleanup(void *data)
{
	struct av_life_test *t = data;

	/* Assertion failure must not strand a blocked system worker. */
	complete_all(&t->release);
	if (t->stop_unbind)
		cancel_work_sync(&t->dcp->external_audio_work);
	if (t->stop_wq) {
		flush_work(&t->stop_work);
		destroy_workqueue(t->stop_wq);
	}
	if (t->group)
		av_life_release_group(t);
	if (t->group_released) {
		t->dcp->audiosrv = NULL;
		t->dcp->avep = NULL;
	}
	av_audiosrv_quiesce(t->dcp);
	/* Keep deliberately broken production mutants safe after assertions. */
	if (t->asrv && t->asrv->works_live) {
		disable_work_sync(&t->asrv->start_av_service_wq);
		disable_work_sync(&t->asrv->terminate_wq);
	}
	if (t->wq)
		destroy_workqueue(t->wq);
	if (t->ep && !IS_ERR(t->ep))
		t->ep->wq = NULL;
	av_life_current = NULL;
}

static void av_life_ep_fn(struct work_struct *work)
{
	struct av_life_test *t = av_life_current;

	t->ep_saw = READ_ONCE(t->dead);
	t->ep_runs++;
}

static void av_life_blocked_fn(struct work_struct *work)
{
	struct av_life_test *t = av_life_current;

	complete(&t->entered);
	wait_for_completion(&t->release);
	atomic_set(&t->owner_saw, READ_ONCE(t->dead));
	atomic_inc(&t->owner_runs);
	complete(&t->owner_done);
}

static void av_life_queue_blocker_fn(struct work_struct *work)
{
	struct av_life_test *t = av_life_current;

	complete(&t->entered);
	wait_for_completion(&t->release);
}

static void av_life_producer_fn(struct work_struct *work)
{
	struct av_life_test *t = av_life_current;

	complete(&t->entered);
	wait_for_completion(&t->release);
	/* Model only the initializer publication boundary, with real works. */
	t->dcp->audiosrv = t->asrv;
	schedule_work(&t->asrv->start_av_service_wq);
}

static struct av_life_test *av_life_setup(struct kunit *test, work_func_t owner_fn)
{
	struct av_life_test *t = kunit_kzalloc(test, sizeof(*t), GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, t);
	t->dev = kunit_device_register(test, "av-lifetime");
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, t->dev);
	t->dcp = kunit_kzalloc(test, sizeof(*t->dcp), GFP_KERNEL);
	t->asrv = kunit_kzalloc(test, sizeof(*t->asrv), GFP_KERNEL);
	t->ep = kunit_kzalloc(test, sizeof(*t->ep), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, t->dcp);
	KUNIT_ASSERT_NOT_NULL(test, t->asrv);
	KUNIT_ASSERT_NOT_NULL(test, t->ep);
	t->dcp->dev = t->dev;
	dev_set_drvdata(t->dev, t->dcp);
	spin_lock_init(&t->ep->lock);
	init_rwsem(&t->asrv->srv_rwsem);
	INIT_WORK(&t->asrv->start_av_service_wq, owner_fn);
	INIT_WORK(&t->asrv->terminate_wq, owner_fn);
	INIT_WORK(&t->dcp->external_audio_work, av_life_owner_fn);
	t->asrv->works_live = true;
	t->dcp->audiosrv = t->asrv;
	atomic_set(&t->owner_saw, -1);
	init_completion(&t->entered);
	init_completion(&t->release);
	init_completion(&t->owner_done);
	init_completion(&t->stop_entered);
	init_completion(&t->stop_done);
	INIT_WORK(&t->stop_work, av_life_stop_fn);
	av_life_current = t;
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, av_life_cleanup, t), 0);
	return t;
}

static bool av_life_wait_reports_stopped(struct av_life_test *t)
{
	unsigned long deadline = jiffies + HZ;
	bool stopped;

	do {
		down_read(&t->asrv->srv_rwsem);
		stopped = t->asrv->reports_stopped;
		up_read(&t->asrv->srv_rwsem);
		if (stopped)
			return true;
		usleep_range(50, 100);
	} while (time_before(jiffies, deadline));
	return false;
}

static void av_life_parallel_stop(struct kunit *test, struct av_life_test *t)
{
	unsigned long left;

	t->stop_wq = alloc_ordered_workqueue("av-lifetime-stop", 0);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, t->stop_wq);
	KUNIT_ASSERT_TRUE(test, queue_work(t->stop_wq, &t->stop_work));
	left = wait_for_completion_timeout(&t->stop_entered, HZ);
	KUNIT_ASSERT_NE(test, left, 0UL);
	/* Observe entry into the real drain, rather than just its caller. */
	KUNIT_ASSERT_TRUE(test, av_life_wait_reports_stopped(t));
	/* The owner is deliberately still running: quiesce must wait. */
	left = wait_for_completion_timeout(&t->stop_done, msecs_to_jiffies(20));
	KUNIT_EXPECT_EQ(test, left, 0UL);
	complete(&t->release);
	left = wait_for_completion_timeout(&t->stop_done, HZ);
	KUNIT_ASSERT_NE(test, left, 0UL);
	KUNIT_EXPECT_TRUE(test, t->stop_saw_owner_done);
}

/* Quiesce waits for owner work and flushes a live endpoint before dead. */
static void av_lifetime_drain_before_dead(struct kunit *test)
{
	struct av_life_test *t = av_life_setup(test, av_life_blocked_fn);
	unsigned long left;

	t->wq = alloc_ordered_workqueue("av-lifetime-test", 0);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, t->wq);
	t->ep->wq = t->wq;
	t->dcp->avep = t->ep;
	INIT_WORK(&t->ep_work, av_life_ep_fn);

	KUNIT_EXPECT_TRUE(test, queue_work(t->wq, &t->ep_work));
	KUNIT_EXPECT_TRUE(test, schedule_work(&t->asrv->terminate_wq));
	left = wait_for_completion_timeout(&t->entered, HZ);
	KUNIT_ASSERT_NE(test, left, 0UL);
	av_life_parallel_stop(test, t);
	KUNIT_EXPECT_EQ(test, t->ep_runs, 1);
	KUNIT_EXPECT_EQ(test, t->ep_saw, 0);
	KUNIT_EXPECT_EQ(test, atomic_read(&t->owner_runs), 1);
	KUNIT_EXPECT_EQ(test, atomic_read(&t->owner_saw), 0);
	WRITE_ONCE(t->dead, 1);
	KUNIT_EXPECT_EQ(test, atomic_read(&t->owner_saw), 0);
	KUNIT_EXPECT_EQ(test, t->ep_saw, 0);
	KUNIT_EXPECT_TRUE(test, t->asrv->work_disabled);
	KUNIT_EXPECT_TRUE(test, t->asrv->reports_stopped);
}

/*
 * afk_init publishes ERR_PTR. Quiesce still drains owner work and must not
 * dereference that pointer or clear it.
 */
static void av_lifetime_err_ptr_endpoint(struct kunit *test)
{
	struct av_life_test *t = av_life_setup(test, av_life_blocked_fn);
	unsigned long left;

	t->dcp->avep = ERR_PTR(-EINVAL);
	KUNIT_EXPECT_TRUE(test, schedule_work(&t->asrv->start_av_service_wq));
	left = wait_for_completion_timeout(&t->entered, HZ);
	KUNIT_ASSERT_NE(test, left, 0UL);
	av_life_parallel_stop(test, t);
	KUNIT_EXPECT_EQ(test, atomic_read(&t->owner_runs), 1);
	KUNIT_EXPECT_EQ(test, atomic_read(&t->owner_saw), 0);
	WRITE_ONCE(t->dead, 1);
	KUNIT_EXPECT_EQ(test, atomic_read(&t->owner_saw), 0);
	KUNIT_EXPECT_TRUE(test, IS_ERR(t->dcp->avep));
	KUNIT_EXPECT_EQ(test, PTR_ERR(t->dcp->avep), -EINVAL);
	KUNIT_EXPECT_TRUE(test, t->asrv->work_disabled);
}

/* Pending items are cancelled; only already-running callbacks must finish. */
static void av_lifetime_pending_owner_cancelled(struct kunit *test)
{
	struct av_life_test *t = av_life_setup(test, av_life_owner_fn);
	unsigned long left;

	t->wq = alloc_ordered_workqueue("av-lifetime-pending", 0);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, t->wq);
	INIT_WORK(&t->ep_work, av_life_queue_blocker_fn);
	KUNIT_ASSERT_TRUE(test, queue_work(t->wq, &t->ep_work));
	left = wait_for_completion_timeout(&t->entered, HZ);
	KUNIT_ASSERT_NE(test, left, 0UL);
	KUNIT_ASSERT_TRUE(test, queue_work(t->wq, &t->asrv->start_av_service_wq));
	KUNIT_ASSERT_TRUE(test, queue_work(t->wq, &t->asrv->terminate_wq));
	av_audiosrv_quiesce(t->dcp);
	KUNIT_EXPECT_EQ(test, atomic_read(&t->owner_runs), 0);
	KUNIT_EXPECT_FALSE(test, work_pending(&t->asrv->start_av_service_wq));
	KUNIT_EXPECT_FALSE(test, work_pending(&t->asrv->terminate_wq));
	WRITE_ONCE(t->dead, 1);
	complete(&t->release);
	flush_workqueue(t->wq);
	KUNIT_EXPECT_EQ(test, atomic_read(&t->owner_runs), 0);
}

/* The real common-unbind entry must stop initialization before owner drain. */
static void av_lifetime_unbind_waits_for_initializer(struct kunit *test)
{
	struct av_life_test *t = av_life_setup(test, av_life_owner_fn);
	unsigned long left;
	unsigned long deadline;
	bool disabled = false;

	t->dcp->external_native = true;
	t->dcp->ext_dead = true;
	t->dcp->audiosrv = NULL;
	t->stop_unbind = true;
	INIT_WORK(&t->dcp->external_audio_work, av_life_producer_fn);
	KUNIT_ASSERT_TRUE(test, schedule_work(&t->dcp->external_audio_work));
	left = wait_for_completion_timeout(&t->entered, HZ);
	KUNIT_ASSERT_NE(test, left, 0UL);
	t->stop_wq = alloc_ordered_workqueue("av-lifetime-unbind", 0);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, t->stop_wq);
	KUNIT_ASSERT_TRUE(test, queue_work(t->stop_wq, &t->stop_work));
	left = wait_for_completion_timeout(&t->stop_entered, HZ);
	KUNIT_ASSERT_NE(test, left, 0UL);
	/*
	 * The running producer cannot finish yet. Queue rejection with no
	 * pending requeue proves unbind reached disable_work_sync, rather
	 * than merely pausing before its first statement. Any admitted
	 * requeue is cancelled by the real disable before this succeeds.
	 */
	deadline = jiffies + HZ;
	do {
		if (!schedule_work(&t->dcp->external_audio_work) &&
		    !work_pending(&t->dcp->external_audio_work)) {
			disabled = true;
			break;
		}
		usleep_range(50, 100);
	} while (time_before(jiffies, deadline));
	KUNIT_EXPECT_TRUE(test, disabled);
	left = wait_for_completion_timeout(&t->stop_done, msecs_to_jiffies(20));
	KUNIT_EXPECT_EQ(test, left, 0UL);
	complete(&t->release);
	left = wait_for_completion_timeout(&t->stop_done, HZ);
	KUNIT_ASSERT_NE(test, left, 0UL);
	KUNIT_EXPECT_PTR_EQ(test, t->dcp->audiosrv, t->asrv);
	KUNIT_EXPECT_TRUE(test, t->asrv->reports_stopped);
	KUNIT_EXPECT_TRUE(test, t->asrv->work_disabled);
	KUNIT_EXPECT_FALSE(test, work_busy(&t->asrv->start_av_service_wq));
	KUNIT_EXPECT_FALSE(test, schedule_work(&t->asrv->start_av_service_wq));
	KUNIT_EXPECT_FALSE(test, schedule_work(&t->asrv->terminate_wq));
}

/* Two quiesces are one disable; one restart arms a single queue. */
static void av_lifetime_quiesce_once_restart_once(struct kunit *test)
{
	struct av_life_test *t = av_life_setup(test, av_life_owner_fn);

	av_audiosrv_quiesce(t->dcp);
	av_audiosrv_quiesce(t->dcp);
	KUNIT_EXPECT_FALSE(test, schedule_work(&t->asrv->terminate_wq));
	av_audiosrv_reports_restart(t->dcp);
	/* A second restart must not underflow either work's disable count. */
	av_audiosrv_reports_restart(t->dcp);
	KUNIT_EXPECT_FALSE(test, t->asrv->reports_stopped);
	KUNIT_EXPECT_FALSE(test, t->asrv->work_disabled);
	KUNIT_EXPECT_TRUE(test, schedule_work(&t->asrv->terminate_wq));
	flush_work(&t->asrv->terminate_wq);
	KUNIT_EXPECT_EQ(test, atomic_read(&t->owner_runs), 1);
	KUNIT_EXPECT_EQ(test, atomic_read(&t->owner_saw), 0);
	av_audiosrv_quiesce(t->dcp);
}

static void av_lifetime_report_after_stop(struct kunit *test)
{
	struct av_life_test *t = av_life_setup(test, av_life_owner_fn);
	struct apple_epic_service svc = { };
	struct epic_service_call call = {
		.group = cpu_to_le16(2),
		.command = cpu_to_le32(1),
		.magic = cpu_to_le32(EPIC_SERVICE_CALL_MAGIC),
	};
	t->dcp->fw_compat = DCP_FIRMWARE_V_14_7;
	t->dcp->external_native = true;
	t->ep->dcp = t->dcp;
	svc.ep = t->ep;
	t->asrv->srv = &svc;

	av_audiosrv_quiesce(t->dcp);
	av_audiosrv_quiesce(t->dcp);
	av_audiosrv_report(&svc, 0, &call, sizeof(call));
	KUNIT_EXPECT_EQ(test, atomic_read(&t->owner_runs), 0);
	KUNIT_EXPECT_FALSE(test, work_busy(&t->asrv->terminate_wq));

	av_audiosrv_reports_restart(t->dcp);
	av_audiosrv_report(&svc, 0, &call, sizeof(call));
	flush_work(&t->asrv->terminate_wq);
	KUNIT_EXPECT_EQ(test, atomic_read(&t->owner_runs), 1);
	av_audiosrv_quiesce(t->dcp);
}

/*
 * Negative control. The owner is marked dead with the callback blocked and
 * without quiesce. The callback must observe that dead owner.
 */
static void av_lifetime_control_without_drain(struct kunit *test)
{
	struct av_life_test *t = av_life_setup(test, av_life_blocked_fn);
	unsigned long left;

	KUNIT_EXPECT_TRUE(test, schedule_work(&t->asrv->terminate_wq));
	left = wait_for_completion_timeout(&t->entered, HZ);
	if (!left) {
		complete(&t->release);
		flush_work(&t->asrv->terminate_wq);
		KUNIT_FAIL(test, "terminate work did not start");
		return;
	}
	WRITE_ONCE(t->dead, 1);
	complete(&t->release);
	flush_work(&t->asrv->terminate_wq);
	KUNIT_EXPECT_EQ(test, atomic_read(&t->owner_runs), 1);
	KUNIT_EXPECT_EQ(test, atomic_read(&t->owner_saw), 1);
}

/* Never-initialized owner work is not passed to the workqueue. */
static void av_lifetime_never_inited(struct kunit *test)
{
	struct av_life_test *t = kunit_kzalloc(test, sizeof(*t), GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, t);
	t->dcp = kunit_kzalloc(test, sizeof(*t->dcp), GFP_KERNEL);
	t->asrv = kunit_kzalloc(test, sizeof(*t->asrv), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, t->dcp);
	KUNIT_ASSERT_NOT_NULL(test, t->asrv);
	t->dcp->audiosrv = t->asrv;
	t->dcp->avep = ERR_PTR(-ENODEV);
	av_audiosrv_quiesce(t->dcp);
	av_audiosrv_reports_restart(t->dcp);
	KUNIT_EXPECT_FALSE(test, t->asrv->works_live);
	KUNIT_EXPECT_FALSE(test, t->asrv->work_disabled);
	KUNIT_EXPECT_FALSE(test, t->asrv->reports_stopped);
	KUNIT_EXPECT_EQ(test, PTR_ERR(t->dcp->avep), -ENODEV);
}

/* Failed start stays disarmed across a bind restart. */
static void av_lifetime_failed_start_stays_stopped(struct kunit *test)
{
	struct av_life_test *t = av_life_setup(test, av_life_owner_fn);

	t->dcp->avep = ERR_PTR(-EIO);
	av_audiosrv_block_owner_restart(t->asrv);
	av_audiosrv_quiesce(t->dcp);
	av_audiosrv_reports_restart(t->dcp);
	KUNIT_EXPECT_TRUE(test, t->asrv->work_disabled);
	KUNIT_EXPECT_TRUE(test, t->asrv->reports_stopped);
	KUNIT_EXPECT_FALSE(test, schedule_work(&t->asrv->terminate_wq));
	KUNIT_EXPECT_EQ(test, PTR_ERR(t->dcp->avep), -EIO);
	KUNIT_EXPECT_EQ(test, atomic_read(&t->owner_runs), 0);
}


/* The fake context action runs where afk_release_context would run. */
static void av_life_endpoint_release(void *data)
{
	struct av_life_test *t = data;

	t->release_saw_stopped = t->release_owner->reports_stopped &&
				 t->release_owner->work_disabled;
	t->release_saw_quiesced = !t->release_ep->wq;
	t->endpoint_released = true;
	/* Keep a missing-quiesce mutant from leaking the actual queue. */
	afk_quiesce(t->release_ep);
}

static void av_life_release_group(struct av_life_test *t)
{
	devres_release_group(t->dev, t->group);
	t->group = NULL;
	t->group_released = true;
	t->asrv = NULL;
	t->ep = NULL;
	t->wq = NULL;
}

static struct av_life_test *av_life_group_setup(struct kunit *test,
					      work_func_t fn, int endpoint)
{
	struct av_life_test *t = av_life_setup(test, fn);

	t->group = devres_open_group(t->dev, NULL, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, t->group);
	t->asrv = devm_kzalloc(t->dev, sizeof(*t->asrv), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, t->asrv);
	init_rwsem(&t->asrv->srv_rwsem);
	INIT_WORK(&t->asrv->start_av_service_wq, fn);
	INIT_WORK(&t->asrv->terminate_wq, fn);
	t->asrv->works_live = true;
	t->dcp->audiosrv = t->asrv;
	if (endpoint > 0) {
		t->ep = devm_kzalloc(t->dev, sizeof(*t->ep), GFP_KERNEL);
		KUNIT_ASSERT_NOT_NULL(test, t->ep);
		spin_lock_init(&t->ep->lock);
		t->ep->dcp = t->dcp;
		t->wq = alloc_ordered_workqueue("av-lifetime-devres", 0);
		KUNIT_ASSERT_NOT_ERR_OR_NULL(test, t->wq);
		t->ep->wq = t->wq;
		t->release_ep = t->ep;
		t->release_owner = t->asrv;
		KUNIT_ASSERT_EQ(test, devm_add_action_or_reset(t->dev,
					av_life_endpoint_release, t), 0);
	} else {
		t->ep = endpoint < 0 ? ERR_PTR(endpoint) : NULL;
	}
	t->dcp->avep = t->ep;
	return t;
}

static void av_lifetime_devres_order(struct kunit *test)
{
	struct av_life_test *t = av_life_group_setup(test, av_life_blocked_fn, 1);
	unsigned long left;

	KUNIT_ASSERT_EQ(test, av_audiosrv_register_cleanup(t->dcp, t->asrv, t->ep), 0);
	devres_close_group(t->dev, t->group);
	KUNIT_ASSERT_TRUE(test, schedule_work(&t->asrv->terminate_wq));
	left = wait_for_completion_timeout(&t->entered, HZ);
	KUNIT_ASSERT_NE(test, left, 0UL);
	t->stop_release_group = true;
	av_life_parallel_stop(test, t);
	KUNIT_EXPECT_TRUE(test, t->endpoint_released);
	KUNIT_EXPECT_TRUE(test, t->release_saw_stopped);
	KUNIT_EXPECT_TRUE(test, t->release_saw_quiesced);
	KUNIT_EXPECT_PTR_EQ(test, t->dcp->audiosrv, NULL);
	KUNIT_EXPECT_PTR_EQ(test, t->dcp->avep, NULL);
	if (!t->dcp->audiosrv)
		av_audiosrv_quiesce(t->dcp);
}

static void av_lifetime_devres_null_endpoint(struct kunit *test)
{
	struct av_life_test *t = av_life_group_setup(test, av_life_owner_fn, 0);

	KUNIT_ASSERT_EQ(test, av_audiosrv_register_cleanup(t->dcp, t->asrv, NULL), 0);
	devres_close_group(t->dev, t->group);
	av_life_release_group(t);
	KUNIT_EXPECT_PTR_EQ(test, t->dcp->audiosrv, NULL);
	KUNIT_EXPECT_PTR_EQ(test, t->dcp->avep, NULL);
}

static void av_lifetime_devres_error_endpoint(struct kunit *test)
{
	struct av_life_test *t = av_life_group_setup(test, av_life_owner_fn, -ENOMEM);

	KUNIT_ASSERT_EQ(test, av_audiosrv_register_cleanup(t->dcp, t->asrv, t->ep), 0);
	devres_close_group(t->dev, t->group);
	av_life_release_group(t);
	KUNIT_EXPECT_PTR_EQ(test, t->dcp->audiosrv, NULL);
	KUNIT_EXPECT_PTR_EQ(test, t->dcp->avep, NULL);
}

/* Model only devm's allocation boundary; execute its real reset callback. */
static int av_life_fail_add_action(struct device *dev, void (*action)(void *), void *data)
{
	action(data);
	return -ENOMEM;
}

static void av_lifetime_add_action_reset(struct kunit *test)
{
	struct av_life_test *t = av_life_group_setup(test, av_life_owner_fn, 1);

	kunit_activate_static_stub(test, av_audiosrv_add_action, av_life_fail_add_action);
	KUNIT_EXPECT_EQ(test, av_audiosrv_register_cleanup(t->dcp, t->asrv, t->ep), -ENOMEM);
	KUNIT_EXPECT_TRUE(test, t->asrv->restart_blocked);
	KUNIT_EXPECT_TRUE(test, t->asrv->work_disabled);
	KUNIT_EXPECT_TRUE(test, t->asrv->reports_stopped);
	KUNIT_EXPECT_PTR_EQ(test, t->dcp->audiosrv, NULL);
	KUNIT_EXPECT_PTR_EQ(test, t->dcp->avep, NULL);
	KUNIT_EXPECT_PTR_EQ(test, t->ep->wq, NULL);
	/* The endpoint action still belongs to this group and is idempotent. */
	devres_close_group(t->dev, t->group);
	av_life_release_group(t);
	KUNIT_EXPECT_TRUE(test, t->release_saw_stopped);
	KUNIT_EXPECT_TRUE(test, t->release_saw_quiesced);
}

static void av_lifetime_stale_owner_shared_error(struct kunit *test)
{
	struct av_life_test *t = av_life_group_setup(test, av_life_owner_fn, -ENOMEM);
	struct audiosrv_data *next = kunit_kzalloc(test, sizeof(*next), GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, next);
	KUNIT_ASSERT_EQ(test, av_audiosrv_register_cleanup(t->dcp, t->asrv, t->ep), 0);
	devres_close_group(t->dev, t->group);
	t->dcp->audiosrv = next;
	/* A shared ERR_PTR value identifies no particular endpoint owner. */
	t->dcp->avep = ERR_PTR(-ENOMEM);
	av_life_release_group(t);
	KUNIT_EXPECT_PTR_EQ(test, t->dcp->audiosrv, next);
	KUNIT_EXPECT_PTR_EQ(test, t->dcp->avep, ERR_PTR(-ENOMEM));
}

static void av_lifetime_devres_preserves_successor(struct kunit *test)
{
	struct av_life_test *t = av_life_group_setup(test, av_life_owner_fn, 1);
	struct audiosrv_data *next = kunit_kzalloc(test, sizeof(*next), GFP_KERNEL);
	struct apple_dcp_afkep *ep = kunit_kzalloc(test, sizeof(*ep), GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, next);
	KUNIT_ASSERT_NOT_NULL(test, ep);
	KUNIT_ASSERT_EQ(test, av_audiosrv_register_cleanup(t->dcp, t->asrv, t->ep), 0);
	devres_close_group(t->dev, t->group);
	t->dcp->audiosrv = next;
	t->dcp->avep = ep;
	av_life_release_group(t);
	KUNIT_EXPECT_TRUE(test, t->release_saw_stopped);
	KUNIT_EXPECT_TRUE(test, t->release_saw_quiesced);
	KUNIT_EXPECT_PTR_EQ(test, t->dcp->audiosrv, next);
	KUNIT_EXPECT_PTR_EQ(test, t->dcp->avep, ep);
	KUNIT_EXPECT_FALSE(test, next->reports_stopped);
	KUNIT_EXPECT_FALSE(test, next->work_disabled);
}


/* Releasing one group cannot leave restart state on the next owner. */
static void av_lifetime_devres_fresh_rebind(struct kunit *test)
{
	struct av_life_test *t = av_life_group_setup(test, av_life_owner_fn, 0);

	KUNIT_ASSERT_EQ(test, av_audiosrv_register_cleanup(t->dcp, t->asrv, NULL), 0);
	devres_close_group(t->dev, t->group);
	av_life_release_group(t);
	KUNIT_EXPECT_PTR_EQ(test, t->dcp->audiosrv, NULL);
	t->group = devres_open_group(t->dev, NULL, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, t->group);
	t->group_released = false;
	t->asrv = devm_kzalloc(t->dev, sizeof(*t->asrv), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, t->asrv);
	init_rwsem(&t->asrv->srv_rwsem);
	INIT_WORK(&t->asrv->start_av_service_wq, av_life_owner_fn);
	INIT_WORK(&t->asrv->terminate_wq, av_life_owner_fn);
	t->asrv->works_live = true;
	t->dcp->audiosrv = t->asrv;
	KUNIT_ASSERT_EQ(test, av_audiosrv_register_cleanup(t->dcp, t->asrv, NULL), 0);
	devres_close_group(t->dev, t->group);
	av_audiosrv_reports_restart(t->dcp);
	KUNIT_EXPECT_FALSE(test, t->asrv->work_disabled);
	KUNIT_EXPECT_FALSE(test, t->asrv->restart_blocked);
	KUNIT_ASSERT_TRUE(test, schedule_work(&t->asrv->start_av_service_wq));
	flush_work(&t->asrv->start_av_service_wq);
	KUNIT_EXPECT_EQ(test, atomic_read(&t->owner_runs), 1);
	av_life_release_group(t);
	KUNIT_EXPECT_PTR_EQ(test, t->dcp->audiosrv, NULL);
}

/* Common unbind owns one producer disable, balanced by one bind enable. */
static void av_lifetime_producer_rebind_balance(struct kunit *test)
{
	struct av_life_test *t = av_life_setup(test, av_life_owner_fn);

	t->dcp->external_native = true;
	t->dcp->ext_dead = true;
	t->stop_unbind = true;
	dcp_comp_unbind(t->dev, NULL, NULL);
	KUNIT_EXPECT_FALSE(test, schedule_work(&t->dcp->external_audio_work));
	KUNIT_EXPECT_TRUE(test, enable_work(&t->dcp->external_audio_work));
	av_audiosrv_reports_restart(t->dcp);
	KUNIT_EXPECT_TRUE(test, schedule_work(&t->dcp->external_audio_work));
	flush_work(&t->dcp->external_audio_work);
	KUNIT_EXPECT_EQ(test, atomic_read(&t->owner_runs), 1);
	dcp_comp_unbind(t->dev, NULL, NULL);
	KUNIT_EXPECT_FALSE(test, schedule_work(&t->dcp->external_audio_work));
	KUNIT_EXPECT_TRUE(test, enable_work(&t->dcp->external_audio_work));
	av_audiosrv_reports_restart(t->dcp);
	KUNIT_EXPECT_TRUE(test, schedule_work(&t->dcp->external_audio_work));
	flush_work(&t->dcp->external_audio_work);
	KUNIT_EXPECT_EQ(test, atomic_read(&t->owner_runs), 2);
}

static struct kunit_case av_lifetime_cases[] = {
	KUNIT_CASE(av_lifetime_devres_fresh_rebind),
	KUNIT_CASE(av_lifetime_producer_rebind_balance),
	KUNIT_CASE(av_lifetime_devres_order),
	KUNIT_CASE(av_lifetime_devres_null_endpoint),
	KUNIT_CASE(av_lifetime_devres_error_endpoint),
	KUNIT_CASE(av_lifetime_add_action_reset),
	KUNIT_CASE(av_lifetime_stale_owner_shared_error),
	KUNIT_CASE(av_lifetime_devres_preserves_successor),
	KUNIT_CASE(av_lifetime_drain_before_dead),
	KUNIT_CASE(av_lifetime_err_ptr_endpoint),
	KUNIT_CASE(av_lifetime_pending_owner_cancelled),
	KUNIT_CASE(av_lifetime_unbind_waits_for_initializer),
	KUNIT_CASE(av_lifetime_quiesce_once_restart_once),
	KUNIT_CASE(av_lifetime_report_after_stop),
	KUNIT_CASE(av_lifetime_control_without_drain),
	KUNIT_CASE(av_lifetime_never_inited),
	KUNIT_CASE(av_lifetime_failed_start_stays_stopped),
	{}
};

static struct kunit_suite av_lifetime_suite = {
	.name = "apple-av-audio-lifetime",
	.test_cases = av_lifetime_cases,
};

kunit_test_suite(av_lifetime_suite);
