// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit tests for the generic xHCI platform driver
 */

#include <kunit/device.h>
#include <kunit/test.h>
#include <kunit/visibility.h>
#include <linux/device.h>
#include <linux/module.h>
#include <linux/property.h>

#include "xhci.h"
#include "xhci-plat.h"

/* Only suspend-to-idle with the quirk, wakeup enabled and a wakeup path keeps it running. */
static void xhci_plat_keep_running_test(struct kunit *test)
{
	static const suspend_state_t targets[] = {
		PM_SUSPEND_ON, PM_SUSPEND_TO_IDLE, PM_SUSPEND_STANDBY, PM_SUSPEND_MEM,
	};
	unsigned int i, combo;

	for (i = 0; i < ARRAY_SIZE(targets); i++) {
		for (combo = 0; combo < 8; combo++) {
			u64 quirks = (combo & 1) ? XHCI_RUN_IN_S2IDLE : 0;
			bool may_wakeup = combo & 2, wakeup_path = combo & 4;
			bool expected = combo == 7 && targets[i] == PM_SUSPEND_TO_IDLE;

			bool kept = xhci_plat_keep_running(quirks, targets[i], may_wakeup,
							   wakeup_path);

			KUNIT_EXPECT_EQ_MSG(test, kept, expected,
					    "target %d quirk %d may_wakeup %d wakeup_path %d",
					    targets[i], combo & 1, may_wakeup, wakeup_path);
		}
	}
}

/* Other quirks do not stand in for the suspend-to-idle one. */
static void xhci_plat_keep_running_other_quirks_test(struct kunit *test)
{
	KUNIT_EXPECT_FALSE(test, xhci_plat_keep_running(~XHCI_RUN_IN_S2IDLE, PM_SUSPEND_TO_IDLE,
							true, true));
	KUNIT_EXPECT_TRUE(test, xhci_plat_keep_running(~0ULL, PM_SUSPEND_TO_IDLE, true, true));
}

/* A running controller is left alone; a halted, failed or unpowered one is brought up again. */
static void xhci_plat_stopped_test(struct kunit *test)
{
	/* Running, with an event pending and a port change */
	KUNIT_EXPECT_FALSE(test, xhci_plat_stopped(0));
	KUNIT_EXPECT_FALSE(test, xhci_plat_stopped(STS_EINT | STS_PORT));
	/* Halted, Host System Error, Host Controller Error */
	KUNIT_EXPECT_TRUE(test, xhci_plat_stopped(STS_HALT));
	KUNIT_EXPECT_TRUE(test, xhci_plat_stopped(STS_FATAL | STS_HALT));
	KUNIT_EXPECT_TRUE(test, xhci_plat_stopped(STS_HCE));
	/* Register reads of a powered-off block */
	KUNIT_EXPECT_TRUE(test, xhci_plat_stopped(~0U));
}

static u64 xhci_plat_test_quirks(struct kunit *test, const struct property_entry *props)
{
	struct device *dev;

	dev = kunit_device_register(test, "xhci-plat-test");
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, dev);
	KUNIT_ASSERT_EQ(test, device_create_managed_software_node(dev, props, NULL), 0);

	return xhci_plat_property_quirks(dev);
}

/* A device without properties gets no quirks. */
static void xhci_plat_no_property_quirks_test(struct kunit *test)
{
	static const struct property_entry props[] = { {} };

	KUNIT_EXPECT_EQ(test, xhci_plat_test_quirks(test, props), 0ULL);
}

/* The property a glue driver sets for suspend-to-idle maps to its quirk, and only to it. */
static void xhci_plat_run_in_s2idle_property_test(struct kunit *test)
{
	static const struct property_entry props[] = {
		PROPERTY_ENTRY_BOOL("xhci-run-in-s2idle"),
		{}
	};

	KUNIT_EXPECT_EQ(test, xhci_plat_test_quirks(test, props), XHCI_RUN_IN_S2IDLE);
}

/* Every boolean property maps to its quirk. */
static void xhci_plat_all_property_quirks_test(struct kunit *test)
{
	static const struct property_entry props[] = {
		PROPERTY_ENTRY_BOOL("usb2-lpm-disable"),
		PROPERTY_ENTRY_BOOL("usb3-lpm-capable"),
		PROPERTY_ENTRY_BOOL("quirk-broken-port-ped"),
		PROPERTY_ENTRY_BOOL("xhci-sg-trb-cache-size-quirk"),
		PROPERTY_ENTRY_BOOL("write-64-hi-lo-quirk"),
		PROPERTY_ENTRY_BOOL("xhci-missing-cas-quirk"),
		PROPERTY_ENTRY_BOOL("xhci-skip-phy-init-quirk"),
		PROPERTY_ENTRY_BOOL("xhci-run-in-s2idle"),
		{}
	};

	KUNIT_EXPECT_EQ(test, xhci_plat_test_quirks(test, props),
			XHCI_HW_LPM_DISABLE | XHCI_LPM_SUPPORT | XHCI_BROKEN_PORT_PED |
			XHCI_SG_TRB_CACHE_SIZE_QUIRK | XHCI_WRITE_64_HI_LO | XHCI_MISSING_CAS |
			XHCI_SKIP_PHY_INIT | XHCI_RUN_IN_S2IDLE);
}

static struct kunit_case xhci_plat_test_cases[] = {
	KUNIT_CASE(xhci_plat_keep_running_test),
	KUNIT_CASE(xhci_plat_keep_running_other_quirks_test),
	KUNIT_CASE(xhci_plat_stopped_test),
	KUNIT_CASE(xhci_plat_no_property_quirks_test),
	KUNIT_CASE(xhci_plat_run_in_s2idle_property_test),
	KUNIT_CASE(xhci_plat_all_property_quirks_test),
	{}
};

static struct kunit_suite xhci_plat_test_suite = {
	.name = "xhci_plat",
	.test_cases = xhci_plat_test_cases,
};
kunit_test_suite(xhci_plat_test_suite);

MODULE_DESCRIPTION("KUnit tests for the generic xHCI platform driver");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS("EXPORTED_FOR_KUNIT_TESTING");
