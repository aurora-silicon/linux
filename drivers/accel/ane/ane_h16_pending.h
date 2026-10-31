/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef __ANE_H16_PENDING_H__
#define __ANE_H16_PENDING_H__

#include <linux/jiffies.h>

#include "ane_h16_execute.h"

#define ANE_H16_PENDING_MAX 64

struct ane_h16_pending {
	struct ane_h16_request_key key;
	unsigned long deadline;
	int status;
	bool active;
	bool terminal;
};

/* All operations, including event dispatch, require the firmware mutex. */
struct ane_h16_pending_table {
	struct ane_h16_pending *slots[ANE_H16_PENDING_MAX];
	u64 next_uuid;
	int error;
	bool quarantined;
};

static inline int
ane_h16_pending_insert(struct ane_h16_pending_table *table,
		       struct ane_h16_pending *request, unsigned long now)
{
	unsigned int i;

	if (table->quarantined)
		return -EIO;
	if (!request || request->active || time_after_eq(now, request->deadline))
		return -EINVAL;
	for (i = 0; i < ANE_H16_PENDING_MAX; i++)
		if (table->slots[i] == request)
			return -EEXIST;
	if (table->next_uuid == U64_MAX)
		return -EOVERFLOW;
	for (i = 0; i < ANE_H16_PENDING_MAX; i++) {
		if (table->slots[i])
			continue;
		/* Never reuse a UUID, including after pre-publication failure. */
		request->key.uuid = ++table->next_uuid;
		request->status = -EINPROGRESS;
		request->terminal = false;
		request->active = true;
		table->slots[i] = request;
		return 0;
	}
	return -ENOSPC;
}

/* Removal is not permission to release DMA backing. */
static inline void
ane_h16_pending_remove(struct ane_h16_pending_table *table,
		       struct ane_h16_pending *request)
{
	unsigned int i;

	for (i = 0; i < ANE_H16_PENDING_MAX; i++)
		if (table->slots[i] == request)
			table->slots[i] = NULL;
	request->active = false;
}

/* A published command with an uncertain outcome pins all its mappings. */
static inline void
ane_h16_pending_abort(struct ane_h16_pending_table *table,
		      struct ane_h16_pending *request, int status)
{
	table->quarantined = true;
	table->error = status;
	ane_h16_pending_remove(table, request);
	request->status = status;
}

static inline int
ane_h16_pending_event(struct ane_h16_pending_table *table, const void *bytes,
		      size_t length, unsigned int channel, unsigned long now)
{
	struct ane_h16_pending *request = NULL;
	struct ane_h16_event event;
	unsigned int i;
	int ret;

	if (table->quarantined)
		return -EIO;
	ret = ane_h16_event_decode(bytes, length, channel, &event);
	if (ret)
		goto corrupt;
	if (event.destination)
		return -ENOMSG;
	for (i = 0; i < ANE_H16_PENDING_MAX; i++)
		if (table->slots[i] && table->slots[i]->key.uuid == event.key.uuid) {
			request = table->slots[i];
			break;
		}
	/* Duplicates and late/foreign UUIDs can never retire another call. */
	if (!request)
		return -ENOENT;
	ret = ane_h16_event_result(bytes, length, channel, &request->key);
	if (ret < 0)
		goto corrupt;
	if (time_after_eq(now, request->deadline)) {
		ane_h16_pending_abort(table, request, -ETIMEDOUT);
		return -ETIMEDOUT;
	}
	if (ret == ANE_H16_EVENT_PROGRESS)
		return ret;
	ane_h16_pending_remove(table, request);
	request->terminal = true;
	request->status = ret == ANE_H16_EVENT_COMPLETE ? 0 : -EIO;
	if (request->status) {
		table->error = request->status;
		table->quarantined = true;
	}
	return ret;

corrupt:
	table->error = ret;
	table->quarantined = true;
	return ret;
}

/* Channel-6 CPU-load notifications are diagnostics, never completions. */
static inline int
ane_h16_pending_dispatch(struct ane_h16_pending_table *table, const void *bytes,
			 size_t length, unsigned int channel, unsigned long now)
{
	const u8 *p = bytes;

	if (table->quarantined)
		return -EIO;
	if (p && channel == 6 && length == 0xc &&
	    get_unaligned_le16(p + 4) == 0x0107 && !get_unaligned_le16(p + 6))
		return ANE_H16_EVENT_PROGRESS;
	return ane_h16_pending_event(table, bytes, length, channel, now);
}

#endif /* __ANE_H16_PENDING_H__ */
