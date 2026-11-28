// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * KUnit tests for the check that the xART store's block device is on the
 * internal NVMe disk. The device trees are built by hand and never registered.
 */

#include <kunit/test.h>

static const struct device_driver sep_store_test_ans = { .name = "nvme-apple" };
static const struct device_driver sep_store_test_usb = { .name = "xhci-hcd" };
static const struct device_driver sep_store_test_pcie = { .name = "pcie-apple" };
static const struct class sep_store_test_block = { .name = "block" };
static const char sep_store_test_label[] = "iBootSystemContainer";

/*
 * disk -> controller -> @host, as for an NVMe namespace, plus two spare
 * devices for the cases that insert a bridge or add a top level. Allocated,
 * as struct device is too large for the stack.
 */
struct sep_store_test_chain {
	struct device host, ctrl, disk, extra, top;
	struct gendisk gd;
	struct block_device whole, part;
	struct partition_meta_info meta;
};

static struct sep_store_test_chain *
sep_store_test_chain(struct kunit *test, const struct device_driver *drv)
{
	struct sep_store_test_chain *c = kunit_kzalloc(test, sizeof(*c), GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, c);
	c->host.bus = &platform_bus_type;
	c->host.driver = (struct device_driver *)drv;
	c->ctrl.parent = &c->host;
	c->disk.parent = &c->ctrl;
	c->gd.part0 = &c->whole;
	c->whole.bd_device.parent = &c->ctrl;
	c->part.bd_disk = &c->gd;
	c->part.bd_device.class = &sep_store_test_block;
	c->part.bd_meta_info = &c->meta;
	atomic_set(&c->part.__bd_flags, 1);
	strscpy(c->meta.volname, sep_store_test_label);
	return c;
}

static void sep_store_accepts_the_embedded_nvme(struct kunit *test)
{
	struct sep_store_test_chain *c = sep_store_test_chain(test, &sep_store_test_ans);

	KUNIT_EXPECT_TRUE(test, sep_store_dev_is_internal(&c->disk));
}

static void sep_store_refuses_a_usb_disk(struct kunit *test)
{
	struct sep_store_test_chain *c = sep_store_test_chain(test, &sep_store_test_usb);

	/* disk -> SCSI host -> USB controller */
	c->extra.parent = &c->ctrl;
	c->disk.parent = &c->extra;
	KUNIT_EXPECT_FALSE(test, sep_store_dev_is_internal(&c->disk));
}

/* PCIe or Thunderbolt NVMe: the PCI function is not a platform device. */
static void sep_store_refuses_a_pcie_nvme_disk(struct kunit *test)
{
	struct sep_store_test_chain *c = sep_store_test_chain(test, &sep_store_test_pcie);

	c->extra.parent = &c->host;
	c->ctrl.parent = &c->extra;
	KUNIT_EXPECT_FALSE(test, sep_store_dev_is_internal(&c->disk));
}

static void sep_store_refuses_an_unbound_controller(struct kunit *test)
{
	struct sep_store_test_chain *c = sep_store_test_chain(test, NULL);

	KUNIT_EXPECT_FALSE(test, sep_store_dev_is_internal(&c->disk));
}

/* A loop device, or any disk with no platform ancestor. */
static void sep_store_refuses_a_disk_without_a_controller(struct kunit *test)
{
	struct sep_store_test_chain *c = sep_store_test_chain(test, NULL);

	c->ctrl.parent = NULL;
	KUNIT_EXPECT_FALSE(test, sep_store_dev_is_internal(&c->disk));
	c->disk.parent = NULL;
	KUNIT_EXPECT_FALSE(test, sep_store_dev_is_internal(&c->disk));
}

/* Only the nearest platform ancestor decides. */
static void sep_store_checks_the_nearest_platform_device(struct kunit *test)
{
	struct sep_store_test_chain *c = sep_store_test_chain(test, &sep_store_test_usb);

	c->top.bus = &platform_bus_type;
	c->top.driver = (struct device_driver *)&sep_store_test_ans;
	c->host.parent = &c->top;
	KUNIT_EXPECT_FALSE(test, sep_store_dev_is_internal(&c->disk));
}

static void sep_store_selects_internal_despite_duplicate_usb_label(struct kunit *test)
{
	struct sep_store_test_chain *usb = sep_store_test_chain(test, &sep_store_test_usb);
	struct sep_store_test_chain *ans = sep_store_test_chain(test, &sep_store_test_ans);

	/* The USB candidate must be skipped even when encountered first. */
	KUNIT_EXPECT_FALSE(test, sep_store_matches_partition(&usb->part.bd_device,
							     sep_store_test_label));
	KUNIT_EXPECT_TRUE(test, sep_store_matches_partition(&ans->part.bd_device,
							    sep_store_test_label));
}

static void sep_store_requires_the_partition_label(struct kunit *test)
{
	struct sep_store_test_chain *c = sep_store_test_chain(test, &sep_store_test_ans);

	strscpy(c->meta.volname, "other");
	KUNIT_EXPECT_FALSE(test, sep_store_matches_partition(&c->part.bd_device,
							     sep_store_test_label));
	c->part.bd_meta_info = NULL;
	KUNIT_EXPECT_FALSE(test, sep_store_matches_partition(&c->part.bd_device,
							     sep_store_test_label));
}

static void sep_store_requires_a_partition(struct kunit *test)
{
	struct sep_store_test_chain *c = sep_store_test_chain(test, &sep_store_test_ans);

	atomic_set(&c->part.__bd_flags, 0);
	KUNIT_EXPECT_FALSE(test, sep_store_matches_partition(&c->part.bd_device,
							     sep_store_test_label));
}

static void sep_store_requires_a_block_device(struct kunit *test)
{
	struct sep_store_test_chain *c = sep_store_test_chain(test, &sep_store_test_ans);

	KUNIT_EXPECT_FALSE(test, sep_store_matches_partition(&c->disk,
							     sep_store_test_label));
}

static struct kunit_case sep_store_internal_cases[] = {
	KUNIT_CASE(sep_store_accepts_the_embedded_nvme),
	KUNIT_CASE(sep_store_refuses_a_usb_disk),
	KUNIT_CASE(sep_store_refuses_a_pcie_nvme_disk),
	KUNIT_CASE(sep_store_refuses_an_unbound_controller),
	KUNIT_CASE(sep_store_refuses_a_disk_without_a_controller),
	KUNIT_CASE(sep_store_checks_the_nearest_platform_device),
	KUNIT_CASE(sep_store_selects_internal_despite_duplicate_usb_label),
	KUNIT_CASE(sep_store_requires_the_partition_label),
	KUNIT_CASE(sep_store_requires_a_partition),
	KUNIT_CASE(sep_store_requires_a_block_device),
	{}
};

static struct kunit_suite sep_store_internal_suite = {
	.name = "apple_sep_store_internal",
	.test_cases = sep_store_internal_cases,
};

kunit_test_suite(sep_store_internal_suite);
