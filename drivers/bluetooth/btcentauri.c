// SPDX-License-Identifier: GPL-2.0-only
/* Apple N1 Beta command/event transport for J813 bring-up.
 *
 * The Centauri Control driver must have booted the shared firmware first.
 * This driver owns only Beta. It never resets the port or the Wi-Fi function.
 * BR/EDR discovery, ACL/SDP, pairing and encrypted AVDTP discovery are qualified.
 * ACL remains opt-in; LE data, persistent bonds and audio are unqualified.
 * SCO/ISO delivery is not implemented.
 */
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/pci.h>
#include <linux/random.h>
#include <linux/seq_file.h>
#include <linux/sizes.h>
#include <linux/unaligned.h>
#include <linux/vmalloc.h>
#include <linux/workqueue.h>
#include <net/bluetooth/bluetooth.h>
#include <net/bluetooth/hci_core.h>

#define CEN_ARENA_SIZE SZ_1M
#define CEN_MTR 0x2000
#define CEN_MCR 0x2200
#define CEN_MESSAGES 0x4000
#define CEN_RX 0x8000
#define CEN_TX 0xa000
#define CEN_FOOTER 260
#define CEN_STRIDE (16 + CEN_FOOTER)
#define CEN_RX_SLOTS 17
#define CEN_TX_SLOTS 5
#define CEN_LE_RX 0xc000
#define CEN_LE_TX 0xe000
#define CEN_LE_FOOTER 256
#define CEN_LE_STRIDE (16 + CEN_LE_FOOTER)
#define CEN_LE_SLOTS 17
#define CEN_MESSAGE_SLOTS 16
#define CEN_AUX_BASE 0x20000
#define CEN_CAPTURE_SIZE SZ_1M

static bool arm_le_data_rings = true;
module_param(arm_le_data_rings, bool, 0444);
MODULE_PARM_DESC(arm_le_data_rings, "Arm native Beta LE rings for radio bring-up");

static bool arm_auxiliary_rings;
module_param(arm_auxiliary_rings, bool, 0444);
MODULE_PARM_DESC(arm_auxiliary_rings, "Diagnostic: arm remaining native Beta resources and capture received frames privately");

static bool native_startup;
module_param(native_startup, bool, 0444);
MODULE_PARM_DESC(native_startup, "Diagnostic: program a temporary address, InitDone and random seed before the first HCI reset");

static bool enable_acl;
module_param(enable_acl, bool, 0444);
MODULE_PARM_DESC(enable_acl, "Experimental ACL data delivery; requires LE and auxiliary rings");

static bool native_hrb;
module_param(native_hrb, bool, 0444);
MODULE_PARM_DESC(native_hrb, "Experimental native Proxima Create Connection; requires native startup, ACL and firmware capability bit 9");

#define CEN_OP_READ_PROP_FEATURES 0xffa3
#define CEN_OP_HRB_CREATE_CONN 0xff7b
#define CEN_PROP_HRB_CREATE BIT(9)

struct cen_aux_spec {
	u16 id, slots, footer;
	bool receive;
};

/* Host native Beta inventory; each resource is opt-in until target-qualified.
 * HCI and LE rings remain in their existing, separately qualified arenas.
 */
static const struct cen_aux_spec cen_aux_specs[] = {
	{ 2, 9, 2592, false }, /* ACL TX */
	{ 3, 9, 724, false },  /* SCO TX */
	{ 5, 9, 260, false },  /* LEA TX */
	{ 6, 5, 1024, false }, /* LLA TX */
	{ 8, 9, 2592, true },  /* ACL RX */
	{ 9, 9, 724, true },   /* SCO RX */
	{ 11, 9, 268, true },  /* LEA RX */
	{ 12, 128, 1024, true }, /* Firmware debug RX */
	{ 13, 17, 260, true }, /* LMP RX */
	{ 14, 9, 1304, false }, /* TSI TX */
	{ 15, 9, 1304, true }, /* TSI RX */
};

struct cen_aux_state {
	u32 offset, packets;
	u16 head, post;
};

struct cen_bt {
	struct pci_dev *pdev;
	struct hci_dev *hdev;
	struct dentry *debug;
	void __iomem *bar;
	u8 *arena;
	dma_addr_t dma;
	struct mutex lock;
	struct delayed_work poll;
	struct sk_buff_head txq;
	struct sk_buff_head acl_txq, le_txq;
	u16 acl_tx_head, le_tx_head;
	u16 message_head, message_tag, tx_head, rx_head, rx_post;
	u16 le_rx_head, le_rx_post;
	u32 le_rx_packets;
	struct cen_aux_state aux[ARRAY_SIZE(cen_aux_specs)];
	u8 *capture;
	u32 capture_used, capture_records, capture_dropped;
	bool ipc_requested, running;
	u8 native_startup_steps;
	bool hrb_supported, hrb_status_pending;
	u32 proprietary_features, hrb_commands, hrb_status_events;
	u32 hrb_connection_events;
	u8 hrb_last_link_type;
};

static u16 cen_tail(struct cen_bt *bt, u16 ring)
{
	return le16_to_cpu(READ_ONCE(*(__le16 *)(bt->arena + 0x7a + ring * 2)));
}

static void cen_post(struct cen_bt *bt, u16 ring, u16 value)
{
	put_unaligned_le16(value, bt->arena + 0xa8 + ring * 2);
	dma_wmb();
	writel(value, bt->bar + 0x1000 + ring * 4);
}

static int cen_message(struct cen_bt *bt, const void *data, size_t length)
{
	u16 head = bt->message_head, tail, next = (head + 1) % CEN_MESSAGE_SLOTS;
	u16 tag = bt->message_tag + 1;
	u8 *descriptor = bt->arena + CEN_MTR + head * 16;
	u8 *completion = bt->arena + CEN_MCR + head * 32;
	int ret;

	if (head >= CEN_MESSAGE_SLOTS || !length || length > 512)
		return -EINVAL;
	memset(descriptor, 0, 16);
	memset(completion, 0, 32);
	memcpy(bt->arena + CEN_MESSAGES + head * 512, data, length);
	put_unaligned_le32(1 | (length << 8), descriptor);
	put_unaligned_le64(bt->dma + CEN_MESSAGES + head * 512, descriptor + 4);
	put_unaligned_le16(tag, descriptor + 12);
	cen_post(bt, 0, next);
	ret = read_poll_timeout(cen_tail, tail, tail == next,
			      100, 1500000, false, bt, 0);
	if (ret)
		return ret;
	/* The device publishes the message completion before advancing its head. */
	ret = read_poll_timeout(le16_to_cpu, tail, tail == next,
			      100, 1500000, false,
			      READ_ONCE(*(__le16 *)(bt->arena + 0x78)));
	if (ret)
		return ret;
	dma_rmb();
	if (get_unaligned_le16(completion + 4) != tag ||
	    get_unaligned_le16(completion + 6) != length ||
	    get_unaligned_le32(completion + 8))
		return -EPROTO;
	bt->message_head = next;
	bt->message_tag = tag;
	put_unaligned_le16(bt->message_head, bt->arena + 0xa6);
	dma_wmb();
	return 0;
}

static int cen_open_ring(struct cen_bt *bt, u16 id, u16 slots, u32 offset, u16 footer)
{
	u8 message[0x34] = { 1 };
	u32 words = footer / 4, shift = 0;

	if (!words || footer % 4)
		return -EINVAL;
	if (words > 255)
		shift = min_t(u32, __ffs(words), 4);
	if ((words >> shift) > 255)
		return -EINVAL;
	message[2] = words >> shift;
	message[3] = shift << 4;
	put_unaligned_le16(id, message + 4);
	put_unaligned_le64(bt->dma + offset, message + 8);
	put_unaligned_le64(~0ULL, message + 0x10);
	put_unaligned_le16(slots, message + 0x18);
	put_unaligned_le16(0xffff, message + 0x1a);
	put_unaligned_le16(id, message + 0x1c);
	put_unaligned_le16(0x40, message + 0x1e);
	return cen_message(bt, message, sizeof(message));
}

static void cen_rx_buffer(struct cen_bt *bt, u16 slot)
{
	u8 *descriptor = bt->arena + CEN_RX + slot * CEN_STRIDE;

	memset(descriptor, 0, CEN_STRIDE);
	put_unaligned_le32(1 | (CEN_FOOTER << 8), descriptor);
	put_unaligned_le16(slot + 1, descriptor + 12);
}

static void cen_le_rx_buffer(struct cen_bt *bt, u16 slot)
{
	u8 *descriptor = bt->arena + CEN_LE_RX + slot * CEN_LE_STRIDE;

	memset(descriptor, 0, CEN_LE_STRIDE);
	put_unaligned_le32(1 | (CEN_LE_FOOTER << 8), descriptor);
	put_unaligned_le16(slot + 1, descriptor + 12);
}

static void cen_capture_frame(struct cen_bt *bt, u16 ring, const u8 *data, u16 length)
{
	u8 *record;

	if (!bt->capture)
		return;
	if (8U + length > CEN_CAPTURE_SIZE - bt->capture_used) {
		bt->capture_dropped++;
		return;
	}
	record = bt->capture + bt->capture_used;
	put_unaligned_le16(ring, record);
	put_unaligned_le16(length, record + 2);
	put_unaligned_le32(bt->capture_records++, record + 4);
	memcpy(record + 8, data, length);
	bt->capture_used += 8 + length;
}

static int cen_receive_acl(struct cen_bt *bt, const u8 *data, u32 length)
{
	struct sk_buff *skb;
	u32 packet_length, pos;

	if (length < HCI_ACL_HDR_SIZE)
		return -EPROTO;
	packet_length = HCI_ACL_HDR_SIZE + get_unaligned_le16(data + 2);
	if (packet_length > length)
		return -EPROTO;
	/* Airship RX attributes follow, rather than belong to, the HCI packet.
	 * Only the native parser's known record sizes are accepted. Their values
	 * remain opaque and are retained only in the private diagnostic capture.
	 */
	for (pos = packet_length; pos < length;) {
		if (!data[pos])
			break;
		if (data[pos] == 1)
			pos++;
		else if (data[pos] == 2 && length - pos >= 9)
			pos += 9;
		else
			return -EPROTO;
	}
	skb = bt_skb_alloc(packet_length, GFP_KERNEL);
	if (!skb) {
		bt->hdev->stat.err_rx++;
		return 0;
	}
	skb_put_data(skb, data, packet_length);
	hci_skb_pkt_type(skb) = HCI_ACLDATA_PKT;
	bt->hdev->stat.byte_rx += packet_length;
	hci_recv_frame(bt->hdev, skb);
	return 0;
}

static void cen_aux_rx_buffer(struct cen_bt *bt, unsigned int index, u16 slot)
{
	const struct cen_aux_spec *spec = &cen_aux_specs[index];
	u8 *descriptor = bt->arena + bt->aux[index].offset + slot * (16 + spec->footer);

	memset(descriptor, 0, 16 + spec->footer);
	put_unaligned_le32(1 | (spec->footer << 8), descriptor);
	put_unaligned_le16(slot + 1, descriptor + 12);
}

static int cen_aux_start(struct cen_bt *bt)
{
	u32 offset = CEN_AUX_BASE;
	unsigned int i, slot;
	int ret;

	for (i = 0; i < ARRAY_SIZE(cen_aux_specs); i++) {
		const struct cen_aux_spec *spec = &cen_aux_specs[i];
		struct cen_aux_state *state = &bt->aux[i];
		u32 length = spec->slots * (16U + spec->footer);

		if (offset > CEN_ARENA_SIZE || length > CEN_ARENA_SIZE - offset)
			return -ENOSPC;
		state->offset = offset;
		offset = ALIGN(offset + length, SZ_4K);
		ret = cen_open_ring(bt, spec->id, spec->slots, state->offset, spec->footer);
		if (ret) {
			dev_err(&bt->pdev->dev, "Auxiliary ring %u open failed: %d\n", spec->id, ret);
			return ret;
		}
		if (!spec->receive)
			continue;
		for (slot = 0; slot < spec->slots - 1; slot++)
			cen_aux_rx_buffer(bt, i, slot);
		state->post = spec->slots - 1;
		cen_post(bt, spec->id, state->post);
	}
	return 0;
}

static int cen_aux_drain(struct cen_bt *bt, bool repost)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(cen_aux_specs); i++) {
		const struct cen_aux_spec *spec = &cen_aux_specs[i];
		struct cen_aux_state *state = &bt->aux[i];
		u16 tail;
		int budget = spec->slots;

		if (!spec->receive)
			continue;
		tail = cen_tail(bt, spec->id);
		if (tail >= spec->slots)
			return -EPROTO;
		while (state->head != tail && budget--) {
			u8 *descriptor = bt->arena + state->offset + state->head * (16 + spec->footer);
			u32 length;

			dma_rmb();
			length = get_unaligned_le32(descriptor) >> 8;
			if (length > spec->footer)
				return -EPROTO;
			cen_capture_frame(bt, spec->id, descriptor + 16, length);
			if (repost && enable_acl && spec->id == 8 &&
			    cen_receive_acl(bt, descriptor + 16, length))
				return -EPROTO;
			state->packets++;
			state->head = (state->head + 1) % spec->slots;
			if (repost) {
				cen_aux_rx_buffer(bt, i, state->post);
				state->post = (state->post + 1) % spec->slots;
				cen_post(bt, spec->id, state->post);
			}
		}
	}
	return 0;
}

static int cen_start(struct cen_bt *bt)
{
	static const u16 indices[] = { 0x78, 0x7a, 0xa6, 0xa8 };
	u8 config[4] = { 9, 0, 0xff, 0xff };
	u32 status;
	int i, ret;

	put_unaligned_le16(0x300, bt->arena);
	put_unaligned_le16(0x68, bt->arena + 2);
	put_unaligned_le64(bt->dma + 0x68, bt->arena + 8);
	for (i = 0; i < ARRAY_SIZE(indices); i++)
		put_unaligned_le64(bt->dma + indices[i], bt->arena + 0x10 + i * 8);
	put_unaligned_le16(1, bt->arena + 0x30);
	put_unaligned_le16(22, bt->arena + 0x32);
	put_unaligned_le64(bt->dma + CEN_MCR, bt->arena + 0x34);
	put_unaligned_le64(bt->dma + CEN_MTR, bt->arena + 0x3c);
	put_unaligned_le16(16, bt->arena + 0x44);
	put_unaligned_le16(16, bt->arena + 0x46);
	/* The 25G76 native context encodes the MCR footer in four-byte words. */
	bt->arena[0x53] = 4;
	put_unaligned_le64(bt->dma + 0x10000, bt->arena + 0x58);
	pci_set_master(bt->pdev);
	writel(lower_32_bits(bt->dma), bt->bar + 0xc);
	writel(upper_32_bits(bt->dma), bt->bar + 0x10);
	writel(CEN_ARENA_SIZE, bt->bar + 0x14);
	bt->ipc_requested = true;
	writel(1, bt->bar + 0x107c);
	ret = readl_poll_timeout(bt->bar, status, status == 1, 100, 2000000);
	if (ret)
		return ret;
	dma_wmb();
	writel(lower_32_bits(bt->dma), bt->bar + 4);
	writel(upper_32_bits(bt->dma), bt->bar + 8);
	writel(2, bt->bar + 0x107c);
	ret = readl_poll_timeout(bt->bar, status, status == 2, 100, 2000000);
	if (ret)
		return ret;
	ret = cen_message(bt, config, sizeof(config));
	if (!ret)
		ret = cen_open_ring(bt, 7, CEN_RX_SLOTS, CEN_RX, CEN_FOOTER);
	if (!ret)
		ret = cen_open_ring(bt, 1, CEN_TX_SLOTS, CEN_TX, CEN_FOOTER);
	if (!ret && arm_le_data_rings)
		ret = cen_open_ring(bt, 10, CEN_LE_SLOTS, CEN_LE_RX, CEN_LE_FOOTER);
	if (!ret && arm_le_data_rings)
		ret = cen_open_ring(bt, 4, CEN_LE_SLOTS, CEN_LE_TX, CEN_LE_FOOTER);
	if (ret)
		return ret;
	for (i = 0; i < CEN_RX_SLOTS - 1; i++)
		cen_rx_buffer(bt, i);
	bt->rx_post = CEN_RX_SLOTS - 1;
	cen_post(bt, 7, bt->rx_post);
	if (arm_le_data_rings) {
		for (i = 0; i < CEN_LE_SLOTS - 1; i++)
			cen_le_rx_buffer(bt, i);
		bt->le_rx_post = CEN_LE_SLOTS - 1;
		cen_post(bt, 10, bt->le_rx_post);
	}
	if (arm_auxiliary_rings)
		return cen_aux_start(bt);
	return 0;
}

static int cen_transmit(struct cen_bt *bt, struct sk_buff_head *queue,
			u16 ring, u16 slots, u32 offset, u16 footer, u16 *head)
{
	struct sk_buff *skb;
	u16 tail, next;
	u8 *descriptor;
	u32 wire_length;

	while (!skb_queue_empty(queue)) {
		tail = cen_tail(bt, ring);
		if (tail >= slots)
			return -EPROTO;
		next = (*head + 1) % slots;
		if (next == tail)
			break;
		dma_rmb();
		skb = skb_dequeue(queue);
		if (!skb)
			break;
		descriptor = bt->arena + offset + *head * (16 + footer);
		memset(descriptor, 0, 16 + footer);
		put_unaligned_le16(*head + 1, descriptor + 12);
		memcpy(descriptor + 16, skb->data, skb->len);
		wire_length = skb->len;
		if (ring == 1 && READ_ONCE(bt->hrb_supported) &&
		    get_unaligned_le16(skb->data) == HCI_OP_CREATE_CONN) {
			u8 *command = descriptor + 16;

			/* Native Proxima uses the ordinary 13-byte parameter layout
			 * followed by two zero HRB fields (no forced band). It sends
			 * tagging=false and role_switch=0. Address byte order matches
			 * standard HCI. Keep the core's sent skb/opcode unchanged.
			 */
			put_unaligned_le16(CEN_OP_HRB_CREATE_CONN, command);
			command[2] = 15;
			command[HCI_COMMAND_HDR_SIZE + 9] = 0;
			command[HCI_COMMAND_HDR_SIZE + 12] = 0;
			wire_length = HCI_COMMAND_HDR_SIZE + 15;
			bt->hrb_status_pending = true;
			bt->hrb_commands++;
			cen_capture_frame(bt, 1, command, wire_length);
		}
		put_unaligned_le32(1 | (wire_length << 8), descriptor);
		*head = next;
		cen_post(bt, ring, next);
		if (hci_skb_pkt_type(skb) == HCI_COMMAND_PKT)
			bt->hdev->stat.cmd_tx++;
		else
			bt->hdev->stat.acl_tx++;
		bt->hdev->stat.byte_tx += wire_length;
		kfree_skb(skb);
	}
	return 0;
}

static void cen_poll(struct work_struct *work)
{
	struct cen_bt *bt = container_of(to_delayed_work(work), struct cen_bt, poll);
	struct hci_dev *hdev = bt->hdev;
	struct sk_buff *skb;
	u16 tail;
	u8 *descriptor;
	u32 length;
	int budget = CEN_RX_SLOTS;

	mutex_lock(&bt->lock);
	if (!bt->running)
		goto unlock;
	if (readl(bt->bar + 0x18) != 2 || readl(bt->bar) != 2) {
		/* Preserve already completed diagnostics after abort, without posting
		 * any more buffers or ringing doorbells to the failed firmware.
		 */
		if (arm_auxiliary_rings && readl(bt->bar) == 2)
			cen_aux_drain(bt, false);
		goto protocol_error;
	}
	if (arm_auxiliary_rings && cen_aux_drain(bt, true))
		goto protocol_error;
	if (arm_le_data_rings) {
		tail = cen_tail(bt, 10);
		if (tail >= CEN_LE_SLOTS)
			goto protocol_error;
		while (bt->le_rx_head != tail && budget--) {
			dma_rmb();
			descriptor = bt->arena + CEN_LE_RX + bt->le_rx_head * CEN_LE_STRIDE;
			length = get_unaligned_le32(descriptor) >> 8;
			if (length > CEN_LE_FOOTER)
				goto protocol_error;
			bt->le_rx_packets++;
			cen_capture_frame(bt, 10, descriptor + 16, length);
			if (enable_acl && cen_receive_acl(bt, descriptor + 16, length))
				goto protocol_error;
			bt->le_rx_head = (bt->le_rx_head + 1) % CEN_LE_SLOTS;
			cen_le_rx_buffer(bt, bt->le_rx_post);
			bt->le_rx_post = (bt->le_rx_post + 1) % CEN_LE_SLOTS;
			cen_post(bt, 10, bt->le_rx_post);
		}
		budget = CEN_RX_SLOTS;
	}
	tail = cen_tail(bt, 7);
	if (tail >= CEN_RX_SLOTS)
		goto protocol_error;
	while (bt->rx_head != tail && budget--) {
		dma_rmb();
		descriptor = bt->arena + CEN_RX + bt->rx_head * CEN_STRIDE;
		length = get_unaligned_le32(descriptor) >> 8;
		if (length < HCI_EVENT_HDR_SIZE || length > CEN_FOOTER ||
		    length != descriptor[17] + HCI_EVENT_HDR_SIZE)
			goto protocol_error;
		skb = bt_skb_alloc(length, GFP_KERNEL);
		if (skb) {
			skb_put_data(skb, descriptor + 16, length);
			/* The firmware's status belongs to the standard connection
			 * request tracked by Linux. Capture the native response before
			 * restoring only its opcode; preserve status and command credit.
			 */
			if (bt->hrb_status_pending && length == 6 &&
			    skb->data[0] == HCI_EV_CMD_STATUS &&
			    get_unaligned_le16(skb->data + 4) == CEN_OP_HRB_CREATE_CONN) {
				cen_capture_frame(bt, 7, skb->data, length);
				put_unaligned_le16(HCI_OP_CREATE_CONN, skb->data + 4);
				bt->hrb_status_pending = false;
				bt->hrb_status_events++;
			}
			/* Native HCI flow treats 0xf0/0xf1 Connection Complete as
			 * ACL, with additional proprietary capability flags. Linux
			 * otherwise cannot find its pending ACL connection by type.
			 * Preserve the original event privately before normalizing it.
			 */
			if (READ_ONCE(bt->hrb_supported) &&
			    length == HCI_EVENT_HDR_SIZE + sizeof(struct hci_ev_conn_complete) &&
			    skb->data[0] == HCI_EV_CONN_COMPLETE &&
			    (skb->data[11] & 0xfe) == 0xf0) {
				cen_capture_frame(bt, 7, skb->data, length);
				bt->hrb_last_link_type = skb->data[11];
				skb->data[11] = ACL_LINK;
				bt->hrb_connection_events++;
			}
			hci_skb_pkt_type(skb) = HCI_EVENT_PKT;
			hdev->stat.byte_rx += length;
			hci_recv_frame(hdev, skb);
		} else {
			hdev->stat.err_rx++;
		}
		bt->rx_head = (bt->rx_head + 1) % CEN_RX_SLOTS;
		cen_rx_buffer(bt, bt->rx_post);
		bt->rx_post = (bt->rx_post + 1) % CEN_RX_SLOTS;
		cen_post(bt, 7, bt->rx_post);
	}
	if (cen_transmit(bt, &bt->txq, 1, CEN_TX_SLOTS, CEN_TX,
			 CEN_FOOTER, &bt->tx_head))
		goto protocol_error;
	/* cen_aux_specs[0] is the native BR/EDR ACL TX resource (ring 2). */
	if (enable_acl &&
	    (cen_transmit(bt, &bt->acl_txq, 2, cen_aux_specs[0].slots,
			  bt->aux[0].offset, cen_aux_specs[0].footer,
			  &bt->acl_tx_head) ||
	     cen_transmit(bt, &bt->le_txq, 4, CEN_LE_SLOTS, CEN_LE_TX,
			  CEN_LE_FOOTER, &bt->le_tx_head)))
		goto protocol_error;
	schedule_delayed_work(&bt->poll, msecs_to_jiffies(1));
	goto unlock;
protocol_error:
	bt->running = false;
	hdev->stat.err_rx++;
	dev_err(&bt->pdev->dev, "Beta transport stopped (exec=%u ipc=%u); polling disabled\n",
		readl(bt->bar + 0x18), readl(bt->bar));
unlock:
	mutex_unlock(&bt->lock);
}

static int cen_hci_open(struct hci_dev *hdev)
{
	struct cen_bt *bt = hci_get_drvdata(hdev);

	mutex_lock(&bt->lock);
	bt->running = true;
	schedule_delayed_work(&bt->poll, 0);
	mutex_unlock(&bt->lock);
	return 0;
}

static int cen_hci_close(struct hci_dev *hdev)
{
	struct cen_bt *bt = hci_get_drvdata(hdev);

	mutex_lock(&bt->lock);
	bt->running = false;
	bt->hrb_status_pending = false;
	mutex_unlock(&bt->lock);
	cancel_delayed_work_sync(&bt->poll);
	skb_queue_purge(&bt->txq);
	skb_queue_purge(&bt->acl_txq);
	skb_queue_purge(&bt->le_txq);
	return 0;
}

static int cen_vendor_command(struct hci_dev *hdev, u16 opcode,
			      const void *data, u8 length)
{
	struct sk_buff *skb;
	int ret;

	skb = __hci_cmd_sync(hdev, opcode, length, data, HCI_INIT_TIMEOUT);
	if (IS_ERR(skb))
		return PTR_ERR(skb);
	ret = skb->len < 1 ? -EPROTO : -bt_to_errno(skb->data[0]);
	kfree_skb(skb);
	return ret;
}

static int cen_hci_post_init(struct hci_dev *hdev)
{
	struct cen_bt *bt = hci_get_drvdata(hdev);
	struct sk_buff *skb;
	u32 features;
	int ret;

	if (!native_hrb)
		return 0;
	WRITE_ONCE(bt->hrb_supported, false);
	skb = __hci_cmd_sync(hdev, CEN_OP_READ_PROP_FEATURES, 0, NULL,
			     HCI_INIT_TIMEOUT);
	if (IS_ERR(skb))
		return PTR_ERR(skb);
	ret = skb->len == 5 ? -bt_to_errno(skb->data[0]) : -EPROTO;
	features = skb->len == 5 ? get_unaligned_le32(skb->data + 1) : 0;
	kfree_skb(skb);
	WRITE_ONCE(bt->proprietary_features, features);
	if (ret)
		return ret;
	if (!(features & CEN_PROP_HRB_CREATE))
		return -EOPNOTSUPP;
	WRITE_ONCE(bt->hrb_supported, true);
	bt_dev_info(hdev, "Native Proxima Create Connection capability confirmed");
	return 0;
}

static int cen_hci_setup(struct hci_dev *hdev)
{
	struct cen_bt *bt = hci_get_drvdata(hdev);
	u8 address[6], init_done[16], seed[24];
	int ret;

	if (!native_startup)
		return 0;

	/* Native PROXIMA initialization starts with these three commands, then
	 * HCI Reset. The core calls setup before its ordinary reset/init sequence.
	 * Address provisioning is still diagnostic: use a temporary locally
	 * administered address, in native WriteBdAddr display/network byte order.
	 */
	ret = get_random_bytes_wait(address, sizeof(address));
	if (ret)
		return ret;
	address[0] = (address[0] & 0xfc) | 0x02;
	get_random_bytes(seed, sizeof(seed));
	memset(init_done, 0xff, sizeof(init_done));
	ret = cen_vendor_command(hdev, 0xfc06, address, sizeof(address));
	if (ret)
		goto out;
	WRITE_ONCE(bt->native_startup_steps, 1);
	ret = cen_vendor_command(hdev, 0xff0b, init_done, sizeof(init_done));
	if (ret)
		goto out;
	WRITE_ONCE(bt->native_startup_steps, 2);
	ret = cen_vendor_command(hdev, 0xfe44, seed, sizeof(seed));
	if (!ret)
		WRITE_ONCE(bt->native_startup_steps, 3);
out:
	memzero_explicit(seed, sizeof(seed));
	if (ret)
		bt_dev_err(hdev, "Native startup stopped after %u commands: %d",
			   bt->native_startup_steps, ret);
	else
		bt_dev_info(hdev, "Native startup accepted; temporary diagnostic address in use");
	return ret;
}

static int cen_hci_flush(struct hci_dev *hdev)
{
	struct cen_bt *bt = hci_get_drvdata(hdev);

	skb_queue_purge(&bt->txq);
	skb_queue_purge(&bt->acl_txq);
	skb_queue_purge(&bt->le_txq);
	return 0;
}

static int cen_hci_send(struct hci_dev *hdev, struct sk_buff *skb)
{
	struct cen_bt *bt = hci_get_drvdata(hdev);
	struct sk_buff_head *queue = &bt->txq;
	struct hci_conn *conn;
	u16 handle;
	u8 link_type;
	int ret = 0;

	if (hci_skb_pkt_type(skb) == HCI_ACLDATA_PKT && enable_acl) {
		if (skb->len < HCI_ACL_HDR_SIZE ||
		    skb->len != HCI_ACL_HDR_SIZE + get_unaligned_le16(skb->data + 2)) {
			ret = -EMSGSIZE;
			goto out;
		}
		handle = get_unaligned_le16(skb->data);
		rcu_read_lock();
		conn = hci_conn_hash_lookup_handle(hdev, hci_handle(handle));
		link_type = conn ? conn->type : 0xff;
		rcu_read_unlock();
		if (link_type == LE_LINK && (handle & 0x3000) != 0x3000) {
			queue = &bt->le_txq;
			if (skb->len > CEN_LE_FOOTER)
				ret = -EMSGSIZE;
		} else if (link_type == ACL_LINK) {
			queue = &bt->acl_txq;
			if (skb->len > cen_aux_specs[0].footer)
				ret = -EMSGSIZE;
		} else {
			ret = -EOPNOTSUPP;
		}
		if (ret)
			goto out;
	} else if (hci_skb_pkt_type(skb) != HCI_COMMAND_PKT) {
		ret = -EOPNOTSUPP;
		goto out;
	} else if (skb->len < HCI_COMMAND_HDR_SIZE || skb->len > CEN_FOOTER ||
		   skb->len != HCI_COMMAND_HDR_SIZE + skb->data[2]) {
		ret = -EMSGSIZE;
		goto out;
	}
	if (hci_skb_pkt_type(skb) == HCI_COMMAND_PKT && native_hrb) {
		u16 opcode = get_unaligned_le16(skb->data);

		/* Do not mix unmanaged native commands with translated requests. */
		if (opcode == CEN_OP_HRB_CREATE_CONN) {
			ret = -EOPNOTSUPP;
			goto out;
		}
		if (opcode == HCI_OP_CREATE_CONN &&
		    skb->len != HCI_COMMAND_HDR_SIZE + sizeof(struct hci_cp_create_conn)) {
			ret = -EMSGSIZE;
			goto out;
		}
	}
	mutex_lock(&bt->lock);
	if (!bt->running)
		ret = -ENETDOWN;
	else if (skb_queue_len(queue) >= 64)
		ret = -ENOBUFS;
	else {
		skb_queue_tail(queue, skb);
		mod_delayed_work(system_wq, &bt->poll, 0);
	}
	mutex_unlock(&bt->lock);
	if (!ret)
		return 0;
out:
	/* hci_send_frame() frees rejected packets. Ownership transfers to this
	 * driver only when the skb is queued and send returns success.
	 */
	return ret;
}

static void cen_stop(struct cen_bt *bt)
{
	pci_clear_master(bt->pdev);
	kvfree(bt->capture);
	bt->capture = NULL;
	/* IPC control 0 caused a firmware abort in the initial hardware probe.
	 * Until coordinated firmware teardown is understood, retain any published
	 * arena after detaching. A fresh firmware boot is required before rebinding.
	 */
	if (bt->ipc_requested || !pci_wait_for_pending_transaction(bt->pdev))
		dev_warn(&bt->pdev->dev, "Beta detach requires fresh firmware boot; retaining DMA arena\n");
	else
		dma_free_coherent(&bt->pdev->dev, CEN_ARENA_SIZE, bt->arena, bt->dma);
}

static int cen_status_show(struct seq_file *s, void *unused)
{
	struct cen_bt *bt = s->private;
	unsigned int i;

	mutex_lock(&bt->lock);
	seq_printf(s, "exec=%u ipc=%u running=%u tx_head=%u tx_consumed=%u rx_head=%u rx_consumed=%u queued=%u\n",
		   readl(bt->bar + 0x18), readl(bt->bar), bt->running,
		   bt->tx_head, cen_tail(bt, 1), bt->rx_head, cen_tail(bt, 7),
		   skb_queue_len(&bt->txq));
	seq_printf(s, "commands=%u events=%u tx_bytes=%u rx_bytes=%u tx_errors=%u rx_errors=%u\n",
		   bt->hdev->stat.cmd_tx, bt->hdev->stat.evt_rx,
		   bt->hdev->stat.byte_tx, bt->hdev->stat.byte_rx,
		   bt->hdev->stat.err_tx, bt->hdev->stat.err_rx);
	seq_printf(s, "le_rings=%u le_rx_packets=%u\n", arm_le_data_rings, bt->le_rx_packets);
	seq_printf(s, "native_startup=%u native_startup_steps=%u\n",
		   native_startup, READ_ONCE(bt->native_startup_steps));
	seq_printf(s, "native_hrb=%u hrb_supported=%u proprietary_features=0x%08x hrb_commands=%u hrb_status_events=%u hrb_pending=%u\n",
		   native_hrb, READ_ONCE(bt->hrb_supported),
		   READ_ONCE(bt->proprietary_features), bt->hrb_commands,
		   bt->hrb_status_events, bt->hrb_status_pending);
	seq_printf(s, "hrb_connection_events=%u hrb_last_link_type=0x%02x\n",
		   bt->hrb_connection_events, bt->hrb_last_link_type);
	seq_printf(s, "acl_enabled=%u acl_tx=%u acl_rx=%u acl_queued=%u le_queued=%u\n",
		   enable_acl, bt->hdev->stat.acl_tx, bt->hdev->stat.acl_rx,
		   skb_queue_len(&bt->acl_txq), skb_queue_len(&bt->le_txq));
	seq_printf(s, "auxiliary_rings=%u messages=%u message_head=%u capture_bytes=%u capture_records=%u capture_dropped=%u\n",
		   arm_auxiliary_rings, bt->message_tag, bt->message_head,
		   bt->capture_used, bt->capture_records, bt->capture_dropped);
	if (arm_auxiliary_rings)
		for (i = 0; i < ARRAY_SIZE(cen_aux_specs); i++)
			if (cen_aux_specs[i].receive)
				seq_printf(s, "aux_rx ring=%u packets=%u consumed=%u device_tail=%u\n",
					   cen_aux_specs[i].id, bt->aux[i].packets,
					   bt->aux[i].head, cen_tail(bt, cen_aux_specs[i].id));
	mutex_unlock(&bt->lock);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(cen_status);

static ssize_t cen_capture_read(struct file *file, char __user *buffer,
				size_t count, loff_t *pos)
{
	struct cen_bt *bt = file->private_data;
	ssize_t ret;

	mutex_lock(&bt->lock);
	ret = simple_read_from_buffer(buffer, count, pos, bt->capture, bt->capture_used);
	mutex_unlock(&bt->lock);
	return ret;
}

static const struct file_operations cen_capture_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.read = cen_capture_read,
	.llseek = default_llseek,
};

static ssize_t cen_capture_reset(struct file *file, const char __user *buffer,
				 size_t count, loff_t *pos)
{
	struct cen_bt *bt = file->private_data;
	unsigned int value;
	int ret = kstrtouint_from_user(buffer, count, 0, &value);

	if (ret || value != 1)
		return ret ?: -EINVAL;
	mutex_lock(&bt->lock);
	bt->capture_used = 0;
	bt->capture_records = 0;
	bt->capture_dropped = 0;
	mutex_unlock(&bt->lock);
	return count;
}

static const struct file_operations cen_capture_reset_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.write = cen_capture_reset,
};

static int cen_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	struct cen_bt *bt;
	struct hci_dev *hdev;
	int ret;

	if (!of_machine_is_compatible("apple,j813") || PCI_FUNC(pdev->devfn) != 2)
		return -ENODEV;
	bt = devm_kzalloc(&pdev->dev, sizeof(*bt), GFP_KERNEL);
	if (!bt)
		return -ENOMEM;
	bt->pdev = pdev;
	mutex_init(&bt->lock);
	skb_queue_head_init(&bt->txq);
	skb_queue_head_init(&bt->acl_txq);
	skb_queue_head_init(&bt->le_txq);
	if (enable_acl && (!arm_le_data_rings || !arm_auxiliary_rings))
		return -EINVAL;
	if (native_hrb && (!native_startup || !enable_acl))
		return -EINVAL;
	INIT_DELAYED_WORK(&bt->poll, cen_poll);
	ret = pcim_enable_device(pdev);
	if (ret)
		return ret;
	if (pci_resource_len(pdev, 0) != SZ_64K)
		return -ENODEV;
	ret = pcim_iomap_regions(pdev, BIT(0), "btcentauri");
	if (ret)
		return ret;
	bt->bar = pcim_iomap_table(pdev)[0];
	if (readl(bt->bar + 0x18) != 2 || readl(bt->bar) != 0)
		return dev_err_probe(&pdev->dev, -EBUSY,
				     "Beta requires running firmware and unowned IPC (exec=%u ipc=%u)\n",
				     readl(bt->bar + 0x18), readl(bt->bar));
	ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(40));
	if (ret)
		return ret;
	bt->arena = dma_alloc_coherent(&pdev->dev, CEN_ARENA_SIZE, &bt->dma, GFP_KERNEL);
	if (!bt->arena)
		return -ENOMEM;
	memset(bt->arena, 0, CEN_ARENA_SIZE);
	if (arm_auxiliary_rings) {
		bt->capture = kvzalloc(CEN_CAPTURE_SIZE, GFP_KERNEL);
		if (!bt->capture) {
			ret = -ENOMEM;
			goto stop;
		}
	}
	ret = cen_start(bt);
	if (ret) {
		dev_err(&pdev->dev, "Beta IPC initialization failed: %d\n", ret);
		goto stop;
	}
	hdev = hci_alloc_dev();
	if (!hdev) {
		ret = -ENOMEM;
		goto stop;
	}
	bt->hdev = hdev;
	hdev->bus = HCI_PCI;
	hdev->open = cen_hci_open;
	hdev->close = cen_hci_close;
	hdev->flush = cen_hci_flush;
	hdev->send = cen_hci_send;
	hdev->setup = cen_hci_setup;
	hdev->post_init = cen_hci_post_init;
	SET_HCIDEV_DEV(hdev, &pdev->dev);
	hci_set_drvdata(hdev, bt);
	pci_set_drvdata(pdev, bt);
	ret = hci_register_dev(hdev);
	if (ret) {
		hci_free_dev(hdev);
		goto stop;
	}
	bt->debug = debugfs_create_dir("btcentauri", NULL);
	debugfs_create_file("status", 0400, bt->debug, bt, &cen_status_fops);
	if (bt->capture) {
		debugfs_create_file("auxiliary_capture", 0400, bt->debug, bt, &cen_capture_fops);
		debugfs_create_file("capture_reset", 0200, bt->debug, bt, &cen_capture_reset_fops);
	}
	dev_info(&pdev->dev, "Beta HCI transport registered; ACL enabled=%u, SCO/ISO unsupported\n",
		 enable_acl);
	return 0;
stop:
	cen_stop(bt);
	return ret;
}

static void cen_remove(struct pci_dev *pdev)
{
	struct cen_bt *bt = pci_get_drvdata(pdev);

	debugfs_remove_recursive(bt->debug);
	hci_unregister_dev(bt->hdev);
	cen_hci_close(bt->hdev);
	hci_free_dev(bt->hdev);
	cen_stop(bt);
}

static const struct pci_device_id cen_ids[] = {
	{ PCI_DEVICE(PCI_VENDOR_ID_APPLE, 0x1903) },
	{ }
};
MODULE_DEVICE_TABLE(pci, cen_ids);

static struct pci_driver cen_driver = {
	.name = "btcentauri",
	.id_table = cen_ids,
	.probe = cen_probe,
	.remove = cen_remove,
};
module_pci_driver(cen_driver);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Apple N1 Beta HCI command/event transport for J813 bring-up");
