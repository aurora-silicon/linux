/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef __APPLE_ADMAC_PAYLOAD_H
#define __APPLE_ADMAC_PAYLOAD_H

#include <linux/dma/apple-admac.h>
#include <linux/string.h>

/* Four FIFO entries, an active entry, and headroom for completion reporting. */
#define ADMAC_PAYLOAD_SLOTS 8
#define ADMAC_PAYLOAD_MAX_PERIOD (128 * 1024)

struct admac_payload {
	struct apple_admac_playback_filter filter;
	void *area;
	dma_addr_t dma;
	u64 ticket[ADMAC_PAYLOAD_SLOTS];
	u8 head;
	u8 pending;
	bool quarantined;
};

/* The same ownership implementation is exercised without MMIO by KUnit. */
static inline int admac_payload_prepare(struct admac_payload *p, size_t offset,
				       dma_addr_t *address)
{
	size_t length = p->filter.period_bytes;
	unsigned int slot;
	void *copy;
	u64 ticket = 0;
	int ret;

	if (p->quarantined)
		return -EIO;
	if (p->pending == ADMAC_PAYLOAD_SLOTS)
		return -ENOSPC;
	if (!length || length > p->filter.buffer_bytes ||
	    offset > p->filter.buffer_bytes - length)
		return -EINVAL;
	slot = (p->head + p->pending) % ADMAC_PAYLOAD_SLOTS;
	copy = p->area + slot * length;
	memcpy(copy, p->filter.source + offset, length);
	ret = p->filter.process(p->filter.context, copy, length, &ticket);
	if (ret)
		return ret;
	p->ticket[slot] = ticket;
	p->pending++;
	*address = p->dma + slot * length;
	return 0;
}

static inline int admac_payload_retire(struct admac_payload *p)
{
	int ret;

	if (p->quarantined)
		return -EIO;
	if (!p->pending)
		return -ESTALE;
	ret = p->filter.retire(p->filter.context, p->ticket[p->head]);
	if (ret)
		return ret;
	p->head = (p->head + 1) % ADMAC_PAYLOAD_SLOTS;
	p->pending--;
	return 0;
}

static inline void admac_payload_quarantine(struct admac_payload *p)
{
	if (p->quarantined)
		return;
	p->quarantined = true;
	p->filter.fault(p->filter.context);
	/* The client may go away; no later callback may reach its context. */
	p->filter.process = NULL;
	p->filter.retire = NULL;
	p->filter.fault = NULL;
	p->filter.context = NULL;
}

#endif
