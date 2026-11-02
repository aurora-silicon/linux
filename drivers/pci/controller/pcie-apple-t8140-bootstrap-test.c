// SPDX-License-Identifier: GPL-2.0-only
#include <kunit/test.h>
#include <linux/module.h>

#include "pcie-apple-t8140-bootstrap-state.h"

static void apple_piodma_lifecycle_test(struct kunit *test)
{
	static const enum apple_piodma_event events[] = {
		PIODMA_GUARD, PIODMA_PRIME, PIODMA_PUBLISH,
		PIODMA_COMPLETE, PIODMA_ADMIT,
	};
	int state = PIODMA_IDLE;
	int i;

	for (i = 0; i < ARRAY_SIZE(events); i++) {
		state = apple_piodma_next(state, events[i]);
		KUNIT_ASSERT_GE(test, state, 0);
		KUNIT_EXPECT_TRUE(test, apple_piodma_guarded(state));
		KUNIT_EXPECT_FALSE(test, apple_piodma_sleep_allowed(state, false));
		KUNIT_EXPECT_EQ(test, apple_piodma_sleep_allowed(state, true),
				i == ARRAY_SIZE(events) - 1);
	}
	KUNIT_EXPECT_EQ(test, state, PIODMA_ADMITTED);
	KUNIT_EXPECT_TRUE(test, apple_piodma_ready(state));
	KUNIT_EXPECT_TRUE(test, apple_piodma_retained(state));
}

static void apple_piodma_failure_test(struct kunit *test)
{
	int state;

	/* Guard contention/unwind before PRIME does not consume the attempt. */
	state = apple_piodma_next(PIODMA_GUARDED, PIODMA_RELEASE);
	KUNIT_ASSERT_EQ(test, state, PIODMA_IDLE);
	KUNIT_EXPECT_FALSE(test, apple_piodma_attempted(state));
	KUNIT_EXPECT_EQ(test, apple_piodma_next(state, PIODMA_GUARD), PIODMA_GUARDED);

	/* Preflight failure consumes the attempt, but may release both vetoes. */
	state = apple_piodma_next(PIODMA_ATTEMPTED, PIODMA_FAIL);
	KUNIT_ASSERT_EQ(test, state, PIODMA_FAILED);
	KUNIT_EXPECT_TRUE(test, apple_piodma_guarded(state));
	KUNIT_EXPECT_FALSE(test, apple_piodma_retained(state));
	state = apple_piodma_next(state, PIODMA_RELEASE);
	KUNIT_ASSERT_EQ(test, state, PIODMA_REJECTED);
	KUNIT_EXPECT_FALSE(test, apple_piodma_guarded(state));
	KUNIT_EXPECT_TRUE(test, apple_piodma_attempted(state));
	KUNIT_EXPECT_TRUE(test, apple_piodma_sleep_allowed(state, false));
	KUNIT_EXPECT_LT(test, apple_piodma_next(state, PIODMA_GUARD), 0);

	/* Initialization/transfer failure must keep memory and vetoes owned. */
	state = apple_piodma_next(PIODMA_RETAINED, PIODMA_FAIL);
	KUNIT_ASSERT_EQ(test, state, PIODMA_FAILED_RETAINED);
	KUNIT_EXPECT_TRUE(test, apple_piodma_retained(state));
	KUNIT_EXPECT_TRUE(test, apple_piodma_guarded(state));
	KUNIT_EXPECT_FALSE(test, apple_piodma_ready(state));
	KUNIT_EXPECT_FALSE(test, apple_piodma_sleep_allowed(state, true));
}

static void apple_piodma_transition_guards_test(struct kunit *test)
{
	int state, event, next;

	/* Exhaust all transitions: no retained state can release or retry. */
	for (state = PIODMA_IDLE; state < PIODMA_STATE_COUNT; state++) {
		for (event = PIODMA_GUARD; event < PIODMA_EVENT_COUNT; event++) {
			next = apple_piodma_next(state, event);
			if (apple_piodma_retained(state)) {
				if (next >= 0)
					KUNIT_EXPECT_TRUE(test, apple_piodma_retained(next));
				if (event == PIODMA_RELEASE || event == PIODMA_PRIME ||
				    event == PIODMA_GUARD)
					KUNIT_EXPECT_LT(test, next, 0);
			}
			if (event == PIODMA_ADMIT && state != PIODMA_READY)
				KUNIT_EXPECT_LT(test, next, 0);
			if (event == PIODMA_COMPLETE && state != PIODMA_RETAINED)
				KUNIT_EXPECT_LT(test, next, 0);
		}
	}
}

static void apple_piodma_arena_test(struct kunit *test)
{
	const u64 end = PIODMA_PACKET_MASK, size = 3 * SZ_16K;
	const u64 top = BIT_ULL(36) - size, prefix = PIODMA_POINTER_PREFIX;
	const struct {
		u64 dma, size, prefix, start, end;
		bool forced, valid;
	} cases[] = {
		{ 0, size, prefix, 0, end, true, true },
		{ top, size, prefix, 0, end, true, true },
		{ top + SZ_16K, size, prefix, 0, end, true, false },
		{ 1, size, prefix, 0, end, true, false },
		{ U64_MAX - SZ_16K + 1, size, prefix, 0, end, true, false },
		{ 0, 0, prefix, 0, end, true, false },
		{ 0, size, 0, 0, end, true, false },
		{ 0, size, prefix, 0, end, false, false },
		{ 0, size, prefix, SZ_16K, end, true, false },
		{ 0, size, prefix, 0, end - 1, true, false },
	};
	bool valid;
	int i;

	for (i = 0; i < ARRAY_SIZE(cases); i++) {
		valid = apple_piodma_arena_valid(cases[i].dma, cases[i].size,
						 cases[i].prefix, cases[i].forced,
						 cases[i].start, cases[i].end);
		KUNIT_EXPECT_EQ_MSG(test, valid, cases[i].valid, "case %d", i);
	}
}

static void apple_piodma_irq_test(struct kunit *test)
{
	KUNIT_EXPECT_FALSE(test, apple_piodma_irq_invalid(1, BIT(0), 1, 1));
	KUNIT_EXPECT_FALSE(test, apple_piodma_irq_invalid(15, BIT(6), 1, 1));
	KUNIT_EXPECT_TRUE(test, apple_piodma_irq_matches(BIT(0), 1, 1));
	KUNIT_EXPECT_TRUE(test, apple_piodma_irq_matches(BIT(6), 1, 1));
	KUNIT_EXPECT_TRUE(test, apple_piodma_irq_invalid(16, BIT(0), 1, 1));
	KUNIT_EXPECT_TRUE(test, apple_piodma_irq_invalid(1, BIT(1), 1, 1));
	KUNIT_EXPECT_TRUE(test, apple_piodma_irq_invalid(1, BIT(0), 0, 1));
	KUNIT_EXPECT_TRUE(test, apple_piodma_irq_invalid(1, BIT(0), 2, 1));
	KUNIT_EXPECT_TRUE(test, apple_piodma_irq_invalid(1, BIT(0), 1, 0));
	KUNIT_EXPECT_FALSE(test, apple_piodma_irq_matches(0, 1, 1));
	KUNIT_EXPECT_FALSE(test, apple_piodma_irq_matches(BIT(0), 0, 0));
	/* A late old tag neither faults nor qualifies the active request. */
	KUNIT_EXPECT_FALSE(test, apple_piodma_irq_invalid(1, BIT(0), 1, 2));
	KUNIT_EXPECT_FALSE(test, apple_piodma_irq_matches(BIT(0), 1, 2));
}

static struct kunit_case apple_piodma_cases[] = {
	KUNIT_CASE(apple_piodma_lifecycle_test),
	KUNIT_CASE(apple_piodma_failure_test),
	KUNIT_CASE(apple_piodma_transition_guards_test),
	KUNIT_CASE(apple_piodma_arena_test),
	KUNIT_CASE(apple_piodma_irq_test),
	{ }
};

static struct kunit_suite apple_piodma_suite = {
	.name = "apple-t8140-bootstrap",
	.test_cases = apple_piodma_cases,
};

kunit_test_suite(apple_piodma_suite);

MODULE_DESCRIPTION("Apple T8140 PCIe bootstrap ownership and guard tests");
MODULE_LICENSE("GPL");
