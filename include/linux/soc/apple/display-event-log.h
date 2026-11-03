/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LINUX_SOC_APPLE_DISPLAY_EVENT_LOG_H
#define _LINUX_SOC_APPLE_DISPLAY_EVENT_LOG_H

#include <linux/atomic.h>
#include <linux/build_bug.h>
#include <linux/errno.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/timekeeping.h>
#include <linux/types.h>

#define APPLE_DISPLAY_EVENT_CAPACITY 512U
#define APPLE_DISPLAY_EVENT_CHUNK 8U
#define APPLE_DISPLAY_EVENT_SCHEMA 1U

struct apple_display_event_record {
	u64 timestamp_ns;
	u64 sequence;
	u16 type;
	u16 flags;
	s32 result;
	u64 data[9];
};
static_assert(sizeof(struct apple_display_event_record) == 96);

/* Atomics count attempted calls, including callers which cannot take lock. */
struct apple_display_event_log {
	raw_spinlock_t lock;
	char name[64];
	atomic64_t attempts;
	atomic64_t busy_drops;
	atomic64_t sequence_drops;
	u64 committed;
	struct apple_display_event_record records[APPLE_DISPLAY_EVENT_CAPACITY];
};

/* Atomic counters are individually sampled, not one atomic transaction. */
struct apple_display_event_counters {
	u64 committed;
	u64 attempts;
	u64 busy_drops;
	u64 sequence_drops;
	u64 overwritten;
};

/* Standalone storage: no device, ring, lock or callback pointer survives copy. */
struct apple_display_event_snapshot {
	char name[64];
	struct apple_display_event_counters start;
	struct apple_display_event_counters end;
	u64 seq_start;
	u64 seq_end;
	u64 start_ns;
	u64 end_ns;
	u32 record_count;
	u32 copied;
	u32 gaps;
	bool complete;
	u8 valid[APPLE_DISPLAY_EVENT_CAPACITY];
	struct apple_display_event_record records[APPLE_DISPLAY_EVENT_CAPACITY];
};

/* Owner allocates/initializes before publishing, and quiesces before freeing. */
static inline void apple_display_event_log_init(struct apple_display_event_log *log,
					       const char *name)
{
	memset(log, 0, sizeof(*log));
	raw_spin_lock_init(&log->lock);
	atomic64_set(&log->attempts, 0);
	atomic64_set(&log->busy_drops, 0);
	atomic64_set(&log->sequence_drops, 0);
	strscpy(log->name, name, sizeof(log->name));
}

static inline void __apple_display_event_record(struct apple_display_event_log *log,
						u16 type, s32 result, u16 flags,
						const u64 data[9])
{
	struct apple_display_event_record *record;
	unsigned long irqflags;
	u64 sequence;

	atomic64_inc(&log->attempts);
	if (!raw_spin_trylock_irqsave(&log->lock, irqflags)) {
		atomic64_inc(&log->busy_drops);
		return;
	}
	/* Never wrap to sequence zero or relabel old records after exhaustion. */
	if (log->committed == ~(u64)0) {
		atomic64_inc(&log->sequence_drops);
		raw_spin_unlock_irqrestore(&log->lock, irqflags);
		return;
	}
	sequence = log->committed + 1;
	record = &log->records[(sequence - 1) % APPLE_DISPLAY_EVENT_CAPACITY];
	record->timestamp_ns = ktime_get_mono_fast_ns();
	record->sequence = sequence;
	record->type = type;
	record->flags = flags;
	record->result = result;
	memcpy(record->data, data, sizeof(record->data));
	log->committed = sequence;
	raw_spin_unlock_irqrestore(&log->lock, irqflags);
}

/* Disabled means no payload evaluation, clock read, lock or counter access.
 * Payload expressions must still be POD observations, never hardware helpers.
 */
#define APPLE_DISPLAY_EVENT_RECORD(log_, type_, result_, flags_, \
				   a_, b_, c_, d_, e_, f_, g_, h_, i_) do { \
	struct apple_display_event_log *__event_log = (log_); \
	if (__event_log) { \
		const u64 __event_data[9] = { (a_), (b_), (c_), (d_), (e_), \
					     (f_), (g_), (h_), (i_) }; \
		__apple_display_event_record(__event_log, (type_), (result_), \
					     (flags_), __event_data); \
	} \
} while (0)

/* Caller holds the recorder lock; no hardware or owner state is accessed. */
static inline void __apple_display_event_counters(struct apple_display_event_log *log,
						 struct apple_display_event_counters *out)
{
	out->committed = log->committed;
	out->attempts = (u64)atomic64_read(&log->attempts);
	out->busy_drops = (u64)atomic64_read(&log->busy_drops);
	out->sequence_drops = (u64)atomic64_read(&log->sequence_drops);
	out->overwritten = log->committed > APPLE_DISPLAY_EVENT_CAPACITY ?
		log->committed - APPLE_DISPLAY_EVENT_CAPACITY : 0;
}

/* Allocate snapshot before acquiring any owner reference/lock. EAGAIN is not
 * retried here: a reader must never spin waiting for a producer or vice versa.
 */
static inline int apple_display_event_snapshot_begin(struct apple_display_event_log *log,
						     struct apple_display_event_snapshot *out)
{
	unsigned long irqflags;

	memset(out, 0, sizeof(*out));
	out->start_ns = ktime_get_mono_fast_ns();
	if (!raw_spin_trylock_irqsave(&log->lock, irqflags))
		return -EAGAIN;
	memcpy(out->name, log->name, sizeof(out->name));
	__apple_display_event_counters(log, &out->start);
	out->seq_end = log->committed;
	out->record_count = log->committed < APPLE_DISPLAY_EVENT_CAPACITY ?
		log->committed : APPLE_DISPLAY_EVENT_CAPACITY;
	out->seq_start = out->record_count ?
		out->seq_end - out->record_count + 1 : 0;
	raw_spin_unlock_irqrestore(&log->lock, irqflags);
	return 0;
}

/* Return 1 for more chunks, 0 when complete, or EAGAIN without blocking.
 * The fixed begin bounds never expand to include later writes. Each requested
 * sequence is checked under lock; an overwrite is explicitly retained as a gap.
 */
static inline int apple_display_event_snapshot_next(struct apple_display_event_log *log,
						    struct apple_display_event_snapshot *out)
{
	unsigned long irqflags;
	u32 limit, i;
	bool finished;

	if (out->complete)
		return 0;
	if (!raw_spin_trylock_irqsave(&log->lock, irqflags))
		return -EAGAIN;
	limit = out->record_count - out->copied;
	if (limit > APPLE_DISPLAY_EVENT_CHUNK)
		limit = APPLE_DISPLAY_EVENT_CHUNK;
	for (i = 0; i < limit; i++) {
		u32 index = out->copied++;
		u64 sequence = out->seq_start + index;
		struct apple_display_event_record *source =
			&log->records[(sequence - 1) % APPLE_DISPLAY_EVENT_CAPACITY];

		if (source->sequence == sequence) {
			out->records[index] = *source;
			out->valid[index] = 1;
		} else {
			/* Cleared by begin: do not expose the replacement payload. */
			out->records[index].sequence = sequence;
			out->gaps++;
		}
	}
	finished = out->copied == out->record_count;
	if (finished)
		__apple_display_event_counters(log, &out->end);
	raw_spin_unlock_irqrestore(&log->lock, irqflags);
	if (finished) {
		out->end_ns = ktime_get_mono_fast_ns();
		out->complete = true;
	}
	return finished ? 0 : 1;
}

static inline int apple_display_event_snapshot_copy(struct apple_display_event_log *log,
						    struct apple_display_event_snapshot *out)
{
	int ret = apple_display_event_snapshot_begin(log, out);

	if (ret)
		return ret;
	do {
		ret = apple_display_event_snapshot_next(log, out);
	} while (ret > 0);
	return ret;
}

#endif /* _LINUX_SOC_APPLE_DISPLAY_EVENT_LOG_H */
