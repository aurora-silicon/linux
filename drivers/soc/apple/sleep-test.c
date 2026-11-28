// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * Included by sleep.c: each matched SoC's data. A word a list names but the SoC's full list
 * lacks is skipped silently at the wake, and a duplicate splits its books.
 */
#include <kunit/test.h>

/* Qualified, commit-gated and explicitly unavailable inventory-only profiles. */
static const char *const apple_sleep_test_profiles[] = {
	"apple,t8140-sleep",
	"apple,t8132-sleep",
	"apple,t8142-sleep",
	"apple,t6040-sleep",
	"apple,t6050-sleep",
	"apple,t6050-j716c-sleep",
	"apple,t8103-sleep",
	"apple,t8122-sleep",
};

/*
 * Region-based profiles, and those whose operations drive every word themselves (T8103),
 * keep their words in their own tables, not in the shared lists checked here.
 */
static bool apple_sleep_test_own_tables(const struct apple_sleep_soc *soc)
{
	return soc->regions || !soc->ps_all.count;
}

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

static void apple_sleep_test_known_profiles(struct kunit *test)
{
	const struct of_device_id *id;
	int n = 0;

	for (id = apple_sleep_of_match; id->compatible[0]; id++, n++) {
		KUNIT_EXPECT_NOT_NULL_MSG(test, id->data, "%s has no data", id->compatible);
		KUNIT_ASSERT_LT(test, n, ARRAY_SIZE(apple_sleep_test_profiles));
		KUNIT_EXPECT_STREQ(test, id->compatible, apple_sleep_test_profiles[n]);
		if (!strcmp(id->compatible, "apple,t8142-sleep") ||
		    !strcmp(id->compatible, "apple,t8122-sleep")) {
			const struct apple_sleep_soc *soc = id->data;

			KUNIT_ASSERT_NOT_NULL(test, soc);
			KUNIT_EXPECT_TRUE(test, soc->untested);
		}
	}
	KUNIT_EXPECT_EQ(test, n, ARRAY_SIZE(apple_sleep_test_profiles));
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

		if (apple_sleep_test_own_tables(soc))
			continue;
		apple_sleep_test_full(test, c, "ps_all", all);
		apple_sleep_test_full(test, c, "ps_aon_all", aon);
		/* 0: a word the SoC doesn't name (a second display, T8142's USB words). */
		for (i = 0; i < ARRAY_SIZE(named); i++)
			if (named[i])
				apple_sleep_test_in(test, c, "named", all, named[i]);
		/* An untested SoC only reads its debug switch, outside the lists. */
		if (!soc->untested)
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

/* Every matched SoC brings its own operations for each SoC-specific step. */
static void apple_sleep_test_ops(struct kunit *test)
{
	const struct of_device_id *id;

	for (id = apple_sleep_of_match; id->compatible[0]; id++) {
		const struct apple_sleep_soc *soc = id->data;
		const struct apple_sleep_ops *ops = soc->ops;

		if (soc->unavailable_reason) {
			KUNIT_EXPECT_EQ(test, apple_sleep_profile_validate(soc), -EOPNOTSUPP);
			continue;
		}
		KUNIT_EXPECT_EQ(test, apple_sleep_profile_validate(soc), 0);
		KUNIT_ASSERT_NOT_NULL_MSG(test, ops, "%s has no operations", id->compatible);
		KUNIT_EXPECT_NOT_NULL(test, ops->deep_wfi_arm);
		/* Its own irreversible steps and preparation, or the shared ones around these two */
		if (ops->commit_regs) {
			KUNIT_EXPECT_NOT_NULL(test, ops->prepare_regs);
			KUNIT_EXPECT_NOT_NULL(test, ops->undo_regs);
			KUNIT_EXPECT_NOT_NULL(test, ops->display_off);
		} else {
			KUNIT_EXPECT_NOT_NULL(test, ops->lower_io);
			KUNIT_EXPECT_NOT_NULL(test, ops->last_mile);
		}
		KUNIT_EXPECT_NOT_NULL(test, ops->resume);
		KUNIT_EXPECT_NOT_NULL(test, ops->wake);
		KUNIT_EXPECT_NOT_NULL(test, ops->aic_mask_all);
		KUNIT_EXPECT_NOT_NULL(test, ops->aic_ack);
		KUNIT_EXPECT_NOT_NULL(test, ops->aic_restore);
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

		/* A profile without a "wake" block (T6040 arms its own) has no last word */
		if (soc->regions || !soc->wake_words)
			continue;
		KUNIT_EXPECT_NE(test, soc->wake_last_word, 0);
		for (i = 0; i < soc->nwake_names; i++) {
			u32 n = soc->wake_names[i].id;

			KUNIT_EXPECT_LT_MSG(test, n, soc->wake_words * 32, "%s",
					    soc->wake_names[i].name);
			if (n / 32 == soc->wake_words - 1)
				KUNIT_EXPECT_TRUE_MSG(test, soc->wake_last_word & BIT(n % 32), "%s",
						      soc->wake_names[i].name);
			for (j = 0; j < i; j++)
				KUNIT_EXPECT_NE(test, soc->wake_names[j].id, n);
		}
	}
}

/* The SoC's counts fit the driver's arrays, and each list is set. */
static void apple_sleep_test_layout(struct kunit *test)
{
	const struct of_device_id *id;

	for (id = apple_sleep_of_match; id->compatible[0]; id++) {
		const struct apple_sleep_soc *soc = id->data;

		/* Its memory controller and NI words are its operations' own, too. */
		if (apple_sleep_test_own_tables(soc))
			continue;
		KUNIT_EXPECT_GE(test, soc->ncluster_cpu, 1);
		KUNIT_EXPECT_LE(test, soc->ncluster_cpu, SLEEP_CPU_CLUSTERS_MAX);
		KUNIT_EXPECT_GT(test, soc->cpu_start_stride, 0);
		KUNIT_EXPECT_LE(test, soc->nclk_mux_parked, SLEEP_CLK_MUX_MAX);
		KUNIT_EXPECT_LE(test, soc->ncllt, SLEEP_CLLT_MAX);
		/* Every SoC steps its memory controller down (T8142: from the J813 trace). */
		KUNIT_EXPECT_GE(test, soc->nmcc, 1);
		KUNIT_EXPECT_LE(test, soc->nmcc, MCC_CHANNELS);
		KUNIT_EXPECT_LE(test, soc->namcc, SLEEP_AMCC_MAX);
		/* Each SoC NI table is mapped 0x80 bytes long (the T8142 sleep node). */
		KUNIT_EXPECT_LE(test, soc->nsocni, SLEEP_SOCNI_MAX);
		KUNIT_EXPECT_EQ(test, !!soc->socni_words, !!soc->nsocni);
		for (int j = 0; j < soc->nsocni; j++)
			KUNIT_EXPECT_LE(test, soc->socni_words[j] * 4, 0x80);
		KUNIT_EXPECT_GE(test, soc->wake_words, 1);
		KUNIT_EXPECT_LE(test, soc->wake_words, WAKE_WORDS);
		KUNIT_EXPECT_GE(test, soc->wake_status, soc->wake_mask + soc->wake_words * 4);
		KUNIT_EXPECT_LT(test, soc->pmu_usid, 16);
		/* PS write-disable words: in "ps" (0x18000 on T8142), aligned, one bit each */
		KUNIT_EXPECT_LE(test, soc->nps_wr_dis, SLEEP_PS_WR_DIS_MAX);
		for (int i = 0; i < soc->nps_wr_dis; i++) {
			KUNIT_EXPECT_EQ(test, soc->ps_wr_dis[i].offset % 4, 0);
			KUNIT_EXPECT_LT(test, soc->ps_wr_dis[i].offset, 0x18000);
			KUNIT_EXPECT_LT(test, soc->ps_wr_dis[i].bit, 32);
		}
	}
}

static void apple_sleep_test_t6050(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, t6050_sleep_plan_complex_request(0xf, NULL), -EINVAL);
	KUNIT_EXPECT_NOT_NULL(test, t6050_sleep_soc.ops->commit_regs);
	KUNIT_EXPECT_NOT_NULL(test, t6050_sleep_soc.ops->display_off);
	KUNIT_EXPECT_EQ(test, sleep_region_validate(t6050_sleep_soc.regions), 0);
	KUNIT_EXPECT_EQ(test, t6050_sleep_soc.cpu_cluster_affinity, 1U);
	KUNIT_EXPECT_EQ(test, t6050_sleep_soc.cpu_start_stride, 6U);
}

/* The Max extends, rather than replaces or reorders, the common inventory. */
static void apple_sleep_test_j716c_inventory(struct kunit *test)
{
	const struct apple_sleep_words *common = &t6050_region_profile.power;
	const struct apple_sleep_words *max = &t6050_j716c_region_profile.power;
	unsigned int i, group, lane;
	static const u32 amcc_base[] = { 0x418, 0x498, 0x518, 0x598 };

	KUNIT_ASSERT_EQ(test, sleep_region_validate(&t6050_j716c_region_profile), 0);
	KUNIT_ASSERT_EQ(test, max->count, common->count + 22);
	for (i = 0; i < common->count; i++)
		KUNIT_EXPECT_EQ(test, max->offset[i], common->offset[i]);
	for (group = 0; group < ARRAY_SIZE(amcc_base); group++)
		for (lane = 0; lane < 4; lane++) {
			u32 word = SLEEP_REGION_WORD(T6050_PS_MEDIA, amcc_base[group] + 8 * lane);

			KUNIT_EXPECT_TRUE(test, apple_sleep_test_has(max, word));
			KUNIT_EXPECT_FALSE(test, apple_sleep_test_has(common, word));
		}
	KUNIT_EXPECT_TRUE(test, apple_sleep_test_has(max,
			  SLEEP_REGION_WORD(T6050_PS_MEDIA, 0x1f0)));
	KUNIT_EXPECT_FALSE(test, apple_sleep_test_has(common,
			   SLEEP_REGION_WORD(T6050_PS_MEDIA, 0x1f0)));
	for (i = 0; i < 5; i++) {
		u32 word = SLEEP_REGION_WORD(T6050_PS_MEDIA, 0x8028 + 8 * i);

		KUNIT_EXPECT_TRUE(test, apple_sleep_test_has(max, word));
		KUNIT_EXPECT_FALSE(test, apple_sleep_test_has(common, word));
	}
}

/* An inventory must never authorize unqualified hardware entry callbacks. */
static void apple_sleep_test_unavailable(struct kunit *test)
{
	struct apple_sleep_soc incomplete = t6050_j716c_sleep_soc;

	incomplete.unavailable_reason = "not qualified";
	KUNIT_EXPECT_EQ(test, apple_sleep_profile_validate(&incomplete), -EOPNOTSUPP);
	/* Deleting the explanation alone still cannot enable a profile without operations. */
	incomplete.unavailable_reason = NULL;
	incomplete.ops = NULL;
	KUNIT_EXPECT_EQ(test, apple_sleep_profile_validate(&incomplete), -EINVAL);
}

/* The J716c runs the J714s's steps: only its map, inventory, board and PMP differ. */
static void apple_sleep_test_j716c_full(struct kunit *test)
{
	const struct apple_sleep_soc *max = &t6050_j716c_sleep_soc, *pro = &t6050_sleep_soc;
	struct apple_sleep_ops ops = *max->ops;
	struct apple_sleep *state = kunit_kzalloc(test, sizeof(*state), GFP_KERNEL);
	unsigned int i;

	KUNIT_ASSERT_NOT_NULL(test, state);
	KUNIT_EXPECT_EQ(test, apple_sleep_profile_validate(max), 0);
	KUNIT_EXPECT_PTR_EQ(test, max->unavailable_reason, NULL);
	KUNIT_EXPECT_STREQ(test, max->machine, "apple,j716c");
	KUNIT_EXPECT_PTR_EQ(test, max->regions, &t6050_j716c_region_profile);
	KUNIT_EXPECT_EQ(test, sleep_region_validate(max->regions), 0);
	KUNIT_EXPECT_EQ(test, max->regions->ntables, pro->regions->ntables);
	for (i = 0; i < max->regions->ntables; i++)
		KUNIT_EXPECT_MEMEQ(test, &max->regions->table[i], &pro->regions->table[i],
				   sizeof(pro->regions->table[i]));
	KUNIT_EXPECT_TRUE(test, max->pmp_unstarted);
	KUNIT_EXPECT_TRUE(test, max->ops->map_regs == t6050_j716c_sleep_map);
	ops.map_regs = pro->ops->map_regs;
	KUNIT_EXPECT_MEMEQ(test, &ops, pro->ops, sizeof(ops));
	KUNIT_EXPECT_PTR_EQ(test, max->smc_policy, pro->smc_policy);
	KUNIT_EXPECT_PTR_EQ(test, max->gate_coprocs, pro->gate_coprocs);
	KUNIT_EXPECT_EQ(test, max->ngate_coprocs, pro->ngate_coprocs);
	KUNIT_EXPECT_PTR_EQ(test, max->gate_off, pro->gate_off);
	KUNIT_EXPECT_EQ(test, max->ngate_off, pro->ngate_off);
	KUNIT_EXPECT_EQ(test, max->strict_gate_inventory, pro->strict_gate_inventory);
	KUNIT_EXPECT_EQ(test, max->early_context, pro->early_context);
	KUNIT_EXPECT_EQ(test, max->own_wake, pro->own_wake);
	KUNIT_EXPECT_EQ(test, max->spmi_notify_sleep, pro->spmi_notify_sleep);
	KUNIT_EXPECT_EQ(test, max->boot_mpidr, pro->boot_mpidr);
	KUNIT_EXPECT_EQ(test, max->cpu_count, pro->cpu_count);
	KUNIT_EXPECT_EQ(test, max->cpu_cluster_affinity, pro->cpu_cluster_affinity);
	KUNIT_EXPECT_EQ(test, max->cpu_clusters, pro->cpu_clusters);
	KUNIT_EXPECT_EQ(test, max->cpu_cores_per_cluster, pro->cpu_cores_per_cluster);
	KUNIT_EXPECT_EQ(test, max->cpu_start_stride, pro->cpu_start_stride);
	KUNIT_EXPECT_PTR_EQ(test, max->cpu_valid, pro->cpu_valid);
	KUNIT_EXPECT_EQ(test, max->ndisp, pro->ndisp);
	KUNIT_EXPECT_MEMEQ(test, &max->ps, &pro->ps, sizeof(max->ps));
	/* Every word the J714s's steps name by index is the J716c's too, at the same index. */
	for (i = 0; i < pro->regions->power.count; i++)
		KUNIT_EXPECT_EQ(test, max->regions->power.offset[i], pro->regions->power.offset[i]);
	state->soc = max;
	KUNIT_EXPECT_TRUE(test, apple_sleep_t6050_full(state));
	state->soc = pro;
	KUNIT_EXPECT_TRUE(test, apple_sleep_t6050_full(state));
}

/* The J714s's profile is as it was: its own map, its inventory, a PMP that runs. */
static void apple_sleep_test_j714s_unchanged(struct kunit *test)
{
	const struct apple_sleep_soc *pro = &t6050_sleep_soc;

	KUNIT_EXPECT_PTR_EQ(test, pro->unavailable_reason, NULL);
	KUNIT_EXPECT_STREQ(test, pro->machine, "apple,j714s");
	KUNIT_EXPECT_PTR_EQ(test, pro->ops, &t6050_sleep_ops);
	KUNIT_EXPECT_TRUE(test, pro->ops->map_regs == t6050_sleep_map_regs);
	KUNIT_EXPECT_PTR_EQ(test, pro->regions, &t6050_region_profile);
	KUNIT_EXPECT_PTR_EQ(test, pro->regions->power.offset, &t6050_ps_all[0]);
	KUNIT_EXPECT_EQ(test, pro->regions->power.count, ARRAY_SIZE(t6050_ps_all));
	KUNIT_EXPECT_FALSE(test, pro->pmp_unstarted);
	KUNIT_EXPECT_EQ(test, apple_sleep_profile_validate(pro), 0);
}

static void apple_sleep_test_region_rejects_bad_inventory(struct kunit *test)
{
	struct apple_sleep_region_profile p = t6050_j716c_region_profile;
	u32 words[] = { SLEEP_REGION_WORD(T6050_PS_MEDIA, 0x418),
			SLEEP_REGION_WORD(T6050_PS_MEDIA, 0x418) };

	p.power = (struct apple_sleep_words)SLEEP_WORDS(words);
	KUNIT_EXPECT_EQ(test, sleep_region_validate(&p), -EINVAL);
	words[1] = SLEEP_REGION_WORD(T6050_PS_MEDIA, 0xc000);
	KUNIT_EXPECT_EQ(test, sleep_region_validate(&p), -EINVAL);
	words[1] = SLEEP_REGION_WORD(T6050_CLK, 0x418);
	KUNIT_EXPECT_EQ(test, sleep_region_validate(&p), -EINVAL);
}

struct apple_sleep_test_wdt_saved {
	struct device *dev;
	void (*restore)(void *data);
	void *data;
	bool owned;
};

struct apple_sleep_test_wdt_ctx {
	u32 *wdt_regs;
	int calls;
	u32 ctl_at_restore;
	bool owned_at_restore;
};

struct apple_sleep_test_wdt_state {
	struct device dev1;
	struct device dev2;
	u32 fake_wdt[16];
	struct apple_sleep_test_wdt_saved saved;
};

static void apple_sleep_test_wdt_restore_global(void *data)
{
	struct apple_sleep_test_wdt_saved *saved = data;
	unsigned long flags;

	spin_lock_irqsave(&apple_sleep_wdt_lock, flags);
	apple_sleep_wdt_dev = saved->dev;
	apple_sleep_wdt_restore = saved->restore;
	apple_sleep_wdt_data = saved->data;
	apple_sleep_wd2_owned = saved->owned;
	spin_unlock_irqrestore(&apple_sleep_wdt_lock, flags);
}

static void apple_sleep_test_wdt_restore_cb(void *data)
{
	struct apple_sleep_test_wdt_ctx *ctx = data;

	ctx->calls++;
	ctx->ctl_at_restore = ctx->wdt_regs[WDT_WD2_CTL / 4];
	ctx->owned_at_restore = apple_sleep_wd2_owned;
}

static void apple_sleep_test_wdt_handoff(struct kunit *test)
{
	struct resource t6050_sleep_wdt = DEFINE_RES_MEM(0x28836c000ULL, 0x4000);
	struct resource t6050_dev_wdt = DEFINE_RES_MEM(0x28836c000ULL, 0x4000);
	struct resource t8142_sleep_wdt = DEFINE_RES_MEM(0x23d2b0000ULL, 0x30);
	struct resource t8142_dev_wdt = DEFINE_RES_MEM(0x23d2b0000ULL, 0x4000);
	struct resource die1_wdt = DEFINE_RES_MEM(0x228836c000ULL, 0x4000);
	struct apple_sleep_test_wdt_state *st;
	struct apple_sleep_test_wdt_ctx ctx;
	unsigned long flags;

	st = kunit_kzalloc(test, sizeof(*st), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, st);

	/* Isolate global handoff state so a probed apple_wdt on J714S/J716C is untouched. */
	spin_lock_irqsave(&apple_sleep_wdt_lock, flags);
	st->saved.dev = apple_sleep_wdt_dev;
	st->saved.restore = apple_sleep_wdt_restore;
	st->saved.data = apple_sleep_wdt_data;
	st->saved.owned = apple_sleep_wd2_owned;
	apple_sleep_wdt_dev = NULL;
	apple_sleep_wdt_restore = NULL;
	apple_sleep_wdt_data = NULL;
	apple_sleep_wd2_owned = false;
	spin_unlock_irqrestore(&apple_sleep_wdt_lock, flags);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, apple_sleep_test_wdt_restore_global,
						  &st->saved), 0);

	ctx = (struct apple_sleep_test_wdt_ctx){ .wdt_regs = st->fake_wdt };

	/* Block matching compares resource start across differing map sizes. */
	KUNIT_EXPECT_TRUE(test, apple_sleep_wdt_same_block(&t6050_sleep_wdt, &t6050_dev_wdt));
	KUNIT_EXPECT_TRUE(test, apple_sleep_wdt_same_block(&t8142_sleep_wdt, &t8142_dev_wdt));
	KUNIT_EXPECT_FALSE(test, apple_sleep_wdt_same_block(&t6050_sleep_wdt, &die1_wdt));
	KUNIT_EXPECT_FALSE(test, apple_sleep_wdt_same_block(NULL, &t6050_dev_wdt));
	KUNIT_EXPECT_FALSE(test, apple_sleep_wdt_same_block(&t6050_sleep_wdt, NULL));

	/* Invalid arguments and unmatched watchdogs. */
	KUNIT_EXPECT_EQ(test, apple_sleep_wdt_register_matched(NULL, true,
							       apple_sleep_test_wdt_restore_cb,
							       &ctx), -EINVAL);
	KUNIT_EXPECT_EQ(test, apple_sleep_wdt_register_matched(&st->dev1, true, NULL, &ctx),
			-EINVAL);
	KUNIT_EXPECT_EQ(test, apple_sleep_wdt_register_matched(&st->dev2, false,
							       apple_sleep_test_wdt_restore_cb,
							       &ctx), 0);
	KUNIT_EXPECT_FALSE(test, apple_sleep_wdt_active(&st->dev2));

	/* Matching watchdog claims the single slot; a second claiming it gets -EBUSY. */
	KUNIT_EXPECT_EQ(test, apple_sleep_wdt_register_matched(&st->dev1, true,
							       apple_sleep_test_wdt_restore_cb,
							       &ctx), 0);
	KUNIT_EXPECT_EQ(test, apple_sleep_wdt_register_matched(&st->dev2, true,
							       apple_sleep_test_wdt_restore_cb,
							       &ctx), -EBUSY);
	KUNIT_EXPECT_EQ(test, apple_sleep_wdt_register_matched(&st->dev2, false,
							       apple_sleep_test_wdt_restore_cb,
							       &ctx), 0);
	KUNIT_EXPECT_FALSE(test, apple_sleep_wdt_active(&st->dev1));

	/* Ownership applies only to the registered watchdog; .end orders CTL=0, restore, clear. */
	spin_lock_irqsave(&apple_sleep_wdt_lock, flags);
	apple_sleep_wd2_owned = true;
	spin_unlock_irqrestore(&apple_sleep_wdt_lock, flags);
	KUNIT_EXPECT_TRUE(test, apple_sleep_wdt_active(&st->dev1));
	KUNIT_EXPECT_FALSE(test, apple_sleep_wdt_active(&st->dev2));

	st->fake_wdt[WDT_WD2_CTL / 4] = 1;
	apple_sleep_wdt_end((void __iomem __force *)st->fake_wdt);
	KUNIT_EXPECT_EQ(test, ctx.calls, 1);
	KUNIT_EXPECT_EQ(test, ctx.ctl_at_restore, 0u);
	KUNIT_EXPECT_TRUE(test, ctx.owned_at_restore);
	KUNIT_EXPECT_FALSE(test, apple_sleep_wdt_active(&st->dev1));

	/* Unregistering a non-holder leaves the slot intact; unregistering the holder frees it. */
	apple_sleep_wdt_unregister(&st->dev2);
	KUNIT_EXPECT_EQ(test, apple_sleep_wdt_register_matched(&st->dev2, true,
							       apple_sleep_test_wdt_restore_cb,
							       &ctx), -EBUSY);
	apple_sleep_wdt_unregister(&st->dev1);
	KUNIT_EXPECT_EQ(test, apple_sleep_wdt_register_matched(&st->dev2, true,
							       apple_sleep_test_wdt_restore_cb,
							       &ctx), 0);
	apple_sleep_wdt_unregister(&st->dev2);
}

static void apple_sleep_test_t6040_wdt_span(struct kunit *test)
{
	struct resource full = DEFINE_RES_MEM(T6040_WDT_PA, 0x4000);
	struct resource exact = DEFINE_RES_MEM(T6040_WDT_PA, T6040_WDT_SIZE);
	struct resource short_wd2 = DEFINE_RES_MEM(T6040_WDT_PA, WDT_WD2_CTL);
	struct resource wd0_only = DEFINE_RES_MEM(T6040_WDT_PA, 0x10);
	struct resource elsewhere = DEFINE_RES_MEM(0x23d2b0000ULL, 0x4000);

	KUNIT_EXPECT_EQ(test, T6040_WDT_SIZE, (size_t)0x30);
	KUNIT_EXPECT_TRUE(test, t6040_wdt_resource_ok(&full));
	KUNIT_EXPECT_TRUE(test, t6040_wdt_resource_ok(&exact));
	/* At the right address but short of WD2_CTL: rejected. */
	KUNIT_EXPECT_FALSE(test, t6040_wdt_resource_ok(&short_wd2));
	KUNIT_EXPECT_FALSE(test, t6040_wdt_resource_ok(&wd0_only));
	KUNIT_EXPECT_FALSE(test, t6040_wdt_resource_ok(&elsewhere));
}

static struct kunit_case apple_sleep_test_cases[] = {
	KUNIT_CASE(apple_sleep_test_layout),
	KUNIT_CASE(apple_sleep_test_t6050),
	KUNIT_CASE(apple_sleep_test_j716c_inventory),
	KUNIT_CASE(apple_sleep_test_unavailable),
	KUNIT_CASE(apple_sleep_test_j716c_full),
	KUNIT_CASE(apple_sleep_test_j714s_unchanged),
	KUNIT_CASE(apple_sleep_test_region_rejects_bad_inventory),
	KUNIT_CASE(apple_sleep_test_known_profiles),
	KUNIT_CASE(apple_sleep_test_ops),
	KUNIT_CASE(apple_sleep_test_words),
	KUNIT_CASE(apple_sleep_test_books),
	KUNIT_CASE(apple_sleep_test_wake_names),
	KUNIT_CASE(apple_sleep_test_wdt_handoff),
	KUNIT_CASE(apple_sleep_test_t6040_wdt_span),
	{}
};

static struct kunit_suite apple_sleep_test_suite = {
	.name = "apple-sleep-soc-data",
	.test_cases = apple_sleep_test_cases,
};
kunit_test_suite(apple_sleep_test_suite);
