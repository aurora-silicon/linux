// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * Apple DockChannel UART driver
 * Copyright The Asahi Linux Contributors
 *
 * Based on the DockChannel UART driver by Yureka Lilian <yureka@cyberchaos.dev>.
 */

#include <linux/console.h>
#include <linux/io.h>
#include <linux/circ_buf.h>
#include <linux/serial_core.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/tty_flip.h>
#include <linux/timer.h>
#include <linux/jiffies.h>

#include "apple_dockchannel.h"

#define FIFO_SIZE         0x800

#define IRQ_MASK          0x0
#define IRQ_FLAG          0x4
/*
 * The interrupt window is shared by all channels of the block: channel n
 * owns bit 2n (TX) and bit 2n + 1 (RX) of both registers.
 */
#define IRQ_TX(ch)        BIT(2 * (ch))
#define IRQ_RX(ch)        BIT(2 * (ch) + 1)
#define MAX_CHANNELS      16

#define CONFIG_TX_THRESH  0x0
#define CONFIG_RX_THRESH  0x4

#define DATA_TX8          0x04
#define DATA_TX32         0x10
#define DATA_TX_FREE      0x14
#define DATA_RX8          0x1c
#define DATA_RX32         0x28
#define DATA_RX_COUNT     0x2c

#define MAX_PORTS         8
/*
 * Software-drained channels are polled at this interval. At 115200 baud
 * that is about 115 bytes per poll, far below the FIFO size.
 */
#define SW_DRAIN_MS       10
/*
 * TX never waits for FIFO room: what does not fit stays in the xmit buffer
 * and the timer sends it as room appears. After this many ticks with no room
 * at all, nothing is reading the channel: drop what is queued, so writers do
 * not block on a reader that may never come.
 */
#define TX_STUCK_TICKS    2

struct dcuart_port {
	struct uart_port port;
	void __iomem *config_base;
	void __iomem *irq_base;
	u32 irq_tx;
	u32 irq_rx;
	/* The channel does not interrupt on RX; drain it from a timer. */
	bool sw_drain;
	struct timer_list drain_timer;
	/* Serializes IRQ mask updates and RX FIFO draining. */
	spinlock_t lock;
	/* DOCKCHANNEL_NEVER_WAIT: TX never waits for room (J775D only). */
	bool never_wait;
	/* One console write's view of the TX FIFO (dockchannel_console_putc). */
	struct dockchannel_tx console_tx;
	/* Timer ticks in a row that found TX output queued and no FIFO room. */
	unsigned int tx_stuck_ticks;
};

static struct uart_driver dcuart_drv;
static struct dcuart_port *dcuart_ports[MAX_PORTS];

static inline struct dcuart_port *port_to_dc(struct uart_port *port)
{
	return container_of(port, struct dcuart_port, port);
}

static void dcuart_irq_update(struct dcuart_port *dcuart,
			      u32 bits, bool enable)
{
	u32 mask = readl_relaxed(dcuart->irq_base + IRQ_MASK);

	/* Leave the bits of the other channels alone. */
	if (enable)
		mask |= bits;
	else
		mask &= ~bits;
	writel_relaxed(mask, dcuart->irq_base + IRQ_MASK);
}

static void dcuart_irq_enable(struct dcuart_port *dcuart, u32 bits)
{
	/*
	 * IRQ_FLAG is write-to-clear. Clear stale latched flags before
	 * unmasking so the next interrupt reflects current FIFO state.
	 */
	writel_relaxed(bits, dcuart->irq_base + IRQ_FLAG);
	dcuart_irq_update(dcuart, bits, true);
}

static void dcuart_irq_disable(struct dcuart_port *dcuart, u32 bits)
{
	dcuart_irq_update(dcuart, bits, false);
}

/* The last console character timed out: see dockchannel_wait_putc(). */
static bool dcuart_no_reader;

static void dcuart_putc(struct uart_port *port, unsigned char c)
{
	struct dcuart_port *dcuart = container_of(port, struct dcuart_port, port);

	if (dcuart->never_wait)
		dockchannel_console_putc(port->membase, &dcuart->console_tx, c);
	else
		dockchannel_wait_putc(port->membase, &dcuart_no_reader, c);
}

static void dcuart_write(struct console *co, const char *s,
			 unsigned int n)
{
	struct dcuart_port *dcuart = dcuart_ports[co->index];
	struct uart_port *port = &dcuart->port;
	unsigned long flags;
	bool locked = true;

	if (!dcuart->never_wait) {
		uart_console_write(port, s, n, dcuart_putc);
		return;
	}

	/* The TX path writes the same FIFO, and the room read must stay true. */
	if (oops_in_progress)
		locked = uart_port_trylock_irqsave(port, &flags);
	else
		uart_port_lock_irqsave(port, &flags);

	dockchannel_tx_begin(&dcuart->console_tx);
	uart_console_write(port, s, n, dcuart_putc);

	if (locked)
		uart_port_unlock_irqrestore(port, flags);
}

static int dcuart_console_setup(struct console *co, char *options)
{
	struct dcuart_port *dcuart;

	if (co->index < 0 || co->index >= MAX_PORTS)
		return -ENODEV;

	dcuart = dcuart_ports[co->index];
	if (!dcuart)
		return -ENODEV;

	return 0;
}

static struct console dcuart_console = {
	.name    = "ttyDC",
	.device  = uart_console_device,
	.flags   = CON_PRINTBUFFER,
	.index   = -1,
	.write   = dcuart_write,
	.setup   = dcuart_console_setup,
	.data    = &dcuart_drv,
};

static struct uart_driver dcuart_drv = {
	.owner          = THIS_MODULE,
	.driver_name    = "dcuart",
	.nr             = MAX_PORTS,
	.cons           = &dcuart_console,
	.dev_name       = "ttyDC",
};

/* Move every byte in the RX FIFO to the TTY. Called with dcuart->lock held. */
static bool dcuart_drain_rx(struct dcuart_port *dcuart)
{
	bool got = false;
	int left;

	while ((left = readl_relaxed(dcuart->port.membase + DATA_RX_COUNT))) {
		left = min_t(size_t, left, FIFO_SIZE);
		while (left) {
			/*
			 * The byte FIFO register returns the byte in bits [15:8] on
			 * these instances.
			 */
			char c;

			c = readl_relaxed(dcuart->port.membase + DATA_RX8) >> 8;
			uart_insert_char(&dcuart->port, 0, 0, c, TTY_NORMAL);
			dcuart->port.icount.rx++;
			left--;
		}
		got = true;
	}

	return got;
}

static irqreturn_t dcuart_interrupt(int irq, void *data)
{
	struct dcuart_port *dcuart = data;
	u32 pending = readl_relaxed(dcuart->irq_base + IRQ_FLAG);
	u32 handled = 0;

	pending &= dcuart->irq_tx | dcuart->irq_rx;
	if (!pending)
		return IRQ_NONE;

	spin_lock(&dcuart->lock);

	if (pending & dcuart->irq_rx) {
		dcuart_drain_rx(dcuart);
		handled |= dcuart->irq_rx;
	}

	if (handled & dcuart->irq_rx)
		tty_flip_buffer_push(&dcuart->port.state->port);
	writel_relaxed(handled, dcuart->irq_base + IRQ_FLAG);
	spin_unlock(&dcuart->lock);

	return IRQ_HANDLED;
}

/*
 * Send what the TX FIFO has room for, without waiting. From the timer (tick),
 * also count ticks without room and drop the queue after TX_STUCK_TICKS.
 * Called with the port lock held. Returns the bytes left queued.
 */
static unsigned int dcuart_tx_chars(struct dcuart_port *dcuart, bool tick)
{
	struct uart_port *port = &dcuart->port;
	struct tty_port *tport = &port->state->port;
	unsigned int queued = kfifo_len(&tport->xmit_fifo), pending;
	u8 ch;

	if (!queued && !port->x_char)
		return 0;

	pending = uart_port_tx_limited(port, ch, readl(port->membase + DATA_TX_FREE),
				       true, writel(ch, port->membase + DATA_TX8), ({}));
	if (!pending || pending < queued) {
		dcuart->tx_stuck_ticks = 0;
		return pending;
	}
	if (!tick || ++dcuart->tx_stuck_ticks < TX_STUCK_TICKS)
		return pending;

	kfifo_reset_out(&tport->xmit_fifo);
	dcuart->tx_stuck_ticks = 0;
	uart_write_wakeup(port);
	return 0;
}

static void dcuart_drain_timer(struct timer_list *t)
{
	struct dcuart_port *dcuart = timer_container_of(dcuart, t, drain_timer);
	struct uart_port *port = &dcuart->port;
	unsigned long flags;
	unsigned int pending;

	if (!dcuart->never_wait) {
		bool got;

		spin_lock_irqsave(&dcuart->lock, flags);
		got = dcuart_drain_rx(dcuart);
		spin_unlock_irqrestore(&dcuart->lock, flags);

		if (got)
			tty_flip_buffer_push(&port->state->port);
		mod_timer(&dcuart->drain_timer, jiffies + msecs_to_jiffies(SW_DRAIN_MS));
		return;
	}

	if (dcuart->sw_drain) {
		bool got;

		spin_lock_irqsave(&dcuart->lock, flags);
		got = dcuart_drain_rx(dcuart);
		spin_unlock_irqrestore(&dcuart->lock, flags);

		if (got)
			tty_flip_buffer_push(&port->state->port);
	}

	uart_port_lock_irqsave(port, &flags);
	pending = dcuart_tx_chars(dcuart, true);
	uart_port_unlock_irqrestore(port, flags);

	if (dcuart->sw_drain || pending)
		mod_timer(&dcuart->drain_timer, jiffies + msecs_to_jiffies(SW_DRAIN_MS));
}

/*
 * Bytes in the FIFO stay there for a host to read whenever it comes; a full
 * FIFO means none is reading, not that a transmission is in progress. So
 * report empty: a close or tcdrain must not wait for a reader (it waited
 * twice the port timeout, 0.4 s, on every close of an unread channel).
 */
static unsigned int dcuart_tx_empty(struct uart_port *port)
{
	if (!port_to_dc(port)->never_wait)
		return (readl(port->membase + DATA_TX_FREE) != 0) ? TIOCSER_TEMT : 0;
	return TIOCSER_TEMT;
}

static void dcuart_start_tx(struct uart_port *port)
{
	struct dcuart_port *dcuart = port_to_dc(port);

	if (!dcuart->never_wait) {
		struct tty_port *tport = &port->state->port;
		char ch;

		while (kfifo_get(&tport->xmit_fifo, &ch)) {
			dcuart_putc(port, ch);
			port->icount.tx++;
		}
		return;
	}

	/* The rest goes from the timer, which the RX poll keeps running anyway. */
	if (dcuart_tx_chars(dcuart, false) && !timer_pending(&dcuart->drain_timer))
		mod_timer(&dcuart->drain_timer, jiffies + msecs_to_jiffies(SW_DRAIN_MS));
}

static void dcuart_stop_tx(struct uart_port *port)
{
}

static void dcuart_stop_rx(struct uart_port *port)
{
	struct dcuart_port *dcuart = port_to_dc(port);
	unsigned long flags;

	spin_lock_irqsave(&dcuart->lock, flags);
	dcuart_irq_disable(dcuart, dcuart->irq_rx);
	spin_unlock_irqrestore(&dcuart->lock, flags);
}

static const char *dcuart_type(struct uart_port *port)
{
	return port->type == PORT_GENERIC ?
		"Apple DockChannel UART" : NULL;
}

static int dcuart_startup(struct uart_port *port)
{
	struct dcuart_port *dcuart = port_to_dc(port);
	unsigned long flags;

	/*
	 * A close waits for the xmit buffer to empty, which on an unread
	 * channel takes until the timer drops it (systemd closes the console
	 * after every status line: 30 ms each). Never wait for a reader.
	 */
	if (dcuart->never_wait)
		port->state->port.closing_wait = ASYNC_CLOSING_WAIT_NONE;

	spin_lock_irqsave(&dcuart->lock, flags);
	dcuart_irq_disable(dcuart, dcuart->irq_tx | dcuart->irq_rx);
	writel_relaxed(1, dcuart->config_base + CONFIG_RX_THRESH);
	writel_relaxed(FIFO_SIZE, dcuart->config_base + CONFIG_TX_THRESH);
	if (!dcuart->sw_drain)
		dcuart_irq_enable(dcuart, dcuart->irq_rx);
	spin_unlock_irqrestore(&dcuart->lock, flags);

	if (dcuart->sw_drain)
		mod_timer(&dcuart->drain_timer, jiffies + msecs_to_jiffies(SW_DRAIN_MS));
	else
		enable_irq(port->irq);
	return 0;
}

static void dcuart_shutdown(struct uart_port *port)
{
	struct dcuart_port *dcuart = port_to_dc(port);
	unsigned long flags;

	/* With never_wait the timer also sends TX in IRQ mode. */
	if (dcuart->sw_drain || dcuart->never_wait)
		timer_delete_sync(&dcuart->drain_timer);
	if (!dcuart->sw_drain)
		disable_irq(port->irq);

	spin_lock_irqsave(&dcuart->lock, flags);
	dcuart_irq_disable(dcuart, dcuart->irq_tx | dcuart->irq_rx);
	spin_unlock_irqrestore(&dcuart->lock, flags);
}

static void dcuart_set_mctrl(struct uart_port *port, unsigned int mctrl)
{
}

static unsigned int dcuart_get_mctrl(struct uart_port *port)
{
	return TIOCM_CAR | TIOCM_DSR | TIOCM_CTS;
}

static void dcuart_set_termios(struct uart_port *port, struct ktermios *new,
			       const struct ktermios *old)
{
	new->c_cflag &= ~(CSIZE | PARENB);
	new->c_cflag |= CS8 | CLOCAL;
	tty_termios_encode_baud_rate(new, 115200, 115200);
	uart_update_timeout(port, new->c_cflag, 115200);
}

static void dcuart_release_port(struct uart_port *port)
{
}

static int dcuart_request_port(struct uart_port *port)
{
	return 0;
}

static void dcuart_config_port(struct uart_port *port, int flags)
{
	if (flags & UART_CONFIG_TYPE)
		port->type = PORT_GENERIC;
}

static const struct uart_ops dcuart_ops = {
	.tx_empty = dcuart_tx_empty,
	.start_tx = dcuart_start_tx,
	.stop_tx = dcuart_stop_tx,
	.stop_rx = dcuart_stop_rx,
	.type = dcuart_type,
	.startup = dcuart_startup,
	.shutdown = dcuart_shutdown,
	.set_mctrl = dcuart_set_mctrl,
	.get_mctrl = dcuart_get_mctrl,
	.set_termios = dcuart_set_termios,
	.release_port = dcuart_release_port,
	.request_port = dcuart_request_port,
	.config_port = dcuart_config_port,
};

static const struct of_device_id dcuart_id_table[] = {
	{ .compatible = "apple,dockchannel-uart", },
	{},
};
MODULE_DEVICE_TABLE(of, dcuart_id_table);

static int dcuart_probe(struct platform_device *pdev)
{
	int ret, index;
	struct dcuart_port *dcuart;
	struct resource *data_res;
	u32 channel = 0;

	for (index = 0; index < MAX_PORTS; index += 1) {
		if (!dcuart_ports[index])
			break;
	}
	if (index == MAX_PORTS)
		return dev_err_probe(&pdev->dev, -ENOSPC, "maximum number of ports reached");

	dcuart = devm_kzalloc(&pdev->dev, sizeof(*dcuart), GFP_KERNEL);
	if (!dcuart)
		return -ENOMEM;

	dcuart->port.ops = &dcuart_ops;
	dcuart->port.dev = &pdev->dev;
	dcuart->port.line = index;
	dcuart->port.fifosize = FIFO_SIZE;
	dcuart->port.iotype = UPIO_MEM32;
	dcuart->port.flags = UPF_BOOT_AUTOCONF;
	spin_lock_init(&dcuart->lock);

	of_property_read_u32(pdev->dev.of_node, "apple,channel", &channel);
	if (channel >= MAX_CHANNELS)
		return dev_err_probe(&pdev->dev, -EINVAL,
				     "invalid DockChannel channel %u\n", channel);
	dcuart->irq_tx = IRQ_TX(channel);
	dcuart->irq_rx = IRQ_RX(channel);
	dcuart->sw_drain = of_property_read_bool(pdev->dev.of_node, "apple,sw-drain");
	dcuart->never_wait = of_property_read_bool(pdev->dev.of_node, DOCKCHANNEL_NEVER_WAIT);
	timer_setup(&dcuart->drain_timer, dcuart_drain_timer, 0);

	dcuart->irq_base = devm_platform_ioremap_resource_byname(pdev, "irq");
	if (IS_ERR(dcuart->irq_base))
		return PTR_ERR(dcuart->irq_base);
	dcuart->config_base = devm_platform_ioremap_resource_byname(pdev, "config");
	if (IS_ERR(dcuart->config_base))
		return PTR_ERR(dcuart->config_base);
	dcuart->port.membase = devm_platform_ioremap_resource_byname(pdev, "data");
	if (IS_ERR(dcuart->port.membase))
		return PTR_ERR(dcuart->port.membase);

	data_res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "data");
	if (!data_res)
		return -EINVAL;
	dcuart->port.mapbase = data_res->start;

	dcuart_irq_disable(dcuart, dcuart->irq_tx | dcuart->irq_rx);
	writel_relaxed(dcuart->irq_tx | dcuart->irq_rx,
		       dcuart->irq_base + IRQ_FLAG);

	if (!dcuart->sw_drain) {
		dcuart->port.irq = platform_get_irq(pdev, 0);
		if (dcuart->port.irq < 0)
			return dcuart->port.irq;

		ret = devm_request_irq(&pdev->dev, dcuart->port.irq, dcuart_interrupt,
				       IRQF_NO_AUTOEN, dev_name(&pdev->dev), dcuart);
		if (ret)
			return dev_err_probe(&pdev->dev, ret, "failed to request IRQ\n");
	}

	dcuart_ports[index] = dcuart;
	ret = uart_add_one_port(&dcuart_drv, &dcuart->port);
	if (ret)
		goto cleanup;

	platform_set_drvdata(pdev, dcuart);

	return 0;
cleanup:
	dcuart_ports[index] = NULL;
	return ret;
}

static void dcuart_remove(struct platform_device *pdev)
{
	struct dcuart_port *dcuart = platform_get_drvdata(pdev);

	uart_remove_one_port(&dcuart_drv, &dcuart->port);
	dcuart_ports[dcuart->port.line] = NULL;
}

static struct platform_driver dcuart_platform_drv = {
	.probe          = dcuart_probe,
	.remove         = dcuart_remove,
	.driver         = {
		.name   = "apple-dockchannel-uart",
		.of_match_table = dcuart_id_table,
	},
};

static int __init dcuart_init(void)
{
	int ret;

	ret = uart_register_driver(&dcuart_drv);
	if (ret)
		return ret;

	ret = platform_driver_register(&dcuart_platform_drv);
	if (ret)
		uart_unregister_driver(&dcuart_drv);

	return ret;
}
device_initcall(dcuart_init);

MODULE_AUTHOR("Yureka Lilian <yureka@cyberchaos.dev>");
MODULE_DESCRIPTION("Apple DockChannel UART driver");
MODULE_LICENSE("Dual MIT/GPL");
