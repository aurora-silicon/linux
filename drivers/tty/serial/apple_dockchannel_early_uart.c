// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * Apple DockChannel UART driver
 * Copyright The Asahi Linux Contributors
 *
 * Based on the DockChannel UART earlycon by Yureka Lilian <yureka@cyberchaos.dev>.
 */

#include <linux/console.h>
#include <linux/io.h>
#include <linux/libfdt.h>
#include <linux/of_fdt.h>
#include <linux/serial_core.h>

#include "apple_dockchannel.h"

static struct dockchannel_tx dockchannel_early_tx;
static bool dockchannel_early_never_wait;
/* The last character timed out: see dockchannel_wait_putc(). */
static bool dockchannel_early_no_reader;

static void dockchannel_uart_putc(struct uart_port *port, unsigned char c)
{
	if (dockchannel_early_never_wait)
		dockchannel_console_putc(port->membase, &dockchannel_early_tx, c);
	else
		dockchannel_wait_putc(port->membase, &dockchannel_early_no_reader, c);
}

/* The DT's DockChannel UART node asks for DOCKCHANNEL_NEVER_WAIT (flat DT). */
static bool __init dockchannel_uart_early_never_wait(void)
{
	const void *fdt = initial_boot_params;
	int node;

	if (!fdt)
		return false;
	node = fdt_node_offset_by_compatible(fdt, -1, "apple,dockchannel-uart");
	return node >= 0 && fdt_getprop(fdt, node, DOCKCHANNEL_NEVER_WAIT, NULL);
}

static void dockchannel_uart_early_write(struct console *con, const char *s,
					 unsigned int n)
{
	struct earlycon_device *dev = con->data;

	dockchannel_tx_begin(&dockchannel_early_tx);
	uart_console_write(&dev->port, s, n, dockchannel_uart_putc);
}

static int __init dockchannel_uart_early_setup(struct earlycon_device *device,
					       const char *opt)
{
	struct uart_port *port = &device->port;

	if (!port->mapbase)
		return -ENODEV;

	dockchannel_early_never_wait = dockchannel_uart_early_never_wait();
	device->con->write = dockchannel_uart_early_write;
	return 0;
}

OF_EARLYCON_DECLARE(dockchannel, "apple,dockchannel-uart",
		    dockchannel_uart_early_setup);
EARLYCON_DECLARE(dockchannel, dockchannel_uart_early_setup);
