// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * Included by t6050-aop.c: an endpoint's buffer is the firmware's first GETBUF's, for as long as
 * the firmware runs; a later GETBUF that fits in it gets it again.
 */
#include <kunit/test.h>

static void t6050_aop_test_first_getbuf_allocates(struct kunit *test)
{
	struct t6050_aop_ring r = {};

	KUNIT_EXPECT_EQ(test, t6050_aop_getbuf_claim(&r, 0x4000), 0);
	KUNIT_EXPECT_EQ(test, t6050_aop_getbuf_claim(&r, 0xffffUL << AFK_BLOCK_SHIFT), 0);
}

static void t6050_aop_test_empty_getbuf_refused(struct kunit *test)
{
	static u8 mem[0x100];
	struct t6050_aop_ring r = {};

	KUNIT_EXPECT_LT(test, t6050_aop_getbuf_claim(&r, 0), 0);
	r.buf = mem;
	r.size = sizeof(mem);
	KUNIT_EXPECT_LT(test, t6050_aop_getbuf_claim(&r, 0), 0);
}

static void t6050_aop_test_repeat_getbuf_reuses(struct kunit *test)
{
	static u8 mem[0x100];
	struct t6050_aop_ring r = { .buf = mem, .size = sizeof(mem), .iova = 0x1000 };
	int i;

	for (i = 0; i < 64; i++)
		KUNIT_EXPECT_EQ(test, t6050_aop_getbuf_claim(&r, sizeof(mem)), 1);
}

static void t6050_aop_test_smaller_getbuf_reuses(struct kunit *test)
{
	static u8 mem[0x100];
	struct t6050_aop_ring r = { .buf = mem, .size = sizeof(mem), .iova = 0x1000 };

	KUNIT_EXPECT_EQ(test, t6050_aop_getbuf_claim(&r, sizeof(mem) / 2), 1);
	KUNIT_EXPECT_EQ(test, t6050_aop_getbuf_claim(&r, 1), 1);
}

static void t6050_aop_test_reused_getbuf_restarts_rings(struct kunit *test)
{
	static __le32 mem[0x400 / 4];
	struct t6050_aop_ring r = { .buf = mem, .size = sizeof(mem), .iova = 0x1000 };
	struct t6050_aop_view v;
	u32 bufsz = 2 * AFK_QE_SLOT;

	mem[0] = cpu_to_le32(bufsz);
	r.tx_off = 0;
	r.tx_size = bufsz + 3 * 0x40;
	r.tx_ready = true;
	r.rx_off = 0;
	r.rx_size = r.tx_size;
	r.rx_ready = true;
	r.seq = 5;
	KUNIT_EXPECT_EQ(test, t6050_aop2_view(&r, r.tx_off, r.tx_size, r.tx_ready, &v), 0);

	/* The firmware asks again: no ring until it announces them again. */
	KUNIT_EXPECT_EQ(test, t6050_aop_getbuf_claim(&r, sizeof(mem)), 1);
	KUNIT_EXPECT_FALSE(test, r.tx_ready);
	KUNIT_EXPECT_FALSE(test, r.rx_ready);
	KUNIT_EXPECT_EQ(test, r.seq, 0);
	KUNIT_EXPECT_EQ(test, t6050_aop2_view(&r, r.tx_off, r.tx_size, r.tx_ready, &v), -EINVAL);
	KUNIT_EXPECT_EQ(test, t6050_aop2_view(&r, r.rx_off, r.rx_size, r.rx_ready, &v), -EINVAL);
}

static void t6050_aop_test_refused_getbuf_keeps_rings(struct kunit *test)
{
	static u8 mem[0x100];
	struct t6050_aop_ring r = { .buf = mem, .size = sizeof(mem), .iova = 0x1000,
				    .tx_ready = true, .rx_ready = true, .seq = 5 };

	KUNIT_EXPECT_LT(test, t6050_aop_getbuf_claim(&r, sizeof(mem) + 1), 0);
	KUNIT_EXPECT_TRUE(test, r.tx_ready);
	KUNIT_EXPECT_TRUE(test, r.rx_ready);
	KUNIT_EXPECT_EQ(test, r.seq, 5);
}

static void t6050_aop_test_larger_getbuf_refused(struct kunit *test)
{
	static u8 mem[0x100];
	struct t6050_aop_ring r = { .buf = mem, .size = sizeof(mem), .iova = 0x1000 };

	KUNIT_EXPECT_LT(test, t6050_aop_getbuf_claim(&r, sizeof(mem) + 1), 0);
	KUNIT_EXPECT_LT(test, t6050_aop_getbuf_claim(&r, sizeof(mem) * 2), 0);
	KUNIT_EXPECT_LT(test, t6050_aop_getbuf_claim(&r, 0xffffUL << AFK_BLOCK_SHIFT), 0);
}

static void t6050_aop_test_view_inside_buffer(struct kunit *test)
{
	static __le32 mem[0x400 / 4];
	struct t6050_aop_ring r = {};
	struct t6050_aop_view v;
	u32 bufsz = 2 * AFK_QE_SLOT;
	u32 size = bufsz + 3 * 0x40;

	mem[0] = cpu_to_le32(bufsz);
	/* No buffer yet. */
	KUNIT_EXPECT_EQ(test, t6050_aop2_view(&r, 0, size, true, &v), -EINVAL);
	r.buf = mem;
	r.size = sizeof(mem);
	KUNIT_EXPECT_EQ(test, t6050_aop2_view(&r, 0, size, true, &v), 0);
	KUNIT_EXPECT_PTR_EQ(test, (void *)v.hdr, (void *)mem);
	/* A ring that does not fit the buffer. */
	KUNIT_EXPECT_EQ(test, t6050_aop2_view(&r, 0x400 - 0x40, size, true, &v), -EINVAL);
	KUNIT_EXPECT_EQ(test, t6050_aop2_view(&r, 0x440, 0x40, true, &v), -EINVAL);
}

static struct kunit_case t6050_aop_test_cases[] = {
	KUNIT_CASE(t6050_aop_test_first_getbuf_allocates),
	KUNIT_CASE(t6050_aop_test_empty_getbuf_refused),
	KUNIT_CASE(t6050_aop_test_repeat_getbuf_reuses),
	KUNIT_CASE(t6050_aop_test_smaller_getbuf_reuses),
	KUNIT_CASE(t6050_aop_test_larger_getbuf_refused),
	KUNIT_CASE(t6050_aop_test_reused_getbuf_restarts_rings),
	KUNIT_CASE(t6050_aop_test_refused_getbuf_keeps_rings),
	KUNIT_CASE(t6050_aop_test_view_inside_buffer),
	{}
};

static struct kunit_suite t6050_aop_test_suite = {
	.name = "apple_t6050_aop",
	.test_cases = t6050_aop_test_cases,
};

kunit_test_suite(t6050_aop_test_suite);
