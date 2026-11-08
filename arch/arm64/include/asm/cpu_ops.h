/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2013 ARM Ltd.
 */
#ifndef __ASM_CPU_OPS_H
#define __ASM_CPU_OPS_H

#include <linux/init.h>
#include <linux/threads.h>
#include <linux/types.h>

/**
 * struct cpu_operations - Callback operations for hotplugging CPUs.
 *
 * @name:	Name of the property as appears in a devicetree cpu node's
 *		enable-method property. On systems booting with ACPI, @name
 *		identifies the struct cpu_operations entry corresponding to
 *		the boot protocol specified in the ACPI MADT table.
 * @cpu_init:	Reads any data necessary for a specific enable-method for a
 *		proposed logical id.
 * @cpu_prepare: Early one-time preparation step for a cpu. If there is a
 *		mechanism for doing so, tests whether it is possible to boot
 *		the given CPU.
 * @cpu_boot:	Boots a cpu into the kernel.
 * @cpu_postboot: Optionally, perform any post-boot cleanup or necessary
 *		synchronisation. Called from the cpu being booted.
 * @cpu_can_disable: Determines whether a CPU can be disabled based on
 *		mechanism-specific information.
 * @cpu_disable: Prepares a cpu to die. May fail for some mechanism-specific
 * 		reason, which will cause the hot unplug to be aborted. Called
 * 		from the cpu to be killed.
 * @cpu_die:	Makes a cpu leave the kernel. Must not fail. Called from the
 *		cpu being killed.
 * @cpu_kill:  Ensures a cpu has left the kernel. Called from another cpu.
 */
struct cpu_operations {
	const char	*name;
	int		(*cpu_init)(unsigned int);
	int		(*cpu_prepare)(unsigned int);
	int		(*cpu_boot)(unsigned int);
	void		(*cpu_postboot)(void);
#ifdef CONFIG_HOTPLUG_CPU
	bool		(*cpu_can_disable)(unsigned int cpu);
	int		(*cpu_disable)(unsigned int cpu);
	void		(*cpu_die)(unsigned int cpu);
	int		(*cpu_kill)(unsigned int cpu);
#endif
};

/**
 * struct spin_table_power_ops - Platform control of parking spin-table CPUs
 * (smp_spin_table.c).
 *
 * @cpu_stop:	Tell the hardware the calling CPU is stopping. The CPU keeps
 *		running, and stays coherent, until @cpu_off.
 * @cpu_off:	Power off the calling CPU, which must not return.
 * @cpu_on:	Start a CPU that @cpu_off powered off, at its reset vector.
 *
 * Each takes the CPU's MPIDR hardware ID.
 */
struct spin_table_power_ops {
	void	(*cpu_stop)(u64 mpidr);
	void	(*cpu_off)(u64 mpidr);
	int	(*cpu_on)(u64 mpidr);
};

#ifdef CONFIG_HOTPLUG_CPU
int spin_table_set_power_ops(const struct spin_table_power_ops *ops);
int spin_table_hold_parked(void);
void spin_table_unhold_parked(void);
int spin_table_stop_parked(void);
void spin_table_release_stopped(void);
void spin_table_stop_finished(void);
bool spin_table_cpus_stuck(void);
#else
static inline int spin_table_hold_parked(void)
{
	return -EOPNOTSUPP;
}

static inline void spin_table_unhold_parked(void) {}

static inline int spin_table_stop_parked(void)
{
	return -EOPNOTSUPP;
}

static inline void spin_table_release_stopped(void) {}
static inline void spin_table_stop_finished(void) {}

static inline int spin_table_set_power_ops(const struct spin_table_power_ops *ops)
{
	return -EOPNOTSUPP;
}

static inline bool spin_table_cpus_stuck(void)
{
	return false;
}
#endif

int __init init_cpu_ops(int cpu);
extern const struct cpu_operations *get_cpu_ops(int cpu);

static inline void __init init_bootcpu_ops(void)
{
	init_cpu_ops(0);
}

#endif /* ifndef __ASM_CPU_OPS_H */
