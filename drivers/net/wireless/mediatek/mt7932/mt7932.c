// SPDX-License-Identifier: GPL-2.0-only
/* J700 firmware-owned Wi-Fi: native transport and cfg80211 station support.
 * Covers firmware download and the NIC capability exchange.
 * All addresses here are BAR0 offsets, never chip/CPU physical addresses.
 */
#include <linux/mt7932.h>
#include "mt7932.h"

#include <linux/pm_wakeup.h>
#include <linux/suspend.h>

static int mt_ring_alloc(struct mt7932 *m, struct mt7932_ring *q,
			 unsigned int n, bool rx, unsigned int count);
static void mt_rx_post(struct mt7932_ring *q, unsigned int index);
static void mt_receive(struct mt7932 *m, const u8 *packet, size_t length);
static void mt_rx_drain(struct mt7932 *m, struct mt7932_ring *q);
static irqreturn_t mt_irq(int irq, void *data);
static void mt_service(struct mt7932 *m);
static irqreturn_t mt_irq_thread(int irq, void *data);
static int mt_submit(struct mt7932 *m, struct mt7932_ring *q, size_t length);
static int mt_command(struct mt7932 *m, u8 cid, const void *payload, size_t length);
static int mt_ownership(struct mt7932 *m);
static int mt_dma_setup(struct mt7932 *m);
static int mt_rom_gate(struct mt7932 *m);
static int mt_status(struct mt7932 *m, u8 cid, const void *payload, size_t length);
static int mt_scatter(struct mt7932 *m, const u8 *data, size_t length);
static int mt_download(struct mt7932 *m, const u8 *data, struct mt7932_region *r,
		       bool ram);
static int mt_secure_acquire(struct mt7932 *m);
static void mt_secure_release(struct mt7932 *m);
static int mt_otp_byte(struct mt7932 *m, u32 address, u8 *value);
static int mt_capability_gate(struct mt7932 *m);
static int mt_config_gate(struct mt7932 *m);
static int mt_board_gate(struct mt7932 *m);
static int mt_precal_gate(struct mt7932 *m);
static int mt_runtime_start(struct mt7932 *m);
static int mt_firmware_gate(struct mt7932 *m);
static int mt_initialize(struct mt7932 *m);
static int mt_probe(struct pci_dev *pdev, const struct pci_device_id *id);
static void mt_shutdown(struct pci_dev *pdev);

static bool legacy_irq_test;
module_param(legacy_irq_test, bool, 0444);
MODULE_PARM_DESC(legacy_irq_test, "Explicit diagnostic use of standard PCI INTx instead of MSI");
static bool poll_test;
module_param(poll_test, bool, 0444);
MODULE_PARM_DESC(poll_test, "Diagnostic ROM/firmware polling with endpoint IRQ generation masked");
static bool reset_test;
module_param(reset_test, bool, 0444);
MODULE_PARM_DESC(reset_test, "Qualify one FLR and fresh firmware epoch before publishing an interface, retaining old DMA");

bool mt_transport_polled(void)
{
	return poll_test;
}


bool mt_rf_allowed(struct mt7932 *m)
{
	return !READ_ONCE(m->stopping) && READ_ONCE(m->rf_ready) && !READ_ONCE(m->reg_pending) &&
	       !READ_ONCE(m->policy_failed);
}

u32 mt_read(struct mt7932 *m, u32 reg)
{
	return readl(m->bar + reg);
}

void mt_write(struct mt7932 *m, u32 reg, u32 value)
{
	writel(value, m->bar + reg);
}

void mt_rmw(struct mt7932 *m, u32 reg, u32 mask, u32 value)
{
	mt_write(m, reg, (mt_read(m, reg) & ~mask) | value);
}

int mt_poll(struct mt7932 *m, u32 reg, u32 mask, u32 expected, u32 timeout)
{
	u32 value;
	int ret = readl_poll_timeout(m->bar + reg, value,
				    value == U32_MAX || (value & mask) == expected,
				    1000, timeout);
	if (ret || value == U32_MAX) {
		dev_err(&m->pdev->dev, "poll %x got %08x wanted %08x/%08x\n",
			reg, value, expected, mask);
		return ret ?: -ENODEV;
	}
	return 0;
}

void *mt_alloc(struct mt7932 *m, size_t size, dma_addr_t *dma)
{
	void *p = dmam_alloc_coherent(&m->pdev->dev, size, dma, GFP_KERNEL);

	if (!p)
		return NULL;
	if (*dma >= BIT(27) || size > BIT(27) - *dma) {
		dev_err(&m->pdev->dev, "DMA allocation outside qualified 27-bit aperture\n");
		return NULL;
	}
	return p;
}

static int mt_ring_alloc(struct mt7932 *m, struct mt7932_ring *q,
			 unsigned int n, bool rx, unsigned int count)
{
	unsigned int i;

	q->count = count;
	q->reg = W + (rx ? 0x500 : 0x300) + 16 * n;
	q->desc = mt_alloc(m, count * sizeof(*q->desc), &q->dma);
	if (!q->desc)
		return -ENOMEM;
	for (i = 0; i < count; i++)
		q->desc[i].control = cpu_to_le32(DONE);
	q->slots = rx ? count : n == 16 ? 640 : n == 17 ? 1024 : 0;
	if (q->slots) {
		q->buffers = mt_alloc(m, q->slots * (rx ? RX_STRIDE : STRIDE),
				      &q->buffers_dma);
		if (!q->buffers)
			return -ENOMEM;
	}
	return 0;
}

static void mt_rx_post(struct mt7932_ring *q, unsigned int index)
{
	struct mt7932_desc *d = &q->desc[index];

	d->address = cpu_to_le32(q->buffers_dma + index * RX_STRIDE);
	d->second = 0;
	d->info = 0;
	d->control = cpu_to_le32(MT7932_RX_CAPACITY << 16);
}

static void mt_receive(struct mt7932 *m, const u8 *packet, size_t length)
{
	struct mt7932_event event;
	unsigned long flags;
	int ret;

	if (length >= 4 && (get_unaligned_le32(packet) >> 27) == 6) {
		mt_data_complete(m, packet, length);
		return;
	}
	ret = mt7932_event_parse(packet, length, &event);

	m->packets++;
	if (ret) {
		if (ret == -ENOMSG) {
			mt_packet_receive(m, packet, length);
			return;
		}
		dev_dbg_ratelimited(&m->pdev->dev, "RX framing result %d len %zu word %08x\n",
				     ret, length, length >= 4 ? get_unaligned_le32(packet) : 0);
		return;
	}
	spin_lock_irqsave(&m->response_lock, flags);
	if (event.eid == 0xee) {
		mt_link_event(m, &event);
		goto unlock;
	}
	if (event.eid == 0x0d) {
		mt_scan_event(m, &event);
		goto unlock;
	}
	if (event.eid == 0x11) {
		mt_bss_presence(m, &event);
		goto unlock;
	}
	if (event.eid == 0xd6) {
		ret = mt7932_cal_null(&m->cal_state, packet + 36,
				     event.length >= 36 ? event.length - 36 : 0,
				     m->smart_version);
		dev_info(&m->pdev->dev, "CAL_EVENT: seq=%u length=%zu result=%d credits=%u/%u\n",
			 event.seq, event.length, ret, m->cal_state.received, m->cal_state.expected);
		if (ret)
			complete(&m->cal_response);
		goto unlock;
	}
	if (event.eid == 0xd7) {
		/* The terminal peer gate and publication share response_lock. Join
		 * admitted calibration work, but never enqueue another producer
		 * behind retirement's final join. D6/owned responses still drain.
		 * An unexpected new request makes ownership uncertain: retain it
		 * for checked reset, rather than silently declaring retirement safe.
		 */
		if (m->disconnecting || m->link_failed) {
			m->cal_state.error = -EPROTO;
			m->link_failed = true;
			complete(&m->cal_response);
			goto unlock;
		}
		if (event.length < 52 || m->cal_request_count == ARRAY_SIZE(m->cal_requests)) {
			m->cal_state.error = -EPROTO;
			complete(&m->cal_response);
		} else {
			unsigned int at = (m->cal_request_head + m->cal_request_count) % ARRAY_SIZE(m->cal_requests);

			memcpy(m->cal_requests[at], packet + 36, 16);
			m->cal_request_seq[at] = event.seq;
			m->cal_request_count++;
			dev_info(&m->pdev->dev, "CAL_REQUEST: seq=%u words=%08x/%08x/%08x/%08x queued=%u\n",
				 event.seq, get_unaligned_le32(packet + 36), get_unaligned_le32(packet + 40),
				 get_unaligned_le32(packet + 44), get_unaligned_le32(packet + 48), m->cal_request_count);
			if (READ_ONCE(m->rf_ready))
				schedule_work(&m->cal_work);
		}
		goto unlock;
	}
	if (mt7932_event_matches(&event, m->waiting) && !m->reply_length) {
		memcpy(m->reply, packet, event.length);
		m->reply_length = event.length;
		complete(&m->response);
	} else {
		dev_dbg_ratelimited(&m->pdev->dev, "event eid=%02x seq=%u option=%02x len=%zu\n",
				     event.eid, event.seq, event.option, event.length);
	}
unlock:
	spin_unlock_irqrestore(&m->response_lock, flags);
}

static void mt_rx_drain(struct mt7932 *m, struct mt7932_ring *q)
{
	unsigned int budget = q->count;

	while (budget--) {
		u32 ctrl = le32_to_cpu(READ_ONCE(q->desc[q->tail].control));
		u32 length;

		if (!(ctrl & DONE))
			break;
		dma_rmb();
		length = (ctrl >> 16) & 0x3fff;
		if (length <= MT7932_RX_CAPACITY && (ctrl & BIT(30)))
			mt_receive(m, q->buffers + q->tail * RX_STRIDE, length);
		else
			dev_err_ratelimited(&m->pdev->dev, "RX length overflow %u\n", length);
		if (READ_ONCE(m->ram_config) && length >= 33 && length <= MT7932_RX_CAPACITY) {
			u8 *packet = q->buffers + q->tail * RX_STRIDE;

			/* Source-specific RAM-config acknowledgment before RX0 refill. */
			if (q == &m->rx[0] && packet[28] == 1 && packet[29] == m->waiting &&
			    !packet[30] && packet[32] == 1) {
				mt_write(m, W + 0x204, 0);
				mt_write(m, W + 0x200, 0x0c000001);
				mt_write(m, W + 0x204, poll_test ? 0 : IRQ_MASK);
				mt_write(m, W + 0x1f4, 0xffff);
			}
		}
		/* Runtime owns all N slots; initial download reserves one empty. */
		mt_rx_post(q, m->runtime_epoch ? q->tail : q->head);
		q->head = (q->head + 1) % q->count;
		q->tail = (q->tail + 1) % q->count;
		dma_wmb();
		mt_write(m, q->reg + 8,
			 m->runtime_epoch ? (q->head + q->count - 1) % q->count : q->head);
	}
}

static irqreturn_t mt_irq(int irq, void *data)
{
	struct mt7932 *m = data;

	if (!READ_ONCE(m->running))
		return IRQ_NONE;
	if (!(mt_read(m, W + 0x200) & m->irq_mask))
		return IRQ_NONE;
	mt_write(m, W + 0x204, 0);
	return IRQ_WAKE_THREAD;
}

static void mt_service(struct mt7932 *m)
{
	u32 causes = mt_read(m, W + 0x200) & m->irq_mask;
	unsigned int i;

	mt_data_clean(m);
	for (i = 0; i < ARRAY_SIZE(m->rx); i++)
		if (m->rx[i].count)
			mt_rx_drain(m, &m->rx[i]);
	if (causes & BIT(29)) {
		u32 sw = mt_read(m, W + 0x1f0);

		mt_write(m, W + 0x1f0, sw);
	}
	mt_write(m, W + 0x200, causes);
	if (READ_ONCE(m->running) && !poll_test)
		mt_write(m, W + 0x204, m->irq_mask);
}

static irqreturn_t mt_irq_thread(int irq, void *data)
{
	struct mt7932 *m = data;

	m->interrupts++;
	mt_service(m);
	return IRQ_HANDLED;
}

void mt_tx_clean(struct mt7932_ring *q)
{
	while (q->queued && (le32_to_cpu(READ_ONCE(q->desc[q->tail].control)) & DONE)) {
		dma_rmb();
		q->tail = (q->tail + 1) % q->count;
		q->queued--;
	}
}

static int mt_submit(struct mt7932 *m, struct mt7932_ring *q, size_t length)
{
	struct mt7932_desc *d;
	unsigned int retry;

	if (q->used >= q->slots || length > STRIDE || !length)
		return -ENOSPC;
	for (retry = 0; retry < 50; retry++) {
		mt_tx_clean(q);
		if (q->queued < q->count - 1)
			break;
		usleep_range(100, 200);
	}
	if (q->queued == q->count - 1)
		return -EBUSY;
	d = &q->desc[q->head];
	d->address = cpu_to_le32(q->buffers_dma + q->used * STRIDE);
	d->second = 0;
	d->info = 0;
	dma_wmb();
	d->control = cpu_to_le32((length << 16) | BIT(30));
	q->used++;
	q->head = (q->head + 1) % q->count;
	q->queued++;
	dma_wmb();
	mt_write(m, q->reg + 8, q->head);
	return 0;
}

/* One caller, one outstanding command, no retry after uncertain completion. */
int mt_request_ext(struct mt7932 *m, u8 cid, u8 ext, bool runtime, bool set, bool wait,
		      const void *payload, size_t length)
{
	struct mt7932_ring *q = &m->tx[17];
	unsigned long flags;
	int ret;

	if (READ_ONCE(m->stopping))
		return -ESHUTDOWN;
	if (q->used >= q->slots) {
		/* Command buffers can wrap only after every published descriptor has
		 * retired. Never reclaim by elapsed time or by a reply alone.
		 */
		mt_tx_clean(q);
		if (q->queued)
			return -ENOSPC;
		q->used = 0;
	}
	if (!++m->seq)
		m->seq = 1;
	ret = mt7932_envelope(q->buffers + q->used * STRIDE, STRIDE,
			      cid, ext, m->seq, runtime, set, wait, payload, length);
	if (ret < 0)
		return ret;
	reinit_completion(&m->response);
	spin_lock_irqsave(&m->response_lock, flags);
	m->waiting = wait ? m->seq : 0;
	m->reply_length = 0;
	spin_unlock_irqrestore(&m->response_lock, flags);
	ret = mt_submit(m, q, ret);
	if (!ret && wait) {
		if (poll_test) {
			unsigned long deadline = jiffies + msecs_to_jiffies(5000);

			do {
				mt_service(m);
				if (completion_done(&m->response))
					break;
				usleep_range(100, 200);
			} while (time_before(jiffies, deadline));
			if (!completion_done(&m->response))
				ret = -ETIMEDOUT;
		} else if (!wait_for_completion_timeout(&m->response, msecs_to_jiffies(5000))) {
			ret = -ETIMEDOUT;
		}
	}
	if (READ_ONCE(m->stopping))
		ret = -ESHUTDOWN;
	if (ret == -ETIMEDOUT) {
		/* Diagnostic only: never turn a polled late reply into IRQ success. */
		unsigned int i;
		u16 command;
		u32 mask, pending;
		unsigned int cap = pci_find_capability(m->pdev, PCI_CAP_ID_MSI);

		pci_read_config_word(m->pdev, PCI_COMMAND, &command);
		pci_read_config_dword(m->pdev, cap + PCI_MSI_MASK_64, &mask);
		pci_read_config_dword(m->pdev, cap + PCI_MSI_PENDING_64, &pending);
		dev_info(&m->pdev->dev, "timeout PCI command=%04x MSI mask=%08x pending=%08x\n",
			 command, mask, pending);

		dev_info(&m->pdev->dev, "timeout gates MAC=%08x WFDMA=%08x SW=%08x\n",
			 mt_read(m, 0x10188), mt_read(m, W + 0x204), mt_read(m, W + 0x1f4));
		disable_irq(pci_irq_vector(m->pdev, 0));
		for (i = 0; i < ARRAY_SIZE(m->rx); i++) {
			struct mt7932_ring *rx = &m->rx[i];

			if (!rx->count)
				continue;
			dev_info(&m->pdev->dev, "timeout RX%u ctrl=%08x CPU=%u DMA=%u\n", i,
				 le32_to_cpu(rx->desc[rx->tail].control),
				 mt_read(m, rx->reg + 8), mt_read(m, rx->reg + 12));
			if (le32_to_cpu(rx->desc[rx->tail].control) & DONE) {
				dma_rmb();
				print_hex_dump(KERN_INFO, "ROM RX prefix: ", DUMP_PREFIX_OFFSET,
					       16, 1, rx->buffers + rx->tail * RX_STRIDE, 40, false);
			}
			mt_rx_drain(m, rx);
		}
		dev_info(&m->pdev->dev, "late polled reply length=%zu eid=%02x (not IRQ success)\n",
			 m->reply_length, m->reply_length > 28 ? m->reply[28] : 0);
		enable_irq(pci_irq_vector(m->pdev, 0));
	}
	spin_lock_irqsave(&m->response_lock, flags);
	m->waiting = 0;
	spin_unlock_irqrestore(&m->response_lock, flags);
	if (ret)
		dev_err(&m->pdev->dev,
			"command %02x seq %u failed %d IRQ=%llu RX=%llu status=%08x TX=%u/%u\n",
			cid, m->seq, ret, m->interrupts, m->packets, mt_read(m, W + 0x200),
			mt_read(m, q->reg + 8), mt_read(m, q->reg + 12));
	return ret;
}

int mt_request(struct mt7932 *m, u8 cid, bool runtime, bool set, bool wait,
		      const void *payload, size_t length)
{
	return mt_request_ext(m, cid, 0, runtime, set, wait, payload, length);
}

static int mt_command(struct mt7932 *m, u8 cid, const void *payload, size_t length)
{
	return mt_request(m, cid, false, false, true, payload, length);
}

static int mt_ownership(struct mt7932 *m)
{
	unsigned int attempt;
	int ret = -ETIMEDOUT;

	mt_write(m, 0xe0018, 1);
	for (attempt = 0; attempt < 10; attempt++) {
		mt_write(m, 0xe0010, 2);
		ret = mt_poll(m, 0xe0010, BIT(2), 0, 50000);
		if (!ret || ret == -ENODEV)
			break;
	}
	if (ret)
		return ret;
	mt_write(m, 0xfe250, 0x70001846);
	udelay(2);
	if ((mt_read(m, 0xfe250) >> 16) != 0x7000)
		return -EIO;
	dev_info(&m->pdev->dev, "DRIVER_OWN_OK power=%08x\n", mt_read(m, 0xe00f0));
	return 0;
}

static int mt_dma_setup(struct mt7932 *m)
{
	static const u16 rx_counts[] = {32, 0, 512, 32, 32, 224, 128, 512, 64};
	static const u16 rx_prefetch[] = {0, 0, 0x40, 0xc0, 0x100, 0x140, 0x180, 0x1c0, 0x200};
	static const u16 tx_prefetch[] = {0x240, 0x280, 0x2c0, 0x300, 0x340, 0x400,
		0x440, 0x480, 0x4c0, 0x580, 0x5c0, 0x600, 0x640, 0x700, 0x740,
		0, 0x780, 0x7c0, 0x800};
	unsigned int i, j;
	int ret;
	u32 glo;

	mt_write(m, W + 0x204, 0);
	mt_write(m, 0x10188, poll_test ? 0 : 0xff);
	mt_rmw(m, W + 0x208, BIT(0) | BIT(2) | BIT(15) | BIT(21) | BIT(27) | BIT(28), 0);
	ret = mt_poll(m, W + 0x208, BIT(1) | BIT(3), 0, 100000);
	if (ret)
		return ret;
	mt_rmw(m, W + 0x2b0, BIT(6), 0);
	if (mt_read(m, H + 4) & BIT(28))
		return -EINVAL;
	mt_rmw(m, W + 0x100, BIT(4) | BIT(5), 0);
	mt_rmw(m, W + 0x100, 0, BIT(4) | BIT(5));
	mt_rmw(m, H + 0x1c, 0x0fff0fff, 1);
	mt_rmw(m, H + 0x10, 0xffff0000, 0x80000000);
	for (i = 0; i < 16; i++) {
		u32 quota = i < 4 ? 0x02000028 : i == 15 ? 0 :
			    !(i % 4) ? 0x01000014 : 0x01000028;

		mt_write(m, H + 0x20 + 4 * i, quota);
		if ((mt_read(m, H + 0x20 + 4 * i) & 0x0fff0fff) != quota)
			return -EIO;
	}
	mt_write(m, H + 0x60, 0x76543210);
	mt_write(m, H + 0x64, 0xfedcba98);
	mt_write(m, H + 0x68, 0x11111000);
	mt_write(m, H + 0x6c, 0x11111000);
	mt_write(m, H + 0x70, 0x76543210);
	mt_write(m, H + 0x74, 0xfedcba98);
	mt_rmw(m, H + 0x0c, 3 << 16, BIT(16));
	mt_rmw(m, H + 4, BIT(28), 0);
	for (i = 0; i < ARRAY_SIZE(m->tx); i++) {
		if (i == 15)
			continue;
		ret = mt_ring_alloc(m, &m->tx[i], i, false, i < 15 ? 512 : i == 16 ? 256 : i == 17 ? 24 : 16);
		if (ret)
			return ret;
	}
	for (i = 0; i < ARRAY_SIZE(m->rx); i++) {
		if (!rx_counts[i])
			continue;
		ret = mt_ring_alloc(m, &m->rx[i], i, true, rx_counts[i]);
		if (ret)
			return ret;
	}
	m->ipc = mt_alloc(m, 0x300, &m->ipc_dma);
	m->aux = mt_alloc(m, 0x320, &m->aux_dma);
	if (!m->ipc || !m->aux)
		return -ENOMEM;
	for (i = 0; i < ARRAY_SIZE(m->tx); i++) {
		struct mt7932_ring *q = &m->tx[i];

		if (!q->count)
			continue;
		mt_write(m, q->reg, q->dma);
		mt_write(m, q->reg + 8, 0);
		mt_write(m, q->reg + 4, q->count);
		mt_write(m, W + 0x600 + 4 * i, (tx_prefetch[i] << 16) | (i == 4 || i == 8 || i == 12 ? 12 : 4));
	}
	for (i = 0; i < ARRAY_SIZE(m->rx); i++) {
		struct mt7932_ring *q = &m->rx[i];

		if (!q->count)
			continue;
		mt_write(m, q->reg, q->dma);
		mt_write(m, q->reg + 8, 0);
		mt_write(m, q->reg + 4, q->count);
		for (j = 0; j < q->count - 1; j++)
			mt_rx_post(q, j);
		q->head = q->count - 1;
		dma_wmb();
		mt_write(m, q->reg + 8, q->head);
		mt_write(m, W + 0x680 + 4 * i, (rx_prefetch[i] << 16) | (i == 2 ? 8 : 4));
	}
	mt_write(m, W + 0x20c, U32_MAX);
	glo = (mt_read(m, W + 0x208) & ~(GENMASK(5, 4) | BIT(11) | BIT(13) | BIT(20))) |
		BIT(6) | BIT(12) | BIT(15) | BIT(21) | BIT(28) | BIT(30);
	mt_write(m, W + 0x208, glo);
	mt_rmw(m, W + 0x2b0, 0, BIT(6));
	/* All endpoint-visible storage and interrupt state now have an owner. */
	dma_wmb();
	WRITE_ONCE(m->dma_owned, true);
	pci_set_master(m->pdev);
	WRITE_ONCE(m->running, true);
	mt_rmw(m, W + 0x208, 0, BIT(0) | BIT(2));
	mt_write(m, W + 0x298, 0xc);
	mt_write(m, C + 0x38, 0x13);
	mt_write(m, W + 0x2f0, 0x8032800a);
	mt_rmw(m, 0x2120, 0, BIT(1));
	mt_write(m, W + 0x1f4, 0xffff);
	mt_write(m, W + 0x204, poll_test ? 0 : IRQ_MASK);
	dev_info(&m->pdev->dev, "DMA_PUBLISHED glo=%08x TX17=%pad RX0=%pad IPC=%pad\n",
		 mt_read(m, W + 0x208), &m->tx[17].dma, &m->rx[0].dma, &m->ipc_dma);
	return 0;
}

static int mt_rom_gate(struct mt7932 *m)
{
	static const u32 addresses[] = {0x70010204, 0x88000004};
	u8 request[12] = {};
	unsigned int i;
	int ret = mt_poll(m, 0xe00f0, 7, 1, 1000000);

	if (ret)
		return ret;
	mt_write(m, 0xfe254, 0x18051848);
	udelay(2);
	if ((mt_read(m, 0xfe254) >> 16) != 0x1805)
		return -EIO;
	dma_wmb();
	mt_write(m, 0x93c28, 1);
	mt_write(m, 0x93a38, 2);
	mt_write(m, 0x93a3c, 0x300);
	mt_write(m, 0x93a30, m->ipc_dma);
	mt_write(m, 0x93a54, m->aux_dma);
	if (mt_read(m, 0x93a30) != m->ipc_dma || mt_read(m, 0x93a54) != m->aux_dma)
		return -EIO;
	for (i = 0; i < ARRAY_SIZE(addresses); i++) {
		put_unaligned_le32(addresses[i], request + 4);
		ret = mt_command(m, 3, request, sizeof(request));
		if (ret)
			return ret;
		if (m->reply_length < 40 || m->reply[28] != 2 ||
		    get_unaligned_le32(m->reply + 32) != addresses[i])
			return -EPROTO;
		dev_info(&m->pdev->dev, "ROM_READ_OK %08x=%08x seq=%u IRQ=%llu\n",
			 addresses[i], get_unaligned_le32(m->reply + 36), m->seq, m->interrupts);
	}
	dev_info(&m->pdev->dev, "ROM_TRANSPORT_OK mode=%s IRQ=%llu (no firmware download or RF yet)\n",
		 poll_test ? "polled-test" : "interrupt", m->interrupts);
	return 0;
}

static int mt_status(struct mt7932 *m, u8 cid, const void *payload, size_t length)
{
	int ret = mt_command(m, cid, payload, length);

	if (ret)
		return ret;
	if (m->reply_length < 33 || m->reply[28] != 1 || m->reply[32] != 1) {
		dev_err(&m->pdev->dev, "boot status CID=%02x EID=%02x status=%u len=%zu\n",
			cid, m->reply[28], m->reply[32], m->reply_length);
		return -EPROTO;
	}
	return 0;
}

static int mt_scatter(struct mt7932 *m, const u8 *data, size_t length)
{
	struct mt7932_ring *q = &m->tx[16];

	while (length) {
		size_t bytes = min_t(size_t, length, 2048);
		int ret;

		if (q->used >= q->slots)
			return -ENOSPC;
		memcpy(q->buffers + q->used * STRIDE, data, bytes);
		ret = mt_submit(m, q, bytes);
		if (ret)
			return ret;
		mt_tx_clean(q);
		data += bytes;
		length -= bytes;
	}
	return 0;
}

static int mt_download(struct mt7932 *m, const u8 *data, struct mt7932_region *r,
		       bool ram)
{
	u8 request[12];
	u8 cid = r->target == 0x900000 || r->target == 0x200000 ? 5 : 1;
	int ret;

	put_unaligned_le32(r->target, request);
	put_unaligned_le32(r->length, request + 4);
	put_unaligned_le32(r->mode, request + 8);
	WRITE_ONCE(m->ram_config, ram);
	ret = mt_status(m, cid, request, sizeof(request));
	WRITE_ONCE(m->ram_config, false);
	if (ret)
		return ret;
	ret = mt_scatter(m, data + r->offset, r->length);
	dev_info(&m->pdev->dev, "%s region target=%08x length=%u scatter=%d\n",
		 ram ? "RAM" : "PATCH", r->target, r->length, ret);
	return ret;
}

static int mt_secure_acquire(struct mt7932 *m)
{
	int ret;

	mt_write(m, 0xfe24c, 0x18451807);
	ret = mt_poll(m, 0x40060, 1, 1, 5000000);
	if (!ret)
		mt_write(m, 0xfe24c, 0x1845184f);
	return ret;
}

static void mt_secure_release(struct mt7932 *m)
{
	mt_write(m, 0xfe24c, 0x18451807);
	mt_write(m, 0x40260, 1);
	mt_write(m, 0xfe24c, 0x1845184f);
}

static int mt_otp_byte(struct mt7932 *m, u32 address, u8 *value)
{
	u8 request[4];
	int ret;

	put_unaligned_le32(address, request);
	ret = mt_command(m, 0x50, request, sizeof(request));
	if (ret)
		return ret;
	if (m->reply_length < 40)
		return -EPROTO;
	*value = m->reply[36];
	return 0;
}

static int mt_capability_gate(struct mt7932 *m)
{
	u8 entropy[68] = {};
	const u8 *body;
	size_t offset = 4;
	int ret, count, i;
	bool mac = false, phy = false;

	get_random_bytes(entropy + 4, 64);
	ret = mt_request(m, 0xe2, false, false, false, entropy, sizeof(entropy));
	memzero_explicit(entropy, sizeof(entropy));
	if (ret)
		return ret;
	ret = mt_request(m, 0, false, false, false, NULL, 0);
	if (ret)
		return ret;
	ret = mt_request(m, 0x8a, true, false, true, NULL, 0);
	if (ret)
		return ret;
	if (m->reply_length < 40)
		return -EPROTO;
	body = m->reply + 36;
	if (m->reply[28] != 0xec)
		return -EPROTO;
	count = mt7932_capabilities(body, m->reply_length - 36);
	if (count < 0) {
		dev_err(&m->pdev->dev, "invalid capability reply len=%zu parse=%d\n", m->reply_length, count);
		print_hex_dump(KERN_INFO, "NIC_CAP_REJECTED: ", DUMP_PREFIX_OFFSET,
			       16, 1, m->reply, m->reply_length, false);
		return count;
	}
	for (i = 0; i < count; i++) {
		u32 type = get_unaligned_le32(body + offset);
		u32 length = get_unaligned_le32(body + offset + 4);
		const u8 *value = body + offset + 8;

		dev_info(&m->pdev->dev, "capability TLV type=%x bytes=%u\n", type, length);
		if (type == 7) {
			dev_info(&m->pdev->dev, "firmware MAC %pM\n", value);
			mac = true;
		} else if (type == 8) {
			if (!value[4] || value[4] > 4)
				return -EOPNOTSUPP;
			memcpy(m->phy_cap, value, 12);
			m->antenna_mask = (1U << value[4]) - 1;
			dev_info(&m->pdev->dev, "firmware PHY NSS=%u paths=%02x HT=%u VHT=%u HE=%u\n",
				 value[4], value[10], value[0], value[1], value[11]);
			phy = true;
		} else if (type == 0x38 && length) {
			m->runtime_cal = value[0];
			m->runtime_cal_valid = true;
			dev_info(&m->pdev->dev, "RUNTIME_CAL_CAPABILITY: %u\n", value[0]);
		} else if (type == 0x18 && length >= 4) {
			m->six_ghz = value[0];
			m->six_ghz_valid = value[0] <= 1;
		} else if (type == 0x34 && length >= 2) {
			m->stof_supported = value[0] || value[1];
		}
		offset += 8 + length;
	}
	if (!mac || !phy)
		return -ENODATA;
	print_hex_dump(KERN_INFO, "NIC_CAP: ", DUMP_PREFIX_OFFSET, 16, 1,
		       m->reply, m->reply_length, false);
	dev_info(&m->pdev->dev, "MCU_CAPABILITY_OK eid=%02x seq=%u (no calibration or RF yet)\n",
		 m->reply[28], m->seq);
	return 0;
}

static int mt_config_gate(struct mt7932 *m)
{
	u8 basic[24], mlme[4];
	unsigned long deadline;
	int ret;

	mt7932_basic_config(basic);
	mt7932_mlme_config(mlme);
	ret = mt_request(m, 2, true, true, false, basic, sizeof(basic));
	if (ret)
		return ret;
	ret = mt_request_ext(m, 0xea, 0x4e, true, true, false, mlme, sizeof(mlme));
	if (ret)
		return ret;
	deadline = jiffies + msecs_to_jiffies(500);
	do {
		mt_tx_clean(&m->tx[17]);
		if (!m->tx[17].queued) {
			dev_info(&m->pdev->dev, "BASIC_MLME_SUBMITTED: TX retired, no semantic ACK or RF startup\n");
			return 0;
		}
		usleep_range(100, 200);
	} while (time_before(jiffies, deadline));
	return -ETIMEDOUT;
}


static int mt_board_gate(struct mt7932 *m)
{
	const u32 addresses[] = {0x70, 0x160};
	u8 request[24] = {}, module = 0, board_byte = 0;
	u32 status[2];
	unsigned int i, candidate;
	int ret;

	if (!m->runtime_epoch)
		return -EINVAL;
	ret = mt_request_ext(m, 0xed, 0xc1, true, false, true, NULL, 0);
	if (ret)
		return ret;
	if (m->reply_length < 44 || m->reply[28] != 0xed || m->reply[32] != 0xc1)
		return -EPROTO;
	dev_info(&m->pdev->dev, "CHIP_UID_QUERY_OK: 8 opaque bytes received, not published\n");
	for (i = 0; i < ARRAY_SIZE(addresses); i++) {
		put_unaligned_le32(addresses[i], request);
		ret = mt_request_ext(m, 0xed, 1, true, false, true, request, sizeof(request));
		if (ret)
			return ret;
		if (m->reply_length < 60 || m->reply[28] != 0xed || m->reply[32] != 1 ||
		    get_unaligned_le32(m->reply + 36) != addresses[i])
			return -EPROTO;
		status[i] = get_unaligned_le32(m->reply + 40);
		if (!i)
			module = m->reply[54];
		else
			board_byte = m->reply[48];
	}
	candidate = !(module & 15) ? 2 : !board_byte ? 3 : 1;
	m->board_type = candidate;
	m->module_byte = module;
	dev_info(&m->pdev->dev, "BOARD_CLASSIFICATION_CAPTURED: candidate=%u module=%02x board-byte=%02x status=%08x/%08x; no buffer branch selected\n",
		 candidate, module, board_byte, status[0], status[1]);
	return 0;
}

static int mt_precal_gate(struct mt7932 *m)
{
	const struct firmware *ppr;
	u8 request[416] = {2, 0, 0x9c, 1};
	u32 status;
	int ret;

	if (!m->runtime_epoch || m->board_type != 3 || !m->runtime_cal_valid)
		return -EINVAL;
	/* Stock EfuseBufferModeCal=3, own eFuse board3: original PPR source2.
	 * This is not the external EEPROM/source0 or own-WCAL/source3 path.
	 */
	ret = request_firmware_direct(&ppr, "mediatek/mt7932/ppr.bin", &m->pdev->dev);
	if (ret)
		return ret;
	if (ppr->size != sizeof(request) - 4) {
		release_firmware(ppr);
		return -EINVAL;
	}
	memcpy(request + 4, ppr->data, ppr->size);
	release_firmware(ppr);
	ret = mt_request_ext(m, 0xed, 0x21, true, false, true, request, sizeof(request));
	if (ret)
		return ret;
	if (m->reply_length < 44 || m->reply[28] != 0xed || m->reply[32])
		return -EPROTO;
	status = get_unaligned_le32(m->reply + 40);
	dev_info(&m->pdev->dev, "PPR_SOURCE2_RESULT: status=%08x\n", status);
	if (status)
		return -EREMOTEIO;
	memset(request, 0, sizeof(request));
	ret = mt_request_ext(m, 0xed, 0xa9, true, false, true, request, 4);
	if (ret)
		return ret;
	if (m->reply_length < 40 || m->reply[28] != 0xed || m->reply[32] != 0xaa)
		return -EPROTO;
	m->smart_version = m->reply[38] & 15;
	dev_info(&m->pdev->dev, "SMART_VERSION_REPLY: eid=%02x ext=%02x version=%u\n",
		 m->reply[28], m->reply[32], m->smart_version);
	request[3] = 9;
	ret = mt_request(m, 0xbf, true, false, true, request, 20);
	if (ret)
		return ret;
	if (m->reply_length < 56)
		return -EPROTO;
	dev_info(&m->pdev->dev, "CALTYPE_REPLY: eid=%02x ext=%02x H=%u P=%u O=%u V=%u marker=%08x\n",
		 m->reply[28], m->reply[32], m->runtime_cal, m->reply[37],
		 m->reply[44], m->reply[38], get_unaligned_le32(m->reply + 48));
	/* M=0/T=3 exact host predicate. Stage metadata only after acceptance;
	 * do not select the legacy absent-marker D6 compatibility fallback.
	 */
	if (get_unaligned_le32(m->reply + 48) != 0xbf || m->runtime_cal ||
	    m->reply[37] != 1 || m->reply[44] != 1)
		return -EPROTO;
	m->preload_version = m->reply[38];
	dev_info(&m->pdev->dev, "PRECAL_HANDSHAKE_OK: own calibration and RF policy still required\n");
	return 0;
}

static int mt_runtime_start(struct mt7932 *m)
{
	int ret;

	ret = mt_capability_gate(m);
	if (!ret)
		ret = mt_config_gate(m);
	if (!ret)
		ret = mt_recovery_gate(m);
	if (!ret)
		ret = mt_board_gate(m);
	if (!ret)
		ret = mt_precal_gate(m);

	return ret;
}

static int mt_firmware_gate(struct mt7932 *m)
{
	const struct firmware *patch, *ram;
	struct mt7932_region region;
	u8 request[8] = {}, value;
	u32 entry = 0;
	int ret, patches, regions, i;

	ret = request_firmware_direct(&patch, "mediatek/mt7932/IZUBA_WIFI_MT7932_patch_mcu_1_2_hdr.bin", &m->pdev->dev);
	if (ret)
		return ret;
	ret = request_firmware_direct(&ram, "mediatek/mt7932/IZUBA_W7932_2.bin", &m->pdev->dev);
	if (ret)
		goto out_patch;
	/* Validate all regions before emitting the first download command. */
	patches = mt7932_patch_region(patch->data, patch->size, 0, &region);
	regions = mt7932_ram_region(ram->data, ram->size, 0, &region);
	ret = -EINVAL;
	if (patches < 0 || regions < 0)
		goto out;
	for (i = 0; i < patches; i++)
		if (mt7932_patch_region(patch->data, patch->size, i, &region) < 0)
			goto out;
	for (i = 0; i < regions; i++)
		if (mt7932_ram_region(ram->data, ram->size, i, &region) < 0)
			goto out;
	dev_info(&m->pdev->dev, "stock containers patch=%zu/%d regions RAM=%zu/%d regions\n",
		 patch->size, patches, ram->size, regions);
	put_unaligned_le32(2, request);
	ret = mt_command(m, 0x10, request, 4);
	if (ret)
		goto out;
	if (m->reply_length < 33 || m->reply[32] < 1 || m->reply[32] > 3) {
		ret = -EPROTO;
		goto out;
	}
	if (m->reply[32] != 1) {
		for (i = 0; i < patches; i++) {
			mt7932_patch_region(patch->data, patch->size, i, &region);
			ret = mt_download(m, patch->data, &region, false);
			if (ret)
				goto out;
		}
		ret = mt_secure_acquire(m);
		if (ret)
			goto out;
		memset(request, 0, sizeof(request));
		ret = mt_status(m, 7, request, 4);
		if (ret)
			goto out;
		mt_secure_release(m);
	}
	dev_info(&m->pdev->dev, "PATCH_READY\n");
	ret = mt_otp_byte(m, 0x7a, &value);
	if (!ret)
		ret = mt_otp_byte(m, 0x164, &value);
	if (!ret)
		ret = mt_otp_byte(m, 0x200, &value);
	if (ret)
		goto out;
	if (value != 0x15) {
		dev_err(&m->pdev->dev, "OTP selector mismatch: %02x\n", value);
		ret = -EPROTO;
		goto out;
	}
	for (i = 0x200; i <= 0x23f; i++) {
		ret = mt_otp_byte(m, i, &value);
		if (ret)
			goto out;
	}
	for (i = 0; i < regions; i++) {
		mt7932_ram_region(ram->data, ram->size, i, &region);
		if (region.features & BIT(5))
			entry = region.target;
		if (region.features & BIT(6))
			continue;
		ret = mt_download(m, ram->data, &region, true);
		if (ret)
			goto out;
	}
	for (i = 0; i < 1000; i++) {
		mt_tx_clean(&m->tx[16]);
		if (!m->tx[16].queued)
			break;
		usleep_range(100, 200);
	}
	if (m->tx[16].queued) {
		ret = -ETIMEDOUT;
		goto out;
	}
	usleep_range(20000, 21000);
	ret = mt_secure_acquire(m);
	if (ret)
		goto out;
	put_unaligned_le32(!!entry, request);
	put_unaligned_le32(entry, request + 4);
	ret = mt_status(m, 2, request, sizeof(request));
	if (ret)
		goto out;
	mt_secure_release(m);
	ret = mt_poll(m, 0xe00f0, 3, 3, 5000000);
	if (ret)
		goto out;
	dev_info(&m->pdev->dev, "FIRMWARE_READY entry=%08x\n", entry);
	ret = mt_runtime_start(m);
out:
	release_firmware(ram);
out_patch:
	release_firmware(patch);
	return ret;
}

void mt_transport_snapshot(struct mt7932 *m)
{
	unsigned int i;
	struct mt7932_ring *tx = &m->tx[17];

	dev_info(&m->pdev->dev, "TRANSPORT: interrupts=%llu packets=%llu pending=%08x mask=%08x TX17 head=%u tail=%u queued=%u DMA=%u\n",
		 m->interrupts, m->packets, mt_read(m, W + 0x200), mt_read(m, W + 0x204),
		 tx->head, tx->tail, tx->queued, mt_read(m, tx->reg + 12));
	for (i = 0; i < ARRAY_SIZE(m->rx); i++) {
		struct mt7932_ring *rx = &m->rx[i];

		if (rx->count)
			dev_info(&m->pdev->dev, "TRANSPORT_RX%u: tail=%u producer=%u DMA=%u control=%08x\n",
				 i, rx->tail, mt_read(m, rx->reg + 8), mt_read(m, rx->reg + 12),
				 le32_to_cpu(READ_ONCE(rx->desc[rx->tail].control)));
	}
}


static int mt_initialize(struct mt7932 *m)
{
	int ret;

	ret = mt_dma_setup(m);
	if (!ret)
		ret = mt_rom_gate(m);
	if (!ret)
		ret = mt_firmware_gate(m);
	return ret;
}

static int mt_reset_retained(struct mt7932 **epoch)
{
	struct mt7932 *old = *epoch, *fresh;
	struct pci_dev *pdev = old->pdev;
	int irq = pci_irq_vector(pdev, 0), ret;

	/* No interface/work producers have been published at this test gate. */
	WRITE_ONCE(old->running, false);
	mt_write(old, W + 0x204, 0);
	devm_free_irq(&pdev->dev, irq, old);
	old->irq_requested = false;
	ret = mt_ownership(old);
	if (!ret)
		ret = mt_dma_stop(old);
	if (!ret)
		ret = mt_function_reset(old);
	if (ret)
		return ret;
	old->dma_owned = false;
	dev_info(&pdev->dev, "RESET_TEST: function reset complete; retaining old DMA\n");
	fresh = devm_kzalloc(&pdev->dev, sizeof(*fresh), GFP_KERNEL);
	if (!fresh)
		return -ENOMEM;
	fresh->pdev = pdev;
	fresh->bar = old->bar;
	fresh->irq_mask = IRQ_MASK;
	fresh->vectors_allocated = true;
	ether_addr_copy(fresh->mac, old->mac);
	init_completion(&fresh->response);
	init_completion(&fresh->cal_response);
	init_completion(&fresh->reset_retry);
	mutex_init(&fresh->command_mutex);
	spin_lock_init(&fresh->response_lock);
	*epoch = fresh;
	pci_set_drvdata(pdev, fresh);
	ret = mt_ownership(fresh);
	if (ret)
		return ret;
	ret = mt_poll(fresh, 0xf0140, BIT(4), BIT(4), 200000);
	if (ret)
		return ret;
	mt_write(fresh, W + 0x204, 0);
	ret = devm_request_threaded_irq(&pdev->dev, irq, mt_irq, mt_irq_thread,
					IRQF_ONESHOT | IRQF_SHARED, "mt7932", fresh);
	if (ret)
		return ret;
	fresh->irq_requested = true;
	ret = mt_initialize(fresh);
	if (!ret)
		dev_info(&pdev->dev, "RESET_TEST: fresh ROM and firmware gates passed\n");
	return ret;
}

/* A failed PCI reset cannot be turned into successful removal: devres would
 * free still-owned DMA. Keep the remove callback (and therefore its device,
 * mappings and module text) alive. This explicit retry does not run hardware
 * operations from sysfs or bypass the PCI device lock held by remove.
 */
static ssize_t removal_retry_store(struct device *dev,
				   struct device_attribute *attr,
				   const char *buf, size_t count)
{
	struct mt7932 *m = dev_get_drvdata(dev);
	bool retry;

	if (kstrtobool(buf, &retry) || !retry)
		return -EINVAL;
	if (!READ_ONCE(m->removal_error))
		return -EBUSY;
	complete(&m->reset_retry);
	return count;
}
static DEVICE_ATTR_WO(removal_retry);

static int mt_retire_dma(struct mt7932 *m)
{
	int ret;

	if (!m->dma_owned)
		return 0;
	ret = mt_ownership(m);
	if (!ret)
		ret = mt_dma_stop(m);
	if (!ret)
		ret = mt_function_reset(m);
	if (!ret)
		m->dma_owned = false;
	return ret;
}

static void mt_remove(struct pci_dev *pdev)
{
	struct mt7932 *m = pci_get_drvdata(pdev);
	int ret;

	mt_stop_host(m);
	while ((ret = mt_retire_dma(m))) {
		WRITE_ONCE(m->removal_error, ret);
		dev_crit(&pdev->dev, "removal blocked: reset error %d; DMA remains owned. Write 1 to removal_retry to retry, or reset the system.\n", ret);
		/* No automatic reset loop and no free-on-timeout. A permanently
		 * inaccessible function requires platform recovery, not a fake
		 * successful rmmod. Unload intentionally remains pending.
		 */
		wait_for_completion(&m->reset_retry);
		WRITE_ONCE(m->removal_error, 0);
	}
	device_remove_file(&pdev->dev, &dev_attr_removal_retry);
	if (m->netdev) {
		unregister_netdev(m->netdev);
		free_netdev(m->netdev);
		m->netdev = NULL;
	}
	if (m->wiphy_registered)
		wiphy_unregister(m->wiphy);
	if (m->wiphy)
		wiphy_free(m->wiphy);
	if (m->vectors_allocated)
		pci_free_irq_vectors(pdev);
	dev_info(&pdev->dev, "REMOVE_COMPLETE: host users joined, PCI function reset, DMA retired\n");
	/* All coherent allocations/BAR mappings are devres-owned and now safe
	 * to release. No default-domain ownership manipulation is necessary.
	 */
}

static int mt_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	struct mt7932 *m;
	struct iommu_domain *domain;
	int ret;

	if (!of_machine_is_compatible("apple,j700") || pdev->devfn != 0)
		return -ENODEV;
	domain = iommu_get_domain_for_dev(&pdev->dev);
	if (!domain || (domain->type != IOMMU_DOMAIN_DMA && domain->type != IOMMU_DOMAIN_DMA_FQ))
		return dev_err_probe(&pdev->dev, -EINVAL, "translated DMA domain required\n");
	/* Station operation was qualified with ASPM and clock PM off on this link. */
	ret = pci_disable_link_state(pdev, PCIE_LINK_STATE_ALL);
	if (ret)
		dev_warn(&pdev->dev, "link power management left as found: %d\n", ret);
	ret = pcim_enable_device(pdev);
	if (ret)
		return ret;
	pci_clear_master(pdev);
	if (pci_resource_len(pdev, 0) < 0x100000)
		return -EINVAL;
	ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(27));
	if (ret)
		return ret;
	m = devm_kzalloc(&pdev->dev, sizeof(*m), GFP_KERNEL);
	if (!m)
		return -ENOMEM;
	m->pdev = pdev;
	m->irq_mask = IRQ_MASK;
	ret = of_get_mac_address(pdev->dev.of_node, m->mac);
	if (ret || !is_valid_ether_addr(m->mac))
		return dev_err_probe(&pdev->dev, ret ?: -EINVAL, "valid own-device DT MAC required\n");
	dev_dbg(&pdev->dev, "ADT_WIFI_IDENTITY_OK MAC=%pM\n", m->mac);
	m->bar = pcim_iomap_region(pdev, 0, "mt7932");
	if (IS_ERR(m->bar))
		return PTR_ERR(m->bar);
	pci_set_drvdata(pdev, m);
	init_completion(&m->response);
	init_completion(&m->cal_response);
	init_completion(&m->reset_retry);
	mutex_init(&m->command_mutex);
	spin_lock_init(&m->response_lock);
	ret = mt_ownership(m);
	if (ret)
		goto retain;
	mt_write(m, W + 0x204, 0);
	ret = pci_alloc_irq_vectors(pdev, 1, 1, legacy_irq_test ? PCI_IRQ_INTX : PCI_IRQ_ALL_TYPES);
	if (ret < 0)
		goto retain;
	m->vectors_allocated = true;
	ret = devm_request_threaded_irq(&pdev->dev, pci_irq_vector(pdev, 0),
					mt_irq, mt_irq_thread, IRQF_ONESHOT | IRQF_SHARED, "mt7932", m);
	if (ret)
		goto retain;
	m->irq_requested = true;
	{
		struct irq_data *irq = irq_get_irq_data(pci_irq_vector(pdev, 0));
		unsigned int cap = pci_find_capability(pdev, PCI_CAP_ID_MSI);
		u16 control, command;
		u32 lo, hi, data, mask;

		pci_read_config_word(pdev, PCI_COMMAND, &command);
		pci_read_config_word(pdev, cap + PCI_MSI_FLAGS, &control);
		pci_read_config_dword(pdev, cap + PCI_MSI_ADDRESS_LO, &lo);
		pci_read_config_dword(pdev, cap + PCI_MSI_ADDRESS_HI, &hi);
		pci_read_config_dword(pdev, cap + PCI_MSI_DATA_64, &data);
		pci_read_config_dword(pdev, cap + PCI_MSI_MASK_64, &mask);
		dev_dbg(&pdev->dev, "PCI command=%04x MSI cap=%x control=%04x addr=%08x:%08x data=%08x\n",
			 command, cap, control, hi, lo, data);
		dev_dbg(&pdev->dev, "MSI mask=%08x MSI-X cap=%x chosen=%s\n", mask,
			 pci_find_capability(pdev, PCI_CAP_ID_MSIX),
			 pdev->msix_enabled ? "MSI-X" : pdev->msi_enabled ? "MSI" : "INTx");
		for (; irq; irq = irq->parent_data)
			dev_dbg(&pdev->dev, "IRQ %u chip=%s hwirq=%lu\n", irq->irq,
				 irq->chip->name, irq->hwirq);
	}
	ret = mt_initialize(m);
	if (!ret && reset_test)
		ret = mt_reset_retained(&m);
	if (!ret)
		ret = mt_register_regulatory_gate(m);
	if (!ret)
		WRITE_ONCE(m->ready, true);
retain:
	if (ret) {
		int error = ret;

		mt_stop_host(m);
		ret = mt_retire_dma(m);
		if (!ret) {
			mt_remove(pdev);
			return dev_err_probe(&pdev->dev, error, "initialization failed; DMA retired\n");
		}
		dev_err(&pdev->dev, "initialization failed: %d, reset error %d; retaining binding and DMA\n", error, ret);
	}
	ret = device_create_file(&pdev->dev, &dev_attr_removal_retry);
	if (ret)
		dev_warn(&pdev->dev, "removal retry attribute unavailable: %d\n", ret);
	return 0;
}

static void mt_shutdown(struct pci_dev *pdev)
{
	struct mt7932 *m = pci_get_drvdata(pdev);

	WRITE_ONCE(m->running, false);
	mt_write(m, W + 0x204, 0);
	pci_clear_master(pdev);
	/* Storage remains allocated through reset, even if the engine is stuck. */
}

/*
 * Stopping the firmware for sleep would take the whole remove and probe
 * sequence, so leave it running across suspend-to-idle. A saved state keeps
 * the PCI core from changing the function's power state or command register,
 * and the awake path keeps the PCIe controller powered until resume.
 */
static int mt_suspend(struct device *dev)
{
	if (!pm_suspend_no_platform())
		return -EOPNOTSUPP;
	pci_save_state(to_pci_dev(dev));
	device_set_awake_path(dev);
	return 0;
}

static const struct dev_pm_ops mt_pm_ops = {
	.suspend = mt_suspend,
};

static const struct pci_device_id mt_ids[] = {
	{ PCI_DEVICE(PCI_VENDOR_ID_MEDIATEK, 0x7932) },
	{}
};
MODULE_DEVICE_TABLE(pci, mt_ids);

static struct pci_driver mt_driver = {
	.name = "mt7932-fullmac",
	.id_table = mt_ids,
	.probe = mt_probe,
	.remove = mt_remove,
	.shutdown = mt_shutdown,
	.driver.suppress_bind_attrs = true,
	.driver.pm = pm_sleep_ptr(&mt_pm_ops),
};

/**
 * mt7932_fullmac_ready() - check that the Wi-Fi function is up
 * @pdev: function 0 of the radio
 *
 * A Wi-Fi function whose initialization failed stays bound when its DMA
 * could not be stopped, so being bound does not mean its firmware runs.
 *
 * Return: true if @pdev is bound to this driver and finished initialization.
 */
bool mt7932_fullmac_ready(struct pci_dev *pdev)
{
	struct mt7932 *m;

	if (!device_is_bound(&pdev->dev) || pdev->dev.driver != &mt_driver.driver)
		return false;
	m = pci_get_drvdata(pdev);
	return m && READ_ONCE(m->ready);
}
EXPORT_SYMBOL_GPL(mt7932_fullmac_ready);

module_pci_driver(mt_driver);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("J700 MT7932 firmware-owned cfg80211 station driver");
