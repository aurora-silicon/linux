/* SPDX-License-Identifier: GPL-2.0-only */
/* Experimental 25G76/64-bit AVE mailbox and bidirectional ring transport.
 * Included after private driver types. All callers hold ave->lock.
 */
#define AVE_CHANNEL_OFF 0x4000
#define AVE_HANDSHAKE_OFF 0x24000
#define AVE_SHARED_OFF 0x28000
#define AVE_COMMAND_OFF 0x40000
#define AVE_COMMAND_BYTES SZ_128K
#define AVE_ARENA_OFF 0x80000
#define AVE_START_MAGIC 0x08042006

static int ave_fw_offset(struct apple_ave *ave, u64 address, size_t bytes, u32 *offset)
{
	u64 delta = address - ave->ipc_fw_base;

	if (address < ave->ipc_fw_base || delta > ave->ipc.size || bytes > ave->ipc.size - delta)
		return -ERANGE;
	*offset = delta;
	return 0;
}

static void ave_mailbox_send(struct apple_ave *ave, u32 a, u32 b, u32 c, u32 d)
{
	writel(a, ave->mailbox + 0x18);
	writel(b, ave->mailbox + 0x1c);
	writel(c, ave->mailbox + 0x20);
	writel(d, ave->mailbox + 0x24);
	wmb();
	writel(1, ave->mailbox + 0x0c);
}

static int ave_mailbox_receive(struct apple_ave *ave)
{
	u32 status;
	int ret = readl_poll_timeout(ave->mailbox + 0x10, status, status & 1, 50, 5000000);

	if (ret)
		return ret;
	/* Acknowledge the interrupt before reading the four scratch words. */
	writel(1, ave->mailbox + 0x10);
	for (unsigned int i = 0; i < 4; i++)
		ave->ipc_reply[i] = readl(ave->mailbox + 0x18 + i * 4);
	dma_rmb();
	dev_info(ave->dev, "AVE_IPC_MAILBOX phase=%s words=%#x,%#x,%#x,%#x\n",
		 ave->phase, ave->ipc_reply[0], ave->ipc_reply[1], ave->ipc_reply[2], ave->ipc_reply[3]);
	return 0;
}

static int ave_create_channels(struct apple_ave *ave)
{
	bool found[2] = { false, false };

	for (unsigned int i = 0; i < 2; i++) {
		u8 *desc = ave->ipc.cpu + AVE_CHANNEL_OFF + i * 0x100;
		u32 direction = get_unaligned_le32(desc + 0x40);
		u32 interrupt = get_unaligned_le32(desc + 0x44);
		u32 slots = get_unaligned_le32(desc + 0x48), offset;
		u64 address = get_unaligned_le64(desc + 0x4c);
		struct ave_channel *c;
		unsigned int id;

		if (!memchr(desc, 0, 64))
			return -EPROTO;
		if (!strcmp(desc, "IO"))
			id = 0;
		else if (!strcmp(desc, "IO_T2H"))
			id = 1;
		else
			return -EPROTO;
		if (found[id] || direction != id || !interrupt || interrupt >= 32 ||
		    !slots || slots > ave->channel_bytes / 64 || !IS_ALIGNED(address, 64) ||
		    ave_fw_offset(ave, address, (size_t)slots * 64, &offset) ||
		    offset < AVE_CHANNEL_OFF + 0x200 ||
		    offset + slots * 64 > AVE_CHANNEL_OFF + ave->channel_bytes)
			return -EPROTO;
		found[id] = true;
		c = &ave->channel[id];
		strscpy(c->name, desc, sizeof(c->name));
		c->mode = direction ^ 1;
		c->interrupt = interrupt;
		c->slots = slots;
		c->offset = offset;
		c->consumer = c->mode ? -1 : 0;
		c->producer = c->mode ? 0 : -1;
	}
	if (!found[0] || !found[1] ||
	    (ave->channel[0].offset < ave->channel[1].offset + ave->channel[1].slots * 64 &&
	     ave->channel[1].offset < ave->channel[0].offset + ave->channel[0].slots * 64))
		return -EPROTO;
	/* Initialize only host-originated slots; leave all padding and
	 * firmware-originated slots unchanged. Ownership is bit zero of address.
	 */
	for (unsigned int i = 0; i < ave->channel[0].slots; i++) {
		__le64 *slot = ave->ipc.cpu + ave->channel[0].offset + i * 64;
		slot[0] = cpu_to_le64(1);
		slot[1] = 0;
		slot[2] = 0;
	}
	dma_wmb();
	return 0;
}

static int ave_negotiate_ipc(struct apple_ave *ave)
{
	u64 ticks, time_delta, handshake_address, channel_address;
	u32 frequency, status, offset;
	u8 *handshake = ave->ipc.cpu + AVE_HANDSHAKE_OFF;
	int ret;

	ave->phase = "validate-offer";
	if (ave->hello[0] != 2 || ave->hello[2] != 0x100 ||
	    ave->hello[1] < 0x200 || ave->hello[1] > AVE_HANDSHAKE_OFF - AVE_CHANNEL_OFF ||
	    !ave->hello[3] || ave->hello[3] > 4 * SZ_1M)
		return -EPROTO;
	ave->channel_bytes = ave->hello[1];
	ret = ave_alloc_buffer(ave, &ave->heap, PAGE_ALIGN(ave->hello[3]));
	if (ret)
		return ret;
	writel(1, ave->mailbox + 0x10);
	ticks = readq(ave->coproc + 0x378000);
	frequency = readl(ave->coproc + 0x360020);
	if (frequency && frequency < 1000000)
		return -EPROTO;
	ticks = div64_u64(ticks, frequency ? frequency / 1000000 : 24);
	time_delta = div_u64(ktime_get_ns(), 1000) - ticks;
	writel(lower_32_bits(time_delta), ave->mailbox + 0x28);
	writel(upper_32_bits(time_delta), ave->mailbox + 0x2c);
	ave->phase = "ipc-base";
	ave_mailbox_send(ave, lower_32_bits(ave->ipc.dma), upper_32_bits(ave->ipc.dma), ave->ipc.size, 0);
	ret = ave_mailbox_receive(ave);
	if (ret)
		return ret;
	ave->ipc_fw_base = (u64)ave->ipc_reply[1] << 32 | ave->ipc_reply[0];
	if (!ave->ipc_fw_base || !IS_ALIGNED(ave->ipc_fw_base, PAGE_SIZE) ||
	    ave->ipc_fw_base > U64_MAX - ave->ipc.size)
		return -EPROTO;
	memset(ave->ipc.cpu + AVE_CHANNEL_OFF, 0, ave->channel_bytes);
	memset(handshake, 0, 0x50);
	memset(ave->ipc.cpu + AVE_SHARED_OFF, 0, SZ_64K);
	channel_address = ave->ipc_fw_base + AVE_CHANNEL_OFF;
	handshake_address = ave->ipc_fw_base + AVE_HANDSHAKE_OFF;
	put_unaligned_le64(channel_address, handshake + 8);
	put_unaligned_le64(ave->ipc_fw_base + AVE_SHARED_OFF, handshake + 0x10);
	put_unaligned_le32(SZ_64K, handshake + 0x18);
	put_unaligned_le64(ave->heap.dma, handshake + 0x1c);
	put_unaligned_le32(ave->hello[3], handshake + 0x24);
	put_unaligned_le32(27, handshake + 0x28);
	dma_wmb();
	ave->phase = "channel-offer";
	ave_mailbox_send(ave, lower_32_bits(handshake_address), upper_32_bits(handshake_address), 0, 0);
	ret = ave_mailbox_receive(ave);
	if (ret)
		return ret;
	ave->extra_bytes = ave->ipc_reply[2];
	if (ave_fw_offset(ave, (u64)ave->ipc_reply[1] << 32 | ave->ipc_reply[0],
			  ave->channel_bytes, &offset) || offset != AVE_CHANNEL_OFF ||
	    ave->extra_bytes > 0x13c000)
		return -EPROTO;
	ave->phase = "channel-layout";
	ret = ave_create_channels(ave);
	if (ret)
		return ret;
	ave->phase = "ready-flag";
	writel(AVE_START_MAGIC, ave->mailbox + 0x24);
	ret = readl_poll_timeout(ave->mailbox + 0x24, status, !status, 50, 5000000);
	if (ret)
		return ret;
	ave->ipc_ready = true;
	ave->phase = "ready";
	return 0;
}

static int ave_ring_send(struct apple_ave *ave, struct ave_channel *c, u64 addr, u32 len, u32 flags)
{
	__le64 *slot;
	int next;

	if (c->producer < 0)
		return -ENOSPC;
	slot = ave->ipc.cpu + c->offset + c->producer * 64;
	WRITE_ONCE(slot[1], cpu_to_le64(len));
	WRITE_ONCE(slot[2], cpu_to_le64(flags));
	dma_wmb();
	WRITE_ONCE(slot[0], cpu_to_le64(addr | (c->mode ^ 1)));
	dma_wmb();
	if (c->consumer < 0)
		c->consumer = c->producer;
	next = (c->producer + 1) % c->slots;
	c->producer = next == c->consumer ? -1 : next;
	writel(BIT(c->interrupt), ave->mailbox + 0x0c);
	return 0;
}

static int ave_ring_receive(struct apple_ave *ave, struct ave_channel *c, u64 *addr, u32 *len, u32 *flags)
{
	__le64 *slot;
	u64 word;
	int next;

	if (c->consumer < 0)
		return -EAGAIN;
	slot = ave->ipc.cpu + c->offset + c->consumer * 64;
	word = le64_to_cpu(READ_ONCE(slot[0]));
	if ((word & 1) != c->mode)
		return -EAGAIN;
	dma_rmb();
	*addr = word & ~3ULL;
	*len = le64_to_cpu(READ_ONCE(slot[1]));
	*flags = le64_to_cpu(READ_ONCE(slot[2]));
	if (c->producer < 0)
		c->producer = c->consumer;
	next = (c->consumer + 1) % c->slots;
	c->consumer = next == c->producer ? -1 : next;
	return 0;
}

/* Root-only experimental packet transport. The packet is copied into owned
 * coherent memory; timeout pins the command too and forbids reuse this boot.
 * Firmware command success is established by decoding replies, not this API.
 */
static ssize_t ave_command_write(struct file *file, const char __user *data, size_t len, loff_t *pos)
{
	struct apple_ave *ave = file->private_data;
	u8 *cmd = ave->ipc.cpu + AVE_COMMAND_OFF;
	u64 deadline, addr;
	u32 bytes, flags, offset;
	bool ack = false;
	int ret = 0;

	if (!capable(CAP_SYS_RAWIO))
		return -EPERM;
	static_assert(AVE_COMMAND_OFF + AVE_COMMAND_BYTES <= AVE_ARENA_OFF);
	if (len < 0x48 || len > AVE_COMMAND_BYTES)
		return -EINVAL;
	mutex_lock(&ave->lock);
	if (!ave->ipc_ready || ave->command_failed) {
		ret = -EBUSY;
		goto out;
	}
	if (copy_from_user(cmd, data, len)) {
		ret = -EFAULT;
		goto out;
	}
	/* The common command timeout is {duration_us, creation_time_us}.
	 * Stamp it in the same Linux monotonic clock synchronized at startup;
	 * captured macOS absolute times cannot be replayed on this boot.
	 */
	put_unaligned_le64(3000000, cmd + 0x30);
	put_unaligned_le64(div_u64(ktime_get_ns(), 1000), cmd + 0x38);
	ave->response_bytes = 0;
	ret = ave_ring_send(ave, &ave->channel[0], ave->ipc_fw_base + AVE_COMMAND_OFF, len, 0);
	if (ret)
		goto out;
	ave->commands++;
	deadline = ktime_get_ns() + 5ULL * NSEC_PER_SEC;
	do {
		u32 pending = readl(ave->mailbox + 0x10);
		if (pending)
			writel(pending, ave->mailbox + 0x10);
		for (unsigned int ch = 0; ch < 2; ch++) {
			ret = ave_ring_receive(ave, &ave->channel[ch], &addr, &bytes, &flags);
			if (ret == -EAGAIN)
				continue;
			/* Channel 1 returns the original address with zero payload
			 * bytes (verified native AVC captures). Only channel 2 is
			 * copied to response, independent of submitted packet size.
			 */
			if (ret || (!ch && bytes != 0) ||
			    (ch && bytes > sizeof(ave->response) - 24) ||
			    ave_fw_offset(ave, addr, bytes, &offset)) {
				ret = -EPROTO;
				goto failed;
			}
			dev_info(ave->dev, "AVE_COMMAND_REPLY channel=%u address=%#llx bytes=%u flags=%#x\n",
				 ch + 1, addr, bytes, flags);
			if (!ch) {
				if (addr != ave->ipc_fw_base + AVE_COMMAND_OFF) {
					ret = -EPROTO;
					goto failed;
				}
				ack = true;
				ave->transport_acks++;
			} else {
				put_unaligned_le32(ch + 1, ave->response);
				put_unaligned_le32(bytes, ave->response + 4);
				put_unaligned_le32(flags, ave->response + 8);
				put_unaligned_le32(0, ave->response + 12);
				put_unaligned_le64(addr, ave->response + 16);
				memcpy(ave->response + 24, ave->ipc.cpu + offset, bytes);
				ave->response_bytes = bytes + 24;
				ave->replies++;
				/* Reply echoes use zero flags on the observed transport. */
				ret = ave_ring_send(ave, &ave->channel[ch], addr, bytes, 0);
				if (ret)
					goto failed;
			}
		}
		if (ack && ave->response_bytes) {
			ret = 0;
			goto out;
		}
		usleep_range(100, 200);
	} while (ktime_get_ns() < deadline);
	ret = -ETIMEDOUT;
failed:
	ave->command_failed = true;
out:
	mutex_unlock(&ave->lock);
	return ret ?: len;
}

static ssize_t ave_command_read(struct file *file, char __user *data, size_t len, loff_t *pos)
{
	struct apple_ave *ave = file->private_data;
	ssize_t ret;

	mutex_lock(&ave->lock);
	ret = simple_read_from_buffer(data, len, pos, ave->response, ave->response_bytes);
	mutex_unlock(&ave->lock);
	return ret;
}

static const struct file_operations ave_command_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.write = ave_command_write,
	.read = ave_command_read,
};

/* Drain later firmware events without publishing another command. Results
 * replace the response buffer and consist of one or more 24-byte-header
 * records, each followed by its declared payload. Never discard silently.
 */
static ssize_t ave_poll_write(struct file *file, const char __user *data, size_t len, loff_t *pos)
{
	struct apple_ave *ave = file->private_data;
	bool requested;
	u64 deadline, addr;
	u32 bytes, flags, offset;
	int ret;

	if (!capable(CAP_SYS_RAWIO))
		return -EPERM;
	ret = kstrtobool_from_user(data, len, &requested);
	if (ret || !requested)
		return ret ?: -EINVAL;
	mutex_lock(&ave->lock);
	if (!ave->ipc_ready || ave->command_failed) {
		ret = -EBUSY;
		goto out;
	}
	ave->response_bytes = 0;
	deadline = ktime_get_ns() + NSEC_PER_SEC;
	do {
		u32 pending = readl(ave->mailbox + 0x10);
		u8 *dst;

		if (pending)
			writel(pending, ave->mailbox + 0x10);
		ret = ave_ring_receive(ave, &ave->channel[1], &addr, &bytes, &flags);
		if (ret == -EAGAIN) {
			usleep_range(100, 200);
			continue;
		}
		if (ret || ave_fw_offset(ave, addr, bytes, &offset) ||
		    bytes > sizeof(ave->response) - 24 ||
		    ave->response_bytes > sizeof(ave->response) - 24 - bytes) {
			ret = -EPROTO;
			goto failed;
		}
		dst = ave->response + ave->response_bytes;
		put_unaligned_le32(2, dst);
		put_unaligned_le32(bytes, dst + 4);
		put_unaligned_le32(flags, dst + 8);
		put_unaligned_le32(0, dst + 12);
		put_unaligned_le64(addr, dst + 16);
		memcpy(dst + 24, ave->ipc.cpu + offset, bytes);
		ave->response_bytes += 24 + bytes;
		ave->replies++;
		ret = ave_ring_send(ave, &ave->channel[1], addr, bytes, 0);
		if (ret)
			goto failed;
	} while (ktime_get_ns() < deadline);
	ret = 0;
	goto out;
failed:
	ave->command_failed = true;
out:
	mutex_unlock(&ave->lock);
	return ret ?: len;
}

static const struct file_operations ave_poll_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.write = ave_poll_write,
};

/* Bounded staging area for image/reference/output surfaces. Userspace offsets
 * start at zero here, beyond all handshake, ring and command allocations.
 * The status file supplies both DART and firmware addresses for this arena.
 */
static ssize_t ave_arena_read(struct file *file, char __user *data, size_t len, loff_t *pos)
{
	struct apple_ave *ave = file->private_data;
	ssize_t ret;

	mutex_lock(&ave->lock);
	dma_rmb();
	ret = simple_read_from_buffer(data, len, pos, ave->ipc.cpu + AVE_ARENA_OFF,
				     ave->ipc.size - AVE_ARENA_OFF);
	mutex_unlock(&ave->lock);
	return ret;
}

static ssize_t ave_arena_write(struct file *file, const char __user *data, size_t len, loff_t *pos)
{
	struct apple_ave *ave = file->private_data;
	ssize_t ret;

	if (!capable(CAP_SYS_RAWIO))
		return -EPERM;
	mutex_lock(&ave->lock);
	if (!ave->ipc_ready || ave->command_failed)
		ret = -EBUSY;
	else
		ret = simple_write_to_buffer(ave->ipc.cpu + AVE_ARENA_OFF,
					    ave->ipc.size - AVE_ARENA_OFF, pos, data, len);
	dma_wmb();
	mutex_unlock(&ave->lock);
	return ret;
}

static loff_t ave_arena_seek(struct file *file, loff_t offset, int whence)
{
	return fixed_size_llseek(file, offset, whence, AVE_IPC_BYTES - AVE_ARENA_OFF);
}

static const struct file_operations ave_arena_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.read = ave_arena_read,
	.write = ave_arena_write,
	.llseek = ave_arena_seek,
};
