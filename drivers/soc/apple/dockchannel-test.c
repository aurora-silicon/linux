// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * KUnit tests for the Apple DockChannel FIFO.
 *
 * Included at the end of dockchannel.c. The FIFO data and threshold
 * registers are modelled behind static stubs on the register accessors:
 * TX_FREE and RX_COUNT live in a buffer of kernel memory (so that the
 * receive interrupt thread, which the stubs do not redirect, reads the same
 * counts), the bytes written to the TX FIFO are logged and the RX FIFO is a
 * queue of bytes. Writing a threshold register can add room or data and
 * raise the FIFO's interrupt, which is a simulated interrupt handled by the
 * driver's own handlers. The interrupt controller functions run on a buffer
 * of kernel memory. Nothing touches hardware.
 */

#include <kunit/static_stub.h>
#include <kunit/test.h>
#include <linux/atomic.h>
#include <linux/delay.h>
#include <linux/irq_sim.h>
#include <linux/kthread.h>

#define DCT_FIFO_SIZE	16
#define DCT_LOG		256
#define DCT_STEPS	8
#define DCT_GUARD	256
#define DCT_POISON	0xee
/* Past this many model violations, abort the case: a runaway loop. */
#define DCT_RUNAWAY	32
/* No case reads a count register more often than this. */
#define DCT_MAX_POLLS	64

struct dct_ctx {
	struct dockchannel dc;
	u8 *cfg;
	u8 *data;
	u8 *irqregs;

	struct fwnode_handle *fwnode;
	struct irq_domain *domain;
	struct task_struct *worker;
	atomic_t fire_tx;
	atomic_t fire_rx;

	/* TX FIFO: what was written, and how ('W' word, 'B' byte). */
	u8 tx_log[DCT_LOG];
	unsigned int tx_len;
	char tx_ops[DCT_LOG + 1];
	unsigned int tx_nops;
	unsigned int tx_free_reads;
	/* Room added by each TX threshold write; each also raises the IRQ. */
	unsigned int tx_refill[DCT_STEPS];
	unsigned int tx_nrefill;
	unsigned int tx_refill_pos;
	u32 tx_thresh[DCT_STEPS];
	unsigned int tx_nthresh;

	/* RX FIFO: rx_q[rx_pos..rx_len) is queued, RX_COUNT of it visible. */
	u8 rx_q[DCT_LOG];
	unsigned int rx_len;
	unsigned int rx_pos;
	char rx_ops[DCT_LOG + 1];
	unsigned int rx_nops;
	unsigned int rx_count_reads;
	unsigned int rx_refill[DCT_STEPS];
	unsigned int rx_nrefill;
	unsigned int rx_refill_pos;
	u32 rx_thresh[DCT_STEPS];
	unsigned int rx_nthresh;

	/* Accesses the FIFO would not allow (write without room, etc.). */
	unsigned int violations;

	/* Receive callback (runs in the RX interrupt thread). */
	struct completion cb_done;
	atomic_t cb_calls;
	size_t cb_avail;
	void *cb_cookie;
	bool cb_recv;
	u8 cb_buf[4];
	int cb_ret;
};

static u32 dct_reg(struct dct_ctx *ctx, unsigned int off)
{
	return READ_ONCE(*(u32 *)(ctx->data + off));
}

static void dct_set_reg(struct dct_ctx *ctx, unsigned int off, u32 val)
{
	WRITE_ONCE(*(u32 *)(ctx->data + off), val);
}

/* Too many bad accesses: the driver is in a runaway loop, stop the case. */
static void dct_violation(struct dct_ctx *ctx)
{
	if (++ctx->violations > DCT_RUNAWAY)
		KUNIT_FAIL_AND_ABORT(kunit_get_current_test(),
				     "runaway FIFO access loop");
}

static u32 dct_rx_pop(struct dct_ctx *ctx, unsigned int n, char op)
{
	u32 avail = dct_reg(ctx, DATA_RX_COUNT);
	u32 val = 0;
	unsigned int i;

	if (avail < n || ctx->rx_pos + n > ctx->rx_len) {
		dct_violation(ctx);
		return 0xdeadbeef;
	}
	for (i = 0; i < n; i++)
		val |= (u32)ctx->rx_q[ctx->rx_pos++] << (8 * i);
	dct_set_reg(ctx, DATA_RX_COUNT, avail - n);
	if (ctx->rx_nops < DCT_LOG)
		ctx->rx_ops[ctx->rx_nops++] = op;

	return val;
}

static u32 dct_readl(void __iomem *addr)
{
	struct dct_ctx *ctx = kunit_get_current_test()->priv;
	long off = (u8 __force *)addr - ctx->data;

	switch (off) {
	case DATA_TX_FREE:
		if (++ctx->tx_free_reads > DCT_MAX_POLLS)
			dct_violation(ctx);
		return dct_reg(ctx, DATA_TX_FREE);
	case DATA_RX_COUNT:
		if (++ctx->rx_count_reads > DCT_MAX_POLLS)
			dct_violation(ctx);
		return dct_reg(ctx, DATA_RX_COUNT);
	case DATA_RX32:
		return dct_rx_pop(ctx, 4, 'W');
	case DATA_RX8:
		/* The byte comes in bits [15:8]; fill the other bits with junk. */
		return 0x5a0000a5 | (dct_rx_pop(ctx, 1, 'B') & 0xff) << 8;
	default:
		ctx->violations++;
		return 0;
	}
}

static void dct_tx_push(struct dct_ctx *ctx, u32 val, unsigned int n, char op)
{
	u32 room = dct_reg(ctx, DATA_TX_FREE);
	unsigned int i;

	if (room < n || ctx->tx_len + n > DCT_LOG) {
		dct_violation(ctx);
		return;
	}
	for (i = 0; i < n; i++)
		ctx->tx_log[ctx->tx_len++] = val >> (8 * i);
	dct_set_reg(ctx, DATA_TX_FREE, room - n);
	if (ctx->tx_nops < DCT_LOG)
		ctx->tx_ops[ctx->tx_nops++] = op;
}

static void dct_writel(u32 val, void __iomem *addr)
{
	struct dct_ctx *ctx = kunit_get_current_test()->priv;
	u8 *p = (u8 __force *)addr;

	if (p == ctx->cfg + CONFIG_TX_THRESH) {
		if (ctx->tx_nthresh < DCT_STEPS)
			ctx->tx_thresh[ctx->tx_nthresh++] = val;
		if (ctx->tx_refill_pos < ctx->tx_nrefill) {
			dct_set_reg(ctx, DATA_TX_FREE, dct_reg(ctx, DATA_TX_FREE) +
				    ctx->tx_refill[ctx->tx_refill_pos++]);
			atomic_set(&ctx->fire_tx, 1);
		}
	} else if (p == ctx->cfg + CONFIG_RX_THRESH) {
		if (ctx->rx_nthresh < DCT_STEPS)
			ctx->rx_thresh[ctx->rx_nthresh++] = val;
		if (ctx->rx_refill_pos < ctx->rx_nrefill) {
			dct_set_reg(ctx, DATA_RX_COUNT, dct_reg(ctx, DATA_RX_COUNT) +
				    ctx->rx_refill[ctx->rx_refill_pos++]);
			atomic_set(&ctx->fire_rx, 1);
		}
	} else if (p == ctx->data + DATA_TX32) {
		dct_tx_push(ctx, val, 4, 'W');
	} else {
		ctx->violations++;
	}
}

static void dct_writeb(u8 val, void __iomem *addr)
{
	struct dct_ctx *ctx = kunit_get_current_test()->priv;

	if ((u8 __force *)addr == ctx->data + DATA_TX8)
		dct_tx_push(ctx, val, 1, 'B');
	else
		ctx->violations++;
}

/* Raise a FIFO interrupt once the driver has enabled it. */
static void dct_try_fire(atomic_t *fire, unsigned int virq)
{
	struct irq_data *d = irq_get_irq_data(virq);

	if (!atomic_read(fire) || irqd_irq_disabled(d) || irqd_irq_masked(d))
		return;
	atomic_set(fire, 0);
	irq_set_irqchip_state(virq, IRQCHIP_STATE_PENDING, true);
}

static int dct_worker(void *arg)
{
	struct dct_ctx *ctx = arg;

	while (!kthread_should_stop()) {
		dct_try_fire(&ctx->fire_tx, ctx->dc.tx_irq);
		dct_try_fire(&ctx->fire_rx, ctx->dc.rx_irq);
		usleep_range(200, 400);
	}

	return 0;
}

static void dct_fire_now(struct kunit *test, unsigned int virq)
{
	struct irq_data *d = irq_get_irq_data(virq);

	KUNIT_ASSERT_FALSE(test, irqd_irq_disabled(d));
	KUNIT_ASSERT_EQ(test, irq_set_irqchip_state(virq, IRQCHIP_STATE_PENDING, true), 0);
}

static bool dct_irq_enabled(unsigned int virq)
{
	return !irqd_irq_disabled(irq_get_irq_data(virq));
}

static int dct_init(struct kunit *test)
{
	struct dct_ctx *ctx;
	int irq, ret;

	ctx = kunit_kzalloc(test, sizeof(*ctx), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, ctx);
	ctx->cfg = kunit_kzalloc(test, 0x10, GFP_KERNEL);
	ctx->data = kunit_kzalloc(test, 0x40, GFP_KERNEL);
	ctx->irqregs = kunit_kzalloc(test, 0x10, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, ctx->cfg);
	KUNIT_ASSERT_NOT_NULL(test, ctx->data);
	KUNIT_ASSERT_NOT_NULL(test, ctx->irqregs);

	ctx->dc.config_base = (void __force __iomem *)ctx->cfg;
	ctx->dc.data_base = (void __force __iomem *)ctx->data;
	ctx->dc.fifo_size = DCT_FIFO_SIZE;
	init_completion(&ctx->dc.tx_comp);
	init_completion(&ctx->dc.rx_comp);
	init_completion(&ctx->cb_done);
	test->priv = ctx;

	ctx->fwnode = irq_domain_alloc_named_fwnode("dockchannel-kunit");
	KUNIT_ASSERT_NOT_NULL(test, ctx->fwnode);
	ctx->domain = irq_domain_create_sim(ctx->fwnode, 2);
	KUNIT_ASSERT_FALSE(test, IS_ERR(ctx->domain));

	irq = irq_create_mapping(ctx->domain, 0);
	KUNIT_ASSERT_GT(test, irq, 0);
	ctx->dc.tx_irq = irq;
	irq = irq_create_mapping(ctx->domain, 1);
	KUNIT_ASSERT_GT(test, irq, 0);
	ctx->dc.rx_irq = irq;

	/* As dockchannel_init() requests them. */
	ret = request_irq(ctx->dc.tx_irq, dockchannel_tx_irq, IRQF_NO_AUTOEN,
			  "dockchannel-kunit-tx", &ctx->dc);
	KUNIT_ASSERT_EQ(test, ret, 0);
	ret = request_threaded_irq(ctx->dc.rx_irq, dockchannel_rx_irq,
				   dockchannel_rx_irq_thread,
				   IRQF_NO_AUTOEN | IRQF_NO_SUSPEND,
				   "dockchannel-kunit-rx", &ctx->dc);
	if (ret)
		free_irq(ctx->dc.tx_irq, &ctx->dc);
	KUNIT_ASSERT_EQ(test, ret, 0);

	ctx->worker = kthread_run(dct_worker, ctx, "dockchannel-kunit");
	KUNIT_ASSERT_FALSE(test, IS_ERR(ctx->worker));

	kunit_activate_static_stub(test, dockchannel_readl, dct_readl);
	kunit_activate_static_stub(test, dockchannel_writel, dct_writel);
	kunit_activate_static_stub(test, dockchannel_writeb, dct_writeb);

	return 0;
}

static void dct_exit(struct kunit *test)
{
	struct dct_ctx *ctx = test->priv;

	if (!ctx || !ctx->domain || IS_ERR(ctx->domain))
		goto fwnode;

	if (ctx->worker && !IS_ERR(ctx->worker)) {
		kthread_stop(ctx->worker);
		free_irq(ctx->dc.rx_irq, &ctx->dc);
		free_irq(ctx->dc.tx_irq, &ctx->dc);
	}
	if (ctx->dc.rx_irq > 0)
		irq_dispose_mapping(ctx->dc.rx_irq);
	if (ctx->dc.tx_irq > 0)
		irq_dispose_mapping(ctx->dc.tx_irq);
	irq_domain_remove_sim(ctx->domain);
fwnode:
	if (ctx && ctx->fwnode)
		irq_domain_free_fwnode(ctx->fwnode);
}

static void dct_fill(u8 *buf, unsigned int len, u8 seed)
{
	unsigned int i;

	for (i = 0; i < len; i++)
		buf[i] = seed + 7 * i;
}

static void dct_queue_rx(struct dct_ctx *ctx, unsigned int len, unsigned int visible)
{
	dct_fill(ctx->rx_q, len, 0x31);
	ctx->rx_len = len;
	dct_set_reg(ctx, DATA_RX_COUNT, visible);
}

static void dct_expect_guard(struct kunit *test, const u8 *guard, unsigned int len)
{
	unsigned int i, bad = 0;

	for (i = 0; i < len; i++)
		bad += guard[i] != DCT_POISON;
	KUNIT_EXPECT_EQ_MSG(test, bad, 0, "bytes written past the caller's buffer");
}

/* TX */

static void dct_send_whole_words(struct kunit *test)
{
	struct dct_ctx *ctx = test->priv;
	u8 buf[8];

	dct_fill(buf, sizeof(buf), 1);
	dct_set_reg(ctx, DATA_TX_FREE, 64);

	KUNIT_EXPECT_EQ(test, dockchannel_send(&ctx->dc, buf, sizeof(buf)), 8);
	KUNIT_EXPECT_STREQ(test, ctx->tx_ops, "WW");
	KUNIT_ASSERT_EQ(test, ctx->tx_len, 8);
	KUNIT_EXPECT_MEMEQ(test, ctx->tx_log, buf, 8);
	KUNIT_EXPECT_EQ(test, dct_reg(ctx, DATA_TX_FREE), 56);
	KUNIT_EXPECT_EQ(test, ctx->tx_nthresh, 0);
	KUNIT_EXPECT_EQ(test, ctx->violations, 0);
}

static void dct_send_tail_bytes(struct kunit *test)
{
	struct dct_ctx *ctx = test->priv;
	u8 buf[7];

	dct_fill(buf, sizeof(buf), 0x80);
	dct_set_reg(ctx, DATA_TX_FREE, 64);

	KUNIT_EXPECT_EQ(test, dockchannel_send(&ctx->dc, buf, sizeof(buf)), 7);
	/* One little-endian word, then the three tail bytes in order. */
	KUNIT_EXPECT_STREQ(test, ctx->tx_ops, "WBBB");
	KUNIT_ASSERT_EQ(test, ctx->tx_len, 7);
	KUNIT_EXPECT_MEMEQ(test, ctx->tx_log, buf, 7);
	KUNIT_EXPECT_EQ(test, ctx->violations, 0);
}

static void dct_send_short(struct kunit *test)
{
	struct dct_ctx *ctx = test->priv;
	u8 buf[3] = { 0x10, 0x20, 0x30 };

	dct_set_reg(ctx, DATA_TX_FREE, 64);

	KUNIT_EXPECT_EQ(test, dockchannel_send(&ctx->dc, buf, sizeof(buf)), 3);
	KUNIT_EXPECT_STREQ(test, ctx->tx_ops, "BBB");
	KUNIT_ASSERT_EQ(test, ctx->tx_len, 3);
	KUNIT_EXPECT_MEMEQ(test, ctx->tx_log, buf, 3);
}

static void dct_send_unaligned_source(struct kunit *test)
{
	struct dct_ctx *ctx = test->priv;
	u8 buf[16];

	dct_fill(buf, sizeof(buf), 0x42);
	dct_set_reg(ctx, DATA_TX_FREE, 64);

	KUNIT_EXPECT_EQ(test, dockchannel_send(&ctx->dc, buf + 1, 9), 9);
	KUNIT_EXPECT_STREQ(test, ctx->tx_ops, "WWB");
	KUNIT_ASSERT_EQ(test, ctx->tx_len, 9);
	KUNIT_EXPECT_MEMEQ(test, ctx->tx_log, buf + 1, 9);
}

static void dct_send_exact_room(struct kunit *test)
{
	struct dct_ctx *ctx = test->priv;
	u8 buf[10];

	dct_fill(buf, sizeof(buf), 3);
	dct_set_reg(ctx, DATA_TX_FREE, sizeof(buf));

	/* Exactly full: no wait, no threshold write. */
	KUNIT_EXPECT_EQ(test, dockchannel_send(&ctx->dc, buf, sizeof(buf)), 10);
	KUNIT_EXPECT_STREQ(test, ctx->tx_ops, "WWBB");
	KUNIT_EXPECT_MEMEQ(test, ctx->tx_log, buf, 10);
	KUNIT_EXPECT_EQ(test, dct_reg(ctx, DATA_TX_FREE), 0);
	KUNIT_EXPECT_EQ(test, ctx->tx_nthresh, 0);
	KUNIT_EXPECT_EQ(test, ctx->tx_free_reads, 1);
	KUNIT_EXPECT_EQ(test, ctx->violations, 0);
}

static void dct_send_partial_room(struct kunit *test)
{
	struct dct_ctx *ctx = test->priv;
	u8 buf[10];

	dct_fill(buf, sizeof(buf), 9);
	dct_set_reg(ctx, DATA_TX_FREE, 6);
	ctx->tx_refill[0] = 8;
	ctx->tx_nrefill = 1;

	/*
	 * 6 bytes fit (a word and two bytes, never a word into 2 bytes of
	 * room), then the FIFO is full: wait for the 4 that are left.
	 */
	KUNIT_EXPECT_EQ(test, dockchannel_send(&ctx->dc, buf, sizeof(buf)), 10);
	KUNIT_EXPECT_STREQ(test, ctx->tx_ops, "WBBW");
	KUNIT_ASSERT_EQ(test, ctx->tx_len, 10);
	KUNIT_EXPECT_MEMEQ(test, ctx->tx_log, buf, 10);
	KUNIT_ASSERT_EQ(test, ctx->tx_nthresh, 1);
	KUNIT_EXPECT_EQ(test, ctx->tx_thresh[0], 4);
	KUNIT_EXPECT_EQ(test, dct_reg(ctx, DATA_TX_FREE), 4);
	KUNIT_EXPECT_FALSE(test, dct_irq_enabled(ctx->dc.tx_irq));
	KUNIT_EXPECT_EQ(test, ctx->violations, 0);
}

static void dct_send_threshold(struct kunit *test)
{
	struct dct_ctx *ctx = test->priv;
	u8 buf[40];

	dct_fill(buf, sizeof(buf), 0x11);
	ctx->tx_refill[0] = 16;
	ctx->tx_refill[1] = 20;
	ctx->tx_refill[2] = 4;
	ctx->tx_nrefill = 3;

	/* Half the FIFO while more is left, then exactly what is left. */
	KUNIT_EXPECT_EQ(test, dockchannel_send(&ctx->dc, buf, sizeof(buf)), 40);
	KUNIT_ASSERT_EQ(test, ctx->tx_nthresh, 3);
	KUNIT_EXPECT_EQ(test, ctx->tx_thresh[0], DCT_FIFO_SIZE / 2);
	KUNIT_EXPECT_EQ(test, ctx->tx_thresh[1], DCT_FIFO_SIZE / 2);
	KUNIT_EXPECT_EQ(test, ctx->tx_thresh[2], 4);
	KUNIT_ASSERT_EQ(test, ctx->tx_len, 40);
	KUNIT_EXPECT_MEMEQ(test, ctx->tx_log, buf, 40);
	KUNIT_EXPECT_EQ(test, ctx->violations, 0);
}

static void dct_send_byte_room(struct kunit *test)
{
	struct dct_ctx *ctx = test->priv;
	u8 buf[4] = { 0xa1, 0xb2, 0xc3, 0xd4 };

	/* Room for one byte at a time: never a word write. */
	dct_set_reg(ctx, DATA_TX_FREE, 1);
	ctx->tx_refill[0] = 1;
	ctx->tx_refill[1] = 1;
	ctx->tx_refill[2] = 1;
	ctx->tx_nrefill = 3;

	KUNIT_EXPECT_EQ(test, dockchannel_send(&ctx->dc, buf, sizeof(buf)), 4);
	KUNIT_EXPECT_STREQ(test, ctx->tx_ops, "BBBB");
	KUNIT_EXPECT_MEMEQ(test, ctx->tx_log, buf, 4);
	KUNIT_ASSERT_EQ(test, ctx->tx_nthresh, 3);
	KUNIT_EXPECT_EQ(test, ctx->tx_thresh[0], 3);
	KUNIT_EXPECT_EQ(test, ctx->tx_thresh[1], 2);
	KUNIT_EXPECT_EQ(test, ctx->tx_thresh[2], 1);
	KUNIT_EXPECT_EQ(test, ctx->violations, 0);
}

static void dct_send_spurious_wakeup(struct kunit *test)
{
	struct dct_ctx *ctx = test->priv;
	u8 buf[5];

	dct_fill(buf, sizeof(buf), 0x55);
	/* The first interrupt comes with no room: wait again. */
	ctx->tx_refill[0] = 0;
	ctx->tx_refill[1] = 8;
	ctx->tx_nrefill = 2;

	KUNIT_EXPECT_EQ(test, dockchannel_send(&ctx->dc, buf, sizeof(buf)), 5);
	KUNIT_ASSERT_EQ(test, ctx->tx_nthresh, 2);
	KUNIT_EXPECT_EQ(test, ctx->tx_thresh[0], 5);
	KUNIT_EXPECT_EQ(test, ctx->tx_thresh[1], 5);
	KUNIT_EXPECT_STREQ(test, ctx->tx_ops, "WB");
	KUNIT_EXPECT_MEMEQ(test, ctx->tx_log, buf, 5);
	KUNIT_EXPECT_EQ(test, ctx->violations, 0);
}

static void dct_send_timeout(struct kunit *test)
{
	struct dct_ctx *ctx = test->priv;
	u8 buf[4] = { 1, 2, 3, 4 };

	/* Nothing ever drains the FIFO. */
	KUNIT_EXPECT_EQ(test, dockchannel_send(&ctx->dc, buf, sizeof(buf)), -ETIMEDOUT);
	KUNIT_EXPECT_EQ(test, ctx->tx_len, 0);
	KUNIT_ASSERT_EQ(test, ctx->tx_nthresh, 1);
	KUNIT_EXPECT_EQ(test, ctx->tx_thresh[0], 4);
	KUNIT_EXPECT_FALSE(test, dct_irq_enabled(ctx->dc.tx_irq));
	KUNIT_EXPECT_EQ(test, ctx->violations, 0);
}

static void dct_send_zero(struct kunit *test)
{
	struct dct_ctx *ctx = test->priv;
	u8 buf[1] = { 0 };

	dct_set_reg(ctx, DATA_TX_FREE, 64);
	KUNIT_EXPECT_EQ(test, dockchannel_send(&ctx->dc, buf, 0), 0);
	KUNIT_EXPECT_EQ(test, ctx->tx_free_reads, 0);
	KUNIT_EXPECT_EQ(test, ctx->tx_len, 0);
	KUNIT_EXPECT_EQ(test, ctx->tx_nthresh, 0);
}

/* RX */

static void dct_recv_words_and_tail(struct kunit *test)
{
	struct dct_ctx *ctx = test->priv;
	u8 *buf = kunit_kmalloc(test, 7 + DCT_GUARD, GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, buf);
	memset(buf, DCT_POISON, 7 + DCT_GUARD);
	dct_queue_rx(ctx, 7, 7);

	KUNIT_EXPECT_EQ(test, dockchannel_recv(&ctx->dc, buf, 7), 7);
	KUNIT_EXPECT_STREQ(test, ctx->rx_ops, "WBBB");
	/* Word unpacked little-endian, tail bytes taken from bits [15:8]. */
	KUNIT_EXPECT_MEMEQ(test, buf, ctx->rx_q, 7);
	dct_expect_guard(test, buf + 7, DCT_GUARD);
	KUNIT_EXPECT_EQ(test, dct_reg(ctx, DATA_RX_COUNT), 0);
	KUNIT_EXPECT_EQ(test, ctx->rx_nthresh, 0);
	KUNIT_EXPECT_EQ(test, ctx->violations, 0);
}

static void dct_recv_count_exceeds_buffer(struct kunit *test)
{
	struct dct_ctx *ctx = test->priv;
	u8 *buf = kunit_kmalloc(test, 5 + DCT_GUARD, GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, buf);
	memset(buf, DCT_POISON, 5 + DCT_GUARD);
	dct_queue_rx(ctx, 64, 64);

	/* 64 bytes waiting, room for 5: take exactly 5, leave 59. */
	KUNIT_EXPECT_EQ(test, dockchannel_recv(&ctx->dc, buf, 5), 5);
	KUNIT_EXPECT_STREQ(test, ctx->rx_ops, "WB");
	KUNIT_EXPECT_MEMEQ(test, buf, ctx->rx_q, 5);
	dct_expect_guard(test, buf + 5, DCT_GUARD);
	KUNIT_EXPECT_EQ(test, ctx->rx_pos, 5);
	KUNIT_EXPECT_EQ(test, dct_reg(ctx, DATA_RX_COUNT), 59);
	KUNIT_EXPECT_EQ(test, ctx->violations, 0);
}

static void dct_recv_all_ones_count(struct kunit *test)
{
	struct dct_ctx *ctx = test->priv;
	u8 *buf = kunit_kmalloc(test, 6 + DCT_GUARD, GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, buf);
	memset(buf, DCT_POISON, 6 + DCT_GUARD);
	/* A count register that reads all ones must not size the copy. */
	dct_queue_rx(ctx, 8, 0xffffffff);

	KUNIT_EXPECT_EQ(test, dockchannel_recv(&ctx->dc, buf, 6), 6);
	KUNIT_EXPECT_MEMEQ(test, buf, ctx->rx_q, 6);
	dct_expect_guard(test, buf + 6, DCT_GUARD);
	KUNIT_EXPECT_EQ(test, ctx->rx_pos, 6);
	KUNIT_EXPECT_EQ(test, ctx->violations, 0);
}

static void dct_recv_unaligned_dest(struct kunit *test)
{
	struct dct_ctx *ctx = test->priv;
	u8 *buf = kunit_kmalloc(test, 3 + 9 + DCT_GUARD, GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, buf);
	memset(buf, DCT_POISON, 3 + 9 + DCT_GUARD);
	dct_queue_rx(ctx, 9, 9);

	KUNIT_EXPECT_EQ(test, dockchannel_recv(&ctx->dc, buf + 3, 9), 9);
	KUNIT_EXPECT_STREQ(test, ctx->rx_ops, "WWB");
	dct_expect_guard(test, buf, 3);
	KUNIT_EXPECT_MEMEQ(test, buf + 3, ctx->rx_q, 9);
	dct_expect_guard(test, buf + 12, DCT_GUARD);
}

static void dct_recv_partial_waits(struct kunit *test)
{
	struct dct_ctx *ctx = test->priv;
	u8 *buf = kunit_kmalloc(test, 9 + DCT_GUARD, GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, buf);
	memset(buf, DCT_POISON, 9 + DCT_GUARD);
	/* 3 of 9 bytes there; the rest comes with the threshold interrupt. */
	dct_queue_rx(ctx, 9, 3);
	ctx->rx_refill[0] = 6;
	ctx->rx_nrefill = 1;

	KUNIT_EXPECT_EQ(test, dockchannel_recv(&ctx->dc, buf, 9), 9);
	KUNIT_EXPECT_STREQ(test, ctx->rx_ops, "BBBWBB");
	KUNIT_EXPECT_MEMEQ(test, buf, ctx->rx_q, 9);
	dct_expect_guard(test, buf + 9, DCT_GUARD);
	KUNIT_ASSERT_EQ(test, ctx->rx_nthresh, 1);
	KUNIT_EXPECT_EQ(test, ctx->rx_thresh[0], 6);
	KUNIT_EXPECT_FALSE(test, dct_irq_enabled(ctx->dc.rx_irq));
	KUNIT_EXPECT_EQ(test, ctx->violations, 0);
}

static void dct_recv_threshold(struct kunit *test)
{
	struct dct_ctx *ctx = test->priv;
	u8 *buf = kunit_kmalloc(test, 30 + DCT_GUARD, GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, buf);
	memset(buf, DCT_POISON, 30 + DCT_GUARD);
	dct_queue_rx(ctx, 30, 0);
	ctx->rx_refill[0] = 12;
	ctx->rx_refill[1] = 18;
	ctx->rx_nrefill = 2;

	KUNIT_EXPECT_EQ(test, dockchannel_recv(&ctx->dc, buf, 30), 30);
	KUNIT_ASSERT_EQ(test, ctx->rx_nthresh, 2);
	KUNIT_EXPECT_EQ(test, ctx->rx_thresh[0], DCT_FIFO_SIZE / 2);
	KUNIT_EXPECT_EQ(test, ctx->rx_thresh[1], DCT_FIFO_SIZE / 2);
	KUNIT_EXPECT_MEMEQ(test, buf, ctx->rx_q, 30);
	dct_expect_guard(test, buf + 30, DCT_GUARD);
	KUNIT_EXPECT_EQ(test, ctx->violations, 0);
}

static void dct_recv_timeout(struct kunit *test)
{
	struct dct_ctx *ctx = test->priv;
	u8 *buf = kunit_kmalloc(test, 4 + DCT_GUARD, GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, buf);
	memset(buf, DCT_POISON, 4 + DCT_GUARD);

	KUNIT_EXPECT_EQ(test, dockchannel_recv(&ctx->dc, buf, 4), -ETIMEDOUT);
	dct_expect_guard(test, buf, 4 + DCT_GUARD);
	KUNIT_ASSERT_EQ(test, ctx->rx_nthresh, 1);
	KUNIT_EXPECT_EQ(test, ctx->rx_thresh[0], 4);
	KUNIT_EXPECT_FALSE(test, dct_irq_enabled(ctx->dc.rx_irq));
	KUNIT_EXPECT_EQ(test, ctx->violations, 0);
}

static void dct_recv_zero(struct kunit *test)
{
	struct dct_ctx *ctx = test->priv;
	u8 buf[1] = { DCT_POISON };

	dct_queue_rx(ctx, 8, 8);
	KUNIT_EXPECT_EQ(test, dockchannel_recv(&ctx->dc, buf, 0), 0);
	KUNIT_EXPECT_EQ(test, ctx->rx_count_reads, 0);
	KUNIT_EXPECT_EQ(test, ctx->rx_pos, 0);
	KUNIT_EXPECT_EQ(test, buf[0], DCT_POISON);
}

/* Await */

static void dct_callback(void *cookie, size_t avail)
{
	struct dct_ctx *ctx = cookie;

	ctx->cb_cookie = cookie;
	ctx->cb_avail = avail;
	if (ctx->cb_recv)
		ctx->cb_ret = dockchannel_recv(&ctx->dc, ctx->cb_buf, sizeof(ctx->cb_buf));
	atomic_inc(&ctx->cb_calls);
	complete(&ctx->cb_done);
}

static void dct_await_threshold(struct kunit *test)
{
	struct dct_ctx *ctx = test->priv;

	KUNIT_EXPECT_EQ(test, dockchannel_await(&ctx->dc, dct_callback, ctx, 4), 4);
	KUNIT_ASSERT_EQ(test, ctx->rx_nthresh, 1);
	KUNIT_EXPECT_EQ(test, ctx->rx_thresh[0], 4);
	KUNIT_EXPECT_TRUE(test, ctx->dc.awaiting);
	KUNIT_EXPECT_TRUE(test, dct_irq_enabled(ctx->dc.rx_irq));

	/* A zero count disarms. */
	KUNIT_EXPECT_EQ(test, dockchannel_await(&ctx->dc, NULL, NULL, 0), 0);
	KUNIT_EXPECT_FALSE(test, ctx->dc.awaiting);
	KUNIT_EXPECT_FALSE(test, dct_irq_enabled(ctx->dc.rx_irq));
	KUNIT_EXPECT_EQ(test, ctx->rx_nthresh, 1);
	KUNIT_EXPECT_EQ(test, atomic_read(&ctx->cb_calls), 0);
}

static void dct_await_clamped(struct kunit *test)
{
	struct dct_ctx *ctx = test->priv;

	/* A threshold above the FIFO size would never be reached. */
	KUNIT_EXPECT_EQ(test, dockchannel_await(&ctx->dc, dct_callback, ctx, 100),
			DCT_FIFO_SIZE);
	KUNIT_ASSERT_EQ(test, ctx->rx_nthresh, 1);
	KUNIT_EXPECT_EQ(test, ctx->rx_thresh[0], DCT_FIFO_SIZE);
	KUNIT_EXPECT_EQ(test, dockchannel_await(&ctx->dc, NULL, NULL, 0), 0);
}

static void dct_await_callback(struct kunit *test)
{
	struct dct_ctx *ctx = test->priv;

	KUNIT_ASSERT_EQ(test, dockchannel_await(&ctx->dc, dct_callback, ctx, 4), 4);
	dct_set_reg(ctx, DATA_RX_COUNT, 9);
	dct_fire_now(test, ctx->dc.rx_irq);

	KUNIT_ASSERT_TRUE(test, wait_for_completion_timeout(&ctx->cb_done,
							    msecs_to_jiffies(2000)));
	KUNIT_EXPECT_EQ(test, atomic_read(&ctx->cb_calls), 1);
	KUNIT_EXPECT_PTR_EQ(test, ctx->cb_cookie, (void *)ctx);
	/* The callback is told what RX_COUNT says, and is not re-armed. */
	KUNIT_EXPECT_EQ(test, ctx->cb_avail, 9);
	KUNIT_EXPECT_FALSE(test, ctx->dc.awaiting);
	KUNIT_EXPECT_FALSE(test, dct_irq_enabled(ctx->dc.rx_irq));
}

static void dct_await_recv_in_callback(struct kunit *test)
{
	struct dct_ctx *ctx = test->priv;
	static const u8 want[4] = { 0x78, 0x56, 0x34, 0x12 };

	/*
	 * The interrupt thread is not the test thread, so the stubs do not
	 * apply there: it reads the register buffer as it is.
	 */
	dct_set_reg(ctx, DATA_RX32, 0x12345678);
	ctx->cb_recv = true;
	KUNIT_ASSERT_EQ(test, dockchannel_await(&ctx->dc, dct_callback, ctx, 4), 4);
	dct_set_reg(ctx, DATA_RX_COUNT, 4);
	dct_fire_now(test, ctx->dc.rx_irq);

	KUNIT_ASSERT_TRUE(test, wait_for_completion_timeout(&ctx->cb_done,
							    msecs_to_jiffies(2000)));
	KUNIT_EXPECT_EQ(test, ctx->cb_ret, 4);
	KUNIT_EXPECT_MEMEQ(test, ctx->cb_buf, want, 4);
}

static void dct_await_rearm(struct kunit *test)
{
	struct dct_ctx *ctx = test->priv;
	int i;

	/* As the HID client does: every callback arms the next one. */
	for (i = 1; i <= 3; i++) {
		reinit_completion(&ctx->cb_done);
		KUNIT_ASSERT_EQ(test, dockchannel_await(&ctx->dc, dct_callback, ctx, 8), 8);
		KUNIT_ASSERT_TRUE(test, dct_irq_enabled(ctx->dc.rx_irq));
		dct_set_reg(ctx, DATA_RX_COUNT, 8 + i);
		dct_fire_now(test, ctx->dc.rx_irq);
		KUNIT_ASSERT_TRUE(test,
				  wait_for_completion_timeout(&ctx->cb_done,
							      msecs_to_jiffies(2000)));
		KUNIT_EXPECT_EQ(test, atomic_read(&ctx->cb_calls), i);
		KUNIT_EXPECT_EQ(test, ctx->cb_avail, 8 + i);
	}
	KUNIT_EXPECT_EQ(test, ctx->rx_nthresh, 3);
}

static void dct_recv_after_await(struct kunit *test)
{
	struct dct_ctx *ctx = test->priv;
	u8 buf[4];

	/*
	 * Once disarmed, the receive interrupt wakes a waiting
	 * dockchannel_recv() and no longer calls the callback.
	 */
	KUNIT_ASSERT_EQ(test, dockchannel_await(&ctx->dc, dct_callback, ctx, 4), 4);
	KUNIT_ASSERT_EQ(test, dockchannel_await(&ctx->dc, NULL, NULL, 0), 0);

	dct_queue_rx(ctx, 4, 0);
	ctx->rx_refill[0] = 4;
	ctx->rx_nrefill = 1;
	KUNIT_EXPECT_EQ(test, dockchannel_recv(&ctx->dc, buf, 4), 4);
	KUNIT_EXPECT_MEMEQ(test, buf, ctx->rx_q, 4);
	KUNIT_EXPECT_EQ(test, atomic_read(&ctx->cb_calls), 0);
}

/* Interrupt controller */

static void dct_irqchip_mask_unmask(struct kunit *test)
{
	struct dct_ctx *ctx = test->priv;
	struct dockchannel_common dcc = {
		.irq_base = (void __force __iomem *)ctx->irqregs,
	};
	struct irq_data d = { .chip_data = &dcc };
	u32 *mask = (u32 *)(ctx->irqregs + IRQ_MASK);
	u32 *flag = (u32 *)(ctx->irqregs + IRQ_FLAG);

	WRITE_ONCE(*mask, 0xa5a5a5a5);
	WRITE_ONCE(*flag, 0x12345678);

	d.hwirq = 0;
	dockchannel_irq_mask(&d);
	KUNIT_EXPECT_EQ(test, READ_ONCE(*mask), 0xa5a5a5a4);
	d.hwirq = 1;
	dockchannel_irq_unmask(&d);
	KUNIT_EXPECT_EQ(test, READ_ONCE(*mask), 0xa5a5a5a6);
	d.hwirq = 31;
	dockchannel_irq_mask(&d);
	KUNIT_EXPECT_EQ(test, READ_ONCE(*mask), 0x25a5a5a6);
	dockchannel_irq_unmask(&d);
	KUNIT_EXPECT_EQ(test, READ_ONCE(*mask), 0xa5a5a5a6);
	/* Unmasking an unmasked line changes nothing. */
	dockchannel_irq_unmask(&d);
	KUNIT_EXPECT_EQ(test, READ_ONCE(*mask), 0xa5a5a5a6);
	KUNIT_EXPECT_EQ(test, READ_ONCE(*flag), 0x12345678);
}

static void dct_irqchip_ack(struct kunit *test)
{
	struct dct_ctx *ctx = test->priv;
	struct dockchannel_common dcc = {
		.irq_base = (void __force __iomem *)ctx->irqregs,
	};
	struct irq_data d = { .chip_data = &dcc, .hwirq = 5 };
	u32 *mask = (u32 *)(ctx->irqregs + IRQ_MASK);
	u32 *flag = (u32 *)(ctx->irqregs + IRQ_FLAG);

	WRITE_ONCE(*mask, 0xffffffff);
	WRITE_ONCE(*flag, 0);

	/* IRQ_FLAG is write-one-to-clear: write only this line's bit. */
	dockchannel_irq_ack(&d);
	KUNIT_EXPECT_EQ(test, READ_ONCE(*flag), BIT(5));
	d.hwirq = 31;
	dockchannel_irq_ack(&d);
	KUNIT_EXPECT_EQ(test, READ_ONCE(*flag), BIT(31));
	KUNIT_EXPECT_EQ(test, READ_ONCE(*mask), 0xffffffff);
}

static struct kunit_case dockchannel_test_cases[] = {
	KUNIT_CASE(dct_send_whole_words),
	KUNIT_CASE(dct_send_tail_bytes),
	KUNIT_CASE(dct_send_short),
	KUNIT_CASE(dct_send_unaligned_source),
	KUNIT_CASE(dct_send_exact_room),
	KUNIT_CASE(dct_send_partial_room),
	KUNIT_CASE(dct_send_threshold),
	KUNIT_CASE(dct_send_byte_room),
	KUNIT_CASE(dct_send_spurious_wakeup),
	KUNIT_CASE(dct_send_timeout),
	KUNIT_CASE(dct_send_zero),
	KUNIT_CASE(dct_recv_words_and_tail),
	KUNIT_CASE(dct_recv_count_exceeds_buffer),
	KUNIT_CASE(dct_recv_all_ones_count),
	KUNIT_CASE(dct_recv_unaligned_dest),
	KUNIT_CASE(dct_recv_partial_waits),
	KUNIT_CASE(dct_recv_threshold),
	KUNIT_CASE(dct_recv_timeout),
	KUNIT_CASE(dct_recv_zero),
	KUNIT_CASE(dct_await_threshold),
	KUNIT_CASE(dct_await_clamped),
	KUNIT_CASE(dct_await_callback),
	KUNIT_CASE(dct_await_recv_in_callback),
	KUNIT_CASE(dct_await_rearm),
	KUNIT_CASE(dct_recv_after_await),
	KUNIT_CASE(dct_irqchip_mask_unmask),
	KUNIT_CASE(dct_irqchip_ack),
	{}
};

static struct kunit_suite dockchannel_test_suite = {
	.name = "apple-dockchannel",
	.init = dct_init,
	.exit = dct_exit,
	.test_cases = dockchannel_test_cases,
};

kunit_test_suite(dockchannel_test_suite);
