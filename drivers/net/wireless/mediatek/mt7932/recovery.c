/* SPDX-License-Identifier: GPL-2.0-only */
#include "mt7932.h"

static int mt_tx_retire(struct mt7932 *m);
static int mt_runtime_scheduler(struct mt7932 *m, bool publish);
static int mt_runtime_rebuild(struct mt7932 *m);
static void mt_runtime_publish(struct mt7932 *m);
static int mt_runtime_ack(struct mt7932 *m, u32 bit);
/* First runtime epoch only. Contracts: NEO_IRQ_SELECTION_AND_RUNTIME_HANDSHAKE
 * and NEO_RUNTIME_RING_REBUILD_AND_OBSERVED_PRECAL.
 */

static int mt_tx_retire(struct mt7932 *m)
{
	unsigned long deadline = jiffies + msecs_to_jiffies(500);
	unsigned int i;
	bool pending;

	do {
		pending = false;
		for (i = 0; i < ARRAY_SIZE(m->tx); i++) {
			mt_tx_clean(&m->tx[i]);
			pending |= !!m->tx[i].queued;
		}
		if (!pending)
			return 0;
		usleep_range(100, 200);
	} while (time_before(jiffies, deadline));
	return -ETIMEDOUT;
}

static int mt_runtime_scheduler(struct mt7932 *m, bool publish)
{
	static const u32 offsets[] = {0x1c, 0x60, 0x64, 0x68, 0x6c, 0x70, 0x74, 0x0c};
	static const u32 values[] = {1, 0x76543210, 0xfedcba98, 0x11111000,
		0x11111000, 0x76543210, 0xfedcba98, 0x003d1000};
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(offsets); i++) {
		if (publish)
			mt_write(m, H + offsets[i], values[i]);
		if (mt_read(m, H + offsets[i]) != values[i])
			return -EIO;
	}
	return 0;
}

static int mt_runtime_rebuild(struct mt7932 *m)
{
	static const u16 counts[] = {32, 0, 512, 32, 32, 224, 128, 512, 64};
	static const u16 rx_pf[] = {0, 0, 0x40, 0xc0, 0x100, 0x140, 0x180, 0x1c0, 0x200};
	static const u16 tx_pf[] = {0x240, 0x280, 0x2c0, 0x300, 0x340, 0x400,
		0x440, 0x480, 0x4c0, 0x580, 0x5c0, 0x600, 0x640, 0x700, 0x740,
		0, 0x780, 0x7c0, 0x800};
	unsigned int i, j;
	int ret;

	if (m->runtime_epoch || mt_read(m, W + 0x208) != 0x1010b870 ||
	    mt_read(m, W + 0x2b0) != 0x28c004df || (mt_read(m, H + 4) & BIT(28)))
		return -EINVAL;
	/* All geometry and device ownership must pass before any descriptor edit. */
	for (i = 0; i < ARRAY_SIZE(m->tx); i++) {
		struct mt7932_ring *q = &m->tx[i];
		u16 expected = i == 15 ? 0 : i < 15 ? 512 : i == 16 ? 256 : i == 17 ? 24 : 16;

		mt_tx_clean(q);
		if (q->count != expected || q->queued || (q->count && !q->desc))
			return -EBUSY;
	}
	for (i = 0; i < ARRAY_SIZE(m->rx); i++) {
		struct mt7932_ring *q = &m->rx[i];

		if (q->count != counts[i] || (q->count &&
		    (!q->desc || !q->buffers || q->slots != q->count)))
			return -EINVAL;
	}
	for (i = 0; i < ARRAY_SIZE(m->tx); i++)
		for (j = 0; j < m->tx[i].count; j++)
			m->tx[i].desc[j].control &= cpu_to_le32(~DONE);
	for (i = 0; i < ARRAY_SIZE(m->rx); i++) {
		struct mt7932_ring *q = &m->rx[i];

		for (j = 0; j < q->count; j++) {
			struct mt7932_desc *d = &q->desc[j];
			dma_addr_t addr = q->buffers_dma + j * RX_STRIDE;

			d->address = cpu_to_le32(lower_32_bits(addr));
			d->second = cpu_to_le32(upper_32_bits(addr) & 15);
			d->info = 0;
			d->control = cpu_to_le32((le32_to_cpu(d->control) & (BIT(30) | 0xffff)) |
						(MT7932_RX_CAPACITY << 16));
		}
		q->head = q->tail = 0;
		q->queued = q->count;
	}
	dma_wmb();
	mt_rmw(m, W + 0x208, BIT(0) | BIT(2) | BIT(15) | BIT(21) | BIT(27) | BIT(28), 0);
	ret = mt_poll(m, W + 0x208, BIT(1) | BIT(3), 0, 100000);
	if (ret)
		return ret;
	if (mt_read(m, W + 0x208) != 0x00103870)
		return -EIO;
	for (i = 0; i < ARRAY_SIZE(m->tx); i++) {
		struct mt7932_ring *q = &m->tx[i];

		if (!q->count)
			continue;
		/* Final empty runtime TX image, no live submissions in this epoch. */
		for (j = 0; j < q->count; j++)
			q->desc[j].control = 0;
		dma_wmb();
		mt_write(m, q->reg, q->dma);
		mt_write(m, q->reg + 8, 0);
		mt_write(m, q->reg + 4, q->count);
	}
	for (i = 0; i < ARRAY_SIZE(m->rx); i++) {
		struct mt7932_ring *q = &m->rx[i];

		if (!q->count)
			continue;
		mt_write(m, q->reg, q->dma);
		mt_write(m, q->reg + 8, q->count - 1);
		mt_write(m, q->reg + 4, q->count);
	}
	mt_write(m, W + 0x208, 0x00103870);
	if (mt_read(m, W + 0x208) != 0x00103870)
		return -EIO;
	for (i = 0; i < ARRAY_SIZE(m->rx); i++)
		if (m->rx[i].count)
			mt_write(m, W + 0x680 + 4 * i, (rx_pf[i] << 16) | (i == 2 ? 8 : 4));
	for (i = 0; i < ARRAY_SIZE(m->tx); i++)
		if (m->tx[i].count)
			mt_write(m, W + 0x600 + 4 * i, (tx_pf[i] << 16) |
				 (i == 4 || i == 8 || i == 12 ? 12 : 4));
	mt_write(m, W + 0x20c, U32_MAX);
	if (mt_read(m, W + 0x2b0) != 0x28c004df)
		return -EIO;
	mt_write(m, W + 0x208, 0x5030b870);
	if ((mt_read(m, W + 0x208) & ~(BIT(1) | BIT(3))) != 0x5030b870)
		return -EIO;
	dma_wmb();
	mt_write(m, W + 0x208, 0x5030b875);
	mt_write(m, W + 0x298, 0x0c);
	mt_write(m, C + 0x38, 0x13);
	mt_write(m, W + 0x2f0, 0x8032800a);
	if ((mt_read(m, W + 0x208) & ~(BIT(1) | BIT(3))) != 0x5030b875 ||
	    mt_read(m, W + 0x298) != 0x0c || mt_read(m, C + 0x38) != 0x13 ||
	    mt_read(m, W + 0x2f0) != 0x8032800a ||
	    mt_read(m, m->tx[17].reg + 8) || mt_read(m, m->tx[17].reg + 12))
		return -EIO;
	for (i = 0; i < ARRAY_SIZE(m->tx); i++) {
		m->tx[i].head = m->tx[i].tail = 0;
		m->tx[i].used = 0;
	}
	m->runtime_epoch = true;
	return 0;
}

static void mt_runtime_publish(struct mt7932 *m)
{
	m->irq_mask = 0xeec8000d;
	mt_write(m, W + 0x200, m->irq_mask);
	mt_write(m, W + 0x204, m->irq_mask);
	mt_write(m, W + 0x1f4, 0xffff);
}

static int mt_runtime_ack(struct mt7932 *m, u32 bit)
{
	u32 value;
	int ret = readl_poll_timeout(m->bar + W + 0x1f0, value,
				    value == U32_MAX || (value & bit), 10, 500000);

	if (ret || value == U32_MAX)
		return ret ?: -ENODEV;
	mt_write(m, W + 0x1f0, bit);
	return 0;
}

int mt_recovery_gate(struct mt7932 *m)
{
	/* Original observed 25F84 CID6b bodies, not PL country tables.
	 * Explicit experiment; first two action2 field meanings remain opaque.
	 */
	static const u8 queue_config[3][32] = {
		{0x10,0x11,2,0,3,0,7,0,0x2f,0,2},
		{0xe0,0x6e,2,0,0x0f,0,0xff,3,0,0,3},
		{0x0f,0,1},
	};
	static const u8 recover[] = {3, 1, 0, 0};
	u64 start;
	unsigned int i, phase = 0;
	int ret;

	/* mt_runtime_start() completed configuration before entering this stage. */
	if (mt_transport_polled() || m->runtime_epoch)
		return -EINVAL;
	for (i = 0; i < ARRAY_SIZE(queue_config); i++) {
		ret = mt_request(m, 0x6b, true, true, false, queue_config[i], 32);
		if (ret)
			return ret;
	}
	ret = mt_tx_retire(m);
	if (ret)
		return ret;
	disable_irq(pci_irq_vector(m->pdev, 0));
	WRITE_ONCE(m->running, false);
	ret = mt_request_ext(m, 0xed, 0x81, true, true, false, recover, sizeof(recover));
	if (ret)
		goto out;
	ret = mt_tx_retire(m);
	if (ret)
		goto out;
	phase = 1;
	mt_write(m, W + 0x204, 0);
	ret = mt_runtime_ack(m, 4);
	if (ret)
		goto out;
	mt_write(m, 0x2108, 1);
	mt_runtime_publish(m);
	phase = 2;
	start = ktime_get_ns();
	mt_write(m, 0xe0010, 1);
	ret = mt_poll(m, 0xe0010, BIT(2), BIT(2), 500000);
	if (ret)
		goto out;
	while (ktime_get_ns() - start < 1692041)
		udelay(10);
	mt_write(m, W + 0x204, 0);
	while (ktime_get_ns() - start < 2259166)
		udelay(10);
	mt_write(m, 0xe0010, 2);
	ret = mt_poll(m, 0xe0010, BIT(2), 0, 500000);
	if (ret)
		goto out;
	phase = 3;
	ret = mt_runtime_ack(m, 8);
	if (ret)
		goto out;
	if (mt_read(m, W + 0x2b0) != 0x28c004df) {
		ret = -EIO;
		goto out;
	}
	phase = 4;
	ret = mt_runtime_scheduler(m, true);
	if (ret)
		goto out;
	phase = 5;
	ret = mt_runtime_rebuild(m);
	if (ret)
		goto out;
	phase = 6;
	mt_write(m, 0x2108, 2);
	mt_runtime_publish(m);
	mt_write(m, W + 0x204, 0);
	ret = mt_runtime_ack(m, 0x10);
	if (ret)
		goto out;
	phase = 7;
	mt_write(m, 0x2108, 8);
	mt_runtime_publish(m);
	mt_write(m, W + 0x204, 0);
	ret = mt_runtime_ack(m, 0x20);
	if (ret)
		goto out;
	mt_runtime_publish(m);
	phase = 8;
	ret = mt_runtime_scheduler(m, false);
out:
	if (ret) {
		mt_write(m, W + 0x204, 0);
		dev_err(&m->pdev->dev, "runtime transition phase=%u error=%d SW=%08x GLO=%08x EXT=%08x TX17=%u/%u\n",
			phase, ret, mt_read(m, W + 0x1f0), mt_read(m, W + 0x208),
			mt_read(m, W + 0x2b0), mt_read(m, m->tx[17].reg + 8),
			mt_read(m, m->tx[17].reg + 12));
	} else {
		WRITE_ONCE(m->running, true);
		dev_info(&m->pdev->dev, "RUNTIME_DMA_READY: full RX epoch, TX17 reset, firmware NORMAL_STATE\n");
	}
	enable_irq(pci_irq_vector(m->pdev, 0));
	if (!ret) {
		/* New read-only validation transaction on TX17 slot0, not pre-cal replay. */
		ret = mt_request(m, 0x8a, true, false, true, NULL, 0);
		if (!ret && (m->reply_length < 40 || m->reply[28] != 0xec ||
		    mt7932_capabilities(m->reply + 36, m->reply_length - 36) < 0))
			ret = -EPROTO;
		if (!ret)
			dev_info(&m->pdev->dev, "RUNTIME_QUERY_OK: TX17 capability response, IRQ=%llu\n",
				 m->interrupts);
	}
	return ret;
}
