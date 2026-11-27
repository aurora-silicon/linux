// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * KUnit fault injection for starting a halted PMP: the start's checks, the
 * enabling of the PMP nodes and the PMP driver's bind are replaced, and the
 * report region is memory, so each failure after the PMP was enabled (no
 * bind, no ready status, no acknowledgment), a consumer's own timeout and
 * the teardown of the provider can be driven without Apple hardware.
 *
 * The fast die-temperature effort is read from a report in memory, too: its
 * lanes, the reports that have none or do not run, and the T8142 layout.
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

/*
 * A report in memory with the PMP's status in its first word and the fast
 * die-temperature efforts in an entry of its own: 16 bytes, of which the
 * low 32 bits hold the lanes.
 */
#define PFD_STATUS	0x08
#define PFD_EFFORT	0x10
#define PFD_LANES	0xa4c30f5aU
#define PFD_OTHER	0x1122334400000000ULL

struct pfd {
	struct apple_pmp_report rep;
	struct apple_pmp_report_offsets offsets;
	struct device dev;
	struct device_node node;
	u64 regs[4];
};

static void pfd_remove(void *data)
{
	struct pfd *t = data;

	mutex_lock(&apple_pmp_reports_lock);
	list_del_rcu(&t->rep.list);
	mutex_unlock(&apple_pmp_reports_lock);
	synchronize_rcu();
}

/* A running report with @lanes fast-die lanes, published as probe does. */
static struct pfd *pfd_new(struct kunit *test, u32 lanes)
{
	struct pfd *t = kunit_kzalloc(test, sizeof(*t), GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, t);
	t->offsets.status = PFD_STATUS;
	t->offsets.fast_die = lanes ? PFD_EFFORT : 0;
	t->offsets.fast_die_lanes = lanes;
	t->dev.of_node = &t->node;
	t->rep.dev = &t->dev;
	t->rep.offsets = &t->offsets;
	t->rep.base = (__force void __iomem *)t->regs;
	spin_lock_init(&t->rep.lock);

	t->regs[PFD_STATUS / sizeof(u64)] = PMP_REPORT_READY;
	t->regs[PFD_EFFORT / sizeof(u64)] = PFD_OTHER | PFD_LANES;
	t->regs[PFD_EFFORT / sizeof(u64) + 1] = ~0ULL;

	mutex_lock(&apple_pmp_reports_lock);
	list_add_tail_rcu(&t->rep.list, &apple_pmp_reports);
	mutex_unlock(&apple_pmp_reports_lock);
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, pfd_remove, t), 0);
	return t;
}

static int pfd_effort(struct pfd *t, unsigned int lane, u8 *effort)
{
	*effort = 0xee;
	return apple_pmp_report_fast_die_effort(&t->node, lane, effort);
}

/* Each lane is one byte of the low word, read 32 or 64 bits wide. */
static void pmp_fast_die_lanes_test(struct kunit *test)
{
	struct pfd *t = pfd_new(test, 3);
	u8 effort;
	int wide;

	for (wide = 0; wide < 2; wide++) {
		t->offsets.fast_die_wide = wide;
		t->offsets.fast_die_lanes = 3;
		KUNIT_EXPECT_EQ(test, pfd_effort(t, 0, &effort), 0);
		KUNIT_EXPECT_EQ(test, effort, 0x5a);
		KUNIT_EXPECT_EQ(test, pfd_effort(t, 1, &effort), 0);
		KUNIT_EXPECT_EQ(test, effort, 0x0f);
		KUNIT_EXPECT_EQ(test, pfd_effort(t, 2, &effort), 0);
		KUNIT_EXPECT_EQ(test, effort, 0xc3);

		/* Past the report's lanes: refused, nothing is stored. */
		KUNIT_EXPECT_EQ(test, pfd_effort(t, 3, &effort), -EINVAL);
		KUNIT_EXPECT_EQ(test, effort, 0xee);
		KUNIT_EXPECT_EQ(test, pfd_effort(t, 4, &effort), -EINVAL);
		KUNIT_EXPECT_EQ(test, pfd_effort(t, UINT_MAX, &effort), -EINVAL);

		t->offsets.fast_die_lanes = 4;
		KUNIT_EXPECT_EQ(test, pfd_effort(t, 3, &effort), 0);
		KUNIT_EXPECT_EQ(test, effort, 0xa4);
	}
}

/* A stopped PMP reads 0, which would ask for no limit: no effort then. */
static void pmp_fast_die_running_test(struct kunit *test)
{
	struct pfd *t = pfd_new(test, 2);
	u64 *status = &t->regs[PFD_STATUS / sizeof(u64)];
	u8 effort;

	WRITE_ONCE(*status, 0);
	KUNIT_EXPECT_EQ(test, pfd_effort(t, 0, &effort), -EAGAIN);
	KUNIT_EXPECT_EQ(test, effort, 0xee);
	t->offsets.running_any = true;
	KUNIT_EXPECT_EQ(test, pfd_effort(t, 0, &effort), -EAGAIN);
	KUNIT_EXPECT_EQ(test, effort, 0xee);

	/* Status bits other than "ready" only count where the SoC says so. */
	WRITE_ONCE(*status, BIT_ULL(36));
	KUNIT_EXPECT_EQ(test, pfd_effort(t, 1, &effort), 0);
	KUNIT_EXPECT_EQ(test, effort, 0x0f);
	t->offsets.running_any = false;
	KUNIT_EXPECT_EQ(test, pfd_effort(t, 1, &effort), -EAGAIN);

	WRITE_ONCE(*status, BIT_ULL(36) | PMP_REPORT_READY);
	KUNIT_EXPECT_EQ(test, pfd_effort(t, 1, &effort), 0);
	KUNIT_EXPECT_EQ(test, effort, 0x0f);

	/* A lane the report does not have is the caller's error, running or not. */
	WRITE_ONCE(*status, 0);
	KUNIT_EXPECT_EQ(test, pfd_effort(t, 2, &effort), -EINVAL);
}

/* The report is found by its node; one without the entry says so. */
static void pmp_fast_die_lookup_test(struct kunit *test)
{
	struct pfd *none = pfd_new(test, 0);
	struct pfd *a = pfd_new(test, 2);
	struct pfd *b = pfd_new(test, 2);
	struct device_node *unbound;
	u8 effort = 0xee;

	unbound = kunit_kzalloc(test, sizeof(*unbound), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, unbound);
	KUNIT_EXPECT_EQ(test, apple_pmp_report_fast_die_effort(unbound, 0, &effort),
			-EPROBE_DEFER);
	KUNIT_EXPECT_EQ(test, effort, 0xee);

	b->regs[PFD_EFFORT / sizeof(u64)] = 0x7781;
	KUNIT_EXPECT_EQ(test, pfd_effort(a, 0, &effort), 0);
	KUNIT_EXPECT_EQ(test, effort, 0x5a);
	KUNIT_EXPECT_EQ(test, pfd_effort(b, 0, &effort), 0);
	KUNIT_EXPECT_EQ(test, effort, 0x81);
	KUNIT_EXPECT_EQ(test, pfd_effort(b, 1, &effort), 0);
	KUNIT_EXPECT_EQ(test, effort, 0x77);

	/* No entry: unsupported whatever the lane and the PMP's state. */
	KUNIT_EXPECT_EQ(test, pfd_effort(none, 0, &effort), -EOPNOTSUPP);
	KUNIT_EXPECT_EQ(test, effort, 0xee);
	none->regs[PFD_STATUS / sizeof(u64)] = 0;
	KUNIT_EXPECT_EQ(test, pfd_effort(none, 0, &effort), -EOPNOTSUPP);
}

/*
 * The T8142 report as its PMP's table of PTD entries gives it: PMP-STATUS 1,
 * FAST-DIE-CE 15 (E, P), SOC-DEV-PS-REQ 39, SOC-DEV-PS-ACK 47 and
 * SOC-DEV-DVFS 55 to 70. An entry is read in 16 bytes from the base and
 * written in 8 bytes from the update aperture.
 */
static void pmp_fast_die_t8142_test(struct kunit *test)
{
	const struct apple_pmp_report_offsets *offs = NULL;
	const struct of_device_id *id;

	for (id = apple_pmp_report_of_match; id->compatible[0]; id++)
		if (!strcmp(id->compatible, "apple,t8142-pmp-v2-report"))
			offs = id->data;
	KUNIT_ASSERT_NOT_NULL(test, offs);

	KUNIT_EXPECT_EQ(test, offs->status, 1 * 16);
	KUNIT_EXPECT_EQ(test, offs->fast_die, 15 * 16);
	KUNIT_EXPECT_EQ(test, offs->fast_die_lanes, 2);
	KUNIT_EXPECT_TRUE(test, offs->fast_die_wide);
	KUNIT_EXPECT_EQ(test, offs->tgt_read, 39 * 16);
	KUNIT_EXPECT_EQ(test, offs->tgt_write, offs->update + 39 * 8);
	KUNIT_EXPECT_EQ(test, offs->actual, 47 * 16);
	KUNIT_EXPECT_EQ(test, offs->dvfs_first, 55);
	KUNIT_EXPECT_EQ(test, offs->dvfs_first + offs->dvfs_count - 1, 70);
	KUNIT_EXPECT_TRUE(test, offs->running_any);
	KUNIT_EXPECT_TRUE(test, offs->defer_requests);
	KUNIT_EXPECT_FALSE(test, offs->starts_pmp);
}

static struct kunit_case pmp_fast_die_cases[] = {
	KUNIT_CASE(pmp_fast_die_lanes_test),
	KUNIT_CASE(pmp_fast_die_running_test),
	KUNIT_CASE(pmp_fast_die_lookup_test),
	KUNIT_CASE(pmp_fast_die_t8142_test),
	{}
};

static struct kunit_suite pmp_fast_die_suite = {
	.name = "apple-pmp-fast-die",
	.test_cases = pmp_fast_die_cases,
};

kunit_test_suites(&pmp_start_suite, &pmp_fast_die_suite);
