// SPDX-License-Identifier: GPL-2.0-only
/*
 * Spin Table SMP initialisation
 *
 * Copyright (C) 2013 ARM Ltd.
 */

#include <linux/bitmap.h>
#include <linux/delay.h>
#include <linux/init.h>
#include <linux/of.h>
#include <linux/smp.h>
#include <linux/suspend.h>
#include <linux/types.h>
#include <linux/mm.h>

#include <asm/cacheflush.h>
#include <asm/cpu_ops.h>
#include <asm/cputype.h>
#include <asm/io.h>
#include <asm/kexec.h>
#include <asm/mmu_context.h>
#include <asm/smp_plat.h>

extern void secondary_holding_pen(void);
volatile unsigned long __section(".mmuoff.data.read")
secondary_holding_pen_release = INVALID_HWID;

static phys_addr_t cpu_release_addr[NR_CPUS];

/*
 * Write secondary_holding_pen_release in a way that is guaranteed to be
 * visible to all observers, irrespective of whether they're taking part
 * in coherency or not.  This is necessary for the hotplug code to work
 * reliably.
 */
static void write_pen_release(u64 val)
{
	void *start = (void *)&secondary_holding_pen_release;
	unsigned long size = sizeof(secondary_holding_pen_release);

	secondary_holding_pen_release = val;
	dcache_clean_inval_poc((unsigned long)start, (unsigned long)start + size);
}


static int smp_spin_table_cpu_init(unsigned int cpu)
{
	struct device_node *dn;
	int ret;

	dn = of_get_cpu_node(cpu, NULL);
	if (!dn)
		return -ENODEV;

	/*
	 * Determine the address from which the CPU is polling.
	 */
	ret = of_property_read_u64(dn, "cpu-release-addr",
				   &cpu_release_addr[cpu]);
	if (ret)
		pr_err("CPU %d: missing or invalid cpu-release-addr property\n",
		       cpu);

	of_node_put(dn);

	return ret;
}

static int smp_spin_table_cpu_prepare(unsigned int cpu)
{
	__le64 __iomem *release_addr;
	phys_addr_t pa_holding_pen = __pa_symbol(secondary_holding_pen);

	if (!cpu_release_addr[cpu])
		return -ENODEV;

	/*
	 * The cpu-release-addr may or may not be inside the linear mapping.
	 * As ioremap_cache will either give us a new mapping or reuse the
	 * existing linear mapping, we can use it to cover both cases. In
	 * either case the memory will be MT_NORMAL.
	 */
	release_addr = ioremap_cache(cpu_release_addr[cpu],
				     sizeof(*release_addr));
	if (!release_addr)
		return -ENOMEM;

	/*
	 * We write the release address as LE regardless of the native
	 * endianness of the kernel. Therefore, any boot-loaders that
	 * read this address need to convert this address to the
	 * boot-loader's endianness before jumping. This is mandated by
	 * the boot protocol.
	 */
	writeq_relaxed(pa_holding_pen, release_addr);
	dcache_clean_inval_poc((__force unsigned long)release_addr,
			    (__force unsigned long)release_addr +
				    sizeof(*release_addr));

	/*
	 * Send an event to wake up the secondary CPU.
	 */
	sev();

	iounmap(release_addr);

	return 0;
}

static int smp_spin_table_cpu_boot(unsigned int cpu)
{
	/*
	 * Update the pen release flag.
	 */
	write_pen_release(cpu_logical_map(cpu));

	/*
	 * Send an event, causing the secondaries to read pen_release.
	 */
	sev();

	return 0;
}

const struct cpu_operations smp_spin_table_ops = {
	.name		= "spin-table",
	.cpu_init	= smp_spin_table_cpu_init,
	.cpu_prepare	= smp_spin_table_cpu_prepare,
	.cpu_boot	= smp_spin_table_cpu_boot,
};

/*
 * A spin table whose CPUs can leave the kernel.
 *
 * Plain spin-table has no way out of the kernel, so disabling the non-boot
 * CPUs for system sleep fails with -EOPNOTSUPP. Some boot monitors leave a
 * CPU's reset vector locked to their own image and, after a reset, send the
 * CPU back through the same release address. Such a monitor publishes
 * /chosen/aurora,s2r-mailbox, and its "spin-table" CPUs then get these ops
 * (cpu_ops.c), so the device tree keeps the method every kernel knows. One can
 * also name "aurora,rvbar-spin-table" as a CPU's method.
 *
 * A CPU that really powers off can stop the rest of the system on such a chip,
 * so a CPU leaving the kernel parks here instead: still coherent, interrupts
 * masked. From there it goes back through secondary_holding_pen, or, when the
 * platform suspends the system, it stops and takes its last WFI after the commit.
 * It is then started again at its reset vector. Stopping and starting a CPU is
 * SoC-specific, so a platform driver provides it (spin_table_set_power_ops());
 * until one has, a suspend that needs the parked CPUs stopped is refused.
 * Parking and leaving the park need only cpu_soft_restart(): without
 * KEXEC_CORE, CPU hotplug fails cleanly, as on plain spin-table.
 *
 * A CPU left awake at the commit can hang the machine, and the commit can't be
 * undone. So each parked CPU first answers a hold, while the suspend can still
 * refuse, and only a held CPU stops.
 */
#ifdef CONFIG_HOTPLUG_CPU
static const struct spin_table_power_ops *spin_table_power __read_mostly;
static struct cpumask spin_table_cpus_parked;
static struct cpumask spin_table_cpus_held;
static struct cpumask spin_table_cpus_unpark;
static struct cpumask spin_table_cpus_off;
static bool spin_table_hold_requested;
static bool spin_table_stop_requested;
static bool spin_table_stop_released;

#define SPIN_TABLE_WAIT_US	100000

#if defined(CONFIG_HIBERNATION) && defined(CONFIG_KEXEC_CORE)
/*
 * Resuming from hibernation, the boot kernel takes its secondaries offline and
 * then overwrites every page the image saved, the park and its cpumasks
 * included. A CPU parked there would run on in memory that changes under it,
 * and its translation tables with it. On that path a CPU leaving the kernel
 * waits instead in a pen the image doesn't contain: the code and a slot per
 * CPU in .data..nosave, run with the MMU off. arm64 resumes only an image of
 * the kernel build that is running; with the boot kernel loaded at the image
 * kernel's physical address (checked when the resume starts) the pen is at the
 * same place in both, and the restored kernel reads what the boot kernel left
 * there. It starts each CPU from the pen through secondary_holding_pen, as at
 * boot. Nothing is powered off, so this needs no platform power ops.
 */
#define SPIN_TABLE_PEN_MAGIC	0x314e455034535053ULL	/* "SPS4PEN1" */
#define SPIN_TABLE_PEN_HERE	0x45524548ULL		/* "HERE", from the pen */

struct spin_table_pen_slot {
	u64 release;	/* where the CPU goes; 0 holds it */
	u64 here;	/* the CPU reached the pen */
	u64 mpidr;
} __aligned(ARCH_DMA_MINALIGN);

static struct {
	u64 magic;
	u64 self;	/* this record's address in the kernel that wrote it */
	unsigned long penned[BITS_TO_LONGS(NR_CPUS)];
	struct spin_table_pen_slot slot[NR_CPUS];
} spin_table_pen __nosavedata __aligned(ARCH_DMA_MINALIGN);

extern char spin_table_hibernate_pen[];
extern const struct cpu_operations smp_rvbar_spin_table_ops;
static struct cpumask spin_table_cpus_penned;
static bool spin_table_pen_requested;

static void spin_table_pen_clean(void *start, size_t size)
{
	dcache_clean_inval_poc((unsigned long)start, (unsigned long)start + size);
}

/*
 * Resuming from hibernation, before the boot kernel takes its secondaries offline.
 * The restored kernel finds the pen only where it is in its own image, which holds
 * when the boot kernel is loaded at the image kernel's physical address. arm64 can
 * restore from a boot kernel placed elsewhere (@image_ttbr1, the image's
 * swapper_pg_dir, is used for the jump), but there the pen's frames are image
 * frames: the restore would overwrite them while the penned CPUs run from them with
 * the MMU off. Refuse the resume then, as long as a CPU other than @primary (the
 * one that stays online) would need the pen.
 */
int spin_table_hibernate_pen_begin(phys_addr_t image_ttbr1, int primary)
{
	phys_addr_t here = __pa_symbol(swapper_pg_dir);
	int cpu;

	/* No record of an earlier, abandoned resume may reach the restored kernel. */
	spin_table_pen.magic = 0;
	spin_table_pen_clean(&spin_table_pen, sizeof(spin_table_pen.magic));

	if (image_ttbr1 != here) {
		for_each_online_cpu(cpu) {
			if (cpu == primary || get_cpu_ops(cpu) != &smp_rvbar_spin_table_ops)
				continue;
			pr_err("hibernation: the image's kernel ran at another physical address (swapper_pg_dir %pa, here %pa): spin-table CPUs cannot wait in this kernel's pen, resume refused\n",
			       &image_ttbr1, &here);
			return -EINVAL;
		}
		return 0;
	}
	WRITE_ONCE(spin_table_pen_requested, true);
	return 0;
}

/*
 * After they went offline. With all of them frozen, the restored kernel finds the
 * record; otherwise the resume is abandoned and they go back online from the pen.
 */
void spin_table_hibernate_pen_end(bool frozen)
{
	unsigned int cpu;

	WRITE_ONCE(spin_table_pen_requested, false);
	if (!frozen || cpumask_empty(&spin_table_cpus_penned))
		return;

	bitmap_zero(spin_table_pen.penned, NR_CPUS);
	for_each_cpu(cpu, &spin_table_cpus_penned)
		__set_bit(cpu, spin_table_pen.penned);
	spin_table_pen.self = __pa_symbol(&spin_table_pen);
	spin_table_pen.magic = SPIN_TABLE_PEN_MAGIC;
	spin_table_pen_clean(&spin_table_pen, offsetof(typeof(spin_table_pen), slot));
	pr_info("hibernation: %u CPUs wait outside the image\n",
		cpumask_weight(&spin_table_cpus_penned));
}

/*
 * In the restored kernel, right after the image took over: the CPUs the image parked
 * before its snapshot are in the boot kernel's pen now.
 */
void spin_table_hibernate_restored(void)
{
	unsigned int cpu, found = 0;

	if (spin_table_pen.magic != SPIN_TABLE_PEN_MAGIC)
		return;
	spin_table_pen.magic = 0;
	spin_table_pen_clean(&spin_table_pen, sizeof(spin_table_pen.magic));
	if (spin_table_pen.self != __pa_symbol(&spin_table_pen)) {
		pr_err("hibernation: the boot kernel's CPU pen was at %#llx, not here: its CPUs stay offline\n",
		       spin_table_pen.self);
		return;
	}

	cpumask_clear(&spin_table_cpus_penned);
	for_each_present_cpu(cpu) {
		if (!test_bit(cpu, spin_table_pen.penned))
			continue;
		if (spin_table_pen.slot[cpu].mpidr != cpu_logical_map(cpu)) {
			pr_err("hibernation: CPU%u: the pen holds MPIDR %#llx, not %#llx\n", cpu,
			       spin_table_pen.slot[cpu].mpidr, cpu_logical_map(cpu));
			continue;
		}
		cpumask_clear_cpu(cpu, &spin_table_cpus_parked);
		cpumask_clear_cpu(cpu, &spin_table_cpus_held);
		cpumask_clear_cpu(cpu, &spin_table_cpus_unpark);
		cpumask_clear_cpu(cpu, &spin_table_cpus_off);
		cpumask_set_cpu(cpu, &spin_table_cpus_penned);
		found++;
	}
	pr_info("hibernation: %u CPUs start from the boot kernel's pen\n", found);
}

/* Leaves the kernel for the pen; the boot CPU waits for it to arrive (cpu_kill). */
static void __noreturn smp_rvbar_spin_table_pen(unsigned int cpu, u64 mpidr)
{
	struct spin_table_pen_slot *slot = &spin_table_pen.slot[cpu];
	typeof(cpu_soft_restart) *restart;

	slot->release = 0;
	slot->here = 0;
	slot->mpidr = mpidr;
	spin_table_pen_clean(slot, sizeof(*slot));
	cpumask_set_cpu(cpu, &spin_table_cpus_penned);

	cpu_install_idmap();
	restart = (void *)__pa_symbol(cpu_soft_restart);
	restart(0, __pa_symbol(spin_table_hibernate_pen), __pa_symbol(slot), 0, 0);
	unreachable();
}

static bool spin_table_pen_arrived(unsigned int cpu)
{
	struct spin_table_pen_slot *slot = &spin_table_pen.slot[cpu];

	/* Written with the MMU off: drop any copy of the line before reading. */
	dcache_inval_poc((unsigned long)slot, (unsigned long)(slot + 1));
	return READ_ONCE(slot->here) == SPIN_TABLE_PEN_HERE;
}

/* Starts a CPU waiting in the pen; it goes on to secondary_holding_pen. */
static void spin_table_pen_release(unsigned int cpu)
{
	struct spin_table_pen_slot *slot = &spin_table_pen.slot[cpu];

	WRITE_ONCE(slot->release, __pa_symbol(secondary_holding_pen));
	spin_table_pen_clean(slot, sizeof(*slot));
}
#else
static const bool spin_table_pen_requested;
static struct cpumask spin_table_cpus_penned;

static void smp_rvbar_spin_table_pen(unsigned int cpu, u64 mpidr)
{
}

static bool spin_table_pen_arrived(unsigned int cpu)
{
	return false;
}

static void spin_table_pen_release(unsigned int cpu)
{
}

#ifdef CONFIG_HIBERNATION
/* Without cpu_soft_restart() a CPU cannot leave the kernel: nothing to keep. */
int spin_table_hibernate_pen_begin(phys_addr_t image_ttbr1, int primary) { return 0; }
void spin_table_hibernate_pen_end(bool frozen) {}
void spin_table_hibernate_restored(void) {}
#endif
#endif

int spin_table_set_power_ops(const struct spin_table_power_ops *ops)
{
	return cmpxchg(&spin_table_power, NULL, ops) ? -EBUSY : 0;
}

/* A refused suspend: the held CPUs go back to the park. */
void spin_table_unhold_parked(void)
{
	WRITE_ONCE(spin_table_hold_requested, false);
	dsb(ish);
	sev();
}

/*
 * Every CPU not online is held or off. One that is neither, parked or not (a start that
 * failed, a CPU never started), may be awake anywhere.
 */
static bool spin_table_all_held(bool report)
{
	unsigned int cpu;
	bool all = true;

	for_each_present_cpu(cpu) {
		if (cpu_online(cpu) || cpumask_test_cpu(cpu, &spin_table_cpus_held) ||
		    cpumask_test_cpu(cpu, &spin_table_cpus_off))
			continue;
		if (!report)
			return false;
		pr_err("CPU%u did not answer the hold for a suspend\n", cpu);
		all = false;
	}

	return all;
}

/*
 * Before a suspend's last refusal, interrupts off, other CPUs offline: every parked CPU
 * answers and waits, still parked, for spin_table_stop_parked() or
 * spin_table_unhold_parked(). Nothing is written to the hardware. Returns -ETIMEDOUT,
 * with the hold undone, if any CPU not online is neither held nor off.
 */
int spin_table_hold_parked(void)
{
	int us;

	if (!READ_ONCE(spin_table_power))
		return -EOPNOTSUPP;

	WRITE_ONCE(spin_table_hold_requested, true);
	dsb(ish);
	sev();

	for (us = 0; us < SPIN_TABLE_WAIT_US; us++) {
		if (spin_table_all_held(false))
			return 0;
		udelay(1);
	}

	if (spin_table_all_held(true))
		return 0;
	spin_table_unhold_parked();

	return -ETIMEDOUT;
}

/*
 * At the commit, after spin_table_hold_parked(): the held CPUs stop and wait for
 * spin_table_release_stopped(). Returns -ETIMEDOUT if any didn't stop. The request
 * stays up until the wake, so a late one still stops.
 */
int spin_table_stop_parked(void)
{
	int us;

	WRITE_ONCE(spin_table_stop_requested, true);
	dsb(ish);
	sev();

	for (us = 0; us < SPIN_TABLE_WAIT_US; us++) {
		if (cpumask_empty(&spin_table_cpus_parked))
			return 0;
		udelay(1);
	}

	return -ETIMEDOUT;
}

/* After the commit: the stopped CPUs take their last WFI. */
void spin_table_release_stopped(void)
{
	WRITE_ONCE(spin_table_stop_released, true);
	dsb(ish);
	sev();
}

/*
 * Back from a suspend, before starting any CPU. Every CPU lost its power with the
 * SoC, so one that never saw the stop is started at its reset vector too.
 */
void spin_table_stop_finished(void)
{
	WRITE_ONCE(spin_table_hold_requested, false);
	WRITE_ONCE(spin_table_stop_requested, false);
	WRITE_ONCE(spin_table_stop_released, false);
	cpumask_or(&spin_table_cpus_off, &spin_table_cpus_off, &spin_table_cpus_parked);
	cpumask_clear(&spin_table_cpus_parked);
	cpumask_clear(&spin_table_cpus_held);
	cpumask_clear(&spin_table_cpus_unpark);
}

static int smp_rvbar_spin_table_cpu_boot(unsigned int cpu)
{
	u64 mpidr = cpu_logical_map(cpu);
	int ret;

	/* The CPU may run straight into the pen: release it before it starts. */
	write_pen_release(mpidr);

	if (cpumask_test_and_clear_cpu(cpu, &spin_table_cpus_penned)) {
		spin_table_pen_release(cpu);
	} else if (cpumask_test_and_clear_cpu(cpu, &spin_table_cpus_off)) {
		ret = spin_table_power->cpu_on(mpidr);
		if (ret) {
			cpumask_set_cpu(cpu, &spin_table_cpus_off);
			return ret;
		}
	} else if (cpumask_test_cpu(cpu, &spin_table_cpus_parked)) {
		cpumask_set_cpu(cpu, &spin_table_cpus_unpark);
	}

	dsb(ish);
	sev();

	return 0;
}

static bool smp_rvbar_spin_table_cpu_can_disable(unsigned int cpu)
{
	return cpu != 0;
}

static int smp_rvbar_spin_table_cpu_disable(unsigned int cpu)
{
	/*
	 * A parked CPU comes back through cpu_soft_restart(). Only stopping it for a
	 * suspend needs the platform's power ops (spin_table_hold_parked()).
	 */
	return IS_ENABLED(CONFIG_KEXEC_CORE) ? 0 : -EOPNOTSUPP;
}

#ifdef CONFIG_KEXEC_CORE
static void smp_rvbar_spin_table_restart(void)
{
	typeof(cpu_soft_restart) *restart;

	cpu_install_idmap();
	restart = (void *)__pa_symbol(cpu_soft_restart);
	restart(0, __pa_symbol(secondary_holding_pen), 0, 0, 0);
}
#else
/* Never reached: cpu_die() doesn't leave its loop without KEXEC_CORE. */
static void smp_rvbar_spin_table_restart(void)
{
}
#endif

/* Held until the commit's stop, or until a refusal sends it back to the park. */
static void smp_rvbar_spin_table_hold(unsigned int cpu, u64 mpidr)
{
	cpumask_set_cpu(cpu, &spin_table_cpus_held);

	while (READ_ONCE(spin_table_hold_requested)) {
		if (READ_ONCE(spin_table_stop_requested)) {
			spin_table_power->cpu_stop(mpidr);
			cpumask_set_cpu(cpu, &spin_table_cpus_off);
			cpumask_clear_cpu(cpu, &spin_table_cpus_held);
			cpumask_clear_cpu(cpu, &spin_table_cpus_parked);

			while (!READ_ONCE(spin_table_stop_released))
				wfe();
			spin_table_power->cpu_off(mpidr);
		}
		wfe();
	}

	cpumask_clear_cpu(cpu, &spin_table_cpus_held);
}

static void smp_rvbar_spin_table_cpu_die(unsigned int cpu)
{
	u64 mpidr = cpu_logical_map(cpu);

	/* The early-boot and crash paths get here without hotplug support. */
	if (!IS_ENABLED(CONFIG_KEXEC_CORE)) {
		while (1)
			wfe();
	}

	if (READ_ONCE(spin_table_pen_requested))
		smp_rvbar_spin_table_pen(cpu, mpidr);

	cpumask_set_cpu(cpu, &spin_table_cpus_parked);

	while (!cpumask_test_cpu(cpu, &spin_table_cpus_unpark)) {
		if (READ_ONCE(spin_table_hold_requested))
			smp_rvbar_spin_table_hold(cpu, mpidr);
		wfe();
	}

	cpumask_clear_cpu(cpu, &spin_table_cpus_unpark);
	cpumask_clear_cpu(cpu, &spin_table_cpus_parked);

	smp_rvbar_spin_table_restart();
}

static int smp_rvbar_spin_table_cpu_kill(unsigned int cpu)
{
	int us;

	for (us = 0; us < SPIN_TABLE_WAIT_US; us += 10) {
		if (cpumask_test_cpu(cpu, &spin_table_cpus_parked))
			return 0;
		if (cpumask_test_cpu(cpu, &spin_table_cpus_penned) && spin_table_pen_arrived(cpu))
			return 0;
		udelay(10);
	}

	return -ETIMEDOUT;
}

const struct cpu_operations smp_rvbar_spin_table_ops = {
	.name		= "aurora,rvbar-spin-table",
	.cpu_init	= smp_spin_table_cpu_init,
	.cpu_prepare	= smp_spin_table_cpu_prepare,
	.cpu_boot	= smp_rvbar_spin_table_cpu_boot,
	.cpu_can_disable = smp_rvbar_spin_table_cpu_can_disable,
	.cpu_disable	= smp_rvbar_spin_table_cpu_disable,
	.cpu_die	= smp_rvbar_spin_table_cpu_die,
	.cpu_kill	= smp_rvbar_spin_table_cpu_kill,
};

/*
 * An offlined parking CPU waits in this kernel's text and stack, which a kexec or a
 * hibernation restore overwrites: it is stuck for those, power ops or not.
 */
bool spin_table_cpus_stuck(void)
{
	unsigned int cpu;

	for_each_possible_cpu(cpu)
		if (get_cpu_ops(cpu) == &smp_rvbar_spin_table_ops)
			return true;
	return false;
}

#ifdef CONFIG_HIBERNATION
/*
 * For a hibernation that only holds without the pen. With it the kernel that
 * resumes an image takes its offlined CPUs out of the memory the restore
 * overwrites before it starts (spin_table_hibernate_pen_begin()), and the
 * restored kernel starts them from there.
 */
bool spin_table_hibernate_cpus_stuck(void)
{
	return !IS_ENABLED(CONFIG_KEXEC_CORE) && spin_table_cpus_stuck();
}
#endif
#endif /* CONFIG_HOTPLUG_CPU */
