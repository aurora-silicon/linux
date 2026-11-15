// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* Copyright 2026 Ryan Murray */

#include <kunit/test.h>
#include <linux/kernel.h>

#include "apple_boot_display.h"

#define EDP	DRM_MODE_CONNECTOR_eDP
#define HDMI	DRM_MODE_CONNECTOR_HDMIA
#define USB	DRM_MODE_CONNECTOR_USB

/* A laptop: the internal panel's processor carries the boot framebuffer. */
static void boot_display_laptop_test(struct kunit *test)
{
	const int type[] = { EDP, USB };
	bool ready[] = { true, true };

	KUNIT_EXPECT_TRUE(test, apple_boot_display_up(type, ready, ARRAY_SIZE(type)));

	/* The panel's processor failed: an external one does not replace it. */
	ready[0] = false;
	KUNIT_EXPECT_FALSE(test, apple_boot_display_up(type, ready, ARRAY_SIZE(type)));

	/* Only the external processor failed: the panel goes on. */
	ready[0] = true;
	ready[1] = false;
	KUNIT_EXPECT_TRUE(test, apple_boot_display_up(type, ready, ARRAY_SIZE(type)));
}

/* A laptop with an HDMI port: the panel still comes first. */
static void boot_display_laptop_hdmi_test(struct kunit *test)
{
	const int type[] = { EDP, HDMI, USB };
	bool ready[] = { false, true, true };

	KUNIT_EXPECT_FALSE(test, apple_boot_display_up(type, ready, ARRAY_SIZE(type)));
}

/* A desktop: the HDMI port's processor carries it. */
static void boot_display_desktop_test(struct kunit *test)
{
	const int type[] = { USB, HDMI };
	bool ready[] = { true, false };

	KUNIT_EXPECT_FALSE(test, apple_boot_display_up(type, ready, ARRAY_SIZE(type)));
	ready[1] = true;
	KUNIT_EXPECT_TRUE(test, apple_boot_display_up(type, ready, ARRAY_SIZE(type)));
}

/* Only Type-C outputs: any one of them will do. */
static void boot_display_typec_only_test(struct kunit *test)
{
	const int type[] = { USB, DRM_MODE_CONNECTOR_Unknown };
	bool ready[] = { false, true };

	KUNIT_EXPECT_TRUE(test, apple_boot_display_up(type, ready, ARRAY_SIZE(type)));
	ready[1] = false;
	KUNIT_EXPECT_FALSE(test, apple_boot_display_up(type, ready, ARRAY_SIZE(type)));
}

/* Nothing came up, or there was nothing: keep the boot framebuffer. */
static void boot_display_none_test(struct kunit *test)
{
	const int type[] = { EDP };
	const bool ready[] = { false };

	KUNIT_EXPECT_FALSE(test, apple_boot_display_up(type, ready, ARRAY_SIZE(type)));
	KUNIT_EXPECT_FALSE(test, apple_boot_display_up(type, ready, 0));
}

static struct kunit_case boot_display_cases[] = {
	KUNIT_CASE(boot_display_laptop_test),
	KUNIT_CASE(boot_display_laptop_hdmi_test),
	KUNIT_CASE(boot_display_desktop_test),
	KUNIT_CASE(boot_display_typec_only_test),
	KUNIT_CASE(boot_display_none_test),
	{}
};

static struct kunit_suite boot_display_suite = {
	.name = "apple-drm-boot-display",
	.test_cases = boot_display_cases,
};

kunit_test_suite(boot_display_suite);
