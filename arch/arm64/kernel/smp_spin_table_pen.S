/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Where a parking spin-table CPU waits while the kernel resuming from
 * hibernation overwrites memory with the image (smp_spin_table.c).
 */

#include <linux/linkage.h>
#include <asm/assembler.h>

/*
 * x0: the CPU's struct spin_table_pen_slot: the entry address at offset 0,
 * written by the kernel that starts the CPU again, and the arrival word at 8,
 * written here.
 *
 * Entered from cpu_soft_restart() with the MMU and caches off and interrupts
 * masked, so nothing here depends on the kernel's page tables or on cache
 * maintenance by this CPU. The code and the slots are in .data..nosave: the
 * image never contains them, so the restore leaves both as the boot kernel
 * wrote them, at the same physical address in the restored kernel.
 */
	.pushsection ".data..nosave", "aw"
	.balign	64
SYM_CODE_START(spin_table_hibernate_pen)
	movz	x1, #0x4548
	movk	x1, #0x4552, lsl #16		// "HERE"
	str	x1, [x0, #8]
	dsb	sy
1:	ldr	x1, [x0]
	cbnz	x1, 2f
	wfe
	b	1b
2:	br	x1
SYM_CODE_END(spin_table_hibernate_pen)
	.popsection
