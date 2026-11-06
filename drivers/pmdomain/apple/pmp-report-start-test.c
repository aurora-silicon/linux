// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * KUnit fault injection for starting a halted PMP: the start's checks, the
 * enabling of the PMP nodes and the PMP driver's bind are replaced, and the
 * report region is memory, so each failure after the PMP was enabled (no
 * bind, no ready status, no acknowledgment), a consumer's own timeout and
 * the teardown of the provider can be driven without Apple hardware.
 *
 * Included by pmp-report.c.
 */

#include <kunit/device.h>
#include <kunit/test.h>
#include <linux/kthread.h>

/* Two requests: bit 0 acknowledged at startup, bit 5 seeded without ack. */
#define PST_SEED	(BIT_ULL(0) | BIT_ULL(5))
#define PST_ACK		BIT_ULL(0)

static const struct apple_pmp_report_offsets pst_offsets = {
	.tgt_read = 0x00,
	.tgt_write = 0x08,
	.actual = 0x10,
	.status = 0x18,
	.starts_pmp = true,
	.name = "TEST",
	.fallback = "test.enable=0",
};

static const struct apple_pmp_start_timing pst_timing = {
	.bind = 200,
	.quiet = 0,
	.ready = 200,
	.ack = 100,
};

/* How the simulated PMP behaves. */
struct pst_pmp {
	int check;		/* result of the checks before the start */
	int enable;		/* result of enabling the PMP nodes */
	bool binds;		/* the PMP driver binds */
	unsigned int bind_ms;	/* ... this long after the nodes are enabled */
	bool ready;		/* once bound, the PTD reports ready */
	u64 ack;		/* ... and these requests are acknowledged */
	unsigned long enabled_at;
	unsigned int enables;
};

struct pst {
	struct apple_pmp_report rep;
	struct apple_pmp_start st;
	struct apple_pmp_start_ops ops;
	struct pst_pmp pmp;
	u64 *regs;
};

static struct pst *pst_of(struct apple_pmp_start *st)
{
	return container_of(st, struct pst, st);
}

static void pst_write(struct pst *t, u32 offset, u64 val)
{
	WRITE_ONCE(t->regs[offset / sizeof(u64)], val);
}

static int pst_check(struct apple_pmp_start *st, uuid_t *image)
{
	uuid_gen(image);
	return pst_of(st)->pmp.check;
}

static int pst_enable(struct apple_pmp_start *st)
{
	struct pst *t = pst_of(st);

	t->pmp.enables++;
	t->pmp.enabled_at = jiffies;
	return t->pmp.enable;
}

static bool pst_bound(struct apple_pmp_start *st)
{
	struct pst *t = pst_of(st);

	if (!t->pmp.binds ||
	    time_before(jiffies, t->pmp.enabled_at + msecs_to_jiffies(t->pmp.bind_ms)))
		return false;
	/* The firmware is up: it reports what this test lets it. */
	if (t->pmp.ready)
		pst_write(t, pst_offsets.status, PMP_REPORT_READY);
	pst_write(t, pst_offsets.actual, t->pmp.ack);
	return true;
}

/* A report whose halted PMP is handed over, with every request seeded. */
static struct pst *pst_new(struct kunit *test)
{
	struct device *dev = kunit_device_register(test, "apple-pmp-start-test");
	struct pst *t;

	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, dev);
	t = kunit_kzalloc(test, sizeof(*t), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, t);
	t->regs = kunit_kzalloc(test, 4 * sizeof(u64), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, t->regs);

	t->rep.dev = dev;
	t->rep.offsets = &pst_offsets;
	t->rep.base = (__force void __iomem *)t->regs;
	spin_lock_init(&t->rep.lock);
	INIT_LIST_HEAD(&t->rep.list);

	t->ops.check = pst_check;
	t->ops.enable = pst_enable;
	t->ops.bound = pst_bound;
	t->st.rep = &t->rep;
	t->st.ops = &t->ops;
	t->st.timing = &pst_timing;
	t->st.seed = PST_SEED;
	t->st.ack = PST_ACK;
	t->st.startup_ack = PST_ACK;
	INIT_LIST_HEAD(&t->st.list);
	INIT_WORK(&t->st.work, apple_pmp_start_work);
	init_completion(&t->st.started);
	init_completion(&t->st.waiters_done);
	t->rep.start = &t->st;

	/* The PTD as the start finds it: requests seeded, PMP halted. */
	pst_write(t, pst_offsets.tgt_read, PST_SEED);

	/* A PMP that starts cleanly, unless a test says otherwise. */
	t->pmp.binds = true;
	t->pmp.ready = true;
	t->pmp.ack = PST_ACK;
	return t;
}

/* As apple_pmp_report_wait_ready() does once it has found the start. */
static int pst_wait(struct pst *t, u32 id, unsigned int timeout_ms)
{
	scoped_guard(mutex, &apple_pmp_start_mutex) {
		if (t->st.stopping)
			return -ENODEV;
		t->st.waiters++;
	}
	return apple_pmp_start_wait(&t->st, id, msecs_to_jiffies(timeout_ms));
}

static void pst_drain(struct pst *t)
{
	apple_pmp_start_drain(&t->st);
}

static void pmp_start_ok_test(struct kunit *test)
{
	struct pst *t = pst_new(test);

	KUNIT_EXPECT_EQ(test, pst_wait(t, 0, 5000), 0);
	KUNIT_EXPECT_EQ(test, t->pmp.enables, 1U);
	KUNIT_EXPECT_TRUE(test, READ_ONCE(t->st.enabled));
	/* Later consumers neither start it again nor wait. */
	KUNIT_EXPECT_EQ(test, pst_wait(t, 0, 1), 0);
	KUNIT_EXPECT_EQ(test, t->pmp.enables, 1U);
	/* A request the PMP does not acknowledge cannot be waited for. */
	KUNIT_EXPECT_EQ(test, pst_wait(t, 5, 1), -EINVAL);
	pst_drain(t);
}

/* A failed check leaves the PMP halted: nothing was enabled. */
static void pmp_start_check_fails_test(struct kunit *test)
{
	struct pst *t = pst_new(test);

	t->pmp.check = -EIO;
	KUNIT_EXPECT_EQ(test, pst_wait(t, 0, 5000), -EIO);
	KUNIT_EXPECT_EQ(test, t->pmp.enables, 0U);
	KUNIT_EXPECT_FALSE(test, READ_ONCE(t->st.enabled));
	pst_drain(t);
}

/* A PTD that already has a running status is not started over. */
static void pmp_start_busy_ptd_test(struct kunit *test)
{
	struct pst *t = pst_new(test);

	pst_write(t, pst_offsets.status, PMP_REPORT_READY);
	KUNIT_EXPECT_EQ(test, pst_wait(t, 0, 5000), -EBUSY);
	KUNIT_EXPECT_EQ(test, t->pmp.enables, 0U);
	pst_drain(t);
}

static void pmp_start_enable_fails_test(struct kunit *test)
{
	struct pst *t = pst_new(test);

	t->pmp.enable = -EBUSY;
	KUNIT_EXPECT_EQ(test, pst_wait(t, 0, 5000), -EBUSY);
	KUNIT_EXPECT_EQ(test, t->pmp.enables, 1U);
	KUNIT_EXPECT_FALSE(test, READ_ONCE(t->st.enabled));
	pst_drain(t);
}

/* The PMP driver never binds: a timeout, with the PMP left enabled. */
static void pmp_start_bind_timeout_test(struct kunit *test)
{
	struct pst *t = pst_new(test);

	t->pmp.binds = false;
	KUNIT_EXPECT_EQ(test, pst_wait(t, 0, 5000), -ETIMEDOUT);
	KUNIT_EXPECT_TRUE(test, READ_ONCE(t->st.enabled));
	/* The result stands for every later consumer. */
	KUNIT_EXPECT_EQ(test, pst_wait(t, 0, 1), -ETIMEDOUT);
	pst_drain(t);
}

/* Bound, but the PTD never reports ready. */
static void pmp_start_ready_timeout_test(struct kunit *test)
{
	struct pst *t = pst_new(test);

	t->pmp.ready = false;
	KUNIT_EXPECT_EQ(test, pst_wait(t, 0, 5000), -ETIMEDOUT);
	KUNIT_EXPECT_TRUE(test, READ_ONCE(t->st.enabled));
	pst_drain(t);
}

/* Ready, but the startup request is not acknowledged. */
static void pmp_start_ack_fails_test(struct kunit *test)
{
	struct pst *t = pst_new(test);

	t->pmp.ack = 0;
	KUNIT_EXPECT_EQ(test, pst_wait(t, 0, 5000), -EIO);
	KUNIT_EXPECT_TRUE(test, READ_ONCE(t->st.enabled));
	pst_drain(t);
}

/* Acknowledged at startup, withdrawn by the time a consumer asks. */
static void pmp_start_ack_withdrawn_test(struct kunit *test)
{
	struct pst *t = pst_new(test);

	KUNIT_EXPECT_EQ(test, pst_wait(t, 0, 5000), 0);
	pst_write(t, pst_offsets.actual, 0);
	KUNIT_EXPECT_EQ(test, pst_wait(t, 0, 1), -EIO);
	pst_write(t, pst_offsets.actual, PST_ACK);
	pst_write(t, pst_offsets.status, 0);
	KUNIT_EXPECT_EQ(test, pst_wait(t, 0, 1), -EIO);
	pst_drain(t);
}

/* A consumer that gives up before the start ends does not end it. */
static void pmp_start_consumer_timeout_test(struct kunit *test)
{
	struct pst *t = pst_new(test);

	t->pmp.bind_ms = 100;
	KUNIT_EXPECT_EQ(test, pst_wait(t, 0, 10), -ETIMEDOUT);
	KUNIT_EXPECT_EQ(test, pst_wait(t, 0, 5000), 0);
	KUNIT_EXPECT_EQ(test, t->pmp.enables, 1U);
	pst_drain(t);
}

struct pst_waiter {
	struct pst *t;
	int ret;
	struct completion done;
};

static int pst_waiter_fn(void *data)
{
	struct pst_waiter *w = data;

	w->ret = pst_wait(w->t, 0, 60000);
	complete(&w->done);
	return 0;
}

/*
 * The provider goes away while a consumer waits on a start that is still
 * polling for the PMP driver: the consumer is told -ENODEV, the start gives
 * up at its next poll instead of running out its timeout, and the PMP
 * stays enabled.
 */
static void pmp_start_teardown_while_waiting_test(struct kunit *test)
{
	static const struct apple_pmp_start_timing slow = {
		.bind = 60000, .quiet = 0, .ready = 60000, .ack = 60000,
	};
	struct pst *t = pst_new(test);
	struct task_struct *task;
	struct pst_waiter w = { .t = t };
	unsigned long began;

	t->st.timing = &slow;
	t->pmp.binds = false;
	init_completion(&w.done);
	task = kthread_run(pst_waiter_fn, &w, "pmp-start-waiter");
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, task);

	/* Let the waiter queue the start and the start enable the PMP. */
	while (!READ_ONCE(t->st.enabled))
		msleep(20);

	began = jiffies;
	pst_drain(t);
	KUNIT_EXPECT_LT(test, jiffies_to_msecs(jiffies - began), 5000U);
	KUNIT_ASSERT_NE(test, wait_for_completion_timeout(&w.done, 5 * HZ), 0UL);
	KUNIT_EXPECT_EQ(test, w.ret, -ENODEV);
	KUNIT_EXPECT_TRUE(test, READ_ONCE(t->st.enabled));
	KUNIT_EXPECT_EQ(test, t->st.result, -ENODEV);
	KUNIT_EXPECT_EQ(test, t->st.waiters, 0U);

	/* After teardown nobody can wait on it any more. */
	KUNIT_EXPECT_EQ(test, pst_wait(t, 0, 1), -ENODEV);
}

/* Teardown before anyone asked: the PMP was never started. */
static void pmp_start_teardown_idle_test(struct kunit *test)
{
	struct pst *t = pst_new(test);

	pst_drain(t);
	KUNIT_EXPECT_EQ(test, pst_wait(t, 0, 1), -ENODEV);
	KUNIT_EXPECT_EQ(test, t->pmp.enables, 0U);
	KUNIT_EXPECT_FALSE(test, READ_ONCE(t->st.enabled));
}

/* A torn-down entry or report fails its power callbacks, without MMIO. */
static void pmp_start_entry_teardown_test(struct kunit *test)
{
	struct apple_pmp_report_entry *ent;

	ent = kunit_kzalloc(test, sizeof(*ent), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, ent);
	ent->removed = true;
	KUNIT_EXPECT_EQ(test, apple_pmp_report_entry_power_on(&ent->genpd), -ENODEV);
	KUNIT_EXPECT_EQ(test, apple_pmp_report_entry_power_off(&ent->genpd), -ENODEV);

	/* Not removed, but its report region is no longer bound. */
	ent->removed = false;
	KUNIT_EXPECT_EQ(test, apple_pmp_report_entry_power_on(&ent->genpd), -ENODEV);
}

/* T6030's real overlay: DISP (7), deferred external requests (8/9),
 * and storage (16), which has no acknowledgement. Only DISP gates startup.
 */
static struct pst *pst_t6030(struct kunit *test)
{
	struct pst *t = pst_new(test);

	t->st.seed = BIT_ULL(7) | BIT_ULL(8) | BIT_ULL(9) | BIT_ULL(16);
	t->st.ack = BIT_ULL(7) | BIT_ULL(8) | BIT_ULL(9);
	t->st.startup_ack = BIT_ULL(7);
	t->pmp.ack = BIT_ULL(7);
	pst_write(t, pst_offsets.tgt_read, t->st.seed);
	return t;
}

static void pmp_start_t6030_deferred_external_test(struct kunit *test)
{
	struct pst *t = pst_t6030(test);

	KUNIT_ASSERT_EQ(test, pst_wait(t, 7, 5000), 0);
	KUNIT_EXPECT_EQ(test, t->pmp.enables, 1U);
	KUNIT_EXPECT_EQ(test, pst_wait(t, 8, 1), -EIO);
	KUNIT_EXPECT_EQ(test, pst_wait(t, 9, 1), -EIO);
	KUNIT_EXPECT_EQ(test, pst_wait(t, 16, 1), -EINVAL);
	pst_write(t, pst_offsets.actual, BIT_ULL(7) | BIT_ULL(8));
	KUNIT_EXPECT_EQ(test, pst_wait(t, 8, 1), 0);
	KUNIT_EXPECT_EQ(test, t->pmp.enables, 1U);
	pst_drain(t);
}

/* An external consumer must not bypass the internal startup admission. */
static void pmp_start_t6030_external_first_test(struct kunit *test)
{
	struct pst *t = pst_t6030(test);

	t->pmp.ack = BIT_ULL(8);
	KUNIT_EXPECT_EQ(test, pst_wait(t, 8, 5000), -EIO);
	KUNIT_EXPECT_EQ(test, pst_wait(t, 7, 1), -EIO);
	pst_drain(t);
}

static struct kunit_case pmp_start_cases[] = {
	KUNIT_CASE(pmp_start_t6030_deferred_external_test),
	KUNIT_CASE(pmp_start_t6030_external_first_test),
	KUNIT_CASE(pmp_start_ok_test),
	KUNIT_CASE(pmp_start_check_fails_test),
	KUNIT_CASE(pmp_start_busy_ptd_test),
	KUNIT_CASE(pmp_start_enable_fails_test),
	KUNIT_CASE(pmp_start_bind_timeout_test),
	KUNIT_CASE(pmp_start_ready_timeout_test),
	KUNIT_CASE(pmp_start_ack_fails_test),
	KUNIT_CASE(pmp_start_ack_withdrawn_test),
	KUNIT_CASE(pmp_start_consumer_timeout_test),
	KUNIT_CASE(pmp_start_teardown_while_waiting_test),
	KUNIT_CASE(pmp_start_teardown_idle_test),
	KUNIT_CASE(pmp_start_entry_teardown_test),
	{}
};

static struct kunit_suite pmp_start_suite = {
	.name = "apple-pmp-start",
	.test_cases = pmp_start_cases,
};

kunit_test_suite(pmp_start_suite);

/* Device DVFS: emulated report/update windows, no hardware. */

static void pmp_t8112_encoding_test(struct kunit *test)
{
	const struct apple_pmp_report_offsets *o = &apple_pmp_offsets_t8112;
	struct apple_pmp_report *rep = kunit_kzalloc(test, sizeof(*rep), GFP_KERNEL);
	void *mem = kunit_kzalloc(test, 0x20000, GFP_KERNEL);
	u64 other = 3ULL << 36;

	KUNIT_ASSERT_NOT_NULL(test, rep);
	KUNIT_ASSERT_NOT_NULL(test, mem);
	rep->offsets = o;
	rep->base = (void __iomem *)mem;
	spin_lock_init(&rep->lock);
	KUNIT_EXPECT_TRUE(test, apple_pmp_report_dvfs_valid(o, 0xb0, 2));
	KUNIT_EXPECT_FALSE(test, apple_pmp_report_dvfs_valid(o, 0xaf, 2));
	KUNIT_EXPECT_FALSE(test, apple_pmp_report_dvfs_valid(o, 0xb1, 2));
	KUNIT_EXPECT_FALSE(test, apple_pmp_report_dvfs_valid(o, 0xa7, 2));
	KUNIT_EXPECT_FALSE(test, apple_pmp_report_dvfs_valid(o, 0xb8, 2));
	KUNIT_EXPECT_FALSE(test, apple_pmp_report_dvfs_valid(o, 0xb0, 32));
	KUNIT_EXPECT_EQ(test, o->update + 8 * 0xb0, 0x10580U);
	apple_pmp_report_dvfs_write(rep, 0xb0, PMP_DVFS_VALID | 2);
	KUNIT_EXPECT_EQ(test, readq(rep->base + 0x10580), 0x2000000000000002ULL);
	/* Another provider-owned class survives a default-floor update/release. */
	rep->dvfs_words[0] |= other;
	apple_pmp_report_dvfs_write(rep, 0xb0, PMP_DVFS_VALID);
	KUNIT_EXPECT_EQ(test, readq(rep->base + 0x10580), PMP_DVFS_VALID | other);
	KUNIT_EXPECT_EQ(test, rep->dvfs_words[0], PMP_DVFS_VALID | other);
	KUNIT_EXPECT_EQ(test, rep->dvfs_known, 1ULL);
}

struct pmp_dvfs_test {
	struct apple_pmp_report rep;
	struct apple_pmp_report_entry ent;
};

static int pmp_dvfs_init(struct kunit *test)
{
	struct pmp_dvfs_test *t = kunit_kzalloc(test, sizeof(*t), GFP_KERNEL);
	void *mem = kunit_kzalloc(test, 0x20000, GFP_KERNEL);

	if (!t || !mem)
		return -ENOMEM;
	test->priv = t;
	t->rep.base = (void __iomem *)mem;
	t->rep.offsets = &apple_pmp_offsets_t8112;
	/* Identity only: neither callback dereferences this emulated node. */
	t->ent.report_node = (struct device_node *)t;
	t->rep.dev = kunit_kzalloc(test, sizeof(*t->rep.dev), GFP_KERNEL);
	if (!t->rep.dev)
		return -ENOMEM;
	t->rep.dev->of_node = t->ent.report_node;
	t->ent.id = 10;
	t->ent.dvfs = true;
	t->ent.dvfs_entry = 0xb0;
	t->ent.dvfs_on = PMP_DVFS_VALID | 2;
	spin_lock_init(&t->rep.lock);
	mutex_lock(&apple_pmp_reports_lock);
	list_add_tail_rcu(&t->rep.list, &apple_pmp_reports);
	mutex_unlock(&apple_pmp_reports_lock);
	return 0;
}

static void pmp_dvfs_exit(struct kunit *test)
{
	struct pmp_dvfs_test *t = test->priv;

	mutex_lock(&apple_pmp_reports_lock);
	list_del_rcu(&t->rep.list);
	mutex_unlock(&apple_pmp_reports_lock);
	synchronize_rcu();
}

static void pmp_t8112_stopped_test(struct kunit *test)
{
	struct pmp_dvfs_test *t = test->priv;

	KUNIT_EXPECT_EQ(test, apple_pmp_report_entry_power_on(&t->ent.genpd), -EAGAIN);
	KUNIT_EXPECT_EQ(test, t->rep.known, 0ULL);
	KUNIT_EXPECT_EQ(test, t->rep.dvfs_known, 0ULL);
	writeq(1, t->rep.base + 0x10);
	t->rep.defer = true;
	KUNIT_EXPECT_EQ(test, apple_pmp_report_entry_power_on(&t->ent.genpd), -EAGAIN);
	KUNIT_EXPECT_EQ(test, t->rep.dvfs_known, 0ULL);
}

static void pmp_t8112_ack_timeout_test(struct kunit *test)
{
	struct pmp_dvfs_test *t = test->priv;

	writeq(1, t->rep.base + 0x10);
	KUNIT_EXPECT_EQ(test, apple_pmp_report_entry_power_on(&t->ent.genpd), -ETIMEDOUT);
	KUNIT_EXPECT_EQ(test, readq(t->rep.base + 0x10500), BIT_ULL(10));
	KUNIT_EXPECT_EQ(test, t->rep.dvfs_known, 0ULL);
	KUNIT_EXPECT_EQ(test, readq(t->rep.base + 0x10580), 0ULL);
}

static void pmp_t8112_active_release_test(struct kunit *test)
{
	struct pmp_dvfs_test *t = test->priv;

	writeq(1, t->rep.base + 0x10);
	writeq(BIT_ULL(10), t->rep.base + 0xa40);
	KUNIT_ASSERT_EQ(test, apple_pmp_report_entry_power_on(&t->ent.genpd), 0);
	KUNIT_EXPECT_EQ(test, readq(t->rep.base + 0x10500), BIT_ULL(10));
	KUNIT_EXPECT_EQ(test, readq(t->rep.base + 0x10580), PMP_DVFS_VALID | 2);
	/* ACK deliberately stays asserted: release is stored before ACK clear. */
	KUNIT_EXPECT_EQ(test, apple_pmp_report_entry_power_off(&t->ent.genpd), -ETIMEDOUT);
	KUNIT_EXPECT_EQ(test, readq(t->rep.base + 0x10580), PMP_DVFS_VALID);
	KUNIT_EXPECT_EQ(test, readq(t->rep.base + 0x10500), 0ULL);
	writeq(0, t->rep.base + 0xa40);
	KUNIT_EXPECT_EQ(test, apple_pmp_report_entry_power_off(&t->ent.genpd), 0);
	KUNIT_EXPECT_EQ(test, t->rep.shadow, 0ULL);
}

static struct kunit_case pmp_dvfs_cases[] = {
	KUNIT_CASE(pmp_t8112_encoding_test),
	KUNIT_CASE(pmp_t8112_stopped_test),
	KUNIT_CASE(pmp_t8112_ack_timeout_test),
	KUNIT_CASE(pmp_t8112_active_release_test),
	{}
};

static struct kunit_suite pmp_dvfs_suite = {
	.name = "apple-pmp-dvfs",
	.init = pmp_dvfs_init,
	.exit = pmp_dvfs_exit,
	.test_cases = pmp_dvfs_cases,
};

kunit_test_suite(pmp_dvfs_suite);
