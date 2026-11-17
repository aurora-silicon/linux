// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* Copyright 2022 Eileen Yoon <eyn@gmx.com> */

#include <linux/bitfield.h>
#include <linux/device.h>
#include <linux/kobject.h>
#include <linux/sysfs.h>
#include <linux/iopoll.h>
#include <linux/pm_runtime.h>
#include <linux/module.h>
#include <linux/of.h>
#include "ane_tm.h"
#include "ane_pm.h"

#define ANE_TQ_COUNT 8
static const int TQ_PRTY_TABLE[ANE_TQ_COUNT] = { 0x1, 0x2, 0x3,	 0x4,
						 0x5, 0x6, 0x1e, 0x1f };

#define ANE_TM_BASE		  0x20000
#define ANE_TQ_BASE		  0x21000

#define TM_ADDR			  0x0
#define TM_INFO			  0x4
#define TM_PUSH			  0x8
#define TM_TQ_EN		  0xc

#define TM_IRQ_EVTC(line)	  (0x14 + ((line) * 0x14))
#define TM_IRQ_INFO(line)	  (0x18 + ((line) * 0x14))
#define TM_IRQ_UNK1(line)	  (0x1c + ((line) * 0x14))
#define TM_IRQ_TMST(line)	  (0x20 + ((line) * 0x14))
#define TM_IRQ_UNK2(line)	  (0x24 + ((line) * 0x14))

#define TM_COMMITTED		  0x44
#define TM_STATUS		  0x54
#define TM_ERROR1		  0x58
#define TM_ERROR2		  0x5c
#define TM_ERROR3		  0x60
#define TM_IRQ_EN1		  0x68
#define TM_IRQ_ACK		  0x6c
#define TM_IRQ_EN2		  0x70

#define TQ_STATUS(qid)		  (0x000 + ((qid) * 0x148))
#define TQ_HW_STATUS(qid)	  (0x004 + ((qid) * 0x148))
#define TQ_PENDING(qid)		  (0x00c + ((qid) * 0x148))
#define TQ_PRTY(qid)		  (0x010 + ((qid) * 0x148))
#define TQ_VACANT(qid)		  (0x014 + ((qid) * 0x148))
#define TQ_INFO(qid)		  (0x01c + ((qid) * 0x148))

#define TQ_BAR1(qid, bdx)	  (0x020 + ((qid) * 0x148) + ((bdx) * 0x4))
#define TQ_NID1(qid)		  (0x0a0 + ((qid) * 0x148))
#define TQ_SIZE2(qid)		  (0x0a4 + ((qid) * 0x148))
#define TQ_ADDR2(qid)		  (0x0a8 + ((qid) * 0x148))

#define TQ_BAR2(qid, bdx)	  (0x0ac + ((qid) * 0x148) + ((bdx) * 0x4))
#define TQ_NID2(qid)		  (0x12c + ((qid) * 0x148))
#define TQ_SIZE1(qid)		  (0x130 + ((qid) * 0x148))
#define TQ_ADDR1(qid)		  (0x134 + ((qid) * 0x148))

#define TM_IS_IDLE		  0x1
#define TM_IS_FINE		  0x22222222

/*
 * Per-queue abort. Writing a queue number with START set asks the task
 * manager to stop that queue's in-flight descriptors; START reads back
 * set until the command has been taken. The queue then reports hardware
 * idle in its own status word, and its pending-descriptor count drains
 * to zero. TQ_STATUS (+0x00) is only the software ownership word the
 * submit path sets; it says nothing about the hardware.
 */
#define TM_ABORT		  0xf4
#define TM_ABORT_QUEUE		  GENMASK(2, 0)
#define TM_ABORT_START		  BIT(4)
#define TQ_HW_STATUS_IDLE	  BIT(0)
#define TQ_PENDING_TDS		  GENMASK(15, 0)
/* Valid bit of both descriptor slots of a queue (TQ_NID1, TQ_NID2). */
#define TQ_NID_VALID		  BIT(0)
/* Bounds of each abort wait, polled once per millisecond. */
#define ANE_TM_ABORT_POLL_US	  1000
#define ANE_TM_ABORT_TIMEOUT_US	  2000000

static bool tm_abort = true;
module_param(tm_abort, bool, 0644);
MODULE_PARM_DESC(tm_abort,
		 "Abort a hung queue in the task manager before the recovery power cycle on T600x (default on).");

#define tm_read32(ane, off)	  (readl((ane)->engine + ANE_TM_BASE + (off)))
#define tq_read32(ane, off)	  (readl((ane)->engine + ANE_TQ_BASE + (off)))
#define tm_write32(ane, off, val) \
	(writel(val, (ane)->engine + ANE_TM_BASE + (off)))
#define tq_write32(ane, off, val) \
	(writel(val, (ane)->engine + ANE_TQ_BASE + (off)))

/* The ane SET block (m1n1 ANE.ps_map) maps the pmgr power-state words
 * for this engine (set0, base, set1..4). pmgr reads are always safe and
 * give recovery its ACTUAL power state; the SET words themselves are
 * firmware-locked: a direct write external-aborts the SoC (T6001 at
 * 0x28e08c000 with PS_SET0 down and all sets gated; T8103 hard-resets
 * the same way at 0x23b70c000). No code here ever writes the SET block.
 */
#define ANE_PS_ACTUAL_MASK	  0xf0
#define ANE_PS_WORDS		  6 /* set0, base, set1..4 */

/* ACTUAL nibble of each SET word, word 0 in the low nibble; 0 when the
 * SET block is unmapped. pmgr registers only: engine MMIO is never
 * read for power state (a readl through a warm gate external-aborts
 * and hard-resets the machine).
 */
u32 ane_ps_act(struct ane_device *ane)
{
	u32 v = 0;
	int i;

	if (!ane->ps)
		return 0;
	for (i = 0; i < ANE_PS_WORDS; i++)
		v |= ((readl(ane->ps + i * 8) & ANE_PS_ACTUAL_MASK) >> 4)
		     << (i * 4);
	return v;
}

/* Bisect probe: per-word named reads of the SET window. Each word logs
 * before its readl, so a window whose address does not decode on this
 * SoC is named by the last line logged (word index + byte offset)
 * instead of a silent hard reset between two other prints. The lines
 * are debug messages: dynamic debug turns them on.
 */
u32 ane_ps_act_probe(struct ane_device *ane)
{
	u32 v = 0;
	int i;

	if (!ane->ps) {
		dev_dbg(ane->dev, "ps probe: SET window unmapped\n");
		return 0;
	}
	dev_dbg(ane->dev, "ps probe: SET window at %pap, %d words\n",
		&ane->ps_base, ANE_PS_WORDS);
	for (i = 0; i < ANE_PS_WORDS; i++) {
		u32 w;

		dev_dbg(ane->dev, "ps probe: word %d @ ps+0x%02x reading\n",
			i, i * 8);
		w = readl(ane->ps + i * 8);
		v |= ((w & ANE_PS_ACTUAL_MASK) >> 4) << (i * 4);
		dev_dbg(ane->dev, "ps probe: word %d -> %#x\n", i, w);
	}
	return v;
}

/* Recovery-path MMIO logging. Every write prints before and after, and
 * every engine read prints its value, each line carrying the pmgr
 * ACTUAL of the owning partitions. A write that external-aborts the
 * SoC is then named by the last line that reached a remote console,
 * and the bisect starts from that register instead of a guess. The
 * lines are debug messages: dynamic debug turns them on.
 */
static void ane_rec_writel(struct ane_device *ane, const char *reg,
			   void __iomem *addr, u32 val)
{
	dev_dbg(ane->dev, "ANEWR %s <- %#x (ps act %#x)\n", reg, val,
		ane_ps_act(ane));
	writel(val, addr);
	dev_dbg(ane->dev, "ANEWR %s wrote (ps act %#x)\n", reg,
		ane_ps_act(ane));
}

static u32 ane_rec_read32(struct ane_device *ane, const char *reg,
			  void __iomem *addr)
{
	u32 val = readl(addr);

	dev_dbg(ane->dev, "ANERD %s -> %#x (ps act %#x)\n", reg, val,
		ane_ps_act(ane));
	return val;
}

u32 ane_tm_ps_act(struct ane_device *ane)
{
	return ane_ps_act(ane);
}

void ane_tm_enable(struct ane_device *ane, bool rec)
{
	void __iomem *tq_en = ane->engine + ANE_TM_BASE + TM_TQ_EN;
	u32 val = rec ? ane_rec_read32(ane, "TM_TQ_EN tm+0x0c", tq_en)
		      : readl(tq_en);
	char reg[24];

	val |= 0x1000;
	if (rec)
		ane_rec_writel(ane, "TM_TQ_EN tm+0x0c", tq_en, val);
	else
		writel(val, tq_en);

	for (int qid = 0; qid < ANE_TQ_COUNT; qid++) {
		void __iomem *prty =
			ane->engine + ANE_TQ_BASE + TQ_PRTY(qid);

		if (rec) {
			snprintf(reg, sizeof(reg), "TQ_PRTY[%d] tq+%#x", qid,
				 TQ_PRTY(qid));
			ane_rec_writel(ane, reg, prty, TQ_PRTY_TABLE[qid]);
		} else {
			writel(TQ_PRTY_TABLE[qid], prty);
		}
	}

	if (rec) {
		ane_rec_writel(ane, "TM_IRQ_EN1 tm+0x68",
			       ane->engine + ANE_TM_BASE + TM_IRQ_EN1,
			       0x4000000);
		ane_rec_writel(ane, "TM_IRQ_EN2 tm+0x70",
			       ane->engine + ANE_TM_BASE + TM_IRQ_EN2, 0x6);
	} else {
		tm_write32(ane, TM_IRQ_EN1, 0x4000000);
		tm_write32(ane, TM_IRQ_EN2, 0x6);
	}
}

u32 ane_tm_status(struct ane_device *ane)
{
	return tm_read32(ane, TM_STATUS);
}

int ane_tm_enqueue(struct ane_device *ane, struct ane_request *req)
{
	int qid = req->qid;

	tq_write32(ane, TQ_STATUS(qid), 0x1);

	for (int bdx = 0; bdx < DRM_ANE_TILE_COUNT; bdx++)
		tq_write32(ane, TQ_BAR1(qid, bdx), req->bar[bdx]);

	tq_write32(ane, TQ_SIZE1(qid), ((req->td_size >> 2) - 1) << 0x10);
	tq_write32(ane, TQ_ADDR1(qid), req->btsp_iova);
	tq_write32(ane, TQ_NID1(qid), (req->nid & 0xff) << 8 | 1);

	return 0;
}

static void ane_tm_push_tq(struct ane_device *ane, struct ane_request *req)
{
	int qid = req->qid;

	tm_write32(ane, TM_ADDR, tq_read32(ane, TQ_ADDR1(qid)));
	tm_write32(ane, TM_INFO, tq_read32(ane, TQ_SIZE1(qid)) | req->td_count);
	tm_write32(ane, TM_PUSH, TQ_PRTY_TABLE[qid] | (qid & 7) << 8); // magic
}

/*
 * Last raw TM tick read from TM_IRQ_TMST in the latest event drain.
 * Diagnostic only (unit unknown; the timeline file labels it as a
 * raw tick); unsynchronized by design.
 */
u32 ane_last_tmst;

/*
 * Every task of a request can post an event, so a program with more tasks
 * than one poll drains leaves a backlog: a delayed poller (all CPUs busy)
 * saw 84 events on line 1 for a 99-task MobileNet. Only a count no queue
 * can produce means the read itself is bad.
 */
#define ANE_TM_EVENTS_MAX 0x1000

static int ane_tm_collect_events(struct ane_device *ane,
				 struct ane_request *req, u32 *finished)
{
	for (unsigned int line = 0; line < 2; line++) {
		u32 count = tm_read32(ane, TM_IRQ_EVTC(line));

		if (count > ANE_TM_EVENTS_MAX)
			return -EIO;
		for (u32 n = 0; n < count; n++) {
			u32 info = tm_read32(ane, TM_IRQ_INFO(line));

			tm_read32(ane, TM_IRQ_UNK1(line));
			ane_last_tmst = tm_read32(ane, TM_IRQ_TMST(line));
			tm_read32(ane, TM_IRQ_UNK2(line));
			if (req && info == (0x05000000 | (req->nid << 16) |
					    (req->td_count - 1)))
				*finished |= BIT(line);
		}
	}
	tm_write32(ane, TM_IRQ_ACK, tm_read32(ane, TM_IRQ_ACK) | 2);
	return *finished == 3 && (tm_read32(ane, TM_STATUS) & TM_IS_IDLE);
}

/* read_poll_timeout op: consult the DART latch before any engine access.
 * Returns -EIO when a fault is latched, so a fault that clears before the
 * caller reads the latch again is still a failure. The caller handles a
 * latched fault without ever reading the wedged engine.
 */
static int ane_tm_poll_step(struct ane_device *ane, struct ane_request *req,
			    u32 *finished)
{
	if (ane_dart_faulted(ane, NULL, NULL))
		return -EIO;
	return ane_tm_collect_events(ane, req, finished);
}

static void ane_tm_log_queue(struct ane_device *ane, int qid, const char *when)
{
	dev_dbg(ane->dev,
		"abort %s: q%d tq_en %#x abort %#x tm_status %#x tq %#x hw %#x pending %#x prty %#x nid1 %#x nid2 %#x (ps act %#x)\n",
		when, qid, tm_read32(ane, TM_TQ_EN), tm_read32(ane, TM_ABORT),
		tm_read32(ane, TM_STATUS), tq_read32(ane, TQ_STATUS(qid)),
		tq_read32(ane, TQ_HW_STATUS(qid)),
		tq_read32(ane, TQ_PENDING(qid)), tq_read32(ane, TQ_PRTY(qid)),
		tq_read32(ane, TQ_NID1(qid)), tq_read32(ane, TQ_NID2(qid)),
		ane_ps_act(ane));
}

/* Abort-wait conditions. Each pass reads the DART latch before the engine. */
static int ane_tm_abort_taken(struct ane_device *ane)
{
	if (ane_dart_faulted(ane, NULL, NULL))
		return -EIO;
	return !(tm_read32(ane, TM_ABORT) & TM_ABORT_START);
}

static int ane_tm_queue_quiet(struct ane_device *ane, int qid)
{
	if (ane_dart_faulted(ane, NULL, NULL))
		return -EIO;
	return (tq_read32(ane, TQ_HW_STATUS(qid)) & TQ_HW_STATUS_IDLE) &&
	       !FIELD_GET(TQ_PENDING_TDS, tq_read32(ane, TQ_PENDING(qid)));
}

/*
 * Stop a hung queue in the task manager while the engine is still powered,
 * so the power cycle that follows does not cut an in-flight descriptor.
 * Abort the queue, wait for the command to be taken, then for the queue to
 * report hardware idle with no descriptor pending. Only then give the
 * queue back the way a completed job does: invalidate both descriptor
 * slots, then clear the ownership word, and restore the queue priority.
 *
 * Returns 0 only when the task manager acknowledged every step; on any
 * failure nothing is cleared. A task stalled below the task manager never
 * lets the abort be taken; the power cycle is then what stops it.
 */
static int ane_tm_abort_queue(struct ane_device *ane, int qid)
{
	int ret, done;
	u32 nid;

	lockdep_assert_held(&ane->engine_lock);
	if (qid < 0 || qid >= ANE_TQ_COUNT)
		return -EINVAL;
	/* pmgr reads only: no engine MMIO unless every island reads on. */
	if (ane->ps && ane_ps_act(ane) != ANE_PS_ALL_ON)
		return -ENODEV;
	if (ane_dart_faulted(ane, NULL, NULL))
		return -EIO;

	ane_tm_log_queue(ane, qid, "before");
	tm_write32(ane, TM_ABORT,
		   TM_ABORT_START | FIELD_PREP(TM_ABORT_QUEUE, qid));

	ret = read_poll_timeout(ane_tm_abort_taken, done, done,
				ANE_TM_ABORT_POLL_US, ANE_TM_ABORT_TIMEOUT_US,
				false, ane);
	if (!ret && done < 0)
		ret = done;
	if (ret) {
		dev_err(ane->dev, "q%d abort not taken: %d (abort %#x)\n", qid,
			ret, ret == -EIO ? 0 : tm_read32(ane, TM_ABORT));
		return ret;
	}

	ret = read_poll_timeout(ane_tm_queue_quiet, done, done,
				ANE_TM_ABORT_POLL_US, ANE_TM_ABORT_TIMEOUT_US,
				false, ane, qid);
	if (!ret && done < 0)
		ret = done;
	if (ret) {
		if (ret == -EIO)
			dev_err(ane->dev, "q%d abort: DART fault latched\n", qid);
		else
			dev_err(ane->dev,
				"q%d abort taken but queue not idle: hw %#x pending %#x\n",
				qid, tq_read32(ane, TQ_HW_STATUS(qid)),
				tq_read32(ane, TQ_PENDING(qid)));
		return ret;
	}

	nid = tq_read32(ane, TQ_NID1(qid));
	tq_write32(ane, TQ_NID1(qid), nid & ~TQ_NID_VALID);
	nid = tq_read32(ane, TQ_NID2(qid));
	tq_write32(ane, TQ_NID2(qid), nid & ~TQ_NID_VALID);
	/* Both slots are invalid before the queue is handed back. */
	wmb();
	tq_write32(ane, TQ_STATUS(qid), 0x0);
	tq_write32(ane, TQ_PRTY(qid), TQ_PRTY_TABLE[qid]);
	ane_tm_log_queue(ane, qid, "after");
	return 0;
}

/*
 * Keep the engine wedged until reboot and tell userspace: the wedged
 * attribute stays 1 (pollable), wedge_reason says why, and a change uevent
 * carries ANE_WEDGED=1. Resources stay preserved, as for a failed reset.
 */
static void ane_tm_wedge_final(struct ane_device *ane, const char *reason)
{
	static char *env[] = { "ANE_WEDGED=1", NULL };

	lockdep_assert_held(&ane->engine_lock);
	if (ane->wedge_final)
		return;
	ane->wedge_final = true;
	ane->wedge_reason = reason;
	dev_err(ane->dev,
		"engine wedged until reboot: %s; submissions refused, resources retained\n",
		reason);
	sysfs_notify(&ane->dev->kobj, NULL, "wedged");
	kobject_uevent_env(&ane->dev->kobj, KOBJ_CHANGE, env);
}

int ane_tm_execute(struct ane_device *ane, struct ane_request *req)
{
	u32 finished = 0;
	u32 fault_status = 0;
	u64 fault_iova = 0;
	struct ane_dart_scratch *scratch = ane->scratch;
	bool faulted = false;
	int err, status;
	ktime_t deadline = ktime_add_ms(ktime_get(), 1000);

	lockdep_assert_held(&ane->engine_lock);
	ane_dart_mask(ane);

	err = ane_tm_collect_events(ane, NULL, &finished);
	if (err < 0)
		goto wedge;
	/* Order the CPU writes to the request buffers and the event
	 * acknowledgments before the queue writes that start the engine.
	 */
	wmb();
	ane_tm_push_tq(ane, req);

	for (;;) {
		if (ktime_compare(ktime_get(), deadline) >= 0) {
			err = -ETIMEDOUT;
			break;
		}
		/* Latch-first completion poll. Engine register reads hang
		 * the SoC (T8103) once a DART fault has landed, inside the
		 * 1-microsecond completion poll and before any fault
		 * handling can run, so the poll op consults the DART error
		 * latch first and stops the poll before the engine is
		 * touched. Residual hazard: a fault landing between one
		 * iteration's latch read and engine read.
		 */
		err = read_poll_timeout(ane_tm_poll_step, status,
					status != 0 || ktime_compare(ktime_get(), deadline) >= 0,
					1, 1000000, false,
					ane, req, &finished);
		if (!err && status < 0)
			err = status;
		if (!err && !status)
			err = -ETIMEDOUT;
		if (ane_dart_faulted(ane, &fault_iova, &fault_status)) {
			faulted = true;
			if (ane->scratch_pages >= ANE_DART_SCRATCH_MAX) {
				err = -EIO;
				break;
			}
			err = ane_dart_drain_fault(ane, fault_iova, scratch,
						   &ane->scratch_pages);
			if (err)
				break;
			/* The retried transaction now completes; poll on. */
			continue;
		}
		break;
	}

	if (faulted) {
		/* The faulting program drained through the scratch pages
		 * and the engine is idle again (the latch is clear or the
		 * poll would still be running). Gate the queue with the
		 * normal completion writes, hand the scratch pages back,
		 * and fail this request. No partition power cycle, no
		 * stream disable: the page tables were never changed by
		 * the fault.
		 */
		if (err || ane_dart_faulted(ane, NULL, NULL)) {
			/* Drain failed or the drained program never reached
			 * completion: the engine is not idle and access is
			 * off the table. Retain scratch backing until reset.
			 */
			err = err ?: -EIO;
			goto wedge;
		}
		status = tq_read32(ane, TQ_NID1(req->qid));
		tq_write32(ane, TQ_NID1(req->qid), status & ~1U);
		/* Kept from the established completion sequence (clear the
		 * NID valid bit, then the queue status). writel() already
		 * orders the two writes to the engine.
		 */
		rmb();
		tq_write32(ane, TQ_STATUS(req->qid), 0x0);
		ane_dart_release_scratch(ane, scratch, ane->scratch_pages);
		ane_dart_unmask(ane);
		dev_err(ane->dev,
			"DART fault contained: status=%#x iova=%#llx scratch=%d\n",
			fault_status, fault_iova, ane->scratch_pages);
		ane->scratch_pages = 0;
		return -EIO;
	}
	if (err)
		goto wedge;
	status = tq_read32(ane, TQ_NID1(req->qid));
	if (((status >> 8) & 0xff) != req->nid) {
		err = -EIO;
		goto wedge;
	}
	tq_write32(ane, TQ_NID1(req->qid), status & ~1U);
	/* Same completion sequence as the contained-fault path above. */
	rmb();
	tq_write32(ane, TQ_STATUS(req->qid), 0x0);
	ane_dart_unmask(ane);
	ane->recovery_unproven = 0;
	return 0;

wedge:
	ane_dart_unmask(ane);
	if (atomic_xchg(&ane->wedged, 1) == 0) {
		__module_get(THIS_MODULE);
		dev_err(ane->dev,
			"tm completion failed: %d, finish lines=%x (q%d nid=%#x)\n",
			err, finished, req->qid, req->nid);
	}

	if (!ane_tm_failure_recoverable(ane->tm_retention,
					ane->recovery_unproven, err,
					finished)) {
		ane_tm_wedge_final(ane, ane->tm_retention ?
				   "jobs keep hanging without an event after every recovery" :
				   "the first job after a recovery hung without an event");
		return err;
	}

	/* Stop the hung queue cleanly first where the task manager still
	 * takes commands; the power cycle below covers the case where it
	 * does not.
	 */
	if (err == -ETIMEDOUT && ane->tm_retention && tm_abort) {
		if (!ane_tm_abort_queue(ane, req->qid))
			dev_info(ane->dev, "q%d hung job aborted: queue idle, nothing pending\n",
				 req->qid);
		else
			dev_info(ane->dev, "q%d abort not taken; the power cycle stops the task\n",
				 req->qid);
	}

	/* One bounded recovery attempt: stop the tm, power-cycle the engine
	 * and return to accepting work. Only a failed reset preserves
	 * resources until reboot.
	 */
	if (ane_tm_recover(ane) < 0)
		dev_err(ane->dev,
			"recovery failed; submissions disabled, resources retained while DMA may be active; retry reset or reboot\n");
	return err;
}

/*
 * Recovery runs with engine_lock and pm_lock held and one consumer PM
 * reference. Suspend through runtime PM, just as autosuspend does, so the
 * supplier references and the domain hierarchy are released together.
 * Never use the system-sleep force helpers here: an in-use forced supplier
 * retains RPM_ACTIVE, which prevents its parent domain from powering off.
 */

/* No engine MMIO unless the owning partitions read powered on (ACTUAL
 * nibble per word): a readl through a warm gate external-aborts and
 * hard resets the machine. Covers set0, base and set1..4 from the SET
 * block map; sys_cpu rides its own genpd resume and has no cell in
 * this block on T6001.
 */
static int ane_ps_verify_on(struct ane_device *ane)
{
	u32 act = 0;
	int err = -ETIMEDOUT;
	int i;

	if (!ane->ps)
		return 0;
	for (i = 0; i < 100; i++) {
		act = ane_ps_act(ane);
		if (act == ANE_PS_ALL_ON) {
			err = 0;
			break;
		}
		usleep_range(1000, 2000);
	}
	dev_dbg(ane->dev, "ANERD ps verify act=%#x err=%d\n", act, err);
	return err;
}

/*
 * A power domain the ANE shares with its performance controller (the T600x
 * PMP report SRAM, ps_pms_sram) stays on for as long as the controller is
 * up, whatever the ANE does, so it is no part of the engine's power state.
 */
static bool ane_pd_is_perf_supply(struct ane_device *ane, int index)
{
	struct device_node *report = ane->perf.report;
	struct of_phandle_args pd, rp;
	bool shared = false;

	if (!report || of_parse_phandle_with_args(ane->dev->of_node, "power-domains",
						  "#power-domain-cells", index, &pd))
		return false;
	for (int i = 0; !shared; i++) {
		if (of_parse_phandle_with_args(report, "power-domains",
					       "#power-domain-cells", i, &rp))
			break;
		shared = rp.np == pd.np && rp.args_count == pd.args_count &&
			 !memcmp(rp.args, pd.args, pd.args_count * sizeof(pd.args[0]));
		of_node_put(rp.np);
	}
	of_node_put(pd.np);
	return shared;
}

static bool ane_partitions_off(struct ane_device *ane)
{
	if (ane_ps_act(ane))
		return false;

	/* The SET window does not include the shared SYS_CPU parent. Its
	 * DART consumers suspend asynchronously after our references drop.
	 * Wait for every attached domain as well, so resuming the consumer
	 * cannot race that work and leave the TM register file powered.
	 */
	if (ane->pd_count == 1)
		return !dev_pm_genpd_is_on(ane->dev);
	for (int i = 0; i < ane->pd_count; i++)
		if (dev_pm_genpd_is_on(ane->pd_dev[i]) &&
		    !ane_pd_is_perf_supply(ane, i))
			return false;
	return true;
}

static int ane_verify_off(void *ctx)
{
	struct ane_device *ane = ctx;
	bool off;
	int err;

	/* genpd may retain a shared or busy parent even after suspend succeeds.
	 * Only PMGR and provider state are safe to read while the engine is off.
	 */
	if (!ane->ps)
		return -EOPNOTSUPP;
	err = read_poll_timeout(ane_partitions_off, off, off,
				100, 100000, false, ane);
	dev_info(ane->dev, "recovery: engine power-off actual=%#x rc=%d\n",
		 ane_ps_act(ane), err);
	return err;
}

static int ane_pd_cycle(struct ane_device *ane)
{
	int err;

	lockdep_assert_held(&ane->pm_lock);
	ane->recovering = true;
	/* Release the performance request before the domains go down. */
	err = ane_perf_suspend(ane);
	if (!err)
		err = ane_pm_cycle(ane->dev, ane->pd_dev, ane->pd_count,
				   ane_verify_off, ane);
	ane->recovering = false;
	return err;
}

/*
 * Drain the task-manager state a timed-out task leaves behind (from
 * omarchy-ane ane_tm_drain_retained by Joshua Warren, MIT) when the
 * tm/tq file survives the power cycle in retention (T6001; descriptor
 * tm_retention). T8103 never runs this: its cycle is a full POR of the
 * file, and the re-arm below starts it clean.
 *
 * Two things must go, or the retained TM never reads idle again:
 *   - pending events on both IRQ lines. Each event pops by reading
 *     INFO/UNK1/TMST/UNK2, exactly as ane_tm_collect_events pops them;
 *     a stale backlog would make the next submit's completion poll
 *     miss its events.
 *   - the latched queue entry of the dead task. Both descriptor slots
 *     (NID1 and NID2) lose their valid bit before TQ_STATUS is cleared,
 *     the order the normal completion path gives a finished queue.
 */
static void ane_tm_drain_retained(struct ane_device *ane)
{
	int line, qid;

	/* Stop the TM first by clearing its enable (bit 12); the re-arm
	 * below sets it again. Keep bit 13: it reads set after every power-on
	 * and the TM does not dispatch without it. With it cleared a queued
	 * descriptor stays pending while TM_STATUS reads idle, so every job
	 * after the recovery timed out without an event.
	 */
	ane_rec_writel(ane, "TM_TQ_EN stop tm+0x0c",
		       ane->engine + ANE_TM_BASE + TM_TQ_EN,
		       tm_read32(ane, TM_TQ_EN) & ~0x1000U);

	for (line = 0; line < 2; line++) {
		u32 count = tm_read32(ane, TM_IRQ_EVTC(line));
		u32 n;

		if (count > ANE_TM_EVENTS_MAX)
			count = ANE_TM_EVENTS_MAX;
		for (n = 0; n < count; n++) {
			tm_read32(ane, TM_IRQ_INFO(line));
			tm_read32(ane, TM_IRQ_UNK1(line));
			tm_read32(ane, TM_IRQ_TMST(line));
			tm_read32(ane, TM_IRQ_UNK2(line));
		}
	}
	tm_write32(ane, TM_IRQ_ACK, tm_read32(ane, TM_IRQ_ACK) | 3);

	/* The dead task leaves per-queue error codes latched here (seen
	 * 0x22222222 / 0x2222: four-bit code 2 per TQ slot). While any
	 * latch is set the TM stays halted and TM_STATUS reads 0. Try the
	 * W1C convention first (write the read value back), fall back to
	 * plain zero-clear, and log what this silicon answered to.
	 */
	{
		static const u16 err_reg[3] = { TM_ERROR1, TM_ERROR2,
						TM_ERROR3 };
		char reg[24];
		int ei;

		for (ei = 0; ei < 3; ei++) {
			void __iomem *addr =
				ane->engine + ANE_TM_BASE + err_reg[ei];
			u32 val = readl(addr);

			if (!val)
				continue;
			writel(val, addr);
			if (readl(addr))
				writel(0x0, addr);
			snprintf(reg, sizeof(reg), "TM_ERROR%d clear tm+%#x",
				 ei + 1, err_reg[ei]);
			ane_rec_read32(ane, reg, addr);
		}
	}

	for (qid = 0; qid < ANE_TQ_COUNT; qid++) {
		u32 nid1 = tq_read32(ane, TQ_NID1(qid));
		u32 nid2 = tq_read32(ane, TQ_NID2(qid));

		if (nid1 & TQ_NID_VALID)
			tq_write32(ane, TQ_NID1(qid), nid1 & ~TQ_NID_VALID);
		if (nid2 & TQ_NID_VALID)
			tq_write32(ane, TQ_NID2(qid), nid2 & ~TQ_NID_VALID);
		/* Both slots are invalid before the queue is handed back. */
		wmb();
		tq_write32(ane, TQ_STATUS(qid), 0x0);
	}

	/* Log the retained state: what the dead task left in the
	 * committed counter and the error latches.
	 */
	ane_rec_read32(ane, "TM_COMMITTED tm+0x44 (drain)",
		       ane->engine + ANE_TM_BASE + TM_COMMITTED);
	ane_rec_read32(ane, "TM_ERROR1 tm+0x58 (drain)",
		       ane->engine + ANE_TM_BASE + TM_ERROR1);
	ane_rec_read32(ane, "TM_ERROR2 tm+0x5c (drain)",
		       ane->engine + ANE_TM_BASE + TM_ERROR2);
	ane_rec_read32(ane, "TM_ERROR3 tm+0x60 (drain)",
		       ane->engine + ANE_TM_BASE + TM_ERROR3);
}

int ane_tm_recover(struct ane_device *ane)
{
	u32 status;
	int err;

	lockdep_assert_held(&ane->engine_lock);
	if (!atomic_read(&ane->wedged))
		return 0;
	if (ane->wedge_final) {
		dev_err(ane->dev, "no recovery: %s; reboot required\n",
			ane->wedge_reason);
		return -EIO;
	}

	dev_err(ane->dev, "recovering: power-cycling engine partitions\n");

	err = ane_pd_cycle(ane);
	if (err) {
		dev_err(ane->dev, "recovery: engine power cycle failed: %d\n",
			err);
		return err;
	}

	/* No engine MMIO before the islands read powered on. */
	err = ane_ps_verify_on(ane);
	if (err) {
		dev_err(ane->dev,
			"recovery: ane set islands not powered on: %d\n", err);
		return err;
	}

	/* On T600x drain what the tm/tq file may still hold after the
	 * cycle (events, error latches, queue slots); this is what recovers
	 * a failed task ("-5, finish lines=1"). After a hang without an
	 * event the cycle has been seen to return the file at its reset
	 * values (no committed work, no valid slot); the drain then only
	 * re-arms it. Anything left fails the idle check below and stays
	 * wedged.
	 */
	if (ane->tm_retention)
		ane_tm_drain_retained(ane);

	ane_tm_enable(ane, true);

	err = readl_poll_timeout(ane->engine + ANE_TM_BASE + TM_STATUS,
				 status,
				 (status & TM_IS_IDLE) ||
				 (ane->tm_status_known &&
				  status == ane->tm_status_fresh),
				 100, 1000000);
	status = ane_rec_read32(ane, "TM_STATUS tm+0x54",
				ane->engine + ANE_TM_BASE + TM_STATUS);
	if (err) {
		dev_err(ane->dev, "recovery: tm not idle after reset: %#x\n",
			status);
		return err;
	}

	if (atomic_xchg(&ane->wedged, 0)) {
		module_put(THIS_MODULE); /* verified reset releases the wedge pin */
		/* The power cycle above quiesced every transaction, so the
		 * wedge-preserved mappings (dead BOs kept alive only to
		 * stop IOVA teardown under active DMA) can now be unmapped
		 * and their ranges handed back to the allocator. Without
		 * this reclaim the ranges would stay pinned for the whole
		 * module lifetime.
		 */
		ane_reclaim_preserved(ane);
		/* Idle is all a reset can show; the next job proves it. */
		ane->recovery_unproven++;
		dev_info(ane->dev, "tm recovered: idle, accepting work again\n");
	}
	return 0;
}
