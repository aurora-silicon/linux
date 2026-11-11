/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/*
 * Apple DockChannel UART: console output shared by the earlycon and the driver.
 */
#ifndef _APPLE_DOCKCHANNEL_H
#define _APPLE_DOCKCHANNEL_H

#include <linux/io.h>
#include <linux/processor.h>
#include <linux/types.h>

#define DOCKCHANNEL_TX8		0x04
#define DOCKCHANNEL_TX_FREE	0x14
#define DOCKCHANNEL_TX_RETRIES	10000

/*
 * Whether console and tty output never wait for a reader: the DT property
 * apple,tx-never-wait on the UART node, which only J775D (t6050-j775d.dts)
 * sets, where unattended boots run with nothing reading the channel. Every
 * other DockChannel Mac keeps waiting for room as before. Before widening
 * it (or making it the default), check on one other DockChannel Mac with a
 * slow attached reader (m1n1 proxy, USB debug) that long printk lines and tty
 * output still arrive whole.
 */
#define DOCKCHANNEL_NEVER_WAIT	"apple,tx-never-wait"

/* Waiting for room, at most DOCKCHANNEL_TX_RETRIES reads per character. */
static inline void dockchannel_wait_putc(void __iomem *data, unsigned char c)
{
	unsigned int retries = DOCKCHANNEL_TX_RETRIES;

	while (readl(data + DOCKCHANNEL_TX_FREE) == 0 && --retries)
		cpu_relax();
	if (!retries)
		return;

	writel(c, data + DOCKCHANNEL_TX8);
}

/*
 * With DOCKCHANNEL_NEVER_WAIT: the channel drains only while a debug USB host
 * reads it; with none, the TX FIFO stays full for good. So console output
 * never waits for room: each
 * write sends what TX_FREE says fits and drops the rest. One write reads
 * TX_FREE again when the room it read runs out, and stops reading once it
 * reads 0. Callers serialize writes, so the room read stays true.
 */
struct dockchannel_tx {
	u32 room;
	bool full;
};

static inline void dockchannel_tx_begin(struct dockchannel_tx *tx)
{
	tx->room = 0;
	tx->full = false;
}

static inline void dockchannel_console_putc(void __iomem *data,
					    struct dockchannel_tx *tx,
					    unsigned char c)
{
	if (!tx->room && !tx->full) {
		tx->room = readl(data + DOCKCHANNEL_TX_FREE);
		tx->full = !tx->room;
	}
	if (!tx->room)
		return;
	tx->room--;
	writel(c, data + DOCKCHANNEL_TX8);
}

#endif
