// SPDX-License-Identifier: GPL-2.0-only OR MIT
// Included in g17_live_boot so the runtime remains the sole firmware owner.

struct G17PAsyncComputeActive {
    packet: Arc<crate::g17_job::G17PComputeJobPacket>,
    _lease: crate::g17_drm::G17PComputeLeaseGuard,
}

/// Installed queue graphs survive logical queue destruction. Firmware retains
/// their addresses; exact command retirement permits rebinding, not freeing.
struct G17PAsyncComputeQueue {
    owner: Option<u64>,
    queue: g17_manager::G17PSksmQueue,
    binding: Option<g17_manager::G17PComputeUserBinding>,
    context: Option<Arc<mmu::T8140ComputeExecutionContext>>,
    active: Option<G17PAsyncComputeActive>,
    released: bool,
    quarantined: bool,
}

impl G17PLiveRuntime {
    pub(crate) fn prepare_async_compute_queue(
        &mut self, owner: u64, vm: &mmu::Vm,
    ) -> Result<(u8, Arc<mmu::T8140ComputeExecutionContext>)> {
        if !self.accepting_submissions || self.is_crashed() { return Err(EIO); }
        if let Some(q) = self.async_compute_queues.iter().find(|q| q.owner == Some(owner)) {
            return Ok((q.queue.queue_id(), q.context.as_ref().ok_or(EIO)?.clone()));
        }
        let manager = self.manager.as_mut().ok_or(ENODEV)?;
        manager.stage_compute_flist_state()?;
        let free = self.async_compute_queues.iter().position(|q|
            q.owner.is_none() && q.active.is_none() && !q.quarantined);
        let index = if let Some(index) = free { index } else {
            self.async_compute_queues.reserve(1, GFP_KERNEL)?;
            // Reuse the unsubmitted boot graph, preserving QID 4 as first queue.
            let queue = if self.compute_binding.is_none() && self.pending_compute.is_none() {
                self.queue.take()
            } else { None };
            let queue = match queue {
                Some(queue) => queue,
                None => manager.prepare_g17p_compute_queue(self.drm.as_ref().ok_or(ENODEV)?)?,
            };
            self.queue_setup_pending = false;
            let index = self.async_compute_queues.len();
            self.async_compute_queues.push(G17PAsyncComputeQueue {
                owner: None, queue, binding: None, context: None, active: None,
                released: false, quarantined: false,
            }, GFP_KERNEL)?;
            index
        };
        let q = &mut self.async_compute_queues[index];
        let (binding, context) = manager.bind_independent_compute_user_vm(&mut q.queue, vm)?;
        q.binding = Some(binding);
        q.context = Some(context.clone());
        q.owner = Some(owner);
        q.released = false;
        Ok((q.queue.queue_id(), context))
    }

    /// A signaled error fence is still signaled to DRM syncobj waiters. Surface
    /// device failure at the next submit instead of accepting work whose
    /// hardware owner is quarantined and can never make queue progress.
    pub(crate) fn check_async_compute_submission(&self) -> Result {
        if !self.accepting_submissions || self.is_crashed()
            || self.async_compute_queues.iter().any(|q| q.quarantined)
        { return Err(EIO); }
        Ok(())
    }

    pub(crate) fn async_compute_needs_install(&self, owner: u64) -> Result<bool> {
        let q = self.async_compute_queues.iter().find(|q| q.owner == Some(owner)).ok_or(EINVAL)?;
        Ok(q.queue.requires_first_install())
    }

    pub(crate) fn async_compute_dependency(&self, owner: u64) -> Option<kernel::dma_fence::Fence> {
        self.async_compute_queues.iter().find(|q| q.owner == Some(owner) && !q.quarantined)?
            .active.as_ref()?.packet.completion().ok().map(|c| c.fence())
    }

    pub(crate) fn publish_async_compute(
        &mut self, owner: u64, packet: Arc<crate::g17_job::G17PComputeJobPacket>,
        lease: crate::g17_drm::G17PComputeLeaseGuard,
    ) -> Result {
        if !self.accepting_submissions || self.is_crashed() { return Err(EIO); }
        self.settle_open_recovery_before_publication()?;
        // Render jobs now retain independent application-GART roots and
        // job-private low views.  Compute QID4+ has no render context lease to
        // reclaim, so publication may proceed while either render slot runs.
        if *module_parameters::g17p_power_wire.value() == 1 { self.wire_g17p_submit_gpu_power()?; }
        let index = self.async_compute_queues.iter().position(|q| q.owner == Some(owner)).ok_or(EINVAL)?;
        let q = &mut self.async_compute_queues[index];
        if q.active.is_some() || q.quarantined || q.released { return Err(EBUSY); }
        if q.queue.queue_id() != packet.hardware_queue_id()
            || q.context.as_ref().ok_or(EIO)?.context_id() != packet.context().context_id()
        { return Err(EINVAL); }
        let timestamp_start = packet.completion()?.gpu_address();
        let timestamp_end = timestamp_start.checked_add(8).ok_or(EOVERFLOW)?;
        let state = self.gfx_state.as_ref().cloned().ok_or(ENODEV)?;
        state.submission_phase.store(g17_manager::G17P_SUBMIT_PHASE_IDLE, Ordering::Release);
        // From this point, every error retains the packet, mappings and root.
        // A failed notification does not prove the firmware missed the kick.
        q.active = Some(G17PAsyncComputeActive { packet: packet.clone(), _lease: lease });
        let result = (|| {
            let notifications = self.manager.as_mut().ok_or(ENODEV)?.submit_translated_compute(
                self.registers.as_ref().ok_or(ENODEV)?, &mut q.queue,
                &state.submission_phase, q.binding.as_ref().ok_or(ENODEV)?, packet.command(),
                g17_manager::G17PComputeSubmissionContext {
                    context_id: packet.context().context_id(),
                    dispatch_identity: 0x0100_01d7_0200_01dc, execution_gate: 1,
                    timestamps: g17_uapi::ComputeTimestampAddresses { start: timestamp_start, end: timestamp_end },
                    descriptor_flag_4c: false, descriptor_flag_5c8: false,
                    converted_command_timestamp: 1, barriers: &[], mcache: None,
                    rce_kind: 0, auxiliary: 0x003f_ffff_ffff_ffff,
                },
            )?;
            if let Some(notifications) = notifications {
                for message in [notifications.activation, notifications.direct_kick].into_iter().flatten() {
                    fence(Ordering::SeqCst);
                    Pin::new(self.gfx_rtkit.as_mut().ok_or(ENODEV)?)
                        .send_message(g17_rtkit::ENDPOINT_GFX_INTERRUPTS, message)?;
                }
            }
            Ok(())
        })();
        if let Err(error) = result {
            if state.submission_phase.load(Ordering::Acquire) == g17_manager::G17P_SUBMIT_PHASE_IDLE {
                // No firmware-facing publication occurred: release this packet
                // and permit a later valid job to retry the prepared queue.
                self.async_compute_queues[index].active = None;
            } else {
                self.quarantine_async_compute(owner, packet.submission_id(), error);
            }
            return Err(error);
        }
        if *module_parameters::g17p_power_wire.value() >= 2 {
            if let Err(error) = self.wire_g17p_submit_gpu_power() {
                self.quarantine_async_compute(owner, packet.submission_id(), error);
                return Err(error);
            }
        }
        let outstanding = self.async_compute_queues.iter().filter(|q| q.active.is_some()).count();
        dev_info!(self.pdev.as_ref().ok_or(ENODEV)?.as_ref(),
            "G17P async compute: PUBLISHED submission={} owner={} qid={} context={} root={:#x} outstanding={}\n",
            packet.submission_id(), owner, packet.hardware_queue_id(),
            packet.context().context_id(), packet.context().root(), outstanding);
        // Firmware may notify before publishing the final stamp, and an
        // interrupt coalesced with a running worker cannot enqueue it twice.
        // Keep completion service armed while any hardware owner is active.
        self.async_visibility_pending = true;
        crate::driver::queue_g17p_firmware_event_worker(state.dev.clone());
        Ok(())
    }

    fn service_async_compute_completions(&mut self) -> Result {
        self.async_visibility_pending = false;
        let manager = self.manager.as_mut().ok_or(ENODEV)?;
        for q in &mut self.async_compute_queues {
            if q.active.is_none() || q.quarantined { continue; }
            match manager.poll_independent_compute_completion(&mut q.queue) {
                Ok(Some(timestamps)) => {
                    let active = q.active.take().ok_or(EIO)?;
                    dev_info!(self.pdev.as_ref().ok_or(ENODEV)?.as_ref(),
                        "G17P async compute: COMPLETED submission={} qid={} context={} start={} end={}\n",
                        active.packet.submission_id(), q.queue.queue_id(),
                        active.packet.context().context_id(), timestamps[0], timestamps[1]);
                    active.packet.completion()?.complete(Ok(timestamps));
                    drop(active);
                    if q.released {
                        q.binding = None; q.context = None; q.owner = None;
                    }
                }
                Ok(None) => { self.async_visibility_pending = true; },
                Err(error) if error == EAGAIN => { self.async_visibility_pending = true; },
                Err(error) => {
                    q.quarantined = true;
                    q.active.as_ref().ok_or(EIO)?._lease.quarantine();
                    q.active.as_ref().ok_or(EIO)?.packet.completion()?.complete(Err(error));
                }
            }
        }
        // Poll only while there is accepted hardware work, with a sleep rather
        // than a busy loop. This closes early/coalesced interrupt races without
        // requiring another notification after the completion record or stamp.
        // The scheduler's finite timeout ends service by quarantining the job.
        if self.async_visibility_pending { fsleep(Delta::from_micros(250)); }
        Ok(())
    }

    pub(crate) fn release_async_compute_queue(&mut self, owner: u64) {
        if let Some(q) = self.async_compute_queues.iter_mut().find(|q| q.owner == Some(owner)) {
            q.released = true;
            if q.active.is_none() && !q.quarantined {
                q.binding = None; q.context = None; q.owner = None;
            }
        }
    }

    pub(crate) fn quarantine_async_compute(&mut self, owner: u64, submission: u64, error: Error) -> bool {
        let _ = self.service_async_compute_completions();
        if let Some(q) = self.async_compute_queues.iter_mut().find(|q| q.owner == Some(owner)) {
            if let Some(active) = &q.active {
                if active.packet.submission_id() == submission {
                    if !q.quarantined {
                        q.quarantined = true;
                        active._lease.quarantine();
                        if let Some(pdev) = &self.pdev {
                            dev_err!(pdev.as_ref(),
                                "G17P async compute: QUARANTINE submission={} qid={} context={} error={:?}\n",
                                submission, q.queue.queue_id(), active.packet.context().context_id(), error);
                        }
                        if let Some(manager) = self.manager.as_mut() {
                            let _ = manager.dump_compute_completion_records("async-timeout");
                        }
                        if let Ok(completion) = active.packet.completion() { completion.complete(Err(error)); }
                    }
                    return true;
                }
            }
        }
        false
    }
}
