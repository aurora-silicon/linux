// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* Copyright The Asahi Linux Contributors */

/*
 * The J514S (DCP firmware 14.7) present encoding is checked byte for byte:
 * every case's 0x1b58-byte request must match the CRC32 of a reference
 * request recorded for the panel's hardware-qualified profile.
 */

#include <kunit/device.h>
#include <kunit/test.h>
#include <linux/crc32.h>
#include <linux/module.h>
#include <linux/stddef.h>
#include <linux/unaligned.h>

#include "dcp.h"
#include "iomfb_v14_7_present.h"


#define PANEL_W 3024
#define PANEL_H 1964
#define SWAP_SIZE sizeof(struct dcp_swap_submit_req_v14_7)

/* Frames of the representative cases; the backing is the 16K-rounded size. */
static const struct iomfb_v14_7_frame v14_7_frames[] = {
	[1] = {
		.dva = 0x10000004000ULL, .backing = 0x16a8000,
		.width = PANEL_W, .height = PANEL_H, .stride = 12096,
		.alpha = false,
	},
	[2] = {
		.dva = 0x10040000000ULL, .backing = 0x16c8000,
		.width = PANEL_W, .height = PANEL_H, .stride = 12160,
		.alpha = true,
	},
};

enum v14_7_path {
	/* An atomic present: the frame (or blank) with its swap ID and level. */
	V14_7_ATOMIC,
	/* A brightness-only re-present of a completed frame's snapshot. */
	V14_7_REPRESENT,
	/* The soft-DPMS blank, re-encoded from the pinned frame's snapshot. */
	V14_7_DPMS_BLANK,
};

struct v14_7_case {
	const char *name;
	enum v14_7_path path;
	u32 swap_id;
	unsigned int frame;	/* index into v14_7_frames; 0 is the blank */
	bool change;
	u32 nits;
	u32 crc32;		/* standard CRC-32 of the 0x1b58-byte request */
};

static const struct v14_7_case v14_7_cases[] = {
	{ "primary frame", V14_7_ATOMIC, 0x2a, 1, false, 0, 0xf38285f2 },
	{ "first present at 140 nits", V14_7_ATOMIC, 7, 1, true, 140, 0xd05cf8c9 },
	{ "soft DPMS blank at 0 nits", V14_7_DPMS_BLANK, 0x1001, 1, true, 0, 0xdb3c1a63 },
	{ "brightness-only re-present", V14_7_REPRESENT, 0x1002, 1, true, 300, 0xe7387727 },
	{ "ARGB8888 at the 500-nit maximum", V14_7_ATOMIC, 0xfffffffe, 2, true, 500, 0x5909dbe7 },
	{ "background without a level", V14_7_ATOMIC, 1, 0, false, 0, 0xa5a06192 },
	{ "nits change to 1 on a re-present", V14_7_REPRESENT, 3, 2, true, 1, 0x18dcd7c0 },
};

static void v14_7_case_desc(const struct v14_7_case *c, char *desc)
{
	strscpy(desc, c->name, KUNIT_PARAM_DESC_SIZE);
}

KUNIT_ARRAY_PARAM(v14_7_case, v14_7_cases, v14_7_case_desc);

static u32 v14_7_crc(const void *buf)
{
	return crc32_le(~0U, buf, SWAP_SIZE) ^ ~0U;
}

/*
 * Build a case the way the driver does: an atomic present encodes the frame,
 * A406 supplies the swap ID and the backlight policy the level.  The
 * brightness-only paths start from the completed present's snapshot, which
 * has neither.
 */
static void v14_7_encode_case(struct kunit *test, struct dcp_swap_submit_req_v14_7 *req,
			      const struct v14_7_case *c)
{
	const struct iomfb_v14_7_frame *frame = c->frame ? &v14_7_frames[c->frame] : NULL;
	u32 swap_id;

	iomfb_v14_7_encode_present(req, frame);
	if (c->path != V14_7_ATOMIC) {
		/* The snapshot of an earlier present, as completion stores it. */
		req->swap.swap_id = 0x55;
		KUNIT_ASSERT_EQ(test, iomfb_v14_7_encode_nits(req, true, 77), 0);
		req->swap.swap_id = 0;
		KUNIT_ASSERT_EQ(test, iomfb_v14_7_encode_nits(req, false, 0), 0);
	}
	req->swap.swap_id = c->swap_id;
	if (c->path == V14_7_DPMS_BLANK) {
		swap_id = req->swap.swap_id;
		iomfb_v14_7_encode_present(req, NULL);
		req->swap.swap_id = swap_id;
	}
	KUNIT_ASSERT_EQ(test, iomfb_v14_7_encode_nits(req, c->change, c->nits), 0);
}

static void v14_7_present_encoding_test(struct kunit *test)
{
	const struct v14_7_case *c = test->param_value;
	struct dcp_swap_submit_req_v14_7 *req;

	req = kunit_kmalloc(test, SWAP_SIZE, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, req);
	memset(req, 0xa5, SWAP_SIZE);
	v14_7_encode_case(test, req, c);

	KUNIT_EXPECT_TRUE(test, iomfb_v14_7_single_surface(req));
	KUNIT_EXPECT_EQ_MSG(test, v14_7_crc(req), c->crc32, "%s", c->name);

}

/* The wire landmarks of the panel's profile, and its colour bytes. */
static void v14_7_layout_test(struct kunit *test)
{
	struct dcp_swap_submit_req_v14_7 *req;
	const u8 *b;

	KUNIT_EXPECT_EQ(test, SWAP_SIZE, 0x1b58);
	KUNIT_EXPECT_EQ(test, offsetof(struct dcp_swap_submit_req_v14_7, surf), 0x508);
	KUNIT_EXPECT_EQ(test, offsetof(struct dcp_swap_submit_req_v14_7, surf_iova), 0xdb8);
	KUNIT_EXPECT_EQ(test, offsetof(struct dcp_swap_submit_req_v14_7, surf_null), 0x1b4b);
	KUNIT_EXPECT_EQ(test, offsetof(struct dcp_swap_submit_req_v14_7, unknown_pointer_null),
			0x1b56);
	KUNIT_EXPECT_EQ(test, offsetof(struct dcp_swap_v14_7, bl_nits_present), 0x354);
	KUNIT_EXPECT_EQ(test, offsetof(struct dcp_swap_v14_7, bl_nits), 0x35c);
	KUNIT_EXPECT_EQ(test, sizeof(struct dc_swap_complete_resp_v14_7), 0x6f0);

	req = kunit_kzalloc(test, SWAP_SIZE, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, req);
	iomfb_v14_7_encode_present(req, &v14_7_frames[1]);
	b = (const u8 *)req;
	/* Surface 0: transfer 13, primaries 12, premultiplied XRGB, identity 1. */
	KUNIT_EXPECT_EQ(test, b[0x51b], 13);
	KUNIT_EXPECT_EQ(test, b[0x51c], 12);
	KUNIT_EXPECT_EQ(test, b[0x50a], 1);
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(b + 0x508 + 0x35), 1U);
	KUNIT_EXPECT_EQ(test, get_unaligned_le16(b + 0x508 + 0x71), 4U);
	/* Swap: identity 1 in slot 0, per-layer word 0, full masks, black. */
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(b + 0x9c), 1U);
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(b + 0xec), 0U);
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(b + 0x14c), 0x80000007U);
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(b + 0x150), 0x80000007U);
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(b + 0x154), 0xff000000U);
	KUNIT_EXPECT_EQ(test, get_unaligned_le64(b + 0xdb8), v14_7_frames[1].dva);
	KUNIT_EXPECT_EQ(test, b[0x354], 0);
	/* ARGB is straight alpha. */
	iomfb_v14_7_encode_present(req, &v14_7_frames[2]);
	KUNIT_EXPECT_EQ(test, b[0x50a], 0);
}

static void v14_7_nits_test(struct kunit *test)
{
	struct dcp_swap_submit_req_v14_7 *req;
	u32 nits;

	KUNIT_EXPECT_EQ(test, iomfb_v14_7_nits_binary64(0), 0ULL);
	KUNIT_EXPECT_EQ(test, iomfb_v14_7_nits_binary64(1), 0x3ff0000000000000ULL);
	KUNIT_EXPECT_EQ(test, iomfb_v14_7_nits_binary64(140), 0x4061800000000000ULL);
	KUNIT_EXPECT_EQ(test, iomfb_v14_7_nits_binary64(500), 0x407f400000000000ULL);
	req = kunit_kzalloc(test, SWAP_SIZE, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, req);
	/* Above the panel maximum the present is refused. */
	KUNIT_EXPECT_EQ(test, iomfb_v14_7_encode_nits(req, true, 501), -EINVAL);
	KUNIT_EXPECT_EQ(test, ((u8 *)req)[0x354], 0);
	/* Clearing the level leaves no stale value behind. */
	KUNIT_ASSERT_EQ(test, iomfb_v14_7_encode_nits(req, true, 333), 0);
	KUNIT_ASSERT_EQ(test, iomfb_v14_7_encode_nits(req, false, 333), 0);
	for (nits = 0x354; nits < 0x364; nits++)
		KUNIT_EXPECT_EQ(test, ((u8 *)req)[nits], 0);
}

/* Admitted: 16K-aligned, full panel, exact stride, in the scanout window. */
static void v14_7_frame_check_test(struct kunit *test)
{
	static const struct {
		u64 dva, backing;
		u32 width, height, stride;
	} frames[] = {
		{ 0x10000004000ULL, 0x16a8000, PANEL_W, PANEL_H, 12096 },	/* good */
		{ 0x10000004000ULL, 0x16a7000, PANEL_W, PANEL_H, 12096 },	/* short */
		{ 0x10000002000ULL, 0x16a8000, PANEL_W, PANEL_H, 12096 },	/* 8K */
		{ 0x0ff00000000ULL, 0x16a8000, PANEL_W, PANEL_H, 12096 },	/* below */
		{ 0x11000000000ULL, 0x16a8000, PANEL_W, PANEL_H, 12096 },	/* above */
		{ 0x10fff000000ULL, 0x2000000, PANEL_W, PANEL_H, 12096 },	/* crosses */
		{ 0x10000004000ULL, 0x16a8000, PANEL_W, PANEL_H, 12100 },	/* stride */
		{ 0x10000004000ULL, 0x16a8000, PANEL_W, PANEL_H, 12032 },	/* narrow */
		{ 0x10000004000ULL, 0x16a8000, PANEL_W, 1890, 12096 },		/* notch */
		{ 0x10000004000ULL, 0x16a8000, 3008, PANEL_H, 12096 },		/* width */
		{ 0, 0x16a8000, PANEL_W, PANEL_H, 12096 },			/* null */
	};
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(frames); i++) {
		struct iomfb_v14_7_frame f = {
			.dva = frames[i].dva, .backing = frames[i].backing,
			.width = frames[i].width, .height = frames[i].height,
			.stride = frames[i].stride,
		};
		int ret = iomfb_v14_7_frame_check(&f, PANEL_W, PANEL_H);

		KUNIT_EXPECT_EQ_MSG(test, ret, i ? -EINVAL : 0, "frame %u", i);
	}
}

/* Any second surface, layer word or mask is refused before the wire. */
static void v14_7_single_surface_test(struct kunit *test)
{
	struct dcp_swap_submit_req_v14_7 *req;

	req = kunit_kzalloc(test, SWAP_SIZE, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, req);
	iomfb_v14_7_encode_present(req, NULL);
	KUNIT_EXPECT_TRUE(test, iomfb_v14_7_single_surface(req));
	iomfb_v14_7_encode_present(req, &v14_7_frames[1]);
	KUNIT_EXPECT_TRUE(test, iomfb_v14_7_single_surface(req));
	req->surf_null[3] = 0;
	KUNIT_EXPECT_FALSE(test, iomfb_v14_7_single_surface(req));
	iomfb_v14_7_encode_present(req, &v14_7_frames[1]);
	req->swap.surf_flags[0] = 1;
	KUNIT_EXPECT_FALSE(test, iomfb_v14_7_single_surface(req));
	iomfb_v14_7_encode_present(req, &v14_7_frames[1]);
	req->swap.swap_enabled |= BIT(3);
	KUNIT_EXPECT_FALSE(test, iomfb_v14_7_single_surface(req));
	iomfb_v14_7_encode_present(req, &v14_7_frames[1]);
	req->surf2_null[5] = 0;
	KUNIT_EXPECT_FALSE(test, iomfb_v14_7_single_surface(req));
}

static void v14_7_noop_work(struct work_struct *work)
{
}

/* An adopted J514S DCP as far as the presentation policy needs one. */
static struct apple_dcp *v14_7_dcp(struct kunit *test)
{
	struct apple_dcp *dcp;

	dcp = kunit_kzalloc(test, sizeof(*dcp), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, dcp);
	dcp->crtc = kunit_kzalloc(test, sizeof(*dcp->crtc), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, dcp->crtc);
	dcp->dev = kunit_device_register(test, "apple-dcp-v14_7-test");
	KUNIT_ASSERT_FALSE(test, IS_ERR(dcp->dev));
	dcp->fw_compat = DCP_FIRMWARE_V_14_7;
	dcp->connector_type = DRM_MODE_CONNECTOR_eDP;
	dcp->panel.width_mm = 302;
	dcp->panel.height_mm = 196;
	mutex_init(&dcp->bl_register_mutex);
	INIT_WORK(&dcp->bl_register_wq, v14_7_noop_work);
	INIT_DELAYED_WORK(&dcp->swap_watchdog_wq, v14_7_noop_work);
	spin_lock_init(&dcp->backlight.lock);
	mutex_init(&dcp->iomfb.lock);
	return dcp;
}

/* The policy hands A407 its level: what iomfb_swap_prepare_v14_7() sent. */
static bool v14_7_prepare(struct apple_dcp *dcp, struct iomfb_transaction *transaction,
			  const struct dcp_swap_submit_req_v14_7 *request, u32 swap_id)
{
	bool ok;

	mutex_lock(&dcp->iomfb.lock);
	dcp->iomfb.active = transaction;
	dcp->swap_v14_7 = *request;
	dcp->swap_v14_7.swap.swap_id = swap_id;
	ok = iomfb_swap_prepare_v14_7(dcp, &dcp->swap_v14_7);
	mutex_unlock(&dcp->iomfb.lock);
	return ok;
}

static void v14_7_bl(struct apple_dcp *dcp, int nits, int dpms)
{
	unsigned long flags;

	spin_lock_irqsave(&dcp->backlight.lock, flags);
	if (nits >= 0)
		dcp_bl_request(&dcp->backlight.state, nits, false, false);
	if (dpms >= 0)
		dcp_bl_dpms(&dcp->backlight.state, dpms);
	spin_unlock_irqrestore(&dcp->backlight.lock, flags);
}

/*
 * The brightness policy through the driver's own path, from takeover to
 * soft DPMS and back: the requests it hands A407.
 */
static void v14_7_policy_test(struct kunit *test)
{
	struct iomfb_transaction atomic = { .presentation = true };
	struct iomfb_transaction brightness = {
		.presentation = true, .brightness_only = true,
	};
	struct dcp_swap_submit_req_v14_7 *frame, *blank, *expect;
	struct dcp_present_state_h17p *state;
	struct iomfb_scanout_v14_7 *scanout;
	struct apple_dcp *dcp;

	if (!IS_ENABLED(CONFIG_DRM_APPLE_DCP_T6030))
		kunit_skip(test, "needs DRM_APPLE_DCP_T6030");

	dcp = v14_7_dcp(test);
	state = &dcp->present_state_h17p;
	frame = kunit_kzalloc(test, SWAP_SIZE, GFP_KERNEL);
	blank = kunit_kzalloc(test, SWAP_SIZE, GFP_KERNEL);
	expect = kunit_kzalloc(test, SWAP_SIZE, GFP_KERNEL);
	scanout = kunit_kzalloc(test, sizeof(*scanout), GFP_KERNEL);
	KUNIT_ASSERT_TRUE(test, frame && blank && expect && scanout);
	iomfb_v14_7_encode_present(frame, &v14_7_frames[1]);
	iomfb_v14_7_encode_present(blank, NULL);

	/* Before the backlight is set up, presents carry no level. */
	KUNIT_ASSERT_TRUE(test, v14_7_prepare(dcp, &atomic, frame, 0x2a));
	KUNIT_EXPECT_EQ(test, v14_7_crc(&dcp->swap_v14_7), 0xf38285f2U);

	/* The first enable: 500 nits at most, 140 requested, no fades. */
	iomfb_configure_backlight_v14_7(dcp);
	flush_work(&dcp->bl_register_wq);
	KUNIT_ASSERT_TRUE(test, dcp_backlight_active(dcp));
	KUNIT_EXPECT_EQ(test, dcp->backlight.state.maximum, 500U);
	KUNIT_EXPECT_EQ(test, dcp->backlight.state.target, 140U);
	KUNIT_EXPECT_FALSE(test, dcp->backlight.state.fade);
	KUNIT_EXPECT_TRUE(test, dcp->backlight.state.dirty);
	iomfb_configure_backlight_v14_7(dcp);
	KUNIT_EXPECT_EQ(test, dcp->backlight.state.sequence, 0ULL);

	/* The first present carries the 140-nit request. */
	KUNIT_ASSERT_TRUE(test, v14_7_prepare(dcp, &atomic, frame, 7));
	KUNIT_EXPECT_EQ(test, v14_7_crc(&dcp->swap_v14_7), 0xd05cf8c9U);
	KUNIT_ASSERT_TRUE(test, atomic.backlight_reserved);
	KUNIT_EXPECT_EQ(test, atomic.backlight.nits, 140U);
	KUNIT_ASSERT_TRUE(test, dcp_backlight_complete(dcp, atomic.backlight.sequence, true));
	atomic.backlight_reserved = false;
	KUNIT_EXPECT_EQ(test, dcp->backlight.state.actual, 140U);

	/* An unchanged level is not sent again. */
	KUNIT_ASSERT_TRUE(test, v14_7_prepare(dcp, &atomic, frame, 0x2a));
	KUNIT_EXPECT_EQ(test, v14_7_crc(&dcp->swap_v14_7), 0xf38285f2U);
	KUNIT_EXPECT_FALSE(test, atomic.backlight_reserved);

	/* The completed frame is the snapshot brightness-only presents repeat. */
	scanout->request = *frame;
	dcp->iomfb_v14_7.scanout = scanout;

	/* A new level: a brightness-only re-present, completed by D589. */
	v14_7_bl(dcp, 300, -1);
	KUNIT_ASSERT_TRUE(test, v14_7_prepare(dcp, &brightness, &scanout->request, 0x1002));
	KUNIT_EXPECT_EQ(test, v14_7_crc(&dcp->swap_v14_7), 0xe7387727U);
	KUNIT_ASSERT_TRUE(test, dcp_present_begin_h17p(state, 0x1002));
	KUNIT_ASSERT_TRUE(test, dcp_present_submit_h17p(state, 0x1002, true));
	mutex_lock(&dcp->iomfb.lock);
	iomfb_swap_complete_v14_7(dcp, 0x1002);
	mutex_unlock(&dcp->iomfb.lock);
	KUNIT_EXPECT_TRUE(test, brightness.completed);
	KUNIT_EXPECT_FALSE(test, brightness.backlight_reserved);
	KUNIT_EXPECT_EQ(test, dcp->backlight.state.actual, 300U);
	KUNIT_EXPECT_EQ(test, dcp->iomfb_v14_7.brightness_presents, 1ULL);
	KUNIT_EXPECT_EQ(test, dcp->last_swap_id, 0x1002U);
	KUNIT_EXPECT_MEMEQ(test, &scanout->request, frame, SWAP_SIZE);

	/*
	 * Soft DPMS off: the blank at 0 nits.  Its D589 overtakes the A407
	 * reply here; the present completes only with the reply.
	 */
	v14_7_bl(dcp, -1, 0);
	brightness.completed = false;
	KUNIT_ASSERT_TRUE(test, v14_7_prepare(dcp, &brightness, &scanout->request, 0x1001));
	KUNIT_EXPECT_EQ(test, v14_7_crc(&dcp->swap_v14_7), 0xdb3c1a63U);
	KUNIT_EXPECT_TRUE(test, dcp->iomfb_v14_7.blank);
	KUNIT_ASSERT_TRUE(test, dcp_present_begin_h17p(state, 0x1001));
	mutex_lock(&dcp->iomfb.lock);
	iomfb_swap_complete_v14_7(dcp, 0x1001);
	mutex_unlock(&dcp->iomfb.lock);
	KUNIT_EXPECT_FALSE(test, brightness.completed);
	KUNIT_EXPECT_TRUE(test, dcp->iomfb_v14_7.early_complete);
	KUNIT_EXPECT_FALSE(test, READ_ONCE(dcp->crashed));
	KUNIT_ASSERT_TRUE(test, dcp_present_submit_h17p(state, 0x1001, true));
	mutex_lock(&dcp->iomfb.lock);
	iomfb_swap_accepted_v14_7(dcp, 0x1001);
	mutex_unlock(&dcp->iomfb.lock);
	KUNIT_EXPECT_TRUE(test, brightness.completed);
	KUNIT_EXPECT_FALSE(test, state->pending);
	KUNIT_EXPECT_EQ(test, dcp->backlight.state.actual, 0U);
	KUNIT_EXPECT_EQ(test, dcp->iomfb_v14_7.blank_presents, 1ULL);
	/* The blank is what the panel scans now; the saved level is kept. */
	KUNIT_EXPECT_MEMEQ(test, &scanout->request, blank, SWAP_SIZE);
	KUNIT_EXPECT_EQ(test, dcp->backlight.state.target, 300U);

	/* Enable: the next atomic present re-applies the saved level. */
	v14_7_bl(dcp, -1, 1);
	KUNIT_ASSERT_TRUE(test, v14_7_prepare(dcp, &atomic, frame, 7));
	*expect = *frame;
	expect->swap.swap_id = 7;
	KUNIT_ASSERT_EQ(test, iomfb_v14_7_encode_nits(expect, true, 300), 0);
	KUNIT_EXPECT_MEMEQ(test, &dcp->swap_v14_7, expect, SWAP_SIZE);
	KUNIT_ASSERT_TRUE(test, dcp_backlight_complete(dcp, atomic.backlight.sequence, true));

	/* A completion nobody waits for latches the failure. */
	mutex_lock(&dcp->iomfb.lock);
	iomfb_swap_complete_v14_7(dcp, 0x77);
	mutex_unlock(&dcp->iomfb.lock);
	KUNIT_EXPECT_TRUE(test, READ_ONCE(dcp->crashed));
}

/*
 * Which DCPs stamp their swaps: the main display of a board profile that
 * asks for it (T6031).  The J514S panel, the external role and a DCP with
 * no 14.7 board profile send zero timestamps, byte for byte as before.
 */
struct v14_7_ts_case {
	const char *name;
	bool profile;		/* a 14.7 board profile was admitted */
	bool swap_timestamps;	/* the profile asks for timestamps */
	bool main_display;
	bool stamped;
};

static const struct v14_7_ts_case v14_7_ts_cases[] = {
	{ "T6031 main display", true, true, true, true },
	{ "J514S main display", true, false, true, false },
	{ "T6031 profile, external role", true, true, false, false },
	{ "no 14.7 board profile", false, false, true, false },
};

static void v14_7_ts_case_desc(const struct v14_7_ts_case *c, char *desc)
{
	strscpy(desc, c->name, KUNIT_PARAM_DESC_SIZE);
}

KUNIT_ARRAY_PARAM(v14_7_ts_case, v14_7_ts_cases, v14_7_ts_case_desc);

static void v14_7_expect_timestamps(struct kunit *test,
				    const struct dcp_swap_submit_req_v14_7 *req,
				    bool stamped, const char *present)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(req->swap.timestamp); i++) {
		u64 want = stamped && (i == 0 || i == 1 || i == 6) ? 120 : 0;

		KUNIT_EXPECT_EQ_MSG(test, req->swap.timestamp[i], want,
				    "%s: slot %u", present, i);
	}
}

static void v14_7_swap_timestamps_test(struct kunit *test)
{
	const struct v14_7_ts_case *c = test->param_value;
	struct iomfb_transaction atomic = { .presentation = true };
	struct iomfb_transaction brightness = {
		.presentation = true, .brightness_only = true,
	};
	struct dcp_swap_submit_req_v14_7 *frame;
	struct iomfb_scanout_v14_7 *scanout;
	struct apple_dcp *dcp;

	if (!IS_ENABLED(CONFIG_DRM_APPLE_DCP_T6030))
		kunit_skip(test, "needs DRM_APPLE_DCP_T6030");

	dcp = v14_7_dcp(test);
	dcp->main_display = c->main_display;
	if (c->profile) {
		dcp->v14_7 = kunit_kzalloc(test, sizeof(*dcp->v14_7), GFP_KERNEL);
		KUNIT_ASSERT_NOT_NULL(test, dcp->v14_7);
		dcp->v14_7->swap_timestamps = c->swap_timestamps;
	}
	frame = kunit_kzalloc(test, SWAP_SIZE, GFP_KERNEL);
	scanout = kunit_kzalloc(test, sizeof(*scanout), GFP_KERNEL);
	KUNIT_ASSERT_TRUE(test, frame && scanout);
	iomfb_v14_7_encode_present(frame, &v14_7_frames[1]);

	/* An atomic present before the backlight is set up. */
	KUNIT_ASSERT_TRUE(test, v14_7_prepare(dcp, &atomic, frame, 0x2a));
	v14_7_expect_timestamps(test, &dcp->swap_v14_7, c->stamped, "atomic");
	if (!c->stamped)
		KUNIT_EXPECT_EQ(test, v14_7_crc(&dcp->swap_v14_7), 0xf38285f2U);

	/* The first present with a level. */
	iomfb_configure_backlight_v14_7(dcp);
	flush_work(&dcp->bl_register_wq);
	KUNIT_ASSERT_TRUE(test, dcp_backlight_active(dcp));
	KUNIT_ASSERT_TRUE(test, v14_7_prepare(dcp, &atomic, frame, 7));
	v14_7_expect_timestamps(test, &dcp->swap_v14_7, c->stamped, "level");
	if (!c->stamped)
		KUNIT_EXPECT_EQ(test, v14_7_crc(&dcp->swap_v14_7), 0xd05cf8c9U);
	KUNIT_ASSERT_TRUE(test, atomic.backlight_reserved);
	KUNIT_ASSERT_TRUE(test, dcp_backlight_complete(dcp, atomic.backlight.sequence, true));

	/* The soft-DPMS blank, re-encoded from the completed frame. */
	scanout->request = *frame;
	dcp->iomfb_v14_7.scanout = scanout;
	v14_7_bl(dcp, -1, 0);
	KUNIT_ASSERT_TRUE(test, v14_7_prepare(dcp, &brightness, &scanout->request, 0x1001));
	KUNIT_EXPECT_TRUE(test, dcp->iomfb_v14_7.blank);
	v14_7_expect_timestamps(test, &dcp->swap_v14_7, c->stamped, "DPMS blank");
	if (!c->stamped)
		KUNIT_EXPECT_EQ(test, v14_7_crc(&dcp->swap_v14_7), 0xdb3c1a63U);
}

static struct kunit_case v14_7_present_cases[] = {
	KUNIT_CASE_PARAM(v14_7_present_encoding_test, v14_7_case_gen_params),
	KUNIT_CASE(v14_7_layout_test),
	KUNIT_CASE(v14_7_nits_test),
	KUNIT_CASE(v14_7_frame_check_test),
	KUNIT_CASE(v14_7_single_surface_test),
	KUNIT_CASE(v14_7_policy_test),
	KUNIT_CASE_PARAM(v14_7_swap_timestamps_test, v14_7_ts_case_gen_params),
	{}
};

static struct kunit_suite v14_7_present_suite = {
	.name = "apple-dcp-v14_7-present",
	.test_cases = v14_7_present_cases,
};

kunit_test_suite(v14_7_present_suite);

MODULE_LICENSE("Dual MIT/GPL");
