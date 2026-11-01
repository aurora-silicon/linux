// SPDX-License-Identifier: GPL-2.0-only
#include <kunit/test.h>
#include <linux/limits.h>
#include <linux/unaligned.h>
#include <sound/pcm.h>

#include "speaker-energy.h"

static struct apple_energy_gate *new_gate(struct kunit *test)
{
	struct apple_energy_gate *gate;
	u8 slots[] = { 0, 1 };

	gate = kunit_kzalloc(test, sizeof(*gate), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, gate);
	apple_energy_init(gate);
	KUNIT_ASSERT_EQ(test, apple_energy_configure(gate, 48000, 2, 2, slots),
			0);
	return gate;
}

static void grant(struct kunit *test, struct apple_energy_gate *gate, u64 a,
		  u64 b)
{
	struct apple_energy_snapshot snapshot;
	u64 ceiling[] = { a, b };

	apple_energy_snapshot(gate, &snapshot);
	KUNIT_ASSERT_EQ(
		test, apple_energy_grant(gate, &snapshot, ceiling, 1, 100), 0);
}

static void energy_rounding(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, apple_energy_cost(0), 0ULL);
	KUNIT_EXPECT_EQ(test, apple_energy_cost(1), 1ULL);
	KUNIT_EXPECT_EQ(test, apple_energy_cost(-1), 1ULL);
	KUNIT_EXPECT_EQ(test, apple_energy_cost(65536), 1ULL);
	KUNIT_EXPECT_EQ(test, apple_energy_cost(65537), 1ULL);
	KUNIT_EXPECT_EQ(test, apple_energy_cost(1 << 21), 1ULL);
	KUNIT_EXPECT_EQ(test, apple_energy_cost((1 << 21) + 1), 2ULL);
	/* Continuous full scale must not naturally saturate within ten years. */
	KUNIT_EXPECT_GT(test,
			U64_MAX / APPLE_ENERGY_UNIT_RATE /
				APPLE_ENERGY_UNIT_SCALE,
			10ULL * 365 * 24 * 3600);
	KUNIT_EXPECT_EQ(test, apple_energy_cost(S32_MIN),
			(APPLE_ENERGY_UNIT_SCALE * 1 / 1));
	KUNIT_EXPECT_EQ(test, apple_energy_cost(S32_MAX),
			(APPLE_ENERGY_UNIT_SCALE * 1 / 1));
}

static void absent_and_expired(struct kunit *test)
{
	struct apple_energy_gate *gate = new_gate(test);
	s32 samples[] = { S32_MIN, S32_MAX };
	u64 ticket;

	KUNIT_ASSERT_EQ(
		test, apple_energy_filter(gate, samples, 1, 2, 1, &ticket), 0);
	KUNIT_EXPECT_EQ(test, samples[0], 0);
	KUNIT_EXPECT_EQ(test, samples[1], 0);
	KUNIT_ASSERT_EQ(test, apple_energy_complete(gate, ticket), 0);
	grant(test, gate, (APPLE_ENERGY_UNIT_SCALE * 1 / 1),
	      (APPLE_ENERGY_UNIT_SCALE * 1 / 1));
	samples[0] = S32_MIN;
	samples[1] = S32_MAX;
	KUNIT_ASSERT_EQ(test,
			apple_energy_filter(gate, samples, 1, 2, 100, &ticket),
			0);
	KUNIT_EXPECT_EQ(test, samples[0], 0);
	KUNIT_EXPECT_EQ(test, samples[1], 0);
}

static void exhaustion_per_speaker(struct kunit *test)
{
	struct apple_energy_gate *gate = new_gate(test);
	struct apple_energy_snapshot snapshot;
	s32 samples[] = { S32_MIN, S32_MAX, S32_MIN, S32_MAX };
	u64 ticket;

	grant(test, gate, (APPLE_ENERGY_UNIT_SCALE * 1 / 1),
	      (APPLE_ENERGY_UNIT_SCALE * 1 / 4));
	KUNIT_ASSERT_EQ(
		test, apple_energy_filter(gate, samples, 2, 2, 2, &ticket), 0);
	KUNIT_EXPECT_EQ(test, samples[0], S32_MIN);
	KUNIT_EXPECT_EQ(test, samples[1], 1 << 30);
	KUNIT_EXPECT_EQ(test, samples[2], 0);
	KUNIT_EXPECT_EQ(test, samples[3], 0);
	apple_energy_snapshot(gate, &snapshot);
	KUNIT_EXPECT_EQ(test, snapshot.submitted[0],
			(APPLE_ENERGY_UNIT_SCALE * 1 / 1));
	KUNIT_EXPECT_EQ(test, snapshot.submitted[1],
			(APPLE_ENERGY_UNIT_SCALE * 1 / 4));
	KUNIT_EXPECT_EQ(test, snapshot.completed[0], 0ULL);
	KUNIT_ASSERT_EQ(test, apple_energy_complete(gate, ticket), 0);
	apple_energy_snapshot(gate, &snapshot);
	KUNIT_EXPECT_EQ(test, snapshot.completed[0], snapshot.submitted[0]);
}

static void shared_slot(struct kunit *test)
{
	struct apple_energy_gate *gate = new_gate(test);
	struct apple_energy_snapshot snapshot;
	u8 slots[] = { 0, 0 };
	s32 samples[] = { S32_MAX, S32_MAX };
	u64 ticket;

	KUNIT_ASSERT_EQ(test, apple_energy_configure(gate, 48000, 2, 2, slots),
			0);
	grant(test, gate, (APPLE_ENERGY_UNIT_SCALE * 1 / 1),
	      (APPLE_ENERGY_UNIT_SCALE * 1 / 4));
	KUNIT_ASSERT_EQ(
		test, apple_energy_filter(gate, samples, 1, 2, 2, &ticket), 0);
	KUNIT_EXPECT_EQ(test, samples[0], 1 << 30);
	KUNIT_EXPECT_EQ(test, samples[1], 0);
	apple_energy_snapshot(gate, &snapshot);
	KUNIT_EXPECT_EQ(test, snapshot.submitted[0],
			(APPLE_ENERGY_UNIT_SCALE * 1 / 4));
	KUNIT_EXPECT_EQ(test, snapshot.submitted[1],
			(APPLE_ENERGY_UNIT_SCALE * 1 / 4));
}

static void stale_grant_and_replay(struct kunit *test)
{
	struct apple_energy_gate *gate = new_gate(test);
	struct apple_energy_snapshot snapshot;
	u64 ceiling[] = { (APPLE_ENERGY_UNIT_SCALE * 1 / 1),
			  (APPLE_ENERGY_UNIT_SCALE * 1 / 1) };
	s32 samples[] = { 65536, 65536 };
	u64 ticket;

	apple_energy_snapshot(gate, &snapshot);
	KUNIT_ASSERT_EQ(
		test, apple_energy_grant(gate, &snapshot, ceiling, 1, 100), 0);
	KUNIT_EXPECT_EQ(test,
			apple_energy_grant(gate, &snapshot, ceiling, 1, 100),
			-ESTALE);
	KUNIT_ASSERT_EQ(
		test, apple_energy_filter(gate, samples, 1, 2, 2, &ticket), 0);
	KUNIT_EXPECT_EQ(test, samples[0], 0);
	KUNIT_ASSERT_EQ(test, apple_energy_complete(gate, ticket), 0);
	grant(test, gate, ceiling[0], ceiling[1]);
	apple_energy_snapshot(gate, &snapshot);
	samples[0] = 65536;
	samples[1] = 65536;
	KUNIT_ASSERT_EQ(
		test, apple_energy_filter(gate, samples, 1, 2, 2, &ticket), 0);
	/* Traffic can spend the absolute ceiling while the daemon computes. */
	KUNIT_EXPECT_EQ(
		test, apple_energy_grant(gate, &snapshot, ceiling, 2, 100), 0);
	apple_energy_snapshot(gate, &snapshot);
	KUNIT_EXPECT_EQ(test, snapshot.ceiling[0], ceiling[0]);
	KUNIT_EXPECT_EQ(test, snapshot.submitted[0], 1ULL);
}

static void revoke_preserves_queue_debt(struct kunit *test)
{
	struct apple_energy_gate *gate = new_gate(test);
	struct apple_energy_snapshot before, after;
	u8 slots[] = { 1, 0 };
	s32 samples[] = { S32_MIN, S32_MIN };
	u64 ticket;

	grant(test, gate, (APPLE_ENERGY_UNIT_SCALE * 1 / 1),
	      (APPLE_ENERGY_UNIT_SCALE * 1 / 1));
	KUNIT_ASSERT_EQ(
		test, apple_energy_filter(gate, samples, 1, 2, 2, &ticket), 0);
	apple_energy_snapshot(gate, &before);
	apple_energy_revoke(gate); /* daemon close/crash: no fresh credit */
	KUNIT_EXPECT_EQ(test, apple_energy_configure(gate, 96000, 2, 2, slots),
			-EBUSY);
	apple_energy_snapshot(gate, &after);
	KUNIT_EXPECT_EQ(test, after.submitted[0], before.submitted[0]);
	KUNIT_EXPECT_EQ(test, after.completed[0], 0ULL);
	KUNIT_EXPECT_EQ(test, after.epoch, before.epoch);
	KUNIT_ASSERT_EQ(test, apple_energy_complete(gate, ticket), 0);
	KUNIT_ASSERT_EQ(test, apple_energy_configure(gate, 96000, 2, 2, slots),
			0);
	apple_energy_snapshot(gate, &after);
	KUNIT_EXPECT_GT(test, after.epoch, before.epoch);
	KUNIT_EXPECT_EQ(test, after.submitted[0], before.submitted[0]);
	KUNIT_EXPECT_EQ(test, after.completed[0], before.submitted[0]);
}

static void stalled_completion_pool(struct kunit *test)
{
	struct apple_energy_gate *gate = new_gate(test);
	s32 samples[2];
	u64 tickets[APPLE_ENERGY_QUEUE], ticket;
	int i;

	grant(test, gate, U64_MAX, U64_MAX);
	for (i = 0; i < APPLE_ENERGY_QUEUE; i++) {
		int ret;

		samples[0] = 65536;
		samples[1] = 65536;
		ret = apple_energy_filter(gate, samples, 1, 2, 2, &tickets[i]);
		KUNIT_ASSERT_EQ(test, ret, 0);
	}
	samples[0] = S32_MAX;
	samples[1] = S32_MAX;
	KUNIT_EXPECT_EQ(test,
			apple_energy_filter(gate, samples, 1, 2, 2, &ticket),
			-ENOSPC);
	KUNIT_EXPECT_EQ(test, ticket, 0ULL);
	KUNIT_EXPECT_EQ(test, samples[0], 0);
	for (i = 0; i < APPLE_ENERGY_QUEUE; i++)
		KUNIT_ASSERT_EQ(test, apple_energy_complete(gate, tickets[i]),
				0);
	KUNIT_EXPECT_EQ(test, apple_energy_complete(gate, tickets[0]), -ESTALE);
}

static void completion_is_ordered(struct kunit *test)
{
	struct apple_energy_gate *gate = new_gate(test);
	struct apple_energy_snapshot snapshot;
	s32 samples[] = { 65536, 65536 };
	u64 first, second;

	grant(test, gate, 10, 10);
	KUNIT_ASSERT_EQ(test,
			apple_energy_filter(gate, samples, 1, 2, 2, &first), 0);
	KUNIT_ASSERT_EQ(
		test, apple_energy_filter(gate, samples, 1, 2, 2, &second), 0);
	KUNIT_EXPECT_EQ(test, apple_energy_complete(gate, second), -ESTALE);
	apple_energy_snapshot(gate, &snapshot);
	KUNIT_EXPECT_EQ(test, snapshot.completed[0], 0ULL);
	KUNIT_ASSERT_EQ(test, apple_energy_complete(gate, first), 0);
	KUNIT_ASSERT_EQ(test, apple_energy_complete(gate, second), 0);
}

static void absolute_ceiling_not_topup(struct kunit *test)
{
	struct apple_energy_gate *gate = new_gate(test);
	s32 samples[] = { 65536, 65536 };
	u64 ticket;

	grant(test, gate, 1, 1);
	KUNIT_ASSERT_EQ(
		test, apple_energy_filter(gate, samples, 1, 2, 2, &ticket), 0);
	KUNIT_ASSERT_EQ(test, apple_energy_complete(gate, ticket), 0);
	grant(test, gate, 1, 1);
	KUNIT_ASSERT_EQ(
		test, apple_energy_filter(gate, samples, 1, 2, 2, &ticket), 0);
	KUNIT_EXPECT_EQ(test, samples[0], 0);
}

static void integer_boundary_and_saturation(struct kunit *test)
{
	struct apple_energy_gate *gate = new_gate(test);
	struct apple_energy_snapshot snapshot;
	s32 samples[] = { S32_MIN, S32_MIN };
	u64 ticket;

	/* Test-only injection at the lifetime counter boundary. */
	gate->state.submitted[0] = U64_MAX - 1;
	gate->state.completed[0] = U64_MAX - 1;
	grant(test, gate, U64_MAX, U64_MAX);
	KUNIT_ASSERT_EQ(
		test, apple_energy_filter(gate, samples, 1, 2, 2, &ticket), 0);
	KUNIT_EXPECT_EQ(test, samples[0], -(1 << (31 - APPLE_ENERGY_UNIT_SHIFT / 2)));
	KUNIT_ASSERT_EQ(test, apple_energy_complete(gate, ticket), 0);
	apple_energy_snapshot(gate, &snapshot);
	KUNIT_EXPECT_EQ(test, snapshot.submitted[0], U64_MAX);
	KUNIT_EXPECT_EQ(test, snapshot.completed[0], U64_MAX);
	gate->state.sequence = U64_MAX;
	samples[0] = S32_MIN;
	samples[1] = S32_MIN;
	KUNIT_EXPECT_EQ(test,
			apple_energy_filter(gate, samples, 1, 2, 2, &ticket),
			-EOVERFLOW);
	KUNIT_EXPECT_EQ(test, samples[0], 0);
}

static void every_small_budget_bounds_sample(struct kunit *test)
{
	struct apple_energy_gate *gate = new_gate(test);
	struct apple_energy_snapshot snapshot;
	u64 ceiling[2], ticket;
	u32 rng = 0x13189815;
	unsigned int i;

	for (i = 0; i < 2000; i++) {
		s32 samples[2];
		u64 budget = i;

		rng = rng * 1664525U + 1013904223U;
		samples[0] = rng;
		samples[1] = ~rng;
		apple_energy_snapshot(gate, &snapshot);
		ceiling[0] = snapshot.submitted[0] + budget;
		ceiling[1] = snapshot.submitted[1] + budget;
		KUNIT_ASSERT_EQ(test,
				apple_energy_grant(gate, &snapshot, ceiling, 1,
						   100),
				0);
		KUNIT_ASSERT_EQ(test,
				apple_energy_filter(gate, samples, 1, 2, 2,
						    &ticket),
				0);
		KUNIT_EXPECT_LE(test, apple_energy_cost(samples[0]), budget);
		KUNIT_EXPECT_LE(test, apple_energy_cost(samples[1]), budget);
		KUNIT_ASSERT_EQ(test, apple_energy_complete(gate, ticket), 0);
	}
}

static void invalid_route_cannot_reenable_old_mapping(struct kunit *test)
{
	struct apple_energy_gate *gate = new_gate(test);
	struct apple_energy_snapshot snapshot;
	u64 ceiling[] = { 100, 100 };
	u8 slots[] = { 0, 2 };
	s32 samples[] = { S32_MIN, S32_MIN };
	u64 ticket;

	grant(test, gate, 100, 100);
	KUNIT_EXPECT_EQ(test, apple_energy_configure(gate, 48000, 2, 2, slots),
			-EINVAL);
	apple_energy_snapshot(gate, &snapshot);
	KUNIT_EXPECT_EQ(test,
			apple_energy_grant(gate, &snapshot, ceiling, 1, 100),
			-EINVAL);
	KUNIT_EXPECT_EQ(test,
			apple_energy_filter(gate, samples, 1, 2, 2, &ticket),
			-EINVAL);
	KUNIT_EXPECT_EQ(test, samples[0], 0);
	KUNIT_EXPECT_EQ(test, ticket, 0ULL);
	slots[1] = 1;
	KUNIT_ASSERT_EQ(test, apple_energy_configure(gate, 48000, 2, 2, slots),
			0);
	grant(test, gate, 100, 100);
}

static void exhausted_and_expired_use_reserved_safe_tail(struct kunit *test)
{
	struct apple_energy_gate *gate = new_gate(test);
	struct apple_energy_snapshot snapshot;
	s32 samples[] = { S32_MIN, S32_MAX };
	u64 ticket;

	apple_energy_fallback(gate,
			      1U << 27); /* independently reserved -24 dB */
	grant(test, gate, 0, 0);
	KUNIT_ASSERT_EQ(
		test, apple_energy_filter(gate, samples, 1, 2, 2, &ticket), 0);
	KUNIT_EXPECT_EQ(test, samples[0], -(1 << 27));
	KUNIT_EXPECT_EQ(test, samples[1], (1 << 27) - 1);
	apple_energy_snapshot(gate, &snapshot);
	KUNIT_EXPECT_EQ(test, snapshot.submitted[0], 0ULL);
	KUNIT_ASSERT_EQ(test, apple_energy_complete(gate, ticket), 0);
	grant(test, gate, (APPLE_ENERGY_UNIT_SCALE * 1 / 1),
	      (APPLE_ENERGY_UNIT_SCALE * 1 / 1));
	samples[0] = S32_MIN;
	samples[1] = S32_MAX;
	KUNIT_ASSERT_EQ(test,
			apple_energy_filter(gate, samples, 1, 2, 100, &ticket),
			0);
	KUNIT_EXPECT_EQ(test, samples[0], -(1 << 27));
	apple_energy_snapshot(gate, &snapshot);
	KUNIT_EXPECT_EQ(test, snapshot.submitted[0], 0ULL);
}

static void pcm_formats_round_toward_zero_before_spending(struct kunit *test)
{
	struct apple_energy_gate *gate = new_gate(test);
	struct apple_energy_snapshot snapshot;
	u64 ticket;
	s16 narrow[] = { S16_MIN, S16_MAX };
	u32 wide[] = { 0xde800000, 0xab7fffff }; /* S24 upper byte is ignored */

	grant(test, gate, (APPLE_ENERGY_UNIT_SCALE * 1 / 4),
	      (APPLE_ENERGY_UNIT_SCALE * 1 / 4));
	KUNIT_ASSERT_EQ(test,
			apple_energy_filter_pcm(gate, narrow, 1, 2,
						SNDRV_PCM_FORMAT_S16_LE, 2,
						&ticket),
			0);
	KUNIT_EXPECT_EQ(test, narrow[0], (s16)-16384);
	KUNIT_EXPECT_EQ(test, narrow[1], (s16)16384);
	KUNIT_ASSERT_EQ(test, apple_energy_complete(gate, ticket), 0);
	grant(test, gate, (APPLE_ENERGY_UNIT_SCALE * 2 / 4),
	      (APPLE_ENERGY_UNIT_SCALE * 2 / 4));
	KUNIT_ASSERT_EQ(test,
			apple_energy_filter_pcm(gate, wide, 1, 2,
						SNDRV_PCM_FORMAT_S24_LE, 2,
						&ticket),
			0);
	KUNIT_EXPECT_EQ(test, wide[0], 0xffc00000U);
	KUNIT_EXPECT_EQ(test, wide[1], 0x00400000U);
	apple_energy_snapshot(gate, &snapshot);
	KUNIT_EXPECT_EQ(test, snapshot.submitted[0],
			(APPLE_ENERGY_UNIT_SCALE * 2 / 4));
	KUNIT_EXPECT_EQ(test, snapshot.submitted[1],
			(APPLE_ENERGY_UNIT_SCALE * 2 / 4));
}

static void rate_changes_do_not_reinterpret_lifetime_units(struct kunit *test)
{
	struct apple_energy_gate *gate = new_gate(test);
	struct apple_energy_snapshot snapshot;
	u8 slots[] = { 0, 1 };
	s32 samples[] = { S32_MIN, S32_MIN };
	u64 ticket;

	KUNIT_ASSERT_EQ(test, apple_energy_configure(gate, 96000, 2, 2, slots),
			0);
	grant(test, gate, (APPLE_ENERGY_UNIT_SCALE * 1 / 2),
	      (APPLE_ENERGY_UNIT_SCALE * 1 / 2));
	KUNIT_ASSERT_EQ(
		test, apple_energy_filter(gate, samples, 1, 2, 2, &ticket), 0);
	KUNIT_EXPECT_EQ(test, samples[0], S32_MIN);
	KUNIT_ASSERT_EQ(test, apple_energy_complete(gate, ticket), 0);
	KUNIT_ASSERT_EQ(test, apple_energy_configure(gate, 48000, 2, 2, slots),
			0);
	grant(test, gate, (APPLE_ENERGY_UNIT_SCALE * 3 / 2),
	      (APPLE_ENERGY_UNIT_SCALE * 3 / 2));
	KUNIT_ASSERT_EQ(
		test, apple_energy_filter(gate, samples, 1, 2, 2, &ticket), 0);
	KUNIT_ASSERT_EQ(test, apple_energy_complete(gate, ticket), 0);
	apple_energy_snapshot(gate, &snapshot);
	KUNIT_EXPECT_EQ(test, snapshot.completed[0],
			(APPLE_ENERGY_UNIT_SCALE * 3 / 2));
}

static void safe_raw_prefix_is_charged_before_gain_can_rise(struct kunit *test)
{
	struct apple_energy_gate *gate = new_gate(test);
	struct apple_energy_snapshot state;
	u64 ceiling[] = { (APPLE_ENERGY_UNIT_SCALE * 2 / 1),
			  (APPLE_ENERGY_UNIT_SCALE * 2 / 1) };
	s32 samples[] = { S32_MIN, S32_MIN };
	u64 ticket;

	apple_energy_fallback(gate, 21474);
	KUNIT_ASSERT_EQ(test, apple_energy_hardware_safe(gate, true), 0);
	KUNIT_ASSERT_EQ(
		test, apple_energy_filter(gate, samples, 1, 2, 2, &ticket), 0);
	KUNIT_EXPECT_EQ(test, samples[0],
			S32_MIN); /* hardware is still -100 dB */
	apple_energy_snapshot(gate, &state);
	KUNIT_EXPECT_EQ(test, state.submitted[0], 0ULL);
	KUNIT_EXPECT_EQ(test, state.unbudgeted[0],
			(APPLE_ENERGY_UNIT_SCALE * 1 / 1));
	KUNIT_EXPECT_EQ(test, apple_energy_hardware_safe(gate, false), -EAGAIN);
	KUNIT_ASSERT_EQ(test, apple_energy_grant(gate, &state, ceiling, 2, 100),
			0);
	apple_energy_snapshot(gate, &state);
	KUNIT_EXPECT_EQ(test, state.submitted[0],
			(APPLE_ENERGY_UNIT_SCALE * 1 / 1));
	KUNIT_EXPECT_EQ(test, state.unbudgeted[0], 0ULL);
	KUNIT_ASSERT_EQ(test, apple_energy_hardware_safe(gate, false), 0);
	KUNIT_ASSERT_EQ(test, apple_energy_complete(gate, ticket), 0);
	apple_energy_snapshot(gate, &state);
	KUNIT_EXPECT_EQ(test, state.completed[0],
			(APPLE_ENERGY_UNIT_SCALE * 1 / 1));
	KUNIT_EXPECT_EQ(test, state.refunded[0], 0ULL);
}

static void
unaffordable_raw_prefix_drains_under_the_hardware_lock(struct kunit *test)
{
	struct apple_energy_gate *gate = new_gate(test);
	struct apple_energy_snapshot state;
	u64 ceiling[] = { (APPLE_ENERGY_UNIT_SCALE * 1 / 2),
			  (APPLE_ENERGY_UNIT_SCALE * 1 / 2) };
	s32 samples[] = { S32_MIN, S32_MIN };
	u64 raw, protected;

	apple_energy_fallback(gate, 21474);
	KUNIT_ASSERT_EQ(test, apple_energy_hardware_safe(gate, true), 0);
	KUNIT_ASSERT_EQ(test, apple_energy_filter(gate, samples, 1, 2, 2, &raw),
			0);
	apple_energy_snapshot(gate, &state);
	KUNIT_EXPECT_EQ(test, apple_energy_grant(gate, &state, ceiling, 2, 100),
			-EAGAIN);
	KUNIT_EXPECT_EQ(test, apple_energy_hardware_safe(gate, false), -EAGAIN);
	KUNIT_ASSERT_EQ(test,
			apple_energy_filter(gate, samples, 1, 2, 2, &protected),
			0);
	KUNIT_EXPECT_LT(test, apple_energy_cost(samples[0]),
			(APPLE_ENERGY_UNIT_SCALE * 1 / 1));
	KUNIT_ASSERT_EQ(test, apple_energy_complete(gate, raw), 0);
	KUNIT_ASSERT_EQ(test, apple_energy_complete(gate, protected), 0);
	apple_energy_snapshot(gate, &state);
	KUNIT_EXPECT_EQ(test, state.completed[0], state.refunded[0]);
	KUNIT_EXPECT_GT(test, state.completed[0], 0ULL);
	KUNIT_EXPECT_EQ(test, state.unbudgeted[0], 0ULL);
}

static void bounded_current_metric_covers_arbitrary_voltage_current_phase(
	struct kunit *test)
{
	struct apple_energy_gate *gate = new_gate(test);
	struct apple_energy_snapshot state;
	s32 samples[] = { 1 << 30, -(1 << 30) };
	u64 ticket;

	KUNIT_ASSERT_EQ(test, apple_energy_metric(gate, APPLE_ENERGY_ABSOLUTE),
			0);
	grant(test, gate, (APPLE_ENERGY_UNIT_SCALE * 1 / 4),
	      (APPLE_ENERGY_UNIT_SCALE * 1 / 4));
	KUNIT_ASSERT_EQ(
		test, apple_energy_filter(gate, samples, 1, 2, 2, &ticket), 0);
	KUNIT_EXPECT_EQ(test, samples[0], 1 << 29);
	KUNIT_EXPECT_EQ(test, samples[1], -(1 << 29));
	apple_energy_snapshot(gate, &state);
	KUNIT_EXPECT_EQ(test, state.submitted[0],
			(APPLE_ENERGY_UNIT_SCALE * 1 / 4));
	KUNIT_EXPECT_EQ(test, state.source_submitted[0],
			(APPLE_ENERGY_UNIT_SCALE * 1 / 4));
	KUNIT_ASSERT_EQ(test, apple_energy_complete(gate, ticket), 0);
	/* A different power law may not reinterpret already-issued counters. */
	KUNIT_EXPECT_EQ(test, apple_energy_metric(gate, APPLE_ENERGY_SQUARED),
			-EINVAL);
}


/* These are LEAP binary32 words, not S32 bit patterns or ordinary 1.0 FS. */
static void neo_float_full_scale_is_half_not_one(struct kunit *test)
{
	struct apple_energy_gate *gate = new_gate(test);
	struct apple_energy_snapshot state;
	__le32 words[] = { cpu_to_le32(0xbf000000), cpu_to_le32(0x3e800000) };
	u64 ticket;

	grant(test, gate, APPLE_ENERGY_UNIT_SCALE, APPLE_ENERGY_UNIT_SCALE);
	KUNIT_ASSERT_EQ(test, apple_energy_filter_pcm(gate, words, 1, 2,
		APPLE_ENERGY_FORMAT_NEO_F32, 2, &ticket), 0);
	KUNIT_EXPECT_EQ(test, le32_to_cpu(words[0]), 0xbf000000U);
	KUNIT_EXPECT_EQ(test, le32_to_cpu(words[1]), 0x3e800000U);
	apple_energy_snapshot(gate, &state);
	KUNIT_EXPECT_EQ(test, state.submitted[0], APPLE_ENERGY_UNIT_SCALE);
	KUNIT_EXPECT_EQ(test, state.submitted[1], APPLE_ENERGY_UNIT_SCALE / 4);
	KUNIT_ASSERT_EQ(test, apple_energy_complete(gate, ticket), 0);
}

static void neo_float_malformed_payload_never_reaches_dma(struct kunit *test)
{
	/* Infinity, NaN, 1.0, >0.5, +0.5 (not signed-Q31), subnormal,
	 * and a value between Q31 lattice points. All must fail closed.
	 */
	static const u32 invalid[] = {
		0x7f800000, 0xff800000, 0x7fc00000, 0x3f800000,
		0xbf000001, 0x3f000000, 1, 0x2f800001,
	};
	unsigned int k;

	for (k = 0; k < ARRAY_SIZE(invalid); k++) {
		struct apple_energy_gate *gate = new_gate(test);
		struct apple_energy_snapshot state;
		__le32 words[] = { cpu_to_le32(0xbf000000), cpu_to_le32(invalid[k]) };
		u64 ticket = 123;

		grant(test, gate, APPLE_ENERGY_UNIT_SCALE, APPLE_ENERGY_UNIT_SCALE);
		KUNIT_EXPECT_EQ(test, apple_energy_filter_pcm(gate, words, 1, 2,
			APPLE_ENERGY_FORMAT_NEO_F32, 2, &ticket), -EINVAL);
		KUNIT_EXPECT_EQ(test, ticket, 0ULL);
		KUNIT_EXPECT_EQ(test, words[0], (__le32)0);
		KUNIT_EXPECT_EQ(test, words[1], (__le32)0);
		apple_energy_snapshot(gate, &state);
		KUNIT_EXPECT_TRUE(test, state.failed);
		KUNIT_EXPECT_FALSE(test, state.enabled);
		KUNIT_EXPECT_EQ(test, state.pending, 0U);
	}
}

/* Sweep every small allowance and both signs. Decode the emitted float
 * independently, and compare exact squared Q31 magnitude to the allowance.
 */
static void neo_float_rounding_never_spends_more_than_the_grant(struct kunit *test)
{
	struct apple_energy_gate *gate = new_gate(test);
	u64 allowance;

	for (allowance = 0; allowance < 4096; allowance++) {
		struct apple_energy_snapshot before, after;
		u64 ceiling[2], ticket;
		__le32 words[] = { cpu_to_le32(0xbf000000), cpu_to_le32(0x3effffff) };
		unsigned int k;

		apple_energy_snapshot(gate, &before);
		for (k = 0; k < 2; k++)
			ceiling[k] = before.submitted[k] + allowance;
		KUNIT_ASSERT_EQ(test, apple_energy_grant(gate, &before, ceiling, 1, 100), 0);
		KUNIT_ASSERT_EQ(test, apple_energy_filter_pcm(gate, words, 1, 2,
			APPLE_ENERGY_FORMAT_NEO_F32, 2, &ticket), 0);
		apple_energy_snapshot(gate, &after);
		for (k = 0; k < 2; k++) {
			u32 bits = le32_to_cpu(words[k]) & 0x7fffffff;
			u32 exponent = bits >> 23;
			u64 magnitude = bits ? (bits & 0x7fffff) | BIT(23) : 0;

			if (bits && exponent < 118)
				magnitude >>= 118 - exponent;
			else if (bits)
				magnitude <<= exponent - 118;
			KUNIT_EXPECT_LE(test, magnitude * magnitude,
				allowance << (62 - APPLE_ENERGY_UNIT_SHIFT));
			KUNIT_EXPECT_LE(test, after.submitted[k] - before.submitted[k], allowance);
		}
		KUNIT_ASSERT_EQ(test, apple_energy_complete(gate, ticket), 0);
	}
}

static void neo_float_death_keeps_queued_dose_and_mutes_refills(struct kunit *test)
{
	struct apple_energy_gate *gate = new_gate(test);
	struct apple_energy_snapshot before, after;
	__le32 words[] = { cpu_to_le32(0xbf000000), cpu_to_le32(0xbf000000) };
	u64 paid, muted;

	grant(test, gate, APPLE_ENERGY_UNIT_SCALE, APPLE_ENERGY_UNIT_SCALE);
	KUNIT_ASSERT_EQ(test, apple_energy_filter_pcm(gate, words, 1, 2,
		APPLE_ENERGY_FORMAT_NEO_F32, 2, &paid), 0);
	apple_energy_snapshot(gate, &before);
	apple_energy_revoke(gate); /* owner death, independent of delayed work */
	words[0] = words[1] = cpu_to_le32(0xbf000000);
	KUNIT_ASSERT_EQ(test, apple_energy_filter_pcm(gate, words, 1, 2,
		APPLE_ENERGY_FORMAT_NEO_F32, 3, &muted), 0);
	KUNIT_EXPECT_EQ(test, words[0], (__le32)0);
	KUNIT_EXPECT_EQ(test, words[1], (__le32)0);
	apple_energy_snapshot(gate, &after);
	KUNIT_EXPECT_EQ(test, after.submitted[0], before.submitted[0]);
	KUNIT_EXPECT_EQ(test, after.completed[0], before.completed[0]);
	KUNIT_ASSERT_EQ(test, apple_energy_complete(gate, paid), 0);
	KUNIT_ASSERT_EQ(test, apple_energy_complete(gate, muted), 0);
	apple_energy_snapshot(gate, &after);
	KUNIT_EXPECT_EQ(test, after.completed[0], before.submitted[0]);
	KUNIT_EXPECT_EQ(test, after.refunded[0], 0ULL);
}


static void neo_float_cannot_cache_or_refund_a_copy_time_cap(struct kunit *test)
{
	struct apple_energy_gate *gate = new_gate(test);
	struct apple_energy_snapshot state;
	__le32 words[] = { cpu_to_le32(0xbf000000), cpu_to_le32(0xbf000000) };
	u64 ticket;

	/* Deliberately assert a cap the Neo does not actually have. The
	 * payload format itself must exclude raw caching and safe refunds.
	 */
	KUNIT_ASSERT_EQ(test, apple_energy_hardware_safe(gate, true), 0);
	KUNIT_ASSERT_EQ(test, apple_energy_filter_pcm(gate, words, 1, 2,
		APPLE_ENERGY_FORMAT_NEO_F32, 1, &ticket), 0);
	KUNIT_EXPECT_EQ(test, words[0], (__le32)0);
	KUNIT_ASSERT_EQ(test, apple_energy_complete(gate, ticket), 0);
	grant(test, gate, APPLE_ENERGY_UNIT_SCALE, APPLE_ENERGY_UNIT_SCALE);
	words[0] = words[1] = cpu_to_le32(0xbf000000);
	KUNIT_ASSERT_EQ(test, apple_energy_filter_pcm(gate, words, 1, 2,
		APPLE_ENERGY_FORMAT_NEO_F32, 2, &ticket), 0);
	KUNIT_ASSERT_EQ(test, apple_energy_complete(gate, ticket), 0);
	apple_energy_snapshot(gate, &state);
	KUNIT_EXPECT_EQ(test, state.completed[0], APPLE_ENERGY_UNIT_SCALE);
	KUNIT_EXPECT_EQ(test, state.refunded[0], 0ULL);
	KUNIT_EXPECT_EQ(test, state.unbudgeted[0], 0ULL);
}

static struct kunit_case apple_energy_cases[] = {
	KUNIT_CASE(neo_float_cannot_cache_or_refund_a_copy_time_cap),
	KUNIT_CASE(neo_float_full_scale_is_half_not_one),
	KUNIT_CASE(neo_float_malformed_payload_never_reaches_dma),
	KUNIT_CASE(neo_float_rounding_never_spends_more_than_the_grant),
	KUNIT_CASE(neo_float_death_keeps_queued_dose_and_mutes_refills),
	KUNIT_CASE(energy_rounding),
	KUNIT_CASE(absent_and_expired),
	KUNIT_CASE(exhaustion_per_speaker),
	KUNIT_CASE(shared_slot),
	KUNIT_CASE(stale_grant_and_replay),
	KUNIT_CASE(revoke_preserves_queue_debt),
	KUNIT_CASE(stalled_completion_pool),
	KUNIT_CASE(completion_is_ordered),
	KUNIT_CASE(absolute_ceiling_not_topup),
	KUNIT_CASE(integer_boundary_and_saturation),
	KUNIT_CASE(every_small_budget_bounds_sample),
	KUNIT_CASE(invalid_route_cannot_reenable_old_mapping),
	KUNIT_CASE(exhausted_and_expired_use_reserved_safe_tail),
	KUNIT_CASE(pcm_formats_round_toward_zero_before_spending),
	KUNIT_CASE(rate_changes_do_not_reinterpret_lifetime_units),
	KUNIT_CASE(safe_raw_prefix_is_charged_before_gain_can_rise),
	KUNIT_CASE(unaffordable_raw_prefix_drains_under_the_hardware_lock),
	KUNIT_CASE(
		bounded_current_metric_covers_arbitrary_voltage_current_phase),
	{}
};

static struct kunit_suite apple_energy_suite = {
	.name = "apple-speaker-energy",
	.test_cases = apple_energy_cases,
};

kunit_test_suite(apple_energy_suite);

MODULE_LICENSE("GPL");
