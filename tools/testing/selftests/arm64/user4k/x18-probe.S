/* SPDX-License-Identifier: GPL-2.0-only */
#include <asm/unistd.h>

	.text
	.globl x18_syscall
	.type x18_syscall, %function
/* (nr, arg0, arg1, arg2, observed_x18); no libc code inside the probe. */
x18_syscall:
	hint	34			/* bti c */
	stp	x18, x19, [sp, #-16]!
	mov	x19, x4
	movz	x18, #0x3210
	movk	x18, #0x7654, lsl #16
	movk	x18, #0xba98, lsl #32
	movk	x18, #0xfedc, lsl #48
	mov	x8, x0
	mov	x0, x1
	mov	x1, x2
	mov	x2, x3
	svc	#0
	str	x18, [x19]
	ldp	x18, x19, [sp], #16
	ret
	.size x18_syscall, .-x18_syscall
	.section .note.GNU-stack,"",%progbits
