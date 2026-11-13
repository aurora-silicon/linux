/* SPDX-License-Identifier: GPL-2.0 */
/* T6050's ANS2 sleep provider: the sleep makes ANS's final stop. Included after the common ANS
 * owner helpers. No raw PMGR, PCIe or physical-address writes.
 */
#ifndef _APPLE_NVME_M5_SLEEP_H_
#define _APPLE_NVME_M5_SLEEP_H_

static void apple_nvme_m5_fail_locked(struct apple_nvme *anv, int error)
{
	lockdep_assert_held(&anv->disable_lock);
	WRITE_ONCE(anv->m5.phase, APPLE_M5_ANS_FAILED);
	WRITE_ONCE(anv->suspended, true);
	WRITE_ONCE(anv->sleep_failed, true);
	apple_nvme_set_reset_ready(anv, false);
	nvme_quiesce_io_queues(&anv->ctrl);
	nvme_quiesce_admin_queue(&anv->ctrl);
	apple_nvme_disable_queue(&anv->ioq);
	apple_nvme_disable_queue(&anv->adminq);
	/* Unknown firmware/DMA ownership survives remove and devres cleanup. */
	if (!READ_ONCE(anv->quarantined))
		apple_nvme_quarantine(anv, error);
	dev_crit(anv->dev, "M5 ANS wake fenced (%d); platform reset required\n", error);
}

static bool apple_nvme_m5_sleep(struct apple_nvme *anv);

/*
 * Another namespace (the M5's ANS has more than one) may stay unflushed if Linux has never
 * written to it since boot. Otherwise any refuses.
 */
static bool apple_nvme_m5_ns_unwritten(struct apple_nvme *anv, struct nvme_ns *ns)
{
	return apple_nvme_m5_sleep(anv) && ns->disk &&
	       !part_stat_read(ns->disk->part0, sectors[STAT_WRITE]);
}

static bool apple_nvme_m5_namespace_valid(struct apple_nvme *anv)
{
	struct nvme_ns *ns;
	unsigned int count = 0;
	bool valid = true;
	int idx;

	idx = srcu_read_lock(&anv->ctrl.srcu);
	list_for_each_entry_srcu(ns, &anv->ctrl.namespaces, list,
				srcu_read_lock_held(&anv->ctrl.srcu)) {
		if (ns->head->ns_id == anv->hw->m5_sleep->flush_nsid &&
		    ns->queue == anv->m5.flushed_queue) {
			count++;
			continue;
		}
		if (!apple_nvme_m5_ns_unwritten(anv, ns))
			valid = false;
	}
	srcu_read_unlock(&anv->ctrl.srcu, idx);
	return valid && count == 1;
}

/* Noirq, after SART supplier suspend_late; never sends a stop request. */
static int apple_nvme_m5_last_check(void *data)
{
	struct apple_nvme *anv = data;
	u32 cc, csts;
	int ret;

	if (READ_ONCE(anv->m5.phase) != APPLE_M5_ANS_PREPARED ||
	    !anv->m5.frozen || !anv->m5.admin_frozen || !anv->m5.held || !anv->owns_rtkit ||
	    work_busy(&anv->ctrl.scan_work) || !apple_nvme_m5_namespace_valid(anv) ||
	    READ_ONCE(anv->quarantined) || READ_ONCE(anv->removing) ||
	    READ_ONCE(anv->abort_reset) ||
	    test_bit(APPLE_NVME_RECOVERY_PENDING, &anv->recovery_flags) ||
	    nvme_ctrl_state(&anv->ctrl) != NVME_CTRL_RESETTING ||
	    apple_nvme_queue_enabled(&anv->ioq) ||
	    apple_nvme_queue_enabled(&anv->adminq))
		return -EBUSY;
	ret = apple_sleep_ans_m5_preflight(anv->dev);
	if (ret) {
		dev_info(anv->dev, "last check: manager preflight %d\n", ret);
		return ret;
	}
	ret = apple_rtkit_ans_check(anv->rtk);
	if (ret) {
		dev_info(anv->dev, "last check: RTKit %d\n", ret);
		return ret;
	}
	ret = apple_sart_sleep_validate(anv->sart);
	if (ret) {
		dev_info(anv->dev, "last check: SART %d\n", ret);
		return ret;
	}
	cc = readl(anv->mmio_nvme + NVME_REG_CC);
	csts = readl(anv->mmio_nvme + NVME_REG_CSTS);
	if (!(cc & NVME_CC_ENABLE) || (cc & NVME_CC_SHN_MASK) ||
	    !(csts & NVME_CSTS_RDY) || (csts & NVME_CSTS_CFS) ||
	    apple_nvme_cqe_pending(&anv->ioq) || apple_nvme_cqe_pending(&anv->adminq)) {
		dev_info(anv->dev, "last check: CC %#x CSTS %#x, CQEs %d/%d\n", cc, csts,
			 apple_nvme_cqe_pending(&anv->ioq), apple_nvme_cqe_pending(&anv->adminq));
		return -EIO;
	}
	return 0;
}

/* Final phase: every SEP/PMP/GFX/ANS cancellation check is already behind us.
 * The return is a diagnostic to the non-returnable manager, never a veto.
 */
static int apple_nvme_m5_final_stop(void *data)
{
	struct apple_nvme *anv = data;
	/* macOS writes CC and polls CSTS at 0x41dcc0014/1c, the NVMMU window's base */
	void __iomem *regs = anv->mmio_nvmmu;
	u32 cc, csts, other;
	unsigned int us = 0;
	int ret, idle_ret;

	WRITE_ONCE(anv->m5.phase, APPLE_M5_ANS_FINAL);
	cc = readl(regs + NVME_REG_CC);
	anv->ctrl.ctrl_config &= ~NVME_CC_SHN_MASK;
	anv->ctrl.ctrl_config |= anv->hw->m5_sleep->shn;
	writel(anv->ctrl.ctrl_config, regs + NVME_REG_CC);
	ret = readl_poll_timeout_atomic(regs + NVME_REG_CSTS, csts,
		(csts & NVME_CSTS_SHST_MASK) == NVME_CSTS_SHST_CMPLT &&
		!(csts & NVME_CSTS_CFS), 100, APPLE_ANS_SLEEP_SHN_TIMEOUT);
	/* Whether the two windows reach one register: the other window's CSTS, read once */
	other = regs != anv->mmio_nvme ? readl(anv->mmio_nvme + NVME_REG_CSTS) : csts;
	idle_ret = apple_rtkit_sleep_idle_polled(anv->rtk,
		anv->mmio_coproc + APPLE_ANS_COPROC_CPU_CONTROL, &us);
	dev_info(anv->dev, "final stop: CC %#x -> %#x, CSTS %#x (other window %#x): %d, idle %d in %u us\n",
		 cc, anv->ctrl.ctrl_config, csts, other, ret, idle_ret, us);
	anv->m5.stop_error = ret ?: idle_ret;
	/* Even an uncertain/late idle is irreversible. Retain every DMA owner. */
	apple_rtkit_mailbox_recv_off(anv->rtk);
	/* RUN clear (twice) and all storage power gates belong to the manager. */
	return anv->m5.stop_error ? 1 : 0;
}

static int apple_nvme_m5_resources(struct apple_nvme *anv)
{
	static const struct {
		const char *name;
		resource_size_t size;
	} required[] = {
		{ "nvme", APPLE_ANS_BOOT_STATUS + sizeof(u32) },
		{ "ans", APPLE_ANS_COPROC_CPU_STATUS + sizeof(u32) },
		{ "nvmmu", APPLE_NVMMU_TCB_STAT + sizeof(u32) },
	};
	struct platform_device *pdev = to_platform_device(anv->dev);
	unsigned int i;

	if (!anv->hw->has_separate_nvmmu || !anv->lsq_in_nvmmu ||
	    !anv->hw->needs_ioq_registers || anv->hw->max_queue_depth != 64 ||
	    anv->hw->cold_reset || !anv->sart ||
	    !(anv->hw->quirks & NVME_QUIRK_BROKEN_FUA))
		return -EINVAL;
	for (i = 0; i < ARRAY_SIZE(required); i++) {
		struct resource *r = platform_get_resource_byname(pdev,
			IORESOURCE_MEM, required[i].name);

		/* Do not borrow another window or silently change mapping attributes. */
		if (!r || resource_size(r) < required[i].size ||
		    !(r->flags & IORESOURCE_MEM_NONPOSTED))
			return -EOPNOTSUPP;
	}
	return 0;
}

static int apple_nvme_m5_attach(struct apple_nvme *anv)
{
	int ret;

	if (!of_machine_is_compatible("apple,j714s"))
		return -EOPNOTSUPP;
	ret = apple_nvme_m5_resources(anv);
	if (ret)
		return ret;
	init_completion(&anv->m5.rebuild_done);
	ret = apple_sleep_ans_m5_register(anv->dev, anv->mmio_coproc,
		apple_nvme_m5_last_check, apple_nvme_m5_final_stop, anv);
	if (!ret)
		anv->m5.registered = true;
	return ret;
}

/* T6050's ANS2, whose final stop the sleep makes in its last steps */
static bool apple_nvme_m5_sleep(struct apple_nvme *anv)
{
	return anv->hw->m5_sleep == &apple_nvme_t6050_sleep_policy && apple_sleep_ans_late() &&
	       of_device_is_compatible(anv->dev->of_node, "apple,t6050-nvme-ans2");
}

struct apple_nvme_m5_drain {
	struct request *flush;
	unsigned int pending;
};

static bool apple_nvme_m5_pending(struct request *rq, void *data)
{
	struct apple_nvme_m5_drain *d = data;

	if (rq != d->flush && blk_mq_request_started(rq) &&
	    !blk_mq_request_completed(rq))
		d->pending++;
	return true;
}

static int apple_nvme_m5_drain(struct apple_nvme *anv, struct request *flush)
{
	unsigned long end = jiffies + msecs_to_jiffies(anv->hw->m5_sleep->drain_ms);

	do {
		struct apple_nvme_m5_drain d = { .flush = flush };

		if (READ_ONCE(anv->quarantined) || READ_ONCE(anv->abort_reset) ||
		    !apple_rtkit_is_running(anv->rtk))
			return -EIO;
		blk_mq_tagset_busy_iter(&anv->tagset, apple_nvme_m5_pending, &d);
		blk_mq_tagset_busy_iter(&anv->admin_tagset, apple_nvme_m5_pending, &d);
		if (!d.pending)
			return 0;
		msleep(1);
	} while (time_before(jiffies, end));
	return -EBUSY;
}

/* Preallocate under SRCU while the queue still admits requests. The request
 * pins its queue through freeze. Any extra namespace refuses, not silently skips.
 */
static struct request *apple_nvme_m5_flush_request(struct apple_nvme *anv)
{
	struct nvme_ctrl *ctrl = &anv->ctrl;
	struct nvme_command cmd = { };
	struct request *rq = ERR_PTR(-ENODEV);
	struct nvme_ns *ns, *selected = NULL;
	int idx;

	idx = srcu_read_lock(&ctrl->srcu);
	list_for_each_entry_srcu(ns, &ctrl->namespaces, list,
				srcu_read_lock_held(&ctrl->srcu)) {
		if (!selected && ns->head->ns_id == anv->hw->m5_sleep->flush_nsid) {
			selected = ns;
			continue;
		}
		if (!apple_nvme_m5_ns_unwritten(anv, ns)) {
			rq = ERR_PTR(-ERANGE);
			goto out;
		}
	}
	if (!selected)
		goto out;
	cmd.common.opcode = nvme_cmd_flush;
	cmd.common.nsid = cpu_to_le32(selected->head->ns_id);
	anv->m5.flushed_queue = selected->queue;
	rq = blk_mq_alloc_request(selected->queue, nvme_req_op(&cmd), BLK_MQ_REQ_NOWAIT);
	if (!IS_ERR(rq))
		nvme_init_request(rq, &cmd); /* copies cmd; no stack pointer retained */
out:
	srcu_read_unlock(&ctrl->srcu, idx);
	return rq;
}

/* These references cover the unresolved worker, commands and supplier
 * ownership before a bounded PM wait starts. Failure retains them until reset.
 * The list also prevents a new platform probe adopting the same controller.
 */
static void apple_nvme_m5_rebuild_pin(struct apple_nvme *anv)
{
	int i;

	__module_get(THIS_MODULE);
	nvme_get_ctrl(&anv->ctrl);
	get_device(anv->dev);
	if (anv->pd_count > 1)
		for (i = 0; i < anv->pd_count; i++)
			pm_runtime_get_noresume(anv->pd_dev[i]);
	mutex_lock(&apple_nvme_quarantine_lock);
	list_add_tail(&anv->quarantine_node, &apple_nvme_quarantines);
	mutex_unlock(&apple_nvme_quarantine_lock);
	anv->m5.rebuild_pinned = true;
}

static void apple_nvme_m5_rebuild_unpin(struct apple_nvme *anv)
{
	int i;

	mutex_lock(&apple_nvme_quarantine_lock);
	list_del_init(&anv->quarantine_node);
	mutex_unlock(&apple_nvme_quarantine_lock);
	if (anv->pd_count > 1)
		for (i = 0; i < anv->pd_count; i++)
			pm_runtime_put_noidle(anv->pd_dev[i]);
	anv->m5.rebuild_pinned = false;
	put_device(anv->dev);
	nvme_put_ctrl(&anv->ctrl);
	module_put(THIS_MODULE);
}

/* No mutex, work flush/cancel, MMIO, or completion/free of unresolved requests.
 * The owner deadline is a terminal software disposition, never a DMA fence.
 */
static int apple_nvme_m5_rebuild_expired(struct apple_nvme *anv, int error)
{
	WRITE_ONCE(anv->m5.rebuild_aborted, true);
	WRITE_ONCE(anv->m5.phase, APPLE_M5_ANS_FAILED);
	WRITE_ONCE(anv->suspended, true);
	WRITE_ONCE(anv->sleep_failed, true);
	apple_nvme_set_reset_ready(anv, false);
	dev_crit(anv->dev, "M5 ANS rebuild unresolved (%d); owner returned fenced, reset required\n", error);
	return error;
}

/* The PM device lock serializes owner calls. No lock is held across the
 * completion wait, and publication's lock acquisition uses the same deadline.
 */
static int apple_nvme_m5_rebuild(struct apple_nvme *anv, bool warm)
{
	unsigned long end = jiffies + msecs_to_jiffies(anv->hw->m5_sleep->rebuild_ms);
	long remaining;
	int ret;

	if (!mutex_trylock(&anv->disable_lock))
		return -EBUSY;
	if (apple_nvme_m5_retained(anv) || anv->m5.rebuild_active ||
	    work_busy(&anv->ctrl.reset_work)) {
		mutex_unlock(&anv->disable_lock);
		return -EBUSY;
	}
	apple_nvme_m5_rebuild_pin(anv);
	reinit_completion(&anv->m5.rebuild_done);
	anv->m5.rebuild_result = -EINPROGRESS;
	anv->m5.rebuild_after_wake = warm;
	WRITE_ONCE(anv->m5.rebuild_active, true);
	WRITE_ONCE(anv->m5.phase, APPLE_M5_ANS_REBUILDING);
	if (anv->m5.admin_frozen) {
		blk_mq_unfreeze_queue_nomemrestore(anv->ctrl.admin_q);
		anv->m5.admin_frozen = false;
	}
	mutex_unlock(&anv->disable_lock);
	ret = nvme_ctrl_state(&anv->ctrl) == NVME_CTRL_RESETTING ?
		nvme_try_sched_reset(&anv->ctrl) : nvme_reset_ctrl(&anv->ctrl);
	if (ret)
		return apple_nvme_m5_rebuild_expired(anv, ret);
	remaining = end - jiffies;
	if (remaining <= 0 ||
	    !wait_for_completion_timeout(&anv->m5.rebuild_done, remaining))
		return apple_nvme_m5_rebuild_expired(anv, -ETIMEDOUT);
	while (!mutex_trylock(&anv->disable_lock)) {
		if (time_after_eq(jiffies, end))
			return apple_nvme_m5_rebuild_expired(anv, -ETIMEDOUT);
		msleep(1);
	}
	ret = anv->m5.rebuild_result;
	if (!ret && (apple_nvme_m5_retained(anv) || READ_ONCE(anv->abort_reset) ||
		    READ_ONCE(anv->removing) || nvme_ctrl_state(&anv->ctrl) != NVME_CTRL_LIVE))
		ret = -EIO;
	if (!ret && warm)
		ret = apple_rtkit_ans_check(anv->rtk);
	if (ret) {
		mutex_unlock(&anv->disable_lock);
		return apple_nvme_m5_rebuild_expired(anv, ret);
	}
	/* The complete worker has returned a valid result. Only this owner
	 * releases the hold and the I/O admission fence, under the PM lock.
	 */
	if (anv->m5.held) {
		apple_rtkit_ans_release(anv->rtk);
		anv->m5.held = false;
	}
	if (test_bit(NVME_CTRL_FROZEN, &anv->ctrl.flags))
		nvme_unfreeze(&anv->ctrl);
	anv->m5.frozen = false;
	WRITE_ONCE(anv->m5.phase, APPLE_M5_ANS_LIVE);
	WRITE_ONCE(anv->m5.rebuild_active, false);
	WRITE_ONCE(anv->suspended, false);
	nvme_unquiesce_io_queues(&anv->ctrl);
	apple_nvme_set_reset_ready(anv, true);
	apple_nvme_m5_rebuild_unpin(anv);
	mutex_unlock(&anv->disable_lock);
	return 0;
}

/* Refusal has not sent SHN/idle. A changed controller still needs a bounded
 * reset result; never optimistically publish LIVE before that result exists.
 */
static int apple_nvme_m5_unprepare(struct apple_nvme *anv)
{
	bool live;

	if (!mutex_trylock(&anv->disable_lock))
		return -EBUSY;
	if (apple_nvme_m5_retained(anv)) {
		mutex_unlock(&anv->disable_lock);
		return -EIO;
	}
	if (anv->m5.held) {
		apple_rtkit_ans_release(anv->rtk);
		anv->m5.held = false;
	}
	live = nvme_ctrl_state(&anv->ctrl) == NVME_CTRL_LIVE &&
	       apple_rtkit_is_running(anv->rtk) && !READ_ONCE(anv->abort_reset);
	if (!live) {
		mutex_unlock(&anv->disable_lock);
		return apple_nvme_m5_rebuild(anv, false);
	}
	if (anv->m5.admin_frozen) {
		blk_mq_unfreeze_queue_nomemrestore(anv->ctrl.admin_q);
		anv->m5.admin_frozen = false;
	}
	apple_nvme_enable_queue(&anv->adminq);
	apple_nvme_enable_queue(&anv->ioq);
	nvme_unquiesce_admin_queue(&anv->ctrl);
	if (anv->m5.frozen)
		nvme_unfreeze(&anv->ctrl);
	anv->m5.frozen = false;
	WRITE_ONCE(anv->m5.phase, APPLE_M5_ANS_LIVE);
	WRITE_ONCE(anv->suspended, false);
	nvme_unquiesce_io_queues(&anv->ctrl);
	apple_nvme_set_reset_ready(anv, true);
	mutex_unlock(&anv->disable_lock);
	return 0;
}

static enum rq_end_io_ret apple_nvme_m5_flush_done(struct request *rq,
		blk_status_t status, const struct io_comp_batch *batch)
{
	struct apple_nvme *anv = rq->end_io_data;

	anv->m5.flush_result = (nvme_req(rq)->flags & NVME_REQ_CANCELLED) ? -EINTR :
		(nvme_req(rq)->status ?: blk_status_to_errno(status));
	complete(&anv->m5.flush_done);
	return RQ_END_IO_FREE;
}

static int apple_nvme_m5_prepare(struct apple_nvme *anv)
{
	const struct apple_nvme_m5_policy *p = anv->hw->m5_sleep;
	struct request *flush;
	s64 generation;
	int ret;

	/* The owner registers here, in the sleep the manager chose, after its checks. */
	if (!anv->m5.registered && apple_nvme_m5_sleep(anv)) {
		ret = apple_nvme_m5_attach(anv);
		if (ret)
			return ret;
	}
	if (!apple_nvme_m5_sleep(anv) || !anv->m5.registered)
		return -EOPNOTSUPP;
	if (READ_ONCE(anv->m5.phase) != APPLE_M5_ANS_LIVE)
		return -EIO;
	/* Refuse active background owners instead of importing their possibly
	 * unresolved command waits into suspend preparation.
	 */
	if (!mutex_trylock(&anv->disable_lock))
		return -EBUSY;
	if (work_busy(&anv->ctrl.reset_work) || work_busy(&anv->ctrl.scan_work) ||
	    work_busy(&anv->recovery_work) || work_busy(&anv->flush_dwork.work) ||
	    delayed_work_pending(&anv->flush_dwork)) {
		mutex_unlock(&anv->disable_lock);
		return -EBUSY;
	}
	if (nvme_ctrl_state(&anv->ctrl) != NVME_CTRL_LIVE || !anv->owns_rtkit ||
	    READ_ONCE(anv->quarantined) || READ_ONCE(anv->removing) ||
	    !apple_rtkit_is_running(anv->rtk) ||
	    test_bit(NVME_CTRL_FROZEN, &anv->ctrl.flags) ||
	    test_bit(APPLE_NVME_RECOVERY_PENDING, &anv->recovery_flags)) {
		mutex_unlock(&anv->disable_lock);
		return -EBUSY;
	}
	WRITE_ONCE(anv->m5.phase, APPLE_M5_ANS_PREPARING);
	WRITE_ONCE(anv->suspended, true); /* timeout stop may run, never schedule reset */
	apple_nvme_set_reset_ready(anv, false);
	anv->m5.stop_error = 0;
	mutex_unlock(&anv->disable_lock);

	anv->m5.held = true; /* prepare may set its hold before failing */
	ret = apple_rtkit_ans_prepare(anv->rtk);
	if (ret)
		goto undo;
	flush = apple_nvme_m5_flush_request(anv);
	if (IS_ERR(flush)) {
		ret = PTR_ERR(flush);
		goto undo;
	}
	nvme_start_freeze(&anv->ctrl);
	anv->m5.frozen = true;
	blk_freeze_queue_start(anv->ctrl.admin_q);
	anv->m5.admin_frozen = true;
	if (blk_mq_freeze_queue_wait_timeout(anv->ctrl.admin_q,
				msecs_to_jiffies(p->drain_ms)) <= 0) {
		blk_mq_free_request(flush);
		ret = -EBUSY;
		goto undo;
	}
	nvme_quiesce_admin_queue(&anv->ctrl);
	ret = apple_nvme_m5_drain(anv, flush);
	if (ret) {
		blk_mq_free_request(flush); /* not submitted; no DMA owner */
		goto undo;
	}
	/* Stabilize dispatch between the busy-tag drain and generation sample. */
	nvme_quiesce_io_queues(&anv->ctrl);
	ret = apple_nvme_m5_drain(anv, flush);
	if (ret) {
		blk_mq_free_request(flush);
		goto undo;
	}
	generation = atomic64_read(&anv->m5.submissions);
	nvme_unquiesce_io_queues(&anv->ctrl);
	/* No disable_lock: timeout's existing worker must be able to stop DMA. */
	init_completion(&anv->m5.flush_done);
	anv->m5.flush_result = -EINPROGRESS;
	flush->end_io = apple_nvme_m5_flush_done;
	flush->end_io_data = anv;
	flush->timeout = msecs_to_jiffies(p->flush_ms);
	blk_execute_rq_nowait(flush, false);
	if (!wait_for_completion_timeout(&anv->m5.flush_done,
					 msecs_to_jiffies(p->flush_ms))) {
		/* May still complete/DMA: callback, request, queue and owner stay
		 * pinned. No cancellation or free based on this timeout alone.
		 */
		guard(mutex)(&anv->disable_lock);
		apple_nvme_m5_fail_locked(anv, -ETIMEDOUT);
		return -ETIMEDOUT;
	}
	ret = anv->m5.flush_result;
	if (ret)
		goto undo;
	if (nvme_wait_freeze_timeout(&anv->ctrl) <= 0 ||
	    atomic64_read(&anv->m5.submissions) != generation + 1) {
		ret = -EBUSY;
		goto undo;
	}
	nvme_quiesce_io_queues(&anv->ctrl);
	mutex_lock(&anv->disable_lock);
	ret = apple_rtkit_ans_check(anv->rtk);
	if (ret || READ_ONCE(anv->abort_reset) || READ_ONCE(anv->quarantined) ||
	    test_bit(APPLE_NVME_RECOVERY_PENDING, &anv->recovery_flags) ||
	    !nvme_change_ctrl_state(&anv->ctrl, NVME_CTRL_RESETTING)) {
		mutex_unlock(&anv->disable_lock);
		ret = ret ?: -EBUSY;
		goto undo;
	}
	apple_nvme_disable_queue(&anv->adminq);
	apple_nvme_disable_queue(&anv->ioq);
	WRITE_ONCE(anv->m5.phase, APPLE_M5_ANS_PREPARED);
	mutex_unlock(&anv->disable_lock);
	apple_nvme_keep_link_powered(anv);
	/* SART snapshot comes in supplier suspend_late; last-check validates it. */
	apple_sleep_coproc_set_ready(anv->dev, true);
	return 0;
undo:
	{
		int recovery = apple_nvme_m5_unprepare(anv);

		return recovery ?: (ret < 0 ? ret : -EIO);
	}
}

static int apple_nvme_m5_resume_early(struct apple_nvme *anv)
{
	int ret;

	if (READ_ONCE(anv->m5.phase) == APPLE_M5_ANS_PREPARED)
		return 0; /* A reversible refusal; normal resume releases preparation. */
	if (READ_ONCE(anv->m5.phase) != APPLE_M5_ANS_FINAL)
		return READ_ONCE(anv->m5.phase) == APPLE_M5_ANS_FAILED ? -EIO : 0;
	mutex_lock(&anv->disable_lock);
	ret = anv->m5.stop_error;
	if (!ret && !pm_resume_via_firmware())
		ret = -EIO;
	if (!ret)
		ret = apple_sleep_ans_m5_domains_ready(anv->dev);
	/* Receipt is read before clearing ready (which clears manager stopped). */
	apple_sleep_coproc_set_ready(anv->dev, false);
	if (ret)
		goto failed;
	WRITE_ONCE(anv->m5.phase, APPLE_M5_ANS_STARTING);
	/* Manager owns pins and power. Its receipt includes pin restoration. */
	msleep(anv->hw->m5_sleep->phy_to_run_ms);
	ret = apple_sart_replay(anv->sart);
	if (ret) {
		ret = ret < 0 ? ret : -EIO;
		goto failed;
	}
	ret = apple_rtkit_ans_start(anv->rtk,
		anv->mmio_coproc + APPLE_ANS_COPROC_CPU_CONTROL);
	if (ret)
		goto failed;
	mutex_unlock(&anv->disable_lock);
	return 0;
failed:
	apple_nvme_m5_fail_locked(anv, ret);
	mutex_unlock(&anv->disable_lock);
	return ret;
}

static int apple_nvme_m5_resume(struct apple_nvme *anv)
{
	int ret;

	if (READ_ONCE(anv->m5.phase) == APPLE_M5_ANS_PREPARED) {
		apple_sleep_coproc_set_ready(anv->dev, false);
		/* No SHN, idle or domain change occurred. Controller reset only. */
		return apple_nvme_m5_unprepare(anv);
	}
	if (READ_ONCE(anv->m5.phase) == APPLE_M5_ANS_FAILED)
		return -EIO;
	if (READ_ONCE(anv->m5.phase) != APPLE_M5_ANS_STARTING)
		return 0;
	mutex_lock(&anv->disable_lock);
	/* One bounded warm handshake; do not send INIT or reallocate buffers. */
	ret = apple_rtkit_ans_finish(anv->rtk);
	if (ret)
		goto failed;
	WRITE_ONCE(anv->sleep_restarted, true);
	mutex_unlock(&anv->disable_lock);
	return apple_nvme_m5_rebuild(anv, true);
failed:
	apple_nvme_m5_fail_locked(anv, ret);
	mutex_unlock(&anv->disable_lock);
	return ret;
}
#endif
