// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* Copyright 2026 Ryan Murray */

#include <kunit/platform_device.h>
#include <kunit/test.h>
#include <linux/module.h>
#include <linux/platform_device.h>

#include <drm/drm_edid.h>

#include "afk.h"
#include "connector.h"
#include "dcp-internal.h"
#include "parser.h"

/*
 * An external H17P DCP driving the QG270 S3 at 1920x1080 and 180 Hz over a
 * direct two-lane HBR3 link: the facts its first probe needs arrive one by
 * one, in any order.
 */
struct ready_test {
	struct apple_dcp *dcp;
	struct apple_connector *con;
	struct apple_dcp_afkep dptx_ep;
	struct apple_epic_service dptx;
	struct apple_epic_service av;
	struct dcp_display_mode modes[2];
};

static const struct drm_display_mode ready_mode_180 = {
	.clock = 414090,
	.hdisplay = 1920, .hsync_start = 1968, .hsync_end = 2000, .htotal = 2080,
	.vdisplay = 1080, .vsync_start = 1083, .vsync_end = 1088, .vtotal = 1106,
	.flags = DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_NVSYNC,
};

static const struct drm_display_mode ready_mode_60 = {
	.clock = 148500,
	.hdisplay = 1920, .hsync_start = 2008, .hsync_end = 2052, .htotal = 2200,
	.vdisplay = 1080, .vsync_start = 1084, .vsync_end = 1089, .vtotal = 1125,
	.flags = DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC,
};

static void ready_checksum(u8 *block)
{
	u8 sum = 0;
	int i;

	for (i = 0; i < 127; i++)
		sum += block[i];
	block[127] = -sum;
}

/* A 256-byte EDID that admits ready_mode_180. */
static const struct drm_edid *ready_edid(struct kunit *test)
{
	static const u8 header[] = { 0, 255, 255, 255, 255, 255, 255, 0 };
	static const u8 dtd[] = {
		0xc1, 0xa1, 0x80, 0xa0, 0x70, 0x38, 0x1a, 0x40, 0x30,
		0x20, 0x35, 0x00, 0, 0, 0, 0, 0, 0x1a,
	};
	static const u8 name[] = {
		0, 0, 0, 0xfc, 0, 'Q', 'G', '2', '7', '0', ' ', 'S', '3', '\n',
		' ', ' ', ' ', ' ',
	};
	static const u8 hf_vsdb[] = { 0x67, 0xd8, 0x5d, 0xc4, 1, 0x54, 0x80, 0 };
	const struct drm_edid *edid;
	u8 *p;

	p = kunit_kzalloc(test, 256, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, p);
	memcpy(p, header, sizeof(header));
	p[8] = 0x04;
	p[9] = 0x72;
	p[10] = 0x4f;
	p[11] = 0x0b;
	p[18] = 1;
	p[19] = 4;
	p[20] = 0x80;
	memcpy(p + 54, dtd, sizeof(dtd));
	memcpy(p + 72, name, sizeof(name));
	p[126] = 1;
	ready_checksum(p);
	p[128] = 2;
	p[129] = 3;
	p[130] = 4 + sizeof(hf_vsdb);
	memcpy(p + 132, hf_vsdb, sizeof(hf_vsdb));
	ready_checksum(p + 128);

	edid = drm_edid_alloc(p, 256);
	KUNIT_ASSERT_NOT_NULL(test, edid);
	return edid;
}

static void ready_free_edid(void *data)
{
	drm_edid_free(data);
}

static struct ready_test *ready_init(struct kunit *test)
{
	struct platform_device *pdev;
	struct ready_test *t;
	struct apple_dcp *dcp;

	t = kunit_kzalloc(test, sizeof(*t), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, t);
	dcp = kunit_kzalloc(test, sizeof(*dcp), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, dcp);
	t->con = kunit_kzalloc(test, sizeof(*t->con), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, t->con);
	pdev = kunit_platform_device_alloc(test, "dcp-external-ready", PLATFORM_DEVID_AUTO);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, pdev);
	KUNIT_ASSERT_EQ(test, kunit_platform_device_add(test, pdev), 0);

	t->dcp = dcp;
	dcp->dev = &pdev->dev;
	dcp->hw.iomfb_method_profile = DCP_IOMFB_METHODS_H17P_EXTERNAL;
	mutex_init(&dcp->hpd_mutex);
	mutex_init(&dcp->dcpavserv.lock);
	spin_lock_init(&dcp->metadata.lock);
	dcp->connector = t->con;
	t->con->dcp = pdev;
	t->con->connected = true;

	/* A direct route: the PHY and route are only compared, never used. */
	dcp->phy = kunit_kzalloc(test, 1, GFP_KERNEL);
	dcp->active_typec_route = kunit_kzalloc(test, sizeof(*dcp->active_typec_route),
						GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, dcp->phy);
	KUNIT_ASSERT_NOT_NULL(test, dcp->active_typec_route);
	t->dptx_ep.dcp = dcp;
	t->dptx_ep.endpoint = DPTX_ENDPOINT_V2;
	t->dptx_ep.wire_v2 = true;
	t->dptx.ep = &t->dptx_ep;
	t->dptx.cookie = &dcp->dptxport[0];
	t->dptx.enabled = true;
	dcp->dptxport[0].service = &t->dptx;
	dcp->dptxport[0].enabled = true;
	dcp->dptxport[0].connected = true;
	dcp->dptxport[0].atcphy = dcp->phy;

	t->modes[0].mode = ready_mode_60;
	t->modes[1].mode = ready_mode_180;
	t->modes[1].automatic_rgb8_quirk = true;
	t->modes[1].sdr_rgb = (struct dcp_color_mode) {
		.depth = 8,
		.format = DCP_COLOR_FORMAT_RGB,
		.range = DCP_COLOR_YCBCR_RANGE_FULL,
		.eotf = DCP_EOTF_SDR_GAMMA,
		.colorimetry = DCP_COLORIMETRY_SRGB,
	};

	/* A new cable. */
	dcp->typec_cable_connected = true;
	dcp->typec_generation = 1;
	dcp_firmware_metadata_connection(dcp, dcp->typec_generation, true);

	return t;
}

/* What dcpep_process_chunks() publishes for a complete TimingElements. */
static void ready_catalogue(struct ready_test *t, unsigned int nr_modes)
{
	struct apple_dcp *dcp = t->dcp;
	struct dcp_metadata_ticket ticket;
	unsigned long flags;
	unsigned int i;

	spin_lock_irqsave(&dcp->metadata.lock, flags);
	ticket = dcp_metadata_begin(&dcp->metadata.epoch);
	ticket.associated = dcp->metadata.epoch.generation == dcp->typec_generation &&
			    dcp->typec_cable_connected;
	dcp->rate_proof.catalog_sequence++;
	for (i = 0; i < nr_modes; i++) {
		if (!t->modes[i].automatic_rgb8_quirk)
			continue;
		t->modes[i].unsafe_rgb8_ticket = ticket;
		t->modes[i].unsafe_rgb8_service_epoch = dcp->dcpavserv.epoch;
		t->modes[i].automatic_service_pending = !dcp->dcpavserv.enabled &&
							!dcp->dcpavserv.service;
		t->modes[i].automatic_connection_sequence = dcp->rate_proof.connection_sequence;
		t->modes[i].automatic_catalog_sequence = dcp->rate_proof.catalog_sequence;
	}
	dcp->modes = t->modes;
	dcp->nr_modes = nr_modes;
	dcp->metadata.catalog_ticket = ticket;
	dcp->metadata.catalog_ticket.associated &= nr_modes > 0;
	dcp->metadata.catalog_connection_sequence = dcp->rate_proof.connection_sequence;
	spin_unlock_irqrestore(&dcp->metadata.lock, flags);
}

/* The AV service the EDID is read from comes up (dcpavserv_init()). */
static void ready_service(struct ready_test *t)
{
	struct apple_dcp *dcp = t->dcp;

	guard(mutex)(&dcp->dcpavserv.lock);
	dcp->dcpavserv.service = &t->av;
	dcp->dcpavserv.enabled = true;
	dcp->dcpavserv.epoch++;
}

/* The firmware announces the display (dcpep_cb_hotplug()). */
static void ready_announce(struct ready_test *t)
{
	dcp_firmware_metadata_hotplug(t->dcp, true);
}

/* The DPTX endpoint reports the link rate and lane count it configured. */
static void ready_link(struct ready_test *t, u32 rate, u32 lanes)
{
	struct dcp_180hz_token token;

	token = dcp_180hz_config_begin(&t->dptx, false);
	dcp_180hz_config_complete(&t->dptx, false, token, rate, true);
	token = dcp_180hz_config_begin(&t->dptx, true);
	dcp_180hz_config_complete(&t->dptx, true, token, lanes, true);
}

static bool ready(struct ready_test *t)
{
	guard(mutex)(&t->dcp->hpd_mutex);
	return dcp_external_modes_ready(t->dcp);
}

/* What dcp_get_modes() decides for the 180 Hz mode once it has read the EDID. */
static bool ready_probe_lists_180(struct kunit *test, struct ready_test *t)
{
	struct apple_dcp *dcp = t->dcp;
	const struct drm_edid *edid = ready_edid(test);
	unsigned long flags;
	bool listed;

	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, ready_free_edid, (void *)edid), 0);
	guard(mutex)(&dcp->hpd_mutex);
	guard(mutex)(&dcp->dcpavserv.lock);
	t->con->drm_edid = edid;
	t->con->drm_edid_generation = dcp->typec_generation;
	t->con->drm_edid_service_epoch = dcp->dcpavserv.epoch;
	spin_lock_irqsave(&dcp->metadata.lock, flags);
	listed = dcp_180hz_mode_current(dcp, &t->modes[1]);
	spin_unlock_irqrestore(&dcp->metadata.lock, flags);
	t->con->drm_edid = NULL;
	return listed;
}

static void ready_complete_test(struct kunit *test)
{
	struct ready_test *t = ready_init(test);

	ready_catalogue(t, 2);
	ready_service(t);
	ready_announce(t);
	ready_link(t, 0x1e, 2);

	KUNIT_EXPECT_TRUE(test, ready(t));
	KUNIT_EXPECT_TRUE(test, ready_probe_lists_180(test, t));
}

enum ready_fact { READY_CATALOGUE, READY_SERVICE, READY_ANNOUNCE, READY_LINK, READY_FACTS };

static void ready_fact(struct ready_test *t, enum ready_fact fact)
{
	switch (fact) {
	case READY_CATALOGUE:
		ready_catalogue(t, 2);
		break;
	case READY_SERVICE:
		ready_service(t);
		break;
	case READY_ANNOUNCE:
		ready_announce(t);
		break;
	default:
		ready_link(t, 0x1e, 2);
		break;
	}
}

/*
 * Back from system sleep on the same cable: suspend advanced the generation
 * without a new connection, so the firmware has to announce the display
 * again.  (A new cable counts as announced from dcp_dptx_connect_oob().)
 * Each fact can come last; until it has, a probe would miss the 180 Hz mode.
 */
static void ready_any_order_test(struct kunit *test)
{
	enum ready_fact last, fact;

	for (last = 0; last < READY_FACTS; last++) {
		struct ready_test *t = ready_init(test);

		t->dcp->typec_generation++;
		dcp_firmware_metadata_connection(t->dcp, t->dcp->typec_generation, false);

		for (fact = 0; fact < READY_FACTS; fact++)
			if (fact != last)
				ready_fact(t, fact);
		KUNIT_EXPECT_FALSE_MSG(test, ready(t), "fact %d missing", last);
		KUNIT_EXPECT_FALSE_MSG(test, ready_probe_lists_180(test, t),
				       "fact %d missing", last);

		ready_fact(t, last);
		KUNIT_EXPECT_TRUE_MSG(test, ready(t), "fact %d last", last);
		KUNIT_EXPECT_TRUE_MSG(test, ready_probe_lists_180(test, t),
				      "fact %d last", last);
	}
}

/*
 * A replug keeps the previous connection's catalogue until TimingElements
 * replaces it: that catalogue and a new EDID service must not count.
 */
static void ready_retained_catalogue_test(struct kunit *test)
{
	struct ready_test *t = ready_init(test);
	struct apple_dcp *dcp = t->dcp;

	ready_catalogue(t, 2);
	ready_service(t);
	ready_announce(t);
	ready_link(t, 0x1e, 2);
	KUNIT_ASSERT_TRUE(test, ready(t));

	/* Unplug (dcp_external_disconnect()), then a new cable. */
	dcp->typec_cable_connected = false;
	dcp->typec_generation++;
	dcp_firmware_metadata_connection(dcp, dcp->typec_generation, false);
	dcp->dcpavserv.enabled = false;
	dcp->dcpavserv.service = NULL;
	dcp->dcpavserv.epoch++;
	dcp->typec_cable_connected = true;
	dcp->typec_generation++;
	dcp_firmware_metadata_connection(dcp, dcp->typec_generation, true);

	ready_service(t);
	ready_announce(t);
	ready_link(t, 0x1e, 2);
	KUNIT_EXPECT_EQ(test, dcp->nr_modes, 2);
	KUNIT_EXPECT_FALSE(test, ready(t));

	ready_catalogue(t, 2);
	KUNIT_EXPECT_TRUE(test, ready(t));
}

/*
 * System sleep advances the generation of a cable that stays attached.
 * Facts from before it, and a callback delivered after it, prove nothing.
 */
static void ready_generation_advance_test(struct kunit *test)
{
	struct ready_test *t = ready_init(test);
	struct apple_dcp *dcp = t->dcp;

	ready_catalogue(t, 2);
	ready_service(t);
	ready_announce(t);
	ready_link(t, 0x1e, 2);
	KUNIT_ASSERT_TRUE(test, ready(t));

	dcp->typec_generation++;
	dcp_firmware_metadata_connection(dcp, dcp->typec_generation, false);
	KUNIT_EXPECT_FALSE(test, ready(t));

	/* A late announcement of the old link, with the new generation set. */
	ready_announce(t);
	KUNIT_EXPECT_FALSE(test, ready(t));
	KUNIT_EXPECT_FALSE(test, ready_probe_lists_180(test, t));

	ready_catalogue(t, 2);
	ready_link(t, 0x1e, 2);
	KUNIT_EXPECT_TRUE(test, ready(t));
}

/* The display behind a dock that stays attached drops its link. */
static void ready_downstream_loss_test(struct kunit *test)
{
	struct ready_test *t = ready_init(test);

	ready_catalogue(t, 2);
	ready_service(t);
	ready_announce(t);
	ready_link(t, 0x1e, 2);
	KUNIT_ASSERT_TRUE(test, ready(t));

	dcp_firmware_metadata_hotplug(t->dcp, false);
	KUNIT_EXPECT_FALSE(test, ready(t));
}

/* Without an exception mode, the catalogue and the EDID service suffice. */
static void ready_ordinary_catalogue_test(struct kunit *test)
{
	struct ready_test *t = ready_init(test);

	ready_catalogue(t, 1);
	KUNIT_EXPECT_FALSE(test, ready(t));
	ready_service(t);
	KUNIT_EXPECT_TRUE(test, ready(t));
}

/* A link configured at another rate never proves the 180 Hz mode. */
static void ready_other_link_test(struct kunit *test)
{
	struct ready_test *t = ready_init(test);

	ready_catalogue(t, 2);
	ready_service(t);
	ready_announce(t);
	ready_link(t, 0x14, 2);
	KUNIT_EXPECT_FALSE(test, ready(t));
	KUNIT_EXPECT_FALSE(test, ready_probe_lists_180(test, t));
}

static struct kunit_case ready_cases[] = {
	KUNIT_CASE(ready_complete_test),
	KUNIT_CASE(ready_any_order_test),
	KUNIT_CASE(ready_retained_catalogue_test),
	KUNIT_CASE(ready_generation_advance_test),
	KUNIT_CASE(ready_downstream_loss_test),
	KUNIT_CASE(ready_ordinary_catalogue_test),
	KUNIT_CASE(ready_other_link_test),
	{}
};

static struct kunit_suite ready_suite = {
	.name = "apple-dcp-external-ready",
	.test_cases = ready_cases,
};

kunit_test_suite(ready_suite);
