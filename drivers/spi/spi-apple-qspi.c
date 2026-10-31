// SPDX-License-Identifier: GPL-2.0
/*
 * Apple QSPIMC controller
 *
 * QSPIMC is not register-compatible with the ordinary Apple SPIMC block.
 * It consumes packed transfer descriptors and payload words through a
 * command FIFO. This driver intentionally implements the PIO spi-mem path
 * only; that is sufficient for SPI NOR discovery and access.
 *
 * The boot NOR starts read-only. On the qualified boards (J613, J713,
 * J616s and the J700) root may opt in at runtime to bounded, verified
 * programs and 4 KiB erases inside the nvram partition; everything else
 * stays read-only. On T8140 data reads reach the whole NOR.
 */

#include <linux/bitfield.h>
#include <linux/clk.h>
#include <linux/completion.h>
#include <linux/interrupt.h>
#include <linux/iopoll.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm.h>
#include <linux/pm_runtime.h>
#include <linux/slab.h>
#include <linux/spi/spi.h>
#include <linux/spi/spi-mem.h>

#include "spi-apple-qspi-policy.h"

#define APPLE_QSPI_C_OFFSET		0x800
#define APPLE_QSPI_MIN_SIZE		(APPLE_QSPI_C_OFFSET + 0xa0)

/* P (queue) block. */
#define APPLE_QSPI_P_TXFIFO		0x18
#define APPLE_QSPI_P_RXFIFO		0x1c
#define APPLE_QSPI_P_QSTATUS		0x20
#define APPLE_QSPI_P_QSTATUS_TX_USED	GENMASK(15, 8)
#define APPLE_QSPI_P_QSTATUS_RX_AVAIL	GENMASK(31, 24)
#define APPLE_QSPI_P_IRQ_ENABLE		0x2c
#define APPLE_QSPI_P_IRQ_STATUS		0x30
#define APPLE_QSPI_P_VERSION		0x3c

/* C (command) block. */
#define APPLE_QSPI_C_CONTROL		0x00
#define APPLE_QSPI_C_CONTROL_START	BIT(0)
#define APPLE_QSPI_C_CONTROL_RESET	BIT(8)
#define APPLE_QSPI_C_MODE		0x04
#define APPLE_QSPI_C_MODE_CPHA		BIT(8)
#define APPLE_QSPI_C_MODE_CPOL		BIT(9)
#define APPLE_QSPI_C_PAYLOAD_COUNT	0x14
#define APPLE_QSPI_C_RX_COUNT		0x18
#define APPLE_QSPI_C_QTHRESHOLD		0x24
#define APPLE_QSPI_C_IRQ_ENABLE		0x28
#define APPLE_QSPI_C_IRQ_STATUS		0x2c
#define APPLE_QSPI_C_IRQ_COMPLETE	BIT(0)
#define APPLE_QSPI_C_QDEPTH		0x38
#define APPLE_QSPI_C_QDEPTH_TX		GENMASK(7, 0)
#define APPLE_QSPI_C_QDEPTH_RX		GENMASK(15, 8)
#define APPLE_QSPI_C_CONFIG_48		0x48
#define APPLE_QSPI_C_CONFIG_58		0x58
#define APPLE_QSPI_C_CONFIG_5C		0x5c
#define APPLE_QSPI_C_CLKDIV		0x64
#define APPLE_QSPI_C_CONFIG_68		0x68
#define APPLE_QSPI_C_CONFIG_6C		0x6c
#define APPLE_QSPI_C_CONFIG_7C		0x7c
#define APPLE_QSPI_C_CONFIG_80		0x80
#define APPLE_QSPI_C_CONFIG_84		0x84
#define APPLE_QSPI_C_CONFIG_90		0x90
#define APPLE_QSPI_C_CONFIG_94		0x94
#define APPLE_QSPI_C_CONFIG_98		0x98
#define APPLE_QSPI_C_CONFIG_9C		0x9c

/* Packed transfer descriptor, word 0. */
#define APPLE_QSPI_CMD_MODE		GENMASK(3, 0)
#define APPLE_QSPI_CMD_FRAME_A		BIT(4)
#define APPLE_QSPI_CMD_FRAME_B		BIT(5)
#define APPLE_QSPI_CMD_TAG		GENMASK(15, 12)
#define APPLE_QSPI_CMD_SHARED_COUNT	GENMASK(19, 16)
#define APPLE_QSPI_CMD_MULTI_COUNT	GENMASK(23, 20)
#define APPLE_QSPI_CMD_TURNAROUND	GENMASK(31, 24)
#define APPLE_QSPI_CMD_RX		BIT(3)

#define APPLE_QSPI_IRQ_MASK		0x3
#define APPLE_QSPI_MAX_DATA		4096
#define APPLE_QSPI_MAX_HZ		40000000
#define APPLE_QSPI_TIMEOUT_MS		1000
#define APPLE_QSPI_AUTOSUSPEND_MS		100

/* Per-SoC controller differences, from the compatible. */
struct apple_qspi_soc {
	/* T8140: data reads of any shape pass straight to the controller. */
	bool multi_io_reads;
	bool runtime_pm;
};

/*
 * A board whose NVRAM may be written: the controller compatible, the
 * machine, the controller aperture, the nvram partition the device tree
 * must describe, the boot NOR JEDEC ID and the SR1 block-protect bits its
 * firmware leaves set at boot.
 */
struct apple_qspi_board {
	const char *controller;
	const char *machine;
	resource_size_t base;
	u32 nvram_start;
	u32 nvram_size;
	u8 jedec[3];
	u8 bp_ok;
};

#define APPLE_QSPI_JEDEC_W25Q64		{ 0xef, 0x65, 0x17 }
#define APPLE_QSPI_JEDEC_W25Q16		{ 0xef, 0x65, 0x15 }
#define APPLE_QSPI_JEDEC_MX_2B27	{ 0xc2, 0x2b, 0x27 }

static const struct apple_qspi_board apple_qspi_boards[] = {
	{ "apple,t8122-qspi", "apple,j613", 0x2a1118000ULL,
	  0x700000, 0x100000, APPLE_QSPI_JEDEC_W25Q64, 0 },
	{ "apple,t8132-qspi", "apple,j713", 0x3ad118000ULL,
	  0x700000, 0x100000, APPLE_QSPI_JEDEC_W25Q64, 0 },
	/* Macronix; the ID macOS reports for the J616s boot NOR. */
	{ "apple,t6041-qspi", "apple,j616s", 0x429118000ULL,
	  0x700000, 0x100000, APPLE_QSPI_JEDEC_MX_2B27, 0 },
	/* iBoot leaves BP0 set on the J700, over the top 64 KiB. */
	{ "apple,t8140-qspi", "apple,j700", 0x385110000ULL,
	  0x24000, 0x1d0000, APPLE_QSPI_JEDEC_W25Q16, 0x1c },
};

static DEFINE_MUTEX(apple_qspi_policy_lock);
static bool read_only = true;
static bool runtime_ready;
static bool write_fault_latched;

static int apple_qspi_set_read_only(const char *val, const struct kernel_param *kp)
{
	bool value;
	int ret = kstrtobool(val, &value);

	if (ret)
		return ret;
	mutex_lock(&apple_qspi_policy_lock);
	/* Boot arguments and insmod cannot arm this driver before probe. */
	if (!value && (!runtime_ready || write_fault_latched))
		ret = -EPERM;
	else
		*(bool *)kp->arg = value;
	mutex_unlock(&apple_qspi_policy_lock);
	return ret;
}

static int apple_qspi_get_read_only(char *buf, const struct kernel_param *kp)
{
	int ret;

	mutex_lock(&apple_qspi_policy_lock);
	ret = param_get_bool(buf, kp);
	mutex_unlock(&apple_qspi_policy_lock);
	return ret;
}

static const struct kernel_param_ops apple_qspi_read_only_ops = {
	.set = apple_qspi_set_read_only,
	.get = apple_qspi_get_read_only,
};
module_param_cb(read_only, &apple_qspi_read_only_ops, &read_only, 0644);
MODULE_PARM_DESC(read_only, "Default true; root runtime opt-in for bounded NVRAM writes on qualified boards");

static bool apple_qspi_opcode_is_read(u8 opcode)
{
	switch (opcode) {
	case 0x03: /* READ */
	case 0x0b: /* FAST_READ */
	case 0x3b: /* READ_1_1_2 */
	case 0x6b: /* READ_1_1_4 */
	case 0x13: /* READ_4B */
	case 0x0c: /* FAST_READ_4B */
	case 0x05: /* RDSR */
	case 0x35: /* RDCR / RDSR2 */
	case 0x15: /* RDSR3 */
	case 0x70: /* RDFSR */
	case 0x5a: /* RDSFDP */
	case 0x9f: /* RDID */
		return true;
	default:
		return false;
	}
}

struct apple_qspi {
	/* Every write-path request goes through the verified NVRAM policy. */
	bool nvram_policy;
	/* T8140: data reads of any shape pass straight to the controller. */
	bool nvram_window;
	/* The nvram partition [window_start, window_end). */
	u64 window_start;
	u64 window_end;
	/* Expected JEDEC ID and the SR1 block-protect bits left by firmware. */
	u8 jedec[3];
	u8 bp_ok;
	bool nvram_write_candidate;
	bool write_fault;
	void __iomem *base;
	void __iomem *p;
	void __iomem *c;
	struct clk *clk;
	struct completion done;
	/* Protects the active transfer pointers shared with the IRQ handler. */
	spinlock_t lock;
	int irq;

	const u8 *tx;
	size_t tx_left;
	u8 *rx;
	size_t rx_left;
	u8 tag;
	u8 tx_depth;
	u8 rx_depth;
	bool active;
};

struct apple_qspi_op_shape {
	u8 width;
	u8 shared_count;
	u8 multi_count;
	u8 turnaround;
};

static inline u32 apple_qspi_p_read(struct apple_qspi *qspi, u32 reg)
{
	return readl_relaxed(qspi->p + reg);
}

static inline void apple_qspi_p_write(struct apple_qspi *qspi, u32 reg, u32 val)
{
	writel_relaxed(val, qspi->p + reg);
}

static inline u32 apple_qspi_c_read(struct apple_qspi *qspi, u32 reg)
{
	return readl_relaxed(qspi->c + reg);
}

static inline void apple_qspi_c_write(struct apple_qspi *qspi, u32 reg, u32 val)
{
	writel_relaxed(val, qspi->c + reg);
}

static int apple_qspi_width_mode(u8 width)
{
	switch (width) {
	case 1:
		return 1;
	case 2:
		return 2;
	case 4:
		return 3;
	default:
		return -EINVAL;
	}
}

static int apple_qspi_shape_op(const struct spi_mem_op *op,
			       struct apple_qspi_op_shape *shape)
{
	u32 dummy_cycles = 0;
	u8 width;

	if (op->cmd.nbytes != 1 || op->cmd.buswidth != 1 || op->cmd.dtr)
		return -EOPNOTSUPP;
	if ((op->addr.nbytes && op->addr.dtr) ||
	    (op->dummy.nbytes && op->dummy.dtr) ||
	    (op->data.nbytes && (op->data.dtr || op->data.ecc ||
				 op->data.swap16)))
		return -EOPNOTSUPP;

	if (op->data.nbytes)
		width = op->data.buswidth;
	else if (op->dummy.nbytes)
		width = op->dummy.buswidth;
	else if (op->addr.nbytes)
		width = op->addr.buswidth;
	else
		width = 1;

	if (apple_qspi_width_mode(width) < 0)
		return -EOPNOTSUPP;
	if (op->addr.nbytes && op->addr.buswidth != 1 &&
	    op->addr.buswidth != width)
		return -EOPNOTSUPP;
	if (op->dummy.nbytes && op->dummy.buswidth != width)
		return -EOPNOTSUPP;
	if (op->data.nbytes && op->data.buswidth != width)
		return -EOPNOTSUPP;

	shape->width = width;
	shape->shared_count = op->cmd.nbytes;
	shape->multi_count = 0;
	if (op->addr.nbytes) {
		if (op->addr.buswidth == 1)
			shape->shared_count += op->addr.nbytes;
		else
			shape->multi_count = op->addr.nbytes;
	}

	if (shape->shared_count > 15 || shape->multi_count > 15)
		return -EOPNOTSUPP;

	if (op->dummy.nbytes) {
		dummy_cycles = op->dummy.nbytes * 8 / op->dummy.buswidth;
		if (dummy_cycles > 255)
			return -EOPNOTSUPP;
	}
	shape->turnaround = dummy_cycles;

	return 0;
}

static bool apple_qspi_supports_op(struct spi_mem *mem,
				   const struct spi_mem_op *op)
{
	struct apple_qspi_op_shape shape;

	if (spi_get_chipselect(mem->spi, 0) || spi_get_csgpiod(mem->spi, 0))
		return false;
	if (op->addr.nbytes > 4 || op->data.nbytes > APPLE_QSPI_MAX_DATA)
		return false;
	if (apple_qspi_shape_op(op, &shape))
		return false;

	return spi_mem_default_supports_op(mem, op);
}

static int apple_qspi_adjust_op_size(struct spi_mem *mem, struct spi_mem_op *op)
{
	op->data.nbytes = min_t(unsigned int, op->data.nbytes, APPLE_QSPI_MAX_DATA);
	return 0;
}

static void apple_qspi_fill_tx(struct apple_qspi *qspi)
{
	u32 status, used, available;

	status = apple_qspi_p_read(qspi, APPLE_QSPI_P_QSTATUS);
	used = FIELD_GET(APPLE_QSPI_P_QSTATUS_TX_USED, status);
	available = used < qspi->tx_depth ? qspi->tx_depth - used : 0;

	while (available-- && qspi->tx_left) {
		u32 word = 0;
		unsigned int shift;

		for (shift = 0; shift < 32 && qspi->tx_left; shift += 8) {
			word |= (u32)*qspi->tx++ << shift;
			qspi->tx_left--;
		}
		apple_qspi_p_write(qspi, APPLE_QSPI_P_TXFIFO, word);
	}
}

static void apple_qspi_drain_rx(struct apple_qspi *qspi)
{
	u32 status, available;

	status = apple_qspi_p_read(qspi, APPLE_QSPI_P_QSTATUS);
	available = FIELD_GET(APPLE_QSPI_P_QSTATUS_RX_AVAIL, status);

	while (available-- && qspi->rx_left) {
		u32 word = apple_qspi_p_read(qspi, APPLE_QSPI_P_RXFIFO);
		unsigned int shift;

		for (shift = 0; shift < 32 && qspi->rx_left; shift += 8) {
			*qspi->rx++ = word >> shift;
			qspi->rx_left--;
		}
	}
}

static irqreturn_t apple_qspi_irq(int irq, void *data)
{
	struct apple_qspi *qspi = data;
	unsigned long flags;
	u32 p_status, c_status;
	bool complete_op = false;

	p_status = apple_qspi_p_read(qspi, APPLE_QSPI_P_IRQ_STATUS);
	c_status = apple_qspi_c_read(qspi, APPLE_QSPI_C_IRQ_STATUS);
	if (!p_status && !c_status)
		return IRQ_NONE;

	apple_qspi_p_write(qspi, APPLE_QSPI_P_IRQ_STATUS, p_status);
	apple_qspi_c_write(qspi, APPLE_QSPI_C_IRQ_STATUS, c_status);

	spin_lock_irqsave(&qspi->lock, flags);
	if (qspi->active) {
		apple_qspi_drain_rx(qspi);
		apple_qspi_fill_tx(qspi);
		if (c_status & APPLE_QSPI_C_IRQ_COMPLETE)
			complete_op = true;
	}
	spin_unlock_irqrestore(&qspi->lock, flags);

	if (complete_op)
		complete(&qspi->done);

	return IRQ_HANDLED;
}

static void apple_qspi_clear_irqs(struct apple_qspi *qspi)
{
	u32 status;

	status = apple_qspi_p_read(qspi, APPLE_QSPI_P_IRQ_STATUS);
	apple_qspi_p_write(qspi, APPLE_QSPI_P_IRQ_STATUS, status);
	status = apple_qspi_c_read(qspi, APPLE_QSPI_C_IRQ_STATUS);
	apple_qspi_c_write(qspi, APPLE_QSPI_C_IRQ_STATUS, status);
}

static void apple_qspi_init_config(struct apple_qspi *qspi)
{
	u32 val;

	apple_qspi_c_write(qspi, APPLE_QSPI_C_CONFIG_7C, 0);
	apple_qspi_c_write(qspi, APPLE_QSPI_C_CONFIG_80, 0);
	apple_qspi_c_write(qspi, APPLE_QSPI_C_CONFIG_58, 11);
	apple_qspi_c_write(qspi, APPLE_QSPI_C_CONFIG_5C, 10);
	apple_qspi_c_write(qspi, APPLE_QSPI_C_CONFIG_94, 0);
	apple_qspi_c_write(qspi, APPLE_QSPI_C_CONFIG_98, 0);
	apple_qspi_c_write(qspi, APPLE_QSPI_C_CONFIG_9C, 0);
	apple_qspi_c_write(qspi, APPLE_QSPI_C_CONFIG_68, 11);
	apple_qspi_c_write(qspi, APPLE_QSPI_C_CONFIG_6C, 10);

	val = apple_qspi_c_read(qspi, APPLE_QSPI_C_CONFIG_48);
	apple_qspi_c_write(qspi, APPLE_QSPI_C_CONFIG_48, val | 0x00300000);
	apple_qspi_c_write(qspi, APPLE_QSPI_C_QTHRESHOLD,
			   1 | ((qspi->rx_depth >> 1) << 8));
	apple_qspi_c_write(qspi, APPLE_QSPI_C_CONFIG_90, 4);
	apple_qspi_c_write(qspi, APPLE_QSPI_C_CONFIG_84, 0x00020000);
}

static int apple_qspi_hw_reset(struct apple_qspi *qspi)
{
	u32 val;
	int ret;

	apple_qspi_p_write(qspi, APPLE_QSPI_P_IRQ_ENABLE, 0);
	apple_qspi_c_write(qspi, APPLE_QSPI_C_IRQ_ENABLE, 0);
	apple_qspi_c_write(qspi, APPLE_QSPI_C_CONTROL,
			   APPLE_QSPI_C_CONTROL_RESET);
	ret = readl_poll_timeout(qspi->c + APPLE_QSPI_C_CONTROL, val,
				 !(val & APPLE_QSPI_C_CONTROL_RESET), 10, 10000);
	if (ret)
		return ret;

	apple_qspi_clear_irqs(qspi);
	apple_qspi_init_config(qspi);
	return 0;
}

static int apple_qspi_set_frequency(struct apple_qspi *qspi,
				    struct spi_mem *mem,
				    const struct spi_mem_op *op)
{
	unsigned long input = clk_get_rate(qspi->clk);
	u32 requested, divisor;

	requested = op->max_freq ?: mem->spi->max_speed_hz;
	requested = min_t(u32, requested, APPLE_QSPI_MAX_HZ);
	if (!input || !requested)
		return -EINVAL;

	divisor = DIV_ROUND_UP(input, requested);
	/* Never exceed the requested clock when it is below our range. */
	if (divisor > 256)
		return -EINVAL;
	divisor = max_t(u32, divisor, 2);
	apple_qspi_c_write(qspi, APPLE_QSPI_C_CLKDIV, divisor - 1);
	return 0;
}

static bool apple_qspi_poll_complete(struct apple_qspi *qspi)
{
	unsigned long flags;
	u32 queue_irqs, command_irqs;

	spin_lock_irqsave(&qspi->lock, flags);
	queue_irqs = apple_qspi_p_read(qspi, APPLE_QSPI_P_IRQ_STATUS);
	command_irqs = apple_qspi_c_read(qspi, APPLE_QSPI_C_IRQ_STATUS);
	apple_qspi_p_write(qspi, APPLE_QSPI_P_IRQ_STATUS, queue_irqs);
	apple_qspi_c_write(qspi, APPLE_QSPI_C_IRQ_STATUS, command_irqs);
	if (qspi->active) {
		apple_qspi_drain_rx(qspi);
		apple_qspi_fill_tx(qspi);
	}
	spin_unlock_irqrestore(&qspi->lock, flags);

	return (command_irqs & APPLE_QSPI_C_IRQ_COMPLETE) ||
	       try_wait_for_completion(&qspi->done);
}

static bool apple_qspi_wait_complete(struct apple_qspi *qspi)
{
	bool finished;

	/*
	 * T8140 can latch completion without delivering the wired IRQ, and
	 * the RX threshold IRQ does not reliably drain the FIFO. A jiffy
	 * between polls throttles reads to one FIFO per scheduler tick.
	 * Service the FIFOs at microsecond intervals while retaining the
	 * bounded transfer deadline and IRQ completion path.
	 */
	return !read_poll_timeout(apple_qspi_poll_complete, finished, finished,
				 20, APPLE_QSPI_TIMEOUT_MS * USEC_PER_MSEC,
				 false, qspi);
}

static int apple_qspi_transfer_op(struct spi_mem *mem,
			      const struct spi_mem_op *op)
{
	struct spi_controller *ctlr = mem->spi->controller;
	struct apple_qspi *qspi = spi_controller_get_devdata(ctlr);
	struct apple_qspi_op_shape shape;
	unsigned long flags;
	size_t header_len, tx_len;
	u8 *tx_buf, *p;
	u32 descriptor, mode;
	u32 p_status, c_status;
	int ret, retries;
	bool complete, tx_stream;

	ret = apple_qspi_shape_op(op, &shape);
	if (ret)
		return ret;

	header_len = op->cmd.nbytes + op->addr.nbytes;
	tx_len = header_len;
	if (op->data.dir == SPI_MEM_DATA_OUT)
		tx_len += op->data.nbytes;

	tx_buf = kmalloc(tx_len, GFP_KERNEL);
	if (!tx_buf)
		return -ENOMEM;

	p = tx_buf;
	*p++ = op->cmd.opcode;
	if (op->addr.nbytes) {
		int i;

		for (i = op->addr.nbytes - 1; i >= 0; i--)
			*p++ = op->addr.val >> (8 * i);
	}
	if (op->data.dir == SPI_MEM_DATA_OUT && op->data.nbytes)
		memcpy(p, op->data.buf.out, op->data.nbytes);

	ret = apple_qspi_set_frequency(qspi, mem, op);
	if (ret)
		goto out_free;

	mode = (mem->spi->mode & SPI_CPOL ? APPLE_QSPI_C_MODE_CPOL : 0) |
	       (mem->spi->mode & SPI_CPHA ? APPLE_QSPI_C_MODE_CPHA : 0);
	apple_qspi_c_write(qspi, APPLE_QSPI_C_MODE, mode);

	mode = apple_qspi_width_mode(shape.width);
	if (op->data.dir == SPI_MEM_DATA_IN)
		mode |= APPLE_QSPI_CMD_RX;
	/* The M3/M4 controller consumes TX-only commands as one byte stream. */
	tx_stream = qspi->nvram_policy && op->data.dir != SPI_MEM_DATA_IN;
	if (tx_stream) {
		shape.shared_count = 0;
		shape.multi_count = 0;
	}
	descriptor = FIELD_PREP(APPLE_QSPI_CMD_MODE, mode) |
		     APPLE_QSPI_CMD_FRAME_A | APPLE_QSPI_CMD_FRAME_B |
		     FIELD_PREP(APPLE_QSPI_CMD_TAG, qspi->tag++ & 0xf) |
		     FIELD_PREP(APPLE_QSPI_CMD_SHARED_COUNT,
				shape.shared_count) |
		     FIELD_PREP(APPLE_QSPI_CMD_MULTI_COUNT,
				shape.multi_count) |
		     FIELD_PREP(APPLE_QSPI_CMD_TURNAROUND,
				shape.turnaround);

	apple_qspi_p_write(qspi, APPLE_QSPI_P_IRQ_ENABLE, 0);
	apple_qspi_c_write(qspi, APPLE_QSPI_C_IRQ_ENABLE, 0);
	apple_qspi_clear_irqs(qspi);
	reinit_completion(&qspi->done);

	spin_lock_irqsave(&qspi->lock, flags);
	qspi->tx = tx_buf;
	qspi->tx_left = tx_len;
	qspi->rx = op->data.dir == SPI_MEM_DATA_IN ? op->data.buf.in : NULL;
	qspi->rx_left = op->data.dir == SPI_MEM_DATA_IN ? op->data.nbytes : 0;
	qspi->active = true;
	apple_qspi_p_write(qspi, APPLE_QSPI_P_TXFIFO, descriptor);
	apple_qspi_p_write(qspi, APPLE_QSPI_P_TXFIFO,
			   tx_stream ? tx_len : op->data.nbytes);
	apple_qspi_fill_tx(qspi);
	spin_unlock_irqrestore(&qspi->lock, flags);

	apple_qspi_c_write(qspi, APPLE_QSPI_C_IRQ_ENABLE,
			   APPLE_QSPI_IRQ_MASK);
	apple_qspi_p_write(qspi, APPLE_QSPI_P_IRQ_ENABLE,
			   APPLE_QSPI_IRQ_MASK);
	apple_qspi_c_write(qspi, APPLE_QSPI_C_CONTROL,
			   APPLE_QSPI_C_CONTROL_START);

	complete = apple_qspi_wait_complete(qspi);
	apple_qspi_p_write(qspi, APPLE_QSPI_P_IRQ_ENABLE, 0);
	apple_qspi_c_write(qspi, APPLE_QSPI_C_IRQ_ENABLE, 0);
	synchronize_irq(qspi->irq);

	spin_lock_irqsave(&qspi->lock, flags);
	for (retries = 100; qspi->rx_left && retries; retries--)
		apple_qspi_drain_rx(qspi);
	qspi->active = false;
	ret = qspi->tx_left || qspi->rx_left ? -EIO : 0;
	spin_unlock_irqrestore(&qspi->lock, flags);

	apple_qspi_c_write(qspi, APPLE_QSPI_C_CONTROL, 0);
	if (!complete) {
		p_status = apple_qspi_p_read(qspi, APPLE_QSPI_P_QSTATUS);
		c_status = apple_qspi_c_read(qspi, APPLE_QSPI_C_IRQ_STATUS);
		dev_err(&mem->spi->dev,
			"transfer timed out: p=%#x c=%#x pcnt=%#x prxcnt=%#x tx=%zu rx=%zu\n",
			p_status, c_status,
			apple_qspi_c_read(qspi, APPLE_QSPI_C_PAYLOAD_COUNT),
			apple_qspi_c_read(qspi, APPLE_QSPI_C_RX_COUNT),
			qspi->tx_left, qspi->rx_left);
		ret = -ETIMEDOUT;
		if (apple_qspi_hw_reset(qspi))
			dev_err(&mem->spi->dev,
				"controller reset failed after timeout\n");
	} else if (ret) {
		dev_err(&mem->spi->dev,
			"transfer completed with %zu tx and %zu rx bytes pending\n",
			qspi->tx_left, qspi->rx_left);
	}

out_free:
	kfree(tx_buf);
	return ret;
}

/* Private transfers used by the serialized NVRAM write policy. */
static int apple_qspi_read_register(struct spi_mem *mem, u8 opcode, u8 *buf, u32 len)
{
	struct spi_mem_op op = SPI_MEM_OP(SPI_MEM_OP_CMD(opcode, 1),
		SPI_MEM_OP_NO_ADDR, SPI_MEM_OP_NO_DUMMY,
		SPI_MEM_OP_DATA_IN(len, buf, 1));

	return apple_qspi_transfer_op(mem, &op);
}

static int apple_qspi_command(struct spi_mem *mem, u8 opcode)
{
	struct spi_mem_op op = SPI_MEM_OP(SPI_MEM_OP_CMD(opcode, 1),
		SPI_MEM_OP_NO_ADDR, SPI_MEM_OP_NO_DUMMY, SPI_MEM_OP_NO_DATA);

	return apple_qspi_transfer_op(mem, &op);
}

static int apple_qspi_read_array(struct spi_mem *mem, u64 addr, u8 *buf, u32 len)
{
	struct spi_mem_op op = SPI_MEM_OP(SPI_MEM_OP_CMD(0x03, 1),
		SPI_MEM_OP_ADDR(3, addr, 1), SPI_MEM_OP_NO_DUMMY,
		SPI_MEM_OP_DATA_IN(len, buf, 1));

	return apple_qspi_transfer_op(mem, &op);
}

/*
 * Winbond parts have three status registers (05h, 35h, 15h). On Macronix
 * parts 35h is EQIO, which switches the flash to QPI mode, so only the
 * status (05h) and configuration (15h) registers are read there.
 */
static int apple_qspi_status(struct spi_mem *mem, u8 mfr, u8 sr[3])
{
	static const u8 winbond[] = { 0x05, 0x35, 0x15 };
	static const u8 macronix[] = { 0x05, 0x15 };
	const u8 *opcode = winbond;
	int i, n = ARRAY_SIZE(winbond), ret;

	if (mfr == APPLE_QSPI_MFR_MACRONIX) {
		opcode = macronix;
		n = ARRAY_SIZE(macronix);
	}
	sr[2] = 0;
	for (i = 0; i < n; i++) {
		ret = apple_qspi_read_register(mem, opcode[i], &sr[i], 1);
		if (ret)
			return ret;
	}
	return 0;
}

static int apple_qspi_nvram_modify(struct spi_mem *mem, const struct spi_mem_op *op)
{
	struct apple_qspi *qspi = spi_controller_get_devdata(mem->spi->controller);
	unsigned long deadline;
	u8 id[3], before[3], after[3], sr1;
	u8 *verify;
	u32 len = op->cmd.opcode == 0x20 ? APPLE_QSPI_ERASE_SIZE : op->data.nbytes;
	int i, ret, disable_ret;

	/* Allocate and validate EVERYTHING before a real WREN goes on the wire. */
	verify = kmalloc(len, GFP_KERNEL);
	if (!verify)
		return -ENOMEM;
	ret = apple_qspi_read_register(mem, 0x9f, id, sizeof(id));
	if (ret)
		goto out;
	if (memcmp(id, qspi->jedec, sizeof(id))) {
		ret = -ENODEV;
		goto out;
	}
	ret = apple_qspi_status(mem, id[0], before);
	if (ret)
		goto out;
	/* No unlocking. Conservatively refuse BP/CMP/WPS, BUSY/WEL or suspend. */
	if (id[0] == APPLE_QSPI_MFR_MACRONIX ?
	    !apple_qspi_mx_status_allows_write(before[0], qspi->bp_ok) :
	    !apple_qspi_status_allows_write(before[0], before[1], before[2],
					    qspi->bp_ok)) {
		ret = -EACCES;
		goto out;
	}
	ret = apple_qspi_read_array(mem, op->addr.val, verify, len);
	if (ret)
		goto out;
	if (op->cmd.opcode == 0x02) {
		const u8 *data = op->data.buf.out;

		for (i = 0; i < len; i++) {
			/* Reject attempts to turn programmed zeroes into ones. */
			if ((verify[i] & data[i]) != data[i]) {
				ret = -EINVAL;
				goto out;
			}
		}
		/* Identical pages need no physical program at all. */
		if (!memcmp(verify, data, len)) {
			ret = 0;
			goto out;
		}
	}

	ret = apple_qspi_command(mem, 0x06);
	if (ret)
		goto fault;
	ret = apple_qspi_read_register(mem, 0x05, &sr1, 1);
	if (ret || sr1 != (before[0] | 2)) {
		ret = ret ?: -EIO;
		goto fault;
	}
	ret = apple_qspi_transfer_op(mem, op);
	if (ret)
		goto fault;
	/* Controller completion is not NOR completion. Never replay on timeout. */
	deadline = jiffies + msecs_to_jiffies(3000);
	do {
		ret = apple_qspi_read_register(mem, 0x05, &sr1, 1);
		if (ret)
			goto fault;
		if (!(sr1 & 1))
			break;
		if (time_after_eq(jiffies, deadline)) {
			ret = -ETIMEDOUT;
			goto fault;
		}
		usleep_range(1000, 2000);
	} while (true);

	ret = apple_qspi_command(mem, 0x04);
	if (ret)
		goto fault;
	ret = apple_qspi_status(mem, id[0], after);
	if (ret)
		goto fault;
	if (memcmp(before, after, sizeof(before))) {
		ret = -EIO;
		goto fault;
	}
	ret = apple_qspi_read_array(mem, op->addr.val, verify, len);
	if (ret)
		goto fault;
	if (op->cmd.opcode == 0x02) {
		if (memcmp(verify, op->data.buf.out, len)) {
			ret = -EIO;
			goto fault;
		}
	} else {
		for (i = 0; i < len; i++) {
			if (verify[i] != 0xff) {
				ret = -EIO;
				goto fault;
			}
		}
	}
	goto out;

fault:
	/* Best effort to clear WEL; a latched fault cannot rearm this module instance. */
	disable_ret = apple_qspi_command(mem, 0x04);
	qspi->write_fault = true;
	write_fault_latched = true;
	read_only = true;
	dev_err(&mem->spi->dev,
		"NVRAM write fault %d (WRDI %d); read-only latched, do not retry\n",
		ret, disable_ret);
out:
	kfree(verify);
	return ret;
}

static int apple_qspi_exec_op(struct spi_mem *mem, const struct spi_mem_op *op)
{
	struct apple_qspi *qspi = spi_controller_get_devdata(mem->spi->controller);
	struct apple_qspi_op_shape shape;
	const char *denial = "modify-status";
	int ret;

	if (!apple_qspi_supports_op(mem, op) || apple_qspi_shape_op(op, &shape))
		return -EOPNOTSUPP;
	mutex_lock(&apple_qspi_policy_lock);
	/* 35h is EQIO, not a register read, on Macronix parts. */
	if (op->cmd.opcode == 0x35 &&
	    qspi->jedec[0] == APPLE_QSPI_MFR_MACRONIX) {
		ret = -EOPNOTSUPP;
		goto out;
	}
	if (op->data.dir == SPI_MEM_DATA_IN && op->data.nbytes &&
	    apple_qspi_opcode_is_read(op->cmd.opcode)) {
		ret = apple_qspi_transfer_op(mem, op);
		goto out;
	}
	ret = -EACCES;
	if (read_only || !qspi->nvram_write_candidate || qspi->write_fault) {
		denial = read_only ? "read-only" :
			 !qspi->nvram_write_candidate ? "layout" : "write-fault";
		goto out;
	}
	/* Single-wire, three-byte addressing only: no mode/status/OTP commands. */
	if (shape.width != 1 || op->addr.buswidth > 1 || op->dummy.nbytes) {
		denial = "shape";
		goto out;
	}
	if (!apple_qspi_mutation_allowed(op->cmd.opcode, op->addr.nbytes,
					op->addr.val, op->data.nbytes,
					op->data.dir == SPI_MEM_DATA_OUT,
					qspi->window_start, qspi->window_end)) {
		denial = "mutation-policy";
		goto out;
	}
	/* SPI-NOR's WREN/WRDI are virtual. WEL is never left armed by a request. */
	if (op->cmd.opcode == 0x06 || op->cmd.opcode == 0x04) {
		ret = 0;
		goto out;
	}
	ret = apple_qspi_nvram_modify(mem, op);
out:
	if (ret == -EACCES)
		dev_warn_ratelimited(&mem->spi->dev,
			"NVRAM denied: reason=%s opcode=%02x addr=%#llx addr_bytes=%u len=%u width=%u addr_width=%u dummy=%u dir=%u ro=%u candidate=%u fault=%u\n",
			denial, op->cmd.opcode, (unsigned long long)op->addr.val,
			op->addr.nbytes, op->data.nbytes, shape.width,
			op->addr.buswidth, op->dummy.nbytes, op->data.dir,
			read_only, qspi->nvram_write_candidate, qspi->write_fault);
	mutex_unlock(&apple_qspi_policy_lock);
	return ret;
}

static int apple_qspi_mem_exec_op(struct spi_mem *mem, const struct spi_mem_op *op)
{
	struct spi_controller *ctlr = mem->spi->controller;
	struct apple_qspi *qspi = spi_controller_get_devdata(ctlr);
	int ret;

	/* T8140 keeps its multi-line data reads; spi-mem serializes them. */
	if (!qspi->nvram_policy || (qspi->nvram_window &&
				    op->data.dir == SPI_MEM_DATA_IN &&
				    op->data.nbytes))
		ret = apple_qspi_transfer_op(mem, op);
	else
		ret = apple_qspi_exec_op(mem, op);
	/* spi-mem drops its runtime reference with pm_runtime_put(). */
	if (ctlr->auto_runtime_pm)
		pm_runtime_mark_last_busy(ctlr->dev.parent);
	return ret;
}

static const struct spi_controller_mem_ops apple_qspi_mem_ops = {
	.adjust_op_size = apple_qspi_adjust_op_size,
	.supports_op = apple_qspi_supports_op,
	.exec_op = apple_qspi_mem_exec_op,
};

/* Keep write qualification tied to the board and its controller aperture. */
static const struct apple_qspi_board *
apple_qspi_nvram_write_board(struct device_node *node, resource_size_t base)
{
	const struct apple_qspi_board *board;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(apple_qspi_boards); i++) {
		board = &apple_qspi_boards[i];
		if (of_device_is_compatible(node, board->controller) &&
		    of_machine_is_compatible(board->machine) &&
		    base == board->base)
			return board;
	}
	return NULL;
}

/*
 * The one "nvram" partition of the flash on CS0, 4 KiB aligned and within
 * three-byte addressing, since the policy only admits three-byte programs
 * and erases. Without it, no program or erase is admitted.
 */
static void apple_qspi_nvram_window(struct device *dev, struct apple_qspi *qspi)
{
	struct device_node *flash, *parts, *part;
	const char *label;
	u32 reg[2], cells[2], cs;
	u64 start = 0, end = 0;
	int found = 0;

	flash = of_get_child_by_name(dev->of_node, "flash");
	if (!flash)
		goto out;
	if (!of_device_is_compatible(flash, "jedec,spi-nor") ||
	    of_property_read_u32(flash, "reg", &cs) || cs) {
		of_node_put(flash);
		goto out;
	}
	parts = of_get_child_by_name(flash, "partitions");
	of_node_put(flash);
	if (!parts)
		goto out;
	if (of_device_is_compatible(parts, "fixed-partitions") &&
	    !of_property_read_u32(parts, "#address-cells", &cells[0]) &&
	    !of_property_read_u32(parts, "#size-cells", &cells[1]) &&
	    cells[0] == 1 && cells[1] == 1) {
		for_each_available_child_of_node(parts, part) {
			if (of_property_read_string(part, "label", &label) ||
			    strcmp(label, "nvram"))
				continue;
			found++;
			if (of_property_count_u32_elems(part, "reg") == 2 &&
			    !of_property_read_u32_array(part, "reg", reg, 2)) {
				start = reg[0];
				end = start + reg[1];
			}
		}
	}
	of_node_put(parts);
	if (found != 1 || start >= end || end > 1ULL << 24 ||
	    !IS_ALIGNED(start, APPLE_QSPI_ERASE_SIZE) ||
	    !IS_ALIGNED(end, APPLE_QSPI_ERASE_SIZE))
		start = end = 0;
out:
	qspi->window_start = start;
	qspi->window_end = end;
	if (start < end)
		dev_dbg(dev, "nvram partition [%#llx, %#llx)\n", start, end);
	else
		dev_dbg(dev, "boot NOR read-only: no usable nvram partition\n");
}

static int apple_qspi_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	const struct apple_qspi_board *board;
	const struct apple_qspi_soc *soc;
	struct spi_controller *ctlr;
	struct apple_qspi *qspi;
	struct resource *res;
	u32 depths, version;
	int irq, ret;

	soc = of_device_get_match_data(dev);
	if (!soc)
		return -ENODEV;

	ctlr = devm_spi_alloc_host(dev, sizeof(*qspi));
	if (!ctlr)
		return -ENOMEM;

	qspi = spi_controller_get_devdata(ctlr);
	qspi->nvram_policy = true;
	qspi->nvram_window = soc->multi_io_reads;
	apple_qspi_nvram_window(dev, qspi);
	if (of_device_is_compatible(dev->of_node, "apple,t8122-qspi") &&
	    !of_machine_is_compatible("apple,j613"))
		return -ENODEV;
	init_completion(&qspi->done);
	spin_lock_init(&qspi->lock);

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res || resource_size(res) < APPLE_QSPI_MIN_SIZE)
		return dev_err_probe(dev, -EINVAL,
				     "QSPIMC aperture is too small\n");

	/* The device tree must describe exactly the board's nvram partition. */
	board = apple_qspi_nvram_write_board(dev->of_node, res->start);
	if (board && qspi->window_start == board->nvram_start &&
	    qspi->window_end == (u64)board->nvram_start + board->nvram_size) {
		memcpy(qspi->jedec, board->jedec, sizeof(qspi->jedec));
		qspi->bp_ok = board->bp_ok;
		qspi->nvram_write_candidate = true;
	}
	qspi->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(qspi->base))
		return PTR_ERR(qspi->base);
	qspi->p = qspi->base;
	qspi->c = qspi->base + APPLE_QSPI_C_OFFSET;

	qspi->clk = devm_clk_get_enabled(dev, NULL);
	if (IS_ERR(qspi->clk))
		return dev_err_probe(dev, PTR_ERR(qspi->clk),
				     "failed to enable controller clock\n");

	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		return irq;
	qspi->irq = irq;
	ret = devm_request_irq(dev, irq, apple_qspi_irq, 0,
			       dev_name(dev), qspi);
	if (ret)
		return dev_err_probe(dev, ret, "failed to request IRQ\n");

	ret = apple_qspi_hw_reset(qspi);
	if (ret)
		return dev_err_probe(dev, ret, "controller reset timed out\n");

	depths = apple_qspi_c_read(qspi, APPLE_QSPI_C_QDEPTH);
	qspi->tx_depth = FIELD_GET(APPLE_QSPI_C_QDEPTH_TX, depths);
	qspi->rx_depth = FIELD_GET(APPLE_QSPI_C_QDEPTH_RX, depths);
	if (qspi->tx_depth < 3 || !qspi->rx_depth)
		return dev_err_probe(dev, -ENODEV,
				     "invalid FIFO depths tx=%u rx=%u\n",
				     qspi->tx_depth, qspi->rx_depth);
	apple_qspi_init_config(qspi);

	ctlr->num_chipselect = 1;
	ctlr->max_native_cs = 1;
	ctlr->mode_bits = SPI_CPHA | SPI_CPOL;
	if (qspi->nvram_window)
		ctlr->mode_bits |= SPI_RX_DUAL | SPI_RX_QUAD |
				   SPI_TX_DUAL | SPI_TX_QUAD;
	ctlr->bits_per_word_mask = SPI_BPW_MASK(8);
	ctlr->mem_ops = &apple_qspi_mem_ops;

	platform_set_drvdata(pdev, ctlr);
	/* Keep probe and SPI NOR discovery powered until registration finishes. */
	if (soc->runtime_pm && IS_ENABLED(CONFIG_PM)) {
		pm_runtime_get_noresume(dev);
		ret = devm_pm_runtime_set_active_enabled(dev);
		if (ret) {
			pm_runtime_put_noidle(dev);
			return ret;
		}
		/* Coalesce NOR read chunks and userspace NVRAM access bursts. */
		pm_runtime_set_autosuspend_delay(dev, APPLE_QSPI_AUTOSUSPEND_MS);
		pm_runtime_use_autosuspend(dev);
		ctlr->auto_runtime_pm = true;
	}
	ret = devm_spi_register_controller(dev, ctlr);
	if (ret) {
		if (ctlr->auto_runtime_pm)
			pm_runtime_put_noidle(dev);
		return dev_err_probe(dev, ret,
				     "failed to register SPI controller\n");
	}

	mutex_lock(&apple_qspi_policy_lock);
	runtime_ready = true;
	mutex_unlock(&apple_qspi_policy_lock);
	version = apple_qspi_p_read(qspi, APPLE_QSPI_P_VERSION);
	dev_dbg(dev, "QSPIMC version %#x, %lu Hz clock, txq=%u rxq=%u\n",
		version, clk_get_rate(qspi->clk), qspi->tx_depth,
		qspi->rx_depth);
	if (ctlr->auto_runtime_pm)
		pm_runtime_put_autosuspend(dev);
	return 0;
}

/* Fixed read-only diagnostic: no user-selected opcode, address or payload. */
static int apple_qspi_match_cs0(struct device *dev, const void *unused)
{
	return dev->bus == &spi_bus_type &&
		!spi_get_chipselect(to_spi_device(dev), 0);
}

static int apple_qspi_flash_read_reg(struct spi_mem *mem, u8 opcode,
				     u8 *buf, unsigned int len)
{
	struct spi_mem_op op = SPI_MEM_OP(SPI_MEM_OP_CMD(opcode, 1),
		SPI_MEM_OP_NO_ADDR, SPI_MEM_OP_NO_DUMMY,
		SPI_MEM_OP_DATA_IN(len, buf, 1));

	return spi_mem_exec_op(mem, &op);
}

static ssize_t flash_status_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct spi_controller *ctlr = dev_get_drvdata(dev);
	struct spi_mem mem = { };
	struct device *child;
	u8 *data;
	int ret;

	child = device_find_child(&ctlr->dev, NULL, apple_qspi_match_cs0);
	if (!child)
		return -ENODEV;
	mem.spi = to_spi_device(child);
	data = kzalloc(6, GFP_KERNEL);
	if (!data) {
		ret = -ENOMEM;
		goto out_put;
	}

	/* spi-mem supplies queue, bus and I/O serialization for each read.
	 * These are sequential samples, not an atomic protection snapshot.
	 * The JEDEC ID decides which registers exist: 35h is SR2 on Winbond
	 * parts but EQIO on Macronix ones, so it is sent to Winbond only.
	 */
	ret = apple_qspi_flash_read_reg(&mem, 0x9f, data, 3) ?:
	      apple_qspi_flash_read_reg(&mem, 0x05, data + 3, 1);
	if (ret)
		goto out_free;
	switch (data[0]) {
	case 0xef:
		ret = apple_qspi_flash_read_reg(&mem, 0x35, data + 4, 1) ?:
		      apple_qspi_flash_read_reg(&mem, 0x15, data + 5, 1);
		if (!ret)
			ret = sysfs_emit(buf, "jedec=%02x%02x%02x sr1=%02x sr2=%02x sr3=%02x\n",
					 data[0], data[1], data[2], data[3],
					 data[4], data[5]);
		break;
	case APPLE_QSPI_MFR_MACRONIX:
		ret = apple_qspi_flash_read_reg(&mem, 0x15, data + 4, 1);
		if (!ret)
			ret = sysfs_emit(buf, "jedec=%02x%02x%02x sr=%02x cr=%02x\n",
					 data[0], data[1], data[2], data[3],
					 data[4]);
		break;
	default:
		ret = sysfs_emit(buf, "jedec=%02x%02x%02x sr1=%02x\n",
				 data[0], data[1], data[2], data[3]);
		break;
	}
out_free:
	kfree(data);
out_put:
	put_device(child);
	return ret;
}
static DEVICE_ATTR_RO(flash_status);

static struct attribute *apple_qspi_attrs[] = {
	&dev_attr_flash_status.attr,
	NULL,
};
ATTRIBUTE_GROUPS(apple_qspi);

/* No IRQ handler may read the block while genpd has it powered off. */
static int apple_qspi_runtime_suspend(struct device *dev)
{
	struct spi_controller *ctlr = dev_get_drvdata(dev);
	struct apple_qspi *qspi = spi_controller_get_devdata(ctlr);

	disable_irq(qspi->irq);
	return 0;
}

static int apple_qspi_runtime_resume(struct device *dev)
{
	struct spi_controller *ctlr = dev_get_drvdata(dev);
	struct apple_qspi *qspi = spi_controller_get_devdata(ctlr);
	int ret;

	ret = apple_qspi_hw_reset(qspi);
	if (ret)
		return ret;
	enable_irq(qspi->irq);
	return 0;
}

static int apple_qspi_suspend(struct device *dev)
{
	struct spi_controller *ctlr = dev_get_drvdata(dev);
	int ret;

	ret = spi_controller_suspend(ctlr);
	if (ret || !ctlr->auto_runtime_pm)
		return ret;
	ret = pm_runtime_force_suspend(dev);
	if (ret)
		spi_controller_resume(ctlr);
	return ret;
}

/*
 * The controller loses its configuration when its power domain is gated
 * during system sleep. Restore it before the first operation after resume.
 */
static int apple_qspi_resume(struct device *dev)
{
	struct spi_controller *ctlr = dev_get_drvdata(dev);
	struct apple_qspi *qspi = spi_controller_get_devdata(ctlr);
	int ret;

	/*
	 * Reopen the controller even if the reset failed. Left suspended, it
	 * would fail every queued transfer until reboot. Runtime PM still
	 * refuses MMIO if the controller could not be brought back up.
	 */
	if (ctlr->auto_runtime_pm)
		ret = pm_runtime_force_resume(dev);
	else
		ret = apple_qspi_hw_reset(qspi);
	if (ret)
		dev_err(dev, "controller reset failed on resume: %d\n", ret);

	return spi_controller_resume(ctlr);
}

static const struct dev_pm_ops apple_qspi_pm_ops = {
	SYSTEM_SLEEP_PM_OPS(apple_qspi_suspend, apple_qspi_resume)
	RUNTIME_PM_OPS(apple_qspi_runtime_suspend, apple_qspi_runtime_resume, NULL)
};

static const struct apple_qspi_soc apple_qspi_single_io = { };

static const struct apple_qspi_soc apple_qspi_t8140 = {
	.multi_io_reads = true,
	.runtime_pm = true,
};

static const struct of_device_id apple_qspi_of_match[] = {
	{ .compatible = "apple,t8122-qspi", .data = &apple_qspi_single_io },
	{ .compatible = "apple,t8132-qspi", .data = &apple_qspi_single_io },
	{ .compatible = "apple,t6041-qspi", .data = &apple_qspi_single_io },
	{ .compatible = "apple,t8140-qspi", .data = &apple_qspi_t8140 },
	{}
};
MODULE_DEVICE_TABLE(of, apple_qspi_of_match);

static struct platform_driver apple_qspi_driver = {
	.probe = apple_qspi_probe,
	.driver = {
		.name = "apple-qspi",
		.dev_groups = apple_qspi_groups,
		.of_match_table = apple_qspi_of_match,
		.pm = pm_ptr(&apple_qspi_pm_ops),
	},
};
module_platform_driver(apple_qspi_driver);

MODULE_AUTHOR("The Asahi Linux Contributors");
MODULE_DESCRIPTION("Apple QSPIMC controller driver");
MODULE_LICENSE("GPL");
