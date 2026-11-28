// SPDX-License-Identifier: GPL-2.0-only
/* Apple N1 Beta HCI transport driver.
 *
 * The Centauri Control driver boots the shared firmware first. This driver
 * owns only the Beta (Bluetooth) PCI function and its ACIPC rings.
 */
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/etherdevice.h>
#include <linux/interrupt.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/pci.h>
#include <linux/property.h>
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
MODULE_PARM_DESC(arm_le_data_rings, "Arm Beta LE data rings");

static bool arm_auxiliary_rings = true;
module_param(arm_auxiliary_rings, bool, 0444);
MODULE_PARM_DESC(arm_auxiliary_rings, "Arm Beta's ACL data rings");

static bool diagnostic_capture;
module_param(diagnostic_capture, bool, 0444);
MODULE_PARM_DESC(diagnostic_capture, "Diagnostic: arm every auxiliary ring and capture received frames privately; requires the auxiliary rings");

static bool native_startup;
module_param(native_startup, bool, 0444);
MODULE_PARM_DESC(native_startup, "Diagnostic: program a temporary locally administered address when no factory BD_ADDR is present");

static bool factory_address = true;
module_param(factory_address, bool, 0444);
MODULE_PARM_DESC(factory_address, "Program the loader-provided local-bd-address before the first HCI reset");

static bool fixup_adv_phy = true;
module_param(fixup_adv_phy, bool, 0444);
MODULE_PARM_DESC(fixup_adv_phy, "Mask the firmware's extra bits in LE extended advertising report PHYs");

static bool scan_dup_filter = true;
module_param(scan_dup_filter, bool, 0444);
MODULE_PARM_DESC(scan_dup_filter, "Allow controller LE duplicate filtering (uses vendor duplicate-filter reset on scan enable)");

static bool accept_list;
module_param(accept_list, bool, 0444);
MODULE_PARM_DESC(accept_list, "Let the controller filter LE scans by its accept list (off: report its size as 0, the host filters)");

static bool enable_acl = true;
module_param(enable_acl, bool, 0444);
MODULE_PARM_DESC(enable_acl, "Enable BR/EDR and LE ACL, SCO, and LE Audio ISO data delivery; requires LE and auxiliary rings");

static bool native_hrb = true;
module_param(native_hrb, bool, 0444);
MODULE_PARM_DESC(native_hrb, "Translate BR/EDR Create Connection to the firmware's extended connection command when advertised");

#define CEN_OP_READ_PROP_FEATURES 0xffa3
#define CEN_OP_HRB_CREATE_CONN 0xff7b
#define CEN_OP_HRB_SETUP_SYNC_CONN 0xfdc7
#define CEN_OP_HRB_ACCEPT_SYNC_CONN 0xfdf8
#define CEN_OP_WRITE_VOICE_CONFIG 0xfe10
#define CEN_OP_FLUSH_DUPLICATES 0xff3f
#define CEN_PROP_HRB_CREATE BIT(9)
#define CEN_VOICE_TRANSPARENT 0x0003
#define CEN_VOICE_TRANSPARENT_16BIT 0x0063
/* Bits 13:10 select extended high-rate eSCO packet types; keep clear on standard calls. */
#define CEN_HRB_SYNC_PKT_HDR_MASK 0x3c00
#define CEN_MAX_HRB_CONNS 16

struct cen_hrb_conn {
	bool valid;
	u16 handle;
	u8 bdaddr[6];
};

struct cen_aux_spec {
	u16 id, slots, footer;
	bool receive;
};

/* Beta's auxiliary rings. Only the ACL rings (2 and 8) are armed by default; the
 * others only with diagnostic_capture. HCI and LE rings have their own arenas.
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
	bool opened;
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
	struct sk_buff_head acl_txq, le_txq, sco_txq, iso_txq;
	u16 acl_tx_head, le_tx_head, sco_tx_head, iso_tx_head;
	u16 message_head, message_tag, tx_head, rx_head, rx_post;
	u16 le_rx_head, le_rx_post;
	u32 le_rx_packets;
	struct cen_aux_state aux[ARRAY_SIZE(cen_aux_specs)];
	u8 *capture;
	u32 capture_used, capture_records, capture_dropped;
	bool ipc_requested, running;
	u8 native_startup_steps;
	bool hrb_supported, hrb_status_pending;
	u16 hrb_sync_pending_wire, hrb_sync_pending_orig;
	bool voice_hci_routed;
	struct cen_hrb_conn hrb_conns[CEN_MAX_HRB_CONNS];
	u32 proprietary_features, hrb_commands, hrb_status_events;
	u32 hrb_sync_commands, hrb_sync_status_events;
	u32 hrb_connection_events;
	u8 hrb_last_link_type;
	u32 scan_dup_rewritten, accept_list_hidden;
	bool dup_filter_fallback;
	bool transport_paused, transport_sleep_sent;
	/* MSI backed by a DMA buffer, outside the PCI core's IRQ allocation. */
	struct {
		u32 lo, hi;
		u16 flags, data;
		u8 cap;
		bool saved;
	} sleep_msi;
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

static void cen_skip_rx_attrs(struct cen_bt *bt, const char *kind,
			      const u8 *data, u32 offset, u32 length)
{
	u32 pos;

	/* Trailing RX metadata attributes follow the HCI data payload. */
	for (pos = offset; pos < length;) {
		if (!data[pos])
			break;
		if (data[pos] == 1)
			pos++;
		else if (data[pos] == 2 && length - pos >= 9)
			pos += 9;
		else {
			dev_warn_once(&bt->pdev->dev,
				      "Ignoring unknown %s RX trailing attribute %#x (remaining=%u)\n",
				      kind, data[pos], length - pos);
			break;
		}
	}
}

static void cen_receive_acl(struct cen_bt *bt, const u8 *data, u32 length)
{
	struct sk_buff *skb;
	u32 packet_length;

	if (length < HCI_ACL_HDR_SIZE) {
		bt->hdev->stat.err_rx++;
		dev_warn_ratelimited(&bt->pdev->dev,
				     "Dropping truncated ACL frame (len=%u)\n",
				     length);
		return;
	}
	packet_length = HCI_ACL_HDR_SIZE + get_unaligned_le16(data + 2);
	if (packet_length > length) {
		bt->hdev->stat.err_rx++;
		dev_warn_ratelimited(&bt->pdev->dev,
				     "Dropping malformed ACL frame (hdr_len=%u ring_len=%u)\n",
				     packet_length, length);
		return;
	}
	cen_skip_rx_attrs(bt, "ACL", data, packet_length, length);
	skb = bt_skb_alloc(packet_length, GFP_KERNEL);
	if (!skb) {
		bt->hdev->stat.err_rx++;
		return;
	}
	skb_put_data(skb, data, packet_length);
	hci_skb_pkt_type(skb) = HCI_ACLDATA_PKT;
	bt->hdev->stat.byte_rx += packet_length;
	hci_recv_frame(bt->hdev, skb);
}

static void cen_receive_sco(struct cen_bt *bt, const u8 *data, u32 length)
{
	struct sk_buff *skb;
	u32 packet_length;

	if (length < HCI_SCO_HDR_SIZE) {
		bt->hdev->stat.err_rx++;
		dev_warn_ratelimited(&bt->pdev->dev,
				     "Dropping truncated SCO frame (len=%u)\n",
				     length);
		return;
	}
	packet_length = HCI_SCO_HDR_SIZE + data[2];
	if (packet_length > length) {
		bt->hdev->stat.err_rx++;
		dev_warn_ratelimited(&bt->pdev->dev,
				     "Dropping malformed SCO frame (hdr_len=%u ring_len=%u)\n",
				     packet_length, length);
		return;
	}
	cen_skip_rx_attrs(bt, "SCO", data, packet_length, length);
	skb = bt_skb_alloc(packet_length, GFP_KERNEL);
	if (!skb) {
		bt->hdev->stat.err_rx++;
		return;
	}
	skb_put_data(skb, data, packet_length);
	hci_skb_pkt_type(skb) = HCI_SCODATA_PKT;
	bt->hdev->stat.byte_rx += packet_length;
	hci_recv_frame(bt->hdev, skb);
}

static void cen_receive_iso(struct cen_bt *bt, const u8 *data, u32 length)
{
	struct sk_buff *skb;
	u32 packet_length;

	/* Ring 11 prefixes each payload with an H4 packet indicator byte. */
	if (length < 1) {
		bt->hdev->stat.err_rx++;
		dev_warn_ratelimited(&bt->pdev->dev,
				     "Dropping empty ISO ring frame\n");
		return;
	}
	if (data[0] == HCI_ACLDATA_PKT) {
		cen_receive_acl(bt, data + 1, length - 1);
		return;
	}
	if (data[0] != HCI_ISODATA_PKT) {
		bt->hdev->stat.err_rx++;
		dev_warn_ratelimited(&bt->pdev->dev,
				     "Dropping unexpected ISO ring packet type %#x (len=%u)\n",
				     data[0], length);
		return;
	}
	data++;
	length--;
	if (length < HCI_ISO_HDR_SIZE) {
		bt->hdev->stat.err_rx++;
		dev_warn_ratelimited(&bt->pdev->dev,
				     "Dropping truncated ISO frame (len=%u)\n",
				     length);
		return;
	}
	packet_length = HCI_ISO_HDR_SIZE + (get_unaligned_le16(data + 2) & 0x3fff);
	if (packet_length > length) {
		bt->hdev->stat.err_rx++;
		dev_warn_ratelimited(&bt->pdev->dev,
				     "Dropping malformed ISO frame (hdr_len=%u ring_len=%u)\n",
				     packet_length, length);
		return;
	}
	cen_skip_rx_attrs(bt, "ISO", data, packet_length, length);
	skb = bt_skb_alloc(packet_length, GFP_KERNEL);
	if (!skb) {
		bt->hdev->stat.err_rx++;
		return;
	}
	skb_put_data(skb, data, packet_length);
	hci_skb_pkt_type(skb) = HCI_ISODATA_PKT;
	bt->hdev->stat.byte_rx += packet_length;
	hci_recv_frame(bt->hdev, skb);
}

static void cen_aux_rx_buffer(struct cen_bt *bt, unsigned int index, u16 slot)
{
	const struct cen_aux_spec *spec = &cen_aux_specs[index];
	u8 *descriptor = bt->arena + bt->aux[index].offset + slot * (16 + spec->footer);

	memset(descriptor, 0, 16 + spec->footer);
	put_unaligned_le32(1 | (spec->footer << 8), descriptor);
	put_unaligned_le16(slot + 1, descriptor + 12);
}

static bool cen_aux_required(u16 id)
{
	return id == 2 || id == 3 || id == 5 || id == 8 || id == 9 || id == 11;
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
		if (!diagnostic_capture && !cen_aux_required(spec->id))
			continue;
		ret = cen_open_ring(bt, spec->id, spec->slots, state->offset, spec->footer);
		if (ret) {
			dev_err(&bt->pdev->dev, "Auxiliary ring %u open failed: %d\n", spec->id, ret);
			return ret;
		}
		state->opened = true;
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

		if (!state->opened || !spec->receive)
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
			if (repost && enable_acl) {
				if (spec->id == 8)
					cen_receive_acl(bt, descriptor + 16, length);
				else if (spec->id == 9)
					cen_receive_sco(bt, descriptor + 16, length);
				else if (spec->id == 11)
					cen_receive_iso(bt, descriptor + 16, length);
			}
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
	/* MCR footer length is encoded in 4-byte words. */
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

static void cen_hrb_clear(struct cen_bt *bt)
{
	memset(bt->hrb_conns, 0, sizeof(bt->hrb_conns));
	bt->hrb_status_pending = false;
	bt->hrb_sync_pending_wire = 0;
	bt->hrb_sync_pending_orig = 0;
}

static void cen_hrb_track_add(struct cen_bt *bt, u16 handle, const u8 bdaddr[6])
{
	unsigned int i, free_slot = CEN_MAX_HRB_CONNS;

	handle &= 0x0fff;
	for (i = 0; i < CEN_MAX_HRB_CONNS; i++) {
		if (bt->hrb_conns[i].valid && bt->hrb_conns[i].handle == handle) {
			memcpy(bt->hrb_conns[i].bdaddr, bdaddr, 6);
			return;
		}
		if (!bt->hrb_conns[i].valid && free_slot == CEN_MAX_HRB_CONNS)
			free_slot = i;
	}
	if (free_slot < CEN_MAX_HRB_CONNS) {
		bt->hrb_conns[free_slot].valid = true;
		bt->hrb_conns[free_slot].handle = handle;
		memcpy(bt->hrb_conns[free_slot].bdaddr, bdaddr, 6);
	}
}

static void cen_hrb_track_del(struct cen_bt *bt, u16 handle)
{
	unsigned int i;

	handle &= 0x0fff;
	for (i = 0; i < CEN_MAX_HRB_CONNS; i++) {
		if (bt->hrb_conns[i].valid && bt->hrb_conns[i].handle == handle)
			bt->hrb_conns[i].valid = false;
	}
}

static bool cen_hrb_has_handle(const struct cen_bt *bt, u16 handle)
{
	unsigned int i;

	handle &= 0x0fff;
	for (i = 0; i < CEN_MAX_HRB_CONNS; i++) {
		if (bt->hrb_conns[i].valid && bt->hrb_conns[i].handle == handle)
			return true;
	}
	return false;
}

static bool cen_hrb_has_bdaddr(const struct cen_bt *bt, const u8 bdaddr[6])
{
	unsigned int i;

	for (i = 0; i < CEN_MAX_HRB_CONNS; i++) {
		if (bt->hrb_conns[i].valid &&
		    !memcmp(bt->hrb_conns[i].bdaddr, bdaddr, 6))
			return true;
	}
	return false;
}

static u16 cen_hrb_sync_pkt_mask(u16 pkt_type)
{
	return pkt_type & ~CEN_HRB_SYNC_PKT_HDR_MASK;
}

static u8 cen_hrb_normalize_link_type(u8 ev_code, u8 link_type)
{
	if (ev_code == HCI_EV_CONN_COMPLETE && (link_type & 0xfe) == 0xf0)
		return ACL_LINK;
	if (ev_code == HCI_EV_SYNC_CONN_COMPLETE && link_type >= 0xf0)
		return ESCO_LINK;
	if (ev_code == HCI_EV_CONN_REQUEST) {
		if ((link_type & 0xfe) == 0xf0)
			return ACL_LINK;
		if ((link_type & 0xfe) == 0xf2)
			return ESCO_LINK;
	}
	return link_type;
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
		if (ring == 5) {
			descriptor[16] = HCI_ISODATA_PKT;
			memcpy(descriptor + 17, skb->data, skb->len);
			wire_length = skb->len + 1;
		} else {
			memcpy(descriptor + 16, skb->data, skb->len);
			wire_length = skb->len;
		}
		if (ring == 1 && get_unaligned_le16(skb->data) == HCI_OP_RESET)
			cen_hrb_clear(bt);
		if (ring == 1 && READ_ONCE(bt->hrb_supported) &&
		    get_unaligned_le16(skb->data) == HCI_OP_CREATE_CONN) {
			u8 *command = descriptor + 16;

			/* Extended Create Connection extends the 13-byte HCI
			 * parameter payload with two zero bytes while leaving
			 * the core's skb opcode unchanged.
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
		if (ring == 1 &&
		    get_unaligned_le16(skb->data) == HCI_OP_SETUP_SYNC_CONN &&
		    skb->len == HCI_COMMAND_HDR_SIZE + sizeof(struct hci_cp_setup_sync_conn)) {
			u8 *command = descriptor + 16;
			u16 handle = get_unaligned_le16(command + HCI_COMMAND_HDR_SIZE);

			/* Firmware requires 16-bit 2's complement input format
			 * alongside transparent air coding (0x0063 vs 0x0003).
			 */
			if (get_unaligned_le16(command + HCI_COMMAND_HDR_SIZE + 12) ==
			    CEN_VOICE_TRANSPARENT)
				put_unaligned_le16(CEN_VOICE_TRANSPARENT_16BIT,
						   command + HCI_COMMAND_HDR_SIZE + 12);
			/* Extended links (0xf0/0xf1) require the 18-byte
			 * extended synchronous setup command with 24-bit
			 * packet type mask and high-rate packet bits masked off.
			 */
			if (READ_ONCE(bt->hrb_supported) &&
			    cen_hrb_has_handle(bt, handle)) {
				u16 pkt = get_unaligned_le16(command + HCI_COMMAND_HDR_SIZE + 15);

				put_unaligned_le16(CEN_OP_HRB_SETUP_SYNC_CONN, command);
				command[2] = 18;
				put_unaligned_le16(cen_hrb_sync_pkt_mask(pkt),
						   command + HCI_COMMAND_HDR_SIZE + 15);
				command[HCI_COMMAND_HDR_SIZE + 17] = 0;
				wire_length = HCI_COMMAND_HDR_SIZE + 18;
				bt->hrb_sync_pending_wire = CEN_OP_HRB_SETUP_SYNC_CONN;
				bt->hrb_sync_pending_orig = HCI_OP_SETUP_SYNC_CONN;
				bt->hrb_sync_commands++;
				cen_capture_frame(bt, 1, command, wire_length);
			}
		}
		if (ring == 1 &&
		    get_unaligned_le16(skb->data) == HCI_OP_ACCEPT_SYNC_CONN_REQ &&
		    skb->len == HCI_COMMAND_HDR_SIZE + sizeof(struct hci_cp_accept_sync_conn_req)) {
			u8 *command = descriptor + 16;

			if (get_unaligned_le16(command + HCI_COMMAND_HDR_SIZE + 16) ==
			    CEN_VOICE_TRANSPARENT)
				put_unaligned_le16(CEN_VOICE_TRANSPARENT_16BIT,
						   command + HCI_COMMAND_HDR_SIZE + 16);
			if (READ_ONCE(bt->hrb_supported) &&
			    cen_hrb_has_bdaddr(bt, command + HCI_COMMAND_HDR_SIZE)) {
				u16 pkt = get_unaligned_le16(command + HCI_COMMAND_HDR_SIZE + 19);

				put_unaligned_le16(CEN_OP_HRB_ACCEPT_SYNC_CONN, command);
				command[2] = 22;
				put_unaligned_le16(cen_hrb_sync_pkt_mask(pkt),
						   command + HCI_COMMAND_HDR_SIZE + 19);
				command[HCI_COMMAND_HDR_SIZE + 21] = 0;
				wire_length = HCI_COMMAND_HDR_SIZE + 22;
				bt->hrb_sync_pending_wire = CEN_OP_HRB_ACCEPT_SYNC_CONN;
				bt->hrb_sync_pending_orig = HCI_OP_ACCEPT_SYNC_CONN_REQ;
				bt->hrb_sync_commands++;
				cen_capture_frame(bt, 1, command, wire_length);
			}
		}
		put_unaligned_le32(1 | (wire_length << 8), descriptor);
		*head = next;
		cen_post(bt, ring, next);
		if (hci_skb_pkt_type(skb) == HCI_COMMAND_PKT)
			bt->hdev->stat.cmd_tx++;
		else if (hci_skb_pkt_type(skb) == HCI_ACLDATA_PKT)
			bt->hdev->stat.acl_tx++;
		else if (hci_skb_pkt_type(skb) == HCI_SCODATA_PKT)
			bt->hdev->stat.sco_tx++;
		bt->hdev->stat.byte_tx += wire_length;
		kfree_skb(skb);
	}
	return 0;
}

/* The N1 reports legacy scan responses in LE Extended Advertising Reports with an event type whose low
 * byte is 0x18 (legacy PDU and scan response, neither connectable nor scannable) and other bits in the
 * high byte (seen: 0x2518, 0x2618, 0x2718). Linux accepts only the Core values 0x1a and 0x1b for a
 * legacy scan response and drops anything else as "Unknown advertising packet type", so report the
 * low byte 0x18 as 0x1a and keep the high byte.
 */
static void cen_normalize_ext_adv(struct sk_buff *skb)
{
	unsigned int offset = HCI_EVENT_HDR_SIZE + 2, reports;

	if (skb->len < offset + 1 || skb->data[0] != HCI_EV_LE_META ||
	    skb->data[2] != HCI_EV_LE_EXT_ADV_REPORT)
		return;
	reports = skb->data[3];
	while (reports--) {
		struct hci_ev_le_ext_adv_info *info;

		if (offset + sizeof(*info) > skb->len)
			return;
		info = (struct hci_ev_le_ext_adv_info *)(skb->data + offset);
		if ((le16_to_cpu(info->type) & 0x00ff) == 0x0018)
			info->type = cpu_to_le16((le16_to_cpu(info->type) & 0xff00) | 0x001a);
		offset += sizeof(*info) + info->length;
	}
}

/*
 * When a paired LE device is put on the accept list and hci_sync scans
 * passively with filter policy 1, the N1 sends no advertising report and no
 * other event at all, so the connection is never made. Unless accept_list is
 * set, report an accept list of size 0: hci_update_accept_list_sync() then
 * finds no room and scans with filter policy 0, and the host matches
 * pend_le_conns itself, as for a controller whose list is full.
 */
static void cen_fixup_event(struct cen_bt *bt, struct sk_buff *skb)
{
	if (skb->len >= 7 && skb->data[0] == HCI_EV_CMD_COMPLETE &&
	    get_unaligned_le16(skb->data + 3) == HCI_OP_LE_READ_ACCEPT_LIST_SIZE &&
	    !skb->data[5] && !accept_list) {
		bt_dev_info(bt->hdev, "LE accept list (%u entries) not used; the host filters scans",
			    skb->data[6]);
		skb->data[6] = 0;
		bt->accept_list_hidden++;
	}
	if (skb->len >= HCI_EVENT_HDR_SIZE + 3 + sizeof(struct hci_rp_read_buffer_size) &&
	    skb->data[0] == HCI_EV_CMD_COMPLETE &&
	    get_unaligned_le16(skb->data + 3) == HCI_OP_READ_BUFFER_SIZE &&
	    !skb->data[5]) {
		struct hci_rp_read_buffer_size *rp = (void *)(skb->data + 5);

		if (!rp->sco_mtu || !le16_to_cpu(rp->sco_max_pkt)) {
			bt_dev_info(bt->hdev,
				    "Controller reported SCO buffers %u:%u; using ring defaults 60:8",
				    rp->sco_mtu, le16_to_cpu(rp->sco_max_pkt));
			if (!rp->sco_mtu)
				rp->sco_mtu = 60;
			if (!le16_to_cpu(rp->sco_max_pkt))
				rp->sco_max_pkt = cpu_to_le16(8);
		}
	}
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
	if (!bt->running || READ_ONCE(bt->transport_paused))
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
			if (enable_acl)
				cen_receive_acl(bt, descriptor + 16, length);
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
			 * request tracked by Linux. Restore only its opcode while
			 * preserving status and command credit.
			 */
			if (bt->hrb_status_pending && length == 6 &&
			    skb->data[0] == HCI_EV_CMD_STATUS &&
			    get_unaligned_le16(skb->data + 4) == CEN_OP_HRB_CREATE_CONN) {
				cen_capture_frame(bt, 7, skb->data, length);
				put_unaligned_le16(HCI_OP_CREATE_CONN, skb->data + 4);
				bt->hrb_status_pending = false;
				bt->hrb_status_events++;
			}
			if (bt->hrb_sync_pending_wire && length == 6 &&
			    skb->data[0] == HCI_EV_CMD_STATUS &&
			    get_unaligned_le16(skb->data + 4) == bt->hrb_sync_pending_wire) {
				cen_capture_frame(bt, 7, skb->data, length);
				put_unaligned_le16(bt->hrb_sync_pending_orig, skb->data + 4);
				bt->hrb_sync_pending_wire = 0;
				bt->hrb_sync_pending_orig = 0;
				bt->hrb_sync_status_events++;
			}
			/* Normalize vendor 0xf0/0xf1 Connection Complete link types
			 * to standard ACL_LINK so HCI matches the pending connection,
			 * and record the handle so subsequent SCO setup uses the
			 * extended synchronous command.
			 */
			if (READ_ONCE(bt->hrb_supported) &&
			    length == HCI_EVENT_HDR_SIZE + sizeof(struct hci_ev_conn_complete) &&
			    skb->data[0] == HCI_EV_CONN_COMPLETE &&
			    (skb->data[11] & 0xfe) == 0xf0) {
				cen_capture_frame(bt, 7, skb->data, length);
				bt->hrb_last_link_type = skb->data[11];
				if (!skb->data[2])
					cen_hrb_track_add(bt,
							  get_unaligned_le16(skb->data + 3),
							  skb->data + 5);
				skb->data[11] = cen_hrb_normalize_link_type(HCI_EV_CONN_COMPLETE,
									    skb->data[11]);
				bt->hrb_connection_events++;
			}
			if (READ_ONCE(bt->hrb_supported) &&
			    length == HCI_EVENT_HDR_SIZE +
				      sizeof(struct hci_ev_sync_conn_complete) &&
			    skb->data[0] == HCI_EV_SYNC_CONN_COMPLETE &&
			    skb->data[11] >= 0xf0) {
				cen_capture_frame(bt, 7, skb->data, length);
				skb->data[11] = cen_hrb_normalize_link_type(
					HCI_EV_SYNC_CONN_COMPLETE, skb->data[11]);
			}
			if (READ_ONCE(bt->hrb_supported) &&
			    length == HCI_EVENT_HDR_SIZE + sizeof(struct hci_ev_conn_request) &&
			    skb->data[0] == HCI_EV_CONN_REQUEST &&
			    skb->data[11] >= 0xf0)
				skb->data[11] = cen_hrb_normalize_link_type(HCI_EV_CONN_REQUEST,
									    skb->data[11]);
			if (length == HCI_EVENT_HDR_SIZE + sizeof(struct hci_ev_disconn_complete) &&
			    skb->data[0] == HCI_EV_DISCONN_COMPLETE &&
			    !skb->data[2])
				cen_hrb_track_del(bt, get_unaligned_le16(skb->data + 3));
			cen_normalize_ext_adv(skb);
			cen_fixup_event(bt, skb);
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
	/* cen_aux_specs[0/1/2] are BR/EDR ACL (2), SCO (3), and ISO (5) TX rings. */
	if (enable_acl &&
	    ((bt->aux[0].opened &&
	      cen_transmit(bt, &bt->acl_txq, 2, cen_aux_specs[0].slots,
			   bt->aux[0].offset, cen_aux_specs[0].footer,
			   &bt->acl_tx_head)) ||
	     cen_transmit(bt, &bt->le_txq, 4, CEN_LE_SLOTS, CEN_LE_TX,
			  CEN_LE_FOOTER, &bt->le_tx_head) ||
	     (bt->aux[1].opened &&
	      cen_transmit(bt, &bt->sco_txq, 3, cen_aux_specs[1].slots,
			   bt->aux[1].offset, cen_aux_specs[1].footer,
			   &bt->sco_tx_head)) ||
	     (bt->aux[2].opened &&
	      cen_transmit(bt, &bt->iso_txq, 5, cen_aux_specs[2].slots,
			   bt->aux[2].offset, cen_aux_specs[2].footer,
			   &bt->iso_tx_head))))
		goto protocol_error;
	/*
	 * Beta's MSI is a DMA-backed mailbox the firmware programs itself, not a
	 * vector the PCI core hands out (see cen_resume_noirq()), so the rings
	 * are polled.
	 */
	mod_delayed_work(system_percpu_wq, &bt->poll, msecs_to_jiffies(1));
	goto unlock;
protocol_error:
	bt->running = false;
	hdev->stat.err_rx++;
	dev_err(&bt->pdev->dev, "Beta transport stopped (exec=%u ipc=%u)\n",
		readl(bt->bar + 0x18), readl(bt->bar));
unlock:
	mutex_unlock(&bt->lock);
}

static int cen_hci_open(struct hci_dev *hdev)
{
	struct cen_bt *bt = hci_get_drvdata(hdev);

	mutex_lock(&bt->lock);
	bt->running = true;
	if (!READ_ONCE(bt->transport_paused))
		mod_delayed_work(system_percpu_wq, &bt->poll, 0);
	mutex_unlock(&bt->lock);
	return 0;
}

static int cen_hci_close(struct hci_dev *hdev)
{
	struct cen_bt *bt = hci_get_drvdata(hdev);

	mutex_lock(&bt->lock);
	bt->running = false;
	cen_hrb_clear(bt);
	mutex_unlock(&bt->lock);
	cancel_delayed_work_sync(&bt->poll);
	skb_queue_purge(&bt->txq);
	skb_queue_purge(&bt->acl_txq);
	skb_queue_purge(&bt->le_txq);
	skb_queue_purge(&bt->sco_txq);
	skb_queue_purge(&bt->iso_txq);
	return 0;
}

static void cen_hci_reset(struct hci_dev *hdev)
{
	struct cen_bt *bt = hci_get_drvdata(hdev);
	bool paused;
	u32 exec, ipc;

	mutex_lock(&bt->lock);
	bt->hrb_status_pending = false;
	bt->hrb_sync_pending_wire = 0;
	bt->hrb_sync_pending_orig = 0;
	paused = READ_ONCE(bt->transport_paused);
	if (!paused && bt->bar && bt->arena) {
		exec = readl(bt->bar + 0x18);
		ipc = readl(bt->bar);
		bt_dev_err(hdev,
			   "HCI command timeout (exec=%u ipc=%u running=%u tx=%u/%u rx=%u/%u)",
			   exec, ipc, bt->running, bt->tx_head, cen_tail(bt, 1),
			   bt->rx_head, cen_tail(bt, 7));
	} else {
		bt_dev_err(hdev,
			   "HCI command timeout while transport unavailable (running=%u paused=%u)",
			   bt->running, paused);
	}
	if (bt->running && !paused)
		mod_delayed_work(system_percpu_wq, &bt->poll, 0);
	mutex_unlock(&bt->lock);
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

static int cen_hci_reset_dup_filter(struct hci_dev *hdev)
{
	struct cen_bt *bt = hci_get_drvdata(hdev);
	static const u8 flags;
	int ret;

	if (READ_ONCE(bt->dup_filter_fallback))
		return 0;
	ret = __hci_cmd_sync_status(hdev, CEN_OP_FLUSH_DUPLICATES,
				    sizeof(flags), &flags, HCI_CMD_TIMEOUT);
	if (ret) {
		bt_dev_warn(hdev,
			    "Duplicate filter reset failed (%d); falling back to host filtering",
			    ret);
		WRITE_ONCE(bt->dup_filter_fallback, true);
	}
	return 0;
}

static int cen_hci_post_init(struct hci_dev *hdev)
{
	struct cen_bt *bt = hci_get_drvdata(hdev);
	static const u8 voice_hci[6] = { 0x01, 0x00, 0xff, 0xff, 0xff, 0xff };
	struct sk_buff *skb;
	u32 features;
	int ret;

	/* Route synchronous voice audio over the HCI SCO rings (3/9). */
	ret = cen_vendor_command(hdev, CEN_OP_WRITE_VOICE_CONFIG,
				 voice_hci, sizeof(voice_hci));
	if (ret) {
		bt_dev_dbg(hdev, "Voice HCI routing command returned %d", ret);
		WRITE_ONCE(bt->voice_hci_routed, false);
	} else {
		WRITE_ONCE(bt->voice_hci_routed, true);
	}

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
	if (!ret && !(features & CEN_PROP_HRB_CREATE))
		ret = -EOPNOTSUPP;
	if (ret) {
		bt_dev_warn(hdev, "Firmware connection command unavailable: %d",
			    ret);
		return 0;
	}
	WRITE_ONCE(bt->hrb_supported, true);
	bt_dev_dbg(hdev, "Extended Create Connection capability confirmed");
	return 0;
}

/*
 * The factory address in network byte order, for WriteBdAddr, from the
 * standard local-bd-address (least significant byte first; the loader fills
 * it from the ADT for the bluetooth0 alias). As in btcentauri_t6050.c.
 */
static int cen_bt_address(struct device *dev, u8 address[6])
{
	u8 bd[6];
	int i, ret;

	if (device_property_count_u8(dev, "local-bd-address") != 6)
		return -ENOENT;
	ret = device_property_read_u8_array(dev, "local-bd-address", bd, sizeof(bd));
	if (ret)
		return ret;
	for (i = 0; i < 6; i++)
		address[i] = bd[5 - i];
	if (is_zero_ether_addr(address) || is_broadcast_ether_addr(address))
		return -EINVAL;
	return 0;
}

static int cen_hci_set_bdaddr(struct hci_dev *hdev, const bdaddr_t *bdaddr)
{
	u8 address[6];
	int i;

	for (i = 0; i < 6; i++)
		address[i] = bdaddr->b[5 - i];
	if (is_zero_ether_addr(address) || is_broadcast_ether_addr(address))
		return -EINVAL;
	return cen_vendor_command(hdev, 0xfc06, address, sizeof(address));
}

static int cen_hci_setup(struct hci_dev *hdev)
{
	struct cen_bt *bt = hci_get_drvdata(hdev);
	u8 address[6], init_done[16], seed[24];
	bool factory;
	int ret;

	/*
	 * Without WriteBdAddr the controller reports BD_ADDR 00:00:00:00:00:00
	 * and bluetoothd ignores it ("No Bluetooth address for index 0").
	 */
	factory = factory_address && !cen_bt_address(&bt->pdev->dev, address);
	if (factory_address && !factory)
		bt_dev_info(hdev, "No factory local-bd-address from the loader");
	if (!factory && !native_startup)
		return 0;

	/* Controller initialization runs WriteBdAddr, InitDone, and RNG seed
	 * before the core's standard HCI Reset sequence.
	 */
	if (!factory) {
		ret = get_random_bytes_wait(address, sizeof(address));
		if (ret)
			return ret;
		address[0] = (address[0] & 0xfc) | 0x02;
	}
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
		bt_dev_info(hdev, "Native startup accepted; %s address in use",
			    factory ? "factory" : "temporary diagnostic");
	return ret;
}

static int cen_hci_flush(struct hci_dev *hdev)
{
	struct cen_bt *bt = hci_get_drvdata(hdev);

	skb_queue_purge(&bt->txq);
	skb_queue_purge(&bt->acl_txq);
	skb_queue_purge(&bt->le_txq);
	skb_queue_purge(&bt->sco_txq);
	skb_queue_purge(&bt->iso_txq);
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
		} else if (link_type == ACL_LINK && bt->aux[0].opened) {
			queue = &bt->acl_txq;
			if (skb->len > cen_aux_specs[0].footer)
				ret = -EMSGSIZE;
		} else {
			ret = -EOPNOTSUPP;
		}
		if (ret)
			goto out;
	} else if (hci_skb_pkt_type(skb) == HCI_SCODATA_PKT && enable_acl &&
		   bt->aux[1].opened) {
		if (skb->len < HCI_SCO_HDR_SIZE ||
		    skb->len != HCI_SCO_HDR_SIZE + skb->data[2] ||
		    skb->len > cen_aux_specs[1].footer) {
			ret = -EMSGSIZE;
			goto out;
		}
		queue = &bt->sco_txq;
	} else if (hci_skb_pkt_type(skb) == HCI_ISODATA_PKT && enable_acl &&
		   bt->aux[2].opened) {
		if (skb->len < HCI_ISO_HDR_SIZE ||
		    skb->len != HCI_ISO_HDR_SIZE +
				(get_unaligned_le16(skb->data + 2) & 0x3fff) ||
		    skb->len + 1 > cen_aux_specs[2].footer) {
			ret = -EMSGSIZE;
			goto out;
		}
		queue = &bt->iso_txq;
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
		if (opcode == CEN_OP_HRB_CREATE_CONN ||
		    opcode == CEN_OP_HRB_SETUP_SYNC_CONN ||
		    opcode == CEN_OP_HRB_ACCEPT_SYNC_CONN) {
			ret = -EOPNOTSUPP;
			goto out;
		}
		if (opcode == HCI_OP_CREATE_CONN &&
		    skb->len != HCI_COMMAND_HDR_SIZE + sizeof(struct hci_cp_create_conn)) {
			ret = -EMSGSIZE;
			goto out;
		}
		if (opcode == HCI_OP_SETUP_SYNC_CONN &&
		    skb->len != HCI_COMMAND_HDR_SIZE + sizeof(struct hci_cp_setup_sync_conn)) {
			ret = -EMSGSIZE;
			goto out;
		}
		if (opcode == HCI_OP_ACCEPT_SYNC_CONN_REQ &&
		    skb->len != HCI_COMMAND_HDR_SIZE + sizeof(struct hci_cp_accept_sync_conn_req)) {
			ret = -EMSGSIZE;
			goto out;
		}
	}
	mutex_lock(&bt->lock);
	/*
	 * The N1 does not reset its duplicate filter when scanning is enabled
	 * again. A discovery shortly after another one reports only addresses
	 * not seen before, and so does the passive scan hci_sync starts for a
	 * pending LE connection: a device found in discovery never matches
	 * pend_le_conns and no LE Create Connection is sent. Unless
	 * scan_dup_filter is set, enable scans with Filter_Duplicates = 0; the
	 * host copes with repeated reports.
	 */
	if (hci_skb_pkt_type(skb) == HCI_COMMAND_PKT && skb->len >= 5 &&
	    (!scan_dup_filter || READ_ONCE(bt->dup_filter_fallback)) &&
	    (get_unaligned_le16(skb->data) == HCI_OP_LE_SET_EXT_SCAN_ENABLE ||
	     get_unaligned_le16(skb->data) == HCI_OP_LE_SET_SCAN_ENABLE) &&
	    skb->data[3] && skb->data[4] && !skb_ensure_writable(skb, 5)) {
		skb->data[4] = 0;
		bt->scan_dup_rewritten++;
	}
	if (!bt->running)
		ret = -ENETDOWN;
	else if (skb_queue_len(queue) >= 64)
		ret = -ENOBUFS;
	else {
		skb_queue_tail(queue, skb);
		if (!READ_ONCE(bt->transport_paused))
			mod_delayed_work(system_percpu_wq, &bt->poll, 0);
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
	cancel_delayed_work_sync(&bt->poll);
	pci_clear_master(bt->pdev);
	kvfree(bt->capture);
	bt->capture = NULL;
	/* Retain any published arena after detaching until a fresh firmware boot. */
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
	seq_printf(s, "peripheral_exec=%u peripheral_ipc=%u sleep_status=%u transport_paused=%u\n",
		   le32_to_cpu(READ_ONCE(*(__le32 *)(bt->arena + 0x68))),
		   le32_to_cpu(READ_ONCE(*(__le32 *)(bt->arena + 0x6c))),
		   le32_to_cpu(READ_ONCE(*(__le32 *)(bt->arena + 0x70))),
		   bt->transport_paused);
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
	seq_printf(s, "hrb_connection_events=%u hrb_last_link_type=0x%02x hrb_sync_commands=%u hrb_sync_status_events=%u voice_hci_routed=%u\n",
		   bt->hrb_connection_events, bt->hrb_last_link_type,
		   bt->hrb_sync_commands, bt->hrb_sync_status_events,
		   READ_ONCE(bt->voice_hci_routed));
	seq_printf(s, "accept_list=%u accept_list_hidden=%u\n", accept_list,
		   bt->accept_list_hidden);
	seq_printf(s, "scan_dup_filter=%u scan_dup_rewritten=%u\n", scan_dup_filter,
		   bt->scan_dup_rewritten);
	seq_printf(s, "acl_enabled=%u acl_tx=%u acl_rx=%u acl_queued=%u le_queued=%u\n",
		   enable_acl, bt->hdev->stat.acl_tx, bt->hdev->stat.acl_rx,
		   skb_queue_len(&bt->acl_txq), skb_queue_len(&bt->le_txq));
	seq_printf(s, "sco_tx=%u sco_rx=%u sco_queued=%u iso_queued=%u sco_mtu=%u sco_pkts=%u\n",
		   bt->hdev->stat.sco_tx, bt->hdev->stat.sco_rx,
		   skb_queue_len(&bt->sco_txq), skb_queue_len(&bt->iso_txq),
		   bt->hdev->sco_mtu, bt->hdev->sco_pkts);
	seq_printf(s, "auxiliary_rings=%u messages=%u message_head=%u capture_bytes=%u capture_records=%u capture_dropped=%u\n",
		   arm_auxiliary_rings, bt->message_tag, bt->message_head,
		   bt->capture_used, bt->capture_records, bt->capture_dropped);
	if (arm_auxiliary_rings)
		for (i = 0; i < ARRAY_SIZE(cen_aux_specs); i++)
			if (cen_aux_specs[i].receive && bt->aux[i].opened)
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
	struct pci_host_bridge *bridge = pci_find_host_bridge(pdev->bus);

	/*
	 * The T6050 transport (btcentauri_t6050.c) owns every N1 function behind
	 * an "apple,t6050-pcie" host, as in centauri_pci.c. Any other host is
	 * ours; the firmware and IPC state are checked below.
	 */
	if ((bridge && bridge->dev.parent &&
	     of_device_is_compatible(bridge->dev.parent->of_node, "apple,t6050-pcie")) ||
	    PCI_FUNC(pdev->devfn) != 2)
		return -ENODEV;
	bt = devm_kzalloc(&pdev->dev, sizeof(*bt), GFP_KERNEL);
	if (!bt)
		return -ENOMEM;
	bt->pdev = pdev;
	mutex_init(&bt->lock);
	skb_queue_head_init(&bt->txq);
	skb_queue_head_init(&bt->acl_txq);
	skb_queue_head_init(&bt->le_txq);
	skb_queue_head_init(&bt->sco_txq);
	skb_queue_head_init(&bt->iso_txq);
	if (enable_acl && (!arm_le_data_rings || !arm_auxiliary_rings))
		return -EINVAL;
	if (diagnostic_capture && !arm_auxiliary_rings)
		return -EINVAL;
	if (native_hrb && ((!native_startup && !factory_address) || !enable_acl))
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
	/*
	 * The function appears with the N1's IPC start, before Beta's firmware runs.
	 * apple_centauri attaches it again once that firmware is up.
	 */
	if (readl(bt->bar + 0x18) != 2 || readl(bt->bar) != 0) {
		dev_info(&pdev->dev, "waiting for Beta's firmware (exec=%u ipc=%u)\n",
			 readl(bt->bar + 0x18), readl(bt->bar));
		return -ENXIO;
	}
	ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(40));
	if (ret)
		return ret;
	bt->arena = dma_alloc_coherent(&pdev->dev, CEN_ARENA_SIZE, &bt->dma, GFP_KERNEL);
	if (!bt->arena)
		return -ENOMEM;
	memset(bt->arena, 0, CEN_ARENA_SIZE);
	if (diagnostic_capture) {
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
	hdev->set_bdaddr = cen_hci_set_bdaddr;
	hdev->post_init = cen_hci_post_init;
	hdev->reset = cen_hci_reset;
	hdev->reset_dup_filter = cen_hci_reset_dup_filter;
	/*
	 * The N1 sets bits above the PHY value in LE Extended Advertising
	 * Reports, as on T6050 (btcentauri_t6050.c). Unmasked, no PHY matches in
	 * hci_le_ext_create_conn_sync(): LE Extended Create Connection goes out
	 * with no initiating PHY and LE devices never connect.
	 */
	if (fixup_adv_phy)
		hci_set_quirk(hdev, HCI_QUIRK_FIXUP_LE_EXT_ADV_REPORT_PHY);
	hci_set_quirk(hdev, HCI_QUIRK_WIDEBAND_SPEECH_SUPPORTED);
	/*
	 * The N1 lists Enhanced Setup Synchronous Connection in its supported
	 * commands but answers it with Unsupported Feature or Parameter Value
	 * (0x11), so no SCO/eSCO link comes up and a headset's microphone stays
	 * silent. Use Setup Synchronous Connection instead, with the air mode
	 * (transparent for mSBC) in the voice setting.
	 */
	hci_set_quirk(hdev, HCI_QUIRK_BROKEN_ENHANCED_SETUP_SYNC_CONN);
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
	dev_info(&pdev->dev, "Beta HCI transport registered; ACL/SCO/ISO enabled=%u\n",
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

/*
 * Beta ACIPC host-sleep uses BAR0 + 0x1078 for sleep_control (3 = sleep,
 * 0 = wake) and polls peripheral_info.sleep_status at context + 0x70
 * (3 = asleep, <= 1 = awake). Keep all ring doorbells paused until wake.
 */
static u32 cen_sleep_status(struct cen_bt *bt)
{
	return le32_to_cpu(READ_ONCE(*(__le32 *)(bt->arena + 0x70)));
}

static int cen_transport_state(struct cen_bt *bt, bool asleep)
{
	u32 status;
	int ret;

	if (readl(bt->bar + 0x18) != 2 || readl(bt->bar) != 2)
		return -EIO;
	dma_wmb();
	writel(asleep ? 3 : 0, bt->bar + 0x1078);
	ret = read_poll_timeout(cen_sleep_status, status,
				asleep ? status == 3 : status <= 1,
				100, 2000000, false, bt);
	dma_rmb();
	dev_dbg(&bt->pdev->dev,
		"ACIPC host %s: ret=%d status=%u exec=%u ipc=%u\n",
		asleep ? "sleep" : "wake", ret, status,
		readl(bt->bar + 0x18), readl(bt->bar));
	return ret;
}

static int cen_resume(struct device *dev);

static int cen_suspend(struct device *dev)
{
	struct cen_bt *bt = pci_get_drvdata(to_pci_dev(dev));
	struct pci_dev *pdev = bt->pdev;
	u16 flags;
	int cap, ret;

	mutex_lock(&bt->lock);
	WRITE_ONCE(bt->transport_paused, true);
	mutex_unlock(&bt->lock);
	cancel_delayed_work_sync(&bt->poll);
	/*
	 * Tell Beta's firmware the host is going to sleep on every N1 board, as
	 * Alpha does. Only a running firmware can take the request; one that
	 * isn't running has nothing to quiesce.
	 */
	if (bt->ipc_requested && readl(bt->bar + 0x18) == 2 &&
	    readl(bt->bar) == 2) {
		bt->transport_sleep_sent = true;
		ret = cen_transport_state(bt, true);
		if (ret)
			goto undo;
	}
	bt->sleep_msi.saved = false;
	cap = pci_find_capability(pdev, PCI_CAP_ID_MSI);
	if (cap && !pdev->msi_enabled &&
	    !pci_read_config_word(pdev, cap + PCI_MSI_FLAGS, &flags) &&
	    (flags & PCI_MSI_FLAGS_ENABLE) && (flags & PCI_MSI_FLAGS_64BIT)) {
		bt->sleep_msi.cap = cap;
		bt->sleep_msi.flags = flags;
		bt->sleep_msi.saved =
			!pci_read_config_dword(pdev, cap + PCI_MSI_ADDRESS_LO, &bt->sleep_msi.lo) &&
			!pci_read_config_dword(pdev, cap + PCI_MSI_ADDRESS_HI, &bt->sleep_msi.hi) &&
			!pci_read_config_word(pdev, cap + PCI_MSI_DATA_64, &bt->sleep_msi.data);
	}
	dev_dbg(dev, "Beta suspend exec=%x ipc=%x fallback MSI saved=%d\n",
		readl(bt->bar + 0x18), readl(bt->bar), bt->sleep_msi.saved);
	device_set_awake_path(dev);
	ret = pci_save_state(pdev);
	if (!ret)
		return 0;
undo:
	cen_resume(dev);
	return ret;
}

static int cen_resume_noirq(struct device *dev)
{
	struct cen_bt *bt = pci_get_drvdata(to_pci_dev(dev));
	struct pci_dev *pdev = bt->pdev;
	int cap = bt->sleep_msi.cap;

	dev_dbg(dev, "Beta noirq exec=%x ipc=%x fallback MSI saved=%d\n",
		readl(bt->bar + 0x18), readl(bt->bar), bt->sleep_msi.saved);
	/*
	 * PERST clears MSI. The PCI core only restores vectors it allocated;
	 * Centauri's DMA-backed MSI mailbox is programmed directly by the
	 * Control driver. Restore it before interrupts and polling resume.
	 */
	if (bt->sleep_msi.saved && !pdev->msi_enabled) {
		pci_write_config_word(pdev, cap + PCI_MSI_FLAGS,
				      bt->sleep_msi.flags & ~PCI_MSI_FLAGS_ENABLE);
		pci_write_config_dword(pdev, cap + PCI_MSI_ADDRESS_LO, bt->sleep_msi.lo);
		pci_write_config_dword(pdev, cap + PCI_MSI_ADDRESS_HI, bt->sleep_msi.hi);
		pci_write_config_word(pdev, cap + PCI_MSI_DATA_64, bt->sleep_msi.data);
		pci_write_config_word(pdev, cap + PCI_MSI_FLAGS, bt->sleep_msi.flags);
	}
	return 0;
}

static int cen_resume(struct device *dev)
{
	struct cen_bt *bt = pci_get_drvdata(to_pci_dev(dev));
	int ret;

	if (bt->transport_sleep_sent) {
		ret = cen_transport_state(bt, false);
		if (ret)
			return ret;
		bt->transport_sleep_sent = false;
	}
	mutex_lock(&bt->lock);
	WRITE_ONCE(bt->transport_paused, false);
	if (bt->running)
		mod_delayed_work(system_percpu_wq, &bt->poll, 0);
	mutex_unlock(&bt->lock);
	return 0;
}

static const struct dev_pm_ops cen_pm_ops = {
	SYSTEM_SLEEP_PM_OPS(cen_suspend, cen_resume)
	NOIRQ_SYSTEM_SLEEP_PM_OPS(NULL, cen_resume_noirq)
};

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
	.driver.pm = pm_sleep_ptr(&cen_pm_ops),
};
module_pci_driver(cen_driver);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Apple N1 Beta HCI Bluetooth transport");
