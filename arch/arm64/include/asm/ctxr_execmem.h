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
 * 1 when a window is active, else 0. Written on every boot and cleaned to the
 * PoC by ctxr_exec_reserve(); the secondary and resume entries read it with the
 * MMU off. The boot CPU never reads it. See init_kernel_el.
 */
extern u32 ctxr_stay_el2;

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

/*
 * Bytes of the window the executable pool gets, or 0 with no window. Valid as
 * soon as ctxr_exec_reserve() has run, before the pool itself exists.
 */
u64 ctxr_exec_pool_bytes(void);

/* True if cores confined to the window can't run this kernel. */
bool ctxr_exec_secondaries_held(void);

/* True if @addr's kernel mapping resolves outside an active window. */
bool ctxr_exec_fetch_outside_window(unsigned long addr);
#else
static inline void ctxr_exec_reserve(void) {}
static inline void ctxr_exec_keep_reserved(void) {}
static inline bool ctxr_exec_execmem_setup(void) { return false; }
static inline bool ctxr_exec_efi_runtime_ok(void) { return true; }
static inline bool ctxr_exec_window_reserved(void) { return false; }
static inline bool ctxr_exec_secondaries_held(void) { return false; }
static inline u64 ctxr_exec_pool_bytes(void) { return 0; }
static inline bool ctxr_exec_fetch_outside_window(unsigned long addr) { return false; }
#endif

#endif /* __ASM_CTXR_EXECMEM_H */
