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

static const struct apple_sleep_ops t6040_sleep_ops = {
	/* As T8140's */
	.resume = cpu_resume,
};

static const struct apple_sleep_soc t6040_sleep_soc = {
	.unavailable_reason = "T6040 entry is not qualified",
	.ops = &t6040_sleep_ops,
	.machine = "apple,j616s",
	.ndisp = 5,
};
