// SPDX-License-Identifier: GPL-2.0-only
/* Policy lookup tests; no sysfs callbacks or hardware operations. */
#include <kunit/test.h>
#include <linux/pci.h>

#include "pci.h"

struct pci_sysfs_test_hierarchy {
	struct pci_host_bridge *bridge;
	struct pci_bus *root_bus;
	struct pci_bus *child_bus;
	struct pci_bus *grandchild_bus;
	struct pci_dev *root_port;
	struct pci_dev *endpoint;
	struct pci_dev *grandchild;
};

static int pci_sysfs_test_init(struct kunit *test)
{
	struct pci_sysfs_test_hierarchy *h;

	h = kunit_kzalloc(test, sizeof(*h), GFP_KERNEL);
	if (!h)
		return -ENOMEM;
	h->bridge = kunit_kzalloc(test, sizeof(*h->bridge), GFP_KERNEL);
	h->root_bus = kunit_kzalloc(test, sizeof(*h->root_bus), GFP_KERNEL);
	h->child_bus = kunit_kzalloc(test, sizeof(*h->child_bus), GFP_KERNEL);
	h->grandchild_bus = kunit_kzalloc(test, sizeof(*h->grandchild_bus), GFP_KERNEL);
	h->root_port = kunit_kzalloc(test, sizeof(*h->root_port), GFP_KERNEL);
	h->endpoint = kunit_kzalloc(test, sizeof(*h->endpoint), GFP_KERNEL);
	h->grandchild = kunit_kzalloc(test, sizeof(*h->grandchild), GFP_KERNEL);
	if (!h->bridge || !h->root_bus || !h->child_bus || !h->grandchild_bus ||
	    !h->root_port || !h->endpoint || !h->grandchild)
		return -ENOMEM;
	/* Only the real host lookup's immutable topology is needed. */
	h->root_bus->bridge = &h->bridge->dev;
	h->child_bus->parent = h->root_bus;
	h->grandchild_bus->parent = h->child_bus;
	h->root_port->bus = h->root_bus;
	h->endpoint->bus = h->child_bus;
	h->grandchild->bus = h->grandchild_bus;
	test->priv = h;
	return 0;
}

static void pci_sysfs_remove_default_test(struct kunit *test)
{
	struct pci_sysfs_test_hierarchy *h = test->priv;

	KUNIT_EXPECT_EQ(test, pci_sysfs_remove_check(h->root_port), 0);
	KUNIT_EXPECT_EQ(test, pci_sysfs_remove_check(h->endpoint), 0);
	KUNIT_EXPECT_EQ(test, pci_sysfs_remove_check(h->grandchild), 0);
}

static void pci_sysfs_remove_retained_test(struct kunit *test)
{
	struct pci_sysfs_test_hierarchy *h = test->priv;

	h->bridge->no_user_remove = true;
	KUNIT_EXPECT_EQ(test, pci_sysfs_remove_check(h->root_port), -EOPNOTSUPP);
	KUNIT_EXPECT_EQ(test, pci_sysfs_remove_check(h->endpoint), -EOPNOTSUPP);
	KUNIT_EXPECT_EQ(test, pci_sysfs_remove_check(h->grandchild), -EOPNOTSUPP);
}

static void pci_sysfs_remove_independent_host_test(struct kunit *test)
{
	struct pci_sysfs_test_hierarchy *h = test->priv;
	struct pci_host_bridge *other;

	other = kunit_kzalloc(test, sizeof(*other), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, other);
	h->bridge->no_user_remove = true;
	KUNIT_ASSERT_EQ(test, pci_sysfs_remove_check(h->grandchild), -EOPNOTSUPP);
	/* A descendant of an unrestricted host must not inherit another veto. */
	h->root_bus->bridge = &other->dev;
	KUNIT_EXPECT_EQ(test, pci_sysfs_remove_check(h->grandchild), 0);
}

static void pci_sysfs_reset_default_test(struct kunit *test)
{
	struct pci_sysfs_test_hierarchy *h = test->priv;

	KUNIT_EXPECT_EQ(test, pci_sysfs_reset_check(h->root_port), 0);
	KUNIT_EXPECT_EQ(test, pci_sysfs_reset_check(h->endpoint), 0);
	KUNIT_EXPECT_EQ(test, pci_sysfs_reset_check(h->grandchild), 0);
}

static void pci_sysfs_reset_retained_test(struct kunit *test)
{
	struct pci_sysfs_test_hierarchy *h = test->priv;

	h->bridge->reset_sensitive = true;
	KUNIT_EXPECT_EQ(test, pci_sysfs_reset_check(h->root_port), -EOPNOTSUPP);
	KUNIT_EXPECT_EQ(test, pci_sysfs_reset_check(h->endpoint), -EOPNOTSUPP);
	KUNIT_EXPECT_EQ(test, pci_sysfs_reset_check(h->grandchild), -EOPNOTSUPP);
	/* Reset and removal policies are independent for other host users. */
	KUNIT_EXPECT_EQ(test, pci_sysfs_remove_check(h->grandchild), 0);
	h->bridge->reset_sensitive = false;
	h->bridge->no_user_remove = true;
	KUNIT_EXPECT_EQ(test, pci_sysfs_reset_check(h->grandchild), 0);
}

static void pci_sysfs_reset_independent_host_test(struct kunit *test)
{
	struct pci_sysfs_test_hierarchy *h = test->priv;
	struct pci_host_bridge *other;

	other = kunit_kzalloc(test, sizeof(*other), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, other);
	h->bridge->reset_sensitive = true;
	KUNIT_ASSERT_EQ(test, pci_sysfs_reset_check(h->grandchild), -EOPNOTSUPP);
	h->root_bus->bridge = &other->dev;
	KUNIT_EXPECT_EQ(test, pci_sysfs_reset_check(h->grandchild), 0);
}

static struct kunit_case pci_sysfs_test_cases[] = {
	KUNIT_CASE(pci_sysfs_remove_default_test),
	KUNIT_CASE(pci_sysfs_remove_retained_test),
	KUNIT_CASE(pci_sysfs_remove_independent_host_test),
	KUNIT_CASE(pci_sysfs_reset_default_test),
	KUNIT_CASE(pci_sysfs_reset_retained_test),
	KUNIT_CASE(pci_sysfs_reset_independent_host_test),
	{}
};

static struct kunit_suite pci_sysfs_test_suite = {
	.name = "pci-sysfs-policy",
	.init = pci_sysfs_test_init,
	.test_cases = pci_sysfs_test_cases,
};

kunit_test_suite(pci_sysfs_test_suite);
