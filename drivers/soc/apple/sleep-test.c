// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * Included by sleep.c: each matched SoC's data. A word a list names but the SoC's full list
 * lacks is skipped silently at the wake, and a duplicate splits its books.
 */
#include <kunit/test.h>

/* Matched only once its sleep has been tested on hardware. */
static const char *const apple_sleep_test_tested[] = {
	"apple,t8140-sleep",
	"apple,t6050-sleep",
};

static bool apple_sleep_test_has(const struct apple_sleep_words *all, u32 offset)
{
	return sleep_ps_find(all->offset, all->count, offset) >= 0;
}

static void apple_sleep_test_in(struct kunit *test, const char *compat, const char *list,
				const struct apple_sleep_words *all, u32 offset)
{
	KUNIT_EXPECT_TRUE_MSG(test, apple_sleep_test_has(all, offset),
			      "%s: %s word %#x isn't in its full list", compat, list, offset);
}

static void apple_sleep_test_list_in(struct kunit *test, const char *compat, const char *list,
				     const struct apple_sleep_words *all,
				     const struct apple_sleep_words *words)
{
	int i;

	for (i = 0; i < words->count; i++)
		apple_sleep_test_in(test, compat, list, all, words->offset[i]);
}

static void apple_sleep_test_full(struct kunit *test, const char *compat, const char *list,
				  const struct apple_sleep_words *all)
{
	int i;

	KUNIT_EXPECT_GT_MSG(test, all->count, 0, "%s: %s is empty", compat, list);
	for (i = 0; i < all->count; i++) {
		KUNIT_EXPECT_EQ_MSG(test, all->offset[i] % 4, 0, "%s: %s word %#x", compat, list,
				    all->offset[i]);
		KUNIT_EXPECT_EQ_MSG(test, sleep_ps_find(all->offset, all->count, all->offset[i]), i,
				    "%s: %s has %#x twice", compat, list, all->offset[i]);
	}
}

static void apple_sleep_test_only_tested(struct kunit *test)
{
	const struct of_device_id *id;
	int n = 0;

	for (id = apple_sleep_of_match; id->compatible[0]; id++) {
		KUNIT_EXPECT_NOT_NULL_MSG(test, id->data, "%s has no data", id->compatible);
		KUNIT_ASSERT_LT(test, n, ARRAY_SIZE(apple_sleep_test_tested));
		KUNIT_EXPECT_STREQ(test, id->compatible, apple_sleep_test_tested[n++]);
	}
	KUNIT_EXPECT_EQ(test, n, ARRAY_SIZE(apple_sleep_test_tested));
}

static void apple_sleep_test_words(struct kunit *test)
{
	const struct of_device_id *id;

	for (id = apple_sleep_of_match; id->compatible[0]; id++) {
		const struct apple_sleep_soc *soc = id->data;
		const struct apple_sleep_domains *d = &soc->ps;
		const struct apple_sleep_words *all = &soc->ps_all, *aon = &soc->ps_aon_all;
		const char *c = id->compatible;
		u32 named[] = {
			d->disp[0][0], d->disp[0][1], d->disp[0][2], d->disp[1][0], d->disp[1][1],
			d->disp[1][2], d->sio_cpu, d->sep, d->usb_aon, d->usb_common, d->usb,
			d->ap_tmm, d->gpio, d->ans, d->apcie_gp, d->apcie_sys_gp, d->apcie_st,
			d->apcie_sys_st, d->apcie_phy_sw,
		};
		int i;

		/* Region-based profiles keep their words in their own tables */
		if (soc->regions)
			continue;
		apple_sleep_test_full(test, c, "ps_all", all);
		apple_sleep_test_full(test, c, "ps_aon_all", aon);
		for (i = 0; i < ARRAY_SIZE(named); i++)
			apple_sleep_test_in(test, c, "named", all, named[i]);
		apple_sleep_test_in(test, c, "debug switch", aon, soc->ps_aon_debug_switch);
		apple_sleep_test_list_in(test, c, "ps_macos_on", all, &soc->ps_macos_on);
		apple_sleep_test_list_in(test, c, "ps_macos_off", all, &soc->ps_macos_off);
		apple_sleep_test_list_in(test, c, "ps_wake_order", all, &soc->ps_wake_order);
		apple_sleep_test_list_in(test, c, "ps_still_on", all, &soc->ps_still_on);
		apple_sleep_test_list_in(test, c, "ps_aon_macos_off", aon, &soc->ps_aon_macos_off);
		apple_sleep_test_list_in(test, c, "ps_aon_macos_no_auto", aon,
					 &soc->ps_aon_macos_no_auto);
		apple_sleep_test_list_in(test, c, "ps_aon_wake_order", aon,
					 &soc->ps_aon_wake_order);
		for (i = 0; i < soc->nstray; i++)
			apple_sleep_test_in(test, c, soc->stray[i].name, all, soc->stray[i].offset);
		for (i = 0; i < soc->ngate_off; i++)
			apple_sleep_test_in(test, c, soc->gate_off[i].name, all,
					    soc->gate_off[i].offset);
		for (i = 0; i < soc->ngate_coprocs; i++) {
			KUNIT_EXPECT_TRUE(test, soc->gate_coprocs[i].path[0] == '/');
			if (soc->gate_coprocs[i].ps)
				apple_sleep_test_in(test, c, soc->gate_coprocs[i].path, all,
						    soc->gate_coprocs[i].ps);
		}
	}
}

/* The books keep one bit for each stray word and one gate for each coprocessor. */
static void apple_sleep_test_books(struct kunit *test)
{
	const struct of_device_id *id;

	for (id = apple_sleep_of_match; id->compatible[0]; id++) {
		const struct apple_sleep_soc *soc = id->data;

		KUNIT_EXPECT_LE(test, soc->nstray, BITS_PER_LONG);
		KUNIT_EXPECT_LE(test, soc->ngate_coprocs,
				ARRAY_SIZE(((struct apple_sleep *)0)->gate));
	}
}

static void apple_sleep_test_wake_names(struct kunit *test)
{
	const struct of_device_id *id;

	for (id = apple_sleep_of_match; id->compatible[0]; id++) {
		const struct apple_sleep_soc *soc = id->data;
		int i, j;

		if (soc->regions)
			continue;
		KUNIT_EXPECT_NE(test, soc->wake_last_word, 0);
		for (i = 0; i < soc->nwake_names; i++) {
			u32 n = soc->wake_names[i].id;

			KUNIT_EXPECT_LT_MSG(test, n, WAKE_WORDS * 32, "%s",
					    soc->wake_names[i].name);
			if (n / 32 == WAKE_WORDS - 1)
				KUNIT_EXPECT_TRUE_MSG(test, soc->wake_last_word & BIT(n % 32), "%s",
						      soc->wake_names[i].name);
			for (j = 0; j < i; j++)
				KUNIT_EXPECT_NE(test, soc->wake_names[j].id, n);
		}
	}
}

static void apple_sleep_test_t6050(struct kunit *test)
{
	u32 request = 0xa5a5;

	KUNIT_EXPECT_EQ(test, t6050_sleep_plan_complex_request(0xf, &request), 0);
	KUNIT_EXPECT_EQ(test, request, 0U);
	KUNIT_EXPECT_EQ(test, t6050_sleep_plan_complex_request(0x2f, &request), 0);
	KUNIT_EXPECT_EQ(test, request, 0x20U);
	KUNIT_EXPECT_EQ(test, t6050_sleep_plan_complex_request(0x3f, &request), -EOPNOTSUPP);
	KUNIT_EXPECT_EQ(test, request, 0x20U);
	KUNIT_EXPECT_NOT_NULL(test, t6050_sleep_soc.ops->commit_regs);
	KUNIT_EXPECT_NOT_NULL(test, t6050_sleep_soc.ops->display_off);
	KUNIT_EXPECT_EQ(test, sleep_region_validate(t6050_sleep_soc.regions), 0);
	KUNIT_EXPECT_EQ(test, t6050_sleep_soc.cpu_cluster_affinity, 1U);
	KUNIT_EXPECT_EQ(test, t6050_sleep_soc.cpu_start_stride, 6U);
}

static struct kunit_case apple_sleep_test_cases[] = {
	KUNIT_CASE(apple_sleep_test_t6050),
	KUNIT_CASE(apple_sleep_test_only_tested),
	KUNIT_CASE(apple_sleep_test_words),
	KUNIT_CASE(apple_sleep_test_books),
	KUNIT_CASE(apple_sleep_test_wake_names),
	{}
};

static struct kunit_suite apple_sleep_test_suite = {
	.name = "apple-sleep-soc-data",
	.test_cases = apple_sleep_test_cases,
};
kunit_test_suite(apple_sleep_test_suite);
