// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * Included by dcp.c: the native external pipe's audio work across probe,
 * bind, sleep and unbind. The work function is swapped for a counter, so a
 * start that queued it shows.
 */
#include <kunit/test.h>

static atomic_t dcp_ext_audio_runs;

static void dcp_ext_audio_count(struct work_struct *work)
{
	atomic_inc(&dcp_ext_audio_runs);
}

static unsigned int dcp_ext_audio_start_runs(struct apple_dcp *dcp)
{
	atomic_set(&dcp_ext_audio_runs, 0);
	dcp_external_audio_start(dcp);
	flush_work(&dcp->external_audio_work);
	return atomic_read(&dcp_ext_audio_runs);
}

static void dcp_ext_audio_stop(void *data)
{
	struct apple_dcp *dcp = data;

	disable_work_sync(&dcp->external_audio_work);
}

static struct apple_dcp *dcp_ext_audio_probe(struct kunit *test, bool native)
{
	struct apple_dcp *dcp = kunit_kzalloc(test, sizeof(*dcp), GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, dcp);
	dcp->fw_compat = DCP_FIRMWARE_V_14_7;
	dcp->external = native;
	dcp->external_native = native;
	dcp_external_audio_init(dcp);
	dcp->external_audio_work.func = dcp_ext_audio_count;
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, dcp_ext_audio_stop, dcp), 0);
	return dcp;
}

/*
 * Probe leaves the work disabled: a first EDID before bind queues nothing,
 * and bind's single enable is balanced. Suspend and resume, unbind and a
 * second bind keep it so.
 */
static void dcp_ext_audio_lifecycle(struct kunit *test)
{
	struct apple_dcp *dcp = dcp_ext_audio_probe(test, true);

	KUNIT_EXPECT_EQ(test, dcp_ext_audio_start_runs(dcp), 0U);
	/* bind */
	KUNIT_EXPECT_TRUE(test, enable_work(&dcp->external_audio_work));
	KUNIT_EXPECT_EQ(test, dcp_ext_audio_start_runs(dcp), 1U);
	/* suspend, then resume */
	disable_work_sync(&dcp->external_audio_work);
	KUNIT_EXPECT_EQ(test, dcp_ext_audio_start_runs(dcp), 0U);
	KUNIT_EXPECT_TRUE(test, enable_work(&dcp->external_audio_work));
	/* unbind, then bind again */
	disable_work_sync(&dcp->external_audio_work);
	KUNIT_EXPECT_TRUE(test, enable_work(&dcp->external_audio_work));
	KUNIT_EXPECT_EQ(test, dcp_ext_audio_start_runs(dcp), 1U);
}

/* Only a native external pipe starts it, and only once per session. */
static void dcp_ext_audio_start_gates(struct kunit *test)
{
	struct apple_dcp *other = dcp_ext_audio_probe(test, false);
	struct apple_dcp *dcp = dcp_ext_audio_probe(test, true);

	KUNIT_EXPECT_TRUE(test, enable_work(&other->external_audio_work));
	KUNIT_EXPECT_EQ(test, dcp_ext_audio_start_runs(other), 0U);
	KUNIT_EXPECT_TRUE(test, enable_work(&dcp->external_audio_work));
	dcp->external_audio_started = true;
	KUNIT_EXPECT_EQ(test, dcp_ext_audio_start_runs(dcp), 0U);
}

static struct kunit_case dcp_ext_audio_cases[] = {
	KUNIT_CASE(dcp_ext_audio_lifecycle),
	KUNIT_CASE(dcp_ext_audio_start_gates),
	{}
};

static struct kunit_suite dcp_ext_audio_suite = {
	.name = "apple-dcp-external-audio-work",
	.test_cases = dcp_ext_audio_cases,
};

kunit_test_suite(dcp_ext_audio_suite);
