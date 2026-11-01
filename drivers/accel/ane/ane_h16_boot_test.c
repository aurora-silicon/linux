// SPDX-License-Identifier: GPL-2.0-only OR MIT
#include <kunit/test.h>
#include <linux/module.h>

#include "ane_h16_boot_data.h"

struct ane_h16_sim {
	struct kunit *test;
	struct ane_h16_boot boot;
	struct ane_h16_build build;
	struct ane_fw_platform soc;
	u32 scratch[8];
	u32 dapf[3][SZ_16K / 4];
	u64 now;
	u64 ce_ack;
	u32 cpu;
	u32 command_count;
	u32 scratch_writes;
	u32 tuning_writes;
	u32 dapf_writes;
	u32 mailbox_polls;
	u32 maps;
	u32 pins;
	u32 quarantines;
	u32 allocations;
	u32 lock;
	u32 fault;
	u32 channel_request;
	u32 ipc_request;
	u32 heap_request;
	u32 tick;
	struct ane_fw_rtk rtk;
	u32 mailbox_tx;
	bool offered_mailbox;
	bool fault_after_run;
	u32 lifecycle_mode;
	bool lifecycle_requested;
	u16 refuse_id;
	bool no_ready;
	bool wrong_table;
	bool stale_table;
	bool no_clear;
	bool shared_pending;
	bool shared_replied;
	bool ce_timeout;
	bool bad_power;
	bool bad_rvbar;
	bool fail_map;
	bool must_allocate_first;
	bool fault_on_drain;
};

static const u32 sim_power_words[] = { 0x3c0, 0xc008, 0xc000, 0xc010, 0xc018 };

static u32 sim_read32(void *ctx, enum ane_h16_window w, u32 off)
{
	struct ane_h16_sim *s = ctx;

	if (w == ANE_H16_PMGR)
		return s->bad_power ? 0xf : 0xff;
	if (w >= ANE_H16_DART0 && w <= ANE_H16_DART2) {
		if (off == s->build.dapf_version)
			return 0x203;
		if (off == s->build.dapf_protect || off == s->build.dapf_lock)
			return s->lock;
		if (off == s->build.dart_fault)
			return s->fault;
		return 0;
	}
	if (w >= ANE_H16_DAPF0)
		return s->dapf[w - ANE_H16_DAPF0][off / 4];
	if (w == ANE_H16_ENGINE) {
		if (off == s->build.cpu_control)
			return s->cpu;
		if (off == 0x197fffc)
			return 0;
		if (off >= s->soc.scratch && off < s->soc.scratch + 32)
			return s->scratch[(off - s->soc.scratch) / 4];
		if (off == s->soc.irq_status)
			return 0;
	}
	return 0xa5a5a5a5;
}

static u64 sim_read64(void *ctx, enum ane_h16_window w, u32 off)
{
	struct ane_h16_sim *s = ctx;

	if (off == s->build.rvbar)
		return s->bad_rvbar ? 0 : s->build.expected_rvbar;
	if (off == s->build.ce + 8)
		return s->ce_ack;
	return 0;
}

static void sim_reply(struct ane_h16_sim *s)
{
	struct ane_fw_transport *t = &s->boot.transport;
	struct ane_fw_transaction *tx = &t->pending[ANE_FW_IO];
	u8 *slot = ane_fw_transport_slot(t, ANE_FW_IO);
	u8 *command = (u8 *)t->pool.cpu + (tx->dva - t->pool.dva);

	if (tx->id == 3)
		put_unaligned_le32(s->tick, command + 8);
	if (tx->id == 0x22)
		memset(command + 8, 0x5a, 92);
	if (tx->id == s->refuse_id)
		put_unaligned_le16(7, command + 6);
	put_unaligned_le64(0, slot + 16);
	put_unaligned_le64(tx->dva | 1, slot);
}

static void sim_write32(void *ctx, enum ane_h16_window w, u32 off, u32 value)
{
	struct ane_h16_sim *s = ctx;
	const struct ane_h16_build *f = &s->build;

	if (w >= ANE_H16_DAPF0) {
		s->dapf[w - ANE_H16_DAPF0][off / 4] = value;
		s->dapf_writes++;
		return;
	}
	if (w == ANE_H16_ENGINE && off == f->cpu_control) {
		s->cpu = value;
		if (value == f->run && s->fault_after_run)
			s->fault = BIT(31);
		if (value == f->run && !s->no_ready) {
			s->scratch[0] = s->channel_request;
			s->scratch[1] = s->ipc_request;
			s->scratch[3] = s->heap_request;
			s->scratch[4] = 0;
			s->scratch[7] = f->ready;
		}
		return;
	}
	if (w == ANE_H16_ENGINE && off >= s->soc.scratch && off < s->soc.scratch + 32) {
		u32 cell = (off - s->soc.scratch) / 4;

		if (s->scratch_writes < 8) {
			KUNIT_EXPECT_EQ(s->test, cell, s->scratch_writes);
			KUNIT_EXPECT_EQ(s->test, value, 0U);
			KUNIT_EXPECT_EQ(s->test, s->tuning_writes, 103U);
		}
		s->scratch_writes++;
		s->scratch[cell] = value;
		if (cell == 7 && value == ~f->ready) {
			u64 address = f->ipc_dva + (s->wrong_table ? SZ_16K : 0);

			if (!s->stale_table) {
				s->scratch[0] = lower_32_bits(address);
				s->scratch[1] = upper_32_bits(address);
			}
			s->scratch[7] = f->ready;
		}
		if (cell == 3 && value == f->ready && !s->no_clear)
			s->scratch[3] = 0;
		return;
	}
	if (w == ANE_H16_ENGINE && off == s->soc.doorbell) {
		struct ane_fw_transport *t = &s->boot.transport;

		if (value == BIT(1)) {
			struct ane_fw_transaction *tx = &t->pending[ANE_FW_IO];

			KUNIT_EXPECT_LT(s->test, s->command_count, 4U);
			if (s->command_count < 4)
				KUNIT_EXPECT_EQ(s->test, tx->id, f->startup[s->command_count].id);
			if (s->must_allocate_first)
				KUNIT_EXPECT_TRUE(s->test, s->shared_replied);
			s->command_count++;
			if (tx->id == 3 && s->lifecycle_mode) {
				u8 *slot = ane_fw_transport_slot(t, ANE_FW_SHAREDMALLOC);
				u64 dva = s->lifecycle_mode == 2 ? 0 : s->build.shared_dva;

				if (s->lifecycle_mode == 3)
					dva += SZ_16K;

				put_unaligned_le64(dva, slot);
				put_unaligned_le64(0, slot + 8);
				put_unaligned_le64(s->lifecycle_mode == 2 ? 1 : 0, slot + 16);
				s->lifecycle_requested = true;
			} else if (!tx->id) {
				u8 *slot = ane_fw_transport_slot(t, ANE_FW_SHAREDMALLOC);

				put_unaligned_le64(0, slot);
				put_unaligned_le64(SZ_16K, slot + 8);
				put_unaligned_le64(0, slot + 16);
				s->shared_pending = true;
			} else {
				sim_reply(s);
			}
		}
		if (value == BIT(5)) {
			s->shared_replied = true;
			if (s->lifecycle_requested) {
				u8 *slot = ane_fw_transport_slot(t, ANE_FW_SHAREDMALLOC);

				KUNIT_EXPECT_EQ(s->test, s->lifecycle_mode, 1U);
				KUNIT_EXPECT_EQ(s->test, get_unaligned_le64(slot), 1ULL);
				KUNIT_EXPECT_EQ(s->test, get_unaligned_le64(slot + 8), 0ULL);
				KUNIT_EXPECT_EQ(s->test, get_unaligned_le64(slot + 16), 0ULL);
			}
			if (t->pending[ANE_FW_IO].active)
				sim_reply(s);
		}
		return;
	}
	/* The selected AXI and XL tables must finish before scratch writes. */
	KUNIT_EXPECT_FALSE(s->test, s->boot.ran);
	s->tuning_writes++;
}

static void sim_write64(void *ctx, enum ane_h16_window w, u32 off, u64 value)
{
	struct ane_h16_sim *s = ctx;

	KUNIT_EXPECT_EQ(s->test, w, ANE_H16_ENGINE);
	if (off == s->build.ce && !s->ce_timeout)
		s->ce_ack = value;
}

static void sim_delay(void *ctx, unsigned int us)
{
	struct ane_h16_sim *s = ctx;

	s->now += (u64)us * 1000;
	if (s->fault_on_drain && s->boot.transport.armed)
		s->fault = BIT(31);
}

static int sim_map(void *ctx)
{
	struct ane_h16_sim *s = ctx;

	s->maps++;
	return s->fail_map ? -ENOMEM : 0;
}

static int sim_alloc_at(void *ctx, u64 dva, size_t size, struct ane_fw_surface *out)
{
	struct ane_h16_sim *s = ctx;

	/* Sparse simulated heap. The engine only accesses pool and IPC bytes. */
	out->cpu = kunit_kzalloc(s->test, min_t(size_t, size, SZ_256K), GFP_KERNEL);
	if (!out->cpu)
		return -ENOMEM;
	out->dva = dva;
	out->size = size;
	s->allocations++;
	if (dva == s->build.ipc_dva) {
		static const char * const names[] = {
			"TERMINAL", "IO", "DEBUG", "BUF_H2T", "BUF_T2H",
			"SHAREDMALLOC", "IO_T2H", "DATA_CHAIN_H2T",
		};
		unsigned int i;

		for (i = 0; i < 8; i++) {
			u8 *p = (u8 *)out->cpu + i * ANE_FW_DESCRIPTOR_SIZE;

			strscpy(p, names[i], 64);
			put_unaligned_le32(i == 0 ? 2 : i == 4 || i == 5 || i == 6, p + 0x40);
			put_unaligned_le32(i, p + 0x44);
			put_unaligned_le64(1, p + 0x48);
			put_unaligned_le64(dva + ANE_FW_TABLE_SIZE + i * 64, p + 0x50);
		}
	}
	return 0;
}

static int sim_pin(void *ctx)
{
	((struct ane_h16_sim *)ctx)->pins++;
	return 0;
}

static int sim_mailbox_send(void *ctx, u64 word0, u64 word1)
{
	struct ane_h16_sim *s = ctx;
	static const u8 types[] = { 2, 8, 8, 5, 6, 0xb, 5 };

	KUNIT_EXPECT_LT(s->test, s->mailbox_tx, ARRAY_SIZE(types));
	if (s->mailbox_tx < ARRAY_SIZE(types))
		KUNIT_EXPECT_EQ(s->test, (word0 >> 52) & 0xff, (u64)types[s->mailbox_tx]);
	KUNIT_EXPECT_EQ(s->test, word1, 0ULL);
	s->mailbox_tx++;
	return 0;
}

static void sim_mailbox(void *ctx)
{
	struct ane_h16_sim *s = ctx;
	static const u64 offered[] = {
		ANE_FW_RTK_TYPE(1) | (12 << 16) | 12,
		ANE_FW_RTK_TYPE(8) | BIT(0) | BIT(1),
		ANE_FW_RTK_TYPE(8) | BIT_ULL(32) | ANE_FW_RTK_LAST | BIT(0),
		ANE_FW_RTK_TYPE(7) | 0x20,
		ANE_FW_RTK_TYPE(0xb) | 0x20,
	};
	unsigned int i;

	s->mailbox_polls++;
	if (!s->offered_mailbox || !s->boot.ran || s->mailbox_tx)
		return;
	for (i = 0; i < ARRAY_SIZE(offered); i++) {
		int ret = ane_fw_rtk_receive(&s->rtk, offered[i], 0, s, sim_mailbox_send);

		KUNIT_EXPECT_EQ(s->test, ret, 0);
	}
}

static void sim_log(void *ctx, const char *item, u64 a, u64 b)
{
}

static void sim_quarantine(void *ctx, int error)
{
	((struct ane_h16_sim *)ctx)->quarantines++;
}

static u64 sim_load64(void *ctx, const void *p)
{
	return get_unaligned_le64(p);
}

static void sim_store64(void *ctx, void *p, u64 value)
{
	put_unaligned_le64(value, p);
}

static void sim_barrier(void *ctx)
{
}

static u32 sim_engine_read(void *ctx, u32 off)
{
	return sim_read32(ctx, ANE_H16_ENGINE, off);
}

static void sim_engine_write(void *ctx, u32 off, u32 value)
{
	sim_write32(ctx, ANE_H16_ENGINE, off, value);
}

static u64 sim_now(void *ctx)
{
	return ((struct ane_h16_sim *)ctx)->now;
}

static void sim_relax(void *ctx)
{
	sim_delay(ctx, 1000);
}

static int sim_alloc_shared(void *ctx, size_t size, u32 tag, struct ane_fw_surface *out)
{
	struct ane_h16_sim *s = ctx;

	return sim_alloc_at(ctx, s->build.shared_dva, size, out);
}

static void sim_refused(void *ctx, u64 w0, u64 w1, u64 w2, int error)
{
}

static const struct ane_h16_boot_ops sim_ops = {
	.read32 = sim_read32,
	.read64 = sim_read64,
	.write32 = sim_write32,
	.write64 = sim_write64,
	.delay_us = sim_delay,
	.map = sim_map,
	.alloc_at = sim_alloc_at,
	.pin = sim_pin,
	.mailbox = sim_mailbox,
	.log = sim_log,
	.quarantine = sim_quarantine,
};

static const struct ane_fw_transport_ops sim_transport_ops = {
	.load64 = sim_load64,
	.store64 = sim_store64,
	.read_barrier = sim_barrier,
	.write_barrier = sim_barrier,
	.read32 = sim_engine_read,
	.write32 = sim_engine_write,
	.now_ns = sim_now,
	.relax = sim_relax,
	.alloc = sim_alloc_shared,
	.refused = sim_refused,
};

static int sim_init(struct kunit *test)
{
	struct ane_h16_sim *s = kunit_kzalloc(test, sizeof(*s), GFP_KERNEL);
	unsigned int i;

	if (!s)
		return -ENOMEM;
	test->priv = s;
	s->test = test;
	s->build = ane_h16_build_26a428;
	/* Separate DART header and DAPF record windows. */
	s->soc.scratch = 0x1880020;
	s->soc.doorbell = 0x1884000;
	s->soc.irq_status = 0x188c000;
	s->soc.irq_ack = 0x1890000;
	s->soc.power_words = sim_power_words;
	s->soc.power_count = ARRAY_SIZE(sim_power_words);
	s->boot.soc = &s->soc;
	s->boot.build = &s->build;
	s->boot.ops = &sim_ops;
	s->boot.transport_ops = &sim_transport_ops;
	s->boot.ctx = s;
	for (i = 0; i < 4; i++) {
		s->boot.ranges[i][0] = 0x500000000ULL + i * SZ_16K;
		s->boot.ranges[i][1] = s->boot.ranges[i][0] + 3;
	}
	s->channel_request = 8;
	s->ipc_request = 0xc440;
	s->heap_request = 0xf00000;
	s->tick = 25000000;
	s->refuse_id = 0xffff;
	return 0;
}

static void ane_h16_happy_test(struct kunit *test)
{
	struct ane_h16_sim *s = test->priv;
	u8 *p;

	KUNIT_ASSERT_EQ(test, ane_h16_run(&s->boot, ANE_H16_STARTUP), 0);
	KUNIT_EXPECT_TRUE(test, s->boot.ran);
	KUNIT_EXPECT_FALSE(test, s->boot.quarantined);
	KUNIT_EXPECT_EQ(test, s->pins, 1U);
	KUNIT_EXPECT_EQ(test, s->command_count, 4U);
	KUNIT_EXPECT_TRUE(test, s->shared_replied);
	KUNIT_EXPECT_GT(test, s->mailbox_polls, 0U);
	KUNIT_EXPECT_EQ(test, s->boot.tick, 25000000U);
	KUNIT_EXPECT_EQ(test, s->boot.resource_info[99], (u8)0x5a);
	p = (u8 *)s->boot.pool.cpu + s->build.init_offset;
	KUNIT_EXPECT_EQ(test, get_unaligned_le64(p), 0ULL);
	KUNIT_EXPECT_EQ(test, get_unaligned_le64(p + 8), s->build.ipc_dva);
	KUNIT_EXPECT_EQ(test, get_unaligned_le64(p + 0x10), 0x380000ULL);
	KUNIT_EXPECT_EQ(test, get_unaligned_le64(p + 0x18), 0xfc80000ULL);
	KUNIT_EXPECT_EQ(test, get_unaligned_le64(p + 0x28), 0xf00000ULL);
	KUNIT_EXPECT_EQ(test, get_unaligned_le64(p + 0x50), 0xc441ULL);
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(p + 0x12c), 4U);
	KUNIT_EXPECT_EQ(test, get_unaligned_le64(p + 0x38), 0ULL);
	KUNIT_EXPECT_EQ(test, ane_h16_run(&s->boot, ANE_H16_STARTUP), -EBUSY);
}

static void ane_h16_no_ready_test(struct kunit *test)
{
	struct ane_h16_sim *s = test->priv;

	s->no_ready = true;
	KUNIT_EXPECT_EQ(test, ane_h16_run(&s->boot, ANE_H16_BOOT), -ETIMEDOUT);
	KUNIT_EXPECT_TRUE(test, s->boot.quarantined);
	KUNIT_EXPECT_EQ(test, s->quarantines, 1U);
	KUNIT_EXPECT_EQ(test, s->allocations, 1U);
	KUNIT_EXPECT_EQ(test, s->now, 1000000000ULL);
	KUNIT_EXPECT_EQ(test, ane_h16_run(&s->boot, ANE_H16_BOOT), -EBUSY);
}

static void ane_h16_wrong_table_test(struct kunit *test)
{
	struct ane_h16_sim *s = test->priv;

	s->wrong_table = true;
	KUNIT_EXPECT_EQ(test, ane_h16_run(&s->boot, ANE_H16_BOOT), -EPROTO);
	KUNIT_EXPECT_TRUE(test, s->boot.quarantined);
	KUNIT_EXPECT_FALSE(test, s->boot.transport.armed);
}

static void ane_h16_stale_table_test(struct kunit *test)
{
	struct ane_h16_sim *s = test->priv;

	s->stale_table = true;
	KUNIT_EXPECT_EQ(test, ane_h16_run(&s->boot, ANE_H16_BOOT), -ETIMEDOUT);
	KUNIT_EXPECT_TRUE(test, s->boot.quarantined);
	KUNIT_EXPECT_FALSE(test, s->boot.transport.armed);
}

static void ane_h16_oversized_ipc_test(struct kunit *test)
{
	struct ane_h16_sim *s = test->priv;

	s->ipc_request = U32_MAX;
	KUNIT_EXPECT_EQ(test, ane_h16_run(&s->boot, ANE_H16_BOOT), -E2BIG);
	KUNIT_EXPECT_TRUE(test, s->boot.quarantined);
	KUNIT_EXPECT_EQ(test, s->allocations, 1U);
}

static void ane_h16_oversized_heap_test(struct kunit *test)
{
	struct ane_h16_sim *s = test->priv;

	s->heap_request = U32_MAX;
	KUNIT_EXPECT_EQ(test, ane_h16_run(&s->boot, ANE_H16_BOOT), -E2BIG);
	KUNIT_EXPECT_TRUE(test, s->boot.quarantined);
}

static void ane_h16_channel_limit_test(struct kunit *test)
{
	struct ane_h16_sim *s = test->priv;

	s->channel_request = 0x21;
	KUNIT_EXPECT_EQ(test, ane_h16_run(&s->boot, ANE_H16_BOOT), -E2BIG);
	KUNIT_EXPECT_TRUE(test, s->boot.quarantined);
}

static void ane_h16_locked_dapf_test(struct kunit *test)
{
	struct ane_h16_sim *s = test->priv;

	s->lock = BIT(2);
	KUNIT_EXPECT_EQ(test, ane_h16_run(&s->boot, ANE_H16_MAP), -EPERM);
	KUNIT_EXPECT_FALSE(test, s->boot.ran);
	KUNIT_EXPECT_EQ(test, s->dapf_writes, 0U);
	KUNIT_EXPECT_EQ(test, s->pins, 0U);
}

static void ane_h16_nonzero_ack_test(struct kunit *test)
{
	struct ane_h16_sim *s = test->priv;

	s->refuse_id = 0;
	KUNIT_EXPECT_EQ(test, ane_h16_run(&s->boot, ANE_H16_STARTUP), -EREMOTEIO);
	KUNIT_EXPECT_TRUE(test, s->boot.quarantined);
	KUNIT_EXPECT_EQ(test, s->command_count, 2U);
	KUNIT_EXPECT_EQ(test, s->pins, 1U);
}

static void ane_h16_preflight_test(struct kunit *test)
{
	struct ane_h16_sim *s = test->priv;

	KUNIT_EXPECT_EQ(test, ane_h16_run(&s->boot, ANE_H16_PREFLIGHT), 0);
	KUNIT_EXPECT_EQ(test, s->maps, 0U);
	KUNIT_EXPECT_EQ(test, s->dapf_writes, 0U);
	KUNIT_EXPECT_EQ(test, s->tuning_writes, 0U);
	KUNIT_EXPECT_EQ(test, s->scratch_writes, 0U);
}

static void ane_h16_default_test(struct kunit *test)
{
	struct ane_h16_sim *s = test->priv;

	KUNIT_EXPECT_EQ(test, ane_h16_stage_parse(NULL), ANE_H16_REFUSE);
	KUNIT_EXPECT_EQ(test, ane_h16_stage_parse("unknown"), -EINVAL);
	KUNIT_EXPECT_EQ(test, ane_h16_run(&s->boot, ANE_H16_REFUSE), -EOPNOTSUPP);
	KUNIT_EXPECT_FALSE(test, s->boot.attempted);
	KUNIT_EXPECT_EQ(test, s->scratch_writes, 0U);
}

static void ane_h16_ce_timeout_test(struct kunit *test)
{
	struct ane_h16_sim *s = test->priv;

	s->ce_timeout = true;
	KUNIT_EXPECT_EQ(test, ane_h16_run(&s->boot, ANE_H16_BOOT), -ETIMEDOUT);
	KUNIT_EXPECT_FALSE(test, s->boot.ran);
	KUNIT_EXPECT_EQ(test, s->scratch_writes, 0U);
	KUNIT_EXPECT_EQ(test, s->now, 50010000ULL);
}

static void ane_h16_zero_tick_test(struct kunit *test)
{
	struct ane_h16_sim *s = test->priv;

	s->tick = 0;
	KUNIT_EXPECT_EQ(test, ane_h16_run(&s->boot, ANE_H16_STARTUP), -EPROTO);
	KUNIT_EXPECT_TRUE(test, s->boot.quarantined);
}

static void ane_h16_scratch3_timeout_test(struct kunit *test)
{
	struct ane_h16_sim *s = test->priv;

	s->no_clear = true;
	KUNIT_EXPECT_EQ(test, ane_h16_run(&s->boot, ANE_H16_BOOT), -ETIMEDOUT);
	KUNIT_EXPECT_TRUE(test, s->boot.quarantined);
	KUNIT_EXPECT_TRUE(test, s->boot.transport.armed);
	KUNIT_EXPECT_EQ(test, s->command_count, 0U);
}

static void ane_h16_mailbox_test(struct kunit *test)
{
	struct ane_h16_sim *s = test->priv;

	s->offered_mailbox = true;
	KUNIT_ASSERT_EQ(test, ane_h16_run(&s->boot, ANE_H16_STARTUP), 0);
	KUNIT_EXPECT_EQ(test, s->rtk.phase, ANE_FW_RTK_READY);
	KUNIT_EXPECT_EQ(test, s->mailbox_tx, 7U);
}

static void ane_h16_no_heap_test(struct kunit *test)
{
	struct ane_h16_sim *s = test->priv;
	u8 *record;

	s->heap_request = 0;
	KUNIT_ASSERT_EQ(test, ane_h16_run(&s->boot, ANE_H16_BOOT), 0);
	KUNIT_EXPECT_EQ(test, s->allocations, 2U);
	record = (u8 *)s->boot.pool.cpu + s->build.init_offset;
	KUNIT_EXPECT_EQ(test, get_unaligned_le64(record + 0x20), 0ULL);
	KUNIT_EXPECT_EQ(test, get_unaligned_le64(record + 0x28), 0ULL);
}

static void ane_h16_power_actual_test(struct kunit *test)
{
	struct ane_h16_sim *s = test->priv;

	s->bad_power = true;
	KUNIT_EXPECT_EQ(test, ane_h16_run(&s->boot, ANE_H16_PREFLIGHT), -EHOSTDOWN);
	KUNIT_EXPECT_EQ(test, s->scratch_writes, 0U);
	KUNIT_EXPECT_EQ(test, s->pins, 0U);
}

static void ane_h16_rvbar_test(struct kunit *test)
{
	struct ane_h16_sim *s = test->priv;

	s->bad_rvbar = true;
	KUNIT_EXPECT_EQ(test, ane_h16_run(&s->boot, ANE_H16_BOOT), -EBUSY);
	KUNIT_EXPECT_EQ(test, s->scratch_writes, 0U);
	KUNIT_EXPECT_EQ(test, s->maps, 0U);
}

static void ane_h16_dart_fault_test(struct kunit *test)
{
	struct ane_h16_sim *s = test->priv;

	s->fault_after_run = true;
	KUNIT_EXPECT_EQ(test, ane_h16_run(&s->boot, ANE_H16_BOOT), -EIO);
	KUNIT_EXPECT_TRUE(test, s->boot.quarantined);
	KUNIT_EXPECT_EQ(test, s->pins, 1U);
	KUNIT_EXPECT_EQ(test, s->quarantines, 1U);
}

static void ane_h16_map_failure_test(struct kunit *test)
{
	struct ane_h16_sim *s = test->priv;

	s->fail_map = true;
	KUNIT_EXPECT_EQ(test, ane_h16_run(&s->boot, ANE_H16_BOOT), -ENOMEM);
	KUNIT_EXPECT_FALSE(test, s->boot.ran);
	KUNIT_EXPECT_FALSE(test, s->boot.quarantined);
	KUNIT_EXPECT_EQ(test, s->pins, 0U);
}

static void ane_h16_locked_matching_test(struct kunit *test)
{
	struct ane_h16_sim *s = test->priv;
	unsigned int dart, i;

	s->lock = BIT(2);
	for (dart = 0; dart < 3; dart++) {
		for (i = 0; i < 4; i++) {
			u32 *p = &s->dapf[dart][(s->build.dapf_records + i * 0x40) / 4];

			p[0] = 0x80000031;
			p[2] = lower_32_bits(s->boot.ranges[i][0]);
			p[3] = upper_32_bits(s->boot.ranges[i][0]);
			p[4] = lower_32_bits(s->boot.ranges[i][1]);
			p[5] = upper_32_bits(s->boot.ranges[i][1]);
			p[8] = 0xabcd0001;
		}
	}
	KUNIT_EXPECT_EQ(test, ane_h16_run(&s->boot, ANE_H16_MAP), 0);
	KUNIT_EXPECT_EQ(test, s->dapf_writes, 0U);
}

static void ane_h16_heap_overlap_test(struct kunit *test)
{
	struct ane_h16_sim *s = test->priv;

	s->heap_request = SZ_16M;
	KUNIT_EXPECT_EQ(test, ane_h16_run(&s->boot, ANE_H16_BOOT), -E2BIG);
	KUNIT_EXPECT_TRUE(test, s->boot.quarantined);
}

static void ane_h16_native_free_test(struct kunit *test)
{
	struct ane_h16_sim *s = test->priv;

	s->lifecycle_mode = 1;
	KUNIT_ASSERT_EQ(test, ane_h16_run(&s->boot, ANE_H16_STARTUP), 0);
	KUNIT_EXPECT_TRUE(test, s->lifecycle_requested);
	KUNIT_EXPECT_TRUE(test, s->boot.transport.shared_retired[0]);
	KUNIT_EXPECT_TRUE(test, s->boot.transport.shared[0].cpu);
}

static void ane_h16_control_refusal_test(struct kunit *test)
{
	struct ane_h16_sim *s = test->priv;

	s->lifecycle_mode = 2;
	KUNIT_EXPECT_EQ(test, ane_h16_run(&s->boot, ANE_H16_STARTUP), -EOPNOTSUPP);
	KUNIT_EXPECT_TRUE(test, s->boot.quarantined);
	KUNIT_EXPECT_FALSE(test, s->boot.transport.shared_retired[0]);
}

static void ane_h16_foreign_free_test(struct kunit *test)
{
	struct ane_h16_sim *s = test->priv;

	s->lifecycle_mode = 3;
	KUNIT_EXPECT_EQ(test, ane_h16_run(&s->boot, ANE_H16_STARTUP), -ENOENT);
	KUNIT_EXPECT_TRUE(test, s->boot.quarantined);
	KUNIT_EXPECT_FALSE(test, s->boot.transport.shared_retired[0]);
}

static void ane_h16_pre_io_allocation_test(struct kunit *test)
{
	struct ane_h16_sim *s = test->priv;
	u8 *slot;

	KUNIT_ASSERT_EQ(test, ane_h16_run(&s->boot, ANE_H16_BOOT), 0);
	slot = ane_fw_transport_slot(&s->boot.transport, ANE_FW_SHAREDMALLOC);
	put_unaligned_le64(0, slot);
	put_unaligned_le64(SZ_16K, slot + 8);
	put_unaligned_le64(0, slot + 16);
	s->must_allocate_first = true;
	s->boot.startup_drain_ms = 1;
	KUNIT_ASSERT_EQ(test, ane_h16_startup(&s->boot), 0);
	KUNIT_EXPECT_TRUE(test, s->shared_replied);
	KUNIT_EXPECT_EQ(test, s->command_count, 4U);
}

static void ane_h16_pre_io_fault_test(struct kunit *test)
{
	struct ane_h16_sim *s = test->priv;

	s->boot.startup_drain_ms = 10;
	s->fault_on_drain = true;
	KUNIT_EXPECT_EQ(test, ane_h16_run(&s->boot, ANE_H16_STARTUP), -EIO);
	KUNIT_EXPECT_EQ(test, s->command_count, 0U);
	KUNIT_EXPECT_TRUE(test, s->boot.quarantined);
	KUNIT_EXPECT_EQ(test, s->quarantines, 1U);
}

static struct kunit_case ane_h16_boot_cases[] = {
	KUNIT_CASE(ane_h16_pre_io_allocation_test),
	KUNIT_CASE(ane_h16_pre_io_fault_test),
	KUNIT_CASE(ane_h16_happy_test),
	KUNIT_CASE(ane_h16_locked_matching_test),
	KUNIT_CASE(ane_h16_heap_overlap_test),
	KUNIT_CASE(ane_h16_native_free_test),
	KUNIT_CASE(ane_h16_control_refusal_test),
	KUNIT_CASE(ane_h16_foreign_free_test),
	KUNIT_CASE(ane_h16_mailbox_test),
	KUNIT_CASE(ane_h16_no_heap_test),
	KUNIT_CASE(ane_h16_power_actual_test),
	KUNIT_CASE(ane_h16_rvbar_test),
	KUNIT_CASE(ane_h16_dart_fault_test),
	KUNIT_CASE(ane_h16_map_failure_test),
	KUNIT_CASE(ane_h16_no_ready_test),
	KUNIT_CASE(ane_h16_wrong_table_test),
	KUNIT_CASE(ane_h16_stale_table_test),
	KUNIT_CASE(ane_h16_oversized_ipc_test),
	KUNIT_CASE(ane_h16_oversized_heap_test),
	KUNIT_CASE(ane_h16_channel_limit_test),
	KUNIT_CASE(ane_h16_locked_dapf_test),
	KUNIT_CASE(ane_h16_nonzero_ack_test),
	KUNIT_CASE(ane_h16_preflight_test),
	KUNIT_CASE(ane_h16_default_test),
	KUNIT_CASE(ane_h16_ce_timeout_test),
	KUNIT_CASE(ane_h16_zero_tick_test),
	KUNIT_CASE(ane_h16_scratch3_timeout_test),
	{}
};

static struct kunit_suite ane_h16_boot_suite = {
	.name = "ane-h16-boot",
	.init = sim_init,
	.test_cases = ane_h16_boot_cases,
};

kunit_test_suite(ane_h16_boot_suite);

MODULE_DESCRIPTION("KUnit simulated H16 cold boot engine");
MODULE_LICENSE("Dual MIT/GPL");
