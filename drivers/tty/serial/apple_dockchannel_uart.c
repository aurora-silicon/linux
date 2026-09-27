// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * Apple DockChannel UART driver
 * Copyright The Asahi Linux Contributors
 */

#include <linux/console.h>
#include <linux/io.h>
#include <linux/circ_buf.h>
#include <linux/serial_core.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/tty_flip.h>

#define FIFO_SIZE         0x800

#define IRQ_MASK          0x0
#define IRQ_FLAG          0x4
#define IRQ_TX            BIT(0)
#define IRQ_RX            BIT(1)

#define CONFIG_TX_THRESH  0x0
#define CONFIG_RX_THRESH  0x4

#define DATA_TX8          0x04
#define DATA_TX32         0x10
#define DATA_TX_FREE      0x14
#define DATA_RX8          0x1c
#define DATA_RX32         0x28
#define DATA_RX_COUNT     0x2c

#define MAX_PORTS         8
#define TX_RETRIES        10000

struct dcuart_port {
	struct uart_port port;
	void __iomem *config_base;
	void __iomem *irq_base;
	u32 irq_mask;
	/* Serializes IRQ mask updates and RX FIFO draining. */
	spinlock_t lock;
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
	if (enable)
		dcuart->irq_mask |= bits;
	else
		dcuart->irq_mask &= ~bits;
	writel_relaxed(dcuart->irq_mask, dcuart->irq_base + IRQ_MASK);
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

static void dcuart_putc(struct uart_port *port, unsigned char c)
{
	unsigned int retries = TX_RETRIES;

	while (readl(port->membase + DATA_TX_FREE) == 0 && --retries)
		cpu_relax();
	if (!retries)
		return;

	writel(c, port->membase + DATA_TX8);
}

static void dcuart_write(struct console *co, const char *s,
			 unsigned int n)
{
	struct dcuart_port *dcuart = dcuart_ports[co->index];

	uart_console_write(&dcuart->port, s, n, dcuart_putc);
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

static irqreturn_t dcuart_interrupt(int irq, void *data)
{
	struct dcuart_port *dcuart = data;
	u32 pending = readl_relaxed(dcuart->irq_base + IRQ_FLAG);
	u32 handled = 0;

	if (!(pending & (IRQ_TX | IRQ_RX)))
		return IRQ_NONE;

	spin_lock(&dcuart->lock);

	if (pending & IRQ_RX) {
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
		}
		handled |= IRQ_RX;
	}

	if (handled & IRQ_RX)
		tty_flip_buffer_push(&dcuart->port.state->port);
	writel_relaxed(handled, dcuart->irq_base + IRQ_FLAG);
	spin_unlock(&dcuart->lock);

	return IRQ_HANDLED;
}

static unsigned int dcuart_tx_empty(struct uart_port *port)
{
	return (readl(port->membase + DATA_TX_FREE) != 0) ?
		TIOCSER_TEMT :
		0;
}

static void dcuart_start_tx(struct uart_port *port)
{
	char ch;
	struct tty_port *tport = &port->state->port;

	if (!kfifo_is_empty(&tport->xmit_fifo)) {
		while (kfifo_get(&tport->xmit_fifo, &ch)) {
			dcuart_putc(port, ch);
			port->icount.tx++;
		}
	}
}

static void dcuart_stop_tx(struct uart_port *port)
{
}

static void dcuart_stop_rx(struct uart_port *port)
{
	struct dcuart_port *dcuart = port_to_dc(port);
	unsigned long flags;

	spin_lock_irqsave(&dcuart->lock, flags);
	dcuart_irq_disable(dcuart, IRQ_RX);
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

	spin_lock_irqsave(&dcuart->lock, flags);
	dcuart_irq_disable(dcuart, IRQ_TX | IRQ_RX);
	writel_relaxed(1, dcuart->config_base + CONFIG_RX_THRESH);
	writel_relaxed(FIFO_SIZE, dcuart->config_base + CONFIG_TX_THRESH);
	dcuart_irq_enable(dcuart, IRQ_RX);
	spin_unlock_irqrestore(&dcuart->lock, flags);

	enable_irq(port->irq);
	return 0;
}

static void dcuart_shutdown(struct uart_port *port)
{
	struct dcuart_port *dcuart = port_to_dc(port);
	unsigned long flags;

	disable_irq(port->irq);

	spin_lock_irqsave(&dcuart->lock, flags);
	dcuart_irq_disable(dcuart, IRQ_TX | IRQ_RX);
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

	writel_relaxed(0, dcuart->irq_base + IRQ_MASK);
	writel_relaxed(~0, dcuart->irq_base + IRQ_FLAG);

	dcuart->port.irq = platform_get_irq(pdev, 0);
	if (dcuart->port.irq < 0)
		return dcuart->port.irq;

	ret = devm_request_irq(&pdev->dev, dcuart->port.irq, dcuart_interrupt,
			       IRQF_NO_AUTOEN, dev_name(&pdev->dev), dcuart);
	if (ret)
		return dev_err_probe(&pdev->dev, ret, "failed to request IRQ\n");

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

MODULE_DESCRIPTION("Apple DockChannel UART driver");
MODULE_LICENSE("Dual MIT/GPL");
