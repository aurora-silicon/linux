// SPDX-License-Identifier: GPL-2.0-only
#include <kunit/test.h>

#include "pcie-apple-pref-window.h"

static void mirror_follows_kernel_init(struct kunit *test)
{
	KUNIT_EXPECT_TRUE(test, apple_pcie_mirror_pref_window(true, false, false));
}

static void mirror_follows_cold_init(struct kunit *test)
{
	KUNIT_EXPECT_TRUE(test, apple_pcie_mirror_pref_window(false, true, false));
}

/*
 * USB4 PCIe-C tunnels (tunnel_intr2axi) probe with kernel_init == false, and
 * on retained-power boots tunnel_cold_init is false too. b43d20295a73 left
 * the mirror off for exactly this case, so on jw16 the port block kept the
 * previous boot's window latch while realloc assigned a fresh layout; BAR0
 * MMIO then read all-ones and xe misdetected SR-IOV VF mode. The mirror must
 * cover these ports.
 */
static void mirror_covers_intr2axi_tunnels(struct kunit *test)
{
	KUNIT_EXPECT_TRUE(test, apple_pcie_mirror_pref_window(false, false, true));
}

static void no_mirror_when_nothing_applies(struct kunit *test)
{
	KUNIT_EXPECT_FALSE(test, apple_pcie_mirror_pref_window(false, false, false));
}

static struct kunit_case apple_pcie_pref_window_cases[] = {
	KUNIT_CASE(mirror_follows_kernel_init),
	KUNIT_CASE(mirror_follows_cold_init),
	KUNIT_CASE(mirror_covers_intr2axi_tunnels),
	KUNIT_CASE(no_mirror_when_nothing_applies),
	{},
};

static struct kunit_suite apple_pcie_pref_window_suite = {
	.name = "pcie-apple-pref-window",
	.test_cases = apple_pcie_pref_window_cases,
};

kunit_test_suite(apple_pcie_pref_window_suite);
MODULE_LICENSE("GPL");
