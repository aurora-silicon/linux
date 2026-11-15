// SPDX-License-Identifier: GPL-2.0-only
/*
 * KUnit tests for the Type-C mux lookup: a connector passes over a mux inside
 * a node that is not available, and still waits for one that is. The
 * connectors and muxes come from typec-mux-test.dtso.
 */
#include <kunit/device.h>
#include <kunit/of.h>
#include <kunit/test.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/usb/typec_mux.h>

/* The node at @path, put when the test ends, before the overlay goes. */
static struct device_node *mux_test_node(struct kunit *test, const char *path)
{
	struct device_node *np = of_find_node_by_path(path);

	KUNIT_ASSERT_NOT_NULL(test, np);
	of_node_put_kunit(test, np);
	return np;
}

static int mux_test_sets;

static int mux_test_set(struct typec_mux_dev *mux_dev, struct typec_mux_state *state)
{
	mux_test_sets++;
	return 0;
}

/*
 * Registers a mux on the route at @route, looks up the muxes of the
 * connector at @conn, and checks that exactly one of them is set.
 */
static void mux_test_lookup_one(struct kunit *test, const char *conn, const char *route)
{
	struct typec_mux_desc desc = { .set = mux_test_set, .name = "typec-mux-test" };
	struct typec_mux_state state = {};
	struct typec_mux_dev *mux_dev;
	struct typec_mux *mux;
	struct device *dev;

	dev = kunit_device_register(test, "typec-mux-test");
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, dev);
	KUNIT_ASSERT_NOT_NULL(test, dev->driver);
	desc.fwnode = of_fwnode_handle(mux_test_node(test, route));
	mux_dev = typec_mux_register(dev, &desc);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, mux_dev);

	mux = fwnode_typec_mux_get(of_fwnode_handle(mux_test_node(test, conn)));
	KUNIT_EXPECT_NOT_ERR_OR_NULL(test, mux);
	if (!IS_ERR_OR_NULL(mux)) {
		mux_test_sets = 0;
		KUNIT_EXPECT_EQ(test, typec_mux_set(mux, &state), 0);
		KUNIT_EXPECT_EQ(test, mux_test_sets, 1);
		typec_mux_put(mux);
	}
	typec_mux_unregister(mux_dev);
}

/* Before, the lookup waited for good: nothing registers this mux. */
static void typec_mux_in_disabled_node_skipped_test(struct kunit *test)
{
	struct device_node *conn;
	struct typec_mux *mux;

	KUNIT_ASSERT_EQ(test, 0, of_overlay_apply_kunit(test, typec_mux_test));
	conn = mux_test_node(test, "/typec-mux-test/connector-off");

	mux = fwnode_typec_mux_get(of_fwnode_handle(conn));
	KUNIT_EXPECT_NULL(test, mux);
	typec_mux_put(mux);
}

static void typec_mux_in_available_node_waited_for_test(struct kunit *test)
{
	struct device_node *conn;
	struct typec_mux *mux;

	KUNIT_ASSERT_EQ(test, 0, of_overlay_apply_kunit(test, typec_mux_test));
	conn = mux_test_node(test, "/typec-mux-test/connector-on");

	mux = fwnode_typec_mux_get(of_fwnode_handle(conn));
	KUNIT_EXPECT_EQ(test, PTR_ERR_OR_ZERO(mux), -EPROBE_DEFER);
	typec_mux_put(mux);
}

/* A mux that did register there is used all the same. */
static void typec_mux_registered_in_disabled_node_used_test(struct kunit *test)
{
	KUNIT_ASSERT_EQ(test, 0, of_overlay_apply_kunit(test, typec_mux_test));
	mux_test_lookup_one(test, "/typec-mux-test/connector-off",
			    "/typec-mux-test/processor-off/typec-routes/route@0");
}

/* The routes of a port that reaches both: only the available one is used. */
static void typec_mux_beside_disabled_route_test(struct kunit *test)
{
	KUNIT_ASSERT_EQ(test, 0, of_overlay_apply_kunit(test, typec_mux_test));
	mux_test_lookup_one(test, "/typec-mux-test/connector-both",
			    "/typec-mux-test/processor-on/typec-routes/route@1");
}

static struct kunit_case typec_mux_test_cases[] = {
	KUNIT_CASE(typec_mux_in_disabled_node_skipped_test),
	KUNIT_CASE(typec_mux_in_available_node_waited_for_test),
	KUNIT_CASE(typec_mux_registered_in_disabled_node_used_test),
	KUNIT_CASE(typec_mux_beside_disabled_route_test),
	{}
};

static struct kunit_suite typec_mux_test_suite = {
	.name = "typec_mux_lookup",
	.test_cases = typec_mux_test_cases,
};

kunit_test_suite(typec_mux_test_suite);

MODULE_DESCRIPTION("KUnit tests for the USB Type-C mux lookup");
MODULE_LICENSE("GPL");
