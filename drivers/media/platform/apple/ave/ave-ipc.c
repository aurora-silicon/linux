// SPDX-License-Identifier: GPL-2.0-only
/*
 * Apple AVE video encoder: coprocessor mailbox and command rings
 *
 * The mailbox negotiation and the 64-byte ring protocol here come from
 * Acelogic's J700 AVE transport (the first Linux code to drive this
 * firmware); this file reshapes it into the driver core.
 *
 * Copyright The Aurora Silicon Contributors
 */
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/ktime.h>
#include <linux/math64.h>
#include <linux/sizes.h>
#include <linux/string.h>
#include <linux/unaligned.h>

#include "ave.h"
#include "ave-regs.h"

static int ave_fw_offset(struct ave_dev *ave, u64 addr, size_t len, u32 *offset)
{
	u64 delta = addr - ave->ipc_fw_base;

	if (addr < ave->ipc_fw_base || delta > ave->ipc.size ||
	    len > ave->ipc.size - delta)
		return -ERANGE;
	*offset = delta;
	return 0;
}

static void ave_mbox_send(struct ave_dev *ave, u32 a, u32 b, u32 c, u32 d)
{
	writel(a, ave->mbox + AVE_MBOX_SCRATCH(0));
	writel(b, ave->mbox + AVE_MBOX_SCRATCH(1));
	writel(c, ave->mbox + AVE_MBOX_SCRATCH(2));
	writel(d, ave->mbox + AVE_MBOX_SCRATCH(3));
	wmb();
	writel(1, ave->mbox + AVE_MBOX_DOORBELL);
}

static int ave_mbox_recv(struct ave_dev *ave, u32 reply[4])
{
	u32 status;
	int ret;

	ret = readl_poll_timeout(ave->mbox + AVE_MBOX_STATUS, status,
				 status & AVE_MBOX_STATUS_PENDING, 50,
				 AVE_MBOX_TIMEOUT_US);
	if (ret)
		return ret;

	/* Acknowledge before reading the scratch words. */
	writel(AVE_MBOX_STATUS_PENDING, ave->mbox + AVE_MBOX_STATUS);
	for (unsigned int i = 0; i < 4; i++)
		reply[i] = readl(ave->mbox + AVE_MBOX_SCRATCH(i));
	dma_rmb();
	return 0;
}

static int ave_ring_setup(struct ave_dev *ave)
{
	bool found[2] = { false, false };

	for (unsigned int i = 0; i < 2; i++) {
		u8 *desc = ave->ipc.cpu + AVE_IPC_CHANNEL_OFF + i * AVE_CHAN_DESC_STRIDE;
		u32 dir = get_unaligned_le32(desc + AVE_CHAN_DESC_DIR);
		u32 doorbell = get_unaligned_le32(desc + AVE_CHAN_DESC_IRQ);
		u32 slots = get_unaligned_le32(desc + AVE_CHAN_DESC_SLOTS);
		u64 addr = get_unaligned_le64(desc + AVE_CHAN_DESC_ADDR);
		struct ave_ring *r;
		unsigned int id;
		u32 offset;

		if (!memchr(desc, 0, AVE_CHAN_DESC_NAME_LEN))
			return -EPROTO;
		if (!strcmp(desc, "IO"))
			id = 0;
		else if (!strcmp(desc, "IO_T2H"))
			id = 1;
		else
			return -EPROTO;

		if (found[id] || dir != id || !doorbell || doorbell >= 32 || !slots ||
		    slots > ave->channel_bytes / AVE_RING_SLOT_BYTES ||
		    !IS_ALIGNED(addr, AVE_RING_SLOT_BYTES) ||
		    ave_fw_offset(ave, addr, (size_t)slots * AVE_RING_SLOT_BYTES, &offset) ||
		    offset < AVE_IPC_CHANNEL_OFF + 2 * AVE_CHAN_DESC_STRIDE ||
		    offset + slots * AVE_RING_SLOT_BYTES > AVE_IPC_CHANNEL_OFF + ave->channel_bytes)
			return -EPROTO;

		found[id] = true;
		r = &ave->ring[id];
		strscpy(r->name, desc, sizeof(r->name));
		/* Host-to-firmware rings start empty, firmware-to-host rings full. */
		r->mode = dir ^ 1;
		r->doorbell = doorbell;
		r->slots = slots;
		r->offset = offset;
		r->consumer = r->mode ? -1 : 0;
		r->producer = r->mode ? 0 : -1;
	}

	if (!found[0] || !found[1])
		return -EPROTO;
	if (ave->ring[0].offset < ave->ring[1].offset + ave->ring[1].slots * AVE_RING_SLOT_BYTES &&
	    ave->ring[1].offset < ave->ring[0].offset + ave->ring[0].slots * AVE_RING_SLOT_BYTES)
		return -EPROTO;

	/* Hand every host-to-firmware slot to the host; ownership is bit 0. */
	for (unsigned int i = 0; i < ave->ring[0].slots; i++) {
		__le64 *slot = ave->ipc.cpu + ave->ring[0].offset + i * AVE_RING_SLOT_BYTES;

		slot[0] = cpu_to_le64(1);
		slot[1] = 0;
		slot[2] = 0;
	}
	dma_wmb();
	return 0;
}

/*
 * Finish the coprocessor start-up once the firmware has answered the boot
 * request: hand it the heap and the IPC window, then the channel area, and
 * read back the two ring descriptors it writes there.
 */
int ave_ipc_negotiate(struct ave_dev *ave, const u32 hello[4])
{
	u8 *hs = ave->ipc.cpu + AVE_IPC_HANDSHAKE_OFF;
	u64 ticks, offset_us;
	u32 freq, status, reply[4], off;
	int ret;

	if (hello[0] != AVE_HELLO_VERSION || hello[2] != AVE_HELLO_DESC_BYTES ||
	    hello[1] < 2 * AVE_CHAN_DESC_STRIDE ||
	    hello[1] > AVE_IPC_HANDSHAKE_OFF - AVE_IPC_CHANNEL_OFF ||
	    !hello[3] || hello[3] > 4 * SZ_1M)
		return -EPROTO;
	ave->channel_bytes = hello[1];

	if (ave->heap.cpu && ave->heap.size != PAGE_ALIGN(hello[3]))
		return -EPROTO;
	if (!ave->heap.cpu) {
		ave->heap.size = PAGE_ALIGN(hello[3]);
		ave->heap.cpu = dma_alloc_coherent(ave->dev, ave->heap.size,
						   &ave->heap.dma, GFP_KERNEL);
		if (!ave->heap.cpu)
			return -ENOMEM;
	}
	memset(ave->heap.cpu, 0, ave->heap.size);
	writel(AVE_MBOX_STATUS_PENDING, ave->mbox + AVE_MBOX_STATUS);

	/* Tell the firmware how far its timebase is from the host's. */
	ticks = readq(ave->coproc + AVE_COPROC_TIMEBASE);
	freq = readl(ave->coproc + AVE_COPROC_TIMEBASE_FREQ);
	if (freq && freq < 1000000)
		return -EPROTO;
	ticks = div64_u64(ticks, freq ? freq / 1000000 : 24);
	offset_us = div_u64(ktime_get_ns(), NSEC_PER_USEC) - ticks;
	writel(lower_32_bits(offset_us), ave->mbox + AVE_MBOX_TIME_LO);
	writel(upper_32_bits(offset_us), ave->mbox + AVE_MBOX_TIME_HI);

	ave_mbox_send(ave, lower_32_bits(ave->ipc.dma), upper_32_bits(ave->ipc.dma),
		      ave->ipc.size, 0);
	ret = ave_mbox_recv(ave, reply);
	if (ret)
		return ret;
	ave->ipc_fw_base = (u64)reply[1] << 32 | reply[0];
	if (!ave->ipc_fw_base || !IS_ALIGNED(ave->ipc_fw_base, SZ_16K) ||
	    ave->ipc_fw_base > U64_MAX - ave->ipc.size)
		return -EPROTO;

	memset(ave->ipc.cpu + AVE_IPC_CHANNEL_OFF, 0, ave->channel_bytes);
	memset(hs, 0, AVE_HS_BYTES);
	memset(ave->ipc.cpu + AVE_IPC_SHARED_OFF, 0, AVE_IPC_SHARED_BYTES);
	put_unaligned_le64(ave->ipc_fw_base + AVE_IPC_CHANNEL_OFF, hs + AVE_HS_CHANNELS);
	put_unaligned_le64(ave->ipc_fw_base + AVE_IPC_SHARED_OFF, hs + AVE_HS_SHARED);
	put_unaligned_le32(AVE_IPC_SHARED_BYTES, hs + AVE_HS_SHARED_BYTES);
	put_unaligned_le64(ave->heap.dma, hs + AVE_HS_HEAP);
	put_unaligned_le32(hello[3], hs + AVE_HS_HEAP_BYTES);
	put_unaligned_le32(AVE_HS_MAGIC_VALUE, hs + AVE_HS_MAGIC);
	dma_wmb();

	ave_mbox_send(ave, lower_32_bits(ave->ipc_fw_base + AVE_IPC_HANDSHAKE_OFF),
		      upper_32_bits(ave->ipc_fw_base + AVE_IPC_HANDSHAKE_OFF), 0, 0);
	ret = ave_mbox_recv(ave, reply);
	if (ret)
		return ret;
	if (ave_fw_offset(ave, (u64)reply[1] << 32 | reply[0], ave->channel_bytes, &off) ||
	    off != AVE_IPC_CHANNEL_OFF || reply[2] > AVE_HELLO_EXTRA_MAX)
		return -EPROTO;

	ret = ave_ring_setup(ave);
	if (ret)
		return ret;

	writel(AVE_BOOT_MAGIC, ave->mbox + AVE_MBOX_READY);
	return readl_poll_timeout(ave->mbox + AVE_MBOX_READY, status, !status, 50,
				  AVE_MBOX_TIMEOUT_US);
}

static int ave_ring_send(struct ave_dev *ave, struct ave_ring *r, u64 addr, u32 len)
{
	__le64 *slot;
	int next;

	if (r->producer < 0)
		return -ENOSPC;

	slot = ave->ipc.cpu + r->offset + r->producer * AVE_RING_SLOT_BYTES;
	WRITE_ONCE(slot[1], cpu_to_le64(len));
	WRITE_ONCE(slot[2], 0);
	dma_wmb();
	WRITE_ONCE(slot[0], cpu_to_le64(addr | (r->mode ^ 1)));
	dma_wmb();

	if (r->consumer < 0)
		r->consumer = r->producer;
	next = (r->producer + 1) % r->slots;
	r->producer = next == r->consumer ? -1 : next;
	writel(BIT(r->doorbell), ave->mbox + AVE_MBOX_DOORBELL);
	return 0;
}

static int ave_ring_recv(struct ave_dev *ave, struct ave_ring *r, u64 *addr, u32 *len)
{
	__le64 *slot;
	u64 word;
	int next;

	if (r->consumer < 0)
		return -EAGAIN;

	slot = ave->ipc.cpu + r->offset + r->consumer * AVE_RING_SLOT_BYTES;
	word = le64_to_cpu(READ_ONCE(slot[0]));
	if ((word & 1) != r->mode)
		return -EAGAIN;
	dma_rmb();
	*addr = word & ~3ULL;
	*len = le64_to_cpu(READ_ONCE(slot[1]));

	if (r->producer < 0)
		r->producer = r->consumer;
	next = (r->consumer + 1) % r->slots;
	r->consumer = next == r->producer ? -1 : next;
	return 0;
}

/*
 * Poll both rings once. Returns 1 when the command at @cmd_addr was
 * acknowledged, 2 when a firmware message was copied to ave->msg (and its
 * buffer handed back), 0 when nothing was pending.
 */
static int ave_ipc_poll_once(struct ave_dev *ave, u64 cmd_addr)
{
	u32 pending, len, off;
	u64 addr;
	int ret;

	pending = readl(ave->mbox + AVE_MBOX_STATUS);
	if (pending)
		writel(pending, ave->mbox + AVE_MBOX_STATUS);

	ret = ave_ring_recv(ave, &ave->ring[0], &addr, &len);
	if (!ret) {
		/* Transport acks return the command address with no payload. */
		if (len || addr != cmd_addr)
			return -EPROTO;
		return 1;
	}
	if (ret != -EAGAIN)
		return ret;

	ret = ave_ring_recv(ave, &ave->ring[1], &addr, &len);
	if (ret == -EAGAIN)
		return 0;
	if (ret)
		return ret;
	if (len > sizeof(ave->msg) || ave_fw_offset(ave, addr, len, &off))
		return -EPROTO;
	memcpy(ave->msg, ave->ipc.cpu + off, len);
	ave->msg_len = len;
	/* Return the message buffer to the firmware. */
	ret = ave_ring_send(ave, &ave->ring[1], addr, len);
	return ret ?: 2;
}

/*
 * Send one command and wait for its transport ack and the firmware's reply
 * message. The reply is left in ave->msg. Caller holds hw_lock.
 */
int ave_ipc_call(struct ave_dev *ave, const void *cmd, size_t len)
{
	u64 cmd_addr = ave->ipc_fw_base + AVE_IPC_CMD_OFF;
	u8 *buf = ave->ipc.cpu + AVE_IPC_CMD_OFF;
	bool acked = false, replied = false;
	ktime_t deadline;
	u32 status;
	int ret;

	lockdep_assert_held(&ave->hw_lock);
	if (ave->fw_state != AVE_FW_RUNNING)
		return -EIO;
	if (len < AVE_CMD_HDR_BYTES || len > AVE_IPC_CMD_BYTES || !IS_ALIGNED(len, 4))
		return -EINVAL;

	memcpy(buf, cmd, len);
	/* The timeout is {duration, creation} in the clock synced at boot. */
	put_unaligned_le64(AVE_CMD_TIMEOUT_US, buf + AVE_CMD_TIMEOUT);
	put_unaligned_le64(div_u64(ktime_get_ns(), NSEC_PER_USEC), buf + AVE_CMD_CREATED);
	ave->msg_len = 0;

	ret = ave_ring_send(ave, &ave->ring[0], cmd_addr, len);
	if (ret)
		goto broken;

	deadline = ktime_add_ms(ktime_get(), AVE_CMD_WAIT_MS);
	while (!acked || !replied) {
		ret = ave_ipc_poll_once(ave, cmd_addr);
		if (ret < 0)
			goto broken;
		if (ret == 1)
			acked = true;
		else if (ret == 2)
			replied = true;
		else if (ktime_after(ktime_get(), deadline))
			goto timeout;
		else
			usleep_range(50, 150);
	}

	/* The firmware refused the command: the session fails, the firmware lives on. */
	if (ave->msg_len < AVE_REPLY_MIN_BYTES)
		goto broken_len;
	status = get_unaligned_le32(ave->msg + AVE_REPLY_STATUS);
	if (status) {
		dev_err(ave->dev, "command %u refused: status %#x\n",
			get_unaligned_le32(cmd), status);
		return -EIO;
	}
	return 0;

broken_len:
	ret = -EPROTO;
	goto broken;
timeout:
	ret = -ETIMEDOUT;
broken:
	dev_err(ave->dev, "command %u failed: %d (acked %d, replied %d)\n",
		get_unaligned_le32(cmd), ret, acked, replied);
	ave->fw_state = AVE_FW_BROKEN;
	return ret;
}

/* Wait for one more firmware message (frame completions). */
int ave_ipc_wait_msg(struct ave_dev *ave, unsigned int timeout_ms)
{
	ktime_t deadline = ktime_add_ms(ktime_get(), timeout_ms);
	int ret;

	lockdep_assert_held(&ave->hw_lock);
	if (ave->fw_state != AVE_FW_RUNNING)
		return -EIO;

	for (;;) {
		ret = ave_ipc_poll_once(ave, 0);
		if (ret == 2)
			return 0;
		if (ret < 0)
			break;
		if (ktime_after(ktime_get(), deadline)) {
			ret = -ETIMEDOUT;
			break;
		}
		usleep_range(50, 150);
	}
	dev_err(ave->dev, "no firmware message: %d\n", ret);
	ave->fw_state = AVE_FW_BROKEN;
	return ret;
}
