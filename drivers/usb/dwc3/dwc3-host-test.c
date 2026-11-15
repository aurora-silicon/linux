// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit tests for the properties dwc3 gives its xHCI device
 */

#include <kunit/test.h>
#include <kunit/visibility.h>
#include <linux/module.h>
#include <linux/property.h>
#include <linux/slab.h>
#include <linux/string.h>

#include "core.h"

struct dwc3_host_test {
	struct dwc3 *dwc;
	struct property_entry props[DWC3_XHCI_MAX_PROPERTIES + 1];
};

static int dwc3_host_test_init(struct kunit *test)
{
	struct dwc3_host_test *t;

	t = kunit_kzalloc(test, sizeof(*t), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, t);
	t->dwc = kunit_kzalloc(test, sizeof(*t->dwc), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, t->dwc);
	/* Fill with garbage: the helper has to clear what it does not set. */
	memset(t->props, 0xa5, sizeof(t->props));
	test->priv = t;

	return 0;
}

static const struct property_entry *dwc3_host_test_find(const struct property_entry *props,
							const char *name)
{
	for (; props->name; props++)
		if (!strcmp(props->name, name))
			return props;

	return NULL;
}

/* Without the glue asking for it, xHCI is not told to keep running in suspend-to-idle. */
static void dwc3_host_no_run_in_s2idle_test(struct kunit *test)
{
	struct dwc3_host_test *t = test->priv;
	unsigned int n;

	n = dwc3_host_xhci_properties(t->dwc, t->props);

	KUNIT_EXPECT_LE(test, n, DWC3_XHCI_MAX_PROPERTIES);
	KUNIT_EXPECT_NULL(test, t->props[n].name);
	KUNIT_EXPECT_NULL(test, dwc3_host_test_find(t->props, "xhci-run-in-s2idle"));
	KUNIT_EXPECT_NOT_NULL(test, dwc3_host_test_find(t->props, "xhci-sg-trb-cache-size-quirk"));
}

/* The glue property reaches xHCI as a boolean property. */
static void dwc3_host_run_in_s2idle_test(struct kunit *test)
{
	struct dwc3_host_test *t = test->priv;
	const struct property_entry *prop;
	unsigned int n;

	t->dwc->xhci_run_in_s2idle = true;
	n = dwc3_host_xhci_properties(t->dwc, t->props);

	KUNIT_EXPECT_NULL(test, t->props[n].name);
	prop = dwc3_host_test_find(t->props, "xhci-run-in-s2idle");
	KUNIT_ASSERT_NOT_NULL(test, prop);
	/* A boolean property has no value. */
	KUNIT_EXPECT_EQ(test, prop->length, 0);
}

/* With every optional property set, the array still ends in an empty entry. */
static void dwc3_host_all_properties_test(struct kunit *test)
{
	struct dwc3_host_test *t = test->priv;
	unsigned int n;

	t->dwc->ip = DWC3_IP;
	t->dwc->revision = DWC3_REVISION_290A;
	t->dwc->usb3_lpm_capable = true;
	t->dwc->usb2_lpm_disable = true;
	t->dwc->xhci_run_in_s2idle = true;
	n = dwc3_host_xhci_properties(t->dwc, t->props);

	KUNIT_EXPECT_EQ(test, n, DWC3_XHCI_MAX_PROPERTIES);
	KUNIT_EXPECT_NULL(test, t->props[n].name);
	KUNIT_EXPECT_NOT_NULL(test, dwc3_host_test_find(t->props, "usb3-lpm-capable"));
	KUNIT_EXPECT_NOT_NULL(test, dwc3_host_test_find(t->props, "usb2-lpm-disable"));
	KUNIT_EXPECT_NOT_NULL(test, dwc3_host_test_find(t->props, "quirk-broken-port-ped"));
	KUNIT_EXPECT_NOT_NULL(test, dwc3_host_test_find(t->props, "num-hc-interrupters"));
	KUNIT_EXPECT_NOT_NULL(test, dwc3_host_test_find(t->props, "xhci-run-in-s2idle"));
}

static struct kunit_case dwc3_host_test_cases[] = {
	KUNIT_CASE(dwc3_host_no_run_in_s2idle_test),
	KUNIT_CASE(dwc3_host_run_in_s2idle_test),
	KUNIT_CASE(dwc3_host_all_properties_test),
	{}
};

static struct kunit_suite dwc3_host_test_suite = {
	.name = "dwc3_host",
	.init = dwc3_host_test_init,
	.test_cases = dwc3_host_test_cases,
};

kunit_test_suite(dwc3_host_test_suite);

MODULE_DESCRIPTION("KUnit tests for the dwc3 xHCI device properties");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS("EXPORTED_FOR_KUNIT_TESTING");
