/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __ASM_APPLE_AMX_H
#define __ASM_APPLE_AMX_H

#include <linux/bits.h>
#include <linux/types.h>
#include <asm/sysreg.h>

/*
 * Apple AMX, the matrix coprocessor of Apple CPUs (AIDR_EL1 bit 4). EL0
 * drives it with instructions in the reserved 0x00201000 encoding space.
 */
#define AIDR_APPLE_AMX_SHIFT		4

/* EL0 access control; the EL2 copy when the kernel runs at EL2 (VHE) */
#define SYS_IMP_APL_AMX_CTL_EL1		sys_reg(3, 4, 15, 1, 4)
#define AMX_CTL_APPLE_EN		BIT_ULL(63)
/* SET while already SET faults (m1n1 leaves it set); a guest owns its copy */
#define AMX_CTL_APPLE_SET_FAULT		BIT_ULL(8)
#define AMX_CTL_APPLE_GUEST_BITS	(AMX_CTL_APPLE_EN | AMX_CTL_APPLE_SET_FAULT)
/* In the EL2 copy: let EL1 (a guest kernel) use AMX as its own copy allows */
#define AMX_CTL_APPLE_EN_EL1		BIT_ULL(62)
/* The EL1 copy, from EL2 with HCR_EL2.E2H set */
#define SYS_IMP_APL_AMX_CTL_EL12	sys_reg(3, 4, 15, 4, 6)

/* Bit 0 is set on this CPU between the AMX SET and CLR instructions */
#define SYS_IMP_APL_AMX_STATE_T		sys_reg(3, 4, 15, 1, 3)
#define AMX_STATE_T_APPLE_ACTIVE	BIT_ULL(0)

/* An AMX instruction while AMX is disabled: EC 0x3f, ISS 3 (ESR 0xfe000003) */
#define ESR_ELx_EC_APPLE_IMPDEF		0x3f
#define ESR_APPLE_ISS_AMX_DISABLED	3

#define AMX_APPLE_INSN_MASK		0xfffffc00
#define AMX_APPLE_INSN			0x00201000

#define APPLE_AMX_REGS_SIZE		(80 * 64)

struct task_struct;
struct pt_regs;
struct apple_amx_context;
struct user_apple_amx;

#ifdef CONFIG_ARM64_APPLE_AMX
void apple_amx_thread_switch(struct task_struct *next);
int apple_amx_copy_thread(struct task_struct *p);
void apple_amx_release(struct task_struct *tsk);
void apple_amx_exec(void);
bool try_handle_apple_amx(struct pt_regs *regs, u32 insn);
int apple_amx_get(void);
bool apple_amx_signal_needed(void);
int apple_amx_preserve(struct apple_amx_context __user *ctx);
int apple_amx_restore(struct apple_amx_context __user *ctx, u32 size);
struct apple_amx_state *apple_amx_vcpu_alloc(void);
void apple_amx_vcpu_free(struct apple_amx_state *guest);
void apple_amx_vcpu_reset(struct apple_amx_state *guest);
void apple_amx_vcpu_load(struct apple_amx_state *guest, u64 guest_ctl);
void apple_amx_vcpu_put(struct apple_amx_state *guest);
void apple_amx_vcpu_set_ctl(struct apple_amx_state *guest, u64 guest_ctl);
bool apple_amx_vcpu_active(struct apple_amx_state *guest, bool loaded);
bool apple_amx_regset_active(struct task_struct *t);
int apple_amx_regset_get(struct task_struct *t, struct user_apple_amx *u);
int apple_amx_regset_set(struct task_struct *t, const struct user_apple_amx *u);
#else
static inline struct apple_amx_state *apple_amx_vcpu_alloc(void) { return NULL; }
static inline void apple_amx_vcpu_free(struct apple_amx_state *guest) { }
static inline void apple_amx_vcpu_reset(struct apple_amx_state *guest) { }
static inline void apple_amx_vcpu_load(struct apple_amx_state *guest, u64 guest_ctl) { }
static inline void apple_amx_vcpu_put(struct apple_amx_state *guest) { }
static inline void apple_amx_vcpu_set_ctl(struct apple_amx_state *guest, u64 guest_ctl) { }
static inline bool apple_amx_vcpu_active(struct apple_amx_state *guest, bool loaded) { return false; }
static inline bool apple_amx_signal_needed(void) { return false; }
static inline int apple_amx_preserve(struct apple_amx_context __user *ctx) { return -EINVAL; }
static inline int apple_amx_restore(struct apple_amx_context __user *ctx, u32 size) { return -EINVAL; }
static inline void apple_amx_thread_switch(struct task_struct *next) { }
static inline int apple_amx_copy_thread(struct task_struct *p) { return 0; }
static inline void apple_amx_release(struct task_struct *tsk) { }
static inline void apple_amx_exec(void) { }
static inline bool try_handle_apple_amx(struct pt_regs *regs, u32 insn) { return false; }
#endif

#endif /* __ASM_APPLE_AMX_H */
