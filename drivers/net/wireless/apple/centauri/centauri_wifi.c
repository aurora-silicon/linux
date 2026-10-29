// SPDX-License-Identifier: GPL-2.0-only

#include <linux/etherdevice.h>
#include <linux/ieee80211.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/unaligned.h>
#include <linux/workqueue.h>
#include <net/cfg80211.h>
#include <net/regulatory.h>
#include "centauri.h"

#define CR_BASE 0x100000
#define TR_BASE 0x400000
#define RING_SPAN 0x20000
#define TX_BUFFER 0x600000
#define BUFFER_SIZE 2048

#define DATA_RX_BUFFER SZ_16M
#define DATA_TX_BUFFER (SZ_16M + SZ_8M)
#define DATA_RX_SLOTS 2048
#define DATA_TX_SLOTS 2048
#define DATA_TX_QUEUE_LIMIT DATA_TX_SLOTS

static_assert(DATA_TX_BUFFER >=
	      CEN_ALPHA_SCRATCH_OFFSET + CEN_ALPHA_SCRATCH_SIZE);
static_assert(DATA_RX_BUFFER >=
	      CEN_ALPHA_SCRATCH_OFFSET + CEN_ALPHA_SCRATCH_SIZE);
static_assert(DATA_RX_BUFFER + DATA_RX_SLOTS * 4096 <= DATA_TX_BUFFER);
static_assert(DATA_TX_BUFFER + DATA_TX_SLOTS * BUFFER_SIZE <=
	      CEN_ALPHA_WINDOW_SIZE);

struct centauri_wifi {
	struct centauri *control;
	struct cen_alpha_resources a;
	struct wiphy *wiphy;
	struct wireless_dev wdev;
	struct net_device *netdev;
	struct work_struct scan_work;
	struct work_struct connect_work;
	struct work_struct link_work;
	bool irq_ready;
	struct cfg80211_scan_request *scan_request;
	struct mutex lock; /* Serializes firmware commands and ring consumers. */

	u16 main_head, command_head, sequence;
	u32 rx_seen[5];
	u16 rx_next[5], rx_head[5];
	u16 reply_sequence, reply_status;
	u32 reply_command, channels_seen, scans_done, beacons_seen;
	bool reply_valid, registered, configured;
	bool stopping, command_failed;
	bool scan_abort, connect_abort;
	u8 scan_sequence;
	bool connecting, connected, connect_event, disconnect_event;
	bool disconnect_pending;
	u16 connect_status;
	u8 bssid[ETH_ALEN], connect_payload[1024], request_ie[256];
	size_t connect_size, request_ie_size;
	u32 data_rx_received, data_rx_reposted, data_rx_bad;
	struct sk_buff_head data_tx_queue;
	u16 data_tx_length[DATA_TX_SLOTS];
	bool data_tx_busy[DATA_TX_SLOTS];
	u32 data_tx_submitted, data_tx_completed, data_tx_failed;
	u32 data_tx_coalesced, data_tx_bad;
	s8 station_signal;
	bool station_signal_valid;
	unsigned long station_signal_at, connected_at;
	struct net_device_stats station_start;
	u8 country[3];
	bool country_valid;
	u32 data_rings_open;
	bool data_failed;
	struct ieee80211_supported_band bands[2];
	struct ieee80211_channel channels[39];
};

static int query_link_status(struct centauri_wifi *wifi);

static const u32 rx_buffer[5] = { 0, 0, 0x620000, 0x680000, 0x700000 };
static const u16 ring_size[5] = { 0, 64, 192, 96, 192 };
static const u16 five_ghz[] = {
	36,  40,  44,  48,  52,	 56,  60,  64,	100, 104, 108, 112, 116,
	120, 124, 128, 132, 136, 140, 144, 149, 153, 157, 161, 165,
};
static struct ieee80211_rate rates[] = {
	{ .bitrate = 10 },  { .bitrate = 20 },	{ .bitrate = 55 },
	{ .bitrate = 110 }, { .bitrate = 60 },	{ .bitrate = 90 },
	{ .bitrate = 120 }, { .bitrate = 180 }, { .bitrate = 240 },
	{ .bitrate = 360 }, { .bitrate = 480 }, { .bitrate = 540 },
};

static const u32 akm_suites[] = { WLAN_AKM_SUITE_PSK };

static const u32 cipher_suites[] = {
	WLAN_CIPHER_SUITE_CCMP,
	WLAN_CIPHER_SUITE_AES_CMAC,
};

static u16 index_read(struct centauri_wifi *wifi, unsigned int array,
		      unsigned int index)
{
	return le16_to_cpu(READ_ONCE(*(
		__le16 *)(wifi->a.context + wifi->a.index[array] + 2 * index)));
}

static void index_write(struct centauri_wifi *wifi, unsigned int array,
			unsigned int index, u16 value)
{
	WRITE_ONCE(
		*(__le16 *)(wifi->a.context + wifi->a.index[array] + 2 * index),
		cpu_to_le16(value));
}

static void completions(struct centauri_wifi *wifi)
{
	unsigned int vector;

	for (vector = 0; vector <= 4; vector++)
		index_write(wifi, 2, vector, index_read(wifi, 0, vector));
	dma_wmb();
}

static int alpha_message(struct centauri_wifi *wifi, const void *message,
			 size_t size)
{
	u16 head = wifi->main_head;
	u8 *descriptor;
	unsigned long deadline = jiffies + msecs_to_jiffies(2000);

	if (head >= 15 || size > 512)
		return -ENOSPC;
	memcpy(wifi->a.messages + (head + 1) * 512, message, size);
	descriptor = wifi->a.context + wifi->a.mtr + 16 * head;
	put_unaligned_le32(1 | (size << 8), descriptor);
	put_unaligned_le64(wifi->a.messages_dma + (head + 1) * 512,
			   descriptor + 4);
	put_unaligned_le32(head + 1, descriptor + 12);
	index_write(wifi, 3, 0, head + 1);
	if (head)
		index_write(wifi, 3, 8, head + 1);
	dma_wmb();
	wifi->main_head++;
	writel(1, wifi->a.bar + 0x1004);
	do {
		if (readl(wifi->a.bar + 8) != 2)
			return -EIO;
		if (index_read(wifi, 1, 0) >= head + 1) {
			dma_rmb();
			completions(wifi);
			return 0;
		}
		usleep_range(100, 200);
	} while (time_before(jiffies, deadline));
	return -ETIMEDOUT;
}

static int open_pair(struct centauri_wifi *wifi, u16 id)
{
	u8 cr[0x2c] = { 2 }, tr[0x34] = { 1 };
	int ret;

	put_unaligned_le32(id | (id << 16), cr + 4);
	put_unaligned_le64(wifi->a.window_dma + CR_BASE + (id - 1) * RING_SPAN,
			   cr + 8);
	put_unaligned_le16(1024, cr + 0x10);
	put_unaligned_le16(0xffff, cr + 0x12);
	ret = alpha_message(wifi, cr, sizeof(cr));
	if (ret)
		return ret;
	put_unaligned_le32(id | (id << 16), tr + 4);
	put_unaligned_le64(wifi->a.window_dma + TR_BASE + (id - 1) * RING_SPAN,
			   tr + 8);
	put_unaligned_le64(~0ULL, tr + 0x10);
	put_unaligned_le16(ring_size[id], tr + 0x18);
	put_unaligned_le16(id, tr + 0x1a);
	put_unaligned_le16(id, tr + 0x1c);
	put_unaligned_le16(0x40, tr + 0x1e);
	put_unaligned_le16(2, tr + 0x24);
	return alpha_message(wifi, tr, sizeof(tr));
}

static void report_beacon(struct centauri_wifi *wifi, const u8 *payload,
			  size_t size)
{
	const u8 *frame, *ie, *end;
	struct ieee80211_channel *channel;
	struct cfg80211_bss *bss;
	u16 frame_size;
	int number = 0, frequency, signal;
	enum nl80211_band band;

	if (!wifi->registered || size < 0x60 + 36)
		return;

	frame_size = size - 0x60;
	frame = payload + 0x60;
	if (!ieee80211_is_beacon(*(__le16 *)frame) &&
	    !ieee80211_is_probe_resp(*(__le16 *)frame))
		return;
	end = frame + frame_size;
	for (ie = frame + 36; ie + 2 <= end && ie + 2 + ie[1] <= end;
	     ie += 2 + ie[1]) {
		if ((ie[0] == WLAN_EID_DS_PARAMS ||
		     ie[0] == WLAN_EID_HT_OPERATION) &&
		    ie[1])
			number = ie[2];
	}
	if (!number)
		return;
	band = number <= 14 ? NL80211_BAND_2GHZ : NL80211_BAND_5GHZ;
	frequency = ieee80211_channel_to_frequency(number, band);
	channel = ieee80211_get_channel(wifi->wiphy, frequency);
	if (!channel)
		return;
	signal = max_t(s8, payload[0x16], payload[0x17]);
	if (signal >= 0 || signal < -127)
		signal = -95;
	bss = cfg80211_inform_bss_frame(wifi->wiphy, channel,
					(struct ieee80211_mgmt *)frame,
					frame_size, signal * 100, GFP_KERNEL);
	if (bss) {
		wifi->beacons_seen++;
		cfg80211_put_bss(wifi->wiphy, bss);
	}
}

static void post_receive(struct centauri_wifi *wifi, unsigned int ring,
			 u16 slot)
{
	u8 *descriptor =
		wifi->a.window + TR_BASE + (ring - 1) * RING_SPAN + slot * 16;
	u32 offset = rx_buffer[ring] + slot * BUFFER_SIZE;

	memset(wifi->a.window + offset, 0, BUFFER_SIZE);
	put_unaligned_le32(1 | (BUFFER_SIZE << 8), descriptor);
	put_unaligned_le64(wifi->a.window_dma + offset, descriptor + 4);
	put_unaligned_le32(slot + 1, descriptor + 12);
}

static bool data_rx_frame(const u8 *packet, unsigned int capacity,
			  unsigned int length, u32 footer, unsigned int *offset,
			  unsigned int *frame_length)
{
	unsigned int padding = (footer >> 12) & 15;

	if (!(footer & BIT(7)))
		return false;
	if (length < padding + ETH_HLEN || length > capacity)
		return false;
	if (footer & BIT(24)) {
		if (capacity < 4)
			return false;
		*offset = get_unaligned_le16(packet + 2);
	} else {
		*offset = padding;
	}
	*frame_length = length - padding;
	return *offset <= capacity && *frame_length <= capacity - *offset;
}

static void drain_data_rx(struct centauri_wifi *wifi)
{
	u16 head, tail, post;
	unsigned int processed = 0;

	if (!(wifi->data_rings_open & BIT(4)) || readl(wifi->a.bar + 8) != 2)
		return;
	head = index_read(wifi, 0, 207);
	tail = index_read(wifi, 2, 207);
	post = index_read(wifi, 3, 12);
	if (head >= DATA_RX_SLOTS || tail >= DATA_RX_SLOTS ||
	    post >= DATA_RX_SLOTS) {
		wifi->data_rx_bad++;
		return;
	}
	dma_rmb();
	while (tail != head && processed < (DATA_RX_SLOTS - 1)) {
		u8 *completion = wifi->a.window + 0x240000 + tail * 32;
		u32 header = get_unaligned_le32(completion);
		u32 info = get_unaligned_le32(completion + 4);
		u16 cookie = info & 0xffff, length = info >> 16;
		u16 next = (post + 1) % DATA_RX_SLOTS;
		u8 *packet, *descriptor;
		u32 buffer;

		if ((header >> 16) != 12 || !cookie ||
		    cookie > (DATA_RX_SLOTS - 1) || length > 4096 ||
		    get_unaligned_le32(completion + 8)) {
			wifi->data_rx_bad++;
			break;
		}

		if (next == index_read(wifi, 1, 12))
			break;
		buffer = DATA_RX_BUFFER + (cookie - 1) * 4096;
		packet = wifi->a.window + buffer;

		wifi->data_rx_received++;
		if (wifi->connected && netif_running(wifi->netdev)) {
			u32 footer = get_unaligned_le32(completion + 12);
			unsigned int offset, frame_length;
			struct sk_buff *skb = NULL;

			if (data_rx_frame(packet, 4096, length, footer, &offset,
					  &frame_length))
				skb = netdev_alloc_skb_ip_align(wifi->netdev,
								frame_length);
			if (skb) {
				skb_put_data(skb, packet + offset,
					     frame_length);
				skb->protocol =
					eth_type_trans(skb, wifi->netdev);
				skb->ip_summed = CHECKSUM_NONE;
				wifi->netdev->stats.rx_packets++;
				wifi->netdev->stats.rx_bytes += frame_length;
				netif_rx(skb);
			} else {
				wifi->netdev->stats.rx_dropped++;
			}
		} else {
			wifi->netdev->stats.rx_dropped++;
		}
		descriptor = wifi->a.window + 0x520000 + post * 16;
		put_unaligned_le32(1 | (4096 << 8), descriptor);
		put_unaligned_le64(wifi->a.window_dma + buffer, descriptor + 4);
		put_unaligned_le32(cookie, descriptor + 12);
		wifi->data_rx_reposted++;
		tail = (tail + 1) % DATA_RX_SLOTS;
		post = next;
		processed++;
	}
	if (processed) {
		dma_wmb();
		index_write(wifi, 2, 207, tail);
		index_write(wifi, 3, 12, post);
		dma_wmb();
		writel(1, wifi->a.bar + 0x1050);
	}
}

static bool data_tx_completion_span(u16 cookie, u16 wire_count, u8 flags,
				    unsigned int *first, u16 *count)
{
	bool counted = (flags & BIT(0)) || (flags & 0x0e) == 8;

	if (!cookie || cookie > DATA_TX_SLOTS || counted != !!wire_count ||
	    wire_count >= DATA_TX_SLOTS)
		return false;
	*count = counted ? wire_count : 1;
	*first = (cookie + DATA_TX_SLOTS - *count) % DATA_TX_SLOTS;
	return true;
}

static void drain_data_tx(struct centauri_wifi *wifi)
{
	u16 head, tail, post;
	unsigned int budget = DATA_TX_SLOTS;
	bool posted = false, completed = false;

	if (wifi->command_failed || !(wifi->data_rings_open & BIT(5)) ||
	    readl(wifi->a.bar + 8) != 2)
		return;
	head = index_read(wifi, 0, 192);
	tail = index_read(wifi, 2, 192);
	post = index_read(wifi, 3, 25);
	if (head >= 2048 || tail >= 2048 || post >= DATA_TX_SLOTS)
		return;
	dma_rmb();
	while (tail != head && budget--) {
		u8 *entry = wifi->a.window + 0x200000 + tail * 32;
		u16 ring = get_unaligned_le32(entry) >> 16;
		u16 cookie = get_unaligned_le16(entry + 4);
		u16 count = get_unaligned_le16(entry + 9);
		u8 status = entry[12];

		if (ring == 25) {
			unsigned int first, i;

			if (!data_tx_completion_span(cookie, count, entry[1],
						     &first, &count)) {
				wifi->data_tx_bad++;
				break;
			}
			for (i = 0; i < count; i++)
				if (!wifi->data_tx_busy[(first + i) %
							DATA_TX_SLOTS])
					break;
			if (i != count) {
				wifi->data_tx_bad++;
				break;
			}
			if (count > 1)
				wifi->data_tx_coalesced++;
			for (i = 0; i < count; i++) {
				unsigned int slot = (first + i) % DATA_TX_SLOTS;

				wifi->data_tx_completed++;
				if (status == 0 || status == 0x83) {
					wifi->netdev->stats.tx_packets++;
					wifi->netdev->stats.tx_bytes +=
						wifi->data_tx_length[slot];
				} else {
					wifi->data_tx_failed++;
					wifi->netdev->stats.tx_errors++;
				}
				wifi->data_tx_busy[slot] = false;
			}
		}
		tail = (tail + 1) % 2048;
		completed = true;
	}
	if (completed)
		index_write(wifi, 2, 192, tail);
	budget = DATA_TX_SLOTS - 1;
	while (wifi->connected && !READ_ONCE(wifi->stopping) &&
	       netif_running(wifi->netdev) && budget--) {
		u16 next = (post + 1) % DATA_TX_SLOTS;
		u32 buffer = DATA_TX_BUFFER + post * BUFFER_SIZE;
		u8 *descriptor = wifi->a.window + 0x540000 + post * 32;
		struct sk_buff *skb;
		u64 metadata = 6ULL << 27;

		if (wifi->data_tx_busy[post] || next == index_read(wifi, 1, 25))
			break;
		skb = skb_dequeue(&wifi->data_tx_queue);
		if (!skb)
			break;

		if (skb_copy_bits(skb, 0, wifi->a.window + buffer, skb->len)) {
			wifi->netdev->stats.tx_dropped++;
			dev_kfree_skb_any(skb);
			continue;
		}
		memset(descriptor, 0, 32);
		put_unaligned_le32(3 | (skb->len << 8), descriptor);
		put_unaligned_le64(wifi->a.window_dma + buffer, descriptor + 4);
		put_unaligned_le16(post + 1, descriptor + 12);

		metadata |= 4ULL << 15;
		put_unaligned_le64(metadata, descriptor + 16);
		put_unaligned_le64(2000000, descriptor + 24);
		wifi->data_tx_length[post] = skb->len;
		wifi->data_tx_busy[post] = true;
		wifi->data_tx_submitted++;
		dev_kfree_skb_any(skb);
		post = next;
		posted = true;
	}
	if (posted || completed) {
		dma_wmb();
		if (posted)
			index_write(wifi, 3, 25, post);
		dma_wmb();
		writel(1, wifi->a.bar + 0x1030);
	}
	if (wifi->connected && !READ_ONCE(wifi->stopping) &&
	    netif_running(wifi->netdev) &&
	    skb_queue_len(&wifi->data_tx_queue) < DATA_TX_QUEUE_LIMIT)
		netif_wake_queue(wifi->netdev);
}

static bool link_signal_read(const u8 *packet, size_t size, s8 *signal)
{
	size_t pos = 32;

	while (pos <= size && size - pos >= 4) {
		u16 tag = get_unaligned_le16(packet + pos);
		u16 length = get_unaligned_le16(packet + pos + 2);

		if (length > size - pos - 4)
			return false;
		if (tag == 268 && length >= 115) {
			*signal = (s8)packet[pos + 5];
			return *signal != 0;
		}
		pos += 4 + length;
	}
	return false;
}

static void drain(struct centauri_wifi *wifi)
{
	unsigned int ring;
	bool posted = false;

	for (ring = 2; ring <= 4; ring++) {
		unsigned int budget = ring_size[ring];

		while (budget--) {
			u16 slot = wifi->rx_next[ring], size, status, sequence;
			u32 command, wire_command;
			u8 *descriptor = wifi->a.window + TR_BASE +
					 (ring - 1) * RING_SPAN + slot * 16;
			u8 *packet = wifi->a.window + rx_buffer[ring] +
				     slot * BUFFER_SIZE;

			if (!(READ_ONCE(descriptor[15]) & 4))
				break;
			dma_rmb();
			size = get_unaligned_le16(packet + 0x10);
			status = get_unaligned_le16(packet + 0xe);
			sequence = get_unaligned_le16(packet + 0x12);
			wire_command = get_unaligned_le32(packet + 0x14);
			command = wire_command & ~BIT(30);
			wifi->rx_seen[ring]++;
			wifi->rx_next[ring] = (slot + 1) % ring_size[ring];
			if (size < 24 || size > BUFFER_SIZE)
				goto refill;
			if (ring == 4) {
				report_beacon(wifi, packet + 24, size - 24);
			} else if (command & BIT(31)) {
				if (command == 0x80000030)
					wifi->channels_seen++;
				if (command == 0x80000005)
					wifi->scans_done++;
				if (command == 0x80000000 && wifi->connecting) {
					wifi->connect_event = true;
					wifi->connect_status = status;
				}
				if (command == 0x80000003 ||
				    command == 0x80000004)
					wifi->disconnect_event = true;
				if (command == 0x80000004)
					wifi->disconnect_pending = false;

			} else {
				wifi->reply_valid = true;
				wifi->reply_sequence = sequence;
				wifi->reply_status = status;

				wifi->reply_command = wire_command;
				if (command == 10) {
					wifi->country_valid = !status &&
							      size >= 27;
					if (wifi->country_valid)
						memcpy(wifi->country,
						       packet + 24, 3);
				}

				if (command == 20 && !status) {
					wifi->station_signal_valid =
						link_signal_read(
							packet, size,
							&wifi->station_signal);
					wifi->station_signal_at = jiffies;
				}
			}
refill:
			post_receive(wifi, ring, wifi->rx_head[ring]);
			wifi->rx_head[ring] =
				(wifi->rx_head[ring] + 1) % ring_size[ring];
			dma_wmb();
			index_write(wifi, 3, ring, wifi->rx_head[ring]);
			posted = true;
		}
	}
	completions(wifi);
	if (posted)
		writel(1, wifi->a.bar + 0x1004);
	drain_data_rx(wifi);
	drain_data_tx(wifi);
}

/* Caller holds wifi->lock. Unknown completion keeps DMA payloads owned. */
static void command_fault(struct centauri_wifi *wifi)
{
	wifi->command_failed = true;
	wifi->configured = false;
	wifi->data_failed = true;
	netif_stop_queue(wifi->netdev);
	if (smp_load_acquire(&wifi->irq_ready))
		schedule_work(&wifi->link_work);
}

static bool command_payload_consumed(struct centauri_wifi *wifi, u16 slot)
{
	if (slot >= ring_size[1] ||
	    index_read(wifi, 1, 1) != (slot + 1) % ring_size[1])
		return false;
	dma_rmb();
	return true;
}

static int command(struct centauri_wifi *wifi, u32 id, const void *payload,
		   size_t size)
{
	u16 slot = wifi->command_head, sequence = ++wifi->sequence;
	u32 stage;
	u8 *packet, *descriptor;
	unsigned long deadline = jiffies + msecs_to_jiffies(1500);

	if (wifi->command_failed)
		return -EIO;
	if (size > BUFFER_SIZE - 24)
		return -EMSGSIZE;
	if ((slot + 1) % ring_size[1] == index_read(wifi, 1, 1))
		return -ENOSPC;
	packet = wifi->a.window + TX_BUFFER + slot * BUFFER_SIZE;
	memset(packet, 0, 24 + size);
	put_unaligned_le16(24 + size, packet + 0x10);
	put_unaligned_le16(sequence, packet + 0x12);
	put_unaligned_le32(id, packet + 0x14);
	if (size)
		memcpy(packet + 24, payload, size);
	descriptor = wifi->a.window + TR_BASE + slot * 16;
	put_unaligned_le32(1 | ((24 + size) << 8), descriptor);
	put_unaligned_le64(wifi->a.window_dma + TX_BUFFER + slot * BUFFER_SIZE,
			   descriptor + 4);
	put_unaligned_le32(slot + 1, descriptor + 12);
	dma_wmb();
	wifi->command_head = (slot + 1) % ring_size[1];
	index_write(wifi, 3, 1, wifi->command_head);
	wifi->reply_valid = false;
	dma_wmb();
	writel(1, wifi->a.bar + 0x1004);
	do {
		if (READ_ONCE(wifi->stopping) ||
		    READ_ONCE(wifi->control->removing)) {
			command_fault(wifi);
			return -ECANCELED;
		}
		drain(wifi);
		stage = readl(wifi->a.bar + 8);
		if (stage != 2) {
			dev_err(&wifi->control->pdev->dev,
				"Wi-Fi command %#x lost transport: stage=%#x sequence=%u rx=%u\n",
				id, stage, sequence, wifi->rx_seen[2]);
			command_fault(wifi);
			return -EIO;
		}

		if (wifi->reply_valid && wifi->reply_sequence == sequence &&
		    (wifi->reply_command & ~BIT(30)) == id) {
			return wifi->reply_status ? -EREMOTEIO : 0;
		}
		usleep_range(500, 1000);
	} while (time_before(jiffies, deadline));
	dev_err(&wifi->control->pdev->dev,
		"Wi-Fi command %#x timed out (rx=%u)\n", id, wifi->rx_seen[2]);
	command_fault(wifi);
	return -ETIMEDOUT;
}

static int station_configure(struct centauri_wifi *wifi, const u8 *address)
{
	u8 start[5 + 4 + 3 * ETH_ALEN] = { 0, 0x0b, 0, 0, 0 };
	size_t start_size = 5;
	int ret;

	ret = command(wifi, 1, address, ETH_ALEN);
	if (!ret)
		ret = command(wifi, 2, start, start_size);
	return ret;
}

static int radio_start(struct centauri_wifi *wifi)
{
	unsigned int ring, slot;
	int ret;

	for (ring = 1; ring <= 4; ring++) {
		ret = open_pair(wifi, ring);
		if (ret)
			return ret;
	}
	for (ring = 2; ring <= 4; ring++) {
		for (slot = 0; slot < ring_size[ring] - 1; slot++)
			post_receive(wifi, ring, slot);
		wifi->rx_head[ring] = ring_size[ring] - 1;
		dma_wmb();
		index_write(wifi, 3, ring, ring_size[ring] - 1);
	}
	dma_wmb();
	writel(1, wifi->a.bar + 0x1004);
	msleep(400);
	ret = command(wifi, 0, NULL, 0);
	if (!ret)
		ret = station_configure(wifi, wifi->netdev->dev_addr);
	if (ret)
		return ret;
	msleep(1000);
	wifi->configured = true;
	return 0;
}

#define CEN_SCAN_CHANNELS_MAX 59
static int scan_profile27(struct centauri_wifi *wifi)
{
	struct cfg80211_scan_request *request = wifi->scan_request;
	u8 scan[14 + 5 + 4 * CEN_SCAN_CHANNELS_MAX + 9 + 4 + 2 + 32];
	unsigned int next = 0;

	if (!request || !request->n_channels || request->n_ssids > 1)
		return -EINVAL;
	while (next < request->n_channels) {
		u8 channels[1 + 4 * CEN_SCAN_CHANNELS_MAX] = {};
		u8 options[5] = { 0, 0, 1, 0, 0 };
		unsigned int count = 0;
		u32 done = wifi->scans_done;
		size_t size = 14;
		unsigned long deadline;
		int ret;
		bool abort_sent = false;

		if (READ_ONCE(wifi->scan_abort))
			return -ECANCELED;
		memset(scan, 0, sizeof(scan));
		scan[0] = (++wifi->scan_sequence) & 0x1f;
		scan[3] = 0x80;
		scan[5] = 2;
		scan[7] = 0x10;

		put_unaligned_le16(40, scan + 8);
		put_unaligned_le16(40, scan + 10);
		put_unaligned_le16(110, scan + 12);
		while (next < request->n_channels &&
		       count < CEN_SCAN_CHANNELS_MAX) {
			struct ieee80211_channel *channel =
				request->channels[next++];
			unsigned int number;

			/* The passive-scan wire mode is not established for this profile. */
			if (channel->flags &
			    (IEEE80211_CHAN_DISABLED | IEEE80211_CHAN_NO_IR))
				continue;
			if (channel->band != NL80211_BAND_2GHZ &&
			    channel->band != NL80211_BAND_5GHZ)
				return -EOPNOTSUPP;
			number = ieee80211_frequency_to_channel(
				channel->center_freq);
			if (!number || number > 255)
				return -EINVAL;
			channels[1 + 4 * count] = number;
			channels[2 + 4 * count] = channel->band ==
						  NL80211_BAND_5GHZ;
			count++;
		}
		if (!count)
			continue;
		channels[0] = count;
		put_unaligned_le16(260, scan + size);
		put_unaligned_le16(1 + 4 * count, scan + size + 2);
		memcpy(scan + size + 4, channels, 1 + 4 * count);
		size += 5 + 4 * count;
		put_unaligned_le16(357, scan + size);
		put_unaligned_le16(sizeof(options), scan + size + 2);
		memcpy(scan + size + 4, options, sizeof(options));
		size += 4 + sizeof(options);
		if (request->n_ssids) {
			const struct cfg80211_ssid *ssid = &request->ssids[0];

			if (ssid->ssid_len > 32)
				return -EINVAL;

			scan[2] = !!ssid->ssid_len;
			put_unaligned_le16(258, scan + size);
			put_unaligned_le16(2 + ssid->ssid_len, scan + size + 2);
			scan[size + 4] = ssid->ssid_len ? 5 : 4;
			scan[size + 5] = ssid->ssid_len;
			memcpy(scan + size + 6, ssid->ssid, ssid->ssid_len);
			size += 6 + ssid->ssid_len;
		}
		ret = command(wifi, 65551, scan, size);
		if (ret)
			return ret;
		deadline = jiffies +
			   msecs_to_jiffies(max(8000U, count * 220 + 2000));
		do {
			drain(wifi);
			if (READ_ONCE(wifi->stopping) ||
			    readl(wifi->a.bar + 8) != 2)
				return -EIO;
			if (wifi->scans_done != done)
				break;
			if (READ_ONCE(wifi->scan_abort) && !abort_sent) {
				ret = command(wifi, 65537, NULL, 0);
				if (ret)
					return ret;
				abort_sent = true;
			}
			usleep_range(1000, 2000);
		} while (time_before(jiffies, deadline));
		if (wifi->scans_done == done) {
			command(wifi, 65537, NULL, 0);
			return -ETIMEDOUT;
		}
		if (READ_ONCE(wifi->scan_abort))
			return -ECANCELED;
	}
	return 0;
}

static void scan_worker(struct work_struct *work)
{
	struct centauri_wifi *wifi =
		container_of(work, struct centauri_wifi, scan_work);
	struct cfg80211_scan_info info = {};
	struct cfg80211_scan_request *request;
	int ret = 0;

	mutex_lock(&wifi->lock);
	if (READ_ONCE(wifi->scan_abort) || READ_ONCE(wifi->stopping)) {
		ret = -ECANCELED;
		goto completed;
	}
	{
		ret = scan_profile27(wifi);
		goto completed;
	}
completed:
	info.aborted = !!ret || READ_ONCE(wifi->scan_abort) ||
		       READ_ONCE(wifi->stopping);
	request = wifi->scan_request;
	wifi->scan_request = NULL;
	mutex_unlock(&wifi->lock);
	if (request)
		cfg80211_scan_done(request, &info);
}

static int wifi_scan(struct wiphy *wiphy, struct cfg80211_scan_request *request)
{
	struct centauri_wifi *wifi =
		*(struct centauri_wifi **)wiphy_priv(wiphy);
	int ret = 0;

	mutex_lock(&wifi->lock);
	if (READ_ONCE(wifi->stopping))
		ret = -ENODEV;
	else if (wifi->command_failed)
		ret = -EIO;
	else if (!wifi->configured)
		ret = -ENETDOWN;
	else if (wifi->scan_request || wifi->connecting ||
		 wifi->disconnect_pending)
		ret = -EBUSY;
	else {
		WRITE_ONCE(wifi->scan_abort, false);
		wifi->scan_request = request;
		schedule_work(&wifi->scan_work);
	}
	mutex_unlock(&wifi->lock);
	return ret;
}

static void wifi_abort_scan(struct wiphy *wiphy, struct wireless_dev *wdev)
{
	struct centauri_wifi *wifi =
		*(struct centauri_wifi **)wiphy_priv(wiphy);

	if (wdev == &wifi->wdev)
		WRITE_ONCE(wifi->scan_abort, true);
}

static void cancel_scan(struct centauri_wifi *wifi)
{
	struct cfg80211_scan_info info = { .aborted = true };
	struct cfg80211_scan_request *request;

	WRITE_ONCE(wifi->scan_abort, true);
	cancel_work_sync(&wifi->scan_work);
	mutex_lock(&wifi->lock);
	request = wifi->scan_request;
	wifi->scan_request = NULL;
	mutex_unlock(&wifi->lock);

	if (request)
		cfg80211_scan_done(request, &info);
}

static void cancel_connect_locked(struct centauri_wifi *wifi)
{
	if (!wifi->connecting)
		return;
	wifi->connecting = false;
	wifi->connected = false;
	memzero_explicit(wifi->connect_payload, sizeof(wifi->connect_payload));
	cfg80211_connect_timeout(wifi->netdev, wifi->bssid, wifi->request_ie,
				 wifi->request_ie_size, GFP_KERNEL,
				 NL80211_TIMEOUT_UNSPECIFIED);
}

static int rsn_caps_offset(const u8 *body, size_t size)
{
	size_t offset = 6;
	u16 count;
	unsigned int i;

	for (i = 0; i < 2; i++) {
		if (offset + 2 > size)
			return -EINVAL;
		count = get_unaligned_le16(body + offset);
		offset += 2 + 4 * count;
		if (offset > size)
			return -EINVAL;
	}
	return offset + 2 <= size ? offset : -EINVAL;
}

static int rsn_psk_offset(const u8 *body, size_t size)
{
	size_t offset, end;
	u16 count;
	unsigned int i;

	if (size < 8 || get_unaligned_le16(body) != 1)
		return -EINVAL;
	count = get_unaligned_le16(body + 6);
	if (!count)
		return -EINVAL;
	offset = 8 + 4 * count;
	if (offset + 2 > size)
		return -EINVAL;
	count = get_unaligned_le16(body + offset);
	end = offset + 2 + 4 * count;
	if (!count || end + 2 > size)
		return -EINVAL;
	for (i = 0; i < count; i++)
		if (get_unaligned_be32(body + offset + 2 + 4 * i) ==
		    WLAN_AKM_SUITE_PSK)
			return offset;
	return -EOPNOTSUPP;
}

static int select_psk_rsn(const struct cfg80211_crypto_settings *crypto,
			  const u8 *body, size_t size, u8 *selected)
{
	size_t tail;
	unsigned int i;
	int offset;

	for (i = 0; i < crypto->n_akm_suites; i++)
		if (crypto->akm_suites[i] == WLAN_AKM_SUITE_PSK)
			break;
	if (i == crypto->n_akm_suites)
		return -EOPNOTSUPP;
	offset = rsn_psk_offset(body, size);
	if (offset < 0)
		return offset;

	if (get_unaligned_be32(body + 2) != WLAN_CIPHER_SUITE_CCMP ||
	    get_unaligned_le16(body + 6) != 1 ||
	    get_unaligned_be32(body + 8) != WLAN_CIPHER_SUITE_CCMP)
		return -EOPNOTSUPP;
	tail = offset + 2 + 4 * get_unaligned_le16(body + offset);
	memcpy(selected, body, offset);
	put_unaligned_le16(1, selected + offset);
	put_unaligned_be32(WLAN_AKM_SUITE_PSK, selected + offset + 2);
	memcpy(selected + offset + 6, body + tail, size - tail);
	return offset + 6 + size - tail;
}

static void append_tlv(u8 *payload, size_t *size, u16 tag, const void *data,
		       u16 length)
{
	put_unaligned_le16(tag, payload + *size);
	put_unaligned_le16(length, payload + *size + 2);
	memcpy(payload + *size + 4, data, length);
	*size += 4 + length;
}

static int wifi_connect(struct wiphy *wiphy, struct net_device *netdev,
			struct cfg80211_connect_params *params)
{
	static const u8 mld_defaults[14] = {
		1,    0,    1,	  0xff, 0xff, 0xff, 0xff,
		0xff, 0xff, 0xff, 0xff, 3,    0,    0,
	};

	static const __le32 connection_timeouts[] = {
		cpu_to_le32(260),   cpu_to_le32(6000), cpu_to_le32(2000),
		cpu_to_le32(15000), cpu_to_le32(300),
	};
	struct centauri_wifi *wifi =
		*(struct centauri_wifi **)wiphy_priv(wiphy);
	struct cfg80211_bss *bss;
	const struct cfg80211_bss_ies *ies;
	const u8 *rsn, *ap_rsn;
	u8 rsn_body[255], ssid[35], zero[20] = {}, pmf;
	u8 *payload = wifi->connect_payload;
	size_t size = 130, rsn_size, rsn_prefix, rsn_tail;
	u16 caps = 0;
	int ret = 0, offset;

	if (!params->ssid_len || params->ssid_len > 32 || !params->crypto.psk ||
	    params->crypto.n_ciphers_pairwise != 1 ||
	    params->crypto.ciphers_pairwise[0] != WLAN_CIPHER_SUITE_CCMP ||
	    params->crypto.cipher_group != WLAN_CIPHER_SUITE_CCMP ||
	    params->ie_len > sizeof(wifi->request_ie))
		return -EOPNOTSUPP;
	rsn = cfg80211_find_ie(WLAN_EID_RSN, params->ie, params->ie_len);
	if (!rsn)
		return -EINVAL;
	ret = select_psk_rsn(&params->crypto, rsn + 2, rsn[1], rsn_body);
	if (ret < 0)
		return ret;
	rsn_size = ret;
	ret = 0;
	mutex_lock(&wifi->lock);
	if (READ_ONCE(wifi->stopping)) {
		ret = -ENODEV;
		goto out;
	}
	if (!wifi->configured || wifi->connecting || wifi->connected ||
	    wifi->scan_request || wifi->disconnect_pending) {
		ret = -EBUSY;
		goto out;
	}

	bss = cfg80211_get_bss(wiphy, params->channel, params->bssid,
			       params->ssid, params->ssid_len,
			       IEEE80211_BSS_TYPE_ESS, IEEE80211_PRIVACY_ON);
	if (!bss) {
		ret = -ENOENT;
		goto out;
	}

	rcu_read_lock();
	ies = rcu_dereference(bss->ies);
	ap_rsn = ies ? cfg80211_find_ie(WLAN_EID_RSN, ies->data, ies->len) :
		       NULL;
	if (!ap_rsn) {
		ret = -EINVAL;
	} else {
		ret = rsn_psk_offset(ap_rsn + 2, ap_rsn[1]);
		if (ret >= 0) {
			offset = rsn_caps_offset(ap_rsn + 2, ap_rsn[1]);
			caps = get_unaligned_le16(ap_rsn + 2 + offset);
			ret = 0;
		}
	}
	rcu_read_unlock();
	if (ret) {
		cfg80211_put_bss(wiphy, bss);
		goto out;
	}
	offset = rsn_caps_offset(rsn_body, rsn_size);
	put_unaligned_le16(caps, rsn_body + offset);
	pmf = !!(caps & BIT(7));
	memset(payload, 0, sizeof(wifi->connect_payload));
	payload[0] = 1;
	memcpy(payload + 1, bss->bssid, ETH_ALEN);
	memcpy(wifi->bssid, bss->bssid, ETH_ALEN);
	payload[7] = ieee80211_frequency_to_channel(bss->channel->center_freq);

	payload[8] = bss->channel->band == NL80211_BAND_2GHZ ? 0 : 1;
	payload[0x47] = 4;

	put_unaligned_le16(4, payload + 0x48);
	put_unaligned_le16(32, payload + 0x4a);
	memcpy(payload + 0x4c, params->crypto.psk, 32);

	append_tlv(payload, &size, 304, zero, 1);
	append_tlv(payload, &size, 63763, connection_timeouts,
		   sizeof(connection_timeouts));
	ssid[0] = 1;
	ssid[1] = 4;
	ssid[2] = params->ssid_len;
	memcpy(ssid + 3, params->ssid, params->ssid_len);
	append_tlv(payload, &size, 342, ssid, 3 + params->ssid_len);
	append_tlv(payload, &size, WLAN_EID_RSN, rsn_body, rsn_size);

	append_tlv(payload, &size, 316, &pmf, 1);
	append_tlv(payload, &size, 376, mld_defaults, sizeof(mld_defaults));
	wifi->connect_size = size;

	rsn_prefix = rsn - params->ie;
	rsn_tail = rsn_prefix + 2 + rsn[1];
	memcpy(wifi->request_ie, params->ie, rsn_prefix);
	wifi->request_ie[rsn_prefix] = WLAN_EID_RSN;
	wifi->request_ie[rsn_prefix + 1] = rsn_size;
	memcpy(wifi->request_ie + rsn_prefix + 2, rsn_body, rsn_size);
	memcpy(wifi->request_ie + rsn_prefix + 2 + rsn_size,
	       params->ie + rsn_tail, params->ie_len - rsn_tail);
	wifi->request_ie_size = params->ie_len - rsn[1] + rsn_size;
	WRITE_ONCE(wifi->connect_abort, false);
	wifi->connecting = true;
	wifi->connect_status = 0;
	wifi->connect_event = wifi->disconnect_event = false;
	cfg80211_put_bss(wiphy, bss);
	schedule_work(&wifi->connect_work);
out:
	mutex_unlock(&wifi->lock);
	return ret;
}

static void connect_worker(struct work_struct *work)
{
	struct centauri_wifi *wifi =
		container_of(work, struct centauri_wifi, connect_work);
	unsigned long deadline;
	u16 slot, status;
	int ret;

	mutex_lock(&wifi->lock);
	if (READ_ONCE(wifi->connect_abort) || READ_ONCE(wifi->stopping)) {
		cancel_connect_locked(wifi);
		mutex_unlock(&wifi->lock);
		return;
	}
	slot = wifi->command_head;
	ret = command(wifi, 131072, wifi->connect_payload, wifi->connect_size);
	memzero_explicit(wifi->connect_payload, sizeof(wifi->connect_payload));
	deadline = jiffies + msecs_to_jiffies(15000);
	while (!ret && !wifi->connect_event &&
	       !READ_ONCE(wifi->connect_abort) && !READ_ONCE(wifi->stopping) &&
	       time_before(jiffies, deadline)) {
		drain(wifi);

		if (wifi->disconnect_event)
			ret = -ENOTCONN;
		if (readl(wifi->a.bar + 8) != 2)
			ret = -EIO;
		usleep_range(1000, 2000);
	}
	if (READ_ONCE(wifi->connect_abort) || READ_ONCE(wifi->stopping))
		ret = -ECANCELED;
	else if (!ret && !wifi->connect_event)
		ret = -ETIMEDOUT;
	status = !ret && wifi->connect_status == 0 ?
			 WLAN_STATUS_SUCCESS :
			 WLAN_STATUS_UNSPECIFIED_FAILURE;
	wifi->connecting = false;
	wifi->connected = status == WLAN_STATUS_SUCCESS;
	wifi->station_signal_valid = false;
	if (wifi->connected) {
		wifi->connected_at = jiffies;
		wifi->station_signal_at = jiffies - HZ;
		wifi->station_start = wifi->netdev->stats;
	}

	if (command_payload_consumed(wifi, slot))
		memzero_explicit(wifi->a.window + TX_BUFFER +
					 slot * BUFFER_SIZE,
				 BUFFER_SIZE);

	if (ret == -ECANCELED || ret == -ETIMEDOUT)
		cfg80211_connect_timeout(wifi->netdev, wifi->bssid,
					 wifi->request_ie,
					 wifi->request_ie_size, GFP_KERNEL,
					 NL80211_TIMEOUT_UNSPECIFIED);
	else
		cfg80211_connect_result(wifi->netdev, wifi->bssid,
					wifi->request_ie, wifi->request_ie_size,
					NULL, 0, status, GFP_KERNEL);
	if (status == WLAN_STATUS_SUCCESS && !READ_ONCE(wifi->stopping)) {
		cfg80211_port_authorized(wifi->netdev, wifi->bssid, NULL, 0,
					 GFP_KERNEL);
		netif_carrier_on(wifi->netdev);
		if (wifi->data_rings_open & BIT(5))
			netif_wake_queue(wifi->netdev);
		schedule_work(&wifi->link_work);
	}
	mutex_unlock(&wifi->lock);
}

irqreturn_t centauri_alpha_irq(int irq, void *data)
{
	struct centauri *c = data;
	struct centauri_wifi *wifi = smp_load_acquire(&c->wifi);

	if (wifi && smp_load_acquire(&wifi->irq_ready) &&
	    !READ_ONCE(wifi->stopping))
		schedule_work(&wifi->link_work);
	return IRQ_HANDLED;
}

static void link_worker(struct work_struct *work)
{
	struct centauri_wifi *wifi =
		container_of(work, struct centauri_wifi, link_work);
	bool disconnected, was_connected;

	mutex_lock(&wifi->lock);
	was_connected = wifi->connected;
	drain(wifi);
	disconnected = wifi->command_failed || wifi->disconnect_event ||
		       READ_ONCE(wifi->stopping) || readl(wifi->a.bar + 8) != 2;
	if (disconnected)
		wifi->connected = false;
	if (disconnected && was_connected) {
		netif_stop_queue(wifi->netdev);
		skb_queue_purge(&wifi->data_tx_queue);
		netif_carrier_off(wifi->netdev);
		cfg80211_disconnected(wifi->netdev, WLAN_REASON_UNSPECIFIED,
				      NULL, 0, false, GFP_KERNEL);
	}
	mutex_unlock(&wifi->lock);
}

static int wait_disconnect(struct centauri_wifi *wifi)
{
	unsigned long deadline = jiffies + msecs_to_jiffies(5000);

	while (wifi->disconnect_pending) {
		drain(wifi);
		if (READ_ONCE(wifi->stopping) || readl(wifi->a.bar + 8) != 2)
			return -EIO;
		if (!wifi->disconnect_pending)
			break;
		if (time_after_eq(jiffies, deadline))
			return -ETIMEDOUT;
		usleep_range(500, 1000);
	}
	return 0;
}

static int wifi_disconnect(struct wiphy *wiphy, struct net_device *netdev,
			   u16 reason)
{
	struct centauri_wifi *wifi =
		*(struct centauri_wifi **)wiphy_priv(wiphy);
	u8 payload[16] = {};
	bool connected;
	u16 reply_status;
	int ret;

	WRITE_ONCE(wifi->connect_abort, true);
	cancel_scan(wifi);
	cancel_work_sync(&wifi->connect_work);
	mutex_lock(&wifi->lock);
	cancel_connect_locked(wifi);
	put_unaligned_le16(reason, payload);
	put_unaligned_le16(328, payload + 8);
	put_unaligned_le16(4, payload + 10);

	put_unaligned_le32(10, payload + 12);
	wifi->disconnect_pending = !READ_ONCE(wifi->stopping);
	ret = READ_ONCE(wifi->stopping) ?
		      0 :
		      command(wifi, 131073, payload, sizeof(payload));
	reply_status = wifi->reply_status;
	if (!ret && wifi->disconnect_pending && (wifi->reply_command & BIT(30)))
		ret = wait_disconnect(wifi);
	else if (ret != -EIO && ret != -ETIMEDOUT)
		wifi->disconnect_pending = false;

	connected = wifi->connected;
	wifi->connected = false;
	netif_stop_queue(netdev);
	skb_queue_purge(&wifi->data_tx_queue);
	netif_carrier_off(netdev);
	if (connected)
		cfg80211_disconnected(netdev, reason, NULL, 0, true,
				      GFP_KERNEL);
	mutex_unlock(&wifi->lock);
	cancel_work_sync(&wifi->link_work);
	return ret == -EREMOTEIO && reply_status == 326 ? 0 : ret;
}

static int wifi_power_mgmt(struct wiphy *wiphy, struct net_device *netdev,
			   bool enabled, int timeout)
{
	struct centauri_wifi *wifi =
		*(struct centauri_wifi **)wiphy_priv(wiphy);
	u8 mode = enabled ? 0 : 3;
	int ret;

	mutex_lock(&wifi->lock);
	ret = wifi->configured ? command(wifi, 7, &mode, sizeof(mode)) :
				 -ENETDOWN;
	mutex_unlock(&wifi->lock);
	return ret;
}

static void wifi_regulatory(struct wiphy *wiphy,
			    struct regulatory_request *request)
{
	struct centauri_wifi *wifi =
		*(struct centauri_wifi **)wiphy_priv(wiphy);
	u8 country[3] = { request->alpha2[0], request->alpha2[1], 1 };
	int ret;

	if (country[0] < 'A' || country[0] > 'Z' || country[1] < 'A' ||
	    country[1] > 'Z')
		return;
	mutex_lock(&wifi->lock);
	if (wifi->configured) {
		ret = command(wifi, 9, country, sizeof(country));
		if (ret)
			dev_err(&wifi->control->pdev->dev,
				"firmware country update failed: %d\n", ret);
	}
	mutex_unlock(&wifi->lock);
}

static void fill_station(struct centauri_wifi *wifi, struct station_info *sinfo)
{
	const struct net_device_stats *stats = &wifi->netdev->stats;

	if (time_after_eq(jiffies, wifi->station_signal_at + HZ)) {
		wifi->station_signal_valid = false;
		wifi->station_signal_at = jiffies;
		query_link_status(wifi);
	}
	sinfo->filled |= BIT_ULL(NL80211_STA_INFO_CONNECTED_TIME) |
			 BIT_ULL(NL80211_STA_INFO_RX_BYTES64) |
			 BIT_ULL(NL80211_STA_INFO_TX_BYTES64) |
			 BIT_ULL(NL80211_STA_INFO_RX_PACKETS) |
			 BIT_ULL(NL80211_STA_INFO_TX_PACKETS);
	sinfo->connected_time = (jiffies - wifi->connected_at) / HZ;
	sinfo->rx_bytes = stats->rx_bytes - wifi->station_start.rx_bytes;
	sinfo->tx_bytes = stats->tx_bytes - wifi->station_start.tx_bytes;
	sinfo->rx_packets = stats->rx_packets - wifi->station_start.rx_packets;
	sinfo->tx_packets = stats->tx_packets - wifi->station_start.tx_packets;
	if (wifi->station_signal_valid) {
		sinfo->filled |= BIT_ULL(NL80211_STA_INFO_SIGNAL);
		sinfo->signal = wifi->station_signal;
	}
}

static int wifi_get_station(struct wiphy *wiphy, struct wireless_dev *wdev,
			    const u8 *mac, struct station_info *sinfo)
{
	struct centauri_wifi *wifi =
		*(struct centauri_wifi **)wiphy_priv(wiphy);
	int ret = 0;

	mutex_lock(&wifi->lock);
	if (wdev != &wifi->wdev || !wifi->connected ||
	    READ_ONCE(wifi->stopping) || !ether_addr_equal(mac, wifi->bssid))
		ret = -ENOENT;
	else
		fill_station(wifi, sinfo);
	mutex_unlock(&wifi->lock);
	return ret;
}

static int wifi_dump_station(struct wiphy *wiphy, struct wireless_dev *wdev,
			     int index, u8 *mac, struct station_info *sinfo)
{
	struct centauri_wifi *wifi =
		*(struct centauri_wifi **)wiphy_priv(wiphy);
	int ret = 0;

	mutex_lock(&wifi->lock);
	if (index || wdev != &wifi->wdev || !wifi->connected ||
	    READ_ONCE(wifi->stopping))
		ret = -ENOENT;
	else {
		ether_addr_copy(mac, wifi->bssid);
		fill_station(wifi, sinfo);
	}
	mutex_unlock(&wifi->lock);
	return ret;
}

static const struct cfg80211_ops wifi_ops = {
	.scan = wifi_scan,
	.abort_scan = wifi_abort_scan,
	.connect = wifi_connect,
	.disconnect = wifi_disconnect,
	.set_power_mgmt = wifi_power_mgmt,
	.get_station = wifi_get_station,
	.dump_station = wifi_dump_station,
};

static int data_start(struct centauri_wifi *wifi);

static int net_open(struct net_device *netdev)
{
	struct centauri_wifi *wifi =
		*(struct centauri_wifi **)netdev_priv(netdev);
	int ret;

	netif_carrier_off(netdev);
	netif_stop_queue(netdev);
	mutex_lock(&wifi->lock);
	ret = wifi->configured ? data_start(wifi) : -ENETDOWN;
	mutex_unlock(&wifi->lock);
	return ret;
}

static int net_stop(struct net_device *netdev)
{
	struct centauri_wifi *wifi =
		*(struct centauri_wifi **)netdev_priv(netdev);

	cancel_scan(wifi);
	netif_stop_queue(netdev);
	skb_queue_purge(&wifi->data_tx_queue);
	return 0;
}

static netdev_tx_t net_transmit(struct sk_buff *skb, struct net_device *netdev)
{
	struct centauri_wifi *wifi =
		*(struct centauri_wifi **)netdev_priv(netdev);

	if (READ_ONCE(wifi->stopping) || !READ_ONCE(wifi->connected) ||
	    skb->len < ETH_HLEN || skb->len > BUFFER_SIZE) {
		netdev->stats.tx_dropped++;
		dev_kfree_skb_any(skb);
		return NETDEV_TX_OK;
	}
	if (skb_queue_len(&wifi->data_tx_queue) >= DATA_TX_QUEUE_LIMIT) {
		netif_stop_queue(netdev);
		return NETDEV_TX_BUSY;
	}
	skb_queue_tail(&wifi->data_tx_queue, skb);
	if (skb_queue_len(&wifi->data_tx_queue) >= DATA_TX_QUEUE_LIMIT)
		netif_stop_queue(netdev);
	schedule_work(&wifi->link_work);
	return NETDEV_TX_OK;
}

static int station_reconfigure(struct centauri_wifi *wifi, const u8 *address)
{
	const u8 stop[5] = {};
	int ret;

	wifi->configured = false;
	ret = command(wifi, 3, stop, sizeof(stop));
	if (!ret)
		ret = station_configure(wifi, address);
	if (!ret)
		wifi->configured = true;
	return ret;
}

static int net_set_mac_address(struct net_device *netdev, void *data)
{
	struct centauri_wifi *wifi =
		*(struct centauri_wifi **)netdev_priv(netdev);
	struct sockaddr *address = data;
	int ret, restore;

	ret = eth_prepare_mac_addr_change(netdev, data);
	if (ret)
		return ret;
	mutex_lock(&wifi->lock);
	if (READ_ONCE(wifi->stopping)) {
		ret = -ENODEV;
		goto out;
	}
	if (!wifi->configured) {
		ret = -ENETDOWN;
		goto out;
	}
	if (wifi->connecting || wifi->connected || wifi->scan_request ||
	    wifi->disconnect_pending) {
		ret = -EBUSY;
		goto out;
	}
	if (ether_addr_equal(netdev->dev_addr, address->sa_data))
		goto out;

	ret = station_reconfigure(wifi, (const u8 *)address->sa_data);
	if (!ret) {
		eth_commit_mac_addr_change(netdev, data);
	} else {
		restore = station_reconfigure(wifi, netdev->dev_addr);
		if (restore)
			dev_err(&wifi->control->pdev->dev,
				"Wi-Fi MAC restore failed: %d (change: %d)\n",
				restore, ret);
	}
out:
	mutex_unlock(&wifi->lock);
	return ret;
}

static const struct net_device_ops net_ops = {
	.ndo_open = net_open,
	.ndo_stop = net_stop,
	.ndo_start_xmit = net_transmit,
	.ndo_validate_addr = eth_validate_addr,
	.ndo_set_mac_address = net_set_mac_address,
};

static int data_ring_open(struct centauri_wifi *wifi, unsigned int id)
{
	u8 message[0x34 + 10] = { 0 };
	unsigned int vector, offset, size, bit, doorbell, irq;
	bool transfer = false;
	int ret;

	lockdep_assert_held(&wifi->lock);
	if (wifi->data_failed)
		return -EIO;
	if (id == 76) {
		vector = 192;
		offset = 0x200000;
		size = 2048;
		bit = 0;
		doorbell = 18;
		irq = 3;
	} else if (id == 14) {
		vector = 206;
		offset = 0x220000;
		size = 256;
		bit = 1;
		doorbell = 11;
		irq = 8;
	} else if (id == 15) {
		vector = 207;
		offset = 0x240000;
		size = DATA_RX_SLOTS;
		bit = 2;
		doorbell = 11;
		irq = 8;
	} else if (id == 24 || id == 25 || id == 12) {
		transfer = true;
		vector = id;
		offset = id == 12 ? 0x520000 : id == 24 ? 0x500000 : 0x540000;
		size = id == 12 ? DATA_RX_SLOTS : DATA_TX_SLOTS;
		bit = id == 12 ? 4 : id == 24 ? 3 : 5;
		doorbell = id == 12 ? 11 : 18;
		irq = id == 12 ? 8 : 3;
	} else {
		return -EINVAL;
	}
	if (readl(wifi->a.bar + 8) != 2) {
		ret = -EIO;
		goto out;
	}
	if (wifi->data_rings_open & BIT(bit))
		return 0;
	if (wifi->connecting || wifi->scan_request) {
		ret = -EBUSY;
		goto out;
	}
	memset(wifi->a.window + offset, 0, size * 32);
	index_write(wifi, transfer ? 1 : 0, vector, 0);
	index_write(wifi, transfer ? 3 : 2, vector, 0);
	message[0] = transfer ? 1 : 2;
	message[2] = transfer && id == 12 ? 0 : 4;
	put_unaligned_le16(id, message + 4);
	put_unaligned_le16(vector, message + 6);
	put_unaligned_le64(wifi->a.window_dma + offset, message + 8);
	if (transfer) {
		put_unaligned_le64(~0ULL, message + 0x10);
		put_unaligned_le16(size, message + 0x18);
		put_unaligned_le16(id == 12 ? 12 : 76, message + 0x1a);
		put_unaligned_le16(doorbell, message + 0x1c);
		put_unaligned_le16(id == 12 ? 0x220 : 0, message + 0x1e);
		put_unaligned_le32(16, message + 0x20);
		put_unaligned_le16(irq, message + 0x24);
	} else {
		put_unaligned_le16(size, message + 0x10);
		put_unaligned_le16(id == 15 ? 12 : 0xffff, message + 0x12);
		put_unaligned_le16(doorbell, message + 0x14);
		put_unaligned_le32(1, message + 0x18);
		put_unaligned_le16(irq, message + 0x1c);
	}

	ret = alpha_message(wifi, message, (transfer ? 0x34 : 0x2c) + 10);
	if (!ret && readl(wifi->a.bar + 8) != 2)
		ret = -EIO;
	if (ret) {
		wifi->data_failed = true;
		goto out;
	}
	wifi->data_rings_open |= BIT(bit);
	if (transfer && id == 12) {
		unsigned int slot;

		for (slot = 0; slot < size - 1; slot++) {
			u8 *descriptor = wifi->a.window + offset + slot * 16;
			u32 buffer = DATA_RX_BUFFER + slot * 4096;

			memset(wifi->a.window + buffer, 0, 4096);
			put_unaligned_le32(1 | (4096 << 8), descriptor);
			put_unaligned_le64(wifi->a.window_dma + buffer,
					   descriptor + 4);
			put_unaligned_le32(slot + 1, descriptor + 12);
		}
		dma_wmb();
		index_write(wifi, 3, vector, size - 1);
		dma_wmb();
		writel(1, wifi->a.bar + 0x1050);
	}
out:
	return ret;
}

static int data_start(struct centauri_wifi *wifi)
{
	static const unsigned int rings[] = { 76, 15, 12, 25 };
	int ret;

	for (unsigned int i = 0; i < ARRAY_SIZE(rings); i++) {
		ret = data_ring_open(wifi, rings[i]);
		if (ret)
			return ret;
	}
	return 0;
}

static int query_link_status(struct centauri_wifi *wifi)
{
	return wifi->configured ? command(wifi, 20, NULL, 0) : -ENETDOWN;
}

int centauri_wifi_register(struct centauri *c)
{
	struct centauri_wifi *wifi;
	u8 address[ETH_ALEN];
	unsigned int i;
	int ret;

	wifi = kzalloc_obj(*wifi);
	if (!wifi)
		return -ENOMEM;
	wifi->control = c;
	mutex_init(&wifi->lock);
	skb_queue_head_init(&wifi->data_tx_queue);
	INIT_WORK(&wifi->scan_work, scan_worker);
	INIT_WORK(&wifi->connect_work, connect_worker);
	INIT_WORK(&wifi->link_work, link_worker);
	smp_store_release(&c->wifi, wifi);
	ret = centauri_alpha_resources(c, &wifi->a);
	if (ret)
		goto failed;
	wifi->wiphy = wiphy_new(&wifi_ops, sizeof(struct centauri_wifi *));
	wifi->netdev = alloc_etherdev(sizeof(struct centauri_wifi *));
	if (!wifi->wiphy || !wifi->netdev) {
		ret = -ENOMEM;
		goto failed;
	}
	*(struct centauri_wifi **)wiphy_priv(wifi->wiphy) = wifi;
	*(struct centauri_wifi **)netdev_priv(wifi->netdev) = wifi;
	set_wiphy_dev(wifi->wiphy, &c->pdev->dev);
	wifi->wiphy->interface_modes = BIT(NL80211_IFTYPE_STATION);
	wifi->wiphy->max_scan_ssids = 1;
	wifi->wiphy->signal_type = CFG80211_SIGNAL_TYPE_MBM;
	wifi->wiphy->reg_notifier = wifi_regulatory;
	wifi->wiphy->cipher_suites = cipher_suites;
	wifi->wiphy->n_cipher_suites = ARRAY_SIZE(cipher_suites);
	wifi->wiphy->akm_suites = akm_suites;
	wifi->wiphy->n_akm_suites = ARRAY_SIZE(akm_suites);
	wiphy_ext_feature_set(wifi->wiphy,
			      NL80211_EXT_FEATURE_4WAY_HANDSHAKE_STA_PSK);
	for (i = 0; i < 13; i++) {
		wifi->channels[i].band = NL80211_BAND_2GHZ;
		wifi->channels[i].hw_value = i + 1;
		wifi->channels[i].center_freq = ieee80211_channel_to_frequency(
			i + 1, NL80211_BAND_2GHZ);
		wifi->channels[i].max_power = 20;
	}
	for (i = 0; i < ARRAY_SIZE(five_ghz); i++) {
		wifi->channels[13 + i].band = NL80211_BAND_5GHZ;
		wifi->channels[13 + i].hw_value = five_ghz[i];
		wifi->channels[13 + i].center_freq =
			ieee80211_channel_to_frequency(five_ghz[i],
						       NL80211_BAND_5GHZ);
		wifi->channels[13 + i].max_power = 20;
	}
	wifi->bands[0].channels = wifi->channels;
	wifi->bands[0].n_channels = 13;
	wifi->bands[0].bitrates = rates;
	wifi->bands[0].n_bitrates = ARRAY_SIZE(rates);
	wifi->bands[1].channels = wifi->channels + 13;
	wifi->bands[1].n_channels = ARRAY_SIZE(five_ghz);
	wifi->bands[1].bitrates = rates + 4;
	wifi->bands[1].n_bitrates = ARRAY_SIZE(rates) - 4;
	wifi->wiphy->bands[NL80211_BAND_2GHZ] = &wifi->bands[0];
	wifi->wiphy->bands[NL80211_BAND_5GHZ] = &wifi->bands[1];

	ret = eth_platform_get_mac_address(&wifi->a.pdev->dev, address);
	if (!ret) {
		eth_hw_addr_set(wifi->netdev, address);
		ether_addr_copy(wifi->wiphy->perm_addr, address);
	} else if (ret == -EPROBE_DEFER) {
		goto failed;
	} else {
		eth_hw_addr_random(wifi->netdev);
		dev_warn(
			&wifi->a.pdev->dev,
			"No valid factory MAC property; using a random address\n");
	}
	ret = wiphy_register(wifi->wiphy);
	if (ret)
		goto failed;
	wifi->registered = true;
	wifi->wdev.wiphy = wifi->wiphy;
	wifi->wdev.iftype = NL80211_IFTYPE_STATION;
	wifi->wdev.netdev = wifi->netdev;
	wifi->netdev->ieee80211_ptr = &wifi->wdev;
	wifi->netdev->netdev_ops = &net_ops;
	SET_NETDEV_DEV(wifi->netdev, &c->pdev->dev);
	strscpy(wifi->netdev->name, "wlan%d", IFNAMSIZ);
	wifi->netdev->max_mtu = ETH_DATA_LEN;

	ret = radio_start(wifi);
	if (ret) {
		dev_err(&c->pdev->dev,
			"Wi-Fi ring/interface setup failed: %d\n", ret);
		goto failed;
	}
	ret = register_netdev(wifi->netdev);
	if (ret)
		goto failed;
	smp_store_release(&wifi->irq_ready, true);
	schedule_work(&wifi->link_work);

	return 0;
failed:
	centauri_wifi_unregister(c);
	return ret;
}

void centauri_wifi_unregister(struct centauri *c)
{
	struct centauri_wifi *wifi = c->wifi;

	if (!wifi)
		return;
	WRITE_ONCE(wifi->stopping, true);
	WRITE_ONCE(wifi->connect_abort, true);
	WRITE_ONCE(wifi->scan_abort, true);
	smp_store_release(&c->wifi, NULL);

	for (unsigned int i = 0; i < c->alpha_irqs.count; i++)
		synchronize_irq(pci_irq_vector(c->alpha_irqs.pdev, i));
	if (wifi->netdev && wifi->netdev->reg_state == NETREG_REGISTERED)
		netif_tx_disable(wifi->netdev);

	cancel_scan(wifi);
	cancel_work_sync(&wifi->connect_work);
	cancel_work_sync(&wifi->link_work);
	mutex_lock(&wifi->lock);
	cancel_connect_locked(wifi);
	mutex_unlock(&wifi->lock);
	skb_queue_purge(&wifi->data_tx_queue);
	if (wifi->netdev && wifi->netdev->reg_state == NETREG_REGISTERED)
		unregister_netdev(wifi->netdev);
	if (wifi->registered)
		wiphy_unregister(wifi->wiphy);
	if (wifi->wiphy)
		wiphy_free(wifi->wiphy);
	if (wifi->netdev)
		free_netdev(wifi->netdev);
	memzero_explicit(wifi, sizeof(*wifi));
	kfree(wifi);
}
