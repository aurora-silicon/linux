// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * KUnit tests for the kernel-drawn camera-in-use indicator.
 */

#include <kunit/test.h>
#include <linux/slab.h>
#include <linux/soc/apple/camera-indicator.h>
#include <linux/unaligned.h>
#include <linux/workqueue.h>
#include <drm/drm_gem_dma_helper.h>

#include "connector.h"
#include "dcp.h"
#include "dcp-camera-indicator.h"
#include "dcp-internal.h"

#define CAMIND_TEST_IOVA	0x12340000ULL

static u32 camind_px(const u32 *pixels, int x, int y)
{
	return pixels[y * (DCP_CAMIND_PITCH / 4) + x];
}

/* An opaque disc with an opaque ring, transparent corners. */
static void camind_test_draw(struct kunit *test)
{
	u32 *pixels = kunit_kzalloc(test, DCP_CAMIND_PITCH * DCP_CAMIND_SIZE,
				    GFP_KERNEL);
	int c = DCP_CAMIND_SIZE / 2;

	KUNIT_ASSERT_NOT_NULL(test, pixels);
	dcp_camind_draw(pixels);
	KUNIT_EXPECT_EQ(test, camind_px(pixels, c, c) >> 24, 0xffu);
	KUNIT_EXPECT_NE(test, camind_px(pixels, c, c) & 0xffffff, 0u);
	/* Ring: opaque black between the disc and the edge of the circle. */
	KUNIT_EXPECT_EQ(test, camind_px(pixels, c + 18, c), 0xff000000u);
	KUNIT_EXPECT_EQ(test, camind_px(pixels, 0, 0), 0u);
	KUNIT_EXPECT_EQ(test, camind_px(pixels, DCP_CAMIND_SIZE - 1,
					DCP_CAMIND_SIZE - 1), 0u);
}

/* Top right, below the notch, never off the panel. */
static void camind_test_place(struct kunit *test)
{
	struct dcp_rect dst;

	KUNIT_ASSERT_TRUE(test, dcp_camind_place(2408, 1506, 0, &dst));
	KUNIT_EXPECT_EQ(test, dst.x, 2408u - DCP_CAMIND_SIZE - DCP_CAMIND_MARGIN);
	KUNIT_EXPECT_EQ(test, dst.y, (u32)DCP_CAMIND_MARGIN);
	KUNIT_EXPECT_EQ(test, dst.w, (u32)DCP_CAMIND_SIZE);
	KUNIT_EXPECT_EQ(test, dst.h, (u32)DCP_CAMIND_SIZE);

	KUNIT_ASSERT_TRUE(test, dcp_camind_place(3024, 1890, 74, &dst));
	KUNIT_EXPECT_EQ(test, dst.y, 74u + DCP_CAMIND_MARGIN);

	KUNIT_EXPECT_FALSE(test, dcp_camind_place(0, 0, 0, &dst));
	KUNIT_EXPECT_FALSE(test, dcp_camind_place(32, 1000, 0, &dst));
}

static void camind_test_noop(struct work_struct *work)
{
}

static void camind_test_cleanup(void *data)
{
	struct apple_dcp *dcp = data;

	cancel_delayed_work_sync(&dcp->camind.work);
	apple_cam_indicator_set_shown(false);
}

static struct apple_dcp *camind_test_dcp(struct kunit *test)
{
	struct apple_dcp *dcp = kunit_kzalloc(test, sizeof(*dcp), GFP_KERNEL);
	struct dcp_camind *ci;

	KUNIT_ASSERT_NOT_NULL(test, dcp);
	ci = &dcp->camind;
	ci->obj = kunit_kzalloc(test, sizeof(*ci->obj), GFP_KERNEL);
	dcp->connector = kunit_kzalloc(test, sizeof(*dcp->connector), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, ci->obj);
	KUNIT_ASSERT_NOT_NULL(test, dcp->connector);
	ci->obj->dma_addr = CAMIND_TEST_IOVA;
	ci->slot = 2;
	dcp_camind_surface(&ci->surf, false);
	ci->registered = true;
	ci->mode_w = 2408;
	ci->mode_h = 1506;
	INIT_DELAYED_WORK(&ci->work, camind_test_noop);
	spin_lock_init(&dcp->backlight.lock);
	dcp->connector->connected = true;
	dcp->mode_state.valid = true;
	dcp->brightness.maximum = 500;
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, camind_test_cleanup, dcp),
			0);
	return dcp;
}

/*
 * Every present while it is wanted draws it in its own surface, on top of
 * whatever the compositor presented, and its completion confirms it.
 */
static void camind_test_present(struct kunit *test)
{
	struct apple_dcp *dcp = camind_test_dcp(test);
	struct dcp_swap_submit_req_h17p *req;
	struct dcp_camind *ci = &dcp->camind;

	req = kunit_kzalloc(test, sizeof(*req), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, req);
	req->swap.swap_enabled = BIT(0);
	req->swap.swap_completed = BIT(0);
	req->surf_null[2] = true;

	/* Not wanted: the present is left alone. */
	dcp_camind_prepare(dcp, req, 6);
	KUNIT_EXPECT_EQ(test, req->swap.swap_enabled, BIT(0));
	KUNIT_EXPECT_TRUE(test, req->surf_null[2]);

	ci->want = true;
	dcp_camind_prepare(dcp, req, 7);
	KUNIT_EXPECT_EQ(test, req->swap.swap_enabled, BIT(0) | BIT(2));
	KUNIT_EXPECT_EQ(test, req->swap.swap_completed, BIT(0) | BIT(2));
	KUNIT_EXPECT_FALSE(test, req->surf_null[2]);
	KUNIT_EXPECT_EQ(test, req->surf_iova[2], CAMIND_TEST_IOVA);
	KUNIT_EXPECT_EQ(test, req->surf[2].base.width, (u32)DCP_CAMIND_SIZE);
	KUNIT_EXPECT_EQ(test, req->swap.dst_rect[2].x,
			2408u - DCP_CAMIND_SIZE - DCP_CAMIND_MARGIN);
	KUNIT_EXPECT_FALSE(test, apple_cam_indicator_shown());

	/* Another present's completion confirms nothing. */
	dcp_camind_complete(dcp, 6);
	KUNIT_EXPECT_EQ(test, atomic_read(&ci->confirmed), 0);
	dcp_camind_complete(dcp, 7);
	KUNIT_EXPECT_EQ(test, atomic_read(&ci->confirmed), 1);
	KUNIT_EXPECT_TRUE(test, apple_cam_indicator_shown());

	/* Lost at once on a power-off, sleep or mode set. */
	dcp_camind_lost(dcp);
	KUNIT_EXPECT_EQ(test, atomic_read(&ci->confirmed), 0);
	KUNIT_EXPECT_FALSE(test, apple_cam_indicator_shown());
	KUNIT_EXPECT_TRUE(test, dcp_camind_pending(dcp));
}

/* A present that was in flight across a loss does not bring it back. */
static void camind_test_stale_completion(struct kunit *test)
{
	struct apple_dcp *dcp = camind_test_dcp(test);
	struct dcp_swap_submit_req_h17p *req;
	struct dcp_camind *ci = &dcp->camind;

	req = kunit_kzalloc(test, sizeof(*req), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, req);
	ci->want = true;
	dcp_camind_prepare(dcp, req, 8);
	dcp_camind_lost(dcp);
	dcp_camind_complete(dcp, 8);
	KUNIT_EXPECT_EQ(test, atomic_read(&ci->confirmed), 0);
	KUNIT_EXPECT_FALSE(test, apple_cam_indicator_shown());
}

/* With the panel blanked a completed present proves nothing. */
static void camind_test_panel_off(struct kunit *test)
{
	struct apple_dcp *dcp = camind_test_dcp(test);
	struct dcp_swap_submit_req_h17p *req;
	struct dcp_camind *ci = &dcp->camind;

	req = kunit_kzalloc(test, sizeof(*req), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, req);
	ci->want = true;
	dcp->backlight.state.dpms_off = true;
	dcp_camind_prepare(dcp, req, 9);
	dcp_camind_complete(dcp, 9);
	KUNIT_EXPECT_EQ(test, atomic_read(&ci->confirmed), 0);

	dcp->backlight.state.dpms_off = false;
	dcp->mode_state.valid = false;
	dcp_camind_prepare(dcp, req, 10);
	dcp_camind_complete(dcp, 10);
	KUNIT_EXPECT_EQ(test, atomic_read(&ci->confirmed), 0);

	/* Without a mode it cannot be placed, so it is not drawn. */
	dcp->mode_state.valid = true;
	ci->mode_w = 0;
	memset(req, 0, sizeof(*req));
	dcp_camind_prepare(dcp, req, 11);
	KUNIT_EXPECT_EQ(test, req->swap.swap_enabled, 0u);
	dcp_camind_complete(dcp, 11);
	KUNIT_EXPECT_EQ(test, atomic_read(&ci->confirmed), 0);
}

/* Released: the next present takes it off, once. */
static void camind_test_clear(struct kunit *test)
{
	struct apple_dcp *dcp = camind_test_dcp(test);
	struct dcp_swap_submit_req_h17p *req;
	struct dcp_camind *ci = &dcp->camind;

	req = kunit_kzalloc(test, sizeof(*req), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, req);
	ci->want = false;
	ci->clear = true;
	KUNIT_EXPECT_TRUE(test, dcp_camind_pending(dcp));
	dcp_camind_prepare(dcp, req, 12);
	KUNIT_EXPECT_EQ(test, req->swap.swap_enabled, BIT(2));
	KUNIT_EXPECT_TRUE(test, req->surf_null[2]);
	KUNIT_EXPECT_FALSE(test, ci->clear);
	KUNIT_EXPECT_FALSE(test, dcp_camind_pending(dcp));

	memset(req, 0, sizeof(*req));
	dcp_camind_prepare(dcp, req, 13);
	KUNIT_EXPECT_EQ(test, req->swap.swap_enabled, 0u);
}

/* Wanted, the backlight goes no lower than the floor, and comes back after. */
static void camind_test_backlight_floor(struct kunit *test)
{
	struct apple_dcp *dcp = camind_test_dcp(test);
	struct dcp_present_h17p *wire;
	struct dcp_camind *ci = &dcp->camind;

	wire = kunit_kzalloc(test, sizeof(*wire), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, wire);

	ci->want = true;
	dcp_camind_backlight(dcp, wire, 200);
	KUNIT_EXPECT_FALSE(test, ci->floor);
	KUNIT_EXPECT_EQ(test, wire->swap[0x354], 0);

	dcp_camind_backlight(dcp, wire, 10);
	KUNIT_EXPECT_TRUE(test, ci->floor);
	KUNIT_EXPECT_EQ(test, wire->swap[0x354], 1);
	/* 50 nits as binary64. */
	KUNIT_EXPECT_EQ(test, get_unaligned_le64(wire->swap + 0x35e),
			0x4049000000000000ULL);

	memset(wire, 0, sizeof(*wire));
	ci->want = false;
	dcp_camind_backlight(dcp, wire, 10);
	KUNIT_EXPECT_FALSE(test, ci->floor);
	KUNIT_EXPECT_EQ(test, wire->swap[0x354], 1);
	KUNIT_EXPECT_EQ(test, get_unaligned_le64(wire->swap + 0x35e),
			0x4024000000000000ULL);

	/* Then left alone. */
	memset(wire, 0, sizeof(*wire));
	dcp_camind_backlight(dcp, wire, 10);
	KUNIT_EXPECT_EQ(test, wire->swap[0x354], 0);
}

static struct kunit_case camind_test_cases[] = {
	KUNIT_CASE(camind_test_draw),
	KUNIT_CASE(camind_test_place),
	KUNIT_CASE(camind_test_present),
	KUNIT_CASE(camind_test_stale_completion),
	KUNIT_CASE(camind_test_panel_off),
	KUNIT_CASE(camind_test_clear),
	KUNIT_CASE(camind_test_backlight_floor),
	{}
};

static struct kunit_suite camind_test_suite = {
	.name = "apple-dcp-camera-indicator",
	.test_cases = camind_test_cases,
};

kunit_test_suite(camind_test_suite);
