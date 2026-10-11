/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __ASM_APPLE_IDLE_H
#define __ASM_APPLE_IDLE_H

#include <linux/jump_label.h>

#ifdef CONFIG_ARCH_APPLE
extern struct static_key_false apple_wfi_loses_regs;
static __always_inline bool apple_wfi_needs_save(void)
{
	return static_branch_unlikely(&apple_wfi_loses_regs);
}
#else
static __always_inline bool apple_wfi_needs_save(void)
{
	return false;
}
#endif

/* WFI that preserves x18-x30 on cores selected by the idle quirk. */
void apple_cpu_wfi(void);

#endif
