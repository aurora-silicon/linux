// SPDX-License-Identifier: GPL-2.0-only OR MIT

/*
 * The built-in panel's base EDID, read back with the DRM EDID helpers: the
 * make and product come from the panel's compatible, and the connector keeps
 * the panel's size in millimetres after the EDID update.
 */

#include <kunit/test.h>
#include <linux/errno.h>
#include <linux/string.h>

#include <drm/drm_connector.h>
#include <drm/drm_drv.h>
#include <drm/drm_edid.h>
#include <drm/drm_kunit_helpers.h>
#include <drm/drm_property.h>

#include "dcp-panel-edid.h"

#define J516_W_MM 346
#define J516_H_MM 223

static void panel_id_j516(struct kunit *test, struct dcp_panel_id *id)
{
	KUNIT_ASSERT_EQ(test, dcp_panel_id_from_compatible(id, "apple,panel-j516"), 0);
}

static void panel_id_from_compatible(struct kunit *test)
{
	struct dcp_panel_id id;

	panel_id_j516(test, &id);
	KUNIT_EXPECT_STREQ(test, id.vendor, "APP");
	KUNIT_EXPECT_STREQ(test, id.name, "J516");
}

static void panel_id_needs_a_model_and_a_known_vendor(struct kunit *test)
{
	struct dcp_panel_id id;

	KUNIT_EXPECT_EQ(test, dcp_panel_id_from_compatible(&id, "apple,panel"), -EINVAL);
	KUNIT_EXPECT_EQ(test, dcp_panel_id_from_compatible(&id, "apple,panel-"), -EINVAL);
	KUNIT_EXPECT_EQ(test, dcp_panel_id_from_compatible(&id, "panel-j516"), -EINVAL);
	KUNIT_EXPECT_EQ(test, dcp_panel_id_from_compatible(&id, "acme,panel-j516"), -ENOENT);
	KUNIT_EXPECT_EQ(test, dcp_panel_id_from_compatible(&id, "apple,panel-j516 x"), -EINVAL);
	KUNIT_EXPECT_EQ(test, dcp_panel_id_from_compatible(&id,
							   "apple,panel-j5160000000000"),
			-EINVAL);
}

static const struct drm_edid *panel_edid_j516(struct kunit *test, struct edid *block)
{
	struct dcp_panel_id id;
	const struct drm_edid *edid;

	panel_id_j516(test, &id);
	KUNIT_ASSERT_EQ(test, dcp_panel_edid_block(&id, J516_W_MM, J516_H_MM, block), 0);
	edid = drm_edid_alloc(block, sizeof(*block));
	KUNIT_ASSERT_NOT_NULL(test, edid);
	return edid;
}

static void panel_edid_parses_with_make_product_and_size(struct kunit *test)
{
	struct drm_edid_product_id product;
	struct edid block;
	const struct drm_edid *edid = panel_edid_j516(test, &block);
	const struct edid *raw;
	char vendor[4], name[14];

	KUNIT_EXPECT_TRUE(test, drm_edid_valid(edid));
	drm_edid_get_product_id(edid, &product);
	drm_edid_decode_mfg_id(be16_to_cpu(product.manufacturer_name), vendor);
	KUNIT_EXPECT_STREQ(test, vendor, "APP");
	KUNIT_EXPECT_EQ(test, le16_to_cpu(product.product_code), 0);
	KUNIT_EXPECT_EQ(test, le32_to_cpu(product.serial_number), 0);

	raw = drm_edid_raw(edid);
	KUNIT_ASSERT_NOT_NULL(test, raw);
	drm_edid_get_monitor_name(raw, name, sizeof(name));
	KUNIT_EXPECT_STREQ(test, name, "J516");
	KUNIT_EXPECT_EQ(test, raw->width_cm, 35);
	KUNIT_EXPECT_EQ(test, raw->height_cm, 22);
	KUNIT_EXPECT_EQ(test, raw->extensions, 0);
	drm_edid_free(edid);
}

static void panel_edid_has_no_serial_and_no_timings(struct kunit *test)
{
	struct edid block;
	const struct drm_edid *edid = panel_edid_j516(test, &block);
	unsigned int i, names = 0;

	for (i = 0; i < ARRAY_SIZE(block.detailed_timings); i++) {
		const struct detailed_timing *d = &block.detailed_timings[i];

		KUNIT_EXPECT_EQ(test, le16_to_cpu(d->pixel_clock), 0);
		KUNIT_EXPECT_NE(test, d->data.other_data.type, EDID_DETAIL_MONITOR_SERIAL);
		KUNIT_EXPECT_NE(test, d->data.other_data.type, EDID_DETAIL_MONITOR_RANGE);
		names += d->data.other_data.type == EDID_DETAIL_MONITOR_NAME;
	}
	KUNIT_EXPECT_EQ(test, names, 1);
	for (i = 0; i < sizeof(block.established_timings); i++)
		KUNIT_EXPECT_EQ(test, ((u8 *)&block.established_timings)[i], 0);
	drm_edid_free(edid);
}

static void panel_edid_refuses_a_size_it_cannot_hold(struct kunit *test)
{
	struct dcp_panel_id id;
	struct edid block;

	panel_id_j516(test, &id);
	KUNIT_EXPECT_EQ(test, dcp_panel_edid_block(&id, 0, J516_H_MM, &block), -EINVAL);
	KUNIT_EXPECT_EQ(test, dcp_panel_edid_block(&id, J516_W_MM, 2560, &block), -EINVAL);
	id.vendor[0] = '\0';
	KUNIT_EXPECT_EQ(test, dcp_panel_edid_block(&id, J516_W_MM, J516_H_MM, &block),
			-EINVAL);
}

struct panel_edid_test {
	struct drm_device drm;
	struct drm_connector connector;
};

static const struct drm_connector_funcs panel_edid_connector_funcs = {};

static void panel_edid_update_keeps_the_size_in_mm(struct kunit *test)
{
	struct panel_edid_test *t;
	struct drm_connector *connector;
	struct dcp_panel_id id;
	struct device *dev;
	int ret;

	dev = drm_kunit_helper_alloc_device(test);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, dev);
	t = drm_kunit_helper_alloc_drm_device(test, dev, struct panel_edid_test, drm,
					      DRIVER_MODESET);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, t);
	connector = &t->connector;
	ret = drmm_connector_init(&t->drm, connector, &panel_edid_connector_funcs,
				  DRM_MODE_CONNECTOR_eDP, NULL);
	KUNIT_ASSERT_EQ(test, ret, 0);

	panel_id_j516(test, &id);
	mutex_lock(&t->drm.mode_config.mutex);
	ret = dcp_panel_edid_update(connector, &id, J516_W_MM, J516_H_MM);
	mutex_unlock(&t->drm.mode_config.mutex);
	KUNIT_ASSERT_EQ(test, ret, 0);

	/* The EDID update sets 350x220 from the block; the panel's own size wins. */
	KUNIT_EXPECT_EQ(test, connector->display_info.width_mm, J516_W_MM);
	KUNIT_EXPECT_EQ(test, connector->display_info.height_mm, J516_H_MM);
	KUNIT_ASSERT_NOT_NULL(test, connector->edid_blob_ptr);
	KUNIT_EXPECT_EQ(test, connector->edid_blob_ptr->length, EDID_LENGTH);
}

static struct kunit_case panel_edid_cases[] = {
	KUNIT_CASE(panel_id_from_compatible),
	KUNIT_CASE(panel_id_needs_a_model_and_a_known_vendor),
	KUNIT_CASE(panel_edid_parses_with_make_product_and_size),
	KUNIT_CASE(panel_edid_has_no_serial_and_no_timings),
	KUNIT_CASE(panel_edid_refuses_a_size_it_cannot_hold),
	KUNIT_CASE(panel_edid_update_keeps_the_size_in_mm),
	{}
};

static struct kunit_suite panel_edid_suite = {
	.name = "apple-dcp-panel-edid",
	.test_cases = panel_edid_cases,
};
kunit_test_suite(panel_edid_suite);
