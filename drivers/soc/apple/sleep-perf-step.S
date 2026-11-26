/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/*
 * One performance-state step of a PMGR clock-performance register, with no
 * memory access between the write and the end of the transition: the whole
 * routine sits in one cache line, so a memory-clock change can't stall its
 * own instruction fetch half-way.
 */

#include <linux/linkage.h>
#include <asm/assembler.h>

/*
 * u32 apple_sleep_perf_step(void __iomem *reg, u32 state)
 *
 * Writes @state into the target field (bits 3:0) and waits for the busy bit
 * (31) to clear, about two seconds at most. Returns the last value read; bit
 * 31 still set means the transition didn't finish.
 */
	.balign	64
SYM_FUNC_START(apple_sleep_perf_step)
	ldr	w2, [x0]
	bfi	w2, w1, #0, #4
	dsb	sy
	isb
	str	w2, [x0]
	mov	x3, #(1 << 22)
1:	ldr	w2, [x0]
	tbz	w2, #31, 2f
	subs	x3, x3, #1
	b.ne	1b
2:	dsb	sy
	mov	w0, w2
	ret
SYM_FUNC_END(apple_sleep_perf_step)
