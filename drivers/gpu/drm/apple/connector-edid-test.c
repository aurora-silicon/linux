// SPDX-License-Identifier: GPL-2.0-only OR MIT

#include <kunit/test.h>

#include "connector.h"

struct apple_edid_fixture {
	struct apple_connector connector;
	struct platform_device first;
	struct platform_device second;
};

static int apple_edid_test_init(struct kunit *test)
{
	struct apple_edid_fixture *fixture;

	fixture = kunit_kzalloc(test, sizeof(*fixture), GFP_KERNEL);
	if (!fixture)
		return -ENOMEM;
	apple_connector_edid_init(&fixture->connector);
	apple_connector_set_pipeline(&fixture->connector, &fixture->first);
	test->priv = fixture;
	return 0;
}

static void apple_edid_test_exit(struct kunit *test)
{
	struct apple_edid_fixture *fixture = test->priv;

	apple_connector_invalidate_edid(&fixture->connector);
	mutex_destroy(&fixture->connector.edid_lock);
}

static const struct drm_edid *apple_edid_test_alloc(u8 marker)
{
	u8 raw[EDID_LENGTH] = { 0 };

	raw[0] = marker;
	return drm_edid_alloc(raw, sizeof(raw));
}

static void apple_edid_install_and_duplicate(struct kunit *test)
{
	struct apple_edid_fixture *fixture = test->priv;
	struct apple_connector *connector = &fixture->connector;
	const struct drm_edid *fetched, *copy;
	u64 generation;

	KUNIT_ASSERT_TRUE(test, apple_connector_edid_begin(connector,
							   &fixture->first, &generation));
	fetched = apple_edid_test_alloc(1);
	KUNIT_ASSERT_NOT_NULL(test, fetched);
	KUNIT_EXPECT_TRUE(test, apple_connector_edid_install(connector,
							     &fixture->first, generation,
							     fetched));
	KUNIT_EXPECT_FALSE(test, apple_connector_edid_begin(connector,
							    &fixture->first, &generation));
	copy = apple_connector_edid_dup(connector, &fixture->first);
	KUNIT_ASSERT_NOT_NULL(test, copy);
	KUNIT_EXPECT_PTR_NE(test, copy, fetched);
	apple_connector_invalidate_edid(connector);
	KUNIT_EXPECT_PTR_EQ(test, connector->drm_edid, NULL);
	KUNIT_EXPECT_EQ(test, ((const u8 *)drm_edid_raw(copy))[0], 1);
	drm_edid_free(copy);
}

static void apple_edid_clear_rejects_inflight(struct kunit *test)
{
	struct apple_edid_fixture *fixture = test->priv;
	struct apple_connector *connector = &fixture->connector;
	const struct drm_edid *fetched;
	u64 generation, replacement;

	KUNIT_ASSERT_TRUE(test, apple_connector_edid_begin(connector,
							   &fixture->first, &generation));
	/* A sink change invalidates a fetch even when the cache is still empty. */
	apple_connector_invalidate_edid(connector);
	fetched = apple_edid_test_alloc(2);
	KUNIT_ASSERT_NOT_NULL(test, fetched);
	KUNIT_EXPECT_FALSE(test, apple_connector_edid_install(connector,
							      &fixture->first, generation,
							      fetched));
	KUNIT_EXPECT_PTR_EQ(test, connector->drm_edid, NULL);
	KUNIT_ASSERT_TRUE(test, apple_connector_edid_begin(connector,
							   &fixture->first, &replacement));
	KUNIT_EXPECT_NE(test, replacement, generation);
}

static void apple_edid_route_change_rejects_inflight(struct kunit *test)
{
	struct apple_edid_fixture *fixture = test->priv;
	struct apple_connector *connector = &fixture->connector;
	const struct drm_edid *fetched;
	u64 generation;

	KUNIT_ASSERT_TRUE(test, apple_connector_edid_begin(connector,
							   &fixture->first, &generation));
	apple_connector_set_pipeline(connector, &fixture->second);
	fetched = apple_edid_test_alloc(3);
	KUNIT_ASSERT_NOT_NULL(test, fetched);
	KUNIT_EXPECT_FALSE(test, apple_connector_edid_install(connector,
							      &fixture->first, generation,
							      fetched));
	KUNIT_EXPECT_PTR_EQ(test, apple_connector_edid_dup(connector,
							   &fixture->first), NULL);
	apple_connector_set_pipeline(connector, NULL);
	KUNIT_EXPECT_FALSE(test, apple_connector_edid_begin(connector, NULL, &generation));
	KUNIT_EXPECT_FALSE(test, apple_connector_edid_begin(connector,
							    &fixture->second, &generation));
	KUNIT_EXPECT_PTR_EQ(test, apple_connector_edid_dup(connector, NULL), NULL);
}

static void apple_edid_first_completion_wins(struct kunit *test)
{
	struct apple_edid_fixture *fixture = test->priv;
	struct apple_connector *connector = &fixture->connector;
	const struct drm_edid *first, *second, *copy;
	u64 generation, concurrent;

	KUNIT_ASSERT_TRUE(test, apple_connector_edid_begin(connector,
							   &fixture->first, &generation));
	KUNIT_ASSERT_TRUE(test, apple_connector_edid_begin(connector,
							   &fixture->first, &concurrent));
	first = apple_edid_test_alloc(4);
	KUNIT_ASSERT_NOT_NULL(test, first);
	KUNIT_ASSERT_TRUE(test, apple_connector_edid_install(connector,
							     &fixture->first, generation, first));
	second = apple_edid_test_alloc(5);
	KUNIT_ASSERT_NOT_NULL(test, second);
	KUNIT_EXPECT_FALSE(test, apple_connector_edid_install(connector,
							      &fixture->first, concurrent, second));
	copy = apple_connector_edid_dup(connector, &fixture->first);
	KUNIT_ASSERT_NOT_NULL(test, copy);
	KUNIT_EXPECT_EQ(test, ((const u8 *)drm_edid_raw(copy))[0], 4);
	drm_edid_free(copy);
	apple_connector_set_pipeline(connector, &fixture->second);
	KUNIT_EXPECT_PTR_EQ(test, connector->drm_edid, NULL);
}

static void apple_edid_disconnected_admission(struct kunit *test)
{
	struct apple_edid_fixture *fixture = test->priv;
	struct apple_connector *connector = &fixture->connector;
	const struct drm_edid *fetched;
	u64 generation;

	KUNIT_ASSERT_TRUE(test, apple_connector_edid_begin(connector,
							   &fixture->first, &generation));
	/* DRM can defer its connection flag while a modeset is running. */
	connector->connected = true;
	apple_connector_edid_set_live(connector, false);
	KUNIT_EXPECT_TRUE(test, connector->connected);
	fetched = apple_edid_test_alloc(6);
	KUNIT_ASSERT_NOT_NULL(test, fetched);
	KUNIT_EXPECT_FALSE(test, apple_connector_edid_install(connector,
							      &fixture->first, generation,
							      fetched));
	KUNIT_EXPECT_FALSE(test, apple_connector_edid_begin(connector,
							    &fixture->first, &generation));
	KUNIT_EXPECT_PTR_EQ(test, apple_connector_edid_dup(connector, &fixture->first), NULL);
	apple_connector_invalidate_edid(connector);
	KUNIT_EXPECT_FALSE(test, apple_connector_edid_begin(connector,
							    &fixture->first, &generation));
	apple_connector_edid_set_live(connector, true);
	KUNIT_EXPECT_TRUE(test, apple_connector_edid_begin(connector,
							   &fixture->first, &generation));
}

static struct kunit_case apple_edid_test_cases[] = {
	KUNIT_CASE(apple_edid_install_and_duplicate),
	KUNIT_CASE(apple_edid_clear_rejects_inflight),
	KUNIT_CASE(apple_edid_route_change_rejects_inflight),
	KUNIT_CASE(apple_edid_first_completion_wins),
	KUNIT_CASE(apple_edid_disconnected_admission),
	{}
};

static struct kunit_suite apple_edid_test_suite = {
	.name = "apple-connector-edid",
	.init = apple_edid_test_init,
	.exit = apple_edid_test_exit,
	.test_cases = apple_edid_test_cases,
};

kunit_test_suite(apple_edid_test_suite);

MODULE_LICENSE("Dual MIT/GPL");
