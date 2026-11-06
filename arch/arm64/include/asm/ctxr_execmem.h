/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Confine the kernel's executable code to a fixed physical window so it can
 * run at EL2 behind an Apple-silicon CTXR (code text execute range).
 */
#ifndef __ASM_CTXR_EXECMEM_H
#define __ASM_CTXR_EXECMEM_H

#include <linux/types.h>

#ifdef CONFIG_ARM64_CTXR_EXECMEM
/*
 * Parse the window (from the kernel command line or the device tree), validate
 * that the kernel image fits inside it, and reserve it from memblock. Called
 * once from arm64_memblock_init(), while memblock is still authoritative.
 */
void ctxr_exec_reserve(void);

/* Close any hole left in the window before memblock releases free memory. */
void ctxr_exec_keep_reserved(void);

/*
 * Build the gen_pool over the reserved window and register it with the execmem
 * confinement core. Called from execmem_arch_setup(). Returns true when
 * confinement is active and the caller should flag its executable ranges with
 * EXECMEM_CONFINE.
 */
bool ctxr_exec_execmem_setup(void);

/*
 * False if a window is active and EFI runtime code can't run behind it.
 * Warns once when it returns false.
 */
bool ctxr_exec_efi_runtime_ok(void);

/*
 * True once ctxr_exec_reserve() has reserved a window. Valid from
 * arm64_memblock_init() until the end of init.
 */
bool ctxr_exec_window_reserved(void);
#else
static inline void ctxr_exec_reserve(void) {}
static inline void ctxr_exec_keep_reserved(void) {}
static inline bool ctxr_exec_execmem_setup(void) { return false; }
static inline bool ctxr_exec_efi_runtime_ok(void) { return true; }
static inline bool ctxr_exec_window_reserved(void) { return false; }
#endif

#endif /* __ASM_CTXR_EXECMEM_H */
