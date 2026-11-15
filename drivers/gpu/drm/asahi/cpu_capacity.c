// SPDX-License-Identifier: GPL-2.0-only OR MIT
//
// CPU capacity lookups for the syncobj wait hint (gpu.rs) and the G17 render
// scheduler (g17/job.rs).

#include <linux/arch_topology.h>
#include <linux/cpumask.h>
#include <linux/minmax.h>
#include <linux/sched/topology.h>

unsigned int asahi_min_cpu_capacity(void);
void asahi_fastest_cpus(struct cpumask *cpus);

/* The capacity of the smallest possible CPU, on the 0..1024 scale. */
unsigned int asahi_min_cpu_capacity(void)
{
	unsigned long capacity = SCHED_CAPACITY_SCALE;
	unsigned int cpu;

	for_each_possible_cpu(cpu)
		capacity = min(capacity, arch_scale_cpu_capacity(cpu));
	return capacity;
}

/*
 * The possible CPUs of the highest capacity into @cpus: the P cores of a part
 * that also has E cores, every CPU of a part with one kind.
 */
void asahi_fastest_cpus(struct cpumask *cpus)
{
	unsigned long capacity = 0;
	unsigned int cpu;

	for_each_possible_cpu(cpu)
		capacity = max(capacity, arch_scale_cpu_capacity(cpu));
	cpumask_clear(cpus);
	for_each_possible_cpu(cpu)
		if (arch_scale_cpu_capacity(cpu) == capacity)
			cpumask_set_cpu(cpu, cpus);
}
