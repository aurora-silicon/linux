/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/*
 * H14 firmware allocation and program lifetime rules.
 *
 * Kept free of device access so the KUnit suite (ane_t6021_lifetime_test.c)
 * runs the same code as the driver. The caller serializes every function
 * here with the firmware command lock.
 *
 * The SHAREDMALLOC request classes, the slot reuse rule for the firmware
 * allocation table and the per-file program references follow the design of
 * omarchy-ane by Joshua Warren (MIT), adapted to this driver's opaque program
 * IDs and firmware restart.
 *
 * SHAREDMALLOC (channel 5) carries one request per 64-byte slot as three
 * words: header, size, mode. A slot with header bit 0 set is host-owned.
 *  - MALLOC: header 0, size in bytes, a 32-bit mode. The host answers with
 *    the buffer IOVA | 1 in the header and a table index in word 2.
 *  - FREE: header is the IOVA the firmware gives back (16 KiB aligned, so
 *    bit 0 is clear and the header is not 0), size and mode 0. The firmware
 *    has already dropped its mapping and waits for the answer without a
 *    timeout while it holds its shared-memory lock, so the host must answer
 *    every FREE: header 1, size and mode 0. The request can arrive after the
 *    command that caused it was acknowledged.
 *  - header 0 and size 0: nothing posted yet. The slot is read again on
 *    the next poll and the cursor does not move.
 *
 * A program is shared by every file that loaded byte-identical sections and
 * outlives the firmware instance it was first loaded into. Each file holds one
 * reference per program it loaded, by an opaque kernel ID that never changes.
 * Whether the program is in the firmware right now (resident) is separate:
 *
 *  - Every live process holds a 2 MiB firmware allocation, and a LOAD sent
 *    while eight programs with processes are resident never completes. The
 *    driver keeps at most ANE_T6021_FW_RESIDENT_MAX programs resident and
 *    makes room before each LOAD.
 *  - TERMINATE_PROCESS (0x203) of a process whose program has more than one
 *    task waits inside the firmware without a timeout and never completes,
 *    and no other command frees such a process. Such a process is never
 *    terminated. Only a program proven to have exactly one task (its TD
 *    property section and its TD stream both count one) is unloaded:
 *    TERMINATE_PROCESS for its process, then DESTROY_PROGRAM (0x201).
 *  - An unreferenced program that cannot be unloaded stays resident as a
 *    cache entry; a later load of the same bytes reuses it with no command.
 *  - Room comes first from unloading a single-task program (unreferenced
 *    first, then the least recently used), and when none is left from a
 *    cold firmware restart at an idle point. A restart drops every cache
 *    entry; referenced programs become non-resident and are loaded again,
 *    with a new process, on their next use. Files see no change.
 *
 * A program whose load or unload failed is HELD: its buffers stay and its
 * slot is not reused until a verified firmware restart.
 */
#ifndef __ANE_T6021_LIFETIME_H__
#define __ANE_T6021_LIFETIME_H__

#include <linux/errno.h>
#include <linux/limits.h>
#include <linux/sizes.h>
#include <linux/string.h>
#include <linux/types.h>

/* ---- SHAREDMALLOC requests ---- */

enum ane_t6021_mring_kind {
	ANE_T6021_MRING_IDLE,		/* nothing posted in this slot */
	ANE_T6021_MRING_MALLOC,		/* allocate size bytes */
	ANE_T6021_MRING_FREE,		/* the firmware returns the IOVA in hdr */
	ANE_T6021_MRING_ANSWERED,	/* host-owned slot */
	ANE_T6021_MRING_BAD,		/* no request has this shape */
};

static inline enum ane_t6021_mring_kind
ane_t6021_mring_classify(u64 hdr, u64 size, u64 mode)
{
	if (hdr & 1)
		return ANE_T6021_MRING_ANSWERED;
	if (!hdr) {
		/* No size yet: not a request, look again later. */
		if (!size)
			return ANE_T6021_MRING_IDLE;
		return mode > U32_MAX ? ANE_T6021_MRING_BAD :
					ANE_T6021_MRING_MALLOC;
	}
	if (size || mode || hdr & (SZ_16K - 1))
		return ANE_T6021_MRING_BAD;
	return ANE_T6021_MRING_FREE;
}

/* ---- firmware allocation table ---- */

/* One buffer the firmware asked for. A slot is free when cpu is NULL. */
struct ane_legacy_buffer {
	void *cpu;
	dma_addr_t dma;
	size_t size;
};

/*
 * The slot a new buffer takes: the first free one below the high-water mark
 * @used, else @used itself while the table can grow below @cap, else -1.
 * Reusing freed slots keeps the table at the peak of live allocations
 * instead of growing with every allocation the firmware makes.
 */
static inline int ane_t6021_legtab_pick(const struct ane_legacy_buffer *t,
					u32 used, u32 cap)
{
	u32 i;

	for (i = 0; i < used; i++)
		if (!t[i].cpu)
			return i;
	return used < cap ? (int)used : -1;
}

/* The live slot holding @dma, never @keep (a buffer the driver itself uses). */
static inline int ane_t6021_legtab_find(const struct ane_legacy_buffer *t,
					u32 used, u64 dma,
					const struct ane_legacy_buffer *keep)
{
	u32 i;

	for (i = 0; i < used; i++)
		if (t[i].cpu && t[i].dma == dma && &t[i] != keep)
			return i;
	return -1;
}

/* ---- task count ---- */

/*
 * The task count of a program, from the two sections that carry it. The
 * firmware rejects a LOAD whose counts disagree, so both are read here and
 * a program is single-task only when both say one. Anything this code
 * cannot parse counts as multi-task: such a process is never terminated.
 * The TD walk follows the firmware's block walk as omarchy-ane's libane
 * section builder (Joshua Warren, MIT) documents it.
 */
#define ANE_T6021_SEC_TEXT	3	/* the TD stream */
#define ANE_T6021_SEC_TDPROP	7	/* TD property segments */
#define ANE_T6021_TDPROP_SEG	0x20
#define ANE_T6021_TDPROP_MAX_SEGS 64

static inline u32 ane_t6021_le32(const u8 *p)
{
	return p[0] | p[1] << 8 | p[2] << 16 | (u32)p[3] << 24;
}

/* Sum of blockNbr over the TD property segments: u32 count at +0, then
 * count 0x20-byte segments from +8, blockNbr at segment +4.
 */
static inline long ane_t6021_tdprop_tasks(const u8 *p, size_t size)
{
	u32 segs, i;
	u64 sum = 0;

	if (size < 8)
		return -EINVAL;
	segs = ane_t6021_le32(p);
	if (!segs || segs > ANE_T6021_TDPROP_MAX_SEGS ||
	    size < 8 + (size_t)segs * ANE_T6021_TDPROP_SEG)
		return -EINVAL;
	for (i = 0; i < segs; i++)
		sum += ane_t6021_le32(p + 8 + i * ANE_T6021_TDPROP_SEG + 4);
	return sum > INT_MAX ? -EINVAL : (long)sum;
}

/* TD blocks in the stream: a 0x10-byte header (0x30 when word 0 bit 2 is
 * set), then blocks whose u16 at +2 holds the word count in bits 10:0.
 */
static inline long ane_t6021_td_blocks(const u8 *p, size_t size)
{
	size_t cur;
	long n = 0;

	if (size < 4)
		return -EINVAL;
	cur = (ane_t6021_le32(p) & 4) ? 0x30 : 0x10;
	while (cur < size) {
		u32 words;

		if (size - cur < 4)
			return -EINVAL;
		words = (p[cur + 2] | p[cur + 3] << 8) & 0x7ff;
		if (!words)
			return -EINVAL;
		cur += ((words << 2) + 0xf) & 0x3ff0;
		n++;
	}
	return n;
}

/*
 * Header word 2 of each TD block: 0x08 opens the program on its first task
 * and 0x22 closes it on its last, 0 between, 0x2a for a single task. Every
 * decoded Apple H14 program follows this. A program with 0x2a on every task
 * of several returned from its call and then left the firmware unable to
 * complete any further command until reboot (M2, 13.5 bia firmware), so such
 * a program is refused before the firmware sees it. Returns -EINVAL for a
 * stream that parses and breaks the rule, 0 otherwise (a stream that does
 * not parse is left to the firmware's own descriptor check).
 */
static inline int ane_t6021_td_flags_check(const u8 *p, size_t size)
{
	size_t cur;
	u32 w2, last = 0;
	long n = 0;

	if (size < 4)
		return 0;
	cur = (ane_t6021_le32(p) & 4) ? 0x30 : 0x10;
	while (cur < size) {
		u32 words;

		if (size - cur < 12)
			return 0;
		words = (p[cur + 2] | p[cur + 3] << 8) & 0x7ff;
		if (words < 3)
			return 0;
		w2 = ane_t6021_le32(p + cur + 8);
		if (n == 0 && w2 != 0x08 && w2 != 0x2a)
			return -EINVAL;
		if (n > 0 && last != (n == 1 ? 0x08 : 0))
			return -EINVAL;
		last = w2;
		cur += ((words << 2) + 0xf) & 0x3ff0;
		n++;
	}
	if (!n)
		return 0;
	return last == (n == 1 ? 0x2a : 0x22) ? 0 : -EINVAL;
}

/* ---- program table ---- */

/* The firmware table holds 256 programs; the 257th LOAD is refused. */
#define ANE_T6021_MAX_PROGRAMS	250
#define ANE_T6021_PROG_SECS	9
#define ANE_T6021_NO_ID		U32_MAX
/*
 * Programs resident in the firmware at once, each with at most one process.
 * The firmware's L2 spill pool holds eight 2 MiB EXPP buffers: every
 * CREATE_PROCESS grows it while it holds fewer than eight, whatever the
 * program, and a TERMINATE may shrink it again. Its program and process
 * tables are far larger (240 and 496 advertised). On the M2 a LOAD sent with
 * eight processes resident (9 baseline + 8 x 2 MiB allocations) never
 * completed, and the firmware has no static check that explains it. So no
 * LOAD or CREATE is sent while this many programs are resident, which keeps
 * one spill buffer per live process and stays below that observed state.
 */
#define ANE_T6021_FW_RESIDENT_MAX	8

#define ANE_T6021_CMD_DESTROY_PROGRAM	0x0201
#define ANE_T6021_CMD_TERMINATE_PROCESS	0x0203

enum ane_t6021_prog_state {
	ANE_T6021_PROG_FREE = 0,
	ANE_T6021_PROG_LIVE,	/* known; shared by the files that hold it */
	ANE_T6021_PROG_HELD,	/* load or unload uncertain: keep everything */
};

struct ane_t6021_prog {
	u8 digest[32];
	u32 prog_id;		/* opaque kernel ID, never reused */
	u32 proc_id;		/* opaque, ANE_T6021_NO_ID until a process is asked for */
	u32 fw_prog_id;		/* firmware wire IDs, only while resident */
	u32 fw_proc_id;
	u32 users;		/* files holding a reference */
	enum ane_t6021_prog_state state;
	bool resident;		/* LOADed into the current firmware instance */
	bool single_task;	/* proven: its process may be terminated */
	u64 last_use;
	/* The program's own reference on each section buffer, or NULL, and
	 * the IOVA and size its LOAD names for that section.
	 */
	void *sec[ANE_T6021_PROG_SECS];
	u64 sec_iova[ANE_T6021_PROG_SECS];
	u64 sec_size[ANE_T6021_PROG_SECS];
};

struct ane_t6021_progtab {
	struct ane_t6021_prog progs[ANE_T6021_MAX_PROGRAMS];
	unsigned int hi;	/* slots below this index may be in use */
	u32 gen;		/* bumped by every firmware restart */
	u64 clock;		/* last_use source */
	u32 restarts;		/* budget restarts, for diagnostics */
};

/* The program IDs one file holds a reference on. */
struct ane_t6021_fdprogs {
	u32 n;
	u32 ids[ANE_T6021_MAX_PROGRAMS];
};

struct ane_t6021_progtab_ops {
	/* Unload from the firmware; zero only when every command was acked. */
	int (*unload)(void *ctx, struct ane_t6021_prog *p);
	/* The firmware no longer reads the sections: drop their references. */
	void (*release)(void *ctx, struct ane_t6021_prog *p);
	/* LOAD the program again; on success set fw_prog_id. */
	int (*load)(void *ctx, struct ane_t6021_prog *p);
	/* CREATE_PROCESS; on success set fw_proc_id. */
	int (*create)(void *ctx, struct ane_t6021_prog *p);
	/* Cold-restart the firmware at an idle point. Zero only when the new
	 * instance is verified, in which case it has called
	 * ane_t6021_progtab_restarted().
	 */
	int (*restart)(void *ctx);
};

/* Only LIVE programs are ever found, so HELD and stale entries are unusable. */
static inline struct ane_t6021_prog *
ane_t6021_progtab_find(struct ane_t6021_progtab *t, const u8 *digest)
{
	unsigned int i;

	for (i = 0; i < t->hi; i++)
		if (t->progs[i].state == ANE_T6021_PROG_LIVE &&
		    !memcmp(t->progs[i].digest, digest, sizeof(t->progs[i].digest)))
			return &t->progs[i];
	return NULL;
}

static inline struct ane_t6021_prog *
ane_t6021_progtab_by_id(struct ane_t6021_progtab *t, u32 prog_id)
{
	unsigned int i;

	if (prog_id == ANE_T6021_NO_ID)
		return NULL;
	for (i = 0; i < t->hi; i++)
		if (t->progs[i].state == ANE_T6021_PROG_LIVE &&
		    t->progs[i].prog_id == prog_id)
			return &t->progs[i];
	return NULL;
}

/* Programs that hold firmware state now: resident, or HELD (uncertain). */
static inline unsigned int
ane_t6021_progtab_resident(const struct ane_t6021_progtab *t)
{
	unsigned int i, n = 0;

	for (i = 0; i < t->hi; i++)
		if (t->progs[i].state == ANE_T6021_PROG_HELD ||
		    (t->progs[i].state == ANE_T6021_PROG_LIVE &&
		     t->progs[i].resident))
			n++;
	return n;
}

/* The firmware can give a resident program back without a restart: it has
 * no process, or its process runs a program proven to have one task.
 */
static inline bool ane_t6021_prog_unloadable(const struct ane_t6021_prog *p)
{
	return p->state == ANE_T6021_PROG_LIVE && p->resident &&
	       (p->fw_proc_id == ANE_T6021_NO_ID || p->single_task);
}

/*
 * Claim a slot for a program about to be loaded. It starts HELD so nothing
 * can find it, and it owns the section references taken before the LOAD is
 * sent: if the LOAD fails, the buffers stay held with it. Make room in the
 * firmware before this: a restart frees unreferenced slots, this one too.
 */
static inline struct ane_t6021_prog *
ane_t6021_progtab_reserve(struct ane_t6021_progtab *t, const u8 *digest)
{
	struct ane_t6021_prog *p = NULL;
	unsigned int i;

	for (i = 0; i < t->hi; i++) {
		if (t->progs[i].state == ANE_T6021_PROG_FREE) {
			p = &t->progs[i];
			break;
		}
	}
	if (!p) {
		if (t->hi == ANE_T6021_MAX_PROGRAMS)
			return NULL;
		p = &t->progs[t->hi++];
	}
	memset(p, 0, sizeof(*p));
	memcpy(p->digest, digest, sizeof(p->digest));
	p->prog_id = ANE_T6021_NO_ID;
	p->proc_id = ANE_T6021_NO_ID;
	p->fw_prog_id = ANE_T6021_NO_ID;
	p->fw_proc_id = ANE_T6021_NO_ID;
	p->state = ANE_T6021_PROG_HELD;
	return p;
}

/* The firmware acknowledged the LOAD: publish the program. */
static inline void ane_t6021_progtab_commit(struct ane_t6021_progtab *t,
					    struct ane_t6021_prog *p,
					    u32 prog_id, u32 fw_prog_id)
{
	p->prog_id = prog_id;
	p->fw_prog_id = fw_prog_id;
	p->resident = true;
	p->last_use = ++t->clock;
	p->state = ANE_T6021_PROG_LIVE;
}

static inline bool ane_t6021_fdprogs_has(const struct ane_t6021_fdprogs *f,
					 u32 prog_id)
{
	u32 i;

	for (i = 0; i < f->n; i++)
		if (f->ids[i] == prog_id)
			return true;
	return false;
}

/* Forget IDs whose program is gone. */
static inline void ane_t6021_fdprogs_compact(struct ane_t6021_progtab *t,
					     struct ane_t6021_fdprogs *f)
{
	u32 i, n = 0;

	for (i = 0; i < f->n; i++)
		if (ane_t6021_progtab_by_id(t, f->ids[i]))
			f->ids[n++] = f->ids[i];
	f->n = n;
}

/* True when the file can take one more program reference. */
static inline bool ane_t6021_fdprogs_room(struct ane_t6021_progtab *t,
					  struct ane_t6021_fdprogs *f)
{
	if (f->n < ANE_T6021_MAX_PROGRAMS)
		return true;
	ane_t6021_fdprogs_compact(t, f);
	return f->n < ANE_T6021_MAX_PROGRAMS;
}

/* The file takes one reference on a LIVE program, once per file. */
static inline int ane_t6021_fdprogs_ref(struct ane_t6021_progtab *t,
					struct ane_t6021_fdprogs *f,
					struct ane_t6021_prog *p)
{
	if (p->state != ANE_T6021_PROG_LIVE)
		return -ESTALE;
	if (ane_t6021_fdprogs_has(f, p->prog_id))
		return 0;
	if (!ane_t6021_fdprogs_room(t, f))
		return -ENOSPC;
	f->ids[f->n++] = p->prog_id;
	p->users++;
	return 0;
}

/*
 * The firmware commands of an unload: the process first, because destroying
 * a program that still has a process halts the firmware. @cmd returns zero
 * only for an acknowledged command. Nothing after a failure is sent, and a
 * process of a program not proven single-task is never terminated.
 */
static inline int
ane_t6021_prog_unload_cmds(const struct ane_t6021_prog *p,
			   int (*cmd)(void *ctx, u16 opcode, u32 fw_prog,
				      u32 fw_proc),
			   void *ctx)
{
	int ret;

	if (p->fw_proc_id != ANE_T6021_NO_ID) {
		if (!p->single_task)
			return -EBUSY;
		ret = cmd(ctx, ANE_T6021_CMD_TERMINATE_PROCESS, p->fw_prog_id,
			  p->fw_proc_id);
		if (ret)
			return ret;
	}
	return cmd(ctx, ANE_T6021_CMD_DESTROY_PROGRAM, p->fw_prog_id,
		   ANE_T6021_NO_ID);
}

static inline void ane_t6021_progtab_free(struct ane_t6021_prog *p,
					  const struct ane_t6021_progtab_ops *ops,
					  void *ctx)
{
	ops->release(ctx, p);
	memset(p, 0, sizeof(*p));
}

/*
 * Give a resident program back to the firmware. The program stays known
 * while a file holds it and is loaded again on its next use. A failed
 * unload marks it HELD, unless a restart during the unload already reset
 * the table. Returns zero when the program left the firmware.
 */
static inline int ane_t6021_progtab_evict(struct ane_t6021_progtab *t,
					  struct ane_t6021_prog *p,
					  const struct ane_t6021_progtab_ops *ops,
					  void *ctx)
{
	u32 gen = t->gen;
	int ret;

	if (!ane_t6021_prog_unloadable(p))
		return -EBUSY;
	ret = ops->unload(ctx, p);
	/* A restart during the unload already dropped every program. */
	if (t->gen != gen)
		return 0;
	if (ret) {
		p->state = ANE_T6021_PROG_HELD;
		return ret;
	}
	p->resident = false;
	p->fw_prog_id = ANE_T6021_NO_ID;
	p->fw_proc_id = ANE_T6021_NO_ID;
	if (!p->users)
		ane_t6021_progtab_free(p, ops, ctx);
	return 0;
}

/*
 * The file is closing: drop its references. A program whose last reference
 * this was is unloaded when the firmware can give it back, freed when it is
 * not resident, and otherwise kept resident as a cache entry. Returns how
 * many programs ended HELD.
 */
static inline unsigned int
ane_t6021_progtab_fd_close(struct ane_t6021_progtab *t,
			   struct ane_t6021_fdprogs *f,
			   const struct ane_t6021_progtab_ops *ops, void *ctx)
{
	unsigned int held = 0;
	u32 i;

	for (i = 0; i < f->n; i++) {
		struct ane_t6021_prog *p = ane_t6021_progtab_by_id(t, f->ids[i]);

		if (!p || !p->users)
			continue;
		if (--p->users)
			continue;
		if (!p->resident)
			ane_t6021_progtab_free(p, ops, ctx);
		else if (ane_t6021_prog_unloadable(p) &&
			 ane_t6021_progtab_evict(t, p, ops, ctx) &&
			 p->state == ANE_T6021_PROG_HELD)
			held++;
	}
	f->n = 0;
	return held;
}

/*
 * The resident program the firmware can give back most cheaply, never
 * @keep: an unreferenced one first, then the least recently used.
 */
static inline struct ane_t6021_prog *
ane_t6021_progtab_victim(struct ane_t6021_progtab *t,
			 const struct ane_t6021_prog *keep)
{
	struct ane_t6021_prog *best = NULL;
	unsigned int i;

	for (i = 0; i < t->hi; i++) {
		struct ane_t6021_prog *p = &t->progs[i];

		if (p == keep || !ane_t6021_prog_unloadable(p))
			continue;
		if (!best || (!p->users && best->users) ||
		    (!p->users == !best->users && p->last_use < best->last_use))
			best = p;
	}
	return best;
}

/*
 * Make room for one more resident program. Evicts single-task programs
 * first; when none is left, restarts the firmware, which drops every
 * resident program. @keep is never evicted.
 */
static inline int ane_t6021_progtab_make_room(struct ane_t6021_progtab *t,
					      const struct ane_t6021_prog *keep,
					      const struct ane_t6021_progtab_ops *ops,
					      void *ctx)
{
	unsigned int tries = ANE_T6021_FW_RESIDENT_MAX + 2;
	int ret;

	while (ane_t6021_progtab_resident(t) >= ANE_T6021_FW_RESIDENT_MAX) {
		struct ane_t6021_prog *v = ane_t6021_progtab_victim(t, keep);

		if (!tries--)
			return -EBUSY;
		if (v) {
			ret = ane_t6021_progtab_evict(t, v, ops, ctx);
		} else {
			ret = ops->restart(ctx);
			if (!ret)
				t->restarts++;
		}
		if (ret)
			return ret;
	}
	return 0;
}

/*
 * Put a LIVE program into the firmware, with a process when @proc. The
 * caller holds a file reference on it, so a restart on the way keeps it.
 */
static inline int ane_t6021_progtab_ensure(struct ane_t6021_progtab *t,
					   struct ane_t6021_prog *p, bool proc,
					   const struct ane_t6021_progtab_ops *ops,
					   void *ctx)
{
	int ret;

	if (p->state != ANE_T6021_PROG_LIVE || !p->users)
		return -ESTALE;
	if (!p->resident) {
		ret = ane_t6021_progtab_make_room(t, p, ops, ctx);
		if (ret)
			return ret;
		if (p->state != ANE_T6021_PROG_LIVE)
			return -ESTALE;
		p->fw_prog_id = ANE_T6021_NO_ID;
		p->fw_proc_id = ANE_T6021_NO_ID;
		ret = ops->load(ctx, p);
		/* Acked without an ID: the firmware may hold it after all. */
		if (ret == -EPROTO)
			p->state = ANE_T6021_PROG_HELD;
		if (ret)
			return ret;
		p->resident = true;
	}
	if (proc && p->fw_proc_id == ANE_T6021_NO_ID) {
		u32 gen = t->gen;

		ret = ops->create(ctx, p);
		if (ret == -EPROTO && t->gen == gen)
			p->state = ANE_T6021_PROG_HELD;
		if (ret)
			return ret;
		if (t->gen != gen || !p->resident)
			return -ESTALE;
	}
	p->last_use = ++t->clock;
	return 0;
}

/*
 * A verified firmware restart dropped every program and process. Programs
 * no file holds are freed; held ones become LIVE and non-resident, keep
 * their kernel IDs and section references, and load again on next use.
 */
static inline void ane_t6021_progtab_restarted(struct ane_t6021_progtab *t,
					       const struct ane_t6021_progtab_ops *ops,
					       void *ctx)
{
	unsigned int i;

	for (i = 0; i < t->hi; i++) {
		struct ane_t6021_prog *p = &t->progs[i];

		if (p->state == ANE_T6021_PROG_FREE)
			continue;
		if (!p->users || p->prog_id == ANE_T6021_NO_ID) {
			ane_t6021_progtab_free(p, ops, ctx);
			continue;
		}
		p->state = ANE_T6021_PROG_LIVE;
		p->resident = false;
		p->fw_prog_id = ANE_T6021_NO_ID;
		p->fw_proc_id = ANE_T6021_NO_ID;
	}
	while (t->hi && t->progs[t->hi - 1].state == ANE_T6021_PROG_FREE)
		t->hi--;
	t->gen++;
}

/* Give back a reservation whose LOAD was never sent. */
static inline void ane_t6021_progtab_cancel(struct ane_t6021_prog *p,
					    const struct ane_t6021_progtab_ops *ops,
					    void *ctx)
{
	ane_t6021_progtab_free(p, ops, ctx);
}

struct ane_t6021_progtab_stats {
	unsigned int live, held, resident, cached, single_task;
};

static inline void ane_t6021_progtab_count(const struct ane_t6021_progtab *t,
					   struct ane_t6021_progtab_stats *s)
{
	unsigned int i;

	memset(s, 0, sizeof(*s));
	for (i = 0; i < t->hi; i++) {
		const struct ane_t6021_prog *p = &t->progs[i];

		if (p->state == ANE_T6021_PROG_HELD) {
			s->held++;
		} else if (p->state == ANE_T6021_PROG_LIVE) {
			s->live++;
			s->resident += p->resident;
			s->cached += p->resident && !p->users;
			s->single_task += p->single_task;
		}
	}
}

/* ---- firmware restart ---- */

/*
 * A cold restart of the H14 firmware. The ASC must be proven stopped
 * (CPU_STATUS STOPPED) before its power domains go off: on T8112 an idle
 * core reads 0x2c and a halted but running one 0x28, neither stopped. A
 * planned restart first asks the idle firmware to shut down (POWER_DOWN,
 * which never completes; its dispatch is seen instead) and waits for WFI.
 * After a command timeout the command worker is blocked and nothing can
 * overtake it, so only the RUN clear is tried. If STOPPED is not seen
 * nothing is powered off, and the caller keeps the instance quarantined
 * with every mapping and its power claim.
 */
enum ane_t6021_restart_stage {
	ANE_T6021_RESTART_STOP,		/* failed before power removal */
	ANE_T6021_RESTART_CYCLE,	/* failed in the power cycle */
	ANE_T6021_RESTART_BOOT,		/* failed booting the new instance */
	ANE_T6021_RESTART_DONE,
};

struct ane_t6021_restart_ops {
	int (*power_down)(void *ctx);	/* POWER_DOWN taken by the firmware */
	int (*wait_idle)(void *ctx);	/* bounded wait for WFI */
	int (*halt)(void *ctx);		/* clear RUN, require STOPPED */
	int (*power_cycle)(void *ctx);	/* domains verified off, then on */
	int (*boot)(void *ctx);		/* cold boot and handshake */
};

static inline int ane_t6021_restart_run(const struct ane_t6021_restart_ops *ops,
					void *ctx, bool planned,
					enum ane_t6021_restart_stage *stage)
{
	int ret;

	*stage = ANE_T6021_RESTART_STOP;
	if (planned) {
		ret = ops->power_down(ctx);
		if (!ret)
			ret = ops->wait_idle(ctx);
		if (ret)
			return ret;
	}
	ret = ops->halt(ctx);
	if (ret)
		return ret;
	*stage = ANE_T6021_RESTART_CYCLE;
	ret = ops->power_cycle(ctx);
	if (ret)
		return ret;
	*stage = ANE_T6021_RESTART_BOOT;
	ret = ops->boot(ctx);
	if (ret)
		return ret;
	*stage = ANE_T6021_RESTART_DONE;
	return 0;
}

#endif /* __ANE_T6021_LIFETIME_H__ */
