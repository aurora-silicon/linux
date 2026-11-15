// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit tests for the Apple SPI controller's interrupt handler, which moves
 * the FIFO data of an interrupt-driven transfer itself. The registers are a
 * block of ordinary memory, so each test sets the FIFO levels and interrupt
 * flags the handler will read and checks what it leaves behind.
 *
 * Included by spi-apple.c.
 */

#include <kunit/test.h>

#define APPLE_SPI_TEST_REGS	0x200

static struct apple_spi *apple_spi_test_init(struct kunit *test)
{
	struct apple_spi *spi;
	void *regs;

	spi = kunit_kzalloc(test, sizeof(*spi), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, spi);
	regs = kunit_kzalloc(test, APPLE_SPI_TEST_REGS, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, regs);

	spi->regs = (void __iomem __force *)regs;
	init_completion(&spi->done);
	spin_lock_init(&spi->lock);
	spi->bytes_per_word = 1;
	return spi;
}

static void apple_spi_test_fifo_level(struct apple_spi *spi, u32 tx, u32 rx)
{
	reg_write(spi, APPLE_SPI_FIFOSTAT,
		  FIELD_PREP(APPLE_SPI_FIFOSTAT_LEVEL_TX, tx) |
		  FIELD_PREP(APPLE_SPI_FIFOSTAT_LEVEL_RX, rx));
}

/* A transmit threshold interrupt refills the FIFO without the thread. */
static void apple_spi_test_irq_refills_tx(struct kunit *test)
{
	struct apple_spi *spi = apple_spi_test_init(test);
	u8 *buf = kunit_kzalloc(test, 40, GFP_KERNEL);
	int i;

	KUNIT_ASSERT_NOT_NULL(test, buf);
	for (i = 0; i < 40; i++)
		buf[i] = i;

	/* 16 words primed, 8 of them already shifted out. */
	spi->active = true;
	spi->tx_ptr = buf + 16;
	spi->remaining_tx = 24;
	spi->xfer_flags = APPLE_SPI_XFER_TXCOMPLETE;
	apple_spi_test_fifo_level(spi, 8, 0);
	reg_write(spi, APPLE_SPI_IE_FIFO, APPLE_SPI_FIFO_TXTHRESH);
	reg_write(spi, APPLE_SPI_IF_FIFO, APPLE_SPI_FIFO_TXTHRESH);
	reg_write(spi, APPLE_SPI_IE_XFER, APPLE_SPI_XFER_TXCOMPLETE);

	KUNIT_EXPECT_EQ(test, apple_spi_irq(0, spi), IRQ_HANDLED);
	KUNIT_EXPECT_EQ(test, spi->remaining_tx, 16);
	KUNIT_EXPECT_PTR_EQ(test, spi->tx_ptr, (const void *)(buf + 24));
	KUNIT_EXPECT_EQ(test, reg_read(spi, APPLE_SPI_TXDATA), 23);
	KUNIT_EXPECT_TRUE(test, spi->active);
	KUNIT_EXPECT_FALSE(test, completion_done(&spi->done));
	KUNIT_EXPECT_EQ(test, reg_read(spi, APPLE_SPI_IE_FIFO), APPLE_SPI_FIFO_TXTHRESH);
	KUNIT_EXPECT_EQ(test, reg_read(spi, APPLE_SPI_IE_XFER), APPLE_SPI_XFER_TXCOMPLETE);
}

/* A receive threshold interrupt drains the FIFO into the buffer. */
static void apple_spi_test_irq_drains_rx(struct kunit *test)
{
	struct apple_spi *spi = apple_spi_test_init(test);
	u8 *buf = kunit_kzalloc(test, 20, GFP_KERNEL);
	int i;

	KUNIT_ASSERT_NOT_NULL(test, buf);
	spi->active = true;
	spi->rx_ptr = buf;
	spi->remaining_rx = 20;
	spi->xfer_flags = APPLE_SPI_XFER_RXCOMPLETE;
	apple_spi_test_fifo_level(spi, 0, 8);
	reg_write(spi, APPLE_SPI_RXDATA, 0xa5);
	reg_write(spi, APPLE_SPI_IE_FIFO, APPLE_SPI_FIFO_RXTHRESH);
	reg_write(spi, APPLE_SPI_IF_FIFO, APPLE_SPI_FIFO_RXTHRESH);
	reg_write(spi, APPLE_SPI_IE_XFER, APPLE_SPI_XFER_RXCOMPLETE);

	KUNIT_EXPECT_EQ(test, apple_spi_irq(0, spi), IRQ_HANDLED);
	KUNIT_EXPECT_EQ(test, spi->remaining_rx, 12);
	for (i = 0; i < 8; i++)
		KUNIT_EXPECT_EQ(test, buf[i], 0xa5);
	KUNIT_EXPECT_EQ(test, buf[8], 0);
	KUNIT_EXPECT_FALSE(test, completion_done(&spi->done));
	KUNIT_EXPECT_EQ(test, reg_read(spi, APPLE_SPI_IE_FIFO), APPLE_SPI_FIFO_RXTHRESH);
}

/* With one half complete, only the other half's interrupt stays enabled. */
static void apple_spi_test_irq_waits_for_both_halves(struct kunit *test)
{
	struct apple_spi *spi = apple_spi_test_init(test);
	u32 both = APPLE_SPI_XFER_TXCOMPLETE | APPLE_SPI_XFER_RXCOMPLETE;

	spi->active = true;
	spi->xfer_flags = both;
	reg_write(spi, APPLE_SPI_IE_XFER, both);
	reg_write(spi, APPLE_SPI_IF_XFER, APPLE_SPI_XFER_TXCOMPLETE);

	KUNIT_EXPECT_EQ(test, apple_spi_irq(0, spi), IRQ_HANDLED);
	KUNIT_EXPECT_EQ(test, spi->xfer_flags, APPLE_SPI_XFER_RXCOMPLETE);
	KUNIT_EXPECT_EQ(test, reg_read(spi, APPLE_SPI_IE_XFER), APPLE_SPI_XFER_RXCOMPLETE);
	KUNIT_EXPECT_EQ(test, reg_read(spi, APPLE_SPI_IE_FIFO), 0);
	KUNIT_EXPECT_TRUE(test, spi->active);
	KUNIT_EXPECT_FALSE(test, completion_done(&spi->done));
}

/* The thread is woken once, when both halves of the transfer are over. */
static void apple_spi_test_irq_completes_transfer(struct kunit *test)
{
	struct apple_spi *spi = apple_spi_test_init(test);
	u32 both = APPLE_SPI_XFER_TXCOMPLETE | APPLE_SPI_XFER_RXCOMPLETE;

	spi->active = true;
	spi->xfer_flags = both;
	reg_write(spi, APPLE_SPI_IE_XFER, both);
	reg_write(spi, APPLE_SPI_IF_XFER, both);

	KUNIT_EXPECT_EQ(test, apple_spi_irq(0, spi), IRQ_HANDLED);
	KUNIT_EXPECT_EQ(test, spi->xfer_flags, 0);
	KUNIT_EXPECT_FALSE(test, spi->active);
	KUNIT_EXPECT_TRUE(test, completion_done(&spi->done));
	KUNIT_EXPECT_EQ(test, reg_read(spi, APPLE_SPI_IE_XFER), 0);
	KUNIT_EXPECT_EQ(test, reg_read(spi, APPLE_SPI_IE_FIFO), 0);
}

/* A flag whose interrupt is disabled is not ours to handle. */
static void apple_spi_test_irq_ignores_disabled_flags(struct kunit *test)
{
	struct apple_spi *spi = apple_spi_test_init(test);

	spi->active = true;
	spi->remaining_tx = 4;
	spi->xfer_flags = APPLE_SPI_XFER_TXCOMPLETE;
	reg_write(spi, APPLE_SPI_IF_FIFO, APPLE_SPI_FIFO_TXTHRESH);
	reg_write(spi, APPLE_SPI_IF_XFER, APPLE_SPI_XFER_TXCOMPLETE);

	KUNIT_EXPECT_EQ(test, apple_spi_irq(0, spi), IRQ_NONE);
	KUNIT_EXPECT_EQ(test, spi->remaining_tx, 4);
	KUNIT_EXPECT_EQ(test, spi->xfer_flags, APPLE_SPI_XFER_TXCOMPLETE);
	KUNIT_EXPECT_TRUE(test, spi->active);
}

/*
 * An interrupt that arrives after the thread gave up on the transfer
 * touches neither the buffers nor the completion, and is switched off.
 */
static void apple_spi_test_irq_after_timeout(struct kunit *test)
{
	struct apple_spi *spi = apple_spi_test_init(test);
	u8 *buf = kunit_kzalloc(test, 8, GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, buf);
	spi->active = false;
	spi->rx_ptr = buf;
	spi->remaining_rx = 8;
	spi->xfer_flags = APPLE_SPI_XFER_RXCOMPLETE;
	apple_spi_test_fifo_level(spi, 0, 8);
	reg_write(spi, APPLE_SPI_RXDATA, 0x5a);
	reg_write(spi, APPLE_SPI_IE_FIFO, APPLE_SPI_FIFO_RXTHRESH);
	reg_write(spi, APPLE_SPI_IF_FIFO, APPLE_SPI_FIFO_RXTHRESH);

	KUNIT_EXPECT_EQ(test, apple_spi_irq(0, spi), IRQ_HANDLED);
	KUNIT_EXPECT_EQ(test, spi->remaining_rx, 8);
	KUNIT_EXPECT_PTR_EQ(test, spi->rx_ptr, (void *)buf);
	KUNIT_EXPECT_EQ(test, buf[0], 0);
	KUNIT_EXPECT_FALSE(test, completion_done(&spi->done));
	KUNIT_EXPECT_EQ(test, reg_read(spi, APPLE_SPI_IE_FIFO), 0);
	KUNIT_EXPECT_EQ(test, reg_read(spi, APPLE_SPI_IE_XFER), 0);
}

/* The TX-only fallback masks the interrupt and leaves refill to the thread. */
static void apple_spi_test_irq_wakes_tx_thread(struct kunit *test)
{
	struct apple_spi *spi = apple_spi_test_init(test);
	u8 buf[32] = {};

	spi->active = true;
	spi->threaded_tx = true;
	spi->tx_ptr = buf;
	spi->remaining_tx = sizeof(buf);
	spi->xfer_flags = APPLE_SPI_XFER_TXCOMPLETE;
	apple_spi_test_fifo_level(spi, 8, 0);
	reg_write(spi, APPLE_SPI_IE_FIFO, APPLE_SPI_FIFO_TXTHRESH);
	reg_write(spi, APPLE_SPI_IF_FIFO, APPLE_SPI_FIFO_TXTHRESH);
	reg_write(spi, APPLE_SPI_IE_XFER, APPLE_SPI_XFER_TXCOMPLETE);

	KUNIT_EXPECT_EQ(test, apple_spi_irq(0, spi), IRQ_HANDLED);
	KUNIT_EXPECT_EQ(test, spi->remaining_tx, sizeof(buf));
	KUNIT_EXPECT_TRUE(test, completion_done(&spi->done));
	KUNIT_EXPECT_EQ(test, reg_read(spi, APPLE_SPI_IE_FIFO), 0);
	KUNIT_EXPECT_EQ(test, reg_read(spi, APPLE_SPI_IE_XFER), 0);
}

static struct kunit_case apple_spi_test_cases[] = {
	KUNIT_CASE(apple_spi_test_irq_refills_tx),
	KUNIT_CASE(apple_spi_test_irq_wakes_tx_thread),
	KUNIT_CASE(apple_spi_test_irq_drains_rx),
	KUNIT_CASE(apple_spi_test_irq_waits_for_both_halves),
	KUNIT_CASE(apple_spi_test_irq_completes_transfer),
	KUNIT_CASE(apple_spi_test_irq_ignores_disabled_flags),
	KUNIT_CASE(apple_spi_test_irq_after_timeout),
	{}
};

static struct kunit_suite apple_spi_test_suite = {
	.name = "spi-apple-irq",
	.test_cases = apple_spi_test_cases,
};

kunit_test_suite(apple_spi_test_suite);
