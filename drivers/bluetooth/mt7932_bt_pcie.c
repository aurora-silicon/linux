// SPDX-License-Identifier: GPL-2.0-only
/*
 * J700 MT7932 Bluetooth experiment, reconstructed from the attributed
 * protocol contracts and public Linux interfaces. Original transport research:
 * DJ (DjDeveloperr), Ace (Acelogic), and Ryan Murray.
 *
 * Firmware and board inputs are supplied locally. System sleep preserves the
 * transport arena and uses the firmware quiesce/restore handshake. Unload
 * releases DMA only after the PCI function has stopped bus mastering.
 */
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/firmware.h>
#include <linux/interrupt.h>
#include <linux/iommu.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/overflow.h>
#include <linux/pci.h>
#include <linux/pm.h>
#include <linux/pm_runtime.h>
#include <linux/skbuff.h>
#include <linux/slab.h>
#include <linux/unaligned.h>
#include <linux/workqueue.h>
#include <net/bluetooth/bluetooth.h>
#include <net/bluetooth/hci_core.h>

#define BT7932_DEPTH 128
#define BT7932_MASK (BT7932_DEPTH - 1)
#define BT7932_CHANNELS 11
#define BT7932_CONTROL_SIZE 68
#define BT7932_BAR_SIZE 0x100000
#define BT7932_DOORBELL 0x19484
#define BT7932_STATE 0x33804
#define BT7932_IMAGE_LO 0x33810
#define BT7932_IMAGE_HI 0x33814
#define BT7932_IMAGE_LEN 0x33818
#define BT7932_IMAGE_RESPONSE 0x3381c
#define BT7932_CONTROL 0x33828
#define BT7932_SLEEP 0x3382c
#define BT7932_CONTEXT_LO 0x33830
#define BT7932_CONTEXT_HI 0x33834
#define BT7932_WINDOW_LO 0x3383c
#define BT7932_WINDOW_HI 0x33840
#define BT7932_WINDOW_SIZE 0x33844
#define BT7932_LTR 0x33c28
#define BT7932_TRANSPORT_SELECTOR 0xfe3b4
#define BT7932_CONNECTION_SELECTOR 0xfe3b8
#define BT7932_SEMAPHORE 0xc1060
#define BT7932_SEMAPHORE_RELEASE 0xc1260
#define BT7932_CHIP_ID 0xc0200
#define BT7932_BOOT_CONFIG 0x48c
#define BT7932_CONTROL_POOL_SIZE (BT7932_DEPTH * BT7932_CONTROL_SIZE)
#define BT7932_COMPLETION_SIZE (BT7932_DEPTH * 0x18)
#define BT7932_TX_QUEUE_MAX 128

#define BT7932_FW_B0 "mediatek/MT7932B0_OS_TypeB_0.1.44.0_241001003711.bin"
#define BT7932_FW_B1 "mediatek/MT7932B1_OS_TypeB_2.0.177.0_260706180356.bin"
#define BT7932_FW_B1_FALLBACK "mediatek/MT7932B1_OS_TypeB_0.1.133.0_260128190103.bin"
#define BT7932_PTX "mediatek/MT7932_PTB_IzubaA_0.1.0.0_20251021141303.ptx"
#define BT7932_CAL "mediatek/j700-mt7932-btcal.bin"
#define BT7932_ADDR "mediatek/j700-mt7932-bdaddr.bin"

static bool enable;
module_param(enable, bool, 0600);
MODULE_PARM_DESC(enable, "Diagnostic late-probe gate; default closed, no effect on an already bound device");

struct bt7932_geometry {
	u16 stride;
	u16 payload;
	u8 doorbell;
	u8 class;
	u8 packet;
	bool rx;
};

static const struct bt7932_geometry geometry[BT7932_CHANNELS] = {
	[1] = { 0x11c, 268, 21, 0x43, HCI_COMMAND_PKT, false },
	[2] = { 0x11c, 268, 22, 0x43, HCI_EVENT_PKT, true },
	[3] = { 0x40c, 1020, 23, 0xff, HCI_SCODATA_PKT, false },
	[4] = { 0x40c, 1020, 24, 0xff, HCI_SCODATA_PKT, true },
	[5] = { 0x418, 1032, 25, 0x81, HCI_ACLDATA_PKT, false },
	[6] = { 0x418, 1032, 26, 0x81, HCI_ACLDATA_PKT, true },
	[8] = { 0x418, 1032, 27, 0x81, 0, true },
	[9] = { 0x318, 776, 28, 0xc2, HCI_ISODATA_PKT, false },
	[10] = { 0x318, 776, 29, 0xc2, HCI_ISODATA_PKT, true },
};

struct bt7932_ring {
	size_t offset;
	u16 host;
	u16 previous;
};

enum bt7932_pm_state {
	BT7932_RUNNING,
	BT7932_QUIESCING,
	BT7932_QUIESCED,
	BT7932_RESTORING,
};

struct bt7932 {
	struct pci_dev *pdev;
	void __iomem *bar;
	struct hci_dev *hdev;
	struct mutex lock;
	struct workqueue_struct *tx_wq;
	struct work_struct tx_work;
	struct sk_buff_head tx_queue;
	wait_queue_head_t wake_wait;
	struct bt7932_ring rings[BT7932_CHANNELS];
	void *arena;
	dma_addr_t arena_dma;
	size_t arena_size;
	size_t context, peripheral, cr_producer, fw_transfer;
	size_t cr_consumer, host_transfer, completions, requests, diagnostic;
	void *image;
	dma_addr_t image_dma;
	size_t image_size, image_body;
	u8 *calibration, *ptx;
	size_t calibration_size, ptx_size;
	u8 address[6];
	u8 rom;
	u16 cr_previous;
	u32 last_sleep;
	int irq;
	bool enabled, regions, vectors, irq_requested, retained;
	bool ipc_ready, opened, awake, waking, fault, registered;
	bool stopping, runtime_forbidden;
	enum bt7932_pm_state pm_state;
	u64 tx_packets, rx_packets, diagnostics;
};

static void bt7932_ring_doorbell(struct bt7932 *bt, unsigned int bit)
{
	dma_wmb();
	writel(BIT(bit), bt->bar + BT7932_DOORBELL);
}

static u16 bt7932_counter(struct bt7932 *bt, size_t base, unsigned int channel)
{
	__le16 *cell = bt->arena + base + channel * sizeof(*cell);
	u16 value = le16_to_cpu(READ_ONCE(*cell)) & BT7932_MASK;

	dma_rmb();
	return value;
}

static void bt7932_publish_counter(struct bt7932 *bt, size_t base,
				    unsigned int channel, u16 value)
{
	__le16 *cell = bt->arena + base + channel * sizeof(*cell);

	dma_wmb();
	WRITE_ONCE(*cell, cpu_to_le16(value));
}

static void bt7932_fault_locked(struct bt7932 *bt, const char *reason)
{
	int channel;

	if (bt->fault)
		return;
	WRITE_ONCE(bt->fault, true);
	dev_err(&bt->pdev->dev, "terminal transport fault: %s; owned DMA retained, external reset required\n",
		reason);
	for (channel = 0; channel < BT7932_CHANNELS; channel++)
		dev_err(&bt->pdev->dev, "ring%u host=%u firmware=%u previous=%u\n", channel,
			bt7932_counter(bt, bt->host_transfer, channel),
			bt7932_counter(bt, bt->fw_transfer, channel), bt->rings[channel].previous);
	pci_clear_master(bt->pdev);
	if (bt->irq_requested)
		disable_irq_nosync(bt->irq);
	wake_up_all(&bt->wake_wait);
}

static bool bt7932_hci_length(u8 packet, const u8 *data, size_t len)
{
	size_t expected;

	switch (packet) {
	case HCI_COMMAND_PKT:
		if (len < HCI_COMMAND_HDR_SIZE)
			return false;
		expected = HCI_COMMAND_HDR_SIZE + data[2];
		break;
	case HCI_EVENT_PKT:
		if (len < HCI_EVENT_HDR_SIZE)
			return false;
		expected = HCI_EVENT_HDR_SIZE + data[1];
		break;
	case HCI_ACLDATA_PKT:
		if (len < HCI_ACL_HDR_SIZE)
			return false;
		expected = HCI_ACL_HDR_SIZE + get_unaligned_le16(data + 2);
		break;
	case HCI_SCODATA_PKT:
		if (len < HCI_SCO_HDR_SIZE)
			return false;
		expected = HCI_SCO_HDR_SIZE + data[2];
		break;
	case HCI_ISODATA_PKT:
		if (len < HCI_ISO_HDR_SIZE)
			return false;
		expected = HCI_ISO_HDR_SIZE + (get_unaligned_le16(data + 2) & 0x3fff);
		break;
	default:
		return false;
	}
	return len == expected;
}

static int bt7932_validate_ptx(const u8 *data, size_t size)
{
	static const u32 types[] = { 0x101, 0x201, 0x301, 0x401 };
	static const u32 sizes[] = { 14, 44, 44, 0 };
	u32 next = 96;
	int i;

	if (size != 198 || memcmp(data, "BLOB", 4) ||
	    get_unaligned_le32(data + 4) != 96 ||
	    get_unaligned_le16(data + 8) != 1 ||
	    get_unaligned_le16(data + 10) != ARRAY_SIZE(types) ||
	    get_unaligned_le32(data + 12))
		return -EINVAL;
	for (i = 0; i < ARRAY_SIZE(types); i++) {
		const u8 *record = data + 16 + i * 20;
		u32 offset = get_unaligned_le32(record + 4);
		u32 length = get_unaligned_le32(record + 8);
		u32 sum = 0, j;

		if (get_unaligned_le32(record) != types[i] || offset != next ||
		    length != sizes[i] || offset > size || length > size - offset ||
		    get_unaligned_le32(record + 16))
			return -EINVAL;
		for (j = 0; j < length; j++)
			sum += data[offset + j];
		if (sum != get_unaligned_le32(record + 12))
			return -EINVAL;
		next = offset + length;
	}
	return next == size ? 0 : -EINVAL;
}

static int bt7932_read_inputs(struct bt7932 *bt)
{
	const struct firmware *fw;
	const u8 *trailer;
	u64 last;
	int i, ret;

	if (!bt->rom) {
		ret = request_firmware(&fw, BT7932_FW_B0, &bt->pdev->dev);
	} else {
		ret = firmware_request_nowarn(&fw, BT7932_FW_B1, &bt->pdev->dev);
		if (ret == -ENOENT)
			ret = request_firmware(&fw, BT7932_FW_B1_FALLBACK, &bt->pdev->dev);
	}
	if (ret)
		return ret;
	if (fw->size <= 32 || fw->size - 32 > U32_MAX - 3) {
		ret = -EINVAL;
		goto release_image;
	}
	trailer = fw->data + fw->size - 32;
	if (memcmp(trailer + 16, "ALPS\x8a\x10\x8a\x10", 8)) {
		ret = -EINVAL;
		goto release_image;
	}
	bt->image_body = fw->size - 32;
	bt->image_size = ALIGN(bt->image_body, 4);
	bt->image = dma_alloc_coherent(&bt->pdev->dev, bt->image_size,
				      &bt->image_dma, GFP_KERNEL);
	if (!bt->image) {
		ret = -ENOMEM;
		goto release_image;
	}
	if (check_add_overflow((u64)bt->image_dma, bt->image_size - 1, &last) ||
	    last > DMA_BIT_MASK(32)) {
		ret = -ERANGE;
		goto release_image;
	}
	memset(bt->image, 0, bt->image_size);
	memcpy(bt->image, fw->data, bt->image_body);
release_image:
	release_firmware(fw);
	if (ret)
		return ret;
	ret = request_firmware(&fw, BT7932_CAL, &bt->pdev->dev);
	if (ret)
		return ret;
	if (!fw->size || fw->size > U16_MAX) {
		ret = -EINVAL;
	} else {
		bt->calibration_size = fw->size;
		bt->calibration = kmemdup(fw->data, fw->size, GFP_KERNEL);
		if (!bt->calibration)
			ret = -ENOMEM;
	}
	release_firmware(fw);
	if (ret)
		return ret;
	ret = request_firmware(&fw, BT7932_PTX, &bt->pdev->dev);
	if (ret)
		return ret;
	ret = bt7932_validate_ptx(fw->data, fw->size);
	if (!ret) {
		bt->ptx_size = fw->size;
		bt->ptx = kmemdup(fw->data, fw->size, GFP_KERNEL);
		if (!bt->ptx)
			ret = -ENOMEM;
	}
	release_firmware(fw);
	if (ret)
		return ret;
	ret = request_firmware(&fw, BT7932_ADDR, &bt->pdev->dev);
	if (ret)
		return ret;
	if (fw->size != sizeof(bt->address) ||
	    !memchr_inv(fw->data, 0, fw->size) || !memchr_inv(fw->data, 0xff, fw->size)) {
		ret = -EINVAL;
	} else {
		for (i = 0; i < sizeof(bt->address); i++)
			bt->address[i] = fw->data[sizeof(bt->address) - i - 1];
	}
	release_firmware(fw);
	return ret;
}

static int bt7932_reserve(size_t *total, size_t bytes, size_t *offset)
{
	size_t aligned;

	if (check_add_overflow(*total, (size_t)63, &aligned))
		return -EOVERFLOW;
	aligned &= ~(size_t)63;
	*offset = aligned;
	if (check_add_overflow(aligned, bytes, total))
		return -EOVERFLOW;
	return 0;
}

static int bt7932_arena(struct bt7932 *bt)
{
	struct {
		size_t bytes;
		size_t *offset;
	} regions[] = {
		{ 104, &bt->context }, { 16, &bt->peripheral },
		{ 4, &bt->cr_producer }, { 24, &bt->fw_transfer },
		{ 4, &bt->cr_consumer }, { 24, &bt->host_transfer },
		{ BT7932_COMPLETION_SIZE, &bt->completions },
		{ BT7932_CONTROL_POOL_SIZE, &bt->requests },
		{ 0x1400, &bt->diagnostic },
	};
	size_t total = 0;
	u8 *context;
	u64 last;
	int i, ret;

	for (i = 0; i < ARRAY_SIZE(regions); i++) {
		ret = bt7932_reserve(&total, regions[i].bytes, regions[i].offset);
		if (ret)
			return ret;
	}
	for (i = 1; i < BT7932_CHANNELS; i++) {
		if (!geometry[i].stride)
			continue;
		ret = bt7932_reserve(&total, geometry[i].stride * BT7932_DEPTH,
				     &bt->rings[i].offset);
		if (ret)
			return ret;
	}
	if (total > U32_MAX - PAGE_SIZE)
		return -EOVERFLOW;
	bt->arena_size = PAGE_ALIGN(total);
	bt->arena = dma_alloc_coherent(&bt->pdev->dev, bt->arena_size,
				      &bt->arena_dma, GFP_KERNEL);
	if (!bt->arena)
		return -ENOMEM;
	if (!IS_ALIGNED(bt->arena_dma, 64) ||
	    check_add_overflow((u64)bt->arena_dma, bt->arena_size - 1, &last) ||
	    last > DMA_BIT_MASK(32))
		return -ERANGE;
	memset(bt->arena, 0, bt->arena_size);
	context = bt->arena + bt->context;
	put_unaligned_le64(bt->arena_dma + bt->peripheral, context + 0x08);
	put_unaligned_le64(bt->arena_dma + bt->cr_producer, context + 0x10);
	put_unaligned_le64(bt->arena_dma + bt->fw_transfer, context + 0x18);
	put_unaligned_le64(bt->arena_dma + bt->cr_consumer, context + 0x20);
	put_unaligned_le64(bt->arena_dma + bt->host_transfer, context + 0x28);
	put_unaligned_le32(0x000b0001, context + 0x30);
	put_unaligned_le64(bt->arena_dma + bt->completions, context + 0x34);
	put_unaligned_le64(bt->arena_dma + bt->requests, context + 0x3c);
	put_unaligned_le16(BT7932_DEPTH, context + 0x44);
	put_unaligned_le16(BT7932_DEPTH, context + 0x46);
	context[0x51] = 13;
	context[0x53] = 2;
	dma_wmb();
	dev_info(&bt->pdev->dev, "owned SID2 arena=%pad bytes=%zu image=%pad bytes=%zu, both low32\n",
		 &bt->arena_dma, bt->arena_size, &bt->image_dma, bt->image_size);
	return 0;
}

/* Serialize control requests with sleep notifications and ring publication. */
static void bt7932_request_sleep(struct bt7932 *bt, u32 control)
{
	writel(control, bt->bar + BT7932_SLEEP);
	bt7932_ring_doorbell(bt, 12);
}

/* Caller owns lock; no HCI command wait can run from the IRQ thread. */
static int bt7932_sleep_locked(struct bt7932 *bt)
{
	__le32 *notification = bt->arena + bt->peripheral + 8;
	u32 value;

	dma_rmb();
	value = le32_to_cpu(READ_ONCE(*notification));
	if (value == 3) {
		if (bt->pm_state == BT7932_QUIESCING) {
			WRITE_ONCE(bt->pm_state, BT7932_QUIESCED);
			WRITE_ONCE(bt->awake, false);
			wake_up_all(&bt->wake_wait);
		} else if (bt->pm_state != BT7932_QUIESCED &&
			   bt->pm_state != BT7932_RESTORING) {
			return -EPROTO;
		}
		return 0;
	}
	if (value > 1)
		return -EPROTO;
	/* A previous host-sleep notification must not cancel control 3. */
	if (bt->pm_state == BT7932_QUIESCING)
		return 0;
	if (bt->pm_state == BT7932_QUIESCED)
		return -EPROTO;
	if (bt->pm_state == BT7932_RESTORING) {
		if (value) {
			if (!bt->waking) {
				bt->waking = true;
				bt7932_request_sleep(bt, 1);
				bt7932_request_sleep(bt, 2);
			}
			return 0;
		}
		bt->waking = true;
		WRITE_ONCE(bt->pm_state, BT7932_RUNNING);
	}
	if (!value) {
		if (bt->last_sleep != value || bt->waking)
			bt7932_request_sleep(bt, 0);
		WRITE_ONCE(bt->awake, true);
		bt->waking = false;
		wake_up_all(&bt->wake_wait);
	} else {
		WRITE_ONCE(bt->awake, false);
		if (!bt->waking && bt->last_sleep != value) {
			bt7932_request_sleep(bt, 1);
		}
	}
	bt->last_sleep = value;
	return 0;
}

/* On success returns with lock held, covering the final check/publication. */
static int bt7932_wake_and_lock(struct bt7932 *bt, bool require_open)
{
	unsigned long deadline = jiffies + HZ;
	unsigned long now;
	int ret;

	for (;;) {
		mutex_lock(&bt->lock);
		if (bt->fault || bt->stopping || bt->pm_state != BT7932_RUNNING ||
		    (require_open && !bt->opened)) {
			mutex_unlock(&bt->lock);
			return -EHOSTDOWN;
		}
		ret = bt7932_sleep_locked(bt);
		if (ret)
			goto fail;
		if (bt->awake)
			return 0;
		if (!bt->waking) {
			bt->waking = true;
			bt7932_request_sleep(bt, 2);
		}
		mutex_unlock(&bt->lock);
		now = jiffies;
		if (time_after_eq(now, deadline)) {
			mutex_lock(&bt->lock);
			ret = -ETIMEDOUT;
			goto fail;
		}
		wait_event_timeout(bt->wake_wait,
				   READ_ONCE(bt->awake) || READ_ONCE(bt->fault) ||
				   READ_ONCE(bt->stopping) ||
				   (require_open && !READ_ONCE(bt->opened)),
				   min_t(unsigned long, msecs_to_jiffies(10), deadline - now));
	}
fail:
	bt7932_fault_locked(bt, "wake handshake failed");
	mutex_unlock(&bt->lock);
	return ret;
}

static int bt7932_tx_channel(u8 packet)
{
	switch (packet) {
	case HCI_COMMAND_PKT: return 1;
	case HCI_SCODATA_PKT: return 3;
	case HCI_ACLDATA_PKT: return 5;
	case HCI_ISODATA_PKT: return 9;
	default: return -EINVAL;
	}
}

static int bt7932_tx_progress(struct bt7932 *bt, unsigned int channel)
{
	struct bt7932_ring *ring = &bt->rings[channel];
	u16 consumer = bt7932_counter(bt, bt->fw_transfer, channel);
	u16 completed = (consumer - ring->previous) & BT7932_MASK;
	u16 outstanding = (ring->host - ring->previous) & BT7932_MASK;

	if (completed > outstanding)
		return -EPROTO;
	ring->previous = consumer;
	return 0;
}

static void bt7932_tx_work(struct work_struct *work)
{
	struct bt7932 *bt = container_of(work, struct bt7932, tx_work);
	struct sk_buff *skb;

	while ((skb = skb_dequeue(&bt->tx_queue))) {
		int channel = bt7932_tx_channel(hci_skb_pkt_type(skb));
		struct bt7932_ring *ring;
		u16 consumer;
		u8 *descriptor;

		if (bt7932_wake_and_lock(bt, true)) {
			kfree_skb(skb);
			skb_queue_purge(&bt->tx_queue);
			return;
		}
		ring = &bt->rings[channel];
		if (bt7932_tx_progress(bt, channel)) {
			bt7932_fault_locked(bt, "TX consumer exceeded outstanding descriptors");
			mutex_unlock(&bt->lock);
			kfree_skb(skb);
			skb_queue_purge(&bt->tx_queue);
			return;
		}
		consumer = ring->previous;
		if (((ring->host + 1) & BT7932_MASK) == consumer) {
			skb_queue_head(&bt->tx_queue, skb);
			mutex_unlock(&bt->lock);
			return;
		}
		descriptor = bt->arena + ring->offset + ring->host * geometry[channel].stride;
		memset(descriptor, 0, geometry[channel].stride);
		descriptor[0] = 2;
		descriptor[1] = skb->len;
		descriptor[2] = skb->len >> 8;
		descriptor[3] = skb->len >> 16;
		put_unaligned_le16(ring->host, descriptor + 12);
		descriptor[15] = 2;
		memcpy(descriptor + 16, skb->data, skb->len);
		ring->host = (ring->host + 1) & BT7932_MASK;
		bt7932_publish_counter(bt, bt->host_transfer, channel, ring->host);
		bt7932_ring_doorbell(bt, geometry[channel].doorbell);
		bt->tx_packets++;
		bt->hdev->stat.byte_tx += skb->len;
		mutex_unlock(&bt->lock);
		kfree_skb(skb);
	}
}

static void bt7932_offer(struct bt7932 *bt, unsigned int channel, u16 slot)
{
	u8 *descriptor = bt->arena + bt->rings[channel].offset + slot * geometry[channel].stride;

	memset(descriptor, 0, geometry[channel].stride);
	put_unaligned_le16(slot, descriptor + 12);
}

static int bt7932_rx_locked(struct bt7932 *bt, unsigned int channel)
{
	const struct bt7932_geometry *geo = &geometry[channel];
	struct bt7932_ring *ring = &bt->rings[channel];
	u16 completed = bt7932_counter(bt, bt->fw_transfer, channel);
	u16 count = (completed - ring->previous) & BT7932_MASK;
	u16 offered = (ring->host - ring->previous) & BT7932_MASK;
	u16 slot = ring->previous;
	int i;

	if (count > offered)
		return -EPROTO;
	for (i = 0; i < count; i++) {
		u8 *descriptor = bt->arena + ring->offset + slot * geo->stride;
		u32 length = descriptor[1] | descriptor[2] << 8 | descriptor[3] << 16;
		struct sk_buff *skb;

		if (descriptor[0] != 2 || get_unaligned_le16(descriptor + 12) != slot ||
		    descriptor[14] || descriptor[15] != 4 ||
		    memchr_inv(descriptor + 4, 0, 8) || length > geo->payload)
			return -EPROTO;
		if (geo->packet && !bt7932_hci_length(geo->packet, descriptor + 16, length))
			return -EPROTO;
		if (geo->packet && bt->opened) {
			skb = bt_skb_alloc(length, GFP_KERNEL);
			if (!skb)
				return -ENOMEM;
			hci_skb_pkt_type(skb) = geo->packet;
			skb_put_data(skb, descriptor + 16, length);
			bt->hdev->stat.byte_rx += length;
			bt->rx_packets++;
			hci_recv_frame(bt->hdev, skb);
		} else if (!geo->packet) {
			bt->diagnostics++;
		}
		slot = (slot + 1) & BT7932_MASK;
	}
	for (i = 0; i < count; i++) {
		bt7932_offer(bt, channel, ring->host);
		ring->host = (ring->host + 1) & BT7932_MASK;
	}
	ring->previous = completed;
	if (count) {
		bt7932_publish_counter(bt, bt->host_transfer, channel, ring->host);
		bt7932_ring_doorbell(bt, geo->doorbell);
	}
	return 0;
}

static irqreturn_t bt7932_irq_thread(int irq, void *data)
{
	struct bt7932 *bt = data;
	u16 producer;
	int channel, ret;

	mutex_lock(&bt->lock);
	if (!bt->ipc_ready || bt->fault || bt->stopping)
		goto out;
	if (READ_ONCE(*(u8 *)(bt->arena + bt->peripheral + 4)) == 4) {
		bt7932_fault_locked(bt, "firmware entered fault state4");
		goto out;
	}
	ret = bt7932_sleep_locked(bt);
	if (ret) {
		bt7932_fault_locked(bt, "invalid sleep notification");
		goto out;
	}
	/* Drain the old offers until quiescence, then stop all publication. */
	if (bt->pm_state == BT7932_QUIESCED || bt->pm_state == BT7932_RESTORING)
		goto out;
	producer = bt7932_counter(bt, bt->cr_producer, 0);
	if (producer != bt->cr_previous) {
		/* Counter retirement only; no invented per-record ACK interpretation. */
		bt7932_publish_counter(bt, bt->cr_consumer, 0, producer);
		bt->cr_previous = producer;
		bt7932_ring_doorbell(bt, 19);
	}
	for (channel = 1; channel < BT7932_CHANNELS; channel++) {
		if (!geometry[channel].stride)
			continue;
		ret = geometry[channel].rx ? bt7932_rx_locked(bt, channel) :
			bt7932_tx_progress(bt, channel);
		if (ret) {
			bt7932_fault_locked(bt, "invalid ring completion or RX allocation failure");
			goto out;
		}
	}
	if (!skb_queue_empty(&bt->tx_queue))
		queue_work(bt->tx_wq, &bt->tx_work);
out:
	mutex_unlock(&bt->lock);
	return IRQ_HANDLED;
}

static int bt7932_download(struct bt7932 *bt)
{
	unsigned long deadline;
	u32 value, boot;
	int ret;

	writel(0x7000188a, bt->bar + BT7932_TRANSPORT_SELECTOR);
	writel(1, bt->bar + BT7932_LTR);
	dma_wmb();
	writel(lower_32_bits(bt->image_dma), bt->bar + BT7932_IMAGE_LO);
	writel(upper_32_bits(bt->image_dma), bt->bar + BT7932_IMAGE_HI);
	writel(bt->image_body, bt->bar + BT7932_IMAGE_LEN);
	writel(0x188d1807, bt->bar + BT7932_CONNECTION_SELECTOR);
	ret = readl_poll_timeout(bt->bar + BT7932_SEMAPHORE, value,
				value == U32_MAX || value & 1, 1000, 5000000);
	if (ret || value == U32_MAX)
		return ret ?: -EIO;
	bt7932_ring_doorbell(bt, 11);
	deadline = jiffies + HZ;
	for (;;) {
		value = readl(bt->bar + BT7932_IMAGE_RESPONSE);
		ret = pci_read_config_dword(bt->pdev, BT7932_BOOT_CONFIG, &boot);
		if (ret || value == U32_MAX || boot == U32_MAX)
			return -EIO;
		if (value == 1 && (boot & 0xf) == 2)
			break;
		if (time_after_eq(jiffies, deadline))
			return -ETIMEDOUT;
		usleep_range(1000, 2000);
	}
	writel(0x188d1807, bt->bar + BT7932_CONNECTION_SELECTOR);
	writel(1, bt->bar + BT7932_SEMAPHORE_RELEASE);
	dev_info(&bt->pdev->dev, "BT_IMAGE_ACCEPTED response1 boot0->2; image remains retained\n");
	return 0;
}

static int bt7932_ipc_state(struct bt7932 *bt, u8 *state)
{
	u32 value;

	dma_rmb();
	*state = READ_ONCE(*(u8 *)(bt->arena + bt->peripheral + 4));
	if (*state <= 1) {
		value = readl(bt->bar + BT7932_STATE);
		if (value == U32_MAX)
			return -EIO;
		/* The reviewed early-state rule replaces memory with the BAR byte. */
		*state = value & 0xff;
	}
	return 0;
}

static int bt7932_start_ipc(struct bt7932 *bt)
{
	unsigned long deadline = jiffies + 10 * HZ;
	bool init_sent = false, context_sent = false;
	u64 context = bt->arena_dma + bt->context;
	u8 state;
	int ret;

	while (time_before(jiffies, deadline)) {
		ret = bt7932_ipc_state(bt, &state);
		if (ret)
			return ret;
		if (!state && !init_sent) {
			writel(1, bt->bar + BT7932_CONTROL);
			bt7932_ring_doorbell(bt, 13);
			init_sent = true;
		} else if (state == 1 && !context_sent) {
			dma_wmb();
			writel(lower_32_bits(context), bt->bar + BT7932_CONTEXT_LO);
			writel(upper_32_bits(context), bt->bar + BT7932_CONTEXT_HI);
			writel(lower_32_bits(bt->arena_dma), bt->bar + BT7932_WINDOW_LO);
			writel(upper_32_bits(bt->arena_dma), bt->bar + BT7932_WINDOW_HI);
			writel(bt->arena_size, bt->bar + BT7932_WINDOW_SIZE);
			writel(2, bt->bar + BT7932_CONTROL);
			bt7932_ring_doorbell(bt, 13);
			context_sent = true;
			deadline = min(deadline, jiffies + HZ);
		} else if (state == 2) {
			return context_sent ? 0 : -EPROTO;
		} else if (state > 2) {
			return -EPROTO;
		}
		usleep_range(1000, 2000);
	}
	return -ETIMEDOUT;
}

static void bt7932_channels(struct bt7932 *bt)
{
	unsigned int channel, index;

	/* Ten control records remain owned and are never reused. */
	for (channel = 1; channel < BT7932_CHANNELS; channel++) {
		const struct bt7932_geometry *geo = &geometry[channel];
		u16 producer = channel - 1;
		u8 *request = bt->arena + bt->requests + producer * BT7932_CONTROL_SIZE;

		if (geo->stride) {
			put_unaligned_le16(0x34, request + 1);
			put_unaligned_le16(producer, request + 0x0c);
			request[0x10] = 1;
			request[0x12] = geo->class;
			request[0x13] = geo->class == 0x81;
			put_unaligned_le32(channel | channel << 16, request + 0x14);
			put_unaligned_le64(bt->arena_dma + bt->rings[channel].offset,
					   request + 0x18);
			put_unaligned_le32(BT7932_DEPTH, request + 0x28);
			put_unaligned_le16(geo->doorbell, request + 0x2c);
			put_unaligned_le16(0x59, request + 0x2e);
			put_unaligned_le32(4, request + 0x30);
			put_unaligned_le32(geo->doorbell, request + 0x34);
			put_unaligned_le16(1000, request + 0x3c);
		}
		bt7932_publish_counter(bt, bt->host_transfer, channel, 0);
		bt7932_publish_counter(bt, bt->fw_transfer, channel, 0);
		bt7932_publish_counter(bt, bt->host_transfer, 0, producer + 1);
		bt7932_ring_doorbell(bt, 20);
	}
	for (channel = 1; channel < BT7932_CHANNELS; channel++) {
		if (!geometry[channel].rx)
			continue;
		for (index = 0; index < BT7932_DEPTH - 1; index++)
			bt7932_offer(bt, channel, index);
		bt->rings[channel].host = BT7932_DEPTH - 1;
		bt7932_publish_counter(bt, bt->host_transfer, channel, BT7932_DEPTH - 1);
		bt7932_ring_doorbell(bt, geometry[channel].doorbell);
	}
	/* The reviewed cold IPC transition starts ACTIVE; no initial sleep ACK. */
	bt->last_sleep = 0;
	WRITE_ONCE(bt->awake, true);
	WRITE_ONCE(bt->ipc_ready, true);
	dev_info(&bt->pdev->dev, "BT_IPC_READY ten retained advertisements;127 RX offers per channel\n");
}

static int bt7932_open(struct hci_dev *hdev)
{
	struct bt7932 *bt = hci_get_drvdata(hdev);
	int ret = 0;

	mutex_lock(&bt->lock);
	if (bt->fault || !bt->ipc_ready || bt->stopping ||
	    bt->pm_state != BT7932_RUNNING)
		ret = -EHOSTDOWN;
	else
		WRITE_ONCE(bt->opened, true);
	mutex_unlock(&bt->lock);
	return ret;
}

static int bt7932_flush(struct hci_dev *hdev)
{
	struct bt7932 *bt = hci_get_drvdata(hdev);

	cancel_work_sync(&bt->tx_work);
	skb_queue_purge(&bt->tx_queue);
	return 0;
}

static int bt7932_close(struct hci_dev *hdev)
{
	struct bt7932 *bt = hci_get_drvdata(hdev);

	WRITE_ONCE(bt->opened, false);
	wake_up_all(&bt->wake_wait);
	return bt7932_flush(hdev);
}

static int bt7932_send(struct hci_dev *hdev, struct sk_buff *skb)
{
	struct bt7932 *bt = hci_get_drvdata(hdev);
	int channel = bt7932_tx_channel(hci_skb_pkt_type(skb));
	unsigned long flags;
	int ret;

	if (READ_ONCE(bt->fault) || !READ_ONCE(bt->opened) ||
	    READ_ONCE(bt->stopping) || READ_ONCE(bt->pm_state) != BT7932_RUNNING)
		return -EHOSTDOWN;
	if (channel < 0 || skb->len > geometry[channel].payload)
		return -EMSGSIZE;
	ret = skb_linearize(skb);
	if (ret)
		return ret;
	if (!bt7932_hci_length(hci_skb_pkt_type(skb), skb->data, skb->len))
		return -EINVAL;
	spin_lock_irqsave(&bt->tx_queue.lock, flags);
	if (READ_ONCE(bt->stopping) || READ_ONCE(bt->pm_state) != BT7932_RUNNING) {
		spin_unlock_irqrestore(&bt->tx_queue.lock, flags);
		return -EHOSTDOWN;
	}
	if (bt->tx_queue.qlen >= BT7932_TX_QUEUE_MAX) {
		spin_unlock_irqrestore(&bt->tx_queue.lock, flags);
		return -EBUSY;
	}
	__skb_queue_tail(&bt->tx_queue, skb);
	spin_unlock_irqrestore(&bt->tx_queue.lock, flags);
	queue_work(bt->tx_wq, &bt->tx_work);
	return 0;
}

static int bt7932_setup_chunks(struct hci_dev *hdev, u8 kind,
			      const u8 *data, size_t length)
{
	u8 command[244];
	size_t offset = 0;
	int ret;

	while (offset < length) {
		size_t chunk = min_t(size_t, length - offset, sizeof(command) - 4);

		command[0] = 1;
		command[1] = 4;
		command[2] = kind;
		command[3] = offset + chunk < length;
		memcpy(command + 4, data + offset, chunk);
		ret = __hci_cmd_sync_status(hdev, 0xfdd0, chunk + 4, command, HCI_INIT_TIMEOUT);
		memzero_explicit(command, sizeof(command));
		if (ret) {
			dev_err(&hdev->dev, "BT_SETUP_CHUNK_FAILED kind=%02x offset=%zu status=%d\n",
				kind, offset, ret);
			return ret;
		}
		offset += chunk;
	}
	return 0;
}

static int bt7932_setup(struct hci_dev *hdev)
{
	struct bt7932 *bt = hci_get_drvdata(hdev);
	const char *stage = "Reset";
	u16 opcode = HCI_OP_RESET;
	int ret;

	ret = __hci_cmd_sync_status(hdev, HCI_OP_RESET, 0, NULL, HCI_INIT_TIMEOUT);
	if (ret)
		goto fault;
	stage = "calibration";
	opcode = 0xfdd0;
	ret = bt7932_setup_chunks(hdev, 0x30, bt->calibration, bt->calibration_size);
	if (ret)
		goto fault;
	stage = "PTX";
	ret = bt7932_setup_chunks(hdev, 0x20, bt->ptx, bt->ptx_size);
	if (ret)
		goto fault;
	stage = "address";
	opcode = 0xfc1a;
	ret = __hci_cmd_sync_status(hdev, 0xfc1a, sizeof(bt->address), bt->address,
				   HCI_INIT_TIMEOUT);
	if (ret)
		goto fault;
	dev_info(&bt->pdev->dev, "BT_HCI_SETUP_OK Reset/calibration/PTX/address completed in order\n");
	return 0;
fault:
	dev_err(&bt->pdev->dev, "BT_HCI_SETUP_FAILED stage=%s opcode=%04x status=%d\n",
		stage, opcode, ret);
	mutex_lock(&bt->lock);
	bt7932_fault_locked(bt, "HCI setup command failed; no retry");
	mutex_unlock(&bt->lock);
	return ret;
}

static void bt7932_reset(struct hci_dev *hdev)
{
	struct bt7932 *bt = hci_get_drvdata(hdev);

	mutex_lock(&bt->lock);
	bt7932_fault_locked(bt, "HCI requested recovery; reset is not qualified");
	mutex_unlock(&bt->lock);
}

static void bt7932_hw_error(struct hci_dev *hdev, u8 code)
{
	bt7932_reset(hdev);
}

static int bt7932_iommu(struct bt7932 *bt)
{
	struct device *dev = &bt->pdev->dev;
	struct iommu_domain *domain = iommu_get_domain_for_dev(dev);
	struct iommu_fwspec *spec = dev_iommu_fwspec_get(dev);
	struct pci_bus *root = bt->pdev->bus;
	struct device_node *target = NULL;
	u32 rid = pci_dev_id(bt->pdev), sid = U32_MAX;
	int ret;

	if (!domain || (domain->type != IOMMU_DOMAIN_DMA &&
			domain->type != IOMMU_DOMAIN_DMA_FQ)) {
		dev_err(dev, "BT_IOMMU_REJECT translated domain required type=%u\n",
			domain ? domain->type : 0);
		return -ENODEV;
	}
	if (!spec || !is_of_node(spec->iommu_fwnode)) {
		dev_err(dev, "BT_IOMMU_REJECT missing OF provider\n");
		return -ENODEV;
	}
	while (root->parent)
		root = root->parent;
	if (!of_device_is_compatible(root->dev.of_node, "apple,t8140-pcie")) {
		dev_err(dev, "BT_IOMMU_REJECT unexpected host node\n");
		return -ENODEV;
	}
	/* Apple DART stores IDs in private stream_maps, not fwspec->ids. */
	ret = of_map_id(root->dev.of_node, rid, "iommu-map", "iommu-map-mask",
			&target, &sid);
	if (ret || !target || sid != 2 ||
	    of_fwnode_handle(target) != spec->iommu_fwnode ||
	    !of_device_is_compatible(target, "apple,t8140-dart")) {
		dev_err(dev, "BT_IOMMU_REJECT RID=%03x SID=%u map_status=%d provider_match=%u\n",
			rid, sid, ret, target && of_fwnode_handle(target) == spec->iommu_fwnode);
		ret = ret ?: -ENODEV;
	} else {
		ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
		if (ret)
			dev_err(dev, "BT_IOMMU_REJECT 32-bit DMA mask status=%d\n", ret);
		else
			dev_info(dev, "BT_IOMMU_ADMITTED RID101->SID2 matched translated DART\n");
	}
	of_node_put(target);
	return ret;
}

static void bt7932_cleanup(struct bt7932 *bt, bool release_dma)
{
	if (bt->irq_requested)
		free_irq(bt->irq, bt);
	if (bt->vectors)
		pci_free_irq_vectors(bt->pdev);
	if (bt->hdev)
		hci_free_dev(bt->hdev);
	if (bt->tx_wq)
		destroy_workqueue(bt->tx_wq);
	if (release_dma && bt->arena)
		dma_free_coherent(&bt->pdev->dev, bt->arena_size, bt->arena, bt->arena_dma);
	if (release_dma && bt->image)
		dma_free_coherent(&bt->pdev->dev, bt->image_size, bt->image, bt->image_dma);
	kfree_sensitive(bt->calibration);
	kfree_sensitive(bt->ptx);
	if (bt->bar)
		pci_iounmap(bt->pdev, bt->bar);
	if (bt->regions)
		pci_release_regions(bt->pdev);
	if (bt->runtime_forbidden)
		pm_runtime_allow(&bt->pdev->dev);
	if (bt->enabled)
		pci_disable_device(bt->pdev);
	kfree_sensitive(bt);
}

static int bt7932_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	struct bt7932 *bt;
	u32 chip, boot = U32_MAX, selector;
	u16 command;
	int ret;

	/* The coordinator opens this only after the real root has its inputs. */
	if (!READ_ONCE(enable))
		return -ENODEV;
	if (!of_machine_is_compatible("apple,j700") ||
	    !of_machine_is_compatible("apple,t8140") ||
	    pci_domain_nr(pdev->bus) || pdev->bus->number != 1 || pdev->devfn != PCI_DEVFN(0, 1))
		return -ENODEV;
	bt = kzalloc_obj(*bt);
	if (!bt)
		return -ENOMEM;
	bt->pdev = pdev;
	bt->last_sleep = U32_MAX;
	mutex_init(&bt->lock);
	init_waitqueue_head(&bt->wake_wait);
	skb_queue_head_init(&bt->tx_queue);
	INIT_WORK(&bt->tx_work, bt7932_tx_work);
	msleep(100);
	ret = pci_enable_device_mem(pdev);
	if (ret)
		goto fail;
	bt->enabled = true;
	if (!(pci_resource_flags(pdev, 0) & IORESOURCE_MEM) ||
	    pci_resource_len(pdev, 0) < BT7932_BAR_SIZE) {
		ret = -EINVAL;
		goto fail;
	}
	ret = pci_read_config_word(pdev, PCI_COMMAND, &command);
	if (ret || command & PCI_COMMAND_MASTER) {
		ret = -EBUSY;
		goto fail;
	}
	ret = bt7932_iommu(bt);
	if (ret)
		goto fail;
	ret = pci_request_regions(pdev, "mt7932_bt_pcie");
	if (ret)
		goto fail;
	bt->regions = true;
	bt->bar = pci_iomap(pdev, 0, BT7932_BAR_SIZE);
	if (!bt->bar) {
		ret = -ENOMEM;
		goto fail;
	}
	dev_info(&pdev->dev, "BT_BAR0 resource=%pR\n", &pdev->resource[0]);
	ret = pci_read_config_dword(pdev, BT7932_BOOT_CONFIG, &boot);
	if (ret || boot == U32_MAX || (boot & 0xf) || ((boot >> 4) & 7) > 1) {
		dev_err(&pdev->dev, "BT_IDENTITY_REJECT config_status=%d boot=%#x\n", ret, boot);
		ret = -ENODEV;
		goto fail;
	}
	bt->rom = (boot >> 4) & 7;
	writel(0x188d7001, bt->bar + BT7932_CONNECTION_SELECTOR);
	selector = readl(bt->bar + BT7932_CONNECTION_SELECTOR);
	chip = readl(bt->bar + BT7932_CHIP_ID);
	if (chip == U32_MAX || (chip & 0xffff) != 0x7932) {
		dev_err(&pdev->dev, "BT_IDENTITY_REJECT boot=%#x selector=%#x chip=%#x\n",
			boot, selector, chip);
		ret = -ENODEV;
		goto fail;
	}
	dev_info(&pdev->dev, "BT_COLD_ADMISSION ROM=%u bootstate0 chip7932 SID2\n", bt->rom);
	ret = bt7932_read_inputs(bt);
	if (ret)
		goto fail;
	ret = bt7932_arena(bt);
	if (ret)
		goto fail;
	bt->tx_wq = alloc_ordered_workqueue("mt7932-bt-tx", WQ_MEM_RECLAIM);
	if (!bt->tx_wq) {
		ret = -ENOMEM;
		goto fail;
	}
	bt->hdev = hci_alloc_dev();
	if (!bt->hdev) {
		ret = -ENOMEM;
		goto fail;
	}
	bt->hdev->bus = HCI_PCI;
	SET_HCIDEV_DEV(bt->hdev, &pdev->dev);
	hci_set_drvdata(bt->hdev, bt);
	bt->hdev->open = bt7932_open;
	bt->hdev->close = bt7932_close;
	bt->hdev->flush = bt7932_flush;
	bt->hdev->send = bt7932_send;
	bt->hdev->setup = bt7932_setup;
	bt->hdev->reset = bt7932_reset;
	bt->hdev->hw_error = bt7932_hw_error;
	hci_set_quirk(bt->hdev, HCI_QUIRK_RESET_ON_CLOSE);
	hci_set_quirk(bt->hdev, HCI_QUIRK_NON_PERSISTENT_SETUP);
	ret = pci_alloc_irq_vectors(pdev, 1, 1, PCI_IRQ_MSI);
	if (ret < 0)
		goto fail;
	bt->vectors = true;
	bt->irq = pci_irq_vector(pdev, 0);
	ret = request_threaded_irq(bt->irq, NULL, bt7932_irq_thread, IRQF_ONESHOT,
				   "mt7932-bt", bt);
	if (ret)
		goto fail;
	bt->irq_requested = true;
	pci_set_drvdata(pdev, bt);
	pm_runtime_forbid(&pdev->dev);
	bt->runtime_forbidden = true;
	/* Failed admission remains bound until removal or a full reset. */
	bt->retained = true;
	pci_set_master(pdev);
	ret = bt7932_download(bt);
	if (ret)
		goto retained_fault;
	ret = bt7932_start_ipc(bt);
	if (ret)
		goto retained_fault;
	mutex_lock(&bt->lock);
	bt7932_channels(bt);
	mutex_unlock(&bt->lock);
	ret = hci_register_dev(bt->hdev);
	if (ret)
		goto retained_fault;
	bt->registered = true;
	dev_info(&pdev->dev, "BT_HCI_REGISTERED; setup/discovery/connection remain separate gates\n");
	return 0;
retained_fault:
	mutex_lock(&bt->lock);
	bt7932_fault_locked(bt, "startup failed after bus mastering/publication");
	mutex_unlock(&bt->lock);
	dev_err(&pdev->dev, "BT_STARTUP_FAILED error=%d; device bound, no retry\n", ret);
	return 0;
fail:
	dev_err(&pdev->dev, "BT_ADMISSION_FAILED error=%d; no DMA published\n", ret);
	bt7932_cleanup(bt, true);
	return ret;
}

/* Poll coherent notifications as well as IRQs, including a lost final edge. */
static int bt7932_wait_pm(struct bt7932 *bt, enum bt7932_pm_state target)
{
	unsigned long limit = jiffies + HZ;
	int ret;

	for (;;) {
		mutex_lock(&bt->lock);
		ret = bt->fault || bt->stopping ? -EIO : bt7932_sleep_locked(bt);
		if (!ret && bt->pm_state == target) {
			mutex_unlock(&bt->lock);
			return 0;
		}
		mutex_unlock(&bt->lock);
		if (ret)
			return ret;
		if (time_after_eq(jiffies, limit))
			return -ETIMEDOUT;
		usleep_range(1000, 2000);
	}
}

static int bt7932_quiesce(struct bt7932 *bt)
{
	int ret;

	ret = bt7932_wake_and_lock(bt, false);
	if (ret)
		return ret;
	WRITE_ONCE(bt->pm_state, BT7932_QUIESCING);
	WRITE_ONCE(bt->awake, false);
	bt7932_request_sleep(bt, 3);
	mutex_unlock(&bt->lock);
	wake_up_all(&bt->wake_wait);
	cancel_work_sync(&bt->tx_work);
	skb_queue_purge(&bt->tx_queue);
	return bt7932_wait_pm(bt, BT7932_QUIESCED);
}

static int bt7932_restore_transport(struct bt7932 *bt)
{
	int ret;

	mutex_lock(&bt->lock);
	if (bt->fault || bt->stopping) {
		mutex_unlock(&bt->lock);
		return -EIO;
	}
	WRITE_ONCE(bt->pm_state, BT7932_RESTORING);
	bt->waking = false;
	bt7932_request_sleep(bt, 0);
	mutex_unlock(&bt->lock);
	ret = bt7932_wait_pm(bt, BT7932_RUNNING);
	if (ret) {
		mutex_lock(&bt->lock);
		bt7932_fault_locked(bt, "system sleep restoration failed");
		mutex_unlock(&bt->lock);
	}
	return ret;
}

static int bt7932_suspend(struct device *dev)
{
	struct bt7932 *bt = pci_get_drvdata(to_pci_dev(dev));
	int ret;

	if (READ_ONCE(bt->fault))
		return -EIO;
	if (!bt->registered)
		return 0;
	ret = hci_suspend_dev(bt->hdev);
	if (ret)
		return ret;
	ret = bt7932_quiesce(bt);
	if (!ret) {
		dev_dbg(dev, "system sleep quiesce acknowledged\n");
		return 0;
	}
	dev_warn(dev, "system sleep quiesce failed: %d\n", ret);
	/* Undo our request before the PM core rolls back the rest of the bus. */
	if (!READ_ONCE(bt->fault))
		bt7932_restore_transport(bt);
	hci_resume_dev(bt->hdev);
	return ret;
}

static int bt7932_resume(struct device *dev)
{
	struct bt7932 *bt = pci_get_drvdata(to_pci_dev(dev));
	int ret;

	if (READ_ONCE(bt->fault))
		return -EIO;
	if (!bt->registered)
		return 0;
	if (READ_ONCE(bt->pm_state) != BT7932_RUNNING) {
		ret = bt7932_restore_transport(bt);
		if (ret)
			return ret;
	}
	dev_dbg(dev, "transport restored after system sleep\n");
	return hci_resume_dev(bt->hdev);
}

static int bt7932_freeze(struct device *dev)
{
	struct bt7932 *bt = pci_get_drvdata(to_pci_dev(dev));

	/* A restored image cannot adopt the running firmware's DMA pointers. */
	return bt->retained ? -EOPNOTSUPP : 0;
}

static const struct dev_pm_ops bt7932_pm_ops = {
	.suspend = bt7932_suspend,
	.resume = bt7932_resume,
	.freeze = bt7932_freeze,
	.thaw = bt7932_resume,
	.poweroff = bt7932_freeze,
	.restore = bt7932_resume,
};

static void bt7932_remove(struct pci_dev *pdev)
{
	struct bt7932 *bt = pci_get_drvdata(pdev);
	u16 command = U16_MAX;
	bool quiesced, drained;

	/* Closing HCI may send Reset: keep the transport and IRQ alive for it. */
	if (bt->registered)
		hci_unregister_dev(bt->hdev);
	quiesced = bt->ipc_ready && !READ_ONCE(bt->fault) && !bt7932_quiesce(bt);
	mutex_lock(&bt->lock);
	WRITE_ONCE(bt->stopping, true);
	WRITE_ONCE(bt->opened, false);
	mutex_unlock(&bt->lock);
	wake_up_all(&bt->wake_wait);
	cancel_work_sync(&bt->tx_work);
	skb_queue_purge(&bt->tx_queue);
	if (bt->irq_requested) {
		free_irq(bt->irq, bt);
		bt->irq_requested = false;
	}
	pci_clear_master(pdev);
	drained = quiesced && !pci_read_config_word(pdev, PCI_COMMAND, &command) &&
		  command != U16_MAX && !(command & PCI_COMMAND_MASTER) &&
		  pci_wait_for_pending_transaction(pdev);
	if (!drained && bt->retained) {
		/* Keep the DMA allocations and their device alive until reset. */
		get_device(&pdev->dev);
		dev_err(&pdev->dev, "PCI drain unconfirmed; DMA retained until reset\n");
	}
	pci_set_drvdata(pdev, NULL);
	bt7932_cleanup(bt, drained || !bt->retained);
}

static const struct pci_device_id bt7932_ids[] = {
	{ PCI_DEVICE(PCI_VENDOR_ID_MEDIATEK, 0x793b) },
	{ }
};
MODULE_DEVICE_TABLE(pci, bt7932_ids);

static struct pci_driver bt7932_driver = {
	.name = "mt7932_bt_pcie",
	.id_table = bt7932_ids,
	.probe = bt7932_probe,
	.remove = bt7932_remove,
	.shutdown = bt7932_remove,
	.driver = {
		.pm = pm_sleep_ptr(&bt7932_pm_ops),
	},
};
module_pci_driver(bt7932_driver);

MODULE_DESCRIPTION("Experimental standalone J700 MT7932 Bluetooth PCIe transport");
MODULE_LICENSE("GPL");
MODULE_FIRMWARE(BT7932_FW_B0);
MODULE_FIRMWARE(BT7932_FW_B1);
MODULE_FIRMWARE(BT7932_FW_B1_FALLBACK);
MODULE_FIRMWARE(BT7932_PTX);
MODULE_FIRMWARE(BT7932_CAL);
MODULE_FIRMWARE(BT7932_ADDR);
