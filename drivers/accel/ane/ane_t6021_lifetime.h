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
 * A firmware program is shared by every file that loaded byte-identical
 * sections. Each file holds one reference per program it loaded. When the
 * last reference goes, the program is unloaded: TERMINATE_PROCESS (0x203)
 * for its process, then DESTROY_PROGRAM (0x201). Only after both are
 * acknowledged are its section buffers given back. A program whose unload
 * failed is HELD: its buffers stay and its slot is never reused until a
 * verified firmware restart resets the table.
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

/* ---- program table ---- */

/* The firmware table holds 256 programs; the 257th LOAD is refused. */
#define ANE_T6021_MAX_PROGRAMS	250
#define ANE_T6021_PROG_SECS	9
#define ANE_T6021_NO_ID		U32_MAX

#define ANE_T6021_CMD_DESTROY_PROGRAM	0x0201
#define ANE_T6021_CMD_TERMINATE_PROCESS	0x0203

enum ane_t6021_prog_state {
	ANE_T6021_PROG_FREE = 0,
	ANE_T6021_PROG_LIVE,	/* loaded, shared by the files that hold it */
	ANE_T6021_PROG_HELD,	/* load or unload uncertain: keep everything */
};

struct ane_t6021_prog {
	u8 digest[32];
	u32 prog_id;		/* opaque kernel ID, never reused */
	u32 proc_id;		/* opaque, ANE_T6021_NO_ID until a process exists */
	u32 fw_prog_id;		/* firmware wire IDs */
	u32 fw_proc_id;
	u32 users;		/* files holding a reference */
	enum ane_t6021_prog_state state;
	/* The program's own reference on each section buffer, or NULL. */
	void *sec[ANE_T6021_PROG_SECS];
};

struct ane_t6021_progtab {
	struct ane_t6021_prog progs[ANE_T6021_MAX_PROGRAMS];
	unsigned int hi;	/* slots below this index may be in use */
	u32 gen;		/* bumped by every reset */
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

/*
 * Claim a slot for a program about to be loaded. It starts HELD so nothing
 * can find it, and it owns the section references taken before the LOAD is
 * sent: if the LOAD fails, the buffers stay held with it.
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
static inline void ane_t6021_progtab_commit(struct ane_t6021_prog *p,
					    u32 prog_id, u32 fw_prog_id)
{
	p->prog_id = prog_id;
	p->fw_prog_id = fw_prog_id;
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

/* Forget IDs whose program is gone (a firmware restart reset the table). */
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
 * only for an acknowledged command. Nothing after a failure is sent.
 */
static inline int
ane_t6021_prog_unload_cmds(const struct ane_t6021_prog *p,
			   int (*cmd)(void *ctx, u16 opcode, u32 fw_prog,
				      u32 fw_proc),
			   void *ctx)
{
	int ret;

	if (p->fw_proc_id != ANE_T6021_NO_ID) {
		ret = cmd(ctx, ANE_T6021_CMD_TERMINATE_PROCESS, p->fw_prog_id,
			  p->fw_proc_id);
		if (ret)
			return ret;
	}
	return cmd(ctx, ANE_T6021_CMD_DESTROY_PROGRAM, p->fw_prog_id,
		   ANE_T6021_NO_ID);
}

/*
 * The file is closing: drop its references and unload every program whose
 * last reference this was. Returns how many programs ended HELD.
 */
static inline unsigned int
ane_t6021_progtab_fd_close(struct ane_t6021_progtab *t,
			   struct ane_t6021_fdprogs *f,
			   const struct ane_t6021_progtab_ops *ops, void *ctx)
{
	unsigned int held = 0;
	u32 i, gen;

	for (i = 0; i < f->n; i++) {
		struct ane_t6021_prog *p = ane_t6021_progtab_by_id(t, f->ids[i]);

		if (!p || !p->users)
			continue;
		if (--p->users)
			continue;
		gen = t->gen;
		if (ops->unload(ctx, p)) {
			/* A restart during the unload already reset the table. */
			if (t->gen == gen) {
				p->state = ANE_T6021_PROG_HELD;
				held++;
			}
			continue;
		}
		ops->release(ctx, p);
		memset(p, 0, sizeof(*p));
	}
	f->n = 0;
	return held;
}

/*
 * A verified firmware restart dropped every program: release all section
 * references, LIVE and HELD alike. Files keep their old IDs, which never
 * match again because kernel IDs are not reused.
 */
static inline void ane_t6021_progtab_reset(struct ane_t6021_progtab *t,
					   const struct ane_t6021_progtab_ops *ops,
					   void *ctx)
{
	unsigned int i;

	for (i = 0; i < t->hi; i++) {
		struct ane_t6021_prog *p = &t->progs[i];

		if (p->state != ANE_T6021_PROG_FREE)
			ops->release(ctx, p);
		memset(p, 0, sizeof(*p));
	}
	t->hi = 0;
	t->gen++;
}

/* Give back a reservation whose LOAD was never sent. */
static inline void ane_t6021_progtab_cancel(struct ane_t6021_prog *p,
					    const struct ane_t6021_progtab_ops *ops,
					    void *ctx)
{
	ops->release(ctx, p);
	memset(p, 0, sizeof(*p));
}

/* LIVE and HELD program counts, for diagnostics. */
static inline void ane_t6021_progtab_count(const struct ane_t6021_progtab *t,
					   unsigned int *live,
					   unsigned int *held)
{
	unsigned int i;

	*live = 0;
	*held = 0;
	for (i = 0; i < t->hi; i++) {
		if (t->progs[i].state == ANE_T6021_PROG_LIVE)
			(*live)++;
		else if (t->progs[i].state == ANE_T6021_PROG_HELD)
			(*held)++;
	}
}

#endif /* __ANE_T6021_LIFETIME_H__ */
