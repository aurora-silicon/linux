// SPDX-License-Identifier: GPL-2.0-only
/*
 * Context switching for the Apple AMX matrix coprocessor.
 *
 * Measured on the M2 Pro (t6020) with Linux at EL2:
 *  - With AMX_CTL.EN (bit 63) clear, every AMX instruction from EL0 traps
 *    with the IMPLEMENTATION DEFINED EC 0x3f, ISS 3. m1n1 leaves it clear.
 *  - AMX SET makes the register file usable and zeroes it; CLR discards it.
 *    AMX_STATE_T bit 0 tells whether the CPU is between the two.
 *  - That state belongs to the CPU, not to the thread: a thread that moves
 *    to another CPU between SET and CLR faults there, and the CPU it left
 *    stays SET. A CPU left SET by an exited thread hard-reset the Mac.
 *  - The kernel can save and reload the register file with the same
 *    load/store instructions EL0 uses.
 *
 * So AMX starts disabled for every thread. The first AMX instruction of a
 * thread traps, which gives it a save area and enables AMX for it. At each
 * switch away from such a thread, a SET register file is saved and the CPU
 * is CLRed; switching back SETs it and reloads it. No CPU ever runs another
 * thread, or goes idle, with a thread's AMX state still SET.
 */

#include <linux/init.h>
#include <linux/preempt.h>
#include <linux/prctl.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#include <asm/apple_amx.h>
#include <asm/cpufeature.h>
#include <asm/ptrace.h>
#include <asm/sigcontext.h>
#include <asm/sysreg.h>
#include <asm/virt.h>

/* 8 X and 8 Y registers and 64 Z rows, 64 bytes each */
#define AMX_X_OFF	0
#define AMX_Y_OFF	(8 * 64)
#define AMX_Z_OFF	(16 * 64)
#define AMX_REGS_SIZE	APPLE_AMX_REGS_SIZE

struct apple_amx_state {
	u8 regs[AMX_REGS_SIZE] __aligned(128);
	bool live;		/* regs holds a SET register file */
	bool parked;		/* a KVM guest borrowed the CPU (VMM thread) */
};

static struct kmem_cache *apple_amx_cache __ro_after_init;

/* LDX 0, LDY 1, STX 2, STY 3, LDZ 4, STZ 5; operand in x0 */
#define AMX_LDX		0
#define AMX_LDY		1
#define AMX_STX		2
#define AMX_STY		3
#define AMX_LDZ		4
#define AMX_STZ		5

static __always_inline void amx_op(const u32 op, u64 operand)
{
	register u64 x0 asm("x0") = operand;

	asm volatile(".inst	(0x00201000 | (%1 << 5))"
		     : : "r" (x0), "i" (op) : "memory");
}

static __always_inline void amx_set(void)
{
	asm volatile("nop\n\tnop\n\tnop\n\t.inst	0x00201220" ::: "memory");
}

static __always_inline void amx_clr(void)
{
	asm volatile("nop\n\tnop\n\tnop\n\t.inst	0x00201221" ::: "memory");
}

/*
 * Operand: address in bits 55:0, register in bits 61:56, bit 62 moves a
 * pair of registers (128 bytes). Kernel addresses must lose their top byte.
 */
static __always_inline u64 amx_operand(const void *p, u64 reg)
{
	return ((u64)p & GENMASK_ULL(55, 0)) | (reg << 56) | BIT_ULL(62);
}

static void amx_save(struct apple_amx_state *st)
{
	u64 i;

	for (i = 0; i < 8; i += 2) {
		amx_op(AMX_STX, amx_operand(st->regs + AMX_X_OFF + i * 64, i));
		amx_op(AMX_STY, amx_operand(st->regs + AMX_Y_OFF + i * 64, i));
	}
	for (i = 0; i < 64; i += 2)
		amx_op(AMX_STZ, amx_operand(st->regs + AMX_Z_OFF + i * 64, i));
}

static void amx_load(const struct apple_amx_state *st)
{
	u64 i;

	for (i = 0; i < 8; i += 2) {
		amx_op(AMX_LDX, amx_operand(st->regs + AMX_X_OFF + i * 64, i));
		amx_op(AMX_LDY, amx_operand(st->regs + AMX_Y_OFF + i * 64, i));
	}
	for (i = 0; i < 64; i += 2)
		amx_op(AMX_LDZ, amx_operand(st->regs + AMX_Z_OFF + i * 64, i));
}

static bool amx_active(void)
{
	return read_sysreg_s(SYS_IMP_APL_AMX_STATE_T) & AMX_STATE_T_APPLE_ACTIVE;
}

static void amx_el0_enable(bool on)
{
	u64 ctl = read_sysreg_s(SYS_IMP_APL_AMX_CTL_EL1);
	u64 want = on ? ctl | AMX_CTL_APPLE_EN : ctl & ~AMX_CTL_APPLE_EN;

	if (ctl != want) {
		write_sysreg_s(want, SYS_IMP_APL_AMX_CTL_EL1);
		isb();
	}
}

static bool apple_amx_usable(void)
{
	return alternative_has_cap_unlikely(ARM64_HAS_AMX_APPLE) && apple_amx_cache;
}

/* Called from __switch_to() with preemption disabled; current is prev */
void apple_amx_thread_switch(struct task_struct *next)
{
	struct apple_amx_state *prev_st = current->thread.amx;
	struct apple_amx_state *next_st = next->thread.amx;

	if (!alternative_has_cap_unlikely(ARM64_HAS_AMX_APPLE))
		return;
	if (!prev_st && !next_st)
		return;

	if (prev_st) {
		prev_st->live = amx_active();
		if (prev_st->live) {
			amx_save(prev_st);
			amx_clr();
		}
	}

	amx_el0_enable(next_st != NULL);

	if (next_st && next_st->live) {
		amx_set();
		amx_load(next_st);
	}
}

/*
 * An AMX instruction from EL0 trapped because AMX is disabled. If the thread
 * has not used AMX yet, give it a save area, enable AMX and retry the
 * instruction. A thread that already has one only gets here if AMX_CTL lost
 * the enable bit, which is restored the same way.
 */
bool try_handle_apple_amx(struct pt_regs *regs, u32 insn)
{
	struct apple_amx_state *st;

	if ((insn & AMX_APPLE_INSN_MASK) != AMX_APPLE_INSN || !apple_amx_usable())
		return false;

	if (current->thread.amx) {
		bool retry;

		preempt_disable();
		retry = !(read_sysreg_s(SYS_IMP_APL_AMX_CTL_EL1) & AMX_CTL_APPLE_EN);
		amx_el0_enable(true);
		preempt_enable();
		return retry;
	}

	st = kmem_cache_zalloc(apple_amx_cache, GFP_KERNEL);
	if (!st)
		return false;

	preempt_disable();
	current->thread.amx = st;
	amx_el0_enable(true);
	preempt_enable();
	return true;
}

/* fork/clone: the child gets its own copy of the parent's register file */
int apple_amx_copy_thread(struct task_struct *p)
{
	struct apple_amx_state *st;

	p->thread.amx = NULL;
	if (!current->thread.amx || !apple_amx_usable())
		return 0;

	st = kmem_cache_zalloc(apple_amx_cache, GFP_KERNEL);
	if (!st)
		return -ENOMEM;

	preempt_disable();
	st->live = amx_active();
	if (st->live)
		amx_save(st);
	preempt_enable();

	p->thread.amx = st;
	return 0;
}

void apple_amx_exec(void)
{
	struct apple_amx_state *st = current->thread.amx;

	if (!st)
		return;

	preempt_disable();
	if (amx_active())
		amx_clr();
	current->thread.amx = NULL;
	amx_el0_enable(false);
	preempt_enable();

	kmem_cache_free(apple_amx_cache, st);
}

void apple_amx_release(struct task_struct *tsk)
{
	if (tsk->thread.amx) {
		kmem_cache_free(apple_amx_cache, tsk->thread.amx);
		tsk->thread.amx = NULL;
	}
}

static struct apple_amx_state *apple_amx_alloc(struct task_struct *t)
{
	if (!t->thread.amx)
		t->thread.amx = kmem_cache_zalloc(apple_amx_cache, GFP_KERNEL);
	return t->thread.amx;
}

/*
 * Move a running thread's SET register file into its save area and CLR the
 * CPU, so that nothing is left live. Returns whether the thread was SET.
 */
static bool amx_park_current(void)
{
	struct apple_amx_state *st = current->thread.amx;
	bool set;

	preempt_disable();
	set = amx_active();
	if (set) {
		amx_save(st);
		amx_clr();
	}
	st->live = false;
	preempt_enable();
	return set;
}

/* Signal delivery: a frame record is only needed while the thread is SET */
bool apple_amx_signal_needed(void)
{
	bool set;

	if (!current->thread.amx || !apple_amx_usable())
		return false;

	preempt_disable();
	set = amx_active();
	preempt_enable();
	return set;
}

/* Save the SET register file in the frame; the handler starts clear */
int apple_amx_preserve(struct apple_amx_context __user *ctx)
{
	struct apple_amx_state *st = current->thread.amx;
	u32 flags = 0;
	int err = 0;

	if (st && amx_park_current())
		flags = APPLE_AMX_SIG_SET;

	__put_user_error(APPLE_AMX_MAGIC, &ctx->head.magic, err);
	__put_user_error(sizeof(*ctx), &ctx->head.size, err);
	__put_user_error(flags, &ctx->flags, err);
	__put_user_error(0, &ctx->__reserved, err);
	if (!err && flags && copy_to_user(ctx->regs, st->regs, sizeof(ctx->regs)))
		err = -EFAULT;
	return err;
}

/* sigreturn: drop whatever the handler left and reload the saved file */
int apple_amx_restore(struct apple_amx_context __user *ctx, u32 size)
{
	struct apple_amx_state *st;
	u32 flags;
	int err = 0;

	if (size != sizeof(*ctx) || !apple_amx_usable())
		return -EINVAL;

	__get_user_error(flags, &ctx->flags, err);
	if (err)
		return err;
	if (flags & ~APPLE_AMX_SIG_SET)
		return -EINVAL;

	st = apple_amx_alloc(current);
	if (!st)
		return -ENOMEM;

	amx_park_current();
	if (!(flags & APPLE_AMX_SIG_SET))
		return 0;

	if (copy_from_user(st->regs, ctx->regs, sizeof(st->regs)))
		return -EFAULT;

	preempt_disable();
	amx_el0_enable(true);
	amx_set();
	amx_load(st);
	preempt_enable();
	return 0;
}

bool apple_amx_regset_active(struct task_struct *t)
{
	return apple_amx_usable() && t->thread.amx;
}

/*
 * ptrace and core dumps. A stopped task's file is in its save area; for the
 * dumping task itself, copy the live file out without changing its state.
 */
int apple_amx_regset_get(struct task_struct *t, struct user_apple_amx *u)
{
	struct apple_amx_state *st = t->thread.amx;

	if (!apple_amx_usable())
		return -EINVAL;

	memset(u, 0, sizeof(*u));
	if (!st)
		return 0;

	if (t == current) {
		preempt_disable();
		if (amx_active()) {
			amx_save(st);
			u->flags = USER_APPLE_AMX_SET;
		}
		preempt_enable();
	} else if (st->live) {
		u->flags = USER_APPLE_AMX_SET;
	}

	if (u->flags)
		memcpy(u->regs, st->regs, sizeof(u->regs));
	return 0;
}

int apple_amx_regset_set(struct task_struct *t, const struct user_apple_amx *u)
{
	struct apple_amx_state *st;

	if (!apple_amx_usable() || t == current)
		return -EINVAL;
	if (u->flags & ~USER_APPLE_AMX_SET)
		return -EINVAL;

	st = apple_amx_alloc(t);
	if (!st)
		return -ENOMEM;

	/* The stopped task loads this the next time it is switched in */
	memcpy(st->regs, u->regs, sizeof(st->regs));
	st->live = u->flags & USER_APPLE_AMX_SET;
	return 0;
}

/*
 * KVM guests. With the guest's AMX_CTL_EL1 in the EL1 copy and EN_EL1 in the
 * EL2 copy, AMX works at guest EL1 and EL0, and a guest's AMX instruction
 * with its own EN clear traps to the guest's EL1. The register file is the
 * CPU's, so vcpu_load/vcpu_put move it like a context switch does, and park
 * the VMM thread's own SET file meanwhile. Kernel AMX accesses need EN.
 */
static u64 amx_kernel_begin(void)
{
	u64 ctl = read_sysreg_s(SYS_IMP_APL_AMX_CTL_EL1);

	if (!(ctl & AMX_CTL_APPLE_EN)) {
		write_sysreg_s(ctl | AMX_CTL_APPLE_EN, SYS_IMP_APL_AMX_CTL_EL1);
		isb();
	}
	return ctl;
}

static void amx_kernel_end(u64 ctl)
{
	write_sysreg_s(ctl, SYS_IMP_APL_AMX_CTL_EL1);
	isb();
}

struct apple_amx_state *apple_amx_vcpu_alloc(void)
{
	if (!apple_amx_usable() || !is_kernel_in_hyp_mode())
		return NULL;
	return kmem_cache_zalloc(apple_amx_cache, GFP_KERNEL_ACCOUNT);
}

void apple_amx_vcpu_free(struct apple_amx_state *guest)
{
	if (guest)
		kmem_cache_free(apple_amx_cache, guest);
}

void apple_amx_vcpu_reset(struct apple_amx_state *guest)
{
	if (guest)
		guest->live = false;
}

/* Called with preemption disabled */
void apple_amx_vcpu_load(struct apple_amx_state *guest, u64 guest_ctl)
{
	struct apple_amx_state *host = current->thread.amx;
	u64 ctl;

	if (!guest)
		return;

	ctl = amx_kernel_begin();
	if (host && !host->parked && amx_active()) {
		amx_save(host);
		amx_clr();
		host->parked = true;
	}

	write_sysreg_s(guest_ctl & AMX_CTL_APPLE_GUEST_BITS,
		       SYS_IMP_APL_AMX_CTL_EL12);
	if (guest->live) {
		amx_set();
		amx_load(guest);
	}
	amx_kernel_end(ctl | AMX_CTL_APPLE_EN_EL1);
}

/* Called with preemption disabled */
void apple_amx_vcpu_put(struct apple_amx_state *guest)
{
	struct apple_amx_state *host = current->thread.amx;
	u64 ctl;

	if (!guest)
		return;

	ctl = amx_kernel_begin();
	guest->live = amx_active();
	if (guest->live) {
		amx_save(guest);
		amx_clr();
	}

	if (host && host->parked) {
		amx_set();
		amx_load(host);
		host->parked = false;
	}
	amx_kernel_end(ctl & ~AMX_CTL_APPLE_EN_EL1);
}

/*
 * The loaded guest wrote its AMX_CTL_EL1. Only the control changes: its
 * register file is live on the CPU and must not be reloaded.
 */
void apple_amx_vcpu_set_ctl(struct apple_amx_state *guest, u64 guest_ctl)
{
	if (!guest)
		return;

	write_sysreg_s(guest_ctl & AMX_CTL_APPLE_GUEST_BITS,
		       SYS_IMP_APL_AMX_CTL_EL12);
	isb();
}

/* The guest's AMX_STATE_T: the CPU's while the vCPU is loaded */
bool apple_amx_vcpu_active(struct apple_amx_state *guest, bool loaded)
{
	if (loaded)
		return amx_active();
	return guest && guest->live;
}

int apple_amx_get(void)
{
	if (!apple_amx_usable())
		return -EINVAL;

	return current->thread.amx ? PR_AURORA_AMX_IN_USE : PR_AURORA_AMX_AVAILABLE;
}

static int __init apple_amx_init(void)
{
	if (!alternative_has_cap_unlikely(ARM64_HAS_AMX_APPLE))
		return 0;

	apple_amx_cache = kmem_cache_create("apple_amx_state",
					    sizeof(struct apple_amx_state),
					    128, SLAB_PANIC, NULL);
	return 0;
}
core_initcall(apple_amx_init);
