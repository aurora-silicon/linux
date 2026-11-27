// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit tests for the Apple M1 CPU PMU driver's event logic.
 *
 * Included at the end of apple_m1_cpu_pmu.c. The tests build an arm_pmu
 * with each match-table init function and then only call the callbacks
 * that never touch a register: map_event, set_event_filter,
 * get_event_idx, clear_event_idx and map_pmuv3_event, plus the sysfs show
 * helpers. Nothing is registered with perf and no Apple system register
 * is accessed, so the tests run on any arm64 machine.
 */

#include <kunit/test.h>
#include <linux/bitmap.h>

/* A dynamic perf type: anything past the generic PERF_TYPE_* range. */
#define M1PMU_TEST_PMU_TYPE	(PERF_TYPE_MAX + 7)

#define M1PMU_CACHE(type, op, result)					\
	((u64)PERF_COUNT_HW_CACHE_##type |				\
	 ((u64)PERF_COUNT_HW_CACHE_OP_##op << 8) |			\
	 ((u64)PERF_COUNT_HW_CACHE_RESULT_##result << 16))

static armpmu_init_fn m1pmu_test_init_fn(struct kunit *test, const char *compat)
{
	const struct of_device_id *id;

	for (id = m1_pmu_of_device_ids; id->compatible[0]; id++)
		if (!strcmp(id->compatible, compat))
			return id->data;

	KUNIT_FAIL(test, "%s not in the match table", compat);
	return NULL;
}

static struct arm_pmu *m1pmu_test_pmu(struct kunit *test, const char *compat)
{
	struct arm_pmu *pmu;
	armpmu_init_fn init;

	init = m1pmu_test_init_fn(test, compat);
	KUNIT_ASSERT_NOT_NULL(test, init);

	pmu = kunit_kzalloc(test, sizeof(*pmu), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, pmu);
	pmu->pmu.type = M1PMU_TEST_PMU_TYPE;

	KUNIT_ASSERT_EQ(test, init(pmu), 0);
	return pmu;
}

static struct pmu_hw_events *m1pmu_test_cpuc(struct kunit *test)
{
	struct pmu_hw_events *cpuc;

	cpuc = kunit_kzalloc(test, sizeof(*cpuc), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, cpuc);
	return cpuc;
}

static struct perf_event *m1pmu_test_event(struct kunit *test,
					   struct arm_pmu *pmu,
					   u32 type, u64 config)
{
	struct perf_event *event;

	event = kunit_kzalloc(test, sizeof(*event), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, event);
	event->pmu = &pmu->pmu;
	event->attr.type = type;
	event->attr.config = config;
	event->attr.exclude_guest = 1;
	event->hw.idx = -1;
	return event;
}

static int m1pmu_test_map(struct kunit *test, struct arm_pmu *pmu,
			  u32 type, u64 config)
{
	return pmu->map_event(m1pmu_test_event(test, pmu, type, config));
}

static int m1pmu_test_hw(struct kunit *test, struct arm_pmu *pmu, u64 config)
{
	return m1pmu_test_map(test, pmu, PERF_TYPE_HARDWARE, config);
}

static int m1pmu_test_cache(struct kunit *test, struct arm_pmu *pmu,
			    u64 config)
{
	return m1pmu_test_map(test, pmu, PERF_TYPE_HW_CACHE, config);
}

/*
 * The steps of the arm_pmu core's event init and add that reach the
 * driver: map the event, apply the mode filter, then pick a counter.
 * Returns the counter index or a negative errno.
 */
static int m1pmu_test_add(struct kunit *test, struct arm_pmu *pmu,
			  struct pmu_hw_events *cpuc, u32 type, u64 config,
			  struct perf_event **out)
{
	struct perf_event *event = m1pmu_test_event(test, pmu, type, config);
	int mapping, ret;

	mapping = pmu->map_event(event);
	if (mapping < 0)
		return mapping;

	event->hw.config_base = 0;
	ret = pmu->set_event_filter(&event->hw, &event->attr);
	if (ret)
		return ret;
	event->hw.config_base |= mapping;

	ret = pmu->get_event_idx(cpuc, event);
	if (ret >= 0)
		event->hw.idx = ret;
	if (out)
		*out = event;
	return ret;
}

static int m1pmu_test_raw(struct kunit *test, struct arm_pmu *pmu,
			  struct pmu_hw_events *cpuc, u64 config)
{
	return m1pmu_test_add(test, pmu, cpuc, PERF_TYPE_RAW, config, NULL);
}

/* Match table: every compatible, its name, counters and callbacks. */
struct m1pmu_test_variant {
	const char *compat;
	const char *name;
	u64 cntr_mask;
	int (*map_event)(struct perf_event *event);
	int (*get_event_idx)(struct pmu_hw_events *cpuc,
			     struct perf_event *event);
	int (*map_pmuv3_event)(unsigned int eventsel);
	void (*enable)(struct perf_event *event);
	const struct attribute_group *events;
	const struct attribute_group *formats;
	u32 evt_flag;
};

static const struct m1pmu_test_variant m1pmu_test_variants[] = {
	{ "apple,icestorm-pmu", "apple_icestorm_pmu", GENMASK(9, 0),
	  m1_pmu_map_event, m1_pmu_get_event_idx, m1_pmu_map_pmuv3_event,
	  m1_pmu_enable_event, &m1_pmu_events_attr_group,
	  &m1_pmu_format_attr_group, ARMPMU_EVT_47BIT },
	{ "apple,firestorm-pmu", "apple_firestorm_pmu", GENMASK(9, 0),
	  m1_pmu_map_event, m1_pmu_get_event_idx, m1_pmu_map_pmuv3_event,
	  m1_pmu_enable_event, &m1_pmu_events_attr_group,
	  &m1_pmu_format_attr_group, ARMPMU_EVT_47BIT },
	{ "apple,avalanche-pmu", "apple_avalanche_pmu", GENMASK(9, 0),
	  m2_pmu_map_event, m1_pmu_get_event_idx, m1_pmu_map_pmuv3_event,
	  m1_pmu_enable_event, &m1_pmu_events_attr_group,
	  &m1_pmu_format_attr_group, ARMPMU_EVT_63BIT },
	{ "apple,blizzard-pmu", "apple_blizzard_pmu", GENMASK(9, 0),
	  m2_pmu_map_event, m1_pmu_get_event_idx, m1_pmu_map_pmuv3_event,
	  m1_pmu_enable_event, &m1_pmu_events_attr_group,
	  &m1_pmu_format_attr_group, ARMPMU_EVT_63BIT },
	{ "apple,t8140-pmu", "apple_t8140_pmu", GENMASK(7, 0),
	  m4_pmu_map_event, m4_pmu_get_event_idx, m4_pmu_map_pmuv3_event,
	  m4_pmu_enable_event, &m4_pmu_events_attr_group,
	  &m4_pmu_format_attr_group, ARMPMU_EVT_63BIT },
	{ "apple,t6040-pmu", "apple_t6040_pmu", GENMASK(7, 0),
	  m4_pmu_map_event, m4_pmu_get_event_idx, m4_pmu_map_pmuv3_event,
	  m4_pmu_enable_event, &m4_pmu_events_attr_group,
	  &m4_pmu_format_attr_group, ARMPMU_EVT_63BIT },
	{ "apple,t6050-pmu", "apple_t6050_pmu", GENMASK(9, 0),
	  t6050_pmu_map_event, t6050_pmu_get_event_idx,
	  t6050_pmu_map_pmuv3_event, m4_pmu_enable_event,
	  &t6050_pmu_events_attr_group, &m4_pmu_format_attr_group,
	  ARMPMU_EVT_63BIT },
	{ "apple,t8142-pmu", "apple_t8142_pmu", GENMASK(9, 0),
	  t6050_pmu_map_event, t6050_pmu_get_event_idx,
	  t6050_pmu_map_pmuv3_event, m4_pmu_enable_event,
	  &t6050_pmu_events_attr_group, &m4_pmu_format_attr_group,
	  ARMPMU_EVT_63BIT },
};

static void m1pmu_test_variant_desc(const struct m1pmu_test_variant *v,
				    char *desc)
{
	strscpy(desc, v->compat, KUNIT_PARAM_DESC_SIZE);
}

KUNIT_ARRAY_PARAM(m1pmu_test_variant, m1pmu_test_variants,
		  m1pmu_test_variant_desc);

static void m1pmu_test_match_table_complete(struct kunit *test)
{
	const struct of_device_id *id;
	size_t n = 0, i;

	for (id = m1_pmu_of_device_ids; id->compatible[0]; id++) {
		bool found = false;

		KUNIT_EXPECT_NOT_NULL(test, id->data);
		for (i = 0; i < ARRAY_SIZE(m1pmu_test_variants); i++)
			found |= !strcmp(id->compatible,
					 m1pmu_test_variants[i].compat);
		KUNIT_EXPECT_TRUE_MSG(test, found, "%s has no test variant",
				      id->compatible);
		n++;
	}
	KUNIT_EXPECT_EQ(test, n, ARRAY_SIZE(m1pmu_test_variants));
}

/* Each init function wires its own callbacks, counters and sysfs groups. */
static void m1pmu_test_variant_init(struct kunit *test)
{
	const struct m1pmu_test_variant *v = test->param_value;
	struct arm_pmu *pmu = m1pmu_test_pmu(test, v->compat);
	DECLARE_BITMAP(want, ARMPMU_MAX_HWEVENTS);
	struct perf_event *event;

	KUNIT_EXPECT_STREQ(test, pmu->name, v->name);
	KUNIT_EXPECT_PTR_EQ(test, (void *)pmu->map_event, (void *)v->map_event);
	KUNIT_EXPECT_PTR_EQ(test, (void *)pmu->get_event_idx,
			    (void *)v->get_event_idx);
	KUNIT_EXPECT_PTR_EQ(test, (void *)pmu->map_pmuv3_event,
			    (void *)v->map_pmuv3_event);
	KUNIT_EXPECT_PTR_EQ(test, (void *)pmu->enable, (void *)v->enable);
	KUNIT_EXPECT_PTR_EQ(test, (void *)pmu->clear_event_idx,
			    (void *)m1_pmu_clear_event_idx);
	KUNIT_EXPECT_PTR_EQ(test, (void *)pmu->set_event_filter,
			    (void *)m1_pmu_set_event_filter);
	KUNIT_EXPECT_PTR_EQ(test, (void *)pmu->disable,
			    (void *)m1_pmu_disable_event);
	KUNIT_EXPECT_PTR_EQ(test, (void *)pmu->reset, (void *)m1_pmu_reset);
	KUNIT_EXPECT_PTR_EQ(test, (void *)pmu->handle_irq,
			    (void *)m1_pmu_handle_irq);
	KUNIT_EXPECT_PTR_EQ(test, pmu->attr_groups[ARMPMU_ATTR_GROUP_EVENTS],
			    v->events);
	KUNIT_EXPECT_PTR_EQ(test, pmu->attr_groups[ARMPMU_ATTR_GROUP_FORMATS],
			    v->formats);

	bitmap_zero(want, ARMPMU_MAX_HWEVENTS);
	bitmap_from_u64(want, v->cntr_mask);
	KUNIT_EXPECT_TRUE(test, bitmap_equal(pmu->cntr_mask, want,
					     ARMPMU_MAX_HWEVENTS));
	KUNIT_EXPECT_EQ(test, bitmap_weight(pmu->cntr_mask,
					    ARMPMU_MAX_HWEVENTS),
			hweight64(v->cntr_mask));

	/* map_event advertises the overflow bit width. */
	event = m1pmu_test_event(test, pmu, PERF_TYPE_HARDWARE,
				 PERF_COUNT_HW_CPU_CYCLES);
	KUNIT_EXPECT_GE(test, pmu->map_event(event), 0);
	KUNIT_EXPECT_EQ(test, event->hw.flags & (ARMPMU_EVT_47BIT |
						 ARMPMU_EVT_63BIT |
						 ARMPMU_EVT_64BIT),
			v->evt_flag);
}

/* Cycles and instructions always land on fixed counters 0 and 1. */
static void m1pmu_test_variant_fixed_counters(struct kunit *test)
{
	const struct m1pmu_test_variant *v = test->param_value;
	struct arm_pmu *pmu = m1pmu_test_pmu(test, v->compat);
	struct pmu_hw_events *cpuc = m1pmu_test_cpuc(test);

	KUNIT_EXPECT_EQ(test, m1pmu_test_add(test, pmu, cpuc,
					     PERF_TYPE_HARDWARE,
					     PERF_COUNT_HW_CPU_CYCLES, NULL), 0);
	KUNIT_EXPECT_EQ(test, m1pmu_test_add(test, pmu, cpuc,
					     PERF_TYPE_HARDWARE,
					     PERF_COUNT_HW_INSTRUCTIONS, NULL),
			1);
}

/* Filling the PMU with events of the default affinity uses each counter once. */
static void m1pmu_test_variant_fill(struct kunit *test)
{
	const struct m1pmu_test_variant *v = test->param_value;
	struct arm_pmu *pmu = m1pmu_test_pmu(test, v->compat);
	struct pmu_hw_events *cpuc = m1pmu_test_cpuc(test);
	DECLARE_BITMAP(used, ARMPMU_MAX_HWEVENTS);
	unsigned int i, n;
	int idx;

	/* Event 0xf0 has no special affinity on any variant. */
	bitmap_zero(used, ARMPMU_MAX_HWEVENTS);
	for (n = 0; n < ARMPMU_MAX_HWEVENTS; n++) {
		idx = m1pmu_test_raw(test, pmu, cpuc, 0xf0);
		if (idx < 0)
			break;
		KUNIT_EXPECT_GE(test, idx, 2);
		KUNIT_EXPECT_TRUE(test, test_bit(idx, pmu->cntr_mask));
		KUNIT_EXPECT_FALSE(test, test_bit(idx, used));
		__set_bit(idx, used);
	}
	KUNIT_EXPECT_EQ(test, idx, -EAGAIN);

	/* Every counter the PMU exposes, minus the two fixed ones. */
	KUNIT_EXPECT_EQ(test, n, hweight64(v->cntr_mask & ~GENMASK_ULL(1, 0)));
	for (i = 0; i < n; i++)
		KUNIT_EXPECT_TRUE(test, test_bit(i + 2, used));
}

/* Generic hardware events, per variant event numbering. */
static void m1pmu_test_hw_map_m1(struct kunit *test)
{
	struct arm_pmu *pmu = m1pmu_test_pmu(test, "apple,firestorm-pmu");
	struct arm_pmu *m2 = m1pmu_test_pmu(test, "apple,blizzard-pmu");
	struct arm_pmu *p[] = { pmu, m2 };
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(p); i++) {
		KUNIT_EXPECT_EQ(test, m1pmu_test_hw(test, p[i],
						    PERF_COUNT_HW_CPU_CYCLES),
				0x02);
		KUNIT_EXPECT_EQ(test, m1pmu_test_hw(test, p[i],
						    PERF_COUNT_HW_INSTRUCTIONS),
				0x8c);
		KUNIT_EXPECT_EQ(test,
				m1pmu_test_hw(test, p[i],
					      PERF_COUNT_HW_BRANCH_INSTRUCTIONS),
				0x8d);
		KUNIT_EXPECT_EQ(test, m1pmu_test_hw(test, p[i],
						    PERF_COUNT_HW_BRANCH_MISSES),
				0xcb);
	}
}

static void m1pmu_test_hw_map_m4(struct kunit *test)
{
	struct arm_pmu *pmu = m1pmu_test_pmu(test, "apple,t6040-pmu");

	KUNIT_EXPECT_EQ(test, m1pmu_test_map(test, pmu, PERF_TYPE_HARDWARE,
					     PERF_COUNT_HW_CPU_CYCLES), 0x02);
	KUNIT_EXPECT_EQ(test, m1pmu_test_map(test, pmu, PERF_TYPE_HARDWARE,
					     PERF_COUNT_HW_INSTRUCTIONS), 0x08);
	KUNIT_EXPECT_EQ(test, m1pmu_test_map(test, pmu, PERF_TYPE_HARDWARE,
					     PERF_COUNT_HW_BRANCH_INSTRUCTIONS),
			0x21);
	KUNIT_EXPECT_EQ(test, m1pmu_test_map(test, pmu, PERF_TYPE_HARDWARE,
					     PERF_COUNT_HW_BRANCH_MISSES), 0x22);
}

static void m1pmu_test_hw_map_t6050(struct kunit *test)
{
	struct arm_pmu *pmu = m1pmu_test_pmu(test, "apple,t6050-pmu");

	KUNIT_EXPECT_EQ(test, m1pmu_test_map(test, pmu, PERF_TYPE_HARDWARE,
					     PERF_COUNT_HW_CPU_CYCLES), 0x011);
	KUNIT_EXPECT_EQ(test, m1pmu_test_map(test, pmu, PERF_TYPE_HARDWARE,
					     PERF_COUNT_HW_INSTRUCTIONS), 0x008);
	KUNIT_EXPECT_EQ(test, m1pmu_test_map(test, pmu, PERF_TYPE_HARDWARE,
					     PERF_COUNT_HW_BRANCH_INSTRUCTIONS),
			0x021);
	KUNIT_EXPECT_EQ(test, m1pmu_test_map(test, pmu, PERF_TYPE_HARDWARE,
					     PERF_COUNT_HW_BRANCH_MISSES), 0x022);
}

/* Generic events with no mapping, and out-of-range configs. */
static void m1pmu_test_hw_map_unsupported(struct kunit *test)
{
	static const u64 unmapped[] = {
		PERF_COUNT_HW_CACHE_REFERENCES,
		PERF_COUNT_HW_CACHE_MISSES,
		PERF_COUNT_HW_BUS_CYCLES,
		PERF_COUNT_HW_STALLED_CYCLES_FRONTEND,
		PERF_COUNT_HW_STALLED_CYCLES_BACKEND,
		PERF_COUNT_HW_REF_CPU_CYCLES,
	};
	unsigned int i, j;

	for (i = 0; i < ARRAY_SIZE(m1pmu_test_variants); i++) {
		struct arm_pmu *pmu =
			m1pmu_test_pmu(test, m1pmu_test_variants[i].compat);

		for (j = 0; j < ARRAY_SIZE(unmapped); j++)
			KUNIT_EXPECT_EQ(test,
					m1pmu_test_hw(test, pmu, unmapped[j]),
					-ENOENT);
		KUNIT_EXPECT_EQ(test, m1pmu_test_hw(test, pmu, PERF_COUNT_HW_MAX),
				-EINVAL);
		/* A type that is neither generic nor this PMU's. */
		KUNIT_EXPECT_EQ(test,
				m1pmu_test_map(test, pmu, PERF_TYPE_TRACEPOINT, 0),
				-ENOENT);
	}
}

/*
 * Generic cache events. Only the L1D read miss may ever be mapped (it is
 * the one event with a known counter on every core), so leave it out.
 */
static void m1pmu_test_cache_map(struct kunit *test)
{
	static const u64 unmapped[] = {
		M1PMU_CACHE(L1D, READ, ACCESS),
		M1PMU_CACHE(L1D, WRITE, MISS),
		M1PMU_CACHE(L1I, READ, MISS),
		M1PMU_CACHE(LL, READ, MISS),
		M1PMU_CACHE(DTLB, READ, MISS),
		M1PMU_CACHE(ITLB, READ, MISS),
		M1PMU_CACHE(BPU, READ, ACCESS),
		M1PMU_CACHE(NODE, PREFETCH, MISS),
	};
	unsigned int i, j;

	for (i = 0; i < ARRAY_SIZE(m1pmu_test_variants); i++) {
		struct arm_pmu *pmu =
			m1pmu_test_pmu(test, m1pmu_test_variants[i].compat);

		for (j = 0; j < ARRAY_SIZE(unmapped); j++)
			KUNIT_EXPECT_EQ(test,
					m1pmu_test_cache(test, pmu, unmapped[j]),
					-ENOENT);
		KUNIT_EXPECT_EQ(test,
				m1pmu_test_cache(test, pmu,
						 PERF_COUNT_HW_CACHE_MAX),
				-EINVAL);
		KUNIT_EXPECT_EQ(test,
				m1pmu_test_cache(test, pmu,
						 (u64)PERF_COUNT_HW_CACHE_OP_MAX << 8),
				-EINVAL);
		KUNIT_EXPECT_EQ(test,
				m1pmu_test_cache(test, pmu,
						 (u64)PERF_COUNT_HW_CACHE_RESULT_MAX << 16),
				-EINVAL);
	}
}

/* Raw events: 8-bit selectors on M1/M2, 16-bit from M4 on. */
static void m1pmu_test_raw_mask(struct kunit *test)
{
	struct arm_pmu *m1 = m1pmu_test_pmu(test, "apple,icestorm-pmu");
	struct arm_pmu *m2 = m1pmu_test_pmu(test, "apple,avalanche-pmu");
	struct arm_pmu *m4 = m1pmu_test_pmu(test, "apple,t8140-pmu");
	struct arm_pmu *m5 = m1pmu_test_pmu(test, "apple,t8142-pmu");

	KUNIT_EXPECT_EQ(test, m1pmu_test_map(test, m1, PERF_TYPE_RAW, 0x1a3),
			0xa3);
	KUNIT_EXPECT_EQ(test, m1pmu_test_map(test, m2, PERF_TYPE_RAW,
					     0xffff00cb), 0xcb);
	KUNIT_EXPECT_EQ(test, m1pmu_test_map(test, m1, M1PMU_TEST_PMU_TYPE,
					     0x38c), 0x8c);
	KUNIT_EXPECT_EQ(test, m1pmu_test_map(test, m4, PERF_TYPE_RAW,
					     0x30008), 0x0008);
	KUNIT_EXPECT_EQ(test, m1pmu_test_map(test, m4, M1PMU_TEST_PMU_TYPE,
					     0xabcd), 0xabcd);
	KUNIT_EXPECT_EQ(test, m1pmu_test_map(test, m5, PERF_TYPE_RAW,
					     0x105a3), 0x5a3);
	KUNIT_EXPECT_EQ(test, m1pmu_test_map(test, m5, M1PMU_TEST_PMU_TYPE,
					     0x895), 0x895);
}

/* exclude_* bits become the count-user/kernel/host/guest config bits. */
static void m1pmu_test_event_filter(struct kunit *test)
{
	struct arm_pmu *pmu = m1pmu_test_pmu(test, "apple,firestorm-pmu");
	struct perf_event_attr attr = { };
	struct hw_perf_event hw = { };

	attr.exclude_guest = 1;
	KUNIT_ASSERT_EQ(test, pmu->set_event_filter(&hw, &attr), 0);
	KUNIT_EXPECT_EQ(test, hw.config_base,
			(unsigned long)(M1_PMU_CFG_COUNT_KERNEL |
					M1_PMU_CFG_COUNT_USER |
					M1_PMU_CFG_COUNT_HOST));

	attr.exclude_kernel = 1;
	KUNIT_ASSERT_EQ(test, pmu->set_event_filter(&hw, &attr), 0);
	KUNIT_EXPECT_EQ(test, hw.config_base,
			(unsigned long)(M1_PMU_CFG_COUNT_USER |
					M1_PMU_CFG_COUNT_HOST));

	attr.exclude_kernel = 0;
	attr.exclude_user = 1;
	KUNIT_ASSERT_EQ(test, pmu->set_event_filter(&hw, &attr), 0);
	KUNIT_EXPECT_EQ(test, hw.config_base,
			(unsigned long)(M1_PMU_CFG_COUNT_KERNEL |
					M1_PMU_CFG_COUNT_HOST));

	attr.exclude_host = 1;
	attr.exclude_kernel = 1;
	KUNIT_ASSERT_EQ(test, pmu->set_event_filter(&hw, &attr), 0);
	KUNIT_EXPECT_EQ(test, hw.config_base, 0UL);

	/* Counting the guest needs VHE; without it, refuse. */
	memset(&attr, 0, sizeof(attr));
	hw.config_base = 0x5a;
	if (is_kernel_in_hyp_mode()) {
		KUNIT_ASSERT_EQ(test, pmu->set_event_filter(&hw, &attr), 0);
		KUNIT_EXPECT_EQ(test, hw.config_base,
				(unsigned long)(M1_PMU_CFG_COUNT_KERNEL |
						M1_PMU_CFG_COUNT_USER |
						M1_PMU_CFG_COUNT_HOST |
						M1_PMU_CFG_COUNT_GUEST));
	} else {
		KUNIT_EXPECT_EQ(test, pmu->set_event_filter(&hw, &attr),
				-EOPNOTSUPP);
		KUNIT_EXPECT_EQ(test, hw.config_base, 0x5aUL);
	}
}

/* The filter bits must not change which counter an event gets. */
static void m1pmu_test_idx_ignores_filter_bits(struct kunit *test)
{
	struct arm_pmu *pmu = m1pmu_test_pmu(test, "apple,icestorm-pmu");
	struct pmu_hw_events *cpuc = m1pmu_test_cpuc(test);
	struct perf_event *event;

	event = m1pmu_test_event(test, pmu, PERF_TYPE_RAW, 0);
	event->hw.config_base = M1_PMU_CFG_COUNT_USER |
				M1_PMU_CFG_COUNT_KERNEL |
				M1_PMU_CFG_COUNT_HOST |
				M1_PMU_CFG_COUNT_GUEST |
				M1_PMU_PERFCTR_RETIRE_UOP;
	KUNIT_EXPECT_EQ(test, pmu->get_event_idx(cpuc, event), 7);

	pmu = m1pmu_test_pmu(test, "apple,t6050-pmu");
	cpuc = m1pmu_test_cpuc(test);
	event->pmu = &pmu->pmu;
	event->hw.config_base = M1_PMU_CFG_COUNT_USER |
				M1_PMU_CFG_COUNT_HOST | 0x897;
	KUNIT_EXPECT_EQ(test, pmu->get_event_idx(cpuc, event), 7);
}

/* M1/M2 per-event affinities from m1_pmu_event_affinity. */
static void m1pmu_test_m1_affinity(struct kunit *test)
{
	struct arm_pmu *pmu = m1pmu_test_pmu(test, "apple,firestorm-pmu");
	struct pmu_hw_events *cpuc = m1pmu_test_cpuc(test);

	/* RETIRE_UOP only counts on counter 7. */
	KUNIT_EXPECT_EQ(test, m1pmu_test_raw(test, pmu, cpuc, 0x01), 7);
	KUNIT_EXPECT_EQ(test, m1pmu_test_raw(test, pmu, cpuc, 0x01), -EAGAIN);

	/* INST_ALL: counter 1, then 7, which is now taken. */
	KUNIT_EXPECT_EQ(test, m1pmu_test_raw(test, pmu, cpuc, 0x8c), 1);
	KUNIT_EXPECT_EQ(test, m1pmu_test_raw(test, pmu, cpuc, 0x8c), -EAGAIN);

	/* Branch events are confined to 5-7; only 5 and 6 are left. */
	KUNIT_EXPECT_EQ(test, m1pmu_test_raw(test, pmu, cpuc, 0x8d), 5);
	KUNIT_EXPECT_EQ(test, m1pmu_test_raw(test, pmu, cpuc, 0xcb), 6);
	KUNIT_EXPECT_EQ(test, m1pmu_test_raw(test, pmu, cpuc, 0xc5), -EAGAIN);

	/* 0xf5-0xf7 and 0xfd count on 2, 4, 6. */
	KUNIT_EXPECT_EQ(test, m1pmu_test_raw(test, pmu, cpuc, 0xf5), 2);
	KUNIT_EXPECT_EQ(test, m1pmu_test_raw(test, pmu, cpuc, 0xfd), 4);
	KUNIT_EXPECT_EQ(test, m1pmu_test_raw(test, pmu, cpuc, 0xf6), -EAGAIN);

	/* 0xf8 counts on 2-7: only 3 is free there. */
	KUNIT_EXPECT_EQ(test, m1pmu_test_raw(test, pmu, cpuc, 0xf8), 3);
	KUNIT_EXPECT_EQ(test, m1pmu_test_raw(test, pmu, cpuc, 0xf8), -EAGAIN);

	/* Anything else goes on 8 and 9, never on 0. */
	KUNIT_EXPECT_EQ(test, m1pmu_test_raw(test, pmu, cpuc, 0xa3), 8);
	KUNIT_EXPECT_EQ(test, m1pmu_test_raw(test, pmu, cpuc, 0x00), 9);
	KUNIT_EXPECT_EQ(test, m1pmu_test_raw(test, pmu, cpuc, 0xa3), -EAGAIN);

	/* Cycles still has its fixed counter, then nowhere to go. */
	KUNIT_EXPECT_EQ(test, m1pmu_test_raw(test, pmu, cpuc, 0x02), 0);
	KUNIT_EXPECT_EQ(test, m1pmu_test_raw(test, pmu, cpuc, 0x02), -EAGAIN);
	KUNIT_EXPECT_EQ(test, bitmap_weight(cpuc->used_mask,
					    ARMPMU_MAX_HWEVENTS), 10);
}

/* Only cycles and instructions may use counters 0 and 1 on M1. */
static void m1pmu_test_m1_fixed_reserved(struct kunit *test)
{
	struct arm_pmu *pmu = m1pmu_test_pmu(test, "apple,blizzard-pmu");
	unsigned int evt;

	for (evt = 0; evt <= M1_PMU_PERFCTR_LAST; evt++) {
		struct pmu_hw_events *cpuc = m1pmu_test_cpuc(test);
		int first = m1pmu_test_raw(test, pmu, cpuc, evt);

		KUNIT_EXPECT_GE(test, first, 0);
		if (evt == M1_PMU_PERFCTR_CORE_ACTIVE_CYCLE)
			KUNIT_EXPECT_EQ(test, first, 0);
		else if (evt == M1_PMU_PERFCTR_INST_ALL)
			KUNIT_EXPECT_EQ(test, first, 1);
		else
			KUNIT_EXPECT_GE_MSG(test, first, 2, "event 0x%x", evt);
		kunit_kfree(test, cpuc);
	}
}

/* A freed counter is handed out again; others stay taken. */
static void m1pmu_test_clear_idx(struct kunit *test)
{
	struct arm_pmu *pmu = m1pmu_test_pmu(test, "apple,icestorm-pmu");
	struct pmu_hw_events *cpuc = m1pmu_test_cpuc(test);
	struct perf_event *a, *b, *c;

	KUNIT_ASSERT_EQ(test, m1pmu_test_add(test, pmu, cpuc, PERF_TYPE_RAW,
					     0xa3, &a), 2);
	KUNIT_ASSERT_EQ(test, m1pmu_test_add(test, pmu, cpuc, PERF_TYPE_RAW,
					     0xa3, &b), 3);
	KUNIT_ASSERT_EQ(test, m1pmu_test_add(test, pmu, cpuc, PERF_TYPE_RAW,
					     0x01, &c), 7);

	pmu->clear_event_idx(cpuc, a);
	KUNIT_EXPECT_FALSE(test, test_bit(2, cpuc->used_mask));
	KUNIT_EXPECT_TRUE(test, test_bit(3, cpuc->used_mask));
	KUNIT_EXPECT_EQ(test, m1pmu_test_raw(test, pmu, cpuc, 0xa3), 2);
	KUNIT_EXPECT_EQ(test, m1pmu_test_raw(test, pmu, cpuc, 0x01), -EAGAIN);

	pmu->clear_event_idx(cpuc, c);
	KUNIT_EXPECT_EQ(test, m1pmu_test_raw(test, pmu, cpuc, 0x01), 7);

	pmu->clear_event_idx(cpuc, b);
	KUNIT_EXPECT_EQ(test, m1pmu_test_raw(test, pmu, cpuc, 0x8d), 5);
	KUNIT_EXPECT_EQ(test, m1pmu_test_raw(test, pmu, cpuc, 0xf0), 3);
}

/* M4: cycles only on 0, instructions on 1 or 2-7, the rest on 2-7. */
static void m1pmu_test_m4_affinity(struct kunit *test)
{
	struct arm_pmu *pmu = m1pmu_test_pmu(test, "apple,t8140-pmu");
	struct pmu_hw_events *cpuc = m1pmu_test_cpuc(test);
	unsigned int i;

	KUNIT_EXPECT_EQ(test, m1pmu_test_raw(test, pmu, cpuc, 0x02), 0);
	KUNIT_EXPECT_EQ(test, m1pmu_test_raw(test, pmu, cpuc, 0x02), -EAGAIN);
	KUNIT_EXPECT_EQ(test, m1pmu_test_raw(test, pmu, cpuc, 0x08), 1);
	KUNIT_EXPECT_EQ(test, m1pmu_test_raw(test, pmu, cpuc, 0x08), 2);

	/* 16-bit selectors: 0x1008 is not INST_ALL. */
	KUNIT_EXPECT_EQ(test, m1pmu_test_raw(test, pmu, cpuc, 0x1008), 3);
	/* The M1 RETIRE_UOP pin to counter 7 does not apply here. */
	KUNIT_EXPECT_EQ(test, m1pmu_test_raw(test, pmu, cpuc, 0x01), 4);

	for (i = 5; i <= 7; i++)
		KUNIT_EXPECT_EQ(test, m1pmu_test_raw(test, pmu, cpuc, 0x03), i);
	/* Counters 8 and 9 are never handed out. */
	KUNIT_EXPECT_EQ(test, m1pmu_test_raw(test, pmu, cpuc, 0x03), -EAGAIN);
	KUNIT_EXPECT_EQ(test, m1pmu_test_raw(test, pmu, cpuc, 0x08), -EAGAIN);
	KUNIT_EXPECT_FALSE(test, test_bit(8, cpuc->used_mask));
	KUNIT_EXPECT_FALSE(test, test_bit(9, cpuc->used_mask));
}

/* T6050: cycles on 0 or 2-9, the instruction classes on 2-7 or only 7. */
static void m1pmu_test_t6050_affinity(struct kunit *test)
{
	struct arm_pmu *pmu = m1pmu_test_pmu(test, "apple,t6050-pmu");
	struct pmu_hw_events *cpuc = m1pmu_test_cpuc(test);
	unsigned int i, ev;

	KUNIT_EXPECT_EQ(test, m1pmu_test_raw(test, pmu, cpuc, 0x011), 0);
	KUNIT_EXPECT_EQ(test, m1pmu_test_raw(test, pmu, cpuc, 0x011), 2);
	KUNIT_EXPECT_EQ(test, m1pmu_test_raw(test, pmu, cpuc, 0x008), 1);
	KUNIT_EXPECT_EQ(test, m1pmu_test_raw(test, pmu, cpuc, 0x008), 3);

	/* The counter-7-only list. */
	for (i = 0; i < ARRAY_SIZE(t6050_pmu_pmc7_events); i++) {
		ev = t6050_pmu_pmc7_events[i];
		KUNIT_EXPECT_EQ(test, t6050_pmu_event_affinity(ev), BIT(7));
	}
	KUNIT_EXPECT_EQ(test, m1pmu_test_raw(test, pmu, cpuc, 0x8a4), 7);
	KUNIT_EXPECT_EQ(test, m1pmu_test_raw(test, pmu, cpuc, 0x897), -EAGAIN);

	/* The 2-7 list: 4, 5, 6 left, then full even with 8 and 9 free. */
	for (i = 0; i < ARRAY_SIZE(t6050_pmu_pmc2_7_events); i++) {
		ev = t6050_pmu_pmc2_7_events[i];
		KUNIT_EXPECT_EQ(test, t6050_pmu_event_affinity(ev),
				GENMASK(7, 2) |
				(ev == T6050_PMU_PERFCTR_INST_ALL ? BIT(1) : 0));
	}
	KUNIT_EXPECT_EQ(test, m1pmu_test_raw(test, pmu, cpuc, 0x895), 4);
	KUNIT_EXPECT_EQ(test, m1pmu_test_raw(test, pmu, cpuc, 0x022), 5);
	KUNIT_EXPECT_EQ(test, m1pmu_test_raw(test, pmu, cpuc, 0x8ca), 6);
	KUNIT_EXPECT_EQ(test, m1pmu_test_raw(test, pmu, cpuc, 0x88e), -EAGAIN);

	/* Unlisted events, such as the L1D load miss, may use 8 and 9. */
	KUNIT_EXPECT_EQ(test, m1pmu_test_raw(test, pmu, cpuc, 0x5a3), 8);
	KUNIT_EXPECT_EQ(test, m1pmu_test_raw(test, pmu, cpuc, 0x011), 9);
	KUNIT_EXPECT_EQ(test, m1pmu_test_raw(test, pmu, cpuc, 0x5a3), -EAGAIN);
	KUNIT_EXPECT_EQ(test, bitmap_weight(cpuc->used_mask,
					    ARMPMU_MAX_HWEVENTS), 10);
}

/* The 2-7 and 7-only lists must not overlap, or the order would matter. */
static bool m1pmu_test_in_pmc2_7(unsigned int event)
{
	return t6050_pmu_event_in(t6050_pmu_pmc2_7_events,
				  ARRAY_SIZE(t6050_pmu_pmc2_7_events), event);
}

static void m1pmu_test_t6050_lists_disjoint(struct kunit *test)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(t6050_pmu_pmc7_events); i++)
		KUNIT_EXPECT_FALSE(test,
				   m1pmu_test_in_pmc2_7(t6050_pmu_pmc7_events[i]));
	KUNIT_EXPECT_FALSE(test, m1pmu_test_in_pmc2_7(0x011));
	KUNIT_EXPECT_EQ(test, t6050_pmu_event_affinity(0x5a3), GENMASK(9, 2));
	KUNIT_EXPECT_EQ(test, t6050_pmu_event_affinity(0x8c8), GENMASK(7, 2));
	KUNIT_EXPECT_EQ(test, t6050_pmu_event_affinity(0x8c9), GENMASK(9, 2));
}

/* Every hardware event a variant maps can be placed on an empty PMU. */
static void m1pmu_test_hw_events_placeable(struct kunit *test)
{
	unsigned int i, cfg;

	for (i = 0; i < ARRAY_SIZE(m1pmu_test_variants); i++) {
		struct arm_pmu *pmu =
			m1pmu_test_pmu(test, m1pmu_test_variants[i].compat);

		for (cfg = 0; cfg < PERF_COUNT_HW_MAX; cfg++) {
			struct pmu_hw_events *cpuc = m1pmu_test_cpuc(test);
			int idx;

			if (m1pmu_test_map(test, pmu, PERF_TYPE_HARDWARE,
					   cfg) < 0)
				continue;
			idx = m1pmu_test_add(test, pmu, cpuc,
					     PERF_TYPE_HARDWARE, cfg, NULL);
			KUNIT_EXPECT_GE_MSG(test, idx, 0, "%s hw %u",
					    m1pmu_test_variants[i].compat, cfg);
			if (idx >= 0)
				KUNIT_EXPECT_TRUE(test,
						  test_bit(idx, pmu->cntr_mask));
			kunit_kfree(test, cpuc);
		}
	}
}

/* PMUv3 events KVM exposes to guests, and the PMCEID bitmap built from them. */
static void m1pmu_test_pmuv3_map(struct kunit *test)
{
	struct arm_pmu *m1 = m1pmu_test_pmu(test, "apple,firestorm-pmu");
	struct arm_pmu *m4 = m1pmu_test_pmu(test, "apple,t6040-pmu");
	struct arm_pmu *m5 = m1pmu_test_pmu(test, "apple,t6050-pmu");

	KUNIT_EXPECT_EQ(test, m1->map_pmuv3_event(ARMV8_PMUV3_PERFCTR_CPU_CYCLES),
			0x02);
	KUNIT_EXPECT_EQ(test, m1->map_pmuv3_event(ARMV8_PMUV3_PERFCTR_INST_RETIRED),
			0x8c);
	KUNIT_EXPECT_EQ(test, m1->map_pmuv3_event(ARMV8_PMUV3_PERFCTR_BR_RETIRED),
			0x8d);
	KUNIT_EXPECT_EQ(test,
			m1->map_pmuv3_event(ARMV8_PMUV3_PERFCTR_BR_MIS_PRED_RETIRED),
			0xcb);
	KUNIT_EXPECT_EQ(test, m1->map_pmuv3_event(ARMV8_PMUV3_PERFCTR_L1D_CACHE),
			-EOPNOTSUPP);

	KUNIT_EXPECT_EQ(test, m4->map_pmuv3_event(ARMV8_PMUV3_PERFCTR_CPU_CYCLES),
			0x02);
	KUNIT_EXPECT_EQ(test, m4->map_pmuv3_event(ARMV8_PMUV3_PERFCTR_INST_RETIRED),
			0x08);
	KUNIT_EXPECT_EQ(test, m4->map_pmuv3_event(ARMV8_PMUV3_PERFCTR_BR_RETIRED),
			0x21);

	KUNIT_EXPECT_EQ(test, m5->map_pmuv3_event(ARMV8_PMUV3_PERFCTR_CPU_CYCLES),
			0x011);
	KUNIT_EXPECT_EQ(test, m5->map_pmuv3_event(ARMV8_PMUV3_PERFCTR_INST_RETIRED),
			0x008);
	KUNIT_EXPECT_EQ(test,
			m5->map_pmuv3_event(ARMV8_PMUV3_PERFCTR_BR_MIS_PRED_RETIRED),
			0x022);

	/* Past the common event space. */
	KUNIT_EXPECT_EQ(test, m1->map_pmuv3_event(ARMV8_PMUV3_MAX_COMMON_EVENTS),
			-EOPNOTSUPP);
	KUNIT_EXPECT_EQ(test, m5->map_pmuv3_event(0xffff), -EOPNOTSUPP);
	KUNIT_EXPECT_EQ(test, m4->map_pmuv3_event(UINT_MAX), -EOPNOTSUPP);

	/* PMCEID advertises exactly the mapped events. */
	KUNIT_EXPECT_EQ(test, bitmap_weight(m1->pmceid_bitmap,
					    ARMV8_PMUV3_MAX_COMMON_EVENTS), 4);
	KUNIT_EXPECT_TRUE(test, test_bit(ARMV8_PMUV3_PERFCTR_CPU_CYCLES,
					 m1->pmceid_bitmap));
	KUNIT_EXPECT_TRUE(test, test_bit(ARMV8_PMUV3_PERFCTR_BR_MIS_PRED_RETIRED,
					 m1->pmceid_bitmap));
	KUNIT_EXPECT_EQ(test, bitmap_weight(m4->pmceid_bitmap,
					    ARMV8_PMUV3_MAX_COMMON_EVENTS), 3);
	KUNIT_EXPECT_FALSE(test, test_bit(ARMV8_PMUV3_PERFCTR_BR_MIS_PRED_RETIRED,
					  m4->pmceid_bitmap));
	KUNIT_EXPECT_EQ(test, bitmap_weight(m5->pmceid_bitmap,
					    ARMV8_PMUV3_MAX_COMMON_EVENTS), 4);
	KUNIT_EXPECT_TRUE(test, bitmap_empty(m1->pmceid_ext_bitmap,
					     ARMV8_PMUV3_MAX_COMMON_EVENTS));
}

/* Each PMUv3 event KVM maps can be placed on the PMU that maps it. */
static void m1pmu_test_pmuv3_placeable(struct kunit *test)
{
	unsigned int i, ev;

	for (i = 0; i < ARRAY_SIZE(m1pmu_test_variants); i++) {
		struct arm_pmu *pmu =
			m1pmu_test_pmu(test, m1pmu_test_variants[i].compat);

		for_each_set_bit(ev, pmu->pmceid_bitmap,
				 ARMV8_PMUV3_MAX_COMMON_EVENTS) {
			struct pmu_hw_events *cpuc = m1pmu_test_cpuc(test);
			int raw = pmu->map_pmuv3_event(ev);

			KUNIT_ASSERT_GE(test, raw, 0);
			KUNIT_EXPECT_GE_MSG(test,
					    m1pmu_test_raw(test, pmu, cpuc, raw),
					    0, "%s pmuv3 0x%x",
					    m1pmu_test_variants[i].compat, ev);
			kunit_kfree(test, cpuc);
		}
	}
}

static const char *m1pmu_test_show(struct kunit *test,
				   const struct attribute_group *grp,
				   const char *name)
{
	struct attribute **a;
	char *page;

	page = kunit_kzalloc(test, PAGE_SIZE, GFP_KERNEL);
	if (!page)
		return "";

	for (a = grp->attrs; *a; a++) {
		struct device_attribute *da;

		if (strcmp((*a)->name, name))
			continue;
		da = container_of(*a, struct device_attribute, attr);
		if (da->show(NULL, da, page) <= 0)
			return "";
		return page;
	}
	return "";
}

static void m1pmu_test_expect_show(struct kunit *test,
				   const struct attribute_group *grp,
				   const char *name, const char *want)
{
	KUNIT_EXPECT_STREQ_MSG(test, m1pmu_test_show(test, grp, name), want,
			       "%s/%s", grp->name, name);
}

/* sysfs events and format strings that perf tools parse. */
static void m1pmu_test_sysfs(struct kunit *test)
{
	m1pmu_test_expect_show(test, &m1_pmu_events_attr_group, "cycles",
			       "event=0x0002\n");
	m1pmu_test_expect_show(test, &m1_pmu_events_attr_group,
			       "instructions", "event=0x008c\n");
	m1pmu_test_expect_show(test, &m1_pmu_events_attr_group, "branches",
			       "");
	m1pmu_test_expect_show(test, &m4_pmu_events_attr_group,
			       "l1d-load-misses", "event=0x0003\n");
	m1pmu_test_expect_show(test, &m4_pmu_events_attr_group,
			       "branch-misses", "event=0x0022\n");
	m1pmu_test_expect_show(test, &t6050_pmu_events_attr_group,
			       "l1d-load-misses", "event=0x05a3\n");
	m1pmu_test_expect_show(test, &t6050_pmu_events_attr_group, "loads",
			       "event=0x0895\n");
	m1pmu_test_expect_show(test, &t6050_pmu_events_attr_group, "cycles",
			       "event=0x0011\n");

	m1pmu_test_expect_show(test, &m1_pmu_format_attr_group, "event",
			       "config:0-7\n");
	m1pmu_test_expect_show(test, &m4_pmu_format_attr_group, "event",
			       "config:0-15\n");
}

/* Every named sysfs event maps back to itself as a raw event. */
static void m1pmu_test_sysfs_events_round_trip(struct kunit *test)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(m1pmu_test_variants); i++) {
		const struct m1pmu_test_variant *v = &m1pmu_test_variants[i];
		struct arm_pmu *pmu = m1pmu_test_pmu(test, v->compat);
		struct attribute **a;

		for (a = v->events->attrs; *a; a++) {
			struct perf_pmu_events_attr *pa;
			struct pmu_hw_events *cpuc = m1pmu_test_cpuc(test);

			pa = container_of(*a, struct perf_pmu_events_attr,
					  attr.attr);
			KUNIT_EXPECT_EQ(test,
					m1pmu_test_map(test, pmu,
						       M1PMU_TEST_PMU_TYPE,
						       pa->id),
					(int)pa->id);
			KUNIT_EXPECT_GE_MSG(test,
					    m1pmu_test_raw(test, pmu, cpuc, pa->id),
					    0, "%s %s", v->compat, (*a)->name);
			kunit_kfree(test, cpuc);
		}
	}
}

static struct kunit_case m1pmu_test_cases[] = {
	KUNIT_CASE(m1pmu_test_match_table_complete),
	KUNIT_CASE_PARAM(m1pmu_test_variant_init,
			 m1pmu_test_variant_gen_params),
	KUNIT_CASE_PARAM(m1pmu_test_variant_fixed_counters,
			 m1pmu_test_variant_gen_params),
	KUNIT_CASE_PARAM(m1pmu_test_variant_fill,
			 m1pmu_test_variant_gen_params),
	KUNIT_CASE(m1pmu_test_hw_map_m1),
	KUNIT_CASE(m1pmu_test_hw_map_m4),
	KUNIT_CASE(m1pmu_test_hw_map_t6050),
	KUNIT_CASE(m1pmu_test_hw_map_unsupported),
	KUNIT_CASE(m1pmu_test_cache_map),
	KUNIT_CASE(m1pmu_test_raw_mask),
	KUNIT_CASE(m1pmu_test_event_filter),
	KUNIT_CASE(m1pmu_test_idx_ignores_filter_bits),
	KUNIT_CASE(m1pmu_test_m1_affinity),
	KUNIT_CASE(m1pmu_test_m1_fixed_reserved),
	KUNIT_CASE(m1pmu_test_clear_idx),
	KUNIT_CASE(m1pmu_test_m4_affinity),
	KUNIT_CASE(m1pmu_test_t6050_affinity),
	KUNIT_CASE(m1pmu_test_t6050_lists_disjoint),
	KUNIT_CASE(m1pmu_test_hw_events_placeable),
	KUNIT_CASE(m1pmu_test_pmuv3_map),
	KUNIT_CASE(m1pmu_test_pmuv3_placeable),
	KUNIT_CASE(m1pmu_test_sysfs),
	KUNIT_CASE(m1pmu_test_sysfs_events_round_trip),
	{}
};

static struct kunit_suite m1pmu_test_suite = {
	.name = "apple-m1-cpu-pmu",
	.test_cases = m1pmu_test_cases,
};

kunit_test_suite(m1pmu_test_suite);
