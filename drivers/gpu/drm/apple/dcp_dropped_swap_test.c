// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* Copyright 2026 Ryan Murray */

#include <kunit/platform_device.h>
#include <kunit/test.h>
#include <linux/jiffies.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/workqueue.h>

#include <drm/drm_device.h>
#include <drm/drm_framebuffer.h>

#include "dcp.h"
#include "dcp-internal.h"

/*
 * A dock leaves during the first mode set of an external H17P display.  The
 * firmware fails the mode set, accepts the next present and drops it without
 * a completion or an abort.  The teardown that follows waits for the IOMFB
 * queue to settle.  This replays that sequence through the real queue, swap
 * watchdog and external stop; the firmware is the test, which answers the
 * present as the receive callbacks would and never completes it.
 */
#define DROPPED_SWAP_ID	162

struct dropped_test;

struct dropped_present {
	struct iomfb_transaction transaction;
	struct dropped_test *t;
	struct iomfb_scanout_h17p *scanout;
	u32 swap_id;
	bool started, released;
};

struct dropped_test {
	struct apple_dcp dcp;
	/* Only its event lock: the watchdog completes the flip. */
	struct drm_device drm;
	struct apple_crtc crtc;
	struct platform_device *pdev;
	/* fb[0] is on screen, and present i shows fb[i + 1]. */
	struct drm_framebuffer fb[4];
	struct dropped_present present[3];
	unsigned int freed;
};

static void dropped_test_noop(struct work_struct *work)
{
}

static void dropped_test_noop_delayed(struct work_struct *work)
{
}

static void dropped_test_fb_free(struct kref *kref)
{
	struct drm_framebuffer *fb = container_of(kref, struct drm_framebuffer,
						  base.refcount);
	struct dropped_test *t = fb->dev ? container_of(fb->dev, struct dropped_test, drm) : NULL;

	if (t)
		t->freed++;
}

static unsigned int dropped_test_refs(struct drm_framebuffer *fb)
{
	return kref_read(&fb->base.refcount);
}

/* As iomfb_atomic_start() and the accepted submit reply of dcp_swapped(). */
static void dropped_test_start(struct apple_dcp *dcp, struct iomfb_transaction *transaction)
{
	struct dropped_present *p = container_of(transaction, struct dropped_present, transaction);

	dcp->iomfb.next_scanout = p->scanout;
	p->started = dcp_present_begin_h17p(&dcp->present_state_h17p, p->swap_id) &&
		     dcp_present_submit_h17p(&dcp->present_state_h17p, p->swap_id, true);
	dcp_swap_watchdog_arm(dcp);
}

/* As iomfb_atomic_release(). */
static void dropped_test_release(struct iomfb_transaction *transaction)
{
	struct dropped_present *p = container_of(transaction, struct dropped_present, transaction);

	iomfb_scanout_put_h17p(&p->t->dcp, p->scanout);
	p->released = true;
}

static struct iomfb_scanout_h17p *
dropped_test_scanout(struct kunit *test, struct drm_framebuffer *fb)
{
	struct iomfb_scanout_h17p *scanout = kzalloc_obj(*scanout);

	KUNIT_ASSERT_NOT_NULL(test, scanout);
	INIT_LIST_HEAD(&scanout->link);
	scanout->fb[0] = fb;
	drm_framebuffer_get(fb);
	return scanout;
}

static void dropped_test_scanout_free(struct iomfb_scanout_h17p *scanout)
{
	unsigned int i;

	for (i = 0; i < SWAP_SURFACES; i++)
		if (scanout->fb[i])
			drm_framebuffer_put(scanout->fb[i]);
	kfree(scanout);
}

static void dropped_test_exit(void *data)
{
	struct dropped_test *t = data;
	struct apple_dcp *dcp = &t->dcp;
	struct iomfb_scanout_h17p *scanout, *next;

	cancel_delayed_work_sync(&dcp->swap_watchdog_wq);
	cancel_delayed_work_sync(&dcp->external_stop_retry_wq);
	iomfb_queue_stop(dcp);
	guard(mutex)(&dcp->iomfb.lock);
	list_for_each_entry_safe(scanout, next, &dcp->iomfb.dropped, link) {
		list_del(&scanout->link);
		dropped_test_scanout_free(scanout);
	}
	if (dcp->iomfb.next_scanout && dcp->iomfb.next_scanout != dcp->iomfb.scanout)
		dropped_test_scanout_free(dcp->iomfb.next_scanout);
	if (dcp->iomfb.scanout)
		dropped_test_scanout_free(dcp->iomfb.scanout);
}

static struct dropped_test *dropped_test_init(struct kunit *test,
					      enum dcp_firmware_version fw, int profile)
{
	struct dropped_test *t = kunit_kzalloc(test, sizeof(*t), GFP_KERNEL);
	struct apple_dcp *dcp;
	unsigned int i;

	KUNIT_ASSERT_NOT_NULL(test, t);
	dcp = &t->dcp;
	t->pdev = kunit_platform_device_alloc(test, "dcp-dropped", PLATFORM_DEVID_AUTO);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, t->pdev);
	KUNIT_ASSERT_EQ(test, kunit_platform_device_add(test, t->pdev), 0);
	platform_set_drvdata(t->pdev, dcp);
	dcp->dev = &t->pdev->dev;
	dcp->fw_compat = fw;
	dcp->hw.iomfb_method_profile = profile;
	dcp->fixed_connector_type = DRM_MODE_CONNECTOR_USB;

	iomfb_queue_init(dcp);
	mutex_init(&dcp->external_lock);
	mutex_init(&dcp->hpd_mutex);
	mutex_init(&dcp->swapped_out_fbs_lock);
	INIT_LIST_HEAD(&dcp->swapped_out_fbs);
	spin_lock_init(&dcp->mode_state.lock);
	spin_lock_init(&dcp->backlight.lock);
	INIT_WORK(&dcp->vblank_wq, dropped_test_noop);
	INIT_DELAYED_WORK(&dcp->swap_watchdog_wq, dcp_swap_watchdog);
	INIT_DELAYED_WORK(&dcp->typec_reconnect_wq, dropped_test_noop_delayed);
	INIT_DELAYED_WORK(&dcp->placeholder_edid_wq, dropped_test_noop_delayed);
	/* A retried stop would send the power-off, which this test cannot answer. */
	INIT_DELAYED_WORK(&dcp->external_stop_retry_wq, dropped_test_noop_delayed);

	spin_lock_init(&t->drm.event_lock);
	t->crtc.base.dev = &t->drm;
	dcp->crtc = &t->crtc;
	dcp->mode_state.valid = true;

	for (i = 0; i < ARRAY_SIZE(t->fb); i++) {
		t->fb[i].dev = &t->drm;
		kref_init(&t->fb[i].base.refcount);
		t->fb[i].base.free_cb = dropped_test_fb_free;
	}
	/* The display shows fb[0] from a present completed before the plug. */
	dcp->iomfb.scanout = dropped_test_scanout(test, &t->fb[0]);
	for (i = 0; i < ARRAY_SIZE(t->present); i++) {
		t->present[i].t = t;
		t->present[i].swap_id = DROPPED_SWAP_ID + i;
		t->present[i].transaction.start = dropped_test_start;
		t->present[i].transaction.release = dropped_test_release;
	}
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, dropped_test_exit, t), 0);
	return t;
}

static struct dropped_test *dropped_test_external(struct kunit *test)
{
	return dropped_test_init(test, DCP_FIRMWARE_H17P, DCP_IOMFB_METHODS_H17P_EXTERNAL);
}

/* Queue present @i and let the queue send it; the firmware accepts it. */
static void dropped_test_present(struct kunit *test, struct dropped_test *t, unsigned int i,
				 struct drm_framebuffer *fb)
{
	struct dropped_present *p = &t->present[i];

	p->scanout = dropped_test_scanout(test, fb);
	KUNIT_ASSERT_EQ(test, iomfb_queue(&t->dcp, &p->transaction), 0);
	flush_work(&t->dcp.iomfb.work);
	KUNIT_ASSERT_TRUE(test, p->started);
}

/* As dcpep_cb_swap_complete() for @swap_id on the receive path. */
static bool dropped_test_complete(struct dropped_test *t, u32 swap_id)
{
	struct apple_dcp *dcp = &t->dcp;

	guard(mutex)(&dcp->iomfb.lock);
	if (!dcp_present_complete_h17p(&dcp->present_state_h17p, swap_id))
		return false;
	iomfb_present_complete_h17p(dcp);
	dcp_swap_watchdog_complete(dcp);
	return true;
}

/* The firmware completes @swap_id: the real receive callback handles it. */
static int dropped_test_deliver(struct dropped_test *t, u32 swap_id)
{
	guard(mutex)(&t->dcp.iomfb.lock);
	return iomfb_h17p_deliver_swap_complete(&t->dcp, swap_id);
}

/* Present @i is dropped: the watchdog retires it and retrains the display. */
static void dropped_test_retire(struct kunit *test, struct dropped_test *t, unsigned int i)
{
	dropped_test_present(test, t, i, &t->fb[i + 1]);
	KUNIT_ASSERT_EQ(test, iomfb_queue_settle(&t->dcp, msecs_to_jiffies(2000)), 0);
	/* The queue settles before the watchdog retrains, outside the lock. */
	flush_delayed_work(&t->dcp.swap_watchdog_wq);
	KUNIT_ASSERT_TRUE(test, dcp_present_was_retired_h17p(&t->dcp.present_state_h17p,
							     t->present[i].swap_id));
}

/*
 * The captured sequence: the teardown's queue settle times out on the dropped
 * present.  The swap watchdog retires it a second after it was accepted, so a
 * retry settles, the queue moves on and the coprocessor is not declared
 * crashed.
 */
static void dropped_present_retired_test(struct kunit *test)
{
	struct dropped_test *t = dropped_test_external(test);
	struct apple_dcp *dcp = &t->dcp;

	dropped_test_present(test, t, 0, &t->fb[1]);
	KUNIT_EXPECT_EQ(test, iomfb_queue_settle(dcp, msecs_to_jiffies(500)), -ETIMEDOUT);

	KUNIT_EXPECT_EQ(test, iomfb_queue_settle(dcp, msecs_to_jiffies(2000)), 0);
	KUNIT_EXPECT_FALSE(test, READ_ONCE(dcp->crashed));
	KUNIT_EXPECT_TRUE(test, t->present[0].released);
	KUNIT_EXPECT_FALSE(test, delayed_work_pending(&dcp->iomfb.timeout));
	KUNIT_EXPECT_FALSE(test, dcp->present_state_h17p.pending);
	KUNIT_EXPECT_TRUE(test, dcp_present_was_retired_h17p(&dcp->present_state_h17p,
							     DROPPED_SWAP_ID));
	/* The firmware did not abort it. */
	KUNIT_EXPECT_FALSE(test, dcp_present_was_aborted_h17p(&dcp->present_state_h17p,
							      DROPPED_SWAP_ID));
	/*
	 * The display was retrained as before.  The queue settles as soon as
	 * the present is retired; the watchdog retrains after that, outside
	 * the IOMFB lock.
	 */
	flush_delayed_work(&dcp->swap_watchdog_wq);
	KUNIT_EXPECT_FALSE(test, READ_ONCE(dcp->mode_state.valid));
}

/*
 * The firmware might still read a dropped present: its framebuffers, and the
 * one it would have replaced, stay referenced until a later present
 * completes.  A late completion of the dropped present releases nothing.
 */
static void dropped_present_buffers_test(struct kunit *test)
{
	struct dropped_test *t = dropped_test_external(test);
	struct apple_dcp *dcp = &t->dcp;

	dropped_test_present(test, t, 0, &t->fb[1]);
	KUNIT_ASSERT_EQ(test, iomfb_queue_settle(dcp, msecs_to_jiffies(2000)), 0);
	KUNIT_EXPECT_EQ(test, dropped_test_refs(&t->fb[1]), 2);
	KUNIT_EXPECT_EQ(test, dropped_test_refs(&t->fb[0]), 2);
	KUNIT_EXPECT_FALSE(test, list_empty(&dcp->iomfb.dropped));

	KUNIT_EXPECT_FALSE(test, dropped_test_complete(t, DROPPED_SWAP_ID));
	KUNIT_EXPECT_TRUE(test, dcp_present_was_retired_h17p(&dcp->present_state_h17p,
							     DROPPED_SWAP_ID));
	KUNIT_EXPECT_EQ(test, dropped_test_refs(&t->fb[1]), 2);

	/* The next mode set's present is sent and completes. */
	dropped_test_present(test, t, 1, &t->fb[2]);
	KUNIT_EXPECT_TRUE(test, dropped_test_complete(t, DROPPED_SWAP_ID + 1));
	KUNIT_EXPECT_TRUE(test, list_empty(&dcp->iomfb.dropped));
	KUNIT_EXPECT_EQ(test, dropped_test_refs(&t->fb[1]), 1);
	KUNIT_EXPECT_EQ(test, dropped_test_refs(&t->fb[0]), 1);
	KUNIT_EXPECT_EQ(test, dropped_test_refs(&t->fb[2]), 2);
	KUNIT_EXPECT_PTR_EQ(test, dcp->iomfb.scanout, t->present[1].scanout);
	KUNIT_EXPECT_EQ(test, t->freed, 0);
}

/*
 * A watchdog that was already running when its present completed must not
 * retire the next present, which was accepted just now.
 */
static void completed_present_kept_test(struct kunit *test)
{
	struct dropped_test *t = dropped_test_external(test);
	struct apple_dcp *dcp = &t->dcp;

	dropped_test_present(test, t, 0, &t->fb[1]);
	KUNIT_ASSERT_TRUE(test, dropped_test_complete(t, DROPPED_SWAP_ID));
	/* As iomfb_queue_advance() after the completion. */
	scoped_guard(mutex, &dcp->iomfb.lock) {
		dcp->iomfb.active = NULL;
		cancel_delayed_work(&dcp->iomfb.timeout);
		t->present[0].transaction.release(&t->present[0].transaction);
	}
	dropped_test_present(test, t, 1, &t->fb[2]);

	dcp_swap_watchdog(&dcp->swap_watchdog_wq.work);
	KUNIT_EXPECT_TRUE(test, dcp->present_state_h17p.pending);
	KUNIT_EXPECT_FALSE(test, dcp_present_was_aborted_h17p(&dcp->present_state_h17p,
							      DROPPED_SWAP_ID + 1));
	KUNIT_EXPECT_FALSE(test, t->present[1].released);
	KUNIT_EXPECT_TRUE(test, READ_ONCE(dcp->mode_state.valid));
}

/*
 * The teardown of the captured sequence: a queue that does not settle before
 * the power-off is sent leaves no firmware request outstanding.  The stop is
 * not latched and keeps everything owned; once the watchdog has retired the
 * dropped present, a retry finds the queue settled.
 */
static void stop_deferred_test(struct kunit *test)
{
	struct dropped_test *t = dropped_test_external(test);
	struct apple_dcp *dcp = &t->dcp;

	dcp->external_prepared = true;
	dcp->external_cable = true;
	dcp->external_power_attempted = true;
	dcp->external_running = true;
	dcp->dptxport[0].connected = true;
	dcp->pipe_enabled_h17p = true;
	dropped_test_present(test, t, 0, &t->fb[1]);

	dcp_poweroff(t->pdev);
	KUNIT_EXPECT_EQ(test, dcp->external_error, 0);
	KUNIT_EXPECT_FALSE(test, dcp->external_stopped);
	KUNIT_EXPECT_TRUE(test, READ_ONCE(dcp->pipe_enabled_h17p));
	KUNIT_EXPECT_TRUE(test, READ_ONCE(dcp->dptxport[0].connected));
	KUNIT_EXPECT_TRUE(test, dcp->iomfb.presentation_paused);
	/* Nothing else retries a CRTC that is off: the stop retries itself. */
	KUNIT_EXPECT_TRUE(test, dcp->external_stop_deferred);
	KUNIT_EXPECT_TRUE(test, delayed_work_pending(&dcp->external_stop_retry_wq));

	KUNIT_EXPECT_EQ(test, iomfb_queue_settle(dcp, msecs_to_jiffies(2000)), 0);
	KUNIT_EXPECT_FALSE(test, READ_ONCE(dcp->crashed));
}

/*
 * Without a completed present for IOMFB_DROPPED_MAX dropped ones, the next one
 * is not retired: the display is retrained as before and the queue is left to
 * its timeout.
 */
static void dropped_present_bounded_test(struct kunit *test)
{
	struct dropped_test *t = dropped_test_external(test);
	struct apple_dcp *dcp = &t->dcp;
	struct dcp_present_state_h17p *state = &dcp->present_state_h17p;
	unsigned int i;

	/* As IOMFB_DROPPED_MAX retirements leave the present state and list. */
	for (i = 0; i < IOMFB_DROPPED_MAX; i++) {
		KUNIT_ASSERT_TRUE(test, dcp_present_begin_h17p(state, 100 + i));
		KUNIT_ASSERT_TRUE(test, dcp_present_submit_h17p(state, 100 + i, true));
		KUNIT_ASSERT_TRUE(test, dcp_present_retire_h17p(state, 100 + i));
		list_add_tail(&dropped_test_scanout(test, &t->fb[0])->link, &dcp->iomfb.dropped);
	}
	dropped_test_present(test, t, 0, &t->fb[1]);
	KUNIT_EXPECT_EQ(test, iomfb_queue_settle(dcp, msecs_to_jiffies(1500)), -ETIMEDOUT);
	/* The watchdog has given up on the present; let it finish retraining. */
	flush_delayed_work(&dcp->swap_watchdog_wq);
	KUNIT_EXPECT_FALSE(test, READ_ONCE(dcp->mode_state.valid));
	KUNIT_EXPECT_TRUE(test, state->pending);
	KUNIT_EXPECT_FALSE(test, dcp_present_was_retired_h17p(state, DROPPED_SWAP_ID));
	KUNIT_EXPECT_FALSE(test, t->present[0].released);
	KUNIT_EXPECT_EQ(test, list_count_nodes(&dcp->iomfb.dropped), IOMFB_DROPPED_MAX);
}

/* Other firmware keeps the watchdog it had: no present state is touched. */
static void dcp_dropped_legacy_watchdog(struct kunit *test, enum dcp_firmware_version fw,
					int profile)
{
	struct dropped_test *t = dropped_test_init(test, fw, profile);
	struct apple_dcp *dcp = &t->dcp;

	dcp->present_state_h17p.pending = true;
	dcp->present_state_h17p.accepted = true;
	dcp->swap_watchdog_deadline = jiffies + HZ;
	dcp_swap_watchdog(&dcp->swap_watchdog_wq.work);
	KUNIT_EXPECT_FALSE(test, READ_ONCE(dcp->mode_state.valid));
	KUNIT_EXPECT_TRUE(test, dcp->present_state_h17p.pending);
	KUNIT_EXPECT_FALSE(test, dcp->present_state_h17p.has_last_aborted);
	KUNIT_EXPECT_EQ(test, dcp->present_state_h17p.nr_retired, 0U);
	dcp->present_state_h17p.pending = false;
}

static void legacy_watchdog_test(struct kunit *test)
{
	dcp_dropped_legacy_watchdog(test, DCP_FIRMWARE_V_13_5, 0);
}

/* H17P firmware with the H17G method profile keeps its transport too. */
static void h17g_watchdog_test(struct kunit *test)
{
	dcp_dropped_legacy_watchdog(test, DCP_FIRMWARE_H17P, DCP_IOMFB_METHODS_H17G);
}

/*
 * Two plugs in a row drop their present: the watchdog retires 162, a mode set
 * succeeds, and it retires 163.  Both scanouts are kept.  The firmware then
 * completes 162 after all: a late completion, not an unexpected one.  The
 * coprocessor is not declared crashed, nothing is released, and the next stop
 * finds the queue settled.
 */
static void older_retired_completion_test(struct kunit *test)
{
	struct dropped_test *t = dropped_test_external(test);
	struct apple_dcp *dcp = &t->dcp;

	dropped_test_retire(test, t, 0);
	/* The recovery mode set succeeds. */
	WRITE_ONCE(dcp->mode_state.valid, true);
	dropped_test_retire(test, t, 1);
	KUNIT_ASSERT_EQ(test, list_count_nodes(&dcp->iomfb.dropped), 2);
	KUNIT_ASSERT_FALSE(test, READ_ONCE(dcp->crashed));

	KUNIT_ASSERT_EQ(test, dropped_test_deliver(t, DROPPED_SWAP_ID), 0);
	KUNIT_EXPECT_FALSE(test, READ_ONCE(dcp->crashed));
	KUNIT_EXPECT_EQ(test, list_count_nodes(&dcp->iomfb.dropped), 2);
	KUNIT_EXPECT_EQ(test, dropped_test_refs(&t->fb[0]), 2);
	KUNIT_EXPECT_EQ(test, dropped_test_refs(&t->fb[1]), 2);
	KUNIT_EXPECT_EQ(test, dropped_test_refs(&t->fb[2]), 2);
	KUNIT_ASSERT_EQ(test, dropped_test_deliver(t, DROPPED_SWAP_ID + 1), 0);
	KUNIT_EXPECT_FALSE(test, READ_ONCE(dcp->crashed));
	KUNIT_EXPECT_EQ(test, iomfb_queue_settle(dcp, msecs_to_jiffies(100)), 0);

	/* A completion of a present that was never sent is still unexpected. */
	KUNIT_ASSERT_EQ(test, dropped_test_deliver(t, DROPPED_SWAP_ID - 1), 0);
	KUNIT_EXPECT_TRUE(test, READ_ONCE(dcp->crashed));
}

/*
 * As above, with present 164 of the next mode set in flight when 162
 * completes: that completion neither completes 164 nor stops its watchdog,
 * and releases nothing.  The completion of 164 shows that the firmware is past
 * 162 and 163: their scanouts are released and they are forgotten, so a
 * completion of 162 after that is unexpected again.
 */
static void retired_completion_in_flight_test(struct kunit *test)
{
	struct dropped_test *t = dropped_test_external(test);
	struct apple_dcp *dcp = &t->dcp;
	struct dcp_present_state_h17p *state = &dcp->present_state_h17p;

	dropped_test_retire(test, t, 0);
	dropped_test_retire(test, t, 1);
	dropped_test_present(test, t, 2, &t->fb[3]);

	KUNIT_ASSERT_EQ(test, dropped_test_deliver(t, DROPPED_SWAP_ID), 0);
	KUNIT_EXPECT_FALSE(test, READ_ONCE(dcp->crashed));
	KUNIT_EXPECT_TRUE(test, state->pending);
	KUNIT_EXPECT_EQ(test, state->swap_id, DROPPED_SWAP_ID + 2);
	KUNIT_EXPECT_PTR_EQ(test, dcp->iomfb.active, &t->present[2].transaction);
	KUNIT_EXPECT_FALSE(test, t->present[2].transaction.completed);
	KUNIT_EXPECT_TRUE(test, delayed_work_pending(&dcp->swap_watchdog_wq));
	KUNIT_EXPECT_EQ(test, list_count_nodes(&dcp->iomfb.dropped), 2);
	KUNIT_EXPECT_EQ(test, dropped_test_refs(&t->fb[1]), 2);
	KUNIT_EXPECT_EQ(test, dropped_test_refs(&t->fb[2]), 2);
	KUNIT_EXPECT_EQ(test, dropped_test_refs(&t->fb[0]), 2);

	KUNIT_ASSERT_EQ(test, dropped_test_deliver(t, DROPPED_SWAP_ID + 2), 0);
	KUNIT_EXPECT_FALSE(test, READ_ONCE(dcp->crashed));
	KUNIT_EXPECT_FALSE(test, state->pending);
	KUNIT_EXPECT_TRUE(test, t->present[2].transaction.completed);
	KUNIT_EXPECT_FALSE(test, delayed_work_pending(&dcp->swap_watchdog_wq));
	KUNIT_EXPECT_TRUE(test, list_empty(&dcp->iomfb.dropped));
	KUNIT_EXPECT_EQ(test, dropped_test_refs(&t->fb[1]), 1);
	KUNIT_EXPECT_EQ(test, dropped_test_refs(&t->fb[2]), 1);
	KUNIT_EXPECT_EQ(test, dropped_test_refs(&t->fb[0]), 1);
	KUNIT_EXPECT_PTR_EQ(test, dcp->iomfb.scanout, t->present[2].scanout);
	KUNIT_EXPECT_FALSE(test, dcp_present_was_retired_h17p(state, DROPPED_SWAP_ID));
	KUNIT_EXPECT_FALSE(test, dcp_present_was_retired_h17p(state, DROPPED_SWAP_ID + 1));

	KUNIT_ASSERT_EQ(test, dropped_test_deliver(t, DROPPED_SWAP_ID), 0);
	KUNIT_EXPECT_TRUE(test, READ_ONCE(dcp->crashed));
}

static void desktop_present_completion_test(struct kunit *test)
{
	struct apple_dcp *dcp = kunit_kzalloc(test, sizeof(*dcp), GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, dcp);
	dcp->desktop = kunit_kzalloc(test, sizeof(*dcp->desktop), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, dcp->desktop);
	dcp->desktop->present_id = 42;
	dcp->desktop->present_pending = true;
	KUNIT_EXPECT_FALSE(test, dcp_desktop_present_complete(dcp, 41));
	KUNIT_EXPECT_TRUE(test, dcp->desktop->present_pending);
	KUNIT_EXPECT_TRUE(test, dcp_desktop_present_complete(dcp, 42));
	KUNIT_EXPECT_FALSE(test, dcp_desktop_present_complete(dcp, 42));
	/* A late completion cannot claim the replacement's event. */
	dcp->desktop->present_id = 43;
	dcp->desktop->present_pending = true;
	KUNIT_EXPECT_FALSE(test, dcp_desktop_present_complete(dcp, 42));
	KUNIT_EXPECT_TRUE(test, dcp->desktop->present_pending);
	KUNIT_EXPECT_TRUE(test, dcp_desktop_present_complete(dcp, 43));
}

static void takeover_adopt_watched_test(struct kunit *test)
{
	struct apple_dcp *dcp = kunit_kzalloc(test, sizeof(*dcp), GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, dcp);
	dcp->desktop = kunit_kzalloc(test, sizeof(*dcp->desktop), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, dcp->desktop);
	dcp->fw_compat = DCP_FIRMWARE_V_26_6_DESKTOP;
	KUNIT_EXPECT_FALSE(test, dcp_desktop_swap_watched(dcp));
	/* An adopted loader mode is watched until a swap completes on it. */
	dcp->desktop->takeover_adopted = true;
	KUNIT_EXPECT_TRUE(test, dcp_desktop_swap_watched(dcp));
	dcp_takeover_adopt_proven(dcp);
	KUNIT_EXPECT_FALSE(test, dcp->desktop->takeover_adopted);
	KUNIT_EXPECT_FALSE(test, dcp_desktop_swap_watched(dcp));
	dcp->vrr_enabled = true;
	KUNIT_EXPECT_TRUE(test, dcp_desktop_swap_watched(dcp));
	/* Other firmware never sees the flag. */
	dcp->vrr_enabled = false;
	dcp->desktop->takeover_adopted = true;
	dcp->fw_compat = DCP_FIRMWARE_H17P;
	KUNIT_EXPECT_FALSE(test, dcp_desktop_swap_watched(dcp));
}

static struct kunit_case dropped_swap_cases[] = {
	KUNIT_CASE(desktop_present_completion_test),
	KUNIT_CASE(takeover_adopt_watched_test),
	KUNIT_CASE(dropped_present_retired_test),
	KUNIT_CASE(dropped_present_buffers_test),
	KUNIT_CASE(completed_present_kept_test),
	KUNIT_CASE(stop_deferred_test),
	KUNIT_CASE(dropped_present_bounded_test),
	KUNIT_CASE(legacy_watchdog_test),
	KUNIT_CASE(h17g_watchdog_test),
	KUNIT_CASE(older_retired_completion_test),
	KUNIT_CASE(retired_completion_in_flight_test),
	{}
};

static struct kunit_suite dropped_swap_suite = {
	.name = "apple-dcp-dropped-swap",
	.test_cases = dropped_swap_cases,
};

kunit_test_suite(dropped_swap_suite);
