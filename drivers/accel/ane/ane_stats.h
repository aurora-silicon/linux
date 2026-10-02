/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/*
 * Producer-side ANE counters and submission ring, shared by ane.ko and
 * ane_t6021.ko.
 *
 * Contract:
 *   - sysfs under /sys/class/accel/accel<digit>/device/ane_stats,
 *     mode 0444, ASCII "key value" lines, integers only: busy_ns
 *     (cumulative u64), jobs (cumulative u64). busy_ns counts only
 *     nanoseconds the engine was busy (union of submit-to-completion
 *     windows) since the device was bound, and advances while work is
 *     in flight: the show callback adds the open period's live tail.
 *   - debugfs ane_timeline: preallocated ring of the last N submissions
 *     with seq, submit_ns, start_ns, end_ns, tasks, rc (and tmst raw
 *     when known). Head counter is atomic; per-slot seqlock so the
 *     reader never blocks the producer and torn reads are detected.
 *   - `stats` module parameter bool, default 1; stats=0 makes the hot
 *     path one predictable branch and skips sysfs/debugfs creation.
 *
 * No allocation, locking, or formatting in the hot path. The sysfs
 * show callback formats in process context; the ring writer commits
 * the slot with a release store, and the reader does an acquire load
 * on begin/end. Counters update with atomics only. busy_ns is the
 * union of busy intervals: a busy period opens when the first
 * submission lands on an idle engine and closes when the last one
 * completes, and its whole span folds once at close; overlapping
 * submissions share the period, so a serialized engine (ane.ko,
 * behind engine_lock) reports sum(end - start) and a parallel engine
 * (ane_t6021) reports the union, never the sum.
 */

#ifndef __ANE_STATS_H__
#define __ANE_STATS_H__

#include <linux/atomic.h>
#include <linux/ktime.h>
#include <linux/seq_file.h>
#include <linux/string.h>
#include <linux/sysfs.h>
#include <linux/types.h>
#include <drm/drm_debugfs.h>

/*
 * Per-device counters. busy_ns holds the busy time folded from closed
 * busy periods: a period opens when the first submission arrives on an
 * idle engine and closes when the last one completes, and its whole
 * span folds once at close. Overlapping submissions share the period,
 * so busy_ns is the union of the submit-to-completion intervals, not
 * their sum. The show callback adds the live tail of the still-open
 * period, so the reported value advances while engine work is in
 * flight. jobs counts completed submissions. last_busy_end is the open
 * period's start timestamp; inflight is the number of in-flight
 * submissions, with ANE_STATS_INFLIGHT_TRANS as the transient close/
 * open sentinel that keeps a fold and a period start from interleaving.
 *
 * Module-level `stats` parameter governs whether these counters and
 * the ring are created and whether the hot path branches out.
 */
#define ANE_STATS_INFLIGHT_TRANS 0xFFFFFFFFu

struct ane_stats_counters {
	atomic64_t	busy_ns;
	atomic64_t	jobs;
	atomic64_t	last_busy_end;
	atomic64_t	max_end;
	atomic_t	inflight;
};

/*
 * max_end tracks the latest completion sample fed to complete(); the
 * draining completion folds against max(end_ns, max_end) so a stale
 * sample on the last completer cannot truncate the period.
 */
static inline void ane_stats_atomic64_max(u64 i, atomic64_t *v)
{
	u64 old = atomic64_read(v);

	while (i > old && !atomic64_try_cmpxchg(v, &old, i))
		;
}

/*
 * Ring slot: seqlock-style per-slot sequence. Writer bumps seq at
 * entry (odd) and again on commit (even); reader loads begin/end
 * inside acquire load and the order relative to begin/end is preserved
 * by release. begin/end 0 means the slot is empty. submit_ns, end_ns,
 * tasks, rc, tmst are the diagnostic fields; tmst is the raw TM tick
 * on ane.ko (unknown unit) and 0 on ane_t6021 (unavailable, marked so
 * in the file header line).
 */
struct ane_stats_ring_entry {
	atomic64_t	seq;		/* even = committed; odd = writing */
	atomic64_t	submit_ns;
	atomic64_t	start_ns;
	atomic64_t	end_ns;
	atomic_t	tasks;
	atomic_t	rc;
	atomic64_t	tmst;		/* raw TM tick on ane.ko; 0 = unavailable */
};

/*
 * Ring container. head is the next slot to write (increments mod N).
 * N is a power of two preallocated at probe. slots is the array.
 * Reader takes begin/end under acquire and the seqlock protects
 * against torn writes.
 */
struct ane_stats_ring {
	atomic64_t			head;
	u32				n;	/* power of two */
	u32				mask;	/* n - 1 */
	struct ane_stats_ring_entry	*slots;
};

/*
 * Initialize counters and ring from zero. The ring slots array must
 * be preallocated (probe-time) and zeroed. Named
 * ane_stats_counters_init (not ane_stats_init) so the per-driver
 * glue function (ane_stats_init(struct ane_device *) etc.) can name
 * its own static initializer without conflicting with the prototype.
 */
static inline void ane_stats_counters_init(struct ane_stats_counters *ctrs,
					   struct ane_stats_ring *ring,
					   u32 n_shift)
{
	u32 n_total = (1u << n_shift);

	memset(ctrs, 0, sizeof(*ctrs));
	memset(ring, 0, sizeof(*ring));
	ring->n = n_total;
	ring->mask = n_total - 1u;
	atomic64_set_release(&ring->head, 0ull);
}

/*
 * Hot-path submission start. Caller holds the device's submission
 * serialization if the engine is single-producer (ane.ko); concurrent
 * drivers (ane_t6021) pass ring/ctrs and rely on the cmpxchg loops.
 * Returns the submission ticket (head + 1, starting at 1) for the
 * caller to later call ane_stats_complete() with. The ticket, not a
 * slot index, identifies the submission: slots are shared after wrap,
 * tickets are not.
 *
 * Counter side: the first submission onto an idle engine opens a busy
 * period by latching last_busy_end = submit_ns under the transition
 * sentinel; later overlapping submissions only increment inflight.
 * The sentinel window is a handful of instructions, so the bounded
 * cmpxchg retries never observe a half-open period.
 *
 * A period never starts before the previous one folded: a caller
 * sample can be stale (taken before the transition), so the latch is
 * max(submit_ns, max_end), with max_end read under the transition
 * sentinel.
 */
static inline u64 ane_stats_begin(struct ane_stats_counters *ctrs,
				  struct ane_stats_ring *ring,
				  u64 submit_ns, u32 tasks)
{
	u64 ticket = atomic64_fetch_add(1ull, &ring->head) + 1ull;
	struct ane_stats_ring_entry *e =
		&ring->slots[(size_t)(ticket - 1ull) & ring->mask];
	u64 latch;
	u32 cur = atomic_read(&ctrs->inflight);

	for (;;) {
		if (cur == ANE_STATS_INFLIGHT_TRANS) {
			cur = atomic_read(&ctrs->inflight);
		} else if (!cur) {
			if (atomic_cmpxchg(&ctrs->inflight, 0u,
					   ANE_STATS_INFLIGHT_TRANS) != 0u) {
				cur = atomic_read(&ctrs->inflight);
				continue;
			}
			/*
			 * Read max_end only now. The drainer of the previous
			 * period fed its end sample into max_end before its
			 * release store of inflight = 0, and every other
			 * member fed before its decrement. The fully ordered
			 * cmpxchg above read that 0, so this read sees every
			 * feed and the latch is never below the previous
			 * fold end: periods cannot overlap.
			 */
			latch = atomic64_read(&ctrs->max_end);
			if (submit_ns > latch)
				latch = submit_ns;
			atomic64_set_release(&ctrs->last_busy_end, latch);
			atomic_set_release(&ctrs->inflight, 1u);
			break;
		} else if (atomic_try_cmpxchg(&ctrs->inflight, &cur,
					      cur + 1u)) {
			break;
		}
	}

	/*
	 * Order the counter stores above (last_busy_end, inflight) before
	 * the slot stores below.
	 */
	smp_wmb();
	(void)atomic64_read_acquire(&e->seq); /* pair with reader */
	/*
	 * In flight: seq stays odd (2*ticket - 1) until complete()
	 * commits the even final value 2*ticket. The reader only prints
	 * even seqs it can match, so an unfinished submission never
	 * prints and a torn write is never visible.
	 */
	atomic64_set_release(&e->seq, 2ull * ticket - 1ull);
	atomic64_set_release(&e->submit_ns, submit_ns);
	/*
	 * start_ns is the busy-period start this submission is counted
	 * from (the latch), not the caller's sample: a sample taken
	 * before the transition can predate period entry, and the
	 * union reference must use consumed values.
	 */
	atomic64_set_release(&e->start_ns,
			     atomic64_read(&ctrs->last_busy_end));
	atomic64_set_release(&e->end_ns, submit_ns);
	atomic_set(&e->tasks, tasks);
	atomic_set(&e->rc, (u32)0xFFFFFFFFu); /* sentinel: not done */
	atomic64_set_release(&e->tmst, 0ull);
	return ticket;
}

/*
 * Hot-path submission completion. The last submission out of a busy
 * period closes it: under the transition sentinel it folds the whole
 * period span [last_busy_end, end_ns] into busy_ns before reopening
 * the counter, so no reader can see a period both live and folded.
 * Overlapping submissions share the period, so busy_ns is the union
 * of the submit-to-completion intervals, not their sum. Updates the
 * slot's end_ns/rc/tmst and increments jobs.
 */
static inline void ane_stats_complete(struct ane_stats_counters *ctrs,
				      struct ane_stats_ring *ring,
				      u64 ticket, u64 end_ns,
				      u32 rc, u64 tmst)
{
	struct ane_stats_ring_entry *e =
		&ring->slots[(size_t)(ticket - 1ull) & ring->mask];
	u32 cur = atomic_read(&ctrs->inflight);
	u64 fold_end = 0ull;

	/*
	 * Feed this completion's end sample before the transition: a
	 * drainer can only observe inflight == 1 after every other
	 * completer has fed, so the fold below sees the whole period.
	 */
	ane_stats_atomic64_max(end_ns, &ctrs->max_end);

	for (;;) {
		if (cur == ANE_STATS_INFLIGHT_TRANS) {
			cur = atomic_read(&ctrs->inflight);
		} else if (cur > 1u) {
			if (atomic_try_cmpxchg(&ctrs->inflight, &cur,
					       cur - 1u))
				break;
		} else if (!cur) {
			/*
			 * Unbalanced complete (cannot happen with the
			 * documented one-begin-per-complete pairing):
			 * count the job, fold nothing, never hang.
			 */
			break;
		} else if (atomic_cmpxchg(&ctrs->inflight, 1u,
					  ANE_STATS_INFLIGHT_TRANS) != 1u) {
			cur = atomic_read(&ctrs->inflight);
		} else {
			u64 s = atomic64_read(&ctrs->last_busy_end);

			/*
			 * The period ends at the latest completion sample
			 * in it, not at this caller's (possibly stale)
			 * sample: every member fed max_end before the
			 * drain could observe inflight == 1.
			 */
			fold_end = atomic64_read(&ctrs->max_end);
			if (fold_end < end_ns)
				fold_end = end_ns;
			if (fold_end > s)
				atomic64_add(fold_end - s, &ctrs->busy_ns);
			atomic_set_release(&ctrs->inflight, 0u);
			break;
		}
	}
	atomic64_add(1ull, &ctrs->jobs);

	/* Order the counter stores above before the slot stores below. */
	smp_wmb();
	/*
	 * The drainer records the consumed fold end, not its own sample,
	 * so the slot union equals busy_ns exactly.
	 */
	if (fold_end)
		atomic64_set_release(&e->end_ns, fold_end);
	else
		atomic64_set_release(&e->end_ns, end_ns);
	atomic_set(&e->rc, rc);
	atomic64_set_release(&e->tmst, tmst);
	/*
	 * Order the slot field stores before the even seq store that
	 * publishes them; pairs with the acquire load of e->seq in
	 * ane_timeline_show().
	 */
	smp_wmb();
	/*
	 * Commit the slot with the even final seq 2*ticket. The value
	 * must not depend on the current ring->head: concurrent
	 * submissions (ane_t6021) advance it, and a head-derived seq
	 * would mislabel the slot.
	 */
	atomic64_set_release(&e->seq, 2ull * ticket);
}

/*
 * The timeline formatter is header-only so each module gets its own
 * copy: kbuild rejects one object linked into two modules, and with
 * both drivers built-in a shared object would duplicate symbols at
 * vmlinux link. Both drivers register it with drm_debugfs_add_file(),
 * so m->private is the struct drm_debugfs_entry and the ring is the
 * data pointer given at registration.
 */
static inline int ane_timeline_show(struct seq_file *m, void *v)
{
	struct drm_debugfs_entry *entry = m->private;
	struct ane_stats_ring *ring = entry->file.data;
	struct ane_stats_ring_entry *ring_slots = ring->slots;
	u32 mask = ring->mask;
	u64 head = atomic64_read(&ring->head);
	u64 live = head < (u64)mask + 1ull ? head : (u64)mask + 1ull;
	u64 i;

	seq_puts(m,
		 "# ane_timeline: seq submit_ns start_ns end_ns tasks rc tmst (tmst raw tick on ane.ko, 0 = unavailable on ane_t6021)\n");
	/*
	 * Newest first. Submission ticket t lives in slot (t - 1) & mask
	 * and prints once complete() commits seq = 2*t; anything else
	 * (odd in-flight seq, stale slot) is skipped.
	 */
	for (i = 0; i < live; i++) {
		u64 ticket = head - i;
		struct ane_stats_ring_entry *e =
			&ring_slots[(size_t)(ticket - 1ull) & mask];
		u64 seq = atomic64_read_acquire(&e->seq);
		u64 submit = atomic64_read(&e->submit_ns);
		u64 st = atomic64_read(&e->start_ns);
		u64 en = atomic64_read(&e->end_ns);
		u32 tasks = atomic_read(&e->tasks);
		u32 rc = atomic_read(&e->rc);
		u64 tmst = atomic64_read(&e->tmst);

		if (seq != 2ull * ticket)
			continue; /* in flight or stale; never printed */
		seq_printf(m, "%llu %llu %llu %llu %u %u %llu\n",
			   2ull * ticket, submit, st, en, tasks, rc, tmst);
	}
	return 0;
}

#define ANE_STATS_RING_ORDER_DEFAULT 8  /* 256 slots */

/*
 * Accounting rule (coreglass producer contract): jobs counts completed
 * submissions, one begin/complete pair per engine submission — one per
 * ANE_SUBMIT on ane.ko (so libane ane_exec is 1 job and ane_exec_loop
 * with N iterations is N jobs) and one per firmware PROCEDURE_CALL
 * (CSNE_CMD_PROCEDURE_CALL) on ane_t6021.ko: that opcode is the only
 * engine work on the shared ane_rtclient_command path. The control-
 * plane exchanges that ride the same function — LOAD_PROGRAM,
 * CREATE_PROCESS, CH_PROPERTY_WRITE, install-time CONFIG_GET — and the
 * boot transport are not engine submissions and are not counted; jobs
 * must match the number of engine calls the workload made (+-1 at the
 * sampler boundary). A submission that completes with an error still
 * completes its begin/complete pair, so the counter stays balanced.
 *
 * Typed sysfs formatter for the ane_stats attribute. The per-driver
 * show callbacks fetch the counters from their real drvdata type
 * (struct ane_device * on ane.ko, struct ane_rtclient * on
 * ane_t6021.ko) and pass &...->stats_ctrs here. The formatter never
 * sees the device pointer, so the drvdata type confusion that shipped
 * in the first round (reading the head of ane_device as counters)
 * cannot compile again: there is no cast to remove.
 */
/*
 * Consistent counter snapshot across the tiny close/open transition:
 * the transition sentinel is never accepted, and (inflight, busy_ns)
 * must read back unchanged, so a value is only reported between
 * transitions. The value is busy_ns plus the live tail of the
 * still-open period, so it advances while engine work is in flight.
 * Over the timeline the reported value is non-decreasing: folds only
 * add, and at a close the live tail equals the fold.
 */
static inline u64 ane_stats_snapshot(const struct ane_stats_counters *ctrs)
{
	u64 raw, busy = 0, now, s = 0;
	u32 inflight;

	for (;;) {
		inflight = atomic_read_acquire(&ctrs->inflight);
		if (inflight == ANE_STATS_INFLIGHT_TRANS)
			continue; /* close/open in progress: spin it out */
		raw = atomic64_read(&ctrs->busy_ns);
		if (inflight)
			s = atomic64_read(&ctrs->last_busy_end);
		now = ktime_get_ns();
		/*
		 * Recheck after the timestamp: a fold that landed before
		 * `now` changes busy_ns and retries; one that lands
		 * after is genuinely later than `now`, so the value is
		 * exact, never an overshoot.
		 */
		if (atomic64_read(&ctrs->busy_ns) != raw ||
		    atomic_read(&ctrs->inflight) != inflight ||
		    (inflight &&
		     atomic64_read(&ctrs->last_busy_end) != s))
			continue;
		busy = raw;
		if (inflight && now > s)
			busy += now - s;
		break;
	}
	return busy;
}

static inline ssize_t ane_stats_emit(char *buf,
				     const struct ane_stats_counters *ctrs)
{
	return sysfs_emit(buf, "busy_ns %llu\njobs %llu\n",
			  (unsigned long long)ane_stats_snapshot(ctrs),
			  (unsigned long long)atomic64_read(&ctrs->jobs));
}

#endif /* __ANE_STATS_H__ */
