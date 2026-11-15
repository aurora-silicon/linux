// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* Copyright 2026 Ryan Murray */

#include <kunit/test.h>
#include <linux/module.h>

#include "dcp-announce.h"

#define TIMEOUT msecs_to_jiffies(DCP_ANNOUNCE_TIMEOUT_MS)
#define RESUME_TIMEOUT msecs_to_jiffies(DCP_ANNOUNCE_RESUME_TIMEOUT_MS)

static const struct dcp_announce_facts link_only = {
	.generation = 1, .connected = true, .present = true,
};

static const struct dcp_announce_facts link_ready = {
	.generation = 1, .connected = true, .present = true, .ready = true,
};

static const struct dcp_announce_facts unplugged = {
	.generation = 2,
};

static void announce_ready_test(struct kunit *test)
{
	struct dcp_announce a = { };

	KUNIT_EXPECT_EQ(test, dcp_announce_update(&a, &link_ready, 100),
			DCP_ANNOUNCE_CONNECTED);
	KUNIT_EXPECT_TRUE(test, a.announced);
	KUNIT_EXPECT_FALSE(test, a.holding);
	KUNIT_EXPECT_TRUE(test, dcp_announce_notify(&a, DCP_ANNOUNCE_CONNECTED));
}

static void announce_waits_for_ready_test(struct kunit *test)
{
	struct dcp_announce a = { };

	KUNIT_EXPECT_EQ(test, dcp_announce_update(&a, &link_only, 100),
			DCP_ANNOUNCE_HOLD);
	KUNIT_EXPECT_TRUE(test, a.holding);
	KUNIT_EXPECT_FALSE(test, a.announced);
	KUNIT_EXPECT_FALSE(test, a.timed);
	KUNIT_EXPECT_FALSE(test, dcp_announce_notify(&a, DCP_ANNOUNCE_HOLD));
	dcp_announce_start(&a, 100);
	KUNIT_EXPECT_EQ(test, a.deadline, 100 + TIMEOUT);

	/* Neither re-evaluation nor starting again extends the deadline. */
	KUNIT_EXPECT_EQ(test, dcp_announce_update(&a, &link_only, 100 + TIMEOUT - 1),
			DCP_ANNOUNCE_HOLD);
	dcp_announce_start(&a, 100 + TIMEOUT - 1);
	KUNIT_EXPECT_EQ(test, a.deadline, 100 + TIMEOUT);

	KUNIT_EXPECT_EQ(test, dcp_announce_update(&a, &link_ready, 100 + TIMEOUT - 1),
			DCP_ANNOUNCE_CONNECTED);
	KUNIT_EXPECT_FALSE(test, a.holding);
	KUNIT_EXPECT_TRUE(test, dcp_announce_notify(&a, DCP_ANNOUNCE_CONNECTED));
}

static void announce_timeout_test(struct kunit *test)
{
	struct dcp_announce a = { };

	dcp_announce_update(&a, &link_only, 100);
	dcp_announce_start(&a, 100);
	KUNIT_EXPECT_FALSE(test, dcp_announce_expired(&a, 1, 100 + TIMEOUT - 1));
	KUNIT_EXPECT_TRUE(test, dcp_announce_expired(&a, 1, 100 + TIMEOUT));
	KUNIT_EXPECT_EQ(test, dcp_announce_update(&a, &link_only, 100 + TIMEOUT),
			DCP_ANNOUNCE_CONNECTED);
	KUNIT_EXPECT_TRUE(test, a.announced);
	KUNIT_EXPECT_FALSE(test, a.holding);
	/* A timer that fires after the hold ended finds nothing to release. */
	KUNIT_EXPECT_FALSE(test, dcp_announce_expired(&a, 1, 100 + 2 * TIMEOUT));
}

static void announce_timeout_wraps_test(struct kunit *test)
{
	unsigned long start = ULONG_MAX - TIMEOUT / 2;
	struct dcp_announce a = { };

	dcp_announce_update(&a, &link_only, start);
	dcp_announce_start(&a, start);
	KUNIT_EXPECT_EQ(test, dcp_announce_update(&a, &link_only, start + TIMEOUT - 1),
			DCP_ANNOUNCE_HOLD);
	KUNIT_EXPECT_EQ(test, dcp_announce_update(&a, &link_only, start + TIMEOUT),
			DCP_ANNOUNCE_CONNECTED);
}

/* A failed link will not get ready; report it without waiting. */
static void announce_failed_test(struct kunit *test)
{
	struct dcp_announce_facts f = link_only;
	struct dcp_announce a = { };

	f.failed = true;
	KUNIT_EXPECT_EQ(test, dcp_announce_update(&a, &f, 0), DCP_ANNOUNCE_CONNECTED);
}

/* A link retrained on the same cable must not withdraw the connection. */
static void announce_keeps_reported_test(struct kunit *test)
{
	struct dcp_announce_facts f = link_only;
	struct dcp_announce a = { };

	dcp_announce_update(&a, &link_ready, 0);
	f.generation = 2;
	KUNIT_EXPECT_EQ(test, dcp_announce_update(&a, &f, 10 * TIMEOUT),
			DCP_ANNOUNCE_CONNECTED);
	KUNIT_EXPECT_FALSE(test, a.holding);
}

/* A missing cable wins over a link DCP has not dropped yet. */
static void announce_absent_wins_test(struct kunit *test)
{
	struct dcp_announce_facts f = link_ready;
	struct dcp_announce a = { };

	dcp_announce_update(&a, &link_ready, 0);
	f.present = false;
	KUNIT_EXPECT_EQ(test, dcp_announce_update(&a, &f, 1), DCP_ANNOUNCE_DISCONNECTED);
	KUNIT_EXPECT_FALSE(test, a.announced);
	KUNIT_EXPECT_TRUE(test, dcp_announce_notify(&a, DCP_ANNOUNCE_DISCONNECTED));
}

static void announce_disconnect_during_hold_test(struct kunit *test)
{
	struct dcp_announce a = { };

	dcp_announce_update(&a, &link_only, 0);
	dcp_announce_start(&a, 0);
	KUNIT_EXPECT_EQ(test, dcp_announce_update(&a, &unplugged, 1),
			DCP_ANNOUNCE_DISCONNECTED);
	KUNIT_EXPECT_FALSE(test, a.holding);
	KUNIT_EXPECT_FALSE(test, dcp_announce_owed(&a));
	KUNIT_EXPECT_FALSE(test, dcp_announce_expired(&a, 1, TIMEOUT));

	/* The next connection gets a full timeout of its own. */
	KUNIT_EXPECT_EQ(test, dcp_announce_update(&a, &link_only, 5 * TIMEOUT),
			DCP_ANNOUNCE_HOLD);
	KUNIT_EXPECT_FALSE(test, a.timed);
	dcp_announce_start(&a, 5 * TIMEOUT);
	KUNIT_EXPECT_EQ(test, a.deadline, 5 * TIMEOUT + TIMEOUT);
	KUNIT_EXPECT_FALSE(test, dcp_announce_expired(&a, 1, TIMEOUT));
}

/*
 * A replug that the hotplug worker only sees once the new link is up: the
 * old connection still needs its event while the new one is held.
 */
static void announce_replug_test(struct kunit *test)
{
	struct dcp_announce_facts f = link_only;
	struct dcp_announce a = { };

	dcp_announce_update(&a, &link_ready, 0);
	dcp_announce_notify(&a, DCP_ANNOUNCE_CONNECTED);
	dcp_announce_end(&a);
	f.generation = 2;
	KUNIT_EXPECT_EQ(test, dcp_announce_update(&a, &f, 1), DCP_ANNOUNCE_HOLD);
	KUNIT_EXPECT_TRUE(test, dcp_announce_notify(&a, DCP_ANNOUNCE_HOLD));
	KUNIT_EXPECT_FALSE(test, dcp_announce_notify(&a, DCP_ANNOUNCE_HOLD));
	f.ready = true;
	KUNIT_EXPECT_EQ(test, dcp_announce_update(&a, &f, 2), DCP_ANNOUNCE_CONNECTED);
}

/* Ending a connection that was never reported owes userspace nothing. */
static void announce_end_unreported_test(struct kunit *test)
{
	struct dcp_announce a = { };

	dcp_announce_update(&a, &link_only, 0);
	dcp_announce_end(&a);
	KUNIT_EXPECT_FALSE(test, a.withdrawn);
	KUNIT_EXPECT_FALSE(test, a.holding);
}

/* Only a reported connection has an event to hold. */
static void hold_unreported_test(struct kunit *test)
{
	struct dcp_announce a = { };

	KUNIT_EXPECT_FALSE(test, dcp_announce_hold(&a, 1));
	KUNIT_EXPECT_FALSE(test, a.holding);
}

static struct dcp_announce held(u64 generation)
{
	struct dcp_announce a = { .announced = true };

	dcp_announce_hold(&a, generation);
	return a;
}

/*
 * A held event keeps the connection reported, and its timeout runs only once
 * started: the link cannot come back before then.
 */
static void hold_reported_test(struct kunit *test)
{
	struct dcp_announce a = held(1);

	KUNIT_EXPECT_EQ(test, dcp_announce_update(&a, &link_only, 0), DCP_ANNOUNCE_HOLD);
	KUNIT_EXPECT_TRUE(test, a.announced);
	KUNIT_EXPECT_FALSE(test, dcp_announce_notify(&a, DCP_ANNOUNCE_HOLD));
	KUNIT_EXPECT_EQ(test, dcp_announce_update(&a, &link_only, 100 * RESUME_TIMEOUT),
			DCP_ANNOUNCE_HOLD);
	KUNIT_EXPECT_FALSE(test, dcp_announce_expired(&a, 1, 100 * RESUME_TIMEOUT));

	dcp_announce_start(&a, 1000);
	KUNIT_EXPECT_EQ(test, a.deadline, 1000 + RESUME_TIMEOUT);
	/* Starting again does not move the deadline. */
	dcp_announce_start(&a, 2000);
	KUNIT_EXPECT_EQ(test, a.deadline, 1000 + RESUME_TIMEOUT);
	KUNIT_EXPECT_EQ(test, dcp_announce_update(&a, &link_only, 1000 + RESUME_TIMEOUT - 1),
			DCP_ANNOUNCE_HOLD);
	KUNIT_EXPECT_TRUE(test, dcp_announce_expired(&a, 1, 1000 + RESUME_TIMEOUT));
	KUNIT_EXPECT_EQ(test, dcp_announce_update(&a, &link_only, 1000 + RESUME_TIMEOUT),
			DCP_ANNOUNCE_CONNECTED);
	KUNIT_EXPECT_TRUE(test, dcp_announce_notify(&a, DCP_ANNOUNCE_CONNECTED));
	KUNIT_EXPECT_FALSE(test, a.holding);
}

static void hold_ready_test(struct kunit *test)
{
	struct dcp_announce a = held(1);

	dcp_announce_start(&a, 0);
	KUNIT_EXPECT_EQ(test, dcp_announce_update(&a, &link_only, 1), DCP_ANNOUNCE_HOLD);
	KUNIT_EXPECT_EQ(test, dcp_announce_update(&a, &link_ready, 2), DCP_ANNOUNCE_CONNECTED);
	KUNIT_EXPECT_TRUE(test, dcp_announce_notify(&a, DCP_ANNOUNCE_CONNECTED));
	KUNIT_EXPECT_FALSE(test, dcp_announce_expired(&a, 1, RESUME_TIMEOUT));
}

static void hold_failed_test(struct kunit *test)
{
	struct dcp_announce_facts f = link_only;
	struct dcp_announce a = held(1);

	f.failed = true;
	KUNIT_EXPECT_EQ(test, dcp_announce_update(&a, &f, 0), DCP_ANNOUNCE_CONNECTED);
	KUNIT_EXPECT_TRUE(test, a.announced);
}

/* An unplug while the event is held is reported at once. */
static void hold_unplug_test(struct kunit *test)
{
	struct dcp_announce a = held(1);

	KUNIT_EXPECT_EQ(test, dcp_announce_update(&a, &unplugged, 0),
			DCP_ANNOUNCE_DISCONNECTED);
	KUNIT_EXPECT_TRUE(test, dcp_announce_notify(&a, DCP_ANNOUNCE_DISCONNECTED));
	KUNIT_EXPECT_FALSE(test, a.holding);
}

/*
 * A hold that outlives its cable generation, as across another sleep, waits
 * for the next one from the start: the old deadline is not carried over.
 */
static void hold_next_generation_test(struct kunit *test)
{
	struct dcp_announce_facts f = link_only;
	struct dcp_announce a = held(1);

	dcp_announce_start(&a, 0);
	f.generation = 2;
	KUNIT_EXPECT_EQ(test, dcp_announce_update(&a, &f, RESUME_TIMEOUT),
			DCP_ANNOUNCE_HOLD);
	KUNIT_EXPECT_EQ(test, a.generation, 2);
	KUNIT_EXPECT_FALSE(test, dcp_announce_expired(&a, 2, 10 * RESUME_TIMEOUT));
	dcp_announce_start(&a, 10 * RESUME_TIMEOUT);
	KUNIT_EXPECT_EQ(test, a.deadline, 11 * RESUME_TIMEOUT);
}

/*
 * The deadline of one generation never ends the hold of the next, even
 * before an update has moved the hold on.
 */
static void hold_deadline_generation_test(struct kunit *test)
{
	struct dcp_announce a = held(1);

	dcp_announce_start(&a, 0);
	KUNIT_EXPECT_TRUE(test, dcp_announce_expired(&a, 1, RESUME_TIMEOUT));
	KUNIT_EXPECT_FALSE(test, dcp_announce_expired(&a, 2, RESUME_TIMEOUT));
}

/* A new connection seen first in a later generation restarts its timeout. */
static void announce_next_generation_test(struct kunit *test)
{
	struct dcp_announce_facts f = link_only;
	struct dcp_announce a = { };

	dcp_announce_update(&a, &f, 0);
	dcp_announce_start(&a, 0);
	f.generation = 2;
	KUNIT_EXPECT_EQ(test, dcp_announce_update(&a, &f, TIMEOUT), DCP_ANNOUNCE_HOLD);
	KUNIT_EXPECT_FALSE(test, a.timed);
	dcp_announce_start(&a, 3 * TIMEOUT);
	KUNIT_EXPECT_EQ(test, a.deadline, 4 * TIMEOUT);
}

/* Until its timeout is started, a hold waits however long it takes. */
static void announce_untimed_test(struct kunit *test)
{
	struct dcp_announce a = { };

	dcp_announce_update(&a, &link_only, 0);
	KUNIT_EXPECT_EQ(test, dcp_announce_update(&a, &link_only, 100 * TIMEOUT),
			DCP_ANNOUNCE_HOLD);
	KUNIT_EXPECT_FALSE(test, dcp_announce_expired(&a, 1, 100 * TIMEOUT));
}

/*
 * However a new connection is released, and whoever evaluates it, its event
 * is owed until it is handed out, and it is handed out once.
 */
static void announce_release_owed_test(struct kunit *test)
{
	struct dcp_announce a = { };

	dcp_announce_update(&a, &link_only, 0);
	dcp_announce_start(&a, 0);
	KUNIT_EXPECT_FALSE(test, dcp_announce_owed(&a));
	KUNIT_EXPECT_EQ(test, dcp_announce_update(&a, &link_only, TIMEOUT),
			DCP_ANNOUNCE_CONNECTED);
	KUNIT_EXPECT_TRUE(test, dcp_announce_owed(&a));
	/* Evaluating it again does not hand the event out. */
	KUNIT_EXPECT_EQ(test, dcp_announce_update(&a, &link_only, TIMEOUT + 1),
			DCP_ANNOUNCE_CONNECTED);
	KUNIT_EXPECT_TRUE(test, dcp_announce_owed(&a));
	KUNIT_EXPECT_TRUE(test, dcp_announce_notify(&a, DCP_ANNOUNCE_CONNECTED));
	KUNIT_EXPECT_FALSE(test, dcp_announce_owed(&a));
}

/* An evaluation after the hold ended and its event went out owes nothing. */
static void hold_release_once_test(struct kunit *test)
{
	struct dcp_announce a = held(1);

	dcp_announce_start(&a, 0);
	KUNIT_EXPECT_EQ(test, dcp_announce_update(&a, &link_only, RESUME_TIMEOUT),
			DCP_ANNOUNCE_CONNECTED);
	KUNIT_EXPECT_TRUE(test, dcp_announce_owed(&a));
	dcp_announce_notify(&a, DCP_ANNOUNCE_CONNECTED);
	KUNIT_EXPECT_EQ(test, dcp_announce_update(&a, &link_only, 2 * RESUME_TIMEOUT),
			DCP_ANNOUNCE_CONNECTED);
	KUNIT_EXPECT_FALSE(test, dcp_announce_owed(&a));
}

/*
 * A new hold before the event of the last one went out holds that event
 * too: it goes out once, when the new hold ends.
 */
static void hold_again_test(struct kunit *test)
{
	struct dcp_announce a = held(1);

	KUNIT_EXPECT_EQ(test, dcp_announce_update(&a, &link_ready, 0),
			DCP_ANNOUNCE_CONNECTED);
	KUNIT_EXPECT_TRUE(test, dcp_announce_hold(&a, 2));
	KUNIT_EXPECT_FALSE(test, dcp_announce_owed(&a));
	KUNIT_EXPECT_FALSE(test, dcp_announce_notify(&a, DCP_ANNOUNCE_HOLD));
	KUNIT_EXPECT_EQ(test, dcp_announce_update(&a, &link_ready, 1),
			DCP_ANNOUNCE_CONNECTED);
	KUNIT_EXPECT_TRUE(test, dcp_announce_owed(&a));
	KUNIT_EXPECT_TRUE(test, dcp_announce_notify(&a, DCP_ANNOUNCE_CONNECTED));
	KUNIT_EXPECT_FALSE(test, dcp_announce_owed(&a));
}

/* A reported connection whose link was not restored is withdrawn once. */
static void lost_withdraws_test(struct kunit *test)
{
	struct dcp_announce_facts lost = link_ready;
	struct dcp_announce a = { };

	KUNIT_ASSERT_EQ(test, dcp_announce_update(&a, &link_ready, 1),
			DCP_ANNOUNCE_CONNECTED);
	KUNIT_ASSERT_TRUE(test, dcp_announce_notify(&a, DCP_ANNOUNCE_CONNECTED));

	dcp_announce_lose(&a, 1);
	KUNIT_EXPECT_TRUE(test, dcp_announce_lost(&a, 1));
	lost.lost = true;
	KUNIT_EXPECT_EQ(test, dcp_announce_update(&a, &lost, 2),
			DCP_ANNOUNCE_DISCONNECTED);
	KUNIT_EXPECT_FALSE(test, a.announced);
	KUNIT_EXPECT_TRUE(test, dcp_announce_owed(&a));
	KUNIT_EXPECT_TRUE(test, dcp_announce_notify(&a, DCP_ANNOUNCE_DISCONNECTED));
	KUNIT_EXPECT_FALSE(test, dcp_announce_owed(&a));

	/* Modes still in for the lost generation do not bring it back. */
	KUNIT_EXPECT_EQ(test, dcp_announce_update(&a, &lost, 3),
			DCP_ANNOUNCE_DISCONNECTED);
	KUNIT_EXPECT_FALSE(test, dcp_announce_owed(&a));
	/* Nor does an unchanged loss send its event again. */
	KUNIT_EXPECT_FALSE(test, dcp_announce_notify(&a, DCP_ANNOUNCE_DISCONNECTED));
}

/* A loss belongs to its cable generation: the next cable is not lost. */
static void lost_generation_test(struct kunit *test)
{
	struct dcp_announce a = { };

	dcp_announce_lose(&a, 1);
	KUNIT_EXPECT_TRUE(test, dcp_announce_lost(&a, 1));
	KUNIT_EXPECT_FALSE(test, dcp_announce_lost(&a, 2));

	/* Releasing a CRTC on the next cable retries nothing, and forgets the loss. */
	KUNIT_EXPECT_FALSE(test, dcp_announce_retry(&a, 2));
	KUNIT_EXPECT_FALSE(test, dcp_announce_lost(&a, 1));
}

/* A lost connection whose CRTC was released is a new connection. */
static void lost_retry_test(struct kunit *test)
{
	struct dcp_announce_facts lost = link_only;
	struct dcp_announce a = { };

	KUNIT_ASSERT_EQ(test, dcp_announce_update(&a, &link_ready, 1),
			DCP_ANNOUNCE_CONNECTED);
	dcp_announce_lose(&a, 1);
	lost.lost = true;
	KUNIT_ASSERT_EQ(test, dcp_announce_update(&a, &lost, 2),
			DCP_ANNOUNCE_DISCONNECTED);
	KUNIT_ASSERT_TRUE(test, dcp_announce_notify(&a, DCP_ANNOUNCE_DISCONNECTED));

	KUNIT_EXPECT_TRUE(test, dcp_announce_retry(&a, 1));
	KUNIT_EXPECT_FALSE(test, dcp_announce_lost(&a, 1));
	KUNIT_EXPECT_FALSE(test, dcp_announce_retry(&a, 1));
	KUNIT_EXPECT_EQ(test, dcp_announce_update(&a, &link_only, 3),
			DCP_ANNOUNCE_HOLD);
	KUNIT_EXPECT_FALSE(test, a.announced);
	KUNIT_EXPECT_EQ(test, dcp_announce_update(&a, &link_ready, 4),
			DCP_ANNOUNCE_CONNECTED);
	KUNIT_EXPECT_TRUE(test, dcp_announce_owed(&a));
	KUNIT_EXPECT_TRUE(test, dcp_announce_notify(&a, DCP_ANNOUNCE_CONNECTED));
}

/* A connection lost again after its retry waits for the next cable. */
static void lost_retry_once_test(struct kunit *test)
{
	struct dcp_announce a = { };

	dcp_announce_lose(&a, 1);
	KUNIT_EXPECT_TRUE(test, dcp_announce_retry(&a, 1));
	dcp_announce_lose(&a, 1);
	KUNIT_EXPECT_FALSE(test, dcp_announce_retry(&a, 1));
	KUNIT_EXPECT_TRUE(test, dcp_announce_lost(&a, 1));

	/* The next cable starts afresh. */
	dcp_announce_lose(&a, 2);
	KUNIT_EXPECT_TRUE(test, dcp_announce_retry(&a, 2));
	KUNIT_EXPECT_FALSE(test, dcp_announce_lost(&a, 2));
}

static struct kunit_case announce_cases[] = {
	KUNIT_CASE(announce_ready_test),
	KUNIT_CASE(announce_waits_for_ready_test),
	KUNIT_CASE(announce_timeout_test),
	KUNIT_CASE(announce_timeout_wraps_test),
	KUNIT_CASE(announce_failed_test),
	KUNIT_CASE(announce_keeps_reported_test),
	KUNIT_CASE(announce_absent_wins_test),
	KUNIT_CASE(announce_disconnect_during_hold_test),
	KUNIT_CASE(announce_replug_test),
	KUNIT_CASE(announce_end_unreported_test),
	KUNIT_CASE(announce_next_generation_test),
	KUNIT_CASE(announce_untimed_test),
	KUNIT_CASE(announce_release_owed_test),
	KUNIT_CASE(hold_unreported_test),
	KUNIT_CASE(hold_reported_test),
	KUNIT_CASE(hold_ready_test),
	KUNIT_CASE(hold_failed_test),
	KUNIT_CASE(hold_unplug_test),
	KUNIT_CASE(hold_next_generation_test),
	KUNIT_CASE(hold_deadline_generation_test),
	KUNIT_CASE(hold_release_once_test),
	KUNIT_CASE(hold_again_test),
	KUNIT_CASE(lost_withdraws_test),
	KUNIT_CASE(lost_generation_test),
	KUNIT_CASE(lost_retry_test),
	KUNIT_CASE(lost_retry_once_test),
	{}
};

static struct kunit_suite announce_suite = {
	.name = "apple-dcp-announce",
	.test_cases = announce_cases,
};

kunit_test_suite(announce_suite);

MODULE_DESCRIPTION("KUnit tests for the Apple DCP external connection announcement");
MODULE_LICENSE("Dual MIT/GPL");
