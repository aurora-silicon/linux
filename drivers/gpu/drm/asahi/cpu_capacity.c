// SPDX-License-Identifier: GPL-2.0-only OR MIT
//
// CPU capacity lookup for the syncobj wait hint (gpu.rs).

#include <linux/arch_topology.h>
#include <linux/cpumask.h>
#include <linux/minmax.h>
#include <linux/sched/topology.h>

unsigned int asahi_min_cpu_capacity(void);

/* The capacity of the smallest possible CPU, on the 0..1024 scale. */
unsigned int asahi_min_cpu_capacity(void)
{
	unsigned long capacity = SCHED_CAPACITY_SCALE;
	unsigned int cpu;

	for_each_possible_cpu(cpu)
		capacity = min(capacity, arch_scale_cpu_capacity(cpu));
	return capacity;
}
