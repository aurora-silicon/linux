// SPDX-License-Identifier: GPL-2.0-only
/*
 * KUnit tests for arm64 TPIDRRO_EL0 logical CPU index exposure and
 * PR_GET_TPIDRRO_CPU. Included from process.c to test static helpers.
 */
#include <kunit/test.h>
#include <linux/cpumask.h>
#include <linux/sched.h>

static void tpidrro_cpu_switch_value_test(struct kunit *test)
{
	struct task_struct *task;

	task = kunit_kzalloc(test, sizeof(*task), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, task);

	task->thread.uw.tp_value = 0xdeadbeefUL;

	/* Native 64-bit tasks receive the logical CPU index. */
	KUNIT_EXPECT_EQ(test, tls_tpidrro_value(task, 0), 0ULL);
	KUNIT_EXPECT_EQ(test, tls_tpidrro_value(task, 7), 7ULL);
	KUNIT_EXPECT_EQ(test, tls_tpidrro_value(task, 255), 255ULL);

#ifdef CONFIG_COMPAT
	/* Compat 32-bit tasks keep their TLS pointer regardless of CPU. */
	set_tsk_thread_flag(task, TIF_32BIT);
	KUNIT_EXPECT_EQ(test, tls_tpidrro_value(task, 0), 0xdeadbeefULL);
	KUNIT_EXPECT_EQ(test, tls_tpidrro_value(task, 7), 0xdeadbeefULL);
#endif
}

static void tpidrro_cpu_prctl_query_test(struct kunit *test)
{
	struct task_struct *task;

	task = kunit_kzalloc(test, sizeof(*task), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, task);

	KUNIT_EXPECT_EQ(test, arch_prctl_get_tpidrro_cpu(), 1);
	KUNIT_EXPECT_EQ(test, arch_prctl_get_tpidrro_cpu_for_task(task), 1);

#ifdef CONFIG_COMPAT
	set_tsk_thread_flag(task, TIF_32BIT);
	KUNIT_EXPECT_EQ(test, arch_prctl_get_tpidrro_cpu_for_task(task), 0);
#endif
}

static void tpidrro_cpu_live_migration_test(struct kunit *test)
{
	cpumask_var_t saved_mask;
	unsigned int cpu;
	unsigned int visited = 0;

	KUNIT_ASSERT_TRUE(test, zalloc_cpumask_var(&saved_mask, GFP_KERNEL));
	cpumask_copy(saved_mask, current->cpus_ptr);

	for_each_online_cpu(cpu) {
		unsigned int actual_cpu;
		u64 reg_val;
		int ret;

		ret = set_cpus_allowed_ptr(current, cpumask_of(cpu));
		if (ret)
			continue;

		preempt_disable();
		actual_cpu = smp_processor_id();
		reg_val = read_sysreg(tpidrro_el0);
		preempt_enable();

		KUNIT_EXPECT_EQ(test, actual_cpu, cpu);
		KUNIT_EXPECT_EQ(test, reg_val, (u64)cpu);
		visited++;
	}

	set_cpus_allowed_ptr(current, saved_mask);
	free_cpumask_var(saved_mask);

	KUNIT_EXPECT_GT(test, visited, 0U);
}

static struct kunit_case tpidrro_cpu_cases[] = {
	KUNIT_CASE(tpidrro_cpu_switch_value_test),
	KUNIT_CASE(tpidrro_cpu_prctl_query_test),
	KUNIT_CASE(tpidrro_cpu_live_migration_test),
	{}
};

static struct kunit_suite tpidrro_cpu_suite = {
	.name = "arm64-tpidrro-cpu",
	.test_cases = tpidrro_cpu_cases,
};

kunit_test_suite(tpidrro_cpu_suite);
