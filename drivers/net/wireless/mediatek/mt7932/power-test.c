// SPDX-License-Identifier: GPL-2.0-only
#include <kunit/test.h>

#include "mt7932.h"

struct mt_power_test_context {
	struct kunit *test;
	struct mt7932 *m;
	unsigned int commands;
	u8 mode;
	int error;
};

static int mt_power_test_send(struct mt7932 *m, const void *data, size_t length)
{
	struct mt_power_test_context *ctx = m->power_test_context;
	const u8 *body = data;

	lockdep_assert_held(&m->command_mutex);
	KUNIT_EXPECT_EQ(ctx->test, length, (size_t)4);
	if (length != 4)
		return -EINVAL;
	KUNIT_EXPECT_EQ(ctx->test, body[0], 0); /* owned BSS0 */
	KUNIT_EXPECT_EQ(ctx->test, body[2], 0);
	KUNIT_EXPECT_EQ(ctx->test, body[3], 0);
	ctx->commands++;
	ctx->mode = body[1];
	return ctx->error;
}

static void mt_power_test_device_release(struct device *dev)
{
}

static void mt_power_test_cleanup(void *data)
{
	struct mt7932 *m = data;

	cancel_work_sync(&m->power_work);
	put_device(&m->pdev->dev);
}

static int mt_power_test_init(struct kunit *test)
{
	struct mt_power_test_context *ctx;
	struct mt7932 *m;
	int ret;

	ctx = kunit_kzalloc(test, sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;
	m = kunit_kzalloc(test, sizeof(*m), GFP_KERNEL);
	if (!m)
		return -ENOMEM;
	m->pdev = kunit_kzalloc(test, sizeof(*m->pdev), GFP_KERNEL);
	if (!m->pdev)
		return -ENOMEM;
	device_initialize(&m->pdev->dev);
	m->pdev->dev.release = mt_power_test_device_release;
	ret = dev_set_name(&m->pdev->dev, "mt7932-power-test");
	if (ret) {
		put_device(&m->pdev->dev);
		return ret;
	}
	mutex_init(&m->command_mutex);
	spin_lock_init(&m->response_lock);
	INIT_WORK(&m->power_work, mt_power_work);
	ret = kunit_add_action_or_reset(test, mt_power_test_cleanup, m);
	if (ret)
		return ret;
	m->rf_ready = true;
	m->peer_valid = true;
	m->power_test_send = mt_power_test_send;
	m->power_test_context = ctx;
	ctx->m = m;
	ctx->test = test;
	test->priv = ctx;
	return 0;
}

static int mt_power_test_prejoin(struct mt7932 *m)
{
	int ret;

	mutex_lock(&m->command_mutex);
	ret = mt_power_init(m);
	mutex_unlock(&m->command_mutex);
	return ret;
}

static void mt_power_test_run(struct mt7932 *m)
{
	schedule_work(&m->power_work);
	flush_work(&m->power_work);
}

static void mt_power_awake_after_join_test(struct kunit *test)
{
	struct mt_power_test_context *ctx = test->priv;
	struct mt7932 *m = ctx->m;
	unsigned int join;

	/* The old cache skipped the second SET in every mode-0 association.
	 * Exercise reconnect too: each session must submit both phases again.
	 */
	for (join = 0; join < 2; join++) {
		m->connected = false;
		KUNIT_ASSERT_EQ(test, mt_power_test_prejoin(m), 0);
		KUNIT_EXPECT_EQ(test, ctx->commands, join * 2 + 1);
		KUNIT_EXPECT_EQ(test, ctx->mode, 0);
		KUNIT_EXPECT_FALSE(test, m->power_applied_valid);
		m->connected = true;
		mt_power_test_run(m);
		KUNIT_EXPECT_EQ(test, ctx->commands, join * 2 + 2);
		KUNIT_EXPECT_EQ(test, ctx->mode, 0);
		KUNIT_EXPECT_TRUE(test, m->power_applied_valid);
		/* Deduplication remains valid within this associated session. */
		mt_power_test_run(m);
		KUNIT_EXPECT_EQ(test, ctx->commands, join * 2 + 2);
	}
}

static void mt_power_tim_gates_sleep_test(struct kunit *test)
{
	struct mt_power_test_context *ctx = test->priv;
	struct mt7932 *m = ctx->m;
	unsigned int mode;

	for (mode = 1; mode <= 2; mode++) {
		m->power_mode = mode;
		KUNIT_ASSERT_EQ(test, mt_power_test_prejoin(m), 0);
		KUNIT_EXPECT_FALSE(test, m->power_tim);
		m->connected = true;
		mt_power_test_run(m);
		KUNIT_EXPECT_EQ(test, ctx->commands, mode * 2 - 1);
		KUNIT_EXPECT_FALSE(test, m->power_applied_valid);
		m->power_tim = true;
		mt_power_test_run(m);
		KUNIT_EXPECT_EQ(test, ctx->commands, mode * 2);
		KUNIT_EXPECT_EQ(test, ctx->mode, mode);
		KUNIT_EXPECT_EQ(test, m->power_applied, mode);
	}
}

static void mt_power_failed_submission_retry_test(struct kunit *test)
{
	struct mt_power_test_context *ctx = test->priv;
	struct mt7932 *m = ctx->m;

	ctx->error = -EIO;
	KUNIT_EXPECT_EQ(test, mt_power_test_prejoin(m), -EIO);
	KUNIT_EXPECT_FALSE(test, m->power_applied_valid);
	ctx->error = 0;
	KUNIT_ASSERT_EQ(test, mt_power_test_prejoin(m), 0);
	m->connected = true;
	ctx->error = -EIO;
	mt_power_test_run(m);
	KUNIT_EXPECT_FALSE(test, m->power_applied_valid);
	ctx->error = 0;
	mt_power_test_run(m);
	KUNIT_EXPECT_EQ(test, ctx->commands, 4U);
	KUNIT_EXPECT_TRUE(test, m->power_applied_valid);
}

static void mt_power_terminal_session_test(struct kunit *test)
{
	struct mt_power_test_context *ctx = test->priv;
	struct mt7932 *m = ctx->m;
	unsigned int state;

	KUNIT_ASSERT_EQ(test, mt_power_test_prejoin(m), 0);
	for (state = 0; state < 6; state++) {
		m->stopping = state == 0;
		m->rf_ready = state != 1;
		m->connected = state != 2;
		m->disconnecting = state == 3;
		m->peer_valid = state != 4;
		m->link_failed = state == 5;
		mt_power_test_run(m);
		KUNIT_EXPECT_EQ(test, ctx->commands, 1U);
		KUNIT_EXPECT_FALSE(test, m->power_applied_valid);
	}
}

static struct kunit_case mt_power_test_cases[] = {
	KUNIT_CASE(mt_power_awake_after_join_test),
	KUNIT_CASE(mt_power_tim_gates_sleep_test),
	KUNIT_CASE(mt_power_failed_submission_retry_test),
	KUNIT_CASE(mt_power_terminal_session_test),
	{}
};

static struct kunit_suite mt_power_test_suite = {
	.name = "mt7932-power",
	.init = mt_power_test_init,
	.test_cases = mt_power_test_cases,
};

kunit_test_suite(mt_power_test_suite);
