// SPDX-License-Identifier: GPL-2.0

#ifndef __ASM_APPLE_CPUFEATURES_H
#define __ASM_APPLE_CPUFEATURES_H

#include <linux/bits.h>
#include <asm/sysreg.h>

#define AIDR_APPLE_TSO_SHIFT	9
#define AIDR_APPLE_TSO		BIT(9)

#define ACTLR_APPLE_TSO_SHIFT	1
#define ACTLR_APPLE_TSO		BIT(1)

/*
 * APFLG: with ACTLR_EL1.EnAPFLG set, flag-setting integer instructions also
 * compute the x86 parity flag into NZCV bit 26 and the x86 adjust flag into
 * NZCV bit 27, and MSR/MRS NZCV reach both bits. Exception entry does not
 * record them in SPSR_ELx; the CPU saves them in ASPSR_ELx (P in bit 1, A in
 * bit 2) and ERET restores them from there.
 */
#define AIDR_APPLE_APFLG_SHIFT	19
#define AIDR_APPLE_APFLG	BIT(19)

#define ACTLR_APPLE_APFLG_SHIFT	4
#define ACTLR_APPLE_APFLG	BIT(4)

#define SYS_IMP_APL_ASPSR_EL1	sys_reg(3, 6, 15, 8, 3)
#define SYS_IMP_APL_ASPSR_EL12	sys_reg(3, 6, 15, 12, 7)
#define SYS_IMP_APL_ASPSR_EL2	sys_reg(3, 6, 15, 8, 6)
#define ASPSR_APPLE_PF_SHIFT	1

/* Where the kernel keeps them in the saved user PSTATE: as NZCV shows them */
#define PSR_APPLE_PF_SHIFT	26
#define PSR_APPLE_PF_BIT	BIT(26)
#define PSR_APPLE_AF_BIT	BIT(27)

/*
 * AFP: with ACTLR_EL1.EnAFP set, floating point follows x86 SSE rules
 * (negative default NaN, FMAX/FMIN return the second operand for NaN and
 * signed-zero ties, FPCR.FZ ignored) and EL0 may access AFPCR_EL0, whose
 * DAZ and FTZ bits replace FPCR.FZ. AFPCR_EL0 is UNDEFINED at EL0 while
 * EnAFP is clear.
 */
#define AIDR_APPLE_AFP_SHIFT	23
#define AIDR_APPLE_AFP		BIT(23)

#define ACTLR_APPLE_AFP_SHIFT	5
#define ACTLR_APPLE_AFP		BIT(5)

#define SYS_IMP_APL_AFPCR_EL0	sys_reg(3, 6, 15, 2, 5)
#define AFPCR_APPLE_DAZ		BIT(0)
#define AFPCR_APPLE_FTZ		BIT(1)

/*
 * PRSV: with ACTLR_EL1.EnPRSV set, scalar floating-point instructions keep
 * the upper elements of the destination vector, as x86 SSE scalar
 * instructions do, instead of zeroing them. AIDR_EL1 spells it PSRV.
 */
#define AIDR_APPLE_PRSV_SHIFT	20
#define AIDR_APPLE_PRSV		BIT(20)

#define ACTLR_APPLE_PRSV_SHIFT	6
#define ACTLR_APPLE_PRSV	BIT(6)

#endif
