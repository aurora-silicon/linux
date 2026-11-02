// SPDX-License-Identifier: GPL-2.0-only OR MIT
#include <kunit/test.h>
#include <linux/module.h>
#include <linux/slab.h>

#include "ane_fw_transport.h"
#include "ane_h16_execute.h"

#define TEST_IPC_DVA 0x100003d0000ULL
#define TEST_POOL_DVA 0x10000384000ULL
#define TEST_SHARED_DVA 0x10002000000ULL
#define TEST_IPC_SIZE SZ_64K
#define TEST_POOL_SIZE SZ_256K
#define TEST_RESERVED SZ_16K
#define TEST_DEADLINE 100ULL

struct ane_test_access {
	char kind;
	const void *p;
	u64 value;
};

struct ane_transport_test {
	struct kunit *test;
	struct ane_fw_transport t;
	struct ane_fw_platform soc;
	struct ane_fw_surface ipc;
	struct ane_fw_surface pool;
	struct ane_test_access access[128];
	unsigned int count;
	u64 now;
	u64 alloc_time;
	u64 waited;
	u64 next_dva;
	u32 irq;
	u32 last_reg;
	u32 last_value;
	unsigned int allocs;
	unsigned int refusals;
	u64 refused[3];
	int refused_error;
	int alloc_error;
	int alloc_bad;
};

static void ane_test_access(struct ane_transport_test *f, char kind, const void *p, u64 value)
{
	if (f->count < ARRAY_SIZE(f->access))
		f->access[f->count++] = (struct ane_test_access) { kind, p, value };
}

static u64 ane_test_load(void *ctx, const void *p)
{
	struct ane_transport_test *f = ctx;
	u64 value = get_unaligned_le64(p);

	ane_test_access(f, 'r', p, value);
	return value;
}

static void ane_test_store(void *ctx, void *p, u64 value)
{
	struct ane_transport_test *f = ctx;

	ane_test_access(f, 'w', p, value);
	put_unaligned_le64(value, p);
}

static void ane_test_rmb(void *ctx)
{
	ane_test_access(ctx, 'R', NULL, 0);
}

static void ane_test_wmb(void *ctx)
{
	ane_test_access(ctx, 'W', NULL, 0);
}

static u32 ane_test_read32(void *ctx, u32 reg)
{
	struct ane_transport_test *f = ctx;

	f->last_reg = reg;
	ane_test_access(f, 'i', NULL, reg);
	return f->irq;
}

static void ane_test_write32(void *ctx, u32 reg, u32 value)
{
	struct ane_transport_test *f = ctx;

	f->last_reg = reg;
	f->last_value = value;
	ane_test_access(f, 'o', NULL, value);
}

static u64 ane_test_now(void *ctx)
{
	return ((struct ane_transport_test *)ctx)->now;
}

static void ane_test_relax(void *ctx, u64 waited_ns)
{
	struct ane_transport_test *f = ctx;

	f->waited = waited_ns;
	f->now++;
}

static int ane_test_alloc(void *ctx, size_t size, u32 tag, struct ane_fw_surface *s)
{
	struct ane_transport_test *f = ctx;

	f->allocs++;
	f->now += f->alloc_time;
	if (f->alloc_error)
		return f->alloc_error;
	*s = (struct ane_fw_surface) {
		.cpu = kunit_kzalloc(f->test, size, GFP_KERNEL),
		.dva = f->next_dva,
		.size = size,
	};
	if (!s->cpu)
		return -ENOMEM;
	f->next_dva += size;
	switch (f->alloc_bad) {
	case 1:
		s->dva++;
		break;
	case 2:
		s->size--;
		break;
	case 3:
		s->dva = f->pool.dva;
		break;
	case 4:
		s->dva = f->ipc.dva;
		break;
	case 5:
		s->dva = U64_MAX - SZ_16K + 1;
		break;
	case 6:
		s->cpu = NULL;
		break;
	}
	return 0;
}

static void ane_test_refused(void *ctx, u64 word0, u64 word1, u64 word2, int error)
{
	struct ane_transport_test *f = ctx;

	f->refusals++;
	f->refused[0] = word0;
	f->refused[1] = word1;
	f->refused[2] = word2;
	f->refused_error = error;
}

static const struct ane_fw_transport_ops ane_test_ops = {
	.load64 = ane_test_load,
	.store64 = ane_test_store,
	.read_barrier = ane_test_rmb,
	.write_barrier = ane_test_wmb,
	.read32 = ane_test_read32,
	.write32 = ane_test_write32,
	.now_ns = ane_test_now,
	.relax = ane_test_relax,
	.alloc = ane_test_alloc,
	.refused = ane_test_refused,
};

static void ane_test_table(struct ane_transport_test *f)
{
	static const char * const names[] = {
		"TERMINAL", "IO", "DEBUG", "BUF_H2T", "BUF_T2H",
		"SHAREDMALLOC", "IO_T2H", "DATA_CHAIN_H2T",
	};
	unsigned int i;

	memset(f->ipc.cpu, 0, f->ipc.size);
	for (i = 0; i < ANE_FW_CHANNELS; i++) {
		u8 *p = (u8 *)f->ipc.cpu + i * ANE_FW_DESCRIPTOR_SIZE;

		strscpy((char *)p, names[i], 64);
		put_unaligned_le32(i == 0 ? 2 : (i >= 4 && i <= 6), p + 0x40);
		put_unaligned_le32(i, p + 0x44);
		put_unaligned_le64(2, p + 0x48);
		put_unaligned_le64(f->ipc.dva + 0x800 + i * 0x80, p + 0x50);
	}
}

static int ane_test_init_transport(struct ane_transport_test *f)
{
	return ane_fw_transport_init(&f->t, &f->soc, &ane_test_ops, f,
				     f->ipc, f->pool, TEST_RESERVED,
				     f->ipc.dva, ANE_FW_TABLE_SIZE);
}

static int ane_transport_test_init(struct kunit *test)
{
	struct ane_transport_test *f = kunit_kzalloc(test, sizeof(*f), GFP_KERNEL);

	if (!f)
		return -ENOMEM;
	test->priv = f;
	f->test = test;
	f->soc.doorbell = 0x1884000;
	f->soc.irq_status = 0x188c000;
	f->soc.irq_ack = 0x1890000;
	f->ipc = (struct ane_fw_surface) {
		.cpu = kunit_kzalloc(test, TEST_IPC_SIZE, GFP_KERNEL),
		.dva = TEST_IPC_DVA,
		.size = TEST_IPC_SIZE,
	};
	f->pool = (struct ane_fw_surface) {
		.cpu = kunit_kzalloc(test, TEST_POOL_SIZE, GFP_KERNEL),
		.dva = TEST_POOL_DVA,
		.size = TEST_POOL_SIZE,
	};
	if (!f->ipc.cpu || !f->pool.cpu)
		return -ENOMEM;
	f->next_dva = TEST_SHARED_DVA;
	ane_test_table(f);
	return ane_test_init_transport(f);
}

static u8 *ane_test_slot(struct ane_transport_test *f, unsigned int ch)
{
	return ane_fw_transport_slot(&f->t, ch);
}

static int ane_test_publish(struct ane_transport_test *f, unsigned int ch)
{
	u8 command[16] = {};

	put_unaligned_le16(ch == ANE_FW_IO ? 3 : 0x2d, command + 4);
	return ane_fw_transport_publish(&f->t, ch, command, sizeof(command), TEST_DEADLINE);
}

static void ane_test_reply(struct ane_transport_test *f, unsigned int ch)
{
	const struct ane_fw_transaction *tx = &f->t.pending[ch];
	u8 *p = ane_test_slot(f, ch);

	put_unaligned_le64(tx->dva | 1, p);
	put_unaligned_le64(tx->size, p + 8);
	put_unaligned_le64(0, p + 16);
}

static void ane_test_shared_request(struct ane_transport_test *f, u64 w0, u64 w1, u64 w2)
{
	u8 *p = ane_test_slot(f, ANE_FW_SHAREDMALLOC);

	put_unaligned_le64(w0, p);
	put_unaligned_le64(w1, p + 8);
	put_unaligned_le64(w2, p + 16);
}

static unsigned int ane_test_writes(struct ane_transport_test *f)
{
	unsigned int i, n = 0;

	for (i = 0; i < f->count; i++)
		if (f->access[i].kind == 'w' || f->access[i].kind == 'o')
			n++;
	return n;
}

static void ane_transport_table_test(struct kunit *test)
{
	struct ane_transport_test *f = test->priv;
	struct ane_fw_channel parsed[ANE_FW_CHANNELS];
	u8 *d = (u8 *)f->ipc.cpu + ANE_FW_DESCRIPTOR_SIZE;
	unsigned int i;
	int ret;

	for (i = 0; i < 15; i++) {
		ane_test_table(f);
		switch (i) {
		case 0:
			put_unaligned_le64(0, d + 0x48);
			break;
		case 1:
			put_unaligned_le64(0x401, d + 0x48);
			break;
		case 2:
			put_unaligned_le64(U64_MAX, d + 0x48);
			break;
		case 3:
			put_unaligned_le64(f->ipc.dva, d + 0x50);
			break;
		case 4:
			put_unaligned_le64(f->ipc.dva + 0x800, d + 0x50);
			break;
		case 5:
			put_unaligned_le64(f->ipc.dva + f->ipc.size - 64, d + 0x50);
			break;
		case 6:
			put_unaligned_le64(f->ipc.dva + 0x881, d + 0x50);
			break;
		case 7:
			put_unaligned_le64(f->ipc.dva - 64, d + 0x50);
			break;
		case 8:
			put_unaligned_le32(32, d + 0x44);
			break;
		case 9:
			put_unaligned_le32(3, d + 0x40);
			break;
		case 10:
			put_unaligned_le32(1, d + 0x40);
			break;
		case 11:
			strscpy((char *)d, "TERMINAL", 64);
			break;
		case 12:
			memset(d, 'A', 64);
			break;
		case 13:
			strscpy((char *)d, "UNKNOWN", 64);
			break;
		case 14:
			put_unaligned_le64(U64_MAX - 63, d + 0x50);
			break;
		}
		memset(parsed, 0xa5, sizeof(parsed));
		ret = ane_fw_channels_parse(f->ipc.cpu, f->ipc.dva, f->ipc.size,
					    f->ipc.dva, parsed);
		KUNIT_EXPECT_EQ_MSG(test, ret, -EPROTO, "mutation %u", i);
		KUNIT_EXPECT_PTR_EQ(test, memchr_inv(parsed, 0xa5, sizeof(parsed)), NULL);
	}
	KUNIT_EXPECT_FALSE(test, ane_fw_contains(U64_MAX - 15, 64, U64_MAX - 15, 8));
}

static void ane_transport_table_roles_test(struct kunit *test)
{
	struct ane_transport_test *f = test->priv;
	unsigned int ch;

	for (ch = 5; ch <= 7; ch++) {
		u8 *d = (u8 *)f->ipc.cpu + ch * ANE_FW_DESCRIPTOR_SIZE;

		ane_test_table(f);
		put_unaligned_le32(ch == 7 ? 1 : 0, d + 0x40);
		KUNIT_EXPECT_EQ(test, ane_test_init_transport(f), -EPROTO);
		KUNIT_EXPECT_EQ(test, ane_fw_transport_arm(&f->t), -EPROTO);
		KUNIT_EXPECT_EQ(test, ane_test_writes(f), 0U);
	}
}

static void ane_transport_table_bounds_test(struct kunit *test)
{
	struct ane_transport_test *f = test->priv;
	struct ane_fw_channel channels[ANE_FW_CHANNELS];
	unsigned int i;
	int ret;

	for (i = 0; i < ANE_FW_TABLE_SIZE; i++) {
		ret = ane_fw_transport_init(&f->t, &f->soc, &ane_test_ops, f,
					    f->ipc, f->pool, 0, f->ipc.dva, i);
		KUNIT_EXPECT_EQ(test, ret, -EINVAL);
	}
	ret = ane_fw_transport_init(&f->t, &f->soc, &ane_test_ops, f, f->ipc,
				    f->pool, 0, f->ipc.dva, ANE_FW_TABLE_SIZE + 1);
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);
	ret = ane_fw_channels_parse(f->ipc.cpu, f->ipc.dva, f->ipc.size,
				    f->ipc.dva + f->ipc.size - 64, channels);
	KUNIT_EXPECT_EQ(test, ret, -EPROTO);
	ret = ane_fw_channels_parse(NULL, f->ipc.dva, f->ipc.size, f->ipc.dva, channels);
	KUNIT_EXPECT_EQ(test, ret, -EPROTO);
	KUNIT_EXPECT_EQ(test, ane_test_writes(f), 0U);
}

static void ane_transport_table_edges_test(struct kunit *test)
{
	struct ane_transport_test *f = test->priv;
	u8 *d;

	/* One 0x400-slot ring fits exactly after a relocated table and seven slots. */
	f->ipc.size = 2 * TEST_IPC_SIZE;
	f->ipc.cpu = kunit_kzalloc(test, f->ipc.size, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, f->ipc.cpu);
	ane_test_table(f);
	d = f->ipc.cpu;
	put_unaligned_le64(0x400, d + 0x48);
	put_unaligned_le64(f->ipc.dva + TEST_IPC_SIZE, d + 0x50);
	put_unaligned_le32(31, d + 0x44);
	KUNIT_EXPECT_EQ(test, ane_test_init_transport(f), 0);
	put_unaligned_le64(1, d + 0x48);
	put_unaligned_le64(f->ipc.dva + ANE_FW_TABLE_SIZE, d + 0x50);
	KUNIT_EXPECT_EQ(test, ane_test_init_transport(f), 0);
}

static void ane_transport_arm_test(struct kunit *test)
{
	struct ane_transport_test *f = test->priv;
	unsigned int ch, slot, n = 1;

	memset((u8 *)f->ipc.cpu + 0x800, 0xa5, 0x400);
	f->count = 0;
	KUNIT_ASSERT_EQ(test, ane_fw_transport_arm(&f->t), 0);
	for (ch = 0; ch < ANE_FW_CHANNELS; ch++) {
		for (slot = 0; slot < 2; slot++) {
			u8 *p = ane_test_slot(f, ch) + slot * ANE_FW_SLOT_SIZE;

			if (f->t.channels[ch].type) {
				KUNIT_EXPECT_PTR_EQ(test, memchr_inv(p, 0xa5, 64), NULL);
				continue;
			}
			KUNIT_EXPECT_EQ(test, get_unaligned_le64(p), 1ULL);
			KUNIT_EXPECT_EQ(test, get_unaligned_le64(p + 8), 0ULL);
			KUNIT_EXPECT_EQ(test, get_unaligned_le64(p + 16), 0ULL);
			KUNIT_EXPECT_PTR_EQ(test, memchr_inv(p + 24, 0xa5, 40), NULL);
			KUNIT_EXPECT_PTR_EQ(test, f->access[n++].p, p + 8);
			KUNIT_EXPECT_PTR_EQ(test, f->access[n++].p, p + 16);
			KUNIT_EXPECT_PTR_EQ(test, f->access[n++].p, p);
		}
	}
	KUNIT_EXPECT_EQ(test, f->access[n].kind, 'W');
	KUNIT_EXPECT_EQ(test, ane_fw_transport_arm(&f->t), -EBUSY);
}

static void ane_transport_publish_order_test(struct kunit *test)
{
	struct ane_transport_test *f = test->priv;
	unsigned int ch;

	KUNIT_ASSERT_EQ(test, ane_fw_transport_arm(&f->t), 0);
	for (ch = ANE_FW_IO; ch <= ANE_FW_DATA_CHAIN; ch += 6) {
		u8 *p = ane_test_slot(f, ch);
		u64 dva;

		f->count = 0;
		KUNIT_ASSERT_EQ(test, ane_test_publish(f, ch), 0);
		dva = get_unaligned_le64(p);
		KUNIT_EXPECT_TRUE(test, ane_fw_contains(f->pool.dva, f->pool.size, dva, 16));
		KUNIT_EXPECT_GE(test, dva, f->pool.dva + TEST_RESERVED);
		KUNIT_EXPECT_EQ(test, dva & 63, 0ULL);
		KUNIT_EXPECT_EQ(test, f->access[2].kind, 'w');
		KUNIT_EXPECT_PTR_EQ(test, f->access[2].p, p + 8);
		KUNIT_EXPECT_EQ(test, f->access[2].value, 16ULL);
		KUNIT_EXPECT_PTR_EQ(test, f->access[3].p, p + 16);
		KUNIT_EXPECT_EQ(test, f->access[3].value, 16ULL);
		KUNIT_EXPECT_EQ(test, f->access[4].kind, 'W');
		KUNIT_EXPECT_PTR_EQ(test, f->access[5].p, p);
		KUNIT_EXPECT_EQ(test, f->access[5].value, dva);
		KUNIT_EXPECT_EQ(test, f->access[6].kind, 'W');
		KUNIT_EXPECT_EQ(test, f->access[7].kind, 'o');
		KUNIT_EXPECT_EQ(test, f->last_reg, f->soc.doorbell);
		KUNIT_EXPECT_EQ(test, f->last_value, (u32)BIT(ch));
		KUNIT_EXPECT_EQ(test, ane_test_publish(f, ch), -EBUSY);
		KUNIT_EXPECT_EQ(test, ane_fw_transport_poll(&f->t, ch), -EAGAIN);
		ane_test_reply(f, ch);
		KUNIT_EXPECT_EQ(test, ane_fw_transport_poll(&f->t, ch), 0);
		KUNIT_EXPECT_EQ(test, f->t.head[ch], 1U);
	}
}

static void ane_transport_wrap_test(struct kunit *test)
{
	struct ane_transport_test *f = test->priv;
	unsigned int i;

	KUNIT_ASSERT_EQ(test, ane_fw_transport_arm(&f->t), 0);
	for (i = 0; i < 5; i++) {
		KUNIT_ASSERT_EQ(test, ane_test_publish(f, ANE_FW_DATA_CHAIN), 0);
		ane_test_reply(f, ANE_FW_DATA_CHAIN);
		KUNIT_ASSERT_EQ(test, ane_fw_transport_poll(&f->t, ANE_FW_DATA_CHAIN), 0);
		KUNIT_EXPECT_EQ(test, f->t.head[ANE_FW_DATA_CHAIN], (i + 1) % 2);
	}
}

static void ane_transport_reply_corruption_test(struct kunit *test)
{
	struct ane_transport_test *f = test->priv;
	unsigned int i;

	for (i = 0; i < 8; i++) {
		u8 *p, *command;
		u64 dva;

		KUNIT_ASSERT_EQ(test, ane_test_init_transport(f), 0);
		KUNIT_ASSERT_EQ(test, ane_fw_transport_arm(&f->t), 0);
		KUNIT_ASSERT_EQ(test, ane_test_publish(f, ANE_FW_IO), 0);
		ane_test_reply(f, ANE_FW_IO);
		p = ane_test_slot(f, ANE_FW_IO);
		dva = f->t.pending[ANE_FW_IO].dva;
		command = (u8 *)f->pool.cpu + (dva - f->pool.dva);
		switch (i) {
		case 0:
			put_unaligned_le64(dva | 3, p);
			break;
		case 1:
			put_unaligned_le64(dva | 2, p);
			break;
		case 2:
			put_unaligned_le64((dva + 64) | 1, p);
			break;
		case 3:
			put_unaligned_le64(15, p + 8);
			break;
		case 4:
			put_unaligned_le64(1, p + 16);
			break;
		case 5:
			put_unaligned_le32(0x1000000, command);
			break;
		case 6:
			put_unaligned_le16(4, command + 4);
			break;
		case 7:
			put_unaligned_le64(dva + 64, p);
			break;
		}
		KUNIT_EXPECT_EQ_MSG(test, ane_fw_transport_poll(&f->t, ANE_FW_IO),
				    -EPROTO, "mutation %u", i);
		KUNIT_EXPECT_TRUE(test, f->t.pending[ANE_FW_IO].active);
		KUNIT_EXPECT_EQ(test, f->t.head[ANE_FW_IO], 0U);
		f->count = 0;
		KUNIT_EXPECT_EQ(test, ane_test_publish(f, ANE_FW_DATA_CHAIN), -EPROTO);
		KUNIT_EXPECT_EQ(test, ane_test_writes(f), 0U);
	}
}

static void ane_transport_refusal_test(struct kunit *test)
{
	struct ane_transport_test *f = test->priv;
	u8 *command;

	KUNIT_ASSERT_EQ(test, ane_fw_transport_arm(&f->t), 0);
	KUNIT_ASSERT_EQ(test, ane_test_publish(f, ANE_FW_IO), 0);
	command = (u8 *)f->pool.cpu + (f->t.pending[ANE_FW_IO].dva - f->pool.dva);
	ane_test_reply(f, ANE_FW_IO);
	put_unaligned_le16(0x100, command + 6);
	KUNIT_EXPECT_EQ(test, ane_fw_transport_poll(&f->t, ANE_FW_IO), -EREMOTEIO);
	KUNIT_EXPECT_TRUE(test, f->t.pending[ANE_FW_IO].active);
	KUNIT_EXPECT_EQ(test, f->t.head[ANE_FW_IO], 0U);
	KUNIT_EXPECT_EQ(test, get_unaligned_le16(command + 6), (u16)0x100);
}

static void ane_transport_pool_test(struct kunit *test)
{
	struct ane_transport_test *f = test->priv;
	u8 command[16] = {};

	KUNIT_EXPECT_EQ(test, ane_test_publish(f, ANE_FW_IO), -EINVAL);
	KUNIT_ASSERT_EQ(test, ane_fw_transport_arm(&f->t), 0);
	KUNIT_EXPECT_EQ(test, ane_fw_transport_publish(&f->t, 2, command, 16, 100), -EINVAL);
	KUNIT_EXPECT_EQ(test, ane_fw_transport_publish(&f->t, 1, NULL, 16, 100), -EINVAL);
	KUNIT_EXPECT_EQ(test, ane_fw_transport_publish(&f->t, 1, command, 7, 100), -EINVAL);
	command[0] = 1;
	KUNIT_EXPECT_EQ(test, ane_fw_transport_publish(&f->t, 1, command, 16, 100), -EINVAL);
	command[0] = 0;
	command[6] = 1;
	KUNIT_EXPECT_EQ(test, ane_fw_transport_publish(&f->t, 1, command, 16, 100), -EINVAL);
	command[6] = 0;
	memset(f->pool.cpu, 0xa5, TEST_RESERVED);
	KUNIT_ASSERT_EQ(test, ane_test_publish(f, ANE_FW_IO), 0);
	KUNIT_EXPECT_PTR_EQ(test, memchr_inv(f->pool.cpu, 0xa5, TEST_RESERVED), NULL);
	ane_test_reply(f, ANE_FW_IO);
	KUNIT_ASSERT_EQ(test, ane_fw_transport_poll(&f->t, ANE_FW_IO), 0);
	f->t.pool_used = f->pool.size - 1;
	f->count = 0;
	KUNIT_EXPECT_EQ(test, ane_test_publish(f, ANE_FW_IO), -ENOSPC);
	KUNIT_EXPECT_EQ(test, ane_test_writes(f), 0U);
	f->t.pool_used = SIZE_MAX;
	KUNIT_EXPECT_EQ(test, ane_test_publish(f, ANE_FW_IO), -ENOSPC);
}

static void ane_transport_snapshot_test(struct kunit *test)
{
	struct ane_transport_test *f = test->priv;

	((u8 *)f->ipc.cpu)[0x58] = 1;
	f->count = 0;
	KUNIT_EXPECT_EQ(test, ane_fw_transport_arm(&f->t), -EPROTO);
	KUNIT_EXPECT_EQ(test, ane_test_writes(f), 0U);
}

static void ane_transport_report_request(struct ane_transport_test *f, u64 dva, u64 size)
{
	u8 *p = ane_test_slot(f, ANE_FW_IO_T2H);

	put_unaligned_le64(dva, p);
	put_unaligned_le64(size, p + 8);
	put_unaligned_le64(0, p + 16);
}

static void ane_transport_consume_test(struct kunit *test)
{
	struct ane_transport_test *f = test->priv;
	struct ane_h16_request_key key = { .uuid = 9, .program = 1, .process = 2 };
	u8 report[0x60] = {};
	u8 *source = (u8 *)f->pool.cpu + 0x100;
	u8 *p;
	size_t size = 0;

	KUNIT_ASSERT_EQ(test, ane_fw_transport_arm(&f->t), 0);
	KUNIT_EXPECT_EQ(test, ane_fw_transport_consume(&f->t, report, sizeof(report), &size),
			-EAGAIN);
	put_unaligned_le16(0x300, source + 4);
	put_unaligned_le64(key.uuid, source + 8);
	put_unaligned_le32(key.program, source + 0x10);
	put_unaligned_le32(key.process, source + 0x14);
	put_unaligned_le32(1, source + 0x1c);
	ane_transport_report_request(f, f->pool.dva + 0x100, sizeof(report));
	p = ane_test_slot(f, ANE_FW_IO_T2H);
	KUNIT_EXPECT_EQ(test, ane_fw_transport_consume(&f->t, report, 8, &size), -ENOSPC);
	KUNIT_EXPECT_EQ(test, get_unaligned_le64(p), f->pool.dva + 0x100);
	f->count = 0;
	KUNIT_ASSERT_EQ(test, ane_fw_transport_consume(&f->t, report, sizeof(report), &size), 0);
	KUNIT_EXPECT_MEMEQ(test, report, source, sizeof(report));
	KUNIT_EXPECT_EQ(test, size, sizeof(report));
	KUNIT_EXPECT_EQ(test, get_unaligned_le64(p), (f->pool.dva + 0x100) | 1);
	KUNIT_EXPECT_EQ(test, get_unaligned_le64(p + 8), 0x60ULL);
	KUNIT_EXPECT_EQ(test, get_unaligned_le64(p + 16), 0ULL);
	KUNIT_EXPECT_EQ(test, f->access[f->count - 4].kind, 'W');
	KUNIT_EXPECT_PTR_EQ(test, f->access[f->count - 3].p, p);
	KUNIT_EXPECT_EQ(test, f->access[f->count - 2].kind, 'W');
	KUNIT_EXPECT_EQ(test, f->last_value, (u32)BIT(6));
	KUNIT_EXPECT_EQ(test, ane_h16_event_result(report, size, 6, &key),
			ANE_H16_EVENT_COMPLETE);
	/* A stale UUID is consumed, but cannot retire the current pending request. */
	key.uuid++;
	KUNIT_EXPECT_EQ(test, ane_h16_event_result(report, size, 6, &key), -EPROTO);
	KUNIT_EXPECT_EQ(test, ane_fw_transport_consume(&f->t, report, sizeof(report), &size),
			-EAGAIN);
}

static void ane_transport_report_bounds_test(struct kunit *test)
{
	struct ane_transport_test *f = test->priv;
	u8 report[0x60];
	size_t size;
	unsigned int i;
	int ret;

	for (i = 0; i < 7; i++) {
		u64 dva = f->pool.dva, bytes = sizeof(report);

		KUNIT_ASSERT_EQ(test, ane_test_init_transport(f), 0);
		KUNIT_ASSERT_EQ(test, ane_fw_transport_arm(&f->t), 0);
		switch (i) {
		case 0:
			dva += 2;
			break;
		case 1:
			dva = f->pool.dva + f->pool.size - 4;
			break;
		case 2:
			dva = TEST_SHARED_DVA;
			break;
		case 3:
			bytes = U64_MAX;
			break;
		case 4:
			bytes = 0;
			break;
		case 5:
			bytes = 7;
			break;
		case 6:
			dva = 0;
			break;
		}
		ane_transport_report_request(f, dva, bytes);
		f->count = 0;
		ret = ane_fw_transport_consume(&f->t, report, sizeof(report), &size);
		KUNIT_EXPECT_EQ_MSG(test, ret, -EPROTO, "mutation %u", i);
		KUNIT_EXPECT_EQ(test, ane_test_writes(f), 0U);
	}
}

static void ane_transport_report_word2_test(struct kunit *test)
{
	struct ane_transport_test *f = test->priv;
	u8 report[12];
	size_t size;

	KUNIT_ASSERT_EQ(test, ane_fw_transport_arm(&f->t), 0);
	ane_transport_report_request(f, f->pool.dva, sizeof(report));
	put_unaligned_le64(1, ane_test_slot(f, ANE_FW_IO_T2H) + 16);
	KUNIT_EXPECT_EQ(test, ane_fw_transport_consume(&f->t, report, sizeof(report), &size),
			-EPROTO);
}

static void ane_transport_presence_test(struct kunit *test)
{
	KUNIT_EXPECT_FALSE(test, ane_fw_transport_present(0, 0));
	KUNIT_EXPECT_FALSE(test, ane_fw_transport_present(1, 0));
	KUNIT_EXPECT_FALSE(test, ane_fw_transport_present(0x1001, 96));
	KUNIT_EXPECT_TRUE(test, ane_fw_transport_present(0x1000, 0));
	KUNIT_EXPECT_TRUE(test, ane_fw_transport_present(0, 96));
	KUNIT_EXPECT_TRUE(test, ane_fw_transport_present(0x1000, 96));
}

static void ane_transport_shared_test(struct kunit *test)
{
	struct ane_transport_test *f = test->priv;
	u8 *p;

	KUNIT_ASSERT_EQ(test, ane_fw_transport_arm(&f->t), 0);
	KUNIT_EXPECT_EQ(test, ane_fw_transport_shared(&f->t), -EAGAIN);
	ane_test_shared_request(f, 0, 1, 0x1234);
	p = ane_test_slot(f, ANE_FW_SHAREDMALLOC);
	f->count = 0;
	KUNIT_ASSERT_EQ(test, ane_fw_transport_shared(&f->t), 0);
	KUNIT_EXPECT_EQ(test, get_unaligned_le64(p), TEST_SHARED_DVA | 1);
	KUNIT_EXPECT_EQ(test, get_unaligned_le64(p + 8), 0ULL);
	KUNIT_EXPECT_EQ(test, get_unaligned_le64(p + 16), 1ULL);
	KUNIT_EXPECT_EQ(test, f->t.shared_bytes, (size_t)SZ_16K);
	KUNIT_EXPECT_EQ(test, f->access[f->count - 6].value, 0ULL);
	KUNIT_EXPECT_EQ(test, f->access[f->count - 5].value, 1ULL);
	KUNIT_EXPECT_EQ(test, f->access[f->count - 4].kind, 'W');
	KUNIT_EXPECT_PTR_EQ(test, f->access[f->count - 3].p, p);
	KUNIT_EXPECT_EQ(test, f->access[f->count - 2].kind, 'W');
	KUNIT_EXPECT_EQ(test, f->last_value, (u32)BIT(5));
	ane_test_shared_request(f, 0, SZ_2M, ANE_FW_SHARED_EXPP);
	KUNIT_ASSERT_EQ(test, ane_fw_transport_shared(&f->t), 0);
	KUNIT_EXPECT_EQ(test, f->t.shared_bytes, (size_t)(SZ_2M + SZ_16K));
	KUNIT_EXPECT_EQ(test, f->t.shared_count, 2U);
}

static void ane_transport_shared_rejections_test(struct kunit *test)
{
	static const u64 tuples[][3] = {
		{ 0x10000, SZ_16K, 0 },
		{ 0, 0, 1 },
		{ 0, SZ_1M + 1, 0 },
		{ 0, SZ_1M + 1, ANE_FW_SHARED_EXPP },
		{ 0, SZ_2M, ANE_FW_SHARED_EXPP + 1 },
		{ 0, SZ_2M + 1, ANE_FW_SHARED_EXPP },
		{ 0, U64_MAX, 0 },
		{ 0, SZ_16K, BIT_ULL(32) },
	};
	struct ane_transport_test *f = test->priv;
	size_t size;
	unsigned int i;

	KUNIT_EXPECT_EQ(test, ane_fw_shared_size(0, 0, 0, &size), -EOPNOTSUPP);
	for (i = 0; i < ARRAY_SIZE(tuples); i++) {
		int error = i < 2 ? -EOPNOTSUPP : -E2BIG;

		KUNIT_ASSERT_EQ(test, ane_test_init_transport(f), 0);
		KUNIT_ASSERT_EQ(test, ane_fw_transport_arm(&f->t), 0);
		ane_test_shared_request(f, tuples[i][0], tuples[i][1], tuples[i][2]);
		f->count = 0;
		KUNIT_EXPECT_EQ(test, ane_fw_transport_shared(&f->t), error);
		KUNIT_EXPECT_EQ(test, f->refusals, i + 1);
		KUNIT_EXPECT_MEMEQ(test, f->refused, tuples[i], sizeof(f->refused));
		KUNIT_EXPECT_EQ(test, f->refused_error, error);
		KUNIT_EXPECT_EQ(test, f->allocs, 0U);
		KUNIT_EXPECT_EQ(test, ane_test_writes(f), 0U);
	}
}

static void ane_transport_allocator_rejections_test(struct kunit *test)
{
	struct ane_transport_test *f = test->priv;
	unsigned int i;

	for (i = 1; i <= 6; i++) {
		KUNIT_ASSERT_EQ(test, ane_test_init_transport(f), 0);
		KUNIT_ASSERT_EQ(test, ane_fw_transport_arm(&f->t), 0);
		f->alloc_bad = i;
		ane_test_shared_request(f, 0, SZ_16K, 0);
		f->count = 0;
		KUNIT_EXPECT_EQ_MSG(test, ane_fw_transport_shared(&f->t), -EPROTO,
				    "allocator result %u", i);
		KUNIT_EXPECT_EQ(test, f->t.shared_count, 0U);
		KUNIT_EXPECT_EQ(test, ane_test_writes(f), 0U);
	}
	KUNIT_ASSERT_EQ(test, ane_test_init_transport(f), 0);
	KUNIT_ASSERT_EQ(test, ane_fw_transport_arm(&f->t), 0);
	f->alloc_error = -ENOMEM;
	ane_test_shared_request(f, 0, SZ_16K, 0);
	KUNIT_EXPECT_EQ(test, ane_fw_transport_shared(&f->t), -ENOMEM);
}

static void ane_transport_shared_overlap_test(struct kunit *test)
{
	struct ane_transport_test *f = test->priv;

	KUNIT_ASSERT_EQ(test, ane_fw_transport_arm(&f->t), 0);
	ane_test_shared_request(f, 0, SZ_16K, 0);
	KUNIT_ASSERT_EQ(test, ane_fw_transport_shared(&f->t), 0);
	f->next_dva = TEST_SHARED_DVA;
	ane_test_shared_request(f, 0, SZ_16K, 0);
	KUNIT_EXPECT_EQ(test, ane_fw_transport_shared(&f->t), -EPROTO);
	KUNIT_EXPECT_EQ(test, f->t.shared_count, 1U);
}

static void ane_transport_shared_count_test(struct kunit *test)
{
	struct ane_transport_test *f = test->priv;
	unsigned int i;

	KUNIT_ASSERT_EQ(test, ane_fw_transport_arm(&f->t), 0);
	for (i = 0; i < ANE_FW_SHARED_MAX; i++) {
		ane_test_shared_request(f, 0, SZ_16K, 0);
		KUNIT_ASSERT_EQ(test, ane_fw_transport_shared(&f->t), 0);
	}
	ane_test_shared_request(f, 0, SZ_16K, 0);
	KUNIT_EXPECT_EQ(test, ane_fw_transport_shared(&f->t), -ENOSPC);
	KUNIT_EXPECT_EQ(test, f->allocs, (unsigned int)ANE_FW_SHARED_MAX);
}

static void ane_transport_shared_total_test(struct kunit *test)
{
	struct ane_transport_test *f = test->priv;
	unsigned int i;

	KUNIT_ASSERT_EQ(test, ane_fw_transport_arm(&f->t), 0);
	for (i = 0; i < 12; i++) {
		ane_test_shared_request(f, 0, SZ_1M, 0);
		KUNIT_ASSERT_EQ(test, ane_fw_transport_shared(&f->t), 0);
	}
	KUNIT_EXPECT_EQ(test, f->t.shared_bytes, (size_t)ANE_FW_SHARED_TOTAL);
	ane_test_shared_request(f, 0, 1, 0);
	KUNIT_EXPECT_EQ(test, ane_fw_transport_shared(&f->t), -ENOSPC);
	KUNIT_EXPECT_EQ(test, f->allocs, 12U);
}

static void ane_transport_shared_report_test(struct kunit *test)
{
	struct ane_transport_test *f = test->priv;
	u8 report[12];
	size_t size;

	KUNIT_ASSERT_EQ(test, ane_fw_transport_arm(&f->t), 0);
	ane_test_shared_request(f, 0, SZ_16K, 0);
	KUNIT_ASSERT_EQ(test, ane_fw_transport_shared(&f->t), 0);
	put_unaligned_le16(0x107, (u8 *)f->t.shared[0].cpu + 4);
	ane_transport_report_request(f, TEST_SHARED_DVA, sizeof(report));
	KUNIT_ASSERT_EQ(test, ane_fw_transport_consume(&f->t, report, sizeof(report), &size), 0);
	KUNIT_EXPECT_EQ(test, get_unaligned_le16(report + 4), (u16)0x107);
	KUNIT_EXPECT_EQ(test, size, sizeof(report));
}

static void ane_transport_irq_test(struct kunit *test)
{
	struct ane_transport_test *f = test->priv;
	u32 pending = 0;

	f->count = 0;
	KUNIT_ASSERT_EQ(test, ane_fw_transport_irq(&f->t, &pending), 0);
	KUNIT_EXPECT_EQ(test, ane_test_writes(f), 0U);
	f->irq = 0xa5;
	KUNIT_ASSERT_EQ(test, ane_fw_transport_irq(&f->t, &pending), 0);
	KUNIT_EXPECT_EQ(test, pending, 0xa5U);
	KUNIT_EXPECT_EQ(test, f->last_reg, f->soc.irq_ack);
	KUNIT_EXPECT_EQ(test, f->last_value, 0xa5U);
	f->irq = BIT(8) | 1;
	f->count = 0;
	KUNIT_EXPECT_EQ(test, ane_fw_transport_irq(&f->t, &pending), -EPROTO);
	KUNIT_EXPECT_EQ(test, pending, 0xa5U);
	KUNIT_EXPECT_EQ(test, ane_test_writes(f), 0U);
}

static void ane_transport_match_data_test(struct kunit *test)
{
	struct ane_transport_test *f = test->priv;
	u8 *d = (u8 *)f->ipc.cpu + ANE_FW_DESCRIPTOR_SIZE;
	u32 pending;

	f->soc.doorbell = 0x100;
	f->soc.irq_status = 0x200;
	f->soc.irq_ack = 0x300;
	put_unaligned_le32(31, d + 0x44);
	KUNIT_ASSERT_EQ(test, ane_test_init_transport(f), 0);
	KUNIT_ASSERT_EQ(test, ane_fw_transport_arm(&f->t), 0);
	KUNIT_ASSERT_EQ(test, ane_test_publish(f, ANE_FW_IO), 0);
	KUNIT_EXPECT_EQ(test, f->last_reg, 0x100U);
	KUNIT_EXPECT_EQ(test, f->last_value, (u32)BIT(31));
	f->irq = 0xff;
	KUNIT_ASSERT_EQ(test, ane_fw_transport_irq(&f->t, &pending), 0);
	KUNIT_EXPECT_EQ(test, f->last_reg, 0x300U);
	KUNIT_EXPECT_EQ(test, f->last_value, 0xffU);
}

static void ane_transport_deadline_test(struct kunit *test)
{
	struct ane_transport_test *f = test->priv;

	KUNIT_ASSERT_EQ(test, ane_fw_transport_arm(&f->t), 0);
	KUNIT_ASSERT_EQ(test, ane_test_publish(f, ANE_FW_IO), 0);
	f->now = TEST_DEADLINE - 1;
	KUNIT_EXPECT_EQ(test, ane_fw_transport_poll(&f->t, ANE_FW_IO), -EAGAIN);
	f->now++;
	ane_test_reply(f, ANE_FW_IO);
	KUNIT_EXPECT_EQ(test, ane_fw_transport_poll(&f->t, ANE_FW_IO), -ETIMEDOUT);
	KUNIT_EXPECT_TRUE(test, f->t.pending[ANE_FW_IO].active);
	KUNIT_EXPECT_EQ(test, f->t.head[ANE_FW_IO], 0U);
}

static void ane_transport_deadline_service_test(struct kunit *test)
{
	struct ane_transport_test *f = test->priv;

	KUNIT_ASSERT_EQ(test, ane_fw_transport_arm(&f->t), 0);
	KUNIT_ASSERT_EQ(test, ane_test_publish(f, ANE_FW_DATA_CHAIN), 0);
	f->now = TEST_DEADLINE - 1;
	f->alloc_time = 2;
	f->irq = BIT(5);
	ane_test_shared_request(f, 0, SZ_16K, 0);
	KUNIT_EXPECT_EQ(test, ane_fw_transport_wait(&f->t, ANE_FW_DATA_CHAIN), -ETIMEDOUT);
	KUNIT_EXPECT_EQ(test, f->allocs, 1U);
	KUNIT_EXPECT_EQ(test, f->t.shared_count, 1U);
	KUNIT_EXPECT_EQ(test, f->t.pending[ANE_FW_DATA_CHAIN].deadline_ns, TEST_DEADLINE);
	KUNIT_EXPECT_TRUE(test, f->t.pending[ANE_FW_DATA_CHAIN].active);
}

static void ane_transport_wait_test(struct kunit *test)
{
	struct ane_transport_test *f = test->priv;

	KUNIT_ASSERT_EQ(test, ane_fw_transport_arm(&f->t), 0);
	KUNIT_ASSERT_EQ(test, ane_test_publish(f, ANE_FW_IO), 0);
	ane_test_reply(f, ANE_FW_IO);
	KUNIT_EXPECT_EQ(test, ane_fw_transport_wait(&f->t, ANE_FW_IO), 0);
	KUNIT_ASSERT_EQ(test, ane_test_publish(f, ANE_FW_DATA_CHAIN), 0);
	KUNIT_EXPECT_EQ(test, ane_fw_transport_wait(&f->t, ANE_FW_DATA_CHAIN), -ETIMEDOUT);
	KUNIT_EXPECT_EQ(test, f->now, TEST_DEADLINE);
}

static void ane_transport_deny_test(struct kunit *test)
{
	struct ane_transport_test *f = test->priv;
	const u16 ids[] = { 0x27, 0x28, 0x30, 0x31, 0x34 };
	u8 command[12] = {};
	int ret;

	KUNIT_ASSERT_EQ(test, ane_fw_transport_arm(&f->t), 0);
	for (unsigned int i = 0; i < ARRAY_SIZE(ids); i++) {
		put_unaligned_le16(ids[i], command + 4);
		f->count = 0;
		ret = ane_fw_transport_publish(&f->t, ANE_FW_IO, command,
					       sizeof(command), TEST_DEADLINE);
		KUNIT_EXPECT_EQ(test, ret, -EPERM);
		KUNIT_EXPECT_EQ(test, ane_test_writes(f), 0U);
		KUNIT_EXPECT_FALSE(test, f->t.pending[ANE_FW_IO].active);
		KUNIT_EXPECT_EQ(test, f->t.pool_used, (size_t)TEST_RESERVED);
	}
}

static void ane_transport_free_test(struct kunit *test)
{
	struct ane_transport_test *f = test->priv;
	u8 *slot;
	u64 dva;

	KUNIT_ASSERT_EQ(test, ane_fw_transport_arm(&f->t), 0);
	ane_test_shared_request(f, 0, SZ_2M, ANE_FW_SHARED_EXPP);
	KUNIT_ASSERT_EQ(test, ane_fw_transport_shared(&f->t), 0);
	dva = f->t.shared[0].dva;
	KUNIT_EXPECT_EQ(test, f->t.shared_tags[0], ANE_FW_SHARED_EXPP);
	f->t.allow_free = true;
	ane_test_shared_request(f, dva, 0, 0);
	slot = ane_test_slot(f, ANE_FW_SHAREDMALLOC);
	KUNIT_ASSERT_EQ(test, ane_fw_transport_shared(&f->t), 0);
	KUNIT_EXPECT_EQ(test, get_unaligned_le64(slot), dva | 1);
	KUNIT_EXPECT_EQ(test, get_unaligned_le64(slot + 8), 0ULL);
	KUNIT_EXPECT_EQ(test, get_unaligned_le64(slot + 16), 0ULL);
	KUNIT_EXPECT_EQ(test, f->t.free_count, 1U);
	KUNIT_EXPECT_TRUE(test, f->t.shared_retired[0]);
	KUNIT_EXPECT_NOT_NULL(test, f->t.shared[0].cpu);
	KUNIT_EXPECT_PTR_EQ(test, ane_fw_transport_report(&f->t, dva, 8), NULL);
	/* Duplicate FREE cannot acknowledge or retire a different mapping. */
	ane_test_shared_request(f, dva, 0, 0);
	f->count = 0;
	KUNIT_EXPECT_EQ(test, ane_fw_transport_shared(&f->t), -ENOENT);
	KUNIT_EXPECT_EQ(test, ane_test_writes(f), 0U);
}

/*
 * One program lifetime: the firmware asks for an EXPP pool at LOAD, then
 * frees it. With late=true the FREE of each pool comes only after the next
 * program asked for its own, as observed on H17. Returns the DVA served.
 */
static u64 ane_test_pool_cycle(struct ane_transport_test *f, u64 *pending)
{
	struct kunit *test = f->test;
	u64 dva;
	u8 *slot;
	unsigned int i;

	ane_test_shared_request(f, 0, SZ_2M, ANE_FW_SHARED_EXPP);
	slot = ane_test_slot(f, ANE_FW_SHAREDMALLOC);
	KUNIT_ASSERT_EQ(test, ane_fw_transport_shared(&f->t), 0);
	dva = get_unaligned_le64(slot) & ~1ULL;
	for (i = 0; i < f->t.shared_count; i++)
		if (f->t.shared[i].dva == dva)
			break;
	KUNIT_ASSERT_LT(test, i, f->t.shared_count);
	KUNIT_EXPECT_FALSE(test, f->t.shared_retired[i]);
	/* Served memory is zero, whatever the previous program left. */
	KUNIT_EXPECT_FALSE(test, memchr_inv(f->t.shared[i].cpu, 0, SZ_2M));
	memset(f->t.shared[i].cpu, 0xa5, SZ_2M);
	if (pending) {
		if (*pending) {
			ane_test_shared_request(f, *pending, 0, 0);
			KUNIT_ASSERT_EQ(test, ane_fw_transport_shared(&f->t), 0);
		}
		*pending = dva;
	} else {
		ane_test_shared_request(f, dva, 0, 0);
		KUNIT_ASSERT_EQ(test, ane_fw_transport_shared(&f->t), 0);
	}
	return dva;
}

/* Twenty program lifetimes in one boot never run out of shared quota. */
static void ane_transport_pool_reuse_test(struct kunit *test)
{
	struct ane_transport_test *f = test->priv;
	u64 first, pending = 0, dva[2];
	size_t bytes;
	unsigned int i;

	KUNIT_ASSERT_EQ(test, ane_fw_transport_arm(&f->t), 0);
	ane_test_shared_request(f, 0, SZ_64K, 0x4c4f47);
	KUNIT_ASSERT_EQ(test, ane_fw_transport_shared(&f->t), 0);
	f->t.allow_free = true;
	first = ane_test_pool_cycle(f, NULL);
	bytes = f->t.shared_bytes;
	for (i = 1; i < 20; i++)
		KUNIT_EXPECT_EQ(test, ane_test_pool_cycle(f, NULL), first);
	KUNIT_EXPECT_EQ(test, f->t.shared_count, 2U);
	KUNIT_EXPECT_EQ(test, f->t.shared_bytes, bytes);
	KUNIT_EXPECT_EQ(test, f->t.free_count, 20U);
	KUNIT_EXPECT_EQ(test, f->refusals, 0U);
	KUNIT_EXPECT_EQ(test, f->t.error, 0);

	/* Late FREE: two pools alternate, still no growth after the second. */
	for (i = 0; i < 20; i++) {
		dva[i % 2] = ane_test_pool_cycle(f, &pending);
		if (i >= 2)
			KUNIT_EXPECT_NE(test, dva[0], dva[1]);
	}
	KUNIT_EXPECT_EQ(test, f->t.shared_count, 3U);
	KUNIT_EXPECT_EQ(test, f->t.shared_bytes, bytes + SZ_2M);
	KUNIT_EXPECT_EQ(test, f->t.error, 0);

	/* A retired pool never serves another tag or size. */
	ane_test_shared_request(f, pending, 0, 0);
	KUNIT_ASSERT_EQ(test, ane_fw_transport_shared(&f->t), 0);
	ane_test_shared_request(f, 0, SZ_64K, 0x4c4f47);
	KUNIT_ASSERT_EQ(test, ane_fw_transport_shared(&f->t), 0);
	KUNIT_EXPECT_EQ(test, f->t.shared_count, 4U);
}

/* H16 26A428 takes the native FREE reply 1; the pool is reused the same way. */
static void ane_transport_pool_reuse_native_test(struct kunit *test)
{
	struct ane_transport_test *f = test->priv;
	u64 first;
	unsigned int i;

	KUNIT_ASSERT_EQ(test, ane_fw_transport_arm(&f->t), 0);
	f->t.native_free = true;
	first = ane_test_pool_cycle(f, NULL);
	for (i = 1; i < 20; i++)
		KUNIT_EXPECT_EQ(test, ane_test_pool_cycle(f, NULL), first);
	KUNIT_EXPECT_EQ(test, f->t.shared_count, 1U);
	KUNIT_EXPECT_EQ(test, f->t.shared_bytes, (size_t)SZ_2M);
	KUNIT_EXPECT_EQ(test, f->refusals, 0U);
}

/* Without FREE the quota still runs out: nothing the firmware owns is reused. */
static void ane_transport_pool_no_free_test(struct kunit *test)
{
	struct ane_transport_test *f = test->priv;
	unsigned int i;

	KUNIT_ASSERT_EQ(test, ane_fw_transport_arm(&f->t), 0);
	for (i = 0; i < ANE_FW_SHARED_TOTAL / SZ_2M; i++) {
		ane_test_shared_request(f, 0, SZ_2M, ANE_FW_SHARED_EXPP);
		KUNIT_ASSERT_EQ(test, ane_fw_transport_shared(&f->t), 0);
	}
	ane_test_shared_request(f, 0, SZ_2M, ANE_FW_SHARED_EXPP);
	KUNIT_EXPECT_EQ(test, ane_fw_transport_shared(&f->t), -ENOSPC);
}

static void ane_transport_free_refusal_test(struct kunit *test)
{
	struct ane_transport_test *f = test->priv;

	KUNIT_ASSERT_EQ(test, ane_fw_transport_arm(&f->t), 0);
	ane_test_shared_request(f, 0, SZ_16K, 0);
	KUNIT_ASSERT_EQ(test, ane_fw_transport_shared(&f->t), 0);
	ane_test_shared_request(f, f->t.shared[0].dva, 0, 0);
	f->count = 0;
	KUNIT_EXPECT_EQ(test, ane_fw_transport_shared(&f->t), -EOPNOTSUPP);
	KUNIT_EXPECT_FALSE(test, f->t.shared_retired[0]);
	KUNIT_EXPECT_EQ(test, ane_test_writes(f), 0U);
}

/* Publish one EXECUTE on the data channel; return its flags word as copied. */
static u32 ane_test_execute(struct ane_transport_test *f, u32 program, u32 process)
{
	const struct ane_fw_transaction *tx = &f->t.pending[ANE_FW_DATA_CHAIN];
	u8 *command = kunit_kzalloc(f->test, ANE_H16_EXECUTE_BASE_SIZE, GFP_KERNEL);
	u32 flags;
	int ret;

	KUNIT_ASSERT_NOT_NULL(f->test, command);
	put_unaligned_le16(0x20c, command + 4);
	put_unaligned_le32(program, command + 8);
	put_unaligned_le32(process, command + 0xc);
	ret = ane_fw_transport_publish(&f->t, ANE_FW_DATA_CHAIN, command,
				       ANE_H16_EXECUTE_BASE_SIZE, TEST_DEADLINE);
	KUNIT_EXPECT_EQ(f->test, ret, 0);
	flags = get_unaligned_le32((u8 *)f->t.pool.cpu + (tx->dva - f->t.pool.dva) +
				   ANE_H16_EXECUTE_FLAGS);
	/* The caller's request is never written. */
	KUNIT_EXPECT_EQ(f->test, get_unaligned_le32(command + ANE_H16_EXECUTE_FLAGS), 0U);
	ane_test_reply(f, ANE_FW_DATA_CHAIN);
	KUNIT_EXPECT_EQ(f->test, ane_fw_transport_poll(&f->t, ANE_FW_DATA_CHAIN), 0);
	return flags;
}

static void ane_test_io(struct ane_transport_test *f, u16 id, u32 program)
{
	u8 command[0x1c0] = {};
	int ret;

	put_unaligned_le16(id, command + 4);
	put_unaligned_le32(program, command + 8);
	ret = ane_fw_transport_publish(&f->t, ANE_FW_IO, command,
				       id == 0x200 ? sizeof(command) : 16, TEST_DEADLINE);
	KUNIT_EXPECT_EQ(f->test, ret, 0);
	ane_test_reply(f, ANE_FW_IO);
	KUNIT_EXPECT_EQ(f->test, ane_fw_transport_poll(&f->t, ANE_FW_IO), 0);
}

static void ane_transport_second_load_test(struct kunit *test)
{
	struct ane_transport_test *f = test->priv;

	f->soc.chip_family = 17;
	KUNIT_ASSERT_EQ(test, ane_fw_transport_arm(&f->t), 0);
	/* A later program is admitted after UNLOAD within one firmware boot. */
	ane_test_io(f, 0x200, 0);
	ane_test_io(f, 0x201, 0);
	ane_test_io(f, 0x200, 1);
	KUNIT_EXPECT_EQ(test, f->t.error, 0);
}

static void ane_transport_bar_context_test(struct kunit *test)
{
	struct ane_transport_test *f = test->priv;
	const u32 q = ANE_H16_EXECUTE_FLAG_QUEUED;

	f->soc.chip_family = 17;
	KUNIT_ASSERT_EQ(test, ane_fw_transport_arm(&f->t), 0);
	/* Unknown context after start: queue the first job. */
	KUNIT_EXPECT_EQ(test, ane_test_execute(f, 0, 0), q);
	KUNIT_EXPECT_EQ(test, ane_test_execute(f, 0, 0), 0U);
	KUNIT_EXPECT_EQ(test, ane_test_execute(f, 0, 0), 0U);
	/* Switching to another loaded program and back queues each switch. */
	KUNIT_EXPECT_EQ(test, ane_test_execute(f, 1, 1), q);
	KUNIT_EXPECT_EQ(test, ane_test_execute(f, 1, 1), 0U);
	KUNIT_EXPECT_EQ(test, ane_test_execute(f, 0, 0), q);
	/* Same program, different process. */
	KUNIT_EXPECT_EQ(test, ane_test_execute(f, 0, 2), q);
	/* Program lifetime commands forget the context: IDs may be reused. */
	ane_test_io(f, 0x203, 0);
	KUNIT_EXPECT_EQ(test, ane_test_execute(f, 0, 2), q);
	ane_test_io(f, 0x201, 0);
	KUNIT_EXPECT_EQ(test, ane_test_execute(f, 0, 2), q);
	ane_test_io(f, 0x200, 0);
	KUNIT_EXPECT_EQ(test, ane_test_execute(f, 0, 2), q);
	ane_test_io(f, 0x202, 0);
	KUNIT_EXPECT_EQ(test, ane_test_execute(f, 0, 2), q);
	KUNIT_EXPECT_EQ(test, ane_test_execute(f, 0, 2), 0U);
	/* Other IO commands keep it. */
	ane_test_io(f, 3, 0);
	KUNIT_EXPECT_EQ(test, ane_test_execute(f, 0, 2), 0U);
}

static void ane_transport_bar_context_h16_test(struct kunit *test)
{
	struct ane_transport_test *f = test->priv;

	f->soc.chip_family = 16;
	KUNIT_ASSERT_EQ(test, ane_fw_transport_arm(&f->t), 0);
	/* Not yet measured on H16: requests are published unchanged. */
	KUNIT_EXPECT_EQ(test, ane_test_execute(f, 0, 0), 0U);
	KUNIT_EXPECT_EQ(test, ane_test_execute(f, 1, 1), 0U);
}

/* Each relax() learns how long its wait has run, and every call is counted. */
static void ane_transport_wait_polls_test(struct kunit *test)
{
	struct ane_transport_test *f = test->priv;

	KUNIT_ASSERT_EQ(test, ane_fw_transport_arm(&f->t), 0);
	KUNIT_ASSERT_EQ(test, ane_test_publish(f, ANE_FW_IO), 0);
	ane_test_reply(f, ANE_FW_IO);
	KUNIT_EXPECT_EQ(test, ane_fw_transport_wait(&f->t, ANE_FW_IO), 0);
	/* A reply already there: no sleep at all. */
	KUNIT_EXPECT_EQ(test, f->t.polls, 0ULL);
	KUNIT_ASSERT_EQ(test, ane_test_publish(f, ANE_FW_DATA_CHAIN), 0);
	f->now = TEST_DEADLINE - 5;
	KUNIT_EXPECT_EQ(test, ane_fw_transport_wait(&f->t, ANE_FW_DATA_CHAIN), -ETIMEDOUT);
	KUNIT_EXPECT_EQ(test, f->t.polls, 5ULL);
	/* The fifth sleep came four ticks after the wait began. */
	KUNIT_EXPECT_EQ(test, f->waited, 4ULL);
}

static void ane_transport_poll_sleep_test(struct kunit *test)
{
	unsigned int max_us;

	/* A reply within the first 30 us is caught by busy steps, no sleep. */
	KUNIT_EXPECT_EQ(test, ane_fw_poll_sleep_us(0, &max_us), 0U);
	KUNIT_EXPECT_EQ(test, max_us, 0U);
	KUNIT_EXPECT_EQ(test, ane_fw_poll_sleep_us(ANE_FW_POLL_SPIN_NS - 1, &max_us), 0U);
	KUNIT_EXPECT_EQ(test, ane_fw_poll_sleep_us(ANE_FW_POLL_SPIN_NS, &max_us), 20U);
	KUNIT_EXPECT_EQ(test, max_us, 50U);
	KUNIT_EXPECT_EQ(test, ane_fw_poll_sleep_us(ANE_FW_POLL_FINE_NS - 1, &max_us), 20U);
	KUNIT_EXPECT_EQ(test, max_us, 50U);
	KUNIT_EXPECT_EQ(test, ane_fw_poll_sleep_us(ANE_FW_POLL_FINE_NS, &max_us), 250U);
	KUNIT_EXPECT_EQ(test, max_us, 300U);
	KUNIT_EXPECT_EQ(test, ane_fw_poll_sleep_us(ANE_FW_POLL_MEDIUM_NS - 1, &max_us), 250U);
	KUNIT_EXPECT_EQ(test, ane_fw_poll_sleep_us(ANE_FW_POLL_MEDIUM_NS, &max_us), 1000U);
	KUNIT_EXPECT_EQ(test, max_us, 1050U);
	KUNIT_EXPECT_EQ(test, ane_fw_poll_sleep_us(U64_MAX, &max_us), 1000U);
}

static struct kunit_case ane_transport_cases[] = {
	KUNIT_CASE(ane_transport_second_load_test),
	KUNIT_CASE(ane_transport_bar_context_test),
	KUNIT_CASE(ane_transport_bar_context_h16_test),
	KUNIT_CASE(ane_transport_deny_test),
	KUNIT_CASE(ane_transport_free_test),
	KUNIT_CASE(ane_transport_pool_reuse_test),
	KUNIT_CASE(ane_transport_pool_reuse_native_test),
	KUNIT_CASE(ane_transport_pool_no_free_test),
	KUNIT_CASE(ane_transport_free_refusal_test),
	KUNIT_CASE(ane_transport_table_test),
	KUNIT_CASE(ane_transport_table_roles_test),
	KUNIT_CASE(ane_transport_table_bounds_test),
	KUNIT_CASE(ane_transport_table_edges_test),
	KUNIT_CASE(ane_transport_arm_test),
	KUNIT_CASE(ane_transport_publish_order_test),
	KUNIT_CASE(ane_transport_wrap_test),
	KUNIT_CASE(ane_transport_reply_corruption_test),
	KUNIT_CASE(ane_transport_refusal_test),
	KUNIT_CASE(ane_transport_pool_test),
	KUNIT_CASE(ane_transport_snapshot_test),
	KUNIT_CASE(ane_transport_consume_test),
	KUNIT_CASE(ane_transport_report_bounds_test),
	KUNIT_CASE(ane_transport_report_word2_test),
	KUNIT_CASE(ane_transport_presence_test),
	KUNIT_CASE(ane_transport_shared_test),
	KUNIT_CASE(ane_transport_shared_rejections_test),
	KUNIT_CASE(ane_transport_allocator_rejections_test),
	KUNIT_CASE(ane_transport_shared_overlap_test),
	KUNIT_CASE(ane_transport_shared_count_test),
	KUNIT_CASE(ane_transport_shared_total_test),
	KUNIT_CASE(ane_transport_shared_report_test),
	KUNIT_CASE(ane_transport_irq_test),
	KUNIT_CASE(ane_transport_match_data_test),
	KUNIT_CASE(ane_transport_deadline_test),
	KUNIT_CASE(ane_transport_deadline_service_test),
	KUNIT_CASE(ane_transport_wait_test),
	KUNIT_CASE(ane_transport_wait_polls_test),
	KUNIT_CASE(ane_transport_poll_sleep_test),
	{}
};

static struct kunit_suite ane_transport_suite = {
	.name = "ane-fw-transport",
	.init = ane_transport_test_init,
	.test_cases = ane_transport_cases,
};

kunit_test_suite(ane_transport_suite);
MODULE_DESCRIPTION("KUnit tests for the shared H16/H17 firmware transport");
MODULE_LICENSE("Dual MIT/GPL");
