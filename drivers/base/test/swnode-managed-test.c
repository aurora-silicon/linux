// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit tests for the lifetime of managed software nodes.
 *
 * A node created with device_create_managed_software_node() belongs to one
 * device, but other devices may carry the same fwnode (a child that reuses
 * its parent's node, a gpiochip's gdev->dev, an i2c adapter). Removing any
 * of them must drop only that device's reference, in either order, and the
 * node must be freed exactly once after the last one is gone.
 *
 * The use-after-free and double put these tests guard against are silent
 * without CONFIG_KASAN; with it, KASAN fails the running test.
 */

#include <linux/device.h>
#include <linux/property.h>
#include <linux/slab.h>

#include <kunit/test.h>

static const struct property_entry swnode_managed_props[] = {
	PROPERTY_ENTRY_U32("test-value", 42),
	{ }
};

static void swnode_managed_dev_release(struct device *dev)
{
	kfree(dev);
}

static struct device *swnode_managed_dev_add(struct kunit *test,
					     const char *name,
					     struct fwnode_handle *fwnode)
{
	struct device *dev;
	int ret;

	dev = kzalloc_obj(*dev);
	KUNIT_ASSERT_NOT_NULL(test, dev);

	device_initialize(dev);
	dev->release = swnode_managed_dev_release;
	device_set_node(dev, fwnode);

	ret = dev_set_name(dev, "%s", name);
	if (!ret)
		ret = device_add(dev);
	if (ret) {
		put_device(dev);
		KUNIT_FAIL_AND_ABORT(test, "adding %s failed: %d\n", name, ret);
	}

	return dev;
}

/* Add the owner, give it a managed node, add a second device sharing it. */
static void swnode_managed_setup(struct kunit *test, struct device **owner,
				 struct device **sharer)
{
	struct fwnode_handle *fwnode;
	int ret;

	*owner = swnode_managed_dev_add(test, "swnode-managed-owner", NULL);

	ret = device_create_managed_software_node(*owner, swnode_managed_props,
						  NULL);
	if (ret) {
		device_unregister(*owner);
		KUNIT_FAIL_AND_ABORT(test, "creating the node failed: %d\n", ret);
	}

	fwnode = dev_fwnode(*owner);
	KUNIT_ASSERT_TRUE(test, is_software_node(fwnode));

	*sharer = swnode_managed_dev_add(test, "swnode-managed-sharer", fwnode);
}

static void swnode_managed_expect_value(struct kunit *test, struct device *dev)
{
	u32 val = 0;

	KUNIT_EXPECT_EQ(test, device_property_read_u32(dev, "test-value", &val),
			0);
	KUNIT_EXPECT_EQ(test, val, 42);
}

/*
 * The usual order: the sharer goes first. It must not take the owner's
 * reference with it, or the owner's removal frees the node and then puts
 * it again.
 */
static void swnode_managed_sharer_removed_first(struct kunit *test)
{
	struct device *owner, *sharer;

	swnode_managed_setup(test, &owner, &sharer);

	device_unregister(sharer);

	KUNIT_EXPECT_TRUE(test, is_software_node(dev_fwnode(owner)));
	swnode_managed_expect_value(test, owner);

	device_unregister(owner);
}

/*
 * The owner goes first. The sharer keeps the node alive through its own
 * reference, and its removal drops the last one: nothing may read the node
 * after that.
 */
static void swnode_managed_owner_removed_first(struct kunit *test)
{
	struct device *owner, *sharer;

	swnode_managed_setup(test, &owner, &sharer);

	device_unregister(owner);

	KUNIT_EXPECT_TRUE(test, is_software_node(dev_fwnode(sharer)));
	swnode_managed_expect_value(test, sharer);

	device_unregister(sharer);
}

static struct kunit_case swnode_managed_test_cases[] = {
	KUNIT_CASE(swnode_managed_sharer_removed_first),
	KUNIT_CASE(swnode_managed_owner_removed_first),
	{ }
};

static struct kunit_suite swnode_managed_test_suite = {
	.name = "software-node-managed",
	.test_cases = swnode_managed_test_cases,
};

kunit_test_suite(swnode_managed_test_suite);

MODULE_DESCRIPTION("Test module for managed software node lifetime");
MODULE_LICENSE("GPL");
