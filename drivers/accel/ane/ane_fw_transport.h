/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef __ANE_FW_TRANSPORT_H__
#define __ANE_FW_TRANSPORT_H__

#include <linux/delay.h>

#include "ane_fw_platform.h"
#include "ane_fw_protocol.h"
#include "ane_h16_execute.h"

#define ANE_FW_IO 1
#define ANE_FW_SHAREDMALLOC 5
#define ANE_FW_IO_T2H 6
#define ANE_FW_DATA_CHAIN 7
#define ANE_FW_SHARED_MAX 64
#define ANE_FW_SHARED_LIMIT SZ_1M
#define ANE_FW_SHARED_TOTAL (12 * SZ_1M)
#define ANE_FW_SHARED_EXPP 0x45585050U
/* The firmware's text log, 0x80-byte records. */
#define ANE_FW_SHARED_LOG 0x004c4f47U

struct ane_fw_surface {
	void *cpu;
	u64 dva;
	size_t size;
	/*
	 * Backend handle of a CPU-cacheable surface the device reads and writes
	 * without snooping, kept coherent through ane_h16_session_ops.sync; 0
	 * for coherent (uncached) memory.
	 */
	u64 handle;
};

/*
 * Pacing between two polls of one wait. A command reply arrives within tens
 * of microseconds, so the first ANE_FW_POLL_SPIN_NS of a wait polls in 2 us
 * busy steps and sees it without a sleep and its wakeup latency. Up to 5 ms
 * (a whole inference) each step sleeps 20-50 us; long waits (firmware
 * startup, a stalled request) back off to 250 us and, after 50 ms, to the
 * 1 ms cadence, so they cost no more CPU than a fixed 1 ms poll did.
 * Returns the sleep in microseconds, or 0 for a busy step.
 */
#define ANE_FW_POLL_SPIN_NS (30ULL * 1000)
#define ANE_FW_POLL_SPIN_STEP_US 2
#define ANE_FW_POLL_FINE_NS (5ULL * 1000 * 1000)
#define ANE_FW_POLL_MEDIUM_NS (50ULL * 1000 * 1000)

static inline unsigned int ane_fw_poll_sleep_us(u64 waited_ns, unsigned int *max_us)
{
	unsigned int us = waited_ns < ANE_FW_POLL_SPIN_NS ? 0 :
			  waited_ns < ANE_FW_POLL_FINE_NS ? 20 :
			  waited_ns < ANE_FW_POLL_MEDIUM_NS ? 250 : 1000;

	*max_us = !us ? 0 : us == 20 ? 50 : us + 50;
	return us;
}

/* One pacing step of a wait that began waited_ns ago (process context). */
static inline void ane_fw_poll_pause(u64 waited_ns)
{
	unsigned int min_us, max_us;

	min_us = ane_fw_poll_sleep_us(waited_ns, &max_us);
	if (min_us)
		usleep_range(min_us, max_us);
	else
		udelay(ANE_FW_POLL_SPIN_STEP_US);
}

/*
 * Accessors operate on coherent owned memory and engine-relative registers.
 * Barriers must supply DMA read/write ordering, not just compiler ordering.
 * alloc returns a zeroed, pinned, private-domain mapping of exactly size bytes.
 * It and its caller retain every mapping, including a rejected result, until
 * reset. refused must log the tuple and error without exposing private data.
 */
struct ane_fw_transport_ops {
	u64 (*load64)(void *ctx, const void *p);
	void (*store64)(void *ctx, void *p, u64 value);
	void (*read_barrier)(void *ctx);
	void (*write_barrier)(void *ctx);
	u32 (*read32)(void *ctx, u32 reg);
	void (*write32)(void *ctx, u32 reg, u32 value);
	u64 (*now_ns)(void *ctx);
	/* Wait before the next poll of a wait that began waited_ns ago. */
	void (*relax)(void *ctx, u64 waited_ns);
	int (*alloc)(void *ctx, size_t size, u32 tag, struct ane_fw_surface *surface);
	void (*refused)(void *ctx, u64 word0, u64 word1, u64 word2, int error);
	/* Optional: a shared allocation was freed ("FREE") or served again ("reuse"). */
	void (*shared_note)(void *ctx, const char *event, u64 dva, u64 size, u32 tag);
};

struct ane_fw_transaction {
	u64 dva;
	u64 deadline_ns;
	size_t size;
	u16 id;
	bool active;
};

/* Caller serializes all entry points. No surface is released by this object. */
struct ane_fw_transport {
	const struct ane_fw_transport_ops *ops;
	const struct ane_fw_platform *soc;
	void *ctx;
	struct ane_fw_surface ipc;
	struct ane_fw_surface pool;
	struct ane_fw_surface shared[ANE_FW_SHARED_MAX];
	bool shared_retired[ANE_FW_SHARED_MAX];
	u32 shared_tags[ANE_FW_SHARED_MAX];
	/* FREE reply form: 1 (H16 26A428) or DVA|1 (H17 25G83); else refused. */
	bool native_free;
	bool allow_free;
	u32 free_count;
	/* H17: program and process of the last published EXECUTE. */
	bool bar_known;
	u32 bar_program, bar_process;
	struct ane_fw_channel channels[ANE_FW_CHANNELS];
	struct ane_fw_transaction pending[ANE_FW_CHANNELS];
	u32 head[ANE_FW_CHANNELS];
	u8 table[ANE_FW_TABLE_SIZE];
	size_t table_offset;
	size_t pool_used;
	size_t shared_bytes;
	u32 shared_count;
	int error;
	/* relax() calls made by every wait so far; callers diff it. */
	u64 polls;
	bool armed;
};

/* The live shared allocation the firmware asked for under tag, or NULL. */
static inline const struct ane_fw_surface *
ane_fw_transport_shared_tagged(const struct ane_fw_transport *t, u32 tag)
{
	u32 i;

	for (i = 0; i < t->shared_count; i++)
		if (t->shared_tags[i] == tag && !t->shared_retired[i])
			return &t->shared[i];
	return NULL;
}

static inline bool ane_fw_surface_valid(const struct ane_fw_surface *s)
{
	return s->cpu && s->size && s->size <= U64_MAX - s->dva;
}

static inline bool
ane_fw_surfaces_overlap(const struct ane_fw_surface *a, const struct ane_fw_surface *b)
{
	return a->dva < b->dva + b->size && b->dva < a->dva + a->size;
}

static inline int ane_fw_transport_fail(struct ane_fw_transport *t, int error)
{
	if (!t->error)
		t->error = error;
	return t->error;
}

/* pool_reserved protects the init record and any other boot-owned pool bytes. */
static inline int
ane_fw_transport_init(struct ane_fw_transport *t, const struct ane_fw_platform *soc,
		      const struct ane_fw_transport_ops *ops, void *ctx,
		      struct ane_fw_surface ipc, struct ane_fw_surface pool,
		      size_t pool_reserved, u64 table_dva, size_t table_size)
{
	int ret;

	if (!t || !soc || !ops || !ops->load64 || !ops->store64 ||
	    !ops->read_barrier || !ops->write_barrier || !ops->read32 ||
	    !ops->write32 || !ops->now_ns || !ops->relax || !ops->alloc ||
	    !ops->refused || !ane_fw_surface_valid(&ipc) ||
	    !ane_fw_surface_valid(&pool) || ane_fw_surfaces_overlap(&ipc, &pool) ||
	    !IS_ALIGNED(pool.dva, ANE_FW_SLOT_SIZE) || pool_reserved > pool.size ||
	    table_size != ANE_FW_TABLE_SIZE ||
	    !ane_fw_contains(ipc.dva, ipc.size, table_dva, table_size))
		return -EINVAL;
	memset(t, 0, sizeof(*t));
	t->ops = ops;
	t->soc = soc;
	t->ctx = ctx;
	t->ipc = ipc;
	t->pool = pool;
	t->pool_used = pool_reserved;
	t->table_offset = table_dva - ipc.dva;
	ops->read_barrier(ctx);
	memcpy(t->table, (u8 *)ipc.cpu + t->table_offset, sizeof(t->table));
	ret = ane_fw_channels_parse(t->table, ipc.dva, ipc.size, table_dva, t->channels);
	return ret ? ane_fw_transport_fail(t, ret) : 0;
}

/*
 * After the firmware was restarted (ane_fw_transport_init() again on the
 * same IPC and pool), give the new transport the shared allocations of the
 * old one, all retired: the restarted firmware owns none of them, and its
 * SHAREDMALLOC requests of the same tag and size are served from them
 * (ane_fw_shared_reusable()) instead of mapping new memory every cycle.
 */
static inline void ane_fw_transport_carry_shared(struct ane_fw_transport *t,
						 const struct ane_fw_transport *old)
{
	unsigned int i;

	t->shared_count = old->shared_count;
	t->shared_bytes = old->shared_bytes;
	for (i = 0; i < old->shared_count; i++) {
		t->shared[i] = old->shared[i];
		t->shared_tags[i] = old->shared_tags[i];
		t->shared_retired[i] = true;
	}
}

static inline int ane_fw_transport_check(struct ane_fw_transport *t)
{
	if (t->error)
		return t->error;
	t->ops->read_barrier(t->ctx);
	if (memcmp(t->table, (u8 *)t->ipc.cpu + t->table_offset, sizeof(t->table)))
		return ane_fw_transport_fail(t, -EPROTO);
	return 0;
}

static inline void *ane_fw_transport_slot(struct ane_fw_transport *t, unsigned int ch)
{
	return (u8 *)t->ipc.cpu + t->channels[ch].offset +
	       t->head[ch] * ANE_FW_SLOT_SIZE;
}

static inline void ane_fw_transport_advance(struct ane_fw_transport *t, unsigned int ch)
{
	t->head[ch] = (t->head[ch] + 1) % t->channels[ch].slots;
}

static inline void ane_fw_transport_doorbell(struct ane_fw_transport *t, unsigned int ch)
{
	t->ops->write32(t->ctx, t->soc->doorbell, BIT(t->channels[ch].bit));
}

static inline int ane_fw_transport_arm(struct ane_fw_transport *t)
{
	unsigned int ch, i;
	int ret = ane_fw_transport_check(t);

	if (ret)
		return ret;
	if (t->armed)
		return -EBUSY;
	for (ch = 0; ch < ANE_FW_CHANNELS; ch++) {
		const struct ane_fw_channel *c = &t->channels[ch];

		if (c->type)
			continue;
		for (i = 0; i < c->slots; i++) {
			u8 *p = (u8 *)t->ipc.cpu + c->offset + i * ANE_FW_SLOT_SIZE;

			t->ops->store64(t->ctx, p + 8, 0);
			t->ops->store64(t->ctx, p + 16, 0);
			t->ops->store64(t->ctx, p, 1);
		}
	}
	t->ops->write_barrier(t->ctx);
	t->armed = true;
	return 0;
}

/*
 * The H17 firmware's direct submission path reuses the engine's current BAR
 * context. When that context came from a different program, a job (of a new
 * or previously loaded program, even after the earlier one was destroyed) can
 * write through the earlier program's output binding. Bit 0 of the request
 * flags word queues the job for the full execution loop instead, which
 * installs a fresh BAR context. Queue the first EXECUTE after any change of program, process or
 * program lifetime; later jobs of the same process keep the direct path. This
 * lifts the one-program-per-firmware-session limit. Applies to the pool copy,
 * so every published EXECUTE follows the rule. H16 shares the firmware
 * architecture but has not executed under Linux yet; it is left unchanged.
 */
static inline void
ane_fw_transport_bar_context(struct ane_fw_transport *t, u8 *copy, size_t size)
{
	u16 id = get_unaligned_le16(copy + 4);
	u32 program, process;

	if (id != 0x20c) {
		/* LOAD, UNLOAD, CREATE and TERMINATE may reuse IDs. */
		if (id >= 0x200 && id <= 0x203)
			t->bar_known = false;
		return;
	}
	if (size < ANE_H16_EXECUTE_FLAGS + 4) {
		t->bar_known = false;
		return;
	}
	program = get_unaligned_le32(copy + 8);
	process = get_unaligned_le32(copy + 0xc);
	if (!t->bar_known || t->bar_program != program || t->bar_process != process)
		put_unaligned_le32(get_unaligned_le32(copy + ANE_H16_EXECUTE_FLAGS) |
				   ANE_H16_EXECUTE_FLAG_QUEUED, copy + ANE_H16_EXECUTE_FLAGS);
	t->bar_known = true;
	t->bar_program = program;
	t->bar_process = process;
}

/*
 * Whether one firmware session may run several programs: the transport
 * queues the first EXECUTE after every program change (H17, above). H16
 * shares the firmware design, but the rule is unconfirmed there.
 */
static inline bool ane_fw_transport_switch_safe(const struct ane_fw_transport *t)
{
	return t->soc->chip_family == 17;
}

/* Commands are copied into a monotonic pool arena, never external DMA memory. */
static inline int
ane_fw_transport_publish(struct ane_fw_transport *t, unsigned int ch,
			 const void *command, size_t size, u64 deadline_ns)
{
	struct ane_fw_transaction *tx;
	size_t offset;
	u8 *p;
	u64 header;
	int ret = ane_fw_transport_check(t);

	if (ret)
		return ret;
	if (!t->armed || (ch != ANE_FW_IO && ch != ANE_FW_DATA_CHAIN) ||
	    !command || size < 8 || get_unaligned_le32(command) ||
	    get_unaligned_le16((const u8 *)command + 6))
		return -EINVAL;
	if (ane_fw_command_denied(get_unaligned_le16((const u8 *)command + 4)))
		return -EPERM;
	tx = &t->pending[ch];
	if (tx->active)
		return -EBUSY;
	if (t->ops->now_ns(t->ctx) >= deadline_ns)
		return ane_fw_transport_fail(t, -ETIMEDOUT);
	p = ane_fw_transport_slot(t, ch);
	header = t->ops->load64(t->ctx, p);
	if (header & 2)
		return ane_fw_transport_fail(t, -EPROTO);
	if (!(header & 1))
		return -EBUSY;
	if (t->pool_used > SIZE_MAX - (ANE_FW_SLOT_SIZE - 1))
		return -ENOSPC;
	offset = ALIGN(t->pool_used, ANE_FW_SLOT_SIZE);
	if (offset > t->pool.size || size > t->pool.size - offset)
		return -ENOSPC;
	memcpy((u8 *)t->pool.cpu + offset, command, size);
	*tx = (struct ane_fw_transaction) {
		.dva = t->pool.dva + offset,
		.deadline_ns = deadline_ns,
		.size = size,
		.id = get_unaligned_le16((const u8 *)command + 4),
		.active = true,
	};
	t->pool_used = offset + size;
	if (ane_fw_transport_switch_safe(t))
		ane_fw_transport_bar_context(t, (u8 *)t->pool.cpu + offset, size);
	t->ops->store64(t->ctx, p + 8, size);
	t->ops->store64(t->ctx, p + 16, size);
	t->ops->write_barrier(t->ctx);
	t->ops->store64(t->ctx, p, tx->dva);
	t->ops->write_barrier(t->ctx);
	ane_fw_transport_doorbell(t, ch);
	return 0;
}

/* Command arena position, for ane_fw_transport_release(). */
static inline size_t ane_fw_transport_mark(const struct ane_fw_transport *t)
{
	return t->pool_used;
}

/*
 * Give back the arena used by every command published since @mark. The caller
 * guarantees that the firmware is done with all of them, not only that they
 * were acknowledged: an EXECUTE is done once its completion report arrived.
 * Refused while any channel still has a command in flight.
 */
static inline int ane_fw_transport_release(struct ane_fw_transport *t, size_t mark)
{
	unsigned int ch;

	if (t->error || mark > t->pool_used || mark > t->pool.size)
		return -EINVAL;
	for (ch = 0; ch < ANE_FW_CHANNELS; ch++)
		if (t->pending[ch].active)
			return -EBUSY;
	t->pool_used = mark;
	return 0;
}

/* On success/refusal the reply remains in the pool for the caller to inspect. */
static inline int ane_fw_transport_poll(struct ane_fw_transport *t, unsigned int ch)
{
	struct ane_fw_transaction *tx;
	u64 header, length, result;
	u8 *p;
	int ret = ane_fw_transport_check(t);

	if (ret)
		return ret;
	if (ch != ANE_FW_IO && ch != ANE_FW_DATA_CHAIN)
		return -EINVAL;
	tx = &t->pending[ch];
	if (!tx->active)
		return -EINVAL;
	if (t->ops->now_ns(t->ctx) >= tx->deadline_ns)
		return ane_fw_transport_fail(t, -ETIMEDOUT);
	p = ane_fw_transport_slot(t, ch);
	header = t->ops->load64(t->ctx, p);
	if (header & 2)
		return ane_fw_transport_fail(t, -EPROTO);
	if (!(header & 1)) {
		if (header != tx->dva)
			return ane_fw_transport_fail(t, -EPROTO);
		return -EAGAIN;
	}
	t->ops->read_barrier(t->ctx);
	length = t->ops->load64(t->ctx, p + 8);
	result = t->ops->load64(t->ctx, p + 16);
	ret = ane_fw_reply_status(header, length, result, tx->dva, tx->size,
				  tx->id, (u8 *)t->pool.cpu + (tx->dva - t->pool.dva), true);
	if (ret)
		return ane_fw_transport_fail(t, ret);
	tx->active = false;
	ane_fw_transport_advance(t, ch);
	return 0;
}

static inline bool ane_fw_transport_present(u64 word0, u64 word1)
{
	return !(word0 & 1) && (word0 || word1);
}

static inline const void *
ane_fw_transport_report(struct ane_fw_transport *t, u64 dva, u64 size)
{
	const struct ane_fw_surface *s;
	unsigned int i;

	for (i = 0; i < t->shared_count + 2; i++) {
		if (i >= 2 && t->shared_retired[i - 2])
			continue;
		s = i == 0 ? &t->ipc : i == 1 ? &t->pool : &t->shared[i - 2];
		if (ane_fw_contains(s->dva, s->size, dva, size))
			return (u8 *)s->cpu + (dva - s->dva);
	}
	return NULL;
}

/* Copy before acknowledging. UUID dispatch is deliberately above the transport. */
static inline int
ane_fw_transport_consume(struct ane_fw_transport *t, void *report, size_t capacity,
			 size_t *size)
{
	const void *src;
	u64 word0, word1, word2;
	u8 *p;
	int ret = ane_fw_transport_check(t);

	if (ret)
		return ret;
	if (!t->armed || !report || !size)
		return -EINVAL;
	p = ane_fw_transport_slot(t, ANE_FW_IO_T2H);
	word0 = t->ops->load64(t->ctx, p);
	t->ops->read_barrier(t->ctx);
	word1 = t->ops->load64(t->ctx, p + 8);
	if (!ane_fw_transport_present(word0, word1))
		return -EAGAIN;
	t->ops->read_barrier(t->ctx);
	word2 = t->ops->load64(t->ctx, p + 16);
	if ((word0 & 3) || word1 < 8 || word2)
		return ane_fw_transport_fail(t, -EPROTO);
	src = ane_fw_transport_report(t, word0, word1);
	if (!src)
		return ane_fw_transport_fail(t, -EPROTO);
	if (word1 > capacity)
		return -ENOSPC;
	memcpy(report, src, word1);
	*size = word1;
	/* The copy must finish before firmware is allowed to recycle the report. */
	t->ops->write_barrier(t->ctx);
	t->ops->store64(t->ctx, p, word0 | 1);
	t->ops->write_barrier(t->ctx);
	ane_fw_transport_doorbell(t, ANE_FW_IO_T2H);
	ane_fw_transport_advance(t, ANE_FW_IO_T2H);
	return 0;
}

/* Conservative default allocation policy. Native FREE requires build opt-in. */
static inline int ane_fw_shared_size(u64 word0, u64 bytes, u64 tag, size_t *size)
{
	if (word0 || !bytes)
		return -EOPNOTSUPP;
	if (tag > U32_MAX)
		return -E2BIG;
	if (bytes > ANE_FW_SHARED_LIMIT &&
	    (bytes != SZ_2M || tag != ANE_FW_SHARED_EXPP))
		return -E2BIG;
	*size = ALIGN(bytes, SZ_16K);
	return 0;
}

static inline void ane_fw_shared_note(struct ane_fw_transport *t, const char *event,
				      unsigned int i)
{
	if (t->ops->shared_note)
		t->ops->shared_note(t->ctx, event, t->shared[i].dva, t->shared[i].size,
				    t->shared_tags[i]);
}

/*
 * SHAREDMALLOC FREE of a live allocation. Backing storage, its mapping and
 * its quota stay until reset; FREE retires the surface, so no later report
 * resolves into it, and makes it available to a later request of the same
 * tag and size (ane_fw_shared_reusable()). Reply before revoking authority.
 * H16 26A428 (r5 section 6) takes the reply 1. H17 25G83 takes the observed
 * DVA|1, and its request carries zero length and handle.
 */
static inline int
ane_fw_transport_free(struct ane_fw_transport *t, void *slot, u64 dva, u64 bytes, u64 tag)
{
	unsigned int i;

	if (!t->native_free && !t->allow_free)
		return -EOPNOTSUPP;
	if (!t->native_free && (bytes || tag || !IS_ALIGNED(dva, SZ_16K)))
		return -EPROTO;
	for (i = 0; i < t->shared_count; i++)
		if (!t->shared_retired[i] && dva == t->shared[i].dva)
			break;
	if (i == t->shared_count)
		return -ENOENT;
	t->ops->store64(t->ctx, (u8 *)slot + 8, 0);
	t->ops->store64(t->ctx, (u8 *)slot + 16, 0);
	t->ops->write_barrier(t->ctx);
	t->ops->store64(t->ctx, slot, t->native_free ? 1 : dva | 1);
	t->ops->write_barrier(t->ctx);
	ane_fw_transport_doorbell(t, ANE_FW_SHAREDMALLOC);
	t->shared_retired[i] = true;
	t->free_count++;
	ane_fw_transport_advance(t, ANE_FW_SHAREDMALLOC);
	ane_fw_shared_note(t, "FREE", i);
	return 0;
}

/*
 * A retired allocation that can serve a new request of this tag and size, or
 * shared_count. Every program LOAD asks for a 2 MiB EXPP process pool and the
 * firmware frees it after UNLOAD (often only during a later program), but the
 * backing stays mapped until reset: without reuse the 12 MiB quota ran out at
 * the sixth program of a boot. Only memory the firmware itself gave back by
 * FREE is served again, and only for the same tag and size.
 */
static inline unsigned int
ane_fw_shared_reusable(const struct ane_fw_transport *t, size_t size, u64 tag)
{
	unsigned int i;

	for (i = 0; i < t->shared_count; i++)
		if (t->shared_retired[i] && t->shared[i].size == size &&
		    t->shared_tags[i] == tag)
			return i;
	return t->shared_count;
}

static inline int ane_fw_transport_shared(struct ane_fw_transport *t)
{
	struct ane_fw_surface s = {};
	u64 word0, word1, word2;
	size_t size;
	unsigned int i;
	u8 *p;
	int ret = ane_fw_transport_check(t);

	if (ret)
		return ret;
	if (!t->armed)
		return -EINVAL;
	p = ane_fw_transport_slot(t, ANE_FW_SHAREDMALLOC);
	word0 = t->ops->load64(t->ctx, p);
	t->ops->read_barrier(t->ctx);
	word1 = t->ops->load64(t->ctx, p + 8);
	if (word0 & 1)
		return -EAGAIN;
	t->ops->read_barrier(t->ctx);
	word2 = t->ops->load64(t->ctx, p + 16);
	if (!word0 && !word1 && !word2)
		return -EAGAIN;
	if (word0) {
		ret = ane_fw_transport_free(t, p, word0, word1, word2);
		if (ret)
			goto refused;
		return 0;
	}
	ret = ane_fw_shared_size(word0, word1, word2, &size);
	if (ret)
		goto refused;
	i = ane_fw_shared_reusable(t, size, word2);
	if (i < t->shared_count) {
		/* Same DVA, same handle as when first served; zeroed like new memory. */
		memset(t->shared[i].cpu, 0, size);
		t->shared_retired[i] = false;
		t->ops->store64(t->ctx, p + 8, 0);
		t->ops->store64(t->ctx, p + 16, i + 1);
		t->ops->write_barrier(t->ctx);
		t->ops->store64(t->ctx, p, t->shared[i].dva | 1);
		t->ops->write_barrier(t->ctx);
		ane_fw_transport_doorbell(t, ANE_FW_SHAREDMALLOC);
		ane_fw_transport_advance(t, ANE_FW_SHAREDMALLOC);
		ane_fw_shared_note(t, "reuse", i);
		return 0;
	}
	if (t->shared_count == ANE_FW_SHARED_MAX || size > ANE_FW_SHARED_TOTAL -
	    t->shared_bytes) {
		ret = -ENOSPC;
		goto refused;
	}
	ret = t->ops->alloc(t->ctx, size, word2, &s);
	if (ret)
		goto refused;
	if (!ane_fw_surface_valid(&s) || !IS_ALIGNED(s.dva, SZ_16K) ||
	    s.size != size || ane_fw_surfaces_overlap(&s, &t->ipc) ||
	    ane_fw_surfaces_overlap(&s, &t->pool)) {
		ret = -EPROTO;
		goto refused;
	}
	for (i = 0; i < t->shared_count; i++) {
		if (ane_fw_surfaces_overlap(&s, &t->shared[i])) {
			ret = -EPROTO;
			goto refused;
		}
	}
	t->shared_tags[t->shared_count] = word2;
	t->shared[t->shared_count++] = s;
	t->shared_bytes += size;
	t->ops->store64(t->ctx, p + 8, 0);
	t->ops->store64(t->ctx, p + 16, t->shared_count);
	t->ops->write_barrier(t->ctx);
	t->ops->store64(t->ctx, p, s.dva | 1);
	t->ops->write_barrier(t->ctx);
	ane_fw_transport_doorbell(t, ANE_FW_SHAREDMALLOC);
	ane_fw_transport_advance(t, ANE_FW_SHAREDMALLOC);
	return 0;
refused:
	t->ops->refused(t->ctx, word0, word1, word2, ret);
	return ane_fw_transport_fail(t, ret);
}

/* Read once, reject unknown channels, acknowledge exactly that snapshot. */
static inline int ane_fw_transport_irq(struct ane_fw_transport *t, u32 *pending)
{
	u32 value;

	if (t->error)
		return t->error;
	if (!pending)
		return -EINVAL;
	value = t->ops->read32(t->ctx, t->soc->irq_status);
	if (value & ~GENMASK(7, 0))
		return ane_fw_transport_fail(t, -EPROTO);
	*pending = value;
	if (value)
		t->ops->write32(t->ctx, t->soc->irq_ack, value);
	return 0;
}

/* The original absolute command deadline survives every provider/IRQ service. */
/*
 * After a program is torn down the firmware posts FREE for its process pool,
 * sometimes after the UNLOAD reply, and waits for the answer with its shared
 * memory lock held. Serve the SHAREDMALLOC ring until it has been quiet for
 * quiet_ns, for at most limit_ns. Returns the number of requests served.
 */
static inline int ane_fw_transport_settle(struct ane_fw_transport *t, u64 quiet_ns,
					  u64 limit_ns)
{
	u64 start = t->ops->now_ns(t->ctx), last = start, now;
	u32 pending;
	int ret, served = 0;

	for (;;) {
		ret = ane_fw_transport_irq(t, &pending);
		if (ret)
			return ret;
		ret = ane_fw_transport_shared(t);
		now = t->ops->now_ns(t->ctx);
		if (!ret) {
			served++;
			last = now;
		} else if (ret != -EAGAIN) {
			return ret;
		}
		if (now - last >= quiet_ns || now - start >= limit_ns)
			return served;
		t->polls++;
		t->ops->relax(t->ctx, now - last);
	}
}

static inline int ane_fw_transport_wait(struct ane_fw_transport *t, unsigned int ch)
{
	u64 start = t->ops->now_ns(t->ctx), now;
	u32 pending;
	int ret;

	for (;;) {
		ret = ane_fw_transport_poll(t, ch);
		if (ret != -EAGAIN)
			return ret;
		ret = ane_fw_transport_irq(t, &pending);
		if (ret)
			return ret;
		ret = ane_fw_transport_shared(t);
		if (ret && ret != -EAGAIN)
			return ret;
		/* Recheck before sleeping, including time spent in alloc(). */
		now = t->ops->now_ns(t->ctx);
		if (now >= t->pending[ch].deadline_ns)
			return ane_fw_transport_fail(t, -ETIMEDOUT);
		t->polls++;
		t->ops->relax(t->ctx, now - start);
	}
}

#endif /* __ANE_FW_TRANSPORT_H__ */
