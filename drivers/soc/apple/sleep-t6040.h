/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/*
 * T6040 (M4 Pro) suspend to RAM. Included by sleep.c after the T6050 part, as that one is.
 *
 * The T6040 sleeps through the same boot object and mailbox as the other SoCs this driver
 * takes, with three CPU complexes (E, P0, P1), five display pipes whose power states have to
 * read off before a sleep, two storage PCIe lanes behind ANS and its own PMU sequence. It is
 * described here without entry: the profile matches the sleep controller node for a precise
 * refusal and cannot reach hardware preparation or suspend_set_ops() until each step of the
 * entry has been measured on a J616s and qualified.
 */

/* The CPU-start bits of the clusters E, P0 and P1: four, six and six slots, in cpu-id order. */
static const u8 t6040_cpu_start_first_bit[] = { 0, 4, 10 };

/* The slots of each cluster: E has four, a P cluster six (the ADT enables five of them on a J616s). */
static bool t6040_sleep_cpu_valid(u64 mpidr)
{
	static const u8 slots[] = { 4, 6, 6 };
	unsigned int cluster = MPIDR_AFFINITY_LEVEL(mpidr, 1), core = MPIDR_AFFINITY_LEVEL(mpidr, 0);

	return cluster < ARRAY_SIZE(slots) && core < slots[cluster] &&
	       MPIDR_AFFINITY_LEVEL(mpidr, 2) == !!cluster;
}

static const struct apple_sleep_ops t6040_sleep_ops = {
	/* As T8140's */
	.resume = cpu_resume,
};

static const struct apple_sleep_soc t6040_sleep_soc = {
	.unavailable_reason = "T6040 entry is not qualified",
	.ops = &t6040_sleep_ops,
	.machine = "apple,j616s",
	.boot_mpidr = 0x10100,
	.cpu_count = 14,
	.cpu_cluster_affinity = 1,
	.cpu_clusters = 3,
	.cpu_cores_per_cluster = 6,
	.cpu_start_first_bit = t6040_cpu_start_first_bit,
	.cpu_valid = t6040_sleep_cpu_valid,
	.ndisp = 5,
};
