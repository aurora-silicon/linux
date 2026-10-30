// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* Exercise the actual firmware-property parser without MMIO or firmware. */
#include <kunit/test.h>
#include <linux/module.h>

#include "pmp-dram.h"

struct pmp_dram_fixture {
	struct software_node nodes[8];
	const struct software_node *group[9];
	struct software_node_ref_args ref[1];
	struct property_entry props[8];
	struct property_entry table_props[2];
	struct property_entry opp_props[6][4];
};

static void pmp_dram_unregister(void *data)
{
	struct pmp_dram_fixture *f = data;

	software_node_unregister_node_group(f->group);
}

static struct pmp_dram_fixture *pmp_dram_fixture(struct kunit *test, u32 count,
						 u32 width, u64 rate, u64 bandwidth_class)
{
	static const char * const names[] = {
		"opp-0", "opp-1", "opp-2", "opp-3", "opp-4", "opp-5",
	};
	struct pmp_dram_fixture *f;
	unsigned int i;

	f = kunit_kzalloc(test, sizeof(*f), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, f);
	f->ref[0] = SOFTWARE_NODE_REFERENCE(&f->nodes[1]);
	f->props[0] = PROPERTY_ENTRY_STRING("apple,dram-type", "lpddr5");
	f->props[1] = PROPERTY_ENTRY_U32("apple,data-bus-width", width);
	f->props[2] = PROPERTY_ENTRY_U32("apple,dcs-state-count", count);
	f->props[3] = PROPERTY_ENTRY_U64("apple,peak-bandwidth-bytes-per-second",
					 rate * (width / 8));
	f->props[4] = PROPERTY_ENTRY_U64("apple,bandwidth-class-bytes-per-second", bandwidth_class);
	f->props[5] = PROPERTY_ENTRY_REF_ARRAY("operating-points-v2", f->ref);
	f->nodes[0].name = "pmp-dram-test";
	f->nodes[0].properties = f->props;
	f->table_props[0] = PROPERTY_ENTRY_STRING("compatible", "operating-points-v2-apple-dram");
	f->nodes[1].name = "opp-table-dram";
	f->nodes[1].properties = f->table_props;
	for (i = 0; i < count; i++) {
		struct software_node *opp = &f->nodes[i + 2];

		opp->name = names[i];
		opp->parent = &f->nodes[1];
		opp->properties = f->opp_props[i];
		/* Reverse order: the highest state is not the last child. */
		f->opp_props[i][0] = PROPERTY_ENTRY_U32("opp-level", count - i - 1);
		if (!i) {
			f->opp_props[i][1] = PROPERTY_ENTRY_U64("apple,data-rate", rate);
			f->opp_props[i][2] = PROPERTY_ENTRY_U64("opp-hz", rate / 2 + rate % 2);
		}
	}
	for (i = 0; i < count + 2; i++)
		f->group[i] = &f->nodes[i];
	return f;
}

static int pmp_dram_parse_fixture(struct kunit *test, struct pmp_dram_fixture *f,
				  struct apple_pmp_dram *dram)
{
	int ret;

	ret = software_node_register_node_group(f->group);
	KUNIT_ASSERT_EQ(test, ret, 0);
	ret = kunit_add_action_or_reset(test, pmp_dram_unregister, f);
	KUNIT_ASSERT_EQ(test, ret, 0);
	return apple_pmp_dram_parse(software_node_fwnode(&f->nodes[0]), dram);
}

static void pmp_dram_valid(struct kunit *test)
{
	struct pmp_dram_fixture *f = pmp_dram_fixture(test, 4, 256, 6400000000ULL, 200000000000ULL);
	struct apple_pmp_dram dram = {};

	KUNIT_ASSERT_EQ(test, pmp_dram_parse_fixture(test, f, &dram), 0);
	KUNIT_EXPECT_STREQ(test, dram.type, "lpddr5");
	KUNIT_EXPECT_EQ(test, dram.max_rate, 6400000000ULL);
	KUNIT_EXPECT_EQ(test, dram.peak_bandwidth, 204800000000ULL);
	KUNIT_EXPECT_EQ(test, dram.bandwidth_class, 200000000000ULL);
	KUNIT_EXPECT_EQ(test, dram.state_count, 4U);
	KUNIT_EXPECT_EQ(test, dram.width, 256U);
}

static void pmp_dram_m2(struct kunit *test)
{
	struct pmp_dram_fixture *f = pmp_dram_fixture(test, 5, 128, 6400000000ULL, 100000000000ULL);
	struct apple_pmp_dram dram = {};

	KUNIT_ASSERT_EQ(test, pmp_dram_parse_fixture(test, f, &dram), 0);
	KUNIT_EXPECT_EQ(test, dram.peak_bandwidth, 102400000000ULL);
	KUNIT_EXPECT_EQ(test, dram.state_count, 5U);
}

static void pmp_dram_m4pro(struct kunit *test)
{
	struct pmp_dram_fixture *f = pmp_dram_fixture(test, 6, 256, 8533333333ULL, 273000000000ULL);
	struct apple_pmp_dram dram = {};

	KUNIT_ASSERT_EQ(test, pmp_dram_parse_fixture(test, f, &dram), 0);
	KUNIT_EXPECT_EQ(test, dram.max_rate, 8533333333ULL);
	KUNIT_EXPECT_EQ(test, dram.peak_bandwidth, 273066666656ULL);
	KUNIT_EXPECT_EQ(test, dram.state_count, 6U);
}

static void pmp_dram_a18pro(struct kunit *test)
{
	struct pmp_dram_fixture *f = pmp_dram_fixture(test, 5, 64, 7500000000ULL, 60000000000ULL);
	struct apple_pmp_dram dram = {};

	KUNIT_ASSERT_EQ(test, pmp_dram_parse_fixture(test, f, &dram), 0);
	KUNIT_EXPECT_EQ(test, dram.peak_bandwidth, 60000000000ULL);
	KUNIT_EXPECT_EQ(test, dram.width, 64U);
}

enum pmp_dram_fault {
	DRAM_ZERO_WIDTH, DRAM_UNALIGNED_WIDTH, DRAM_ZERO_COUNT, DRAM_LARGE_COUNT,
	DRAM_DUPLICATE, DRAM_MISSING, DRAM_UNKNOWN_LEVEL, DRAM_MISSING_LEVEL,
	DRAM_NO_MAX, DRAM_ZERO_RATE, DRAM_BAD_CK, DRAM_CK_ONLY, DRAM_BAD_PEAK,
	DRAM_OVERFLOW, DRAM_BAD_TYPE, DRAM_NO_CLASS, DRAM_ZERO_CLASS,
	DRAM_NO_TABLE, DRAM_BAD_COMPATIBLE, DRAM_LOW_ABOVE_MAX, DRAM_WRONG_TYPE,
};

static void pmp_dram_bad_description(struct kunit *test)
{
	enum pmp_dram_fault fault = *(enum pmp_dram_fault *)test->param_value;
	struct pmp_dram_fixture *f = pmp_dram_fixture(test, 4, 256, 6400000000ULL, 200000000000ULL);
	struct apple_pmp_dram dram = { .max_rate = 123 };

	switch (fault) {
	case DRAM_ZERO_WIDTH:
		f->props[1] = PROPERTY_ENTRY_U32("apple,data-bus-width", 0);
		break;
	case DRAM_UNALIGNED_WIDTH:
		f->props[1] = PROPERTY_ENTRY_U32("apple,data-bus-width", 7);
		break;
	case DRAM_ZERO_COUNT:
		f->props[2] = PROPERTY_ENTRY_U32("apple,dcs-state-count", 0);
		break;
	case DRAM_LARGE_COUNT:
		f->props[2] = PROPERTY_ENTRY_U32("apple,dcs-state-count", 33);
		break;
	case DRAM_DUPLICATE:
		f->opp_props[1][0] = PROPERTY_ENTRY_U32("opp-level", 3);
		break;
	case DRAM_MISSING:
		f->group[5] = NULL;
		break;
	case DRAM_UNKNOWN_LEVEL:
		f->opp_props[1][0] = PROPERTY_ENTRY_U32("opp-level", 4);
		break;
	case DRAM_MISSING_LEVEL:
		memset(&f->opp_props[1][0], 0, sizeof(struct property_entry));
		break;
	case DRAM_NO_MAX:
		memset(&f->opp_props[0][1], 0, 2 * sizeof(struct property_entry));
		break;
	case DRAM_ZERO_RATE:
		f->opp_props[0][1] = PROPERTY_ENTRY_U64("apple,data-rate", 0);
		break;
	case DRAM_BAD_CK:
		f->opp_props[0][2] = PROPERTY_ENTRY_U64("opp-hz", 6400000000ULL);
		break;
	case DRAM_CK_ONLY:
		f->opp_props[0][1] = f->opp_props[0][2];
		memset(&f->opp_props[0][2], 0, sizeof(struct property_entry));
		break;
	case DRAM_BAD_PEAK:
		f->props[3] = PROPERTY_ENTRY_U64("apple,peak-bandwidth-bytes-per-second",
						 200000000000ULL);
		break;
	case DRAM_OVERFLOW:
		f->opp_props[0][1] = PROPERTY_ENTRY_U64("apple,data-rate", U64_MAX - 1);
		f->opp_props[0][2] = PROPERTY_ENTRY_U64("opp-hz", (U64_MAX - 1) / 2);
		break;
	case DRAM_BAD_TYPE:
		f->props[0] = PROPERTY_ENTRY_STRING("apple,dram-type", "lpddr5x");
		break;
	case DRAM_NO_CLASS:
		f->props[4] = PROPERTY_ENTRY_U64("unrelated", 1);
		break;
	case DRAM_ZERO_CLASS:
		f->props[4] = PROPERTY_ENTRY_U64("apple,bandwidth-class-bytes-per-second", 0);
		break;
	case DRAM_NO_TABLE:
		memset(&f->props[5], 0, sizeof(struct property_entry));
		break;
	case DRAM_BAD_COMPATIBLE:
		f->table_props[0] = PROPERTY_ENTRY_STRING("compatible", "operating-points-v2");
		break;
	case DRAM_LOW_ABOVE_MAX:
		f->opp_props[1][1] = PROPERTY_ENTRY_U64("apple,data-rate", 7500000000ULL);
		f->opp_props[1][2] = PROPERTY_ENTRY_U64("opp-hz", 3750000000ULL);
		break;
	case DRAM_WRONG_TYPE:
		f->props[2] = PROPERTY_ENTRY_STRING("apple,dcs-state-count", "four");
		break;
	}
	KUNIT_EXPECT_LT(test, pmp_dram_parse_fixture(test, f, &dram), 0);
	/* Never publish partially parsed values or a fallback maximum. */
	KUNIT_EXPECT_EQ(test, dram.max_rate, 123ULL);
}

static const enum pmp_dram_fault pmp_dram_faults[] = {
	DRAM_ZERO_WIDTH, DRAM_UNALIGNED_WIDTH, DRAM_ZERO_COUNT, DRAM_LARGE_COUNT,
	DRAM_DUPLICATE, DRAM_MISSING, DRAM_UNKNOWN_LEVEL, DRAM_MISSING_LEVEL,
	DRAM_NO_MAX, DRAM_ZERO_RATE, DRAM_BAD_CK, DRAM_CK_ONLY, DRAM_BAD_PEAK,
	DRAM_OVERFLOW, DRAM_BAD_TYPE, DRAM_NO_CLASS, DRAM_ZERO_CLASS,
	DRAM_NO_TABLE, DRAM_BAD_COMPATIBLE, DRAM_LOW_ABOVE_MAX, DRAM_WRONG_TYPE,
};

KUNIT_ARRAY_PARAM(pmp_dram_fault, pmp_dram_faults, NULL);

static void pmp_dram_old_dt(struct kunit *test)
{
	struct pmp_dram_fixture *f = pmp_dram_fixture(test, 4, 256, 6400000000ULL, 200000000000ULL);
	struct apple_pmp_dram dram = {};

	memset(f->props, 0, sizeof(f->props));
	KUNIT_EXPECT_EQ(test, pmp_dram_parse_fixture(test, f, &dram), -ENODATA);
}

static void pmp_dram_partial_dt(struct kunit *test)
{
	struct pmp_dram_fixture *f = pmp_dram_fixture(test, 4, 256, 6400000000ULL, 200000000000ULL);
	struct apple_pmp_dram dram = {};

	f->props[0] = PROPERTY_ENTRY_U32("apple,data-bus-width", 128);
	memset(&f->props[1], 0, sizeof(struct property_entry));
	KUNIT_EXPECT_EQ(test, pmp_dram_parse_fixture(test, f, &dram), -EINVAL);
}

static struct kunit_case pmp_dram_cases[] = {
	KUNIT_CASE(pmp_dram_valid),
	KUNIT_CASE(pmp_dram_m2),
	KUNIT_CASE(pmp_dram_m4pro),
	KUNIT_CASE(pmp_dram_a18pro),
	KUNIT_CASE_PARAM(pmp_dram_bad_description, pmp_dram_fault_gen_params),
	KUNIT_CASE(pmp_dram_old_dt),
	KUNIT_CASE(pmp_dram_partial_dt),
	{}
};

static struct kunit_suite pmp_dram_suite = {
	.name = "apple-pmp-dram",
	.test_cases = pmp_dram_cases,
};

kunit_test_suite(pmp_dram_suite);

MODULE_DESCRIPTION("KUnit tests for Apple PMP DRAM property parsing");
MODULE_LICENSE("Dual MIT/GPL");
