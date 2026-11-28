// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * KUnit tests for the Apple DockChannel UART.
 *
 * Included at the end of apple_dockchannel_uart.c. The port is built by the
 * test and never registered. The data FIFO accesses of the RX drain and TX
 * paths go to a model behind static stubs; the interrupt and config
 * registers, and the data registers as the console helpers in
 * apple_dockchannel.h see them, are buffers of kernel memory. Received bytes
 * are read back from the port's flip buffers. Nothing touches hardware.
 */

#include <kunit/static_stub.h>
#include <kunit/test.h>
#include <linux/irq_sim.h>

#define DCUT_CHANNEL	3
#define DCUT_OTHERS	(BIT(0) | BIT(1) | BIT(9) | BIT(31))
#define DCUT_LOG	4096
#define DCUT_STEPS	8
#define DCUT_POISON	0xee
#define DCUT_RUNAWAY	32
#define DCUT_CON	(MAX_PORTS - 1)

struct dcut_ctx {
	struct dcuart_port dcuart;
	struct uart_state *state;
	u8 *data;
	u8 *cfg;
	u8 *irqregs;

	struct fwnode_handle *fwnode;
	struct irq_domain *domain;
	int virq;
	bool irq_requested;
	bool con_slot;

	/* RX FIFO: rx_q[rx_pos..rx_len), rx_visible of it in RX_COUNT. */
	u8 rx_q[DCUT_LOG];
	unsigned int rx_len;
	unsigned int rx_pos;
	unsigned int rx_visible;
	/* Bytes that arrive each time RX_COUNT is read while it is 0. */
	unsigned int rx_chunk[DCUT_STEPS];
	unsigned int rx_nchunk;
	unsigned int rx_chunk_pos;
	u32 rx_counts[DCUT_STEPS];
	unsigned int rx_ncounts;

	/* TX FIFO */
	unsigned int tx_room;
	u8 tx_log[DCUT_LOG];
	unsigned int tx_len;
	unsigned int tx_free_reads;

	unsigned int violations;
};

static u32 *dcut_reg(u8 *base, unsigned int off)
{
	return (u32 *)(base + off);
}

/*
 * The stubs are active for the test task, so an interrupt that runs a real
 * DockChannel port on top of it lands here too: give that port its own
 * registers, as the driver would.
 */
static u32 dcut_rx_readl(struct dcuart_port *dcuart, u32 reg)
{
	struct dcut_ctx *ctx = kunit_get_current_test()->priv;
	u8 c;

	if (dcuart != &ctx->dcuart)
		return readl_relaxed(dcuart->port.membase + reg);

	switch (reg) {
	case DATA_RX_COUNT:
		if (!ctx->rx_visible && ctx->rx_chunk_pos < ctx->rx_nchunk)
			ctx->rx_visible = ctx->rx_chunk[ctx->rx_chunk_pos++];
		if (ctx->violations > DCUT_RUNAWAY)
			ctx->rx_visible = 0;
		if (ctx->rx_ncounts < DCUT_STEPS)
			ctx->rx_counts[ctx->rx_ncounts++] = ctx->rx_visible;
		return ctx->rx_visible;
	case DATA_RX8:
		if (!ctx->rx_visible || ctx->rx_pos >= ctx->rx_len) {
			ctx->violations++;
			return 0xdeadbeef;
		}
		ctx->rx_visible--;
		c = ctx->rx_q[ctx->rx_pos++];
		/* The byte comes in bits [15:8]; fill the other bits with junk. */
		return 0x5a0000a5 | (u32)c << 8;
	default:
		ctx->violations++;
		return 0;
	}
}

static u32 dcut_tx_free(struct dcuart_port *dcuart)
{
	struct dcut_ctx *ctx = kunit_get_current_test()->priv;

	if (dcuart != &ctx->dcuart)
		return readl(dcuart->port.membase + DATA_TX_FREE);

	ctx->tx_free_reads++;
	return ctx->tx_room;
}

static void dcut_tx_put(struct dcuart_port *dcuart, u8 ch)
{
	struct dcut_ctx *ctx = kunit_get_current_test()->priv;

	if (dcuart != &ctx->dcuart) {
		writel(ch, dcuart->port.membase + DATA_TX8);
		return;
	}

	if (!ctx->tx_room || ctx->tx_len >= DCUT_LOG) {
		ctx->violations++;
		return;
	}
	ctx->tx_room--;
	ctx->tx_log[ctx->tx_len++] = ch;
}

static int dcut_init(struct kunit *test)
{
	struct dcut_ctx *ctx;
	struct uart_port *port;

	ctx = kunit_kzalloc(test, sizeof(*ctx), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, ctx);
	ctx->data = kunit_kzalloc(test, 0x40, GFP_KERNEL);
	ctx->cfg = kunit_kzalloc(test, 0x10, GFP_KERNEL);
	ctx->irqregs = kunit_kzalloc(test, 0x10, GFP_KERNEL);
	ctx->state = kunit_kzalloc(test, sizeof(*ctx->state), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, ctx->data);
	KUNIT_ASSERT_NOT_NULL(test, ctx->cfg);
	KUNIT_ASSERT_NOT_NULL(test, ctx->irqregs);
	KUNIT_ASSERT_NOT_NULL(test, ctx->state);
	test->priv = ctx;

	tty_port_init(&ctx->state->port);
	KUNIT_ASSERT_EQ(test, kfifo_alloc(&ctx->state->port.xmit_fifo, PAGE_SIZE,
					  GFP_KERNEL), 0);

	/* As dcuart_probe() sets the port up. */
	port = &ctx->dcuart.port;
	port->ops = &dcuart_ops;
	port->fifosize = FIFO_SIZE;
	port->iotype = UPIO_MEM32;
	port->type = PORT_GENERIC;
	port->membase = (void __force __iomem *)ctx->data;
	port->state = ctx->state;
	ctx->state->uart_port = port;
	spin_lock_init(&port->lock);
	spin_lock_init(&ctx->dcuart.lock);
	ctx->dcuart.irq_base = (void __force __iomem *)ctx->irqregs;
	ctx->dcuart.config_base = (void __force __iomem *)ctx->cfg;
	ctx->dcuart.irq_tx = IRQ_TX(DCUT_CHANNEL);
	ctx->dcuart.irq_rx = IRQ_RX(DCUT_CHANNEL);
	timer_setup(&ctx->dcuart.drain_timer, dcuart_drain_timer, 0);

	kunit_activate_static_stub(test, dcuart_rx_readl, dcut_rx_readl);
	kunit_activate_static_stub(test, dcuart_tx_free, dcut_tx_free);
	kunit_activate_static_stub(test, dcuart_tx_put, dcut_tx_put);

	return 0;
}

static void dcut_exit(struct kunit *test)
{
	struct dcut_ctx *ctx = test->priv;

	if (!ctx || !ctx->state)
		return;

	timer_delete_sync(&ctx->dcuart.drain_timer);
	if (ctx->con_slot)
		dcuart_ports[DCUT_CON] = NULL;
	if (ctx->irq_requested)
		free_irq(ctx->virq, ctx);
	if (ctx->virq > 0)
		irq_dispose_mapping(ctx->virq);
	if (ctx->domain && !IS_ERR(ctx->domain))
		irq_domain_remove_sim(ctx->domain);
	if (ctx->fwnode)
		irq_domain_free_fwnode(ctx->fwnode);
	tty_port_destroy(&ctx->state->port);
	kfifo_free(&ctx->state->port.xmit_fifo);
}

/* Give the port a (simulated) interrupt line, as a non-sw-drain probe does. */
static irqreturn_t dcut_irq_handler(int irq, void *data)
{
	return IRQ_HANDLED;
}

static void dcut_setup_irq(struct kunit *test, struct dcut_ctx *ctx)
{
	ctx->fwnode = irq_domain_alloc_named_fwnode("dockchannel-uart-kunit");
	KUNIT_ASSERT_NOT_NULL(test, ctx->fwnode);
	ctx->domain = irq_domain_create_sim(ctx->fwnode, 1);
	KUNIT_ASSERT_FALSE(test, IS_ERR(ctx->domain));
	ctx->virq = irq_create_mapping(ctx->domain, 0);
	KUNIT_ASSERT_GT(test, ctx->virq, 0);
	KUNIT_ASSERT_EQ(test, request_irq(ctx->virq, dcut_irq_handler, IRQF_NO_AUTOEN,
					  "dockchannel-uart-kunit", ctx), 0);
	ctx->irq_requested = true;
	ctx->dcuart.port.irq = ctx->virq;
}

static bool dcut_irq_enabled(struct dcut_ctx *ctx)
{
	return !irqd_irq_disabled(irq_get_irq_data(ctx->virq));
}

static void dcut_queue_rx(struct dcut_ctx *ctx, unsigned int len, unsigned int visible)
{
	unsigned int i;

	for (i = 0; i < len; i++)
		ctx->rx_q[i] = 0x20 + (i * 13) % 0x5f;
	ctx->rx_len = len;
	ctx->rx_visible = visible;
}

static unsigned int dcut_queue_tx(struct dcut_ctx *ctx, const char *s)
{
	return kfifo_in(&ctx->state->port.xmit_fifo, s, strlen(s));
}

/* What the port handed to the TTY layer (flip buffers not yet pushed). */
static unsigned int dcut_flip_read(struct dcut_ctx *ctx, u8 *out, unsigned int max)
{
	struct tty_buffer *b;
	unsigned int n = 0, i;

	for (b = ctx->state->port.buf.head; b; b = b->next)
		for (i = b->read; i < b->used && n < max; i++)
			out[n++] = *char_buf_ptr(b, i);

	return n;
}

static unsigned int dcut_drain(struct dcut_ctx *ctx, bool *got)
{
	unsigned long flags;

	spin_lock_irqsave(&ctx->dcuart.lock, flags);
	*got = dcuart_drain_rx(&ctx->dcuart);
	spin_unlock_irqrestore(&ctx->dcuart.lock, flags);

	return ctx->dcuart.port.icount.rx;
}

static unsigned int dcut_tx_chars(struct dcut_ctx *ctx, bool tick)
{
	unsigned long flags;
	unsigned int pending;

	uart_port_lock_irqsave(&ctx->dcuart.port, &flags);
	pending = dcuart_tx_chars(&ctx->dcuart, tick);
	uart_port_unlock_irqrestore(&ctx->dcuart.port, flags);

	return pending;
}

/* RX */

static void dcut_drain_rx_bytes(struct kunit *test)
{
	struct dcut_ctx *ctx = test->priv;
	u8 out[16];
	bool got;

	dcut_queue_rx(ctx, 5, 5);

	KUNIT_EXPECT_EQ(test, dcut_drain(ctx, &got), 5);
	KUNIT_EXPECT_TRUE(test, got);
	/* Each byte taken from bits [15:8] of the byte FIFO register. */
	KUNIT_ASSERT_EQ(test, dcut_flip_read(ctx, out, sizeof(out)), 5);
	KUNIT_EXPECT_MEMEQ(test, out, ctx->rx_q, 5);
	KUNIT_ASSERT_EQ(test, ctx->rx_ncounts, 2);
	KUNIT_EXPECT_EQ(test, ctx->rx_counts[0], 5);
	KUNIT_EXPECT_EQ(test, ctx->rx_counts[1], 0);
	KUNIT_EXPECT_EQ(test, ctx->dcuart.port.icount.buf_overrun, 0);
	KUNIT_EXPECT_EQ(test, ctx->violations, 0);
}

static void dcut_drain_rx_empty(struct kunit *test)
{
	struct dcut_ctx *ctx = test->priv;
	u8 out[4];
	bool got;

	KUNIT_EXPECT_EQ(test, dcut_drain(ctx, &got), 0);
	KUNIT_EXPECT_FALSE(test, got);
	KUNIT_EXPECT_EQ(test, ctx->rx_ncounts, 1);
	KUNIT_EXPECT_EQ(test, dcut_flip_read(ctx, out, sizeof(out)), 0);
	KUNIT_EXPECT_EQ(test, ctx->violations, 0);
}

static void dcut_drain_rx_clamps_pass(struct kunit *test)
{
	struct dcut_ctx *ctx = test->priv;
	static u8 out[DCUT_LOG];
	bool got;

	/* More than a FIFO's worth: one FIFO_SIZE pass, then the rest. */
	dcut_queue_rx(ctx, 3000, 3000);

	KUNIT_EXPECT_EQ(test, dcut_drain(ctx, &got), 3000);
	KUNIT_ASSERT_EQ(test, ctx->rx_ncounts, 3);
	KUNIT_EXPECT_EQ(test, ctx->rx_counts[0], 3000);
	KUNIT_EXPECT_EQ(test, ctx->rx_counts[1], 3000 - FIFO_SIZE);
	KUNIT_EXPECT_EQ(test, ctx->rx_counts[2], 0);
	KUNIT_ASSERT_EQ(test, dcut_flip_read(ctx, out, sizeof(out)), 3000);
	KUNIT_EXPECT_MEMEQ(test, out, ctx->rx_q, 3000);
	KUNIT_EXPECT_EQ(test, ctx->violations, 0);
}

static void dcut_drain_rx_arrivals(struct kunit *test)
{
	struct dcut_ctx *ctx = test->priv;
	u8 out[16];
	bool got;

	/* Data that arrives while draining is drained too. */
	dcut_queue_rx(ctx, 10, 3);
	ctx->rx_chunk[0] = 4;
	ctx->rx_chunk[1] = 3;
	ctx->rx_nchunk = 2;

	KUNIT_EXPECT_EQ(test, dcut_drain(ctx, &got), 10);
	KUNIT_ASSERT_EQ(test, ctx->rx_ncounts, 4);
	KUNIT_EXPECT_EQ(test, ctx->rx_counts[0], 3);
	KUNIT_EXPECT_EQ(test, ctx->rx_counts[1], 4);
	KUNIT_EXPECT_EQ(test, ctx->rx_counts[2], 3);
	KUNIT_EXPECT_EQ(test, ctx->rx_counts[3], 0);
	KUNIT_ASSERT_EQ(test, dcut_flip_read(ctx, out, sizeof(out)), 10);
	KUNIT_EXPECT_MEMEQ(test, out, ctx->rx_q, 10);
	KUNIT_EXPECT_EQ(test, ctx->violations, 0);
}

static void dcut_interrupt_rx(struct kunit *test)
{
	struct dcut_ctx *ctx = test->priv;
	u32 *flag = dcut_reg(ctx->irqregs, IRQ_FLAG);

	dcut_queue_rx(ctx, 4, 4);
	WRITE_ONCE(*flag, ctx->dcuart.irq_rx | ctx->dcuart.irq_tx | DCUT_OTHERS);

	KUNIT_EXPECT_EQ(test, dcuart_interrupt(0, &ctx->dcuart), IRQ_HANDLED);
	KUNIT_EXPECT_EQ(test, ctx->dcuart.port.icount.rx, 4);
	KUNIT_EXPECT_EQ(test, ctx->rx_visible, 0);
	/* Only this channel's RX flag is cleared (write-one-to-clear). */
	KUNIT_EXPECT_EQ(test, READ_ONCE(*flag), ctx->dcuart.irq_rx);
	KUNIT_EXPECT_EQ(test, ctx->violations, 0);
}

static void dcut_interrupt_other_channel(struct kunit *test)
{
	struct dcut_ctx *ctx = test->priv;
	u32 *flag = dcut_reg(ctx->irqregs, IRQ_FLAG);

	dcut_queue_rx(ctx, 4, 4);
	WRITE_ONCE(*flag, DCUT_OTHERS | IRQ_RX(DCUT_CHANNEL + 1) | IRQ_TX(DCUT_CHANNEL - 1));

	KUNIT_EXPECT_EQ(test, dcuart_interrupt(0, &ctx->dcuart), IRQ_NONE);
	KUNIT_EXPECT_EQ(test, READ_ONCE(*flag), DCUT_OTHERS | IRQ_RX(DCUT_CHANNEL + 1) |
			IRQ_TX(DCUT_CHANNEL - 1));
	KUNIT_EXPECT_EQ(test, ctx->rx_ncounts, 0);
	KUNIT_EXPECT_EQ(test, ctx->dcuart.port.icount.rx, 0);
}

/* Interrupt mask, startup and shutdown */

static void dcut_irq_bits_per_channel(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, IRQ_TX(0), BIT(0));
	KUNIT_EXPECT_EQ(test, IRQ_RX(0), BIT(1));
	KUNIT_EXPECT_EQ(test, IRQ_TX(3), BIT(6));
	KUNIT_EXPECT_EQ(test, IRQ_RX(3), BIT(7));
	KUNIT_EXPECT_EQ(test, IRQ_RX(MAX_CHANNELS - 1), BIT(31));
}

static void dcut_irq_mask_other_channels(struct kunit *test)
{
	struct dcut_ctx *ctx = test->priv;
	struct dcuart_port *dc = &ctx->dcuart;
	u32 *mask = dcut_reg(ctx->irqregs, IRQ_MASK);
	u32 *flag = dcut_reg(ctx->irqregs, IRQ_FLAG);

	WRITE_ONCE(*mask, DCUT_OTHERS);
	WRITE_ONCE(*flag, 0);

	dcuart_irq_enable(dc, dc->irq_rx);
	KUNIT_EXPECT_EQ(test, READ_ONCE(*mask), DCUT_OTHERS | dc->irq_rx);
	/* Stale latched flag cleared before the unmask. */
	KUNIT_EXPECT_EQ(test, READ_ONCE(*flag), dc->irq_rx);

	dcuart_irq_update(dc, dc->irq_tx, true);
	KUNIT_EXPECT_EQ(test, READ_ONCE(*mask), DCUT_OTHERS | dc->irq_rx | dc->irq_tx);

	dcuart_irq_disable(dc, dc->irq_tx | dc->irq_rx);
	KUNIT_EXPECT_EQ(test, READ_ONCE(*mask), DCUT_OTHERS);
	dcuart_irq_disable(dc, dc->irq_rx);
	KUNIT_EXPECT_EQ(test, READ_ONCE(*mask), DCUT_OTHERS);
}

static void dcut_startup_irq_mode(struct kunit *test)
{
	struct dcut_ctx *ctx = test->priv;
	struct dcuart_port *dc = &ctx->dcuart;
	u32 *mask = dcut_reg(ctx->irqregs, IRQ_MASK);
	u32 *flag = dcut_reg(ctx->irqregs, IRQ_FLAG);

	dcut_setup_irq(test, ctx);
	WRITE_ONCE(*mask, DCUT_OTHERS | dc->irq_tx);
	WRITE_ONCE(*flag, 0);

	KUNIT_EXPECT_EQ(test, dcuart_startup(&dc->port), 0);
	KUNIT_EXPECT_EQ(test, READ_ONCE(*dcut_reg(ctx->cfg, CONFIG_RX_THRESH)), 1);
	KUNIT_EXPECT_EQ(test, READ_ONCE(*dcut_reg(ctx->cfg, CONFIG_TX_THRESH)), FIFO_SIZE);
	/* RX unmasked, TX masked, the other channels untouched. */
	KUNIT_EXPECT_EQ(test, READ_ONCE(*mask), DCUT_OTHERS | dc->irq_rx);
	KUNIT_EXPECT_EQ(test, READ_ONCE(*flag), dc->irq_rx);
	KUNIT_EXPECT_TRUE(test, dcut_irq_enabled(ctx));
	KUNIT_EXPECT_FALSE(test, timer_pending(&dc->drain_timer));
	KUNIT_EXPECT_NE(test, ctx->state->port.closing_wait, ASYNC_CLOSING_WAIT_NONE);

	dcuart_shutdown(&dc->port);
	KUNIT_EXPECT_FALSE(test, dcut_irq_enabled(ctx));
	KUNIT_EXPECT_EQ(test, READ_ONCE(*mask), DCUT_OTHERS);
}

static void dcut_startup_sw_drain(struct kunit *test)
{
	struct dcut_ctx *ctx = test->priv;
	struct dcuart_port *dc = &ctx->dcuart;
	u32 *mask = dcut_reg(ctx->irqregs, IRQ_MASK);

	dc->sw_drain = true;
	WRITE_ONCE(*mask, DCUT_OTHERS | dc->irq_rx | dc->irq_tx);

	KUNIT_EXPECT_EQ(test, dcuart_startup(&dc->port), 0);
	/* No RX interrupt: the drain timer polls the channel. */
	KUNIT_EXPECT_EQ(test, READ_ONCE(*mask), DCUT_OTHERS);
	KUNIT_EXPECT_TRUE(test, timer_pending(&dc->drain_timer));

	dcuart_shutdown(&dc->port);
	KUNIT_EXPECT_FALSE(test, timer_pending(&dc->drain_timer));
	KUNIT_EXPECT_EQ(test, READ_ONCE(*mask), DCUT_OTHERS);
}

static void dcut_startup_never_wait(struct kunit *test)
{
	struct dcut_ctx *ctx = test->priv;
	struct dcuart_port *dc = &ctx->dcuart;

	dc->sw_drain = true;
	dc->never_wait = true;

	/* A close must not wait for a reader that may never come. */
	KUNIT_EXPECT_EQ(test, dcuart_startup(&dc->port), 0);
	KUNIT_EXPECT_EQ(test, ctx->state->port.closing_wait, ASYNC_CLOSING_WAIT_NONE);
	dcuart_shutdown(&dc->port);
	KUNIT_EXPECT_FALSE(test, timer_pending(&dc->drain_timer));
}

/* TX */

static void dcut_tx_chars_room_limits(struct kunit *test)
{
	struct dcut_ctx *ctx = test->priv;

	ctx->dcuart.never_wait = true;
	dcut_queue_tx(ctx, "abcdefgh");
	ctx->tx_room = 5;

	KUNIT_EXPECT_EQ(test, dcut_tx_chars(ctx, false), 3);
	KUNIT_ASSERT_EQ(test, ctx->tx_len, 5);
	KUNIT_EXPECT_MEMEQ(test, ctx->tx_log, "abcde", 5);
	KUNIT_EXPECT_EQ(test, ctx->dcuart.port.icount.tx, 5);
	KUNIT_EXPECT_EQ(test, kfifo_len(&ctx->state->port.xmit_fifo), 3);
	KUNIT_EXPECT_EQ(test, ctx->dcuart.tx_stuck_ticks, 0);
	KUNIT_EXPECT_EQ(test, ctx->violations, 0);

	/* The rest when room appears. */
	ctx->tx_room = 64;
	KUNIT_EXPECT_EQ(test, dcut_tx_chars(ctx, true), 0);
	KUNIT_ASSERT_EQ(test, ctx->tx_len, 8);
	KUNIT_EXPECT_MEMEQ(test, ctx->tx_log, "abcdefgh", 8);
}

static void dcut_tx_chars_exact_room(struct kunit *test)
{
	struct dcut_ctx *ctx = test->priv;

	ctx->dcuart.never_wait = true;
	dcut_queue_tx(ctx, "12345678");
	ctx->tx_room = 8;

	KUNIT_EXPECT_EQ(test, dcut_tx_chars(ctx, false), 0);
	KUNIT_EXPECT_EQ(test, ctx->tx_len, 8);
	KUNIT_EXPECT_EQ(test, ctx->tx_room, 0);
	KUNIT_EXPECT_EQ(test, kfifo_len(&ctx->state->port.xmit_fifo), 0);
	KUNIT_EXPECT_EQ(test, ctx->violations, 0);
}

static void dcut_tx_chars_nothing_queued(struct kunit *test)
{
	struct dcut_ctx *ctx = test->priv;

	ctx->tx_room = 8;
	KUNIT_EXPECT_EQ(test, dcut_tx_chars(ctx, true), 0);
	/* Not even a room read. */
	KUNIT_EXPECT_EQ(test, ctx->tx_free_reads, 0);
	KUNIT_EXPECT_EQ(test, ctx->tx_len, 0);
}

static void dcut_tx_chars_no_reader_drops(struct kunit *test)
{
	struct dcut_ctx *ctx = test->priv;

	ctx->dcuart.never_wait = true;
	dcut_queue_tx(ctx, "wxyz");
	ctx->tx_room = 0;

	/* Without a tick (start_tx), a full FIFO is not counted. */
	KUNIT_EXPECT_EQ(test, dcut_tx_chars(ctx, false), 4);
	KUNIT_EXPECT_EQ(test, ctx->dcuart.tx_stuck_ticks, 0);

	/* TX_STUCK_TICKS ticks without room: the queue goes. */
	KUNIT_EXPECT_EQ(test, dcut_tx_chars(ctx, true), 4);
	KUNIT_EXPECT_EQ(test, ctx->dcuart.tx_stuck_ticks, 1);
	KUNIT_EXPECT_EQ(test, dcut_tx_chars(ctx, true), 0);
	KUNIT_EXPECT_EQ(test, ctx->dcuart.tx_stuck_ticks, 0);
	KUNIT_EXPECT_EQ(test, kfifo_len(&ctx->state->port.xmit_fifo), 0);
	KUNIT_EXPECT_EQ(test, ctx->tx_len, 0);
	KUNIT_EXPECT_EQ(test, ctx->violations, 0);
}

static void dcut_tx_chars_progress_resets(struct kunit *test)
{
	struct dcut_ctx *ctx = test->priv;

	ctx->dcuart.never_wait = true;
	dcut_queue_tx(ctx, "abcd");
	ctx->dcuart.tx_stuck_ticks = 1;
	ctx->tx_room = 2;

	/* Some room: progress, so the stuck count starts over. */
	KUNIT_EXPECT_EQ(test, dcut_tx_chars(ctx, true), 2);
	KUNIT_EXPECT_EQ(test, ctx->dcuart.tx_stuck_ticks, 0);
	KUNIT_EXPECT_EQ(test, dcut_tx_chars(ctx, true), 2);
	KUNIT_EXPECT_EQ(test, ctx->dcuart.tx_stuck_ticks, 1);
	KUNIT_EXPECT_EQ(test, kfifo_len(&ctx->state->port.xmit_fifo), 2);
	KUNIT_EXPECT_MEMEQ(test, ctx->tx_log, "ab", 2);
}

static void dcut_tx_chars_x_char(struct kunit *test)
{
	struct dcut_ctx *ctx = test->priv;

	ctx->dcuart.never_wait = true;
	ctx->dcuart.port.x_char = 0x13;
	ctx->tx_room = 1;

	KUNIT_EXPECT_EQ(test, dcut_tx_chars(ctx, false), 0);
	KUNIT_ASSERT_EQ(test, ctx->tx_len, 1);
	KUNIT_EXPECT_EQ(test, ctx->tx_log[0], 0x13);
	KUNIT_EXPECT_EQ(test, ctx->dcuart.port.x_char, 0);
	KUNIT_EXPECT_EQ(test, ctx->violations, 0);
}

static void dcut_tx_empty(struct kunit *test)
{
	struct dcut_ctx *ctx = test->priv;
	struct uart_port *port = &ctx->dcuart.port;

	ctx->tx_room = 0;
	KUNIT_EXPECT_EQ(test, dcuart_tx_empty(port), 0);
	ctx->tx_room = 1;
	KUNIT_EXPECT_EQ(test, dcuart_tx_empty(port), TIOCSER_TEMT);

	/* Never waiting: always empty, whatever the FIFO says. */
	ctx->dcuart.never_wait = true;
	ctx->tx_room = 0;
	ctx->tx_free_reads = 0;
	KUNIT_EXPECT_EQ(test, dcuart_tx_empty(port), TIOCSER_TEMT);
	KUNIT_EXPECT_EQ(test, ctx->tx_free_reads, 0);
}

static void dcut_start_tx_never_wait(struct kunit *test)
{
	struct dcut_ctx *ctx = test->priv;
	struct dcuart_port *dc = &ctx->dcuart;

	dc->never_wait = true;
	dcut_queue_tx(ctx, "hello");
	ctx->tx_room = 2;

	/* What does not fit goes from the timer. */
	dcuart_start_tx(&dc->port);
	KUNIT_EXPECT_EQ(test, ctx->tx_len, 2);
	KUNIT_EXPECT_EQ(test, kfifo_len(&ctx->state->port.xmit_fifo), 3);
	KUNIT_EXPECT_TRUE(test, timer_pending(&dc->drain_timer));
	timer_delete_sync(&dc->drain_timer);
}

static void dcut_start_tx_waiting(struct kunit *test)
{
	struct dcut_ctx *ctx = test->priv;
	struct dcuart_port *dc = &ctx->dcuart;

	/*
	 * Without never_wait every byte goes through dockchannel_wait_putc(),
	 * which reads the register buffer directly.
	 */
	WRITE_ONCE(*dcut_reg(ctx->data, DATA_TX_FREE), 1);
	dcut_queue_tx(ctx, "xyz");

	dcuart_start_tx(&dc->port);
	KUNIT_EXPECT_EQ(test, kfifo_len(&ctx->state->port.xmit_fifo), 0);
	KUNIT_EXPECT_EQ(test, dc->port.icount.tx, 3);
	KUNIT_EXPECT_EQ(test, READ_ONCE(*dcut_reg(ctx->data, DATA_TX8)), 'z');
	KUNIT_EXPECT_FALSE(test, timer_pending(&dc->drain_timer));
	KUNIT_EXPECT_EQ(test, ctx->tx_len, 0);
}

static void dcut_drain_timer_sw_drain(struct kunit *test)
{
	struct dcut_ctx *ctx = test->priv;
	struct dcuart_port *dc = &ctx->dcuart;

	dc->never_wait = true;
	dc->sw_drain = true;
	dcut_queue_rx(ctx, 3, 3);
	dcut_queue_tx(ctx, "ok");
	ctx->tx_room = 16;

	dcuart_drain_timer(&dc->drain_timer);
	KUNIT_EXPECT_EQ(test, dc->port.icount.rx, 3);
	KUNIT_EXPECT_EQ(test, ctx->tx_len, 2);
	/* A software-drained channel keeps polling. */
	KUNIT_EXPECT_TRUE(test, timer_pending(&dc->drain_timer));
	timer_delete_sync(&dc->drain_timer);
	KUNIT_EXPECT_EQ(test, ctx->violations, 0);
}

static void dcut_drain_timer_no_reader(struct kunit *test)
{
	struct dcut_ctx *ctx = test->priv;
	struct dcuart_port *dc = &ctx->dcuart;

	dc->never_wait = true;
	dcut_queue_tx(ctx, "abc");
	ctx->tx_room = 0;

	dcuart_drain_timer(&dc->drain_timer);
	KUNIT_EXPECT_TRUE(test, timer_pending(&dc->drain_timer));
	timer_delete_sync(&dc->drain_timer);
	KUNIT_EXPECT_EQ(test, kfifo_len(&ctx->state->port.xmit_fifo), 3);

	/* The second tick without room drops the queue and stops the timer. */
	dcuart_drain_timer(&dc->drain_timer);
	KUNIT_EXPECT_EQ(test, kfifo_len(&ctx->state->port.xmit_fifo), 0);
	KUNIT_EXPECT_FALSE(test, timer_pending(&dc->drain_timer));
	KUNIT_EXPECT_EQ(test, ctx->rx_ncounts, 0);
}

/* termios and port ops */

static void dcut_set_termios(struct kunit *test)
{
	struct dcut_ctx *ctx = test->priv;
	struct ktermios t = {
		.c_cflag = B9600 | CS7 | PARENB | PARODD | CRTSCTS,
	};

	dcuart_set_termios(&ctx->dcuart.port, &t, NULL);
	/* 8N1 at 115200, CLOCAL, whatever was asked. */
	KUNIT_EXPECT_EQ(test, t.c_cflag & CSIZE, CS8);
	KUNIT_EXPECT_FALSE(test, t.c_cflag & PARENB);
	KUNIT_EXPECT_TRUE(test, t.c_cflag & CLOCAL);
	KUNIT_EXPECT_EQ(test, tty_termios_baud_rate(&t), 115200);
	KUNIT_EXPECT_EQ(test, t.c_ispeed, 115200);
	KUNIT_EXPECT_EQ(test, t.c_ospeed, 115200);
	/* 10 bits a character at 115200 baud, rounded up. */
	KUNIT_EXPECT_EQ(test, ctx->dcuart.port.frame_time, 86806);
}

static void dcut_port_ops(struct kunit *test)
{
	struct dcut_ctx *ctx = test->priv;
	struct uart_port *port = &ctx->dcuart.port;

	KUNIT_EXPECT_EQ(test, dcuart_get_mctrl(port), TIOCM_CAR | TIOCM_DSR | TIOCM_CTS);
	KUNIT_EXPECT_STREQ(test, dcuart_type(port), "Apple DockChannel UART");
	port->type = PORT_UNKNOWN;
	KUNIT_EXPECT_NULL(test, dcuart_type(port));
	dcuart_config_port(port, 0);
	KUNIT_EXPECT_EQ(test, port->type, PORT_UNKNOWN);
	dcuart_config_port(port, UART_CONFIG_TYPE);
	KUNIT_EXPECT_EQ(test, port->type, PORT_GENERIC);
}

/* Console helpers (apple_dockchannel.h, shared with the earlycon) */

static void dcut_console_putc_room(struct kunit *test)
{
	struct dcut_ctx *ctx = test->priv;
	void __iomem *data = ctx->dcuart.port.membase;
	u32 *tx_free = dcut_reg(ctx->data, DATA_TX_FREE);
	u32 *tx8 = dcut_reg(ctx->data, DATA_TX8);
	struct dockchannel_tx tx;

	dockchannel_tx_begin(&tx);
	WRITE_ONCE(*tx_free, 2);
	dockchannel_console_putc(data, &tx, 'a');
	dockchannel_console_putc(data, &tx, 'b');
	KUNIT_EXPECT_EQ(test, READ_ONCE(*tx8), 'b');
	KUNIT_EXPECT_EQ(test, tx.room, 0);

	/* Room used up: read again; it reads 0, so the rest is dropped. */
	WRITE_ONCE(*tx_free, 0);
	dockchannel_console_putc(data, &tx, 'c');
	KUNIT_EXPECT_TRUE(test, tx.full);
	KUNIT_EXPECT_EQ(test, READ_ONCE(*tx8), 'b');
	/* ... without reading again in the same write. */
	WRITE_ONCE(*tx_free, 5);
	dockchannel_console_putc(data, &tx, 'd');
	KUNIT_EXPECT_EQ(test, READ_ONCE(*tx8), 'b');

	/* The next write starts over. */
	dockchannel_tx_begin(&tx);
	dockchannel_console_putc(data, &tx, 'e');
	KUNIT_EXPECT_EQ(test, READ_ONCE(*tx8), 'e');
	KUNIT_EXPECT_EQ(test, tx.room, 4);
	KUNIT_EXPECT_FALSE(test, tx.full);
}

static void dcut_wait_putc_no_reader(struct kunit *test)
{
	struct dcut_ctx *ctx = test->priv;
	void __iomem *data = ctx->dcuart.port.membase;
	u32 *tx_free = dcut_reg(ctx->data, DATA_TX_FREE);
	u32 *tx8 = dcut_reg(ctx->data, DATA_TX8);
	bool no_reader = false;

	WRITE_ONCE(*tx8, DCUT_POISON);
	WRITE_ONCE(*tx_free, 0);
	dockchannel_wait_putc(data, &no_reader, 'q');
	KUNIT_EXPECT_TRUE(test, no_reader);
	KUNIT_EXPECT_EQ(test, READ_ONCE(*tx8), DCUT_POISON);
	dockchannel_wait_putc(data, &no_reader, 'r');
	KUNIT_EXPECT_TRUE(test, no_reader);
	KUNIT_EXPECT_EQ(test, READ_ONCE(*tx8), DCUT_POISON);

	/* Room again: write, and wait normally from now on. */
	WRITE_ONCE(*tx_free, 1);
	dockchannel_wait_putc(data, &no_reader, 's');
	KUNIT_EXPECT_FALSE(test, no_reader);
	KUNIT_EXPECT_EQ(test, READ_ONCE(*tx8), 's');
}

static void dcut_console_setup(struct kunit *test)
{
	struct dcut_ctx *ctx = test->priv;
	struct console co = dcuart_console;

	co.index = -1;
	KUNIT_EXPECT_EQ(test, dcuart_console_setup(&co, NULL), -ENODEV);
	co.index = MAX_PORTS;
	KUNIT_EXPECT_EQ(test, dcuart_console_setup(&co, NULL), -ENODEV);

	if (dcuart_ports[DCUT_CON])
		kunit_skip(test, "port %d is in use", DCUT_CON);
	co.index = DCUT_CON;
	KUNIT_EXPECT_EQ(test, dcuart_console_setup(&co, NULL), -ENODEV);
	dcuart_ports[DCUT_CON] = &ctx->dcuart;
	ctx->con_slot = true;
	KUNIT_EXPECT_EQ(test, dcuart_console_setup(&co, NULL), 0);
}

static void dcut_console_write_never_wait(struct kunit *test)
{
	struct dcut_ctx *ctx = test->priv;
	struct console co = dcuart_console;
	u32 *tx_free = dcut_reg(ctx->data, DATA_TX_FREE);
	u32 *tx8 = dcut_reg(ctx->data, DATA_TX8);

	if (dcuart_ports[DCUT_CON])
		kunit_skip(test, "port %d is in use", DCUT_CON);
	dcuart_ports[DCUT_CON] = &ctx->dcuart;
	ctx->con_slot = true;
	ctx->dcuart.never_wait = true;
	co.index = DCUT_CON;

	/* No room: nothing written, and the write returns. */
	WRITE_ONCE(*tx8, DCUT_POISON);
	WRITE_ONCE(*tx_free, 0);
	dcuart_write(&co, "hi\n", 3);
	KUNIT_EXPECT_EQ(test, READ_ONCE(*tx8), DCUT_POISON);

	WRITE_ONCE(*tx_free, 64);
	dcuart_write(&co, "hi\n", 3);
	KUNIT_EXPECT_EQ(test, READ_ONCE(*tx8), '\n');
	KUNIT_EXPECT_EQ(test, ctx->dcuart.console_tx.room, 64 - 4);
}

static struct kunit_case dcuart_test_cases[] = {
	KUNIT_CASE(dcut_drain_rx_bytes),
	KUNIT_CASE(dcut_drain_rx_empty),
	KUNIT_CASE(dcut_drain_rx_clamps_pass),
	KUNIT_CASE(dcut_drain_rx_arrivals),
	KUNIT_CASE(dcut_interrupt_rx),
	KUNIT_CASE(dcut_interrupt_other_channel),
	KUNIT_CASE(dcut_irq_bits_per_channel),
	KUNIT_CASE(dcut_irq_mask_other_channels),
	KUNIT_CASE(dcut_startup_irq_mode),
	KUNIT_CASE(dcut_startup_sw_drain),
	KUNIT_CASE(dcut_startup_never_wait),
	KUNIT_CASE(dcut_tx_chars_room_limits),
	KUNIT_CASE(dcut_tx_chars_exact_room),
	KUNIT_CASE(dcut_tx_chars_nothing_queued),
	KUNIT_CASE(dcut_tx_chars_no_reader_drops),
	KUNIT_CASE(dcut_tx_chars_progress_resets),
	KUNIT_CASE(dcut_tx_chars_x_char),
	KUNIT_CASE(dcut_tx_empty),
	KUNIT_CASE(dcut_start_tx_never_wait),
	KUNIT_CASE(dcut_start_tx_waiting),
	KUNIT_CASE(dcut_drain_timer_sw_drain),
	KUNIT_CASE(dcut_drain_timer_no_reader),
	KUNIT_CASE(dcut_set_termios),
	KUNIT_CASE(dcut_port_ops),
	KUNIT_CASE(dcut_console_putc_room),
	KUNIT_CASE(dcut_wait_putc_no_reader),
	KUNIT_CASE(dcut_console_setup),
	KUNIT_CASE(dcut_console_write_never_wait),
	{}
};

static struct kunit_suite dcuart_test_suite = {
	.name = "apple-dockchannel-uart",
	.init = dcut_init,
	.exit = dcut_exit,
	.test_cases = dcuart_test_cases,
};

kunit_test_suite(dcuart_test_suite);
