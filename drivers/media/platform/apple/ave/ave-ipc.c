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
	/* All four words must land before the doorbell. */
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

int ave_ipc_parse_channels(struct ave_dev *ave)
{
	bool found[2] = { false, false };

	for (unsigned int i = 0; i < ave->layout->abi->transport->descriptors;
	     i++) {
		u8 *desc = ave->ipc.cpu + AVE_IPC_CHANNEL_OFF +
			   i * AVE_CHAN_DESC_STRIDE;
		u32 dir = get_unaligned_le32(desc + AVE_CHAN_DESC_DIR);
		u32 doorbell = get_unaligned_le32(desc + AVE_CHAN_DESC_IRQ);
		u32 slots = get_unaligned_le32(desc + AVE_CHAN_DESC_SLOTS);
		u64 addr = get_unaligned_le64(desc + AVE_CHAN_DESC_ADDR);
		struct ave_ring *r;
		unsigned int id;
		u32 offset;

		const struct ave_fw_transport *t = ave->layout->abi->transport;

		if (!memchr(desc, 0, AVE_CHAN_DESC_NAME_LEN) ||
		    slots > ave->channel_bytes / AVE_RING_SLOT_BYTES ||
		    !IS_ALIGNED(addr, AVE_RING_SLOT_BYTES) ||
		    ave_fw_offset(ave, addr,
				  (size_t)slots * AVE_RING_SLOT_BYTES,
				  &offset) ||
		    offset < AVE_IPC_CHANNEL_OFF +
				     t->descriptors * AVE_CHAN_DESC_STRIDE ||
		    offset + slots * AVE_RING_SLOT_BYTES >
			    AVE_IPC_CHANNEL_OFF + ave->channel_bytes)
			return -EPROTO;
		if (!strcmp(desc, "IO"))
			id = 0;
		else if (!strcmp(desc, "IO_T2H"))
			id = 1;
		else if (t->descriptors > 2)
			continue;
		else
			return -EPROTO;
		if (found[id] || dir != id || !doorbell || doorbell >= 32 ||
		    !slots ||
		    (t->slots[id] &&
		     (slots != t->slots[id] || doorbell != t->doorbells[id] ||
		      offset != AVE_IPC_CHANNEL_OFF + t->offsets[id] ||
		      i != (id ? t->t2h_index : t->io_index))))
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
	if (ave->ring[0].offset <
		    ave->ring[1].offset +
			    ave->ring[1].slots * AVE_RING_SLOT_BYTES &&
	    ave->ring[1].offset <
		    ave->ring[0].offset +
			    ave->ring[0].slots * AVE_RING_SLOT_BYTES)
		return -EPROTO;

	/* Hand every host-to-firmware slot to the host; ownership is bit 0. */
	for (unsigned int i = 0; i < ave->ring[0].slots; i++) {
		__le64 *slot = ave->ipc.cpu + ave->ring[0].offset +
			       i * AVE_RING_SLOT_BYTES;

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
int ave_ipc_check_hello(const struct ave_fw_transport *t, const u32 hello[4])
{
	if (hello[0] != t->hello[0] || hello[2] != t->hello[2] ||
	    hello[1] < t->descriptors * AVE_CHAN_DESC_STRIDE ||
	    hello[1] > AVE_IPC_HANDSHAKE_OFF - AVE_IPC_CHANNEL_OFF)
		return -EPROTO;
	if (t->boot_config)
		return !hello[3] || hello[3] > 4 * SZ_1M ? -EPROTO : 0;
	return hello[1] != t->hello[1] || hello[3] != t->hello[3] ? -EPROTO : 0;
}

static int ave_iop_time_us(struct ave_dev *ave, u64 *us)
{
	u64 ticks = readq(ave->coproc + ave->variant->regs->timebase);
	u64 freq = readl(ave->coproc + ave->variant->regs->timebase_freq);

	freq *= ave->variant->timebase_multiplier;
	if (freq && freq < 1000000)
		return -EPROTO;
	*us = div64_u64(ticks, freq ? div_u64(freq, 1000000) : 24);
	return 0;
}

int ave_ipc_negotiate(struct ave_dev *ave, const u32 hello[4])
{
	const struct ave_fw_transport *t = ave->layout->abi->transport;
	u8 *hs = ave->ipc.cpu + AVE_IPC_HANDSHAKE_OFF;
	u64 ticks, offset_us;
	u32 status, reply[4], off;
	int ret;

	ret = ave_ipc_check_hello(t, hello);
	if (ret) {
		dev_err(ave->dev, "unexpected hello {%#x, %#x, %#x, %#x}\n",
			hello[0], hello[1], hello[2], hello[3]);
		return ret;
	}
	if (t->scratch_halt)
		dev_dbg(ave->dev, "hello {%#x, %#x, %#x, %#x}\n", hello[0],
			hello[1], hello[2], hello[3]);
	ave->channel_bytes = hello[1];

	if (t->boot_config) {
		if (ave->heap.cpu && ave->heap.size != PAGE_ALIGN(hello[3]))
			return -EPROTO;
		if (!ave->heap.cpu) {
			ret = ave_alloc(ave, &ave->heap, PAGE_ALIGN(hello[3]),
					0);
			if (ret)
				return ret;
		}
		memset(ave->heap.cpu, 0, ave->heap.size);
	}
	writel(AVE_MBOX_STATUS_PENDING, ave->mbox + AVE_MBOX_STATUS);
	if (t->sync_host_clock) {
		ret = ave_iop_time_us(ave, &ticks);
		if (ret)
			return ret;
		offset_us = div_u64(ktime_get_ns(), NSEC_PER_USEC) - ticks;
		writel(lower_32_bits(offset_us), ave->mbox + AVE_MBOX_TIME_LO);
		writel(upper_32_bits(offset_us), ave->mbox + AVE_MBOX_TIME_HI);
	}

	ave_mbox_send(ave, lower_32_bits(ave->ipc.dma),
		      upper_32_bits(ave->ipc.dma), ave->ipc.size, 0);
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
	put_unaligned_le64(ave->ipc_fw_base + AVE_IPC_CHANNEL_OFF,
			   hs + AVE_HS_CHANNELS);
	if (t->boot_config) {
		put_unaligned_le64(ave->ipc_fw_base + AVE_IPC_SHARED_OFF,
				   hs + AVE_HS_SHARED);
		put_unaligned_le32(AVE_IPC_SHARED_BYTES,
				   hs + AVE_HS_SHARED_BYTES);
		put_unaligned_le64(ave->heap.dma, hs + AVE_HS_HEAP);
		put_unaligned_le32(hello[3], hs + AVE_HS_HEAP_BYTES);
	}
	put_unaligned_le32(ave->variant->device_type, hs + AVE_HS_DEVICE_TYPE);
	dma_wmb();

	ave_mbox_send(ave,
		      lower_32_bits(ave->ipc_fw_base + AVE_IPC_HANDSHAKE_OFF),
		      upper_32_bits(ave->ipc_fw_base + AVE_IPC_HANDSHAKE_OFF),
		      0, 0);
	ret = ave_mbox_recv(ave, reply);
	if (ret)
		return ret;
	if (ave_fw_offset(ave, (u64)reply[1] << 32 | reply[0],
			  ave->channel_bytes, &off) ||
	    off != AVE_IPC_CHANNEL_OFF ||
	    (t->boot_config && reply[2] > AVE_HELLO_EXTRA_MAX))
		return -EPROTO;

	ret = ave_ipc_parse_channels(ave);
	if (ret)
		return ret;

	writel(AVE_BOOT_MAGIC, ave->mbox + AVE_MBOX_READY);
	return readl_poll_timeout(ave->mbox + AVE_MBOX_READY, status, !status,
				  50, AVE_MBOX_TIMEOUT_US);
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

static int ave_ipc_buf_offset(struct ave_dev *ave, unsigned int buf,
			      size_t *max)
{
	const struct ave_fw_transport *t = ave->layout->abi->transport;

	if (buf == AVE_BUF_CMD) {
		*max = t->command_bytes;
		return t->command_offset;
	}
	if (buf < AVE_BUF_PROC(0)) {
		*max = AVE_IPC_SCALAR_BYTES;
		return t->scalar_offset +
		       (buf - AVE_BUF_SCALAR(0)) * AVE_IPC_SCALAR_BYTES;
	}
	*max = t->process_bytes;
	return t->process_offset + (buf - AVE_BUF_PROC(0)) * t->process_bytes;
}

/* Which command buffer does a transport ack refer to? */
static u32 ave_ipc_command_end(struct ave_dev *ave)
{
	const struct ave_fw_transport *t = ave->layout->abi->transport;

	return t->process_offset + AVE_IPC_PROC_BUFS * t->process_bytes;
}

static int ave_ipc_buf_of(struct ave_dev *ave, u64 addr)
{
	u64 off = addr - ave->ipc_fw_base;
	size_t max;

	if (addr < ave->ipc_fw_base ||
	    off < ave->layout->abi->transport->command_offset ||
	    off >= ave_ipc_command_end(ave))
		return -EPROTO;
	for (unsigned int buf = 0; buf < AVE_BUF_COUNT; buf++)
		if (ave_ipc_buf_offset(ave, buf, &max) == off)
			return buf;
	return -EPROTO;
}

/*
 * Get command buffer @buf, cleared for @len bytes, or NULL while the
 * firmware still owns it. Caller holds hw_lock.
 */
void *ave_ipc_cmd(struct ave_dev *ave, unsigned int buf, size_t len)
{
	size_t max;
	int off;

	if (buf >= AVE_BUF_COUNT || (ave->unacked & BIT(buf)))
		return NULL;
	off = ave_ipc_buf_offset(ave, buf, &max);
	if (len > max || off > ave->ipc.size || len > ave->ipc.size - off)
		return NULL;
	memset(ave->ipc.cpu + off, 0, len);
	return ave->ipc.cpu + off;
}

static bool ave_ipc_acked(struct ave_dev *ave, void *arg)
{
	return !(ave->unacked & BIT(*(unsigned int *)arg));
}

/* Wait until the firmware has taken command buffer @buf. Caller holds hw_lock. */
int ave_ipc_wait_ack(struct ave_dev *ave, unsigned int buf)
{
	return ave_ipc_pump(ave, AVE_CMD_WAIT_MS, ave_ipc_acked, &buf);
}

static bool ave_ipc_scalar_free(struct ave_dev *ave, void *arg)
{
	return (ave->unacked & GENMASK(AVE_BUF_PROC(0) - 1, AVE_BUF_SCALAR(0))) !=
		GENMASK(AVE_BUF_PROC(0) - 1, AVE_BUF_SCALAR(0));
}

/* Pick a free buffer for a scalar command. Caller holds hw_lock. */
int ave_ipc_scalar_buf(struct ave_dev *ave)
{
	int ret;

	ret = ave_ipc_pump(ave, AVE_CMD_WAIT_MS, ave_ipc_scalar_free, NULL);
	if (ret)
		return ret;
	for (unsigned int i = 0; i < AVE_IPC_SCALAR_BUFS; i++) {
		unsigned int buf = AVE_BUF_SCALAR((ave->scalar_next + i) % AVE_IPC_SCALAR_BUFS);

		if (!(ave->unacked & BIT(buf))) {
			ave->scalar_next = (buf - AVE_BUF_SCALAR(0) + 1) % AVE_IPC_SCALAR_BUFS;
			return buf;
		}
	}
	return -EBUSY;
}

/*
 * Hand the command in buffer @buf to the firmware. The timeout words are
 * {duration, creation}, using the host clock for 9003 and the IOP clock
 * for 6070.
 * With @reply, the firmware's answer is counted as expected; Halt and
 * Process are answered differently. Caller holds hw_lock.
 */
int ave_ipc_submit(struct ave_dev *ave, unsigned int buf, size_t len,
		   u64 timeout_us, bool reply)
{
	size_t max;
	int off;
	u8 *cmd;
	const struct ave_fw_abi *abi = ave->layout->abi;
	u32 wire_id;
	enum ave_opcode op = 0;
	u64 created;
	int ret;

	if (buf >= AVE_BUF_COUNT)
		return -EINVAL;
	off = ave_ipc_buf_offset(ave, buf, &max);
	if (len > max || off > ave->ipc.size || len > ave->ipc.size - off)
		return -EINVAL;
	cmd = ave->ipc.cpu + off;
	wire_id = get_unaligned_le16(cmd);
	for (unsigned int i = 1; i < AVE_OP_COUNT; i++)
		if (wire_id && (abi->wire_id[0][i] == wire_id ||
				abi->wire_id[1][i] == wire_id)) {
			op = i;
			break;
		}
	lockdep_assert_held(&ave->hw_lock);
	if (ave->fw_state != AVE_FW_RUNNING) {
		dev_err_ratelimited(ave->dev,
				    "command %u not sent: firmware state %d\n",
				    op, ave->fw_state);
		return -EIO;
	}
	if (!op || len < abi->header_bytes || len > max ||
	    !IS_ALIGNED(len, 4) || (ave->unacked & BIT(buf)) ||
	    (reply && op >= ARRAY_SIZE(ave->want)))
		return -EINVAL;

	if (!abi->transport->boot_config && reply && ave->want[op])
		return -EBUSY;
	if (abi->transport->sync_host_clock) {
		created = div_u64(ktime_get_ns(), NSEC_PER_USEC);
	} else {
		ret = ave_iop_time_us(ave, &created);
		if (ret)
			return ret;
	}
	ave_stamp_command(abi, cmd, timeout_us, created);
	if (!abi->transport->boot_config && reply) {
		ave->expected[op].client = get_unaligned_le32(cmd + 0x10);
		ave->expected[op].subtype = get_unaligned_le32(cmd + 0x1c);
	}

	ret = ave_ring_send(ave, &ave->ring[0], ave->ipc_fw_base + off, len);
	if (ret) {
		ave_fw_mark_broken(ave, ret, "command ring full");
		return ret;
	}
	ave->unacked |= BIT(buf);
	if (reply)
		ave->want[op]++;
	return 0;
}

static int ave_ipc_dispatch(struct ave_dev *ave)
{
	const struct ave_fw_abi *abi = ave->layout->abi;
	struct ave_reply reply;
	int ret = abi->parse_reply(abi, ave->msg, ave->msg_len, &reply);

	if (ret) {
		if (!abi->transport->boot_config)
			return ret;
		dev_warn(ave->dev, "short firmware message (%u bytes)\n",
			 ave->msg_len);
		ave->reply_err = ret;
		return 0;
	}
	if (reply.op == AVE_OP_PROCESS) {
		if (!ave->layout->encode_ready)
			return -EPROTO;
		ave_sess_reply(ave, &reply);
		return 0;
	}
	if (reply.op && ave->want[reply.op]) {
		if (!abi->transport->boot_config &&
		    (reply.client != ave->expected[reply.op].client ||
		     reply.subtype != ave->expected[reply.op].subtype)) {
			dev_err(ave->dev,
				"reply %#x has wrong route: client %llu subtype %#x\n",
				abi->reply_id[reply.op], reply.client,
				reply.subtype);
			return -EPROTO;
		}
		if (abi->transport->scratch_halt)
			dev_dbg(ave->dev,
				"reply %#x client %llu subtype %#x status %#x\n",
				abi->reply_id[reply.op], reply.client,
				reply.subtype, reply.raw_status);
		ave->want[reply.op]--;
		if (reply.status) {
			dev_err(ave->dev, "command %u refused: status %#x\n",
				reply.op, reply.raw_status);
			ave->reply_err = -EIO;
		}
		return 0;
	}
	dev_warn_ratelimited(ave->dev,
			     "unexpected firmware message (op %u status %#x)\n",
			     reply.op, reply.raw_status);
	return abi->transport->boot_config ? 0 : -EPROTO;
}

/*
 * Poll both rings once. Returns 1 when an event (ack or message) was
 * handled, 0 when nothing was pending.
 */
static int ave_ipc_poll_once(struct ave_dev *ave)
{
	u32 pending, len, off;
	u64 addr;
	int ret, buf;

	pending = readl(ave->mbox + AVE_MBOX_STATUS);
	if (pending)
		writel(pending, ave->mbox + AVE_MBOX_STATUS);

	ret = ave_ring_recv(ave, &ave->ring[0], &addr, &len);
	if (!ret) {
		/* Transport acks return the command address with no payload. */
		buf = ave_ipc_buf_of(ave, addr);
		if (len || buf < 0 || !(ave->unacked & BIT(buf))) {
			dev_err(ave->dev, "bad command ack %#llx/%u\n", addr,
				len);
			return -EPROTO;
		}
		ave->unacked &= ~BIT(buf);
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
	/* Messages must not land in the buffers the host writes commands to. */
	if (off < ave_ipc_command_end(ave) &&
	    off + len > ave->layout->abi->transport->command_offset) {
		dev_err(ave->dev,
			"firmware message at %#x overlaps the command area\n",
			off);
		return -EPROTO;
	}
	memcpy(ave->msg, ave->ipc.cpu + off, len);
	ave->msg_len = len;
	/* Return the message buffer to the firmware. */
	ret = ave_ring_send(ave, &ave->ring[1], addr, len);
	if (ret)
		return ret;
	ret = ave_ipc_dispatch(ave);
	return ret ?: 1;
}

/*
 * Handle acks and messages until @cond holds, for at most @wait_ms. With no
 * @cond, handle whatever is pending and return. Running out of time means
 * the firmware is stuck: it is marked broken. Caller holds hw_lock.
 *
 * Which interrupt line signals the rings is not known, so this polls. It
 * sleeps between polls: briefly at first (commands are acknowledged within
 * microseconds), then for up to a millisecond, so a long wait for a frame
 * costs next to no CPU time.
 */
#define AVE_POLL_FAST_ROUNDS 20

int ave_ipc_pump(struct ave_dev *ave, unsigned int wait_ms,
		 bool (*cond)(struct ave_dev *ave, void *arg), void *arg)
{
	ktime_t deadline = ktime_add_ms(ktime_get(), wait_ms);
	unsigned int idle = 0;
	int ret;

	lockdep_assert_held(&ave->hw_lock);
	for (;;) {
		if (cond && cond(ave, arg))
			return 0;
		if (cond && ktime_after(ktime_get(), deadline)) {
			ave_fw_mark_broken(ave, -ETIMEDOUT, "no answer");
			return -ETIMEDOUT;
		}
		if (ave->fw_state != AVE_FW_RUNNING) {
			dev_err_ratelimited(ave->dev, "waiting on a firmware in state %d\n",
					    ave->fw_state);
			return -EIO;
		}
		ret = ave_ipc_poll_once(ave);
		if (ret < 0) {
			ave_fw_mark_broken(ave, ret, "ring error");
			return ret;
		}
		if (ret) {
			idle = 0;
			continue;
		}
		if (!cond)
			return 0;
		if (idle++ < AVE_POLL_FAST_ROUNDS)
			usleep_range(50, 100);
		else
			usleep_range(500, 1000);
	}
}

static bool ave_ipc_answered(struct ave_dev *ave, void *arg)
{
	u32 mask = *(u32 *)arg;

	if (ave->unacked & mask)
		return false;
	for (unsigned int i = 0; i < ARRAY_SIZE(ave->want); i++)
		if (ave->want[i] && i != AVE_OP_HALT)
			return false;
	return true;
}

/*
 * Wait until every command except Process has been acknowledged and
 * answered. Returns the first refusal seen since the last call.
 * Caller holds hw_lock.
 */
int ave_ipc_settle(struct ave_dev *ave, unsigned int wait_ms)
{
	u32 mask = GENMASK(AVE_BUF_PROC(0) - 1, 0);
	int ret;

	ret = ave_ipc_pump(ave, wait_ms, ave_ipc_answered, &mask);
	if (!ret)
		ret = ave->reply_err;
	ave->reply_err = 0;
	return ret;
}
