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
 * PRSV: with ACTLR_EL1.EnPRSV set, scalar floating-point instructions keep
 * the upper elements of the destination vector, as x86 SSE scalar
 * instructions do, instead of zeroing them. AIDR_EL1 spells it PSRV.
 */
#define AIDR_APPLE_PRSV_SHIFT	20
#define AIDR_APPLE_PRSV		BIT(20)

#define ACTLR_APPLE_PRSV_SHIFT	6
#define ACTLR_APPLE_PRSV	BIT(6)

#endif
