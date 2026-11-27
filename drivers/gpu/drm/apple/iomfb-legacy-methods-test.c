// SPDX-License-Identifier: GPL-2.0-only
/* Included by the 13.3 IOMFB backend shared by firmware 13.5. */
#include <kunit/test.h>

static void iomfb_legacy_required_methods(struct kunit *test)
{
	static const enum dcpep_method required[] = {
		dcpep_start_signal,
		dcpep_set_digital_out_mode,
		dcpep_swap_start,
		dcpep_swap_submit,
		dcpep_set_power_state,
	};
	char empty[sizeof(dcp_methods[0].tag)] = {};
	unsigned int i, j;

	for (i = 0; i < ARRAY_SIZE(required); i++) {
		const struct dcp_method_entry *entry = &dcp_methods[required[i]];

		KUNIT_EXPECT_NOT_NULL(test, entry->name);
		KUNIT_EXPECT_MEMNEQ(test, entry->tag, empty, sizeof(entry->tag));
		for (j = 0; j < i; j++)
			KUNIT_EXPECT_MEMNEQ(test, entry->tag, dcp_methods[required[j]].tag,
					    sizeof(entry->tag));
	}
}

static struct kunit_case iomfb_legacy_methods_cases[] = {
	KUNIT_CASE(iomfb_legacy_required_methods),
	{}
};

static struct kunit_suite iomfb_legacy_methods_suite = {
	.name = "apple-iomfb-legacy-methods",
	.test_cases = iomfb_legacy_methods_cases,
};

kunit_test_suite(iomfb_legacy_methods_suite);
