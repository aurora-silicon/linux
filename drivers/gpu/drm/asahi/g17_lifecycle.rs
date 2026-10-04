// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Reversible T8140 dual-ASC CPU execution.

#![cfg_attr(not(test), allow(dead_code))]

/// Both provider roles now have matching start and stop operations.
pub(crate) const TRANSACTIONAL_CPU_EXECUTOR_AVAILABLE: bool = true;

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) enum LifecycleError {
    ProviderLifecycleIncomplete,
}

/// Admit CPU start only when both providers report a complete lifecycle.
pub(crate) const fn admit_cpu_start(
    gfx_missing: u32,
    gfx1_missing: u32,
) -> Result<(), LifecycleError> {
    if gfx_missing != 0 || gfx1_missing != 0 {
        return Err(LifecycleError::ProviderLifecycleIncomplete);
    }

    Ok(())
}

/// One provider-owned ASC CPU lifecycle.
pub(crate) trait AscCpuLifecycle {
    type Error;

    fn start_cpu(&self) -> Result<(), Self::Error>;
    fn stop_cpu(&self) -> Result<(), Self::Error>;
}

/// Owned pair of provider handles whose CPUs are running.
///
/// Construction starts GFX before GFX1. Dropping the owner always attempts to
/// stop GFX1 before GFX, so a platform driver can retain the pair for its
/// entire bound lifetime without duplicating partial-start rollback logic.
pub(crate) struct StartedCpuPair<P: AscCpuLifecycle> {
    gfx: P,
    gfx1: P,
    gfx_running: bool,
    gfx1_running: bool,
}

impl<P: AscCpuLifecycle> StartedCpuPair<P> {
    /// Start both provider-owned CPUs and return their retained owner.
    pub(crate) fn start(gfx: P, gfx1: P) -> Result<Self, P::Error> {
        Self::start_staged(gfx, gfx1, || Ok(()))
    }

    pub(crate) fn start_staged<F>(gfx: P, gfx1: P, gfx_ready: F) -> Result<Self, P::Error>
    where
        F: FnOnce() -> Result<(), P::Error>,
    {
        gfx.start_cpu()?;
        if let Err(err) = gfx_ready() {
            let _ = gfx.stop_cpu();
            return Err(err);
        }
        if let Err(err) = gfx1.start_cpu() {
            let _ = gfx.stop_cpu();
            return Err(err);
        }

        Ok(Self {
            gfx,
            gfx1,
            gfx_running: true,
            gfx1_running: true,
        })
    }

    /// Stop both processors and retain their provider handles.
    ///
    /// A successful return is the boundary after which firmware can no longer
    /// access queue, descriptor, timestamp, or translation objects. Both stop
    /// calls run even if the first reports an error.
    pub(crate) fn stop(&mut self) -> Result<(), P::Error> {
        if !self.gfx_running && !self.gfx1_running {
            return Ok(());
        }

        let mut first_error = None;
        if self.gfx1_running {
            match self.gfx1.stop_cpu() {
                Ok(()) => self.gfx1_running = false,
                Err(err) => first_error = Some(err),
            }
        }
        if self.gfx_running {
            match self.gfx.stop_cpu() {
                Ok(()) => self.gfx_running = false,
                Err(err) if first_error.is_none() => first_error = Some(err),
                Err(_) => {}
            }
        }
        first_error.map_or(Ok(()), Err)
    }

    /// Borrow the retained primary provider while both lifecycle owners stay
    /// inside this pair.
    pub(crate) fn gfx(&self) -> &P {
        &self.gfx
    }
}

impl<P: AscCpuLifecycle> Drop for StartedCpuPair<P> {
    fn drop(&mut self) {
        let _ = self.stop();
    }
}

/// Operations for replacing a timed-out persistent firmware session.
///
/// The old graph stays alive until both processors stop. A failed rebuild
/// leaves submissions paused so the caller can retry the complete rebuild.
pub(crate) trait PersistentRuntimeRecovery {
    type Error;

    fn pause_submissions(&mut self);
    fn stop_cpu_pair(&mut self) -> Result<(), Self::Error>;
    fn drop_gfx1_rtkit(&mut self);
    fn drop_gfx_rtkit(&mut self);
    /// Disable the hardware-visible SKSM queue while retaining its storage.
    fn disable_sksm_queue(&mut self) -> Result<(), Self::Error>;
    /// Drop per-session queue and render state while retaining manager/UAT.
    fn release_runtime_objects(&mut self);
    /// Restore initdata and transports on the retained manager/UAT.
    fn rebuild_session_in_place(&mut self) -> Result<(), Self::Error>;
    fn resume_submissions(&mut self);
}

/// Stop both processors, release the old graph, and build fresh initdata.
pub(crate) fn recover_persistent_runtime<T: PersistentRuntimeRecovery>(
    owner: &mut T,
) -> Result<(), T::Error> {
    owner.pause_submissions();
    owner.stop_cpu_pair()?;
    owner.drop_gfx1_rtkit();
    owner.drop_gfx_rtkit();
    owner.disable_sksm_queue()?;
    owner.release_runtime_objects();
    owner.rebuild_session_in_place()?;
    owner.resume_submissions();
    Ok(())
}

/// Decide whether the next render must start a fresh firmware session.
/// Physical 256-slot SKSM/outer rings wrap without a session restart. The
/// current event writer admits 24-bit stamps; retain that real width bound.
/// A VM change still requires a cold session because the graph has an owner.
pub(crate) const fn retained_render_requires_recycle(
    retained_graph: bool,
    same_vm: bool,
    next_ordinal: u32,
) -> bool {
    retained_graph && (!same_vm || next_ordinal >= 0x00ff_ffff)
}

/// Operations needed to dismantle a retained G17P runtime.
///
/// Keeping the ordering in this testable helper makes the platform owner's
/// `Drop` path explicit: both CPUs stop before any transport, queue, binding,
/// or mapping can disappear. Manager/UAT resources are released last.
pub(crate) trait PersistentRuntimeTeardown {
    /// Stop both firmware processors. No firmware-visible owner may drop
    /// unless this succeeds.
    fn stop_cpu_pair(&mut self) -> bool;
    fn retain_after_stop_failure(&mut self);
    fn unregister_queue(&mut self);
    fn drop_queue(&mut self);
    fn drop_gfx1_rtkit(&mut self);
    fn drop_gfx_rtkit(&mut self);
    fn drop_cpu_pair(&mut self);
    fn drop_manager(&mut self);
}

/// Tear down retained runtime state after proving firmware cannot dereference it.
pub(crate) fn teardown_persistent_runtime<T: PersistentRuntimeTeardown>(owner: &mut T) {
    if !owner.stop_cpu_pair() {
        owner.retain_after_stop_failure();
        return;
    }
    owner.unregister_queue();
    owner.drop_gfx1_rtkit();
    owner.drop_gfx_rtkit();
    owner.drop_queue();
    owner.drop_cpu_pair();
    owner.drop_manager();
}

/// Start GFX then GFX1, run `body`, and stop GFX1 then GFX.
///
/// Every path after GFX starts attempts its matching stop. A GFX1 start error
/// stops GFX before returning. Once both starts succeed, both stops run even
/// if the body or the first stop reports an error.
pub(crate) fn execute_cpu_pair<P, F>(gfx: &P, gfx1: &P, body: F) -> Result<(), P::Error>
where
    P: AscCpuLifecycle,
    F: FnOnce() -> Result<(), P::Error>,
{
    gfx.start_cpu()?;
    if let Err(err) = gfx1.start_cpu() {
        let _ = gfx.stop_cpu();
        return Err(err);
    }

    let body_result = body();
    let gfx1_stop = gfx1.stop_cpu();
    let gfx_stop = gfx.stop_cpu();

    match (body_result, gfx1_stop, gfx_stop) {
        (Err(err), _, _) | (Ok(()), Err(err), _) | (Ok(()), Ok(()), Err(err)) => Err(err),
        (Ok(()), Ok(()), Ok(())) => Ok(()),
    }
}

