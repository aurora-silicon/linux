/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef __ANE_FW_TRANSPORT_H__
#define __ANE_FW_TRANSPORT_H__

#include "ane_fw_platform.h"
#include "ane_fw_protocol.h"

#define ANE_FW_TERMINAL 0
#define ANE_FW_IO 1
#define ANE_FW_SHAREDMALLOC 5
#define ANE_FW_IO_T2H 6
#define ANE_FW_DATA_CHAIN 7
#define ANE_FW_SHARED_MAX 64
#define ANE_FW_SHARED_LIMIT SZ_1M
#define ANE_FW_SHARED_TOTAL (12 * SZ_1M)
#define ANE_FW_SHARED_EXPP 0x45585050U

struct ane_fw_surface {
	void *cpu;
	u64 dva;
	size_t size;
};

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
	void (*relax)(void *ctx);
	int (*alloc)(void *ctx, size_t size, u32 tag, struct ane_fw_surface *surface);
	void (*refused)(void *ctx, u64 word0, u64 word1, u64 word2, int error);
};

struct ane_fw_transaction {
	u64 dva;
	u64 deadline_ns;
	u64 rung_ns;
	u32 rerings;
	size_t size;
	u16 id;
	bool active;
};

/* A doorbell the firmware misses while it initializes is not repeated by it. */
#define ANE_FW_RERING_NS (10 * NSEC_PER_MSEC)

/* Caller serializes all entry points. No surface is released by this object. */
struct ane_fw_transport {
	const struct ane_fw_transport_ops *ops;
	const struct ane_fw_platform *soc;
	void *ctx;
	struct ane_fw_surface ipc;
	struct ane_fw_surface pool;
	struct ane_fw_surface shared[ANE_FW_SHARED_MAX];
	struct ane_fw_channel channels[ANE_FW_CHANNELS];
	struct ane_fw_transaction pending[ANE_FW_CHANNELS];
	u32 head[ANE_FW_CHANNELS];
	u8 table[ANE_FW_TABLE_SIZE];
	size_t table_offset;
	size_t pool_used;
	size_t shared_bytes;
	u32 shared_count;
	u64 terminal_messages;
	u64 terminal_bytes;
	int error;
	bool armed;
	bool native_free;
	bool shared_retired[ANE_FW_SHARED_MAX];
};

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
		.rung_ns = t->ops->now_ns(t->ctx),
	};
	t->pool_used = offset + size;
	t->ops->store64(t->ctx, p + 8, size);
	t->ops->store64(t->ctx, p + 16, size);
	t->ops->write_barrier(t->ctx);
	t->ops->store64(t->ctx, p, tx->dva);
	t->ops->write_barrier(t->ctx);
	ane_fw_transport_doorbell(t, ch);
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

/* 25F84 _IOProcessorChannelUnidirectionalReceive (0x929a260): type-2
 * TERMINAL owns a slot when bit0 is clear, masks pointer bits[1:0], and
 * returns only header=1 to the producer. It does not echo the DVA or ring
 * a doorbell. Drain at most one ring per service; retain all backing pages.
 * Text is deliberately discarded here, avoiding firmware printf backpressure.
 */
static inline int ane_fw_transport_terminal(struct ane_fw_transport *t)
{
	unsigned int i;
	int ret = ane_fw_transport_check(t);

	if (ret)
		return ret;
	if (!t->armed)
		return -EINVAL;
	if (t->channels[ANE_FW_TERMINAL].type != 2)
		return 0;
	for (i = 0; i < t->channels[ANE_FW_TERMINAL].slots; i++) {
		u8 *slot = ane_fw_transport_slot(t, ANE_FW_TERMINAL);
		u64 header = t->ops->load64(t->ctx, slot), bytes, dva;

		t->ops->read_barrier(t->ctx);
		if (header & 1)
			break;
		bytes = t->ops->load64(t->ctx, slot + 8);
		dva = header & ~3ULL;
		if (bytes && !ane_fw_transport_report(t, dva, bytes))
			return ane_fw_transport_fail(t, -EPROTO);
		t->terminal_messages++;
		t->terminal_bytes += bytes;
		t->ops->write_barrier(t->ctx);
		t->ops->store64(t->ctx, slot, 1);
		t->ops->write_barrier(t->ctx);
		ane_fw_transport_advance(t, ANE_FW_TERMINAL);
	}
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

/* H16 26A428 native FREE, r5 section 6. Reply before revoking authority.
 * The backend retains backing storage, mappings and quota until reset.
 */
static inline int
ane_fw_transport_native_free(struct ane_fw_transport *t, void *slot, u64 dva)
{
	unsigned int i;

	for (i = 0; i < t->shared_count; i++)
		if (!t->shared_retired[i] && dva == t->shared[i].dva)
			break;
	if (i == t->shared_count)
		return -ENOENT;
	t->ops->store64(t->ctx, (u8 *)slot + 8, 0);
	t->ops->store64(t->ctx, (u8 *)slot + 16, 0);
	t->ops->write_barrier(t->ctx);
	t->ops->store64(t->ctx, slot, 1);
	t->ops->write_barrier(t->ctx);
	ane_fw_transport_doorbell(t, ANE_FW_SHAREDMALLOC);
	t->shared_retired[i] = true;
	ane_fw_transport_advance(t, ANE_FW_SHAREDMALLOC);
	return 0;
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
	if (t->native_free && word0) {
		ret = ane_fw_transport_native_free(t, p, word0);
		if (ret)
			goto refused;
		return 0;
	}
	ret = ane_fw_shared_size(word0, word1, word2, &size);
	if (ret)
		goto refused;
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

/* Drain one coalesced IRQ's allocation requests before sleeping. Bound a
 * pass by the ring size; caller rechecks target/deadline after any progress.
 */
static inline int ane_fw_transport_shared_drain(struct ane_fw_transport *t,
					       bool *progress)
{
	unsigned int i;
	int ret = ane_fw_transport_check(t);

	if (ret)
		return ret;
	if (!progress || !t->armed)
		return -EINVAL;
	*progress = false;
	for (i = 0; i < t->channels[ANE_FW_SHAREDMALLOC].slots; i++) {
		ret = ane_fw_transport_shared(t);
		if (ret == -EAGAIN)
			return 0;
		if (ret)
			return ret;
		*progress = true;
	}
	return 0;
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

/*
 * Ring a still-pending command's doorbell again after ANE_FW_RERING_NS. Only
 * called while the slot is still the firmware's (poll returned -EAGAIN).
 */
static inline void ane_fw_transport_rering(struct ane_fw_transport *t, unsigned int ch)
{
	struct ane_fw_transaction *tx = &t->pending[ch];
	u64 now = t->ops->now_ns(t->ctx);

	if (!tx->active || now - tx->rung_ns < ANE_FW_RERING_NS)
		return;
	tx->rung_ns = now;
	tx->rerings++;
	ane_fw_transport_doorbell(t, ch);
}

/* The original absolute command deadline survives every provider/IRQ service. */
static inline int ane_fw_transport_wait(struct ane_fw_transport *t, unsigned int ch)
{
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
		ret = ane_fw_transport_terminal(t);
		if (ret)
			return ret;
		/* Recheck before sleeping, including time spent in alloc(). */
		if (t->ops->now_ns(t->ctx) >= t->pending[ch].deadline_ns)
			return ane_fw_transport_fail(t, -ETIMEDOUT);
		ane_fw_transport_rering(t, ch);
		t->ops->relax(t->ctx);
	}
}

#endif /* __ANE_FW_TRANSPORT_H__ */
