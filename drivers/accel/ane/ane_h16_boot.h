/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef __ANE_H16_BOOT_H__
#define __ANE_H16_BOOT_H__

#include "ane_fw_transport.h"

enum ane_h16_stage {
	ANE_H16_REFUSE,
	ANE_H16_PREFLIGHT,
	ANE_H16_MAP,
	ANE_H16_BOOT,
	ANE_H16_STARTUP,
};

enum ane_h16_window {
	ANE_H16_ENGINE,
	ANE_H16_PMGR,
	ANE_H16_XL,
	ANE_H16_DART0,
	ANE_H16_DART1,
	ANE_H16_DART2,
	ANE_H16_DAPF0,
	ANE_H16_DAPF1,
	ANE_H16_DAPF2,
	ANE_H16_WINDOWS,
};

struct ane_h16_tunable {
	u32 offset;
	u32 mask;
	u32 value;
};

struct ane_h16_tunables {
	enum ane_h16_window window;
	u32 base;
	const struct ane_h16_tunable *values;
	size_t count;
	bool c1ppt;
	u32 lock_offset;
	u32 lock_mask;
};

/* Selected only after the immutable resident firmware identity passes. */
struct ane_h16_build {
	const char *identity;
	u32 cpu_control;
	u32 rvbar;
	u32 ce;
	u32 ce_delays;
	u32 ce_delay_us;
	u32 ready;
	u32 run;
	u32 scratch_polls;
	u32 scratch_delay_us;
	u64 command_timeout_ns;
	u64 pool_dva;
	u64 ipc_dva;
	u64 heap_dva;
	u64 shared_dva;
	u64 expected_rvbar;
	u32 pool_size;
	u32 init_offset;
	u32 init_size;
	u32 config_span;
	u32 config_window;
	u32 template_count;
	u32 template_word;
	u32 template_value;
	u32 max_channels;
	u32 max_heap;
	u32 dart_fault;
	u32 dart_address_lo;
	u32 dart_address_hi;
	u32 dapf_version;
	/* Expected low half of the DART word at dapf_version; DAPF instances. */
	u32 dapf_version_value;
	u32 dapf_instances;
	u32 dapf_protect;
	u32 dapf_lock;
	u32 dapf_records;
	u32 dapf_stride;
	bool native_free;
	bool dapf_layout_known;
	bool c1ppt;
	const struct ane_h16_tunables *tunables;
	size_t tunable_count;
	struct {
		u16 id;
		u16 size;
	} startup[4];
};

struct ane_h16_boot;
struct ane_h16_boot_ops {
	u32 (*read32)(void *ctx, enum ane_h16_window window, u32 offset);
	u64 (*read64)(void *ctx, enum ane_h16_window window, u32 offset);
	void (*write32)(void *ctx, enum ane_h16_window window, u32 offset, u32 value);
	void (*write64)(void *ctx, enum ane_h16_window window, u32 offset, u64 value);
	void (*delay_us)(void *ctx, unsigned int us);
	int (*map)(void *ctx);
	int (*alloc_at)(void *ctx, u64 dva, size_t size, struct ane_fw_surface *s);
	int (*pin)(void *ctx);
	void (*mailbox)(void *ctx);
	void (*log)(void *ctx, const char *item, u64 a, u64 b);
	void (*quarantine)(void *ctx, int error);
};

/* One caller, one attempt. No mapping is released here, even on success. */
struct ane_h16_boot {
	const struct ane_fw_platform *soc;
	const struct ane_h16_build *build;
	const struct ane_h16_boot_ops *ops;
	const struct ane_fw_transport_ops *transport_ops;
	void *ctx;
	struct ane_fw_transport transport;
	struct ane_fw_surface pool;
	struct ane_fw_surface ipc;
	struct ane_fw_surface heap;
	u64 ranges[4][2];
	u32 requests[5];
	u32 tick;
	u32 startup_drain_ms; /* Diagnostic only: service FW before first IO. */
	u8 resource_info[100];
	int error;
	bool attempted;
	bool ran;
	bool quarantined;
};

static inline int ane_h16_stage_parse(const char *name)
{
	if (!name || !*name)
		return ANE_H16_REFUSE;
	if (!strcmp(name, "preflight"))
		return ANE_H16_PREFLIGHT;
	if (!strcmp(name, "map"))
		return ANE_H16_MAP;
	if (!strcmp(name, "boot"))
		return ANE_H16_BOOT;
	if (!strcmp(name, "startup"))
		return ANE_H16_STARTUP;
	return -EINVAL;
}

static inline u32 ane_h16_read(struct ane_h16_boot *b, u32 cell)
{
	return b->ops->read32(b->ctx, ANE_H16_ENGINE, b->soc->scratch + cell * 4);
}

static inline void ane_h16_write(struct ane_h16_boot *b, u32 cell, u32 value)
{
	b->ops->write32(b->ctx, ANE_H16_ENGINE, b->soc->scratch + cell * 4, value);
}

static inline int ane_h16_faults(struct ane_h16_boot *b, bool log)
{
	const struct ane_h16_build *f = b->build;
	unsigned int i;
	int ret = 0;

	for (i = 0; i < 3; i++) {
		enum ane_h16_window w = ANE_H16_DART0 + i;
		u32 status = b->ops->read32(b->ctx, w, f->dart_fault);
		if (log || (status & BIT(31))) {
			u64 address = b->ops->read32(b->ctx, w, f->dart_address_lo);

			address |= (u64)b->ops->read32(b->ctx, w, f->dart_address_hi) << 32;
			b->ops->log(b->ctx, "DART status", i, status);
			b->ops->log(b->ctx, "DART address", i, address);
		}
		if (status & BIT(31))
			ret = -EIO;
	}
	return ret;
}

static inline int ane_h16_fail(struct ane_h16_boot *b, int error)
{
	unsigned int i;

	if (!b->error)
		b->error = error;
	if (b->ran && !b->quarantined) {
		b->quarantined = true;
		for (i = 0; i < 8; i++)
			b->ops->log(b->ctx, "quarantine scratch", i, ane_h16_read(b, i));
		ane_h16_faults(b, true);
		b->ops->quarantine(b->ctx, b->error);
	}
	return b->error;
}

static inline int ane_h16_preflight(struct ane_h16_boot *b)
{
	const struct ane_h16_build *f = b->build;
	unsigned int pass, i;
	u64 rvbar;
	u32 control;
	int ret;

	/* All suppliers are raised by genpd before entry. Never write PMGR. */
	for (pass = 0; pass < 2; pass++) {
		for (i = 0; i < b->soc->power_count; i++) {
			u32 word = b->ops->read32(b->ctx, ANE_H16_PMGR,
						b->soc->power_words[i]);

			b->ops->log(b->ctx, "power ACTUAL", i, word);
			if ((word & 0xff) != 0xff)
				return -EHOSTDOWN;
		}
	}
	control = b->ops->read32(b->ctx, ANE_H16_ENGINE, f->cpu_control);
	rvbar = b->ops->read64(b->ctx, ANE_H16_ENGINE, f->rvbar);
	b->ops->log(b->ctx, "CPU control/RVBAR", control, rvbar);
	if (control || rvbar != f->expected_rvbar)
		return -EBUSY;
	ret = ane_h16_faults(b, true);
	if (ret)
		return ret;
	return 0;
}

static inline bool ane_h16_dapf_record(struct ane_h16_boot *b, unsigned int dart,
				       unsigned int record)
{
	const struct ane_h16_build *f = b->build;
	enum ane_h16_window w = ANE_H16_DAPF0 + dart;
	u32 off = f->dapf_records + record * f->dapf_stride;
	u32 flags = b->ops->read32(b->ctx, w, off);
	u64 start = b->ops->read32(b->ctx, w, off + 8);
	u64 end = b->ops->read32(b->ctx, w, off + 0x10);
	u32 enabled = b->ops->read32(b->ctx, w, off + 0x20);

	b->ops->log(b->ctx, "DAPF instance/record", dart, record);
	start |= (u64)b->ops->read32(b->ctx, w, off + 0xc) << 32;
	end |= (u64)b->ops->read32(b->ctx, w, off + 0x14) << 32;
	b->ops->log(b->ctx, "DAPF flags/enable", flags, enabled);
	b->ops->log(b->ctx, "DAPF start/end", start, end);
	return (flags & 0x333) == 0x31 && (enabled & 0xffff) == 1 &&
	       start == b->ranges[record][0] && end == b->ranges[record][1];
}

static inline int ane_h16_dapf(struct ane_h16_boot *b, bool program)
{
	const struct ane_h16_build *f = b->build;
	unsigned int dart, record;

	if (!f->dapf_layout_known)
		return -ENODATA;
	/* Check every lock before writing any record. */
	for (dart = 0; dart < 3; dart++) {
		enum ane_h16_window w = ANE_H16_DART0 + dart;
		u32 version = b->ops->read32(b->ctx, w, f->dapf_version);
		u32 protect = b->ops->read32(b->ctx, w, f->dapf_protect);
		u32 lock = b->ops->read32(b->ctx, w, f->dapf_lock);

		b->ops->log(b->ctx, "DAPF version", dart, version);
		b->ops->log(b->ctx, "DAPF protect/lock", protect, lock);
		if ((version & 0xffff) != f->dapf_version_value)
			return -ENODEV;
		if (dart >= f->dapf_instances)
			continue;
		for (record = 0; record < 4; record++)
			if (!ane_h16_dapf_record(b, dart, record) &&
			    program && (lock & BIT(2)))
				return -EPERM;
	}
	if (!program)
		return 0;
	for (dart = 0; dart < f->dapf_instances; dart++) {
		enum ane_h16_window w = ANE_H16_DAPF0 + dart;

		for (record = 0; record < 4; record++) {
			u32 off = f->dapf_records + record * f->dapf_stride;
			u32 value;

			if (ane_h16_dapf_record(b, dart, record))
				continue;
			value = b->ops->read32(b->ctx, w, off);
			b->ops->write32(b->ctx, w, off, (value & ~0x333) | 0x31);
			b->ops->write32(b->ctx, w, off + 8, lower_32_bits(b->ranges[record][0]));
			b->ops->write32(b->ctx, w, off + 0xc, upper_32_bits(b->ranges[record][0]));
			b->ops->write32(b->ctx, w, off + 0x10, lower_32_bits(b->ranges[record][1]));
			b->ops->write32(b->ctx, w, off + 0x14, upper_32_bits(b->ranges[record][1]));
			value = b->ops->read32(b->ctx, w, off + 0x20);
			b->ops->write32(b->ctx, w, off + 0x20, (value & ~0xffff) | 1);
			if (!ane_h16_dapf_record(b, dart, record))
				return -EIO;
		}
	}
	return 0;
}

/* r5 section 1.1. H16 has CE, with no H17 PWGATE pair. */
static inline int ane_h16_ce(struct ane_h16_boot *b)
{
	const struct ane_h16_build *f = b->build;
	u64 state = b->ops->read64(b->ctx, ANE_H16_ENGINE, f->ce + 0x10);
	u64 error = b->ops->read64(b->ctx, ANE_H16_ENGINE, f->ce + 0x18);
	u64 wanted = (state & 3) ^ 3;
	unsigned int i;

	b->ops->log(b->ctx, "CE state/error", state, error);
	if (error & wanted)
		return -EIO;
	if ((state & 3) == 3)
		return 0;
	b->ops->write64(b->ctx, ANE_H16_ENGINE, f->ce, wanted);
	for (i = 0; i <= f->ce_delays; i++) {
		if ((b->ops->read64(b->ctx, ANE_H16_ENGINE, f->ce + 8) & 3) == wanted)
			break;
		if (i == f->ce_delays)
			return -ETIMEDOUT;
		b->ops->delay_us(b->ctx, f->ce_delay_us);
	}
	error = b->ops->read64(b->ctx, ANE_H16_ENGINE, f->ce + 0x18);
	if (error & wanted)
		return -EIO;
	b->ops->write64(b->ctx, ANE_H16_ENGINE, f->ce + 8, wanted);
	b->ops->log(b->ctx, "CE ack bits/delays", wanted, i);
	return 0;
}

static inline int ane_h16_prepare(struct ane_h16_boot *b)
{
	const struct ane_h16_build *f = b->build;
	unsigned int i, j;
	int ret = ane_h16_ce(b);

	if (ret)
		return ret;
	for (i = 0; i < f->tunable_count; i++) {
		const struct ane_h16_tunables *group = &f->tunables[i];

		if (group->c1ppt && !f->c1ppt)
			continue;
		if (group->lock_mask &&
		    (b->ops->read32(b->ctx, group->window, group->base +
				    group->lock_offset) & group->lock_mask)) {
			for (j = 0; j < group->count; j++) {
				const struct ane_h16_tunable *v = &group->values[j];
				u32 value = b->ops->read32(b->ctx, group->window,
							group->base + v->offset);

				if ((value & v->mask) != (v->value & v->mask)) {
					b->ops->log(b->ctx, "locked tunable offset/value",
						    group->base + v->offset, value);
					return -EPERM;
				}
			}
			b->ops->log(b->ctx, "tunables locked match", group->window, group->count);
			continue;
		}
		for (j = 0; j < group->count; j++) {
			const struct ane_h16_tunable *v = &group->values[j];
			u32 off = group->base + v->offset;
			u32 old = b->ops->read32(b->ctx, group->window, off);

			b->ops->write32(b->ctx, group->window, off,
					(old & ~v->mask) | (v->value & v->mask));
		}
		b->ops->log(b->ctx, "tunables applied", group->window, group->count);
	}
	return 0;
}

static inline int ane_h16_wait(struct ane_h16_boot *b, u32 cell, u32 value,
			       bool table)
{
	const struct ane_h16_build *f = b->build;
	unsigned int i;
	int ret;
	u64 deadline = b->transport_ops->now_ns(b->ctx) +
		       (u64)f->scratch_polls * f->scratch_delay_us * 1000;

	for (i = 0; i < f->scratch_polls; i++) {
		if (b->transport_ops->now_ns(b->ctx) >= deadline)
			break;
		b->ops->mailbox(b->ctx);
		ret = ane_h16_faults(b, false);
		if (ret)
			return ret;
		if (ane_h16_read(b, cell) == value) {
			u64 address;

			if (!table)
				return 0;
			address = ane_h16_read(b, 0);
			address |= (u64)ane_h16_read(b, 1) << 32;
			if (address == f->ipc_dva)
				return 0;
			/* READY can pre-exist publication. Never accept the init DVA. */
			if (address != f->pool_dva + f->init_offset)
				return -EPROTO;
		}
		b->ops->delay_us(b->ctx, f->scratch_delay_us);
	}
	return -ETIMEDOUT;
}

static inline int ane_h16_alloc(struct ane_h16_boot *b, u64 dva, size_t size,
				struct ane_fw_surface *s)
{
	int ret = b->ops->alloc_at(b->ctx, dva, size, s);

	if (ret)
		return ret;
	if (!ane_fw_surface_valid(s) || s->dva != dva || s->size != size)
		return -EPROTO;
	return 0;
}

static inline int ane_h16_scratch_boot(struct ane_h16_boot *b)
{
	const struct ane_h16_build *f = b->build;
	u64 ipc_bytes, heap_bytes;
	u8 *p;
	unsigned int i;
	int ret;

	ret = ane_h16_alloc(b, f->pool_dva, f->pool_size, &b->pool);
	if (ret)
		return ret;
	ret = ane_h16_prepare(b);
	if (ret)
		return ret;
	for (i = 0; i < 8; i++)
		ane_h16_write(b, i, 0);
	/* Endpoint-1 management client is installed, never awaited. */
	b->ops->mailbox(b->ctx);
	ane_h16_write(b, 6, 1);
	ane_h16_write(b, 7, 0);
	if (b->ops->read64(b->ctx, ANE_H16_ENGINE, f->rvbar) != f->expected_rvbar)
		return -EBUSY;
	ret = b->ops->pin(b->ctx);
	if (ret)
		return ret;
	b->ops->write32(b->ctx, ANE_H16_ENGINE, f->cpu_control, 0);
	/* Latch before the first RUN store, including a failure at that store. */
	b->ran = true;
	b->ops->write32(b->ctx, ANE_H16_ENGINE, f->cpu_control, f->run);
	ret = ane_h16_wait(b, 7, f->ready, false);
	if (ret)
		return ret;
	b->ops->log(b->ctx, "READY", f->ready, 0);
	for (i = 0; i < ARRAY_SIZE(b->requests); i++)
		b->requests[i] = ane_h16_read(b, i);
	b->ops->log(b->ctx, "requests channels/IPC-minus-one", b->requests[0], b->requests[1]);
	b->ops->log(b->ctx, "requests heap/mode", b->requests[3], b->requests[4]);
	ipc_bytes = (u64)b->requests[1] + 1;
	heap_bytes = b->requests[3];
	/* Host policy. IPC cannot reach the fixed heap; no guessed firmware ceiling. */
	if (!b->requests[0] || b->requests[0] >= f->max_channels ||
	    ipc_bytes < ANE_FW_TABLE_SIZE || ipc_bytes > f->heap_dva - f->ipc_dva ||
	    heap_bytes > f->max_heap ||
	    ALIGN(heap_bytes, SZ_16K) > f->shared_dva - f->heap_dva ||
	    b->requests[4] > 1)
		return -E2BIG;
	ret = ane_h16_alloc(b, f->ipc_dva, ALIGN(ipc_bytes, SZ_16K), &b->ipc);
	if (ret)
		return ret;
	if (heap_bytes) {
		ret = ane_h16_alloc(b, f->heap_dva, ALIGN(heap_bytes, SZ_16K), &b->heap);
		if (ret)
			return ret;
	}
	p = (u8 *)b->pool.cpu + f->init_offset;
	memset(p, 0, f->init_size);
	put_unaligned_le64(f->ipc_dva, p + 8);
	put_unaligned_le64(f->config_span, p + 0x10);
	put_unaligned_le64(f->config_window - f->config_span, p + 0x18);
	put_unaligned_le64(heap_bytes ? f->heap_dva : 0, p + 0x20);
	put_unaligned_le64(heap_bytes, p + 0x28);
	put_unaligned_le64(ipc_bytes, p + 0x50);
	put_unaligned_le64(f->pool_dva, p + 0x58);
	put_unaligned_le64(f->pool_size, p + 0x60);
	put_unaligned_le32(f->template_count, p + 0x68);
	put_unaligned_le32(f->template_value, p + f->template_word);
	b->transport_ops->write_barrier(b->ctx);
	ane_h16_write(b, 0, lower_32_bits(f->pool_dva + f->init_offset));
	ane_h16_write(b, 1, upper_32_bits(f->pool_dva + f->init_offset));
	ane_h16_write(b, 7, ~f->ready);
	ret = ane_h16_wait(b, 7, f->ready, true);
	if (ret)
		return ret;
	ret = ane_fw_transport_init(&b->transport, b->soc, b->transport_ops, b->ctx,
				    b->ipc, b->pool, f->init_offset + f->init_size,
				    f->ipc_dva, ANE_FW_TABLE_SIZE);
	if (ret)
		return ret;
	b->transport.native_free = f->native_free;
	b->ops->log(b->ctx, "table validated", f->ipc_dva, ANE_FW_CHANNELS);
	ret = ane_fw_transport_arm(&b->transport);
	if (ret)
		return ret;
	ane_h16_write(b, 3, f->ready);
	ret = ane_h16_wait(b, 3, 0, false);
	if (!ret)
		b->ops->log(b->ctx, "scratch 3 cleared", 0, 0);
	return ret;
}

static inline int ane_h16_startup(struct ane_h16_boot *b)
{
	const struct ane_h16_build *f = b->build;
	unsigned int i;
	int ret;

	/* Isolate early-IO ordering without withholding allocation/log service. */
	if (b->startup_drain_ms) {
		u64 until = b->transport_ops->now_ns(b->ctx) +
			(u64)min(b->startup_drain_ms, 1000U) * NSEC_PER_MSEC;

		do {
			u32 pending;

			ret = ane_fw_transport_irq(&b->transport, &pending);
			if (ret)
				return ret;
			ret = ane_fw_transport_shared(&b->transport);
			if (ret && ret != -EAGAIN)
				return ret;
			ret = ane_fw_transport_terminal(&b->transport);
			if (ret)
				return ret;
			b->ops->mailbox(b->ctx);
			ret = ane_h16_faults(b, false);
			if (ret)
				return ret;
			b->ops->delay_us(b->ctx, 10);
		} while (b->transport_ops->now_ns(b->ctx) < until);
		b->ops->log(b->ctx, "startup pre-IO shared/messages",
			    b->transport.shared_count, b->transport.terminal_messages);
	}

	for (i = 0; i < ARRAY_SIZE(f->startup); i++) {
		u8 command[100] = {};
		u8 *reply;
		u64 deadline = b->transport_ops->now_ns(b->ctx) + f->command_timeout_ns;

		ane_fw_command_header(command, f->startup[i].id, true);
		ret = ane_fw_transport_publish(&b->transport, ANE_FW_IO, command,
					       f->startup[i].size, deadline);
		if (ret)
			return ret;
		ret = ane_fw_transport_wait(&b->transport, ANE_FW_IO);
		reply = (u8 *)b->pool.cpu +
			(b->transport.pending[ANE_FW_IO].dva - b->pool.dva);
		if (ret) {
			if (ret == -EREMOTEIO)
				b->ops->log(b->ctx, "startup refusal ID/ack",
					    f->startup[i].id, get_unaligned_le16(reply + 6));
			b->ops->log(b->ctx, "startup wait failed ID/re-rings", f->startup[i].id,
				    b->transport.pending[ANE_FW_IO].rerings);
			b->ops->log(b->ctx, "startup IO slot header/error",
				    b->transport_ops->load64(b->ctx,
					ane_fw_transport_slot(&b->transport, ANE_FW_IO)), ret);
			b->ops->log(b->ctx, "startup IRQ status/shared count",
				    b->transport_ops->read32(b->ctx, b->soc->irq_status),
				    b->transport.shared_count);
			/* Read coherent ring state before quarantine; no ownership changes. */
			for (unsigned int ch = 0; ch < ANE_FW_CHANNELS; ch++) {
				struct ane_fw_transport *t = &b->transport;
				const struct ane_fw_channel *c = &t->channels[ch];
				u32 head = t->head[ch], prev = (head + c->slots - 1) % c->slots;
				u8 *slot = (u8 *)t->ipc.cpu + c->offset + head * ANE_FW_SLOT_SIZE;
				u8 *last = (u8 *)t->ipc.cpu + c->offset + prev * ANE_FW_SLOT_SIZE;

				b->transport_ops->read_barrier(b->ctx);
				b->ops->log(b->ctx, "startup channel/head", ch, head);
				b->ops->log(b->ctx, "startup current slot header/word1",
					    t->ops->load64(b->ctx, slot), t->ops->load64(b->ctx, slot + 8));
				b->ops->log(b->ctx, "startup previous slot header/word1",
					    t->ops->load64(b->ctx, last), t->ops->load64(b->ctx, last + 8));
			}
			return ret;
		}
		b->ops->log(b->ctx, "startup ack", f->startup[i].id, get_unaligned_le16(reply + 6));
		b->ops->log(b->ctx, "startup doorbell re-rings", f->startup[i].id,
			    b->transport.pending[ANE_FW_IO].rerings);
		if (f->startup[i].id == 3) {
			b->tick = get_unaligned_le32(reply + 8);
			b->ops->log(b->ctx, "CONFIG_GET tick", b->tick, 0);
			if (!b->tick)
				return -EPROTO;
		}
		if (f->startup[i].id == 0x22)
			memcpy(b->resource_info, reply, sizeof(b->resource_info));
		b->ops->log(b->ctx, "startup terminal messages/bytes",
			    b->transport.terminal_messages, b->transport.terminal_bytes);
		ret = ane_h16_faults(b, false);
		if (ret)
			return ret;
	}
	return 0;
}

static inline int ane_h16_run(struct ane_h16_boot *b, enum ane_h16_stage stage)
{
	int ret;

	if (stage == ANE_H16_REFUSE)
		return -EOPNOTSUPP;
	if (stage < ANE_H16_PREFLIGHT || stage > ANE_H16_STARTUP)
		return -EINVAL;
	if (b->attempted || b->quarantined)
		return -EBUSY;
	b->attempted = true;
	ret = ane_h16_preflight(b);
	if (!ret)
		ret = ane_h16_dapf(b, false);
	if (!ret && stage >= ANE_H16_MAP) {
		ret = b->ops->map(b->ctx);
		if (!ret)
			ret = ane_h16_dapf(b, true);
		if (!ret)
			ret = ane_h16_faults(b, true);
	}
	if (!ret && stage >= ANE_H16_BOOT)
		ret = ane_h16_scratch_boot(b);
	if (!ret && stage == ANE_H16_STARTUP)
		ret = ane_h16_startup(b);
	return ret ? ane_h16_fail(b, ret) : 0;
}

#endif /* __ANE_H16_BOOT_H__ */
