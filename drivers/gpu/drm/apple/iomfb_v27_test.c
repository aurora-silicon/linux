// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* Copyright 2026 Aurora Silicon */

/*
 * Firmware 27 is selected by the firmware-compat a DCP reports, on every SoC:
 * 27 or later runs the firmware-27 instance when the build has a complete
 * method table and is not started otherwise; older firmware is left to the
 * older interfaces. These tests cover that rule and the in-tree tables.
 */

#include <drm/drm_drv.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem_dma_helper.h>
#include <drm/drm_kunit_helpers.h>
#include <kunit/test.h>
#include <linux/bitfield.h>
#include <linux/dma-mapping.h>
#include <linux/jiffies.h>
#include <linux/kref.h>
#include <linux/module.h>
#include <linux/string.h>
#include <linux/unaligned.h>
#include <linux/workqueue.h>

#include "dcp.h"
#include "dcp-internal.h"
#include "iomfb.h"
#include "iomfb_internal.h"
#include "iomfb_v27.h"

#define V27_IDLE_TEST_SWAP_ID	77

struct v27_idle_test {
	struct drm_device drm;
	struct device *dev;
	struct apple_dcp dcp;
	struct apple_crtc crtc;
	struct apple_connector connector;
	struct work_struct ack_work;
	u64 pending_ack;
	bool has_pending_ack;
	bool fail_submit;
	bool saw_negotiate;
	bool saw_swap_start;
	bool saw_swap_submit;
	bool submit_ch_armed;
	u32 submit_swap_id;
	u32 submit_intent;
	u32 submit_surf_id;
	dma_addr_t submit_iova;
	unsigned int sentinel_calls;
	void *sentinel_cookie;
};

static void v27_idle_test_noop(struct work_struct *work)
{
}

static void v27_idle_test_ack_work(struct work_struct *work)
{
	struct v27_idle_test *t = container_of(work, struct v27_idle_test, ack_work);

	if (t->has_pending_ack) {
		t->has_pending_ack = false;
		iomfb_recv_msg(&t->dcp, t->pending_ack);
	}
}

static int v27_idle_test_send(struct apple_dcp *dcp, u8 endpoint, u64 message)
{
	struct v27_idle_test *t = container_of(dcp, struct v27_idle_test, dcp);
	enum dcp_context_id ctx = FIELD_GET(IOMFB_MSG_CONTEXT, message);
	u16 offset = FIELD_GET(IOMFB_MSG_OFFSET, message);
	struct dcp_packet_header *hdr;
	void *in, *out;

	if (endpoint != IOMFB_ENDPOINT || ctx != DCP_CONTEXT_CMD)
		return 0;

	hdr = (struct dcp_packet_header *)(dcp->shmem + offset);
	in = hdr + 1;
	out = (u8 *)in + hdr->in_len;

	if (!memcmp(hdr->tag, "253A", 4)) {
		t->saw_negotiate = true;
		put_unaligned_le32(0, out);
	} else if (!memcmp(hdr->tag, "604A", 4)) {
		struct dcp_swap_start_resp_h17p resp = {
			.swap_id = V27_IDLE_TEST_SWAP_ID,
			.ret = 0,
		};

		t->saw_swap_start = true;
		memcpy(out, &resp, sizeof(resp));
	} else if (!memcmp(hdr->tag, "804A", 4)) {
		const struct dcp_swap_submit_req_v27 *req = in;

		t->saw_swap_submit = true;
		t->submit_swap_id = req->swap.swap_id;
		t->submit_intent = req->swap.swap_intent;
		t->submit_surf_id = req->swap.surf_ids[0];
		t->submit_iova = req->surf_iova[0];
		t->submit_ch_armed = dcp->ch_cmd.depth == 1 &&
				     dcp->ch_cmd.callbacks[0];
		if (t->fail_submit)
			return -EAGAIN;
		memset(out, 0, sizeof(struct dcp_swap_submit_resp_v27));
	} else {
		return 0;
	}

	t->pending_ack = FIELD_PREP(IOMFB_MESSAGE_TYPE, IOMFB_MESSAGE_TYPE_MSG) |
			 FIELD_PREP(IOMFB_MSG_CONTEXT, ctx) | IOMFB_MSG_ACK;
	t->has_pending_ack = true;
	schedule_work(&t->ack_work);
	return 0;
}

static int v27_idle_test_send_no_ack(struct apple_dcp *dcp, u8 endpoint,
				     u64 message)
{
	(void)dcp;
	(void)endpoint;
	(void)message;
	return 0;
}

static void v27_idle_test_sentinel_cb(struct apple_dcp *dcp, void *out, void *cookie)
{
	struct v27_idle_test *t = container_of(dcp, struct v27_idle_test, dcp);

	t->sentinel_calls++;
	t->sentinel_cookie = cookie;
}

static void v27_idle_test_exit(void *data)
{
	struct v27_idle_test *t = data;
	struct apple_dcp *dcp = &t->dcp;

	cancel_delayed_work_sync(&dcp->v27_idle_cache_work);
	cancel_work_sync(&t->ack_work);
	iomfb_v27_idle_cache_cancel(dcp);
	iomfb_queue_stop(dcp);
	dcp_test_release_retained_session(dcp->dev);
	WRITE_ONCE(dcp->retain_dma, false);
	iomfb_v27_idle_cache_reset(dcp);
	iomfb_v27_idle_cache_release_obj(dcp);
}

static struct v27_idle_test *v27_idle_test_init(struct kunit *test, bool fail_submit)
{
	struct v27_idle_test *t;
	struct apple_dcp *dcp;
	struct device *dev;
	int ret;

	dev = drm_kunit_helper_alloc_device(test);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, dev);
	ret = dma_coerce_mask_and_coherent(dev, DMA_BIT_MASK(64));
	KUNIT_ASSERT_EQ(test, ret, 0);

	t = drm_kunit_helper_alloc_drm_device(test, dev, struct v27_idle_test, drm,
					      DRIVER_GEM | DRIVER_MODESET |
					      DRIVER_ATOMIC);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, t);
	t->dev = dev;
	t->fail_submit = fail_submit;
	INIT_WORK(&t->ack_work, v27_idle_test_ack_work);

	dcp = &t->dcp;
	dcp->dev = dev;
	dcp->fw_compat = DCP_FIRMWARE_V_27_0;
	dcp->v27_methods = iomfb_v27_methods("27.0.0");
	KUNIT_ASSERT_NOT_NULL(test, dcp->v27_methods);
	dcp->main_display = true;
	dcp->panel.width_mm = 300;
	dcp->active = true;
	t->crtc.base.dev = &t->drm;
	dcp->crtc = &t->crtc;
	t->connector.connected = true;
	dcp->connector = &t->connector;
	dcp->mode_state.valid = true;
	dcp->shmem = kunit_kzalloc(test, 0x10000, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, dcp->shmem);

	iomfb_queue_init(dcp);
	INIT_WORK(&dcp->vblank_wq, v27_idle_test_noop);
	INIT_DELAYED_WORK(&dcp->swap_watchdog_wq, v27_idle_test_noop);
	INIT_DELAYED_WORK(&dcp->backlight.retry_work, v27_idle_test_noop);
	dcp->test_send = v27_idle_test_send;

	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, v27_idle_test_exit, t), 0);
	return t;
}

static void v27_idle_test_trigger_worker(struct apple_dcp *dcp)
{
	unsigned long flags;

	spin_lock_irqsave(&dcp->v27_idle_lock, flags);
	dcp->v27_idle_deadline = jiffies;
	spin_unlock_irqrestore(&dcp->v27_idle_lock, flags);
	mod_delayed_work(system_dfl_wq, &dcp->v27_idle_cache_work, 0);
	flush_delayed_work(&dcp->v27_idle_cache_work);
}

static void v27_idle_test_present_static(struct v27_idle_test *t)
{
	struct dcp_surface surf = {
		.width = 64,
		.height = 64,
		.stride = 256,
		.buf_size = 64 * 256,
		.plane_cnt = 1,
		.plane_cnt2 = 1,
		.has_planes = true,
		.planes = {
			[0] = {
				.width = 64,
				.height = 64,
				.stride = 256,
				.size = 64 * 256,
			},
		},
	};
	struct dcp_rect rect = { .w = 64, .h = 64 };

	iomfb_v27_idle_cache_on_present(&t->dcp, &surf, &rect, &rect, true);
	iomfb_v27_idle_cache_on_displayed(&t->dcp);
}

/*
 * Injecting a failed mailbox send on swap_submit after the cache DMA surface
 * and session retention are prepared must unwind the command channel, release
 * the uncommitted session references, leave DMA unretained, and preserve
 * subsequent channel callbacks and DMA ownership cleanup.
 */
static void v27_idle_failed_submit_unwind_test(struct kunit *test)
{
	struct v27_idle_test *t = v27_idle_test_init(test, true);
	struct apple_dcp *dcp = &t->dcp;
	unsigned int base_dev_refs = kref_read(&dcp->dev->kobj.kref);
	int sentinel_cookie = 0x5150;
	u32 prop = 0;

	v27_idle_test_present_static(t);

	/* Phase 1: negotiate replay support. */
	v27_idle_test_trigger_worker(dcp);
	flush_work(&dcp->iomfb.work);
	flush_work(&t->ack_work);
	KUNIT_ASSERT_TRUE(test, t->saw_negotiate);
	KUNIT_ASSERT_TRUE(test, dcp->v27_idle_method_set);

	/* Phase 2: prepare DMA/IOVA + session, send swap_start, fail swap_submit. */
	v27_idle_test_trigger_worker(dcp);
	KUNIT_ASSERT_NOT_NULL(test, dcp->v27_idle_obj);
	flush_work(&dcp->iomfb.work);
	flush_work(&t->ack_work);
	cancel_delayed_work_sync(&dcp->v27_idle_cache_work);

	KUNIT_EXPECT_TRUE(test, t->saw_swap_start);
	KUNIT_EXPECT_TRUE(test, t->saw_swap_submit);
	KUNIT_EXPECT_TRUE(test, t->submit_ch_armed);
	KUNIT_EXPECT_EQ(test, t->submit_swap_id, V27_IDLE_TEST_SWAP_ID);
	KUNIT_EXPECT_EQ(test, t->submit_intent, 8U);
	KUNIT_EXPECT_EQ(test, t->submit_surf_id, 2U);
	KUNIT_EXPECT_EQ(test, t->submit_iova, dcp->v27_idle_obj->dma_addr);

	/* Channel slot 0 was unwound cleanly and the active tx was retired. */
	KUNIT_EXPECT_EQ(test, dcp->ch_cmd.depth, 0);
	KUNIT_EXPECT_NULL(test, dcp->ch_cmd.callbacks[0]);
	KUNIT_EXPECT_PTR_EQ(test, dcp->ch_cmd.cookies[0], NULL);
	KUNIT_EXPECT_PTR_EQ(test, dcp->ch_cmd.output[0], NULL);
	KUNIT_EXPECT_EQ(test, dcp->ch_cmd.end[0], 0);
	KUNIT_EXPECT_PTR_EQ(test, dcp->iomfb.active, NULL);

	/* Idle-cache and session state rolled back; no sticky swap filter. */
	KUNIT_EXPECT_FALSE(test, dcp->v27_idle_in_flight);
	KUNIT_EXPECT_FALSE(test, dcp->v27_idle_submitting);
	KUNIT_EXPECT_FALSE(test, dcp->v27_idle_swap_pending);
	KUNIT_EXPECT_EQ(test, dcp->v27_idle_swap_id, 0U);
	KUNIT_EXPECT_FALSE(test, iomfb_v27_idle_cache_swap_complete(dcp, V27_IDLE_TEST_SWAP_ID));
	KUNIT_EXPECT_FALSE(test, dcp->v27_idle_dma_pinned);
	KUNIT_EXPECT_FALSE(test, dcp->retain_dma);
	KUNIT_EXPECT_FALSE(test, dcp_test_session_retained(dcp->dev));
	KUNIT_EXPECT_EQ(test, kref_read(&dcp->dev->kobj.kref), base_dev_refs);

	/* Subsequent commands on the unwound channel preserve callback delivery. */
	dcp->test_send = v27_idle_test_send_no_ack;
	mutex_lock(&dcp->iomfb.lock);
	WRITE_ONCE(dcp->iomfb.owner, current);
	dcp_push(dcp, false, &dcp->v27_methods[dcpep_pipe_set_352],
		 sizeof(prop), sizeof(prop), &prop,
		 v27_idle_test_sentinel_cb, &sentinel_cookie);
	WRITE_ONCE(dcp->iomfb.owner, NULL);
	mutex_unlock(&dcp->iomfb.lock);
	KUNIT_EXPECT_EQ(test, dcp->ch_cmd.depth, 1);
	iomfb_recv_msg(dcp, FIELD_PREP(IOMFB_MESSAGE_TYPE, IOMFB_MESSAGE_TYPE_MSG) |
			    FIELD_PREP(IOMFB_MSG_CONTEXT, DCP_CONTEXT_CMD) |
			    IOMFB_MSG_ACK);
	KUNIT_EXPECT_EQ(test, t->sentinel_calls, 1U);
	KUNIT_EXPECT_PTR_EQ(test, t->sentinel_cookie, &sentinel_cookie);
	KUNIT_EXPECT_EQ(test, dcp->ch_cmd.depth, 0);

	/* Closing admission releases the unpinned DMA surface cleanly. */
	iomfb_v27_idle_cache_cancel(dcp);
	iomfb_queue_stop(dcp);
	iomfb_v27_idle_cache_release_obj(dcp);
	KUNIT_EXPECT_PTR_EQ(test, dcp->v27_idle_obj, NULL);
	KUNIT_EXPECT_EQ(test, dcp->v27_idle_obj_size, (size_t)0);
}

static int v27_idle_test_fail_second_send(struct apple_dcp *dcp, u8 endpoint, u64 message)
{
	struct v27_idle_test *t = container_of(dcp, struct v27_idle_test, dcp);

	(void)endpoint;
	(void)message;
	if (t->saw_swap_submit)
		return -EAGAIN;
	t->saw_swap_submit = true;
	return 0;
}

/*
 * Unwinding a nested command failure at depth > 0 must preserve an outer slot's
 * callback and cookie, and a prior DMA pin must survive a failed re-submit
 * until reset while a clean submit commits session retention.
 */
static void v27_idle_unwind_preserves_outer_callback_and_pin_test(struct kunit *test)
{
	struct v27_idle_test *t = v27_idle_test_init(test, true);
	struct apple_dcp *dcp = &t->dcp;
	unsigned int base_dev_refs;
	int outer_cookie = 0x1111, inner_cookie = 0x2222;
	u32 prop = 0;
	int outer_ret, ret = 0;
	u8 outer_depth;

	/* Outer slot 0 stays intact when inner slot 1 unwinds on send error. */
	dcp->test_send = v27_idle_test_fail_second_send;
	mutex_lock(&dcp->iomfb.lock);
	WRITE_ONCE(dcp->iomfb.owner, current);
	outer_ret = iomfb_test_push_context(dcp, DCP_CONTEXT_CMD,
					    &dcp->v27_methods[dcpep_pipe_set_352],
					    sizeof(prop), sizeof(prop), &prop,
					    v27_idle_test_sentinel_cb,
					    &outer_cookie, true);
	outer_depth = dcp->ch_cmd.depth;
	if (!outer_ret && outer_depth == 1)
		ret = iomfb_test_push_context(dcp, DCP_CONTEXT_CMD,
					      &dcp->v27_methods[dcpep_pipe_set_352],
					      sizeof(prop), sizeof(prop), &prop,
					      v27_idle_test_sentinel_cb,
					      &inner_cookie, true);
	WRITE_ONCE(dcp->iomfb.owner, NULL);
	mutex_unlock(&dcp->iomfb.lock);
	KUNIT_ASSERT_EQ(test, outer_ret, 0);
	KUNIT_ASSERT_EQ(test, outer_depth, 1);
	KUNIT_EXPECT_EQ(test, ret, -EAGAIN);
	KUNIT_EXPECT_EQ(test, dcp->ch_cmd.depth, 1);
	KUNIT_EXPECT_NULL(test, dcp->ch_cmd.callbacks[1]);
	KUNIT_EXPECT_PTR_EQ(test, dcp->ch_cmd.cookies[1], NULL);
	KUNIT_EXPECT_PTR_EQ(test, dcp->ch_cmd.output[1], NULL);
	KUNIT_EXPECT_EQ(test, dcp->ch_cmd.end[1], 0);
	KUNIT_EXPECT_TRUE(test, dcp->ch_cmd.callbacks[0] == v27_idle_test_sentinel_cb);
	KUNIT_EXPECT_PTR_EQ(test, dcp->ch_cmd.cookies[0], &outer_cookie);

	iomfb_recv_msg(dcp, FIELD_PREP(IOMFB_MESSAGE_TYPE, IOMFB_MESSAGE_TYPE_MSG) |
			    FIELD_PREP(IOMFB_MSG_CONTEXT, DCP_CONTEXT_CMD) |
			    IOMFB_MSG_ACK);
	KUNIT_EXPECT_EQ(test, t->sentinel_calls, 1U);
	KUNIT_EXPECT_PTR_EQ(test, t->sentinel_cookie, &outer_cookie);
	KUNIT_EXPECT_EQ(test, dcp->ch_cmd.depth, 0);

	/* Prior DMA pin survives failed submit, while new session refs are aborted. */
	t->saw_swap_submit = false;
	dcp->test_send = v27_idle_test_send;
	dcp->v27_idle_method_set = true;
	dcp->v27_idle_dma_pinned = true;
	v27_idle_test_present_static(t);
	base_dev_refs = kref_read(&dcp->dev->kobj.kref);

	v27_idle_test_trigger_worker(dcp);
	KUNIT_ASSERT_NOT_NULL(test, dcp->v27_idle_obj);
	flush_work(&dcp->iomfb.work);
	flush_work(&t->ack_work);
	cancel_delayed_work_sync(&dcp->v27_idle_cache_work);

	KUNIT_EXPECT_TRUE(test, t->saw_swap_submit);
	KUNIT_EXPECT_TRUE(test, dcp->v27_idle_dma_pinned);
	KUNIT_EXPECT_FALSE(test, dcp->retain_dma);
	KUNIT_EXPECT_EQ(test, kref_read(&dcp->dev->kobj.kref), base_dev_refs);
	iomfb_v27_idle_cache_release_obj(dcp);
	KUNIT_EXPECT_NOT_NULL(test, dcp->v27_idle_obj);

	/* Clean submit commits session retention and keeps DMA ownership on unbind. */
	t->fail_submit = false;
	t->saw_swap_submit = false;
	v27_idle_test_trigger_worker(dcp);
	flush_work(&dcp->iomfb.work);
	flush_work(&t->ack_work);
	KUNIT_EXPECT_TRUE(test, t->saw_swap_submit);
	KUNIT_EXPECT_TRUE(test, dcp->retain_dma);
	KUNIT_EXPECT_TRUE(test, dcp_test_session_retained(dcp->dev));
	KUNIT_EXPECT_GT(test, kref_read(&dcp->dev->kobj.kref), base_dev_refs);
	iomfb_v27_idle_cache_release_obj(dcp);
	KUNIT_EXPECT_NOT_NULL(test, dcp->v27_idle_obj);
}

static int v27_idle_test_fb_dirty(struct drm_framebuffer *fb,
				  struct drm_file *file, unsigned int flags,
				  unsigned int color, struct drm_clip_rect *clips,
				  unsigned int num_clips)
{
	return 0;
}

/*
 * Firmware-27 internal panels are eligible by default; profiles opted out via
 * hw.no_v27_idle_cache must never negotiate, allocate, or retain a session.
 */
static void v27_idle_profile_opt_out_test(struct kunit *test)
{
	struct v27_idle_test *t = v27_idle_test_init(test, false);
	struct apple_dcp *dcp = &t->dcp;
	unsigned int base_dev_refs = kref_read(&dcp->dev->kobj.kref);

	KUNIT_EXPECT_TRUE(test, dcp_v27_idle_cache_supported(dcp));

	dcp->hw.no_v27_idle_cache = true;
	KUNIT_EXPECT_FALSE(test, dcp_v27_idle_cache_supported(dcp));

	v27_idle_test_present_static(t);
	KUNIT_EXPECT_FALSE(test, dcp->v27_idle_pending_valid);
	KUNIT_EXPECT_FALSE(test, dcp->v27_idle_template_valid);

	v27_idle_test_trigger_worker(dcp);
	flush_work(&dcp->iomfb.work);
	flush_work(&t->ack_work);

	KUNIT_EXPECT_FALSE(test, t->saw_negotiate);
	KUNIT_EXPECT_FALSE(test, t->saw_swap_start);
	KUNIT_EXPECT_PTR_EQ(test, dcp->v27_idle_obj, NULL);
	KUNIT_EXPECT_FALSE(test, dcp->retain_dma);
	KUNIT_EXPECT_FALSE(test, dcp_test_session_retained(dcp->dev));
	KUNIT_EXPECT_EQ(test, kref_read(&dcp->dev->kobj.kref), base_dev_refs);
}

/*
 * Headless in-kernel client framebuffers (apple_drm_client_fb_funcs without a
 * dirty hook) draw directly in place without KMS commits and must skip
 * idle-cache admission without touching dev->fb_helper, while shadowed fbdev
 * and compositor framebuffers stay eligible.
 */
static void v27_idle_skips_unshadowed_fbdev_test(struct kunit *test)
{
	struct v27_idle_test *t = v27_idle_test_init(test, false);
	struct apple_dcp *dcp = &t->dcp;
	struct drm_framebuffer_funcs plain_funcs = {};
	struct drm_framebuffer_funcs dirty_funcs = {
		.dirty = v27_idle_test_fb_dirty,
	};
	struct drm_framebuffer fbdev_fb = {
		.dev = &t->drm,
		.funcs = &apple_drm_client_fb_funcs,
	};
	struct drm_framebuffer kms_fb = {
		.dev = &t->drm,
		.funcs = &plain_funcs,
	};
	struct dcp_surface surf = {
		.width = 64,
		.height = 64,
		.stride = 256,
		.buf_size = 64 * 256,
		.plane_cnt = 1,
		.plane_cnt2 = 1,
		.has_planes = true,
		.planes = { [0] = { .width = 64, .height = 64, .stride = 256, .size = 64 * 256 } },
	};
	struct dcp_rect rect = { .w = 64, .h = 64 };

	KUNIT_EXPECT_FALSE(test, iomfb_v27_idle_cache_fb_eligible(NULL));
	KUNIT_EXPECT_FALSE(test, iomfb_v27_idle_cache_fb_eligible(&fbdev_fb));
	KUNIT_EXPECT_TRUE(test, iomfb_v27_idle_cache_fb_eligible(&kms_fb));

	fbdev_fb.funcs = &dirty_funcs;
	KUNIT_EXPECT_TRUE(test, iomfb_v27_idle_cache_fb_eligible(&fbdev_fb));
	fbdev_fb.funcs = &apple_drm_client_fb_funcs;

	/* Switching from a compositor FB to unshadowed fbdev clears the template. */
	v27_idle_test_present_static(t);
	KUNIT_ASSERT_TRUE(test, dcp->v27_idle_template_valid);
	iomfb_v27_idle_cache_on_present(dcp, &surf, &rect, &rect,
					iomfb_v27_idle_cache_fb_eligible(&fbdev_fb));
	iomfb_v27_idle_cache_on_displayed(dcp);
	iomfb_v27_idle_cache_kick(dcp);
	KUNIT_EXPECT_FALSE(test, dcp->v27_idle_pending_valid);
	KUNIT_EXPECT_FALSE(test, dcp->v27_idle_template_valid);

	v27_idle_test_trigger_worker(dcp);
	flush_work(&dcp->iomfb.work);
	flush_work(&t->ack_work);
	KUNIT_EXPECT_FALSE(test, t->saw_negotiate);
	KUNIT_EXPECT_PTR_EQ(test, dcp->v27_idle_obj, NULL);
}

static enum dcp_v27_admission admission(const char *compat)
{
	const struct dcp_method_entry *methods;
	unsigned int num_missing;
	const char *missing;

	return dcp_v27_admission(compat, &methods, &missing, &num_missing);
}

/* Builds with a complete table start, whatever SoC reports them. */
static void v27_complete_table_admits_test(struct kunit *test)
{
	const struct dcp_method_entry *methods;
	unsigned int num_missing;
	const char *missing;

	KUNIT_EXPECT_EQ(test, dcp_v27_admission("27.2.0", &methods, &missing,
						&num_missing),
			DCP_V27_ADMITTED);
	KUNIT_EXPECT_PTR_EQ(test, methods, iomfb_v27_methods("27.2.0"));
	KUNIT_EXPECT_PTR_EQ(test, missing, NULL);
	KUNIT_EXPECT_EQ(test, num_missing, 0U);
	KUNIT_EXPECT_EQ(test, admission("27.0.0"), DCP_V27_ADMITTED);
}

/* 27 or later without a usable table is refused, never handed down. */
static void v27_unusable_table_refuses_test(struct kunit *test)
{
	const struct dcp_method_entry *methods;
	unsigned int num_missing;
	const char *missing;

	KUNIT_EXPECT_EQ(test, admission("27.1.0"), DCP_V27_NO_TABLE);
	KUNIT_EXPECT_EQ(test, admission("28.0.0"), DCP_V27_NO_TABLE);
	KUNIT_EXPECT_EQ(test, dcp_v27_admission("27.3.0", &methods, &missing,
						&num_missing),
			DCP_V27_INCOMPLETE);
	KUNIT_EXPECT_NOT_NULL(test, missing);
	KUNIT_EXPECT_GT(test, num_missing, 0U);
}

/* Older firmware keeps the 26.x, 14.x and 13.x selection. */
static void v27_older_firmware_not_v27_test(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, admission("26.6.2"), DCP_V27_NOT_V27);
	KUNIT_EXPECT_EQ(test, admission("26.5.2"), DCP_V27_NOT_V27);
	KUNIT_EXPECT_EQ(test, admission("14.7.0"), DCP_V27_NOT_V27);
	KUNIT_EXPECT_EQ(test, admission("13.5.0"), DCP_V27_NOT_V27);
	KUNIT_EXPECT_EQ(test, admission("12.3.0"), DCP_V27_NOT_V27);
	KUNIT_EXPECT_EQ(test, admission("UNKNOWN"), DCP_V27_NOT_V27);
}

static struct kunit_case v27_cases[] = {
	KUNIT_CASE(v27_complete_table_admits_test),
	KUNIT_CASE(v27_unusable_table_refuses_test),
	KUNIT_CASE(v27_older_firmware_not_v27_test),
	KUNIT_CASE(v27_idle_failed_submit_unwind_test),
	KUNIT_CASE(v27_idle_unwind_preserves_outer_callback_and_pin_test),
	KUNIT_CASE(v27_idle_profile_opt_out_test),
	KUNIT_CASE(v27_idle_skips_unshadowed_fbdev_test),
	{}
};

static struct kunit_suite v27_suite = {
	.name = "apple-dcp-v27",
	.test_cases = v27_cases,
};

kunit_test_suite(v27_suite);

MODULE_LICENSE("Dual MIT/GPL");
