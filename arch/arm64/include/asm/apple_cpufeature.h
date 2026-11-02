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

#endif
