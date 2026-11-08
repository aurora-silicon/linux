// SPDX-License-Identifier: GPL-2.0-only OR MIT
//! Application compute submission on the retained firmware session.
use super::*;

impl G18Session {
    #[inline(never)]
    pub(super) fn submit_application_compute(&mut self, vm: &mmu::Vm,
        translated: &crate::g18::uapi::command::TranslatedComputeCommand)
        -> core::result::Result<(), Failure> {
        self.poll_idle_faults("application compute prepare")?;
        let (command, pool, generation, knobs) = self.memory.as_mut().ok_or(EIO)?
            .prepare_application_compute(&self.dev, vm, translated)?;
        if generation == 1 {
            self.queue_probe.as_mut().ok_or(EIO)?.prepare_initial_context(command.context_id)?;
            for phase in 1..=3 {
                self.check_faults("application compute binding")?;
                let probe = self.queue_probe.as_mut().ok_or(EIO)?;
                match phase {
                    1 => {
                        if probe.counters()? != [0, 0, 1] || probe.slot() != [0; 5] {
                            return Err(EBUSY.into());
                        }
                    },
                    2 => probe.prepare_scheduler_bind()?,
                    _ => probe.prepare_pool_bind(pool)?,
                }
                let after = self.memory.as_mut().ok_or(EIO)?
                    .publish_application_compute_outer(phase)?;
                Pin::new(&mut self.roles[G18Role::Gfx.index()].rtkit)
                    .send_message(EP_GFX_INTERRUPTS, 0x0083_0000_0000_000a)?;
                let mut passed = false;
                for _ in 0..CONTROL_POLLS {
                    self.check_faults("application compute binding")?;
                    let outer = self.memory.as_mut().ok_or(EIO)?.compute_outer_counters()?;
                    let probe = self.queue_probe.as_mut().ok_or(EIO)?;
                    if outer == [after; 3] && probe.counters()? == [phase; 3]
                        && G18QueueProbe::installed(probe.slot())
                        && (phase < 2 || G18QueueProbe::scheduler_bound(probe.scheduler_status()?))
                        && (phase < 3 || G18QueueProbe::pool_bound_for_context(
                            probe.pool_status(), pool, command.context_id)) {
                        passed = true; break;
                    }
                    fsleep(Delta::from_millis(1));
                }
                if !passed { return Err(ETIMEDOUT.into()); }
                dev_info!(self.device(), "G18_APP_COMPUTE_BIND_PASS phase={}\n", phase);
            }
        }
        let (previous_pool, bound_context) = self.memory.as_ref().ok_or(EIO)?
            .application_compute_previous_binding()?;
        let previous_context = if generation == 1 { command.context_id } else { bound_context };
        let probe = self.queue_probe.as_mut().ok_or(EIO)?;
        if let Some(what) = probe.compute_unready(previous_pool, previous_context, generation)? {
            let (counters, slot, scheduler, pool_words) = probe.compute_state()?;
            dev_err!(self.device(),
                "G18_APP_COMPUTE_UNREADY compute={} unready={} counters={:?} slot={:x?} scheduler={:x?} pool={:x?}\n",
                generation, what, counters, slot, scheduler, pool_words);
            return Err(EINVAL.into());
        }
        self.memory.as_mut().ok_or(EIO)?.install_application_compute_pool(&self.dev)?;
        self.queue_probe.as_mut().ok_or(EIO)?
            .prepare_application_compute_rebind(command, pool, previous_pool, previous_context, generation)?;
        self.poll_idle_faults("application compute pre-kick")?;
        let memory = self.memory.as_mut().ok_or(EIO)?;
        let completed = memory.compute().completion_snapshot()?;
        if completed.stamps != [0; 2] || completed.timestamps != [0; 2]
            || memory.completion_masks() != [0; 2] { return Err(EBUSY.into()); }
        // The exact knob snapshot this command was built from (g18_app_*).
        crate::g18_kick_info!(self.device(), "G18_APP_COMPUTE_KNOBS compute={} {}\n", generation, knobs);
        let memory = self.memory.as_mut().ok_or(EIO)?;
        let roots = memory.compute().publish_context()?;
        if memory.scheduler().publish_kick(0)? != generation { return Err(EIO.into()); }
        self.queue_probe.as_mut().ok_or(EIO)?.publish_compute_job()?;
        let inner_after = (3 + generation * 3) % 256;
        let outer_after = self.memory.as_mut().ok_or(EIO)?
            .publish_application_compute_outer(inner_after)?;
        Pin::new(&mut self.roles[G18Role::Gfx.index()].rtkit)
            .send_message(EP_GFX_INTERRUPTS, 0x0083_0000_0000_000a)?;
        crate::g18_kick_info!(self.device(),
            "G18_APP_KICK compute={} context={} roots={:x?} stamp={:#x}\n",
            generation, command.context_id, roots, command.expected_stamp);
        let expected_stamp = command.expected_stamp;
        // Phases: outer, inner, scheduler submitted, completed, linked, stamp,
        // completion mask, pool binding visibility.
        let kicked = Instant::<Monotonic>::now();
        let mut first = [0i64; 8];
        let mut gpu_ticks = 0u64;
        let mut pool_pending_reported = false;
        let end = self.wait_job("application compute completion", |session, ms, recovered, report| {
            let inner = session.queue_probe.as_mut().ok_or(EIO)?.counters()?;
            let pool_words = session.queue_probe.as_ref().ok_or(EIO)?.pool_status();
            let pool_bound = G18QueueProbe::pool_bound_for_context(
                pool_words, pool, command.context_id);
            let memory = session.memory.as_mut().ok_or(EIO)?;
            let outer = memory.compute_outer_counters()?;
            let completed = memory.compute().completion_snapshot()?;
            let scheduler = memory.scheduler().snapshot(0)?;
            let masks = memory.completion_masks();
            // What the firmware owns: the queue consumed and the scheduler
            // retired. After a recovery the dispatch never completes, so its
            // stamp, timestamps and completion event are not required.
            let consumed = outer == [outer_after; 3] && inner == [inner_after; 3]
                && scheduler.submitted == generation && scheduler.completed == generation
                && scheduler.linked == 0;
            let finished = completed.stamps[0] == expected_stamp
                && completed.timestamps[0] != 0
                && completed.timestamps[1] >= completed.timestamps[0]
                && masks[0] & (1 << QUEUE_ID) != 0;
            recovery::note_phases(&mut first, &kicked, &[outer == [outer_after; 3], inner == [inner_after; 3],
                scheduler.submitted == generation, scheduler.completed == generation, scheduler.linked == 0,
                completed.stamps[0] == expected_stamp, masks[0] & (1 << QUEUE_ID) != 0, pool_bound]);
            if finished {
                gpu_ticks = completed.timestamps[1] - completed.timestamps[0];
            }
            // Queue/stamp completion can precede an observable pool-table
            // update. Keep both owners until the full binding is visible,
            // within the same bounded wait as command retirement.
            if consumed && finished && !pool_bound && !pool_pending_reported {
                dev_info!(session.device(),
                    "G18_APP_COMPUTE_POOL_PENDING generation={} binding={:?} observed={:x?}; owners retained\n",
                    generation, pool, pool_words);
                pool_pending_reported = true;
            }
            if report {
                dev_info!(session.device(),
                    "G18_APP_COMPUTE_WAIT generation={} ms={} recovered={} outer={:?} inner={:?} stamps={:x?} timestamps={:x?} masks={:x?} scheduler={:?} expected_outer={} expected_inner={} expected_stamp={:#x} consumed={} finished={} pool_bound={} pool={:x?}\n",
                    generation, ms, recovered, outer, inner, completed.stamps, completed.timestamps,
                    masks, scheduler, outer_after, inner_after, expected_stamp, consumed, finished, pool_bound, pool_words);
            }
            Ok(consumed && ((finished && pool_bound) || recovered))
        })?;
        let recovery = match end {
            JobEnd::Retired => match self.poll_faults("application compute retired")? {
                None => {
                    let observed = self.queue_probe.as_ref().ok_or(EIO)?.pool_status();
                    if !G18QueueProbe::pool_bound_for_context(observed, pool, command.context_id) {
                        dev_err!(self.device(),
                            "G18_COMPUTE_POOL_VERIFY_FAILED compute={} binding={:?} observed={:x?}; both owners retained\n",
                            generation, pool, observed);
                        return Err(EIO.into());
                    }
                    if pool.generation != previous_pool.generation {
                        crate::g18_kick_info!(self.device(),
                            "G18_COMPUTE_POOL_VERIFY compute={} generation={} state={:#x} context={} slot={} observed={:x?} verified=1\n",
                            generation, pool.generation, pool.state, command.context_id, pool.slot, observed);
                    }
                    if recovery::phases_due(generation) {
                        dev_info!(self.device(),
                            "G18_JOB_PHASES compute={} us={:?} gpu_ticks={} (outer inner submitted completed linked stamp mask pool)\n",
                            generation, first, gpu_ticks);
                    }
                    self.queue_probe.as_mut().ok_or(EIO)?.retire_application_compute(generation)?;
                    self.memory.as_mut().ok_or(EIO)?
                        .retire_application_compute(generation, command.context_id)?;
                    crate::g18_kick_info!(self.device(),
                        "G18_APP_FRAME_PASS compute={} stamp={:#x} scheduler={} outer={} inner={}\n",
                        generation, expected_stamp, generation, outer_after, inner_after);
                    return Ok(());
                }
                // Retired, but a recovery followed at once: do not vouch for
                // the dispatch. Its queue is retired either way.
                Some(recovery) => recovery,
            },
            JobEnd::Recovered(recovery) => recovery,
            JobEnd::Stranded(recovery) => {
                self.memory.as_mut().ok_or(EIO)?.quarantine_application_compute();
                dev_err!(self.device(),
                    "G18_APP_STRANDED compute={} epoch={} host={}: queue never retired; compute quarantined, owners retained\n",
                    generation, recovery.epoch, recovery.host);
                return Err(Failure::Job(JobFailure::Stranded(recovery, JobPath::Compute)));
            }
        };
        self.queue_probe.as_mut().ok_or(EIO)?.retire_application_compute(generation)?;
        self.memory.as_mut().ok_or(EIO)?.retire_recovered_application_compute(generation)?;
        dev_err!(self.device(), "G18_APP_RECOVERED compute={} epoch={} host={}: queue retired\n",
            generation, recovery.epoch, recovery.host);
        // The next job needs the queue's firmware bindings as they were; a
        // recovery may reset them. Refuse the path rather than kick into it.
        let probe = self.queue_probe.as_mut().ok_or(EIO)?;
        let unready = probe.compute_unready(pool, command.context_id, generation + 1)?;
        let (counters, slot, scheduler, pool_words) = probe.compute_state()?;
        dev_err!(self.device(),
            "G18_APP_COMPUTE_POSTRECOVERY compute={} unready={:?} counters={:?} slot={:x?} scheduler={:x?} pool={:x?}\n",
            generation, unready, counters, slot, scheduler, pool_words);
        match unready {
            None => Err(Failure::Job(JobFailure::Recovered(recovery))),
            Some(what) => Err(Failure::Job(JobFailure::Unbound(recovery, JobPath::Compute, what))),
        }
    }
}
