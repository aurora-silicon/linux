// SPDX-License-Identifier: GPL-2.0-only
#include <kunit/test.h>
#include <linux/completion.h>
#include <linux/errno.h>
#include <linux/kthread.h>
#include <linux/module.h>
#include <linux/sched/task.h>

#include "sdhci-pci-gli-pwrseq.h"

struct gli_mock {
	struct kunit *test;
	struct gli_pwrseq seq;
	/* Models the outer PCI rescan/remove lock for concurrent events. */
	struct mutex bus_lock;
	bool bus_locked;
	bool rail;
	bool port;
	bool config;
	bool endpoint;
	int present;
	char fail;
	int failures;
	bool insert_on_remove;
	char log[256];
	unsigned int count;
};

static void gli_record(struct gli_mock *m, char op)
{
	KUNIT_EXPECT_TRUE(m->test, m->bus_locked);
	KUNIT_EXPECT_TRUE(m->test, mutex_is_locked(&m->seq.lock));
	KUNIT_EXPECT_TRUE(m->test, !m->port || m->rail);
	KUNIT_EXPECT_TRUE(m->test, !m->config || (m->port && m->rail));
	if (m->count < sizeof(m->log) - 1)
		m->log[m->count++] = op;
}

static int gli_fail(struct gli_mock *m, char op)
{
	if (m->fail != op || !m->failures)
		return 0;
	m->failures--;
	return -EIO;
}

static void gli_mock_lock(void *ctx)
{
	struct gli_mock *m = ctx;

	mutex_lock(&m->bus_lock);
	KUNIT_EXPECT_FALSE(m->test, m->bus_locked);
	m->bus_locked = true;
}

static void gli_mock_unlock(void *ctx)
{
	struct gli_mock *m = ctx;

	KUNIT_EXPECT_FALSE(m->test, mutex_is_locked(&m->seq.lock));
	KUNIT_EXPECT_TRUE(m->test, m->bus_locked);
	m->bus_locked = false;
	mutex_unlock(&m->bus_lock);
}

static int gli_mock_rail(void *ctx, bool on)
{
	struct gli_mock *m = ctx;
	char op = on ? 'R' : 'r';
	int ret;

	gli_record(m, op);
	KUNIT_EXPECT_EQ(m->test, m->seq.state,
			on ? GLI_POWERING : GLI_QUIESCING);
	if (!on) {
		KUNIT_EXPECT_FALSE(m->test, m->port);
		KUNIT_EXPECT_FALSE(m->test, m->config);
		KUNIT_EXPECT_FALSE(m->test, m->endpoint);
	}
	ret = gli_fail(m, op);
	/* Failed enables may have changed hardware, failed disables may not. */
	if (on || !ret)
		m->rail = on;
	return ret;
}

static int gli_mock_port(void *ctx, bool on)
{
	struct gli_mock *m = ctx;
	char op = on ? 'P' : 'p';
	int ret;

	gli_record(m, op);
	KUNIT_EXPECT_TRUE(m->test, m->rail);
	KUNIT_EXPECT_EQ(m->test, m->seq.state,
			on ? GLI_POWERING : GLI_QUIESCING);
	if (!on) {
		KUNIT_EXPECT_FALSE(m->test, m->config);
		KUNIT_EXPECT_FALSE(m->test, m->endpoint);
	}
	ret = gli_fail(m, op);
	if (on || !ret)
		m->port = on;
	return ret;
}

static void gli_mock_config(void *ctx, bool allow)
{
	struct gli_mock *m = ctx;

	gli_record(m, allow ? 'C' : 'c');
	if (allow) {
		KUNIT_EXPECT_TRUE(m->test, m->rail && m->port);
		KUNIT_EXPECT_EQ(m->test, m->seq.state, GLI_ENUMERATING);
	}
	m->config = allow;
}

static int gli_mock_rescan(void *ctx)
{
	struct gli_mock *m = ctx;

	gli_record(m, 'E');
	KUNIT_EXPECT_TRUE(m->test, m->config);
	KUNIT_EXPECT_EQ(m->test, m->seq.state, GLI_ENUMERATING);
	m->endpoint = true;
	return gli_fail(m, 'E');
}

static int gli_mock_media(void *ctx)
{
	struct gli_mock *m = ctx;

	gli_record(m, 'M');
	KUNIT_EXPECT_TRUE(m->test, m->config && m->endpoint);
	return m->present;
}

static int gli_mock_remove(void *ctx)
{
	struct gli_mock *m = ctx;
	int ret;

	gli_record(m, 'Q');
	KUNIT_EXPECT_TRUE(m->test, m->config);
	KUNIT_EXPECT_EQ(m->test, m->seq.state, GLI_QUIESCING);
	if (m->insert_on_remove)
		m->present = 1;
	if (m->present > 0)
		return -EBUSY;
	ret = gli_fail(m, 'Q');
	if (!ret)
		m->endpoint = false;
	return ret;
}

static int gli_mock_gen2(void *ctx)
{
	struct gli_mock *m = ctx;

	gli_record(m, 'G');
	KUNIT_EXPECT_TRUE(m->test, m->config && m->endpoint);
	KUNIT_EXPECT_EQ(m->test, m->seq.state, GLI_ON_MEDIA);
	return gli_fail(m, 'G');
}

static void gli_mock_delay(void *ctx, unsigned int ms)
{
	struct gli_mock *m = ctx;

	gli_record(m, ms == GLI_PWRSEQ_ON_MS ? 's' : 'd');
	if (ms == GLI_PWRSEQ_ON_MS) {
		KUNIT_EXPECT_TRUE(m->test, m->rail);
		KUNIT_EXPECT_FALSE(m->test, m->port);
		KUNIT_EXPECT_EQ(m->test, m->seq.state, GLI_POWERING);
	} else {
		KUNIT_EXPECT_EQ(m->test, ms, GLI_PWRSEQ_OFF_MS);
		KUNIT_EXPECT_FALSE(m->test, m->rail || m->port || m->config);
		KUNIT_EXPECT_EQ(m->test, m->seq.state, GLI_QUIESCING);
	}
}

static const struct gli_pwrseq_ops gli_mock_ops = {
	.bus_lock = gli_mock_lock,
	.bus_unlock = gli_mock_unlock,
	.rail = gli_mock_rail,
	.port = gli_mock_port,
	.config = gli_mock_config,
	.rescan = gli_mock_rescan,
	.media = gli_mock_media,
	.remove = gli_mock_remove,
	.gen2 = gli_mock_gen2,
	.delay = gli_mock_delay,
};

static int gli_test_init(struct kunit *test)
{
	struct gli_mock *m = kunit_kzalloc(test, sizeof(*m), GFP_KERNEL);

	if (!m)
		return -ENOMEM;
	m->test = test;
	mutex_init(&m->bus_lock);
	gli_pwrseq_init(&m->seq, &gli_mock_ops, m);
	test->priv = m;
	return 0;
}

static void gli_expect_off(struct kunit *test)
{
	struct gli_mock *m = test->priv;

	KUNIT_EXPECT_EQ(test, m->seq.state, GLI_OFF_NO_MEDIA);
	KUNIT_EXPECT_FALSE(test, m->rail || m->port || m->config || m->endpoint);
	KUNIT_EXPECT_FALSE(test, m->seq.rail_on || m->seq.port_on || m->seq.endpoint);
}

static void gli_insert_remove_test(struct kunit *test)
{
	struct gli_mock *m = test->priv;

	m->present = 1;
	KUNIT_ASSERT_EQ(test, gli_pwrseq_event(&m->seq, GLI_HELPER_INSERT), 0);
	KUNIT_EXPECT_EQ(test, m->seq.state, GLI_ON_MEDIA);
	KUNIT_EXPECT_STREQ(test, m->log, "RsPCEMG");
	/* Duplicate wake and stale removal indications cannot drop media. */
	KUNIT_EXPECT_EQ(test, gli_pwrseq_event(&m->seq, GLI_HELPER_INSERT), 0);
	KUNIT_EXPECT_EQ(test, gli_pwrseq_event(&m->seq, GLI_MEDIA_CHANGE), 0);
	KUNIT_EXPECT_STREQ(test, m->log, "RsPCEMGM");
	m->present = 0;
	KUNIT_EXPECT_EQ(test, gli_pwrseq_event(&m->seq, GLI_MEDIA_CHANGE), 0);
	KUNIT_EXPECT_STREQ(test, m->log, "RsPCEMGMMQcprd");
	gli_expect_off(test);
	/* A fresh helper event can enumerate again after removal. */
	m->present = 1;
	KUNIT_EXPECT_EQ(test, gli_pwrseq_event(&m->seq, GLI_HELPER_INSERT), 0);
	KUNIT_EXPECT_EQ(test, m->seq.state, GLI_ON_MEDIA);
}

static void gli_empty_wake_test(struct kunit *test)
{
	struct gli_mock *m = test->priv;

	KUNIT_EXPECT_EQ(test, gli_pwrseq_event(&m->seq, GLI_MEDIA_CHANGE), 0);
	KUNIT_EXPECT_STREQ(test, m->log, "");
	KUNIT_EXPECT_EQ(test, gli_pwrseq_event(&m->seq, GLI_HELPER_INSERT), 0);
	KUNIT_EXPECT_STREQ(test, m->log, "RsPCEMQcprd");
	gli_expect_off(test);
}

static void gli_retry_test(struct kunit *test)
{
	struct gli_mock *m = test->priv;
	const char failures[] = { 'R', 'P', 'E' };
	const char * const logs[] = {
		"RcrdRcrdRcrd",
		"RsPcprdRsPcprdRsPcprd",
		"RsPCEQcprdRsPCEQcprdRsPCEQcprd",
	};
	int i;

	for (i = 0; i < ARRAY_SIZE(failures); i++) {
		m->fail = failures[i];
		m->failures = GLI_PWRSEQ_ATTEMPTS;
		m->count = 0;
		memset(m->log, 0, sizeof(m->log));
		KUNIT_EXPECT_EQ(test, gli_pwrseq_event(&m->seq, GLI_HELPER_INSERT), -EIO);
		KUNIT_EXPECT_EQ(test, m->failures, 0);
		KUNIT_EXPECT_STREQ(test, m->log, logs[i]);
		gli_expect_off(test);
	}
}

static void gli_retry_success_test(struct kunit *test)
{
	struct gli_mock *m = test->priv;

	m->fail = 'P';
	m->failures = 1;
	m->present = 1;
	KUNIT_EXPECT_EQ(test, gli_pwrseq_event(&m->seq, GLI_HELPER_INSERT), 0);
	KUNIT_EXPECT_STREQ(test, m->log, "RsPcprdRsPCEMG");
	KUNIT_EXPECT_EQ(test, m->seq.state, GLI_ON_MEDIA);
}

static void gli_gen2_failure_test(struct kunit *test)
{
	struct gli_mock *m = test->priv;

	m->present = 1;
	m->fail = 'G';
	m->failures = 1;
	KUNIT_EXPECT_EQ(test, gli_pwrseq_event(&m->seq, GLI_HELPER_INSERT), 0);
	KUNIT_EXPECT_EQ(test, m->seq.state, GLI_ON_MEDIA);
	KUNIT_EXPECT_TRUE(test, m->rail && m->port && m->endpoint);
	KUNIT_EXPECT_STREQ(test, m->log, "RsPCEMG");
}

static void gli_unknown_media_test(struct kunit *test)
{
	struct gli_mock *m = test->priv;

	m->present = -EIO;
	KUNIT_EXPECT_EQ(test, gli_pwrseq_event(&m->seq, GLI_HELPER_INSERT), -EIO);
	KUNIT_EXPECT_EQ(test, gli_pwrseq_event(&m->seq, GLI_MEDIA_CHANGE), -EIO);
	KUNIT_EXPECT_EQ(test, m->seq.state, GLI_ON_MEDIA);
	KUNIT_EXPECT_STREQ(test, m->log, "RsPCEMM");
}

static void gli_reinsert_race_test(struct kunit *test)
{
	struct gli_mock *m = test->priv;

	m->insert_on_remove = true;
	KUNIT_EXPECT_EQ(test, gli_pwrseq_event(&m->seq, GLI_HELPER_INSERT), -EBUSY);
	KUNIT_EXPECT_EQ(test, m->seq.state, GLI_ON_MEDIA);
	KUNIT_EXPECT_STREQ(test, m->log, "RsPCEMQ");
	KUNIT_EXPECT_TRUE(test, m->rail && m->port && m->endpoint);
	/* Repeat the reinsertion race after a confirmed ON_MEDIA removal. */
	m->present = 0;
	KUNIT_EXPECT_EQ(test, gli_pwrseq_event(&m->seq, GLI_MEDIA_CHANGE), -EBUSY);
	KUNIT_EXPECT_EQ(test, m->seq.state, GLI_ON_MEDIA);
	KUNIT_EXPECT_STREQ(test, m->log, "RsPCEMQMQ");
}

static void gli_remove_failure_test(struct kunit *test)
{
	struct gli_mock *m = test->priv;

	m->fail = 'Q';
	m->failures = 1;
	KUNIT_EXPECT_EQ(test, gli_pwrseq_event(&m->seq, GLI_HELPER_INSERT), -EIO);
	KUNIT_EXPECT_EQ(test, m->seq.state, GLI_ON_MEDIA);
	KUNIT_EXPECT_STREQ(test, m->log, "RsPCEMQ");
	KUNIT_EXPECT_EQ(test, gli_pwrseq_event(&m->seq, GLI_MEDIA_CHANGE), 0);
	gli_expect_off(test);
}

static void gli_partial_rescan_media_test(struct kunit *test)
{
	struct gli_mock *m = test->priv;

	/* A failed rescan may already have exposed media to the MMC layer. */
	m->present = 1;
	m->fail = 'E';
	m->failures = GLI_PWRSEQ_ATTEMPTS;
	KUNIT_EXPECT_EQ(test, gli_pwrseq_event(&m->seq, GLI_HELPER_INSERT), -EBUSY);
	KUNIT_EXPECT_EQ(test, m->seq.state, GLI_ON_MEDIA);
	KUNIT_EXPECT_STREQ(test, m->log, "RsPCEQ");
	KUNIT_EXPECT_EQ(test, m->failures, GLI_PWRSEQ_ATTEMPTS - 1);
}

static void gli_shutdown_failure_test(struct kunit *test)
{
	struct gli_mock *m = test->priv;
	const char failures[] = { 'p', 'r' };
	int i;

	for (i = 0; i < ARRAY_SIZE(failures); i++) {
		m->fail = failures[i];
		m->failures = 1;
		KUNIT_EXPECT_EQ(test, gli_pwrseq_event(&m->seq, GLI_HELPER_INSERT), -EIO);
		KUNIT_EXPECT_EQ(test, m->seq.state, GLI_QUIESCING);
		KUNIT_EXPECT_TRUE(test, m->rail);
		KUNIT_EXPECT_FALSE(test, m->config);
		KUNIT_EXPECT_EQ(test, gli_pwrseq_event(&m->seq, GLI_SUSPEND), -EBUSY);
		KUNIT_EXPECT_EQ(test, gli_pwrseq_event(&m->seq, GLI_MEDIA_CHANGE), 0);
		gli_expect_off(test);
	}
}

static void gli_shutdown_pending_wake_test(struct kunit *test)
{
	struct gli_mock *m = test->priv;

	m->fail = 'r';
	m->failures = 1;
	KUNIT_EXPECT_EQ(test, gli_pwrseq_event(&m->seq, GLI_HELPER_INSERT), -EIO);
	KUNIT_EXPECT_EQ(test, gli_pwrseq_event(&m->seq, GLI_HELPER_INSERT), 0);
	KUNIT_EXPECT_TRUE(test, m->seq.pending_insert);
	m->present = 1;
	KUNIT_EXPECT_EQ(test, gli_pwrseq_event(&m->seq, GLI_MEDIA_CHANGE), 0);
	KUNIT_EXPECT_FALSE(test, m->seq.pending_insert);
	KUNIT_EXPECT_EQ(test, m->seq.state, GLI_ON_MEDIA);
	KUNIT_EXPECT_STREQ(test, m->log, "RsPCEMQcprcrdRsPCEMG");
}

struct gli_worker {
	struct gli_pwrseq *seq;
	enum gli_pwrseq_event event;
	struct completion done;
	int ret;
};

static int gli_event_thread(void *data)
{
	struct gli_worker *worker = data;

	worker->ret = gli_pwrseq_event(worker->seq, worker->event);
	complete(&worker->done);
	return 0;
}

static void gli_concurrent_events_test(struct kunit *test)
{
	struct gli_mock *m = test->priv;
	struct gli_worker workers[3];
	struct task_struct *threads[3];
	int i, started = 0;

	/* Stale removal and two helper events race; media must stay powered. */
	m->present = 1;
	for (i = 0; i < ARRAY_SIZE(workers); i++) {
		workers[i].seq = &m->seq;
		workers[i].event = i ? GLI_HELPER_INSERT : GLI_MEDIA_CHANGE;
		init_completion(&workers[i].done);
		threads[i] = kthread_create(gli_event_thread, &workers[i], "gli-event");
		if (IS_ERR(threads[i])) {
			KUNIT_FAIL(test, "could not start event thread");
			break;
		}
		get_task_struct(threads[i]);
		wake_up_process(threads[i]);
		started++;
	}
	for (i = 0; i < started; i++) {
		wait_for_completion(&workers[i].done);
		kthread_stop(threads[i]);
		put_task_struct(threads[i]);
		KUNIT_EXPECT_EQ(test, workers[i].ret, 0);
	}
	KUNIT_EXPECT_EQ(test, m->seq.state, GLI_ON_MEDIA);
	/* Only one bring-up, regardless of which event won the locks. */
	KUNIT_EXPECT_EQ(test, m->count, m->log[7] ? 8 : 7);
	KUNIT_EXPECT_EQ(test, m->log[0], 'R');
	KUNIT_EXPECT_EQ(test, m->log[6], 'G');
	KUNIT_EXPECT_TRUE(test, m->rail && m->port && m->endpoint);
}

static void gli_suspend_test(struct kunit *test)
{
	struct gli_mock *m = test->priv;

	KUNIT_EXPECT_EQ(test, gli_pwrseq_event(&m->seq, GLI_SUSPEND), 0);
	KUNIT_EXPECT_EQ(test, gli_pwrseq_event(&m->seq, GLI_RESUME), 0);
	gli_expect_off(test);
	KUNIT_EXPECT_STREQ(test, m->log, "");
	KUNIT_EXPECT_EQ(test, gli_pwrseq_event(&m->seq, GLI_SUSPEND), 0);
	KUNIT_EXPECT_EQ(test, gli_pwrseq_event(&m->seq, GLI_HELPER_INSERT), 0);
	KUNIT_EXPECT_TRUE(test, m->seq.pending_insert);
	KUNIT_EXPECT_EQ(test, gli_pwrseq_event(&m->seq, GLI_MEDIA_CHANGE), -EBUSY);
	KUNIT_EXPECT_STREQ(test, m->log, "");
	m->present = 1;
	KUNIT_EXPECT_EQ(test, gli_pwrseq_event(&m->seq, GLI_RESUME), 0);
	KUNIT_EXPECT_FALSE(test, m->seq.pending_insert);
	KUNIT_EXPECT_EQ(test, m->seq.state, GLI_ON_MEDIA);
	KUNIT_EXPECT_STREQ(test, m->log, "RsPCEMG");
	KUNIT_EXPECT_EQ(test, gli_pwrseq_event(&m->seq, GLI_SUSPEND), 0);
	KUNIT_EXPECT_EQ(test, gli_pwrseq_event(&m->seq, GLI_MEDIA_CHANGE), -EBUSY);
	KUNIT_EXPECT_EQ(test, gli_pwrseq_event(&m->seq, GLI_RESUME), 0);
	KUNIT_EXPECT_STREQ(test, m->log, "RsPCEMG");
}

static void gli_invalid_event_test(struct kunit *test)
{
	struct gli_mock *m = test->priv;

	KUNIT_EXPECT_EQ(test, gli_pwrseq_event(&m->seq, -1), -EINVAL);
	gli_expect_off(test);
	KUNIT_EXPECT_FALSE(test, m->bus_locked);
}

static struct kunit_case gli_pwrseq_cases[] = {
	KUNIT_CASE(gli_insert_remove_test),
	KUNIT_CASE(gli_empty_wake_test),
	KUNIT_CASE(gli_retry_test),
	KUNIT_CASE(gli_retry_success_test),
	KUNIT_CASE(gli_gen2_failure_test),
	KUNIT_CASE(gli_unknown_media_test),
	KUNIT_CASE(gli_reinsert_race_test),
	KUNIT_CASE(gli_remove_failure_test),
	KUNIT_CASE(gli_partial_rescan_media_test),
	KUNIT_CASE(gli_shutdown_failure_test),
	KUNIT_CASE(gli_shutdown_pending_wake_test),
	KUNIT_CASE(gli_concurrent_events_test),
	KUNIT_CASE(gli_suspend_test),
	KUNIT_CASE(gli_invalid_event_test),
	{}
};

static struct kunit_suite gli_pwrseq_suite = {
	.name = "sdhci-pci-gli-pwrseq",
	.init = gli_test_init,
	.test_cases = gli_pwrseq_cases,
};

kunit_test_suite(gli_pwrseq_suite);

MODULE_DESCRIPTION("GL9755 board power sequencing tests");
MODULE_LICENSE("GPL");
