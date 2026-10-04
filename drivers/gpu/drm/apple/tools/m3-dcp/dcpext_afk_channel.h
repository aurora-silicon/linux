/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/* Bounded additional AFK channel, independent state for concurrent services.
 * Adapted from this probe's Asahi-derived afk_probe ring/handshake code.
 * Authors/references: Sven Peter, Asahi Linux Contributors; dynamic ring
 * block layout from Eileen Yoon / m1n1 rbep.py. No hardware ownership here:
 * caller retains every allocation until ALL endpoints and RTKit quiesce.
 * Single serialized caller; callbacks must not reenter this engine.
 */
#ifndef DCPEXT_AFK_CHANNEL_H
#define DCPEXT_AFK_CHANNEL_H
/* Announcement-only M3 copy: the unused M4 ring writer is omitted. */
struct afkc;
struct afkc_ops {
	int (*send)(void *cookie, u8 endpoint, u64 message);
	int (*allocate)(void *cookie, u32 size, u8 **buffer, u64 *dva);
	int (*record)(void *cookie, const u8 *payload, u32 size);
};
struct afkc_ring {
	u8 *header, *data;
	u32 size, block, offset, extent;
};
struct afkc {
	const struct afkc_ops *ops;
	void *cookie;
	u8 endpoint;
	bool started, running, stopping, stopped;
	bool streaming; /* Desktop owner accounts traffic across endpoints. */
	int error;
	u32 messages, buffer_size, tag, capture_size;
	u32 max_payload; /* Zero preserves the 256-byte default; explicit maximum4096. */
	u8 *buffer;
	struct afkc_ring rings[2];
	u8 capture[32768];
};

static int afkc_fail(struct afkc *c, int error)
{
	if (!c->error)
		c->error = error;
	return c->error;
}

static int afkc_send(struct afkc *c, u32 type, u64 payload)
{
	int ret = c->ops->send(c->cookie, c->endpoint, (u64)type << 48 | payload);
	return ret ? afkc_fail(c, ret) : 0;
}

static int afkc_start(struct afkc *c, const struct afkc_ops *ops, void *cookie, u8 ep)
{
	int ret;
	if (c->started || !ops || !ops->send || !ops->allocate || !ops->record ||
	    ep < 0x20 || ep > 0x3f)
		return -EINVAL;
	c->ops = ops;
	c->cookie = cookie;
	c->endpoint = ep;
	c->started = true; /* Published start may take effect even on send error. */
	ret = ops->send(cookie, 0, (5ULL << 52) | ((u64)ep << 32) | 2);
	return ret ? afkc_fail(c, ret) : afkc_send(c, 0x80, 0);
}

static bool afkc_tx_empty(struct afkc *c)
{
	struct afkc_ring *r = &c->rings[0];
	u32 rd, wr;
	if (!r->header)
		return false;
	rd = READ_ONCE(*(u32 *)(r->header + r->block));
	wr = READ_ONCE(*(u32 *)(r->header + 2 * r->block));
	dma_rmb();
	return rd < r->size && !(rd & (r->block - 1)) && rd == wr;
}

static int afkc_drain(struct afkc *c)
{
	struct afkc_ring *r = &c->rings[1];
	u32 rd, wr, bytes, total;
	u8 *qe;
	int ret;
	if (!c->running || !r->header)
		return -EPROTO;
	for (u32 n = 0; n < 64; n++) {
		rd = READ_ONCE(*(u32 *)(r->header + r->block));
		wr = READ_ONCE(*(u32 *)(r->header + 2 * r->block));
		dma_rmb();
		if (rd >= r->size || wr >= r->size || (rd | wr) & (r->block - 1))
			return -EPROTO;
		if (rd == wr)
			return 0;
		qe = r->data + rd;
		if (get_unaligned_le32(qe) != 0x20504f49)
			return -EPROTO;
		bytes = get_unaligned_le32(qe + 4);
		if (bytes > r->size - 16)
			return -EPROTO;
		if (bytes + 16 > r->size - rd) {
			if (wr >= rd || !wr)
				return -EPROTO;
			rd = 0;
			qe = r->data;
			if (get_unaligned_le32(qe) != 0x20504f49)
				return -EPROTO;
			bytes = get_unaligned_le32(qe + 4);
		}
		if (bytes < 8 || bytes > r->size - rd - 16)
			return -EPROTO;
		total = ALIGN(bytes + 16, r->block);
		if (c->streaming && bytes + 12 > sizeof(c->capture) - c->capture_size)
			c->capture_size = 0;
		if (total > r->size - rd || (rd < wr && total > wr - rd) ||
		    bytes + 12 > sizeof(c->capture) - c->capture_size)
			return -EOVERFLOW;
		/* Preserve the existing offline capture framing. */
		memcpy(c->capture + c->capture_size, qe + 8, 8);
		put_unaligned_le32(bytes, c->capture + c->capture_size + 8);
		memcpy(c->capture + c->capture_size + 12, qe + 16, bytes);
		c->capture_size += bytes + 12;
		ret = c->ops->record(c->cookie, qe + 16, bytes);
		rd += total;
		if (rd == r->size)
			rd = 0;
		dma_mb();
		WRITE_ONCE(*(u32 *)(r->header + r->block), rd);
		if (ret)
			return ret;
	}
	return -E2BIG;
}

static int afkc_receive(struct afkc *c, u64 message)
{
	u32 type = message >> 48, size, offset, index, data_size, block;
	u64 dva;
	struct afkc_ring *r, *other;
	int ret = 0;
	if (c->error)
		return c->error;
	if (!c->started || c->stopped || (!c->streaming && ++c->messages > 128))
		return afkc_fail(c, -EPROTO);
	switch (type) {
	case 0xa0:
	case 0x8c:
		break;
	case 0x89:
		size = ((message >> 16) & 0xffff) * 64;
		if (c->buffer || c->stopping || !size || size > 65536)
			return afkc_fail(c, -EINVAL);
		ret = c->ops->allocate(c->cookie, size, &c->buffer, &dva);
		if (ret)
			break;
		c->buffer_size = size;
		c->tag = message & 0xffff;
		if (!c->buffer || dva < (1ULL << 40) ||
		    dva - (1ULL << 40) >= (1ULL << 36) || dva & 0x3fff)
			return afkc_fail(c, -EINVAL);
		ret = afkc_send(c, 0xa1, dva);
		break;
	case 0x8a:
	case 0x8b:
		index = type == 0x8a ? 0 : 1;
		r = &c->rings[index];
		other = &c->rings[!index];
		offset = ((message >> 32) & 0xffff) * 64;
		size = ((message >> 16) & 0xffff) * 64;
		if (!c->buffer || r->header || c->stopping || size < 256 ||
		    (message & 0xffff) != c->tag || offset >= c->buffer_size ||
		    size > c->buffer_size - offset || (other->header &&
		    offset < other->offset + other->extent && other->offset < offset + size))
			return afkc_fail(c, -EINVAL);
		data_size = READ_ONCE(*(u32 *)(c->buffer + offset));
		if (data_size >= size || (size - data_size) % 3)
			return afkc_fail(c, -EPROTO);
		block = (size - data_size) / 3;
		if (block < 64 || block > 256 || !is_power_of_2(block) ||
		    data_size < block || data_size % block)
			return afkc_fail(c, -EPROTO);
		*r = (struct afkc_ring){.header = c->buffer + offset,
			.data = c->buffer + offset + 3 * block, .size = data_size,
			.block = block, .offset = offset, .extent = size};
		if (other->header)
			ret = afkc_send(c, 0xa3, 0);
		break;
	case 0x86:
		if (!c->rings[0].header || !c->rings[1].header || c->running || c->stopping)
			return afkc_fail(c, -EPROTO);
		c->running = true;
		break;
	case 0x85:
		ret = afkc_drain(c);
		break;
	case 0xc1:
		if (!c->stopping || !afkc_tx_empty(c))
			return afkc_fail(c, -EPROTO);
		c->stopped = true;
		break;
	default:
		ret = -EOPNOTSUPP;
	}
	return ret ? afkc_fail(c, ret) : 0;
}

static int afkc_stop(struct afkc *c)
{
	if (c->error)
		return c->error;
	if (!c->running || c->stopping || !afkc_tx_empty(c))
		return -EAGAIN;
	c->stopping = true;
	return afkc_send(c, 0xc0, 0);
}
#endif
